// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the reported `bounds` describing geometry that is actually THERE.
//
// THE DEFECT: the box was reduced over the VERTEX BUFFER. FDynamicMesh3::GetBounds is
// documented "Computes bounding box of all vertices" and loops VertexIndicesItr() with no
// reference to triangles, so a vertex still allocated but referenced by no triangle votes on
// the answer. A cut can leave one behind: live, isolated, and part of no surface.
//
// Measured on the shape the second test builds: an 8-gon prism (r=100, z 0..200) written as
// `revolve` and cut at z=150 by a `subtract` reported bounds.max.z = 200, while its
// signedVolume was exactly the analytic volume of the same prism at height 150 and its health
// block read closed / 0 boundary edges / 0 degenerates / 0 bowties. The identical solid
// written with `cylinder` reported 150. Both meshes were the same cut solid; only the box
// differed. Only `revolve` produces it because only `revolve` takes an author-supplied profile
// that can have a point ON the revolution axis, and the engine's profile sweep welds such a
// point to one shared vertex (SweepGenerator.h:268-270) - the apex the cut then strands.
//
// It is the persuasive kind of wrong answer: 200 is exactly the extent the author asked the
// generator for, so it does not read as a bad measurement - it reads as "the boolean did
// nothing", and sends the author to debug a boolean that worked. `model.authoring` prescribes
// this box as the check that lets a model be fitted to a size WITHOUT writing an asset per
// iteration, so it is measured on the surface that creates nothing and has to be right there.
//
// The two tests attack it from opposite ends, and both are needed:
//
//  - AnUnreferencedVertexDoesNotWidenTheReportedBox is DETERMINISTIC and needs no boolean at
//    all. `append_buffers` appends every vertex it is handed and then the triangles, so a
//    vertex no triangle names is orphaned by construction - the op's own source says so
//    ("the orphaned vertices still counted in appendedVertices"). Two documents that differ
//    in exactly one unreferenced vertex must report the same box. This one cannot depend on
//    how any engine version happens to implement a cut.
//  - ACutRevolveReportsTheCutExtentLikeACutCylinder is the reported shape itself, written as
//    a DIFFERENCE between the two spellings of one solid. The `cylinder` half passed before
//    the fix and is the control: it is what proves the pair used to disagree about a mesh
//    they agree on in every other field.
//
// Driven through the real dispatcher rather than FPwModelCompiler directly: `bounds` is a
// WIRE contract on `model.validate` / `model.compile`, and a compiler that measures a box the
// handler never marshals is the same blindness one layer up.
#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::FSinkPtr;

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.

struct FPwModelBoundsTest_Run
{
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
};

// One model.validate run. diagnosticLimit=0 and collapseDiagnostics=false on purpose: the
// defaults can drop or fold the diagnostics a failure message needs to be readable.
FPwModelBoundsTest_Run PwModelBoundsTest_Validate(const TCHAR* Source)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), Source);
    Payload->SetNumberField(TEXT("diagnosticLimit"), 0);
    Payload->SetBoolField(TEXT("collapseDiagnostics"), false);

    FPwModelBoundsTest_Run Run;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-bounds"),
        Payload, Run.bSuccess, Run.Result, Run.ErrorCode);
    return Run;
}

// Every failure message carries the diagnostics and the box that was actually reported;
// "expected 150, got 200" on a compiler test costs a rerun under a debugger otherwise.
FString PwModelBoundsTest_Describe(const FPwModelBoundsTest_Run& Run)
{
    if (!Run.Result.IsValid())
    {
        return FString::Printf(TEXT("success=%d error='%s' <no result>"),
            Run.bSuccess ? 1 : 0, *Run.ErrorCode);
    }

    TArray<FString> Lines;
    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    if (Run.Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry).IsValid())
            {
                Lines.Add(FString::Printf(TEXT("%s [%s] line %d: %s"),
                    *(*Entry)->GetStringField(TEXT("severity")),
                    *(*Entry)->GetStringField(TEXT("code")),
                    static_cast<int32>((*Entry)->GetNumberField(TEXT("line"))),
                    *(*Entry)->GetStringField(TEXT("message"))));
            }
        }
    }

    FString BoxText = TEXT("<no bounds>");
    const TSharedPtr<FJsonObject>* Bounds = nullptr;
    if (Run.Result->TryGetObjectField(TEXT("bounds"), Bounds) && (*Bounds).IsValid())
    {
        const TSharedPtr<FJsonObject>* Min = nullptr;
        const TSharedPtr<FJsonObject>* Max = nullptr;
        if ((*Bounds)->TryGetObjectField(TEXT("min"), Min)
            && (*Bounds)->TryGetObjectField(TEXT("max"), Max))
        {
            BoxText = FString::Printf(TEXT("min=(%g, %g, %g) max=(%g, %g, %g)"),
                (*Min)->GetNumberField(TEXT("x")), (*Min)->GetNumberField(TEXT("y")),
                (*Min)->GetNumberField(TEXT("z")), (*Max)->GetNumberField(TEXT("x")),
                (*Max)->GetNumberField(TEXT("y")), (*Max)->GetNumberField(TEXT("z")));
        }
    }

    return FString::Printf(TEXT("success=%d error='%s' bounds={%s} triangles=%g vertices=%g diagnostics=[%s]"),
        Run.bSuccess ? 1 : 0, *Run.ErrorCode, *BoxText,
        Run.Result->GetNumberField(TEXT("meshTriangleCount")),
        Run.Result->GetNumberField(TEXT("meshVertexCount")),
        *FString::Join(Lines, TEXT("; ")));
}

// The `parts` entry at this index, or null. Read by index rather than by name because these
// documents have one part each and the index is what the failure message can name.
TSharedPtr<FJsonObject> PwModelBoundsTest_Part(const FPwModelBoundsTest_Run& Run, int32 Index)
{
    const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
    if (!Run.Result.IsValid() || !Run.Result->TryGetArrayField(TEXT("parts"), Parts) || !Parts
        || !Parts->IsValidIndex(Index))
    {
        return nullptr;
    }
    const TSharedPtr<FJsonObject>* Entry = nullptr;
    if ((*Parts)[Index].IsValid() && (*Parts)[Index]->TryGetObject(Entry))
    {
        return *Entry;
    }
    return nullptr;
}

// Reads the `bounds` block off a result or a part entry. FAILS the test when any level is
// missing rather than defaulting to zero: 0 is a legal coordinate, so a defaulting read would
// turn "the field is gone" into "the box sits at the origin", which is the same class of lie
// this file exists to catch.
bool PwModelBoundsTest_ReadBox(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Owner,
                               const TCHAR* Where, FVector& OutMin, FVector& OutMax)
{
    const TSharedPtr<FJsonObject>* Bounds = nullptr;
    if (!Test.TestTrue(*FString::Printf(TEXT("%s carries a 'bounds' block"), Where),
            Owner.IsValid() && Owner->TryGetObjectField(TEXT("bounds"), Bounds)
                && (*Bounds).IsValid()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Min = nullptr;
    const TSharedPtr<FJsonObject>* Max = nullptr;
    if (!Test.TestTrue(*FString::Printf(TEXT("%s bounds carries 'min' and 'max'"), Where),
            (*Bounds)->TryGetObjectField(TEXT("min"), Min) && (*Min).IsValid()
                && (*Bounds)->TryGetObjectField(TEXT("max"), Max) && (*Max).IsValid()))
    {
        return false;
    }

    OutMin = FVector((*Min)->GetNumberField(TEXT("x")), (*Min)->GetNumberField(TEXT("y")),
                     (*Min)->GetNumberField(TEXT("z")));
    OutMax = FVector((*Max)->GetNumberField(TEXT("x")), (*Max)->GetNumberField(TEXT("y")),
                     (*Max)->GetNumberField(TEXT("z")));
    return true;
}

// Reads `health.unreferencedVertices` off a result. FAILS when the block or the field is absent
// rather than defaulting to zero, for the same reason PwModelBoundsTest_ReadBox refuses to
// default a coordinate: 0 is the answer a clean mesh gives, so a defaulting read would turn "the
// field was never emitted" into "there are no orphans" and pass against a wire contract that does
// not exist.
bool PwModelBoundsTest_ReadOrphanCount(FAutomationTestBase& Test,
                                       const FPwModelBoundsTest_Run& Run, const TCHAR* Where,
                                       double& OutCount)
{
    const TSharedPtr<FJsonObject>* Health = nullptr;
    if (!Test.TestTrue(*FString::Printf(TEXT("%s carries a 'health' block"), Where),
            Run.Result.IsValid() && Run.Result->TryGetObjectField(TEXT("health"), Health)
                && (*Health).IsValid()))
    {
        return false;
    }

    if (!Test.TestTrue(
            *FString::Printf(TEXT("%s health carries 'unreferencedVertices'"), Where),
            (*Health)->HasTypedField<EJson::Number>(TEXT("unreferencedVertices"))))
    {
        return false;
    }

    OutCount = (*Health)->GetNumberField(TEXT("unreferencedVertices"));
    return true;
}

// ---------------------------------------------------------------------------------------
// Documents.
// ---------------------------------------------------------------------------------------

// Two triangles over four corners of a 100 x 100 square in the z=0 plane.
const TCHAR* const PwModelBoundsTest_Sheet =
    TEXT("pwmodel 0\n")
    TEXT("part sheet {\n")
    TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)] ")
    TEXT("triangles=[(0, 1, 2), (0, 2, 3)]\n")
    TEXT("}\n");

// The SAME two triangles over the same four corners, plus a fifth vertex 1000 units up that
// no triangle names. append_buffers appends every vertex it is handed and then the triangles,
// so index 4 lands in the mesh orphaned - the state a boolean leaves behind, built without
// one. Nothing about the SURFACE differs between this document and the one above.
const TCHAR* const PwModelBoundsTest_SheetWithOrphan =
    TEXT("pwmodel 0\n")
    TEXT("part sheet {\n")
    TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0), (0, 0, 1000)] ")
    TEXT("triangles=[(0, 1, 2), (0, 2, 3)]\n")
    TEXT("}\n");

// An 8-gon prism, r=100, spanning z 0..200, with the part above z=150 cut away. The profile has
// points ON the revolution axis at (0, 0) and (0, 200); the engine's profile sweep welds such a
// point to a single shared vertex (SweepGenerator.h:268-270), so each end closes with a fan
// around one apex, and something about the cut leaves the upper apex at z=200 behind.
const TCHAR* const PwModelBoundsTest_CutRevolve =
    TEXT("pwmodel 0\n")
    TEXT("part solid {\n")
    TEXT("    revolve steps=8 profile=[(0, 0), (100, 0), (100, 200), (0, 200)]\n")
    TEXT("    subtract { box size=(400, 400, 200) at=(0, 0, 250) }\n")
    TEXT("}\n");

// The SAME solid and the SAME cut, spelled with the generator that has no on-axis apex. This
// half reported the cut extent before the fix and is the control: the two documents describe
// one solid, so a box they disagree on is a measurement fault and nothing else.
const TCHAR* const PwModelBoundsTest_CutCylinder =
    TEXT("pwmodel 0\n")
    TEXT("part solid {\n")
    TEXT("    cylinder radius=100 height=200 segments=8 at=(0, 0, 100)\n")
    TEXT("    subtract { box size=(400, 400, 200) at=(0, 0, 250) }\n")
    TEXT("}\n");
}

// ============================================================================
// An orphaned vertex is not geometry, and must not widen the box
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelOrphanVertexBoundsTest,
    "PinWright.Model.Bounds.AnUnreferencedVertexDoesNotWidenTheReportedBox",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelOrphanVertexBoundsTest::RunTest(const FString& Parameters)
{
    constexpr double Tol = 0.001;

    const FPwModelBoundsTest_Run Plain = PwModelBoundsTest_Validate(PwModelBoundsTest_Sheet);
    const FPwModelBoundsTest_Run Orphaned =
        PwModelBoundsTest_Validate(PwModelBoundsTest_SheetWithOrphan);

    if (!TestTrue(*FString::Printf(TEXT("the plain sheet validates. %s"),
            *PwModelBoundsTest_Describe(Plain)), Plain.bSuccess))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("the sheet with an unreferenced vertex validates. %s"),
            *PwModelBoundsTest_Describe(Orphaned)), Orphaned.bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("both runs carry a result object"),
            Plain.Result.IsValid() && Orphaned.Result.IsValid()))
    {
        return false;
    }

    // The premise: the extra vertex really did reach the mesh. Without this the test could
    // pass because append_buffers dropped it, which proves nothing about the reduction.
    TestEqual(*FString::Printf(TEXT("the extra vertex is in the mesh. %s"),
            *PwModelBoundsTest_Describe(Orphaned)),
        Orphaned.Result->GetNumberField(TEXT("meshVertexCount")),
        Plain.Result->GetNumberField(TEXT("meshVertexCount")) + 1.0, Tol);

    // And that it added no surface: same triangles, so the two documents describe the same
    // solid and any difference in the box is a measurement difference.
    TestEqual(*FString::Printf(TEXT("and it added no triangle. %s"),
            *PwModelBoundsTest_Describe(Orphaned)),
        Orphaned.Result->GetNumberField(TEXT("meshTriangleCount")),
        Plain.Result->GetNumberField(TEXT("meshTriangleCount")), Tol);

    FVector PlainMin, PlainMax;
    FVector OrphanMin, OrphanMax;
    if (!PwModelBoundsTest_ReadBox(*this, Plain.Result, TEXT("the plain sheet"), PlainMin, PlainMax)
        || !PwModelBoundsTest_ReadBox(*this, Orphaned.Result, TEXT("the orphaned sheet"),
               OrphanMin, OrphanMax))
    {
        return false;
    }

    // The sheet is flat at z=0, so the orphan at z=1000 is the whole difference between a box
    // measured over triangles and one measured over the vertex buffer.
    TestEqual(TEXT("the plain sheet's box is the square it describes"), PlainMax.Z, 0.0, Tol);
    TestEqual(*FString::Printf(TEXT("and the unreferenced vertex does not raise the ceiling. %s"),
            *PwModelBoundsTest_Describe(Orphaned)), OrphanMax.Z, 0.0, Tol);
    TestEqual(TEXT("nor lower the floor"), OrphanMin.Z, PlainMin.Z, Tol);
    TestEqual(TEXT("the x span is untouched"), OrphanMax.X, PlainMax.X, Tol);
    TestEqual(TEXT("the y span is untouched"), OrphanMax.Y, PlainMax.Y, Tol);

    // The per-part box is a SECOND reduction over the same mesh, emitted from its own call
    // site. A fix applied to only one of the two leaves the other lying.
    const TSharedPtr<FJsonObject> OrphanPart = PwModelBoundsTest_Part(Orphaned, 0);
    FVector PartMin, PartMax;
    if (PwModelBoundsTest_ReadBox(*this, OrphanPart, TEXT("part 'sheet'"), PartMin, PartMax))
    {
        TestEqual(*FString::Printf(TEXT("the per-part box excludes it too. %s"),
                *PwModelBoundsTest_Describe(Orphaned)), PartMax.Z, 0.0, Tol);
    }

    return true;
}

// ============================================================================
// The reported shape: a cut revolve must measure like a cut cylinder
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCutRevolveBoundsTest,
    "PinWright.Model.Bounds.ACutRevolveReportsTheCutExtentLikeACutCylinder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCutRevolveBoundsTest::RunTest(const FString& Parameters)
{
    // Three orders of magnitude below the 50-unit error under test, and well above the
    // residue a boolean leaves on an axis-aligned cut plane.
    constexpr double Tol = 0.05;
    constexpr double CutZ = 150.0;

    const FPwModelBoundsTest_Run Revolved =
        PwModelBoundsTest_Validate(PwModelBoundsTest_CutRevolve);
    const FPwModelBoundsTest_Run Cylindrical =
        PwModelBoundsTest_Validate(PwModelBoundsTest_CutCylinder);

    // A boolean that removes nothing is an ERROR in this compiler, so success here is also
    // the assertion that both cuts actually happened.
    if (!TestTrue(*FString::Printf(TEXT("the cut revolve validates. %s"),
            *PwModelBoundsTest_Describe(Revolved)), Revolved.bSuccess))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("the cut cylinder validates. %s"),
            *PwModelBoundsTest_Describe(Cylindrical)), Cylindrical.bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("both runs carry a result object"),
            Revolved.Result.IsValid() && Cylindrical.Result.IsValid()))
    {
        return false;
    }

    FVector RevolvedMin, RevolvedMax;
    FVector CylinderMin, CylinderMax;
    if (!PwModelBoundsTest_ReadBox(*this, Revolved.Result, TEXT("the cut revolve"),
            RevolvedMin, RevolvedMax)
        || !PwModelBoundsTest_ReadBox(*this, Cylindrical.Result, TEXT("the cut cylinder"),
               CylinderMin, CylinderMax))
    {
        return false;
    }

    // The control, which passed before the fix.
    TestEqual(*FString::Printf(TEXT("the cut cylinder reports the cut extent. %s"),
            *PwModelBoundsTest_Describe(Cylindrical)), CylinderMax.Z, CutZ, Tol);

    // The defect: this read 200 - the uncut extent, and exactly the height the generator was
    // asked for, which is what made it read as a boolean that did nothing.
    TestEqual(*FString::Printf(TEXT("the cut revolve reports the cut extent too. %s"),
            *PwModelBoundsTest_Describe(Revolved)), RevolvedMax.Z, CutZ, Tol);

    // Stated as an agreement as well as an absolute, because the invariant the ticket broke
    // is that two spellings of one solid must measure the same.
    TestEqual(TEXT("the two spellings agree on the ceiling"), RevolvedMax.Z, CylinderMax.Z, Tol);
    TestEqual(TEXT("and on the floor"), RevolvedMin.Z, CylinderMin.Z, Tol);

    // The per-part box comes from its own reduction over the same mesh; assert it separately
    // so a fix applied to only one of the two emission sites is caught.
    const TSharedPtr<FJsonObject> RevolvedPart = PwModelBoundsTest_Part(Revolved, 0);
    FVector PartMin, PartMax;
    if (PwModelBoundsTest_ReadBox(*this, RevolvedPart, TEXT("part 'solid'"), PartMin, PartMax))
    {
        TestEqual(*FString::Printf(TEXT("the per-part box is cut too. %s"),
                *PwModelBoundsTest_Describe(Revolved)), PartMax.Z, CutZ, Tol);
    }

    return true;
}

// ============================================================================
// The orphan the box excludes has to be nameable
// ============================================================================
//
// The box is reduced over TRIANGLES, so an unreferenced vertex cannot widen it, while
// `meshVertexCount` is the vertex buffer and still counts it. Two fields of one response
// therefore describe the same mesh and disagree about what is in it - correctly, and with
// nothing naming the difference until `health.unreferencedVertices`. This asserts the
// reconciling number is emitted and is right, on the same pair of documents the first test
// uses: they differ in exactly one vertex that no triangle names.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelOrphanVertexHealthTest,
    "PinWright.Model.Bounds.TheOrphanTheBoxExcludesIsCountedOnTheHealthBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelOrphanVertexHealthTest::RunTest(const FString& Parameters)
{
    constexpr double Tol = 0.001;

    const FPwModelBoundsTest_Run Plain = PwModelBoundsTest_Validate(PwModelBoundsTest_Sheet);
    const FPwModelBoundsTest_Run Orphaned =
        PwModelBoundsTest_Validate(PwModelBoundsTest_SheetWithOrphan);

    if (!TestTrue(*FString::Printf(TEXT("the plain sheet validates. %s"),
            *PwModelBoundsTest_Describe(Plain)), Plain.bSuccess))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("the sheet with an unreferenced vertex validates. %s"),
            *PwModelBoundsTest_Describe(Orphaned)), Orphaned.bSuccess))
    {
        return false;
    }

    double PlainOrphans = -1.0;
    double OrphanedOrphans = -1.0;
    if (!PwModelBoundsTest_ReadOrphanCount(*this, Plain, TEXT("the plain sheet"), PlainOrphans)
        || !PwModelBoundsTest_ReadOrphanCount(*this, Orphaned, TEXT("the orphaned sheet"),
               OrphanedOrphans))
    {
        return false;
    }

    // The control. A sheet every one of whose vertices carries a triangle must report zero, or
    // the field is counting something else and its value on the document below means nothing.
    TestEqual(*FString::Printf(TEXT("the plain sheet reports no orphan. %s"),
            *PwModelBoundsTest_Describe(Plain)), PlainOrphans, 0.0, Tol);

    // The signal itself: one vertex, named by no triangle, counted as exactly one.
    TestEqual(*FString::Printf(TEXT("the unreferenced vertex is counted. %s"),
            *PwModelBoundsTest_Describe(Orphaned)), OrphanedOrphans, 1.0, Tol);

    // And it accounts for the whole discrepancy it exists to explain: the two documents describe
    // the same surface, so every vertex `meshVertexCount` has over the plain sheet's is an orphan.
    TestEqual(*FString::Printf(TEXT("and it accounts for the extra meshVertexCount. %s"),
            *PwModelBoundsTest_Describe(Orphaned)),
        Orphaned.Result->GetNumberField(TEXT("meshVertexCount"))
            - Plain.Result->GetNumberField(TEXT("meshVertexCount")),
        OrphanedOrphans - PlainOrphans, Tol);

    return true;
}
