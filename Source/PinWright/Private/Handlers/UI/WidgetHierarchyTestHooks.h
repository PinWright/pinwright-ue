// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS
namespace PinWrightWidgetHierarchyTestHooks
{
    inline bool& ForcePostRenameReadbackFailure()
    {
        static bool bForceFailure = false;
        return bForceFailure;
    }

    inline bool ConsumeForcePostRenameReadbackFailure()
    {
        bool& bForceFailure = ForcePostRenameReadbackFailure();
        const bool bShouldForceFailure = bForceFailure;
        bForceFailure = false;
        return bShouldForceFailure;
    }

    class FScopedForcePostRenameReadbackFailure final
    {
    public:
        FScopedForcePostRenameReadbackFailure()
            : bPreviousValue(ForcePostRenameReadbackFailure())
        {
            ForcePostRenameReadbackFailure() = true;
        }

        ~FScopedForcePostRenameReadbackFailure()
        {
            ForcePostRenameReadbackFailure() = bPreviousValue;
        }

        FScopedForcePostRenameReadbackFailure(
            const FScopedForcePostRenameReadbackFailure&) = delete;
        FScopedForcePostRenameReadbackFailure& operator=(
            const FScopedForcePostRenameReadbackFailure&) = delete;

    private:
        bool bPreviousValue;
    };
}
#endif
