// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetBindingUtils.cpp
// Utility functions for widget binding operations (extracted from WidgetBindingHandler.cpp)

#include "Handlers/UI/WidgetBindingUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace WidgetBindingHelpers
{
    namespace
    {
        // The suffix UMG appends to a property name when resolving a property binding.
        const TCHAR* const PropertyDelegateSuffix = TEXT("Delegate");
    }

    bool IsBindableEventDelegate(const FDelegateProperty* DelegateProperty)
    {
        static const FName IsBindableEventName(TEXT("IsBindableEvent"));
        if (!DelegateProperty)
        {
            return false;
        }
        // Case-insensitive EndsWith, matching the engine predicate this mirrors.
        return DelegateProperty->HasMetaData(IsBindableEventName)
            || DelegateProperty->GetName().EndsWith(TEXT("Event"));
    }

    void CollectBindableNames(const UClass* WidgetClass,
        TArray<FString>& OutPropertyNames, TArray<FString>& OutEventNames)
    {
        OutPropertyNames.Reset();
        OutEventNames.Reset();
        if (!WidgetClass)
        {
            return;
        }

        const int32 SuffixLen = FCString::Strlen(PropertyDelegateSuffix);
        for (TFieldIterator<FProperty> PropIt(WidgetClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
        {
            const FDelegateProperty* DelegateProperty = CastField<FDelegateProperty>(*PropIt);
            if (!DelegateProperty)
            {
                continue;
            }

            const FString DelegateName = DelegateProperty->GetName();
            if (IsBindableEventDelegate(DelegateProperty))
            {
                OutEventNames.AddUnique(DelegateName);
            }
            else if (DelegateName.Len() > SuffixLen && DelegateName.EndsWith(PropertyDelegateSuffix))
            {
                OutPropertyNames.AddUnique(DelegateName.LeftChop(SuffixLen));
            }
        }

        OutPropertyNames.Sort();
        OutEventNames.Sort();
    }

    bool ResolveWidgetBindingTarget(const UClass* WidgetClass, const FString& RequestedName,
        FWidgetBindingTarget& OutTarget)
    {
        OutTarget = FWidgetBindingTarget();
        if (!WidgetClass || RequestedName.IsEmpty())
        {
            return false;
        }

        // Order matters and is UMG's: the "<Name>Delegate" spelling wins over the verbatim one.
        const FString PropertyDelegateName = RequestedName + PropertyDelegateSuffix;
        if (FDelegateProperty* PropertyDelegate =
                FindFProperty<FDelegateProperty>(WidgetClass, *PropertyDelegateName))
        {
            OutTarget.DelegateProperty = PropertyDelegate;
            OutTarget.PropertyName = FName(*RequestedName);
            OutTarget.bIsEvent = false;
            return true;
        }

        FDelegateProperty* EventDelegate = FindFProperty<FDelegateProperty>(WidgetClass, *RequestedName);
        if (EventDelegate && IsBindableEventDelegate(EventDelegate))
        {
            OutTarget.DelegateProperty = EventDelegate;
            OutTarget.PropertyName = FName(*RequestedName);
            OutTarget.bIsEvent = true;
            return true;
        }

        // A property-binding delegate spelled out in full ("TextDelegate") does resolve at runtime,
        // but the details panel matches bindings on the stem, so such a record would be invisible in
        // the editor and would not replace a stem binding on the same slot. Refused; the caller is
        // steered back to the stem, which CollectBindableNames publishes.
        return false;
    }

    UEdGraph* FindFunctionGraphByName(UWidgetBlueprint* WidgetBP, const FString& FunctionName)
    {
        if (!WidgetBP)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : WidgetBP->FunctionGraphs)
        {
            if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    bool EnsureFunctionGraph(
        UWidgetBlueprint* WidgetBP,
        UFunction* DelegateSignature,
        const FString& FunctionName,
        bool bPureFunction,
        bool& bOutCreated,
        FString& OutError)
    {
        bOutCreated = false;
        OutError.Reset();
        if (!WidgetBP)
        {
            OutError = TEXT("Widget blueprint is null");
            return false;
        }
        if (!DelegateSignature)
        {
            OutError = TEXT("Bindable delegate has no signature function");
            return false;
        }

        if (FindFunctionGraphByName(WidgetBP, FunctionName))
        {
            // Leave an author's existing graph alone. Whether it actually satisfies the delegate is
            // decided by FDelegateEditorBinding::IsBindingValid before anything is written.
            return true;
        }

        UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
            WidgetBP,
            FName(*FunctionName),
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        if (!FunctionGraph)
        {
            OutError = TEXT("Failed to create function graph");
            return false;
        }

        // Terminators generated from the delegate's own signature: the entry node receives the
        // delegate's parameters and the result node its return value, which is what
        // IsSignatureCompatibleWith compares against inside IsBindingValid.
        FBlueprintEditorUtils::AddFunctionGraph(
            WidgetBP, FunctionGraph, /*bIsUserCreated=*/true, DelegateSignature);

        if (bPureFunction)
        {
            // A "<Name>Delegate" property binding only validates against a pure handler
            // (bNeedsToBePure in FDelegateEditorBinding::IsBindingValid).
            GetDefault<UEdGraphSchema_K2>()->AddExtraFunctionFlags(FunctionGraph, FUNC_BlueprintPure);
        }

        bOutCreated = true;
        return true;
    }

    FDelegateEditorBinding MakeFunctionBinding(UWidgetBlueprint* WidgetBP, const FString& ObjectName,
        const FName PropertyName, const FString& FunctionName)
    {
        FDelegateEditorBinding Binding;
        Binding.ObjectName = ObjectName;
        Binding.PropertyName = PropertyName;
        Binding.FunctionName = FName(*FunctionName);
        Binding.Kind = EBindingKind::Function;

        if (WidgetBP)
        {
            UBlueprint::GetGuidFromClassByFieldName<UFunction>(
                WidgetBP->SkeletonGeneratedClass, Binding.FunctionName, Binding.MemberGuid);
        }
        return Binding;
    }

    bool UpsertWidgetBinding(
        UWidgetBlueprint* WidgetBP,
        const FDelegateEditorBinding& Binding,
        bool& bOutCreated,
        FString& OutError)
    {
        bOutCreated = false;
        OutError.Reset();
        if (!WidgetBP)
        {
            OutError = TEXT("Widget blueprint is null");
            return false;
        }

        WidgetBP->Modify();

        const int32 ExistingIndex = WidgetBP->Bindings.IndexOfByKey(Binding);
        if (ExistingIndex == INDEX_NONE)
        {
            WidgetBP->Bindings.Add(Binding);
            bOutCreated = true;
        }
        else
        {
            WidgetBP->Bindings[ExistingIndex] = Binding;
        }
        return true;
    }
}
