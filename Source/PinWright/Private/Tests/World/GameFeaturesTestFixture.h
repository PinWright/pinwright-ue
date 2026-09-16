// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test-only fixture helpers for board ticket F-game-features-live-fixture.
//
// PinWright's game_features.* surface can only be regression-tested against a LIVE,
// registered Game Feature plugin, which no fuzz host ships. This header GENERATES a
// disposable synthetic GF plugin entirely at test time: it writes a minimal .uplugin
// (CanContainContent=true, ExplicitlyLoaded=true) under the project's
// Plugins/GameFeatures/ folder (so UGameFeaturesSubsystemSettings::IsValidGameFeaturePlugin
// accepts the descriptor path) and saves a real UGameFeatureData asset carrying one
// UGameFeatureAction at the conventional package path /<Plugin>/GameFeatureData.
//
// The on-disk UGameFeatureData is what lets the plugin reach Active AND expose a
// NON-EMPTY UGameFeatureData::GetActions(): a CanContainContent=false plugin only gets
// a transient EMPTY GameFeatureData procedurally created by the state machine
// (GameFeaturePluginStateMachine.cpp:3296-3302), and a content plugin missing that asset
// errors Registering (:3456) — neither can satisfy the actions readback the deferred
// game_features.get_actions verb needs.
//
// Nothing is committed: the plugin folder is created + deleted within the test, so the
// fuzz loop's per-iteration git clean never sees it. Uses only in-code content and the
// engine GameFeatures module — no Lyra, no committed .uasset, no stubbed handler.
//
// The synchronous create/delete helpers live in this named-namespace header so the
// deferred F-game-features-set-state-and-actions verb tests can reuse them; the async
// Registered->Active drive + assertions live in TestGameFeaturesFixture.cpp.

#pragma once

// Compat/EngineVersionCompat.h (not the raw engine header): UE 5.3's
// Misc/EngineVersionComparison.h defines only UE_VERSION_NEWER_THAN / _OLDER_THAN, so the
// _OR_EQUAL spelling used below is undefined there and the #if below mis-parses.
#include "Compat/EngineVersionCompat.h"

// Mirror the production handler's gate (Handlers/Systems/GameFeaturesHandler.cpp): the
// FGameFeatureInfo enumerator + this fixture's APIs exist only on 5.4+ with the module linked.
#if __has_include("GameFeaturesSubsystem.h") && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "GameFeaturesSubsystem.h"
#include "GameFeatureData.h"
#include "GameFeatureAction.h"

#define MCP_TEST_HAS_GAMEFEATURES 1

namespace PinWrightGF
{

// Identity + on-disk locations of a generated synthetic GF plugin.
struct FSyntheticGFPlugin
{
    FString PluginName;    // PinWrightGFFix_<guid>
    FString PluginDir;     // <ProjectPlugins>/GameFeatures/<PluginName>
    FString UPluginFile;   // <PluginDir>/<PluginName>.uplugin
    FString ContentDir;    // <PluginDir>/Content
    FString MountRoot;     // /<PluginName>/
    FString PluginURL;     // file:<relative .uplugin path>
    bool bValid = false;
};

// Constructs an engine UObject by class path so the test TU never needs the class's
// (MinimalAPI) constructor exported — NewObject<T> on a MinimalAPI engine UCLASS
// link-errors cross-module; NewObject<UObject>(Outer, Class) resolved by reflection does not.
inline UObject* NewObjectByClassPath(UObject* Outer, const TCHAR* ClassPath, FName Name = NAME_None,
    EObjectFlags Flags = RF_NoFlags)
{
    UClass* Cls = FindObject<UClass>(nullptr, ClassPath);
    if (!Cls)
    {
        return nullptr;
    }
    return NewObject<UObject>(Outer, Cls, Name, Flags);
}

// Temporarily disables the editor's validate-on-save, reached via the
// UDataValidationSettings CDO by REFLECTION so PinWright takes no hard dependency on the
// DataValidation module. Without this, SavePackage of a GameFeatureData schedules the
// deferred asset-validation pass whose EditorValidator_Localization fires a Handled
// ensure on GameFeatureData assets (no native validation result) — which the automation
// framework counts as a test failure. The save-hook checks the flag synchronously, so
// bracketing SavePackage suppresses the whole chain; the prior value is restored on exit.
struct FScopedDisableValidateOnSaveReflect
{
    FBoolProperty* Prop = nullptr;
    UObject* CDO = nullptr;
    bool bPrev = false;

    FScopedDisableValidateOnSaveReflect()
    {
        if (UClass* Cls = FindObject<UClass>(nullptr, TEXT("/Script/DataValidation.DataValidationSettings")))
        {
            CDO = Cls->GetDefaultObject();
            Prop = FindFProperty<FBoolProperty>(Cls, TEXT("bValidateOnSave"));
            if (Prop && CDO)
            {
                bPrev = Prop->GetPropertyValue_InContainer(CDO);
                Prop->SetPropertyValue_InContainer(CDO, false);
            }
        }
    }

    ~FScopedDisableValidateOnSaveReflect()
    {
        if (Prop && CDO)
        {
            Prop->SetPropertyValue_InContainer(CDO, bPrev);
        }
    }
};

// Writes a disposable synthetic GF plugin (uplugin + a UGameFeatureData asset carrying
// one action) to disk. Returns false + a reason on failure. Caller must call
// DeleteOnDiskGFPlugin() during teardown. The content mount point is registered ONLY for
// the SavePackage window, then unregistered so the GameFeatures subsystem mounts the
// plugin cleanly itself during its own Mounting state (avoids a double-mount verify()).
inline bool CreateOnDiskGFPluginWithAction(FSyntheticGFPlugin& Out, FString& OutError)
{
    Out = FSyntheticGFPlugin();
    Out.PluginName = FString::Printf(TEXT("PinWrightGFFix_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Out.PluginDir = FPaths::ProjectPluginsDir() / TEXT("GameFeatures") / Out.PluginName;
    Out.UPluginFile = Out.PluginDir / (Out.PluginName + TEXT(".uplugin"));
    Out.ContentDir = Out.PluginDir / TEXT("Content");
    Out.MountRoot = FString::Printf(TEXT("/%s/"), *Out.PluginName);

    IFileManager& FM = IFileManager::Get();
    if (FM.DirectoryExists(*Out.PluginDir))
    {
        FM.DeleteDirectory(*Out.PluginDir, /*RequireExists*/ false, /*Tree*/ true);
    }
    if (!FM.MakeDirectory(*Out.ContentDir, /*Tree*/ true))
    {
        OutError = FString::Printf(TEXT("failed to create content dir %s"), *Out.ContentDir);
        return false;
    }

    // Minimal content-only GF plugin descriptor. CanContainContent so it carries a
    // UGameFeatureData; ExplicitlyLoaded so the subsystem (not startup) drives its lifecycle.
    const FString Descriptor = FString::Printf(TEXT(
        "{\n"
        "\t\"FileVersion\": 3,\n"
        "\t\"Version\": 1,\n"
        "\t\"VersionName\": \"1.0\",\n"
        "\t\"FriendlyName\": \"%s\",\n"
        "\t\"Description\": \"PinWright synthetic GF fixture (generated + deleted at test time)\",\n"
        "\t\"Category\": \"PinWrightTesting\",\n"
        "\t\"CreatedBy\": \"PinWright automation\",\n"
        "\t\"EnabledByDefault\": false,\n"
        "\t\"CanContainContent\": true,\n"
        "\t\"IsBetaVersion\": false,\n"
        "\t\"IsExperimentalVersion\": false,\n"
        "\t\"Installed\": false,\n"
        "\t\"ExplicitlyLoaded\": true,\n"
        "\t\"BuiltInInitialFeatureState\": \"Installed\"\n"
        "}\n"), *Out.PluginName);
    if (!FFileHelper::SaveStringToFile(Descriptor, *Out.UPluginFile))
    {
        OutError = FString::Printf(TEXT("failed to write %s"), *Out.UPluginFile);
        return false;
    }

    FString AbsContentDir = FPaths::ConvertRelativePathToFull(Out.ContentDir);
    if (!AbsContentDir.EndsWith(TEXT("/")))
    {
        AbsContentDir += TEXT("/");
    }
    FPackageName::RegisterMountPoint(Out.MountRoot, AbsContentDir);

    bool bSaved = false;
    {
        const FString PackageName = Out.MountRoot + TEXT("GameFeatureData"); // /<Plugin>/GameFeatureData
        if (UPackage* Package = CreatePackage(*PackageName))
        {
            // RF_Public but NOT RF_Standalone on the package and the asset. The plugin's
            // own content mount is unmounted during teardown, and the plugin-manager leak
            // check fires a Handled ensure on any package under that mount that survives GC.
            // RF_Standalone is a GC keep-flag, so a standalone package/asset would linger and
            // trip that ensure; RF_Public alone lets GC reclaim this scaffolding at unmount.
            // While the plugin is active the subsystem holds a hard ref to the loaded
            // GameFeatureData, so it is not collected prematurely; the on-disk .uasset is the
            // source of truth (a fresh subsystem load is equally valid).
            UObject* GFDObj = NewObjectByClassPath(Package, TEXT("/Script/GameFeatures.GameFeatureData"),
                FName(TEXT("GameFeatureData")), RF_Public);
            if (UGameFeatureData* GFD = Cast<UGameFeatureData>(GFDObj))
            {
                UObject* ActionObj = NewObjectByClassPath(GFD,
                    TEXT("/Script/GameFeatures.GameFeatureAction_AddCheats"));
                if (UGameFeatureAction* Action = Cast<UGameFeatureAction>(ActionObj))
                {
#if WITH_EDITOR
                    GFD->GetMutableActionsInEditor().Add(Action);
#else
                    OutError = TEXT("WITH_EDITOR is required to populate GameFeatureData actions");
#endif
                }
                else
                {
                    OutError = TEXT("could not construct UGameFeatureAction_AddCheats by reflection");
                }

                // Only persist a GameFeatureData that actually carries its action: saving an
                // actionless asset would let this "WithAction" helper return success with an
                // empty GetActions() array, surfacing downstream as a misleading empty-actions
                // assertion instead of the real construction failure recorded in OutError.
                if (OutError.IsEmpty())
                {
                    const FString Filename = FPackageName::LongPackageNameToFilename(
                        PackageName, FPackageName::GetAssetPackageExtension());

                    FSavePackageArgs Args;
                    Args.TopLevelFlags = RF_Public;
                    Args.SaveFlags = SAVE_NoError;
                    {
                        FScopedDisableValidateOnSaveReflect DisableValidation;
                        bSaved = UPackage::SavePackage(Package, GFD, *Filename, Args);
                    }
                    if (!bSaved && OutError.IsEmpty())
                    {
                        OutError = FString::Printf(TEXT("SavePackage failed for %s"), *Filename);
                    }
                }
            }
            else if (OutError.IsEmpty())
            {
                OutError = TEXT("could not construct UGameFeatureData by reflection");
            }
        }
        else
        {
            OutError = FString::Printf(TEXT("CreatePackage failed for %s"), *PackageName);
        }
    }

    FPackageName::UnRegisterMountPoint(Out.MountRoot, AbsContentDir);

    if (!bSaved)
    {
        return false;
    }

    Out.PluginURL = UGameFeaturesSubsystem::GetPluginURL_FileProtocol(
        IFileManager::Get().ConvertToRelativePath(*Out.UPluginFile));
    Out.bValid = true;
    return true;
}

// Deletes the on-disk plugin folder generated by CreateOnDiskGFPluginWithAction. Call
// only AFTER the plugin has been terminated through the subsystem (so its content mount
// + plugin-manager registration are already released).
inline void DeleteOnDiskGFPlugin(const FSyntheticGFPlugin& P)
{
    if (!P.PluginDir.IsEmpty() && IFileManager::Get().DirectoryExists(*P.PluginDir))
    {
        IFileManager::Get().DeleteDirectory(*P.PluginDir, /*RequireExists*/ false, /*Tree*/ true);
    }
}

} // namespace PinWrightGF

#else
#define MCP_TEST_HAS_GAMEFEATURES 0
#endif
