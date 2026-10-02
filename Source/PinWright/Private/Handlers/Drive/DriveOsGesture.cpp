// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveOsGesture.h"

#include "Handlers/Drive/DriveOsInput.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace DriveOsGestureLocal
{
    using EKind = FDriveOsGestureStep::EKind;

    // Append the interpolated motion from InOutCur to To, skipping points the pointer is already
    // on. The distinct points are spread evenly over DurationMs, the first exactly at StartMs, so a
    // short (sub-24 px) segment still starts moving when the hold ends instead of idling through
    // the interpolation steps that rounded onto the starting pixel.
    void AddSegment(TArray<FDriveOsGestureStep>& Plan, FIntPoint& InOutCur, const FIntPoint& To,
        double StartMs, double DurationMs)
    {
        TArray<FIntPoint> Distinct;
        for (const FIntPoint& Point : FDriveOsInput::ComputeMotionPath(InOutCur, To))
        {
            if (Point != (Distinct.Num() > 0 ? Distinct.Last() : InOutCur))
            {
                Distinct.Add(Point);
            }
        }
        for (int32 Index = 0; Index < Distinct.Num(); ++Index)
        {
            Plan.Add({ EKind::Move, Distinct[Index], StartMs + Index * DurationMs / Distinct.Num() });
        }
        if (Distinct.Num() > 0)
        {
            InOutCur = Distinct.Last();
        }
    }

    // The XTEST side of one step. Only the press can refuse; the gesture's lock is already held.
    bool ExecuteX(const FDriveOsGestureStep& Step, EDriveMouseButton Button, FDriveInjectFailure& OutFailure,
        FDriveOsGesture::FResult& InOutResult)
    {
        switch (Step.Kind)
        {
        case EKind::Move:
            FDriveOsInput::SendMotion(Step.Point);
            return true;
        case EKind::Press:
            if (!FDriveOsInput::CheckPress(Step.Point, OutFailure))
            {
                return false;
            }
            FDriveOsInput::SendButton(Button, /*bPress*/ true);
            InOutResult.PressFrame = GFrameCounter;
            return true;
        case EKind::Release:
            FDriveOsInput::SendButton(Button, /*bPress*/ false);
            InOutResult.ReleaseFrame = GFrameCounter;
            return true;
        case EKind::End:
        default:
            return true;
        }
    }

    FIntPoint ReleasePoint(const TArray<FDriveOsGestureStep>& Plan)
    {
        const FDriveOsGestureStep* Release = Plan.FindByPredicate(
            [](const FDriveOsGestureStep& Step) { return Step.Kind == EKind::Release; });
        return Release ? Release->Point : FIntPoint::ZeroValue;
    }

    void FinishResult(const TArray<FDriveOsGestureStep>& Plan, double StartSeconds, FDriveOsGesture::FResult& Result)
    {
        Result.ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
        Result.bPointerOnRelease = FDriveOsInput::IsPointerAt(ReleasePoint(Plan), Result.PointerAfter);
    }
}

using namespace DriveOsGestureLocal;

TArray<FDriveOsGestureStep> FDriveOsGesture::BuildPlan(const FIntPoint& Pointer, const FIntPoint& Press,
    const TArray<FIntPoint>& PressedPath, int32 HoldMs, int32 SegmentMs)
{
    TArray<FDriveOsGestureStep> Plan;
    FIntPoint Cur = Pointer;

    AddSegment(Plan, Cur, Press, StepMs, DefaultSegmentMs);
    const double PressMs = (Plan.Num() > 0 ? Plan.Last().AtMs : 0.0) + ApproachSettleMs;
    Plan.Add({ EKind::Press, Press, PressMs });

    // The pressed motion starts once the hold is over; each segment that moves takes SegmentMs.
    double T = PressMs + FMath::Max(HoldMs, 0);
    double ReleaseMs = T;
    for (const FIntPoint& Waypoint : PressedPath)
    {
        const int32 Before = Plan.Num();
        AddSegment(Plan, Cur, Waypoint, T, FMath::Max(SegmentMs, 0));
        if (Plan.Num() > Before)
        {
            ReleaseMs = Plan.Last().AtMs + StepMs;
            T += FMath::Max(SegmentMs, 0);
        }
    }

    Plan.Add({ EKind::Release, Cur, ReleaseMs });
    Plan.Add({ EKind::End, Cur, ReleaseMs + ReleaseSettleMs });
    return Plan;
}

bool FDriveOsGesture::RunDueSteps(const TArray<FDriveOsGestureStep>& Plan, double NowMs, int32& InOutNext,
    FExecuteStep Execute, FDriveInjectFailure& OutFailure)
{
    while (Plan.IsValidIndex(InOutNext) && Plan[InOutNext].AtMs <= NowMs)
    {
        if (!Execute(Plan[InOutNext], OutFailure))
        {
            return false;
        }
        ++InOutNext;
    }
    return true;
}

FVector2D FDriveOsGesture::ViewportPixelToScreen(const FVector2D& Pixel, const FIntPoint& ViewportSize,
    const FVector2D& WidgetScreenPos, const FVector2D& WidgetScreenSize)
{
    return WidgetScreenPos + FVector2D(
        ViewportSize.X > 0 ? Pixel.X * WidgetScreenSize.X / ViewportSize.X : 0.0,
        ViewportSize.Y > 0 ? Pixel.Y * WidgetScreenSize.Y / ViewportSize.Y : 0.0);
}

void FDriveOsGesture::Start(const FIntPoint& Press, const TArray<FIntPoint>& PressedPath,
    EDriveMouseButton Button, int32 HoldMs, int32 SegmentMs, FOnDone OnDone)
{
    FIntPoint Pointer;
    FDriveInjectFailure Failure;
    const TSharedPtr<FDriveOsInput::FDisplayLock> Lock = FDriveOsInput::BeginGesture(Pointer, Failure);
    if (!Lock.IsValid())
    {
        OnDone(false, Failure, FResult());
        return;
    }

    struct FState
    {
        TArray<FDriveOsGestureStep> Plan;
        TSharedPtr<FDriveOsInput::FDisplayLock> Lock;
        double StartSeconds = 0.0;
        int32 Next = 0;
        FResult Result;
    };
    const TSharedRef<FState> State = MakeShared<FState>();
    State->Plan = BuildPlan(Pointer, Press, PressedPath, HoldMs, SegmentMs);
    State->Lock = Lock;
    State->StartSeconds = FPlatformTime::Seconds();

    // One ticker callback per engine frame: every step due by now goes out in this frame, and the
    // engine pumps it before the next callback.
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [State, Button, OnDone = MoveTemp(OnDone)](float) -> bool
        {
            FDriveInjectFailure StepFailure;
            const double NowMs = (FPlatformTime::Seconds() - State->StartSeconds) * 1000.0;
            const bool bOk = RunDueSteps(State->Plan, NowMs, State->Next,
                [&State, Button](const FDriveOsGestureStep& Step, FDriveInjectFailure& OutFailure)
                {
                    return ExecuteX(Step, Button, OutFailure, State->Result);
                },
                StepFailure);
            if (bOk && State->Plan.IsValidIndex(State->Next))
            {
                return true;
            }
            FinishResult(State->Plan, State->StartSeconds, State->Result);
            State->Lock.Reset();
            OnDone(bOk, StepFailure, State->Result);
            return false;
        }));
}

bool FDriveOsGesture::RunBlocking(const FIntPoint& Press, const TArray<FIntPoint>& PressedPath,
    EDriveMouseButton Button, int32 HoldMs, int32 SegmentMs, FDriveInjectFailure& OutFailure)
{
    FIntPoint Pointer;
    const TSharedPtr<FDriveOsInput::FDisplayLock> Lock = FDriveOsInput::BeginGesture(Pointer, OutFailure);
    if (!Lock.IsValid())
    {
        return false;
    }

    const TArray<FDriveOsGestureStep> Plan = BuildPlan(Pointer, Press, PressedPath, HoldMs, SegmentMs);
    const double StartSeconds = FPlatformTime::Seconds();
    FResult Result;
    int32 Next = 0;
    while (Plan.IsValidIndex(Next))
    {
        const double WaitMs = Plan[Next].AtMs - (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
        if (WaitMs > 0.0)
        {
            FPlatformProcess::Sleep(static_cast<float>(WaitMs / 1000.0));
        }
        if (!RunDueSteps(Plan, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, Next,
                [&Result, Button](const FDriveOsGestureStep& Step, FDriveInjectFailure& StepFailure)
                {
                    return ExecuteX(Step, Button, StepFailure, Result);
                },
                OutFailure))
        {
            return false;
        }
    }
    return true;
}
