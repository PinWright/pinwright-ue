// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWrightProjectSettings.h"

#include "Internationalization/Text.h"

UPinWrightProjectSettings::UPinWrightProjectSettings()
{
    // Empty = use the built-in defaults under <ProjectSavedDir>/PinWright.
    AssetDumpRootDirectory = FString();
    WikiOutputDirectory = FString();
}

FText UPinWrightProjectSettings::GetSectionText() const
{
    return NSLOCTEXT("PinWright", "ProjectSettingsSection", "PinWright (Project)");
}
