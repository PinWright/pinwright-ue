// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Modules/ModuleManager.h"

// Carrier module for the PCG handler/decompiler/test files split out of the main
// PinWright module; all registration happens via static auto-registration, so no
// custom module implementation is needed.
IMPLEMENT_MODULE(FDefaultModuleImpl, PinWrightPCG)
