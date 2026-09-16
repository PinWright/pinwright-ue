// Copyright (c) 2026 Alexander Penkin. MIT License.

using UnrealBuildTool;
using EpicGames.Core;
using System;
using System.IO;

public class PinWright : ModuleRules
{
    public PinWright(ReadOnlyTargetRules Target) : base(Target)
    {
        // PCH + unity. Shared anonymous-namespace helpers consolidated to per-cluster
        // headers (AGIRCompilerHelpers, NiagaraJsonHelpers, JsonBuilders, MGIRHelpers,
        // BlueprintEnumHelpers, WidgetInspectHelpers, MeshBoundsHelpers, WidgetFinders)
        // to dodge ODR collisions when unity merges TUs.
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = true;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "CinematicCamera", "Engine", "Json", "JsonUtilities",
            "LevelSequence", "MovieScene", "MovieSceneTracks", "MovieSceneTextTrack", "GameplayTags",
            "AIModule",   // UEnvQueryTest_Distance and other EQS classes
            "Landscape",  // FGrassVariety and other landscape classes
            "LevelSequenceEditor", "Sequencer", "MovieSceneTools", "Niagara", "NiagaraCore", "NiagaraEditor", "UnrealEd",
            "WorldPartitionEditor", "DataLayerEditor", "EnhancedInput",
            "BehaviorTreeEditor",  // UBehaviorTreeGraphNode classes
            "MaterialEditor"       // UMaterialExpressionRotator and other material expressions
        });

        // Header-only dependency — no linker dep, just include path for IPythonScriptPlugin.h
        PrivateIncludePathModuleNames.Add("PythonScriptPlugin");

        string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);

        // UE 5.6 moved NiagaraNodeStaticSwitch.h from Public/ to Private/. The class is
        // UCLASS(MinimalAPI) so symbols are exported, but the header now lives in the
        // module's private dir. Add an explicit private include path so handlers that
        // need the full type definition can include it via "NiagaraNodeStaticSwitch.h".
        {
            string NiagaraEditorPrivate = Path.Combine(EngineDir, "Plugins", "FX", "Niagara", "Source", "NiagaraEditor", "Private");
            if (Directory.Exists(NiagaraEditorPrivate))
            {
                PrivateIncludePaths.Add(NiagaraEditorPrivate);
            }
        }

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "ApplicationCore", "Slate", "SlateCore", "Projects", "InputCore", "DeveloperSettings", "Settings", "EngineSettings", "ContentBrowser", "Sockets", "Networking",
            "EditorSubsystem", "EditorScriptingUtilities", "BlueprintGraph", "HTTP",
            "BSPUtils",  // FBSPOps::csgPrepMovingBrush — builds brush collision model for spawned volumes
            "Kismet", "KismetCompiler", "GraphEditor", "AssetRegistry", "AssetTools", "SourceControl", "BlueprintEditorLibrary", "Blutility", "ContentBrowserData",
            "AudioEditor", "DataValidation", "NiagaraEditor",
            // SequencerScriptingEditor: USequencerToolsFunctionLibrary FBX round-trip
            // (ExportLevelSequenceFBX / ImportLevelSequenceFBX) backing sequencer.export_fbx /
            // sequencer.import_fbx. MinimalAPI class with UE_API-exported statics -> links directly;
            // the FBX API signatures are stable across UE 5.3-5.7 so no version gate is needed.
            "SequencerScriptingEditor",
            "GameplayAbilities",  // UAttributeSet, UGameplayEffect, UGameplayAbility
            "GameplayTagsEditor",  // IGameplayTagsEditorModule for project gameplay tag registry mutation
            "AudioMixer",         // FAudioEQEffect::ClampValues
            // SignalProcessing: Audio::ArrayMixIn (DSP/FloatArrayMath.h) plus the DSP
            // generator/analysis primitives behind the AudioGen layer in Private/AudioGen.
            // A plain engine Runtime module (Engine/Source/Runtime/SignalProcessing), present
            // on every host, so it is a hard dep rather than a TryAddConditionalModule probe.
            "SignalProcessing",
            "XmlParser",          // FXmlFile in widget XML import/export
            "LandscapeEditor", "LandscapeEditorUtilities", "Foliage", "FoliageEdit",
            "AnimGraph", "AnimGraphRuntime", "AnimationBlueprintLibrary", "AnimationCore", "Persona", "ToolMenus", "EditorWidgets", "PropertyEditor", "LevelEditor",
            // AdvancedPreviewScene: FAdvancedPreviewScene, FPreviewSceneProfile and
            // UAssetViewerSettings, for the scoped `previewScene` capture rig
            // (Handlers/Render/PreviewSceneRig.*). NOT reachable transitively -- UnrealEd.Build.cs
            // does not list it -- and the `key`/`sky` half of the rig would not need it at all
            // (FPreviewScene is Runtime/Engine, already a public dep). What it buys is the profile
            // SNAPSHOT: the shared UAssetViewerSettings::Profiles array is what an asset-editor
            // capture leaks through, and restoring it is the one mechanism that disarms both the
            // Niagara floor write (SNiagaraSystemViewport.cpp:872) and the committed-config write
            // that follows it (SAdvancedPreviewDetailsTab.cpp:46).
            "AdvancedPreviewScene",
            "MainFrame", "WorkspaceMenuStructure",  // setup screen: startup tab + Tools menu group
            "ControlRig", "ControlRigDeveloper", "ControlRigEditor", "RigVM", "RigVMDeveloper", "UMG", "UMGEditor", "ProceduralMeshComponent",
            "EnvironmentQueryEditor", "RenderCore", "RHI", "AutomationController", "GameplayDebugger", "TraceLog", "TraceAnalysis", "TraceServices", "AIGraph",
            // ImageCore: FImage/ERawImageFormat. ImageWrapper: FImageUtils::LoadImage PNG decode
            // (render-capture regression test reads a captured PNG back to assert it is non-blank).
            "ImageCore", "ImageWrapper",
            // PhysicsUtilities: FPhysicsAssetUtils::CreateFromSkeletalMesh + FPhysAssetCreateParams.
            // skeleton.create_physics_asset / physics.setup_physics_simulation drive this
            // non-interactive PhysicsAsset generator directly, bypassing UPhysicsAssetFactory's
            // modal body-generation dialog that wedges the headless game thread
            // (B-physics-asset-factory-modal-hang).
            "MeshUtilities", "MaterialUtilities", "PhysicsCore", "PhysicsUtilities", "ClothingSystemRuntimeCommon",
            // IMeshMergeUtilities::MergeComponentsToStaticMesh — performance.merge_actors drives the
            // merge directly with an explicit output package (headless), bypassing the Merge Actors
            // tool's modal CreateModalSaveAssetDialog. MeshMergeUtilities is the Developer module that
            // exposes IMeshMergeModule::GetUtilities() (the same API FMeshMergingTool::RunMerge uses).
            "MeshMergeUtilities",
            "MeshDescription", "StaticMeshDescription",
            "NavigationSystem",
            // SubobjectData moved from SubobjectData to SubobjectDataInterface in UE 5.4.
            "SubobjectDataInterface",
            // Lightweight in-plugin journal facade; the PIE lifecycle handler calls FJournalRecorder.
            "PinWrightRecorder",
            // UWebBrowser (engine UMG CEF widget) for the drive capability's surface=web bridge
            // (FDriveWebBridge discovers live browsers, injects JS, reads console results back).
            // SWebBrowser::GetSource (WEBBROWSER_API) lives in the WebBrowser module, distinct
            // from the WebBrowserWidget UMG wrapper — both are required to link the bridge.
            "WebBrowserWidget", "WebBrowser"
        });

        // FUniversalObjectLocator / FUniversalObjectLocatorFragment back a possessable's binding
        // locators. MovieScene exposes them through its headers, so a per-file compile succeeds
        // without this and only the link reports the missing symbols.
        //
        // The UniversalObjectLocator module (and the whole locator-based binding model) arrived in
        // UE 5.4. On 5.3 the module does not exist at all and UBT hard-fails at the rules stage with
        // "Could not find definition for module 'UniversalObjectLocator'", so the dep must be gated
        // here -- a header shim cannot paper over a missing .Build.cs. 5.3 resolves bindings through
        // the pre-locator LevelSequence path instead (see SequencerBindingUtils.h).
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 4)
        {
            PublicDependencyModuleNames.Add("UniversalObjectLocator");
        }

        // OpenSSL RAND_bytes backs the gateway auth token; FGenericPlatformMisc::CreateGuid is not a CSPRNG on all platforms.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

        // FInstancedStruct / FStructView live in the standalone StructUtils plugin module
        // in UE 5.4 (STRUCTUTILS_API). From UE 5.5 on they were folded into CoreUObject, so
        // the separate module must NOT be linked there. Link it only on 5.4 and earlier.
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 4)
        {
            PrivateDependencyModuleNames.Add("StructUtils");
        }

        // TOptional<T> UPROPERTY reflection (FOptionalProperty) only exists on UE 5.5+, and
        // UE 5.3's UHT rejects a version #if wrapped around a reflected type. So the reflected
        // optional test fixture lives in a sibling Source/V55Fixtures/ directory (outside this
        // module's auto-scanned tree) and is added to the module only on 5.5+. On 5.3/5.4 the
        // directory is never scanned by UHT; the 5.5+-gated tests that use it compile it out.
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 5)
        {
            string V55FixturesDir = Path.Combine(ModuleDirectory, "..", "V55Fixtures");
            if (Directory.Exists(V55FixturesDir))
            {
                ConditionalAddModuleDirectory(new DirectoryReference(V55FixturesDir));
                PrivateIncludePaths.Add(V55FixturesDir);
            }
        }

        // Optional modules that vary by UE version / plugin configuration.
        TryAddConditionalModule(Target, EngineDir, "MetasoundEngine", "MetasoundEngine");
        // localization.gather / localization.compile use the same process
        // launcher as the Unreal Localization Dashboard. Older engine builds
        // may not ship this editor module; the handler has a compile-time guard
        // and reports NOT_SUPPORTED on those hosts.
        TryAddConditionalModule(Target, EngineDir, "LocalizationCommandletExecution", "LocalizationCommandletExecution");
        TryAddConditionalModule(Target, EngineDir, "MetasoundFrontend", "MetasoundFrontend");
        TryAddConditionalModule(Target, EngineDir, "MetasoundEditor", "MetasoundEditor");
        TryAddConditionalModule(Target, EngineDir, "PropertyBindingUtils", "PropertyBindingUtils");
        TryAddConditionalModule(Target, EngineDir, "StateTreeModule", "StateTreeModule");
        TryAddConditionalModule(Target, EngineDir, "StateTreeEditorModule", "StateTreeEditorModule");
        TryAddConditionalModule(Target, EngineDir, "MovieRenderPipelineCore", "MovieRenderPipelineCore");
        TryAddConditionalModule(Target, EngineDir, "MovieRenderPipelineEditor", "MovieRenderPipelineEditor");
        TryAddConditionalModule(Target, EngineDir, "MovieRenderPipelineSettings", "MovieRenderPipelineSettings");
        TryAddConditionalModule(Target, EngineDir, "Water", "Water");
        // GameFeatures: game_features.* read-only introspection over
        // UGameFeaturesSubsystem (Handlers/Systems/GameFeaturesHandler.cpp). Ships with
        // the engine at Plugins/Runtime/GameFeatures but is disabled by default and not
        // enabled by this host's uproject; soft-linking the module here loads it so the
        // engine subsystem exists and the handler can enumerate GF plugins (empty on a
        // host with none). The handler body __has_include-guards GameFeaturesSubsystem.h.
        TryAddConditionalModule(Target, EngineDir, "GameFeatures", "GameFeatures");
        TryAddConditionalModule(Target, EngineDir, "ChaosVehicles", "ChaosVehicles");
        TryAddConditionalModule(Target, EngineDir, "ChaosVehiclesEditor", "ChaosVehiclesEditor");

        // IK Rig / IK Retargeter authoring family (animation.authoring.create_ik_rig,
        // add_ik_chain, create_ik_retargeter, set_retarget_chain_mapping). The IKRig
        // plugin ships with the engine (UE 5.0+) under Plugins/Animation/IKRig and is the
        // only surface for cross-skeleton retargeting authoring. Without these deps the
        // headers leave the include path and the whole family compiles out behind the
        // __has_include guards in AnimationAuthoringHandler_AnimBlueprint.cpp, hard-erroring
        // NOT_SUPPORTED at runtime despite the wiki advertising the workflow as live.
        // IKRig = runtime types (UIKRigDefinition, UIKRetargeter); IKRigEditor = the
        // factories + UIKRigController / UIKRetargeterController used to author them.
        TryAddConditionalModule(Target, EngineDir, "IKRig", "IKRig");
        TryAddConditionalModule(Target, EngineDir, "IKRigEditor", "IKRigEditor");

        // Chaos Cloth authoring (skeleton.create_cloth_from_section). The runtime side
        // (UClothingAssetBase / UClothingAssetCommon, bind/unbind) lives in
        // ClothingSystemRuntimeCommon, already linked above. Authoring a NEW
        // UClothingAsset from a mesh section is editor-only: ClothingSystemEditorInterface
        // exposes the abstraction the create path uses — FClothingSystemEditorInterfaceModule
        // + UClothingAssetFactoryBase (GetClothingAssetFactory / CreateFromSkeletalMesh).
        // The concrete factory is resolved at runtime as a modular feature (registered by
        // the engine's ClothingSystemEditor module when it is loaded), so the new code never
        // references a concrete-factory symbol — we link only the interface module, not the
        // concrete ClothingSystemEditor. Without it the create path compiles out behind the
        // __has_include guard in SkeletalMeshHandler.cpp and the verb hard-errors
        // CLOTH_CREATE_UNSUPPORTED at runtime.
        TryAddConditionalModule(Target, EngineDir, "ClothingSystemEditorInterface", "ClothingSystemEditorInterface");

        // In-process Live Coding compile trigger + status readback
        // (Handlers/System/LiveCodingHandler.cpp: system.live_coding_compile /
        // system.live_coding_status over ILiveCodingModule). LiveCoding is a
        // Developer/Windows module not reachable by TryAddConditionalModule's
        // Runtime/Editor/Plugins search, so link it directly under the same
        // bWithLiveCoding gate UBT uses to define WITH_LIVE_CODING (mirrors Epic's
        // UE 5.8 LiveCodingToolset.Build.cs). The handler body additionally
        // __has_include-guards ILiveCodingModule.h so a build without the module
        // degrades to a graceful NOT_AVAILABLE instead of a compile error.
        if (Target.bWithLiveCoding)
        {
            PrivateDependencyModuleNames.Add("LiveCoding");
        }
    }

    /// <summary>
    /// Searches for a directory with the given name up to a maximum depth.
    /// </summary>
    /// <param name="rootDir">The root directory to start searching from.</param>
    /// <param name="targetName">The directory name to search for.</param>
    /// <param name="maxDepth">Maximum depth to search (0 = root only).</param>
    /// <returns>True if directory is found within the depth limit.</returns>
    private bool SearchDirectoryBounded(string rootDir, string targetName, int maxDepth)
    {
        if (maxDepth < 0 || !Directory.Exists(rootDir)) return false;
        
        try
        {
            foreach (string subDir in Directory.GetDirectories(rootDir))
            {
                string dirName = Path.GetFileName(subDir);
                if (string.Equals(dirName, targetName, StringComparison.OrdinalIgnoreCase))
                    return true;
                
                if (maxDepth > 0 && SearchDirectoryBounded(subDir, targetName, maxDepth - 1))
                    return true;
            }
        }
        catch { /* Ignore access denied errors */ }
        return false;
    }

    /// <summary>
    /// Conditionally adds a module dependency if it exists in the engine or plugins directories.
    /// Used for optional plugin modules that may not be available in all UE versions (StateTree, MetaSound, MovieRenderPipeline).
    /// </summary>
    /// <param name="Target">Build target settings.</param>
    /// <param name="EngineDir">Absolute path to the engine root directory.</param>
    /// <param name="ModuleName">The module name to add to dependencies if found.</param>
    /// <param name="SearchName">The directory name to search for in engine/plugin paths.</param>
    private void TryAddConditionalModule(ReadOnlyTargetRules Target, string EngineDir, string ModuleName, string SearchName)
    {
        try
        {
            // Check Runtime modules
            string RuntimePath = Path.Combine(EngineDir, "Source", "Runtime", SearchName);
            if (Directory.Exists(RuntimePath))
            {
                PrivateDependencyModuleNames.Add(ModuleName);
                return;
            }

            // Check Editor modules
            string EditorPath = Path.Combine(EngineDir, "Source", "Editor", SearchName);
            if (Directory.Exists(EditorPath))
            {
                PrivateDependencyModuleNames.Add(ModuleName);
                return;
            }

            // Check Plugins directory
            string PluginsDir = Path.Combine(EngineDir, "Plugins");
            if (Directory.Exists(PluginsDir))
            {
                // Check common plugin locations
                string[] SearchPaths = new string[]
                {
                    Path.Combine(PluginsDir, "AI", SearchName),
                    Path.Combine(PluginsDir, "Animation", SearchName),
                    Path.Combine(PluginsDir, "Runtime", SearchName),
                    Path.Combine(PluginsDir, "Experimental", SearchName),
                    Path.Combine(PluginsDir, "Runtime", "MassEntity", "Source", SearchName),
                    Path.Combine(PluginsDir, "Runtime", "MassGameplay", "Source", SearchName),
                    Path.Combine(PluginsDir, "Runtime", "SmartObjects", "Source", SearchName),
                    Path.Combine(PluginsDir, "Runtime", "StateTree", "Source", SearchName)
                };

                foreach (string SearchPath in SearchPaths)
                {
                    if (Directory.Exists(SearchPath))
                    {
                        PrivateDependencyModuleNames.Add(ModuleName);
                        return;
                    }
                }

                // Probe Plugins/Experimental/<XxxPlugin>/Source/<SearchName>/ shape (e.g., ChaosVehiclesPlugin).
                // ChaosVehicles lives at Plugins/Experimental/ChaosVehiclesPlugin/Source/ChaosVehicles/ —
                // the explicit SearchPaths above don't cover that nesting.
                string ExperimentalDir = Path.Combine(PluginsDir, "Experimental");
                if (Directory.Exists(ExperimentalDir))
                {
                    foreach (string PluginRoot in Directory.GetDirectories(ExperimentalDir))
                    {
                        string Candidate = Path.Combine(PluginRoot, "Source", SearchName);
                        if (Directory.Exists(Candidate))
                        {
                            PrivateDependencyModuleNames.Add(ModuleName);
                            return;
                        }
                    }
                }

                // Fallback: bounded depth search (max 4 levels) to avoid slow unbounded recursion
                if (SearchDirectoryBounded(PluginsDir, SearchName, 4))
                {
                    PrivateDependencyModuleNames.Add(ModuleName);
                    return;
                }
            }
        }
        catch { /* Module not available - this is expected for optional modules */ }
    }
}
