// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/BpirShapeMetadata.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Utils/PropertyUtils.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_Event.h"
#include "K2Node_FormatText.h"
#include "UObject/UnrealType.h"

namespace BpirShapeMetadata
{
namespace
{
    // FormatText shape generation requires the Format pin's DefaultTextValue to
    // be set and PinDefaultValueChanged to be invoked so PinNames is rebuilt.
    // The pin is created by AllocateDefaultPins, so this hook runs after pins exist.
    bool FormatTextHook(UK2Node* Node, const TMap<FString, FString>& NodeProps, FString& OutError)
    {
        const FString* FormatStr = NodeProps.Find(TEXT("Format"));
        if (!FormatStr)
        {
            return true;
        }

        UK2Node_FormatText* FormatNode = Cast<UK2Node_FormatText>(Node);
        if (!FormatNode)
        {
            OutError = TEXT("FormatText hook expected UK2Node_FormatText");
            return false;
        }

        UEdGraphPin* FormatPin = FormatNode->FindPin(TEXT("Format"));
        if (!FormatPin)
        {
            OutError = TEXT("FormatText node missing 'Format' pin");
            return false;
        }

        FText NewFormat;
        FString CoerceErr;
        const FText Existing = FormatPin->DefaultTextValue;
        if (!CoerceStringToPersistedFText(*FormatStr, &Existing, NewFormat, CoerceErr))
        {
            OutError = FString::Printf(TEXT("FormatText 'Format' value: %s"), *CoerceErr);
            return false;
        }

        FormatPin->DefaultTextValue = NewFormat;
        FormatPin->DefaultValue.Empty();
        FormatPin->DefaultObject = nullptr;
        FormatNode->PinDefaultValueChanged(FormatPin);
        return true;
    }

    // BaseMCDelegate replay deferred: the audit prescribes
    // SetFromProperty(DelegateProp, bSelfContext, SearchClass) so AllocateDefaultPins
    // sees the resolved FMulticastDelegateProperty plus search-class context that
    // SetFromField caches. Resolving DelegateProp requires the owner UClass, which is
    // not plumbed through node_props today. Until the parser surfaces a search class
    // (or we infer it from the FMemberReference's MemberParent during apply), copy
    // the FMemberReference struct verbatim via ImportText and warn — the node may
    // miss any context SetFromProperty would have configured separately.
    bool BaseMCDelegateHook(UK2Node* Node, const TMap<FString, FString>& NodeProps, FString& OutError)
    {
        if (!Cast<UK2Node_BaseMCDelegate>(Node))
        {
            return true;
        }
        const FString* RefStr = NodeProps.Find(TEXT("DelegateReference"));
        if (!RefStr)
        {
            return true;
        }

        UE_LOG(LogBpirCompiler, Warning,
            TEXT("BaseMCDelegate node_props replay via raw property set may be incorrect; SetFromProperty path not implemented"));

        FProperty* RefProp = Node->GetClass()->FindPropertyByName(TEXT("DelegateReference"));
        if (!RefProp)
        {
            OutError = TEXT("BaseMCDelegate hook: DelegateReference property not found");
            return false;
        }
        FString ImportErr;
        if (!ImportTextToProperty(Node, RefProp, *RefStr, ImportErr))
        {
            OutError = FString::Printf(TEXT("BaseMCDelegate DelegateReference apply: %s"), *ImportErr);
            return false;
        }
        return true;
    }

    // Forward-looking no-op: ProxyFactoryClass / ProxyFactoryFunctionName are protected on
    // UK2Node_BaseAsyncTask, and CreateGenericK2Node rejects UK2Node_AsyncAction outright,
    // so the InitializeProxyFromFunction call this hook would make is unreachable today.
    // Reflective property access via FProperty is the path forward when generic AsyncAction
    // creation is unblocked.
    bool AsyncTaskHook(UK2Node* Node, const TMap<FString, FString>& NodeProps, FString& OutError)
    {
        return true;
    }

    const TMap<UClass*, FBpirShapeDescriptor>& GetRegistry()
    {
        static const TMap<UClass*, FBpirShapeDescriptor> Registry = []
        {
            TMap<UClass*, FBpirShapeDescriptor> R;

            {
                FBpirShapeDescriptor Desc;
                Desc.PreAllocateProperties = { FName(TEXT("EventReference")), FName(TEXT("bOverrideFunction")) };
                R.Add(UK2Node_Event::StaticClass(), MoveTemp(Desc));
            }
            {
                FBpirShapeDescriptor Desc;
                // Hook owns DelegateReference apply (raw set with warning) until a
                // SetFromProperty path with search-class context lands.
                Desc.PreAllocateHook = &BaseMCDelegateHook;
                R.Add(UK2Node_BaseMCDelegate::StaticClass(), MoveTemp(Desc));
            }
            {
                FBpirShapeDescriptor Desc;
                Desc.PreAllocateProperties = { FName(TEXT("FunctionReference")) };
                R.Add(UK2Node_CallFunction::StaticClass(), MoveTemp(Desc));
            }
            {
                FBpirShapeDescriptor Desc;
                Desc.PreAllocateHook = &FormatTextHook;
                // Format is the FormatText 'Format' pin's DefaultTextValue, not a UPROPERTY.
                // The hook applies it; the reflective apply loop must not try to resolve it.
                Desc.HookHandledProperties = { FName(TEXT("Format")) };
                R.Add(UK2Node_FormatText::StaticClass(), MoveTemp(Desc));
            }
            {
                FBpirShapeDescriptor Desc;
                Desc.PreAllocateProperties = {
                    FName(TEXT("ProxyFactoryFunctionName")),
                    FName(TEXT("ProxyFactoryClass")),
                    FName(TEXT("ProxyClass"))
                };
                // Currently dead: CreateGenericK2Node rejects UK2Node_AsyncAction; kept forward-looking for non-AsyncAction BaseAsyncTask subclasses.
                Desc.PreAllocateHook = &AsyncTaskHook;
                R.Add(UK2Node_BaseAsyncTask::StaticClass(), MoveTemp(Desc));
            }
            {
                FBpirShapeDescriptor Desc;
                R.Add(UK2Node_ConstructObjectFromClass::StaticClass(), MoveTemp(Desc));
            }

            return R;
        }();

        return Registry;
    }
}

const FBpirShapeDescriptor* FindBpirShapeDescriptor(UClass* NodeClass)
{
    if (!NodeClass)
    {
        return nullptr;
    }

    const TMap<UClass*, FBpirShapeDescriptor>& Registry = GetRegistry();
    for (UClass* Cur = NodeClass; Cur; Cur = Cur->GetSuperClass())
    {
        if (const FBpirShapeDescriptor* Found = Registry.Find(Cur))
        {
            return Found;
        }
    }
    return nullptr;
}

bool ReplayGenericNodeProps(UK2Node* Node, const TMap<FString, FString>& NodeProps, TArray<FCompileError>& OutErrors, int32 SourceLine)
{
    if (!Node || NodeProps.Num() == 0)
    {
        return true;
    }

    // Practical reality: CodeNodeEmitter::CreateGenericK2Node has already invoked
    // AllocateDefaultPins by the time this runs. The "PreAllocate" name is preserved
    // from the per-class shape audit (B-bpir-compile-property-vs-allocate-pins-ordering)
    // for the conceptual ordering — pre-Allocate properties are applied first, the hook
    // runs next (which may e.g. trigger PinDefaultValueChanged), then ReconstructNode
    // re-runs AllocateDefaultPins so dynamic pins re-derive from the final UPROPERTY state.
    const FBpirShapeDescriptor* Desc = FindBpirShapeDescriptor(Node->GetClass());

    UClass* NodeClass = Node->GetClass();
    bool bAnyApplied = false;

    auto ApplyEntry = [&](const FString& Key, const FString& Value) -> bool
    {
        FProperty* Prop = NodeClass->FindPropertyByName(*Key);
        if (!Prop)
        {
            OutErrors.Add(FCompileError(SourceLine,
                FString::Printf(TEXT("node_props: property '%s' not found on '%s'"), *Key, *NodeClass->GetName())));
            return false;
        }

        TSharedPtr<FJsonValue> JsonVal = CoerceStringToJsonValueByProperty(Value, Prop);
        FString ApplyError;
        if (ApplyJsonValueToProperty(Node, Prop, JsonVal, ApplyError))
        {
            bAnyApplied = true;
            return true;
        }
        if (ImportTextToProperty(Node, Prop, Value, ApplyError))
        {
            bAnyApplied = true;
            return true;
        }
        OutErrors.Add(FCompileError(SourceLine,
            FString::Printf(TEXT("node_props: failed to set '%s' on '%s': %s"),
                *Key, *NodeClass->GetName(), *ApplyError)));
        return false;
    };

    bool bOk = true;

    if (Desc)
    {
        for (const TPair<FString, FString>& Pair : NodeProps)
        {
            if (Desc->PreAllocateProperties.Contains(FName(*Pair.Key)))
            {
                bOk &= ApplyEntry(Pair.Key, Pair.Value);
            }
        }

        if (Desc->PreAllocateHook)
        {
            FString HookError;
            if (!Desc->PreAllocateHook(Node, NodeProps, HookError))
            {
                OutErrors.Add(FCompileError(SourceLine,
                    FString::Printf(TEXT("node_props pre-allocate hook on '%s': %s"),
                        *NodeClass->GetName(), *HookError)));
                bOk = false;
            }
        }
    }

    for (const TPair<FString, FString>& Pair : NodeProps)
    {
        const FName Key(*Pair.Key);
        const bool bIsPre = Desc && Desc->PreAllocateProperties.Contains(Key);
        if (bIsPre)
        {
            continue;
        }
        // HookHandledProperties: the registered PreAllocateHook owns these keys
        // (typically because they map to pin values rather than UPROPERTYs). Skipping
        // here prevents a spurious "property not found" reflective lookup. Without a
        // hook the set has nothing to consume the key, so surface that as an error
        // rather than silently dropping the value.
        if (Desc && Desc->HookHandledProperties.Contains(Key))
        {
            if (!Desc->PreAllocateHook)
            {
                OutErrors.Add(FCompileError(SourceLine,
                    FString::Printf(TEXT("node_props: '%s' marked hook-handled on '%s' but no hook is registered"),
                        *Pair.Key, *NodeClass->GetName())));
                bOk = false;
            }
            continue;
        }
        bOk &= ApplyEntry(Pair.Key, Pair.Value);
    }

    // ReconstructNode re-runs AllocateDefaultPins; needed for any class whose pin
    // shape depends on UPROPERTY state, including subclasses unknown to the registry.
    if (bAnyApplied)
    {
        Node->ReconstructNode();
    }

    return bOk;
}

void RunPostWireHooks(UK2Node* Node)
{
    if (!Node)
    {
        return;
    }

    if (UK2Node_CreateDelegate* CDNode = Cast<UK2Node_CreateDelegate>(Node))
    {
        CDNode->HandleAnyChange();
    }
}

} // namespace BpirShapeMetadata
