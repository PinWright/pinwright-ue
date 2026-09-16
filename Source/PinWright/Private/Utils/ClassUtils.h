// Copyright (c) 2026 Alexander Penkin. MIT License.

// Class resolution utilities for PinWright
#pragma once

#include "CoreMinimal.h"


// SHARED REFUSAL CONTRACT for all four resolvers below. Every one of them refuses an input
// containing "//" and returns nullptr, logging the input and the rule at Warning. That byte
// sequence can reach CreatePackage's Fatal through any load these functions perform
// (UObjectGlobals.cpp:1094-1096), which ends the editor process rather than returning an error.
// A caller therefore cannot distinguish "refused" from "not found" by the return value - the log
// line is the discriminator. "//" is the only rule applied: FPackageName::IsValidLongPackageName
// would additionally refuse bare short names and any '.', which are shapes these functions exist
// to accept.

// Resolve a UClass by a variety of heuristics: try full path lookup, attempt
// to load an asset by path (UBlueprint or UClass), then fall back to scanning
// loaded classes by name or path suffix.
PINWRIGHT_API UClass* ResolveClassByName(const FString& ClassNameOrPath);

// Resolve a UClass from a string that may be a full path, a blueprint class
// path, or a short class name.
PINWRIGHT_API UClass* ResolveUClass(const FString& Input);

// Resolve a UEnum from a full path (`/Script/Module.EnumName`) or short name.
// Handles project-module enums by iterating loaded UEnums when direct lookup misses.
// Returns nullptr on miss.
PINWRIGHT_API UEnum* ResolveUEnum(const FString& EnumName);

// Resolve a UScriptStruct by full path, short name, or F-prefixed short name.
// Accepts: `/Script/Module.StructName`, `StructName`, `FStructName` (leading F stripped).
// Falls back to iterating loaded UScriptStructs when direct lookup misses.
// Returns nullptr on miss.
PINWRIGHT_API UScriptStruct* ResolveUScriptStruct(const FString& StructName);
