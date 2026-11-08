// Copyright (c) 2026 Alexander Penkin. MIT License.

// The one instance of the automation-mode interval state (Dispatch/ScopedUnattendedRpc.h).

#include "Dispatch/ScopedUnattendedRpc.h"

namespace PinWrightAutomationMode::Detail
{
    int32& Depth()
    {
        static int32 GDepth = 0;
        return GDepth;
    }

    bool& Engaged()
    {
        static bool bGEngaged = false;
        return bGEngaged;
    }

    bool& SavedFlag()
    {
        static bool bGSaved = false;
        return bGSaved;
    }
}
