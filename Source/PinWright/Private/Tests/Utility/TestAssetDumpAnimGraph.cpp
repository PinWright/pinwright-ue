// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/TestSkipReporting.h"


#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"

// ============================================================================
// AssetDumpHandler.AnimBlueprint_EmitsAnimGraphJson
// Dumping a UAnimBlueprint must produce the anim_graph.json aspect with the
// pages / state_machines / anim_node_classes top-level keys.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpHandlerAnimBlueprintTest,
    "PinWright.asset.dump.AnimBlueprint_EmitsAnimGraphJson",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpHandlerAnimBlueprintTest::RunTest(const FString& Parameters)
{
    const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpHandlerTests")
        / FGuid::NewGuid().ToString();

    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    };

    // Host Mannequin AnimBP (ABP_Manny) — Lyra mannequin content that not every
    // host project ships. Where present it carries at least one state machine,
    // so positive assertions on state_machines are safe.
    const FString AssetPath =
        TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(AssetPath, TestRoot, /*bDiff=*/false);

    if (!Result.ErrorCode.IsEmpty())
    {
        // ASSET_FILE_MISSING comes from DumpSingleAsset's pre-LoadObject
        // DoesPackageExist check: the fixture package is absent from this host
        // project, so pass with the audit-greppable note. Every other error code
        // (a load/dump failure on a host that HAS the package) stays a HARD FAILURE.
        // AddWarning, not AddInfo: a skipped test still reports Success, so an Info
        // note is invisible in the run summary and the lost coverage reads as a pass.
        if (Result.ErrorCode == AssetDumpErrorCodes::AssetFileMissing)
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
                FString::Printf(
                    TEXT("FIXTURE-SKIP: %s not present in this host project (requires Lyra mannequin content); "
                         "skipping AnimBlueprint_EmitsAnimGraphJson."),
                    *AssetPath));
            return true;
        }
        AddError(FString::Printf(
            TEXT("AnimBlueprint_EmitsAnimGraphJson: required fixture not loadable (%s: %s)"),
            *Result.ErrorCode, *Result.ErrorMessage));
        return false;
    }

    // anim_graph.json must appear in WrittenPaths and exist on disk.
    FString AnimGraphPath;
    for (const FString& Path : Result.WrittenPaths)
    {
        if (Path.EndsWith(DumpFileNames::AnimGraph))
        {
            AnimGraphPath = Path;
            break;
        }
    }
    TestFalse(TEXT("anim_graph.json is in WrittenPaths"), AnimGraphPath.IsEmpty());
    if (AnimGraphPath.IsEmpty())
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("anim_graph.json exists on disk: %s"), *AnimGraphPath),
        IFileManager::Get().FileExists(*AnimGraphPath));

    // Parse and assert top-level shape.
    FString AnimGraphRaw;
    TestTrue(TEXT("anim_graph.json is readable"),
        FFileHelper::LoadFileToString(AnimGraphRaw, *AnimGraphPath));

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AnimGraphRaw);
    TestTrue(TEXT("anim_graph.json parses as JSON object"),
        FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());
    if (!Root.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* PagesArray = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* StateMachinesArray = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* AnimNodeClassesArray = nullptr;

    TestTrue(TEXT("anim_graph.json has 'pages' array"),
        Root->TryGetArrayField(TEXT("pages"), PagesArray));
    TestTrue(TEXT("anim_graph.json has 'state_machines' array"),
        Root->TryGetArrayField(TEXT("state_machines"), StateMachinesArray));
    TestTrue(TEXT("anim_graph.json has 'anim_node_classes' array"),
        Root->TryGetArrayField(TEXT("anim_node_classes"), AnimNodeClassesArray));

    if (PagesArray)
    {
        TestTrue(TEXT("'pages' is non-empty (root AnimGraph page always exists)"),
            PagesArray->Num() > 0);
    }

    if (AnimNodeClassesArray)
    {
        TestTrue(TEXT("'anim_node_classes' is non-empty"), AnimNodeClassesArray->Num() > 0);
        for (const TSharedPtr<FJsonValue>& Entry : *AnimNodeClassesArray)
        {
            const TSharedPtr<FJsonObject>* EntryObj = nullptr;
            if (Entry.IsValid() && Entry->TryGetObject(EntryObj) && EntryObj && (*EntryObj).IsValid())
            {
                FString ClassName;
                const bool bHasClass = (*EntryObj)->TryGetStringField(TEXT("class"), ClassName);
                TestTrue(TEXT("anim_node_classes entry has non-empty 'class' string"),
                    bHasClass && !ClassName.IsEmpty());
            }
            else
            {
                TestTrue(TEXT("anim_node_classes entry is a JSON object"), false);
            }
        }
    }

    if (StateMachinesArray)
    {
        // ABP_Manny is known to host at least one state machine.
        TestTrue(TEXT("'state_machines' is non-empty for Mannequin ABP_Manny"),
            StateMachinesArray->Num() > 0);
    }

    return true;
}
