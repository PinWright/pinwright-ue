// Copyright (c) 2026 Alexander Penkin. MIT License.

using UnrealBuildTool;
using System.IO;

public class PinWrightChooser : ModuleRules
{
    public PinWrightChooser(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = true;

        // Files moved out of the main module keep their original
        // #include "Handlers/..." / "Utils/..." / "Tests/..." paths; resolve them
        // against the main module's Private dir.
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "PinWright", "Private"));

        string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);

        // Guard for engines that do not ship the Chooser plugin at all (custom or stripped
        // engines): fail by name here instead of leaving the consumer with a raw C1083 on a
        // Chooser header. This module's sources include those headers directly and ModuleRules
        // cannot drop its own .cpp files, so it cannot build empty. This does NOT cover the
        // plugin-present-but-reference-disabled case; that is the job of the "Enabled": true +
        // "Optional": true ref in PinWright.uplugin (plus the CI invariant check) and, at
        // runtime, of IntegrationGates only loading this module when IPluginManager reports
        // Chooser enabled.
        string ChooserDir = FindEnginePluginDir(EngineDir, "Chooser", new string[]
        {
            "Plugins",                               // UE 5.5-5.8
            Path.Combine("Plugins", "Experimental")  // UE 5.3-5.4
        });

        if (string.IsNullOrEmpty(ChooserDir))
        {
            throw new BuildException(
                "PinWrightChooser requires the engine plugin 'Chooser', which was not found under " +
                Path.Combine(EngineDir, "Plugins") + ". PinWrightChooser is an optional PinWright integration " +
                "sub-module whose sources include Chooser headers directly, so it cannot be built against an " +
                "engine that does not ship that plugin. Restore the plugin, or remove the 'PinWrightChooser' " +
                "module entry from PinWright.uplugin and delete Source/PinWrightChooser to build PinWright " +
                "without the chooser integration.");
        }

        // The Chooser plugin keeps FChooserPropertyBinding / column structs in its Internal
        // header dir, which sits at the same place under the plugin root on every layout.
        {
            string ChooserInternal = Path.Combine(ChooserDir, "Source", "Chooser", "Internal");
            if (Directory.Exists(ChooserInternal))
            {
                PrivateIncludePaths.Add(ChooserInternal);
            }
        }

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Json", "JsonUtilities",
            // Main plugin module: exported handler registration/context and Utils helpers.
            "PinWright",
            // Chooser is hard-linked on purpose: this module uses LoadingPhase None and is only
            // loaded by the main module (LoadModulePtr) when the Chooser plugin is enabled, so the
            // main PinWright DLL no longer hard-imports the optional Chooser plugin DLL.
            "Chooser",
            "AssetRegistry",            // FAssetRegistryModule::AssetCreated in ChooserAuthoringHandler.cpp
            "EditorScriptingUtilities"  // UEditorAssetLibrary via Tests/TestUtils.h
        });

        // FInstancedStruct (chooser column/result storage) lives in the standalone
        // StructUtils plugin module on UE 5.4 and earlier; from UE 5.5 it merged into
        // CoreUObject, so the separate module must NOT be linked there.
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 4)
        {
            PrivateDependencyModuleNames.Add("StructUtils");
        }
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
