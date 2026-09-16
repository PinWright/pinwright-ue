// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-sequencer-curve-channel-ops (reworded to the interp/tangent core).
//
// sequencer.add_keyframe historically wrote every float key with hardcoded cubic interpolation
// (FMovieSceneFloatChannel::AddCubicKey), so RPC callers could not author constant (holds/stepped)
// or linear keys — generated cinematics always looked mechanical. The fix adds an optional `interp`
// param (constant|linear|cubic, default cubic) that routes to AddConstantKey / AddLinearKey /
// AddCubicKey, and surfaces the per-key interp mode through the sequencer.list_sections
// includeKeys=true readback (MovieSceneJsonUtils::BuildChannelKeysJson).
//
// This test drives the PRODUCTION sequencer.add_keyframe handler through the real registration list
// (InvokeHandlerWithCapture) three times on ONE float property — interp=constant, linear, cubic at
// distinct times — then verifies BOTH halves of the fix:
//   1. WRITE side: the FMovieSceneFloatValue stored on the channel carries the requested InterpMode
//      (RCIM_Constant / RCIM_Linear / RCIM_Cubic), matched per key by its tick frame. Pre-fix every
//      key landed RCIM_Cubic, so the constant/linear assertions FAIL.
//   2. READ side: sequencer.list_sections with includeKeys=true reports a per-key `interp` token that
//      round-trips the authored mode. Pre-fix the readback emitted only {frame, value} with no
//      `interp` field, so that assertion FAILS.
//
// Fixture is content-free: a real /Game LevelSequence created via the registered sequencer.create
// handler (so add_keyframe's LoadObject<ULevelSequence> resolves it) with a possessable binding added
// in-code (add_keyframe needs only FindBinding to succeed; the float track/section it authors does
// not require a bound object instance). No example/Lyra asset is loaded.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Curves/RichCurve.h"

#include "Tests/TestUtils.h"

namespace
{
    // Distinctly named (mirrors CreateAddKeyframeUnitsSequence in the sibling seconds-as-ticks test)
    // so anonymous-namespace symbols don't ODR-collide when Unity merges these TUs: create a real
    // /Game LevelSequence via the registered sequencer.create handler so add_keyframe's
    // LoadObject<ULevelSequence> can resolve it. Empty path + nullptr on failure.
    ULevelSequence* CreateAddKeyframeInterpSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_AddKeyframeInterpSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_AddKeyframeInterpProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create — "
                               "the add_keyframe interp repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Returns the first float section on the binding's float track for PropertyName, mirroring the
    // handler's OWN track lookup (Cast<UMovieSceneFloatTrack> + case-insensitive GetPropertyName
    // match), so the exact channel the handler wrote is the one inspected. nullptr if absent.
    UMovieSceneFloatSection* FindInterpFloatSection(UMovieScene* MovieScene, const FGuid& BindingGuid,
        const FString& PropertyName)
    {
        FMovieSceneBinding* Binding = MovieScene ? MovieScene->FindBinding(BindingGuid) : nullptr;
        if (!Binding)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* T : Binding->GetTracks())
        {
            UMovieSceneFloatTrack* FT = Cast<UMovieSceneFloatTrack>(T);
            if (!FT || !FT->GetPropertyName().ToString().Equals(PropertyName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            const TArray<UMovieSceneSection*>& Sections = FT->GetAllSections();
            if (Sections.Num() == 0)
            {
                return nullptr;
            }
            return Cast<UMovieSceneFloatSection>(Sections[0]);
        }
        return nullptr;
    }

    // One authoring case: request `Interp` at `Seconds`, expect the channel to store `Expected`.
    struct FInterpCase
    {
        double Seconds;
        const TCHAR* Interp;
        ERichCurveInterpMode Expected;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddKeyframeInterpTest,
    "PinWright.Sequencer.AddKeyframe.InterpModeApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddKeyframeInterpTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateAddKeyframeInterpSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    // A possessable binding so add_keyframe's FindBinding resolves. The handler needs only a valid
    // binding GUID; the float track/section it authors doesn't require a bound object instance.
    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_AddKeyframeInterpActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    const FString PropertyName = TEXT("Intensity");
    const FInterpCase Cases[] = {
        { 1.0, TEXT("constant"), RCIM_Constant },
        { 2.0, TEXT("linear"),   RCIM_Linear },
        { 3.0, TEXT("cubic"),    RCIM_Cubic },
    };

    // Author each key through the production handler with an explicit interp mode.
    for (const FInterpCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("sequencePath"), FullPath);
        AddPayload->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
        AddPayload->SetStringField(TEXT("propertyName"), PropertyName);
        AddPayload->SetNumberField(TEXT("time"), Case.Seconds);
        AddPayload->SetNumberField(TEXT("value"), 1.0);
        AddPayload->SetStringField(TEXT("interp"), Case.Interp);

        FTestResponseCapture AddCapture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.add_keyframe"), AddPayload, AddCapture);
        if (!TestTrue(TEXT("sequencer.add_keyframe handler is registered and invoked"), bFound))
        {
            return false;
        }
        if (!TestTrue(*FString::Printf(TEXT("add_keyframe(interp=%s) reported success"), Case.Interp),
                AddCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_keyframe(interp=%s) failed (code=%s msg=%s)"),
                Case.Interp, *AddCapture.ErrorCode, *AddCapture.Message));
            return false;
        }
        // The response must echo the applied interp so a caller confirms it without a readback.
        FString EchoedInterp;
        if (AddCapture.Result.IsValid() && AddCapture.Result->TryGetStringField(TEXT("interp"), EchoedInterp))
        {
            TestEqual(*FString::Printf(TEXT("add_keyframe echoes interp=%s"), Case.Interp),
                EchoedInterp, FString(Case.Interp));
        }
        else
        {
            AddError(FString::Printf(TEXT("add_keyframe(interp=%s) response omits the applied interp echo"),
                Case.Interp));
        }
    }

    // ---- WRITE side: the stored FMovieSceneFloatValue.InterpMode must match the request per key ----
    UMovieSceneFloatSection* FloatSection = FindInterpFloatSection(MovieScene, BindingGuid, PropertyName);
    if (!TestNotNull(TEXT("add_keyframe authored a float section for the property"), FloatSection))
    {
        return false;
    }
    const TArrayView<const FFrameNumber> Times = FloatSection->GetChannel().GetData().GetTimes();
    const auto Values = FloatSection->GetChannel().GetData().GetValues();
    if (!TestEqual(TEXT("all three requested keys landed on one channel"), Times.Num(), 3))
    {
        return false;
    }
    // frame -> stored interp mode, so each case is matched by its tick frame (not by array order).
    TMap<int32, ERichCurveInterpMode> StoredByFrame;
    for (int32 i = 0; i < Times.Num() && i < Values.Num(); ++i)
    {
        StoredByFrame.Add(Times[i].Value, static_cast<ERichCurveInterpMode>(Values[i].InterpMode.GetValue()));
    }
    for (const FInterpCase& Case : Cases)
    {
        const int32 ExpectedFrame = TickResolution.AsFrameNumber(Case.Seconds).Value;
        const ERichCurveInterpMode* Stored = StoredByFrame.Find(ExpectedFrame);
        if (!TestNotNull(*FString::Printf(TEXT("a key exists at the tick frame for %s (%.1fs)"),
                Case.Interp, Case.Seconds), Stored))
        {
            continue;
        }
        // Pre-fix every key was written via AddCubicKey (RCIM_Cubic), so constant/linear fail here.
        TestEqual(*FString::Printf(TEXT("stored InterpMode for the %s key is the requested mode"), Case.Interp),
            static_cast<int32>(*Stored), static_cast<int32>(Case.Expected));
    }

    // ---- READ side: sequencer.list_sections includeKeys=true must surface a per-key `interp` token ----
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), FullPath);
    ListPayload->SetBoolField(TEXT("includeKeys"), true);
    FTestResponseCapture ListCapture;
    if (!TestTrue(TEXT("sequencer.list_sections handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.list_sections succeeded"), ListCapture.bSuccess) || !ListCapture.Result.IsValid())
    {
        return false;
    }

    // Build frame -> interp token from the readback's keyed channel (the one with all three keys).
    TMap<int32, FString> ReadbackByFrame;
    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    if (ListCapture.Result->TryGetArrayField(TEXT("sections"), Sections))
    {
        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            const TSharedPtr<FJsonObject> SectionObj = SectionValue.IsValid() ? SectionValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* ChannelsArr = nullptr;
            if (!SectionObj.IsValid() || !SectionObj->TryGetArrayField(TEXT("channels"), ChannelsArr))
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& ChannelValue : *ChannelsArr)
            {
                const TSharedPtr<FJsonObject> ChannelObj = ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
                if (!ChannelObj.IsValid() || !ChannelObj->TryGetArrayField(TEXT("keys"), Keys) || Keys->Num() != 3)
                {
                    continue;
                }
                for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                {
                    const TSharedPtr<FJsonObject> KeyObj = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                    if (!KeyObj.IsValid())
                    {
                        continue;
                    }
                    double Frame = 0.0;
                    FString Interp;
                    if (KeyObj->TryGetNumberField(TEXT("frame"), Frame) &&
                        KeyObj->TryGetStringField(TEXT("interp"), Interp))
                    {
                        ReadbackByFrame.Add(static_cast<int32>(Frame), Interp);
                    }
                }
            }
        }
    }

    // Pre-fix the readback carried no `interp` field at all, so this map is empty and every case fails.
    if (!TestEqual(TEXT("list_sections includeKeys surfaced a per-key interp token for all three keys"),
            ReadbackByFrame.Num(), 3))
    {
        return true; // assertions already recorded the failure
    }
    for (const FInterpCase& Case : Cases)
    {
        const int32 ExpectedFrame = TickResolution.AsFrameNumber(Case.Seconds).Value;
        const FString* Token = ReadbackByFrame.Find(ExpectedFrame);
        if (!TestNotNull(*FString::Printf(TEXT("readback has an interp token for the %s key"), Case.Interp), Token))
        {
            continue;
        }
        TestEqual(*FString::Printf(TEXT("readback interp token for the %s key round-trips the mode"), Case.Interp),
            *Token, FString(Case.Interp));
    }

    return true;
}
