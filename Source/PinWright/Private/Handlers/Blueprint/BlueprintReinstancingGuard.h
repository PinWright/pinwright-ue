// Copyright (c) 2026 Alexander Penkin. MIT License.

// The live-instance precondition every Blueprint compile verb answers to.
//
// WHY THIS EXISTS. FKismetEditorUtilities::CompileBlueprint flushes the Blueprint
// compilation manager's reinstancing queue
// (FBlueprintCompilationManagerImpl::FlushReinstancingQueueImpl,
// Editor/Kismet/Private/BlueprintCompilationManager.cpp:392 -> :2086), which walks
// every live instance of the class through ReplaceInstancesOfClass_Inner and, for
// actors, ends at
//
//     World->EditorDestroyActor(OldActor, /*bShouldModifyLevel =*/true)
//     (Editor/UnrealEd/Private/Kismet2/KismetReinstanceUtilities.cpp:3011)
//
// The old actor is destroyed and a replacement is constructed. That is designed UE
// behaviour - a human pressing Compile in the Blueprint editor gets the identical
// pass - and it has two consequences an RPC caller cannot see and did not ask for:
//
//   1. It DESTROYS OBJECTS THE CURRENT FRAME IS STILL TICKING. FTickTaskLevel cooks
//      one TGraphTask<FTickFunctionTask> per enabled tick function at StartFrame, and
//      the task holds a RAW FTickFunction* (Runtime/Engine/Private/TickTaskManager.cpp:284)
//      which it dereferences in DoTask (:334 -> FTickFunction::ExecuteTick, a
//      PURE_VIRTUAL at Runtime/Engine/Classes/Engine/EngineBaseTypes.h:524).
//      Unregistering a tick function does not cancel an already-cooked task, so
//      trashing the owning actor mid-frame leaves the task pointing at a destructed
//      object: `LowLevelFatalError [EngineBaseTypes.h:524] Pure virtual not
//      implemented ()`, reached through FTickTaskSequencer::ReleaseTickGroup ->
//      UWorld::Tick -> UEditorEngine::Tick. That is the second shipped editor kill on
//      board ticket E-compile-reinstances-live-instances-no-guard, and it needed no
//      PIE session and no game code on the stack.
//
//      The POSITION half of that is fixed by the safe-point gate: blueprint.compile
//      and blueprint.set_default are entries in Dispatch/SafePoint.cpp, so the compile
//      runs from UPinWrightSubsystem::Tick on the core ticker - after the world tick
//      has ended and no tick task is cooked - instead of from the named-thread pump
//      the transport marshals every request onto. This header is the OTHER half.
//
//   2. It REBUILDS PLACED ACTORS IN WORLDS THE CALLER DOES NOT OWN, and dirties their
//      level (EditorDestroyActor is called with bShouldModifyLevel=true). In a shared
//      editor with several agent streams attached, one stream compiling its own
//      Blueprint silently destroys and re-creates instances sitting in another
//      stream's open map. Neither party can see it coming from its own side, so it is
//      not a discipline problem a working agreement can fix - it has to be a
//      precondition on the verb.
//
// THE CONTRACT. Survey first, then either report or refuse:
//   - SurveyLiveInstances() counts the instances a compile would reinstance, grouped
//     by the world that owns them.
//   - Verbs on the shared compile path report a non-empty survey on their SUCCESS
//     response (BlueprintHandlerUtils::AddCompileDiagnosticsToJson emits `reinstanced`),
//     so a caller can tell a no-op compile from one that just rebuilt another team's
//     level actors.
//   - RefuseIfLiveInstancesWouldBeReinstanced() is the opt-in gate: a compile verb that
//     adopts it refuses with LIVE_INSTANCES_WOULD_BE_REINSTANCED, naming the count and
//     every owning world, unless the caller passes allowReinstancing=true. Reinstancing
//     is legitimate - the flag exists so it is CHOSEN rather than stumbled into.
//
// WHAT WAS REJECTED, so it is not re-proposed:
//   - "Unregister the live instances' tick functions before compiling." It does not
//     work. FTickTaskLevel::RemoveTickFunction (TickTaskManager.cpp:1807) removes the
//     function from the manager's lists; the graph task cooked for THIS frame still
//     holds the same raw pointer and still calls ExecuteTick on it.
//   - "Compile with EBlueprintCompileOptions::SkipReinstancing and flush later." The
//     engine forbids it: CompileSynchronouslyImpl asserts
//     `ensure(!bSkipReinstancing); // This is an internal option, should not go through
//     CompileSynchronouslyImpl` (BlueprintCompilationManager.cpp:365).

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Handlers/ParamSpec.h"

class FHandlerContext;
class UBlueprint;
class UWorld;

namespace BlueprintReinstancingGuard
{

// One world's share of the survey. WorldName is the world PACKAGE name (the map path
// an operator recognises, e.g. /Game/FPS/Test/T_Weapons), because "whose map is this"
// is the question the cross-stream half of the hazard asks.
struct FWorldInstanceCount
{
    FString WorldName;
    FString WorldType;   // "Editor" | "PIE" | "Game"
    int32 InstanceCount = 0;
    int32 ActorCount = 0;
};

struct FLiveInstanceSurvey
{
    int32 InstanceCount = 0;
    int32 ActorCount = 0;
    bool bPieActive = false;
    TArray<FWorldInstanceCount> Worlds;

    bool IsEmpty() const { return InstanceCount == 0; }
};

// Every live instance a compile of Blueprint would reinstance, grouped by owning world.
//
// SCOPE, and each exclusion is load-bearing:
//   - Derived classes are INCLUDED (bIncludeDerivedClasses=true) because reinstancing
//     rebuilds the whole class hierarchy under the recompiled class, not just exact-type
//     instances.
//   - CDOs and archetypes are EXCLUDED (RF_ClassDefaultObject | RF_ArchetypeObject):
//     they are rebuilt by every compile by definition, so counting them would make the
//     survey non-empty for every Blueprint in the project and the gate meaningless.
//   - Only EWorldType::Editor / PIE / Game worlds are counted. EditorPreview is
//     deliberately out: opening a Blueprint editor puts an instance in its preview
//     scene, and counting that would refuse a compile purely because the asset is open.
//     Inactive worlds are out for the same reason they are not the hazard - nothing is
//     ticking them.
PINWRIGHT_API FLiveInstanceSurvey SurveyLiveInstances(const UBlueprint* Blueprint);

// Writes `reinstanced: { count, actorCount, pieActive, worlds: [...] }` into Out.
// No-op for an empty survey, so a compile that reinstances nothing keeps its existing
// response shape byte for byte.
PINWRIGHT_API void AddSurveyToJson(const FLiveInstanceSurvey& Survey, const TSharedPtr<FJsonObject>& Out);

// "3 live instance(s) (3 placed actor(s)) in 2 loaded world(s): /Game/A/B (Editor) x2, ..."
// Empty string for an empty survey.
PINWRIGHT_API FString DescribeSurvey(const FLiveInstanceSurvey& Survey);

// The wire name of the opt-in. Spelled once so the param spec, the handler read and
// the refusal message can never drift apart.
inline const TCHAR* AllowReinstancingParamName() { return TEXT("allowReinstancing"); }

// The FParamSpec every adopting verb appends to its RPC_PARAMS. Declared here rather
// than copied per verb for the reason the safe-point table exists: a hand-copied gate
// is what let four level verbs ship ungated.
inline FParamSpec AllowReinstancingParam()
{
    return FParamSpec{
        FString(AllowReinstancingParamName()),
        TEXT("boolean"),
        TEXT("Proceed even though loaded worlds hold live instances of this class. A compile flushes ")
        TEXT("the Blueprint reinstancing queue, which DESTROYS and re-creates every live instance and ")
        TEXT("dirties the level that owns it - in a shared editor those can be placed actors in another ")
        TEXT("agent's open map. Defaults to false: the call is refused with ")
        TEXT("LIVE_INSTANCES_WOULD_BE_REINSTANCED naming the instance count and each owning world, and ")
        TEXT("the response carries the same `reinstanced` block the success path reports. Set true to ")
        TEXT("accept the rebuild (or stop PIE / close the map first)."),
        false,
        TEXT("false")};
}

// The precondition itself. Returns TRUE when it REFUSED and already answered through
// Ctx - the caller must then `return true;` without compiling. Returns false when the
// compile may proceed (no live instances, or the caller passed allowReinstancing=true).
//
// Verb is the registered method name, used only in the refusal message.
PINWRIGHT_API bool RefuseIfLiveInstancesWouldBeReinstanced(
    FHandlerContext& Ctx,
    const UBlueprint* Blueprint,
    const TCHAR* Verb);

// Use this overload when the success response must report the same pre-compile
// survey that the consent decision used.
PINWRIGHT_API bool RefuseIfLiveInstancesWouldBeReinstanced(
    FHandlerContext& Ctx,
    const UBlueprint* Blueprint,
    const FLiveInstanceSurvey& Survey,
    const TCHAR* Verb);

}  // namespace BlueprintReinstancingGuard
