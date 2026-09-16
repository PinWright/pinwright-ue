// Copyright (c) 2026 Alexander Penkin. MIT License.

// geometry.audit_skeletal_animation_floating - evaluate the actual animation pose and look
// for component islands that separate from the largest component. This is deliberately a
// separate verb from geometry.audit_static_meshes: bind-pose floaters belong to the model, while
// a component that only separates while the sequence plays belongs to the animation.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/MeshAuditUtils.h"
#include "Utils/PathUtils.h"

#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"

namespace
{
    bool RequireAnimationAuditAssetPath(const FHandlerContext& Ctx, const TCHAR* Key,
                                        FString& OutPath)
    {
        FString RawPath;
        if (!Ctx.RequireString(Key, RawPath))
        {
            return false;
        }

        const FString SanitizedPath = SanitizeProjectRelativePath(RawPath);
        if (SanitizedPath.IsEmpty() || !IsValidAssetPath(SanitizedPath))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(
                    TEXT("Invalid asset path for field '%s': %s. Expected a mounted content "
                          "path such as /Game/Folder/Asset."),
                    Key, *RawPath));
            return false;
        }

        OutPath = SanitizedPath;
        return true;
    }
}

TSharedPtr<FJsonObject> MeshAudit::SerializeAnimationFloatingResult(
    const FString& MeshPath, const FString& AnimationPath,
    const FAnimationFloatingReport& Report, PinWrightAudit::EFailOn FailOn)
{
    const FCheckInfo& Check = CheckInfo(ECheck::FloatingComponents);
    int32 ModelFloatingCount = 0;
    int32 AnimationFloatingCount = 0;
    int32 SuppressedCount = 0;
    for (const FAnimationFloatingComponent& Component : Report.Components)
    {
        if (Component.bSuppressed)
        {
            ++SuppressedCount;
            continue;
        }
        if (Component.bAlreadySeparatedAtBindPose)
        {
            ++ModelFloatingCount;
        }
        else if (Component.FirstSeparatedFrame >= 0)
        {
            ++AnimationFloatingCount;
        }
    }

    PinWrightAudit::FVerdict Verdict;
    const int32 FindingCount = ModelFloatingCount + AnimationFloatingCount;
    if (Check.Severity == PinWrightAudit::ESeverity::Error)
    {
        Verdict.ErrorCount = FindingCount;
    }
    else
    {
        Verdict.WarningCount = FindingCount;
    }
    Verdict.UnrunnableCount = Report.bMeasured ? 0 : 1;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("pass"), Verdict.DerivePass(FailOn));
    Result->SetStringField(TEXT("failOn"), PinWrightAudit::FailOnToWire(FailOn));
    Result->SetStringField(
        TEXT("passRule"),
        PinWrightAudit::PassRuleText(
            /*bIncludeTruncation=*/false,
            TEXT("Bind-pose floaters are model warnings; rows first separated by the "
                 "sequence are animation warnings.")));
    Result->SetStringField(TEXT("check"), Check.Id);
    Result->SetStringField(
        TEXT("code"), Report.UnrunnableCode.IsEmpty() ? FString(Check.Code) : Report.UnrunnableCode);
    Result->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Check.Severity));
    Result->SetStringField(TEXT("status"), Report.bMeasured ? TEXT("measured") : TEXT("unrunnable"));
    Result->SetStringField(TEXT("skeletalMeshPath"), MeshPath);
    Result->SetStringField(TEXT("animationPath"), AnimationPath);

    TSharedPtr<FJsonObject> CheckRow = MakeShared<FJsonObject>();
    CheckRow->SetStringField(TEXT("check"), Check.Id);
    CheckRow->SetBoolField(TEXT("selected"), true);
    CheckRow->SetNumberField(TEXT("applicable"), 1);
    CheckRow->SetNumberField(TEXT("notApplicable"), 0);
    CheckRow->SetNumberField(TEXT("flagged"), Report.bMeasured && FindingCount > 0 ? 1 : 0);
    CheckRow->SetNumberField(TEXT("unrunnable"), Report.bMeasured ? 0 : 1);
    CheckRow->SetNumberField(TEXT("clean"), Report.bMeasured && FindingCount == 0 ? 1 : 0);
    TArray<TSharedPtr<FJsonValue>> CheckRows;
    CheckRows.Add(MakeShared<FJsonValueObject>(CheckRow));
    Result->SetArrayField(TEXT("checks"), CheckRows);

    if (!Report.bMeasured)
    {
        Result->SetNumberField(TEXT("suppressedCount"), SuppressedCount);
        Result->SetNumberField(TEXT("unsuppressedCount"), 0);
        TArray<TSharedPtr<FJsonValue>> Allowed;
        for (const int32 ComponentIndex : Report.AllowedComponentIndices)
        {
            Allowed.Add(MakeShared<FJsonValueNumber>(ComponentIndex));
        }
        Result->SetArrayField(TEXT("allowFloatingComponents"), Allowed);
        Result->SetStringField(TEXT("unrunnableReason"), Report.UnrunnableReason);
        return Result;
    }

    Result->SetNumberField(TEXT("modelFloatingCount"), ModelFloatingCount);
    Result->SetNumberField(TEXT("animationFloatingCount"), AnimationFloatingCount);
    Result->SetNumberField(TEXT("suppressedCount"), SuppressedCount);
    Result->SetNumberField(TEXT("unsuppressedCount"), FindingCount);
    TArray<TSharedPtr<FJsonValue>> Allowed;
    for (const int32 ComponentIndex : Report.AllowedComponentIndices)
    {
        Allowed.Add(MakeShared<FJsonValueNumber>(ComponentIndex));
    }
    Result->SetArrayField(TEXT("allowFloatingComponents"), Allowed);
    Result->SetNumberField(TEXT("bindIslandCount"), Report.BindIslandCount);
    Result->SetNumberField(TEXT("totalBindProximityIslands"), Report.BindIslandCount);
    Result->SetNumberField(TEXT("nonMainBindProximityIslands"),
                           Report.BindNonMainIslandCount);
    Result->SetNumberField(TEXT("bindFloatingCount"), Report.BindFloatingCount);
    Result->SetNumberField(TEXT("floatingComponentRows"), Report.FloatingComponentRows);
    Result->SetNumberField(TEXT("uniqueAnimationSeparatedProximityIslands"),
                           Report.UniqueAnimationSeparatedIslandCount);

    TSharedPtr<FJsonObject> Sampling = MakeShared<FJsonObject>();
    Sampling->SetNumberField(TEXT("frameCount"), Report.FrameCount);
    Sampling->SetNumberField(TEXT("sampleStride"), Report.SampleStride);
    Sampling->SetNumberField(TEXT("sampledFrameCount"), Report.SampledFrameCount);
    TArray<TSharedPtr<FJsonValue>> SampledFrameIds;
    SampledFrameIds.Reserve(Report.SampledFrameIds.Num());
    for (const int32 FrameId : Report.SampledFrameIds)
    {
        SampledFrameIds.Add(MakeShared<FJsonValueNumber>(FrameId));
    }
    Sampling->SetArrayField(TEXT("sampledFrameIds"), SampledFrameIds);
    Sampling->SetBoolField(
        TEXT("complete"), Report.SampledFrameCount == Report.FrameCount + 1);
    Result->SetObjectField(TEXT("sampling"), Sampling);

    TSharedPtr<FJsonObject> Tolerance = MakeShared<FJsonObject>();
    Tolerance->SetNumberField(TEXT("fraction"), Report.ToleranceFraction);
    Tolerance->SetNumberField(TEXT("boundingSphereRadius"), Report.BoundingSphereRadius);
    Tolerance->SetNumberField(TEXT("distance"), Report.Tolerance);
    Result->SetObjectField(TEXT("tolerance"), Tolerance);

    TArray<TSharedPtr<FJsonValue>> Rows;
    Rows.Reserve(Report.Components.Num());
    for (const FAnimationFloatingComponent& Component : Report.Components)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("componentIndex"), Component.ComponentIndex);
        Row->SetNumberField(TEXT("proximityIslandId"), Component.ProximityIslandId);
        Row->SetNumberField(TEXT("triangleCount"), Component.TriangleCount);
        Row->SetNumberField(TEXT("signedVolume"), Component.SignedVolume);
        TSharedPtr<FJsonObject> Center = MakeShared<FJsonObject>();
        Center->SetNumberField(TEXT("x"), Component.Center.X);
        Center->SetNumberField(TEXT("y"), Component.Center.Y);
        Center->SetNumberField(TEXT("z"), Component.Center.Z);
        Row->SetObjectField(TEXT("center"), Center);
        Row->SetNumberField(TEXT("nearestComponentIndex"), Component.NearestComponentIndex);
        Row->SetNumberField(TEXT("firstSeparation"), Component.FirstSeparation);
        Row->SetNumberField(TEXT("firstSeparatedFrame"), Component.FirstSeparatedFrame);
        Row->SetNumberField(TEXT("worstFrame"), Component.WorstFrame);
        Row->SetNumberField(TEXT("worstSeparation"), Component.WorstSeparation);
        Row->SetBoolField(TEXT("alreadySeparatedAtBindPose"),
                          Component.bAlreadySeparatedAtBindPose);
        Row->SetBoolField(TEXT("suppressed"), Component.bSuppressed);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Result->SetArrayField(TEXT("components"), Rows);
    return Result;
}

REGISTER_RPC_HANDLER("geometry.audit_skeletal_animation_floating", "geometry",
    "Evaluate a skeletal mesh in the actual frames of an animation sequence and report the "
    "component islands that separate from the largest component. The check uses the same "
    "edge-connected component walk and triangle-distance tolerance as static mesh auditing; "
    "a bind-pose floater is attributed to the model and is not repeated as an animation event. "
    "Rows are edge-connected components outside the largest proximity island, so bindFloatingCount "
    "can exceed bindIslandCount when one floating island has multiple rows. "
    "The response names total and non-main bind islands, floating component rows, and unique "
    "animation-separated islands; each row carries a stable proximityIslandId. "
    "The job is scheduled because full per-vertex skinning can be expensive. A call without "
    "args is documentation mode. Executable buffered JSON, especially args.wait:false, returns "
    "a small running ticket for system.job_status polling. With progressToken and Accept: "
    "text/event-stream, wait absent or true intentionally blocks until the terminal full result; "
    "that SSE result is not the ticket response.",
    RPC_PARAMS(
        RPC_PARAM_REQ("skeletalMeshPath", "path",
            "Content path to a USkeletalMesh, e.g. /Game/Characters/SK_Hero."),
        RPC_PARAM_REQ("animationPath", "path",
            "Content path to the UAnimSequence that actually plays on the mesh."),
        RPC_PARAM_DEF("sampleStride", "integer",
            "Evaluate every frame by default. A stride of N evaluates frames 0, N, 2N and "
            "always includes the final frame; the response reports the actual sample count. "
            "Must be at least 1.", "1"),
        RPC_PARAM_DEF("floatingToleranceFraction", "number",
            "Dimensionless fraction of the bind-pose bounding-sphere radius used as the "
            "triangle-distance link tolerance. Default 0.005; no project-unit distance is "
            "hardcoded.", "0.005"),
        RPC_PARAM_DEF("allowFloatingComponents", "array",
            "Optional bind-pose component indices that are intentionally separate. Their rows "
            "and measured distances remain in the response, but they do not count as warnings. "
            "Use component indices from this audit response.", "[]"),
        RPC_PARAM_DEF("failOn", "string",
            "Severity that makes pass false: error | any | none. The finding itself is a "
            "warning, so the default error bar reports it without failing the call.", "error")
    ))
{
    FString SkeletalMeshPath;
    if (!RequireAnimationAuditAssetPath(Ctx, TEXT("skeletalMeshPath"), SkeletalMeshPath))
    {
        return true;
    }
    FString AnimationPath;
    if (!RequireAnimationAuditAssetPath(Ctx, TEXT("animationPath"), AnimationPath))
    {
        return true;
    }

    const int32 SampleStride = Ctx.GetInt(TEXT("sampleStride"), 1);
    if (SampleStride < 1)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                      TEXT("sampleStride must be at least 1; it is a frame stride, not a "
                           "boolean switch."));
        return true;
    }

    const double ToleranceFraction = Ctx.GetNumber(
        TEXT("floatingToleranceFraction"), MeshAudit::DefaultFloatingToleranceFraction);
    if (!FMath::IsFinite(ToleranceFraction) || ToleranceFraction < 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                      TEXT("floatingToleranceFraction must be a finite, non-negative number."));
        return true;
    }

    PinWrightAudit::EFailOn FailOn;
    const FString FailOnToken = Ctx.GetString(TEXT("failOn"), TEXT("error"));
    if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOn))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                      TEXT("Unknown failOn. Valid values are error, any, and none."));
        return true;
    }

    TSet<int32> AllowedComponents;
    if (Ctx.GetRawPayload()->HasField(TEXT("allowFloatingComponents")))
    {
        const TArray<TSharedPtr<FJsonValue>>* AllowedArray =
            Ctx.GetArray(TEXT("allowFloatingComponents"));
        if (!AllowedArray)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                          TEXT("allowFloatingComponents must be an array of non-negative integer component indices."));
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Value : *AllowedArray)
        {
            double RawIndex = 0.0;
            if (!Value.IsValid() || Value->Type != EJson::Number
                || !Value->TryGetNumber(RawIndex)
                || !FMath::IsFinite(RawIndex) || RawIndex < 0.0
                || RawIndex > static_cast<double>(MAX_int32)
                || FMath::FloorToDouble(RawIndex) != RawIndex)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                              TEXT("allowFloatingComponents must contain only non-negative integer component indices."));
                return true;
            }
            AllowedComponents.Add(static_cast<int32>(RawIndex));
        }
    }

    TArray<int32> AllowedComponentIndices = AllowedComponents.Array();
    AllowedComponentIndices.Sort();

    auto SendUnrunnableAssetResult = [&Ctx, &SkeletalMeshPath, &AnimationPath, FailOn,
                                      &AllowedComponentIndices](const FString& Code,
                                                                 const FString& Reason)
    {
        MeshAudit::FAnimationFloatingReport Report;
        Report.UnrunnableCode = Code;
        Report.UnrunnableReason = Reason;
        Report.AllowedComponentIndices = AllowedComponentIndices;
        Ctx.SendSuccess(MeshAudit::SerializeAnimationFloatingResult(
            SkeletalMeshPath, AnimationPath, Report, FailOn));
    };

    USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(
        StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *SkeletalMeshPath));
    if (!SkeletalMesh)
    {
        SendUnrunnableAssetResult(
            ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Could not load a USkeletalMesh at '%s'."), *SkeletalMeshPath));
        return true;
    }
    UAnimSequence* Sequence = Cast<UAnimSequence>(
        StaticLoadObject(UAnimSequence::StaticClass(), nullptr, *AnimationPath));
    if (!Sequence)
    {
        SendUnrunnableAssetResult(
            ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Could not load a UAnimSequence at '%s'."), *AnimationPath));
        return true;
    }
    USkeleton* MeshSkeleton = SkeletalMesh->GetSkeleton();
    USkeleton* AnimationSkeleton = Sequence->GetSkeleton();
    if (!MeshSkeleton || !AnimationSkeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                      TEXT("Both skeletalMeshPath and animationPath must reference assets with "
                           "a skeleton; the audit is rejected before a job is created."));
        return true;
    }
    if (MeshSkeleton != AnimationSkeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                      TEXT("skeletalMeshPath and animationPath reference different skeleton "
                           "assets; the animation cannot be evaluated on this mesh."));
        return true;
    }

    TSharedPtr<FJsonObject> Started = MakeShared<FJsonObject>();
    Started->SetStringField(TEXT("skeletalMeshPath"), SkeletalMeshPath);
    Started->SetStringField(TEXT("animationPath"), AnimationPath);
    Started->SetNumberField(TEXT("sampleStride"), SampleStride);
    TArray<TSharedPtr<FJsonValue>> StartedAllowed;
    for (const int32 ComponentIndex : AllowedComponentIndices)
    {
        StartedAllowed.Add(MakeShared<FJsonValueNumber>(ComponentIndex));
    }
    Started->SetArrayField(TEXT("allowFloatingComponents"), StartedAllowed);

    FJobBindArgs Args;
    Args.Method = TEXT("geometry.audit_skeletal_animation_floating");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [SkeletalMesh, Sequence, SkeletalMeshPath, AnimationPath, SampleStride, ToleranceFraction,
         FailOn, AllowedComponents, AllowedComponentIndices](FJobOnComplete OnComplete)
    {
        MeshAudit::FAnimationFloatingReport Report;
        MeshAudit::MeasureSkeletalAnimationFloating(
            SkeletalMesh, Sequence, SampleStride, ToleranceFraction, Report);
        Report.AllowedComponentIndices = AllowedComponentIndices;
        for (MeshAudit::FAnimationFloatingComponent& Component : Report.Components)
        {
            Component.bSuppressed = AllowedComponents.Contains(Component.ComponentIndex);
        }
        for (const MeshAudit::FAnimationFloatingComponent& Component : Report.Components)
        {
            if (Component.bSuppressed)
            {
                ++Report.SuppressedCount;
            }
            else
            {
                ++Report.UnsuppressedCount;
            }
        }
        // A failure to obtain editor source data is an unanswered audit, not a clean animation.
        // It is represented in the success payload so the shared verdict can make pass false;
        // malformed arguments were rejected before this job was created.
        TSharedPtr<FJsonObject> Result = MeshAudit::SerializeAnimationFloatingResult(
            SkeletalMeshPath, AnimationPath, Report, FailOn);
        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}
