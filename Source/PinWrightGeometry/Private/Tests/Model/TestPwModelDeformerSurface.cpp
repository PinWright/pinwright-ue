// Copyright (c) 2026 Alexander Penkin. MIT License.

// The .pwmodel surface for parameters the ops layer already has: the warp frame (`axis=` /
// `center=` on bend / twist / taper), `magnitude_mode=` on noise_deform, and the CLAMPED domain
// every silently-clamped count publishes.
//
// What each group would let through:
//
//  - A PARAMETER THE COMPILER HONOURS AND THE TABLE OMITS is one that works and cannot be found.
//    `model.describe_ops` emits the op table verbatim and both format documents tell an author to
//    read parameters from there and from nowhere else, so the table is the only discovery
//    surface there is. These check the table AND the geometry, because publishing a parameter
//    that reaches no op is the same defect wearing the other face.
//
//  - THE DEFAULTS MUST BE BEHAVIOUR-PRESERVING, and "we did not change the default" is not a
//    measurement of that. The claim is arithmetic - axis z with centre (0,0,0) builds the
//    identity basis and a zero translation - so the test writes the defaults out explicitly and
//    requires the result to match omitting them. Every shipped document depends on this holding.
//
//  - THE CLAMPED DOMAIN MUST NOT BECOME AN ENFORCED ONE. A clamped count is ACCEPTED below its
//    floor, moved, and warned about; the second tier where `<= 0` means "unset" lives outside the
//    domain by construction. Declaring the domain as the parser's enforced range would convert
//    every clamp into a refusal and make the unset tier unwritable - so the negative is asserted
//    by compiling values that must NOT be refused, not merely by reading a flag.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Handlers/Geometry/GeometryClampDomains.h"
#include "Model/PwModelAst.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.
FPwModelCompileResult PwModelDeformerTest_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FString PwModelDeformerTest_Describe(const FPwModelCompileResult& Result)
{
    FString Bounds = TEXT("(no parts)");
    if (Result.Parts.Num() > 0 && Result.Parts[0].MeshBounds.IsValid != 0)
    {
        const FBox& Box = Result.Parts[0].MeshBounds;
        Bounds = FString::Printf(TEXT("min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f)"),
            Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z);
    }
    return FString::Printf(TEXT("success=%d tris=%d bounds=%s diagnostics=[%s]"),
        Result.bSuccess ? 1 : 0, Result.MeshTriangleCount, *Bounds,
        *JoinPwDiagnostics(Result.Diagnostics));
}

bool PwModelDeformerTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return true;
        }
    }
    return false;
}

bool PwModelDeformerTest_Bounds(const FPwModelCompileResult& Result, FBox& OutBounds)
{
    if (Result.Parts.Num() != 1 || Result.Parts[0].MeshBounds.IsValid == 0)
    {
        return false;
    }
    OutBounds = Result.Parts[0].MeshBounds;
    return true;
}

// Two documents whose part bounds must agree to within float residue. Used for the
// behaviour-preserving claims, where the two runs are supposed to be the SAME arithmetic.
void PwModelDeformerTest_ExpectSameShape(FAutomationTestBase& Test, const TCHAR* What,
                                         const TCHAR* SourceA, const TCHAR* SourceB)
{
    const FPwModelCompileResult A = PwModelDeformerTest_Validate(SourceA);
    const FPwModelCompileResult B = PwModelDeformerTest_Validate(SourceB);

    Test.TestTrue(*FString::Printf(TEXT("%s: the first document compiled. %s"),
        What, *PwModelDeformerTest_Describe(A)), A.bSuccess);
    Test.TestTrue(*FString::Printf(TEXT("%s: the second document compiled. %s"),
        What, *PwModelDeformerTest_Describe(B)), B.bSuccess);

    FBox BoundsA;
    FBox BoundsB;
    if (!PwModelDeformerTest_Bounds(A, BoundsA) || !PwModelDeformerTest_Bounds(B, BoundsB))
    {
        Test.AddError(FString::Printf(TEXT("%s: one of the two runs produced no box."), What));
        return;
    }

    Test.TestEqual(*FString::Printf(TEXT("%s: the triangle counts match"), What),
        A.MeshTriangleCount, B.MeshTriangleCount);
    Test.TestTrue(*FString::Printf(
        TEXT("%s: the boxes match. A %s | B %s"), What,
        *PwModelDeformerTest_Describe(A), *PwModelDeformerTest_Describe(B)),
        (BoundsA.Min - BoundsB.Min).GetAbsMax() < 0.01
            && (BoundsA.Max - BoundsB.Max).GetAbsMax() < 0.01);
}
}

// ============================================================================
// The warp frame
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarpFrameIsPublishedTest,
    "PinWright.Model.Deformers.EveryWarpDeformerPublishesItsFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWarpFrameIsPublishedTest::RunTest(const FString& Parameters)
{
    // The three that share FWarpFrameSpec. Named rather than swept, because the property is that
    // these SPECIFIC ops gained the pair - a sweep keyed on "has an extent" would pass on an
    // empty set if the ops were ever renamed.
    for (const TCHAR* OpName : { TEXT("bend"), TEXT("twist"), TEXT("taper") })
    {
        const FPwModelOpSpec* Spec = PwModelOpTable::Find(FString(OpName), EPwModelOpContext::Part);
        if (!Spec)
        {
            AddError(FString::Printf(TEXT("no part-context op named '%s'"), OpName));
            continue;
        }

        const FPwModelParamSpec* Axis = Spec->FindParam(FString(TEXT("axis")));
        const FPwModelParamSpec* Center = Spec->FindParam(FString(TEXT("center")));

        TestNotNull(*FString::Printf(TEXT("'%s' publishes 'axis'"), OpName), Axis);
        TestNotNull(*FString::Printf(TEXT("'%s' publishes 'center'"), OpName), Center);

        if (Axis)
        {
            // Spelled the same way harmonic_deform spells it - the sibling deformer - so the
            // family is learned once. A different vocabulary here would be a silent second
            // dialect rather than a missing feature.
            TestEqual(*FString::Printf(TEXT("'%s' axis defaults to z"), OpName),
                Axis->Default, FString(TEXT("z")));
            TestTrue(*FString::Printf(TEXT("'%s' axis takes the three axis names"), OpName),
                Axis->AllowedValues.Contains(TEXT("x"))
                    && Axis->AllowedValues.Contains(TEXT("y"))
                    && Axis->AllowedValues.Contains(TEXT("z")));
        }
        if (Center)
        {
            TestTrue(*FString::Printf(TEXT("'%s' center is a 3-vector"), OpName),
                Center->Type == EPwModelParamType::Vector3);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarpFrameDefaultsAreIdentityTest,
    "PinWright.Model.Deformers.WritingTheFrameDefaultsChangesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWarpFrameDefaultsAreIdentityTest::RunTest(const FString& Parameters)
{
    // The behaviour-preserving claim, measured rather than asserted in a comment. Axis z with
    // centre (0,0,0) is supposed to build the identity basis and a zero translation - which is
    // what these ops passed before the frame existed - so the explicit spelling and the omitted
    // one must produce the same mesh. Every shipped document depends on this.
    PwModelDeformerTest_ExpectSameShape(*this, TEXT("bend"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    cylinder radius=10 height=200 height_steps=12\n")
        TEXT("    bend angle=40 extent=100\n")
        TEXT("}\n"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    cylinder radius=10 height=200 height_steps=12\n")
        TEXT("    bend angle=40 extent=100 axis=z center=(0, 0, 0)\n")
        TEXT("}\n"));

    PwModelDeformerTest_ExpectSameShape(*this, TEXT("twist"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    box size=(40, 40, 200) segments=(2, 2, 12)\n")
        TEXT("    twist angle=60 extent=100\n")
        TEXT("}\n"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    box size=(40, 40, 200) segments=(2, 2, 12)\n")
        TEXT("    twist angle=60 extent=100 axis=z center=(0, 0, 0)\n")
        TEXT("}\n"));

    PwModelDeformerTest_ExpectSameShape(*this, TEXT("taper"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    cylinder radius=20 height=200 height_steps=12\n")
        TEXT("    taper flare=(60, 60) extent=100\n")
        TEXT("}\n"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    cylinder radius=20 height=200 height_steps=12\n")
        TEXT("    taper flare=(60, 60) extent=100 axis=z center=(0, 0, 0)\n")
        TEXT("}\n"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarpFrameReachesTheOpTest,
    "PinWright.Model.Deformers.TheFrameActuallyMovesWhatTheDeformerTouches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWarpFrameReachesTheOpTest::RunTest(const FString& Parameters)
{
    // Published and honoured are different claims, and a parser that accepted the parameter and
    // dropped it would satisfy the first test above while changing nothing. This one measures the
    // geometry: a bend about X and the same bend about Z cannot produce the same box.
    const FPwModelCompileResult AboutZ = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    cylinder radius=10 height=200 height_steps=12\n")
        TEXT("    bend angle=60 extent=100 axis=z\n")
        TEXT("}\n"));
    const FPwModelCompileResult AboutX = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    cylinder radius=10 height=200 height_steps=12\n")
        TEXT("    bend angle=60 extent=100 axis=x\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the z-axis bend compiled. %s"), *PwModelDeformerTest_Describe(AboutZ)),
        AboutZ.bSuccess);
    TestTrue(*FString::Printf(TEXT("the x-axis bend compiled. %s"), *PwModelDeformerTest_Describe(AboutX)),
        AboutX.bSuccess);

    FBox BoundsZ;
    FBox BoundsX;
    if (PwModelDeformerTest_Bounds(AboutZ, BoundsZ) && PwModelDeformerTest_Bounds(AboutX, BoundsX))
    {
        TestTrue(*FString::Printf(
            TEXT("'axis' reaches the op - the two bends differ. z %s | x %s"),
            *PwModelDeformerTest_Describe(AboutZ), *PwModelDeformerTest_Describe(AboutX)),
            (BoundsZ.Min - BoundsX.Min).GetAbsMax() > 0.5
                || (BoundsZ.Max - BoundsX.Max).GetAbsMax() > 0.5);
    }
    else
    {
        AddError(TEXT("one of the two bends produced no box."));
    }

    // And `center`, on its own: the extent is measured FROM the centre, so moving it off the
    // origin moves which band of the mesh is deformed. This is the case no combination of extent
    // and lower_extent can express, since both are measured ALONG the axis.
    const FPwModelCompileResult AtOrigin = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    box size=(40, 20, 200) segments=(2, 2, 16) at=(0, 0, 300)\n")
        TEXT("    twist angle=90 extent=100\n")
        TEXT("}\n"));
    const FPwModelCompileResult AtTheLimb = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    box size=(40, 20, 200) segments=(2, 2, 16) at=(0, 0, 300)\n")
        TEXT("    twist angle=90 extent=100 center=(0, 0, 300)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the origin-centred twist compiled. %s"),
        *PwModelDeformerTest_Describe(AtOrigin)), AtOrigin.bSuccess);
    TestTrue(*FString::Printf(TEXT("the limb-centred twist compiled. %s"),
        *PwModelDeformerTest_Describe(AtTheLimb)), AtTheLimb.bSuccess);

    FBox OriginBounds;
    FBox LimbBounds;
    if (PwModelDeformerTest_Bounds(AtOrigin, OriginBounds)
        && PwModelDeformerTest_Bounds(AtTheLimb, LimbBounds))
    {
        // A limb sitting at z 200..400 lies entirely OUTSIDE a [-100, +100] extent centred on the
        // origin, so the origin-centred twist leaves its rectangular cross-section unchanged; the
        // limb-centred one twists that anisotropic cross-section. The X/Y extents are what
        // separate them.
        TestTrue(*FString::Printf(
            TEXT("'center' reaches the op - the two twists differ. origin %s | limb %s"),
            *PwModelDeformerTest_Describe(AtOrigin), *PwModelDeformerTest_Describe(AtTheLimb)),
            (OriginBounds.GetSize() - LimbBounds.GetSize()).GetAbsMax() > 0.5
                || (OriginBounds.GetCenter() - LimbBounds.GetCenter()).GetAbsMax() > 0.5);
    }
    else
    {
        AddError(TEXT("one of the two twists produced no box."));
    }

    return true;
}

// ============================================================================
// noise_deform magnitude_mode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelNoiseMagnitudeModeTest,
    "PinWright.Model.Deformers.NoiseMagnitudeModeIsPublishedAndReachesTheOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelNoiseMagnitudeModeTest::RunTest(const FString& Parameters)
{
    const FPwModelOpSpec* Spec = PwModelOpTable::Find(FString(TEXT("noise_deform")), EPwModelOpContext::Part);
    if (!Spec)
    {
        AddError(TEXT("no part-context op named 'noise_deform'"));
        return true;
    }

    const FPwModelParamSpec* Mode = Spec->FindParam(FString(TEXT("magnitude_mode")));
    if (TestNotNull(TEXT("'noise_deform' publishes 'magnitude_mode'"), Mode))
    {
        // absolute is the engine's own reading and must stay the default, or every existing
        // document silently changes shape.
        TestEqual(TEXT("it defaults to absolute, so existing documents are unchanged"),
            Mode->Default, FString(TEXT("absolute")));
        TestTrue(TEXT("and it takes both modes"),
            Mode->AllowedValues.Contains(TEXT("absolute")) && Mode->AllowedValues.Contains(TEXT("relative")));
    }

    PwModelDeformerTest_ExpectSameShape(*this, TEXT("noise_deform default"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=4 frequency=0.1 seed=7\n")
        TEXT("}\n"),
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=4 frequency=0.1 seed=7 magnitude_mode=absolute\n")
        TEXT("}\n"));

    // Relative reads the same field with a different ruler, so the same magnitude must produce a
    // different surface - otherwise the parameter is decoration.
    const FPwModelCompileResult Absolute = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=4 frequency=0.1 seed=7 magnitude_mode=absolute\n")
        TEXT("}\n"));
    const FPwModelCompileResult Relative = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=4 frequency=0.1 seed=7 magnitude_mode=relative\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the absolute run compiled. %s"), *PwModelDeformerTest_Describe(Absolute)),
        Absolute.bSuccess);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    TestTrue(*FString::Printf(TEXT("the relative run compiled. %s"), *PwModelDeformerTest_Describe(Relative)),
        Relative.bSuccess);

    FBox AbsoluteBounds;
    FBox RelativeBounds;
    if (PwModelDeformerTest_Bounds(Absolute, AbsoluteBounds)
        && PwModelDeformerTest_Bounds(Relative, RelativeBounds))
    {
        TestTrue(*FString::Printf(
            TEXT("'magnitude_mode' reaches the op - the two rulers differ. absolute %s | relative %s"),
            *PwModelDeformerTest_Describe(Absolute), *PwModelDeformerTest_Describe(Relative)),
            (AbsoluteBounds.GetSize() - RelativeBounds.GetSize()).GetAbsMax() > 0.01);
    }
    else
    {
        AddError(TEXT("one of the two noise runs produced no box."));
    }
#else
    // Relative magnitude is built on the engine's ComputePerlinNoise, which only ships from UE
    // 5.8, so the op refuses the mode here rather than serving absolute under its name (see
    // NoiseDeform in GeometryOps_Modeling.cpp). The parameter must still be PUBLISHED and must
    // still REACH the op - a document written against a newer editor has to come back with that
    // refusal, naming the engine, and not with "unknown parameter".
    TestFalse(*FString::Printf(TEXT("the relative run is refused on this engine. %s"),
        *PwModelDeformerTest_Describe(Relative)), Relative.bSuccess);
    TestTrue(*FString::Printf(TEXT("and the refusal names the engine version it needs. %s"),
        *PwModelDeformerTest_Describe(Relative)),
        PwModelDeformerTest_Describe(Relative).Contains(TEXT("UNSUPPORTED_ENGINE_VERSION")));
#endif

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelNoiseModeConflictTest,
    "PinWright.Model.Deformers.RelativeMagnitudeAlongTheNoiseVectorIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelNoiseModeConflictTest::RunTest(const FString& Parameters)
{
    // The op refuses this pair. Refusing it at the LINE is the point: reaching the op instead
    // gives PWMODEL_OP_FAILED carrying INVALID_ARGUMENT with no position of its own, on a
    // condition that is plainly visible in the source text.
    const FPwModelCompileResult Result = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=0.5 magnitude_mode=relative apply_along_normal=false\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("the compile failed. %s"), *PwModelDeformerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("the conflict is named. %s"), *PwModelDeformerTest_Describe(Result)),
        PwModelDeformerTest_HasCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_NOISE_MODE_CONFLICT));

    // The parser owns it, so a validate-only run reports it before any mesh exists - which means
    // the diagnostic carries the line rather than arriving from the op stage at -1.
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_NOISE_MODE_CONFLICT)
        {
            TestTrue(TEXT("and it is anchored on a real source line"), Diagnostic.Line > 0);
        }
    }

    // Each half alone is legal, or the refusal would be banning ordinary documents.
    const FPwModelCompileResult RelativeAlone = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=0.5 magnitude_mode=relative\n")
        TEXT("}\n"));
    const FPwModelCompileResult VectorAlone = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a {\n")
        TEXT("    sphere radius=50 subdivisions=12\n")
        TEXT("    noise_deform magnitude=4 apply_along_normal=false\n")
        TEXT("}\n"));

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    TestTrue(*FString::Printf(TEXT("relative magnitude alone is legal. %s"),
        *PwModelDeformerTest_Describe(RelativeAlone)), RelativeAlone.bSuccess);
#else
    // On this engine relative magnitude is refused for want of ComputePerlinNoise, so the half
    // that is still asserted is the one that matters here: the refusal is the ENGINE's, arriving
    // from the op, and not the line-level mode conflict this test is about.
    TestFalse(*FString::Printf(TEXT("relative magnitude alone is refused by the engine, not the line. %s"),
        *PwModelDeformerTest_Describe(RelativeAlone)), RelativeAlone.bSuccess);
    TestFalse(*FString::Printf(TEXT("and it is not the mode conflict. %s"),
        *PwModelDeformerTest_Describe(RelativeAlone)),
        PwModelDeformerTest_HasCode(RelativeAlone.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_NOISE_MODE_CONFLICT));
#endif
    TestTrue(*FString::Printf(TEXT("vector displacement alone is legal. %s"),
        *PwModelDeformerTest_Describe(VectorAlone)), VectorAlone.bSuccess);

    return true;
}

// ============================================================================
// The clamped domain
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelClampDomainPublishedTest,
    "PinWright.Model.Deformers.ClampedCountsPublishTheirDomainAsFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelClampDomainPublishedTest::RunTest(const FString& Parameters)
{
    // Walks the ops layer's declared table - the one whose own test RUNS each op at its bounds -
    // rather than a second list written here. A row this format publishes must arrive on the
    // matching parameter; a row it does not publish (an op with no .pwmodel spelling) is not this
    // test's business and is covered by the geometry-side one.
    int32 Stamped = 0;
    for (const GeometryOps::ClampDomains::FClampDomain& Domain : GeometryOps::ClampDomains::Entries())
    {
        for (const FPwModelOpSpec& Spec : PwModelOpTable::Get())
        {
            if (Spec.Context != EPwModelOpContext::Part)
            {
                continue;
            }
            for (const FPwModelParamSpec& Param : Spec.Params)
            {
                if (!Param.bHasClampDomain)
                {
                    continue;
                }
                if (Param.ClampMax != Domain.ClampMax
                    || Param.ClampMinClosedSweep != Domain.ClampMinClosedSweep)
                {
                    continue;
                }

                // THE LOAD-BEARING NEGATIVE. A clamped domain is advisory; the enforced pair must
                // stay absent, or every clamp becomes a refusal and the `<= 0 means unset` tier
                // becomes unwritable.
                TestFalse(*FString::Printf(
                    TEXT("'%s.%s' publishes a clamped domain and must NOT enforce it"),
                    *Spec.Name, *Param.Name), Param.bHasRange);

                TestTrue(*FString::Printf(TEXT("'%s.%s' carries a sane domain (%d-%d)"),
                    *Spec.Name, *Param.Name, Param.ClampMin, Param.ClampMax),
                    Param.ClampMin >= 0 && Param.ClampMax > Param.ClampMin);
                ++Stamped;
            }
        }
    }

    TestTrue(*FString::Printf(
        TEXT("the sweep found clamped domains stamped onto the op table (found %d)"), Stamped),
        Stamped > 0);

    // Spot-checked by name as well as by sweep, because a sweep over an empty intersection passes
    // silently. `cylinder segments` is the archetype: floor 3, ceiling 256, and 0-or-less means
    // unset and takes 16.
    if (const FPwModelOpSpec* Cylinder = PwModelOpTable::Find(FString(TEXT("cylinder")), EPwModelOpContext::Part))
    {
        if (const FPwModelParamSpec* Segments = Cylinder->FindParam(FString(TEXT("segments"))))
        {
            TestTrue(TEXT("cylinder segments publishes a clamped domain"), Segments->bHasClampDomain);
            TestEqual(TEXT("its floor is 3"), Segments->ClampMin, 3);
            TestEqual(TEXT("its ceiling is 256"), Segments->ClampMax, 256);
            TestTrue(TEXT("and 0 or less reads as unset"), Segments->bClampZeroMeansUnset);
            TestEqual(TEXT("taking the verb's own default"), Segments->ClampUnsetDefault, 16);
        }
        else
        {
            AddError(TEXT("cylinder publishes no 'segments' parameter"));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelClampDomainIsNotEnforcedTest,
    "PinWright.Model.Deformers.AClampedCountIsMovedNotRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelClampDomainIsNotEnforcedTest::RunTest(const FString& Parameters)
{
    // The negative asserted as BEHAVIOUR rather than as a flag. If the clamped domain were ever
    // declared as the parser's enforced range, each of these would become PWSRC_BAD_VALUE and the
    // documented tiers would stop being writable - so they are compiled, not inspected.

    // Below the floor: accepted, moved, and warned about.
    const FPwModelCompileResult BelowFloor = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a { cylinder radius=10 height=50 segments=1 }\n"));
    TestTrue(*FString::Printf(TEXT("a count below the floor still compiles. %s"),
        *PwModelDeformerTest_Describe(BelowFloor)), BelowFloor.bSuccess);
    TestTrue(*FString::Printf(TEXT("and it says it was clamped. %s"),
        *PwModelDeformerTest_Describe(BelowFloor)),
        PwModelDeformerTest_HasCode(BelowFloor.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING));

    // Zero: the second tier. Not raised to the floor - it means "unset" and takes the verb's own
    // default, which is outside the clamped domain by construction and is exactly the value an
    // enforced range would reject.
    const FPwModelCompileResult Zero = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a { cylinder radius=10 height=50 segments=0 }\n"));
    TestTrue(*FString::Printf(TEXT("zero is legal and means unset. %s"),
        *PwModelDeformerTest_Describe(Zero)), Zero.bSuccess);

    // Above the ceiling: accepted and moved, same as below the floor.
    const FPwModelCompileResult AboveCeiling = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a { cylinder radius=10 height=50 segments=9999 }\n"));
    TestTrue(*FString::Printf(TEXT("a count above the ceiling still compiles. %s"),
        *PwModelDeformerTest_Describe(AboveCeiling)), AboveCeiling.bSuccess);

    // The unset tier really does take the verb's default rather than the floor: 0 must build the
    // 16-segment cylinder, not the 3-segment prism the floor would give.
    const FPwModelCompileResult AtDefault = PwModelDeformerTest_Validate(
        TEXT("pwmodel 0\npart a { cylinder radius=10 height=50 }\n"));
    TestTrue(*FString::Printf(TEXT("the unset run compiled. %s"), *PwModelDeformerTest_Describe(AtDefault)),
        AtDefault.bSuccess);
    TestEqual(TEXT("segments=0 builds what writing nothing builds, not what the floor builds"),
        Zero.MeshTriangleCount, AtDefault.MeshTriangleCount);
    TestTrue(*FString::Printf(
        TEXT("and that is not the floor's mesh (unset %d triangles, floor-clamped %d)"),
        Zero.MeshTriangleCount, BelowFloor.MeshTriangleCount),
        Zero.MeshTriangleCount != BelowFloor.MeshTriangleCount);

    return true;
}
