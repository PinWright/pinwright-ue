// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/LiveUiSnapshotXmlWriter.h"

#include "Compat/JsonKeyCompat.h"
#include "Handlers/UI/LiveUiSnapshot.h"
#include "Handlers/UI/WidgetInspectHelpers.h"
#include "Handlers/UI/WidgetXmlUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    using WidgetInspectHelpers::HasVector;

    FString SnapshotFloat(const double Value)
    {
        return FString::Printf(TEXT("%.3f"), Value);
    }

    FString IndentString(const int32 Indent)
    {
        return FString::ChrN(Indent * 2, TEXT(' '));
    }

    void AddStringAttribute(
        TArray<TPair<FString, FString>>& Attributes,
        const TCHAR* Name,
        const FString& Value)
    {
        if (!Value.IsEmpty())
        {
            Attributes.Emplace(Name, WidgetXmlHelpers::XmlEscapeAttribute(Value));
        }
    }

    void AddBoolAttribute(
        TArray<TPair<FString, FString>>& Attributes,
        const TCHAR* Name,
        const bool bValue)
    {
        Attributes.Emplace(Name, bValue ? TEXT("true") : TEXT("false"));
    }

    FString BuildOpenTag(
        const FString& Indent,
        const FString& TagName,
        const TArray<TPair<FString, FString>>& Attributes)
    {
        FString Xml = Indent + TEXT("<") + TagName;
        for (const TPair<FString, FString>& Attribute : Attributes)
        {
            Xml += FString::Printf(TEXT(" %s=\"%s\""), *Attribute.Key, *Attribute.Value);
        }
        return Xml;
    }

    FString BuildSelfClosingElement(
        const FString& Indent,
        const FString& TagName,
        const TArray<TPair<FString, FString>>& Attributes)
    {
        return BuildOpenTag(Indent, TagName, Attributes) + TEXT(" />\n");
    }

    TArray<FString> GetSortedFieldNames(const TSharedPtr<FJsonObject>& Object)
    {
        TArray<FString> FieldNames;
        if (Object.IsValid())
        {
            for (const auto& Pair : Object->Values)
            {
                FieldNames.Add(EARGCompat::JsonKeyToString(Pair.Key));
            }
            FieldNames.Sort();
        }
        return FieldNames;
    }

    void AppendJsonFieldAttributes(
        TArray<TPair<FString, FString>>& Attributes,
        const TSharedPtr<FJsonObject>& Object,
        const TSet<FString>& ExcludedFields)
    {
        if (!Object.IsValid())
        {
            return;
        }

        for (const FString& FieldName : GetSortedFieldNames(Object))
        {
            if (ExcludedFields.Contains(FieldName))
            {
                continue;
            }

            const TSharedPtr<FJsonValue>* FieldValue = Object->Values.Find(EARGCompat::JsonFieldKey(FieldName));
            if (!FieldValue || !FieldValue->IsValid() || (*FieldValue)->Type == EJson::Null)
            {
                continue;
            }

            Attributes.Emplace(
                FieldName,
                WidgetXmlHelpers::XmlEscapeAttribute(WidgetXmlHelpers::JsonValueToAttrString(*FieldValue)));
        }
    }

    FString GetStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        FString Value;
        if (Object.IsValid())
        {
            Object->TryGetStringField(FieldName, Value);
        }
        return Value;
    }

    FString BuildSourceElement(const FLiveUiSnapshotSourceInfo& SourceInfo, const int32 Indent)
    {
        if (!SourceInfo.bHasBackingWidget)
        {
            return FString();
        }

        TArray<TPair<FString, FString>> Attributes;
        AddStringAttribute(Attributes, TEXT("widget_bp"), SourceInfo.WidgetBlueprintPath);
        AddStringAttribute(Attributes, TEXT("widget_class"), SourceInfo.WidgetClassPath);
        AddStringAttribute(Attributes, TEXT("widget_name"), SourceInfo.WidgetName);
        AddStringAttribute(Attributes, TEXT("owning_user_widget"), SourceInfo.OwningUserWidgetName);
        AddStringAttribute(Attributes, TEXT("owning_user_widget_class"), SourceInfo.OwningUserWidgetClassPath);
        AddStringAttribute(Attributes, TEXT("source_kind"), SourceInfo.SourceKind);

        return BuildSelfClosingElement(IndentString(Indent), TEXT("Source"), Attributes);
    }

    FString BuildProperties(const TSharedPtr<FJsonObject>& Properties, const int32 Indent)
    {
        if (!Properties.IsValid() || Properties->Values.Num() == 0)
        {
            return FString();
        }

        FString Xml;
        const FString IndentText = IndentString(Indent);
        for (const FString& PropertyName : GetSortedFieldNames(Properties))
        {
            const TSharedPtr<FJsonValue>* PropertyValue = Properties->Values.Find(EARGCompat::JsonFieldKey(PropertyName));
            if (!PropertyValue || !PropertyValue->IsValid() || (*PropertyValue)->Type == EJson::Null)
            {
                continue;
            }

            TArray<TPair<FString, FString>> Attributes;
            AddStringAttribute(Attributes, TEXT("name"), PropertyName);
            Attributes.Emplace(
                TEXT("value"),
                WidgetXmlHelpers::XmlEscapeAttribute(WidgetXmlHelpers::JsonValueToAttrString(*PropertyValue)));
            Xml += BuildSelfClosingElement(IndentText, TEXT("Property"), Attributes);
        }

        return Xml;
    }

    FString BuildSlotElement(const FLiveUiSnapshotSlotInfo& SlotInfo, const int32 Indent)
    {
        const bool bHasProperties = SlotInfo.Properties.IsValid() && SlotInfo.Properties->Values.Num() > 0;
        if (SlotInfo.SlotClassName.IsEmpty() && !bHasProperties)
        {
            return FString();
        }

        TArray<TPair<FString, FString>> Attributes;
        AddStringAttribute(Attributes, TEXT("type"), SlotInfo.SlotClassName);

        const FString IndentText = IndentString(Indent);
        if (!bHasProperties)
        {
            return BuildSelfClosingElement(IndentText, TEXT("Slot"), Attributes);
        }

        FString Xml = BuildOpenTag(IndentText, TEXT("Slot"), Attributes) + TEXT(">\n");
        Xml += BuildProperties(SlotInfo.Properties, Indent + 1);
        Xml += IndentText + TEXT("</Slot>\n");
        return Xml;
    }

    FString BuildBindingLikeElement(
        const TSharedPtr<FJsonObject>& BindingInfo,
        const int32 Indent,
        const TCHAR* ElementName,
        const TCHAR* PrimaryAttributeName,
        const TArray<FString>& PrimaryFieldCandidates)
    {
        if (!BindingInfo.IsValid() || BindingInfo->Values.Num() == 0)
        {
            return FString();
        }

        TArray<TPair<FString, FString>> Attributes;
        TSet<FString> ConsumedFields;

        for (const FString& Candidate : PrimaryFieldCandidates)
        {
            const FString PrimaryValue = GetStringField(BindingInfo, *Candidate);
            if (!PrimaryValue.IsEmpty())
            {
                AddStringAttribute(Attributes, PrimaryAttributeName, PrimaryValue);
                ConsumedFields.Add(Candidate);
                break;
            }
        }

        const FString FunctionName = GetStringField(BindingInfo, TEXT("functionName"));
        if (!FunctionName.IsEmpty())
        {
            AddStringAttribute(Attributes, TEXT("function"), FunctionName);
            ConsumedFields.Add(TEXT("functionName"));
        }

        AppendJsonFieldAttributes(Attributes, BindingInfo, ConsumedFields);
        return BuildSelfClosingElement(IndentString(Indent), ElementName, Attributes);
    }

    FString BuildBindingElements(const TArray<TSharedPtr<FJsonObject>>& Bindings, const int32 Indent)
    {
        FString Xml;
        TArray<FString> PrimaryFields;
        PrimaryFields.Add(TEXT("propertyName"));
        PrimaryFields.Add(TEXT("property"));
        for (const TSharedPtr<FJsonObject>& Binding : Bindings)
        {
            Xml += BuildBindingLikeElement(
                Binding,
                Indent,
                TEXT("Binding"),
                TEXT("property"),
                PrimaryFields);
        }
        return Xml;
    }

    FString BuildDelegateElements(const TArray<TSharedPtr<FJsonObject>>& Delegates, const int32 Indent)
    {
        FString Xml;
        TArray<FString> PrimaryFields;
        PrimaryFields.Add(TEXT("eventName"));
        PrimaryFields.Add(TEXT("event"));
        PrimaryFields.Add(TEXT("propertyName"));
        PrimaryFields.Add(TEXT("property"));
        for (const TSharedPtr<FJsonObject>& Delegate : Delegates)
        {
            Xml += BuildBindingLikeElement(
                Delegate,
                Indent,
                TEXT("Delegate"),
                TEXT("event"),
                PrimaryFields);
        }
        return Xml;
    }

    FString BuildRenderTransformElement(const FWidgetTransform& Transform, const int32 Indent)
    {
        TArray<TPair<FString, FString>> Attributes;
        Attributes.Emplace(TEXT("translation_x"), SnapshotFloat(Transform.Translation.X));
        Attributes.Emplace(TEXT("translation_y"), SnapshotFloat(Transform.Translation.Y));
        Attributes.Emplace(TEXT("scale_x"), SnapshotFloat(Transform.Scale.X));
        Attributes.Emplace(TEXT("scale_y"), SnapshotFloat(Transform.Scale.Y));
        Attributes.Emplace(TEXT("shear_x"), SnapshotFloat(Transform.Shear.X));
        Attributes.Emplace(TEXT("shear_y"), SnapshotFloat(Transform.Shear.Y));
        Attributes.Emplace(TEXT("angle_deg"), SnapshotFloat(Transform.Angle));

        return BuildSelfClosingElement(IndentString(Indent), TEXT("RenderTransform"), Attributes);
    }

    FString BuildGeometryElement(
        const FLiveUiSnapshotRuntimeState& RuntimeState,
        const int32 Indent,
        const bool bGeometryIncluded)
    {
        if (!bGeometryIncluded)
        {
            return FString();
        }

        TArray<TPair<FString, FString>> Attributes;
        Attributes.Emplace(TEXT("abs_x"), SnapshotFloat(RuntimeState.AbsolutePosition.X));
        Attributes.Emplace(TEXT("abs_y"), SnapshotFloat(RuntimeState.AbsolutePosition.Y));
        Attributes.Emplace(TEXT("abs_w"), SnapshotFloat(RuntimeState.AbsoluteSize.X));
        Attributes.Emplace(TEXT("abs_h"), SnapshotFloat(RuntimeState.AbsoluteSize.Y));

        return BuildSelfClosingElement(IndentString(Indent), TEXT("Geometry"), Attributes);
    }

    TArray<TPair<FString, FString>> BuildNodeAttributes(const FLiveUiSnapshotNode& Node)
    {
        TArray<TPair<FString, FString>> Attributes;
        AddStringAttribute(Attributes, TEXT("name"), Node.DebugName);
        AddStringAttribute(Attributes, TEXT("visibility"), Node.RuntimeState.Visibility);
        if (Node.RuntimeState.bEnabled.IsSet())
        {
            AddBoolAttribute(Attributes, TEXT("enabled"), Node.RuntimeState.bEnabled.Get(false));
        }
        if (Node.RuntimeState.bFocused)
        {
            AddBoolAttribute(Attributes, TEXT("focused"), true);
        }
        if (Node.RuntimeState.bFocusable)
        {
            AddBoolAttribute(Attributes, TEXT("focusable"), true);
        }
        AddStringAttribute(Attributes, TEXT("clipping"), Node.RuntimeState.Clipping);
        if (Node.RuntimeState.bVolatile)
        {
            AddBoolAttribute(Attributes, TEXT("volatile"), true);
        }
        if (HasVector(Node.RuntimeState.DesiredSize))
        {
            Attributes.Emplace(TEXT("desired_w"), SnapshotFloat(Node.RuntimeState.DesiredSize.X));
            Attributes.Emplace(TEXT("desired_h"), SnapshotFloat(Node.RuntimeState.DesiredSize.Y));
        }
        return Attributes;
    }

    FString BuildNodeElement(
        const FLiveUiSnapshotNode& Node,
        const int32 Indent,
        const bool bGeometryIncluded)
    {
        const FString TagName = WidgetXmlHelpers::SanitizeXmlName(
            Node.SlateType.IsEmpty() ? TEXT("SWidget") : Node.SlateType);
        const FString IndentText = IndentString(Indent);
        const TArray<TPair<FString, FString>> Attributes = BuildNodeAttributes(Node);

        FString Body;
        Body += BuildSourceElement(Node.SourceInfo, Indent + 1);
        Body += BuildSlotElement(Node.SlotInfo, Indent + 1);
        Body += BuildProperties(Node.Properties, Indent + 1);
        Body += BuildBindingElements(Node.Bindings, Indent + 1);
        Body += BuildDelegateElements(Node.Delegates, Indent + 1);
        if (Node.RuntimeState.bHasRenderTransform)
        {
            Body += BuildRenderTransformElement(Node.RuntimeState.RenderTransform, Indent + 1);
        }
        Body += BuildGeometryElement(Node.RuntimeState, Indent + 1, bGeometryIncluded);

        for (const FLiveUiSnapshotNode& Child : Node.Children)
        {
            Body += BuildNodeElement(Child, Indent + 1, bGeometryIncluded);
        }

        if (Body.IsEmpty())
        {
            return BuildSelfClosingElement(IndentText, TagName, Attributes);
        }

        FString Xml = BuildOpenTag(IndentText, TagName, Attributes) + TEXT(">\n");
        Xml += Body;
        Xml += IndentText + TEXT("</") + TagName + TEXT(">\n");
        return Xml;
    }
}

FString FLiveUiSnapshotXmlWriter::Write(const FLiveUiSnapshot& Snapshot)
{
    return BuildNodeElement(Snapshot.RootNode, 0, Snapshot.bGeometryIncluded);
}
