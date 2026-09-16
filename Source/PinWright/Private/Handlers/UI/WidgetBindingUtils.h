// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetBindingUtils.h
// Utility functions for widget binding operations (extracted from WidgetBindingHandler.cpp)

#pragma once
#include "CoreMinimal.h"
#include "WidgetBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"

namespace WidgetBindingHelpers
{
    // What a caller-supplied propertyName resolved to, and how UMG reached it.
    struct FWidgetBindingTarget
    {
        // The delegate UMG will look up. Never null after a successful resolve.
        FDelegateProperty* DelegateProperty = nullptr;

        // Value written to FDelegateEditorBinding::PropertyName: the stem for a property binding
        // ("Text" for UTextBlock::TextDelegate), the delegate's own name for a bindable event.
        FName PropertyName;

        // True when the name resolved verbatim to a bindable event delegate. False means the
        // "<Name>Delegate" property-binding spelling, which additionally requires a pure handler.
        bool bIsEvent = false;
    };

    // Mirrors FBlueprintWidgetCustomization::IsBindableEvent (UMGDetailCustomizations.cpp): a
    // single-cast delegate carrying meta=(IsBindableEvent) or whose name ends in "Event".
    // Both clauses are needed - UMenuAnchor::OnGetUserMenuContentEvent has no meta, and
    // UComboBoxKey::OnGenerateContentWidget has the meta but no suffix.
    bool IsBindableEventDelegate(const FDelegateProperty* DelegateProperty);

    // Every propertyName widget.bind can accept on WidgetClass. Property names are the stems UMG
    // appends "Delegate" to; event names are the delegate names verbatim. Used to name what WOULD
    // have resolved when a requested name does not.
    void CollectBindableNames(const UClass* WidgetClass,
        TArray<FString>& OutPropertyNames, TArray<FString>& OutEventNames);

    // Resolves RequestedName the way UMG does - "<Name>Delegate" first, then "<Name>" verbatim
    // (UWidgetBlueprintGeneratedClass::InitializeBindingsStatic and
    // FDelegateEditorBinding::IsBindingValid both perform exactly these two lookups, in this order).
    // Returns false when neither lands on a delegate this verb can bind.
    bool ResolveWidgetBindingTarget(const UClass* WidgetClass, const FString& RequestedName,
        FWidgetBindingTarget& OutTarget);

    UEdGraph* FindFunctionGraphByName(UWidgetBlueprint* WidgetBP, const FString& FunctionName);

    // Creates FunctionName as a graph whose entry/result terminators are generated from
    // DelegateSignature, so the handler is signature-compatible with the delegate it will be bound
    // to; bPureFunction additionally flags it FUNC_BlueprintPure. Same construction the UMG details
    // panel performs in SPropertyBinding::HandleCreateAndAddBinding. An existing graph of that name
    // is left untouched - the caller verifies its compatibility before writing any binding.
    bool EnsureFunctionGraph(
        UWidgetBlueprint* WidgetBP,
        UFunction* DelegateSignature,
        const FString& FunctionName,
        bool bPureFunction,
        bool& bOutCreated,
        FString& OutError);

    // Builds the editor-side binding record for a resolved target. Kind is Function (the struct
    // default is Property, which mislabels every binding this verb writes), and MemberGuid lets a
    // later rename of the handler follow the binding. A missing guid is not a failure:
    // FDelegateEditorBinding::ToRuntimeBinding falls back to FunctionName.
    FDelegateEditorBinding MakeFunctionBinding(UWidgetBlueprint* WidgetBP, const FString& ObjectName,
        FName PropertyName, const FString& FunctionName);

    // Adds Binding, or replaces the one already occupying its (ObjectName, PropertyName) slot -
    // FDelegateEditorBinding::operator== compares only that pair, and UMG reaches at most one
    // binding per slot.
    bool UpsertWidgetBinding(
        UWidgetBlueprint* WidgetBP,
        const FDelegateEditorBinding& Binding,
        bool& bOutCreated,
        FString& OutError);
}
