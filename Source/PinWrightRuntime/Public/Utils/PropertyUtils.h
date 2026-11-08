// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Deprecated umbrella forwarder — prefer the per-concern headers below.
// Split into four concern-focused TUs:
//   PropertyExport.h     — JSON emission from live property values
//   PropertyImport.h     — JSON / text → property writes
//   PropertyInspection.h — structural metadata / reflection
//   PropertyDiff.h       — cross-object property comparison + transfer
#include "Utils/PropertyExport.h"
#include "Utils/PropertyImport.h"
#include "Utils/PropertyInspection.h"
#include "Utils/PropertyDiff.h"
