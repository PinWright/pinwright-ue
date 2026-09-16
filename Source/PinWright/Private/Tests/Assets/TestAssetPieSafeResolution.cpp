// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Templates/UnrealTemplate.h"

#include "Handlers/ErrorCodes.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetDeletePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/PieState.h"

namespace AssetPieSafeResolutionTests
{
    struct FAllowedMutationSite
    {
        const TCHAR* RelativePath;
        const TCHAR* ScopeMarker;
        const TCHAR* Call;
        int32 ExpectedCount;
        const TCHAR* GatePath;
        const TCHAR* GateScopeMarker;
        const TCHAR* Gate;
        const TCHAR* RefusalMarker;
        const TCHAR* GateBeforeMarker;
    };

    struct FPolicyCallerGate
    {
        const TCHAR* RelativePath;
        const TCHAR* ScopeMarker;
        const TCHAR* Gate;
        const TCHAR* RefusalMarker;
        const TCHAR* CallMarker;
    };

    struct FScannedSource
    {
        FString RelativePath;
        FString Source;
    };

    FString ResolvePrivateRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid()
            ? (Plugin->GetBaseDir() / TEXT("Source/PinWright/Private"))
            : FString();
    }

    FString MakePrivateRelativePath(const FString& FilePath, const FString& PrivateRoot)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(FilePath);
        const FString FullRoot = FPaths::ConvertRelativePathToFull(PrivateRoot) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullRoot);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    int32 CountOccurrences(const FString& Source, const TCHAR* Needle)
    {
        const FString NeedleString(Needle);
        int32 Count = 0;
        int32 SearchStart = 0;
        while (true)
        {
            const int32 Match = Source.Find(NeedleString, ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchStart);
            if (Match == INDEX_NONE)
            {
                return Count;
            }
            ++Count;
            SearchStart = Match + NeedleString.Len();
        }
    }

    int32 FindOpeningBrace(const FString& Source, int32 SearchStart)
    {
        bool bInLiteral = false;
        TCHAR LiteralQuote = 0;
        for (int32 Index = SearchStart; Index < Source.Len(); ++Index)
        {
            const TCHAR Character = Source[Index];
            if (bInLiteral)
            {
                if (Character == TEXT('\\') && Index + 1 < Source.Len())
                {
                    ++Index;
                }
                else if (Character == LiteralQuote)
                {
                    bInLiteral = false;
                }
                continue;
            }

            if (Character == TEXT('"') || Character == TEXT('\''))
            {
                bInLiteral = true;
                LiteralQuote = Character;
                continue;
            }
            if (Character == TEXT('{'))
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    FString ExtractBraceScope(const FString& Source, const TCHAR* ScopeMarker)
    {
        if (!ScopeMarker || *ScopeMarker == TEXT('\0'))
        {
            return FString();
        }

        const FString Marker(ScopeMarker);
        const int32 MarkerStart = Source.Find(
            Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        if (MarkerStart == INDEX_NONE)
        {
            return FString();
        }

        const int32 OpeningBrace = FindOpeningBrace(Source, MarkerStart + Marker.Len());
        if (OpeningBrace == INDEX_NONE)
        {
            return FString();
        }

        int32 BraceDepth = 0;
        bool bInLiteral = false;
        TCHAR LiteralQuote = 0;
        for (int32 Index = OpeningBrace; Index < Source.Len(); ++Index)
        {
            const TCHAR Character = Source[Index];
            if (bInLiteral)
            {
                if (Character == TEXT('\\') && Index + 1 < Source.Len())
                {
                    ++Index;
                }
                else if (Character == LiteralQuote)
                {
                    bInLiteral = false;
                }
                continue;
            }

            if (Character == TEXT('"') || Character == TEXT('\''))
            {
                bInLiteral = true;
                LiteralQuote = Character;
            }
            else if (Character == TEXT('{'))
            {
                ++BraceDepth;
            }
            else if (Character == TEXT('}'))
            {
                --BraceDepth;
                if (BraceDepth == 0)
                {
                    return Source.Mid(OpeningBrace, Index - OpeningBrace + 1);
                }
            }
        }
        return FString();
    }

    bool GateTextuallyPrecedesFirstMutation(const FString& Scope,
                                            const TCHAR* Mutation,
                                            const TCHAR* Gate,
                                            const TCHAR* RefusalMarker)
    {
        if (!Mutation || !Gate)
        {
            return false;
        }

        const int32 MutationIndex = Scope.Find(
            Mutation, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        const int32 GateIndex = Scope.Find(
            Gate, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        if (MutationIndex == INDEX_NONE || GateIndex == INDEX_NONE || GateIndex > MutationIndex)
        {
            return false;
        }

        // Shared gate helpers own their refusal marker outside the caller scope.
        if (!RefusalMarker)
        {
            return true;
        }

        const int32 RefusalIndex = Scope.Find(
            RefusalMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return RefusalIndex != INDEX_NONE && GateIndex <= RefusalIndex &&
            RefusalIndex <= MutationIndex;
    }

    bool MarkersPrecede(const FString& Scope,
                        const TCHAR* Gate,
                        const TCHAR* RefusalMarker,
                        const TCHAR* BeforeMarker)
    {
        if (!Gate || !RefusalMarker || !BeforeMarker)
        {
            return false;
        }

        const int32 GateIndex = Scope.Find(
            Gate, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        const int32 RefusalIndex = Scope.Find(
            RefusalMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        const int32 BeforeIndex = Scope.Find(
            BeforeMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return GateIndex != INDEX_NONE && RefusalIndex != INDEX_NONE &&
            BeforeIndex != INDEX_NONE && GateIndex <= RefusalIndex &&
            RefusalIndex <= BeforeIndex;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetResolverRegistryAndLoadTest,
    "PinWright.Assets.AssetResolution.RegistryAndLoadAvoidEditorAssetLibrary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetResolverRegistryAndLoadTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
    const FString ObjectPath = PackagePath + TEXT(".DefaultMaterial");

    const FResolvedAsset RegistryResult = ResolveAsset(PackagePath);
    TestTrue(TEXT("Package-form lookup resolves from the registry"), RegistryResult.bExists);
    TestTrue(TEXT("Registry lookup identifies the named package"),
             RegistryResult.PackageName == FName(*PackagePath));

    const FResolvedAsset LoadedResult = ResolveAsset(ObjectPath, /*bLoadObject=*/true);
    TestTrue(TEXT("Object-form lookup resolves"), LoadedResult.bExists);
    TestNotNull(TEXT("Resolver loads the object without EditorAssetLibrary"), LoadedResult.Object);
    TestTrue(TEXT("Loaded object keeps the canonical object path"),
             LoadedResult.ObjectPath.ToString() == ObjectPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHandlersAvoidPieBlockedReadsTest,
    "PinWright.Assets.AssetResolution.AssetHandlersAvoidPieBlockedLibraryReads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetHandlersAvoidPieBlockedReadsTest::RunTest(const FString& Parameters)
{
    using namespace AssetPieSafeResolutionTests;

    const FString PrivateRoot = ResolvePrivateRoot();
    if (!TestFalse(TEXT("Resolved PinWright private source root"), PrivateRoot.IsEmpty()))
    {
        return false;
    }

    const TCHAR* const ForbiddenCalls[] = {
        TEXT("UEditorAssetLibrary::DoesAssetExist("),
        TEXT("UEditorAssetLibrary::LoadAsset("),
        TEXT("UEditorAssetLibrary::DoesDirectoryExist("),
        TEXT("UEditorAssetLibrary::ListAssets("),
        TEXT("UEditorAssetLibrary::FindAssetData("),
        TEXT("UEditorAssetLibrary::GetMetadataTag("),
        TEXT("UEditorAssetLibrary::SetMetadataTag("),
    };

    const TCHAR* const MutationCalls[] = {
        TEXT("UEditorAssetLibrary::MakeDirectory("),
        TEXT("UEditorAssetLibrary::DuplicateAsset("),
        TEXT("UEditorAssetLibrary::RenameAsset("),
        TEXT("UEditorAssetLibrary::DeleteAsset("),
        TEXT("UEditorAssetLibrary::DeleteDirectory("),
        TEXT("UEditorAssetLibrary::SaveAsset("),
        TEXT("UEditorAssetLibrary::SaveLoadedAsset("),
    };

    const FString HandlersRoot = PrivateRoot / TEXT("Handlers");
    if (!TestTrue(TEXT("Handler source tree exists on disk"),
                  IFileManager::Get().DirectoryExists(*HandlersRoot)))
    {
        return false;
    }

    TArray<FString> SourceFiles;
    IFileManager::Get().FindFilesRecursive(SourceFiles, *HandlersRoot, TEXT("*.cpp"),
                                           true, false, false);
    IFileManager::Get().FindFilesRecursive(SourceFiles, *HandlersRoot, TEXT("*.h"),
                                           true, false, false);
    SourceFiles.AddUnique(PrivateRoot / TEXT("Utils/AssetUtils.cpp"));
    SourceFiles.AddUnique(PrivateRoot / TEXT("Utils/AssetDeletePolicy.cpp"));
    TestTrue(TEXT("Recursive scan found handler/policy source files"), SourceFiles.Num() > 0);

    TArray<FScannedSource> ScannedSources;
    for (const FString& FilePath : SourceFiles)
    {
        const FString RelativePath = MakePrivateRelativePath(FilePath, PrivateRoot);
        FString RawSource;
        if (!TestTrue(*FString::Printf(TEXT("Read %s"), *RelativePath),
                      FFileHelper::LoadFileToString(RawSource, *FilePath)))
        {
            continue;
        }

        FScannedSource& Scanned = ScannedSources.Emplace_GetRef();
        Scanned.RelativePath = RelativePath;
        Scanned.Source = NeutralizeSourceText(RawSource);
        for (const TCHAR* ForbiddenCall : ForbiddenCalls)
        {
            TestFalse(*FString::Printf(TEXT("%s avoids PIE-blocked call %s"),
                                      *RelativePath, ForbiddenCall),
                      Scanned.Source.Contains(ForbiddenCall, ESearchCase::CaseSensitive));
        }
    }

    const FScannedSource* AssetManageSource = ScannedSources.FindByPredicate(
        [](const FScannedSource& Scanned)
        {
            return Scanned.RelativePath.Equals(
                TEXT("Handlers/Asset/AssetManageHandler.cpp"), ESearchCase::IgnoreCase);
        });
    TestNotNull(TEXT("Asset mutation gate helper source is present"), AssetManageSource);
    if (AssetManageSource)
    {
        const FString GateHelperScope = ExtractBraceScope(
            AssetManageSource->Source, TEXT("static bool RefuseAssetMutationDuringPie("));
        TestFalse(TEXT("Asset mutation gate helper scope is present"), GateHelperScope.IsEmpty());
        TestTrue(TEXT("Asset mutation gate helper returns PIE_ACTIVE before allowing callers to mutate"),
            MarkersPrecede(GateHelperScope,
                TEXT("PinWrightPieState::IsPlayInEditorActive()"),
                TEXT("ErrorCodes::ERR_PIE_ACTIVE"), TEXT("return true;")));
    }

    const AssetPieSafeResolutionTests::FAllowedMutationSite AllowedMutations[] = {
        {TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.create_animation_asset\", \"animation\","),
         TEXT("UEditorAssetLibrary::MakeDirectory("), 1,
         TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.create_animation_asset\", \"animation\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::MakeDirectory(")},
        {TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.setup_retargeting\", \"animation\","),
         TEXT("UEditorAssetLibrary::MakeDirectory("), 1,
         TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.setup_retargeting\", \"animation\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::MakeDirectory(")},
        {TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.setup_retargeting\", \"animation\","),
         TEXT("UEditorAssetLibrary::DuplicateAsset("), 1,
         TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.setup_retargeting\", \"animation\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::DuplicateAsset(")},
        {TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.setup_retargeting\", \"animation\","),
         TEXT("UEditorAssetLibrary::DeleteAsset("), 6,
         TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.setup_retargeting\", \"animation\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::DeleteAsset(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.duplicate\", \"asset\","),
         TEXT("UEditorAssetLibrary::MakeDirectory("), 1,
         TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.duplicate\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.duplicate\"))"),
         nullptr,
         TEXT("UEditorAssetLibrary::MakeDirectory(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.duplicate\", \"asset\","),
         TEXT("UEditorAssetLibrary::DuplicateAsset("), 2,
         TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.duplicate\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.duplicate\"))"),
         nullptr,
         TEXT("UEditorAssetLibrary::DuplicateAsset(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.rename\", \"asset\","),
         TEXT("UEditorAssetLibrary::RenameAsset("), 1,
         TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.rename\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.rename\"))"),
         nullptr,
         TEXT("UEditorAssetLibrary::RenameAsset(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.move\", \"asset\","),
         TEXT("UEditorAssetLibrary::RenameAsset("), 1,
         TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.move\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.move\"))"),
         nullptr,
         TEXT("UEditorAssetLibrary::RenameAsset(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.create_folder\", \"asset\","),
         TEXT("UEditorAssetLibrary::MakeDirectory("), 1,
         TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.create_folder\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.create_folder\"))"),
         nullptr,
         TEXT("UEditorAssetLibrary::MakeDirectory(")},
        {TEXT("Handlers/Blueprint/BlueprintTypeDefinitionHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"blueprint.create_enum\", \"blueprint\","),
         TEXT("UEditorAssetLibrary::DeleteAsset("), 2,
         TEXT("Handlers/Blueprint/BlueprintTypeDefinitionHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"blueprint.create_enum\", \"blueprint\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("TEXT(\"PIE_ACTIVE\")"),
         TEXT("UEditorAssetLibrary::DeleteAsset(")},
        {TEXT("Handlers/Editor/EditorCommandHandler.cpp"),
         TEXT("SaveDirtyPackagesWithIntegrityGate("),
         TEXT("UEditorAssetLibrary::SaveAsset("), 1,
         TEXT("Handlers/Editor/EditorCommandHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"editor.save_all\", \"editor\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("EditorSaveAllDiagnostic::SaveDirtyPackagesWithIntegrityGate(")},
        {TEXT("Handlers/Level/LevelHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"level.rename\", \"level\","),
         TEXT("UEditorAssetLibrary::RenameAsset("), 1,
         TEXT("Handlers/Level/LevelHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"level.rename\", \"level\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::RenameAsset(")},
        {TEXT("Handlers/Level/LevelHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"level.duplicate\", \"level\","),
         TEXT("UEditorAssetLibrary::DuplicateAsset("), 1,
         TEXT("Handlers/Level/LevelHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"level.duplicate\", \"level\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::DuplicateAsset(")},
        {TEXT("Handlers/Physics/PhysicsHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"physics.setup_physics_simulation\", \"physics\","),
         TEXT("UEditorAssetLibrary::MakeDirectory("), 1,
         TEXT("Handlers/Physics/PhysicsHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"physics.setup_physics_simulation\", \"physics\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("UEditorAssetLibrary::MakeDirectory(")},
        {TEXT("Utils/AssetUtils.cpp"),
         TEXT("ESaveLoadedAssetOutcome SaveLoadedAssetThrottled("),
         TEXT("UEditorAssetLibrary::SaveLoadedAsset("), 1,
         TEXT("Utils/AssetUtils.cpp"),
         TEXT("ESaveLoadedAssetOutcome SaveLoadedAssetThrottled("),
         TEXT("PinWrightPieSaveBlock::ProbePieSaveBlock(PieBlock)"),
         TEXT("ESaveLoadedAssetOutcome::RefusedBlockedByPie"),
         TEXT("UEditorAssetLibrary::SaveLoadedAsset(")},
        {TEXT("Utils/AssetDeletePolicy.cpp"),
         TEXT("AssetDeletePolicy::FResult AssetDeletePolicy::DeleteAsset("),
         TEXT("UEditorAssetLibrary::DeleteAsset("), 1,
         nullptr, nullptr, nullptr, nullptr, nullptr},
        {TEXT("Utils/AssetDeletePolicy.cpp"),
         TEXT("AssetDeletePolicy::FResult AssetDeletePolicy::DeleteDirectory("),
         TEXT("UEditorAssetLibrary::DeleteDirectory("), 2,
         nullptr, nullptr, nullptr, nullptr, nullptr},
    };

    for (const FScannedSource& Scanned : ScannedSources)
    {
        for (const TCHAR* MutationCall : MutationCalls)
        {
            int32 AllowedCount = 0;
            for (const FAllowedMutationSite& Allowed : AllowedMutations)
            {
                if (Scanned.RelativePath.Equals(Allowed.RelativePath,
                                                ESearchCase::IgnoreCase) &&
                    FCString::Strcmp(MutationCall, Allowed.Call) == 0)
                {
                    AllowedCount += Allowed.ExpectedCount;
                }
            }
            TestEqual(*FString::Printf(TEXT("%s has only allowlisted mutation %s"),
                                       *Scanned.RelativePath, MutationCall),
                      CountOccurrences(Scanned.Source, MutationCall), AllowedCount);
        }
    }

    for (const FAllowedMutationSite& Allowed : AllowedMutations)
    {
        const FScannedSource* MutationSource = nullptr;
        const FScannedSource* GateSource = nullptr;
        for (const FScannedSource& Scanned : ScannedSources)
        {
            if (Scanned.RelativePath.Equals(Allowed.RelativePath, ESearchCase::IgnoreCase))
            {
                MutationSource = &Scanned;
            }
            if (Allowed.GatePath &&
                Scanned.RelativePath.Equals(Allowed.GatePath, ESearchCase::IgnoreCase))
            {
                GateSource = &Scanned;
            }
        }

        TestNotNull(*FString::Printf(TEXT("Allowlisted mutation source %s is present"),
                                    Allowed.RelativePath), MutationSource);
        if (!MutationSource)
        {
            continue;
        }

        const FString MutationScope = ExtractBraceScope(
            MutationSource->Source, Allowed.ScopeMarker);
        TestFalse(*FString::Printf(TEXT("%s mutation scope is present"), Allowed.ScopeMarker),
                  MutationScope.IsEmpty());
        if (MutationScope.IsEmpty())
        {
            continue;
        }

        TestEqual(*FString::Printf(TEXT("%s mutation count %s"),
                                   Allowed.ScopeMarker, Allowed.Call),
                  CountOccurrences(MutationScope, Allowed.Call),
                  Allowed.ExpectedCount);

        if (!Allowed.GatePath)
        {
            continue;
        }

        TestNotNull(*FString::Printf(TEXT("Mutation gate source %s is present"),
                                    Allowed.GatePath), GateSource);
        if (!GateSource)
        {
            continue;
        }

        const FString GateScope = ExtractBraceScope(
            GateSource->Source, Allowed.GateScopeMarker);
        TestFalse(*FString::Printf(TEXT("%s gate scope is present"), Allowed.GateScopeMarker),
                  GateScope.IsEmpty());
        if (GateScope.IsEmpty())
        {
            continue;
        }

        if (FCString::Strcmp(Allowed.ScopeMarker, Allowed.GateScopeMarker) == 0)
        {
            TestTrue(*FString::Printf(TEXT("%s PIE gate textually precedes first mutation"),
                                      Allowed.ScopeMarker),
                     GateTextuallyPrecedesFirstMutation(
                         MutationScope, Allowed.Call, Allowed.Gate, Allowed.RefusalMarker));
        }
        else
        {
            TestTrue(*FString::Printf(TEXT("%s gate precedes the save helper"),
                                      Allowed.GateScopeMarker),
                     MarkersPrecede(
                         GateScope, Allowed.Gate, Allowed.RefusalMarker,
                         Allowed.GateBeforeMarker));
        }
    }

    const AssetPieSafeResolutionTests::FPolicyCallerGate PolicyCallerGates[] = {
        {TEXT("Handlers/Animation/AnimationHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"animation.cleanup\", \"animation\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("AssetDeletePolicy::DeleteAsset(")},
        {TEXT("Handlers/Level/LevelHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"level.delete\", \"level\","),
         TEXT("PinWrightPieState::IsPlayInEditorActive()"),
         TEXT("ErrorCodes::ERR_PIE_ACTIVE"),
         TEXT("AssetDeletePolicy::DeleteAsset(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.delete\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.delete\"))"),
         nullptr,
         TEXT("AssetDeletePolicy::DeleteAsset(")},
        {TEXT("Handlers/Asset/AssetManageHandler.cpp"),
         TEXT("REGISTER_RPC_HANDLER(\"asset.delete\", \"asset\","),
         TEXT("RefuseAssetMutationDuringPie(Ctx, TEXT(\"asset.delete\"))"),
         nullptr,
         TEXT("AssetDeletePolicy::DeleteDirectory(")},
    };
    for (const AssetPieSafeResolutionTests::FPolicyCallerGate& PolicyGate : PolicyCallerGates)
    {
        const FScannedSource* CallerSource = nullptr;
        for (const FScannedSource& Scanned : ScannedSources)
        {
            if (Scanned.RelativePath.Equals(PolicyGate.RelativePath,
                                             ESearchCase::IgnoreCase))
            {
                CallerSource = &Scanned;
                break;
            }
        }
        TestNotNull(*FString::Printf(TEXT("Asset policy caller source %s is present"),
                                    PolicyGate.RelativePath), CallerSource);
        if (CallerSource)
        {
            const FString CallerScope = ExtractBraceScope(
                CallerSource->Source, PolicyGate.ScopeMarker);
            TestFalse(*FString::Printf(TEXT("Asset policy caller scope %s is present"),
                                       PolicyGate.ScopeMarker),
                      CallerScope.IsEmpty());
            if (!CallerScope.IsEmpty())
            {
                TestEqual(*FString::Printf(TEXT("Asset policy call count %s"),
                                           PolicyGate.CallMarker),
                          CountOccurrences(CallerScope, PolicyGate.CallMarker), 1);
                TestTrue(*FString::Printf(TEXT("Asset policy PIE gate textually precedes call %s"),
                                          PolicyGate.ScopeMarker),
                         GateTextuallyPrecedesFirstMutation(CallerScope,
                             PolicyGate.CallMarker, PolicyGate.Gate, PolicyGate.RefusalMarker));
            }
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentsTransientPieReadTest,
    "PinWright.Assets.AssetResolution.ActorGetComponentsReadsTransientAssetInPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentsTransientPieReadTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = CompilerTestUtils::CreateTransientTestBP(TEXT("AssetPieActorRead"));
    if (!TestNotNull(TEXT("transient actor Blueprint created"), Blueprint))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Blueprint->GetPathName());

    TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
    TestTrue(TEXT("simulated PIE gate is active"), PinWrightPieState::IsPlayInEditorActive());

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.get_components"),
        TEXT("asset-pie-actor-read"), Payload, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("actor.get_components completed through the dispatcher"), Sink->bWasCalled);
    TestTrue(TEXT("actor.get_components succeeds for a transient Blueprint in PIE"), bSuccess);
    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    TestTrue(TEXT("actor.get_components returned its components array"),
        Result.IsValid() && Result->TryGetArrayField(TEXT("components"), Components));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerGetBindingsTransientPieReadTest,
    "PinWright.Assets.AssetResolution.SequencerGetBindingsReadsTransientAssetInPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerGetBindingsTransientPieReadTest::RunTest(const FString& Parameters)
{
    FString SequencePath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so it
    // cannot answer a later /Engine/Transient asset-registry rescan. Declared before the PIE
    // guard below so the simulated PIE flag is already restored when it runs.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SequencePath);
    };

    ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(
        TEXT("AssetPieSequencerRead"), SequencePath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), SequencePath);

    TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
    TestTrue(TEXT("simulated PIE gate is active"), PinWrightPieState::IsPlayInEditorActive());

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("sequencer.get_bindings"),
        TEXT("asset-pie-sequencer-read"), Payload, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("sequencer.get_bindings completed through the dispatcher"), Sink->bWasCalled);
    TestTrue(TEXT("sequencer.get_bindings succeeds for a transient sequence in PIE"), bSuccess);
    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    TestTrue(TEXT("sequencer.get_bindings returned its bindings array"),
        Result.IsValid() && Result->TryGetArrayField(TEXT("bindings"), Bindings));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeletePolicyTransientPieGateTest,
    "PinWright.Assets.AssetResolution.AssetDeletePolicyResolvesAndDeleteIsPieGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeletePolicyTransientPieGateTest::RunTest(const FString& Parameters)
{
    FString PolicyPath;
    FString DeletePath;
    // Both fixtures are RF_Standalone, so the periodic suite GC keeps them alive; detach them so
    // they cannot answer a later /Engine/Transient asset-registry rescan. Declared before the PIE
    // guard below so the simulated PIE flag is already restored when it runs. A path the test
    // already deleted is a no-op here.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PolicyPath);
        CleanupTestAsset(DeletePath);
    };

    ULevelSequence* PolicyTarget = SequencerTestFixtures::MakeTransientSequence(
        TEXT("AssetPieDeletePolicy"), PolicyPath);
    ULevelSequence* DeleteTarget = SequencerTestFixtures::MakeTransientSequence(
        TEXT("AssetPieDeleteGate"), DeletePath);
    if (!TestNotNull(TEXT("transient policy sequence created"), PolicyTarget) ||
        !TestNotNull(TEXT("transient delete sequence created"), DeleteTarget))
    {
        return true;
    }

    TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
    TestTrue(TEXT("simulated PIE gate is active"), PinWrightPieState::IsPlayInEditorActive());

    const FResolvedAsset ResolvedPolicyTarget = ResolveAsset(PolicyPath, /*bLoadObject=*/true);
    TestTrue(TEXT("AssetDeletePolicy input resolves through the shared resolver"),
        ResolvedPolicyTarget.bExists && ResolvedPolicyTarget.Object == PolicyTarget);

    const AssetDeletePolicy::FResult SafePolicyResult =
        AssetDeletePolicy::DeleteAsset(PolicyPath, /*bForce=*/false);
    TestFalse(TEXT("non-force AssetDeletePolicy path does not enter the force/library path"),
        SafePolicyResult.bForced);
    TestTrue(TEXT("non-force AssetDeletePolicy produced a resolved terminal result"),
        SafePolicyResult.bDeleted || SafePolicyResult.bRefused ||
        !SafePolicyResult.Message.IsEmpty());

    TSharedPtr<FJsonObject> DeletePayload = MakeShared<FJsonObject>();
    DeletePayload->SetStringField(TEXT("path"), DeletePath);
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
    bool bSuccess = true;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("asset.delete"),
        TEXT("asset-pie-delete"), DeletePayload, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("asset.delete completed through the dispatcher"), Sink->bWasCalled);
    TestFalse(TEXT("asset.delete is refused during PIE"), bSuccess);
    TestEqual(TEXT("asset.delete reports PIE_ACTIVE"), ErrorCode,
        FString(ErrorCodes::ERR_PIE_ACTIVE));
    TestTrue(TEXT("PIE refusal leaves the target asset alive"),
        ResolveAsset(DeletePath, /*bLoadObject=*/true).Object == DeleteTarget);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiePredicateMatchesEditorGateTest,
    "PinWright.Assets.AssetResolution.PiePredicateMirrorsEngineGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPiePredicateMatchesEditorGateTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("Predicate is false before the PIE-world flag is set"),
              PinWrightPieState::IsPlayInEditorActive());

    {
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        TestTrue(TEXT("Predicate follows the PIE-world flag"),
                 PinWrightPieState::IsPlayInEditorActive());
    }

    TestFalse(TEXT("Predicate is false after the PIE-world flag is restored"),
              PinWrightPieState::IsPlayInEditorActive());
    return true;
}
