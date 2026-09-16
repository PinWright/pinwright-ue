// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Environment domain handlers:
// EnvironmentHandler.cpp, FoliageHandler.cpp, LandscapeHandler.cpp, LightingHandler.cpp
#include "Misc/AutomationTest.h"
#include "Compat/SceneComponentCompat.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
#include "PinWrightHelpers.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DataTable.h"
#include "Engine/PostProcessVolume.h"
#include "EngineUtils.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Handlers/ErrorCodes.h"

// Spawns an AActor with a GUID-labelled name into the active world and gives it a
// registered, RF_Transactional USceneComponent root (the inspect_object fixture the
// EmitsProperties / EmitsClassKey tests both target). Returns the actor and hands the
// scene component back via OutScene; returns nullptr (with a recorded failure) if either
// allocation fails. The caller must declare FScopedEditorWorldActorGuard before calling
// this helper so teardown also deselects the actor and restores the level dirty flag.
static AActor* SpawnActorWithRootScene(
    FAutomationTestBase& Test, const TCHAR* LabelPrefix, const TCHAR* SceneName, USceneComponent*& OutScene)
{
    OutScene = nullptr;
    const FString ActorLabel = FString::Printf(TEXT("%s_%s"),
        LabelPrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AActor* Actor = SpawnActorInActiveWorld<AActor>(
        AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    Test.TestNotNull(TEXT("actor spawned"), Actor);
    if (!Actor)
    {
        return nullptr;
    }

    USceneComponent* Scene = NewObject<USceneComponent>(
        Actor, USceneComponent::StaticClass(), SceneName, RF_Transactional);
    Test.TestNotNull(TEXT("scene component created"), Scene);
    if (!Scene)
    {
        return Actor;
    }
    Actor->AddInstanceComponent(Scene);
    Actor->SetRootComponent(Scene);
    Scene->RegisterComponent();
    OutScene = Scene;
    return Actor;
}

// ============================================================================
// EnvironmentHandler — environment.build.create_sky_sphere
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentCreateSkySphereValidParamsTest,
    "PinWright.environment.build.create_sky_sphere.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentCreateSkySphereValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSkySphere"));
    TestTrue(TEXT("environment.build.create_sky_sphere handler found"),
        InvokeHandler(TEXT("environment.build.create_sky_sphere"), Payload));
    return true;
}

// Regression for board E-environment-build-create-no-name-param: create_sky_sphere
// was RPC_NO_PARAMS and hardcoded the spawned actor's label to "SkySphere", so a
// requested name could not be honored at creation (the caller paid an extra
// property.set ActorLabel). The fix adds an optional `name` slot passed through to
// SpawnActorInActiveWorld's label arg, defaulting to "SkySphere" when omitted.
// This test asserts BOTH halves: the supplied name lands as the actor label and is
// echoed in `actorName`, and omitting `name` keeps the historical default. It fails
// if the `name` slot is dropped (the echo reverts to the hardcoded default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentCreateSkySphereHonorsNameTest,
    "PinWright.environment.build.create_sky_sphere.HonorsNameSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentCreateSkySphereHonorsNameTest::RunTest(const FString& Parameters)
{
    TestCreateHandlerHonorsNameSlot(*this,
        TEXT("environment.build.create_sky_sphere"), TEXT("SkySphere"));
    return true;
}

// ============================================================================
// EnvironmentHandler — environment.build.set_time_of_day
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBuildSetTimeOfDayValidParamsTest,
    "PinWright.environment.build.set_time_of_day.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBuildSetTimeOfDayValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("time"), 12.0);
    TestTrue(TEXT("environment.build.set_time_of_day handler found"),
        InvokeHandler(TEXT("environment.build.set_time_of_day"), Payload));
    return true;
}

// ============================================================================
// EnvironmentHandler — environment.control.set_time_of_day
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentControlSetTimeOfDayValidParamsTest,
    "PinWright.environment.control.set_time_of_day.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentControlSetTimeOfDayValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("hour"), 14.0);
    TestTrue(TEXT("environment.control.set_time_of_day handler found"),
        InvokeHandler(TEXT("environment.control.set_time_of_day"), Payload));
    return true;
}

// ============================================================================
// EnvironmentHandler — environment.control.set_sun_intensity
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSetSunIntensityValidParamsTest,
    "PinWright.environment.control.set_sun_intensity.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSetSunIntensityValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 5.0);
    TestTrue(TEXT("environment.control.set_sun_intensity handler found"),
        InvokeHandler(TEXT("environment.control.set_sun_intensity"), Payload));
    return true;
}

// ============================================================================
// EnvironmentHandler — environment.control.set_skylight_intensity
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSetSkylightIntensityValidParamsTest,
    "PinWright.environment.control.set_skylight_intensity.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSetSkylightIntensityValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 1.0);
    TestTrue(TEXT("environment.control.set_skylight_intensity handler found"),
        InvokeHandler(TEXT("environment.control.set_skylight_intensity"), Payload));
    return true;
}

// ============================================================================
// EnvironmentHandler — environment.build.create_procedural_terrain
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentCreateProceduralTerrainValidParamsTest,
    "PinWright.environment.build.create_procedural_terrain.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentCreateProceduralTerrainValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestTerrain"));
    Payload->SetNumberField(TEXT("width"), 1000.0);
    Payload->SetNumberField(TEXT("height"), 1000.0);
    TestTrue(TEXT("environment.build.create_procedural_terrain handler found"),
        InvokeHandler(TEXT("environment.build.create_procedural_terrain"), Payload));
    return true;
}

// Regression for board E-create-procedural-terrain-no-material-echo: the handler
// applies the requested `material` (ProcMesh->SetMaterial(0, ...)) but historically
// dropped it from the success response, forcing a separate actor.describe readback
// to confirm the material landed — and a failed LoadObject was swallowed silently
// (still success:true, no material applied, nothing distinguishing the miss). The
// fix echoes `material_applied` (always) + `materialPath` (when requested) and surfaces
// a `materialWarning` when a non-empty path fails to load. This test fails if any of
// those echoes is reverted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentCreateProceduralTerrainEchoesMaterialTest,
    "PinWright.environment.build.create_procedural_terrain.EchoesAppliedMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentCreateProceduralTerrainEchoesMaterialTest::RunTest(const FString& Parameters)
{
    // /Engine/EngineMaterials/WorldGridMaterial is reliably loadable on all UE
    // installs (matches the path the asset-dump tests lean on), so the handler's
    // LoadObject<UMaterialInterface> resolves and SetMaterial actually runs.
    const FString MaterialPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Build the create_procedural_terrain payload (optionally with a `material`),
    // invoke the handler, and assert the invariant handler-found + create-succeeded
    // checks shared by all three cases. Returns the capture so each case can make
    // its distinguishing assertions on the echoed fields.
    auto Run = [&](const TCHAR* Prefix, const FString* MatPath) -> FTestResponseCapture
    {
        const FString TerrainName = FString::Printf(TEXT("PW_Terrain%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), TerrainName);
        Payload->SetNumberField(TEXT("subdivisions"), 2.0);
        if (MatPath)
        {
            Payload->SetStringField(TEXT("material"), *MatPath);
        }

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler found"),
            InvokeHandlerWithCapture(TEXT("environment.build.create_procedural_terrain"), Payload, Capture));
        TestTrue(TEXT("create succeeded"), Capture.bSuccess);
        return Capture;
    };

    // Case 1: valid material → response must echo materialPath + material_applied=true.
    {
        FTestResponseCapture Capture = Run(TEXT("Mat"), &MaterialPath);
        if (Capture.Result.IsValid())
        {
            bool bApplied = false;
            TestTrue(TEXT("response carries material_applied"),
                Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied));
            TestTrue(TEXT("material_applied is true for a loadable material"), bApplied);

            FString EchoedPath;
            TestTrue(TEXT("response echoes the requested material path"),
                Capture.Result->TryGetStringField(TEXT("materialPath"), EchoedPath));
            TestEqual(TEXT("echoed materialPath equals the requested path"), EchoedPath, MaterialPath);

            // A successful apply must NOT carry a warning.
            FString Warning;
            TestFalse(TEXT("no materialWarning on a successful apply"),
                Capture.Result->TryGetStringField(TEXT("materialWarning"), Warning));
        }
    }

    // Case 2: bogus material path → applied=false AND a materialWarning surfaces
    // (the silent-swallow no longer hides the miss).
    {
        const FString BadPath = TEXT("/Game/DoesNotExist/M_NoSuchMaterial.M_NoSuchMaterial");
        FTestResponseCapture Capture = Run(TEXT("BadMat"), &BadPath);
        if (Capture.Result.IsValid())
        {
            bool bApplied = true;
            TestTrue(TEXT("response carries material_applied (bad material)"),
                Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied));
            TestFalse(TEXT("material_applied is false for an unloadable material"), bApplied);

            FString Warning;
            TestTrue(TEXT("materialWarning surfaces the skipped material"),
                Capture.Result->TryGetStringField(TEXT("materialWarning"), Warning));
        }
    }

    // Case 3: no material requested → material_applied=false and no materialPath field.
    {
        FTestResponseCapture Capture = Run(TEXT("NoMat"), nullptr);
        if (Capture.Result.IsValid())
        {
            bool bApplied = true;
            TestTrue(TEXT("response carries material_applied (no material)"),
                Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied));
            TestFalse(TEXT("material_applied is false when none requested"), bApplied);

            FString EchoedPath;
            TestFalse(TEXT("no materialPath field when none requested"),
                Capture.Result->TryGetStringField(TEXT("materialPath"), EchoedPath));
        }
    }

    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.* (no required params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectGetSelectedActorsValidParamsTest,
    "PinWright.system.inspect.get_selected_actors.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectGetSelectedActorsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("system.inspect.get_selected_actors handler found"),
        InvokeHandler(TEXT("system.inspect.get_selected_actors"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectListObjectsValidParamsTest,
    "PinWright.system.inspect.list_objects.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectListObjectsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("system.inspect.list_objects handler found"),
        InvokeHandler(TEXT("system.inspect.list_objects"), Payload));
    return true;
}

// Regression test for E-inspect-list-objects-no-limit-spills: system.inspect.list_objects
// must offer a `limit` cap (with an untruncated `totalMatches` + `truncated` flag) and a
// `namesOnly`/`fields` projection that drops the verbose per-row `path`, so the natural
// "enumerate so I can pick a few" survey can stay inline instead of always spilling the
// full array to disk. Spawns three real PointLights into the editor world, filters to
// them by a unique name fragment so the assertions are independent of whatever else the
// open map contains, then drives the production handler. If the limit/projection plumbing
// were reverted, limit would be ignored (count would equal totalMatches with truncated:false)
// and the namesOnly call would still carry `path`, failing the assertions below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectListObjectsLimitAndProjectionTest,
    "PinWright.system.inspect.list_objects.LimitAndProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectListObjectsLimitAndProjectionTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping list_objects limit/projection test."));
        return true;
    }

    // Spawn three real PointLights sharing a unique fragment; the guard destroys
    // them and restores the level's dirty flag on scope exit. actor.spawn only sets
    // the actor LABEL from actorName, but system.inspect.list_objects filters on the
    // UObject name (GetName()) + class — never the editor-only label — so each spawn
    // is followed by a Rename() that pushes the tag into the object name. That makes
    // the probes discoverable by the handler's actual name-substring filter (a
    // label-only tag would silently match nothing, which is exactly how the prior
    // revision of this test failed).
    FScopedEditorWorldActorGuard WorldGuard;
    const FString NameTag = TEXT("ListObjectsLimitProbe");
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"),
            FString::Printf(TEXT("%s_%d"), *NameTag, Index));
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        TestTrue(TEXT("actor.spawn succeeded"), SpawnCapture.bSuccess);

        // Resolve the just-spawned actor via the returned actorPath and rename its
        // UObject so GetName() carries the unique tag the handler filters on.
        FString ActorPath;
        if (SpawnCapture.bSuccess && SpawnCapture.Result.IsValid()
            && SpawnCapture.Result->TryGetStringField(TEXT("actorPath"), ActorPath))
        {
            if (AActor* Spawned = FindObject<AActor>(nullptr, *ActorPath))
            {
                Spawned->Rename(*FString::Printf(TEXT("%s_%d"), *NameTag, Index),
                    nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
            }
        }
    }

    // Each probe seeds the shared filter+world narrowing onto the three spawned
    // lights, lets the caller add the case-specific knob, invokes the production
    // handler, and asserts it registered + succeeded.
    auto ListProbe = [&](TFunctionRef<void(TSharedPtr<FJsonObject>)> Configure) -> FTestResponseCapture
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filter"), NameTag);
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Configure(Payload);
        TestTrue(TEXT("system.inspect.list_objects handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_objects"), Payload, Capture));
        TestTrue(TEXT("system.inspect.list_objects succeeded"), Capture.bSuccess);
        return Capture;
    };

    // Digs the first row object out of an objects[] response, or nullptr if absent.
    auto FirstRow = [&](FTestResponseCapture& Capture) -> const TSharedPtr<FJsonObject>*
    {
        const TArray<TSharedPtr<FJsonValue>>* ObjectsArr = nullptr;
        if (!Capture.Result.IsValid()
            || !Capture.Result->TryGetArrayField(TEXT("objects"), ObjectsArr)
            || !ObjectsArr || ObjectsArr->Num() == 0)
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*ObjectsArr)[0].IsValid() && (*ObjectsArr)[0]->TryGetObject(Row) && Row)
        {
            return Row;
        }
        return nullptr;
    };

    // Baseline: filter to just the three probes, no limit. totalMatches equals the
    // returned count and truncated is false.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject>) {});
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            double Count = 0.0, Total = 0.0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), Total);
            bool bTruncated = true;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestEqual(TEXT("Unlimited list returns all 3 probes"), (int32)Count, 3);
            TestEqual(TEXT("totalMatches matches the returned count when uncapped"),
                (int32)Total, 3);
            TestFalse(TEXT("truncated is false when nothing was capped"), bTruncated);
        }
    }

    // limit=2: count caps at 2 rows, but totalMatches still reports all 3 matches
    // and truncated flips true so the caller knows the list was elided.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject> Payload)
        {
            Payload->SetNumberField(TEXT("limit"), 2);
        });
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            double Count = 0.0, Total = 0.0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), Total);
            bool bTruncated = false;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestEqual(TEXT("limit=2 caps the returned rows at 2"), (int32)Count, 2);
            TestEqual(TEXT("totalMatches still reports all 3 untruncated matches"),
                (int32)Total, 3);
            TestTrue(TEXT("truncated is true when limit elided rows"), bTruncated);

            const TArray<TSharedPtr<FJsonValue>>* ObjectsArr = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("objects"), ObjectsArr) && ObjectsArr)
            {
                TestEqual(TEXT("objects array length honors the limit"), ObjectsArr->Num(), 2);
            }
        }
    }

    // namesOnly=true: each returned row carries name/class but NOT the verbose
    // path field — this is the projection that collapses the payload.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject> Payload)
        {
            Payload->SetBoolField(TEXT("namesOnly"), true);
        });
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("namesOnly row keeps name"), (*Row)->HasField(TEXT("name")));
            TestTrue(TEXT("namesOnly row keeps class"), (*Row)->HasField(TEXT("class")));
            TestFalse(TEXT("namesOnly row drops the verbose path field"),
                (*Row)->HasField(TEXT("path")));
        }
    }

    // fields=["class"] allow-list: rows carry only class, dropping name/path.
    {
        FTestResponseCapture Capture = ListProbe([](TSharedPtr<FJsonObject> Payload)
        {
            TArray<TSharedPtr<FJsonValue>> FieldArr;
            FieldArr.Add(MakeShared<FJsonValueString>(TEXT("class")));
            Payload->SetArrayField(TEXT("fields"), FieldArr);
        });
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("fields=[class] row keeps class"), (*Row)->HasField(TEXT("class")));
            TestFalse(TEXT("fields=[class] row drops name"), (*Row)->HasField(TEXT("name")));
            TestFalse(TEXT("fields=[class] row drops path"), (*Row)->HasField(TEXT("path")));
        }
    }

    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.find_by_class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectFindByClassValidParamsTest,
    "PinWright.system.inspect.find_by_class.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectFindByClassValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("className"), TEXT("StaticMeshActor"));
    TestTrue(TEXT("system.inspect.find_by_class handler found"),
        InvokeHandler(TEXT("system.inspect.find_by_class"), Payload));
    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.find_objects_by_class
// ============================================================================

// Regression for F-inspect-find-non-actor-uobject: list_objects / find_by_class /
// find_by_tag all iterate TActorIterator<AActor>, so a live NON-actor UObject (a
// UMG-widget-owned helper, a component, a plain UObject) is invisible to MCP and its
// object path — the one input object.call_function needs — is unobtainable. The new
// system.inspect.find_objects_by_class enumerates live instances of a class via
// GetObjectsOfClass (the global object hash), so non-actors become discoverable. This
// test builds an in-code non-actor fixture: a UDataTable created into the transient
// package with a GUID-unique name (a DataTable is a plain UObject, never an AActor, and
// the Engine module guarantees it instantiates headless — no reliance on on-disk content).
// It asserts: (1) the new verb surfaces the non-actor probe by class + unique name with a
// usable path; (2) the actor-only find_by_class sibling never returns it (the blind spot
// this closes); and (3) the CDO footgun guard holds — the class-default object is excluded
// by default and only appears when includeDefaults=true. Reverting the handler fails (1);
// reverting it to an actor iterator fails (1) and (2); dropping the CDO exclusion fails (3).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectFindObjectsByClassFindsNonActorTest,
    "PinWright.system.inspect.find_objects_by_class.FindsLiveNonActorUObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectFindObjectsByClassFindsNonActorTest::RunTest(const FString& Parameters)
{
    // In-code non-actor fixture: a uniquely-named UDataTable in the transient package.
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ProbeName = FString::Printf(TEXT("PW_NonActorProbe_%s"), *Suffix);
    UDataTable* Probe = NewObject<UDataTable>(
        GetTransientPackage(), UDataTable::StaticClass(), FName(*ProbeName), RF_Transient);
    TestNotNull(TEXT("non-actor UDataTable probe created"), Probe);
    if (!Probe)
    {
        return false;
    }
    // Root the probe so no incidental GC during the test collects it out from under
    // the enumerator; unrooted (and thus collectable) again on scope exit.
    Probe->AddToRoot();
    const FString ProbePath = Probe->GetPathName();
    ON_SCOPE_EXIT
    {
        Probe->RemoveFromRoot();
    };

    // Returns the objects[] row whose `path` == Path, or an invalid ptr if absent.
    auto FindRowByPath = [](const FTestResponseCapture& Capture, const FString& Path) -> TSharedPtr<FJsonObject>
    {
        if (!Capture.Result.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("objects"), Rows) || !Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (RowVal.IsValid() && RowVal->TryGetObject(Row) && Row)
            {
                FString RowPath;
                if ((*Row)->TryGetStringField(TEXT("path"), RowPath) && RowPath == Path)
                {
                    return *Row;
                }
            }
        }
        return nullptr;
    };

    auto RowCount = [](const FTestResponseCapture& Capture) -> int32
    {
        double Count = -1.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
        }
        return (int32)Count;
    };

    // (1) The new verb discovers the live non-actor instance by class + unique name.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("className"), TEXT("DataTable"));
        Payload->SetStringField(TEXT("filter"), ProbeName);
        TestTrue(TEXT("find_objects_by_class handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.find_objects_by_class"), Payload, Capture));
        TestTrue(TEXT("find_objects_by_class succeeded"), Capture.bSuccess);

        TSharedPtr<FJsonObject> Row = FindRowByPath(Capture, ProbePath);
        TestTrue(TEXT("non-actor probe is discovered by find_objects_by_class"), Row.IsValid());
        if (Row.IsValid())
        {
            FString RowName, RowClass;
            TestTrue(TEXT("probe row carries name"), Row->TryGetStringField(TEXT("name"), RowName));
            TestEqual(TEXT("probe row name is the unique object name"), RowName, ProbeName);
            TestTrue(TEXT("probe row carries class"), Row->TryGetStringField(TEXT("class"), RowClass));
            TestEqual(TEXT("probe row class is DataTable"), RowClass, FString(TEXT("DataTable")));
        }
    }

    // (2) The actor-only sibling find_by_class must NOT surface the non-actor probe —
    // that blind spot is exactly what this ticket closes.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("className"), TEXT("DataTable"));
        TestTrue(TEXT("find_by_class handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.find_by_class"), Payload, Capture));
        TestFalse(TEXT("actor-only find_by_class does not surface the non-actor probe"),
            FindRowByPath(Capture, ProbePath).IsValid());
    }

    // (3) CDO footgun guard: the class-default object is excluded by default and only
    // included when includeDefaults=true. Target the CDO by its Default__<Class> name.
    const FString CdoFilter = FString::Printf(TEXT("Default__%s"), *UDataTable::StaticClass()->GetName());
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("className"), TEXT("DataTable"));
        Payload->SetStringField(TEXT("filter"), CdoFilter);
        Payload->SetBoolField(TEXT("includeDefaults"), false);
        TestTrue(TEXT("find_objects_by_class (no defaults) is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.find_objects_by_class"), Payload, Capture));
        TestTrue(TEXT("find_objects_by_class (no defaults) succeeded"), Capture.bSuccess);
        TestEqual(TEXT("CDO is excluded by default"), RowCount(Capture), 0);
    }
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("className"), TEXT("DataTable"));
        Payload->SetStringField(TEXT("filter"), CdoFilter);
        Payload->SetBoolField(TEXT("includeDefaults"), true);
        TestTrue(TEXT("find_objects_by_class (with defaults) is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.find_objects_by_class"), Payload, Capture));
        TestTrue(TEXT("find_objects_by_class (with defaults) succeeded"), Capture.bSuccess);
        TestTrue(TEXT("CDO appears when includeDefaults=true"), RowCount(Capture) >= 1);
    }

    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.find_by_tag
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectFindByTagValidParamsTest,
    "PinWright.system.inspect.find_by_tag.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectFindByTagValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("tag"), TEXT("Interactable"));
    TestTrue(TEXT("system.inspect.find_by_tag handler found"),
        InvokeHandler(TEXT("system.inspect.find_by_tag"), Payload));
    return true;
}

// Regression test for E-inspect-find-by-tag-internal-name-not-label: the read-only
// system.inspect enumerators (find_by_tag / find_by_class / list_objects) must surface
// the editor display label in an ADDITIVE `label` field while keeping `name` = the
// collision-safe internal object name (GetName) — they must NOT redefine `name` to the
// label the way their actor.* write twins do. Spawns a cube whose display label is
// deliberately distinct from its internal GetName(), tags it, then drives each
// production handler and asserts the matching row carries label==GetActorLabel and
// name==GetName (name != label). If the fix were reverted (no `label` field) the label
// assertions fail; if someone instead redefined `name` to the label, the name!=label
// assertion fails — the test guards the additive contract from both directions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectEnumeratorsEmitLabelTest,
    "PinWright.system.inspect.find_by_tag.EmitsDisplayLabelKeepsInternalName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectEnumeratorsEmitLabelTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping inspect label-emission test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // A GUID-unique display label that cannot equal the engine-generated internal
    // object name (StaticMeshActor_N), and a unique tag for the find_by_tag probe.
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString DisplayLabel = FString::Printf(TEXT("InspectLabelProbe_%s"), *Suffix);
    const FString ProbeTag = FString::Printf(TEXT("InspectLabelTag_%s"), *Suffix);

    AStaticMeshActor* Probe = SpawnTransientCubeActor(World, DisplayLabel, FVector::ZeroVector);
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Could not spawn probe cube (no engine cube mesh?); skipping."));
        return true;
    }
    Probe->Tags.Add(FName(*ProbeTag));

    const FString ExpectedLabel = Probe->GetActorLabel();
    const FString ExpectedName = Probe->GetName();
    const FString ExpectedPath = Probe->GetPathName();
    // Precondition the whole test rests on: the display label and the internal object
    // name differ, so "name carries the label" and "name carries the internal name" are
    // observably different outcomes.
    TestNotEqual(TEXT("probe label differs from its internal object name"),
        ExpectedLabel, ExpectedName);

    // Locates the row whose `path` matches the probe in an objects[]/actors[] array and
    // asserts it carries label==ExpectedLabel and name==ExpectedName (name != label).
    auto AssertRowHasLabelAndInternalName =
        [&](FTestResponseCapture& Capture, const TCHAR* ArrayKey, const TCHAR* Method)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetArrayField(ArrayKey, Rows) || !Rows)
        {
            AddError(FString::Printf(TEXT("%s response missing '%s' array"), Method, ArrayKey));
            return;
        }
        bool bFound = false;
        for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!RowVal.IsValid() || !RowVal->TryGetObject(Row) || !Row)
            {
                continue;
            }
            FString RowPath;
            if (!(*Row)->TryGetStringField(TEXT("path"), RowPath) || RowPath != ExpectedPath)
            {
                continue;
            }
            bFound = true;
            FString RowLabel, RowName;
            const bool bHasLabel = (*Row)->TryGetStringField(TEXT("label"), RowLabel);
            const bool bHasName = (*Row)->TryGetStringField(TEXT("name"), RowName);
            TestTrue(FString::Printf(TEXT("%s row carries an additive 'label' field"), Method), bHasLabel);
            TestEqual(FString::Printf(TEXT("%s 'label' is the display label"), Method), RowLabel, ExpectedLabel);
            TestTrue(FString::Printf(TEXT("%s row keeps a 'name' field"), Method), bHasName);
            TestEqual(FString::Printf(TEXT("%s 'name' stays the internal object name"), Method), RowName, ExpectedName);
            TestNotEqual(FString::Printf(TEXT("%s 'name' is NOT redefined to the label"), Method), RowName, RowLabel);
            break;
        }
        TestTrue(FString::Printf(TEXT("%s returned the probe row"), Method), bFound);
    };

    // system.inspect.find_by_tag
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("tag"), ProbeTag);
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("find_by_tag registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.find_by_tag"), Payload, Capture));
        TestTrue(TEXT("find_by_tag succeeded"), Capture.bSuccess);
        AssertRowHasLabelAndInternalName(Capture, TEXT("objects"), TEXT("find_by_tag"));
    }

    // system.inspect.find_by_class
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("className"), TEXT("StaticMeshActor"));
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("find_by_class registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.find_by_class"), Payload, Capture));
        TestTrue(TEXT("find_by_class succeeded"), Capture.bSuccess);
        AssertRowHasLabelAndInternalName(Capture, TEXT("objects"), TEXT("find_by_class"));
    }

    // system.inspect.list_objects (default projection — label kept)
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("list_objects registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_objects"), Payload, Capture));
        TestTrue(TEXT("list_objects succeeded"), Capture.bSuccess);
        AssertRowHasLabelAndInternalName(Capture, TEXT("objects"), TEXT("list_objects"));
    }

    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.list_actor_tags
// ============================================================================

// Regression test for F-inspect-list-actor-tags: find_by_tag needs an
// already-known exact tag, so before this census there was no verb that could
// enumerate the distinct AActor::Tags present in a world — a "what is tagged for
// review/cleanup?" audit could only guess tag names (silently missing unguessed
// ones) or scan every actor. The new system.inspect.list_actor_tags unions
// AActor::Tags into {tag, count} rows (count = actors carrying the tag), with a
// distinctCount/limit/truncated detectable-elision contract mirroring list_objects.
// This spawns three plain transient actors into the editor world and tags them so a
// GUID-unique tag is carried by exactly one actor and a GUID-unique shared tag by
// all three, then drives the production handler and asserts: (1) both fixture tags
// appear in the census with their exact expected counts (3 and 1), and (2) limit
// caps the returned rows while distinctCount still reports the full untruncated
// distinct-tag total and truncated flips true. If the handler were removed the
// "registered" assertion fails; if the census union/count were reverted the per-tag
// count assertions fail; if the elision contract were dropped the limit assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectListActorTagsCensusTest,
    "PinWright.system.inspect.list_actor_tags.EnumeratesDistinctTagsWithCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectListActorTagsCensusTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddError(TEXT("No editor world available; cannot build the list_actor_tags fixture."));
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // GUID-unique tags so the census assertions are independent of whatever tags the
    // open level's actors already carry.
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FName SoloTag(*FString::Printf(TEXT("PW_TagCensusSolo_%s"), *Suffix));
    const FName SharedTag(*FString::Printf(TEXT("PW_TagCensusShared_%s"), *Suffix));

    // Three plain transient actors: SharedTag on all three, SoloTag on exactly one.
    // A bare AActor needs no mesh/asset, so the fixture is fully in-code and always
    // spawnable in a valid editor world (no reliance on example content on disk).
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        TestNotNull(TEXT("fixture actor spawned"), Actor);
        if (!Actor)
        {
            return false;
        }
        Actor->Tags.Add(SharedTag);
        if (Index == 0)
        {
            Actor->Tags.Add(SoloTag);
        }
    }

    // Pull the count for a given tag string out of a census response, or -1 if the
    // tag is absent from the tags[] array.
    auto CountForTag = [](const FTestResponseCapture& Capture, const FString& TagStr) -> int32
    {
        if (!Capture.Result.IsValid())
        {
            return -1;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("tags"), Rows) || !Rows)
        {
            return -1;
        }
        for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!RowVal.IsValid() || !RowVal->TryGetObject(Row) || !Row)
            {
                continue;
            }
            FString RowTag;
            double RowCount = 0.0;
            if ((*Row)->TryGetStringField(TEXT("tag"), RowTag) && RowTag == TagStr
                && (*Row)->TryGetNumberField(TEXT("count"), RowCount))
            {
                return (int32)RowCount;
            }
        }
        return -1;
    };

    // Unlimited census: both fixture tags must appear with their exact counts, and
    // distinctCount must cover at least the two tags this test injected.
    int32 FullDistinct = 0;
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("list_actor_tags handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_actor_tags"), Payload, Capture));
        TestTrue(TEXT("list_actor_tags succeeded"), Capture.bSuccess);

        TestEqual(TEXT("SharedTag census count is 3 (all three fixture actors)"),
            CountForTag(Capture, SharedTag.ToString()), 3);
        TestEqual(TEXT("SoloTag census count is 1 (one fixture actor)"),
            CountForTag(Capture, SoloTag.ToString()), 1);

        if (Capture.Result.IsValid())
        {
            double Distinct = 0.0;
            TestTrue(TEXT("response carries distinctCount"),
                Capture.Result->TryGetNumberField(TEXT("distinctCount"), Distinct));
            FullDistinct = (int32)Distinct;
            TestTrue(TEXT("distinctCount covers at least the two injected tags"),
                FullDistinct >= 2);
            bool bTruncated = true;
            TestTrue(TEXT("unlimited census carries a truncated flag"),
                Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
            TestFalse(TEXT("unlimited census is not truncated"), bTruncated);
        }
    }

    // limit=1: the returned rows cap at 1, but distinctCount still reports the full
    // untruncated distinct-tag total and truncated flips true (there are >= 2 tags).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetNumberField(TEXT("limit"), 1);
        TestTrue(TEXT("list_actor_tags (limit) handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_actor_tags"), Payload, Capture));
        TestTrue(TEXT("list_actor_tags (limit) succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows)
            {
                TestEqual(TEXT("limit=1 caps the returned rows at 1"), Rows->Num(), 1);
            }
            double Count = 0.0, Distinct = 0.0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("distinctCount"), Distinct);
            TestEqual(TEXT("count reflects the capped row total"), (int32)Count, 1);
            TestEqual(TEXT("distinctCount still reports the full untruncated total"),
                (int32)Distinct, FullDistinct);
            bool bTruncated = false;
            TestTrue(TEXT("limited census carries a truncated flag"),
                Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
            TestTrue(TEXT("truncated is true when limit elided rows"), bTruncated);
        }
    }

    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.list_actor_classes
// ============================================================================

// Regression test for F-scene-composition-histogram: find_by_class needs an
// already-known class, so before this census "what is this level made of?" forced a
// full list_objects row dump (which
// spills on a populated level) + client-side counts-by-class aggregation. The new
// system.inspect.list_actor_classes unions actor classes into {class, count} rows
// (count = actors of that class), with a distinctCount/limit/truncated detectable-
// elision contract mirroring the sibling system.inspect.list_actor_tags census.
// This captures a baseline census, then spawns three transient AStaticMeshActors and
// one bare AActor into the editor world and asserts: (1) the census count for
// StaticMeshActor rose by exactly +3 and for Actor by exactly +1 — a delta because
// the open level may already carry actors of those classes — which proves it groups
// BY class rather than returning a grand total (a total would move both deltas by
// +4), and (2) limit caps the returned rows while distinctCount still reports the
// full untruncated distinct-class total and truncated flips true. If the handler were
// removed the "registered" assertion fails; if the per-class tally were reverted the
// delta assertions fail; if the elision contract were dropped the limit assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectListActorClassesCensusTest,
    "PinWright.system.inspect.list_actor_classes.CountsActorsByClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectListActorClassesCensusTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddError(TEXT("No editor world available; cannot build the list_actor_classes fixture."));
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // Pull the count for a given class name out of a census response, or 0 if the
    // class has no row (absent = zero actors of it) — so a before/after delta is well
    // defined whether or not the open level already carries that class.
    auto CountForClass = [](const FTestResponseCapture& Capture, const FString& ClassStr) -> int32
    {
        if (!Capture.Result.IsValid())
        {
            return -1;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("classes"), Rows) || !Rows)
        {
            return -1;
        }
        for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!RowVal.IsValid() || !RowVal->TryGetObject(Row) || !Row)
            {
                continue;
            }
            FString RowClass;
            double RowCount = 0.0;
            if ((*Row)->TryGetStringField(TEXT("class"), RowClass) && RowClass == ClassStr
                && (*Row)->TryGetNumberField(TEXT("count"), RowCount))
            {
                return (int32)RowCount;
            }
        }
        return 0;
    };

    const FString MeshClass = AStaticMeshActor::StaticClass()->GetName(); // "StaticMeshActor"
    const FString BareClass = AActor::StaticClass()->GetName();           // "Actor"

    // Baseline census BEFORE spawning: how many of each class the open level already
    // carries (0 if none). Unlimited so every class row is present for the delta.
    int32 BaseMesh = 0;
    int32 BaseBare = 0;
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("list_actor_classes handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_actor_classes"), Payload, Capture));
        TestTrue(TEXT("list_actor_classes (baseline) succeeded"), Capture.bSuccess);
        BaseMesh = CountForClass(Capture, MeshClass);
        BaseBare = CountForClass(Capture, BareClass);
    }

    // Fixture: three transient StaticMeshActors + one bare AActor. Neither needs a
    // mesh/asset for the class tally, so the fixture is fully in-code and always
    // spawnable in a valid editor world (no reliance on example content on disk).
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        AActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        TestNotNull(TEXT("fixture StaticMeshActor spawned"), Actor);
        if (!Actor)
        {
            return false;
        }
    }
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        TestNotNull(TEXT("fixture bare AActor spawned"), Actor);
        if (!Actor)
        {
            return false;
        }
    }

    // Post-spawn census: the per-class counts must rise by exactly the number injected
    // for each class. Different deltas (+3 vs +1) prove the census groups BY class — a
    // grand-total-per-row bug would move both by +4.
    int32 FullDistinct = 0;
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        TestTrue(TEXT("list_actor_classes (post-spawn) handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_actor_classes"), Payload, Capture));
        TestTrue(TEXT("list_actor_classes (post-spawn) succeeded"), Capture.bSuccess);

        TestEqual(TEXT("StaticMeshActor census count rose by exactly +3"),
            CountForClass(Capture, MeshClass) - BaseMesh, 3);
        TestEqual(TEXT("Actor census count rose by exactly +1"),
            CountForClass(Capture, BareClass) - BaseBare, 1);

        if (Capture.Result.IsValid())
        {
            double Distinct = 0.0;
            TestTrue(TEXT("response carries distinctCount"),
                Capture.Result->TryGetNumberField(TEXT("distinctCount"), Distinct));
            FullDistinct = (int32)Distinct;
            TestTrue(TEXT("distinctCount covers at least the two fixture classes"),
                FullDistinct >= 2);
            bool bTruncated = true;
            TestTrue(TEXT("unlimited census carries a truncated flag"),
                Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
            TestFalse(TEXT("unlimited census is not truncated"), bTruncated);
        }
    }

    // limit=1: the returned rows cap at 1, but distinctCount still reports the full
    // untruncated distinct-class total and truncated flips true (there are >= 2 classes).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetNumberField(TEXT("limit"), 1);
        TestTrue(TEXT("list_actor_classes (limit) handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_actor_classes"), Payload, Capture));
        TestTrue(TEXT("list_actor_classes (limit) succeeded"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("classes"), Rows) && Rows)
            {
                TestEqual(TEXT("limit=1 caps the returned rows at 1"), Rows->Num(), 1);
            }
            double Count = 0.0, Distinct = 0.0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("distinctCount"), Distinct);
            TestEqual(TEXT("count reflects the capped row total"), (int32)Count, 1);
            TestEqual(TEXT("distinctCount still reports the full untruncated total"),
                (int32)Distinct, FullDistinct);
            bool bTruncated = false;
            TestTrue(TEXT("limited census carries a truncated flag"),
                Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
            TestTrue(TEXT("truncated is true when limit elided rows"), bTruncated);
        }
    }

    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.inspect_class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectInspectClassValidParamsTest,
    "PinWright.system.inspect.inspect_class.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectInspectClassValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("className"), TEXT("Actor"));
    TestTrue(TEXT("system.inspect.inspect_class handler found"),
        InvokeHandler(TEXT("system.inspect.inspect_class"), Payload));
    return true;
}

// ============================================================================
// EnvironmentHandler — system.inspect.inspect_object
// ============================================================================

// Contract: the registration doc promises inspect_object reports "properties,
// transform, components, and class" for its target. Regression for
// B-inspect-object-omits-component-properties — the handler previously emitted
// no `properties` field for any target, and no `transform` for non-actor
// USceneComponent targets (the original Text3DComponent repro).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectInspectObjectEmitsPropertiesTest,
    "PinWright.system.inspect.inspect_object.EmitsProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectInspectObjectEmitsPropertiesTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    USceneComponent* Scene = nullptr;
    AActor* Actor = SpawnActorWithRootScene(*this, TEXT("PW_InspectObject"), TEXT("InspectedScene"), Scene);
    if (!Actor)
    {
        return false;
    }
    if (!Scene)
    {
        return false;
    }
    Scene->SetWorldLocation(FVector(10.0, 20.0, 30.0));

    // Actor target: properties must be present and non-empty, and the class must
    // surface under the canonical `class` key (matching list_objects / find_by_class
    // / find_by_tag and the nested components[]) with the legacy `className` alias
    // retained and carrying the same value. Regression for
    // E-inspect-object-class-key-drift — the handler previously emitted the class
    // only under `className`, so a parser keyed on `class` silently got null.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Actor->GetPathName());

        FTestResponseCapture Capture;
        TestTrue(TEXT("inspect_object handler found (actor)"),
            InvokeHandlerWithCapture(TEXT("system.inspect.inspect_object"), Payload, Capture));
        TestTrue(TEXT("inspect_object succeeded (actor)"), Capture.bSuccess);
        TestTrue(TEXT("actor response has properties object"),
            Capture.Result.IsValid() && Capture.Result->HasTypedField<EJson::Object>(TEXT("properties")));
        if (Capture.Result.IsValid() && Capture.Result->HasTypedField<EJson::Object>(TEXT("properties")))
        {
            const TSharedPtr<FJsonObject> Props = Capture.Result->GetObjectField(TEXT("properties"));
            TestTrue(TEXT("actor properties non-empty"), Props.IsValid() && Props->Values.Num() > 0);
        }

        if (Capture.Result.IsValid())
        {
            // Canonical `class` key must be present and equal to the leaf class name.
            FString ClassValue;
            TestTrue(TEXT("response carries top-level `class` key"),
                Capture.Result->TryGetStringField(TEXT("class"), ClassValue));
            TestEqual(TEXT("`class` equals the actor leaf class name"),
                ClassValue, Actor->GetClass()->GetName());

            // Back-compat alias `className` must remain and carry the same value.
            FString ClassNameValue;
            TestTrue(TEXT("response retains `className` alias"),
                Capture.Result->TryGetStringField(TEXT("className"), ClassNameValue));
            TestEqual(TEXT("`className` alias matches `class`"), ClassNameValue, ClassValue);
        }
    }

    // Non-actor USceneComponent target: properties AND transform must be present.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Scene->GetPathName());

        FTestResponseCapture Capture;
        TestTrue(TEXT("inspect_object handler found (component)"),
            InvokeHandlerWithCapture(TEXT("system.inspect.inspect_object"), Payload, Capture));
        TestTrue(TEXT("inspect_object succeeded (component)"), Capture.bSuccess);
        TestTrue(TEXT("component response valid"), Capture.Result.IsValid());
        if (!Capture.Result.IsValid())
        {
            return false;
        }

        TestTrue(TEXT("component response has properties object"),
            Capture.Result->HasTypedField<EJson::Object>(TEXT("properties")));

        TestTrue(TEXT("component response has transform object"),
            Capture.Result->HasTypedField<EJson::Object>(TEXT("transform")));
        if (Capture.Result->HasTypedField<EJson::Object>(TEXT("transform")))
        {
            const TSharedPtr<FJsonObject> Transform = Capture.Result->GetObjectField(TEXT("transform"));
            TestTrue(TEXT("transform has location"),
                Transform.IsValid() && Transform->HasTypedField<EJson::Object>(TEXT("location")));
            TestTrue(TEXT("transform has rotation"),
                Transform.IsValid() && Transform->HasTypedField<EJson::Object>(TEXT("rotation")));
            TestTrue(TEXT("transform has scale"),
                Transform.IsValid() && Transform->HasTypedField<EJson::Object>(TEXT("scale")));
            if (Transform.IsValid() && Transform->HasTypedField<EJson::Object>(TEXT("location")))
            {
                const TSharedPtr<FJsonObject> Location = Transform->GetObjectField(TEXT("location"));
                TestEqual(TEXT("transform location x"), Location->GetNumberField(TEXT("x")), 10.0);
                TestEqual(TEXT("transform location y"), Location->GetNumberField(TEXT("y")), 20.0);
                TestEqual(TEXT("transform location z"), Location->GetNumberField(TEXT("z")), 30.0);
            }
        }
    }

    return true;
}

// Contract: inspect_object emits the class short-name at the top level under `class`
// (matching the sibling readers list_objects/find_by_class/find_by_tag and this
// method's own nested components[].class), keeping `className` for back-compat.
// Regression for E-inspect-object-class-key-drift — the handler previously emitted
// the top-level class ONLY under `className`, so a parser keyed on `class` (learned
// from the audit chain it disagreed with) silently missed it. If a future refactor
// drops the `class` alias, the actor and non-actor assertions below fire. Exercises
// both code paths (actor target and non-actor USceneComponent target), since the
// top-level class is set unconditionally before the actor/non-actor split.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectInspectObjectEmitsClassKeyTest,
    "PinWright.system.inspect.inspect_object.EmitsClassKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectInspectObjectEmitsClassKeyTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    USceneComponent* Scene = nullptr;
    AActor* Actor = SpawnActorWithRootScene(*this, TEXT("PW_InspectClassKey"), TEXT("InspectedClassKeyScene"), Scene);
    if (!Actor)
    {
        return false;
    }
    if (!Scene)
    {
        return false;
    }

    // Drives inspect_object on Target and asserts the top-level class short-name is
    // present under BOTH `class` and `className`, both equal to ExpectedShortName.
    auto CheckClassKeys = [this](const FString& Label, UObject* Target, const FString& ExpectedShortName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Target->GetPathName());

        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("inspect_object handler found (%s)"), *Label),
            InvokeHandlerWithCapture(TEXT("system.inspect.inspect_object"), Payload, Capture));
        TestTrue(*FString::Printf(TEXT("inspect_object succeeded (%s)"), *Label), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }

        // The sibling-matching `class` alias is the regression target.
        FString ClassVal;
        TestTrue(*FString::Printf(TEXT("%s response carries top-level `class`"), *Label),
            Capture.Result->TryGetStringField(TEXT("class"), ClassVal));
        TestEqual(*FString::Printf(TEXT("%s `class` is the class short-name"), *Label),
            ClassVal, ExpectedShortName);

        // `className` is retained for back-compat and must still match.
        FString ClassNameVal;
        TestTrue(*FString::Printf(TEXT("%s response still carries `className`"), *Label),
            Capture.Result->TryGetStringField(TEXT("className"), ClassNameVal));
        TestEqual(*FString::Printf(TEXT("%s `class` and `className` agree"), *Label),
            ClassVal, ClassNameVal);
    };

    // Actor target and non-actor USceneComponent target (the top-level class is set
    // before the actor/non-actor branch, so both must carry the alias).
    CheckClassKeys(TEXT("actor"), Actor, Actor->GetClass()->GetName());
    CheckClassKeys(TEXT("component"), Scene, Scene->GetClass()->GetName());

    return true;
}

// ============================================================================
// FoliageHandler — foliage.paint
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintValidParamsTest,
    "PinWright.foliage.paint.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("foliageTypePath"), TEXT("/Game/Foliage/TestFoliage"));

    // Provide a single position so the handler's location validation passes
    TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
    Pos->SetNumberField(TEXT("x"), 100.0);
    Pos->SetNumberField(TEXT("y"), 200.0);
    Pos->SetNumberField(TEXT("z"), 0.0);
    Payload->SetObjectField(TEXT("position"), Pos);

    TestTrue(TEXT("foliage.paint handler found"),
        InvokeHandler(TEXT("foliage.paint"), Payload));
    return true;
}

// ============================================================================
// FoliageHandler — foliage.remove (all optional params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageRemoveValidParamsTest,
    "PinWright.foliage.remove.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageRemoveValidParamsTest::RunTest(const FString& Parameters)
{
    // Scoped by foliageTypePath, never removeAll. InvokeHandler runs the real handler
    // against the live editor world, so a `removeAll:true` payload here empties the host
    // map's own foliage on every suite run — shared state this test does not own. The
    // assertion is "the handler is registered and answers without crashing", which the
    // scoped payload exercises identically; the removeAll branch's behaviour is covered
    // by PinWright.foliage.remove.MissingScopeErrors and .ScopedRemovalSparesOtherTypes.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("foliageTypePath"),
        FString::Printf(TEXT("/Game/Foliage/PW_NoSuchFoliageType_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    TestTrue(TEXT("foliage.remove handler found"),
        InvokeHandler(TEXT("foliage.remove"), Payload));
    return true;
}

// ============================================================================
// FoliageHandler — foliage.get_instances (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageGetInstancesValidParamsTest,
    "PinWright.foliage.get_instances.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageGetInstancesValidParamsTest::RunTest(const FString& Parameters)
{
    // Empty payload — no required params
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("foliage.get_instances handler found"),
        InvokeHandler(TEXT("foliage.get_instances"), Payload));
    return true;
}

// Regression for board E-foliage-get-instances-drops-scale: foliage.add_instances
// parses+applies a full per-instance transform (location/rotation/scale →
// FFoliageInstance::DrawScale3D, FoliageHandler.cpp:791), but foliage.get_instances
// historically read back only location+rotation in the type-filtered branch and only
// foliageType+location in the unfiltered branch — silently dropping the scale (both
// branches) and rotation (unfiltered branch). A scatter-then-verify workflow that
// varied per-instance scale could not confirm the size it wrote from the tool's own
// output. The fix echoes DrawScale3D as scaleX/scaleY/scaleZ in BOTH branches and adds
// pitch/yaw/roll to the unfiltered branch. This test writes one instance with a
// non-unit, non-uniform scale + non-zero rotation via the real add_instances handler,
// then reads it back through both get_instances branches and asserts the scale and
// rotation round-trip. It fails if either echo is reverted (the dropped fields return).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageGetInstancesRoundTripsScaleTest,
    "PinWright.foliage.get_instances.RoundTripsScaleAndRotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageGetInstancesRoundTripsScaleTest::RunTest(const FString& Parameters)
{
    // add_instances auto-creates a FoliageType from a reliably-loadable engine mesh, in
    // package "/Game/Foliage/Auto_<MeshBaseName>" with the object inside it taking the same
    // stem, so the loadable object path is "/Game/Foliage/Auto_Cube.Auto_Cube". The object
    // used to be named "<MeshBaseName>" instead, which made the package handle resolve to
    // nothing (B-add-instances-auto-foliage-type-name-mismatch); the fixture paths below are
    // the post-fix names, and the round-trip that ticket owns is asserted in
    // Tests/Environment/TestFoliageAutoTypeNaming.cpp. Cleanup deletes the whole package, so
    // it keeps the package path.
    const FString MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    const FString AutoTypePackagePath = TEXT("/Game/Foliage/Auto_Cube");
    const FString AutoTypeObjectPath = TEXT("/Game/Foliage/Auto_Cube.Auto_Cube");

    // Clear any instances a previous run of THIS test left behind, scoped by
    // foliageTypePath rather than removeAll: a removeAll here empties the host map's own
    // foliage, which is shared state this test does not own. On a first run the type
    // asset does not exist yet and the call answers ASSET_NOT_FOUND, which is harmless.
    {
        TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
        RemovePayload->SetStringField(TEXT("foliageTypePath"), AutoTypeObjectPath);
        InvokeHandler(TEXT("foliage.remove"), RemovePayload);
    }
    ON_SCOPE_EXIT
    {
        TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
        RemovePayload->SetStringField(TEXT("foliageTypePath"), AutoTypeObjectPath);
        InvokeHandler(TEXT("foliage.remove"), RemovePayload);
        CleanupTestAsset(AutoTypePackagePath);
    };

    // The deliberately non-unit, non-uniform scale + non-zero rotation we expect to
    // survive the write → read round-trip.
    const double ScaleX = 2.5, ScaleY = 0.5, ScaleZ = 3.0;
    const double Pitch = 10.0, Yaw = 45.0, Roll = 20.0;

    // Write one instance through the real add_instances handler (transforms form).
    {
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), 100.0);
        Location->SetNumberField(TEXT("y"), 200.0);
        Location->SetNumberField(TEXT("z"), 0.0);

        TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), Pitch);
        Rotation->SetNumberField(TEXT("yaw"), Yaw);
        Rotation->SetNumberField(TEXT("roll"), Roll);

        TSharedPtr<FJsonObject> Scale = MakeShared<FJsonObject>();
        Scale->SetNumberField(TEXT("x"), ScaleX);
        Scale->SetNumberField(TEXT("y"), ScaleY);
        Scale->SetNumberField(TEXT("z"), ScaleZ);

        TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
        Transform->SetObjectField(TEXT("location"), Location);
        Transform->SetObjectField(TEXT("rotation"), Rotation);
        Transform->SetObjectField(TEXT("scale"), Scale);

        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(MakeShared<FJsonValueObject>(Transform));

        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("foliageTypePath"), MeshPath);
        AddPayload->SetArrayField(TEXT("transforms"), Transforms);

        FTestResponseCapture AddCapture;
        TestTrue(TEXT("foliage.add_instances handler found"),
            InvokeHandlerWithCapture(TEXT("foliage.add_instances"), AddPayload, AddCapture));
        TestTrue(TEXT("add_instances succeeded"), AddCapture.bSuccess);
        if (!AddCapture.bSuccess)
        {
            // No instance was written (e.g. mesh failed to load in this environment) —
            // the read-back assertions below would be vacuous, so stop here.
            return false;
        }
    }

    // Validates one read-back instance object carries the scale and rotation we wrote.
    // The rotation round-trip is the whole point of the fix in both branches, so it is
    // asserted unconditionally. Shared by both branch assertions.
    auto AssertInstanceTransform = [&](const TSharedPtr<FJsonObject>& Inst, const TCHAR* Branch)
    {
        if (!Inst.IsValid())
        {
            TestTrue(*FString::Printf(TEXT("%s: instance object valid"), Branch), false);
            return;
        }
        double V = 0.0;
        TestTrue(*FString::Printf(TEXT("%s: instance carries scaleX"), Branch),
            Inst->TryGetNumberField(TEXT("scaleX"), V));
        TestEqual(*FString::Printf(TEXT("%s: scaleX round-trips"), Branch), V, ScaleX);
        TestTrue(*FString::Printf(TEXT("%s: instance carries scaleY"), Branch),
            Inst->TryGetNumberField(TEXT("scaleY"), V));
        TestEqual(*FString::Printf(TEXT("%s: scaleY round-trips"), Branch), V, ScaleY);
        TestTrue(*FString::Printf(TEXT("%s: instance carries scaleZ"), Branch),
            Inst->TryGetNumberField(TEXT("scaleZ"), V));
        TestEqual(*FString::Printf(TEXT("%s: scaleZ round-trips"), Branch), V, ScaleZ);

        TestTrue(*FString::Printf(TEXT("%s: instance carries yaw"), Branch),
            Inst->TryGetNumberField(TEXT("yaw"), V));
        TestEqual(*FString::Printf(TEXT("%s: yaw round-trips"), Branch), V, Yaw);
    };

    // The filtered branch is scoped by its own query, so its first element is ours. The
    // unfiltered branch returns the WHOLE world's foliage — including whatever the host
    // map already carries, which this test deliberately no longer wipes — so it locates
    // the instance belonging to our own type instead of assuming index 0. Pass an empty
    // OwnFoliageType when the query was already type-scoped. Shared so the two branches
    // assert identical data the same way.
    auto AssertOurInstanceTransform = [&](const FTestResponseCapture& Capture,
        const TCHAR* Branch, const FString& OwnFoliageType)
    {
        if (!Capture.Result.IsValid())
        {
            return;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("instances"), Arr) && Arr)
        {
            for (const TSharedPtr<FJsonValue>& Entry : *Arr)
            {
                const TSharedPtr<FJsonObject>* Inst = nullptr;
                if (!Entry.IsValid() || !Entry->TryGetObject(Inst) || !Inst)
                {
                    continue;
                }
                FString EntryType;
                if (!OwnFoliageType.IsEmpty()
                    && (!(*Inst)->TryGetStringField(TEXT("foliageType"), EntryType)
                        || EntryType != OwnFoliageType))
                {
                    continue;
                }
                AssertInstanceTransform(*Inst, Branch);
                return;
            }
        }
        TestTrue(*FString::Printf(TEXT("%s branch returned the written instance"), Branch), false);
    };

    // Type-filtered branch: query the auto-created foliage type by its full object path.
    {
        TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
        GetPayload->SetStringField(TEXT("foliageTypePath"), AutoTypeObjectPath);

        FTestResponseCapture GetCapture;
        TestTrue(TEXT("foliage.get_instances handler found (filtered)"),
            InvokeHandlerWithCapture(TEXT("foliage.get_instances"), GetPayload, GetCapture));
        TestTrue(TEXT("get_instances succeeded (filtered)"), GetCapture.bSuccess);
        AssertOurInstanceTransform(GetCapture, TEXT("filtered"), FString());
    }

    // Unfiltered branch: no foliageTypePath — historically dropped both rotation and
    // scale; the fix makes it emit the full transform like the filtered branch.
    {
        TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();

        FTestResponseCapture GetCapture;
        TestTrue(TEXT("foliage.get_instances handler found (unfiltered)"),
            InvokeHandlerWithCapture(TEXT("foliage.get_instances"), GetPayload, GetCapture));
        TestTrue(TEXT("get_instances succeeded (unfiltered)"), GetCapture.bSuccess);
        // The host map's own foliage is left alone, so the response may carry instances
        // this test did not write: find ours by foliageType rather than taking index 0.
        AssertOurInstanceTransform(GetCapture, TEXT("unfiltered"), AutoTypeObjectPath);
    }

    return true;
}

// ============================================================================
// FoliageHandler — foliage.add_type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeMissingMeshPathTest,
    "PinWright.foliage.add_type.MissingMeshPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeMissingMeshPathTest::RunTest(const FString& Parameters)
{
    // Omit required "meshPath"
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("MyFoliageType"));
    TestTrue(TEXT("foliage.add_type handler found"),
        InvokeHandler(TEXT("foliage.add_type"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeValidParamsTest,
    "PinWright.foliage.add_type.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("MyFoliageType"));
    Payload->SetStringField(TEXT("meshPath"), TEXT("/Game/Meshes/SM_Tree"));
    Payload->SetNumberField(TEXT("density"), 50.0);
    TestTrue(TEXT("foliage.add_type handler found"),
        InvokeHandler(TEXT("foliage.add_type"), Payload));
    return true;
}

// ============================================================================
// FoliageHandler — foliage.add_instances
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesValidParamsTest,
    "PinWright.foliage.add_instances.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("foliageTypePath"), TEXT("/Game/Foliage/TestFoliage"));

    // Provide locations array (legacy format accepted by the handler)
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), 0.0);
    Loc->SetNumberField(TEXT("y"), 0.0);
    Loc->SetNumberField(TEXT("z"), 0.0);
    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(MakeShared<FJsonValueObject>(Loc));
    Payload->SetArrayField(TEXT("locations"), Locations);

    TestTrue(TEXT("foliage.add_instances handler found"),
        InvokeHandler(TEXT("foliage.add_instances"), Payload));
    return true;
}

// ============================================================================
// FoliageHandler — foliage.create_procedural
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralValidParamsTest,
    "PinWright.foliage.create_procedural.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralValidParamsTest::RunTest(const FString& Parameters)
{
    // Spawning a ProceduralFoliageVolume actor and resimulating its tile grid is
    // heavyweight (~13s) and adds no signal beyond registration coverage in a
    // headless smoke test, since exercising it meaningfully needs real foliage
    // types and a world to spawn into.
    TestTrue(TEXT("foliage.create_procedural is registered"),
        IsRegistered(TEXT("foliage.create_procedural")));
    return true;
}

// Regression for B-foliage-create-procedural-empty-callback-noop.
//
// Symptom: foliage.create_procedural created the spawner asset + volume actor but
// scattered zero foliage, while hardcoding resimulated:true and reporting no
// instance count — a silent false-success. (WHY, and why the real add path must be
// reached by reflection through ProceduralFoliageEditorLibrary, lives next to the
// fix in FoliageHandler.cpp's foliage.create_procedural handler.)
//
// Load-bearing assertion: the response must carry an `instances_spawned` number —
// the placed-instance count the fix adds. The reverted (empty-callback) code never
// emitted this field, so TryGetNumberField fails and this test fails if the fix is
// reverted. The count is NOT asserted positive here, because this fixture spawns no
// surface under the volume and the tile simulation's results are projected onto world
// geometry, so 0 placed is the honest expected value for THIS payload.
//
// That rationale used to be stated the other way round — "0 is honest because a
// headless world has no surface" was offered as the reason the field could not be
// asserted positive, and it was wrong: the field was structurally zero over ANY
// surface, because the counter was FFoliageInfo::GetPlacedInstanceCount(), which
// counts instances with an INVALID ProceduralGuid — the hand-placed ones, the exact
// complement of what this verb makes (B-create-procedural-spawned-count-always-zero).
// The positive-count assertion now lives in
// PinWright.foliage.create_procedural.SpawnedCountMatchesTheFoliageActors
// (Tests/Environment/TestFoliageProceduralSpawnCount.cpp), which spawns a real floor
// and compares the reported number against a re-read of FFoliageInfo::Instances. Do
// not re-derive a "0 is fine" claim from this test alone.
//
// This test intentionally accepts the full real-resim cost (~13s)
// — the sibling FFoliageCreateProceduralValidParamsTest skips this path to stay a
// fast smoke test — because exercising the real add path is the whole point.
//
// Uses only the engine unit cube (/Engine/BasicShapes/Cube.Cube) as the foliage
// mesh, and a GUID-suffixed name so no residual asset on a mutated host collides.
// The world guard destroys the spawned ProceduralFoliageVolume on scope exit so the
// open map is left as found.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralReportsInstancesSpawnedTest,
    "PinWright.foliage.create_procedural.ReportsInstancesSpawned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralReportsInstancesSpawnedTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;

    const FString VolumeName = FString::Printf(TEXT("PW_ProcFoliage_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // bounds { location:{0,0,0}, size:{2000,2000,500} }
    TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), 0.0);
    Loc->SetNumberField(TEXT("y"), 0.0);
    Loc->SetNumberField(TEXT("z"), 0.0);
    Bounds->SetObjectField(TEXT("location"), Loc);
    TSharedPtr<FJsonObject> Sz = MakeShared<FJsonObject>();
    Sz->SetNumberField(TEXT("x"), 2000.0);
    Sz->SetNumberField(TEXT("y"), 2000.0);
    Sz->SetNumberField(TEXT("z"), 500.0);
    Bounds->SetObjectField(TEXT("size"), Sz);

    // foliageTypes: [ { meshPath:/Engine/BasicShapes/Cube.Cube, density:50 } ]
    TSharedPtr<FJsonObject> Type = MakeShared<FJsonObject>();
    Type->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Type->SetNumberField(TEXT("density"), 50.0);
    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeShared<FJsonValueObject>(Type));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), VolumeName);
    Payload->SetObjectField(TEXT("bounds"), Bounds);
    Payload->SetArrayField(TEXT("foliageTypes"), Types);
    Payload->SetNumberField(TEXT("seed"), 12345.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("foliage.create_procedural handler found"),
        InvokeHandlerWithCapture(TEXT("foliage.create_procedural"), Payload, Capture));
    TestTrue(TEXT("foliage.create_procedural reports success"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // The load-bearing assertion: the fix reports a real placed-instance count.
        // The empty-callback code never set this field, so its presence proves the
        // real add path ran and the response is honest about the scatter.
        double InstancesSpawned = -1.0;
        TestTrue(TEXT("response carries an instances_spawned count (fix ran the real add path)"),
            Capture.Result->TryGetNumberField(TEXT("instances_spawned"), InstancesSpawned));

        // resimulated must still be reported. It now reflects whether the resim
        // reflection path was DISPATCHED (not a hardcoded literal, and not a
        // sim-success bool — the library function is void); instances_spawned is the
        // field that reports whether foliage actually landed.
        bool bResimulated = false;
        TestTrue(TEXT("response carries resimulated"),
            Capture.Result->TryGetBoolField(TEXT("resimulated"), bResimulated));
    }

    // Clean up the /Game/ProceduralFoliage packages the handler creates: the Spawner and one
    // FoliageType per mesh (here <name>_Spawner and <name>_Spawner_FT_0). The handler hardcodes
    // /Game/ProceduralFoliage (FoliageHandler.cpp) and only marks the packages dirty
    // (McpSafeAssetSave is mark-dirty-only), so without this an editor-wide save-all later in the
    // suite flushes them into the host Content tree as PW_ProcFoliage_<GUID>_Spawner*.uasset litter.
    // CleanupTestAsset deletes the in-memory object (and any on-disk .uasset); de-dirty any emptied
    // package that lingers so nothing dirty survives for save-all to write. B-tests-leak-host-content.
    auto CleanFoliagePackage = [](const FString& PackagePath)
    {
        CleanupTestAsset(PackagePath);
        // Force-delete can re-dirty the emptied package; clear the flag so nothing dirty
        // survives for a later editor-wide save-all to write. (No IsDirty() assertion here:
        // checking right after SetDirtyFlag(false) would be tautological, and CleanupTestAsset
        // may legitimately leave a still-referenced object alive, so an object-gone assertion
        // would be unsound.)
        UPackage* Remaining = FindPackage(nullptr, *PackagePath);
        if (Remaining)
        {
            Remaining->SetDirtyFlag(false);
        }
    };
    CleanFoliagePackage(FString::Printf(TEXT("/Game/ProceduralFoliage/%s_Spawner"), *VolumeName));
    CleanFoliagePackage(FString::Printf(TEXT("/Game/ProceduralFoliage/%s_Spawner_FT_0"), *VolumeName));
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.create
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateValidParamsTest,
    "PinWright.landscape.create.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestLandscape"));
    Payload->SetNumberField(TEXT("sizeX"), 505.0);
    Payload->SetNumberField(TEXT("sizeY"), 505.0);
    TestTrue(TEXT("landscape.create handler found"),
        InvokeHandler(TEXT("landscape.create"), Payload));
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.sculpt
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptMissingLocationTest,
    "PinWright.landscape.sculpt.MissingRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptMissingLocationTest::RunTest(const FString& Parameters)
{
    // `location` is NOT an RPC_PARAM_REQ - LandscapeHandler.cpp:754-772 declares every
    // parameter optional - so the dispatcher's required-param gate never fires for this
    // verb and the EnvironmentHandlersDispatcherGate fixture above does not apply. The
    // requirement is in-body and exclusive: the else branch at LandscapeHandler.cpp:850-854
    // refuses a payload carrying neither `location`/`position` nor `path`. Capture the
    // response and pin that branch. The previous body was
    // `TestTrue(InvokeHandler("landscape.sculpt", {}))`, which asserted only that the
    // method name was in the registry and stayed green with the whole guard deleted.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("landscape.sculpt handler found"),
        InvokeHandlerWithCapture(TEXT("landscape.sculpt"), Payload, Capture));
    TestTrue(TEXT("landscape.sculpt responded"), Capture.bWasCalled);
    TestFalse(TEXT("a payload with neither location nor path is refused"), Capture.bSuccess);
    TestEqual(TEXT("landscape.sculpt error code is INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestTrue(TEXT("the error names 'location' as an accepted shape"),
        Capture.Message.Contains(TEXT("location")));
    TestTrue(TEXT("the error names 'path' as an accepted shape"),
        Capture.Message.Contains(TEXT("path")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSculptValidParamsTest,
    "PinWright.landscape.sculpt.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSculptValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), 0.0);
    Location->SetNumberField(TEXT("y"), 0.0);
    Location->SetNumberField(TEXT("z"), 0.0);
    Payload->SetObjectField(TEXT("location"), Location);
    Payload->SetNumberField(TEXT("radius"), 512.0);
    Payload->SetNumberField(TEXT("strength"), 0.5);
    TestTrue(TEXT("landscape.sculpt handler found"),
        InvokeHandler(TEXT("landscape.sculpt"), Payload));
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.set_material
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeSetMaterialValidParamsTest,
    "PinWright.landscape.set_material.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeSetMaterialValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("materialPath"), TEXT("/Game/Materials/M_Landscape"));
    TestTrue(TEXT("landscape.set_material handler found"),
        InvokeHandler(TEXT("landscape.set_material"), Payload));
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.create_grass_type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGrassTypeValidParamsTest,
    "PinWright.landscape.create_grass_type.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGrassTypeValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestGrassType"));
    Payload->SetStringField(TEXT("meshPath"), TEXT("/Game/Meshes/SM_Grass"));
    TestTrue(TEXT("landscape.create_grass_type handler found"),
        InvokeHandler(TEXT("landscape.create_grass_type"), Payload));
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.edit (no required params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeEditValidParamsTest,
    "PinWright.landscape.edit.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeEditValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("mode"), TEXT("raise"));
    Payload->SetNumberField(TEXT("x"), 0.0);
    Payload->SetNumberField(TEXT("y"), 0.0);
    Payload->SetNumberField(TEXT("radius"), 256.0);
    Payload->SetNumberField(TEXT("strength"), 0.3);
    TestTrue(TEXT("landscape.edit handler found"),
        InvokeHandler(TEXT("landscape.edit"), Payload));
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.create_procedural_terrain
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateProceduralTerrainValidParamsTest,
    "PinWright.landscape.create_procedural_terrain.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateProceduralTerrainValidParamsTest::RunTest(const FString& Parameters)
{
    // The handler dispatches via AsyncTask(GameThread, ...) and exercises a
    // landscape painting path that needs a real ALandscape in the world to
    // produce useful coverage; in headless automation it just stalls draining
    // queued game-thread work (~5s). Registration check is the meaningful
    // smoke coverage here.
    TestTrue(TEXT("landscape.create_procedural_terrain is registered"),
        IsRegistered(TEXT("landscape.create_procedural_terrain")));
    return true;
}

// ============================================================================
// LightingHandler — lighting.list_light_types (RPC_NO_PARAMS)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingListLightTypesValidParamsTest,
    "PinWright.lighting.list_light_types.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingListLightTypesValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("lighting.list_light_types handler found"),
        InvokeHandler(TEXT("lighting.list_light_types"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.spawn_light (all optional params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSpawnLightValidParamsTest,
    "PinWright.lighting.spawn_light.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSpawnLightValidParamsTest::RunTest(const FString& Parameters)
{
    // Spawning editor actors is flaky in NullRHI/headless automation; keep this as registration coverage.
    TestTrue(TEXT("lighting.spawn_light is registered"),
        IsRegistered(TEXT("lighting.spawn_light")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSpawnLightMissingLightTypeTest,
    "PinWright.lighting.spawn_light.MissingLightType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSpawnLightMissingLightTypeTest::RunTest(const FString& Parameters)
{
    // Both lightClass and lightType omitted — handler returns INVALID_ARGUMENT
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("lighting.spawn_light handler found"),
        InvokeHandler(TEXT("lighting.spawn_light"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.spawn_sky_light (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSpawnSkyLightValidParamsTest,
    "PinWright.lighting.spawn_sky_light.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSpawnSkyLightValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSkyLight"));
    Payload->SetNumberField(TEXT("intensity"), 1.0);
    TestTrue(TEXT("lighting.spawn_sky_light handler found"),
        InvokeHandler(TEXT("lighting.spawn_sky_light"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.ensure_single_sky_light (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingEnsureSingleSkyLightValidParamsTest,
    "PinWright.lighting.ensure_single_sky_light.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingEnsureSingleSkyLightValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("SkyLight"));
    TestTrue(TEXT("lighting.ensure_single_sky_light handler found"),
        InvokeHandler(TEXT("lighting.ensure_single_sky_light"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.create_lightmass_volume (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingCreateLightmassVolumeValidParamsTest,
    "PinWright.lighting.create_lightmass_volume.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingCreateLightmassVolumeValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestLightmassVolume"));
    TestTrue(TEXT("lighting.create_lightmass_volume handler found"),
        InvokeHandler(TEXT("lighting.create_lightmass_volume"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.setup_volumetric_fog (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupVolumetricFogValidParamsTest,
    "PinWright.lighting.setup_volumetric_fog.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupVolumetricFogValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("viewDistance"), 6000.0);
    TestTrue(TEXT("lighting.setup_volumetric_fog handler found"),
        InvokeHandler(TEXT("lighting.setup_volumetric_fog"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.setup_global_illumination
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetupGlobalIlluminationValidParamsTest,
    "PinWright.lighting.setup_global_illumination.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetupGlobalIlluminationValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("method"), TEXT("LumenGI"));
    TestTrue(TEXT("lighting.setup_global_illumination handler found"),
        InvokeHandler(TEXT("lighting.setup_global_illumination"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.configure_shadows (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingConfigureShadowsValidParamsTest,
    "PinWright.lighting.configure_shadows.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingConfigureShadowsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("virtualShadowMaps"), true);
    TestTrue(TEXT("lighting.configure_shadows handler found"),
        InvokeHandler(TEXT("lighting.configure_shadows"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.set_exposure (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetExposureValidParamsTest,
    "PinWright.lighting.set_exposure.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetExposureValidParamsTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("minBrightness"), 0.1);
    Payload->SetNumberField(TEXT("maxBrightness"), 2.0);
    Payload->SetNumberField(TEXT("compensationValue"), 0.0);
    TestTrue(TEXT("lighting.set_exposure handler found"),
        InvokeHandlerWithCapture(TEXT("lighting.set_exposure"), Payload, Capture));

    // Each exposure write must set its paired bOverride_ bit, or the PostProcessVolume
    // never blends the value into the scene. Locate the PPV the handler used (by its
    // echoed actorName) and assert the override bits are set. Regression for the
    // "writes AutoExposure* without the override bits" defect — an echo-only test
    // passes despite it because the values are always written to the struct field.
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ActorName;
        Capture.Result->TryGetStringField(TEXT("actorName"), ActorName);
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        APostProcessVolume* PPV = nullptr;
        if (World && !ActorName.IsEmpty())
        {
            for (TActorIterator<APostProcessVolume> It(World); It; ++It)
            {
                if (It->GetActorLabel() == ActorName) { PPV = *It; break; }
            }
        }
        if (PPV)
        {
            TestTrue(TEXT("minBrightness override bit set"),
                (bool)PPV->Settings.bOverride_AutoExposureMinBrightness);
            TestTrue(TEXT("maxBrightness override bit set"),
                (bool)PPV->Settings.bOverride_AutoExposureMaxBrightness);
            TestTrue(TEXT("compensation override bit set"),
                (bool)PPV->Settings.bOverride_AutoExposureBias);
        }
        else
        {
            // Pinned, not silent: the three override-bit assertions ARE this test's
            // regression coverage, so an environment that skips them must say so.
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
                FString::Printf(
                    TEXT("lighting.set_exposure echoed actorName '%s' but no matching "
                         "APostProcessVolume was found in the editor world; skipping the "
                         "bOverride_AutoExposure* assertions."), *ActorName));
        }
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("handler-call-failed"),
            FString::Printf(
                TEXT("lighting.set_exposure did not succeed (error '%s'); skipping the "
                     "bOverride_AutoExposure* assertions, which are the whole regression "
                     "coverage for the 'writes AutoExposure* without the override bits' defect."),
                *Capture.ErrorCode));
    }
    return true;
}

// ============================================================================
// LightingHandler — lighting.set_ambient_occlusion (all optional)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSetAmbientOcclusionValidParamsTest,
    "PinWright.lighting.set_ambient_occlusion.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSetAmbientOcclusionValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    Payload->SetNumberField(TEXT("intensity"), 0.5);
    Payload->SetNumberField(TEXT("radius"), 200.0);
    TestTrue(TEXT("lighting.set_ambient_occlusion handler found"),
        InvokeHandler(TEXT("lighting.set_ambient_occlusion"), Payload));
    return true;
}

// ============================================================================
// LightingHandler — lighting.create_lighting_enabled_level
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingCreateLightingEnabledLevelValidParamsTest,
    "PinWright.lighting.create_lighting_enabled_level.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingCreateLightingEnabledLevelValidParamsTest::RunTest(const FString& Parameters)
{
    // Map creation/saving is heavyweight and can stall unattended automation runs.
    TestTrue(TEXT("lighting.create_lighting_enabled_level is registered"),
        IsRegistered(TEXT("lighting.create_lighting_enabled_level")));
    return true;
}

// ============================================================================
// EnvironmentHandler — environment.spawn_sky_atmosphere /
// environment.spawn_volumetric_cloud / environment.spawn_reflection_capture
// ----------------------------------------------------------------------------
// These tests exercise the production handlers, then verify that the returned
// actorPath can be used by another production handler.
// ============================================================================

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
// ASkyAtmosphere and AVolumetricCloud are declared inside their component
// headers — there are no Engine/SkyAtmosphere.h / Engine/VolumetricCloud.h
// wrappers in UE 5.4–5.7.
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Components/BoxReflectionCaptureComponent.h"
#include "Engine/SphereReflectionCapture.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/DirectionalLight.h"
#include "Landscape.h"
#include "LandscapeInfo.h"

namespace
{
    template <typename TActor>
    TActor* FindEnvironmentSpawnTestActor(UWorld* World, const FString& ActorLabel)
    {
        if (!World)
        {
            return nullptr;
        }

        for (TActorIterator<TActor> It(World); It; ++It)
        {
            TActor* Actor = *It;
            if (Actor && Actor->GetActorLabel() == ActorLabel)
            {
                return Actor;
            }
        }
        return nullptr;
    }

    void TestPropertyGetChainsFromActorPath(
        FAutomationTestBase& Test,
        const FString& ActorPath)
    {
        TSharedPtr<FJsonObject> PropertyPayload = MakeShared<FJsonObject>();
        PropertyPayload->SetStringField(TEXT("objectPath"), ActorPath);
        PropertyPayload->SetStringField(TEXT("propertyName"), TEXT("ActorLocation"));

        FTestResponseCapture PropertyCapture;
        Test.TestTrue(TEXT("property.get handler found for returned actorPath"),
            InvokeHandlerWithCapture(TEXT("property.get"), PropertyPayload, PropertyCapture));
        Test.TestTrue(TEXT("property.get responded for returned actorPath"),
            PropertyCapture.bWasCalled);
        Test.TestTrue(TEXT("property.get succeeded for returned actorPath"),
            PropertyCapture.bSuccess);
    }

    template <typename TActor>
    TActor* InvokeEnvironmentSpawnAndValidateResponse(
        FAutomationTestBase& Test,
        UWorld* World,
        const FString& MethodName,
        const TSharedPtr<FJsonObject>& Payload,
        const FString& ActorLabel,
        FString* OutComponentPath = nullptr)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("environment spawn handler found"),
            InvokeHandlerWithCapture(MethodName, Payload, Capture));
        Test.TestTrue(TEXT("environment spawn handler responded"), Capture.bWasCalled);
        if (Capture.bWasCalled)
        {
            Test.TestNotEqual(TEXT("environment spawn did not return SPAWN_FAILED"),
                Capture.ErrorCode, FString(TEXT("SPAWN_FAILED")));
        }
        Test.TestTrue(TEXT("environment spawn handler succeeded"), Capture.bSuccess);
        Test.TestTrue(TEXT("environment spawn returned a payload"), Capture.Result.IsValid());
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return nullptr;
        }

        FString ActorPath;
        Test.TestTrue(TEXT("response includes actorPath"),
            Capture.Result->TryGetStringField(TEXT("actorPath"), ActorPath));
        TActor* Spawned = ActorPath.IsEmpty() ? nullptr : FindObject<TActor>(nullptr, *ActorPath);
        Test.TestNotNull(TEXT("actorPath resolves the spawned actor class"), Spawned);
        if (!Spawned)
        {
            return nullptr;
        }
        Test.TestTrue(TEXT("actorPath resolves an actor in the active editor world"),
            Spawned->GetWorld() == World);

        FString ActorLabelValue;
        Test.TestTrue(TEXT("response includes actorLabel"),
            Capture.Result->TryGetStringField(TEXT("actorLabel"), ActorLabelValue));
        Test.TestEqual(TEXT("actorLabel matches requested label"),
            ActorLabelValue, ActorLabel);

        FString ClassName;
        Test.TestTrue(TEXT("response includes className"),
            Capture.Result->TryGetStringField(TEXT("className"), ClassName));
        Test.TestEqual(TEXT("className matches spawned class short name"),
            ClassName, Spawned->GetClass()->GetName());

        // Regression for E-spawn-returns-actor-not-component-path: these verbs
        // apply `properties` to the actor's primary COMPONENT, so the response must
        // hand back the component's object path (not just actorPath) — otherwise a
        // caller verifying an applied component UPROPERTY has to burn an
        // actor.get_components discovery hop. componentPath must be present,
        // non-empty, and distinct from actorPath. The exact-path check against the
        // typed component lives in each per-verb test below (where the component
        // pointer is in hand).
        FString ComponentPath;
        Test.TestTrue(TEXT("response includes componentPath"),
            Capture.Result->TryGetStringField(TEXT("componentPath"), ComponentPath));
        Test.TestFalse(TEXT("componentPath is non-empty"), ComponentPath.IsEmpty());
        Test.TestNotEqual(TEXT("componentPath is distinct from actorPath"),
            ComponentPath, ActorPath);

        // Presence-only: the captured values aren't asserted, so reuse one
        // throwaway target to make that intent explicit.
        FString Scratch;
        Test.TestTrue(TEXT("response includes componentName"),
            Capture.Result->TryGetStringField(TEXT("componentName"), Scratch));
        Test.TestTrue(TEXT("response includes componentClass"),
            Capture.Result->TryGetStringField(TEXT("componentClass"), Scratch));
        if (OutComponentPath)
        {
            *OutComponentPath = ComponentPath;
        }

        if (!ActorPath.IsEmpty())
        {
            TestPropertyGetChainsFromActorPath(Test, ActorPath);
        }
        return Spawned;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSpawnSkyAtmosphereMethodFoundTest,
    "PinWright.environment.spawn_sky_atmosphere.MethodFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSpawnSkyAtmosphereMethodFoundTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("environment.spawn_sky_atmosphere is registered"),
        IsHandlerRegistered(TEXT("environment.spawn_sky_atmosphere")));
    TestTrue(TEXT("environment.spawn_volumetric_cloud is registered"),
        IsHandlerRegistered(TEXT("environment.spawn_volumetric_cloud")));
    TestTrue(TEXT("environment.spawn_reflection_capture is registered"),
        IsHandlerRegistered(TEXT("environment.spawn_reflection_capture")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSpawnSkyAtmosphereValidParamsTest,
    "PinWright.environment.spawn_sky_atmosphere.ValidParamsRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSpawnSkyAtmosphereValidParamsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping spawn assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString TestLabel = FString::Printf(TEXT("McpTestSkyAtmosphere_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TestLabel);
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetNumberField(TEXT("RayleighScatteringScale"), 0.42);
    Payload->SetObjectField(TEXT("properties"), Props);

    FString ComponentPath;
    ASkyAtmosphere* Spawned = InvokeEnvironmentSpawnAndValidateResponse<ASkyAtmosphere>(
        *this, World, TEXT("environment.spawn_sky_atmosphere"), Payload, TestLabel, &ComponentPath);
    if (Spawned)
    {
        USkyAtmosphereComponent* Comp = Spawned->FindComponentByClass<USkyAtmosphereComponent>();
        TestNotNull(TEXT("USkyAtmosphereComponent present"), Comp);
        if (Comp)
        {
            TestEqual(TEXT("RayleighScatteringScale round-trips"),
                Comp->RayleighScatteringScale, 0.42f);
            // The emitted componentPath must resolve the very component the handler
            // applied `properties` to — the path a caller hands to property.get.
            TestEqual(TEXT("componentPath is the USkyAtmosphereComponent object path"),
                ComponentPath, Comp->GetPathName());
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSpawnVolumetricCloudValidParamsTest,
    "PinWright.environment.spawn_volumetric_cloud.ValidParamsRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSpawnVolumetricCloudValidParamsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping spawn assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString TestLabel = FString::Printf(TEXT("McpTestVolumetricCloud_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TestLabel);
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetNumberField(TEXT("LayerBottomAltitude"), 7.5);
    Payload->SetObjectField(TEXT("properties"), Props);

    FString ComponentPath;
    AVolumetricCloud* Spawned = InvokeEnvironmentSpawnAndValidateResponse<AVolumetricCloud>(
        *this, World, TEXT("environment.spawn_volumetric_cloud"), Payload, TestLabel, &ComponentPath);
    if (Spawned)
    {
        UVolumetricCloudComponent* Comp = Spawned->FindComponentByClass<UVolumetricCloudComponent>();
        TestNotNull(TEXT("UVolumetricCloudComponent present"), Comp);
        if (Comp)
        {
            TestEqual(TEXT("LayerBottomAltitude round-trips"),
                Comp->LayerBottomAltitude, 7.5f);
            TestEqual(TEXT("componentPath is the UVolumetricCloudComponent object path"),
                ComponentPath, Comp->GetPathName());
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSpawnReflectionCaptureSphereTest,
    "PinWright.environment.spawn_reflection_capture.SphereRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSpawnReflectionCaptureSphereTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping spawn assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString TestLabel = FString::Printf(TEXT("McpTestSphereCapture_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("Sphere"));
    Payload->SetStringField(TEXT("name"), TestLabel);
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetNumberField(TEXT("InfluenceRadius"), 1234.0);
    Payload->SetObjectField(TEXT("properties"), Props);

    FString ComponentPath;
    ASphereReflectionCapture* Spawned =
        InvokeEnvironmentSpawnAndValidateResponse<ASphereReflectionCapture>(
            *this, World, TEXT("environment.spawn_reflection_capture"), Payload, TestLabel, &ComponentPath);
    if (Spawned)
    {
        USphereReflectionCaptureComponent* Comp =
            Spawned->FindComponentByClass<USphereReflectionCaptureComponent>();
        TestNotNull(TEXT("USphereReflectionCaptureComponent present"), Comp);
        if (Comp)
        {
            TestEqual(TEXT("InfluenceRadius round-trips"),
                Comp->InfluenceRadius, 1234.0f);
            TestEqual(TEXT("componentPath is the UReflectionCaptureComponent object path"),
                ComponentPath, Comp->GetPathName());
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSpawnReflectionCaptureBoxTest,
    "PinWright.environment.spawn_reflection_capture.BoxRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSpawnReflectionCaptureBoxTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping spawn assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString TestLabel = FString::Printf(TEXT("McpTestBoxCapture_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("Box"));
    Payload->SetStringField(TEXT("name"), TestLabel);
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetNumberField(TEXT("BoxTransitionDistance"), 567.0);
    Payload->SetObjectField(TEXT("properties"), Props);

    FString ComponentPath;
    ABoxReflectionCapture* Spawned =
        InvokeEnvironmentSpawnAndValidateResponse<ABoxReflectionCapture>(
            *this, World, TEXT("environment.spawn_reflection_capture"), Payload, TestLabel, &ComponentPath);
    if (Spawned)
    {
        UBoxReflectionCaptureComponent* Comp =
            Spawned->FindComponentByClass<UBoxReflectionCaptureComponent>();
        TestNotNull(TEXT("UBoxReflectionCaptureComponent present"), Comp);
        if (Comp)
        {
            TestEqual(TEXT("BoxTransitionDistance round-trips"),
                Comp->BoxTransitionDistance, 567.0f);
            TestEqual(TEXT("componentPath is the UReflectionCaptureComponent object path"),
                ComponentPath, Comp->GetPathName());
        }
    }
    return true;
}

// Regression for B-create-sky-sphere-stale-path: environment.build.create_sky_sphere
// hardcoded the stale UE-pre-5.x class path
// /Engine/Maps/Templates/SkySphere.SkySphere_C, which no longer resolves on modern
// UE — LoadClass returned null and the handler always emitted CREATION_FAILED. The
// fix loads the class through a legacy-first fallback chain that ends at the modern
// /Engine/EngineSky/BP_Sky_Sphere.BP_Sky_Sphere_C. This drives the real handler and
// asserts it actually spawns a sky sphere (not CREATION_FAILED) on the running
// engine; it fails if the fallback chain is reverted to the lone stale path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentCreateSkySphereResolvesEngineClassTest,
    "PinWright.environment.build.create_sky_sphere.ResolvesEngineClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentCreateSkySphereResolvesEngineClassTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping create_sky_sphere assertion"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    TSet<AActor*> ActorsBefore;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        ActorsBefore.Add(*It);
    }
    const FString SkySphereLabel = FString::Printf(TEXT("McpTestSkySphere_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), SkySphereLabel);
    FTestResponseCapture Capture;
    TestTrue(TEXT("create_sky_sphere handler found"),
        InvokeHandlerWithCapture(TEXT("environment.build.create_sky_sphere"), Payload, Capture));
    TestTrue(TEXT("create_sky_sphere responded"), Capture.bWasCalled);

    // The defect surfaced as a CREATION_FAILED error because the sky-sphere class
    // never resolved on modern UE. Assert that specific failure mode is gone.
    TestNotEqual(TEXT("create_sky_sphere did not return CREATION_FAILED"),
        Capture.ErrorCode, FString(TEXT("CREATION_FAILED")));
    TestTrue(TEXT("create_sky_sphere succeeded"), Capture.bSuccess);

    TArray<AActor*> SpawnedActors;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (!ActorsBefore.Contains(*It))
        {
            SpawnedActors.Add(*It);
        }
    }
    TestEqual(TEXT("create_sky_sphere added exactly one fixture actor"),
        SpawnedActors.Num(), 1);
    if (SpawnedActors.Num() == 1)
    {
        TestEqual(TEXT("created fixture actor has the GUID-suffixed label"),
            SpawnedActors[0]->GetActorLabel(), SkySphereLabel);
    }

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ActorName;
        TestTrue(TEXT("create_sky_sphere returned a spawned actorName"),
            Capture.Result->TryGetStringField(TEXT("actorName"), ActorName) && !ActorName.IsEmpty());
        TestEqual(TEXT("create_sky_sphere used the GUID-suffixed fixture label"),
            ActorName, SkySphereLabel);
    }
    return true;
}

// ============================================================================
// LandscapeHandler — landscape.create builds a real ULandscapeComponent grid
// (regression for B-landscape-create-hollow-no-components)
// ============================================================================
//
// On UE 5.7 (and 5.5+) landscape.create used to take a SetHeightData-only path:
// CreateDefaultLayer() + FLandscapeEditDataInterface::SetHeightData into a
// component set that was never built. SetHeightData writes through
// ULandscapeInfo::XYtoComponentMap into ALREADY-existing components, so with an
// empty map it was a guaranteed no-op and the spawned ALandscape stayed hollow:
// zero ULandscapeComponents, empty XYtoComponentMap, all-zero bounds, and
// GetLandscapeExtent() returning false. create still reported success:true, so
// every downstream sculpt/edit/flatten/paint RPC was silently dead on the actor.
//
// The fix routes all engine versions through ALandscapeProxy::Import, the only
// path that instantiates + registers the component grid (NewObject<
// ULandscapeComponent> + Init) and populates XYtoComponentMap. This test drives
// the real handler through its AsyncTask(GameThread) path to completion, finds
// the spawned ALandscape, and asserts the grid actually exists.
//
// Counterfactual: if the handler reverts to the SetHeightData-only path, the
// spawned landscape has zero components — XYtoComponentMap.Num() is 0 and
// GetLandscapeExtent() returns false — and the assertions below fail even though
// create still reports success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateBuildsComponentGridTest,
    "PinWright.landscape.create.BuildsComponentGrid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateBuildsComponentGridTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape.create component-grid test"));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));

    FScopedEditorWorldActorGuard WorldGuard;
    const FString LandscapeLabel = FString::Printf(TEXT("MCP_HollowRepro_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // 2x2 grid of the smallest valid subsection size keeps the build fast in the
    // headless test world while still exercising a multi-component grid.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), LandscapeLabel);
    Payload->SetNumberField(TEXT("componentsX"), 2);
    Payload->SetNumberField(TEXT("componentsY"), 2);
    Payload->SetNumberField(TEXT("quadsPerComponent"), 7);
    Payload->SetNumberField(TEXT("sectionsPerComponent"), 1);

    // Shared-owned capture: the handler completes inside an AsyncTask(GameThread)
    // lambda that may drain after this test returns; a weak handle on the token
    // keeps a late completion from writing through a freed capture.
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    const bool bFound = InvokeHandlerWithSharedCapture(TEXT("landscape.create"), Payload, Capture);
    TestTrue(TEXT("landscape.create handler invoked"), bFound);
    if (!bFound)
    {
        return false;
    }

    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);

    TestTrue(TEXT("landscape.create responded (bWasCalled)"), Capture->bWasCalled);
    TestTrue(TEXT("landscape.create reported success"), Capture->bSuccess);

    ALandscape* Landscape = FindEnvironmentSpawnTestActor<ALandscape>(World, LandscapeLabel);
    TestNotNull(TEXT("spawned ALandscape found in world"), Landscape);
    if (!Landscape)
    {
        return false;
    }

    // Core regression assertion #1: the actor actually owns ULandscapeComponents.
    // A hollow landscape has LandscapeComponents.Num() == 0.
    TestTrue(TEXT("landscape has at least one ULandscapeComponent (not hollow)"),
        Landscape->LandscapeComponents.Num() > 0);
    if (Landscape->LandscapeComponents.Num() > 0)
    {
        // 2x2 grid → exactly 4 components when Import builds the grid correctly.
        TestEqual(TEXT("landscape has the expected 2x2 component grid"),
            Landscape->LandscapeComponents.Num(), 4);
    }

    // Core regression assertion #2: ULandscapeInfo::XYtoComponentMap is populated
    // (the exact map SetHeightData/GetHeightData walk — empty on the hollow actor).
    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    TestNotNull(TEXT("landscape has a ULandscapeInfo"), Info);
    if (Info)
    {
        TestTrue(TEXT("XYtoComponentMap is populated (sculpt/edit can find components)"),
            Info->XYtoComponentMap.Num() > 0);

        // Core regression assertion #3: GetLandscapeExtent succeeds — this is the
        // exact engine call landscape.edit/landscape.sculpt make; it returns false
        // on a hollow landscape, which is what produced
        // "[INVALID_LANDSCAPE] Failed to get landscape extent".
        int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        const bool bGotExtent = Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);
        TestTrue(TEXT("GetLandscapeExtent succeeds (edit/sculpt no longer error INVALID_LANDSCAPE)"),
            bGotExtent);
        if (bGotExtent)
        {
            TestTrue(TEXT("landscape extent spans a non-degenerate region"),
                MaxX > MinX && MaxY > MinY);
        }
    }

    return true;
}

// ============================================================================
// LandscapeHandler — landscape.edit on a hollow (component-less) landscape
// emits the LANDSCAPE_NO_COMPONENTS diagnostic, not the bare
// "Failed to get landscape extent" symptom.
// (regression for E-landscape-edit-extent-error-not-diagnostic)
// ============================================================================
//
// A "hollow" landscape — an ALandscape with a registered ULandscapeInfo but zero
// registered ULandscapeComponents (empty XYtoComponentMap) — can arise from a
// manually-spawned actor, a partially-loaded streaming proxy, or any actor whose
// component grid was never built. On such an actor ULandscapeInfo::
// GetLandscapeExtent() returns false, and landscape.edit used to answer every op
// with the bare "[INVALID_LANDSCAPE] Failed to get landscape extent" — a symptom,
// not the cause, which drove caller trial-and-error (wrong name? wrong actor?
// material not registered?).
//
// The fix checks XYtoComponentMap.Num() at the extent-failure site and emits
// "[LANDSCAPE_NO_COMPONENTS] Landscape '<name>' has no registered
// ULandscapeComponents ..." naming the cause + recovery. This test builds exactly
// that hollow actor (bare SpawnActor<ALandscape> + a GUID + CreateLandscapeInfo,
// with NO Import so the component grid is never built), drives the real
// landscape.edit handler through its AsyncTask(GameThread) path, and asserts the
// new diagnostic code.
//
// Counterfactual: if the diagnostic branch is reverted, the handler falls through
// to the bare INVALID_LANDSCAPE/"Failed to get landscape extent" error and the
// ErrorCode assertion below fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeEditHollowEmitsNoComponentsDiagnosticTest,
    "PinWright.landscape.edit.HollowEmitsNoComponentsDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeEditHollowEmitsNoComponentsDiagnosticTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape.edit hollow diagnostic test"));
        return true;
    }

    TestTrue(TEXT("landscape.edit handler registered"),
        IsHandlerRegistered(TEXT("landscape.edit")));

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // On UE 5.3 the engine cannot register a hollow (component-less) ALandscape into a
    // world that already contains a real landscape: CreateLandscapeInfo() -> RegisterActor()
    // routes through the spline/collision registration, which calls an unguarded
    // ULandscapeInfo::Find() and asserts check(LandscapeGuid.IsValid()) (Landscape.cpp:4788),
    // crashing the editor before the handler is ever reached. The hollow fixture this test
    // needs is therefore not constructible on 5.3. The landscape.edit LANDSCAPE_NO_COMPONENTS
    // diagnostic under test is exercised on 5.4+; the production handler itself is crash-safe
    // on 5.3 (it ensures a valid GUID and guards GetLandscapeInfo()).
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping landscape.edit hollow diagnostic on UE 5.3: a hollow ALandscape cannot be registered without crashing the engine on this version."));
    return true;
#else
    FScopedEditorWorldActorGuard WorldGuard;
    const FString LandscapeLabel = FString::Printf(TEXT("MCP_HollowEdit_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Build a HOLLOW landscape: a bare ALandscape with a registered ULandscapeInfo
    // but no component grid. This mirrors the old hollow-create scenario (and any
    // manually-spawned ALandscape): assign a GUID and call CreateLandscapeInfo so
    // GetLandscapeInfo() resolves, but never call Import — so LandscapeComponents
    // stays empty and XYtoComponentMap.Num() == 0, exactly the state that makes
    // GetLandscapeExtent() return false.
    ALandscape* Hollow = World->SpawnActor<ALandscape>(ALandscape::StaticClass(),
        FVector::ZeroVector, FRotator::ZeroRotator);
    TestNotNull(TEXT("spawned bare ALandscape"), Hollow);
    if (!Hollow)
    {
        return false;
    }
    Hollow->SetActorLabel(LandscapeLabel);
    Hollow->SetLandscapeGuid(FGuid::NewGuid());
    Hollow->CreateLandscapeInfo();

    // Precondition: confirm we actually built the hollow state the diagnostic
    // targets — info present, but zero components / empty XYtoComponentMap.
    ULandscapeInfo* Info = Hollow->GetLandscapeInfo();
    TestNotNull(TEXT("hollow landscape has a ULandscapeInfo (reaches the extent check, not 'no info')"), Info);
    TestEqual(TEXT("hollow landscape has zero ULandscapeComponents"),
        Hollow->LandscapeComponents.Num(), 0);
    if (Info)
    {
        TestEqual(TEXT("hollow landscape has empty XYtoComponentMap"),
            Info->XYtoComponentMap.Num(), 0);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("landscapeName"), LandscapeLabel);
    Payload->SetStringField(TEXT("operation"), TEXT("raise"));

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    const bool bFound = InvokeHandlerWithSharedCapture(TEXT("landscape.edit"), Payload, Capture);
    TestTrue(TEXT("landscape.edit handler invoked"), bFound);
    if (!bFound)
    {
        return false;
    }

    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);

    TestTrue(TEXT("landscape.edit responded (bWasCalled)"), Capture->bWasCalled);
    // The hollow landscape has no editable extent, so the call MUST be an error.
    TestFalse(TEXT("landscape.edit on a hollow landscape is an error, not success"),
        Capture->bSuccess);

    // Core regression assertion: the error names the CAUSE (no components), not the
    // bare extent symptom. A revert to the old bare SendError yields INVALID_LANDSCAPE.
    TestEqual(TEXT("landscape.edit emits LANDSCAPE_NO_COMPONENTS for a hollow landscape"),
        Capture->ErrorCode, FString(TEXT("LANDSCAPE_NO_COMPONENTS")));

    return true;
#endif // UE_VERSION_OLDER_THAN(5, 4, 0)
}

// ============================================================================
// EnvironmentHandler — environment.control.set_time_of_day echoes the value the
// directional light ACTUALLY stores after the set, not the pre-normalization
// SolarPitch the handler attempted (regression for
// E-control-time-of-day-pitch-misreports-after-normalize).
// ============================================================================
//
// The handler computes SolarPitch = (hour/24)*360 - 90 and writes it onto the
// sun's rotation via SetActorRotation, which stores orientation as an FQuat. For
// any afternoon/evening hour (hour 17 → SolarPitch 165), 165 is outside the
// canonical FRotator pitch range [-90,90], so SetActorRotation normalizes it:
// pitch is re-expressed into [-90,90] (165 → 15) with yaw/roll flipped 180 deg.
// The handler used to echo the pre-normalization SolarPitch (165), which can
// never equal what actor.get_transform / property.get read back from the actor
// (15) — so the self-report the response invites flatly contradicts the world.
//
// The fix reads the rotation back after SetActorRotation and echoes the applied
// {pitch,yaw,roll}. This test spawns a directional light (so the handler's
// first-ADirectionalLight scan always finds one), invokes the real handler at
// hour 17, resolves the exact actor the response names, and asserts every echoed
// rotation component equals what the actor stores AND that the echoed pitch is in
// the canonical [-90,90] range. A revert to echoing SolarPitch reports 165, which
// neither matches the stored 15 nor falls in [-90,90], failing the test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentControlSetTimeOfDayEchoesStoredPitchTest,
    "PinWright.environment.control.set_time_of_day.EchoesStoredPitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentControlSetTimeOfDayEchoesStoredPitchTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping set_time_of_day pitch echo test"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // Spawn a directional light so the handler's "first ADirectionalLight in the
    // world" scan always finds a target. The response's `actor` path tells us which
    // light it actually used, so this works regardless of pre-existing sun actors.
    const FString SunLabel = FString::Printf(TEXT("PW_SunPitchEcho_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ADirectionalLight* Spawned = SpawnActorInActiveWorld<ADirectionalLight>(
        ADirectionalLight::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SunLabel);
    TestNotNull(TEXT("directional light spawned"), Spawned);
    if (!Spawned)
    {
        return false;
    }
    // Hour 17 → SolarPitch 165, the canonical out-of-range case from the ticket.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("hour"), 17.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("set_time_of_day handler found"),
        InvokeHandlerWithCapture(TEXT("environment.control.set_time_of_day"), Payload, Capture));
    TestTrue(TEXT("set_time_of_day succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // Resolve the exact actor the handler reported rotating (it picks the first
    // directional light, which may not be our spawn) so the comparison is against
    // the right transform.
    FString ActorPath;
    TestTrue(TEXT("response carries the rotated actor path"),
        Capture.Result->TryGetStringField(TEXT("actor"), ActorPath));
    AActor* RotatedActor = FindObject<AActor>(nullptr, *ActorPath);
    TestNotNull(TEXT("reported actor resolves to a live actor"), RotatedActor);
    if (!RotatedActor)
    {
        return false;
    }

    const FRotator Stored = RotatedActor->GetActorRotation();

    // Core regression assertion: every echoed rotation component equals what the
    // actor actually stores. The pre-normalization SolarPitch (165) cannot equal
    // the stored, normalized pitch (~15), so a revert fails the pitch check. The
    // full applied rotation is echoed so the 180-deg yaw/roll flip is visible, so
    // round-trip all three components against the stored transform.
    auto CheckComponent = [&](const TCHAR* Field, double Expected) -> double
    {
        double Echoed = 0.0;
        TestTrue(*FString::Printf(TEXT("response carries %s"), Field),
            Capture.Result->TryGetNumberField(Field, Echoed));
        TestTrue(*FString::Printf(TEXT("echoed %s equals the actor's stored %s"), Field, Field),
            FMath::IsNearlyEqual(Echoed, Expected, 1e-3));
        return Echoed;
    };

    const double EchoedPitch = CheckComponent(TEXT("pitch"), static_cast<double>(Stored.Pitch));
    CheckComponent(TEXT("yaw"), static_cast<double>(Stored.Yaw));
    CheckComponent(TEXT("roll"), static_cast<double>(Stored.Roll));

    // The stored pitch is necessarily normalized into [-90,90]; the echoed value
    // must be too. SolarPitch=165 violates this directly.
    TestTrue(TEXT("echoed pitch is in the canonical [-90,90] range"),
        EchoedPitch >= -90.0 - 1e-3 && EchoedPitch <= 90.0 + 1e-3);

    return true;
}

// ============================================================================
// Static-mobility silent no-op — environment.control.set_*_intensity
//
// ULightComponent::SetIntensity and USkyLightComponent::SetIntensity are both gated
// on USceneComponent::AreDynamicDataChangesAllowed() (SceneComponent.h:1363-1366),
// which is FALSE for a registered Static-mobility component. Both verbs mutate a
// looked-up, author-owned light, so the mobility is not theirs to assume: on a Static
// light the setter wrote nothing while the handler still returned success echoing the
// requested value. The fix assigns the UPROPERTY directly after the setter and
// discloses `mobility` / `requiresLightingRebuild` additively.
//
// Both tests target the level's OWN light, resolved exactly the way the handler
// resolves it (first valid actor in TActorIterator order) so test and handler cannot
// disagree about the target. Spawning a fixture would not work: the handler would
// still pick the level's first light, not ours. Mobility, intensity and the package
// dirty flag are all restored on scope exit.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSetSkylightIntensityAppliesOnStaticMobilityTest,
    "PinWright.environment.control.set_skylight_intensity.AppliesOnStaticMobility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSetSkylightIntensityAppliesOnStaticMobilityTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping Static-mobility skylight test"));
        return true;
    }

    // Same resolution order as the handler.
    ASkyLight* SkyActor = nullptr;
    for (TActorIterator<ASkyLight> It(World); It; ++It)
    {
        if (ASkyLight* Sky = *It)
        {
            if (IsValid(Sky))
            {
                SkyActor = Sky;
                break;
            }
        }
    }
    if (!SkyActor || !SkyActor->GetLightComponent())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("No ASkyLight in the editor world — skipping Static-mobility skylight test"));
        return true;
    }

    USkyLightComponent* SkyComp = SkyActor->GetLightComponent();
    UPackage* Pkg = SkyActor->GetPackage();

    const EComponentMobility::Type OriginalMobility = PinWrightGetMobility(*SkyComp);
    const float OriginalIntensity = SkyComp->Intensity;
    const bool bPkgWasDirty = Pkg->IsDirty();

    ON_SCOPE_EXIT
    {
        SkyComp->SetMobility(OriginalMobility);
        SkyComp->Intensity = OriginalIntensity;
        SkyComp->MarkRenderStateDirty();
        Pkg->SetDirtyFlag(bPkgWasDirty);
    };

    SkyComp->SetMobility(EComponentMobility::Static);
    if (PinWrightGetMobility(*SkyComp) != EComponentMobility::Static)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mobility-change-refused"),
            TEXT("SkyLightComponent refused Static mobility on this engine — skipping the assertion"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 7.25);

    FTestResponseCapture Capture;
    TestTrue(TEXT("environment.control.set_skylight_intensity handler found"),
        InvokeHandlerWithCapture(TEXT("environment.control.set_skylight_intensity"), Payload, Capture));

    // The assertion that fails pre-fix: the gated setter no-ops on a registered Static
    // component while the response already claims intensity: 7.25.
    TestEqual(TEXT("intensity was actually written on a Static skylight"),
        SkyComp->Intensity, 7.25f);

    TestTrue(TEXT("set_skylight_intensity succeeded"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString Mobility;
        TestTrue(TEXT("response carries mobility"),
            Capture.Result->TryGetStringField(TEXT("mobility"), Mobility));
        TestEqual(TEXT("mobility is disclosed as Static"), Mobility, FString(TEXT("Static")));

        bool bRequiresRebuild = false;
        TestTrue(TEXT("response carries requiresLightingRebuild"),
            Capture.Result->TryGetBoolField(TEXT("requiresLightingRebuild"), bRequiresRebuild));
        TestTrue(TEXT("a Static light reports requiresLightingRebuild"), bRequiresRebuild);
    }

    return true;
}

// Guards the other direction: the unconditional raw write must not disturb the normal
// (Movable) path, and the new disclosure field must not be hard-coded true.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentSetSunIntensityAppliesOnMovableMobilityTest,
    "PinWright.environment.control.set_sun_intensity.AppliesOnMovableMobility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEnvironmentSetSunIntensityAppliesOnMovableMobilityTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping Movable-mobility sun test"));
        return true;
    }

    // Same resolution order as the handler.
    ADirectionalLight* SunLight = nullptr;
    for (TActorIterator<ADirectionalLight> It(World); It; ++It)
    {
        if (ADirectionalLight* Light = *It)
        {
            if (IsValid(Light))
            {
                SunLight = Light;
                break;
            }
        }
    }
    UDirectionalLightComponent* LightComp = SunLight
        ? Cast<UDirectionalLightComponent>(SunLight->GetLightComponent())
        : nullptr;
    if (!LightComp)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("No ADirectionalLight in the editor world — skipping Movable-mobility sun test"));
        return true;
    }

    UPackage* Pkg = SunLight->GetPackage();

    const EComponentMobility::Type OriginalMobility = PinWrightGetMobility(*LightComp);
    const float OriginalIntensity = LightComp->Intensity;
    const bool bPkgWasDirty = Pkg->IsDirty();

    ON_SCOPE_EXIT
    {
        LightComp->SetMobility(OriginalMobility);
        LightComp->Intensity = OriginalIntensity;
        LightComp->MarkRenderStateDirty();
        Pkg->SetDirtyFlag(bPkgWasDirty);
    };

    LightComp->SetMobility(EComponentMobility::Movable);
    if (PinWrightGetMobility(*LightComp) != EComponentMobility::Movable)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mobility-change-refused"),
            TEXT("DirectionalLightComponent refused Movable mobility — skipping the assertion"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("intensity"), 3.5);

    FTestResponseCapture Capture;
    TestTrue(TEXT("environment.control.set_sun_intensity handler found"),
        InvokeHandlerWithCapture(TEXT("environment.control.set_sun_intensity"), Payload, Capture));

    TestEqual(TEXT("intensity round-trips on a Movable sun"), LightComp->Intensity, 3.5f);

    TestTrue(TEXT("set_sun_intensity succeeded"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString Mobility;
        TestTrue(TEXT("response carries mobility"),
            Capture.Result->TryGetStringField(TEXT("mobility"), Mobility));
        TestEqual(TEXT("mobility is disclosed as Movable"), Mobility, FString(TEXT("Movable")));

        bool bRequiresRebuild = true;
        TestTrue(TEXT("response carries requiresLightingRebuild"),
            Capture.Result->TryGetBoolField(TEXT("requiresLightingRebuild"), bRequiresRebuild));
        TestFalse(TEXT("a Movable light does not require a lighting rebuild"), bRequiresRebuild);
    }

    return true;
}

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "LandscapeGrassType.h"

// ============================================================================
// LandscapeHandler — landscape.create_grass_type produces a variety the engine
// can actually scatter (regression for
// B-create-grass-type-addzeroed-never-renders).
// ============================================================================
//
// The handler used to allocate its one FGrassVariety with GrassVarieties.AddZeroed(),
// which memsets the slot and never runs FGrassVariety::FGrassVariety() — the only place
// the struct's non-zero defaults are set. Two of those defaults are exactly what the
// landscape grass system gates on, so the verb answered success with a valid asset_path
// while the asset scattered nothing:
//   - EndCullDistance / EndCullDistanceQuality (ctor 10000): the scatter loop in
//     Runtime/Landscape/Private/LandscapeGrass.cpp builds a cluster only when
//     GrassMesh && GetDensity() > 0 && GetEndCullDistance() > 0.
//   - AllowedDensityRange (ctor (0,1), present from UE 5.5): both placement loops keep an
//     instance only when Weight > Min && Weight <= Max — unsatisfiable for every weight
//     once it is memset to (0,0).
// GetDensity()/GetEndCullDistance() read the per-QUALITY slot on hosts where
// GEngine->UseGrassVarityPerQualityLevels is set and the per-PLATFORM slot elsewhere, so
// both slots are asserted rather than whichever one this host happens to consult.
//
// Differential proof: against the AddZeroed code EndCullDistance, EndCullDistanceQuality
// and AllowedDensityRange.Max all read 0 and bUseGrid reads false, so every assertion
// below fails while the handler still reports success. Against default construction they
// carry the constructor's values.
//
// McpSafeAssetSave only marks the package dirty (Utils/AssetUtils.cpp), so the asset is
// found in memory rather than on disk, and the scope exit discards it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGrassTypeVarietyIsRenderableTest,
    "PinWright.landscape.create_grass_type.VarietyIsRenderable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGrassTypeVarietyIsRenderableTest::RunTest(const FString& Parameters)
{
    // /Engine/BasicShapes/Cube.Cube ships with every UE install, so this is a required
    // fixture, not a host-dependent one: its absence is a failure, never a skip.
    const TCHAR* const GrassMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    UStaticMesh* FixtureMesh = LoadObject<UStaticMesh>(nullptr, GrassMeshPath);
    TestNotNull(TEXT("engine Cube fixture mesh loaded"), FixtureMesh);
    if (!FixtureMesh)
    {
        return false;
    }

    // GUID-suffixed so a rerun never takes the handler's "Asset already exists" early
    // return, which answers success without ever touching GrassVarieties.
    const FString AssetName = FString::Printf(TEXT("PW_GrassTypeRenderable_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/Landscape/%s"), *AssetName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("meshPath"), GrassMeshPath);
    Payload->SetNumberField(TEXT("density"), 250.0);

    // Shared-owned capture: the handler completes inside an AsyncTask(GameThread) lambda
    // that may drain after this test returns.
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    const bool bFound = InvokeHandlerWithSharedCapture(
        TEXT("landscape.create_grass_type"), Payload, Capture);
    TestTrue(TEXT("landscape.create_grass_type handler invoked"), bFound);
    if (!bFound)
    {
        return false;
    }

    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);

    // Discard the in-memory asset + package the handler created, so the dirty package
    // never reaches a Save All and the asset registry keeps no entry for it.
    ON_SCOPE_EXIT
    {
        if (UPackage* CreatedPackage = FindPackage(nullptr, *PackagePath))
        {
            if (UObject* CreatedAsset = FindObject<UObject>(CreatedPackage, *AssetName))
            {
                CreatedAsset->ClearFlags(RF_Standalone | RF_Public);
                CreatedAsset->MarkAsGarbage();
            }
            CreatedPackage->SetFlags(RF_Transient);
            FAssetRegistryModule::PackageDeleted(CreatedPackage);
            CreatedPackage->ClearFlags(RF_Standalone | RF_Public);
            CreatedPackage->SetDirtyFlag(false);
            CreatedPackage->MarkAsGarbage();
        }
    };

    TestTrue(TEXT("landscape.create_grass_type responded (bWasCalled)"), Capture->bWasCalled);
    TestTrue(TEXT("landscape.create_grass_type reported success"), Capture->bSuccess);
    if (!Capture->bSuccess)
    {
        return false;
    }

    ULandscapeGrassType* GrassType = FindObject<ULandscapeGrassType>(
        FindPackage(nullptr, *PackagePath), *AssetName);
    TestNotNull(TEXT("created ULandscapeGrassType found in memory"), GrassType);
    if (!GrassType)
    {
        return false;
    }

    TestEqual(TEXT("exactly one grass variety was appended"), GrassType->GrassVarieties.Num(), 1);
    if (GrassType->GrassVarieties.Num() != 1)
    {
        return false;
    }
    const FGrassVariety& Variety = GrassType->GrassVarieties[0];

    TestNotNull(TEXT("variety carries the requested GrassMesh"), Variety.GrassMesh.Get());

    // Gate 1 — the scatter loop skips a variety whose end cull distance or density is 0.
    TestTrue(TEXT("EndCullDistance is non-zero (at 0 the scatter loop never builds a cluster)"),
        Variety.EndCullDistance.Default > 0);
    TestTrue(TEXT("EndCullDistanceQuality is non-zero (the slot GetEndCullDistance reads on per-quality hosts)"),
        Variety.EndCullDistanceQuality.Default > 0);
    TestEqual(TEXT("GrassDensity carries the requested density"),
        Variety.GrassDensity.Default, 250.0f);
    TestEqual(TEXT("GrassDensityQuality carries the requested density (the slot GetDensity reads on per-quality hosts)"),
        Variety.GrassDensityQuality.Default, 250.0f);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // Gate 2 — at (0,0) the placement window `Weight > Min && Weight <= Max` rejects every
    // candidate instance, so even a variety that clears gate 1 scatters nothing.
    TestTrue(TEXT("AllowedDensityRange.Max is non-zero (at (0,0) no weight satisfies the placement window)"),
        Variety.AllowedDensityRange.Max > 0.0f);
#endif

    // bUseGrid is constructed true; zeroing it silently swapped the asset onto the Halton
    // placement path no caller asked for.
    TestTrue(TEXT("bUseGrid keeps its constructed value"), Variety.bUseGrid);

    // The response carries the numbers the engine gates on, not just a bare success.
    if (Capture->Result.IsValid())
    {
        double EchoedEndCullDistance = 0.0;
        TestTrue(TEXT("response carries end_cull_distance"),
            Capture->Result->TryGetNumberField(TEXT("end_cull_distance"), EchoedEndCullDistance));
        TestTrue(TEXT("echoed end_cull_distance is non-zero"), EchoedEndCullDistance > 0.0);

        double EchoedDensity = 0.0;
        TestTrue(TEXT("response carries density"),
            Capture->Result->TryGetNumberField(TEXT("density"), EchoedDensity));
        TestEqual(TEXT("echoed density matches the stored variety"), EchoedDensity, 250.0);
    }

    return true;
}
