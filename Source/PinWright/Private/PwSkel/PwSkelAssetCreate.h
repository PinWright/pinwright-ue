// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSkelAssetCreate.h - value-in/value-out creation of a USkeleton from a parsed .pwskel AST.
#pragma once

#include "CoreMinimal.h"

#include "PwSkel/PwSkelAst.h"
#include "PwSource/PwDiagnostic.h"
#include "Utils/AssetSaveState.h"

class USkeleton;

// Everything needed to create one skeleton asset.  The source compiler supplies the parsed
// document separately so this seam never reads source text, JSON, or an RPC handler context.
struct FSkeletonCreateSpec
{
    // Long package path, for example /Game/Characters/SK_Hero.  The leaf becomes the asset name.
    FString AssetPath;

    // Permission for an unstamped or differently stamped occupant, and for any rebuild that
    // would discard state named by the recompile/takeover guard.
    bool bOverwrite = false;

    // Empty means that no source provenance stamp is written.  An empty stamp must never match
    // another empty stamp, so it cannot grant permission to update an occupied path.
    FString SourcePath;
    FString SourceHash;

    // False leaves the generated package dirty for a later explicit flush.
    bool bSave = true;

#if WITH_DEV_AUTOMATION_TESTS
    // Failure-direction seam: lets a test disturb the finished UObject immediately before the
    // production post-condition. Shipping callers cannot supply this hook.
    TFunction<void(USkeleton*)> BeforePostConditionForTest;
#endif
};

struct FSkeletonCreateResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;

    USkeleton* Asset = nullptr;
    FString AssetPath;
    FString PackageName;
    bool bUpdatedInPlace = false;
    int32 BoneCount = 0;

    // Parse and recompile-state diagnostics. A permitted destructive overwrite can succeed and
    // still carry PWSRC_RECOMPILE_UNMANAGED_STATE as a warning.
    TArray<FPwDiagnostic> Diagnostics;

    bool bSavedToDisk = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    bool bPendingFlush = false;
    int64 SizeBytes = 0;
};

// Create or rebuild the USkeleton described by Document.  Document must already have been
// parsed by FPwSkelParser; this function owns only the UObject, provenance, registry, and save
// boundary.
PINWRIGHT_API FSkeletonCreateResult CreateSkeleton(
    const FPwSkelDocument& Document, const FSkeletonCreateSpec& Spec);

using FPwSkeletonCreateSpec = FSkeletonCreateSpec;
using FPwSkeletonCreateResult = FSkeletonCreateResult;
