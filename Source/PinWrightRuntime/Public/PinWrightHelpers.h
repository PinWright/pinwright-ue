// Copyright (c) 2026 Alexander Penkin. MIT License.

// Umbrella header — pulls in all extracted utility modules.
// Previously 2,751 lines of static inline functions; each module now lives
// in its own translation unit under Utils/.
#pragma once

#include "Runtime/Launch/Resources/Version.h"
#include "Utils/PathUtils.h"
#include "Utils/AssetResolveUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogUtils.h"
#include "Utils/PropertyUtils.h"
#include "Utils/ActorUtils.h"
#include "PinWrightGlobals.h"

// The editor-only asset helpers (saves, creation) live in the editor module. Modules that can
// see it (PinWright and its sub-modules) keep getting them through this header; the runtime
// module cannot, so it never includes an editor-module header.
#if __has_include("Utils/AssetUtils.h")
#include "Utils/AssetUtils.h"
#elif WITH_EDITOR
// What Utils/AssetUtils.h used to pull in transitively for editor builds of the runtime module.
#include "Editor.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif
