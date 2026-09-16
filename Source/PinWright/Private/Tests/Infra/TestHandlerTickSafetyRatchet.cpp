// Copyright (c) 2026 Alexander Penkin. MIT License.

// Structural guards for handler code whose correctness depends on where it runs. A behavioural
// regression test would have to reproduce an editor-kill stack, so this file instead rejects the
// source shapes that bypass the dispatcher safe-point and reentrancy gates.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

#include "Dispatch/SafePoint.h"
#include "Tests/TestUtils.h"

namespace HandlerTickSafetyRatchet
{
    FString ResolvePluginRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
    }

    FString ResolveHandlersRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid()
            ? (Plugin->GetBaseDir() / TEXT("Source/PinWright/Private/Handlers"))
            : FString();
    }

    FString ResolvePrivateRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid()
            ? (Plugin->GetBaseDir() / TEXT("Source/PinWright/Private"))
            : FString();
    }

    FString MakeRelative(const FString& File, const FString& Root)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(File);
        const FString FullRoot = FPaths::ConvertRelativePathToFull(Root) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullRoot);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    TArray<FString> CollectSourceFiles(const FString& Root)
    {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.cpp"), true, false, false);
        IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.h"), true, false, false);
        return Files;
    }

    int32 CountMatches(const FString& Contents, const TCHAR* Pattern)
    {
        int32 Count = 0;
        FRegexMatcher Matcher(FRegexPattern(Pattern), Contents);
        while (Matcher.FindNext())
        {
            ++Count;
        }
        return Count;
    }

    const TCHAR* const NestedGameThreadPattern =
        TEXT("AsyncTask\\s*\\(\\s*ENamedThreads::GameThread");
    const TCHAR* const DirectFullCompilePattern =
        TEXT("FKismetEditorUtilities::CompileBlueprint\\s*\\(");
    const TCHAR* const SkeletonOnlyCompilePattern =
        TEXT("FKismetEditorUtilities::CompileBlueprint\\s*\\([^;]*")
        TEXT("EBlueprintCompileOptions::RegenerateSkeletonOnly\\s*\\)");
    const TCHAR* const DiagnosticsCompilePattern =
        TEXT("FKismetEditorUtilities::CompileBlueprint\\s*\\([^;]*")
        TEXT("EBlueprintCompileOptions::SkipGarbageCollection\\s*,\\s*&ResultsLog\\s*\\)");
    const TCHAR* const DeferredFullGarbageCollectionPattern =
        TEXT("GEngine\\s*->\\s*ForceGarbageCollection\\s*\\(\\s*true\\s*\\)");

    // This continuation is inside a StartJob bind delegate. StartJob invokes the bind
    // delegate synchronously, so removing its marshal would turn the advertised job into an
    // inline operation. It needs a different safe-point remedy if its body becomes hazardous.
    const TPair<const TCHAR*, int32> AllowedNestedGameThreadMarshals[] = {
        {TEXT("Blueprint/BlueprintApiIndexHandler.cpp"), 1},
    };

    // The BPIR compiler's two calls are RegenerateSkeletonOnly, which does not flush the
    // reinstancing queue. The diagnostics helper is the sole full-compile chokepoint.
    const TPair<const TCHAR*, int32> AllowedDirectCompileCalls[] = {
        {TEXT("Compiler/BpirCompiler.cpp"), 2},
        {TEXT("Handlers/Blueprint/BlueprintHandlerUtils.cpp"), 1},
    };

    struct FRequiredSafePointSite
    {
        const TCHAR* File;
        const TCHAR* Method;
    };

    const FRequiredSafePointSite RequiredRunAtSafePointSites[] = {
        {TEXT("Environment/LandscapeHandler.cpp"), TEXT("landscape.create")},
        {TEXT("Environment/LandscapeHandler.cpp"), TEXT("landscape.sculpt")},
        {TEXT("Environment/LandscapeHandler.cpp"), TEXT("landscape.set_material")},
        {TEXT("Environment/LandscapeHandler.cpp"), TEXT("landscape.create_grass_type")},
        {TEXT("Environment/LandscapeHandler.cpp"), TEXT("landscape.edit")},
        {TEXT("Environment/LandscapeHandler.cpp"), TEXT("landscape.create_procedural_terrain")},
    };

    const FRequiredSafePointSite RequiredJobSafePointSites[] = {
        {TEXT("Debug/TraceAnalysisHandler.cpp"), TEXT("insights.export_trace")},
        {TEXT("Level/LevelHandler.cpp"), TEXT("level.save")},
        {TEXT("Level/LevelHandler.cpp"), TEXT("level.save_as")},
        {TEXT("Render/RenderHandler.cpp"), TEXT("render.nanite_rebuild_mesh")},
    };

    struct FRequiredStaticMeshRebuildGuardSite
    {
        const TCHAR* File;
        const TCHAR* Method;
    };

    const FRequiredStaticMeshRebuildGuardSite RequiredStaticMeshRebuildGuardSites[] = {
        {TEXT("Source/PinWright/Private/Handlers/Asset/AssetWorkflowHandler.cpp"),
         TEXT("asset.generate_lods")},
        {TEXT("Source/PinWright/Private/Handlers/Asset/AssetWorkflowHandler.cpp"),
         TEXT("asset.nanite_rebuild_mesh")},
        {TEXT("Source/PinWright/Private/Handlers/Asset/StaticMeshBakeTransformHandler.cpp"),
         TEXT("static_mesh.bake_transform")},
        {TEXT("Source/PinWrightGeometry/Private/Handlers/Geometry/LODCollisionHandler.cpp"),
         TEXT("geometry.set_lod_settings")},
        {TEXT("Source/PinWrightGeometry/Private/Handlers/Model/ModelCompileHandler.cpp"),
         TEXT("model.compile")},
    };


    const TCHAR* const BlueprintCompileVerbs[] = {
        TEXT("ai.assign_behavior_tree"),
        TEXT("ai.assign_blackboard"),
        TEXT("ai.run_behavior_tree"),
        TEXT("blueprint.add_dispatcher"),
        TEXT("blueprint.add_event"),
        TEXT("blueprint.add_function"),
        TEXT("blueprint.add_interface"),
        TEXT("blueprint.add_macro"),
        TEXT("blueprint.add_variable"),
        TEXT("blueprint.compile_bpir"),
        TEXT("blueprint.delete_unused_variables"),
        TEXT("blueprint.graph.delete_orphaned_nodes"),
        TEXT("blueprint.insert_bpir_at_node"),
        TEXT("blueprint.insert_bpir_before_node"),
        TEXT("blueprint.modify_scs"),
        TEXT("blueprint.remove_event"),
        TEXT("blueprint.remove_function"),
        TEXT("blueprint.remove_interface"),
        TEXT("blueprint.remove_variable"),
        TEXT("blueprint.rename_variable"),
        TEXT("blueprint.reparent"),
        TEXT("blueprint.scs.add_component"),
        TEXT("blueprint.scs.duplicate_component"),
        TEXT("blueprint.scs.remove_component"),
        TEXT("blueprint.scs.reparent_component"),
        TEXT("blueprint.scs.set_property"),
        TEXT("blueprint.scs.set_transform"),
        TEXT("blueprint.set_function_settings"),
        TEXT("blueprint.set_variable_metadata"),
        TEXT("blueprint.set_variable_settings"),
        TEXT("game_framework.configure_game_rules"),
        TEXT("game_framework.configure_round_system"),
        TEXT("game_framework.configure_scoring_system"),
        TEXT("game_framework.configure_spawn_system"),
        TEXT("game_framework.configure_spectating"),
        TEXT("game_framework.configure_team_system"),
        TEXT("game_framework.set_respawn_rules"),
        TEXT("gas.add_attribute"),
        TEXT("gas.create_execution_calculation"),
        TEXT("interaction.add_interaction_events"),
        TEXT("interaction.configure_chest_properties"),
        TEXT("interaction.configure_door_properties"),
        TEXT("interaction.configure_interaction_trace"),
        TEXT("interaction.configure_interaction_widget"),
        TEXT("interaction.configure_switch_properties"),
        TEXT("networking.add_network_prediction_data"),
        TEXT("networking.configure_rpc_validation"),
        TEXT("networking.create_rpc_function"),
        TEXT("networking.set_autonomous_proxy"),
        TEXT("networking.set_property_replicated"),
        TEXT("networking.set_replicated_using"),
        TEXT("networking.set_replication_condition"),
        TEXT("networking.set_rpc_reliability"),
        TEXT("vehicle.create_wheel_asset"),
        TEXT("vehicle.remove_wheel_setup"),
        TEXT("vehicle.set_suspension"),
        TEXT("vehicle.set_wheel_asset_property"),
        TEXT("vehicle.set_wheel_setup"),
        TEXT("widget.bind_event"),
    };

    struct FRequiredConsentSite
    {
        const TCHAR* File;
        int32 ParamCount;
        int32 RefusalCount;
    };

    const FRequiredConsentSite RequiredConsentSites[] = {
        {TEXT("Blueprint/BpirCompilerHandler.cpp"), 3, 3},
        {TEXT("Blueprint/BlueprintComponentHandler.cpp"), 1, 1},
        {TEXT("Blueprint/BlueprintReparentHandler.cpp"), 1, 1},
        {TEXT("Blueprint/SCSComponentDuplicateHandler.cpp"), 1, 1},
        {TEXT("Blueprint/SCSHandler.cpp"), 5, 1},
    };

    bool HandlerBlockReportsCompile(
        const FString& Source,
        const TCHAR* Verb,
        FString& OutRegistrationBlock)
    {
        const FString Needle = FString::Printf(
            TEXT("REGISTER_RPC_HANDLER(\"%s\""), Verb);
        const int32 Start = Source.Find(Needle);
        if (Start == INDEX_NONE)
        {
            return false;
        }

        int32 End = Source.Find(
            TEXT("REGISTER_RPC_HANDLER("),
            ESearchCase::CaseSensitive,
            ESearchDir::FromStart,
            Start + Needle.Len());
        if (End == INDEX_NONE)
        {
            End = Source.Len();
        }
        OutRegistrationBlock = Source.Mid(Start, End - Start);

        return OutRegistrationBlock.Contains(TEXT("AddCompileDiagnosticsToJson("))
            || OutRegistrationBlock.Contains(TEXT("HandleBlueprintInterfaceMutation("))
            || OutRegistrationBlock.Contains(TEXT("SendSCSResult("));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerHazardsStayGatedTest,
    "PinWright.infra.tick_safety.HandlerHazardsStayGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerHazardsStayGatedTest::RunTest(const FString& Parameters)
{
    using namespace HandlerTickSafetyRatchet;

    const FString HandlersRoot = ResolveHandlersRoot();
    if (!TestFalse(TEXT("Resolved the handler source root"), HandlersRoot.IsEmpty()) ||
        !TestTrue(TEXT("Handler source root exists"),
                  IFileManager::Get().DirectoryExists(*HandlersRoot)))
    {
        return false;
    }

    const TArray<FString> Files = CollectSourceFiles(HandlersRoot);
    if (!TestTrue(TEXT("Recursive walk found handler source files"), Files.Num() > 0))
    {
        return false;
    }

    const FString PatternSample = NeutralizeSourceText(
        TEXT("AsyncTask ( ENamedThreads::GameThread, []{});\n")
        TEXT("// AsyncTask(ENamedThreads::GameThread, []{});\n"));
    if (!TestEqual(TEXT("Marshal pattern matches code but not a comment"),
                   CountMatches(PatternSample, NestedGameThreadPattern), 1))
    {
        return false;
    }

    TMap<FString, int32> Expected;
    for (const TPair<const TCHAR*, int32>& Entry : AllowedNestedGameThreadMarshals)
    {
        Expected.Add(FString(Entry.Key), Entry.Value);
    }

    TMap<FString, int32> Found;
    int32 FilesRead = 0;
    for (const FString& File : Files)
    {
        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *File))
        {
            AddError(FString::Printf(TEXT("Failed to read handler source file: %s"), *File));
            continue;
        }
        ++FilesRead;

        const int32 Count = CountMatches(NeutralizeSourceText(RawContents),
                                         NestedGameThreadPattern);
        if (Count > 0)
        {
            Found.Add(MakeRelative(File, HandlersRoot), Count);
        }
    }

    if (!TestTrue(TEXT("Read at least one handler source file"), FilesRead > 0))
    {
        return false;
    }

    for (const TPair<FString, int32>& Pair : Found)
    {
        const int32* AllowedCount = Expected.Find(Pair.Key);
        if (!AllowedCount)
        {
            AddError(FString::Printf(
                TEXT("%s adds %d nested GameThread marshal(s). Handler bodies already run on the ")
                TEXT("GameThread; detaching them escapes the dispatcher's safe-point and reentrancy ")
                TEXT("guards. Board B-nested-gamethread-marshal-defeats-tick-gate."),
                *Pair.Key, Pair.Value));
        }
        else
        {
            TestEqual(*FString::Printf(TEXT("Allowed job marshals in %s"), *Pair.Key),
                      Pair.Value, *AllowedCount);
        }
    }

    for (const TPair<FString, int32>& Pair : Expected)
    {
        TestEqual(*FString::Printf(TEXT("Explicit job-marshal allow-list entry %s still reproduces"),
                                  *Pair.Key),
                  Found.FindRef(Pair.Key), Pair.Value);
    }

    for (const FRequiredSafePointSite& Entry : RequiredRunAtSafePointSites)
    {
        FString RawContents;
        const FString File = HandlersRoot / Entry.File;
        if (!TestTrue(*FString::Printf(TEXT("Required safe-point file %s exists"), Entry.File),
                      FFileHelper::LoadFileToString(RawContents, *File)))
        {
            continue;
        }

        const FString RequiredCall = FString::Printf(
            TEXT("RunAtSafePoint(Ctx, TEXT(\"%s\")"), Entry.Method);
        TestTrue(*FString::Printf(TEXT("%s stays behind its in-handler safe-point gate"),
                                 Entry.Method),
                 NeutralizeSourceText(RawContents).Contains(RequiredCall));
    }

    for (const FRequiredSafePointSite& Entry : RequiredJobSafePointSites)
    {
        FString RawContents;
        const FString File = HandlersRoot / Entry.File;
        if (!TestTrue(*FString::Printf(TEXT("Required job safe-point file %s exists"), Entry.File),
                      FFileHelper::LoadFileToString(RawContents, *File)))
        {
            continue;
        }

        const FString RequiredCall = FString::Printf(
            TEXT("DeferJobToSafePoint(Ctx, TEXT(\"%s\")"), Entry.Method);
        TestTrue(*FString::Printf(TEXT("%s stays behind its job safe-point continuation"),
                                 Entry.Method),
                 NeutralizeSourceText(RawContents).Contains(RequiredCall));
    }

    const FString PluginRoot = ResolvePluginRoot();
    if (!TestFalse(TEXT("Resolved the PinWright plugin root"), PluginRoot.IsEmpty()))
    {
        return false;
    }

    for (const FRequiredStaticMeshRebuildGuardSite& Entry : RequiredStaticMeshRebuildGuardSites)
    {
        FString RawContents;
        const FString File = PluginRoot / Entry.File;
        if (!TestTrue(*FString::Printf(TEXT("Shared StaticMesh guard file %s exists"), Entry.File),
                      FFileHelper::LoadFileToString(RawContents, *File)))
        {
            continue;
        }

        const FString Neutralized = NeutralizeSourceText(RawContents);
        FString Compact;
        Compact.Reserve(Neutralized.Len());
        for (const TCHAR Character : Neutralized)
        {
            if (!FChar::IsWhitespace(Character))
            {
                Compact.AppendChar(Character);
            }
        }
        const FString RequiredCall = FString::Printf(
            TEXT("RunGuardedStaticMeshRebuild(Ctx,TEXT(\"%s\")"), Entry.Method);
        TestTrue(*FString::Printf(TEXT("%s uses the shared StaticMesh rebuild guard"),
                                  Entry.Method),
                 Compact.Contains(RequiredCall));
    }

    FString GuardSource;
    const FString GuardFile = PluginRoot /
        TEXT("Source/PinWright/Private/Utils/MeshRebuildRenderGuard.h");
    if (TestTrue(TEXT("Shared StaticMesh rebuild helper exists"),
                 FFileHelper::LoadFileToString(GuardSource, *GuardFile)))
    {
        const FString NeutralizedGuard = NeutralizeSourceText(GuardSource);
        TestTrue(TEXT("Shared helper owns the safe-point hop"),
                 NeutralizedGuard.Contains(TEXT("RunAtSafePoint(Ctx")));
        TestTrue(TEXT("Shared helper scans and quiesces live render consumers"),
                 NeutralizedGuard.Contains(TEXT("ScanForStaticMeshRebuildConsumers")) &&
                     NeutralizedGuard.Contains(TEXT("FQuiesceScope Quiesce")));
    }

    TestTrue(TEXT("animation.setup_retargeting stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("animation.setup_retargeting")));
    TestTrue(TEXT("asset.reset_instance_parameters stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("asset.reset_instance_parameters")));
    TestTrue(TEXT("asset.set_metadata stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("asset.set_metadata")));
    TestTrue(TEXT("asset.fixup_redirectors stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("asset.fixup_redirectors")));
    TestTrue(TEXT("audio.authoring.set_sound_wave_properties stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("audio.authoring.set_sound_wave_properties")));
    TestTrue(TEXT("data_table.add_row stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("data_table.add_row")));
    TestTrue(TEXT("data_table.remove_row stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("data_table.remove_row")));
    TestTrue(TEXT("data_table.set_row stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("data_table.set_row")));
    TestTrue(TEXT("data_table.set_row_struct stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("data_table.set_row_struct")));
    TestTrue(TEXT("editor.create_utility_widget stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("editor.create_utility_widget")));
    TestTrue(TEXT("landscape.flush_grass stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("landscape.flush_grass")));
    TestTrue(TEXT("level.save stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("level.save")));
    TestTrue(TEXT("level.save_as stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("level.save_as")));
    TestTrue(TEXT("pcg.create_graph stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("pcg.create_graph")));
    TestTrue(TEXT("render.nanite_rebuild_mesh stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("render.nanite_rebuild_mesh")));
    TestTrue(TEXT("physics.setup_physics_simulation stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("physics.setup_physics_simulation")));
    TestTrue(TEXT("skeleton.create_physics_asset stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("skeleton.create_physics_asset")));
    TestTrue(TEXT("sequencer.bake_control_space stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("sequencer.bake_control_space")));
    TestTrue(TEXT("widget.create_widget_blueprint stays dispatcher-gated"),
             PinWrightSafePoint::IsTickUnsafeMethod(TEXT("widget.create_widget_blueprint")));

    const FString PrivateRoot = ResolvePrivateRoot();
    if (!TestFalse(TEXT("Resolved the private source root"), PrivateRoot.IsEmpty()) ||
        !TestTrue(TEXT("Private source root exists"),
                  IFileManager::Get().DirectoryExists(*PrivateRoot)))
    {
        return false;
    }

    const FString CompilePatternSample = NeutralizeSourceText(
        TEXT("FKismetEditorUtilities::CompileBlueprint(BP);\n")
        TEXT("// FKismetEditorUtilities::CompileBlueprint(CommentedOut);\n"));
    TestEqual(TEXT("Direct compile pattern matches code but not a comment"),
              CountMatches(CompilePatternSample, DirectFullCompilePattern), 1);

    TMap<FString, int32> AllowedCompileCalls;
    for (const TPair<const TCHAR*, int32>& Entry : AllowedDirectCompileCalls)
    {
        AllowedCompileCalls.Add(FString(Entry.Key), Entry.Value);
    }

    TMap<FString, int32> FoundCompileCalls;
    TMap<FString, FString> CompileSources;
    for (const FString& File : CollectSourceFiles(PrivateRoot))
    {
        const FString Relative = MakeRelative(File, PrivateRoot);
        if (Relative.StartsWith(TEXT("Tests/")))
        {
            continue;
        }

        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *File))
        {
            AddError(FString::Printf(TEXT("Failed to read plugin source file: %s"), *File));
            continue;
        }

        FString Neutralized = NeutralizeSourceText(RawContents);
        const int32 Count = CountMatches(Neutralized, DirectFullCompilePattern);
        if (Count > 0)
        {
            FoundCompileCalls.Add(Relative, Count);
            CompileSources.Add(Relative, MoveTemp(Neutralized));
        }
    }

    for (const TPair<FString, int32>& Pair : FoundCompileCalls)
    {
        const int32* AllowedCount = AllowedCompileCalls.Find(Pair.Key);
        if (!AllowedCount)
        {
            AddError(FString::Printf(
                TEXT("%s adds %d direct CompileBlueprint call(s) outside the diagnostics ")
                TEXT("chokepoint. Route full compiles through CompileBlueprintWithDiagnostics. ")
                TEXT("Board B-blueprint-mutators-compile-ungated-tick-unsafe."),
                *Pair.Key, Pair.Value));
        }
        else
        {
            TestEqual(*FString::Printf(TEXT("Allowed direct compiles in %s"), *Pair.Key),
                      Pair.Value, *AllowedCount);
        }
    }

    for (const TPair<FString, int32>& Pair : AllowedCompileCalls)
    {
        TestEqual(*FString::Printf(TEXT("Direct-compile allow-list entry %s still reproduces"),
                                  *Pair.Key),
                  FoundCompileCalls.FindRef(Pair.Key), Pair.Value);
    }

    TestEqual(TEXT("BPIR direct compile exceptions stay skeleton-only"),
              CountMatches(CompileSources.FindRef(TEXT("Compiler/BpirCompiler.cpp")),
                           SkeletonOnlyCompilePattern),
              2);
    TestEqual(TEXT("The diagnostics helper full compile must pass SkipGarbageCollection"),
              CountMatches(CompileSources.FindRef(
                               TEXT("Handlers/Blueprint/BlueprintHandlerUtils.cpp")),
                           DiagnosticsCompilePattern),
              1);
    TestEqual(TEXT("The diagnostics helper must request one deferred full garbage collection"),
              CountMatches(CompileSources.FindRef(
                               TEXT("Handlers/Blueprint/BlueprintHandlerUtils.cpp")),
                           DeferredFullGarbageCollectionPattern),
              1);

    TestEqual(TEXT("The complete compile-mutator route inventory stays at 59"),
              static_cast<int32>(UE_ARRAY_COUNT(BlueprintCompileVerbs)), 59);

    TMap<FString, FString> HandlerSources;
    for (const FString& File : Files)
    {
        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *File))
        {
            AddError(FString::Printf(TEXT("Failed to read reporting source file: %s"), *File));
            continue;
        }
        HandlerSources.Add(MakeRelative(File, HandlersRoot), NeutralizeSourceText(RawContents));
    }

    for (const TCHAR* Verb : BlueprintCompileVerbs)
    {
        TestTrue(*FString::Printf(TEXT("%s stays dispatcher-gated"), Verb),
                 PinWrightSafePoint::IsTickUnsafeMethod(Verb));

        int32 RegistrationCount = 0;
        bool bReportsCompile = false;
        for (const TPair<FString, FString>& Pair : HandlerSources)
        {
            FString RegistrationBlock;
            if (HandlerBlockReportsCompile(Pair.Value, Verb, RegistrationBlock))
            {
                ++RegistrationCount;
                bReportsCompile = true;
            }
            else if (!RegistrationBlock.IsEmpty())
            {
                ++RegistrationCount;
            }
        }
        TestEqual(*FString::Printf(TEXT("%s has one registered handler"), Verb),
                  RegistrationCount, 1);
        TestTrue(*FString::Printf(TEXT("%s reports compile diagnostics"), Verb),
                 bReportsCompile);
    }

    TestTrue(TEXT("Interface route delegation emits diagnostics"),
             HandlerSources.FindRef(TEXT("Blueprint/BlueprintInterfaceHandler.cpp"))
                 .Contains(TEXT("AddCompileDiagnosticsToJson(")));

    const FString BpirSource =
        HandlerSources.FindRef(TEXT("Blueprint/BpirCompilerHandler.cpp"));
    TestTrue(TEXT("BPIR Blueprint-compile failures retain their diagnostics payload"),
             BpirSource.Contains(TEXT("PendingResponse.Payload = Payload")));
    TestEqual(TEXT("All three BPIR Blueprint-compile failure routes pass the payload"),
              CountMatches(BpirSource,
                  TEXT("SetBlueprintCompileFailureResponse\\s*\\(\\s*PendingResponse\\s*,\\s*Out")),
              3);

    FString SuspensionBlock;
    const FString VehicleSource =
        HandlerSources.FindRef(TEXT("Physics/ChaosVehicleHandler.cpp"));
    TestTrue(TEXT("vehicle.set_suspension handler remains registered"),
             HandlerBlockReportsCompile(
                 VehicleSource, TEXT("vehicle.set_suspension"), SuspensionBlock));
    TestTrue(TEXT("vehicle.set_suspension returns every per-asset compile result"),
             SuspensionBlock.Contains(TEXT("SetArrayField(TEXT(\"compileResults\")")));
    TestTrue(TEXT("vehicle.set_suspension fails when any wheel Blueprint compile fails"),
             SuspensionBlock.Contains(
                 TEXT("Ctx.SendError(ErrorCodes::ERR_COMPILE_FAILED")));

    FString SCSImplementation;
    const FString SCSImplementationFile = PrivateRoot / TEXT("PinWright_SCSHandlers.cpp");
    if (TestTrue(TEXT("SCS implementation source exists"),
                 FFileHelper::LoadFileToString(SCSImplementation, *SCSImplementationFile)))
    {
        const FString NeutralizedSCS = NeutralizeSourceText(SCSImplementation);
        TestEqual(TEXT("Five SCS mutators share the reporting compile finalizer"),
                  CountMatches(NeutralizedSCS,
                      TEXT("FinalizeBlueprintSCSChange\\s*\\(")),
                  6);
        TestEqual(TEXT("SCS finalizer emits compile diagnostics once"),
                  CountMatches(NeutralizedSCS,
                      TEXT("AddCompileDiagnosticsToJson\\s*\\(")),
                  1);
    }

    for (const FRequiredConsentSite& Site : RequiredConsentSites)
    {
        FString RawContents;
        const FString File = HandlersRoot / Site.File;
        if (!TestTrue(*FString::Printf(TEXT("Consent file %s exists"), Site.File),
                      FFileHelper::LoadFileToString(RawContents, *File)))
        {
            continue;
        }

        const FString Neutralized = NeutralizeSourceText(RawContents);
        TestEqual(*FString::Printf(TEXT("allowReinstancing specs in %s"), Site.File),
                  CountMatches(Neutralized, TEXT("AllowReinstancingParam\\s*\\(\\s*\\)")),
                  Site.ParamCount);
        TestEqual(*FString::Printf(TEXT("live-instance refusals in %s"), Site.File),
                  CountMatches(Neutralized,
                      TEXT("RefuseIfLiveInstancesWouldBeReinstanced\\s*\\(")),
                  Site.RefusalCount);
    }

    return true;
}
