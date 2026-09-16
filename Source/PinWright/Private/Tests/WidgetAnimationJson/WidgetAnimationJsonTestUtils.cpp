// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "WidgetAnimationJsonTestUtils.h"


#include "Animation/WidgetAnimation.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/SlateWrapperTypes.h"
#include "Components/TextBlock.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
// The MovieSceneText headers moved into Channels/Sections/Tracks subfolders (MovieSceneTracks
// module) in UE 5.7; the old flat paths remain only as deprecation stubs. Prefer the new
// locations when present, fall back to the flat form for UE 5.6 and earlier.
#if __has_include("Channels/MovieSceneTextChannel.h")
#include "Channels/MovieSceneTextChannel.h"
#else
#include "MovieSceneTextChannel.h"
#endif
#if __has_include("Sections/MovieSceneTextSection.h")
#include "Sections/MovieSceneTextSection.h"
#else
#include "MovieSceneTextSection.h"
#endif
#if __has_include("Tracks/MovieSceneTextTrack.h")
#include "Tracks/MovieSceneTextTrack.h"
#else
#include "MovieSceneTextTrack.h"
#endif
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Misc/EngineVersionComparison.h"

namespace WidgetAnimationJsonTestUtils
{
    namespace
    {
        // UE 5.6 added UWidgetBlueprint::OnVariableAdded (and WidgetVariableNameToGuidMap).
        // On UE 5.4/5.5 there is no such API; the test fixtures don't depend on the GUID map,
        // so registration is a clean no-op there.
        void RegisterWidgetVariable(UWidgetBlueprint* WidgetBlueprint, const FName& VariableName)
        {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
            (void)WidgetBlueprint;
            (void)VariableName;
#else
            if (WidgetBlueprint)
            {
                WidgetBlueprint->OnVariableAdded(VariableName);
            }
#endif
        }
    }

    FString MakeWidgetAnimationJsonTestAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWidgetBlueprint* CreateTransientWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UWidgetBlueprint* WidgetBlueprint = NewObject<UWidgetBlueprint>(
            Package, *AssetName, RF_Transient | RF_Public | RF_Standalone);
        if (!WidgetBlueprint)
        {
            return nullptr;
        }

        WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, NAME_None, RF_Transient);
        WidgetBlueprint->Status = BS_BeingCreated;
        return WidgetBlueprint;
    }

    UTextBlock* AddNamedTextBlock(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            return nullptr;
        }

        UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
        if (!RootCanvas)
        {
            RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
                UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
            RegisterWidgetVariable(WidgetBlueprint, RootCanvas->GetFName());
        }

        UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(
            UTextBlock::StaticClass(), WidgetName);
        if (!TextBlock)
        {
            return nullptr;
        }

        RootCanvas->AddChild(TextBlock);
        RegisterWidgetVariable(WidgetBlueprint, TextBlock->GetFName());
        return TextBlock;
    }

    UImage* AddNamedImage(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            return nullptr;
        }

        UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
        if (!RootCanvas)
        {
            RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
                UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
            RegisterWidgetVariable(WidgetBlueprint, RootCanvas->GetFName());
        }

        UImage* Image = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(
            UImage::StaticClass(), WidgetName);
        if (!Image)
        {
            return nullptr;
        }

        RootCanvas->AddChild(Image);
        RegisterWidgetVariable(WidgetBlueprint, Image->GetFName());
        return Image;
    }

    bool AddFloatAnimationFixture(
        UWidgetBlueprint* WidgetBlueprint,
        const FName& AnimationName,
        const FName& WidgetName,
        FWidgetAnimationFixture& OutFixture)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            return false;
        }

        UTextBlock* TargetWidget = Cast<UTextBlock>(WidgetBlueprint->WidgetTree->FindWidget(WidgetName));
        if (!TargetWidget)
        {
            TargetWidget = AddNamedTextBlock(WidgetBlueprint, WidgetName);
        }
        if (!TargetWidget)
        {
            return false;
        }

        UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(
            WidgetBlueprint, AnimationName, RF_Transient | RF_Transactional);
        // The MovieScene takes the animation's own FName. The UMG compiler generates the
        // animation's property from the ANIMATION's name while UWidgetBlueprintGeneratedClass
        // binds that property by the MOVIE SCENE's name, so any other MovieScene name produces
        // an animation that compiles clean, reads null on the generated property and never
        // plays. Fixtures must model the shape the authoring path produces, not a broken one.
        UMovieScene* MovieScene = Animation
            ? NewObject<UMovieScene>(Animation, Animation->GetFName(), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Animation || !MovieScene)
        {
            return false;
        }

        Animation->MovieScene = MovieScene;
        MovieScene->SetDisplayRate(FFrameRate(30, 1));
        MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));

        const FGuid BindingGuid = MovieScene->AddPossessable(TargetWidget->GetFName().ToString(), TargetWidget->GetClass());
        if (!BindingGuid.IsValid())
        {
            return false;
        }

        FWidgetAnimationBinding Binding;
        Binding.WidgetName = TargetWidget->GetFName();
        Binding.AnimationGuid = BindingGuid;
        Binding.SlotWidgetName = NAME_None;
        Binding.bIsRootWidget = false;
        Animation->AnimationBindings.Add(Binding);

        UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
        if (!FloatTrack)
        {
            return false;
        }
        FloatTrack->SetPropertyNameAndPath(FName(TEXT("RenderOpacity")), TEXT("RenderOpacity"));

        UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
        if (!FloatSection)
        {
            return false;
        }
        FloatSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
        FloatTrack->AddSection(*FloatSection);

        FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
        Channel.AddLinearKey(FFrameNumber(0), 0.0f);

        FMovieSceneTangentData Tangent;
        Tangent.ArriveTangent = 0.25f;
        Tangent.LeaveTangent = -0.5f;
        Channel.AddCubicKey(FFrameNumber(15), 0.75f, RCTM_User, Tangent);
        Channel.AddConstantKey(FFrameNumber(30), 1.0f);

        WidgetBlueprint->Animations.Add(Animation);
        RegisterWidgetVariable(WidgetBlueprint, Animation->GetFName());

        OutFixture.WidgetBlueprint = WidgetBlueprint;
        OutFixture.RootWidget = WidgetBlueprint->WidgetTree->RootWidget;
        OutFixture.AnimatedWidget = TargetWidget;
        OutFixture.Animation = Animation;
        OutFixture.MovieScene = MovieScene;
        OutFixture.FloatTrack = FloatTrack;
        OutFixture.FloatSection = FloatSection;
        OutFixture.BindingGuid = BindingGuid;
        return true;
    }

    FWidgetAnimationFixture CreateFloatAnimationFixture(const FString& Prefix)
    {
        FWidgetAnimationFixture Fixture;
        Fixture.WidgetPath = MakeWidgetAnimationJsonTestAssetPath(Prefix);
        Fixture.WidgetBlueprint = CreateTransientWidgetBlueprint(Fixture.WidgetPath);
        if (Fixture.WidgetBlueprint)
        {
            AddFloatAnimationFixture(
                Fixture.WidgetBlueprint,
                TEXT("FadeIn"),
                TEXT("AnimatedLabel"),
                Fixture);
            Fixture.WidgetPath = Fixture.WidgetBlueprint->GetOutermost()->GetName();
        }
        return Fixture;
    }

    FCoreUmgAnimationFixture CreateCoreUmgAnimationFixture(const FString& Prefix)
    {
        FCoreUmgAnimationFixture Fixture;
        Fixture.WidgetPath = MakeWidgetAnimationJsonTestAssetPath(Prefix);
        Fixture.WidgetBlueprint = CreateTransientWidgetBlueprint(Fixture.WidgetPath);
        if (!Fixture.WidgetBlueprint)
        {
            return Fixture;
        }

        Fixture.MaterialWidget = AddNamedImage(Fixture.WidgetBlueprint, TEXT("AnimatedImage"));
        Fixture.TextWidget = AddNamedTextBlock(Fixture.WidgetBlueprint, TEXT("AnimatedLabel"));
        if (!Fixture.MaterialWidget || !Fixture.TextWidget)
        {
            return Fixture;
        }

        Fixture.Animation = NewObject<UWidgetAnimation>(
            Fixture.WidgetBlueprint, TEXT("CoreUmg"), RF_Transient | RF_Transactional);
        // MovieScene name must equal the animation's — see AddFloatAnimationFixture.
        Fixture.MovieScene = Fixture.Animation
            ? NewObject<UMovieScene>(Fixture.Animation, Fixture.Animation->GetFName(), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Fixture.Animation || !Fixture.MovieScene)
        {
            return Fixture;
        }

        Fixture.Animation->MovieScene = Fixture.MovieScene;
        Fixture.MovieScene->SetDisplayRate(FFrameRate(30, 1));
        Fixture.MovieScene->SetTickResolutionDirectly(FFrameRate(30, 1));
        Fixture.MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));

        Fixture.MaterialBindingGuid = Fixture.MovieScene->AddPossessable(
            Fixture.MaterialWidget->GetFName().ToString(), Fixture.MaterialWidget->GetClass());
        Fixture.TextBindingGuid = Fixture.MovieScene->AddPossessable(
            Fixture.TextWidget->GetFName().ToString(), Fixture.TextWidget->GetClass());
        if (!Fixture.MaterialBindingGuid.IsValid() || !Fixture.TextBindingGuid.IsValid())
        {
            return Fixture;
        }

        FWidgetAnimationBinding MaterialBinding;
        MaterialBinding.WidgetName = Fixture.MaterialWidget->GetFName();
        MaterialBinding.AnimationGuid = Fixture.MaterialBindingGuid;
        MaterialBinding.SlotWidgetName = NAME_None;
        MaterialBinding.bIsRootWidget = false;
        Fixture.Animation->AnimationBindings.Add(MaterialBinding);

        FWidgetAnimationBinding TextBinding;
        TextBinding.WidgetName = Fixture.TextWidget->GetFName();
        TextBinding.AnimationGuid = Fixture.TextBindingGuid;
        TextBinding.SlotWidgetName = NAME_None;
        TextBinding.bIsRootWidget = false;
        Fixture.Animation->AnimationBindings.Add(TextBinding);

        Fixture.MaterialTrack = Fixture.MovieScene->AddTrack<UMovieSceneWidgetMaterialTrack>(Fixture.MaterialBindingGuid);
        if (Fixture.MaterialTrack)
        {
            TArray<FName> BrushPropertyNamePath;
            BrushPropertyNamePath.Add(FName(TEXT("Brush")));
            Fixture.MaterialTrack->SetBrushPropertyNamePath(BrushPropertyNamePath);
            Fixture.MaterialSection = Cast<UMovieSceneParameterSection>(Fixture.MaterialTrack->CreateNewSection());
            if (Fixture.MaterialSection)
            {
                Fixture.MaterialSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));

                FScalarParameterNameAndCurve& Scalar = Fixture.MaterialSection->GetScalarParameterNamesAndCurves().Add_GetRef(
                    FScalarParameterNameAndCurve(FName(TEXT("FillAmount"))));
                Scalar.ParameterCurve.AddLinearKey(FFrameNumber(0), 0.2f);
                Scalar.ParameterCurve.AddLinearKey(FFrameNumber(30), 0.8f);

                FColorParameterNameAndCurves& Color = Fixture.MaterialSection->GetColorParameterNamesAndCurves().Add_GetRef(
                    FColorParameterNameAndCurves(FName(TEXT("Tint"))));
                Color.RedCurve.AddLinearKey(FFrameNumber(0), 0.1f);
                Color.GreenCurve.AddLinearKey(FFrameNumber(10), 0.4f);
                Color.BlueCurve.AddLinearKey(FFrameNumber(20), 0.7f);
                Color.AlphaCurve.AddLinearKey(FFrameNumber(30), 1.0f);

                Fixture.MaterialTrack->AddSection(*Fixture.MaterialSection);
            }
        }

        Fixture.TransformTrack = Fixture.MovieScene->AddTrack<UMovieScene2DTransformTrack>(Fixture.TextBindingGuid);
        if (Fixture.TransformTrack)
        {
            Fixture.TransformTrack->SetPropertyNameAndPath(FName(TEXT("RenderTransform")), TEXT("RenderTransform"));
            Fixture.TransformSection = Cast<UMovieScene2DTransformSection>(Fixture.TransformTrack->CreateNewSection());
            if (Fixture.TransformSection)
            {
                Fixture.TransformSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                Fixture.TransformSection->SetMask(FMovieScene2DTransformMask(EMovieScene2DTransformChannel::AllTransform));
                Fixture.TransformSection->Translation[0].AddLinearKey(FFrameNumber(0), 12.0f);
                Fixture.TransformSection->Translation[1].AddLinearKey(FFrameNumber(0), 24.0f);
                Fixture.TransformSection->Rotation.AddLinearKey(FFrameNumber(15), 45.0f);
                Fixture.TransformSection->Scale[0].AddLinearKey(FFrameNumber(30), 1.5f);
                Fixture.TransformSection->Scale[1].AddLinearKey(FFrameNumber(30), 0.5f);
                Fixture.TransformTrack->AddSection(*Fixture.TransformSection);
            }
        }

        Fixture.TextTrack = Fixture.MovieScene->AddTrack<UMovieSceneTextTrack>(Fixture.TextBindingGuid);
        if (Fixture.TextTrack)
        {
            Fixture.TextTrack->SetPropertyNameAndPath(FName(TEXT("Text")), TEXT("Text"));
            Fixture.TextSection = Cast<UMovieSceneTextSection>(Fixture.TextTrack->CreateNewSection());
            if (Fixture.TextSection)
            {
                Fixture.TextSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                TArray<FFrameNumber> Times;
                Times.Add(FFrameNumber(0));
                Times.Add(FFrameNumber(30));

                TArray<FText> Values;
                Values.Add(NSLOCTEXT("PinWrightTests", "CoreUmgReady", "Ready"));
                Values.Add(NSLOCTEXT("PinWrightTests", "CoreUmgGo", "Go"));

                FMovieSceneTextChannel& TextChannel = const_cast<FMovieSceneTextChannel&>(Fixture.TextSection->GetChannel());
                TextChannel.AddKeys(Times, Values);
                Fixture.TextTrack->AddSection(*Fixture.TextSection);
            }
        }

        Fixture.WidgetBlueprint->Animations.Add(Fixture.Animation);
        RegisterWidgetVariable(Fixture.WidgetBlueprint, Fixture.Animation->GetFName());
        Fixture.WidgetPath = Fixture.WidgetBlueprint->GetOutermost()->GetName();
        return Fixture;
    }

    FCommonPropertyAnimationFixture CreateCommonPropertyAnimationFixture(const FString& Prefix)
    {
        FCommonPropertyAnimationFixture Fixture;
        Fixture.WidgetPath = MakeWidgetAnimationJsonTestAssetPath(Prefix);
        Fixture.WidgetBlueprint = CreateTransientWidgetBlueprint(Fixture.WidgetPath);
        if (!Fixture.WidgetBlueprint)
        {
            return Fixture;
        }

        Fixture.AnimatedImage = AddNamedImage(Fixture.WidgetBlueprint, TEXT("AnimatedImage"));
        if (!Fixture.AnimatedImage)
        {
            return Fixture;
        }

        Fixture.Animation = NewObject<UWidgetAnimation>(
            Fixture.WidgetBlueprint, TEXT("CommonPropAnim"), RF_Transient | RF_Transactional);
        // MovieScene name must equal the animation's — see AddFloatAnimationFixture.
        Fixture.MovieScene = Fixture.Animation
            ? NewObject<UMovieScene>(Fixture.Animation, Fixture.Animation->GetFName(), RF_Transient | RF_Transactional)
            : nullptr;
        if (!Fixture.Animation || !Fixture.MovieScene)
        {
            return Fixture;
        }

        Fixture.Animation->MovieScene = Fixture.MovieScene;
        Fixture.MovieScene->SetDisplayRate(FFrameRate(30, 1));
        Fixture.MovieScene->SetTickResolutionDirectly(FFrameRate(30, 1));
        Fixture.MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));

        Fixture.BindingGuid = Fixture.MovieScene->AddPossessable(
            Fixture.AnimatedImage->GetFName().ToString(), Fixture.AnimatedImage->GetClass());
        if (!Fixture.BindingGuid.IsValid())
        {
            return Fixture;
        }

        FWidgetAnimationBinding ImageBinding;
        ImageBinding.WidgetName = Fixture.AnimatedImage->GetFName();
        ImageBinding.AnimationGuid = Fixture.BindingGuid;
        ImageBinding.SlotWidgetName = NAME_None;
        ImageBinding.bIsRootWidget = false;
        Fixture.Animation->AnimationBindings.Add(ImageBinding);

        // Color track: Brush.TintColor — two red-channel keys ramping 0→1
        Fixture.ColorTrack = Fixture.MovieScene->AddTrack<UMovieSceneColorTrack>(Fixture.BindingGuid);
        if (Fixture.ColorTrack)
        {
            Fixture.ColorTrack->SetPropertyNameAndPath(FName(TEXT("ColorAndOpacity")), TEXT("ColorAndOpacity"));
            Fixture.ColorSection = Cast<UMovieSceneColorSection>(Fixture.ColorTrack->CreateNewSection());
            if (Fixture.ColorSection)
            {
                Fixture.ColorSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                Fixture.ColorSection->GetRedChannel().AddLinearKey(FFrameNumber(0), 0.0f);
                Fixture.ColorSection->GetRedChannel().AddLinearKey(FFrameNumber(30), 1.0f);
                Fixture.ColorSection->GetGreenChannel().AddLinearKey(FFrameNumber(0), 0.5f);
                Fixture.ColorSection->GetAlphaChannel().AddLinearKey(FFrameNumber(0), 1.0f);
                Fixture.ColorTrack->AddSection(*Fixture.ColorSection);
            }
        }

        // Byte track: Visibility — one key (ESlateVisibility::Hidden) with enum set
        Fixture.ByteTrack = Fixture.MovieScene->AddTrack<UMovieSceneByteTrack>(Fixture.BindingGuid);
        if (Fixture.ByteTrack)
        {
            Fixture.ByteTrack->SetPropertyNameAndPath(FName(TEXT("Visibility")), TEXT("Visibility"));
            Fixture.ByteTrack->SetEnum(StaticEnum<ESlateVisibility>());
            Fixture.ByteSection = Cast<UMovieSceneByteSection>(Fixture.ByteTrack->CreateNewSection());
            if (Fixture.ByteSection)
            {
                Fixture.ByteSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                TArray<FFrameNumber> ByteTimes;
                TArray<uint8> ByteValues;
                ByteTimes.Add(FFrameNumber(15));
                ByteValues.Add(static_cast<uint8>(ESlateVisibility::Hidden));
                Fixture.ByteSection->ByteCurve.AddKeys(ByteTimes, ByteValues);
                Fixture.ByteTrack->AddSection(*Fixture.ByteSection);
            }
        }

        // FloatVector track: 3 channels, 3 keys
        Fixture.FloatVectorTrack = Fixture.MovieScene->AddTrack<UMovieSceneFloatVectorTrack>(Fixture.BindingGuid);
        if (Fixture.FloatVectorTrack)
        {
            Fixture.FloatVectorTrack->SetPropertyNameAndPath(FName(TEXT("RenderScale")), TEXT("RenderScale"));
            Fixture.FloatVectorTrack->SetNumChannelsUsed(3);
            Fixture.FloatVectorSection = Cast<UMovieSceneFloatVectorSection>(Fixture.FloatVectorTrack->CreateNewSection());
            if (Fixture.FloatVectorSection)
            {
                Fixture.FloatVectorSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                Fixture.FloatVectorSection->SetChannelsUsed(3);
                const_cast<FMovieSceneFloatChannel&>(Fixture.FloatVectorSection->GetChannel(0)).AddLinearKey(FFrameNumber(0), 1.0f);
                const_cast<FMovieSceneFloatChannel&>(Fixture.FloatVectorSection->GetChannel(1)).AddLinearKey(FFrameNumber(10), 2.0f);
                const_cast<FMovieSceneFloatChannel&>(Fixture.FloatVectorSection->GetChannel(2)).AddLinearKey(FFrameNumber(20), 3.0f);
                Fixture.FloatVectorTrack->AddSection(*Fixture.FloatVectorSection);
            }
        }

        // DoubleVector track: 2 channels, 2 keys
        Fixture.DoubleVectorTrack = Fixture.MovieScene->AddTrack<UMovieSceneDoubleVectorTrack>(Fixture.BindingGuid);
        if (Fixture.DoubleVectorTrack)
        {
            Fixture.DoubleVectorTrack->SetPropertyNameAndPath(FName(TEXT("RenderTranslation")), TEXT("RenderTranslation"));
            Fixture.DoubleVectorTrack->SetNumChannelsUsed(2);
            Fixture.DoubleVectorSection = Cast<UMovieSceneDoubleVectorSection>(Fixture.DoubleVectorTrack->CreateNewSection());
            if (Fixture.DoubleVectorSection)
            {
                Fixture.DoubleVectorSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
                Fixture.DoubleVectorSection->SetChannelsUsed(2);
                FMovieSceneDoubleValue DoubleValX(10.5);
                DoubleValX.InterpMode = RCIM_Linear;
                FMovieSceneDoubleValue DoubleValY(20.25);
                DoubleValY.InterpMode = RCIM_Linear;
                TArray<FFrameNumber> DoubleTimes0 = { FFrameNumber(0) };
                TArray<FMovieSceneDoubleValue> DoubleValues0 = { DoubleValX };
                TArray<FFrameNumber> DoubleTimes1 = { FFrameNumber(15) };
                TArray<FMovieSceneDoubleValue> DoubleValues1 = { DoubleValY };
                const_cast<FMovieSceneDoubleChannel&>(Fixture.DoubleVectorSection->GetChannel(0)).Set(DoubleTimes0, DoubleValues0);
                const_cast<FMovieSceneDoubleChannel&>(Fixture.DoubleVectorSection->GetChannel(1)).Set(DoubleTimes1, DoubleValues1);
                Fixture.DoubleVectorTrack->AddSection(*Fixture.DoubleVectorSection);
            }
        }

        Fixture.WidgetBlueprint->Animations.Add(Fixture.Animation);
        RegisterWidgetVariable(Fixture.WidgetBlueprint, Fixture.Animation->GetFName());
        Fixture.WidgetPath = Fixture.WidgetBlueprint->GetOutermost()->GetName();
        return Fixture;
    }
}
