// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/NotifyHook.h"
#include "Setup/AgentMcpConfigurator.h"
#include "Widgets/SCompoundWidget.h"

class FSpawnTabArgs;
class SDockTab;

// Persists the settings CDO when the footer checkbox is edited through
// FPropertyEditorModule::CreateSingleProperty (which bypasses PostEditChangeProperty
// unless a notify hook forwards the change).
class FGatewaySetupSettingsHook final : public FNotifyHook
{
public:
    virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent,
                                  FProperty* PropertyThatChanged) override;
};

// Startup setup screen: shows the live MCP endpoint and one-click config install
// buttons for common AI agent clients (Claude Code, Codex CLI, Cursor, Gemini CLI,
// VS Code Copilot). With bEmbedded it renders as a bare panel for hosting inside
// the plugin's Project Settings page (no border/scroll/title/footer).
class SGatewaySetupScreen : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SGatewaySetupScreen)
        : _bEmbedded(false)
    {}
        SLATE_ARGUMENT(bool, bEmbedded)
    SLATE_END_ARGS()

    static FName GetTabId() { return FName("PinWrightSetupScreen"); }

    void Construct(const FArguments& InArgs);

    static TSharedRef<SDockTab> CreateSetupScreenTab(const FSpawnTabArgs& Args);

private:
    struct FAgentRowDesc
    {
        EAgentTool Agent = EAgentTool::ClaudeCode;
        FText DisplayName;
        FText Caveat;
        EAgentConfigState State = EAgentConfigState::NotConfigured;
    };

    TSharedRef<SWidget> MakeHeader();
    TSharedRef<SWidget> MakeStatusBanner();
    TSharedRef<SWidget> MakeAgentRow(int32 RowIndex);
    TSharedRef<SWidget> MakeSetupPromptArea();
    TSharedRef<SWidget> MakeFooter();

    void RefreshAgentStates();
    FReply OnApplyClicked(int32 RowIndex);
    FText GetStateText(int32 RowIndex) const;
    FText GetButtonLabel(int32 RowIndex) const;

    FString EndpointUrl;
    TArray<FAgentRowDesc> AgentRows;
    FGatewaySetupSettingsHook SettingsPropertyHook;
    bool bEmbedded = false;
};
