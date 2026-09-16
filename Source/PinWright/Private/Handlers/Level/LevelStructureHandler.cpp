// Copyright (c) 2026 Alexander Penkin. MIT License.

// LevelStructureHandler.cpp - Migrated from PinWright_LevelStructureHandlers.cpp
// Level structure management: levels, sublevels, streaming, world partition,
// data layers, HLOD, level blueprint, level instances, packed level actors

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Level/LevelNameParamUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"
#include "Compat/EngineVersionCompat.h"

#include "Editor.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
// FWorldInitializationValues moved to its own header in UE 5.4; on 5.3 it lives in Engine/World.h (above).
#if __has_include("Engine/WorldInitializationValues.h")
#include "Engine/WorldInitializationValues.h"
#endif
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "LevelEditor.h"
#include "EditorLevelUtils.h"
#include "FileHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelScriptActor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/NameTypes.h"
#include "UObject/SavePackage.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/DataLayer/DataLayerSubsystem.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "Engine/LODActor.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "PackedLevelActor/PackedLevelActor.h"
#include "PackedLevelActor/PackedLevelActorBuilder.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "AssetToolsModule.h"
#include "WorldPartition/WorldPartitionMiniMapVolume.h"
#include "WorldPartition/RuntimeHashSet/WorldPartitionRuntimeHashSet.h"
#include "WorldPartition/WorldPartitionRuntimeSpatialHash.h"
#include "Engine/LevelStreamingVolume.h"
#include "EditorAssetLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpLevelStructureHandler, Log, All);

// ============================================================================
// Helper Functions
// ============================================================================


// Get object field from JSON payload (no consolidated equivalent)
static TSharedPtr<FJsonObject> GetObjectFieldLS(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName)
{
    if (Payload.IsValid() && Payload->HasTypedField<EJson::Object>(FieldName))
    {
        return Payload->GetObjectField(FieldName);
    }
    return nullptr;
}

// Get FVector from JSON object field
static FVector GetVectorFromJsonLS(const TSharedPtr<FJsonObject>& JsonObj, FVector Default = FVector::ZeroVector)
{
    if (!JsonObj.IsValid()) return Default;
    return FVector(
        GetJsonNumberField(JsonObj, TEXT("x"), Default.X),
        GetJsonNumberField(JsonObj, TEXT("y"), Default.Y),
        GetJsonNumberField(JsonObj, TEXT("z"), Default.Z)
    );
}

// Get FRotator from JSON object field
static FRotator GetRotatorFromJsonLS(const TSharedPtr<FJsonObject>& JsonObj, FRotator Default = FRotator::ZeroRotator)
{
    if (!JsonObj.IsValid()) return Default;
    return FRotator(
        GetJsonNumberField(JsonObj, TEXT("pitch"), Default.Pitch),
        GetJsonNumberField(JsonObj, TEXT("yaw"), Default.Yaw),
        GetJsonNumberField(JsonObj, TEXT("roll"), Default.Roll)
    );
}

// Get current editor world
static UWorld* GetEditorWorldLS()
{
    if (GEditor)
    {
        return GEditor->GetEditorWorldContext().World();
    }
    return nullptr;
}

// ============================================================================
// Package-path safety for the three verbs in this file that hand a composed
// "<folder>/<caller name>" to CreatePackage: create_level, create_data_layer,
// configure_hlod_layer.
//
// CreatePackage (UObjectGlobals.cpp:1086-1120) logs at **Fatal** - a verbosity that is NOT
// compiled out in any configuration - for a name containing "//" (:1094-1096) and for a name that
// resolves to empty (:1118). Fatal ends the PROCESS, so a bad caller string does not fail the
// call: it kills the editor and every unsaved package in it. Measured on board
// B-foliage-add-type-name-with-slash-kills-the-editor. The `if (!Package)` branch after each call
// can never fire, because nothing after CreatePackage is reached - so the only place to stop it is
// before the call.
//
// All three verbs already sanitize their FOLDER argument (SanitizeProjectRelativePath, whose
// result is assigned back over the raw argument, so the folder half is genuinely guarded). Two
// holes are left, and these two helpers close them:
//
//   1. The caller's bare NAME is concatenated raw. "a//b" is a literal double slash, and ".."
//      composes "/Game/Maps/.." which CreatePackage trims to "/Game/Maps/." and ResolveName2 then
//      empties - the second Fatal. create_level's hand-rolled character filter catches '/' and
//      '\' but not '.', so it does not cover either route.
//   2. The `IsValidMountPoint` fallback below each composition prepends "/Game/" to a path that
//      already begins with '/', manufacturing "/Game//..." itself. It is reachable whenever the
//      folder is a mounted root other than /Game|/Engine|/Script and the name carries a character
//      that is legal for an object but not for a package path ('\', '*', '?', '<', '>').
//
// The shared PinWrightComposeAssetPackagePath (Handlers/PackagePathCompose.h) is deliberately NOT
// used here, for two reasons: it composes with Printf("%s/%s"), which doubles the separator when
// the folder ends in '/' - and SanitizeProjectRelativePath does not strip a trailing slash, so a
// routine `levelPath: "/Game/Maps/"` that FString::operator/ handles correctly today would start
// being refused; and it never sees the string produced by hole 2, which is what CreatePackage is
// actually given. The same two engine rules that helper applies are applied here instead, split
// across the two points where each one is meaningful.

// Engine rule for a bare object name: INVALID_OBJECTNAME_CHARACTERS rejects '/', '.', ':', '|'
// and the rest, so a path-shaped or traversal name is refused before it is concatenated. The
// reason text is the engine's, surfaced verbatim, so a refused caller is told which rule it broke.
// CALL THIS ABOVE THE FOLDER SANITIZER in each handler: the ordering is what lets a regression
// test pair a bad name with a folder the sanitizer rejects and still tell a fixed build from a
// reverted one without ever composing the path. See Tests/World/TestLevelStructureNameSafety.cpp.
static bool PinWrightLevelStructureValidateBareName(const FString& Name, const TCHAR* ParamName,
    FString& OutError)
{
    FText Reason;
    if (FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &Reason))
    {
        return true;
    }
    OutError = FString::Printf(TEXT("%s '%s' is not a bare name: %s"), ParamName, *Name,
        *Reason.ToString());
    return false;
}

// Engine rule for CreatePackage's own input, applied to the exact string about to be passed to it:
// rejects "//", an empty/too-short name, a missing leading slash, a trailing slash,
// INVALID_LONGPACKAGE_CHARACTERS (where '\' is caught) and an unmounted root. Call this AFTER the
// mount-point fallback, so the fallback's own concatenation is covered.
static bool PinWrightLevelStructureValidatePackagePath(const FString& PackagePath, FString& OutError)
{
    FText Reason;
    if (FPackageName::IsValidLongPackageName(PackagePath, /*bIncludeReadOnlyRoots=*/true, &Reason))
    {
        return true;
    }
    OutError = FString::Printf(TEXT("'%s' is not a valid package path: %s"), *PackagePath,
        *Reason.ToString());
    return false;
}


// ============================================================================
// Levels Handlers (3 actions)
// ============================================================================

// ---- level.structure.create_level ----
REGISTER_RPC_HANDLER("level.structure.create_level", "level.structure",
    "Create a new level package, optionally as a World-Partition map. Companion to level.create — that one is non-WP only; this one supports both via bCreateWorldPartition.",
    RPC_PARAMS(
        LevelNameParamUtils::CreateNameParam(TEXT("Short name for the new level (no extension); accepts the 'name' alias."), /*bRequired=*/true),
        RPC_PARAM_OPT("levelPath", "path", "Destination folder path for the new package. Defaults to /Game/Maps."),
        RPC_PARAM_DEF("bCreateWorldPartition", "boolean", "Create the level with World Partition enabled if true; flat-world otherwise.", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the package immediately after creation.", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // levelName is required - resolve through the {levelName, name} alias set. The
    // dispatcher already rejected a payload carrying neither key as MISSING_REQUIRED_PARAM,
    // so an empty result here means the key was present but blank.
    FString LevelName = LevelNameParamUtils::ResolveCreateName(Ctx);

    // Fail if levelName resolved empty (present-but-blank, or whitespace-only).
    if (LevelName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("levelName is required for create_level"));
        return true;
    }

    // Validate levelName for invalid characters
    const FString InvalidChars = TEXT("\\/:*?\"<>|");
    for (const TCHAR& Char : LevelName)
    {
        if (InvalidChars.Contains(FString(1, &Char)))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("levelName contains invalid character: '%c'. Cannot use: \\ / : * ? \" < > |"), Char));
            return true;
        }
    }

    // Check length (max 255 chars)
    if (LevelName.Len() > 255)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("levelName exceeds maximum length of 255 characters"));
        return true;
    }

    // Check for reserved Windows filenames
    const TArray<FString> ReservedNames = {
        TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"),
        TEXT("COM1"), TEXT("COM2"), TEXT("COM3"), TEXT("COM4"), TEXT("COM5"),
        TEXT("COM6"), TEXT("COM7"), TEXT("COM8"), TEXT("COM9"),
        TEXT("LPT1"), TEXT("LPT2"), TEXT("LPT3"), TEXT("LPT4"), TEXT("LPT5"),
        TEXT("LPT6"), TEXT("LPT7"), TEXT("LPT8"), TEXT("LPT9")
    };
    FString UpperLevelName = LevelName.ToUpper();
    if (ReservedNames.Contains(UpperLevelName))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("levelName cannot be a reserved Windows device name: %s"), *LevelName));
        return true;
    }

    // levelName is concatenated onto levelPath and handed to CreatePackage, which is Fatal on a
    // composed "//" or on a name that resolves to empty. The character filter above covers '/'
    // and '\' but not '.', so ".." still reaches the second Fatal. Keep this check ABOVE the
    // levelPath read and its sanitizer (see PinWrightLevelStructureValidateBareName).
    FString LevelNameError;
    if (!PinWrightLevelStructureValidateBareName(LevelName, TEXT("levelName"), LevelNameError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), LevelNameError);
        return true;
    }

    FString LevelPath = GetJsonStringField(Payload, TEXT("levelPath"), TEXT("/Game/Maps"));
    bool bCreateWorldPartition = GetJsonBoolField(Payload, TEXT("bCreateWorldPartition"), false);
    bool bSave = GetJsonBoolField(Payload, TEXT("save"), true);

    // Security: Validate level path format to prevent traversal attacks
    FString SafeLevelPath = SanitizeProjectRelativePath(LevelPath);
    if (SafeLevelPath.IsEmpty())
    {
        Ctx.SendError(TEXT("SECURITY_VIOLATION"),
            FString::Printf(TEXT("Invalid or unsafe level path: %s"), *LevelPath));
        return true;
    }
    LevelPath = SafeLevelPath;

    // Build full path
    FString FullPath = LevelPath / LevelName;
    if (!IsValidMountPoint(FullPath))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P`. Concatenation MANUFACTURES a "//"
        // whenever P already carries a leading slash - which is always here, since FullPath was
        // composed from a sanitized folder - and that byte sequence is what CreatePackage logs
        // Fatal on. FString::operator/ routes through PathAppend, which absorbs the duplicate
        // separator. The same rule applies at the four sibling prepends in this file.
        FullPath = FString(TEXT("/Game")) / FullPath;
    }

    // The prepend above no longer manufactures a "//", but the caller's own text can still carry
    // one. Validate the exact string that reaches CreatePackage.
    FString LevelPackagePathError;
    if (!PinWrightLevelStructureValidatePackagePath(FullPath, LevelPackagePathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), LevelPackagePathError);
        return true;
    }

    // CRITICAL: Check if level already exists to prevent WorldSettings collision crash
    UPackage* ExistingPackage = FindObject<UPackage>(nullptr, *FullPath);
    if (ExistingPackage)
    {
        UWorld* ExistingWorld = FindObject<UWorld>(ExistingPackage, *LevelName);
        if (ExistingWorld)
        {
            Ctx.SendError(TEXT("LEVEL_ALREADY_EXISTS"),
                FString::Printf(TEXT("Level already exists in memory: %s. Use load_level or provide a different name."), *FullPath));
            return true;
        }
    }

    if (FPackageName::DoesPackageExist(FullPath))
    {
        Ctx.SendError(TEXT("LEVEL_ALREADY_EXISTS"),
            FString::Printf(TEXT("Level already exists: %s. Use load_level or provide a different name."), *FullPath));
        return true;
    }

    // Create the level package
    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            FString::Printf(TEXT("Failed to create package for level: %s"), *FullPath));
        return true;
    }

    // Create a new world. When World Partition is requested, attach a real
    // UWorldPartition by passing a FWorldInitializationValues with
    // CreateWorldPartition(true) through UWorld::InitializeNewWorld — the same engine
    // entry point NewMap ultimately reaches — instead of the old hardcoded no-op stub.
    // Passing an explicit IVS also runs InitWorld internally, so the manual InitWorld
    // below is skipped for this path (guarded by bIsWorldInitialized).
    //
    // The IVS must mirror the engine's own nullptr-IVS defaults for this EWorldType
    // (World.cpp UWorld::CreateWorld) and only flip the WP bit — a bare
    // default-constructed IVS would silently create an Inactive world with a physics
    // scene, simulated physics, navigation and AI systems (struct defaults) that an
    // Inactive editor world must NOT have, and lose trace collision, purely as a side
    // effect of toggling WP. Building this IVS unconditionally (WP bit set from the
    // flag) keeps the WP and non-WP worlds identical except for partitioning.
    constexpr EWorldType::Type WorldType = EWorldType::Inactive;
    UWorld::InitializationValues IVS = UWorld::InitializationValues()
        .CreatePhysicsScene(WorldType != EWorldType::Inactive)
        .ShouldSimulatePhysics(false)
        .EnableTraceCollision(true)
        .CreateNavigation(WorldType == EWorldType::Editor)
        .CreateAISystem(WorldType == EWorldType::Editor)
        .CreateWorldPartition(bCreateWorldPartition);
    UWorld* NewWorld = UWorld::CreateWorld(WorldType, false, FName(*LevelName), Package,
        /*bAddToRoot=*/true, ERHIFeatureLevel::Num, &IVS);
    if (!NewWorld)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            FString::Printf(TEXT("Failed to create world for level: %s"), *FullPath));
        return true;
    }

    // Initialize the world only if not already initialized (CreateWorld with an
    // explicit IVS already ran InitWorld inside InitializeNewWorld).
    if (!NewWorld->bIsWorldInitialized)
    {
        NewWorld->InitWorld();
    }

    // Report the actual partitioned state by querying the attached UWorldPartition,
    // never a hardcoded constant. If the engine genuinely partitioned the world,
    // GetWorldPartition() is non-null and the whole level.structure WP namespace
    // (configure_grid_size / create_data_layer / configure_hlod_layer / …) becomes
    // reachable on this world.
    bool bWorldPartitionActuallyEnabled = false;
    if (bCreateWorldPartition)
    {
        bWorldPartitionActuallyEnabled = (NewWorld->GetWorldPartition() != nullptr);
    }

    // Mark package dirty
    Package->MarkPackageDirty();

    // Save if requested
    bool bSaveSucceeded = true;
    if (bSave)
    {
        // CRITICAL: Use McpSafeLevelSave to avoid Intel GPU driver crashes.
        bSaveSucceeded = McpSafeLevelSave(NewWorld->PersistentLevel, FullPath, 5);

        // This is a create-to-a-new-/Game/-path flow: we already proved above
        // (DoesPackageExist == false) that nothing existed on disk at FullPath,
        // so a genuine save MUST land a .umap there. The shared
        // ShouldTreatLevelSaveAsSuccess OR-policy intentionally accepts
        // package/clean/registry signals for package/mount workflows where the
        // direct file probe is invalid — but for THIS flow the only honest
        // persistence signal is the file on disk. Re-verify it here so we never
        // report a false saved:true for a level that was never written.
        FString LevelFilename;
        const bool bConverted = FPackageName::TryConvertLongPackageNameToFilename(
            FullPath, LevelFilename, FPackageName::GetMapPackageExtension());
        const bool bFileOnDisk = bConverted && IFileManager::Get().FileExists(*LevelFilename);
        const bool bPreviouslySucceeded = bSaveSucceeded;
        bSaveSucceeded = ShouldTreatCreateLevelSaveAsSuccess(bSaveSucceeded, bFileOnDisk);
        if (bPreviouslySucceeded && !bSaveSucceeded)
        {
            UE_LOG(LogMcpLevelStructureHandler, Error,
                TEXT("create_level: save reported success but no .umap on disk for new path: %s (file=%s)"),
                *FullPath, *LevelFilename);
        }

        if (bSaveSucceeded)
        {
            if (bConverted)
            {
                IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
                TArray<FString> FilesToScan;
                FilesToScan.Add(LevelFilename);
                AssetRegistry.ScanFilesSynchronous(FilesToScan, true);
            }
        }
        else
        {
            UE_LOG(LogMcpLevelStructureHandler, Error, TEXT("McpSafeLevelSave failed for: %s"), *FullPath);

            // Fail loud BEFORE tearing the world down — AddAssetVerification below
            // would otherwise dereference a destroyed UWorld.
            Ctx.SendError(TEXT("SAVE_VERIFICATION_FAILED"),
                FString::Printf(TEXT("Level created but no .umap was written to disk: %s"), *FullPath));

            // Don't strand an orphaned in-memory UWorld. If we leave it, the
            // LEVEL_ALREADY_EXISTS guard above (FindObject<UPackage>/<UWorld>)
            // permanently blocks recreation with the same name even though no
            // file ever landed. Tear the world down and rename the package to a
            // transient name so a retry can recreate it cleanly.
            NewWorld->DestroyWorld(false);
            Package->ClearDirtyFlag();
            Package->ClearFlags(RF_Standalone | RF_Public);
            Package->Rename(
                *MakeUniqueObjectName(GetTransientPackage(), UPackage::StaticClass()).ToString(),
                GetTransientPackage(),
                REN_DontCreateRedirectors | REN_NonTransactional);
            Package->MarkAsGarbage();
            return true;
        }
    }

    // Make the created world the ACTIVE editor world. Without this, the world is
    // created as EWorldType::Inactive and never installed in the editor world
    // context, so every WP-gated level.structure verb (configure_grid_size /
    // create_data_layer / create_minimap_volume / configure_hlod_layer /
    // get_level_structure_info / …) — all of which resolve their target via
    // GetEditorWorldLS() == GEditor->GetEditorWorldContext().World() — would operate
    // on whatever map was already open, never on the world the caller just created.
    // That stranded the WP-authoring flow create_level → configure_grid_size →
    // create_data_layer was built for. This mirrors the activation legacy
    // level.create already performs (LevelHandler.cpp: GEditor->NewMap(false) +
    // GetEditorWorldContext().SetCurrentWorld(NewWorld)).
    //
    // The active editor world is expected to be EWorldType::Editor (the engine's own
    // active-world path, e.g. NewMap / FScopedEditorWorld, installs an Editor world),
    // so promote the world's type from Inactive to Editor before installing it as
    // current. Done on BOTH the save:false branch and the post-successful-save:true
    // path — only a save FAILURE returns early above (after tearing the world down),
    // and that path never reaches here.
    bool bMadeActiveWorld = false;
    if (GEditor)
    {
        FWorldContext& EditorContext = GEditor->GetEditorWorldContext();
        UWorld* const OutgoingWorld = EditorContext.World();

        // Swapping the active editor world is NOT just a SetCurrentWorld + GWorld write.
        // The engine's own active-world install (UEditorEngine::NewMap / UEditorEngine::Map_Load,
        // EditorServer.cpp) FIRST tears the OUTGOING world down (its private EditorDestroyWorld),
        // which detaches it from the editor's persistent subsystems — the Chaos physics scene and
        // the Niagara world manager keep references to a live world otherwise — clears the actor
        // selection, flushes streaming, and RemoveFromRoot's it so GC can reclaim it. Skip that
        // teardown and merely point the context at our new world, and the previously-open map is
        // stranded half-attached: FPhysScene_Chaos / FNiagaraWorldManager keep referencing it, it
        // never GCs, and the NEXT real map open (Map_Load -> CheckForWorldGCLeaks,
        // EditorServer.cpp:1938 — e.g. the test harness's map-restore guard, or any later
        // editor.open_level / level.load) fatals with "World Memory Leaks: Old World not cleaned
        // up by GC". EditorDestroyWorld is private, so reproduce its GC-critical teardown with the
        // public surface: clear the actor selection, then UWorld::DestroyWorld the outgoing world
        // (which runs CleanupWorld -> broadcasts FWorldDelegates::OnWorldCleanup, the hook Niagara
        // and Chaos use to drop the world; flushes level streaming; RemoveFromRoot's it). The
        // NewWorld arg keeps our freshly-created world alive through that teardown. Skip when the
        // outgoing world IS our new world (defensive — they are always distinct here).
        //
        // This block is why `level.structure.create_level` is listed in the
        // tick-unsafe method table in Dispatch/SafePoint.cpp. Unlike its siblings it
        // does NOT reach CollectGarbage on this stack — UWorld::DestroyWorld
        // (World.cpp:2770-2801) contains no collect — so it cannot trip
        // FreeTickTaskLevel directly. It is unsafe for a stricter reason: it runs
        // FlushLevelStreaming (World.cpp:2775), CleanupWorld (:2776) and
        // RemoveFromRoot (:2792) against, and then swaps GWorld away from, the world
        // that may be in the middle of UWorld::Tick right now. The dispatcher
        // re-queues the whole request onto the core ticker; do not add a second gate
        // here.
        if (OutgoingWorld && OutgoingWorld != NewWorld)
        {
            GEditor->SelectNone(/*bNoteSelectionChange=*/true, /*bDeselectBSPSurfs=*/true);
            OutgoingWorld->DestroyWorld(/*bInformEngineOfWorld=*/true, /*NewWorld=*/NewWorld);
        }

        NewWorld->WorldType = EWorldType::Editor;
        EditorContext.SetCurrentWorld(NewWorld);
        // FWorldContext::SetCurrentWorld updates only the context's world pointer, NOT the GWorld
        // global (UnrealEngine.cpp). NewMap / Map_Load set BOTH in lock-step
        // (Context.SetCurrentWorld(World); GWorld = World;), and the editor invariant
        // Context.World() == GWorld is asserted at the top of Map_Load (EditorServer.cpp:2370);
        // leaving GWorld stale crashes the next map open. Mirror the engine.
        GWorld = NewWorld;
        bMadeActiveWorld = (GetEditorWorldLS() == NewWorld);
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddAssetVerification(ResponseJson, NewWorld);
    ResponseJson->SetStringField(TEXT("levelName"), LevelName);
    ResponseJson->SetStringField(TEXT("levelPath"), FullPath);
    ResponseJson->SetBoolField(TEXT("worldPartitionEnabled"), bWorldPartitionActuallyEnabled);
    ResponseJson->SetBoolField(TEXT("worldPartitionRequested"), bCreateWorldPartition);
    ResponseJson->SetBoolField(TEXT("saved"), bSave && bSaveSucceeded);
    // Disclose activation so a caller (or a no-GEditor commandlet context where the
    // world could not be installed) knows whether the follow-on WP-gated verbs will
    // actually target this world.
    ResponseJson->SetBoolField(TEXT("activeWorld"), bMadeActiveWorld);
    if (bMadeActiveWorld)
    {
        ResponseJson->SetStringField(TEXT("note"),
            TEXT("This world is now the active editor world, so the World-Partition level.structure verbs "
                 "(configure_grid_size / create_data_layer / create_minimap_volume / etc.) operate on it."));
    }
    else
    {
        ResponseJson->SetStringField(TEXT("note"),
            TEXT("This world was created in memory but could NOT be made the active editor world (no editor), "
                 "so the active-world level.structure verbs will not target it."));
    }
    if (!bSave)
    {
        // A failed save returns early above (SendError at the !bSaveSucceeded branch),
        // so reaching here with bSave set always means the save succeeded — the only
        // not-persisted case left is save:false (built in memory only).
        ResponseJson->SetStringField(TEXT("persistenceNote"),
            TEXT("save:false — created in memory only, not written to disk; level.load will refuse it with "
                 "LEVEL_NOT_PERSISTED. It is reachable now because it is the active editor world."));
    }
    if (bCreateWorldPartition && !bWorldPartitionActuallyEnabled)
    {
        // The engine declined to attach a UWorldPartition to the requested world
        // (unexpected — CreateWorldPartition(true) normally succeeds). Surface it
        // instead of silently reporting a non-WP world as if WP were unsupported.
        ResponseJson->SetStringField(TEXT("worldPartitionNote"), TEXT("World Partition was requested but the engine did not attach a UWorldPartition to the new world"));
    }

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.configure_level_streaming ----
REGISTER_RPC_HANDLER("level.structure.configure_level_streaming", "level.structure",
    "Update streaming-method, initial visibility, and load-on-startup flags on an existing streaming level. Use after level.add_sublevel to tune behaviour.",
    RPC_PARAMS(
        RPC_PARAM_REQ("levelName", "string", "Name of the streaming level"),
        RPC_PARAM_DEF("streamingMethod", "string", "Streaming method", "Blueprint"),
        RPC_PARAM_DEF("bShouldBeVisible", "boolean", "Should be visible", "true"),
        RPC_PARAM_DEF("bShouldBlockOnLoad", "boolean", "Block on load", "false"),
        RPC_PARAM_DEF("bDisableDistanceStreaming", "boolean", "Disable distance streaming", "false")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // CRITICAL: levelName is required
    FString LevelName;
    if (Payload.IsValid())
    {
        Payload->TryGetStringField(TEXT("levelName"), LevelName);
    }

    if (LevelName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("levelName is required for configure_level_streaming"));
        return true;
    }

    FString StreamingMethod = GetJsonStringField(Payload, TEXT("streamingMethod"), TEXT("Blueprint"));
    bool bShouldBeVisible = GetJsonBoolField(Payload, TEXT("bShouldBeVisible"), true);
    bool bShouldBlockOnLoad = GetJsonBoolField(Payload, TEXT("bShouldBlockOnLoad"), false);
    bool bDisableDistanceStreaming = GetJsonBoolField(Payload, TEXT("bDisableDistanceStreaming"), false);

    // Only reclass the level when streamingMethod was actually sent: switching the class
    // unloads and re-adds the level, so the schema default ("Blueprint") must not silently
    // convert an AlwaysLoaded level on a call that just meant to flip a visibility flag.
    const bool bStreamingMethodRequested =
        Payload.IsValid() && Payload->HasTypedField<EJson::String>(TEXT("streamingMethod"));

    UClass* RequestedStreamingClass = nullptr;
    if (bStreamingMethodRequested)
    {
        const FString NormalizedMethod = StreamingMethod.Replace(TEXT(" "), TEXT(""));
        if (NormalizedMethod.Equals(TEXT("Blueprint"), ESearchCase::IgnoreCase) ||
            NormalizedMethod.Equals(TEXT("Dynamic"), ESearchCase::IgnoreCase))
        {
            RequestedStreamingClass = ULevelStreamingDynamic::StaticClass();
        }
        else if (NormalizedMethod.Equals(TEXT("AlwaysLoaded"), ESearchCase::IgnoreCase))
        {
            RequestedStreamingClass = ULevelStreamingAlwaysLoaded::StaticClass();
        }
        else
        {
            Ctx.SendError(TEXT("UNKNOWN_STREAMING_METHOD"),
                FString::Printf(TEXT("Unknown streamingMethod: %s. Valid: Blueprint (alias Dynamic), AlwaysLoaded"),
                    *StreamingMethod));
            return true;
        }
    }

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    // Find the streaming level
    ULevelStreaming* FoundLevel = nullptr;
    for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        if (StreamingLevel && StreamingLevel->GetWorldAssetPackageFName().ToString().Contains(LevelName))
        {
            FoundLevel = StreamingLevel;
            break;
        }
    }

    if (!FoundLevel)
    {
        Ctx.SendError(TEXT("LEVEL_NOT_FOUND"),
            FString::Printf(TEXT("Streaming level not found: %s"), *LevelName));
        return true;
    }

    // Apply the streaming method by swapping the level's ULevelStreaming class. The class is
    // the only carrier of the streaming method, so nothing short of this actually changes it.
    bool bStreamingClassChanged = false;
    if (RequestedStreamingClass && FoundLevel->GetClass() != RequestedStreamingClass)
    {
        // SetStreamingClassForLevel check()s the loaded ULevel (it hides, removes and re-adds
        // the level), so it would assert on a level that was never loaded.
        if (!FoundLevel->GetLoadedLevel())
        {
            Ctx.SendError(TEXT("LEVEL_NOT_LOADED"),
                FString::Printf(TEXT("Streaming level '%s' is not loaded; its streaming method cannot be changed until it is."),
                    *LevelName));
            return true;
        }

        ULevelStreaming* ReclassedLevel = UEditorLevelUtils::SetStreamingClassForLevel(FoundLevel, RequestedStreamingClass);
        if (!ReclassedLevel)
        {
            Ctx.SendError(TEXT("OPERATION_FAILED"),
                FString::Printf(TEXT("Failed to set streaming class %s on level: %s"),
                    *RequestedStreamingClass->GetName(), *LevelName));
            return true;
        }
        // The old streaming level was removed from the world by the reclass.
        FoundLevel = ReclassedLevel;
        bStreamingClassChanged = true;
    }

    // Configure streaming settings
    FoundLevel->SetShouldBeVisible(bShouldBeVisible);
    FoundLevel->bShouldBlockOnLoad = bShouldBlockOnLoad;
    FoundLevel->bDisableDistanceStreaming = bDisableDistanceStreaming;

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddAssetVerification(ResponseJson, World);
    ResponseJson->SetStringField(TEXT("levelName"), LevelName);
    ResponseJson->SetStringField(TEXT("streamingMethod"), StreamingMethod);
    ResponseJson->SetStringField(TEXT("streamingClass"), FoundLevel->GetClass()->GetName());
    ResponseJson->SetBoolField(TEXT("streamingClassChanged"), bStreamingClassChanged);
    ResponseJson->SetBoolField(TEXT("shouldBeVisible"), bShouldBeVisible);

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.set_streaming_distance ----
REGISTER_RPC_HANDLER("level.structure.set_streaming_distance", "level.structure",
    "Configure level-streaming-volume distances and usage mode for the named streaming level. Affects when the level loads/becomes visible relative to player position.",
    RPC_PARAMS(
        RPC_PARAM_REQ("levelName", "string", "Name of the streaming level"),
        RPC_PARAM_DEF("streamingDistance", "number", "Streaming distance", "10000"),
        RPC_PARAM_DEF("streamingUsage", "string", "Streaming usage mode", "LoadingAndVisibility"),
        RPC_PARAM_OPT("volumeLocation", "object", "Location for the volume {x,y,z}"),
        RPC_PARAM_DEF("createVolume", "boolean", "Create a streaming volume", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // CRITICAL: levelName is required
    FString LevelName;
    if (Payload.IsValid())
    {
        Payload->TryGetStringField(TEXT("levelName"), LevelName);
    }

    if (LevelName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("levelName is required for set_streaming_distance"));
        return true;
    }

    double StreamingDistance = GetJsonNumberField(Payload, TEXT("streamingDistance"), 10000.0);
    FString StreamingUsage = GetJsonStringField(Payload, TEXT("streamingUsage"), TEXT("LoadingAndVisibility"));
    TSharedPtr<FJsonObject> VolumeLocationJson = GetObjectFieldLS(Payload, TEXT("volumeLocation"));
    FVector VolumeLocation = VolumeLocationJson.IsValid() ? GetVectorFromJsonLS(VolumeLocationJson) : FVector::ZeroVector;
    bool bCreateVolume = GetJsonBoolField(Payload, TEXT("createVolume"), true);

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    // Find the streaming level
    ULevelStreaming* FoundLevel = nullptr;
    for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        if (StreamingLevel && StreamingLevel->GetWorldAssetPackageFName().ToString().Contains(LevelName))
        {
            FoundLevel = StreamingLevel;
            break;
        }
    }

    if (!FoundLevel)
    {
        Ctx.SendError(TEXT("LEVEL_NOT_FOUND"),
            FString::Printf(TEXT("Streaming level not found: %s"), *LevelName));
        return true;
    }

    if (!bCreateVolume)
    {
        // Just report current streaming volumes
        TArray<TSharedPtr<FJsonValue>> VolumesArray;
        for (ALevelStreamingVolume* Volume : FoundLevel->EditorStreamingVolumes)
        {
            if (Volume)
            {
                TSharedPtr<FJsonObject> VolumeObj = MakeShareable(new FJsonObject());
                VolumeObj->SetStringField(TEXT("name"), Volume->GetActorLabel());
                VolumeObj->SetNumberField(TEXT("usage"), static_cast<int32>(Volume->StreamingUsage));
                VolumesArray.Add(MakeShareable(new FJsonValueObject(VolumeObj)));
            }
        }

        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
        AddAssetVerification(ResponseJson, World);
        ResponseJson->SetStringField(TEXT("levelName"), LevelName);
        ResponseJson->SetArrayField(TEXT("streamingVolumes"), VolumesArray);
        ResponseJson->SetNumberField(TEXT("volumeCount"), VolumesArray.Num());
        ResponseJson->SetStringField(TEXT("note"), TEXT("Use createVolume=true to create a streaming volume for distance-based loading"));

        Ctx.SendSuccess(ResponseJson);
        return true;
    }

    // Create an ALevelStreamingVolume
    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = MakeUniqueObjectName(World, ALevelStreamingVolume::StaticClass(),
        FName(*FString::Printf(TEXT("StreamingVolume_%s"), *LevelName)));
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ALevelStreamingVolume* NewVolume = World->SpawnActor<ALevelStreamingVolume>(
        ALevelStreamingVolume::StaticClass(),
        VolumeLocation,
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (!NewVolume)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Failed to spawn ALevelStreamingVolume actor"));
        return true;
    }

    NewVolume->SetActorLabel(FString::Printf(TEXT("StreamingVolume_%s"), *LevelName));

    // Configure streaming usage
    if (StreamingUsage == TEXT("Loading"))
    {
        NewVolume->StreamingUsage = EStreamingVolumeUsage::SVB_Loading;
    }
    else if (StreamingUsage == TEXT("VisibilityBlockingOnLoad"))
    {
        NewVolume->StreamingUsage = EStreamingVolumeUsage::SVB_VisibilityBlockingOnLoad;
    }
    else if (StreamingUsage == TEXT("BlockingOnLoad"))
    {
        NewVolume->StreamingUsage = EStreamingVolumeUsage::SVB_BlockingOnLoad;
    }
    else if (StreamingUsage == TEXT("LoadingNotVisible"))
    {
        NewVolume->StreamingUsage = EStreamingVolumeUsage::SVB_LoadingNotVisible;
    }
    else // Default: LoadingAndVisibility
    {
        NewVolume->StreamingUsage = EStreamingVolumeUsage::SVB_LoadingAndVisibility;
    }

    // Scale the volume to match the streaming distance
    FVector DesiredScale = FVector(StreamingDistance / 100.0);
    NewVolume->SetActorScale3D(DesiredScale);

    // Associate the volume with the streaming level
    FoundLevel->EditorStreamingVolumes.AddUnique(NewVolume);

    UE_LOG(LogMcpLevelStructureHandler, Verbose, TEXT("Streaming volume created - refs will update on save"));

    FoundLevel->MarkPackageDirty();
    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddActorVerification(ResponseJson, NewVolume);
    ResponseJson->SetStringField(TEXT("levelName"), LevelName);
    ResponseJson->SetStringField(TEXT("volumeName"), NewVolume->GetActorLabel());
    ResponseJson->SetNumberField(TEXT("streamingDistance"), StreamingDistance);
    ResponseJson->SetStringField(TEXT("streamingUsage"), StreamingUsage);

    TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject());
    LocationJson->SetNumberField(TEXT("x"), VolumeLocation.X);
    LocationJson->SetNumberField(TEXT("y"), VolumeLocation.Y);
    LocationJson->SetNumberField(TEXT("z"), VolumeLocation.Z);
    ResponseJson->SetObjectField(TEXT("volumeLocation"), LocationJson);

    ResponseJson->SetNumberField(TEXT("totalStreamingVolumes"), FoundLevel->EditorStreamingVolumes.Num());

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ============================================================================
// World Partition Handlers (6 actions)
// ============================================================================

// ---- level.structure.enable_world_partition ----
REGISTER_RPC_HANDLER("level.structure.enable_world_partition", "level.structure",
    "Report the active level's World-Partition state. Cannot enable WP on an already-loaded non-WP world (returns OPERATION_FAILED; create a partitioned map with create_level {bCreateWorldPartition:true} instead); bEnableWorldPartition only no-ops when it matches the current flag.",
    RPC_PARAMS(
        RPC_PARAM_DEF("bEnableWorldPartition", "boolean", "Enable World Partition", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    bool bEnable = GetJsonBoolField(Payload, TEXT("bEnableWorldPartition"), true);

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    UWorldPartition* WorldPartition = World->GetWorldPartition();

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetBoolField(TEXT("worldPartitionEnabled"), WorldPartition != nullptr);
    ResponseJson->SetBoolField(TEXT("requested"), bEnable);

    if (bEnable && !WorldPartition)
    {
        ResponseJson->SetStringField(TEXT("note"), TEXT("World Partition must be enabled when creating the level. Convert existing level via Edit > Convert Level"));
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            TEXT("Cannot enable World Partition programmatically. Use 'Edit > Convert Level' in editor or create a new level with World Partition enabled."));
        return true;
    }

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.configure_grid_size ----
REGISTER_RPC_HANDLER("level.structure.configure_grid_size", "level.structure",
    "Set the cell size and loading range of a World Partition grid. Bigger cells = fewer streaming actors but coarser visibility; loadingRange controls how aggressively cells stream in around the player.",
    RPC_PARAMS(
        RPC_PARAM_OPT("gridName", "string", "Name of the grid to configure"),
        RPC_PARAM_DEF("gridCellSize", "number", "Grid cell size", "12800"),
        RPC_PARAM_DEF("loadingRange", "number", "Loading range", "25600"),
        RPC_PARAM_DEF("bBlockOnSlowStreaming", "boolean", "Block on slow streaming", "false"),
        RPC_PARAM_DEF("priority", "number", "Grid priority", "0"),
        RPC_PARAM_DEF("createIfMissing", "boolean", "Create grid if not found", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString GridName = GetJsonStringField(Payload, TEXT("gridName"), TEXT(""));
    int32 GridCellSize = GetJsonIntField(Payload, TEXT("gridCellSize"), 12800);
    float LoadingRange = static_cast<float>(GetJsonNumberField(Payload, TEXT("loadingRange"), 25600.0));
    bool bBlockOnSlowStreaming = GetJsonBoolField(Payload, TEXT("bBlockOnSlowStreaming"), false);
    int32 Priority = GetJsonIntField(Payload, TEXT("priority"), 0);
    bool bCreateIfMissing = GetJsonBoolField(Payload, TEXT("createIfMissing"), true);

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("World Partition is not enabled for this level"));
        return true;
    }

    // Get the runtime hash
    UWorldPartitionRuntimeHash* RuntimeHash = WorldPartition->RuntimeHash;
    if (!RuntimeHash)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("World Partition RuntimeHash not available"));
        return true;
    }

    // Check if we're dealing with RuntimeSpatialHash or RuntimeHashSet
    UWorldPartitionRuntimeSpatialHash* SpatialHash = Cast<UWorldPartitionRuntimeSpatialHash>(RuntimeHash);
    UWorldPartitionRuntimeHashSet* HashSet = Cast<UWorldPartitionRuntimeHashSet>(RuntimeHash);

    if (!SpatialHash && !HashSet)
    {
        TSharedPtr<FJsonObject> ErrorJson = MakeShareable(new FJsonObject());
        ErrorJson->SetStringField(TEXT("currentHashType"), RuntimeHash->GetClass()->GetName());
        ErrorJson->SetStringField(TEXT("supportedHashTypes"), TEXT("WorldPartitionRuntimeSpatialHash, WorldPartitionRuntimeHashSet"));
        ErrorJson->SetStringField(TEXT("hint"), TEXT("World Partition must use RuntimeSpatialHash for grid configuration."));
        ErrorJson->SetStringField(TEXT("solution"), TEXT("Create a new level with World Partition enabled, or check World Partition settings in the editor."));

        Ctx.SendError(TEXT("INVALID_PARTITION_TYPE"),
            FString::Printf(TEXT("World Partition is using unsupported hash type: %s. Grid configuration not applicable."),
                *RuntimeHash->GetClass()->GetName()));
        return true;
    }

    // Handle RuntimeHashSet (UE 5.1+ only)
    if (HashSet)
    {
        FProperty* PartitionsProperty = HashSet->GetClass()->FindPropertyByName(TEXT("RuntimePartitions"));
        if (!PartitionsProperty)
        {
            Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Could not find RuntimePartitions property on RuntimeHashSet"));
            return true;
        }

        FArrayProperty* ArrayProp = CastField<FArrayProperty>(PartitionsProperty);
        if (!ArrayProp)
        {
            Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("RuntimePartitions property is not an array"));
            return true;
        }

        void* PartitionsArrayPtr = PartitionsProperty->ContainerPtrToValuePtr<void>(HashSet);
        FScriptArrayHelper ArrayHelper(ArrayProp, PartitionsArrayPtr);

        bool bFound = false;
        bool bCreated = false;
        int32 ModifiedIndex = -1;
        FName TargetPartitionName = GridName.IsEmpty() ? FName(TEXT("MainPartition")) : FName(*GridName);

        FStructProperty* StructProp = CastField<FStructProperty>(ArrayProp->Inner);
        if (!StructProp)
        {
            Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("RuntimePartitions array element is not a struct"));
            return true;
        }

        UStruct* PartitionStruct = StructProp->Struct;

        for (int32 i = 0; i < ArrayHelper.Num(); ++i)
        {
            void* PartitionPtr = ArrayHelper.GetRawPtr(i);
            if (!PartitionPtr) continue;

            FProperty* NameProp = PartitionStruct->FindPropertyByName(TEXT("Name"));
            if (NameProp && NameProp->IsA<FNameProperty>())
            {
                FNameProperty* NameProperty = CastField<FNameProperty>(NameProp);
                FName PartitionName = NameProperty->GetPropertyValue(PartitionPtr);

                if (PartitionName == TargetPartitionName)
                {
                    FProperty* LoadingRangeProp = PartitionStruct->FindPropertyByName(TEXT("LoadingRange"));
                    if (LoadingRangeProp && LoadingRangeProp->IsA<FFloatProperty>())
                    {
                        CastField<FFloatProperty>(LoadingRangeProp)->SetPropertyValue(PartitionPtr, LoadingRange);
                    }

                    FProperty* GridSizeProp = PartitionStruct->FindPropertyByName(TEXT("GridSize"));
                    if (!GridSizeProp)
                    {
                        GridSizeProp = PartitionStruct->FindPropertyByName(TEXT("CellSize"));
                    }
                    if (GridSizeProp && GridSizeProp->IsA<FIntProperty>())
                    {
                        CastField<FIntProperty>(GridSizeProp)->SetPropertyValue(PartitionPtr, GridCellSize);
                    }

                    bFound = true;
                    ModifiedIndex = i;
                    break;
                }
            }
        }

        // If not found and createIfMissing, add a new partition
        if (!bFound && bCreateIfMissing)
        {
            int32 NewIndex = ArrayHelper.AddValue();
            void* NewPartition = ArrayHelper.GetRawPtr(NewIndex);
            if (NewPartition)
            {
                FProperty* NameProp = PartitionStruct->FindPropertyByName(TEXT("Name"));
                if (NameProp && NameProp->IsA<FNameProperty>())
                {
                    CastField<FNameProperty>(NameProp)->SetPropertyValue(NewPartition, TargetPartitionName);
                }

                FProperty* LoadingRangeProp = PartitionStruct->FindPropertyByName(TEXT("LoadingRange"));
                if (LoadingRangeProp && LoadingRangeProp->IsA<FFloatProperty>())
                {
                    CastField<FFloatProperty>(LoadingRangeProp)->SetPropertyValue(NewPartition, LoadingRange);
                }

                FProperty* GridSizeProp = PartitionStruct->FindPropertyByName(TEXT("GridSize"));
                if (!GridSizeProp)
                {
                    GridSizeProp = PartitionStruct->FindPropertyByName(TEXT("CellSize"));
                }
                if (GridSizeProp && GridSizeProp->IsA<FIntProperty>())
                {
                    CastField<FIntProperty>(GridSizeProp)->SetPropertyValue(NewPartition, GridCellSize);
                }

                bCreated = true;
                bFound = true;
            }
        }

        HashSet->MarkPackageDirty();

        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
        AddAssetVerification(ResponseJson, World);
        ResponseJson->SetBoolField(TEXT("success"), true);
        ResponseJson->SetStringField(TEXT("hashType"), TEXT("RuntimeHashSet"));
        ResponseJson->SetStringField(TEXT("partitionName"), TargetPartitionName.ToString());
        ResponseJson->SetNumberField(TEXT("loadingRange"), LoadingRange);
        ResponseJson->SetNumberField(TEXT("cellSize"), GridCellSize);
        ResponseJson->SetBoolField(TEXT("created"), bCreated);
        ResponseJson->SetBoolField(TEXT("modified"), bFound);

        Ctx.SendSuccess(ResponseJson);
        return true;
    }

    // Handle RuntimeSpatialHash
    FProperty* GridsProperty = SpatialHash->GetClass()->FindPropertyByName(TEXT("Grids"));
    if (!GridsProperty)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Could not find Grids property on RuntimeSpatialHash"));
        return true;
    }

    FArrayProperty* ArrayProp = CastField<FArrayProperty>(GridsProperty);
    if (!ArrayProp)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Grids property is not an array"));
        return true;
    }

    void* GridsArrayPtr = GridsProperty->ContainerPtrToValuePtr<void>(SpatialHash);
    FScriptArrayHelper ArrayHelper(ArrayProp, GridsArrayPtr);

    bool bFound = false;
    bool bCreated = false;
    int32 ModifiedIndex = -1;
    FName TargetGridName = GridName.IsEmpty() ? FName(NAME_None) : FName(*GridName);

    for (int32 i = 0; i < ArrayHelper.Num(); ++i)
    {
        FSpatialHashRuntimeGrid* Grid = reinterpret_cast<FSpatialHashRuntimeGrid*>(ArrayHelper.GetRawPtr(i));
        if (Grid)
        {
            if (GridName.IsEmpty() || Grid->GridName == TargetGridName)
            {
                Grid->CellSize = GridCellSize;
                Grid->LoadingRange = LoadingRange;
                Grid->bBlockOnSlowStreaming = bBlockOnSlowStreaming;
                Grid->Priority = Priority;

                bFound = true;
                ModifiedIndex = i;
                break;
            }
        }
    }

    // If not found and createIfMissing, add a new grid
    if (!bFound && bCreateIfMissing && !GridName.IsEmpty())
    {
        int32 NewIndex = ArrayHelper.AddValue();
        FSpatialHashRuntimeGrid* NewGrid = reinterpret_cast<FSpatialHashRuntimeGrid*>(ArrayHelper.GetRawPtr(NewIndex));
        if (NewGrid)
        {
            NewGrid->GridName = FName(*GridName);
            NewGrid->CellSize = GridCellSize;
            NewGrid->LoadingRange = LoadingRange;
            NewGrid->bBlockOnSlowStreaming = bBlockOnSlowStreaming;
            NewGrid->Priority = Priority;
            NewGrid->Origin = FVector2D::ZeroVector;
            NewGrid->DebugColor = FLinearColor::MakeRandomColor();
            NewGrid->bClientOnlyVisible = false;
            NewGrid->HLODLayer = nullptr;

            bCreated = true;
            ModifiedIndex = NewIndex;
        }
    }

    if (!bFound && !bCreated)
    {
        TArray<FString> AvailableGrids;
        for (int32 i = 0; i < ArrayHelper.Num(); ++i)
        {
            FSpatialHashRuntimeGrid* Grid = reinterpret_cast<FSpatialHashRuntimeGrid*>(ArrayHelper.GetRawPtr(i));
            if (Grid)
            {
                AvailableGrids.Add(Grid->GridName.ToString());
            }
        }

        FString AvailableStr = AvailableGrids.Num() > 0
            ? FString::Join(AvailableGrids, TEXT(", "))
            : TEXT("(none - use createIfMissing=true to create a new grid)");

        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Grid '%s' not found. Available grids: %s"), *GridName, *AvailableStr));
        return true;
    }

    // Mark the object as modified
    SpatialHash->Modify();
    SpatialHash->MarkPackageDirty();
    World->MarkPackageDirty();

    // Build response with current grid configuration
    TArray<TSharedPtr<FJsonValue>> GridsArray;
    for (int32 i = 0; i < ArrayHelper.Num(); ++i)
    {
        FSpatialHashRuntimeGrid* Grid = reinterpret_cast<FSpatialHashRuntimeGrid*>(ArrayHelper.GetRawPtr(i));
        if (Grid)
        {
            TSharedPtr<FJsonObject> GridObj = MakeShareable(new FJsonObject());
            GridObj->SetStringField(TEXT("gridName"), Grid->GridName.ToString());
            GridObj->SetNumberField(TEXT("cellSize"), Grid->CellSize);
            GridObj->SetNumberField(TEXT("loadingRange"), Grid->LoadingRange);
            GridObj->SetBoolField(TEXT("blockOnSlowStreaming"), Grid->bBlockOnSlowStreaming);
            GridObj->SetNumberField(TEXT("priority"), Grid->Priority);
            GridObj->SetBoolField(TEXT("modified"), i == ModifiedIndex);
            GridsArray.Add(MakeShareable(new FJsonValueObject(GridObj)));
        }
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddAssetVerification(ResponseJson, World);
    ResponseJson->SetStringField(TEXT("gridName"), GridName.IsEmpty() ? TEXT("(default)") : GridName);
    ResponseJson->SetNumberField(TEXT("cellSize"), GridCellSize);
    ResponseJson->SetNumberField(TEXT("loadingRange"), LoadingRange);
    ResponseJson->SetBoolField(TEXT("blockOnSlowStreaming"), bBlockOnSlowStreaming);
    ResponseJson->SetNumberField(TEXT("priority"), Priority);
    ResponseJson->SetBoolField(TEXT("created"), bCreated);
    ResponseJson->SetBoolField(TEXT("modified"), bFound);
    ResponseJson->SetArrayField(TEXT("allGrids"), GridsArray);
    ResponseJson->SetStringField(TEXT("note"), TEXT("Grid configuration updated. Regenerate streaming data to apply changes (World Partition > Generate Streaming)."));

    Ctx.SendSuccess(ResponseJson);
    return true;

}

// ---- level.structure.create_data_layer ----
REGISTER_RPC_HANDLER("level.structure.create_data_layer", "level.structure",
    "Create a UDataLayerInstance asset and register it with the active World Partition. Data layers let you toggle groups of actors on/off at runtime or per-streaming-policy.",
    RPC_PARAMS(
        RPC_PARAM_REQ("dataLayerName", "string", "Name of the data layer"),
        RPC_PARAM_DEF("dataLayerAssetPath", "path", "Asset path for the data layer", "/Game/DataLayers"),
        RPC_PARAM_DEF("bIsInitiallyVisible", "boolean", "Initially visible", "true"),
        RPC_PARAM_DEF("bIsInitiallyLoaded", "boolean", "Initially loaded", "true"),
        RPC_PARAM_DEF("dataLayerType", "string", "Data layer type (Runtime/Editor)", "Runtime"),
        RPC_PARAM_DEF("bIsPrivate", "boolean", "Private data layer", "false")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // CRITICAL: dataLayerName is required
    FString DataLayerName;
    if (Payload.IsValid())
    {
        Payload->TryGetStringField(TEXT("dataLayerName"), DataLayerName);
    }

    if (DataLayerName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("dataLayerName is required for create_data_layer"));
        return true;
    }

    // dataLayerName is concatenated onto dataLayerAssetPath and handed to CreatePackage, which is
    // Fatal on a composed "//" or on a name that resolves to empty. Nothing else validates it.
    // Keep this check ABOVE the world / World-Partition / subsystem gates and the folder
    // sanitizer (see PinWrightLevelStructureValidateBareName).
    FString DataLayerNameError;
    if (!PinWrightLevelStructureValidateBareName(DataLayerName, TEXT("dataLayerName"),
            DataLayerNameError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), DataLayerNameError);
        return true;
    }

    FString DataLayerAssetPath = GetJsonStringField(Payload, TEXT("dataLayerAssetPath"), TEXT("/Game/DataLayers"));
    bool bIsInitiallyVisible = GetJsonBoolField(Payload, TEXT("bIsInitiallyVisible"), true);
    bool bIsInitiallyLoaded = GetJsonBoolField(Payload, TEXT("bIsInitiallyLoaded"), true);
    FString DataLayerType = GetJsonStringField(Payload, TEXT("dataLayerType"), TEXT("Runtime"));
    bool bIsPrivate = GetJsonBoolField(Payload, TEXT("bIsPrivate"), false);

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition)
    {
        Ctx.SendError(TEXT("WORLD_PARTITION_NOT_ENABLED"),
            TEXT("World Partition is not enabled for this level. Data layers require World Partition."));
        return true;
    }

    UDataLayerEditorSubsystem* DataLayerEditorSubsystem = UDataLayerEditorSubsystem::Get();
    if (!DataLayerEditorSubsystem)
    {
        Ctx.SendError(TEXT("SUBSYSTEM_NOT_AVAILABLE"), TEXT("Data Layer Editor Subsystem not available"));
        return true;
    }

    // Security: Validate data layer asset path
    FString SafeAssetPath = SanitizeProjectRelativePath(DataLayerAssetPath);
    if (SafeAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("SECURITY_VIOLATION"),
            FString::Printf(TEXT("Invalid or unsafe data layer asset path: %s"), *DataLayerAssetPath));
        return true;
    }
    DataLayerAssetPath = SafeAssetPath;

    // Step 1: Create a UDataLayerAsset
    FString FullAssetPath = DataLayerAssetPath / DataLayerName;
    if (!IsValidMountPoint(FullAssetPath))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P` - see the composition note at
        // level.structure.create_level above. Concatenation manufactures the "//" CreatePackage
        // logs Fatal on; FString::operator/ absorbs the duplicate separator.
        FullAssetPath = FString(TEXT("/Game")) / FullAssetPath;
    }

    // The prepend above no longer manufactures a "//", but the caller's own text can still carry
    // one. Validate the exact string that reaches CreatePackage.
    FString DataLayerPackagePathError;
    if (!PinWrightLevelStructureValidatePackagePath(FullAssetPath, DataLayerPackagePathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), DataLayerPackagePathError);
        return true;
    }

    UPackage* AssetPackage = CreatePackage(*FullAssetPath);
    if (!AssetPackage)
    {
        Ctx.SendError(TEXT("PACKAGE_CREATION_FAILED"),
            FString::Printf(TEXT("Failed to create package for DataLayerAsset at: %s"), *FullAssetPath));
        return true;
    }

    UDataLayerAsset* NewDataLayerAsset = NewObject<UDataLayerAsset>(AssetPackage, *DataLayerName, RF_Public | RF_Standalone);
    if (!NewDataLayerAsset)
    {
        Ctx.SendError(TEXT("ASSET_CREATION_FAILED"), TEXT("Failed to create UDataLayerAsset object"));
        return true;
    }

    if (DataLayerType == TEXT("Runtime"))
    {
        NewDataLayerAsset->SetType(EDataLayerType::Runtime);
    }
    else
    {
        NewDataLayerAsset->SetType(EDataLayerType::Editor);
    }

    AssetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewDataLayerAsset);
    McpSafeAssetSave(NewDataLayerAsset);

    // Step 2: Create a UDataLayerInstance
    FDataLayerCreationParameters CreationParams;
    CreationParams.DataLayerAsset = NewDataLayerAsset;
    CreationParams.WorldDataLayers = World->GetWorldDataLayers();
    CreationParams.bIsPrivate = bIsPrivate;

    UDataLayerInstance* NewDataLayerInstance = DataLayerEditorSubsystem->CreateDataLayerInstance(CreationParams);
    if (!NewDataLayerInstance)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            FString::Printf(TEXT("Created DataLayerAsset '%s' but failed to create DataLayerInstance. The asset exists at: %s"),
                *DataLayerName, *FullAssetPath));
        return true;
    }

    // Configure initial visibility and loaded state
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // SetDataLayerIsInitiallyVisible was added in UE 5.6; absent in 5.4/5.5
    DataLayerEditorSubsystem->SetDataLayerIsInitiallyVisible(NewDataLayerInstance, bIsInitiallyVisible);
#endif
    DataLayerEditorSubsystem->SetDataLayerIsLoadedInEditor(NewDataLayerInstance, bIsInitiallyLoaded, false);

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddAssetVerification(ResponseJson, NewDataLayerAsset);
    ResponseJson->SetStringField(TEXT("dataLayerName"), DataLayerName);
    ResponseJson->SetStringField(TEXT("dataLayerAssetPath"), FullAssetPath);
    ResponseJson->SetStringField(TEXT("dataLayerType"), DataLayerType);
    ResponseJson->SetBoolField(TEXT("initiallyVisible"), bIsInitiallyVisible);
    ResponseJson->SetBoolField(TEXT("initiallyLoaded"), bIsInitiallyLoaded);
    ResponseJson->SetBoolField(TEXT("isPrivate"), bIsPrivate);

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.assign_actor_to_data_layer ----
REGISTER_RPC_HANDLER("level.structure.assign_actor_to_data_layer", "level.structure",
    "Add the named actor to a World Partition data layer so it inherits that layer's visibility and streaming policy.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_REQ("dataLayerName", "string", "Name of the data layer")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString ActorName = GetJsonStringField(Payload, TEXT("actorName"), TEXT(""));
    FString DataLayerName = GetJsonStringField(Payload, TEXT("dataLayerName"), TEXT(""));

    if (ActorName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName is required"));
        return true;
    }

    if (DataLayerName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("dataLayerName is required"));
        return true;
    }

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition)
    {
        Ctx.SendError(TEXT("WORLD_PARTITION_NOT_ENABLED"),
            TEXT("World Partition is not enabled for this level. Data layers require World Partition."));
        return true;
    }

    UDataLayerEditorSubsystem* DataLayerEditorSubsystem = UDataLayerEditorSubsystem::Get();
    if (!DataLayerEditorSubsystem)
    {
        Ctx.SendError(TEXT("SUBSYSTEM_NOT_AVAILABLE"), TEXT("Data Layer Editor Subsystem not available"));
        return true;
    }

    // Find the actor
    AActor* FoundActor = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
        {
            FoundActor = *It;
            break;
        }
    }

    if (!FoundActor)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    // Find the data layer instance by name
    UDataLayerInstance* DataLayerInstance = nullptr;

    // Method 1: Direct FName lookup
    DataLayerInstance = DataLayerEditorSubsystem->GetDataLayerInstance(FName(*DataLayerName));

    // Method 2: Search by short name (case-insensitive)
    if (!DataLayerInstance)
    {
        TArray<UDataLayerInstance*> AllDataLayers = DataLayerEditorSubsystem->GetAllDataLayers();
        for (UDataLayerInstance* DL : AllDataLayers)
        {
            if (DL)
            {
                FString ShortName = DL->GetDataLayerShortName();
                if (ShortName.Equals(DataLayerName, ESearchCase::IgnoreCase))
                {
                    DataLayerInstance = DL;
                    break;
                }
                FString FullName = DL->GetDataLayerFullName();
                if (FullName.Equals(DataLayerName, ESearchCase::IgnoreCase))
                {
                    DataLayerInstance = DL;
                    break;
                }
            }
        }
    }

    if (!DataLayerInstance)
    {
        TArray<UDataLayerInstance*> AllDataLayers = DataLayerEditorSubsystem->GetAllDataLayers();
        TArray<FString> AvailableNames;
        for (UDataLayerInstance* DL : AllDataLayers)
        {
            if (DL)
            {
                AvailableNames.Add(DL->GetDataLayerShortName());
            }
        }

        FString AvailableStr = AvailableNames.Num() > 0
            ? FString::Join(AvailableNames, TEXT(", "))
            : TEXT("(none)");

        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Data layer not found: '%s'. Available data layers: %s"), *DataLayerName, *AvailableStr));
        return true;
    }

    // IDEMPOTENCY: Check if actor is already in the target data layer
    bool bAlreadyInLayer = FoundActor->ContainsDataLayer(DataLayerInstance);

    if (bAlreadyInLayer)
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
        AddActorVerification(ResponseJson, FoundActor);
        ResponseJson->SetStringField(TEXT("actorName"), ActorName);
        ResponseJson->SetStringField(TEXT("dataLayerName"), DataLayerName);
        ResponseJson->SetBoolField(TEXT("assigned"), true);
        ResponseJson->SetBoolField(TEXT("alreadyAssigned"), true);

        Ctx.SendSuccess(ResponseJson);
        return true;
    }

    bool bSuccess = DataLayerEditorSubsystem->AddActorToDataLayer(FoundActor, DataLayerInstance);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddActorVerification(ResponseJson, FoundActor);
    ResponseJson->SetStringField(TEXT("actorName"), ActorName);
    ResponseJson->SetStringField(TEXT("dataLayerName"), DataLayerName);
    ResponseJson->SetBoolField(TEXT("assigned"), bSuccess);

    if (bSuccess)
    {
        Ctx.SendSuccess(ResponseJson);
    }
    else
    {
        ResponseJson->SetStringField(TEXT("reason"), TEXT("Actor is not compatible with data layers"));
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            FString::Printf(TEXT("Failed to assign actor '%s' to data layer '%s'. Actor may not be compatible with data layers."),
                *ActorName, *DataLayerName));
    }
    return true;
}

// ---- level.structure.configure_hlod_layer ----
REGISTER_RPC_HANDLER("level.structure.configure_hlod_layer", "level.structure",
    "Create or update a Hierarchical LOD layer. HLODs group distant actors into merged proxy meshes for streaming-friendly large worlds.",
    RPC_PARAMS(
        RPC_PARAM_REQ("hlodLayerName", "string", "Name of the HLOD layer"),
        RPC_PARAM_DEF("hlodLayerPath", "path", "Asset path for the HLOD layer", "/Game/HLOD"),
        RPC_PARAM_DEF("bIsSpatiallyLoaded", "boolean", "Spatially loaded", "true"),
        RPC_PARAM_DEF("cellSize", "number", "HLOD cell size", "25600"),
        RPC_PARAM_DEF("loadingDistance", "number", "Loading distance", "51200"),
        RPC_PARAM_DEF("layerType", "string", "HLOD layer type", "MeshMerge")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // CRITICAL: hlodLayerName is required
    FString HlodLayerName;
    if (Payload.IsValid())
    {
        Payload->TryGetStringField(TEXT("hlodLayerName"), HlodLayerName);
    }

    if (HlodLayerName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("hlodLayerName is required for configure_hlod_layer"));
        return true;
    }

    // hlodLayerName is concatenated onto hlodLayerPath and handed to CreatePackage, which is Fatal
    // on a composed "//" or on a name that resolves to empty. Nothing else validates it. Keep this
    // check ABOVE the reflection gate and the folder sanitizer (see
    // PinWrightLevelStructureValidateBareName).
    FString HlodLayerNameError;
    if (!PinWrightLevelStructureValidateBareName(HlodLayerName, TEXT("hlodLayerName"),
            HlodLayerNameError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), HlodLayerNameError);
        return true;
    }

    FString HlodLayerPath = GetJsonStringField(Payload, TEXT("hlodLayerPath"), TEXT("/Game/HLOD"));
    bool bIsSpatiallyLoaded = GetJsonBoolField(Payload, TEXT("bIsSpatiallyLoaded"), true);
    int32 CellSize = GetJsonIntField(Payload, TEXT("cellSize"), 25600);
    double LoadingDistance = GetJsonNumberField(Payload, TEXT("loadingDistance"), 51200.0);
    FString LayerType = GetJsonStringField(Payload, TEXT("layerType"), TEXT("MeshMerge"));

    // UHLODLayer ships no setters for CellSize / LoadingRange on any supported engine, and
    // SetIsSpatiallyLoaded is deprecated from 5.7 — the three fields are only reachable by
    // reflection. Resolve them before anything is created so a schema change fails the call
    // instead of leaving a half-configured asset behind.
    FBoolProperty* SpatiallyLoadedProp = CastField<FBoolProperty>(
        UHLODLayer::StaticClass()->FindPropertyByName(TEXT("bIsSpatiallyLoaded")));
    FIntProperty* CellSizeProp = CastField<FIntProperty>(
        UHLODLayer::StaticClass()->FindPropertyByName(TEXT("CellSize")));
    FDoubleProperty* LoadingRangeProp = CastField<FDoubleProperty>(
        UHLODLayer::StaticClass()->FindPropertyByName(TEXT("LoadingRange")));

    if (!SpatiallyLoadedProp || !CellSizeProp || !LoadingRangeProp)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            TEXT("UHLODLayer no longer exposes bIsSpatiallyLoaded / CellSize / LoadingRange; "
                 "the layer cannot be configured on this engine version"));
        return true;
    }

    // Security: Validate HLOD layer path
    FString SafePath = SanitizeProjectRelativePath(HlodLayerPath);
    if (SafePath.IsEmpty())
    {
        Ctx.SendError(TEXT("SECURITY_VIOLATION"),
            FString::Printf(TEXT("Invalid or unsafe HLOD layer path: %s"), *HlodLayerPath));
        return true;
    }
    HlodLayerPath = SafePath;

    // Build full path
    FString FullPath = HlodLayerPath / HlodLayerName;
    if (!IsValidMountPoint(FullPath))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P` - see the composition note at
        // level.structure.create_level above. Concatenation manufactures the "//" CreatePackage
        // logs Fatal on; FString::operator/ absorbs the duplicate separator.
        FullPath = FString(TEXT("/Game")) / FullPath;
    }

    // The prepend above no longer manufactures a "//", but the caller's own text can still carry
    // one. Validate the exact string that reaches CreatePackage.
    FString HlodPackagePathError;
    if (!PinWrightLevelStructureValidatePackagePath(FullPath, HlodPackagePathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), HlodPackagePathError);
        return true;
    }

    UPackage* AssetPackage = CreatePackage(*FullPath);
    if (!AssetPackage)
    {
        Ctx.SendError(TEXT("PACKAGE_CREATION_FAILED"),
            FString::Printf(TEXT("Failed to create package for HLOD layer at: %s"), *FullPath));
        return true;
    }

    UHLODLayer* NewHLODLayer = NewObject<UHLODLayer>(AssetPackage, *HlodLayerName, RF_Public | RF_Standalone);
    if (!NewHLODLayer)
    {
        Ctx.SendError(TEXT("ASSET_CREATION_FAILED"), TEXT("Failed to create UHLODLayer object"));
        return true;
    }

    // Configure the HLOD layer (reflection for the three setter-less / deprecated-setter
    // fields; SetLayerType is public and non-deprecated on every supported engine).
    SpatiallyLoadedProp->SetPropertyValue_InContainer(NewHLODLayer, bIsSpatiallyLoaded);
    CellSizeProp->SetPropertyValue_InContainer(NewHLODLayer, CellSize);
    LoadingRangeProp->SetPropertyValue_InContainer(NewHLODLayer, LoadingDistance);

    if (LayerType == TEXT("Instancing"))
    {
        NewHLODLayer->SetLayerType(EHLODLayerType::Instancing);
    }
    else if (LayerType == TEXT("MeshSimplify") || LayerType == TEXT("SimplifiedMesh"))
    {
        NewHLODLayer->SetLayerType(EHLODLayerType::MeshSimplify);
    }
    else if (LayerType == TEXT("MeshApproximate") || LayerType == TEXT("ApproximatedMesh"))
    {
        NewHLODLayer->SetLayerType(EHLODLayerType::MeshApproximate);
    }
    else // Default to MeshMerge
    {
        NewHLODLayer->SetLayerType(EHLODLayerType::MeshMerge);
    }

    AssetPackage->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewHLODLayer);
    McpSafeAssetSave(NewHLODLayer);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetStringField(TEXT("hlodLayerName"), HlodLayerName);
    ResponseJson->SetStringField(TEXT("hlodLayerPath"), FullPath);
    ResponseJson->SetBoolField(TEXT("isSpatiallyLoaded"), bIsSpatiallyLoaded);
    ResponseJson->SetNumberField(TEXT("cellSize"), CellSize);
    ResponseJson->SetNumberField(TEXT("loadingDistance"), LoadingDistance);
    ResponseJson->SetStringField(TEXT("layerType"), LayerType);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    ResponseJson->SetStringField(TEXT("note"),
        TEXT("UE 5.7+ deprecated the HLOD layer's spatially-loaded / cellSize / loadingDistance "
             "fields. They are written on the asset, but the runtime grid they used to drive is "
             "now configured in the world partition's settings (level.structure.configure_grid_size)."));
#endif

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.create_minimap_volume ----
REGISTER_RPC_HANDLER("level.structure.create_minimap_volume", "level.structure",
    "Spawn a World Partition minimap volume defining the area captured into the WP minimap texture. Dimensions are in world centimeters.",
    RPC_PARAMS(
        RPC_PARAM_DEF("volumeName", "string", "Name of the volume", "MinimapVolume"),
        RPC_PARAM_OPT("volumeLocation", "object", "Volume location {x,y,z}"),
        RPC_PARAM_OPT("volumeExtent", "object", "Volume extent {x,y,z}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString VolumeName = GetJsonStringField(Payload, TEXT("volumeName"), TEXT("MinimapVolume"));
    FVector VolumeLocation = GetVectorFromJsonLS(GetObjectFieldLS(Payload, TEXT("volumeLocation")));
    FVector VolumeExtent = GetVectorFromJsonLS(GetObjectFieldLS(Payload, TEXT("volumeExtent")), FVector(10000.0));

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    UWorldPartition* WorldPartition = World->GetWorldPartition();
    if (!WorldPartition)
    {
        Ctx.SendError(TEXT("WORLD_PARTITION_NOT_ENABLED"),
            TEXT("World Partition is not enabled. AWorldPartitionMiniMapVolume requires World Partition."));
        return true;
    }

    // Spawn the AWorldPartitionMiniMapVolume
    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = MakeUniqueObjectName(World, AWorldPartitionMiniMapVolume::StaticClass(), FName(*VolumeName));
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AWorldPartitionMiniMapVolume* MiniMapVolume = World->SpawnActor<AWorldPartitionMiniMapVolume>(
        AWorldPartitionMiniMapVolume::StaticClass(),
        VolumeLocation,
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (!MiniMapVolume)
    {
        Ctx.SendError(TEXT("ACTOR_SPAWN_FAILED"), TEXT("Failed to spawn AWorldPartitionMiniMapVolume actor"));
        return true;
    }

    MiniMapVolume->SetActorLabel(*VolumeName);

    FVector DesiredScale = VolumeExtent / 100.0;
    MiniMapVolume->SetActorScale3D(DesiredScale);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddActorVerification(ResponseJson, MiniMapVolume);
    ResponseJson->SetStringField(TEXT("volumeName"), VolumeName);
    ResponseJson->SetStringField(TEXT("volumeClass"), TEXT("AWorldPartitionMiniMapVolume"));

    TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject());
    LocationJson->SetNumberField(TEXT("x"), VolumeLocation.X);
    LocationJson->SetNumberField(TEXT("y"), VolumeLocation.Y);
    LocationJson->SetNumberField(TEXT("z"), VolumeLocation.Z);
    ResponseJson->SetObjectField(TEXT("volumeLocation"), LocationJson);

    TSharedPtr<FJsonObject> ExtentJson = MakeShareable(new FJsonObject());
    ExtentJson->SetNumberField(TEXT("x"), VolumeExtent.X);
    ExtentJson->SetNumberField(TEXT("y"), VolumeExtent.Y);
    ExtentJson->SetNumberField(TEXT("z"), VolumeExtent.Z);
    ResponseJson->SetObjectField(TEXT("volumeExtent"), ExtentJson);

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ============================================================================
// Level Blueprint Handlers (3 actions)
// ============================================================================

// ---- level.structure.open_level_blueprint ----
REGISTER_RPC_HANDLER("level.structure.open_level_blueprint", "level.structure",
    "Open the level Blueprint editor for the active level. Required prerequisite for some level-blueprint node-graph operations that need an editor instance.",
    RPC_NO_PARAMS)
{
    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    ULevel* PersistentLevel = World->PersistentLevel;
    if (!PersistentLevel)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("No persistent level available"));
        return true;
    }

    FString LevelPackageName = World->GetOutermost()->GetName();
    bool bIsSavedLevel = !LevelPackageName.IsEmpty() && !LevelPackageName.StartsWith(TEXT("/Temp/"));

    ULevelScriptBlueprint* LevelBP = PersistentLevel->GetLevelScriptBlueprint(true);
    if (!LevelBP)
    {
        if (!bIsSavedLevel)
        {
            Ctx.SendError(TEXT("OPERATION_FAILED"),
                TEXT("Level Blueprint unavailable for unsaved levels. Please save the level first."));
            return true;
        }
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Failed to get or create Level Blueprint"));
        return true;
    }

    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(LevelBP);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    // AddAssetVerification now surfaces the level-script BP's full object path
    // (…:PersistentLevel.<Map>) as assetPath — the handle that round-trips into the
    // blueprint.graph.* family this verb is the prerequisite for (a ULevelScriptBlueprint
    // is a sub-object of the .umap and cannot be LoadObject<UBlueprint>'d at the bare
    // package path; reusing that path hard-fails ASSET_NOT_FOUND). Surface that usable
    // value explicitly under blueprintObjectPath and the bare .umap package path under
    // mapPath, mirroring the actorPath/mapPath split AddActorVerification adopted for the
    // same anti-pattern. See E-open-level-blueprint-unusable-assetpath.
    AddAssetVerification(ResponseJson, LevelBP);
    // blueprintObjectPath is an explicit ergonomic alias of the assetPath the helper just
    // emitted (documented in level.structure.md) — read it back rather than independently
    // re-deriving GetPathName(), so the two response keys provably cannot drift if
    // ResolveVerificationAssetPath's rule ever changes.
    ResponseJson->SetStringField(TEXT("blueprintObjectPath"),
        ResponseJson->GetStringField(TEXT("assetPath")));
    if (UPackage* LevelBPPackage = LevelBP->GetPackage())
    {
        ResponseJson->SetStringField(TEXT("mapPath"), LevelBPPackage->GetPathName());
    }

    ResponseJson->SetStringField(TEXT("levelName"), World->GetMapName());

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.add_level_blueprint_node ----
REGISTER_RPC_HANDLER("level.structure.add_level_blueprint_node", "level.structure",
    "Add an UNBOUND stub node of the requested class to the level Blueprint's event graph (no event/function reference is bound). For real authoring of a bound node — e.g. a BeginPlay event or a Print String call — use blueprint.graph.create_node (with eventName / target) on the level Blueprint object instead. nodeName is applied as the node's comment label and echoed back under nodeName; the auto-generated node title comes back separately under nodeTitle.",
    RPC_PARAMS(
        RPC_PARAM_REQ("nodeClass", "classref", "Class of the node to add"),
        RPC_PARAM_OPT("nodeName", "string", "Comment label for the node; echoed back under nodeName"),
        RPC_PARAM_OPT("nodePosition", "object", "Node position {x,y}. Flat x/y params are also accepted as an alias."),
        RPC_PARAM_OPT("x", "number", "Node X position (flat alias for nodePosition.x, matching blueprint.graph.create_node)"),
        RPC_PARAM_OPT("y", "number", "Node Y position (flat alias for nodePosition.y, matching blueprint.graph.create_node)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString NodeClass = GetJsonStringField(Payload, TEXT("nodeClass"), TEXT(""));
    FString NodeName = GetJsonStringField(Payload, TEXT("nodeName"), TEXT(""));
    // Position accepts both the documented object shape {x,y} and the sibling
    // blueprint.graph.create_node flat-x/y spelling, so a flat arg no longer
    // silently zeroes to the origin (E-level-bp-node-verbs-cant-author-bound-nodes).
    TSharedPtr<FJsonObject> PositionJson = GetObjectFieldLS(Payload, TEXT("nodePosition"));
    const TSharedPtr<FJsonObject>& PosSource = PositionJson.IsValid() ? PositionJson : Payload;
    int32 PosX = static_cast<int32>(GetJsonNumberField(PosSource, TEXT("x")));
    int32 PosY = static_cast<int32>(GetJsonNumberField(PosSource, TEXT("y")));

    if (NodeClass.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("nodeClass is required"));
        return true;
    }

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    ULevel* CurrentLevel = World->GetCurrentLevel();
    if (!CurrentLevel)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("No current level available"));
        return true;
    }

    ULevelScriptBlueprint* LevelBP = CurrentLevel->GetLevelScriptBlueprint(true);
    if (!LevelBP)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Failed to get Level Blueprint"));
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(LevelBP);
    if (!EventGraph)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Failed to find event graph in Level Blueprint"));
        return true;
    }

    // Find the node class - try multiple lookup paths
    FString TriedPaths;
    UClass* NodeClassObj = FindObject<UClass>(nullptr, *NodeClass);
    TriedPaths = NodeClass;

    if (!NodeClassObj)
    {
        FString BlueprintGraphPath = TEXT("/Script/BlueprintGraph.") + NodeClass;
        NodeClassObj = FindObject<UClass>(nullptr, *BlueprintGraphPath);
        TriedPaths += TEXT(", ") + BlueprintGraphPath;
    }

    if (!NodeClassObj)
    {
        FString EnginePath = TEXT("/Script/Engine.") + NodeClass;
        NodeClassObj = FindObject<UClass>(nullptr, *EnginePath);
        TriedPaths += TEXT(", ") + EnginePath;
    }

    if (!NodeClassObj)
    {
        FString UnrealEdPath = TEXT("/Script/UnrealEd.") + NodeClass;
        NodeClassObj = FindObject<UClass>(nullptr, *UnrealEdPath);
        TriedPaths += TEXT(", ") + UnrealEdPath;
    }

    FString CreatedNodeName;
    if (NodeClassObj && NodeClassObj->IsChildOf(UK2Node::StaticClass()))
    {
        UK2Node* NewNode = NewObject<UK2Node>(EventGraph, NodeClassObj);
        if (NewNode)
        {
            NewNode->CreateNewGuid();
            NewNode->PostPlacedNewNode();
            NewNode->AllocateDefaultPins();
            NewNode->NodePosX = PosX;
            NewNode->NodePosY = PosY;
            // Honor the caller's nodeName instead of dropping it: apply it as the
            // node's comment label so it is not silently discarded
            // (E-level-bp-node-verbs-cant-author-bound-nodes).
            if (!NodeName.IsEmpty())
            {
                NewNode->NodeComment = NodeName;
                NewNode->bCommentBubbleVisible = true;
            }
            EventGraph->AddNode(NewNode, true, false);
            CreatedNodeName = NewNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        }
    }

    if (CreatedNodeName.IsEmpty())
    {
        FString ErrorMsg;
        if (!NodeClassObj)
        {
            ErrorMsg = FString::Printf(TEXT("Node class not found. Tried paths: [%s]"), *TriedPaths);
        }
        else if (!NodeClassObj->IsChildOf(UK2Node::StaticClass()))
        {
            ErrorMsg = FString::Printf(TEXT("Class '%s' found but is not a K2Node subclass"), *NodeClass);
        }
        else
        {
            ErrorMsg = FString::Printf(TEXT("Failed to create node instance of class: %s"), *NodeClass);
        }
        Ctx.SendError(TEXT("OPERATION_FAILED"), ErrorMsg);
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(LevelBP);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddAssetVerification(ResponseJson, LevelBP);
    ResponseJson->SetStringField(TEXT("nodeClass"), NodeClass);
    // nodeName echoes the caller's value (the comment label we applied), NOT the
    // auto-generated title — those are two distinct values and must not share one
    // key (E-level-bp-node-verbs-cant-author-bound-nodes). The auto title goes
    // under its own nodeTitle key.
    ResponseJson->SetStringField(TEXT("nodeName"), NodeName);
    ResponseJson->SetStringField(TEXT("nodeTitle"), CreatedNodeName);
    ResponseJson->SetNumberField(TEXT("posX"), PosX);
    ResponseJson->SetNumberField(TEXT("posY"), PosY);
    ResponseJson->SetBoolField(TEXT("nodeCreated"), true);
    // Steer callers toward the bound-node path: this verb only emits an unbound stub.
    // Also flag that nodeName is a display-only comment label, NOT a handle:
    // connect_level_blueprint_nodes matches on node title / object name, so it cannot
    // look a node up by the nodeName assigned here.
    ResponseJson->SetStringField(TEXT("note"),
        TEXT("This node is an unbound stub (no event/function reference). For a bound "
             "node use blueprint.graph.create_node (with eventName / target) on the level Blueprint. "
             "nodeName is a display-only comment label, not a node handle: "
             "connect_level_blueprint_nodes matches on node title / object name and cannot "
             "reference a node by the nodeName assigned here."));

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.connect_level_blueprint_nodes ----
REGISTER_RPC_HANDLER("level.structure.connect_level_blueprint_nodes", "level.structure",
    "Wire two nodes in the level Blueprint's event graph by name (and optionally pin name). For arbitrary graphs use blueprint.graph.connect_pins.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourceNodeName", "string", "Source node name"),
        RPC_PARAM_OPT("sourcePinName", "string", "Source pin name"),
        RPC_PARAM_REQ("targetNodeName", "string", "Target node name"),
        RPC_PARAM_OPT("targetPinName", "string", "Target pin name")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString SourceNodeName = GetJsonStringField(Payload, TEXT("sourceNodeName"), TEXT(""));
    FString SourcePinName = GetJsonStringField(Payload, TEXT("sourcePinName"), TEXT(""));
    FString TargetNodeName = GetJsonStringField(Payload, TEXT("targetNodeName"), TEXT(""));
    FString TargetPinName = GetJsonStringField(Payload, TEXT("targetPinName"), TEXT(""));

    if (SourceNodeName.IsEmpty() || TargetNodeName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sourceNodeName and targetNodeName are required"));
        return true;
    }

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    ULevel* CurrentLevel = World->GetCurrentLevel();
    ULevelScriptBlueprint* LevelBP = CurrentLevel ? CurrentLevel->GetLevelScriptBlueprint(false) : nullptr;
    if (!LevelBP)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Level Blueprint not available"));
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(LevelBP);
    if (!EventGraph)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Event graph not found"));
        return true;
    }

    // Find source and target nodes
    UEdGraphNode* SourceNode = nullptr;
    UEdGraphNode* TargetNode = nullptr;

    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        if (NodeTitle.Contains(SourceNodeName) || Node->GetName().Contains(SourceNodeName))
        {
            SourceNode = Node;
        }
        if (NodeTitle.Contains(TargetNodeName) || Node->GetName().Contains(TargetNodeName))
        {
            TargetNode = Node;
        }
    }

    if (!SourceNode || !TargetNode)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Could not find nodes: source='%s' target='%s'"),
                *SourceNodeName, *TargetNodeName));
        return true;
    }

    // Find pins and connect
    UEdGraphPin* SourcePin = nullptr;
    UEdGraphPin* TargetPin = nullptr;

    for (UEdGraphPin* Pin : SourceNode->Pins)
    {
        if (Pin->PinName.ToString() == SourcePinName || Pin->GetDisplayName().ToString() == SourcePinName)
        {
            SourcePin = Pin;
            break;
        }
    }

    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        if (Pin->PinName.ToString() == TargetPinName || Pin->GetDisplayName().ToString() == TargetPinName)
        {
            TargetPin = Pin;
            break;
        }
    }

    bool bConnected = false;
    if (SourcePin && TargetPin)
    {
        SourcePin->MakeLinkTo(TargetPin);
        bConnected = SourcePin->LinkedTo.Contains(TargetPin);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(LevelBP);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddAssetVerification(ResponseJson, LevelBP);
    ResponseJson->SetStringField(TEXT("sourceNode"), SourceNodeName);
    ResponseJson->SetStringField(TEXT("sourcePin"), SourcePinName);
    ResponseJson->SetStringField(TEXT("targetNode"), TargetNodeName);
    ResponseJson->SetStringField(TEXT("targetPin"), TargetPinName);
    ResponseJson->SetBoolField(TEXT("connected"), bConnected);

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ============================================================================
// Level Instances Handlers (2 actions)
// ============================================================================

// ---- level.structure.create_level_instance ----
REGISTER_RPC_HANDLER("level.structure.create_level_instance", "level.structure",
    "Spawn an ALevelInstance actor that embeds a level asset by reference at the given transform. Level instances support nested instancing without flattening; companion to packed level actors.",
    RPC_PARAMS(
        RPC_PARAM_DEF("levelInstanceName", "string", "Name of the level instance", "LevelInstance"),
        RPC_PARAM_REQ("levelAssetPath", "path", "Path to the level asset"),
        RPC_PARAM_OPT("instanceLocation", "object", "Instance location {x,y,z}"),
        RPC_PARAM_OPT("instanceRotation", "object", "Instance rotation {pitch,yaw,roll}"),
        RPC_PARAM_OPT("instanceScale", "object", "Instance scale {x,y,z}")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString LevelInstanceName = GetJsonStringField(Payload, TEXT("levelInstanceName"), TEXT("LevelInstance"));
    FString LevelAssetPath = GetJsonStringField(Payload, TEXT("levelAssetPath"), TEXT(""));
    FVector InstanceLocation = GetVectorFromJsonLS(GetObjectFieldLS(Payload, TEXT("instanceLocation")));
    FRotator InstanceRotation = GetRotatorFromJsonLS(GetObjectFieldLS(Payload, TEXT("instanceRotation")));
    FVector InstanceScale = GetVectorFromJsonLS(GetObjectFieldLS(Payload, TEXT("instanceScale")), FVector(1.0));

    if (LevelAssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("levelAssetPath is required"));
        return true;
    }

    // Validate that the level asset exists
    FString NormalizedLevelPath = LevelAssetPath;
    if (!IsValidMountPoint(NormalizedLevelPath))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P` - see the composition note at
        // level.structure.create_level above. levelAssetPath arrives raw off the wire here, so a
        // rooted-but-unmounted spelling reaches this branch and concatenation would put a "//"
        // into both the resolved package name and the LEVEL_NOT_FOUND message below.
        NormalizedLevelPath = FString(TEXT("/Game")) / NormalizedLevelPath;
    }
    NormalizedLevelPath.RemoveFromEnd(TEXT(".umap"));

    if (!FPackageName::DoesPackageExist(NormalizedLevelPath))
    {
        Ctx.SendError(TEXT("LEVEL_NOT_FOUND"),
            FString::Printf(TEXT("Level asset not found: %s"), *LevelAssetPath));
        return true;
    }

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    ULevelInstanceSubsystem* LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>();
    if (!LevelInstanceSubsystem)
    {
        Ctx.SendError(TEXT("SUBSYSTEM_NOT_AVAILABLE"), TEXT("Level Instance Subsystem not available"));
        return true;
    }

    // Spawn Level Instance Actor
    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = MakeUniqueObjectName(World, ALevelInstance::StaticClass(), FName(*LevelInstanceName));
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ALevelInstance* LevelInstanceActor = World->SpawnActor<ALevelInstance>(
        ALevelInstance::StaticClass(),
        InstanceLocation,
        InstanceRotation,
        SpawnParams
    );

    if (!LevelInstanceActor)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Failed to spawn Level Instance actor"));
        return true;
    }

    LevelInstanceActor->SetActorScale3D(InstanceScale);
    LevelInstanceActor->SetActorLabel(*LevelInstanceName);

    // A spawned ALevelInstance embeds nothing until its WorldAsset is assigned; SetWorldAsset
    // only stores the soft reference, UpdateLevelInstanceFromWorldAsset is what loads it.
    const FString WorldAssetObjectPath = FString::Printf(TEXT("%s.%s"),
        *NormalizedLevelPath, *FPackageName::GetShortName(NormalizedLevelPath));
    const FSoftObjectPath WorldAssetPath(WorldAssetObjectPath);
    const TSoftObjectPtr<UWorld> WorldAsset(WorldAssetPath);

    if (!LevelInstanceActor->SetWorldAsset(WorldAsset))
    {
        World->EditorDestroyActor(LevelInstanceActor, /*bShouldModifyLevel=*/false);
        Ctx.SendError(TEXT("OPERATION_FAILED"),
            FString::Printf(TEXT("Level asset rejected as a level instance: %s (circular reference, or the level was never saved)"),
                *LevelAssetPath));
        return true;
    }
    LevelInstanceActor->UpdateLevelInstanceFromWorldAsset();

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddActorVerification(ResponseJson, LevelInstanceActor);
    ResponseJson->SetStringField(TEXT("levelInstanceName"), LevelInstanceName);
    ResponseJson->SetStringField(TEXT("levelAssetPath"), LevelAssetPath);
    ResponseJson->SetStringField(TEXT("worldAsset"), WorldAssetObjectPath);

    TSharedPtr<FJsonObject> LocationJson = MakeShareable(new FJsonObject());
    LocationJson->SetNumberField(TEXT("x"), InstanceLocation.X);
    LocationJson->SetNumberField(TEXT("y"), InstanceLocation.Y);
    LocationJson->SetNumberField(TEXT("z"), InstanceLocation.Z);
    ResponseJson->SetObjectField(TEXT("location"), LocationJson);

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- level.structure.create_packed_level_actor ----
REGISTER_RPC_HANDLER("level.structure.create_packed_level_actor", "level.structure",
    "Spawn a Packed Level Actor — a Blueprint that bakes a level asset's contents into instanced static meshes for performance. Companion to create_level_instance (which keeps the contents live).",
    RPC_PARAMS(
        RPC_PARAM_DEF("packedLevelName", "string", "Name of the packed level actor", "PackedLevel"),
        RPC_PARAM_OPT("levelAssetPath", "path", "Path to the level asset"),
        RPC_PARAM_OPT("instanceLocation", "object", "Instance location {x,y,z}"),
        RPC_PARAM_OPT("instanceRotation", "object", "Instance rotation {pitch,yaw,roll}"),
        RPC_PARAM_DEF("bPackBlueprints", "boolean", "Pack blueprints", "true"),
        RPC_PARAM_DEF("bPackStaticMeshes", "boolean", "Pack static meshes", "true")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString PackedLevelName = GetJsonStringField(Payload, TEXT("packedLevelName"), TEXT("PackedLevel"));
    FString LevelAssetPath = GetJsonStringField(Payload, TEXT("levelAssetPath"), TEXT(""));
    FVector InstanceLocation = GetVectorFromJsonLS(GetObjectFieldLS(Payload, TEXT("instanceLocation")));
    FRotator InstanceRotation = GetRotatorFromJsonLS(GetObjectFieldLS(Payload, TEXT("instanceRotation")));
    bool bPackBlueprints = GetJsonBoolField(Payload, TEXT("bPackBlueprints"), true);
    bool bPackStaticMeshes = GetJsonBoolField(Payload, TEXT("bPackStaticMeshes"), true);

    // FPackedLevelActorBuilder::CreateDefaultBuilder() always installs both the recursive
    // (nested level instance) and ISM (static mesh) builders, and the builder classes are
    // engine-private, so no subset can be selected. Reject an opt-out rather than report it
    // as applied.
    if (!bPackBlueprints || !bPackStaticMeshes)
    {
        Ctx.SendError(TEXT("UNSUPPORTED_OPTION"),
            TEXT("bPackBlueprints / bPackStaticMeshes cannot be disabled: the engine's packed-level "
                 "builder always packs both nested level instances and static meshes. Omit them (or "
                 "pass true) to pack, or use level.structure.create_level_instance to keep the source "
                 "level unpacked."));
        return true;
    }

    // Validate levelAssetPath if provided
    FString NormalizedLevelPath;
    if (!LevelAssetPath.IsEmpty())
    {
        NormalizedLevelPath = LevelAssetPath;
        if (!IsValidMountPoint(NormalizedLevelPath))
        {
            // `TEXT("/Game") / P`, never `TEXT("/Game/") + P` - see the composition note at
            // level.structure.create_level above.
            NormalizedLevelPath = FString(TEXT("/Game")) / NormalizedLevelPath;
        }
        NormalizedLevelPath.RemoveFromEnd(TEXT(".umap"));

        if (!FPackageName::DoesPackageExist(NormalizedLevelPath))
        {
            Ctx.SendError(TEXT("LEVEL_NOT_FOUND"),
                FString::Printf(TEXT("Level asset not found: %s"), *LevelAssetPath));
            return true;
        }
    }

    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    // The packing builder block-loads the source level instance through the subsystem, and
    // cannot run while another level instance is open for edit.
    ULevelInstanceSubsystem* LevelInstanceSubsystem = nullptr;
    if (!NormalizedLevelPath.IsEmpty())
    {
        LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>();
        if (!LevelInstanceSubsystem)
        {
            Ctx.SendError(TEXT("SUBSYSTEM_NOT_AVAILABLE"), TEXT("Level Instance Subsystem not available"));
            return true;
        }
        if (LevelInstanceSubsystem->GetEditingLevelInstance())
        {
            Ctx.SendError(TEXT("OPERATION_FAILED"),
                TEXT("A level instance is currently open for edit; commit or discard it before packing a level."));
            return true;
        }
    }

    // Spawn Packed Level Actor
    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = MakeUniqueObjectName(World, APackedLevelActor::StaticClass(), FName(*PackedLevelName));
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    APackedLevelActor* PackedActor = World->SpawnActor<APackedLevelActor>(
        APackedLevelActor::StaticClass(),
        InstanceLocation,
        InstanceRotation,
        SpawnParams
    );

    if (!PackedActor)
    {
        Ctx.SendError(TEXT("OPERATION_FAILED"), TEXT("Failed to spawn Packed Level Actor"));
        return true;
    }

    PackedActor->SetActorLabel(*PackedLevelName);

    // A spawned APackedLevelActor holds no source level and no packed components until the
    // WorldAsset is assigned and FPackedLevelActorBuilder bakes that level into it.
    bool bPacked = false;
    int32 PackedComponentCount = 0;
    FString WorldAssetObjectPath;

    if (!NormalizedLevelPath.IsEmpty())
    {
        WorldAssetObjectPath = FString::Printf(TEXT("%s.%s"),
            *NormalizedLevelPath, *FPackageName::GetShortName(NormalizedLevelPath));
        const FSoftObjectPath WorldAssetPath(WorldAssetObjectPath);
        const TSoftObjectPtr<UWorld> WorldAsset(WorldAssetPath);

        if (!PackedActor->SetWorldAsset(WorldAsset))
        {
            World->EditorDestroyActor(PackedActor, /*bShouldModifyLevel=*/false);
            Ctx.SendError(TEXT("OPERATION_FAILED"),
                FString::Printf(TEXT("Level asset rejected as a packed level source: %s (circular reference, or the level was never saved)"),
                    *LevelAssetPath));
            return true;
        }

        // The source level must be streamed in as a level instance before it can be baked into
        // PackedActor. Do NOT use FPackedLevelActorBuilder::PackActor(Actor, WorldAsset): that
        // overload spawns its source level instance inside the builder's own FPreviewScene world,
        // yet FPackedLevelActorBuilder::PackActor(Context) resolves the loaded source level with
        // GetPackedLevelActor()->GetLevelInstanceSubsystem()->GetLevelInstanceLevel(...). Because
        // ULevelInstanceSubsystem is a UWorldSubsystem, that lookup asks the *editor* world's
        // subsystem for an instance registered with the *preview* world's subsystem, misses, and
        // packing always fails. Loading the source instance in the packed actor's own world is the
        // same-world arrangement the engine's blueprint-packing path relies on.
        FActorSpawnParameters SourceSpawnParams;
        SourceSpawnParams.bCreateActorPackage = false;
        SourceSpawnParams.bHideFromSceneOutliner = true;
        SourceSpawnParams.bNoFail = true;
        SourceSpawnParams.ObjectFlags |= RF_Transient;
        SourceSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        SourceSpawnParams.OverrideLevel = World->PersistentLevel;

        APackedLevelActor* SourceLevelInstance = World->SpawnActor<APackedLevelActor>(
            APackedLevelActor::StaticClass(),
            InstanceLocation,
            InstanceRotation,
            SourceSpawnParams
        );

        if (!SourceLevelInstance)
        {
            World->EditorDestroyActor(PackedActor, /*bShouldModifyLevel=*/false);
            Ctx.SendError(TEXT("OPERATION_FAILED"),
                TEXT("Failed to spawn the transient source level instance used for packing"));
            return true;
        }

        // An APackedLevelActor only streams its source level in while it is flagged for packing
        // (APackedLevelActor::IsLoadingEnabled -> ShouldLoadForPacking), and it disables partial
        // editor loading so the whole level is available to the builder.
        SourceLevelInstance->SetShouldLoadForPacking(true);
        SourceLevelInstance->SetWorldAsset(WorldAsset);
        LevelInstanceSubsystem->BlockLoadLevelInstance(SourceLevelInstance);

        TSharedPtr<FPackedLevelActorBuilder> Builder = FPackedLevelActorBuilder::CreateDefaultBuilder();
        bPacked = Builder.IsValid() && Builder->PackActor(PackedActor, SourceLevelInstance);

        // Unregistering the actor unloads the level instance it streamed in.
        World->EditorDestroyActor(SourceLevelInstance, /*bShouldModifyLevel=*/false);

        if (!bPacked)
        {
            World->EditorDestroyActor(PackedActor, /*bShouldModifyLevel=*/false);
            Ctx.SendError(TEXT("OPERATION_FAILED"),
                FString::Printf(TEXT("Failed to pack level '%s' into the packed level actor (see the PackedLevelActor message log)"),
                    *LevelAssetPath));
            return true;
        }

        TArray<UActorComponent*> PackedComponents;
        PackedActor->GetPackedComponents(PackedComponents);
        PackedComponentCount = PackedComponents.Num();
    }

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    AddActorVerification(ResponseJson, PackedActor);
    ResponseJson->SetStringField(TEXT("packedLevelName"), PackedLevelName);
    ResponseJson->SetStringField(TEXT("levelAssetPath"), LevelAssetPath);
    ResponseJson->SetStringField(TEXT("worldAsset"), WorldAssetObjectPath);
    ResponseJson->SetBoolField(TEXT("packed"), bPacked);
    ResponseJson->SetNumberField(TEXT("packedComponents"), PackedComponentCount);
    ResponseJson->SetBoolField(TEXT("packBlueprints"), bPackBlueprints);
    ResponseJson->SetBoolField(TEXT("packStaticMeshes"), bPackStaticMeshes);

    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ============================================================================
// Utility Handlers (1 action)
// ============================================================================

// ---- level.structure.get_level_structure_info ----
REGISTER_RPC_HANDLER("level.structure.get_level_structure_info", "level.structure",
    "Return a structural overview of the active world: persistent level, sublevels, World Partition state, and data layer summary. Read-only; useful before scripting WP edits.",
    RPC_NO_PARAMS)
{
    UWorld* World = GetEditorWorldLS();
    if (!World)
    {
        Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available"));
        return true;
    }

    TSharedPtr<FJsonObject> InfoJson = MakeShareable(new FJsonObject());
    InfoJson->SetStringField(TEXT("currentLevel"), World->GetMapName());

    // Get streaming levels
    TArray<TSharedPtr<FJsonValue>> SublevelsArray;
    const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
    InfoJson->SetNumberField(TEXT("sublevelCount"), StreamingLevels.Num());

    for (const ULevelStreaming* StreamingLevel : StreamingLevels)
    {
        if (StreamingLevel)
        {
            SublevelsArray.Add(MakeShareable(new FJsonValueString(StreamingLevel->GetWorldAssetPackageFName().ToString())));
        }
    }
    InfoJson->SetArrayField(TEXT("sublevels"), SublevelsArray);

    // Check World Partition
    UWorldPartition* WorldPartition = World->GetWorldPartition();
    InfoJson->SetBoolField(TEXT("worldPartitionEnabled"), WorldPartition != nullptr);

    if (WorldPartition)
    {
        // Enumerate the world's registered data layers. Use the editor subsystem's
        // GetAllDataLayers() — the same source assign_actor_to_data_layer resolves
        // against — so the readback reflects exactly the layers create_data_layer
        // registered. (The legacy UDataLayerSubsystem this branch used to fetch is
        // not the editor-time enumeration surface; with it the array stayed empty.)
        TArray<TSharedPtr<FJsonValue>> DataLayersArray;
        // Enumerate the world's registered data layers through the SAME editor
        // subsystem that create_data_layer / assign_actor_to_data_layer use to
        // register and resolve them (UDataLayerEditorSubsystem::Get()->GetAllDataLayers()),
        // not the runtime UDataLayerSubsystem the prior stub fetched and never read.
        // Each entry mirrors create_data_layer's response fields so a "create then
        // read back" verify step honestly reflects the layers that were made.
        if (UDataLayerEditorSubsystem* DataLayerEditorSubsystem = UDataLayerEditorSubsystem::Get())
        {
            const TArray<UDataLayerInstance*> AllDataLayers = DataLayerEditorSubsystem->GetAllDataLayers();
            for (const UDataLayerInstance* DataLayer : AllDataLayers)
            {
                if (!DataLayer)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> LayerJson = MakeShareable(new FJsonObject());
                LayerJson->SetStringField(TEXT("name"), DataLayer->GetDataLayerShortName());
                LayerJson->SetStringField(TEXT("fullName"), DataLayer->GetDataLayerFullName());

                // Surface the underlying UDataLayerAsset path so callers can resolve
                // the layer's asset (matches create_data_layer's dataLayerAssetPath).
                if (const UDataLayerAsset* DataLayerAsset = DataLayer->GetAsset())
                {
                    LayerJson->SetStringField(TEXT("assetPath"), DataLayerAsset->GetPathName());
                }

                // Report the Runtime/Editor type string create_data_layer accepts/echoes;
                // callers derive runtime-ness from type == "Runtime" (no separate isRuntime
                // field — it would just restate the same bit and could drift from type).
                const EDataLayerType LayerType = DataLayer->GetType();
                LayerJson->SetStringField(TEXT("type"),
                    LayerType == EDataLayerType::Runtime ? TEXT("Runtime")
                    : LayerType == EDataLayerType::Editor ? TEXT("Editor")
                    : TEXT("Unknown"));

                LayerJson->SetBoolField(TEXT("initiallyVisible"), DataLayer->IsInitiallyVisible());
                LayerJson->SetBoolField(TEXT("initiallyLoaded"), DataLayer->IsInitiallyLoadedInEditor());

                DataLayersArray.Add(MakeShareable(new FJsonValueObject(LayerJson)));
            }
        }
        InfoJson->SetArrayField(TEXT("dataLayers"), DataLayersArray);
    }

    // Get level instances
    TArray<TSharedPtr<FJsonValue>> LevelInstancesArray;
    for (TActorIterator<ALevelInstance> It(World); It; ++It)
    {
        FString ActorLabel = It->GetActorLabel();
        LevelInstancesArray.Add(MakeShareable(new FJsonValueString(ActorLabel)));
    }
    InfoJson->SetArrayField(TEXT("levelInstances"), LevelInstancesArray);

    // HLOD layers - enumerate from World Partition or legacy HLOD system
    TArray<TSharedPtr<FJsonValue>> HlodLayersArray;

    // Check for World Partition HLOD layers
    if (World->GetWorldPartition())
    {
        for (TObjectIterator<UHLODLayer> It; It; ++It)
        {
            UHLODLayer* Layer = *It;
            if (Layer && Layer->GetOuter() && Layer->GetOuter()->GetWorld() == World)
            {
                TSharedPtr<FJsonObject> LayerJson = MakeShared<FJsonObject>();
                LayerJson->SetStringField(TEXT("name"), Layer->GetName());
                LayerJson->SetStringField(TEXT("type"), TEXT("world_partition"));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
                PRAGMA_DISABLE_DEPRECATION_WARNINGS
#endif
                LayerJson->SetNumberField(TEXT("cellSize"), Layer->GetCellSize());
                LayerJson->SetNumberField(TEXT("loadingRange"), Layer->GetLoadingRange());
                LayerJson->SetBoolField(TEXT("isSpatiallyLoaded"), Layer->IsSpatiallyLoaded());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
                PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif

                FString LayerTypeStr;
                switch (Layer->GetLayerType())
                {
                    case EHLODLayerType::Instancing: LayerTypeStr = TEXT("Instancing"); break;
                    case EHLODLayerType::MeshMerge: LayerTypeStr = TEXT("MeshMerge"); break;
                    case EHLODLayerType::MeshSimplify: LayerTypeStr = TEXT("MeshSimplify"); break;
                    case EHLODLayerType::MeshApproximate: LayerTypeStr = TEXT("MeshApproximate"); break;
                    case EHLODLayerType::Custom: LayerTypeStr = TEXT("Custom"); break;
                    default: LayerTypeStr = TEXT("Unknown"); break;
                }
                LayerJson->SetStringField(TEXT("layerType"), LayerTypeStr);

                TSoftObjectPtr<UHLODLayer> ParentLayerSoft = Layer->GetParentLayer();
                if (ParentLayerSoft.IsValid())
                {
                    LayerJson->SetStringField(TEXT("parentLayer"), ParentLayerSoft->GetName());
                }

                HlodLayersArray.Add(MakeShareable(new FJsonValueObject(LayerJson)));
            }
        }
    }

    // Also check for World Partition HLOD actors in the world
    if (HlodLayersArray.Num() == 0 && World->GetWorldPartition())
    {
        TSet<FString> FoundLayers;
        for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
        {
            AWorldPartitionHLOD* HLODActor = *It;
            if (HLODActor)
            {
                FString LayerName = FString::Printf(TEXT("HLOD_Level_%d"), HLODActor->GetLODLevel());
                if (!FoundLayers.Contains(LayerName))
                {
                    FoundLayers.Add(LayerName);
                    TSharedPtr<FJsonObject> LayerJson = MakeShared<FJsonObject>();
                    LayerJson->SetStringField(TEXT("name"), LayerName);
                    LayerJson->SetStringField(TEXT("type"), TEXT("world_partition_hlod_actor"));
                    LayerJson->SetNumberField(TEXT("lodLevel"), HLODActor->GetLODLevel());
                    HlodLayersArray.Add(MakeShareable(new FJsonValueObject(LayerJson)));
                }
            }
        }
    }

    // Check for legacy HLOD system (ALODActor) for non-WP levels
    if (HlodLayersArray.Num() == 0)
    {
        TMap<int32, int32> LodLevelCounts;
        for (TActorIterator<ALODActor> It(World); It; ++It)
        {
            ALODActor* LODActor = *It;
            if (LODActor)
            {
                int32 Level = LODActor->LODLevel;
                LodLevelCounts.FindOrAdd(Level)++;
            }
        }

        for (const auto& Pair : LodLevelCounts)
        {
            TSharedPtr<FJsonObject> LayerJson = MakeShared<FJsonObject>();
            LayerJson->SetStringField(TEXT("name"), FString::Printf(TEXT("LOD_Level_%d"), Pair.Key));
            LayerJson->SetStringField(TEXT("type"), TEXT("legacy_hlod"));
            LayerJson->SetNumberField(TEXT("lodLevel"), Pair.Key);
            LayerJson->SetNumberField(TEXT("actorCount"), Pair.Value);
            HlodLayersArray.Add(MakeShareable(new FJsonValueObject(LayerJson)));
        }
    }

    InfoJson->SetArrayField(TEXT("hlodLayers"), HlodLayersArray);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
    ResponseJson->SetObjectField(TEXT("levelStructureInfo"), InfoJson);

    Ctx.SendSuccess(ResponseJson);
    return true;
}
