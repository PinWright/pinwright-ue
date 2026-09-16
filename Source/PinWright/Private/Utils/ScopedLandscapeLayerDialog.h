// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Compat/EngineVersionCompat.h"
#include "LandscapeSettings.h"

// Suppress the modal "Insert New Landscape Edit Layer" dialog for the duration of a
// programmatic landscape/water operation.
//
// When the Water plugin (or any auto-layer source) is active, adding a landscape edit
// layer routes through FLandscapeEditorModule::GetOrCreateEditLayer, which raises a
// blocking modal drag-and-drop dialog gated only by
// ULandscapeSettings::bShowDialogForAutomaticLayerCreation. That path has no unattended
// guard, so the dialog hangs both headless automation runs and live MCP sessions — there
// is never a user present to drive it, and "append" is always the correct insertion for
// programmatic creation. This RAII guard disables the flag while in scope and restores it.
//
// The triggering broadcast (GEngine->OnLevelActorAdded, consumed by the Water editor
// module) fires synchronously inside SpawnActor, so wrapping the spawn call is sufficient.
struct FScopedLandscapeLayerDialog
{
    ULandscapeSettings* Settings = nullptr;
    bool bPrev = false;

    FScopedLandscapeLayerDialog()
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        // ULandscapeSettings::bShowDialogForAutomaticLayerCreation was added in UE 5.5.
        Settings = GetMutableDefault<ULandscapeSettings>();
        if (Settings)
        {
            bPrev = Settings->bShowDialogForAutomaticLayerCreation;
            Settings->bShowDialogForAutomaticLayerCreation = false;
        }
#endif
    }

    ~FScopedLandscapeLayerDialog()
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        if (Settings)
        {
            Settings->bShowDialogForAutomaticLayerCreation = bPrev;
        }
#endif
    }
};
