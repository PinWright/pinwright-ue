// Copyright (c) 2026 Alexander Penkin. MIT License.

// `from=` / `to=` / `up=` on a generator: what aiming produces, and every way of writing a
// contradiction that must be refused rather than resolved.
//
// What each group would let through if it were reverted:
//
//  - THE SPAN. An aimed op is only useful if its geometry actually reaches both points, and the
//    two halves of that - the midpoint and the length - fail independently. These measure the
//    part's bounding box against the endpoints, which is the one assertion that catches a
//    midpoint off by half a length, an axis mapped to the wrong local direction, or a length
//    taken from the wrong parameter. A test that only checked "it compiled" would pass on all
//    three.
//
//  - THE CAPSULE'S CAPS. `length` is the cylindrical section ONLY, with a hemisphere of `radius`
//    added at each end, so an aimed capsule needs `length = distance - 2*radius`. Handing the
//    generator the raw distance instead overshoots `to` by exactly one radius at each end. It is
//    invisible in every count, it compiles green, and it is the primitive an author reaches for
//    most when aiming between two joints - so it is measured here against a cylinder aimed the
//    same way, which must produce the SAME span from different arithmetic.
//
//  - THE TWIST. Aiming fixes two degrees of freedom. The third - the rotation about the aimed
//    axis - decides which world direction a non-uniform `scale=` flattens towards, because the
//    flattening axis is the primitive's own local Y. Any implementation that solves only for the
//    direction leaves it to whatever the arithmetic happened to produce, and a fan of flattened
//    cards then comes out half flat and half on edge with every parameter in the document
//    correct and nothing to grep for. DefaultTwistIsTheSameFrameAtEveryAzimuth measures the
//    unflattened axis across four aims that differ only in heading; UpChoosesWhichAxisFlattens
//    measures that `up` moves it, which is what makes the degree of freedom the author's.
//
//  - THE REFUSALS. Each names an input from which two different placements can be derived. A
//    compiler that picks one produces geometry the document does not describe and reports
//    success - which is the failure the whole family exists to make impossible, so a refusal
//    quietly becoming a warning is a regression even though nothing turns red.
//
//  - THE VOCABULARY. model.describe_ops emits the op table verbatim and both format documents
//    tell an author to read the parameters from there and from nowhere else. A generator that
//    takes at/rotate/scale and does NOT publish from/to/up is a parameter that works and cannot
//    be discovered; an aim kind that disagrees with the parameter it names is a published
//    contradiction. Both are checked against the table rather than against a second list.
#include "Misc/AutomationTest.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.
FPwModelCompileResult PwModelAimTest_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FString PwModelAimTest_Describe(const FPwModelCompileResult& Result)
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

bool PwModelAimTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

int32 PwModelAimTest_CountCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    int32 Count = 0;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            ++Count;
        }
    }
    return Count;
}

// A refusal has to FAIL the compile as well as carry its code: a diagnostic emitted at Warning
// severity still reports success:true, which is the exact outcome these codes exist to prevent.
void PwModelAimTest_ExpectRefusal(FAutomationTestBase& Test, const TCHAR* Code, const TCHAR* Source)
{
    const FPwModelCompileResult Result = PwModelAimTest_Validate(Source);
    Test.TestFalse(*FString::Printf(TEXT("%s: the compile failed. %s"),
        Code, *PwModelAimTest_Describe(Result)), Result.bSuccess);
    Test.TestTrue(*FString::Printf(TEXT("%s was reported. %s"),
        Code, *PwModelAimTest_Describe(Result)), PwModelAimTest_HasCode(Result.Diagnostics, Code));
}

// The bounding box of the one part, or an invalid box when the run produced none. Every span
// assertion below goes through this so a failure prints the box it measured.
bool PwModelAimTest_PartBounds(const FPwModelCompileResult& Result, FBox& OutBounds)
{
    if (Result.Parts.Num() != 1 || Result.Parts[0].MeshBounds.IsValid == 0)
    {
        return false;
    }
    OutBounds = Result.Parts[0].MeshBounds;
    return true;
}

// Generous against a tessellated round primitive and far tighter than any failure this file is
// written to catch: the capsule-cap mistake is off by a whole radius, the twist mistakes swap
// axes outright, and a midpoint mistake is off by half a length.
constexpr double PwModelAimTest_Tolerance = 0.75;
}

// ============================================================================
// What an aim produces
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimCylinderSpansTheTwoPointsTest,
    "PinWright.Model.Aim.AimedGeometryReachesBothPoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimCylinderSpansTheTwoPointsTest::RunTest(const FString& Parameters)
{
    // Aimed along +X, so the local +Z the cylinder is built along has to land on world +X. A
    // cylinder is centre-placed, so a correct aim puts the box at x 0..200 with the radius on the
    // other two axes; the midpoint and the length are both wrong in a visible way if either half
    // of the derivation is.
    const FPwModelCompileResult Result = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part limb {\n")
        TEXT("    cylinder radius=10 from=(0, 0, 0) to=(200, 0, 0)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelAimTest_Describe(Result)),
        Result.bSuccess);

    FBox Bounds;
    if (!PwModelAimTest_PartBounds(Result, Bounds))
    {
        AddError(FString::Printf(TEXT("expected one part with a box. %s"), *PwModelAimTest_Describe(Result)));
        return true;
    }

    TestTrue(*FString::Printf(TEXT("the geometry starts at 'from'. %s"), *PwModelAimTest_Describe(Result)),
        FMath::Abs(Bounds.Min.X - 0.0) < PwModelAimTest_Tolerance);
    TestTrue(*FString::Printf(TEXT("the geometry ends at 'to'. %s"), *PwModelAimTest_Describe(Result)),
        FMath::Abs(Bounds.Max.X - 200.0) < PwModelAimTest_Tolerance);
    TestTrue(*FString::Printf(TEXT("the radius is on the cross axes, not the aim. %s"),
        *PwModelAimTest_Describe(Result)),
        FMath::Abs(Bounds.Max.Y - 10.0) < PwModelAimTest_Tolerance
            && FMath::Abs(Bounds.Max.Z - 10.0) < PwModelAimTest_Tolerance);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimDiagonalMidpointTest,
    "PinWright.Model.Aim.ADiagonalAimIsCentredOnTheMidpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimDiagonalMidpointTest::RunTest(const FString& Parameters)
{
    // Off-axis in all three components, so no coincidence of a single axis can carry the result:
    // the endpoints are 100 apart in x, y and z, the midpoint is (50, 50, 50), and the aim
    // direction is a diagonal that no default rotation produces.
    const FPwModelCompileResult Result = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part strut {\n")
        TEXT("    cylinder radius=4 segments=24 from=(0, 0, 0) to=(100, 100, 100)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelAimTest_Describe(Result)),
        Result.bSuccess);

    FBox Bounds;
    if (!PwModelAimTest_PartBounds(Result, Bounds))
    {
        AddError(FString::Printf(TEXT("expected one part with a box. %s"), *PwModelAimTest_Describe(Result)));
        return true;
    }

    const FVector Centre = Bounds.GetCenter();
    TestTrue(*FString::Printf(TEXT("the box is centred on the midpoint of the two points. %s"),
        *PwModelAimTest_Describe(Result)),
        (Centre - FVector(50.0, 50.0, 50.0)).GetAbsMax() < PwModelAimTest_Tolerance);

    // A cylinder aimed along a diagonal reaches its endpoints exactly, and its box therefore
    // extends past them by the radius on every axis - which is what proves the AXIS landed on
    // the diagonal rather than on a nearby axis-aligned direction.
    TestTrue(*FString::Printf(TEXT("the box brackets both endpoints. %s"), *PwModelAimTest_Describe(Result)),
        Bounds.Min.X < 0.0 && Bounds.Min.Y < 0.0 && Bounds.Min.Z < 0.0
            && Bounds.Max.X > 100.0 && Bounds.Max.Y > 100.0 && Bounds.Max.Z > 100.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimCapsuleSubtractsItsCapsTest,
    "PinWright.Model.Aim.AnAimedCapsuleSpansTheSameDistanceAsACylinder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimCapsuleSubtractsItsCapsTest::RunTest(const FString& Parameters)
{
    // The two ops are aimed identically and their spans MUST match, from different arithmetic:
    // the cylinder takes `height` = 200 and the capsule takes `length` = 200 - 2*radius, because
    // its two hemispheres add the radius back at each end. Written as a comparison rather than
    // as two literal numbers because that is the property that matters - an author swapping one
    // primitive for the other must not have to re-derive anything.
    const FPwModelCompileResult Capsule = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part limb {\n")
        TEXT("    capsule radius=25 from=(0, 0, 0) to=(200, 0, 0)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the capsule compiled. %s"), *PwModelAimTest_Describe(Capsule)),
        Capsule.bSuccess);

    FBox Bounds;
    if (!PwModelAimTest_PartBounds(Capsule, Bounds))
    {
        AddError(FString::Printf(TEXT("expected one part with a box. %s"), *PwModelAimTest_Describe(Capsule)));
        return true;
    }

    // Taking the raw distance as `length` instead would put these at -25 and 225.
    TestTrue(*FString::Printf(TEXT("the capsule starts at 'from', caps included. %s"),
        *PwModelAimTest_Describe(Capsule)),
        FMath::Abs(Bounds.Min.X - 0.0) < PwModelAimTest_Tolerance);
    TestTrue(*FString::Printf(TEXT("the capsule ends at 'to', caps included. %s"),
        *PwModelAimTest_Describe(Capsule)),
        FMath::Abs(Bounds.Max.X - 200.0) < PwModelAimTest_Tolerance);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimBoxKeepsItsCrossSectionTest,
    "PinWright.Model.Aim.AnAimedBoxTakesOnlyItsThirdComponentFromTheAim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimBoxKeepsItsCrossSectionTest::RunTest(const FString& Parameters)
{
    // A box's extent along local Z lives inside `size`, so aiming one has to replace exactly one
    // of the three components and leave the author's other two alone. The zero is the spelling
    // that says "take this from the aim" - any other value is a second answer for the same
    // length and is refused by the test below.
    const FPwModelCompileResult Result = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part beam {\n")
        TEXT("    box size=(30, 12, 0) from=(0, 0, 0) to=(0, 0, 150)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelAimTest_Describe(Result)),
        Result.bSuccess);

    FBox Bounds;
    if (!PwModelAimTest_PartBounds(Result, Bounds))
    {
        AddError(FString::Printf(TEXT("expected one part with a box. %s"), *PwModelAimTest_Describe(Result)));
        return true;
    }

    const FVector Size = Bounds.GetSize();
    TestTrue(*FString::Printf(TEXT("the cross-section is the author's. %s"), *PwModelAimTest_Describe(Result)),
        FMath::Abs(Size.X - 30.0) < PwModelAimTest_Tolerance
            && FMath::Abs(Size.Y - 12.0) < PwModelAimTest_Tolerance);
    TestTrue(*FString::Printf(TEXT("the length is the distance between the points. %s"),
        *PwModelAimTest_Describe(Result)),
        FMath::Abs(Size.Z - 150.0) < PwModelAimTest_Tolerance);
    TestTrue(*FString::Printf(TEXT("the box spans from one point to the other. %s"),
        *PwModelAimTest_Describe(Result)),
        FMath::Abs(Bounds.Min.Z - 0.0) < PwModelAimTest_Tolerance
            && FMath::Abs(Bounds.Max.Z - 150.0) < PwModelAimTest_Tolerance);

    return true;
}

// ============================================================================
// The twist
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimDefaultTwistIsStableTest,
    "PinWright.Model.Aim.DefaultTwistIsTheSameFrameAtEveryAzimuth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimDefaultTwistIsStableTest::RunTest(const FString& Parameters)
{
    // THE TWIST TRAP, measured. Four horizontal aims differing only in heading, each carrying the
    // same flattening scale. `scale=` acts in the primitive's LOCAL frame and flattens along its
    // local Y, so a twist that varies with heading flattens a different world axis for each of
    // these - the failure that produces a fan of cards half flat and half on edge.
    //
    // The observable is the VERTICAL extent. With the twist pinned, local Y is horizontal for
    // every horizontal aim, so the vertical extent stays the full unflattened 2*radius on all
    // four; an unpinned solve gives four different answers, at least one of them near 0.2 of it.
    struct FCase
    {
        const TCHAR* Name;
        const TCHAR* Source;
    };

    const FCase Cases[] = {
        { TEXT("+X"), TEXT("pwmodel 0\npart a { cylinder radius=20 segments=24 scale=(1, 0.2, 1) from=(0, 0, 0) to=(100, 0, 0) }\n") },
        { TEXT("+Y"), TEXT("pwmodel 0\npart a { cylinder radius=20 segments=24 scale=(1, 0.2, 1) from=(0, 0, 0) to=(0, 100, 0) }\n") },
        { TEXT("-X"), TEXT("pwmodel 0\npart a { cylinder radius=20 segments=24 scale=(1, 0.2, 1) from=(0, 0, 0) to=(-100, 0, 0) }\n") },
        { TEXT("diagonal"), TEXT("pwmodel 0\npart a { cylinder radius=20 segments=24 scale=(1, 0.2, 1) from=(0, 0, 0) to=(70, 70, 0) }\n") },
    };

    for (const FCase& Case : Cases)
    {
        const FPwModelCompileResult Result = PwModelAimTest_Validate(Case.Source);
        TestTrue(*FString::Printf(TEXT("%s aim compiled. %s"), Case.Name, *PwModelAimTest_Describe(Result)),
            Result.bSuccess);

        FBox Bounds;
        if (!PwModelAimTest_PartBounds(Result, Bounds))
        {
            AddError(FString::Printf(TEXT("%s aim produced no box. %s"),
                Case.Name, *PwModelAimTest_Describe(Result)));
            continue;
        }

        TestTrue(*FString::Printf(
            TEXT("%s aim keeps the unflattened axis vertical (expected a 40 uu vertical extent, got %.3f). %s"),
            Case.Name, Bounds.GetSize().Z, *PwModelAimTest_Describe(Result)),
            FMath::Abs(Bounds.GetSize().Z - 40.0) < PwModelAimTest_Tolerance);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimUpChoosesTheFlattenedAxisTest,
    "PinWright.Model.Aim.UpChoosesWhichAxisFlattens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimUpChoosesTheFlattenedAxisTest::RunTest(const FString& Parameters)
{
    // The same aim and the same flattening scale, twice, differing only in `up`. If `up` did not
    // reach the frame - or reached it as a suggestion the solve was free to ignore - both runs
    // would produce the same box, which is exactly what makes the degree of freedom unavailable
    // to the author rather than merely awkward.
    const TCHAR* Default =
        TEXT("pwmodel 0\n")
        TEXT("part card {\n")
        TEXT("    cylinder radius=20 segments=24 scale=(1, 0.2, 1) from=(0, 0, 0) to=(100, 0, 0)\n")
        TEXT("}\n");

    const TCHAR* Turned =
        TEXT("pwmodel 0\n")
        TEXT("part card {\n")
        TEXT("    cylinder radius=20 segments=24 scale=(1, 0.2, 1) up=(0, 1, 0) from=(0, 0, 0) to=(100, 0, 0)\n")
        TEXT("}\n");

    const FPwModelCompileResult DefaultResult = PwModelAimTest_Validate(Default);
    const FPwModelCompileResult TurnedResult = PwModelAimTest_Validate(Turned);

    TestTrue(*FString::Printf(TEXT("the default-up run compiled. %s"), *PwModelAimTest_Describe(DefaultResult)),
        DefaultResult.bSuccess);
    TestTrue(*FString::Printf(TEXT("the explicit-up run compiled. %s"), *PwModelAimTest_Describe(TurnedResult)),
        TurnedResult.bSuccess);

    FBox DefaultBounds;
    FBox TurnedBounds;
    if (!PwModelAimTest_PartBounds(DefaultResult, DefaultBounds)
        || !PwModelAimTest_PartBounds(TurnedResult, TurnedBounds))
    {
        AddError(TEXT("one of the two runs produced no box."));
        return true;
    }

    // Default up is world up, so the flattening lands on the horizontal cross axis and the
    // vertical extent survives at 2*radius.
    TestTrue(*FString::Printf(
        TEXT("default up flattens the horizontal cross axis, not the vertical one. %s"),
        *PwModelAimTest_Describe(DefaultResult)),
        FMath::Abs(DefaultBounds.GetSize().Z - 40.0) < PwModelAimTest_Tolerance
            && FMath::Abs(DefaultBounds.GetSize().Y - 8.0) < PwModelAimTest_Tolerance);

    // up=(0, 1, 0) turns the frame a quarter turn about the aim, so the same scale now flattens
    // the vertical extent and the horizontal one survives. The two boxes are each other's
    // transpose, which no implementation that ignores `up` can produce.
    TestTrue(*FString::Printf(
        TEXT("an explicit up moves the flattening onto the vertical axis. %s"),
        *PwModelAimTest_Describe(TurnedResult)),
        FMath::Abs(TurnedBounds.GetSize().Z - 8.0) < PwModelAimTest_Tolerance
            && FMath::Abs(TurnedBounds.GetSize().Y - 40.0) < PwModelAimTest_Tolerance);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimVerticalDefaultDoesNotRefuseTest,
    "PinWright.Model.Aim.AVerticalAimTakesTheFallbackFrameRatherThanRefusing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimVerticalDefaultDoesNotRefuseTest::RunTest(const FString& Parameters)
{
    // The default twist reference IS world up, and a vertical limb is the most ordinary thing an
    // author writes - so the case where the reference lies along the aim must NOT be refused the
    // way an explicit one is. Both directions, because a fallback that only covers +Z leaves the
    // downward limb to a different code path.
    const FPwModelCompileResult Up = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part mast { cylinder radius=6 from=(0, 0, 0) to=(0, 0, 120) }\n"));
    const FPwModelCompileResult Down = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part root { cylinder radius=6 from=(0, 0, 0) to=(0, 0, -120) }\n"));

    TestTrue(*FString::Printf(TEXT("an upward vertical aim compiles. %s"), *PwModelAimTest_Describe(Up)),
        Up.bSuccess);
    TestTrue(*FString::Printf(TEXT("a downward vertical aim compiles. %s"), *PwModelAimTest_Describe(Down)),
        Down.bSuccess);
    TestFalse(*FString::Printf(TEXT("no up-parallel refusal on the default frame. %s"),
        *PwModelAimTest_Describe(Up)),
        PwModelAimTest_HasCode(Up.Diagnostics, PwModelDiagnosticCodes::PWMODEL_AIM_UP_PARALLEL));

    FBox UpBounds;
    FBox DownBounds;
    if (PwModelAimTest_PartBounds(Up, UpBounds) && PwModelAimTest_PartBounds(Down, DownBounds))
    {
        TestTrue(*FString::Printf(TEXT("the upward limb spans 0..120. %s"), *PwModelAimTest_Describe(Up)),
            FMath::Abs(UpBounds.Min.Z - 0.0) < PwModelAimTest_Tolerance
                && FMath::Abs(UpBounds.Max.Z - 120.0) < PwModelAimTest_Tolerance);
        TestTrue(*FString::Printf(TEXT("the downward limb spans -120..0. %s"), *PwModelAimTest_Describe(Down)),
            FMath::Abs(DownBounds.Min.Z + 120.0) < PwModelAimTest_Tolerance
                && FMath::Abs(DownBounds.Max.Z - 0.0) < PwModelAimTest_Tolerance);
    }
    else
    {
        AddError(TEXT("one of the two vertical aims produced no box."));
    }

    return true;
}

// ============================================================================
// Refusals
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimIncompleteTest,
    "PinWright.Model.Aim.HalfAnAimIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimIncompleteTest::RunTest(const FString& Parameters)
{
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_INCOMPLETE,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 from=(0, 0, 0) }\n"));

    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_INCOMPLETE,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 to=(0, 0, 100) }\n"));

    // `up` alone pins the twist of an aim that is not there. Silently ignoring it is how a
    // document ends up carrying a parameter its author believes is doing something.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_INCOMPLETE,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 height=50 up=(0, 1, 0) }\n"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimConflictTest,
    "PinWright.Model.Aim.AimingAlongsideAtOrRotateIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimConflictTest::RunTest(const FString& Parameters)
{
    // Aiming COMPUTES at and rotate. Honouring either written value would discard the other
    // answer without saying so, and the author cannot tell which of the two placements they got.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_CONFLICT,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 at=(10, 0, 0) from=(0, 0, 0) to=(100, 0, 0) }\n"));

    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_CONFLICT,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 rotate=(0, 45, 0) from=(0, 0, 0) to=(100, 0, 0) }\n"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimDegenerateTest,
    "PinWright.Model.Aim.AZeroLengthAimIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimDegenerateTest::RunTest(const FString& Parameters)
{
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_DEGENERATE,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 from=(12, 4, 7) to=(12, 4, 7) }\n"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimUpParallelTest,
    "PinWright.Model.Aim.AnExplicitUpAlongTheAimIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimUpParallelTest::RunTest(const FString& Parameters)
{
    // Parallel and antiparallel both name no frame, and a zero vector names none either. All
    // three are the same mistake and all three are refused rather than silently falling back -
    // the fallback exists for the DEFAULT reference only, where a vertical limb is ordinary.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_UP_PARALLEL,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 up=(0, 0, 1) from=(0, 0, 0) to=(0, 0, 100) }\n"));

    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_UP_PARALLEL,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 up=(0, 0, -1) from=(0, 0, 0) to=(0, 0, 100) }\n"));

    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_UP_PARALLEL,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 up=(0, 0, 0) from=(0, 0, 0) to=(100, 0, 0) }\n"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimExtentConflictTest,
    "PinWright.Model.Aim.WritingTheLengthTwiceIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimExtentConflictTest::RunTest(const FString& Parameters)
{
    // Each of the three aim kinds, because each derives the extent from a different parameter and
    // a check keyed on one name would leave the other two accepting a second answer.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_EXTENT_CONFLICT,
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=5 height=50 from=(0, 0, 0) to=(100, 0, 0) }\n"));

    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_EXTENT_CONFLICT,
        TEXT("pwmodel 0\n")
        TEXT("part a { capsule radius=5 length=50 from=(0, 0, 0) to=(100, 0, 0) }\n"));

    // A box's non-zero Z is the second answer; its X and Y are not, which is why zero is legal
    // there and the test above it has no such exception.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_EXTENT_CONFLICT,
        TEXT("pwmodel 0\n")
        TEXT("part a { box size=(30, 12, 90) from=(0, 0, 0) to=(0, 0, 150) }\n"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimCapsuleTooShortTest,
    "PinWright.Model.Aim.ACapsuleShorterThanItsOwnCapsIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimCapsuleTooShortTest::RunTest(const FString& Parameters)
{
    // 2*radius already spans 80 of the requested 40, so the cylindrical section would be
    // negative. Clamping it to zero would silently ship a sphere where a limb was asked for.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_TOO_SHORT,
        TEXT("pwmodel 0\n")
        TEXT("part a { capsule radius=40 from=(0, 0, 0) to=(40, 0, 0) }\n"));

    // The default radius, which the parser cannot see - it lives in the generator's params
    // struct. This is the case that makes the check belong to the compiler.
    PwModelAimTest_ExpectRefusal(*this, PwModelDiagnosticCodes::PWMODEL_AIM_TOO_SHORT,
        TEXT("pwmodel 0\n")
        TEXT("part a { capsule from=(0, 0, 0) to=(20, 0, 0) }\n"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimLengthUnusedTest,
    "PinWright.Model.Aim.AimingAnOpWithNoAxialExtentSaysTheDistanceWentUnused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimLengthUnusedTest::RunTest(const FString& Parameters)
{
    // A sphere has no extent along its local Z, so an aim places and turns it and the distance
    // does nothing. That is worth a warning rather than a refusal: turning a sphere is pointless
    // but turning a plane or a disc is the whole reason those ops accept an aim at all.
    const FPwModelCompileResult Result = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a { sphere radius=10 from=(0, 0, 0) to=(100, 0, 0) }\n"));

    TestTrue(*FString::Printf(TEXT("a warning does not fail the compile. %s"),
        *PwModelAimTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("the unused distance is reported. %s"), *PwModelAimTest_Describe(Result)),
        PwModelAimTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_AIM_LENGTH_UNUSED));

    FBox Bounds;
    if (PwModelAimTest_PartBounds(Result, Bounds))
    {
        TestTrue(*FString::Printf(TEXT("the sphere is still placed on the midpoint. %s"),
            *PwModelAimTest_Describe(Result)),
            (Bounds.GetCenter() - FVector(50.0, 0.0, 0.0)).GetAbsMax() < PwModelAimTest_Tolerance);
    }

    // The ops that DO take a length must not raise it, or the warning means nothing.
    const FPwModelCompileResult Sized = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=10 from=(0, 0, 0) to=(100, 0, 0) }\n"));
    TestFalse(*FString::Printf(TEXT("an op whose length the aim sets raises nothing. %s"),
        *PwModelAimTest_Describe(Sized)),
        PwModelAimTest_HasCode(Sized.Diagnostics, PwModelDiagnosticCodes::PWMODEL_AIM_LENGTH_UNUSED));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimAxialScaleTest,
    "PinWright.Model.Aim.ScalingAlongTheAimSaysTheGeometryNoLongerReachesTheSecondPoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimAxialScaleTest::RunTest(const FString& Parameters)
{
    // scale bakes into the vertices AFTER the aim has set the length, so a Z other than 1
    // multiplies the span. Legal - it is occasionally what an author wants - and therefore a
    // warning, but never silent: the whole promise of an aim is that the geometry ends at `to`.
    const FPwModelCompileResult Result = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=10 scale=(1, 1, 0.5) from=(0, 0, 0) to=(0, 0, 100) }\n"));

    TestTrue(*FString::Printf(TEXT("a warning does not fail the compile. %s"),
        *PwModelAimTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("the shortened span is reported. %s"), *PwModelAimTest_Describe(Result)),
        PwModelAimTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_AIM_AXIAL_SCALE));

    FBox Bounds;
    if (PwModelAimTest_PartBounds(Result, Bounds))
    {
        // The warning has to be true: half the length, still centred on the midpoint.
        TestTrue(*FString::Printf(TEXT("the geometry really does span half the distance. %s"),
            *PwModelAimTest_Describe(Result)),
            FMath::Abs(Bounds.GetSize().Z - 50.0) < PwModelAimTest_Tolerance);
    }

    // A cross-section scale is the intended use and must stay quiet, or the warning becomes noise
    // on every flattened card in a document.
    const FPwModelCompileResult CrossSection = PwModelAimTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a { cylinder radius=10 scale=(1, 0.2, 1) from=(0, 0, 0) to=(0, 0, 100) }\n"));
    TestEqual(*FString::Printf(TEXT("a cross-section scale raises nothing. %s"),
        *PwModelAimTest_Describe(CrossSection)),
        PwModelAimTest_CountCode(CrossSection.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_AIM_AXIAL_SCALE), 0);

    return true;
}

// ============================================================================
// The published vocabulary
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelAimVocabularyTest,
    "PinWright.Model.Aim.EveryTransformCapableGeneratorPublishesTheAimParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelAimVocabularyTest::RunTest(const FString& Parameters)
{
    // `transform` also has `at` / `rotate` / `scale`, but it is a modifier over the geometry
    // already accumulated in the part, not a generator with a canonical local +Z and extent.
    // ResolveAim is consequently only reached by RunGenerator; publishing from/to/up on
    // transform would accept parameters that the modifier ignores. Keep that exclusion explicit
    // by name, then key the actual sweep on the table's generator flag rather than another list
    // of shape names. model.describe_ops emits this table verbatim and both format documents tell
    // authors to read parameters from there and nowhere else, so a parameter the compiler honours
    // and the table omits is one that works and cannot be found.
    int32 Checked = 0;
    for (const FPwModelOpSpec& Spec : PwModelOpTable::Get())
    {
        if (Spec.Context != EPwModelOpContext::Part || Spec.FindParam(FString(TEXT("at"))) == nullptr)
        {
            continue;
        }

        if (Spec.Name == TEXT("transform"))
        {
            TestFalse(TEXT("'transform' remains a modifier, not an aimable generator"), Spec.bGenerator);
            for (const TCHAR* Name : { TEXT("from"), TEXT("to"), TEXT("up") })
            {
                TestFalse(*FString::Printf(TEXT("'transform' does not publish ignored aim parameter '%s'"), Name),
                    Spec.FindParam(FString(Name)) != nullptr);
            }
            continue;
        }

        if (!Spec.bGenerator)
        {
            continue;
        }

        ++Checked;
        for (const TCHAR* Name : { TEXT("from"), TEXT("to"), TEXT("up") })
        {
            TestTrue(*FString::Printf(TEXT("'%s' publishes '%s'"), *Spec.Name, Name),
                Spec.FindParam(FString(Name)) != nullptr);
        }

        // The aim kind and the parameter it names must agree with each other and with the op's
        // real parameter list, or describe_ops publishes a derivation that cannot happen.
        if (Spec.AimExtent == EPwModelAimExtent::None)
        {
            TestTrue(*FString::Printf(TEXT("'%s' names no extent parameter when it has no extent"), *Spec.Name),
                Spec.AimExtentParam.IsEmpty());
        }
        else
        {
            TestTrue(*FString::Printf(TEXT("'%s' names the parameter its aim writes"), *Spec.Name),
                !Spec.AimExtentParam.IsEmpty() && Spec.FindParam(Spec.AimExtentParam) != nullptr);
        }
    }

    TestTrue(TEXT("the sweep found the transform-capable generators at all"), Checked >= 10);

    // The five ops whose length an aim can set, pinned by name as well as by rule: this is the
    // list the format documents, and an op silently losing its aim kind would otherwise only
    // show up as a document that stopped resizing.
    struct FExpected
    {
        const TCHAR* Op;
        EPwModelAimExtent Extent;
        const TCHAR* Param;
    };
    const FExpected Expected[] = {
        { TEXT("box"),      EPwModelAimExtent::SizeZ,         TEXT("size") },
        { TEXT("cylinder"), EPwModelAimExtent::Scalar,        TEXT("height") },
        { TEXT("cone"),     EPwModelAimExtent::Scalar,        TEXT("height") },
        { TEXT("pipe"),     EPwModelAimExtent::Scalar,        TEXT("height") },
        { TEXT("capsule"),  EPwModelAimExtent::CapsuleLength, TEXT("length") },
    };

    for (const FExpected& Entry : Expected)
    {
        const FPwModelOpSpec* Spec = PwModelOpTable::Find(FString(Entry.Op), EPwModelOpContext::Part);
        if (!Spec)
        {
            AddError(FString::Printf(TEXT("no part-context op named '%s'"), Entry.Op));
            continue;
        }
        TestTrue(*FString::Printf(TEXT("'%s' keeps its aim kind"), Entry.Op),
            Spec->AimExtent == Entry.Extent);
        TestEqual(*FString::Printf(TEXT("'%s' aims into the parameter it always has"), Entry.Op),
            Spec->AimExtentParam, FString(Entry.Param));
    }

    // A sphere is the control: transform-capable, aimable, and with nothing for the distance to
    // set. If this ever reads as an extent-bearing op the warning above it goes silent.
    if (const FPwModelOpSpec* Sphere = PwModelOpTable::Find(FString(TEXT("sphere")), EPwModelOpContext::Part))
    {
        TestTrue(TEXT("a sphere has no extent along its local Z"),
            Sphere->AimExtent == EPwModelAimExtent::None);
    }

    return true;
}
