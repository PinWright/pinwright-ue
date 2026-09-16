// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Audit/AuditFramework.h" // the shared check-table / finding / verdict contract

// Terrain SHAPE metrics for landscape.audit_shape - does a height-change boundary curve, or
// does it follow the heightfield's own cell lattice?
//
// Pure math over a row-major uint16 height buffer (the same buffer landscape.get_heights
// reads), so every threshold is unit-testable without an ALandscape.
namespace LandscapeShape
{
    // ---- Checks -------------------------------------------------------------------------
    enum class ECheck : uint8
    {
        AxisLocked = 0,
        SteppedProfile,
        Count
    };

    constexpr int32 CheckCount = static_cast<int32>(ECheck::Count);
    static_assert(CheckCount <= 32, "The check selection bitmask is a uint32.");

    // Severity, finding status and the verdict come from the one shared audit contract
    // (Audit/AuditFramework.h). Aliased rather than redeclared so LandscapeShape::ESeverity
    // stays the spelling at every call site while there is only one definition of the type.
    using ESeverity = PinWrightAudit::ESeverity;
    using EFindingStatus = PinWrightAudit::EFindingStatus;

    struct FCheckInfo
    {
        ECheck Check;
        const TCHAR* Id;
        const TCHAR* Code;
        ESeverity Severity;
        bool bDefaultOn;
        const TCHAR* Summary;
    };

    const TArray<FCheckInfo>& AllChecks();
    inline const FCheckInfo& CheckInfo(ECheck Check) { return PinWrightAudit::CheckInfo(AllChecks(), Check); }
    // False for an unknown id, which the handler turns into INVALID_ARGUMENT rather than a
    // silent skip. Now trims surrounding whitespace, matching level.audit and
    // geometry.audit_static_meshes; it previously did not, so " axis_locked " was rejected here
    // and accepted there.
    inline bool ParseCheckId(const FString& Id, ECheck& OutCheck)
    {
        return PinWrightAudit::ParseCheckId(AllChecks(), Id, OutCheck);
    }

    inline uint32 CheckBit(ECheck Check) { return PinWrightAudit::CheckBit(Check); }
    inline bool HasCheck(uint32 Mask, ECheck Check) { return PinWrightAudit::HasCheck(Mask, Check); }
    inline uint32 DefaultCheckMask() { return PinWrightAudit::DefaultCheckMask(AllChecks()); }

    // ---- Parameters ---------------------------------------------------------------------
    struct FParams
    {
        int32 Stride = 8;
        double EpsDeg = 5.0;
        int32 MarginCells = 0;
        int32 BandCount = 16;
        int32 BandHeight = 0;
        int32 MinChords = 24;
        int32 MinStepUnits = 8;
        double MaxAxisFraction = 0.50;
        double MaxStepFraction = 0.60;
    };

    // ---- Measurement --------------------------------------------------------------------
    struct FMeasurement
    {
        int32 SizeX = 0;
        int32 SizeY = 0;
        int32 MinHeight = 0;
        int32 MaxHeight = 0;
        int32 BandHeight = 0;
        int32 OccupiedBands = 0;
        int32 Edges = 0;
        int32 EdgesSkippedMargin = 0;
        int32 Paths = 0;
        int32 Chords = 0;
        TOptional<double> AxisFraction;
        TArray<int32> AngleHistogram;
        double TotalRise = 0.0;
        double SteppedRise = 0.0;
        int32 IsolatedSteps = 0;
        int32 SubThresholdSteps = 0;
        TOptional<double> StepFraction;
    };

    void Measure(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY,
                 const FParams& Params, FMeasurement& Out);

    // ---- Findings -----------------------------------------------------------------------
    struct FFinding
    {
        ECheck Check = ECheck::AxisLocked;
        EFindingStatus Status = EFindingStatus::Flagged;
        ESeverity Severity = ESeverity::Error;
        FString Code;
        FString Message;
    };

    void Evaluate(const FMeasurement& M, const FParams& Params, uint32 SelectedChecks,
                  TArray<FFinding>& OutFindings);
}
