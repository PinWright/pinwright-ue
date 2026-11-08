// Copyright (c) 2026 Alexander Penkin. MIT License.

// EditorCommandHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles editor.undo, editor.redo, editor.undo_history, editor.save_all,
// editor.open_asset, editor.close_asset, editor.open_level,
// editor.set_preferences, editor.start_recording,
// editor.stop_recording, editor.create_bookmark, editor.jump_to_bookmark.
// editor.console_command and editor.simulate_input are in the PinWrightRuntime module.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Dispatch/SafePoint.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "Handlers/Editor/EditorSaveAllDiagnostic.h"
#include "Handlers/Level/LevelLoadDiagnostics.h"
#include "Utils/PieState.h"
#include "Utils/AssetUtils.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "EditorAssetLibrary.h"
#include "EditorViewportClient.h"
#include "Bookmarks/IBookmarkTypeTools.h"
#include "Engine/BookmarkBase.h"
#include "Engine/BookMark.h"
#include "Engine/Blueprint.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
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
#include "Containers/Ticker.h"
#include "Widgets/SWindow.h"
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

        // SaveAsset resolves an asset through the registry and logs an Error for a package
        // that holds none (emptied by a rename or delete, or only transient objects), so it
        // could only fail here; report it without the call.
        if (!Package->FindAssetInPackage())
        {
            bSuccess = false;
            FailedAssets.Emplace(PackagePath, TEXT("NoAsset"));
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

namespace EditorUndoHistory
{

// One transaction-buffer entry as the wire sees it. Index is the UTransactor queue index
// (0 = oldest), so an entry reads the same in undo_history and in an undo/redo response.
static TSharedPtr<FJsonObject> DescribeTransaction(const UTransactor& Trans, int32 QueueIndex)
{
  TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
  Entry->SetNumberField(TEXT("index"), QueueIndex);
  if (const FTransaction* Transaction = Trans.GetTransaction(QueueIndex)) {
    const FTransactionContext Context = Transaction->GetContext();
    Entry->SetStringField(TEXT("title"), Context.Title.ToString());
    Entry->SetStringField(TEXT("context"), Context.Context);
    Entry->SetStringField(TEXT("id"), Context.TransactionId.ToString(EGuidFormats::DigitsWithHyphens));
    if (Context.PrimaryObject) {
      Entry->SetStringField(TEXT("primaryObject"), Context.PrimaryObject->GetPathName());
    }
  }
  return Entry;
}

// Shared body of editor.undo and editor.redo: applies up to `steps` transactions one engine call
// at a time and reports each one actually applied. Stops at the first step the engine refuses.
static bool RunSteps(FHandlerContext& Ctx, bool bRedo)
{
  if (!GEditor || !GEditor->Trans) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor transaction buffer not available"));
    return true;
  }

  const int32 Steps = Ctx.GetInt(TEXT("steps"), 1);
  if (Steps < 1) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("steps must be >= 1 (got %d)."), Steps));
    return true;
  }

  UTransactor* Trans = GEditor->Trans;
  TArray<TSharedPtr<FJsonValue>> Applied;
  FString StoppedReason;
  for (int32 Step = 0; Step < Steps; ++Step) {
    FText Blocked;
    if (!(bRedo ? Trans->CanRedo(&Blocked) : Trans->CanUndo(&Blocked))) {
      StoppedReason = Blocked.ToString();
      break;
    }
    if (!(bRedo ? GEditor->RedoTransaction() : GEditor->UndoTransaction())) {
      StoppedReason = FString::Printf(
          TEXT("The editor declined the %s: a package save or garbage collection is in progress, or every remaining transaction has expired."),
          bRedo ? TEXT("redo") : TEXT("undo"));
      break;
    }
    // One engine Undo/Redo can step over expired transactions before applying one, so the applied
    // entry is read at the position the buffer ended on, not predicted from where it started.
    const int32 QueueLength = Trans->GetQueueLength();
    const int32 UndoCount = Trans->GetUndoCount();
    const int32 AppliedIndex = bRedo ? QueueLength - UndoCount - 1 : QueueLength - UndoCount;
    Applied.Add(MakeShared<FJsonValueObject>(DescribeTransaction(*Trans, AppliedIndex)));
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("action"), bRedo ? TEXT("redo") : TEXT("undo"));
  Resp->SetBoolField(TEXT("success"), Applied.Num() == Steps);
  Resp->SetNumberField(TEXT("requestedSteps"), Steps);
  Resp->SetNumberField(TEXT("completedSteps"), Applied.Num());
  Resp->SetArrayField(bRedo ? TEXT("redone") : TEXT("undone"), Applied);
  Resp->SetNumberField(TEXT("queueLength"), Trans->GetQueueLength());
  Resp->SetNumberField(TEXT("undoCount"), Trans->GetUndoCount());
  if (!StoppedReason.IsEmpty()) {
    Resp->SetStringField(TEXT("stoppedReason"), StoppedReason);
  }

  if (Applied.Num() == 0) {
    Ctx.SendError(bRedo ? ErrorCodes::ERR_NOTHING_TO_REDO : ErrorCodes::ERR_NOTHING_TO_UNDO,
        FString::Printf(TEXT("Nothing was %s: %s Read editor.undo_history to see the buffer."),
            bRedo ? TEXT("redone") : TEXT("undone"), *StoppedReason),
        Resp);
    return true;
  }
  Ctx.SendSuccess(Resp);
  return true;
}

} // namespace EditorUndoHistory

// ---- editor.undo ----
REGISTER_RPC_HANDLER("editor.undo", "editor", "Undo the most recent editor transaction(s) (Ctrl+Z equivalent). Returns undone[] with the index, title, context, id and primaryObject of every transaction actually undone, newest first; success is true only when all requested steps ran, and stoppedReason carries the engine's reason when fewer did. Zero steps undone is NOTHING_TO_UNDO (empty undo side, undo barrier, or a transaction in progress) with the same payload. See editor.undo_history for the buffer.",
    RPC_PARAMS(
        RPC_PARAM_DEF("steps", "integer", "How many transactions to undo, one engine undo per step; must be >= 1. Stops early at the first step the engine refuses.", "1")
    ))
{
  return EditorUndoHistory::RunSteps(Ctx, /*bRedo=*/false);
}

// ---- editor.redo ----
REGISTER_RPC_HANDLER("editor.redo", "editor", "Redo the most recently undone editor transaction(s) (Ctrl+Y equivalent). Returns redone[] with the index, title, context, id and primaryObject of every transaction actually redone, in redo order; success is true only when all requested steps ran, and stoppedReason carries the engine's reason when fewer did. Zero steps redone is NOTHING_TO_REDO with the same payload. Any new transaction discards the redo side.",
    RPC_PARAMS(
        RPC_PARAM_DEF("steps", "integer", "How many transactions to redo, one engine redo per step; must be >= 1. Stops early at the first step the engine refuses.", "1")
    ))
{
  return EditorUndoHistory::RunSteps(Ctx, /*bRedo=*/true);
}

// ---- editor.undo_history ----
REGISTER_RPC_HANDLER("editor.undo_history", "editor", "Read-only: the editor transaction buffer. Returns queueLength, undoCount (entries already undone and redoable), canUndo/canRedo with the engine's reason when false (empty side, undo barrier, transaction in progress), transactionActive, and the titled entries on each side: undo[] newest first (the next editor.undo first) and redo[] in redo order, each capped at limit, with undoTruncated/redoTruncated. An empty buffer is a normal answer (queueLength 0, both lists empty), not an error.",
    RPC_PARAMS(
        RPC_PARAM_DEF("limit", "integer", "Maximum entries listed on each side (undo and redo); must be >= 1.", "20")
    ))
{
  if (!GEditor || !GEditor->Trans) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor transaction buffer not available"));
    return true;
  }

  const int32 Limit = Ctx.GetInt(TEXT("limit"), 20);
  if (Limit < 1) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("limit must be >= 1 (got %d)."), Limit));
    return true;
  }

  UTransactor* Trans = GEditor->Trans;
  const int32 QueueLength = Trans->GetQueueLength();
  const int32 UndoCount = Trans->GetUndoCount();
  // Queue layout: [0, QueueLength - UndoCount) is the undo side, oldest first;
  // [QueueLength - UndoCount, QueueLength) is the redo side, next redo first.
  const int32 FirstRedo = QueueLength - UndoCount;

  TArray<TSharedPtr<FJsonValue>> UndoEntries;
  for (int32 Index = FirstRedo - 1; Index >= 0 && UndoEntries.Num() < Limit; --Index) {
    UndoEntries.Add(MakeShared<FJsonValueObject>(EditorUndoHistory::DescribeTransaction(*Trans, Index)));
  }
  TArray<TSharedPtr<FJsonValue>> RedoEntries;
  for (int32 Index = FirstRedo; Index < QueueLength && RedoEntries.Num() < Limit; ++Index) {
    RedoEntries.Add(MakeShared<FJsonValueObject>(EditorUndoHistory::DescribeTransaction(*Trans, Index)));
  }

  FText UndoBlocked;
  FText RedoBlocked;
  const bool bCanUndo = Trans->CanUndo(&UndoBlocked);
  const bool bCanRedo = Trans->CanRedo(&RedoBlocked);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetNumberField(TEXT("queueLength"), QueueLength);
  Resp->SetNumberField(TEXT("undoCount"), UndoCount);
  Resp->SetBoolField(TEXT("transactionActive"), Trans->IsActive());
  Resp->SetBoolField(TEXT("canUndo"), bCanUndo);
  if (!bCanUndo) {
    Resp->SetStringField(TEXT("canUndoReason"), UndoBlocked.ToString());
  }
  Resp->SetBoolField(TEXT("canRedo"), bCanRedo);
  if (!bCanRedo) {
    Resp->SetStringField(TEXT("canRedoReason"), RedoBlocked.ToString());
  }
  Resp->SetArrayField(TEXT("undo"), UndoEntries);
  Resp->SetArrayField(TEXT("redo"), RedoEntries);
  Resp->SetBoolField(TEXT("undoTruncated"), UndoEntries.Num() < FirstRedo);
  Resp->SetBoolField(TEXT("redoTruncated"), RedoEntries.Num() < UndoCount);
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
// Tick-unsafe for the reason editor.set_camera / editor.set_game_view are listed in
// Dispatch/SafePoint.cpp (family B): EditorHandlerUtils::ForceRedrawViewportClient draws the
// viewport synchronously (Viewport->Draw()) on the handler's stack. Found by
// PinWright.infra.tick_safety.HazardReachingVerbsAreGated.
REGISTER_RPC_HANDLER_TICK_UNSAFE("editor.jump_to_bookmark", "editor", "Move the viewport camera to a previously stored bookmark slot (0..9). Fails if the slot is empty.",
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
