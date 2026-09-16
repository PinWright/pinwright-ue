// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWidgetAnimation;
class UWidgetBlueprint;

namespace WidgetAnimationEventIntrospection
{
    void AppendAnimationEventMetadata(
        UWidgetBlueprint* WidgetBlueprint,
        UWidgetAnimation* Animation,
        const TSharedPtr<FJsonObject>& AnimationObject);
}
