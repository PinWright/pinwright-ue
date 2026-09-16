// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for the ANIMATION-ASSET subject provider
// (Handlers/Render/CaptureSubjectProviders_Animation.{h,cpp}).
//
// THE CONTRACT ASSERTIONS DO NOT DEPEND ON A LIVE PREVIEW VIEWPORT, and that is a property of the
// provider's shape rather than a concession. ResolveAnimationSubjectFacts settles the animation, the
// preview mesh, the bounds and the time window from the request alone and opens nothing; the pose
// and bounds behaviour is driven against the same production functions the provider installs
// (MakeScrubTimeSetter, ComputeAssetBounds, ComputeSampledPosedUnionBounds, FPreviewInstanceState)
// on a UDebugSkelMeshComponent in a plain FPreviewScene this file builds. So this chunk's acceptance
// criterion — posed bounds are never used — is measured on every host, not only on one that can
// realise a Persona window.
//
// The end-to-end Resolve() path is asserted IN ADDITION, exactly as the mesh and Niagara providers'
// tests do. When it cannot realise a preview viewport the test reports NOT MEASURED for that half
// rather than passing quietly, and the code it got must be in a typed host-limited set — "the
// resolve silently did nothing" cannot pass as a skip.
//
// NOTHING BELOW DEREFERENCES FResolvedSubject::ViewportClient. Comparing the pointer to null is
// defined whether or not the RAII release has already run; reading through it would not be.
//
// EVERY TEST THAT CAN OPEN AN ASSET EDITOR CLOSES IT. FResolvedSubject releases in its destructor,
// and the scope guard below is declared FIRST so it runs LAST — after that destructor — and catches
// anything the release rule deliberately left open. An asset editor still open when the editor exits
// faults in the toolkit destructor (docs/lessons.md:166), which turns a passing test into a crashed
// suite at shutdown.
//
// EVERY SKIP ANNOUNCES ITSELF. The fixture pair is engine content and a host can be missing it. Each
// early return emits the `PINWRIGHT_ASSERTIONS_SKIPPED:` marker that
// Content/Python/check_suite_log.py reconciles into its own `skipped` outcome, so a run that
// measured nothing here cannot read as COMPLETED_CLEAN. It is a WARNING, never a failure — board
// tickets `B-test-skips-assertions-silently` and `B-tests-host-dependent-fixtures-hard-fail`.
#include "Misc/AutomationTest.h"

#include "Handlers/Render/AnimationPoseEvidence.h"
#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Animation.h"
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Misc/FrameRate.h"
#include "PreviewScene.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/StrongObjectPtr.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "Tests/TestSkipReporting.h"

// std::numeric_limits: the only portable source of a quiet NaN for the non-finite-instant refusal.
// TNumericLimits has no NaN, and building one by arithmetic trips MSVC's constant-overflow warning,
// which this codebase compiles as an error. Same include the recorder reductions use.
#include <limits>

// File-unique NAMED namespace, not an anonymous one: Unity merges translation units and these helper
// names would otherwise collide with the same-shaped helpers in sibling Tests/Render/*.cpp files.
namespace PinWrightCaptureSubjectAnimationTests
{
    // Engine-shipped fixture pair: a skinned mesh and an animation on its skeleton, both under
    // Engine/Content, so this file depends on no host project content.
    const TCHAR* const FixtureMeshPath =
        TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
    const TCHAR* const FixtureAnimationPath =
        TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
    // A non-animation asset, used to reach the kind's refusal without opening anything.
    const TCHAR* const NonAnimationAssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // Asserted as LITERALS, never as ErrorCodes::ERR_*: what matters is the wire string a caller
    // sees. Comparing the provider's constant against the same constant would still pass if the
    // constant's value changed.
    const TCHAR* const UnsupportedCode = TEXT("UNSUPPORTED_ASSET_EDITOR");
    const TCHAR* const AnimationNotFoundCode = TEXT("ANIMATION_NOT_FOUND");
    const TCHAR* const AssetNotFoundCode = TEXT("ASSET_NOT_FOUND");
    const TCHAR* const InvalidArgumentCode = TEXT("INVALID_ARGUMENT");

    FString AbsentPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/MCP_AnimSubjectAbsent/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Codes that mean "this host could not realise a preview viewport", as distinct from "the
    // provider rejected the request". Anything outside this set on an otherwise-valid request is a
    // hard failure.
    bool IsHostLimitedCode(const FString& Code)
    {
        return Code == TEXT("PREVIEW_VIEWPORT_NOT_FOUND")
            || Code == TEXT("PREVIEW_NOT_FOUND")
            || Code == TEXT("OPEN_FAILED")
            || Code == TEXT("SUBSYSTEM_MISSING")
            || Code == TEXT("SKELETAL_MESH_NOT_FOUND")
            || Code == TEXT("EDITOR_NOT_AVAILABLE");
    }

    void CloseEditorFor(const TCHAR* AssetPath)
    {
        if (!GEditor)
        {
            return;
        }
        UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        UObject* Asset = LoadObject<UObject>(nullptr, AssetPath);
        if (Subsystem && Asset)
        {
            Subsystem->CloseAllEditorsForAsset(Asset);
        }
    }

    // ---- the amplitude fixture the posed-bounds test needs, authored from engine parts ----
    //
    // WHY IT IS AUTHORED RATHER THAN LOADED. PosedBoundsAreNotUsed can only distinguish "the code
    // read the asset's bounds" from "the code read the component's posed bounds" on a subject whose
    // POSED measurement actually moves between instants; on a fixture that barely moves, both
    // implementations produce the same numbers and the test asserts nothing. Engine content ships
    // exactly two UAnimSequences, both on this one skeleton, and MEASURED, neither is that fixture:
    // Tutorial_Walk_Fwd spans 91.9043..99.6268 cm across nine instants (8.40%) and Tutorial_Idle
    // spans 100.8370..101.5517 cm (0.71%). A walk cycle is bounds-neutral almost by definition —
    // that is what lets it loop — and an idle more so, so no choice among engine clips fixes this,
    // which is why the clip is built here instead of the gate being lowered to fit the walk.
    //
    // WHY SCALE. ComputePosedBoneBox measures the box around component-space bone LOCATIONS. That
    // box's extent is invariant under translating the whole cloud, and its extent.Size() is a
    // diagonal length, so it is very nearly invariant under rotating the cloud as a whole too —
    // tipping the skeleton 90 degrees swaps two extents and leaves the diagonal alone. A uniform
    // scale on the root is the one whole-skeleton transform that moves the measurement by a KNOWN
    // factor: the cloud scales about the root's own origin, so the spread this fixture produces is
    // arithmetic (~1.0 -> ~1.9 across the sampled instants) rather than a property of somebody's
    // animation that a future engine content drop could quietly change.
    //
    // Everything it needs is engine content: the same skeleton the engine mesh fixture already
    // requires, and its own bone-0 name and reference pose. Nothing is written to disk.
    UAnimSequence* MakeAmplifiedPoseClip(FString& OutFailure)
    {
        USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, FixtureMeshPath);
        USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
        if (!Skeleton)
        {
            OutFailure = FString::Printf(TEXT("engine fixture skeleton unavailable (mesh=%s)"),
                Mesh ? TEXT("ok") : TEXT("missing"));
            return nullptr;
        }
        const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
        if (RefSkeleton.GetNum() <= 0 || RefSkeleton.GetRefBonePose().Num() <= 0)
        {
            OutFailure = TEXT("the engine fixture skeleton has no reference pose to key against");
            return nullptr;
        }
        const FName RootBone = RefSkeleton.GetBoneName(0);
        const FTransform RootRefPose = RefSkeleton.GetRefBonePose()[0];

        UAnimSequence* Sequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
        if (!Sequence)
        {
            OutFailure = TEXT("could not create the transient UAnimSequence");
            return nullptr;
        }
        Sequence->SetSkeleton(Skeleton);

        constexpr int32 NumFrames = 30;
        constexpr float EndScale = 2.0f;
        TArray<FVector3f> PositionKeys;
        TArray<FQuat4f> RotationKeys;
        TArray<FVector3f> ScaleKeys;
        PositionKeys.Reserve(NumFrames + 1);
        RotationKeys.Reserve(NumFrames + 1);
        ScaleKeys.Reserve(NumFrames + 1);
        for (int32 KeyIndex = 0; KeyIndex <= NumFrames; ++KeyIndex)
        {
            const float Alpha = static_cast<float>(KeyIndex) / static_cast<float>(NumFrames);
            PositionKeys.Add(FVector3f(RootRefPose.GetTranslation()));
            RotationKeys.Add(FQuat4f(RootRefPose.GetRotation()));
            ScaleKeys.Add(FVector3f(RootRefPose.GetScale3D()) * FMath::Lerp(1.0f, EndScale, Alpha));
        }

        // bShouldTransact=false throughout: a test has no undo to populate, and a transaction here
        // would leave the editor's buffer holding a transient asset after the test freed it.
        IAnimationDataController& Controller = Sequence->GetController();
        Controller.OpenBracket(
            FText::FromString(TEXT("PinWright posed-bounds fixture")), /*bShouldTransact=*/false);
        Controller.InitializeModel();
        Controller.SetFrameRate(FFrameRate(NumFrames, 1), /*bShouldTransact=*/false);
        Controller.SetNumberOfFrames(FFrameNumber(NumFrames), /*bShouldTransact=*/false);
        const bool bTrackAdded = Controller.AddBoneCurve(RootBone, /*bShouldTransact=*/false);
        const bool bKeysSet = bTrackAdded
            && Controller.SetBoneTrackKeys(RootBone, PositionKeys, RotationKeys, ScaleKeys,
                /*bShouldTransact=*/false);
        Controller.NotifyPopulated();
        Controller.CloseBracket(/*bShouldTransact=*/false);
        if (!bKeysSet)
        {
            OutFailure = FString::Printf(
                TEXT("the animation data controller refused the fixture clip (bone='%s' trackAdded=%s)"),
                *RootBone.ToString(), bTrackAdded ? TEXT("yes") : TEXT("no"));
            return nullptr;
        }

        // Same reason the loaded fixture calls it: an uncompressed sequence has no keys to sample,
        // and NotifyPopulated is what kicks the compression this waits on.
        Sequence->WaitOnExistingCompression();
        if (!(Sequence->GetPlayLength() > 0.0f))
        {
            OutFailure = TEXT("the authored fixture clip reports a zero play length");
            return nullptr;
        }
        return Sequence;
    }

    // A registered UDebugSkelMeshComponent in its own preview scene — the same component class
    // Persona hands the provider, without a Persona window.
    struct FPreviewFixture
    {
        TUniquePtr<FPreviewScene> Scene;
        UDebugSkelMeshComponent* Component = nullptr;
        USkeletalMesh* Mesh = nullptr;
        UAnimSequence* Animation = nullptr;
        // Non-empty when the fixture could not be built; the caller skips with it as the detail.
        FString Failure;

        // bEnablePreview=false leaves the component in whatever state registration produced, which
        // is what the save/restore test needs to measure as its entry state.
        //
        // AnimationOverride replaces only the CLIP, never the mesh or the skeleton, and it still
        // goes through the same-skeleton check below — so a caller handing in an authored clip gets
        // the identical fixture every other test here runs against, driven by a different animation.
        bool Build(bool bEnablePreview = true, UAnimSequence* AnimationOverride = nullptr)
        {
            Mesh = LoadObject<USkeletalMesh>(nullptr, FixtureMeshPath);
            Animation = AnimationOverride
                ? AnimationOverride
                : LoadObject<UAnimSequence>(nullptr, FixtureAnimationPath);
            if (!Mesh || !Animation)
            {
                Failure = FString::Printf(TEXT("engine fixture unavailable (mesh=%s animation=%s)"),
                    Mesh ? TEXT("ok") : TEXT("missing"), Animation ? TEXT("ok") : TEXT("missing"));
                return false;
            }
            if (Animation->GetSkeleton() != Mesh->GetSkeleton())
            {
                Failure = TEXT("engine fixture mesh and animation are on different skeletons");
                return false;
            }

            // An asset still compiling evaluates to nothing: USkinnedMeshComponent::UpdateBounds
            // early-outs on IsCompiling (SkinnedMeshComponent.cpp:2190-2196), and a compressed
            // animation has no keys to sample until its compression finishes.
            if (Mesh->IsCompiling())
            {
                FSkinnedAssetCompilingManager::Get().FinishCompilation({ Mesh });
            }
            // Unconditional: UAnimSequence::IsCompiling() is PROTECTED (AnimSequence.h:789), so the
            // guard the mesh gets is unavailable here. WaitOnExistingCompression (:824, public)
            // returns immediately when nothing is pending, so calling it always costs nothing and
            // removes the only state in which the sequence has no keys to sample.
            Animation->WaitOnExistingCompression();

            Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
                .SetCreatePhysicsScene(false)
                .SetTransactional(false));
            Component = NewObject<UDebugSkelMeshComponent>(GetTransientPackage());
            if (!Component)
            {
                Failure = TEXT("could not create a UDebugSkelMeshComponent");
                return false;
            }
            Component->SetSkeletalMeshAsset(Mesh);
            // Registration is what runs InitAnim, which is what creates the preview instance
            // (DebugSkelMeshComponent.cpp:607-620). EnablePreview before this is a no-op.
            Scene->AddComponent(Component, FTransform::Identity);

            if (bEnablePreview)
            {
                Component->EnablePreview(true, Animation);
                if (!Component->GetSingleNodeInstance())
                {
                    Failure = TEXT("the preview component has no single-node instance after EnablePreview");
                    return false;
                }
                Component->GetSingleNodeInstance()->SetPlaying(false);
            }
            return true;
        }

        ~FPreviewFixture()
        {
            if (Scene.IsValid() && Component)
            {
                Scene->RemoveComponent(Component);
            }
            Component = nullptr;
            Scene.Reset();
        }
    };

    // Sample times spread across the animation, endpoint-exclusive: a cyclic clip's last frame
    // repeats its first, and a duplicate instant weakens every spread measurement below.
    TArray<double> MakeSampleTimes(const UAnimSequence& Animation, int32 Count)
    {
        TArray<double> Times;
        const double Length = static_cast<double>(Animation.GetPlayLength());
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Times.Add((Length > 0.0) ? (Length * Index) / static_cast<double>(Count) : 0.0);
        }
        return Times;
    }

    // The pose-driven bounds measurement — see ComputePosedBoneBox's header comment for why
    // CalcBounds is not usable as one on a component that is not rendering.
    double PosedBoneRadius(const USkeletalMeshComponent& Component)
    {
        const FBox Box = PinWrightCaptureSubjectAnimation::ComputePosedBoneBox(Component);
        return Box.IsValid ? Box.GetExtent().Size() : 0.0;
    }

    // FResolvedSubject is non-copyable and releases in its destructor, so it is held in place here
    // rather than returned by value.
    struct FResolveProbe
    {
        bool bResolved = false;
        FString ErrCode;
        FString ErrMsg;
        PinWrightCaptureSubject::FResolvedSubject Resolved;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
    };
}

// ---------------------------------------------------------------------------------------------
// Registration. A provider file that fails to reach RegisterProvider is invisible: every other test
// here would report "no provider for kind" and read as an environment problem.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationProviderIsRegisteredTest,
    "PinWright.render.capture_subject_animation.ProviderIsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationProviderIsRegisteredTest::RunTest(const FString& Parameters)
{
    const PinWrightCaptureSubject::FSubjectProvider* Provider =
        PinWrightCaptureSubject::FindProvider(PinWrightCaptureSubject::ESubjectKind::Animation);
    if (!TestNotNull(TEXT("a provider is registered for the animation kind"), Provider))
    {
        return false;
    }
    TestTrue(TEXT("the registered provider claims the animation kind"),
        Provider->Kind == PinWrightCaptureSubject::ESubjectKind::Animation);
    TestTrue(TEXT("the registered provider carries an Acquire"), static_cast<bool>(Provider->Acquire));
    TestTrue(TEXT("the registered provider carries a Release"), static_cast<bool>(Provider->Release));
    return true;
}

// The gate the shared walk applies before it casts an IAssetEditorInstance*. If the Persona names
// ever leave that list this provider stops resolving with a message about an unsupported toolkit,
// which reads like a Persona problem and is not one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationToolkitGateTest,
    "PinWright.render.capture_subject_animation.PersonaToolkitsPassTheSharedGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationToolkitGateTest::RunTest(const FString& Parameters)
{
    const TArrayView<const FName> PersonaNames =
        PinWrightCaptureSubjectMesh::PersonaFamilyToolkitNames();
    TestEqual(TEXT("the Persona family is the four editors that own a preview scene"),
        PersonaNames.Num(), 4);
    for (const FName& Name : PersonaNames)
    {
        TestTrue(*FString::Printf(
                TEXT("AcquireAssetEditorViewport accepts the '%s' toolkit"), *Name.ToString()),
            PinWrightCaptureSubject::IsSupportedAssetEditorToolkit(Name));
        TestTrue(*FString::Printf(TEXT("'%s' is Persona family"), *Name.ToString()),
            PinWrightCaptureSubjectMesh::IsPersonaFamilyToolkit(Name));
    }
    // Both directions: a list that accepts everything is not a gate.
    TestFalse(TEXT("StaticMeshEditor is not Persona family"),
        PinWrightCaptureSubjectMesh::IsPersonaFamilyToolkit(FName(TEXT("StaticMeshEditor"))));
    TestFalse(TEXT("Niagara is not Persona family"),
        PinWrightCaptureSubjectMesh::IsPersonaFamilyToolkit(FName(TEXT("Niagara"))));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The request-only phase: everything that must be decided before anything is opened

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationFactsRefusalsTest,
    "PinWright.render.capture_subject_animation.FactsRefuseWhatIsNotAnAnimation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationFactsRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    // None of these opens an asset editor, which is the point: a malformed request must be refused
    // before any editor state is mutated.
    {
        FSubjectRequest Request;
        Request.Kind = ESubjectKind::Animation;
        FAnimationSubjectFacts Facts;
        FString Code;
        FString Message;
        TestFalse(TEXT("an animation subject naming nothing is refused"),
            ResolveAnimationSubjectFacts(Request, Facts, Code, Message));
        TestEqual(TEXT("naming nothing is UNSUPPORTED_ASSET_EDITOR"), Code, FString(UnsupportedCode));
        TestTrue(*FString::Printf(TEXT("the refusal names the argument that would fix it (got '%s')"),
                *Message),
            Message.Contains(TEXT("subject.animation")));
    }
    {
        FSubjectRequest Request;
        Request.Kind = ESubjectKind::Animation;
        Request.AssetPath = AbsentPath(TEXT("Anim"));
        FAnimationSubjectFacts Facts;
        FString Code;
        FString Message;
        TestFalse(TEXT("an unresolvable path is refused"),
            ResolveAnimationSubjectFacts(Request, Facts, Code, Message));
        TestEqual(TEXT("an unresolvable path is ASSET_NOT_FOUND"), Code, FString(AssetNotFoundCode));
    }
    {
        FSubjectRequest Request;
        Request.Kind = ESubjectKind::Animation;
        Request.AssetPath = NonAnimationAssetPath;
        Request.AnimationPath = AbsentPath(TEXT("Anim"));
        FAnimationSubjectFacts Facts;
        FString Code;
        FString Message;
        TestFalse(TEXT("an unresolvable animation is refused"),
            ResolveAnimationSubjectFacts(Request, Facts, Code, Message));
        TestEqual(TEXT("an unresolvable animation is ANIMATION_NOT_FOUND"),
            Code, FString(AnimationNotFoundCode));
    }
    {
        // A Static Mesh reaches the animation kind. It is not an animation asset and it never can
        // be, so the refusal must name the kind that does serve it rather than the dispatcher's
        // UNKNOWN_PARAMS, which tells the caller nothing (plan decision 6).
        if (LoadObject<UObject>(nullptr, NonAnimationAssetPath))
        {
            FSubjectRequest Request;
            Request.Kind = ESubjectKind::Animation;
            Request.AssetPath = NonAnimationAssetPath;
            FAnimationSubjectFacts Facts;
            FString Code;
            FString Message;
            TestFalse(TEXT("a Static Mesh is not an animation subject"),
                ResolveAnimationSubjectFacts(Request, Facts, Code, Message));
            TestEqual(TEXT("a non-animation asset is UNSUPPORTED_ASSET_EDITOR"),
                Code, FString(UnsupportedCode));
            TestTrue(*FString::Printf(TEXT("the refusal points at the kind that serves it (got '%s')"),
                    *Message),
                Message.Contains(TEXT("skeletalMesh")));
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
                FString::Printf(TEXT("engine cube missing: %s"), NonAnimationAssetPath));
        }
    }
    {
        // Wrong kind into this provider's facts function: refused rather than served, so a
        // mis-registration is visible instead of silently photographing the wrong thing.
        FSubjectRequest Request;
        Request.Kind = ESubjectKind::StaticMesh;
        Request.AssetPath = NonAnimationAssetPath;
        FAnimationSubjectFacts Facts;
        FString Code;
        FString Message;
        TestFalse(TEXT("a non-animation KIND is refused by the animation facts"),
            ResolveAnimationSubjectFacts(Request, Facts, Code, Message));
        TestEqual(TEXT("the wrong kind is INVALID_ARGUMENT"), Code, FString(InvalidArgumentCode));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationFactsCarryAssetBoundsTest,
    "PinWright.render.capture_subject_animation.FactsCarryAssetBoundsAndTheTimeWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationFactsCarryAssetBoundsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, FixtureAnimationPath);
    if (!Animation)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("engine animation fixture missing: %s"), FixtureAnimationPath));
        return true;
    }

    FSubjectRequest Request;
    Request.Kind = ESubjectKind::Animation;
    Request.AssetPath = FixtureAnimationPath;

    // Measured before AND after rather than "no editor is open": another test in the same run may
    // legitimately have one open, and asserting the absolute state would make this test's verdict
    // depend on execution order.
    UAssetEditorSubsystem* Subsystem =
        GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    UObject* const AnimationObject = Animation;
    const bool bEditorOpenBefore =
        Subsystem && Subsystem->FindEditorForAsset(AnimationObject, /*bFocusIfOpen=*/false) != nullptr;

    FAnimationSubjectFacts Facts;
    FString Code;
    FString Message;
    if (!TestTrue(*FString::Printf(TEXT("the animation resolves from the request alone (%s %s)"),
                *Code, *Message),
            ResolveAnimationSubjectFacts(Request, Facts, Code, Message)))
    {
        return false;
    }

    TestTrue(TEXT("the facts name the animation asset"), Facts.AnimationAsset == Animation);
    if (!TestNotNull(TEXT("the facts resolve a preview mesh"), Facts.PreviewMesh))
    {
        return false;
    }
    TestEqual(TEXT("bounds come from the asset, and the source says so"),
        Facts.BoundsSource, FString(TEXT("assetBounds")));
    TestEqual(TEXT("the capture source is the string capture_animation_preview already ships"),
        Facts.CaptureSource, FString(TEXT("personaPreviewViewport")));

    // Exact, with the tolerance spelled 0.0: TestEqual's double overload defaults to
    // UE_KINDA_SMALL_NUMBER, which would let a posed-bounds implementation pass on a mesh whose pose
    // barely moves.
    TestEqual(TEXT("the bounds radius IS the mesh asset's, to the bit"),
        Facts.BoundsRadius, static_cast<double>(Facts.PreviewMesh->GetBounds().SphereRadius), 0.0);
    TestTrue(TEXT("the bounds origin IS the mesh asset's"),
        Facts.BoundsOrigin == Facts.PreviewMesh->GetBounds().Origin);

    TestTrue(TEXT("an animation subject has a time axis"), Facts.bTimeSupported);
    TestEqual(TEXT("the time window starts at zero"), Facts.TimeStartSeconds, 0.0, 0.0);
    TestEqual(TEXT("the time window ends at the animation's play length"),
        Facts.TimeEndSeconds, static_cast<double>(Animation->GetPlayLength()), 1.0e-6);

    // Nothing was opened. The facts phase exists so a malformed request never reaches an editor, and
    // this is the assertion that keeps it honest.
    if (Subsystem)
    {
        const bool bEditorOpenAfter =
            Subsystem->FindEditorForAsset(AnimationObject, /*bFocusIfOpen=*/false) != nullptr;
        TestTrue(TEXT("resolving the facts opened no asset editor"),
            bEditorOpenAfter == bEditorOpenBefore);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Decision 4: bounds come from the mesh asset, never from the posed component

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationPosedBoundsAreNotUsedTest,
    "PinWright.render.capture_subject_animation.PosedBoundsAreNotUsed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationPosedBoundsAreNotUsedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    // THE test for this kind. Framing from posed bounds moves the camera under the subject between
    // instants, which destroys the one comparison a frame burst exists to support — two images of
    // the same thing at two times. The regression is invisible in the images themselves (they look
    // fine, they are just framed differently), so it has to be pinned numerically.
    //
    // Driven by the AUTHORED clip, not by the engine walk cycle every other test here uses: the
    // walk moves the posed bone box by 8.40% across nine instants, under the 10% this test needs
    // before an asset-bounds implementation and a posed-bounds one produce visibly different
    // numbers, so on the walk this test skipped rather than measured. MakeAmplifiedPoseClip's
    // header has the full reasoning and the engine clips that were measured first.
    FString ClipFailure;
    UAnimSequence* AmplifiedClip = MakeAmplifiedPoseClip(ClipFailure);
    if (!AmplifiedClip)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), ClipFailure);
        return true;
    }
    // The clip is transient and referenced by nothing until EnablePreview takes it; a GC between
    // here and there would collect it out from under the fixture.
    const TStrongObjectPtr<UAnimSequence> ClipGuard(AmplifiedClip);

    FPreviewFixture Fixture;
    if (!Fixture.Build(/*bEnablePreview=*/true, AmplifiedClip))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.Failure);
        return true;
    }

    const TArray<double> Times = MakeSampleTimes(*Fixture.Animation, 9);
    const PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter = MakeScrubTimeSetter(*Fixture.Component);

    TArray<double> AssetRadii;
    TArray<FVector> AssetOrigins;
    TArray<double> PosedRadii;
    TArray<PinWrightAnimationPose::FPoseSample> Poses;
    for (const double TimeSeconds : Times)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (!TimeSetter(TimeSeconds, ErrorCode, ErrorMessage))
        {
            AddError(FString::Printf(TEXT("The time setter refused instant %.4f s: %s %s"),
                TimeSeconds, *ErrorCode, *ErrorMessage));
            return false;
        }

        FVector Origin = FVector::ZeroVector;
        double Radius = 0.0;
        TestTrue(TEXT("the mesh asset reports usable bounds"),
            ComputeAssetBounds(*Fixture.Mesh, Origin, Radius));
        AssetOrigins.Add(Origin);
        AssetRadii.Add(Radius);
        PosedRadii.Add(PosedBoneRadius(*Fixture.Component));

        PinWrightAnimationPose::FPoseSample Sample;
        PinWrightAnimationPose::SamplePose(Fixture.Component, Sample);
        Poses.Add(MoveTemp(Sample));
    }

    // Precondition 1: the pose actually moved between instants. If it did not, this test cannot tell
    // an asset-bounds implementation from a posed-bounds one and every assertion below is
    // decoration. A component whose animation system never evaluated at all is a HOST fact, not a
    // code fact, so it is reported as not-measured rather than failed — the invariance assertions
    // still run either way, because they read the asset and not the component.
    if (!Poses[0].bValid || !Poses[Poses.Num() / 2].bValid)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pose-never-evaluated"),
            TEXT("the preview component reported no component-space bone transforms, so this run could not "
                 "establish that the pose moves between instants"));
    }
    else
    {
        const PinWrightAnimationPose::FPoseDelta SpanDelta =
            PinWrightAnimationPose::ComparePose(Poses[0], Poses[Poses.Num() / 2]);
        TestTrue(TEXT("the fixture's pose is comparable across instants"), SpanDelta.bComparable);
        TestTrue(*FString::Printf(
                TEXT("the fixture's pose MOVES across the burst (moved bones=%d, max translation=%.4f cm) — ")
                TEXT("without motion this test cannot fail"),
                SpanDelta.MovedBoneCount, SpanDelta.MaxTranslationCm),
            PinWrightAnimationPose::PoseChanged(SpanDelta));
    }

    // Precondition 2: the POSED measurement differs enough between instants that a posed-bounds
    // implementation would be visible in the numbers. Reported rather than failed — the amplitude
    // belongs to the fixture, not to the code under test — but a run where it does not hold has NOT
    // measured decision 4 and must not read as a clean pass. With the authored clip this can only
    // trip if the component never evaluated the scale ramp at all, since the ramp's amplitude is
    // arithmetic; it is kept because "not measured" must stay distinguishable from "measured and
    // passed" whatever the reason.
    double MinPosed = TNumericLimits<double>::Max();
    double MaxPosed = 0.0;
    for (const double PosedRadius : PosedRadii)
    {
        MinPosed = FMath::Min(MinPosed, PosedRadius);
        MaxPosed = FMath::Max(MaxPosed, PosedRadius);
    }
    const double PosedSpread = (MinPosed > 0.0) ? ((MaxPosed - MinPosed) / MinPosed) : 0.0;
    if (!(PosedSpread > 0.10))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("posed-bounds-spread-too-small"),
            FString::Printf(
                TEXT("posed bone-bounds radius spans %.4f..%.4f cm (%.2f%%), under the 10%% this test needs "
                     "to distinguish asset bounds from posed bounds; the invariance assertion below still "
                     "ran but could not have failed on this fixture"),
                MinPosed, MaxPosed, PosedSpread * 100.0));
    }

    // THE assertion. Bit-identical, not "within a tolerance": these come from the same asset call at
    // every instant, so any difference at all means something read the component.
    const double Expected = static_cast<double>(Fixture.Mesh->GetBounds().SphereRadius);
    for (int32 Index = 0; Index < AssetRadii.Num(); ++Index)
    {
        TestTrue(*FString::Printf(
                TEXT("instant %d: bounds radius is bit-identical to USkeletalMesh::GetBounds() (%.17g vs %.17g)"),
                Index, AssetRadii[Index], Expected),
            AssetRadii[Index] == Expected);
        TestTrue(*FString::Printf(TEXT("instant %d: bounds origin is bit-identical to the asset's"), Index),
            AssetOrigins[Index] == AssetOrigins[0]);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The time setter

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationScrubPosesTheComponentTest,
    "PinWright.render.capture_subject_animation.ScrubPosesTheComponentNotJustTheClock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationScrubPosesTheComponentTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    // Both directions, because only the pair proves anything. SetPosition alone writes the proxy's
    // clock and nothing else; if the setter ever loses its TickAnimation / RefreshBoneTransforms
    // pair, every instant of every burst photographs the pose from the previous evaluation while the
    // response still reports the time it asked for.
    FPreviewFixture Fixture;
    if (!Fixture.Build())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.Failure);
        return true;
    }

    UAnimSingleNodeInstance* SingleNode = Fixture.Component->GetSingleNodeInstance();
    if (!SingleNode)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-single-node-instance"),
            TEXT("the preview component has no single-node instance, so there is no clock to write"));
        return true;
    }

    const PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter = MakeScrubTimeSetter(*Fixture.Component);
    FString ErrorCode;
    FString ErrorMessage;
    if (!TimeSetter(0.0, ErrorCode, ErrorMessage))
    {
        AddError(FString::Printf(TEXT("The time setter refused instant 0: %s %s"), *ErrorCode, *ErrorMessage));
        return false;
    }

    PinWrightAnimationPose::FPoseSample PoseAtZero;
    if (!PinWrightAnimationPose::SamplePose(Fixture.Component, PoseAtZero))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pose-never-evaluated"),
            TEXT("the component reported no component-space bone transforms, so the animation system never ran"));
        return true;
    }

    // Half the clip away: far enough that a walk cycle is unambiguously in a different pose.
    const double FarTime = static_cast<double>(Fixture.Animation->GetPlayLength()) * 0.5;

    // Direction 1: the clock alone moves nothing. Nothing ticks a preview scene inside a test, so
    // any pose change here would have to come from SetPosition itself.
    SingleNode->SetPosition(static_cast<float>(FarTime), /*bFireNotifies=*/false);
    PinWrightAnimationPose::FPoseSample PoseAfterBareSetPosition;
    PinWrightAnimationPose::SamplePose(Fixture.Component, PoseAfterBareSetPosition);
    const PinWrightAnimationPose::FPoseDelta BareDelta =
        PinWrightAnimationPose::ComparePose(PoseAtZero, PoseAfterBareSetPosition);
    TestFalse(*FString::Printf(
            TEXT("SetPosition alone does not pose the component (moved bones=%d) — if this ever becomes ")
            TEXT("true the setter's TickAnimation/RefreshBoneTransforms pair is no longer load-bearing"),
            BareDelta.MovedBoneCount),
        PinWrightAnimationPose::PoseChanged(BareDelta));

    // Direction 2: the production setter does pose it.
    if (!TimeSetter(FarTime, ErrorCode, ErrorMessage))
    {
        AddError(FString::Printf(TEXT("The time setter refused instant %.4f s: %s %s"),
            FarTime, *ErrorCode, *ErrorMessage));
        return false;
    }
    PinWrightAnimationPose::FPoseSample PoseAfterSetter;
    PinWrightAnimationPose::SamplePose(Fixture.Component, PoseAfterSetter);
    const PinWrightAnimationPose::FPoseDelta SetterDelta =
        PinWrightAnimationPose::ComparePose(PoseAtZero, PoseAfterSetter);
    TestTrue(TEXT("the poses before and after the setter are comparable"), SetterDelta.bComparable);
    TestTrue(*FString::Printf(
            TEXT("the time setter evaluates the pose (moved bones=%d, max translation=%.4f cm)"),
            SetterDelta.MovedBoneCount, SetterDelta.MaxTranslationCm),
        PinWrightAnimationPose::PoseChanged(SetterDelta));

    // The instance landed on the instant asked for. Read off the instance, never off the value
    // passed in: a position past the end of the asset is clamped, and the caller has to be able to
    // see that it was.
    TestEqual(TEXT("the single-node instance holds the requested time"),
        static_cast<double>(SingleNode->GetCurrentTime()), FarTime, 1.0e-3);
    TestFalse(TEXT("the preview is left stopped, so a Slate pump cannot advance it"),
        SingleNode->IsPlaying());

    // A non-finite instant is CALLER-FIXABLE and must not be reported as an unsupported subject —
    // the split CaptureSubject.h documents on FSubjectTimeSetter.
    FString NanCode;
    FString NanMessage;
    TestFalse(TEXT("a non-finite instant is refused"),
        TimeSetter(std::numeric_limits<double>::quiet_NaN(), NanCode, NanMessage));
    TestEqual(TEXT("a non-finite instant is INVALID_ARGUMENT, not UNSUPPORTED_ASSET_EDITOR"),
        NanCode, FString(InvalidArgumentCode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationBindPoseHasNoTimeAxisTest,
    "PinWright.render.capture_subject_animation.BindPosePreviewHasNoTimeAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationBindPoseHasNoTimeAxisTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    // Decision 6: a capability a subject cannot support is a TYPED REFUSAL, not silence. A bind-pose
    // preview asked for an instant must say it has no time axis; succeeding quietly would hand the
    // caller N identical images and call it a burst.
    FPreviewFixture Fixture;
    if (!Fixture.Build(/*bEnablePreview=*/false))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.Failure);
        return true;
    }
    Fixture.Component->EnablePreview(true, nullptr);

    const PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter = MakeScrubTimeSetter(*Fixture.Component);
    FString ErrorCode;
    FString ErrorMessage;
    const bool bScrubbed = TimeSetter(0.25, ErrorCode, ErrorMessage);

    TestFalse(TEXT("a bind-pose preview refuses a time rather than pretending to scrub"), bScrubbed);
    TestEqual(TEXT("the refusal is UNSUPPORTED_ASSET_EDITOR, not a generic failure"),
        ErrorCode, FString(UnsupportedCode));
    TestTrue(*FString::Printf(TEXT("the refusal names the missing time axis (got '%s')"), *ErrorMessage),
        ErrorMessage.Contains(TEXT("time axis")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The second bounds strategy: the sampled union

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationSampledUnionTest,
    "PinWright.render.capture_subject_animation.SampledUnionContainsEveryInstant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationSampledUnionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    FPreviewFixture Fixture;
    if (!Fixture.Build())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.Failure);
        return true;
    }

    const TArray<double> Times = MakeSampleTimes(*Fixture.Animation, 5);
    const PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter = MakeScrubTimeSetter(*Fixture.Component);

    // Failure direction first, and it needs no fixture amplitude: an empty sample list is a caller
    // asking for a union of nothing, which is asset bounds by another name.
    {
        FVector Origin = FVector::ZeroVector;
        double Radius = 0.0;
        FString ErrorCode;
        FString ErrorMessage;
        const bool bComputed = ComputeSampledPosedUnionBounds(*Fixture.Component, TimeSetter,
            TArrayView<const double>(), Origin, Radius, ErrorCode, ErrorMessage);
        TestFalse(TEXT("a union over zero instants is refused, not silently answered"), bComputed);
        TestEqual(TEXT("an empty sample list is INVALID_ARGUMENT"),
            ErrorCode, FString(InvalidArgumentCode));
    }

    // Per-instant bounds, measured the same way the union measures them, so the containment
    // assertion compares like with like.
    TArray<double> InstantRadii;
    for (const double TimeSeconds : Times)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (!TimeSetter(TimeSeconds, ErrorCode, ErrorMessage))
        {
            AddError(FString::Printf(TEXT("The time setter refused instant %.4f s: %s %s"),
                TimeSeconds, *ErrorCode, *ErrorMessage));
            return false;
        }
        const FTransform ComponentToWorld = Fixture.Component->GetComponentTransform();
        FBox Instant = Fixture.Component->CalcBounds(ComponentToWorld).GetBox();
        const FBox BoneBox = ComputePosedBoneBox(*Fixture.Component);
        if (BoneBox.IsValid)
        {
            Instant += BoneBox.TransformBy(ComponentToWorld);
        }
        InstantRadii.Add(Instant.IsValid ? Instant.GetExtent().Size() : 0.0);
    }

    FVector UnionOrigin = FVector::ZeroVector;
    double UnionRadius = 0.0;
    FString UnionErrorCode;
    FString UnionErrorMessage;
    if (!TestTrue(TEXT("the sampled-union pre-pass completes"),
            ComputeSampledPosedUnionBounds(*Fixture.Component, TimeSetter, Times,
                UnionOrigin, UnionRadius, UnionErrorCode, UnionErrorMessage)))
    {
        AddError(FString::Printf(TEXT("union refused: %s %s"), *UnionErrorCode, *UnionErrorMessage));
        return false;
    }

    // A union CONTAINS every instant it was built from. Strict growth is not asserted: that only
    // happens when two instants reach in opposite directions, which is a property of the fixture's
    // choreography rather than of this code.
    for (int32 Index = 0; Index < InstantRadii.Num(); ++Index)
    {
        TestTrue(*FString::Printf(
                TEXT("the union (%.4f) is at least as large as instant %d (%.4f)"),
                UnionRadius, Index, InstantRadii[Index]),
            UnionRadius >= InstantRadii[Index] - KINDA_SMALL_NUMBER);
    }

    // And it is a bounds pair a camera can be solved from, labelled so the response can say which of
    // the two correct strategies produced it. Both strategies are expressible on this kind; only
    // the label tells them apart, which is the whole content of plan decision 4.
    PinWrightCaptureSubject::FResolvedSubject Subject;
    Subject.BoundsSource = TEXT("assetBounds");
    FString ApplyErrorCode;
    FString ApplyErrorMessage;
    if (TestTrue(TEXT("the union can be written into a resolved subject"),
            ApplySampledUnionBounds(Subject, *Fixture.Component, TimeSetter, Times,
                ApplyErrorCode, ApplyErrorMessage)))
    {
        TestEqual(TEXT("the subject reports the union as its bounds source"),
            Subject.BoundsSource, FString(TEXT("sampledUnion")));
        TestTrue(TEXT("the union radius is positive"), Subject.BoundsRadius > 0.0);
    }

    // A union that cannot be computed leaves the subject's existing bounds alone rather than
    // zeroing them: a failed second opinion must not destroy the first one.
    PinWrightCaptureSubject::FResolvedSubject Untouched;
    Untouched.BoundsSource = TEXT("assetBounds");
    Untouched.BoundsRadius = 123.0;
    FString FailCode;
    FString FailMessage;
    TestFalse(TEXT("a union over zero instants fails"),
        ApplySampledUnionBounds(Untouched, *Fixture.Component, TimeSetter,
            TArrayView<const double>(), FailCode, FailMessage));
    TestEqual(TEXT("the failed union left the asset bounds in place"),
        Untouched.BoundsRadius, 123.0, 0.0);
    TestEqual(TEXT("the failed union left the bounds source in place"),
        Untouched.BoundsSource, FString(TEXT("assetBounds")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Restore discipline

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationPreviewStateRestoredTest,
    "PinWright.render.capture_subject_animation.PreviewInstanceStateIsRestored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationPreviewStateRestoredTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    // The provider poses somebody else's Persona tab. Leaving it scrubbed to a different frame with
    // a different animation loaded is the difference between a review verb and an editing one.
    FPreviewFixture Fixture;
    if (!Fixture.Build(/*bEnablePreview=*/false))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"), Fixture.Failure);
        return true;
    }

    const bool bEntryPreviewOn = Fixture.Component->IsPreviewOn();
    UAnimSingleNodeInstance* EntryInstance = Fixture.Component->GetSingleNodeInstance();
    UAnimationAsset* const EntryAsset = EntryInstance ? EntryInstance->GetAnimationAsset() : nullptr;
    const float EntryPosition = EntryInstance ? EntryInstance->GetCurrentTime() : 0.0f;
    const bool bEntryPlaying = EntryInstance ? EntryInstance->IsPlaying() : false;
    const bool bEntryLooping = EntryInstance ? EntryInstance->IsLooping() : true;

    FPreviewInstanceState Saved;
    Saved.CaptureFrom(*Fixture.Component);

    // Do to the component exactly what the provider does, and then some: load the animation, stop
    // it, and scrub away from wherever it was.
    Fixture.Component->EnablePreview(true, Fixture.Animation);
    if (UAnimSingleNodeInstance* SingleNode = Fixture.Component->GetSingleNodeInstance())
    {
        SingleNode->SetPlaying(false);
        SingleNode->SetLooping(false);
        SingleNode->SetPosition(Fixture.Animation->GetPlayLength() * 0.5f, /*bFireNotifies=*/false);
    }

    Saved.RestoreTo(*Fixture.Component);

    TestTrue(TEXT("preview mode is put back to how it was found"),
        Fixture.Component->IsPreviewOn() == bEntryPreviewOn);
    UAnimSingleNodeInstance* RestoredInstance = Fixture.Component->GetSingleNodeInstance();
    if (EntryInstance)
    {
        if (TestNotNull(TEXT("the single-node instance survives the restore"), RestoredInstance))
        {
            TestTrue(TEXT("the animation asset is put back"),
                RestoredInstance->GetAnimationAsset() == EntryAsset);
            TestEqual(TEXT("the playhead is put back"),
                static_cast<double>(RestoredInstance->GetCurrentTime()),
                static_cast<double>(EntryPosition), 1.0e-4);
            TestTrue(TEXT("the play state is put back"), RestoredInstance->IsPlaying() == bEntryPlaying);
            TestTrue(TEXT("the loop state is put back"), RestoredInstance->IsLooping() == bEntryLooping);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Preview mesh resolution

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationPreviewMeshChainTest,
    "PinWright.render.capture_subject_animation.PreviewMeshResolutionFollowsPersonaChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationPreviewMeshChainTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;
    using namespace PinWrightCaptureSubjectAnimation;

    UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, FixtureAnimationPath);
    if (!Animation)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("engine animation fixture missing: %s"), FixtureAnimationPath));
        return true;
    }

    EPreviewMeshSource Source = EPreviewMeshSource::None;
    USkeletalMesh* Resolved = ResolvePreviewMesh(Animation, Source);
    if (!TestNotNull(TEXT("an animation asset resolves to a preview mesh"), Resolved))
    {
        return false;
    }
    TestTrue(TEXT("the resolution names a source rather than reporting none"),
        Source != EPreviewMeshSource::None);
    TestTrue(*FString::Printf(TEXT("the resolved mesh is bound to the animation's skeleton (source=%s)"),
            PreviewMeshSourceName(Source)),
        Resolved->GetSkeleton() == Animation->GetSkeleton());

    // The engine finding this chain exists for, asserted as a sentinel: on UE 5.8
    // UAnimationAsset::GetPreviewMesh IGNORES bFindIfNotSet (AnimationAsset.cpp:400-415), so the two
    // calls agree and the "find one if not set" behaviour has to come from the skeleton. If a later
    // engine implements the fallback, this fires and the chain above can be collapsed.
    TestTrue(TEXT("UAnimationAsset::GetPreviewMesh(true) still ignores bFindIfNotSet"),
        Animation->GetPreviewMesh(/*bFindIfNotSet=*/true) == Animation->GetPreviewMesh());

    // Null in, none out — no crash, no guessing.
    EPreviewMeshSource NullSource = EPreviewMeshSource::CompatibleMesh;
    TestNull(TEXT("a null animation resolves to no mesh"), ResolvePreviewMesh(nullptr, NullSource));
    TestTrue(TEXT("a null animation reports 'none' as its source"),
        NullSource == EPreviewMeshSource::None);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The end-to-end path, through the registry and the shared asset-editor walk

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAnimationResolveOpensPersonaTest,
    "PinWright.render.capture_subject_animation.ResolveOpensThePersonaPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAnimationResolveOpensPersonaTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectAnimationTests;

    if (!LoadObject<UAnimSequence>(nullptr, FixtureAnimationPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("engine animation fixture missing: %s"), FixtureAnimationPath));
        return true;
    }

    // Declared FIRST so it runs LAST — after the probe's FResolvedSubject destructor has already run
    // its release. It catches anything the three-state close rule deliberately left open, because an
    // asset editor still open at editor exit faults in the toolkit destructor.
    ON_SCOPE_EXIT { CloseEditorFor(FixtureAnimationPath); };

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::Animation;
    Request.AssetPath = FixtureAnimationPath;

    FResolveProbe Probe;
    Probe.bResolved = PinWrightCaptureSubject::Resolve(
        Request, Probe.Resolved, Probe.TimeSetter, Probe.ErrCode, Probe.ErrMsg);

    if (!Probe.bResolved)
    {
        // A typed host limitation is a skip; anything else is a defect. "The resolve silently did
        // nothing" cannot pass as either.
        if (!TestTrue(*FString::Printf(
                    TEXT("an unresolved animation subject reports a typed host-limited code (got '%s': %s)"),
                    *Probe.ErrCode, *Probe.ErrMsg),
                IsHostLimitedCode(Probe.ErrCode)))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-preview-viewport"),
            FString::Printf(TEXT("Resolve could not realise a Persona preview viewport on this host: %s %s"),
                *Probe.ErrCode, *Probe.ErrMsg));
        return true;
    }

    TestTrue(TEXT("a resolved animation subject carries a viewport client"),
        Probe.Resolved.ViewportClient != nullptr);
    TestTrue(TEXT("a resolved animation subject carries a scene viewport"),
        Probe.Resolved.SceneViewport.IsValid());
    TestTrue(TEXT("the resolved subject names the animation kind"),
        Probe.Resolved.Kind == PinWrightCaptureSubject::ESubjectKind::Animation);
    TestEqual(TEXT("the capture source is the Persona preview viewport"),
        Probe.Resolved.CaptureSource, FString(TEXT("personaPreviewViewport")));
    TestEqual(TEXT("bounds come from the asset, and the source says so"),
        Probe.Resolved.BoundsSource, FString(TEXT("assetBounds")));
    TestTrue(TEXT("the resolved subject carries a positive bounds radius"),
        Probe.Resolved.BoundsRadius > 0.0);
    TestTrue(TEXT("an animation subject reports a time axis"), Probe.Resolved.bTimeSupported);
    TestTrue(TEXT("scrubbing is reported as reproducible, unlike a particle simulation"),
        Probe.Resolved.bTimeReproducible);

    // The time setter that came back drives the real preview.
    FString SetterCode;
    FString SetterMessage;
    TestTrue(*FString::Printf(TEXT("the resolved time setter poses the subject (%s %s)"),
            *SetterCode, *SetterMessage),
        Probe.TimeSetter(0.0, SetterCode, SetterMessage));

    // Release explicitly rather than at scope exit, so `assetEditorClosed` is MEASURED here rather
    // than snapshotted before the close. This call opened the window, so the default close rule must
    // dispose of it.
    //
    // THE CLOSE IS QUEUED, NOT PERFORMED, and this test asserts the queue plus the flush rather
    // than a synchronous `bEditorClosed`. CloseAssetEditor no longer destroys the toolkit inside a
    // release path - doing so faults with an access violation and takes the editor down (board
    // B-capture-asset-preview-no-safe-close-mode) - so the honest measurement at this instant is
    // "a close is queued for this asset", and the window is gone after the queue runs.
    const FString ProbeAssetPath = Probe.Resolved.AssetPath;
    PinWrightCaptureSubject::ReleaseSubject(Probe.Resolved);
    if (!Probe.Resolved.bEditorWasAlreadyOpen)
    {
        TestTrue(TEXT("a window this call opened is disposed of on release, either closed outright "
                      "or queued for the next tick"),
            Probe.Resolved.bEditorClosed ||
                PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(ProbeAssetPath));
        PinWrightCaptureSubject::FlushDeferredAssetEditorCloses();
        TestFalse(TEXT("...and nothing is left queued for it afterwards"),
            PinWrightCaptureSubject::HasPendingDeferredAssetEditorClose(ProbeAssetPath));
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-already-open"),
            TEXT("the fixture's asset editor was already open before this test, so the close rule "
                 "deliberately left it open and the close could not be measured"));
    }
    return true;
}
