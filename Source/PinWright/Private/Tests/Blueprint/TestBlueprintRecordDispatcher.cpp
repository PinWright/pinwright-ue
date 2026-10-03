// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.record_dispatcher (F-observe-blueprint-dispatcher-at-runtime).
//
// Fixture: a Blueprint (UObject parent) with dispatcher OnFoo(Amount:int, Label:string) added by
// blueprint.add_dispatcher, and a live instance of its generated class. Broadcasts go through the
// delegate's own ProcessDelegate, which is what the Blueprint VM's CallMulticastDelegate calls.
//
// Every test reads the dispatcher's invocation list directly (BoundObjectCount), never only the
// verb's own bindingRemoved field, and asserts the binding is PRESENT while the job runs - the
// known-bad control that proves the fixture bound at all before "unbound" is believed.
// Counterfactuals: drop the Unbind call on any exit and the post-exit BoundObjectCount checks
// fail; drop the parameter export and the params[] assertions fail; drop the pause and the
// bDebugPauseExecution assertion fails.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/PieTimeControl.h"
#include "Components/SceneComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utils/PieState.h"

namespace TestBlueprintRecordDispatcherHelpers
{
    struct FFixture
    {
        FString AssetPath;
        UClass* GeneratedClass = nullptr;
        FMulticastDelegateProperty* Property = nullptr;
    };

    // Creates the Blueprint and checks the signature preconditions; false (with errors) on failure.
    // bLeadingInt adds an int member variable (LayoutPad) before OnFoo, so removing it later
    // shifts OnFoo's offset.
    bool MakeFixture(FAutomationTestBase& Test, FFixture& Out, bool bLeadingInt = false)
    {
        Out.AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/BP_RecordDispatcher_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* Package = CreatePackage(*Out.AssetPath);
        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(Out.AssetPath)), BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        if (!Test.TestNotNull(TEXT("fixture Blueprint created"), Blueprint))
        {
            return false;
        }

        if (bLeadingInt)
        {
            TSharedPtr<FJsonObject> VarPayload = MakeShared<FJsonObject>();
            VarPayload->SetStringField(TEXT("path"), Out.AssetPath);
            VarPayload->SetStringField(TEXT("variableName"), TEXT("LayoutPad"));
            VarPayload->SetStringField(TEXT("variableType"), TEXT("int"));
            FTestResponseCapture VarCapture;
            if (!InvokeHandlerWithCapture(TEXT("blueprint.add_variable"), VarPayload, VarCapture) || !VarCapture.bSuccess)
            {
                Test.AddError(FString::Printf(TEXT("fixture blueprint.add_variable failed: %s %s"), *VarCapture.ErrorCode, *VarCapture.Message));
                return false;
            }
        }

        TArray<TSharedPtr<FJsonValue>> Params;
        for (const TPair<const TCHAR*, const TCHAR*>& Pin : { TPair<const TCHAR*, const TCHAR*>(TEXT("Amount"), TEXT("int")),
                                                               TPair<const TCHAR*, const TCHAR*>(TEXT("Label"), TEXT("string")) })
        {
            TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
            Param->SetStringField(TEXT("name"), Pin.Key);
            Param->SetStringField(TEXT("type"), Pin.Value);
            Params.Add(MakeShared<FJsonValueObject>(Param));
        }
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Out.AssetPath);
        Payload->SetStringField(TEXT("name"), TEXT("OnFoo"));
        Payload->SetArrayField(TEXT("params"), Params);
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("blueprint.add_dispatcher"), Payload, Capture) || !Capture.bSuccess)
        {
            Test.AddError(FString::Printf(TEXT("fixture blueprint.add_dispatcher failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }

        Out.GeneratedClass = Blueprint->GeneratedClass.Get();
        Out.Property = Out.GeneratedClass ? FindFProperty<FMulticastDelegateProperty>(Out.GeneratedClass, TEXT("OnFoo")) : nullptr;
        if (!Test.TestNotNull(TEXT("fixture class has dispatcher OnFoo"), Out.Property)
            || !Test.TestNotNull(TEXT("fixture dispatcher has a signature"), Out.Property->SignatureFunction.Get()))
        {
            return false;
        }
        UFunction* Signature = Out.Property->SignatureFunction;
        return Test.TestNotNull(TEXT("fixture signature has Amount:int"), FindFProperty<FIntProperty>(Signature, TEXT("Amount")))
            && Test.TestNotNull(TEXT("fixture signature has Label:string"), FindFProperty<FStrProperty>(Signature, TEXT("Label")));
    }

    UObject* NewInstance(const FFixture& Fixture, UObject* Outer)
    {
        return NewObject<UObject>(Outer, Fixture.GeneratedClass,
            MakeUniqueObjectName(Outer, Fixture.GeneratedClass, TEXT("RecordDispatcherProbe")));
    }

    void Broadcast(const FFixture& Fixture, UObject* Instance, int32 Amount, const FString& Label)
    {
        UFunction* Signature = Fixture.Property->SignatureFunction;
        TArray<uint8> Buffer;
        Buffer.SetNumZeroed(FMath::Max<int32>(1, Signature->ParmsSize));
        Signature->InitializeStruct(Buffer.GetData());
        FindFProperty<FIntProperty>(Signature, TEXT("Amount"))->SetPropertyValue_InContainer(Buffer.GetData(), Amount);
        FindFProperty<FStrProperty>(Signature, TEXT("Label"))->SetPropertyValue_InContainer(Buffer.GetData(), Label);
        const FMulticastScriptDelegate* Delegate =
            Fixture.Property->GetMulticastDelegate(Fixture.Property->ContainerPtrToValuePtr<void>(Instance));
        if (Delegate)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
            Delegate->ProcessDelegate<UObject>(Buffer.GetData());
#else
            Delegate->ProcessMulticastDelegate<UObject>(Buffer.GetData());
#endif
        }
        Signature->DestroyStruct(Buffer.GetData());
    }

    int32 BoundObjectCount(const FFixture& Fixture, UObject* Instance)
    {
        const FMulticastScriptDelegate* Delegate =
            Fixture.Property->GetMulticastDelegate(Fixture.Property->ContainerPtrToValuePtr<void>(Instance));
        return Delegate ? Delegate->GetAllObjects().Num() : 0;
    }

    TSharedPtr<FJsonObject> Payload(UObject* Instance, int32 Count, double TimeoutSeconds)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("objectPath"), Instance->GetPathName());
        Json->SetStringField(TEXT("dispatcher"), TEXT("OnFoo"));
        Json->SetNumberField(TEXT("count"), Count);
        Json->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
        return Json;
    }

    // Starts the job and returns its ticket id ("" with errors on failure).
    FString StartRecording(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Json)
    {
        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("blueprint.record_dispatcher registered"),
                InvokeHandlerWithCapture(TEXT("blueprint.record_dispatcher"), Json, Capture))
            || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("record_dispatcher did not start: %s %s"), *Capture.ErrorCode, *Capture.Message));
            return FString();
        }
        FString TicketId;
        Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId);
        const TArray<TSharedPtr<FJsonValue>>* Signature = nullptr;
        Test.TestTrue(TEXT("started response describes the two-param signature"),
            Capture.Result->TryGetArrayField(TEXT("signature"), Signature) && Signature && Signature->Num() == 2);
        Test.TestFalse(TEXT("started response carries a ticket id"), TicketId.IsEmpty());
        return TicketId;
    }

    FJobTicket GetTicket(const FString& TicketId)
    {
        FJobTicket Ticket;
        FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket);
        return Ticket;
    }

    double NumberOf(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
    {
        double Value = -1.0;
        if (Json.IsValid())
        {
            Json->TryGetNumberField(Field, Value);
        }
        return Value;
    }

    FString StringOf(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
    {
        FString Value;
        if (Json.IsValid())
        {
            Json->TryGetStringField(Field, Value);
        }
        return Value;
    }

    bool BoolOf(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
    {
        bool bValue = false;
        return Json.IsValid() && Json->TryGetBoolField(Field, bValue) && bValue;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherCountReachedTest,
    "PinWright.blueprint.record_dispatcher.CountReachedRecordsParamsAndUnbinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherCountReachedTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    FFixture Fixture;
    ON_SCOPE_EXIT { if (!Fixture.AssetPath.IsEmpty()) { CleanupTestAsset(Fixture.AssetPath); } };
    if (!MakeFixture(*this, Fixture))
    {
        return true;
    }
    TStrongObjectPtr<UObject> Instance(NewInstance(Fixture, GetTransientPackage()));
    TestEqual(TEXT("fixture instance starts unbound"), BoundObjectCount(Fixture, Instance.Get()), 0);

    const FString TicketId = StartRecording(*this, Payload(Instance.Get(), 2, 30.0));
    if (TicketId.IsEmpty())
    {
        return true;
    }
    TestEqual(TEXT("recorder is bound while the job runs"), BoundObjectCount(Fixture, Instance.Get()), 1);

    Broadcast(Fixture, Instance.Get(), 7, TEXT("first"));
    TestEqual(TEXT("one broadcast of two: job still running"), GetTicket(TicketId).Status, FString(TEXT("running")));
    TestEqual(TEXT("one broadcast of two: still bound"), BoundObjectCount(Fixture, Instance.Get()), 1);

    Broadcast(Fixture, Instance.Get(), 9, TEXT("second"));
    const FJobTicket Done = GetTicket(TicketId);
    TestEqual(TEXT("count reached: job completed"), Done.Status, FString(TEXT("completed")));
    TestEqual(TEXT("count reached: recorder gone from the invocation list"), BoundObjectCount(Fixture, Instance.Get()), 0);

    // Late broadcast: the recorder is already unbound, so the fireCount read below must stay 2.
    Broadcast(Fixture, Instance.Get(), 11, TEXT("late"));

    const TSharedPtr<FJsonObject> Result = GetTicket(TicketId).Result;
    if (!TestTrue(TEXT("completed ticket carries a result"), Result.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("outcome"), StringOf(Result, TEXT("outcome")), FString(TEXT("count_reached")));
    TestTrue(TEXT("met"), BoolOf(Result, TEXT("met")));
    TestEqual(TEXT("fireCount stops at the requested count"), NumberOf(Result, TEXT("fireCount")), 2.0);
    TestTrue(TEXT("bindingRemoved"), BoolOf(Result, TEXT("bindingRemoved")));
    TestTrue(TEXT("targetAlive"), BoolOf(Result, TEXT("targetAlive")));

    const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
    if (TestTrue(TEXT("two records"), Result->TryGetArrayField(TEXT("records"), Records) && Records && Records->Num() == 2))
    {
        const int32 ExpectedAmounts[] = { 7, 9 };
        const TCHAR* ExpectedLabels[] = { TEXT("first"), TEXT("second") };
        for (int32 Index = 0; Index < 2; ++Index)
        {
            const TSharedPtr<FJsonObject> Record = (*Records)[Index]->AsObject();
            TestEqual(FString::Printf(TEXT("record %d seq"), Index), NumberOf(Record, TEXT("seq")), double(Index + 1));
            TestTrue(FString::Printf(TEXT("record %d has elapsedSeconds"), Index), NumberOf(Record, TEXT("elapsedSeconds")) >= 0.0);
            const TSharedPtr<FJsonObject>* Params = nullptr;
            if (TestTrue(FString::Printf(TEXT("record %d has params"), Index),
                    Record.IsValid() && Record->TryGetObjectField(TEXT("params"), Params) && Params))
            {
                TestEqual(FString::Printf(TEXT("record %d Amount"), Index), NumberOf(*Params, TEXT("Amount")), double(ExpectedAmounts[Index]));
                TestEqual(FString::Printf(TEXT("record %d Label"), Index), StringOf(*Params, TEXT("Label")), FString(ExpectedLabels[Index]));
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherCancelTest,
    "PinWright.blueprint.record_dispatcher.CancelUnbinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherCancelTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    FFixture Fixture;
    ON_SCOPE_EXIT { if (!Fixture.AssetPath.IsEmpty()) { CleanupTestAsset(Fixture.AssetPath); } };
    if (!MakeFixture(*this, Fixture))
    {
        return true;
    }
    TStrongObjectPtr<UObject> Instance(NewInstance(Fixture, GetTransientPackage()));

    const FString TicketId = StartRecording(*this, Payload(Instance.Get(), 1, 30.0));
    if (TicketId.IsEmpty())
    {
        return true;
    }
    TestEqual(TEXT("recorder is bound while the job runs"), BoundObjectCount(Fixture, Instance.Get()), 1);

    TestEqual(TEXT("cancel is supported"),
        static_cast<int32>(FPluginState::Get().GetJobRegistry().Cancel(TicketId)),
        static_cast<int32>(EJobCancelResult::Requested));
    TestEqual(TEXT("ticket cancelled"), GetTicket(TicketId).Status, FString(TEXT("cancelled")));
    TestEqual(TEXT("cancel removed the recorder from the invocation list"), BoundObjectCount(Fixture, Instance.Get()), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherRefusalsTest,
    "PinWright.blueprint.record_dispatcher.RefusalsNameTheWayOut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    FFixture Fixture;
    ON_SCOPE_EXIT { if (!Fixture.AssetPath.IsEmpty()) { CleanupTestAsset(Fixture.AssetPath); } };
    if (!MakeFixture(*this, Fixture))
    {
        return true;
    }
    TStrongObjectPtr<UObject> Instance(NewInstance(Fixture, GetTransientPackage()));

    TSharedPtr<FJsonObject> Unknown = Payload(Instance.Get(), 1, 5.0);
    Unknown->SetStringField(TEXT("dispatcher"), TEXT("OnNoSuchThing"));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("blueprint.record_dispatcher"), Unknown, Capture);
    TestFalse(TEXT("unknown dispatcher refused"), Capture.bSuccess);
    TestEqual(TEXT("unknown dispatcher code"), Capture.ErrorCode, FString(ErrorCodes::ERR_DISPATCHER_NOT_FOUND));
    // The refusal payload names the dispatchers the object does have.
    bool bListsOnFoo = false;
    const TArray<TSharedPtr<FJsonValue>>* Available = nullptr;
    if (Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("available"), Available) && Available)
    {
        for (const TSharedPtr<FJsonValue>& Name : *Available)
        {
            bListsOnFoo |= Name->AsString() == TEXT("OnFoo");
        }
    }
    TestTrue(TEXT("available[] lists OnFoo"), bListsOnFoo);

    // A destroyed (garbage, not yet collected) object is refused, not bound.
    TStrongObjectPtr<UObject> Destroyed(NewInstance(Fixture, GetTransientPackage()));
    Destroyed->MarkAsGarbage();
    InvokeHandlerWithCapture(TEXT("blueprint.record_dispatcher"), Payload(Destroyed.Get(), 1, 5.0), Capture);
    TestFalse(TEXT("destroyed object refused"), Capture.bSuccess);
    TestEqual(TEXT("destroyed object refusal code"), Capture.ErrorCode, FString(ErrorCodes::ERR_OBJECT_NOT_FOUND));
    TestEqual(TEXT("refused destroyed object holds no binding"), BoundObjectCount(Fixture, Destroyed.Get()), 0);

    // A class default object never broadcasts at runtime.
    TSharedPtr<FJsonObject> Template = Payload(Instance.Get(), 1, 5.0);
    Template->SetStringField(TEXT("objectPath"), Fixture.GeneratedClass->GetDefaultObject()->GetPathName());
    InvokeHandlerWithCapture(TEXT("blueprint.record_dispatcher"), Template, Capture);
    TestFalse(TEXT("class default object refused"), Capture.bSuccess);
    TestEqual(TEXT("class default object refusal code"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));
    TestEqual(TEXT("refused CDO holds no binding"),
        BoundObjectCount(Fixture, Fixture.GeneratedClass->GetDefaultObject()), 0);

    if (!GEditor || !GEditor->PlayWorld)
    {
        TSharedPtr<FJsonObject> Pause = Payload(Instance.Get(), 1, 5.0);
        Pause->SetBoolField(TEXT("pauseOnFire"), true);
        InvokeHandlerWithCapture(TEXT("blueprint.record_dispatcher"), Pause, Capture);
        TestFalse(TEXT("pauseOnFire on a non-PIE object refused"), Capture.bSuccess);
        TestEqual(TEXT("pauseOnFire refusal code"), Capture.ErrorCode, FString(ErrorCodes::ERR_NO_ACTIVE_SESSION));
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The pauseOnFire refusal needs no PIE session running."));
    }

    TestEqual(TEXT("no refusal left a binding behind"), BoundObjectCount(Fixture, Instance.Get()), 0);
    return true;
}

// ---- Timeout (needs the core ticker, so latent) ----

namespace TestBlueprintRecordDispatcherHelpers
{
    struct FTimeoutState
    {
        FFixture Fixture;
        TStrongObjectPtr<UObject> Instance;
        FString TicketId;
        double Deadline = 0.0;
    };
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRecordDispatcherAwaitTimeout,
    FAutomationTestBase*, Test, TSharedRef<TestBlueprintRecordDispatcherHelpers::FTimeoutState>, State);

bool FRecordDispatcherAwaitTimeout::Update()
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    const FJobTicket Ticket = GetTicket(State->TicketId);
    if (Ticket.Status == TEXT("running") && FPlatformTime::Seconds() < State->Deadline)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        State->Instance.Reset();
        CleanupTestAsset(State->Fixture.AssetPath);
    };
    Test->TestEqual(TEXT("timed-out job completed"), Ticket.Status, FString(TEXT("completed")));
    Test->TestEqual(TEXT("timeout removed the recorder from the invocation list"),
        BoundObjectCount(State->Fixture, State->Instance.Get()), 0);
    const TSharedPtr<FJsonObject> Result = Ticket.Result;
    Test->TestEqual(TEXT("outcome"), StringOf(Result, TEXT("outcome")), FString(TEXT("timed_out")));
    Test->TestFalse(TEXT("met is false with no broadcast"), BoolOf(Result, TEXT("met")));
    Test->TestEqual(TEXT("fireCount"), NumberOf(Result, TEXT("fireCount")), 0.0);
    Test->TestTrue(TEXT("bindingRemoved"), BoolOf(Result, TEXT("bindingRemoved")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherTimeoutTest,
    "PinWright.blueprint.record_dispatcher.TimeoutCompletesUnmetAndUnbinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherTimeoutTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    const TSharedRef<FTimeoutState> State = MakeShared<FTimeoutState>();
    if (!MakeFixture(*this, State->Fixture))
    {
        if (!State->Fixture.AssetPath.IsEmpty())
        {
            CleanupTestAsset(State->Fixture.AssetPath);
        }
        return true;
    }
    State->Instance.Reset(NewInstance(State->Fixture, GetTransientPackage()));
    State->TicketId = StartRecording(*this, Payload(State->Instance.Get(), 1, 0.2));
    if (State->TicketId.IsEmpty())
    {
        CleanupTestAsset(State->Fixture.AssetPath);
        return true;
    }
    TestEqual(TEXT("recorder is bound while the job runs"), BoundObjectCount(State->Fixture, State->Instance.Get()), 1);
    State->Deadline = FPlatformTime::Seconds() + 10.0;
    ADD_LATENT_AUTOMATION_COMMAND(FRecordDispatcherAwaitTimeout(this, State));
    return true;
}

// ---- Target destroyed (needs the core ticker, so latent) ----

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRecordDispatcherAwaitTargetDestroyed,
    FAutomationTestBase*, Test, TSharedRef<TestBlueprintRecordDispatcherHelpers::FTimeoutState>, State);

bool FRecordDispatcherAwaitTargetDestroyed::Update()
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    const FJobTicket Ticket = GetTicket(State->TicketId);
    if (Ticket.Status == TEXT("running") && FPlatformTime::Seconds() < State->Deadline)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        State->Instance.Reset();
        CleanupTestAsset(State->Fixture.AssetPath);
    };
    Test->TestEqual(TEXT("job completed after the target was destroyed"), Ticket.Status, FString(TEXT("completed")));
    // The strong pointer keeps the garbage object's memory, so its delegate is still readable:
    // the binding must have been removed from it, not merely abandoned with it.
    Test->TestEqual(TEXT("destroyed target holds no binding"),
        BoundObjectCount(State->Fixture, State->Instance.Get()), 0);
    const TSharedPtr<FJsonObject> Result = Ticket.Result;
    Test->TestEqual(TEXT("outcome"), StringOf(Result, TEXT("outcome")), FString(TEXT("target_destroyed")));
    Test->TestFalse(TEXT("targetAlive is false"), BoolOf(Result, TEXT("targetAlive")));
    Test->TestTrue(TEXT("bindingRemoved"), BoolOf(Result, TEXT("bindingRemoved")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherTargetDestroyedTest,
    "PinWright.blueprint.record_dispatcher.TargetDestroyedCompletesAndUnbinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherTargetDestroyedTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    const TSharedRef<FTimeoutState> State = MakeShared<FTimeoutState>();
    if (!MakeFixture(*this, State->Fixture))
    {
        if (!State->Fixture.AssetPath.IsEmpty())
        {
            CleanupTestAsset(State->Fixture.AssetPath);
        }
        return true;
    }
    State->Instance.Reset(NewInstance(State->Fixture, GetTransientPackage()));
    State->TicketId = StartRecording(*this, Payload(State->Instance.Get(), 1, 30.0));
    if (State->TicketId.IsEmpty())
    {
        CleanupTestAsset(State->Fixture.AssetPath);
        return true;
    }
    TestEqual(TEXT("recorder is bound while the job runs"), BoundObjectCount(State->Fixture, State->Instance.Get()), 1);
    State->Instance->MarkAsGarbage();
    State->Deadline = FPlatformTime::Seconds() + 10.0;
    ADD_LATENT_AUTOMATION_COMMAND(FRecordDispatcherAwaitTargetDestroyed(this, State));
    return true;
}

// ---- Recompile that shifts the layout (needs the core ticker, so latent) ----
//
// The fixture has an int (LayoutPad) ahead of OnFoo. Removing it recompiles the class in place:
// the old instance moves to a REINST_ class that keeps the old layout, while the class now puts
// OnFoo at a different offset. The unbind must use the old instance's own class's property;
// using the recompiled class's offset on the old memory leaves the binding in place (or writes
// into the wrong field).

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRecordDispatcherAwaitRecompileExit,
    FAutomationTestBase*, Test, TSharedRef<TestBlueprintRecordDispatcherHelpers::FTimeoutState>, State);

bool FRecordDispatcherAwaitRecompileExit::Update()
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    const FJobTicket Ticket = GetTicket(State->TicketId);
    if (Ticket.Status == TEXT("running") && FPlatformTime::Seconds() < State->Deadline)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        State->Instance.Reset();
        CleanupTestAsset(State->Fixture.AssetPath);
    };
    Test->TestEqual(TEXT("job completed after the recompile"), Ticket.Status, FString(TEXT("completed")));
    Test->TestEqual(TEXT("outcome"), StringOf(Ticket.Result, TEXT("outcome")), FString(TEXT("target_destroyed")));
    Test->TestTrue(TEXT("bindingRemoved"), BoolOf(Ticket.Result, TEXT("bindingRemoved")));
    // Read through the old instance's OWN class (the REINST_ duplicate), whose offset matches its memory.
    UObject* OldInstance = State->Instance.Get();
    const FMulticastDelegateProperty* OwnProperty =
        FindFProperty<FMulticastDelegateProperty>(OldInstance->GetClass(), TEXT("OnFoo"));
    if (Test->TestNotNull(TEXT("old instance's own class still has OnFoo"), OwnProperty))
    {
        const FMulticastScriptDelegate* Delegate =
            OwnProperty->GetMulticastDelegate(OwnProperty->ContainerPtrToValuePtr<void>(OldInstance));
        Test->TestEqual(TEXT("old instance holds no binding after the recompile"),
            Delegate ? Delegate->GetAllObjects().Num() : 0, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherRecompileTest,
    "PinWright.blueprint.record_dispatcher.LayoutShiftingRecompileUnbindsOldInstance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherRecompileTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    const TSharedRef<FTimeoutState> State = MakeShared<FTimeoutState>();
    if (!MakeFixture(*this, State->Fixture, /*bLeadingInt=*/true))
    {
        if (!State->Fixture.AssetPath.IsEmpty())
        {
            CleanupTestAsset(State->Fixture.AssetPath);
        }
        return true;
    }
    State->Instance.Reset(NewInstance(State->Fixture, GetTransientPackage()));
    State->TicketId = StartRecording(*this, Payload(State->Instance.Get(), 1, 30.0));
    if (State->TicketId.IsEmpty())
    {
        CleanupTestAsset(State->Fixture.AssetPath);
        return true;
    }
    TestEqual(TEXT("recorder is bound while the job runs"), BoundObjectCount(State->Fixture, State->Instance.Get()), 1);
    UClass* const GeneratedClass = State->Fixture.GeneratedClass;
    const int32 OldOffset = State->Fixture.Property->GetOffset_ForInternal();
    State->Fixture.Property = nullptr; // freed or rebuilt by the recompile below

    // Removes LayoutPad and recompiles through the shared compile path (SkipGarbageCollection),
    // so the old instance is garbage but still present on the next core tick.
    TSharedPtr<FJsonObject> Remove = MakeShared<FJsonObject>();
    Remove->SetStringField(TEXT("path"), State->Fixture.AssetPath);
    Remove->SetStringField(TEXT("variableName"), TEXT("LayoutPad"));
    FTestResponseCapture Capture;
    if (!InvokeHandlerWithCapture(TEXT("blueprint.remove_variable"), Remove, Capture) || !Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("blueprint.remove_variable failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
    }
    // Preconditions: the recompile reinstanced the old object and actually moved OnFoo.
    const FMulticastDelegateProperty* NewProperty =
        FindFProperty<FMulticastDelegateProperty>(GeneratedClass, TEXT("OnFoo"));
    TestNotEqual(TEXT("fixture: old instance moved off the recompiled class"),
        State->Instance->GetClass(), GeneratedClass);
    if (TestNotNull(TEXT("fixture: recompiled class has OnFoo"), NewProperty))
    {
        TestNotEqual(TEXT("fixture: the recompile shifted OnFoo's offset"), NewProperty->GetOffset_ForInternal(), OldOffset);
    }
    State->Deadline = FPlatformTime::Seconds() + 10.0;
    ADD_LATENT_AUTOMATION_COMMAND(FRecordDispatcherAwaitRecompileExit(this, State));
    return true;
}

// ---- Sparse native delegate (UActorComponent::OnComponentActivated, on a scene component) ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherSparseDelegateTest,
    "PinWright.blueprint.record_dispatcher.SparseNativeDelegateRecordsAndUnbinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherSparseDelegateTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    // UActorComponent is abstract; USceneComponent is the plainest concrete carrier of the delegate.
    TStrongObjectPtr<USceneComponent> Component(NewObject<USceneComponent>(GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), USceneComponent::StaticClass(), TEXT("RecordDispatcherSparseProbe"))));
    if (!TestNotNull(TEXT("fixture: OnComponentActivated is a sparse multicast delegate property"),
            FindFProperty<FMulticastSparseDelegateProperty>(UActorComponent::StaticClass(), TEXT("OnComponentActivated"))))
    {
        return true;
    }
    TestFalse(TEXT("fixture component starts unbound"), Component->OnComponentActivated.IsBound());

    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("objectPath"), Component->GetPathName());
    Json->SetStringField(TEXT("dispatcher"), TEXT("OnComponentActivated"));
    Json->SetNumberField(TEXT("timeoutSeconds"), 30.0);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("blueprint.record_dispatcher"), Json, Capture);
    FString TicketId;
    if (!TestTrue(FString::Printf(TEXT("record started (%s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess && Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId)))
    {
        return true;
    }
    TestTrue(TEXT("sparse delegate is bound while the job runs"), Component->OnComponentActivated.IsBound());

    Component->OnComponentActivated.Broadcast(Component.Get(), true);

    TestFalse(TEXT("count reached: sparse delegate unbound"), Component->OnComponentActivated.IsBound());
    const FJobTicket Ticket = GetTicket(TicketId);
    TestEqual(TEXT("job completed"), Ticket.Status, FString(TEXT("completed")));
    TestEqual(TEXT("outcome"), StringOf(Ticket.Result, TEXT("outcome")), FString(TEXT("count_reached")));
    TestTrue(TEXT("bindingRemoved"), BoolOf(Ticket.Result, TEXT("bindingRemoved")));
    const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
    const TSharedPtr<FJsonObject>* Params = nullptr;
    if (TestTrue(TEXT("one record with params"),
            Ticket.Result.IsValid() && Ticket.Result->TryGetArrayField(TEXT("records"), Records) && Records && Records->Num() == 1
            && (*Records)[0]->AsObject()->TryGetObjectField(TEXT("params"), Params) && Params))
    {
        TestEqual(TEXT("Component param is the component's path"), StringOf(*Params, TEXT("Component")), Component->GetPathName());
        TestTrue(TEXT("bReset param is true"), BoolOf(*Params, TEXT("bReset")));
    }
    return true;
}

// ---- pauseOnFire (owned host-neutral PIE) ----

namespace TestBlueprintRecordDispatcherHelpers
{
    struct FPieState
    {
        FFixture Fixture;
        double Deadline = 0.0;
        double CleanupDeadline = 0.0;
    };
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRecordDispatcherRunPauseOnFire,
    FAutomationTestBase*, Test, TSharedRef<TestBlueprintRecordDispatcherHelpers::FPieState>, State);

bool FRecordDispatcherRunPauseOnFire::Update()
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
    if (!PlayWorld)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_world_unavailable"),
            TEXT("The owned PIE session did not create PlayWorld."));
        return true;
    }

    // Outered to the PIE world, so GetWorld() is the PIE world. Released before PIE ends.
    TStrongObjectPtr<UObject> Instance(NewInstance(State->Fixture, PlayWorld));
    ON_SCOPE_EXIT { Instance.Reset(); };
    Test->TestFalse(TEXT("PIE starts unpaused"), PlayWorld->bDebugPauseExecution);

    TSharedPtr<FJsonObject> Json = Payload(Instance.Get(), 1, 30.0);
    Json->SetBoolField(TEXT("pauseOnFire"), true);
    const FString TicketId = StartRecording(*Test, Json);
    if (TicketId.IsEmpty())
    {
        return true;
    }
    Test->TestFalse(TEXT("arming the recorder does not pause"), PlayWorld->bDebugPauseExecution);

    Broadcast(State->Fixture, Instance.Get(), 3, TEXT("pie"));
    Test->TestTrue(TEXT("the completing broadcast paused the PIE world"), PlayWorld->bDebugPauseExecution);
    const TSharedPtr<FJsonObject> Result = GetTicket(TicketId).Result;
    Test->TestTrue(TEXT("result reports paused"), BoolOf(Result, TEXT("paused")));
    Test->TestTrue(TEXT("record carries worldSeconds in PIE"), [&Result]()
    {
        const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
        double WorldSeconds = -1.0;
        return Result.IsValid() && Result->TryGetArrayField(TEXT("records"), Records) && Records && Records->Num() == 1
            && (*Records)[0]->AsObject()->TryGetNumberField(TEXT("worldSeconds"), WorldSeconds);
    }());
    Test->TestEqual(TEXT("recorder gone from the PIE object's invocation list"), BoundObjectCount(State->Fixture, Instance.Get()), 0);

    FTestResponseCapture Resume;
    InvokeHandlerWithCapture(TEXT("editor.resume"), MakeShared<FJsonObject>(), Resume);
    Test->TestTrue(TEXT("editor.resume releases the pause"), Resume.bSuccess && !PlayWorld->bDebugPauseExecution);

    // A broadcast from PIE teardown (world tearing down) or after an end was queued must not
    // pause: EndPIE has fired or is about to, so nothing would release the freeze.
    struct FEndingCase { const TCHAR* Name; bool bTearingDown; };
    for (const FEndingCase& Case : { FEndingCase{ TEXT("tearing down"), true }, FEndingCase{ TEXT("end queued"), false } })
    {
        const FString EndingTicket = StartRecording(*Test, Json);
        if (EndingTicket.IsEmpty())
        {
            return true;
        }
        const bool bWasTearingDown = PlayWorld->bIsTearingDown;
        const bool bWasEndQueued = GEditor->bRequestEndPlayMapQueued;
        ON_SCOPE_EXIT
        {
            PlayWorld->bIsTearingDown = bWasTearingDown;
            GEditor->bRequestEndPlayMapQueued = bWasEndQueued;
        };
        if (Case.bTearingDown)
        {
            PlayWorld->bIsTearingDown = true;
        }
        else
        {
            GEditor->bRequestEndPlayMapQueued = true;
        }
        Broadcast(State->Fixture, Instance.Get(), 4, TEXT("ending"));
        Test->TestFalse(FString::Printf(TEXT("%s: world not paused"), Case.Name), PlayWorld->bDebugPauseExecution);
        Test->TestFalse(FString::Printf(TEXT("%s: UI clock not frozen"), Case.Name), PinWrightPieTime::IsFrozen());
        const TSharedPtr<FJsonObject> EndingResult = GetTicket(EndingTicket).Result;
        Test->TestEqual(FString::Printf(TEXT("%s: count reached"), Case.Name),
            StringOf(EndingResult, TEXT("outcome")), FString(TEXT("count_reached")));
        bool bPaused = true;
        Test->TestTrue(FString::Printf(TEXT("%s: result reports paused false"), Case.Name),
            EndingResult.IsValid() && EndingResult->TryGetBoolField(TEXT("paused"), bPaused) && !bPaused);
    }
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRecordDispatcherCleanupPie,
    FAutomationTestBase*, Test, TSharedRef<TestBlueprintRecordDispatcherHelpers::FPieState>, State);

bool FRecordDispatcherCleanupPie::Update()
{
    if (PinWrightPieState::IsPlayInEditorActive())
    {
        if (State->CleanupDeadline == 0.0)
        {
            State->CleanupDeadline = FPlatformTime::Seconds() + 15.0;
        }
        if (FPlatformTime::Seconds() < State->CleanupDeadline)
        {
            return false;
        }
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
    }
    CleanupTestAsset(State->Fixture.AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintRecordDispatcherPauseOnFireTest,
    "PinWright.blueprint.record_dispatcher.PauseOnFireFreezesPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintRecordDispatcherPauseOnFireTest::RunTest(const FString& Parameters)
{
    using namespace TestBlueprintRecordDispatcherHelpers;
    if (!GEditor || !FSlateApplication::IsInitialized() || !GEditor->GetActiveViewport())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("owned_pie_unavailable"),
            TEXT("Owned PIE requires GEditor, Slate and an active level viewport."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The test never borrows or stops ambient PIE."));
        return true;
    }

    const TSharedRef<FPieState> State = MakeShared<FPieState>();
    if (!MakeFixture(*this, State->Fixture))
    {
        if (!State->Fixture.AssetPath.IsEmpty())
        {
            CleanupTestAsset(State->Fixture.AssetPath);
        }
        return true;
    }
    State->Deadline = FPlatformTime::Seconds() + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FRecordDispatcherRunPauseOnFire(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FRecordDispatcherCleanupPie(this, State));
    return true;
}
