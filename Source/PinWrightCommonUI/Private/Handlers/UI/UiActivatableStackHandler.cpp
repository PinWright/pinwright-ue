// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/ActivatableLayerResolver.h"
#include "PinWrightHelpers.h"

#include "Blueprint/UserWidget.h"
#include "CommonActivatableWidget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

namespace
{
    // Locates a stack widget by name on a host UUserWidget instance currently
    // live in a world (PIE). Populates OutError on miss.
    UCommonActivatableWidgetContainerBase* FindStackInPie(
        const FString& HostName,
        const FString& StackName,
        FString& OutErrorCode,
        FString& OutErrorMsg)
    {
        UUserWidget* HostWidget = nullptr;
        for (TObjectIterator<UUserWidget> It; It; ++It)
        {
            if (It->GetWorld() != nullptr && It->GetName() == HostName)
            {
                HostWidget = *It;
                break;
            }
        }

        if (!HostWidget)
        {
            OutErrorCode = TEXT("HOST_NOT_FOUND");
            OutErrorMsg = FString::Printf(TEXT("Host widget '%s' not found in any live world"), *HostName);
            return nullptr;
        }

        UWidget* Child = HostWidget->GetWidgetFromName(FName(*StackName));
        if (!Child)
        {
            OutErrorCode = TEXT("STACK_NOT_FOUND");
            OutErrorMsg = FString::Printf(TEXT("No widget named '%s' found on host '%s'"), *StackName, *HostName);
            return nullptr;
        }

        UCommonActivatableWidgetContainerBase* Stack = Cast<UCommonActivatableWidgetContainerBase>(Child);
        if (!Stack)
        {
            OutErrorCode = TEXT("NOT_A_STACK");
            OutErrorMsg = FString::Printf(TEXT("Widget '%s' on host '%s' is not a UCommonActivatableWidgetContainerBase (got %s)"),
                *StackName, *HostName, *Child->GetClass()->GetName());
            return nullptr;
        }

        return Stack;
    }

    // Linear scan over a stack's widget list for an entry whose GetName() matches.
    UCommonActivatableWidget* FindStackEntryByName(
        UCommonActivatableWidgetContainerBase* Stack,
        const FString& Name)
    {
        if (!Stack) return nullptr;
        for (UCommonActivatableWidget* Entry : Stack->GetWidgetList())
        {
            if (Entry && Entry->GetName() == Name)
            {
                return Entry;
            }
        }
        return nullptr;
    }

    // Resolves the target container from whichever addressing mode the caller supplied:
    // host+stack (runtime widget-instance addressing) or layerTag (+playerIndex) (CommonGame
    // layer gameplay-tag addressing). The two modes are mutually exclusive. Shared by all four
    // ui.activatable_* methods so both surfaces stay identical. Populates OutError on miss.
    UCommonActivatableWidgetContainerBase* ResolveTargetStack(
        FHandlerContext& Ctx,
        FString& OutErrorCode,
        FString& OutErrorMsg)
    {
        const FString LayerTag = Ctx.GetString(TEXT("layerTag"));
        const FString HostName = Ctx.GetString(TEXT("host"));
        const FString StackName = Ctx.GetString(TEXT("stack"));

        if (!LayerTag.IsEmpty())
        {
            if (!HostName.IsEmpty() || !StackName.IsEmpty())
            {
                OutErrorCode = TEXT("AMBIGUOUS_TARGET");
                OutErrorMsg = TEXT("Provide either layerTag (CommonGame layer addressing) or host+stack, not both.");
                return nullptr;
            }
            const int32 PlayerIndex = Ctx.GetInt(TEXT("playerIndex"), 0);
            return PinWrightUi::ResolveStackByLayerTagInPie(LayerTag, PlayerIndex, OutErrorCode, OutErrorMsg);
        }

        if (HostName.IsEmpty() || StackName.IsEmpty())
        {
            OutErrorCode = TEXT("MISSING_TARGET");
            OutErrorMsg = TEXT("Provide host+stack (runtime widget addressing) or layerTag (CommonGame layer gameplay-tag addressing).");
            return nullptr;
        }
        return FindStackInPie(HostName, StackName, OutErrorCode, OutErrorMsg);
    }
}

// Target-addressing param schema shared by all four ui.activatable_* methods, single-sourced
// next to ResolveTargetStack (their one runtime reader) so the declared params never drift from
// the keys it parses (host / stack / layerTag / playerIndex). Expands to a comma-separated
// FParamSpec list with NO trailing comma for use inside RPC_PARAMS(...); it is not the last
// entry for push/pop, so those callers append a comma after it. Same idiom as
// DRIVE_WINDOW_SELECTOR_PARAMS in DriveHandlerCommon.h.
#define ACTIVATABLE_TARGET_PARAMS \
    RPC_PARAM_OPT("host", "string", "Name of the host UUserWidget instance that owns the stack (paired with 'stack'; alternative to 'layerTag')"), \
    RPC_PARAM_OPT("stack", "string", "Name of the UCommonActivatableWidgetContainerBase child within the host (paired with 'host'; alternative to 'layerTag')"), \
    RPC_PARAM_OPT("layerTag", "string", "CommonGame/Lyra UI layer gameplay tag (e.g. UI.Layer.Menu) resolving the target stack via the active PrimaryGameLayout; mutually exclusive with host/stack"), \
    RPC_PARAM_OPT("playerIndex", "integer", "Local player index for layerTag resolution (default 0)")

// Shared addressing clause appended onto each ui.activatable_* summary (via adjacent
// string-literal concatenation, like ACTORNAME_COLLISION_STEER) so the one addressing fact lives
// once. Leading space joins it to the per-verb sentence; REGISTER_RPC_HANDLER wraps the summary
// in TEXT(), concatenating the fragments at compile time.
#define ACTIVATABLE_ADDRESSING_SUMMARY " Address the stack by host+stack (runtime widget instance) or by layerTag (CommonGame/Lyra UI layer gameplay tag). Requires an active PIE session."

REGISTER_RPC_HANDLER("ui.activatable_push", "ui",
    "Instantiate an activatable widget and push it onto a UCommonActivatableWidgetContainerBase." ACTIVATABLE_ADDRESSING_SUMMARY,
    RPC_PARAMS(
        ACTIVATABLE_TARGET_PARAMS,
        RPC_PARAM_REQ("widgetClass", "classref", "Class path of the UCommonActivatableWidget subclass to instantiate")
    ))
{
    FString ClassPath;
    if (!Ctx.RequireString(TEXT("widgetClass"), ClassPath)) return true;

    // Route through the canonical resolver so a bare Blueprint asset path
    // (/Game/.../WBP_Menu.WBP_Menu) resolves to the generated UClass without the
    // caller having to append the _C generated-class suffix — matching ui.create_hud
    // and the resolver contract every other class-path slot in the API already honors.
    // ResolveUClass returns any UClass*; keep the IsChildOf(UCommonActivatableWidget)
    // guard so the downstream CreateWidget<UCommonActivatableWidget> cast stays type-safe.
    UClass* WidgetClass = ResolveUClass(ClassPath);
    if (!WidgetClass || !WidgetClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Failed to load UCommonActivatableWidget class: %s"), *ClassPath));
        return true;
    }

    FString ErrorCode, ErrorMsg;
    UCommonActivatableWidgetContainerBase* Stack = ResolveTargetStack(Ctx, ErrorCode, ErrorMsg);
    if (!Stack)
    {
        Ctx.SendError(ErrorCode, ErrorMsg);
        return true;
    }

    UWorld* World = Stack->GetWorld();
    APlayerController* Owner = Stack->GetOwningPlayer();
    if (!Owner && World)
    {
        Owner = World->GetFirstPlayerController();
    }

    UCommonActivatableWidget* Instance = CreateWidget<UCommonActivatableWidget>(Owner, WidgetClass);
    if (!Instance)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"),
            FString::Printf(TEXT("CreateWidget returned null for class %s"), *ClassPath));
        return true;
    }

    Stack->AddWidgetInstance(*Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("instanceName"), Instance->GetName());
    Result->SetStringField(TEXT("className"), WidgetClass->GetName());
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ui.activatable_pop", "ui",
    "Pop the active (or named) activatable widget off a stack." ACTIVATABLE_ADDRESSING_SUMMARY,
    RPC_PARAMS(
        ACTIVATABLE_TARGET_PARAMS,
        RPC_PARAM_OPT("instanceName", "string", "Specific entry's instance name to remove (default: active/top)")
    ))
{
    const FString InstanceName = Ctx.GetString(TEXT("instanceName"));

    FString ErrorCode, ErrorMsg;
    UCommonActivatableWidgetContainerBase* Stack = ResolveTargetStack(Ctx, ErrorCode, ErrorMsg);
    if (!Stack)
    {
        Ctx.SendError(ErrorCode, ErrorMsg);
        return true;
    }

    UCommonActivatableWidget* ToRemove = nullptr;
    if (!InstanceName.IsEmpty())
    {
        ToRemove = FindStackEntryByName(Stack, InstanceName);
        if (!ToRemove)
        {
            Ctx.SendError(TEXT("WIDGET_NOT_FOUND"),
                FString::Printf(TEXT("No entry named '%s' on stack '%s'"), *InstanceName, *Stack->GetName()));
            return true;
        }
    }
    else
    {
        ToRemove = Stack->GetActiveWidget();
        if (!ToRemove)
        {
            const TArray<UCommonActivatableWidget*>& List = Stack->GetWidgetList();
            if (List.Num() > 0)
            {
                ToRemove = List.Last();
            }
        }
        if (!ToRemove)
        {
            Ctx.SendError(TEXT("STACK_EMPTY"),
                FString::Printf(TEXT("Stack '%s' has no active widget to pop"), *Stack->GetName()));
            return true;
        }
    }

    const FString RemovedName = ToRemove->GetName();
    Stack->RemoveWidget(*ToRemove);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removedName"), RemovedName);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ui.list_stack_widgets", "ui",
    "List all activatable widgets currently on a stack, top -> bottom." ACTIVATABLE_ADDRESSING_SUMMARY,
    RPC_PARAMS(
        ACTIVATABLE_TARGET_PARAMS
    ))
{
    FString ErrorCode, ErrorMsg;
    UCommonActivatableWidgetContainerBase* Stack = ResolveTargetStack(Ctx, ErrorCode, ErrorMsg);
    if (!Stack)
    {
        Ctx.SendError(ErrorCode, ErrorMsg);
        return true;
    }

    UCommonActivatableWidget* Active = Stack->GetActiveWidget();
    const TArray<UCommonActivatableWidget*>& List = Stack->GetWidgetList();

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (int32 i = List.Num() - 1; i >= 0; --i)
    {
        UCommonActivatableWidget* Entry = List[i];
        if (!Entry) continue;
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("instanceName"), Entry->GetName());
        Obj->SetStringField(TEXT("className"), Entry->GetClass()->GetName());
        Obj->SetBoolField(TEXT("isActive"), Entry == Active);
        Entries.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("widgets"), Entries);
    Result->SetNumberField(TEXT("count"), Entries.Num());
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ui.get_active_widget", "ui",
    "Return the currently-active (top) activatable widget on a stack." ACTIVATABLE_ADDRESSING_SUMMARY,
    RPC_PARAMS(
        ACTIVATABLE_TARGET_PARAMS
    ))
{
    FString ErrorCode, ErrorMsg;
    UCommonActivatableWidgetContainerBase* Stack = ResolveTargetStack(Ctx, ErrorCode, ErrorMsg);
    if (!Stack)
    {
        Ctx.SendError(ErrorCode, ErrorMsg);
        return true;
    }

    UCommonActivatableWidget* Active = Stack->GetActiveWidget();
    if (!Active)
    {
        Ctx.SendError(TEXT("NO_ACTIVE_WIDGET"),
            FString::Printf(TEXT("Stack '%s' has no active widget"), *Stack->GetName()));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("instanceName"), Active->GetName());
    Result->SetStringField(TEXT("className"), Active->GetClass()->GetName());
    Ctx.SendSuccess(Result);
    return true;
}

#undef ACTIVATABLE_TARGET_PARAMS
#undef ACTIVATABLE_ADDRESSING_SUMMARY
