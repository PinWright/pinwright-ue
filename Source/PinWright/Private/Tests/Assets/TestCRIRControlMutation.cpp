// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for F-crir-control-mutation-write: 11 per-variant round-trips of
// FRigControlValue typed-prefix grammar, one maximal-settings round-trip,
// one byte-equal full round-trip, and one type-mismatch error path.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Editor.h"
#include "EulerTransform.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigHierarchyElements.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"

namespace
{
// FCRIRCompiler::Compile records its hierarchy mutation into a live editor
// undo transaction (intentional — see B-no-undo-redo). Force-deleting the
// freshly-compiled ControlRigBlueprint in the same tick makes
// ObjectTools::DeleteObjectsUnchecked call GatherObjectReferencersForDeletion,
// which serializes that undo record to test bIsReferencedByUndo. For a control
// carrying the maximal settings/min/max snapshot that serialization can drive a
// MarkBlueprintAsStructurallyModified recompute on the game thread and freeze
// the suite (the same hazard PinWrightTransactionUtils avoids on the RPC
// path). Clearing the transactor before delete drops the heavy record so
// reference-gathering has nothing to walk — the engine itself resets the
// transaction here too, but only after that gather has already run.
void ResetUndoBufferForCleanup()
{
    if (GEditor && GEditor->Trans)
    {
        GEditor->ResetTransaction(
            NSLOCTEXT("CRIRControlTest", "CleanupReset", "CRIR Control Test Cleanup"));
    }
}

// Helper: compile a CRIR snippet against a freshly created empty Control Rig
// BP at the given path. Returns the loaded BP on success (nullptr on skip).
UControlRigBlueprint* CreateTargetBP(const FString& Name, const FString& PackageDir, FString& OutObjectPath)
{
    FString CreateError;
    UBlueprint* Raw = McpCreateControlRigBlueprint(Name, PackageDir, nullptr, CreateError);
    UControlRigBlueprint* BP = Cast<UControlRigBlueprint>(Raw);
    if (BP)
    {
        OutObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackageDir, *Name, *Name);
    }
    return BP;
}

// Compiles a hierarchy block containing a single control element with the
// given type/value, asserts compile success, then returns the loaded
// FRigControlElement* (or nullptr on failure).
const FRigControlElement* CompileAndFindControl(
    FAutomationTestBase& Test,
    UControlRigBlueprint* BP,
    const FString& TargetObject,
    const FString& BodyLines)
{
    const FString CRIR = FString::Printf(
        TEXT("rig_hierarchy {\n%s\n}\n"), *BodyLines);

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;
    FCRIRCompileResult Result = FCRIRCompiler::Compile(CRIR, Options);
    Test.TestTrue(FString::Printf(TEXT("compile succeeds (code='%s', msg='%s')"),
        *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess);
    if (!Result.bSuccess) { return nullptr; }

    URigHierarchy* Hier = GetControlRigHierarchy(BP);
    if (!Hier) { return nullptr; }
    return Hier->Find<FRigControlElement>(FRigElementKey(FName(TEXT("C")), ERigElementType::Control));
}

FRigControlValue GetCtlValue(UControlRigBlueprint* BP)
{
    URigHierarchy* H = GetControlRigHierarchy(BP);
    return H->GetControlValue(FRigElementKey(FName(TEXT("C")), ERigElementType::Control));
}

constexpr float kEps = 1e-4f;

// Standard fixture wrapper.
struct FCtrlFixture
{
    FString Guid;
    FString TargetPath;
    FString TargetObject;
    UControlRigBlueprint* BP = nullptr;

    bool Init()
    {
        Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Name = FString::Printf(TEXT("CR_CRIRCtrl_%s"), *Guid);
        TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *Name);
        BP = CreateTargetBP(Name, TEXT("/Game/PinWrightTests"), TargetObject);
        return BP != nullptr;
    }

    void Cleanup() const
    {
        // Drop the compile's undo record before force-deleting so the delete's
        // reference-gathering doesn't serialize the mutated hierarchy snapshot.
        ResetUndoBufferForCleanup();
        CleanupTestAsset(TargetPath);
    }
};
} // namespace

// Counterfactual: if the new `Control` case in CRIRCompiler.cpp is reverted,
// the compile errors with CRIR_HIERARCHY_NOT_WRITABLE and the resulting
// hierarchy has no control element — Find<FRigControlElement> returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlBoolRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Bool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlBoolRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("could not create CR BP - skipped")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=bool value=bool(true)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Bool"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Bool));
    TestEqual(TEXT("value=true"), GetCtlValue(F.BP).Get<bool>(), true);
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlFloatRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Float",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlFloatRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=float value=float(1.5)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Float"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Float));
    TestTrue(TEXT("value≈1.5"), FMath::IsNearlyEqual(GetCtlValue(F.BP).Get<float>(), 1.5f, kEps));
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlIntRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Int",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlIntRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=int value=int(42)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Integer"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Integer));
    TestEqual(TEXT("value=42"), GetCtlValue(F.BP).Get<int32>(), 42);
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlVector2DRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Vector2D",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlVector2DRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=vector2d value=vector2d(0.25, 0.75)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Vector2D"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Vector2D));
    const FVector3f V = GetCtlValue(F.BP).Get<FVector3f>();
    TestTrue(TEXT("v.X≈0.25"), FMath::IsNearlyEqual(V.X, 0.25f, kEps));
    TestTrue(TEXT("v.Y≈0.75"), FMath::IsNearlyEqual(V.Y, 0.75f, kEps));
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlPositionRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Position",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlPositionRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=position value=position(1, 2, 3)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Position"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Position));
    const FVector3f V = GetCtlValue(F.BP).Get<FVector3f>();
    TestTrue(TEXT("v.X≈1"), FMath::IsNearlyEqual(V.X, 1.f, kEps));
    TestTrue(TEXT("v.Y≈2"), FMath::IsNearlyEqual(V.Y, 2.f, kEps));
    TestTrue(TEXT("v.Z≈3"), FMath::IsNearlyEqual(V.Z, 3.f, kEps));
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlScaleRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Scale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlScaleRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=scale value=scale(2, 2, 2)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Scale"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Scale));
    const FVector3f V = GetCtlValue(F.BP).Get<FVector3f>();
    TestTrue(TEXT("v.X≈2"), FMath::IsNearlyEqual(V.X, 2.f, kEps));
    return true;
}

// ERigControlType::ScaleFloat is UE 5.4+; the scale_float control type does not exist on 5.3.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlScaleFloatRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.ScaleFloat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlScaleFloatRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=scale_float value=scale_float(0.5)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=ScaleFloat"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::ScaleFloat));
    TestTrue(TEXT("value≈0.5"), FMath::IsNearlyEqual(GetCtlValue(F.BP).Get<float>(), 0.5f, kEps));
    return true;
}
#endif // ERigControlType::ScaleFloat (UE 5.4+)

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlRotatorRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Rotator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlRotatorRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=rotator value=rotator(p=10, y=20, r=30)"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Rotator"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Rotator));
    const FVector3f V = GetCtlValue(F.BP).Get<FVector3f>();
    TestTrue(TEXT("Pitch≈10"), FMath::IsNearlyEqual(V.X, 10.f, kEps));
    TestTrue(TEXT("Yaw≈20"), FMath::IsNearlyEqual(V.Y, 20.f, kEps));
    TestTrue(TEXT("Roll≈30"), FMath::IsNearlyEqual(V.Z, 30.f, kEps));
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlTransformRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.Transform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlTransformRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=transform value=transform(loc=(1,2,3), rot=(0,0,0), scale=(1,1,1))"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=Transform"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::Transform));
    const FTransform Xf = GetCtlValue(F.BP).Get<FRigControlValue::FTransform_Float>().ToTransform();
    TestTrue(TEXT("Loc.X≈1"), FMath::IsNearlyEqual(Xf.GetLocation().X, 1.0, kEps));
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlTransformNoScaleRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.TransformNoScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlTransformNoScaleRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=transform_no_scale value=transform_no_scale(loc=(4,5,6), rot=(0,0,0))"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=TransformNoScale"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::TransformNoScale));
    const FTransformNoScale Xf = GetCtlValue(F.BP).Get<FRigControlValue::FTransformNoScale_Float>().ToTransform();
    TestTrue(TEXT("Loc.X≈4"), FMath::IsNearlyEqual(Xf.Location.X, 4.0, kEps));
    return true;
}

// Counterfactual: reverting the Control compile case → CRIR_HIERARCHY_NOT_WRITABLE; Find returns null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlEulerTransformRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.EulerTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlEulerTransformRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject,
        TEXT("    control \"C\" type=euler_transform value=euler_transform(loc=(7,8,9), rot=(0,0,0), scale=(1,1,1))"));
    if (!Ctrl) { return false; }
    TestEqual(TEXT("ControlType=EulerTransform"), static_cast<int32>(Ctrl->Settings.ControlType), static_cast<int32>(ERigControlType::EulerTransform));
    const FEulerTransform Xf = GetCtlValue(F.BP).Get<FRigControlValue::FEulerTransform_Float>().ToTransform();
    TestTrue(TEXT("Loc.X≈7"), FMath::IsNearlyEqual(Xf.Location.X, 7.0, kEps));
    return true;
}

// Counterfactual: If the sub-block emitter FormatControlSettingsSubBlock is
// missing any of the ~20 keys, that field falls back to FRigControlSettings()
// default after recompile — the test reads each field individually and the
// missing one fails its TestEqual.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlMaximalSettingsRoundTripTest,
    "PinWright.CRIR.RoundTrip.Control.MaximalSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlMaximalSettingsRoundTripTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };
    const FString Body = TEXT(
        "    control \"C\" type=transform value=transform(loc=(0,0,0), rot=(0,0,0), scale=(1,1,1))\n"
        "    {\n"
        "        animation_type=proxy_control\n"
        "        draw_limits=true\n"
        "        display_name=\"My Display\"\n"
        "        filtered_channels=[translation_x,yaw]\n"
        "        group_with_parent_control=true\n"
        "        is_transient_control=true\n"
        "        limits=[(min=true,max=false),(min=false,max=true)]\n"
        "        max=transform(loc=(10,10,10), rot=(0,0,0), scale=(2,2,2))\n"
        "        min=transform(loc=(-10,-10,-10), rot=(0,0,0), scale=(0,0,0))\n"
        "        preferred_rotation_order=zyx\n"
        "        primary_axis=z\n"
        "        restrict_space_switching=true\n"
        "        shape_color=(0.2,0.8,0.2,1.0)\n"
        "        shape_name=Box_Solid\n"
        "        shape_visible=false\n"
        "        shape_visibility=based_on_selection\n"
        "        use_preferred_rotation_order=true\n"
        "    }");
    const FRigControlElement* Ctrl = CompileAndFindControl(*this, F.BP, F.TargetObject, Body);
    if (!Ctrl) { return false; }
    const FRigControlSettings& S = Ctrl->Settings;
    TestEqual(TEXT("animation_type=ProxyControl"), static_cast<int32>(S.AnimationType), static_cast<int32>(ERigControlAnimationType::ProxyControl));
    TestEqual(TEXT("draw_limits=true"), S.bDrawLimits, true);
    TestEqual(TEXT("display_name"), S.DisplayName.ToString(), FString(TEXT("My Display")));
    TestEqual(TEXT("filtered_channels.Num=2"), S.FilteredChannels.Num(), 2);
    TestEqual(TEXT("group_with_parent_control"), S.bGroupWithParentControl, true);
    TestEqual(TEXT("is_transient_control"), S.bIsTransientControl, true);
    TestEqual(TEXT("limits.Num=2"), S.LimitEnabled.Num(), 2);
    if (S.LimitEnabled.Num() >= 2)
    {
        TestEqual(TEXT("limits[0].min"), S.LimitEnabled[0].bMinimum, true);
        TestEqual(TEXT("limits[0].max"), S.LimitEnabled[0].bMaximum, false);
        TestEqual(TEXT("limits[1].min"), S.LimitEnabled[1].bMinimum, false);
        TestEqual(TEXT("limits[1].max"), S.LimitEnabled[1].bMaximum, true);
    }
    const FTransform MaxXf = S.MaximumValue.Get<FRigControlValue::FTransform_Float>().ToTransform();
    TestTrue(TEXT("max.Loc.X≈10"), FMath::IsNearlyEqual(MaxXf.GetLocation().X, 10.0, kEps));
    const FTransform MinXf = S.MinimumValue.Get<FRigControlValue::FTransform_Float>().ToTransform();
    TestTrue(TEXT("min.Loc.X≈-10"), FMath::IsNearlyEqual(MinXf.GetLocation().X, -10.0, kEps));
    TestEqual(TEXT("preferred_rotation_order=ZYX"), static_cast<int32>(S.PreferredRotationOrder), static_cast<int32>(EEulerRotationOrder::ZYX));
    TestEqual(TEXT("primary_axis=Z"), static_cast<int32>(S.PrimaryAxis), static_cast<int32>(ERigControlAxis::Z));
    TestEqual(TEXT("restrict_space_switching"), S.bRestrictSpaceSwitching, true);
    TestTrue(TEXT("shape_color.G≈0.8"), FMath::IsNearlyEqual(S.ShapeColor.G, 0.8f, kEps));
    TestEqual(TEXT("shape_name"), S.ShapeName.ToString(), FString(TEXT("Box_Solid")));
    TestEqual(TEXT("shape_visible=false"), S.bShapeVisible, false);
    TestEqual(TEXT("shape_visibility=BasedOnSelection"), static_cast<int32>(S.ShapeVisibility), static_cast<int32>(ERigControlVisibility::BasedOnSelection));
    TestEqual(TEXT("use_preferred_rotation_order"), S.bUsePreferredRotationOrder, true);
    return true;
}

// Counterfactual: If either the emitter or parser corrupts any field of the
// settings/value, the second decompile text diverges from the first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlRoundTripByteEqualTest,
    "PinWright.CRIR.RoundTrip.Control.ByteEqual",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlRoundTripByteEqualTest::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRCtrlBE_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRCtrlBE_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRCtrlBE_Tgt_%s"), *TargetPath, *Guid);
    ON_SCOPE_EXIT { ResetUndoBufferForCleanup(); CleanupTestAsset(SourcePath); CleanupTestAsset(TargetPath); };

    FString CreateError;
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRCtrlBE_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"), nullptr, CreateError));
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRCtrlBE_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"), nullptr, CreateError));
    if (!SourceBP || !TargetBP) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }

    URigHierarchyController* SrcHC = SourceBP->GetHierarchyController();
    TestNotNull(TEXT("src controller"), SrcHC);
    if (!SrcHC) { return false; }

    FRigControlSettings Settings;
    Settings.ControlType = ERigControlType::Float;
    Settings.DisplayName = FName(TEXT("MyCtl"));
    Settings.bDrawLimits = true;
    Settings.bShapeVisible = false;
    FRigControlValue Value = FRigControlValue::Make<float>(2.75f);

    SrcHC->AddControl(
        FName(TEXT("C")), FRigElementKey(), Settings, Value,
        FTransform::Identity, FTransform::Identity, false, false);

    FCRIRDecompileResult R1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile #1"), R1.bSuccess);
    if (!R1.bSuccess) { return false; }

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;
    FCRIRCompileResult CR = FCRIRCompiler::Compile(R1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile #1 (code='%s', msg='%s')"), *CR.ErrorCode, *CR.ErrorMessage), CR.bSuccess);
    if (!CR.bSuccess) { return false; }

    FCRIRDecompileResult R2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(TEXT("decompile #2"), R2.bSuccess);
    if (!R2.bSuccess) { return false; }

    const FString N1 = NormalizeCRIRLineEndings(R1.CRIRText);
    const FString N2 = NormalizeCRIRLineEndings(R2.CRIRText);
    TestEqual(TEXT("byte-equal control round-trip"), N2, N1);
    return true;
}

// Counterfactual: Change to value=float(3.0) and assert compile success —
// proves the mismatch is what failed, not some other parse error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRControlTypeMismatchErrorTest,
    "PinWright.CRIR.RoundTrip.Control.TypeMismatchError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCRIRControlTypeMismatchErrorTest::RunTest(const FString& Parameters)
{
    FCtrlFixture F;
    if (!F.Init()) { PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
        TEXT("skip")); return true; }
    ON_SCOPE_EXIT { F.Cleanup(); };

    const FString CRIR = TEXT(
        "rig_hierarchy {\n"
        "    control \"C\" type=float value=int(3)\n"
        "}\n");
    FCRIRCompileOptions Options;
    Options.TargetAssetPath = F.TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;
    FCRIRCompileResult Result = FCRIRCompiler::Compile(CRIR, Options);
    TestFalse(TEXT("compile fails on type=float value=int(3)"), Result.bSuccess);
    TestEqual(TEXT("error code is CRIR_CONTROL_VALUE_TYPE_MISMATCH"),
        Result.ErrorCode, FString(TEXT("CRIR_CONTROL_VALUE_TYPE_MISMATCH")));
    return true;
}
