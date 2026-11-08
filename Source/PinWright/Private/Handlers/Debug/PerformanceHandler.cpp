// Copyright (c) 2026 Alexander Penkin. MIT License.

// PerformanceHandler.cpp (editor module): the editor-only performance.* verbs. merge_actors writes
// assets and edits the level through UnrealEd; optimize_shaders drives the editor shader compiler.
// The game-safe performance.* verbs live in PinWrightRuntime/Private/Handlers/Debug/PerformanceHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Debug/PerformanceHooks.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Containers/Ticker.h"
#include "ShaderCompiler.h"
#include "Engine/Engine.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Modules/ModuleManager.h"

// merge_actors headless path: drive IMeshMergeUtilities::MergeComponentsToStaticMesh
// directly with an explicit output package, bypassing the Merge Actors tool's modal
// CreateModalSaveAssetDialog (which would hang a headless MCP call).
#include "MeshMergeModule.h"
#include "IMeshMergeUtilities.h"
// FMeshMergingSettings concrete definition. NOTE: the Engine/MeshMerging.h umbrella only
// pulls these settings headers in under UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5 (off in
// 5.7's modern include order), so include the concrete header directly — IMeshMergeUtilities.h
// only forward-declares FMeshMergingSettings. The header split out of Engine/MeshMerging.h
// in UE 5.5; on 5.3/5.4 the concrete struct still lives in the Engine umbrella header.
#if __has_include("MeshMerge/MeshMergingSettings.h")
#include "MeshMerge/MeshMergingSettings.h"
#else
#include "Engine/MeshMerging.h"
#endif
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"

// Feeds performance.run_benchmark's guardHeldFrames: the throttle guard lives in the editor subsystem.
static struct FPinWrightRegisterThrottleGuardHook
{
    FPinWrightRegisterThrottleGuardHook()
    {
        PinWrightPerformance::IsBackgroundThrottleGuardHeld = []() -> bool
        {
            const UPinWrightSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UPinWrightSubsystem>() : nullptr;
            return Subsystem && Subsystem->IsHoldingBackgroundThrottle();
        };
    }
} GPinWrightRegisterThrottleGuardHook;

// ---- performance.merge_actors ----
// Drives the merge headlessly via IMeshMergeUtilities::MergeComponentsToStaticMesh
// with a computed output package, NOT the interactive Merge Actors tool
// (IMergeActorsTool::RunMergeFromSelection). The tool path routes through
// GetPackageNameForMergeAction -> CreateModalSaveAssetDialog, a modal dialog that
// cannot be answered in a headless MCP context (board
// B-merge-actors-rejects-valid-selection). Driving the utilities API directly with
// an explicit, non-empty package name bypasses the dialog and the editor-selection
// gate entirely.
REGISTER_RPC_HANDLER("performance.merge_actors", "performance", "Combine 2+ static-mesh actors into a single merged StaticMeshActor (UE's mesh-merge utilities) — the standard workflow for reducing draw calls on grouped static geometry. Runs headless (no save dialog). Optionally deletes the source actors and replaces them with the merge result.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actors", "array", "Array of actor display labels or paths; minimum 2 entries. Only actors carrying a UStaticMeshComponent with a non-null static mesh are merged."),
        RPC_PARAM_OPT("toolName", "string", "Merge tool variant; defaults to 'Merge' (UE's mesh-merging tool, GetToolNameText()==\"Merge\"). The legacy alias 'MeshMerging' is accepted and mapped to 'Merge'. Only the mesh-merging variant is performed — other tool names are rejected."),
        RPC_PARAM_OPT("outputPackage", "string", "Explicit long package name for the merged static mesh asset (e.g. '/Game/Merged/MyMerge'); defaults to a generated, unique '/Game/Merged/MERGED_<FirstActor>' path."),
        RPC_PARAM_OPT("replaceSourceActors", "boolean", "When true, deletes the source actors after merging; defaults to false (kept alongside the merged result).")
    ))
{
    const TArray<TSharedPtr<FJsonValue>>* NamesArray = Ctx.GetArray(TEXT("actors"));
    if (!NamesArray || NamesArray->Num() < 2)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("merge_actors requires an 'actors' array with at least 2 entries"));
        return true;
    }

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available for merge_actors"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    TArray<AActor*> ActorsToMerge;

    auto ResolveActorByName = [World](const FString& Name) -> AActor*
    {
        if (Name.IsEmpty()) return nullptr;

        if (AActor* ByPath = FindObject<AActor>(nullptr, *Name))
            return ByPath;

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor) continue;

            const FString Label = Actor->GetActorLabel();
            const FString ObjName = Actor->GetName();
            if (Label.Equals(Name, ESearchCase::IgnoreCase) ||
                ObjName.Equals(Name, ESearchCase::IgnoreCase))
            {
                return Actor;
            }
        }
        return nullptr;
    };

    for (const TSharedPtr<FJsonValue>& Val : *NamesArray)
    {
        if (!Val.IsValid() || Val->Type != EJson::String) continue;

        const FString RawName = Val->AsString().TrimStartAndEnd();
        if (RawName.IsEmpty()) continue;

        if (AActor* Resolved = ResolveActorByName(RawName))
        {
            ActorsToMerge.AddUnique(Resolved);
        }
    }

    if (ActorsToMerge.Num() < 2)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("merge_actors resolved fewer than 2 valid actors"));
        return true;
    }

    // Validate the toolName param against the only variant this handler can honor.
    // The merge does NOT go through any IMergeActorsTool's RunMergeFromSelection():
    // that path calls GetPackageNameForMergeAction(GetDefaultPackageName(), ...) ->
    // ContentBrowserModule.CreateModalSaveAssetDialog(), a blocking modal "Create
    // Merged Actor" dialog that hangs a headless MCP call. Instead we drive
    // IMeshMergeUtilities::MergeComponentsToStaticMesh directly with an explicit
    // output package (the same engine API FMeshMergingTool::RunMerge uses, minus the
    // dialog). Because the merge is hardwired to mesh-merging, the only accepted
    // toolName values are "Merge" (UE 5.7's mesh-merging tool reports
    // GetToolNameText()=="Merge") and the historical alias "MeshMerging" — any other
    // registered tool (MeshProxy, Batch, ...) is rejected rather than silently
    // running a mesh merge under a name the handler does not actually perform.
    FString RequestedToolName = Ctx.GetString(TEXT("toolName"));
    if (!RequestedToolName.IsEmpty()
        && !RequestedToolName.Equals(TEXT("Merge"), ESearchCase::IgnoreCase)
        && !RequestedToolName.Equals(TEXT("MeshMerging"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(TEXT("MERGE_TOOL_UNAVAILABLE"),
            FString::Printf(TEXT("merge_actors only performs the mesh-merging variant; toolName '%s' is not honored (use 'Merge')"),
                *RequestedToolName));
        return true;
    }

    // Collect the static-mesh primitive components to merge directly from the resolved
    // actors — no dependency on the editor selection set or the interactive tool's
    // CanMergeFromSelection() gate. This is exactly the includability rule the engine
    // applies (UStaticMeshComponent with a non-null GetStaticMesh()). UniqueLevels is
    // tracked so replaceSourceActors can require a single shared level and so the merged
    // actor is spawned into the source level.
    TArray<UPrimitiveComponent*> ComponentsToMerge;
    TArray<ULevel*> UniqueLevels;
    for (AActor* Actor : ActorsToMerge)
    {
        if (!Actor) continue;
        TArray<UStaticMeshComponent*> MeshComponents;
        Actor->GetComponents(MeshComponents);
        for (UStaticMeshComponent* MeshComponent : MeshComponents)
        {
            if (MeshComponent && MeshComponent->GetStaticMesh() != nullptr)
            {
                ComponentsToMerge.Add(MeshComponent);
            }
        }
        UniqueLevels.AddUnique(Actor->GetLevel());
    }

    if (ComponentsToMerge.Num() == 0)
    {
        Ctx.SendError(TEXT("MERGE_NOT_POSSIBLE"),
            TEXT("None of the resolved actors carry a UStaticMeshComponent with an assigned static mesh"));
        return true;
    }

    const bool bReplaceSources = Ctx.GetBool(TEXT("replaceSourceActors"), false);
    if (bReplaceSources && UniqueLevels.Num() > 1)
    {
        Ctx.SendError(TEXT("MERGE_NOT_POSSIBLE"),
            TEXT("replaceSourceActors requires all source actors to live in the same level"));
        return true;
    }

    // Compute an explicit, non-empty output package so the utilities path never opens
    // a modal save dialog. A caller-supplied outputPackage wins; otherwise default to a
    // generated, unique /Game/Merged/MERGED_<FirstActor> path.
    FString OutputPackage = Ctx.GetString(TEXT("outputPackage")).TrimStartAndEnd();
    if (OutputPackage.IsEmpty())
    {
        // Sanitize the actor label into a valid asset-name segment (labels routinely
        // carry spaces/dots/slashes that are illegal in a long package name), then run
        // it through CreateUniqueAssetName so a repeat merge yields MERGED_<Actor>_1,
        // _2, ... instead of silently overwriting a prior merged mesh.
        const FString SafeSegment = SanitizeAssetName(
            FString::Printf(TEXT("MERGED_%s"), *ActorsToMerge[0]->GetActorLabel()));
        const FString DesiredPackage = FString::Printf(TEXT("/Game/Merged/%s"), *SafeSegment);

        const FAssetToolsModule& AssetToolsModule =
            FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        FString UniqueAssetName;
        AssetToolsModule.Get().CreateUniqueAssetName(DesiredPackage, TEXT(""), OutputPackage, UniqueAssetName);
    }

    // Validate the final long package name so an invalid caller string or sanitized
    // segment fails with the project's standard INVALID_PATH error instead of reaching
    // low-level package code with a malformed path.
    if (!IsValidMountPoint(OutputPackage) || OutputPackage.Contains(TEXT(" ")))
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            FString::Printf(TEXT("Invalid output package path '%s'"), *OutputPackage));
        return true;
    }

    IMeshMergeUtilities& MeshUtilities =
        FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();

    FMeshMergingSettings MergeSettings;
    TArray<UObject*> AssetsToSync;
    FVector MergedActorLocation = FVector::ZeroVector;
    const float ScreenAreaSize = TNumericLimits<float>::Max();

    MeshUtilities.MergeComponentsToStaticMesh(
        ComponentsToMerge, World, MergeSettings, /*InBaseMaterial*/ nullptr, /*InOuter*/ nullptr,
        OutputPackage, AssetsToSync, MergedActorLocation, ScreenAreaSize, /*bSilent*/ true);

    // Single pass over the produced assets: register each so it appears in the
    // Content Browser, and capture the first UStaticMesh as the merge result.
    UStaticMesh* MergedMesh = nullptr;
    for (UObject* Asset : AssetsToSync)
    {
        if (!Asset) continue;
        FAssetRegistryModule::AssetCreated(Asset);
        if (!MergedMesh)
        {
            MergedMesh = Cast<UStaticMesh>(Asset);
        }
    }

    if (!MergedMesh)
    {
        Ctx.SendError(TEXT("MERGE_FAILED"), TEXT("Mesh merge produced no static mesh asset"));
        return true;
    }

    // Spawn the merged actor at the computed merge location and assign the merged mesh.
    // ComponentsToMerge is non-empty here (the gate above returned otherwise), so the
    // same loop ran UniqueLevels.AddUnique() at least once — UniqueLevels[0] always exists.
    // ToRawPtr(World->PersistentLevel): PersistentLevel is a TObjectPtr<ULevel> while
    // UniqueLevels[0] is a raw ULevel* — without the explicit raw conversion the ternary
    // has ambiguous common type (C2445) between ULevel* and TObjectPtr<ULevel>.
    ULevel* SpawnLevel = UniqueLevels[0] ? UniqueLevels[0] : ToRawPtr(World->PersistentLevel);
    FActorSpawnParameters SpawnParams;
    SpawnParams.OverrideLevel = SpawnLevel;
    AStaticMeshActor* MergedActor = World->SpawnActor<AStaticMeshActor>(
        MergedActorLocation, FRotator::ZeroRotator, SpawnParams);

    if (!MergedActor)
    {
        Ctx.SendError(TEXT("MERGE_FAILED"), TEXT("Failed to spawn merged StaticMeshActor"));
        return true;
    }

    if (UStaticMeshComponent* MergedComponent = MergedActor->GetStaticMeshComponent())
    {
        MergedComponent->SetStaticMesh(MergedMesh);
        MergedComponent->MarkRenderStateDirty();
    }
    MergedActor->SetActorLabel(MergedMesh->GetName());

    if (bReplaceSources)
    {
        // Delete via UEditorActorSubsystem so the removal is undoable and keeps the
        // editor selection/outliner consistent — matching the plugin's actor-deletion
        // convention (LifecycleHandler, EffectHandler, etc.). Fall back to
        // World->EditorDestroyActor only if the subsystem is unavailable.
        UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
        for (AActor* Actor : ActorsToMerge)
        {
            if (!Actor) continue;
            if (ActorSS)
            {
                ActorSS->DestroyActor(Actor);
            }
            else
            {
                World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("mergedActorCount"), ActorsToMerge.Num());
    Resp->SetNumberField(TEXT("sourceActorCount"), ActorsToMerge.Num());
    Resp->SetNumberField(TEXT("mergedComponentCount"), ComponentsToMerge.Num());
    Resp->SetBoolField(TEXT("replaceSourceActors"), bReplaceSources);
    // mergedPackageName / outputPackage are the same explicit, non-modal output package;
    // both are reported (mergedPackageName for callers/tests that assert the headless
    // path was taken, outputPackage as the human-facing name).
    Resp->SetStringField(TEXT("mergedPackageName"), OutputPackage);
    Resp->SetStringField(TEXT("outputPackage"), OutputPackage);
    Resp->SetStringField(TEXT("mergedAssetPath"), MergedMesh->GetPathName());
    Resp->SetStringField(TEXT("mergedMesh"), MergedMesh->GetPathName());
    AddActorVerification(Resp, MergedActor);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- performance.optimize_shaders ----
REGISTER_RPC_HANDLER("performance.optimize_shaders", "performance", "Recompile shaders by mode",
    RPC_PARAMS(
        RPC_PARAM_OPT("mode", "string", "Recompile mode: 'changed', 'material', 'global'"),
        RPC_PARAM_OPT("forceRecompile", "boolean", "Force recompile all shaders")
    ))
{
    FString Mode = Ctx.GetString(TEXT("mode"), TEXT("changed"));
    bool bForceRecompile = Ctx.GetBool(TEXT("forceRecompile"), false);

    FString Cmd;
    if (bForceRecompile)
        Cmd = TEXT("all");
    else if (Mode.Equals(TEXT("material"), ESearchCase::IgnoreCase))
        Cmd = TEXT("material");
    else if (Mode.Equals(TEXT("global"), ESearchCase::IgnoreCase))
        Cmd = TEXT("global");
    else
        Cmd = TEXT("changed");

    FJobBindArgs Args;
    Args.Method = TEXT("performance.optimize_shaders");
    Args.StartedPayload = MakeShared<FJsonObject>();
    Args.StartedPayload->SetStringField(TEXT("mode"), Cmd);

    Args.BindNativeDelegate =
        [Cmd](FJobOnComplete OnComplete)
    {
        if (GEngine)
        {
            GEngine->Exec(nullptr, *FString::Printf(TEXT("recompileshaders %s"), *Cmd));
        }
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [OnComplete](float) mutable -> bool
        {
            if (!GShaderCompilingManager || !GShaderCompilingManager->IsCompiling())
            {
                auto R = MakeShared<FJsonObject>();
                R->SetBoolField(TEXT("compiled"), true);
                OnComplete(true, R, FString());
                return false;
            }
            return true;
        }), 0.5f);
    };
    Ctx.StartJob(Args);
    return true;
}
