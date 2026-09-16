// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FLiveUiSnapshot;

class FLiveUiSnapshotJsonWriter
{
public:
    static TSharedPtr<FJsonObject> Write(const FLiveUiSnapshot& Snapshot);
};
