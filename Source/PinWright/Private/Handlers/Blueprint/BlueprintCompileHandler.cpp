// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintCompileHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint compilation: compile, add_construction_script

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"

#include "Handlers/ErrorCodes.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PathUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Modules/ModuleManager.h"

using namespace BlueprintHandlerUtils;

// ---- blueprint.compile ----
REGISTER_RPC_HANDLER("blueprint.compile", "blueprint", "Run UE's Blueprint compiler on the named asset and return any compile errors/warnings. Required after structural changes (new variables, graph edits, reparenting) before the BP is functional. Compile is in-memory only — to persist the result to disk, call asset.save afterward (it runs the same Blueprint integrity gate at the save choke point). blueprint.compile_bpir compiles + then runs this implicitly. Refused with LIVE_INSTANCES_WOULD_BE_REINSTANCED when loaded worlds hold live instances of the class; see allowReinstancing. errors include the engine's compile-time data validation (e.g. UMG's 'Leak Detected' check, which fails a widget that holds its Slate widget at design time), and an Error status stays on the loaded Blueprint until a clean compile: the editor's Play button then prompts about it, while editor.play starts anyway and lists it in blueprintsWithErrors.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path to compile.")),
        BlueprintReinstancingGuard::AllowReinstancingParam(),
        RPC_PARAM_DEF("warningsAsErrors", "boolean",
            "Report compiler warnings as errors: they move into `errors` and `compiled` is false. "
            "`status` still reports the engine's own Blueprint status.", "false")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH, TEXT("blueprint.compile requires a blueprint path."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, LoadErr.IsEmpty() ? TEXT("Failed to load blueprint for compilation") : *LoadErr);
        return true;
    }

    // Live-instance precondition. A compile flushes the reinstancing queue, which
    // destroys and re-creates every live instance of the class in every loaded world —
    // including placed actors in a map another agent has open. Refuse unless the caller
    // said allowReinstancing=true (BlueprintReinstancingGuard.h has the engine chain).
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, BP, TEXT("blueprint.compile")))
    {
        return true;
    }

    // Compile-only: this verb checks compile status and never persists. Persistence
    // (and its Blueprint integrity gate) lives on the save path — call asset.save to
    // write to disk, where ValidateBlueprintGraphIntegrity runs at the universal
    // SaveLoadedAssetThrottled choke point for every save source.
    FCompileDiagnosticsOptions Options;
    Options.bWarningsAsErrors = Ctx.GetBool(TEXT("warningsAsErrors"), false);
    const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(BP, Options);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("blueprintPath"), Path);
    AddCompileDiagnosticsToJson(Diagnostics, Out);

    Ctx.SendSuccess(Out);
    return true;
}

// ---- blueprint.compile_batch ----
namespace BlueprintCompileBatch
{
    constexpr int32 DefaultLimit = 50;
    // Every row is a load plus a full compile on the game thread; the job cannot be cancelled
    // mid-compile, so the page size is the bound.
    constexpr int32 MaxLimit = 200;

    FString StatusToString(EBlueprintStatus Status)
    {
        switch (Status)
        {
        case BS_Unknown:              return TEXT("Unknown");
        case BS_Dirty:                return TEXT("Dirty");
        case BS_Error:                return TEXT("Error");
        case BS_UpToDate:             return TEXT("UpToDate");
        case BS_BeingCreated:         return TEXT("BeingCreated");
        case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
        default:                      return TEXT("Unknown");
        }
    }

    struct FTarget
    {
        FString ObjectPath;
        bool bInRegistry = false;
    };
}

REGISTER_RPC_HANDLER("blueprint.compile_batch", "blueprint",
    "Compile many Blueprints in ONE call (an explicit list or a content folder) and report one row "
    "per Blueprint: path, statusBefore, outcome (compiled | failed | refused | unloadable), the "
    "engine status after, errors[{message, nodeGuid?, graph?}], warnings, and the `reinstanced` "
    "block when live instances were rebuilt. summary.compiled + failed + refused + unloadable equals "
    "summary.examined, which equals summary.matched unless the page was truncated. A Blueprint with "
    "live instances is a `refused` ROW coded LIVE_INSTANCES_WOULD_BE_REINSTANCED, not a call error, "
    "unless allowReinstancing is true. onlyStatus=error compiles only the Blueprints currently "
    "LOADED in the Error state (the editor.play blueprintsWithErrors set, scoped to the request); "
    "dirty likewise for Dirty. Compile-only: nothing is saved (Save-on-Compile is suppressed); "
    "call asset.save afterwards. An empty match set is NO_ASSETS_MATCHED. Returns a JOB TICKET with "
    "one progress event per Blueprint, emitted before its compile starts; a running batch cannot "
    "be cancelled, so `limit` bounds it.",
    RPC_PARAMS(
        RPC_PARAM_OPT("folder", "path",
            "Content folder to compile, e.g. /Game/Blueprints. Provide exactly one of folder or assets."),
        RPC_PARAM_OPT("assets", "array",
            "Explicit Blueprint asset paths (package path /Game/A/BP_X or object path "
            "/Game/A/BP_X.BP_X). A string that is not a content path, or names a non-Blueprint "
            "asset, is INVALID_ARGUMENT before anything compiles. A well-formed path with nothing "
            "behind it is an `unloadable` row coded ASSET_NOT_FOUND, never dropped."),
        RPC_PARAM_DEF("recursive", "boolean", "Include sub-folders of `folder`.", "true"),
        RPC_PARAM_OPT("namePattern", "string",
            "Wildcard filter on the asset name (`*`, `?`), case-insensitive, e.g. BP_Enemy_*."),
        RPC_PARAM_DEF("onlyStatus", "string",
            "all | error | dirty. error/dirty keep only Blueprints already loaded with that "
            "UBlueprint::Status (an unloaded Blueprint has no status yet and never matches). Explicit "
            "paths that resolve to nothing are reported whatever the filter.", "all"),
        BlueprintReinstancingGuard::AllowReinstancingParam(),
        RPC_PARAM_DEF("warningsAsErrors", "boolean",
            "Count compiler warnings as errors: they move into `errors` and the row is `failed`.",
            "false"),
        RPC_PARAM_DEF("limit", "integer",
            "Blueprints this call compiles (1-200). Paging is ordered by object path.", "50"),
        RPC_PARAM_DEF("offset", "integer", "Matched Blueprints to skip before the page.", "0")
    ))
{
    using namespace BlueprintCompileBatch;

    const FString RawFolder = Ctx.GetString(TEXT("folder")).TrimStartAndEnd();
    TArray<FString> ExplicitAssets;
    if (const TArray<TSharedPtr<FJsonValue>>* AssetArray = Ctx.GetArray(TEXT("assets")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *AssetArray)
        {
            FString Entry;
            if (Value.IsValid() && Value->TryGetString(Entry))
            {
                Entry.TrimStartAndEndInline();
                if (!Entry.IsEmpty()) { ExplicitAssets.AddUnique(Entry); }
            }
        }
    }
    if (RawFolder.IsEmpty() == (ExplicitAssets.Num() == 0))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Provide exactly one of `folder` (a content folder) or `assets` (a list of "
                 "Blueprint paths)."));
        return true;
    }

    const FString OnlyStatus = Ctx.GetString(TEXT("onlyStatus"), TEXT("all")).ToLower();
    if (OnlyStatus != TEXT("all") && OnlyStatus != TEXT("error") && OnlyStatus != TEXT("dirty"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown onlyStatus '%s'. Valid: all, error, dirty."), *OnlyStatus));
        return true;
    }

    const bool bRecursive = Ctx.GetBool(TEXT("recursive"), true);
    const bool bAllowReinstancing =
        Ctx.GetBool(BlueprintReinstancingGuard::AllowReinstancingParamName(), false);
    const bool bWarningsAsErrors = Ctx.GetBool(TEXT("warningsAsErrors"), false);
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), DefaultLimit), 1, MaxLimit);
    const int32 Offset = FMath::Max(0, Ctx.GetInt(TEXT("offset"), 0));
    const FString NamePattern = Ctx.GetString(TEXT("namePattern")).TrimStartAndEnd();

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FAssetData> Found;
    TArray<FTarget> Targets;
    FString Scope;
    if (!RawFolder.IsEmpty())
    {
        FString Folder = SanitizeProjectRelativePath(RawFolder);
        while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/"))) { Folder.LeftChopInline(1); }
        if (Folder.IsEmpty() || !IsValidAssetPath(Folder) || Folder.Contains(TEXT(".")))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("'folder' is '%s', which is not a content folder path such as "
                                     "/Game/Blueprints."), *RawFolder));
            return true;
        }
        FARFilter Filter;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        Filter.bRecursivePaths = bRecursive;
        Filter.PackagePaths.Add(FName(*Folder));
        AssetRegistry.GetAssets(Filter, Found);
        if (!NamePattern.IsEmpty())
        {
            Found.RemoveAll([&NamePattern](const FAssetData& Data)
            {
                return !Data.AssetName.ToString().MatchesWildcard(NamePattern, ESearchCase::IgnoreCase);
            });
        }
        Scope = FString::Printf(TEXT("%s%s"), *Folder,
            bRecursive ? TEXT(" (recursive)") : TEXT(" (this folder only)"));
    }
    else
    {
        for (const FString& Entry : ExplicitAssets)
        {
            FString ObjectPath;
            FString NormalizeError;
            const FString Sanitized = SanitizeProjectRelativePath(Entry);
            if (Sanitized.IsEmpty() || !NormalizeToObjectPath(Sanitized, ObjectPath, NormalizeError))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("'%s' in `assets` is not a Blueprint asset path. %s"),
                        *Entry, *NormalizeError));
                return true;
            }
            const FAssetData Data = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
            if (!Data.IsValid())
            {
                Targets.Add({ObjectPath, false});
                continue;
            }
            const UClass* AssetClass = FindObject<UClass>(Data.AssetClassPath);
            if (!AssetClass || !AssetClass->IsChildOf(UBlueprint::StaticClass()))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("'%s' in `assets` is a %s, not a Blueprint."),
                        *Entry, *Data.AssetClassPath.GetAssetName().ToString()));
                return true;
            }
            Found.Add(Data);
        }
        Scope = FString::Printf(TEXT("%d explicit asset path(s)"), ExplicitAssets.Num());
    }

    for (const FAssetData& Data : Found)
    {
        if (OnlyStatus != TEXT("all"))
        {
            // Status is transient (Blueprint.h), so only a loaded Blueprint has one to filter on.
            const UBlueprint* Loaded = Cast<UBlueprint>(Data.FastGetAsset(/*bLoad=*/false));
            const EBlueprintStatus Wanted = OnlyStatus == TEXT("error") ? BS_Error : BS_Dirty;
            if (!Loaded || Loaded->Status != Wanted) { continue; }
        }
        Targets.Add({Data.GetObjectPathString(), true});
    }

    // Sorted so `offset` addresses the same Blueprint on every call.
    Targets.Sort([](const FTarget& A, const FTarget& B)
    {
        return A.ObjectPath.Compare(B.ObjectPath, ESearchCase::CaseSensitive) < 0;
    });
    const int32 Total = Targets.Num();
    if (Total == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_ASSETS_MATCHED,
            FString::Printf(TEXT("No Blueprints matched %s%s%s. Nothing was compiled.%s"),
                *Scope,
                NamePattern.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" with namePattern '%s'"), *NamePattern),
                OnlyStatus == TEXT("all") ? TEXT("")
                    : *FString::Printf(TEXT(" with onlyStatus '%s' (loaded Blueprints only)"), *OnlyStatus),
                AssetRegistry.IsLoadingAssets()
                    ? TEXT(" The asset registry is still scanning, so the match set may be incomplete.")
                    : TEXT("")));
        return true;
    }
    if (Offset >= Total)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'offset' is %d but only %d Blueprint(s) matched; pass an offset below %d."),
                Offset, Total, Total));
        return true;
    }
    const int32 PageSize = FMath::Min(Limit, Total - Offset);

    FJobBindArgs Args;
    Args.Method = TEXT("blueprint.compile_batch");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("scope"), Scope);
    Args.StartedPayload->SetNumberField(TEXT("matched"), Total);
    Args.StartedPayload->SetNumberField(TEXT("examining"), PageSize);
    const FString TicketId = Ctx.StartJob(Args);
    FJobRegistry& Jobs = FPluginState::Get().GetJobRegistry();

    FCompileDiagnosticsOptions Options;
    Options.bWarningsAsErrors = bWarningsAsErrors;
    Options.bScheduleGarbageCollection = false;   // one deferred GC after the loop
    Options.bSkipSaveOnCompile = true;

    int32 Compiled = 0, Failed = 0, Refused = 0, Unloadable = 0;
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 Index = 0; Index < PageSize; ++Index)
    {
        const FTarget& Target = Targets[Offset + Index];

        // Emitted BEFORE the synchronous load + compile, so a caller watching a slow
        // Blueprint sees which one it is.
        TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
        Progress->SetNumberField(TEXT("progress"), Index);
        Progress->SetNumberField(TEXT("total"), PageSize);
        Progress->SetStringField(TEXT("currentAsset"), Target.ObjectPath);
        Jobs.RecordProgress(TicketId,
            FString::Printf(TEXT("compiling %d/%d: %s"), Index + 1, PageSize, *Target.ObjectPath),
            Progress, /*bBypassRateLimit=*/true);

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), Target.ObjectPath);
        Rows.Add(MakeShared<FJsonValueObject>(Row));

        UBlueprint* BP = Target.bInRegistry ? FindObject<UBlueprint>(nullptr, *Target.ObjectPath) : nullptr;
        Row->SetStringField(TEXT("statusBefore"), BP ? StatusToString(BP->Status) : FString(TEXT("Unloaded")));
        if (!Target.bInRegistry)
        {
            ++Unloadable;
            Row->SetStringField(TEXT("outcome"), TEXT("unloadable"));
            Row->SetStringField(TEXT("code"), ErrorCodes::ERR_ASSET_NOT_FOUND);
            Row->SetStringField(TEXT("message"), TEXT("The asset registry holds no asset at this path."));
            continue;
        }
        if (!BP)
        {
            // The compile below covers what compile-on-load would have done (the
            // CompileAllBlueprints commandlet loads the same way).
            FString Refusal;
            BP = PinWrightGuardedLoad::LoadObjectChecked<UBlueprint>(
                Target.ObjectPath, &Refusal, LOAD_NoWarn | LOAD_DisableCompileOnLoad);
            if (!BP)
            {
                ++Unloadable;
                Row->SetStringField(TEXT("outcome"), TEXT("unloadable"));
                Row->SetStringField(TEXT("code"), ErrorCodes::ERR_ASSET_LOAD_FAILED);
                Row->SetStringField(TEXT("message"),
                    Refusal.IsEmpty() ? FString(TEXT("The Blueprint package failed to load.")) : Refusal);
                continue;
            }
        }

        const BlueprintReinstancingGuard::FLiveInstanceSurvey Survey =
            BlueprintReinstancingGuard::SurveyLiveInstances(BP);
        if (!Survey.IsEmpty() && !bAllowReinstancing)
        {
            ++Refused;
            Row->SetStringField(TEXT("outcome"), TEXT("refused"));
            Row->SetStringField(TEXT("status"), StatusToString(BP->Status));
            Row->SetStringField(TEXT("code"), ErrorCodes::ERR_LIVE_INSTANCES_WOULD_BE_REINSTANCED);
            Row->SetStringField(TEXT("message"), FString::Printf(
                TEXT("Not compiled: %s Pass allowReinstancing=true to accept the rebuild."),
                *BlueprintReinstancingGuard::DescribeSurvey(Survey)));
            BlueprintReinstancingGuard::AddSurveyToJson(Survey, Row);
            continue;
        }

        const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(BP, Options);
        AddCompileDiagnosticsToJson(Diagnostics, Row);
        if (Diagnostics.bCompiled) { ++Compiled; } else { ++Failed; }
        Row->SetStringField(TEXT("outcome"), Diagnostics.bCompiled ? TEXT("compiled") : TEXT("failed"));
    }

    // One deferred full purge for the whole batch, never a GC on this stack (see
    // CompileBlueprintWithDiagnostics for why).
    if (GEngine && Compiled + Failed > 0)
    {
        GEngine->ForceGarbageCollection(true);
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("matched"), Total);
    Summary->SetNumberField(TEXT("examined"), PageSize);
    Summary->SetNumberField(TEXT("compiled"), Compiled);
    Summary->SetNumberField(TEXT("failed"), Failed);
    Summary->SetNumberField(TEXT("refused"), Refused);
    Summary->SetNumberField(TEXT("unloadable"), Unloadable);
    Summary->SetBoolField(TEXT("truncated"), Offset + PageSize < Total);

    TSharedPtr<FJsonObject> Page = MakeShared<FJsonObject>();
    Page->SetNumberField(TEXT("offset"), Offset);
    Page->SetNumberField(TEXT("limit"), Limit);
    Page->SetNumberField(TEXT("total"), Total);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("scope"), Scope);
    Result->SetStringField(TEXT("onlyStatus"), OnlyStatus);
    Result->SetBoolField(TEXT("warningsAsErrors"), bWarningsAsErrors);
    Result->SetObjectField(TEXT("summary"), Summary);
    Result->SetObjectField(TEXT("page"), Page);
    Result->SetArrayField(TEXT("blueprints"), Rows);
    Jobs.Complete(TicketId, /*bSuccess=*/true, Result, FString());
    return true;
}

// ---- blueprint.add_construction_script ----
REGISTER_RPC_HANDLER("blueprint.add_construction_script", "blueprint", "Ensure a construction script graph exists on a blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path"))
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_BLUEPRINT_PATH, TEXT("blueprint.add_construction_script requires a blueprint path."));
        return true;
    }

    auto* Subsystem = Ctx.GetSubsystem();
    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND, LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    UEdGraph* ConstructionGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript)
        {
            ConstructionGraph = Graph;
            break;
        }
    }

    if (!ConstructionGraph)
    {
        ConstructionGraph = FBlueprintEditorUtils::CreateNewGraph(
            BP, UEdGraphSchema_K2::FN_UserConstructionScript,
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(
            BP, ConstructionGraph, /*bIsUserCreated=*/false, nullptr);
    }

    if (ConstructionGraph)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("blueprintPath"), Path);
        Result->SetStringField(TEXT("graphName"), ConstructionGraph->GetName());
        Result->SetStringField(TEXT("note"),
            TEXT("Construction script graph ensured. Use blueprint.graph.create_node with graphName='UserConstructionScript' to add nodes."));
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_GRAPH_ERROR, TEXT("Failed to create construction script graph"));
    }
    return true;
}
