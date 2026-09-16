// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// TEAR THE UMG DESIGNER PREVIEW DOWN BEFORE A WIDGET BLUEPRINT IS RECOMPILED.
//
// THE ENGINE CONTRACT THIS RESTORES. The only engine path that compiles a Widget Blueprint with
// its Designer open destroys the live preview FIRST:
//
//     void FWidgetBlueprintEditor::Compile()          // UE 5.8 WidgetBlueprintEditor.cpp:1799
//     {
//         DestroyPreview();
//         FBlueprintEditor::Compile();
//         ...
//     }
//
// and SDesignerView says why in its own words (SDesignerView.cpp:1593-1598): "Because widget
// blueprints can contain other widget blueprints, the safe thing to do is to have all designers
// jettison their previews on the compilation of any widget blueprint. We do this to prevent having
// slate widgets that still may reference data in their owner UWidget that has been garbage
// collected." That jettison is driven by FWidgetBlueprintEditor::OnWidgetPreviewUpdated, which is
// broadcast ONLY from the toolkit's own preview lifecycle - InvalidatePreview(bViewOnly=true),
// DestroyPreview and UpdatePreview.
//
// WHAT PINWRIGHT DID INSTEAD. Every compile this plugin runs goes to
// FKismetEditorUtilities::CompileBlueprint (53 call sites: blueprint.compile, blueprint.compile_bpir,
// blueprint.add_function, blueprint.add_variable, the BPIR emitters, the widget event binder ...),
// which is one layer BELOW the toolkit. No designer is asked to jettison anything, so the preview
// UUserWidget is alive across the class rebuild, and the fault lands on an ordinary Slate paint
// rather than in the compile:
//
//     UUserWidget::RebuildWidget()            UserWidget.cpp:1214  <- WidgetTree->RootWidget, null
//     UWidget::TakeWidget_Private()           Widget.cpp:993
//     SDesignerView::UpdatePreviewWidget()    SDesignerView.cpp:2305
//     SDesignerView::Tick()                   SDesignerView.cpp:2401
//     ... SWindow::PaintSlowPath ...          EXCEPTION_ACCESS_VIOLATION reading 0x38
//
// The null is reachable because UWidgetBlueprintGeneratedClass::PurgeClass nulls the CLASS's widget
// tree the moment the compile starts (WidgetBlueprintGeneratedClass.cpp:390-397), and
// UUserWidget::RebuildWidget dereferences the INSTANCE's WidgetTree with no null test at all
// (UserWidget.cpp:1213). A preview instance re-initialising against a purged class - or reinstanced
// into a fresh object, whose WidgetTree is `Transient, DuplicateTransient` and therefore never
// copied (UserWidget.h:1523) - has a null WidgetTree and paints one frame later. The whole editor
// dies, taking every other agent sharing the process with it
// (board B-screenshot-designer-leaves-designer-open-compile-crash).
//
// WHY THIS IS A HOOK AND NOT AN EDITOR_OPEN REFUSAL. The Niagara guard's own header
// (Handlers/Niagara/NiagaraEditorOpenGuard.h) states the test: refuse only where the mutation
// invalidates something a live widget caches AND no notification reaches it. Here a notification
// DOES reach it - FWidgetBlueprintEditor::OnBlueprintChangedImpl destroys and rebuilds the preview
// after the compile (WidgetBlueprintEditor.cpp:807-816) - and the engine compiles widgets under an
// open Designer all day. What is missing is only the PRE-compile teardown, so the fix is to satisfy
// the contract, not to refuse the compile. Refusing would also be unusable in a shared editor,
// where the open Designer routinely belongs to somebody else.
//
// WHY THE HOOK IS GLOBAL. GEditor->BroadcastBlueprintPreCompile fires once per blueprint inside the
// compilation manager, immediately before the class is purged (BlueprintCompilationManager.cpp:1361-1367).
// Subscribing there covers every compile route in the process - all 53 plugin call sites, any future
// one, and the engine's own toolkit-less paths such as Compile All Blueprints - from one place. It
// is idempotent by construction: a compile that already went through FWidgetBlueprintEditor::Compile
// finds no preview and does nothing.
//
// SCOPE, STATED SO IT IS NOT MISTAKEN FOR AN OVERSIGHT: only the compiled blueprint's OWN editor.
// SDesignerView's comment says "all designers", but the engine does not implement that globally -
// OnPreviewNeedsRecreation is bound per toolkit (SDesignerView.cpp:387), so a designer jettisons
// only when ITS OWN blueprint changes, and a parent widget embedding a recompiled child relies on
// the reinstancer plus the toolkit's own staleness check. Matching FWidgetBlueprintEditor::Compile
// exactly is what this guard restores; widening it to every open designer on every compile would
// destroy state no engine path destroys, in a process several agents share. A cross-widget case
// needs its own evidence and its own ticket.
//
// The teardown uses only public UMGEditor API, in the same order DestroyPreview uses
// (WidgetBlueprintEditor.cpp:1884-1901): re-stamp the designer flags, broadcast
// OnWidgetPreviewUpdated so every listener drops its Slate reference, then destroy the widget.
// FWidgetBlueprintEditor::PreviewWidgetPtr is private and cannot be reset from here, but
// MarkAsGarbage makes it resolve to null, which is what GetPreview() and the toolkit's own
// `PreviewWidgetPtr.IsStale(true)` rebuild check both read (WidgetBlueprintEditor.cpp:1047-1051).
// So the preview comes back on the toolkit's next Tick even if the compile never reaches its own
// post-compile broadcast.

#include "CoreMinimal.h"

class FWidgetBlueprintEditor;
class UBlueprint;

namespace WidgetDesignerCompileGuard
{
    // Destroy the live Designer preview held by one open Widget Blueprint editor, the way
    // FWidgetBlueprintEditor::DestroyPreview does. Returns true when a preview was torn down;
    // false when the editor is null or already has none (so calling it twice is a no-op).
    //
    // The toolkit rebuilds the preview by itself: PreviewWidgetPtr goes stale, and Tick's
    // stale/invalidated check calls RefreshPreview. Nothing here schedules that.
    bool JettisonDesignerPreview(FWidgetBlueprintEditor* WidgetEditor);

    // The pre-compile step. No-op unless Blueprint is a UWidgetBlueprint with an open Widget
    // Blueprint editor holding a live preview. Returns true when a preview was torn down.
    bool JettisonDesignerPreviewBeforeCompile(UBlueprint* Blueprint);

    // Subscribe / unsubscribe JettisonDesignerPreviewBeforeCompile to
    // GEditor->OnBlueprintPreCompile(). Both are idempotent and safe with no GEditor.
    void RegisterPreCompileGuard();
    void UnregisterPreCompileGuard();

    // Whether the hook is currently subscribed. For tests, which cannot otherwise observe a
    // one-line registration.
    bool IsPreCompileGuardRegistered();

    // How many previews JettisonDesignerPreviewBeforeCompile has torn down this session,
    // monotonic. THIS IS THE ONLY WAY A TEST CAN SEE THE GUARD RUN INSIDE A REAL COMPILE, and the
    // reason is an engine property worth knowing before writing any listener:
    // TMulticastDelegateBase::Broadcast walks its invocation list BACKWARDS -- "call bound
    // functions in reverse order, so we ignore any instances that may be added by callees"
    // (UE 5.8 Core/Public/Delegates/MulticastDelegateBase.h:299-300) -- while AddDelegateInstance
    // APPENDS (:336). So the LAST listener registered runs FIRST, and a second listener added at
    // test time to watch this one's effect always runs BEFORE it and always sees a live preview,
    // whether the guard works or not. A delta on this counter is order-free and cannot be faked by
    // a compile that never reached the hook.
    uint64 GetJettisonedPreviewCount();
}
