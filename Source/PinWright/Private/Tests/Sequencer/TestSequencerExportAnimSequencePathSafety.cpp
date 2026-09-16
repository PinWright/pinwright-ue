// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestSequencerExportAnimSequencePathSafety.cpp - regression coverage for the
// sequencer.export_anim_sequence site of B-createpackage-unvalidated-paths-plugin-wide.
//
// THE DEFECT CLASS. `outAssetPath` is a whole caller-supplied package path that becomes the
// argument to CreatePackage in Handlers/Sequencer/SequencerBakeHandler.cpp. CreatePackage logs at
// **Fatal** for a name containing "//" (UObjectGlobals.cpp:1094-1096) and for one that resolves to
// empty after ResolveName2 (:1118) - the second is reachable from a ".."-shaped name, not only
// from the first. Fatal is not compiled out in any configuration, so such a call does not fail:
// it ends the editor PROCESS and every unsaved package in it, and the handler's `if (!Package)`
// can never fire because nothing after the call runs. The mechanism was MEASURED on a sibling
// verb (B-foliage-add-type-name-with-slash-kills-the-editor, editor pid 7856).
//
// WHAT THE RE-VERIFICATION OF THIS SITE ACTUALLY FOUND, stated because the sweep ticket lists it
// under "no guard at all" and that is not what the code said. `outAssetPath` already went through
// NormalizeAssetPath (Utils/AssetUtils.cpp), which sets bIsValid only after its own
// FPackageName::IsValidLongPackageName call on the path it returns - on BOTH of its success
// branches - so the Fatal was already unreachable here. The two things this test pins are
// therefore the ones that were genuinely missing: that the invariant is asserted in the file that
// makes the call rather than inherited from an undocumented postcondition of a shared Utils
// helper, and that the check happens BEFORE the sequence and binding resolution.
//
// WHY THIS TEST CANNOT DRIVE THE FATAL, ON EITHER BUILD. A Fatal takes the test host down with
// it, so a test that reproduced the defect would abort the whole suite rather than report a red -
// and a suite that dies mid-queue is not a failure signal, it is the absence of one (the
// DID_NOT_COMPLETE state in the plugin's testing notes). Every refusal case below pairs its bad
// `outAssetPath` with a `sequence` that is a well-formed long package name naming NO asset (a
// fresh GUID). That makes the ordering do the work:
//
//   * FIXED build - the path is refused as a path error above BakeResolveSequence, and
//     CreatePackage is never reached. Since the dispatch-boundary wave there are TWO such layers:
//     `outAssetPath` is a declared `path` param, so a "//" shape is refused INVALID_ARGUMENT by
//     the gate before the handler is entered, while a backslash or unmounted root carries no gate
//     rule and reaches the handler's own INVALID_PATH check. Each case below asserts its own code,
//     so removing EITHER layer goes red.
//   * REVERTED build (this block deleted, or moved back below the sequence resolution where it
//     used to sit) - BakeResolveSequence answers null on the absent sequence and the handler
//     returns SEQUENCE_NOT_FOUND, which is ABOVE the CreatePackage. The TestEqual on the expected
//     path error goes red and the process lives.
//
// That property depends on the handler keeping its outAssetPath check ABOVE BakeResolveSequence,
// and the handler carries a comment saying so. Do NOT "improve" these cases by supplying a
// sequence that exists, and do NOT rewrite them into a crash expectation: either change hands a
// live editor a string that ends it.
//
// WHAT IS DELIBERATELY NOT DRIVEN FROM HERE: the `sequence` argument's own guard. CreatePackage is
// not the only door to its Fatal - StaticLoadObjectInternal calls ResolveName2(..., Create=true)
// (UObjectGlobals.cpp:1427) and ResolveName2 calls CreatePackage (:1310), so BakeResolveSequence's
// LoadObject on a raw `sequence` was a second one-argument kill in the same file. It now checks
// FPackageName::IsValidLongPackageName on the package half of that path before loading. No wire
// payload can discriminate a build with that check from one without it: the shapes that WOULD
// discriminate (a "//" in `sequence`) end the process on the build without it, and the shapes that
// are safe to send (an unmounted root) answer SEQUENCE_NOT_FOUND on both. A test that cannot fail
// is worse than none, and a test that can kill the host is worse than both - so the guard ships
// with the comment at its call site as its documentation, and this note as the record of why.
//
// THE CONTROL IS NOT OPTIONAL AND IT PROVES THE ORDERING. A handler that refused every path would
// satisfy every refusal case above. The control drives a well-formed outAssetPath with the same
// absent sequence and asserts the answer is SEQUENCE_NOT_FOUND - which is simultaneously the
// proof that a valid path gets PAST the new check, and the proof that the check sits above the
// sequence resolution rather than below it.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"
#include "Misc/PackageName.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace SequencerExportAnimPathSafetyHelpers
{
    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-Fatal guarantee described in the file header: it makes a build WITHOUT the guard bail at
    // BakeResolveSequence, above the CreatePackage, instead of reaching it.
    inline FString ExportAnimSafetyAbsentSequencePath()
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/LS_Absent_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A syntactically valid possessable GUID. It is never consulted on either build - the path
    // check fires above it on the fixed build, the sequence load fails above it on the reverted
    // one - but the payload has to be otherwise well-formed for the refusal to be attributable.
    inline FString ExportAnimSafetyBindingGuid()
    {
        return FGuid::NewGuid().ToString(EGuidFormats::Digits);
    }

    inline FString ExportAnimSafetyUniqueLeaf(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWExportAnimSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Drives one refusal case and reports on the three facts that separate the post-fix contract
    // from the pre-fix behaviour: it is refused, it is refused as a PATH error rather than as a
    // missing sequence (the reverted-build discriminator), and the message names the offending
    // value so the caller can act on it.
    inline void ExportAnimSafetyExpectPathRefused(FAutomationTestBase& Test,
        FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& BadPath, const TCHAR* Label, const TCHAR* ExpectedCode)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("outAssetPath"), BadPath);
        Params->SetStringField(TEXT("sequence"), ExportAnimSafetyAbsentSequencePath());
        Params->SetStringField(TEXT("binding"), ExportAnimSafetyBindingGuid());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("sequencer.export_anim_sequence"),
            TEXT("req-export-anim-path-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("a %s outAssetPath ('%s') is refused"),
            Label, *BadPath), bSuccess);
        // The discriminator against a reverted or relocated guard: with the check absent, or
        // sitting below BakeResolveSequence where it used to, this same payload answers
        // SEQUENCE_NOT_FOUND and never a path error.
        //
        // ExpectedCode differs by shape. Since the dispatch-boundary wave, `outAssetPath` is a
        // declared `path` param, so a doubled slash is refused by the GATE (INVALID_ARGUMENT)
        // before the handler runs at all. A backslash carries no gate rule and still reaches the
        // handler's own check (INVALID_PATH). Asserting per shape keeps this able to tell the two
        // layers apart if either is removed.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s outAssetPath is refused as a path error, above the sequence resolution"),
            Label), ErrorCode, FString(ExpectedCode));
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal quotes the offending path"), Label),
            Sink->Message.Contains(BadPath));
    }
}

// ============================================================================
// A malformed outAssetPath is refused instead of reaching CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerExportAnimSequencePathSafetyTest,
    "PinWright.Sequencer.ExportAnimSequence.OutAssetPathIsGuardedBeforeCreatePackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerExportAnimSequencePathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace SequencerExportAnimPathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // The exact byte sequence CreatePackage logs Fatal on, at the folder/leaf boundary. The leaf
    // is GUID-unique. That used to be load-bearing: NormalizeAssetPath carried a "retry the bare
    // leaf under /Game, /Engine and /Script" fallback that could rescue a malformed path by
    // finding an existing package of that name, turning this refusal case into a success on some
    // hosts. That fallback has since been deleted (AssetUtils.cpp; the block returned bIsValid=true
    // naming a DIFFERENT package, which under this verb's own `overwrite` was a data-loss path), so
    // the GUID is now belt-and-braces rather than the thing making this case deterministic.
    ExportAnimSafetyExpectPathRefused(*this, Dispatcher, Sink,
        FString::Printf(TEXT("/Game/PinWrightTests//%s"), *ExportAnimSafetyUniqueLeaf(TEXT("Dbl"))),
        TEXT("double slash"), TEXT("INVALID_ARGUMENT"));

    // The same kill with a perfectly bare leaf and the "//" buried in the FOLDER instead. It is
    // the half that reads safe: a normalizer that trims trailing slashes and maps aliases does not
    // collapse an interior "//", so a name-only character check passes this straight through to
    // the Fatal. Only a check on the COMPOSED path refuses it.
    ExportAnimSafetyExpectPathRefused(*this, Dispatcher, Sink,
        FString::Printf(TEXT("/Game//PinWrightTests/%s"), *ExportAnimSafetyUniqueLeaf(TEXT("Fld"))),
        TEXT("double slash inside the folder"), TEXT("INVALID_ARGUMENT"));

    // Same class with no leading slash, which is the shape a caller reaches for when they mean a
    // relative path. It must not be repaired into something CreatePackage would die on.
    ExportAnimSafetyExpectPathRefused(*this, Dispatcher, Sink,
        FString::Printf(TEXT("%s//Leaf"), *ExportAnimSafetyUniqueLeaf(TEXT("Rel"))),
        TEXT("unrooted double slash"), TEXT("INVALID_ARGUMENT"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this is the case proving the path is checked against the
    // engine's package rules rather than against a hand-rolled character list.
    ExportAnimSafetyExpectPathRefused(*this, Dispatcher, Sink,
        FString::Printf(TEXT("/Game/PinWrightTests/%s\\Leaf"),
            *ExportAnimSafetyUniqueLeaf(TEXT("Back"))),
        TEXT("backslash"), TEXT("INVALID_PATH"));

    // An unmounted root: syntactically clean, and CreatePackage would happily build a package
    // under it that nothing can ever save.
    ExportAnimSafetyExpectPathRefused(*this, Dispatcher, Sink,
        FString::Printf(TEXT("/PinWrightNotAMountedRoot/%s"),
            *ExportAnimSafetyUniqueLeaf(TEXT("Unmounted"))),
        TEXT("unmounted root"), TEXT("INVALID_PATH"));

    // CONTROL. A well-formed destination, driven with the SAME absent sequence, so the only
    // possible answer is the sequence refusal - which is what proves both that a valid path gets
    // past the new check and that the check runs above BakeResolveSequence.
    {
        const FString GoodPath = FString::Printf(TEXT("/Game/PinWrightTests/%s"),
            *ExportAnimSafetyUniqueLeaf(TEXT("Control")));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("outAssetPath"), GoodPath);
        Params->SetStringField(TEXT("sequence"), ExportAnimSafetyAbsentSequencePath());
        Params->SetStringField(TEXT("binding"), ExportAnimSafetyBindingGuid());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("sequencer.export_anim_sequence"),
            TEXT("req-export-anim-path-control"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the control call still fails - its sequence does not exist"), bSuccess);
        TestEqual(TEXT("a well-formed outAssetPath passes the path check and is refused on the "
                       "sequence instead"), ErrorCode, FString(TEXT("SEQUENCE_NOT_FOUND")));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        TestNull(TEXT("no package was created for a call that never got to the export"),
            FindObject<UObject>(nullptr, *FString::Printf(TEXT("%s.%s"), *GoodPath,
                *FPackageName::GetLongPackageAssetName(GoodPath))));
    }

    return true;
}
