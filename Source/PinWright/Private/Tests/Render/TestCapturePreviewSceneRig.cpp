// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the scoped `previewScene` capture rig: the vocabulary it accepts, the refusals it
// makes instead of half-applying, the arrival-azimuth conversion, and -- the part this whole file
// exists for -- the THREE-LEVEL restore.
//
// WHAT THIS DEFENDS. An asset-editor capture does not only touch its own viewport. The lighting it
// changes lives on a PROCESS-WIDE UObject (`UAssetViewerSettings`) shared by every open asset
// editor in the session, and that object is flushed to a COMMITTED, source-controlled config file
// (`Config/DefaultEditor.ini`) by the destructor of any "Preview Scene Settings" details tab, with
// no dirty check of any kind (UE 5.8 AssetViewerSettings.cpp:118-146,
// SAdvancedPreviewDetailsTab.cpp:46). Two engine paths already write into that object without being
// asked -- `SNiagaraSystemViewport::Construct` calls `SetFloorVisibility(false)` with `bDirect`
// defaulted false (SNiagaraSystemViewport.cpp:872), and `FAdvancedPreviewScene::UpdateScene` writes
// a scene's component light rotation back into the shared profile whenever the two differ
// (AdvancedPreviewScene.cpp:177-188). So a capture that changes the key light and puts back only
// the COMPONENT launders its override into the shared profile and, from there, into a working-tree
// diff on a tracked file.
//
// WHY ALMOST NONE OF IT NEEDS A GPU. A capture test that cannot get a GPU takes a conditional-skip
// path and reports success having asserted nothing -- board ticket
// B-test-skips-assertions-silently, where PinWright.render.capture_asset_preview.
// PinnedCapturesAreIdentical skipped its only substantive assertions in 3 of 3 runs and the suite
// totals looked identical either way. So the parser, the arrival conversion, the report block, the
// profile comparison and the config digest are all asserted as pure functions with nothing to skip,
// and the two tests that genuinely need a scene build their own and FAIL rather than skip when they
// cannot.
//
// THE FAILURE DIRECTION MATTERS. Asserting that a capture with `previewScene` succeeded proves
// nothing -- that is exactly how camera.orbit_shots shipped a `viewMode` argument that changed
// nothing and echoed itself back. Every test below asserts either that the rig REACHED the scene,
// that state came BACK, or that an unusable request was REFUSED, and each one that could pass
// vacuously carries an explicit precondition assertion that fails first if the fixture is inert.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestSkipReporting.h"

#include "AdvancedPreviewScene.h"
#include "AssetViewerSettings.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Dom/JsonObject.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PreviewScene.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // Prefixed for the same reason as every other helper under Tests/Render: anonymous namespaces
    // in one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR
    // clash.
    TSharedPtr<FJsonObject> RigPayloadFromJson(const FString& Json)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Root);
        return Root;
    }

    // Names the first divergence SharedProfilesMatch would report, so a failed whole-array
    // assertion says WHICH profile and WHICH property moved rather than only that something did.
    // Walks the same reflected struct SharedProfilesMatch walks, one property at a time.
    FString RigDescribeFirstProfileDifference(
        const TArray<FPreviewSceneProfile>& A, const TArray<FPreviewSceneProfile>& B)
    {
        if (A.Num() != B.Num())
        {
            return FString::Printf(TEXT("profile count %d vs %d"), A.Num(), B.Num());
        }
        UScriptStruct* Struct = FPreviewSceneProfile::StaticStruct();
        if (!Struct)
        {
            return TEXT("FPreviewSceneProfile::StaticStruct() is null");
        }
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            for (FProperty* Property = Struct->PropertyLink; Property;
                 Property = Property->PropertyLinkNext)
            {
                if (Property->Identical_InContainer(&A[Index], &B[Index], 0, PPF_None))
                {
                    continue;
                }
                FString AText;
                FString BText;
                Property->ExportText_InContainer(0, AText, &A[Index], &A[Index], nullptr, PPF_None);
                Property->ExportText_InContainer(0, BText, &B[Index], &B[Index], nullptr, PPF_None);
                if (AText == BText)
                {
                    // ExportText rounds; the compare does not. A sub-print-precision difference
                    // would otherwise read as "these identical values differ", so the raw bytes
                    // are appended when the printed text cannot tell them apart.
                    const uint8* ABytes = Property->ContainerPtrToValuePtr<uint8>(&A[Index]);
                    const uint8* BBytes = Property->ContainerPtrToValuePtr<uint8>(&B[Index]);
                    FString AHex;
                    FString BHex;
                    for (int32 Byte = 0; Byte < Property->GetSize(); ++Byte)
                    {
                        AHex += FString::Printf(TEXT("%02x"), ABytes[Byte]);
                        BHex += FString::Printf(TEXT("%02x"), BBytes[Byte]);
                    }
                    return FString::Printf(TEXT("profile[%d/%d].%s: bytes %s vs expected %s"),
                        Index, A.Num(), *Property->GetName(), *AHex, *BHex);
                }
                return FString::Printf(TEXT("profile[%d/%d].%s: '%s' vs expected '%s'"),
                    Index, A.Num(), *Property->GetName(), *AText, *BText);
            }
        }
        return TEXT("no per-property difference found");
    }

    // A bare FPreviewScene with an FEditorViewportClient over it -- everything
    // FScopedPreviewSceneRig needs for the key/sky half of the rig, and nothing else. NOT an
    // FAdvancedPreviewScene: this client has no SEditorViewport widget, so the allow-list in
    // PreviewSceneRig.cpp correctly declines to report it as an advanced scene, and the floor /
    // environment half is exercised separately against a scene rather than through a client.
    //
    // Build() returns false with a filled Failure rather than crashing, so a host that cannot
    // supply GEditor or Slate produces a RED TEST naming the reason instead of taking a silent
    // skip. The same file's tests never branch to "assume it passed".
    struct FRigPreviewClientFixture
    {
        TUniquePtr<FPreviewScene> Scene;
        TUniquePtr<FEditorViewportClient> Client;
        FString Failure;

        bool Build()
        {
            if (!GEditor)
            {
                Failure = TEXT("no GEditor -- FEditorViewportClient registers itself with the "
                               "editor's viewport client list in its constructor");
                return false;
            }
            if (!FSlateApplication::IsInitialized())
            {
                Failure = TEXT("Slate is not initialized -- FEditorViewportClient's constructor "
                               "binds OnWindowDPIScaleChanged unconditionally");
                return false;
            }
            Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
                .SetCreatePhysicsScene(false)
                .SetTransactional(false));
            if (!Scene->DirectionalLight || !Scene->SkyLight)
            {
                Failure = TEXT("the preview scene has no default lighting components");
                return false;
            }
            Client = MakeUnique<FEditorViewportClient>(nullptr, Scene.Get());
            return true;
        }

        ~FRigPreviewClientFixture()
        {
            // Client first: it holds a raw FPreviewScene* and unregisters itself from GEditor in
            // its own destructor.
            Client.Reset();
            Scene.Reset();
        }
    };

    // The engine assets FAdvancedPreviewScene's constructor loads with a bare `check()`
    // (AdvancedPreviewScene.cpp:65, :72). Pre-loaded so a host missing either produces a named
    // test failure instead of taking the whole suite down with an assertion.
    const TCHAR* const RigSkySpherePath =
        TEXT("/Engine/EditorMeshes/AssetViewer/Sphere_inversenormals.Sphere_inversenormals");
    const TCHAR* const RigSkyMaterialPath =
        TEXT("/Engine/EditorMaterials/AssetViewer/M_SkyBox.M_SkyBox");

    struct FRigAdvancedSceneFixture
    {
        TUniquePtr<FAdvancedPreviewScene> Scene;
        TUniquePtr<FEditorViewportClient> Client;
        FString Failure;

        bool Build()
        {
            if (!GEditor || !FSlateApplication::IsInitialized())
            {
                Failure = TEXT("no GEditor / Slate is not initialized");
                return false;
            }
            UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
            if (!Settings || Settings->Profiles.Num() == 0)
            {
                Failure = TEXT("UAssetViewerSettings has no profiles -- "
                               "FAdvancedPreviewScene's constructor check() would fire");
                return false;
            }
            if (!LoadObject<UStaticMesh>(nullptr, RigSkySpherePath) ||
                !LoadObject<UMaterial>(nullptr, RigSkyMaterialPath))
            {
                Failure = TEXT("the engine's asset-viewer sky sphere or sky material is missing -- "
                               "FAdvancedPreviewScene's constructor check() would fire");
                return false;
            }
            Scene = MakeUnique<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues()
                .SetCreatePhysicsScene(false)
                .SetTransactional(false));
            if (!Scene->DirectionalLight)
            {
                Failure = TEXT("the advanced preview scene has no directional light component");
                return false;
            }
            Client = MakeUnique<FEditorViewportClient>(nullptr, Scene.Get());
            return true;
        }

        FPreviewSceneProfile* Profile() const
        {
            return Scene.IsValid()
                ? PinWrightPreviewSceneRig::CurrentProfile(*Scene)
                : nullptr;
        }

        ~FRigAdvancedSceneFixture()
        {
            Client.Reset();
            Scene.Reset();
        }
    };
}

// ---------------------------------------------------------------------------------------------
// The arrival conversion
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigArrivalRoundTripTest,
    "PinWright.render.preview_scene_rig.ArrivalRoundTripsTheEngineDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigArrivalRoundTripTest::RunTest(const FString& Parameters)
{
    // READ OFF THE ENGINE AT ASSERT TIME, never typed in. Two functions that are consistent
    // inverses of each other pass an inverse-only test while both being wrong; the only thing that
    // pins them to reality is the engine's own shipped default, which is also the value
    // FPreviewSceneProfile's constructor uses (AssetViewerSettings.h:62) and the value every
    // measured lit-band observation in docs/wiki-src/visual-review.model-rig.md was taken under.
    const FRotator EngineDefault = FPreviewScene::ConstructionValues().LightRotation;

    double Azimuth = 0.0;
    double Elevation = 0.0;
    PinWrightPreviewSceneRig::LightRotationToArrival(EngineDefault, Azimuth, Elevation);

    // The derived pair, stated as a number so a change to either function shows up as a failing
    // comparison rather than as a quietly different report field. 112.5 / 40 is independently
    // corroborated by measurement at visual-review.model-rig.md:52 ("near azimuth 110").
    TestEqual(TEXT("the engine default arrives from azimuth 112.5"), Azimuth, 112.5, 1e-3);
    TestEqual(TEXT("the engine default arrives at elevation 40"), Elevation, 40.0, 1e-3);

    // ...and back to the engine's own literal, within 1e-3.
    const FRotator RoundTrip =
        PinWrightPreviewSceneRig::ArrivalToLightRotation(Azimuth, Elevation);
    TestTrue(FString::Printf(
            TEXT("ArrivalToLightRotation(%.4f, %.4f) reproduces the engine default %s (got %s)"),
            Azimuth, Elevation, *EngineDefault.ToString(), *RoundTrip.ToString()),
        RoundTrip.Equals(EngineDefault, 1e-3));

    // The forward direction, from the documented pair rather than from the measured one, so the
    // published convention itself is asserted: arrival azimuth 112.5 / elevation 40 IS
    // FRotator(-40, -67.5, 0).
    const FRotator FromDocumentedPair =
        PinWrightPreviewSceneRig::ArrivalToLightRotation(112.5, 40.0);
    TestTrue(TEXT("arrival (112.5, 40) is the engine's FRotator(-40, -67.5, 0)"),
        FromDocumentedPair.Equals(EngineDefault, 1e-3));

    // A second, unrelated point, so a conversion that happened to be right only at the default
    // cannot pass. Arrival from due north (azimuth 0) at the horizon travels due south.
    const FRotator Northerly = PinWrightPreviewSceneRig::ArrivalToLightRotation(0.0, 0.0);
    TestEqual(TEXT("arrival azimuth 0 gives yaw 180"), FMath::Abs(Northerly.Yaw), 180.0, 1e-6);
    TestEqual(TEXT("arrival elevation 0 gives pitch 0"), Northerly.Pitch, 0.0, 1e-6);

    double BackAzimuth = 0.0;
    double BackElevation = 0.0;
    PinWrightPreviewSceneRig::LightRotationToArrival(Northerly, BackAzimuth, BackElevation);
    TestEqual(TEXT("azimuth 0 comes back as 0, not 360"), BackAzimuth, 0.0, 1e-6);
    TestEqual(TEXT("elevation 0 comes back as 0"), BackElevation, 0.0, 1e-6);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Parsing and refusals
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigHalfAnAimIsRefusedTest,
    "PinWright.render.preview_scene_rig.HalfAnAimIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigHalfAnAimIsRefusedTest::RunTest(const FString& Parameters)
{
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
    FString ErrCode;
    FString ErrMsg;

    const bool bOk = PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
        RigPayloadFromJson(TEXT("{\"previewScene\":{\"key\":{\"azimuth\":110}}}")),
        Pin, ErrCode, ErrMsg);

    TestFalse(TEXT("azimuth without elevation is refused"), bOk);
    TestEqual(TEXT("the refusal is INVALID_ARGUMENT"), ErrCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    // BOTH substrings, not merely "it failed". A typo in the field name also fails, and a message
    // that named only the field the caller typed would leave them no way to learn what the pair
    // is.
    TestTrue(FString::Printf(TEXT("the message names azimuth: %s"), *ErrMsg),
        ErrMsg.Contains(TEXT("azimuth")));
    TestTrue(FString::Printf(TEXT("the message names elevation: %s"), *ErrMsg),
        ErrMsg.Contains(TEXT("elevation")));
    TestFalse(TEXT("nothing was written to the pin"), Pin.WantsRig());

    // The other half of the pair, so a check that only ever tested one ordering cannot pass.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin2;
    FString ErrCode2;
    FString ErrMsg2;
    TestFalse(TEXT("elevation without azimuth is refused"),
        PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
            RigPayloadFromJson(TEXT("{\"previewScene\":{\"key\":{\"elevation\":40}}}")),
            Pin2, ErrCode2, ErrMsg2));
    TestTrue(TEXT("the reverse message names azimuth"), ErrMsg2.Contains(TEXT("azimuth")));
    TestTrue(TEXT("the reverse message names elevation"), ErrMsg2.Contains(TEXT("elevation")));

    // And the contrast case: the complete pair is ACCEPTED, so this test cannot be satisfied by a
    // parser that refuses every `key` block.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin3;
    FString ErrCode3;
    FString ErrMsg3;
    TestTrue(TEXT("the complete pair is accepted"),
        PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
            RigPayloadFromJson(
                TEXT("{\"previewScene\":{\"key\":{\"azimuth\":110,\"elevation\":40}}}")),
            Pin3, ErrCode3, ErrMsg3));
    TestTrue(TEXT("the complete pair sets the aim"), Pin3.bKeyAimProvided);
    TestEqual(TEXT("the parsed azimuth"), Pin3.KeyAzimuthDegrees, 110.0, 1e-9);
    TestEqual(TEXT("the parsed elevation"), Pin3.KeyElevationDegrees, 40.0, 1e-9);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigEmptyBlockIsRefusedTest,
    "PinWright.render.preview_scene_rig.EmptyBlockIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigEmptyBlockIsRefusedTest::RunTest(const FString& Parameters)
{
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
    FString ErrCode;
    FString ErrMsg;

    // ASSERT THE ERROR, NOT THE NO-OP. A parser that read `{}` as "absent" would return true here
    // with WantsRig() false, and a test that only checked WantsRig() would pass on exactly the
    // behaviour this criterion forbids.
    const bool bOk = PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
        RigPayloadFromJson(TEXT("{\"previewScene\":{}}")), Pin, ErrCode, ErrMsg);
    TestFalse(TEXT("an empty previewScene object is refused"), bOk);
    TestEqual(TEXT("the refusal is INVALID_ARGUMENT"), ErrCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestFalse(TEXT("nothing was written to the pin"), Pin.WantsRig());

    // A block whose sub-objects are present but themselves empty asks for nothing either, and is
    // the shape a caller reaches by deleting fields one at a time.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin2;
    FString ErrCode2;
    FString ErrMsg2;
    TestFalse(TEXT("previewScene:{key:{},sky:{}} is refused too"),
        PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
            RigPayloadFromJson(TEXT("{\"previewScene\":{\"key\":{},\"sky\":{}}}")),
            Pin2, ErrCode2, ErrMsg2));
    TestEqual(TEXT("that refusal is INVALID_ARGUMENT too"), ErrCode2,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    // The contrast: an ABSENT previewScene is not an error and writes nothing.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin3;
    FString ErrCode3;
    FString ErrMsg3;
    TestTrue(TEXT("an absent previewScene parses cleanly"),
        PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
            RigPayloadFromJson(TEXT("{\"width\":512}")), Pin3, ErrCode3, ErrMsg3));
    TestFalse(TEXT("an absent previewScene asks for nothing"), Pin3.WantsRig());
    TestTrue(TEXT("an absent previewScene reports no error"), ErrCode3.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigMalformedColorIsRefusedTest,
    "PinWright.render.preview_scene_rig.MalformedColorIsRefusedNotBlackened",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigMalformedColorIsRefusedTest::RunTest(const FString& Parameters)
{
    // The engine's FColor::FromHex returns opaque BLACK for anything it cannot read, which is
    // indistinguishable from a caller who asked for black -- so an unreadable colour would light
    // the subject with no light at all and report success. Refused instead.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
    FString ErrCode;
    FString ErrMsg;
    TestFalse(TEXT("a non-hex colour is refused"),
        PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
            RigPayloadFromJson(TEXT("{\"previewScene\":{\"key\":{\"color\":\"warmwhite\"}}}")),
            Pin, ErrCode, ErrMsg));
    TestEqual(TEXT("the refusal is INVALID_ARGUMENT"), ErrCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    // The contrast case, which is what stops this passing on a parser that refuses every colour:
    // a well-formed value is accepted AND lands on the pin as the colour that was written.
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin2;
    FString ErrCode2;
    FString ErrMsg2;
    TestTrue(TEXT("#FF8800 is accepted"),
        PinWrightPreviewSceneRig::ParsePreviewSceneRigPin(
            RigPayloadFromJson(TEXT("{\"previewScene\":{\"key\":{\"color\":\"#FF8800\"}}}")),
            Pin2, ErrCode2, ErrMsg2));
    TestTrue(TEXT("the colour is marked provided"), Pin2.bKeyColorProvided);
    TestEqual(TEXT("red channel"), static_cast<int32>(Pin2.KeyColor.R), 255);
    TestEqual(TEXT("green channel"), static_cast<int32>(Pin2.KeyColor.G), 136);
    TestEqual(TEXT("blue channel"), static_cast<int32>(Pin2.KeyColor.B), 0);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The report block
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigLevelViewportReportsUnavailableTest,
    "PinWright.render.preview_scene_rig.LevelViewportReportsSceneUnavailable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigLevelViewportReportsUnavailableTest::RunTest(const FString& Parameters)
{
    // What a level-viewport capture produces: MeasureRig returns bSceneAvailable false because
    // FEditorViewportClient::GetPreviewScene() is null there, and every other field stays at its
    // default.
    PinWrightRenderCapture::FViewportCaptureOutput Capture;
    const TSharedPtr<FJsonObject> Block =
        PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Capture);
    if (!TestTrue(TEXT("the previewScene block is built"), Block.IsValid()))
    {
        return false;
    }

    bool bSceneAvailable = true;
    TestTrue(TEXT("sceneAvailable is present"),
        Block->TryGetBoolField(TEXT("sceneAvailable"), bSceneAvailable));
    TestFalse(TEXT("sceneAvailable is false"), bSceneAvailable);

    // ABSENCE, not an empty string. An empty `profileName` is indistinguishable from a profile
    // whose name is empty, and a reader cannot tell "there is no preview scene" from "the rig was
    // measured and the profile has no name".
    TestFalse(TEXT("profileName is ABSENT, not empty"), Block->HasField(TEXT("profileName")));
    TestFalse(TEXT("profileIndex is absent"), Block->HasField(TEXT("profileIndex")));
    TestFalse(TEXT("key is absent"), Block->HasField(TEXT("key")));
    TestFalse(TEXT("sky is absent"), Block->HasField(TEXT("sky")));
    TestFalse(TEXT("restore is absent"), Block->HasField(TEXT("restore")));
    TestFalse(TEXT("advancedScene is absent"), Block->HasField(TEXT("advancedScene")));

    // The block is still ATTACHED to `viewport` on this verb, per decision 8 -- a verb-by-verb
    // difference in the viewport block is a bug, not a design. Suppressing it here would make
    // "this verb has no preview scene" and "this response predates the parameter" the same
    // reading.
    const TSharedPtr<FJsonObject> Viewport =
        PinWrightRenderCapture::MakeViewportInfoObject(Capture);
    if (!TestTrue(TEXT("the viewport block is built"), Viewport.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("viewport.previewScene is present on a level-viewport capture"),
        Viewport->HasField(TEXT("previewScene")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigReportPublishesTheMeasuredRigTest,
    "PinWright.render.preview_scene_rig.ReportPublishesTheMeasuredRigNotTheRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigReportPublishesTheMeasuredRigTest::RunTest(const FString& Parameters)
{
    // A rig REQUESTED and NOT applied: the block must publish the rig the pixels were actually
    // drawn under and say so, rather than echoing the request back. This is the shape
    // camera.orbit_shots' `viewMode` shipped in for months without anyone being able to tell.
    PinWrightRenderCapture::FViewportCaptureOutput Capture;
    Capture.bPreviewSceneRigRequested = true;
    Capture.bPreviewSceneRigApplied = false;
    Capture.PreviewSceneRigDrawn.bSceneAvailable = true;
    Capture.PreviewSceneRigDrawn.bAdvancedScene = true;
    Capture.PreviewSceneRigDrawn.ProfileName = TEXT("Epic Headquarters");
    Capture.PreviewSceneRigDrawn.ProfileIndex = 0;
    Capture.PreviewSceneRigDrawn.KeyRotation = FRotator(-40.0, -67.5, 0.0);
    PinWrightPreviewSceneRig::LightRotationToArrival(Capture.PreviewSceneRigDrawn.KeyRotation,
        Capture.PreviewSceneRigDrawn.KeyAzimuthDegrees,
        Capture.PreviewSceneRigDrawn.KeyElevationDegrees);
    Capture.PreviewSceneRigDrawn.KeyIntensity = 3.14159;
    Capture.PreviewSceneRigDrawn.SkyIntensity = 1.0;
    Capture.PreviewSceneRigBefore = Capture.PreviewSceneRigDrawn;
    Capture.PreviewSceneRigAfter = Capture.PreviewSceneRigDrawn;

    const TSharedPtr<FJsonObject> Block =
        PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Capture);
    if (!TestTrue(TEXT("the previewScene block is built"), Block.IsValid()))
    {
        return false;
    }

    FString ProfileName;
    TestTrue(TEXT("profileName is published"),
        Block->TryGetStringField(TEXT("profileName"), ProfileName));
    TestEqual(TEXT("the profile the frame was drawn under"), ProfileName,
        FString(TEXT("Epic Headquarters")));

    const TSharedPtr<FJsonObject>* Key = nullptr;
    if (TestTrue(TEXT("key is published"), Block->TryGetObjectField(TEXT("key"), Key)) && Key)
    {
        double Azimuth = 0.0;
        double Elevation = 0.0;
        TestTrue(TEXT("key.azimuth is published"),
            (*Key)->TryGetNumberField(TEXT("azimuth"), Azimuth));
        TestTrue(TEXT("key.elevation is published"),
            (*Key)->TryGetNumberField(TEXT("elevation"), Elevation));
        // The derived arrival pair, so a caller never redoes the trig -- and so a change to the
        // conversion surfaces here as well as in the round-trip test.
        TestEqual(TEXT("the published arrival azimuth"), Azimuth, 112.5, 1e-3);
        TestEqual(TEXT("the published arrival elevation"), Elevation, 40.0, 1e-3);
        TestTrue(TEXT("the raw FRotator is published beside it"),
            (*Key)->HasField(TEXT("rotation")));
    }

    bool bApplied = true;
    TestTrue(TEXT("applied is published"), Block->TryGetBoolField(TEXT("applied"), bApplied));
    TestFalse(TEXT("applied reports the measurement, not the request"), bApplied);
    TestTrue(TEXT("a rig requested and not applied carries a rigWarning"),
        Block->HasField(TEXT("rigWarning")));

    // The counter-case: the same block with nothing requested must NOT carry the warning, or the
    // warning is noise and nobody reads it.
    PinWrightRenderCapture::FViewportCaptureOutput Quiet;
    Quiet.PreviewSceneRigDrawn = Capture.PreviewSceneRigDrawn;
    Quiet.PreviewSceneRigBefore = Capture.PreviewSceneRigDrawn;
    Quiet.PreviewSceneRigAfter = Capture.PreviewSceneRigDrawn;
    const TSharedPtr<FJsonObject> QuietBlock =
        PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Quiet);
    TestFalse(TEXT("no rigWarning when no rig was requested"),
        QuietBlock->HasField(TEXT("rigWarning")));
    // ...but the rig itself is still published, which is decision 8's whole point: the default rig
    // is what made two machines' captures differ at the same pinned exposure with nothing in the
    // response to show it.
    TestTrue(TEXT("the default rig is published anyway"), QuietBlock->HasField(TEXT("key")));
    // ...and the canonical triple, which is what a caller reads to know the rig above is the
    // editor's own rather than one this call installed.
    TestTrue(TEXT("requested is published anyway"), QuietBlock->HasField(TEXT("requested")));
    TestTrue(TEXT("applied is published anyway"), QuietBlock->HasField(TEXT("applied")));
    TestTrue(TEXT("restored is published anyway"), QuietBlock->HasField(TEXT("restored")));

    // ---- WHAT IS NOT PUBLISHED, AND WHY THAT IS THE FIX -------------------------------------
    //
    // This assertion was inverted on 2026-08-23. It used to require `restore` here, on the
    // strength of "the restore verdict is published anyway" -- but nothing was CHANGED on this
    // path, so `previous` and `afterRestore` were byte-for-byte the drawn rig already flattened
    // into the block, and `restore` announced the successful restoration of nothing. Three
    // restatements and an outcome that did not occur, at 1,114 characters of every capture
    // response, against a 10,000-character display budget the verb was already overrunning.
    //
    // The counter-case immediately below is what keeps this from becoming a hiding place: a
    // restore that FAILED publishes all three even with nothing requested.
    TestFalse(TEXT("previous is OMITTED when nothing was changed"),
        QuietBlock->HasField(TEXT("previous")));
    TestFalse(TEXT("afterRestore is OMITTED when nothing was changed"),
        QuietBlock->HasField(TEXT("afterRestore")));
    TestFalse(TEXT("restore is OMITTED when nothing was changed"),
        QuietBlock->HasField(TEXT("restore")));

    // A FAILED restore publishes the ledger whether or not the caller asked for a rig. This is the
    // direction that matters: the fields exist to show what was left behind, and suppressing them
    // exactly when something WAS left behind would be a far worse defect than the one above.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Damaged;
        Damaged.PreviewSceneRigDrawn = Capture.PreviewSceneRigDrawn;
        Damaged.PreviewSceneRigBefore = Capture.PreviewSceneRigDrawn;
        Damaged.PreviewSceneRigAfter = Capture.PreviewSceneRigDrawn;
        Damaged.bSharedProfilesRestored = false;
        const TSharedPtr<FJsonObject> DamagedBlock =
            PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Damaged);
        TestTrue(TEXT("a failed profile restore publishes previous"),
            DamagedBlock->HasField(TEXT("previous")));
        TestTrue(TEXT("a failed profile restore publishes afterRestore"),
            DamagedBlock->HasField(TEXT("afterRestore")));
        TestTrue(TEXT("a failed profile restore publishes the restore ledger"),
            DamagedBlock->HasField(TEXT("restore")));
        TestTrue(TEXT("a failed profile restore still carries its restoreWarning"),
            DamagedBlock->HasField(TEXT("restoreWarning")));
    }

    // A REQUESTED rig publishes the ledger too, cleanly restored or not: there the two sides are
    // genuinely different and the restore is the caller's proof that their pin was undone.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Requested;
        Requested.PreviewSceneRigDrawn = Capture.PreviewSceneRigDrawn;
        Requested.PreviewSceneRigBefore = Capture.PreviewSceneRigDrawn;
        Requested.PreviewSceneRigAfter = Capture.PreviewSceneRigDrawn;
        Requested.bPreviewSceneRigRequested = true;
        Requested.bPreviewSceneRigApplied = true;
        const TSharedPtr<FJsonObject> RequestedBlock =
            PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Requested);
        TestTrue(TEXT("a requested rig publishes previous"),
            RequestedBlock->HasField(TEXT("previous")));
        TestTrue(TEXT("a requested rig publishes afterRestore"),
            RequestedBlock->HasField(TEXT("afterRestore")));
        TestTrue(TEXT("a requested rig publishes the restore ledger"),
            RequestedBlock->HasField(TEXT("restore")));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The shared profile array
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigProfileEqualityIsFieldWiseTest,
    "PinWright.render.preview_scene_rig.ProfileEqualityIsFieldWiseNotByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigProfileEqualityIsFieldWiseTest::RunTest(const FString& Parameters)
{
    // THE TRAP THIS GUARDS. FPreviewSceneProfile::operator== compares ONLY ProfileName
    // (AssetViewerSettings.h:237-240), so `TArray<FPreviewSceneProfile>::operator!=` reports two
    // arrays EQUAL when every lighting value in them differs. A restore written against
    // `Profiles != ProfilesBefore` would therefore never fire, on any mutation this whole
    // mechanism exists to undo, and every restore field would report success.
    TArray<FPreviewSceneProfile> A;
    A.AddDefaulted(1);
    A[0].ProfileName = TEXT("Epic Headquarters");
    A[0].bShowFloor = true;
    A[0].DirectionalLightIntensity = 1.0f;
    A[0].DirectionalLightRotation = FRotator(-40.0, -67.5, 0.0);

    TArray<FPreviewSceneProfile> B = A;
    B[0].bShowFloor = false;

    // The engine's own comparison, shown failing to see the difference. Asserting this is what
    // makes the next assertion mean something: without it, a reader cannot tell whether
    // SharedProfilesMatch is doing real work or merely agreeing with operator==.
    //
    // FPreviewSceneProfile declares operator== only from UE 5.8 (AssetViewerSettings.h:237); on
    // older engines the struct has no equality at all, so there is nothing to demonstrate the
    // trap against. SharedProfilesMatch - the production code this test is actually about - is
    // asserted on every engine either way.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    TestTrue(TEXT("the engine's own operator== cannot see a changed bShowFloor"), A[0] == B[0]);
#endif
    TestFalse(TEXT("SharedProfilesMatch DOES see it"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(A, B));

    // A moved key rotation -- the value FAdvancedPreviewScene::UpdateScene writes back -- is seen
    // too.
    TArray<FPreviewSceneProfile> C = A;
    C[0].DirectionalLightRotation = FRotator(-10.0, 20.0, 0.0);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    TestTrue(TEXT("operator== cannot see a changed light rotation either"), A[0] == C[0]);
#endif
    TestFalse(TEXT("SharedProfilesMatch sees the changed light rotation"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(A, C));

    // And identical arrays still compare equal, so the check is not simply "always different".
    TArray<FPreviewSceneProfile> D = A;
    TestTrue(TEXT("identical arrays match"), PinWrightPreviewSceneRig::SharedProfilesMatch(A, D));
    // A different length is a difference, whatever the contents.
    D.AddDefaulted(1);
    TestFalse(TEXT("a different profile count does not match"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(A, D));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigProfileArrayIsRestoredTest,
    "PinWright.render.preview_scene_rig.ProfileArrayIsRestoredAfterAnEngineMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigProfileArrayIsRestoredTest::RunTest(const FString& Parameters)
{
    // COUNTERFACTUAL: delete the RestoreSharedProfiles call in FScopedSharedProfiles' destructor
    // and this test fails on the "byte-equal on exit" assertion -- and in production the mutation
    // below survives into every open asset editor in the session and, at the next Preview Scene
    // Settings tab teardown, into a diff on the tracked Config/DefaultEditor.ini.
    UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
    if (!Settings || Settings->Profiles.Num() == 0)
    {
        AddError(TEXT("UAssetViewerSettings has no profiles; the shared-profile restore cannot be "
                      "measured on this host."));
        return false;
    }

    const TArray<FPreviewSceneProfile> AtEntry = Settings->Profiles;
    // Belt and braces: whatever this test does, the process-wide array goes back. A failing
    // assertion must not leave the editor's shared lighting moved.
    ON_SCOPE_EXIT
    {
        if (UAssetViewerSettings* S = UAssetViewerSettings::Get())
        {
            S->Profiles = AtEntry;
        }
    };

    const int32 Index = 0;
    const bool bFloorAtEntry = Settings->Profiles[Index].bShowFloor;

    {
        PinWrightPreviewSceneRig::FScopedSharedProfiles Guard;

        // The mutation SNiagaraSystemViewport::Construct performs on every Niagara editor open
        // (SNiagaraSystemViewport.cpp:872 -> AdvancedPreviewScene.cpp:391-403), reproduced by
        // writing the same field. PostEditChangeProperty is deliberately NOT called: this test is
        // about the array's contents, and the broadcast would reach every live preview scene in
        // whatever editor is running the suite.
        Settings->Profiles[Index].bShowFloor = !bFloorAtEntry;

        // PRECONDITION, IN BOTH DIRECTIONS. A fixture that mutated nothing would satisfy the
        // exit assertion trivially, so the mutation is asserted to have taken effect INSIDE the
        // scope -- both as a field read and through the same comparison the restore uses.
        TestEqual(TEXT("the mutation took effect inside the scope"),
            Settings->Profiles[Index].bShowFloor, !bFloorAtEntry);
        TestFalse(TEXT("and the comparison the restore uses can see it"),
            PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));
    }

    TestTrue(TEXT("the whole profile array is field-wise equal to entry after the scope"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));
    TestEqual(TEXT("and the mutated field specifically is back"),
        Settings->Profiles[Index].bShowFloor, bFloorAtEntry);

    // The same property through the guard's own verdict, which is what the capture publishes as
    // `viewport.previewScene.restore.profilesRestored`.
    {
        PinWrightPreviewSceneRig::FScopedSharedProfiles Guard;
        Settings->Profiles[Index].bShowFloor = !bFloorAtEntry;
        TestFalse(TEXT("precondition: the array differs inside the second scope"),
            PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));
    }
    TestTrue(TEXT("the array is back after the second scope too"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));

    // A scope that changed NOTHING must report that it needed no restore -- otherwise
    // `profilesRestored` would be true for both cases and could not distinguish "nothing mutated"
    // from "something did and we undid it".
    {
        PinWrightPreviewSceneRig::FScopedSharedProfiles Quiet;
        // deliberately no mutation
    }
    TestTrue(TEXT("an unmutated scope leaves the array equal"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The committed config file
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigConfigDigestDetectsAChangeTest,
    "PinWright.render.preview_scene_rig.ConfigDigestDetectsAChangedByte",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigConfigDigestDetectsAChangeTest::RunTest(const FString& Parameters)
{
    // The discrimination half of the config criterion, on a file this test owns. Without it,
    // "the digest at exit equals the digest at entry" is satisfied by a digest function that
    // returns a constant -- or by one computed once and compared to itself.
    const FString ScratchPath = FPaths::Combine(FPaths::ProjectIntermediateDir(),
        TEXT("PinWright"), TEXT("r1_preview_scene_rig_digest_probe.ini"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ScratchPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
    };

    if (!FFileHelper::SaveStringToFile(TEXT("[Section]\nKey=A\n"), *ScratchPath))
    {
        AddError(FString::Printf(TEXT("could not write the digest probe file at %s"),
            *ScratchPath));
        return false;
    }
    const FString DigestA = PinWrightPreviewSceneRig::DigestFile(ScratchPath);
    TestFalse(TEXT("the digest of an existing file is non-empty"), DigestA.IsEmpty());
    TestEqual(TEXT("the digest is stable across two reads of the same bytes"),
        PinWrightPreviewSceneRig::DigestFile(ScratchPath), DigestA);

    if (!FFileHelper::SaveStringToFile(TEXT("[Section]\nKey=B\n"), *ScratchPath))
    {
        AddError(TEXT("could not rewrite the digest probe file"));
        return false;
    }
    const FString DigestB = PinWrightPreviewSceneRig::DigestFile(ScratchPath);
    TestFalse(TEXT("the digest of the changed file is non-empty"), DigestB.IsEmpty());
    TestNotEqual(TEXT("ONE changed byte changes the digest"), DigestB, DigestA);

    // A missing file digests to empty rather than to some sentinel that could collide with a real
    // digest.
    IFileManager::Get().Delete(*ScratchPath, false, true);
    TestTrue(TEXT("a missing file digests to empty"),
        PinWrightPreviewSceneRig::DigestFile(ScratchPath).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigConfigDigestUnchangedTest,
    "PinWright.render.preview_scene_rig.ConfigDigestIsUnchangedAcrossAGuardedMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigConfigDigestUnchangedTest::RunTest(const FString& Parameters)
{
    // COUNTERFACTUAL: remove the profile restore and this assertion still passes on its own --
    // because UAssetViewerSettings::Save() is called from asset-editor teardown, not from here.
    // That is exactly why the criterion is a DIGEST COMPARISON and not "we did not call Save()":
    // this plugin never calls it, so an assertion about the call would be untestable. What this
    // test proves is that the guarded window leaves the tracked file byte-identical, measured
    // against a digest that has been shown able to change (see ConfigDigestDetectsAChangedByte).
    const FString ConfigPath = PinWrightPreviewSceneRig::PreviewSceneConfigFilePath();
    TestFalse(TEXT("the config path is computed, not empty"), ConfigPath.IsEmpty());
    TestTrue(FString::Printf(TEXT("the computed path is the shared-profile config file: %s"),
            *ConfigPath),
        ConfigPath.EndsWith(TEXT("DefaultEditor.ini")));

    if (!FPaths::FileExists(ConfigPath))
    {
        AddError(FString::Printf(
            TEXT("%s does not exist, so the config-restore criterion cannot be measured. That "
                 "file is where USharedProfiles (UCLASS(config = Editor, defaultconfig)) is "
                 "flushed, and in this project it is tracked in git."),
            *ConfigPath));
        return false;
    }

    const FString DigestBefore = PinWrightPreviewSceneRig::DigestFile(ConfigPath);
    if (!TestFalse(TEXT("the digest at entry is non-empty"), DigestBefore.IsEmpty()))
    {
        return false;
    }

    UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
    if (!Settings || Settings->Profiles.Num() == 0)
    {
        AddError(TEXT("UAssetViewerSettings has no profiles."));
        return false;
    }
    const TArray<FPreviewSceneProfile> AtEntry = Settings->Profiles;
    ON_SCOPE_EXIT
    {
        if (UAssetViewerSettings* S = UAssetViewerSettings::Get())
        {
            S->Profiles = AtEntry;
        }
    };

    {
        PinWrightPreviewSceneRig::FScopedSharedProfiles Guard;
        Settings->Profiles[0].DirectionalLightRotation = FRotator(-12.5, 33.25, 0.0);
        Settings->Profiles[0].bShowFloor = !Settings->Profiles[0].bShowFloor;
        // PRECONDITION: the mutation is real and visible to the comparison the restore uses.
        TestFalse(TEXT("precondition: the profile array differs inside the scope"),
            PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));
    }

    const FString DigestAfter = PinWrightPreviewSceneRig::DigestFile(ConfigPath);
    TestFalse(TEXT("the digest at exit is non-empty"), DigestAfter.IsEmpty());
    TestEqual(TEXT("the committed Config/DefaultEditor.ini is byte-identical across the scope"),
        DigestAfter, DigestBefore);
    TestTrue(TEXT("and the shared profile array came back"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, AtEntry));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The guard, against a real preview scene
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigOmittedParameterWritesNothingTest,
    "PinWright.render.preview_scene_rig.OmittedParameterWritesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigOmittedParameterWritesNothingTest::RunTest(const FString& Parameters)
{
    FRigPreviewClientFixture Fixture;
    if (!Fixture.Build())
    {
        // A FAILURE, not a skip. A test that quietly reports success when it could not build its
        // fixture is the exact defect board ticket B-test-skips-assertions-silently records.
        AddError(FString::Printf(TEXT("could not build the preview fixture: %s"),
            *Fixture.Failure));
        return false;
    }

    UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
    if (!Settings || Settings->Profiles.Num() == 0)
    {
        AddError(TEXT("UAssetViewerSettings has no profiles."));
        return false;
    }

    const FRotator RotationBefore = Fixture.Scene->GetLightDirection();
    const float IntensityBefore = Fixture.Scene->DirectionalLight->Intensity;
    const FColor ColorBefore = Fixture.Scene->DirectionalLight->LightColor;
    const float SkyBefore = Fixture.Scene->SkyLight->Intensity;
    const TArray<FPreviewSceneProfile> ProfilesBefore = Settings->Profiles;

    PinWrightPreviewSceneRig::FPreviewSceneRigReport Previous;
    bool bApplied = true;
    {
        // Default-constructed pin: bRequested false. This is the omitted-parameter path.
        const PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig Guard(*Fixture.Client, Pin);
        bApplied = Guard.WasApplied();
        Previous = Guard.GetPreviousRig();

        // Measured INSIDE the scope: the omitted path must not write even while it is open, or
        // the pixels of a capture that asked for nothing would already be different.
        TestTrue(TEXT("the light rotation is untouched inside the scope"),
            Fixture.Scene->GetLightDirection().Equals(RotationBefore, 1e-4));
        TestEqual(TEXT("the light intensity is untouched inside the scope"),
            Fixture.Scene->DirectionalLight->Intensity, IntensityBefore);
    }

    TestFalse(TEXT("applied is false on the omitted-parameter path"), bApplied);

    // `previous` is still POPULATED -- invariant 1. Without it a caller cannot tell "no rig was
    // asked for" from "a rig was asked for and did nothing", which are opposite facts about the
    // same frame.
    TestTrue(TEXT("previous still reports the scene as available"), Previous.bSceneAvailable);
    TestTrue(TEXT("previous carries the measured key rotation"),
        Previous.KeyRotation.Equals(RotationBefore, 1e-4));
    TestEqual(TEXT("previous carries the measured key intensity"), Previous.KeyIntensity,
        static_cast<double>(IntensityBefore), 1e-4);
    TestEqual(TEXT("previous carries the measured sky intensity"), Previous.SkyIntensity,
        static_cast<double>(SkyBefore), 1e-4);

    // Every component value, and the WHOLE profile array. Asserting only `applied == false` would
    // pass on a guard that wrote and then restored -- a different thing, and one that would move
    // the pixels of any capture racing it.
    TestTrue(TEXT("the light rotation is bit-identical after the scope"),
        Fixture.Scene->GetLightDirection().Equals(RotationBefore, 1e-4));
    TestEqual(TEXT("the light intensity is identical after the scope"),
        Fixture.Scene->DirectionalLight->Intensity, IntensityBefore);
    TestTrue(TEXT("the light colour is identical after the scope"),
        Fixture.Scene->DirectionalLight->LightColor == ColorBefore);
    TestEqual(TEXT("the sky intensity is identical after the scope"),
        Fixture.Scene->SkyLight->Intensity, SkyBefore);
    TestTrue(TEXT("the entire shared profile array is field-wise identical after the scope"),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, ProfilesBefore));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigKeyAimAppliesAndRestoresTest,
    "PinWright.render.preview_scene_rig.KeyAimAppliesAndRestoresOnTheComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigKeyAimAppliesAndRestoresTest::RunTest(const FString& Parameters)
{
    // The positive control for OmittedParameterWritesNothing. Without this, a guard that wrote
    // nothing at all -- ever -- would pass every restore test in this file.
    FRigPreviewClientFixture Fixture;
    if (!Fixture.Build())
    {
        AddError(FString::Printf(TEXT("could not build the preview fixture: %s"),
            *Fixture.Failure));
        return false;
    }

    const FRotator RotationBefore = Fixture.Scene->GetLightDirection();
    const float IntensityBefore = Fixture.Scene->DirectionalLight->Intensity;
    const float SkyBefore = Fixture.Scene->SkyLight->Intensity;

    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
    Pin.bRequested = true;
    Pin.bKeyAimProvided = true;
    Pin.KeyAzimuthDegrees = 20.0;
    Pin.KeyElevationDegrees = 55.0;
    Pin.bKeyIntensityProvided = true;
    Pin.KeyIntensity = 4.0;
    Pin.bSkyIntensityProvided = true;
    Pin.SkyIntensity = 2.0;

    const FRotator Wanted = PinWrightPreviewSceneRig::ArrivalToLightRotation(20.0, 55.0);
    // PRECONDITION: the requested aim is not where the light already is, or "it applied" and "it
    // was never touched" would be the same observation.
    TestFalse(TEXT("precondition: the requested aim differs from the scene's current aim"),
        Wanted.Equals(RotationBefore, 0.5));
    TestFalse(TEXT("precondition: the requested intensity differs"),
        FMath::IsNearlyEqual(static_cast<double>(IntensityBefore), 4.0, 0.01));

    {
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig Guard(*Fixture.Client, Pin);
        TestTrue(TEXT("the guard reports it applied"), Guard.WasApplied());

        // MEASURED off the scene, not read back off the guard. A guard that assigns a bool in its
        // own constructor proves only that the constructor ran.
        TestTrue(FString::Printf(TEXT("the key light points where the request asked (%s, got %s)"),
                *Wanted.ToString(), *Fixture.Scene->GetLightDirection().ToString()),
            Fixture.Scene->GetLightDirection().Equals(Wanted, 0.01));
        TestEqual(TEXT("the key intensity is what the request asked"),
            Fixture.Scene->DirectionalLight->Intensity, 4.0f, 1e-4f);
        TestEqual(TEXT("the sky intensity is what the request asked"),
            Fixture.Scene->SkyLight->Intensity, 2.0f, 1e-4f);

        // The measured report the response publishes, through the same path a capture uses.
        const PinWrightPreviewSceneRig::FPreviewSceneRigReport Drawn =
            PinWrightPreviewSceneRig::MeasureRig(*Fixture.Client);
        TestEqual(TEXT("the reported arrival azimuth is what was asked"), Drawn.KeyAzimuthDegrees,
            20.0, 0.01);
        TestEqual(TEXT("the reported arrival elevation is what was asked"),
            Drawn.KeyElevationDegrees, 55.0, 0.01);
    }

    TestTrue(TEXT("the key rotation is restored"),
        Fixture.Scene->GetLightDirection().Equals(RotationBefore, 1e-3));
    TestEqual(TEXT("the key intensity is restored"),
        Fixture.Scene->DirectionalLight->Intensity, IntensityBefore, 1e-4f);
    TestEqual(TEXT("the sky intensity is restored"),
        Fixture.Scene->SkyLight->Intensity, SkyBefore, 1e-4f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigDrivesTheSkyCaptureUpdateTest,
    "PinWright.render.preview_scene_rig.RigApplyDrivesTheSkyCaptureUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigDrivesTheSkyCaptureUpdateTest::RunTest(const FString& Parameters)
{
    // WHAT THIS DEFENDS. `applied: true` says the rig's WRITES landed. It never said the pixels
    // were lit by them, because the sky-light and reflection captures those writes feed are
    // drained only by FPreviewScene::UpdateCaptureContents, which the engine calls from three
    // Ticks and nowhere else (PreviewScene.cpp:245-253) -- and a capture pumps Slate and draws
    // without ever running an editor frame. SetCaptureIsDirty only appends to a static queue
    // (SkyLightComponent.cpp:354-368); nothing in a capture used to empty it.
    //
    // The warm-up settle loop cannot substitute for this assertion: its pump is the capture's
    // own, so it cannot advance the work it would have to observe, and a frame lit by a stale
    // capture converges immediately and legitimately.
    //
    // COUNTERFACTUAL: delete the UpdatePreviewSceneCaptures call from FScopedPreviewSceneRig's
    // constructor and the queued capture is still queued when the scope is measured --
    // HasSkyCapturesToUpdate stays true and CaptureContentsUpdated() reports false.

    // ---- the wire half, which needs no scene and can never be skipped ----
    {
        PinWrightRenderCapture::FViewportCaptureOutput Stale;
        Stale.PreviewSceneRigDrawn.bSceneAvailable = true;
        Stale.bPreviewSceneCaptureUpdated = false;
        const TSharedPtr<FJsonObject> StaleBlock =
            PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Stale);
        bool bReported = true;
        TestTrue(TEXT("captureUpdated is published"),
            StaleBlock->TryGetBoolField(TEXT("captureUpdated"), bReported));
        TestFalse(TEXT("captureUpdated reports the drain that did not run"), bReported);
        // Gated on the DRAIN, not on the warm-up: this output carries no warm-up measurement at
        // all, so a warning that only fired on `!settled` would say nothing here.
        TestTrue(TEXT("a skipped capture update carries its own warning"),
            StaleBlock->HasField(TEXT("skyCaptureWarning")));

        PinWrightRenderCapture::FViewportCaptureOutput Fresh;
        Fresh.PreviewSceneRigDrawn.bSceneAvailable = true;
        Fresh.bPreviewSceneCaptureUpdated = true;
        const TSharedPtr<FJsonObject> FreshBlock =
            PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Fresh);
        bReported = false;
        TestTrue(TEXT("captureUpdated is published on the healthy path too"),
            FreshBlock->TryGetBoolField(TEXT("captureUpdated"), bReported));
        TestTrue(TEXT("captureUpdated reports the drain that ran"), bReported);
        // The counter-case, or the warning is noise and nobody reads it.
        TestFalse(TEXT("no skyCaptureWarning when the update ran"),
            FreshBlock->HasField(TEXT("skyCaptureWarning")));
        TestFalse(TEXT("captureIncomplete is OMITTED when the drain came back clean"),
            FreshBlock->HasField(TEXT("captureIncomplete")));

        // DRIVEN IS NOT COMPLETED. The engine re-queues a capture it could not finish while
        // assets compile and will not retry it for 5 s, so this pair is the ordinary state of the
        // first capture after an asset load -- and it is reported separately rather than folded
        // into captureUpdated, whose engine predicate is process-wide.
        PinWrightRenderCapture::FViewportCaptureOutput Deferred;
        Deferred.PreviewSceneRigDrawn.bSceneAvailable = true;
        Deferred.bPreviewSceneCaptureUpdated = true;
        Deferred.bPreviewSceneCaptureIncomplete = true;
        const TSharedPtr<FJsonObject> DeferredBlock =
            PinWrightRenderCapture::MakePreviewSceneRigInfoObject(Deferred);
        bReported = false;
        TestTrue(TEXT("captureUpdated still reports the drain that ran"),
            DeferredBlock->TryGetBoolField(TEXT("captureUpdated"), bReported) && bReported);
        TestTrue(TEXT("captureIncomplete is published when a capture was left queued"),
            DeferredBlock->HasField(TEXT("captureIncomplete")));
        TestTrue(TEXT("and it carries its own warning"),
            DeferredBlock->HasField(TEXT("captureIncompleteWarning")));
        // NOT the stale-sky warning: the drain ran, so that one's premise is false here and two
        // warnings describing different mechanisms would leave a caller unable to act on either.
        TestFalse(TEXT("a deferred capture does not also raise skyCaptureWarning"),
            DeferredBlock->HasField(TEXT("skyCaptureWarning")));

        // And the reverse pairing: with no drain there is nothing for `captureIncomplete` to have
        // measured, so it must not appear beside the stale-sky warning as if it had.
        TestFalse(TEXT("captureIncomplete is OMITTED when the drain never ran"),
            StaleBlock->HasField(TEXT("captureIncomplete")));
    }

    // ---- the production half: the guard really drains the queue ----
    FRigPreviewClientFixture Fixture;
    if (!Fixture.Build())
    {
        AddError(FString::Printf(TEXT("could not build the preview fixture: %s"),
            *Fixture.Failure));
        return false;
    }

    UWorld* World = Fixture.Scene->GetWorld();
    if (!World || !World->Scene)
    {
        // Both engine entry points test exactly this, so there is no drain to observe and
        // `captureUpdated: false` would be the CORRECT answer -- there is nothing to assert.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preview-world-has-no-renderer-scene"),
            TEXT("The fixture's preview world carries no FSceneInterface, so "
                 "USkyLightComponent::UpdateSkyCaptureContents early-outs by design."));
        return true;
    }

    // The ENGINE's drain, called directly, so the precondition below does not depend on the code
    // under test. Constructing an FPreviewScene queues its own sky light
    // (USkyLightComponent::PostInitProperties, SkyLightComponent.cpp:451-460), so without this the
    // queue is already non-empty for a reason unrelated to the assertion.
    Fixture.Scene->UpdateCaptureContents();
    if (PinWrightPreviewSceneRig::HasSkyCapturesToUpdate())
    {
        // SkyCapturesToUpdate is process-wide and the engine skips entries belonging to another
        // world, so a sky light this test cannot drain would make the assertion below unreadable.
        // Same for r.SkylightUpdateEveryFrame, which makes the predicate permanently true.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("sky-capture-queue-not-isolable"),
            TEXT("The process-wide sky-capture queue is non-empty after draining this fixture's "
                 "own world, so it cannot isolate this guard's drain."));
        return true;
    }

    Fixture.Scene->SkyLight->SetCaptureIsDirty();
    // PRECONDITION, and a real one: SetCaptureIsDirty silently does nothing for a hidden, a
    // world-detached or a static sky light (SkyLightComponent.cpp:356), and a fixture in that
    // state would make the assertion below pass with the fix reverted.
    if (!TestTrue(TEXT("precondition: the fixture's sky capture is queued"),
            PinWrightPreviewSceneRig::HasSkyCapturesToUpdate()))
    {
        return false;
    }

    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
    Pin.bRequested = true;
    Pin.bSkyIntensityProvided = true;
    Pin.SkyIntensity = 2.0;

    bool bCaptureUpdated = false;
    bool bCaptureIncomplete = true;
    {
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig Guard(*Fixture.Client, Pin);
        bCaptureUpdated = Guard.WasApplied() && Guard.CaptureContentsUpdated();
        bCaptureIncomplete = Guard.CaptureContentsIncomplete();
        // MEASURED INSIDE THE SCOPE, which is where every frame of a capture is drawn.
        TestFalse(TEXT("the rig-apply path drained the queued sky capture"),
            PinWrightPreviewSceneRig::HasSkyCapturesToUpdate());
    }

    TestTrue(TEXT("the guard reports that it drove the capture update"), bCaptureUpdated);
    // The guard's own reading of the post-drain queue, so the field the response publishes is
    // exercised rather than only the static the assertion above reads directly.
    TestFalse(TEXT("and reports nothing left queued after it"), bCaptureIncomplete);

    // The engine's queue is PROCESS-WIDE and this test does not own it. A sky light dirtied
    // anywhere in the editor between the two scopes -- the guard's own destructor can broadcast
    // into every live FAdvancedPreviewScene on the profile-restore path -- would make the second
    // half a false failure rather than a real one, so isolability is re-established here instead
    // of being assumed to survive from the check above.
    if (PinWrightPreviewSceneRig::HasSkyCapturesToUpdate())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("sky-capture-queue-not-isolable"),
            TEXT("A sky capture was queued elsewhere in the editor between the two scopes, so "
                 "the process-wide queue can no longer isolate the omitted-parameter drain. The "
                 "requested-rig half above already ran its assertions."));
        return true;
    }

    // The omitted-parameter path drains too. A capture that requested no rig is lit by the same
    // never-drained queue, and a 240-still set taken with no `previewScene` write is where that
    // was measured -- so "nothing was requested" must not report `captureUpdated: false`.
    Fixture.Scene->SkyLight->SetCaptureIsDirty();
    if (!TestTrue(TEXT("precondition: the sky capture is queued again"),
            PinWrightPreviewSceneRig::HasSkyCapturesToUpdate()))
    {
        return false;
    }
    {
        const PinWrightPreviewSceneRig::FPreviewSceneRigPin NoRig;
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig Guard(*Fixture.Client, NoRig);
        TestFalse(TEXT("the omitted-parameter path still applies nothing"), Guard.WasApplied());
        TestTrue(TEXT("but it still drove the capture update"),
            Guard.CaptureContentsUpdated());
        TestFalse(TEXT("and the queued sky capture is drained there too"),
            PinWrightPreviewSceneRig::HasSkyCapturesToUpdate());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigRestoreOrderPreventsWritebackTest,
    "PinWright.render.preview_scene_rig.RestoreOrderPreventsTheProfileWriteback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigRestoreOrderPreventsWritebackTest::RunTest(const FString& Parameters)
{
    // THE TEST THIS WHOLE CHUNK TURNS ON. FAdvancedPreviewScene::UpdateScene compares
    // GetLightDirection() against Profile.DirectionalLightRotation and, when they differ, writes
    // the COMPONENT's rotation into the SHARED PROFILE (AdvancedPreviewScene.cpp:177-188). So a
    // component-only override launders itself into process-wide state and, from there, into the
    // committed Config/DefaultEditor.ini. Restoring the component BEFORE anything drives
    // UpdateScene makes the engine's own bLightDirChanged false, so the block never runs.
    //
    // COUNTERFACTUAL: move the component restore in FScopedPreviewSceneRig's destructor to AFTER
    // the profile restore and the second half of this test fails -- the profile comes back
    // carrying the rig's rotation instead of its own.
    //
    // WHY UpdateScene IS CALLED DIRECTLY AND NOT VIA OnAssetViewerSettingsChanged().Broadcast().
    // The broadcast reaches EVERY live FAdvancedPreviewScene in whatever editor is running the
    // suite, including windows this test does not own. AdvancedPreviewScene.cpp:630 shows what the
    // broadcast does for a NAME_None property: UpdateScene(Profile, true, true, true, true) -- so
    // calling that directly on a scene this test built exercises the identical code path with the
    // identical flags, and the "did the broadcast actually reach UpdateScene" clause is satisfied
    // by construction rather than by hope.
    UAssetViewerSettings* SettingsForRestore = UAssetViewerSettings::Get();
    if (!SettingsForRestore || SettingsForRestore->Profiles.Num() == 0)
    {
        AddError(TEXT("UAssetViewerSettings has no profiles."));
        return false;
    }
    // Snapshotted BEFORE the fixture is built, not after. FAdvancedPreviewScene's own constructor
    // ends in UpdateScene(Profile) (AdvancedPreviewScene.cpp:121), so merely CONSTRUCTING one can
    // already write a light rotation into the shared profile if the component's derived rotator is
    // not bit-equal to the profile's stored one -- the exact quirk this test is about. Restoring
    // to the pre-construction value leaves the editor as this test found it.
    const TArray<FPreviewSceneProfile> ProfilesBeforeFixture = SettingsForRestore->Profiles;
    ON_SCOPE_EXIT
    {
        if (UAssetViewerSettings* S = UAssetViewerSettings::Get())
        {
            S->Profiles = ProfilesBeforeFixture;
        }
    };

    FRigAdvancedSceneFixture Fixture;
    if (!Fixture.Build())
    {
        AddError(FString::Printf(TEXT("could not build the advanced preview fixture: %s"),
            *Fixture.Failure));
        return false;
    }
    FPreviewSceneProfile* Profile = Fixture.Profile();
    if (!Profile)
    {
        AddError(TEXT("the advanced preview scene has no current profile."));
        return false;
    }

    // SETTLE THE FIXTURE BEFORE SNAPSHOTTING, because a freshly built scene is not necessarily a
    // FIXPOINT of the engine's own rotator round-trip. The constructor pushes the profile's stored
    // rotation onto the light component but never pushes the component's DERIVED rotation back;
    // FPreviewScene::GetLightDirection rebuilds the rotator from the component transform's X axis
    // (PreviewScene.cpp:239-247), and on UE 5.4 the AdvancedPreviewScene default (-40, -67.5, 0)
    // comes back as (-40.000000000000007, -67.49999999999999, 0). UpdateScene's write-back gate is
    // `GetLightDirection() != Profile.DirectionalLightRotation` -- FRotator::operator!=, which is
    // exact, not tolerant -- so the FIRST UpdateScene after construction writes that round-tripped
    // value into the shared profile whatever this test does, and a later one writes nothing more
    // (the round-trip is idempotent from its first application). Running it here means the
    // field-wise assertions below measure THE GUARD's restore rather than that one-off engine
    // drift. On an engine where construction already left a fixpoint this call changes nothing.
    Fixture.Scene->UpdateScene(*Profile, true, true, true, true);

    UAssetViewerSettings* Settings = SettingsForRestore;
    // Taken AFTER construction, because that is the state the guard under test will snapshot.
    const TArray<FPreviewSceneProfile> ProfilesAtEntry = Settings->Profiles;

    // ---- the negative control: the write-back mechanism is LIVE on this engine ----
    // Without this the second half proves nothing -- a build in which UpdateScene never wrote back
    // would pass it trivially.
    const FRotator ProfileRotationAtEntry = Profile->DirectionalLightRotation;
    const FRotator ComponentRotationAtEntry = Fixture.Scene->GetLightDirection();
    const FRotator Elsewhere(-15.0, 42.0, 0.0);

    Fixture.Scene->SetLightDirection(Elsewhere);
    // PRECONDITION for bLightDirChanged: the component and the profile must actually differ, or
    // the engine's block is skipped for a reason that has nothing to do with the restore order.
    TestFalse(TEXT("precondition: the component rotation now differs from the profile's"),
        Fixture.Scene->GetLightDirection().Equals(Profile->DirectionalLightRotation, 0.01));

    Fixture.Scene->UpdateScene(*Profile, true, true, true, true);
    TestTrue(FString::Printf(
            TEXT("the engine DOES launder a component-only aim into the shared profile "
                 "(profile now %s, was %s)"),
            *Profile->DirectionalLightRotation.ToString(), *ProfileRotationAtEntry.ToString()),
        Profile->DirectionalLightRotation.Equals(Elsewhere, 0.01));

    // Put the fixture back by hand before the real half.
    Profile->DirectionalLightRotation = ProfileRotationAtEntry;
    Fixture.Scene->SetLightDirection(ComponentRotationAtEntry);

    // ---- the real half: the guard's restore order defeats it ----
    PinWrightPreviewSceneRig::FPreviewSceneRigPin Pin;
    Pin.bRequested = true;
    Pin.bKeyAimProvided = true;
    Pin.KeyAzimuthDegrees = 250.0;
    Pin.KeyElevationDegrees = 12.0;
    const FRotator Wanted = PinWrightPreviewSceneRig::ArrivalToLightRotation(250.0, 12.0);

    {
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig Guard(*Fixture.Client, Pin);
        TestTrue(TEXT("the guard applied the aim"), Guard.WasApplied());
        // PRECONDITION, INSIDE the scope: while the rig is applied the component and the profile
        // DO differ, so the write-back is armed and the restore has something to defeat.
        TestTrue(TEXT("the component carries the requested aim inside the scope"),
            Fixture.Scene->GetLightDirection().Equals(Wanted, 0.01));
        TestFalse(TEXT("precondition: the write-back is armed inside the scope"),
            Fixture.Scene->GetLightDirection().Equals(Profile->DirectionalLightRotation, 0.01));
    }

    // Now drive the engine's write-back path, exactly as a broadcast, a tick or the next details
    // tab would.
    FPreviewSceneProfile* ProfileAfter = Fixture.Profile();
    if (!ProfileAfter)
    {
        AddError(TEXT("the advanced preview scene lost its profile."));
        return false;
    }
    Fixture.Scene->UpdateScene(*ProfileAfter, true, true, true, true);

    TestTrue(FString::Printf(
            TEXT("the shared profile's light rotation is STILL the original %s (got %s)"),
            *ProfileRotationAtEntry.ToString(), *ProfileAfter->DirectionalLightRotation.ToString()),
        ProfileAfter->DirectionalLightRotation.Equals(ProfileRotationAtEntry, 0.01));
    TestFalse(TEXT("and it is NOT the rig's rotation"),
        ProfileAfter->DirectionalLightRotation.Equals(Wanted, 0.01));
    TestTrue(*FString::Printf(
            TEXT("the whole shared profile array is field-wise equal to entry (%s)"),
            *RigDescribeFirstProfileDifference(Settings->Profiles, ProfilesAtEntry)),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, ProfilesAtEntry));

    // ---- the third case: laundering that happens WHILE the rig is applied ----
    //
    // The plan's acceptance criterion asked for the broadcast to be raised INSIDE the scope and
    // for the profile to be unchanged there. It cannot be: inside the scope the component
    // deliberately carries the override, so bLightDirChanged is TRUE by construction and
    // UpdateScene writes it into the shared profile -- a correct implementation FAILS that
    // assertion. The restore ORDER protects the broadcast that happens at or after scope exit,
    // which is where the guard's own conditional broadcast, the next tick and the next details-tab
    // teardown all land. What protects the in-scope case is the other half of the mechanism: the
    // whole-array SNAPSHOT. Both halves are asserted here, so neither can rot unnoticed.
    {
        PinWrightPreviewSceneRig::FScopedPreviewSceneRig Guard(*Fixture.Client, Pin);
        FPreviewSceneProfile* Live = Fixture.Profile();
        if (!Live)
        {
            AddError(TEXT("the advanced preview scene lost its profile inside the scope."));
            return false;
        }
        Fixture.Scene->UpdateScene(*Live, true, true, true, true);
        // The hazard is REAL and is asserted as real, not assumed: while the rig is applied, an
        // UpdateScene does launder the override into process-wide state.
        TestTrue(TEXT("precondition: an in-scope UpdateScene DOES launder the rig into the profile"),
            Live->DirectionalLightRotation.Equals(Wanted, 0.01));
    }

    FPreviewSceneProfile* ProfileAfterLaundering = Fixture.Profile();
    if (!ProfileAfterLaundering)
    {
        AddError(TEXT("the advanced preview scene lost its profile after the third scope."));
        return false;
    }
    // COUNTERFACTUAL: delete the profile snapshot/restore from FScopedPreviewSceneRig and this is
    // the assertion that fails -- and in production the laundered rotation then rides into the
    // tracked Config/DefaultEditor.ini at the next Preview Scene Settings tab teardown.
    TestTrue(TEXT("the snapshot undoes the in-scope laundering on exit"),
        ProfileAfterLaundering->DirectionalLightRotation.Equals(ProfileRotationAtEntry, 0.01));
    TestTrue(*FString::Printf(
            TEXT("and the whole array is field-wise equal to entry again (%s)"),
            *RigDescribeFirstProfileDifference(Settings->Profiles, ProfilesAtEntry)),
        PinWrightPreviewSceneRig::SharedProfilesMatch(Settings->Profiles, ProfilesAtEntry));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The pose-list forward
// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigPoseListForwardTest,
    "PinWright.render.preview_scene_rig.PoseListForwardsTheRigToEveryFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigPoseListForwardTest::RunTest(const FString& Parameters)
{
    // WHAT THIS CATCHES. render.capture_asset_preview and render.capture_animation_preview both go
    // through the pose-list primitive; render.capture_annotated does not. If MakeFrameRequest
    // carried the field on FPoseListCaptureRequest but did not COPY it onto the per-frame
    // FViewportCaptureRequest, the rig would work on one verb and silently do nothing on the other
    // two -- the call succeeds, the response reports a rig was requested, and the pixels are lit
    // by whatever was already there. A test that only asserted the field exists on the request
    // passes with the forward deleted, so this drives the primitive and reads the FRAME.
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.Width = 64;
    Request.Height = 64;
    Request.FilenamePrefix = TEXT("r1_rig_forward");
    Request.bWarmupShot = true;
    Request.PreviewSceneRig.bRequested = true;
    Request.PreviewSceneRig.bKeyAimProvided = true;
    Request.PreviewSceneRig.KeyAzimuthDegrees = 137.0;
    Request.PreviewSceneRig.KeyElevationDegrees = 21.0;
    Request.PreviewSceneRig.bSkyIntensityProvided = true;
    Request.PreviewSceneRig.SkyIntensity = 2.5;

    for (int32 Index = 0; Index < 3; ++Index)
    {
        PinWrightPoseCapture::FCameraPose Pose;
        Pose.Location = FVector(100.0 * Index, 0.0, 0.0);
        Pose.Rotation = FRotator::ZeroRotator;
        Pose.Filename = FString::Printf(TEXT("r1_rig_forward_%d.png"), Index);
        Request.Poses.Add(Pose);
    }

    TArray<PinWrightRenderCapture::FViewportCaptureRequest> Frames;
    const PinWrightPoseCapture::FPoseFrameCapturer Capturer =
        [&Frames](const PinWrightRenderCapture::FViewportCaptureRequest& Frame,
            bool bWarmupFrame,
            PinWrightRenderCapture::FViewportCaptureOutput& OutCapture,
            FString& OutErrorCode,
            FString& OutErrorMessage) -> bool
    {
        Frames.Add(Frame);
        OutCapture.Filename = Frame.Filename;
        OutCapture.Width = Frame.Width;
        OutCapture.Height = Frame.Height;
        OutCapture.Path = bWarmupFrame
            ? FString()
            : FString::Printf(TEXT("/stub/%s"), *Frame.Filename);
        OutCapture.EffectiveLocation = Frame.Location;
        OutCapture.EffectiveRotation = Frame.Rotation;
        return true;
    };

    PinWrightPoseCapture::FPoseListCaptureOutput Result;
    FString ErrCode;
    FString ErrMsg;
    if (!TestTrue(TEXT("the pose set captured"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Capturer, Result, ErrCode, ErrMsg)))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"), *ErrCode, *ErrMsg));
        return false;
    }

    // PRECONDITION: frames were actually drawn, so "every frame carried the rig" is not vacuous
    // over an empty list. The warm-up shot counts -- it draws into the same viewport and would be
    // lit differently if the rig were dropped on it.
    if (!TestTrue(TEXT("precondition: at least four frames were drawn (3 shots + warm-up)"),
            Frames.Num() >= 4))
    {
        return false;
    }

    for (int32 Index = 0; Index < Frames.Num(); ++Index)
    {
        const PinWrightRenderCapture::FViewportCaptureRequest& Frame = Frames[Index];
        TestTrue(FString::Printf(TEXT("frame %d carries the rig request"), Index),
            Frame.PreviewSceneRig.WantsRig());
        TestTrue(FString::Printf(TEXT("frame %d carries the aim flag"), Index),
            Frame.PreviewSceneRig.bKeyAimProvided);
        TestEqual(FString::Printf(TEXT("frame %d carries the azimuth"), Index),
            Frame.PreviewSceneRig.KeyAzimuthDegrees, 137.0, 1e-9);
        TestEqual(FString::Printf(TEXT("frame %d carries the elevation"), Index),
            Frame.PreviewSceneRig.KeyElevationDegrees, 21.0, 1e-9);
        TestEqual(FString::Printf(TEXT("frame %d carries the sky intensity"), Index),
            Frame.PreviewSceneRig.SkyIntensity, 2.5, 1e-9);
    }

    // The contrast case: a set with NO rig must forward no rig, or the assertion above would be
    // satisfied by a primitive that hardcoded one.
    PinWrightPoseCapture::FPoseListCaptureRequest Quiet = Request;
    Quiet.PreviewSceneRig = PinWrightPreviewSceneRig::FPreviewSceneRigPin();
    Frames.Reset();
    PinWrightPoseCapture::FPoseListCaptureOutput QuietResult;
    FString QuietErrCode;
    FString QuietErrMsg;
    TestTrue(TEXT("the unrigged set captured"), PinWrightPoseCapture::RunPoseListCapture(
        Quiet, Capturer, QuietResult, QuietErrCode, QuietErrMsg));
    TestTrue(TEXT("precondition: the unrigged set drew frames too"), Frames.Num() >= 4);
    for (int32 Index = 0; Index < Frames.Num(); ++Index)
    {
        TestFalse(FString::Printf(TEXT("unrigged frame %d asks for nothing"), Index),
            Frames[Index].PreviewSceneRig.WantsRig());
    }
    return true;
}
