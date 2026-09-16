// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Gate for the hard-linked engine-plugin integrations quarantined into
// LoadingPhase=None sub-modules (PinWrightGeometry, PinWrightPCG,
// PinWrightChooser, PinWrightPoseSearch, PinWrightCommonUI). The main module
// loads each sub-module at subsystem init only when its owning engine plugin
// is enabled, so UnrealEditor-PinWright.dll never hard-imports DLLs of plugins
// a consumer project may have disabled (Fab LoadLibrary error 126).
//
// This header must stay free of engine-plugin includes — it is compiled into
// the main module unconditionally.
namespace IntegrationGates
{
    // For each gated integration: if the owning engine plugin is enabled, load
    // its sub-module; otherwise record it as skipped. Must run BEFORE
    // FRpcDispatcher::DrainAutoRegistrations so sub-module static-init handler
    // registrations drain together with the main-module ones.
    void LoadEnabledIntegrations();

    // Returns the owning engine plugin name if Method starts with a skipped
    // integration's method prefix (e.g. "geometry.", "ui.activatable_");
    // empty otherwise.
    FString FindSkippedByMethod(const FString& Method);

    // Returns the owning engine plugin name if Slug exactly names a skipped
    // integration's wiki namespace (geometry, pcg, chooser, pose_search);
    // empty otherwise. CommonUI has no exclusive namespace (methods live under
    // the shared "ui" namespace) and never matches here.
    FString FindSkippedByNamespace(const FString& Slug);
}
