// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintPreviewHandler.cpp - blueprint.preview_construction: run a Blueprint's construction
// scripts (SCS + UserConstructionScript) on a throwaway instance and report what they built.
//
// Model: FBlueprintEditor::UpdatePreviewActor (BlueprintEditor.cpp), which spawns the class into
// an FPreviewScene. The instance is spawned DEFERRED so caller `variables` land before
// FinishSpawning runs the construction scripts - the same moment "Expose on Spawn" values land -
// and it is RF_Transient so nothing it touches can dirty a package: MarkPackageDirty returns early
// for any object transient along its outer chain (UObjectBaseUtility.cpp, MarkPackageDirty), and
// a child actor spawned by a transient owner is itself transient (ChildActorComponent.cpp,
// CreateChildActor). Nothing persists, so the verb opens no transaction.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/ActorDescribeBuilder.h"
#include "Utils/JsonBuilders.h"
#include "Utils/PropertyImport.h"

#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "PreviewScene.h"

using namespace BlueprintHandlerUtils;

// Named, not anonymous: Unity merges this TU with its Blueprint siblings.
namespace PinWrightBlueprintPreview
{
    const TCHAR* StatusToString(EBlueprintStatus Status)
    {
        switch (Status)
        {
        case BS_Unknown:               return TEXT("Unknown");
        case BS_Dirty:                 return TEXT("Dirty");
        case BS_Error:                 return TEXT("Error");
        case BS_UpToDate:              return TEXT("UpToDate");
        case BS_BeingCreated:          return TEXT("BeingCreated");
        case BS_UpToDateWithWarnings:  return TEXT("UpToDateWithWarnings");
        default:                       return TEXT("Unknown");
        }
    }

    // FPreviewScene's teardown requests a process-wide full-purge GC whenever
    // r.ForceGCOnPreviewSceneExit is set (PreviewScene.cpp, Uninitialize). Suppressed for the
    // destruction only - the same choice niagara.simulate and render.capture_mesh make.
    struct FScopedPreviewSceneGcSuppression
    {
        IConsoleVariable* Variable =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.ForceGCOnPreviewSceneExit"));
        int32 Previous = Variable ? Variable->GetInt() : 0;

        FScopedPreviewSceneGcSuppression()
        {
            if (Variable)
            {
                Variable->Set(0, ECVF_SetByCode);
            }
        }
        ~FScopedPreviewSceneGcSuppression()
        {
            if (Variable)
            {
                Variable->Set(Previous, ECVF_SetByCode);
            }
        }
    };

    TSharedPtr<FJsonObject> BuildBoundsJson(AActor* Actor)
    {
        const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding=*/true, /*bIncludeFromChildActors=*/true);
        TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
        Bounds->SetBoolField(TEXT("valid"), Box.IsValid != 0);
        if (Box.IsValid)
        {
            Bounds->SetObjectField(TEXT("min"), JsonBuilders::BuildVectorJson(Box.Min));
            Bounds->SetObjectField(TEXT("max"), JsonBuilders::BuildVectorJson(Box.Max));
        }
        return Bounds;
    }

    // Reads the constructed instance. Component rows reuse actor.describe's builder so the two
    // verbs share one vocabulary (creationMethod, attachParent, sparse `properties` diff against
    // each component's archetype); worldTransform is the one field a preview adds.
    void DescribeConstructedActor(AActor* Actor, const TSharedPtr<FJsonObject>& Out)
    {
        TArray<TSharedPtr<FJsonValue>> Components = ActorDescribeBuilder::BuildComponentsJson(Actor);
        for (const TSharedPtr<FJsonValue>& Value : Components)
        {
            const TSharedPtr<FJsonObject> Row = Value->AsObject();
            FString Name;
            if (!Row.IsValid() || !Row->TryGetStringField(TEXT("name"), Name))
            {
                continue;
            }
            if (const USceneComponent* Scene = FindObjectFast<USceneComponent>(Actor, *Name))
            {
                Row->SetObjectField(TEXT("worldTransform"),
                    JsonBuilders::BuildTransformJson(Scene->GetComponentTransform()));
            }
        }
        Out->SetNumberField(TEXT("componentCount"), Components.Num());
        Out->SetArrayField(TEXT("components"), Components);

        TArray<TSharedPtr<FJsonValue>> ChildActors;
        TArray<UChildActorComponent*> ChildActorComponents;
        Actor->GetComponents(ChildActorComponents);
        for (const UChildActorComponent* ChildComponent : ChildActorComponents)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("component"), ChildComponent->GetName());
            const UClass* ChildClass = ChildComponent->GetChildActorClass();
            Row->SetStringField(TEXT("class"), ChildClass ? ChildClass->GetPathName() : FString());
            AActor* Child = ChildComponent->GetChildActor();
            Row->SetBoolField(TEXT("spawned"), Child != nullptr);
            if (Child)
            {
                Row->SetStringField(TEXT("actorName"), Child->GetName());
                Row->SetArrayField(TEXT("components"),
                    ActorDescribeBuilder::BuildComponentsJson(Child));
            }
            ChildActors.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("childActors"), ChildActors);
        Out->SetObjectField(TEXT("bounds"), BuildBoundsJson(Actor));
    }
}

// ---- blueprint.preview_construction ----
REGISTER_RPC_HANDLER("blueprint.preview_construction", "blueprint", "Run an Actor Blueprint's construction scripts (SCS + UserConstructionScript) on a throwaway transient instance and report every component they built - name, class, creationMethod, attachParent, relative and world transform, sparse property diff against the template - plus child actors and bounds. Optional `variables` are applied before construction runs, like Expose-on-Spawn values. The instance is destroyed before the call returns: nothing is placed, saved or dirtied. Refused with BLUEPRINT_COMPILE_FAILED when the class (or a Blueprint parent) is in compile error.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Actor Blueprint asset path.")),
        RPC_PARAM_DEF("world", "string", "Where the instance is constructed: 'preview' (a private empty world, the default) or 'editor' (the open level, hidden from the outliner, so a construction script that queries the world sees the level). Echoed in the response.", "preview"),
        RPC_PARAM_OPT("variables", "object", "Object whose keys are property names on the generated class and values are JSON values, applied before the construction scripts run. An unknown name is PROPERTY_NOT_FOUND, an unconvertible value TYPE_MISMATCH; nothing is applied to the Blueprint itself."),
        RPC_PARAM_OPT("location", "object", "Spawn location {x,y,z} in cm; defaults to origin."),
        RPC_PARAM_OPT("rotation", "object", "Spawn rotation {pitch,yaw,roll} in degrees; defaults to identity."),
        RPC_PARAM_OPT("scale", "object", "Spawn scale {x,y,z}; defaults to (1,1,1).")
    ))
{
    using namespace PinWrightBlueprintPreview;

    const FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("blueprint.preview_construction requires a blueprint path."));
        return true;
    }

    const FString WorldMode = Ctx.GetString(TEXT("world"), TEXT("preview")).ToLower();
    if (WorldMode != TEXT("preview") && WorldMode != TEXT("editor"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("world must be 'preview' or 'editor', got '%s'."), *WorldMode));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* BP = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!BP)
    {
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_NOT_FOUND, LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    UClass* Class = BP->GeneratedClass;
    if (!Class || !Class->IsChildOf(AActor::StaticClass())
        || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE,
            FString::Printf(TEXT("'%s' has no instantiable Actor class (generated class: %s). Only a compiled, non-abstract Actor Blueprint has construction scripts to run."),
                *Path, Class ? *Class->GetPathName() : TEXT("none")));
        return true;
    }

    // The engine's own gate: with any Blueprint in the class chain in BS_Error, ExecuteConstruction
    // skips SCS and UCS and builds a placeholder billboard instead (ActorConstruction.cpp), so a
    // preview would report a construction that never ran.
    TArray<const UBlueprintGeneratedClass*> ClassChain;
    if (!UBlueprintGeneratedClass::GetGeneratedClassesHierarchy(Class, ClassChain))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("compileStatus"), StatusToString(BP->Status));
        Ctx.SendError(ErrorCodes::ERR_BLUEPRINT_COMPILE_FAILED,
            FString::Printf(TEXT("'%s' (or a Blueprint parent) is in compile error, so its construction scripts would not run. Fix it and call blueprint.compile first."), *Path),
            Data);
        return true;
    }

    // Resolve every variable name before anything is spawned.
    TArray<TPair<FProperty*, TSharedPtr<FJsonValue>>> Variables;
    if (const TSharedPtr<FJsonObject> VariablesObj = Ctx.GetObject(TEXT("variables")))
    {
        for (const auto& Pair : VariablesObj->Values)
        {
            const FString Key(*Pair.Key);
            FProperty* Property = Class->FindPropertyByName(FName(*Key));
            if (!Property)
            {
                Ctx.SendError(ErrorCodes::ERR_PROPERTY_NOT_FOUND,
                    FString::Printf(TEXT("'%s' has no property '%s'. Read the class variables with blueprint.inspect."), *Class->GetName(), *Key));
                return true;
            }
            Variables.Emplace(Property, Pair.Value);
        }
    }

    TUniquePtr<FPreviewScene> Scene;
    UWorld* World = nullptr;
    if (WorldMode == TEXT("preview"))
    {
        FPreviewScene::ConstructionValues SceneValues;
        SceneValues.SetCreatePhysicsScene(false).ShouldSimulatePhysics(false).SetTransactional(false)
            .SetCreateDefaultLighting(false);
        Scene = MakeUnique<FPreviewScene>(SceneValues);
        World = Scene->GetWorld();
    }
    else
    {
        World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_SPAWN_FAILED, TEXT("No world to construct the preview instance in."));
        return true;
    }

    const FTransform SpawnTransform(
        Ctx.GetRotator(TEXT("rotation"), FRotator::ZeroRotator),
        Ctx.GetVector(TEXT("location"), FVector::ZeroVector),
        Ctx.GetVector(TEXT("scale"), FVector::OneVector));

    FActorSpawnParameters Params;
    Params.bDeferConstruction = true;
    Params.bTemporaryEditorActor = true;
    Params.bHideFromSceneOutliner = true;
    Params.bNoFail = true;
    // Transient and NOT transactional: never saved, never dirties a package, never in undo history.
    Params.ObjectFlags = RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Params.OverrideLevel = World->PersistentLevel;

    AActor* Actor = World->SpawnActor(Class, &SpawnTransform, Params);

    // Every exit below destroys the instance and then the private world.
    ON_SCOPE_EXIT
    {
        if (IsValid(Actor))
        {
            World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
        }
        if (Scene)
        {
            FScopedPreviewSceneGcSuppression SuppressGc;
            Scene.Reset();
        }
    };

    if (!Actor)
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_SPAWN_FAILED,
            FString::Printf(TEXT("SpawnActor returned null for %s in the %s world."), *Class->GetName(), *WorldMode));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Applied;
    for (const TPair<FProperty*, TSharedPtr<FJsonValue>>& Variable : Variables)
    {
        FString ApplyError;
        if (!ApplyJsonValueToProperty(Actor, Variable.Key, Variable.Value, ApplyError))
        {
            Ctx.SendError(ErrorCodes::ERR_TYPE_MISMATCH,
                FString::Printf(TEXT("Variable '%s' (%s) rejected the value: %s"),
                    *Variable.Key->GetName(), *Variable.Key->GetCPPType(), *ApplyError));
            return true;
        }
        Applied.Add(MakeShared<FJsonValueString>(Variable.Key->GetName()));
    }

    Actor->FinishSpawning(SpawnTransform);

    // ExecuteConstruction skips the UCS when [Kismet] bTurnOffEditorConstructionScript is set
    // (ActorConstruction.cpp); report the switch so an empty UCS result is not mistaken for a
    // script that built nothing.
    bool bUcsTurnedOff = false;
    GConfig->GetBool(TEXT("Kismet"), TEXT("bTurnOffEditorConstructionScript"), bUcsTurnedOff, GEngineIni);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("blueprintPath"), Path);
    Out->SetStringField(TEXT("class"), Class->GetPathName());
    Out->SetStringField(TEXT("world"), WorldMode);
    Out->SetStringField(TEXT("compileStatus"), StatusToString(BP->Status));
    Out->SetBoolField(TEXT("userConstructionScriptEnabled"), !bUcsTurnedOff);
    Out->SetArrayField(TEXT("variablesApplied"), Applied);
    Out->SetObjectField(TEXT("spawnTransform"), JsonBuilders::BuildTransformJson(SpawnTransform));
    DescribeConstructedActor(Actor, Out);

    Ctx.SendSuccess(Out);
    return true;
}
