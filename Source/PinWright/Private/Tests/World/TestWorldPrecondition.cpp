// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral coverage for the shared expectWorld dispatcher precondition. A real
// actor.set_transform call proves refusal happens before the handler mutates the
// actor, while matching and omitted assertions exercise the normal handler path.

#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/SafePoint.h"
#include "Dispatch/WorldPrecondition.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "PinWrightSubsystem.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightWorldPreconditionTest
{
    TSharedPtr<FJsonObject> MakeTransformPayload(
        const AActor& Actor, const FVector& Location, const FString* ExpectedWorld)
    {
        TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
        LocationJson->SetNumberField(TEXT("x"), Location.X);
        LocationJson->SetNumberField(TEXT("y"), Location.Y);
        LocationJson->SetNumberField(TEXT("z"), Location.Z);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorPath"), Actor.GetPathName());
        Payload->SetObjectField(TEXT("location"), LocationJson);
        if (ExpectedWorld)
        {
            Payload->SetStringField(TEXT("expectWorld"), *ExpectedWorld);
        }
        return Payload;
    }

    FString ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Value;
        if (Object.IsValid())
        {
            Object->TryGetStringField(Field, Value);
        }
        return Value;
    }

    bool RegistrationDeclaresWorldPrecondition(const FHandlerRegistration& Registration)
    {
        for (const FParamSpec& Param : Registration.Params)
        {
            if (Param.Name == TEXT("expectWorld"))
            {
                return !Param.bRequired && Param.Type == TEXT("string");
            }
        }
        return false;
    }

    FRpcDispatcher*& ActiveNestedDispatcher()
    {
        static FRpcDispatcher* Dispatcher = nullptr;
        return Dispatcher;
    }

    struct FScopedNestedDispatcher
    {
        FScopedNestedDispatcher(FRpcDispatcher& InDispatcher, FResponseCapture& InCapture)
            : Dispatcher(InDispatcher)
        {
            ActiveNestedDispatcher() = &Dispatcher;
            Dispatcher.SetResponseCaptureForTesting(&InCapture);
        }

        ~FScopedNestedDispatcher()
        {
            Dispatcher.SetResponseCaptureForTesting(nullptr);
            ActiveNestedDispatcher() = nullptr;
        }

        FRpcDispatcher& Dispatcher;
    };
}

// Test-only forwarding verb proves that a handler-to-handler call inherits the
// same world precondition immediately before the nested handler entry.
REGISTER_RPC_MUTATING_HANDLER("_test.world_nested_dispatch", "_test",
    "Forward to actor.set_transform for world-precondition coverage",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorPath", "string", "Actor path forwarded to actor.set_transform"),
        RPC_PARAM_OPT("location", "object", "Location forwarded to actor.set_transform")
    ))
{
    FRpcDispatcher* NestedDispatcher =
        PinWrightWorldPreconditionTest::ActiveNestedDispatcher();
    if (!NestedDispatcher || !Ctx.GetRawPayload().IsValid())
    {
        return false;
    }
    TSharedPtr<FJsonObject> NestedPayload = MakeShared<FJsonObject>();
    NestedPayload->SetStringField(TEXT("actorPath"),
        Ctx.GetString(TEXT("actorPath")));
    NestedPayload->SetField(TEXT("location"),
        Ctx.GetRawPayload()->TryGetField(TEXT("location")));
    NestedPayload->SetStringField(TEXT("expectWorld"),
        TEXT("/Game/PinWrightTests/NestedWorldThatCannotBeActive"));
    return NestedDispatcher->DispatchMethod(TEXT("actor.set_transform"), Ctx.GetRequestId(),
        NestedPayload);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldPreconditionDispatchContractTest,
    "PinWright.world.precondition.DispatchContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldPreconditionDispatchContractTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-unavailable"),
            TEXT("The world-precondition behavior test requires an editor world outside PIE."));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World || !World->GetOutermost())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-unavailable"),
            TEXT("The world-precondition behavior test requires a named editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    AStaticMeshActor* Actor = SpawnTransientCubeActor(
        World,
        FString::Printf(TEXT("WorldPreconditionProbe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)),
        FVector(10.0, 20.0, 30.0));
    if (!Actor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-spawn-failed"),
            TEXT("Could not create the real actor fixture for actor.set_transform."));
        return true;
    }

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FHandlerRegistration* Registration =
        Dispatcher.GetAutoRegisteredHandlers().Find(TEXT("actor.set_transform"));
    TestNotNull(TEXT("actor.set_transform is registered"), Registration);
    if (Registration)
    {
        TestTrue(TEXT("actor.set_transform is marked mutating"), Registration->bMutating);
        TestTrue(TEXT("its schema declares optional expectWorld"),
            PinWrightWorldPreconditionTest::RegistrationDeclaresWorldPrecondition(*Registration));
    }
    const FHandlerRegistration* ReadOnlyRegistration =
        Dispatcher.GetAutoRegisteredHandlers().Find(TEXT("actor.get"));
    TestNotNull(TEXT("actor.get is registered"), ReadOnlyRegistration);
    if (ReadOnlyRegistration)
    {
        TestFalse(TEXT("read-only actor.get is not marked mutating"),
            ReadOnlyRegistration->bMutating);
        TestFalse(TEXT("read-only actor.get has no expectWorld schema"),
            PinWrightWorldPreconditionTest::RegistrationDeclaresWorldPrecondition(*ReadOnlyRegistration));
    }
    const FHandlerRegistration* ReadOnlySafePointRegistration =
        Dispatcher.GetAutoRegisteredHandlers().Find(TEXT("audio.analysis.analyze"));
    TestNotNull(TEXT("audio.analysis.analyze is registered"), ReadOnlySafePointRegistration);
    if (ReadOnlySafePointRegistration)
    {
        TestFalse(TEXT("read-only safe-point analysis is not marked mutating"),
            ReadOnlySafePointRegistration->bMutating);
    }

    const FString ActualWorld = World->GetPathName();
    const FString WorldPackage = World->GetOutermost()->GetName();
    const FVector InitialLocation = Actor->GetActorLocation();
    const FString WrongWorld = TEXT("/Game/PinWrightTests/DefinitelyNotTheActiveWorld");

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(
        Dispatcher, Sink, TEXT("actor.set_transform"), TEXT("world-mismatch"),
        PinWrightWorldPreconditionTest::MakeTransformPayload(
            *Actor, FVector(111.0, 222.0, 333.0), &WrongWorld),
        bSuccess, Result, ErrorCode);

    TestFalse(TEXT("a mismatched world is refused"), bSuccess);
    TestEqual(TEXT("the refusal is typed"), ErrorCode,
        FString(ErrorCodes::ERR_WORLD_MISMATCH));
    TestTrue(TEXT("the actor was not mutated"),
        Actor->GetActorLocation().Equals(InitialLocation, KINDA_SMALL_NUMBER));
    TestEqual(TEXT("the mismatch payload echoes the expected world"),
        PinWrightWorldPreconditionTest::ReadString(Result, TEXT("expectedWorld")), WrongWorld);
    TestEqual(TEXT("the mismatch payload names the actual world"),
        PinWrightWorldPreconditionTest::ReadString(Result, TEXT("world")), ActualWorld);
    TestTrue(TEXT("the mismatch message names both worlds"),
        Sink->Message.Contains(WrongWorld) && Sink->Message.Contains(ActualWorld));

    const FVector MatchingLocation(400.0, 500.0, 600.0);
    DispatcherTestHelpers::Dispatch(
        Dispatcher, Sink, TEXT("actor.set_transform"), TEXT("world-match"),
        PinWrightWorldPreconditionTest::MakeTransformPayload(
            *Actor, MatchingLocation, &WorldPackage),
        bSuccess, Result, ErrorCode);

    TestTrue(TEXT("a matching package-path assertion passes"), bSuccess);
    TestEqual(TEXT("the matching call has no error code"), ErrorCode, FString());
    TestTrue(TEXT("the matching call mutates the actor"),
        Actor->GetActorLocation().Equals(MatchingLocation, KINDA_SMALL_NUMBER));

    const FVector UnassertedLocation(700.0, 800.0, 900.0);
    DispatcherTestHelpers::Dispatch(
        Dispatcher, Sink, TEXT("actor.set_transform"), TEXT("world-omitted"),
        PinWrightWorldPreconditionTest::MakeTransformPayload(
            *Actor, UnassertedLocation, nullptr),
        bSuccess, Result, ErrorCode);

    TestTrue(TEXT("omitting expectWorld preserves compatibility"), bSuccess);
    TestEqual(TEXT("the unasserted call has no error code"), ErrorCode, FString());
    TestTrue(TEXT("the unasserted call mutates the actor"),
        Actor->GetActorLocation().Equals(UnassertedLocation, KINDA_SMALL_NUMBER));

    // ProcessRequest establishes the outer active-request scope; the test-only
    // forwarding handler then enters actor.set_transform through DispatchMethod
    // with a deliberately stale nested assertion. The nested body must not run.
    FRpcDispatcher NestedDispatcher;
    NestedDispatcher.Initialize(FResponseSink(
        [](const FString&, bool, const FString&, const TSharedPtr<FJsonObject>&, const FString&) {}));
    // A null subsystem selects the dispatcher capture path and avoids the
    // transport/editor-readiness gate. The real precondition still runs in
    // DispatchMethod immediately before the nested actor handler.
    NestedDispatcher.DrainAutoRegistrations(nullptr);
    if (!TestNotNull(TEXT("nested forwarding handler is registered"),
            NestedDispatcher.GetAutoRegisteredHandlers().Find(
                TEXT("_test.world_nested_dispatch"))))
    {
        return false;
    }
    TSharedPtr<FJsonObject> NestedPayload = MakeShared<FJsonObject>();
    NestedPayload->SetStringField(TEXT("actorPath"), Actor->GetPathName());
    TSharedPtr<FJsonObject> NestedLocation = MakeShared<FJsonObject>();
    NestedLocation->SetNumberField(TEXT("x"), 1234.0);
    NestedLocation->SetNumberField(TEXT("y"), 2345.0);
    NestedLocation->SetNumberField(TEXT("z"), 3456.0);
    NestedPayload->SetObjectField(TEXT("location"), NestedLocation);
    const FVector BeforeNested = Actor->GetActorLocation();
    FTestResponseCapture NestedCapture;
    PinWrightWorldPreconditionTest::FScopedNestedDispatcher NestedDispatcherScope(
        NestedDispatcher, NestedCapture);
    NestedDispatcher.ProcessRequest(TEXT("_test-nested"),
        TEXT("_test.world_nested_dispatch"), NestedPayload);
    TestTrue(TEXT("nested DispatchMethod rejects a stale expectWorld before mutation"),
        Actor->GetActorLocation().Equals(BeforeNested, KINDA_SMALL_NUMBER));
    TestTrue(TEXT("nested DispatchMethod reports a response"), NestedCapture.bWasCalled);
    TestFalse(TEXT("nested DispatchMethod reports failure"), NestedCapture.bSuccess);
    TestEqual(TEXT("nested DispatchMethod uses WORLD_MISMATCH"), NestedCapture.ErrorCode,
        FString(ErrorCodes::ERR_WORLD_MISMATCH));
    TestEqual(TEXT("nested mismatch payload names the actual world"),
        PinWrightWorldPreconditionTest::ReadString(
            NestedCapture.Result, TEXT("world")), ActualWorld);
    TestEqual(TEXT("nested mismatch payload names the expected world"),
        PinWrightWorldPreconditionTest::ReadString(
            NestedCapture.Result, TEXT("expectedWorld")),
        FString(TEXT("/Game/PinWrightTests/NestedWorldThatCannotBeActive")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldPreconditionResponsePolicyTest,
    "PinWright.world.precondition.ResponsePolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldPreconditionResponsePolicyTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = NewObject<UPinWrightSubsystem>();
    if (!TestNotNull(TEXT("response-policy subsystem fixture"), Subsystem))
    {
        return false;
    }

    const FString ExpectedWorld = PinWrightWorldPrecondition::GetActiveWorldId();

    // Distinct IDs model the original request and the additional subscribers
    // drained by blueprint.create's shared in-flight completion fanout.
    TSharedPtr<FJsonObject> OriginalResult = MakeShared<FJsonObject>();
    Subsystem->RegisterResponsePolicyForTesting(TEXT("fanout-original"), true);
    Subsystem->DecorateResponseForTesting(
        TEXT("fanout-original"), /*bSuccess=*/true, OriginalResult);
    TestTrue(TEXT("mutating fanout original gets a world echo"),
        OriginalResult->HasField(TEXT("world")));
    TestEqual(TEXT("mutating fanout original echoes the resolved world"),
        PinWrightWorldPreconditionTest::ReadString(OriginalResult, TEXT("world")), ExpectedWorld);

    TSharedPtr<FJsonObject> SubscriberResult;
    Subsystem->RegisterResponsePolicyForTesting(TEXT("fanout-subscriber"), true);
    Subsystem->DecorateResponseForTesting(
        TEXT("fanout-subscriber"), /*bSuccess=*/true, SubscriberResult);
    TestNotNull(TEXT("a null mutating result becomes an object"), SubscriberResult.Get());
    if (SubscriberResult.IsValid())
    {
        TestTrue(TEXT("mutating fanout subscriber gets a world echo"),
            SubscriberResult->HasField(TEXT("world")));
        TestEqual(TEXT("mutating fanout subscriber echoes the resolved world"),
            PinWrightWorldPreconditionTest::ReadString(SubscriberResult, TEXT("world")), ExpectedWorld);
    }

    TSharedPtr<FJsonObject> FailureResult = MakeShared<FJsonObject>();
    Subsystem->RegisterResponsePolicyForTesting(TEXT("mutating-failure"), true);
    Subsystem->DecorateResponseForTesting(
        TEXT("mutating-failure"), /*bSuccess=*/false, FailureResult);
    TestTrue(TEXT("mutating failures carry a world echo"),
        FailureResult->HasField(TEXT("world")));
    TestEqual(TEXT("mutating failure echoes the resolved world"),
        PinWrightWorldPreconditionTest::ReadString(FailureResult, TEXT("world")), ExpectedWorld);

    TSharedPtr<FJsonObject> ReadOnlyResult = MakeShared<FJsonObject>();
    Subsystem->RegisterResponsePolicyForTesting(TEXT("read-only"), false);
    Subsystem->DecorateResponseForTesting(
        TEXT("read-only"), /*bSuccess=*/true, ReadOnlyResult);
    TestFalse(TEXT("read-only successes do not get a world echo"),
        ReadOnlyResult->HasField(TEXT("world")));

    TSharedPtr<FJsonObject> ReusedIdResult = MakeShared<FJsonObject>();
    Subsystem->RegisterResponsePolicyForTesting(TEXT("reused-id"), true);
    Subsystem->RegisterResponsePolicyForTesting(TEXT("reused-id"), false);
    Subsystem->DecorateResponseForTesting(
        TEXT("reused-id"), /*bSuccess=*/true, ReusedIdResult);
    TestFalse(TEXT("a reused ID does not inherit a stale mutating policy"),
        ReusedIdResult->HasField(TEXT("world")));

    return true;
}

// This exercises the retained RunAtSafePoint continuation rather than only the
// inline dispatcher check. The editor-world context is changed after the work is
// queued, so the continuation must reject the stale expectation and report the
// world that is active when the continuation actually runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldPreconditionDeferredContinuationTest,
    "PinWright.world.precondition.DeferredContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldPreconditionDeferredContinuationTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-required"),
            TEXT("The deferred world-precondition test requires an editor world outside PIE."));
        return true;
    }

    UWorld* OriginalEditorWorld = GEditor->GetEditorWorldContext().World();
    if (!OriginalEditorWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-world-required"),
            TEXT("The deferred world-precondition test requires a current editor world."));
        return true;
    }

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    // UE 5.8's CreateWorld accepts the initialization values as its seventh
    // argument; keep these fixtures explicitly non-partitioned so they cannot
    // enter the World Partition/data-layer path used by the neighboring tests.
    UWorld::InitializationValues TransientWorldInitialization =
        UWorld::InitializationValues().CreateWorldPartition(false);
    UPackage* ExpectedPackage = CreatePackage(*FString::Printf(
        TEXT("/Temp/PinWrightTests/WorldPreconditionExpected_%s"), *Stamp));
    UWorld* ExpectedWorld = ExpectedPackage
        ? UWorld::CreateWorld(
            EWorldType::Editor, false,
            FName(*FString::Printf(TEXT("WorldPreconditionExpected_%s"), *Stamp)),
            ExpectedPackage, /*bAddToRoot=*/false, ERHIFeatureLevel::Num,
            &TransientWorldInitialization)
        : nullptr;
    FScopedTransientWorldGuard ExpectedWorldGuard(ExpectedWorld);

    UPackage* ActualPackage = CreatePackage(*FString::Printf(
        TEXT("/Temp/PinWrightTests/WorldPreconditionActual_%s"), *Stamp));
    UWorld* ActualWorld = ActualPackage
        ? UWorld::CreateWorld(
            EWorldType::Editor, false,
            FName(*FString::Printf(TEXT("WorldPreconditionActual_%s"), *Stamp)),
            ActualPackage, /*bAddToRoot=*/false, ERHIFeatureLevel::Num,
            &TransientWorldInitialization)
        : nullptr;
    FScopedTransientWorldGuard ActualWorldGuard(ActualWorld);

    if (!ExpectedWorld || !ActualWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not create the transient editor worlds for deferred validation."));
        return true;
    }

    GEditor->GetEditorWorldContext().SetCurrentWorld(ExpectedWorld);
    ON_SCOPE_EXIT
    {
        GEditor->GetEditorWorldContext().SetCurrentWorld(OriginalEditorWorld);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("expectWorld"), ExpectedWorld->GetPathName());
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    FHandlerContext Context = FHandlerContext::MakeTestContextWithSharedCapture(
        TEXT("world-precondition-deferred"), TEXT("actor.set_transform"), Payload, Capture);
    TSharedRef<int32> WorkRuns = MakeShared<int32>(0);

    PinWrightSafePoint::SetForcedUnsafeForTests(true);
    const bool bQueued = PinWrightSafePoint::RunAtSafePoint(
        Context, TEXT("world-precondition-deferred"),
        [WorkRuns](const PinWrightSafePoint::FSafePointResponder& Responder)
        {
            ++(*WorkRuns);
            Responder.SendSuccess(MakeShared<FJsonObject>());
        });
    PinWrightSafePoint::SetForcedUnsafeForTests(false);

    TestTrue(TEXT("RunAtSafePoint accepts the retained continuation"), bQueued);
    TestEqual(TEXT("the mutating body has not run before the safe-point hop"), *WorkRuns, 0);
    TestFalse(TEXT("the retained continuation has not answered before the safe-point hop"),
        Capture->bWasCalled);

    GEditor->GetEditorWorldContext().SetCurrentWorld(ActualWorld);
    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestEqual(TEXT("the stale-world continuation is refused before the body"), *WorkRuns, 0);
    TestTrue(TEXT("the retained continuation reports a response"), Capture->bWasCalled);
    TestFalse(TEXT("the retained continuation reports failure"), Capture->bSuccess);
    TestEqual(TEXT("the retained continuation uses WORLD_MISMATCH"), Capture->ErrorCode,
        FString(ErrorCodes::ERR_WORLD_MISMATCH));
    TestEqual(TEXT("the error data carries the world active at continuation time"),
        PinWrightWorldPreconditionTest::ReadString(Capture->Result, TEXT("world")),
        ActualWorld->GetPathName());
    TestEqual(TEXT("the error data carries the original expected world"),
        PinWrightWorldPreconditionTest::ReadString(Capture->Result, TEXT("expectedWorld")),
        ExpectedWorld->GetPathName());

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
