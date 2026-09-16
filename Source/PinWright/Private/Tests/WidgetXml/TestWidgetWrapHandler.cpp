// Copyright (c) 2026 Alexander Penkin. MIT License.

// Counterfactual: if UPanelWidget::ReplaceChild is swapped for
// RemoveChild+AddChild, the wrapper gets a fresh UCanvasPanelSlot at (0,0) and
// the Position/Size assertions below fail. ReplaceChild rehomes the existing
// UPanelSlot so the grandparent layout is preserved for free.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "WidgetXmlTestHelpers.h"
#include "Handlers/ErrorCodes.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Overlay.h"
#include "Components/Image.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Compat/EngineVersionCompat.h"


using WidgetXmlTestHelpers::MakeOnDiskShapedWidgetBlueprint;

namespace
{
    FString MakeWrapTestAssetPath()
    {
        return FString::Printf(TEXT("/Game/_Test/WBP_WidgetWrap_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetWrapPreservesGrandparentSlotTest,
    "PinWright.widget.wrap.PreservesGrandparentSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetWrapPreservesGrandparentSlotTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeWrapTestAssetPath();

    // 1. Build an in-memory widget blueprint at the /Game/_Test path the
    // handler will look up via FindObject.
    UWidgetBlueprint* WidgetBP = MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WidgetBP);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        return false;
    }

    // 2. Construct the minimal grandparent -> parent tree the handler needs.
    UCanvasPanel* Root = WidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("Root"));
    WidgetBP->WidgetTree->RootWidget = Root;

    UImage* Img = WidgetBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(), TEXT("Img"));
    UCanvasPanelSlot* ImgSlot = Cast<UCanvasPanelSlot>(Root->AddChild(Img));

    // ConstructWidget does not populate WidgetVariableNameToGuidMap. The
    // handler's ForEachSourceWidget loop calls EnsureWidgetVariableGuid which
    // ensureAlways-fails on any source widget missing a GUID entry. Register
    // these two here; the handler registers the new wrapper itself.
    // UE 5.4/5.5 has no OnVariableAdded / GUID map (and no such ensure), so the
    // pre-registration is a clean no-op there.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    WidgetBP->OnVariableAdded(Root->GetFName());
    WidgetBP->OnVariableAdded(Img->GetFName());
#endif
    TestNotNull(TEXT("image slot is UCanvasPanelSlot"), ImgSlot);
    if (!ImgSlot)
    {
        return false;
    }

    const FVector2D ExpectedPosition(100.0, 200.0);
    const FVector2D ExpectedSize(300.0, 50.0);
    ImgSlot->SetPosition(ExpectedPosition);
    ImgSlot->SetSize(ExpectedSize);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    // 3. Invoke widget.wrap: wrap the Image in an Overlay named "Wrap".
    TSharedPtr<FJsonObject> WrapPayload = MakeShared<FJsonObject>();
    WrapPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    WrapPayload->SetStringField(TEXT("targetName"), TEXT("Img"));
    WrapPayload->SetStringField(TEXT("wrapperType"), TEXT("Overlay"));
    WrapPayload->SetStringField(TEXT("wrapperName"), TEXT("Wrap"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("widget.wrap"), WrapPayload, Capture);
    TestTrue(TEXT("widget.wrap handler found"), bFound);
    TestTrue(TEXT("widget.wrap succeeded"), Capture.bSuccess);

    // 4. Structural assertions.
    UWidget* WrapperFound = WidgetBP->WidgetTree->FindWidget(FName(TEXT("Wrap")));
    TestNotNull(TEXT("wrapper exists by name"), WrapperFound);
    UOverlay* Wrapper = Cast<UOverlay>(WrapperFound);
    TestNotNull(TEXT("wrapper is UOverlay"), Wrapper);

    UWidget* ImgFound = WidgetBP->WidgetTree->FindWidget(FName(TEXT("Img")));
    TestNotNull(TEXT("image still exists"), ImgFound);
    TestTrue(TEXT("image is UImage"), ImgFound && ImgFound->IsA<UImage>());
    if (ImgFound)
    {
        TestEqual(TEXT("image is parented by wrapper"),
            (UObject*)ImgFound->GetParent(), (UObject*)Wrapper);
    }

    // 5. Critical assertion — the wrapper's slot on the grandparent must be
    // the original UCanvasPanelSlot with the exact layout values from (2).
    // If ReplaceChild were swapped for RemoveChild+AddChild this fails because
    // a fresh slot with default (0,0)/default-size would be constructed.
    if (Wrapper)
    {
        UCanvasPanelSlot* WrapperCanvasSlot = Cast<UCanvasPanelSlot>(Wrapper->Slot);
        TestNotNull(TEXT("wrapper slot is UCanvasPanelSlot on grandparent"), WrapperCanvasSlot);
        if (WrapperCanvasSlot)
        {
            TestEqual(TEXT("wrapper slot position preserved"),
                WrapperCanvasSlot->GetPosition(), ExpectedPosition);
            TestEqual(TEXT("wrapper slot size preserved"),
                WrapperCanvasSlot->GetSize(), ExpectedSize);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetWrapFailurePreservesHierarchyTest,
    "PinWright.widget.wrap.FailurePreservesHierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetWrapFailurePreservesHierarchyTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeWrapTestAssetPath();
    UWidgetBlueprint* WidgetBP = MakeOnDiskShapedWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("widget blueprint allocated"), WidgetBP);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* Root = WidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("Root"));
    WidgetBP->WidgetTree->RootWidget = Root;
    UImage* Img = WidgetBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(), TEXT("Img"));
    UCanvasPanelSlot* ImgSlot = Cast<UCanvasPanelSlot>(Root->AddChild(Img));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    WidgetBP->OnVariableAdded(Root->GetFName());
    WidgetBP->OnVariableAdded(Img->GetFName());
#endif
    TestNotNull(TEXT("image slot is UCanvasPanelSlot"), ImgSlot);
    if (!ImgSlot)
    {
        return false;
    }

    const FVector2D ExpectedPosition(100.0, 200.0);
    const FVector2D ExpectedSize(300.0, 50.0);
    ImgSlot->SetPosition(ExpectedPosition);
    ImgSlot->SetSize(ExpectedSize);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    const int32 OriginalIndex = Root->GetChildIndex(Img);
    UPanelSlot* OriginalSlot = Img->Slot;

    TSharedPtr<FJsonObject> InvalidSlot = MakeShared<FJsonObject>();
    InvalidSlot->SetNumberField(TEXT("ZOrder"), 7);
    InvalidSlot->SetStringField(TEXT("NotARealSlotProperty"), TEXT("bad"));

    TSharedPtr<FJsonObject> WrapPayload = MakeShared<FJsonObject>();
    WrapPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    WrapPayload->SetStringField(TEXT("targetName"), TEXT("Img"));
    WrapPayload->SetStringField(TEXT("wrapperType"), TEXT("Overlay"));
    WrapPayload->SetStringField(TEXT("wrapperName"), TEXT("WrapFailure"));
    WrapPayload->SetObjectField(TEXT("wrapperSlot"), InvalidSlot);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.wrap handler found"),
        InvokeHandlerWithCapture(TEXT("widget.wrap"), WrapPayload, Capture));
    TestTrue(TEXT("widget.wrap responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid wrapper slot is rejected"), Capture.bSuccess);
    TestEqual(TEXT("invalid wrapper slot uses INVALID_PROPERTY"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PROPERTY));

    TestNull(TEXT("failed wrap does not leave a wrapper"),
        WidgetBP->WidgetTree->FindWidget(FName(TEXT("WrapFailure"))));
    TestEqual(TEXT("original child remains at its original index"),
        Root->GetChildAt(OriginalIndex), static_cast<UWidget*>(Img));
    TestEqual(TEXT("original parent remains attached"), Img->GetParent(),
        static_cast<UPanelWidget*>(Root));
    TestEqual(TEXT("original slot object is preserved"), Img->Slot.Get(), OriginalSlot);
    TestEqual(TEXT("original slot position is preserved"), ImgSlot->GetPosition(), ExpectedPosition);
    TestEqual(TEXT("original slot size is preserved"), ImgSlot->GetSize(), ExpectedSize);

    // The reflected Slots array is accepted by the preflight scratch object,
    // and null plus an owned original slot remain legal inputs. They deliberately
    // make a Button appear occupied before ReplaceChild; ReplaceChild then
    // mutates the grandparent slot, and AddChild fails on the single-child
    // wrapper. This exercises rollback after hierarchy mutation.
    TArray<TSharedPtr<FJsonValue>> SeededSlots;
    SeededSlots.Add(MakeShared<FJsonValueNull>());
    SeededSlots.Add(MakeShared<FJsonValueString>(ImgSlot->GetPathName()));
    TSharedPtr<FJsonObject> WrapperProperties = MakeShared<FJsonObject>();
    WrapperProperties->SetArrayField(TEXT("Slots"), SeededSlots);

    TSharedPtr<FJsonObject> LateFailurePayload = MakeShared<FJsonObject>();
    LateFailurePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    LateFailurePayload->SetStringField(TEXT("targetName"), TEXT("Img"));
    LateFailurePayload->SetStringField(TEXT("wrapperType"), TEXT("Button"));
    LateFailurePayload->SetStringField(TEXT("wrapperName"), TEXT("WrapLateFailure"));
    LateFailurePayload->SetObjectField(TEXT("wrapperProperties"), WrapperProperties);

    FTestResponseCapture LateFailureCapture;
    TestTrue(TEXT("late-failure widget.wrap handler found"),
        InvokeHandlerWithCapture(TEXT("widget.wrap"), LateFailurePayload, LateFailureCapture));
    TestTrue(TEXT("late-failure widget.wrap responded"), LateFailureCapture.bWasCalled);
    TestFalse(TEXT("late AddChild failure is reported"), LateFailureCapture.bSuccess);
    TestEqual(TEXT("late AddChild failure uses INTERNAL_ERROR"), LateFailureCapture.ErrorCode,
        FString(ErrorCodes::ERR_INTERNAL_ERROR));
    TestNull(TEXT("late-failed wrap does not leave a wrapper"),
        WidgetBP->WidgetTree->FindWidget(FName(TEXT("WrapLateFailure"))));
    TestEqual(TEXT("late-failed wrap restores the original child count"),
        Root->GetChildrenCount(), 1);
    TestEqual(TEXT("late-failed wrap restores the original child index"),
        Root->GetChildAt(OriginalIndex), static_cast<UWidget*>(Img));
    TestEqual(TEXT("late-failed wrap restores the original parent"), Img->GetParent(),
        static_cast<UPanelWidget*>(Root));
    TestEqual(TEXT("late-failed wrap restores the original slot object"), Img->Slot.Get(), OriginalSlot);
    TestEqual(TEXT("late-failed wrap restores the original slot position"),
        ImgSlot->GetPosition(), ExpectedPosition);
    TestEqual(TEXT("late-failed wrap restores the original slot size"),
        ImgSlot->GetSize(), ExpectedSize);

    const FString ForeignAssetPath = MakeWrapTestAssetPath();
    UWidgetBlueprint* ForeignWidgetBP = MakeOnDiskShapedWidgetBlueprint(ForeignAssetPath);
    TestNotNull(TEXT("foreign widget blueprint allocated"), ForeignWidgetBP);
    if (!ForeignWidgetBP || !ForeignWidgetBP->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* ForeignRoot = ForeignWidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("ForeignRoot"));
    ForeignWidgetBP->WidgetTree->RootWidget = ForeignRoot;
    UImage* ForeignImg = ForeignWidgetBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(), TEXT("ForeignImg"));
    UCanvasPanelSlot* ForeignSlot = Cast<UCanvasPanelSlot>(ForeignRoot->AddChild(ForeignImg));
    TestNotNull(TEXT("foreign image slot is UCanvasPanelSlot"), ForeignSlot);
    if (!ForeignSlot)
    {
        return false;
    }

    const int32 ForeignOriginalIndex = ForeignRoot->GetChildIndex(ForeignImg);
    UPanelSlot* ForeignOriginalSlot = ForeignImg->Slot;
    TArray<TSharedPtr<FJsonValue>> ForeignSlots;
    ForeignSlots.Add(MakeShared<FJsonValueString>(ForeignSlot->GetPathName()));
    TSharedPtr<FJsonObject> ForeignWrapperProperties = MakeShared<FJsonObject>();
    ForeignWrapperProperties->SetArrayField(TEXT("Slots"), ForeignSlots);

    TSharedPtr<FJsonObject> ForeignReferencePayload = MakeShared<FJsonObject>();
    ForeignReferencePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    ForeignReferencePayload->SetStringField(TEXT("targetName"), TEXT("Img"));
    ForeignReferencePayload->SetStringField(TEXT("wrapperType"), TEXT("Overlay"));
    ForeignReferencePayload->SetStringField(TEXT("wrapperName"), TEXT("WrapForeignRejected"));
    ForeignReferencePayload->SetObjectField(TEXT("wrapperProperties"), ForeignWrapperProperties);

    FTestResponseCapture ForeignReferenceCapture;
    TestTrue(TEXT("foreign-reference widget.wrap handler found"),
        InvokeHandlerWithCapture(TEXT("widget.wrap"), ForeignReferencePayload,
            ForeignReferenceCapture));
    TestTrue(TEXT("foreign-reference widget.wrap responded"), ForeignReferenceCapture.bWasCalled);
    TestFalse(TEXT("foreign slot reference is rejected before mutation"),
        ForeignReferenceCapture.bSuccess);
    TestEqual(TEXT("foreign slot reference uses INVALID_PROPERTY"),
        ForeignReferenceCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PROPERTY));
    TestNull(TEXT("foreign-reference wrap does not leave a wrapper"),
        WidgetBP->WidgetTree->FindWidget(FName(TEXT("WrapForeignRejected"))));
    TestEqual(TEXT("foreign-reference wrap preserves the original target child count"),
        Root->GetChildrenCount(), 1);
    TestEqual(TEXT("foreign-reference wrap preserves the original target child"),
        Root->GetChildAt(OriginalIndex), static_cast<UWidget*>(Img));
    TestEqual(TEXT("foreign-reference wrap preserves the original target parent"),
        Img->GetParent(), static_cast<UPanelWidget*>(Root));
    TestEqual(TEXT("foreign-reference wrap preserves the original target slot"),
        Img->Slot.Get(), OriginalSlot);
    TestEqual(TEXT("foreign-reference wrap preserves the original target slot position"),
        ImgSlot->GetPosition(), ExpectedPosition);
    TestEqual(TEXT("foreign-reference wrap preserves the original target slot size"),
        ImgSlot->GetSize(), ExpectedSize);
    TestEqual(TEXT("foreign-reference wrap preserves the foreign child count"),
        ForeignRoot->GetChildrenCount(), 1);
    TestEqual(TEXT("foreign-reference wrap preserves the foreign child index"),
        ForeignRoot->GetChildAt(ForeignOriginalIndex), static_cast<UWidget*>(ForeignImg));
    TestEqual(TEXT("foreign-reference wrap preserves the foreign child parent"),
        ForeignImg->GetParent(), static_cast<UPanelWidget*>(ForeignRoot));
    TestEqual(TEXT("foreign-reference wrap preserves the foreign slot"),
        ForeignImg->Slot.Get(), ForeignOriginalSlot);
    return true;
}
