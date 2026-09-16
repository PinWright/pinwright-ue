// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/TestUtils.h"

#include "Animation/AnimSequence.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationDescribeSequenceReturnsDumpShapeTest,
    "PinWright.animation.describe_sequence.ReturnsDumpShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimationDescribeSequenceReturnsDumpShapeTest::RunTest(const FString& Parameters)
{
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered animation fixture created"),
        Fixture.Create(TEXT("AS_DescribeSequence")));
    UAnimSequence* Sequence = Fixture.Sequence;
    TestNotNull(TEXT("registered UAnimSequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Fixture.SequenceObjectPath);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("animation.describe_sequence"), Payload, Capture);

    TestTrue(TEXT("Handler found in registration list"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestTrue(FString::Printf(TEXT("Handler succeeded (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess);
    TestTrue(TEXT("Result returned"), Capture.Result.IsValid());
    if (!bFound || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("assetKind is a string"),
        Capture.Result->HasTypedField<EJson::String>(TEXT("assetKind")));
    TestEqual(TEXT("assetKind equals 'AnimSequence'"),
        Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("AnimSequence")));

    TestTrue(TEXT("path is a string"),
        Capture.Result->HasTypedField<EJson::String>(TEXT("path")));
    TestTrue(TEXT("lengthSeconds is a number"),
        Capture.Result->HasTypedField<EJson::Number>(TEXT("lengthSeconds")));

    const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
    TestTrue(TEXT("notifies is an array"),
        Capture.Result->TryGetArrayField(TEXT("notifies"), Notifies));

    const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
    TestTrue(TEXT("curves is an array"),
        Capture.Result->TryGetArrayField(TEXT("curves"), Curves));

    const TArray<TSharedPtr<FJsonValue>>* SyncMarkers = nullptr;
    TestTrue(TEXT("syncMarkers is an array"),
        Capture.Result->TryGetArrayField(TEXT("syncMarkers"), SyncMarkers));

    return true;
}
