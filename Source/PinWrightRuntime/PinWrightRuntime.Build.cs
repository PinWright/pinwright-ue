// Copyright (c) 2026 Alexander Penkin. MIT License.

using UnrealBuildTool;

// Transport, dispatch, handler registry and game-safe handlers (docs/runtime-module.md).
// Loads in editor, -game and packaged Development processes; denied for Shipping and Test in the
// .uplugin (TraceAnalysis / TraceServices are Developer modules, absent without developer tools).
public class PinWrightRuntime : ModuleRules
{
    public PinWrightRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = true;
        // Dispatch/RpcDispatcher.cpp wraps every handler in try/catch (INTERNAL_ERROR reply instead
        // of a crash). Editor targets get /EHsc globally (UEBuildTarget: bCompileAgainstEditor); game targets do not.
        bEnableExceptions = true;

        // Handlers/HandlerRegistration.h: verbs registered here are marked runtime in the registry/wiki.
        PrivateDefinitions.Add("PINWRIGHT_REGISTERING_RUNTIME_MODULE=1");

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Json", "JsonUtilities", "DeveloperSettings",
            "Slate", "SlateCore", "InputCore", "UMG"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "ApplicationCore", "Projects", "Sockets", "Networking", "RenderCore", "RHI",
            "ImageCore", "ImageWrapper", "TraceLog", "TraceAnalysis", "TraceServices",
            "PhysicsCore", "Landscape", "AIModule", "GameplayTags",
            // Handlers/VFX effect.* (runtime Niagara API); the .uplugin enables Niagara for Game targets too.
            "Niagara",
            // Utils/AssetResolveUtils, Utils/ClassUtils (FAssetRegistryModule); Engine re-exports it only provisionally.
            "AssetRegistry",
            // Journal / drive.events_since in game processes too.
            "PinWrightRecorder"
        });

        // Editor branches (WITH_EDITOR) of the world accessor and of moved handlers and utils.
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "UnrealEd", "EditorSubsystem", "EditorScriptingUtilities", "WebBrowserWidget", "WebBrowser",
                "UMGEditor", "BlueprintGraph",
                // Settings/LevelEditorPlaySettings.h (editor.pie_status emulation read-back) includes ToolMenuContext.h.
                "ToolMenus",
                // Handlers/Volume/VolumeBrushGeometry.h (FBSPOps, editor-only brush builds in actor.spawn).
                "BSPUtils"
            });

            // GameFeatures is a plugin the .uplugin enables for editor targets only; game_features.*
            // __has_include-guards its headers, so a game build compiles that family out.
            if (System.IO.Directory.Exists(System.IO.Path.Combine(
                    System.IO.Path.GetFullPath(Target.RelativeEnginePath), "Plugins", "Runtime", "GameFeatures")))
            {
                PrivateDependencyModuleNames.Add("GameFeatures");
            }

            // Utils/PropertyExport: FInstancedStruct is the StructUtils plugin module on 5.4 and earlier
            // (CoreUObject from 5.5), mirroring PinWright.Build.cs. Game builds there compile it out
            // through Compat/InstancedStructCompat.h.
            if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 4)
            {
                PrivateDependencyModuleNames.Add("StructUtils");
            }
        }

        // OpenSSL RAND_bytes backs the gateway and instance auth tokens; CreateGuid is not a CSPRNG everywhere.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

        // ConvertStringSecurityDescriptorToSecurityDescriptorW / SetFileSecurityW (instance file DACL).
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("advapi32.lib");
        }
    }
}
