// Copyright (c) 2026 Alexander Penkin. MIT License.

// PIE capture: measured exposure and aim on editor.screenshot's game branch, and the editor-world
// warning on render.capture_open_level.
//   - B-editor-screenshot-pie-auto-exposure-returns-no-ev100: {mode:"auto"} on the game branch drew
//     nothing through an observer, so it answered viewCount:0 with no ev100Equivalent.
//   - B-ev100-not-comparable-between-open-level-and-pie-screenshot: the game branch echoed the
//     requested ev100 instead of reading what the drawn view carried.
//   - F-one-verb-for-exposed-and-aimed-pie-capture: editor.screenshot could expose but not aim.
//   - B-capture-open-level-during-pie-silently-captures-editor-world: no warning when PIE is live
//     and the capture is of the editor world.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Handlers/Render/OpenLevelCapture.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SceneViewExtension.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/PieState.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Layout/SBox.h"

namespace PieCaptureExposureAimTest
{
    struct FState
    {
        double Deadline = 0.0;
        double CleanupDeadline = 0.0;
        uint64 FirstFrame = 0;
    };

    // Runs editor.screenshot and returns the completed ticket's result, or null (with the error
    // code in OutError) when the capture did not complete.
    TSharedPtr<FJsonObject> Screenshot(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
        FString& OutError)
    {
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("editor.screenshot handler is registered"),
            InvokeHandlerWithCapture(TEXT("editor.screenshot"), Payload, Capture));
        FString TicketId;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId);
        }
        FJobTicket Ticket;
        if (TicketId.IsEmpty() || !FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket))
        {
            OutError = Capture.ErrorCode.IsEmpty() ? FString(TEXT("NO_TICKET")) : Capture.ErrorCode;
            return nullptr;
        }
        if (Ticket.Status != TEXT("completed") || !Ticket.Result.IsValid())
        {
            OutError = Ticket.Error.IsEmpty() ? Capture.ErrorCode : Ticket.Error;
            return nullptr;
        }
        FString Path;
        if (Ticket.Result->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
        return Ticket.Result;
    }

    TSharedPtr<FJsonObject> GetObject(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Out = nullptr;
        return Json.IsValid() && Json->TryGetObjectField(Field, Out) && Out ? *Out : nullptr;
    }

    TSharedPtr<FJsonObject> MakeVector(const FVector& V, bool bRotator)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(bRotator ? TEXT("pitch") : TEXT("x"), V.X);
        Json->SetNumberField(bRotator ? TEXT("yaw") : TEXT("y"), V.Y);
        Json->SetNumberField(bRotator ? TEXT("roll") : TEXT("z"), V.Z);
        return Json;
    }

    int32 ActiveExtensionCount(UGameViewportClient* GameViewport)
    {
        return GEngine->ViewExtensions->GatherActiveExtensions(
            FSceneViewExtensionContext(GameViewport->Viewport)).Num();
    }

    FString NewFilename(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s.png"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunPieCaptureExposureAim,
    FAutomationTestBase*, Test, TSharedRef<PieCaptureExposureAimTest::FState>, State);

bool FRunPieCaptureExposureAim::Update()
{
    using namespace PieCaptureExposureAimTest;
    UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
    ULocalPlayer* LocalPlayer = PlayWorld ? GEngine->GetFirstGamePlayer(PlayWorld) : nullptr;
    APlayerController* PC = LocalPlayer ? LocalPlayer->PlayerController.Get() : nullptr;
    UGameViewportClient* GameViewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
    if (!PC || !GameViewport || !GameViewport->Viewport)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_unavailable"),
            TEXT("The owned PIE session did not create a local player with a game viewport."));
        return true;
    }
    // Let the game viewport draw enough frames for the view state's eye-adaptation readback
    // (a few frames of GPU->CPU latency) to land.
    if (State->FirstFrame == 0)
    {
        State->FirstFrame = GFrameCounter;
    }
    if (GFrameCounter < State->FirstFrame + 30 && FPlatformTime::Seconds() < State->Deadline)
    {
        return false;
    }

    // A known non-black corner so an empty test map cannot read back as BLANK_CAPTURE. Slate only:
    // nothing outered to the PIE game instance outlives this command.
    TSharedRef<SWidget> Marker = SNew(SBox)
        .HAlign(HAlign_Left).VAlign(VAlign_Top)
        [
            SNew(SBox).WidthOverride(32.0f).HeightOverride(32.0f)
            [
                SNew(SColorBlock).Color(FLinearColor::Green)
            ]
        ];
    GameViewport->AddViewportWidgetContent(Marker, 99999);
    ON_SCOPE_EXIT
    {
        GameViewport->RemoveViewportWidgetContent(Marker);
    };

    // ---- auto: the game branch reports the exposure the view resolved ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filename"), NewFilename(TEXT("pie_auto")));
        // Fixed size: the composite carries the Slate marker even where the native back-buffer
        // grab is unavailable and the scene-only fallback would read an empty map as blank.
        Payload->SetNumberField(TEXT("width"), 320);
        Payload->SetNumberField(TEXT("height"), 180);
        TSharedPtr<FJsonObject> ExposureArg = MakeShared<FJsonObject>();
        ExposureArg->SetStringField(TEXT("mode"), TEXT("auto"));
        Payload->SetObjectField(TEXT("exposure"), ExposureArg);
        FString Error;
        const TSharedPtr<FJsonObject> Result = Screenshot(*Test, Payload, Error);
        Test->TestTrue(FString::Printf(TEXT("auto capture completes (%s)"), *Error), Result.IsValid());
        const TSharedPtr<FJsonObject> Exposure = GetObject(Result, TEXT("exposure"));
        if (Exposure.IsValid())
        {
            Test->TestEqual(TEXT("auto capture used the game viewport"),
                Result->GetStringField(TEXT("captureSource")), FString(TEXT("gameViewport")));
            Test->TestTrue(TEXT("auto capture observed at least one drawn view (was viewCount:0)"),
                Exposure->GetNumberField(TEXT("viewCount")) > 0);
            Test->TestFalse(TEXT("auto capture is not pinned"), Exposure->GetBoolField(TEXT("pinned")));
            bool bMeasured = false;
            Test->TestTrue(TEXT("auto capture says whether the exposure was measured"),
                Exposure->TryGetBoolField(TEXT("adaptedMeasured"), bMeasured));
            if (bMeasured)
            {
                double Ev100Equivalent = 0.0;
                Test->TestTrue(TEXT("measured auto capture carries ev100Equivalent"),
                    Exposure->TryGetNumberField(TEXT("ev100Equivalent"), Ev100Equivalent));
                Test->TestEqual(TEXT("unpinned exposure comes from the view-state readback"),
                    Exposure->GetStringField(TEXT("adaptedSource")), FString(TEXT("readback")));
                Test->TestTrue(TEXT("adapted is a positive gain"),
                    Exposure->GetNumberField(TEXT("adapted")) > 0.0);
            }
            else
            {
                Test->TestTrue(TEXT("an unmeasured exposure names why"),
                    Exposure->HasTypedField<EJson::String>(TEXT("adaptedReadbackPending")));
                PinWrightTestSkip::SkipAssertions(*Test, TEXT("eye_adaptation_readback_pending"),
                    TEXT("The PIE view state had no completed eye-adaptation readback; the measured-value half was not exercised."));
            }
        }
    }

    // ---- fixed + aim: the pin and the pose are read off the drawn view ----
    {
        FVector CameraBefore;
        FRotator CameraRotBefore;
        PC->GetPlayerViewPoint(CameraBefore, CameraRotBefore);

        const int32 ExtensionsBefore = ActiveExtensionCount(GameViewport);
        const FVector AimLocation = CameraBefore + FVector(137.0, -211.0, 523.0);
        const FRotator AimRotation(-38.0, CameraRotBefore.Yaw + 71.0, 0.0);
        constexpr double Ev100 = -1.5;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("filename"), NewFilename(TEXT("pie_aim")));
        Payload->SetNumberField(TEXT("width"), 320);
        Payload->SetNumberField(TEXT("height"), 180);
        Payload->SetNumberField(TEXT("exposure"), Ev100);
        Payload->SetObjectField(TEXT("location"), MakeVector(AimLocation, false));
        Payload->SetObjectField(TEXT("rotation"),
            MakeVector(FVector(AimRotation.Pitch, AimRotation.Yaw, AimRotation.Roll), true));
        FString Error;
        const TSharedPtr<FJsonObject> Result = Screenshot(*Test, Payload, Error);
        Test->TestTrue(FString::Printf(TEXT("aimed fixed capture completes (%s)"), *Error), Result.IsValid());
        const TSharedPtr<FJsonObject> Exposure = GetObject(Result, TEXT("exposure"));
        const TSharedPtr<FJsonObject> Aim = GetObject(Result, TEXT("aim"));
        Test->TestTrue(TEXT("aimed capture reports an aim block"), Aim.IsValid());
        if (Exposure.IsValid())
        {
            Test->TestTrue(TEXT("the pin governed the view"), Exposure->GetBoolField(TEXT("pinned")));
            Test->TestTrue(TEXT("the family carried a fixed exposure"), Exposure->GetBoolField(TEXT("fixed")));
            Test->TestEqual(TEXT("ev100 is the family's fixed EV100"),
                Exposure->GetNumberField(TEXT("ev100")), Ev100, 1e-4);
            Test->TestEqual(TEXT("ev100Requested echoes the request separately"),
                Exposure->GetNumberField(TEXT("ev100Requested")), Ev100, 1e-4);
            Test->TestEqual(TEXT("pinned exposure source is the fixed pin"),
                Exposure->GetStringField(TEXT("adaptedSource")), FString(TEXT("fixedPin")));
            Test->TestEqual(TEXT("ev100Equivalent round-trips the pin"),
                Exposure->GetNumberField(TEXT("ev100Equivalent")), Ev100, 1e-3);
        }
        if (Aim.IsValid())
        {
            Test->TestTrue(TEXT("aim applied"), Aim->GetBoolField(TEXT("applied")));
            const TSharedPtr<FJsonObject> Loc = GetObject(Aim, TEXT("location"));
            const TSharedPtr<FJsonObject> Rot = GetObject(Aim, TEXT("rotation"));
            if (Test->TestTrue(TEXT("aim reports the view pose"), Loc.IsValid() && Rot.IsValid()))
            {
                const FVector ViewLocation(Loc->GetNumberField(TEXT("x")), Loc->GetNumberField(TEXT("y")),
                    Loc->GetNumberField(TEXT("z")));
                const FRotator ViewRotation(Rot->GetNumberField(TEXT("pitch")), Rot->GetNumberField(TEXT("yaw")),
                    Rot->GetNumberField(TEXT("roll")));
                Test->TestTrue(TEXT("the view was drawn from the requested location"),
                    ViewLocation.Equals(AimLocation, 0.5));
                Test->TestTrue(TEXT("the view was drawn from the requested rotation"),
                    ViewRotation.Equals(AimRotation, 0.05f));
            }
        }

        // The aim lives only in the draw-scoped extension (ULocalPlayer::GetViewPoint runs every
        // active one's SetupViewPoint), so a leaked extension would put every later live frame at
        // the aimed pose. None may still be active on this viewport after the capture.
        Test->TestEqual(TEXT("the capture's view extension is released (no aim leaks into live frames)"),
            ActiveExtensionCount(GameViewport), ExtensionsBefore);
    }

    // ---- capture_open_level while PIE is live: the warning must name the editor world ----
    {
        // The handler counts PIE worlds through GatherPieContexts; it must see this session.
        Test->TestFalse(TEXT("a live PIE world plus an editor-world capture warns"),
            PinWrightOpenLevelCapture::MakePieWorldWarning(false,
                PieWorldSelector::GatherPieContexts().Num()).IsEmpty());
    }
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupPieCaptureExposureAim,
    FAutomationTestBase*, Test, TSharedRef<PieCaptureExposureAimTest::FState>, State);

bool FCleanupPieCaptureExposureAim::Update()
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (State->CleanupDeadline == 0.0)
    {
        State->CleanupDeadline = FPlatformTime::Seconds() + 15.0;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorScreenshotPieExposureAimTest,
    "PinWright.editor.screenshot.PieExposureAndAim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorScreenshotPieExposureAimTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }
    if (!GEditor || !FSlateApplication::IsInitialized() || !GEditor->GetActiveViewport())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("owned_pie_unavailable"),
            TEXT("Owned PIE requires GEditor, Slate and an active level viewport."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The test never borrows or stops ambient PIE."));
        return true;
    }

    const TSharedRef<PieCaptureExposureAimTest::FState> State = MakeShared<PieCaptureExposureAimTest::FState>();
    State->Deadline = FPlatformTime::Seconds() + 20.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FRunPieCaptureExposureAim(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupPieCaptureExposureAim(this, State));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelPieWorldWarningTest,
    "PinWright.render.capture_open_level.PieWorldWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelPieWorldWarningTest::RunTest(const FString& Parameters)
{
    using PinWrightOpenLevelCapture::MakePieWorldWarning;
    TestTrue(TEXT("no PIE running: no warning"), MakePieWorldWarning(false, 0).IsEmpty());
    TestTrue(TEXT("the capture was of the PIE world: no warning"), MakePieWorldWarning(true, 1).IsEmpty());
    const FString Warning = MakePieWorldWarning(false, 1);
    TestTrue(TEXT("PIE live, editor world captured: the warning says which world"),
        Warning.Contains(TEXT("EDITOR world")));
    TestTrue(TEXT("the warning names both remedies"),
        Warning.Contains(TEXT("Eject")) && Warning.Contains(TEXT("editor.screenshot")));

    // The handler emits it on every success, counted from the live PIE contexts. Source text,
    // because driving the level viewport while PIE owns it is not a safe fixture.
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    FString Source;
    if (!TestTrue(TEXT("RenderHandler.cpp loads"), Plugin.IsValid() && FFileHelper::LoadFileToString(Source,
            *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/PinWright/Private/Handlers/Render/RenderHandler.cpp")))))
    {
        return false;
    }
    Source.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Source = NeutralizeSourceText(Source);
    TestTrue(TEXT("capture_open_level success sets pieWorldWarning from the live PIE contexts"),
        Source.Contains(TEXT("MakePieWorldWarning(\n        WorldMatch.bViewportIsPieWorld, PieWorldSelector::GatherPieContexts().Num())"))
        && Source.Contains(TEXT("Result->SetStringField(TEXT(\"pieWorldWarning\"), PieWorldWarning)")));
    return true;
}
