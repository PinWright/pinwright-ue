// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestMetaSoundPatchPreset.cpp
//
// Covers MetaSound patch + preset creation at two levels:
//   1. FCreateMetaSoundPatchAndPresetTest — the factory contract, driven through the shared
//      production helper PinWright::MetaSound::MakeMetaSoundPresetTemplate.
//   2. FMetaSoundCreateSaveWritesToDiskTest — save:true genuinely writes the .uasset.
//   3. FCreateMetaSoundPresetIsGenuinePresetTest — the audio.authoring.create_metasound_preset
//      RPC end to end, asserting PRESETNESS in memory and on disk.
//
// Why (3) exists: a "did the call return an object" assertion cannot fail here. UMetaSoundFactory
// hands back a perfectly valid, perfectly EMPTY UMetaSoundPatch whether or not the parent link
// was ever applied, so TestNotNull passes on a blank non-preset. The handler used to set the
// factory's `ReferencedMetaSoundObject`, which is meta=(Deprecated = 5.8) with zero engine
// readers — the assignment did nothing, no compiler warning was emitted, and the RPC reported
// success with a referencedAssetPath describing a link the asset did not carry. Every assertion
// below therefore reads the parent back off the created DOCUMENT, never off the request.

#include "Misc/AutomationTest.h"

#if __has_include("MetasoundFactory.h")
#if __has_include("MetasoundSource.h")

#include "MetasoundSource.h"
#include "Metasound.h"
#include "MetasoundFactory.h"
#include "UObject/Package.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"
#include "Tests/TestUtils.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Compat/EngineVersionCompat.h"

// Named (not anonymous) namespace: test TUs are Unity-merged, and an anonymous-namespace helper
// with a plausible name collides across merged files (CLAUDE.md → Build notes).
namespace MetaSoundPresetTestPrivate
{
    // True when the file's raw bytes contain Needle. Probes BOTH encodings a package name table
    // uses — ANSI for pure-ASCII FNames, UTF-16LE otherwise — so the assertion does not depend
    // on which one the serializer picked.
    inline bool PackageBytesContain(const FString& PackageFilename, const FString& Needle)
    {
        TArray<uint8> Bytes;
        if (Needle.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *PackageFilename))
        {
            return false;
        }

        auto ContainsPattern = [&Bytes](const TArray<uint8>& Pattern)
        {
            if (Pattern.Num() == 0 || Bytes.Num() < Pattern.Num())
            {
                return false;
            }
            for (int32 Start = 0; Start <= Bytes.Num() - Pattern.Num(); ++Start)
            {
                if (FMemory::Memcmp(Bytes.GetData() + Start, Pattern.GetData(), Pattern.Num()) == 0)
                {
                    return true;
                }
            }
            return false;
        };

        TArray<uint8> Ansi;
        TArray<uint8> Utf16;
        Ansi.Reserve(Needle.Len());
        Utf16.Reserve(Needle.Len() * 2);
        for (int32 Index = 0; Index < Needle.Len(); ++Index)
        {
            const uint16 Code = static_cast<uint16>(Needle[Index]);
            Ansi.Add(static_cast<uint8>(Code & 0xFF));
            Utf16.Add(static_cast<uint8>(Code & 0xFF));
            Utf16.Add(static_cast<uint8>((Code >> 8) & 0xFF));
        }

        return ContainsPattern(Ansi) || ContainsPattern(Utf16);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateMetaSoundPatchAndPresetTest,
    "PinWright.Assets.CreateMetaSoundPatchAndPreset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateMetaSoundPatchAndPresetTest::RunTest(const FString& Parameters)
{
    // --- 1. Create a plain MetaSound patch via UMetaSoundFactory ---
    const FString PatchName = FString::Printf(
        TEXT("MS_TestPatch_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PatchPackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *PatchName);
    UPackage* PatchPackage = CreatePackage(*PatchPackageName);
    TestNotNull(TEXT("Patch package created"), PatchPackage);
    if (!PatchPackage)
    {
        return false;
    }

    UMetaSoundFactory* PatchFactory = NewObject<UMetaSoundFactory>();
    UMetaSoundPatch* Patch = Cast<UMetaSoundPatch>(
        PatchFactory->FactoryCreateNew(UMetaSoundPatch::StaticClass(), PatchPackage,
                                       FName(*PatchName), RF_Public | RF_Standalone,
                                       nullptr, GWarn));

    TestNotNull(TEXT("UMetaSoundFactory::FactoryCreateNew returns a non-null UMetaSoundPatch"), Patch);
    if (!Patch)
    {
        return false;
    }
    Patch->AddToRoot();

    // A plain patch must NOT read as a preset. This is the negative half of the presetness
    // predicate: without it, an implementation of FindMetaSoundPresetParent that returned
    // something for every MetaSound would satisfy the positive assertion below.
    //
    // Guarded on the same feature macro as its positive half below. FindMetaSoundPresetParent
    // only exists where the 5.8 document-template headers do, and the two assertions are one
    // predicate - leaving the negative half unguarded stopped this TU compiling at all on an
    // engine with no preset template.
#if PW_METASOUND_HAS_PRESET_TEMPLATE
    TestNull(TEXT("A plain (non-preset) patch carries no preset parent"),
        PinWright::MetaSound::FindMetaSoundPresetParent(Patch));
#endif

    // --- 2. Create a patch preset referencing the patch above ---
    const FString PresetName = FString::Printf(
        TEXT("MS_TestPatchPreset_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PresetPackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *PresetName);
    UPackage* PresetPackage = CreatePackage(*PresetPackageName);
    TestNotNull(TEXT("Preset package created"), PresetPackage);
    if (!PresetPackage)
    {
        Patch->RemoveFromRoot();
        // Objects are real assets now (non-transient); clear dirty so they don't leave a dirty package behind.
        PatchPackage->SetDirtyFlag(false);
        return false;
    }

#if (UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0) && UE_VERSION_OLDER_THAN(5, 6, 0)) || !PW_METASOUND_HAS_PRESET_TEMPLATE
    // UE 5.5 only: preset creation routes through InitAsset -> Builder.ConvertToPreset(doc),
    // whose 5.5 default arg is a default-constructed TSharedRef that hard-fatals
    // (CoreMisc.cpp:435). Also skipped where the 5.8 document-template headers are absent, since
    // there is then no non-deprecated way to author a preset at all.
    (void)PresetPackage;
#else
    // Same construction the production handler uses — one shared helper, so a regression in the
    // handler's preset construction regresses here too.
    UMetaSoundFactory* PresetFactory = NewObject<UMetaSoundFactory>();
    PresetFactory->Template = PinWright::MetaSound::MakeMetaSoundPresetTemplate(Patch);
    PresetFactory->SelectedObjects.Add(Patch);
    UMetaSoundPatch* PatchPreset = Cast<UMetaSoundPatch>(
        PresetFactory->FactoryCreateNew(UMetaSoundPatch::StaticClass(), PresetPackage,
                                        FName(*PresetName), RF_Public | RF_Standalone,
                                        nullptr, GWarn));

    TestNotNull(TEXT("UMetaSoundFactory with a preset Template produces a non-null patch"), PatchPreset);

    // The assertion that a blank patch cannot satisfy: the created document must carry a
    // FMetaSoundFrontendPresetTemplate whose Parent is the patch above. Revert
    // MakeMetaSoundPresetTemplate to the deprecated ReferencedMetaSoundObject assignment and
    // this is nullptr, because nothing in the engine reads that property any more.
    TestTrue(TEXT("Created preset's document references the source patch as its preset parent"),
        PinWright::MetaSound::FindMetaSoundPresetParent(PatchPreset) == Patch);
#endif

    // Cleanup — objects are real assets now (non-transient) and never saved, so clear the
    // dirty flag on both packages to avoid leaving a dirty (unsaved) package behind.
    Patch->RemoveFromRoot();
    PatchPackage->SetDirtyFlag(false);
    PresetPackage->SetDirtyFlag(false);
    return true;
}

// ---------------------------------------------------------------------------
// Regression: create_metasound_patch's save:true must write the .uasset to disk
// (B-metasound-create-save-no-disk-write).
//
// The create handlers previously routed save:true through the mark-dirty-only
// McpSafeAssetSave, which returns true WITHOUT writing the .uasset (deferred to
// dodge the bulkdata-corruption vector) — so the create reported existsAfter:true
// while nothing landed on disk, and the asset vanished on editor close / git reset.
// The fix routes save:true through SaveAssetToDiskReportingPresence (the same
// real-save helper the niagara.create_* / create_level fixes adopted), which
// actually saves and gates the result on the file existing on disk.
//
// This test drives the production helper SaveAssetToDiskReportingPresence against
// a freshly factory-created MetaSound patch and asserts the .uasset is genuinely on
// disk afterward. If a handler is reverted to McpSafeAssetSave (mark-dirty only),
// the helper's disk probe (IFileManager::FileSize >= 0) — and these assertions —
// fail, because no file is written.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundCreateSaveWritesToDiskTest,
    "PinWright.Assets.MetaSoundCreateSaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundCreateSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    const FString PatchName = FString::Printf(
        TEXT("MS_SaveDisk_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *PatchName);

    UPackage* Package = CreatePackage(*PackageName);
    if (!TestNotNull(TEXT("Patch package created"), Package))
    {
        return false;
    }

    UMetaSoundFactory* Factory = NewObject<UMetaSoundFactory>();
    UMetaSoundPatch* Patch = Cast<UMetaSoundPatch>(
        Factory->FactoryCreateNew(UMetaSoundPatch::StaticClass(), Package,
                                  FName(*PatchName), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!TestNotNull(TEXT("MetaSound patch created"), Patch))
    {
        Package->SetDirtyFlag(false);
        return false;
    }

    // Register the new asset exactly as the production create_metasound_patch handler
    // does. Without this, SaveAssetToDiskReportingPresence won't flush a brand-new
    // in-memory package to disk — this step IS part of the production save path the
    // fix exercises, so the test must include it to mirror the handler faithfully.
    FAssetRegistryModule::AssetCreated(Patch);

    // The .uasset must NOT exist before the save (fresh, never-saved package).
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());
    TestTrue(TEXT("No .uasset on disk before save (mark-dirty-only state)"),
        IFileManager::Get().FileSize(*PackageFilename) < 0);

    // Production save path used by the fixed create handlers: a real disk write
    // gated on the file actually landing on disk (ShouldTreatAssetSaveAsSuccess).
    FString OutPackage;
    int64 OutSize = 0;
    const bool bSaved = SaveAssetToDiskReportingPresence(Patch, /*bForce=*/true, &OutPackage, &OutSize);

    // The fix's whole point: save:true reports saved only when the file is on disk.
    TestTrue(TEXT("SaveAssetToDiskReportingPresence reports saved:true for the patch"), bSaved);

    // Hard disk-presence proof: OutSize is the helper's own IFileManager::FileSize
    // probe of the saved .uasset (0 when absent), so OutSize > 0 proves the file is
    // genuinely on disk. This is the assertion that fails if save:true reverts to the
    // mark-dirty-only McpSafeAssetSave.
    TestTrue(TEXT("Reported on-disk size is non-zero"), OutSize > 0);

    // Cleanup the saved test asset (deletes the .uasset from disk). The patch was
    // never added to the root set, so no RemoveFromRoot is needed here.
    CleanupTestAsset(PackageName);
    return true;
}

// ---------------------------------------------------------------------------
// Regression: audio.authoring.create_metasound_preset must produce a GENUINE preset
// (B-metasound-preset-deprecated-factory-property).
//
// Before the fix the handler set UMetaSoundBaseFactory::ReferencedMetaSoundObject, which is
// `meta = (Deprecated = 5.8, DeprecationMessage = "Use document template instead")`
// (MetasoundFactory.h:23-24) and has zero readers left in the MetaSound plugin. The
// meta=(Deprecated=...) spelling emits no compiler warning, so the write compiled clean, did
// nothing, and the RPC returned success plus a referencedAssetPath describing a link the created
// asset did not carry. The fix sets the factory's `Template` to a
// TInstancedStruct<FMetaSoundFrontendPresetTemplate> whose Parent is the referenced asset.
//
// Counterfactual: revert the handler to the ReferencedMetaSoundObject assignment and the created
// document carries no template — the handler's own PRESET_NOT_APPLIED gate fires (bSuccess is
// false), isPreset is absent, FindMetaSoundPresetParent returns null, and the saved .uasset
// contains neither the preset template struct nor any reference to the parent. Four independent
// assertions, three of which read state the handler cannot fabricate.
//
// The first version of this test failed at assertions 2 and 3 while the disk half passed, which
// read like the product fix was broken. It was not: verified in a live editor on the 08:51 build,
// create_metasound_preset produces a real preset — describe_metasound reports isPreset:true and
// one graph node named after the parent (FRebuildPresetRootGraph's output) against isPreset:false
// and zero nodes for a plain patch, and it still reads as a preset after asset.reload pulls it
// back off disk. The test was resolving the response's PACKAGE path with FindObject and getting
// the UPackage. Hence the object-path normalization below; do not undo it.
#if PW_METASOUND_HAS_PRESET_TEMPLATE && !(UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0) && UE_VERSION_OLDER_THAN(5, 6, 0))
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateMetaSoundPresetIsGenuinePresetTest,
    "PinWright.Assets.CreateMetaSoundPresetIsGenuinePreset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateMetaSoundPresetIsGenuinePresetTest::RunTest(const FString& Parameters)
{
    const FString TestPath = TEXT("/Game/PinWrightTests");
    const FString ParentName = FString::Printf(
        TEXT("MS_PresetParent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PresetName = FString::Printf(
        TEXT("MS_PresetChild_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // --- Parent patch, created through the production verb so it is registered exactly as a
    // real caller's parent would be (the preset's ConfigureDocument builds against it). ---
    TSharedPtr<FJsonObject> ParentPayload = MakeShared<FJsonObject>();
    ParentPayload->SetStringField(TEXT("name"), ParentName);
    ParentPayload->SetStringField(TEXT("path"), TestPath);
    ParentPayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture ParentCapture;
    if (!TestTrue(TEXT("create_metasound_patch handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_metasound_patch"), ParentPayload, ParentCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("create_metasound_patch succeeds for the preset parent"), ParentCapture.bSuccess)
        || !ParentCapture.Result.IsValid())
    {
        return false;
    }
    FString ParentAssetPath;
    if (!TestTrue(TEXT("create_metasound_patch reports the parent assetPath"),
            ParentCapture.Result->TryGetStringField(TEXT("assetPath"), ParentAssetPath)))
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ParentAssetPath);
    };

    // --- The verb under test. ---
    TSharedPtr<FJsonObject> PresetPayload = MakeShared<FJsonObject>();
    PresetPayload->SetStringField(TEXT("name"), PresetName);
    PresetPayload->SetStringField(TEXT("referencedSource"), ParentAssetPath);
    PresetPayload->SetStringField(TEXT("path"), TestPath);
    PresetPayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture PresetCapture;
    if (!TestTrue(TEXT("create_metasound_preset handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_metasound_preset"), PresetPayload, PresetCapture)))
    {
        return false;
    }

    // Assertion 1 — the handler's own presetness gate. A blank non-preset returns
    // PRESET_NOT_APPLIED rather than a fake success.
    if (!TestTrue(*FString::Printf(TEXT("create_metasound_preset succeeds (error was '%s': %s)"),
            *PresetCapture.ErrorCode, *PresetCapture.Message), PresetCapture.bSuccess)
        || !PresetCapture.Result.IsValid())
    {
        return false;
    }

    FString PresetAssetPath;
    if (!TestTrue(TEXT("create_metasound_preset reports the preset assetPath"),
            PresetCapture.Result->TryGetStringField(TEXT("assetPath"), PresetAssetPath)))
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PresetAssetPath);
    };

    // TWO PATH SHAPES IN ONE RESPONSE — the trap that made the first version of this test
    // report a product bug that did not exist. `assetPath` is rewritten by
    // AddAssetVerification (AssetUtils.cpp:1536-1545 ResolveVerificationAssetPath) to the
    // PACKAGE path for any top-level asset, while `presetParentAssetPath` /
    // `referencedAssetPath` are GetPathName(), i.e. the OBJECT path (Package.Object).
    // Confirmed at runtime: create_metasound_patch answered
    //   assetPath "/Game/PinWrightDiag/MS_DiagParent_A1"
    // and create_metasound_preset answered
    //   presetParentAssetPath "/Game/PinWrightDiag/MS_DiagParent_A1.MS_DiagParent_A1".
    // Comparing the two forms directly is a guaranteed false mismatch, and — far worse —
    // FindObject(nullptr, <package path>) resolves the outer UPackage rather than the asset
    // (docs/lessons.md, the ResolveUObjectByPath entry), which is non-null, so a TestNotNull
    // guard waves it through and every later assertion then interrogates a UPackage.
    // Normalize to the object-path form before comparing or resolving. This helper is
    // idempotent: the save-report path may already preserve the handler's Package.Object value,
    // and appending the leaf again would manufacture Package.Object.Package.Object.
    FString ParentObjectPath;
    FString ParentPathError;
    // The response is allowed to use either package or object form for assetPath; the expected
    // parent must be the one canonical object path, never a second concatenation.
    TestTrue(TEXT("The requested parent path normalizes to an object path"),
        NormalizeToObjectPath(ParentAssetPath, ParentObjectPath, ParentPathError));

    // Assertion 2 — the response claims presetness, and claims the right parent.
    bool bIsPreset = false;
    TestTrue(TEXT("Response carries isPreset"),
        PresetCapture.Result->TryGetBoolField(TEXT("isPreset"), bIsPreset));
    TestTrue(TEXT("Response reports isPreset:true"), bIsPreset);
    FString ReportedParent;
    PresetCapture.Result->TryGetStringField(TEXT("presetParentAssetPath"), ReportedParent);
    TestEqual(TEXT("Response's presetParentAssetPath is the requested parent"),
        ReportedParent, ParentObjectPath);

    // Assertion 3 — the created OBJECT's document, read back independently of the response.
    // Resolved through the production MetaSound resolver, which accepts either path shape and
    // carries the documented package-path retry, rather than a bare FindObject that silently
    // yields the UPackage.
    UObject* ParentAsset = PinWright::MetaSound::LoadMetaSoundDocumentObject(ParentAssetPath);
    UObject* PresetAsset = PinWright::MetaSound::LoadMetaSoundDocumentObject(PresetAssetPath);
    if (!TestNotNull(TEXT("Parent patch resolves to a MetaSound document (not its UPackage)"), ParentAsset)
        || !TestNotNull(TEXT("Created preset resolves to a MetaSound document (not its UPackage)"), PresetAsset))
    {
        return false;
    }
    UObject* AppliedParent = PinWright::MetaSound::FindMetaSoundPresetParent(PresetAsset);
    TestNotNull(TEXT("Created asset's document carries a preset template (not a blank patch)"), AppliedParent);
    TestTrue(TEXT("Preset template's parent is the referenced MetaSound"), AppliedParent == ParentAsset);

    // Assertion 4 — the same claim against DISK, not against the object just written. An
    // in-memory read-back returns the handler's own assignment whether or not anything was
    // serialized; only the .uasset bytes prove the link survived the save (project CLAUDE.md:
    // "verify a write against disk, not against the object you just wrote").
    const FString PresetPackageName = FPackageName::ObjectPathToPackageName(PresetAssetPath);
    const FString PresetFilename = FPackageName::LongPackageNameToFilename(
        PresetPackageName, FPackageName::GetAssetPackageExtension());
    if (TestTrue(TEXT("Preset .uasset is on disk"),
            IFileManager::Get().FileSize(*PresetFilename) > 0))
    {
        // FMetasoundFrontendDocument::Template is a serialized UPROPERTY, so the instanced
        // struct's UScriptStruct lands in the package's import/name table by name.
        TestTrue(TEXT("Saved .uasset names FMetaSoundFrontendPresetTemplate"),
            MetaSoundPresetTestPrivate::PackageBytesContain(
                PresetFilename, TEXT("MetaSoundFrontendPresetTemplate")));
        // The parent is a hard object reference (Template.Parent, plus
        // ReferencedAssetClassObjects), so its GUID-unique asset name must appear in the
        // preset's package. A blank patch references nothing and cannot contain it.
        TestTrue(TEXT("Saved .uasset hard-references the parent MetaSound by name"),
            MetaSoundPresetTestPrivate::PackageBytesContain(PresetFilename, ParentName));

        // NEGATIVE CONTROL for the marker above. Without this, "the bytes contain
        // MetaSoundFrontendPresetTemplate" would be an unfalsifiable claim — it proves the fix
        // only if a NON-preset's package demonstrably lacks the same string. The parent was
        // created by create_metasound_patch with save:true, so it is a real blank patch on disk
        // and is the correct control. Measured on the 08:51 build: the preset's .uasset contains
        // the marker once, the blank patch's contains it zero times.
        const FString ParentFilename = FPackageName::LongPackageNameToFilename(
            FPackageName::ObjectPathToPackageName(ParentAssetPath),
            FPackageName::GetAssetPackageExtension());
        // Asserted, not `if`-guarded: a bare `if (FileSize > 0)` would let a missing control file
        // skip the control silently, which is the same unfalsifiable shape the control exists to
        // remove.
        if (TestTrue(TEXT("Control: the blank parent patch's .uasset is on disk"),
                IFileManager::Get().FileSize(*ParentFilename) > 0))
        {
            TestFalse(TEXT("A blank patch's .uasset does NOT name FMetaSoundFrontendPresetTemplate"),
                MetaSoundPresetTestPrivate::PackageBytesContain(
                    ParentFilename, TEXT("MetaSoundFrontendPresetTemplate")));
        }
    }

    // Assertion 5 — the READ side of the same deprecated-field family, which the preset fix
    // exposed and which had no coverage at all. MetaSoundDumpBuilder used to source
    // rootGraph.isPreset from FMetasoundFrontendGraphClassPresetOptions::bIsPreset, a
    // meta=(DeprecatedProperty) field that UE 5.8's document versioning CLEARS — so
    // describe_metasound and the metasound.json sidecar reported isPreset:false for every
    // preset, disagreeing with their own assetKind. Revert MetaSoundDumpBuilder.cpp to that
    // field and this assertion fails while everything above still passes; the two halves are
    // independent, which is the point of asserting both.
    // Live-verified shape: preset -> isPreset true + one node named for the parent;
    // plain patch -> isPreset false + zero nodes.
    {
        TSharedPtr<FJsonObject> DescribePayload = MakeShared<FJsonObject>();
        DescribePayload->SetStringField(TEXT("assetPath"), PresetAssetPath);
        FTestResponseCapture DescribeCapture;
        if (TestTrue(TEXT("describe_metasound handler registered"),
                InvokeHandlerWithCapture(TEXT("audio.authoring.describe_metasound"),
                    DescribePayload, DescribeCapture))
            && TestTrue(TEXT("describe_metasound succeeds on the preset"), DescribeCapture.bSuccess)
            && DescribeCapture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* RootGraphJson = nullptr;
            if (TestTrue(TEXT("describe_metasound reports rootGraph"),
                    DescribeCapture.Result->TryGetObjectField(TEXT("rootGraph"), RootGraphJson))
                && RootGraphJson && RootGraphJson->IsValid())
            {
                bool bDescribedPreset = false;
                (*RootGraphJson)->TryGetBoolField(TEXT("isPreset"), bDescribedPreset);
                TestTrue(TEXT("describe_metasound reports rootGraph.isPreset:true for a preset"),
                    bDescribedPreset);
            }
        }

        // Negative control, same reasoning as the .uasset one: without it, an isPreset that is
        // hardcoded true would satisfy the assertion above.
        TSharedPtr<FJsonObject> ControlPayload = MakeShared<FJsonObject>();
        ControlPayload->SetStringField(TEXT("assetPath"), ParentAssetPath);
        FTestResponseCapture ControlCapture;
        if (InvokeHandlerWithCapture(TEXT("audio.authoring.describe_metasound"),
                ControlPayload, ControlCapture)
            && TestTrue(TEXT("describe_metasound succeeds on the blank control patch"),
                ControlCapture.bSuccess)
            && ControlCapture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* ControlRootGraph = nullptr;
            if (ControlCapture.Result->TryGetObjectField(TEXT("rootGraph"), ControlRootGraph)
                && ControlRootGraph && ControlRootGraph->IsValid())
            {
                bool bControlPreset = true;
                (*ControlRootGraph)->TryGetBoolField(TEXT("isPreset"), bControlPreset);
                TestFalse(TEXT("describe_metasound reports rootGraph.isPreset:false for a blank patch"),
                    bControlPreset);
            }
        }
    }

    return true;
}
#endif // PW_METASOUND_HAS_PRESET_TEMPLATE && not UE 5.5

// ---------------------------------------------------------------------------
// Regression: the `save` flag on the MetaSound authoring VERBS reaches the disk
// (B-metasound-create-save-no-disk-write #3/#4).
//
// FMetaSoundCreateSaveWritesToDiskTest above proves the shared helper writes a file. It cannot
// prove a handler CALLS it: every MetaSound verb used to route `save` through the mark-dirty-only
// McpSafeAssetSave (several ignored the flag entirely), so a whole graph reported success and then
// vanished on a cold restart. This test drives the RPCs themselves — the Source create plus one
// mutator — and asserts against package BYTES, not against the objects the handlers just wrote.
//
// The mutator half also pins the FinishBuilding-before-save ordering: the input's GUID-unique name
// can only appear in the .uasset if the builder's edit was flushed into the document before the
// write. A save taken ahead of PW_METASOUND_FINISH_BUILDING serializes the pre-edit document and
// the name is absent.
//
// The save:false leg is the negative control. Without it, "the bytes contain the name" would be
// satisfied by a handler that saves unconditionally — which is the other half of the same defect,
// a `save` param that is accepted and ignored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundVerbSaveFlagReachesDiskTest,
    "PinWright.Assets.MetaSoundVerbSaveFlagReachesDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundVerbSaveFlagReachesDiskTest::RunTest(const FString& Parameters)
{
    const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourceName = FString::Printf(TEXT("MS_VerbSave_%s"), *Unique);
    const FString TestPath = TEXT("/Game/PinWrightTests");
    const FString PackageName = FString::Printf(TEXT("%s/%s"), *TestPath, *SourceName);
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());

    // --- 1. create_metasound (Source) with save:true ---
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), SourceName);
    CreatePayload->SetStringField(TEXT("path"), TestPath);
    CreatePayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture CreateCapture;
    if (!TestTrue(TEXT("create_metasound handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_metasound"), CreatePayload, CreateCapture)))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("create_metasound succeeds (error was '%s': %s)"),
            *CreateCapture.ErrorCode, *CreateCapture.Message), CreateCapture.bSuccess)
        || !CreateCapture.Result.IsValid())
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackageName);
    };

    // The response must claim persistence only because it measured it. saved:true here is the
    // field that used to be absent entirely while existsAfter:true implied the same thing.
    bool bCreateSaved = false;
    CreateCapture.Result->TryGetBoolField(TEXT("saved"), bCreateSaved);
    TestTrue(TEXT("create_metasound save:true reports saved:true"), bCreateSaved);
    bool bCreateSaveRequested = false;
    CreateCapture.Result->TryGetBoolField(TEXT("saveRequested"), bCreateSaveRequested);
    TestTrue(TEXT("create_metasound echoes saveRequested:true"), bCreateSaveRequested);
    TestFalse(TEXT("create_metasound save:true does not report pendingFlush"),
        CreateCapture.Result->HasField(TEXT("pendingFlush")));

    // And the claim must agree with the disk. This assertion fails outright if the handler is
    // reverted to McpSafeAssetSave: nothing is written, so there is no file at all.
    if (!TestTrue(TEXT("create_metasound save:true leaves a .uasset on disk"),
            IFileManager::Get().FileSize(*PackageFilename) > 0))
    {
        return false;
    }

    // --- 2. add_metasound_input with save:true — the mutator half ---
    const FString InputName = FString::Printf(TEXT("PwSavedInput_%s"), *Unique);
    TSharedPtr<FJsonObject> InputPayload = MakeShared<FJsonObject>();
    InputPayload->SetStringField(TEXT("assetPath"), PackageName);
    InputPayload->SetStringField(TEXT("inputName"), InputName);
    InputPayload->SetStringField(TEXT("inputType"), TEXT("Float"));
    InputPayload->SetBoolField(TEXT("save"), true);

    FTestResponseCapture InputCapture;
    if (TestTrue(TEXT("add_metasound_input handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_input"), InputPayload, InputCapture))
        && TestTrue(*FString::Printf(TEXT("add_metasound_input succeeds (error was '%s': %s)"),
            *InputCapture.ErrorCode, *InputCapture.Message), InputCapture.bSuccess)
        && InputCapture.Result.IsValid())
    {
        bool bInputSaved = false;
        InputCapture.Result->TryGetBoolField(TEXT("saved"), bInputSaved);
        TestTrue(TEXT("add_metasound_input save:true reports saved:true"), bInputSaved);

        // The load-bearing one: the EDIT is in the package bytes, not merely a file.
        TestTrue(TEXT("Saved .uasset names the input added with save:true"),
            MetaSoundPresetTestPrivate::PackageBytesContain(PackageFilename, InputName));
    }

    // --- 3. Negative control: add_metasound_output with save:false writes nothing ---
    const FString OutputName = FString::Printf(TEXT("PwUnsavedOutput_%s"), *Unique);
    TSharedPtr<FJsonObject> OutputPayload = MakeShared<FJsonObject>();
    OutputPayload->SetStringField(TEXT("assetPath"), PackageName);
    OutputPayload->SetStringField(TEXT("outputName"), OutputName);
    OutputPayload->SetStringField(TEXT("outputType"), TEXT("Audio"));
    OutputPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture OutputCapture;
    if (TestTrue(TEXT("add_metasound_output handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_output"), OutputPayload, OutputCapture))
        && TestTrue(*FString::Printf(TEXT("add_metasound_output succeeds (error was '%s': %s)"),
            *OutputCapture.ErrorCode, *OutputCapture.Message), OutputCapture.bSuccess)
        && OutputCapture.Result.IsValid())
    {
        bool bOutputSaved = true;
        OutputCapture.Result->TryGetBoolField(TEXT("saved"), bOutputSaved);
        TestFalse(TEXT("add_metasound_output save:false reports saved:false"), bOutputSaved);
        bool bOutputSaveRequested = true;
        OutputCapture.Result->TryGetBoolField(TEXT("saveRequested"), bOutputSaveRequested);
        TestFalse(TEXT("add_metasound_output save:false echoes saveRequested:false"), bOutputSaveRequested);
        // pendingFlush means "asked for and not durable"; nothing was asked for here.
        TestFalse(TEXT("add_metasound_output save:false does not report pendingFlush"),
            OutputCapture.Result->HasField(TEXT("pendingFlush")));

        TestFalse(TEXT("Saved .uasset does NOT name the output added with save:false"),
            MetaSoundPresetTestPrivate::PackageBytesContain(PackageFilename, OutputName));
    }

    return true;
}

#endif // __has_include("MetasoundSource.h")
#endif // __has_include("MetasoundFactory.h")
