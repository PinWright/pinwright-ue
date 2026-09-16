// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveFingerprint.h"  // FDriveDiff

class FJsonObject;

// JSON serialization for the drive contract. Output uses snake_case field names
// to match the plugin's wire convention. The write helpers build the structured
// result a handler returns; the parse helpers read the args a handler receives.
class FDriveJson
{
public:
    // FDriveObservation -> JSON result object.
    static TSharedPtr<FJsonObject> WriteObservation(const FDriveObservation& Observation);

    // FDriveElement -> JSON object. Exposed so action/wait responses can serialize a
    // single element (e.g. wait_for's `matched`) without rebuilding a whole observation.
    static TSharedPtr<FJsonObject> WriteElement(const FDriveElement& Element);

    // FDriveJournalDelta -> JSON object (also embedded in an observation).
    static TSharedPtr<FJsonObject> WriteJournalDelta(const FDriveJournalDelta& Delta);

    // Full per-handle diff: { appeared, disappeared, changed } as complete handle lists.
    // Verbose on rich HUDs; use WriteDiffSummary for the compact default and reserve this
    // for the opt-in full_diff path.
    static TSharedPtr<FJsonObject> WriteDiffFull(const FDriveDiff& Diff);

    // Compact diff: { appeared_count, disappeared_count, changed_count, appeared_sample,
    // disappeared_sample, changed_sample, omitted } where each *_sample carries at most the
    // first 15 handles and `omitted` is true when any category exceeded that cap. This is
    // the default diff form embedded in action results.
    static TSharedPtr<FJsonObject> WriteDiffSummary(const FDriveDiff& Diff);

    // Parses a condition from a handler's args object. Returns false when a `type`
    // field is present but unrecognized (or the object is null); missing optional
    // fields fall back to FDriveCondition defaults.
    static bool ParseCondition(const TSharedPtr<FJsonObject>& Json, FDriveCondition& OutCondition);

    // Parses a settle config from a handler's args object. Missing fields keep the
    // FDriveSettleConfig defaults. Returns false only when a `wait_for` sub-object is
    // present but fails to parse as a condition.
    static bool ParseSettleConfig(const TSharedPtr<FJsonObject>& Json, FDriveSettleConfig& OutConfig);

    // Enum <-> wire-string converters (snake_case). The From* variants are
    // case-insensitive and return false on an unrecognized token.
    static FString SurfaceToString(EDriveSurface Surface);
    static bool SurfaceFromString(const FString& Token, EDriveSurface& OutSurface);

    static FString ConditionTypeToString(EDriveConditionType Type);
    static bool ConditionTypeFromString(const FString& Token, EDriveConditionType& OutType);

    static FString CompareOpToString(EDriveCompareOp Op);
    static bool CompareOpFromString(const FString& Token, EDriveCompareOp& OutOp);
};
