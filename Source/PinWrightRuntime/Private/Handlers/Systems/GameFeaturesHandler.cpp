// Copyright (c) 2026 Alexander Penkin. MIT License.

// GameFeaturesHandler.cpp — game_features.* namespace: read-only introspection of
// the Game Features subsystem (UGameFeaturesSubsystem).
//
// Game Feature plugins gate whole gameplay slices on Lyra-derived hosts. Without
// this surface an agent cannot see which GF plugins exist or what lifecycle state
// (Installed/Registered/Loaded/Active) each is in, so "why is this feature not
// running" debugging is blind. This is a distinct concern from game_framework.*
// (GameFrameworkHandler.cpp), which authors GameMode/GameState Blueprints and
// never touches the Game Features subsystem.
//
// Registration is UNCONDITIONAL so discovery lists game_features.* even where the
// GameFeatures plugin is not compiled into the running editor — only the handler
// body branches on the MCP_HAS_GAMEFEATURES gate. Build.cs soft-links the
// GameFeatures module via TryAddConditionalModule; the gate follows the presence
// of that module's public header. Mirrors the optional-engine-plugin pattern in
// Handlers/Water/WaterHandler.cpp.
//
// Scope: this file ships only the read-only `list` verb, whose enumeration is
// fully verifiable on any host (an empty array on a host that enables no GF
// plugins still proves the subsystem was queried and the response was shaped).
// State mutation (game_features.set_state) and per-plugin action readback
// (game_features.get_actions) are deferred: both need a GameFeatures-enabled
// host with a live GF plugin to exercise and verify.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
// Compat header, not the raw engine one: UE 5.3 does not define
// UE_VERSION_NEWER_THAN_OR_EQUAL (used by the gate below); Compat back-fills it.
#include "Compat/EngineVersionCompat.h"

// GameFeatures support — conditional on the subsystem's public header being reachable,
// which is true exactly when Build.cs soft-linked the GameFeatures module via
// TryAddConditionalModule (it ships under Plugins/Runtime on 5.4+ and Plugins/Experimental
// on 5.3; the probe finds both). Drives the namespace body.
#if __has_include("GameFeaturesSubsystem.h")
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#define MCP_HAS_GAMEFEATURES 1

// The FGameFeatureInfo struct and its ForEachGameFeature(TFunctionRef<void(FGameFeatureInfo&&)>)
// enumerator were introduced in UE 5.4. On 5.3 the subsystem is equally live (it mounts,
// initializes and scans built-ins) but exposes no enumerator at all — its own listing path
// walks the PRIVATE GameFeaturePluginStateMachines map. Enumerating from exported public
// API instead keeps game_features.list working identically on 5.3 rather than stubbing out
// a capability the host actually has.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#define MCP_GF_HAS_FOREACH 1
#else
#define MCP_GF_HAS_FOREACH 0
#include "Interfaces/IPluginManager.h"
#endif

#else
#define MCP_HAS_GAMEFEATURES 0
#endif

#if MCP_HAS_GAMEFEATURES && !MCP_GF_HAS_FOREACH
namespace
{
    // GameFeaturePluginStatePrivate::LexToString() is declared in GameFeatureTypes.h but
    // defined in the GameFeatures module's .cpp WITHOUT GAMEFEATURES_API, so calling it
    // from this DLL is an LNK2019. GAME_FEATURE_PLUGIN_STATE_LIST is the public X-macro
    // that LexToString itself is generated from, so expanding it here yields byte-identical
    // state names (Installed/Registered/Loaded/Active/...) with no engine symbol to link.
#define PW_GF_STATE_NAME(InEnum, InText) TEXT(#InEnum),
    const TCHAR* const GGameFeatureStateNames[] = {
        GAME_FEATURE_PLUGIN_STATE_LIST(PW_GF_STATE_NAME)
    };
#undef PW_GF_STATE_NAME

    FString GameFeatureStateToString(EGameFeaturePluginState State)
    {
        const int32 Index = static_cast<int32>(State);
        return (Index >= 0 && Index < UE_ARRAY_COUNT(GGameFeatureStateNames))
            ? FString(GGameFeatureStateNames[Index])
            : FString(TEXT("Unknown"));
    }
}
#endif

// ---- game_features.list ----
REGISTER_RPC_HANDLER("game_features.list", "game_features",
    "List every registered Game Feature plugin with its URL and current lifecycle "
    "state (Installed/Registered/Loaded/Active, plus transition/error states). "
    "Read-only. The plugins array is empty on a host that enables no Game Feature "
    "plugins. Distinct from game_framework.* (GameMode/GameState authoring).",
    RPC_NO_PARAMS)
{
#if MCP_HAS_GAMEFEATURES
    UGameFeaturesSubsystem* GameFeatures = GEngine
        ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>()
        : nullptr;
    if (!GameFeatures)
    {
        // The module is linked but the engine subsystem was not created — report it
        // honestly rather than fabricating an empty list (which would falsely imply
        // "zero GF plugins" when the truth is "could not query").
        Ctx.SendError(TEXT("GAME_FEATURES_NOT_AVAILABLE"),
            TEXT("GameFeatures engine subsystem is not available in this editor session."));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Plugins;
#if MCP_GF_HAS_FOREACH
    GameFeatures->ForEachGameFeature([&Plugins](FGameFeatureInfo&& Info)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Info.Name);
        Entry->SetStringField(TEXT("url"), Info.URL);
        Entry->SetStringField(TEXT("state"), UE::GameFeatures::ToString(Info.CurrentState));
        Entry->SetBoolField(TEXT("loadedAsBuiltIn"), Info.bLoadedAsBuiltIn);
        Plugins.Add(MakeShared<FJsonValueObject>(Entry));
    });
#else
    // UE 5.3 rebuild of the same list from exported public API. GetPluginURLByName consults
    // the subsystem's authoritative GF name->URL map (GameFeaturePluginNameToPathMap), so a
    // discovered plugin that resolves is exactly a Game Feature plugin the subsystem tracks
    // and every ordinary plugin is rejected — the same membership ForEachGameFeature walks.
    // GetPluginState(URL) then reports that plugin's live lifecycle state.
    for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
    {
        FString PluginURL;
        if (!GameFeatures->GetPluginURLByName(Plugin->GetName(), PluginURL))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Plugin->GetName());
        Entry->SetStringField(TEXT("url"), PluginURL);
        Entry->SetStringField(TEXT("state"),
            GameFeatureStateToString(GameFeatures->GetPluginState(PluginURL)));
        // FGameFeatureInfo::bLoadedAsBuiltIn has no 5.3 counterpart. The URL protocol carries
        // the same distinction without inventing state: the built-in scan registers plugins
        // under the file: protocol, while remotely-installed ones use installbundle:.
        Entry->SetBoolField(TEXT("loadedAsBuiltIn"),
            UGameFeaturesSubsystem::GetPluginURLProtocol(PluginURL) == EGameFeaturePluginProtocol::File);
        Plugins.Add(MakeShared<FJsonValueObject>(Entry));
    }
#endif

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("plugins"), Plugins);
    Resp->SetNumberField(TEXT("count"), Plugins.Num());
    Ctx.SendSuccess(Resp);
    return true;
#else
    Ctx.SendError(TEXT("GAME_FEATURES_NOT_AVAILABLE"),
        TEXT("GameFeatures module headers were not compiled into PinWright. Enable "
             "the GameFeatures plugin for this engine and rebuild."));
    return true;
#endif
}
