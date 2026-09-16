// Copyright (c) 2026 Alexander Penkin. MIT License.

// The .pwmodel half of the op widening: the parameters simplify_mesh, remesh_uniform, `uv` and
// the four boolean ops gained must be IN THE OP TABLE, not merely readable by the compiler.
//
// That distinction is the whole reason this file exists separately from the op-level tests in
// Tests/Geometry/TestGeometryWidenedOpOptions.cpp. The compiler reads a parameter by name out of
// a TMap, so a field it reads but the table does not declare is silently unreachable in the
// opposite direction: the PARSER rejects it as PWSRC_UNKNOWN_PARAM before the compiler ever
// sees the document, and the op-level test still passes because it calls the op directly. The
// table is also exactly what model.describe_ops emits - it iterates PwModelOpTable::Get() and
// serializes each FPwModelParamSpec whole (ModelCompileHandler.cpp) - so asserting the table is
// asserting the published vocabulary, and no separate describe_ops assertion is needed or would
// be more direct.
//
// What each group below would let through if it were reverted:
//
//  - A DECLARED-BUT-UNREAD parameter. Caught by the parse tests: a document setting every new
//    parameter must produce zero diagnostics. It cannot catch the reverse (read but undeclared),
//    which is what the presence assertions are for.
//  - AN ENUM WITH THE WRONG SPELLINGS. `method=attribute_aware` is the snake_case of the engine
//    ENUMERATOR minus its type prefix, matching how `complexity` mirrors ECollisionTraceFlag. A
//    table that accepted "AttributeAware" or "normals_aware" would compile fine and leave every
//    document written against the documented vocabulary rejected.
//  - THE SIMPLIFY DEFAULT. `method` publishing a default of `standard_qem` would re-document the
//    override the widening removed, even with the struct fixed.
#include "Misc/AutomationTest.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

const FPwModelParamSpec* WidenedVocabTest_FindParam(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName)
{
    const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(OpName), EPwModelOpContext::Part);
    if (!Op)
    {
        Test.AddError(FString::Printf(TEXT("op '%s' is not in the part-context op table"), OpName));
        return nullptr;
    }

    const FPwModelParamSpec* Param = Op->FindParam(FString(ParamName));
    if (!Param)
    {
        Test.AddError(FString::Printf(
            TEXT("'%s' publishes no parameter named '%s' - the compiler may read it, but the "
                 "parser rejects any document that sets it"), OpName, ParamName));
    }
    return Param;
}

void WidenedVocabTest_ExpectParam(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName,
    EPwModelParamType ExpectedType, const TCHAR* ExpectedDefault)
{
    const FPwModelParamSpec* Param = WidenedVocabTest_FindParam(Test, OpName, ParamName);
    if (!Param)
    {
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

void WidenedVocabTest_ExpectEnum(
    FAutomationTestBase& Test, const TCHAR* OpName, const TCHAR* ParamName,
    const TCHAR* ExpectedDefault, const TArray<FString>& ExpectedValues)
{
    const FPwModelParamSpec* Param = WidenedVocabTest_FindParam(Test, OpName, ParamName);
    if (!Param)
    {
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

// Parses Source and reports every diagnostic in the failure message, because "expected 0, got 3"
// costs a rerun under a debugger to learn anything.
void WidenedVocabTest_ExpectClean(FAutomationTestBase& Test, const TCHAR* What, const FString& Source)
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

void WidenedVocabTest_ExpectCode(
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
// simplify_mesh
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWidenedSimplifyVocabularyTest,
    "PinWright.Model.WidenedOps.SimplifyMeshPublishesItsEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWidenedSimplifyVocabularyTest::RunTest(const FString& Parameters)
{
    WidenedVocabTest_ExpectEnum(*this, TEXT("simplify_mesh"), TEXT("method"), TEXT("attribute_aware"),
        TArray<FString>{ TEXT("standard_qem"), TEXT("volume_preserving"),
                         TEXT("attribute_aware"), TEXT("attribute_aware_v2") });
    WidenedVocabTest_ExpectEnum(*this, TEXT("simplify_mesh"), TEXT("quadric_variant"), TEXT("plane_quadric"),
        TArray<FString>{ TEXT("plane_quadric"), TEXT("triangle_quadric") });

    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("allow_seam_collapse"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("allow_seam_smoothing"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("allow_seam_splits"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("preserve_vertex_positions"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("retain_quadric_memory"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("auto_compact"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("regularize_weight"), EPwModelParamType::Number, TEXT("0.000001"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("normal_attribute_weight"), EPwModelParamType::Number, TEXT("16"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("tangent_attribute_weight"), EPwModelParamType::Number, TEXT("0.1"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("color_attribute_weight"), EPwModelParamType::Number, TEXT("0.1"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("texcoord_attribute_weight"), EPwModelParamType::Number, TEXT("0.5"));
    WidenedVocabTest_ExpectParam(*this, TEXT("simplify_mesh"), TEXT("scale_correction"), EPwModelParamType::Number, TEXT("1"));

    // The three FGeometryScriptWeightMapDensity options stay OUT: each needs an
    // identifier->resource binding the format does not have. Asserted rather than merely
    // documented, because publishing one would look like widening and would be a parameter no
    // document could give a working value to.
    const FPwModelOpSpec* Simplify = PwModelOpTable::Find(FString(TEXT("simplify_mesh")), EPwModelOpContext::Part);
    if (Simplify)
    {
        TestNull(TEXT("edge_length_weight_map is deliberately not published"),
            Simplify->FindParam(FString(TEXT("edge_length_weight_map"))));
        TestNull(TEXT("geometric_tolerance_weight_map is deliberately not published"),
            Simplify->FindParam(FString(TEXT("geometric_tolerance_weight_map"))));
        TestNull(TEXT("quadric_error_weight_map is deliberately not published"),
            Simplify->FindParam(FString(TEXT("quadric_error_weight_map"))));
    }

    WidenedVocabTest_ExpectClean(*this, TEXT("a simplify_mesh setting every published option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        // One statement per LINE. A newline is a token in this grammar, so an op's parameter
        // list cannot be wrapped however long it gets - which is itself worth pinning, since a
        // widened op is exactly where someone reaches for a line break.
        TEXT("  simplify_mesh target_percentage=40 method=attribute_aware_v2 quadric_variant=triangle_quadric allow_seam_collapse=false allow_seam_smoothing=false allow_seam_splits=false preserve_vertex_positions=true retain_quadric_memory=true regularize_weight=0.001 auto_compact=false normal_attribute_weight=4 tangent_attribute_weight=0.2 color_attribute_weight=0.3 texcoord_attribute_weight=0.9 scale_correction=2\n")
        TEXT("}\n"));

    WidenedVocabTest_ExpectCode(*this, TEXT("a misspelled method"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        TEXT("  simplify_mesh method=atribute_aware\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}

// ============================================================================
// remesh_uniform
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWidenedRemeshVocabularyTest,
    "PinWright.Model.WidenedOps.RemeshUniformPublishesItsEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWidenedRemeshVocabularyTest::RunTest(const FString& Parameters)
{
    WidenedVocabTest_ExpectEnum(*this, TEXT("remesh_uniform"), TEXT("target_type"), TEXT("triangle_count"),
        TArray<FString>{ TEXT("triangle_count"), TEXT("target_edge_length") });
    WidenedVocabTest_ExpectEnum(*this, TEXT("remesh_uniform"), TEXT("smoothing_type"), TEXT("mixed"),
        TArray<FString>{ TEXT("uniform"), TEXT("uv_preserving"), TEXT("mixed") });

    // The same four-value vocabulary three times over, which is the point: an edge constraint is
    // one concept, and three copies that drift are three different documents to write.
    const TArray<FString> Constraints{ TEXT("fixed"), TEXT("refine"), TEXT("free"), TEXT("ignore") };
    WidenedVocabTest_ExpectEnum(*this, TEXT("remesh_uniform"), TEXT("mesh_boundary_constraint"), TEXT("free"), Constraints);
    WidenedVocabTest_ExpectEnum(*this, TEXT("remesh_uniform"), TEXT("group_boundary_constraint"), TEXT("free"), Constraints);
    WidenedVocabTest_ExpectEnum(*this, TEXT("remesh_uniform"), TEXT("material_boundary_constraint"), TEXT("free"), Constraints);

    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("target_edge_length"), EPwModelParamType::Number, TEXT("1"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("discard_attributes"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("reproject_to_input_mesh"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("smoothing_rate"), EPwModelParamType::Number, TEXT("0.25"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("allow_flips"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("allow_splits"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("allow_collapses"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("prevent_normal_flips"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("prevent_tiny_triangles"), EPwModelParamType::Bool, TEXT("true"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("use_full_remesh_passes"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("iterations"), EPwModelParamType::Integer, TEXT("20"));
    WidenedVocabTest_ExpectParam(*this, TEXT("remesh_uniform"), TEXT("auto_compact"), EPwModelParamType::Bool, TEXT("true"));

    // smoothing_rate is the one bounded parameter here, and the bound is the engine's clamp.
    // Published rather than enforced only at the engine, so an author learns it from the
    // vocabulary instead of from a silently clamped result.
    if (const FPwModelParamSpec* Rate = WidenedVocabTest_FindParam(*this, TEXT("remesh_uniform"), TEXT("smoothing_rate")))
    {
        TestTrue(TEXT("smoothing_rate publishes its 0-1 domain"),
            Rate->bHasRange && Rate->MinValue == 0.0 && Rate->MaxValue == 1.0);
    }

    WidenedVocabTest_ExpectClean(*this, TEXT("a remesh_uniform setting every published option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        TEXT("  remesh_uniform target_type=target_edge_length target_edge_length=4 target_triangle_count=800 discard_attributes=true reproject_to_input_mesh=false smoothing_type=uv_preserving smoothing_rate=0.5 mesh_boundary_constraint=fixed group_boundary_constraint=refine material_boundary_constraint=ignore allow_flips=false allow_splits=false allow_collapses=false prevent_normal_flips=false prevent_tiny_triangles=false use_full_remesh_passes=true iterations=3 auto_compact=false\n")
        TEXT("}\n"));

    WidenedVocabTest_ExpectCode(*this, TEXT("a smoothing_rate above its published maximum"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        TEXT("  remesh_uniform smoothing_rate=2\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}

// ============================================================================
// uv mode=layout and uv mode=patch_builder
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWidenedUvVocabularyTest,
    "PinWright.Model.WidenedOps.UvPublishesItsLayoutAndPatchBuilderOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWidenedUvVocabularyTest::RunTest(const FString& Parameters)
{
    // layout. `layout_scale` carries a prefix and `texture_resolution` does not, and that is not
    // an inconsistency: FGeometryScriptLayoutUVsOptions::Scale is a uniform post-pack scale while
    // this op already publishes `scale` as the 2D projection frame, so the two cannot share a
    // name. Everything that does not collide keeps the plain snake_case of the engine field.
    WidenedVocabTest_ExpectEnum(*this, TEXT("uv"), TEXT("layout_type"), TEXT("repack"),
        TArray<FString>{ TEXT("transform"), TEXT("stack"), TEXT("repack"), TEXT("normalize") });
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("texture_resolution"), EPwModelParamType::Integer, TEXT("1024"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("layout_scale"), EPwModelParamType::Number, TEXT("1"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("translation"), EPwModelParamType::Vector2, TEXT("(0, 0)"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("preserve_scale"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("preserve_rotation"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("allow_flips"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("enable_udim_layout"), EPwModelParamType::Bool, TEXT("false"));

    // patch_builder.
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("initial_patch_count"), EPwModelParamType::Integer, TEXT("100"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("min_patch_size"), EPwModelParamType::Integer, TEXT("2"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("patch_curvature_alignment_weight"), EPwModelParamType::Number, TEXT("1"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("patch_merging_metric_thresh"), EPwModelParamType::Number, TEXT("1.5"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("patch_merging_angle_thresh"), EPwModelParamType::Number, TEXT("45"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("exp_map_normal_smoothing_rounds"), EPwModelParamType::Integer, TEXT("0"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("exp_map_normal_smoothing_alpha"), EPwModelParamType::Number, TEXT("0.25"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("respect_input_groups"), EPwModelParamType::Bool, TEXT("false"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("auto_pack"), EPwModelParamType::Bool, TEXT("true"));
    // 512 here and 1024 on texture_resolution above: two engine structs feeding two packers.
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("packing_target_image_width"), EPwModelParamType::Integer, TEXT("512"));
    WidenedVocabTest_ExpectParam(*this, TEXT("uv"), TEXT("packing_optimize_island_rotation"), EPwModelParamType::Bool, TEXT("true"));

    // The two nested option structs the engine also carries stay out for the reason
    // GeometryOps::FPatchBuilderUVParams gives: FPwValue has no map or sub-object member,
    // so neither a UDIM resolution table nor a named polygroup layer has a literal here.
    const FPwModelOpSpec* Uv = PwModelOpTable::Find(FString(TEXT("uv")), EPwModelOpContext::Part);
    if (Uv)
    {
        TestNull(TEXT("udim_resolutions is deliberately not published - it is a TMap"),
            Uv->FindParam(FString(TEXT("udim_resolutions"))));
        TestNull(TEXT("group_layer is deliberately not published - nothing here creates one"),
            Uv->FindParam(FString(TEXT("group_layer"))));
    }

    WidenedVocabTest_ExpectClean(*this, TEXT("a uv mode=layout setting every layout option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        TEXT("  uv mode=layout channel=0 layout_type=stack texture_resolution=2048 layout_scale=0.5 translation=(0.1, 0.2) preserve_scale=true preserve_rotation=true allow_flips=true enable_udim_layout=true\n")
        TEXT("}\n"));

    WidenedVocabTest_ExpectClean(*this, TEXT("a uv mode=patch_builder setting every patch option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        TEXT("  uv mode=patch_builder channel=0 initial_patch_count=8 min_patch_size=4 patch_curvature_alignment_weight=2 patch_merging_metric_thresh=2.5 patch_merging_angle_thresh=30 exp_map_normal_smoothing_rounds=3 exp_map_normal_smoothing_alpha=0.5 respect_input_groups=true auto_pack=false packing_target_image_width=1024 packing_optimize_island_rotation=false\n")
        TEXT("}\n"));

    WidenedVocabTest_ExpectCode(*this, TEXT("a misspelled layout_type"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(10, 10, 10)\n")
        TEXT("  uv mode=layout layout_type=repak\n")
        TEXT("}\n"),
        PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    return true;
}

// ============================================================================
// union / subtract / intersection / trim
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWidenedBooleanVocabularyTest,
    "PinWright.Model.WidenedOps.BooleansPublishTheirEngineOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelWidenedBooleanVocabularyTest::RunTest(const FString& Parameters)
{
    // All four ops carried EMPTY parameter lists apart from trim's operation selector, so every
    // assertion here is a field that had no spelling at all before the widening.
    const TCHAR* const BooleanOps[] = { TEXT("union"), TEXT("subtract"), TEXT("intersection"), TEXT("trim") };
    for (const TCHAR* OpName : BooleanOps)
    {
        WidenedVocabTest_ExpectParam(*this, OpName, TEXT("fill_holes"), EPwModelParamType::Bool, TEXT("true"));
        WidenedVocabTest_ExpectParam(*this, OpName, TEXT("simplify_output"), EPwModelParamType::Bool, TEXT("true"));
        WidenedVocabTest_ExpectParam(*this, OpName, TEXT("allow_empty_result"), EPwModelParamType::Bool, TEXT("false"));

        // Two fields of FGeometryScriptMeshBooleanOptions stay out, for two unrelated reasons,
        // and both are asserted so that "widen it" cannot be re-derived from the field count:
        //  - simplify_planar_tolerance: UE 5.8's ApplyMeshBoolean never forwards it to the
        //    operation, so it would be a parameter that provably does nothing here.
        //  - output_space: this compiler passes identity for BOTH operand transforms, so all
        //    three of its values name the same space. It is published on the RPC verbs instead.
        const FPwModelOpSpec* Op = PwModelOpTable::Find(FString(OpName), EPwModelOpContext::Part);
        if (Op)
        {
            TestNull(*FString::Printf(TEXT("%s does not publish simplify_planar_tolerance - the engine ignores it"), OpName),
                Op->FindParam(FString(TEXT("simplify_planar_tolerance"))));
            TestNull(*FString::Printf(TEXT("%s does not publish output_space - both transforms are identity here"), OpName),
                Op->FindParam(FString(TEXT("output_space"))));
        }
    }

    // trim keeps its operation selector alongside the options, and it is not one of them.
    WidenedVocabTest_ExpectParam(*this, TEXT("trim"), TEXT("keep_inside"), EPwModelParamType::Bool, TEXT("false"));

    WidenedVocabTest_ExpectClean(*this, TEXT("booleans setting every published option"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  subtract fill_holes=false simplify_output=false allow_empty_result=true {\n")
        TEXT("    sphere radius=10\n")
        TEXT("  }\n")
        TEXT("  union simplify_output=false {\n")
        TEXT("    box size=(5, 5, 5)\n")
        TEXT("  }\n")
        TEXT("  trim keep_inside=true fill_holes=false simplify_output=false {\n")
        TEXT("    sphere radius=30\n")
        TEXT("  }\n")
        TEXT("}\n"));

    // A boolean now TAKES material= - it names the faces the operation creates - and still rejects
    // color=, which would only recolour vertices the operands' own generators already coloured.
    // The binding is here because a tagged slot nothing binds is PWMODEL_UNBOUND_MATERIAL, and
    // ExpectClean asserts NO diagnostics - a tag inside a boolean block counts as a reference like
    // any other, which is itself part of what this pins.
    WidenedVocabTest_ExpectClean(*this, TEXT("material= on a widened boolean"),
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("  Shell = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("}\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40) material=\"Shell\"\n")
        TEXT("  subtract fill_holes=false material=\"Shell\" {\n")
        TEXT("    sphere radius=10\n")
        TEXT("  }\n")
        TEXT("}\n"));

    WidenedVocabTest_ExpectCode(*this, TEXT("color= on a widened boolean"),
        TEXT("pwmodel 0\n")
        TEXT("part Body {\n")
        TEXT("  box size=(40, 40, 40)\n")
        TEXT("  subtract fill_holes=false color=(1, 0, 0, 1) {\n")
        TEXT("    sphere radius=10\n")
        TEXT("  }\n")
        TEXT("}\n"),
        PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN);

    return true;
}
