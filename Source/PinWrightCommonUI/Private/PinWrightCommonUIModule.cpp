// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Modules/ModuleManager.h"

// No startup/shutdown logic: the module exists only to host the CommonUI-typed
// ui.activatable_* handlers, which self-register via REGISTER_RPC_HANDLER statics.
IMPLEMENT_MODULE(FDefaultModuleImpl, PinWrightCommonUI)
