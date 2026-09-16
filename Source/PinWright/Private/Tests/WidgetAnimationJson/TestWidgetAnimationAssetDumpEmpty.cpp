// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Assets/AssetDumpTestHelpers.h"
#include "Tests/Utility/AssetDumpFixtureHelpers.h"
#include "UObject/Package.h"
#include "WidgetAnimationJsonTestUtils.h"
#include "WidgetBlueprint.h"

namespace
{
    using AssetDumpFixtureHelpers::LoadJsonFile;
    using AssetDumpTestHelpers::FindDumpFile;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetAnimationAspectEmptyTest,
    "PinWright.Assets.AssetDump.WidgetAnimationAspect.EmptyStub",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWidgetAnimationAspectEmptyTest::RunTest(const FString& Parameters)
{
    // Build the zero-animation fixture in-process. This test previously loaded
    // /App/App/UI/KeysOverlay/BP_OverlayKey from a host-project dump corpus and
    // AddInfo-skipped when it was absent — which is every run in this checkout
    // (no /App content mount exists), so the test asserted nothing at all.
    // A transient WidgetBlueprint with an empty Animations array is the same
    // subject and is always present. DumpSingleAsset resolves in-memory packages,
    // so no save-to-disk is needed (same approach as the sibling TrackStartTimer test).
    using namespace WidgetAnimationJsonTestUtils;

    const FString FixturePath = MakeWidgetAnimationJsonTestAssetPath(TEXT("WBP_ZeroAnimationFixture"));
    UWidgetBlueprint* Fixture = CreateTransientWidgetBlueprint(FixturePath);
    TestNotNull(TEXT("zero-animation widget blueprint created"), Fixture);
    if (Fixture == nullptr)
    {
        return false;
    }
    AddNamedTextBlock(Fixture, TEXT("PlainLabel"));

    // Precondition the whole test rests on: the subject really has no animations.
    TestEqual(TEXT("fixture is a zero-animation widget"), Fixture->Animations.Num(), 0);
    if (Fixture->Animations.Num() != 0)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("WidgetAnimationAssetDumpTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(Fixture->GetPathName(), ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for the zero-animation widget"), Result.ErrorCode.IsEmpty());
    if (!Result.ErrorCode.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    const FString WidgetAnimationsPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::WidgetAnimations);
    TestFalse(TEXT("widget_animations.json is unconditionally written for zero-animation widgets"),
        WidgetAnimationsPath.IsEmpty());
    if (WidgetAnimationsPath.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    const TSharedPtr<FJsonObject> Document = LoadJsonFile(WidgetAnimationsPath);
    TestTrue(TEXT("widget_animations.json parses"), Document.IsValid());
    if (!Document.IsValid())
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
        return false;
    }

    FString Schema;
    Document->TryGetStringField(TEXT("schema"), Schema);
    TestEqual(TEXT("schema"), Schema, FString(TEXT("pinwright.widget-animations.v1")));

    const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
    const bool bHasAnimationsArray = Document->TryGetArrayField(TEXT("animations"), Animations) && Animations != nullptr;
    TestTrue(TEXT("animations field exists and is an array"), bHasAnimationsArray);
    if (bHasAnimationsArray)
    {
        TestEqual(TEXT("animations array is empty for zero-animation widget"), Animations->Num(), 0);
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}
