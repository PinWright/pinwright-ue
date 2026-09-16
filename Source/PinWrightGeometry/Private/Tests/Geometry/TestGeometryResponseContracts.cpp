// Copyright (c) 2026 Alexander Penkin. MIT License.

// Response-contract regression tests for three geometry defects of the same family: a result
// struct produced by the layer below and then partly ignored by the RPC wrapper above it.
//
//   1. geometry.convert_to_static_mesh read six fields off FStaticMeshCreateResult and never read
//      Warnings, so a bake that disabled normal/tangent recompute, could not load a bound
//      material, or had its lightmap index clamped answered with a clean success. Same defect as
//      the GeometryOps::FOpResult::Warnings sweep, on a different struct.
//   2. geometry.mirror and geometry.boolean_trim discarded FOpResult::bSuccess along with the
//      counts, so a failed op was reported as a success. The counts stay unpublished on purpose;
//      the failure does not.
//   3. The verbs that spawn a DynamicMeshActor emitted `class` from 7 of them and called
//      AddActorVerification from 13 - one family, two response shapes. The sweep that closed
//      that gap keyed on the create_* NAME, so the three spawning verbs not spelled create_
//      (geometry.revolve, geometry.import_obj, geometry.import_stl) kept both omissions. The
//      family test below therefore enumerates by "spawns an actor", not by prefix.
//
// What these tests can and cannot reach, stated plainly so the next reader does not mistake a
// gap for coverage:
//
//   - The SUCCESS shapes are fully assertable and are asserted here, including the deliberate
//     ABSENCE of fields. Absence is the half that a "widen the response while you are in there"
//     change breaks silently, and no other test in the tree pins it for these two verbs.
//   - The FAILURE path of boolean_trim IS now reachable, and is covered in
//     TestGeometryBooleanFailureDetection.cpp rather than here. The paragraph this replaces was
//     correct when written: ApplyMeshBoolean NEVER returns null - every path, including its
//     empty-result refusal, returns TargetMesh (MeshBooleanFunctions.cpp:38-47, :96-99) - so the
//     `if (!ResultMesh)` checks inside GeometryOps::Boolean/Trim were dead and the engine's real
//     complaint went to a UGeometryScriptDebug argument both ops passed as nullptr. The ops now
//     pass a live one (Handlers/Geometry/GeometryScriptDebugSink.h), so a trim the engine
//     refuses reaches the caller as BOOLEAN_FAILED.
//   - The FAILURE path of mirror is STILL not reachable on UE 5.8, now for a reason one layer
//     further down: its three engine calls receive a live sink too, but ScaleMesh and AppendMesh
//     raise only null-mesh errors, and WeldMeshEdges' error flag is gated on
//     FMergeCoincidentMeshEdges::Apply() returning false, which it never does (single
//     `return true`, MergeCoincidentMeshEdges.cpp:233). Stated rather than covered by a test
//     that would report green without asserting.
//   - The op-level null-handle failures ARE covered, in TestGeometryOpsBoolean.cpp's
//     NullMeshFailure test - but note they exercise the plugin's OWN guard and never reach the
//     engine, which is exactly why they stayed green while the engine's failures were invisible.
//   - The convert warnings are likewise not reachable through that verb today: it hoists
//     EnsureMeshHasUVs above the create/overwrite fork, and the box projection succeeds for any
//     mesh that has triangles (see TestGeometryConvertNoUVMesh.cpp), while a mesh with NO
//     triangles is refused before the bake. So the negative control below is what this verb can
//     assert end to end, and the warning is pinned at its source instead.
#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Geometry/GeometryAssetCreate.h"
#include "Handlers/Geometry/GeometryOps_Primitives.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::MakeDispatcher;
using GeometryTestHelpers::DestroyActorsWithLabel;

// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// sharing a name with a sibling test file's would be an ODR redefinition once the TUs merge.
namespace
{
    // Spawn a probe DynamicMeshActor through the real create verb and report whether it landed.
    bool RespContractSpawnBox(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& Label, const TCHAR* RequestId)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"), RequestId, Params, bSuccess, ErrorCode);
        return bSuccess;
    }

    bool RespContractHasField(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        return Result.IsValid() && Result->HasField(Field);
    }

    // Per-verb argument setup for the spawning-family sweep below. These exist so a verb whose
    // shape needs an argument is DRIVEN by the sweep rather than excused from it - the whole
    // defect was a family member left out for being different.

    // geometry.revolve sweeps a profile path. A real 3-point profile exercises the path a caller
    // takes; a shorter one would silently land on the default profile GenerateRevolve substitutes.
    void RespContractAddRevolveProfile(const TSharedPtr<FJsonObject>& Params)
    {
        static const double ProfileXY[3][2] = { { 10.0, 0.0 }, { 40.0, 30.0 }, { 10.0, 60.0 } };
        TArray<TSharedPtr<FJsonValue>> Profile;
        for (int32 i = 0; i < UE_ARRAY_COUNT(ProfileXY); ++i)
        {
            TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
            Point->SetNumberField(TEXT("x"), ProfileXY[i][0]);
            Point->SetNumberField(TEXT("y"), ProfileXY[i][1]);
            Profile.Add(MakeShared<FJsonValueObject>(Point));
        }
        Params->SetArrayField(TEXT("profile"), Profile);
    }

    // One triangle: the smallest input either importer accepts (both refuse an empty parse with
    // PARSE_FAILED). Inline text keeps the sweep off the filesystem.
    void RespContractAddObjText(const TSharedPtr<FJsonObject>& Params)
    {
        Params->SetStringField(TEXT("text"),
            TEXT("v 0 0 0\nv 100 0 0\nv 0 100 0\nf 1 2 3\n"));
    }

    void RespContractAddStlText(const TSharedPtr<FJsonObject>& Params)
    {
        Params->SetStringField(TEXT("text"),
            TEXT("solid probe\nfacet normal 0 0 1\nouter loop\n")
            TEXT("vertex 0 0 0\nvertex 100 0 0\nvertex 0 100 0\n")
            TEXT("endloop\nendfacet\nendsolid probe\n"));
    }
}


// ============================================================================
// geometry.mirror - success shape, including what it deliberately does NOT say
// ============================================================================
// The failure of the op now reaches the caller as a failure. The counts still do not reach the
// caller at all, and that is the half a later "while we are here" change would quietly break:
// the op HAS VerticesAfter/TrianglesAfter and the sibling verbs in the same file
// (array_linear, array_radial) do echo them, so adding them here looks like a consistency fix
// rather than the wire change it is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMirrorSuccessShapeTest,
    "PinWright.geometry.mirror.SuccessShapeIsUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMirrorSuccessShapeTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping geometry.mirror response-shape test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_MirrorShape_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    if (!TestTrue(TEXT("geometry.create_box spawned the mirror probe"),
            RespContractSpawnBox(Dispatcher, Sink, Label, TEXT("req-mirror-shape-create"))))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetStringField(TEXT("axis"), TEXT("Y"));

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.mirror"), TEXT("req-mirror-shape"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("mirroring a valid box succeeds"), bSuccess);
    TestTrue(TEXT("geometry.mirror returned a result object"), Result.IsValid());

    FString EchoedActor;
    TestTrue(TEXT("geometry.mirror echoes actorName"),
        Result.IsValid() && Result->TryGetStringField(TEXT("actorName"), EchoedActor));
    TestEqual(TEXT("the echoed actorName is the probe"), EchoedActor, Label);

    FString EchoedAxis;
    TestTrue(TEXT("geometry.mirror echoes axis"),
        Result.IsValid() && Result->TryGetStringField(TEXT("axis"), EchoedAxis));
    TestEqual(TEXT("the echoed axis is the requested one, uppercased"), EchoedAxis, FString(TEXT("Y")));

    // The deliberate absences. Failing here means the response widened.
    TestFalse(TEXT("geometry.mirror still publishes no vertexCount"),
        RespContractHasField(Result, TEXT("vertexCount")));
    TestFalse(TEXT("geometry.mirror still publishes no triangleCount"),
        RespContractHasField(Result, TEXT("triangleCount")));
    // Mirror trips no clamp, so the warnings gate must keep the key off entirely - the property
    // that makes the whole warnings sweep additive.
    TestFalse(TEXT("a clamp-free mirror carries no warnings key"),
        RespContractHasField(Result, TEXT("warnings")));

    DestroyActorsWithLabel(Label);
    return true;
}


// ============================================================================
// geometry.boolean_trim - success shape, same contract
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBooleanTrimSuccessShapeTest,
    "PinWright.geometry.boolean_trim.SuccessShapeIsUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBooleanTrimSuccessShapeTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping geometry.boolean_trim response-shape test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetLabel = FString::Printf(TEXT("PW_TrimShapeTarget_%s"), *Suffix);
    const FString ToolLabel = FString::Printf(TEXT("PW_TrimShapeTool_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const bool bTargetSpawned =
        RespContractSpawnBox(Dispatcher, Sink, TargetLabel, TEXT("req-trim-shape-target"));
    const bool bToolSpawned =
        RespContractSpawnBox(Dispatcher, Sink, ToolLabel, TEXT("req-trim-shape-tool"));
    if (!TestTrue(TEXT("both boolean_trim probes spawned"), bTargetSpawned && bToolSpawned))
    {
        DestroyActorsWithLabel(TargetLabel);
        DestroyActorsWithLabel(ToolLabel);
        return true;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TargetLabel);
    Params->SetStringField(TEXT("trimActorName"), ToolLabel);
    // keepInside:true, so the intersection of two coincident boxes is the box itself - a trim
    // that leaves real geometry behind rather than the empty result the engine refuses.
    Params->SetBoolField(TEXT("keepInside"), true);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.boolean_trim"), TEXT("req-trim-shape"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("trimming a box against a coincident box succeeds"), bSuccess);
    TestTrue(TEXT("geometry.boolean_trim returned a result object"), Result.IsValid());

    FString EchoedActor;
    TestTrue(TEXT("geometry.boolean_trim echoes actorName"),
        Result.IsValid() && Result->TryGetStringField(TEXT("actorName"), EchoedActor));
    TestEqual(TEXT("the echoed actorName is the target"), EchoedActor, TargetLabel);

    FString EchoedTool;
    TestTrue(TEXT("geometry.boolean_trim echoes trimActorName"),
        Result.IsValid() && Result->TryGetStringField(TEXT("trimActorName"), EchoedTool));
    TestEqual(TEXT("the echoed trimActorName is the tool"), EchoedTool, ToolLabel);

    bool bEchoedKeepInside = false;
    TestTrue(TEXT("geometry.boolean_trim echoes keepInside"),
        Result.IsValid() && Result->TryGetBoolField(TEXT("keepInside"), bEchoedKeepInside));
    TestTrue(TEXT("the echoed keepInside is the requested one"), bEchoedKeepInside);

    TestFalse(TEXT("geometry.boolean_trim still publishes no vertexCount"),
        RespContractHasField(Result, TEXT("vertexCount")));
    TestFalse(TEXT("geometry.boolean_trim still publishes no triangleCount"),
        RespContractHasField(Result, TEXT("triangleCount")));
    TestFalse(TEXT("a clamp-free trim carries no warnings key"),
        RespContractHasField(Result, TEXT("warnings")));

    DestroyActorsWithLabel(TargetLabel);
    DestroyActorsWithLabel(ToolLabel);
    return true;
}


// ============================================================================
// Every geometry verb that SPAWNS an actor speaks ONE response shape
// ============================================================================
// Every verb, not a sample: the defect was that 8 of them silently disagreed with the other 7,
// and a test that checks three of them would have passed throughout. create_procedural_mesh is
// included because it is a create verb with the same contract even though it builds no geometry.
//
// Membership is the SPAWN, not the create_ prefix. Enumerating by prefix is exactly how
// geometry.revolve, geometry.import_obj and geometry.import_stl escaped the original sweep -
// all three hand back a new DynamicMeshActor, none of them is spelled create_, and all three
// answered without `class` and without the verification block. A new spawning verb missing from
// this table is the same defect a third time.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateFamilySharesOneResponseShapeTest,
    "PinWright.geometry.create.FamilySharesOneResponseShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateFamilySharesOneResponseShapeTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping create-family response-shape test"));
        return true;
    }

    struct FSpawningVerb
    {
        const TCHAR* Method;
        // The verb's new-actor name slot. The create family and revolve take `name` (which
        // carries `actorName` as a declared alias); the import verbs require `actorName`.
        const TCHAR* NameKey;
        // Shape-defining extra arguments, or nullptr where the name alone is enough.
        void (*AddArgs)(const TSharedPtr<FJsonObject>&);
    };

    static const FSpawningVerb SpawningVerbs[] = {
        { TEXT("geometry.create_box"),            TEXT("name"),      nullptr },
        { TEXT("geometry.create_sphere"),         TEXT("name"),      nullptr },
        { TEXT("geometry.create_cylinder"),       TEXT("name"),      nullptr },
        { TEXT("geometry.create_cone"),           TEXT("name"),      nullptr },
        { TEXT("geometry.create_capsule"),        TEXT("name"),      nullptr },
        { TEXT("geometry.create_torus"),          TEXT("name"),      nullptr },
        { TEXT("geometry.create_plane"),          TEXT("name"),      nullptr },
        { TEXT("geometry.create_disc"),           TEXT("name"),      nullptr },
        { TEXT("geometry.create_stairs"),         TEXT("name"),      nullptr },
        { TEXT("geometry.create_spiral_stairs"),  TEXT("name"),      nullptr },
        { TEXT("geometry.create_ring"),           TEXT("name"),      nullptr },
        { TEXT("geometry.create_arch"),           TEXT("name"),      nullptr },
        { TEXT("geometry.create_pipe"),           TEXT("name"),      nullptr },
        { TEXT("geometry.create_ramp"),           TEXT("name"),      nullptr },
        { TEXT("geometry.revolve"),               TEXT("name"),      &RespContractAddRevolveProfile },
        { TEXT("geometry.create_procedural_mesh"), TEXT("name"),     nullptr },
        { TEXT("geometry.import_obj"),            TEXT("actorName"), &RespContractAddObjText },
        { TEXT("geometry.import_stl"),            TEXT("actorName"), &RespContractAddStlText },
    };

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TArray<FString> Labels;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(SpawningVerbs); ++Index)
    {
        const FSpawningVerb& Verb = SpawningVerbs[Index];
        const TCHAR* Method = Verb.Method;
        const FString Label = FString::Printf(TEXT("PW_CreateShape_%s_%d"), *Suffix, Index);
        Labels.Add(Label);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(Verb.NameKey, Label);
        if (Verb.AddArgs)
        {
            Verb.AddArgs(Params);
        }

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, Method, FString::Printf(TEXT("req-create-shape-%d"), Index),
            Params, bSuccess, Result, ErrorCode);

        if (!TestTrue(*FString::Printf(TEXT("%s succeeded"), Method), bSuccess) || !Result.IsValid())
        {
            continue;
        }

        FString ClassName;
        TestTrue(*FString::Printf(TEXT("%s emits class"), Method),
            Result->TryGetStringField(TEXT("class"), ClassName));
        TestEqual(*FString::Printf(TEXT("%s reports class DynamicMeshActor"), Method),
            ClassName, FString(TEXT("DynamicMeshActor")));

        // `existsAfter` is written ONLY by AddActorVerification (AssetUtils.cpp), so its presence
        // is exactly the assertion "the verification block ran" and cannot be satisfied by any
        // hand-rolled echo a verb might grow instead.
        TestTrue(*FString::Printf(TEXT("%s ran AddActorVerification (existsAfter present)"), Method),
            Result->HasField(TEXT("existsAfter")));
    }

    for (const FString& Label : Labels)
    {
        DestroyActorsWithLabel(Label);
    }
    return true;
}


// ============================================================================
// CreateStaticMesh populates the Warnings field the bake verb now forwards
// ============================================================================
// Pinned at the source rather than through geometry.convert_to_static_mesh, because that verb
// cannot reach any of the three warnings today (see the file header). This is the half of the
// defect a test CAN hold still: stop populating Warnings here and the handler's forwarding has
// nothing to forward, silently.
//
// The unloadable material binding is used because it is the one warning with no engine
// preconditions - LoadObject on a path with no asset fails deterministically on any host.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateStaticMeshReportsWarningsTest,
    "PinWright.geometry.create_static_mesh.ReportsNonFatalWarnings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateStaticMeshReportsWarningsTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping CreateStaticMesh warning test"));
        return true;
    }

    UDynamicMesh* Mesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
    GeometryOps::FBoxParams BoxParams;
    GeometryOps::GenerateBox(Mesh, BoxParams, FTransform::Identity);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/SM_WarnProbe_%s"), *Suffix);

    FStaticMeshCreateSpec Spec;
    Spec.AssetPath = AssetPath;
    Spec.MaterialSlots.Add(TEXT("Painted"));
    Spec.MaterialBindings.Add(TEXT("Painted"),
        FString::Printf(TEXT("/Game/PinWrightTests/M_DoesNotExist_%s"), *Suffix));
    // No disk write: the assertion is about the pre-build report, and leaving the package dirty
    // in memory keeps the test from depending on the save path it is not testing.
    Spec.bSave = false;

    const FStaticMeshCreateResult Created = CreateStaticMesh(Mesh, Spec);

    TestTrue(TEXT("a bake with an unloadable material binding still succeeds"), Created.bSuccess);
    TestTrue(TEXT("the unloadable slot is reported in UnboundSlots"),
        Created.UnboundSlots.Contains(TEXT("Painted")));

    // The field the RPC wrapper used to drop.
    TestTrue(TEXT("the unloadable binding is reported in Warnings"), Created.Warnings.Num() > 0);
    bool bNamesTheSlot = false;
    for (const FString& Warning : Created.Warnings)
    {
        bNamesTheSlot |= Warning.Contains(TEXT("Painted"));
    }
    TestTrue(TEXT("the warning names the slot the caller can act on"), bNamesTheSlot);

    CleanupTestAsset(AssetPath);
    return true;
}


// ============================================================================
// geometry.convert_to_static_mesh - negative control on the warnings gate
// ============================================================================
// A clean bake must carry NO `warnings` key at all. This is the property that keeps the field
// additive across the ~146 dispatcher tests, and it is the assertion that fails if someone
// switches convert's emission from non-empty-only to unconditional.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertCleanBakeHasNoWarningsKeyTest,
    "PinWright.geometry.convert_to_static_mesh.CleanBakeHasNoWarningsKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertCleanBakeHasNoWarningsKeyTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping convert warnings-gate test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Label = FString::Printf(TEXT("PW_ConvertNoWarn_%s"), *Suffix);
    const FString AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/SM_ConvertNoWarn_%s"), *Suffix);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    if (!TestTrue(TEXT("geometry.create_box spawned the convert probe"),
            RespContractSpawnBox(Dispatcher, Sink, Label, TEXT("req-convert-nowarn-create"))))
    {
        DestroyActorsWithLabel(Label);
        return true;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetStringField(TEXT("assetPath"), AssetPath);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.convert_to_static_mesh"), TEXT("req-convert-nowarn"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("baking a box carrying UVs succeeds"), bSuccess);
    TestTrue(TEXT("geometry.convert_to_static_mesh returned a result object"), Result.IsValid());
    TestFalse(TEXT("a clean bake carries no warnings key"),
        RespContractHasField(Result, TEXT("warnings")));

    CleanupTestAsset(AssetPath);
    DestroyActorsWithLabel(Label);
    return true;
}


// ============================================================================
// Stair step counts are bounded by GEOM_MAX_STAIR_STEPS, not GEOM_MAX_SEGMENTS
// ============================================================================
// The two numbers differ (400 vs 256) precisely so this test can tell them apart. 300 is the
// case the old bound silently capped: a plausible staircase, well inside what the module's
// triangle ceiling allows, refused for no measured reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryStairsUseTheirOwnStepBoundTest,
    "PinWright.Geometry.Ops.Primitives.StairsUseTheirOwnStepBound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryStairsUseTheirOwnStepBoundTest::RunTest(const FString& Parameters)
{
    // A 300-step staircase survives intact. Under the segment ceiling this became 256 and warned.
    {
        UDynamicMesh* Mesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
        GeometryOps::FStairsParams Params;
        Params.NumSteps = 300;
        // Floating, not solid: the clamp under test is on the COUNT and runs identically either
        // way, while the solid generator is quadratic (2n^2 + 10n) and would spend 183,000
        // triangles on an assertion about an integer. The floating generator is linear.
        Params.bFloating = true;

        const GeometryOps::FOpResult Op = GeometryOps::GenerateStairs(Mesh, Params, FTransform::Identity);
        TestTrue(TEXT("a 300-step staircase builds"), Op.bSuccess);
        TestEqual(TEXT("300 steps are not capped to the segment ceiling"), Params.NumSteps, 300);
        TestEqual(TEXT("an uncapped count warns about nothing"), Op.Warnings.Num(), 0);
    }

    // The bound still exists, and it is the stairs' own.
    {
        UDynamicMesh* Mesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
        GeometryOps::FStairsParams Params;
        Params.NumSteps = GEOM_MAX_STAIR_STEPS * 2;
        Params.bFloating = true;

        const GeometryOps::FOpResult Op = GeometryOps::GenerateStairs(Mesh, Params, FTransform::Identity);
        TestTrue(TEXT("an oversized staircase still builds"), Op.bSuccess);
        TestEqual(TEXT("the count caps at the stairs' own ceiling"), Params.NumSteps, GEOM_MAX_STAIR_STEPS);
        TestTrue(TEXT("the cap reports itself"), Op.Warnings.Num() > 0);
    }

    // The <= 0 substitution is untouched by the ceiling change - it is what fixed the lying
    // numSteps echo, and it must keep both the value and the warning wording it had.
    {
        UDynamicMesh* Mesh = GeometryUtils::GetOrCreateDynamicMesh(GetTransientPackage());
        GeometryOps::FSpiralStairsParams Params;
        Params.NumSteps = 0;
        Params.bFloating = true;

        const GeometryOps::FOpResult Op =
            GeometryOps::GenerateSpiralStairs(Mesh, Params, FTransform::Identity);
        TestTrue(TEXT("a spiral staircase with numSteps=0 builds"), Op.bSuccess);
        TestEqual(TEXT("0 still reads as unset and takes the verb's default"), Params.NumSteps, 8);

        bool bWarned = false;
        for (const FString& Warning : Op.Warnings)
        {
            bWarned |= Warning.Contains(TEXT("numSteps clamped from 0 to 8"));
        }
        TestTrue(TEXT("the substitution keeps its published wording"), bWarned);
    }

    return true;
}
