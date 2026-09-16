// Copyright (c) 2026 Alexander Penkin. MIT License.

using UnrealBuildTool;
using System.IO;

public class PinWrightGeometry : ModuleRules
{
    public PinWrightGeometry(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = true;

        // Files moved out of the main module keep their original
        // #include "Handlers/..." / "Utils/..." / "Tests/..." paths; resolve them
        // against the main module's Private dir.
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "PinWright", "Private"));

        string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);

        // Guard for engines that do not ship the GeometryScripting plugin at all (custom or
        // stripped engines): fail by name here instead of leaving the consumer with a raw
        // C1083 on a GeometryScripting header. This module's sources include those headers
        // directly and ModuleRules cannot drop its own .cpp files, so it cannot build empty.
        // This does NOT cover the plugin-present-but-reference-disabled case; that is the job
        // of the "Enabled": true + "Optional": true ref in PinWright.uplugin (plus the CI
        // invariant check) and, at runtime, of IntegrationGates only loading this module when
        // IPluginManager reports GeometryScripting enabled.
        string GeometryScriptingDir = FindEnginePluginDir(EngineDir, "GeometryScripting", new string[]
        {
            Path.Combine("Plugins", "Runtime"),      // UE 5.4-5.8
            Path.Combine("Plugins", "Experimental")  // UE 5.3
        });

        if (string.IsNullOrEmpty(GeometryScriptingDir))
        {
            throw new BuildException(
                "PinWrightGeometry requires the engine plugin 'GeometryScripting', which was not found under " +
                Path.Combine(EngineDir, "Plugins") + ". PinWrightGeometry is an optional PinWright integration " +
                "sub-module whose sources include GeometryScripting headers directly, so it cannot be built " +
                "against an engine that does not ship that plugin. Restore the plugin, or remove the " +
                "'PinWrightGeometry' module entry from PinWright.uplugin and delete Source/PinWrightGeometry " +
                "to build PinWright without the geometry integration.");
        }

        // Same probe for MeshModelingToolset, which owns ModelingComponentsEditorOnly
        // (UE::AssetUtils::CreateStaticMeshAsset - the single-build StaticMesh creation path in
        // Handlers/Geometry/GeometryAssetCreate.cpp) and ModelingComponents
        // (UE::AssetUtils::GenerateNewMaterialSlotName). GeometryScripting.uplugin hard-depends
        // on MeshModelingToolset, so an engine that satisfies the guard above satisfies this one
        // too - but the path is probed rather than assumed, because a false negative here would
        // break a build that works today.
        string MeshModelingToolsetDir = FindEnginePluginDir(EngineDir, "MeshModelingToolset", new string[]
        {
            Path.Combine("Plugins", "Runtime")       // UE 5.3-5.8
        });

        if (string.IsNullOrEmpty(MeshModelingToolsetDir))
        {
            throw new BuildException(
                "PinWrightGeometry requires the engine plugin 'MeshModelingToolset', which was not found under " +
                Path.Combine(EngineDir, "Plugins") + ". It ships with the engine and is a hard dependency of " +
                "GeometryScripting, so an engine carrying GeometryScripting but not MeshModelingToolset is " +
                "already inconsistent. Restore the plugin to build the geometry integration.");
        }

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "Json", "JsonUtilities",
            // Main plugin module: exported handler registration/context and Utils helpers.
            "PinWright",
            // MeshRenderConsumerScan inspects Niagara systems and mesh renderer properties.
            "Niagara",
            // The GeometryScripting cluster is hard-linked on purpose: this module uses
            // LoadingPhase None and is only loaded by the main module (LoadModulePtr) when
            // the GeometryScripting plugin is enabled, so the main PinWright DLL no longer
            // hard-imports the optional geometry plugin DLLs.
            "GeometryScriptingCore", "GeometryScriptingEditor", "GeometryCore", "GeometryFramework",
            "DynamicMesh", "MeshDescription", "StaticMeshDescription",
            // SkeletalMeshDescription owns SkeletalMeshAttributes.h, which
            // GeometryScriptingCore's PUBLIC header GeometryScript/MeshBoneWeightFunctions.h
            // includes at line 9 (for FSkeletalMeshAttributes::DefaultSkinWeightProfileName,
            // the default of FGeometryScriptBoneWeightProfile::ProfileName) - yet
            // GeometryScriptingCore declares SkeletalMeshDescription only as a PRIVATE
            // dependency (GeometryScriptingCore.Build.cs:45), so that include path is not
            // propagated to us by depending on GeometryScriptingCore. It resolves today only
            // because Engine happens to carry SkeletalMeshDescription in its
            // PublicDependencyModuleNames (Engine.Build.cs:79 block, :107) - exactly the same
            // unstated transitive path MeshDescription/StaticMeshDescription above are listed
            // explicitly to avoid relying on. Listed here for the same reason:
            // SkeletalMeshAssetIOHandler.cpp includes MeshBoneWeightFunctions.h directly.
            "SkeletalMeshDescription",
            // MeshModelingToolset cluster, probed above. ModelingComponentsEditorOnly owns
            // UE::AssetUtils::CreateStaticMeshAsset, the only StaticMesh creation entry point
            // that accepts materials/collision/lightmap BEFORE the first build; the
            // GeometryScripting wrapper over it hides those options and builds twice.
            // ModelingComponents owns UE::AssetUtils::GenerateNewMaterialSlotName.
            "ModelingComponentsEditorOnly", "ModelingComponents",
            "AssetRegistry",            // FAssetRegistryModule / FAssetData in geometry tests
            "PhysicsCore",              // UBodySetup shape elems (FKAggregateGeom) via CollisionHelpers.h
            "EditorScriptingUtilities", // UEditorAssetLibrary via Tests/TestUtils.h
            // FlushRenderingCommands, called by Utils/MeshRebuildRenderGuard.h either side of
            // the render-state teardown it does around a static-mesh rebuild. The main PinWright
            // module already links RenderCore; this module did not, because nothing in it touched
            // the render thread until that guard.
            "RenderCore"
        });
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
