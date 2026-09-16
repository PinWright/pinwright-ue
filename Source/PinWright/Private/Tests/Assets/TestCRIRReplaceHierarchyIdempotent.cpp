// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for B-crir-replace-duplicates-hierarchy: `mode=replace` must clear
// (upsert) the rig hierarchy, not append to it. Compiling a rig's OWN decompiled
// hierarchy back into the SAME asset (the in-place round-trip) must reproduce the
// rig, not double every element with a `_2` suffix.
//
// Before the fix, CompileRigHierarchyBlock ignored Options.Mode and drove
// URigHierarchyController::Add* unconditionally; a same-name add was auto-renamed
// by GetSafeNewName to `<name>_2`, so an in-place replace grew the hierarchy from
// N to 2N elements silently. This test fails (element count doubles, decompile #2
// != decompile #1, no duplicate warning emitted) if the clear-first/upsert step
// is reverted.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"
#include "Utils/ControlRigBlueprintCompat.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRReplaceHierarchyIdempotentTest,
    "PinWright.CRIR.RoundTrip.ReplaceHierarchyIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRReplaceHierarchyIdempotentTest::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Name = FString::Printf(TEXT("CR_CRIRReplaceIdem_%s"), *Guid);
    const FString PackageDir = TEXT("/Game/PinWrightTests");
    const FString AssetPath = FString::Printf(TEXT("%s/%s"), *PackageDir, *Name);
    const FString AssetObject = FString::Printf(TEXT("%s.%s"), *AssetPath, *Name);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    FString CreateError;
    UControlRigBlueprint* BP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        Name, PackageDir, /*TargetSkeleton*/ nullptr, CreateError));
    if (!BP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR replace-idempotent: could not create CR BP (%s) - skipped."), *CreateError));
        return true;
    }

    URigHierarchyController* HierarchyController = BP->GetHierarchyController();
    TestNotNull(TEXT("hierarchy controller"), HierarchyController);
    if (!HierarchyController) { return false; }

    URigHierarchy* Hierarchy = GetControlRigHierarchy(BP);
    TestNotNull(TEXT("hierarchy"), Hierarchy);
    if (!Hierarchy) { return false; }

    // Build a small but representative hierarchy: a root bone, a child bone, a
    // null, a control, and a curve. These are the kinds CompileRigHierarchyBlock
    // drives, so a doubling bug shows up across element types.
    const FRigElementKey RootBone = HierarchyController->AddBone(
        FName(TEXT("root")), FRigElementKey(), FTransform::Identity,
        /*bTransformInGlobal*/ false, ERigBoneType::User, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("root bone added"), RootBone.Type == ERigElementType::Bone);

    const FRigElementKey ChildBone = HierarchyController->AddBone(
        FName(TEXT("spine")), RootBone, FTransform(FVector(0, 0, 10)),
        /*bTransformInGlobal*/ false, ERigBoneType::User, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("child bone added"), ChildBone.Type == ERigElementType::Bone);

    const FRigElementKey NullKey = HierarchyController->AddNull(
        FName(TEXT("offset")), RootBone, FTransform::Identity,
        /*bTransformInGlobal*/ false, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("null added"), NullKey.Type == ERigElementType::Null);

    FRigControlSettings ControlSettings;
    ControlSettings.ControlType = ERigControlType::Float;
    const FRigElementKey ControlKey = HierarchyController->AddControl(
        FName(TEXT("ctrl")), NullKey, ControlSettings, FRigControlValue::Make<float>(0.f),
        FTransform::Identity, FTransform::Identity, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("control added"), ControlKey.Type == ERigElementType::Control);

    const FRigElementKey CurveKey = HierarchyController->AddCurve(
        FName(TEXT("blend")), 0.25f, /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("curve added"), CurveKey.Type == ERigElementType::Curve);

    const int32 BaselineCount = Hierarchy->GetAllKeys(/*bTraverse*/ false).Num();
    TestEqual(TEXT("baseline element count"), BaselineCount, 5);

    // Decompile #1: the rig's own ground-truth text.
    FCRIRDecompileResult Result1 = FCRIRDecompiler(BP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Compile that text back into the SAME asset in Replace mode. This is the
    // in-place round-trip the ticket reported as broken. With the fix, replace
    // clears each colliding element first, so the count stays at BaselineCount;
    // without it, every element is auto-renamed to `<name>_2` and the count doubles.
    FCRIRCompileOptions Options;
    Options.TargetAssetPath = AssetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("in-place replace compile (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    // Core assertion: replace did not append. Pre-fix this is 2*BaselineCount.
    const int32 AfterCount = Hierarchy->GetAllKeys(/*bTraverse*/ false).Num();
    TestEqual(TEXT("replace did not double the hierarchy"), AfterCount, BaselineCount);

    // No element should have been auto-renamed to a `_2` collision name.
    bool bFoundCollisionName = false;
    for (const FRigElementKey& Key : Hierarchy->GetAllKeys(/*bTraverse*/ false))
    {
        if (Key.Name.ToString().EndsWith(TEXT("_2")))
        {
            bFoundCollisionName = true;
            AddError(FString::Printf(TEXT("hierarchy contains an auto-renamed collision element '%s'"), *Key.Name.ToString()));
        }
    }
    TestFalse(TEXT("no `_2` auto-renamed collision element exists"), bFoundCollisionName);

    // In a clean replace there is no collision, so no duplicate warning should fire.
    for (const FString& Warning : CompileResult.Warnings)
    {
        TestFalse(FString::Printf(TEXT("replace emitted no spurious duplicate warning: %s"), *Warning),
            Warning.Contains(TEXT("CRIR_HIERARCHY_DUPLICATE_ELEMENT")));
    }

    // Decompile #2: must match #1 byte-for-byte (the documented round-trip).
    FCRIRDecompileResult Result2 = FCRIRDecompiler(BP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("in-place replace round-trip text equality"), Normalized2, Normalized1);

    return true;
}
