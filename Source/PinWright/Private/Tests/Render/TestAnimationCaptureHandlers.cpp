// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for the animated-capture verbs:
//   camera.animation_shots            — frame burst of a PLACED animated mesh, via a Level Sequence
//   render.capture_animation_preview  — frame burst of a skinned asset IN ISOLATION, via Persona
//
// Every test here drives the PRODUCTION registered handler through the real registration list
// (InvokeHandlerWithCapture), never a helper. That is not a style preference: a shipped opacity
// test in this codebase kept passing after its production call site was deleted, because it
// exercised the helper directly. A pose-comparison helper test would fail the same way — it
// would stay green if the handler stopped calling it, which is precisely the regression that
// matters, so the pose evidence is asserted through the handler's RESPONSE and nowhere else.
//
// What each group pins, and why removing one stops this file being evidence:
//
//  * Argument rejections (no world, no RHI, fully deterministic). These verbs mutate editor
//    state — they open a sequence, move a playhead, open an asset editor — so a malformed
//    request MUST be refused before any of that happens. The tests below deliberately supply
//    only garbage arguments and assert a typed rejection; if the handler ever starts resolving
//    the world first, the actor/sequence lookup would fail first and these would change code.
//  * The shot-budget rejection on an EXPLICIT frame list, which is knowable from the request
//    alone. frames x angles is the real cost of a burst and the ceiling is on the product, so a
//    5-instant six-side request (30 shots) must be refused rather than silently clipped.
//  * The static-mesh rejection on render.capture_animation_preview, and the reciprocal
//    skinned-asset rejection on render.capture_asset_preview NAMING the other verb. Those two
//    messages are the only discovery path between the two capture surfaces; without them an
//    agent told "Static Mesh only" concludes that isolated skinned capture does not exist and
//    goes off to dirty a level instead. Delete either and the pair stops pointing at each other.
//  * The live path, guarded. A successful burst must carry per-shot imageStats and a pose
//    verdict; a failed one must carry a TYPED code from a known list. An empty or unknown code
//    still fails the assertion, so "the capture silently did nothing" cannot pass as a skip.
#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"

#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // A Static Mesh that exists in every engine install, used to prove
    // render.capture_animation_preview refuses a non-skinned asset without opening anything.
    const TCHAR* PWAnimCapEngineCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // A non-mesh asset, used to reach render.capture_asset_preview's non-StaticMesh branch.
    const TCHAR* PWAnimCapEngineMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");

    // An engine-shipped, general-purpose skinned mesh. The motion sequence used with it is
    // synthesized below, so this test does not depend on content from the project hosting the
    // plugin checkout or on a visually subtle engine animation.
    const TCHAR* PWAnimCapFixtureMeshPath =
        TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
    const TCHAR* PWAnimCapFixtureMeshRequestPath =
        TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP");

    struct FPWAnimCapSyntheticAnimation
    {
        ~FPWAnimCapSyntheticAnimation()
        {
            Reset();
        }

        bool Create(USkeleton* Skeleton)
        {
            Reset();
            if (!Skeleton || Skeleton->GetReferenceSkeleton().GetRawBoneNum() < 4)
            {
                return false;
            }

            const FString AssetName = FString::Printf(TEXT("PW_AnimationCapture_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            PackagePath = TEXT("/Game/PinWrightTests/Render/") + AssetName;
            UPackage* Package = CreatePackage(*PackagePath);
            Sequence = Package
                ? NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone)
                : nullptr;
            if (!Sequence)
            {
                return false;
            }

            Sequence->SetSkeleton(Skeleton);
            constexpr int32 LastFrame = 36;
            IAnimationDataController& Controller = Sequence->GetController();
            Controller.InitializeModel();
            Controller.SetFrameRate(FFrameRate(30, 1), /*bShouldTransact=*/false);
            Controller.SetNumberOfFrames(FFrameNumber(LastFrame), /*bShouldTransact=*/false);

            const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
            for (int32 BoneIndex = 1; BoneIndex <= 3; ++BoneIndex)
            {
                const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
                const FTransform& RefPose = RefSkeleton.GetRawRefBonePose()[BoneIndex];
                TArray<FVector3f> Positions;
                TArray<FQuat4f> Rotations;
                TArray<FVector3f> Scales;
                Positions.Reserve(LastFrame + 1);
                Rotations.Reserve(LastFrame + 1);
                Scales.Reserve(LastFrame + 1);
                for (int32 Frame = 0; Frame <= LastFrame; ++Frame)
                {
                    // A square-root sweep to a half turn, not a linear quarter turn: a linear
                    // sweep left the first requested frame (9 of 36) at 22.5 degrees, a pose the
                    // mesh barely moves off frame 0 for. Half the sweep is spent by frame 9
                    // (90 degrees), and the later requested frames (127, 156, 180 degrees) stay
                    // separated from frame 0 and from each other.
                    const double Degrees = 180.0 * FMath::Sqrt(
                        static_cast<double>(Frame) / static_cast<double>(LastFrame));
                    const FVector Axis = BoneIndex % 2 == 0
                        ? FVector::RightVector : FVector::ForwardVector;
                    FQuat Rotation = FQuat(Axis, FMath::DegreesToRadians(Degrees)) *
                        RefPose.GetRotation();
                    Rotation.Normalize();
                    Positions.Add(FVector3f(RefPose.GetTranslation()));
                    Rotations.Add(FQuat4f(Rotation));
                    Scales.Add(FVector3f(RefPose.GetScale3D()));
                }
                Controller.AddBoneCurve(BoneName, /*bShouldTransact=*/false);
                Controller.SetBoneTrackKeys(
                    BoneName, Positions, Rotations, Scales, /*bShouldTransact=*/false);
            }
            Controller.NotifyPopulated();

            SequenceObjectPath = Sequence->GetPathName();
            Sequence->AddToRoot();
            FAssetRegistryModule::AssetCreated(Sequence);
            bRegistered = true;
            return true;
        }

        void Reset()
        {
            if (Sequence)
            {
                if (UPackage* Package = Sequence->GetOutermost())
                {
                    Package->SetDirtyFlag(false);
                }
                Sequence->RemoveFromRoot();
                if (bRegistered)
                {
                    CleanupTestAsset(SequenceObjectPath);
                }
            }
            Sequence = nullptr;
            bRegistered = false;
            SequenceObjectPath.Empty();
            PackagePath.Empty();
        }

        UAnimSequence* Sequence = nullptr;
        FString PackagePath;
        FString SequenceObjectPath;
        bool bRegistered = false;
    };

    FString PWAnimCapMissingPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/MCP_AnimCapAbsent/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    bool PWAnimCapDecodePng(const TArray<uint8>& PngBytes, TArray64<uint8>& OutPixels)
    {
        OutPixels.Reset();
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> PngWrapper =
            ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        return PngWrapper.IsValid() &&
            PngWrapper->SetCompressed(PngBytes.GetData(), PngBytes.Num()) &&
            PngWrapper->GetRaw(ERGBFormat::BGRA, 8, OutPixels);
    }

    // Motion is measured over the bounding box of the tiles that actually changed, never over the
    // whole frame. A pose moves the subject, which covers a small part of a 768x768 preview, so a
    // whole-frame mean divides the signal by the framing instead of by the motion: the same pose
    // scores ~20x lower simply because ~970 of the 1024 tiles are backdrop nothing can move.
    // docs/rpc-design.md records the general form ("a statistic aggregated over the frame is
    // dominated by the part of the frame nothing is happening in"), and the sibling pose-list
    // stability test restricts its own assertion to the subject region for the same reason.
    bool PWAnimCapHasRenderedTileMotion(const TArray64<uint8>& BasePixels,
        const TArray64<uint8>& CurrentPixels, int32 Width, int32 Height,
        double& OutMeanRoiTileDelta, int32& OutTilesOverFive, int32& OutRoiTileCount)
    {
        constexpr int32 TileSize = 24;
        if (Width <= 0 || Height <= 0 || Width % TileSize != 0 || Height % TileSize != 0)
        {
            return false;
        }
        const int64 RequiredBytes = static_cast<int64>(Width) * Height * 4;
        if (BasePixels.Num() < RequiredBytes || CurrentPixels.Num() < RequiredBytes)
        {
            return false;
        }

        const int32 TilesX = Width / TileSize;
        const int32 TilesY = Height / TileSize;
        TArray<double> BaseTiles;
        TArray<double> CurrentTiles;
        BaseTiles.SetNumZeroed(TilesX * TilesY);
        CurrentTiles.SetNumZeroed(TilesX * TilesY);
        const auto BuildTiles = [Width, Height, TilesX, TilesY](
                                    const TArray64<uint8>& Pixels, TArray<double>& Tiles)
        {
            for (int32 TileY = 0; TileY < TilesY; ++TileY)
            {
                for (int32 TileX = 0; TileX < TilesX; ++TileX)
                {
                    double LuminanceSum = 0.0;
                    for (int32 Y = 0; Y < TileSize; ++Y)
                    {
                        for (int32 X = 0; X < TileSize; ++X)
                        {
                            const int64 PixelOffset =
                                (static_cast<int64>(TileY * TileSize + Y) * Width +
                                    TileX * TileSize + X) * 4;
                            // GetRaw(BGRA) returns B, G, R, A. Alpha is deliberately ignored.
                            LuminanceSum +=
                                (Pixels[PixelOffset] + 2.0 * Pixels[PixelOffset + 1] +
                                    Pixels[PixelOffset + 2]) * 0.25;
                        }
                    }
                    Tiles[TileY * TilesX + TileX] =
                        LuminanceSum / static_cast<double>(TileSize * TileSize);
                }
            }
        };
        BuildTiles(BasePixels, BaseTiles);
        BuildTiles(CurrentPixels, CurrentTiles);

        OutMeanRoiTileDelta = 0.0;
        OutTilesOverFive = 0;
        OutRoiTileCount = 0;
        TArray<double> Deltas;
        Deltas.SetNumZeroed(BaseTiles.Num());
        int32 MinTileX = TilesX;
        int32 MinTileY = TilesY;
        int32 MaxTileX = -1;
        int32 MaxTileY = -1;
        for (int32 TileY = 0; TileY < TilesY; ++TileY)
        {
            for (int32 TileX = 0; TileX < TilesX; ++TileX)
            {
                const int32 Index = TileY * TilesX + TileX;
                const double Delta = FMath::Abs(BaseTiles[Index] - CurrentTiles[Index]);
                Deltas[Index] = Delta;
                if (Delta > 5.0)
                {
                    ++OutTilesOverFive;
                    MinTileX = FMath::Min(MinTileX, TileX);
                    MinTileY = FMath::Min(MinTileY, TileY);
                    MaxTileX = FMath::Max(MaxTileX, TileX);
                    MaxTileY = FMath::Max(MaxTileY, TileY);
                }
            }
        }
        if (OutTilesOverFive < 3)
        {
            return false;
        }

        double RoiDeltaSum = 0.0;
        for (int32 TileY = MinTileY; TileY <= MaxTileY; ++TileY)
        {
            for (int32 TileX = MinTileX; TileX <= MaxTileX; ++TileX)
            {
                RoiDeltaSum += Deltas[TileY * TilesX + TileX];
                ++OutRoiTileCount;
            }
        }
        OutMeanRoiTileDelta = RoiDeltaSum / static_cast<double>(OutRoiTileCount);
        return OutMeanRoiTileDelta > 1.5;
    }

    // Distinctly named (the sequencer suites use CreateSetPlayheadSequence / CreateEvalReadbackSequence
    // for the same reason) so anonymous-namespace symbols do not ODR-collide when Unity merges TUs.
    ULevelSequence* PWAnimCapCreateSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_AnimShotsSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_AnimShotsProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create — "
                               "the animation-burst contract cannot be exercised without one."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Spawns a NON-transient AStaticMeshActor carrying the engine unit cube, so the verb under
    // test can actually resolve it. The shared SpawnTransientCubeActor sets RF_Transient, and
    // camera.animation_shots resolves its target through McpActorUtils::FindActorByName(nullptr,
    // ...) -> UEditorActorSubsystem::GetAllLevelActors, which drops transient actors (UE 5.8
    // EditorActorSubsystem.cpp:386). Same helper shape as AnnotatedSpawnResolvableCubeActor in
    // TestAnnotatedCaptureHandlers.cpp. Pair with FScopedEditorWorldActorGuard, which destroys
    // whatever the test spawned and restores the level's dirty flag on scope exit.
    AStaticMeshActor* PWAnimShotsSpawnResolvableCubeActor(UWorld* World, const FString& Label,
        const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams; // deliberately NOT RF_Transient (see above)
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    // Typed, non-crashing exits camera.animation_shots is allowed to return when the burst cannot
    // complete in this run. An empty or unlisted code still fails.
    //
    // ACTOR_NOT_FOUND is deliberately NOT in this list. It used to be, described as "the usual one
    // under -unattended" because the fixture spawned RF_Transient and so was invisible to
    // FindActorByName. That was not an environment-dependent skip — it fired on EVERY run, which
    // meant the StaticMeshActor fixture never reached the skeletal-mesh check it exists to
    // provoke, and the two outcomes were indistinguishable because both codes sat in this list.
    // The fixture now spawns resolvable, so an unresolved target is a real defect and must fail.
    bool PWAnimShotsIsTypedFailure(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("ACTOR_NO_SKELETAL_MESH_COMPONENT") ||
            ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            ErrorCode == TEXT("SEQUENCE_NOT_OPEN") ||
            ErrorCode == TEXT("SEQUENCE_NOT_FOUND") ||
            ErrorCode == TEXT("SEQUENCE_INVALID") ||
            ErrorCode == TEXT("BOUNDS_EMPTY") ||
            ErrorCode == TEXT("EXECUTION_ERROR") ||
            ErrorCode == TEXT("CAPTURE_FAILED") ||
            ErrorCode == TEXT("ENCODE_FAILED") ||
            ErrorCode == TEXT("SAVE_FAILED");
    }
}

// ---------------------------------------------------------------------------
// camera.animation_shots
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsParamSurfaceTest,
    "PinWright.camera.animation_shots.ParamSurfaceIsDiscoverable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsParamSurfaceTest::RunTest(const FString& Parameters)
{
    // Callers discover a verb's shape from the registered specs, so every axis of the burst has
    // to be declared and not merely readable from the payload. Both axes matter: a `frames`
    // slot with no `angles` slot is a burst that cannot be reviewed from more than one side.
    TestTrue(TEXT("camera.animation_shots is registered"),
        IsHandlerRegistered(TEXT("camera.animation_shots")));

    const TCHAR* RequiredParams[] = {
        TEXT("sequencePath"), TEXT("actorName"),
        TEXT("frames"), TEXT("frameCount"), TEXT("frameStep"), TEXT("intervalSeconds"),
        TEXT("angles"), TEXT("count"), TEXT("views"),
        TEXT("width"), TEXT("height"), TEXT("updateMethod"), TEXT("forceUpdate")
    };
    for (const TCHAR* ParamName : RequiredParams)
    {
        TestNotNull(*FString::Printf(TEXT("camera.animation_shots declares a '%s' param"), ParamName),
            GetRegisteredParamSpec(TEXT("camera.animation_shots"), ParamName));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsRequiresSequenceTest,
    "PinWright.camera.animation_shots.RequiresSequencePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsRequiresSequenceTest::RunTest(const FString& Parameters)
{
    // Scrubbing a sequence is the ONLY thing that makes a bound skeletal mesh evaluate in the
    // editor, so a burst with no sequence cannot animate anything. Refusing it is what stops a
    // caller collecting N identical bind-pose images and calling it a review.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("MCP_AnimShots_NoSuchActor"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a burst with no sequence is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("a missing sequence path is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the rejection names sequencePath"),
        Capture.Message.Contains(TEXT("sequencePath")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsRejectsUnknownUpdateMethodTest,
    "PinWright.camera.animation_shots.RejectsUnknownUpdateMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsRejectsUnknownUpdateMethodTest::RunTest(const FString& Parameters)
{
    // The burst shares sequencer.set_playhead's update-method vocabulary through one production
    // parser (SequencePlayheadUtils::ParseUpdateMethod). This pins that it is REJECTING by name
    // rather than falling back to scrub, and — because this rejection is reached with a
    // nonexistent actor and a nonexistent sequence — that the request is validated before any
    // world lookup or editor mutation happens.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("MCP_AnimShots_NoSuchActor"));
    Payload->SetStringField(TEXT("sequencePath"), PWAnimCapMissingPath(TEXT("Seq")));
    Payload->SetStringField(TEXT("updateMethod"), TEXT("teleport"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unknown updateMethod is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("an unknown updateMethod is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the rejection names the offending value"),
        Capture.Message.Contains(TEXT("teleport")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsRejectsViewsWithCountTest,
    "PinWright.camera.animation_shots.RejectsViewsCombinedWithCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsRejectsViewsWithCountTest::RunTest(const FString& Parameters)
{
    // `views` and `count` are two different answers to "which angles". Silently ranking one
    // over the other is how a verb ends up with parameters that contradict each other depending
    // on a mode flag, and the caller never learns which plan ran.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("MCP_AnimShots_NoSuchActor"));
    Payload->SetStringField(TEXT("sequencePath"), PWAnimCapMissingPath(TEXT("Seq")));
    Payload->SetStringField(TEXT("views"), TEXT("sides"));
    Payload->SetNumberField(TEXT("count"), 4.0);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("contradicting view plans is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("contradicting view plans is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the rejection names views"), Capture.Message.Contains(TEXT("views")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsRejectsOversizedBurstTest,
    "PinWright.camera.animation_shots.RejectsShotBudgetOverflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsRejectsOversizedBurstTest::RunTest(const FString& Parameters)
{
    // Five instants from six sides is thirty viewport captures. The ceiling is on the PRODUCT of
    // the two axes, not on either one, and it must be a typed refusal rather than a silent clip:
    // a burst quietly truncated to 24 would drop instants the caller believes it is comparing.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("MCP_AnimShots_NoSuchActor"));
    Payload->SetStringField(TEXT("sequencePath"), PWAnimCapMissingPath(TEXT("Seq")));
    Payload->SetStringField(TEXT("views"), TEXT("sides"));

    TArray<TSharedPtr<FJsonValue>> Frames;
    for (int32 Index = 0; Index < 5; ++Index)
    {
        Frames.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Index * 6)));
    }
    Payload->SetArrayField(TEXT("frames"), Frames);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an oversized burst is an error, not a silently clipped success"), Capture.bSuccess);
    TestEqual(TEXT("an oversized burst is reported as TOO_MANY_SHOTS"),
        Capture.ErrorCode, FString(TEXT("TOO_MANY_SHOTS")));
    TestTrue(TEXT("the rejection states the product that overflowed"),
        Capture.Message.Contains(TEXT("30")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsRejectsUnknownParamTest,
    "PinWright.camera.animation_shots.RejectsUnknownParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsRejectsUnknownParamTest::RunTest(const FString& Parameters)
{
    // Routed through the real dispatcher, which is where unknown-param rejection lives. A
    // misspelled `frameStep` silently ignored would produce a burst on the wrong grid that still
    // reported success.
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("MCP_AnimShots_NoSuchActor"));
    Params->SetStringField(TEXT("sequencePath"), PWAnimCapMissingPath(TEXT("Seq")));
    Params->SetStringField(TEXT("bogusUnknownArg"), TEXT("x"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("camera.animation_shots"),
        TEXT("req-anim-shots-unknown"), Params, bSuccess, ErrorCode);
    TestFalse(TEXT("an unknown param is rejected"), bSuccess);
    TestEqual(TEXT("an unknown param is reported as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAnimationShotsReportsEvidenceTest,
    "PinWright.camera.animation_shots.ReportsPoseEvidenceOrFailsTyped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraAnimationShotsReportsEvidenceTest::RunTest(const FString& Parameters)
{
    // A StaticMeshActor fixture is deliberate: the verb must reject it with
    // ACTOR_NO_SKELETAL_MESH_COMPONENT, because a rig assembled from StaticMeshActors cannot
    // animate by design, and mistaking one for a broken rig is a diagnosis this project has
    // already paid for.
    //
    // That rejection is now ASSERTED EXACTLY rather than accepted as one of thirteen typed exits.
    // It is deterministic and needs no viewport: AnimationShotsHandler.cpp resolves the actor and
    // then checks for a USkeletalMeshComponent BEFORE it loads the sequence or touches the
    // viewport, so nothing environmental can intervene. Previously the fixture was RF_Transient
    // and therefore unresolvable, so every run exited one step earlier at ACTOR_NOT_FOUND — with
    // both codes in the typed-failure list, "the verb rejected a StaticMeshActor" and "the verb
    // never found the actor at all" were indistinguishable and this test proved neither.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Skipped: no editor world."));
        return true;
    }

    FString SequencePath;
    ULevelSequence* Sequence = PWAnimCapCreateSequence(*this, SequencePath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SequencePath); };

    // The guard, not a bare Destroy(): a non-transient spawn dirties the persistent level, and
    // FScopedEditorWorldActorGuard is what removes the actor AND restores the dirty flag, so the
    // open map is left exactly as this test found it.
    FScopedEditorWorldActorGuard ActorGuard;
    const FString Label = FString::Printf(TEXT("MCP_AnimShotsFixture_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Fixture = PWAnimShotsSpawnResolvableCubeActor(World, Label,
        FVector(0.0f, 0.0f, 300.0f));
    if (!Fixture)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-or-spawn-unavailable"),
            TEXT("Skipped: could not spawn the cube fixture (engine cube unavailable)."));
        return true;
    }

    // Resolve by the label the handler will actually see (SetActorLabel stores verbatim, but the
    // fixture reads it back rather than assuming, matching the sibling camera tests).
    const FString ResolvedName = Fixture->GetActorLabel();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), ResolvedName);
    Payload->SetStringField(TEXT("sequencePath"), SequencePath);
    Payload->SetNumberField(TEXT("frameCount"), 2.0);
    Payload->SetNumberField(TEXT("width"), 128.0);
    Payload->SetNumberField(TEXT("height"), 128.0);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("camera.animation_shots handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("camera.animation_shots"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("camera.animation_shots responded"), Capture.bWasCalled);

    // THE assertion this fixture exists for, stated exactly. A StaticMeshActor has no pose to
    // sample, so the verb must refuse it by name rather than photographing it and reporting a set
    // that is identical at every frame — the "photographs identically at every frame" misdiagnosis
    // the verb was built to end.
    TestFalse(TEXT("a StaticMeshActor target is refused, not photographed"), Capture.bSuccess);
    TestEqual(TEXT("a target with no skeletal mesh is refused as ACTOR_NO_SKELETAL_MESH_COMPONENT"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NO_SKELETAL_MESH_COMPONENT")));
    TestTrue(*FString::Printf(TEXT("the refusal is a typed code (got '%s')"), *Capture.ErrorCode),
        PWAnimShotsIsTypedFailure(Capture.ErrorCode));

    if (!Capture.bSuccess)
    {
        return true;
    }

    // Unreachable while the gate above holds — a StaticMeshActor can never produce a burst — but
    // kept so the self-describing-set contract is still stated somewhere. If the gate is ever
    // relaxed deliberately, these fire instead of silently going missing. They need a
    // SkeletalMeshActor fixture to be exercised for real.
    //
    // A successful burst must be self-describing. Every one of these fields exists so a set
    // cannot come back unreadable-but-clean: shots carry their own luminance statistics, and the
    // pose verdict says whether the images differ for a reason.
    TestTrue(TEXT("a successful burst reports whether the pose was sampled"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("poseSampled")));
    TestTrue(TEXT("a successful burst reports whether the pose changed"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("poseChanged")));
    TestTrue(TEXT("a successful burst reports a per-instant frame list"),
        Capture.Result.IsValid() && Capture.Result->HasField(TEXT("frames")));

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (TestTrue(TEXT("a successful burst returns shots"),
            Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("shots"), Shots)) &&
        Shots)
    {
        for (const TSharedPtr<FJsonValue>& ShotValue : *Shots)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            if (ShotValue.IsValid() && ShotValue->TryGetObject(ShotObj) && ShotObj)
            {
                // Statistics per shot, not per set: a six-side set could come back six-for-six
                // black and read as a clean success without them.
                TestTrue(TEXT("every shot carries imageStats"), (*ShotObj)->HasField(TEXT("imageStats")));
                TestTrue(TEXT("every shot carries a blank verdict"), (*ShotObj)->HasField(TEXT("blank")));
                TestTrue(TEXT("every shot names the instant it was taken at"),
                    (*ShotObj)->HasField(TEXT("frame")));
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// render.capture_animation_preview
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAnimationPreviewParamSurfaceTest,
    "PinWright.render.capture_animation_preview.ParamSurfaceIsDiscoverable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAnimationPreviewParamSurfaceTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("render.capture_animation_preview is registered"),
        IsHandlerRegistered(TEXT("render.capture_animation_preview")));

    const TCHAR* RequiredParams[] = {
        TEXT("assetPath"), TEXT("animation"),
        TEXT("frames"), TEXT("frameCount"), TEXT("frameStep"), TEXT("intervalSeconds"), TEXT("time"),
        TEXT("angles"), TEXT("count"), TEXT("views"),
        TEXT("width"), TEXT("height")
    };
    for (const TCHAR* ParamName : RequiredParams)
    {
        TestNotNull(
            *FString::Printf(TEXT("render.capture_animation_preview declares a '%s' param"), ParamName),
            GetRegisteredParamSpec(TEXT("render.capture_animation_preview"), ParamName));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAnimationPreviewRejectsStaticMeshTest,
    "PinWright.render.capture_animation_preview.RejectsStaticMeshAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAnimationPreviewRejectsStaticMeshTest::RunTest(const FString& Parameters)
{
    // The engine cube is not skinned and has no Persona editor. The rejection must happen before
    // any asset editor is opened, and must name the verb that DOES handle static meshes — half
    // of the two-way pointer between the capture surfaces.
    if (!UEditorAssetLibrary::DoesAssetExist(PWAnimCapEngineCubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: engine cube asset unavailable."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAnimCapEngineCubePath);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_animation_preview handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a Static Mesh is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("a Static Mesh is reported as UNSUPPORTED_ASSET_EDITOR"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    TestTrue(TEXT("the rejection points at render.capture_asset_preview"),
        Capture.Message.Contains(TEXT("render.capture_asset_preview")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAssetPreviewNamesAnimationVerbTest,
    "PinWright.render.capture_asset_preview.SkinnedRejectionNamesTheAnimationVerb",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAssetPreviewNamesAnimationVerbTest::RunTest(const FString& Parameters)
{
    // The other half of the two-way pointer. Before this, capture_asset_preview said only
    // "Static Mesh asset editors only", from which an agent reasonably concludes that isolated
    // capture of a skinned asset does not exist — and goes and dirties a level instead.
    if (!UEditorAssetLibrary::DoesAssetExist(PWAnimCapEngineMaterialPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-default-material-missing"),
            TEXT("Skipped: engine default material unavailable."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAnimCapEngineMaterialPath);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_asset_preview handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a non-Static-Mesh asset is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("a non-Static-Mesh asset is reported as UNSUPPORTED_ASSET_EDITOR"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    TestTrue(TEXT("the rejection points at render.capture_animation_preview"),
        Capture.Message.Contains(TEXT("render.capture_animation_preview")));
    // The probe asset IS a material, so the same refusal must also carry the material/texture
    // clause. Without it the caller's next move is to bind the material to a mesh and capture that
    // instead, opening an asset editor for a picture asset.generate_thumbnail renders without one.
    TestTrue(TEXT("a material rejection points at asset.generate_thumbnail"),
        Capture.Message.Contains(TEXT("asset.generate_thumbnail")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAnimationPreviewRejectsMissingAnimationTest,
    "PinWright.render.capture_animation_preview.RejectsMissingAnimation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAnimationPreviewRejectsMissingAnimationTest::RunTest(const FString& Parameters)
{
    // An unresolvable animation must be a typed lookup failure. Falling back to "no animation"
    // would silently downgrade a motion review to a bind-pose capture that still reported success.
    if (!UEditorAssetLibrary::DoesAssetExist(PWAnimCapEngineCubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: engine cube asset unavailable."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAnimCapEngineCubePath);
    Payload->SetStringField(TEXT("animation"), PWAnimCapMissingPath(TEXT("Anim")));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_animation_preview handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unresolvable animation is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("an unresolvable animation is reported as ANIMATION_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ANIMATION_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAnimationPreviewRejectsViewsWithAnglesTest,
    "PinWright.render.capture_animation_preview.RejectsViewsCombinedWithAngles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAnimationPreviewRejectsViewsWithAnglesTest::RunTest(const FString& Parameters)
{
    // Reached with an assetPath that resolves to nothing, which also pins that the view plan is
    // validated BEFORE the asset is loaded and the Persona editor opened — a malformed request
    // must not leave an asset editor tab open on its way to being rejected.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAnimCapMissingPath(TEXT("Mesh")));
    Payload->SetStringField(TEXT("views"), TEXT("sides"));

    TArray<TSharedPtr<FJsonValue>> Angles;
    TSharedPtr<FJsonObject> Angle = MakeShared<FJsonObject>();
    Angle->SetNumberField(TEXT("azimuth"), 0.0);
    Angle->SetNumberField(TEXT("elevation"), 0.0);
    Angles.Add(MakeShared<FJsonValueObject>(Angle));
    Payload->SetArrayField(TEXT("angles"), Angles);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_animation_preview handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("contradicting view plans is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("contradicting view plans is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAnimationPreviewRejectsMissingAssetTest,
    "PinWright.render.capture_animation_preview.RejectsMissingAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAnimationPreviewRejectsMissingAssetTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAnimCapMissingPath(TEXT("Mesh")));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_animation_preview handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unresolvable asset is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("an unresolvable asset is reported as ASSET_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAnimationPreviewBurstWritesDistinctPoseImagesTest,
    "PinWright.render.capture_animation_preview.BurstWritesDistinctPoseImages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAnimationPreviewBurstWritesDistinctPoseImagesTest::RunTest(const FString& Parameters)
{
    // This is the reported five-frame, two-angle burst through the registered handler. It uses
    // the legacy top-level fields because that is the real caller path that stayed broken while
    // the old structured Tutorial fixture passed.
    if (!TestTrue(TEXT("the engine skeletal-mesh fixture exists"),
            UEditorAssetLibrary::DoesAssetExist(PWAnimCapFixtureMeshPath)))
    {
        return false;
    }

    USkeletalMesh* FixtureMesh = LoadObject<USkeletalMesh>(nullptr, PWAnimCapFixtureMeshPath);
    if (!TestNotNull(TEXT("the engine skeletal-mesh fixture loads"), FixtureMesh) ||
        !TestNotNull(TEXT("the engine skeletal-mesh fixture has a skeleton"),
            FixtureMesh ? FixtureMesh->GetSkeleton() : nullptr))
    {
        return false;
    }

    FPWAnimCapSyntheticAnimation AnimationFixture;
    if (!TestTrue(TEXT("the synthetic animation fixture is registered"),
            AnimationFixture.Create(FixtureMesh->GetSkeleton())) ||
        !TestNotNull(TEXT("the synthetic animation fixture exists"), AnimationFixture.Sequence))
    {
        return false;
    }
    UAnimSequence* Animation = AnimationFixture.Sequence;

    const double FrameRate = Animation->GetSamplingFrameRate().AsDecimal();
    const int32 LastFrame = FMath::FloorToInt(Animation->GetPlayLength() * FrameRate);
    if (!TestTrue(TEXT("the animation fixture has at least two valid frames"),
            FrameRate > 0.0 && LastFrame >= 2))
    {
        return false;
    }
    const TArray<int32> RequestedFrames = {0, 9, 18, 27, 36};
    if (!TestTrue(TEXT("the synthetic animation contains every requested frame"),
            LastFrame >= RequestedFrames.Last()))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAnimCapFixtureMeshRequestPath);
    Payload->SetStringField(TEXT("animation"), AnimationFixture.PackagePath);
    TArray<TSharedPtr<FJsonValue>> Frames;
    for (const int32 Frame : RequestedFrames)
    {
        Frames.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Frame)));
    }
    Payload->SetArrayField(TEXT("frames"), Frames);
    TArray<TSharedPtr<FJsonValue>> Angles;
    for (const double Azimuth : {35.0, 145.0})
    {
        TSharedPtr<FJsonObject> Angle = MakeShared<FJsonObject>();
        Angle->SetNumberField(TEXT("azimuth"), Azimuth);
        Angle->SetNumberField(TEXT("elevation"), 12.0);
        Angles.Add(MakeShared<FJsonValueObject>(Angle));
    }
    Payload->SetArrayField(TEXT("angles"), Angles);
    Payload->SetNumberField(TEXT("width"), 768.0);
    Payload->SetNumberField(TEXT("height"), 768.0);
    Payload->SetBoolField(TEXT("closeAfterCapture"), true);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("the burst handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("the five-frame two-angle burst succeeds"), Capture.bSuccess) ||
        !TestNotNull(TEXT("the burst has a JSON result"), Capture.Result.Get()))
    {
        return false;
    }

    bool bPoseChanged = false;
    TestTrue(TEXT("the response reports that the synthetic poses differ"),
        Capture.Result->TryGetBoolField(TEXT("poseChanged"), bPoseChanged));
    if (!TestTrue(TEXT("the five requested synthetic frames genuinely change the pose"), bPoseChanged))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!TestTrue(TEXT("the response contains the two burst shots"),
            Capture.Result->TryGetArrayField(TEXT("shots"), Shots)) ||
        !TestNotNull(TEXT("the shots array is present"), Shots))
    {
        return false;
    }
    if (!TestEqual(TEXT("the burst wrote exactly ten shots"), Shots->Num(), 10))
    {
        return false;
    }

    TArray<FString> WrittenPaths;
    TArray<TArray<uint8>> FirstViewPngBytes;
    TArray<TArray64<uint8>> FirstViewPixels;
    FirstViewPngBytes.SetNum(2);
    FirstViewPixels.SetNum(2);
    ON_SCOPE_EXIT
    {
        for (const FString& WrittenPath : WrittenPaths)
        {
            IFileManager::Get().Delete(*WrittenPath, false, true, true);
        }
    };

    for (int32 FrameIndex = 0; FrameIndex < RequestedFrames.Num(); ++FrameIndex)
    {
        for (int32 ViewIndex = 0; ViewIndex < 2; ++ViewIndex)
        {
            const int32 ShotIndex = FrameIndex * 2 + ViewIndex;
            const TSharedPtr<FJsonObject> Shot = (*Shots)[ShotIndex]->AsObject();
            const FString FrameLabel = FString::Printf(
                TEXT("frame %d view %d"), RequestedFrames[FrameIndex], ViewIndex);
            FString WrittenPath;
            if (!TestNotNull(*FString::Printf(TEXT("%s has an object"), *FrameLabel), Shot.Get()) ||
                !TestTrue(*FString::Printf(TEXT("%s publishes its written PNG path"), *FrameLabel),
                    Shot->TryGetStringField(TEXT("path"), WrittenPath)))
            {
                return false;
            }
            WrittenPaths.Add(WrittenPath);

            TArray<uint8> PngBytes;
            TArray64<uint8> Pixels;
            if (!TestTrue(*FString::Printf(TEXT("%s PNG can be read"), *FrameLabel),
                    FFileHelper::LoadFileToArray(PngBytes, *WrittenPath)) ||
                !TestTrue(*FString::Printf(TEXT("%s PNG decodes to pixels"), *FrameLabel),
                    PWAnimCapDecodePng(PngBytes, Pixels)))
            {
                return false;
            }

            if (FrameIndex == 0)
            {
                FirstViewPngBytes[ViewIndex] = MoveTemp(PngBytes);
                FirstViewPixels[ViewIndex] = MoveTemp(Pixels);
            }
            else
            {
                double MeanRoiTileDelta = 0.0;
                int32 TilesOverFive = 0;
                int32 RoiTileCount = 0;
                const bool bRenderedMotion = PWAnimCapHasRenderedTileMotion(
                    FirstViewPixels[ViewIndex], Pixels, 768, 768,
                    MeanRoiTileDelta, TilesOverFive, RoiTileCount);
                // Logged on pass as well as on fail: a threshold whose margin is only printed
                // when it fires cannot be seen drifting toward it.
                UE_LOG(LogTemp, Display,
                    TEXT("AnimCapBurst %s meanRoiTileDelta=%.3f tilesOver5=%d roiTiles=%d"),
                    *FrameLabel, MeanRoiTileDelta, TilesOverFive, RoiTileCount);
                TestTrue(*FString::Printf(
                    TEXT("%s PNG bytes and rendered content differ at the same camera "
                         "(meanRoiTileDelta=%.3f, tilesOver5=%d, roiTiles=%d)"),
                    *FrameLabel, MeanRoiTileDelta, TilesOverFive, RoiTileCount),
                    FirstViewPngBytes[ViewIndex] != PngBytes && bRenderedMotion);
            }
        }
    }
    return true;
}
