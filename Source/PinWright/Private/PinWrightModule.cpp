// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "Modules/ModuleManager.h"
#include "State/PluginState.h"
#include "Containers/Ticker.h"

#include "Handlers/Niagara/NiagaraSystemViewModelCache.h"
#include "Handlers/UI/WidgetDesignerCompileGuard.h"
#include "Utils/PythonCallbackRegistry.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "IPackageAutoSaver.h"
#include "PinWrightSettings.h"
#include "PinWrightSubsystem.h"
#include "Transport/ModalStateProbe.h"
#include "UnrealEdGlobals.h"
#include "Setup/AgentMcpConfigurator.h"
#include "Setup/GatewaySettingsDetails.h"
#include "Setup/SGatewaySetupScreen.h"
#include "Framework/Application/SlateApplication.h"
#include "PropertyEditorModule.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/CoreDelegates.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

// Suite maintenance lives under Private/Tests, which the packaged release curates out
// (release-manifest.json), and the header declares nothing when automation tests are compiled
// out, so both facts gate the hooks below. WITH_AUTOMATION_TESTS is undefined rather than zero in
// a plugin-only build, and MSVC reports an undefined identifier in an #if as C4668, which that
// build escalates to an error - hence the #ifdef rather than a bare #if.
#if __has_include("Tests/AutomationSuiteMaintenance.h")
#include "Tests/AutomationSuiteMaintenance.h"
#ifdef WITH_AUTOMATION_TESTS
#define PW_HAS_SUITE_MAINTENANCE WITH_AUTOMATION_TESTS
#endif
#endif
#ifndef PW_HAS_SUITE_MAINTENANCE
#define PW_HAS_SUITE_MAINTENANCE 0
#endif

// Save current LOCTEXT_NAMESPACE if defined, then set our own
#pragma push_macro("LOCTEXT_NAMESPACE")
#undef LOCTEXT_NAMESPACE
#define LOCTEXT_NAMESPACE "FPinWrightModule"

DEFINE_LOG_CATEGORY_STATIC(LogPinWright, Log, All);

class FPinWrightModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogPinWright, Log, TEXT("PinWright module initialized."));

#if PW_HAS_SUITE_MAINTENANCE
        PinWrightSuiteMaintenance::Register();
#endif

        // UDeveloperSettings (UPinWrightSettings) are auto-registered with the
        // Project Settings UI. Do not manually register them via ISettingsModule as this
        // produces duplicate entries in Project Settings. The settings class saves
        // automatically in PostEditChangeProperty.
        UE_LOG(LogPinWright, Verbose, TEXT("UPinWrightSettings are exposed via Project Settings (auto-registered)."));

        // Setup-screen UI is meaningless (and Slate is unavailable) in commandlets.
        if (!IsRunningCommandlet())
        {
            EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddStatic(
                &UPinWrightSubsystem::RecordEditorInitialized);
            MCP_ON_POST_ENGINE_INIT.AddRaw(this, &FPinWrightModule::OnPostEngineInit);
        }
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogPinWright, Log, TEXT("PinWright module shut down."));

#if PW_HAS_SUITE_MAINTENANCE
        PinWrightSuiteMaintenance::Unregister();
#endif

        MCP_ON_POST_ENGINE_INIT.RemoveAll(this);

        ModalStateProbe::Unregister();
        WidgetDesignerCompileGuard::UnregisterPreCompileGuard();

        if (EditorInitializedHandle.IsValid())
        {
            FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
            EditorInitializedHandle.Reset();
        }

        if (IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame"))
        {
            MainFrame->OnMainFrameCreationFinished().RemoveAll(this);
        }

        if (bSetupTabRegistered && FSlateApplication::IsInitialized() && !IsEngineExitRequested())
        {
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SGatewaySetupScreen::GetTabId());
        }

        if (bSettingsLayoutRegistered)
        {
            if (FPropertyEditorModule* PropertyEditor =
                    FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
            {
                PropertyEditor->UnregisterCustomClassLayout(TEXT("PinWrightSettings"));
            }
        }

        // No explicit unregister needed because we did not register the settings
        // manually. UDeveloperSettings instances are managed by the engine.

        FAsyncFolderDumpState& FolderDump = FPluginState::Get().GetFolderDump();
        if ((FolderDump.bInProgress || !FolderDump.JobTicketId.IsEmpty())
            && FolderDump.TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(FolderDump.TickerHandle);
        }
        FolderDump = FAsyncFolderDumpState{};

        PinWrightNiagara::ShutdownSystemViewModelCache();

        // The Python-callback tracker binds a static to FEditorDelegates::EndPIE; leaving it
        // bound past a DLL unload would dispatch into freed code on the next PIE stop.
        PinWright::PythonCallbacks::Shutdown();
    }

private:
    FDelegateHandle EditorInitializedHandle;

    // Auto-decline the boot-time "Restore Packages" prompt, and start watching for
    // modals that own the game thread.
    //
    // The prompt is SPackageRestoreDialog in a modal SWindow (PackageRestore.cpp:650)
    // raised from FPackageAutoSaver::OfferToRestorePackages, called unconditionally
    // during boot at UnrealEdMisc.cpp:376 - AFTER UPinWrightSubsystem::Initialize has
    // already logged "initialized" and bound the socket (EditorEngine.cpp:1255), but
    // BEFORE the core ticker has run even once. So the transport is listening, the
    // readiness snapshot is stale-but-positive, and no RPC can reach the game thread:
    // exactly the state an agent cannot distinguish from a crash.
    //
    // OnPostEngineInit (LaunchEngineLoop.cpp:4835) is the only plugin-reachable
    // callback between the auto-saver's construction (UnrealEdEngine.cpp:104) and that
    // recovery point, which is the window IPackageAutoSaver's own doc comment requires:
    // "The flag must be raised before the Engine reach the recovery point during the
    // boot process, otherwise, it has no effect." Subsystem Initialize is too early -
    // GetPackageAutoSaver() would dereference a null TUniquePtr.
    //
    // This protects editors a human launched from the Epic Launcher with no PinWright
    // flags; PinWright-launched editors also get -AutoDeclinePackageRecovery from the
    // stdio proxy (Content/Python/mcp_proxy.py, _ALWAYS_FLAGS).
    void DeclineAutoSaveRecoveryPrompt()
    {
        if (!GUnrealEd || !GetDefault<UPinWrightSettings>()->bDeclineAutoSaveRecoveryPrompt)
        {
            return;
        }
        GUnrealEd->GetPackageAutoSaver().DisableRestorePromptAndDeclinePackageRecovery();
        UE_LOG(LogPinWright, Log,
            TEXT("PinWright declined the auto-save recovery prompt (bDeclineAutoSaveRecoveryPrompt). ")
            TEXT("Auto-saved packages remain under Saved/Autosaves/ and can be recovered manually."));
    }

    void OnPostEngineInit()
    {
        // Both run before the Slate gate below: the decline does not need Slate at
        // all, and registering the probe here (rather than later) is what makes the
        // Restore Packages prompt itself reportable when the decline is switched off.
        DeclineAutoSaveRecoveryPrompt();
        ModalStateProbe::Register();

        // Every plugin compile route reaches FKismetEditorUtilities::CompileBlueprint directly,
        // one layer below the toolkit that owns the UMG Designer preview - so nothing destroys
        // that preview before the widget class is purged, and the editor dies on the next Slate
        // paint. Registered here rather than per verb because the engine's own pre-compile
        // broadcast covers every route at once; the full argument is on the header.
        WidgetDesignerCompileGuard::RegisterPreCompileGuard();

        if (!FSlateApplication::IsInitialized())
        {
            return;
        }

        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
                SGatewaySetupScreen::GetTabId(),
                FOnSpawnTab::CreateStatic(&SGatewaySetupScreen::CreateSetupScreenTab),
                FCanSpawnTab::CreateRaw(this, &FPinWrightModule::CanSpawnSetupTab))
            .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
            .SetDisplayName(LOCTEXT("SetupScreenTabTitle", "PinWright Setup"))
            .SetTooltipText(LOCTEXT("SetupScreenTabTooltip", "One-click MCP setup for AI agent clients connecting to this project's PinWright gateway"))
            .SetMenuType(ETabSpawnerMenuType::Enabled)
            .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"));
        bSetupTabRegistered = true;

        FPropertyEditorModule& PropertyEditor =
            FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        PropertyEditor.RegisterCustomClassLayout(
            UPinWrightSettings::StaticClass()->GetFName(),
            FOnGetDetailCustomizationInstance::CreateStatic(&FGatewaySettingsDetails::MakeInstance));
        bSettingsLayoutRegistered = true;

        if (IMainFrameModule* MainFrame = FModuleManager::LoadModulePtr<IMainFrameModule>("MainFrame"))
        {
            MainFrame->OnMainFrameCreationFinished().AddRaw(
                this, &FPinWrightModule::OnMainFrameCreationFinished);
        }
    }

    void OnMainFrameCreationFinished(TSharedPtr<SWindow> InRootWindow, bool bIsRunningStartupDialog)
    {
        // The global editor-layout restore (which can recreate this nomad tab's standalone
        // window) has already run synchronously by now; CanSpawnSetupTab suppressed that
        // auto-restore so the screen doesn't reappear against bShowSetupScreenOnLaunch. Open
        // the gate now that startup is past, so the explicit launch/conflict logic below and
        // any later manual Tools-menu open behave normally.
        bAllowSetupTabSpawn = true;

        const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();

        // The subsystem's Initialize (which starts the transport) runs before main-frame
        // creation, so the server status is already resolved here.
        EMcpServerStatus Status = EMcpServerStatus::Disabled;
        if (GEditor)
        {
            if (const UPinWrightSubsystem* Subsystem =
                    GEditor->GetEditorSubsystem<UPinWrightSubsystem>())
            {
                Status = Subsystem->GetServerStatus();
            }
        }

        // An installed-but-outdated agent config counts as a problem too: the agent would
        // spawn the proxy with stale plumbing (e.g. pre-port-file args), so surface the
        // setup screen for a one-click Update. Detection keys on structural args
        // (port-file/token paths), not the port value, so a mere port change never trips
        // this. NotConfigured agents are the user's choice and don't trip it either;
        // Cursor is undetectable (user-global config) and is skipped.
        bool bAnyInstalledConfigOutdated = false;
        if (Settings->bShowSetupScreenOnProblem)
        {
            const FString EndpointUrl = AgentMcpConfigurator::GetEndpointUrl();
            const EAgentTool DetectableAgents[] = { EAgentTool::ClaudeCode,
                EAgentTool::CodexCli, EAgentTool::GeminiCli, EAgentTool::VsCodeCopilot };
            for (EAgentTool Agent : DetectableAgents)
            {
                if (AgentMcpConfigurator::Detect(Agent, EndpointUrl) == EAgentConfigState::Outdated)
                {
                    bAnyInstalledConfigOutdated = true;
                    break;
                }
            }
        }

        const bool bProblemAlert =
            ((Status == EMcpServerStatus::PortInUse) || bAnyInstalledConfigOutdated)
            && Settings->bShowSetupScreenOnProblem;

        if (bProblemAlert || Settings->bShowSetupScreenOnLaunch)
        {
            FGlobalTabmanager::Get()->TryInvokeTab(SGatewaySetupScreen::GetTabId());
        }
    }

    // Spawn gate for the setup tab. UE would otherwise auto-restore this nomad tab from the
    // machine-global editor layout on every launch, ignoring bShowSetupScreenOnLaunch. The
    // flag stays false through the startup layout-restore window (blocking that restore) and
    // is opened in OnMainFrameCreationFinished. The same predicate gates the Tools-menu entry,
    // so it must be stateful rather than a constant false.
    bool CanSpawnSetupTab(const FSpawnTabArgs& Args) const
    {
        return bAllowSetupTabSpawn;
    }

    bool bSetupTabRegistered = false;
    bool bSettingsLayoutRegistered = false;
    bool bAllowSetupTabSpawn = false;
};

// Restore the previous LOCTEXT_NAMESPACE
#pragma pop_macro("LOCTEXT_NAMESPACE")

IMPLEMENT_MODULE(FPinWrightModule, PinWright)
