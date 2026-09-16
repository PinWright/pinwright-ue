// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the ONE mesh defect class the health block could not express: an
// inverted closed shell.
//
// A closed mesh whose winding is uniformly reversed renders identically to a correct one -
// backface culling shows whichever wall faces the camera, and the two walls carry opposite
// normals - so every signal the response carried before this file agrees between the two:
// isClosed, boundaryEdges, degenerateTriangles, nonManifoldVertices, componentCount and both
// mesh counts are BYTE-IDENTICAL. An A/B luminance comparison of a render moves by 0.000004,
// a measurement that cannot fail, and it was twice quoted as proof a shipped mesh was fine.
//
// It is not cosmetic. The mesh distance field decides inside from outside by counting backface
// hits (MeshDistanceFieldUtilities.cpp:261-281), so an inverted shell inverts its field and
// Lumen/DFAO light the part as though the camera were inside it - invisible in an asset
// preview, wrong in a lit level.
//
// Every test here is written as a DIFFERENCE between two documents that agree on everything
// else, because that is the property under test: the two documents must be separable, and they
// must be separable by exactly the field this change added.
//
//  - InvertedClosedShellSeparatesOnlyInSignedVolume  - the uniform inversion. Asserts every
//    pre-existing field is equal and signedVolume is equal and opposite. Reverting the change
//    leaves the two documents indistinguishable, which is the state that shipped the defect.
//  - PartlyInvertedSheetSeparatesOnlyInOrientationConsistent - the nastier fault: two triangles
//    sharing an edge that disagree on winding. signedVolume cannot see it (the mesh is open),
//    so this is the second signal's whole reason to exist.
//  - OneInvertedPartIsVisibleOnlyPerPart - the realistic case. A model-wide signed volume
//    AVERAGES an inverted part away: 8000 + (-1000) reads as a healthy 7000. Only the per-part
//    number names the part.
//  - ExtrudeDirectionOpposedToFacingWarns - the authoring mistake that produces all of the
//    above, caught at the call site instead of at the merge.
//
// Driven through the real dispatcher rather than FPwModelCompiler directly: the fields are a
// WIRE contract, and a compiler that measures a number the handler never marshals is the same
// blindness one layer up.
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

// One model.validate run. diagnosticLimit=0 and collapseDiagnostics=false on purpose: the
// defaults (5, collapsed) can drop or fold the one warning a test is looking for, and a test
// that silently stops seeing its own subject is worse than no test.
struct FPwModelOrientTest_Run
{
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
};

FPwModelOrientTest_Run PwModelOrientTest_Validate(const TCHAR* Source)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), Source);
    Payload->SetNumberField(TEXT("diagnosticLimit"), 0);
    Payload->SetBoolField(TEXT("collapseDiagnostics"), false);

    FPwModelOrientTest_Run Run;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-orient"),
        Payload, Run.bSuccess, Run.Result, Run.ErrorCode);
    return Run;
}

// Every failure message carries the whole diagnostic list; "expected true, got false" on a
// compiler test costs a rerun under a debugger to learn anything at all.
FString PwModelOrientTest_Describe(const FPwModelOrientTest_Run& Run)
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

    FString HealthText = TEXT("<no health>");
    const TSharedPtr<FJsonObject>* Health = nullptr;
    if (Run.Result->TryGetObjectField(TEXT("health"), Health) && (*Health).IsValid())
    {
        FString Serialized;
        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : (*Health)->Values)
        {
            Serialized += FString::Printf(TEXT("%s=%s "), *Field.Key,
                Field.Value.IsValid() ? *Field.Value->AsString() : TEXT("?"));
        }
        HealthText = Serialized;
    }

    return FString::Printf(TEXT("success=%d error='%s' health={%s} diagnostics=[%s]"),
        Run.bSuccess ? 1 : 0, *Run.ErrorCode, *HealthText, *FString::Join(Lines, TEXT("; ")));
}

TSharedPtr<FJsonObject> PwModelOrientTest_Health(const FPwModelOrientTest_Run& Run)
{
    const TSharedPtr<FJsonObject>* Health = nullptr;
    if (Run.Result.IsValid() && Run.Result->TryGetObjectField(TEXT("health"), Health))
    {
        return *Health;
    }
    return nullptr;
}

// The `parts` entry with this name, or null. Parts are informational sub-regions of the one
// output mesh, so they are matched by name rather than by index.
TSharedPtr<FJsonObject> PwModelOrientTest_Part(const FPwModelOrientTest_Run& Run, const TCHAR* PartName)
{
    const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
    if (!Run.Result.IsValid() || !Run.Result->TryGetArrayField(TEXT("parts"), Parts) || !Parts)
    {
        return nullptr;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Parts)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry).IsValid()
            && (*Entry)->GetStringField(TEXT("partName")) == PartName)
        {
            return *Entry;
        }
    }
    return nullptr;
}

bool PwModelOrientTest_HasCode(const FPwModelOrientTest_Run& Run, const TCHAR* Code)
{
    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    if (!Run.Result.IsValid() || !Run.Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics)
        || !Diagnostics)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry).IsValid()
            && (*Entry)->GetStringField(TEXT("code")) == Code)
        {
            return true;
        }
    }
    return false;
}

// A field read that FAILS the test when the field is absent, rather than defaulting to 0.
// GetNumberField answers 0 for a missing field, and 0 is a legal signed volume - so the
// defaulting read would turn "the signal does not exist" into "the signal says nothing is
// wrong", which is the exact failure mode this whole file exists to prevent.
double PwModelOrientTest_RequireNumber(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Object,
                                       const TCHAR* Field, const TCHAR* Where)
{
    double Value = 0.0;
    if (!Test.TestTrue(*FString::Printf(TEXT("%s carries '%s'"), Where, Field),
            Object.IsValid() && Object->TryGetNumberField(Field, Value)))
    {
        return 0.0;
    }
    return Value;
}

bool PwModelOrientTest_RequireBool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Object,
                                   const TCHAR* Field, const TCHAR* Where)
{
    bool bValue = false;
    if (!Test.TestTrue(*FString::Printf(TEXT("%s carries '%s'"), Where, Field),
            Object.IsValid() && Object->TryGetBoolField(Field, bValue)))
    {
        return false;
    }
    return bValue;
}

// The diagnostic code the compiler raises when `extrude`'s `direction` opposes the surface's
// own facing normal. Spelled as a LITERAL rather than through PwModelDiagnosticCodes so this
// file compiles - and therefore fails - against a tree that does not have the code yet. It is
// also what the wire actually carries, which is the contract under test.
const TCHAR* const PwModelOrientTest_FacingOpposed = TEXT("PWMODEL_EXTRUDE_FACING_OPPOSED");

// ---------------------------------------------------------------------------------------
// Documents. Each pair differs in ONE token.
// ---------------------------------------------------------------------------------------

const TCHAR* const PwModelOrientTest_ClosedBox =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(10, 10, 10)\n")
    TEXT("}\n");

const TCHAR* const PwModelOrientTest_InvertedBox =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(10, 10, 10)\n")
    TEXT("    flip_normals\n")
    TEXT("}\n");

// Two triangles sharing edge 0-2. Wound consistently, T0 traverses it 2->0 and T1 traverses
// it 0->2, which is what "adjacent triangles agree" means.
const TCHAR* const PwModelOrientTest_ConsistentSheet =
    TEXT("pwmodel 0\n")
    TEXT("part sheet {\n")
    TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)] ")
    TEXT("triangles=[(0, 1, 2), (0, 2, 3)]\n")
    TEXT("}\n");

// The same four vertices and the same two triangles, with the second one's last two corners
// swapped: both triangles now traverse edge 0-2 as 2->0. Same triangle count, same vertex
// count, same boundary loop - and one wall of the pair faces the other way.
const TCHAR* const PwModelOrientTest_PartlyInvertedSheet =
    TEXT("pwmodel 0\n")
    TEXT("part sheet {\n")
    TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)] ")
    TEXT("triangles=[(0, 1, 2), (0, 3, 2)]\n")
    TEXT("}\n");

// A correct 20-cube and an inverted 10-cube, disjoint. Model-wide signed volume is
// 8000 + (-1000) = 7000, comfortably positive: the merged number says the model is fine.
const TCHAR* const PwModelOrientTest_OneInvertedPart =
    TEXT("pwmodel 0\n")
    TEXT("part good {\n")
    TEXT("    box size=(20, 20, 20)\n")
    TEXT("}\n")
    TEXT("part bad {\n")
    TEXT("    box size=(10, 10, 10) at=(100, 0, 0)\n")
    TEXT("    flip_normals\n")
    TEXT("}\n");

// A flat sheet given a thickness. Whole-mesh `extrude` (no face_direction) displaces the
// ORIGINAL triangles along `direction` keeping their winding, and reverses the stationary
// duplicate - so `direction` must point along the sheet's own facing normal or the slab comes
// out inside out. The two documents below differ only in that sign.
const TCHAR* const PwModelOrientTest_ExtrudePlusZ =
    TEXT("pwmodel 0\n")
    TEXT("part sheet {\n")
    TEXT("    plane size=(100, 100)\n")
    TEXT("    extrude distance=10 direction=(0, 0, 1)\n")
    TEXT("}\n");

const TCHAR* const PwModelOrientTest_ExtrudeMinusZ =
    TEXT("pwmodel 0\n")
    TEXT("part sheet {\n")
    TEXT("    plane size=(100, 100)\n")
    TEXT("    extrude distance=10 direction=(0, 0, -1)\n")
    TEXT("}\n");
}

// ============================================================================
// Uniform inversion: separable only in signedVolume
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelInvertedShellSignedVolumeTest,
    "PinWright.Model.Compiler.InvertedClosedShellSeparatesOnlyInSignedVolume",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelInvertedShellSignedVolumeTest::RunTest(const FString& Parameters)
{
    const FPwModelOrientTest_Run Correct = PwModelOrientTest_Validate(PwModelOrientTest_ClosedBox);
    const FPwModelOrientTest_Run Inverted = PwModelOrientTest_Validate(PwModelOrientTest_InvertedBox);

    if (!TestTrue(*FString::Printf(TEXT("the correct box validates. %s"),
            *PwModelOrientTest_Describe(Correct)), Correct.bSuccess))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("the inverted box validates. %s"),
            *PwModelOrientTest_Describe(Inverted)), Inverted.bSuccess))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> CorrectHealth = PwModelOrientTest_Health(Correct);
    const TSharedPtr<FJsonObject> InvertedHealth = PwModelOrientTest_Health(Inverted);
    if (!TestTrue(TEXT("both runs report a health block"),
            CorrectHealth.IsValid() && InvertedHealth.IsValid()))
    {
        return false;
    }

    // The premise. Every one of these is why the defect was twice declared absent: a uniform
    // winding reversal moves NONE of them. If one of them starts differing, the reasoning
    // behind signedVolume needs revisiting, so they are asserted rather than assumed.
    const TCHAR* const Unchanged[] = {
        TEXT("boundaryEdges"), TEXT("degenerateTriangles"),
        TEXT("nonManifoldVertices"), TEXT("componentCount"),
    };
    for (const TCHAR* Field : Unchanged)
    {
        double CorrectValue = 0.0;
        double InvertedValue = 0.0;
        CorrectHealth->TryGetNumberField(Field, CorrectValue);
        InvertedHealth->TryGetNumberField(Field, InvertedValue);
        TestEqual(*FString::Printf(
            TEXT("'%s' cannot separate a correct box from an inverted one"), Field),
            InvertedValue, CorrectValue);
    }
    TestEqual(TEXT("'isClosed' cannot separate them either"),
        InvertedHealth->GetBoolField(TEXT("isClosed")),
        CorrectHealth->GetBoolField(TEXT("isClosed")));
    TestEqual(TEXT("nor can the triangle count"),
        Inverted.Result->GetNumberField(TEXT("meshTriangleCount")),
        Correct.Result->GetNumberField(TEXT("meshTriangleCount")));

    // Both are closed, so signedVolume is meaningful on both.
    TestTrue(TEXT("the correct box is closed"),
        PwModelOrientTest_RequireBool(*this, CorrectHealth, TEXT("isClosed"), TEXT("correct health")));
    TestTrue(TEXT("the inverted box is closed too - that is the whole problem"),
        PwModelOrientTest_RequireBool(*this, InvertedHealth, TEXT("isClosed"), TEXT("inverted health")));

    const double CorrectVolume =
        PwModelOrientTest_RequireNumber(*this, CorrectHealth, TEXT("signedVolume"), TEXT("correct health"));
    const double InvertedVolume =
        PwModelOrientTest_RequireNumber(*this, InvertedHealth, TEXT("signedVolume"), TEXT("inverted health"));

    // size= is FULL extents, so the box encloses 10 * 10 * 10.
    TestTrue(*FString::Printf(TEXT("a correctly wound 10-cube has POSITIVE signed volume, got %g. %s"),
            CorrectVolume, *PwModelOrientTest_Describe(Correct)),
        CorrectVolume > 0.0);
    TestTrue(*FString::Printf(TEXT("the same cube wound inside out has NEGATIVE signed volume, got %g. %s"),
            InvertedVolume, *PwModelOrientTest_Describe(Inverted)),
        InvertedVolume < 0.0);
    TestTrue(*FString::Printf(
            TEXT("it is the SAME shell: the two volumes are equal and opposite (%g vs %g)"),
            CorrectVolume, InvertedVolume),
        FMath::IsNearlyZero(CorrectVolume + InvertedVolume, 1e-6 * FMath::Abs(CorrectVolume) + 1e-6));
    TestTrue(*FString::Printf(TEXT("and it is the enclosed volume, 1000, not some other number: %g"),
            CorrectVolume),
        FMath::IsNearlyEqual(CorrectVolume, 1000.0, 1.0));

    // A UNIFORM reversal leaves every pair of neighbours agreeing, so the second signal is
    // deliberately blind here. Pinned so nobody "fixes" orientationConsistent to cover this
    // case and quietly removes the reason signedVolume exists.
    TestTrue(TEXT("a uniformly inverted shell is still orientation-CONSISTENT"),
        PwModelOrientTest_RequireBool(*this, InvertedHealth, TEXT("orientationConsistent"),
            TEXT("inverted health")));

    return true;
}

// ============================================================================
// Partial inversion: separable only in orientationConsistent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelPartlyInvertedSheetTest,
    "PinWright.Model.Compiler.PartlyInvertedSheetSeparatesOnlyInOrientationConsistent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelPartlyInvertedSheetTest::RunTest(const FString& Parameters)
{
    const FPwModelOrientTest_Run Consistent =
        PwModelOrientTest_Validate(PwModelOrientTest_ConsistentSheet);
    const FPwModelOrientTest_Run Inconsistent =
        PwModelOrientTest_Validate(PwModelOrientTest_PartlyInvertedSheet);

    if (!TestTrue(*FString::Printf(TEXT("the consistent sheet validates. %s"),
            *PwModelOrientTest_Describe(Consistent)), Consistent.bSuccess))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("the partly inverted sheet validates. %s"),
            *PwModelOrientTest_Describe(Inconsistent)), Inconsistent.bSuccess))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> ConsistentHealth = PwModelOrientTest_Health(Consistent);
    const TSharedPtr<FJsonObject> InconsistentHealth = PwModelOrientTest_Health(Inconsistent);
    if (!TestTrue(TEXT("both runs report a health block"),
            ConsistentHealth.IsValid() && InconsistentHealth.IsValid()))
    {
        return false;
    }

    // Same four vertices, same two triangles, same boundary loop. Nothing that counts
    // elements can tell these apart - and signedVolume cannot either, because an open
    // surface encloses nothing.
    TestEqual(TEXT("both sheets carry the same triangle count"),
        Inconsistent.Result->GetNumberField(TEXT("meshTriangleCount")),
        Consistent.Result->GetNumberField(TEXT("meshTriangleCount")));
    TestEqual(TEXT("both sheets carry the same boundary-edge count"),
        InconsistentHealth->GetNumberField(TEXT("boundaryEdges")),
        ConsistentHealth->GetNumberField(TEXT("boundaryEdges")));
    TestFalse(TEXT("both sheets are open, so signedVolume is meaningless on both"),
        ConsistentHealth->GetBoolField(TEXT("isClosed")));

    TestTrue(TEXT("two triangles wound the same way around a shared edge are consistent"),
        PwModelOrientTest_RequireBool(*this, ConsistentHealth, TEXT("orientationConsistent"),
            TEXT("consistent health")));
    TestFalse(*FString::Printf(
            TEXT("one triangle flipped against its neighbour is NOT consistent. %s"),
            *PwModelOrientTest_Describe(Inconsistent)),
        PwModelOrientTest_RequireBool(*this, InconsistentHealth, TEXT("orientationConsistent"),
            TEXT("inconsistent health")));
    TestEqual(*FString::Printf(TEXT("and it names the one edge that disagrees. %s"),
            *PwModelOrientTest_Describe(Inconsistent)),
        PwModelOrientTest_RequireNumber(*this, InconsistentHealth, TEXT("inconsistentEdges"),
            TEXT("inconsistent health")),
        1.0);
    TestEqual(TEXT("the consistent sheet reports zero disagreeing edges"),
        PwModelOrientTest_RequireNumber(*this, ConsistentHealth, TEXT("inconsistentEdges"),
            TEXT("consistent health")),
        0.0);

    return true;
}

// ============================================================================
// One inverted part inside an otherwise correct model
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelOneInvertedPartTest,
    "PinWright.Model.Compiler.OneInvertedPartIsVisibleOnlyPerPart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelOneInvertedPartTest::RunTest(const FString& Parameters)
{
    const FPwModelOrientTest_Run Run = PwModelOrientTest_Validate(PwModelOrientTest_OneInvertedPart);
    if (!TestTrue(*FString::Printf(TEXT("the two-part document validates. %s"),
            *PwModelOrientTest_Describe(Run)), Run.bSuccess))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Health = PwModelOrientTest_Health(Run);
    if (!TestTrue(TEXT("the run reports a health block"), Health.IsValid()))
    {
        return false;
    }

    // The trap this test exists for. 8000 from the good part plus -1000 from the bad one is a
    // healthy-looking 7000, so the model-wide number CANNOT be the gate for a multi-part
    // document. Asserted positive on purpose: if this ever goes negative the test below stops
    // proving anything.
    const double MergedVolume =
        PwModelOrientTest_RequireNumber(*this, Health, TEXT("signedVolume"), TEXT("merged health"));
    TestTrue(*FString::Printf(
            TEXT("the MERGED signed volume averages the inverted part away and reads positive: %g"),
            MergedVolume),
        MergedVolume > 0.0);

    const TSharedPtr<FJsonObject> Good = PwModelOrientTest_Part(Run, TEXT("good"));
    const TSharedPtr<FJsonObject> Bad = PwModelOrientTest_Part(Run, TEXT("bad"));
    if (!TestTrue(TEXT("both parts are reported by name"), Good.IsValid() && Bad.IsValid()))
    {
        return false;
    }

    TestTrue(TEXT("the correct part reports a positive signed volume"),
        PwModelOrientTest_RequireNumber(*this, Good, TEXT("signedVolume"), TEXT("part 'good'")) > 0.0);
    TestTrue(*FString::Printf(TEXT("the inverted part reports a NEGATIVE signed volume. %s"),
            *PwModelOrientTest_Describe(Run)),
        PwModelOrientTest_RequireNumber(*this, Bad, TEXT("signedVolume"), TEXT("part 'bad'")) < 0.0);
    TestTrue(TEXT("both parts are closed, so both per-part volumes are meaningful"),
        PwModelOrientTest_RequireBool(*this, Good, TEXT("isClosed"), TEXT("part 'good'"))
        && PwModelOrientTest_RequireBool(*this, Bad, TEXT("isClosed"), TEXT("part 'bad'")));

    return true;
}

// ============================================================================
// The authoring mistake that produces an inverted shell
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelExtrudeFacingOpposedTest,
    "PinWright.Model.Compiler.ExtrudeDirectionOpposedToFacingWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelExtrudeFacingOpposedTest::RunTest(const FString& Parameters)
{
    const FPwModelOrientTest_Run PlusZ = PwModelOrientTest_Validate(PwModelOrientTest_ExtrudePlusZ);
    const FPwModelOrientTest_Run MinusZ = PwModelOrientTest_Validate(PwModelOrientTest_ExtrudeMinusZ);

    if (!TestTrue(*FString::Printf(TEXT("extrude direction=(0,0,1) validates. %s"),
            *PwModelOrientTest_Describe(PlusZ)), PlusZ.bSuccess))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("extrude direction=(0,0,-1) validates. %s"),
            *PwModelOrientTest_Describe(MinusZ)), MinusZ.bSuccess))
    {
        return false;
    }

    const bool bPlusWarned = PwModelOrientTest_HasCode(PlusZ, PwModelOrientTest_FacingOpposed);
    const bool bMinusWarned = PwModelOrientTest_HasCode(MinusZ, PwModelOrientTest_FacingOpposed);

    // Deliberately NOT hard-coding which sign is right for `plane`. The engine owns the
    // generator's winding and could change it; what must hold is that the warning tracks the
    // facing, so exactly one of an opposed pair fires.
    if (!TestTrue(*FString::Printf(
            TEXT("exactly one of the two opposed extrude directions warns %s (+Z warned=%d, -Z warned=%d). ")
            TEXT("+Z: %s | -Z: %s"),
            PwModelOrientTest_FacingOpposed, bPlusWarned ? 1 : 0, bMinusWarned ? 1 : 0,
            *PwModelOrientTest_Describe(PlusZ), *PwModelOrientTest_Describe(MinusZ)),
            bPlusWarned != bMinusWarned))
    {
        return false;
    }

    const FPwModelOrientTest_Run& Warned = bPlusWarned ? PlusZ : MinusZ;
    const FPwModelOrientTest_Run& Clean = bPlusWarned ? MinusZ : PlusZ;

    const TSharedPtr<FJsonObject> WarnedHealth = PwModelOrientTest_Health(Warned);
    const TSharedPtr<FJsonObject> CleanHealth = PwModelOrientTest_Health(Clean);
    if (!TestTrue(TEXT("both runs report a health block"),
            WarnedHealth.IsValid() && CleanHealth.IsValid()))
    {
        return false;
    }

    // The warning is only worth anything if it fires on the run that is ACTUALLY inverted.
    // Both slabs are closed and identical in every count; the signed volume is what says which
    // one is inside out, and it must agree with the warning.
    TestTrue(TEXT("both slabs are closed"),
        PwModelOrientTest_RequireBool(*this, WarnedHealth, TEXT("isClosed"), TEXT("warned health"))
        && PwModelOrientTest_RequireBool(*this, CleanHealth, TEXT("isClosed"), TEXT("clean health")));

    const double WarnedVolume =
        PwModelOrientTest_RequireNumber(*this, WarnedHealth, TEXT("signedVolume"), TEXT("warned health"));
    const double CleanVolume =
        PwModelOrientTest_RequireNumber(*this, CleanHealth, TEXT("signedVolume"), TEXT("clean health"));

    TestTrue(*FString::Printf(TEXT("the run the warning fired on IS the inverted one (%g < 0). %s"),
            WarnedVolume, *PwModelOrientTest_Describe(Warned)),
        WarnedVolume < 0.0);
    TestTrue(*FString::Printf(TEXT("and the quiet run is the correct one (%g > 0). %s"),
            CleanVolume, *PwModelOrientTest_Describe(Clean)),
        CleanVolume > 0.0);

    // The message has to name the parameter the author has to change. A warning that says
    // "the shell is inside out" without naming `direction` sends them to the wrong line.
    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    bool bNamesDirection = false;
    if (Warned.Result.IsValid() && Warned.Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics)
        && Diagnostics)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && (*Entry).IsValid()
                && (*Entry)->GetStringField(TEXT("code")) == PwModelOrientTest_FacingOpposed)
            {
                bNamesDirection =
                    (*Entry)->GetStringField(TEXT("message")).Contains(TEXT("direction"));
            }
        }
    }
    TestTrue(*FString::Printf(TEXT("the warning names 'direction'. %s"),
            *PwModelOrientTest_Describe(Warned)), bNamesDirection);

    return true;
}
