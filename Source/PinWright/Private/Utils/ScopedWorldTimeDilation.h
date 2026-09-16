// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// TWeakObjectPtr's storage member needs the FWeakObjectPtr definition, which CoreMinimal.h does
// not pull in; without this the member below only compiles inside a unity blob.
#include "UObject/WeakObjectPtr.h"

class AWorldSettings;
class UWorld;

class FScopedWorldTimeDilation
{
public:
    explicit FScopedWorldTimeDilation(UWorld* World);
    ~FScopedWorldTimeDilation();

    FScopedWorldTimeDilation(const FScopedWorldTimeDilation&) = delete;
    FScopedWorldTimeDilation& operator=(const FScopedWorldTimeDilation&) = delete;

    bool Freeze();
    void Restore();

    bool IsAvailable() const { return WorldSettings.IsValid(); }
    bool IsFrozen() const { return bFrozen; }
    bool WasRestored() const { return bRestored; }
    float GetOriginalTimeDilation() const { return OriginalTimeDilation; }
    float GetFrozenTimeDilation() const { return FrozenTimeDilation; }

private:
    TWeakObjectPtr<AWorldSettings> WorldSettings;
    float OriginalTimeDilation = 1.0f;
    float FrozenTimeDilation = 1.0f;
    bool bFrozen = false;
    bool bRestored = false;
};
