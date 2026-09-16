// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwSource/PwSkeletonRef.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/PathUtils.h"

namespace
{
    void PwSkeletonRef_AddError(TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code,
                                int32 Line, int32 Column, FString Message)
    {
        if (Code != nullptr)
        {
            Diagnostics.Add(FPwDiagnostic::MakeError(Code, Line, Column, MoveTemp(Message)));
        }
    }
}

PwSkeletonRef::FResult PwSkeletonRef::Resolve(
    const FPwUse& Use, const FDiagnosticCodes& Codes, TArray<FPwDiagnostic>& OutDiagnostics)
{
    FResult Result;
    Result.AuthoredPath = Use.Path;

    // A missing code is a programmer/configuration error in a format caller, not an authoring
    // condition.  Do not manufacture a shared PWSRC code for it; the format must own all four
    // resolver diagnostics.
    ensureMsgf(Codes.IsComplete(),
        TEXT("PwSkeletonRef::Resolve requires a complete format diagnostic code set"));

    const FString AuthoredPath = Use.Path.TrimStartAndEnd();
    const FString SanitizedPath = SanitizeProjectRelativePath(AuthoredPath);
    FString ObjectPath;
    FString PathError;
    if (SanitizedPath.IsEmpty() || !NormalizeToObjectPath(SanitizedPath, ObjectPath, PathError))
    {
        if (PathError.IsEmpty())
        {
            PathError = TEXT("the path is not under a registered asset mount");
        }

        PwSkeletonRef_AddError(OutDiagnostics, Codes.NotAnAssetPath, Use.Line, Use.Column,
            FString::Printf(
                TEXT("Skeleton reference path \"%s\" is not a mounted asset path: %s. "
                     "Reference the compiled USkeleton object, not a source file."),
                *Use.Path, *PathError));
        return Result;
    }

    Result.ObjectPath = ObjectPath;
    UObject* LoadedObject = LoadObject<UObject>(nullptr, *ObjectPath, nullptr,
        LOAD_NoWarn | LOAD_Quiet);
    if (LoadedObject == nullptr)
    {
        PwSkeletonRef_AddError(OutDiagnostics, Codes.NotFound, Use.Line, Use.Column,
            FString::Printf(
                TEXT("Skeleton asset \"%s\" (normalized to \"%s\") was not found."),
                *Use.Path, *ObjectPath));
        return Result;
    }

    USkeleton* Skeleton = Cast<USkeleton>(LoadedObject);
    if (Skeleton == nullptr)
    {
        FString Message = FString::Printf(
            TEXT("Skeleton reference \"%s\" resolves to %s, not a USkeleton."),
            *Use.Path, *LoadedObject->GetClass()->GetName());

        // A skeletal mesh is the common accidental target.  Include its actual skeleton path
        // when one exists so the diagnostic gives a directly usable correction.
        if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(LoadedObject))
        {
            if (const USkeleton* MeshSkeleton = SkeletalMesh->GetSkeleton())
            {
                Message += FString::Printf(
                    TEXT(" Use its skeleton asset \"%s\" instead."),
                    *MeshSkeleton->GetPathName());
            }
            else
            {
                Message += TEXT(" The skeletal mesh has no skeleton to use.");
            }
        }

        PwSkeletonRef_AddError(OutDiagnostics, Codes.WrongKind, Use.Line, Use.Column,
            MoveTemp(Message));
        return Result;
    }

    if (Skeleton->GetReferenceSkeleton().GetRawBoneNum() <= 0)
    {
        PwSkeletonRef_AddError(OutDiagnostics, Codes.HasNoBones, Use.Line, Use.Column,
            FString::Printf(
                TEXT("Skeleton asset \"%s\" resolves to a USkeleton with no bones."),
                *Skeleton->GetPathName()));
        return Result;
    }

    Result.Skeleton = Skeleton;
    Result.ObjectPath = Skeleton->GetPathName();
    return Result;
}
