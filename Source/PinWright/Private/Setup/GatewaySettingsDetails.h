// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

// Embeds the agent setup panel (endpoint, one-click install buttons, setup prompt)
// at the top of the plugin's Project Settings page.
class FGatewaySettingsDetails final : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
