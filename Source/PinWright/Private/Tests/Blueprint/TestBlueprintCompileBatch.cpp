// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.compile_batch (board F-blueprint-compile-batch).
//
// Fixture: one folder holding a broken, a clean and a live-instanced Blueprint. The batch must
// return one row per Blueprint with the right outcome, totals that sum, one progress event per
// Blueprint, node-located errors, no package save, and NO_ASSETS_MATCHED for an empty match.
// Counterfactuals: drop the per-row guard and the live Blueprint compiles (outcome flips);
// drop the message-link read and nodeGuid disappears; drop RecordProgress and the count is 0.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/Bpir/BpirGraphTestHelpers.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace BlueprintCompileBatchTest
{
    UBlueprint* CreateActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        if (Blueprint)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
            FAssetRegistryModule::AssetCreated(Blueprint);
        }
        return Blueprint;
    }

    // A reachable call node whose function reference names nothing: a node-level compile error.
    UK2Node_CallFunction* PoisonBlueprint(UBlueprint* Blueprint)
    {
        UEdGraph* EventGraph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
        if (!EventGraph)
        {
            return nullptr;
        }
        UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
        UK2Node_CallFunction* BadCall = CompilerTestUtils::SpawnPrintStringCall(EventGraph, 320, 0);
        if (!BeginPlay || !BadCall)
        {
            return nullptr;
        }
        BpirGraphTestHelpers::WireExec(BeginPlay, BadCall);
        BadCall->FunctionReference.SetExternalMember(
            FName(TEXT("PW_CompileBatch_NoSuchFunction")), AActor::StaticClass());
        return BadCall;
    }

    // Runs the verb and returns the finished job's result (null on any failure, recorded).
    TSharedPtr<FJsonObject> RunBatch(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
                                     FString& OutTicketId)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("blueprint.compile_batch registered"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_batch"), Payload, Capture));
        if (!Test.TestTrue(TEXT("batch accepted"), Capture.bSuccess) || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
            return nullptr;
        }
        Capture.Result->TryGetStringField(TEXT("ticket_id"), OutTicketId);
        FJobTicket Ticket;
        if (!Test.TestTrue(TEXT("job ticket exists"),
                FPluginState::Get().GetJobRegistry().Get(OutTicketId, Ticket)))
        {
            return nullptr;
        }
        Test.TestEqual(TEXT("job completed"), Ticket.Status, FString(TEXT("completed")));
        return Ticket.Result;
    }

    TSharedPtr<FJsonObject> FindRow(const TSharedPtr<FJsonObject>& Result, const FString& ObjectPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(TEXT("blueprints"), Rows))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Rows)
            {
                const TSharedPtr<FJsonObject> Row = Value->AsObject();
                if (Row.IsValid() && Row->GetStringField(TEXT("path")) == ObjectPath)
                {
                    return Row;
                }
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileBatchFolderRowsTest,
    "PinWright.blueprint.compile_batch.FolderReportsBrokenCleanAndLiveRows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintCompileBatchFolderRowsTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintCompileBatchTest;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world available"), World))
    {
        return false;
    }
    FScopedEditorWorldActorGuard WorldGuard;

    const FString Folder = FString::Printf(TEXT("/Game/PinWrightTests/__PW_CompileBatch_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString BrokenPath = Folder + TEXT("/BP_Broken");
    const FString CleanPath = Folder + TEXT("/BP_Clean");
    const FString LivePath = Folder + TEXT("/BP_Live");

    AActor* Instance = nullptr;
    int32 PackagesSaved = 0;
    const FDelegateHandle SaveHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
        [&PackagesSaved](const FString&, UPackage*, FObjectPostSaveContext) { ++PackagesSaved; });
    TMap<FString, int32> ProgressByTicket;
    const FDelegateHandle JobHandle = FPluginState::Get().GetJobRegistry().OnJobEvent().AddLambda(
        [&ProgressByTicket](const FString& TicketId, const FString& Event,
                            const TSharedPtr<FJsonObject>&, const TSharedPtr<FJsonObject>&)
        {
            if (Event == TEXT("progress")) { ++ProgressByTicket.FindOrAdd(TicketId); }
        });
    ON_SCOPE_EXIT
    {
        UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
        FPluginState::Get().GetJobRegistry().OnJobEvent().Remove(JobHandle);
        if (IsValid(Instance)) { Instance->Destroy(); }
        for (const FString& Path : {BrokenPath, CleanPath, LivePath})
        {
            if (UPackage* Package = FindPackage(nullptr, *Path)) { Package->SetDirtyFlag(false); }
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(Path));
        }
    };

    UBlueprint* Broken = CreateActorBlueprint(BrokenPath);
    UBlueprint* Clean = CreateActorBlueprint(CleanPath);
    UBlueprint* Live = CreateActorBlueprint(LivePath);
    if (!TestNotNull(TEXT("broken fixture"), Broken) || !TestNotNull(TEXT("clean fixture"), Clean)
        || !TestNotNull(TEXT("live fixture"), Live))
    {
        return false;
    }
    UK2Node_CallFunction* BadCall = PoisonBlueprint(Broken);
    if (!TestNotNull(TEXT("broken fixture has its bad call node"), BadCall))
    {
        return false;
    }
    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags = RF_Transient;
    Instance = World->SpawnActor<AActor>(Live->GeneratedClass, FVector::ZeroVector,
                                         FRotator::ZeroRotator, SpawnParams);
    if (!TestNotNull(TEXT("live fixture instance spawned"), Instance))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("folder"), Folder);
    FString TicketId;
    const TSharedPtr<FJsonObject> Result = RunBatch(*this, Payload, TicketId);
    if (!TestNotNull(TEXT("batch result"), Result.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Summary = Result->GetObjectField(TEXT("summary"));
    const int32 Matched = Summary->GetIntegerField(TEXT("matched"));
    TestEqual(TEXT("three Blueprints matched"), Matched, 3);
    TestEqual(TEXT("compiled + failed + refused + unloadable == matched"),
        Summary->GetIntegerField(TEXT("compiled")) + Summary->GetIntegerField(TEXT("failed"))
            + Summary->GetIntegerField(TEXT("refused")) + Summary->GetIntegerField(TEXT("unloadable")),
        Matched);
    TestEqual(TEXT("one progress event per Blueprint"), ProgressByTicket.FindRef(TicketId), 3);
    TestEqual(TEXT("no package was saved"), PackagesSaved, 0);

    const TSharedPtr<FJsonObject> BrokenRow = FindRow(Result, ToObjectPath(BrokenPath));
    const TSharedPtr<FJsonObject> CleanRow = FindRow(Result, ToObjectPath(CleanPath));
    const TSharedPtr<FJsonObject> LiveRow = FindRow(Result, ToObjectPath(LivePath));
    if (!TestNotNull(TEXT("broken row"), BrokenRow.Get()) || !TestNotNull(TEXT("clean row"), CleanRow.Get())
        || !TestNotNull(TEXT("live row"), LiveRow.Get()))
    {
        return false;
    }
    TestEqual(TEXT("broken -> failed"), BrokenRow->GetStringField(TEXT("outcome")), FString(TEXT("failed")));
    TestEqual(TEXT("clean -> compiled"), CleanRow->GetStringField(TEXT("outcome")), FString(TEXT("compiled")));
    TestEqual(TEXT("live -> refused"), LiveRow->GetStringField(TEXT("outcome")), FString(TEXT("refused")));
    TestEqual(TEXT("refusal is coded"), LiveRow->GetStringField(TEXT("code")),
        FString(TEXT("LIVE_INSTANCES_WOULD_BE_REINSTANCED")));
    TestTrue(TEXT("refusal carries the reinstanced block"), LiveRow->HasField(TEXT("reinstanced")));
    TestTrue(TEXT("live instance survived the batch"), IsValid(Instance));

    // The node-level error points at the node the caller sees in the graph.
    bool bFoundNodeGuid = false;
    for (const TSharedPtr<FJsonValue>& Value : BrokenRow->GetArrayField(TEXT("errors")))
    {
        FString NodeGuid;
        if (Value->AsObject()->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
            && NodeGuid == BadCall->NodeGuid.ToString())
        {
            bFoundNodeGuid = true;
            TestEqual(TEXT("error names its graph"), Value->AsObject()->GetStringField(TEXT("graph")),
                BadCall->GetGraph()->GetName());
        }
    }
    TestTrue(TEXT("broken row error carries the bad node's nodeGuid"), bFoundNodeGuid);

    // onlyStatus=error picks exactly the Blueprint now loaded in BS_Error.
    Payload->SetStringField(TEXT("onlyStatus"), TEXT("error"));
    const TSharedPtr<FJsonObject> ErrorOnly = RunBatch(*this, Payload, TicketId);
    if (TestNotNull(TEXT("onlyStatus=error result"), ErrorOnly.Get()))
    {
        TestEqual(TEXT("onlyStatus=error matches the broken Blueprint only"),
            ErrorOnly->GetObjectField(TEXT("summary"))->GetIntegerField(TEXT("matched")), 1);
        const TSharedPtr<FJsonObject> Row = FindRow(ErrorOnly, ToObjectPath(BrokenPath));
        if (TestNotNull(TEXT("errored row present"), Row.Get()))
        {
            TestEqual(TEXT("statusBefore is Error"), Row->GetStringField(TEXT("statusBefore")),
                FString(TEXT("Error")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileBatchEmptyMatchTest,
    "PinWright.blueprint.compile_batch.EmptyMatchIsNoAssetsMatched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintCompileBatchEmptyMatchTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("folder"), FString::Printf(
        TEXT("/Game/PinWrightTests/__PW_CompileBatchEmpty_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.compile_batch registered"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_batch"), Payload, Capture));
    TestFalse(TEXT("an empty folder is not a zero-row success"), Capture.bSuccess);
    TestEqual(TEXT("error code"), Capture.ErrorCode, FString(TEXT("NO_ASSETS_MATCHED")));

    // Neither scope, or both, is an argument error rather than a guess.
    TArray<TSharedPtr<FJsonValue>> Assets;
    Assets.Add(MakeShared<FJsonValueString>(TEXT("/Game/X/BP_X")));
    Payload->SetArrayField(TEXT("assets"), Assets);
    InvokeHandlerWithCapture(TEXT("blueprint.compile_batch"), Payload, Capture);
    TestEqual(TEXT("folder + assets is INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}
