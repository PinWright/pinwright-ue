// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR round-trip and hierarchy-emit regression tests.
//
// Test 1 (FCRIRRoundTripForwardsSolveTest): builds a synthetic Control Rig BP
// with FRigUnit_BeginExecution wired to FRigUnit_SetBoneTransform via
// URigVMController directly (ground truth, not via CRIR), then decompile →
// compile to fresh asset → decompile and assert byte-equal CRIR text.
//
// Test 2 (FCRIRHierarchyEmitTest): loads the project's Mannequin Control Rig
// (CR_Mannequin_Body) and asserts the rig_hierarchy block enumerates one line
// per hierarchy element.
#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "CRIR/CRIRParser.h"
#include "CRIR/CRIROpcodes.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"

namespace
{
// URigVMNode::FindExecutePin() is UE 5.6+ (RigVMNode.h:139). This reproduces its body exactly
// (RigVMNode.cpp:599-609) on older engines; URigVMPin::IsExecuteContext() and GetPins() are
// public on every supported version, so the two spellings pick the same pin.
URigVMPin* CRIRTest_FindExecutePin(const URigVMNode* Node)
{
    if (!Node)
    {
        return nullptr;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    return Node->FindExecutePin();
#else
    for (URigVMPin* Pin : Node->GetPins())
    {
        if (Pin->IsExecuteContext())
        {
            return Pin;
        }
    }
    return nullptr;
#endif
}

const TCHAR* LyraMannequinControlRigPath =
    TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body");

int32 CountHierarchyLines(const FString& CRIRText)
{
    // Count lines whose first non-whitespace token is a hierarchy element kind
    // keyword. Robust to indentation and to other top-level blocks (rig_graph)
    // that don't contain these keywords as line prefixes.
    TArray<FString> Lines;
    CRIRText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
    int32 Count = 0;
    for (const FString& Raw : Lines)
    {
        FString Trimmed = Raw;
        Trimmed.TrimStartInline();
        if (Trimmed.StartsWith(TEXT("bone "))
            || Trimmed.StartsWith(TEXT("null "))
            || Trimmed.StartsWith(TEXT("control "))
            || Trimmed.StartsWith(TEXT("socket ")))
        {
            ++Count;
        }
    }
    return Count;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRRoundTripForwardsSolveTest,
    "PinWright.CRIR.RoundTrip.ForwardsSolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRRoundTripForwardsSolveTest::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/CR_CRIR_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/CR_CRIR_Tgt_%s"), *Guid);
    const FString SourceObject = FString::Printf(TEXT("%s.CR_CRIR_Src_%s"), *SourcePath, *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIR_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIR_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        /*TargetSkeleton*/ nullptr,
        CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR round-trip: could not create source CR BP (%s) - skipped."),
                *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIR_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        /*TargetSkeleton*/ nullptr,
        CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR round-trip: could not create target CR BP (%s) - skipped."),
                *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source model controller available"), SourceController);
    if (!SourceController || !SourceModel)
    {
        return false;
    }

    // Build ground-truth source via URigVMController directly. AddUnitNodeFromStructPath
    // is the same call site CRIRCompiler uses for both event entries and regular units.
    URigVMNode* BeginNode = SourceController->AddUnitNodeFromStructPath(
        TEXT("/Script/ControlRig.RigUnit_BeginExecution"),
        FRigUnit::GetMethodName(),
        FVector2D(-200, 0),
        /*InNodeName*/ FString(TEXT("BeginExecution")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    URigVMNode* SetBoneNode = SourceController->AddUnitNodeFromStructPath(
        TEXT("/Script/ControlRig.RigUnit_SetBoneTransform"),
        FRigUnit::GetMethodName(),
        FVector2D(200, 0),
        /*InNodeName*/ FString(TEXT("SetBoneTransform")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("BeginExecution node created on source"), BeginNode);
    TestNotNull(TEXT("SetBoneTransform node created on source"), SetBoneNode);
    if (!BeginNode || !SetBoneNode)
    {
        return false;
    }

    // Ask the model for the exec pin's name; never spell it literally here.
    // On UE 5.8 FRigUnitMutable's exec property is `FRigVMExecutePin ExecutePin`
    // (RigUnit.h:121), so the pin — and therefore the emitted `wire_in_<pin>`
    // arg — is named `ExecutePin`, not `ExecuteContext`.
    //
    // Why a hardcoded spelling survived here so long: URigVMNode::FindPin falls
    // back to FindExecutePin() when the requested name matches EITHER
    // FRigVMStruct::ExecuteContextName or FRigVMStruct::ExecutePinName
    // (RigVMNode.cpp:573-580), so the legacy `…ExecuteContext` path below still
    // resolved and AddLink still SUCCEEDED. The stale literal was therefore
    // invisible in the setup and only surfaced on the emit assertion further
    // down — which reads as a decompiler defect rather than a test defect. The
    // sibling CRIR.RoundTrip.InvokeEntry test is unaffected because
    // URigVMInvokeEntryNode synthesises its exec pin from
    // FRigVMStruct::ExecuteContextName, so both spellings agree there.
    URigVMPin* BeginExecPin = CRIRTest_FindExecutePin(BeginNode);
    URigVMPin* SetBoneExecPin = CRIRTest_FindExecutePin(SetBoneNode);
    TestNotNull(TEXT("BeginExecution exposes an execute pin"), BeginExecPin);
    TestNotNull(TEXT("SetBoneTransform exposes an execute pin"), SetBoneExecPin);
    if (!BeginExecPin || !SetBoneExecPin)
    {
        return false;
    }
    // The wire arg is keyed on the TARGET pin's name (CRIRDecompiler.cpp
    // CollectArgsForNode: `Arg.Name = "wire_in_" + TargetSegment`).
    const FString ExecPinName = SetBoneExecPin->GetName();

    const FString BeginExecPinPath = FString::Printf(TEXT("%s.%s"), *BeginNode->GetName(), *BeginExecPin->GetName());
    const FString SetBoneExecPinPath = FString::Printf(TEXT("%s.%s"), *SetBoneNode->GetName(), *ExecPinName);
    const bool bWired = SourceController->AddLink(
        BeginExecPinPath,
        SetBoneExecPinPath,
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestTrue(FString::Printf(TEXT("wired %s -> %s"), *BeginExecPinPath, *SetBoneExecPinPath), bWired);

    // Author a non-default literal on a plain unit-node input pin. This is the
    // node-kind counterpart to CRIR.RoundTrip.Reroute: the decompiler gates
    // literal emission on a single shared predicate, and while that predicate
    // was URigVMPin::HasDefaultValueOverride() — which returns false whenever
    // the shipped-off `RigVM.EnablePinOverrides` CVar is off — EVERY authored
    // pin value was dropped, on every node kind. The reroute test alone covers
    // only the reroute arm; this covers CollectArgsForNode.
    const FString WeightPinPath = FString::Printf(TEXT("%s.Weight"), *SetBoneNode->GetName());
    const bool bWeightSet = SourceController->SetPinDefaultValue(
        WeightPinPath,
        TEXT("0.750000"),
        /*bResizeArrays*/ true,
        /*bSetupUndoRedo*/ false,
        /*bMergeUndoAction*/ false,
        /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("authored SetBoneTransform.Weight = 0.750000 on the source"), bWeightSet);
    URigVMPin* WeightPin = SetBoneNode->FindPin(TEXT("Weight"));
    TestNotNull(TEXT("SetBoneTransform exposes a Weight pin"), WeightPin);
    if (!WeightPin)
    {
        return false;
    }
    // Read the authored value back from the model rather than restating the
    // literal: RigVM post-processes default-value strings, and the assertion
    // below is about the emitter carrying the pin's value, not about float
    // formatting. Guard that the fixture really moved off the struct default
    // (Weight defaults to 1.0) — otherwise the emit assertion is vacuous.
    const FString AuthoredWeight = WeightPin->GetDefaultValue();
    TestTrue(FString::Printf(TEXT("Weight really differs from the struct default (got '%s')"), *AuthoredWeight),
        AuthoredWeight.StartsWith(TEXT("0.75")));

    // Decompile #1: source ground truth.
    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 succeeds (errorCode='%s')"), *Result1.ErrorCode),
        Result1.bSuccess);
    if (!Result1.bSuccess)
    {
        return false;
    }

    // Independent reference on the EMIT side. The byte-equality assertion at the
    // bottom of this test covers the compiler half of the round-trip (see the
    // AddLink counterfactual there) but is blind to the decompiler half: both
    // decompiles run the same emitter, so anything the emitter stops writing is
    // missing from both texts and they stay byte-equal. Concretely, gutting the
    // wire loop in CRIRDecompiler.cpp `CollectArgsForNode` (the block that builds
    // `Arg.Name = "wire_in_" + TargetSegment`) leaves decompile #1 without the
    // exec wire, the compiler with nothing to connect, and decompile #2 matching
    // it exactly — a fully green test over a graph that lost its only link. Same
    // for the SetBoneTransform node line: dropping it from the emitter keeps
    // `NodesCreated > 0` satisfied by the surviving BeginExecution alone.
    TestTrue(TEXT("decompile #1 emits the BeginExecution unit"),
        Result1.CRIRText.Contains(TEXT("/Script/ControlRig.RigUnit_BeginExecution")));
    TestTrue(TEXT("decompile #1 emits the SetBoneTransform unit"),
        Result1.CRIRText.Contains(TEXT("/Script/ControlRig.RigUnit_SetBoneTransform")));
    TestTrue(FString::Printf(TEXT("decompile #1 emits the exec wire argument wire_in_%s=%%"), *ExecPinName),
        Result1.CRIRText.Contains(FString::Printf(TEXT("wire_in_%s=%%"), *ExecPinName)));
    // Emit-side reference for the pin-literal path. Byte-equality below cannot
    // see this: a value the emitter stops writing is absent from both decompiles.
    TestTrue(FString::Printf(TEXT("decompile #1 carries the authored SetBoneTransform.Weight literal (Weight=%s)"),
        *AuthoredWeight),
        Result1.CRIRText.Contains(FString::Printf(TEXT("Weight=%s"), *AuthoredWeight)));

    // Compile into the (already-empty) fresh target.
    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    // Floor is 2, not 1: the fixture authors exactly two unit nodes, so `> 0` was
    // still satisfied when only one of them survived the emit/compile path. The
    // floor is stated as `>=` rather than `==` because a freshly created Control
    // Rig Blueprint may carry engine-authored default nodes that also compile.
    TestTrue(FString::Printf(TEXT("compile created both authored nodes (got %d)"), CompileResult.NodesCreated),
        CompileResult.NodesCreated >= 2);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    // Decompile #2: round-tripped target.
    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 succeeds (errorCode='%s')"), *Result2.ErrorCode),
        Result2.bSuccess);
    if (!Result2.bSuccess)
    {
        return false;
    }

    // Counterfactual: if the URigVMController::AddLink call in
    // CRIRPinResolver.cpp::ResolveAndConnect (Pass B of CRIRCompiler::Compile)
    // is reverted to a no-op, the round-tripped target asset has unconnected
    // nodes and Result2.CRIRText omits the wire_in_<execPin>=%<src>.<execPin>
    // arg on the SetBoneTransform node, breaking byte-equality with Result1.
    // Byte-equality also covers the compile half of the Weight literal: if the
    // compiler stops reapplying pin defaults, decompile #2 loses `Weight=` while
    // decompile #1 keeps it.
    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("CRIR text round-trips byte-equal"), Normalized2, Normalized1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRHierarchyEmitTest,
    "PinWright.CRIR.Decompile.HierarchyEmit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRHierarchyEmitTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinControlRigPath);
    UControlRigBlueprint* BP = LoadObject<UControlRigBlueprint>(nullptr, LyraMannequinControlRigPath);
    if (!TestNotNull(TEXT("Mannequin Control Rig fixture loaded"), BP)) return false;

    FCRIRDecompileResult Result = FCRIRDecompiler(BP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile succeeds (errorCode='%s')"), *Result.ErrorCode),
        Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    URigHierarchy* Hierarchy = GetControlRigHierarchy(BP);
    if (!TestNotNull(TEXT("Control Rig hierarchy present"), Hierarchy)) return false;
    const int32 Expected = Hierarchy->Num();
    if (!TestTrue(TEXT("Mannequin Control Rig has hierarchy elements"), Expected > 0)) return false;

    const int32 Emitted = CountHierarchyLines(Result.CRIRText);

    // Counterfactual: reverting the recursive child walk in CRIRDecompiler.cpp's
    // hierarchy emit (CollectElementsPreOrder) to a single-level walk drops nested
    // bones; for any rig with hierarchy depth > 1 the assertion fails.
    //
    // Tolerance: unsupported element kinds (curve/reference/connector/physics)
    // emit as `# TODO` lines instead of `bone/null/control/socket`, so Emitted
    // can legitimately be less than Expected. We assert at least half of the
    // hierarchy made it through — enough to detect a single-level walk
    // regression on any rig with depth > 1, while tolerating rigs that mix in
    // unsupported element kinds.
    TestTrue(FString::Printf(
        TEXT("hierarchy lines emitted (%d) cover at least half of hierarchy elements (%d)"),
        Emitted, Expected),
        Emitted >= Expected / 2 && Emitted > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRRoundTripHierarchyTest,
    "PinWright.CRIR.RoundTrip.Hierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRRoundTripHierarchyTest::RunTest(const FString& Parameters)
{
    // Wave-1 hierarchy mutation regression: build a synthetic Control Rig BP
    // with bone/null/socket elements via URigHierarchyController directly
    // (ground truth), decompile to CRIR text, compile that text into a fresh
    // empty target BP, decompile again, and assert byte-equal CRIR text.
    //
    // Counterfactual: if CompileRigHierarchyBlock is reverted to the original
    // early CRIR_HIERARCHY_NOT_WRITABLE error, the compile-into-target step
    // returns bSuccess=false and the TestTrue assertion below fails.
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/CR_CRIRHier_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/CR_CRIRHier_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRHier_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRHier_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        /*TargetSkeleton*/ nullptr,
        CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR hierarchy round-trip: could not create source CR BP (%s) - skipped."),
                *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRHier_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        /*TargetSkeleton*/ nullptr,
        CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR hierarchy round-trip: could not create target CR BP (%s) - skipped."),
                *CreateError));
        return true;
    }

    URigHierarchyController* SourceHierController = SourceBP->GetHierarchyController();
    TestNotNull(TEXT("source URigHierarchyController available"), SourceHierController);
    if (!SourceHierController)
    {
        return false;
    }

    // Ground-truth hierarchy: bone "root", null "ctrl_root" parented to root
    // with a non-default translation offset, socket "attach_point" parented
    // to root. The non-default offset on ctrl_root exercises the transform
    // attribute parser; bone+socket parented to root exercises the parent
    // resolver (AddedByName cache + Hierarchy->Find fallback).
    const FRigElementKey RootKey = SourceHierController->AddBone(
        FName(TEXT("root")), FRigElementKey(), FTransform::Identity,
        /*bTransformInGlobal*/ false, ERigBoneType::User,
        /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("source root bone added"), RootKey.Type == ERigElementType::Bone);
    const FRigElementKey CtrlRootKey = SourceHierController->AddNull(
        FName(TEXT("ctrl_root")), RootKey,
        FTransform(FQuat::Identity, FVector(0.0, 0.0, 12.0), FVector::OneVector),
        /*bTransformInGlobal*/ false,
        /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("source ctrl_root null added"), CtrlRootKey.Type == ERigElementType::Null);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // ControlRig sockets (AddSocket / ERigElementType::Socket) are UE 5.4+; on 5.3 the round-trip
    // fixture omits the socket element and still exercises bone/null/control parenting.
    const FRigElementKey AttachKey = SourceHierController->AddSocket(
        FName(TEXT("attach_point")), RootKey, FTransform::Identity,
        /*bTransformInGlobal*/ false, FLinearColor::White, FString(),
        /*bSetupUndo*/ false, /*bPrintPythonCommand*/ false);
    TestTrue(TEXT("source attach_point socket added"), AttachKey.Type == ERigElementType::Socket);
#endif

    // Decompile #1: source ground truth.
    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 succeeds (errorCode='%s')"), *Result1.ErrorCode),
        Result1.bSuccess);
    if (!Result1.bSuccess)
    {
        return false;
    }

    // Compile into the (already-empty) fresh target.
    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile hierarchy to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    // Decompile #2: round-tripped target.
    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 succeeds (errorCode='%s')"), *Result2.ErrorCode),
        Result2.bSuccess);
    if (!Result2.bSuccess)
    {
        return false;
    }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("hierarchy round-trip text equality"), Normalized2, Normalized1);
    return true;
}

// Regression for B-crir-decompile-forward-ref-wire: the decompiler sorts a
// graph's nodes by UObject name (CRIRDecompiler.cpp EmitGraphBody), not by
// exec/topological order, so it can emit a `wire_in_*=%nN` whose source `%nN`
// is declared on a *later* line of the same rig_graph (a forward reference).
// The compile path runs FCRIRParser::Parse with bSkipReferenceValidation=false
// (CRIRCompiler.cpp), so before the fix that text — the decompiler's own
// output — was rejected with CRIR_UNDEFINED_REF, breaking the documented
// "decompile -> compile -> decompile is byte-equal" invariant
// (crir-language-reference.md:489).
//
// This test exercises the production validation gate (ValidateInstructionScope)
// directly via FCRIRParser::Parse, using the ticket's minimal repro text. If the
// two-pass pre-scan in ValidateInstructionScope is reverted to single-pass, the
// forward-ref parse below fails and this test fails. The dangling-ref case pins
// that the fix did NOT degenerate into accepting every reference.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRForwardRefWireParsesTest,
    "PinWright.CRIR.Parse.ForwardRefWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRForwardRefWireParsesTest::RunTest(const FString& Parameters)
{
    // %n2 (ParentConstraint) wires its exec from %n7 (SetTransform), which is
    // declared 3 lines LATER in the same rig_graph — exactly the name-sort vs.
    // exec-order divergence the decompiler emits for a real FK rig.
    const FString ForwardRefText =
        TEXT("rig_graph \"RigVMModel\" {\n")
        TEXT("    %n0 = unit /Script/ControlRig.RigUnit_BeginExecution()\n")
        TEXT("    %n1 = unit /Script/ControlRig.RigUnit_GetTransform()\n")
        TEXT("    %n2 = unit /Script/ControlRig.RigUnit_ParentConstraint(wire_in_ExecutePin=%n7.ExecutePin)\n")
        TEXT("    %n3 = unit /Script/ControlRig.RigUnit_ParentConstraint(wire_in_ExecutePin=%n2.ExecutePin)\n")
        TEXT("    %n7 = unit /Script/ControlRig.RigUnit_SetTransform(wire_in_ExecutePin=%n0.ExecutePin, wire_in_Value=%n1.Transform)\n")
        TEXT("}\n");

    {
        TArray<FCRIREntryBlock> Blocks;
        TArray<FCRIRParseError> Errors;
        const bool bOk = FCRIRParser::Parse(
            ForwardRefText, Blocks, Errors, /*bSkipReferenceValidation*/ false);

        TestTrue(FString::Printf(
            TEXT("forward-ref wire parses with full reference validation (errors='%s')"),
            *JoinCRIRParseErrors(Errors)), bOk);
        TestEqual(TEXT("forward-ref wire produces no parse errors"), Errors.Num(), 0);
    }

    // Negative control: a wire whose source %n9 is declared NOWHERE in the scope
    // must still be rejected — the fix tolerates forward refs, not missing ones.
    {
        const FString DanglingRefText =
            TEXT("rig_graph \"RigVMModel\" {\n")
            TEXT("    %n0 = unit /Script/ControlRig.RigUnit_BeginExecution()\n")
            TEXT("    %n1 = unit /Script/ControlRig.RigUnit_SetTransform(wire_in_ExecutePin=%n9.ExecutePin)\n")
            TEXT("}\n");

        TArray<FCRIREntryBlock> Blocks;
        TArray<FCRIRParseError> Errors;
        const bool bOk = FCRIRParser::Parse(
            DanglingRefText, Blocks, Errors, /*bSkipReferenceValidation*/ false);
        TestFalse(TEXT("genuinely-dangling wire ref is still rejected"), bOk);

        bool bHasUndefinedRef = false;
        for (const FCRIRParseError& Err : Errors)
        {
            if (Err.Code == TEXT("CRIR_UNDEFINED_REF")) { bHasUndefinedRef = true; break; }
        }
        TestTrue(TEXT("dangling ref reported as CRIR_UNDEFINED_REF"), bHasUndefinedRef);
    }

    return true;
}

// End-to-end companion for B-crir-decompile-forward-ref-wire: builds a synthetic
// Control Rig whose two wired nodes are named so that UObject-name sort is the
// REVERSE of exec order — the wire SOURCE node ("ZBegin") sorts AFTER the wire
// SINK node ("ASet"). The decompiler therefore assigns the sink %n0 and the
// source %n1, emitting a forward-ref wire on the sink. Decompiling, compiling
// that text into a fresh asset, and decompiling again must be byte-equal.
//
// Counterfactual: revert ValidateInstructionScope to single-pass and the
// FCRIRCompiler::Compile step below fails with CRIR_UNDEFINED_REF (the compiler
// parses with validation on), so CompileResult.bSuccess is false and the
// assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRRoundTripForwardRefWireTest,
    "PinWright.CRIR.RoundTrip.ForwardRefWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRRoundTripForwardRefWireTest::RunTest(const FString& Parameters)
{
    // Distinguishing fixture only — the create/decompile/compile/decompile/
    // byte-equal scaffold lives in RunCRIRRoundTrip (CRIRTestHelpers.h).
    return RunCRIRRoundTrip(*this, TEXT("CRIRFwd"),
        [this](URigVMController* SourceController, URigVMGraph* /*SourceModel*/)
        {
            // Names chosen so name-sort == REVERSE of exec order: the exec SOURCE
            // ("ZBegin") sorts after the exec SINK ("ASet"). This is what forces
            // the decompiler to emit a forward-ref wire on the sink line.
            URigVMNode* BeginNode = SourceController->AddUnitNodeFromStructPath(
                TEXT("/Script/ControlRig.RigUnit_BeginExecution"),
                FRigUnit::GetMethodName(),
                FVector2D(-200, 0),
                /*InNodeName*/ FString(TEXT("ZBegin")),
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            URigVMNode* SetBoneNode = SourceController->AddUnitNodeFromStructPath(
                TEXT("/Script/ControlRig.RigUnit_SetBoneTransform"),
                FRigUnit::GetMethodName(),
                FVector2D(200, 0),
                /*InNodeName*/ FString(TEXT("ASet")),
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            TestNotNull(TEXT("ZBegin node created on source"), BeginNode);
            TestNotNull(TEXT("ASet node created on source"), SetBoneNode);
            if (!BeginNode || !SetBoneNode)
            {
                return;
            }
            // Confirm the fixture actually inverts name-sort vs. exec order,
            // otherwise it would not exercise the forward-ref path at all.
            TestTrue(TEXT("exec source name sorts AFTER exec sink name (forces forward ref)"),
                BeginNode->GetName().Compare(SetBoneNode->GetName(), ESearchCase::CaseSensitive) > 0);

            const FString BeginExecPin = FString::Printf(TEXT("%s.ExecuteContext"), *BeginNode->GetName());
            const FString SetBoneExecPin = FString::Printf(TEXT("%s.ExecuteContext"), *SetBoneNode->GetName());
            const bool bWired = SourceController->AddLink(
                BeginExecPin,
                SetBoneExecPin,
                /*bSetupUndoRedo*/ false,
                /*bPrintPythonCommand*/ false);
            TestTrue(TEXT("wired ZBegin.ExecuteContext -> ASet.ExecuteContext"), bWired);
        });
}
