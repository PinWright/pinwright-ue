// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace GatewayAuthToken
{
    // <root>/gateway-token, absolute + normalized. Root is <ProjectSavedDir>/PinWright.
    PINWRIGHT_API FString GetTokenFilePath();

    // Returns the trimmed existing token when the file is present and non-empty;
    // otherwise generates 32 CSPRNG bytes, persists them atomically, and returns
    // the new token. Returns "" only on a hard failure (RNG or disk write).
    PINWRIGHT_API FString GetOrCreateToken();

    // Length-safe (length is not treated as secret), value-constant-time equality.
    PINWRIGHT_API bool ConstantTimeEquals(const FString& A, const FString& B);

#if WITH_DEV_AUTOMATION_TESTS
    // Redirects the token root for tests; an empty string clears the override.
    PINWRIGHT_API void SetTokenRootOverrideForTests(const FString& Root);
#endif
}
