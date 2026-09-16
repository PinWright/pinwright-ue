// Copyright (c) 2026 Alexander Penkin. MIT License.

// OPENING AN ASSET EDITOR MUST NOT MOVE THE PROCESS-WIDE PREVIEW-SCENE PROFILE.
//
// A NEW FILE rather than an assertion added to TestCapturePreviewSceneRig.cpp, which asserts the
// GUARD (PinWrightPreviewSceneRig::FScopedSharedProfiles restores what a scope moved). This asserts
// the WIRING: that PinWrightCaptureSubject::AcquireAssetEditorViewport actually wraps the open in
// it. Those are different failures - the guard can be perfect and unreferenced - and the second is
// the one that leaves `bShowFloor=false` in every later preview of the session and arms a diff on
// the committed Config/DefaultEditor.ini.
//
// HOW A TEST MAKES AN ENGINE-SIDE WRITE HAPPEN ON DEMAND. The write this file needs is not the
// Niagara floor write (that needs a Niagara editor); it is the OTHER one, which fires on ANY asset
// editor carrying an FAdvancedPreviewScene. The scene's constructor ends in `UpdateScene(Profile)`
// with all four update flags defaulted true (UE 5.8
// Editor/AdvancedPreviewScene/Private/AdvancedPreviewScene.cpp:151 for the environment block,
// AdvancedPreviewScene.h:39 for the defaults), and that block compares
//
//     GetLightDirection() != Profile.DirectionalLightRotation
//
// with an exact FRotator::operator!= (AdvancedPreviewScene.cpp:176-177). `GetLightDirection()` does
// not return a stored rotator: it is `DirectionalLight->GetComponentTransform().GetUnitAxis(EAxis::X)
// .Rotation()` (Runtime/Engine/Private/PreviewScene.cpp:264-272), and a rotator derived from a
// DIRECTION always has Roll == 0. So a profile carrying a non-zero ROLL can never compare equal,
// the branch runs, and the engine writes the component's rotation - roll 0 - into
// `DefaultSettings->Profiles[CurrentProfileIndex]` (AdvancedPreviewScene.cpp:188).
//
// This test arms exactly that: it writes Roll = 45 into every profile before the open. Roll about
// the light's own +X axis does not move the light DIRECTION at all (the direction is that axis), so
// the arming is photometrically inert - it changes nothing anyone could see in a frame, and it is
// unrecoverable from the direction, which is the whole point.
//
// UNABLE TO FAIL IF:
//   * the host cannot open the Static Mesh editor. Reported through the greppable
//     `PINWRIGHT_ASSERTIONS_SKIPPED:` marker Content/Python/check_suite_log.py refuses a
//     COMPLETED_CLEAN run on, never as a silent pass.
//   * the opened viewport does not carry an FAdvancedPreviewScene, in which case nothing writes the
//     profile and the exit comparison is trivially satisfied. Asserted directly, as a precondition,
//     through PinWrightPreviewSceneRig::IsAdvancedPreviewViewport.
//   * the editor for the fixture was ALREADY open, because then no scene is constructed during the
//     acquire. The test closes it first and refuses to continue if the close did not take.
// The residual hole this cannot close from outside: if `bDefaultLighting` were off for that
// viewport there would be no SkyLight and the environment block would be skipped. It is on -
// FPreviewScene::ConstructionValues defaults it true (PreviewScene.cpp:81-95) and
// SStaticMeshEditorViewport constructs its scene from a default-constructed ConstructionValues
// (SStaticMeshEditorViewport.cpp:143) - and that is a compile-visible engine fact, not a runtime
// one.
//
// EVERYTHING IT TOUCHES GOES BACK. The profile array is restored on every exit path including a
// failed assertion, and any asset editor this test opened is closed - an asset editor still open at
// editor exit faults in ~FStaticMeshEditor (UE 5.8 StaticMeshEditor.cpp:271, docs/lessons.md:166).
#include "Misc/AutomationTest.h"

#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"
#include "Handlers/Render/PreviewSceneRig.h"

#include "AssetViewerSettings.h"
#include "Editor.h"
#include "EditorViewportClient.h"
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
    const TCHAR* const PWSharedProfileCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Photometrically inert and unrecoverable from a direction vector: see the file header.
    constexpr double PWSharedProfileArmedRoll = 45.0;

    // Codes that mean "this host could not realise a preview viewport", as distinct from "the
    // acquire rejected the request". The same set TestCaptureSubjectMesh.cpp uses, and for the same
    // reason: anything OUTSIDE it is a real defect, so "the acquire silently did nothing" cannot
    // pass as a skip.
    bool PWSharedProfileIsHostLimitedCode(const FString& Code)
    {
        return Code == TEXT("PREVIEW_VIEWPORT_NOT_FOUND")
            || Code == TEXT("PREVIEW_NOT_FOUND")
            || Code == TEXT("OPEN_FAILED")
            || Code == TEXT("SUBSYSTEM_MISSING")
            || Code == TEXT("EDITOR_NOT_AVAILABLE")
            || Code == TEXT("UNSUPPORTED_ASSET_EDITOR");
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectSharedProfileSurvivesOpenTest,
    "PinWright.render.capture_subject.SharedProfileSurvivesAnAssetEditorOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectSharedProfileSurvivesOpenTest::RunTest(const FString& Parameters)
{
    UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
    if (!Settings || Settings->Profiles.Num() == 0)
    {
        // A hard error, not a skip: with no profile array there is no mechanism to measure, and
        // reporting that as a pass is the failure mode this whole file exists to avoid.
        AddError(TEXT("UAssetViewerSettings has no profiles, so the shared-profile restore around "
                      "an asset-editor open cannot be measured on this host."));
        return false;
    }
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
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, PWSharedProfileCubePath);
    if (!Cube)
    {
        AddError(FString::Printf(TEXT("The engine fixture mesh %s did not load."),
            PWSharedProfileCubePath));
        return false;
    }

    const TArray<FPreviewSceneProfile> AtEntry = Settings->Profiles;
    // Belt and braces, and it runs after every assertion below including a failing one: whatever
    // this test does to the process-wide array, the array goes back, and no window is left open.
    ON_SCOPE_EXIT
    {
        if (UAssetViewerSettings* SettingsOnExit = UAssetViewerSettings::Get())
        {
            SettingsOnExit->Profiles = AtEntry;
        }
        if (GEditor)
        {
            if (UAssetEditorSubsystem* SubsystemOnExit =
                    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                SubsystemOnExit->CloseAllEditorsForAsset(Cube);
            }
        }
    };

    // CLOSED FIRST, because the write is in the preview scene's CONSTRUCTOR and an editor that is
    // already open constructs nothing. Without this the acquire would be a no-op re-focus and the
    // exit comparison could not fail.
    Subsystem->CloseAllEditorsForAsset(Cube);
    if (Subsystem->FindEditorForAsset(Cube, /*bFocusIfOpen=*/false) != nullptr)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-refused-close"),
            TEXT("The Static Mesh editor for the fixture would not close, so the acquire below "
                 "cannot construct a fresh preview scene and nothing would write the profile."));
        return true;
    }

    // ---- arm the engine's own write-back ----
    for (FPreviewSceneProfile& Profile : Settings->Profiles)
    {
        Profile.DirectionalLightRotation.Roll = PWSharedProfileArmedRoll;
    }
    const TArray<FPreviewSceneProfile> Armed = Settings->Profiles;

    bool bAcquired = false;
    bool bAdvancedScene = false;
    FString ErrCode;
    FString ErrMsg;
    {
        // Scoped so the acquisition's TSharedPtr<FSceneViewport> and TSharedPtr<SEditorViewport>
        // are dropped before the close in ON_SCOPE_EXIT: destroying an asset editor's
        // SEditorViewport asserts check(SceneViewport.IsUnique()) (UE 5.8 SEditorViewport.cpp:65),
        // so a surviving reference aborts the process rather than leaking.
        PinWrightCaptureSubject::FAssetEditorViewportAcquisition Acquisition;
        bAcquired = PinWrightCaptureSubject::AcquireAssetEditorViewport(Cube,
            PinWrightCaptureSubjectMesh::StaticMeshToolkitNames(), Acquisition, ErrCode, ErrMsg);
        if (bAcquired && Acquisition.ViewportClient)
        {
            bAdvancedScene =
                PinWrightPreviewSceneRig::IsAdvancedPreviewViewport(*Acquisition.ViewportClient);
        }
    }

    if (!bAcquired && !PWSharedProfileIsHostLimitedCode(ErrCode))
    {
        AddError(FString::Printf(
            TEXT("AcquireAssetEditorViewport failed on a Static Mesh with an unexpected code "
                 "'%s': %s"), *ErrCode, *ErrMsg));
        return false;
    }
    if (!bAcquired)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-viewport"),
            FString::Printf(TEXT("AcquireAssetEditorViewport refused: %s -- %s"), *ErrCode, *ErrMsg));
        return true;
    }

    // THE PRECONDITION THAT MAKES THE ASSERTION BELOW ABLE TO FAIL. Without an FAdvancedPreviewScene
    // there is no UpdateScene, no write-back, and "the array is unchanged" is true whether or not
    // anything guards it.
    if (!TestTrue(TEXT("the opened Static Mesh viewport carries an FAdvancedPreviewScene, so the "
                       "open really can write the shared profile"), bAdvancedScene))
    {
        return false;
    }

    // COUNTERFACTUAL: delete the FScopedSharedProfiles local in
    // PinWrightCaptureSubject::AcquireAssetEditorViewport and this fails - the armed Roll comes
    // back 0, because the engine wrote the component's derived rotation over it during the open.
    TestTrue(TEXT("the whole shared profile array is field-wise equal to what it was before the "
                  "asset editor was opened"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, Armed));

    // And the armed field specifically, named, so a failure says WHICH profile moved rather than
    // only that something did.
    for (int32 Index = 0; Index < Settings->Profiles.Num(); ++Index)
    {
        TestTrue(FString::Printf(
                TEXT("profile %d kept its armed DirectionalLightRotation.Roll across the open"),
                Index),
            FMath::IsNearlyEqual(Settings->Profiles[Index].DirectionalLightRotation.Roll,
                PWSharedProfileArmedRoll, UE_KINDA_SMALL_NUMBER));
    }
    return true;
}
