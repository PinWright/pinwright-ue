// Copyright (c) 2026 Alexander Penkin. MIT License.

// MorphCurveAuditHandler.cpp - animation.check_morph_curves.
//
// "Will this facial clip move the face?" A name match between a curve and a morph target is not
// the answer. At runtime a curve reaches the morph-target curve map only when the bone container
// flags it MorphTarget (AnimInstanceProxy.cpp UpdateCurvesToEvaluationContext intersects the
// evaluated curves with RequiredBones->GetCurveFlags()). Those flags come from two places
// (BoneContainer.cpp CacheRequiredAnimCurves):
//   - the Skeleton's curve metadata (Type.bMorphtarget), then
//   - the mesh's UAnimCurveMetaData asset user data, which REPLACES the skeleton's entry for any
//     curve it carries a flag for (FNamedValueArrayUtils::Union assigns the mesh element).
// This verb replays exactly that precedence per curve and reports it, so a curve that matches a
// morph by name but carries no flag reads drives:false instead of passing a name-only check.
//
// Read-only asset audit on the shared contract (Audit/AuditFramework.h, rpc-design.md section 18).

#include "Audit/AuditFramework.h"
#include "Handlers/Asset/AnimSequenceDumpBuilder.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PathUtils.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/MorphTarget.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"

namespace
{
    // Float curves an asset outputs when evaluated. A montage's segments are evaluated through
    // the slot node, so their curves are the montage's too.
    bool MorphCurveAuditCollectCurves(UObject* Asset, TSet<FName>& OutCurves)
    {
        auto AddSequenceCurves = [&OutCurves](const UAnimSequenceBase* Sequence)
        {
            // Data model, not the runtime GetCurveData() copy: that copy was measured EMPTY for a
            // sequence whose model held four float curves - every curve would read as absent.
            for (const FFloatCurve& Curve : AnimSequenceDumpBuilder::GetAuthoredFloatCurves(Sequence))
            {
                OutCurves.Add(Curve.GetName());
            }
        };

        if (const UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
        {
            AddSequenceCurves(Montage);
            for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
            {
                for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
                {
                    if (const UAnimSequenceBase* Reference = Segment.GetAnimReference())
                    {
                        AddSequenceCurves(Reference);
                    }
                }
            }
            return true;
        }
        if (const UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Asset))
        {
            AddSequenceCurves(Sequence);
            return true;
        }
        if (const UPoseAsset* PoseAsset = Cast<UPoseAsset>(Asset))
        {
            OutCurves.Append(PoseAsset->GetCurveFNames());
            return true;
        }
        return false;
    }

    TSharedPtr<FJsonObject> MorphCurveAuditUnrunnable(const FString& AssetPath, const TCHAR* Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("assetPath"), AssetPath);
        Row->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(PinWrightAudit::EFindingStatus::Unrunnable));
        Row->SetStringField(TEXT("code"), Code);
        Row->SetStringField(TEXT("message"), Message);
        return Row;
    }
}

REGISTER_RPC_HANDLER("animation.check_morph_curves", "animation",
    "Audit whether animation curves will actually drive a skeletal mesh's morph targets. A curve "
    "drives a morph only when its name matches a morph on the mesh AND its curve metadata carries "
    "the MorphTarget flag - from the mesh's own curve metadata, which overrides the Skeleton's, or "
    "else from the mesh Skeleton's. A name match without the flag never moves the morph. Per asset "
    "returns curves[] {curve, morphOnMesh, flagSource: mesh|skeleton|none, maxLod, drives} and "
    "morphsWithoutCurve[] (morphs no curve in that asset drives). Findings: "
    "unflagged_morph_curve (error: matches a morph, no flag) and flagged_curve_without_morph "
    "(warning: flagged, but this mesh has no such morph). An asset that cannot be loaded or is not "
    "an AnimSequence/AnimMontage/PoseAsset is unrunnable and fails pass. Read-only: leaves every "
    "package's dirty flag unchanged.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "path|array",
            "AnimSequence, AnimMontage (its own curves plus its segments' curves) or PoseAsset "
            "paths to check. Each must be a content path; a malformed one is rejected before the "
            "sweep."),
        RPC_PARAM_REQ("skeletalMeshPath", "path",
            "Skeletal mesh the curves should drive. Required: the answer depends on the mesh's "
            "morph targets, its curve metadata and its Skeleton."),
        RPC_PARAM_DEF("failOn", "string",
            "Severity that makes pass false: 'error' (default), 'any', or 'none'. An unrunnable "
            "asset makes pass false regardless.",
            "error")
    ))
{
    const TArray<TSharedPtr<FJsonValue>>* PathValues = Ctx.GetArray(TEXT("assetPaths"));
    if (!PathValues || PathValues->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("assetPaths must be a non-empty array of animation asset paths."));
        return true;
    }

    // Malformed entries are argument errors, refused before anything is measured (section 18).
    TArray<FString> AssetPaths;
    for (int32 Index = 0; Index < PathValues->Num(); ++Index)
    {
        FString Raw;
        FString Normalized;
        FString PathError;
        if (!(*PathValues)[Index].IsValid() || !(*PathValues)[Index]->TryGetString(Raw)
            || !NormalizeToObjectPath(Raw, Normalized, PathError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("assetPaths[%d] '%s' is not a content path (expected /Game/Folder/Asset): %s"),
                    Index, *Raw, *PathError));
            return true;
        }
        AssetPaths.AddUnique(Normalized);
    }

    const FString FailOnToken = Ctx.GetString(TEXT("failOn"), TEXT("error"));
    PinWrightAudit::EFailOn FailOn;
    if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOn))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown failOn '%s'. Valid: error, any, none."), *FailOnToken));
        return true;
    }

    FString MeshPath;
    FString MeshPathError;
    if (!NormalizeToObjectPath(Ctx.GetString(TEXT("skeletalMeshPath")), MeshPath, MeshPathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, MeshPathError);
        return true;
    }
    UObject* MeshObject = PinWrightGuardedLoad::LoadObjectChecked<UObject>(MeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
    USkeletalMesh* Mesh = Cast<USkeletalMesh>(MeshObject);
    if (!Mesh)
    {
        Ctx.SendError(MeshObject ? ErrorCodes::ERR_INVALID_ASSET_TYPE : ErrorCodes::ERR_MESH_NOT_FOUND,
            MeshObject
                ? FString::Printf(TEXT("'%s' is a %s, not a SkeletalMesh."), *MeshPath, *MeshObject->GetClass()->GetName())
                : FString::Printf(TEXT("Skeletal mesh not found: %s"), *MeshPath));
        return true;
    }

    const USkeleton* Skeleton = Mesh->GetSkeleton();
    const UAnimCurveMetaData* MeshCurveMetaData = Mesh->GetAssetUserData<UAnimCurveMetaData>();

    TArray<FName> MorphNames;
    for (const UMorphTarget* Morph : Mesh->GetMorphTargets())
    {
        if (Morph)
        {
            MorphNames.Add(Morph->GetFName());
        }
    }
    MorphNames.Sort(FNameLexicalLess());

    PinWrightAudit::FVerdict Verdict;
    int32 FlaggedAssets = 0;
    int32 CleanAssets = 0;
    TArray<TSharedPtr<FJsonValue>> AssetRows;
    TArray<TSharedPtr<FJsonValue>> Findings;

    for (const FString& AssetPath : AssetPaths)
    {
        FString Refusal;
        UObject* Asset = PinWrightGuardedLoad::LoadObjectChecked<UObject>(AssetPath, &Refusal, LOAD_NoWarn | LOAD_Quiet);
        TSet<FName> CurveSet;
        TSharedPtr<FJsonObject> Unrunnable;
        if (!Asset)
        {
            Unrunnable = MorphCurveAuditUnrunnable(AssetPath, ErrorCodes::ERR_ASSET_NOT_FOUND,
                Refusal.IsEmpty() ? TEXT("No asset at this path.") : Refusal);
        }
        else if (!MorphCurveAuditCollectCurves(Asset, CurveSet))
        {
            Unrunnable = MorphCurveAuditUnrunnable(AssetPath, ErrorCodes::ERR_INVALID_ASSET_TYPE,
                FString::Printf(TEXT("'%s' is a %s; pass an AnimSequence, AnimMontage or PoseAsset."),
                    *AssetPath, *Asset->GetClass()->GetName()));
        }
        else if (!Skeleton)
        {
            Unrunnable = MorphCurveAuditUnrunnable(AssetPath, ErrorCodes::ERR_INVALID_STATE,
                FString::Printf(TEXT("Skeletal mesh '%s' has no Skeleton, so skeleton curve metadata cannot be read."), *MeshPath));
        }
        if (Unrunnable.IsValid())
        {
            ++Verdict.UnrunnableCount;
            Findings.Add(MakeShared<FJsonValueObject>(Unrunnable));
            AssetRows.Add(MakeShared<FJsonValueObject>(Unrunnable));
            continue;
        }

        TArray<FName> Curves = CurveSet.Array();
        Curves.Sort(FNameLexicalLess());

        bool bAssetFlagged = false;
        TSet<FName> DrivenMorphs;
        TArray<TSharedPtr<FJsonValue>> CurveRows;
        for (const FName& CurveName : Curves)
        {
            // BoneContainer precedence: a mesh entry carrying any flag replaces the skeleton's.
            const FCurveMetaData* MeshMeta = MeshCurveMetaData ? MeshCurveMetaData->GetCurveMetaData(CurveName) : nullptr;
            const FCurveMetaData* SkeletonMeta = Skeleton->GetCurveMetaData(CurveName);
            const bool bMeshOverrides = MeshMeta && (MeshMeta->Type.bMorphtarget || MeshMeta->Type.bMaterial);
            const bool bMorphFlag = bMeshOverrides ? MeshMeta->Type.bMorphtarget : (SkeletonMeta && SkeletonMeta->Type.bMorphtarget);
            const TCHAR* FlagSource = !bMorphFlag ? TEXT("none") : (bMeshOverrides ? TEXT("mesh") : TEXT("skeleton"));
            const bool bMorphOnMesh = MorphNames.Contains(CurveName);
            const bool bDrives = bMorphFlag && bMorphOnMesh;

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("curve"), CurveName.ToString());
            Row->SetBoolField(TEXT("morphOnMesh"), bMorphOnMesh);
            Row->SetStringField(TEXT("flagSource"), FlagSource);
            // Skeleton metadata MaxLOD: the curve is filtered out at LODs above it (255 = every LOD).
            if (SkeletonMeta)
            {
                Row->SetNumberField(TEXT("maxLod"), SkeletonMeta->MaxLOD);
            }
            else
            {
                Row->SetField(TEXT("maxLod"), MakeShared<FJsonValueNull>());
            }
            Row->SetBoolField(TEXT("drives"), bDrives);
            CurveRows.Add(MakeShared<FJsonValueObject>(Row));

            if (bDrives)
            {
                DrivenMorphs.Add(CurveName);
                continue;
            }

            const TCHAR* Check = nullptr;
            PinWrightAudit::ESeverity Severity = PinWrightAudit::ESeverity::Warning;
            FString Message;
            if (bMorphOnMesh)
            {
                Check = TEXT("unflagged_morph_curve");
                Severity = PinWrightAudit::ESeverity::Error;
                Message = FString::Printf(
                    TEXT("Curve '%s' matches a morph target on the mesh but no curve metadata flags it MorphTarget, ")
                    TEXT("so it will never move the morph. Set the MorphTarget flag on the Skeleton's (or the mesh's) curve metadata."),
                    *CurveName.ToString());
            }
            else if (bMorphFlag)
            {
                Check = TEXT("flagged_curve_without_morph");
                Message = FString::Printf(
                    TEXT("Curve '%s' is flagged MorphTarget (%s metadata) but the mesh has no morph target of that name."),
                    *CurveName.ToString(), FlagSource);
            }
            if (!Check)
            {
                continue; // an ordinary attribute/material curve: not a morph concern
            }

            bAssetFlagged = true;
            ++(Severity == PinWrightAudit::ESeverity::Error ? Verdict.ErrorCount : Verdict.WarningCount);
            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("assetPath"), AssetPath);
            Finding->SetStringField(TEXT("curve"), CurveName.ToString());
            Finding->SetStringField(TEXT("check"), Check);
            Finding->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Severity));
            Finding->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(PinWrightAudit::EFindingStatus::Flagged));
            Finding->SetStringField(TEXT("message"), Message);
            Findings.Add(MakeShared<FJsonValueObject>(Finding));
        }

        TArray<TSharedPtr<FJsonValue>> MorphsWithoutCurve;
        for (const FName& MorphName : MorphNames)
        {
            if (!DrivenMorphs.Contains(MorphName))
            {
                MorphsWithoutCurve.Add(MakeShared<FJsonValueString>(MorphName.ToString()));
            }
        }

        ++(bAssetFlagged ? FlaggedAssets : CleanAssets);
        TSharedPtr<FJsonObject> AssetRow = MakeShared<FJsonObject>();
        AssetRow->SetStringField(TEXT("assetPath"), AssetPath);
        AssetRow->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetName());
        AssetRow->SetStringField(TEXT("status"), bAssetFlagged ? TEXT("flagged") : TEXT("clean"));
        AssetRow->SetArrayField(TEXT("curves"), CurveRows);
        AssetRow->SetArrayField(TEXT("morphsWithoutCurve"), MorphsWithoutCurve);
        AssetRows.Add(MakeShared<FJsonValueObject>(AssetRow));
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("assets"), AssetPaths.Num());
    Summary->SetNumberField(TEXT("flagged"), FlaggedAssets);
    Summary->SetNumberField(TEXT("clean"), CleanAssets);
    Summary->SetNumberField(TEXT("unrunnable"), Verdict.UnrunnableCount);
    Summary->SetNumberField(TEXT("errors"), Verdict.ErrorCount);
    Summary->SetNumberField(TEXT("warnings"), Verdict.WarningCount);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("pass"), Verdict.DerivePass(FailOn));
    Result->SetStringField(TEXT("passRule"), PinWrightAudit::PassRuleText(/*bIncludeTruncation=*/false));
    Result->SetStringField(TEXT("failOn"), PinWrightAudit::FailOnToWire(FailOn));
    Result->SetStringField(TEXT("skeletalMeshPath"), MeshPath);
    Result->SetStringField(TEXT("skeletonPath"), Skeleton ? Skeleton->GetPathName() : FString());
    Result->SetNumberField(TEXT("morphTargetCount"), MorphNames.Num());
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetArrayField(TEXT("assets"), AssetRows);
    Result->SetArrayField(TEXT("findings"), Findings);
    Ctx.SendSuccess(Result);
    return true;
}
