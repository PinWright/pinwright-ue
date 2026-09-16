// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ActorDescribeBuilder.h"

#include "Handlers/Material/MaterialLightFunctionAtlas.h"
#include "Utils/JsonBuilders.h"
#include "Utils/PropertyUtils.h"

#include "Components/ActorComponent.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"

namespace
{
    using JsonBuilders::BuildTransformJson;
    using JsonBuilders::BuildNameArrayJson;
    using JsonBuilders::GetActorLevelPackageName;

    FString ComponentCreationMethodToString(EComponentCreationMethod Method)
    {
        switch (Method)
        {
        case EComponentCreationMethod::Native:
            return TEXT("Native");
        case EComponentCreationMethod::SimpleConstructionScript:
            return TEXT("SimpleConstructionScript");
        case EComponentCreationMethod::UserConstructionScript:
            return TEXT("UserConstructionScript");
        case EComponentCreationMethod::Instance:
            return TEXT("Instance");
        default:
            return TEXT("Unknown");
        }
    }

    TSet<FName> GetActorPropertySkipNames()
    {
        return TSet<FName>{
            TEXT("RelativeLocation"),
            TEXT("RelativeRotation"),
            TEXT("RelativeScale3D")
        };
    }

    TSet<FName> GetComponentPropertySkipNames()
    {
        return TSet<FName>{
            TEXT("RelativeLocation"),
            TEXT("RelativeRotation"),
            TEXT("RelativeScale3D")
        };
    }

    TSharedPtr<FJsonObject> BuildComponentJson(UActorComponent* Component)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Component)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("name"), Component->GetName());
        Obj->SetStringField(TEXT("path"), Component->GetPathName());
        Obj->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
        Obj->SetStringField(TEXT("creationMethod"), ComponentCreationMethodToString(Component->CreationMethod));
        Obj->SetArrayField(TEXT("tags"), BuildNameArrayJson(Component->ComponentTags));

        if (USceneComponent* Scene = Cast<USceneComponent>(Component))
        {
            Obj->SetObjectField(TEXT("relativeTransform"), BuildTransformJson(Scene->GetRelativeTransform()));
            if (USceneComponent* Parent = Scene->GetAttachParent())
            {
                Obj->SetStringField(TEXT("attachParent"), Parent->GetPathName());
                Obj->SetStringField(TEXT("attachParentName"), Parent->GetName());
                Obj->SetStringField(TEXT("attachSocket"), Scene->GetAttachSocketName().ToString());
            }
        }

        // A light function material that the light function atlas refuses still modulates
        // opaque surfaces, so LightFunctionMaterial, LightFunctionScale and every
        // r.*LightFunctionAtlas cvar read healthy while the fog / translucency /
        // single-layer-water contribution the light function was attached FOR is absent.
        // The measured verdict lives in MaterialLightFunctionAtlas; report it here, beside
        // the bare material reference, so an author reading actor.describe does not have to
        // already suspect the atlas to find material.authoring.get_material_info. Emits
        // nothing for a light with no light function, for a non-LightFunction material, or
        // on an engine with no atlas override (all three are the helper's own guards).
        if (const ULightComponent* Light = Cast<ULightComponent>(Component))
        {
            if (const UMaterialInterface* LightFunction = Light->LightFunctionMaterial)
            {
                // The domain and the atlas override both live on the base UMaterial, which
                // is what the helper measures, so an instance assigned as a light function
                // resolves to its parent material here.
                PinWright::LightFunctionAtlas::AddReportIfLightFunction(
                    Obj, LightFunction->GetMaterial());
            }
        }

        UObject* Baseline = Component->GetArchetype();
        Obj->SetObjectField(TEXT("properties"),
            BuildSparsePropertyDiffJson(Component, Baseline, GetComponentPropertySkipNames()));
        return Obj;
    }
}

TArray<TSharedPtr<FJsonValue>> ActorDescribeBuilder::BuildComponentsJson(
    AActor* Actor,
    const FComponentReadFilter& ComponentFilter)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    if (!Actor)
    {
        return Result;
    }

    TArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    Components.Sort([](const UActorComponent& A, const UActorComponent& B)
    {
        return A.GetPathName().Compare(B.GetPathName(), ESearchCase::CaseSensitive) < 0;
    });

    for (UActorComponent* Component : Components)
    {
        if (ComponentFilter.Matches(Component))
        {
            Result.Add(MakeShared<FJsonValueObject>(BuildComponentJson(Component)));
        }
    }
    return Result;
}

TSharedPtr<FJsonObject> ActorDescribeBuilder::BuildActorManifestEntry(
    AActor* Actor,
    const FString& RelativeFileName)
{
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    if (!Actor)
    {
        return Entry;
    }

    Entry->SetStringField(TEXT("storage"), TEXT("embedded"));
    Entry->SetStringField(TEXT("name"), Actor->GetName());
    Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Entry->SetStringField(TEXT("path"), Actor->GetPathName());
    Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    Entry->SetStringField(TEXT("level"), GetActorLevelPackageName(Actor));
    Entry->SetStringField(TEXT("file"), RelativeFileName);
    Entry->SetStringField(TEXT("guid"), Actor->GetActorGuid().ToString());
    return Entry;
}

TSharedPtr<FJsonObject> ActorDescribeBuilder::BuildActorJson(
    AActor* Actor,
    const FString& Storage,
    const FComponentReadFilter& ComponentFilter,
    const FActorDescribeOptions& Options)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Actor)
    {
        return Obj;
    }

    Obj->SetStringField(TEXT("schema"), TEXT("pinwright.actor-describe.v1"));
    Obj->SetStringField(TEXT("storage"), Storage);
    Obj->SetStringField(TEXT("name"), Actor->GetName());
    Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Obj->SetStringField(TEXT("path"), Actor->GetPathName());
    Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
    Obj->SetStringField(TEXT("level"), GetActorLevelPackageName(Actor));
    Obj->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
    Obj->SetStringField(TEXT("guid"), Actor->GetActorGuid().ToString());
    Obj->SetArrayField(TEXT("tags"), BuildNameArrayJson(Actor->Tags));
    Obj->SetObjectField(TEXT("transform"), BuildTransformJson(Actor->GetActorTransform()));

    // Read-shaping allow-list: when an explicit Fields projection is supplied it is
    // authoritative, lowercased once into Allowed (with the "schema"/"storage"
    // response envelope always retained). bWantKey is the single per-key inclusion
    // rule consulted before building each projectable block, so projected-away keys
    // are never constructed (rather than built-then-removed) — this matters most for
    // the expensive "properties" sparse-diff and the "components" tree.
    const bool bHasProjection = Options.HasFieldProjection();
    TSet<FString> Allowed;
    if (bHasProjection)
    {
        Allowed.Add(TEXT("schema"));
        Allowed.Add(TEXT("storage"));
        for (const FString& Key : Options.Fields)
        {
            Allowed.Add(Key.ToLower());
        }
    }
    const auto bWantKey = [&](const TCHAR* Key) -> bool
    {
        return bHasProjection ? Allowed.Contains(FString(Key).ToLower()) : true;
    };

    if (bWantKey(TEXT("properties")))
    {
        Obj->SetObjectField(TEXT("properties"),
            BuildSparsePropertyDiffJson(Actor, Actor->GetArchetype(), GetActorPropertySkipNames()));
    }

    // "components" honors the same allow-list when projecting; outside projection it
    // follows the bIncludeComponents:false shorthand.
    const bool bWantComponents = bHasProjection ? bWantKey(TEXT("components")) : Options.bIncludeComponents;
    if (bWantComponents)
    {
        Obj->SetArrayField(TEXT("components"),
            ActorDescribeBuilder::BuildComponentsJson(Actor, ComponentFilter));
    }

    // Drop the cheap identity keys the projection didn't ask for. The expensive
    // "properties"/"components" blocks above were already skipped at their source,
    // so this only prunes the always-built string fields (name/path/class/...).
    if (bHasProjection)
    {
        // Snapshot keys as FString before mutating. UE 5.8 retyped FJsonObject::Values
        // keys to UE::FSharedString, so GetKeys(TArray<FString>&) no longer matches;
        // dereferencing each key to a TCHAR* and constructing FString works on both the
        // old FString key type and the new FSharedString one.
        TArray<FString> PresentKeys;
        PresentKeys.Reserve(Obj->Values.Num());
        for (const auto& Kvp : Obj->Values)
        {
            PresentKeys.Emplace(*Kvp.Key);
        }
        for (const FString& Key : PresentKeys)
        {
            if (!Allowed.Contains(Key.ToLower()))
            {
                Obj->RemoveField(Key);
            }
        }
    }

    return Obj;
}
