// Copyright (c) 2026 Alexander Penkin. MIT License.

// InstancedMeshHandler.cpp - the per-instance verbs:
//   actor.get_instances            - read decomposed per-instance transforms. Non-mutating.
//   actor.set_instance_transforms  - write them, in batch, echoing what each instance was.
//   actor.add_instances            - append instances, optionally replacing the whole scatter.
//   actor.remove_instances         - remove listed instances or all of them, echoing each one.
//
// They exist because an ISM/HISM instance is reachable by no other verb in this plugin.
// `property.get` on PerInstanceSMData hands back a raw FMatrix in COMPONENT-LOCAL space that the
// caller must decompose and compose with the component transform before it is comparable to a
// spatial.raycast hit; `property.set` on the same field STORES, reads back the new matrix, reports
// success - and moves nothing, because every one of ISM's instance-edit side effects lives in
// PostEditChangeChainProperty, which this plugin can never emit. See Handlers/Actor/
// InstancedMeshUtils.h for that mechanism and for the write path both verbs use instead.
//
// foliage.* does not cover this either: all six foliage verbs resolve through an
// AInstancedFoliageActor and take no actor or component parameter, so a plain BP actor carrying a
// HISM, a PCG-managed ISM or a construction-script scatter is invisible to them - and
// foliage.add_instances pointed at the holder's mesh silently builds a SECOND, parallel scatter.
//
// The write verb is BATCH by construction: a scatter fix is N instances, and one RPC per instance
// is what these verbs replace. It carries two structural guarantees:
//   - all-or-nothing pre-flight. One bad index, one duplicated index, or an expectedCount that
//     disagrees with the component refuses the WHOLE call before the first write. Instances are
//     addressed POSITIONALLY and a re-scatter renumbers them, so a partially applied batch keyed
//     on stale indices is the failure mode worth designing against.
//   - every instance it writes is echoed in movedInstances[] with its pre-write transform AND the
//     space that transform is in, so a record lifted out of its response still says what its
//     numbers mean. Replaying one adopts the rows' space; replaying it under a conflicting one is
//     REFUSED. That is the undo, alongside the batch's own FScopedTransaction.
//
// Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Actor/InstancedMeshUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
    // Per-call ceiling on the rows a caller sends (or a read returns), bounding worst-case
    // game-thread time. A caller with more instances pages with limit/offset (read) or splits the
    // batch (write). Two paths are deliberately NOT capped: actor.remove_instances {all:true} and
    // actor.add_instances {replace:true} snapshot the WHOLE component into removedInstances[],
    // because that record is the recovery path and a truncated one is the data loss it prevents.
    constexpr int32 InstanceRpcMaxBatch = 5000;
    constexpr int32 InstanceRpcDefaultLimit = 512;

    // Ceiling on echoed readback-mismatch rows. The count is always exact; only the rows are
    // bounded, the same split spatial.ground_actors uses for its detail rows.
    constexpr int32 InstanceRpcMaxMismatchRows = 64;

    // Readback tolerance for the write verb's verification, in cm / quaternion units.
    // FInstancedStaticMeshInstanceData::Transform is an FMatrix (double under LWC) and the write
    // round-trips world -> local -> matrix -> local -> world, so the residue is float-noise scale;
    // this is well inside that and well outside anything a caller would call "it moved".
    constexpr double InstanceRpcReadbackToleranceCm = 0.05;

    // Editor world by default; PIE world when a play session is active. Same rule as
    // SpawnHandler's ResolveSpawnWorld and GroundPlacementHandler's GroundRpcResolveWorld, so
    // every verb that can touch a scatter targets the same world.
    UWorld* InstanceRpcResolveWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        if (GEditor->PlayWorld)
        {
            return GEditor->PlayWorld;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    // The three-way JSON schema both verbs speak, and the one place `space` is interpreted.
    // "world" is the default because it is the only space comparable to a spatial.raycast hit,
    // an actor transform, or a level coordinate a human read off the viewport; "local" is what
    // the component actually stores and is what a caller reproducing a construction-script
    // scatter wants.
    bool InstanceRpcParseSpace(const FHandlerContext& Ctx, bool& bOutWorldSpace)
    {
        const FString Space = Ctx.GetString(TEXT("space"), TEXT("world")).ToLower();
        if (Space == TEXT("world"))
        {
            bOutWorldSpace = true;
            return true;
        }
        if (Space == TEXT("local"))
        {
            bOutWorldSpace = false;
            return true;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown space '%s'. Valid: world (default), local."), *Space));
        return false;
    }

    void InstanceRpcAddAxisEcho(const TSharedPtr<FJsonObject>& Data, bool bWorldSpace)
    {
        Data->SetStringField(TEXT("space"), InstancedMeshUtils::SpaceToken(bWorldSpace));
        Data->SetStringField(TEXT("units"), TEXT("cm"));
        Data->SetStringField(TEXT("axis"), TEXT("+X fwd, +Y right, +Z up, left-handed"));
    }

    // Reconcile the space the ROWS say they are in with the space the CALL says to interpret them
    // in, before a single instance is written.
    //
    // Every movedInstances[] row this plugin produces carries its own `space` stamp
    // (InstancedMeshUtils::MakeMovedInstanceRow), because the record is routinely lifted out of
    // the response it arrived in. Without the reconciliation below, a batch written with
    // space:"local" produced local-space rows whose naive replay - the documented "hand the record
    // straight back" undo - fell through to this verb's world default, wrote component-local
    // numbers as world coordinates, verified them by re-reading in the SAME wrong space, and
    // answered updated: N with the scatter relocated to garbage.
    //
    // Three outcomes, all decided before any write:
    //   - the call states no `space` and the rows agree on one -> ADOPT it, so the naive replay of
    //     a local-space record simply works and the response says where the space came from;
    //   - the call states a `space` that disagrees with the rows -> REFUSE. A replay that cannot
    //     restore what the record holds must fail loudly, not succeed against the wrong numbers;
    //   - rows disagree with EACH OTHER -> REFUSE. One UpdateInstanceTransform call interprets one
    //     space, so a mixed batch has no single correct reading.
    // Rows with no stamp are silent: get_instances rows and hand-written batches keep working.
    bool InstanceRpcReconcileRowSpace(const FHandlerContext& Ctx, const TCHAR* ArrayKey,
                                      const TArray<TSharedPtr<FJsonValue>>& Entries,
                                      bool& bInOutWorldSpace, bool& bOutAdoptedFromRows)
    {
        bOutAdoptedFromRows = false;

        int32 StampedRow = INDEX_NONE;
        bool bRowWorldSpace = false;
        for (int32 Row = 0; Row < Entries.Num(); ++Row)
        {
            const TSharedPtr<FJsonObject> Entry =
                Entries[Row].IsValid() ? Entries[Row]->AsObject() : nullptr;
            FString RowSpace;
            if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("space"), RowSpace))
            {
                continue;
            }

            RowSpace = RowSpace.ToLower();
            bool bThisRowWorld = false;
            if (RowSpace == TEXT("world"))
            {
                bThisRowWorld = true;
            }
            else if (RowSpace != TEXT("local"))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("%s[%d] declares space '%s'. Valid: world, local. "
                                         "NOTHING WAS WRITTEN."), ArrayKey, Row, *RowSpace));
                return false;
            }

            if (StampedRow == INDEX_NONE)
            {
                StampedRow = Row;
                bRowWorldSpace = bThisRowWorld;
            }
            else if (bThisRowWorld != bRowWorldSpace)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("%s[%d] declares space '%s' but %s[%d] "
                                         "declares '%s'. One call interprets ONE space, so a mixed "
                                         "batch is refused rather than resolved by order. NOTHING "
                                         "WAS WRITTEN - split it into one call per space."),
                        ArrayKey, Row, InstancedMeshUtils::SpaceToken(bThisRowWorld),
                        ArrayKey, StampedRow, InstancedMeshUtils::SpaceToken(bRowWorldSpace)));
                return false;
            }
        }

        if (StampedRow == INDEX_NONE)
        {
            return true;
        }

        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const bool bCallStatedSpace = Payload.IsValid() && Payload->HasField(TEXT("space"));
        if (!bCallStatedSpace)
        {
            bOutAdoptedFromRows = true;
            bInOutWorldSpace = bRowWorldSpace;
            return true;
        }

        if (bRowWorldSpace != bInOutWorldSpace)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("%s[%d] holds %s-space numbers but this call says "
                                     "space '%s'. Interpreting them in the wrong space would move "
                                     "the scatter to coordinates nobody asked for and report it as "
                                     "updated, so the call is REFUSED. NOTHING WAS WRITTEN - drop "
                                     "the top-level 'space' to use the record's own, or pass "
                                     "space: \"%s\" to agree with it."),
                    ArrayKey, StampedRow, InstancedMeshUtils::SpaceToken(bRowWorldSpace),
                    InstancedMeshUtils::SpaceToken(bInOutWorldSpace),
                    InstancedMeshUtils::SpaceToken(bRowWorldSpace)));
            return false;
        }
        return true;
    }

    // Resolve world -> actor -> instanced component, sending the one typed error that failed.
    // Returns false having answered the request.
    bool InstanceRpcResolveTarget(const FHandlerContext& Ctx, UWorld*& OutWorld, AActor*& OutActor,
                                  UInstancedStaticMeshComponent*& OutComponent)
    {
        OutWorld = InstanceRpcResolveWorld();
        if (!OutWorld)
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_WORLD_NOT_AVAILABLE,
                TEXT("No editor/PIE world available."));
            return false;
        }

        FString ActorName;
        if (!ActorNameParamUtils::RequireActorName(Ctx, ActorName))
        {
            return false;
        }

        OutActor = McpActorUtils::FindActorByName(OutWorld, ActorName);
        if (!OutActor)
        {
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("No actor named '%s' in the current world."), *ActorName));
            return false;
        }

        const FString ComponentName = Ctx.GetStringFirstOf(
            {TEXT("component"), TEXT("componentName"), TEXT("component_name")});
        const InstancedMeshUtils::FInstancedComponentResolution Resolution =
            InstancedMeshUtils::ResolveInstancedComponent(OutActor, ComponentName);
        if (!Resolution.Component)
        {
            Ctx.SendError(Resolution.ErrorCode, Resolution.Error);
            return false;
        }
        OutComponent = Resolution.Component;
        return true;
    }

    // Explicit `indices`, validated against the component. Returns false having sent a typed
    // error; an out-of-range index is refused rather than skipped, because instances are
    // positional and a stale index quietly addressing a DIFFERENT instance is the whole hazard.
    bool InstanceRpcReadIndices(const FHandlerContext& Ctx, UInstancedStaticMeshComponent* Component,
                                TArray<int32>& OutIndices, bool& bOutSupplied)
    {
        bOutSupplied = false;
        const TArray<TSharedPtr<FJsonValue>>* Array = Ctx.GetArray(TEXT("indices"));
        if (!Array)
        {
            return true;
        }
        bOutSupplied = true;

        const int32 Count = Component->GetInstanceCount();
        for (const TSharedPtr<FJsonValue>& Value : *Array)
        {
            double Number = 0.0;
            if (!Value.IsValid() || !Value->TryGetNumber(Number))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("'indices' must be an array of instance indices (numbers)."));
                return false;
            }
            const int32 Index = static_cast<int32>(Number);
            if (Index < 0 || Index >= Count)
            {
                Ctx.SendError(ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                    FString::Printf(TEXT("Instance index %d is outside 0..%d; '%s' carries %d "
                                         "instance(s). Nothing was read."),
                        Index, Count - 1, *Component->GetName(), Count));
                return false;
            }
            OutIndices.AddUnique(Index);
        }
        return true;
    }

    // The removal record of actor.remove_instances and of actor.add_instances' replace: one row
    // {index, space:"world", location, rotation, scale} per instance, read BEFORE anything is
    // removed. Flat rather than nested under previousTransform so the array is a valid
    // actor.add_instances `transforms` value as it stands - replaying it puts every removed
    // instance's TRANSFORM back. Per-instance custom data is not recorded: InstanceRpcAddRecordNotes
    // says so in the response, and editor.undo restores it. A discarded scatter is the one state
    // no later call can reconstruct, so a row that cannot be read refuses the whole call instead
    // of being skipped: an incomplete record followed by a completed removal is exactly the data
    // loss these verbs exist to prevent.
    bool InstanceRpcSnapshotRows(const FHandlerContext& Ctx, UInstancedStaticMeshComponent* Component,
                                 const TArray<int32>& Indices, TArray<TSharedPtr<FJsonValue>>& OutRows)
    {
        OutRows.Reserve(Indices.Num());
        for (const int32 Index : Indices)
        {
            FTransform World;
            if (!Component->GetInstanceTransform(Index, World, /*bWorldSpace*/ true))
            {
                Ctx.SendError(ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                    FString::Printf(TEXT("Instance %d of '%s' could not be read, so its removal "
                                         "could not be recorded. NOTHING WAS REMOVED."),
                        Index, *Component->GetName()));
                return false;
            }
            TSharedPtr<FJsonObject> Row = InstancedMeshUtils::TransformToJson(World);
            Row->SetNumberField(TEXT("index"), Index);
            Row->SetStringField(TEXT("space"), InstancedMeshUtils::SpaceToken(true));
            OutRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return true;
    }

    // The expectedCount guard of actor.add_instances and actor.remove_instances, refused before
    // anything is touched. Same contract as actor.set_instance_transforms' inline copy.
    bool InstanceRpcCheckExpectedCount(const FHandlerContext& Ctx, AActor* Actor,
                                       UInstancedStaticMeshComponent* Component, const TCHAR* Nothing)
    {
        const TOptional<int32> Expected =
            Ctx.GetIntFirstOf({TEXT("expectedCount"), TEXT("expected_count")});
        const int32 Count = Component->GetInstanceCount();
        if (!Expected.IsSet() || Expected.GetValue() == Count)
        {
            return true;
        }
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        InstancedMeshUtils::WriteComponentIdentity(Detail, Actor, Component);
        Detail->SetNumberField(TEXT("expectedCount"), Expected.GetValue());
        Ctx.SendError(ErrorCodes::ERR_MATCH_COUNT_MISMATCH,
            FString::Printf(TEXT("'%s' carries %d instance(s), but expectedCount says %d. %s "
                "The scatter changed since you last read it, so the indices or the scope you "
                "built this call from may no longer describe it."),
                *Component->GetName(), Count, Expected.GetValue(), Nothing),
            Detail);
        return false;
    }

    // A foliage component's instances are indexed 1:1 by the foliage ledger (FFoliageInfo), which
    // foliage.* and the foliage tools read. Adding or removing component instances behind its back
    // desyncs that mapping, and the next cluster reallocation silently rebuilds the component from
    // the ledger. Resolved by reflection: both classes are MinimalAPI in the Foliage module.
    bool InstanceRpcRefuseFoliage(const FHandlerContext& Ctx, AActor* Actor,
                                  UInstancedStaticMeshComponent* Component, const TCHAR* Nothing)
    {
        const UClass* FoliageActorClass =
            FindObject<UClass>(nullptr, TEXT("/Script/Foliage.InstancedFoliageActor"));
        const UClass* FoliageComponentClass =
            FindObject<UClass>(nullptr, TEXT("/Script/Foliage.FoliageInstancedStaticMeshComponent"));
        const bool bFoliage = (FoliageActorClass && Actor->IsA(FoliageActorClass))
            || (FoliageComponentClass && Component->IsA(FoliageComponentClass));
        if (!bFoliage)
        {
            return false;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_TARGET_KIND,
            FString::Printf(TEXT("'%s' is a foliage component: its instances belong to the foliage "
                                 "ledger, and changing their count here would desync it. Use "
                                 "foliage.add_instances / foliage.remove. %s"),
                *Component->GetName(), Nothing));
        return true;
    }

    // Response notes shared by both verbs: whether editor.undo can reach the call - bUndoable is
    // Modify()'s own answer to "was this object saved into the transaction buffer", which covers a
    // non-transactional component, no buffer, and CanTransact() refusing - and what
    // removedInstances[] cannot restore.
    void InstanceRpcAddRecordNotes(const TSharedPtr<FJsonObject>& Data,
                                   UInstancedStaticMeshComponent* Component, bool bUndoable,
                                   bool bDiscarded)
    {
        Data->SetBoolField(TEXT("undoable"), bUndoable);
        if (bDiscarded && Component->NumCustomDataFloats > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Warnings;
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("removedInstances[] records transforms only; %d custom-data float(s) per "
                     "instance are not in it. editor.undo restores them, a replay through "
                     "actor.add_instances does not."), Component->NumCustomDataFloats)));
            Data->SetArrayField(TEXT("warnings"), Warnings);
        }
    }

    // One {x,y,z} / {pitch,yaw,roll} row value, refused unless it is an object carrying exactly
    // those three numbers. ParseVectorFromJson would read `scale: 2` as the default and
    // `scale: {x: 2}` as (2,0,0) - a degenerate instance that reads back equal to the parsed
    // transform and is counted as added.
    bool InstanceRpcReadRowTriple(const FHandlerContext& Ctx, int32 Row,
                                  const TSharedPtr<FJsonObject>& Entry, const TCHAR* Key,
                                  const TArray<FString>& Axes, double (&Out)[3])
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        TArray<FString> Unknown;
        bool bValid = Entry->TryGetObjectField(Key, Obj) && Obj && Obj->IsValid();
        if (bValid)
        {
            // Case-insensitive, like the dispatcher's nested-key gate and the field lookups below.
            RejectUnknownKeys(*Obj, Axes, Unknown, ERejectUnknownKeysMode::First,
                ESearchCase::IgnoreCase);
            bValid = Unknown.Num() == 0;
            for (int32 Axis = 0; bValid && Axis < 3; ++Axis)
            {
                // A JSON number only: TryGetNumber would also read a bool as 1/0 and parse a
                // numeric string, so scale {x:false, y:false, z:false} would pass as zero scale.
                const TSharedPtr<FJsonValue> Value = (*Obj)->TryGetField(Axes[Axis]);
                bValid = Value.IsValid() && Value->Type == EJson::Number
                    && Value->TryGetNumber(Out[Axis]);
            }
        }
        if (!bValid)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("transforms[%d].%s must be an object {%s} carrying all three "
                                     "as numbers and nothing else. NOTHING WAS ADDED OR CLEARED."),
                    Row, Key, *FString::Join(Axes, TEXT(", "))));
        }
        return bValid;
    }
}

// ================= actor.get_instances =================

REGISTER_RPC_HANDLER("actor.get_instances", "actor",
    "Read the per-instance transforms of an ISM/HISM component on a placed actor, DECOMPOSED into "
    "location / rotation / scale. This is the only verb that answers the question: property.get on "
    "PerInstanceSMData returns the raw reflected FMatrix in component-LOCAL space (and by default "
    "the whole array), asset dumps elide the field entirely, and foliage.get_instances only ever "
    "sees an InstancedFoliageActor - pointed at a plain holder it returns an empty success. "
    "'component' may be omitted only when the actor carries exactly one instanced component; on an "
    "actor carrying several the call is refused with AMBIGUOUS_INSTANCED_COMPONENT, listing each "
    "one with its instance count so you can name the one you meant. Non-mutating - but the indices "
    "it returns are what the write verbs address, so a read of the wrong component is a loaded "
    "gun rather than a harmless wrong answer. Pair with "
    "actor.set_instance_transforms to write, or spatial.ground_instances to seat instances on "
    "terrain. Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label, internal name or path of the actor carrying the scatter. The "
                 "objectPath and actorPath aliases are also accepted.")),
        FParamSpec{TEXT("component"), TEXT("string"),
            TEXT("Object name of the InstancedStaticMesh/HierarchicalInstancedStaticMesh component "
                 "to read (case-insensitive). Omit ONLY when the actor carries exactly one; with "
                 "several the call is refused with AMBIGUOUS_INSTANCED_COMPONENT rather than "
                 "guessing, and the refusal lists every candidate with its instance count."),
            false, TEXT(""), TArray<FString>({TEXT("componentName"), TEXT("component_name")})},
        RPC_PARAM_OPT("indices", "array",
            "Specific instance indices to read. Omit to walk the whole scatter under limit/offset. "
            "An index outside 0..instanceCount-1 is refused with INSTANCE_INDEX_OUT_OF_RANGE rather "
            "than skipped - instances are addressed positionally and a stale index would otherwise "
            "silently name a different instance."),
        RPC_PARAM_DEF("space", "string",
            "'world' (default) returns transforms in world space, the only space comparable to a "
            "spatial.raycast hit or an actor transform. 'local' returns the component-relative "
            "transforms the component actually stores.", "world"),
        RPC_PARAM_DEF("limit", "number",
            "Maximum instances to return in this call (1-5000). instanceCount always reports the "
            "true total.", "512"),
        RPC_PARAM_DEF("offset", "number", "Index into the instance list to start from.", "0")
    ))
{
    UWorld* World = nullptr;
    AActor* Actor = nullptr;
    UInstancedStaticMeshComponent* Component = nullptr;
    if (!InstanceRpcResolveTarget(Ctx, World, Actor, Component))
    {
        return true;
    }

    bool bWorldSpace = true;
    if (!InstanceRpcParseSpace(Ctx, bWorldSpace))
    {
        return true;
    }

    TArray<int32> Indices;
    bool bExplicitIndices = false;
    if (!InstanceRpcReadIndices(Ctx, Component, Indices, bExplicitIndices))
    {
        return true;
    }

    const int32 Count = Component->GetInstanceCount();
    if (!bExplicitIndices)
    {
        Indices.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Indices.Add(Index);
        }
    }

    const int32 Offset = FMath::Max(Ctx.GetInt(TEXT("offset"), 0), 0);
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), InstanceRpcDefaultLimit),
        1, InstanceRpcMaxBatch);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 Cursor = Offset; Cursor < Indices.Num() && Rows.Num() < Limit; ++Cursor)
    {
        FTransform InstanceTransform;
        if (!Component->GetInstanceTransform(Indices[Cursor], InstanceTransform, bWorldSpace))
        {
            // Unreachable after the range check above unless the scatter changed underneath this
            // call. Skipping is the honest answer: the row would otherwise carry an identity
            // transform indistinguishable from a real one at the origin.
            continue;
        }
        TSharedPtr<FJsonObject> Row = InstancedMeshUtils::TransformToJson(InstanceTransform);
        Row->SetNumberField(TEXT("index"), Indices[Cursor]);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    InstancedMeshUtils::WriteComponentIdentity(Data, Actor, Component);
    Data->SetNumberField(TEXT("selected"), Indices.Num());
    Data->SetNumberField(TEXT("returned"), Rows.Num());
    Data->SetNumberField(TEXT("offset"), Offset);
    Data->SetBoolField(TEXT("truncated"), Rows.Num() < FMath::Max(Indices.Num() - Offset, 0));
    Data->SetArrayField(TEXT("instances"), Rows);
    InstanceRpcAddAxisEcho(Data, bWorldSpace);

    Ctx.SendSuccess(Data);
    return true;
}

// ================= actor.set_instance_transforms =================

REGISTER_RPC_HANDLER("actor.set_instance_transforms", "actor",
    "Write per-instance transforms on an ISM/HISM component, in one batch, and report what each "
    "instance WAS. This is the only write path that works: a property.set / container.array.set "
    "into PerInstanceSMData stores the matrix and reports success while the render buffers, the "
    "instance physics bodies, navigation and a HISM's cluster tree are never told, so nothing in "
    "the world moves. This verb goes through UInstancedStaticMeshComponent::UpdateInstanceTransform, "
    "which does all of that, then dirties the render state once and rebuilds a HISM's tree "
    "synchronously. "
    "ALL-OR-NOTHING: one out-of-range index, one duplicated index, or an expectedCount that "
    "disagrees with the component refuses the whole call before the first write - instances are "
    "addressed positionally and a re-scatter renumbers them. Omitted location/rotation/scale fields "
    "keep the instance's current values, but a row that requests NO pose at all is refused rather "
    "than counted as updated. Every instance written is echoed in movedInstances[] with its "
    "pre-write transform AND the space that transform is in; that is the undo, and passing it "
    "straight back to this verb restores the scatter - the rows' own `space` is adopted when the "
    "replay states none, and a replay that states a DIFFERENT space is refused rather than writing "
    "the record's numbers into the wrong space and reporting success. The batch is also wrapped in "
    "one editor transaction, so editor.undo reaches it. Each write is verified by reading the "
    "instance back, so `updated` counts instances the component actually holds at the requested "
    "pose. Coordinates are unreal units (cm), left-handed (+X fwd, +Y right, +Z up).",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label, internal name or path of the actor carrying the scatter.")),
        FParamSpec{TEXT("component"), TEXT("string"),
            TEXT("Object name of the instanced component to write (case-insensitive). Omit ONLY "
                 "when the actor carries exactly one; with several the call is refused with "
                 "AMBIGUOUS_INSTANCED_COMPONENT before anything is written, listing every "
                 "candidate with its instance count. There is no largest-wins default: on a "
                 "shared holder the biggest component is routinely another caller's scatter."),
            false, TEXT(""), TArray<FString>({TEXT("componentName"), TEXT("component_name")})},
        RPC_PARAM_REQ("instances", "array",
            "REQUIRED. Rows of {index, location?:{x,y,z}, rotation?:{pitch,yaw,roll}, "
            "scale?:{x,y,z}}. index is REQUIRED per row and must be within 0..instanceCount-1; an "
            "omitted location/rotation/scale keeps that part of the instance's current transform. "
            "A row may instead carry previousTransform:{location,rotation,scale} plus space - the "
            "shape movedInstances[] rows come back in, from this verb and from "
            "spatial.ground_instances - and that pose becomes the row's base, so an undo record "
            "is handed back UNCHANGED to restore the scatter. Explicit location/rotation/scale "
            "still win over it, field by field. A row's `space` is an assertion about that row's "
            "numbers: with no top-level space the rows' space is ADOPTED, with a conflicting one "
            "the whole call is refused with INVALID_ARGUMENT, and rows disagreeing with each other "
            "are refused too. A row carrying NONE of previousTransform / location / rotation / "
            "scale is refused - it would rewrite the pose the instance already has and be counted "
            "as updated, which is also what a misspelled row key degrades to (row keys are not "
            "covered by the top-level UNKNOWN_PARAMS gate). "
            "Indices must be unique - two rows for one instance is ambiguous and is refused rather "
            "than resolved by order. Capped at 5000 rows per call."),
        RPC_PARAM_DEF("space", "string",
            "'world' (default) interprets the supplied transforms in world space. 'local' "
            "interprets them as component-relative, which is what the component stores. Omit it "
            "when replaying a movedInstances[] record: the rows carry their own space and it is "
            "adopted, and the response says so with spaceFrom. Stating one that disagrees with the "
            "rows refuses the call.", "world"),
        FParamSpec{TEXT("expectedCount"), TEXT("integer"),
            TEXT("How many instances you believe the component carries. When it disagrees the call "
                 "is refused with MATCH_COUNT_MISMATCH BEFORE anything is written. Instance indices "
                 "are positional: anything that re-scatters, adds or removes instances renumbers "
                 "them, so indices read in an earlier call can silently address different "
                 "instances. Read the current number from actor.get_instances' instanceCount."),
            false, TEXT(""), TArray<FString>({TEXT("expected_count")})}
    ))
{
    UWorld* World = nullptr;
    AActor* Actor = nullptr;
    UInstancedStaticMeshComponent* Component = nullptr;
    if (!InstanceRpcResolveTarget(Ctx, World, Actor, Component))
    {
        return true;
    }

    bool bWorldSpace = true;
    if (!InstanceRpcParseSpace(Ctx, bWorldSpace))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (!Ctx.RequireArray(TEXT("instances"), Entries) || !Entries)
    {
        return true;
    }
    if (Entries->Num() == 0)
    {
        // An empty batch is an error rather than a zero-item success, for the same reason
        // spatial.ground_actors refuses an empty match set: a caller that built the array wrong
        // otherwise cannot tell it from a clean run.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("'instances' is empty. An empty batch is refused rather than reported as a "
                 "zero-instance success."));
        return true;
    }
    if (Entries->Num() > InstanceRpcMaxBatch)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'instances' carries %d rows; the per-call ceiling is %d. Split "
                                 "the batch."), Entries->Num(), InstanceRpcMaxBatch));
        return true;
    }

    // Before anything else reads a pose: settle which space these numbers are in. A replayed undo
    // record states its own, and it either sets the space this call uses or refuses the call.
    bool bSpaceFromRows = false;
    if (!InstanceRpcReconcileRowSpace(Ctx, TEXT("instances"), *Entries, bWorldSpace,
            bSpaceFromRows))
    {
        return true;
    }

    const int32 InstanceCount = Component->GetInstanceCount();

    const TOptional<int32> Expected =
        Ctx.GetIntFirstOf({TEXT("expectedCount"), TEXT("expected_count")});
    if (Expected.IsSet() && Expected.GetValue() != InstanceCount)
    {
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        InstancedMeshUtils::WriteComponentIdentity(Detail, Actor, Component);
        Detail->SetNumberField(TEXT("expectedCount"), Expected.GetValue());
        Ctx.SendError(ErrorCodes::ERR_MATCH_COUNT_MISMATCH,
            FString::Printf(TEXT("'%s' carries %d instance(s), but expectedCount says %d. NOTHING "
                "WAS WRITTEN. Instance indices are positional, so a component whose count changed "
                "has renumbered them and the indices in this call may name different instances "
                "than the ones they were read from."),
                *Component->GetName(), InstanceCount, Expected.GetValue()),
            Detail);
        return true;
    }

    // ---- Pre-flight: resolve every row before writing any of them ----
    struct FInstanceWriteRequest
    {
        int32 Index = INDEX_NONE;
        FTransform Previous;
        FTransform Target;
    };
    TArray<FInstanceWriteRequest> Requests;
    Requests.Reserve(Entries->Num());
    TSet<int32> SeenIndices;

    for (int32 Row = 0; Row < Entries->Num(); ++Row)
    {
        const TSharedPtr<FJsonObject> Entry = (*Entries)[Row].IsValid()
            ? (*Entries)[Row]->AsObject() : nullptr;
        if (!Entry.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("instances[%d] is not an object. Nothing was written."), Row));
            return true;
        }

        double IndexNumber = 0.0;
        if (!Entry->TryGetNumberField(TEXT("index"), IndexNumber))
        {
            Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
                FString::Printf(TEXT("instances[%d] has no 'index'. Every row must name the "
                                     "instance it writes. Nothing was written."), Row));
            return true;
        }
        const int32 Index = static_cast<int32>(IndexNumber);
        if (Index < 0 || Index >= InstanceCount)
        {
            Ctx.SendError(ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                FString::Printf(TEXT("instances[%d] names instance %d, outside 0..%d; '%s' carries "
                                     "%d instance(s). NOTHING WAS WRITTEN - a batch keyed on stale "
                                     "indices is refused whole rather than applied in part."),
                    Row, Index, InstanceCount - 1, *Component->GetName(), InstanceCount));
            return true;
        }
        bool bAlreadySeen = false;
        SeenIndices.Add(Index, &bAlreadySeen);
        if (bAlreadySeen)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("instance %d appears in more than one row. Which one wins is "
                                     "ambiguous, so the call is refused rather than resolved by "
                                     "order. Nothing was written."), Index));
            return true;
        }

        // A row that requests no pose at all resolves every field to the instance's CURRENT
        // transform, writes it back unchanged, reads back identical and counts as `updated` -
        // accepted, counted, restored nothing. It is also what a misspelled row key degrades to:
        // the dispatcher's UNKNOWN_PARAMS gate is top-level only and never inspects instances[]
        // row keys, so {index: 7, previousTransfrom: {...}} is indistinguishable from {index: 7}
        // once it reaches here. Refusing it is the only structural check that catches the typo.
        if (!Entry->HasField(TEXT("previousTransform")) && !Entry->HasField(TEXT("location"))
            && !Entry->HasField(TEXT("rotation")) && !Entry->HasField(TEXT("scale")))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("instances[%d] (instance %d) requests no pose: it carries none "
                                     "of previousTransform, location, rotation or scale, so it "
                                     "would re-apply the transform the instance already has and be "
                                     "reported as updated. NOTHING WAS WRITTEN - check for a "
                                     "misspelled key; row keys are not covered by the "
                                     "UNKNOWN_PARAMS gate."), Row, Index));
            return true;
        }

        FInstanceWriteRequest Request;
        Request.Index = Index;
        if (!Component->GetInstanceTransform(Index, Request.Previous, bWorldSpace))
        {
            Ctx.SendError(ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                FString::Printf(TEXT("Instance %d could not be read back from '%s'. Nothing was "
                                     "written."), Index, *Component->GetName()));
            return true;
        }

        // A movedInstances[] row handed straight back IS the undo path this verb promises, and
        // both producers of those rows (this verb and spatial.ground_instances) carry the pose
        // under `previousTransform` rather than as bare location/rotation/scale. Read as a plain
        // row it names an instance and asks for nothing: every field falls back to the pose the
        // instance ALREADY has, the write re-applies it, and `updated` counts it - a success that
        // restores nothing, which is the exact silent no-op these verbs exist to replace. So an
        // undo row's pose is the base the omitted fields fall back to instead.
        FTransform Base = Request.Previous;
        const TSharedPtr<FJsonObject>* Undo = nullptr;
        if (Entry->TryGetObjectField(TEXT("previousTransform"), Undo) && Undo)
        {
            Base = FTransform(
                ParseRotatorFromJson(*Undo, TEXT("rotation"), Base.GetRotation().Rotator()),
                ParseVectorFromJson(*Undo, TEXT("location"), Base.GetLocation()),
                ParseVectorFromJson(*Undo, TEXT("scale"), Base.GetScale3D()));
        }

        // Omitted fields keep the base pose - what the instance already has, or the
        // previousTransform an undo row supplied - so a caller moving a scatter down does not
        // have to restate every rotation and scale it never read.
        const FVector Location = ParseVectorFromJson(Entry, TEXT("location"), Base.GetLocation());
        const FRotator Rotation = ParseRotatorFromJson(Entry, TEXT("rotation"),
            Base.GetRotation().Rotator());
        const FVector Scale = ParseVectorFromJson(Entry, TEXT("scale"), Base.GetScale3D());
        Request.Target = FTransform(Rotation, Location, Scale);
        Requests.Add(Request);
    }

    // ---- Write ----
    TArray<TSharedPtr<FJsonValue>> MovedRows;
    TArray<TSharedPtr<FJsonValue>> MismatchRows;
    TArray<FInstanceWriteRequest> Applied;
    Applied.Reserve(Requests.Num());
    int32 UpdatedCount = 0;
    int32 MismatchCount = 0;

    {
        // ONE transaction for the whole batch, opened after the all-or-nothing pre-flight passed
        // so a refused call leaves no empty entry on the undo stack. UpdateInstanceTransform's own
        // Modify() calls have somewhere to record only while this is open, and they cost ONE
        // snapshot between them: FTransaction::SaveObject builds an FObjectRecord only for an
        // object that has none yet. See the header of InstancedMeshUtils.h for the arithmetic that
        // used to argue against this.
        FScopedTransaction Transaction(
            FText::FromString(TEXT("MCP: actor.set_instance_transforms")));

        for (const FInstanceWriteRequest& Request : Requests)
        {
            // bMarkRenderStateDirty=false: the batch dirties ONCE below. bTeleport=true so a
            // physics body is moved rather than swept.
            if (!Component->UpdateInstanceTransform(Request.Index, Request.Target, bWorldSpace,
                    /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true))
            {
                ++MismatchCount;
                if (MismatchRows.Num() < InstanceRpcMaxMismatchRows)
                {
                    TSharedPtr<FJsonObject> Miss = MakeShared<FJsonObject>();
                    Miss->SetNumberField(TEXT("index"), Request.Index);
                    Miss->SetStringField(TEXT("reason"),
                        TEXT("UpdateInstanceTransform refused the write."));
                    MismatchRows.Add(MakeShared<FJsonValueObject>(Miss));
                }
                continue;
            }
            Applied.Add(Request);

            // Stamped with the space it was READ in, which is the space this call interpreted -
            // so the row still means something once it is lifted out of this response.
            MovedRows.Add(MakeShared<FJsonValueObject>(
                InstancedMeshUtils::MakeMovedInstanceRow(Request.Index, Request.Previous,
                    bWorldSpace)));
        }
    }

    InstancedMeshUtils::FinishInstanceWrites(Component);

    // ---- Verify, after the batch closed out ----
    // `updated` is what the component HOLDS, re-read from the engine, not the count of calls that
    // returned true. That distinction is the entire point of this verb: the route it replaces
    // reported success and read back the value it had just stored while the world kept the old one.
    // Only writes that were accepted are re-read - a refusal is already counted, and counting it
    // twice would make mismatchedCount say something other than "instances not at the requested
    // pose".
    for (const FInstanceWriteRequest& Request : Applied)
    {
        FTransform Actual;
        const bool bReadBack = Component->GetInstanceTransform(Request.Index, Actual, bWorldSpace);
        if (bReadBack && Actual.Equals(Request.Target, InstanceRpcReadbackToleranceCm))
        {
            ++UpdatedCount;
            continue;
        }
        ++MismatchCount;
        if (MismatchRows.Num() < InstanceRpcMaxMismatchRows)
        {
            TSharedPtr<FJsonObject> Miss = MakeShared<FJsonObject>();
            Miss->SetNumberField(TEXT("index"), Request.Index);
            Miss->SetObjectField(TEXT("requested"),
                InstancedMeshUtils::TransformToJson(Request.Target));
            if (bReadBack)
            {
                Miss->SetObjectField(TEXT("actual"), InstancedMeshUtils::TransformToJson(Actual));
            }
            else
            {
                // `actual` is omitted rather than filled from the default-constructed FTransform:
                // an identity serialised here would read as a real measurement of an instance
                // sitting at the origin.
                Miss->SetStringField(TEXT("reason"),
                    TEXT("The instance could not be read back after the write."));
            }
            MismatchRows.Add(MakeShared<FJsonValueObject>(Miss));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    InstancedMeshUtils::WriteComponentIdentity(Data, Actor, Component);
    Data->SetNumberField(TEXT("requested"), Requests.Num());
    Data->SetNumberField(TEXT("updated"), UpdatedCount);
    // The undo log, written before the diagnostics because it is the more important of the two:
    // one row per instance this call wrote, with the transform it had before AND the space that
    // transform is in.
    Data->SetArrayField(TEXT("movedInstances"), MovedRows);
    if (MismatchCount > 0)
    {
        Data->SetNumberField(TEXT("mismatchedCount"), MismatchCount);
        Data->SetArrayField(TEXT("mismatched"), MismatchRows);
    }
    InstanceRpcAddAxisEcho(Data, bWorldSpace);
    if (bSpaceFromRows)
    {
        // The echoed `space` above is what this call USED. When it came from the rows' own stamps
        // rather than from a `space` argument, say so rather than letting the caller read it as
        // the default they never passed.
        Data->SetStringField(TEXT("spaceFrom"), TEXT("instances[].space"));
    }

    Ctx.SendSuccess(Data);
    return true;
}

// ================= actor.add_instances =================
//
// The two ends of the scatter lifecycle the read/write/ground trio never had. Before these, the
// only way to fill or empty an instance array was python.execute, and the clear-then-refill shape
// a re-scatter needs was two statements in one script: clear_instances() ran, the refill raised,
// and the scatter was gone. Here the clear and the refill are one call, one owner of the failure
// and one editor transaction, and every row is validated before either half runs.

REGISTER_RPC_HANDLER("actor.add_instances", "actor",
    "Append instances to an ISM/HISM component on a placed actor, in one batch, and report the "
    "indices they landed at. With replace:true the component's existing instances are cleared "
    "first, IN THE SAME CALL AND THE SAME editor transaction, after every row has been validated - "
    "so a bad row refuses the whole call with the old scatter untouched, where a clear followed by "
    "a refill in python.execute loses the scatter when the refill raises. "
    "ALL-OR-NOTHING: a row with no location, a wrong-shaped location/rotation/scale, an unknown "
    "row key, a conflicting space stamp, or an expectedCount that disagrees with the component "
    "refuses the call before anything is added or cleared. Every added instance is read back and "
    "compared with the request; a disagreement is VERIFICATION_FAILED, not a success. "
    "replace:true echoes removedInstances[] - every discarded instance with its world transform, "
    "complete and uncapped, so a large scatter's response spills to a file - which is itself a "
    "valid `transforms` value for this verb. It restores transforms only: per-instance custom "
    "data is not recorded (a `warnings` entry says so), while editor.undo restores everything when "
    "the response's `undoable` is true. Foliage components are refused with INVALID_TARGET_KIND - "
    "their instances belong to the foliage ledger; use foliage.*. The component must already "
    "exist: actor.add_component {componentType: "
    "\"HierarchicalInstancedStaticMeshComponent\", meshPath} creates an empty one with its mesh "
    "set. Use actor.set_instance_transforms to move existing instances and actor.remove_instances "
    "to delete some. The level is marked dirty, not saved (saved:false, pendingFlush:true); "
    "persist it with level.save. Coordinates are unreal units (cm), left-handed (+X fwd, +Y "
    "right, +Z up).",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label, internal name or path of the actor carrying the instanced "
                 "component.")),
        FParamSpec{TEXT("component"), TEXT("string"),
            TEXT("Object name of the instanced component to add to (case-insensitive). Omit ONLY "
                 "when the actor carries exactly one; with several the call is refused with "
                 "AMBIGUOUS_INSTANCED_COMPONENT before anything is written."),
            false, TEXT(""), TArray<FString>({TEXT("componentName"), TEXT("component_name")})},
        RPC_PARAM_REQ_NESTED("transforms", "array",
            "REQUIRED. Rows of {location, rotation, scale, space, index} - the shape "
            "spatial.scatter_layout returns in transforms[] and actor.remove_instances returns in "
            "removedInstances[], so either is passed straight through. location is REQUIRED per "
            "row (a row without it would silently land at the origin) and is an object of x, y, z; "
            "rotation (pitch, yaw, roll) defaults to zero and scale (x, y, z) to 1. Each of the three "
            "must be an object carrying all three numbers and nothing else, or the call is refused. "
            "`space` is adopted when the call states none and refused when it conflicts; `index` "
            "(a removal record's old index) is ignored: new instances are always appended, and "
            "addedIndices[] says where. These five keys are the whole row schema and any other "
            "key is refused with UNKNOWN_NESTED_PARAMS, so a misspelled rotation or scale cannot "
            "degrade to the default. 1-5000 rows per call.",
            TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("space"), TEXT("index")),
        RPC_PARAM_DEF("space", "string",
            "'world' (default) interprets the transforms in world space. 'local' interprets them "
            "as component-relative, which is what the component stores. Omit it when replaying a "
            "removedInstances[] record: the rows carry their own space and it is adopted.",
            "world"),
        RPC_PARAM_DEF("replace", "boolean",
            "true clears every existing instance first, in the same transaction and only after "
            "every row has been validated; the discarded instances are echoed in "
            "removedInstances[]. false (default) appends.", "false"),
        FParamSpec{TEXT("expectedCount"), TEXT("integer"),
            TEXT("How many instances you believe the component carries now. When it disagrees the "
                 "call is refused with MATCH_COUNT_MISMATCH before anything is added or cleared - "
                 "use it with replace:true so a scatter that changed since you read it is not "
                 "discarded on a stale belief. An append is not idempotent - a retry after a "
                 "timeout adds the batch twice - and expectedCount turns that retry into a "
                 "refusal. Read the number from actor.get_instances' instanceCount."),
            false, TEXT(""), TArray<FString>({TEXT("expected_count")})}
    ))
{
    UWorld* World = nullptr;
    AActor* Actor = nullptr;
    UInstancedStaticMeshComponent* Component = nullptr;
    if (!InstanceRpcResolveTarget(Ctx, World, Actor, Component))
    {
        return true;
    }

    if (InstanceRpcRefuseFoliage(Ctx, Actor, Component, TEXT("NOTHING WAS ADDED OR CLEARED.")))
    {
        return true;
    }

    bool bWorldSpace = true;
    if (!InstanceRpcParseSpace(Ctx, bWorldSpace))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (!Ctx.RequireArray(TEXT("transforms"), Entries) || !Entries)
    {
        return true;
    }
    if (Entries->Num() == 0 || Entries->Num() > InstanceRpcMaxBatch)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'transforms' carries %d rows; 1-%d are accepted per call. An "
                                 "empty batch is refused rather than reported as a zero-instance "
                                 "success. NOTHING WAS ADDED OR CLEARED."),
                Entries->Num(), InstanceRpcMaxBatch));
        return true;
    }

    bool bSpaceFromRows = false;
    if (!InstanceRpcReconcileRowSpace(Ctx, TEXT("transforms"), *Entries, bWorldSpace,
            bSpaceFromRows))
    {
        return true;
    }

    if (!InstanceRpcCheckExpectedCount(Ctx, Actor, Component, TEXT("NOTHING WAS ADDED OR CLEARED.")))
    {
        return true;
    }
    const bool bReplace = Ctx.GetBool(TEXT("replace"), false);

    // ---- Pre-flight: every row parsed before the clear, so a bad row costs the caller nothing.
    // Unknown row keys never get here: `transforms` declares its NestedKeys, so the dispatcher
    // refuses them with UNKNOWN_NESTED_PARAMS before the handler runs. ----
    static const TArray<FString> XyzAxes = {TEXT("x"), TEXT("y"), TEXT("z")};
    static const TArray<FString> RotatorAxes = {TEXT("pitch"), TEXT("yaw"), TEXT("roll")};
    TArray<FTransform> Transforms;
    Transforms.Reserve(Entries->Num());
    for (int32 Row = 0; Row < Entries->Num(); ++Row)
    {
        const TSharedPtr<FJsonObject> Entry = (*Entries)[Row].IsValid()
            ? (*Entries)[Row]->AsObject() : nullptr;
        if (!Entry.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("transforms[%d] is not an object. NOTHING WAS ADDED OR "
                                     "CLEARED."), Row));
            return true;
        }
        if (!Entry->HasField(TEXT("location")))
        {
            Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
                FString::Printf(TEXT("transforms[%d] has no location object {x,y,z}. NOTHING WAS "
                                     "ADDED OR CLEARED."), Row));
            return true;
        }
        double Location[3] = {0.0, 0.0, 0.0};
        double Rotation[3] = {0.0, 0.0, 0.0};
        double Scale[3] = {1.0, 1.0, 1.0};
        if (!InstanceRpcReadRowTriple(Ctx, Row, Entry, TEXT("location"), XyzAxes, Location)
            || (Entry->HasField(TEXT("rotation"))
                && !InstanceRpcReadRowTriple(Ctx, Row, Entry, TEXT("rotation"), RotatorAxes, Rotation))
            || (Entry->HasField(TEXT("scale"))
                && !InstanceRpcReadRowTriple(Ctx, Row, Entry, TEXT("scale"), XyzAxes, Scale)))
        {
            return true;
        }
        Transforms.Add(FTransform(FRotator(Rotation[0], Rotation[1], Rotation[2]),
            FVector(Location[0], Location[1], Location[2]), FVector(Scale[0], Scale[1], Scale[2])));
    }

    const int32 PreviousCount = Component->GetInstanceCount();
    TArray<TSharedPtr<FJsonValue>> RemovedRows;
    if (bReplace)
    {
        TArray<int32> Everything;
        Everything.Reserve(PreviousCount);
        for (int32 Index = 0; Index < PreviousCount; ++Index)
        {
            Everything.Add(Index);
        }
        if (!InstanceRpcSnapshotRows(Ctx, Component, Everything, RemovedRows))
        {
            return true;
        }
    }

    // ---- Write: clear (when asked) and append in ONE transaction ----
    TArray<int32> NewIndices;
    bool bUndoable = false;
    {
        // Neither ClearInstances nor AddInstances calls Modify() itself, so the transaction would
        // record nothing without the explicit call. One snapshot covers both halves.
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: actor.add_instances")));
        bUndoable = Component->Modify();
        if (bReplace)
        {
            Component->ClearInstances();
        }
        NewIndices = Component->AddInstances(Transforms, /*bShouldReturnIndices*/ true, bWorldSpace);
    }
    InstancedMeshUtils::FinishInstanceWrites(Component);

    // ---- Verify: what the component HOLDS, re-read, not what AddInstances returned ----
    TArray<TSharedPtr<FJsonValue>> IndexValues;
    TArray<TSharedPtr<FJsonValue>> MismatchRows;
    int32 AddedCount = 0;
    int32 MismatchCount = 0;
    for (int32 Row = 0; Row < Transforms.Num(); ++Row)
    {
        FTransform Actual;
        const bool bReadBack = NewIndices.IsValidIndex(Row)
            && Component->GetInstanceTransform(NewIndices[Row], Actual, bWorldSpace);
        if (NewIndices.IsValidIndex(Row))
        {
            IndexValues.Add(MakeShared<FJsonValueNumber>(NewIndices[Row]));
        }
        if (bReadBack && Actual.Equals(Transforms[Row], InstanceRpcReadbackToleranceCm))
        {
            ++AddedCount;
            continue;
        }
        ++MismatchCount;
        if (MismatchRows.Num() < InstanceRpcMaxMismatchRows)
        {
            TSharedPtr<FJsonObject> Miss = MakeShared<FJsonObject>();
            Miss->SetNumberField(TEXT("row"), Row);
            Miss->SetObjectField(TEXT("requested"), InstancedMeshUtils::TransformToJson(Transforms[Row]));
            if (bReadBack)
            {
                Miss->SetNumberField(TEXT("index"), NewIndices[Row]);
                Miss->SetObjectField(TEXT("actual"), InstancedMeshUtils::TransformToJson(Actual));
            }
            else
            {
                Miss->SetStringField(TEXT("reason"),
                    TEXT("No instance for this row could be read back after the write."));
            }
            MismatchRows.Add(MakeShared<FJsonValueObject>(Miss));
        }
    }
    const int32 ExpectedAfter = (bReplace ? 0 : PreviousCount) + Transforms.Num();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    InstancedMeshUtils::WriteComponentIdentity(Data, Actor, Component);
    Data->SetNumberField(TEXT("previousCount"), PreviousCount);
    Data->SetNumberField(TEXT("requested"), Transforms.Num());
    Data->SetNumberField(TEXT("added"), AddedCount);
    Data->SetArrayField(TEXT("addedIndices"), IndexValues);
    Data->SetBoolField(TEXT("replaced"), bReplace);
    if (bReplace)
    {
        Data->SetArrayField(TEXT("removedInstances"), RemovedRows);
    }
    InstanceRpcAddRecordNotes(Data, Component, bUndoable, RemovedRows.Num() > 0);
    if (MismatchCount > 0)
    {
        Data->SetNumberField(TEXT("mismatchedCount"), MismatchCount);
        Data->SetArrayField(TEXT("mismatched"), MismatchRows);
    }
    InstanceRpcAddAxisEcho(Data, bWorldSpace);
    if (bSpaceFromRows)
    {
        Data->SetStringField(TEXT("spaceFrom"), TEXT("transforms[].space"));
    }
    AddMarkDirtySaveReport(Data, Component, /*bSaveRequested*/ true);

    if (MismatchCount > 0 || Component->GetInstanceCount() != ExpectedAfter)
    {
        Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("'%s' holds %d instance(s) after the write where %d were expected, "
                                 "and %d of %d added row(s) did not read back at the requested "
                                 "pose. %s%s."),
                *Component->GetName(), Component->GetInstanceCount(), ExpectedAfter,
                MismatchCount, Transforms.Num(),
                bUndoable
                    ? TEXT("editor.undo reverts the call")
                    : TEXT("The call was not recorded in the undo buffer, so editor.undo cannot revert it"),
                bReplace ? TEXT("; removedInstances[] holds the cleared scatter") : TEXT("")),
            Data);
        return true;
    }
    Ctx.SendSuccess(Data);
    return true;
}

// ================= actor.remove_instances =================

REGISTER_RPC_HANDLER("actor.remove_instances", "actor",
    "Remove instances from an ISM/HISM component on a placed actor - the listed indices, or every "
    "instance with all:true - and echo each removed instance's world transform. Exactly one of "
    "indices / all:true is required. ALL-OR-NOTHING: an out-of-range or duplicated index, an "
    "already-empty component, or an expectedCount that disagrees with the component refuses the "
    "call before anything is removed, and `indices` requires expectedCount, because a retried "
    "removal by index would delete whichever instances were renumbered into those slots. "
    "removedInstances[] rows {index, space, location, rotation, scale} are read before the removal, "
    "complete and uncapped (a large clear's response spills to a file), and are a valid "
    "actor.add_instances `transforms` value as they stand, so handing them back restores the "
    "instances' transforms at new, appended indices - per-instance custom data is not recorded, "
    "and a `warnings` entry says so. The call is one editor transaction, so editor.undo reverts "
    "all of it when the response's `undoable` is true. Foliage components are refused with "
    "INVALID_TARGET_KIND; use foliage.remove. REMOVAL RENUMBERS: instances are "
    "addressed by position and the engine closes the gap (RemoveAt, or RemoveAtSwap when the "
    "component opts in), so the index of any surviving instance may change - re-read with "
    "actor.get_instances before addressing one by index. `removed` is measured from the instance "
    "count before and after, not echoed from the request. To clear and refill in one step use "
    "actor.add_instances with replace:true. The level is marked dirty, not saved (saved:false, "
    "pendingFlush:true); persist it with level.save.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label, internal name or path of the actor carrying the instanced "
                 "component.")),
        FParamSpec{TEXT("component"), TEXT("string"),
            TEXT("Object name of the instanced component to remove from (case-insensitive). Omit "
                 "ONLY when the actor carries exactly one; with several the call is refused with "
                 "AMBIGUOUS_INSTANCED_COMPONENT before anything is removed."),
            false, TEXT(""), TArray<FString>({TEXT("componentName"), TEXT("component_name")})},
        RPC_PARAM_OPT("indices", "array",
            "Instance indices to remove (1-5000), whole numbers each within 0..instanceCount-1 and "
            "each at most once. Requires expectedCount (MISSING_REQUIRED_PARAM without it): a "
            "removal by index is not idempotent, and the count is what refuses a retry. "
            "Mutually exclusive with all:true; one of the two is required. An out-of-range or "
            "repeated index refuses the whole call with nothing removed."),
        RPC_PARAM_DEF("all", "boolean",
            "true removes every instance on the component. Mutually exclusive with indices.",
            "false"),
        FParamSpec{TEXT("expectedCount"), TEXT("integer"),
            TEXT("How many instances you believe the component carries now. When it disagrees the "
                 "call is refused with MATCH_COUNT_MISMATCH before anything is removed: indices "
                 "are positional, so indices read before the scatter changed may name different "
                 "instances. Read the number from actor.get_instances' instanceCount."),
            false, TEXT(""), TArray<FString>({TEXT("expected_count")})}
    ))
{
    UWorld* World = nullptr;
    AActor* Actor = nullptr;
    UInstancedStaticMeshComponent* Component = nullptr;
    if (!InstanceRpcResolveTarget(Ctx, World, Actor, Component))
    {
        return true;
    }

    if (InstanceRpcRefuseFoliage(Ctx, Actor, Component, TEXT("NOTHING WAS REMOVED.")))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* IndexArray = Ctx.GetArray(TEXT("indices"));
    const bool bAll = Ctx.GetBool(TEXT("all"), false);
    if (bAll == (IndexArray != nullptr))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Pass exactly one of 'indices' or all:true - there is no default scope for a "
                 "removal. NOTHING WAS REMOVED."));
        return true;
    }

    // Body-enforced, because the declaration gate cannot say "required WITH indices". A removal
    // by index is not idempotent: a retry after a transport timeout lands on a component the first
    // call already renumbered, and removes whichever instances moved into those slots. The count
    // is the only thing that tells the retry apart. all:true needs no guard - its retry is refused
    // as an empty component.
    if (IndexArray && !Ctx.GetIntFirstOf({TEXT("expectedCount"), TEXT("expected_count")}).IsSet())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_REQUIRED_PARAM,
            TEXT("'expectedCount' is required with 'indices': indices are positional, so a retried "
                 "or stale removal would delete whichever instances now occupy those slots. Read "
                 "instanceCount from actor.get_instances. NOTHING WAS REMOVED."));
        return true;
    }

    if (!InstanceRpcCheckExpectedCount(Ctx, Actor, Component, TEXT("NOTHING WAS REMOVED.")))
    {
        return true;
    }

    const int32 PreviousCount = Component->GetInstanceCount();
    if (PreviousCount == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'%s' carries no instances, so there is nothing to remove. An "
                                 "empty removal is refused rather than reported as a zero-instance "
                                 "success."), *Component->GetName()));
        return true;
    }

    TArray<int32> Indices;
    if (bAll)
    {
        Indices.Reserve(PreviousCount);
        for (int32 Index = 0; Index < PreviousCount; ++Index)
        {
            Indices.Add(Index);
        }
    }
    else
    {
        if (IndexArray->Num() == 0 || IndexArray->Num() > InstanceRpcMaxBatch)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("'indices' carries %d entries; 1-%d are accepted per call "
                                     "(all:true has no cap). NOTHING WAS REMOVED."),
                    IndexArray->Num(), InstanceRpcMaxBatch));
            return true;
        }
        TSet<int32> Seen;
        for (const TSharedPtr<FJsonValue>& Value : *IndexArray)
        {
            double Number = 0.0;
            if (!Value.IsValid() || !Value->TryGetNumber(Number))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("'indices' must be an array of instance indices (numbers). NOTHING WAS "
                         "REMOVED."));
                return true;
            }
            if (Number != FMath::FloorToDouble(Number))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("'indices' holds %g, which is not a whole instance index. "
                                         "NOTHING WAS REMOVED."), Number));
                return true;
            }
            const int32 Index = static_cast<int32>(Number);
            if (Index < 0 || Index >= PreviousCount)
            {
                Ctx.SendError(ErrorCodes::ERR_INSTANCE_INDEX_OUT_OF_RANGE,
                    FString::Printf(TEXT("Instance index %d is outside 0..%d; '%s' carries %d "
                                         "instance(s). NOTHING WAS REMOVED."),
                        Index, PreviousCount - 1, *Component->GetName(), PreviousCount));
                return true;
            }
            bool bAlreadySeen = false;
            Seen.Add(Index, &bAlreadySeen);
            if (bAlreadySeen)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("instance %d is listed more than once. NOTHING WAS "
                                         "REMOVED."), Index));
                return true;
            }
            Indices.Add(Index);
        }
    }

    TArray<TSharedPtr<FJsonValue>> RemovedRows;
    if (!InstanceRpcSnapshotRows(Ctx, Component, Indices, RemovedRows))
    {
        return true;
    }

    bool bUndoable = false;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: actor.remove_instances")));
        bUndoable = Component->Modify();
        if (bAll)
        {
            Component->ClearInstances();
        }
        else
        {
            Component->RemoveInstances(Indices);
        }
    }
    InstancedMeshUtils::FinishInstanceWrites(Component);

    const int32 RemovedCount = PreviousCount - Component->GetInstanceCount();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    InstancedMeshUtils::WriteComponentIdentity(Data, Actor, Component);
    Data->SetNumberField(TEXT("previousCount"), PreviousCount);
    Data->SetNumberField(TEXT("requested"), Indices.Num());
    Data->SetNumberField(TEXT("removed"), RemovedCount);
    Data->SetBoolField(TEXT("all"), bAll);
    Data->SetArrayField(TEXT("removedInstances"), RemovedRows);
    InstanceRpcAddRecordNotes(Data, Component, bUndoable, true);
    AddMarkDirtySaveReport(Data, Component, /*bSaveRequested*/ true);

    if (RemovedCount != Indices.Num())
    {
        Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("'%s' went from %d to %d instance(s), but %d removal(s) were "
                                 "requested. removedInstances[] holds what was meant to go; "
                                 "editor.undo reverts the call when undoable is true."),
                *Component->GetName(), PreviousCount, Component->GetInstanceCount(),
                Indices.Num()),
            Data);
        return true;
    }
    Ctx.SendSuccess(Data);
    return true;
}
