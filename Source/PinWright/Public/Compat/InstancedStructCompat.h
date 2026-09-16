// Copyright (c) 2026 Alexander Penkin. MIT License.

// InstancedStructCompat.h
// FInstancedStruct / FStructView moved between engine versions: on UE <= 5.4 the
// header ships in the standalone StructUtils module (linked in Build.cs); from UE 5.5
// on it was folded into CoreUObject under the StructUtils/ subfolder. This shim does
// the `__has_include` resolution once and exposes a single availability macro so call
// sites can `#include "Compat/InstancedStructCompat.h"` and gate FInstancedStruct code
// on EARG_HAS_INSTANCED_STRUCT instead of repeating the include ladder per file.

#pragma once

#if __has_include("StructUtils/InstancedStruct.h")
    #include "StructUtils/InstancedStruct.h"
    #define EARG_HAS_INSTANCED_STRUCT 1
#elif __has_include("InstancedStruct.h")
    #include "InstancedStruct.h"
    #define EARG_HAS_INSTANCED_STRUCT 1
#else
    #define EARG_HAS_INSTANCED_STRUCT 0
#endif
