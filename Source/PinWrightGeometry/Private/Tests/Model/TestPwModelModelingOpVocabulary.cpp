// Copyright (c) 2026 Alexander Penkin. MIT License.

// The .pwmodel half of the modeling / warp / repair op widening: the parameters those ops gained
// must be IN THE OP TABLE, not merely readable by the compiler.
//
// Sibling file, DIFFERENT ops: Tests/Model/TestPwModelWidenedOpVocabulary.cpp covers
// simplify_mesh, remesh_uniform, `uv` and the four boolean ops. Nothing is asserted twice.
//
// Why this exists separately from Tests/Geometry/TestGeometryModelingOpOptions.cpp: the compiler
// reads a parameter by name out of a TMap, so a field it reads but the table does not declare is
// unreachable in the opposite direction - the PARSER rejects the document with
// PWSRC_UNKNOWN_PARAM before the compiler ever sees it, while the op-level test still passes
// because it calls the op directly. The table is also exactly what model.describe_ops emits
// (ModelCompileHandler.cpp iterates PwModelOpTable::Get() and serializes each FPwModelParamSpec
// whole), so asserting the table is asserting the published vocabulary.
//
// The DELIBERATE OMISSIONS are asserted too, with TestNull. That is the half that stops the next
// pass from "completing" the widening by re-deriving it from a field count: every one of them is
// a field that would reach the engine and provably change nothing, or one the format cannot spell.
#include "Misc/AutomationTest.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

const FPwModelOpSpec* ModelingVocabTest_FindOp(FAutomationTestBase& Test, const TCHAR* OpName)
{
    const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(OpName), EPwModelOpContext::Part);
    if (!Op)
    {
        Test.AddError(FString::Printf(TEXT("op '%s' is not in the part-context op table"), OpName));
    }
    return Op;
}

void ModelingVocabTest_ExpectParam(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName,
    EPwModelParamType ExpectedType, const TCHAR* ExpectedDefault)
{
    const FPwModelOpSpec* Op = ModelingVocabTest_FindOp(Test, OpName);
    if (!Op)
    {
        return;
    }

    const FPwModelParamSpec* Param = Op->FindParam(FString(ParamName));
    if (!Param)
    {
        Test.AddError(FString::Printf(
            TEXT("'%s' publishes no parameter named '%s' - the compiler may read it, but the parser "
                 "rejects any document that sets it"), OpName, ParamName));
        return;
    }

    Test.TestTrue(*FString::Printf(TEXT("%s %s has type %s, not %s"),
            OpName, ParamName, PwModelParamTypeToString(ExpectedType),
            PwModelParamTypeToString(Param->Type)),
        Param->Type == ExpectedType);

    // The published default is display text, and it is the only place an author reads what
    // omitting the parameter does - a stale one is a documentation bug with no other symptom.
    Test.TestEqual(*FString::Printf(TEXT("%s %s publishes default '%s'"), OpName, ParamName, ExpectedDefault),
        Param->Default, FString(ExpectedDefault));
}

void ModelingVocabTest_ExpectEnum(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName,
    const TCHAR* ExpectedDefault, const TArray<FString>& ExpectedValues)
{
    const FPwModelOpSpec* Op = ModelingVocabTest_FindOp(Test, OpName);
    if (!Op)
    {
        return;
    }

    const FPwModelParamSpec* Param = Op->FindParam(FString(ParamName));
    if (!Param)
    {
        Test.AddError(FString::Printf(TEXT("'%s' publishes no parameter named '%s'"), OpName, ParamName));
        return;
    }

    Test.TestTrue(*FString::Printf(TEXT("%s %s is an enum"), OpName, ParamName),
        Param->Type == EPwModelParamType::Enum);
    Test.TestEqual(*FString::Printf(TEXT("%s %s defaults to '%s'"), OpName, ParamName, ExpectedDefault),
        Param->Default, FString(ExpectedDefault));
    // TestTrue over a == rather than TestEqual: the automation framework has no TArray<FString>
    // overload, and the interesting half of the failure is WHICH list came back anyway.
    Test.TestTrue(*FString::Printf(TEXT("%s %s allows exactly [%s], not [%s]"),
            OpName, ParamName, *FString::Join(ExpectedValues, TEXT(", ")),
            *FString::Join(Param->AllowedValues, TEXT(", "))),
        Param->AllowedValues == ExpectedValues);
}

void ModelingVocabTest_ExpectNoParam(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName, const TCHAR* Why)
{
    const FPwModelOpSpec* Op = ModelingVocabTest_FindOp(Test, OpName);
    if (!Op)
    {
        return;
    }
    Test.TestNull(*FString::Printf(TEXT("%s deliberately does not publish %s: %s"), OpName, ParamName, Why),
        Op->FindParam(FString(ParamName)));
}

// Parses Source and reports every diagnostic in the failure message, because "expected 0, got 3"
// costs a rerun under a debugger to learn anything.
void ModelingVocabTest_ExpectClean(FAutomationTestBase& Test, const TCHAR* What, const FString& Source)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(Source, Document, Diagnostics);

    TArray<FString> Reported;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        Reported.Add(FString::Printf(TEXT("%s@%d: %s"),
            *Diagnostic.Code, Diagnostic.Line, *Diagnostic.Message));
    }

    Test.TestTrue(*FString::Printf(TEXT("%s parses. Diagnostics: [%s]"),
            What, *FString::Join(Reported, TEXT(" | "))),
        bParsed && Diagnostics.Num() == 0);
}

void ModelingVocabTest_ExpectCode(
    FAutomationTestBase& Test, const TCHAR* What, const FString& Source, const TCHAR* Code)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(Source, Document, Diagnostics);

    bool bFound = false;
    TArray<FString> Reported;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        bFound |= (Diagnostic.Code == Code);
        Reported.Add(Diagnostic.Code);
    }

    Test.TestTrue(*FString::Printf(TEXT("%s reports %s. Got: [%s]"),
            What, Code, *FString::Join(Reported, TEXT(", "))),
        bFound);
}
}

// ============================================================================
// The four shared face-op parameters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelModelingFaceOpCommonVocabularyTest,
    "PinWright.Model.ModelingOps.FaceOpsPublishTheSharedEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelModelingFaceOpCommonVocabularyTest::RunTest(const FString& Parameters)
{
    // FGeometryScriptMeshEditPolygroupOptions is a NESTED struct in all three engine option
    // structs, and FPwValue has no map member - so it is flattened into two prefixed
    // scalars. Asserting the flattened spelling on all five ops at once is what keeps the five
    // from drifting into five spellings of the same engine field, which is the failure the single
    // AppendFaceOpCommonParams exists to prevent and which nothing else would notice.
    const TCHAR* const FaceOps[] = {
        TEXT("extrude"), TEXT("inset"), TEXT("outset"), TEXT("offset_faces"), TEXT("poke") };

    const TArray<FString> AreaValues{
        TEXT("entire_selection"), TEXT("per_polygroup"), TEXT("per_triangle") };
    const TArray<FString> GroupValues{
        TEXT("preserve_existing"), TEXT("auto_generate_new"), TEXT("set_constant") };

    for (const TCHAR* OpName : FaceOps)
    {
        ModelingVocabTest_ExpectEnum(*this, OpName, TEXT("area_mode"), TEXT("entire_selection"), AreaValues);
        ModelingVocabTest_ExpectEnum(*this, OpName, TEXT("group_mode"), TEXT("preserve_existing"), GroupValues);
        ModelingVocabTest_ExpectParam(*this, OpName, TEXT("group_id"), EPwModelParamType::Integer, TEXT("0"));
        ModelingVocabTest_ExpectParam(*this, OpName, TEXT("uv_scale"), EPwModelParamType::Number, TEXT("1"));

        // The nested-block spelling must NOT exist. If someone later adds a map member to
        // FPwValue, this is the line that says the flattening was a decision.
        ModelingVocabTest_ExpectNoParam(*this, OpName, TEXT("group"),
            TEXT("FPwValue has no map member, so the engine's nested group struct is flattened"));
    }

    ModelingVocabTest_ExpectClean(*this, TEXT("every face op setting all four shared options"),
        // One statement per LINE. A newline is a token in this grammar, so a widened op's
        // parameter list cannot be wrapped however long it gets - itself worth pinning, since a
        // widened op is exactly where someone reaches for a line break.
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  extrude distance=4 area_mode=per_polygroup group_mode=auto_generate_new uv_scale=2\n")
        TEXT("  inset distance=2 area_mode=per_triangle group_mode=set_constant group_id=7 uv_scale=0.5\n")
        TEXT("  outset distance=2 area_mode=per_polygroup uv_scale=3\n")
        TEXT("  offset_faces distance=2 area_mode=per_polygroup group_mode=auto_generate_new\n")
        TEXT("  poke offset=1 area_mode=per_polygroup group_id=2\n")
        TEXT("}\n"));

    ModelingVocabTest_ExpectCode(*this, TEXT("a misspelled area_mode"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  inset distance=2 area_mode=per_poly_group\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}

// ============================================================================
// extrude / inset / outset / offset_faces / poke - their own fields
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelModelingFaceOpOwnVocabularyTest,
    "PinWright.Model.ModelingOps.FaceOpsPublishTheirOwnEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelModelingFaceOpOwnVocabularyTest::RunTest(const FString& Parameters)
{
    const TArray<FString> ExtrudeDirectionValues{ TEXT("fixed_direction"), TEXT("average_face_normal") };
    const TArray<FString> OffsetTypeValues{
        TEXT("vertex_normal"), TEXT("face_normal"), TEXT("parallel_face_offset") };

    // extrude. direction_mode was HARDCODED to fixed_direction, so average_face_normal - the one
    // value that means "push each region along its own normal" - had no spelling at all.
    ModelingVocabTest_ExpectEnum(*this, TEXT("extrude"), TEXT("direction_mode"),
        TEXT("fixed_direction"), ExtrudeDirectionValues);
    ModelingVocabTest_ExpectParam(*this, TEXT("extrude"), TEXT("solids_to_shells"),
        EPwModelParamType::Bool, TEXT("true"));

    // inset. The default is the engine's own true, so publishing it changed nothing at the
    // default - which is the whole point: reproject=false was previously unreachable.
    ModelingVocabTest_ExpectParam(*this, TEXT("inset"), TEXT("reproject"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("inset"), TEXT("boundary_only"),
        EPwModelParamType::Bool, TEXT("false"));
    ModelingVocabTest_ExpectParam(*this, TEXT("inset"), TEXT("softness"),
        EPwModelParamType::Number, TEXT("0"));
    ModelingVocabTest_ExpectParam(*this, TEXT("inset"), TEXT("area_scale"),
        EPwModelParamType::Number, TEXT("1"));

    // outset carries the same engine struct MINUS reproject, and that asymmetry is the single
    // most likely thing for a later pass to "fix". ApplyMeshInsetOutsetFaces reads
    //   InsetOutset.bReproject = (Options.Distance < 0) ? false : Options.bReproject;
    // and outset always passes a negative distance, so the parameter would reach the engine and
    // provably change nothing - the exact defect class this widening exists to remove.
    ModelingVocabTest_ExpectNoParam(*this, TEXT("outset"), TEXT("reproject"),
        TEXT("the engine forces reprojection off for the negative distance outset always passes"));
    ModelingVocabTest_ExpectParam(*this, TEXT("outset"), TEXT("boundary_only"),
        EPwModelParamType::Bool, TEXT("false"));
    ModelingVocabTest_ExpectParam(*this, TEXT("outset"), TEXT("softness"),
        EPwModelParamType::Number, TEXT("0"));
    ModelingVocabTest_ExpectParam(*this, TEXT("outset"), TEXT("area_scale"),
        EPwModelParamType::Number, TEXT("1"));

    // offset_faces and poke share FGeometryScriptMeshOffsetFacesOptions; poke carries the PN
    // tessellation options on top.
    ModelingVocabTest_ExpectEnum(*this, TEXT("offset_faces"), TEXT("offset_type"),
        TEXT("parallel_face_offset"), OffsetTypeValues);
    ModelingVocabTest_ExpectParam(*this, TEXT("offset_faces"), TEXT("solids_to_shells"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectEnum(*this, TEXT("poke"), TEXT("offset_type"),
        TEXT("parallel_face_offset"), OffsetTypeValues);
    ModelingVocabTest_ExpectParam(*this, TEXT("poke"), TEXT("solids_to_shells"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("poke"), TEXT("recompute_normals"),
        EPwModelParamType::Bool, TEXT("true"));

    ModelingVocabTest_ExpectClean(*this, TEXT("the face ops setting every option of their own"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  extrude distance=4 direction=(1, 0, 0) direction_mode=average_face_normal solids_to_shells=false face_direction=(0, 0, 1) face_angle_tolerance=20\n")
        TEXT("  inset distance=2 reproject=false boundary_only=true softness=0.5 area_scale=0.25\n")
        TEXT("  outset distance=2 boundary_only=true softness=0.5 area_scale=0.25\n")
        TEXT("  offset_faces distance=2 offset_type=vertex_normal solids_to_shells=false\n")
        TEXT("  poke offset=1 offset_type=face_normal solids_to_shells=false recompute_normals=false\n")
        TEXT("}\n"));

    // The gate is real, which is what makes every "publishes it" assertion above worth making.
    ModelingVocabTest_ExpectCode(*this, TEXT("reproject on outset"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  outset distance=2 reproject=false\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);

    return true;
}

// ============================================================================
// bevel / shell / subdivide
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelModelingBevelShellVocabularyTest,
    "PinWright.Model.ModelingOps.BevelShellAndSubdividePublishTheirEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelModelingBevelShellVocabularyTest::RunTest(const FString& Parameters)
{
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("round_weight"),
        EPwModelParamType::Number, TEXT("1"));
    // The DOCUMENT's defaults, deliberately not the engine struct's `false` / `0`: material ID 0
    // is whichever slot the first part in the document tagged, so an unqualified bevel used to put
    // every chamfer face in another part's material. The published text is what an author reads to
    // learn what omitting the parameter does, so a stale one here is a documentation bug with no
    // other symptom. `material_id` has no literal default any more - it derives from the geometry
    // being bevelled - which is spelled the same way filter_box_min's absence is.
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("infer_material_id"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("material_id"),
        EPwModelParamType::Integer, TEXT(""));
    // The named-slot spelling, which every other tagging surface in the format has and `bevel`
    // did not - leaving the raw index as the only way to say what a chamfer is made of.
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("material"),
        EPwModelParamType::String, TEXT(""));
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("filter_box_min"),
        EPwModelParamType::Vector3, TEXT(""));
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("filter_box_max"),
        EPwModelParamType::Vector3, TEXT(""));
    ModelingVocabTest_ExpectParam(*this, TEXT("bevel"), TEXT("fully_contained"),
        EPwModelParamType::Bool, TEXT("true"));

    // Two deliberate omissions on bevel, for two different reasons:
    //  - apply_filter_box: the compiler derives it from BOTH corners being present. A flag with
    //    no box, and a box with the flag off, are two spellings of a request that silently does
    //    nothing; this shape has neither.
    //  - filter_box_transform: the box is already in the mesh's own space, so a transform on top
    //    of it adds no shape a caller cannot spell by moving the corners - at the cost of four
    //    more published parameters.
    ModelingVocabTest_ExpectNoParam(*this, TEXT("bevel"), TEXT("apply_filter_box"),
        TEXT("it is derived from both corners being present"));
    ModelingVocabTest_ExpectNoParam(*this, TEXT("bevel"), TEXT("filter_box_transform"),
        TEXT("the box is already in mesh space"));

    ModelingVocabTest_ExpectParam(*this, TEXT("shell"), TEXT("fixed_boundary"),
        EPwModelParamType::Bool, TEXT("false"));
    ModelingVocabTest_ExpectParam(*this, TEXT("shell"), TEXT("solve_steps"),
        EPwModelParamType::Integer, TEXT("5"));
    ModelingVocabTest_ExpectParam(*this, TEXT("shell"), TEXT("smooth_alpha"),
        EPwModelParamType::Number, TEXT("0.1"));
    ModelingVocabTest_ExpectParam(*this, TEXT("shell"), TEXT("reproject_during_smoothing"),
        EPwModelParamType::Bool, TEXT("false"));
    ModelingVocabTest_ExpectParam(*this, TEXT("shell"), TEXT("boundary_alpha"),
        EPwModelParamType::Number, TEXT("0.2"));

    // boundary_alpha is the one widened parameter with a declared domain, taken from the engine's
    // own "should not be > 0.9" comment. The range lives on the spec rather than in a check keyed
    // on the op's name, so this assertion is what proves the declaration is actually there.
    {
        const FPwModelOpSpec* Shell = ModelingVocabTest_FindOp(*this, TEXT("shell"));
        if (Shell)
        {
            const FPwModelParamSpec* Alpha = Shell->FindParam(FString(TEXT("boundary_alpha")));
            if (Alpha)
            {
                TestTrue(TEXT("shell boundary_alpha declares a range"), Alpha->bHasRange);
                TestEqual(TEXT("shell boundary_alpha minimum"), Alpha->MinValue, 0.0);
                TestEqual(TEXT("shell boundary_alpha maximum is the engine's stated ceiling"),
                    Alpha->MaxValue, 0.9);
            }
        }
    }

    ModelingVocabTest_ExpectParam(*this, TEXT("subdivide"), TEXT("recompute_normals"),
        EPwModelParamType::Bool, TEXT("true"));

    ModelingVocabTest_ExpectClean(*this, TEXT("bevel, shell and subdivide setting every option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  subdivide iterations=1 recompute_normals=false\n")
        TEXT("  bevel distance=2 segments=2 round_weight=0.5 infer_material_id=true material_id=3 filter_box_min=(-30, -30, -30) filter_box_max=(30, 30, 0) fully_contained=false\n")
        TEXT("  shell thickness=2 fixed_boundary=true solve_steps=10 smooth_alpha=0.3 reproject_during_smoothing=true boundary_alpha=0.9\n")
        TEXT("}\n"));

    ModelingVocabTest_ExpectCode(*this, TEXT("boundary_alpha above the engine's ceiling"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  shell thickness=2 boundary_alpha=0.95\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}

// ============================================================================
// bend / twist / taper
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelModelingWarpVocabularyTest,
    "PinWright.Model.ModelingOps.WarpsPublishTheirEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelModelingWarpVocabularyTest::RunTest(const FString& Parameters)
{
    // All three warps published NOTHING beyond their angle/flare and extent, and bSymmetricExtents
    // was hardcoded true - which made LowerExtent dead code in the engine struct. Together the two
    // are what turns [-extent, +extent] into the asymmetric range a mesh that does not straddle
    // the origin needs.
    const TCHAR* const Warps[] = { TEXT("bend"), TEXT("twist"), TEXT("taper") };
    for (const TCHAR* OpName : Warps)
    {
        ModelingVocabTest_ExpectParam(*this, OpName, TEXT("symmetric_extents"),
            EPwModelParamType::Bool, TEXT("true"));
        ModelingVocabTest_ExpectParam(*this, OpName, TEXT("lower_extent"),
            EPwModelParamType::Number, TEXT("10"));
    }

    ModelingVocabTest_ExpectParam(*this, TEXT("bend"), TEXT("bidirectional"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("twist"), TEXT("bidirectional"),
        EPwModelParamType::Bool, TEXT("true"));

    // taper's third field is the profile rather than a bidirectional flag - FlareWarpOptions has
    // no bBidirectional, which is why the three warps do not share one parameter list.
    ModelingVocabTest_ExpectEnum(*this, TEXT("taper"), TEXT("flare_type"), TEXT("sin_mode"),
        TArray<FString>{ TEXT("sin_mode"), TEXT("sin_squared_mode"), TEXT("triangle_mode") });
    ModelingVocabTest_ExpectNoParam(*this, TEXT("taper"), TEXT("bidirectional"),
        TEXT("FGeometryScriptFlareWarpOptions has no bBidirectional field"));

    // EGeometryScriptEmptySelectionBehavior is the field smooth, relax and noise_deform all leave
    // out on purpose. None of the three builds a selection - each passes a default-constructed
    // one unconditionally - so the default (FullMeshSelection) is what makes the op run at all,
    // and the only other value makes it an unconditional no-op on every possible input.
    // Publishing it would add a knob whose sole non-default setting is "do nothing".
    const TCHAR* const NoSelectionOps[] = { TEXT("smooth"), TEXT("relax"), TEXT("noise_deform") };
    for (const TCHAR* OpName : NoSelectionOps)
    {
        ModelingVocabTest_ExpectNoParam(*this, OpName, TEXT("empty_behavior"),
            TEXT("these ops build no selection, so its only non-default value is a total no-op"));
    }

    ModelingVocabTest_ExpectClean(*this, TEXT("the three warps setting every option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 200)\n")
        TEXT("  bend angle=30 extent=60 symmetric_extents=false lower_extent=5 bidirectional=false\n")
        TEXT("  twist angle=30 extent=60 symmetric_extents=false lower_extent=5 bidirectional=false\n")
        TEXT("  taper flare=(70, 40) extent=60 symmetric_extents=false lower_extent=5 flare_type=sin_squared_mode\n")
        TEXT("}\n"));

    ModelingVocabTest_ExpectCode(*this, TEXT("a misspelled flare_type"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 200)\n")
        TEXT("  taper flare=(70, 40) flare_type=sin_squared\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}

// ============================================================================
// split_normals / recompute_tangents / fill_holes / remove_degenerates /
// weld_vertices / self_union
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelModelingRepairVocabularyTest,
    "PinWright.Model.ModelingOps.NormalsAndRepairOpsPublishTheirEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelModelingRepairVocabularyTest::RunTest(const FString& Parameters)
{
    // fill_holes, recompute_tangents and remove_degenerates published ZERO of their engine fields
    // before this pass - each was a button with no settings - so every assertion in those three
    // groups is a spelling that did not exist.

    ModelingVocabTest_ExpectEnum(*this, TEXT("fill_holes"), TEXT("method"), TEXT("automatic"),
        TArray<FString>{ TEXT("automatic"), TEXT("minimal_fill"), TEXT("polygon_triangulation"),
                         TEXT("triangle_fan"), TEXT("planar_projection") });
    ModelingVocabTest_ExpectParam(*this, TEXT("fill_holes"), TEXT("delete_isolated_triangles"),
        EPwModelParamType::Bool, TEXT("true"));

    ModelingVocabTest_ExpectEnum(*this, TEXT("recompute_tangents"), TEXT("type"), TEXT("fast_mikkt"),
        TArray<FString>{ TEXT("fast_mikkt"), TEXT("per_triangle"), TEXT("standard_mikkt") });
    ModelingVocabTest_ExpectParam(*this, TEXT("recompute_tangents"), TEXT("uv_layer"),
        EPwModelParamType::Integer, TEXT("0"));

    ModelingVocabTest_ExpectEnum(*this, TEXT("remove_degenerates"), TEXT("mode"), TEXT("repair_or_delete"),
        TArray<FString>{ TEXT("delete_only"), TEXT("repair_or_delete"), TEXT("repair_or_skip") });
    ModelingVocabTest_ExpectParam(*this, TEXT("remove_degenerates"), TEXT("min_triangle_area"),
        EPwModelParamType::Number, TEXT("0.001"));
    ModelingVocabTest_ExpectParam(*this, TEXT("remove_degenerates"), TEXT("min_edge_length"),
        EPwModelParamType::Number, TEXT("0.0001"));
    ModelingVocabTest_ExpectParam(*this, TEXT("remove_degenerates"), TEXT("compact_on_completion"),
        EPwModelParamType::Bool, TEXT("true"));

    // split_normals' 60 is NOT the engine's 15, and must not be moved onto it: this surface has
    // published 60 since it shipped, so the engine value would put a hard edge on every mesh that
    // currently comes out smooth.
    ModelingVocabTest_ExpectParam(*this, TEXT("split_normals"), TEXT("split_angle"),
        EPwModelParamType::Number, TEXT("60"));
    ModelingVocabTest_ExpectParam(*this, TEXT("split_normals"), TEXT("split_by_opening_angle"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("split_normals"), TEXT("split_by_face_group"),
        EPwModelParamType::Bool, TEXT("false"));
    // FGeometryScriptGroupLayer, flattened for the reason the polygroup edit options are.
    ModelingVocabTest_ExpectParam(*this, TEXT("split_normals"), TEXT("use_default_group_layer"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("split_normals"), TEXT("group_layer_index"),
        EPwModelParamType::Integer, TEXT("0"));

    // weld_vertices' shipped tolerance, which is neither the engine's 1e-06 nor mirror's 1e-03.
    ModelingVocabTest_ExpectParam(*this, TEXT("weld_vertices"), TEXT("tolerance"),
        EPwModelParamType::Number, TEXT("0.0001"));
    ModelingVocabTest_ExpectParam(*this, TEXT("weld_vertices"), TEXT("only_unique_pairs"),
        EPwModelParamType::Bool, TEXT("true"));

    // self_union's simplify_output defaults TRUE, unlike the four boolean verbs which override it
    // to false. That divergence is deliberate - self_union has always run on the engine struct
    // defaults - and this is the line that fails if the two are ever "reconciled".
    ModelingVocabTest_ExpectParam(*this, TEXT("self_union"), TEXT("trim_flaps"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("self_union"), TEXT("simplify_output"),
        EPwModelParamType::Bool, TEXT("true"));
    ModelingVocabTest_ExpectParam(*this, TEXT("self_union"), TEXT("simplify_planar_tolerance"),
        EPwModelParamType::Number, TEXT("0.01"));
    ModelingVocabTest_ExpectParam(*this, TEXT("self_union"), TEXT("winding_threshold"),
        EPwModelParamType::Number, TEXT("0.5"));

    ModelingVocabTest_ExpectClean(*this, TEXT("the normals and repair ops setting every option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  self_union fill_holes=false trim_flaps=false simplify_output=false simplify_planar_tolerance=0.05 winding_threshold=1.5\n")
        TEXT("  split_normals split_angle=35 split_by_opening_angle=false split_by_face_group=true use_default_group_layer=false group_layer_index=1\n")
        TEXT("  recompute_tangents type=per_triangle uv_layer=1\n")
        TEXT("  weld_vertices tolerance=0.01 only_unique_pairs=false\n")
        TEXT("  fill_holes method=polygon_triangulation delete_isolated_triangles=false\n")
        TEXT("  remove_degenerates mode=repair_or_skip min_triangle_area=0.5 min_edge_length=0.25 compact_on_completion=false\n")
        TEXT("}\n"));

    ModelingVocabTest_ExpectCode(*this, TEXT("a misspelled fill_holes method"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  fill_holes method=ear_clipping\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    ModelingVocabTest_ExpectCode(*this, TEXT("a uv_layer outside the 0-7 channel range"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  recompute_tangents uv_layer=8\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}
