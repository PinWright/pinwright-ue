// Copyright (c) 2026 Alexander Penkin. MIT License.

// The level lighting count (board B-unlit-level-capture-no-warning): render.capture_open_level and
// level.get_info both say how many light components can light the level, and a level with none
// carries `lightingWarning` naming it. The survey is asserted as a DELTA over whatever the suite's
// world already holds, so the test does not depend on the blank world being unlit.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Utils/LightingSurvey.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"

// ---- the block and its warning, as pure output of a count ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSurveyWarningTest,
    "PinWright.render.lighting_survey.WarnsOnlyWhenTheLevelHasNoLights",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSurveyWarningTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightLightingSurvey;

    TSharedPtr<FJsonObject> Unlit = MakeShared<FJsonObject>();
    AddLightingFields(FLightingSurvey(), TEXT("/Game/Maps/PW_Unlit"), Unlit);
    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (TestTrue(TEXT("an unlit level still publishes the lighting block"),
            Unlit->TryGetObjectField(TEXT("lighting"), Block)) && Block)
    {
        TestEqual(TEXT("with zero light components"),
            static_cast<int32>((*Block)->GetNumberField(TEXT("lightComponents"))), 0);
    }
    FString Warning;
    TestTrue(TEXT("and a lightingWarning"), Unlit->TryGetStringField(TEXT("lightingWarning"), Warning));
    TestTrue(TEXT("that names the level"), Warning.Contains(TEXT("/Game/Maps/PW_Unlit")));

    FLightingSurvey Lit;
    Lit.SkyLights = 1;
    TSharedPtr<FJsonObject> LitResult = MakeShared<FJsonObject>();
    AddLightingFields(Lit, TEXT("/Game/Maps/PW_Lit"), LitResult);
    TestFalse(TEXT("one light is enough to silence the warning"), LitResult->HasField(TEXT("lightingWarning")));

    // level.get_info's shape: ONE level with no lights of its own, in a world a sublevel lights.
    // The block keeps the level's own count; the warning follows the world.
    TSharedPtr<FJsonObject> SublevelLit = MakeShared<FJsonObject>();
    AddLightingFields(FLightingSurvey(), TEXT("/Game/Maps/PW_Persistent"), SublevelLit, &Lit);
    TestFalse(TEXT("a level lit from a visible sublevel carries no lightingWarning"),
        SublevelLit->HasField(TEXT("lightingWarning")));
    const TSharedPtr<FJsonObject>* SublevelBlock = nullptr;
    if (TestTrue(TEXT("the lighting block is still published"),
            SublevelLit->TryGetObjectField(TEXT("lighting"), SublevelBlock)) && SublevelBlock)
    {
        TestEqual(TEXT("lightComponents stays this level's own count"),
            static_cast<int32>((*SublevelBlock)->GetNumberField(TEXT("lightComponents"))), 0);
        TestEqual(TEXT("worldLightComponents carries the world's"),
            static_cast<int32>((*SublevelBlock)->GetNumberField(TEXT("worldLightComponents"))), 1);
    }
    const FLightingSurvey Dark;
    TSharedPtr<FJsonObject> WorldUnlit = MakeShared<FJsonObject>();
    AddLightingFields(Lit, TEXT("/Game/Maps/PW_Hidden"), WorldUnlit, &Dark);
    TestTrue(TEXT("a world with no visible light warns even if the level itself has one"),
        WorldUnlit->HasField(TEXT("lightingWarning")));
    return true;
}

// ---- the count itself, against real actors, through level.get_info ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSurveyCountsRealLightsTest,
    "PinWright.render.lighting_survey.CountsLightsThatAffectTheWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSurveyCountsRealLightsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightLightingSurvey;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"), TEXT("Skipped: no editor world."));
        return true;
    }

    FScopedEditorWorldActorGuard ActorGuard;
    const FLightingSurvey Before = SurveyWorld(World);

    ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>();
    APointLight* Lamp = World->SpawnActor<APointLight>();
    APointLight* Disabled = World->SpawnActor<APointLight>();
    if (!TestNotNull(TEXT("spawned a directional light"), Sun) ||
        !TestNotNull(TEXT("spawned a point light"), Lamp) ||
        !TestNotNull(TEXT("spawned a second point light"), Disabled) || !Sun || !Lamp || !Disabled)
    {
        return false;
    }
    // A light with bAffectsWorld off contributes nothing to the scene (the engine's own definition),
    // so it must not count -- the failure direction of "counts every light component".
    Disabled->PointLightComponent->bAffectsWorld = false;

    const FLightingSurvey After = SurveyWorld(World);
    TestEqual(TEXT("the directional light is counted as directional"),
        After.DirectionalLights - Before.DirectionalLights, 1);
    TestEqual(TEXT("one point light counted; the disabled one is not"),
        After.LocalLights - Before.LocalLights, 1);
    TestEqual(TEXT("no sky light appeared"), After.SkyLights - Before.SkyLights, 0);

    // level.get_info reports the same count for the level the actors were spawned into.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    TestTrue(TEXT("level.get_info handler found"),
        InvokeHandlerWithCapture(TEXT("level.get_info"), Payload, Capture));
    if (TestTrue(TEXT("level.get_info succeeded"), Capture.bSuccess && Capture.Result.IsValid()))
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (TestTrue(TEXT("level.get_info publishes the lighting block"),
                Capture.Result->TryGetObjectField(TEXT("lighting"), Block)) && Block)
        {
            const FLightingSurvey Level = SurveyLevel(World->PersistentLevel);
            TestEqual(TEXT("level.get_info counts what the survey counts"),
                static_cast<int32>((*Block)->GetNumberField(TEXT("lightComponents"))), Level.Total());
            TestTrue(TEXT("and it includes the spawned lights"), Level.Total() >= 2);
            // The warning is decided from every visible level (a persistent level lit from a
            // sublevel is lit), so get_info must survey the world, not only the target level.
            double WorldCount = -1.0;
            TestTrue(TEXT("level.get_info publishes worldLightComponents"),
                (*Block)->TryGetNumberField(TEXT("worldLightComponents"), WorldCount));
            TestEqual(TEXT("worldLightComponents is the world survey"),
                static_cast<int32>(WorldCount), SurveyWorld(World).Total());
        }
        TestFalse(TEXT("a lit level carries no lightingWarning"),
            Capture.Result->HasField(TEXT("lightingWarning")));
    }
    return true;
}

// ---- live: the capture response carries the block (RHI and viewport guarded) ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightingSurveyOnOpenLevelCaptureTest,
    "PinWright.render.lighting_survey.OpenLevelCaptureReportsLighting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLightingSurveyOnOpenLevelCaptureTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this)) { return true; }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    // No allowBlank: a BLANK_CAPTURE refusal is accepted as evidence below, and asserting its
    // details is the only coverage the refusal path's lighting block gets.
    FTestResponseCapture Capture;
    TestTrue(TEXT("render.capture_open_level handler found"),
        InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture));
    const bool bBlankRefusal = !Capture.bSuccess && Capture.ErrorCode == TEXT("BLANK_CAPTURE");
    if (!Capture.bSuccess && !bBlankRefusal)
    {
        // Only a typed "no viewport here" exit is a skip; any other failure is a defect.
        const bool bViewportUnavailable =
            Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            Capture.ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            Capture.ErrorCode == TEXT("VIEWPORT_WORLD_MISMATCH") ||
            Capture.ErrorCode == TEXT("CAPTURE_NOT_READY") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED");
        if (!bViewportUnavailable)
        {
            AddError(FString::Printf(TEXT("render.capture_open_level failed untyped: %s (%s)"),
                *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level-viewport-capture-unavailable"),
            FString::Printf(TEXT("Skipped: open-level capture unavailable here (%s). "
                "CountsLightsThatAffectTheWorld covers the count without a viewport."), *Capture.ErrorCode));
        return true;
    }
    if (!TestTrue(TEXT("the response (or the blank refusal's details) is present"), Capture.Result.IsValid()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Block = nullptr;
    TestTrue(bBlankRefusal ? TEXT("the BLANK_CAPTURE details carry the lighting block")
                           : TEXT("the capture response carries the lighting block"),
        Capture.Result->TryGetObjectField(TEXT("lighting"), Block));
    if (Block)
    {
        const bool bUnlit = static_cast<int32>((*Block)->GetNumberField(TEXT("lightComponents"))) == 0;
        TestEqual(TEXT("lightingWarning is present exactly when the level has no lights"),
            Capture.Result->HasField(TEXT("lightingWarning")), bUnlit);
    }
    FString Path;
    if (Capture.Result->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
    {
        IFileManager::Get().Delete(*Path, false, true);
    }
    return true;
}
