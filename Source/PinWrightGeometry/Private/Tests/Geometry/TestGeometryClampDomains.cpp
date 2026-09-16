// Copyright (c) 2026 Alexander Penkin. MIT License.

// The gate that keeps a clamped domain from drifting away from the clamp that produces it.
//
// Three things claim to know a count's floor and ceiling, and until this file only one of them
// was ever right by construction:
//
//   1. The CALL SITE in GeometryOps_Primitives.cpp - two integer literals in a clamp call. This
//      is the behaviour; everything else is a description of it.
//   2. The DECLARED DOMAIN in GeometryClampDomains.h - added so the domain is a structure rather
//      than a literal, and so a front-end can publish it.
//   3. The PUBLISHED DESCRIPTION in the .pwmodel parameter table, which is the vocabulary an
//      authoring agent reads and the only place most authors will ever see these numbers.
//
// (3) was written by reading (1) by hand, once. Nothing re-read it afterwards. This file makes
// (1) -> (2) a MEASUREMENT and (2) -> (3) an assertion, so a call site that changes without its
// row, or a row without its description, fails here rather than silently misinforming an author.
//
// The measurement half deliberately does not read the source. It RUNS each generator at its
// floor, one below, at its ceiling, one above and at zero, and checks the warnings the op
// actually emits. A test that parsed the literals out of the .cpp would agree with a wrong number
// as happily as with a right one; a test that asks the mesh cannot.
//
// WHY THE DOMAIN IS NOT THE PARAMETER TABLE'S ENFORCED min/max, restated here because the naive
// fix is the one a future reader will reach for: that pair REFUSES a value outside it. These
// bounds ACCEPT a value outside them and move it. Declaring a radial count as an enforced 3..256
// would convert every clamp into a rejection and destroy the documented second tier where 0 or
// less means "unset" and takes the verb's default - a legal, useful value that is outside the
// clamped domain by construction. The assertion that no clamped parameter declares an enforced
// range is therefore not a tidiness check; it is the guard against that specific regression.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryClampDomains.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"

#include "Model/PwModelParser.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Uniquely prefixed: bUseUnity = true merges test TUs, so a plainly-named helper collides.

// The value a clamp reports moving to, or this sentinel when no clamp for that label fired.
constexpr int32 ClampDomainsTest_NoClamp = MIN_int32;

// Reads the "to" value out of either warning shape:
//   "<label> clamped from <V> to <C>"
//   "<label> clamped from <V> to <C> (valid range <A>-<B>)"
// The label match is anchored on the full prefix, so "width clamped from" cannot match a
// "widthSegments clamped from" warning.
int32 ClampDomainsTest_ClampedTo(const TArray<FString>& Warnings, const TCHAR* Label)
{
    const FString Prefix = FString::Printf(TEXT("%s clamped from "), Label);
    const FString Marker = TEXT(" to ");
    int32 Result = ClampDomainsTest_NoClamp;

    for (const FString& Warning : Warnings)
    {
        if (!Warning.StartsWith(Prefix, ESearchCase::CaseSensitive))
        {
            continue;
        }

        FString Tail = Warning.RightChop(Prefix.Len());

        // Drop any parenthesised suffix before looking for the last " to ", so the digits inside
        // "(valid range 3-256)" cannot be mistaken for the clamped value.
        int32 ParenIndex = INDEX_NONE;
        if (Tail.FindChar(TEXT('('), ParenIndex))
        {
            Tail = Tail.Left(ParenIndex).TrimEnd();
        }

        const int32 Split = Tail.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (Split != INDEX_NONE)
        {
            Result = FCString::Atoi(*Tail.RightChop(Split + Marker.Len()));
        }
    }
    return Result;
}

// Runs one generator with ONE named count set to Value and everything else at its own default,
// returning the warnings.
//
// One parameter at a time rather than all of them, for a reason that shows up at the ceiling: a
// box with all three step counts at 256 is a million-triangle mesh built purely to read a warning
// string, and the sphere is worse. Driving the parameter under test alone keeps every probe cheap
// except the one op whose only count IS the expensive one.
//
// SweepDegrees overrides the op's own angle when > 0, which is how the two conditional rows
// (arch majorSteps, revolve steps) get both of their floors measured.
//
// bOutDriven reports whether this method/label pair was recognised, so a row with no driver is a
// named failure rather than an unmeasured pass.
TArray<FString> ClampDomainsTest_Run(
    const FString& RpcMethod, const FString& Label, int32 Value, double SweepDegrees,
    bool& bOutDriven)
{
    bOutDriven = true;
    TStrongObjectPtr<UDynamicMesh> Mesh(NewObject<UDynamicMesh>(GetTransientPackage()));
    const FTransform Identity = FTransform::Identity;

    if (RpcMethod == TEXT("geometry.create_box"))
    {
        GeometryOps::FBoxParams Params;
        if (Label == TEXT("widthSegments"))       { Params.Steps.X = Value; }
        else if (Label == TEXT("heightSegments")) { Params.Steps.Y = Value; }
        else if (Label == TEXT("depthSegments"))  { Params.Steps.Z = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateBox(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_sphere"))
    {
        GeometryOps::FSphereParams Params;
        if (Label == TEXT("subdivisions")) { Params.Subdivisions = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateSphere(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_cylinder"))
    {
        GeometryOps::FCylinderParams Params;
        if (Label == TEXT("segments"))         { Params.RadialSteps = Value; }
        else if (Label == TEXT("heightSteps")) { Params.HeightSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateCylinder(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_cone"))
    {
        GeometryOps::FConeParams Params;
        if (Label == TEXT("segments"))         { Params.RadialSteps = Value; }
        else if (Label == TEXT("heightSteps")) { Params.HeightSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateCone(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_capsule"))
    {
        GeometryOps::FCapsuleParams Params;
        if (Label == TEXT("hemisphereSteps")) { Params.HemisphereSteps = Value; }
        else if (Label == TEXT("segments"))   { Params.RadialSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateCapsule(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_torus"))
    {
        GeometryOps::FTorusParams Params;
        if (Label == TEXT("majorSegments"))        { Params.MajorSteps = Value; }
        else if (Label == TEXT("minorSegments"))   { Params.MinorSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateTorus(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_plane"))
    {
        GeometryOps::FPlaneParams Params;
        if (Label == TEXT("widthSubdivisions"))      { Params.Steps.X = Value; }
        else if (Label == TEXT("depthSubdivisions")) { Params.Steps.Y = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GeneratePlane(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_disc"))
    {
        GeometryOps::FDiscParams Params;
        if (Label == TEXT("segments")) { Params.AngleSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateDisc(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_stairs"))
    {
        GeometryOps::FStairsParams Params;
        if (Label == TEXT("numSteps")) { Params.NumSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateStairs(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_spiral_stairs"))
    {
        GeometryOps::FSpiralStairsParams Params;
        if (Label == TEXT("numSteps")) { Params.NumSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateSpiralStairs(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_ring"))
    {
        GeometryOps::FRingParams Params;
        if (Label == TEXT("segments")) { Params.AngleSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GenerateRing(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_arch"))
    {
        GeometryOps::FArchParams Params;
        if (Label == TEXT("majorSteps"))      { Params.MajorSteps = Value; }
        else if (Label == TEXT("minorSteps")) { Params.MinorSteps = Value; }
        else { bOutDriven = false; return {}; }
        if (SweepDegrees > 0.0)
        {
            Params.Angle = SweepDegrees;
        }
        return GeometryOps::GenerateArch(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.create_pipe"))
    {
        GeometryOps::FPipeParams Params;
        if (Label == TEXT("radialSteps"))      { Params.RadialSteps = Value; }
        else if (Label == TEXT("heightSteps")) { Params.HeightSteps = Value; }
        else { bOutDriven = false; return {}; }
        return GeometryOps::GeneratePipe(Mesh.Get(), Params, Identity).Warnings;
    }
    if (RpcMethod == TEXT("geometry.revolve"))
    {
        GeometryOps::FRevolveParams Params;
        if (Label == TEXT("steps")) { Params.Steps = Value; }
        else { bOutDriven = false; return {}; }
        if (SweepDegrees > 0.0)
        {
            Params.Angle = SweepDegrees;
        }
        // Three profile points, so the op measures the step clamp rather than emitting its
        // profile-substitution warning. The shape is irrelevant to the clamp.
        Params.Profile = { FVector2D(10.0, 0.0), FVector2D(20.0, 0.0), FVector2D(20.0, 40.0) };
        return GeometryOps::GenerateRevolve(Mesh.Get(), Params, Identity).Warnings;
    }

    bOutDriven = false;
    return {};
}

// RPC method -> the .pwmodel op that publishes the same generator. The mapping lives HERE and not
// in GeometryClampDomains.h on purpose: the ops layer stays ignorant of its callers' spellings
// (the same rule PwModelWarningNames exists to serve), so the layer that knows both is the test.
const TCHAR* ClampDomainsTest_ModelOpName(const FString& RpcMethod)
{
    static const TMap<FString, const TCHAR*> Map = {
        { TEXT("geometry.create_box"),            TEXT("box") },
        { TEXT("geometry.create_sphere"),         TEXT("sphere") },
        { TEXT("geometry.create_cylinder"),       TEXT("cylinder") },
        { TEXT("geometry.create_cone"),           TEXT("cone") },
        { TEXT("geometry.create_capsule"),        TEXT("capsule") },
        { TEXT("geometry.create_torus"),          TEXT("torus") },
        { TEXT("geometry.create_plane"),          TEXT("plane") },
        { TEXT("geometry.create_disc"),           TEXT("disc") },
        { TEXT("geometry.create_stairs"),         TEXT("stairs") },
        { TEXT("geometry.create_spiral_stairs"),  TEXT("spiral_stairs") },
        { TEXT("geometry.create_ring"),           TEXT("ring") },
        { TEXT("geometry.create_arch"),           TEXT("arch") },
        { TEXT("geometry.create_pipe"),           TEXT("pipe") },
        { TEXT("geometry.revolve"),               TEXT("revolve") },
    };
    const TCHAR* const* Found = Map.Find(RpcMethod);
    return Found ? *Found : nullptr;
}

// The .pwmodel PARAMETER a warning label lands on. PwModelWarningNames maps the RPC label to the
// document's WARNING label, which for a vector-valued parameter carries a component suffix
// ("segments.x") that the parameter itself does not have - so the suffix is dropped.
FString ClampDomainsTest_ModelParamName(const TCHAR* ModelOp, const TCHAR* RpcLabel)
{
    FString Name = RpcLabel;
    for (const PwModelWarningNames::FEntry& Entry : PwModelWarningNames::Entries())
    {
        if (FCString::Strcmp(Entry.OpName, ModelOp) == 0
            && FCString::Strcmp(Entry.RpcLabel, RpcLabel) == 0)
        {
            Name = Entry.ModelLabel;
            break;
        }
    }
    int32 Dot = INDEX_NONE;
    if (Name.FindChar(TEXT('.'), Dot))
    {
        Name = Name.Left(Dot);
    }
    return Name;
}
}

// ============================================================================
// (1) -> (2): the declared domain is what the op actually does
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryClampDomainsMeasuredTest,
    "PinWright.Geometry.ClampDomains.EveryDeclaredDomainIsWhatTheOpActuallyDoes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryClampDomainsMeasuredTest::RunTest(const FString& Parameters)
{
    const TArrayView<const GeometryOps::ClampDomains::FClampDomain> Table =
        GeometryOps::ClampDomains::Entries();

    TestTrue(TEXT("the clamp-domain table is populated"), Table.Num() > 0);

    for (const GeometryOps::ClampDomains::FClampDomain& Domain : Table)
    {
        const FString Method = Domain.RpcMethod;
        const FString LabelStr = Domain.Label;
        const FString Where = FString::Printf(TEXT("%s %s"), Domain.RpcMethod, Domain.Label);

        auto Probe = [&](int32 Value, double Sweep, bool& bOutDriven) -> int32
        {
            const TArray<FString> Warnings =
                ClampDomainsTest_Run(Method, LabelStr, Value, Sweep, bOutDriven);
            return ClampDomainsTest_ClampedTo(Warnings, Domain.Label);
        };

        // Driver check on the cheapest probe there is. Doing it at the ceiling instead would
        // build one more maximum-subdivision mesh per row for no extra information.
        bool bDriven = false;
        Probe(Domain.ClampMinOpenSweep, 0.0, bDriven);
        if (!TestTrue(FString::Printf(
                TEXT("%s: the test has a driver for this method and label"), *Where), bDriven))
        {
            continue;
        }

        // Both floors are measured, each with the sweep angle that selects it. On the
        // twenty-one non-conditional rows these two passes are the same measurement twice, which
        // costs nothing and keeps the loop free of a special case.
        struct FFloorCase { int32 Floor; double Sweep; };
        const FFloorCase FloorCases[] = {
            { Domain.ClampMinOpenSweep,   Domain.IsSweepConditional() ? 180.0 : 0.0 },
            { Domain.ClampMinClosedSweep, Domain.IsSweepConditional() ? 360.0 : 0.0 },
        };

        for (const FFloorCase& Case : FloorCases)
        {
            // At the floor: legal, so nothing is clamped.
            TestEqual(FString::Printf(TEXT("%s: %d is legal and is not clamped"), *Where, Case.Floor),
                Probe(Case.Floor, Case.Sweep, bDriven), ClampDomainsTest_NoClamp);

            // One below the floor: raised to the floor, and reported.
            //
            // Skipped only where "one below the floor" is not above zero, because zero is the
            // unset tier and is asserted separately below. That is exactly the two step-count
            // rows, whose floor is 1.
            const int32 BelowFloor = (Case.Floor > 1) ? Case.Floor - 1 : -1;
            if (Case.Floor > 1 || !Domain.bZeroMeansUnset)
            {
                TestEqual(FString::Printf(TEXT("%s: %d is raised to the floor %d"),
                        *Where, BelowFloor, Case.Floor),
                    Probe(BelowFloor, Case.Sweep, bDriven), Case.Floor);
            }
        }

        // At the ceiling: legal.
        TestEqual(FString::Printf(TEXT("%s: the ceiling %d is legal"), *Where, Domain.ClampMax),
            Probe(Domain.ClampMax, 0.0, bDriven), ClampDomainsTest_NoClamp);

        // One above it: lowered to the ceiling.
        TestEqual(FString::Printf(TEXT("%s: %d is lowered to the ceiling %d"),
                *Where, Domain.ClampMax + 1, Domain.ClampMax),
            Probe(Domain.ClampMax + 1, 0.0, bDriven), Domain.ClampMax);

        // Zero: the two tiers. Either it reads as "unset" and takes the verb's default - which is
        // the whole reason the domain cannot be published as an enforced range - or it is an
        // ordinary legal value and nothing happens.
        const int32 AtZero = Probe(0, 0.0, bDriven);
        if (Domain.bZeroMeansUnset)
        {
            TestEqual(FString::Printf(TEXT("%s: 0 reads as unset and takes the default %d"),
                    *Where, Domain.Default),
                AtZero, Domain.Default);
        }
        else
        {
            TestEqual(FString::Printf(TEXT("%s: 0 is an ordinary legal value"), *Where),
                AtZero, ClampDomainsTest_NoClamp);
        }
    }

    return true;
}

// ============================================================================
// (2) -> (3): the published description says the same numbers
// ============================================================================
//
// One row is a KNOWN GAP rather than a failure, and it is named rather than filtered silently:
// `sphere subdivisions` publishes its floor in prose ("Minimum 2") and no ceiling at all. It
// predates the convention the other twenty-two follow, and its text lives in a file this change
// could not touch. The fix is one sentence - restate it as "Clamped to 2-256 with a warning; 0 or
// less reads as unset and takes the default 16" - after which the exception below is deleted and
// the row is covered like every other. Leaving the exception is the honest form: the gap is
// visible in the source and shrinks to zero when someone closes it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryClampDomainsPublishedTest,
    "PinWright.Geometry.ClampDomains.EveryClampedCountPublishesItsDomainUnenforced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryClampDomainsPublishedTest::RunTest(const FString& Parameters)
{
    int32 KnownGaps = 0;

    for (const GeometryOps::ClampDomains::FClampDomain& Domain : GeometryOps::ClampDomains::Entries())
    {
        const FString Method = Domain.RpcMethod;
        const FString Where = FString::Printf(TEXT("%s %s"), Domain.RpcMethod, Domain.Label);

        const TCHAR* ModelOp = ClampDomainsTest_ModelOpName(Method);
        if (!TestNotNull(FString::Printf(TEXT("%s: the test maps this op to a .pwmodel op"), *Where),
                ModelOp))
        {
            continue;
        }

        const FPwModelOpSpec* OpSpec =
            PwModelOpTable::Find(FString(ModelOp), EPwModelOpContext::Part);
        if (!TestNotNull(FString::Printf(TEXT("%s: '%s' is a .pwmodel part op"), *Where, ModelOp),
                OpSpec))
        {
            continue;
        }

        const FString ParamName = ClampDomainsTest_ModelParamName(ModelOp, Domain.Label);
        const FPwModelParamSpec* ParamSpec = OpSpec->FindParam(ParamName);
        if (!TestNotNull(FString::Printf(
                    TEXT("%s: '%s' publishes a parameter '%s' for this clamped count"),
                    *Where, ModelOp, *ParamName),
                ParamSpec))
        {
            continue;
        }

        // THE LOAD-BEARING ONE. An enforced range on a clamped count turns every clamp into a
        // refusal and makes the unset tier unwritable. It must stay absent.
        TestFalse(FString::Printf(TEXT(
                "%s: '%s.%s' must NOT declare an ENFORCED range - the clamped domain is advisory, "
                "and enforcing it would reject values this op accepts and moves"),
                *Where, ModelOp, *ParamName),
            ParamSpec->bHasRange);

        // sphere subdivisions: floor in prose, no ceiling. Counted and named, not hidden.
        if (ParamName == TEXT("subdivisions") && FCString::Strcmp(ModelOp, TEXT("sphere")) == 0)
        {
            ++KnownGaps;
            TestTrue(FString::Printf(TEXT(
                    "%s: the known-gap row still at least publishes its floor"), *Where),
                ParamSpec->Description.Contains(FString::FromInt(Domain.ClampMinOpenSweep)));
            continue;
        }

        // The domain, in the form the other twenty-two use.
        const FString OpenDomain = FString::Printf(
            TEXT("%d-%d"), Domain.ClampMinOpenSweep, Domain.ClampMax);
        TestTrue(FString::Printf(TEXT("%s: '%s.%s' publishes the clamped domain %s"),
                *Where, ModelOp, *ParamName, *OpenDomain),
            ParamSpec->Description.Contains(OpenDomain));

        // A conditional floor has to publish BOTH, or an author reading one number gets the
        // wrong one on whichever sweep they did not write.
        if (Domain.IsSweepConditional())
        {
            const FString ClosedDomain = FString::Printf(
                TEXT("%d-%d"), Domain.ClampMinClosedSweep, Domain.ClampMax);
            TestTrue(FString::Printf(TEXT(
                    "%s: '%s.%s' publishes the closed-sweep domain %s as well"),
                    *Where, ModelOp, *ParamName, *ClosedDomain),
                ParamSpec->Description.Contains(ClosedDomain));
        }

        // And the second tier, where there is one: a value of 0 or less is not an error, it is
        // "unset", and the description has to say what it becomes.
        if (Domain.bZeroMeansUnset)
        {
            TestTrue(FString::Printf(TEXT(
                    "%s: '%s.%s' says a value of 0 or less takes the default %d"),
                    *Where, ModelOp, *ParamName, Domain.Default),
                ParamSpec->Description.Contains(FString::FromInt(Domain.Default))
                    && (ParamSpec->Description.Contains(TEXT("unset"))
                        || ParamSpec->Description.Contains(TEXT("default"))));
        }
    }

    // Pinned so the exception cannot quietly grow. Closing the sphere gap makes this 0 and the
    // branch above dead; both are then deleted together.
    TestEqual(TEXT("exactly one clamped count is a known publication gap"), KnownGaps, 1);

    return true;
}
