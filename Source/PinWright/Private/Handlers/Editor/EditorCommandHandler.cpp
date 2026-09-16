// Copyright (c) 2026 Alexander Penkin. MIT License.

// EditorCommandHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles editor.console_command, editor.undo, editor.redo, editor.save_all,
// editor.open_asset, editor.close_asset, editor.open_level,
// editor.set_preferences, editor.simulate_input, editor.start_recording,
// editor.stop_recording, editor.create_bookmark, editor.jump_to_bookmark

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ScalabilityConsoleGuard.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Drive/DriveGameInput.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "Handlers/Editor/EditorSaveAllDiagnostic.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Handlers/Level/LevelLoadDiagnostics.h"
#include "Utils/PieState.h"
#include "Utils/AssetUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EditorViewportClient.h"
#include "Bookmarks/IBookmarkTypeTools.h"
#include "Engine/BookmarkBase.h"
#include "Engine/BookMark.h"
#include "Engine/Blueprint.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/GameInstance.h"
#include "ReplaySubsystem.h"
#if __has_include("Subsystems/AssetEditorSubsystem.h")
#include "Subsystems/AssetEditorSubsystem.h"
#elif __has_include("AssetEditorSubsystem.h")
#include "AssetEditorSubsystem.h"
#endif
#if __has_include("FileHelpers.h")
#include "FileHelpers.h"
#endif
#include "Framework/Application/SlateApplication.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "Compat/EngineVersionCompat.h"
#include "Compat/JsonKeyCompat.h"

#if UE_VERSION_OLDER_THAN(5, 6, 0)
#include "NiagaraSystem.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#endif

namespace EditorSaveAllDiagnostic
{

static UBlueprint* FindBlueprintAssetInPackage(UPackage* Package)
{
    UBlueprint* Blueprint = nullptr;
    if (!Package)
    {
        return nullptr;
    }

    ForEachObjectWithPackage(Package, [&Blueprint](UObject* Object) -> bool
    {
        if (!Blueprint && Object && Object->IsAsset())
        {
            Blueprint = Cast<UBlueprint>(Object);
        }
        return Blueprint == nullptr;
    }, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS);

    return Blueprint;
}

static FString FormatIntegrityFailureReason(const TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure>& Failures)
{
    if (Failures.Num() == 0)
    {
        return TEXT("IntegrityFailure");
    }

    const BlueprintHandlerUtils::FBlueprintIntegrityFailure& First = Failures[0];
    return FString::Printf(TEXT("IntegrityFailure: %s %s"),
        *First.NodeKind,
        *First.Reason);
}

#if UE_VERSION_OLDER_THAN(5, 6, 0)
// On UE 5.4 and 5.5, UNiagaraSystem::PostLoad() does
//   ensure(SystemSpawnScript->GetLatestSource() != nullptr);
//   SystemSpawnScript->GetLatestSource()->OnChanged()...   // <- null deref crash
// (NiagaraSystem.cpp:~654). The ensure() does not gate the following deref, so a null
// source still hard-crashes. UEditorAssetLibrary::SaveAsset re-resolves the asset by path
// and runs PostLoad. If the spawn script's source is missing — or is RF_Transient and so
// won't survive serialization — the post-save reload drops it and PostLoad dereferences
// null, hard-crashing the editor (EXCEPTION_ACCESS_VIOLATION reading 0x50). The deref site
// is unchanged on 5.6/5.7, but the suite does not exercise it there; gate 5.4/5.5 only.
// In-memory Niagara fixtures (e.g. the dump-builder tests) attach a transient source, so
// they would crash on save. A genuine asset has a serializable (non-transient) source and
// is unaffected. Returns true (= the package would crash on save) and fills OutReason.
static bool NiagaraSystemWouldCrashOnSave(UPackage* Package, FString& OutReason)
{
    if (!Package)
    {
        return false;
    }

    bool bWouldCrash = false;
    // Do NOT filter on IsAsset(): in-memory test fixtures create the UNiagaraSystem with
    // RF_Transient, for which UObject::IsAsset() returns false, yet the package is still
    // dirty and still routed to SaveAsset (which reloads + PostLoads it → crash).
    ForEachObjectWithPackage(Package, [&bWouldCrash](UObject* Object) -> bool
    {
        const UNiagaraSystem* System = Cast<UNiagaraSystem>(Object);
        if (System)
        {
            const UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
            const UNiagaraScriptSourceBase* Source = SpawnScript ? SpawnScript->GetLatestSource() : nullptr;
            // Null source, or a source that won't serialize, both yield a null source on reload.
            if (Source == nullptr || Source->HasAnyFlags(RF_Transient))
            {
                bWouldCrash = true;
            }
        }
        return !bWouldCrash; // stop iterating once a crashing system is found
    }, MCP_FOREACH_EXCLUDE_NESTED_OBJECTS);

    if (bWouldCrash)
    {
        OutReason = TEXT("IntegrityFailure: NiagaraSystem spawn-script source is missing or transient (would crash on save on UE 5.4/5.5)");
    }
    return bWouldCrash;
}
#endif // UE_VERSION_OLDER_THAN(5, 6, 0)

static bool ValidatePackageIntegrityBeforeSave(UPackage* Package, FString& OutFailureReason)
{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    if (NiagaraSystemWouldCrashOnSave(Package, OutFailureReason))
    {
        return false;
    }
#endif

    UBlueprint* Blueprint = FindBlueprintAssetInPackage(Package);
    if (!Blueprint)
    {
        return true;
    }

    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    if (BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(Blueprint, Failures))
    {
        return true;
    }

    OutFailureReason = FormatIntegrityFailureReason(Failures);
    return false;
}

FString ClassifyFailureReason(UPackage* Package, bool bPieActive)
{
    if (bPieActive)
    {
        return TEXT("BlockedByPie");
    }

    if (Package)
    {
        FString FilePath;
        const FString PackageName = Package->GetName();
        // Resolve the on-disk path only when the package actually exists on disk.
        if (FPackageName::DoesPackageExist(PackageName, &FilePath))
        {
            if (IFileManager::Get().IsReadOnly(*FilePath))
            {
                return TEXT("ReadOnly");
            }
        }
    }

    return TEXT("Unknown");
}

TSharedPtr<FJsonObject> BuildSaveAllResultJson(
    bool bSuccess,
    int32 SavedCount,
    int32 TotalDirty,
    bool bPieActive,
    const TArray<TPair<FString, FString>>& FailedAssetsWithReasons,
    FString& OutErrorMessage)
{
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("success"), bSuccess);
    R->SetNumberField(TEXT("savedCount"), SavedCount);
    R->SetNumberField(TEXT("totalDirty"), TotalDirty);
    R->SetBoolField(TEXT("pieActive"), bPieActive);
    R->SetStringField(TEXT("editorMode"), bPieActive ? TEXT("PIE") : TEXT("Editor"));

    if (FailedAssetsWithReasons.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> FailedArr;
        for (const TPair<FString, FString>& Pair : FailedAssetsWithReasons)
        {
            auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("path"), Pair.Key);
            Entry->SetStringField(TEXT("reason"), Pair.Value);
            FailedArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        R->SetArrayField(TEXT("failedAssets"), FailedArr);
    }

    if (!bSuccess)
    {
        const int32 FailedCount = TotalDirty - SavedCount;
        if (bPieActive)
        {
            OutErrorMessage = FString::Printf(
                TEXT("Saved %d of %d dirty assets (PIE active; %d asset(s) locked by PIE)."),
                SavedCount, TotalDirty, FailedCount);
        }
        else
        {
            OutErrorMessage = FString::Printf(
                TEXT("Saved %d of %d dirty assets."),
                SavedCount, TotalDirty);
        }
    }

    return R;
}

bool SaveDirtyPackagesWithIntegrityGate(TSharedPtr<FJsonObject>& OutResult, FString& OutErrorMessage)
{
    TArray<UPackage*> DirtyPackages;
    FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
    FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);

    const bool bPieActive = PinWrightPieState::IsPlayInEditorActive();
    bool bSuccess = true;
    int32 SavedCount = 0;
    TArray<TPair<FString, FString>> FailedAssets;

    for (UPackage* Package : DirtyPackages)
    {
        if (!Package)
        {
            continue;
        }

        const FString PackagePath = Package->GetPathName();
        FString IntegrityFailureReason;
        if (!ValidatePackageIntegrityBeforeSave(Package, IntegrityFailureReason))
        {
            bSuccess = false;
            FailedAssets.Emplace(PackagePath, IntegrityFailureReason);
            continue;
        }

        if (UEditorAssetLibrary::SaveAsset(PackagePath, false))
        {
            ++SavedCount;
        }
        else
        {
            bSuccess = false;
            FailedAssets.Emplace(PackagePath, ClassifyFailureReason(Package, bPieActive));
        }
    }

    const bool bOverallSuccess = bSuccess || DirtyPackages.Num() == 0;
    OutResult = BuildSaveAllResultJson(
        bOverallSuccess,
        SavedCount,
        DirtyPackages.Num(),
        bPieActive,
        FailedAssets,
        OutErrorMessage);
    return bOverallSuccess;
}

} // namespace EditorSaveAllDiagnostic

// ---- editor.console_command ----
REGISTER_RPC_HANDLER("editor.console_command", "editor", "Execute a console command in the editor world or, via the optional 'world' selector, inside a specific PIE world ('server', 'client', 'client:N', 'pie:N') — the multiplayer-in-PIE routing path (e.g. run 'servertravel ...' in the PIE server world, 'open <ip>' in a PIE client world). Distinct from system.console_command which targets the broader process; use this for editor-, viewport-, and PIE-world-scoped commands. See editor.pie_status for what PIE worlds exist. Returns EXEC_FAILED when no exec handler and no console variable recognised the line (typo, or a command owned by an unloaded module); a success means the line was consumed, NOT that the command's effect succeeded — read that back with a typed verb. A line that SETS a scalability CVar is REFUSED with SCALABILITY_CVAR_USE_TYPED_VERB — either an 'sg.*' group, or any CVar carrying ECVF_Scalability / ECVF_ScalabilityGroup (r.ViewDistanceScale, r.Streaming.PoolSize, r.ScreenPercentage, r.MaxAnisotropy, ...), because a console set pins it at ECVF_SetByConsole above the ECVF_SetByScalability priority the editor's own Settings > Engine Scalability Settings panel writes at, for the rest of the session. Use performance.set_scalability, or pass force:true to accept the pin. READING such a CVar (its name with no value) is not refused, and neither is the aggregate 'scalability N', which routes through Scalability::SetQualityLevels at the panel's own priority.",
    RPC_PARAMS(
        RPC_PARAM_REQ("command", "string", "Full console command line including arguments, e.g. 'Stat Unit' or 'showflag.Bloom 0'."),
        RPC_PARAM_DEF("world", "string", "Target world selector: 'editor' (default, the historical behavior), 'server' (first PIE world with authority — listen/dedicated server, or the sole standalone instance), 'client' (first PIE client), 'client:N' (N-th PIE client, 1-based), or 'pie:N' (raw PIEInstance N).", "editor"),
        RPC_PARAM_DEF("force", "boolean", "Run a scalability-CVar set anyway ('sg.<Group> N' or any ECVF_Scalability CVar), accepting that the CVar is pinned at ECVF_SetByConsole and the editor's own Scalability panel can no longer change its group until the editor restarts. Ignored for every other command.", "false")
    ))
{
  // A console set of ANY scalability-flagged cvar — the sg.* group or one of the ordinary r.*
  // members the group's ini section drives — pins it at ECVF_SetByConsole for the life of the
  // process, permanently outranking the editor's own Scalability panel: a side effect that
  // outlives this call. Refused in favour of the typed verb that writes at the panel's own
  // priority; see Handlers/ScalabilityConsoleGuard.h. Runs ahead of the GEditor check because
  // whether the line pins a cvar is a fact about the string and the console registry, not about
  // the editor being up.
  FString Command = Ctx.GetString(TEXT("command"));
  if (ScalabilityConsoleGuard::IsScalabilityPinningLine(Command) && !Ctx.GetBool(TEXT("force"), false)) {
    Ctx.SendError(ErrorCodes::ERR_SCALABILITY_CVAR_USE_TYPED_VERB,
        ScalabilityConsoleGuard::MakeScalabilityTypedVerbRefusal(Command));
    return true;
  }

  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  if (Command.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("command parameter is required"));
    return true;
  }

  const FString WorldSelector = Ctx.GetString(TEXT("world"));
  const PieWorldSelector::FParsedSelector Selector = PieWorldSelector::Parse(WorldSelector);
  if (Selector.Kind == PieWorldSelector::ESelectorKind::Invalid) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, Selector.Error);
    return true;
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  UWorld* World = nullptr;
  if (Selector.Kind == PieWorldSelector::ESelectorKind::Editor) {
    World = GEditor->GetEditorWorldContext().World();
    if (!World) {
      // Was unchecked: Exec(nullptr, ...) ran and the handler still reported
      // "Console command executed". World-scoped commands silently did nothing.
      Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND,
          TEXT("No editor world is available to execute the command against."));
      return true;
    }
  } else {
    const TArray<PieWorldSelector::FPieContextInfo> PieContexts = PieWorldSelector::GatherPieContexts();
    const int32 MatchIndex = PieWorldSelector::ResolveSelector(Selector, PieContexts);
    if (MatchIndex == INDEX_NONE) {
      Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND,
          FString::Printf(TEXT("No PIE world matches selector '%s'. Available PIE contexts: %s"),
              *WorldSelector, *PieWorldSelector::DescribeContexts(PieContexts)));
      return true;
    }
    const PieWorldSelector::FPieContextInfo& Match = PieContexts[MatchIndex];
    World = Match.World;
    Resp->SetNumberField(TEXT("pieInstance"), Match.PieInstance);
    Resp->SetStringField(TEXT("kind"), PieWorldSelector::ClassifyNetMode(Match.NetMode));
  }

  // UEditorEngine::Exec -> UEngine::Exec returns false only when NOTHING consumed the
  // line: no exec command matched and IConsoleManager::ProcessUserConsoleInput did not
  // recognise it either (UnrealEngine.cpp:5722 returns true for any cvar get/set/help).
  // So a false here means the command or cvar does not exist — a typo, or a name that
  // only exists in a module this build did not load. The return used to be discarded and
  // the handler answered "Console command executed" regardless, which matters more here
  // than anywhere else: this verb is the fallback an agent reaches for when a typed verb
  // fails, so the documented recovery path could not report failure. Same shape as the
  // honest sibling at Handlers/Editor/ViewportHandler.cpp:434.
  if (!GEditor->Exec(World, *Command)) {
    Ctx.SendError(ErrorCodes::ERR_EXEC_FAILED,
        FString::Printf(
            TEXT("No exec command or console variable consumed '%s'. Check the spelling, and "
                 "note that commands owned by an unloaded module are not registered."),
            *Command));
    return true;
  }

  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("command"), Command);
  Resp->SetStringField(TEXT("world"), WorldSelector.IsEmpty() ? TEXT("editor") : *WorldSelector);
  Resp->SetStringField(TEXT("worldPath"), World->GetPathName());
  // "consumed", not "executed": Exec's true means a handler claimed the line, which is
  // the strongest thing this call can honestly observe. It is not a claim that the
  // command's own effect succeeded — a cvar set to an out-of-range value is consumed too.
  Resp->SetBoolField(TEXT("consumed"), true);
  Resp->SetStringField(TEXT("message"),
      TEXT("Console command was consumed by an exec handler or console variable. This confirms "
           "the command was recognised, not that its effect succeeded — verify the effect with a "
           "read-back verb."));
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.undo ----
REGISTER_RPC_HANDLER("editor.undo", "editor", "Undo the most recent editor transaction (Ctrl+Z equivalent). Returns success=false if there is nothing to undo.",
    RPC_NO_PARAMS)
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  const bool bSuccess = GEditor->UndoTransaction();

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("action"), TEXT("undo"));
  Resp->SetBoolField(TEXT("success"), bSuccess);
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.redo ----
REGISTER_RPC_HANDLER("editor.redo", "editor", "Redo the most recently undone editor transaction (Ctrl+Y equivalent). Returns success=false if the redo stack is empty.",
    RPC_NO_PARAMS)
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  const bool bSuccess = GEditor->RedoTransaction();

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("action"), TEXT("redo"));
  Resp->SetBoolField(TEXT("success"), bSuccess);
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.list_dirty_packages ----
REGISTER_RPC_HANDLER("editor.list_dirty_packages", "editor", "Read-only diagnostic: list every dirty (unsaved) content and world package currently in memory (count + package names). No side effects — does NOT save. Use to verify an operation (e.g. asset.dump_folder) left no packages dirty.",
    RPC_NO_PARAMS)
{
  TArray<UPackage*> DirtyPackages;
  FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
  FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);

  TArray<TSharedPtr<FJsonValue>> NamesArr;
  NamesArr.Reserve(DirtyPackages.Num());
  for (const UPackage* Package : DirtyPackages)
  {
    if (Package)
    {
      NamesArr.Add(MakeShared<FJsonValueString>(Package->GetName()));
    }
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetNumberField(TEXT("count"), NamesArr.Num());
  Resp->SetArrayField(TEXT("packages"), NamesArr);
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.save_all ----
REGISTER_RPC_HANDLER("editor.save_all", "editor", "Save every dirty world and content package. Runs synchronously on the game thread and returns the result inline (savedCount, totalDirty, failedAssets). Equivalent to File > Save All.",
    RPC_NO_PARAMS)
{
  if (PinWrightPieState::IsPlayInEditorActive())
  {
    Ctx.SendError(
        ErrorCodes::ERR_PIE_ACTIVE,
        TEXT("editor.save_all cannot run while the editor is in play mode; stop PIE and retry."));
    return true;
  }

  // Dispatcher guarantees IsInGameThread() (RpcDispatcher.cpp:258-266); UE save
  // pipeline is game-thread-bound, so backgrounding via AsyncTask would only
  // defer by one tick without adding concurrency.
  check(IsInGameThread());
  FString ErrorMessage;
  TSharedPtr<FJsonObject> Result;
  const bool bOverallSuccess = EditorSaveAllDiagnostic::SaveDirtyPackagesWithIntegrityGate(
      Result, ErrorMessage);

  if (bOverallSuccess)
  {
      Ctx.SendSuccess(Result);
  }
  else
  {
      Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED, ErrorMessage, Result);
  }
  return true;
}

// ---- editor.open_asset ----
REGISTER_RPC_HANDLER("editor.open_asset", "editor", "Open the named asset in its default editor (Blueprint editor, material editor, etc.). Loads the asset first if it is not already in memory; errors ASSET_NOT_FOUND if the package does not exist.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path of the asset to open, e.g. /Game/Foo/BP_Bar.BP_Bar.")
    ))
{
  FString AssetPath = Ctx.GetString(TEXT("assetPath"));
  if (AssetPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required"));
    return true;
  }

  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  UAssetEditorSubsystem *AssetEditorSS =
      GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
  if (!AssetEditorSS) {
    Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_MISSING, TEXT("AssetEditorSubsystem not available"));
    return true;
  }

  if (!ResolveAsset(AssetPath).bExists) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Asset not found"));
    return true;
  }

  UObject *Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
  if (!Asset) {
    Ctx.SendError(ErrorCodes::ERR_LOAD_FAILED, TEXT("Failed to load asset"));
    return true;
  }

  // A World has no conventional asset-editor tab: UAssetEditorSubsystem::
  // OpenEditorForAsset routes it to UAssetDefinition_World::OpenAssets ->
  // UEditorEngine::Map_Load, which tears down and GCs the active editor world.
  // Reopening the already-active map (e.g. the cold-boot EditorStartupMap) trips
  // !LevelList.Contains(TickTaskLevel) in FTickTaskManager::FreeTickTaskLevel and
  // hard-crashes the editor (B-open-asset-world-map-load-crash). Route Worlds to the
  // dedicated editor.open_level verb, which delegates to level.load: it no-ops when the
  // requested map is already active and otherwise loads it via FEditorFileUtils::LoadMap
  // — never a destroy-and-reload of the live world from under OpenEditorForAsset.
  if (Asset->IsA(UWorld::StaticClass())) {
    if (auto *Sub = Ctx.GetSubsystem()) {
      // A loaded, existence-checked asset always resolves to its UPackage, so
      // GetOutermost() is non-null — take the bare package path (/Game/Maps/...)
      // directly. (Never AssetPath: that is the object path /Game/Maps/X.X, and
      // open_level only strips a .umap suffix, not the .X object suffix, so it
      // would mangle the path instead of no-op'ing on the active map.)
      const FString LevelPackagePath = Asset->GetOutermost()->GetName();
      TSharedPtr<FJsonObject> ForwardPayload = MakeShared<FJsonObject>();
      ForwardPayload->SetStringField(TEXT("levelPath"), LevelPackagePath);
      return Sub->DispatchMethod(TEXT("editor.open_level"), Ctx.GetRequestId(), ForwardPayload);
    }
    Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_NOT_FOUND, TEXT("Subsystem not available for dispatch"));
    return true;
  }

  const bool bOpened = AssetEditorSS->OpenEditorForAsset(Asset);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), bOpened);
  Resp->SetStringField(TEXT("assetPath"), AssetPath);

  if (bOpened) {
    Ctx.SendSuccess(Resp);
  } else {
    Ctx.SendError(ErrorCodes::ERR_OPEN_FAILED, TEXT("Failed to open asset editor"));
  }
  return true;
}

// ---- editor.close_asset ----
REGISTER_RPC_HANDLER("editor.close_asset", "editor", "Close every editor window currently displaying the named asset. Loads the asset to obtain its UObject; idempotent if no editor is open for it.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path of the asset whose editors should be closed, e.g. /Game/Foo/BP_Bar.BP_Bar.")
    ))
{
  FString AssetPath = Ctx.GetString(TEXT("assetPath"));
  if (AssetPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required"));
    return true;
  }

  UAssetEditorSubsystem* AssetEditorSS = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
  if (!AssetEditorSS) {
    Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_MISSING, TEXT("AssetEditorSubsystem unavailable"));
    return true;
  }

  UObject* Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
  if (!Asset) {
    Ctx.SendError(ErrorCodes::ERR_LOAD_FAILED, TEXT("Failed to load asset"));
    return true;
  }

  AssetEditorSS->CloseAllEditorsForAsset(Asset);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("assetPath"), AssetPath);
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.open_level ----
REGISTER_RPC_HANDLER("editor.open_level", "editor", "Load and open a level (.umap) in the editor as the active world. Synchronous: the call returns once the map is fully loaded. Accepts paths with or without the /Game/ prefix and with or without the .umap extension.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("levelPath"), TEXT("path"),
            TEXT("Path to the level package, e.g. /Game/Maps/MyLevel. Bare names are treated as /Game/<name>; the .umap suffix is optional. The generic path spelling is also accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("levelPath"), TEXT("path")}))
    ))
{
  FString LevelPath = Ctx.GetStringFirstOf({TEXT("levelPath"), TEXT("path")});
  if (LevelPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("levelPath required"));
    return true;
  }

  if (!IsValidMountPoint(LevelPath)) {
    // `TEXT("/Game") / P`, never Printf("/Game/%s"). Printf is concatenation by another spelling:
    // it MANUFACTURES a "//" whenever P already carries a leading slash, and that byte sequence is
    // what CreatePackage logs Fatal on. This branch could not fire on a rooted path while
    // IsValidMountPoint short-circuited TRUE on anything merely STARTING with "/Game"; now that
    // the predicate is segment-aware, a rooted-but-unmounted levelPath ("/GameFoo/L") reaches it.
    // FString::operator/ routes through PathAppend, which absorbs the duplicate separator.
    LevelPath = FString(TEXT("/Game")) / LevelPath;
  }

  if (LevelPath.EndsWith(TEXT(".umap"))) {
    LevelPath.LeftChopInline(5);
  }

  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  // Mount-aware resolution: /Game -> project content, /Engine -> engine content,
  // plugin roots -> their real content dirs. Replaces the old RightChop(6) +
  // ProjectContentDir() string-chop that assumed a fixed 6-char /Game/ prefix and
  // mangled every non-/Game mount (e.g. /Engine/Maps/... -> Content/e/Maps/...).
  FString FullMapPath;
  if (!ResolveLevelPackageToMapFilename(LevelPath, FullMapPath)) {
    Ctx.SendError(ErrorCodes::ERR_FILE_NOT_FOUND,
        FString::Printf(TEXT("Level path has no registered mount point: %s"), *LevelPath));
    return true;
  }
  FullMapPath = FPaths::ConvertRelativePathToFull(FullMapPath);

  if (!FPaths::FileExists(FullMapPath)) {
    // This disk gate fires BEFORE the cross-dispatch to level.load below, so the
    // same in-memory/registry-vs-disk split level.load does must be applied here
    // too — otherwise an unsaved-in-memory world short-circuits to a flat
    // FILE_NOT_FOUND and never reaches level.load's LEVEL_NOT_PERSISTED verdict.
    // Single-sourced through the shared diagnostic helper so the verdict and the
    // user-facing message stay identical to level.load (open_level shows the
    // resolved on-disk filename in the FILE_NOT_FOUND message; level.load shows
    // the package path — a cosmetic difference).
    return SendLevelNotLoadableError(Ctx, LevelPath, FullMapPath);
  }

  // editor.open_level is the editor-domain alias of level.load: both load a map
  // synchronously via FEditorFileUtils::LoadMap and return the same load-result
  // shape. To keep that contract single-sourced (one place computes the `loaded`
  // flag via DoesRequestedLevelMatchCurrentWorld and runs VerifyAssetExists),
  // delegate to the level.load handler over the established cross-dispatch path
  // (Ctx.GetSubsystem()->DispatchMethod, the same mechanism the ~25 alias handlers
  // in this file family use) instead of duplicating the load + response block.
  // The mount-aware path validation above already produced a clear FILE_NOT_FOUND
  // for unmounted/missing paths; level.load then performs the synchronous load and
  // emits the response on this request id. This removes the leaked-job bug that
  // the old Ctx.StartJob + OnMapOpened wrapper caused
  // (B-editor-open-level-no-completion-signal): level.load is fully synchronous.
  if (auto* Sub = Ctx.GetSubsystem())
  {
      TSharedPtr<FJsonObject> ForwardPayload = MakeShared<FJsonObject>();
      ForwardPayload->SetStringField(TEXT("levelPath"), LevelPath);
      return Sub->DispatchMethod(TEXT("level.load"), Ctx.GetRequestId(), ForwardPayload);
  }
  Ctx.SendError(ErrorCodes::ERR_SUBSYSTEM_NOT_FOUND, TEXT("Subsystem not available for dispatch"));
  return true;
}

// ---- editor.set_preferences ----
REGISTER_RPC_HANDLER("editor.set_preferences", "editor", "Set multiple console variables in one call by passing a {cvarName: value} map. Values may be string, number, or bool. Reports applied[] and failed[] arrays separately so partial application can be observed.",
    RPC_PARAMS(
        RPC_PARAM_REQ("preferences", "object", "Map of console variable name to desired value. Values are string/number/bool; unknown CVars land in the 'failed' list.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  TArray<FString> AppliedSettings;
  TArray<FString> FailedSettings;

  TSharedPtr<FJsonObject> PrefsObj;
  if (!Ctx.RequireObject(TEXT("preferences"), PrefsObj))
  {
    return true;  // RequireObject auto-sends error
  }

  for (const auto& Pair : PrefsObj->Values) {
      IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Pair.Key);
      if (CVar) {
        FString Value;
        if (Pair.Value->TryGetString(Value)) {
          CVar->Set(*Value);
          AppliedSettings.Add(EARGCompat::JsonKeyToString(Pair.Key));
        } else {
          double NumVal;
          if (Pair.Value->TryGetNumber(NumVal)) {
            CVar->Set((float)NumVal);
            AppliedSettings.Add(EARGCompat::JsonKeyToString(Pair.Key));
          } else {
            bool BoolVal;
            if (Pair.Value->TryGetBool(BoolVal)) {
              CVar->Set(BoolVal ? 1 : 0);
              AppliedSettings.Add(EARGCompat::JsonKeyToString(Pair.Key));
            } else {
              FailedSettings.Add(EARGCompat::JsonKeyToString(Pair.Key));
            }
          }
        }
      } else {
        FailedSettings.Add(EARGCompat::JsonKeyToString(Pair.Key));
      }
    }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), FailedSettings.Num() == 0);
  Resp->SetNumberField(TEXT("appliedCount"), AppliedSettings.Num());

  if (AppliedSettings.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> AppliedArray;
    for (const FString& Name : AppliedSettings)
      AppliedArray.Add(MakeShared<FJsonValueString>(Name));
    Resp->SetArrayField(TEXT("applied"), AppliedArray);
  }

  if (FailedSettings.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> FailedArray;
    for (const FString& Name : FailedSettings)
      FailedArray.Add(MakeShared<FJsonValueString>(Name));
    Resp->SetArrayField(TEXT("failed"), FailedArray);
  }

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.simulate_input ----
REGISTER_RPC_HANDLER("editor.simulate_input", "editor", "Inject a synthetic keyboard or mouse event. Key events go to a RUNNING PIE GAME when one exists: the session's game viewport is put on the keyboard focus path first, the key then travels the real route (focused in-game UMG widget, then the viewport client, then the possessed player's input stack), and the response names the world / player controller / pawn that received it plus whether the player controller registered it. With no PIE session (or target:'editor') the key goes to the focused editor widget instead. mouse_click/mouse_move take screen-space coordinates and always route through Slate.",
    RPC_PARAMS(
        RPC_PARAM_REQ("type", "string", "Event kind: 'key_down', 'key_up', 'mouse_click', or 'mouse_move' (case-insensitive aliases also accepted)."),
        RPC_PARAM_OPT("key", "string", "FKey name for key events, e.g. 'A', 'SpaceBar', 'LeftShift'."),
        RPC_PARAM_OPT("x", "number", "Screen-space X coordinate for mouse events, in pixels."),
        RPC_PARAM_OPT("y", "number", "Screen-space Y coordinate for mouse events, in pixels."),
        RPC_PARAM_OPT("button", "string", "Mouse button for click events: 'left' (default), 'right', or 'middle'."),
        RPC_PARAM_DEF("target", "string", "Key destination: 'auto' (default — the running PIE game when there is one, else the editor), 'game' (the PIE game; PIE_NOT_ACTIVE when PIE is not running), or 'editor' (the focused editor widget). Ignored by mouse events.", "auto"),
        RPC_PARAM_OPT("world", "string", "PIE world selector for key events aimed at the game, same grammar as editor.console_command: 'server', 'client', 'client:N' (1-based), 'pie:N'. Default picks the active game viewport (preferring one that has a player controller).")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  FString InputType = Ctx.GetString(TEXT("type")).ToLower();
  FString Key = Ctx.GetString(TEXT("key"));

  bool bSuccess = false;
  FString Message;

  const bool bKeyDown = (InputType == TEXT("key_down") || InputType == TEXT("keydown"));
  const bool bKeyUp = (InputType == TEXT("key_up") || InputType == TEXT("keyup"));
  if (bKeyDown || bKeyUp) {
    // Key events are routed by FDriveGameInput when a game is running, so a key aimed at a
    // PIE session reaches the possessed pawn's input stack rather than whatever editor widget
    // happens to hold Slate focus (board B-simulate-input-key-events-never-reach-pie-pawn).
    // The reverted branch dispatched a bare FSlateApplication::ProcessKeyDownEvent along the
    // editor's focus path, discarded the handled result, and hardcoded success — so a key sent
    // into a live PIE session changed nothing and still answered {success:true}.
    if (Key.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("Key parameter required for %s"), *InputType));
      return true;
    }
    const FKey InputKey(*Key);
    if (!EKeys::GetKeyDetails(InputKey).IsValid()) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_KEY, FString::Printf(TEXT("Invalid key: %s"), *Key));
      return true;
    }

    const FString TargetToken = Ctx.GetString(TEXT("target"), TEXT("auto")).ToLower();
    if (TargetToken != TEXT("auto") && TargetToken != TEXT("game") && TargetToken != TEXT("editor")) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("Unknown target '%s'. Valid: 'auto' (default), 'game', 'editor'."), *TargetToken));
      return true;
    }
    const FString WorldSelector = Ctx.GetString(TEXT("world"));

    FDriveGameInputTarget GameTarget;
    bool bHaveGameTarget = false;
    if (TargetToken != TEXT("editor")) {
      FString ResolveErrorCode;
      FString ResolveErrorMessage;
      bHaveGameTarget = FDriveGameInput::ResolveTarget(WorldSelector, GameTarget, ResolveErrorCode, ResolveErrorMessage);
      // 'game' asked for the game explicitly, and so did 'auto' with a named PIE world: neither
      // may silently degrade into an editor keystroke.
      if (!bHaveGameTarget && (TargetToken == TEXT("game") || !WorldSelector.IsEmpty())) {
        Ctx.SendError(ResolveErrorCode, ResolveErrorMessage);
        return true;
      }
    }

    const EDriveKeyAction Action = bKeyDown ? EDriveKeyAction::Down : EDriveKeyAction::Up;
    const TCHAR* Verb = bKeyDown ? TEXT("Key down") : TEXT("Key up");

    TSharedPtr<FJsonObject> KeyResp = MakeShared<FJsonObject>();
    KeyResp->SetStringField(TEXT("type"), InputType);
    KeyResp->SetStringField(TEXT("key"), Key);

    if (bHaveGameTarget) {
      APawn* const Pawn = GameTarget.PlayerController ? GameTarget.PlayerController->GetPawn() : nullptr;
      KeyResp->SetStringField(TEXT("target"), TEXT("game"));
      KeyResp->SetStringField(TEXT("route"), TEXT("viewport_client"));
      KeyResp->SetNumberField(TEXT("pieInstance"), GameTarget.PieInstance);
      KeyResp->SetStringField(TEXT("kind"), PieWorldSelector::ClassifyNetMode(GameTarget.NetMode));
      KeyResp->SetStringField(TEXT("netMode"), PieWorldSelector::NetModeToString(GameTarget.NetMode));
      KeyResp->SetStringField(TEXT("worldPath"), GameTarget.World ? GameTarget.World->GetPathName() : TEXT(""));
      KeyResp->SetStringField(TEXT("map"),
          GameTarget.World ? UWorld::RemovePIEPrefix(GameTarget.World->GetMapName()) : TEXT(""));
      KeyResp->SetStringField(TEXT("playerController"),
          GameTarget.PlayerController ? GameTarget.PlayerController->GetPathName() : TEXT("none"));
      KeyResp->SetStringField(TEXT("pawn"), Pawn ? Pawn->GetPathName() : TEXT("none"));

      if (!GameTarget.PlayerController || !GameTarget.PlayerController->PlayerInput) {
        KeyResp->SetBoolField(TEXT("success"), false);
        Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED,
            TEXT("The selected PIE target has no local UPlayerInput to observe."), KeyResp);
        return true;
      }

      const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
      const TSharedRef<TSharedPtr<FAsyncRequestLifetimeLease>> LeaseHolder =
          MakeShared<TSharedPtr<FAsyncRequestLifetimeLease>>();
      const TSharedRef<TWeakPtr<FDriveGameInputRequest>> RequestHolder =
          MakeShared<TWeakPtr<FDriveGameInputRequest>>();
      TSharedPtr<FDriveGameInputRequest> Request = FDriveGameInput::BeginDeliverKey(
          GameTarget, InputKey, Action,
          [Token, KeyResp, LeaseHolder, VerbString = FString(Verb), Key](
              const FDriveGameKeyObservation& Observation)
          {
            if (LeaseHolder->IsValid() && (*LeaseHolder)->IsActive()) {
              (*LeaseHolder)->Release();
            }
            KeyResp->SetBoolField(TEXT("handled"), Observation.bHandled);
            KeyResp->SetBoolField(TEXT("playerInputEventQueued"), Observation.bEventQueued);
            KeyResp->SetNumberField(TEXT("playerInputEventId"), Observation.EventId);
            KeyResp->SetNumberField(TEXT("injectionFrame"), Observation.InjectionFrame);
            KeyResp->SetBoolField(TEXT("deliveredToGame"), Observation.bDeliveredToGame);
            KeyResp->SetStringField(TEXT("consumingRoute"), Observation.ConsumingRoute);
            KeyResp->SetBoolField(TEXT("success"), Observation.bDeliveredToGame);
            if (Observation.bDeliveredToGame) {
              KeyResp->SetStringField(TEXT("message"), FString::Printf(
                  TEXT("%s: %s reached the selected PIE UPlayerInput on its next tick."),
                  *VerbString, *Key));
              Token->SendSuccess(KeyResp);
            } else {
              Token->SendError(ErrorCodes::ERR_INPUT_FAILED, FString::Printf(
                  TEXT("%s '%s' was not processed by the selected PIE UPlayerInput (consumingRoute=%s)."),
                  *VerbString, *Key, *Observation.ConsumingRoute), KeyResp);
            }
          });
      if (!Request.IsValid()) {
        KeyResp->SetBoolField(TEXT("success"), false);
        Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED,
            TEXT("The selected PIE input observation could not be started."), KeyResp);
        return true;
      }
      *RequestHolder = Request;
      *LeaseHolder = Ctx.RetainAsyncRequestLifetime([RequestHolder]()
      {
        if (const TSharedPtr<FDriveGameInputRequest> Pending = RequestHolder->Pin()) {
          Pending->Cancel(TEXT("dispatcher_ended"));
        }
      });
      return true;
    }

    // Editor destination: the focused editor widget, the historical behavior — but with the
    // handled result reported instead of assumed, so "delivered and ignored" is visible.
    bool bHandled = false;
    const bool bInjected = FDriveInput::PressKeyReportingHandled(
        InputKey, EDriveModifierKeys::None, Action, bHandled);
    const FString FocusedWidget = FDriveGameInput::DescribeKeyboardFocus();

    KeyResp->SetStringField(TEXT("target"), TEXT("editor"));
    KeyResp->SetStringField(TEXT("route"), bInjected ? TEXT("slate") : TEXT("none"));
    KeyResp->SetBoolField(TEXT("handled"), bHandled);
    KeyResp->SetStringField(TEXT("focusedWidget"), FocusedWidget);
    KeyResp->SetBoolField(TEXT("success"), bInjected);

    if (!bInjected) {
      Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED,
          TEXT("Slate application is not available to inject the key event"), KeyResp);
      return true;
    }
    KeyResp->SetStringField(TEXT("message"), FString::Printf(
        TEXT("%s: %s -> editor focus path (%s), handled=%s"),
        Verb, *Key, *FocusedWidget, bHandled ? TEXT("true") : TEXT("false")));
    Ctx.SendSuccess(KeyResp);
    return true;
  }

  if (InputType == TEXT("mouse_click") || InputType == TEXT("click")) {
    double X = Ctx.GetNumber(TEXT("x"), 0.0);
    double Y = Ctx.GetNumber(TEXT("y"), 0.0);

    FString Button = Ctx.GetString(TEXT("button"));
    if (Button.IsEmpty()) Button = TEXT("left");

    const EDriveMouseButton MouseButton = FDriveInput::ParseMouseButton(Button);

    // Route through the vetted FDriveInput click primitive rather than a bare
    // Slate injection: it resolves the native window under the point, enables
    // device input while the editor is not the active OS window, dispatches a
    // real mouse-move first, and builds a full FPointerEvent WITH the effecting
    // button. The old inline path passed a nullptr window and an effecting-button-
    // less pointer event, so synthesized clicks silently missed the widget under
    // the cursor (including SViewport-hosted CEF browsers, whose own viewport
    // forwards a routed Slate click into the DOM). bWasHandled captures whether a
    // widget actually consumed the press, so success is reported honestly instead
    // of the previous unconditional bSuccess = true (a click that landed on
    // nothing used to still report success — a silent false-success).
    bool bWasHandled = false;
    const bool bInjected = FDriveInput::ClickAtReportingHandled(
        FVector2D((float)X, (float)Y), MouseButton, bWasHandled);

    if (!bInjected) {
      Message = TEXT("Slate application is not available to inject the mouse click");
    } else if (bWasHandled) {
      bSuccess = true;
      Message = FString::Printf(TEXT("Mouse click at (%f, %f) was handled by a widget"), X, Y);
    } else {
      Message = FString::Printf(
          TEXT("Mouse click at (%f, %f) reached no interactive widget (nothing under the cursor consumed it)"), X, Y);
    }
  } else if (InputType == TEXT("mouse_move") || InputType == TEXT("move")) {
    double X = Ctx.GetNumber(TEXT("x"), 0.0);
    double Y = Ctx.GetNumber(TEXT("y"), 0.0);

    FSlateApplication& SlateApp = FSlateApplication::Get();
    FVector2D Position((float)X, (float)Y);
    SlateApp.SetCursorPos(Position);

    bSuccess = true;
    Message = FString::Printf(TEXT("Mouse moved to (%f, %f)"), X, Y);
  } else {
    Message = FString::Printf(TEXT("Unknown input type: %s. Supported: key_down, key_up, mouse_click, mouse_move"), *InputType);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), bSuccess);
  Resp->SetStringField(TEXT("type"), InputType);
  Resp->SetStringField(TEXT("message"), Message);

  if (bSuccess) {
    Ctx.SendSuccess(Resp);
  } else {
    Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED, Message);
  }
  return true;
}

// ---- editor.start_recording ----
REGISTER_RPC_HANDLER("editor.start_recording", "editor", "Begin a network demo recording in the active PIE game world. Refuses edit mode with NO_ACTIVE_GAME_WORLD, starts through the GameInstance replay API, and returns success only when the replay subsystem reports that recording is active.",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "filepath", "Filename for the .demo replay; defaults to 'Recording_<timestamp>'.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  FString RequestedRecordingName = Ctx.GetString(TEXT("name"));
  if (RequestedRecordingName.IsEmpty()) {
    RequestedRecordingName = FString::Printf(TEXT("Recording_%s"),
        *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
  }

  UWorld* World = GEditor->PlayWorld.Get();
  UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
  if (!World || !GameInstance) {
    TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
    ErrData->SetBoolField(TEXT("pieActive"), World != nullptr);
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_GAME_WORLD,
        TEXT("editor.start_recording requires an active PIE world with a GameInstance; call editor.play first"),
        ErrData);
    return true;
  }

  UReplaySubsystem* ReplaySubsystem = GameInstance->GetSubsystem<UReplaySubsystem>();
  GameInstance->StartRecordingReplay(RequestedRecordingName, RequestedRecordingName);
  if (!ReplaySubsystem || !ReplaySubsystem->IsRecording()) {
    TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
    ErrData->SetBoolField(TEXT("recording"), false);
    ErrData->SetStringField(TEXT("requestedRecordingName"), RequestedRecordingName);
    ErrData->SetStringField(TEXT("worldPath"), World->GetPathName());
    Ctx.SendError(ErrorCodes::ERR_REPLAY_RECORDING_FAILED,
        TEXT("The replay API returned without an active recording"), ErrData);
    return true;
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetBoolField(TEXT("recording"), true);
  Resp->SetStringField(TEXT("requestedRecordingName"), RequestedRecordingName);
  Resp->SetStringField(TEXT("recordingName"), ReplaySubsystem->GetActiveReplayName());
  if (const UDemoNetDriver* DemoNetDriver = World->GetDemoNetDriver()) {
    Resp->SetStringField(TEXT("recordingBasePath"), DemoNetDriver->GetDemoPath());
  }
  Resp->SetStringField(TEXT("worldPath"), World->GetPathName());
  Resp->SetStringField(TEXT("message"), TEXT("Recording started"));
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.stop_recording ----
REGISTER_RPC_HANDLER("editor.stop_recording", "editor", "Stop the active network demo recording via the 'DemoStop' console command. Idempotent if no recording is in progress.",
    RPC_NO_PARAMS)
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  UWorld* World = GEditor->PlayWorld ? GEditor->PlayWorld.Get() : GEditor->GetEditorWorldContext().World();
  if (World) {
    GEditor->Exec(World, TEXT("DemoStop"));
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("message"), TEXT("Recording stopped"));
  Ctx.SendSuccess(Resp);
  return true;
}

// The bookmark array lives on the world's AWorldSettings, reached through the
// active level-editor viewport client. Resolve it the same way the working
// editor.set_camera / ForceRedrawActiveViewport paths do — via the shared
// EditorHandlerUtils resolver — and require a client whose world resolves so
// IBookmarkTypeTools can reach GetWorldSettings()->GetBookmarks(). Bookmarks are
// NOT routable through GEditor->Exec (SetBookmark/JumpToBookmark are level-
// viewport input-chain commands, not UEngine Exec verbs), which is why the old
// Exec path was a silent no-op.
static FEditorViewportClient* ResolveBookmarkViewportClient()
{
  return EditorHandlerUtils::ResolveActiveLevelViewportClient(/*bRequireWorld=*/true);
}

// ---- editor.create_bookmark ----
REGISTER_RPC_HANDLER("editor.create_bookmark", "editor", "Save the current viewport camera transform into one of the 10 indexed level bookmarks (Ctrl+0..9 in the editor). Overwrites any existing bookmark at that index.",
    RPC_PARAMS(
        RPC_PARAM_OPT("index", "integer", "Bookmark slot 0..9; clamped to that range. Defaults to 0.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  int32 BookmarkIndex = Ctx.GetInt(TEXT("index"), 0);
  BookmarkIndex = FMath::Clamp(BookmarkIndex, 0, 9);

  FEditorViewportClient* ViewportClient = ResolveBookmarkViewportClient();
  if (!ViewportClient) {
    Ctx.SendError(ErrorCodes::ERR_VIEWPORT_NOT_AVAILABLE,
        TEXT("No active level-editor viewport to capture a bookmark from"));
    return true;
  }

  // Write the bookmark through the same engine API the editor's Ctrl+0..9
  // keybinding uses; this captures the live viewport camera into the world's
  // AWorldSettings bookmark array (success here means a real bookmark exists).
  IBookmarkTypeTools& BookmarkTools = IBookmarkTypeTools::Get();
  BookmarkTools.CreateOrSetBookmark(static_cast<uint32>(BookmarkIndex), ViewportClient);

  if (!BookmarkTools.CheckBookmark(static_cast<uint32>(BookmarkIndex), ViewportClient)) {
    Ctx.SendError(ErrorCodes::ERR_BOOKMARK_SET_FAILED,
        FString::Printf(TEXT("Failed to store bookmark %d"), BookmarkIndex));
    return true;
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetNumberField(TEXT("index"), BookmarkIndex);
  Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Bookmark %d created"), BookmarkIndex));
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.jump_to_bookmark ----
REGISTER_RPC_HANDLER("editor.jump_to_bookmark", "editor", "Move the viewport camera to a previously stored bookmark slot (0..9). Fails if the slot is empty.",
    RPC_PARAMS(
        RPC_PARAM_OPT("index", "integer", "Bookmark slot 0..9; clamped to that range. Defaults to 0.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  int32 BookmarkIndex = Ctx.GetInt(TEXT("index"), 0);
  BookmarkIndex = FMath::Clamp(BookmarkIndex, 0, 9);

  FEditorViewportClient* ViewportClient = ResolveBookmarkViewportClient();
  if (!ViewportClient) {
    Ctx.SendError(ErrorCodes::ERR_VIEWPORT_NOT_AVAILABLE,
        TEXT("No active level-editor viewport to move"));
    return true;
  }

  // An empty slot is an honest error, not a fake-success no-op: the engine's
  // JumpToBookmark silently does nothing when the slot is null, so guard it.
  IBookmarkTypeTools& BookmarkTools = IBookmarkTypeTools::Get();
  if (!BookmarkTools.CheckBookmark(static_cast<uint32>(BookmarkIndex), ViewportClient)) {
    Ctx.SendError(ErrorCodes::ERR_BOOKMARK_EMPTY,
        FString::Printf(TEXT("No bookmark stored at slot %d"), BookmarkIndex));
    return true;
  }

  // Move the camera via the same engine API as the editor's Ctrl+0..9 recall,
  // then flush a synchronous redraw so the recalled pose is observable now (the
  // shared ForceRedrawViewportClient idiom, reusing the already-resolved client).
  TSharedPtr<FBookmarkBaseJumpToSettings> JumpSettings = MakeShared<FBookmarkJumpToSettings>();
  BookmarkTools.JumpToBookmark(static_cast<uint32>(BookmarkIndex), JumpSettings, ViewportClient);
  EditorHandlerUtils::ForceRedrawViewportClient(ViewportClient);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetNumberField(TEXT("index"), BookmarkIndex);
  Resp->SetStringField(TEXT("message"), FString::Printf(TEXT("Jumped to bookmark %d"), BookmarkIndex));
  Ctx.SendSuccess(Resp);
  return true;
}
