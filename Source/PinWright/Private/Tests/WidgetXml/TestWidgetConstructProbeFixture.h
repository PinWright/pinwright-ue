// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Editor-only reflected test fixture: a native UUserWidget parent that COUNTS its runtime
// lifecycle hooks instead of doing what a real project widget does there (reach a
// game-instance subsystem, which asserts in the editor). A Widget Blueprint parented to it is
// the synthetic stand-in for any C++-backed UMG widget: if a verb that only measures or
// captures the widget makes NativeConstruct run, the counter says so without killing the suite.
// Used by TestWidgetGeometryResolver.cpp (B-geometry-offscreen-runs-native-construct).

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "TestWidgetConstructProbeFixture.generated.h"

UCLASS()
class UTestWidgetConstructProbe : public UUserWidget
{
    GENERATED_BODY()

public:
    static inline int32 NativeOnInitializedCalls = 0;
    static inline int32 NativeConstructCalls = 0;

    static void ResetCounters()
    {
        NativeOnInitializedCalls = 0;
        NativeConstructCalls = 0;
    }

protected:
    virtual void NativeOnInitialized() override
    {
        Super::NativeOnInitialized();
        ++NativeOnInitializedCalls;
    }

    virtual void NativeConstruct() override
    {
        Super::NativeConstruct();
        ++NativeConstructCalls;
    }
};
