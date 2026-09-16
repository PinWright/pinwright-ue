// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS
namespace PinWrightAssetManageTestHooks
{
    inline bool& ForceMissingDestinationReadback()
    {
        static bool bForceMissing = false;
        return bForceMissing;
    }

    inline bool ConsumeForceMissingDestinationReadback()
    {
        bool& bForceMissing = ForceMissingDestinationReadback();
        const bool bShouldForceMissing = bForceMissing;
        bForceMissing = false;
        return bShouldForceMissing;
    }

    class FScopedForceMissingDestinationReadback final
    {
    public:
        FScopedForceMissingDestinationReadback()
            : bPreviousValue(ForceMissingDestinationReadback())
        {
            ForceMissingDestinationReadback() = true;
        }

        ~FScopedForceMissingDestinationReadback()
        {
            ForceMissingDestinationReadback() = bPreviousValue;
        }

        FScopedForceMissingDestinationReadback(
            const FScopedForceMissingDestinationReadback&) = delete;
        FScopedForceMissingDestinationReadback& operator=(
            const FScopedForceMissingDestinationReadback&) = delete;

    private:
        bool bPreviousValue;
    };
}
#endif
