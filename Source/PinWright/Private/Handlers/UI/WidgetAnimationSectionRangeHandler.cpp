// Copyright (c) 2026 Alexander Penkin. MIT License.

// widget.get_animation_section_ranges / widget.set_animation_section_range
//
// Read and edit the [start, end) range of the sections inside a UWidgetAnimation without
// re-creating the animation (widget.import_animations_json mode:replace does that, and drops
// binding guids and unsupported tracks). Each side of a range is either a tick frame or
// open ("unbounded"); UMG creates every new section fully open.
//
// The verb edits the BLUEPRINT's animation. The compiled class carries a duplicate
// (`<Anim>_INST`) that running widgets evaluate, and it only refreshes on a full compile.
// Compiling here would put a reinstancing compile on this verb's stack (see
// Dispatch/SafePoint.cpp family L), so instead every section reports whether the compiled
// copy matches the source, measured, and the response carries compileRequired.

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "PinWrightHelpers.h"
#include "ScopedTransaction.h"
#include "Utils/JsonUtils.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "WidgetBlueprint.h"

namespace WidgetAnimationSectionRangeHandler
{
    struct FSectionRef
    {
        UMovieSceneSection* Section = nullptr;
        UMovieSceneTrack* Track = nullptr;
        FGuid BindingGuid;          // invalid for a root (unbound) track
        FString WidgetName;
        int32 TrackIndex = INDEX_NONE;
        int32 SectionIndex = INDEX_NONE;
    };

    struct FFilter
    {
        FString WidgetName;
        FString PropertyName;
        int32 SectionIndex = INDEX_NONE;
    };

    // Same shape as widget.export_animations_json's `range` object, so a read here can be
    // compared field for field with an export.
    TSharedPtr<FJsonObject> MakeRangeJson(const TRange<FFrameNumber>& Range)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (Range.HasLowerBound())
        {
            Obj->SetNumberField(TEXT("startFrame"), Range.GetLowerBoundValue().Value);
        }
        else
        {
            Obj->SetBoolField(TEXT("startBounded"), false);
        }
        if (Range.HasUpperBound())
        {
            Obj->SetNumberField(TEXT("endFrame"), Range.GetUpperBoundValue().Value);
        }
        else
        {
            Obj->SetBoolField(TEXT("endBounded"), false);
        }
        return Obj;
    }

    FString PropertyPathOf(const UMovieSceneTrack* Track)
    {
        const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
        return PropertyTrack ? PropertyTrack->GetPropertyPath().ToString() : FString();
    }

    bool TrackMatches(const UMovieSceneTrack* Track, const FString& Wanted)
    {
        if (Wanted.IsEmpty())
        {
            return true;
        }
        if (const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
        {
            if (PropertyTrack->GetPropertyPath().ToString().Equals(Wanted, ESearchCase::IgnoreCase)
                || PropertyTrack->GetPropertyName().ToString().Equals(Wanted, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        // GetName, not GetTrackName: the latter is NAME_None for non-property tracks (see
        // SequenceHandler.cpp GetTrackIdentifier), and this is the name `trackName` reports.
        return Track->GetName().Equals(Wanted, ESearchCase::IgnoreCase);
    }

    FString WidgetNameForBinding(const UWidgetAnimation* Animation, const FGuid& Guid)
    {
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.AnimationGuid == Guid)
            {
                return Binding.WidgetName.ToString();
            }
        }
        return FString();
    }

    template <typename TrackArrayType>
    void CollectFromTracks(const TrackArrayType& Tracks, const FGuid& Guid, const FString& WidgetName,
        const FFilter& Filter, TArray<FSectionRef>& Out)
    {
        for (int32 TrackIndex = 0; TrackIndex < Tracks.Num(); ++TrackIndex)
        {
            UMovieSceneTrack* Track = Tracks[TrackIndex];
            if (!Track || !TrackMatches(Track, Filter.PropertyName))
            {
                continue;
            }
            const auto& Sections = Track->GetAllSections();
            for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
            {
                if (!Sections[SectionIndex]
                    || (Filter.SectionIndex != INDEX_NONE && Filter.SectionIndex != SectionIndex))
                {
                    continue;
                }
                FSectionRef& Ref = Out.AddDefaulted_GetRef();
                Ref.Section = Sections[SectionIndex];
                Ref.Track = Track;
                Ref.BindingGuid = Guid;
                Ref.WidgetName = WidgetName;
                Ref.TrackIndex = TrackIndex;
                Ref.SectionIndex = SectionIndex;
            }
        }
    }

    // Every section of the animation in binding order, then root tracks. A widgetName filter
    // excludes root tracks (they bind no widget).
    TArray<FSectionRef> CollectSections(UWidgetAnimation* Animation, UMovieScene* MovieScene, const FFilter& Filter)
    {
        TArray<FSectionRef> Out;
        for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            const FString WidgetName = WidgetNameForBinding(Animation, Binding.GetObjectGuid());
            if (!Filter.WidgetName.IsEmpty() && !WidgetName.Equals(Filter.WidgetName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            CollectFromTracks(Binding.GetTracks(), Binding.GetObjectGuid(), WidgetName, Filter, Out);
        }
        if (Filter.WidgetName.IsEmpty())
        {
            CollectFromTracks(MovieScene->GetTracks(), FGuid(), FString(), Filter, Out);
        }
        return Out;
    }

    // The same section inside the compiled class's `<Anim>_INST` duplicate, located by
    // binding guid + track index + section index (DuplicateObject preserves all three).
    const UMovieSceneSection* FindCompiledSection(const UWidgetBlueprint* WidgetBP,
        const UWidgetAnimation* Animation, const FSectionRef& Ref)
    {
        const UWidgetBlueprintGeneratedClass* GeneratedClass =
            Cast<UWidgetBlueprintGeneratedClass>(WidgetBP->GeneratedClass);
        if (!GeneratedClass)
        {
            return nullptr;
        }
        const FName CopyName(*(Animation->GetName() + TEXT("_INST")));
        for (UWidgetAnimation* Copy : GeneratedClass->Animations)
        {
            UMovieScene* CopyScene = (Copy && Copy->GetFName() == CopyName) ? Copy->GetMovieScene() : nullptr;
            if (!CopyScene)
            {
                continue;
            }
            const UMovieSceneTrack* Track = nullptr;
            if (Ref.BindingGuid.IsValid())
            {
                const FMovieSceneBinding* Binding = CopyScene->FindBinding(Ref.BindingGuid);
                if (Binding && Binding->GetTracks().IsValidIndex(Ref.TrackIndex))
                {
                    Track = Binding->GetTracks()[Ref.TrackIndex];
                }
            }
            else if (CopyScene->GetTracks().IsValidIndex(Ref.TrackIndex))
            {
                Track = CopyScene->GetTracks()[Ref.TrackIndex];
            }
            if (!Track || Track->GetClass() != Ref.Track->GetClass()
                || !Track->GetAllSections().IsValidIndex(Ref.SectionIndex))
            {
                return nullptr;
            }
            return Track->GetAllSections()[Ref.SectionIndex];
        }
        return nullptr;
    }

    // Writes the section's identity, range, key extent and compiled-copy state. Returns
    // whether the compiled copy carries the same range (false when there is no copy).
    bool DescribeSection(const UWidgetBlueprint* WidgetBP, const UWidgetAnimation* Animation,
        const FSectionRef& Ref, const TSharedPtr<FJsonObject>& Obj)
    {
        Obj->SetStringField(TEXT("widgetName"), Ref.WidgetName);
        Obj->SetStringField(TEXT("bindingGuid"), Ref.BindingGuid.IsValid() ? Ref.BindingGuid.ToString() : FString());
        Obj->SetStringField(TEXT("trackType"), Ref.Track->GetClass()->GetName());
        Obj->SetStringField(TEXT("trackName"), Ref.Track->GetName());
        Obj->SetStringField(TEXT("propertyPath"), PropertyPathOf(Ref.Track));
        Obj->SetNumberField(TEXT("trackIndex"), Ref.TrackIndex);
        Obj->SetNumberField(TEXT("sectionIndex"), Ref.SectionIndex);
        Obj->SetBoolField(TEXT("locked"), Ref.Section->IsLocked());

        const TRange<FFrameNumber> Range = Ref.Section->GetRange();
        Obj->SetObjectField(TEXT("range"), MakeRangeJson(Range));

        // A key outside the section never evaluates: the reported defect is a section ending
        // exclusively AT its final key, so that key is skipped on the last tick.
        TArray<FFrameNumber> KeyTimes;
        for (const FMovieSceneChannelEntry& Entry : Ref.Section->GetChannelProxy().GetAllEntries())
        {
            for (FMovieSceneChannel* Channel : Entry.GetChannels())
            {
                if (Channel)
                {
                    Channel->GetKeys(TRange<FFrameNumber>::All(), &KeyTimes, nullptr);
                }
            }
        }
        Obj->SetNumberField(TEXT("keyCount"), KeyTimes.Num());
        if (KeyTimes.Num() > 0)
        {
            FFrameNumber First = KeyTimes[0];
            FFrameNumber Last = KeyTimes[0];
            for (const FFrameNumber Time : KeyTimes)
            {
                First = FMath::Min(First, Time);
                Last = FMath::Max(Last, Time);
            }
            Obj->SetNumberField(TEXT("firstKeyFrame"), First.Value);
            Obj->SetNumberField(TEXT("lastKeyFrame"), Last.Value);
            Obj->SetBoolField(TEXT("keysInsideSection"), Range.Contains(First) && Range.Contains(Last));
        }

        const UMovieSceneSection* Compiled = FindCompiledSection(WidgetBP, Animation, Ref);
        const bool bMatches = Compiled && Compiled->GetRange() == Range;
        if (Compiled)
        {
            Obj->SetObjectField(TEXT("compiledRange"), MakeRangeJson(Compiled->GetRange()));
        }
        Obj->SetBoolField(TEXT("compiledCopyMatches"), bMatches);
        return bMatches;
    }

    bool ReadFilter(const FHandlerContext& Ctx, FFilter& Out)
    {
        Out.WidgetName = Ctx.GetString(TEXT("widgetName"));
        Out.PropertyName = Ctx.GetString(TEXT("propertyName"));
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (Payload.IsValid() && Payload->HasField(TEXT("sectionIndex")))
        {
            int64 Index = 0;
            FString Error;
            if (!TryParseStrictJsonInteger(Payload->TryGetField(TEXT("sectionIndex")), 0, MAX_int32, Index, Error))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMETER,
                    FString::Printf(TEXT("sectionIndex must be a non-negative integer: %s"), *Error));
                return false;
            }
            Out.SectionIndex = static_cast<int32>(Index);
        }
        return true;
    }

    // Resolves widgetPath + animationName + filters to a non-empty section set, or sends the error.
    bool Resolve(const FHandlerContext& Ctx, UWidgetBlueprint*& OutBP, UWidgetAnimation*& OutAnimation,
        UMovieScene*& OutMovieScene, TArray<FSectionRef>& OutSections)
    {
        FFilter Filter;
        if (!ReadFilter(Ctx, Filter))
        {
            return false;
        }
        OutBP = WidgetAuthoringHelpers::LoadWidgetBlueprint(
            Ctx.GetStringFirstOf(WidgetHandlerUtils::WidgetAssetPathParamNames()));
        if (!OutBP)
        {
            Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT("Widget blueprint not found"));
            return false;
        }
        const FString AnimationName = Ctx.GetString(TEXT("animationName"));
        OutAnimation = WidgetAuthoringHelpers::FindAnimationByName(OutBP, AnimationName);
        if (!OutAnimation)
        {
            Ctx.SendError(ErrorCodes::ERR_ANIMATION_NOT_FOUND,
                FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
            return false;
        }
        OutMovieScene = OutAnimation->GetMovieScene();
        if (!OutMovieScene)
        {
            Ctx.SendError(ErrorCodes::ERR_ANIMATION_INVALID, TEXT("Animation has no MovieScene"));
            return false;
        }
        OutSections = CollectSections(OutAnimation, OutMovieScene, Filter);
        if (OutSections.Num() == 0)
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetNumberField(TEXT("sectionsInAnimation"), CollectSections(OutAnimation, OutMovieScene, FFilter()).Num());
            Ctx.SendError(ErrorCodes::ERR_SECTION_NOT_FOUND,
                TEXT("No section matches widgetName/propertyName/sectionIndex. Call widget.get_animation_section_ranges without filters to list them."),
                Data);
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeAnimationHeader(const UWidgetBlueprint* WidgetBP, const UWidgetAnimation* Animation,
        const UMovieScene* MovieScene)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widgetPath"), WidgetBP->GetOutermost()->GetName());
        Result->SetStringField(TEXT("animationName"), Animation->GetName());
        Result->SetNumberField(TEXT("tickResolution"), MovieScene->GetTickResolution().AsDecimal());
        Result->SetNumberField(TEXT("displayRate"), MovieScene->GetDisplayRate().AsDecimal());
        Result->SetObjectField(TEXT("playbackRange"), MakeRangeJson(MovieScene->GetPlaybackRange()));
        return Result;
    }

    // One side of the requested range: absent (keep), "unbounded" (open), or a value in Units.
    // Returns false after sending INVALID_PARAMETER.
    bool ReadBoundArg(const FHandlerContext& Ctx, const TCHAR* Field, const FString& Units,
        const UMovieScene* MovieScene, bool& bOutPresent, bool& bOutOpen, FFrameNumber& OutFrame)
    {
        bOutPresent = false;
        bOutOpen = false;
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const TSharedPtr<FJsonValue> Value = Payload.IsValid() ? Payload->TryGetField(Field) : nullptr;
        if (!Value.IsValid())
        {
            return true;
        }
        bOutPresent = true;
        FString Text;
        if (Value->TryGetString(Text) && Text.Equals(TEXT("unbounded"), ESearchCase::IgnoreCase))
        {
            bOutOpen = true;
            return true;
        }
        double Number = 0.0;
        FString Error;
        if (!TryParseStrictJsonNumber(Value, Number, Error))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMETER,
                FString::Printf(TEXT("%s must be a number or \"unbounded\": %s"), Field, *Error));
            return false;
        }
        const double TickRate = MovieScene->GetTickResolution().AsDecimal();
        double Ticks = Number;
        if (Units == TEXT("seconds"))
        {
            Ticks = Number * TickRate;
        }
        else if (Units == TEXT("displayFrames"))
        {
            Ticks = Number * TickRate / MovieScene->GetDisplayRate().AsDecimal();
        }
        else if (FMath::Frac(Number) != 0.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMETER,
                FString::Printf(TEXT("%s is in ticks and must be an integer (got %f)."), Field, Number));
            return false;
        }
        if (FMath::Abs(Ticks) > static_cast<double>(MAX_int32 - 1))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMETER,
                FString::Printf(TEXT("%s is outside the frame-number range at this tick resolution."), Field));
            return false;
        }
        OutFrame = FFrameNumber(FMath::RoundToInt32(Ticks));
        return true;
    }
}

// ---- widget.get_animation_section_ranges ----
REGISTER_RPC_HANDLER("widget.get_animation_section_ranges", "widget",
    "Read the [start, end) range of each section in a widget animation (tick frames; open sides reported as startBounded/endBounded false), with key extent, whether every key lies inside the section, and whether the compiled class's copy carries the same range. Read-only.",
    RPC_PARAMS(
        WidgetHandlerUtils::WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_OPT("widgetName", "string", "Only sections on tracks bound to this widget (excludes root tracks)"),
        RPC_PARAM_OPT("propertyName", "string", "Only tracks whose property path, property name or trackName (the track object name) matches (case-insensitive)"),
        RPC_PARAM_OPT("sectionIndex", "integer", "Only the section at this index within each matched track")
    ))
{
    using namespace WidgetAnimationSectionRangeHandler;
    UWidgetBlueprint* WidgetBP = nullptr;
    UWidgetAnimation* Animation = nullptr;
    UMovieScene* MovieScene = nullptr;
    TArray<FSectionRef> Sections;
    if (!Resolve(Ctx, WidgetBP, Animation, MovieScene, Sections))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeAnimationHeader(WidgetBP, Animation, MovieScene);
    TArray<TSharedPtr<FJsonValue>> SectionValues;
    bool bAllCompiledMatch = true;
    for (const FSectionRef& Ref : Sections)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        bAllCompiledMatch &= DescribeSection(WidgetBP, Animation, Ref, Obj);
        SectionValues.Add(MakeShared<FJsonValueObject>(Obj));
    }
    Result->SetArrayField(TEXT("sections"), SectionValues);
    Result->SetNumberField(TEXT("sectionCount"), Sections.Num());
    Result->SetBoolField(TEXT("compileRequired"), !bAllCompiledMatch);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- widget.set_animation_section_range ----
REGISTER_RPC_HANDLER("widget.set_animation_section_range", "widget",
    "Set the start and/or end of every matched section in a widget animation, each side a value or \"unbounded\" (open, the UMG default). One undoable transaction, validated before any write, read back per section. Edits the blueprint's animation; compileRequired reports (measured) that the compiled class still carries the old range until blueprint.compile.",
    RPC_PARAMS(
        WidgetHandlerUtils::WidgetAssetPathParamReq(TEXT("Path to the widget blueprint")),
        RPC_PARAM_REQ("animationName", "string", "Name of the animation"),
        RPC_PARAM_OPT("widgetName", "string", "Only sections on tracks bound to this widget (excludes root tracks)"),
        RPC_PARAM_OPT("propertyName", "string", "Only tracks whose property path, property name or trackName (the track object name) matches (case-insensitive)"),
        RPC_PARAM_OPT("sectionIndex", "integer", "Only the section at this index within each matched track"),
        RPC_PARAM_OPT("start", "number|string", "Inclusive start in `units`, or \"unbounded\". Omit to keep the current start"),
        RPC_PARAM_OPT("end", "number|string", "Exclusive end in `units`, or \"unbounded\". Omit to keep the current end"),
        RPC_PARAM_DEF("units", "string", "ticks (the frame numbers export_animations_json reports) | displayFrames | seconds", "ticks")
    ))
{
    using namespace WidgetAnimationSectionRangeHandler;
    const FString Units = Ctx.GetString(TEXT("units"), TEXT("ticks"));
    if (Units != TEXT("ticks") && Units != TEXT("displayFrames") && Units != TEXT("seconds"))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMETER,
            FString::Printf(TEXT("units must be ticks, displayFrames or seconds (got '%s')."), *Units));
        return true;
    }

    UWidgetBlueprint* WidgetBP = nullptr;
    UWidgetAnimation* Animation = nullptr;
    UMovieScene* MovieScene = nullptr;
    TArray<FSectionRef> Sections;
    if (!Resolve(Ctx, WidgetBP, Animation, MovieScene, Sections))
    {
        return true;
    }

    bool bStartPresent = false, bStartOpen = false, bEndPresent = false, bEndOpen = false;
    FFrameNumber StartFrame, EndFrame;
    if (!ReadBoundArg(Ctx, TEXT("start"), Units, MovieScene, bStartPresent, bStartOpen, StartFrame)
        || !ReadBoundArg(Ctx, TEXT("end"), Units, MovieScene, bEndPresent, bEndOpen, EndFrame))
    {
        return true;
    }
    if (!bStartPresent && !bEndPresent)
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PARAMETER,
            TEXT("Pass start and/or end (a value or \"unbounded\"). To read ranges use widget.get_animation_section_ranges."));
        return true;
    }

    // Validate every section before writing any, so a refusal leaves the animation untouched.
    TArray<TRange<FFrameNumber>> Requested;
    for (const FSectionRef& Ref : Sections)
    {
        const TRange<FFrameNumber> Current = Ref.Section->GetRange();
        const TRangeBound<FFrameNumber> Lower = !bStartPresent ? Current.GetLowerBound()
            : bStartOpen ? TRangeBound<FFrameNumber>::Open() : TRangeBound<FFrameNumber>::Inclusive(StartFrame);
        const TRangeBound<FFrameNumber> Upper = !bEndPresent ? Current.GetUpperBound()
            : bEndOpen ? TRangeBound<FFrameNumber>::Open() : TRangeBound<FFrameNumber>::Exclusive(EndFrame);
        const TRange<FFrameNumber> NewRange(Lower, Upper);
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetNumberField(TEXT("trackIndex"), Ref.TrackIndex);
        Data->SetNumberField(TEXT("sectionIndex"), Ref.SectionIndex);
        Data->SetObjectField(TEXT("currentRange"), MakeRangeJson(Current));
        if (NewRange.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMETER,
                TEXT("The resulting range is empty (start must be before end). Nothing was changed."), Data);
            return true;
        }
        // Same test UMovieSceneSection::TryModify applies, so SetRange cannot silently no-op below.
        if (Ref.Section->IsLocked() || MovieScene->IsReadOnly())
        {
            Ctx.SendError(ErrorCodes::ERR_SECTION_READ_ONLY,
                TEXT("A matched section is locked or the animation's MovieScene is read-only. Narrow the match or unlock it. Nothing was changed."), Data);
            return true;
        }
        Requested.Add(NewRange);
    }

    TSharedPtr<FJsonObject> Result = MakeAnimationHeader(WidgetBP, Animation, MovieScene);
    TArray<TSharedPtr<FJsonValue>> SectionValues;
    TArray<TSharedPtr<FJsonObject>> SectionObjects;
    int32 ChangedCount = 0;
    int32 MismatchCount = 0;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.set_animation_section_range")));
        for (int32 Index = 0; Index < Sections.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetObjectField(TEXT("before"), MakeRangeJson(Sections[Index].Section->GetRange()));
            Obj->SetObjectField(TEXT("requested"), MakeRangeJson(Requested[Index]));
            const bool bWasEqual = Sections[Index].Section->GetRange() == Requested[Index];
            Sections[Index].Section->SetRange(Requested[Index]);
            const bool bApplied = Sections[Index].Section->GetRange() == Requested[Index];
            Obj->SetBoolField(TEXT("applied"), bApplied);
            Obj->SetBoolField(TEXT("changed"), bApplied && !bWasEqual);
            ChangedCount += (bApplied && !bWasEqual) ? 1 : 0;
            MismatchCount += bApplied ? 0 : 1;
            SectionObjects.Add(Obj);
        }
    }

    bool bAllCompiledMatch = true;
    for (int32 Index = 0; Index < Sections.Num(); ++Index)
    {
        // `range` is the post-write readback, so it doubles as `after`.
        bAllCompiledMatch &= DescribeSection(WidgetBP, Animation, Sections[Index], SectionObjects[Index]);
        SectionValues.Add(MakeShared<FJsonValueObject>(SectionObjects[Index]));
    }
    Result->SetArrayField(TEXT("sections"), SectionValues);
    Result->SetNumberField(TEXT("sectionCount"), Sections.Num());
    Result->SetNumberField(TEXT("changedCount"), ChangedCount);
    Result->SetStringField(TEXT("units"), Units);

    // Before the VERIFICATION_FAILED return: sections that did change must still dirty the asset.
    if (ChangedCount > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);
        McpSafeAssetSave(WidgetBP);
    }
    Result->SetBoolField(TEXT("compileRequired"), !bAllCompiledMatch);
    AddMarkDirtySaveReport(Result, WidgetBP, ChangedCount > 0);
    if (MismatchCount > 0)
    {
        Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("%d section(s) did not take the requested range. See sections[].applied."),
                MismatchCount),
            Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}
