// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS
namespace PinWrightWidgetAnimationTestHooks
{
    inline bool& ForceTrackAttachPreflightFailure()
    {
        static bool bForceFailure = false;
        return bForceFailure;
    }

    inline bool ConsumeForceTrackAttachPreflightFailure()
    {
        bool& bForceFailure = ForceTrackAttachPreflightFailure();
        const bool bShouldForceFailure = bForceFailure;
        bForceFailure = false;
        return bShouldForceFailure;
    }

    class FScopedForceTrackAttachPreflightFailure final
    {
    public:
        FScopedForceTrackAttachPreflightFailure()
            : bPreviousValue(ForceTrackAttachPreflightFailure())
        {
            ForceTrackAttachPreflightFailure() = true;
        }

        ~FScopedForceTrackAttachPreflightFailure()
        {
            ForceTrackAttachPreflightFailure() = bPreviousValue;
        }

        FScopedForceTrackAttachPreflightFailure(
            const FScopedForceTrackAttachPreflightFailure&) = delete;
        FScopedForceTrackAttachPreflightFailure& operator=(
            const FScopedForceTrackAttachPreflightFailure&) = delete;

    private:
        bool bPreviousValue;
    };
}
#endif
