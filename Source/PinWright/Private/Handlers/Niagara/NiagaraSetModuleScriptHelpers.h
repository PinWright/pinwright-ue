// Copyright (c) 2026 Alexander Penkin. MIT License.

// Thin re-export header for NiagaraEdit::SnapshotInputOverrides and
// NiagaraEdit::EnumerateScriptInputs. Included by tests that need to call
// these helpers directly. All declarations live in NiagaraEditTypes.h;
// this file just provides a stable include target so test files do not have
// to drag in the full NiagaraEditTypes header chain.
#pragma once

#include "Handlers/Niagara/NiagaraEditTypes.h"
