// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for widget.set_image_brush — verifies the handler loads the texture,
// assigns it to the named brush property, and rejects missing textures with
// ASSET_NOT_FOUND. Counterfactual: removing the brush assignment line in the
// handler leaves Brush.GetResourceObject() nullptr → AppliesTexture fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Dispatch/RpcDispatcher.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
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

// ---------------------------------------------------------------------------
// Non-texture brush resources (E-set-image-brush-rejects-material).
// Counterfactual: restoring the UTexture2D-only load makes the material tests
// fail with ASSET_NOT_FOUND (and removing the texturePath alias declaration makes
// MaterialDefaultSizeWarns fail at the dispatcher's param gate); dropping the drawable-type guard lets a static mesh
// through to FSlateBrush::SetResourceObject (ensure + success response), which
// fails RejectsUndrawableResource.
// ---------------------------------------------------------------------------
namespace TestWidgetSetImageBrushHandlerHelpers
{
    // Builds a transient WBP with one UImage "TestImage", invokes the verb with
    // Payload (widgetPath/widgetName filled in) through the real FRpcDispatcher so
    // the declared-param gate (resourcePath / texturePath alias) is exercised,
    // copies the resulting brush out, and cleans the fixture up. Returns false if
    // the fixture could not be built.
    bool InvokeOnFreshImage(FAutomationTestBase& Test, const TCHAR* AssetName,
        const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& OutCapture, FSlateBrush& OutBrush)
    {
        const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(AssetName);
        UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
        if (!Test.TestNotNull(TEXT("widget blueprint allocated"), WBP) || !WBP->WidgetTree)
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
        UImage* Image = AddImageToPanel(WBP, Root, TEXT("TestImage"));
        if (!Test.TestNotNull(TEXT("image allocated"), Image))
        {
            return false;
        }
        Test.TestNull(TEXT("precondition: fresh image brush has no resource"),
            Image->GetBrush().GetResourceObject());

        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetStringField(TEXT("widgetName"), TEXT("TestImage"));
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("widget.set_image_brush"),
            TEXT("req-set-image-brush"), Payload, OutCapture.bSuccess, OutCapture.Result, OutCapture.ErrorCode);
        OutCapture.bWasCalled = Sink->bWasCalled;
        OutCapture.Message = Sink->Message;
        Test.TestTrue(TEXT("response was sent"), OutCapture.bWasCalled);
        OutBrush = Image->GetBrush();
        return true;
    }

    const TCHAR* const MaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetImageBrushAppliesMaterialTest,
    "PinWright.widget.set_image_brush.AppliesMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetImageBrushAppliesMaterialTest::RunTest(const FString& Parameters)
{
    using namespace TestWidgetSetImageBrushHandlerHelpers;
    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, MaterialPath);
    if (!TestNotNull(TEXT("precondition: DefaultMaterial loads"), Material))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("resourcePath"), MaterialPath);
    TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
    Size->SetNumberField(TEXT("X"), 148.0);
    Size->SetNumberField(TEXT("Y"), 96.0);
    Payload->SetObjectField(TEXT("imageSize"), Size);

    FTestResponseCapture Capture;
    FSlateBrush Brush;
    if (!InvokeOnFreshImage(*this, TEXT("WBP_SetImageBrushMaterial"), Payload, Capture, Brush))
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("handler reported success (got %s: %s)"), *Capture.ErrorCode, *Capture.Message),
        Capture.bSuccess);
    TestEqual(TEXT("brush resource object is the material"), Brush.GetResourceObject(), static_cast<UObject*>(Material));
    TestTrue(TEXT("ImageSize.X from payload"), FMath::IsNearlyEqual((double)Brush.ImageSize.X, 148.0));
    TestTrue(TEXT("ImageSize.Y from payload"), FMath::IsNearlyEqual((double)Brush.ImageSize.Y, 96.0));
    if (Capture.Result.IsValid())
    {
        TestFalse(TEXT("no default-size warning when imageSize was passed"), Capture.Result->HasField(TEXT("warnings")));
        TestEqual(TEXT("resourceClass reported"), Capture.Result->GetStringField(TEXT("resourceClass")), Material->GetClass()->GetName());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetImageBrushMaterialDefaultSizeWarnsTest,
    "PinWright.widget.set_image_brush.MaterialDefaultSizeWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetImageBrushMaterialDefaultSizeWarnsTest::RunTest(const FString& Parameters)
{
    using namespace TestWidgetSetImageBrushHandlerHelpers;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("texturePath"), MaterialPath); // legacy alias spelling

    FTestResponseCapture Capture;
    FSlateBrush Brush;
    if (!InvokeOnFreshImage(*this, TEXT("WBP_SetImageBrushMaterialNoSize"), Payload, Capture, Brush))
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("handler reported success (got %s: %s)"), *Capture.ErrorCode, *Capture.Message),
        Capture.bSuccess);
    TestNotNull(TEXT("material assigned through the texturePath alias"), Brush.GetResourceObject());
    const FSlateBrush DefaultBrush;
    TestTrue(TEXT("ImageSize kept the FSlateBrush default"),
        FMath::IsNearlyEqual((double)Brush.ImageSize.X, (double)DefaultBrush.ImageSize.X)
        && FMath::IsNearlyEqual((double)Brush.ImageSize.Y, (double)DefaultBrush.ImageSize.Y));
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestTrue(TEXT("response carries a warnings array naming the missing imageSize"),
        Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings)
        && Warnings && Warnings->Num() == 1 && (*Warnings)[0]->AsString().Contains(TEXT("imageSize")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetImageBrushRejectsUndrawableResourceTest,
    "PinWright.widget.set_image_brush.RejectsUndrawableResource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetImageBrushRejectsUndrawableResourceTest::RunTest(const FString& Parameters)
{
    using namespace TestWidgetSetImageBrushHandlerHelpers;
    const TCHAR* MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    if (!TestNotNull(TEXT("precondition: Cube static mesh loads"), LoadObject<UObject>(nullptr, MeshPath)))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("resourcePath"), MeshPath);

    FTestResponseCapture Capture;
    FSlateBrush Brush;
    if (!InvokeOnFreshImage(*this, TEXT("WBP_SetImageBrushUndrawable"), Payload, Capture, Brush))
    {
        return false;
    }
    TestFalse(TEXT("handler reported error"), Capture.bSuccess);
    TestEqual(TEXT("undrawable resource returns INVALID_ASSET_TYPE"), Capture.ErrorCode, FString(TEXT("INVALID_ASSET_TYPE")));
    TestTrue(TEXT("message names the offending class"), Capture.Message.Contains(TEXT("StaticMesh")));
    TestNull(TEXT("brush left untouched"), Brush.GetResourceObject());
    return true;
}
