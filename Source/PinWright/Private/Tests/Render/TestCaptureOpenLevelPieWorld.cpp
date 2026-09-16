// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/OpenLevelCapture.h"
#include "Tests/TestWorldUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpenLevelAllowPieWorldGuardTest,
    "PinWright.render.capture_open_level.AllowPieWorldGuardContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpenLevelAllowPieWorldGuardTest::RunTest(const FString& Parameters)
{
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TestNotNull(TEXT("an editor world is available"), EditorWorld);
    if (!EditorWorld || !EditorWorld->GetOutermost())
    {
        return false;
    }

    const FString EditorMapName = EditorWorld->GetOutermost()->GetName();
    const FString MapPath = FPackageName::GetLongPackagePath(EditorMapName);
    const FString MapShortName = FPackageName::GetShortName(EditorMapName);
    const uint32 FixtureId = FPlatformTime::Cycles();
    const auto CreatePieWorld = [&MapPath, FixtureId](const FString& ShortName)
    {
        const FString PackageName = FString::Printf(TEXT("%s/UEDPIE_%u_%s"),
            *MapPath, FixtureId, *ShortName);
        UPackage* Package = CreatePackage(*PackageName);
        return UWorld::CreateWorld(EWorldType::PIE, /*bInformEngineOfWorld=*/false,
            FName(*FPackageName::GetShortName(PackageName)), Package);
    };

    const PinWrightOpenLevelCapture::FWorldMatchDecision DirectMatch =
        PinWrightOpenLevelCapture::EvaluateViewportWorldMatch(
            EditorWorld, EditorWorld, /*bAllowPieWorld=*/false);
    TestTrue(TEXT("the editor viewport world is accepted without an opt-in"),
        DirectMatch.bAllowed);

    UWorld* SameMapPieWorld = CreatePieWorld(MapShortName);
    FScopedTransientWorldGuard SameMapGuard(SameMapPieWorld);
    TestNotNull(TEXT("same-map PIE fixture was created"), SameMapPieWorld);
    if (!SameMapPieWorld)
    {
        return false;
    }

    const PinWrightOpenLevelCapture::FWorldMatchDecision WithoutOptIn =
        PinWrightOpenLevelCapture::EvaluateViewportWorldMatch(
            EditorWorld, SameMapPieWorld, /*bAllowPieWorld=*/false);
    TestFalse(TEXT("same-map PIE is refused by default"), WithoutOptIn.bAllowed);
    TestEqual(TEXT("default refusal uses the viewport-world error"), WithoutOptIn.ErrorCode,
        FString(ErrorCodes::ERR_VIEWPORT_WORLD_MISMATCH));
    TestEqual(TEXT("default refusal explains the missing opt-in"), WithoutOptIn.Reason,
        FString(TEXT("allowPieWorldRequired")));

    const PinWrightOpenLevelCapture::FWorldMatchDecision WithOptIn =
        PinWrightOpenLevelCapture::EvaluateViewportWorldMatch(
            EditorWorld, SameMapPieWorld, /*bAllowPieWorld=*/true);
    TestTrue(TEXT("same-map PIE is accepted with the opt-in"), WithOptIn.bAllowed);
    TestTrue(TEXT("the accepted PIE map matches after prefix removal"), WithOptIn.bMapsMatch);

    UWorld* OtherMapPieWorld = CreatePieWorld(
        FString::Printf(TEXT("PinWrightOtherMap_%u"), FixtureId));
    FScopedTransientWorldGuard OtherMapGuard(OtherMapPieWorld);
    TestNotNull(TEXT("other-map PIE fixture was created"), OtherMapPieWorld);
    if (!OtherMapPieWorld)
    {
        return false;
    }

    const PinWrightOpenLevelCapture::FWorldMatchDecision OtherMap =
        PinWrightOpenLevelCapture::EvaluateViewportWorldMatch(
            EditorWorld, OtherMapPieWorld, /*bAllowPieWorld=*/true);
    TestFalse(TEXT("another-map PIE world remains refused"), OtherMap.bAllowed);
    TestEqual(TEXT("another-map refusal uses the viewport-world error"), OtherMap.ErrorCode,
        FString(ErrorCodes::ERR_VIEWPORT_WORLD_MISMATCH));
    TestEqual(TEXT("another-map refusal is machine-readable"), OtherMap.Reason,
        FString(TEXT("pieMapMismatch")));
    return true;
}
