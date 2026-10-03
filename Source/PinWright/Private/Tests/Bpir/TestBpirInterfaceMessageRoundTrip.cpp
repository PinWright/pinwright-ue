// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirInterfaceMessageRoundTrip.cpp — regression test for
// B-bpir-interface-call-never-dispatches (re-scoped).
//
// The ticket was filed as "an interface call compiles clean and never dispatches" and
// retracted by its own reporter: a plain `call` on an interface-typed target does dispatch.
// What survived the retraction was the claim that `call K2Node_Message(...) node_props { ... }`
// "silently built a plain UK2Node_CallFunction instead". That claim was also wrong — the
// generic lane builds the named class (CodeNodeEmitter.cpp CreateGenericK2Node) — but it was
// UNFALSIFIABLE from the tooling, and that is the real defect this test pins:
//
//   UK2Node_Message derives from UK2Node_CallFunction (K2Node_Message.h:23), so
//   FGraphWalker::ClassifyNode's Cast<UK2Node_CallFunction> arm swallowed it and
//   FBpirTextEmitter::EmitCallNode printed it as a plain `call Foo(...)`, byte-identical to
//   an ordinary function call. A correct message node was indistinguishable from the bug the
//   reporter thought they were looking at, and feeding that text back through compile_bpir
//   silently replaced the message node with a plain call — a node-class change whose only
//   symptom is at runtime.
//
// Counterfactual: revert the `message` keyword and EmitCallNode emits `call`, so the
// "decompiled output names the message form" assertion fails; revert only the compile half
// and the recompile assertion finds a UK2Node_CallFunction that is not a UK2Node_Message.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Internationalization/Regex.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Message.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

using namespace CompilerTestUtils;
using BpirGraphTestHelpers::FindFirstNodeOfType;

namespace
{
    // A native Blueprint Interface that ships with the editor and is reachable from this
    // module (UMG is a hard dependency): UUserListEntry declares
    // BP_OnItemSelectionChanged(bool bIsSelected) as a BlueprintImplementableEvent, so it is
    // an impure, void, one-parameter interface function — exactly the shape a message call
    // takes. Resolved by path rather than by C++ type so the test needs no UMG header.
    const TCHAR* const MessageInterfacePath = TEXT("/Script/UMG.UserListEntry");
    const TCHAR* const MessageInterfaceName = TEXT("UserListEntry");
    const TCHAR* const MessageFunctionName = TEXT("BP_OnItemSelectionChanged");

    // Authored `@(x, y)` positions make the recompile run in authored-position mode, which
    // requires every node-backed instruction to carry one; the implicit K2Node_Self helper
    // behind `Target: self` has no positioned instruction. Strip them so the recompile runs
    // in auto-layout mode — the exercise here is node class, not layout.
    FString StripMessageTestAuthoredPositions(const FString& In)
    {
        const FRegexPattern PositionPattern(TEXT("\\s*@\\(\\s*-?\\d+\\s*,\\s*-?\\d+\\s*\\)"));
        FRegexMatcher Matcher(PositionPattern, In);
        FString Result;
        int32 Cursor = 0;
        while (Matcher.FindNext())
        {
            Result.Append(In.Mid(Cursor, Matcher.GetMatchBeginning() - Cursor));
            Cursor = Matcher.GetMatchEnding();
        }
        Result.Append(In.Mid(Cursor));
        return Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirInterfaceMessageRoundTripTest,
    "PinWright.bpir.round_trip.InterfaceMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirInterfaceMessageRoundTripTest::RunTest(const FString& Parameters)
{
    UClass* InterfaceClass = FindObject<UClass>(nullptr, MessageInterfacePath);
    UFunction* InterfaceFunction =
        InterfaceClass ? InterfaceClass->FindFunctionByName(FName(MessageFunctionName)) : nullptr;
    if (!InterfaceClass || !InterfaceFunction)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("message-interface-fixture-absent"),
            FString::Printf(TEXT("'%s' or its '%s' function is not registered on this host"),
                MessageInterfacePath, MessageFunctionName));
        return true;
    }

    const FString MessageLine = FString::Printf(TEXT("message %s::%s(Target: self, bIsSelected: true)"),
        MessageInterfaceName, MessageFunctionName);

    // --- Compile: `message` must build UK2Node_Message, not UK2Node_CallFunction. ---
    UBlueprint* BP = CreateTransientTestBP(TEXT("InterfaceMessageBP"));
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        return false;
    }

    const FString SourceBpir = FString::Printf(TEXT("entry event BeginPlay() {\n    %s\n}"), *MessageLine);
    FBpirCompiler Compiler(BP);
    const FCompileResult CompileResult = Compiler.Compile(SourceBpir);
    for (const FCompileError& Err : CompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
    }
    if (!TestTrue(TEXT("message call compiles"), CompileResult.bSuccess))
    {
        return false;
    }
    TestNotNull(TEXT("message call emits a UK2Node_Message"), FindFirstNodeOfType<UK2Node_Message>(BP));

    // --- Decompile: the message form must be named, not printed as a plain `call`. ---
    FBpirDecompiler Decompiler(BP);
    const FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    if (!TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess))
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString ExpectedCall = FString::Printf(TEXT("message %s::%s("),
        MessageInterfaceName, MessageFunctionName);
    if (!TestTrue(
            FString::Printf(TEXT("Decompiled output contains '%s' (got: %s)"),
                *ExpectedCall, *DecompileResult.BpirText),
            DecompileResult.BpirText.Contains(ExpectedCall)))
    {
        return false;
    }
    // The self pin of a message node has no self-context fallback, so `Target: self` must
    // survive rather than being dropped as redundant the way a plain call's is.
    TestTrue(TEXT("Decompiled message keeps its Target"),
        DecompileResult.BpirText.Contains(TEXT("Target: self")));

    // --- Recompile: the round trip must land on a message node again. ---
    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("InterfaceMessageRecompileBP"));
    if (!TestNotNull(TEXT("Recompile Blueprint created"), RecompileBP))
    {
        return false;
    }
    FBpirCompiler RecompileCompiler(RecompileBP);
    const FCompileResult RecompileResult =
        RecompileCompiler.Compile(StripMessageTestAuthoredPositions(DecompileResult.BpirText));
    for (const FCompileError& Err : RecompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("Decompiled message call recompiles"), RecompileResult.bSuccess);
    TestNotNull(TEXT("Recompiled graph still holds a UK2Node_Message"),
        FindFirstNodeOfType<UK2Node_Message>(RecompileBP));

    // --- Negative: `message` on a non-interface function is refused, not silently emitted. ---
    // A UK2Node_Message bound to an ordinary class function is a permanent no-op (ExpandNode
    // casts the target to an interface that does not exist), which is precisely the
    // silent-false-success this ticket was filed over.
    {
        UBlueprint* RejectBP = CreateTransientTestBP(TEXT("InterfaceMessageRejectBP"));
        if (TestNotNull(TEXT("Reject-case Blueprint created"), RejectBP))
        {
            FBpirCompiler RejectCompiler(RejectBP);
            const FCompileResult RejectResult = RejectCompiler.Compile(
                TEXT("entry event BeginPlay() {\n    message PrintString(InString: \"x\")\n}"));
            TestFalse(TEXT("message on a non-interface function fails the compile"), RejectResult.bSuccess);
            TestNull(TEXT("no message node is left behind by the refused compile"),
                FindFirstNodeOfType<UK2Node_Message>(RejectBP));
        }
    }

    return true;
}

// E-wiki-bpir-example-uncompilable-and-skel-qualifier-undocumented: the native fixture above has
// no skeleton twin, so it never exercised the path every Blueprint interface takes. A message
// node bound to a Blueprint-interface function resolves through the interface's
// SkeletonGeneratedClass, and GetFunctionDisplayName printed that owner verbatim:
// `message SKEL_BPI_X_C::Fn(...)` instead of the `BPI_X_C::Fn` the author wrote.
// Counterfactual: drop the ClassGeneratedBy -> GeneratedClass mapping in
// BpirTextEmitter.cpp GetFunctionDisplayName and the SKEL_ / exact-qualifier assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirBlueprintInterfaceMessageRoundTripTest,
    "PinWright.bpir.round_trip.BlueprintInterfaceMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBlueprintInterfaceMessageRoundTripTest::RunTest(const FString& Parameters)
{
    const FString InterfaceName = FString::Printf(
        TEXT("BPI_BpirMessageRoundTrip_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString InterfacePath =
        FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/%s"), *InterfaceName);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InterfacePath);
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), InterfaceName);
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(InterfacePath));
        Payload->SetStringField(TEXT("blueprintType"), TEXT("interface"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.create handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
        if (!TestTrue(FString::Printf(TEXT("interface created (%s %s)"), *Capture.ErrorCode, *Capture.Message),
                Capture.bSuccess))
        {
            return false;
        }
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), InterfacePath);
        Payload->SetStringField(TEXT("functionName"), TEXT("ProbePing"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.add_function handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.add_function"), Payload, Capture));
        if (!TestTrue(FString::Printf(TEXT("interface function added (%s %s)"), *Capture.ErrorCode, *Capture.Message),
                Capture.bSuccess))
        {
            return false;
        }
    }

    UBlueprint* InterfaceBP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToObjectPath(InterfacePath)));
    UClass* InterfaceClass = InterfaceBP ? InterfaceBP->GeneratedClass.Get() : nullptr;
    if (!TestNotNull(TEXT("interface generated class exists"), InterfaceClass)
        || !TestNotNull(TEXT("fixture precondition: interface declares ProbePing"),
            InterfaceClass->FindFunctionByName(TEXT("ProbePing"))))
    {
        return false;
    }

    UBlueprint* CallerBP = CreateTransientTestBP(TEXT("BlueprintInterfaceMessageBP"));
    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("BlueprintInterfaceMessageRecompileBP"));
    // Declared after the interface cleanup so it runs first: callers must not outlive the interface.
    ON_SCOPE_EXIT
    {
        for (UBlueprint* Target : {CallerBP, RecompileBP})
        {
            if (Target)
            {
                Target->ClearEditorReferences();
                PwTestAssetTeardown::DiscardLoadedAssetNoGc(Target);
                Target->MarkAsGarbage();
            }
        }
    };
    if (!TestNotNull(TEXT("caller Blueprint created"), CallerBP)
        || !TestNotNull(TEXT("recompile Blueprint created"), RecompileBP))
    {
        return false;
    }

    const FString Qualified = FString::Printf(TEXT("%s::ProbePing"), *InterfaceClass->GetName());
    FBpirCompiler Compiler(CallerBP);
    const FCompileResult CompileResult = Compiler.Compile(FString::Printf(
        TEXT("entry event BeginPlay() {\n    message %s(Target: self)\n}"), *Qualified));
    for (const FCompileError& Err : CompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
    }
    if (!TestTrue(TEXT("Blueprint-interface message compiles"), CompileResult.bSuccess)
        || !TestNotNull(TEXT("compile emits a UK2Node_Message"), FindFirstNodeOfType<UK2Node_Message>(CallerBP)))
    {
        return false;
    }

    FBpirDecompiler Decompiler(CallerBP);
    const FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    if (!TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess))
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("decompile names the generated class 'message %s(' (got: %s)"),
            *Qualified, *DecompileResult.BpirText),
        DecompileResult.BpirText.Contains(FString::Printf(TEXT("message %s("), *Qualified)));
    TestFalse(TEXT("decompile does not leak the SKEL_ skeleton-class qualifier"),
        DecompileResult.BpirText.Contains(TEXT("SKEL_")));

    FBpirCompiler RecompileCompiler(RecompileBP);
    const FCompileResult RecompileResult =
        RecompileCompiler.Compile(StripMessageTestAuthoredPositions(DecompileResult.BpirText));
    for (const FCompileError& Err : RecompileResult.Errors)
    {
        AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
    }
    TestTrue(TEXT("decompiled Blueprint-interface message recompiles"), RecompileResult.bSuccess);
    TestNotNull(TEXT("recompiled graph still holds a UK2Node_Message"),
        FindFirstNodeOfType<UK2Node_Message>(RecompileBP));

    return true;
}
