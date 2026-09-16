// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "Utils/ActorUtils.h"
#include "Utils/PropertyUtils.h"
#include "ScopedTransaction.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"


namespace
{
    FString MakeComponentNameKey(const FString& Name)
    {
        return Name.ToLower();
    }

    void SeedActorComponentNameMap(AActor* Actor, TMap<FString, UActorComponent*>& ComponentNames)
    {
        if (!Actor)
        {
            return;
        }

        TInlineComponentArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (Component)
            {
                const FString NameKey = MakeComponentNameKey(Component->GetName());
                if (!ComponentNames.Contains(NameKey))
                {
                    ComponentNames.Add(NameKey, Component);
                }
            }
        }
    }

    UActorComponent* FindActorComponentInMap(const TMap<FString, UActorComponent*>& ComponentNames, const FString& Name)
    {
        if (UActorComponent* const* Found = ComponentNames.Find(MakeComponentNameKey(Name)))
        {
            return *Found;
        }
        return nullptr;
    }

    FString GenerateActorComponentDuplicateName(AActor* Actor, UClass* ComponentClass,
        const TMap<FString, UActorComponent*>& ComponentNames, const FString& SourceName)
    {
        const FString BaseName = FString::Printf(TEXT("%s_Copy"), *SourceName);
        FString Candidate = MakeUniqueObjectName(Actor, ComponentClass, FName(*BaseName)).ToString();
        int32 Suffix = 1;
        while (ComponentNames.Contains(MakeComponentNameKey(Candidate)))
        {
            const FString FallbackBase = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
            Candidate = MakeUniqueObjectName(Actor, ComponentClass, FName(*FallbackBase)).ToString();
        }
        return Candidate;
    }

    void AddComponentMapping(TSharedPtr<FJsonObject> Mapping, UActorComponent* Source, UActorComponent* Target)
    {
        if (Mapping.IsValid() && Source && Target)
        {
            Mapping->SetStringField(Source->GetName(), Target->GetName());
        }
    }

    void CopyActorComponentProperties(UActorComponent* SourceComponent, UActorComponent* TargetComponent)
    {
        if (!SourceComponent || !TargetComponent)
        {
            return;
        }

        FFilteredPropertyCopyOptions Options;
        AddSceneAttachmentPropertySkips(Options);
        CopyFilteredMatchingProperties(SourceComponent, TargetComponent, Options);

        TargetComponent->ComponentTags = SourceComponent->ComponentTags;
        if (USceneComponent* SourceScene = Cast<USceneComponent>(SourceComponent))
        {
            if (USceneComponent* TargetScene = Cast<USceneComponent>(TargetComponent))
            {
                TargetScene->SetRelativeTransform(SourceScene->GetRelativeTransform());
                TargetScene->SetMobility(SourceScene->Mobility);
            }
        }
    }

    struct FActorComponentDuplicateOptions
    {
        FString DesiredName;
        USceneComponent* TargetParent = nullptr;
        bool bDuplicateChildren = true;
        bool bCopyProperties = true;
        bool bCopyAttachment = true;
    };

    struct FActorComponentDuplicateContext
    {
        TSharedPtr<FJsonObject> Mapping;
        TArray<FString>* Warnings = nullptr;
        TMap<FString, UActorComponent*> ComponentNames;
    };

    UActorComponent* DuplicateActorComponent(AActor* Actor, UActorComponent* SourceComponent,
        const FActorComponentDuplicateOptions& Options, FActorComponentDuplicateContext& DuplicateContext,
        FString& OutError)
    {
        if (!Actor || !SourceComponent)
        {
            OutError = TEXT("Invalid actor or source component");
            return nullptr;
        }

        const FString TargetName = Options.DesiredName.IsEmpty()
            ? GenerateActorComponentDuplicateName(Actor, SourceComponent->GetClass(), DuplicateContext.ComponentNames, SourceComponent->GetName())
            : Options.DesiredName;

        const FString TargetNameKey = MakeComponentNameKey(TargetName);
        if (DuplicateContext.ComponentNames.Contains(TargetNameKey))
        {
            OutError = FString::Printf(TEXT("Component with name '%s' already exists"), *TargetName);
            return nullptr;
        }

        UActorComponent* NewComponent = NewObject<UActorComponent>(Actor, SourceComponent->GetClass(), FName(*TargetName), RF_Transactional);
        if (!NewComponent)
        {
            OutError = FString::Printf(TEXT("Failed to create component '%s'"), *TargetName);
            return nullptr;
        }

        NewComponent->CreationMethod = EComponentCreationMethod::Instance;
        NewComponent->SetFlags(RF_Transactional);
        Actor->AddInstanceComponent(NewComponent);
        DuplicateContext.ComponentNames.Add(MakeComponentNameKey(NewComponent->GetName()), NewComponent);
        NewComponent->OnComponentCreated();

        if (Options.bCopyProperties)
        {
            CopyActorComponentProperties(SourceComponent, NewComponent);
        }

        if (USceneComponent* NewScene = Cast<USceneComponent>(NewComponent))
        {
            USceneComponent* AttachParent = nullptr;
            FName AttachSocket = NAME_None;

            if (Options.bCopyAttachment)
            {
                if (Options.TargetParent)
                {
                    AttachParent = Options.TargetParent;
                    if (USceneComponent* SourceScene = Cast<USceneComponent>(SourceComponent))
                    {
                        AttachSocket = SourceScene->GetAttachSocketName();
                    }
                }
                else if (USceneComponent* SourceScene = Cast<USceneComponent>(SourceComponent))
                {
                    AttachParent = SourceScene->GetAttachParent();
                    AttachSocket = SourceScene->GetAttachSocketName();
                }
            }

            if (AttachParent)
            {
                NewScene->SetupAttachment(AttachParent, AttachSocket);
            }
            if (USceneComponent* SourceScene = Cast<USceneComponent>(SourceComponent))
            {
                NewScene->SetRelativeTransform(SourceScene->GetRelativeTransform());
            }
        }
        else if (Options.TargetParent && DuplicateContext.Warnings)
        {
            DuplicateContext.Warnings->Add(FString::Printf(TEXT("Component '%s' is not a SceneComponent; targetParentName was ignored"), *NewComponent->GetName()));
        }

        if (SourceComponent->IsRegistered())
        {
            NewComponent->RegisterComponent();
        }
        if (USceneComponent* NewScene = Cast<USceneComponent>(NewComponent))
        {
            NewScene->UpdateComponentToWorld();
        }

        AddComponentMapping(DuplicateContext.Mapping, SourceComponent, NewComponent);

        if (Options.bDuplicateChildren)
        {
            if (USceneComponent* SourceScene = Cast<USceneComponent>(SourceComponent))
            {
                USceneComponent* NewSceneParent = Cast<USceneComponent>(NewComponent);
                for (USceneComponent* SourceChild : SourceScene->GetAttachChildren())
                {
                    if (!SourceChild || SourceChild->GetOwner() != Actor)
                    {
                        continue;
                    }

                    // Skip engine-auto-managed editor-only visualization children (the
                    // transient UBillboardComponent sprite USceneComponent::CreateSpriteComponent
                    // auto-attaches to lights/audio/etc. in the editor). These are not part of
                    // the authored component graph: the newly-duplicated component regenerates
                    // its own sprite on RegisterComponent(), so cloning the source's sprite
                    // produces a redundant, persistent instance component (two billboards per
                    // duplicated light). This mirrors the engine's own skip idiom
                    // (IsEditorOnly() || IsVisualizationComponent()).
                    if (SourceChild->IsVisualizationComponent() || SourceChild->IsEditorOnly())
                    {
                        continue;
                    }

                    FString ChildError;
                    FActorComponentDuplicateOptions ChildOptions = Options;
                    ChildOptions.DesiredName.Reset();
                    ChildOptions.TargetParent = NewSceneParent;
                    UActorComponent* NewChild = DuplicateActorComponent(Actor, SourceChild, ChildOptions, DuplicateContext, ChildError);
                    if (!NewChild && DuplicateContext.Warnings)
                    {
                        DuplicateContext.Warnings->Add(ChildError);
                    }
                }
            }
        }

        NewComponent->MarkPackageDirty();
        return NewComponent;
    }
}

REGISTER_RPC_HANDLER("actor.duplicate_component", "actor", "Duplicate an instance-level component on a placed actor, optionally copying properties, attachment, transform, tags, and child components.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the target actor."),
        RPC_PARAM_OPT("sourceName", "string", "Name of the source component."),
        RPC_PARAM_OPT("componentName", "string", "Alias for sourceName."),
        RPC_PARAM_OPT("newName", "string", "Name for the duplicated root component; generated when omitted."),
        RPC_PARAM_OPT("targetParentName", "string", "Target parent scene component; defaults to the source component's current parent."),
        RPC_PARAM_OPT("duplicateChildren", "boolean", "Whether to recursively duplicate attached child components. Defaults true."),
        RPC_PARAM_OPT("copyProperties", "boolean", "Whether to copy safe editable component properties. Defaults true."),
        RPC_PARAM_OPT("copyAttachment", "boolean", "Whether to preserve attachment/socket metadata. Defaults true.")
    ))
{
    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName))
    {
        return true;
    }

    const FString SourceName = Ctx.GetStringFirstOf({TEXT("sourceName"), TEXT("componentName")});
    if (SourceName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sourceName is required"));
        return true;
    }

    AActor* Actor = McpActorUtils::FindActorByName(nullptr, ActorName);
    if (!Actor)
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return true;
    }

    TMap<FString, UActorComponent*> ComponentNames;
    SeedActorComponentNameMap(Actor, ComponentNames);

    UActorComponent* SourceComponent = FindActorComponentInMap(ComponentNames, SourceName);
    if (!SourceComponent)
    {
        Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), FString::Printf(TEXT("Source component not found: %s"), *SourceName));
        return true;
    }

    USceneComponent* TargetParent = nullptr;
    const FString TargetParentName = Ctx.GetString(TEXT("targetParentName"));
    if (!TargetParentName.IsEmpty())
    {
        TargetParent = Cast<USceneComponent>(FindActorComponentInMap(ComponentNames, TargetParentName));
        if (!TargetParent)
        {
            Ctx.SendError(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("Target parent scene component not found: %s"), *TargetParentName));
            return true;
        }
    }

    const bool bDuplicateChildren = Ctx.GetBool(TEXT("duplicateChildren"), true);
    const bool bCopyProperties = Ctx.GetBool(TEXT("copyProperties"), true);
    const bool bCopyAttachment = Ctx.GetBool(TEXT("copyAttachment"), true);

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: actor.duplicate_component")));
    Actor->Modify();

    TSharedPtr<FJsonObject> Mapping = MakeShared<FJsonObject>();
    TArray<FString> Warnings;
    FString DuplicateError;
    FActorComponentDuplicateOptions DuplicateOptions;
    DuplicateOptions.DesiredName = Ctx.GetString(TEXT("newName"));
    DuplicateOptions.TargetParent = TargetParent;
    DuplicateOptions.bDuplicateChildren = bDuplicateChildren;
    DuplicateOptions.bCopyProperties = bCopyProperties;
    DuplicateOptions.bCopyAttachment = bCopyAttachment;
    FActorComponentDuplicateContext DuplicateContext;
    DuplicateContext.Mapping = Mapping;
    DuplicateContext.Warnings = &Warnings;
    DuplicateContext.ComponentNames = ComponentNames;
    UActorComponent* NewComponent = DuplicateActorComponent(Actor, SourceComponent, DuplicateOptions, DuplicateContext, DuplicateError);
    if (!NewComponent)
    {
        Ctx.SendError(TEXT("DUPLICATE_FAILED"), DuplicateError);
        return true;
    }

    Actor->MarkPackageDirty();

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("sourceName"), SourceComponent->GetName());
    Result->SetStringField(TEXT("componentName"), NewComponent->GetName());
    Result->SetStringField(TEXT("componentClass"), NewComponent->GetClass()->GetPathName());
    Result->SetObjectField(TEXT("mapping"), Mapping);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Result->SetBoolField(TEXT("compileRequired"), false);
    Result->SetBoolField(TEXT("saveRequired"), true);
    AddActorVerification(Result, Actor);
    if (USceneComponent* NewScene = Cast<USceneComponent>(NewComponent))
    {
        AddComponentVerification(Result, NewScene);
    }
    Ctx.SendSuccess(Result);
    return true;
}
