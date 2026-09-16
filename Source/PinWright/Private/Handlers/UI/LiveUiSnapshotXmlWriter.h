// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FLiveUiSnapshot;

class FLiveUiSnapshotXmlWriter
{
public:
    static FString Write(const FLiveUiSnapshot& Snapshot);
};
