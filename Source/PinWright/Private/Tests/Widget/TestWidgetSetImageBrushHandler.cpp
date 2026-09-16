// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for widget.set_image_brush — verifies the handler loads the texture,
// assigns it to the named brush property, and rejects missing textures with
// ASSET_NOT_FOUND. Counterfactual: removing the brush assignment line in the
// handler leaves Brush.GetResourceObject() nullptr → AppliesTexture fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

namespace
{
    // Mirrors WidgetTestFixtures::AddTextBlockToPanel for UImage children.
    UImage* AddImageToPanel(UWidgetBlueprint* WBP, UPanelWidget* Parent, const TCHAR* Name)
    {
        if (!WBP || !WBP->WidgetTree || !Parent)
        {
            return nullptr;
        }
        UImage* Img = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), FName(Name));
        if (!Img)
        {
            return nullptr;
        }
        Parent->AddChild(Img);
        // Routes through the shared fixture helper: no-op on UE 5.4 (no OnVariableAdded /
        // GUID map), registers on 5.5+. Keeps the version split centralized.
        WidgetTestFixtures::RegisterWidgetVariable(WBP, Img->GetFName());
        return Img;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetImageBrushAppliesTextureTest,
    "PinWright.widget.set_image_brush.AppliesTexture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetImageBrushAppliesTextureTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_SetImageBrushApplies"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    TestNotNull(TEXT("root canvas allocated"), Root);
    UImage* Image = AddImageToPanel(WBP, Root, TEXT("TestImage"));
    TestNotNull(TEXT("image allocated"), Image);
    if (!Root || !Image)
    {
        return false;
    }

    const FString TexturePath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
    UTexture2D* ExpectedTex = LoadObject<UTexture2D>(nullptr, *TexturePath);
    TestNotNull(TEXT("DefaultTexture available"), ExpectedTex);
    if (!ExpectedTex)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("widgetName"), TEXT("TestImage"));
    Payload->SetStringField(TEXT("texturePath"), TexturePath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.set_image_brush handler found"),
        InvokeHandlerWithCapture(TEXT("widget.set_image_brush"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    // Counterfactual: if the brush-assignment line in the handler is reverted,
    // Brush stays default-constructed and GetResourceObject() returns nullptr,
    // failing this assertion.
    const FSlateBrush& Brush = Image->GetBrush();
    TestEqual(TEXT("brush resource object is the loaded texture"),
        Cast<UTexture2D>(Brush.GetResourceObject()),
        ExpectedTex);

    const FIntPoint ImportedSize = ExpectedTex->GetImportedSize();
    const FVector2D Expected(ImportedSize.X, ImportedSize.Y);
    TestTrue(TEXT("ImageSize.X matches imported size"),
        FMath::IsNearlyEqual(Brush.ImageSize.X, Expected.X, (double)FLT_EPSILON));
    TestTrue(TEXT("ImageSize.Y matches imported size"),
        FMath::IsNearlyEqual(Brush.ImageSize.Y, Expected.Y, (double)FLT_EPSILON));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetImageBrushAssetNotFoundTest,
    "PinWright.widget.set_image_brush.AssetNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetImageBrushAssetNotFoundTest::RunTest(const FString& Parameters)
{
    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_SetImageBrushMissing"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        return false;
    }

    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    TestNotNull(TEXT("root canvas allocated"), Root);
    UImage* Image = AddImageToPanel(WBP, Root, TEXT("TestImage"));
    TestNotNull(TEXT("image allocated"), Image);
    if (!Root || !Image)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
    Payload->SetStringField(TEXT("widgetName"), TEXT("TestImage"));
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Game/Does/Not/Exist"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.set_image_brush handler found"),
        InvokeHandlerWithCapture(TEXT("widget.set_image_brush"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("handler reported error"), Capture.bSuccess);
    TestEqual(TEXT("missing texture returns ASSET_NOT_FOUND"),
        Capture.ErrorCode,
        FString(TEXT("ASSET_NOT_FOUND")));

    return true;
}
