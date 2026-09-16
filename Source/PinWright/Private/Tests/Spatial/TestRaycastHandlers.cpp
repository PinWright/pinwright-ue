// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for spatial.raycast (world line-trace RPC + SpatialTraceUtils).
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Utils/ActorUtils.h"

#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace
{
    // Builds a {x,y,z} JSON object for a request payload.
    TSharedPtr<FJsonObject> RaycastTestVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    UWorld* RaycastTestEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // The editor world does not tick physics on its own, so a body registered on
    // this callstack (the just-spawned cube) is not yet in the scene-query
    // acceleration structure - Chaos defers the external-structure update to a
    // physics scene flush, which only happens inside a world tick. Tick the world a
    // couple of frames to flush pending registrations so an immediate trace can see
    // the new body. Editor worlds don't simulate, so the cube does not move.
    void RaycastTestFlushEditorPhysics(UWorld* World)
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
}

// (a) Spawn a cube floor, raycast straight down onto it, assert a hit at the cube's
// top face (~Z=50 for the 100cm basic Cube centered at Z=0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastHitsFloorTest,
    "PinWright.spatial.raycast.HitsFloor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastHitsFloorTest::RunTest(const FString& Parameters)
{
    UWorld* World = RaycastTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping raycast-hit test."));
        return true;
    }

    // Spawn a cube at an isolated column so no other level geometry sits under the ray.
    const double ColumnX = 123450.0;
    const double ColumnY = 67890.0;
    const FString Label = FString::Printf(TEXT("PW_RaycastFloor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
    SpawnPayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    SpawnPayload->SetStringField(TEXT("actorName"), Label);
    SpawnPayload->SetObjectField(TEXT("location"), RaycastTestVec(ColumnX, ColumnY, 0.0));

    FTestResponseCapture SpawnCapture;
    const bool bSpawnFound = InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture);
    TestTrue(TEXT("actor.spawn handler registered"), bSpawnFound);
    TestTrue(TEXT("cube floor spawned"), SpawnCapture.bSuccess);

    AActor* Spawned = McpActorUtils::FindActorByName(World, Label);
    ON_SCOPE_EXIT
    {
        if (Spawned)
        {
            Spawned->Destroy();
        }
    };

    if (!SpawnCapture.bSuccess)
    {
        // Can't validate a hit without the floor; the spawn assertion above already failed.
        return true;
    }

    // Flush the just-registered cube body into the scene-query structure.
    RaycastTestFlushEditorPhysics(World);

    TSharedPtr<FJsonObject> RayPayload = MakeShared<FJsonObject>();
    RayPayload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 500.0));
    RayPayload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
    RayPayload->SetNumberField(TEXT("maxDistance"), 1000.0);

    FTestResponseCapture RayCapture;
    TestTrue(TEXT("spatial.raycast handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast"), RayPayload, RayCapture));
    TestTrue(TEXT("raycast responded with success"), RayCapture.bSuccess);

    if (RayCapture.bSuccess && RayCapture.Result.IsValid())
    {
        bool bHit = false;
        RayCapture.Result->TryGetBoolField(TEXT("hit"), bHit);
        TestTrue(TEXT("ray hit the cube floor"), bHit);

        const TSharedPtr<FJsonObject>* LocationObj = nullptr;
        if (bHit && RayCapture.Result->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj)
        {
            double HitZ = 0.0;
            (*LocationObj)->TryGetNumberField(TEXT("z"), HitZ);
            TestTrue(TEXT("impact is at the cube's top face (~Z=50)"),
                FMath::Abs(HitZ - 50.0) < 2.0);
        }

        // The units/axis convention echo must be present so callers can interpret the hit.
        FString Units;
        TestTrue(TEXT("result echoes units"), RayCapture.Result->TryGetStringField(TEXT("units"), Units));
        TestEqual(TEXT("units are cm"), Units, FString(TEXT("cm")));
    }

    return true;
}

// (b) A ray into empty space is a successful miss (hit:false), not an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastMissesEmptySpaceTest,
    "PinWright.spatial.raycast.MissesEmptySpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastMissesEmptySpaceTest::RunTest(const FString& Parameters)
{
    if (!RaycastTestEditorWorld())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping raycast-miss test."));
        return true;
    }

    TSharedPtr<FJsonObject> RayPayload = MakeShared<FJsonObject>();
    RayPayload->SetObjectField(TEXT("origin"), RaycastTestVec(1.0e6, 1.0e6, 1.0e6));
    RayPayload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, 1.0));
    RayPayload->SetNumberField(TEXT("maxDistance"), 100.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.raycast handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast"), RayPayload, Capture));
    TestTrue(TEXT("a miss is reported as success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bHit = true;
        Capture.Result->TryGetBoolField(TEXT("hit"), bHit);
        TestFalse(TEXT("empty-space ray reports hit:false"), bHit);
    }

    return true;
}

// (c) Missing origin -> MISSING_REQUIRED_PARAM; origin without direction/target -> INVALID_PARAMS.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastMissingArgsTest,
    "PinWright.spatial.raycast.MissingArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastMissingArgsTest::RunTest(const FString& Parameters)
{
    // No origin at all.
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (no-origin)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), MakeShared<FJsonObject>(), Capture));
        TestFalse(TEXT("missing origin is an error"), Capture.bSuccess);
        TestEqual(TEXT("missing origin -> MISSING_REQUIRED_PARAM"),
            Capture.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    }

    // Origin present but neither direction nor target.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(0.0, 0.0, 0.0));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (no-direction)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestFalse(TEXT("missing direction/target is an error"), Capture.bSuccess);
        TestEqual(TEXT("missing direction/target -> INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    return true;
}

// (d) An unknown argument is rejected by the dispatcher's auto-validation with
// UNKNOWN_PARAMS. This path only runs through the real dispatcher, not the direct
// InvokeHandlerWithCapture bypass, so drive it via a wired dispatcher fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastUnknownArgRejectedTest,
    "PinWright.spatial.raycast.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastUnknownArgRejectedTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetObjectField(TEXT("origin"), RaycastTestVec(0.0, 0.0, 100.0));
    Params->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
    Params->SetStringField(TEXT("bogusParam"), TEXT("nope"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("spatial.raycast"),
        TEXT("req-raycast-unknown"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown param is rejected"), bSuccess);
    TestEqual(TEXT("unknown param -> UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

    return true;
}

// ============================================================================
// Layered / filtered raycast (board ticket B-trace-complex-hits-render-geometry)
//
// The shipped verb returned only the first blocking hit and gave a caller no way to
// say "I only care about the ground", so a downward probe silently reported the
// height of whatever was in front of the terrain. These tests pin the three pieces
// of the fix: multiHit sees PAST a blocking surface, the actor filters pick the hit
// the caller actually wants (and report what they peeled), and the legacy
// single-hit path is untouched.
//
// The live repro used a collisionless foliage mesh under traceComplex:true, which is
// not reproducible from an engine asset here (every /Engine/BasicShapes mesh ships
// simple collision, and mutating a shared engine BodySetup would corrupt it for the
// rest of the session). The capability under test is representation-agnostic: what
// broke the probe was a BLOCKING hit in front of the wanted one, so two stacked
// cubes exercise exactly the same peel/filter path. The collisionless-mesh case
// additionally needs runtime verification against real foliage.
// ============================================================================
namespace
{
    // Spawns a basic cube via actor.spawn and returns the placed actor (null on failure).
    // Label carries a GUID so SetActorLabel never disambiguates with a "_N" suffix.
    AActor* RaycastTestSpawnCube(FAutomationTestBase& Test, UWorld* World,
        const FString& Label, double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        SpawnPayload->SetStringField(TEXT("actorName"), Label);
        SpawnPayload->SetObjectField(TEXT("location"), RaycastTestVec(X, Y, Z));

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("actor.spawn handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("spawned %s"), *Label), Capture.bSuccess);
        return Capture.bSuccess ? McpActorUtils::FindActorByName(World, Label) : nullptr;
    }

    // Reads hits[i].actor.name out of a raycast response, or an empty string.
    FString RaycastTestHitActorName(const TSharedPtr<FJsonObject>& Result, int32 Index)
    {
        if (!Result.IsValid())
        {
            return FString();
        }
        const TArray<TSharedPtr<FJsonValue>>* Hits = nullptr;
        if (!Result->TryGetArrayField(TEXT("hits"), Hits) || !Hits || !Hits->IsValidIndex(Index))
        {
            return FString();
        }
        const TSharedPtr<FJsonObject>* HitObj = nullptr;
        if (!(*Hits)[Index]->TryGetObject(HitObj) || !HitObj)
        {
            return FString();
        }
        const TSharedPtr<FJsonObject>* ActorObj = nullptr;
        FString Name;
        if ((*HitObj)->TryGetObjectField(TEXT("actor"), ActorObj) && ActorObj)
        {
            (*ActorObj)->TryGetStringField(TEXT("name"), Name);
        }
        return Name;
    }

    // Reads the top-level single-hit actor.name out of a raycast response.
    FString RaycastTestTopActorName(const TSharedPtr<FJsonObject>& Result)
    {
        const TSharedPtr<FJsonObject>* ActorObj = nullptr;
        FString Name;
        if (Result.IsValid() && Result->TryGetObjectField(TEXT("actor"), ActorObj) && ActorObj)
        {
            (*ActorObj)->TryGetStringField(TEXT("name"), Name);
        }
        return Name;
    }
}

// (e) multiHit walks PAST a blocking surface: a downward ray through two stacked cubes
// reports both, nearest first. UWorld::LineTraceMultiByChannel cannot do this (it stops
// at the first blocking hit), which is why the handler peels and re-traces.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastMultiHitSeesPastBlockerTest,
    "PinWright.spatial.raycast.MultiHitSeesPastBlocker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastMultiHitSeesPastBlockerTest::RunTest(const FString& Parameters)
{
    UWorld* World = RaycastTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping multi-hit raycast test."));
        return true;
    }

    // Isolated column, distinct from the HitsFloor test's, so no other level geometry
    // (or a sibling fixture) sits under the ray.
    const double ColumnX = 223450.0;
    const double ColumnY = 67890.0;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString UpperLabel = FString::Printf(TEXT("PW_RayUpper_%s"), *Suffix);
    const FString LowerLabel = FString::Printf(TEXT("PW_RayLower_%s"), *Suffix);

    AActor* Upper = RaycastTestSpawnCube(*this, World, UpperLabel, ColumnX, ColumnY, 300.0);
    AActor* Lower = RaycastTestSpawnCube(*this, World, LowerLabel, ColumnX, ColumnY, 0.0);
    ON_SCOPE_EXIT
    {
        if (Upper) { Upper->Destroy(); }
        if (Lower) { Lower->Destroy(); }
    };
    if (!Upper || !Lower)
    {
        return true; // the spawn assertions above already failed
    }

    RaycastTestFlushEditorPhysics(World);

    TSharedPtr<FJsonObject> RayPayload = MakeShared<FJsonObject>();
    RayPayload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 1000.0));
    RayPayload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
    RayPayload->SetNumberField(TEXT("maxDistance"), 2000.0);
    RayPayload->SetBoolField(TEXT("multiHit"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("spatial.raycast handler registered"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast"), RayPayload, Capture));
    TestTrue(TEXT("multi-hit raycast succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Hits = nullptr;
    const bool bHasHits = Capture.Result->TryGetArrayField(TEXT("hits"), Hits);
    TestTrue(TEXT("multiHit response carries a hits[] array"), bHasHits && Hits != nullptr);

    // The whole point: the ray did NOT stop at the first blocking cube.
    TestTrue(TEXT("multiHit reports both stacked cubes (>= 2 hits)"),
        bHasHits && Hits && Hits->Num() >= 2);

    if (bHasHits && Hits && Hits->Num() >= 2)
    {
        TestEqual(TEXT("nearest hit is the upper cube"),
            RaycastTestHitActorName(Capture.Result, 0), UpperLabel);
        TestEqual(TEXT("second hit is the lower cube"),
            RaycastTestHitActorName(Capture.Result, 1), LowerLabel);

        // Ordering contract: hits are nearest-first.
        const TSharedPtr<FJsonObject>* First = nullptr;
        const TSharedPtr<FJsonObject>* Second = nullptr;
        if ((*Hits)[0]->TryGetObject(First) && (*Hits)[1]->TryGetObject(Second) && First && Second)
        {
            double D0 = 0.0;
            double D1 = 0.0;
            (*First)->TryGetNumberField(TEXT("distance"), D0);
            (*Second)->TryGetNumberField(TEXT("distance"), D1);
            TestTrue(TEXT("hits are ordered nearest-first"), D0 < D1);
        }
    }

    // The top-level single-hit fields still describe hits[0], so a caller that flips
    // multiHit on does not have to restructure its read.
    TestEqual(TEXT("top-level actor still describes the nearest hit"),
        RaycastTestTopActorName(Capture.Result), UpperLabel);

    // Two cubes then a miss: the walk completed, it did not run out of budget.
    bool bTruncated = true;
    Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
    TestFalse(TEXT("a completed walk is not truncated"), bTruncated);

    return true;
}

// (f) actorFilter picks the hit the caller actually wants and REPORTS what it peeled.
// This is the B10 workaround expressed as one call: "give me the ground, not whatever
// is standing on it".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastActorFilterSelectsWantedHitTest,
    "PinWright.spatial.raycast.ActorFilterSelectsWantedHit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastActorFilterSelectsWantedHitTest::RunTest(const FString& Parameters)
{
    UWorld* World = RaycastTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping raycast filter test."));
        return true;
    }

    const double ColumnX = 323450.0;
    const double ColumnY = 67890.0;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString UpperLabel = FString::Printf(TEXT("PW_RayBlocker_%s"), *Suffix);
    const FString LowerLabel = FString::Printf(TEXT("PW_RayGround_%s"), *Suffix);

    AActor* Upper = RaycastTestSpawnCube(*this, World, UpperLabel, ColumnX, ColumnY, 300.0);
    AActor* Lower = RaycastTestSpawnCube(*this, World, LowerLabel, ColumnX, ColumnY, 0.0);
    ON_SCOPE_EXIT
    {
        if (Upper) { Upper->Destroy(); }
        if (Lower) { Lower->Destroy(); }
    };
    if (!Upper || !Lower)
    {
        return true;
    }

    RaycastTestFlushEditorPhysics(World);

    // (f1) actorFilter naming the FAR cube must skip the near one, not report a miss.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        Payload->SetNumberField(TEXT("maxDistance"), 2000.0);
        Payload->SetStringField(TEXT("actorFilter"), LowerLabel);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (actorFilter)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestTrue(TEXT("filtered raycast succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bHit = false;
            Capture.Result->TryGetBoolField(TEXT("hit"), bHit);
            TestTrue(TEXT("filtered raycast still hits (the blocker is peeled, not fatal)"), bHit);
            TestEqual(TEXT("filtered raycast returns the requested actor"),
                RaycastTestTopActorName(Capture.Result), LowerLabel);

            // Filtered-away geometry is reported, never silently swallowed.
            const TArray<TSharedPtr<FJsonValue>>* Rejected = nullptr;
            const bool bHasRejects = Capture.Result->TryGetArrayField(TEXT("filteredOut"), Rejected);
            TestTrue(TEXT("filteredOut names the peeled blocker"),
                bHasRejects && Rejected && Rejected->Num() >= 1
                    && (*Rejected)[0]->AsString() == UpperLabel);

            // A single-hit (non-multiHit) filtered call keeps the legacy shape: no hits[].
            TestFalse(TEXT("filtered single-hit response has no hits[] array"),
                Capture.Result->HasField(TEXT("hits")));
        }
    }

    // (f2) onlyActors is the exact-actor form of the same restriction.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        Payload->SetNumberField(TEXT("maxDistance"), 2000.0);
        TArray<TSharedPtr<FJsonValue>> Only;
        Only.Add(MakeShared<FJsonValueString>(LowerLabel));
        Payload->SetArrayField(TEXT("onlyActors"), Only);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (onlyActors)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestTrue(TEXT("onlyActors raycast succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TestEqual(TEXT("onlyActors returns the requested actor"),
                RaycastTestTopActorName(Capture.Result), LowerLabel);
        }
    }

    // (f2b) actorFilter honours the shared matchMode vocabulary (Utils/NameMatchFilter.h),
    // so this verb cannot drift from actor.list's semantics: an "exact" match against a
    // mere PREFIX of the label must select nothing, where the default "contains" would.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        Payload->SetNumberField(TEXT("maxDistance"), 2000.0);
        Payload->SetStringField(TEXT("actorFilter"), TEXT("PW_RayGround_"));
        Payload->SetStringField(TEXT("matchMode"), TEXT("exact"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (matchMode exact)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestTrue(TEXT("matchMode raycast succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bHit = true;
            Capture.Result->TryGetBoolField(TEXT("hit"), bHit);
            TestFalse(TEXT("matchMode:exact against a prefix selects nothing"), bHit);

            // The resolved semantics are echoed, so a caller can see which rule ran.
            FString EchoedMode;
            TestTrue(TEXT("response echoes the resolved matchMode"),
                Capture.Result->TryGetStringField(TEXT("matchMode"), EchoedMode));
            TestEqual(TEXT("echoed matchMode is the canonical spelling"),
                EchoedMode, FString(TEXT("exact")));
        }
    }

    // (f3) ignoreActors excludes the named actor outright (engine-level ignore list):
    // the same ray now lands on the far cube, and ignoring BOTH is a clean miss.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        Payload->SetNumberField(TEXT("maxDistance"), 2000.0);
        TArray<TSharedPtr<FJsonValue>> Ignore;
        Ignore.Add(MakeShared<FJsonValueString>(UpperLabel));
        Payload->SetArrayField(TEXT("ignoreActors"), Ignore);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (ignoreActors)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestTrue(TEXT("ignoreActors raycast succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TestEqual(TEXT("ignored actor is excluded; the ray lands on the next one"),
                RaycastTestTopActorName(Capture.Result), LowerLabel);
        }
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        Payload->SetNumberField(TEXT("maxDistance"), 2000.0);
        TArray<TSharedPtr<FJsonValue>> Ignore;
        Ignore.Add(MakeShared<FJsonValueString>(UpperLabel));
        Ignore.Add(MakeShared<FJsonValueString>(LowerLabel));
        Payload->SetArrayField(TEXT("ignoreActors"), Ignore);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (ignore both)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        // Asserted OUTSIDE the guard, matching every sibling block above: without it a
        // handler that started erroring on a two-entry ignoreActors array would skip the
        // clean-miss assertion below and leave this case green with nothing checked.
        TestTrue(TEXT("ignore-both raycast succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bHit = true;
            Capture.Result->TryGetBoolField(TEXT("hit"), bHit);
            TestFalse(TEXT("ignoring every actor on the ray is a clean miss"), bHit);
        }
    }

    return true;
}

// (g) The legacy single-hit path is unchanged: no multiHit / filter params means one
// trace, the historical top-level shape, and NO hits[]/count/truncated/filteredOut
// keys. The additive per-hit diagnostics (traceComplex echo, simpleCollisionShapes)
// are asserted here too, since they are what makes the render-geometry trap visible.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastLegacyShapeUnchangedTest,
    "PinWright.spatial.raycast.LegacyShapeUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastLegacyShapeUnchangedTest::RunTest(const FString& Parameters)
{
    UWorld* World = RaycastTestEditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping legacy-shape raycast test."));
        return true;
    }

    const double ColumnX = 423450.0;
    const double ColumnY = 67890.0;
    const FString Label = FString::Printf(TEXT("PW_RayLegacy_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    AActor* Cube = RaycastTestSpawnCube(*this, World, Label, ColumnX, ColumnY, 0.0);
    ON_SCOPE_EXIT
    {
        if (Cube) { Cube->Destroy(); }
    };
    if (!Cube)
    {
        return true;
    }

    RaycastTestFlushEditorPhysics(World);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("origin"), RaycastTestVec(ColumnX, ColumnY, 500.0));
    Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
    Payload->SetNumberField(TEXT("maxDistance"), 1000.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered (legacy)"),
        InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
    TestTrue(TEXT("legacy raycast succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    bool bHit = false;
    Capture.Result->TryGetBoolField(TEXT("hit"), bHit);
    TestTrue(TEXT("legacy raycast hits the cube"), bHit);
    TestEqual(TEXT("legacy raycast reports the struck actor"),
        RaycastTestTopActorName(Capture.Result), Label);

    // Multi-hit-only keys must stay absent so an existing consumer sees no new shape.
    TestFalse(TEXT("legacy response has no hits[]"), Capture.Result->HasField(TEXT("hits")));
    TestFalse(TEXT("legacy response has no count"), Capture.Result->HasField(TEXT("count")));
    TestFalse(TEXT("legacy response has no truncated"), Capture.Result->HasField(TEXT("truncated")));
    TestFalse(TEXT("legacy response has no filteredOut"),
        Capture.Result->HasField(TEXT("filteredOut")));
    TestFalse(TEXT("a clean legacy trace emits no warnings"),
        Capture.Result->HasField(TEXT("warnings")));

    // The historical fields are all still there.
    TestTrue(TEXT("legacy response keeps location"), Capture.Result->HasField(TEXT("location")));
    TestTrue(TEXT("legacy response keeps normal"), Capture.Result->HasField(TEXT("normal")));
    TestTrue(TEXT("legacy response keeps distance"), Capture.Result->HasField(TEXT("distance")));
    TestTrue(TEXT("legacy response keeps component"), Capture.Result->HasField(TEXT("component")));

    // Additive diagnostics: the trace mode is echoed, and a cube hit by a SIMPLE trace
    // must report at least one simple collision primitive and must NOT be flagged as a
    // render-geometry hit (only a complex trace can produce that).
    bool bTraceComplexEcho = true;
    TestTrue(TEXT("response echoes traceComplex"),
        Capture.Result->TryGetBoolField(TEXT("traceComplex"), bTraceComplexEcho));
    TestFalse(TEXT("traceComplex echo is false by default"), bTraceComplexEcho);

    double SimpleShapes = -1.0;
    TestTrue(TEXT("response reports simpleCollisionShapes for a static-mesh hit"),
        Capture.Result->TryGetNumberField(TEXT("simpleCollisionShapes"), SimpleShapes));
    TestTrue(TEXT("a cube struck by a simple trace has simple collision"), SimpleShapes >= 1.0);
    TestFalse(TEXT("a simple trace is never a render-geometry hit"),
        Capture.Result->HasField(TEXT("renderGeometryHit")));

    return true;
}

// (h) Filter arguments that cannot possibly match are typed errors, not silent misses:
// an onlyActors list where nothing resolves would report hit:false for every ray, and
// maxHits < 1 would make the walk a guaranteed no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRaycastFilterTypedErrorsTest,
    "PinWright.spatial.raycast.FilterTypedErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRaycastFilterTypedErrorsTest::RunTest(const FString& Parameters)
{
    if (!RaycastTestEditorWorld())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available; skipping raycast filter-error test."));
        return true;
    }

    // onlyActors naming nothing that exists -> ACTOR_NOT_FOUND (never a quiet hit:false).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(0.0, 0.0, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        TArray<TSharedPtr<FJsonValue>> Only;
        Only.Add(MakeShared<FJsonValueString>(
            FString::Printf(TEXT("PW_NoSuchActor_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
        Payload->SetArrayField(TEXT("onlyActors"), Only);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (bad onlyActors)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestFalse(TEXT("an unresolvable onlyActors list is an error"), Capture.bSuccess);
        TestEqual(TEXT("unresolvable onlyActors -> ACTOR_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    }

    // maxHits below 1 -> INVALID_PARAMS. Deliberately NOT actor.list's "0 = all": each
    // layer costs a trace, so an unbounded walk is not on offer.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("origin"), RaycastTestVec(0.0, 0.0, 1000.0));
        Payload->SetObjectField(TEXT("direction"), RaycastTestVec(0.0, 0.0, -1.0));
        Payload->SetBoolField(TEXT("multiHit"), true);
        Payload->SetNumberField(TEXT("maxHits"), 0.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered (maxHits 0)"),
            InvokeHandlerWithCapture(TEXT("spatial.raycast"), Payload, Capture));
        TestFalse(TEXT("maxHits 0 is an error"), Capture.bSuccess);
        TestEqual(TEXT("maxHits 0 -> INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    return true;
}
