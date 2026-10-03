// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-sequencer-display-rate-invalid.
//
// sequencer.set_display_rate used FCString::Atoi on every token and wrote whatever came out:
// "oopsfps" -> 0/1, "24/not-a-number" -> 24/0 (an invalid FFrameRate), 0 / -24 stored as-is,
// 29.97 silently rounded to 30 — each reported as success. Its frameRate slot was also
// declared `number`, so the wire gate refused the documented '30fps' / '24000/1001' forms.
//
// Both tests route through a real FRpcDispatcher (param gate included) and read the stored
// rate straight off the MovieScene. Counterfactuals: revert the strict parse and the
// numeric bad values (0, -24, 29.97, "0") are written with success; revert the slot type to
// `number` and '30fps' / '24000/1001' are refused by the gate.
#include "Misc/AutomationTest.h"
#include "Tests/AutomationSuiteMaintenance.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"

#include "Handlers/ErrorCodes.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

namespace TestSequencerSetDisplayRateStrictHelpers
{
    ULevelSequence* CreateDisplayRateProbeSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_DisplayRateSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = FString(PinWrightSuiteMaintenance::ScratchRootPackagePath()) / TEXT("MCP_DisplayRateProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bSuccess || !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(FString::Printf(TEXT("Fixture: sequencer.create failed (code=%s msg=%s)"),
                *CreateCapture.ErrorCode, *CreateCapture.Message));
            return nullptr;
        }
        ULevelSequence* Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
        if (!Sequence)
        {
            Test.AddError(FString::Printf(TEXT("Fixture: %s did not load as a ULevelSequence"), *OutFullPath));
        }
        return Sequence;
    }

    struct FRateCall
    {
        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
    };

    FRateCall SetRate(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& SeqPath, const TSharedPtr<FJsonValue>& FrameRate)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), SeqPath);
        Params->SetField(TEXT("frameRate"), FrameRate);
        FRateCall Call;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("sequencer.set_display_rate"),
            TEXT("req-set-display-rate"), Params, Call.bSuccess, Call.Result, Call.ErrorCode);
        return Call;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetDisplayRateMalformedRefusedTest,
    "PinWright.sequencer.set_display_rate.MalformedRateRefusedBeforeWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetDisplayRateMalformedRefusedTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ON_SCOPE_EXIT { if (!FullPath.IsEmpty()) { CleanupTestAsset(FullPath); } };
    ULevelSequence* Sequence = TestSequencerSetDisplayRateStrictHelpers::CreateDisplayRateProbeSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Known starting rate, distinct from every value below.
    const TestSequencerSetDisplayRateStrictHelpers::FRateCall Seed = TestSequencerSetDisplayRateStrictHelpers::SetRate(Dispatcher, Sink, FullPath, MakeShared<FJsonValueNumber>(25.0));
    if (!TestTrue(FString::Printf(TEXT("precondition: frameRate=25 accepted (code=%s)"), *Seed.ErrorCode), Seed.bSuccess)
        || !TestTrue(TEXT("precondition: stored rate is 25/1"), MovieScene->GetDisplayRate() == FFrameRate(25, 1)))
    {
        return false;
    }

    const TArray<TPair<FString, TSharedPtr<FJsonValue>>> BadValues = {
        { TEXT("\"oopsfps\""),         MakeShared<FJsonValueString>(TEXT("oopsfps")) },
        { TEXT("\"24/not-a-number\""), MakeShared<FJsonValueString>(TEXT("24/not-a-number")) },
        { TEXT("\"24/0\""),            MakeShared<FJsonValueString>(TEXT("24/0")) },
        { TEXT("\"0/1\""),             MakeShared<FJsonValueString>(TEXT("0/1")) },
        { TEXT("\"-24/1\""),           MakeShared<FJsonValueString>(TEXT("-24/1")) },
        { TEXT("\"24/1001/5\""),       MakeShared<FJsonValueString>(TEXT("24/1001/5")) },
        { TEXT("\"30x\""),             MakeShared<FJsonValueString>(TEXT("30x")) },
        { TEXT("\"30 fps\""),          MakeShared<FJsonValueString>(TEXT("30 fps")) },
        { TEXT("\"fps\""),             MakeShared<FJsonValueString>(TEXT("fps")) },
        { TEXT("\"0\""),               MakeShared<FJsonValueString>(TEXT("0")) },
        { TEXT("\"29.97\""),           MakeShared<FJsonValueString>(TEXT("29.97")) },
        { TEXT("0"),                   MakeShared<FJsonValueNumber>(0.0) },
        { TEXT("-24"),                 MakeShared<FJsonValueNumber>(-24.0) },
        { TEXT("29.97"),               MakeShared<FJsonValueNumber>(29.97) },
        { TEXT("true"),                MakeShared<FJsonValueBoolean>(true) },
    };

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Bad : BadValues)
    {
        const TestSequencerSetDisplayRateStrictHelpers::FRateCall Call = TestSequencerSetDisplayRateStrictHelpers::SetRate(Dispatcher, Sink, FullPath, Bad.Value);
        TestFalse(FString::Printf(TEXT("frameRate=%s is refused"), *Bad.Key), Call.bSuccess);
        TestEqual(FString::Printf(TEXT("frameRate=%s refusal code"), *Bad.Key),
            Call.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        const FFrameRate Stored = MovieScene->GetDisplayRate();
        TestTrue(FString::Printf(TEXT("frameRate=%s left the stored rate at 25/1 (got %d/%d)"),
            *Bad.Key, Stored.Numerator, Stored.Denominator), Stored == FFrameRate(25, 1));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSetDisplayRateDocumentedFormsTest,
    "PinWright.sequencer.set_display_rate.DocumentedFormsWriteExactRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSetDisplayRateDocumentedFormsTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ON_SCOPE_EXIT { if (!FullPath.IsEmpty()) { CleanupTestAsset(FullPath); } };
    ULevelSequence* Sequence = TestSequencerSetDisplayRateStrictHelpers::CreateDisplayRateProbeSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Seed 25/1 so every case below must change the stored rate to pass.
    const TestSequencerSetDisplayRateStrictHelpers::FRateCall Seed =
        TestSequencerSetDisplayRateStrictHelpers::SetRate(Dispatcher, Sink, FullPath, MakeShared<FJsonValueNumber>(25.0));
    if (!TestTrue(TEXT("precondition: stored rate is 25/1"), Seed.bSuccess && MovieScene->GetDisplayRate() == FFrameRate(25, 1)))
    {
        return false;
    }

    struct FGoodCase
    {
        const TCHAR* Label;
        TSharedPtr<FJsonValue> Value;
        FFrameRate Expected;
    };
    const TArray<FGoodCase> GoodValues = {
        { TEXT("\"30fps\""),      MakeShared<FJsonValueString>(TEXT("30fps")),      FFrameRate(30, 1) },
        { TEXT("\"24000/1001\""), MakeShared<FJsonValueString>(TEXT("24000/1001")), FFrameRate(24000, 1001) },
        { TEXT("\"30000/1001fps\""), MakeShared<FJsonValueString>(TEXT("30000/1001fps")), FFrameRate(30000, 1001) },
        { TEXT("\"48\""),         MakeShared<FJsonValueString>(TEXT("48")),         FFrameRate(48, 1) },
        { TEXT("60"),             MakeShared<FJsonValueNumber>(60.0),               FFrameRate(60, 1) },
    };

    for (const FGoodCase& Good : GoodValues)
    {
        const TestSequencerSetDisplayRateStrictHelpers::FRateCall Call = TestSequencerSetDisplayRateStrictHelpers::SetRate(Dispatcher, Sink, FullPath, Good.Value);
        TestTrue(FString::Printf(TEXT("frameRate=%s accepted through the dispatcher (code=%s)"),
            Good.Label, *Call.ErrorCode), Call.bSuccess);
        const FFrameRate Stored = MovieScene->GetDisplayRate();
        TestTrue(FString::Printf(TEXT("frameRate=%s stored %d/%d (got %d/%d)"), Good.Label,
            Good.Expected.Numerator, Good.Expected.Denominator, Stored.Numerator, Stored.Denominator),
            Stored == Good.Expected);
        FString Echo;
        TestTrue(FString::Printf(TEXT("frameRate=%s echoes the stored rate"), Good.Label),
            Call.Result.IsValid() && Call.Result->TryGetStringField(TEXT("displayRate"), Echo)
                && Echo == Stored.ToPrettyText().ToString());
    }
    return true;
}
