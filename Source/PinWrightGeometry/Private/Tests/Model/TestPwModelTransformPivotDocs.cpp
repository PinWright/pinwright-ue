// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the ONE thing the `transform` op's text used to leave out: its `rotate=`
// and `scale=` pivot about the PART-LOCAL ORIGIN, not about the geometry.
//
// The op builds one FTransform - scale, then rotation, then translation, all about (0, 0, 0) -
// and TransformMesh applies it to every accumulated vertex, so geometry authored away from the
// origin is DISPLACED as well as turned, by a lever arm of distance * sin(angle). A boolean tool
// authored at z = 1790 and tilted 7 degrees therefore moves 1790 * sin(7) = 218 uu sideways,
// cuts the wrong material, and passes the ENTIRE documented health gate: isClosed, positive
// signedVolume, zero boundary edges, zero degenerates, zero non-manifold vertices. Only
// `floatingGeometry` caught the reference case, and only because that particular miss happened to
// detach something; a tool displaced onto another part of the same solid leaves no signal at all.
//
// The remedy is published text rather than a warning, for the same reason the sphere floor is
// (see TestPwModelSphereExtentDocs.cpp): swinging accumulated geometry about the part origin is a
// correct and deliberate use of this op - radial placement is spelled exactly that way - so a
// "displacement is large" heuristic would fire on correct documents, and there is a correct
// in-format spelling of the in-place case already (`rotate=` on the generator, which composes into
// the primitive BEFORE its `at=`).
//
// This test is what keeps that text honest. It MEASURES both spellings through the compiler
// first and asserts the published description quotes the displacement it measured, so the doc
// cannot drift from the geometry in either direction - a reword is free, a dropped or wrong
// number fails here. It also pins that the named remedy actually holds still, so the text can
// never recommend a spelling with the same lever arm.
//
// Why here and not in Tests/Geometry: the geometry side is correct and covered - the op transforms
// exactly what its contract says - and no test in that tree reads the op table, so the pivot could
// vanish from the published description with the whole geometry suite green.
#include "Misc/AutomationTest.h"

#include "Model/PwModelCompiler.h"
#include "Model/PwModelParser.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// The reference case from the field report, verbatim in its two numbers: a tool authored high
// above the part origin, tilted a few degrees. The lever arm grows with the height, so the height
// is the number that has to be real rather than a token 1.
constexpr double TransformPivotDocsTest_Height = 1790.0;
constexpr double TransformPivotDocsTest_TiltDegrees = 7.0;

// The bounding-box centre of a one-part document, or a zero vector after reporting the failure.
// A box is symmetric about its own centre at any orientation, so its AABB centre IS the box
// centre - which is what makes "how far did the op move it" answerable from `bounds` alone.
FVector TransformPivotDocsTest_CenterOf(FAutomationTestBase& Test, const TCHAR* Label, FStringView Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(Source, Options);
    if (!Result.bSuccess || !Result.MeshBounds.IsValid)
    {
        Test.AddError(FString::Printf(TEXT("%s did not compile to measurable geometry"), Label));
        return FVector::ZeroVector;
    }
    return Result.MeshBounds.GetCenter();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelTransformPivotDocTest,
    "PinWright.Model.Parser.TransformDocPublishesTheOriginPivotAndItsLeverArm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelTransformPivotDocTest::RunTest(const FString& Parameters)
{
    // ---- the geometry, measured ------------------------------------------------------------

    // `transform rotate=` after the box is placed: the pivot is the part-local origin, 1790 uu
    // below the box, so the box swings.
    const FVector Swung = TransformPivotDocsTest_CenterOf(*this, TEXT("the `transform rotate=` document"),
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(700, 700, 700) at=(0, 0, 1790)\n")
        TEXT("    transform rotate=(0, 7, 0)\n")
        TEXT("}\n"));

    // The remedy the text names: `rotate=` on the generator itself, which composes into the
    // primitive BEFORE its `at=`, so the box turns about its own centre and stays put.
    const FVector InPlace = TransformPivotDocsTest_CenterOf(*this, TEXT("the generator `rotate=` document"),
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(700, 700, 700) rotate=(0, 7, 0) at=(0, 0, 1790)\n")
        TEXT("}\n"));

    if (Swung.IsZero() || InPlace.IsZero())
    {
        return false;
    }

    // Horizontal displacement, taken as a magnitude so the assertion does not depend on which way
    // a positive pitch swings a point on +Z.
    const double Displacement = FVector2D(Swung.X, Swung.Y).Size();
    const double Expected =
        TransformPivotDocsTest_Height * FMath::Sin(FMath::DegreesToRadians(TransformPivotDocsTest_TiltDegrees));

    TestTrue(FString::Printf(TEXT(
            "`transform rotate=` displaces by distance * sin(angle) = %.3f uu: measured %.3f"),
            Expected, Displacement),
        FMath::IsNearlyEqual(Displacement, Expected, 0.01));

    // The remedy has to be a remedy. If this ever failed, the published advice would be one more
    // spelling of the same lever arm.
    TestTrue(FString::Printf(TEXT(
            "the generator's own `rotate=` turns the box in place: centre measured (%.3f, %.3f, %.3f)"),
            InPlace.X, InPlace.Y, InPlace.Z),
        FMath::IsNearlyEqual(FVector2D(InPlace.X, InPlace.Y).Size(), 0.0, 0.01)
            && FMath::IsNearlyEqual(InPlace.Z, TransformPivotDocsTest_Height, 0.01));

    // ---- the text, against that measurement ------------------------------------------------

    const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(TEXT("transform")), EPwModelOpContext::Part);
    if (!TestNotNull(TEXT("'transform' is a .pwmodel part op"), Op))
    {
        return false;
    }

    const FString& OpDoc = Op->Description;

    // The pivot itself - the fact an author's "tilt the tool" mental model is missing.
    TestTrue(FString::Printf(TEXT("the 'transform' description names the part-local origin as the pivot. Text: %s"), *OpDoc),
        OpDoc.Contains(TEXT("PART-LOCAL ORIGIN")));

    // The lever arm in closed form, so the reader can apply it to their own height rather than
    // recognising one anecdote.
    TestTrue(FString::Printf(TEXT("and publishes the lever arm as distance * sin(angle). Text: %s"), *OpDoc),
        OpDoc.Contains(TEXT("distance * sin(angle)")));

    // And the in-place alternative, measured above. Naming the trap without the remedy leaves the
    // author with nothing to write instead.
    TestTrue(FString::Printf(TEXT("and names the generator's own rotate= as the in-place spelling. Text: %s"), *OpDoc),
        OpDoc.Contains(TEXT("rotate=")) && OpDoc.Contains(TEXT("generator")));

    const FPwModelParamSpec* Rotate = Op->FindParam(FString(TEXT("rotate")));
    if (!TestNotNull(TEXT("'transform' publishes a 'rotate' parameter"), Rotate))
    {
        return false;
    }

    const FString& RotateDoc = Rotate->Description;

    TestTrue(FString::Printf(TEXT("the 'rotate' text names the part-local origin as the pivot. Text: %s"), *RotateDoc),
        RotateDoc.Contains(TEXT("PART-LOCAL ORIGIN")));

    // The worked number is FORMATTED FROM THE MEASUREMENT, never typed in: a text quoting a
    // displacement the op does not produce fails here exactly as a text that quotes none.
    const FString MeasuredDisplacement = FString::Printf(TEXT("%d uu"), FMath::RoundToInt(Displacement));
    TestTrue(FString::Printf(TEXT("and quotes the measured displacement %s for the z=%d, %d-degree case. Text: %s"),
            *MeasuredDisplacement, FMath::RoundToInt(TransformPivotDocsTest_Height),
            FMath::RoundToInt(TransformPivotDocsTest_TiltDegrees), *RotateDoc),
        RotateDoc.Contains(MeasuredDisplacement)
            && RotateDoc.Contains(FString::Printf(TEXT("%d"), FMath::RoundToInt(TransformPivotDocsTest_Height))));

    // `scale=` carries the same lever arm for the same reason - one FTransform, one origin - and
    // it is the half of the trap nobody goes looking for.
    const FPwModelParamSpec* Scale = Op->FindParam(FString(TEXT("scale")));
    if (!TestNotNull(TEXT("'transform' publishes a 'scale' parameter"), Scale))
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("the 'scale' text names the same origin pivot. Text: %s"), *Scale->Description),
        Scale->Description.Contains(TEXT("PART-LOCAL ORIGIN")));

    return true;
}
