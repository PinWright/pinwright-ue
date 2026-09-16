// Copyright (c) 2026 Alexander Penkin. MIT License.

// AdvancedMeshOpsHandler.cpp - Advanced mesh operations: bridge, loft, sweep, extrude_along_spline,
//   duplicate_along_spline, edge_split (Phase 19)
//
// The mesh work of every verb here except duplicate_along_spline lives in
// GeometryOps_Advanced.h; what remains is the actor -> data adapter. loft turns its profile
// actors into locations + bounding-box extents, sweep and extrude_along_spline sample their
// USplineComponent into a uniform frame list, and the ops never learn any of that came from a
// level. duplicate_along_spline is not an op at all - it duplicates actors and edits no mesh.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Geometry/GeometryOpWarnings.h"
#include "Handlers/Geometry/GeometryOps_Advanced.h"
#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "PinWrightHelpers.h"
#include "Dom/JsonObject.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/SplineComponent.h"
#include "DynamicMeshActor.h"
#include "UDynamicMesh.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Subsystems/EditorActorSubsystem.h"

#include "GeometryScript/MeshQueryFunctions.h"

// File-local spline sampler, uniquely named for the Unity/ODR reason spelled out in
// GeometryTarget.h.
namespace
{
    // Uniform location+rotation samples along a spline, in world space: the data half of a
    // USplineComponent, and the whole of what geometry.sweep and geometry.extrude_along_spline
    // need off the level. Twist and scale are deliberately NOT applied - the ops apply those so
    // the spline path and the linear fallback cannot drift apart.
    TArray<FTransform> SampleSplineFrames(USplineComponent* SplineComp, int32 StepCount, float SplineLength)
    {
        TArray<FTransform> Samples;
        Samples.Reserve(StepCount + 1);
        for (int32 i = 0; i <= StepCount; ++i)
        {
            const float Alpha = (float)i / StepCount;
            const float Dist = SplineLength * Alpha;
            Samples.Add(FTransform(
                SplineComp->GetQuaternionAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World),
                SplineComp->GetLocationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World)));
        }
        return Samples;
    }
}


// ============================================================================
// bridge
// ============================================================================
// `subdivisions` was DECLARED here and consumed nowhere. It reached no engine call, no
// FBridgeParams field and no branch of the strip builder, which emits exactly one quad per pair
// of loop vertices whatever it is set to - so every value an author passed produced the identical
// mesh, and the response echoed the number back as if it had meant something. A parameter that is
// accepted and silently discarded is worse than one that does not exist: it costs the caller a
// full authoring cycle to discover it does nothing. It is removed from the declared surface here
// and from the .pwmodel op table, which follows the same removal `recalculate_normals`'
// `split_angle` already took.
//
// It could not be WIRED instead. bridge is hand-rolled over FDynamicMesh3::AppendTriangle rather
// than wrapping an engine call, so there is no engine option to route it to; giving it real
// behaviour means generating intermediate interpolated rings between the two loops, which is a
// new feature (and an ill-defined one when the loops have different vertex counts), not a repair.
//
// The RESPONSE still carries `subdivisions`. Removing an echoed field is a response change and
// this deliberately is not one - see the note at the SetNumberField below.
REGISTER_RPC_HANDLER("geometry.bridge", "geometry", "Bridge between boundary loops of a dynamic mesh with a single triangle strip",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("edgeGroupA", "integer", "Index of first boundary loop (default 0)"),
        RPC_PARAM_OPT("edgeGroupB", "integer", "Index of second boundary loop (default 1)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    GeometryOps::FBridgeParams Params;
    Params.EdgeGroupA = Ctx.GetInt(TEXT("edgeGroupA"), 0);
    Params.EdgeGroupB = Ctx.GetInt(TEXT("edgeGroupB"), 1);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FBridgeOutputs Outputs;
    const GeometryOps::FOpResult Op = GeometryOps::Bridge(Target.Mesh, Params, Outputs);
    if (!Op.bSuccess)
    {
        Ctx.SendError(*Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("edgeGroupA"), Params.EdgeGroupA);
    Result->SetNumberField(TEXT("edgeGroupB"), Params.EdgeGroupB);
    // The one field here that is not a report of what happened. It is kept because ~146
    // dispatcher tests assert on response SHAPE and dropping a field is an observable break;
    // the parameter behind it is gone, so it now echoes the constant it always effectively was.
    Result->SetNumberField(TEXT("subdivisions"), 1);
    Result->SetStringField(TEXT("bridgeStatus"), Outputs.Status);
    Result->SetNumberField(TEXT("trianglesCreated"), Outputs.TrianglesCreated);
    Result->SetNumberField(TEXT("trianglesBefore"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("trianglesAfter"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// loft
// ============================================================================
REGISTER_RPC_HANDLER("geometry.loft", "geometry", "Loft a surface between cross-section profiles",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the target DynamicMeshActor"),
        RPC_PARAM_OPT("profileActors", "array", "Array of actor names to use as cross-section profiles"),
        RPC_PARAM_OPT("subdivisions", "integer", "Number of subdivisions along loft (default 8)"),
        RPC_PARAM_OPT("smooth", "boolean", "Apply smooth normals (default true)"),
        RPC_PARAM_OPT("cap", "boolean", "Cap the ends (default true)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    GeometryOps::FLoftParams Params;
    Params.Subdivisions = Ctx.GetInt(TEXT("subdivisions"), 8);
    Params.bSmooth = Ctx.GetBool(TEXT("smooth"), true);
    Params.bCap = Ctx.GetBool(TEXT("cap"), true);

    // Get profile actor names if provided
    auto Payload = Ctx.GetRawPayload();
    TArray<FString> ProfileActors;
    if (Payload->HasField(TEXT("profileActors")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Profiles = Payload->GetArrayField(TEXT("profileActors"));
        for (const auto& Profile : Profiles)
        {
            ProfileActors.Add(Profile->AsString());
        }
    }

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

    Params.bUseProfiles = ProfileActors.Num() > 0 && World != nullptr;

    // The actor -> data adapter: a profile contributes its world location and, for the FIRST
    // resolved profile only, the extent of its mesh's bounding box.
    //
    // GeometryOps::Loft reads Extent off Profiles[0] and nowhere else (its ProfileExtent is the
    // sole use), while Location and bHasMesh are read on the first and last entries and Num() on
    // the whole array - so every entry must still be appended, and only the first must pay for
    // the bounding box. GetMeshBoundingBox is an O(V) walk of the profile's mesh; filling it for
    // all N profiles walked N-1 meshes whose result was discarded, and the code this replaced
    // computed exactly one. Re-check this against GeometryOps_Advanced.cpp before adding a field.
    TArray<GeometryOps::FLoftProfileSample> ProfileSamples;
    if (Params.bUseProfiles)
    {
        for (const FString& ProfileName : ProfileActors)
        {
            const McpActorUtils::FActorResolution ProfileResolution =
                GeometryTarget::ResolveMeshActor(World, ProfileName);
            if (ProfileResolution.IsAmbiguous())
            {
                ActorNameParamUtils::SendAmbiguousActorError(Ctx, ProfileName, ProfileResolution);
                return true;
            }

            ADynamicMeshActor* Profile = ProfileResolution.IsResolved()
                ? Cast<ADynamicMeshActor>(ProfileResolution.Actor)
                : nullptr;
            if (!Profile)
            {
                continue;
            }

            GeometryOps::FLoftProfileSample Sample;
            Sample.Location = Profile->GetActorLocation();
            if (UDynamicMeshComponent* ProfileDMC = Profile->GetDynamicMeshComponent())
            {
                if (UDynamicMesh* ProfileMesh = ProfileDMC->GetDynamicMesh())
                {
                    Sample.bHasMesh = true;
                    if (ProfileSamples.Num() == 0)
                    {
                        Sample.Extent = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(ProfileMesh).GetExtent();
                    }
                }
            }
            ProfileSamples.Add(Sample);
        }
    }

    GeometryOps::FLoftOutputs Outputs;
    const GeometryOps::FOpResult Op = GeometryOps::Loft(Target.Mesh, Params, ProfileSamples, Outputs);
    if (!Op.bSuccess)
    {
        Ctx.SendError(*Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    Result->SetNumberField(TEXT("subdivisions"), Params.Subdivisions);
    Result->SetBoolField(TEXT("smooth"), Params.bSmooth);
    Result->SetBoolField(TEXT("cap"), Params.bCap);
    Result->SetNumberField(TEXT("profilesUsed"), Outputs.ProfilesUsed);
    Result->SetNumberField(TEXT("trianglesBefore"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("trianglesAfter"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// sweep
// ============================================================================
REGISTER_RPC_HANDLER("geometry.sweep", "geometry", "Sweep a cross-section profile along a path or spline",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("splineActorName", "string", "Name of actor with spline component to sweep along"),
        RPC_PARAM_OPT("steps", "integer", "Number of steps along sweep path (default 16)"),
        RPC_PARAM_OPT("twist", "number", "Twist angle in degrees along sweep (default 0)"),
        RPC_PARAM_OPT("scaleStart", "number", "Scale at start of sweep (default 1.0)"),
        RPC_PARAM_OPT("scaleEnd", "number", "Scale at end of sweep (default 1.0)"),
        RPC_PARAM_OPT("cap", "boolean", "Cap the ends (default true)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString SplineActorName = Ctx.GetString(TEXT("splineActorName"));

    GeometryOps::FSweepParams Params;
    Params.Steps = Ctx.GetInt(TEXT("steps"), 16);
    Params.Twist = Ctx.GetNumber(TEXT("twist"), 0.0);
    Params.ScaleStart = Ctx.GetNumber(TEXT("scaleStart"), 1.0);
    Params.ScaleEnd = Ctx.GetNumber(TEXT("scaleEnd"), 1.0);
    Params.bCap = Ctx.GetBool(TEXT("cap"), true);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    AActor* SplineActor = nullptr;
    if (!SplineActorName.IsEmpty())
    {
        const McpActorUtils::FActorResolution SplineResolution =
            GeometryTarget::ResolveAnyActor(World, SplineActorName);
        if (SplineResolution.IsAmbiguous())
        {
            ActorNameParamUtils::SendAmbiguousActorError(Ctx, SplineActorName, SplineResolution);
            return true;
        }
        SplineActor = SplineResolution.IsResolved() ? SplineResolution.Actor : nullptr;
    }

    // The actor -> data adapter: sample the spline into a frame list, or record that the named
    // actor carries no spline so the op can report the wording that case has always used.
    GeometryOps::FSweepPath Path;
    if (SplineActor)
    {
        if (USplineComponent* SplineComp = SplineActor->FindComponentByClass<USplineComponent>())
        {
            Path.SplineLength = SplineComp->GetSplineLength();
            Path.Samples = SampleSplineFrames(SplineComp,
                GeometryOps::SplinePathStepCount(Params.Steps), Path.SplineLength);
        }
        else
        {
            Path.bActorHasNoSplineComponent = true;
        }
    }

    GeometryOps::FSweepOutputs Outputs;
    const GeometryOps::FOpResult Op = GeometryOps::Sweep(Target.Mesh, Params, Path, Outputs);
    if (!Op.bSuccess)
    {
        Ctx.SendError(*Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, Target.Actor);
    if (!SplineActorName.IsEmpty())
    {
        Result->SetStringField(TEXT("splineActorName"), SplineActorName);
        GeometryUtils::AddResolvedActorIdentity(Result, SplineActor, TEXT("spline"));
        Result->SetNumberField(TEXT("splineLength"), Path.SplineLength);
    }
    Result->SetStringField(TEXT("sweepStatus"), Outputs.Status);
    Result->SetNumberField(TEXT("pathSteps"), Outputs.PathSteps);
    Result->SetNumberField(TEXT("profileVertices"), Outputs.ProfileVertices);
    Result->SetNumberField(TEXT("steps"), Params.Steps);
    Result->SetNumberField(TEXT("twist"), Params.Twist);
    Result->SetNumberField(TEXT("scaleStart"), Params.ScaleStart);
    Result->SetNumberField(TEXT("scaleEnd"), Params.ScaleEnd);
    Result->SetBoolField(TEXT("cap"), Params.bCap);
    Result->SetNumberField(TEXT("trianglesBefore"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("trianglesAfter"), Op.TrianglesAfter);
    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// duplicate_along_spline
// ============================================================================
REGISTER_RPC_HANDLER("geometry.duplicate_along_spline", "geometry", "Duplicate an actor along a spline path",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the source DynamicMeshActor to duplicate"),
        RPC_PARAM_REQ("splineActorName", "string", "Name of actor with spline component"),
        RPC_PARAM_OPT("count", "integer", "Number of duplicates (default 10)"),
        RPC_PARAM_OPT("alignToSpline", "boolean", "Align duplicates to spline direction (default true)"),
        RPC_PARAM_OPT("scaleVariation", "number", "Random scale variation range (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString SplineActorName = Ctx.GetString(TEXT("splineActorName"));
    int32 Count = Ctx.GetInt(TEXT("count"), 10);
    bool bAlignToSpline = Ctx.GetBool(TEXT("alignToSpline"), true);
    double ScaleVariation = Ctx.GetNumber(TEXT("scaleVariation"), 0.0);

    if (ActorName.IsEmpty() || SplineActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName and splineActorName required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available"));
        return true;
    }

    // Both are looked up before reporting so the source-actor failure is checked first even
    // when the spline identifier is ambiguous.
    const McpActorUtils::FActorResolution SourceResolution =
        GeometryTarget::ResolveMeshActor(World, ActorName);
    const McpActorUtils::FActorResolution SplineResolution =
        GeometryTarget::ResolveAnyActor(World, SplineActorName);

    if (SourceResolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, ActorName, SourceResolution);
        return true;
    }
    ADynamicMeshActor* SourceActor = SourceResolution.IsResolved()
        ? Cast<ADynamicMeshActor>(SourceResolution.Actor)
        : nullptr;

    if (!SourceActor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("Source actor not found: %s"), *ActorName));
        return true;
    }

    if (SplineResolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, SplineActorName, SplineResolution);
        return true;
    }

    AActor* SplineActor = SplineResolution.IsResolved() ? SplineResolution.Actor : nullptr;

    if (!SplineActor)
    {
        Ctx.SendError(TEXT("SPLINE_NOT_FOUND"), FString::Printf(TEXT("Spline actor not found: %s"), *SplineActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineActor->FindComponentByClass<USplineComponent>();
    if (!SplineComp)
    {
        Ctx.SendError(TEXT("SPLINE_COMPONENT_NOT_FOUND"), TEXT("Actor does not have a spline component"));
        return true;
    }

    float SplineLength = SplineComp->GetSplineLength();
    TArray<FString> CreatedActors;

    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (!ActorSS)
    {
        Ctx.SendError(TEXT("EDITOR_SUBSYSTEM_MISSING"), TEXT("EditorActorSubsystem unavailable"));
        return true;
    }

    // Base template scale is loop-invariant: read the source's own scale once and
    // multiply the per-duplicate variation onto it, so a non-uniform template keeps
    // its proportions instead of collapsing to an absolute uniform scale built from
    // 1.0. See TestGeometryDuplicateAlongSplineScaleVariation for the full rationale.
    const FVector SourceScale = SourceActor->GetActorScale3D();

    for (int32 i = 0; i < Count; ++i)
    {
        float Distance = SplineLength * ((float)i / FMath::Max(Count - 1, 1));
        FVector Location = SplineComp->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
        FRotator Rotation = bAlignToSpline ? SplineComp->GetRotationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World) : FRotator::ZeroRotator;

        AActor* NewActor = ActorSS->DuplicateActor(SourceActor, World);
        if (NewActor)
        {
            NewActor->SetActorLocation(Location);
            NewActor->SetActorRotation(Rotation);

            if (ScaleVariation > 0.0)
            {
                double ScaleFactor = 1.0 + FMath::RandRange(-ScaleVariation, ScaleVariation);
                NewActor->SetActorScale3D(SourceScale * ScaleFactor);
            }

            FString NewName = FString::Printf(TEXT("%s_Dup%d"), *ActorName, i);
            NewActor->SetActorLabel(NewName);
            CreatedActors.Add(NewName);

            // UEditorActorSubsystem::DuplicateActors opens its own FScopedTransaction
            // (EditorActorSubsystem.cpp:2646), so the duplication itself already dirtied
            // the level — but the transform and label writes above land AFTER that
            // transaction closes. Cheap and idempotent; keeps the whole verb covered.
            GeometryUtils::MarkGeometryActorSpawned(NewActor);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sourceActor"), ActorName);
    Result->SetStringField(TEXT("splineActor"), SplineActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, SourceActor, TEXT("source"));
    GeometryUtils::AddResolvedActorIdentity(Result, SplineActor, TEXT("spline"));
    Result->SetNumberField(TEXT("count"), Count);
    Result->SetNumberField(TEXT("splineLength"), SplineLength);
    Result->SetBoolField(TEXT("alignToSpline"), bAlignToSpline);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// extrude_along_spline
// ============================================================================
REGISTER_RPC_HANDLER("geometry.extrude_along_spline", "geometry", "Extrude a profile along a spline path",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_REQ("splineActorName", "string", "Name of actor with spline component"),
        RPC_PARAM_OPT("segments", "integer", "Number of segments along extrusion (default 16)"),
        RPC_PARAM_OPT("cap", "boolean", "Cap the ends (default true)"),
        RPC_PARAM_OPT("scaleStart", "number", "Scale at start (default 1.0)"),
        RPC_PARAM_OPT("scaleEnd", "number", "Scale at end (default 1.0)"),
        RPC_PARAM_OPT("twist", "number", "Twist angle in degrees (default 0)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));
    FString SplineActorName = Ctx.GetString(TEXT("splineActorName"));

    GeometryOps::FExtrudeAlongSplineParams Params;
    Params.Segments = Ctx.GetInt(TEXT("segments"), 16);
    Params.bCap = Ctx.GetBool(TEXT("cap"), true);
    Params.ScaleStart = Ctx.GetNumber(TEXT("scaleStart"), 1.0);
    Params.ScaleEnd = Ctx.GetNumber(TEXT("scaleEnd"), 1.0);
    Params.Twist = Ctx.GetNumber(TEXT("twist"), 0.0);

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
        return true;
    }

    if (SplineActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("splineActorName required"));
        return true;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available"));
        return true;
    }

    // Both are looked up before reporting so the target-actor failure is checked first even
    // when the spline identifier is ambiguous. Deliberately NOT
    // GeometryTarget::ResolveOrSendError: that reports the target before the spline is even
    // looked up, and this verb's error ordering and message wording are its own.
    const McpActorUtils::FActorResolution TargetResolution =
        GeometryTarget::ResolveMeshActor(World, ActorName);
    const McpActorUtils::FActorResolution SplineResolution =
        GeometryTarget::ResolveAnyActor(World, SplineActorName);

    if (TargetResolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, ActorName, TargetResolution);
        return true;
    }

    ADynamicMeshActor* TargetActor = TargetResolution.IsResolved()
        ? Cast<ADynamicMeshActor>(TargetResolution.Actor)
        : nullptr;

    if (!TargetActor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    if (SplineResolution.IsAmbiguous())
    {
        ActorNameParamUtils::SendAmbiguousActorError(Ctx, SplineActorName, SplineResolution);
        return true;
    }

    AActor* SplineActor = SplineResolution.IsResolved() ? SplineResolution.Actor : nullptr;

    if (!SplineActor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("Spline actor not found: %s"), *SplineActorName));
        return true;
    }

    USplineComponent* SplineComp = SplineActor->FindComponentByClass<USplineComponent>();
    if (!SplineComp)
    {
        Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("Spline actor has no USplineComponent"));
        return true;
    }

    UDynamicMeshComponent* DMC = TargetActor->GetDynamicMeshComponent();
    if (!DMC || !DMC->GetDynamicMesh())
    {
        Ctx.SendError(TEXT("MESH_NOT_FOUND"), TEXT("DynamicMesh not available"));
        return true;
    }

    const float SplineLength = SplineComp->GetSplineLength();
    const TArray<FTransform> PathSamples = SampleSplineFrames(SplineComp,
        GeometryOps::SplinePathStepCount(Params.Segments), SplineLength);

    const GeometryOps::FOpResult Op =
        GeometryOps::ExtrudeAlongSpline(DMC->GetDynamicMesh(), Params, PathSamples);
    if (!Op.bSuccess)
    {
        Ctx.SendError(*Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(DMC);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetStringField(TEXT("splineActorName"), SplineActorName);
    Result->SetNumberField(TEXT("splineLength"), SplineLength);
    Result->SetNumberField(TEXT("segments"), Params.Segments);
    Result->SetNumberField(TEXT("trianglesBefore"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("trianglesAfter"), Op.TrianglesAfter);

    // Add verification data for the target actor. Keep actorName as the request echo and add
    // canonical identities separately so path/name requests are not rewritten as labels.
    AddActorVerification(Result, TargetActor);
    Result->SetStringField(TEXT("actorName"), ActorName);
    GeometryUtils::AddResolvedActorIdentity(Result, TargetActor, TEXT("target"));
    GeometryUtils::AddResolvedActorIdentity(Result, SplineActor, TEXT("spline"));

    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// edge_split
// ============================================================================
REGISTER_RPC_HANDLER("geometry.edge_split", "geometry", "Split edges of a dynamic mesh by inserting midpoint vertices",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the DynamicMeshActor"),
        RPC_PARAM_OPT("edges", "array", "Array of edge IDs to split"),
        RPC_PARAM_OPT("edgeIndex", "integer", "Single edge ID to split (alternative to edges array)"),
        RPC_PARAM_OPT("splitFactor", "number", "Position along edge to split 0.0-1.0 (default 0.5)"),
        RPC_PARAM_OPT("weldVertices", "boolean", "Weld vertices after split (default true)"),
        RPC_PARAM_OPT("weldTolerance", "number", "Weld distance tolerance (default 0.0001)")
    ))
{
    FString ActorName = Ctx.GetString(TEXT("actorName"));

    GeometryOps::FEdgeSplitParams Params;

    // Parse edge indices to split (can be array or single number)
    auto Payload = Ctx.GetRawPayload();
    const TArray<TSharedPtr<FJsonValue>>* EdgeArray = nullptr;
    if (Payload->TryGetArrayField(TEXT("edges"), EdgeArray))
    {
        for (const auto& Val : *EdgeArray)
        {
            if (Val.IsValid() && Val->Type == EJson::Number)
            {
                Params.EdgeIndices.Add(static_cast<int32>(Val->AsNumber()));
            }
        }
    }
    else
    {
        int32 EdgeIndex = Ctx.GetInt(TEXT("edgeIndex"), -1);
        if (EdgeIndex >= 0)
        {
            Params.EdgeIndices.Add(EdgeIndex);
        }
    }

    Params.SplitFactor = Ctx.GetNumber(TEXT("splitFactor"), 0.5);
    Params.bWeldVertices = Ctx.GetBool(TEXT("weldVertices"), true);
    Params.WeldTolerance = Ctx.GetNumber(TEXT("weldTolerance"), 0.0001);

    FGeometryTarget Target;
    if (!GeometryTarget::ResolveOrSendError(Ctx, ActorName, Target))
        return true;

    GeometryOps::FEdgeSplitOutputs Outputs;
    const GeometryOps::FOpResult Op = GeometryOps::EdgeSplit(Target.Mesh, Params, Outputs);
    if (!Op.bSuccess)
    {
        Ctx.SendError(*Op.ErrorCode, Op.ErrorMessage);
        return true;
    }

    GeometryUtils::MarkGeometryActorModified(Target.Component);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actorName"), ActorName);
    Result->SetNumberField(TEXT("edgesSplit"), Outputs.EdgesSplit);
    Result->SetNumberField(TEXT("trianglesBefore"), Op.TrianglesBefore);
    Result->SetNumberField(TEXT("trianglesAfter"), Op.TrianglesAfter);

    AddActorVerification(Result, Target.Actor);
    Result->SetStringField(TEXT("actorName"), ActorName);

    GeometryOps::AddOpWarnings(Result, Op);
    Ctx.SendSuccess(Result);
    return true;
}
