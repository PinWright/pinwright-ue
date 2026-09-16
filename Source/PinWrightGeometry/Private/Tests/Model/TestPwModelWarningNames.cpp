// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for PwModelWarningNames - the RPC-label -> `.pwmodel`-label translation the compiler
// applies when it re-emits an FOpResult warning as PWMODEL_STAGE_WARNING.
//
// The defect these pin: GeometryOps labels every clamp warning with the RPC's PUBLISHED parameter
// name, because that is the name a `geometry.*` caller passed and greps its response for. The
// compiler used to forward that text verbatim with only an op-name prefix, so a document that
// wrote `cylinder height_steps=300` was told about `heightSteps`, and one that wrote
// `box segments=(6, 5, 4)` was told about `widthSegments` - names the `.pwmodel` surface does not
// have at all. The author's next move is to grep the format reference for the name in the
// warning, find nothing, and conclude the diagnostic is about some other parameter.
//
// A single label cannot serve both surfaces, so neither one owns it: the ops layer keeps the RPC
// spelling and knows nothing about its callers, and the compiler translates at the one place a
// warning crosses into the document's vocabulary.
//
// What each test here would let through if it were reverted:
//
//  - TABLE TARGETS DECLARED PARAMETERS. A row whose ModelLabel is a name `.pwmodel` does not
//    publish replaces one wrong name with another wrong name, silently. Checked against
//    PwModelOpTable - the same table the parser validates documents against and
//    model.describe_ops emits - rather than against a second hand-written list, because a second
//    list is a second thing to drift.
//  - TRANSLATION IS LEADING-TOKEN AND PER-OP. The label is always the first token of a
//    Clamp*Warn message, so anything looser would rewrite the word wherever it appeared in prose
//    and would rewrite one op's label under another op's name.
//  - COMPILED DOCUMENTS NAME DOCUMENT PARAMETERS, end to end, through the real compiler. The
//    sweep at the end of that test is the guard against a MISSING row: any `<token> clamped from`
//    a compile emits must name a parameter the op actually declares, so a new camelCase label
//    added in GeometryOps without a row here fails this test rather than shipping.
//  - THE RPC SURFACE IS UNCHANGED. The whole point of translating in the compiler is that the
//    ops layer stays the RPC's. If someone "fixes" this by renaming the labels in GeometryOps
//    instead, the .pwmodel half of this file would still pass and the RPC half would fail -
//    which is the failure that says the fix was applied at the wrong seam.
#include "Misc/AutomationTest.h"

#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryUtils.h"

#include "UDynamicMesh.h"
#include "UObject/Package.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.

FPwModelCompileResult PwModelWarnNames_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

TArray<FString> PwModelWarnNames_StageWarnings(const FPwModelCompileResult& Result)
{
    TArray<FString> Messages;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING)
        {
            Messages.Add(Diagnostic.Message);
        }
    }
    return Messages;
}

bool PwModelWarnNames_Any(const TArray<FString>& Messages, const FString& Needle)
{
    for (const FString& Message : Messages)
    {
        if (Message.Contains(Needle, ESearchCase::CaseSensitive))
        {
            return true;
        }
    }
    return false;
}

// `size.x` -> `size`. The component suffix is the translation's own notation for "which axis of
// this vector moved"; the base is what the document declares and what an author greps.
FString PwModelWarnNames_BaseName(const FString& Label)
{
    int32 Dot = INDEX_NONE;
    return Label.FindChar(TEXT('.'), Dot) ? Label.Left(Dot) : Label;
}

UDynamicMesh* PwModelWarnNames_TestMesh()
{
    return GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
}

bool PwModelWarnNames_OpWarns(const GeometryOps::FOpResult& Op, const TCHAR* Label)
{
    const FString Needle = FString::Printf(TEXT("%s clamped from "), Label);
    for (const FString& Warning : Op.Warnings)
    {
        if (Warning.StartsWith(Needle, ESearchCase::CaseSensitive))
        {
            return true;
        }
    }
    return false;
}
}

// ============================================================================
// The table names parameters the format actually publishes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarningNamesTableTargetsDeclaredParametersTest,
    "PinWright.Model.WarningNames.TableTargetsDeclaredDocumentParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelWarningNamesTableTargetsDeclaredParametersTest::RunTest(const FString& Parameters)
{
    const TArrayView<const PwModelWarningNames::FEntry> Entries = PwModelWarningNames::Entries();

    TestTrue(TEXT("the translation table is not empty"), Entries.Num() > 0);

    for (const PwModelWarningNames::FEntry& Entry : Entries)
    {
        const FString OpName(Entry.OpName);
        const FString RpcLabel(Entry.RpcLabel);
        const FString ModelLabel(Entry.ModelLabel);

        const FPwModelOpSpec* Spec = PwModelOpTable::Find(OpName, EPwModelOpContext::Part);
        if (Spec == nullptr)
        {
            AddError(FString::Printf(
                TEXT("row '%s'/'%s' names an op that is not in the part-context op table"),
                *OpName, *RpcLabel));
            continue;
        }

        // The target must be something a document can actually write.
        const FString Base = PwModelWarnNames_BaseName(ModelLabel);
        const FPwModelParamSpec* Param = Spec->FindParam(Base);
        if (Param == nullptr)
        {
            AddError(FString::Printf(
                TEXT("'%s' translates '%s' to '%s', but '%s' declares no parameter '%s'"),
                *OpName, *RpcLabel, *ModelLabel, *OpName, *Base));
            continue;
        }

        // A component suffix is only meaningful on a vector, and only for an axis it has.
        if (Base.Len() != ModelLabel.Len())
        {
            const FString Component = ModelLabel.RightChop(Base.Len() + 1);
            const bool bIsVector2 = Param->Type == EPwModelParamType::Vector2;
            const bool bIsVector3 = Param->Type == EPwModelParamType::Vector3;

            TestTrue(*FString::Printf(
                    TEXT("'%s %s' carries a component suffix, so it must be a vector"),
                    *OpName, *Base),
                bIsVector2 || bIsVector3);
            TestTrue(*FString::Printf(TEXT("'%s' names an axis '%s' has"), *ModelLabel, *Base),
                Component == TEXT("x") || Component == TEXT("y")
                || (Component == TEXT("z") && bIsVector3));
        }

        // And the row must be a REAL divergence. A row whose RPC label is also a declared
        // `.pwmodel` parameter of the same op is either dead weight or, worse, rewriting a name
        // the author could legitimately have written.
        TestNull(*FString::Printf(
                TEXT("'%s' publishes no parameter called '%s', or the row is not a divergence"),
                *OpName, *RpcLabel),
            Spec->FindParam(RpcLabel));
    }

    return true;
}

// ============================================================================
// Translation is leading-token, and belongs to one op
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarningNamesTranslatesLeadingTokenOnlyTest,
    "PinWright.Model.WarningNames.TranslatesTheLeadingTokenOfItsOwnOpOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelWarningNamesTranslatesLeadingTokenOnlyTest::RunTest(const FString& Parameters)
{
    // Every row round-trips: the synthetic message is exactly the shape ClampSegmentsWarn and
    // ClampCountWarn produce, and ClampRangeWarn's is the same with a range clause appended.
    for (const PwModelWarningNames::FEntry& Entry : PwModelWarningNames::Entries())
    {
        const FString Source = FString::Printf(TEXT("%s clamped from 9 to 1"), Entry.RpcLabel);
        const FString Expected = FString::Printf(TEXT("%s clamped from 9 to 1"), Entry.ModelLabel);

        TestEqual(*FString::Printf(TEXT("'%s' rewrites '%s'"), Entry.OpName, Entry.RpcLabel),
            PwModelWarningNames::Translate(FString(Entry.OpName), Source), Expected);
    }

    // Per-op. `sphere` has no `heightSteps` and must not inherit `cylinder`'s row.
    const FString Foreign = TEXT("heightSteps clamped from 9 to 1");
    TestEqual(TEXT("a row belongs to its own op only"),
        PwModelWarningNames::Translate(TEXT("sphere"), Foreign), Foreign);

    // Ops whose two surfaces already agree are untouched - the common case.
    const FString Agreed = TEXT("subdivisions clamped from 1 to 2 (valid range 2-256)");
    TestEqual(TEXT("an already-agreeing label is forwarded byte-identical"),
        PwModelWarningNames::Translate(TEXT("sphere"), Agreed), Agreed);

    // Leading token only: the same word inside prose is left alone. GenerateRevolve's
    // `profile has N point(s)` sibling in this family is prose exactly like this.
    const FString Prose = TEXT("mesh width clamped from 1 to 2");
    TestEqual(TEXT("a mapped word that is not the leading token is left alone"),
        PwModelWarningNames::Translate(TEXT("box"), Prose), Prose);

    // Whole token only: a longer label that merely STARTS with a mapped one is not rewritten,
    // which is what the required space after the match buys.
    const FString Longer = TEXT("widthSegmentsExtra clamped from 9 to 1");
    TestEqual(TEXT("a longer label starting with a mapped one is not rewritten"),
        PwModelWarningNames::Translate(TEXT("box"), Longer), Longer);

    // An op with no rows at all falls straight through.
    const FString Unmapped = TEXT("steps clamped from 1 to 2 (valid range 2-256)");
    TestEqual(TEXT("an op with no rows forwards everything unchanged"),
        PwModelWarningNames::Translate(TEXT("revolve"), Unmapped), Unmapped);

    return true;
}

// ============================================================================
// End to end: a compile that trips a clamp names the DOCUMENT's parameter
// ============================================================================

namespace
{
struct FPwModelWarnNames_Case
{
    const TCHAR* Source;
    const TCHAR* OpName;
    const TCHAR* ModelLabel;  // what the warning must name
    const TCHAR* RpcLabel;    // what it must no longer name
};

// One row per divergent label, each with a document that actually trips that clamp. Values sit
// above the ceiling rather than below zero wherever both work, so a row reads as an obvious
// over-request rather than as a parser edge case.
const FPwModelWarnNames_Case PwModelWarnNames_Cases[] =
{
    // ClampDimensionWarn: 0 reads as unset and takes the verb's 100.
    { TEXT("pwmodel 0\npart p { box size=(0, 0, 0) }\n"),
      TEXT("box"), TEXT("size.x"), TEXT("width") },
    { TEXT("pwmodel 0\npart p { box size=(0, 0, 0) }\n"),
      TEXT("box"), TEXT("size.y"), TEXT("height") },
    { TEXT("pwmodel 0\npart p { box size=(0, 0, 0) }\n"),
      TEXT("box"), TEXT("size.z"), TEXT("depth") },

    // The Vector3 fork the box comment used to record as an accepted compromise. ONE axis
    // over-requests per row and the other two stay at the box's default of 1, because a single
    // document that over-requests all three clamps to 256/256/256 - and FGridBoxMeshGenerator
    // draws that as 2*3*255^2 quads, 780300 triangles, over GEOM_MAX_TRIANGLES_PER_DYNAMIC_MESH.
    // The compile then fails on the mesh budget and no warning is readable at all, which says
    // nothing about the label these rows exist to pin.
    { TEXT("pwmodel 0\npart p { box segments=(300, 1, 1) }\n"),
      TEXT("box"), TEXT("segments.x"), TEXT("widthSegments") },
    { TEXT("pwmodel 0\npart p { box segments=(1, 400, 1) }\n"),
      TEXT("box"), TEXT("segments.y"), TEXT("heightSegments") },
    { TEXT("pwmodel 0\npart p { box segments=(1, 1, 500) }\n"),
      TEXT("box"), TEXT("segments.z"), TEXT("depthSegments") },

    // The same fork one dimension down.
    { TEXT("pwmodel 0\npart p { plane subdivisions=(300, 400) }\n"),
      TEXT("plane"), TEXT("subdivisions.x"), TEXT("widthSubdivisions") },
    { TEXT("pwmodel 0\npart p { plane subdivisions=(300, 400) }\n"),
      TEXT("plane"), TEXT("subdivisions.y"), TEXT("depthSubdivisions") },

    // camelCase vs snake_case, the rest of the set.
    { TEXT("pwmodel 0\npart p { cylinder height_steps=300 }\n"),
      TEXT("cylinder"), TEXT("height_steps"), TEXT("heightSteps") },
    { TEXT("pwmodel 0\npart p { cone height_steps=300 }\n"),
      TEXT("cone"), TEXT("height_steps"), TEXT("heightSteps") },
    { TEXT("pwmodel 0\npart p { capsule hemisphere_steps=1 }\n"),
      TEXT("capsule"), TEXT("hemisphere_steps"), TEXT("hemisphereSteps") },
    { TEXT("pwmodel 0\npart p { torus major_segments=1 minor_segments=1 }\n"),
      TEXT("torus"), TEXT("major_segments"), TEXT("majorSegments") },
    { TEXT("pwmodel 0\npart p { torus major_segments=1 minor_segments=1 }\n"),
      TEXT("torus"), TEXT("minor_segments"), TEXT("minorSegments") },
    { TEXT("pwmodel 0\npart p { arch major_steps=1 minor_steps=1 }\n"),
      TEXT("arch"), TEXT("major_steps"), TEXT("majorSteps") },
    { TEXT("pwmodel 0\npart p { arch major_steps=1 minor_steps=1 }\n"),
      TEXT("arch"), TEXT("minor_steps"), TEXT("minorSteps") },
    { TEXT("pwmodel 0\npart p { pipe radial_steps=1 height_steps=300 }\n"),
      TEXT("pipe"), TEXT("radial_steps"), TEXT("radialSteps") },
    { TEXT("pwmodel 0\npart p { pipe radial_steps=1 height_steps=300 }\n"),
      TEXT("pipe"), TEXT("height_steps"), TEXT("heightSteps") },
    { TEXT("pwmodel 0\npart p { stairs num_steps=0 }\n"),
      TEXT("stairs"), TEXT("num_steps"), TEXT("numSteps") },
    { TEXT("pwmodel 0\npart p { spiral_stairs num_steps=0 }\n"),
      TEXT("spiral_stairs"), TEXT("num_steps"), TEXT("numSteps") },

    // The one modifier in the set. Two disjoint planes are two boundary loops, so loop index 9
    // is out of range and reports itself; `bridge` is the only op here whose clamp bound is
    // discovered from the mesh rather than declared.
    { TEXT("pwmodel 0\n")
      TEXT("part p {\n")
      TEXT("    plane size=(100, 100)\n")
      TEXT("    translate_mesh translation=(0, 0, 100)\n")
      TEXT("    plane size=(100, 100)\n")
      TEXT("    bridge edge_group_a=9 edge_group_b=1\n")
      TEXT("}\n"),
      TEXT("bridge"), TEXT("edge_group_a"), TEXT("edgeGroupA") },
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarningNamesCompiledDocumentsNameDocumentParametersTest,
    "PinWright.Model.WarningNames.CompiledDocumentsNameDocumentParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelWarningNamesCompiledDocumentsNameDocumentParametersTest::RunTest(const FString& Parameters)
{
    for (const FPwModelWarnNames_Case& Case : PwModelWarnNames_Cases)
    {
        const FPwModelCompileResult Result = PwModelWarnNames_Validate(Case.Source);
        const TArray<FString> Warnings = PwModelWarnNames_StageWarnings(Result);
        const FString Diagnostics = JoinPwDiagnostics(Result.Diagnostics);

        if (!TestTrue(*FString::Printf(TEXT("'%s %s' compiles. [%s]"),
                Case.OpName, Case.ModelLabel, *Diagnostics), Result.bSuccess))
        {
            continue;
        }

        // Op-qualified, so a warning raised by another op in the same document cannot satisfy
        // the assertion. The numbers are deliberately not matched - the floors are pinned by
        // docs/pwmodel-format.md's table and by TestGeometryOpsPrimitives; what is under test
        // here is the NAME.
        const FString Expected = FString::Printf(TEXT("'%s': %s clamped from "),
            Case.OpName, Case.ModelLabel);
        TestTrue(*FString::Printf(TEXT("'%s' reports its clamp as '%s'. [%s]"),
                Case.OpName, Case.ModelLabel, *Diagnostics),
            PwModelWarnNames_Any(Warnings, Expected));

        // And the RPC spelling is gone from the document surface entirely.
        const FString Forbidden = FString::Printf(TEXT("%s clamped from "), Case.RpcLabel);
        TestFalse(*FString::Printf(TEXT("'%s' no longer names the RPC parameter '%s'. [%s]"),
                Case.OpName, Case.RpcLabel, *Diagnostics),
            PwModelWarnNames_Any(Warnings, Forbidden));

        // The missing-row guard. EVERY `<token> clamped from` this compile produced has to name
        // something the op declares - so a new camelCase label added in GeometryOps without a
        // row in PwModelWarningNames fails here rather than reaching an author.
        const FString Prefix = FString::Printf(TEXT("'%s': "), Case.OpName);
        for (const FString& Warning : Warnings)
        {
            if (!Warning.StartsWith(Prefix, ESearchCase::CaseSensitive))
            {
                continue;
            }

            const FString Tail = Warning.RightChop(Prefix.Len());
            int32 Space = INDEX_NONE;
            if (!Tail.FindChar(TEXT(' '), Space))
            {
                continue;
            }
            if (!Tail.RightChop(Space + 1).StartsWith(TEXT("clamped from "), ESearchCase::CaseSensitive))
            {
                continue;  // prose, not a clamp report
            }

            const FString Base = PwModelWarnNames_BaseName(Tail.Left(Space));
            const FPwModelOpSpec* Spec =
                PwModelOpTable::Find(FString(Case.OpName), EPwModelOpContext::Part);
            TestTrue(*FString::Printf(
                    TEXT("'%s' clamp-warns about '%s', which is a parameter it publishes. [%s]"),
                    Case.OpName, *Base, *Warning),
                Spec != nullptr && Spec->FindParam(Base) != nullptr);
        }
    }

    return true;
}

// ============================================================================
// The RPC surface is unchanged - the fix is at the compiler seam, not the ops layer
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarningNamesOpsLayerKeepsRpcLabelsTest,
    "PinWright.Model.WarningNames.OpsLayerStillLabelsWithTheRpcParameter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelWarningNamesOpsLayerKeepsRpcLabelsTest::RunTest(const FString& Parameters)
{
    // These are the exact strings GeometryOps::AddOpWarnings puts on a geometry.* response, so
    // asserting them here asserts the wire text. The dispatcher-level counterpart lives in
    // TestGeometryOpWarningsOverDispatcher.cpp, which already pins numSteps and
    // widthSubdivisions on create_stairs / create_plane responses.
    {
        GeometryOps::FBoxParams Params;
        Params.Size = FVector(0.0, 0.0, 0.0);
        Params.Steps = FIntVector(300, 400, 500);
        const GeometryOps::FOpResult Op =
            GeometryOps::GenerateBox(PwModelWarnNames_TestMesh(), Params, FTransform::Identity);

        TestTrue(TEXT("GenerateBox still labels its extents width/height/depth"),
            PwModelWarnNames_OpWarns(Op, TEXT("width"))
            && PwModelWarnNames_OpWarns(Op, TEXT("height"))
            && PwModelWarnNames_OpWarns(Op, TEXT("depth")));
        TestTrue(TEXT("GenerateBox still labels its counts *Segments"),
            PwModelWarnNames_OpWarns(Op, TEXT("widthSegments"))
            && PwModelWarnNames_OpWarns(Op, TEXT("heightSegments"))
            && PwModelWarnNames_OpWarns(Op, TEXT("depthSegments")));
        TestFalse(TEXT("and does NOT emit the document's spelling"),
            PwModelWarnNames_OpWarns(Op, TEXT("size.x"))
            || PwModelWarnNames_OpWarns(Op, TEXT("segments.x")));
    }

    {
        GeometryOps::FPlaneParams Params;
        Params.Steps = FIntPoint(300, 400);
        const GeometryOps::FOpResult Op =
            GeometryOps::GeneratePlane(PwModelWarnNames_TestMesh(), Params, FTransform::Identity);

        TestTrue(TEXT("GeneratePlane still labels widthSubdivisions/depthSubdivisions"),
            PwModelWarnNames_OpWarns(Op, TEXT("widthSubdivisions"))
            && PwModelWarnNames_OpWarns(Op, TEXT("depthSubdivisions")));
    }

    {
        GeometryOps::FCylinderParams Params;
        Params.HeightSteps = 300;
        const GeometryOps::FOpResult Op =
            GeometryOps::GenerateCylinder(PwModelWarnNames_TestMesh(), Params, FTransform::Identity);

        TestTrue(TEXT("GenerateCylinder still labels heightSteps"),
            PwModelWarnNames_OpWarns(Op, TEXT("heightSteps")));
        TestFalse(TEXT("GenerateCylinder does not emit height_steps"),
            PwModelWarnNames_OpWarns(Op, TEXT("height_steps")));
    }

    {
        GeometryOps::FStairsParams Params;
        Params.NumSteps = 0;
        const GeometryOps::FOpResult Op =
            GeometryOps::GenerateStairs(PwModelWarnNames_TestMesh(), Params, FTransform::Identity);

        TestTrue(TEXT("GenerateStairs still labels numSteps"),
            PwModelWarnNames_OpWarns(Op, TEXT("numSteps")));
        TestFalse(TEXT("GenerateStairs does not emit num_steps"),
            PwModelWarnNames_OpWarns(Op, TEXT("num_steps")));
    }

    return true;
}

// ============================================================================
// Failures name document parameters too
// ============================================================================
//
// The compiler translated FOpResult::Warnings and forwarded FOpResult::ErrorMessage VERBATIM, so
// one op could report a clamp in the document's vocabulary and a refusal in the RPC's. Live:
//
//     'pipe' failed [INVALID_PARAMS]: pipe requires 0 < innerRadius < outerRadius;
//                                     got innerRadius=30, outerRadius=10
//
// - camelCase, naming two parameters `.pwmodel` does not have, for a condition
// docs/pwmodel-format.md states in snake_case. A failure is the diagnostic an author is most
// likely to read, and it was the one diagnostic exempt from the page's own normative rule.
//
// TranslateMessage is a WHOLE-WORD rewrite because a failure names its parameter mid-sentence,
// where the leading-token rule that is exactly right for `%s clamped from …` cannot reach.
// Translate itself is deliberately unchanged - TranslatesTheLeadingTokenOfItsOwnOpOnly above
// pins that warnings keep the strict rule, and this test pins the boundary between them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarningNamesTranslatesFailureMessagesTest,
    "PinWright.Model.WarningNames.FailureMessagesNameDocumentParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelWarningNamesTranslatesFailureMessagesTest::RunTest(const FString& Parameters)
{
    // ---- mid-sentence, every occurrence, and the `=` value form too ------------------------
    {
        const FString Raw =
            TEXT("pipe requires 0 < innerRadius < outerRadius; got innerRadius=30, outerRadius=10");
        const FString Translated = PwModelWarningNames::TranslateMessage(TEXT("pipe"), Raw);

        TestEqual(TEXT("every occurrence is rewritten, including the `name=value` form"), Translated,
            FString(TEXT("pipe requires 0 < inner_radius < outer_radius; ")
                    TEXT("got inner_radius=30, outer_radius=10")));
    }

    // ---- per-op, exactly like Translate ------------------------------------------------------
    {
        const FString Foreign = TEXT("sphere requires 0 < innerRadius");
        TestEqual(TEXT("a row belongs to its own op only"),
            PwModelWarningNames::TranslateMessage(TEXT("sphere"), Foreign), Foreign);
    }

    // ---- whole word on both sides ------------------------------------------------------------
    //
    // The trailing-boundary test is what stops the shorter `width` row from eating the head of
    // `widthSegments` and leaving `size.xSegments` - and what makes row ORDER irrelevant.
    {
        TestEqual(TEXT("a longer label that merely starts with a mapped one is left alone"),
            PwModelWarningNames::TranslateMessage(TEXT("box"), TEXT("widthSegmentsExtra is bad")),
            FString(TEXT("widthSegmentsExtra is bad")));
        TestEqual(TEXT("and the longer row still matches when it is the whole word"),
            PwModelWarningNames::TranslateMessage(TEXT("box"), TEXT("widthSegments is bad")),
            FString(TEXT("segments.x is bad")));
        TestEqual(TEXT("a label glued to an identifier tail is not a word"),
            PwModelWarningNames::TranslateMessage(TEXT("pipe"), TEXT("myInnerRadius_2 is bad")),
            FString(TEXT("myInnerRadius_2 is bad")));
    }

    // ---- the leading-token rewrite still happens through this entry point ---------------------
    {
        TestEqual(TEXT("a clamp label reaching a failure keeps Translate's rewrite"),
            PwModelWarningNames::TranslateMessage(TEXT("cylinder"),
                TEXT("heightSteps clamped from 300 to 256")),
            FString(TEXT("height_steps clamped from 300 to 256")));
    }

    // ---- an op with no rows forwards everything unchanged ------------------------------------
    {
        const FString Untouched = TEXT("profile has 2 point(s); at least 3 are needed");
        TestEqual(TEXT("no rows, no rewrite"),
            PwModelWarningNames::TranslateMessage(TEXT("revolve"), Untouched), Untouched);
    }

    // ---- end to end, through the real compiler -----------------------------------------------
    {
        const FPwModelCompileResult Result = PwModelWarnNames_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part bore {\n")
            TEXT("    pipe inner_radius=30 outer_radius=10 height=100\n")
            TEXT("}\n"));

        TestFalse(*FString::Printf(TEXT("an unordered pair is refused, not clamped. [%s]"),
                *JoinPwDiagnostics(Result.Diagnostics)),
            Result.bSuccess);

        FString Failure;
        for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
        {
            if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_OP_FAILED)
            {
                Failure = Diagnostic.Message;
                break;
            }
        }

        if (Failure.IsEmpty())
        {
            AddError(FString::Printf(TEXT("expected PWMODEL_OP_FAILED. [%s]"),
                *JoinPwDiagnostics(Result.Diagnostics)));
            return true;
        }

        TestTrue(*FString::Printf(TEXT("names inner_radius: '%s'"), *Failure),
            Failure.Contains(TEXT("inner_radius"), ESearchCase::CaseSensitive));
        TestTrue(*FString::Printf(TEXT("names outer_radius: '%s'"), *Failure),
            Failure.Contains(TEXT("outer_radius"), ESearchCase::CaseSensitive));
        TestFalse(*FString::Printf(TEXT("and the RPC spellings are gone: '%s'"), *Failure),
            Failure.Contains(TEXT("innerRadius"), ESearchCase::CaseSensitive)
            || Failure.Contains(TEXT("outerRadius"), ESearchCase::CaseSensitive));
    }

    return true;
}

// ============================================================================
// A remedy an author can act on
// ============================================================================
//
// The other tests in this file are about a warning's LABEL. This one is about a warning's
// REMEDY, and it is the harder failure: a wrong label sends an author looking for a parameter
// that does not exist, while a wrong remedy tells them to write an op that does not exist and
// costs a compile to find out.
//
// RecomputeNormals warns whenever the target's normal overlay is empty and it falls back to
// per-vertex normals (MeshNormalsFunctions.cpp, `PrimaryNormals()->ElementCount() == 0`) - the
// ordinary case for a part built from `append_buffers` with no `normals=`. Its remedy names two
// GeometryScript BLUEPRINT NODE titles, "Set Mesh To Per Vertex Normals" and "Compute Split
// Normals". `.pwmodel` publishes neither: `split_normals` wraps the second and the first has no
// spelling at all, so an author who did what the sentence said reached PWSRC_UNKNOWN_OP.
//
// PwModelWarningNames cannot reach this one, which is why the fix is not in the table the tests
// above pin: Translate rewrites the LEADING PARAMETER TOKEN of a clamp message, and these are op
// names buried in prose. The rewrite happens where the message enters an FOpResult, in
// FGeometryScriptDebugSink::DrainWarningsInto - the one place every engine warning crosses into
// this module.
//
// What this would let through if reverted: any remedy naming a verb the format does not have.
// The assertion is therefore not "it says split_normals" but "every op-shaped name it says is
// real", checked against PwModelOpTable rather than a second hand-written list.

namespace
{
// Every `snake_case` token in a warning has to name something the format publishes: an op, or a
// parameter of an op the same warning names. `split_normals` passes because it is an op;
// `compute_split_normals` - the engine's remedy, transliterated - does not, which is the exact
// failure this guards.
void PwModelWarnNames_CheckOpNames(FAutomationTestBase& Test, const FString& Warning)
{
    TArray<FString> Tokens;
    FString Current;
    for (int32 Index = 0; Index <= Warning.Len(); ++Index)
    {
        const TCHAR Char = Index < Warning.Len() ? Warning[Index] : TEXT(' ');
        const bool bWord = (Char >= TEXT('a') && Char <= TEXT('z'))
            || (Char >= TEXT('0') && Char <= TEXT('9'))
            || Char == TEXT('_');
        if (bWord)
        {
            Current.AppendChar(Char);
            continue;
        }
        if (Current.Contains(TEXT("_"), ESearchCase::CaseSensitive))
        {
            Tokens.AddUnique(Current);
        }
        Current.Reset();
    }

    // Ops first: a parameter name is only legal in a remedy that also names the op declaring it,
    // because a bare parameter is not something an author can write on a line of its own either.
    TArray<const FPwModelOpSpec*> Named;
    for (const FString& Token : Tokens)
    {
        if (const FPwModelOpSpec* Spec = PwModelOpTable::Find(Token, EPwModelOpContext::Part))
        {
            Named.Add(Spec);
        }
    }

    for (const FString& Token : Tokens)
    {
        if (PwModelOpTable::Find(Token, EPwModelOpContext::Part) != nullptr)
        {
            continue;
        }

        bool bDeclared = false;
        for (const FPwModelOpSpec* Spec : Named)
        {
            bDeclared = bDeclared || Spec->FindParam(Token) != nullptr;
        }

        Test.TestTrue(*FString::Printf(
                TEXT("'%s' is an op this format publishes, or a parameter of one the same "
                     "warning names. [%s]"),
                *Token, *Warning),
            bDeclared);
    }
}

// The blueprint-node titles the engine writes. Neither is a verb of this format on either
// surface, so neither may reach an author.
const TCHAR* const PwModelWarnNames_EngineNodeTitles[] =
{
    TEXT("Set Mesh To Per Vertex Normals"),
    TEXT("Compute Split Normals"),
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelWarningNamesNormalsFallbackRemedyTest,
    "PinWright.Model.WarningNames.NormalsFallbackRemedyNamesOnlyRealOps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelWarningNamesNormalsFallbackRemedyTest::RunTest(const FString& Parameters)
{
    // ---- the ops layer: a mesh that never had a normal overlay -------------------------------
    {
        const GeometryOps::FRecalculateNormalsParams Params;
        const GeometryOps::FOpResult Op =
            GeometryOps::RecalculateNormals(PwModelWarnNames_TestMesh(), Params);

        FString Fallback;
        for (const FString& Warning : Op.Warnings)
        {
            if (Warning.Contains(TEXT("did not have normals to recompute"), ESearchCase::CaseSensitive))
            {
                Fallback = Warning;
                break;
            }
        }

        if (Fallback.IsEmpty())
        {
            AddError(FString::Printf(
                TEXT("expected the per-vertex fallback warning on a mesh with no normal "
                     "overlay; got [%s]"),
                *FString::Join(Op.Warnings, TEXT(" | "))));
            return true;
        }

        // The DIAGNOSIS is the engine's and stays verbatim. Suppressing the whole warning would
        // cost the caller the one fact it carries - that the normals it got are of a different
        // KIND than the ones it asked for - so only the remedy clause is rewritten.
        TestTrue(*FString::Printf(TEXT("the diagnosis survives: '%s'"), *Fallback),
            Fallback.Contains(TEXT("falling back to per-vertex normals"), ESearchCase::CaseSensitive));

        for (const TCHAR* Title : PwModelWarnNames_EngineNodeTitles)
        {
            TestFalse(
                *FString::Printf(TEXT("the remedy no longer names '%s': '%s'"), Title, *Fallback),
                Fallback.Contains(Title, ESearchCase::CaseSensitive));
        }

        TestTrue(*FString::Printf(TEXT("and names split_normals instead: '%s'"), *Fallback),
            Fallback.Contains(TEXT("split_normals"), ESearchCase::CaseSensitive));

        PwModelWarnNames_CheckOpNames(*this, Fallback);
    }

    // ---- end to end: the reported repro, buffers carrying no `normals=` -----------------------
    {
        const FPwModelCompileResult Result = PwModelWarnNames_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part p {\n")
            TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (50, 100, 0)] ")
            TEXT("triangles=[(0, 1, 2)]\n")
            TEXT("    recalculate_normals\n")
            TEXT("}\n"));

        const TArray<FString> Warnings = PwModelWarnNames_StageWarnings(Result);
        const FString Diagnostics = JoinPwDiagnostics(Result.Diagnostics);

        if (!TestTrue(*FString::Printf(TEXT("the repro document compiles. [%s]"), *Diagnostics),
                Result.bSuccess))
        {
            return true;
        }

        FString Staged;
        for (const FString& Warning : Warnings)
        {
            if (Warning.Contains(TEXT("did not have normals to recompute"), ESearchCase::CaseSensitive))
            {
                Staged = Warning;
                break;
            }
        }

        if (Staged.IsEmpty())
        {
            AddError(FString::Printf(
                TEXT("expected the fallback to reach the author as PWMODEL_STAGE_WARNING. [%s]"),
                *Diagnostics));
            return true;
        }

        for (const TCHAR* Title : PwModelWarnNames_EngineNodeTitles)
        {
            TestFalse(
                *FString::Printf(TEXT("the compiled warning does not name '%s': '%s'"),
                    Title, *Staged),
                Staged.Contains(Title, ESearchCase::CaseSensitive));
        }

        // Both op names in the compiled text - the prefix the compiler adds and the remedy's
        // own - are things this document could have been written with.
        PwModelWarnNames_CheckOpNames(*this, Staged);
    }

    return true;
}
