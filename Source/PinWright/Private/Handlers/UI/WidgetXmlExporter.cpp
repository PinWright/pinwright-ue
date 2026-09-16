// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/WidgetXmlExporter.h"


#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetXmlUtils.h"
#include "Handlers/UI/WidgetInspectHelpers.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Utils/PropertyUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Styling/SlateBrush.h"
#include "UObject/UnrealType.h"
#include "Misc/EngineVersionComparison.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetXmlHelpers;

namespace
{
    using WidgetInspectHelpers::GeoFloat;

    bool ShouldExportProperty(const FProperty* Property)
    {
        if (!Property) return false;
        if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
        {
            return false;
        }
        if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
        {
            return true;
        }
        // UPanelSlot::Parent and UPanelSlot::Content are declared UPROPERTY(Instanced)
        // without CPF_Edit/CPF_BlueprintVisible, so the default visibility filter
        // drops them. They are the slot-chain references the exporter must emit
        // (callers that don't want them pass a PropertyNameSkipSet at the call
        // site — e.g. bOmitSlotChain on parent-walk traversal).
        UStruct* Owner = Property->GetOwnerStruct();
        if (Owner && Owner->IsChildOf(UPanelSlot::StaticClass()))
        {
            const FName PropFName = Property->GetFName();
            if (PropFName == TEXT("Parent") || PropFName == TEXT("Content"))
            {
                return true;
            }
        }
        return false;
    }

    void AppendGeomAttrs(
        TArray<TPair<FString, FString>>& AllAttrs,
        const FResolvedGeometry& Geo)
    {
        switch (Geo.Status)
        {
        case FResolvedGeometry::EStatus::Ok:
            AllAttrs.Emplace(TEXT("Geom.status"), TEXT("ok"));
            AllAttrs.Emplace(TEXT("Geom.abs_x"),   GeoFloat(Geo.AbsolutePos.X));
            AllAttrs.Emplace(TEXT("Geom.abs_y"),   GeoFloat(Geo.AbsolutePos.Y));
            AllAttrs.Emplace(TEXT("Geom.abs_w"),   GeoFloat(Geo.AbsoluteSize.X));
            AllAttrs.Emplace(TEXT("Geom.abs_h"),   GeoFloat(Geo.AbsoluteSize.Y));
            AllAttrs.Emplace(TEXT("Geom.local_w"), GeoFloat(Geo.LocalSize.X));
            AllAttrs.Emplace(TEXT("Geom.local_h"), GeoFloat(Geo.LocalSize.Y));
            if (!Geo.DesiredSize.IsNearlyZero())
            {
                AllAttrs.Emplace(TEXT("Geom.desired_w"), GeoFloat(Geo.DesiredSize.X));
                AllAttrs.Emplace(TEXT("Geom.desired_h"), GeoFloat(Geo.DesiredSize.Y));
            }
            AllAttrs.Emplace(TEXT("Geom.visibility"),
                XmlEscapeAttribute(Geo.EffectiveVisibility.ToString()));
            break;

        case FResolvedGeometry::EStatus::Skipped:
            AllAttrs.Emplace(TEXT("Geom.status"), TEXT("skipped"));
            AllAttrs.Emplace(TEXT("Geom.reason"), XmlEscapeAttribute(Geo.Reason));
            break;

        case FResolvedGeometry::EStatus::Error:
        default:
            AllAttrs.Emplace(TEXT("Geom.status"), TEXT("error"));
            AllAttrs.Emplace(TEXT("Geom.reason"), XmlEscapeAttribute(Geo.Reason));
            break;
        }
    }

    void AppendErrorGeomAttrs(TArray<TPair<FString, FString>>& AllAttrs, const TCHAR* Reason)
    {
        AllAttrs.Emplace(TEXT("Geom.status"), TEXT("error"));
        AllAttrs.Emplace(TEXT("Geom.reason"), Reason);
    }

    FString BuildRenderTransformElement(
        const FString& AttrIndentStr,
        const FWidgetTransform& WT)
    {
        return AttrIndentStr
            + FString::Printf(
                TEXT("<RenderTransform"
                     " translation_x=\"%s\" translation_y=\"%s\""
                     " scale_x=\"%s\" scale_y=\"%s\""
                     " shear_x=\"%s\" shear_y=\"%s\""
                     " angle_deg=\"%s\" />\n"),
                *GeoFloat(WT.Translation.X), *GeoFloat(WT.Translation.Y),
                *GeoFloat(WT.Scale.X),       *GeoFloat(WT.Scale.Y),
                *GeoFloat(WT.Shear.X),       *GeoFloat(WT.Shear.Y),
                *GeoFloat(WT.Angle));
    }

    bool IsOverrideBoolProperty(const FProperty* Property, const FString& PropName)
    {
        return CastField<FBoolProperty>(Property) && PropName.StartsWith(TEXT("bOverride_"));
    }

    FString MakeFullExportTextObjectPath(const UObject* Object)
    {
        if (!Object || !Object->GetClass())
        {
            return FString();
        }
        return FString::Printf(TEXT("%s'%s'"), *Object->GetClass()->GetPathName(), *Object->GetPathName());
    }

    bool TryReplaceResourceObjectValue(FString& AttrStr, const FString& CanonicalPath)
    {
        const FString Key = TEXT("ResourceObject=");
        const int32 ValueStart = AttrStr.Find(Key, ESearchCase::CaseSensitive);
        if (ValueStart == INDEX_NONE)
        {
            return false;
        }

        int32 ValueEnd = ValueStart + Key.Len();
        while (ValueEnd < AttrStr.Len() && AttrStr[ValueEnd] != TEXT(',') && AttrStr[ValueEnd] != TEXT(')'))
        {
            ++ValueEnd;
        }

        AttrStr = AttrStr.Left(ValueStart + Key.Len())
            + CanonicalPath
            + AttrStr.Mid(ValueEnd);
        return true;
    }

    void NormalizeSlateBrushResourceObjectAttribute(
        FString& AttrStr,
        const FProperty* Property,
        const void* InstancePtr)
    {
        const FStructProperty* StructProp = CastField<FStructProperty>(Property);
        if (!StructProp || !StructProp->Struct || !StructProp->Struct->IsChildOf(TBaseStructure<FSlateBrush>::Get()))
        {
            return;
        }

        const FSlateBrush* Brush = static_cast<const FSlateBrush*>(InstancePtr);
        const FString CanonicalPath = MakeFullExportTextObjectPath(Brush ? Brush->GetResourceObject() : nullptr);
        if (!CanonicalPath.IsEmpty())
        {
            TryReplaceResourceObjectValue(AttrStr, CanonicalPath);
        }
    }
} // anonymous namespace

namespace WidgetXmlExporter
{

TMap<FString, TArray<TPair<FString, FString>>> BuildBindingMap(UWidgetBlueprint* WidgetBlueprint)
{
    TMap<FString, TArray<TPair<FString, FString>>> WidgetBindingMap;
    if (!WidgetBlueprint) return WidgetBindingMap;

    auto ReadStructField = [](UScriptStruct* StructType, const void* StructData,
        const TCHAR* FieldName) -> FString
    {
        if (!StructType || !StructData) return FString();
        FProperty* Field = StructType->FindPropertyByName(FName(FieldName));
        if (!Field) return FString();
        const void* ValuePtr = Field->ContainerPtrToValuePtr<void>(StructData);
        if (!ValuePtr) return FString();
        FString Out;
        if (const FNameProperty* NameProp = CastField<FNameProperty>(Field))
            Out = NameProp->GetPropertyValue(ValuePtr).ToString();
        else if (const FStrProperty* StrProp = CastField<FStrProperty>(Field))
            Out = StrProp->GetPropertyValue(ValuePtr);
        else if (const FTextProperty* TextProp = CastField<FTextProperty>(Field))
            Out = TextProp->GetPropertyValue(ValuePtr).ToString();
        else
            Field->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
        return Out;
    };

    if (FArrayProperty* BindingsArrayProp = FindFProperty<FArrayProperty>(
        WidgetBlueprint->GetClass(), TEXT("Bindings")))
    {
        void* BindingsArrayPtr = BindingsArrayProp->ContainerPtrToValuePtr<void>(WidgetBlueprint);
        FScriptArrayHelper BindingsHelper(BindingsArrayProp, BindingsArrayPtr);
        if (FStructProperty* BindingStructProp = CastField<FStructProperty>(
            BindingsArrayProp->Inner))
        {
            UScriptStruct* BindingStruct = BindingStructProp->Struct;
            for (int32 Index = 0; Index < BindingsHelper.Num(); ++Index)
            {
                const void* BindingData = BindingsHelper.GetRawPtr(Index);
                FString ObjectName   = ReadStructField(BindingStruct, BindingData, TEXT("ObjectName"));
                FString PropertyName = ReadStructField(BindingStruct, BindingData, TEXT("PropertyName"));
                FString FunctionName = ReadStructField(BindingStruct, BindingData, TEXT("FunctionName"));
                FString WidgetKey = ObjectName.IsEmpty() ? TEXT("__root__") : ObjectName;
                WidgetBindingMap.FindOrAdd(WidgetKey).Emplace(PropertyName, FunctionName);
            }
        }
    }

    return WidgetBindingMap;
}

FWidgetTreeRootResolution ResolveWidgetTreeRoot(UWidgetBlueprint* WidgetBlueprint)
{
    if (!WidgetBlueprint)
    {
        return {nullptr, false, TEXT("null widget blueprint"), FString()};
    }

    if (!WidgetBlueprint->WidgetTree)
    {
        return {nullptr, false, TEXT("WidgetTree is null"), FString()};
    }

    if (UWidget* RootWidget = WidgetBlueprint->WidgetTree->RootWidget)
    {
        return {RootWidget, false, FString(), FString()};
    }

    UWidgetBlueprintGeneratedClass* BPGC =
        Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
    if (!BPGC)
    {
        return {nullptr, true,
            TEXT("WidgetTree has no RootWidget (likely native-tree parent or inherited-only widget)"),
            FString()};
    }

    UWidgetBlueprintGeneratedClass* OwnerBPGC = BPGC->FindWidgetTreeOwningClass();
    UWidgetTree* OwnerTree = OwnerBPGC ? OwnerBPGC->GetWidgetTreeArchetype() : nullptr;
    if (OwnerBPGC == nullptr || OwnerBPGC == BPGC ||
        !OwnerTree || !OwnerTree->RootWidget)
    {
        return {nullptr, true,
            TEXT("WidgetTree has no RootWidget; no inheritable RootWidget found in ancestor classes"),
            FString()};
    }

    UWidgetBlueprint* ParentWBP = Cast<UWidgetBlueprint>(OwnerBPGC->ClassGeneratedBy);
    const FString ParentPath = ParentWBP ? ParentWBP->GetPathName() : OwnerBPGC->GetPathName();
    return {OwnerTree->RootWidget, false, FString(), ParentPath};
}

TArray<TPair<FString, FString>> CollectOverriddenAttributes(
    UObject* Instance,
    bool bIncludeDefaults,
    const FString& Prefix,
    const TSet<FString>* PropertyNameSkipSet)
{
    TArray<TPair<FString, FString>> Attrs;
    if (!Instance) return Attrs;

    UObject* CDO = Instance->GetClass()->GetDefaultObject();

    for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (!ShouldExportProperty(Property)) continue;

        const FString PropName = Property->GetName();

        // Caller may exclude property names (e.g. Parent/Content sibling-chain
        // expansion that causes O(N^2) blow-up on panel-heavy widgets).
        if (PropertyNameSkipSet && PropertyNameSkipSet->Contains(PropName)) continue;

        const void* InstancePtr = Property->ContainerPtrToValuePtr<void>(Instance);
        if (CDO && !bIncludeDefaults)
        {
            const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(CDO);
            if (Property->Identical(InstancePtr, DefaultPtr, PPF_None)
                && !IsOverrideBoolProperty(Property, PropName))
            {
                continue;
            }
        }

        TSharedPtr<FJsonValue> JsonVal = ExportPropertyToJsonValue(Instance, Property);
        if (!JsonVal.IsValid()) continue;

        FString AttrStr = JsonValueToAttrString(JsonVal);
        NormalizeSlateBrushResourceObjectAttribute(AttrStr, Property, InstancePtr);
        if (AttrStr.IsEmpty()) continue;

        FString AttrName = SanitizeXmlName(Prefix.IsEmpty() ? PropName : Prefix + PropName);
        Attrs.Emplace(MoveTemp(AttrName), XmlEscapeAttribute(AttrStr));
    }
    return Attrs;
}

FString BuildXmlString(
    UWidget* Widget,
    int32 Indent,
    bool bIncludeDefaults,
    const TMap<FString, TArray<TPair<FString, FString>>>& WidgetBindings,
    const TMap<FName, FGuid>* VariableGuidMap,
    int32& WidgetCount,
    const FGeomContext& Geom,
    bool bIsRoot,
    const TArray<TPair<FString, FString>>& ExtraRootAttrs,
    bool bOmitSlotChain)
{
    if (!Widget) return FString();

    ++WidgetCount;

    const FString FullClassName = Widget->GetClass()->GetName();
    const FString RawTagName = WidgetXmlHelpers::StripClassPrefix(FullClassName);
    const FString TagName    = SanitizeXmlName(RawTagName);
    const FString WidgetName = Widget->GetFName().ToString();

    const FString IndentStr = FString::ChrN(Indent * 2, TEXT(' '));
    const FString AttrIndentStr = IndentStr + TEXT("  ");

    TArray<TPair<FString, FString>> AllAttrs;
    AllAttrs.Emplace(TEXT("name"), XmlEscapeAttribute(WidgetName));

    // Preserve original class name for round-trip when the tag was sanitized.
    // Emit the full GetClass()->GetName() (with any _C suffix) so the importer
    // can resolve via direct FindObject without guessing prefix/suffix variants.
    if (TagName != RawTagName)
    {
        AllAttrs.Emplace(TEXT("original-name"), XmlEscapeAttribute(FullClassName));
    }

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // UE 5.4/5.5: no WidgetVariableNameToGuidMap (VariableGuidMap is always nullptr here);
    // the variable signal is UWidget::bIsVariable.
    (void)VariableGuidMap;
    AllAttrs.Emplace(TEXT("IsVariable"),
        Widget->bIsVariable ? TEXT("true") : TEXT("false"));
#else
    if (VariableGuidMap)
    {
        AllAttrs.Emplace(TEXT("IsVariable"),
            VariableGuidMap->Contains(Widget->GetFName()) ? TEXT("true") : TEXT("false"));
    }
#endif

    static const TSet<FString> RawWidgetSlotSkipSet{TEXT("Slot")};
    AllAttrs.Append(CollectOverriddenAttributes(
        Widget, bIncludeDefaults, FString(),
        bOmitSlotChain ? &RawWidgetSlotSkipSet : nullptr));

    if (Widget->Slot)
    {
        // When omitting the slot chain, drop Parent and Content to prevent
        // the O(N^2) sibling-chain expansion on panel-heavy widgets.
        static const TSet<FString> SlotChainSkipSet{TEXT("Parent"), TEXT("Content")};
        AllAttrs.Append(CollectOverriddenAttributes(
            Widget->Slot, bIncludeDefaults, TEXT("Slot."),
            bOmitSlotChain ? &SlotChainSkipSet : nullptr));
    }

    const TArray<TPair<FString, FString>>* Bindings = WidgetBindings.Find(WidgetName);
    if (Bindings)
    {
        for (const TPair<FString, FString>& Binding : *Bindings)
        {
            AllAttrs.Emplace(
                SanitizeXmlName(FString::Printf(TEXT("Bind.%s"), *Binding.Key)),
                XmlEscapeAttribute(Binding.Value));
        }
    }

    if (bIsRoot)
    {
        AllAttrs.Append(ExtraRootAttrs);
    }

    bool bHasRenderTransform = false;
    FWidgetTransform RenderTransformValue;

    if (Geom.bEnabled && Geom.bAmbiguous)
    {
        AppendErrorGeomAttrs(AllAttrs, TEXT("AMBIGUOUS_INSTANCE"));
    }
    else if (Geom.bEnabled && Geom.Result)
    {
        if (Geom.NameIndex && Geom.NameIndex->Num() > 0)
        {
            if (const FResolvedGeometry* Found = FWidgetGeometryResolver::LookupByName(
                    Geom.Result->ByWidget, *Geom.NameIndex, Widget->GetFName()))
            {
                AppendGeomAttrs(AllAttrs, *Found);
                if (Found->Status == FResolvedGeometry::EStatus::Ok && Found->bHasRenderTransform)
                {
                    bHasRenderTransform = true;
                    RenderTransformValue = Found->RenderTransformValue;
                }
            }
            else
            {
                AppendErrorGeomAttrs(AllAttrs, TEXT("NOT_REACHED"));
            }
        }
        else if (!Geom.Result->TopLevelError.IsEmpty())
        {
            AllAttrs.Emplace(TEXT("Geom.status"), TEXT("error"));
            AllAttrs.Emplace(TEXT("Geom.reason"),
                XmlEscapeAttribute(Geom.Result->TopLevelError));
        }
        else
        {
            AppendErrorGeomAttrs(AllAttrs, TEXT("NOT_REACHED"));
        }
    }

    FString Xml = IndentStr + TEXT("<") + TagName;

    if (AllAttrs.Num() == 1)
    {
        Xml += FString::Printf(TEXT(" %s=\"%s\""), *AllAttrs[0].Key, *AllAttrs[0].Value);
    }
    else
    {
        for (int32 i = 0; i < AllAttrs.Num(); ++i)
        {
            if (i == 0)
            {
                Xml += FString::Printf(TEXT(" %s=\"%s\""),
                    *AllAttrs[i].Key, *AllAttrs[i].Value);
            }
            else
            {
                Xml += TEXT("\n") + AttrIndentStr +
                    FString::Printf(TEXT("%s=\"%s\""),
                        *AllAttrs[i].Key, *AllAttrs[i].Value);
            }
        }
    }

    UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
    const int32 ChildCount = Panel ? Panel->GetChildrenCount() : 0;
    const bool bHasChildElements = (Panel && ChildCount > 0) || bHasRenderTransform;

    if (bHasChildElements)
    {
        Xml += TEXT(">\n");

        if (bHasRenderTransform)
        {
            Xml += BuildRenderTransformElement(AttrIndentStr, RenderTransformValue);
        }

        if (Panel && ChildCount > 0)
        {
            for (int32 i = 0; i < ChildCount; ++i)
            {
                UWidget* Child = Panel->GetChildAt(i);
                if (Child)
                {
                    Xml += BuildXmlString(Child, Indent + 1, bIncludeDefaults,
                        WidgetBindings, VariableGuidMap, WidgetCount,
                        Geom, /*bIsRoot=*/false, ExtraRootAttrs, bOmitSlotChain);
                }
            }
        }

        Xml += IndentStr + TEXT("</") + TagName + TEXT(">\n");
    }
    else
    {
        Xml += TEXT(" />\n");
    }

    return Xml;
}

FWidgetTreeXmlResult BuildWidgetTreeXmlWithDiagnostic(UWidgetBlueprint* WidgetBlueprint, bool bIncludeDefaults, bool bOmitSlotChain)
{
    FWidgetTreeRootResolution RootResolution = ResolveWidgetTreeRoot(WidgetBlueprint);
    if (!RootResolution.StartWidget)
    {
        return {FString(), RootResolution.bEmptyByDesign, RootResolution.Reason};
    }

    TMap<FString, TArray<TPair<FString, FString>>> WidgetBindingMap = BuildBindingMap(WidgetBlueprint);

    // UE 5.4/5.5 has no WidgetVariableNameToGuidMap; pass nullptr so BuildXmlString
    // derives the IsVariable attribute from UWidget::bIsVariable instead.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    const TMap<FName, FGuid>* VariableGuidMap = nullptr;
#else
    const TMap<FName, FGuid>* VariableGuidMap = &WidgetBlueprint->WidgetVariableNameToGuidMap;
#endif

    // Geometry disabled — asset-dump path (no live resolve). Per-element gate on Geom.bEnabled
    // already suppresses Geom.* on descendants; emit no root sentinel so absence-means-disabled.
    TArray<TPair<FString, FString>> RootGeomAttrs;
    if (!RootResolution.InheritedFromPath.IsEmpty())
    {
        RootGeomAttrs.Emplace(TEXT("inherited_from"),
            XmlEscapeAttribute(RootResolution.InheritedFromPath));
    }

    FGeomContext GeomCtx;
    // bEnabled = false, so geometry is skipped entirely.

    int32 WidgetCount = 0;
    FString XmlOutput = BuildXmlString(
        RootResolution.StartWidget, 0, bIncludeDefaults, WidgetBindingMap, VariableGuidMap, WidgetCount,
        GeomCtx, /*bIsRoot=*/true, RootGeomAttrs, bOmitSlotChain);

    XmlOutput.TrimEndInline();

    if (!RootResolution.InheritedFromPath.IsEmpty())
    {
        XmlOutput.InsertAt(0, FString::Printf(
            TEXT("<!-- inherited from %s -->\n"), *RootResolution.InheritedFromPath));
    }

    return {MoveTemp(XmlOutput), false, FString()};
}

FString BuildWidgetTreeXml(UWidgetBlueprint* WidgetBlueprint, bool bIncludeDefaults, bool bOmitSlotChain)
{
    return BuildWidgetTreeXmlWithDiagnostic(WidgetBlueprint, bIncludeDefaults, bOmitSlotChain).Xml;
}

} // namespace WidgetXmlExporter
