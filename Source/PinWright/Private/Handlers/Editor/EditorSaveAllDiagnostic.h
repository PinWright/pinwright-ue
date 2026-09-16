// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UPackage;

namespace EditorSaveAllDiagnostic
{


/**
 * Classifies why a package failed to save.
 * Returns "BlockedByPie" when PIE is active (PIE holds asset locks),
 * "ReadOnly" when the on-disk file is write-protected,
 * "Unknown" otherwise.
 */
FString ClassifyFailureReason(UPackage* Package, bool bPieActive);

/**
 * Saves every dirty world/content package after rejecting Blueprints that fail
 * the same integrity gate used by compile-time save paths.
 */
PINWRIGHT_API bool SaveDirtyPackagesWithIntegrityGate(
    TSharedPtr<FJsonObject>& OutResult,
    FString& OutErrorMessage);

/**
 * Builds the enriched save-all result JSON and fills OutErrorMessage with a
 * PIE-aware description when there are failures.
 */
PINWRIGHT_API TSharedPtr<FJsonObject> BuildSaveAllResultJson(
    bool bSuccess,
    int32 SavedCount,
    int32 TotalDirty,
    bool bPieActive,
    const TArray<TPair<FString, FString>>& FailedAssetsWithReasons,
    FString& OutErrorMessage);


} // namespace EditorSaveAllDiagnostic
