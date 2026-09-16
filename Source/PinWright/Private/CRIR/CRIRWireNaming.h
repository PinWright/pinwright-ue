// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRWireNaming.h
//
// Shared CRIR wire-arg naming convention. Both the compiler (literal-vs-wire
// dispatch in CompileRigGraphBlock) and the pin resolver (direction parse on
// the arg name) need to know the `wire_in_` / `wire_out_` prefixes; pulling
// them here keeps a single source of truth.

#pragma once

#include "CoreMinimal.h"


namespace CRIRWireNaming
{
inline constexpr const TCHAR* WireInPrefix = TEXT("wire_in_");
inline constexpr const TCHAR* WireOutPrefix = TEXT("wire_out_");

inline bool IsWireArgName(const FString& Name)
{
    return Name.StartsWith(WireInPrefix) || Name.StartsWith(WireOutPrefix);
}
}
