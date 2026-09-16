// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for board E-create-procedural-terrain-no-label-set.
//
// environment.build.create_procedural_terrain accepts `actorName` and applies it to
// the spawned actor's INTERNAL object name (SpawnParams.Name, NameMode=Requested) but
// never calls SetActorLabel. Consequences the correct behavior must fix:
//   1. The World Outliner display label falls back to the generic class default
//      "Actor" instead of the requested name.
//   2. The success response's `actorName` is misreported: the handler writes
//      GetName() at EnvironmentHandler.cpp:1210, but AddActorVerification
//      (AssetUtils.cpp:1056) then overwrites it with GetActorLabel() — which, with no
//      label ever set, is the generic "Actor" (not the name the caller passed).
// The sibling create verbs create_sky_sphere / create_fog_volume route through
// SpawnActorInActiveWorld(..., Label) which DOES SetActorLabel; only this verb skips it.
//
// This test asserts the CORRECT behavior (the requested name reaches both the actor
// label and the echoed `actorName`). Pre-fix it fails because both surfaces report the
// generic "Actor"; the fix (add TerrainActor->SetActorLabel(ActorName) after spawn)
// flips it green. GetActorLabel() is the same production symbol the response echo uses,
// so the label assertion tracks the exact defect the ticket describes.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "GameFramework/Actor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/Guid.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentCreateProceduralTerrainLabelsActorTest,
    "PinWright.environment.build.create_procedural_terrain.LabelsActorWithRequestedName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentCreateProceduralTerrainLabelsActorTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    // A missing editor world is a fixture failure, not a skip — the handler spawns into
    // the active editor world, so without one there is nothing to assert.
    TestNotNull(TEXT("editor world available for terrain spawn"), World);
    if (!World)
    {
        return false;
    }

    // A GUID-unique requested name that cannot equal the generic class default "Actor",
    // and cannot collide with a pre-existing actor's object name (so NameMode=Requested
    // dedup never touches the OBJECT name — the label defect is what we are isolating).
    const FString RequestedName = FString::Printf(TEXT("PW_MarshTerrain_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), RequestedName);
    // Small grid keeps the headless mesh build fast (matches the sibling material-echo test).
    Payload->SetNumberField(TEXT("subdivisions"), 2.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("create_procedural_terrain handler is registered"),
        InvokeHandlerWithCapture(TEXT("environment.build.create_procedural_terrain"), Payload, Capture));
    TestTrue(TEXT("create_procedural_terrain succeeded"), Capture.bSuccess);

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // Resolve the spawned actor via its object path so the assertions inspect the exact
    // actor returned by the handler. WorldGuard owns teardown on every exit path.
    FString ActorPath;
    Capture.Result->TryGetStringField(TEXT("actorPath"), ActorPath);
    AActor* TerrainActor = ActorPath.IsEmpty() ? nullptr : FindObject<AActor>(nullptr, *ActorPath);

    // Consequence #2 — the success response must echo the name the caller passed, not
    // the generic label. Pre-fix `actorName` is "Actor" (GetActorLabel of an unlabelled
    // actor), so this TestEqual fails.
    FString EchoedName;
    TestTrue(TEXT("response carries actorName"),
        Capture.Result->TryGetStringField(TEXT("actorName"), EchoedName));
    TestEqual(TEXT("response actorName echoes the requested name (not the generic 'Actor')"),
        EchoedName, RequestedName);

    // Consequence #1 — the World Outliner display label must be the requested name.
    // GetActorLabel() is the exact production symbol AddActorVerification reads back, so
    // asserting it here proves the outliner-label fix, not just a response-shaping one.
    TestNotNull(TEXT("spawned terrain actor resolvable by actorPath"), TerrainActor);
    if (TerrainActor)
    {
        TestEqual(TEXT("outliner label is the requested name (SetActorLabel was applied)"),
            TerrainActor->GetActorLabel(), RequestedName);
    }

    return true;
}
