// Copyright (c) 2026 Alexander Penkin. MIT License.

// Runtime capture tests for FLiveUiSnapshotService.

#include "Misc/AutomationTest.h"

#include "Handlers/UI/LiveUiSnapshot.h"
#include "LiveUiSnapshotTestHelpers.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    using LiveUiSnapshotTestHelpers::IsAcceptedLiveCaptureFailureCode;

    enum class ELiveCaptureTestResult
    {
        Captured,
        Skipped,
        Failed
    };

    ELiveCaptureTestResult CaptureOrSkip(
        FAutomationTestBase& Test,
        const FLiveUiSnapshotRequest& Request,
        FLiveUiSnapshot& OutSnapshot)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (FLiveUiSnapshotService::Capture(Request, OutSnapshot, ErrorCode, ErrorMessage))
        {
            return ELiveCaptureTestResult::Captured;
        }

        if (IsAcceptedLiveCaptureFailureCode(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("capture-unavailable"),
                FString::Printf(
                    TEXT("Skipping live UI snapshot capture assertions: %s - %s"),
                    *ErrorCode,
                    *ErrorMessage));
            return ELiveCaptureTestResult::Skipped;
        }

        Test.AddError(FString::Printf(
            TEXT("Unexpected live UI snapshot capture failure: %s - %s"),
            *ErrorCode,
            *ErrorMessage));
        return ELiveCaptureTestResult::Failed;
    }

    bool IsFiniteVector(const FVector2D& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotCaptureSmokeTest,
    "PinWright.widget.LiveSnapshot.Capture.Smoke",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotCaptureSmokeTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshotRequest Request;
    FLiveUiSnapshot Snapshot;

    const ELiveCaptureTestResult Result = CaptureOrSkip(*this, Request, Snapshot);
    if (Result != ELiveCaptureTestResult::Captured)
    {
        return Result == ELiveCaptureTestResult::Skipped;
    }

    TestEqual(TEXT("capture source is live"), Snapshot.CaptureSource, TEXT("live"));
    TestFalse(TEXT("verbose defaults to false"), Snapshot.bVerbose);
    TestFalse(TEXT("geometry defaults to excluded"), Snapshot.bGeometryIncluded);
    TestFalse(TEXT("captured root has a Slate type"), Snapshot.RootNode.SlateType.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotCaptureRootWrapperTest,
    "PinWright.widget.LiveSnapshot.Capture.RootWrapper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotCaptureRootWrapperTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshotRequest Request;
    FLiveUiSnapshot Snapshot;

    const ELiveCaptureTestResult Result = CaptureOrSkip(*this, Request, Snapshot);
    if (Result != ELiveCaptureTestResult::Captured)
    {
        return Result == ELiveCaptureTestResult::Skipped;
    }

    TestEqual(TEXT("root is the UMG canvas wrapper"), Snapshot.RootNode.SlateType, TEXT("SConstraintCanvas"));
    TestTrue(TEXT("root has at least one child"), Snapshot.RootNode.Children.Num() > 0);
    if (Snapshot.RootNode.Children.Num() == 0)
    {
        return false;
    }

    TestEqual(TEXT("first child is the hosted user widget"), Snapshot.RootNode.Children[0].SlateType, TEXT("SObjectWidget"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotCaptureSourceAndGeometryTest,
    "PinWright.widget.LiveSnapshot.Capture.SourceAndGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotCaptureSourceAndGeometryTest::RunTest(const FString& Parameters)
{
    FLiveUiSnapshotRequest Request;
    Request.bIncludeGeometry = true;

    FLiveUiSnapshot Snapshot;
    const ELiveCaptureTestResult Result = CaptureOrSkip(*this, Request, Snapshot);
    if (Result != ELiveCaptureTestResult::Captured)
    {
        return Result == ELiveCaptureTestResult::Skipped;
    }

    TestTrue(TEXT("snapshot records geometry inclusion"), Snapshot.bGeometryIncluded);
    TestTrue(TEXT("viewport size is finite"), IsFiniteVector(Snapshot.ViewportSize));
    TestTrue(TEXT("viewport has positive area"), Snapshot.ViewportSize.X > 0.0 && Snapshot.ViewportSize.Y > 0.0);
    TestTrue(TEXT("root absolute position is finite"), IsFiniteVector(Snapshot.RootNode.RuntimeState.AbsolutePosition));
    TestTrue(TEXT("root absolute size is finite"), IsFiniteVector(Snapshot.RootNode.RuntimeState.AbsoluteSize));
    TestTrue(TEXT("root geometry has positive area"),
        Snapshot.RootNode.RuntimeState.AbsoluteSize.X > 0.0 && Snapshot.RootNode.RuntimeState.AbsoluteSize.Y > 0.0);

    TestTrue(TEXT("root has at least one child"), Snapshot.RootNode.Children.Num() > 0);
    if (Snapshot.RootNode.Children.Num() == 0)
    {
        return false;
    }

    const FLiveUiSnapshotNode& HostedWidget = Snapshot.RootNode.Children[0];
    TestEqual(TEXT("hosted child is an SObjectWidget"), HostedWidget.SlateType, TEXT("SObjectWidget"));
    TestTrue(TEXT("hosted child has backing UWidget source"), HostedWidget.SourceInfo.bHasBackingWidget);
    TestEqual(TEXT("hosted child source kind is UWidget"), HostedWidget.SourceInfo.SourceKind, TEXT("UWidget"));
    TestFalse(TEXT("hosted child widget name is populated"), HostedWidget.SourceInfo.WidgetName.IsEmpty());
    TestFalse(TEXT("hosted child widget class path is populated"), HostedWidget.SourceInfo.WidgetClassPath.IsEmpty());
    TestFalse(TEXT("hosted child owner name is populated"), HostedWidget.SourceInfo.OwningUserWidgetName.IsEmpty());
    TestFalse(TEXT("hosted child owner class path is populated"),
        HostedWidget.SourceInfo.OwningUserWidgetClassPath.IsEmpty());
    TestTrue(TEXT("hosted child absolute position is finite"), IsFiniteVector(HostedWidget.RuntimeState.AbsolutePosition));
    TestTrue(TEXT("hosted child absolute size is finite"), IsFiniteVector(HostedWidget.RuntimeState.AbsoluteSize));
    TestTrue(TEXT("hosted child geometry has positive area"),
        HostedWidget.RuntimeState.AbsoluteSize.X > 0.0 && HostedWidget.RuntimeState.AbsoluteSize.Y > 0.0);

    return true;
}
