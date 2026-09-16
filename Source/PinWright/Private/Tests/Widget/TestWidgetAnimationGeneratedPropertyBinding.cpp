// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-widget-animation-moviescene-unbound.
//
// A UWidgetAnimation is playable only when its MovieScene carries the animation's own
// FName. The UMG compiler generates the animation's blueprint property from the ANIMATION's
// name (WidgetBlueprintCompiler.cpp: `AnimVariableDesc.VarName = Animation->GetFName()`),
// while UWidgetBlueprintGeneratedClass::BindAnimationsStatic assigns that property by
// looking it up under the MOVIE SCENE's name
// (WidgetBlueprintGeneratedClass.cpp: `InPropertyMap.Find(Animation->GetMovieScene()->GetFName())`).
//
// The authoring path used to mint `<AnimName>_MovieScene`, so the lookup missed: the asset
// dumped correctly, the blueprint compiled clean, the generated property existed and was
// readable — and it was permanently null, making every PlayAnimation call a silent no-op.
// Every write echoed success, and no read-back in the plugin exposed the MovieScene's name.
//
// The first test drives the real create verb, compiles the blueprint, initializes an
// instance through the engine's own path and asserts the generated property resolves to the
// animation. That is the only assertion that fails on the old naming — the name equality
// alone is a readback of what the writer just set, so it is asserted as a supporting fact
// rather than as the verdict.
//
// The second test covers the repair direction: an animation already carrying the broken
// name is renamed the next time an animation verb touches it, and widget.get_animation_info
// reports the discriminating fact in both states.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Dom/JsonObject.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
    // A Widget Blueprint that actually survives FKismetEditorUtilities::CompileBlueprint:
    // built through CreateBlueprint (so it has a parent class and a skeleton) rather than a
    // bare NewObject, in a transient package so nothing reaches disk. Every source widget is
    // registered in WidgetVariableNameToGuidMap because the compiler audits that map with
    // ensureAlways once it is non-empty, and creating an animation makes it non-empty.
    UWidgetBlueprint* MakeCompilableWidgetBlueprint(const FString& PackagePath, const TCHAR* ChildWidgetName)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            UUserWidget::StaticClass(),
            Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass()));
        if (!WBP || !WBP->WidgetTree)
        {
            return nullptr;
        }

        UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
        if (!Root || !WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, ChildWidgetName))
        {
            return nullptr;
        }
        return WBP;
    }

    UWidgetAnimation* FindAnimation(const UWidgetBlueprint* WBP, const FName& AnimationName)
    {
        if (!WBP)
        {
            return nullptr;
        }
        for (UWidgetAnimation* Anim : WBP->Animations)
        {
            if (Anim && Anim->GetFName() == AnimationName)
            {
                return Anim;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateAnimationBindsGeneratedPropertyTest,
    "PinWright.widget.create_widget_animation.GeneratedPropertyBindsAfterCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateAnimationBindsGeneratedPropertyTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_AnimationBinding"));
    UWidgetBlueprint* WBP = MakeCompilableWidgetBlueprint(WidgetPath, TEXT("AnimTarget"));
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    const FName AnimName(TEXT("ProbeAnim"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("animationName"), AnimName.ToString());
    Payload->SetNumberField(TEXT("duration"), 1.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.create_widget_animation handler found"),
        InvokeHandlerWithCapture(TEXT("widget.create_widget_animation"), Payload, Capture));
    TestTrue(TEXT("create_widget_animation reports success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("create_widget_animation failed: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    // The response has to name the MovieScene at all - without that field a caller cannot
    // tell a playable animation from an unbound one before a compile.
    FString ReportedMovieSceneName;
    TestTrue(TEXT("response carries movieSceneName"),
        Capture.Result->TryGetStringField(TEXT("movieSceneName"), ReportedMovieSceneName));
    TestEqual(TEXT("reported MovieScene name is the animation name"),
        ReportedMovieSceneName, AnimName.ToString());

    UWidgetAnimation* SourceAnim = FindAnimation(WBP, AnimName);
    TestNotNull(TEXT("animation landed on the blueprint"), SourceAnim);
    if (!SourceAnim || !SourceAnim->GetMovieScene())
    {
        AddError(TEXT("animation or its MovieScene missing after create_widget_animation"));
        return false;
    }

    TestEqual(TEXT("MovieScene FName equals the animation FName"),
        SourceAnim->GetMovieScene()->GetFName(), SourceAnim->GetFName());

    FKismetEditorUtilities::CompileBlueprint(WBP);

    UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WBP->GeneratedClass);
    TestNotNull(TEXT("compile produced a widget generated class"), GeneratedClass);
    if (!GeneratedClass)
    {
        return false;
    }

    // The compiler clones each source animation onto the class as `<Name>_INST`, carrying
    // the source MovieScene's inner name with it - which is why the SOURCE animation's
    // MovieScene name is what has to match, and why this property lookup uses the plain name.
    FObjectPropertyBase* AnimProperty = CastField<FObjectPropertyBase>(
        GeneratedClass->FindPropertyByName(AnimName));
    TestNotNull(TEXT("compiler generated the animation property"), AnimProperty);
    if (!AnimProperty)
    {
        return false;
    }

    UUserWidget* Instance = NewObject<UUserWidget>(GetTransientPackage(), GeneratedClass);
    TestNotNull(TEXT("user widget instance constructed"), Instance);
    if (!Instance)
    {
        return false;
    }
    Instance->AddToRoot();
    ON_SCOPE_EXIT
    {
        Instance->RemoveFromRoot();
    };

    // UUserWidget::Initialize routes into UWidgetBlueprintGeneratedClass::BindAnimationsStatic,
    // the only place the generated animation property is ever assigned. This is the engine's
    // own binder, not a reimplementation of it, and it is what read null before the fix.
    Instance->Initialize();

    UObject* BoundAnimation = AnimProperty->GetObjectPropertyValue_InContainer(Instance);
    TestNotNull(TEXT("generated animation property is non-null after initialization"), BoundAnimation);
    TestTrue(TEXT("generated animation property resolves to a compiled animation"),
        BoundAnimation != nullptr
        && GeneratedClass->Animations.Contains(Cast<UWidgetAnimation>(BoundAnimation)));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddAnimationTrackRepairsMovieSceneNameTest,
    "PinWright.widget.add_animation_track.RepairsMisnamedMovieScene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddAnimationTrackRepairsMovieSceneNameTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_AnimationRepair"));
    UWidgetBlueprint* WBP = MakeCompilableWidgetBlueprint(WidgetPath, TEXT("AnimTarget"));
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    // Hand-build the shape the old authoring path produced: a MovieScene named
    // `<AnimName>_MovieScene`. This is the already-broken asset a repair has to fix.
    const FName AnimName(TEXT("LegacyAnim"));
    UWidgetAnimation* Anim = NewObject<UWidgetAnimation>(WBP, AnimName, RF_Transactional);
    UMovieScene* BrokenMovieScene = Anim
        ? NewObject<UMovieScene>(Anim, FName(TEXT("LegacyAnim_MovieScene")), RF_Transactional)
        : nullptr;
    TestNotNull(TEXT("legacy animation allocated"), Anim);
    TestNotNull(TEXT("legacy MovieScene allocated"), BrokenMovieScene);
    if (!Anim || !BrokenMovieScene)
    {
        return false;
    }
    Anim->MovieScene = BrokenMovieScene;
    BrokenMovieScene->SetDisplayRate(FFrameRate(30, 1));
    BrokenMovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(30)));
    WBP->Animations.Add(Anim);
    WidgetTestFixtures::RegisterWidgetVariable(WBP, Anim->GetFName());

    TestNotEqual(TEXT("fixture really starts in the broken state"),
        BrokenMovieScene->GetFName(), Anim->GetFName());

    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    InfoPayload->SetStringField(TEXT("animationName"), AnimName.ToString());

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.get_animation_info handler found"),
        InvokeHandlerWithCapture(TEXT("widget.get_animation_info"), InfoPayload, Capture));
    TestTrue(TEXT("get_animation_info succeeds on the broken animation"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ReportedName;
        Capture.Result->TryGetStringField(TEXT("movieSceneName"), ReportedName);
        TestEqual(TEXT("get_animation_info reports the broken MovieScene name"),
            ReportedName, FString(TEXT("LegacyAnim_MovieScene")));
        bool bMatches = true;
        TestTrue(TEXT("get_animation_info carries movieSceneNameMatchesAnimation"),
            Capture.Result->TryGetBoolField(TEXT("movieSceneNameMatchesAnimation"), bMatches));
        // The failure direction: the field has to read false on an animation that will not
        // bind, or it cannot diagnose anything.
        TestFalse(TEXT("broken animation reports a name mismatch"), bMatches);
    }

    TSharedPtr<FJsonObject> TrackPayload = MakeShared<FJsonObject>();
    TrackPayload->SetStringField(TEXT("widgetPath"), WidgetPath);
    TrackPayload->SetStringField(TEXT("animationName"), AnimName.ToString());
    TrackPayload->SetStringField(TEXT("widgetName"), TEXT("AnimTarget"));

    TestTrue(TEXT("widget.add_animation_track handler found"),
        InvokeHandlerWithCapture(TEXT("widget.add_animation_track"), TrackPayload, Capture));
    TestTrue(TEXT("add_animation_track reports success"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("add_animation_track failed: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    TestNotNull(TEXT("animation still has a MovieScene after the track add"), Anim->GetMovieScene());
    if (!Anim->GetMovieScene())
    {
        return false;
    }
    TestEqual(TEXT("MovieScene was renamed to the animation name"),
        Anim->GetMovieScene()->GetFName(), Anim->GetFName());
    // The repair renames the existing object rather than replacing it, so any track already
    // keyed onto it survives.
    TestTrue(TEXT("repair renamed the existing MovieScene rather than replacing it"),
        Anim->GetMovieScene() == BrokenMovieScene);

    TestTrue(TEXT("widget.get_animation_info handler found (post-repair)"),
        InvokeHandlerWithCapture(TEXT("widget.get_animation_info"), InfoPayload, Capture));
    TestTrue(TEXT("get_animation_info succeeds after the repair"), Capture.bSuccess);
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ReportedName;
        Capture.Result->TryGetStringField(TEXT("movieSceneName"), ReportedName);
        TestEqual(TEXT("get_animation_info reports the repaired MovieScene name"),
            ReportedName, AnimName.ToString());
        bool bMatches = false;
        Capture.Result->TryGetBoolField(TEXT("movieSceneNameMatchesAnimation"), bMatches);
        TestTrue(TEXT("repaired animation reports a name match"), bMatches);
    }

    return true;
}
