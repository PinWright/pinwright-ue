// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/LiveUiSnapshot.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetInspectHelpers.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Containers/ScriptArray.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Layout/Clipping.h"
#include "Utils/PropertyUtils.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Widgets/SWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
    using WidgetInspectHelpers::StripClassPrefix;
    using WidgetInspectHelpers::ShouldDescribeProperty;

    struct FWidgetBindingMetadata
    {
        TMap<FString, TArray<TSharedPtr<FJsonObject>>> BindingsByWidgetName;
        TMap<FString, TArray<TSharedPtr<FJsonObject>>> DelegatesByWidgetName;
    };

    FString WidgetClippingToString(const EWidgetClipping Clipping)
    {
        switch (Clipping)
        {
        case EWidgetClipping::Inherit:
            return TEXT("Inherit");
        case EWidgetClipping::ClipToBounds:
            return TEXT("ClipToBounds");
        case EWidgetClipping::ClipToBoundsWithoutIntersecting:
            return TEXT("ClipToBoundsWithoutIntersecting");
        case EWidgetClipping::ClipToBoundsAlways:
            return TEXT("ClipToBoundsAlways");
        case EWidgetClipping::OnDemand:
            return TEXT("OnDemand");
        default:
            return TEXT("Unknown");
        }
    }

    TSharedPtr<FJsonObject> CollectOverriddenProperties(UObject* Instance, UObject* CDO, const bool bIncludeDefaults)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        if (!Instance)
        {
            return Properties;
        }

        for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!ShouldDescribeProperty(Property))
            {
                continue;
            }

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
            if (!Value.IsValid())
            {
                continue;
            }

            Properties->SetField(Property->GetName(), Value);
        }

        return Properties;
    }

    TSharedPtr<FJsonObject> CollectLiveWidgetProperties(UWidget* Widget, const bool bIncludeDefaults)
    {
        if (!Widget)
        {
            return nullptr;
        }

        UObject* WidgetCDO = Widget->GetClass()->GetDefaultObject();
        TSharedPtr<FJsonObject> Properties = CollectOverriddenProperties(Widget, WidgetCDO, bIncludeDefaults);
        if (!bIncludeDefaults && Properties->Values.Num() == 0)
        {
            return nullptr;
        }

        return Properties;
    }

    FLiveUiSnapshotSlotInfo CollectLiveSlotInfo(UPanelSlot* Slot, const bool bIncludeDefaults)
    {
        FLiveUiSnapshotSlotInfo SlotInfo;
        if (!Slot)
        {
            return SlotInfo;
        }

        SlotInfo.SlotClassName = StripClassPrefix(Slot->GetClass()->GetName());

        UObject* SlotCDO = Slot->GetClass()->GetDefaultObject();
        SlotInfo.Properties = CollectOverriddenProperties(Slot, SlotCDO, bIncludeDefaults);
        if (!bIncludeDefaults && SlotInfo.Properties.IsValid() && SlotInfo.Properties->Values.Num() == 0)
        {
            SlotInfo.Properties.Reset();
        }

        return SlotInfo;
    }

    FString ReadStructFieldAsString(UScriptStruct* StructType, const void* StructData, const TCHAR* FieldName)
    {
        if (!StructType || !StructData)
        {
            return FString();
        }

        FProperty* Field = StructType->FindPropertyByName(FName(FieldName));
        if (!Field)
        {
            return FString();
        }

        const void* ValuePtr = Field->ContainerPtrToValuePtr<void>(StructData);
        if (!ValuePtr)
        {
            return FString();
        }

        FString OutValue;
        if (const FNameProperty* NameProperty = CastField<FNameProperty>(Field))
        {
            OutValue = NameProperty->GetPropertyValue(ValuePtr).ToString();
        }
        else if (const FStrProperty* StringProperty = CastField<FStrProperty>(Field))
        {
            OutValue = StringProperty->GetPropertyValue(ValuePtr);
        }
        else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Field))
        {
            OutValue = TextProperty->GetPropertyValue(ValuePtr).ToString();
        }
        else
        {
            Field->ExportTextItem_Direct(OutValue, ValuePtr, nullptr, nullptr, PPF_None);
        }

        return OutValue;
    }

    UWidgetBlueprint* ResolveBackingWidgetBlueprint(UWidget* Widget)
    {
        if (!Widget)
        {
            return nullptr;
        }

        if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Widget->GetClass()->ClassGeneratedBy))
        {
            return WidgetBlueprint;
        }

        if (UUserWidget* OwningUserWidget = Widget->GetTypedOuter<UUserWidget>())
        {
            return Cast<UWidgetBlueprint>(OwningUserWidget->GetClass()->ClassGeneratedBy);
        }

        return nullptr;
    }

    const FWidgetBindingMetadata& BuildBindingMetadata(
        UWidgetBlueprint* WidgetBlueprint,
        TMap<TWeakObjectPtr<UWidgetBlueprint>, FWidgetBindingMetadata>& MetadataCache)
    {
        static const FWidgetBindingMetadata EmptyMetadata;
        if (!WidgetBlueprint)
        {
            return EmptyMetadata;
        }

        if (const FWidgetBindingMetadata* Cached = MetadataCache.Find(WidgetBlueprint))
        {
            return *Cached;
        }

        FWidgetBindingMetadata Metadata;
        TMap<FString, TSharedPtr<FJsonObject>> BindingTargetsByFunctionName;

        TArray<UEdGraph*> WidgetGraphs;
        WidgetBlueprint->GetAllGraphs(WidgetGraphs);
        for (UEdGraph* Graph : WidgetGraphs)
        {
            if (!Graph)
            {
                continue;
            }

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }

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

                if (FunctionName.IsEmpty())
                {
                    continue;
                }

                const FString Key = FunctionName.ToLower();
                if (BindingTargetsByFunctionName.Contains(Key))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> TargetObject = MakeShared<FJsonObject>();
                TargetObject->SetStringField(TEXT("functionName"), FunctionName);
                TargetObject->SetStringField(TEXT("entryType"), EntryType);
                TargetObject->SetStringField(TEXT("graphName"), Graph->GetName());
                TargetObject->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
                BindingTargetsByFunctionName.Add(Key, TargetObject);
            }
        }

        if (FArrayProperty* BindingsArrayProperty = FindFProperty<FArrayProperty>(WidgetBlueprint->GetClass(), TEXT("Bindings")))
        {
            void* BindingsArrayPtr = BindingsArrayProperty->ContainerPtrToValuePtr<void>(WidgetBlueprint);
            if (!BindingsArrayPtr)
            {
                return MetadataCache.Add(WidgetBlueprint, MoveTemp(Metadata));
            }

            FScriptArrayHelper BindingsHelper(BindingsArrayProperty, BindingsArrayPtr);

            if (FStructProperty* BindingStructProperty = CastField<FStructProperty>(BindingsArrayProperty->Inner))
            {
                UScriptStruct* BindingStruct = BindingStructProperty->Struct;
                for (int32 Index = 0; Index < BindingsHelper.Num(); ++Index)
                {
                    const void* BindingData = BindingsHelper.GetRawPtr(Index);
                    const FString ObjectName = ReadStructFieldAsString(BindingStruct, BindingData, TEXT("ObjectName"));
                    const FString PropertyName = ReadStructFieldAsString(BindingStruct, BindingData, TEXT("PropertyName"));
                    const FString FunctionName = ReadStructFieldAsString(BindingStruct, BindingData, TEXT("FunctionName"));
                    const FString WidgetNameKey = ObjectName.IsEmpty() ? TEXT("__root__") : ObjectName;

                    TSharedPtr<FJsonObject> BindingObject = MakeShared<FJsonObject>();
                    BindingObject->SetStringField(TEXT("propertyName"), PropertyName);
                    BindingObject->SetStringField(TEXT("functionName"), FunctionName);
                    if (const TSharedPtr<FJsonObject>* BindingTarget = BindingTargetsByFunctionName.Find(FunctionName.ToLower()))
                    {
                        BindingObject->SetObjectField(TEXT("graphBinding"), *BindingTarget);
                    }

                    if (PropertyName.StartsWith(TEXT("On")))
                    {
                        Metadata.DelegatesByWidgetName.FindOrAdd(WidgetNameKey).Add(BindingObject);
                    }
                    else
                    {
                        Metadata.BindingsByWidgetName.FindOrAdd(WidgetNameKey).Add(BindingObject);
                    }
                }
            }
        }

        return MetadataCache.Add(WidgetBlueprint, MoveTemp(Metadata));
    }

    void CollectBindingMetadata(
        UWidget* Widget,
        TArray<TSharedPtr<FJsonObject>>& OutBindings,
        TArray<TSharedPtr<FJsonObject>>& OutDelegates,
        TMap<TWeakObjectPtr<UWidgetBlueprint>, FWidgetBindingMetadata>& BindingMetadataCache)
    {
        OutBindings.Reset();
        OutDelegates.Reset();

        if (!Widget)
        {
            return;
        }

        UWidgetBlueprint* WidgetBlueprint = ResolveBackingWidgetBlueprint(Widget);
        const FWidgetBindingMetadata& Metadata = BuildBindingMetadata(WidgetBlueprint, BindingMetadataCache);

        const FString WidgetName = Widget->GetName();
        if (const TArray<TSharedPtr<FJsonObject>>* Bindings = Metadata.BindingsByWidgetName.Find(WidgetName))
        {
            OutBindings.Append(*Bindings);
        }
        if (const TArray<TSharedPtr<FJsonObject>>* Delegates = Metadata.DelegatesByWidgetName.Find(WidgetName))
        {
            OutDelegates.Append(*Delegates);
        }

        if (Cast<UUserWidget>(Widget))
        {
            static const FString RootKey = TEXT("__root__");
            if (const TArray<TSharedPtr<FJsonObject>>* RootBindings = Metadata.BindingsByWidgetName.Find(RootKey))
            {
                OutBindings.Append(*RootBindings);
            }
            if (const TArray<TSharedPtr<FJsonObject>>* RootDelegates = Metadata.DelegatesByWidgetName.Find(RootKey))
            {
                OutDelegates.Append(*RootDelegates);
            }
        }
    }

    bool ValidateSlateInitialized(FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (FSlateApplication::IsInitialized())
        {
            return true;
        }

        OutErrorCode = TEXT("SLATE_NOT_INITIALIZED");
        OutErrorMessage = TEXT("Slate application is not initialized");
        return false;
    }

    UWorld* ResolvePieWorld()
    {
        if (!GEngine || !GEngine->GameViewport)
        {
            return nullptr;
        }

        UWorld* World = GEngine->GameViewport->GetWorld();
        if (!World)
        {
            return nullptr;
        }

        if (World->WorldType != EWorldType::PIE && !World->IsPlayInEditor())
        {
            return nullptr;
        }

        return World;
    }

    TSharedPtr<SWindow> ResolveGameViewportWindow()
    {
        if (!GEngine || !GEngine->GameViewport)
        {
            return nullptr;
        }

        TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
        if (!ViewportWidget.IsValid())
        {
            return nullptr;
        }

        return FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
    }

    static bool IsUmgRootCandidate(const TSharedRef<SWidget>& Widget)
    {
        const FString TypeName = Widget->GetTypeAsString();
        if (TypeName != TEXT("SConstraintCanvas"))
        {
            return false;
        }

        FChildren* Children = Widget->GetChildren();
        if (!Children || Children->Num() == 0)
        {
            return false;
        }

        return Children->GetChildAt(0)->GetTypeAsString() == TEXT("SObjectWidget");
    }

    void CollectUmgRootCandidates(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SWidget>>& OutCandidates)
    {
        if (IsUmgRootCandidate(Widget))
        {
            OutCandidates.Add(Widget);
            return;
        }

        FChildren* Children = Widget->GetChildren();
        if (!Children)
        {
            return;
        }

        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            CollectUmgRootCandidates(Children->GetChildAt(Index), OutCandidates);
        }
    }

    void BuildBackingWidgetMap(UWorld* World, TMap<SWidget*, UWidget*>& OutMap)
    {
        if (!World)
        {
            return;
        }

        TArray<UUserWidget*> UserWidgets;
        UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, UserWidgets, UUserWidget::StaticClass(), false);

        TFunction<void(UWidget*)> VisitWidget = [&](UWidget* Widget)
        {
            if (!Widget)
            {
                return;
            }

            TSharedPtr<SWidget> Cached = Widget->GetCachedWidget();
            if (Cached.IsValid())
            {
                OutMap.Add(Cached.Get(), Widget);
            }
        };

        for (UUserWidget* UserWidget : UserWidgets)
        {
            if (!UserWidget)
            {
                continue;
            }

            // Include the UUserWidget itself so the live SObjectWidget host resolves back to it.
            VisitWidget(UserWidget);

            if (UserWidget->WidgetTree)
            {
                UserWidget->WidgetTree->ForEachWidget([&VisitWidget](UWidget* Widget)
                {
                    VisitWidget(Widget);
                });
            }
        }
    }

    void PopulateSourceInfo(UWidget* Widget, FLiveUiSnapshotSourceInfo& OutSourceInfo)
    {
        if (!Widget)
        {
            return;
        }

        OutSourceInfo.bHasBackingWidget = true;
        OutSourceInfo.WidgetName = Widget->GetName();
        OutSourceInfo.SourceKind = TEXT("UWidget");

        if (UClass* WidgetClass = Widget->GetClass())
        {
            OutSourceInfo.WidgetClassPath = WidgetClass->GetPathName();

            if (WidgetClass->ClassGeneratedBy)
            {
                OutSourceInfo.WidgetBlueprintPath = WidgetClass->ClassGeneratedBy->GetPathName();
            }
        }

        UUserWidget* OwningUserWidget = Cast<UUserWidget>(Widget);
        if (!OwningUserWidget)
        {
            OwningUserWidget = Widget->GetTypedOuter<UUserWidget>();
        }

        if (!OwningUserWidget)
        {
            return;
        }

        OutSourceInfo.OwningUserWidgetName = OwningUserWidget->GetName();

        if (UClass* OwningClass = OwningUserWidget->GetClass())
        {
            OutSourceInfo.OwningUserWidgetClassPath = OwningClass->GetPathName();

            if (OutSourceInfo.WidgetBlueprintPath.IsEmpty() && OwningClass->ClassGeneratedBy)
            {
                OutSourceInfo.WidgetBlueprintPath = OwningClass->ClassGeneratedBy->GetPathName();
            }
        }
    }

    FLiveUiSnapshotNode BuildSnapshotNode(
        const TSharedRef<SWidget>& SlateWidget,
        const TMap<SWidget*, UWidget*>& BackingMap,
        const FLiveUiSnapshotRequest& Request,
        TMap<TWeakObjectPtr<UWidgetBlueprint>, FWidgetBindingMetadata>& BindingMetadataCache)
    {
        FLiveUiSnapshotNode Node;
        Node.SlateType = SlateWidget->GetTypeAsString();
        Node.DebugName = SlateWidget->ToString();

        Node.RuntimeState.Visibility = SlateWidget->GetVisibility().ToString();
        Node.RuntimeState.bEnabled = SlateWidget->IsEnabled();
        Node.RuntimeState.bFocused = SlateWidget->HasAnyUserFocus().IsSet();
        Node.RuntimeState.bFocusable = SlateWidget->SupportsKeyboardFocus();
        Node.RuntimeState.Clipping = WidgetClippingToString(SlateWidget->GetClipping());
        Node.RuntimeState.bVolatile = SlateWidget->IsVolatile();
        Node.RuntimeState.DesiredSize = SlateWidget->GetDesiredSize();

        if (Request.bIncludeGeometry)
        {
            const FGeometry& CachedGeometry = SlateWidget->GetCachedGeometry();
            Node.RuntimeState.AbsolutePosition = CachedGeometry.GetAbsolutePosition();
            Node.RuntimeState.AbsoluteSize = CachedGeometry.GetAbsoluteSize();
        }

        if (!Request.bVerbose)
        {
            if (Node.RuntimeState.Visibility == TEXT("Visible"))
            {
                Node.RuntimeState.Visibility.Reset();
            }
            if (Node.RuntimeState.Clipping == TEXT("Inherit"))
            {
                Node.RuntimeState.Clipping.Reset();
            }
            if (Node.RuntimeState.bEnabled.Get(false))
            {
                Node.RuntimeState.bEnabled.Reset();
            }
        }

        if (UWidget* const* BackingWidget = BackingMap.Find(&SlateWidget.Get()))
        {
            PopulateSourceInfo(*BackingWidget, Node.SourceInfo);
            Node.Properties = CollectLiveWidgetProperties(*BackingWidget, Request.bVerbose);
            Node.SlotInfo = CollectLiveSlotInfo((*BackingWidget)->Slot, Request.bVerbose);
            const FWidgetTransform& RenderTransform = (*BackingWidget)->GetRenderTransform();
            if (!RenderTransform.IsIdentity())
            {
                Node.RuntimeState.bHasRenderTransform = true;
                Node.RuntimeState.RenderTransform = RenderTransform;
            }
            CollectBindingMetadata(*BackingWidget, Node.Bindings, Node.Delegates, BindingMetadataCache);
        }

        FChildren* Children = SlateWidget->GetChildren();
        if (Children)
        {
            for (int32 Index = 0; Index < Children->Num(); ++Index)
            {
                Node.Children.Add(BuildSnapshotNode(
                    Children->GetChildAt(Index),
                    BackingMap,
                    Request,
                    BindingMetadataCache));
            }
        }

        return Node;
    }

    // The backing-widget name a UMG root candidate is addressable by — the name of
    // the SObjectWidget's backing UWidget (e.g. WBP_PlayerHUD_StateTree_C_0). This is
    // the same name DescribeCandidate enumerates in the AMBIGUOUS_LIVE_ROOT error and
    // the key ui.create_hud returns, so it is what an instance_name selector matches.
    // Returns empty when the candidate has no resolvable backing widget.
    FString ResolveCandidateName(
        const TSharedRef<SWidget>& Candidate,
        const TMap<SWidget*, UWidget*>& BackingMap)
    {
        FChildren* Children = Candidate->GetChildren();
        if (Children && Children->Num() > 0)
        {
            const TSharedRef<SWidget> FirstChild = Children->GetChildAt(0);
            if (UWidget* const* BackingWidget = BackingMap.Find(&FirstChild.Get()))
            {
                return (*BackingWidget)->GetName();
            }
        }

        return FString();
    }

    FString DescribeCandidate(
        const TSharedRef<SWidget>& Candidate,
        const TMap<SWidget*, UWidget*>& BackingMap)
    {
        FString Summary = Candidate->GetTypeAsString();

        FChildren* Children = Candidate->GetChildren();
        if (Children && Children->Num() > 0)
        {
            const TSharedRef<SWidget> FirstChild = Children->GetChildAt(0);
            Summary += FString::Printf(TEXT(" -> %s"), *FirstChild->GetTypeAsString());

            const FString BackingName = ResolveCandidateName(Candidate, BackingMap);
            if (!BackingName.IsEmpty())
            {
                Summary += FString::Printf(TEXT(" (%s)"), *BackingName);
            }
        }

        return Summary;
    }

    FVector2D ResolveViewportSize(const TSharedPtr<SWindow>& Window)
    {
        if (Window.IsValid())
        {
            const FVector2D WindowSize = Window->GetClientSizeInScreen();
            if (!WindowSize.IsNearlyZero())
            {
                return WindowSize;
            }
        }

        if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
        {
            return FVector2D(GEngine->GameViewport->Viewport->GetSizeXY());
        }

        return FVector2D::ZeroVector;
    }
}

FLiveUiSnapshotRequest FLiveUiSnapshotRequest::FromContext(const FHandlerContext& Ctx)
{
    FLiveUiSnapshotRequest Request;
    Request.bVerbose = Ctx.GetBool(TEXT("verbose"), false);
    Request.bIncludeGeometry = Ctx.GetBool(TEXT("include_geometry"), false);
    Request.InstanceName = Ctx.GetStringFirstOf({TEXT("instance_name"), TEXT("instanceName")});
    Request.RootIndex = Ctx.GetIntFirstOf({TEXT("root_index"), TEXT("rootIndex")});
    return Request;
}

bool FLiveUiSnapshotService::SelectRootCandidate(
    const TArray<FString>& CandidateNames,
    const FLiveUiSnapshotRequest& Request,
    int32& OutSelectedIndex,
    FString& OutErrorCode,
    FString& OutErrorMessage,
    const TArray<FString>* DisplaySummaries)
{
    OutSelectedIndex = INDEX_NONE;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (CandidateNames.Num() == 0)
    {
        OutErrorCode = TEXT("LIVE_UI_NOT_FOUND");
        OutErrorMessage = TEXT("No live UMG root subtree was found in the game viewport window");
        return false;
    }

    const bool bHasRootIndex = Request.RootIndex.IsSet();
    const bool bHasInstanceName = !Request.InstanceName.IsEmpty();

    // Positional selector takes precedence and is exact.
    if (bHasRootIndex)
    {
        const int32 Index = Request.RootIndex.GetValue();
        if (Index < 0 || Index >= CandidateNames.Num())
        {
            OutErrorCode = TEXT("LIVE_ROOT_NOT_FOUND");
            OutErrorMessage = FString::Printf(
                TEXT("root_index %d is out of range; %d live UMG root candidate(s) present (valid 0..%d)"),
                Index,
                CandidateNames.Num(),
                CandidateNames.Num() - 1);
            return false;
        }

        // If both selectors are given, the named root must agree with the index.
        if (bHasInstanceName &&
            !CandidateNames[Index].Contains(Request.InstanceName))
        {
            OutErrorCode = TEXT("LIVE_ROOT_NOT_FOUND");
            OutErrorMessage = FString::Printf(
                TEXT("root_index %d ('%s') does not match instance_name '%s'"),
                Index,
                *CandidateNames[Index],
                *Request.InstanceName);
            return false;
        }

        OutSelectedIndex = Index;
        return true;
    }

    // Name selector: substring match against the backing widget names, mirroring the
    // resolve_geometry instance_name precedent (GetName().Contains).
    if (bHasInstanceName)
    {
        int32 MatchedIndex = INDEX_NONE;
        int32 MatchCount = 0;
        for (int32 Index = 0; Index < CandidateNames.Num(); ++Index)
        {
            if (!CandidateNames[Index].IsEmpty() &&
                CandidateNames[Index].Contains(Request.InstanceName))
            {
                if (MatchCount == 0)
                {
                    MatchedIndex = Index;
                }
                ++MatchCount;
            }
        }

        if (MatchCount == 0)
        {
            OutErrorCode = TEXT("LIVE_ROOT_NOT_FOUND");
            OutErrorMessage = FString::Printf(
                TEXT("instance_name '%s' matched no live UMG root candidate; candidates: %s"),
                *Request.InstanceName,
                *FString::Join(CandidateNames, TEXT(", ")));
            return false;
        }

        if (MatchCount > 1)
        {
            OutErrorCode = TEXT("AMBIGUOUS_LIVE_ROOT");
            OutErrorMessage = FString::Printf(
                TEXT("instance_name '%s' matched %d live UMG root candidates; pass a more specific instance_name or root_index. Candidates: %s"),
                *Request.InstanceName,
                MatchCount,
                *FString::Join(CandidateNames, TEXT(", ")));
            return false;
        }

        OutSelectedIndex = MatchedIndex;
        return true;
    }

    // No selector: the single-root case is unambiguous; multi-root needs one.
    if (CandidateNames.Num() == 1)
    {
        OutSelectedIndex = 0;
        return true;
    }

    // Prefer the richer "Type -> Type (name)" candidate summaries for the human-readable
    // list when the caller supplied them; otherwise fall back to the bare backing names.
    const TArray<FString>& CandidateList =
        (DisplaySummaries && DisplaySummaries->Num() == CandidateNames.Num())
            ? *DisplaySummaries
            : CandidateNames;

    OutErrorCode = TEXT("AMBIGUOUS_LIVE_ROOT");
    OutErrorMessage = FString::Printf(
        TEXT("Multiple live UMG root candidates matched: %s. Pass instance_name (the backing widget name, e.g. from ui.create_hud) or root_index to select one."),
        *FString::Join(CandidateList, TEXT(", ")));
    return false;
}

bool FLiveUiSnapshotService::Capture(
    const FLiveUiSnapshotRequest& Request,
    FLiveUiSnapshot& OutSnapshot,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutSnapshot = FLiveUiSnapshot{};
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (!ValidateSlateInitialized(OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    UWorld* World = ResolvePieWorld();
    if (!World)
    {
        OutErrorCode = TEXT("PIE_NOT_RUNNING");
        OutErrorMessage = TEXT("No active PIE game viewport world");
        return false;
    }

    TSharedPtr<SWindow> GameWindow = ResolveGameViewportWindow();
    if (!GameWindow.IsValid())
    {
        OutErrorCode = TEXT("GAME_VIEWPORT_NOT_FOUND");
        OutErrorMessage = TEXT("Could not resolve the active game viewport window");
        return false;
    }

    TArray<TSharedRef<SWidget>> RootCandidates;
    CollectUmgRootCandidates(GameWindow.ToSharedRef(), RootCandidates);

    TMap<SWidget*, UWidget*> BackingMap;
    BuildBackingWidgetMap(World, BackingMap);

    if (RootCandidates.Num() == 0)
    {
        OutErrorCode = TEXT("LIVE_UI_NOT_FOUND");
        OutErrorMessage = TEXT("No live UMG root subtree was found in the game viewport window");
        return false;
    }

    // Resolve which root to dump using the same backing-widget names the candidate
    // summaries enumerate, so an instance_name/root_index selector disambiguates a
    // multi-root viewport instead of dead-ending in AMBIGUOUS_LIVE_ROOT.
    TArray<FString> CandidateNames;
    CandidateNames.Reserve(RootCandidates.Num());
    for (const TSharedRef<SWidget>& Candidate : RootCandidates)
    {
        CandidateNames.Add(ResolveCandidateName(Candidate, BackingMap));
    }

    // Only the no-selector multi-root ambiguity message needs the richer
    // "Type -> Type (name)" summaries; build them just for that path and hand them to
    // SelectRootCandidate, which owns the message text. Every other path lists nothing.
    TArray<FString> CandidateSummaries;
    if (RootCandidates.Num() > 1 && Request.InstanceName.IsEmpty() && !Request.RootIndex.IsSet())
    {
        CandidateSummaries.Reserve(RootCandidates.Num());
        for (const TSharedRef<SWidget>& Candidate : RootCandidates)
        {
            CandidateSummaries.Add(DescribeCandidate(Candidate, BackingMap));
        }
    }

    int32 SelectedIndex = INDEX_NONE;
    if (!SelectRootCandidate(CandidateNames, Request, SelectedIndex, OutErrorCode, OutErrorMessage,
            CandidateSummaries.Num() > 0 ? &CandidateSummaries : nullptr))
    {
        return false;
    }

    OutSnapshot.CaptureSource = TEXT("live");
    OutSnapshot.bVerbose = Request.bVerbose;
    OutSnapshot.bGeometryIncluded = Request.bIncludeGeometry;
    OutSnapshot.ViewportSize = ResolveViewportSize(GameWindow);
    OutSnapshot.SelectedRootIndex = SelectedIndex;
    OutSnapshot.SelectedRootName = CandidateNames[SelectedIndex];
    OutSnapshot.RootCandidateCount = RootCandidates.Num();

    TMap<TWeakObjectPtr<UWidgetBlueprint>, FWidgetBindingMetadata> BindingMetadataCache;
    OutSnapshot.RootNode = BuildSnapshotNode(RootCandidates[SelectedIndex], BackingMap, Request, BindingMetadataCache);
    return true;
}
