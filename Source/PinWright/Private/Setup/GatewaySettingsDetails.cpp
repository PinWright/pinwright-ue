// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Setup/GatewaySettingsDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Setup/SGatewaySetupScreen.h"

TSharedRef<IDetailCustomization> FGatewaySettingsDetails::MakeInstance()
{
    return MakeShared<FGatewaySettingsDetails>();
}

void FGatewaySettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    // Custom rows render above the category's default property rows, so the panel
    // sits on top of the existing "Setup" category (with bShowSetupScreenOnLaunch below).
    IDetailCategoryBuilder& SetupCategory = DetailBuilder.EditCategory(
        TEXT("Setup"), FText::GetEmpty(), ECategoryPriority::Important);

    SetupCategory.AddCustomRow(INVTEXT("Agent Setup"))
        .WholeRowContent()
        [
            SNew(SGatewaySetupScreen).bEmbedded(true)
        ];
}
