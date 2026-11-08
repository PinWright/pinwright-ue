// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/LiveUiSnapshotJsonWriter.h"

#include "Handlers/UI/LiveUiSnapshot.h"
#include "Handlers/UI/WidgetInspectHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    using WidgetInspectHelpers::HasVector;

    struct FLiveUiSnapshotSummary
    {
        int32 TotalNodeCount = 0;
        int32 BackingWidgetNodeCount = 0;
        int32 GeometryNodeCount = 0;
        int32 MaxDepth = 0;
    };

    bool HasObjectFields(const TSharedPtr<FJsonObject>& Object)
    {
        return Object.IsValid() && Object->Values.Num() > 0;
    }

    TSharedPtr<FJsonObject> DeepCopyJsonObject(const TSharedPtr<FJsonObject>& SourceObject);

    TSharedPtr<FJsonValue> DeepCopyJsonValue(const TSharedPtr<FJsonValue>& SourceValue)
    {
        if (!SourceValue.IsValid() || SourceValue->IsNull())
        {
            return MakeShared<FJsonValueNull>();
        }

        switch (SourceValue->Type)
        {
        case EJson::String:
            return MakeShared<FJsonValueString>(SourceValue->AsString());
        case EJson::Number:
            return MakeShared<FJsonValueNumber>(SourceValue->AsNumber());
        case EJson::Boolean:
            return MakeShared<FJsonValueBoolean>(SourceValue->AsBool());
        case EJson::Array:
        {
            TArray<TSharedPtr<FJsonValue>> CopiedArray;
            const TArray<TSharedPtr<FJsonValue>>& SourceArray = SourceValue->AsArray();
            CopiedArray.Reserve(SourceArray.Num());
            for (const TSharedPtr<FJsonValue>& ArrayValue : SourceArray)
            {
                CopiedArray.Add(DeepCopyJsonValue(ArrayValue));
            }
            return MakeShared<FJsonValueArray>(CopiedArray);
        }
        case EJson::Object:
            return MakeShared<FJsonValueObject>(DeepCopyJsonObject(SourceValue->AsObject()));
        case EJson::Null:
        default:
            return MakeShared<FJsonValueNull>();
        }
    }

    TSharedPtr<FJsonObject> DeepCopyJsonObject(const TSharedPtr<FJsonObject>& SourceObject)
    {
        TSharedPtr<FJsonObject> CopiedObject = MakeShared<FJsonObject>();
        if (!SourceObject.IsValid())
        {
            return CopiedObject;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : SourceObject->Values)
        {
            CopiedObject->SetField(Field.Key, DeepCopyJsonValue(Field.Value));
        }

        return CopiedObject;
    }

    TSharedPtr<FJsonObject> BuildSizeObject(const FVector2D& Size)
    {
        TSharedPtr<FJsonObject> SizeObject = MakeShared<FJsonObject>();
        SizeObject->SetNumberField(TEXT("w"), Size.X);
        SizeObject->SetNumberField(TEXT("h"), Size.Y);
        return SizeObject;
    }

    TSharedPtr<FJsonObject> BuildVectorObject(const FVector2D& Vector)
    {
        TSharedPtr<FJsonObject> VectorObject = MakeShared<FJsonObject>();
        VectorObject->SetNumberField(TEXT("x"), Vector.X);
        VectorObject->SetNumberField(TEXT("y"), Vector.Y);
        return VectorObject;
    }

    TSharedPtr<FJsonObject> BuildTransformObject(const FWidgetTransform& Transform)
    {
        TSharedPtr<FJsonObject> TransformObject = MakeShared<FJsonObject>();
        TransformObject->SetObjectField(TEXT("translation"), BuildVectorObject(Transform.Translation));
        TransformObject->SetObjectField(TEXT("scale"), BuildVectorObject(Transform.Scale));
        TransformObject->SetObjectField(TEXT("shear"), BuildVectorObject(Transform.Shear));
        TransformObject->SetNumberField(TEXT("angle_deg"), Transform.Angle);
        return TransformObject;
    }

    TSharedPtr<FJsonObject> BuildRuntimeStateObject(const FLiveUiSnapshotRuntimeState& RuntimeState)
    {
        TSharedPtr<FJsonObject> RuntimeObject = MakeShared<FJsonObject>();

        if (!RuntimeState.Visibility.IsEmpty())
        {
            RuntimeObject->SetStringField(TEXT("visibility"), RuntimeState.Visibility);
        }
        if (RuntimeState.bEnabled.IsSet())
        {
            RuntimeObject->SetBoolField(TEXT("enabled"), RuntimeState.bEnabled.Get(false));
        }
        if (RuntimeState.bFocused)
        {
            RuntimeObject->SetBoolField(TEXT("focused"), true);
        }
        if (RuntimeState.bFocusable)
        {
            RuntimeObject->SetBoolField(TEXT("focusable"), true);
        }
        if (!RuntimeState.Clipping.IsEmpty())
        {
            RuntimeObject->SetStringField(TEXT("clipping"), RuntimeState.Clipping);
        }
        if (RuntimeState.bVolatile)
        {
            RuntimeObject->SetBoolField(TEXT("volatile"), true);
        }
        if (HasVector(RuntimeState.DesiredSize))
        {
            RuntimeObject->SetObjectField(TEXT("desired_size"), BuildSizeObject(RuntimeState.DesiredSize));
        }
        if (RuntimeState.bHasRenderTransform)
        {
            RuntimeObject->SetObjectField(TEXT("render_transform"), BuildTransformObject(RuntimeState.RenderTransform));
        }

        return RuntimeObject;
    }

    TSharedPtr<FJsonObject> BuildGeometryObject(const FLiveUiSnapshotRuntimeState& RuntimeState)
    {
        TSharedPtr<FJsonObject> GeometryObject = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> AbsoluteObject = MakeShared<FJsonObject>();
        AbsoluteObject->SetNumberField(TEXT("x"), RuntimeState.AbsolutePosition.X);
        AbsoluteObject->SetNumberField(TEXT("y"), RuntimeState.AbsolutePosition.Y);
        AbsoluteObject->SetNumberField(TEXT("w"), RuntimeState.AbsoluteSize.X);
        AbsoluteObject->SetNumberField(TEXT("h"), RuntimeState.AbsoluteSize.Y);
        GeometryObject->SetObjectField(TEXT("absolute"), AbsoluteObject);
        return GeometryObject;
    }

    TSharedPtr<FJsonObject> BuildSourceObject(const FLiveUiSnapshotSourceInfo& SourceInfo)
    {
        TSharedPtr<FJsonObject> SourceObject = MakeShared<FJsonObject>();
        if (!SourceInfo.bHasBackingWidget)
        {
            return SourceObject;
        }

        if (!SourceInfo.WidgetBlueprintPath.IsEmpty())
        {
            SourceObject->SetStringField(TEXT("widget_bp"), SourceInfo.WidgetBlueprintPath);
        }
        if (!SourceInfo.WidgetClassPath.IsEmpty())
        {
            SourceObject->SetStringField(TEXT("widget_class"), SourceInfo.WidgetClassPath);
        }
        if (!SourceInfo.WidgetName.IsEmpty())
        {
            SourceObject->SetStringField(TEXT("widget_name"), SourceInfo.WidgetName);
        }
        if (!SourceInfo.OwningUserWidgetName.IsEmpty())
        {
            SourceObject->SetStringField(TEXT("owning_user_widget"), SourceInfo.OwningUserWidgetName);
        }
        if (!SourceInfo.OwningUserWidgetClassPath.IsEmpty())
        {
            SourceObject->SetStringField(TEXT("owning_user_widget_class"), SourceInfo.OwningUserWidgetClassPath);
        }
        if (!SourceInfo.SourceKind.IsEmpty())
        {
            SourceObject->SetStringField(TEXT("source_kind"), SourceInfo.SourceKind);
        }
        return SourceObject;
    }

    TSharedPtr<FJsonObject> BuildSlotObject(const FLiveUiSnapshotSlotInfo& SlotInfo)
    {
        TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
        if (!SlotInfo.SlotClassName.IsEmpty())
        {
            SlotObject->SetStringField(TEXT("type"), SlotInfo.SlotClassName);
        }
        if (HasObjectFields(SlotInfo.Properties))
        {
            SlotObject->SetObjectField(TEXT("properties"), DeepCopyJsonObject(SlotInfo.Properties));
        }
        return SlotObject;
    }

    TArray<TSharedPtr<FJsonValue>> BuildObjectArray(const TArray<TSharedPtr<FJsonObject>>& Objects)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Objects.Num());
        for (const TSharedPtr<FJsonObject>& Object : Objects)
        {
            if (HasObjectFields(Object))
            {
                Values.Add(MakeShared<FJsonValueObject>(DeepCopyJsonObject(Object)));
            }
        }
        return Values;
    }

    void AccumulateSummary(
        const FLiveUiSnapshotNode& Node,
        const int32 Depth,
        const bool bGeometryIncluded,
        FLiveUiSnapshotSummary& Summary)
    {
        ++Summary.TotalNodeCount;
        Summary.MaxDepth = FMath::Max(Summary.MaxDepth, Depth);
        if (Node.SourceInfo.bHasBackingWidget)
        {
            ++Summary.BackingWidgetNodeCount;
        }
        if (bGeometryIncluded)
        {
            ++Summary.GeometryNodeCount;
        }

        for (const FLiveUiSnapshotNode& Child : Node.Children)
        {
            AccumulateSummary(Child, Depth + 1, bGeometryIncluded, Summary);
        }
    }

    TSharedPtr<FJsonObject> BuildSummaryObject(const FLiveUiSnapshotSummary& Summary)
    {
        TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
        SummaryObject->SetNumberField(TEXT("total_node_count"), Summary.TotalNodeCount);
        SummaryObject->SetNumberField(TEXT("backing_widget_node_count"), Summary.BackingWidgetNodeCount);
        SummaryObject->SetNumberField(TEXT("geometry_node_count"), Summary.GeometryNodeCount);
        SummaryObject->SetNumberField(TEXT("max_depth"), Summary.MaxDepth);
        return SummaryObject;
    }

    TSharedPtr<FJsonObject> BuildNodeObject(const FLiveUiSnapshotNode& Node, const bool bGeometryIncluded)
    {
        TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
        NodeObject->SetStringField(TEXT("slate_type"), Node.SlateType);
        NodeObject->SetStringField(TEXT("debug_name"), Node.DebugName);
        NodeObject->SetObjectField(TEXT("runtime_state"), BuildRuntimeStateObject(Node.RuntimeState));
        NodeObject->SetObjectField(TEXT("source"), BuildSourceObject(Node.SourceInfo));
        NodeObject->SetObjectField(TEXT("slot"), BuildSlotObject(Node.SlotInfo));
        NodeObject->SetObjectField(
            TEXT("properties"),
            DeepCopyJsonObject(Node.Properties));
        NodeObject->SetArrayField(TEXT("bindings"), BuildObjectArray(Node.Bindings));
        NodeObject->SetArrayField(TEXT("delegates"), BuildObjectArray(Node.Delegates));
        NodeObject->SetObjectField(
            TEXT("geometry"),
            bGeometryIncluded ? BuildGeometryObject(Node.RuntimeState) : MakeShared<FJsonObject>());

        TArray<TSharedPtr<FJsonValue>> Children;
        Children.Reserve(Node.Children.Num());
        for (const FLiveUiSnapshotNode& Child : Node.Children)
        {
            Children.Add(MakeShared<FJsonValueObject>(BuildNodeObject(Child, bGeometryIncluded)));
        }
        NodeObject->SetArrayField(TEXT("children"), Children);

        return NodeObject;
    }
}

TSharedPtr<FJsonObject> FLiveUiSnapshotJsonWriter::Write(const FLiveUiSnapshot& Snapshot)
{
    FLiveUiSnapshotSummary Summary;
    AccumulateSummary(Snapshot.RootNode, 0, Snapshot.bGeometryIncluded, Summary);

    TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
    RootObject->SetStringField(
        TEXT("capture_source"),
        Snapshot.CaptureSource.IsEmpty() ? TEXT("live") : Snapshot.CaptureSource);
    RootObject->SetBoolField(TEXT("verbose"), Snapshot.bVerbose);
    RootObject->SetBoolField(TEXT("geometry_included"), Snapshot.bGeometryIncluded);
    RootObject->SetObjectField(TEXT("viewport_size"), BuildSizeObject(Snapshot.ViewportSize));

    // Echo which live UMG root was selected so a caller that disambiguated a
    // multi-root viewport can confirm the chosen root and total count.
    RootObject->SetStringField(TEXT("selected_root_name"), Snapshot.SelectedRootName);
    RootObject->SetNumberField(TEXT("selected_root_index"), Snapshot.SelectedRootIndex);
    RootObject->SetNumberField(TEXT("root_candidate_count"), Snapshot.RootCandidateCount);
    RootObject->SetObjectField(TEXT("snapshot_summary"), BuildSummaryObject(Summary));
    RootObject->SetObjectField(TEXT("root"), BuildNodeObject(Snapshot.RootNode, Snapshot.bGeometryIncluded));
    return RootObject;
}
