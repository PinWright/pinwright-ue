// Copyright (c) 2026 Alexander Penkin. MIT License.

// First-class source_control.* namespace — five verbs that route through
// ISourceControlModule::Get().GetProvider() so behaviour is uniform across
// shipped providers (Perforce, Git, Subversion, PlasticSCM). Existing
// asset.source_control_* handlers stay untouched for back-compat.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/SourceControl/SourceControlStateFlags.h"
#include "Handlers/SourceControl/SourceControlPackageResync.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/JsonUtils.h"

namespace
{
    // Convert /Game/Foo or /Game/Foo.Foo to an absolute .uasset filename on
    // disk so it can be fed to ISourceControlProvider::Execute(...).
    static bool TryAssetPathToFilename(const FString& AssetPath, FString& OutFilename, FString& OutError)
    {
        FString PackageName = AssetPath;
        int32 DotIdx = INDEX_NONE;
        if (PackageName.FindLastChar('.', DotIdx))
        {
            PackageName = PackageName.Left(DotIdx);
        }
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFilename, FPackageName::GetAssetPackageExtension()))
        {
            OutError = FString::Printf(TEXT("Could not convert package name to filename: %s"), *AssetPath);
            return false;
        }
        return true;
    }

    // Resolve a JSON "assetPaths" string-array into both the original asset
    // paths and their on-disk filenames. Sends error and returns false on
    // problems so the caller can `return true;` immediately.
    static bool ResolveAssetPathsArray(FHandlerContext& Ctx,
                                       TArray<FString>& OutAssetPaths,
                                       TArray<FString>& OutFilenames)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = Ctx.GetArray(TEXT("assetPaths"));
        if (!Arr || Arr->Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPaths array required"));
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Val : *Arr)
        {
            if (!Val.IsValid() || Val->Type != EJson::String) continue;
            const FString AssetPath = Val->AsString();
            FString Filename;
            FString Err;
            if (!TryAssetPathToFilename(AssetPath, Filename, Err))
            {
                Ctx.SendError(TEXT("INVALID_ASSET_PATH"), Err);
                return false;
            }
            OutAssetPaths.Add(AssetPath);
            OutFilenames.Add(Filename);
        }
        if (OutFilenames.Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("assetPaths contained no usable string entries"));
            return false;
        }
        return true;
    }

    // Centralizes the "module enabled?" gate shared by status/log/revert/mark_for_add.
    // Sends SOURCE_CONTROL_DISABLED error on failure so the caller can `return true;`.
    static bool RequireEnabledProvider(FHandlerContext& Ctx, ISourceControlProvider*& OutProvider)
    {
        if (!ISourceControlModule::Get().IsEnabled())
        {
            Ctx.SendError(TEXT("SOURCE_CONTROL_DISABLED"), TEXT("Source control is not enabled"));
            return false;
        }
        OutProvider = &ISourceControlModule::Get().GetProvider();
        return true;
    }

    // Snapshot the names of every registered source-control provider (Perforce,
    // Git, Subversion, the "None" stub, plus any third-party plugins). Used by
    // source_control.connect both to validate the requested provider before the
    // assert-prone SetProvider() call and to surface the valid choices to the
    // caller on a miss.
    static TArray<FName> GetRegisteredProviderNames()
    {
        TArray<FName> Names;
        ISourceControlModule::Get().GetProviderNames(Names);
        return Names;
    }
}

// ============================================================================
// source_control.get_provider
// ============================================================================
REGISTER_RPC_HANDLER("source_control.get_provider", "source_control",
    "Return the active source-control provider's name and connectivity flags",
    RPC_NO_PARAMS)
{
    const bool bEnabled = ISourceControlModule::Get().IsEnabled();
    ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("providerName"), Provider.GetName().ToString());
    Result->SetBoolField(TEXT("isEnabled"), bEnabled);
    Result->SetBoolField(TEXT("isAvailable"), Provider.IsAvailable());
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// source_control.connect
// ============================================================================
// The bring-up affordance the rest of the namespace depends on: switch the
// active provider and force a connection/login so status/log/revert/mark_for_add
// stop returning SOURCE_CONTROL_DISABLED. Deliberately NOT gated by
// RequireEnabledProvider — it must run in a disabled session, that is the point.
REGISTER_RPC_HANDLER("source_control.connect", "source_control",
    "Switch to / bring up a source-control provider so the namespace becomes usable",
    RPC_PARAMS(
        RPC_PARAM_OPT("providerName", "string", "Provider to switch to (e.g. Perforce, Git, Subversion). Omit to re-attempt connection on the currently configured provider."),
        RPC_PARAM_OPT("settings", "object", "Optional provider init settings (string key/value pairs). Reserved: currently echoed back as appliedSettings only, not yet applied to the provider.")
    ))
{
    ISourceControlModule& Module = ISourceControlModule::Get();
    const TArray<FName> AvailableNames = GetRegisteredProviderNames();

    // Serialize the provider-name snapshot once; both the INVALID_PROVIDER error
    // payload and the success payload echo the same list.
    TArray<FString> AvailableNameStrs;
    AvailableNameStrs.Reserve(AvailableNames.Num());
    for (const FName& Name : AvailableNames)
    {
        AvailableNameStrs.Add(Name.ToString());
    }
    const TArray<TSharedPtr<FJsonValue>> AvailableProvidersJson = EmitStringArray(AvailableNameStrs);

    const FString RequestedName = Ctx.GetString(TEXT("providerName"));
    bool bSwitched = false;

    if (!RequestedName.IsEmpty())
    {
        const FName RequestedFName(*RequestedName);
        // SetProvider() asserts (hard editor crash) on a name that was never
        // registered, so validate membership against GetProviderNames() FIRST
        // and reject with an actionable error listing the valid choices. This
        // guard is the load-bearing correctness point of the handler.
        if (!AvailableNames.Contains(RequestedFName))
        {
            TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
            ErrResult->SetArrayField(TEXT("availableProviders"), AvailableProvidersJson);
            Ctx.SendError(TEXT("INVALID_PROVIDER"),
                FString::Printf(TEXT("Unknown source-control provider '%s'"), *RequestedName),
                ErrResult);
            return true;
        }

        const FName CurrentName = Module.GetProvider().GetName();
        if (CurrentName != RequestedFName)
        {
            Module.SetProvider(RequestedFName);
            bSwitched = true;
        }
    }

    // Optional settings are echoed back as appliedSettings only — they are NOT
    // forwarded to the provider. ISourceControlProvider::Init takes no settings
    // argument; the bring-up below uses Init(bForceConnection)+Login. Settings
    // can be wired in later via Module.CreateProvider(Name, Owner, InitSettings),
    // the only API that consumes them, when that path lands.
    TArray<TSharedPtr<FJsonValue>> EchoedSettings;
    if (TSharedPtr<FJsonObject> SettingsObj = Ctx.GetObject(TEXT("settings")))
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : SettingsObj->Values)
        {
            FString ValueStr;
            if (Pair.Value.IsValid() && Pair.Value->TryGetString(ValueStr))
            {
                EchoedSettings.Add(MakeShared<FJsonValueString>(Pair.Key));
            }
        }
    }

    // Force a connection attempt on the (now possibly switched) provider, then
    // log in. Both are synchronous; failures surface as isEnabled/isAvailable
    // flags rather than an exception.
    ISourceControlProvider& Provider = Module.GetProvider();
    Provider.Init(/*bForceConnection=*/true);
    const ECommandResult::Type LoginResult = Provider.Login();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("providerName"), Provider.GetName().ToString());
    Result->SetBoolField(TEXT("isEnabled"), Module.IsEnabled());
    Result->SetBoolField(TEXT("isAvailable"), Provider.IsAvailable());
    Result->SetBoolField(TEXT("switched"), bSwitched);
    Result->SetBoolField(TEXT("loginSucceeded"), LoginResult == ECommandResult::Succeeded);
    Result->SetArrayField(TEXT("availableProviders"), AvailableProvidersJson);
    if (EchoedSettings.Num() > 0)
    {
        Result->SetArrayField(TEXT("appliedSettings"), EchoedSettings);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// source_control.status
// ============================================================================
REGISTER_RPC_HANDLER("source_control.status", "source_control",
    "Refresh and return source-control state for a batch of asset paths",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Asset paths to query")
    ))
{
    ISourceControlProvider* Provider = nullptr;
    if (!RequireEnabledProvider(Ctx, Provider)) return true;

    TArray<FString> AssetPaths;
    TArray<FString> Filenames;
    if (!ResolveAssetPathsArray(Ctx, AssetPaths, Filenames))
    {
        return true;
    }

    const ECommandResult::Type ExecResult =
        Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), Filenames);

    TArray<TSharedPtr<FJsonValue>> FilesArr;
    for (int32 i = 0; i < Filenames.Num(); ++i)
    {
        TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
        FileObj->SetStringField(TEXT("path"), AssetPaths[i]);
        FileObj->SetStringField(TEXT("filename"), Filenames[i]);

        FSourceControlStatePtr State = Provider->GetState(Filenames[i], EStateCacheUsage::Use);
        if (State.IsValid())
        {
            // Shared derivation; isUnchanged deliberately excludes IsCheckedOut() — see SourceControlStateFlags.h.
            PinWright::SourceControl::WriteStateFlags(*State, *FileObj);
        }
        else
        {
            FileObj->SetBoolField(TEXT("stateValid"), false);
        }
        FilesArr.Add(MakeShared<FJsonValueObject>(FileObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("files"), FilesArr);
    Result->SetBoolField(TEXT("operationSucceeded"), ExecResult == ECommandResult::Succeeded);
    Result->SetNumberField(TEXT("resultCode"), (int32)ExecResult);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// source_control.log
// ============================================================================
REGISTER_RPC_HANDLER("source_control.log", "source_control",
    "Return revision history for a single asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path to query history for"),
        RPC_PARAM_OPT("maxRevisions", "number", "Cap on number of revisions returned (0 = no cap)")
    ))
{
    ISourceControlProvider* Provider = nullptr;
    if (!RequireEnabledProvider(Ctx, Provider)) return true;

    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    FString Filename;
    FString Err;
    if (!TryAssetPathToFilename(AssetPath, Filename, Err))
    {
        Ctx.SendError(TEXT("INVALID_ASSET_PATH"), Err);
        return true;
    }

    const int32 MaxRevisions = Ctx.GetInt(TEXT("maxRevisions"), 0);

    TArray<FString> FilePaths;
    FilePaths.Add(Filename);
    // History is fetched via FUpdateStatus with bUpdateHistory=true; there is
    // no separate FGetFileHistory operation in UE 5.x.
    TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> StatusOp = ISourceControlOperation::Create<FUpdateStatus>();
    StatusOp->SetUpdateHistory(true);
    const ECommandResult::Type ExecResult = Provider->Execute(StatusOp, FilePaths);

    FSourceControlStatePtr State = Provider->GetState(Filename, EStateCacheUsage::Use);

    TArray<TSharedPtr<FJsonValue>> RevsArr;
    if (State.IsValid())
    {
        const int32 HistorySize = State->GetHistorySize();
        const int32 Limit = (MaxRevisions > 0) ? FMath::Min(MaxRevisions, HistorySize) : HistorySize;
        for (int32 i = 0; i < Limit; ++i)
        {
            TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = State->GetHistoryItem(i);
            if (!Rev.IsValid()) continue;

            TSharedPtr<FJsonObject> RevObj = MakeShared<FJsonObject>();
            RevObj->SetNumberField(TEXT("revision"), Rev->GetRevisionNumber());
            RevObj->SetStringField(TEXT("revisionString"), Rev->GetRevision());
            RevObj->SetNumberField(TEXT("changelist"), Rev->GetCheckInIdentifier());
            RevObj->SetStringField(TEXT("user"), Rev->GetUserName());
            RevObj->SetStringField(TEXT("date"), Rev->GetDate().ToIso8601());
            RevObj->SetStringField(TEXT("description"), Rev->GetDescription());
            RevObj->SetStringField(TEXT("action"), Rev->GetAction());
            RevsArr.Add(MakeShared<FJsonValueObject>(RevObj));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), AssetPath);
    Result->SetStringField(TEXT("filename"), Filename);
    Result->SetArrayField(TEXT("revisions"), RevsArr);
    Result->SetBoolField(TEXT("operationSucceeded"), ExecResult == ECommandResult::Succeeded);
    Result->SetNumberField(TEXT("resultCode"), (int32)ExecResult);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// source_control.revert
// ============================================================================
REGISTER_RPC_HANDLER("source_control.revert", "source_control",
    "Discard local changes for a batch of asset paths, and resynchronize the loaded "
    "packages with the reverted bytes. Every currently-resident target is unlinked before "
    "the provider revert and re-read from disk afterwards (the same engine path the Content "
    "Browser's revert uses), so readbacks reflect the reverted content and a later save "
    "cannot re-persist the pre-revert state. `count` is the number of paths REQUESTED; "
    "`loadedCount`, `reloadedCount`, `removedCount` and `staleCount` are MEASURED off the "
    "packages afterwards, and `files[]` carries the same per-path measurement. "
    "`resynchronized:false` plus `resyncWarning` means at least one package kept its "
    "pre-revert in-memory state — do not save it. A loaded map, or a loaded external "
    "package of a loaded world, cannot be reloaded without tearing the world down: the "
    "batch is REFUSED with REVERT_REQUIRES_UNLOADED_PACKAGE and nothing is reverted "
    "(unload the map, or use editor.open_level, then retry).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Asset paths to revert")
    ))
{
    ISourceControlProvider* Provider = nullptr;
    if (!RequireEnabledProvider(Ctx, Provider)) return true;

    TArray<FString> AssetPaths;
    TArray<FString> Filenames;
    if (!ResolveAssetPathsArray(Ctx, AssetPaths, Filenames))
    {
        return true;
    }

    // The provider revert runs INSIDE the resync so the engine can unlink loaders first and
    // reload the packages from the reverted bytes; calling Execute() on its own reverts the
    // file and strands the loaded UPackage on the pre-revert state
    // (board B-source-control-revert-no-package-reload).
    ECommandResult::Type ExecResult = ECommandResult::Failed;
    PinWrightSourceControlResync::FPackageResyncReport Resync;
    PinWrightSourceControlResync::ApplyAndResyncPackages(Filenames,
        [Provider, &ExecResult](const TArray<FString>& RevertFilenames) -> bool
        {
            ExecResult = Provider->Execute(ISourceControlOperation::Create<FRevert>(), RevertFilenames);
            return ExecResult == ECommandResult::Succeeded;
        },
        Resync);

    if (Resync.BlockedPackages.Num() > 0)
    {
        TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
        ErrResult->SetArrayField(TEXT("blockedPackages"), EmitStringArray(Resync.BlockedPackages));
        Ctx.SendError(TEXT("REVERT_REQUIRES_UNLOADED_PACKAGE"),
            FString::Printf(TEXT("Nothing was reverted: %d requested package(s) are loaded map or external packages that cannot be reloaded from disk. Reverting them would leave the live world holding the pre-revert state. Unload them (editor.open_level for a map) and retry."),
                Resync.BlockedPackages.Num()),
            ErrResult);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> FilesArr;
    FilesArr.Reserve(Resync.Entries.Num());
    for (int32 Index = 0; Index < Resync.Entries.Num(); ++Index)
    {
        const PinWrightSourceControlResync::FPackageResyncEntry& Entry = Resync.Entries[Index];
        TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
        FileObj->SetStringField(TEXT("path"), AssetPaths.IsValidIndex(Index) ? AssetPaths[Index] : Entry.PackageName);
        FileObj->SetStringField(TEXT("filename"), Entry.Filename);
        FileObj->SetBoolField(TEXT("wasLoaded"), Entry.bWasLoaded);
        FileObj->SetBoolField(TEXT("reloaded"), Entry.bReloaded);
        FileObj->SetBoolField(TEXT("removed"), Entry.bRemoved);
        FileObj->SetBoolField(TEXT("stale"), Entry.bStale);
        FilesArr.Add(MakeShared<FJsonValueObject>(FileObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), ExecResult == ECommandResult::Succeeded);
    Result->SetNumberField(TEXT("resultCode"), (int32)ExecResult);
    Result->SetNumberField(TEXT("count"), Filenames.Num());
    Result->SetNumberField(TEXT("loadedCount"), Resync.LoadedCount);
    Result->SetNumberField(TEXT("reloadedCount"), Resync.ReloadedCount);
    Result->SetNumberField(TEXT("removedCount"), Resync.RemovedCount);
    Result->SetNumberField(TEXT("staleCount"), Resync.StaleCount);
    Result->SetBoolField(TEXT("resynchronized"), Resync.StaleCount == 0);
    Result->SetArrayField(TEXT("files"), FilesArr);

    // Only when the measurement disagrees with the intent: a stale package is the one state
    // in which the revert has NOT taken hold in the editor, and saving it silently undoes it.
    if (Resync.StaleCount > 0)
    {
        Result->SetStringField(TEXT("resyncWarning"),
            FString::Printf(TEXT("%d package(s) were reverted on disk but could not be reloaded, so their in-memory state is still the pre-revert one. Do not save them; use asset.reload on each (see files[].stale)."),
                Resync.StaleCount));
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// source_control.mark_for_add
// ============================================================================
REGISTER_RPC_HANDLER("source_control.mark_for_add", "source_control",
    "Mark a batch of asset paths for add",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPaths", "array", "Asset paths to mark for add")
    ))
{
    ISourceControlProvider* Provider = nullptr;
    if (!RequireEnabledProvider(Ctx, Provider)) return true;

    TArray<FString> AssetPaths;
    TArray<FString> Filenames;
    if (!ResolveAssetPathsArray(Ctx, AssetPaths, Filenames))
    {
        return true;
    }

    const ECommandResult::Type ExecResult =
        Provider->Execute(ISourceControlOperation::Create<FMarkForAdd>(), Filenames);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), ExecResult == ECommandResult::Succeeded);
    Result->SetNumberField(TEXT("resultCode"), (int32)ExecResult);
    Result->SetNumberField(TEXT("count"), Filenames.Num());
    Ctx.SendSuccess(Result);
    return true;
}
