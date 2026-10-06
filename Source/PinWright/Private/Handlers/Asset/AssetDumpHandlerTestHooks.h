// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS
// Test-only overrides for the dump sweep's per-asset bounds. A negative value keeps the
// production constant from AssetDumpHandler.h.
namespace PinWrightAssetDumpTestHooks
{
    struct FOverrides
    {
        double AsyncLoadTimeoutSeconds = -1.0;
        double SlowAssetThresholdSeconds = -1.0;
        // Stands in for a load that never progresses: the sweep stops pumping the async
        // loader, and tests tick only the core ticker, so nothing else pumps it either.
        bool bSkipAsyncLoadPump = false;
        // A release step's collect is consumed at frame end, and a test body never ends a
        // frame. Instead of waiting out the collect, the sweep restores every flag the
        // release cleared (as a cancel does) and carries on.
        bool bAbortReleaseGcWait = false;
    };

    inline FOverrides& Get()
    {
        static FOverrides Overrides;
        return Overrides;
    }

    class FScopedOverrides final
    {
    public:
        explicit FScopedOverrides(const FOverrides& InOverrides)
            : Previous(Get())
        {
            Get() = InOverrides;
        }

        ~FScopedOverrides()
        {
            Get() = Previous;
        }

        FScopedOverrides(const FScopedOverrides&) = delete;
        FScopedOverrides& operator=(const FScopedOverrides&) = delete;

    private:
        FOverrides Previous;
    };
}
#endif
