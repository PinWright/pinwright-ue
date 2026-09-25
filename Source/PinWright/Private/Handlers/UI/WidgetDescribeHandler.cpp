// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetDescribeHandler.cpp
// Returns a structured JSON description of a widget blueprint's widget tree
// with overridden properties, slot info, and bindings per node.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/LiveUiSnapshot.h"
#include "Handlers/UI/LiveUiSnapshotJsonWriter.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "Handlers/UI/WidgetInspectHelpers.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Utils/PropertyUtils.h"
#include "Utils/PropertyInspection.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "UObject/UnrealType.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;

namespace
{
    using WidgetInspectHelpers::StripClassPrefix;
    using WidgetInspectHelpers::ShouldDescribeProperty;

    TSharedPtr<FJsonObject> CollectOverriddenProperties(
        UObject* Instance,
        UObject* CDO,
        bool bIncludeDefaults,
        bool bIncludeMetadata)
    {
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        if (!Instance) return PropsObj;

        for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!ShouldDescribeProperty(Property)) continue;

            const void* InstancePtr = Property->ContainerPtrToValuePtr<void>(Instance);

            if (CDO && !bIncludeDefaults)
            {
                const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(CDO);
                if (Property->Identical(InstancePtr, DefaultPtr, PPF_None))
                {
                    continue;
                }
            }

            TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(Instance, Property);
            if (!Value.IsValid()) continue;

            if (bIncludeMetadata)
            {
                TSharedPtr<FJsonObject> Wrapped = MakeShared<FJsonObject>();
                Wrapped->SetField(TEXT("value"), Value);
                Wrapped->SetStringField(TEXT("type"), GetPropertyCppTypeWithParams(Property));
                PropsObj->SetObjectField(Property->GetName(), Wrapped);
            }
            else
            {
                PropsObj->SetField(Property->GetName(), Value);
            }
        }
        return PropsObj;
    }

    // Build a JSON object describing per-node resolved geometry.
    // Returns a valid object regardless of match status (never returns nullptr).
    TSharedPtr<FJsonObject> BuildGeometryNode(const FResolvedGeometry& Geo)
    {
        TSharedPtr<FJsonObject> GeoObj = MakeShared<FJsonObject>();

        switch (Geo.Status)
        {
        case FResolvedGeometry::EStatus::Ok:
            GeoObj->SetStringField(TEXT("status"), TEXT("ok"));
            break;
        case FResolvedGeometry::EStatus::Skipped:
            GeoObj->SetStringField(TEXT("status"), TEXT("skipped"));
            GeoObj->SetStringField(TEXT("reason"), Geo.Reason);
            break;
        case FResolvedGeometry::EStatus::Error:
        default:
            GeoObj->SetStringField(TEXT("status"), TEXT("error"));
            GeoObj->SetStringField(TEXT("reason"), Geo.Reason);
            break;
        }

        if (Geo.Status == FResolvedGeometry::EStatus::Ok)
        {
            // absolute: {x, y, w, h}
            TSharedPtr<FJsonObject> AbsObj = MakeShared<FJsonObject>();
            AbsObj->SetNumberField(TEXT("x"), Geo.AbsolutePos.X);
            AbsObj->SetNumberField(TEXT("y"), Geo.AbsolutePos.Y);
            AbsObj->SetNumberField(TEXT("w"), Geo.AbsoluteSize.X);
            AbsObj->SetNumberField(TEXT("h"), Geo.AbsoluteSize.Y);
            GeoObj->SetObjectField(TEXT("absolute"), AbsObj);

            // local: {w, h}
            TSharedPtr<FJsonObject> LocalObj = MakeShared<FJsonObject>();
            LocalObj->SetNumberField(TEXT("w"), Geo.LocalSize.X);
            LocalObj->SetNumberField(TEXT("h"), Geo.LocalSize.Y);
            GeoObj->SetObjectField(TEXT("local"), LocalObj);

            // desired_size: {w, h}
            TSharedPtr<FJsonObject> DesiredObj = MakeShared<FJsonObject>();
            DesiredObj->SetNumberField(TEXT("w"), Geo.DesiredSize.X);
            DesiredObj->SetNumberField(TEXT("h"), Geo.DesiredSize.Y);
            GeoObj->SetObjectField(TEXT("desired_size"), DesiredObj);

            GeoObj->SetStringField(TEXT("visibility"), Geo.EffectiveVisibility.ToString());

            // render_transform (only when non-identity)
            if (Geo.bHasRenderTransform)
            {
                const FWidgetTransform& WT = Geo.RenderTransformValue;
                TSharedPtr<FJsonObject> RtObj = MakeShared<FJsonObject>();

                TSharedPtr<FJsonObject> Trans = MakeShared<FJsonObject>();
                Trans->SetNumberField(TEXT("x"), WT.Translation.X);
                Trans->SetNumberField(TEXT("y"), WT.Translation.Y);
                RtObj->SetObjectField(TEXT("translation"), Trans);

                TSharedPtr<FJsonObject> Scale = MakeShared<FJsonObject>();
                Scale->SetNumberField(TEXT("x"), WT.Scale.X);
                Scale->SetNumberField(TEXT("y"), WT.Scale.Y);
                RtObj->SetObjectField(TEXT("scale"), Scale);

                TSharedPtr<FJsonObject> Shear = MakeShared<FJsonObject>();
                Shear->SetNumberField(TEXT("x"), WT.Shear.X);
                Shear->SetNumberField(TEXT("y"), WT.Shear.Y);
                RtObj->SetObjectField(TEXT("shear"), Shear);

                RtObj->SetNumberField(TEXT("angle_deg"), WT.Angle);

                GeoObj->SetObjectField(TEXT("render_transform"), RtObj);
            }
        }

        return GeoObj;
    }

    TSharedPtr<FJsonObject> MakeErrorGeo(const FString& Reason)
    {
        TSharedPtr<FJsonObject> GeoObj = MakeShared<FJsonObject>();
        GeoObj->SetStringField(TEXT("status"), TEXT("error"));
        GeoObj->SetStringField(TEXT("reason"), Reason);
        return GeoObj;
    }
}

// ---- widget.describe ----
REGISTER_RPC_HANDLER("widget.describe", "widget",
    "Describe a widget blueprint: tree with overridden properties, slot info, and bindings per widget",
        RPC_PARAMS(
        WidgetAssetPathParamOpt(TEXT("Widget blueprint asset path when capture_source=asset")),
        RPC_PARAM_OPT("asset_path", "path",
            "Legacy alias for widgetPath when capture_source=asset"),
        RPC_PARAM_OPT("include_defaults", "boolean",
            "Include all properties, not just overridden (default false)"),
        RPC_PARAM_OPT("include_slot", "boolean",
            "Include slot/layout properties (default true)"),
        RPC_PARAM_OPT("include_bindings", "boolean",
            "Include property bindings and delegates per widget (default true)"),
        RPC_PARAM_OPT("max_depth", "number",
            "Max tree depth, 0 = unlimited (default 0)"),
        RPC_PARAM_OPT("widgetName", "string",
            "Describe only the subtree rooted at this widget (canonical; aliases: name, slotName, widget_name, targetName)"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("include_metadata", "boolean",
            "Include property type info alongside values (default false)"),
        RPC_PARAM_OPT("resolve_geometry", "bool|object",
            "Resolve window-relative geometry (off by default). Pass bool true to enable with defaults, "
            "or object {viewport_size: {w,h}, force_visible_for_measure: bool, instance_name: string} for fine control."),
        RPC_PARAM_OPT("resolveGeometry", "bool|object",
            "Alias for resolve_geometry"),
        RPC_PARAM_OPT("capture_source", "string",
            "Capture source: 'asset' (default) or 'live' for the active PIE UMG-as-root subtree"),
        RPC_PARAM_OPT("verbose", "boolean",
            "When capture_source=live, include default-valued runtime state (default false)"),
        RPC_PARAM_OPT("include_geometry", "boolean",
            "When capture_source=live, include live geometry in the snapshot (default false)"),
        RPC_PARAM_OPT("instance_name", "string",
            "When capture_source=live and multiple UMG roots are on screen, select the root "
            "whose backing widget name contains this value (e.g. the key ui.create_hud returns, "
            "WBP_PlayerHUD_StateTree_C_0). Resolves AMBIGUOUS_LIVE_ROOT."),
        RPC_PARAM_OPT("instanceName", "string", "Alias for instance_name"),
        RPC_PARAM_OPT("root_index", "integer",
            "When capture_source=live, select the Nth live UMG root (0-based, in collection order). "
            "Alternative to instance_name for disambiguating AMBIGUOUS_LIVE_ROOT."),
        RPC_PARAM_OPT("rootIndex", "integer", "Alias for root_index")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> AssetPathKeys = WidgetAssetPathParamNames();
    AssetPathKeys.Add(TEXT("asset_path"));
    const bool bHasCaptureSource = Payload.IsValid() && Payload->HasField(TEXT("capture_source"));
    const FString RequestedCaptureSource = bHasCaptureSource
        ? Ctx.GetString(TEXT("capture_source"))
        : FString();
    if (bHasCaptureSource && RequestedCaptureSource.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            TEXT("capture_source must be 'asset' or 'live'"));
        return true;
    }

    const FString CaptureSource = bHasCaptureSource
        ? RequestedCaptureSource
        : TEXT("asset");
    const FString AssetPath = Ctx.GetStringFirstOf(AssetPathKeys);

    if (CaptureSource != TEXT("asset") && CaptureSource != TEXT("live"))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            TEXT("capture_source must be 'asset' or 'live'"));
        return true;
    }

    if (CaptureSource == TEXT("live"))
    {
        for (const FString& AssetPathKey : AssetPathKeys)
        {
            if (Payload.IsValid() && Payload->HasField(AssetPathKey))
            {
                Ctx.SendError(TEXT("INVALID_PARAMETER"),
                    TEXT("widgetPath, assetPath, path, blueprintPath, blueprint_path, requestedPath, and asset_path are not accepted when capture_source=live"));
                return true;
            }
        }

        const FLiveUiSnapshotRequest Request = FLiveUiSnapshotRequest::FromContext(Ctx);

        FLiveUiSnapshot Snapshot;
        FString ErrorCode;
        FString ErrorMessage;
        if (!FLiveUiSnapshotService::Capture(Request, Snapshot, ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }

        Ctx.SendSuccess(FLiveUiSnapshotJsonWriter::Write(Snapshot));
        return true;
    }

    if (Payload.IsValid() &&
        (Payload->HasField(TEXT("verbose")) || Payload->HasField(TEXT("include_geometry"))))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            TEXT("verbose and include_geometry are only accepted when capture_source=live"));
        return true;
    }

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"),
            TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    const bool bIncludeDefaults  = Ctx.GetBool(TEXT("include_defaults"), false);
    const bool bIncludeSlot      = Ctx.GetBool(TEXT("include_slot"), true);
    const bool bIncludeBindings  = Ctx.GetBool(TEXT("include_bindings"), true);
    const int32 MaxDepth         = Ctx.GetInt(TEXT("max_depth"), 0);
    const FString SubtreeRoot    = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("widget_name"), TEXT("name"), TEXT("slotName"), TEXT("targetName")});
    const bool bIncludeMetadata  = Ctx.GetBool(TEXT("include_metadata"), false);

    TSharedPtr<FJsonValue> GeoValue = Ctx.GetJsonValueFirstOf(
        {TEXT("resolve_geometry"), TEXT("resolveGeometry")});

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(AssetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* StartWidget = WidgetBP->WidgetTree->RootWidget;
    if (!SubtreeRoot.IsEmpty())
    {
        StartWidget = WidgetBP->WidgetTree->FindWidget(FName(*SubtreeRoot));
        if (!StartWidget)
        {
            Ctx.SendError(TEXT("WIDGET_NOT_FOUND"),
                FString::Printf(TEXT("Widget '%s' not found in tree"), *SubtreeRoot));
            return true;
        }
    }
    if (!StartWidget)
    {
        UClass* NativeParent = WidgetBP->ParentClass;
        while (NativeParent && NativeParent->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
            NativeParent = NativeParent->GetSuperClass();
        if (NativeParent && NativeParent != UUserWidget::StaticClass())
        {
            Ctx.SendError(TEXT("TREE_PROGRAMMATIC"),
                FString::Printf(TEXT("Widget tree is empty — parent class '%s' likely builds its tree programmatically in C++"),
                    *NativeParent->GetName()));
            return true;
        }
        Ctx.SendError(TEXT("TREE_EMPTY"), TEXT("Widget tree has no root widget"));
        return true;
    }

    FWidgetGeometryRequest GeoReq;
    const bool bGeometryEnabled = FWidgetGeometryResolver::ParseRequest(GeoValue, WidgetBP, GeoReq);

    FWidgetGeometryResult GeoResult;
    TMap<FName, UWidget*> GeoNameIndex;
    if (bGeometryEnabled)
    {
        GeoResult = FWidgetGeometryResolver::Resolve(GeoReq);
        FWidgetGeometryResolver::BuildNameIndex(GeoResult.GetLiveRoot(), GeoNameIndex);
    }
    const bool bGeoAmbiguous = GeoResult.bAmbiguous;

    // ---- Binding resolution (absorbed from widget.get_widget_tree) ----
    TMap<FString, TArray<TSharedPtr<FJsonValue>>> WidgetBindingsByName;
    TMap<FString, TArray<TSharedPtr<FJsonValue>>> WidgetDelegatesByName;
    TMap<FString, TSharedPtr<FJsonObject>> BindingTargetsByFunctionName;
    int32 BindingTargetCount = 0;

    if (bIncludeBindings)
    {
        auto ReadStructField = [](UScriptStruct* StructType, const void* StructData, const TCHAR* FieldName) -> FString
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

        // Build binding targets from all graphs
        TArray<UEdGraph*> WidgetGraphs;
        WidgetBP->GetAllGraphs(WidgetGraphs);
        for (UEdGraph* Graph : WidgetGraphs)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                FString FunctionName;
                FString EntryType;
                if (Cast<UK2Node_FunctionEntry>(Node))
                {
                    FunctionName = Graph->GetName();
                    EntryType = TEXT("function");
                }
                else if (Cast<UK2Node_CustomEvent>(Node))
                {
                    FunctionName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
                    EntryType = TEXT("customEvent");
                }
                else if (Cast<UK2Node_Event>(Node))
                {
                    FunctionName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
                    EntryType = TEXT("event");
                }
                if (FunctionName.IsEmpty()) continue;
                const FString Key = FunctionName.ToLower();
                if (BindingTargetsByFunctionName.Contains(Key)) continue;

                TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
                TargetObj->SetStringField(TEXT("functionName"), FunctionName);
                TargetObj->SetStringField(TEXT("entryType"), EntryType);
                TargetObj->SetStringField(TEXT("graphName"), Graph->GetName());
                TargetObj->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
                BindingTargetsByFunctionName.Add(Key, TargetObj);
                ++BindingTargetCount;
            }
        }

        // Read the Bindings array via reflection
        if (FArrayProperty* BindingsArrayProp = FindFProperty<FArrayProperty>(WidgetBP->GetClass(), TEXT("Bindings")))
        {
            void* BindingsArrayPtr = BindingsArrayProp->ContainerPtrToValuePtr<void>(WidgetBP);
            FScriptArrayHelper BindingsHelper(BindingsArrayProp, BindingsArrayPtr);
            if (FStructProperty* BindingStructProp = CastField<FStructProperty>(BindingsArrayProp->Inner))
            {
                UScriptStruct* BindingStruct = BindingStructProp->Struct;
                for (int32 Index = 0; Index < BindingsHelper.Num(); ++Index)
                {
                    const void* BindingData = BindingsHelper.GetRawPtr(Index);
                    const FString ObjectName = ReadStructField(BindingStruct, BindingData, TEXT("ObjectName"));
                    const FString PropertyName = ReadStructField(BindingStruct, BindingData, TEXT("PropertyName"));
                    const FString FunctionName = ReadStructField(BindingStruct, BindingData, TEXT("FunctionName"));
                    const FString WidgetNameKey = ObjectName.IsEmpty() ? TEXT("__root__") : ObjectName;

                    TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
                    BindingObj->SetStringField(TEXT("propertyName"), PropertyName);
                    BindingObj->SetStringField(TEXT("functionName"), FunctionName);
                    if (const TSharedPtr<FJsonObject>* BoundTarget = BindingTargetsByFunctionName.Find(FunctionName.ToLower()))
                    {
                        BindingObj->SetObjectField(TEXT("graphBinding"), *BoundTarget);
                    }

                    if (PropertyName.StartsWith(TEXT("On")))
                        WidgetDelegatesByName.FindOrAdd(WidgetNameKey).Add(MakeShared<FJsonValueObject>(BindingObj));
                    else
                        WidgetBindingsByName.FindOrAdd(WidgetNameKey).Add(MakeShared<FJsonValueObject>(BindingObj));
                }
            }
        }
    }

    int32 WidgetCount = 0;

    int32 GeoCountOk = 0;
    int32 GeoCountSkipped = 0;
    int32 GeoCountError = 0;

    TFunction<TSharedPtr<FJsonObject>(UWidget*, int32)> BuildNode;
    BuildNode = [&](UWidget* Widget, int32 Depth) -> TSharedPtr<FJsonObject>
    {
        if (!Widget) return nullptr;
        ++WidgetCount;

        const FString WidgetName = Widget->GetFName().ToString();
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("type"), StripClassPrefix(Widget->GetClass()->GetName()));
        Node->SetStringField(TEXT("name"), WidgetName);
        // UWidget::bIsVariable is the variable test on every supported engine (5.3-5.8), and it is
        // the one FWidgetBlueprintCompilerContext itself uses for bShouldGenerateVariable and
        // CPF_BlueprintVisible. WidgetVariableNameToGuidMap (5.6+) is NOT that set: the compiler
        // populates it from ForEachSourceWidget with no bIsVariable test, so after any compile it
        // holds every widget in the tree.
        const bool bWidgetIsVariable = Widget->bIsVariable;
        Node->SetBoolField(TEXT("isVariable"), bWidgetIsVariable);

        // Widget properties (instance vs CDO)
        UObject* WidgetCDO = Widget->GetClass()->GetDefaultObject();
        Node->SetObjectField(TEXT("props"),
            CollectOverriddenProperties(Widget, WidgetCDO, bIncludeDefaults, bIncludeMetadata));

        // Slot properties
        if (bIncludeSlot && Widget->Slot)
        {
            UPanelSlot* Slot = Widget->Slot;
            TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
            SlotObj->SetStringField(TEXT("type"), StripClassPrefix(Slot->GetClass()->GetName()));
            UObject* SlotCDO = Slot->GetClass()->GetDefaultObject();
            SlotObj->SetObjectField(TEXT("props"),
                CollectOverriddenProperties(Slot, SlotCDO, bIncludeDefaults, bIncludeMetadata));
            Node->SetObjectField(TEXT("slot"), SlotObj);
        }
        else
        {
            Node->SetField(TEXT("slot"), MakeShared<FJsonValueNull>());
        }

        // Bindings and delegates per widget
        if (bIncludeBindings)
        {
            const TArray<TSharedPtr<FJsonValue>>* Bindings = WidgetBindingsByName.Find(WidgetName);
            const TArray<TSharedPtr<FJsonValue>>* Delegates = WidgetDelegatesByName.Find(WidgetName);
            if (Bindings && Bindings->Num() > 0)
                Node->SetArrayField(TEXT("bindings"), *Bindings);
            if (Delegates && Delegates->Num() > 0)
                Node->SetArrayField(TEXT("delegates"), *Delegates);
        }

        if (bGeometryEnabled && !bGeoAmbiguous)
        {
            TSharedPtr<FJsonObject> GeoNode;

            if (GeoNameIndex.Num() > 0)
            {
                if (const FResolvedGeometry* Found = FWidgetGeometryResolver::LookupByName(
                        GeoResult.ByWidget, GeoNameIndex, Widget->GetFName()))
                {
                    GeoNode = BuildGeometryNode(*Found);
                    switch (Found->Status)
                    {
                    case FResolvedGeometry::EStatus::Ok:      ++GeoCountOk;      break;
                    case FResolvedGeometry::EStatus::Skipped: ++GeoCountSkipped; break;
                    default:                                  ++GeoCountError;   break;
                    }
                }
                else
                {
                    GeoNode = MakeErrorGeo(TEXT("NOT_REACHED"));
                    ++GeoCountError;
                }
            }
            else
            {
                GeoNode = MakeErrorGeo(
                    GeoResult.TopLevelError.IsEmpty() ? FString(TEXT("NOT_REACHED"))
                                                       : GeoResult.TopLevelError);
                ++GeoCountError;
            }

            Node->SetObjectField(TEXT("geometry"), GeoNode);
        }

        // Children
        TArray<TSharedPtr<FJsonValue>> Children;
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            const bool bCanRecurse = (MaxDepth == 0) || (Depth < MaxDepth);
            if (bCanRecurse)
            {
                const int32 ChildCount = Panel->GetChildrenCount();
                for (int32 i = 0; i < ChildCount; ++i)
                {
                    if (UWidget* Child = Panel->GetChildAt(i))
                    {
                        TSharedPtr<FJsonObject> ChildNode = BuildNode(Child, Depth + 1);
                        if (ChildNode.IsValid())
                        {
                            Children.Add(MakeShared<FJsonValueObject>(ChildNode));
                        }
                    }
                }
            }
        }
        Node->SetArrayField(TEXT("children"), Children);

        return Node;
    };

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    if (WidgetBP->ParentClass)
    {
        Result->SetStringField(TEXT("root_class"), WidgetBP->ParentClass->GetName());
    }
    Result->SetObjectField(TEXT("tree"), BuildNode(StartWidget, 1));
    Result->SetNumberField(TEXT("widget_count"), WidgetCount);

    if (bIncludeBindings)
    {
        TArray<TSharedPtr<FJsonValue>> BindingTargetsArray;
        for (const auto& Pair : BindingTargetsByFunctionName)
        {
            BindingTargetsArray.Add(MakeShared<FJsonValueObject>(Pair.Value));
        }
        Result->SetArrayField(TEXT("bindingTargets"), BindingTargetsArray);
    }

    // ---- Top-level geometry metadata ----
    if (bGeometryEnabled)
    {
        if (bGeoAmbiguous)
        {
            Result->SetStringField(TEXT("geometry_source"), TEXT("ambiguous"));
            Result->SetBoolField(TEXT("ambiguous"), true);
            Result->SetStringField(TEXT("top_level_error"), TEXT("AMBIGUOUS_INSTANCE"));

            TArray<TSharedPtr<FJsonValue>> CandidatesArray;
            for (const TWeakObjectPtr<UUserWidget>& Candidate : GeoResult.AmbiguousCandidates)
            {
                if (!Candidate.IsValid()) continue;
                TSharedPtr<FJsonObject> CandObj = MakeShared<FJsonObject>();
                CandObj->SetStringField(TEXT("name"), Candidate->GetName());
                if (UObject* Outer = Candidate->GetOuter())
                    CandObj->SetStringField(TEXT("outer"), Outer->GetName());
                CandidatesArray.Add(MakeShared<FJsonValueObject>(CandObj));
            }
            Result->SetArrayField(TEXT("candidates"), CandidatesArray);
        }
        else
        {
            Result->SetStringField(TEXT("geometry_source"),
                FWidgetGeometryResolver::SourceToString(GeoResult.ServedBy));

            {
                TSharedPtr<FJsonObject> VpObj = MakeShared<FJsonObject>();
                VpObj->SetNumberField(TEXT("w"), GeoResult.ViewportSizeUsed.X);
                VpObj->SetNumberField(TEXT("h"), GeoResult.ViewportSizeUsed.Y);
                Result->SetObjectField(TEXT("viewport_size"), VpObj);
            }

            Result->SetNumberField(TEXT("dpi_scale"), GeoResult.DpiScale);

            if (!GeoResult.TopLevelError.IsEmpty())
            {
                Result->SetStringField(TEXT("top_level_error"), GeoResult.TopLevelError);
            }

            {
                const int32 GeoTotal = GeoCountOk + GeoCountSkipped + GeoCountError;
                TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
                SummaryObj->SetNumberField(TEXT("ok"),      GeoCountOk);
                SummaryObj->SetNumberField(TEXT("skipped"), GeoCountSkipped);
                SummaryObj->SetNumberField(TEXT("error"),   GeoCountError);
                SummaryObj->SetNumberField(TEXT("total"),   GeoTotal);
                Result->SetObjectField(TEXT("geometry_summary"), SummaryObj);
            }
        }
    }
    else
    {
        Result->SetStringField(TEXT("geometry_source"), TEXT("off"));
    }

    Ctx.SendSuccess(Result);
    return true;
}
