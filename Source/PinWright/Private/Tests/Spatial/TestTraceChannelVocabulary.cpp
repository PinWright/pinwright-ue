// Copyright (c) 2026 Alexander Penkin. MIT License.

// The one trace-channel vocabulary (SpatialTraceUtils::ParseTraceChannel) shared by
// spatial.raycast, spatial.raycast_screen and the ground verbs' surface.channel. It used to be
// four hand-kept copies of visibility/camera/worldstatic/worlddynamic, so a Pawn trace - what a
// walking character collides with - was refused on every verb.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Handlers/Spatial/SpatialTraceUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/ActorUtils.h"

#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace TraceChannelVocabularyTests
{
    TSharedPtr<FJsonObject> Vec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // Same reason as TestRaycastHandlers.cpp: the editor world does not tick physics, so a body
    // registered (or re-filtered) on this callstack reaches the query structure only on a tick.
    void FlushEditorPhysics(UWorld* World)
    {
        if (!World || World->bInTick)
        {
            return;
        }
        for (int32 Iteration = 0; Iteration < 2; ++Iteration)
        {
            World->Tick(LEVELTICK_All, 1.0f / 60.0f);
        }
    }

    FTestResponseCapture Raycast(double X, double Y, const TCHAR* Channel)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), Vec(X, Y, 500.0));
        Payload->SetObjectField(TEXT("direction"), Vec(0.0, 0.0, -1.0));
        Payload->SetNumberField(TEXT("maxDistance"), 1000.0);
        Payload->SetStringField(TEXT("channel"), Channel);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture);
        return Capture;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceChannelVocabularyResolvesTest,
    "PinWright.spatial.trace_channel.StockAndProjectChannelsResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceChannelVocabularyResolvesTest::RunTest(const FString& Parameters)
{
    using SpatialTraceUtils::ParseTraceChannel;

    const TPair<const TCHAR*, ECollisionChannel> Stock[] = {
        {TEXT("visibility"), ECC_Visibility}, {TEXT("camera"), ECC_Camera},
        {TEXT("worldstatic"), ECC_WorldStatic}, {TEXT("worlddynamic"), ECC_WorldDynamic},
        {TEXT("pawn"), ECC_Pawn}, {TEXT("Pawn"), ECC_Pawn}, {TEXT("PAWN"), ECC_Pawn},
        {TEXT("physicsbody"), ECC_PhysicsBody}, {TEXT("vehicle"), ECC_Vehicle},
        {TEXT("destructible"), ECC_Destructible}};
    for (const TPair<const TCHAR*, ECollisionChannel>& Row : Stock)
    {
        ECollisionChannel Got = ECC_MAX;
        TestTrue(FString::Printf(TEXT("'%s' is accepted"), Row.Key), ParseTraceChannel(Row.Key, Got));
        TestEqual(FString::Printf(TEXT("'%s' resolves to its channel"), Row.Key),
            static_cast<int32>(Got), static_cast<int32>(Row.Value));
    }

    // Every slot the project named (DefaultChannelResponses) resolves by that name, and every
    // slot it did not name stays unreachable by its placeholder enum name.
    const UCollisionProfile* Profile = UCollisionProfile::Get();
    const UEnum* Enum = StaticEnum<ECollisionChannel>();
    for (int32 Index = ECC_Destructible + 1; Index < ECC_OverlapAll_Deprecated; ++Index)
    {
        const FString Name = Profile->ReturnChannelNameFromContainerIndex(Index).ToString();
        const bool bNamed = Enum->GetNameStringByValue(Index).RightChop(4) != Name;
        ECollisionChannel Got = ECC_MAX;
        const bool bAccepted = ParseTraceChannel(Name, Got);
        TestEqual(FString::Printf(TEXT("slot %d '%s' accepted iff the project named it"), Index, *Name),
            bAccepted, bNamed);
        if (bNamed)
        {
            TestEqual(FString::Printf(TEXT("named slot '%s' resolves to slot %d"), *Name, Index),
                static_cast<int32>(Got), Index);
        }
    }

    ECollisionChannel Unused = ECC_MAX;
    TestFalse(TEXT("an unknown name is refused"), ParseTraceChannel(TEXT("NotAChannel"), Unused));
    TestTrue(TEXT("the error vocabulary lists Pawn"),
        SpatialTraceUtils::TraceChannelNames().Contains(TEXT("Pawn")));
    return true;
}

// The issue's case: a component that blocks Visibility but ignores Pawn (or the reverse) is
// invisible to a Visibility probe. Only a Pawn trace answers "would a character hit this".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceChannelRaycastPawnTest,
    "PinWright.spatial.raycast.PawnChannelSeesWhatVisibilityDoesNot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceChannelRaycastPawnTest::RunTest(const FString& Parameters)
{
    using namespace TraceChannelVocabularyTests;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping the pawn-channel raycast test."));
        return true;
    }

    const double ColumnX = 131070.0;
    const double ColumnY = -75310.0;
    const FString Label = FString::Printf(TEXT("PW_PawnChannel_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    SpawnPayload->SetObjectField(TEXT("location"), Vec(ColumnX, ColumnY, 0.0));
    FTestResponseCapture SpawnCapture;
    InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture);

    AActor* Spawned = McpActorUtils::FindActorByName(World, Label);
    ON_SCOPE_EXIT
    {
        if (Spawned)
        {
            Spawned->Destroy();
        }
    };
    UPrimitiveComponent* Prim = Spawned ? Cast<UPrimitiveComponent>(Spawned->GetRootComponent()) : nullptr;
    if (!TestTrue(TEXT("precondition: cube spawned with a primitive root"), SpawnCapture.bSuccess && Prim))
    {
        return true;
    }

    Prim->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    Prim->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    FlushEditorPhysics(World);

    const FTestResponseCapture Visibility = Raycast(ColumnX, ColumnY, TEXT("visibility"));
    TestTrue(TEXT("visibility raycast succeeds"), Visibility.bSuccess);
    bool bVisHit = true;
    if (Visibility.Result.IsValid())
    {
        Visibility.Result->TryGetBoolField(TEXT("hit"), bVisHit);
    }
    TestFalse(TEXT("precondition: a visibility probe misses the visibility-ignoring cube"), bVisHit);

    const FTestResponseCapture Pawn = Raycast(ColumnX, ColumnY, TEXT("pawn"));
    TestTrue(FString::Printf(TEXT("channel 'pawn' is accepted (error: %s %s)"),
        *Pawn.ErrorCode, *Pawn.Message), Pawn.bSuccess);
    if (Pawn.bSuccess && Pawn.Result.IsValid())
    {
        bool bPawnHit = false;
        Pawn.Result->TryGetBoolField(TEXT("hit"), bPawnHit);
        TestTrue(TEXT("the pawn trace hits the cube a character would collide with"), bPawnHit);
        const TSharedPtr<FJsonObject>* ActorObj = nullptr;
        FString HitName;
        if (Pawn.Result->TryGetObjectField(TEXT("actor"), ActorObj) && ActorObj)
        {
            (*ActorObj)->TryGetStringField(TEXT("name"), HitName);
        }
        TestEqual(TEXT("the pawn trace hit the spawned cube"), HitName, Label);
        TestEqual(TEXT("channel echo"), Pawn.Result->GetStringField(TEXT("channel")), FString(TEXT("pawn")));
    }

    const FTestResponseCapture Bad = Raycast(ColumnX, ColumnY, TEXT("NotAChannel"));
    TestEqual(TEXT("unknown channel -> INVALID_PARAMS"), Bad.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(TEXT("the refusal lists this project's channels, Pawn among them"),
        Bad.Message.Contains(TEXT("Pawn")));
    return true;
}

// raycast_screen and both surface parsers (the handler's and the shared ParseSurfaceJson used by
// foliage and level.audit) take the same vocabulary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceChannelOtherVerbsTest,
    "PinWright.spatial.trace_channel.ScreenAndSurfaceParsersShareTheVocabulary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceChannelOtherVerbsTest::RunTest(const FString& Parameters)
{
    // raycast_screen validates the channel before it deprojects, so whatever the viewport state,
    // 'pawn' must not come back as an unknown channel, and a bogus one must list Pawn.
    auto Screen = [](const TCHAR* Channel)
    {
        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), -90.0);
        Rotation->SetNumberField(TEXT("yaw"), 0.0);
        Rotation->SetNumberField(TEXT("roll"), 0.0);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("location"), TraceChannelVocabularyTests::Vec(0.0, 0.0, 1000.0));
        Payload->SetObjectField(TEXT("rotation"), Rotation);
        Payload->SetNumberField(TEXT("x"), 10.0);
        Payload->SetNumberField(TEXT("y"), 10.0);
        Payload->SetNumberField(TEXT("width"), 64.0);
        Payload->SetNumberField(TEXT("height"), 64.0);
        Payload->SetStringField(TEXT("channel"), Channel);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("spatial.raycast_screen"), Payload, Capture);
        return Capture;
    };
    const FTestResponseCapture ScreenPawn = Screen(TEXT("pawn"));
    TestTrue(TEXT("precondition: raycast_screen answered"), ScreenPawn.bWasCalled);
    TestNotEqual(FString::Printf(TEXT("raycast_screen accepts 'pawn' (got: %s %s)"),
        *ScreenPawn.ErrorCode, *ScreenPawn.Message), ScreenPawn.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    const FTestResponseCapture ScreenBad = Screen(TEXT("NotAChannel"));
    TestEqual(TEXT("raycast_screen: unknown channel -> INVALID_PARAMS"),
        ScreenBad.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(TEXT("raycast_screen refusal lists Pawn"), ScreenBad.Message.Contains(TEXT("Pawn")));

    // The handler-side surface parser (spatial.verify_grounding / ground_actors / ground_instances).
    auto Verify = [](const TCHAR* Channel)
    {
        TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
        Surface->SetStringField(TEXT("preset"), TEXT("landscape"));
        Surface->SetStringField(TEXT("channel"), Channel);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("surface"), Surface);
        TArray<TSharedPtr<FJsonValue>> Actors;
        Actors.Add(MakeShared<FJsonValueString>(TEXT("PW_NoSuchActor_TraceChannel")));
        Payload->SetArrayField(TEXT("actors"), Actors);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("spatial.verify_grounding"), Payload, Capture);
        return Capture;
    };
    const FTestResponseCapture VerifyPawn = Verify(TEXT("pawn"));
    TestTrue(TEXT("precondition: verify_grounding answered"), VerifyPawn.bWasCalled);
    TestNotEqual(FString::Printf(TEXT("verify_grounding accepts surface.channel 'pawn' (got: %s %s)"),
        *VerifyPawn.ErrorCode, *VerifyPawn.Message), VerifyPawn.ErrorCode, FString(TEXT("INVALID_SURFACE_SPEC")));
    const FTestResponseCapture VerifyBad = Verify(TEXT("NotAChannel"));
    TestEqual(TEXT("verify_grounding: unknown channel -> INVALID_SURFACE_SPEC"),
        VerifyBad.ErrorCode, FString(TEXT("INVALID_SURFACE_SPEC")));
    TestTrue(TEXT("verify_grounding refusal lists Pawn"), VerifyBad.Message.Contains(TEXT("Pawn")));

    // The shared parser.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TSharedPtr<FJsonObject> Surface = MakeShared<FJsonObject>();
    Surface->SetStringField(TEXT("preset"), TEXT("landscape"));
    Surface->SetStringField(TEXT("channel"), TEXT("Pawn"));
    GroundPlacement::FGroundSurfaceSpec Spec;
    TArray<FString> Unresolved;
    FString Error;
    const bool bParsed = GroundPlacement::ParseSurfaceJson(Surface, World, Spec, Unresolved, Error);
    TestTrue(FString::Printf(TEXT("ParseSurfaceJson accepts 'Pawn' (error: %s)"), *Error), bParsed);
    TestEqual(TEXT("ParseSurfaceJson resolves Pawn"),
        static_cast<int32>(Spec.Channel), static_cast<int32>(ECC_Pawn));
    return true;
}
