// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryTarget.h - The single DynamicMeshActor resolver and spawner for the geometry handlers.
//
// Every geometry verb starts by turning an actor identifier into an ADynamicMeshActor plus its
// UDynamicMeshComponent and UDynamicMesh. The old ~35-line TActorIterator + GetActorLabel scan was
// copy-pasted into ten .cpp files, with four more copies of the spawn helper and six scans
// open-coded inside handler bodies, because two anonymous-namespace statics sharing a name
// become an ODR redefinition once Unity merges the translation units. A per-cluster
// named-namespace header is the sanctioned fix for exactly that (CLAUDE.md:59) and is what the
// rest of the plugin already does for its shared helper clusters.
//
// Error codes and message text are per-call-site DATA here, carried in FResolveOptions, not
// something this header standardizes: the dispatcher tests assert on both, so consolidating the
// scan must not consolidate the vocabulary.
#pragma once

#include "CoreMinimal.h"

// For ERR_NO_WORLD, which is the default FResolveOptions::NoWorldCode below. Every code this
// namespace emits must be declared there or PinWright.core.error_codes.AllEmittedCodesAreRegistered
// fails on the raw literal.
#include "Handlers/ErrorCodes.h"
#include "Utils/ActorUtils.h"

class AActor;
class ADynamicMeshActor;
class FHandlerContext;
class UDynamicMesh;
class UDynamicMeshComponent;
class UWorld;

// The resolved actor and the two handles every geometry op needs off it. Component and Mesh are
// null when the actor carries no DynamicMeshComponent or no mesh; Find leaves them null, while
// ResolveOrSendError never returns true with a null Mesh.
struct FGeometryTarget
{
    ADynamicMeshActor* Actor = nullptr;
    UDynamicMeshComponent* Component = nullptr;
    UDynamicMesh* Mesh = nullptr;
};

namespace GeometryTarget
{
    // The three axes on which the ten replaced resolvers actually differed. The defaults
    // reproduce the eight identical ones verbatim; the two outliers set only their own field.
    struct FResolveOptions
    {
        // Non-null prefixes the ACTOR_NOT_FOUND message as "<Role> actor not found: <identifier>",
        // which is BooleanHandler's target/tool wording.
        const TCHAR* Role = nullptr;

        // BooleanHandler reports a missing UDynamicMeshComponent under its own
        // COMPONENT_NOT_FOUND code; every other resolver folds that case into MESH_NOT_FOUND.
        bool bReportMissingComponent = false;

        // SkeletalMeshAssetIOHandler reports a missing editor world as
        // EDITOR_WORLD_NOT_AVAILABLE / "Editor world not available" rather than NO_WORLD.
        const TCHAR* NoWorldCode = ErrorCodes::ERR_NO_WORLD;
        const TCHAR* NoWorldMessage = TEXT("No world available");
    };

    // The one axis on which the four replaced spawn helpers differed. PrimitiveHandler's and
    // MeshIOHandler's copies were byte-equivalent to each other, and MeshAssetIOHandler's and
    // SkeletalMeshAssetIOHandler's to each other; the two pairs are NOT equivalent, so the
    // difference is parameterized rather than merged away.
    struct FSpawnOptions
    {
        // The create_* / import verbs fall back to the EditorActorSubsystem's world when
        // GEditor's editor world context has none; the asset-import verbs do not.
        bool bFallBackToActorSubsystemWorld = true;
    };

    // Resolve a DynamicMeshActor identifier by exact object path, exact internal object name,
    // then exact display label. Geometry never uses label-substring matching. A path/name
    // match is deterministic; a label match with more than one DynamicMeshActor is Ambiguous
    // and carries all candidates. An exact path/name for a non-DynamicMesh actor remains a
    // resolved non-mesh identity so it cannot be reinterpreted as a DynamicMesh label.
    McpActorUtils::FActorResolution ResolveMeshActor(UWorld* World, const FString& Identifier);

    // Resolve any actor using exact path/name/label precedence and the ambiguity contract.
    // Label substrings are intentionally not accepted by geometry or spline lookups.
    McpActorUtils::FActorResolution ResolveAnyActor(UWorld* World, const FString& Identifier);

    // Compatibility probe for call sites that only need a nullable mesh actor. It returns null
    // for a miss, an ambiguous label, or an exact identity that is not a DynamicMeshActor.
    ADynamicMeshActor* FindMeshActor(UWorld* World, const FString& Identifier);

    // Nullable any-actor probe. It returns null for a miss or an ambiguous label. Used for the
    // spline actors the advanced ops read and for the StaticMesh actor create_from_static_mesh
    // copies out of; handlers needing a typed ambiguity response use ResolveAnyActor directly.
    AActor* FindAnyActor(UWorld* World, const FString& Identifier);

    // Nullable probe: fills Out with whatever resolved and sends nothing. Returns true only
    // when Out.Mesh is non-null. OutResolution is optional for call sites that need to preserve
    // the distinction between an ambiguous label and a missing actor before composing their
    // own message.
    bool Find(
        UWorld* World,
        const FString& Identifier,
        FGeometryTarget& Out,
        McpActorUtils::FActorResolution* OutResolution = nullptr);

    // Resolve-or-error. Returns false after sending exactly one error: INVALID_ARGUMENT on an
    // empty identifier, Options.NoWorldCode when there is no editor world,
    // AMBIGUOUS_ACTOR_NAME with candidate paths when a display label is not unique,
    // ACTOR_NOT_FOUND when no suitable actor carries the identifier, then
    // COMPONENT_NOT_FOUND / MESH_NOT_FOUND per Options.
    bool ResolveOrSendError(
        const FHandlerContext& Ctx,
        const FString& ActorName,
        FGeometryTarget& Out,
        const FResolveOptions& Options = FResolveOptions());

    // Spawn a DynamicMeshActor carrying DynMesh, placed by Transform and labeled Label.
    //
    // Spawn-transform convention (load-bearing): the requested location/rotation/scale lives
    // ONLY on the spawned actor. The mesh is built in LOCAL space - the Append* primitive calls
    // receive FTransform::Identity - so an offset primitive's vertices stay centered on its own
    // origin and the actor carries the offset. Baking the same transform into BOTH the vertices
    // and the actor double-applies it: the primitive lands at 2x its requested location and
    // SetActorScale3D squares the scale, which silently breaks every downstream op that resolves
    // geometry through the actor transform (most visibly geometry.boolean_* - an offset cutter
    // whose mesh is also pre-offset no longer overlaps the target in world space, so the boolean
    // correctly produces no cut and reports a silent success-with-no-effect).
    //
    // Always calls GeometryUtils::MarkGeometryActorSpawned on the new actor: SpawnActor only
    // dirties the level under an open transaction and this namespace opens none, so without it
    // the actor is dropped on editor close.
    //
    // Returns null on failure, having sent the error and marked DynMesh as garbage.
    //
    // Returns the CONCRETE ADynamicMeshActor*, not AActor*: this only ever spawns
    // ADynamicMeshActor, and the AActor* return forced a defensive Cast inside the body whose
    // false branch returned a spawned-but-mesh-less actor with no error - a corrupt actor
    // surfacing at some later verb instead of here. The type now carries that guarantee, and
    // callers that only need AActor* still get it by implicit upcast.
    ADynamicMeshActor* Spawn(
        const FHandlerContext& Ctx,
        UDynamicMesh* DynMesh,
        const FTransform& Transform,
        const FString& Label,
        const FSpawnOptions& Options = FSpawnOptions());
}
