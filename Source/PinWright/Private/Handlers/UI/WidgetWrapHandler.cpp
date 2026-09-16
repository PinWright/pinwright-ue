// Copyright (c) 2026 Alexander Penkin. MIT License.


#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Utils/PropertyUtils.h"
#include "Utils/TransactionUtils.h"
#include "ScopedTransaction.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/CanvasPanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Compat/EngineVersionCompat.h"
#include "Compat/JsonKeyCompat.h"

using namespace WidgetAuthoringHelpers;

namespace
{
    // Apply fill-defaults to Target's fresh slot inside the wrapper. For
    // overlay/border/sizebox-like wrappers the caller almost always wants
    // the contained child to stretch to fill the wrapper; this mirrors the
    // choice a human makes in the UMG designer.
    static void ApplyWrapperChildSlotDefaults(UPanelSlot* ChildSlot)
    {
        if (!ChildSlot) return;

        if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(ChildSlot))
        {
            OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
            OverlaySlot->SetVerticalAlignment(VAlign_Fill);
            return;
        }

        // Border and SizeBox both use slot classes that expose the same
        // HAlign/VAlign properties reflectively. Drive them via the property
        // system to avoid hard-coding a cast per slot type.
        FProperty* HAlignProp = FindPropertyCI(ChildSlot->GetClass(), TEXT("HorizontalAlignment"));
        FProperty* VAlignProp = FindPropertyCI(ChildSlot->GetClass(), TEXT("VerticalAlignment"));
        if (HAlignProp)
        {
            FString Err;
            TSharedPtr<FJsonValue> FillValue = MakeShared<FJsonValueString>(TEXT("HAlign_Fill"));
            ApplyJsonValueToProperty(ChildSlot, HAlignProp, FillValue, Err);
        }
        if (VAlignProp)
        {
            FString Err;
            TSharedPtr<FJsonValue> FillValue = MakeShared<FJsonValueString>(TEXT("VAlign_Fill"));
            ApplyJsonValueToProperty(ChildSlot, VAlignProp, FillValue, Err);
        }
    }

    static bool ApplyJsonObjectFields(UObject* Target, const TSharedPtr<FJsonObject>& Fields, FString& OutError)
    {
        if (!Target || !Fields.IsValid()) return true;
        for (const auto& Pair : Fields->Values)
        {
            FProperty* Prop = FindPropertyCI(Target->GetClass(), EARGCompat::JsonKeyToString(Pair.Key));
            if (!Prop)
            {
                OutError = FString::Printf(TEXT("Property not found: %s"), *Pair.Key);
                return false;
            }
            FString ApplyError;
            if (!ApplyJsonValueToProperty(Target, Prop, Pair.Value, ApplyError))
            {
                OutError = FString::Printf(TEXT("Failed to set '%s': %s"), *Pair.Key, *ApplyError);
                return false;
            }
        }
        return true;
    }

    static bool ValidateJsonObjectFields(UClass* TargetClass,
        const TSharedPtr<FJsonObject>& Fields, FString& OutError)
    {
        OutError.Empty();
        if (!TargetClass || !Fields.IsValid() || Fields->Values.Num() == 0 ||
            TargetClass->HasAnyClassFlags(CLASS_Abstract))
        {
            return true;
        }

        UObject* Scratch = NewObject<UObject>(GetTransientPackage(), TargetClass,
            NAME_None, RF_Transient);
        if (!Scratch)
        {
            OutError = FString::Printf(TEXT("Failed to construct a validation object of type '%s'"),
                *TargetClass->GetName());
            return false;
        }

        const bool bValid = ApplyJsonObjectFields(Scratch, Fields, OutError);
        Scratch->MarkAsGarbage();
        return bValid;
    }

    static bool IsWidgetInSubtree(const UWidget* Target, const UWidget* Candidate)
    {
        for (const UWidget* Current = Candidate; Current; Current = Current->GetParent())
        {
            if (Current == Target)
            {
                return true;
            }
        }
        return false;
    }

    static UObject* ResolveObjectReferenceForValidation(const FString& Path)
    {
        UObject* Resolved = LoadObject<UObject>(nullptr, *Path);
        if (!Resolved && !Path.Contains(TEXT(".")))
        {
            Resolved = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
        }
        return Resolved;
    }

    static bool ValidateWrapperSlotReferences(const UWidget* Target,
        const TSharedPtr<FJsonObject>& WrapperProperties, FString& OutError)
    {
        OutError.Empty();
        if (!Target || !WrapperProperties.IsValid())
        {
            return true;
        }

        for (const auto& Pair : WrapperProperties->Values)
        {
            if (!EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("Slots"), ESearchCase::IgnoreCase) ||
                !Pair.Value.IsValid() || Pair.Value->Type != EJson::Array)
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>& Entries = Pair.Value->AsArray();
            for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
            {
                const TSharedPtr<FJsonValue>& Entry = Entries[EntryIndex];
                if (!Entry.IsValid() || Entry->Type == EJson::Null || Entry->Type != EJson::String)
                {
                    continue;
                }

                const FString Path = Entry->AsString();
                if (Path.IsEmpty() || Path.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
                    Path.Equals(TEXT("null"), ESearchCase::IgnoreCase))
                {
                    continue;
                }

                UObject* ReferencedObject = ResolveObjectReferenceForValidation(Path);
                if (!ReferencedObject)
                {
                    continue;
                }

                if (UWidget* ReferencedWidget = Cast<UWidget>(ReferencedObject))
                {
                    if (!IsWidgetInSubtree(Target, ReferencedWidget))
                    {
                        OutError = FString::Printf(
                            TEXT("wrapperProperties.Slots[%d] references widget '%s' outside the wrapped target subtree"),
                            EntryIndex, *ReferencedWidget->GetPathName());
                        return false;
                    }
                    continue;
                }

                if (UPanelSlot* ReferencedSlot = Cast<UPanelSlot>(ReferencedObject))
                {
                    if (ReferencedSlot->Content &&
                        !IsWidgetInSubtree(Target, ReferencedSlot->Content))
                    {
                        OutError = FString::Printf(
                            TEXT("wrapperProperties.Slots[%d] references slot '%s' whose content widget '%s' is outside the wrapped target subtree"),
                            EntryIndex, *ReferencedSlot->GetPathName(),
                            *ReferencedSlot->Content->GetPathName());
                        return false;
                    }
                }
            }
        }
        return true;
    }
}

REGISTER_RPC_HANDLER("widget.wrap", "widget",
    "Wrap an existing non-root widget with a new panel widget, preserving the target's subtree and its slot layout on the grandparent",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath", "path", "Path to the widget blueprint"),
        RPC_PARAM_REQ("targetName", "string", "Name of the widget to wrap"),
        RPC_PARAM_REQ("wrapperType", "classref", "Panel widget class name (e.g. Overlay, Border, SizeBox, VerticalBox)"),
        RPC_PARAM_REQ("wrapperName", "string", "Name for the new wrapper widget"),
        RPC_PARAM_OPT("wrapperProperties", "object", "Key-value properties to apply to the wrapper after construction"),
        RPC_PARAM_OPT("wrapperSlot", "object", "Key-value slot properties to apply to the wrapper's slot on the grandparent (applied after slot preservation)")
    ))
{
    FString WidgetPath, TargetName, WrapperType, WrapperName;
    if (!Ctx.RequireString(TEXT("widgetPath"), WidgetPath)) return true;
    if (!Ctx.RequireString(TEXT("targetName"), TargetName)) return true;
    if (!Ctx.RequireString(TEXT("wrapperType"), WrapperType)) return true;
    if (!Ctx.RequireString(TEXT("wrapperName"), WrapperName)) return true;

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* Target = FindWidgetByName(WidgetBP, TargetName);
    if (!Target)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, FString::Printf(TEXT("Widget not found: %s"), *TargetName));
        return true;
    }

    if (Target == WidgetBP->WidgetTree->RootWidget)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_OPERATION,
            TEXT("Cannot wrap the root widget — use widget.replace_class instead"));
        return true;
    }

    UPanelWidget* OldParent = Target->GetParent();
    if (!OldParent)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_OPERATION,
            TEXT("Target widget has no parent; cannot wrap an unparented widget"));
        return true;
    }

    UClass* WrapperClass = ResolveWidgetClass(WrapperType);
    if (!WrapperClass)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Wrapper class not found: %s"), *WrapperType));
        return true;
    }
    if (!WrapperClass->IsChildOf(UPanelWidget::StaticClass()))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Wrapper class '%s' is not a UPanelWidget subclass"), *WrapperType));
        return true;
    }

    if (WidgetBP->WidgetTree->FindWidget(FName(*WrapperName)))
    {
        Ctx.SendError(ErrorCodes::ERR_DUPLICATE_NAME,
            FString::Printf(TEXT("Widget with name '%s' already exists"), *WrapperName));
        return true;
    }
    FName FinalWrapperName = FName(*WrapperName);

    const int32 OriginalIndex = OldParent->GetChildIndex(Target);
    UPanelSlot* OriginalSlot = Target->Slot;
    if (OriginalIndex == INDEX_NONE || !OriginalSlot)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_OPERATION,
            TEXT("Target widget has no valid parent slot; cannot wrap"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> WrapperProps = GetObjectField(Payload, TEXT("wrapperProperties"));
    TSharedPtr<FJsonObject> WrapperSlotObj = GetObjectField(Payload, TEXT("wrapperSlot"));

    // Validate both optional property batches against scratch instances before
    // constructing the real wrapper or changing the authored hierarchy. The
    // real writes still run below so they retain their normal object ownership.
    FString PreflightError;
    if (!ValidateWrapperSlotReferences(Target, WrapperProps, PreflightError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PROPERTY, PreflightError);
        return true;
    }
    if (!ValidateJsonObjectFields(WrapperClass, WrapperProps, PreflightError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PROPERTY, PreflightError);
        return true;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    UClass* const OldParentSlotClass = OldParent->GetSlotClass();
#else
    // UPanelWidget::GetSlotClass() is protected through UE 5.4. OriginalSlot was created by this
    // same parent's AddChild, so its class IS what GetSlotClass() returns, and it is checked
    // non-null above.
    UClass* const OldParentSlotClass = OriginalSlot->GetClass();
#endif
    if (!ValidateJsonObjectFields(OldParentSlotClass, WrapperSlotObj, PreflightError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PROPERTY, PreflightError);
        return true;
    }

    UPackage* Package = WidgetBP->GetOutermost();
    const bool bPackageWasDirty = Package && Package->IsDirty();

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.wrap")));

    // ReplaceChild/AddChild update several UObject fields directly rather than
    // recording each owner themselves. Snapshot every owner that represents the
    // pre-call tree so a late property or attachment failure can be canceled as
    // one transaction, including the original slot identity.
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(WidgetBP);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(WidgetBP->WidgetTree);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(OldParent);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(Target);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(OriginalSlot);

    auto SendRolledBackError = [&Transaction, &Ctx, WidgetBP, Target, Package, bPackageWasDirty](
        UPanelWidget* WrapperToDiscard, const TCHAR* ErrorCode, const FString& Message) -> bool
    {
        // The wrapper may temporarily own the pre-existing target. Detach both
        // sides before DiscardConstructedWidgetForAuthoring so cleanup cannot
        // recurse into and destroy the caller's original widget.
        if (WrapperToDiscard)
        {
            if (WrapperToDiscard->GetChildIndex(Target) != INDEX_NONE)
            {
                WrapperToDiscard->RemoveChild(Target);
            }
            // WrapperProperties may have populated the reflected Slots array
            // with a pre-existing slot. Clear every remaining wrapper slot
            // before undo restores that slot's original content, otherwise
            // discard cleanup could recurse into the caller's original child.
            // The null entries go first: that same reflected array accepts a
            // JSON null, and ClearChildren removes through RemoveChildAt, which
            // dereferences the entry before null-checking it on UE 5.3.
            RemoveNullPanelSlots(WrapperToDiscard);
            WrapperToDiscard->ClearChildren();
            WrapperToDiscard->RemoveFromParent();
        }

        PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);
        if (WrapperToDiscard)
        {
            DiscardConstructedWidgetForAuthoring(WidgetBP, WrapperToDiscard);
        }
        if (Package && !bPackageWasDirty)
        {
            Package->SetDirtyFlag(false);
        }
        Ctx.SendError(ErrorCode, Message);
        return true;
    };

    UPanelWidget* Wrapper = WidgetBP->WidgetTree->ConstructWidget<UPanelWidget>(WrapperClass, FinalWrapperName);
    if (!Wrapper)
    {
        return SendRolledBackError(nullptr, ErrorCodes::ERR_CREATION_ERROR,
            FString::Printf(TEXT("Failed to construct wrapper of type: %s"), *WrapperType));
    }

    // Apply wrapper properties before rehoming so construction-time state is
    // visible if the slot preservation path reads anything off the wrapper.
    if (WrapperProps.IsValid())
    {
        FString PropError;
        if (!ApplyJsonObjectFields(Wrapper, WrapperProps, PropError))
        {
            return SendRolledBackError(Wrapper, ErrorCodes::ERR_INVALID_PROPERTY, PropError);
        }
    }

    // ReplaceChild rehomes the existing UPanelSlot onto Wrapper — the slot
    // object itself is reused so anchors/offsets/alignment/etc. are preserved
    // automatically. After this call Target->Slot still points at the (now
    // re-targeted) grandparent slot; AddChild below overwrites it with a
    // fresh slot of the wrapper's slot class.
    if (!OldParent->ReplaceChild(Target, Wrapper))
    {
        return SendRolledBackError(Wrapper, ErrorCodes::ERR_INTERNAL_ERROR,
            TEXT("UPanelWidget::ReplaceChild failed to rehome the original slot"));
    }

    UPanelSlot* ChildSlot = Wrapper->AddChild(Target);
    if (!ChildSlot)
    {
        return SendRolledBackError(Wrapper, ErrorCodes::ERR_INTERNAL_ERROR,
            TEXT("Failed to add target to wrapper"));
    }

    ApplyWrapperChildSlotDefaults(ChildSlot);

    // Now that the wrapper lives on the grandparent, apply any caller-supplied
    // slot overrides. This runs AFTER slot preservation so callers can tweak
    // the preserved slot (e.g. move a CanvasPanel position) rather than
    // authoring a fresh slot from scratch.
    if (WrapperSlotObj.IsValid())
    {
        if (!Wrapper->Slot)
        {
            return SendRolledBackError(Wrapper, ErrorCodes::ERR_INTERNAL_ERROR,
                TEXT("Wrapper has no parent slot after attachment"));
        }
        FString SlotError;
        if (!ApplyJsonObjectFields(Wrapper->Slot, WrapperSlotObj, SlotError))
        {
            return SendRolledBackError(Wrapper, ErrorCodes::ERR_INVALID_PROPERTY, SlotError);
        }
    }

    if (OldParent->GetChildAt(OriginalIndex) != Wrapper ||
        Wrapper->GetChildrenCount() != 1 ||
        Wrapper->GetChildAt(0) != Target ||
        Target->GetParent() != Wrapper ||
        Wrapper->Slot != OriginalSlot)
    {
        return SendRolledBackError(Wrapper, ErrorCodes::ERR_INTERNAL_ERROR,
            TEXT("Wrapper hierarchy verification failed"));
    }

    // UE validates every UWidget still outered to WidgetTree during compile,
    // including stale source widgets that are no longer in the visible tree.
    WidgetBP->ForEachSourceWidget([WidgetBP](UWidget* SourceWidget)
    {
        if (SourceWidget)
        {
            EnsureWidgetVariableGuid(WidgetBP, SourceWidget->GetFName());
        }
    });
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("widgetPath"), WidgetPath);
    Result->SetStringField(TEXT("wrapperName"), Wrapper->GetName());
    Result->SetStringField(TEXT("wrapperClass"), WrapperClass->GetName());
    Result->SetStringField(TEXT("grandparentName"), OldParent->GetName());
    Result->SetStringField(TEXT("targetName"), Target->GetName());
    Result->SetBoolField(TEXT("requiresCompile"), true);

    Ctx.SendSuccess(Result);
    return true;
}
