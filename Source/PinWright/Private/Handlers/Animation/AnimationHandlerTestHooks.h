// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace AnimationHandlerTestHooks
{
#if WITH_DEV_AUTOMATION_TESTS
    inline int32& CreateStateFailureIndex()
    {
        static int32 FailureIndex = INDEX_NONE;
        return FailureIndex;
    }

    inline bool ShouldFailCreateState(const int32 StateIndex)
    {
        return StateIndex == CreateStateFailureIndex();
    }

    class FScopedCreateStateFailure final
    {
    public:
        explicit FScopedCreateStateFailure(const int32 FailureIndex)
            : PreviousFailureIndex(CreateStateFailureIndex())
        {
            CreateStateFailureIndex() = FailureIndex;
        }

        ~FScopedCreateStateFailure()
        {
            CreateStateFailureIndex() = PreviousFailureIndex;
        }

        FScopedCreateStateFailure(const FScopedCreateStateFailure&) = delete;
        FScopedCreateStateFailure& operator=(const FScopedCreateStateFailure&) = delete;

    private:
        int32 PreviousFailureIndex;
    };
#endif
}
