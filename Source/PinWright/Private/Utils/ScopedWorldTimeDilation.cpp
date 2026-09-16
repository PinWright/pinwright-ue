// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/ScopedWorldTimeDilation.h"

#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

FScopedWorldTimeDilation::FScopedWorldTimeDilation(UWorld* World)
{
    AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
    WorldSettings = Settings;
    if (Settings)
    {
        OriginalTimeDilation = Settings->TimeDilation;
        FrozenTimeDilation = OriginalTimeDilation;
    }
}

FScopedWorldTimeDilation::~FScopedWorldTimeDilation()
{
    Restore();
}

bool FScopedWorldTimeDilation::Freeze()
{
    AWorldSettings* Settings = WorldSettings.Get();
    if (!Settings)
    {
        return false;
    }

    Settings->SetTimeDilation(0.0f);
    FrozenTimeDilation = Settings->TimeDilation;
    bFrozen = true;
    bRestored = false;
    return true;
}

void FScopedWorldTimeDilation::Restore()
{
    if (!bFrozen)
    {
        return;
    }

    if (AWorldSettings* Settings = WorldSettings.Get())
    {
        Settings->SetTimeDilation(OriginalTimeDilation);
        bRestored = FMath::IsNearlyEqual(
            Settings->TimeDilation, OriginalTimeDilation, UE_KINDA_SMALL_NUMBER);
    }
    bFrozen = false;
}
