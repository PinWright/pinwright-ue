// Copyright (c) 2026 Alexander Penkin. MIT License.

// Implementation of the live-instance precondition. The full engine evidence and the
// two rejected alternatives are in BlueprintReinstancingGuard.h; this file only
// measures, formats and refuses.

#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"

namespace BlueprintReinstancingGuard
{
namespace
{
    // Only the worlds where a destroy-and-respawn is either observable to another
    // operator (a loaded map) or actively ticking (PIE / Game). See the SCOPE note on
    // SurveyLiveInstances for why EditorPreview and Inactive are out.
    const TCHAR* SurveyedWorldTypeLabel(const UWorld* World)
    {
        if (!World)
        {
            return nullptr;
        }
        switch (World->WorldType)
        {
        case EWorldType::Editor: return TEXT("Editor");
        case EWorldType::PIE:    return TEXT("PIE");
        case EWorldType::Game:   return TEXT("Game");
        default:                 return nullptr;
        }
    }

    FString WorldDisplayName(const UWorld* World)
    {
        if (const UPackage* Package = World ? World->GetPackage() : nullptr)
        {
            return Package->GetName();
        }
        return World ? World->GetName() : FString();
    }
}

FLiveInstanceSurvey SurveyLiveInstances(const UBlueprint* Blueprint)
{
    FLiveInstanceSurvey Survey;
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        return Survey;
    }

    Survey.bPieActive = (GEditor && GEditor->PlayWorld != nullptr);

    TArray<UObject*> Instances;
    GetObjectsOfClass(Blueprint->GeneratedClass, Instances, /*bIncludeDerivedClasses=*/true,
                      RF_ClassDefaultObject | RF_ArchetypeObject,
                      EInternalObjectFlags::Garbage);

    TMap<const UWorld*, int32> WorldToEntry;
    for (UObject* Instance : Instances)
    {
        if (!IsValid(Instance))
        {
            continue;
        }

        const UWorld* World = Instance->GetTypedOuter<UWorld>();
        const TCHAR* TypeLabel = SurveyedWorldTypeLabel(World);
        if (!TypeLabel)
        {
            continue;
        }

        int32* EntryIndex = WorldToEntry.Find(World);
        if (!EntryIndex)
        {
            FWorldInstanceCount Entry;
            Entry.WorldName = WorldDisplayName(World);
            Entry.WorldType = TypeLabel;
            EntryIndex = &WorldToEntry.Add(World, Survey.Worlds.Add(MoveTemp(Entry)));
        }

        FWorldInstanceCount& Entry = Survey.Worlds[*EntryIndex];
        ++Entry.InstanceCount;
        ++Survey.InstanceCount;
        if (Instance->IsA<AActor>())
        {
            ++Entry.ActorCount;
            ++Survey.ActorCount;
        }
    }

    // Deterministic order so a response diff and a test assertion do not depend on
    // UObject hash iteration order.
    Survey.Worlds.Sort([](const FWorldInstanceCount& A, const FWorldInstanceCount& B)
    {
        return A.WorldName < B.WorldName;
    });

    return Survey;
}

void AddSurveyToJson(const FLiveInstanceSurvey& Survey, const TSharedPtr<FJsonObject>& Out)
{
    if (!Out.IsValid() || Survey.IsEmpty())
    {
        return;
    }

    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    Block->SetNumberField(TEXT("count"), Survey.InstanceCount);
    Block->SetNumberField(TEXT("actorCount"), Survey.ActorCount);
    Block->SetBoolField(TEXT("pieActive"), Survey.bPieActive);

    TArray<TSharedPtr<FJsonValue>> WorldsArray;
    for (const FWorldInstanceCount& Entry : Survey.Worlds)
    {
        TSharedPtr<FJsonObject> WorldObj = MakeShared<FJsonObject>();
        WorldObj->SetStringField(TEXT("world"), Entry.WorldName);
        WorldObj->SetStringField(TEXT("worldType"), Entry.WorldType);
        WorldObj->SetNumberField(TEXT("count"), Entry.InstanceCount);
        WorldObj->SetNumberField(TEXT("actorCount"), Entry.ActorCount);
        WorldsArray.Add(MakeShared<FJsonValueObject>(WorldObj));
    }
    Block->SetArrayField(TEXT("worlds"), WorldsArray);

    Out->SetObjectField(TEXT("reinstanced"), Block);
}

FString DescribeSurvey(const FLiveInstanceSurvey& Survey)
{
    if (Survey.IsEmpty())
    {
        return FString();
    }

    TArray<FString> WorldParts;
    WorldParts.Reserve(Survey.Worlds.Num());
    for (const FWorldInstanceCount& Entry : Survey.Worlds)
    {
        WorldParts.Add(FString::Printf(TEXT("%s (%s) x%d"),
                                       *Entry.WorldName, *Entry.WorldType, Entry.InstanceCount));
    }

    return FString::Printf(
        TEXT("%d live instance(s) (%d placed actor(s)) in %d loaded world(s): %s"),
        Survey.InstanceCount, Survey.ActorCount, Survey.Worlds.Num(),
        *FString::Join(WorldParts, TEXT(", ")));
}

bool RefuseIfLiveInstancesWouldBeReinstanced(
    FHandlerContext& Ctx,
    const UBlueprint* Blueprint,
    const TCHAR* Verb)
{
    if (Ctx.GetBool(AllowReinstancingParamName(), false))
    {
        return false;
    }

    return RefuseIfLiveInstancesWouldBeReinstanced(
        Ctx, Blueprint, SurveyLiveInstances(Blueprint), Verb);
}

bool RefuseIfLiveInstancesWouldBeReinstanced(
    FHandlerContext& Ctx,
    const UBlueprint* Blueprint,
    const FLiveInstanceSurvey& Survey,
    const TCHAR* Verb)
{
    if (Ctx.GetBool(AllowReinstancingParamName(), false))
    {
        return false;
    }

    if (Survey.IsEmpty())
    {
        return false;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), Blueprint ? Blueprint->GetPathName() : FString());
    AddSurveyToJson(Survey, Result);

    Ctx.SendError(
        ErrorCodes::ERR_LIVE_INSTANCES_WOULD_BE_REINSTANCED,
        FString::Printf(
            TEXT("%s would compile '%s', and the compile flushes the Blueprint reinstancing queue: ")
            TEXT("%s would be destroyed and re-created, and each owning level marked dirty. In a ")
            TEXT("shared editor those instances can belong to another agent's open map. Re-issue with ")
            TEXT("%s=true to accept the rebuild, or stop PIE / close the map first."),
            Verb ? Verb : TEXT("This verb"),
            Blueprint ? *Blueprint->GetPathName() : TEXT("<null>"),
            *DescribeSurvey(Survey),
            AllowReinstancingParamName()),
        Result);
    return true;
}

}  // namespace BlueprintReinstancingGuard
