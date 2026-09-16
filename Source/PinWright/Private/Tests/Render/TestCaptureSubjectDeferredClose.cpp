// Copyright (c) 2026 Alexander Penkin. MIT License.

// CLOSING AN ASSET EDITOR MUST NOT HAPPEN ON THE CAPTURE'S RELEASE STACK, AND MUST STILL HAPPEN.
//
// `render.capture_asset_preview` had no argument value a caller could pick that was known good
// (board `B-capture-asset-preview-no-safe-close-mode`):
//
//   * `closeAfterCapture: true` ran UAssetEditorSubsystem::CloseAllEditorsForAsset inline inside
//     PinWrightCaptureSubject::CloseAssetEditor, which runs the toolkit's whole destructor chain
//     synchronously. That killed the editor process twice in one session through two DIFFERENT
//     providers - an EXCEPTION_ACCESS_VIOLATION reading 0x0 inside the Niagara stack view model,
//     and one reading 0x3f800000 (the bit pattern of 1.0f dereferenced as a pointer) in the mesh
//     case - with every frame from CloseAssetEditor outward identical. Same frame, two callers:
//     the fault is in the shared teardown, not in a subject kind.
//   * `closeAfterCapture: false` closed nothing ever and had no cap: 29 of 37 asset editors opened
//     in one measured session were still open at the end of it, and an asset editor still open
//     when the editor exits faults during shutdown.
//
// WHAT THESE TESTS ASSERT: the GUARD, not the crash. An automation test cannot reproduce an
// access violation without killing its own suite host, so what is measured here is that the
// dangerous work does not happen on the dangerous stack, and that it still happens afterwards.
//
//   1. CloseIsDeferredOffTheReleaseStack - CloseAssetEditor returns without having destroyed the
//      toolkit (the toolkit is provably still alive on return), a close is queued for the asset,
//      and running the queue closes it. Before the fix the toolkit was already destroyed when
//      CloseAssetEditor returned, nothing was ever queued, and the return was `true`.
//   2. LeaveOpenIsBoundedToOneEditor - a window kept by `closeAfterCapture: false` is queued for
//      close as soon as the NEXT capture opens one, and the new one is not. Before the fix nothing
//      in the plugin closed an editor opened with `false`, at any point, ever.
//
// UNABLE TO FAIL IF the host cannot realise a Static Mesh preview viewport. Reported through the
// greppable `PINWRIGHT_ASSERTIONS_SKIPPED:` marker, never as a silent pass.
//
// EVERYTHING IT OPENS IS CLOSED on every exit path including a failing assertion: this file is
// about not leaving asset editors behind, and a test that leaked one would be arguing against
// itself.
#include "Misc/AutomationTest.h"

#include "Handlers/Render/CaptureSubject.h"

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Misc/ScopeExit.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // File-unique names so a Unity merge cannot ODR-collide these with the same-shaped helpers in
    // the sibling Tests/Render/*.cpp files.
    const TCHAR* const PWDeferredClosePrimaryPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    const TCHAR* const PWDeferredCloseSecondaryPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

    // FStaticMeshEditor::GetToolkitFName(), which is what the mesh provider passes and what the
    // acquire's toolkit gate checks before any cast. Shaped exactly like
    // CaptureSubjectProviders_Mesh.cpp's StaticMeshToolkitNames() so the test drives the acquire
    // the way a provider does.
    TArrayView<const FName> PWDeferredCloseStaticMeshToolkits()
    {
        static const FName Names[] = { FName(TEXT("StaticMeshEditor")) };
        return TArrayView<const FName>(Names, UE_ARRAY_COUNT(Names));
    }

    // Codes that mean "this host could not realise a preview viewport", as distinct from "the
    // acquire rejected the request". The same set the sibling render tests use, and for the same
    // reason: anything OUTSIDE it is a real defect, so "the acquire silently did nothing" cannot
    // pass as a skip.
    bool PWDeferredCloseIsHostLimitedCode(const FString& Code)
    {
        return Code == TEXT("PREVIEW_VIEWPORT_NOT_FOUND")
            || Code == TEXT("PREVIEW_NOT_FOUND")
            || Code == TEXT("OPEN_FAILED")
            || Code == TEXT("SUBSYSTEM_MISSING")
            || Code == TEXT("EDITOR_NOT_AVAILABLE")
            || Code == TEXT("UNSUPPORTED_ASSET_EDITOR");
    }

    bool PWDeferredCloseEditorIsOpen(UAssetEditorSubsystem* Subsystem, UObject* Asset)
    {
        return Subsystem && Asset &&
            Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) != nullptr;
    }

    // The acquisition holds the preview FSceneViewport by value (SEditorViewport::GetSceneViewport
    // returns a shared pointer by value), and CloseAssetEditor's holder gate refuses a close while
    // any extra reference is alive. A verb drops these through ReleaseSubjectAndViewportRefs; a
    // test that acquires directly has to drop them itself or it measures the wrong refusal.
    void PWDeferredCloseDropViewportRefs(PinWrightCaptureSubject::FAssetEditorViewportAcquisition& Acquisition)
    {
        Acquisition.ViewportClient = nullptr;
        Acquisition.SceneViewport.Reset();
        Acquisition.ViewportWidget.Reset();
    }
}

// =================================================================================================
// 1. The close leaves the release stack
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectCloseIsDeferredOffTheReleaseStackTest,
    "PinWright.render.capture_subject_close.CloseIsDeferredOffTheReleaseStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectCloseIsDeferredOffTheReleaseStackTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;

    if (!GEditor)
    {
        AddError(TEXT("GEditor is unavailable, so no asset editor can be opened."));
        return false;
    }
    UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Subsystem)
    {
        AddError(TEXT("AssetEditorSubsystem is unavailable, so no asset editor can be opened."));
        return false;
    }
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, PWDeferredClosePrimaryPath);
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("The engine fixture mesh %s did not load."),
            PWDeferredClosePrimaryPath));
        return false;
    }

    // Runs after every assertion below including a failing one. Both halves are needed: the queue
    // must not outlive the test, and neither must a window it opened.
    ON_SCOPE_EXIT
    {
        FlushDeferredAssetEditorCloses();
        if (GEditor)
        {
            if (UAssetEditorSubsystem* SubsystemOnExit =
                    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                SubsystemOnExit->CloseAllEditorsForAsset(Mesh);
            }
        }
    };

    // CLOSED FIRST, so `bWasAlreadyOpen` is false below and the DEFAULT close rule (which closes
    // only a window this call opened) is the rule under test. Left open, the three-state gate would
    // refuse before reaching anything this test measures.
    Subsystem->CloseAllEditorsForAsset(Mesh);
    if (PWDeferredCloseEditorIsOpen(Subsystem, Mesh))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-refused-close"),
            TEXT("The Static Mesh editor for the fixture would not close before the test began, so "
                 "the acquire could not open a window this call owns and the default close rule "
                 "could not be exercised."));
        return true;
    }

    FAssetEditorViewportAcquisition Acquisition;
    FString ErrCode;
    FString ErrMsg;
    if (!AcquireAssetEditorViewport(Mesh,
            PWDeferredCloseStaticMeshToolkits(),
            Acquisition, ErrCode, ErrMsg))
    {
        if (PWDeferredCloseIsHostLimitedCode(ErrCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-viewport"),
                FString::Printf(TEXT("The host could not realise a Static Mesh preview viewport for "
                                     "%s (%s: %s), so no asset editor teardown could be measured."),
                    PWDeferredClosePrimaryPath, *ErrCode, *ErrMsg));
            return true;
        }
        AddError(FString::Printf(TEXT("Acquiring the Static Mesh preview viewport for %s failed "
                                      "with an unexpected code (%s: %s)."),
            PWDeferredClosePrimaryPath, *ErrCode, *ErrMsg));
        return false;
    }
    if (Acquisition.bWasAlreadyOpen)
    {
        PWDeferredCloseDropViewportRefs(Acquisition);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-already-open"),
            TEXT("The fixture's asset editor reported itself already open despite being closed "
                 "immediately before the acquire, so the default close rule would deliberately "
                 "leave it alone and nothing could be measured."));
        return true;
    }

    // The acquire's own references die before the close is asked for, exactly as a verb's do.
    PWDeferredCloseDropViewportRefs(Acquisition);

    const int32 QueuedBefore = NumPendingDeferredAssetEditorCloses();
    const bool bClosedOnThisStack = CloseAssetEditor(Mesh, /*bCloseAfterCapture=*/true,
        /*bCloseRequestedExplicitly=*/false, /*bWasAlreadyOpen=*/false);

    // ---- THE GUARD. Before the fix, all four of these read the other way. ----
    //
    // The toolkit destructor chain ran inside this call and the fault it can raise took the process
    // with it; on a run that survived, CloseAssetEditor returned `true` with the window already
    // gone and nothing queued.
    TestFalse(TEXT("the close does not report itself done on the caller's stack"),
        bClosedOnThisStack);
    TestTrue(TEXT("the asset editor toolkit is still ALIVE when the release path returns - its "
                  "destructor chain did not run on this stack"),
        PWDeferredCloseEditorIsOpen(Subsystem, Mesh));
    TestTrue(TEXT("a close is queued for the asset"),
        HasPendingDeferredAssetEditorClose(Mesh->GetPathName()));
    TestEqual(TEXT("exactly one close was queued by this call"),
        NumPendingDeferredAssetEditorCloses(), QueuedBefore + 1);

    // The package spelling and the object spelling name the same asset, and callers publish
    // whichever the payload used.
    TestTrue(TEXT("the queue answers to the package spelling of the path too"),
        HasPendingDeferredAssetEditorClose(TEXT("/Engine/BasicShapes/Cube")));

    // ---- AND IT STILL HAPPENS. A deferral that never ran would be the leak wearing a fix. ----
    const int32 ClosedByFlush = FlushDeferredAssetEditorCloses();
    TestTrue(TEXT("running the queue closes at least the window this test opened"),
        ClosedByFlush >= 1);
    TestFalse(TEXT("the asset editor is gone once the queue has run"),
        PWDeferredCloseEditorIsOpen(Subsystem, Mesh));
    TestFalse(TEXT("and nothing is left queued for it"),
        HasPendingDeferredAssetEditorClose(Mesh->GetPathName()));
    return true;
}

// =================================================================================================
// 2. `closeAfterCapture: false` is bounded at one open editor
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectLeaveOpenIsBoundedToOneEditorTest,
    "PinWright.render.capture_subject_close.LeaveOpenIsBoundedToOneEditor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectLeaveOpenIsBoundedToOneEditorTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;

    if (!GEditor)
    {
        AddError(TEXT("GEditor is unavailable, so no asset editor can be opened."));
        return false;
    }
    UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Subsystem)
    {
        AddError(TEXT("AssetEditorSubsystem is unavailable, so no asset editor can be opened."));
        return false;
    }
    UStaticMesh* First = LoadObject<UStaticMesh>(nullptr, PWDeferredClosePrimaryPath);
    UStaticMesh* Second = LoadObject<UStaticMesh>(nullptr, PWDeferredCloseSecondaryPath);
    if (!First || !Second)
    {
        AddError(FString::Printf(TEXT("The engine fixture meshes did not both load (%s, %s)."),
            PWDeferredClosePrimaryPath, PWDeferredCloseSecondaryPath));
        return false;
    }

    ON_SCOPE_EXIT
    {
        FlushDeferredAssetEditorCloses();
        if (GEditor)
        {
            if (UAssetEditorSubsystem* SubsystemOnExit =
                    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                SubsystemOnExit->CloseAllEditorsForAsset(First);
                SubsystemOnExit->CloseAllEditorsForAsset(Second);
            }
        }
    };

    Subsystem->CloseAllEditorsForAsset(First);
    Subsystem->CloseAllEditorsForAsset(Second);
    if (PWDeferredCloseEditorIsOpen(Subsystem, First) || PWDeferredCloseEditorIsOpen(Subsystem, Second))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-refused-close"),
            TEXT("A fixture's asset editor would not close before the test began, so the pool "
                 "could not start from a known state."));
        return true;
    }

    // ---- capture one, and ask for the window to be KEPT ----
    FAssetEditorViewportAcquisition FirstAcquisition;
    FString ErrCode;
    FString ErrMsg;
    if (!AcquireAssetEditorViewport(First,
            PWDeferredCloseStaticMeshToolkits(),
            FirstAcquisition, ErrCode, ErrMsg))
    {
        if (PWDeferredCloseIsHostLimitedCode(ErrCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-viewport"),
                FString::Printf(TEXT("The host could not realise a Static Mesh preview viewport for "
                                     "%s (%s: %s), so the open-editor pool could not be measured."),
                    PWDeferredClosePrimaryPath, *ErrCode, *ErrMsg));
            return true;
        }
        AddError(FString::Printf(TEXT("Acquiring the first preview viewport failed with an "
                                      "unexpected code (%s: %s)."), *ErrCode, *ErrMsg));
        return false;
    }
    PWDeferredCloseDropViewportRefs(FirstAcquisition);

    const bool bFirstClosed = CloseAssetEditor(First, /*bCloseAfterCapture=*/false,
        /*bCloseRequestedExplicitly=*/true, /*bWasAlreadyOpen=*/false);
    TestFalse(TEXT("closeAfterCapture:false closes nothing"), bFirstClosed);
    TestFalse(TEXT("...and queues nothing, because the caller asked for the window"),
        HasPendingDeferredAssetEditorClose(First->GetPathName()));
    if (!TestTrue(TEXT("the kept window is still open"),
            PWDeferredCloseEditorIsOpen(Subsystem, First)))
    {
        return true;
    }

    // ---- start the next capture ----
    FAssetEditorViewportAcquisition SecondAcquisition;
    if (!AcquireAssetEditorViewport(Second,
            PWDeferredCloseStaticMeshToolkits(),
            SecondAcquisition, ErrCode, ErrMsg))
    {
        if (PWDeferredCloseIsHostLimitedCode(ErrCode))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-viewport"),
                FString::Printf(TEXT("The host could not realise a second Static Mesh preview "
                                     "viewport (%s: %s), so the eviction could not be measured."),
                    *ErrCode, *ErrMsg));
            return true;
        }
        AddError(FString::Printf(TEXT("Acquiring the second preview viewport failed with an "
                                      "unexpected code (%s: %s)."), *ErrCode, *ErrMsg));
        return false;
    }
    PWDeferredCloseDropViewportRefs(SecondAcquisition);

    // ---- THE GUARD. Before the fix nothing in the plugin ever closed an editor opened with
    // `closeAfterCapture: false`, so the first window survived to editor exit - which is its own
    // documented shutdown fault - and a session accumulated them without a cap.
    TestTrue(TEXT("starting the next capture queues the window the previous one was told to keep"),
        HasPendingDeferredAssetEditorClose(First->GetPathName()));
    TestFalse(TEXT("the window the current capture is USING is not queued"),
        HasPendingDeferredAssetEditorClose(Second->GetPathName()));

    FlushDeferredAssetEditorCloses();
    TestFalse(TEXT("the evicted window is gone"), PWDeferredCloseEditorIsOpen(Subsystem, First));
    TestTrue(TEXT("the window the current capture is using survives the eviction"),
        PWDeferredCloseEditorIsOpen(Subsystem, Second));
    return true;
}
