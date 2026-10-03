// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestInstancedMeshAddRemove.cpp - actor.add_instances / actor.remove_instances.
//
// The verbs exist because the only route to fill or empty an ISM/HISM instance array was
// python.execute, and a clear-then-refill there lost a whole scatter when the refill raised after
// the clear had run. So the load-bearing assertions are the refusals: a bad row in a replace must
// leave the OLD scatter intact (a verb that cleared before validating fails it), a removal with
// one bad index must remove nothing, the removal record must replay through add_instances, and
// the call must be one undoable transaction.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "InstancedFoliageActor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace TestInstancedMeshAddRemoveHelpers
{
    // An isolated column of the editor world, away from the columns other spatial tests use.
    const FVector HolderLocation(412300.0, -287100.0, 500.0);
    constexpr double Spacing = 200.0;

    // An actor whose root is a HISM carrying Count cube instances along local X - the holder
    // shape actor.add_component produces. Built directly; the verbs under test are what fill it.
    UHierarchicalInstancedStaticMeshComponent* SpawnHolder(FAutomationTestBase& Test, UWorld* World,
                                                           const FString& Label, int32 Count)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        AActor* Holder = World && Cube
            ? World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator)
            : nullptr;
        if (!Holder)
        {
            Test.AddError(TEXT("holder fixture could not be built (no world, cube mesh or actor)"));
            return nullptr;
        }
        UHierarchicalInstancedStaticMeshComponent* Hism =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(Holder, TEXT("HISM_AddRemove"),
                RF_Transactional);
        Holder->SetRootComponent(Hism);
        Holder->AddInstanceComponent(Hism);
        Hism->OnComponentCreated();
        Hism->SetStaticMesh(Cube);
        Hism->RegisterComponent();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Hism->AddInstance(FTransform(FVector(Index * Spacing, 0.0, 0.0)));
        }
        Holder->SetActorLocation(HolderLocation);
        Holder->SetActorLabel(Label);
        Test.TestEqual(TEXT("fixture precondition: holder carries the seeded instances"),
            Hism->GetInstanceCount(), Count);
        return Hism;
    }

    FString Label(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWI_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FVector WorldLocation(UInstancedStaticMeshComponent* Component, int32 Index)
    {
        FTransform Transform;
        return Component->GetInstanceTransform(Index, Transform, /*bWorldSpace*/ true)
            ? Transform.GetLocation() : FVector(TNumericLimits<double>::Max());
    }

    TSharedPtr<FJsonObject> Vec(const FVector& V)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), V.X);
        Obj->SetNumberField(TEXT("y"), V.Y);
        Obj->SetNumberField(TEXT("z"), V.Z);
        return Obj;
    }

    TSharedPtr<FJsonValue> Row(const FVector& Location)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("location"), Vec(Location));
        return MakeShared<FJsonValueObject>(Obj);
    }

    TSharedPtr<FJsonObject> Payload(const FString& ActorLabel)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("actorName"), ActorLabel);
        return Obj;
    }

    FTestResponseCapture Call(const TCHAR* Method, const TSharedPtr<FJsonObject>& Args)
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(Method, Args, Capture);
        return Capture;
    }

    // Through the real dispatcher, for refusals the dispatcher owns (UNKNOWN_NESTED_PARAMS):
    // InvokeHandlerWithCapture calls the handler body directly and never runs those gates.
    FString DispatchErrorCode(const TCHAR* Method, const TSharedPtr<FJsonObject>& Args)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, TEXT("req-instanced-add-remove"),
            Args, bSuccess, ErrorCode);
        // A bare dispatcher has no ticker to drain a parked request; settle it here, the way
        // NestedParamKeyGateTests::DispatchAndSettle does, so a deferral cannot read as "".
        if (!Sink->bWasCalled)
        {
            Dispatcher.ProcessPendingRequests();
        }
        if (!Sink->bWasCalled)
        {
            return FString(TEXT("<no response>"));
        }
        return Sink->bSuccess ? FString(TEXT("<success>")) : Sink->ErrorCode;
    }

    double Number(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
    {
        double Value = -1.0;
        if (Obj.IsValid())
        {
            Obj->TryGetNumberField(Key, Value);
        }
        return Value;
    }

    // Location of the row of `ArrayKey` at Position, or a sentinel no test location equals.
    FVector RowLocation(const TSharedPtr<FJsonObject>& Result, const TCHAR* ArrayKey, int32 Position)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        const TSharedPtr<FJsonObject>* Location = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(ArrayKey, Rows) && Rows->IsValidIndex(Position)
            && (*Rows)[Position]->AsObject().IsValid()
            && (*Rows)[Position]->AsObject()->TryGetObjectField(TEXT("location"), Location))
        {
            return FVector(Number(*Location, TEXT("x")), Number(*Location, TEXT("y")),
                Number(*Location, TEXT("z")));
        }
        return FVector(TNumericLimits<double>::Max());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorAddInstancesTest,
    "PinWright.actor.add_instances.AppendsVerifiedAndRefusesBeforeClearing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorAddInstancesTest::RunTest(const FString& Parameters)
{
    using namespace TestInstancedMeshAddRemoveHelpers;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("no editor world; the actor.add_instances assertions were stepped over."));
        return true;
    }
    FScopedEditorWorldActorGuard Guard;
    const FString HolderLabel = Label(TEXT("Add"));
    UHierarchicalInstancedStaticMeshComponent* Hism = SpawnHolder(*this, World, HolderLabel, 3);
    if (!Hism)
    {
        return true;
    }
    const FVector Before0 = WorldLocation(Hism, 0);

    // ---- Append: indices reported, the component holds the requested pose ----
    const FVector NewA = HolderLocation + FVector(1000.0, 50.0, 25.0);
    TSharedPtr<FJsonObject> AppendArgs = Payload(HolderLabel);
    TSharedPtr<FJsonObject> ScaledRow = MakeShared<FJsonObject>();
    ScaledRow->SetObjectField(TEXT("location"), Vec(NewA));
    ScaledRow->SetObjectField(TEXT("scale"), Vec(FVector(2.0)));
    AppendArgs->SetArrayField(TEXT("transforms"),
        {MakeShared<FJsonValueObject>(ScaledRow), Row(NewA + FVector(200.0, 0.0, 0.0))});
    FTestResponseCapture Append = Call(TEXT("actor.add_instances"), AppendArgs);
    TestTrue(*FString::Printf(TEXT("append succeeds (%s: %s)"), *Append.ErrorCode, *Append.Message),
        Append.bSuccess);
    TestEqual(TEXT("added counts both rows"), Number(Append.Result, TEXT("added")), 2.0);
    TestEqual(TEXT("the component holds 5 instances"), Hism->GetInstanceCount(), 5);
    const TArray<TSharedPtr<FJsonValue>>* Indices = nullptr;
    if (TestTrue(TEXT("addedIndices reported"), Append.Result.IsValid()
            && Append.Result->TryGetArrayField(TEXT("addedIndices"), Indices) && Indices->Num() == 2))
    {
        TestEqual(TEXT("first new instance is index 3"), (*Indices)[0]->AsNumber(), 3.0);
    }
    TestTrue(TEXT("ground truth: instance 3 sits at the requested world location"),
        WorldLocation(Hism, 3).Equals(NewA, 0.01));
    FTransform Scaled;
    Hism->GetInstanceTransform(3, Scaled, true);
    TestTrue(TEXT("ground truth: instance 3 carries the requested scale"),
        Scaled.GetScale3D().Equals(FVector(2.0), 0.001));
    bool bSaved = true;
    TestTrue(TEXT("save report says marked dirty, not saved"), Append.Result.IsValid()
        && Append.Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);
    bool bUndoable = false;
    TestTrue(TEXT("a transactional component reports undoable:true"), Append.Result.IsValid()
        && Append.Result->TryGetBoolField(TEXT("undoable"), bUndoable) && bUndoable);
    TestFalse(TEXT("a plain append discards nothing, so it carries no warnings"),
        Append.Result.IsValid() && Append.Result->HasField(TEXT("warnings")));

    // ---- The data-loss shape: replace with a bad SECOND row must leave the old scatter intact ----
    TSharedPtr<FJsonObject> NoLocation = MakeShared<FJsonObject>();
    NoLocation->SetObjectField(TEXT("scale"), Vec(FVector(1.0)));
    TSharedPtr<FJsonObject> BadReplace = Payload(HolderLabel);
    BadReplace->SetBoolField(TEXT("replace"), true);
    BadReplace->SetArrayField(TEXT("transforms"), {Row(NewA), MakeShared<FJsonValueObject>(NoLocation)});
    FTestResponseCapture Refused = Call(TEXT("actor.add_instances"), BadReplace);
    TestFalse(TEXT("a replace with a row lacking location is refused"), Refused.bSuccess);
    TestEqual(TEXT("missing location -> MISSING_REQUIRED_PARAM"), Refused.ErrorCode,
        FString(TEXT("MISSING_REQUIRED_PARAM")));
    TestEqual(TEXT("the refused replace cleared nothing"), Hism->GetInstanceCount(), 5);
    TestTrue(TEXT("instance 0 is untouched by the refused replace"),
        WorldLocation(Hism, 0).Equals(Before0, 0.01));

    // A misspelled row key is refused rather than defaulted - by the dispatcher's nested-key gate.
    TSharedPtr<FJsonObject> Typo = MakeShared<FJsonObject>();
    Typo->SetObjectField(TEXT("location"), Vec(NewA));
    Typo->SetObjectField(TEXT("rotaton"), Vec(FVector(0.0, 90.0, 0.0)));
    TSharedPtr<FJsonObject> TypoArgs = Payload(HolderLabel);
    TypoArgs->SetArrayField(TEXT("transforms"), {MakeShared<FJsonValueObject>(Typo)});
    TestEqual(TEXT("unknown row key -> UNKNOWN_NESTED_PARAMS"),
        DispatchErrorCode(TEXT("actor.add_instances"), TypoArgs), FString(TEXT("UNKNOWN_NESTED_PARAMS")));
    TestEqual(TEXT("the typo row added nothing"), Hism->GetInstanceCount(), 5);

    // A wrong-shaped value is refused rather than parsed into a default: scale {x:2} would
    // otherwise become (2,0,0), a degenerate instance that reads back "equal" and counts as added.
    TSharedPtr<FJsonObject> HalfScale = MakeShared<FJsonObject>();
    HalfScale->SetNumberField(TEXT("x"), 2.0);
    TSharedPtr<FJsonObject> ShapeRow = MakeShared<FJsonObject>();
    ShapeRow->SetObjectField(TEXT("location"), Vec(NewA));
    ShapeRow->SetObjectField(TEXT("scale"), HalfScale);
    TSharedPtr<FJsonObject> ShapeArgs = Payload(HolderLabel);
    ShapeArgs->SetArrayField(TEXT("transforms"), {MakeShared<FJsonValueObject>(ShapeRow)});
    FTestResponseCapture Shape = Call(TEXT("actor.add_instances"), ShapeArgs);
    TestEqual(TEXT("scale {x:2} -> INVALID_ARGUMENT"), Shape.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestEqual(TEXT("the wrong-shaped row added nothing"), Hism->GetInstanceCount(), 5);

    // A stale expectedCount refuses the replace before it discards anything.
    TSharedPtr<FJsonObject> StaleArgs = Payload(HolderLabel);
    StaleArgs->SetBoolField(TEXT("replace"), true);
    StaleArgs->SetNumberField(TEXT("expectedCount"), 3);
    StaleArgs->SetArrayField(TEXT("transforms"), {Row(NewA)});
    FTestResponseCapture Stale = Call(TEXT("actor.add_instances"), StaleArgs);
    TestEqual(TEXT("stale expectedCount -> MATCH_COUNT_MISMATCH"), Stale.ErrorCode,
        FString(TEXT("MATCH_COUNT_MISMATCH")));
    TestEqual(TEXT("the stale replace cleared nothing"), Hism->GetInstanceCount(), 5);

    // ---- A valid replace: one call, old scatter echoed, and the echo replays ----
    TSharedPtr<FJsonObject> ReplaceArgs = Payload(HolderLabel);
    ReplaceArgs->SetBoolField(TEXT("replace"), true);
    ReplaceArgs->SetNumberField(TEXT("expectedCount"), 5);
    ReplaceArgs->SetArrayField(TEXT("transforms"), {Row(NewA)});
    FTestResponseCapture Replace = Call(TEXT("actor.add_instances"), ReplaceArgs);
    TestTrue(*FString::Printf(TEXT("replace succeeds (%s: %s)"), *Replace.ErrorCode, *Replace.Message),
        Replace.bSuccess);
    TestEqual(TEXT("replace leaves exactly the new row"), Hism->GetInstanceCount(), 1);
    const TArray<TSharedPtr<FJsonValue>>* Removed = nullptr;
    if (!TestTrue(TEXT("removedInstances echoes all 5 discarded instances"), Replace.Result.IsValid()
            && Replace.Result->TryGetArrayField(TEXT("removedInstances"), Removed) && Removed->Num() == 5))
    {
        return true;
    }
    TestTrue(TEXT("the record carries the discarded instance 0's world location"),
        RowLocation(Replace.Result, TEXT("removedInstances"), 0).Equals(Before0, 0.01));

    TSharedPtr<FJsonObject> ReplayArgs = Payload(HolderLabel);
    ReplayArgs->SetArrayField(TEXT("transforms"), *Removed);
    FTestResponseCapture Replay = Call(TEXT("actor.add_instances"), ReplayArgs);
    TestTrue(*FString::Printf(TEXT("the removal record replays verbatim (%s: %s)"), *Replay.ErrorCode,
        *Replay.Message), Replay.bSuccess);
    TestEqual(TEXT("replay restores the 5 instances beside the new one"), Hism->GetInstanceCount(), 6);
    TestTrue(TEXT("the old instance 0 is back where it was (now index 1)"),
        WorldLocation(Hism, 1).Equals(Before0, 0.01));

    // ---- A foliage component is refused by both verbs: its instances belong to the foliage
    // ledger, and a count change here would desync it. Class resolved by reflection (MinimalAPI).
    UClass* FoliageClass =
        FindObject<UClass>(nullptr, TEXT("/Script/Foliage.FoliageInstancedStaticMeshComponent"));
    AActor* FoliageHolder = FoliageClass
        ? World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(HolderLocation + FVector(0.0, 3000.0, 0.0)))
        : nullptr;
    if (!FoliageHolder)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("foliage-component-unavailable"),
            TEXT("FoliageInstancedStaticMeshComponent did not resolve or its holder did not spawn; "
                 "the foliage refusal assertions were stepped over."));
        return true;
    }
    const FString FoliageLabel = Label(TEXT("Foliage"));
    UInstancedStaticMeshComponent* Foliage =
        NewObject<UInstancedStaticMeshComponent>(FoliageHolder, FoliageClass, TEXT("FoliageScatter"));
    FoliageHolder->SetRootComponent(Foliage);
    Foliage->RegisterComponent();
    Foliage->AddInstance(FTransform::Identity);
    FoliageHolder->SetActorLabel(FoliageLabel);
    if (!TestEqual(TEXT("fixture precondition: the foliage component carries one instance"),
            Foliage->GetInstanceCount(), 1))
    {
        return true;
    }

    TSharedPtr<FJsonObject> FoliageAdd = Payload(FoliageLabel);
    FoliageAdd->SetArrayField(TEXT("transforms"), {Row(NewA)});
    TestEqual(TEXT("add_instances on a foliage component -> INVALID_TARGET_KIND"),
        Call(TEXT("actor.add_instances"), FoliageAdd).ErrorCode, FString(TEXT("INVALID_TARGET_KIND")));
    TSharedPtr<FJsonObject> FoliageClear = Payload(FoliageLabel);
    FoliageClear->SetBoolField(TEXT("all"), true);
    TestEqual(TEXT("remove_instances on a foliage component -> INVALID_TARGET_KIND"),
        Call(TEXT("actor.remove_instances"), FoliageClear).ErrorCode, FString(TEXT("INVALID_TARGET_KIND")));
    TestEqual(TEXT("the foliage component kept its instance"), Foliage->GetInstanceCount(), 1);

    // ---- The ACTOR branch: a plain HISM on an InstancedFoliageActor. Some foliage
    // implementations keep their instances in components that are not
    // FoliageInstancedStaticMeshComponents, so only the actor check catches them. The level's
    // foliage actor is reused when one exists (a new one is destroyed by the guard), and the
    // fixture component is removed from it afterwards.
    const bool bIfaPreexisted = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(World, false) != nullptr;
    AInstancedFoliageActor* Ifa = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(World, true);
    if (!Ifa)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("foliage-actor-unavailable"),
            TEXT("no InstancedFoliageActor could be found or created in the current level; the "
                 "actor-branch foliage refusal assertions were stepped over."));
        return true;
    }
    UHierarchicalInstancedStaticMeshComponent* IfaHism = NewObject<UHierarchicalInstancedStaticMeshComponent>(
        Ifa, MakeUniqueObjectName(Ifa, UHierarchicalInstancedStaticMeshComponent::StaticClass(),
            TEXT("PWI_IfaPlainHism")), RF_Transactional);
    IfaHism->SetupAttachment(Ifa->GetRootComponent());
    Ifa->AddInstanceComponent(IfaHism);
    IfaHism->RegisterComponent();
    IfaHism->AddInstance(FTransform::Identity);
    ON_SCOPE_EXIT
    {
        if (bIfaPreexisted && IsValid(IfaHism))
        {
            Ifa->RemoveInstanceComponent(IfaHism);
            IfaHism->DestroyComponent();
        }
    };
    if (!TestEqual(TEXT("fixture precondition: the foliage actor's plain HISM carries one instance"),
            IfaHism->GetInstanceCount(), 1))
    {
        return true;
    }
    TSharedPtr<FJsonObject> IfaClear = Payload(Ifa->GetPathName());
    IfaClear->SetStringField(TEXT("component"), IfaHism->GetName());
    IfaClear->SetBoolField(TEXT("all"), true);
    TestEqual(TEXT("remove_instances on an InstancedFoliageActor -> INVALID_TARGET_KIND"),
        Call(TEXT("actor.remove_instances"), IfaClear).ErrorCode, FString(TEXT("INVALID_TARGET_KIND")));
    TestEqual(TEXT("the foliage actor's component kept its instance"), IfaHism->GetInstanceCount(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorRemoveInstancesTest,
    "PinWright.actor.remove_instances.RemovesListedRefusesWholeAndUndoRestores",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorRemoveInstancesTest::RunTest(const FString& Parameters)
{
    using namespace TestInstancedMeshAddRemoveHelpers;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("no editor world; the actor.remove_instances assertions were stepped over."));
        return true;
    }
    FScopedEditorWorldActorGuard Guard;
    const FString HolderLabel = Label(TEXT("Remove"));
    UHierarchicalInstancedStaticMeshComponent* Hism = SpawnHolder(*this, World, HolderLabel, 4);
    if (!Hism)
    {
        return true;
    }
    const FVector Before1 = WorldLocation(Hism, 1);

    auto Remove = [&](const TSharedPtr<FJsonObject>& Args) { return Call(TEXT("actor.remove_instances"), Args); };
    auto WithIndices = [&](std::initializer_list<int32> List)
    {
        TSharedPtr<FJsonObject> Args = Payload(HolderLabel);
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const int32 Index : List)
        {
            Values.Add(MakeShared<FJsonValueNumber>(Index));
        }
        Args->SetArrayField(TEXT("indices"), Values);
        Args->SetNumberField(TEXT("expectedCount"), Hism->GetInstanceCount());
        return Args;
    };

    // ---- Refusals: nothing removed. The valid index comes FIRST, so a verb that removed as it
    // validated would have taken instance 0 before reaching the bad one. ----
    FTestResponseCapture OutOfRange = Remove(WithIndices({0, 99}));
    TestEqual(TEXT("out-of-range index -> INSTANCE_INDEX_OUT_OF_RANGE"), OutOfRange.ErrorCode,
        FString(TEXT("INSTANCE_INDEX_OUT_OF_RANGE")));
    FTestResponseCapture Duplicate = Remove(WithIndices({2, 2}));
    TestEqual(TEXT("duplicated index -> INVALID_ARGUMENT"), Duplicate.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    // A removal by index without expectedCount is refused: a retried call would delete whatever
    // was renumbered into those slots.
    TSharedPtr<FJsonObject> Unguarded = WithIndices({0});
    Unguarded->RemoveField(TEXT("expectedCount"));
    TestEqual(TEXT("indices without expectedCount -> MISSING_REQUIRED_PARAM"),
        Remove(Unguarded).ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    TSharedPtr<FJsonObject> Fractional = WithIndices({});
    Fractional->SetArrayField(TEXT("indices"), {MakeShared<FJsonValueNumber>(1.5)});
    TestEqual(TEXT("a non-integer index -> INVALID_ARGUMENT"), Remove(Fractional).ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    FTestResponseCapture NoScope = Remove(Payload(HolderLabel));
    TestEqual(TEXT("neither indices nor all -> INVALID_ARGUMENT"), NoScope.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    TSharedPtr<FJsonObject> BothArgs = WithIndices({0});
    BothArgs->SetBoolField(TEXT("all"), true);
    FTestResponseCapture Both = Remove(BothArgs);
    TestEqual(TEXT("indices plus all -> INVALID_ARGUMENT"), Both.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestEqual(TEXT("no refused call removed anything"), Hism->GetInstanceCount(), 4);

    // ---- Remove one by index: measured count, and the record names what went ----
    FTestResponseCapture One = Remove(WithIndices({1}));
    TestTrue(*FString::Printf(TEXT("removing index 1 succeeds (%s: %s)"), *One.ErrorCode, *One.Message),
        One.bSuccess);
    TestEqual(TEXT("removed is 1"), Number(One.Result, TEXT("removed")), 1.0);
    TestEqual(TEXT("the component holds 3"), Hism->GetInstanceCount(), 3);
    TestTrue(TEXT("removedInstances[0] carries the removed instance's world location"),
        RowLocation(One.Result, TEXT("removedInstances"), 0).Equals(Before1, 0.01));

    // ---- Clear all, in one undoable transaction. One custom-data float per instance, which
    // removedInstances[] cannot carry, so the response must say so. ----
    Hism->SetNumCustomDataFloats(1);
    TestEqual(TEXT("fixture precondition: one custom-data float per instance"), Hism->NumCustomDataFloats, 1);
    TSharedPtr<FJsonObject> AllArgs = Payload(HolderLabel);
    AllArgs->SetBoolField(TEXT("all"), true);
    FTestResponseCapture All = Remove(AllArgs);
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (TestTrue(TEXT("a clear of a component with custom data carries one warning"), All.Result.IsValid()
            && All.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings->Num() == 1))
    {
        TestTrue(TEXT("the warning names the unrecorded custom data"),
            (*Warnings)[0]->AsString().Contains(TEXT("custom-data")));
    }
    TestTrue(*FString::Printf(TEXT("all:true succeeds (%s: %s)"), *All.ErrorCode, *All.Message), All.bSuccess);
    TestEqual(TEXT("all:true reports 3 removed"), Number(All.Result, TEXT("removed")), 3.0);
    TestEqual(TEXT("the component is empty"), Hism->GetInstanceCount(), 0);

    if (!GEditor->Trans)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_transaction_buffer"),
            TEXT("GEditor->Trans is null; the remove_instances undo assertions were stepped over."));
    }
    else
    {
        const FTransaction* Newest = GEditor->Trans->GetQueueLength() > 0
            ? GEditor->Trans->GetTransaction(GEditor->Trans->GetQueueLength() - 1) : nullptr;
        const bool bOurs = TestNotNull(TEXT("the clear recorded a transaction"), Newest)
            && TestEqual(TEXT("newest undo entry is the clear's own"),
                Newest->GetContext().Title.ToString(), FString(TEXT("MCP: actor.remove_instances")))
            && TestTrue(TEXT("the transaction recorded the component"), Newest->ContainsObject(Hism));
        if (bOurs && TestTrue(TEXT("undo succeeds"), GEditor->UndoTransaction()))
        {
            TestEqual(TEXT("editor.undo brings the 3 cleared instances back"), Hism->GetInstanceCount(), 3);
        }
    }

    // ---- The removal record replays through add_instances, and an empty clear is refused ----
    const TArray<TSharedPtr<FJsonValue>>* Removed = nullptr;
    if (All.Result.IsValid() && All.Result->TryGetArrayField(TEXT("removedInstances"), Removed))
    {
        TSharedPtr<FJsonObject> ClearAgain = Payload(HolderLabel);
        ClearAgain->SetBoolField(TEXT("all"), true);
        Remove(ClearAgain);
        TestEqual(TEXT("cleared again before the replay"), Hism->GetInstanceCount(), 0);

        TSharedPtr<FJsonObject> ReplayArgs = Payload(HolderLabel);
        ReplayArgs->SetArrayField(TEXT("transforms"), *Removed);
        FTestResponseCapture Replay = Call(TEXT("actor.add_instances"), ReplayArgs);
        TestTrue(*FString::Printf(TEXT("removedInstances replays through actor.add_instances (%s: %s)"),
            *Replay.ErrorCode, *Replay.Message), Replay.bSuccess);
        TestEqual(TEXT("the replay restores 3 instances"), Hism->GetInstanceCount(), 3);
    }
    else
    {
        AddError(TEXT("all:true reported no removedInstances array"));
    }

    Remove(AllArgs);
    FTestResponseCapture Empty = Remove(AllArgs);
    TestEqual(TEXT("clearing an empty component -> INVALID_ARGUMENT"), Empty.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));
    return true;
}
