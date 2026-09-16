// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestRenderTargetCreatePathSafety.cpp - regression coverage for the
// render.create_render_target site of B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. The verb composed `packagePath / name` - both raw off the wire, both optional -
// and handed the result to CreatePackage unchecked. CreatePackage logs at **Fatal** for a name
// containing "//" (UObjectGlobals.cpp:1094-1096) and for one resolving to empty (:1118), and
// Fatal is not compiled out in any configuration: the call does not fail, it ends the editor
// PROCESS and every unsaved package in it. FString::operator/ is not a defence - PathAppend
// (Core/Private/Containers/String.cpp.inl:855-885) only avoids doubling a slash it would add
// itself, so `name: "a//b"` reached the Fatal regardless of the composition style. This site had
// no post-call null check at all, which changes nothing: nothing after the call is reached.
//
// THE CONSTRUCTION, AND WHY IT IS NOT THE FOLIAGE ONE. The full rationale is in
// Tests/Niagara/TestNiagaraCreatePathSafety.cpp - this verb has the same shape and the same
// problem: between the wire and CreatePackage a build with the guard removed passes NOTHING that
// could bail it out, so there is no payload carrying a "//" that such a build survives, and none
// is sent from here. Instead the "//" class is asserted against the guard itself
// (PinWrightComposeAssetPackagePath, a pure function that calls nothing) in that file, and this
// file asserts at the WIRE that this verb routes through that guard, using the one refusal class
// whose reverted-build behaviour is provably harmless: an UNMOUNTED package root. It composes a
// path with no "//" and no invalid characters, so a build without the guard hands CreatePackage a
// string it accepts, builds the render target under a clean FName and answers success - which the
// TestEqual on INVALID_ARGUMENT below scores red while the process lives. CreatePackage's two
// Fatals are unreachable on the fixed build (refused above the call) and on the reverted one (no
// "//", non-empty).
//
// Do NOT "strengthen" this by putting a slash-bearing `name` on the wire: on a build with the
// guard removed that payload does not fail the test, it ends the process running it.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace RenderTargetCreatePathSafetyHelpers
{
    // A root no project mounts: refused with PackageNamePathNotMounted while every character in
    // it stays legal, which is the property the no-Fatal argument in the header rests on.
    constexpr const TCHAR* RtPathSafetyUnmountedFolder =
        TEXT("/PinWrightNotAMountedRoot/RenderTargets");

    constexpr const TCHAR* RtPathSafetyScratchFolder = TEXT("/Game/PinWrightTests");

    inline FString RtPathSafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWRtPathSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

// ============================================================================
// render.create_render_target composes its package path through the guard
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCreateRenderTargetPathSafetyTest,
    "PinWright.render.create_render_target.PathIsGuardedBeforeCreatePackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCreateRenderTargetPathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace RenderTargetCreatePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString UnroutableName = RtPathSafetyUniqueName(TEXT("Unroutable"));
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), UnroutableName);
        Params->SetStringField(TEXT("packagePath"), RtPathSafetyUnmountedFolder);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.create_render_target"),
            TEXT("req-render-target-path-safety"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("render.create_render_target refuses a packagePath under no mounted root"),
            bSuccess);
        // The discriminator against a reverted guard: without it this payload composes a path
        // CreatePackage accepts, the verb builds the render target and answers success.
        TestEqual(TEXT("it is refused as a caller argument error"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        // Routing proof: only PinWrightComposeAssetPackagePath emits the composed candidate
        // alongside the engine's own package-path reason text.
        TestTrue(TEXT("the refusal quotes the composed path"),
            Sink->Message.Contains(FString::Printf(TEXT("%s/%s"),
                RtPathSafetyUnmountedFolder, *UnroutableName)));
        TestTrue(TEXT("the refusal surfaces the engine's own reason verbatim"),
            Sink->Message.Contains(TEXT("not a valid package path")));
    }

    // CONTROL. Without this a handler that refused every request would satisfy the case above.
    // Asserted as "not refused as a caller argument" rather than as full success, so it measures
    // the guard rather than render-target construction.
    {
        const FString ControlName = RtPathSafetyUniqueName(TEXT("Control"));
        // The handler leaves the package dirty; a later save-all would flush it to disk.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(FString::Printf(TEXT("%s/%s"),
                RtPathSafetyScratchFolder, *ControlName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("packagePath"), RtPathSafetyScratchFolder);
        Params->SetNumberField(TEXT("width"), 16.0);
        Params->SetNumberField(TEXT("height"), 16.0);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.create_render_target"),
            TEXT("req-render-target-path-control"), Params, bSuccess, ErrorCode);

        TestNotEqual(TEXT("a bare name under a mounted folder gets past the path guard"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}
