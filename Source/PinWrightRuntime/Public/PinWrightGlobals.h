// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared globals for PinWright plugin
#pragma once

// Workaround for UE 5.0 engine headers using __has_feature (Clang-specific) with MSVC
// ConcurrentLinearAllocator.h and other experimental headers use this macro
#if defined(_MSC_VER) && !defined(__has_feature)
#define __has_feature(x) 0
#endif

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#include "State/PluginState.h"

// Convenience macros for non-blueprint state accessed from many handler files.
// Blueprint state should use FPluginState::Get().Blueprints() directly.
#define GSequenceRegistry           (FPluginState::Get().SequenceRegistry())
#define GCurrentSequencePath        (FPluginState::Get().CurrentSequencePath())

// Lightweight registry used for created Niagara systems when running in
// fast-mode or when native Niagara factories are not available.
#define GNiagaraRegistry            (FPluginState::Get().NiagaraRegistry())
