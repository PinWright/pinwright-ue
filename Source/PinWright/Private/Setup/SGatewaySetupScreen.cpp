// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Setup/SGatewaySetupScreen.h"

#include "Editor.h"
#include "PinWrightSettings.h"
#include "PinWrightSubsystem.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ISettingsModule.h"
#include "ISinglePropertyView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

void FGatewaySetupSettingsHook::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent,
                                                 FProperty* PropertyThatChanged)
{
    GetMutableDefault<UPinWrightSettings>()->SaveConfig();
}

void SGatewaySetupScreen::Construct(const FArguments& InArgs)
{
    bEmbedded = InArgs._bEmbedded;
    EndpointUrl = AgentMcpConfigurator::GetEndpointUrl();

    AgentRows = {
        { EAgentTool::ClaudeCode, INVTEXT("Claude Code"),
          INVTEXT("Writes .mcp.json in the project root. Teammates get a one-time approval prompt "
                  "for the new MCP server the next time they run Claude Code in this project.") },
        { EAgentTool::CodexCli, INVTEXT("Codex CLI"),
          INVTEXT("Writes .codex/config.toml in the project root. The project folder must be "
                  "trusted by Codex - the first 'codex' run in this folder prompts for trust.") },
        { EAgentTool::Cursor, INVTEXT("Cursor"),
          INVTEXT("Opens a deeplink that installs to your user-global Cursor config after a "
                  "confirmation dialog in Cursor. Installation state cannot be detected from here.") },
        { EAgentTool::GeminiCli, INVTEXT("Gemini CLI"),
          INVTEXT("Writes .gemini/settings.json in the project root. The folder must be trusted "
                  "in Gemini CLI before the server is used.") },
        { EAgentTool::VsCodeCopilot, INVTEXT("VS Code Copilot"),
          INVTEXT("Writes .vscode/mcp.json in the project root. VS Code shows a one-time trust "
                  "prompt when the server first starts.") },
    };
    RefreshAgentStates();

    const TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

    Content->AddSlot().AutoHeight()
    [
        MakeHeader()
    ];

    for (int32 RowIndex = 0; RowIndex < AgentRows.Num(); ++RowIndex)
    {
        Content->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
        [
            MakeAgentRow(RowIndex)
        ];
    }

    Content->AddSlot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)
    [
        MakeSetupPromptArea()
    ];

    if (bEmbedded)
    {
        // The details panel hosts this widget: it scrolls and frames the section itself,
        // and the footer would duplicate rows already present on the settings page.
        ChildSlot
        [
            Content
        ];
        return;
    }

    Content->AddSlot().FillHeight(1.0f)
    [
        SNew(SSpacer)
    ];

    Content->AddSlot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 0.0f)
    [
        MakeFooter()
    ];

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(24.0f)
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                Content
            ]
        ]
    ];
}

TSharedRef<SDockTab> SGatewaySetupScreen::CreateSetupScreenTab(const FSpawnTabArgs& Args)
{
    const TSharedRef<SDockTab> MajorTab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
    MajorTab->SetContent(SNew(SGatewaySetupScreen));
    return MajorTab;
}

TSharedRef<SWidget> SGatewaySetupScreen::MakeHeader()
{
    const TSharedRef<SVerticalBox> Header = SNew(SVerticalBox);

    // The Project Settings page titles the section itself; only the standalone tab
    // needs its own heading.
    if (!bEmbedded)
    {
        Header->AddSlot().AutoHeight()
        [
            SNew(STextBlock)
            .Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
            .Text(INVTEXT("PinWright MCP Setup"))
        ];
    }

    // No endpoint URL field here: the port is followed at runtime via the gateway-port
    // file, so surfacing the URL as the thing to copy would only invite baking it into
    // configs. The listening banner below still names it for diagnostics.
    Header->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 16.0f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text(INVTEXT("This editor runs an in-process MCP server AI agents can use to automate "
                          "the editor. Install the connection into your agent's project config - it "
                          "launches a small bundled-Python proxy so the agent stays connected across "
                          "editor restarts, with nothing extra to install."))
    ];

    Header->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
        [
            MakeStatusBanner()
        ];

    return Header;
}

TSharedRef<SWidget> SGatewaySetupScreen::MakeStatusBanner()
{
    EMcpServerStatus Status = EMcpServerStatus::Disabled;
    int32 BindAttempts = 0;
    bool bRetriesExhausted = false;
    if (GEditor)
    {
        if (const UPinWrightSubsystem* Subsystem =
                GEditor->GetEditorSubsystem<UPinWrightSubsystem>())
        {
            Status = Subsystem->GetServerStatus();
            BindAttempts = Subsystem->GetBindAttemptCount();
            bRetriesExhausted = Subsystem->IsBindRetryExhausted();
        }
    }

    // A failed port bind silently kills MCP for the whole editor, so it gets a filled
    // red banner with a large alert icon and a bold heading - it must be impossible to
    // miss. The benign states (listening / disabled) keep the quiet dot + text line.
    //
    // This branch was unreachable until the subsystem started producing PortInUse: a lost
    // bind reported itself as Disabled, so the one failure the red banner exists for showed
    // the quiet amber "transport is disabled in Project Settings" line instead.
    if (Status == EMcpServerStatus::PortInUse)
    {
        // The remedy differs by phase. While the bounded retry is still running the answer is
        // usually "wait" - the common cause is a previous editor of this project that has not
        // finished exiting - and telling the user to change settings and restart would be
        // wrong. Only once the budget is spent is a settings change the actual fix.
        const FString Remedy = bRetriesExhausted
            ? FString(TEXT("The bind was retried until the retry budget was spent, so this is not a "
                           "transient collision. To fix: enable \"Auto-derive Port From Project Path\" in "
                           "Project Settings (PinWright) to give each project a unique port, or set a "
                           "different fixed port, then restart this editor and re-run onboarding to update "
                           "the agent configs."))
            : FString(TEXT("The bind is being retried on a bounded backoff, so if the holder is a previous "
                           "editor of this project that is still exiting, MCP will come up on its own. If it "
                           "does not, enable \"Auto-derive Port From Project Path\" in Project Settings "
                           "(PinWright) to give each project a unique port, or set a different fixed port, "
                           "then restart this editor and re-run onboarding."));

        const FText DetailText = FText::FromString(FString::Printf(
            TEXT("%s could not be bound (%d failed attempt%s) - the port is either already in use (e.g. "
                 "another project's editor on the same default port, or a previous editor of this project "
                 "that has not finished exiting) or reserved by the OS (e.g. a Windows excluded / Hyper-V "
                 "port range). %s"),
            *EndpointUrl, BindAttempts, (BindAttempts == 1 ? TEXT("") : TEXT("s")), *Remedy));

        return SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .BorderBackgroundColor(FLinearColor(0.45f, 0.06f, 0.06f))
            .Padding(FMargin(14.0f, 12.0f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
                    [
                        SNew(SImage)
                        .Image(FAppStyle::GetBrush("Icons.ErrorWithColor.Large"))
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Font(FAppStyle::GetFontStyle("HeadingSmall"))
                        .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.45f, 0.45f)))
                        .AutoWrapText(true)
                        .Text(INVTEXT("MCP server is NOT running - port conflict"))
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(DetailText)
                ]
            ];
    }

    FLinearColor DotColor;
    FText StatusText;
    switch (Status)
    {
    case EMcpServerStatus::Listening:
        DotColor = FLinearColor(0.20f, 0.80f, 0.35f);
        StatusText = FText::FromString(FString::Printf(TEXT("Server listening on %s"), *EndpointUrl));
        break;
    case EMcpServerStatus::Disabled:
    default:
        DotColor = FLinearColor(0.95f, 0.65f, 0.10f);
        StatusText = INVTEXT("MCP transport is disabled in Project Settings.");
        break;
    }

    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 6.0f, 0.0f)
        [
            SNew(STextBlock)
            .Text(INVTEXT("●"))
            .ColorAndOpacity(FSlateColor(DotColor))
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text(StatusText)
        ];
}

TSharedRef<SWidget> SGatewaySetupScreen::MakeAgentRow(int32 RowIndex)
{
    const FAgentRowDesc& Row = AgentRows[RowIndex];

    return SNew(SHorizontalBox)
        .ToolTipText(Row.Caveat)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SBox)
            .MinDesiredWidth(140.0f)
            [
                SNew(STextBlock).Text(Row.DisplayName)
            ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12.0f, 0.0f)
        [
            SNew(STextBlock)
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            .Text_Lambda([this, RowIndex]() { return GetStateText(RowIndex); })
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SBox)
            .MinDesiredWidth(110.0f)
            [
                // ButtonStyle is not attribute-bindable, so the Outdated state swaps in a
                // separately styled green button via visibility instead of restyling one.
                SNew(SOverlay)
                + SOverlay::Slot()
                [
                    SNew(SButton)
                    .HAlign(HAlign_Center)
                    .ToolTipText(Row.Caveat)
                    .Visibility_Lambda([this, RowIndex]()
                    {
                        return AgentRows[RowIndex].State == EAgentConfigState::Outdated
                            ? EVisibility::Collapsed : EVisibility::Visible;
                    })
                    .Text_Lambda([this, RowIndex]() { return GetButtonLabel(RowIndex); })
                    .OnClicked_Lambda([this, RowIndex]() { return OnApplyClicked(RowIndex); })
                ]
                + SOverlay::Slot()
                [
                    // Green call-to-action for stale configs: Update is the one click the
                    // user is expected to make.
                    SNew(SButton)
                    .ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
                    .ForegroundColor(FLinearColor::White)
                    .HAlign(HAlign_Center)
                    .ToolTipText(Row.Caveat)
                    .Visibility_Lambda([this, RowIndex]()
                    {
                        return AgentRows[RowIndex].State == EAgentConfigState::Outdated
                            ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    .Text_Lambda([this, RowIndex]() { return GetButtonLabel(RowIndex); })
                    .OnClicked_Lambda([this, RowIndex]() { return OnApplyClicked(RowIndex); })
                ]
            ]
        ];
}

TSharedRef<SWidget> SGatewaySetupScreen::MakeSetupPromptArea()
{
    // Written as an instruction so users can paste it into any AI agent and let it
    // configure the connection itself. Stdio proxy first (connection survives editor
    // restarts, port followed via the gateway-port file, no baked URL to go stale);
    // direct HTTP only when the proxy launch can't be resolved.
    FString Prompt;
    FString ProxyCommand;
    TArray<FString> ProxyArgs;
    if (AgentMcpConfigurator::GetStdioProxyLaunch(ProxyCommand, ProxyArgs))
    {
        FString ArgsJson;
        for (const FString& Arg : ProxyArgs)
        {
            ArgsJson += FString::Printf(TEXT("%s\"%s\""),
                ArgsJson.IsEmpty() ? TEXT("") : TEXT(", "), *Arg);
        }
        Prompt = FString::Printf(TEXT(
            "Set up the PinWright MCP server in this coding agent:\n"
            "- server name: pinwright\n"
            "- transport: stdio - the agent spawns this bundled proxy (nothing to install; the MCP "
            "connection survives editor restarts):\n"
            "  command: %s\n"
            "  args: [%s]\n"
            "- write it into this agent's project-scoped MCP config file\n"
            "- the proxy re-reads the gateway-port file (in the project's Saved/PinWright folder) "
            "before every call, so the config stays valid when the editor's port changes\n"
            "- only if this agent cannot spawn stdio MCP servers: use streamable HTTP, endpoint %s "
            "(loopback only; requires an Authorization: Bearer token read from the gateway-token "
            "file in Saved/PinWright; the port can change - the live one is in the gateway-port file)"),
            *ProxyCommand, *ArgsJson, *EndpointUrl);
    }
    else
    {
        Prompt = FString::Printf(TEXT(
            "Set up the PinWright MCP server in this coding agent:\n"
            "- server name: pinwright\n"
            "- transport: streamable HTTP, endpoint: %s (loopback only; requires an Authorization: "
            "Bearer token read from the gateway-token file in the project's Saved/PinWright folder)\n"
            "- the editor publishes its live port to the gateway-port file in the same folder; "
            "re-check it if the endpoint stops responding\n"
            "- prefer this agent's project-scoped MCP config file\n"
            "- if this agent supports only stdio MCP servers, spawn the plugin's bundled bridge "
            "with any Python 3 instead: <PinWright plugin dir>/Content/Python/mcp_proxy.py with "
            "args --token-file and --port-file pointing at the gateway-token / gateway-port files "
            "in Saved/PinWright (it forwards stdio to this HTTP endpoint and follows the live port)"),
            *EndpointUrl);
    }

    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            SNew(STextBlock).Text(INVTEXT("Agent setup prompt"))
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                SNew(SMultiLineEditableTextBox)
                .IsReadOnly(true)
                .AutoWrapText(true)
                .Text(FText::FromString(Prompt))
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(8.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SButton)
                .Text(INVTEXT("Copy"))
                .ToolTipText(INVTEXT("Copy the setup prompt to paste into your AI agent"))
                .OnClicked_Lambda([Prompt]()
                {
                    FPlatformApplicationMisc::ClipboardCopy(*Prompt);
                    return FReply::Handled();
                })
            ]
        ];
}

TSharedRef<SWidget> SGatewaySetupScreen::MakeFooter()
{
    FPropertyEditorModule& EditModule =
        FModuleManager::Get().GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

    FSinglePropertyParams Params;
    Params.NotifyHook = &SettingsPropertyHook;
    Params.NamePlacement = EPropertyNamePlacement::Type::Inside;

    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SButton)
            .Text(INVTEXT("Plugin Settings..."))
            .ToolTipText(INVTEXT("Open the PinWright page in Project Settings"))
            .OnClicked_Lambda([]()
            {
                if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
                {
                    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
                    SettingsModule->ShowViewer(Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
                }
                return FReply::Handled();
            })
        ]
        + SHorizontalBox::Slot().FillWidth(1.0f)
        [
            SNew(SSpacer)
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            EditModule.CreateSingleProperty(
                GetMutableDefault<UPinWrightSettings>(),
                GET_MEMBER_NAME_CHECKED(UPinWrightSettings, bShowSetupScreenOnProblem),
                Params).ToSharedRef()
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16.0f, 0.0f, 0.0f, 0.0f)
        [
            EditModule.CreateSingleProperty(
                GetMutableDefault<UPinWrightSettings>(),
                GET_MEMBER_NAME_CHECKED(UPinWrightSettings, bShowSetupScreenOnLaunch),
                Params).ToSharedRef()
        ];
}

void SGatewaySetupScreen::RefreshAgentStates()
{
    for (FAgentRowDesc& Row : AgentRows)
    {
        Row.State = AgentMcpConfigurator::Detect(Row.Agent, EndpointUrl);
    }
}

FReply SGatewaySetupScreen::OnApplyClicked(int32 RowIndex)
{
    FString Message;
    const bool bSuccess = AgentMcpConfigurator::Apply(AgentRows[RowIndex].Agent, EndpointUrl, Message);

    FNotificationInfo Info(FText::FromString(Message));
    Info.ExpireDuration = 5.0f;
    if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
    {
        Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
    }

    RefreshAgentStates();
    return FReply::Handled();
}

FText SGatewaySetupScreen::GetStateText(int32 RowIndex) const
{
    switch (AgentRows[RowIndex].State)
    {
    case EAgentConfigState::Configured:
        return INVTEXT("Configured (up to date)");
    case EAgentConfigState::Outdated:
        return INVTEXT("Configured but outdated - Update to refresh");
    case EAgentConfigState::Unknown:
        return INVTEXT("Cannot detect (user-global config)");
    case EAgentConfigState::NotConfigured:
    default:
        return INVTEXT("Not configured");
    }
}

FText SGatewaySetupScreen::GetButtonLabel(int32 RowIndex) const
{
    if (AgentRows[RowIndex].Agent == EAgentTool::Cursor)
    {
        return INVTEXT("Install...");
    }
    switch (AgentRows[RowIndex].State)
    {
    case EAgentConfigState::Configured:
        return INVTEXT("Installed ✓");
    case EAgentConfigState::Outdated:
        return INVTEXT("Update");
    case EAgentConfigState::NotConfigured:
    default:
        return INVTEXT("Install");
    }
}
