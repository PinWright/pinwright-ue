// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetXmlImportHandler.cpp
// Imports XML/HTML-like markup to build or replace a widget tree in a widget blueprint.


#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "Handlers/UI/WidgetXmlUtils.h"
#include "Utils/PropertyUtils.h"
#include "XmlFile.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;
using namespace WidgetXmlHelpers;

namespace
{

// ---- Binding struct for deferred binding application ----
struct FPendingBinding
{
    FString WidgetName;
    FString PropertyName;
    FString FunctionName;
};

// ---- Widget class resolution with _C fallback for Blueprint generated classes ----
UClass* ResolveWidgetClassFromTag(const FString& TagName)
{
    UClass* Class = ResolveWidgetClass(TagName);
    if (Class)
        return Class;

    // Try Blueprint generated class suffix — in case the tag uses the raw BP name
    // without the _C suffix that the generated class carries.
    Class = ResolveWidgetClass(TagName + TEXT("_C"));
    return Class;
}

// ---- Clear entire widget tree ----
void ClearWidgetTree(UWidgetBlueprint* WidgetBP)
{
    if (!WidgetBP || !WidgetBP->WidgetTree)
        return;

    WidgetBP->WidgetTree->SetFlags(RF_Transactional);
    WidgetBP->WidgetTree->Modify();

    TArray<UWidget*> AllWidgets;
    WidgetBP->WidgetTree->ForEachWidget([&AllWidgets](UWidget* W)
    {
        AllWidgets.Add(W);
    });

    WidgetBP->WidgetTree->RootWidget = nullptr;

    for (UWidget* W : AllWidgets)
    {
        WidgetBP->WidgetTree->RemoveWidget(W);
    }
}

// ---- Remove a widget subtree (widget + all descendants) ----
void RemoveWidgetSubtree(UWidgetBlueprint* WidgetBP, UWidget* Widget)
{
    if (!WidgetBP || !WidgetBP->WidgetTree || !Widget)
        return;

    // Collect all descendants first (bottom-up removal)
    TArray<UWidget*> Subtree;
    TFunction<void(UWidget*)> CollectSubtree = [&](UWidget* W)
    {
        if (!W) return;
        UPanelWidget* Panel = Cast<UPanelWidget>(W);
        if (Panel)
        {
            for (int32 i = Panel->GetChildrenCount() - 1; i >= 0; --i)
            {
                CollectSubtree(Panel->GetChildAt(i));
            }
        }
        Subtree.Add(W);
    };
    CollectSubtree(Widget);

    // Detach from parent
    WidgetBP->WidgetTree->SetFlags(RF_Transactional);
    WidgetBP->WidgetTree->Modify();
    if (Widget == WidgetBP->WidgetTree->RootWidget)
    {
        WidgetBP->WidgetTree->RootWidget = nullptr;
    }
    else if (UPanelWidget* Parent = Widget->GetParent())
    {
        Parent->SetFlags(RF_Transactional);
        Parent->Modify();
        Parent->RemoveChild(Widget);
    }

    // Remove all widgets in subtree
    for (UWidget* W : Subtree)
    {
        WidgetBP->WidgetTree->RemoveWidget(W);
    }
}

// ---- Validation pass: walk XML tree, resolve all classes, check names ----
bool ValidateXmlNode(const FXmlNode* Node, UWidgetBlueprint* WidgetBP,
    TSet<FString>& UsedNames, FString& OutError)
{
    if (!Node)
    {
        OutError = TEXT("Null XML node encountered");
        return false;
    }

    FString TagName = Node->GetTag();

    // Prefer original-name attribute when present — the exporter emits it when the
    // tag was sanitized (e.g. digit-prefixed class name). Falls back to the literal
    // tag for files written before this fix.
    const FString OriginalName = Node->GetAttribute(TEXT("original-name"));
    const FString ClassLookupName = OriginalName.IsEmpty() ? TagName : OriginalName;

    UClass* WidgetClass = ResolveWidgetClassFromTag(ClassLookupName);
    if (!WidgetClass)
    {
        OutError = FString::Printf(TEXT("Cannot resolve widget class for tag '%s'"), *ClassLookupName);
        return false;
    }

    if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        OutError = FString::Printf(TEXT("Class '%s' (from tag '%s') is not a UWidget subclass"),
            *WidgetClass->GetName(), *TagName);
        return false;
    }

    if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
    {
        OutError = FString::Printf(TEXT("Cannot instantiate abstract class '%s' (from tag '%s'). Use a concrete subclass instead"),
            *WidgetClass->GetName(), *TagName);
        return false;
    }

    FString Name = Node->GetAttribute(TEXT("name"));
    if (Name.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Widget element '%s' is missing required name attribute"), *TagName);
        return false;
    }

    // Check for duplicate names
    if (UsedNames.Contains(Name))
    {
        OutError = FString::Printf(TEXT("Duplicate widget name '%s'"), *Name);
        return false;
    }
    UsedNames.Add(Name);

    // Check children: non-panel widgets cannot have child elements
    const TArray<FXmlNode*>& Children = Node->GetChildrenNodes();
    if (Children.Num() > 0 && !WidgetClass->IsChildOf(UPanelWidget::StaticClass()))
    {
        OutError = FString::Printf(TEXT("Widget '%s' (class '%s') is not a panel and cannot have children"),
            *Name, *WidgetClass->GetName());
        return false;
    }

    // Recurse into children
    for (const FXmlNode* Child : Children)
    {
        if (!ValidateXmlNode(Child, WidgetBP, UsedNames, OutError))
            return false;
    }

    return true;
}

// ---- Apply a single attribute value to a widget or slot (case-insensitive property resolution) ----
bool ApplyAttributeToObject(void* Target, UStruct* TargetStruct,
    const FString& AttrName, const FString& Value, FString& OutError)
{
    // FXmlFile decodes XML entities in attribute values, so Value is already unescaped.

    if (AttrName.Contains(TEXT(".")))
    {
        // Nested property path — resolve segment by segment with case-insensitive lookup
        TArray<FString> Segments;
        AttrName.ParseIntoArray(Segments, TEXT("."));

        UStruct* CurrentStruct = TargetStruct;
        void* CurrentContainer = Target;
        FProperty* CurrentProp = nullptr;

        for (int32 i = 0; i < Segments.Num(); ++i)
        {
            CurrentProp = FindPropertyCI(CurrentStruct, Segments[i]);
            if (!CurrentProp)
            {
                // Defensive: attribute names were sanitized on export (_→space).
                // Retry with underscores replaced by spaces so legacy FNames that
                // truly contained spaces can still round-trip.
                FString SpaceVariant = Segments[i].Replace(TEXT("_"), TEXT(" "));
                if (SpaceVariant != Segments[i])
                    CurrentProp = FindPropertyCI(CurrentStruct, SpaceVariant);
            }
            if (!CurrentProp)
            {
                OutError = FString::Printf(TEXT("Property segment '%s' not found on %s"),
                    *Segments[i], *CurrentStruct->GetName());
                return false;
            }

            if (i < Segments.Num() - 1)
            {
                // Not the final segment — must be a struct to traverse
                FStructProperty* StructProp = CastField<FStructProperty>(CurrentProp);
                if (!StructProp)
                {
                    OutError = FString::Printf(TEXT("'%s' is not a struct, cannot traverse into '%s'"),
                        *Segments[i], *Segments[i + 1]);
                    return false;
                }
                CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
                CurrentStruct = StructProp->Struct;
            }
        }

        // CurrentProp is the final property, CurrentContainer is its owner
        TSharedPtr<FJsonValue> JsonVal = CoerceStringToJsonValueByProperty(Value, CurrentProp);
        FString ApplyError;
        if (ApplyJsonValueToProperty(CurrentContainer, CurrentProp, JsonVal, ApplyError))
            return true;
        if (ImportTextToProperty(CurrentContainer, CurrentProp, Value, ApplyError))
            return true;
        OutError = ApplyError;
        return false;
    }

    // Simple property name
    FProperty* Prop = FindPropertyCI(TargetStruct, AttrName);
    if (!Prop)
    {
        // Defensive: attribute names were sanitized on export (_→space).
        // Retry with underscores replaced by spaces so legacy FNames that
        // truly contained spaces can still round-trip.
        FString SpaceVariant = AttrName.Replace(TEXT("_"), TEXT(" "));
        if (SpaceVariant != AttrName)
            Prop = FindPropertyCI(TargetStruct, SpaceVariant);
    }
    if (!Prop)
    {
        OutError = FString::Printf(TEXT("Property '%s' not found on %s"),
            *AttrName, *TargetStruct->GetName());
        return false;
    }

    TSharedPtr<FJsonValue> JsonVal = CoerceStringToJsonValueByProperty(Value, Prop);
    FString ApplyError;
    if (ApplyJsonValueToProperty(Target, Prop, JsonVal, ApplyError))
        return true;
    if (ImportTextToProperty(Target, Prop, Value, ApplyError))
        return true;
    OutError = ApplyError;
    return false;
}

// ---- Construction pass: build widgets from XML nodes ----
bool ProcessXmlNode(const FXmlNode* Node, UWidgetBlueprint* WidgetBP,
    UPanelWidget* Parent, TSet<FString>& UsedNames,
    TArray<FPendingBinding>& PendingBindings,
    int32& WidgetCount, FString& OutError,
    TArray<FString>& OutVariableWidgets,
    int32 InsertIndex = INDEX_NONE)
{
    if (!Node)
    {
        OutError = TEXT("Null XML node");
        return false;
    }

    // Resolve class (already validated)
    FString TagName = Node->GetTag();

    // Prefer original-name attribute when present — the exporter emits it when the
    // tag was sanitized. Falls back to the literal tag for older files.
    const FString OriginalName = Node->GetAttribute(TEXT("original-name"));
    const FString ClassLookupName = OriginalName.IsEmpty() ? TagName : OriginalName;

    UClass* WidgetClass = ResolveWidgetClassFromTag(ClassLookupName);
    if (!WidgetClass)
    {
        OutError = FString::Printf(TEXT("Failed to resolve class for tag '%s' during construction"), *ClassLookupName);
        return false;
    }

    if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
    {
        OutError = FString::Printf(TEXT("Cannot instantiate abstract class '%s' (from tag '%s')"), *WidgetClass->GetName(), *TagName);
        return false;
    }

    FString Name = Node->GetAttribute(TEXT("name"));
    if (Name.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Widget element '%s' is missing required name attribute"), *TagName);
        return false;
    }
    UsedNames.Add(Name);

    // Construct widget
    FString ConstructionError;
    UWidget* Widget = ConstructWidgetForAuthoring(WidgetBP, WidgetClass, FName(*Name), &ConstructionError);
    if (!Widget)
    {
        OutError = ConstructionError.IsEmpty()
            ? FString::Printf(TEXT("Failed to construct widget '%s' of class '%s'"),
                *Name, *WidgetClass->GetName())
            : ConstructionError;
        return false;
    }

    // Attach to parent or set as root
    if (!Parent)
    {
        WidgetBP->WidgetTree->SetFlags(RF_Transactional);
        WidgetBP->WidgetTree->Modify();
        WidgetBP->WidgetTree->RootWidget = Widget;
    }
    else
    {
        Parent->SetFlags(RF_Transactional);
        Parent->Modify();
        UPanelSlot* Slot = InsertIndex == INDEX_NONE
            ? Parent->AddChild(Widget)
            : Parent->InsertChildAt(InsertIndex, Widget);
        if (!Slot)
        {
            DiscardConstructedWidgetForAuthoring(WidgetBP, Widget);
            OutError = FString::Printf(TEXT("Failed to add widget '%s' to parent '%s'"),
                *Name, *Parent->GetName());
            return false;
        }
    }

    // Process attributes
    const TArray<FXmlAttribute>& Attributes = Node->GetAttributes();
    for (const FXmlAttribute& Attr : Attributes)
    {
        FString AttrName = Attr.GetTag();
        FString AttrValue = Attr.GetValue();

        // Skip "name" — already handled
        if (AttrName.Equals(TEXT("name"), ESearchCase::IgnoreCase))
            continue;

        // Skip IsVariable — handled separately after widget creation
        if (AttrName.Equals(TEXT("IsVariable"), ESearchCase::IgnoreCase))
            continue;

        // Skip Geom.* attributes — these are diagnostic/read-only outputs from the exporter
        // (Geom.source, Geom.status, Geom.abs_x, Geom.summary_*, etc.) and do not map to
        // widget properties. They appear on exports even when geometry resolution is disabled
        // (the root always gets Geom.source="off"), so the import must silently skip them to
        // support export -> import round-trips.
        if (AttrName.StartsWith(TEXT("Geom."), ESearchCase::IgnoreCase))
            continue;

        // Skip original-name — it is a sanitization metadata attribute emitted by the
        // exporter for round-trip class resolution; it does not map to a widget property.
        if (AttrName.Equals(TEXT("original-name"), ESearchCase::IgnoreCase))
            continue;

        // Skip the bare "Slot" attribute. The exporter emits the widget's own
        // UPanelSlot* via instanced-subobject recursion as a flattened struct-text
        // blob (e.g. "Slot={Parent=...,bIsEnabledDelegate={...},...}"), which is
        // not a valid path or import payload. The slot's structural identity is
        // already reconstructed by the parent's AddChild() call above, and its
        // authored layout properties (Padding, HorizontalAlignment, etc.) come in
        // separately under the "slot.*" prefix. Re-applying this raw blob would
        // route to the FObjectProperty branch of ApplyJsonValueToProperty and
        // assert inside FName when the string exceeds NAME_SIZE.
        if (AttrName.Equals(TEXT("Slot"), ESearchCase::IgnoreCase))
            continue;

        // FXmlFile decodes XML entities in attribute values, so no manual unescaping needed.

        // Handle bind.* prefix
        if (AttrName.StartsWith(TEXT("bind."), ESearchCase::IgnoreCase))
        {
            FString PropName = AttrName.RightChop(5);
            PendingBindings.Add(FPendingBinding{Name, PropName, AttrValue});
            continue;
        }

        // Handle slot.* prefix (case-insensitive for robustness)
        if (AttrName.StartsWith(TEXT("slot."), ESearchCase::IgnoreCase))
        {
            FString SlotPropPath = AttrName.RightChop(5); // remove "slot." or "Slot."

            // Skip UPanelSlot::Parent and UPanelSlot::Content — these are derived
            // back-references that the exporter emits for human-readable baseline
            // visibility, but the live parent/child relationship is reconstructed
            // from the XML element hierarchy by AddChild() above. Deserializing
            // them here would clobber the correct back-pointer Slot->Parent that
            // AddChild just set, leading to UPanelWidget::GetChildIndex returning
            // INDEX_NONE and a crash when RemoveChild later indexes into Slots.
            // The slot's structural identity is hierarchy-implied; only authored
            // slot layout properties (Padding, HorizontalAlignment, Size, etc.)
            // should be applied here. Match the top-level segment case-insensitively.
            const FString SlotPropHead = SlotPropPath.Contains(TEXT("."))
                ? SlotPropPath.Left(SlotPropPath.Find(TEXT(".")))
                : SlotPropPath;
            if (SlotPropHead.Equals(TEXT("Parent"), ESearchCase::IgnoreCase) ||
                SlotPropHead.Equals(TEXT("Content"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            UPanelSlot* Slot = Widget->Slot;
            if (!Slot)
            {
                OutError = FString::Printf(
                    TEXT("Cannot set slot property '%s' on widget '%s' — widget has no slot (root widget)"),
                    *SlotPropPath, *Name);
                return false;
            }

            FString SlotError;
            if (!ApplyAttributeToObject(Slot, Slot->GetClass(), SlotPropPath, AttrValue, SlotError))
            {
                OutError = FString::Printf(TEXT("Failed to set slot.%s on widget '%s': %s"),
                    *SlotPropPath, *Name, *SlotError);
                return false;
            }
            continue;
        }

        // Regular widget attribute
        FString PropError;
        if (!ApplyAttributeToObject(Widget, Widget->GetClass(), AttrName, AttrValue, PropError))
        {
            OutError = FString::Printf(TEXT("Failed to set property '%s' on widget '%s': %s"),
                *AttrName, *Name, *PropError);
            return false;
        }
    }

    // Register widget variable GUID (conditional on IsVariable attribute)
    {
        FString IsVarAttr = Node->GetAttribute(TEXT("IsVariable"));
        // Missing attribute (empty string) = treat as variable (backward compat)
        bool bShouldBeVariable = !IsVarAttr.Equals(TEXT("false"), ESearchCase::IgnoreCase);
        if (bShouldBeVariable)
        {
            Widget->bIsVariable = true;
            EnsureWidgetVariableGuid(WidgetBP, Widget->GetFName());
            OutVariableWidgets.Add(Name);
        }
        else
        {
            Widget->bIsVariable = false;
        }
    }

    ++WidgetCount;

    // Recurse into children if this is a panel widget
    UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget);
    if (PanelWidget)
    {
        for (const FXmlNode* ChildNode : Node->GetChildrenNodes())
        {
            if (!ProcessXmlNode(ChildNode, WidgetBP, PanelWidget, UsedNames, PendingBindings, WidgetCount, OutError, OutVariableWidgets))
                return false;
        }
    }

    return true;
}

// ---- Apply pending bindings to WidgetBP via reflection ----
int32 ApplyBindings(UWidgetBlueprint* WidgetBP, const TArray<FPendingBinding>& Bindings, FString& OutError)
{
    if (Bindings.IsEmpty())
        return 0;

    FArrayProperty* BindingsArrayProp = FindFProperty<FArrayProperty>(
        WidgetBP->GetClass(), TEXT("Bindings"));
    if (!BindingsArrayProp)
    {
        OutError = TEXT("Cannot find Bindings array on widget blueprint");
        return -1;
    }

    FStructProperty* BindingStructProp = CastField<FStructProperty>(BindingsArrayProp->Inner);
    if (!BindingStructProp)
    {
        OutError = TEXT("Bindings array inner is not a struct");
        return -1;
    }

    UScriptStruct* BindingStruct = BindingStructProp->Struct;
    void* ArrayPtr = BindingsArrayProp->ContainerPtrToValuePtr<void>(WidgetBP);
    FScriptArrayHelper ArrayHelper(BindingsArrayProp, ArrayPtr);

    int32 Count = 0;
    for (const FPendingBinding& PB : Bindings)
    {
        int32 NewIdx = ArrayHelper.AddValue();
        void* EntryPtr = ArrayHelper.GetRawPtr(NewIdx);
        BindingStruct->InitializeStruct(EntryPtr);

        // Helper lambda to set a name/string field on the binding struct
        auto SetField = [&](const TCHAR* FieldName, const FString& Value)
        {
            FProperty* FieldProp = BindingStruct->FindPropertyByName(FieldName);
            if (!FieldProp) return;

            if (FNameProperty* NP = CastField<FNameProperty>(FieldProp))
            {
                NP->SetPropertyValue_InContainer(EntryPtr, FName(*Value));
            }
            else if (FStrProperty* SP = CastField<FStrProperty>(FieldProp))
            {
                SP->SetPropertyValue_InContainer(EntryPtr, Value);
            }
        };

        SetField(TEXT("ObjectName"), PB.WidgetName);
        SetField(TEXT("PropertyName"), PB.PropertyName);
        SetField(TEXT("FunctionName"), PB.FunctionName);
        ++Count;
    }
    return Count;
}

} // anonymous namespace

// ---- widget.import_xml ----
REGISTER_RPC_HANDLER("widget.import_xml", "widget",
    "Import XML markup to build or replace a widget tree",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("xml", "string", "XML markup string defining the widget tree"),
        RPC_PARAM_OPT("mode", "string", "Import mode: 'replace' (default) or 'add'"),
        RPC_PARAM_OPT("widgetName", "string", "Widget to replace (subtree) or parent to add under (canonical; aliases: name, slotName, widget_name, targetName, target_name)"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("target_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName")
    ))
{
    // ---- 1. Parse parameters ----
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    FString XmlString;
    if (!Ctx.RequireString(TEXT("xml"), XmlString))
        return true;

    FString Mode = Ctx.GetString(TEXT("mode"), TEXT("replace"));
    Mode = Mode.ToLower();
    if (!Mode.Equals(TEXT("replace")) && !Mode.Equals(TEXT("add")))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), FString::Printf(
            TEXT("Invalid mode '%s'. Must be 'replace' or 'add'"), *Mode));
        return true;
    }

    FString TargetName = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("targetName"), TEXT("target_name"), TEXT("widget_name"), TEXT("name"), TEXT("slotName")});

    // ---- 2. Load widget blueprint ----
    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found or has no widget tree"));
        return true;
    }

    // ---- 3. Parse XML ----
    // FXmlFile keeps only the first top-level element and silently drops any sibling after it,
    // so parse the payload inside a synthetic root and treat that root's children as the
    // top-level elements. The wrapper tags sit on their own lines so FXmlFile's line-based
    // <?xml / <!DOCTYPE cull still sees a declaration at the start of a line.
    FXmlFile XmlFile(TEXT("<PinWrightImportRoot>\n") + XmlString + TEXT("\n</PinWrightImportRoot>"),
        EConstructMethod::ConstructFromBuffer);
    if (!XmlFile.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_XML"), FString::Printf(
            TEXT("Failed to parse XML: %s"), *XmlFile.GetLastError()));
        return true;
    }

    const TArray<FXmlNode*>& TopLevelXmlNodes = XmlFile.GetRootNode()->GetChildrenNodes();
    if (TopLevelXmlNodes.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_XML"), TEXT("XML has no root node"));
        return true;
    }
    if (TopLevelXmlNodes.Num() > 1 && Mode.Equals(TEXT("replace")))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(
            TEXT("XML has %d top-level elements; 'replace' builds exactly one root. Wrap them in one panel, or use mode 'add' to append several siblings under a parent"),
            TopLevelXmlNodes.Num()));
        return true;
    }

    // ---- 4. Determine parent panel based on mode ----
    UPanelWidget* ParentPanel = nullptr;
    UWidget* TargetWidget = nullptr;
    UPanelWidget* TargetParent = nullptr;
    int32 TargetIndex = INDEX_NONE;

    if (Mode.Equals(TEXT("replace")))
    {
        if (!TargetName.IsEmpty())
        {
            // Replace subtree of target widget
            TargetWidget = WidgetBP->WidgetTree->FindWidget(FName(*TargetName));
            if (!TargetWidget)
            {
                Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(
                    TEXT("Target widget '%s' not found"), *TargetName));
                return true;
            }
            TargetParent = TargetWidget->GetParent();
            // The rebuilt subtree goes back at this index: in box panels the index is the layout.
            TargetIndex = TargetParent ? TargetParent->GetChildIndex(TargetWidget) : INDEX_NONE;
            // ParentPanel will be set after removal
        }
        // else: clear entire tree, parent = nullptr (root)
    }
    else // "add"
    {
        if (TargetName.IsEmpty())
        {
            Ctx.SendError(TEXT("MISSING_PARAMETER"),
                TEXT("targetName is required for 'add' mode"));
            return true;
        }
        UWidget* ParentWidget = WidgetBP->WidgetTree->FindWidget(FName(*TargetName));
        if (!ParentWidget)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(
                TEXT("Parent widget '%s' not found"), *TargetName));
            return true;
        }
        ParentPanel = Cast<UPanelWidget>(ParentWidget);
        if (!ParentPanel)
        {
            Ctx.SendError(TEXT("INVALID_TARGET"), FString::Printf(
                TEXT("Widget '%s' is not a panel and cannot have children added to it"),
                *TargetName));
            return true;
        }
    }

    // ---- 5. Build set of existing widget names (for duplicate checking) ----
    TSet<FString> UsedNames;
    if (Mode.Equals(TEXT("add")))
    {
        // In add mode, existing widget names must not conflict
        WidgetBP->WidgetTree->ForEachWidget([&UsedNames](UWidget* W)
        {
            UsedNames.Add(W->GetName());
        });
    }
    else if (Mode.Equals(TEXT("replace")) && !TargetName.IsEmpty())
    {
        // In targeted replace mode, collect names of widgets NOT in the target subtree
        TSet<FString> SubtreeNames;
        TFunction<void(UWidget*)> CollectSubtreeNames = [&](UWidget* W)
        {
            if (!W) return;
            SubtreeNames.Add(W->GetName());
            UPanelWidget* Panel = Cast<UPanelWidget>(W);
            if (Panel)
            {
                for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
                {
                    CollectSubtreeNames(Panel->GetChildAt(i));
                }
            }
        };
        CollectSubtreeNames(TargetWidget);

        WidgetBP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            FString WName = W->GetName();
            if (!SubtreeNames.Contains(WName))
            {
                UsedNames.Add(WName);
            }
        });
    }
    // For full replace, UsedNames starts empty (entire tree will be cleared)

    // ---- 6. VALIDATION PASS ----
    FString ValidationError;
    TSet<FString> ValidationUsedNames = UsedNames;
    for (const FXmlNode* TopLevelXmlNode : TopLevelXmlNodes)
    {
        if (!ValidateXmlNode(TopLevelXmlNode, WidgetBP, ValidationUsedNames, ValidationError))
        {
            Ctx.SendError(TEXT("VALIDATION_FAILED"), ValidationError);
            return true;
        }
    }

    // ---- 7-11. Transaction scope — mutations happen here, recompile happens after ----
    TArray<FPendingBinding> PendingBindings;
    TArray<FString> VariableWidgets;
    TArray<TSharedPtr<FJsonValue>> RootWidgetValues;
    int32 WidgetCount = 0;
    int32 BindingsCreated = 0;

    {   // Inner scope — FScopedTransaction commits when this scope ends, BEFORE recompile
        FScopedTransaction Transaction(FText::FromString(TEXT("Import Widget XML")));

        // ---- 8. Modify tree based on mode ----
        if (Mode.Equals(TEXT("replace")))
        {
            if (TargetName.IsEmpty())
            {
                ClearWidgetTree(WidgetBP);
                ParentPanel = nullptr;
            }
            else
            {
                ParentPanel = TargetParent;
                RemoveWidgetSubtree(WidgetBP, TargetWidget);
                TargetWidget = nullptr;
            }
        }

        // Save state for error rollback
        UWidget* PreviousRoot = WidgetBP->WidgetTree->RootWidget;
        const TArray<UWidget*> PreviousChildren = ParentPanel ? ParentPanel->GetAllChildren() : TArray<UWidget*>();

        // Rollback helper: remove every imported top-level subtree, wherever it was inserted
        auto RollbackImportedWidgets = [&]()
        {
            if (!ParentPanel)
            {
                if (WidgetBP->WidgetTree->RootWidget && WidgetBP->WidgetTree->RootWidget != PreviousRoot)
                    RemoveWidgetSubtree(WidgetBP, WidgetBP->WidgetTree->RootWidget);
                return;
            }
            for (int32 i = ParentPanel->GetChildrenCount() - 1; i >= 0; --i)
            {
                UWidget* Child = ParentPanel->GetChildAt(i);
                if (!PreviousChildren.Contains(Child))
                    RemoveWidgetSubtree(WidgetBP, Child);
            }
        };

        // ---- 9. CONSTRUCTION PASS ----
        FString ConstructionError;

        // TargetIndex is only set for a targeted replace, which has exactly one top-level node.
        for (const FXmlNode* TopLevelXmlNode : TopLevelXmlNodes)
        {
            if (!ProcessXmlNode(TopLevelXmlNode, WidgetBP, ParentPanel, UsedNames, PendingBindings, WidgetCount, ConstructionError, VariableWidgets, TargetIndex))
            {
                RollbackImportedWidgets();
                Transaction.Cancel();
                Ctx.SendError(TEXT("CONSTRUCTION_FAILED"), ConstructionError);
                return true;
            }
            RootWidgetValues.Add(MakeShared<FJsonValueString>(TopLevelXmlNode->GetAttribute(TEXT("name"))));
        }

        // ---- 10. Apply bindings ----
        if (!PendingBindings.IsEmpty())
        {
            FString BindingError;
            int32 Result = ApplyBindings(WidgetBP, PendingBindings, BindingError);
            if (Result < 0)
            {
                RollbackImportedWidgets();
                Transaction.Cancel();
                Ctx.SendError(TEXT("BINDING_FAILED"), BindingError);
                return true;
            }
            BindingsCreated = Result;
        }

        // ---- 11. Finalize within transaction ----
        // WidgetVariableNameToGuidMap is the engine's guid registry, keyed on tree membership
        // rather than on bIsVariable: FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids
        // re-adds every source widget under an ensureAlways. Pruning non-variable nodes here never
        // stuck — the recompile below undid it — and tripped that ensure on the way.
        EnsureAllWidgetVariableGuids(WidgetBP);
        WidgetBP->SetFlags(RF_Transactional);
        WidgetBP->Modify();
        // Recompile INSIDE transaction — matches UE's PasteWidgets pattern.
        // MarkBlueprintAsStructurallyModified does skeleton-only regen (no GC),
        // safe inside transactions per UE engine source.
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    }

    // ---- 12. Return result ----
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("widget_count"), WidgetCount);
    ResultObj->SetArrayField(TEXT("rootWidgets"), RootWidgetValues);
    ResultObj->SetNumberField(TEXT("bindings_created"), BindingsCreated);
    ResultObj->SetNumberField(TEXT("variablesCreated"), VariableWidgets.Num());
    if (VariableWidgets.Num() > 0)
    {
        ResultObj->SetBoolField(TEXT("requiresCompile"), true);
    }

    TArray<TSharedPtr<FJsonValue>> VariableWidgetValues;
    for (const FString& VarName : VariableWidgets)
    {
        VariableWidgetValues.Add(MakeShared<FJsonValueString>(VarName));
    }
    ResultObj->SetArrayField(TEXT("variableWidgets"), VariableWidgetValues);

    Ctx.SendSuccess(ResultObj);
    return true;
}
