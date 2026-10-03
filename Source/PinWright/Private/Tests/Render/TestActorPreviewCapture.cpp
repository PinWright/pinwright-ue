// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_actor_preview: an actor CLASS spawned transient into a private world, and an
// EXISTING actor drawn in its own (non-active) world, both framed to bounds without touching the
// user's level.

#include "Misc/AutomationTest.h"

#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Info.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/ActorPreviewCapture.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "PreviewScene.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/StrongObjectPtr.h"

namespace TestActorPreviewCaptureHelpers
{
    const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    bool SkipWithoutGpu(FAutomationTestBase& Test)
    {
        if (PinWrightTestSkip::SkipIfRenderingUnavailable(Test))
        {
            return true;
        }
        if (!FApp::CanEverRender() || !GDynamicRHI)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("no-gpu"),
                TEXT("This host has no rendering device, so scene-capture readback cannot run."));
            return true;
        }
        return false;
    }

    // An Actor Blueprint whose only primitive is a cube, so the spawned instance has bounds that
    // exist ONLY once it is constructed - the class itself has nothing to frame.
    UBlueprint* CreateCubeBlueprint(FAutomationTestBase& Test)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
        UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("ActorPreviewCubeBP"));
        if (!Test.TestNotNull(TEXT("engine cube loads"), Cube)
            || !Test.TestNotNull(TEXT("fixture Blueprint created"), BP)
            || !Test.TestNotNull(TEXT("fixture Blueprint has an SCS"), BP->SimpleConstructionScript.Get()))
        {
            return nullptr;
        }
        USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(
            UStaticMeshComponent::StaticClass(), TEXT("CubeMesh"));
        Cast<UStaticMeshComponent>(Node->ComponentTemplate)->SetStaticMesh(Cube);
        BP->SimpleConstructionScript->AddNode(Node);
        FKismetEditorUtilities::CompileBlueprint(BP);
        if (!Test.TestTrue(TEXT("fixture Blueprint compiles"),
                BP->Status != BS_Error && BP->GeneratedClass != nullptr))
        {
            return nullptr;
        }
        return BP;
    }

    int32 CountInstances(UWorld* World, UClass* Class)
    {
        int32 Count = 0;
        for (TActorIterator<AActor> It(World, Class); It; ++It)
        {
            Count += IsValid(*It) ? 1 : 0;
        }
        return Count;
    }

    AStaticMeshActor* SpawnCube(UWorld* World, const FVector& Location)
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transient;
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator, Params);
        if (Actor)
        {
            Actor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, CubePath));
        }
        return Actor;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewSpawnedClassTest,
    "PinWright.render.capture_actor_preview.SpawnedClassRendersAndIsDestroyed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewSpawnedClassTest::RunTest(const FString& Parameters)
{
    using namespace TestActorPreviewCaptureHelpers;
    if (SkipWithoutGpu(*this))
    {
        return true;
    }
    UBlueprint* BP = CreateCubeBlueprint(*this);
    if (!BP)
    {
        return false;
    }
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world"), EditorWorld))
    {
        return false;
    }
    const bool bLevelDirtyBefore = EditorWorld->GetOutermost()->IsDirty();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("classPath"), BP->GeneratedClass->GetPathName());
    Payload->SetNumberField(TEXT("width"), 128);
    Payload->SetNumberField(TEXT("height"), 96);
    Payload->SetNumberField(TEXT("exposure"), 0);
    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(TEXT("render.capture_actor_preview"), Payload, Capture));
    if (!TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess && Capture.Result.IsValid()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>& Result = Capture.Result;
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*Result->GetStringField(TEXT("path")), false, true, true);
    };

    TestEqual(TEXT("exact width"), static_cast<int32>(Result->GetNumberField(TEXT("width"))), 128);
    TestEqual(TEXT("exact height"), static_cast<int32>(Result->GetNumberField(TEXT("height"))), 96);
    TestTrue(TEXT("PNG written"), IFileManager::Get().FileExists(*Result->GetStringField(TEXT("path"))));
    TestTrue(TEXT("spawnedTransient"), Result->GetBoolField(TEXT("spawnedTransient")));
    TestTrue(TEXT("transientDestroyed is measured true"), Result->GetBoolField(TEXT("transientDestroyed")));
    TestEqual(TEXT("drawn in the private preview world"),
        Result->GetObjectField(TEXT("actor"))->GetStringField(TEXT("world")), FString(TEXT("preview")));
    TestFalse(TEXT("the frame is not blank"), Result->GetBoolField(TEXT("blank")));
    double Coverage = -1.0;
    TestTrue(TEXT("subjectCoverage is published"), Result->TryGetNumberField(TEXT("subjectCoverage"), Coverage));
    TestTrue(*FString::Printf(TEXT("the constructed cube occupies part of the frame (%f)"), Coverage),
        Coverage > 0.02 && Coverage < 1.0);
    TestTrue(TEXT("bounds come from the constructed instance"),
        Result->GetObjectField(TEXT("bounds"))->GetNumberField(TEXT("radius")) > 1.0);

    TestEqual(TEXT("nothing of the class reached the user's level"),
        CountInstances(EditorWorld, BP->GeneratedClass), 0);
    TestEqual(TEXT("the level's dirty flag is unchanged"),
        EditorWorld->GetOutermost()->IsDirty(), bLevelDirtyBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewExistingActorTest,
    "PinWright.render.capture_actor_preview.ExistingActorDrawnShowOnlyAndUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewExistingActorTest::RunTest(const FString& Parameters)
{
    using namespace TestActorPreviewCaptureHelpers;
    if (SkipWithoutGpu(*this))
    {
        return true;
    }

    // A world that is NOT the active level, standing in for a runtime/PIE world the user's map
    // does not contain. Its key light arrives from +X, the side the azimuth-0 camera below looks
    // at: under the default rig (arrival azimuth 112.5) every face that camera sees is in full
    // shadow, black on the black show-only background, so both cubes were drawn and still
    // measured 0 coverage.
    FPreviewScene::ConstructionValues Values;
    Values.SetCreatePhysicsScene(false).ShouldSimulatePhysics(false).SetTransactional(false)
        .SetLightRotation(FRotator(-40.0f, 180.0f, 0.0f));
    FPreviewScene Scene(Values);
    UWorld* World = Scene.GetWorld();
    AStaticMeshActor* Subject = SpawnCube(World, FVector::ZeroVector);
    // Beside and behind the subject, with its lit +X face inside the framed view (the camera sits
    // on +X): a renderer that drew the whole scene would put this neighbour into the frame. Not
    // level with the subject - there only its unlit -Y side would face the camera.
    AStaticMeshActor* Neighbour = SpawnCube(World, FVector(-80.0, 110.0, 0.0));
    if (!TestNotNull(TEXT("subject spawned"), Subject) || !TestNotNull(TEXT("neighbour spawned"), Neighbour))
    {
        return false;
    }
    const FTransform SubjectBefore = Subject->GetActorTransform();

    PinWrightActorPreviewCapture::FActorCaptureRequest Request;
    Request.Actor = Subject;
    Request.Capture.Width = 128;
    Request.Capture.Height = 128;
    Request.Capture.Exposure.Mode = PinWrightRenderCapture::EExposureRequestMode::Fixed;
    Request.Capture.Exposure.Ev100 = 0.0f;
    Request.Capture.bRetainPixels = true;
    Request.bWriteFile = false;
    Request.OrbitAzimuth = 0.0f;
    Request.OrbitElevation = 0.0f;
    Request.Padding = 1.0f;

    PinWrightActorPreviewCapture::FActorCaptureOutput WithNeighbour;
    FString Code;
    FString Message;
    if (!TestTrue(*FString::Printf(TEXT("capture with neighbour: %s %s"), *Code, *Message),
            PinWrightActorPreviewCapture::CaptureActorPreview(Request, WithNeighbour, Code, Message)))
    {
        return false;
    }
    TestFalse(TEXT("an existing actor is not reported as spawned"), WithNeighbour.bSpawnedTransient);
    TestEqual(TEXT("the actor's own world is named"), WithNeighbour.World, FString(TEXT("preview")));
    TestTrue(TEXT("the subject is still alive"), IsValid(Subject));
    TestTrue(TEXT("the subject did not move"), Subject->GetActorTransform().Equals(SubjectBefore));
    TestTrue(TEXT("the subject is still visible"),
        Subject->GetStaticMeshComponent()->IsVisible() && !Subject->IsHidden());
    TestTrue(TEXT("the subject covers part of the frame"),
        WithNeighbour.SubjectCoverage.IsSet() && WithNeighbour.SubjectCoverage.GetValue() > 0.02);

    // Control: the neighbour really is inside the frame - draw IT with the same camera.
    PinWrightActorPreviewCapture::FActorCaptureRequest NeighbourRequest = Request;
    NeighbourRequest.Actor = Neighbour;
    NeighbourRequest.bUseOrbitPose = false;
    NeighbourRequest.Capture.Location = WithNeighbour.Capture.EffectiveLocation;
    NeighbourRequest.Capture.Rotation = WithNeighbour.Capture.EffectiveRotation;
    PinWrightActorPreviewCapture::FActorCaptureOutput NeighbourOnly;
    if (!TestTrue(TEXT("neighbour control capture"),
            PinWrightActorPreviewCapture::CaptureActorPreview(NeighbourRequest, NeighbourOnly, Code, Message)))
    {
        return false;
    }
    if (!TestTrue(TEXT("precondition: the neighbour is visible from the subject's camera"),
            NeighbourOnly.SubjectCoverage.IsSet() && NeighbourOnly.SubjectCoverage.GetValue() > 0.02))
    {
        return false;
    }

    // Remove the neighbour and draw the subject again with the identical pose. Show-only means
    // the neighbour never contributed a pixel, so the two subject frames agree.
    World->EditorDestroyActor(Neighbour, false);
    PinWrightActorPreviewCapture::FActorCaptureOutput Alone;
    if (!TestTrue(TEXT("capture alone"),
            PinWrightActorPreviewCapture::CaptureActorPreview(Request, Alone, Code, Message)))
    {
        return false;
    }
    const PinWrightFlatRegion::FFrameDifferenceStats Difference =
        PinWrightFlatRegion::MeasureFrameDifference(Alone.Capture.Pixels, WithNeighbour.Capture.Pixels, 8);
    TestTrue(TEXT("frames are comparable"), Difference.bMeasured);
    TestTrue(*FString::Printf(
            TEXT("the neighbour did not reach the subject's frame (changed %f, neighbour alone covers %f)"),
            Difference.ChangedPixelFraction, NeighbourOnly.SubjectCoverage.GetValue()),
        Difference.ChangedPixelFraction < 0.005);
    World->EditorDestroyActor(Subject, false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewChildActorTest,
    "PinWright.render.capture_actor_preview.ChildActorComponentPartsAreDrawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewChildActorTest::RunTest(const FString& Parameters)
{
    using namespace TestActorPreviewCaptureHelpers;
    if (SkipWithoutGpu(*this))
    {
        return true;
    }
    // A Blueprint whose ONLY component is a Child Actor Component holding the cube Blueprint: every
    // primitive in the frame belongs to a separate (child) actor.
    UBlueprint* CubeBP = CreateCubeBlueprint(*this);
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("ActorPreviewChildActorBP"));
    if (!CubeBP || !TestNotNull(TEXT("child-actor Blueprint created"), BP)
        || !TestNotNull(TEXT("child-actor Blueprint has an SCS"), BP->SimpleConstructionScript.Get()))
    {
        return false;
    }
    USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(
        UChildActorComponent::StaticClass(), TEXT("CubeChild"));
    Cast<UChildActorComponent>(Node->ComponentTemplate)->SetChildActorClass(
        TSubclassOf<AActor>(CubeBP->GeneratedClass.Get()));
    BP->SimpleConstructionScript->AddNode(Node);
    FKismetEditorUtilities::CompileBlueprint(BP);
    if (!TestTrue(TEXT("child-actor Blueprint compiles"),
            BP->Status != BS_Error && BP->GeneratedClass != nullptr))
    {
        return false;
    }

    PinWrightActorPreviewCapture::FActorCaptureRequest Request;
    Request.ActorClass = BP->GeneratedClass;
    Request.Capture.Width = 128;
    Request.Capture.Height = 128;
    Request.Capture.Exposure.Mode = PinWrightRenderCapture::EExposureRequestMode::Fixed;
    Request.Capture.Exposure.Ev100 = 0.0f;
    Request.bWriteFile = false;
    PinWrightActorPreviewCapture::FActorCaptureOutput Output;
    FString Code;
    FString Message;
    // BOUNDS_EMPTY here means the child actor was never constructed - a fixture problem, not the
    // show-only defect this test is about.
    if (!TestTrue(*FString::Printf(TEXT("precondition: the child actor is framed (%s %s)"), *Code, *Message),
            PinWrightActorPreviewCapture::CaptureActorPreview(Request, Output, Code, Message)))
    {
        return false;
    }
    TestTrue(TEXT("the instance and its child were destroyed"), Output.bTransientDestroyed);
    TestFalse(TEXT("the child actor's cube is drawn (frame not blank)"), Output.Capture.ImageStats.bBlank);
    TestTrue(*FString::Printf(TEXT("the child actor's cube covers part of the frame (%f)"),
            Output.SubjectCoverage.Get(-1.0)),
        Output.SubjectCoverage.IsSet() && Output.SubjectCoverage.GetValue() > 0.02);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewEditorActorTest,
    "PinWright.render.capture_actor_preview.EditorActorByPathReportsWorldAndViewport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewEditorActorTest::RunTest(const FString& Parameters)
{
    using namespace TestActorPreviewCaptureHelpers;
    if (SkipWithoutGpu(*this))
    {
        return true;
    }
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world"), EditorWorld))
    {
        return false;
    }
    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("PW_ActorPreview_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Cube = SpawnTransientCubeActor(EditorWorld, Label, FVector(0.0, 0.0, 5000.0));
    if (!TestNotNull(TEXT("editor-world cube spawned"), Cube))
    {
        return false;
    }
    // Clean before the call, so any dirtying the capture does is visible.
    UPackage* LevelPackage = EditorWorld->PersistentLevel->GetOutermost();
    LevelPackage->SetDirtyFlag(false);
    const FTransform CubeBefore = Cube->GetActorTransform();

    // By object path: the fixture is RF_Transient, and the resolver's editor-world label/name tier
    // enumerates UEditorActorSubsystem::GetAllLevelActors, which skips transient actors
    // (E-actor-not-found-hides-transient-label-match). The object-path tier resolves it.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), Cube->GetPathName());
    Payload->SetNumberField(TEXT("width"), 96);
    Payload->SetNumberField(TEXT("height"), 96);
    Payload->SetStringField(TEXT("viewMode"), TEXT("unlit"));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("render.capture_actor_preview"), Payload, Capture);
    if (!TestTrue(*FString::Printf(TEXT("capture succeeds: %s %s"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess && Capture.Result.IsValid()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>& Result = Capture.Result;
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*Result->GetStringField(TEXT("path")), false, true, true);
    };

    const TSharedPtr<FJsonObject> ActorObj = Result->GetObjectField(TEXT("actor"));
    TestEqual(TEXT("drawn in the editor world"), ActorObj->GetStringField(TEXT("world")), FString(TEXT("editor")));
    TestEqual(TEXT("the named actor was drawn"), ActorObj->GetStringField(TEXT("label")), Label);
    TestFalse(TEXT("an existing actor is not spawned"), Result->GetBoolField(TEXT("spawnedTransient")));
    TestFalse(TEXT("transientDestroyed is spawned-only"), Result->HasField(TEXT("transientDestroyed")));
    double Coverage = -1.0;
    Result->TryGetNumberField(TEXT("subjectCoverage"), Coverage);
    TestTrue(*FString::Printf(TEXT("the cube covers part of the frame (%f)"), Coverage), Coverage > 0.02);
    TestFalse(TEXT("the level's dirty flag is unchanged"), LevelPackage->IsDirty());
    TestTrue(TEXT("the cube did not move"), Cube->GetActorTransform().Equals(CubeBefore));

    // The view mode is reported, not just applied.
    const TSharedPtr<FJsonObject>* Viewport = nullptr;
    if (!TestTrue(TEXT("viewport block published"), Result->TryGetObjectField(TEXT("viewport"), Viewport)))
    {
        return false;
    }
    TestEqual(TEXT("viewport type names the actor's own world"),
        (*Viewport)->GetStringField(TEXT("type")), FString(TEXT("ActorWorld")));
    const TSharedPtr<FJsonObject> Override = (*Viewport)->GetObjectField(TEXT("viewModeOverride"));
    TestTrue(TEXT("viewModeOverride.requested"), Override->GetBoolField(TEXT("requested")));
    TestTrue(TEXT("viewModeOverride.applied"), Override->GetBoolField(TEXT("applied")));
    TestFalse(TEXT("viewport reports an unlit frame"), (*Viewport)->GetBoolField(TEXT("lit")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewPreviewSceneRefusedTest,
    "PinWright.render.capture_actor_preview.PreviewSceneRefusedWithReason",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewPreviewSceneRefusedTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    // Declared (the shared parser reads it) and refused by the verb itself, so the caller gets a
    // reason instead of the dispatcher's bare UNKNOWN_PARAMS, and a rig is never silently ignored.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
    TSharedPtr<FJsonObject> Rig = MakeShared<FJsonObject>();
    Rig->SetBoolField(TEXT("showFloor"), true);
    Payload->SetObjectField(TEXT("previewScene"), Rig);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("render.capture_actor_preview"), Payload, Capture);
    TestFalse(TEXT("previewScene is refused"), Capture.bSuccess);
    TestEqual(TEXT("previewScene -> INVALID_ARGUMENT"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestTrue(TEXT("the refusal names previewScene"), Capture.Message.Contains(TEXT("previewScene")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewEmptyBoundsTest,
    "PinWright.render.capture_actor_preview.EmptyBoundsRefusedAndInstanceDestroyed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewEmptyBoundsTest::RunTest(const FString& Parameters)
{
    // No pixels are read on this path, so it runs on every host: the refusal fires on bounds,
    // before the probe exists, and must still tear the transient instance down.
    PinWrightActorPreviewCapture::FActorCaptureRequest Request;
    Request.ActorClass = AActor::StaticClass();
    Request.bWriteFile = false;
    PinWrightActorPreviewCapture::FActorCaptureOutput Output;
    FString Code;
    FString Message;
    TestFalse(TEXT("a primitive-less actor is refused"),
        PinWrightActorPreviewCapture::CaptureActorPreview(Request, Output, Code, Message));
    TestEqual(TEXT("refusal code"), Code, FString(ErrorCodes::ERR_BOUNDS_EMPTY));
    TestTrue(TEXT("the instance was spawned"), Output.bSpawnedTransient);
    TestTrue(TEXT("and destroyed on the error path"), Output.bTransientDestroyed);

    Request.ActorClass = AInfo::StaticClass();
    TestFalse(TEXT("an abstract class is refused"),
        PinWrightActorPreviewCapture::CaptureActorPreview(Request, Output, Code, Message));
    TestEqual(TEXT("abstract refusal code"), Code, FString(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE));
    TestFalse(TEXT("nothing was spawned for it"), Output.bSpawnedTransient);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorPreviewSourceRefusalTest,
    "PinWright.render.capture_actor_preview.ExactlyOneSourceRequired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorPreviewSourceRefusalTest::RunTest(const FString& Parameters)
{
    if (PinWrightTestSkip::SkipIfRenderingUnavailable(*this))
    {
        return true;
    }
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Neither = MakeShared<FJsonObject>();
    InvokeHandlerWithCapture(TEXT("render.capture_actor_preview"), Neither, Capture);
    TestFalse(TEXT("neither source is refused"), Capture.bSuccess);
    TestEqual(TEXT("neither -> INVALID_ARGUMENT"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    TSharedPtr<FJsonObject> Both = MakeShared<FJsonObject>();
    Both->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
    Both->SetStringField(TEXT("actorPath"), TEXT("SomeActor"));
    InvokeHandlerWithCapture(TEXT("render.capture_actor_preview"), Both, Capture);
    TestEqual(TEXT("both -> INVALID_ARGUMENT"), Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
    Missing->SetStringField(TEXT("actorPath"), TEXT("PinWrightNoSuchActor_ActorPreview"));
    InvokeHandlerWithCapture(TEXT("render.capture_actor_preview"), Missing, Capture);
    TestEqual(TEXT("unknown actor -> ACTOR_NOT_FOUND"), Capture.ErrorCode, FString(ErrorCodes::ERR_ACTOR_NOT_FOUND));
    return true;
}
