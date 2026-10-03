// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class ULevel;
class UWorld;

// How many light components a level carries that can actually light it.
//
// WHY THIS EXISTS (board B-unlit-level-capture-no-warning). A level with no SkyLight and no
// DirectionalLight captured at mean luminance 0.20 against 0.59 for a lit map, with `blank: false`
// and no warning: every field in the response was individually true and the aggregate misleading.
// A dark frame has several causes (no lights, an exposure pin, a wrong view mode, a non-realtime
// viewport) and luminance alone cannot separate them. "This level has zero lights" is a fact about
// the level, not a heuristic about the frame, so it is counted and stated rather than inferred.
//
// WHAT COUNTS: every ULightComponentBase whose bAffectsWorld is set and which is visible. That base
// covers directional, point, spot and rect lights AND USkyLightComponent (which is not a
// ULightComponent). A disabled light (bAffectsWorld false) contributes nothing to the scene, which
// is the engine's own definition (LightComponentBase.h), so it is not counted. Emissive materials
// and fog are NOT lights and are deliberately not inferred from.
namespace PinWrightLightingSurvey
{
    struct FLightingSurvey
    {
        int32 DirectionalLights = 0;
        int32 SkyLights = 0;
        // Point, spot and rect lights.
        int32 LocalLights = 0;

        int32 Total() const { return DirectionalLights + SkyLights + LocalLights; }
    };

    // One level's actors.
    FLightingSurvey SurveyLevel(const ULevel* Level);

    // Every VISIBLE level of the world, which is what a viewport of that world renders.
    FLightingSurvey SurveyWorld(const UWorld* World);

    // Writes the `lighting` block {directionalLights, skyLights, localLights, lightComponents},
    // counting components with bAffectsWorld set and visible, unconditionally, plus `lightingWarning` naming LevelPath when the count is zero.
    //
    // WorldSurvey: pass SurveyWorld when Survey covers ONE level of a world (level.get_info). A
    // persistent level lit from a visible sublevel is not unlit, so the warning is then decided
    // from the world and the block gains `worldLightComponents`. Null when Survey already is
    // the world (render.capture_open_level).
    void AddLightingFields(const FLightingSurvey& Survey, const FString& LevelPath,
                           const TSharedPtr<FJsonObject>& Result,
                           const FLightingSurvey* WorldSurvey = nullptr);
}
