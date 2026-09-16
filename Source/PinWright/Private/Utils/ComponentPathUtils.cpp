// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ComponentPathUtils.h"

#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace PinWrightRpc::ComponentPath
{
    namespace
    {
        UActorComponent* ResolveBlueprintCdoSubobject(const FString& AssetPart, const FString& Subobject, FString& OutError)
        {
            UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPart);
            if (!Loaded)
            {
                OutError = TEXT("COMPONENT_NOT_FOUND");
                return nullptr;
            }

            UClass* OwnerClass = nullptr;
            if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
            {
                OwnerClass = BP->GeneratedClass;
            }
            else if (UClass* AsClass = Cast<UClass>(Loaded))
            {
                OwnerClass = AsClass;
            }
            else if (Loaded->HasAnyFlags(RF_ClassDefaultObject))
            {
                // blueprint.scs.get and asset.dump emit component object-refs in the
                // "<BP>.Default__<BP>_C:<Sub>" shape, whose left half loads the CDO INSTANCE
                // rather than the UBlueprint or its generated class. Without this redirect the
                // exact path those read RPCs hand back is rejected COMPONENT_NOT_FOUND by every
                // vehicle.* write verb. Mirrors the UClass->CDO redirect in the property.*
                // resolver, in the opposite direction.
                OwnerClass = Loaded->GetClass();
            }

            if (!OwnerClass)
            {
                OutError = TEXT("COMPONENT_NOT_FOUND");
                return nullptr;
            }

            UObject* CDO = OwnerClass->GetDefaultObject(/*bCreateIfNeeded*/ true);
            if (!CDO)
            {
                OutError = TEXT("COMPONENT_NOT_FOUND");
                return nullptr;
            }

            UObject* Subobj = CDO->GetDefaultSubobjectByName(FName(*Subobject));
            if (!Subobj)
            {
                OutError = TEXT("COMPONENT_NOT_FOUND");
                return nullptr;
            }

            UActorComponent* AsComponent = Cast<UActorComponent>(Subobj);
            if (!AsComponent)
            {
                OutError = TEXT("NOT_AN_ACTORCOMPONENT");
                return nullptr;
            }
            return AsComponent;
        }

        UActorComponent* FindLiveComponent(const FString& ActorPart, const FString& ComponentPart, FString& OutError)
        {
            UWorld* World = nullptr;
            if (GEditor)
            {
                if (GEditor->PlayWorld)
                {
                    World = GEditor->PlayWorld;
                }
                else
                {
                    World = GEditor->GetEditorWorldContext().World();
                }
            }
            if (!World)
            {
                OutError = TEXT("COMPONENT_NOT_FOUND");
                return nullptr;
            }

            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* A = *It;
                if (!A) continue;
                const bool bActorMatch =
                    A->GetActorLabel().Equals(ActorPart, ESearchCase::IgnoreCase) ||
                    A->GetName().Equals(ActorPart, ESearchCase::IgnoreCase);
                if (!bActorMatch) continue;

                TArray<UActorComponent*> Components;
                A->GetComponents(Components);
                for (UActorComponent* Comp : Components)
                {
                    if (!Comp) continue;
                    if (Comp->GetName().Equals(ComponentPart, ESearchCase::IgnoreCase))
                    {
                        return Comp;
                    }
                }
            }
            OutError = TEXT("COMPONENT_NOT_FOUND");
            return nullptr;
        }
    }

    UActorComponent* Resolve(const FString& Path, FString& OutError)
    {
        if (Path.IsEmpty())
        {
            OutError = TEXT("INVALID_PATH");
            return nullptr;
        }

        // Form A: "<AssetPath>:<Subobject>" — BP CDO subobject.
        // The "/Game/.../BP_X.BP_X_C:Sub" style always contains a ':' AND a '/'.
        int32 ColonIdx = INDEX_NONE;
        if (Path.FindLastChar(TEXT(':'), ColonIdx) && ColonIdx > 0)
        {
            const FString Left = Path.Left(ColonIdx);
            const FString Right = Path.Mid(ColonIdx + 1);
            if (Right.IsEmpty())
            {
                OutError = TEXT("INVALID_PATH");
                return nullptr;
            }

            if (Left.StartsWith(TEXT("/")))
            {
                return ResolveBlueprintCdoSubobject(Left, Right, OutError);
            }
            // "ActorLabel:ComponentName" — live actor.
            return FindLiveComponent(Left, Right, OutError);
        }

        // Form B: "ActorName.ComponentName" — split on the last '.'.
        int32 DotIdx = INDEX_NONE;
        if (Path.FindLastChar(TEXT('.'), DotIdx) && DotIdx > 0 && DotIdx < Path.Len() - 1)
        {
            const FString Left = Path.Left(DotIdx);
            const FString Right = Path.Mid(DotIdx + 1);
            return FindLiveComponent(Left, Right, OutError);
        }

        OutError = TEXT("INVALID_PATH");
        return nullptr;
    }
}
