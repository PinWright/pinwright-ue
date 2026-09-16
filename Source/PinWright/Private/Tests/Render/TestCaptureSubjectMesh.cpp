// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for the two mesh subject providers (Handlers/Render/CaptureSubjectProviders_Mesh.cpp).
//
// WHAT IS DELIBERATELY NOT ASSERTED HERE: pixels. Not because the preview scene is dark - it is
// not, and this header used to claim otherwise.
//
// WHAT THIS FILE USED TO SAY, AND WHY IT IS RETRACTED. It said FAdvancedPreviewScene is not lit
// under `UnrealEditor-Cmd -unattended`, and bimodally so across runs - the same fixture measuring
// meanLuminance 0.0978 on one run and 0.0078 on another, with six stops of ev100 producing
// byte-identical frames. That was believed and acted on for weeks. Commit bcc334e8 ("Fix the
// view-mode crash and the capture test that never compared two files") disproved it: the
// auto-generated screenshot filename carries a ONE-SECOND timestamp while a capture takes ~60 ms,
// so the three captures behind those numbers overwrote each other and every "comparison" was one
// file against itself. Re-measured the same day, the same fixture reads meanLuminance 0.3644
// headless and 0.3644 interactively - there is no dim mode and no black mode. See the bullet
// marked `RETRACTED 2026-08-21` in docs/lessons.md, which keeps the withdrawn text verbatim.
//
// The reason pixels stay out of THIS file is narrower and still stands: these providers own
// subject ACQUISITION, not capture, so no frame is produced here to assert on. Every assertion
// below is on the resolved descriptor and on the typed refusals. What survives from the retracted
// lesson is the rule it was replaced by - any assertion comparing two captures must pass an
// explicit `filename` or assert the two returned `path` values differ, or it cannot fail.
//
// THE CONTRACT ASSERTIONS DO NOT DEPEND ON A LIVE PREVIEW VIEWPORT. ResolveMeshSubjectFacts
// decides the asset class, the bounds and the presence of a time axis from the request alone and
// opens nothing, so the two acceptance criteria for this chunk — the Static Mesh no-time-axis
// refusal and skeletal bounds coming from the asset — are measured deterministically on every
// host. The end-to-end Resolve() path is asserted in addition, and when THAT cannot realise a
// preview viewport the test says NOT MEASURED for that half rather than passing quietly.
//
// NOTHING BELOW DEREFERENCES FResolvedSubject::ViewportClient. Comparing the pointer to null is
// defined whether or not the RAII release has already run; reading through it would not be.
//
// EVERY TEST THAT CAN OPEN AN ASSET EDITOR CLOSES IT. FResolvedSubject releases in its destructor,
// and the scope guards below are declared FIRST so they run LAST — after that destructor — and
// catch anything the release rule deliberately left open. An asset editor still open when the
// editor exits faults in ~FStaticMeshEditor (UE 5.8 StaticMeshEditor.cpp:271), which would turn a
// passing test into a crashed suite at shutdown.
#include "Misc/AutomationTest.h"

#include "Handlers/Render/CaptureSubject.h"
#include "Handlers/Render/CaptureSubjectProviders_Mesh.h"

#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Compat/EngineVersionCompat.h"

// FAutomationTestBase::TestEqualSensitive arrived in UE 5.5. Every use below compares two
// FStrings case-sensitively, which is exactly what the 5.5 member does, so on 5.4 the call
// spells that comparison out. A macro rather than a helper because the calls are unqualified
// member calls; the #undef at the end of the file keeps it out of the sibling TUs a Unity blob
// merges after this one.
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define TestEqualSensitive(What, Actual, Expected) \
    TestTrue(What, FString(Actual).Equals(FString(Expected), ESearchCase::CaseSensitive))
#endif

namespace
{
    // File-unique names so Unity merges cannot ODR-collide these with the same-shaped helpers in
    // sibling Tests/Render/*.cpp files.
    const TCHAR* const PWMeshSubjectCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
    const TCHAR* const PWMeshSubjectSkeletalCubePath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
    const TCHAR* const PWMeshSubjectMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");

    // Asserted as a LITERAL, never as ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR: what matters is the
    // wire string a caller sees. Comparing the provider's constant against the same constant would
    // still pass if the constant's value changed, and would still pass if the refusal had never
    // been implemented and the dispatcher's UNKNOWN_PARAMS came back instead.
    const TCHAR* const PWMeshSubjectUnsupportedCode = TEXT("UNSUPPORTED_ASSET_EDITOR");

    FString PWMeshSubjectAbsentPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/MCP_MeshSubjectAbsent/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // FResolvedSubject is non-copyable and releases in its destructor, so it is held in place here
    // rather than returned by value.
    struct PWMeshSubjectProbe
    {
        bool bResolved = false;
        FString ErrCode;
        FString ErrMsg;
        PinWrightCaptureSubject::FResolvedSubject Resolved;
        PinWrightCaptureSubject::FSubjectTimeSetter TimeSetter;
    };

    void PWMeshSubjectResolve(const PinWrightCaptureSubject::FSubjectRequest& Request,
        PWMeshSubjectProbe& Probe)
    {
        Probe.bResolved = PinWrightCaptureSubject::Resolve(
            Request, Probe.Resolved, Probe.TimeSetter, Probe.ErrCode, Probe.ErrMsg);
    }

    bool PWMeshSubjectEditorIsOpenFor(const TCHAR* AssetPath)
    {
        if (!GEditor)
        {
            return false;
        }
        UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        UObject* Asset = LoadObject<UObject>(nullptr, AssetPath);
        return Subsystem && Asset && Subsystem->FindEditorForAsset(Asset, false) != nullptr;
    }

    void PWMeshSubjectCloseEditorFor(const TCHAR* AssetPath)
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

    // Codes that mean "this host could not realise a preview viewport", as distinct from "the
    // provider rejected the request". Anything outside this set on an otherwise-valid request is a
    // hard failure, so "the resolve silently did nothing" cannot pass as a skip.
    bool PWMeshSubjectIsHostLimitedCode(const FString& Code)
    {
        return Code == TEXT("PREVIEW_VIEWPORT_NOT_FOUND")
            || Code == TEXT("PREVIEW_NOT_FOUND")
            || Code == TEXT("OPEN_FAILED")
            || Code == TEXT("SUBSYSTEM_MISSING")
            || Code == TEXT("SKELETAL_MESH_NOT_FOUND")
            || Code == TEXT("EDITOR_NOT_AVAILABLE")
            || Code == TEXT("UNSUPPORTED_ASSET_EDITOR");
    }
}

// ---------------------------------------------------------------------------------------------
// Registration. A provider file that fails to reach RegisterProvider is invisible: every other
// test in this file would report "no provider for kind" and read as an environment problem.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshKindsAreRegisteredTest,
    "PinWright.render.capture_subject.MeshKindsAreRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshKindsAreRegisteredTest::RunTest(const FString& Parameters)
{
    const PinWrightCaptureSubject::FSubjectProvider* StaticMeshProvider =
        PinWrightCaptureSubject::FindProvider(PinWrightCaptureSubject::ESubjectKind::StaticMesh);
    const PinWrightCaptureSubject::FSubjectProvider* SkeletalMeshProvider =
        PinWrightCaptureSubject::FindProvider(PinWrightCaptureSubject::ESubjectKind::SkeletalMesh);

    if (TestNotNull(TEXT("A provider is registered for the staticMesh kind"), StaticMeshProvider))
    {
        TestTrue(TEXT("The staticMesh provider carries an Acquire"),
            static_cast<bool>(StaticMeshProvider->Acquire));
        TestTrue(TEXT("The staticMesh provider carries a Release"),
            static_cast<bool>(StaticMeshProvider->Release));
    }
    if (TestNotNull(TEXT("A provider is registered for the skeletalMesh kind"), SkeletalMeshProvider))
    {
        TestTrue(TEXT("The skeletalMesh provider carries an Acquire"),
            static_cast<bool>(SkeletalMeshProvider->Acquire));
        TestTrue(TEXT("The skeletalMesh provider carries a Release"),
            static_cast<bool>(SkeletalMeshProvider->Release));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Decision 6: a Static Mesh asked for a time series answers with a TYPED code naming the missing
// time axis, not with the dispatcher's UNKNOWN_PARAMS arg gate.
//
// Unable to fail if it asserted only that resolution failed — a typo in the asset path fails too.
// So it asserts the exact wire code, that the code is NOT UNKNOWN_PARAMS, that the message names
// the axis, and that no asset editor was opened to produce any of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshStaticHasNoTimeAxisTest,
    "PinWright.render.capture_subject.StaticMeshHasNoTimeAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshStaticHasNoTimeAxisTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectCubePath);

    const bool bEditorWasOpenBefore = PWMeshSubjectEditorIsOpenFor(PWMeshSubjectCubePath);

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
    Request.AssetPath = PWMeshSubjectCubePath;
    // Naming an animation is how a caller asks a subject for a time axis. On a Static Mesh that is
    // wrong from the request alone, so it must be refused before anything is opened.
    Request.AnimationPath = PWMeshSubjectAbsentPath(TEXT("Anim"));

    // ---- Deterministic half: no editor, no Slate, no host dependence.
    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString FactsCode;
    FString FactsMessage;
    const bool bFacts = PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(
        Request, Facts, FactsCode, FactsMessage);

    TestFalse(TEXT("A Static Mesh with an animation named does not resolve its facts"), bFacts);
    TestEqualSensitive(TEXT("The refusal is UNSUPPORTED_ASSET_EDITOR"), FactsCode,
        FString(PWMeshSubjectUnsupportedCode));
    TestNotEqual(TEXT("The refusal is NOT the dispatcher's UNKNOWN_PARAMS arg gate"), FactsCode,
        FString(TEXT("UNKNOWN_PARAMS")));
    TestTrue(
        FString::Printf(TEXT("The message names the missing time axis (got: %s)"), *FactsMessage),
        FactsMessage.Contains(TEXT("time")));

    // ---- The same refusal through the registered provider, which is the path a verb takes.
    PWMeshSubjectProbe Probe;
    PWMeshSubjectResolve(Request, Probe);
    TestFalse(TEXT("Resolve refuses the same request"), Probe.bResolved);
    TestEqualSensitive(TEXT("Resolve's refusal is the same code"), Probe.ErrCode,
        FString(PWMeshSubjectUnsupportedCode));

    // The refusal is decidable from the request, so nothing should have been opened to reach it.
    if (!bEditorWasOpenBefore)
    {
        TestFalse(TEXT("No Static Mesh editor was opened to produce the refusal"),
            PWMeshSubjectEditorIsOpenFor(PWMeshSubjectCubePath));
    }
    else
    {
        AddInfo(TEXT("A Static Mesh editor for the fixture was already open before this test; the "
                     "no-window-opened half is NOT MEASURED on this run."));
    }
    return true;
}

// The same refusal reached the other way: the caller never named an animation, took the time
// setter the provider handed back, and called it. A setter that returned true — or that was empty
// — would let a time series be silently photographed as one repeated still.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshStaticTimeSetterRefusesTest,
    "PinWright.render.capture_subject.StaticMeshTimeSetterRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshStaticTimeSetterRefusesTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectCubePath);
    // Declared first so it runs last — after the probe's destructor has released the subject.
    ON_SCOPE_EXIT { PWMeshSubjectCloseEditorFor(PWMeshSubjectCubePath); };

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
    Request.AssetPath = PWMeshSubjectCubePath;

    // The time axis is absent whether or not a viewport can be realised, so this half is asserted
    // without opening anything.
    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString FactsCode;
    FString FactsMessage;
    // Called first, into a named bool: the message is built from FactsCode/FactsMessage, and the
    // evaluation order of two arguments to the same call is unspecified in C++.
    const bool bFactsResolved = PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(
        Request, Facts, FactsCode, FactsMessage);
    if (TestTrue(FString::Printf(TEXT("The Static Mesh subject's facts resolve (%s: %s)"),
            *FactsCode, *FactsMessage),
        bFactsResolved))
    {
        TestFalse(TEXT("A Static Mesh reports no time axis"), Facts.bTimeSupported);
        TestEqualSensitive(TEXT("captureSource names the Static Mesh editor preview"),
            Facts.CaptureSource, FString(TEXT("staticMeshEditorPreview")));
    }

    PWMeshSubjectProbe Probe;
    PWMeshSubjectResolve(Request, Probe);

    if (!Probe.bResolved && !PWMeshSubjectIsHostLimitedCode(Probe.ErrCode))
    {
        AddError(FString::Printf(
            TEXT("Resolving a Static Mesh subject failed with an unexpected code '%s': %s"),
            *Probe.ErrCode, *Probe.ErrMsg));
        return false;
    }
    if (!Probe.bResolved)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preview-viewport-unresolved"), FString::Printf(
            TEXT("the preview-viewport half is NOT MEASURED on this host (%s). The time-setter assertions below still run — the setter is "
                 "installed on every path."),
            *Probe.ErrCode));
    }
    else
    {
        TestFalse(TEXT("A resolved Static Mesh subject reports no time axis"),
            Probe.Resolved.bTimeSupported);
        TestNotNull(TEXT("A resolved subject carries a viewport client"),
            Probe.Resolved.ViewportClient);
    }

    FString SetterCode;
    FString SetterMessage;
    TestTrue(TEXT("A time setter was handed back even though the kind has no time axis"),
        static_cast<bool>(Probe.TimeSetter));
    const bool bSetterAccepted = Probe.TimeSetter
        ? Probe.TimeSetter(0.0, SetterCode, SetterMessage)
        : true;
    TestFalse(TEXT("The time setter refuses instead of silently accepting the instant"), bSetterAccepted);
    TestEqualSensitive(TEXT("The setter's refusal is UNSUPPORTED_ASSET_EDITOR"), SetterCode,
        FString(PWMeshSubjectUnsupportedCode));
    TestTrue(
        FString::Printf(TEXT("The setter's message names the missing time axis (got: %s)"), *SetterMessage),
        SetterMessage.Contains(TEXT("time")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Bounds. The acceptance criterion for this chunk: the expected value is read off the fixture AT
// ASSERT TIME rather than written as a literal, so a provider that started framing from the posed
// component — or from anything else — fails here instead of quietly moving the camera.
//
// Unable to fail if both sides were zero: an asset with empty bounds would make `0 == 0` pass and
// prove nothing, so the fixture's own radius is asserted positive as a precondition first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshSkeletalBoundsFromAssetTest,
    "PinWright.render.capture_subject.SkeletalMeshBoundsComeFromTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshSkeletalBoundsFromAssetTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectSkeletalCubePath);
    ON_SCOPE_EXIT { PWMeshSubjectCloseEditorFor(PWMeshSubjectSkeletalCubePath); };

    USkeletalMesh* Fixture = LoadObject<USkeletalMesh>(nullptr, PWMeshSubjectSkeletalCubePath);
    if (!TestNotNull(TEXT("The Skeletal Mesh fixture loads"), Fixture))
    {
        return false;
    }

    // Read at assert time from the fixture, never a literal.
    const FBoxSphereBounds ExpectedBounds = Fixture->GetBounds();
    const double ExpectedRadius = static_cast<double>(ExpectedBounds.SphereRadius);
    if (!TestTrue(
        FString::Printf(TEXT("Precondition: the fixture has non-empty bounds (radius %f)"), ExpectedRadius),
        ExpectedRadius > 0.0))
    {
        return false;
    }

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::SkeletalMesh;
    Request.AssetPath = PWMeshSubjectSkeletalCubePath;

    // ---- Deterministic half.
    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString FactsCode;
    FString FactsMessage;
    // Called first, into a named bool: the message is built from FactsCode/FactsMessage, and the
    // evaluation order of two arguments to the same call is unspecified in C++.
    const bool bFactsResolved = PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(
        Request, Facts, FactsCode, FactsMessage);
    if (!TestTrue(FString::Printf(TEXT("The Skeletal Mesh subject's facts resolve (%s: %s)"),
            *FactsCode, *FactsMessage),
        bFactsResolved))
    {
        return false;
    }
    TestEqualSensitive(TEXT("boundsSource names the asset, not the posed component"),
        Facts.BoundsSource, FString(TEXT("assetBounds")));
    TestTrue(
        FString::Printf(TEXT("Bounds radius equals USkeletalMesh::GetBounds().SphereRadius (%f vs %f)"),
            Facts.BoundsRadius, ExpectedRadius),
        Facts.BoundsRadius == ExpectedRadius);
    TestTrue(
        FString::Printf(TEXT("Bounds origin equals USkeletalMesh::GetBounds().Origin (%s vs %s)"),
            *Facts.BoundsOrigin.ToString(), *ExpectedBounds.Origin.ToString()),
        Facts.BoundsOrigin == ExpectedBounds.Origin);
    TestEqualSensitive(TEXT("captureSource names the Persona preview viewport"),
        Facts.CaptureSource, FString(TEXT("personaPreviewViewport")));

    // ---- The same values must survive into the resolved descriptor a verb reads.
    PWMeshSubjectProbe Probe;
    PWMeshSubjectResolve(Request, Probe);

    if (!Probe.bResolved && !PWMeshSubjectIsHostLimitedCode(Probe.ErrCode))
    {
        AddError(FString::Printf(
            TEXT("Resolving a Skeletal Mesh subject failed with an unexpected code '%s': %s"),
            *Probe.ErrCode, *Probe.ErrMsg));
        return false;
    }
    if (!Probe.bResolved)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preview-viewport-unresolved"), FString::Printf(
            TEXT("the resolved-descriptor half is NOT MEASURED on this host (%s). The bounds contract itself was measured above."),
            *Probe.ErrCode));
        return true;
    }

    TestEqualSensitive(TEXT("The resolved subject's boundsSource names the asset"),
        Probe.Resolved.BoundsSource, FString(TEXT("assetBounds")));
    TestTrue(
        FString::Printf(TEXT("The resolved BoundsRadius equals the asset's (%f vs %f)"),
            Probe.Resolved.BoundsRadius, ExpectedRadius),
        Probe.Resolved.BoundsRadius == ExpectedRadius);
    TestTrue(TEXT("The resolved subject reports the skeletalMesh kind"),
        Probe.Resolved.Kind == PinWrightCaptureSubject::ESubjectKind::SkeletalMesh);
    return true;
}

// The Static Mesh half of the same contract, plus the rule that `subject.radius` is scoped to the
// world/point kinds and must not displace an asset kind's own bounds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshStaticBoundsFromAssetTest,
    "PinWright.render.capture_subject.StaticMeshBoundsComeFromTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshStaticBoundsFromAssetTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectCubePath);

    UStaticMesh* Fixture = LoadObject<UStaticMesh>(nullptr, PWMeshSubjectCubePath);
    if (!TestNotNull(TEXT("The Static Mesh fixture loads"), Fixture))
    {
        return false;
    }

    const FBoxSphereBounds ExpectedBounds = Fixture->GetBounds();
    const double ExpectedRadius = static_cast<double>(ExpectedBounds.SphereRadius);
    if (!TestTrue(
        FString::Printf(TEXT("Precondition: the fixture has non-empty bounds (radius %f)"), ExpectedRadius),
        ExpectedRadius > 0.0))
    {
        return false;
    }

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
    Request.AssetPath = PWMeshSubjectCubePath;
    // Deliberately set, and deliberately expected to be ignored.
    Request.Radius = 12345.0f;
    Request.bRadiusProvided = true;

    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString FactsCode;
    FString FactsMessage;
    // Called first, into a named bool: the message is built from FactsCode/FactsMessage, and the
    // evaluation order of two arguments to the same call is unspecified in C++.
    const bool bFactsResolved = PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(
        Request, Facts, FactsCode, FactsMessage);
    if (!TestTrue(FString::Printf(TEXT("The Static Mesh subject's facts resolve (%s: %s)"),
            *FactsCode, *FactsMessage),
        bFactsResolved))
    {
        return false;
    }

    TestEqualSensitive(TEXT("boundsSource names the asset"), Facts.BoundsSource,
        FString(TEXT("assetBounds")));
    TestTrue(
        FString::Printf(TEXT("Bounds radius equals UStaticMesh::GetBounds().SphereRadius (%f vs %f)"),
            Facts.BoundsRadius, ExpectedRadius),
        Facts.BoundsRadius == ExpectedRadius);
    TestTrue(
        FString::Printf(TEXT("subject.radius did not displace the asset bounds (got %f)"),
            Facts.BoundsRadius),
        Facts.BoundsRadius != 12345.0);
    return true;
}

// ---------------------------------------------------------------------------------------------
// A Skeletal Mesh has a time axis only when the request names an animation. Without one it is a
// bind-pose still, and the setter must say so rather than accept an instant it cannot reach.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshSkeletalNoAnimationNoTimeAxisTest,
    "PinWright.render.capture_subject.SkeletalMeshWithoutAnimationHasNoTimeAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshSkeletalNoAnimationNoTimeAxisTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectSkeletalCubePath);
    ON_SCOPE_EXIT { PWMeshSubjectCloseEditorFor(PWMeshSubjectSkeletalCubePath); };

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::SkeletalMesh;
    Request.AssetPath = PWMeshSubjectSkeletalCubePath;

    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString FactsCode;
    FString FactsMessage;
    // Called first, into a named bool: the message is built from FactsCode/FactsMessage, and the
    // evaluation order of two arguments to the same call is unspecified in C++.
    const bool bFactsResolved = PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(
        Request, Facts, FactsCode, FactsMessage);
    if (TestTrue(FString::Printf(TEXT("The Skeletal Mesh subject's facts resolve (%s: %s)"),
            *FactsCode, *FactsMessage),
        bFactsResolved))
    {
        TestFalse(TEXT("A Skeletal Mesh with no animation reports no time axis"),
            Facts.bTimeSupported);
        TestNull(TEXT("No animation asset was resolved"), Facts.AnimationAsset);
    }

    PWMeshSubjectProbe Probe;
    PWMeshSubjectResolve(Request, Probe);
    if (!Probe.bResolved && !PWMeshSubjectIsHostLimitedCode(Probe.ErrCode))
    {
        AddError(FString::Printf(
            TEXT("Resolving a Skeletal Mesh subject failed with an unexpected code '%s': %s"),
            *Probe.ErrCode, *Probe.ErrMsg));
        return false;
    }
    if (Probe.bResolved)
    {
        TestFalse(TEXT("The resolved subject reports no time axis"), Probe.Resolved.bTimeSupported);
    }

    FString SetterCode;
    FString SetterMessage;
    TestTrue(TEXT("A time setter was handed back"), static_cast<bool>(Probe.TimeSetter));
    const bool bSetterAccepted = Probe.TimeSetter
        ? Probe.TimeSetter(0.25, SetterCode, SetterMessage)
        : true;
    TestFalse(TEXT("The time setter refuses when no animation was named"), bSetterAccepted);
    TestEqualSensitive(TEXT("The refusal is UNSUPPORTED_ASSET_EDITOR"), SetterCode,
        FString(PWMeshSubjectUnsupportedCode));
    TestTrue(
        FString::Printf(TEXT("The message names the missing time axis (got: %s)"), *SetterMessage),
        SetterMessage.Contains(TEXT("time")));
    // The message has to name what WOULD give the subject a time axis, or the caller learns only
    // that it failed.
    TestTrue(
        FString::Printf(TEXT("The message names the argument that supplies one (got: %s)"), *SetterMessage),
        SetterMessage.Contains(TEXT("animation")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Class gates, both directions. These messages are the only discovery path between the kinds: an
// agent told "Static Mesh only", with nothing naming the kind that does handle skinned assets,
// concludes isolated skinned capture does not exist and goes off to dirty a level instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshStaticRefusesNonMeshTest,
    "PinWright.render.capture_subject.StaticMeshKindRefusesANonMeshAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshStaticRefusesNonMeshTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectMaterialPath);

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
    Request.AssetPath = PWMeshSubjectMaterialPath;

    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString Code;
    FString Message;
    TestFalse(TEXT("A Material does not resolve as a staticMesh subject"),
        PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(Request, Facts, Code, Message));
    TestEqualSensitive(TEXT("The refusal is UNSUPPORTED_ASSET_EDITOR"), Code,
        FString(PWMeshSubjectUnsupportedCode));

    // Read off the fixture at assert time rather than written as a literal, so a message that
    // reports a hardcoded or wrong class name fails here. Case-sensitive, or the class name would
    // also be satisfied by the lower-cased kind tokens in the same sentence.
    UObject* Fixture = LoadObject<UObject>(nullptr, PWMeshSubjectMaterialPath);
    if (TestNotNull(TEXT("The Material fixture loads"), Fixture))
    {
        const FString ActualClassName = Fixture->GetClass()->GetName();
        TestTrue(
            FString::Printf(TEXT("The message names the class it actually got ('%s'; got: %s)"),
                *ActualClassName, *Message),
            Message.Contains(ActualClassName, ESearchCase::CaseSensitive));
    }
    TestTrue(
        FString::Printf(TEXT("The message names a kind that could serve the asset (got: %s)"), *Message),
        Message.Contains(TEXT("skeletalMesh"), ESearchCase::CaseSensitive) ||
        Message.Contains(TEXT("animation"), ESearchCase::CaseSensitive));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshSkeletalRefusesStaticMeshTest,
    "PinWright.render.capture_subject.SkeletalMeshKindRefusesAStaticMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshSkeletalRefusesStaticMeshTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectCubePath);

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::SkeletalMesh;
    Request.AssetPath = PWMeshSubjectCubePath;

    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString Code;
    FString Message;
    TestFalse(TEXT("A Static Mesh does not resolve as a skeletalMesh subject"),
        PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(Request, Facts, Code, Message));
    TestEqualSensitive(TEXT("The refusal is UNSUPPORTED_ASSET_EDITOR"), Code,
        FString(PWMeshSubjectUnsupportedCode));

    UObject* Fixture = LoadObject<UObject>(nullptr, PWMeshSubjectCubePath);
    if (TestNotNull(TEXT("The Static Mesh fixture loads"), Fixture))
    {
        const FString ActualClassName = Fixture->GetClass()->GetName();
        TestTrue(
            FString::Printf(TEXT("The message names the class it actually got ('%s'; got: %s)"),
                *ActualClassName, *Message),
            Message.Contains(ActualClassName, ESearchCase::CaseSensitive));
    }
    // Case-sensitive: the kind token is 'staticMesh', which is NOT the class name 'StaticMesh'
    // asserted above. A case-insensitive match here would be satisfied by the class name alone and
    // would not prove the message names the kind that serves the asset.
    TestTrue(
        FString::Printf(TEXT("The message names the kind that does serve it (got: %s)"), *Message),
        Message.Contains(TEXT("staticMesh"), ESearchCase::CaseSensitive));
    return true;
}

// A named animation that does not exist is refused by its own code before anything is opened —
// ANIMATION_NOT_FOUND, not the mesh-kind refusal and not a silently ignored argument.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshSkeletalMissingAnimationTest,
    "PinWright.render.capture_subject.SkeletalMeshMissingAnimationIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshSkeletalMissingAnimationTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectSkeletalCubePath);

    const bool bEditorWasOpenBefore = PWMeshSubjectEditorIsOpenFor(PWMeshSubjectSkeletalCubePath);

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::SkeletalMesh;
    Request.AssetPath = PWMeshSubjectSkeletalCubePath;
    Request.AnimationPath = PWMeshSubjectAbsentPath(TEXT("Anim"));

    PinWrightCaptureSubjectMesh::FMeshSubjectFacts Facts;
    FString Code;
    FString Message;
    TestFalse(TEXT("A missing animation does not resolve"),
        PinWrightCaptureSubjectMesh::ResolveMeshSubjectFacts(Request, Facts, Code, Message));
    TestEqualSensitive(TEXT("The refusal is ANIMATION_NOT_FOUND"), Code,
        FString(TEXT("ANIMATION_NOT_FOUND")));
    TestTrue(
        FString::Printf(TEXT("The message names the path it could not load (got: %s)"), *Message),
        Message.Contains(Request.AnimationPath));

    if (!bEditorWasOpenBefore)
    {
        TestFalse(TEXT("No asset editor was opened to produce the refusal"),
            PWMeshSubjectEditorIsOpenFor(PWMeshSubjectSkeletalCubePath));
    }
    else
    {
        AddInfo(TEXT("An asset editor for the fixture was already open before this test; the "
                     "no-window-opened half is NOT MEASURED on this run."));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// THE CLOSE MUST NOT FIND A SECOND HOLDER OF THE PREVIEW VIEWPORT.
//
// ~SEditorViewport resets its client and then asserts check(SceneViewport.IsUnique()) (UE 5.8
// Editor/UnrealEd/Private/SEditorViewport.cpp:65). A check(), not an ensure(): when a provider's
// Release closes the asset editor while anything else still holds a TSharedPtr<FSceneViewport> to
// that preview, the editor aborts. It did - camera.orbit_shots over a staticMesh subject held its
// own copy across ReleaseSubject and killed a full suite run at 1585 started / 1584 succeeded.
//
// WHAT THIS MEASURES THAT THE FAKE-PROVIDER TEST CANNOT. CountPreviewSceneViewportHolders is what
// CloseAssetEditor now consults before it closes anything, and its baseline is arithmetic on a live
// viewport: SEditorViewport's own member plus the by-value pointer GetSceneViewport() hands the
// probe. That constant cannot be checked against a fake - get it wrong by one and the guard either
// refuses every close (breaking every asset capture) or refuses none (leaving the abort in place).
// So this test resolves a REAL Static Mesh preview and walks the count through three states.
//
// NOTHING IS CLOSED WHILE A REFERENCE IS HELD, deliberately: bCloseAfterCapture is false, so the
// release restores the preview without destroying it. The test asserts the PRECONDITION of the
// crash rather than reproducing the crash, which is the whole point - the next person gets a named
// failure instead of a dead process and a callstack that blames the engine.
//
// WHAT IT FAILS ON: a ReleaseSubject that leaves the subject's SceneViewport in place (the count
// stays at 1 after the release), and a baseline constant that is off by one in either direction
// (the count reads non-zero with nothing held, or stays zero with a copy deliberately held).
//
// UNABLE TO FAIL IF the host cannot realise a Static Mesh preview viewport - the resolve then
// reports a typed host-limited code and every assertion is skipped OUT LOUD. It is also unable to
// fail if it asserted only "the count is zero after release": zero is what an unmeasurable subject
// returns too, which is why the held-copy case below asserts a POSITIVE count from the same
// function first. A detector that always returned zero would sail through the release assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshReleaseLeavesNoViewportHolderTest,
    "PinWright.render.capture_subject.ReleaseLeavesNoPreviewViewportHolder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshReleaseLeavesNoViewportHolderTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectCubePath);

    // Declared FIRST so it runs LAST - after the subject's own destructor - and closes anything the
    // release rule deliberately left open. An asset editor still open at editor exit faults in
    // ~FStaticMeshEditor (UE 5.8 StaticMeshEditor.cpp:271).
    const bool bEditorWasOpenBefore = PWMeshSubjectEditorIsOpenFor(PWMeshSubjectCubePath);
    ON_SCOPE_EXIT
    {
        if (!bEditorWasOpenBefore)
        {
            PWMeshSubjectCloseEditorFor(PWMeshSubjectCubePath);
        }
    };

    UObject* Asset = LoadObject<UObject>(nullptr, PWMeshSubjectCubePath);
    if (!TestNotNull(TEXT("the Static Mesh fixture loads"), Asset))
    {
        return true;
    }

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
    Request.AssetPath = PWMeshSubjectCubePath;
    Request.bKindProvided = true;
    // The window stays open through the whole test: this measures who is HOLDING the viewport, and
    // destroying it while a reference is held is the abort this test exists to prevent.
    Request.bCloseAfterCapture = false;
    Request.bCloseAfterCaptureProvided = true;

    PWMeshSubjectProbe Probe;
    PWMeshSubjectResolve(Request, Probe);

    if (!Probe.bResolved)
    {
        if (!PWMeshSubjectIsHostLimitedCode(Probe.ErrCode))
        {
            AddError(FString::Printf(
                TEXT("Resolving a Static Mesh subject failed with an unexpected code '%s': %s"),
                *Probe.ErrCode, *Probe.ErrMsg));
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-static-mesh-preview-viewport"), FString::Printf(
            TEXT("%s: %s -- NOT MEASURED, nothing below ran"),
            *Probe.ErrCode, *Probe.ErrMsg));
        return true;
    }

    if (!TestTrue(TEXT("the resolved subject carries a scene viewport to hold"),
            Probe.Resolved.SceneViewport.IsValid()))
    {
        return true;
    }

    // ---- 1. the subject itself is visible to the detector ----
    //
    // Asserted before anything is released, so a detector that could only ever answer zero is
    // caught here rather than passing the two assertions that follow.
    const int32 HoldersWhileSubjectHolds =
        PinWrightCaptureSubject::CountPreviewSceneViewportHolders(Asset);
    TestTrue(*FString::Printf(
            TEXT("while the resolved subject holds the preview viewport, the close-guard counts it "
                 "as an extra holder (got %d, expected >= 1)"), HoldersWhileSubjectHolds),
        HoldersWhileSubjectHolds >= 1);

    // ---- 2. release drops the subject's own pair ----
    PinWrightCaptureSubject::ReleaseSubject(Probe.Resolved);
    TestNull(TEXT("the released subject holds no viewport client"), Probe.Resolved.ViewportClient);
    TestFalse(TEXT("the released subject holds no scene viewport"),
        Probe.Resolved.SceneViewport.IsValid());

    const int32 HoldersAfterRelease =
        PinWrightCaptureSubject::CountPreviewSceneViewportHolders(Asset);
    // THE ASSERTION THE CRASH WOULD HAVE FAILED. Zero means CloseAssetEditor may destroy the
    // SEditorViewport without tripping check(SceneViewport.IsUnique()).
    TestEqual(TEXT("after the release nothing holds the preview viewport, so closing it is safe"),
        HoldersAfterRelease, 0);

    // ---- 3. FAILURE DIRECTION: a caller's copy IS detected ----
    //
    // Without this the two assertions above would pass against a detector wired to a wrong baseline
    // that reports zero unconditionally - which is exactly the state that lets the abort through.
    {
        PWMeshSubjectProbe Second;
        PWMeshSubjectResolve(Request, Second);
        if (Second.bResolved && Second.Resolved.SceneViewport.IsValid())
        {
            // The defect in one line: a verb copying the pair into a local and keeping it across
            // the release. This is what camera.orbit_shots did.
            TSharedPtr<FSceneViewport> CallerCopy = Second.Resolved.SceneViewport;
            PinWrightCaptureSubject::ReleaseSubject(Second.Resolved);

            const int32 HoldersWithCallerCopy =
                PinWrightCaptureSubject::CountPreviewSceneViewportHolders(Asset);
            TestEqual(TEXT("a caller's surviving copy is counted, so the close is refused instead "
                           "of aborting the editor"),
                HoldersWithCallerCopy, 1);

            CallerCopy.Reset();
            TestEqual(TEXT("dropping the caller's copy clears the refusal"),
                PinWrightCaptureSubject::CountPreviewSceneViewportHolders(Asset), 0);
        }
        else
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-second-preview-viewport"),
                TEXT("the second resolve did not realise a preview viewport, so the held-copy "
                     "direction is NOT MEASURED"));
        }
    }

    return true;
}

// ---------------------------------------------------------------------------------------------
// THE SUBJECT-COVERAGE SEAM: a mesh subject can be hidden, so it can be MEASURED
// ---------------------------------------------------------------------------------------------
//
// THE DEFECT THIS CLOSES. `subjectCoverage` -- the fraction of a frame's pixels the subject
// actually paints, measured by drawing the same pose twice with the subject hidden for one of them
// -- was bound for the Niagara kind alone. Not because the arithmetic was Niagara-shaped, but
// because render.capture_asset_preview built the hide/show closure itself behind a
// `GetNiagaraReleaseState` test, which made the VERB the owner of a table of which kinds can be
// hidden. Every kind missing from that table returned `subjectCoverage: null` forever, with
// nothing in the response distinguishing "could not be measured" from "was not measured".
//
// The reasoning written on that branch was wrong on its own terms: it said a Static Mesh "cannot
// be hidden without hiding the thing the capture is of". Hiding the thing being captured is
// exactly what a REFERENCE frame is -- the differential wants the same pose WITHOUT the subject so
// it can subtract it -- and the preview scene keeps its floor, sky and lights while the mesh is
// hidden, so what changed between the two frames is the mesh.
//
// The cost was measured: a Static Mesh capture came back as uniform colour with no subject in
// frame while reporting blank:false, crushed:null, litPixelFraction:1.0 and boundsInFrame:true.
// Every published health signal read green and the one field that answers "is the subject in this
// picture" was structurally absent for that kind.
//
// TWO HALVES, AND ONLY ONE OF THEM CAN BE SKIPPED. The structural half is a source scan that runs
// on every host and cannot take a skip path: it holds the verb to owning NO per-kind table. The
// behavioural half needs a live preview viewport, and says NOT MEASURED rather than passing
// quietly when the host cannot realise one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshVisibilitySeamTest,
    "PinWright.render.capture_subject.MeshSubjectsPublishAVisibilitySeam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshVisibilitySeamTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(PWMeshSubjectCubePath);
    // Declared first so it runs last, after the probe's destructor has released the subject.
    ON_SCOPE_EXIT { PWMeshSubjectCloseEditorFor(PWMeshSubjectCubePath); };

    PinWrightCaptureSubject::FSubjectRequest Request;
    Request.Kind = PinWrightCaptureSubject::ESubjectKind::StaticMesh;
    Request.AssetPath = PWMeshSubjectCubePath;

    PWMeshSubjectProbe Probe;
    PWMeshSubjectResolve(Request, Probe);

    if (!Probe.bResolved && !PWMeshSubjectIsHostLimitedCode(Probe.ErrCode))
    {
        AddError(FString::Printf(
            TEXT("Resolving a Static Mesh subject failed with an unexpected code '%s': %s"),
            *Probe.ErrCode, *Probe.ErrMsg));
        return false;
    }
    if (!Probe.bResolved)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preview-viewport-unresolved"),
            FString::Printf(
                TEXT("the visibility-seam assertions are NOT MEASURED on this host (%s): the seam is ")
                TEXT("bound from the resolved preview component, so there is nothing to bind it to."),
                *Probe.ErrCode));
        return true;
    }

    // THE ASSERTION. Before this change the setter was unbound for every kind but Niagara, so this
    // line fails against the old provider -- which is what makes it a regression test rather than a
    // restatement. It cannot pass vacuously: an unbound TFunction converts to false.
    if (!TestTrue(TEXT("a resolved Static Mesh subject publishes a visibility setter"),
        static_cast<bool>(Probe.Resolved.VisibilitySetter)))
    {
        return false;
    }

    // Both directions, and both must SUCCEED. A setter that refuses is worse than an absent one:
    // PoseListCapture treats a failed re-show as fatal to the whole set, precisely because every
    // remaining shot would otherwise be captured with the subject hidden and score 0.000 against an
    // equally hidden reference -- the silent-empty-frame failure introduced by its own catcher.
    {
        FString HideCode;
        FString HideMessage;
        const bool bHidden = Probe.Resolved.VisibilitySetter(false, HideCode, HideMessage);
        TestTrue(FString::Printf(TEXT("the subject can be hidden (%s: %s)"), *HideCode, *HideMessage),
            bHidden);

        FString ShowCode;
        FString ShowMessage;
        const bool bShown = Probe.Resolved.VisibilitySetter(true, ShowCode, ShowMessage);
        TestTrue(FString::Printf(TEXT("the subject can be shown again (%s: %s)"), *ShowCode, *ShowMessage),
            bShown);
    }

    return true;
}

// ---------------------------------------------------------------------------------------------
// The structural half: the VERB owns no table of which kinds can be hidden.
//
// This is the guarantee, not the lambda. A per-kind branch in the handler is what made the gap
// invisible for the whole life of the feature -- the capability was absent, nothing measured its
// absence, and the response reported `subjectCoverage: null` in a shape indistinguishable from
// "the caller turned it off". Runs on every host, needs no viewport, and cannot skip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMeshCoverageHasNoPerKindTableTest,
    "PinWright.render.capture_subject.CoverageBindingIsProviderOwnedNotVerbOwned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMeshCoverageHasNoPerKindTableTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("the PinWright plugin resolves through IPluginManager"), Plugin.IsValid()))
    {
        return true;
    }
    const FString HandlerPath = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Handlers/Render/RenderHandler.cpp");

    FString Contents;
    if (!TestTrue(TEXT("read RenderHandler.cpp from disk"),
        FFileHelper::LoadFileToString(Contents, *HandlerPath)))
    {
        return true;
    }

    TestTrue(TEXT("the verb takes the setter the PROVIDER bound"),
        Contents.Contains(TEXT("PoseRequest.SubjectVisibilitySetter = Resolved.VisibilitySetter;")));
    // The exact shape that made the gap: the handler reaching into ONE provider's state to build a
    // kind-specific closure. Asserted by the engine call that only such a closure would need, so
    // the check does not depend on how the assignment happens to be line-wrapped.
    TestFalse(TEXT("the verb does not reach into a provider's release state to bind coverage"),
        Contents.Contains(TEXT("SetPreviewComponentVisible")));

    return true;
}

#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#undef TestEqualSensitive
#endif
