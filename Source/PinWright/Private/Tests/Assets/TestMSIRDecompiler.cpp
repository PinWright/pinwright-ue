// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"

#if __has_include("MetasoundSource.h")

#include "MSIR/MSIRDecompiler.h"

#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "IrCore/IrTextUtils.h"

#if __has_include("MetasoundAssetKey.h") && __has_include("MetasoundAssetManager.h")
#include "MetasoundAssetKey.h"
#include "MetasoundAssetManager.h"
#define MCP_MSIR_TEST_HAS_ASSET_MANAGER 1
#else
#define MCP_MSIR_TEST_HAS_ASSET_MANAGER 0
#endif
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundSource.h"
#include "Metasound.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "EditorAssetLibrary.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;

    FString MakeUniqueMSIRTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Mirrors the construction pattern from TestMetaSoundIOMutation.cpp — a freshly
    // NewObject'd MetaSoundPatch has zero PagedGraphs; seeding with InitDocument()
    // through a non-priming builder is required before the priming builder runs.
    UMetaSoundPatch* NewTransientMSIRPatch(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueMSIRTestAssetName(TEXT("MS_MSIRPatch"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UMetaSoundPatch* Patch = NewObject<UMetaSoundPatch>(
            Package,
            UMetaSoundPatch::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Patch)
        {
            return nullptr;
        }
        Patch->AddToRoot();

        {
            TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Patch);
            FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
            SeedBuilder.InitDocument();
            PW_METASOUND_FINISH_BUILDING(SeedBuilder);
        }

        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Patch;
    }

    void AddFloatInput(UMetaSoundPatch* Patch, const TCHAR* Name)
    {
#if MCP_HAS_METASOUND_LITERAL_HELPER
        TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(Patch);
        PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

        FMetasoundFrontendLiteral FloatLiteral;
        PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(TEXT("Float"), FloatLiteral);

        FMetasoundFrontendClassInput Input;
        Input.Name = FName(Name);
        Input.TypeName = FName(TEXT("Float"));
        Input.VertexID = FGuid::NewGuid();
        Input.NodeID = FGuid::NewGuid();
        Input.AccessType = EMetasoundFrontendVertexAccessType::Reference;
        // 5.6 paged input defaults: InitDefault() on 5.6+, DefaultLiteral on 5.4/5.5.
        PinWright::MetaSound::SetClassInputDefault(Input, FloatLiteral);

        Builder.AddGraphInput(Input);
        PW_METASOUND_FINISH_BUILDING(Builder);
#endif
    }

    void AddAudioOutput(UMetaSoundPatch* Patch, const TCHAR* Name)
    {
        TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(Patch);
        PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

        FMetasoundFrontendClassOutput Output;
        Output.Name = FName(Name);
        Output.TypeName = FName(TEXT("Audio"));
        Output.VertexID = FGuid::NewGuid();
        Output.NodeID = FGuid::NewGuid();
        Output.AccessType = EMetasoundFrontendVertexAccessType::Reference;

        Builder.AddGraphOutput(Output);
        PW_METASOUND_FINISH_BUILDING(Builder);
    }

#if MCP_MSIR_TEST_HAS_ASSET_MANAGER
    bool RegisterPatchWithAssetManager(
        UMetaSoundPatch* Patch,
        FString& OutAssetPath,
        FString& OutAssetClassGuid,
        bool& bOutRegisteredWithAssetManager)
    {
        bOutRegisteredWithAssetManager = false;
        if (!Patch)
        {
            return false;
        }

        Metasound::Frontend::IMetaSoundAssetManager* AssetManager = Metasound::Frontend::IMetaSoundAssetManager::Get();
        if (!AssetManager)
        {
            return false;
        }

        const FMetaSoundAssetKey AssetKey = AssetManager->AddOrUpdateFromObject(*Patch);
        if (!AssetKey.IsValid())
        {
            return false;
        }
        bOutRegisteredWithAssetManager = true;

        const FTopLevelAssetPath AssetPath = AssetManager->FindAssetPath(AssetKey);
        if (!AssetPath.IsValid())
        {
            return false;
        }

        OutAssetPath = AssetPath.ToString();
        OutAssetClassGuid = AssetKey.ClassName.Name.ToString();
        return true;
    }

    bool AddReferencedPatchNode(UMetaSoundPatch* ParentPatch, UMetaSoundPatch* ReferencedPatch)
    {
        if (!ParentPatch || !ReferencedPatch)
        {
            return false;
        }

        TScriptInterface<IMetaSoundDocumentInterface> ParentInterface(ParentPatch);
        TScriptInterface<IMetaSoundDocumentInterface> ReferencedInterface(ReferencedPatch);
        PW_METASOUND_MAKE_BUILDER(Builder, ParentInterface);

        const FMetasoundFrontendNode* Node = Builder.AddGraphNode(PW_METASOUND_GET_CONST_DOCUMENT(ReferencedInterface).RootGraph);
        PW_METASOUND_FINISH_BUILDING(Builder);
        return Node != nullptr;
    }

    bool ContainsWarningCode(const TArray<FString>& Warnings, const TCHAR* Code)
    {
        return Warnings.ContainsByPredicate([Code](const FString& Warning)
        {
            return Warning.Contains(Code);
        });
    }

    // Simulates version skew between a parent's recorded asset-class dependency and the
    // referenced asset's currently-registered version. The parent document records the
    // referenced asset's version at the moment AddGraphNode runs; once the referenced
    // asset is re-versioned, that recorded version no longer matches. We reproduce this
    // by bumping the major version on every asset-class-GUID dependency (Namespace == None,
    // Name parses as a GUID) in the parent document. Returns the number of dependencies
    // skewed so callers can assert the mutation actually applied.
    int32 SkewAssetClassDependencyVersions(UMetaSoundPatch* ParentPatch)
    {
        if (!ParentPatch)
        {
            return 0;
        }

        TScriptInterface<IMetaSoundDocumentInterface> ParentInterface(ParentPatch);
        // The non-const IMetaSoundDocumentInterface::GetDocument() is private (only the
        // document builder is a friend), and the builder exposes no public mutable-
        // dependency accessor. This skew is deliberately out-of-band — it forges a
        // re-versioned dependency state the normal authoring API would never produce —
        // so we const_cast the sanctioned public const document to edit it directly in
        // this test-only helper.
        FMetasoundFrontendDocument& Document =
            const_cast<FMetasoundFrontendDocument&>(PW_METASOUND_GET_CONST_DOCUMENT(ParentInterface));

        int32 SkewedCount = 0;
        for (FMetasoundFrontendClass& Dependency : Document.Dependencies)
        {
            const FMetasoundFrontendClassName& ClassName = Dependency.Metadata.GetClassName();
            // Use the same predicate the resolver uses to decide what counts as an
            // asset-class-GUID dependency, so this skew filter can't drift from it.
            FString GuidText;
            if (PinWright::MetaSound::IsAssetClassGuidName(ClassName, GuidText))
            {
                FMetasoundFrontendVersionNumber Skewed = Dependency.Metadata.GetVersion();
                Skewed.Major += 1;
                Dependency.Metadata.SetVersion(Skewed);
                ++SkewedCount;
            }
        }
        return SkewedCount;
    }
#endif // MCP_MSIR_TEST_HAS_ASSET_MANAGER

    // DumpSingleAsset's pre-flight DoesPackageExist gate rejects transient packages
    // (returns ASSET_FILE_MISSING) — see AssetDumpHandler.cpp:1331. To exercise the
    // disk-write surface we must clear RF_Transient on both the package and the
    // patch, then SavePackage to the canonical Content path. Returns false on
    // save failure so the caller can fail the assertion cleanly.
    bool PersistMSIRPatchToDisk(UMetaSoundPatch* Patch)
    {
        if (!Patch) return false;
        UPackage* Package = Patch->GetOutermost();
        if (!Package) return false;

        Patch->ClearFlags(RF_Transient);
        Package->ClearFlags(RF_Transient);
        Package->SetFlags(RF_Public | RF_Standalone);
        Package->MarkPackageDirty();

        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        Args.bForceByteSwapping = false;
        Args.bWarnOfLongFilename = false;
        return UPackage::SavePackage(Package, Patch, *PackageFilename, Args);
    }

#if MCP_MSIR_TEST_HAS_ASSET_MANAGER
    // RAII fixture shared by the two asset-registry-fallback MSIR tests
    // (ResolvesAssetClassGuidReferencesFromAssetRegistry and
    // ResolvesAssetClassGuidReferencesAcrossVersionSkew). Both need the identical
    // multi-step arrangement: a referenced patch persisted to disk + registered with
    // the asset registry and the MetaSound asset manager, plus a parent patch that
    // references it via a graph node — and the identical paired teardown (asset-manager
    // RemoveAsset + RemoveFromRoot + on-disk CleanupTestAsset for the referenced patch,
    // RemoveFromRoot for the parent). Keeping it in one place removes the ~80 lines of
    // copy-pasted setup/teardown that previously lived inline in each test and the
    // drift risk that came with it. bSetupOk reports whether every step succeeded so
    // each RunTest can fail cleanly; the destructor cleans up whatever was created.
    struct FOnDiskReferencingPatchFixture
    {
        UMetaSoundPatch* ReferencedPatch = nullptr;
        UMetaSoundPatch* ParentPatch = nullptr;
        FString ReferencedObjectPath;
        FString ReferencedPackagePath;
        FString ReferencedAssetManagerPath;
        FString AssetClassGuid;
        bool bReferencedPatchRegistered = false;
        bool bSetupOk = false;

        explicit FOnDiskReferencingPatchFixture(FAutomationTestBase& Test)
        {
            ReferencedPatch = NewTransientMSIRPatch(ReferencedObjectPath);
            Test.TestNotNull(TEXT("Referenced MetaSoundPatch created"), ReferencedPatch);
            if (!ReferencedPatch)
            {
                return;
            }

            ReferencedPackagePath = ReferencedPatch->GetOutermost()
                ? ReferencedPatch->GetOutermost()->GetName()
                : FString();

            AddFloatInput(ReferencedPatch, TEXT("Gain"));
            AddAudioOutput(ReferencedPatch, TEXT("OutAudio"));

            if (!Test.TestTrue(TEXT("Referenced MetaSoundPatch persists to disk"),
                PersistMSIRPatchToDisk(ReferencedPatch)))
            {
                return;
            }
            FAssetRegistryModule::AssetCreated(ReferencedPatch);

            if (!Test.TestTrue(TEXT("Referenced MetaSoundPatch registers with MetaSound asset manager"),
                RegisterPatchWithAssetManager(
                    ReferencedPatch,
                    ReferencedAssetManagerPath,
                    AssetClassGuid,
                    bReferencedPatchRegistered)))
            {
                return;
            }

            ParentPatch = NewTransientMSIRPatch(ParentObjectPath);
            Test.TestNotNull(TEXT("Parent MetaSoundPatch created"), ParentPatch);
            if (!ParentPatch)
            {
                return;
            }

            if (!Test.TestTrue(TEXT("Parent MetaSoundPatch references the registered patch"),
                AddReferencedPatchNode(ParentPatch, ReferencedPatch)))
            {
                return;
            }

            bSetupOk = true;
        }

        // Drops the referenced patch from the asset manager so the exact-key asset-manager
        // lookup misses; the asset stays on disk / in the asset registry for the fallback.
        void RemoveReferencedFromAssetManager()
        {
            if (Metasound::Frontend::IMetaSoundAssetManager* AssetManager =
                Metasound::Frontend::IMetaSoundAssetManager::Get())
            {
                AssetManager->RemoveAsset(*ReferencedPatch);
                bReferencedPatchRegistered = false;
            }
        }

        ~FOnDiskReferencingPatchFixture()
        {
            if (bReferencedPatchRegistered)
            {
                if (Metasound::Frontend::IMetaSoundAssetManager* AssetManager =
                    Metasound::Frontend::IMetaSoundAssetManager::Get())
                {
                    AssetManager->RemoveAsset(*ReferencedPatch);
                }
            }
            if (ReferencedPatch)
            {
                ReferencedPatch->RemoveFromRoot();
            }
            // Order is load-bearing and unchanged: the asset-manager RemoveAsset above keys off
            // the patch's live object path, so it must run BEFORE the discard renames the patch
            // into /Transient. CleanupTestAsset, not UEditorAssetLibrary::DeleteAsset — force-
            // delete's GatherObjectReferencersForDeletion archive is the documented crash hazard
            // for a never-reloaded MetaSound asset (see Tests/TestAssetTeardown.h). The discard
            // frees the package path, deletes the .uasset and notifies the asset registry, which
            // is everything this teardown needs: nothing is asserted after it, the parent patch
            // records the reference as document data (class-name GUID) rather than a hard object
            // pointer, and both fixture users have already dropped the asset-manager entry.
            if (!ReferencedPackagePath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(ReferencedPackagePath))
            {
                CleanupTestAsset(ReferencedPackagePath);
            }
            if (ParentPatch)
            {
                ParentPatch->RemoveFromRoot();
            }
        }

    private:
        FString ParentObjectPath;
    };
#endif // MCP_MSIR_TEST_HAS_ASSET_MANAGER
}

// Counterfactual: if FMSIRDecompiler::BuildMetaSoundIrText is reverted to return
// FMSIRResult{} (bSuccess=false, empty Text), this test fails because the bSuccess
// assert trips and all subsequent Contains checks against an empty Text fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMSIRDecompilerEmitsHeaderAndSections,
    "PinWright.Assets.MetaSound.MSIR.EmitsHeaderAndSections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMSIRDecompilerEmitsHeaderAndSections::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientMSIRPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    AddFloatInput(Patch, TEXT("RPM"));

    const FMSIRResult Result = FMSIRDecompiler::BuildMetaSoundIrText(Patch);
    TestTrue(TEXT("BuildMetaSoundIrText succeeds"), Result.bSuccess);
    TestTrue(TEXT("MSIR text starts with metasound header"), Result.Text.Contains(TEXT("metasound ")));
    TestTrue(TEXT("MSIR text contains kind=MetaSoundPatch"), Result.Text.Contains(TEXT("kind=MetaSoundPatch")));
    TestTrue(TEXT("MSIR text contains 'input float RPM'"), Result.Text.Contains(TEXT("input Float RPM")));

    FString Trimmed = Result.Text;
    Trimmed.TrimEndInline();
    TestTrue(TEXT("MSIR text ends with closing brace"), Trimmed.EndsWith(TEXT("}")));

    Patch->RemoveFromRoot();
    return true;
}

#if MCP_MSIR_TEST_HAS_ASSET_MANAGER
// Counterfactual: if the asset-class GUID resolver in MSIRDecompiler.cpp is
// reverted, the text still contains None.<AssetClassGuid> because the decompiler
// formats the dependency namespace and raw GUID directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMSIRDecompilerResolvesAssetClassGuidReferences,
    "PinWright.Assets.MSIRDecompiler.ResolvesAssetClassGuidReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMSIRDecompilerResolvesAssetClassGuidReferences::RunTest(const FString& Parameters)
{
    FString ReferencedObjectPath;
    UMetaSoundPatch* ReferencedPatch = NewTransientMSIRPatch(ReferencedObjectPath);
    TestNotNull(TEXT("Referenced MetaSoundPatch created"), ReferencedPatch);
    if (!ReferencedPatch)
    {
        return false;
    }
    bool bReferencedPatchRegistered = false;
    ON_SCOPE_EXIT
    {
        if (bReferencedPatchRegistered)
        {
            if (Metasound::Frontend::IMetaSoundAssetManager* AssetManager =
                Metasound::Frontend::IMetaSoundAssetManager::Get())
            {
                AssetManager->RemoveAsset(*ReferencedPatch);
            }
        }
        ReferencedPatch->RemoveFromRoot();
    };

    AddFloatInput(ReferencedPatch, TEXT("Gain"));
    AddAudioOutput(ReferencedPatch, TEXT("OutAudio"));

    FString ReferencedAssetPath;
    FString AssetClassGuid;
    TestTrue(TEXT("Referenced MetaSoundPatch registers with MetaSound asset manager"),
        RegisterPatchWithAssetManager(ReferencedPatch, ReferencedAssetPath, AssetClassGuid, bReferencedPatchRegistered));

    FString ParentObjectPath;
    UMetaSoundPatch* ParentPatch = NewTransientMSIRPatch(ParentObjectPath);
    TestNotNull(TEXT("Parent MetaSoundPatch created"), ParentPatch);
    if (!ParentPatch)
    {
        return false;
    }
    ON_SCOPE_EXIT { ParentPatch->RemoveFromRoot(); };

    TestTrue(TEXT("Parent MetaSoundPatch references the registered patch"),
        AddReferencedPatchNode(ParentPatch, ReferencedPatch));

    const FMSIRResult Result = FMSIRDecompiler::BuildMetaSoundIrText(ParentPatch);
    TestTrue(TEXT("BuildMetaSoundIrText succeeds"), Result.bSuccess);
    TestFalse(TEXT("MSIR text does not emit None.<AssetClassGuid>"),
        Result.Text.Contains(FString::Printf(TEXT("None.%s"), *AssetClassGuid)));
    TestTrue(TEXT("MSIR text contains resolved MetaSound asset path token"),
        Result.Text.Contains(FIrTextUtils::FormatNameToken(ReferencedAssetPath)));
    TestFalse(TEXT("Resolvable asset class GUID does not warn"),
        ContainsWarningCode(Result.Warnings, TEXT("MSIR_UNRESOLVED_CLASS_REF")));

    return true;
}

// Counterfactual: if the resolver only uses IMetaSoundAssetManager::FindAssetPath,
// this test fails because the referenced patch is deliberately removed from the
// asset manager before decompilation and the emitted MSIR falls back to None.<AssetClassGuid>.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMSIRDecompilerResolvesAssetClassGuidReferencesFromAssetRegistry,
    "PinWright.Assets.MSIRDecompiler.ResolvesAssetClassGuidReferencesFromAssetRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMSIRDecompilerResolvesAssetClassGuidReferencesFromAssetRegistry::RunTest(const FString& Parameters)
{
    FOnDiskReferencingPatchFixture Fixture(*this);
    if (!Fixture.bSetupOk)
    {
        return false;
    }

    // Drop the referenced patch from the asset manager so only the asset-registry path
    // can resolve it.
    Fixture.RemoveReferencedFromAssetManager();

    const FMSIRResult Result = FMSIRDecompiler::BuildMetaSoundIrText(Fixture.ParentPatch);
    TestTrue(TEXT("BuildMetaSoundIrText succeeds"), Result.bSuccess);
    TestFalse(TEXT("MSIR text does not emit None.<AssetClassGuid> after asset-manager removal"),
        Result.Text.Contains(FString::Printf(TEXT("None.%s"), *Fixture.AssetClassGuid)));
    TestTrue(TEXT("MSIR text contains asset-registry MetaSound asset path token"),
        Result.Text.Contains(FIrTextUtils::FormatNameToken(Fixture.ReferencedObjectPath)));
    TestFalse(TEXT("Asset-registry-resolvable class GUID does not warn"),
        ContainsWarningCode(Result.Warnings, TEXT("MSIR_UNRESOLVED_CLASS_REF")));

    return true;
}

// Counterfactual: if the version-agnostic ClassName fallback in MSIRDecompiler.cpp is
// reverted (resolver only matches the exact FMetaSoundAssetKey = ClassName + Version),
// this test fails. The parent's recorded dependency version is deliberately skewed away
// from the referenced asset's registered version, and the asset is removed from the
// MetaSound asset manager, so BOTH exact-key paths (asset manager FindAssetPath and the
// exact-key asset-registry scan) miss and the emit falls back to None.<AssetClassGuid>.
// Only the ClassName-only asset-registry fallback resolves it across the version skew.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMSIRDecompilerResolvesAssetClassGuidReferencesAcrossVersionSkew,
    "PinWright.Assets.MSIRDecompiler.ResolvesAssetClassGuidReferencesAcrossVersionSkew",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMSIRDecompilerResolvesAssetClassGuidReferencesAcrossVersionSkew::RunTest(const FString& Parameters)
{
    FOnDiskReferencingPatchFixture Fixture(*this);
    if (!Fixture.bSetupOk)
    {
        return false;
    }

    // Skew the parent's recorded dependency version so neither exact-key path can match.
    const int32 SkewedDeps = SkewAssetClassDependencyVersions(Fixture.ParentPatch);
    TestTrue(TEXT("Parent has an asset-class dependency whose version was skewed"), SkewedDeps > 0);

    // Drop the referenced patch from the asset manager so the exact-key asset-manager
    // lookup also misses; the asset stays in the asset registry for the ClassName fallback.
    Fixture.RemoveReferencedFromAssetManager();

    const FMSIRResult Result = FMSIRDecompiler::BuildMetaSoundIrText(Fixture.ParentPatch);
    TestTrue(TEXT("BuildMetaSoundIrText succeeds"), Result.bSuccess);
    TestFalse(TEXT("MSIR text does not emit None.<AssetClassGuid> under version skew"),
        Result.Text.Contains(FString::Printf(TEXT("None.%s"), *Fixture.AssetClassGuid)));
    TestTrue(TEXT("MSIR text contains ClassName-resolved MetaSound asset path token"),
        Result.Text.Contains(FIrTextUtils::FormatNameToken(Fixture.ReferencedObjectPath)));
    TestFalse(TEXT("Version-skewed but ClassName-resolvable class GUID does not warn"),
        ContainsWarningCode(Result.Warnings, TEXT("MSIR_UNRESOLVED_CLASS_REF")));

    return true;
}
#endif // MCP_MSIR_TEST_HAS_ASSET_MANAGER

// Counterfactual: if a future maintainer adds a dump-path-only formatting branch
// in BuildMetaSoundIrText that diverges from the live RPC path, the byte-equal
// assert below fails with a visible diff. If BuildMetaSoundIrText is reverted to
// empty text, the IrSidecarRegistry skips writing the sidecar (registry guards
// against empty bodies) and HasDumpFile(msir.txt) fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMSIRDualSurfaceZeroDivergence,
    "PinWright.Assets.MetaSound.MSIR.DualSurfaceZeroDivergence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMSIRDualSurfaceZeroDivergence::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientMSIRPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    AddFloatInput(Patch, TEXT("RPM"));
    AddAudioOutput(Patch, TEXT("OutAudio"));

    const FMSIRResult LiveResult = FMSIRDecompiler::BuildMetaSoundIrText(Patch);
    TestTrue(TEXT("Live BuildMetaSoundIrText succeeds"), LiveResult.bSuccess);

    // asset.dump requires the package to exist on disk (DoesPackageExist gate);
    // promote the transient patch to a real Content asset, dump, then delete.
    const FString PackagePath = Patch->GetOutermost() ? Patch->GetOutermost()->GetName() : FString();
    const bool bSaved = PersistMSIRPatchToDisk(Patch);
    TestTrue(TEXT("MetaSoundPatch package saves to disk"), bSaved);

    const FString ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("MSIRDecompilerTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult DumpResult =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for MetaSoundPatch"), DumpResult.ErrorCode.IsEmpty());
    TestTrue(TEXT("msir.txt sidecar is written"), HasDumpFile(DumpResult.WrittenPaths, DumpFileNames::Msir));

    const FString MsirPath = FindDumpFile(DumpResult.WrittenPaths, DumpFileNames::Msir);
    FString FileText;
    TestTrue(TEXT("msir.txt loads from disk"), FFileHelper::LoadFileToString(FileText, *MsirPath));
    TestEqual(TEXT("RPC ir text and msir.txt sidecar are byte-identical"), FileText, LiveResult.Text);

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    Patch->RemoveFromRoot();
    // End-of-test teardown only — every assertion above has already run and nothing reads the
    // patch afterwards, so the discard path (no ObjectTools::ForceDeleteObjects) is sufficient.
    if (!PackagePath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(PackagePath))
    {
        CleanupTestAsset(PackagePath);
    }
    return true;
}

#endif // __has_include("MetasoundSource.h")
