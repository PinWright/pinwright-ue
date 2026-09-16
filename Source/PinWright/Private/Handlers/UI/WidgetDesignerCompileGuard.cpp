// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/WidgetDesignerCompileGuard.h"

#include "Handlers/UI/WidgetGeometryResolver.h"

#include "Blueprint/UserWidget.h"
#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "WidgetBlueprintEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightWidgetDesignerCompileGuard, Log, All);

namespace WidgetDesignerCompileGuard
{
namespace
{
    // File-scope rather than a local static so the unity build cannot merge it with another
    // translation unit's handle of the same shape.
    FDelegateHandle& PreCompileGuardHandle()
    {
        static FDelegateHandle Handle;
        return Handle;
    }

    // FBlueprintPreCompileEvent is void(UBlueprint*); the guard reports whether it did anything,
    // so the subscription binds this adapter rather than the guard itself.
    void PinWrightOnBlueprintPreCompileJettisonPreview(UBlueprint* Blueprint)
    {
        JettisonDesignerPreviewBeforeCompile(Blueprint);
    }

    // Game-thread only, like every path that reaches it. See the header for why a counter is the
    // only observable the guard can offer a test.
    uint64& JettisonedPreviewCount()
    {
        static uint64 Count = 0;
        return Count;
    }
}

bool JettisonDesignerPreview(FWidgetBlueprintEditor* WidgetEditor)
{
    if (!WidgetEditor)
    {
        return false;
    }

    UUserWidget* Preview = WidgetEditor->GetPreview();
    if (!Preview)
    {
        return false;
    }

    // FWidgetBlueprintEditor::DestroyPreview's own order, using only public API
    // (WidgetBlueprintEditor.cpp:1884-1901).
    //
    // 1. Re-stamp the designer flags. UUserWidget::ReleaseSlateResources branches on
    //    IsDesignTime(), and the engine re-establishes the flag here rather than trusting the
    //    value the preview was created with.
    Preview->SetDesignerFlags(WidgetEditor->GetCurrentDesignerFlags());

    // 2. "Immediately notify anyone with a preview out there they need to dispose of it right
    //    now" - InvalidatePreview(bViewOnly=true) IS OnWidgetPreviewUpdated.Broadcast()
    //    (WidgetBlueprintEditor.cpp:795-800), which is what makes SDesignerView drop both its
    //    TObjectPtr to the preview and the Slate widget built from it
    //    (SDesignerView::OnPreviewNeedsRecreation, SDesignerView.cpp:1593-1608). Without this the
    //    destroy below leaves a live SWidget pointing at a garbage UWidget, and
    //    FWidgetBlueprintEditorUtils::DestroyUserWidget's own ensure says so.
    WidgetEditor->InvalidatePreview(/*bViewOnly=*/true);

    // 3. MarkAsGarbage + ReleaseSlateResources. The toolkit's private PreviewWidgetPtr is not
    //    reachable from here, but a garbage-marked object makes it resolve to null and report
    //    IsStale(true), which is exactly what FWidgetBlueprintEditor::Tick reads to rebuild the
    //    preview (WidgetBlueprintEditor.cpp:1047-1051). So the Designer heals itself even if the
    //    compile that follows never reaches its own post-compile broadcast.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    FWidgetBlueprintEditorUtils::DestroyUserWidget(Preview);
#else
    // FWidgetBlueprintEditorUtils::DestroyUserWidget arrived in UE 5.5, factored out of
    // FWidgetBlueprintEditor::DestroyPreview. On 5.4 the same three steps are still inline there
    // (WidgetBlueprintEditor.cpp:1654-1659) and are reproduced verbatim.
    const TWeakPtr<SWidget> PreviewSlateWidgetWeak = Preview->GetCachedWidget();
    Preview->MarkAsGarbage();
    Preview->ReleaseSlateResources(true);
    ensure(!PreviewSlateWidgetWeak.IsValid());
#endif
    return true;
}

bool JettisonDesignerPreviewBeforeCompile(UBlueprint* Blueprint)
{
    UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
    if (!WidgetBlueprint || !GEditor)
    {
        return false;
    }

    // bFocusIfOpen=false: a compile must never steal window focus, and in a shared editor the open
    // Designer is routinely another caller's. FindWidgetBlueprintEditor is the plugin's one
    // verified cast - it checks GetEditorName() == "WidgetBlueprintEditor" before the
    // static_cast<FWidgetBlueprintEditor*> (WidgetGeometryResolver.cpp:61-80).
    FWidgetBlueprintEditor* WidgetEditor =
        FWidgetGeometryResolver::FindWidgetBlueprintEditor(WidgetBlueprint, /*bFocusIfOpen=*/false);
    if (!WidgetEditor)
    {
        return false;
    }

    if (!JettisonDesignerPreview(WidgetEditor))
    {
        return false;
    }

    ++JettisonedPreviewCount();

    UE_LOG(LogPinWrightWidgetDesignerCompileGuard, Verbose,
        TEXT("Destroyed the live UMG Designer preview for %s before its compile; the toolkit ")
        TEXT("rebuilds it on its next tick. Compiling under a live preview faults on the next ")
        TEXT("Slate paint (UUserWidget::RebuildWidget dereferences a WidgetTree the compile ")
        TEXT("nulled)."),
        *WidgetBlueprint->GetPathName());
    return true;
}

void RegisterPreCompileGuard()
{
    if (!GEditor || PreCompileGuardHandle().IsValid())
    {
        return;
    }
    PreCompileGuardHandle() = GEditor->OnBlueprintPreCompile().AddStatic(
        &PinWrightOnBlueprintPreCompileJettisonPreview);
}

void UnregisterPreCompileGuard()
{
    if (!PreCompileGuardHandle().IsValid())
    {
        return;
    }
    if (GEditor)
    {
        GEditor->OnBlueprintPreCompile().Remove(PreCompileGuardHandle());
    }
    PreCompileGuardHandle().Reset();
}

bool IsPreCompileGuardRegistered()
{
    return PreCompileGuardHandle().IsValid();
}

uint64 GetJettisonedPreviewCount()
{
    return JettisonedPreviewCount();
}

}   // namespace WidgetDesignerCompileGuard
