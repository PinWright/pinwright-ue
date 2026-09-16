// Copyright (c) 2026 Alexander Penkin. MIT License.

// ScalabilityDensityCVars.h - measured-vs-requested reporting for the two density cvars the
// sg.FoliageQuality scalability group drives.
//
// BaseScalability.ini's [FoliageQuality@N] blocks write BOTH grass.densityScale and
// foliage.DensityScale together (0 at @0, 0.4 at @1, 0.8 at @2, 1.0 at @3/@Cine), so one
// scalability setting silently changes what every vegetation number a verb reports MEANS. The two
// cvars are not interchangeable and this header keeps them apart:
//
//   grass.densityScale (Runtime/Landscape/Private/LandscapeGrass.cpp:130-135, ECVF_Scalability)
//     multiplies the variety's density at grass-build time -
//     GrassDensity = GrassVariety.GetDensity() * DensityScale (:2040-2041), UNCLAMPED - and the
//     per-type opt-out ULandscapeGrassType::bEnableDensityScaling defaults TRUE (:1567). So a
//     grass type created with engine defaults IS scaled, always.
//
//   foliage.DensityScale (Runtime/Engine/Private/HierarchicalInstancedStaticMesh.cpp:135-140,
//     ECVF_Scalability) is a render-time cull instead: UpdateDensityScaling clamps it to [0,1]
//     (:3071-3076) into CurrentDensityScaling, which FClusterBuilder uses to drop a random
//     fraction of instances from the HISM cluster tree (:346, DensityRand.GetFraction() >
//     DensityScaling). The instances stay in PerInstanceSMData; they are simply never drawn. Its
//     gate UFoliageType::bEnableDensityScaling defaults FALSE (InstancedFoliage.cpp:669), so this
//     half fires only on a type somebody opted in.
//
// Reporting rules follow Handlers/Environment/LightingHandler.cpp, the landed reference for this
// class of defect: measure through IConsoleManager at response time, publish the effective value
// beside the requested one, OMIT rather than zero when the registry does not carry the cvar, and
// warn naming the remedy. Never set the cvar - a scalability cvar is global editor state a verb
// must not change silently.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"

namespace PinWrightDensityScalability
{
    // The names as REGISTERED. grass.densityScale really is spelled with a lower-case 'd';
    // console-registry keys compare case-insensitively, but the registered spelling is what a
    // caller should paste into system.console_command.
    inline constexpr const TCHAR* GrassDensityScaleCVarName = TEXT("grass.densityScale");
    inline constexpr const TCHAR* FoliageDensityScaleCVarName = TEXT("foliage.DensityScale");

    // A cvar read that keeps "not measured" distinguishable from "measured zero". Value is only
    // meaningful when bFound.
    struct FDensityScaleReading
    {
        bool bFound = false;
        float Value = 1.0f;
    };

    inline FDensityScaleReading ReadDensityScaleCVar(const TCHAR* CVarName)
    {
        FDensityScaleReading Reading;
        if (const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(CVarName))
        {
            Reading.bFound = true;
            Reading.Value = CVar->GetFloat();
        }
        return Reading;
    }

    inline TSharedPtr<FJsonObject> MakeDensityScaleCVarJson(const TCHAR* CVarName,
        const FDensityScaleReading& Reading)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("cvar"), CVarName);
        Obj->SetBoolField(TEXT("found"), Reading.bFound);
        // Absent, not 1.0, when the registry did not carry it: an unmeasured value must not be
        // readable as a measured one.
        if (Reading.bFound)
        {
            Obj->SetNumberField(TEXT("value"), Reading.Value);
        }
        return Obj;
    }

    // The multiplier the grass builder will apply. Returns false when it cannot be established -
    // the type opts in but the registry did not carry the cvar - so the caller omits the
    // effective figure instead of publishing an unmeasured one. Deliberately UNCLAMPED: the grass
    // builder multiplies the raw cvar value.
    inline bool ResolveGrassDensityScale(const FDensityScaleReading& Reading, bool bTypeOptedIn,
        float& OutScale)
    {
        if (!bTypeOptedIn)
        {
            // Measured knowledge, not an assumption: the opt-out is read off the asset, and with
            // it clear the cvar cannot reach this grass type at any value.
            OutScale = 1.0f;
            return true;
        }
        if (!Reading.bFound)
        {
            return false;
        }
        OutScale = Reading.Value;
        return true;
    }

    // Same contract for the foliage side, with the engine's own [0,1] clamp
    // (HierarchicalInstancedStaticMesh.cpp:3076) applied so the reported scale is the one
    // FClusterBuilder actually receives.
    inline bool ResolveFoliageDensityScale(const FDensityScaleReading& Reading, bool bTypeOptedIn,
        float& OutScale)
    {
        if (!bTypeOptedIn)
        {
            OutScale = 1.0f;
            return true;
        }
        if (!Reading.bFound)
        {
            return false;
        }
        OutScale = FMath::Clamp(Reading.Value, 0.0f, 1.0f);
        return true;
    }

    // What a foliage response knows about the instances in its scope. Filled by the caller
    // because the three foliage verbs reach their counts differently (one resolved type for
    // paint / add_instances, a walk over every FFoliageInfo for get_instances).
    struct FFoliageDensityScope
    {
        FDensityScaleReading Reading;
        // Whether ANY instance in scope belongs to a type with bEnableDensityScaling set.
        bool bAnyTypeOptedIn = false;
        // False when the expected drawn count could not be established: the cvar is absent while
        // something in scope opts in, or an instance's owning type is unknown (an orphaned info).
        bool bMeasured = false;
        // What the response's own counter says - the editor-side ledger.
        int32 LedgerCount = 0;
        // Sum over the scope of round(N * effective scale). Meaningless unless bMeasured.
        int32 ExpectedDrawn = 0;
        // The effective scale. Meaningless unless bMeasured.
        float EffectiveScale = 1.0f;
        // False when the scope spans types under DIFFERENT scales (an unfiltered read over a
        // level holding both opted-in and exempt foliage). effectiveDensityScale is then omitted
        // rather than published as one of the two, while expectedDrawnInstances - summed
        // per-type - stays exact.
        bool bEffectiveScaleUniform = true;
    };

    // The single-type case, which is paint and add_instances: one resolved UFoliageType, so one
    // opt-in flag and one scale over the whole ledger.
    inline FFoliageDensityScope MakeSingleTypeFoliageDensityScope(bool bTypeOptedIn, int32 LedgerCount)
    {
        FFoliageDensityScope Scope;
        Scope.Reading = ReadDensityScaleCVar(FoliageDensityScaleCVarName);
        Scope.bAnyTypeOptedIn = bTypeOptedIn;
        Scope.LedgerCount = LedgerCount;
        Scope.bMeasured =
            ResolveFoliageDensityScale(Scope.Reading, bTypeOptedIn, Scope.EffectiveScale);
        if (Scope.bMeasured)
        {
            Scope.ExpectedDrawn = FMath::RoundToInt(LedgerCount * Scope.EffectiveScale);
        }
        return Scope;
    }

    // Publishes the render-density block onto a foliage response. LedgerFieldName is the counter
    // the response already carries (instancesPlaced, instances_count, count, ...) so the warning
    // can name the field the caller is about to misread.
    //
    // expectedDrawnInstances is an EXPECTATION, not a count: FClusterBuilder's exclusion is a
    // per-instance random draw against the scale, so it is exact only at 1 (all drawn) and 0
    // (none drawn) and an expected value in between. It is omitted entirely when unmeasured.
    inline void AddFoliageDensityScaleReport(const TSharedPtr<FJsonObject>& Resp,
        const FFoliageDensityScope& Scope, const TCHAR* LedgerFieldName)
    {
        if (!Resp.IsValid())
        {
            return;
        }

        Resp->SetObjectField(TEXT("foliageDensityScaleCVar"),
            MakeDensityScaleCVarJson(FoliageDensityScaleCVarName, Scope.Reading));
        Resp->SetBoolField(TEXT("densityScalingEnabled"), Scope.bAnyTypeOptedIn);

        if (Scope.bMeasured)
        {
            if (Scope.bEffectiveScaleUniform)
            {
                Resp->SetNumberField(TEXT("effectiveDensityScale"), Scope.EffectiveScale);
            }
            Resp->SetNumberField(TEXT("expectedDrawnInstances"), Scope.ExpectedDrawn);
        }

        // Nothing to warn about when nothing in scope is subject to the cull: the ledger count IS
        // what renders, which is what the response already said.
        if (!Scope.bAnyTypeOptedIn)
        {
            return;
        }

        if (!Scope.bMeasured)
        {
            Resp->SetStringField(TEXT("cvarWarning"), FString::Printf(
                TEXT("%s is the editor-side ledger count. Foliage in this scope opts in to density ")
                TEXT("scaling (UFoliageType::bEnableDensityScaling), but foliage.DensityScale could ")
                TEXT("not be measured on this host, so how many of these instances the renderer ")
                TEXT("will actually draw is UNKNOWN. effectiveDensityScale and ")
                TEXT("expectedDrawnInstances are omitted rather than guessed."),
                LedgerFieldName));
            return;
        }

        if (Scope.EffectiveScale < 1.0f)
        {
            Resp->SetStringField(TEXT("cvarWarning"), FString::Printf(
                TEXT("%s is %d, the editor-side ledger count - NOT what the renderer draws. ")
                TEXT("foliage.DensityScale is %.4g and foliage in this scope opts in to density ")
                TEXT("scaling, so UHierarchicalInstancedStaticMeshComponent excludes a random ")
                TEXT("fraction of these instances from its cluster tree before the frame is ")
                TEXT("built: about %d of them ")
                TEXT("are expected to be drawn%s. The instances are stored correctly and every ")
                TEXT("transform is intact; only the number that reaches the frame differs. ")
                TEXT("BaseScalability.ini sets foliage.DensityScale from sg.FoliageQuality (0 at @0, ")
                TEXT("0.4 at @1, 0.8 at @2, 1.0 at @3), so raise it for this session with ")
                TEXT("system.console_command \"sg.FoliageQuality 3\", or pin foliage.DensityScale=1 ")
                TEXT("under [SystemSettings] in the project's DefaultEngine.ini. Clearing ")
                TEXT("bEnableDensityScaling on the foliage type exempts it instead. This verb does ")
                TEXT("not change the cvar."),
                LedgerFieldName, Scope.LedgerCount, Scope.Reading.Value, Scope.ExpectedDrawn,
                Scope.EffectiveScale <= 0.0f
                    ? TEXT(" - at a scale of 0 that is NONE of them")
                    : TEXT("")));
        }
    }
}
