// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UMovieScene;
class UMovieScene2DTransformSection;
class UMovieScene2DTransformTrack;
class UMovieSceneByteSection;
class UMovieSceneByteTrack;
class UMovieSceneColorSection;
class UMovieSceneColorTrack;
class UMovieSceneDoubleVectorSection;
class UMovieSceneDoubleVectorTrack;
class UMovieSceneFloatSection;
class UMovieSceneFloatTrack;
class UMovieSceneFloatVectorSection;
class UMovieSceneFloatVectorTrack;
class UMovieSceneParameterSection;
class UMovieSceneTextSection;
class UMovieSceneTextTrack;
class UMovieSceneWidgetMaterialTrack;
class UImage;
class UTextBlock;
class UWidget;
class UWidgetAnimation;
class UWidgetBlueprint;

namespace WidgetAnimationJsonTestUtils
{
    struct FWidgetAnimationFixture
    {
        FString WidgetPath;
        UWidgetBlueprint* WidgetBlueprint = nullptr;
        UWidget* RootWidget = nullptr;
        UTextBlock* AnimatedWidget = nullptr;
        UWidgetAnimation* Animation = nullptr;
        UMovieScene* MovieScene = nullptr;
        UMovieSceneFloatTrack* FloatTrack = nullptr;
        UMovieSceneFloatSection* FloatSection = nullptr;
        FGuid BindingGuid;
    };

    struct FCoreUmgAnimationFixture
    {
        FString WidgetPath;
        UWidgetBlueprint* WidgetBlueprint = nullptr;
        UImage* MaterialWidget = nullptr;
        UTextBlock* TextWidget = nullptr;
        UWidgetAnimation* Animation = nullptr;
        UMovieScene* MovieScene = nullptr;
        UMovieSceneWidgetMaterialTrack* MaterialTrack = nullptr;
        UMovieSceneParameterSection* MaterialSection = nullptr;
        UMovieScene2DTransformTrack* TransformTrack = nullptr;
        UMovieScene2DTransformSection* TransformSection = nullptr;
        UMovieSceneTextTrack* TextTrack = nullptr;
        UMovieSceneTextSection* TextSection = nullptr;
        FGuid MaterialBindingGuid;
        FGuid TextBindingGuid;
    };

    struct FCommonPropertyAnimationFixture
    {
        FString WidgetPath;
        UWidgetBlueprint* WidgetBlueprint = nullptr;
        UImage* AnimatedImage = nullptr;
        UWidgetAnimation* Animation = nullptr;
        UMovieScene* MovieScene = nullptr;
        UMovieSceneColorTrack* ColorTrack = nullptr;
        UMovieSceneColorSection* ColorSection = nullptr;
        UMovieSceneByteTrack* ByteTrack = nullptr;
        UMovieSceneByteSection* ByteSection = nullptr;
        UMovieSceneFloatVectorTrack* FloatVectorTrack = nullptr;
        UMovieSceneFloatVectorSection* FloatVectorSection = nullptr;
        UMovieSceneDoubleVectorTrack* DoubleVectorTrack = nullptr;
        UMovieSceneDoubleVectorSection* DoubleVectorSection = nullptr;
        FGuid BindingGuid;
    };

    FString MakeWidgetAnimationJsonTestAssetPath(const FString& Prefix);
    UWidgetBlueprint* CreateTransientWidgetBlueprint(const FString& PackagePath);
    UTextBlock* AddNamedTextBlock(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName);
    UImage* AddNamedImage(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName);
    bool AddFloatAnimationFixture(
        UWidgetBlueprint* WidgetBlueprint,
        const FName& AnimationName,
        const FName& WidgetName,
        FWidgetAnimationFixture& OutFixture);
    FWidgetAnimationFixture CreateFloatAnimationFixture(const FString& Prefix);
    FCoreUmgAnimationFixture CreateCoreUmgAnimationFixture(const FString& Prefix);
    FCommonPropertyAnimationFixture CreateCommonPropertyAnimationFixture(const FString& Prefix);
}
