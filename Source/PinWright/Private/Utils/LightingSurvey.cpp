// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/LightingSurvey.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/SkyLightComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace PinWrightLightingSurvey
{
    static void AccumulateLevel(const ULevel* Level, FLightingSurvey& Out)
    {
        if (!Level)
        {
            return;
        }
        TInlineComponentArray<ULightComponentBase*> Lights;
        for (AActor* Actor : Level->Actors)
        {
            if (!IsValid(Actor))
            {
                continue;
            }
            Actor->GetComponents(Lights);
            for (const ULightComponentBase* Light : Lights)
            {
                if (!IsValid(Light) || !Light->bAffectsWorld || !Light->IsVisible())
                {
                    continue;
                }
                if (Light->IsA<UDirectionalLightComponent>())
                {
                    ++Out.DirectionalLights;
                }
                else if (Light->IsA<USkyLightComponent>())
                {
                    ++Out.SkyLights;
                }
                else
                {
                    ++Out.LocalLights;
                }
            }
        }
    }

    FLightingSurvey SurveyLevel(const ULevel* Level)
    {
        FLightingSurvey Survey;
        AccumulateLevel(Level, Survey);
        return Survey;
    }

    FLightingSurvey SurveyWorld(const UWorld* World)
    {
        FLightingSurvey Survey;
        if (!World)
        {
            return Survey;
        }
        for (const ULevel* Level : World->GetLevels())
        {
            // A hidden streaming level draws nothing, so its lights light nothing in this frame.
            if (Level && Level->bIsVisible)
            {
                AccumulateLevel(Level, Survey);
            }
        }
        return Survey;
    }

    void AddLightingFields(const FLightingSurvey& Survey, const FString& LevelPath,
                           const TSharedPtr<FJsonObject>& Result,
                           const FLightingSurvey* WorldSurvey)
    {
        if (!Result.IsValid())
        {
            return;
        }
        TSharedPtr<FJsonObject> Lighting = MakeShared<FJsonObject>();
        Lighting->SetNumberField(TEXT("directionalLights"), Survey.DirectionalLights);
        Lighting->SetNumberField(TEXT("skyLights"), Survey.SkyLights);
        Lighting->SetNumberField(TEXT("localLights"), Survey.LocalLights);
        Lighting->SetNumberField(TEXT("lightComponents"), Survey.Total());
        if (WorldSurvey)
        {
            Lighting->SetNumberField(TEXT("worldLightComponents"), WorldSurvey->Total());
        }
        Result->SetObjectField(TEXT("lighting"), Lighting);
        if ((WorldSurvey ? WorldSurvey->Total() : Survey.Total()) == 0)
        {
            Result->SetStringField(TEXT("lightingWarning"), FString::Printf(
                TEXT("level '%s' renders with no lighting: no DirectionalLight, SkyLight or other ")
                TEXT("light component with bAffectsWorld in any visible level of its world. A dark ")
                TEXT("frame here is the scene, not the capture. Add a light, or treat the luminance ")
                TEXT("as unlit."),
                *LevelPath));
        }
    }
}
