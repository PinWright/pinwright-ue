// Copyright (c) 2026 Alexander Penkin. MIT License.

using UnrealBuildTool;
using System.IO;

public class PinWrightCommonUI : ModuleRules
{
    public PinWrightCommonUI(ReadOnlyTargetRules Target) : base(Target)
    {
        // Mirrors the main PinWright module's PCH/unity setup.
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = true;

        string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);

        // Guard for engines that do not ship the CommonUI plugin at all (custom or stripped
        // engines): fail by name here instead of leaving the consumer with a raw C1083 on a
        // CommonUI header. This module's sources include those headers directly and ModuleRules
        // cannot drop its own .cpp files, so it cannot build empty. This does NOT cover the
        // plugin-present-but-reference-disabled case; that is the job of the "Enabled": true +
        // "Optional": true ref in PinWright.uplugin (plus the CI invariant check) and, at
        // runtime, of IntegrationGates only loading this module when IPluginManager reports
        // CommonUI enabled.
        string CommonUIDir = FindEnginePluginDir(EngineDir, "CommonUI", new string[]
        {
            Path.Combine("Plugins", "Runtime")  // UE 5.3-5.8
        });

        if (string.IsNullOrEmpty(CommonUIDir))
        {
            throw new BuildException(
                "PinWrightCommonUI requires the engine plugin 'CommonUI', which was not found under " +
                Path.Combine(EngineDir, "Plugins") + ". PinWrightCommonUI is an optional PinWright integration " +
                "sub-module whose sources include CommonUI headers directly, so it cannot be built against an " +
                "engine that does not ship that plugin. Restore the plugin, or remove the 'PinWrightCommonUI' " +
                "module entry from PinWright.uplugin and delete Source/PinWrightCommonUI to build PinWright " +
                "without the activatable-widget integration.");
        }

        // This module hard-imports CommonUI on purpose: it is LoadingPhase None and only
        // loaded by the main module when the CommonUI plugin is available, so the main
        // PinWright DLL carries no CommonUI import and loads cleanly on hosts without it.
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Json", "JsonUtilities",
            // Main module: exported handler machinery (FAutoRegisterHandler, FHandlerContext,
            // FResponseCapture) + PinWrightHelpers utilities (ResolveUClass).
            "PinWright",
            // Activatable-widget stack types the ui.activatable_* family drives.
            "CommonUI",
            "UMG", "Slate", "SlateCore",
            // FGameplayTag layer addressing in ActivatableLayerResolver.
            "GameplayTags",
            // Tests: UWidgetBlueprint authoring (UMGEditor) and UEditorAssetLibrary asset
            // cleanup (EditorScriptingUtilities) via the shared Tests/TestUtils.h helpers.
            "UMGEditor", "EditorScriptingUtilities"
        });

        // The moved sources keep their main-module include shape
        // ("Handlers/...", "Utils/...", "Tests/...").
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "PinWright", "Private"));
    }

    /// <summary>
    /// Resolves an optional engine plugin's root directory, or returns an empty string when the
    /// engine does not ship it. Reuses the main PinWright module's TryAddConditionalModule probe
    /// idiom (Path.GetFullPath(Target.RelativeEnginePath) plus Directory/File.Exists over explicit
    /// layouts, then a bounded recursive fallback): the verified UE 5.3-5.8 layouts are checked
    /// first, and the fallback keeps a custom engine that relocated the plugin from tripping the
    /// guard, since a false negative here would break a build that works today.
    /// </summary>
    /// <param name="EngineDir">Absolute path to the engine root directory.</param>
    /// <param name="PluginName">Plugin name, matching its .uplugin file stem.</param>
    /// <param name="RelativeParents">Engine-relative parent dirs the plugin has shipped under.</param>
    /// <returns>Absolute plugin directory, or an empty string if not found.</returns>
    private string FindEnginePluginDir(string EngineDir, string PluginName, string[] RelativeParents)
    {
        string PluginFile = PluginName + ".uplugin";

        foreach (string RelativeParent in RelativeParents)
        {
            string Candidate = Path.Combine(EngineDir, RelativeParent, PluginName);
            if (File.Exists(Path.Combine(Candidate, PluginFile)))
            {
                return Candidate;
            }
        }

        return SearchPluginDirBounded(Path.Combine(EngineDir, "Plugins"), PluginFile, 4);
    }

    /// <summary>
    /// Searches for the directory holding the given .uplugin file, up to a maximum depth.
    /// Mirrors SearchDirectoryBounded in the main module's Build.cs.
    /// </summary>
    /// <param name="RootDir">The root directory to start searching from.</param>
    /// <param name="PluginFile">The .uplugin file name to look for.</param>
    /// <param name="MaxDepth">Maximum depth to search (0 = direct children only).</param>
    /// <returns>Absolute directory containing the file, or an empty string if not found.</returns>
    private string SearchPluginDirBounded(string RootDir, string PluginFile, int MaxDepth)
    {
        if (MaxDepth < 0 || !Directory.Exists(RootDir)) return string.Empty;

        try
        {
            foreach (string SubDir in Directory.GetDirectories(RootDir))
            {
                if (File.Exists(Path.Combine(SubDir, PluginFile)))
                {
                    return SubDir;
                }

                string Found = SearchPluginDirBounded(SubDir, PluginFile, MaxDepth - 1);
                if (!string.IsNullOrEmpty(Found))
                {
                    return Found;
                }
            }
        }
        catch { /* Ignore access denied errors */ }
        return string.Empty;
    }
}
