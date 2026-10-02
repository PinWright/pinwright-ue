// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS
#include "CoreMinimal.h"

class UMovieSceneControlRigParameterSection;

namespace PinWrightControlRigKeyTestHooks
{
    // Runs after sequencer.set_control_keys / sequencer.pin_controls wrote every key and before
    // they read the keys back, so a test can corrupt a written key and prove the readback
    // mismatch rolls the whole call back. Unset outside that test.
    inline TFunction<void(UMovieSceneControlRigParameterSection*)>& PostWriteHook()
    {
        static TFunction<void(UMovieSceneControlRigParameterSection*)> Hook;
        return Hook;
    }
}
#endif
