// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Engine/DeveloperSettings.h"
#include "PinWrightProjectSettings.generated.h"

// Team-shared PinWright settings, persisted to the project's committed config
// (Config/DefaultEditor.ini via defaultconfig) so output locations are shared
// across the whole team. Per-user preferences live in UPinWrightSettings.

UCLASS(Config = Editor, defaultconfig, meta = (DisplayName = "PinWright (Project)"))
class PINWRIGHT_API UPinWrightProjectSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UPinWrightProjectSettings();

    /** Override for the asset-dump output root; empty = <ProjectSavedDir>/PinWright/asset-dumps. May be absolute or relative to the project directory. */
    UPROPERTY(EditAnywhere, Config, Category = "Output Directories")
    FString AssetDumpRootDirectory;

    /** Override for the generated wiki output directory; empty = <ProjectSavedDir>/PinWright/wiki. May be absolute or relative to the project directory. Generation still writes to an override, but stale-file pruning runs only in the default directory and only for files recorded in PinWright's ownership manifest. */
    UPROPERTY(EditAnywhere, Config, Category = "Output Directories")
    FString WikiOutputDirectory;

    // Container stays at the UDeveloperSettings default ("Project"): these are
    // project-shared values, so they surface in Project Settings, unlike the
    // per-user UPinWrightSettings which lives in Editor Preferences.
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    virtual FName GetSectionName() const override { return TEXT("PinWrightProject"); }
    virtual FText GetSectionText() const override;
};
