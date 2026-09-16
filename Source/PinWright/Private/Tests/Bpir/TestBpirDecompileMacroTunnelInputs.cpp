// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for BPIR decompiler resolving macro-entry tunnel output pins
// to $ParamName references inside the macro body.
//
// Counterfactual: if the new UK2Node_Tunnel macro-entry branch added in
// Decompiler/BpirDecompiler.cpp (around lines 1363-1375) is reverted, the
// resolver falls through to the NodeToValueName lookup which has no entry for
// the entry tunnel, then to the catch-all at lines 1449-1453, emitting `?`
// and the `Unresolvable value: source node 'Inputs'` warning instead of $Pawn.

#include "Misc/AutomationTest.h"

#include "Decompiler/BpirDecompiler.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/KismetSystemLibrary.h"
#include "K2Node_Tunnel.h"
#include "K2Node_CallFunction.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDecompileMacroTunnelInputsTest,
    "PinWright.bpir.decompile.MacroTunnelInputsResolveToParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDecompileMacroTunnelInputsTest::RunTest(const FString& Parameters)
{
    // Build a transient host Blueprint to own the macro graph
    const FName BPName = *FString::Printf(TEXT("TestMacroTunnelInputsBP_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        BPName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("PinWrightTests")));

    if (!BP)
    {
        AddError(TEXT("Failed to create host blueprint"));
        return false;
    }

    // Create a macro graph; AddMacroGraph spawns the entry+exit tunnel pair
    UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        BP,
        FName(TEXT("TestMacro")),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());

    if (!MacroGraph)
    {
        AddError(TEXT("CreateNewGraph failed"));
        return false;
    }

    FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph,
        /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));

    // Locate auto-created tunnel pair (predicate matches GraphWalker.cpp:149)
    UK2Node_Tunnel* EntryTunnel = nullptr;
    UK2Node_Tunnel* ExitTunnel = nullptr;
    for (UEdGraphNode* Node : MacroGraph->Nodes)
    {
        UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
        if (!Tunnel) continue;
        if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
            EntryTunnel = Tunnel;
        else if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
            ExitTunnel = Tunnel;
    }
    if (!EntryTunnel || !ExitTunnel)
    {
        AddError(TEXT("Failed to find tunnel pair"));
        return false;
    }

    // Add a 'Pawn' output data pin (object) on the entry tunnel
    FEdGraphPinType ObjectPinType;
    ObjectPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ObjectPinType.PinSubCategoryObject = UObject::StaticClass();
    EntryTunnel->CreateUserDefinedPin(
        FName(TEXT("Pawn")),
        ObjectPinType,
        EGPD_Output);

    // Add a body call: UKismetSystemLibrary::IsValid(Object) — one object input.
    UK2Node_CallFunction* IsValidCall = NewObject<UK2Node_CallFunction>(MacroGraph);
    IsValidCall->CreateNewGuid();
    IsValidCall->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, IsValid),
        UKismetSystemLibrary::StaticClass());
    IsValidCall->PostPlacedNewNode();
    IsValidCall->AllocateDefaultPins();
    MacroGraph->AddNode(IsValidCall, /*bFromUI=*/true, /*bSelectNewNode=*/false);
    IsValidCall->ReconstructNode();

    // Wire entry tunnel's Pawn output -> IsValid's Object input
    UEdGraphPin* PawnOutPin = EntryTunnel->FindPin(FName(TEXT("Pawn")), EGPD_Output);
    UEdGraphPin* ObjectInPin = IsValidCall->FindPin(FName(TEXT("Object")), EGPD_Input);
    if (!PawnOutPin || !ObjectInPin)
    {
        AddError(TEXT("Failed to find Pawn/Object pins for wiring"));
        return false;
    }
    PawnOutPin->MakeLinkTo(ObjectInPin);

    // Add a 'Result' input data pin (bool) on the exit tunnel and wire ReturnValue -> Result
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    ExitTunnel->CreateUserDefinedPin(
        FName(TEXT("Result")),
        BoolPinType,
        EGPD_Input);

    UEdGraphPin* ReturnValuePin = IsValidCall->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
    UEdGraphPin* ResultInPin = ExitTunnel->FindPin(FName(TEXT("Result")), EGPD_Input);
    if (ReturnValuePin && ResultInPin)
    {
        ReturnValuePin->MakeLinkTo(ResultInPin);
    }

    // Decompile through the production decompiler entry point
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("Decompile reported success"), Result.bSuccess);

    // Body must reference the macro parameter as $Pawn
    TestTrue(TEXT("BPIR body references $Pawn"),
        Result.BpirText.Contains(TEXT("$Pawn")));

    // Body must NOT contain unresolved '?' value at a callsite ' ?'
    TestFalse(TEXT("BPIR body has no unresolved ' ?' values"),
        Result.BpirText.Contains(TEXT(" ?")));

    // Warnings list must NOT contain the legacy unresolvable-Inputs warning
    bool bHasInputsWarning = false;
    for (const FBpirWarning& Warn : Result.Warnings)
    {
        if (Warn.Text.Contains(TEXT("source node 'Inputs'")))
        {
            bHasInputsWarning = true;
            break;
        }
    }
    TestFalse(TEXT("Warnings do not contain 'source node 'Inputs''"), bHasInputsWarning);
    return true;
}
