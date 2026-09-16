// Copyright (c) 2026 Alexander Penkin. MIT License.

// JsonKeyCompat.h
// UE 5.8 changed FJsonObject::Values from TMap<FString, ...> to
// TMap<UE::FSharedString, ...>. That breaks every site that treats a JSON key as
// an FString: string ops on Pair.Key (.Equals/+/Add to TArray<FString>), passing
// Pair.Key to a const FString& parameter, and map lookups keyed by an FString
// (Find/Contains/operator[]/GetKeys/GenerateKeyArray).
//
// These two helpers bridge both directions on every supported engine (5.3-5.8):
//   JsonKeyToString(Key) -> FString   : normalize an iterated key for string use.
//   JsonFieldKey(Str)    -> map key   : adapt an FString for a Values lookup.
// On <5.8 the key type is already FString, so both are pass-through references.

#pragma once

#include "Compat/EngineVersionCompat.h"
#include "Containers/UnrealString.h"

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)

#include "Containers/SharedString.h"

namespace EARGCompat
{
    // FJsonObject::Values key (UE::FSharedString) -> FString for string operations.
    inline FString JsonKeyToString(const UE::FSharedString& Key)
    {
        return FString(*Key);
    }

    // FString -> FJsonObject::Values key type for Find/Contains/operator[] lookups.
    inline UE::FSharedString JsonFieldKey(const FString& Key)
    {
        return UE::FSharedString(*Key);
    }
}

#else

namespace EARGCompat
{
    inline const FString& JsonKeyToString(const FString& Key)
    {
        return Key;
    }

    inline const FString& JsonFieldKey(const FString& Key)
    {
        return Key;
    }
}

#endif
