// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestLandscapeCreateGrassTypeNamePathSafety.cpp - regression coverage for
// B-landscape-create-grass-type-name-with-slash-kills-the-editor.
//
// WHAT WAS WRONG. landscape.create_grass_type concatenated its `name` straight onto a hardcoded
// "/Game/Landscape" and handed the result to CreatePackage with no validation of any kind. A
// `name` beginning with '/' composed "/Game/Landscape//Game/...", and CreatePackage
// (UObjectGlobals.cpp:1094-1096) logs that at **Fatal** - a verbosity that is not compiled out in
// any configuration. So the call did not fail: the editor PROCESS died, taking every unsaved
// package in it. This is the fifth instance of one defect class; the sibling ticket
// B-foliage-add-type-name-with-slash-kills-the-editor carries the MEASURED editor death (pid 7856)
// that established the mechanism, and this verb had the same shape plus no savePath either.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is not a failure signal, it is an absence of one
// (see the DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on
// the post-fix CONTRACT - the refusal - and the fixture is built so that a build WITHOUT the fix
// takes a different, harmless path instead of the fatal one:
//
//   every refusal case pairs its bad `name` with a `meshPath` that is a well-formed long package
//   name naming NO asset (a fresh GUID). The pre-fix handler's async body loaded that mesh as its
//   FIRST act, above the concatenation, so on a reverted build the call is refused
//   ASSET_NOT_FOUND at the mesh load and the TestEqual on INVALID_ARGUMENT below goes red while
//   the process lives. CreatePackage is unreachable on both the fixed and the reverted build.
//
// That property depends on the fix keeping its name/path check ABOVE the mesh load - which the
// fixed handler does by composing on the calling thread, before the AsyncTask is even queued - and
// the handler carries a comment saying so. Do NOT "improve" these cases by supplying a mesh that
// exists, and do NOT rewrite them into a crash expectation: either change hands a live editor a
// string that ends it.
//
// WHAT ELSE IS ASSERTED, AND WHY IT BELONGS IN THE SAME FILE. A refusal on its own would leave the
// caller with no way to reach the legitimate intent: the destination was a literal, the verb
// exposed no path parameter, and putting a path in `name` was the only lever a caller had. The fix
// adds `savePath` - spelled and validated exactly as foliage.add_type and
// foliage.create_procedural spell it - so SavePathChoosesTheDestination asserts the intent is now
// reachable, and TraversalSavePathIsRefused asserts the new parameter did not open a second hole.
// Without the first, a future "simplification" could drop savePath and still pass the refusal test.
//
// WHY TWO ENTRY POINTS. landscape.create_grass_type finishes inside an AsyncTask(GameThread)
// lambda, and FHandlerContext::MakeAsyncToken deliberately does NOT forward the dispatcher's
// stack-owned synchronous capture (HandlerContext.cpp:547-553), so a dispatcher-driven async
// completion is unobservable in-frame. Cases whose answer is ASYNC therefore run through
// InvokeHandlerWithSharedCapture + PumpUntilCaptured, the route the sibling
// TestFoliageDensityScalabilityCVars.cpp already uses for this same verb. TraversalSavePathIsRefused
// answers SYNCHRONOUSLY, so it runs through the real production dispatcher instead - which also
// validates each payload against the declared ParamSpec, making it the case that proves `savePath`
// is actually REGISTERED and not merely read out of the payload.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "LandscapeGrassType.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace LandscapeGrassTypeNamePathSafetyHelpers
{
    // The only content fixture, and only the positive test needs it.
    constexpr const TCHAR* GrassNameSafetyCubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // The verb's hardcoded destination, which every refusal case must leave untouched.
    constexpr const TCHAR* GrassNameSafetyDefaultFolder = TEXT("/Game/Landscape");

    // A folder that is NOT the default, so a savePath that was ignored rather than honoured is
    // visible as an asset sitting under /Game/Landscape instead.
    constexpr const TCHAR* GrassNameSafetyScratchFolder =
        TEXT("/Game/PinWrightTests/LandscapeGrassSavePath");

    inline FString GrassNameSafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWGrassSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-crash guarantee described in the file header: it makes the PRE-FIX handler bail at its
    // mesh load, above the concatenation, instead of reaching CreatePackage. It also survives the
    // handler's SanitizeProjectRelativePath check on meshPath, so the bail is ASSET_NOT_FOUND and
    // not SECURITY_VIOLATION.
    inline FString GrassNameSafetyAbsentMeshPath()
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/SM_Absent_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString GrassNameSafetyPackagePath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s"), Folder, *Name);
    }

    inline FString GrassNameSafetyObjectPath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), Folder, *Name, *Name);
    }

    // Drives one call through the registered handler and waits for the answer, whether it came
    // back synchronously (every refusal) or from the AsyncTask body (the control and the positive
    // case). Returns false only when the handler is not registered at all.
    inline bool GrassNameSafetyInvokeAndWait(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Params, const TSharedRef<FTestResponseCapture>& Capture)
    {
        if (!Test.TestTrue(TEXT("landscape.create_grass_type handler registered"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.create_grass_type"), Params,
                    Capture)))
        {
            return false;
        }
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/15.0);
        return true;
    }

    // Drives one refusal case and reports on all three facts that separate the post-fix contract
    // from the pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT error rather
    // than as a missing mesh, and the message names the offending value so the caller can act.
    inline void GrassNameSafetyExpectNameRefused(FAutomationTestBase& Test, const FString& BadName,
        const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BadName);
        Params->SetStringField(TEXT("meshPath"), GrassNameSafetyAbsentMeshPath());

        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        if (!GrassNameSafetyInvokeAndWait(Test, Params, Capture))
        {
            return;
        }

        Test.TestTrue(*FString::Printf(TEXT("a %s name ('%s') produced an answer"), Label,
            *BadName), Capture->bWasCalled);
        Test.TestFalse(*FString::Printf(TEXT("a %s name ('%s') is refused"), Label, *BadName),
            Capture->bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // ASSET_NOT_FOUND from the async body's mesh load, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s name is refused as a caller argument error, not as a missing mesh"), Label),
            Capture->ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending name or path"), Label),
            Capture->Message.Contains(BadName) ||
            Capture->Message.Contains(
                GrassNameSafetyPackagePath(GrassNameSafetyDefaultFolder, BadName)));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal creates no object"), Label),
            FindObject<UObject>(nullptr,
                *GrassNameSafetyObjectPath(GrassNameSafetyDefaultFolder, BadName)) == nullptr);
    }
}

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// A `name` carrying a path is refused instead of composing "//" into CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGrassTypeNamePathRefusedTest,
    "PinWright.landscape.create_grass_type.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGrassTypeNamePathRefusedTest::RunTest(const FString& Parameters)
{
    using namespace LandscapeGrassTypeNamePathSafetyHelpers;

    // The shape measured on the sibling ticket: a leading slash is what composed
    // "<folder>//Game/..." and killed a shared editor.
    GrassNameSafetyExpectNameRefused(*this,
        TEXT("/Game/PinWrightScratch/PWScratch_GrassNameSafety"), TEXT("rooted path"));

    // The whole class, not just the crashing member. An interior slash composes a nested package
    // rather than "//", so it does not fatal - but it silently writes somewhere the caller did not
    // name, which is the same argument confusion one step short of the crash.
    GrassNameSafetyExpectNameRefused(*this, TEXT("Sub/Leaf"), TEXT("interior slash"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the composed path is checked
    // against the engine's package rules as well as the name against its object rules.
    GrassNameSafetyExpectNameRefused(*this, TEXT("Sub\\Leaf"), TEXT("backslash"));

    // Traversal, in the argument that has no sanitizer of its own.
    GrassNameSafetyExpectNameRefused(*this, TEXT("../Escape"), TEXT("traversal"));

    // A trailing slash composes "/Game/Landscape/Name/", which CreatePackage would resolve to
    // something other than the caller's name.
    GrassNameSafetyExpectNameRefused(*this, TEXT("Trailing/"), TEXT("trailing slash"));

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same absent mesh, so it stops at ASSET_NOT_FOUND - which is exactly the
    // proof that a bare name got PAST the new check and reached the mesh resolution.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), GrassNameSafetyUniqueName(TEXT("Control")));
        Params->SetStringField(TEXT("meshPath"), GrassNameSafetyAbsentMeshPath());

        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        if (GrassNameSafetyInvokeAndWait(*this, Params, Capture))
        {
            TestTrue(TEXT("the control call reached the async body and answered"),
                Capture->bWasCalled);
            TestFalse(TEXT("the control call still fails - its mesh does not exist"),
                Capture->bSuccess);
            TestEqual(TEXT("a BARE name passes the name check and is refused on the mesh instead"),
                Capture->ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
        }
    }

    return true;
}

// ============================================================================
// savePath makes the destination reachable, which is why the refusal above is not a dead end
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGrassTypeSavePathDestinationTest,
    "PinWright.landscape.create_grass_type.SavePathChoosesTheDestination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGrassTypeSavePathDestinationTest::RunTest(const FString& Parameters)
{
    using namespace LandscapeGrassTypeNamePathSafetyHelpers;

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, GrassNameSafetyCubeMeshPath);
    if (!Cube)
    {
        AddError(FString::Printf(TEXT("required fixture mesh %s did not load"),
            GrassNameSafetyCubeMeshPath));
        return true;
    }

    const FString TypeName = GrassNameSafetyUniqueName(TEXT("SavePath"));
    // Both destinations are cleaned up: if savePath were ignored the asset lands under
    // /Game/Landscape, and this test must not leak it into host Content either way.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(GrassNameSafetyPackagePath(GrassNameSafetyScratchFolder, TypeName));
        CleanupTestAsset(GrassNameSafetyPackagePath(GrassNameSafetyDefaultFolder, TypeName));
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TypeName);
    Params->SetStringField(TEXT("meshPath"), GrassNameSafetyCubeMeshPath);
    Params->SetStringField(TEXT("savePath"), GrassNameSafetyScratchFolder);

    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    if (!GrassNameSafetyInvokeAndWait(*this, Params, Capture))
    {
        return true;
    }

    // Pre-fix savePath was neither registered nor read, so the asset landed under /Game/Landscape
    // and every assertion below is red.
    if (!TestTrue(*FString::Printf(
            TEXT("landscape.create_grass_type honours savePath (error=%s: %s)"),
            *Capture->ErrorCode, *Capture->Message),
            Capture->bWasCalled && Capture->bSuccess) || !Capture->Result.IsValid())
    {
        return true;
    }

    // Re-read from the ENGINE at the path the caller can construct, not from the response - a
    // handler that echoed the requested folder while writing to the old literal fails here.
    ULandscapeGrassType* GrassType = LoadObject<ULandscapeGrassType>(nullptr,
        *GrassNameSafetyObjectPath(GrassNameSafetyScratchFolder, TypeName));
    TestTrue(TEXT("the grass type is addressable under the requested savePath"),
        GrassType != nullptr);
    TestFalse(TEXT("nothing was written to the old hardcoded /Game/Landscape destination"),
        UEditorAssetLibrary::DoesAssetExist(
            GrassNameSafetyPackagePath(GrassNameSafetyDefaultFolder, TypeName)));

    // The effective folder is echoed whether supplied or defaulted, so a caller can tell where
    // the asset went without guessing.
    FString ReportedSavePath;
    if (TestTrue(TEXT("the response carries save_path"),
            Capture->Result->TryGetStringField(TEXT("save_path"), ReportedSavePath)))
    {
        TestEqual(TEXT("save_path echoes the requested folder"), ReportedSavePath,
            FString(GrassNameSafetyScratchFolder));
    }

    FString ReportedAssetPath;
    if (TestTrue(TEXT("the response carries asset_path"),
            Capture->Result->TryGetStringField(TEXT("asset_path"), ReportedAssetPath)))
    {
        TestTrue(TEXT("asset_path resolves to the asset that was written"),
            GrassType != nullptr &&
            LoadObject<ULandscapeGrassType>(nullptr, *ReportedAssetPath) == GrassType);
    }

    return true;
}

// ============================================================================
// The new savePath is not a second hole: traversal is refused, and refused before anything
// is created. Driven through the production dispatcher, so this case also proves savePath is a
// REGISTERED parameter rather than one read straight out of the payload.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGrassTypeTraversalSavePathTest,
    "PinWright.landscape.create_grass_type.TraversalSavePathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGrassTypeTraversalSavePathTest::RunTest(const FString& Parameters)
{
    using namespace LandscapeGrassTypeNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = GrassNameSafetyUniqueName(TEXT("Traversal"));
    // Torn down anyway: if a refused call DID create something, the test must not also leak it.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(GrassNameSafetyPackagePath(GrassNameSafetyDefaultFolder, TypeName));
    };

    // Driven with the REAL cube, deliberately: the point of this case is that the savePath is
    // refused on its own terms, before a resolvable mesh could carry the call any further.
    // Unlike the name cases it is safe to do so - SanitizeProjectRelativePath collapses "//"
    // and rejects "..", so nothing path-shaped survives to CreatePackage on either build.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TypeName);
    Params->SetStringField(TEXT("meshPath"), GrassNameSafetyCubeMeshPath);
    Params->SetStringField(TEXT("savePath"), TEXT("/Game/../../Engine/Content"));

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("landscape.create_grass_type"),
        TEXT("req-landscape-grass-savepath-traversal"), Params, bSuccess, ErrorCode);

    // The refusal is SYNCHRONOUS - it happens before MakeAsyncToken - so the dispatcher sink has
    // it in-frame. A build where savePath is unregistered answers UNKNOWN_PARAMS here instead,
    // from the dispatcher's own ParamSpec gate; a build where it is registered but unvalidated
    // answers nothing in-frame at all, because the call would have gone async.
    TestTrue(TEXT("the traversal savePath is answered synchronously, before any async work"),
        Sink->bWasCalled);
    TestFalse(TEXT("a savePath containing '..' is refused"), bSuccess);
    TestEqual(TEXT("an unsafe savePath returns SECURITY_VIOLATION, the code "
                   "foliage.add_type already uses for the same input"),
        ErrorCode, FString(TEXT("SECURITY_VIOLATION")));
    TestFalse(TEXT("a refused savePath falls back to no destination at all, not to "
                   "/Game/Landscape"),
        UEditorAssetLibrary::DoesAssetExist(
            GrassNameSafetyPackagePath(GrassNameSafetyDefaultFolder, TypeName)));

    return true;
}
