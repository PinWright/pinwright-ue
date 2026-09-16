// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetAnimationHandler.cpp
// Widget animation handlers migrated from _WidgetAuthoringHandlers.cpp

#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetAnimationEventIntrospection.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Handlers/UI/WidgetAnimationTestHooks.h"
#endif
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetAnimationJsonSerializer.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "ScopedTransaction.h"
#include "Utils/JsonUtils.h"
#include "Utils/PropertyInspection.h"

#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;

namespace
{
    void EnsureAnimationVariableGuid(UWidgetBlueprint* WidgetBP, const FName& AnimationName)
    {
        // An animation name is just another widget-blueprint variable, so route through
        // the shared helper that owns the version split (no-op on UE 5.4, which has no
        // WidgetVariableNameToGuidMap / OnVariableAdded; registers on 5.5+).
        WidgetAuthoringHelpers::EnsureWidgetVariableGuid(WidgetBP, AnimationName);
    }

    void UpsertAnimationBinding(UWidgetAnimation* Animation, const FGuid& Guid, UWidget* TargetWidget)
    {
        if (!Animation || !TargetWidget)
        {
            return;
        }

        for (FWidgetAnimationBinding& Existing : Animation->AnimationBindings)
        {
            if (Existing.AnimationGuid == Guid || Existing.WidgetName == TargetWidget->GetFName())
            {
                Existing.AnimationGuid = Guid;
                Existing.WidgetName = TargetWidget->GetFName();
                Existing.SlotWidgetName = NAME_None;
                Existing.bIsRootWidget = false;
                return;
            }
        }

        FWidgetAnimationBinding Binding;
        Binding.AnimationGuid = Guid;
        Binding.WidgetName = TargetWidget->GetFName();
        Binding.SlotWidgetName = NAME_None;
        Binding.bIsRootWidget = false;
        Animation->AnimationBindings.Add(Binding);
    }

    // Publish the MovieScene's own FName and whether it still matches the animation's.
    // UWidgetBlueprintGeneratedClass binds the generated animation property by the MOVIE
    // SCENE's name while the compiler generates that property from the ANIMATION's, so an
    // animation whose two names disagree compiles clean and never plays. Nothing else in a
    // read-back exposes that: the generated property only exists after a compile, and the
    // frame rate, range and bindings all look correct on a broken animation.
    void AddMovieSceneIdentity(const TSharedPtr<FJsonObject>& Target, const UWidgetAnimation* Animation)
    {
        if (!Target.IsValid() || !Animation || !Animation->GetMovieScene())
        {
            return;
        }
        const FName MovieSceneName = Animation->GetMovieScene()->GetFName();
        Target->SetStringField(TEXT("movieSceneName"), MovieSceneName.ToString());
        Target->SetBoolField(TEXT("movieSceneNameMatchesAnimation"), MovieSceneName == Animation->GetFName());
    }

    void AddWarnings(TSharedPtr<FJsonObject> Result, const TArray<FString>& Warnings)
    {
        TArray<TSharedPtr<FJsonValue>> WarningValues;
        for (const FString& Warning : Warnings)
        {
            WarningValues.Add(MakeShared<FJsonValueString>(Warning));
        }
        Result->SetArrayField(TEXT("warnings"), WarningValues);
        Result->SetNumberField(TEXT("warningCount"), Warnings.Num());
    }

    void CollectFloatPropertyCandidates(const UClass* WidgetClass, TArray<FString>& OutCandidates)
    {
        OutCandidates.Reset();
        if (!WidgetClass)
        {
            return;
        }

        TSet<FName> Seen;
        for (TFieldIterator<FProperty> It(WidgetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            const FProperty* Property = *It;
            if (CastField<FFloatProperty>(Property) && !Seen.Contains(Property->GetFName()))
            {
                Seen.Add(Property->GetFName());
                OutCandidates.Add(Property->GetName());
            }
        }
        OutCandidates.Sort();
    }

    bool ResolveFloatAnimationProperty(
        UWidget* TargetWidget,
        const FString& RequestedPath,
        FName& OutPropertyName,
        FString& OutPropertyPath,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        TSharedPtr<FJsonObject>& OutErrorData)
    {
        OutPropertyName = NAME_None;
        OutPropertyPath.Reset();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();
        OutErrorData.Reset();

        TArray<FString> Candidates;
        CollectFloatPropertyCandidates(TargetWidget ? TargetWidget->GetClass() : nullptr, Candidates);

        auto BuildErrorData = [&]()
        {
            OutErrorData = MakeShared<FJsonObject>();
            OutErrorData->SetStringField(TEXT("widgetName"), TargetWidget ? TargetWidget->GetName() : FString());
            OutErrorData->SetStringField(TEXT("widgetClass"),
                TargetWidget ? TargetWidget->GetClass()->GetPathName() : FString());
            OutErrorData->SetStringField(TEXT("propertyName"), RequestedPath);
            OutErrorData->SetArrayField(TEXT("candidates"), EmitStringArray(Candidates));
        };

        if (!TargetWidget || RequestedPath.IsEmpty())
        {
            BuildErrorData();
            OutErrorCode = TEXT("PROPERTY_NOT_FOUND");
            OutErrorMessage = RequestedPath.IsEmpty()
                ? TEXT("propertyName must name a reflected float property on the target widget.")
                : TEXT("Cannot resolve an animation property without a target widget.");
            return false;
        }

        void* Container = nullptr;
        FString ResolveError;
        FProperty* Property = ResolvePropertyOnObject(TargetWidget, RequestedPath, Container, ResolveError);
        if (!Property)
        {
            BuildErrorData();
            OutErrorCode = TEXT("PROPERTY_NOT_FOUND");
            OutErrorMessage = FString::Printf(
                TEXT("Property '%s' was not found on widget class '%s'. %s"),
                *RequestedPath, *TargetWidget->GetClass()->GetPathName(), *ResolveError);
            return false;
        }

        if (!CastField<FFloatProperty>(Property))
        {
            BuildErrorData();
            OutErrorData->SetStringField(TEXT("actualType"), GetPropertyCppTypeWithParams(Property));
            OutErrorCode = TEXT("UNSUPPORTED_PROPERTY");
            OutErrorMessage = FString::Printf(
                TEXT("Property '%s' on widget class '%s' has type '%s'; these verbs support float properties only."),
                *RequestedPath,
                *TargetWidget->GetClass()->GetPathName(),
                *GetPropertyCppTypeWithParams(Property));
            return false;
        }

        OutPropertyName = Property->GetFName();
        OutPropertyPath = RequestedPath.Contains(TEXT(".")) ? RequestedPath : Property->GetName();
        return true;
    }

    FGuid FindWidgetMovieSceneBinding(
        const UWidgetAnimation* Animation,
        const UMovieScene* MovieScene,
        const UWidget* TargetWidget)
    {
        if (!Animation || !MovieScene || !TargetWidget)
        {
            return FGuid();
        }

        // Searched through the const GetBindings() accessor rather than FindBinding(), which is
        // non-const through UE 5.3 and so unreachable from this const pointer there. Same answer:
        // FindBinding is a linear search of the same array on every supported engine.
        auto BindingExists = [MovieScene](const FGuid& Guid)
        {
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                if (Binding.GetObjectGuid() == Guid)
                {
                    return true;
                }
            }
            return false;
        };

        for (const FWidgetAnimationBinding& Existing : Animation->AnimationBindings)
        {
            if (Existing.WidgetName == TargetWidget->GetFName()
                && Existing.AnimationGuid.IsValid()
                && BindingExists(Existing.AnimationGuid))
            {
                return Existing.AnimationGuid;
            }
        }
        return FGuid();
    }

    UMovieSceneFloatTrack* FindFloatPropertyTrack(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const FString& PropertyPath)
    {
        FMovieSceneBinding* Binding = MovieScene ? MovieScene->FindBinding(BindingGuid) : nullptr;
        if (!Binding)
        {
            return nullptr;
        }

        for (UMovieSceneTrack* Track : Binding->GetTracks())
        {
            UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
            if (FloatTrack
                && FloatTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::IgnoreCase))
            {
                return FloatTrack;
            }
        }
        return nullptr;
    }

    TStrongObjectPtr<UMovieSceneFloatTrack> StageFloatPropertyTrack(
        const FName& PropertyName,
        const FString& PropertyPath,
        bool bAddSection)
    {
        TStrongObjectPtr<UMovieSceneFloatTrack> Track(
            NewObject<UMovieSceneFloatTrack>(GetTransientPackage(), NAME_None, RF_Transactional));
        if (!Track.IsValid())
        {
            return Track;
        }

        Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
        if (bAddSection)
        {
            UMovieSceneSection* Section = Track->CreateNewSection();
            if (!Section)
            {
                return TStrongObjectPtr<UMovieSceneFloatTrack>();
            }
            Section->SetRange(TRange<FFrameNumber>::All());
            Track->AddSection(*Section);
        }
        return Track;
    }

    bool TryGetOptionalFiniteNumber(
        const FHandlerContext& Ctx,
        const TCHAR* FieldName,
        double DefaultValue,
        double& OutValue,
        FString& OutError)
    {
        OutValue = DefaultValue;
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (!Payload.IsValid() || !Payload->HasField(FieldName))
        {
            return true;
        }
        return TryParseStrictJsonNumber(Payload->TryGetField(FieldName), OutValue, OutError);
    }

    bool ParseAnimationJsonPayload(const FHandlerContext& Ctx, TSharedPtr<FJsonObject>& OutDocument, FString& OutError)
    {
        OutDocument = Ctx.GetObject(TEXT("json"));
        if (OutDocument.IsValid())
        {
            return true;
        }

        const FString JsonText = Ctx.GetString(TEXT("json"));
        if (JsonText.IsEmpty())
        {
            OutError = TEXT("Missing required parameter: json");
            return false;
        }

        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
        if (!FJsonSerializer::Deserialize(Reader, OutDocument) || !OutDocument.IsValid())
        {
            OutError = TEXT("json must be a valid JSON object or a string containing a JSON object.");
            return false;
        }
        return true;
    }
}

// ---- widget.export_animations_json ----
REGISTER_RPC_HANDLER("widget.export_animations_json", "widget",
    "Export persisted UWidgetAnimation MovieScene tracks as JSON. Separate from widget-tree XML; v1 supports widget-name bindings plus float, widget material, 2D transform, and text tracks. Optional event metadata is inspection-only.",
    RPC_PARAMS(
        WidgetAssetPathParamOpt(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("includeEventMetadata", "boolean", "Include inspection-only eventTracks, delegateBindings, and triggeredFunctions metadata")
    ))
{
    const TArray<FString> PathKeys = WidgetAssetPathParamNames();
    const FString WidgetPath = Ctx.GetStringFirstOf(PathKeys);
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    TSharedPtr<FJsonObject> Document;
    TArray<FString> Warnings;
    FString Error;
    const bool bIncludeEventMetadata = Ctx.GetBool(TEXT("includeEventMetadata"), false);
    if (!WidgetAnimationJson::ExportAnimations(WidgetBP, Document, Warnings, Error, bIncludeEventMetadata))
    {
        Ctx.SendError(TEXT("EXPORT_FAILED"), Error);
        return true;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetObjectField(TEXT("json"), Document);
    ResultJson->SetStringField(TEXT("widgetPath"), WidgetBP->GetOutermost()->GetName());
    AddWarnings(ResultJson, Warnings);
    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.import_animations_json ----
REGISTER_RPC_HANDLER("widget.import_animations_json", "widget",
    "Import persisted UWidgetAnimation JSON into a widget blueprint. Requires explicit mode ('replace' or 'merge'); v1 supports widget-name bindings plus float, widget material, 2D transform, and text tracks.",
    RPC_PARAMS(
        WidgetAssetPathParamOpt(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("json", "object|string", "Animation JSON document or a string containing the JSON document"),
        RPC_PARAM_REQ("mode", "string", "Import mode: replace or merge")
    ))
{
    const TArray<FString> PathKeys = WidgetAssetPathParamNames();
    const FString WidgetPath = Ctx.GetStringFirstOf(PathKeys);
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (!Payload.IsValid() || !Payload->HasField(TEXT("mode")))
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: mode"));
        return true;
    }

    WidgetAnimationJson::EImportMode ImportMode;
    FString Error;
    if (!WidgetAnimationJson::ParseImportMode(Ctx.GetString(TEXT("mode")), ImportMode, Error))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"), Error);
        return true;
    }

    TSharedPtr<FJsonObject> Document;
    if (!ParseAnimationJsonPayload(Ctx, Document, Error))
    {
        Ctx.SendError(TEXT("INVALID_JSON"), Error);
        return true;
    }
    if (!WidgetAnimationJson::ParseDocument(Document, Error))
    {
        Ctx.SendError(TEXT("INVALID_JSON"), Error);
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    TArray<FString> Warnings;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.import_animations_json")));
        if (!WidgetAnimationJson::ApplyAnimations(WidgetBP, Document, ImportMode, Warnings, Error))
        {
            Transaction.Cancel();
            Ctx.SendError(TEXT("IMPORT_FAILED"), Error);
            return true;
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("widgetPath"), WidgetBP->GetOutermost()->GetName());
    ResultJson->SetStringField(TEXT("mode"), Ctx.GetString(TEXT("mode")));
    ResultJson->SetNumberField(TEXT("animationCount"), WidgetBP->Animations.Num());
    AddWarnings(ResultJson, Warnings);
    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.create_widget_animation ----
REGISTER_RPC_HANDLER("widget.create_widget_animation", "widget", "Create a new widget animation in a UMG widget blueprint",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name for the animation"),
        RPC_PARAM_OPT("duration", "number", "Duration in seconds (default 1.0)")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    double Duration = Ctx.GetNumber(TEXT("duration"), 1.0);

    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    if (AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: animationName"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    if (FindAnimationByName(WidgetBP, AnimationName))
    {
        Ctx.SendError(TEXT("ALREADY_EXISTS"), FString::Printf(TEXT("Animation '%s' already exists"), *AnimationName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.create_widget_animation")));
    UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WidgetBP, FName(*AnimationName), RF_Transactional);
    if (!NewAnim)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create animation"));
        return true;
    }

    const FAnimationMovieSceneResult MovieSceneResult = EnsureAnimationMovieScene(NewAnim);
    UMovieScene* MovieScene = MovieSceneResult.MovieScene;
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create animation movie scene"));
        return true;
    }

    MovieScene->SetDisplayRate(FFrameRate(30, 1));
    const FFrameRate TickRes = MovieScene->GetTickResolution();
    const FFrameNumber EndFrame = TickRes.AsFrameTime(Duration).RoundToFrame();
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame));

    WidgetBP->Animations.Add(NewAnim);
    EnsureAnimationVariableGuid(WidgetBP, NewAnim->GetFName());

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    // Read the name back off the object: UObject construction uniquifies a taken name, so
    // echoing the request would misreport what the caller must use to play the animation.
    ResultJson->SetStringField(TEXT("animationName"), NewAnim->GetName());
    ResultJson->SetNumberField(TEXT("duration"), Duration);
    ResultJson->SetStringField(TEXT("widgetPath"), WidgetBP->GetPathName());
    ResultJson->SetBoolField(TEXT("movieSceneCreated"), MovieSceneResult.bCreated);
    // The MovieScene's name is the key UWidgetBlueprintGeneratedClass binds the generated
    // animation property on; publishing it is what makes an unbound animation visible to a
    // reader, since the property itself only exists after a compile.
    ResultJson->SetStringField(TEXT("movieSceneName"), MovieScene->GetName());
    ResultJson->SetNumberField(TEXT("changesApplied"), 1);
    ResultJson->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);

    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.add_animation_track ----
REGISTER_RPC_HANDLER("widget.add_animation_track", "widget", "Add a reflected float-property track for a widget after validating and staging the complete edit",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_REQ("widgetName", "string", "Name of the widget to animate"),
        RPC_PARAM_OPT("propertyName", "string", "Reflected float property path to animate (default RenderOpacity)")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    FString WidgetName = Ctx.GetString(TEXT("widgetName"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"), TEXT("RenderOpacity"));

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty() || WidgetName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName, widgetName"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidgetAnimation* Animation = FindAnimationByName(WidgetBP, AnimationName);
    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    UWidget* TargetWidget = FindWidgetByName(WidgetBP, WidgetName);
    if (!TargetWidget)
    {
        Ctx.SendError(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("Widget '%s' not found in tree"), *WidgetName));
        return true;
    }

    FName ReflectedPropertyName;
    FString ReflectedPropertyPath;
    FString PropertyErrorCode;
    FString PropertyErrorMessage;
    TSharedPtr<FJsonObject> PropertyErrorData;
    if (!ResolveFloatAnimationProperty(
            TargetWidget,
            PropertyName,
            ReflectedPropertyName,
            ReflectedPropertyPath,
            PropertyErrorCode,
            PropertyErrorMessage,
            PropertyErrorData))
    {
        Ctx.SendError(PropertyErrorCode, PropertyErrorMessage, PropertyErrorData);
        return true;
    }

    UMovieScene* OriginalMovieScene = Animation->GetMovieScene();
    FGuid BindingGuid = FindWidgetMovieSceneBinding(Animation, OriginalMovieScene, TargetWidget);
    UMovieSceneFloatTrack* ExistingTrack =
        FindFloatPropertyTrack(OriginalMovieScene, BindingGuid, ReflectedPropertyPath);
#if WITH_DEV_AUTOMATION_TESTS
    if (!ExistingTrack
        && PinWrightWidgetAnimationTestHooks::ConsumeForceTrackAttachPreflightFailure())
    {
        Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
            TEXT("The float track failed final attachment preflight."));
        return true;
    }
#endif

    TStrongObjectPtr<UMovieSceneFloatTrack> StagedTrack;
    if (!ExistingTrack)
    {
        if (!UMovieScene::IsTrackClassAllowed(UMovieSceneFloatTrack::StaticClass()))
        {
            Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
                TEXT("Float tracks are not allowed in this MovieScene."));
            return true;
        }
        StagedTrack = StageFloatPropertyTrack(ReflectedPropertyName, ReflectedPropertyPath, true);
        if (!StagedTrack.IsValid())
        {
            Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
                TEXT("Failed to stage the float track and section before committing the request."));
            return true;
        }
    }

    FMovieSceneBinding* PrevalidatedBinding = nullptr;
    bool bNeedsNewBinding = false;
    if (StagedTrack.IsValid())
    {
        if (BindingGuid.IsValid())
        {
            PrevalidatedBinding = OriginalMovieScene->FindBinding(BindingGuid);
            if (!PrevalidatedBinding)
            {
                Ctx.SendError(TEXT("BINDING_NOT_FOUND"),
                    TEXT("The resolved widget binding disappeared during attachment preflight."));
                return true;
            }
        }
        else
        {
            bNeedsNewBinding = true;
        }
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.add_animation_track")));
    UMovieScene* MovieScene = EnsureAnimationMovieScene(Animation).MovieScene;
    checkf(MovieScene, TEXT("A validated widget animation must produce a MovieScene at commit."));

    if (bNeedsNewBinding)
    {
        // A binding's guid belongs to the movie scene: FMovieSceneBinding::SetObjectGuid is
        // deprecated from UE 5.7 and the guid-taking constructors are private to UMovieScene. So
        // create the possessable and its binding through UMovieScene rather than staging a binding
        // that already carries a guid. The staged track then attaches through the same
        // rename-and-AddTrack path an existing binding uses, which also reparents it to the movie
        // scene instead of leaving it outered to the transient package.
        BindingGuid = MovieScene->AddPossessable(
            TargetWidget->GetFName().ToString(), TargetWidget->GetClass());
        PrevalidatedBinding = MovieScene->FindBinding(BindingGuid);
        checkf(PrevalidatedBinding,
            TEXT("UMovieScene::AddPossessable must publish a binding for the guid it returns."));
        UpsertAnimationBinding(Animation, BindingGuid, TargetWidget);
    }

    if (StagedTrack.IsValid() && PrevalidatedBinding)
    {
        const bool bTrackRenamed = StagedTrack->Rename(
            nullptr,
            MovieScene,
            REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);
        checkf(bTrackRenamed, TEXT("A transient staged track must be renameable at commit."));
        PrevalidatedBinding->AddTrack(*StagedTrack.Get(), MovieScene);
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("animationName"), AnimationName);
    ResultJson->SetStringField(TEXT("widgetName"), TargetWidget->GetName());
    ResultJson->SetStringField(TEXT("propertyName"), ReflectedPropertyPath);
    ResultJson->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
    ResultJson->SetBoolField(TEXT("applied"), true);
    ResultJson->SetNumberField(TEXT("changesApplied"), 1);
    ResultJson->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    ResultJson->SetStringField(TEXT("note"),
        TEXT("Widget bound to animation with property track created. Use add_animation_keyframe to add keys."));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.add_animation_keyframe ----
REGISTER_RPC_HANDLER("widget.add_animation_keyframe", "widget", "Add a finite float key to a validated reflected float-property track without partial error-path mutation",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_OPT("widgetName", "string", "Optional widget name to target for this keyframe"),
        RPC_PARAM_OPT("propertyName", "string", "Optional reflected float property path to key (default RenderOpacity)"),
        RPC_PARAM_OPT("time", "number", "Keyframe time in seconds (default 0.0)"),
        RPC_PARAM_OPT("value", "number", "Keyframe value (default 1.0)")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    FString WidgetName = Ctx.GetString(TEXT("widgetName"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"), TEXT("RenderOpacity"));
    double Time = 0.0;
    double Value = 1.0;

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName"));
        return true;
    }

    FString NumberError;
    if (!TryGetOptionalFiniteNumber(Ctx, TEXT("time"), 0.0, Time, NumberError))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"), FString::Printf(TEXT("Invalid time: %s"), *NumberError));
        return true;
    }
    if (!TryGetOptionalFiniteNumber(Ctx, TEXT("value"), 1.0, Value, NumberError)
        || FMath::Abs(Value) > static_cast<double>(TNumericLimits<float>::Max()))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            NumberError.IsEmpty()
                ? TEXT("Invalid value: value is outside the finite float range.")
                : FString::Printf(TEXT("Invalid value: %s"), *NumberError));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidgetAnimation* Animation = FindAnimationByName(WidgetBP, AnimationName);

    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    UWidget* TargetWidget = nullptr;
    if (!WidgetName.IsEmpty())
    {
        TargetWidget = FindWidgetByName(WidgetBP, WidgetName);
        if (!TargetWidget)
        {
            Ctx.SendError(TEXT("WIDGET_NOT_FOUND"),
                FString::Printf(TEXT("Widget '%s' not found in tree"), *WidgetName));
            return true;
        }
    }
    else if (Animation->AnimationBindings.Num() > 0)
    {
        TargetWidget = FindWidgetByName(
            WidgetBP, Animation->AnimationBindings[0].WidgetName.ToString());
        if (!TargetWidget)
        {
            Ctx.SendError(TEXT("BINDING_NOT_FOUND"),
                TEXT("The animation's first binding does not resolve to a widget in the current tree."));
            return true;
        }
    }
    else if (WidgetBP->WidgetTree)
    {
        TargetWidget = WidgetBP->WidgetTree->RootWidget;
    }

    if (!TargetWidget)
    {
        Ctx.SendError(TEXT("BINDING_NOT_FOUND"),
            TEXT("No animation binding was found. Provide widgetName or create a track with widget.add_animation_track first."));
        return true;
    }

    FName ReflectedPropertyName;
    FString ReflectedPropertyPath;
    FString PropertyErrorCode;
    FString PropertyErrorMessage;
    TSharedPtr<FJsonObject> PropertyErrorData;
    if (!ResolveFloatAnimationProperty(
            TargetWidget,
            PropertyName,
            ReflectedPropertyName,
            ReflectedPropertyPath,
            PropertyErrorCode,
            PropertyErrorMessage,
            PropertyErrorData))
    {
        Ctx.SendError(PropertyErrorCode, PropertyErrorMessage, PropertyErrorData);
        return true;
    }

    UMovieScene* OriginalMovieScene = Animation->GetMovieScene();
    FGuid TargetBindingGuid = FindWidgetMovieSceneBinding(Animation, OriginalMovieScene, TargetWidget);
    UMovieSceneFloatTrack* TargetTrack =
        FindFloatPropertyTrack(OriginalMovieScene, TargetBindingGuid, ReflectedPropertyPath);
#if WITH_DEV_AUTOMATION_TESTS
    if (!TargetTrack
        && PinWrightWidgetAnimationTestHooks::ConsumeForceTrackAttachPreflightFailure())
    {
        Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
            TEXT("The float track failed final attachment preflight."));
        return true;
    }
#endif

    UMovieSceneFloatSection* ExistingSection = nullptr;
    TStrongObjectPtr<UMovieSceneFloatTrack> StagedTrack;
    TStrongObjectPtr<UMovieSceneFloatSection> StagedSection;

    if (TargetTrack && TargetTrack->GetAllSections().Num() > 0)
    {
        ExistingSection = Cast<UMovieSceneFloatSection>(TargetTrack->GetAllSections()[0]);
        if (!ExistingSection)
        {
            Ctx.SendError(TEXT("SECTION_TYPE_MISMATCH"),
                TEXT("The existing float track's first section is not a float section."));
            return true;
        }
    }
    else if (TargetTrack)
    {
        StagedSection.Reset(
            NewObject<UMovieSceneFloatSection>(GetTransientPackage(), NAME_None, RF_Transactional));
        if (!StagedSection.IsValid())
        {
            Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
                TEXT("Failed to stage a float section before committing the keyframe."));
            return true;
        }
        StagedSection->SetRange(TRange<FFrameNumber>::All());
    }
    else
    {
        if (!UMovieScene::IsTrackClassAllowed(UMovieSceneFloatTrack::StaticClass()))
        {
            Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
                TEXT("Float tracks are not allowed in this MovieScene."));
            return true;
        }
        StagedTrack = StageFloatPropertyTrack(ReflectedPropertyName, ReflectedPropertyPath, true);
        if (!StagedTrack.IsValid())
        {
            Ctx.SendError(TEXT("TRACK_CREATE_FAILED"),
                TEXT("Failed to stage the float track and section before committing the keyframe."));
            return true;
        }
    }

    FMovieSceneBinding* PrevalidatedBinding = nullptr;
    bool bNeedsNewBinding = false;
    if (StagedTrack.IsValid())
    {
        if (TargetBindingGuid.IsValid())
        {
            PrevalidatedBinding = OriginalMovieScene->FindBinding(TargetBindingGuid);
            if (!PrevalidatedBinding)
            {
                Ctx.SendError(TEXT("BINDING_NOT_FOUND"),
                    TEXT("The resolved widget binding disappeared during attachment preflight."));
                return true;
            }
        }
        else
        {
            bNeedsNewBinding = true;
        }
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.add_animation_keyframe")));
    UMovieScene* MovieScene = EnsureAnimationMovieScene(Animation).MovieScene;
    checkf(MovieScene, TEXT("A validated widget animation must produce a MovieScene at commit."));

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameNumber FrameNumber = TickResolution.AsFrameTime(Time).RoundToFrame();
    if (bNeedsNewBinding)
    {
        // See the matching comment in the add_animation_track handler: the movie scene owns a
        // binding's guid from UE 5.7, so the possessable and binding are created through it.
        TargetBindingGuid = MovieScene->AddPossessable(
            TargetWidget->GetFName().ToString(), TargetWidget->GetClass());
        PrevalidatedBinding = MovieScene->FindBinding(TargetBindingGuid);
        checkf(PrevalidatedBinding,
            TEXT("UMovieScene::AddPossessable must publish a binding for the guid it returns."));
        UpsertAnimationBinding(Animation, TargetBindingGuid, TargetWidget);
    }

    if (StagedTrack.IsValid() && PrevalidatedBinding)
    {
        const bool bTrackRenamed = StagedTrack->Rename(
            nullptr,
            MovieScene,
            REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);
        checkf(bTrackRenamed, TEXT("A transient staged track must be renameable at commit."));
        PrevalidatedBinding->AddTrack(*StagedTrack.Get(), MovieScene);
    }
    if (StagedTrack.IsValid())
    {
        TargetTrack = StagedTrack.Get();
        ExistingSection = CastChecked<UMovieSceneFloatSection>(TargetTrack->GetAllSections()[0]);
    }
    else if (StagedSection.IsValid())
    {
        StagedSection->Rename(nullptr, TargetTrack, REN_DontCreateRedirectors);
        TargetTrack->AddSection(*StagedSection.Get());
        ExistingSection = StagedSection.Get();
    }

    const TRange<FFrameNumber> ExistingRange = MovieScene->GetPlaybackRange();
    const FFrameNumber NewLower = ExistingRange.HasLowerBound()
        ? FMath::Min(ExistingRange.GetLowerBoundValue(), FrameNumber)
        : FrameNumber;
    const FFrameNumber NewUpper = ExistingRange.HasUpperBound()
        ? FMath::Max(ExistingRange.GetUpperBoundValue(), FrameNumber + 1)
        : FrameNumber + 1;
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(NewLower, NewUpper));

    FMovieSceneFloatChannel& Channel = ExistingSection->GetChannel();
    Channel.AddCubicKey(FrameNumber, static_cast<float>(Value));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetBoolField(TEXT("applied"), true);
    ResultJson->SetStringField(TEXT("animationName"), AnimationName);
    ResultJson->SetStringField(TEXT("propertyName"), ReflectedPropertyPath);
    if (!WidgetName.IsEmpty())
    {
        ResultJson->SetStringField(TEXT("widgetName"), WidgetName);
    }
    ResultJson->SetStringField(TEXT("bindingGuid"), TargetBindingGuid.ToString());
    ResultJson->SetNumberField(TEXT("time"), Time);
    ResultJson->SetNumberField(TEXT("value"), Value);
    ResultJson->SetNumberField(TEXT("frameNumber"), FrameNumber.Value);
    ResultJson->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    ResultJson->SetStringField(TEXT("note"),
        TEXT("Keyframe inserted into float track for the resolved widget binding and property."));

    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.set_animation_loop ----
REGISTER_RPC_HANDLER("widget.set_animation_loop", "widget", "Returns NOT_SUPPORTED: loop count is a runtime-only argument to UUserWidget::PlayAnimation(), not a serialized UWidgetAnimation property, so it can never be persisted onto the asset and every call hard-errors regardless of arguments. Set loops at runtime via PlayAnimation(Animation, 0.0, NumLoopsToPlay) in the widget Blueprint graph (e.g. the BPIR/graph authoring RPCs) or C++.",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_OPT("loop", "boolean", "Ignored; looping is a runtime PlayAnimation() arg, not an asset property"),
        RPC_PARAM_OPT("loopCount", "integer", "Ignored; the loop count is a runtime PlayAnimation() arg, not an asset property")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    bool bLoop = Ctx.GetBool(TEXT("loop"), true);
    int32 LoopCount = Ctx.GetInt(TEXT("loopCount"), 0);

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidgetAnimation* Animation = FindAnimationByName(WidgetBP, AnimationName);

    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    Ctx.SendError(TEXT("NOT_SUPPORTED"),
        TEXT("Loop count is a runtime-only parameter controlled via PlayAnimation(). It cannot be persisted in the animation asset. Use PlayAnimation(Animation, 0.0, NumLoopsToPlay) in Blueprint or C++."));
    return true;
}

// ---- widget.set_animation_speed ----
REGISTER_RPC_HANDLER("widget.set_animation_speed", "widget", "Returns NOT_SUPPORTED: playback speed is a runtime-only argument to UUserWidget::PlayAnimation(), not a serialized UWidgetAnimation property, so it can never be persisted onto the asset and every call hard-errors regardless of arguments. Set speed at runtime via PlayAnimation(Animation, 0.0, 1, EUMGSequencePlayMode::Forward, PlaybackSpeed) in the widget Blueprint graph (e.g. the BPIR/graph authoring RPCs) or C++.",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_OPT("speed", "number", "Ignored; playback speed is a runtime PlayAnimation() arg, not an asset property")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidgetAnimation* TargetAnim = FindAnimationByName(WidgetBP, AnimationName);
    if (!TargetAnim)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    Ctx.SendError(TEXT("NOT_SUPPORTED"),
        TEXT("Playback speed is a runtime-only parameter controlled via PlayAnimation(). It cannot be persisted in the animation asset. Use PlayAnimation(Animation, 0.0, 1, EUMGSequencePlayMode::Forward, PlaybackSpeed) in Blueprint or C++."));
    return true;
}

// ---- widget.get_animation_info ----
REGISTER_RPC_HANDLER("widget.get_animation_info", "widget", "Get information about widget animations",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("animationName", "string", "Name of specific animation (omit for list of all)"),
        RPC_PARAM_OPT("includeEvents", "boolean", "For a specific animation, include eventTracks, delegateBindings, and triggeredFunctions metadata")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    const bool bIncludeEvents = Ctx.GetBool(TEXT("includeEvents"), false);

    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();

    if (AnimationName.IsEmpty())
    {
        // Return list of all animations
        TArray<TSharedPtr<FJsonValue>> AnimationsArray;
        for (UWidgetAnimation* Anim : WidgetBP->Animations)
        {
            if (Anim)
            {
                TSharedPtr<FJsonObject> AnimInfo = MakeShareable(new FJsonObject());
                AnimInfo->SetStringField(TEXT("name"), Anim->GetName());
                if (Anim->MovieScene)
                {
                    AddMovieSceneIdentity(AnimInfo, Anim);
                    FFrameRate FrameRate = Anim->MovieScene->GetTickResolution();
                    FFrameNumber Start = Anim->MovieScene->GetPlaybackRange().GetLowerBoundValue();
                    FFrameNumber End = Anim->MovieScene->GetPlaybackRange().GetUpperBoundValue();
                    float Duration = (End - Start).Value / FrameRate.AsDecimal();
                    AnimInfo->SetNumberField(TEXT("durationSeconds"), Duration);
                    int32 TotalTracks = 0;
                    for (const FMovieSceneBinding& Bind : static_cast<const UMovieScene*>(Anim->MovieScene)->GetBindings())
                    {
                        TotalTracks += Bind.GetTracks().Num();
                    }
                    AnimInfo->SetNumberField(TEXT("trackCount"), TotalTracks);
                    AnimInfo->SetNumberField(TEXT("bindingCount"), static_cast<const UMovieScene*>(Anim->MovieScene)->GetBindings().Num());
                }
                AnimationsArray.Add(MakeShareable(new FJsonValueObject(AnimInfo)));
            }
        }
        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetStringField(TEXT("widgetPath"), WidgetPath);
        ResultJson->SetArrayField(TEXT("animations"), AnimationsArray);
        ResultJson->SetNumberField(TEXT("animationCount"), WidgetBP->Animations.Num());
    }
    else
    {
        // Return info for specific animation
        UWidgetAnimation* TargetAnim = nullptr;
        for (UWidgetAnimation* Anim : WidgetBP->Animations)
        {
            if (Anim && Anim->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
            {
                TargetAnim = Anim;
                break;
            }
        }

        if (!TargetAnim)
        {
            Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
            return true;
        }

        ResultJson->SetBoolField(TEXT("success"), true);
        ResultJson->SetStringField(TEXT("widgetPath"), WidgetPath);
        ResultJson->SetStringField(TEXT("animationName"), AnimationName);

        if (TargetAnim->MovieScene)
        {
            AddMovieSceneIdentity(ResultJson, TargetAnim);
            FFrameRate FrameRate = TargetAnim->MovieScene->GetTickResolution();
            FFrameNumber Start = TargetAnim->MovieScene->GetPlaybackRange().GetLowerBoundValue();
            FFrameNumber End = TargetAnim->MovieScene->GetPlaybackRange().GetUpperBoundValue();
            float Duration = (End - Start).Value / FrameRate.AsDecimal();

            ResultJson->SetNumberField(TEXT("durationSeconds"), Duration);
            ResultJson->SetNumberField(TEXT("frameRate"), FrameRate.AsDecimal());
            ResultJson->SetNumberField(TEXT("startFrame"), Start.Value);
            ResultJson->SetNumberField(TEXT("endFrame"), End.Value);

            TArray<TSharedPtr<FJsonValue>> BindingsArray;
            for (const FMovieSceneBinding& Bind : static_cast<const UMovieScene*>(TargetAnim->MovieScene)->GetBindings())
            {
                TSharedPtr<FJsonObject> BindInfo = MakeShared<FJsonObject>();
                BindInfo->SetStringField(TEXT("bindingGuid"), Bind.GetObjectGuid().ToString());

                FString BindingName;
                if (const FMovieScenePossessable* Possessable = TargetAnim->MovieScene->FindPossessable(Bind.GetObjectGuid()))
                {
                    BindingName = Possessable->GetName();
                }
                else if (const FMovieSceneSpawnable* Spawnable = TargetAnim->MovieScene->FindSpawnable(Bind.GetObjectGuid()))
                {
                    BindingName = Spawnable->GetName();
                }
                BindInfo->SetStringField(TEXT("bindingName"), BindingName);

                // Cross-reference AnimationBindings for widget name
                FString WidgetName = TEXT("Unknown");
                for (const FWidgetAnimationBinding& AnimBind : TargetAnim->AnimationBindings)
                {
                    if (AnimBind.AnimationGuid == Bind.GetObjectGuid())
                    {
                        WidgetName = AnimBind.WidgetName.ToString();
                        break;
                    }
                }
                BindInfo->SetStringField(TEXT("widgetName"), WidgetName);

                TArray<TSharedPtr<FJsonValue>> TracksArray;
                for (UMovieSceneTrack* Track : Bind.GetTracks())
                {
                    if (Track)
                    {
                        TSharedPtr<FJsonObject> TrackInfo = MakeShared<FJsonObject>();
                        TrackInfo->SetStringField(TEXT("name"), Track->GetTrackName().ToString());
                        TrackInfo->SetStringField(TEXT("type"), Track->GetClass()->GetName());
                        UMovieScenePropertyTrack* PropTrack = Cast<UMovieScenePropertyTrack>(Track);
                        if (PropTrack)
                        {
                            TrackInfo->SetStringField(TEXT("propertyName"), PropTrack->GetPropertyName().ToString());
                        }
                        TrackInfo->SetNumberField(TEXT("sectionCount"), Track->GetAllSections().Num());
                        TracksArray.Add(MakeShared<FJsonValueObject>(TrackInfo));
                    }
                }
                BindInfo->SetArrayField(TEXT("tracks"), TracksArray);
                BindingsArray.Add(MakeShared<FJsonValueObject>(BindInfo));
            }
            ResultJson->SetArrayField(TEXT("bindings"), BindingsArray);
            if (bIncludeEvents)
            {
                WidgetAnimationEventIntrospection::AppendAnimationEventMetadata(WidgetBP, TargetAnim, ResultJson);
            }
        }
    }

    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.remove_animation_binding ----
REGISTER_RPC_HANDLER("widget.remove_animation_binding", "widget", "Remove a specific widget binding from an animation",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_OPT("bindingGuid", "string", "GUID of the binding to remove"),
        RPC_PARAM_OPT("widgetName", "string", "Widget name to remove (first match)")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    FString BindingGuidStr = Ctx.GetString(TEXT("bindingGuid"));
    FString WidgetName = Ctx.GetString(TEXT("widgetName"));

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName"));
        return true;
    }

    if (BindingGuidStr.IsEmpty() && WidgetName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Must provide either bindingGuid or widgetName to identify the binding"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidgetAnimation* Animation = FindAnimationByName(WidgetBP, AnimationName);
    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("ANIMATION_INVALID"), TEXT("Animation has no MovieScene"));
        return true;
    }

    // Resolve which binding to remove
    FGuid TargetGuid;
    FString RemovedWidgetName;

    if (!BindingGuidStr.IsEmpty())
    {
        FGuid::Parse(BindingGuidStr, TargetGuid);
    }

    if (!TargetGuid.IsValid() && !WidgetName.IsEmpty())
    {
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.WidgetName.ToString().Equals(WidgetName, ESearchCase::IgnoreCase))
            {
                TargetGuid = Binding.AnimationGuid;
                RemovedWidgetName = Binding.WidgetName.ToString();
                break;
            }
        }
    }

    if (!TargetGuid.IsValid())
    {
        Ctx.SendError(TEXT("BINDING_NOT_FOUND"), TEXT("Could not find a matching binding to remove"));
        return true;
    }

    // Get widget name for response if resolved by GUID
    if (RemovedWidgetName.IsEmpty())
    {
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.AnimationGuid == TargetGuid)
            {
                RemovedWidgetName = Binding.WidgetName.ToString();
                break;
            }
        }
    }

    // Remove possessable from MovieScene (also removes all tracks on this binding)
    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.remove_animation_binding")));
    MovieScene->RemovePossessable(TargetGuid);

    // Remove matching entry from AnimationBindings
    Animation->AnimationBindings.RemoveAll([&](const FWidgetAnimationBinding& B)
    {
        return B.AnimationGuid == TargetGuid;
    });

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("animationName"), AnimationName);
    ResultJson->SetStringField(TEXT("removedBindingGuid"), TargetGuid.ToString());
    ResultJson->SetStringField(TEXT("removedWidgetName"), RemovedWidgetName);
    ResultJson->SetNumberField(TEXT("remainingBindings"), Animation->AnimationBindings.Num());

    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.set_animation_playback_range ----
REGISTER_RPC_HANDLER("widget.set_animation_playback_range", "widget", "Set the playback range (duration) of a widget animation",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_REQ("duration", "number", "Duration in seconds")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));
    double Duration = Ctx.GetNumber(TEXT("duration"), 1.0);

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName"));
        return true;
    }

    if (Duration <= 0.0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Duration must be greater than 0"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidgetAnimation* Animation = FindAnimationByName(WidgetBP, AnimationName);
    if (!Animation)
    {
        Ctx.SendError(TEXT("ANIMATION_NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("ANIMATION_INVALID"), TEXT("Animation has no MovieScene"));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.set_animation_playback_range")));
    const FFrameRate TickRes = MovieScene->GetTickResolution();
    const FFrameNumber EndFrame = TickRes.AsFrameTime(Duration).RoundToFrame();
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("animationName"), AnimationName);
    ResultJson->SetNumberField(TEXT("duration"), Duration);
    ResultJson->SetNumberField(TEXT("endFrame"), EndFrame.Value);
    ResultJson->SetNumberField(TEXT("tickResolution"), TickRes.AsDecimal());

    Ctx.SendSuccess(ResultJson);
    return true;
}

// ---- widget.delete_animation ----
REGISTER_RPC_HANDLER("widget.delete_animation", "widget", "Delete a widget animation from a blueprint",
    RPC_PARAMS(
        WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation to delete")
    ))
{
    FString WidgetPath = Ctx.GetStringFirstOf(WidgetAssetPathParamNames());
    FString AnimationName = Ctx.GetString(TEXT("animationName"));

    if (WidgetPath.IsEmpty() || AnimationName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameters: widgetPath, animationName"));
        return true;
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    int32 FoundIndex = INDEX_NONE;
    for (int32 i = 0; i < WidgetBP->Animations.Num(); ++i)
    {
        if (WidgetBP->Animations[i] && WidgetBP->Animations[i]->GetName().Equals(AnimationName, ESearchCase::IgnoreCase))
        {
            FoundIndex = i;
            break;
        }
    }

    if (FoundIndex == INDEX_NONE)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
        return true;
    }

    // Remove animation variable mapping (reverse of EnsureAnimationVariableGuid)
    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.delete_animation")));
    FName AnimFName = WidgetBP->Animations[FoundIndex]->GetFName();
    // Routes through the version-guarded helper: a no-op on UE 5.4 (no GUID map),
    // a map prune on 5.5+. Reverse of EnsureAnimationVariableGuid.
    RemoveWidgetVariableGuid(WidgetBP, AnimFName);

    WidgetBP->Animations.RemoveAt(FoundIndex);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    McpSafeAssetSave(WidgetBP);

    TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
    ResultJson->SetBoolField(TEXT("success"), true);
    ResultJson->SetStringField(TEXT("widgetPath"), WidgetPath);
    ResultJson->SetStringField(TEXT("deletedAnimation"), AnimationName);
    ResultJson->SetNumberField(TEXT("remainingAnimations"), WidgetBP->Animations.Num());

    Ctx.SendSuccess(ResultJson);
    return true;
}
