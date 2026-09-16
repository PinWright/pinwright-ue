// Copyright (c) 2026 Alexander Penkin. MIT License.

// Round-trip tests for AGIR anim-node data-pin bindings — the read side of
// B-decompile-agir-omits-wired-data-pin-bindings and the write side of
// B-agir-variable-pin-args-silently-dropped.
//
// Both bugs were the same hole seen from two ends: a data pin can be driven
// either by a wired UK2Node_VariableGet or by a property-access binding, and
// neither shows up in the runtime FAnimNode_* struct that AGIR's reflected-field
// pass walks. So the decompiler printed nothing for a driven pin and the
// compiler fed `$Variable` to ImportText and dropped it — a working AnimGraph
// and one whose inputs were never connected produced byte-identical AGIR, with
// `warnings: []` both times.
//
// Test 1 covers the graph-wired form end to end: hand-wire a getter into a real
// AnimGraph, decompile, assert `$Var` appears (and that the pin's now-dead
// literal does NOT, so "driven, currently 0.25" reads differently from "sitting
// at 0.25"), then compile into a fresh AnimBP and assert the link was rebuilt.
// Counterfactual: dropping AppendBindingFields from the emitter makes the text
// assertion fail; dropping the `$` branch from WriteAnimNodeArg makes the
// rebuilt-link assertion fail.
//
// Test 2 covers the property-binding form: compile hand-authored AGIR carrying
// `bind <path>`, assert the engine records the binding (public HasBinding), and
// re-decompile to assert the same spelling comes back out.
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Compat/EngineVersionCompat.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "TestAGIRFixtures.h"
#include "Tests/TestUtils.h"
#include "UObject/UnrealType.h"

namespace AGIRPinBindingTestHelpers
{
const TCHAR* const BoundVariableName = TEXT("PinWrightBoundAlpha");

UAnimGraphNode_Root* FindRoot(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
        {
            return Root;
        }
    }
    return nullptr;
}

UAnimGraphNode_ApplyAdditive* FindApplyAdditive(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_ApplyAdditive* Match = Cast<UAnimGraphNode_ApplyAdditive>(Node))
        {
            return Match;
        }
    }
    return nullptr;
}

// Declares a member variable whose pin type matches the supplied anim-node pin
// exactly, so the schema wires the getter directly instead of inserting an
// autocast node (which would be a different upstream shape than `$Name` spells).
void AddMatchingMemberVariable(UAnimBlueprint* AnimBP, const FEdGraphPinType& PinType)
{
    FEdGraphPinType VariableType = PinType;
    VariableType.bIsReference = false;
    VariableType.bIsConst = false;
    FBlueprintEditorUtils::AddMemberVariable(AnimBP, FName(BoundVariableName), VariableType);
}
} // namespace AGIRPinBindingTestHelpers

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRPinBindingsVariableGetRoundTripTest,
    "PinWright.AGIR.PinBindings.VariableGetRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRPinBindingsVariableGetRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace AGIRPinBindingTestHelpers;

    // Host Mannequin AnimBP borrowed only for a real skeleton; absence skips.
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AGIRTestFixtures::LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = AGIRTestFixtures::LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/ABP_AGIR_Bind_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/ABP_AGIR_Bind_Dst_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = AGIRTestFixtures::CreateFreshAnimBlueprint(SourcePath, Skeleton);
    if (!TestNotNull(TEXT("source AnimBlueprint created"), SourceBP)) return false;

    UEdGraph* SourceGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(SourceBP);
    if (!TestNotNull(TEXT("source AnimGraph present"), SourceGraph)) return false;

    UAnimGraphNode_Base* ApplyBase = AnimGraphConstructionUtils::CreateAnimNode(
        SourceGraph, UAnimGraphNode_ApplyAdditive::StaticClass(), FVector2D(300, 0));
    UAnimGraphNode_ApplyAdditive* ApplyNode = Cast<UAnimGraphNode_ApplyAdditive>(ApplyBase);
    UAnimGraphNode_Root* RootNode = FindRoot(SourceGraph);
    if (!TestNotNull(TEXT("ApplyAdditive created"), ApplyNode)) return false;
    if (!TestNotNull(TEXT("source AnimGraph default Root present"), RootNode)) return false;

    // A non-default literal on the pin we are about to drive. Once the pin is
    // bound this value is dead, and the whole point of the fix is that the text
    // stops printing it as if it were the node's behaviour.
    ApplyNode->Node.Alpha = 0.25f;

    UEdGraphPin* AlphaPin = ApplyNode->FindPin(FName(TEXT("Alpha")), EGPD_Input);
    if (!TestNotNull(TEXT("ApplyAdditive exposes an Alpha pin by default"), AlphaPin)) return false;

    // Copy the pin type before adding the variable: AddMemberVariable marks the
    // Blueprint structurally modified, which reconstructs nodes and invalidates
    // every UEdGraphPin* held across the call.
    const FEdGraphPinType AlphaPinType = AlphaPin->PinType;
    AddMatchingMemberVariable(SourceBP, AlphaPinType);

    ApplyNode = FindApplyAdditive(SourceGraph);
    RootNode = FindRoot(SourceGraph);
    if (!ApplyNode || !RootNode)
    {
        AddError(TEXT("ApplyAdditive/Root did not survive the variable-add reconstruction"));
        return false;
    }
    AlphaPin = ApplyNode->FindPin(FName(TEXT("Alpha")), EGPD_Input);
    if (!TestNotNull(TEXT("Alpha pin present after reconstruction"), AlphaPin)) return false;

    FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*SourceGraph);
    UK2Node_VariableGet* Getter = GetterCreator.CreateNode(/*bSelectNewNode=*/false);
    Getter->VariableReference.SetSelfMember(FName(BoundVariableName));
    Getter->NodePosX = 0;
    Getter->NodePosY = 0;
    GetterCreator.Finalize();

    UEdGraphPin* GetterOutPin = nullptr;
    for (UEdGraphPin* Pin : Getter->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output)
        {
            GetterOutPin = Pin;
            break;
        }
    }
    if (!TestNotNull(TEXT("variable getter produced an output pin"), GetterOutPin)) return false;

    const UEdGraphSchema* Schema = SourceGraph->GetSchema();
    if (!TestNotNull(TEXT("source graph has a schema"), Schema)) return false;
    TestTrue(TEXT("wired variable getter into ApplyAdditive.Alpha"),
        Schema->TryCreateConnection(GetterOutPin, AlphaPin));
    TestTrue(TEXT("wired ApplyAdditive pose into Root.Result"),
        AnimGraphConstructionUtils::WirePoseLink(
            ApplyNode, FName(TEXT("Pose")), RootNode, FName(TEXT("Result"))));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceBP);

    // --- Read side -------------------------------------------------------
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile of the wired source succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString ExpectedBinding = FString::Printf(TEXT("Alpha: $%s"), BoundVariableName);
    TestTrue(
        FString::Printf(TEXT("AGIR text carries the wired binding '%s'. Text:\n%s"),
            *ExpectedBinding, *DecompileResult.AGIRText),
        DecompileResult.AGIRText.Contains(ExpectedBinding, ESearchCase::CaseSensitive));
    TestFalse(
        TEXT("the bound pin's dead literal is not printed alongside the binding"),
        DecompileResult.AGIRText.Contains(TEXT("Alpha: 0."), ESearchCase::CaseSensitive));
    for (const FString& Warning : DecompileResult.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("a plain member-variable getter is representable (warning: %s)"), *Warning),
            Warning.Contains(TEXT("AGIR_PIN_BINDING_NOT_REPRESENTABLE")));
    }

    // --- Write side ------------------------------------------------------
    UAnimBlueprint* TargetBP = AGIRTestFixtures::CreateFreshAnimBlueprint(TargetPath, Skeleton);
    if (!TestNotNull(TEXT("target AnimBlueprint created"), TargetBP)) return false;
    // AGIR carries graph structure, not variable declarations: the binding
    // target has to already exist on the AnimBP being compiled into.
    AddMatchingMemberVariable(TargetBP, AlphaPinType);

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    const FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    TestTrue(FString::Printf(TEXT("compile to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UEdGraph* TargetGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(TargetBP);
    UAnimGraphNode_ApplyAdditive* TargetApply = FindApplyAdditive(TargetGraph);
    if (!TestNotNull(TEXT("target AnimGraph has an ApplyAdditive node"), TargetApply)) return false;

    UEdGraphPin* TargetAlphaPin = TargetApply->FindPin(FName(TEXT("Alpha")), EGPD_Input);
    if (!TestNotNull(TEXT("target ApplyAdditive has an Alpha pin"), TargetAlphaPin)) return false;
    if (!TestTrue(TEXT("target Alpha pin is linked after compile"), TargetAlphaPin->LinkedTo.Num() > 0))
    {
        return false;
    }

    UK2Node_VariableGet* TargetGetter = TargetAlphaPin->LinkedTo[0]
        ? Cast<UK2Node_VariableGet>(TargetAlphaPin->LinkedTo[0]->GetOwningNode())
        : nullptr;
    if (!TestNotNull(TEXT("target Alpha pin is driven by a variable getter"), TargetGetter)) return false;
    TestEqual(TEXT("the rebuilt getter reads the same member variable"),
        TargetGetter->VariableReference.GetMemberName().ToString(), FString(BoundVariableName));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRPinBindingsPropertyBindingRoundTripTest,
    "PinWright.AGIR.PinBindings.PropertyBindingRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRPinBindingsPropertyBindingRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace AGIRPinBindingTestHelpers;

    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AGIRTestFixtures::LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = AGIRTestFixtures::LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/ABP_AGIR_PropBind_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* TargetBP = AGIRTestFixtures::CreateFreshAnimBlueprint(TargetPath, Skeleton);
    if (!TestNotNull(TEXT("target AnimBlueprint created"), TargetBP)) return false;

    // A float member for the binding path to resolve against, typed to match the
    // Alpha pin so the engine's binding-type recalculation succeeds.
    FEdGraphPinType FloatPinType;
    FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    AddMatchingMemberVariable(TargetBP, FloatPinType);

    const FString AGIRText = FString::Printf(
        TEXT("entry anim_graph AnimGraph {\n")
        TEXT("    %%apply_additive_0 = call `/Script/AnimGraph.AnimGraphNode_ApplyAdditive`(Alpha: bind %s)\n")
        TEXT("    output %%apply_additive_0\n")
        TEXT("}"),
        BoundVariableName);

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    const FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(AGIRText, Options);
    TestTrue(FString::Printf(TEXT("compile of hand-authored 'bind' AGIR succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;
    for (const FString& Warning : CompileResult.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("the 'bind' argument was not rejected by the field writer (warning: %s)"), *Warning),
            Warning.Contains(TEXT("Alpha")));
    }

    UEdGraph* TargetGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(TargetBP);
    UAnimGraphNode_ApplyAdditive* TargetApply = FindApplyAdditive(TargetGraph);
    if (!TestNotNull(TEXT("target AnimGraph has an ApplyAdditive node"), TargetApply)) return false;

    // Engine-side readback: the binding really is recorded on the node, not
    // just accepted by the parser.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("compiled node reports a property binding on Alpha"),
        TargetApply->HasBinding(FName(TEXT("Alpha"))));
#else
    // UAnimGraphNode_Base::HasBinding is protected through UE 5.3 (it became public in 5.4), and
    // so is the PropertyBindings map it reads. Read the same map reflectively - the way
    // AGIRPinBindings itself reaches it - so this is still the engine's own record of the
    // binding rather than a re-read of the compiler's bookkeeping.
    bool bHasAlphaBinding = false;
    if (FMapProperty* BindingMap =
            FindFProperty<FMapProperty>(TargetApply->GetClass(), TEXT("PropertyBindings")))
    {
        FScriptMapHelper MapHelper(
            BindingMap, BindingMap->ContainerPtrToValuePtr<void>(TargetApply));
        for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
        {
            if (MapHelper.IsValidIndex(Index)
                && *reinterpret_cast<const FName*>(MapHelper.GetKeyPtr(Index))
                       == FName(TEXT("Alpha")))
            {
                bHasAlphaBinding = true;
                break;
            }
        }
    }
    TestTrue(TEXT("compiled node reports a property binding on Alpha"), bHasAlphaBinding);
#endif

    const FAGIRDecompileResult DecompileResult = FAGIRDecompiler(TargetBP).Decompile();
    TestTrue(TEXT("decompile of the bound target succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString ExpectedBinding = FString::Printf(TEXT("Alpha: bind %s"), BoundVariableName);
    TestTrue(
        FString::Printf(TEXT("AGIR text carries the property binding '%s'. Text:\n%s"),
            *ExpectedBinding, *DecompileResult.AGIRText),
        DecompileResult.AGIRText.Contains(ExpectedBinding, ESearchCase::CaseSensitive));

    return true;
}
