// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "Handlers/Drive/DriveOsInput.h"

#if PLATFORM_WINDOWS

// The Win32 backend of FDriveOsInput. DriveOsInput.cpp delegates its injection entry points
// here on Windows, so callers keep the one FDriveOsInput API. Same contract as the X11 path:
// SendInput (absolute, virtual-desktop-normalized moves, then button down/up) instead of XTEST,
// a session-wide named mutex instead of the per-display flock, GetCursorPos for the pre-press
// pointer check, and WindowFromPoint + GetWindowThreadProcessId for the foreign-window gate.
namespace DriveOsInputWindows
{
    bool IsAvailable(FString& OutError);
    bool FindForeignWindowAt(const FVector2D& ScreenPos, FDriveOsInput::FForeignWindow& Out);

    // Name of the session-wide named mutex standing in for the X11 per-display lock file.
    FString LockName();

    // Owned mutex handle, or nullptr with bOutTimedOut / OutError filled.
    void* AcquireLock(const FString& Name, double TimeoutSeconds, bool& bOutTimedOut, FString& OutError);
    void ReleaseLock(void* Mutex);

    bool IsPointerAt(const FIntPoint& Target, FIntPoint& OutPointer);
    bool MoveTo(const FVector2D& ScreenPos, FDriveInjectFailure& OutFailure);
    bool ClickAt(const FVector2D& ScreenPos, EDriveMouseButton Button, FDriveInjectFailure& OutFailure);

    // FDriveOsGesture primitives. FDriveOsInput::BeginGesture owns the lock object; this
    // refuses (OS_INPUT_BUSY / INPUT_FAILED) unless it is held and the pointer can be read.
    bool BeginGesture(const FDriveOsInput::FDisplayLock& Lock, FIntPoint& OutPointer, FDriveInjectFailure& OutFailure);
    void SendMotion(const FIntPoint& Point);
    void SendButton(EDriveMouseButton Button, bool bPress);
}

#endif // PLATFORM_WINDOWS
