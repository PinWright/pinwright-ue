// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared "start a standard Play-In-Editor session" helper for the Editor handler cluster.
// editor.play (PIEHandler.cpp) issues the deferred GEditor->RequestPlaySession(...)
// recipe through it: build
// FRequestPlaySessionParams for PlayInEditor, apply a transient session copy of the
// level-editor play settings (numClients/netMode overrides + explicit network-emulation
// state, force-disabled by default — see the persistence-constraint comment on
// RequestPlayInEditorSession), and target the level editor's first active viewport when
// one exists. Also hosts the engine-touching emulation companions: profile enumeration
// (GetAvailableEmulationProfiles) and the active-session read-back editor.pie_status uses
// (TryGetActiveSessionEmulation); the pure parse/validate side is PieNetworkEmulation.h.
// Consolidating it here means the call sites cannot drift, and the optional LevelEditor / play-settings
// headers plus their __has_include feature gates are declared exactly once — no duplicated
// MCP_* macro family, and no Unity-merge ODR clash to dodge (the gate macros are #undef'd
// at the end of this header so nothing leaks into the including TUs).
#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/NetworkSettings.h"
#include "Handlers/Editor/PieNetworkEmulation.h"
#include "IAssetViewport.h"
#include "Modules/ModuleManager.h"

#if __has_include("LevelEditor.h")
#  include "LevelEditor.h"
#  define MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_MODULE 1
#else
#  define MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_MODULE 0
#endif
#if __has_include("Settings/LevelEditorPlaySettings.h")
#  include "Settings/LevelEditorPlaySettings.h"
#  define MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_PLAY_SETTINGS 1
#else
#  define MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_PLAY_SETTINGS 0
#endif

namespace PieControlUtils
{

// Names of the preconfigured network-emulation profiles the PIE settings UI offers,
// in config order. Source of truth: UNetworkSettings::NetworkEmulationProfiles —
// config-driven via `[/Script/Engine.NetworkSettings]` `+NetworkEmulationProfiles=`
// entries (BaseEngine.ini ships Average / Bad / BufferBloat; projects can add more).
// The actual lag/loss numbers live separately in `[PacketSimulationProfile.<Name>]`
// engine-ini sections, loaded by FPacketSimulationSettings::LoadEmulationProfile at
// net-driver setup — only the *names* are needed for validation here.
inline TArray<FString> GetAvailableEmulationProfiles()
{
    TArray<FString> Names;
    if (const UNetworkSettings* NetworkSettings = GetDefault<UNetworkSettings>())
    {
        Names.Reserve(NetworkSettings->NetworkEmulationProfiles.Num());
        for (const FNetworkEmulationProfileDescription& Profile : NetworkSettings->NetworkEmulationProfiles)
        {
            Names.Add(Profile.ProfileName);
        }
    }
    return Names;
}

// Issues the deferred request that starts a Play-In-Editor session. Callers guard
// GEditor / GEditor->PlayWorld and shape their own response; this only queues the session
// (RequestPlaySession defers actual startup to the next tick). The internal GEditor guard
// keeps the helper safe to call standalone even though every current caller checks first.
//
// NumClients: <= 0 keeps the saved client count. NetMode: 0 = standalone,
// 1 = listen server, 2 = client; < 0 keeps the saved net mode.
// Emulation: the validated network-emulation state for this session (default:
// force-disabled). Unlike NumClients/NetMode there is no "keep saved" value —
// per-user saved emulation silently confounding multiplayer test sessions is the
// failure mode this param exists to eliminate, so every session gets an explicit,
// known wire state.
//
// PERSISTENCE CONSTRAINT — never touch the user's saved ULevelEditorPlaySettings
// (Config=EditorPerProjectUserSettings; the details panel SaveConfig()s it on edit
// and a shutdown save could persist any in-memory mutation). This helper therefore
// never mutates that object: ALL overrides go onto a transient duplicate handed to
// the engine via FRequestPlaySessionParams::EditorPlaySettings. That is provably
// sufficient AND session-complete on the engine side (UE 5.7 sources):
//   - UEditorEngine::RequestPlaySession (PlayLevel.cpp ~979-992) re-duplicates the
//     passed object into the transient package ("Kept alive by AddReferencedObjects",
//     EditorEngine.cpp ~1706-1721), so nothing we pass is ever the saved instance.
//   - The duplicate is stored in PlayInEditorSessionInfo->OriginalRequestParams and
//     handed to EVERY PIE instance login — including clients created later in the
//     session (PlayLevel.cpp ~1825) — so there is no "too early to restore" window:
//     nothing needs restoring, ever, and an editor crash mid-session cannot leak
//     emulation state into a shutdown save.
//   - Emulation is consumed exclusively from that per-session object: in-process
//     instances via UGameInstance::StartPlayInEditorGameInstance
//     (GameInstance.cpp ~414-462, BuildPacketSettingsForURL -> ?PktEmulationProfile=),
//     run-under-separate-process instances via PlayLevelNewProcess.cpp ~243-250
//     (BuildPacketSettingsForCmdLine). No PIE consumer reads emulation from the CDO.
//
// Returns false only when an override (NumClients/NetMode/emulation enabled) was
// requested but ULevelEditorPlaySettings is unavailable on this engine (defensive
// __has_include gate; the header exists on all supported UE 5.3-5.8 engines); the
// plain session is not queued in that case so the caller can report the failure
// honestly.
inline bool RequestPlayInEditorSession(int32 NumClients = 0, int32 NetMode = -1,
    const PieNetworkEmulation::FEmulationSpec& Emulation = PieNetworkEmulation::FEmulationSpec())
{
    if (!GEditor)
    {
        return false;
    }

    FRequestPlaySessionParams PlayParams;
    PlayParams.WorldType = EPlaySessionWorldType::PlayInEditor;
#if MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_PLAY_SETTINGS
    // Always run on a transient duplicate: even a no-override session must force
    // network emulation to a known state (see the persistence-constraint comment).
    ULevelEditorPlaySettings* SessionSettings = DuplicateObject<ULevelEditorPlaySettings>(
        GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
    if (NumClients > 0)
    {
        SessionSettings->SetPlayNumberOfClients(NumClients);
    }
    if (NetMode >= 0)
    {
        SessionSettings->SetPlayNetMode(static_cast<EPlayNetMode>(NetMode));
    }
    // Multi-instance auto-connect only works under one process; harmless for standalone.
    if (NumClients > 1 || NetMode == 1)
    {
        SessionSettings->SetRunUnderOneProcess(true);
    }

    FLevelEditorPlayNetworkEmulationSettings& EmulationSettings = SessionSettings->NetworkEmulationSettings;
    EmulationSettings.bIsNetworkEmulationEnabled = Emulation.bEnabled;
    if (Emulation.bEnabled)
    {
        switch (Emulation.Target)
        {
        case PieNetworkEmulation::EEmulationTarget::ClientsOnly:
            EmulationSettings.EmulationTarget = NetworkEmulationTarget::Client;
            break;
        case PieNetworkEmulation::EEmulationTarget::Everyone:
            EmulationSettings.EmulationTarget = NetworkEmulationTarget::Any;
            break;
        default:
            EmulationSettings.EmulationTarget = NetworkEmulationTarget::Server;
            break;
        }
        // A named (non-"Custom") profile is consumed BY NAME downstream
        // (?PktEmulationProfile= on the URL / LoadEmulationProfile from the
        // [PacketSimulationProfile.<Name>] ini section), so the packet-value
        // members of the struct are irrelevant and stay as duplicated.
        EmulationSettings.CurrentProfile = Emulation.Profile;
    }
    PlayParams.EditorPlaySettings = SessionSettings;
#else
    if (NumClients > 0 || NetMode >= 0 || Emulation.bEnabled)
    {
        return false;
    }
#endif
#if MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_MODULE
    if (FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
    {
        TSharedPtr<IAssetViewport> DestinationViewport =
            LevelEditorModule->GetFirstActiveViewport();
        if (DestinationViewport.IsValid())
        {
            PlayParams.DestinationSlateViewport = DestinationViewport;
        }
    }
#endif

    GEditor->RequestPlaySession(PlayParams);
    return true;
}

// Historical no-override entry point; kept so existing call sites read unchanged.
// Note it still forces network emulation OFF (the FEmulationSpec default).
inline void RequestDefaultPlayInEditorSession()
{
    RequestPlayInEditorSession();
}

// Reads the network-emulation state actually in effect for the RUNNING PIE session
// into OutSpec, from the engine's own session-scoped settings duplicate
// (PlayInEditorSessionInfo->OriginalRequestParams.EditorPlaySettings — the exact
// object every PIE instance login consumes, see the persistence-constraint comment
// above). Because it reads the session copy rather than anything this plugin
// recorded, it reports the truth for sessions started from the editor toolbar too.
// Returns false when no PIE session is active (or the settings class is gated out).
// Note: a toolbar-started session with hand-edited packet values reports the
// details panel's "Custom" pseudo-profile name in OutSpec.Profile.
inline bool TryGetActiveSessionEmulation(PieNetworkEmulation::FEmulationSpec& OutSpec)
{
#if MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_PLAY_SETTINGS
    if (!GEditor)
    {
        return false;
    }
    const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
    if (!SessionInfo.IsSet() || !SessionInfo->OriginalRequestParams.EditorPlaySettings)
    {
        return false;
    }
    const FLevelEditorPlayNetworkEmulationSettings& EmulationSettings =
        SessionInfo->OriginalRequestParams.EditorPlaySettings->NetworkEmulationSettings;
    OutSpec.bEnabled = EmulationSettings.bIsNetworkEmulationEnabled;
    switch (EmulationSettings.EmulationTarget)
    {
    case NetworkEmulationTarget::Client:
        OutSpec.Target = PieNetworkEmulation::EEmulationTarget::ClientsOnly;
        break;
    case NetworkEmulationTarget::Any:
        OutSpec.Target = PieNetworkEmulation::EEmulationTarget::Everyone;
        break;
    default:
        OutSpec.Target = PieNetworkEmulation::EEmulationTarget::ServerOnly;
        break;
    }
    OutSpec.Profile = EmulationSettings.CurrentProfile;
    return true;
#else
    return false;
#endif
}

} // namespace PieControlUtils

#undef MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_MODULE
#undef MCP_PIE_CONTROL_HAS_LEVEL_EDITOR_PLAY_SETTINGS
