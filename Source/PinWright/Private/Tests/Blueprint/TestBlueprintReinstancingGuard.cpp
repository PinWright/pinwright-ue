// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the Blueprint live-instance precondition
// (Handlers/Blueprint/BlueprintReinstancingGuard.h) and its safe-point companion.
//
// THE DEFECT CLASS. A Blueprint compile flushes the reinstancing queue, which destroys
// and re-creates every live instance of the class in every loaded world. Twice on board
// ticket E-compile-reinstances-live-instances-no-guard that killed a shared editor: once
// through a game-side null-deref during PIE, once - with NO PIE running - through a tick
// task cooked for the current frame that still held a raw FTickFunction* into the actor
// the compile had just trashed. The second kill reinstanced a placed actor in a DIFFERENT
// agent's open map: the compiling stream could not have known, and the map's owner could
// not have known either.
//
// The fix has two independent halves and these tests cover both, because either half
// alone leaves a shipped kill reachable:
//   POSITION - blueprint.compile / blueprint.set_default are entries in the tick-unsafe
//     method table, so the reinstancing pass runs from the core ticker instead of from
//     inside the engine frame.
//   CONSENT - the survey is reported on the response, and the compile is refused with
//     LIVE_INSTANCES_WOULD_BE_REINSTANCED unless the caller passed allowReinstancing.
//
// UNABLE TO FAIL IF: the fixture Blueprint or its instance cannot be built. Both are
// asserted rather than skipped past, so a broken fixture is a red, not a green.

#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Named namespace (not anonymous) because the plugin's tests share one module under
// Unity builds: identically-named file-local helpers in two .cpp files that land in the
// same Unity chunk are an ODR clash. Same convention as Tests/Infra/DispatcherTestHelpers.h.
namespace BlueprintReinstancingGuardTest
{
    FString MakeFixturePath()
    {
        return FString::Printf(TEXT("/Game/__PW_ReinstancingGuardTests/BP_Fixture_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A minimal in-memory AActor Blueprint, registered with the asset registry so the
    // handler's LoadBlueprintAsset resolves the /Game path. Never saved; discarded below.
    UBlueprint* CreateFixtureBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package || AssetName.IsEmpty())
        {
            return nullptr;
        }

        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            FName(*AssetName),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Blueprint)
        {
            return nullptr;
        }

        // Without a compile the generated class is not spawnable, and this compile runs
        // while the survey is still empty - so it exercises nothing this file asserts on.
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        FAssetRegistryModule::AssetCreated(Blueprint);
        return Blueprint;
    }

    // Teardown for a never-saved fixture package. Mirrors DiscardFixtureBlueprint in
    // Tests/Actor/TestSpawnMaterialSurvivesConstructionScript.cpp: a plain DeleteAsset
    // would try to resolve an on-disk source that never existed.
    void DiscardFixtureBlueprint(const FString& PackagePath)
    {
        UPackage* Package = FindPackage(nullptr, *PackagePath);
        if (!Package)
        {
            return;
        }
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UObject* Asset = FindObject<UObject>(Package, *AssetName);

        Package->SetFlags(RF_Transient);
        FAssetRegistryModule::PackageDeleted(Package);
        Package->ClearFlags(RF_Standalone | RF_Public);
        Package->SetDirtyFlag(false);
        if (Asset)
        {
            Asset->ClearFlags(RF_Standalone | RF_Public);
            Asset->RemoveFromRoot();
            Asset->MarkAsGarbage();
        }
        Package->MarkAsGarbage();
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    AActor* SpawnFixtureInstance(UBlueprint* Blueprint)
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World || !Blueprint || !Blueprint->GeneratedClass)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        return World->SpawnActor<AActor>(Blueprint->GeneratedClass, FVector::ZeroVector,
                                         FRotator::ZeroRotator, SpawnParams);
    }

    // The `reinstanced.count` a response carries, or -1 when the block is absent.
    int32 ReadReinstancedCount(const TSharedPtr<FJsonObject>& Result)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("reinstanced"), Block) || !Block)
        {
            return -1;
        }
        int32 Count = 0;
        return (*Block)->TryGetNumberField(TEXT("count"), Count) ? Count : -1;
    }
}

// ---------------------------------------------------------------------------
// 1. The measurement
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReinstancingGuardSurveyTest,
    "PinWright.blueprint.reinstancing_guard.SurveyCountsLiveInstancesPerWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintReinstancingGuardSurveyTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintReinstancingGuardTest;

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world available"), EditorWorld))
    {
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString PackagePath = MakeFixturePath();
    ON_SCOPE_EXIT { DiscardFixtureBlueprint(PackagePath); };

    UBlueprint* Blueprint = CreateFixtureBlueprint(PackagePath);
    if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint))
    {
        return false;
    }

    // Before any instance exists the survey must be EMPTY. The CDO and the class
    // archetypes are rebuilt by every compile, so counting them would make the survey
    // non-empty for every Blueprint in the project and the whole gate meaningless.
    const BlueprintReinstancingGuard::FLiveInstanceSurvey Before =
        BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);
    TestTrue(TEXT("survey is empty with no live instances (CDO/archetypes excluded)"),
             Before.IsEmpty());

    AActor* Instance = SpawnFixtureInstance(Blueprint);
    if (!TestNotNull(TEXT("fixture instance spawned into the editor world"), Instance))
    {
        return false;
    }

    const BlueprintReinstancingGuard::FLiveInstanceSurvey After =
        BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);
    TestEqual(TEXT("survey counts the one live instance"), After.InstanceCount, 1);
    TestEqual(TEXT("the instance is counted as an actor"), After.ActorCount, 1);
    if (TestEqual(TEXT("one owning world reported"), After.Worlds.Num(), 1))
    {
        // The world PACKAGE name, because "whose map is this" is the question the
        // cross-stream half of the hazard asks.
        TestEqual(TEXT("owning world is named by package"), After.Worlds[0].WorldName,
                  EditorWorld->GetPackage()->GetName());
        TestEqual(TEXT("owning world type reported"), After.Worlds[0].WorldType, FString(TEXT("Editor")));
        TestEqual(TEXT("per-world count reported"), After.Worlds[0].InstanceCount, 1);
    }

    // Destroying the instance must take the survey back to empty: proves the count came
    // from the INSTANCE and not from the class, which is what makes a non-empty survey a
    // trustworthy refusal reason.
    Instance->Destroy();
    const BlueprintReinstancingGuard::FLiveInstanceSurvey AfterDestroy =
        BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);
    TestTrue(TEXT("survey empties again once the instance is destroyed"), AfterDestroy.IsEmpty());

    return true;
}

// ---------------------------------------------------------------------------
// 2. The report shape
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReinstancingGuardJsonTest,
    "PinWright.blueprint.reinstancing_guard.EmptySurveyLeavesResponseShapeUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintReinstancingGuardJsonTest::RunTest(const FString& Parameters)
{
    // An empty survey must add NOTHING. Every compile verb in the plugin routes its
    // response through AddCompileDiagnosticsToJson, so an unconditional field would change
    // the wire shape of dozens of verbs that reinstance nothing.
    TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
    BlueprintReinstancingGuard::AddSurveyToJson(
        BlueprintReinstancingGuard::FLiveInstanceSurvey(), Empty);
    TestFalse(TEXT("empty survey emits no `reinstanced` field"),
              Empty->HasField(TEXT("reinstanced")));

    BlueprintReinstancingGuard::FLiveInstanceSurvey Survey;
    Survey.InstanceCount = 3;
    Survey.ActorCount = 2;
    Survey.bPieActive = true;
    Survey.Worlds.Add({TEXT("/Game/FPS/Test/T_Weapons"), TEXT("Editor"), 2, 2});
    Survey.Worlds.Add({TEXT("/Game/Maps/M_Arena"), TEXT("PIE"), 1, 0});

    TSharedPtr<FJsonObject> Populated = MakeShared<FJsonObject>();
    BlueprintReinstancingGuard::AddSurveyToJson(Survey, Populated);

    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (!TestTrue(TEXT("populated survey emits `reinstanced`"),
                  Populated->TryGetObjectField(TEXT("reinstanced"), Block) && Block))
    {
        return false;
    }
    int32 Count = 0;
    (*Block)->TryGetNumberField(TEXT("count"), Count);
    TestEqual(TEXT("count reported"), Count, 3);

    const TArray<TSharedPtr<FJsonValue>>* Worlds = nullptr;
    if (TestTrue(TEXT("worlds reported"), (*Block)->TryGetArrayField(TEXT("worlds"), Worlds) && Worlds))
    {
        TestEqual(TEXT("one entry per owning world"), Worlds->Num(), 2);
    }

    // The refusal message must name the worlds, because "refuse with the count and the
    // owning world named" is what makes the refusal actionable across agent streams.
    const FString Described = BlueprintReinstancingGuard::DescribeSurvey(Survey);
    TestTrue(TEXT("description names the first world"), Described.Contains(TEXT("/Game/FPS/Test/T_Weapons")));
    TestTrue(TEXT("description names the second world"), Described.Contains(TEXT("/Game/Maps/M_Arena")));

    return true;
}

// ---------------------------------------------------------------------------
// 3. The verb contract: refuse by default, proceed on the explicit opt-in
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileRefusesLiveInstancesTest,
    "PinWright.blueprint.compile.RefusesLiveInstancesUnlessOptedIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintCompileRefusesLiveInstancesTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintReinstancingGuardTest;

    if (!TestNotNull(TEXT("editor world available"),
                     GEditor ? GEditor->GetEditorWorldContext().World() : nullptr))
    {
        return false;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString PackagePath = MakeFixturePath();
    ON_SCOPE_EXIT { DiscardFixtureBlueprint(PackagePath); };

    UBlueprint* Blueprint = CreateFixtureBlueprint(PackagePath);
    if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture instance spawned"), SpawnFixtureInstance(Blueprint)))
    {
        return false;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Default: refused, and the refusal carries the same `reinstanced` block the success
    // path reports so the caller can decide without a second round trip.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("path"), PackagePath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("blueprint.compile"),
                                    TEXT("reinstancing-guard-refuse"), Params,
                                    bSuccess, Result, ErrorCode);
    if (!Sink->bWasCalled)
    {
        // blueprint.compile is a tick-unsafe table entry; a bare FRpcDispatcher has no
        // subsystem ticker to drain PendingQueue. An automation body is a safe stack, so
        // this normally does not fire - draining keeps the test honest if it ever does.
        Dispatcher.ProcessPendingRequests();
        bSuccess = Sink->bSuccess;
        ErrorCode = Sink->ErrorCode;
        Result = Sink->Result;
    }

    TestFalse(TEXT("compile with a live instance is refused"), bSuccess);
    TestEqual(TEXT("refusal names the precondition"), ErrorCode,
              FString(TEXT("LIVE_INSTANCES_WOULD_BE_REINSTANCED")));
    TestTrue(TEXT("refusal reports what would have been reinstanced"),
             ReadReinstancedCount(Result) >= 1);

    // Opt-in: the compile proceeds and DISCLOSES the rebuild on the success response.
    Params->SetBoolField(TEXT("allowReinstancing"), true);
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("blueprint.compile"),
                                    TEXT("reinstancing-guard-optin"), Params,
                                    bSuccess, Result, ErrorCode);
    if (!Sink->bWasCalled)
    {
        Dispatcher.ProcessPendingRequests();
        bSuccess = Sink->bSuccess;
        Result = Sink->Result;
    }

    TestTrue(TEXT("compile proceeds once allowReinstancing is set"), bSuccess);
    TestTrue(TEXT("success response discloses the reinstanced instances"),
             ReadReinstancedCount(Result) >= 1);

    return true;
}

// ---------------------------------------------------------------------------
// 4. The two halves are actually wired: opt-in declared, verbs tick-gated
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintCompileVerbsCarryReinstancingContractTest,
    "PinWright.blueprint.reinstancing_guard.CompileVerbsAreGatedAndDeclareOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintCompileVerbsCarryReinstancingContractTest::RunTest(const FString& Parameters)
{
    const TCHAR* const Verbs[] = { TEXT("blueprint.compile"), TEXT("blueprint.set_default") };

    const TArray<FString>& TickUnsafe = PinWrightSafePoint::GetTickUnsafeMethods();
    for (const TCHAR* Verb : Verbs)
    {
        // CONSENT half: a caller cannot opt in through a param the dispatcher rejects as
        // unknown, so the declaration - not just the handler-side read - is the contract.
        TestTrue(FString::Printf(TEXT("%s accepts allowReinstancing"), Verb),
                 ParamSpecTestHelpers::IsParamAccepted(
                     Verb, BlueprintReinstancingGuard::AllowReinstancingParamName()));

        // POSITION half: reinstancing from inside the engine frame trashes objects whose
        // tick tasks are already cooked (EngineBaseTypes.h:524, "Pure virtual not
        // implemented"). The table entry is what moves the compile off that stack.
        TestTrue(FString::Printf(TEXT("%s is tick-unsafe"), Verb),
                 TickUnsafe.Contains(FString(Verb)));
    }

    return true;
}
