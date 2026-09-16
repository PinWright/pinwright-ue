// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unified widget property setter — replaces individual widget.set_* methods.


#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "Utils/PropertyUtils.h"
#include "Utils/TransactionUtils.h"
#include "ScopedTransaction.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"
#include "Compat/JsonKeyCompat.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;

namespace
{
    // Anchor preset name → FAnchors(MinX, MinY, MaxX, MaxY)
    static const TMap<FString, FAnchors> AnchorPresets = {
        {TEXT("TopLeft"),      FAnchors(0,   0,   0,   0)},
        {TEXT("TopCenter"),    FAnchors(0.5, 0,   0.5, 0)},
        {TEXT("TopRight"),     FAnchors(1,   0,   1,   0)},
        {TEXT("CenterLeft"),   FAnchors(0,   0.5, 0,   0.5)},
        {TEXT("Center"),       FAnchors(0.5, 0.5, 0.5, 0.5)},
        {TEXT("CenterRight"),  FAnchors(1,   0.5, 1,   0.5)},
        {TEXT("BottomLeft"),   FAnchors(0,   1,   0,   1)},
        {TEXT("BottomCenter"), FAnchors(0.5, 1,   0.5, 1)},
        {TEXT("BottomRight"),  FAnchors(1,   1,   1,   1)},
        {TEXT("StretchTop"),   FAnchors(0,   0,   1,   0)},
        {TEXT("StretchBottom"),FAnchors(0,   1,   1,   1)},
        {TEXT("StretchLeft"),  FAnchors(0,   0,   0,   1)},
        {TEXT("StretchRight"), FAnchors(1,   0,   1,   1)},
        {TEXT("StretchAll"),   FAnchors(0,   0,   1,   1)},
    };

    bool HasCaseInsensitiveKey(const TSharedPtr<FJsonObject>& Object, const FString& Key)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        for (const auto& Pair : Object->Values)
        {
            if (EARGCompat::JsonKeyToString(Pair.Key).Equals(Key, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }

        return false;
    }

    bool RequestsWidgetPropertyMutation(const TSharedPtr<FJsonObject>& Properties)
    {
        if (!Properties.IsValid())
        {
            return false;
        }

        for (const auto& Pair : Properties->Values)
        {
            if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("isVariable"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("anchor"), ESearchCase::IgnoreCase) &&
                Pair.Value.IsValid() &&
                Pair.Value->Type == EJson::String)
            {
                continue;
            }

            return true;
        }

        return false;
    }

    bool RequestsAnchorMutation(const TSharedPtr<FJsonObject>& Properties)
    {
        if (!Properties.IsValid())
        {
            return false;
        }

        for (const auto& Pair : Properties->Values)
        {
            if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("anchor"), ESearchCase::IgnoreCase) &&
                Pair.Value.IsValid() &&
                Pair.Value->Type == EJson::String)
            {
                return true;
            }
        }

        return false;
    }
}

REGISTER_RPC_HANDLER("widget.set", "widget",
    "Set properties and slot properties on a widget by name",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("widgetName", "string", "Name of the target widget (canonical; aliases: name, slotName, widget_name, targetName)"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("properties", "object", "Key-value pairs of widget property names to values"),
        RPC_PARAM_OPT("slot", "integer", "Key-value pairs of slot property names to values")
    ))
{
    FString WidgetPath;
    WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }
    FString WidgetName = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("name"), TEXT("slotName"), TEXT("widget_name"), TEXT("targetName")});
    if (WidgetName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter 'widgetName' (also accepts: name, slotName, widget_name, targetName)"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> Properties = GetObjectField(Payload, TEXT("properties"));
    TSharedPtr<FJsonObject> SlotObj = GetObjectField(Payload, TEXT("slot"));
    const bool bHasProperties = Properties.IsValid() && Properties->Values.Num() > 0;
    const bool bHasSlotProperties = SlotObj.IsValid() && SlotObj->Values.Num() > 0;

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* Widget = FindWidgetByName(WidgetBP, WidgetName);
    if (!Widget)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget not found: ") + WidgetName);
        return true;
    }

    if (!bHasProperties && !bHasSlotProperties)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetNumberField(TEXT("propertiesSet"), 0);
        Ctx.SendSuccess(Result);
        return true;
    }

    int32 SetCount = 0;
    const bool bUpdatesVariableMetadata = HasCaseInsensitiveKey(Properties, TEXT("isVariable"));
    const bool bUpdatesAnchor = RequestsAnchorMutation(Properties);
    const bool bUpdatesWidgetProperties = RequestsWidgetPropertyMutation(Properties);

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.set")));
    if (bUpdatesVariableMetadata)
    {
        PinWrightTransactionUtils::PrepareTransactionalSnapshot(WidgetBP);
    }
    if (bUpdatesWidgetProperties)
    {
        PinWrightTransactionUtils::PrepareTransactionalSnapshot(Widget);
    }
    if (bUpdatesAnchor || bHasSlotProperties)
    {
        PinWrightTransactionUtils::PrepareTransactionalSnapshot(Widget->Slot);
    }

    auto SendRolledBackError = [&Transaction, &Ctx](const FString& ErrorCode, const FString& Message) -> bool
    {
        PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);
        Ctx.SendError(ErrorCode, Message);
        return true;
    };

    if (Properties.IsValid())
    {
        for (const auto& Pair : Properties->Values)
        {
            const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
            const TSharedPtr<FJsonValue>& Value = Pair.Value;

            if (Key.Equals(TEXT("anchor"), ESearchCase::IgnoreCase) &&
                Value->Type == EJson::String)
            {
                UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
                if (!CanvasSlot)
                {
                    return SendRolledBackError(
                        TEXT("INVALID_PROPERTY"),
                        TEXT("Widget is not in a CanvasPanel; cannot set anchor"));
                }
                const FAnchors* Found = nullptr;
                for (const auto& Preset : AnchorPresets)
                {
                    if (Preset.Key.Equals(Value->AsString(), ESearchCase::IgnoreCase))
                    {
                        Found = &Preset.Value;
                        break;
                    }
                }
                if (!Found)
                {
                    return SendRolledBackError(
                        TEXT("INVALID_PROPERTY"),
                        TEXT("Unknown anchor preset: ") + Value->AsString());
                }
                CanvasSlot->SetAnchors(*Found);
                ++SetCount;
                continue;
            }

            if (Key.Equals(TEXT("isVariable"), ESearchCase::IgnoreCase))
            {
                bool bSetVar = false;
                if (Value->Type == EJson::Boolean)
                    bSetVar = Value->AsBool();
                else if (Value->Type == EJson::String)
                    bSetVar = Value->AsString().Equals(TEXT("true"), ESearchCase::IgnoreCase);
                else
                {
                    return SendRolledBackError(
                        TEXT("INVALID_PROPERTY"),
                        TEXT("isVariable must be a boolean or string \"true\"/\"false\""));
                }

                if (bSetVar)
                    EnsureWidgetVariableGuid(WidgetBP, Widget->GetFName());
                else
                    RemoveWidgetVariableGuid(WidgetBP, Widget->GetFName());
                ++SetCount;
                continue;
            }

            if (Key.Contains(TEXT(".")))
            {
                FString PropError;
                void* Container = nullptr;
                FProperty* Leaf = ResolveNestedPropertyPath(Widget, Key, Container, PropError);
                if (!Leaf)
                {
                    return SendRolledBackError(
                        TEXT("INVALID_PROPERTY"),
                        FString::Printf(TEXT("Cannot resolve '%s': %s"), *Key, *PropError));
                }
                FString ApplyError;
                if (!ApplyJsonValueToProperty(Container, Leaf, Value, ApplyError))
                {
                    return SendRolledBackError(
                        TEXT("INVALID_PROPERTY"),
                        FString::Printf(TEXT("Failed to set '%s': %s"), *Key, *ApplyError));
                }
                ++SetCount;
                continue;
            }

            FProperty* Prop = FindPropertyCI(Widget->GetClass(), Key);
            if (!Prop)
            {
                return SendRolledBackError(
                    TEXT("INVALID_PROPERTY"),
                    TEXT("Property not found: ") + Key);
            }
            FString ApplyError;
            if (!ApplyJsonValueToProperty(Widget, Prop, Value, ApplyError))
            {
                return SendRolledBackError(
                    TEXT("INVALID_PROPERTY"),
                    FString::Printf(TEXT("Failed to set '%s': %s"), *Key, *ApplyError));
            }
            ++SetCount;
        }
    }

    if (SlotObj.IsValid() && !Widget->Slot)
    {
        return SendRolledBackError(
            TEXT("INVALID_OPERATION"),
            TEXT("Widget has no slot (root widget or unparented)"));
    }
    if (SlotObj.IsValid() && Widget->Slot)
    {
        UPanelSlot* Slot = Widget->Slot;
        for (const auto& Pair : SlotObj->Values)
        {
            FProperty* Prop = FindPropertyCI(Slot->GetClass(), EARGCompat::JsonKeyToString(Pair.Key));
            if (!Prop)
            {
                return SendRolledBackError(
                    TEXT("INVALID_PROPERTY"),
                    TEXT("Slot property not found: ") + EARGCompat::JsonKeyToString(Pair.Key));
            }
            FString ApplyError;
            if (!ApplyJsonValueToProperty(Slot, Prop, Pair.Value, ApplyError))
            {
                return SendRolledBackError(
                    TEXT("INVALID_PROPERTY"),
                    FString::Printf(TEXT("Failed to set slot '%s': %s"), *Pair.Key, *ApplyError));
            }
            ++SetCount;
        }
    }

    if (SetCount > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetNumberField(TEXT("propertiesSet"), SetCount);
    Ctx.SendSuccess(Result);
    return true;
}
