// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliageAddTypeNamePathSafety.cpp - regression coverage for
// B-foliage-add-type-name-with-slash-kills-the-editor.
//
// WHAT WAS WRONG. foliage.add_type concatenated its `name` straight onto a hardcoded
// "/Game/Foliage" and handed the result to CreatePackage with no validation of any kind. A `name`
// beginning with '/' composed "/Game/Foliage//Game/...", and CreatePackage
// (UObjectGlobals.cpp:1094-1096) logs that at **Fatal** - a verbosity that is not compiled out in
// any configuration. So the call did not fail: the editor PROCESS died, taking every unsaved
// package in it, in an editor shared with another agent. The handler's own `if (!Package)` could
// never fire, because nothing after CreatePackage was ever reached.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is not a failure signal, it is an absence of one
// (see the DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on
// the post-fix CONTRACT - the refusal - and the fixture is built so that a build WITHOUT the fix
// takes a different, harmless path instead of the fatal one:
//
//   every refusal case pairs its bad `name` with a `meshPath` that is a well-formed long package
//   name naming NO asset (a fresh GUID). The pre-fix handler validated meshPath and nothing else,
//   so on a reverted build the call is refused ASSET_NOT_FOUND at the mesh load - which is ABOVE
//   the concatenation - and the TestEqual on INVALID_ARGUMENT below goes red while the process
//   lives. CreatePackage is unreachable on both the fixed and the reverted build.
//
// That property depends on the fix keeping its name/path check ABOVE the meshPath resolution, and
// the handler carries a comment saying so. Do NOT "improve" these cases by supplying a mesh that
// exists, and do NOT rewrite them into a crash expectation: either change hands a live editor a
// string that ends it.
//
// WHAT ELSE IS ASSERTED, AND WHY IT BELONGS IN THE SAME FILE. A refusal on its own would leave the
// caller with no way to reach the legitimate intent: the destination was a literal, the verb
// exposed no path parameter, and putting a path in `name` was the only lever a caller had. The
// fix adds `savePath` - spelled and validated exactly as foliage.create_procedural spells it - so
// SavePathChoosesTheDestination asserts the intent is now reachable, and
// TraversalSavePathIsRefused asserts the new parameter did not open a second hole. Without the
// first, a future "simplification" could drop savePath and still pass the refusal test.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec - so a savePath that was never registered is refused there.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "FoliageType.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliageAddTypeNamePathSafetyHelpers
{
    // The only content fixture, and only the positive test needs it.
    constexpr const TCHAR* SafetyCubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // The verb's hardcoded destination, which every refusal case must leave untouched.
    constexpr const TCHAR* DefaultFoliageFolder = TEXT("/Game/Foliage");

    // A folder that is NOT the default, so a savePath that was ignored rather than honoured is
    // visible as an asset sitting under /Game/Foliage instead.
    constexpr const TCHAR* SafetyScratchFolder = TEXT("/Game/PinWrightTests/FoliageAddTypeSavePath");

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWFoliageSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-crash guarantee described in the file header: it makes the PRE-FIX handler bail at its
    // meshPath check, above the concatenation, instead of reaching CreatePackage.
    inline FString SafetyAbsentMeshPath()
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/SM_Absent_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString SafetyPackagePath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s"), Folder, *Name);
    }

    inline FString SafetyObjectPath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), Folder, *Name, *Name);
    }

    // Drives one refusal case and reports on all three facts that separate the post-fix contract
    // from the pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT error rather
    // than as a missing mesh, and the message names the offending value so the caller can act.
    inline void ExpectNameRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadName, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BadName);
        Params->SetStringField(TEXT("meshPath"), SafetyAbsentMeshPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-add-type-name-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("a %s name ('%s') is refused"), Label, *BadName),
            bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // ASSET_NOT_FOUND from the meshPath check, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s name is refused as a caller argument error, not as a missing mesh"), Label),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending name or path"), Label),
            Sink->Message.Contains(BadName) ||
            Sink->Message.Contains(SafetyPackagePath(DefaultFoliageFolder, BadName)));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal creates no object"), Label),
            FindObject<UObject>(nullptr, *SafetyObjectPath(DefaultFoliageFolder, BadName)) ==
                nullptr);
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// A `name` carrying a path is refused instead of composing "//" into CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeNameCarryingAPathIsRefusedTest,
    "PinWright.foliage.add_type.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAddTypeNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // The exact input measured on the ticket: a leading slash is what composed
    // "/Game/Foliage//Game/PinWrightScratch/..." and killed editor pid 7856.
    ExpectNameRefused(*this, Dispatcher, Sink,
        TEXT("/Game/PinWrightScratch/PWScratch_NameSafety"), TEXT("rooted path"));

    // The whole class, not just the measured member. An interior slash composes a nested package
    // rather than "//", so it does not fatal - but it silently writes somewhere the caller did not
    // name, which is the same argument confusion one step short of the crash.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Sub/Leaf"), TEXT("interior slash"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the composed path is checked
    // against the engine's package rules as well as the name against its object rules.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Sub\\Leaf"), TEXT("backslash"));

    // Traversal, in the argument that has no sanitizer of its own.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("../Escape"), TEXT("traversal"));

    // A trailing slash composes "/Game/Foliage/Name/", which CreatePackage would resolve to
    // something other than the caller's name.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Trailing/"), TEXT("trailing slash"));

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same absent mesh, so it stops at ASSET_NOT_FOUND - which is exactly the
    // proof that a bare name got PAST the new check and reached the mesh resolution.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), SafetyUniqueName(TEXT("Control")));
        Params->SetStringField(TEXT("meshPath"), SafetyAbsentMeshPath());

        bool bSuccess = true;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-add-type-name-control"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the control call still fails - its mesh does not exist"), bSuccess);
        TestEqual(TEXT("a BARE name passes the name check and is refused on the mesh instead"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    return true;
}

// ============================================================================
// savePath makes the destination reachable, which is why the refusal above is not a dead end
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeSavePathChoosesDestinationTest,
    "PinWright.foliage.add_type.SavePathChoosesTheDestination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeSavePathChoosesDestinationTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAddTypeNamePathSafetyHelpers;

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, SafetyCubeMeshPath);
    if (!Cube)
    {
        AddError(FString::Printf(TEXT("required fixture mesh %s did not load"),
            SafetyCubeMeshPath));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = SafetyUniqueName(TEXT("SavePath"));
    // Both destinations are cleaned up: if savePath were ignored the asset lands under
    // /Game/Foliage, and this test must not leak it into host Content either way.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SafetyPackagePath(SafetyScratchFolder, TypeName));
        CleanupTestAsset(SafetyPackagePath(DefaultFoliageFolder, TypeName));
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TypeName);
    Params->SetStringField(TEXT("meshPath"), SafetyCubeMeshPath);
    Params->SetStringField(TEXT("savePath"), SafetyScratchFolder);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"), TEXT("req-foliage-add-type-savepath"),
        Params, bSuccess, Result, ErrorCode);

    // Pre-fix this is UNKNOWN_PARAMS from the dispatcher's own gate: savePath was not registered.
    if (!TestTrue(*FString::Printf(TEXT("foliage.add_type accepts savePath (error=%s: %s)"),
            *ErrorCode, *Sink->Message), bSuccess) || !Result.IsValid())
    {
        return true;
    }

    // Re-read from the ENGINE at the path the caller can construct, not from the response - a
    // handler that echoed the requested folder while writing to the old literal fails here.
    UFoliageType* Type =
        LoadObject<UFoliageType>(nullptr, *SafetyObjectPath(SafetyScratchFolder, TypeName));
    TestTrue(TEXT("the foliage type is addressable under the requested savePath"),
        Type != nullptr);
    TestFalse(TEXT("nothing was written to the old hardcoded /Game/Foliage destination"),
        UEditorAssetLibrary::DoesAssetExist(SafetyPackagePath(DefaultFoliageFolder, TypeName)));

    // The effective folder is echoed whether supplied or defaulted, so a caller can tell where
    // the asset went without guessing.
    FString ReportedSavePath;
    if (TestTrue(TEXT("the response carries save_path"),
            Result->TryGetStringField(TEXT("save_path"), ReportedSavePath)))
    {
        TestEqual(TEXT("save_path echoes the requested folder"), ReportedSavePath,
            FString(SafetyScratchFolder));
    }

    FString ReportedAssetPath;
    if (TestTrue(TEXT("the response carries asset_path"),
            Result->TryGetStringField(TEXT("asset_path"), ReportedAssetPath)))
    {
        TestTrue(TEXT("asset_path resolves to the asset that was written"),
            Type != nullptr && LoadObject<UFoliageType>(nullptr, *ReportedAssetPath) == Type);
    }

    return true;
}

// ============================================================================
// The new savePath is not a second hole: traversal is refused, and refused before anything
// is created
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddTypeTraversalSavePathRefusedTest,
    "PinWright.foliage.add_type.TraversalSavePathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddTypeTraversalSavePathRefusedTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAddTypeNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = SafetyUniqueName(TEXT("Traversal"));
    // Torn down anyway: if a refused call DID create something, the test must not also leak it.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SafetyPackagePath(DefaultFoliageFolder, TypeName));
    };

    // Driven with the REAL cube, deliberately: the point of this case is that the savePath is
    // refused on its own terms, before a resolvable mesh could carry the call any further.
    // Unlike the name cases it is safe to do so - SanitizeProjectRelativePath collapses "//"
    // and rejects "..", so nothing path-shaped survives to CreatePackage on either build.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TypeName);
    Params->SetStringField(TEXT("meshPath"), SafetyCubeMeshPath);
    Params->SetStringField(TEXT("savePath"), TEXT("/Game/../../Engine/Content"));

    bool bSuccess = true;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
        TEXT("req-foliage-add-type-savepath-traversal"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("a savePath containing '..' is refused"), bSuccess);
    TestEqual(TEXT("an unsafe savePath returns SECURITY_VIOLATION, the code "
                   "foliage.create_procedural already uses for the same input"),
        ErrorCode, FString(TEXT("SECURITY_VIOLATION")));
    TestFalse(TEXT("a refused savePath falls back to no destination at all, not to /Game/Foliage"),
        UEditorAssetLibrary::DoesAssetExist(SafetyPackagePath(DefaultFoliageFolder, TypeName)));

    return true;
}
