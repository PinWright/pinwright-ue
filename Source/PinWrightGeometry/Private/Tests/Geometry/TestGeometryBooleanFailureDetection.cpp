// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the defect that PinWright could not detect a FAILED BOOLEAN AT ALL.
//
// Every error path in the boolean family was dead code. GeometryOps::Boolean and
// GeometryOps::Trim gated ERR_BOOLEAN_FAILED on `if (!ResultMesh)`, and
// UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean never returns null: it returns
// its TargetMesh from both null-input guards (MeshBooleanFunctions.cpp:38-47) AND from the
// empty-result refusal (:96-99), reporting the refusal only through the UGeometryScriptDebug
// argument - which both ops passed as nullptr. The detection channel was discarded, so the check
// could not fire. geometry.boolean_union / boolean_subtract / boolean_intersection /
// boolean_trim all answered SUCCESS over a boolean the engine had declined to perform, leaving
// the caller building on the unmodified target.
//
// The fix is Handlers/Geometry/GeometryScriptDebugSink.h: the ops construct a real
// UGeometryScriptDebug, pass it, and turn a reported error into an FOpResult failure carrying
// ERR_BOOLEAN_FAILED and the engine's own message text.
//
// WHY THESE TESTS AND NOT THE EXISTING ONES. The failure tests that already existed assert
// ExpectedCode on NULL-HANDLE inputs, which the ops reject one line before they reach the
// engine - so they pinned the plugin's own guard and never exercised the engine at all, and
// stayed green for the entire life of the defect. Every test in this file drives a boolean the
// ENGINE refuses, and the dispatcher case drives it end to end through the real RPC surface.
//
// WHAT IS REACHABLE, stated plainly rather than left to be inferred:
//
//   REACHABLE - the empty-result refusal. `!Options.bAllowEmptyResult && result has 0 triangles`
//     (MeshBooleanFunctions.cpp:95). Constructed here two ways: a Subtract whose tool encloses
//     the target, and an Intersection of two disjoint meshes. Both are ordinary inputs a caller
//     can pass by accident, which is what made the silence expensive.
//
//   NOT REACHABLE - GeometryOps::Mirror, on UE 5.8, still has no engine failure any input can
//     provoke, so there is deliberately NO mirror failure test here rather than one that reports
//     green without asserting. Its three engine calls (ScaleMesh, AppendMesh, WeldMeshEdges) all
//     return their TargetMesh on every path, and now all three receive a live sink - but
//     ScaleMesh and AppendMesh raise only null-mesh errors, and WeldMeshEdges' "Weld Operation
//     returned error flag" is gated on FMergeCoincidentMeshEdges::Apply() returning false, which
//     that function never does: it has a single return statement and it is `return true`
//     (GeometryCore/Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp:233). Mirror's
//     ERR_OPERATION_FAILED is therefore still unreachable - by a different mechanism than
//     before, one engine layer further down. The wiring is in place so that an engine which
//     starts raising that flag is detected instead of silently ignored.
//
//   NOT REACHABLE - the `if (!ResultMesh)` arms retained behind the sink checks. They can only
//     fire for a null target, which BeginOp/the two-handle guard rejects first. They are kept as
//     future-engine guards, not as live detection, and nothing here pretends to cover them.
//
//   COVERED HERE TOO - the side effect that only a REACHABLE failure could expose. Making a
//     boolean able to fail turned geometry.boolean_*'s unconditional `keepTool: false` tool
//     destruction into a live defect: a refused boolean would delete the cutter and then
//     report BOOLEAN_FAILED. The destruction is now gated on success (BooleanHandler.cpp) and
//     FailedBooleanDoesNotDestroyTheToolActor pins it. Look for this shape after any change
//     that revives a dead error path: side effects sequenced before the failure check were
//     written when that check could not fire.
//
// COUNTERFACTUAL, which is the property that makes this file worth its weight: revert the sink
// wiring in either op and every test below fails, because each one asserts a FAILURE that the
// nullptr-Debug version reports as a success. The AllowEmptyResult case is the control in the
// other direction - it proves the failure is specifically the empty-result refusal and not some
// unrelated breakage, because turning that one option on turns the same call green.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryOps_Boolean.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"

using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::MakeDispatcher;
using GeometryTestHelpers::DestroyActorsWithLabel;

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// The engine's own wording for the boolean refusal these tests provoke, pinned per engine.
// UE 5.4 added FGeometryScriptMeshBooleanOptions::bAllowEmptyResult and rewrote the message to
// name the empty result and the new option; 5.3 has neither concept and emits only the generic
// failure (GeometryScriptingCore MeshBooleanFunctions.cpp:85 in both trees). Each engine asserts
// the text it actually emits rather than a lowest-common-denominator substring that would weaken
// the 5.4+ check.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
const TCHAR* const GeomBoolFailTest_EngineRefusalText =
    TEXT("Boolean operation failed due to an empty result");
const TCHAR* const GeomBoolFailTest_EngineRefusalFragment = TEXT("empty result");
#else
const TCHAR* const GeomBoolFailTest_EngineRefusalText =
    TEXT("BooleanUnion: Boolean operation failed");
const TCHAR* const GeomBoolFailTest_EngineRefusalFragment = TEXT("Boolean operation failed");
#endif

UDynamicMesh* GeomBoolFailTest_NewBox(const FVector& Center, double Dimension)
{
    UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
    FGeometryScriptPrimitiveOptions Options;
    UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
        Mesh, Options, FTransform(Center),
        static_cast<float>(Dimension), static_cast<float>(Dimension), static_cast<float>(Dimension),
        0, 0, 0,
        EGeometryScriptPrimitiveOriginMode::Center, nullptr);
    return Mesh;
}
}

// ============================================================================
// Boolean - the empty-result refusal
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanEnclosedSubtractFailsTest,
    "PinWright.Geometry.Ops.Boolean.EnclosedSubtractFailsInsteadOfReportingSuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBooleanEnclosedSubtractFailsTest::RunTest(const FString& Parameters)
{
    // A tool that swallows the target whole. The difference is empty, and with
    // bAllowEmptyResult at its default false the engine refuses to produce it - returning the
    // UNTOUCHED target and complaining only into the Debug argument.
    // The engine LOGS every message it appends to a UGeometryScriptDebug, at Error verbosity, and
    // it does so unconditionally - before it has even looked at whether a Debug object was passed
    // (GeometryScriptTypes.cpp, MakeScriptError). A test that deliberately drives an engine
    // refusal therefore emits a genuine LogGeometry Error, and UE's automation framework fails a
    // test on any uncaptured Error line. Declaring it expected is what separates "the engine
    // complained, as this test intended" from "something went wrong".
    //
    // Occurrences 0 means "any number, including none", which is deliberate: whether the
    // framework captures the line at all has proved environment-dependent (the boolean family's
    // two dispatcher tests passed in one suite run and failed in the next on identical code), and
    // a fixed count would then fail in the other direction.
    AddExpectedErrorPlain(GeomBoolFailTest_EngineRefusalText,
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    UDynamicMesh* Target = GeomBoolFailTest_NewBox(FVector::ZeroVector, 100.0);
    UDynamicMesh* Tool = GeomBoolFailTest_NewBox(FVector::ZeroVector, 400.0);

    const int32 TrisBefore = Target->GetTriangleCount();
    TestEqual(TEXT("fixture target is a 12-triangle box"), TrisBefore, 12);

    const GeometryOps::FOpResult Op = GeometryOps::Boolean(
        Target, FTransform::Identity, Tool, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());

    // The whole point of the change. This assertion is FALSE on the nullptr-Debug version,
    // which answers bSuccess == true with bChanged == false.
    TestFalse(TEXT("a boolean the engine refused is reported as a failure"), Op.bSuccess);
    TestEqual(TEXT("the failure carries BOOLEAN_FAILED"),
        Op.ErrorCode, FString(ErrorCodes::ERR_BOOLEAN_FAILED));

    // The message must name what the ENGINE said, not just that something went wrong: the
    // engine's own text is the only place the remedy appears.
    TestTrue(TEXT("the message names the operation"), Op.ErrorMessage.Contains(TEXT("Subtract")));
    TestTrue(TEXT("the message carries the engine's own diagnosis"),
        Op.ErrorMessage.Contains(GeomBoolFailTest_EngineRefusalFragment));
    // The remedy half exists only from UE 5.4: it names the Allow Empty Result option, which 5.3
    // does not have, and 5.3's message accordingly offers none.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("the message carries the engine's own remedy"),
        Op.ErrorMessage.Contains(TEXT("Allow Empty Result")));
#endif

    // FailIn, not Fail: the before-counts recorded before the engine ran must survive the
    // failure, because they are what prove the target was left alone.
    TestEqual(TEXT("the before-counts survive the failure"), Op.TrianglesBefore, TrisBefore);
    TestEqual(TEXT("the target mesh is left exactly as it was"),
        Target->GetTriangleCount(), TrisBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanAllowEmptyResultTest,
    "PinWright.Geometry.Ops.Boolean.AllowEmptyResultTurnsTheRefusalIntoAnEmptyMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBooleanAllowEmptyResultTest::RunTest(const FString& Parameters)
{
    // The control for the test above. Same meshes, same operation, one option flipped: if this
    // succeeds and that one fails, the failure is provably the empty-result refusal and not
    // some unrelated breakage in the fixture or the sink.
    UDynamicMesh* Target = GeomBoolFailTest_NewBox(FVector::ZeroVector, 100.0);
    UDynamicMesh* Tool = GeomBoolFailTest_NewBox(FVector::ZeroVector, 400.0);

    GeometryOps::FBooleanParams Params;
    Params.bAllowEmptyResult = true;

    const GeometryOps::FOpResult Op = GeometryOps::Boolean(
        Target, FTransform::Identity, Tool, FTransform::Identity,
        EGeometryScriptBooleanOperation::Subtract, Params);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("with allowEmptyResult the same boolean succeeds"), Op.bSuccess);
    TestEqual(TEXT("no error code is set"), Op.ErrorCode, FString());
    TestEqual(TEXT("the result really is empty"), Op.TrianglesAfter, 0);
    TestTrue(TEXT("emptying the mesh counts as a change"), Op.bChanged);
    TestEqual(TEXT("the target mesh really was emptied"), Target->GetTriangleCount(), 0);
#else
    // FGeometryScriptMeshBooleanOptions::bAllowEmptyResult arrived in UE 5.4; 5.3 always refuses
    // an empty result. GeometryOps::Boolean therefore rejects the request by name instead of
    // serving the opposite behaviour under a success, and the target must be untouched.
    TestFalse(TEXT("allowEmptyResult is refused on an engine that cannot honour it"), Op.bSuccess);
    TestEqual(TEXT("with UNSUPPORTED_ENGINE_VERSION"),
        Op.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
    TestTrue(TEXT("and the message names the option and the version that added it"),
        Op.ErrorMessage.Contains(TEXT("bAllowEmptyResult"))
        && Op.ErrorMessage.Contains(TEXT("5.4")));
    TestTrue(TEXT("and the target mesh is left exactly as it was"),
        Target->GetTriangleCount() > 0);
#endif

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanDisjointIntersectionFailsTest,
    "PinWright.Geometry.Ops.Boolean.DisjointIntersectionFailsWhileDisjointSubtractSucceeds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBooleanDisjointIntersectionFailsTest::RunTest(const FString& Parameters)
{
    // The second reachable shape of the same refusal, and the pair that shows the new failure is
    // discriminating rather than blanket. Two disjoint boxes:
    //   Intersection -> empty -> refused -> BOOLEAN_FAILED
    //   Subtract     -> the whole target -> not empty -> genuine success, bChanged == false
    // Before the sink both answered "success, changed: false", which is why a caller could not
    // tell an intersection that produced nothing from a subtract that legitimately cut nothing.
    AddExpectedErrorPlain(GeomBoolFailTest_EngineRefusalText,
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    {
        UDynamicMesh* Target = GeomBoolFailTest_NewBox(FVector::ZeroVector, 100.0);
        UDynamicMesh* Tool = GeomBoolFailTest_NewBox(FVector(1000.0, 0.0, 0.0), 100.0);

        const GeometryOps::FOpResult Op = GeometryOps::Boolean(
            Target, FTransform::Identity, Tool, FTransform::Identity,
            EGeometryScriptBooleanOperation::Intersection, GeometryOps::FBooleanParams());

        TestFalse(TEXT("a disjoint intersection is a failure, not a silent no-op"), Op.bSuccess);
        TestEqual(TEXT("it carries BOOLEAN_FAILED"),
            Op.ErrorCode, FString(ErrorCodes::ERR_BOOLEAN_FAILED));
        TestTrue(TEXT("the message names Intersection"),
            Op.ErrorMessage.Contains(TEXT("Intersection")));
    }

    {
        UDynamicMesh* Target = GeomBoolFailTest_NewBox(FVector::ZeroVector, 100.0);
        UDynamicMesh* Tool = GeomBoolFailTest_NewBox(FVector(1000.0, 0.0, 0.0), 100.0);

        const GeometryOps::FOpResult Op = GeometryOps::Boolean(
            Target, FTransform::Identity, Tool, FTransform::Identity,
            EGeometryScriptBooleanOperation::Subtract, GeometryOps::FBooleanParams());

        TestTrue(TEXT("a disjoint subtract is still a success"), Op.bSuccess);
        TestEqual(TEXT("and carries no error code"), Op.ErrorCode, FString());
        TestFalse(TEXT("and still reports bChanged == false"), Op.bChanged);
    }

    return true;
}

// ============================================================================
// Trim
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanTrimEmptyResultFailsTest,
    "PinWright.Geometry.Ops.Boolean.TrimThatWouldEmptyTheMeshFailsInsteadOfNoOping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometryBooleanTrimEmptyResultFailsTest::RunTest(const FString& Parameters)
{
    // bKeepInside = true dispatches Intersection, so a disjoint tool keeps nothing. This is the
    // exact case the task called out: a trim that would empty the mesh used to leave it
    // SILENTLY UNTOUCHED under a success response, so the caller went on to bevel, collide and
    // bake geometry that still had the whole tool volume in it.
    AddExpectedErrorPlain(GeomBoolFailTest_EngineRefusalText,
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    UDynamicMesh* Target = GeomBoolFailTest_NewBox(FVector::ZeroVector, 100.0);
    UDynamicMesh* Tool = GeomBoolFailTest_NewBox(FVector(1000.0, 0.0, 0.0), 100.0);

    const int32 TrisBefore = Target->GetTriangleCount();

    GeometryOps::FTrimParams Params;
    Params.bKeepInside = true;

    const GeometryOps::FOpResult Op = GeometryOps::Trim(
        Target, FTransform::Identity, Tool, FTransform::Identity, Params);

    TestFalse(TEXT("a trim the engine refused is reported as a failure"), Op.bSuccess);
    TestEqual(TEXT("the failure carries BOOLEAN_FAILED"),
        Op.ErrorCode, FString(ErrorCodes::ERR_BOOLEAN_FAILED));
    TestTrue(TEXT("the message carries the engine's own diagnosis"),
        Op.ErrorMessage.Contains(GeomBoolFailTest_EngineRefusalFragment));
    TestEqual(TEXT("the mesh is left exactly as it was"), Target->GetTriangleCount(), TrisBefore);

    // FTrimParams::bAllowEmptyResult stays FALSE by default, deliberately - see the field. The
    // caller who wants an empty trim asks for one, and then gets it rather than a failure.
    {
        UDynamicMesh* AllowTarget = GeomBoolFailTest_NewBox(FVector::ZeroVector, 100.0);
        UDynamicMesh* AllowTool = GeomBoolFailTest_NewBox(FVector(1000.0, 0.0, 0.0), 100.0);

        GeometryOps::FTrimParams AllowParams;
        AllowParams.bKeepInside = true;
        AllowParams.bAllowEmptyResult = true;

        const GeometryOps::FOpResult AllowOp = GeometryOps::Trim(
            AllowTarget, FTransform::Identity, AllowTool, FTransform::Identity, AllowParams);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        TestTrue(TEXT("with allowEmptyResult the same trim succeeds"), AllowOp.bSuccess);
        TestEqual(TEXT("and empties the mesh"), AllowTarget->GetTriangleCount(), 0);
#else
        // See the boolean control above: 5.3 has no bAllowEmptyResult, so the request is refused
        // by name and the mesh is left alone.
        TestFalse(TEXT("allowEmptyResult is refused on an engine that cannot honour it"),
            AllowOp.bSuccess);
        TestEqual(TEXT("with UNSUPPORTED_ENGINE_VERSION"),
            AllowOp.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION));
        TestTrue(TEXT("and the mesh is left exactly as it was"),
            AllowTarget->GetTriangleCount() > 0);
#endif
    }

    return true;
}

// ============================================================================
// End to end, over the real dispatcher
//
// The op-level tests above would all have passed while the RPC wrapper swallowed the result -
// which is the shape of the sibling defect this family already suffered twice (warnings dropped
// at the wrapper, bSuccess dropped at the wrapper). Only a dispatcher test proves the failure
// reaches the CALLER.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanFailureReachesTheCallerTest,
    "PinWright.geometry.boolean.FailedBooleanReachesTheCallerAsAnError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBooleanFailureReachesTheCallerTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping boolean failure-detection dispatcher test"));
        return true;
    }

    AddExpectedErrorPlain(GeomBoolFailTest_EngineRefusalText,
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetLabel = FString::Printf(TEXT("PW_BoolFailTarget_%s"), *Suffix);
    const FString ToolLabel = FString::Printf(TEXT("PW_BoolFailTool_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto MakeBox = [&](const FString& Label, double Extent) -> bool
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        Params->SetNumberField(TEXT("width"), Extent);
        Params->SetNumberField(TEXT("height"), Extent);
        Params->SetNumberField(TEXT("depth"), Extent);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            FString::Printf(TEXT("req-boolfail-box-%s"), *Label), Params, bSuccess, ErrorCode);
        return bSuccess;
    };

    // Both boxes spawn at the origin, so the 400 tool encloses the 100 target whole and the
    // difference is empty. Nothing exotic: this is a caller reaching for the wrong tool actor.
    const bool bTargetMade = MakeBox(TargetLabel, 100.0);
    const bool bToolMade = MakeBox(ToolLabel, 400.0);

    if (TestTrue(TEXT("the target box was created"), bTargetMade)
        && TestTrue(TEXT("the tool box was created"), bToolMade))
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("targetActor"), TargetLabel);
        Params->SetStringField(TEXT("toolActor"), ToolLabel);
        // Keep the tool so the cleanup below can find it, and so a failing boolean is not also
        // testing the destruction path.
        Params->SetBoolField(TEXT("keepTool"), true);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("geometry.boolean_subtract"),
            TEXT("req-boolfail-subtract"), Params, bSuccess, Result, ErrorCode);

        // The assertion the whole change exists for. On the nullptr-Debug version this responds
        // success:true with changed:false, and the caller goes on to build on an uncut box.
        TestFalse(TEXT("the caller is told the boolean failed"), bSuccess);
        TestEqual(TEXT("with the BOOLEAN_FAILED code"),
            ErrorCode, FString(ErrorCodes::ERR_BOOLEAN_FAILED));
        TestTrue(TEXT("and a message quoting the engine"),
            Sink->Message.Contains(GeomBoolFailTest_EngineRefusalFragment));
    }

    DestroyActorsWithLabel(TargetLabel);
    DestroyActorsWithLabel(ToolLabel);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanFailedKeepsToolTest,
    "PinWright.geometry.boolean.FailedBooleanDoesNotDestroyTheToolActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBooleanFailedKeepsToolTest::RunTest(const FString& Parameters)
{
    // A defect the boolean fix EXPOSED rather than introduced, which is why it is pinned here
    // next to the fix. geometry.boolean_* destroys the tool actor when keepTool is false, and
    // that destruction ran unconditionally - correct only while BOOLEAN_FAILED was structurally
    // unreachable. Once the op started reading the engine's refusal, an ungated destruction
    // would delete the cutter for a boolean that changed nothing and then report failure: the
    // caller loses the tool, gains no cut, and cannot retry without rebuilding it.
    //
    // Destroying the tool is a COMMIT, so it belongs on the success path with the mesh notify.
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping boolean keepTool failure test"));
        return true;
    }

    AddExpectedErrorPlain(GeomBoolFailTest_EngineRefusalText,
        EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/0);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetLabel = FString::Printf(TEXT("PW_BoolKeepTarget_%s"), *Suffix);
    const FString ToolLabel = FString::Printf(TEXT("PW_BoolKeepTool_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    auto MakeBox = [&](const FString& Label, double Extent) -> bool
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        Params->SetNumberField(TEXT("width"), Extent);
        Params->SetNumberField(TEXT("height"), Extent);
        Params->SetNumberField(TEXT("depth"), Extent);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            FString::Printf(TEXT("req-boolkeep-box-%s"), *Label), Params, bSuccess, ErrorCode);
        return bSuccess;
    };

    if (TestTrue(TEXT("the target box was created"), MakeBox(TargetLabel, 100.0))
        && TestTrue(TEXT("the tool box was created"), MakeBox(ToolLabel, 400.0)))
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("targetActor"), TargetLabel);
        Params->SetStringField(TEXT("toolActor"), ToolLabel);
        Params->SetBoolField(TEXT("keepTool"), false);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.boolean_subtract"),
            TEXT("req-boolkeep-subtract"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the boolean failed"), bSuccess);
        TestEqual(TEXT("with BOOLEAN_FAILED"), ErrorCode, FString(ErrorCodes::ERR_BOOLEAN_FAILED));
        TestNotNull(TEXT("the tool actor survives a failed boolean"),
            GeometryTestHelpers::FindActorByLabel(ToolLabel));
        TestNotNull(TEXT("and so does the target"),
            GeometryTestHelpers::FindActorByLabel(TargetLabel));
    }

    DestroyActorsWithLabel(TargetLabel);
    DestroyActorsWithLabel(ToolLabel);
    return true;
}
