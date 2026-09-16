// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRPinBindings.h"


#include "AnimGraphNode_Base.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace AGIRPinBindings
{
// Named (not anonymous) so the helpers keep one definition under UBT's unity
// builds, which concatenate the AGIR translation units into a single TU.
namespace Detail
{
const TCHAR* const VariableSigil = TEXT("$");
const TCHAR* const BindPrefix = TEXT("bind ");
const TCHAR* const FunctionMarker = TEXT("fn ");

// Reads the `PropertyBindings` map that holds this node's property-access
// bindings, resolving it reflectively rather than through engine headers:
// UE 5.4 moved the map off the node onto an instanced `UAnimGraphNodeBinding`
// (declared in AnimGraph's `Internal/` folder, its members private) and left a
// `PropertyBindings_DEPRECATED` behind, while UE 5.3 keeps the live map on the
// node itself. Looking both up by name covers every supported engine version
// with one code path and writes only to whichever map is actually live.
FMapProperty* FindBindingMap(UAnimGraphNode_Base* Node, UObject*& OutOwner)
{
    OutOwner = nullptr;
    if (!Node)
    {
        return nullptr;
    }

    if (FObjectProperty* BindingProp =
            FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("Binding")))
    {
        if (UObject* BindingObject = BindingProp->GetObjectPropertyValue_InContainer(Node))
        {
            if (FMapProperty* MapProp =
                    FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings")))
            {
                OutOwner = BindingObject;
                return MapProp;
            }
        }
        // UE 5.4+ with no binding object: the node's own map is the
        // `_DEPRECATED` copy, which the compiler ignores. Fall through and let
        // the exact-name lookup below miss rather than write to a dead map.
    }

    if (FMapProperty* MapProp =
            FindFProperty<FMapProperty>(Node->GetClass(), TEXT("PropertyBindings")))
    {
        OutOwner = Node;
        return MapProp;
    }
    return nullptr;
}

// Guards the reinterpret_casts below: the map must really be
// FName -> FAnimGraphNodePropertyBinding before its raw element memory is read
// or written as that type.
bool IsBindingMapUsable(const FMapProperty* MapProp)
{
    if (!MapProp)
    {
        return false;
    }
    const FNameProperty* KeyProp = CastField<FNameProperty>(MapProp->KeyProp);
    const FStructProperty* ValueProp = CastField<FStructProperty>(MapProp->ValueProp);
    return KeyProp != nullptr
        && ValueProp != nullptr
        && ValueProp->Struct == FAnimGraphNodePropertyBinding::StaticStruct();
}

FString FormatBindingValue(const FAnimGraphNodePropertyBinding& Binding)
{
    const FString Path = FString::Join(Binding.PropertyPath, TEXT("."));
    return (Binding.Type == EAnimGraphNodePropertyBindingType::Function)
        ? FString::Printf(TEXT("%s%s%s"), BindPrefix, FunctionMarker, *Path)
        : FString::Printf(TEXT("%s%s"), BindPrefix, *Path);
}

// True when Node is a plain self-context member getter — the only upstream
// shape `$Name` can express. A getter on another object (its class pin wired)
// or on an external member is a chain AGIR has no spelling for.
bool TryReadVariableGetName(const UEdGraphNode* Node, FString& OutVariableName)
{
    const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node);
    if (!Getter || !Getter->VariableReference.IsSelfContext())
    {
        return false;
    }
    for (const UEdGraphPin* Pin : Getter->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
        {
            return false;
        }
    }
    const FName VariableName = Getter->VariableReference.GetMemberName();
    if (VariableName.IsNone())
    {
        return false;
    }
    OutVariableName = VariableName.ToString();
    return true;
}

bool VariableExistsOnBlueprint(UBlueprint* Blueprint, FName VariableName)
{
    if (!Blueprint)
    {
        return false;
    }
    for (const FBPVariableDescription& Description : Blueprint->NewVariables)
    {
        if (Description.VarName == VariableName)
        {
            return true;
        }
    }
    // A variable declared on a native parent (or already compiled into the
    // skeleton class) is equally bindable and never appears in NewVariables.
    if (Blueprint->SkeletonGeneratedClass &&
        Blueprint->SkeletonGeneratedClass->FindPropertyByName(VariableName))
    {
        return true;
    }
    return Blueprint->GeneratedClass != nullptr
        && Blueprint->GeneratedClass->FindPropertyByName(VariableName) != nullptr;
}

// Returns the node's input pin for PinName, exposing the optional property
// first when the pin is currently hidden. SetPinVisibility (inside
// ToggleOptionalPinExposed) reconstructs the node, so the pin must be
// re-looked-up afterwards rather than cached across the call.
UEdGraphPin* EnsureInputPin(UAnimGraphNode_Base* Node, FName PinName)
{
    if (UEdGraphPin* Existing = Node->FindPin(PinName, EGPD_Input))
    {
        return Existing;
    }
    FString IgnoredError;
    AnimGraphConstructionUtils::ToggleOptionalPinExposed(Node, PinName, true, IgnoredError);
    return Node->FindPin(PinName, EGPD_Input);
}

FString ApplyVariableGetLink(UAnimGraphNode_Base* Node, FName PinName, const FString& VariableName)
{
    if (VariableName.IsEmpty())
    {
        return FString::Printf(TEXT("Empty variable name in '$' binding for pin '%s'"),
            *PinName.ToString());
    }

    UEdGraph* Graph = Node->GetGraph();
    UBlueprint* Blueprint = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
    if (!Graph || !Blueprint)
    {
        return FString::Printf(TEXT("Anim node '%s' is not in a Blueprint graph; cannot bind pin '%s' to $%s"),
            *Node->GetName(), *PinName.ToString(), *VariableName);
    }

    const FName VariableFName(*VariableName);
    if (!VariableExistsOnBlueprint(Blueprint, VariableFName))
    {
        return FString::Printf(TEXT("Variable '%s' bound to pin '%s' does not exist on %s"),
            *VariableName, *PinName.ToString(), *Blueprint->GetName());
    }

    UEdGraphPin* TargetPin = EnsureInputPin(Node, PinName);
    if (!TargetPin)
    {
        return FString::Printf(TEXT("Pin '%s' not found on %s; cannot bind it to $%s"),
            *PinName.ToString(), *Node->GetName(), *VariableName);
    }

    FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
    UK2Node_VariableGet* Getter = Creator.CreateNode(false);
    Getter->VariableReference.SetSelfMember(VariableFName);
    // Park the getter left of its consumer, stepped down by the pin's position
    // so several bindings on one node do not stack on the same spot. Cosmetic
    // only: the AGIR layout engine re-lanes any node left at the origin.
    Getter->NodePosX = Node->NodePosX - 260;
    Getter->NodePosY = Node->NodePosY + 40 * Node->Pins.IndexOfByKey(TargetPin);
    Creator.Finalize();

    UEdGraphPin* SourcePin = nullptr;
    for (UEdGraphPin* Pin : Getter->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output)
        {
            SourcePin = Pin;
            break;
        }
    }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!SourcePin || !Schema || !Schema->TryCreateConnection(SourcePin, TargetPin))
    {
        // Leaving a dangling getter behind would be a silent half-applied
        // binding — the exact failure mode this whole feature exists to remove.
        Graph->RemoveNode(Getter);
        return FString::Printf(TEXT("Could not connect $%s to pin '%s' on %s"),
            *VariableName, *PinName.ToString(), *Node->GetName());
    }
    return FString();
}

FString ApplyPropertyBinding(
    UAnimGraphNode_Base* Node, FName PinName, const FString& Path, bool bIsFunction)
{
    if (Path.IsEmpty())
    {
        return FString::Printf(TEXT("Empty path in 'bind' binding for pin '%s'"),
            *PinName.ToString());
    }

    UObject* Owner = nullptr;
    FMapProperty* MapProp = FindBindingMap(Node, Owner);
    if (!MapProp || !Owner || !IsBindingMapUsable(MapProp))
    {
        return FString::Printf(
            TEXT("Anim node '%s' exposes no property-binding map; 'bind %s' on pin '%s' was not applied"),
            *Node->GetName(), *Path, *PinName.ToString());
    }

    if (!EnsureInputPin(Node, PinName))
    {
        return FString::Printf(TEXT("Pin '%s' not found on %s; cannot bind it to '%s'"),
            *PinName.ToString(), *Node->GetName(), *Path);
    }

    FAnimGraphNodePropertyBinding Binding;
    Binding.PropertyName = PinName;
    Path.ParseIntoArray(Binding.PropertyPath, TEXT("."), /*InCullEmpty=*/true);
    Binding.PathAsText = FText::FromString(Path);
    Binding.Type = bIsFunction
        ? EAnimGraphNodePropertyBindingType::Function
        : EAnimGraphNodePropertyBindingType::Property;
    Binding.bIsBound = true;
    // Array-element pins carry their index in the FName number suffix
    // (`BlendWeights_1` -> number 1), mirroring the engine's own binding widget.
    if (PinName.GetNumber() > 0)
    {
        Binding.ArrayIndex = PinName.GetNumber() - 1;
    }

    Owner->Modify();
    FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(Owner));
    void* ValuePtr = MapHelper.FindOrAdd(&PinName);
    if (!ValuePtr)
    {
        return FString::Printf(TEXT("Could not record binding '%s' for pin '%s' on %s"),
            *Path, *PinName.ToString(), *Node->GetName());
    }
    *static_cast<FAnimGraphNodePropertyBinding*>(ValuePtr) = Binding;

    // PinType / PromotedPinType / bIsPromotion are derived, not authored: the
    // engine recomputes them from PropertyPath in the binding object's
    // OnReconstructNode, which is also what materialises the bound pin.
    Node->ReconstructNode();
    return FString();
}
} // namespace Detail

bool IsBindingValue(const FString& Value)
{
    return Value.StartsWith(Detail::VariableSigil, ESearchCase::CaseSensitive)
        || Value.StartsWith(Detail::BindPrefix, ESearchCase::CaseSensitive);
}

void AppendBindingFields(
    UAnimGraphNode_Base* Node,
    TArray<FString>& OutFields,
    TSet<FName>& OutBoundPins,
    TArray<FString>& OutWarnings)
{
    if (!Node)
    {
        return;
    }

    // Graph-wired pins, in pin declaration order (stable across decompiles).
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input || Pin->bOrphanedPin)
        {
            continue;
        }
        if (UAnimationGraphSchema::IsPosePin(Pin->PinType))
        {
            continue;
        }
        if (Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
        {
            continue;
        }

        const UEdGraphNode* Upstream = Pin->LinkedTo[0]->GetOwningNode();
        FString VariableName;
        if (Detail::TryReadVariableGetName(Upstream, VariableName))
        {
            OutFields.Add(FString::Printf(TEXT("%s: %s%s"),
                *Pin->PinName.ToString(), Detail::VariableSigil, *VariableName));
            OutBoundPins.Add(Pin->PinName);
            continue;
        }

        OutWarnings.Add(FString::Printf(
            TEXT("AGIR_PIN_BINDING_NOT_REPRESENTABLE: node '%s' pin '%s' is driven by '%s'; ")
            TEXT("AGIR can only spell a member-variable getter ($Name) or a property binding, ")
            TEXT("so this link is missing from the text"),
            *Node->GetName(),
            *Pin->PinName.ToString(),
            Upstream ? *Upstream->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("<unknown>")));
    }

    // Property-access bindings, sorted by pin name — TMap iteration order is
    // not stable and this text is diffed.
    UObject* Owner = nullptr;
    FMapProperty* MapProp = Detail::FindBindingMap(Node, Owner);
    if (!MapProp || !Owner || !Detail::IsBindingMapUsable(MapProp))
    {
        return;
    }

    TArray<TPair<FName, FString>> BoundEntries;
    FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(Owner));
    for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
    {
        if (!MapHelper.IsValidIndex(Index))
        {
            continue;
        }
        const FName PinName = *reinterpret_cast<const FName*>(MapHelper.GetKeyPtr(Index));
        const FAnimGraphNodePropertyBinding& Binding =
            *reinterpret_cast<const FAnimGraphNodePropertyBinding*>(MapHelper.GetValuePtr(Index));
        if (!Binding.bIsBound || Binding.PropertyPath.Num() == 0)
        {
            continue;
        }
        BoundEntries.Emplace(PinName, Detail::FormatBindingValue(Binding));
    }

    BoundEntries.Sort([](const TPair<FName, FString>& A, const TPair<FName, FString>& B)
    {
        return A.Key.ToString().Compare(B.Key.ToString(), ESearchCase::CaseSensitive) < 0;
    });

    for (const TPair<FName, FString>& Entry : BoundEntries)
    {
        OutFields.Add(FString::Printf(TEXT("%s: %s"), *Entry.Key.ToString(), *Entry.Value));
        OutBoundPins.Add(Entry.Key);
    }
}

FString ApplyBindingArg(UAnimGraphNode_Base* Node, FName PinName, const FString& Value)
{
    if (!Node)
    {
        return TEXT("Node is null");
    }

    if (Value.StartsWith(Detail::VariableSigil, ESearchCase::CaseSensitive))
    {
        return Detail::ApplyVariableGetLink(
            Node, PinName, Value.RightChop(FCString::Strlen(Detail::VariableSigil)).TrimStartAndEnd());
    }

    FString Path = Value.RightChop(FCString::Strlen(Detail::BindPrefix)).TrimStartAndEnd();
    bool bIsFunction = false;
    if (Path.StartsWith(Detail::FunctionMarker, ESearchCase::CaseSensitive))
    {
        bIsFunction = true;
        Path = Path.RightChop(FCString::Strlen(Detail::FunctionMarker)).TrimStartAndEnd();
    }
    return Detail::ApplyPropertyBinding(Node, PinName, Path, bIsFunction);
}
} // namespace AGIRPinBindings
