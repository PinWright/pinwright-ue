// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Per-process discovery file <root>/instances/<pid>.json (docs/runtime-module.md "Instance file").
// Root is <ProjectSavedDir>/PinWright. Written after every successful bind, user-only permissions,
// deleted on clean shutdown; readers skip files whose pid is gone.
namespace PinWrightInstanceFile
{
    PINWRIGHTRUNTIME_API FString GetPath();

    // Atomic tmp-then-move write to GetPath(). Kind is "editor" or "game"; Map is the current map package or "".
    PINWRIGHTRUNTIME_API bool Write(int32 Port, const FString& Token, const FString& Kind,
                                    const FString& Reason, const FString& Map);

    // Path is the GetPath() value the writer recorded (the test root can change in between).
    PINWRIGHTRUNTIME_API void Delete(const FString& Path);

#if WITH_DEV_AUTOMATION_TESTS
    // Redirects the root for tests; an empty string clears the override.
    PINWRIGHTRUNTIME_API void SetRootOverrideForTests(const FString& Root);
#endif
}
