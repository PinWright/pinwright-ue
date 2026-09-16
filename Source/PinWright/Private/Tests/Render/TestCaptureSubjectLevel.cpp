// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the World and Actor capture-subject providers.
//
// WHY ALMOST NONE OF THESE NEEDS A VIEWPORT. Under `UnrealEditor-Cmd -unattended` there may be no
// active Level Editor viewport, and a test that gates its assertions on one reports success without
// having run them -- the failure mode board ticket B-test-skips-assertions-silently was filed for,
// and the one that let render.capture_asset_preview.PinnedCapturesReproduceWithinTolerance read
// green in three consecutive runs while asserting nothing. Bounds, the bounds source, the
// bare-point refusal, the sampled union and the no-time-axis refusal are all decided by
// PinWrightCaptureSubjectLevel::ResolveLevelSubjectBounds, which acquires no viewport, so every
// assertion below runs on every host.
//
// The single test that DOES go through the registry (and therefore through the viewport) is written
// so that both outcomes assert something: a success asserts the resolved subject, and a failure
// asserts the code is one only PinWrightCameraFrame::GetActiveLevelViewport emits -- which is
// itself the proof that the level provider was the one that ran.
//
// WIRE SPELLINGS ARE WRITTEN OUT AS LITERALS HERE, never reused from the header constants. The same
// discipline TestActorFindByNameSubsystemGuard.cpp keeps for error codes: comparing two
// independently written spellings is what makes a rename that forgot the wire fail.
#include "Misc/AutomationTest.h"

#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Level.h"
#include "Tests/TestWorldUtils.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

// Uniquely named namespace, not anonymous: Unity merges the Tests/Render translation units, so a
// bare helper name a sibling also uses is a latent ODR clash.
namespace CaptureSubjectLevelTestLocal
{
    // A NON-transient AStaticMeshActor carrying the engine unit cube.
    //
    // Non-transient is load-bearing for the Actor kind: the provider resolves through
    // McpActorUtils::FindActorByName -> UEditorActorSubsystem::GetAllLevelActors, which drops
    // RF_Transient actors in non-play worlds (UE 5.8 EditorActorSubsystem.cpp:386). The shared
    // SpawnTransientCubeActor fixture is therefore permanently unresolvable to this provider, and a
    // test built on it would exit through ACTOR_NOT_FOUND on every run with its real assertions
    // never reached. Pair with FScopedEditorWorldActorGuard.
    AStaticMeshActor* SpawnResolvableCube(UWorld* World, const FString& Label, const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams; // deliberately NOT RF_Transient (see above)
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    FString UniqueLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // The eight corners of an actor's world AABB. Used instead of a sphere-vs-sphere test because a
    // bounding SPHERE circumscribed on a box sticks out past the box, so "the subject sphere is
    // inside the level sphere" is not implied by containment of the boxes -- while every corner of
    // an enclosed box IS inside the enclosing box, and therefore inside its circumscribed sphere.
    void ActorBoundsCorners(const AActor& Actor, TArray<FVector>& OutCorners)
    {
        FVector Origin = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        Actor.GetActorBounds(false, Origin, Extent);
        OutCorners.Reset();
        for (int32 SignX = -1; SignX <= 1; SignX += 2)
        {
            for (int32 SignY = -1; SignY <= 1; SignY += 2)
            {
                for (int32 SignZ = -1; SignZ <= 1; SignZ += 2)
                {
                    OutCorners.Add(Origin + FVector(
                        Extent.X * SignX, Extent.Y * SignY, Extent.Z * SignZ));
                }
            }
        }
    }

    // Bounding-sphere radius the providers publish for a box: the half-diagonal, floored at 1.
    double SphereRadiusOf(const FVector& Extent)
    {
        return FMath::Max(Extent.Size(), 1.0);
    }
}

// ============================================================================
// 1. The World kind with no point frames the level.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLevelWorldBoundsFitTest,
    "PinWright.render.capture_subject_level.WorldBoundsFitFramesTheLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLevelWorldBoundsFitTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectLevel;
    using namespace CaptureSubjectLevelTestLocal;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    // Asserted rather than skipped: this is an EditorContext test, so an absent editor world is a
    // broken environment worth reporting, not a condition to pass quietly under.
    if (!TestNotNull(TEXT("an editor world is loaded"), World))
    {
        return false;
    }

    // A fixture so the level provably has bounds on every host, including a run against an empty
    // map. Without it, "BoundsRadius > 0" would be a property of whichever map happened to be open
    // rather than of the code under test.
    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* Cube = SpawnResolvableCube(World, UniqueLabel(TEXT("PW_SubjWorld")),
        FVector(1234.0, -567.0, 89.0));
    if (!TestNotNull(TEXT("spawned the cube fixture"), Cube))
    {
        return false;
    }

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::World;

    const FLevelTimePlan TimePlan;
    PinWrightCaptureSubject::FResolvedSubject Subject;
    PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
    FLevelSubjectReport Report;
    FString ErrCode;
    FString ErrMsg;

    if (!ResolveLevelSubjectBounds(Request, TimePlan, Subject, TimeSetter, Report, ErrCode, ErrMsg))
    {
        AddError(FString::Printf(TEXT("world subject refused: %s / %s"), *ErrCode, *ErrMsg));
        return false;
    }

    TestEqual(TEXT("bounds source is levelBounds"), Subject.BoundsSource, FString(TEXT("levelBounds")));
    TestTrue(TEXT("level bounds have a positive radius"), Subject.BoundsRadius > 0.0);
    TestEqual(TEXT("capture source is the level editor viewport"),
        Subject.CaptureSource, FString(TEXT("levelEditorViewport")));
    TestTrue(TEXT("the resolved subject carries its kind"),
        Subject.Kind == PinWrightCaptureSubject::ESubjectKind::World);
    // No sequence was named, so there is no time axis to claim.
    TestFalse(TEXT("a world subject with no sequence reports no time axis"), Subject.bTimeSupported);
    TestFalse(TEXT("and claims no time reproducibility"), Subject.bTimeReproducible);

    // The bounds must actually enclose the fixture. Without this the test would pass on any
    // non-zero number -- including one measured off a different level entirely.
    TArray<FVector> Corners;
    ActorBoundsCorners(*Cube, Corners);
    TestEqual(TEXT("the fixture yields eight AABB corners"), Corners.Num(), 8);
    double WorstCorner = 0.0;
    for (const FVector& Corner : Corners)
    {
        WorstCorner = FMath::Max(WorstCorner, FVector::Dist(Subject.BoundsOrigin, Corner));
    }
    TestTrue(TEXT("every corner of the fixture lies inside the level bounding sphere"),
        WorstCorner <= Subject.BoundsRadius + 1.0);

    ReleaseLevelSubject(Subject);
    return true;
}

// ============================================================================
// 2. A bare point still needs a radius, and the refusal is the one camera.orbit_shots has emitted
//    since it shipped.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLevelPointNeedsRadiusTest,
    "PinWright.render.capture_subject_level.PointWithoutRadiusIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLevelPointNeedsRadiusTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectLevel;

    const FVector Point(100.0, 200.0, 300.0);
    const FLevelTimePlan TimePlan;

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::World;
    Request.Point = Point;
    Request.bPointProvided = true;
    Request.Radius = 0.0f;

    FString ErrCode;
    FString ErrMsg;
    {
        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FLevelSubjectReport Report;
        TestFalse(TEXT("a point with no radius is refused"),
            ResolveLevelSubjectBounds(Request, TimePlan, Subject, TimeSetter, Report, ErrCode, ErrMsg));
    }
    TestEqual(TEXT("refusal is typed INVALID_ARGUMENT"), ErrCode, FString(TEXT("INVALID_ARGUMENT")));
    // Character-for-character the message camera.orbit_shots emits today. Written as a literal so a
    // reworded refusal fails here rather than silently changing what callers read.
    TestEqual(TEXT("refusal message is unchanged"), ErrMsg,
        FString(TEXT("radius (> 0) is required when the target is a bare point with no bounds")));

    // A request assembled in code (a verb normalising its own legacy `point` argument, or a test)
    // may not carry bPointProvided. A non-zero point is taken as evidence of one, so the refusal is
    // reachable there too. Asserted so the fallback is a contract rather than an implementation
    // detail a later edit can drop.
    {
        PinWrightCaptureSubject::FSubjectRequest Inferred = Request;
        Inferred.bPointProvided = false;
        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FLevelSubjectReport Report;
        FString InferredCode;
        FString InferredMsg;
        TestFalse(TEXT("a non-zero point with no radius is refused even without the flag"),
            ResolveLevelSubjectBounds(Inferred, TimePlan, Subject, TimeSetter, Report,
                InferredCode, InferredMsg));
        TestEqual(TEXT("the inferred path gives the same refusal"), InferredMsg, ErrMsg);
    }

    // The other direction: with a radius the point IS the framing extent, verbatim the existing
    // bare-point behaviour. A test that only asserted the refusal would pass against a provider
    // that refused every point.
    {
        PinWrightCaptureSubject::FSubjectRequest WithRadius = Request;
        WithRadius.Radius = 500.0f;
        WithRadius.bRadiusProvided = true;
        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FLevelSubjectReport Report;
        FString OkCode;
        FString OkMsg;
        if (!TestTrue(TEXT("a point with a radius resolves"),
                ResolveLevelSubjectBounds(WithRadius, TimePlan, Subject, TimeSetter, Report,
                    OkCode, OkMsg)))
        {
            AddError(FString::Printf(TEXT("point subject refused: %s / %s"), *OkCode, *OkMsg));
            return false;
        }
        TestEqual(TEXT("bounds source is point"), Subject.BoundsSource, FString(TEXT("point")));
        TestTrue(TEXT("the point is the bounds origin"), Subject.BoundsOrigin.Equals(Point, 0.001));
        TestEqual(TEXT("the radius is the framing extent"), Subject.BoundsRadius, 500.0, 0.001);
        ReleaseLevelSubject(Subject);
    }
    return true;
}

// ============================================================================
// 3. Sampled-union bounds. The property that makes the union worth computing at all is that it is
//    STRICTLY bigger than any single instant -- on a subject that moves.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLevelSampledUnionTest,
    "PinWright.render.capture_subject_level.SampledUnionExceedsAnySingleInstant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLevelSampledUnionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectLevel;
    using namespace CaptureSubjectLevelTestLocal;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("an editor world is loaded"), World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* Cube = SpawnResolvableCube(World, UniqueLabel(TEXT("PW_SubjUnion")),
        FVector::ZeroVector);
    if (!TestNotNull(TEXT("spawned the cube fixture"), Cube))
    {
        return false;
    }

    const TArray<double> Instants = {0.0, 1.0, 2.0};

    // The test's OWN record of where the fixture went, kept independently of anything the code
    // under test measures, so the movement precondition has an oracle the union cannot fake.
    TArray<FVector> DrivenLocations;
    PinWrightCaptureSubject::FSubjectTimeSetter MovingSetter =
        [Cube, &DrivenLocations](double TimeSeconds, FString&, FString&) -> bool
        {
            Cube->SetActorLocation(FVector(0.0, 0.0, TimeSeconds * 1000.0));
            DrivenLocations.Add(Cube->GetActorLocation());
            return true;
        };

    FSampledBoundsUnion Union;
    FString ErrCode;
    FString ErrMsg;
    if (!TestTrue(TEXT("the union walk completed"),
            UnionActorBoundsAcrossInstants(*Cube, Instants, MovingSetter, Union, ErrCode, ErrMsg)))
    {
        AddError(FString::Printf(TEXT("union refused: %s / %s"), *ErrCode, *ErrMsg));
        return false;
    }

    // ---- precondition FIRST: the fixture actually moved. Without this the strict inequality below
    // would be unfalsifiable -- a static fixture makes the union equal to the single instant, so an
    // implementation that returned the single instant would pass. ----
    TestEqual(TEXT("the setter ran once per instant"), DrivenLocations.Num(), Instants.Num());
    double DrivenSeparation = 0.0;
    for (int32 A = 0; A < DrivenLocations.Num(); ++A)
    {
        for (int32 B = A + 1; B < DrivenLocations.Num(); ++B)
        {
            DrivenSeparation = FMath::Max(DrivenSeparation,
                FVector::Dist(DrivenLocations[A], DrivenLocations[B]));
        }
    }
    if (!TestTrue(TEXT("PRECONDITION: the fixture moved between instants"), DrivenSeparation > 1.0))
    {
        return false;
    }

    TestEqual(TEXT("every instant contributed"), Union.InstantsSampled, Instants.Num());
    TestTrue(TEXT("a single instant has a positive radius"), Union.MaxSingleInstantRadius > 0.0);
    TestTrue(TEXT("the union box is valid"), Union.Union.IsValid != 0);

    const double UnionRadius = SphereRadiusOf(Union.Union.GetExtent());
    TestTrue(TEXT("the union radius strictly exceeds the largest single instant"),
        UnionRadius > Union.MaxSingleInstantRadius);
    TestTrue(TEXT("the union reports the movement it measured"), Union.MaxOriginSeparation > 1.0);

    // ---- the counterfactual. A subject that does NOT move must union to the single-instant
    // answer, so the strict inequality above is a property of movement and not an artefact of the
    // union always growing. ----
    Cube->SetActorLocation(FVector::ZeroVector);
    PinWrightCaptureSubject::FSubjectTimeSetter StillSetter =
        [](double, FString&, FString&) -> bool { return true; };

    FSampledBoundsUnion StillUnion;
    FString StillCode;
    FString StillMsg;
    TestTrue(TEXT("the static walk completed"),
        UnionActorBoundsAcrossInstants(*Cube, Instants, StillSetter, StillUnion, StillCode, StillMsg));
    TestEqual(TEXT("a static subject reports no movement"), StillUnion.MaxOriginSeparation, 0.0, 0.001);
    TestEqual(TEXT("and its union is the single-instant answer"),
        SphereRadiusOf(StillUnion.Union.GetExtent()), StillUnion.MaxSingleInstantRadius, 0.001);

    return true;
}

// ============================================================================
// 4. The Actor kind reads the actor, and an unresolvable name is refused the way the three camera
//    verbs refuse it today.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLevelActorBoundsTest,
    "PinWright.render.capture_subject_level.ActorBoundsComeFromTheActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLevelActorBoundsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectLevel;
    using namespace CaptureSubjectLevelTestLocal;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("an editor world is loaded"), World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* Cube = SpawnResolvableCube(World, UniqueLabel(TEXT("PW_SubjActor")),
        FVector(500.0, -250.0, 75.0));
    if (!TestNotNull(TEXT("spawned the cube fixture"), Cube))
    {
        return false;
    }
    // SetActorLabel may disambiguate, so resolve by the label the provider will actually see.
    const FString ResolvedName = Cube->GetActorLabel();
    const FLevelTimePlan TimePlan;

    {
        PinWrightCaptureSubject::FSubjectRequest Request;
        Request.Kind = PinWrightCaptureSubject::ESubjectKind::Actor;
        Request.ActorName = ResolvedName;

        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FLevelSubjectReport Report;
        FString ErrCode;
        FString ErrMsg;

        if (!ResolveLevelSubjectBounds(Request, TimePlan, Subject, TimeSetter, Report, ErrCode, ErrMsg))
        {
            AddError(FString::Printf(TEXT("actor subject refused: %s / %s"), *ErrCode, *ErrMsg));
            return false;
        }

        TestEqual(TEXT("bounds source is actorBounds"), Subject.BoundsSource, FString(TEXT("actorBounds")));
        TestEqual(TEXT("capture source is the level editor viewport"),
            Subject.CaptureSource, FString(TEXT("levelEditorViewport")));
        TestEqual(TEXT("the resolved subject names the actor"), Subject.ActorName, ResolvedName);
        TestEqual(TEXT("no sampled union was taken without a time plan"),
            Report.SampledBounds.InstantsSampled, 0);

        // Read off the fixture AT ASSERT TIME, not written as a literal: a literal would keep
        // passing if the provider stopped reading the actor and returned a constant.
        FVector ExpectedOrigin = FVector::ZeroVector;
        FVector ExpectedExtent = FVector::ZeroVector;
        Cube->GetActorBounds(false, ExpectedOrigin, ExpectedExtent);
        TestTrue(TEXT("the origin is the actor's bounds origin"),
            Subject.BoundsOrigin.Equals(ExpectedOrigin, 0.01));
        TestEqual(TEXT("the radius is the actor's bounding-sphere radius"),
            Subject.BoundsRadius, SphereRadiusOf(ExpectedExtent), 0.01);

        ReleaseLevelSubject(Subject);
    }

    // ---- the refusals, both of them the ones the camera verbs already emit ----
    {
        PinWrightCaptureSubject::FSubjectRequest Unknown;
        Unknown.Kind = PinWrightCaptureSubject::ESubjectKind::Actor;
        Unknown.ActorName = TEXT("PW_NoSuchActor_bb1f2c7e");
        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FLevelSubjectReport Report;
        FString UnknownCode;
        FString UnknownMsg;
        TestFalse(TEXT("an unresolvable actor is refused"),
            ResolveLevelSubjectBounds(Unknown, TimePlan, Subject, TimeSetter, Report,
                UnknownCode, UnknownMsg));
        TestEqual(TEXT("refusal is typed ACTOR_NOT_FOUND"), UnknownCode, FString(TEXT("ACTOR_NOT_FOUND")));
        TestEqual(TEXT("refusal names the actor"), UnknownMsg,
            FString(TEXT("Actor not found: PW_NoSuchActor_bb1f2c7e")));
    }
    {
        PinWrightCaptureSubject::FSubjectRequest Nameless;
        Nameless.Kind = PinWrightCaptureSubject::ESubjectKind::Actor;
        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FLevelSubjectReport Report;
        FString NamelessCode;
        FString NamelessMsg;
        TestFalse(TEXT("an actor subject with no name is refused"),
            ResolveLevelSubjectBounds(Nameless, TimePlan, Subject, TimeSetter, Report,
                NamelessCode, NamelessMsg));
        TestEqual(TEXT("refusal is typed INVALID_ARGUMENT"), NamelessCode,
            FString(TEXT("INVALID_ARGUMENT")));
        TestEqual(TEXT("refusal reproduces the existing target message"), NamelessMsg,
            FString(TEXT("Provide a target: actorName (or objectPath/actorPath) or point {x, y, z}")));
    }
    return true;
}

// ============================================================================
// 5. No sequence, no time axis -- and the refusal arrives from the setter, not from the resolve.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLevelNoTimeAxisTest,
    "PinWright.render.capture_subject_level.NoTimeAxisWithoutASequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLevelNoTimeAxisTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectLevel;
    using namespace CaptureSubjectLevelTestLocal;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("an editor world is loaded"), World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* Cube = SpawnResolvableCube(World, UniqueLabel(TEXT("PW_SubjNoTime")),
        FVector(0.0, 0.0, 200.0));
    if (!TestNotNull(TEXT("spawned the cube fixture"), Cube))
    {
        return false;
    }

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::Actor;
    Request.ActorName = Cube->GetActorLabel();

    const FLevelTimePlan TimePlan;   // no sequence path
    PinWrightCaptureSubject::FResolvedSubject Subject;
    PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
    FLevelSubjectReport Report;
    FString ErrCode;
    FString ErrMsg;

    if (!ResolveLevelSubjectBounds(Request, TimePlan, Subject, TimeSetter, Report, ErrCode, ErrMsg))
    {
        AddError(FString::Printf(TEXT("actor subject refused: %s / %s"), *ErrCode, *ErrMsg));
        return false;
    }

    // The resolve itself must NOT fail: a caller that never asks for a time never sees the refusal.
    // That is the half of the contract a "reject the request outright" implementation would break
    // while still passing a test that only checked the setter.
    TestFalse(TEXT("the subject reports no time axis"), Subject.bTimeSupported);
    TestTrue(TEXT("a setter is handed back anyway"), static_cast<bool>(TimeSetter));

    FString SetterCode;
    FString SetterMsg;
    TestFalse(TEXT("asking for a time is refused"), TimeSetter(0.5, SetterCode, SetterMsg));
    TestEqual(TEXT("the setter refusal is typed INVALID_ARGUMENT"), SetterCode,
        FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the refusal names the missing time axis"), SetterMsg.Contains(TEXT("time axis")));
    TestTrue(TEXT("and names the argument that would supply one"),
        SetterMsg.Contains(TEXT("sequencePath")));

    ReleaseLevelSubject(Subject);
    return true;
}

// ============================================================================
// 6. The providers reached the registry. Both outcomes assert: on a host with a live Level Editor
//    viewport the subject resolves, and on one without it the failure code is one only
//    PinWrightCameraFrame::GetActiveLevelViewport emits -- which is what proves this provider ran
//    rather than the registry refusing the kind.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLevelProvidersRegisteredTest,
    "PinWright.render.capture_subject_level.ProvidersAreRegisteredForBothLevelKinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLevelProvidersRegisteredTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectLevel;
    using namespace CaptureSubjectLevelTestLocal;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("an editor world is loaded"), World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    AStaticMeshActor* Cube = SpawnResolvableCube(World, UniqueLabel(TEXT("PW_SubjRegistry")),
        FVector(300.0, 300.0, 0.0));
    if (!TestNotNull(TEXT("spawned the cube fixture"), Cube))
    {
        return false;
    }

    // Registration is assertable directly -- no viewport involved -- so this half of the test can
    // never be skipped by a headless host.
    TestNotNull(TEXT("a provider is registered for the world kind"),
        PinWrightCaptureSubject::FindProvider(PinWrightCaptureSubject::ESubjectKind::World));
    TestNotNull(TEXT("a provider is registered for the actor kind"),
        PinWrightCaptureSubject::FindProvider(PinWrightCaptureSubject::ESubjectKind::Actor));

    // Codes only the level-viewport acquisition emits. No other provider and no registry miss can
    // produce them, so seeing one is positive evidence that the level provider was selected and ran.
    const auto IsLevelViewportRefusal = [](const FString& Code)
    {
        return Code == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Code == TEXT("NO_EDITOR_WORLD") ||
            Code == TEXT("EDITOR_NOT_AVAILABLE");
    };

    struct FCase
    {
        PinWrightCaptureSubject::ESubjectKind Kind;
        const TCHAR* ExpectedBoundsSource;
    };
    const FCase Cases[] =
    {
        {PinWrightCaptureSubject::ESubjectKind::World, TEXT("levelBounds")},
        {PinWrightCaptureSubject::ESubjectKind::Actor, TEXT("actorBounds")},
    };

    for (const FCase& Case : Cases)
    {
        PinWrightCaptureSubject::FSubjectRequest Request;
        Request.Kind = Case.Kind;
        if (Case.Kind == PinWrightCaptureSubject::ESubjectKind::Actor)
        {
            Request.ActorName = Cube->GetActorLabel();
        }

        PinWrightCaptureSubject::FResolvedSubject Subject;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
        FString ErrCode;
        FString ErrMsg;

        if (PinWrightCaptureSubject::Resolve(Request, Subject, TimeSetter, ErrCode, ErrMsg))
        {
            TestEqual(TEXT("the registry resolved the level bounds source"),
                Subject.BoundsSource, FString(Case.ExpectedBoundsSource));
            TestEqual(TEXT("the registry resolved the level capture source"),
                Subject.CaptureSource, FString(TEXT("levelEditorViewport")));
            TestNotNull(TEXT("a resolved subject carries a viewport client"), Subject.ViewportClient);
            TestTrue(TEXT("a resolved subject carries a scene viewport"), Subject.SceneViewport.IsValid());
            PinWrightCaptureSubject::ReleaseSubject(Subject);
        }
        else
        {
            TestTrue(
                FString::Printf(TEXT("the refusal came from the level viewport step, not from a missing provider (got '%s')"),
                    *ErrCode),
                IsLevelViewportRefusal(ErrCode));
        }
    }
    return true;
}
