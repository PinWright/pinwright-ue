// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared widget-blueprint construction helpers used by widget tests
// (designer screenshot RPC + asset.dump preview aspect). Inline so the same
// implementation is available across translation units without an extra .cpp.


#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Toolkits/SimpleAssetEditor.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Misc/EngineVersionComparison.h"

namespace WidgetTestFixtures
{
    // UE 5.6 added UWidgetBlueprint::OnVariableAdded (and WidgetVariableNameToGuidMap).
    // UE 5.4/5.5 has neither; these fixtures don't rely on the GUID map, so registration
    // is a clean no-op there.
    inline void RegisterWidgetVariable(UWidgetBlueprint* WBP, const FName& VariableName)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        (void)WBP;
        (void)VariableName;
#else
        if (WBP)
        {
            WBP->OnVariableAdded(VariableName);
        }
#endif
    }

    inline FString MakeWidgetAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString MakeWidgetDesignerScreenshotAssetPath(const FString& Prefix)
    {
        return MakeWidgetAssetPath(Prefix);
    }

    inline UWidgetBlueprint* MakeTransientWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
            Package, *AssetName, RF_Transient | RF_Public | RF_Standalone | RF_Transactional);
        if (!WBP)
        {
            return nullptr;
        }

        WBP->WidgetTree = NewObject<UWidgetTree>(
            WBP, NAME_None, RF_Transient | RF_Transactional);
        return WBP;
    }

    inline UCanvasPanel* AddCanvasRoot(UWidgetBlueprint* WBP, const TCHAR* Name = TEXT("RootCanvas"))
    {
        if (!WBP || !WBP->WidgetTree)
        {
            return nullptr;
        }

        UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), FName(Name));
        WBP->WidgetTree->RootWidget = Root;
        if (Root)
        {
            RegisterWidgetVariable(WBP, Root->GetFName());
        }
        return Root;
    }

    inline UTextBlock* AddTextBlockToPanel(UWidgetBlueprint* WBP, UPanelWidget* Parent, const TCHAR* Name)
    {
        if (!WBP || !WBP->WidgetTree || !Parent)
        {
            return nullptr;
        }

        UTextBlock* TextBlock = WBP->WidgetTree->ConstructWidget<UTextBlock>(
            UTextBlock::StaticClass(), FName(Name));
        if (!TextBlock)
        {
            return nullptr;
        }

        Parent->AddChild(TextBlock);
        RegisterWidgetVariable(WBP, TextBlock->GetFName());
        return TextBlock;
    }

    // Builds a Widget Blueprint with an empty CanvasPanel root. Caller is expected
    // to add sized children if the designer-preview bounds need to resolve.
    inline UWidgetBlueprint* MakeWidgetDesignerScreenshotBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }

        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            UUserWidget::StaticClass(),
            Package,
            FName(*AssetName),
            BPTYPE_Normal,
            UWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass()));
        if (!WBP || !WBP->WidgetTree)
        {
            return WBP;
        }

        if (!WBP->WidgetTree->RootWidget)
        {
            UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
                UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
            WBP->WidgetTree->RootWidget = Root;
            if (Root)
            {
                RegisterWidgetVariable(WBP, Root->GetFName());
            }
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        return WBP;
    }

    // Pre-opens a Widget Blueprint with FSimpleAssetEditor (toolkit FName
    // "GenericAssetEditor") so the WBP designer never claims it. This is the deterministic
    // failure path for AssetDumpHandler's screenshot diagnostic recording:
    // FWidgetGeometryResolver::FindWidgetBlueprintEditor filters incoming editor instances
    // by toolkit FName, requiring exactly "WidgetBlueprintEditor"; a generic editor is
    // present + primary, so AssetEditorSubsystem::OpenEditorForAsset returns true without
    // ever spawning the WBP-specific editor, and the lookup returns null
    // → CapturePreviewToPng emits "EDITOR_NOT_FOUND" → diagnostic is recorded.
    //
    // Note: a rootless WidgetTree alone does NOT cause failure — UMG substitutes an SSpacer
    // when WidgetTree->RootWidget is null (UserWidget.cpp:1143), and the SDesignerView's
    // PreviewSizeConstraint forces a non-zero allotted geometry, so the renderer still
    // produces a valid PNG. Pre-claiming the asset slot is the only deterministic block.
    inline TSharedRef<FSimpleAssetEditor> PreemptWidgetBlueprintEditorWithGenericEditor(
        UWidgetBlueprint* WBP)
    {
        // EToolkitMode::Standalone matches the default standalone WBP editor mode and
        // avoids needing a host LevelEditor. Returned ref is held by the asset editor
        // subsystem; caller doesn't need to keep it alive but receives it for tidy
        // teardown via CloseAllEditorsForAsset.
        return FSimpleAssetEditor::CreateEditor(
            EToolkitMode::Standalone,
            /*InitToolkitHost=*/nullptr,
            static_cast<UObject*>(WBP));
    }

    // Adds a 200x100 sized text-block child so the preview canvas has non-zero bounds —
    // sidesteps PREVIEW_BOUNDS_NOT_FOUND once the cached Slate widget resolves.
    inline UTextBlock* AddSizedPreviewLabel(UWidgetBlueprint* WBP)
    {
        if (!WBP || !WBP->WidgetTree)
        {
            return nullptr;
        }
        UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
        if (!RootCanvas)
        {
            return nullptr;
        }
        UTextBlock* PreviewLabel = WBP->WidgetTree->ConstructWidget<UTextBlock>(
            UTextBlock::StaticClass(), TEXT("PreviewLabel"));
        if (!PreviewLabel)
        {
            return nullptr;
        }
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(PreviewLabel)))
        {
            CanvasSlot->SetSize(FVector2D(200.0, 100.0));
        }
        RegisterWidgetVariable(WBP, PreviewLabel->GetFName());
        return PreviewLabel;
    }
}
