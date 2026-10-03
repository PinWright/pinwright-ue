// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirInputKeyDualActive.cpp - B-bpir-dual-active-inputkey. One UK2Node_InputKey with
// both Pressed and Released wired decompiled as a single `entry key_released` block: the
// Pressed body had no entry and vanished from the BPIR without a warning. Each active exec
// pin is now its own entry.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Decompiler/BpirInputKeyHelpers.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_InputKey.h"

namespace TestBpirInputKeyDualActiveHelpers
{
    TArray<UK2Node_InputKey*> FindInputKeyNodes(UBlueprint* BP)
    {
        TArray<UK2Node_InputKey*> Nodes;
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
                {
                    Nodes.Add(InputKeyNode);
                }
            }
        }
        return Nodes;
    }

    // Text of the entry block whose signature line starts with Signature, up to its closing brace.
    FString GetEntryBlockText(const FString& Text, const FString& Signature)
    {
        const int32 Start = Text.Find(Signature + TEXT(" "), ESearchCase::CaseSensitive);
        if (Start == INDEX_NONE)
        {
            return FString();
        }
        const int32 End = Text.Find(TEXT("\n}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
        return End == INDEX_NONE ? FString() : Text.Mid(Start, End - Start);
    }

    bool Compile(FAutomationTestBase& Test, UBlueprint* BP, const FString& Code)
    {
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(Code, EBpirCompileMode::Default);
        for (const FCompileError& Err : Result.Errors)
        {
            Test.AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
        return Test.TestTrue(TEXT("BPIR compiles"), Result.bSuccess);
    }

    FString Decompile(FAutomationTestBase& Test, UBlueprint* BP)
    {
        FBpirDecompiler Decompiler(BP);
        const FBpirDecompileResult Result = Decompiler.Decompile();
        Test.TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
        return Result.BpirText;
    }
}

// Fails unfixed: the dual-active node decompiled as one `entry key_released SpaceBar()`
// holding only the released body, so the pressed assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirInputKeyDualActiveDecompileTest,
    "PinWright.bpir.input_key_dual_active.EmitsBothEntriesAndRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirInputKeyDualActiveDecompileTest::RunTest(const FString& Parameters)
{
    using namespace TestBpirInputKeyDualActiveHelpers;

    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("InputKeyDualActiveBP"));
    if (!TestNotNull(TEXT("Blueprint was created"), BP)) return false;

    // The compiler authors one node per entry; fold the released body onto the pressed
    // node to get the legacy single-node shape the editor allows.
    if (!Compile(*this, BP,
        TEXT("entry key_pressed SpaceBar() {\n    call PrintString(InString: \"pressed body\")\n}\n")
        TEXT("entry key_released SpaceBar() {\n    call PrintString(InString: \"released body\")\n}")))
    {
        return false;
    }
    TArray<UK2Node_InputKey*> Nodes = FindInputKeyNodes(BP);
    if (!TestEqual(TEXT("Compile authored two InputKey nodes"), Nodes.Num(), 2)) return false;
    const bool bFirstIsPressed = FBpirInputKeyHelpers::IsInputKeyExecPinActive(Nodes[0], /*bReleased=*/false);
    UK2Node_InputKey* Keeper = bFirstIsPressed ? Nodes[0] : Nodes[1];
    UK2Node_InputKey* Donor = bFirstIsPressed ? Nodes[1] : Nodes[0];
    UEdGraphPin* DonorReleased = FBpirInputKeyHelpers::FindInputKeyExecPin(Donor, /*bReleased=*/true);
    UEdGraphPin* KeeperReleased = FBpirInputKeyHelpers::FindInputKeyExecPin(Keeper, /*bReleased=*/true);
    if (!TestNotNull(TEXT("Donor Released pin"), DonorReleased)
        || !TestNotNull(TEXT("Keeper Released pin"), KeeperReleased)
        || !TestEqual(TEXT("Donor Released is wired"), DonorReleased->LinkedTo.Num(), 1))
    {
        return false;
    }
    UEdGraphPin* ReleasedBodyExec = DonorReleased->LinkedTo[0];
    DonorReleased->BreakAllPinLinks();
    KeeperReleased->MakeLinkTo(ReleasedBodyExec);
    Donor->GetGraph()->RemoveNode(Donor);

    Nodes = FindInputKeyNodes(BP);
    if (!TestEqual(TEXT("Fixture has one InputKey node"), Nodes.Num(), 1)) return false;
    if (!TestTrue(TEXT("Fixture node has Pressed wired"),
            FBpirInputKeyHelpers::IsInputKeyExecPinActive(Nodes[0], /*bReleased=*/false))
        || !TestTrue(TEXT("Fixture node has Released wired"),
            FBpirInputKeyHelpers::IsInputKeyExecPinActive(Nodes[0], /*bReleased=*/true)))
    {
        return false;
    }

    const FString First = Decompile(*this, BP);
    const FString PressedBlock = GetEntryBlockText(First, TEXT("entry key_pressed SpaceBar()"));
    const FString ReleasedBlock = GetEntryBlockText(First, TEXT("entry key_released SpaceBar()"));
    TestTrue(FString::Printf(TEXT("Pressed entry carries the pressed body (text:\n%s)"), *First),
        PressedBlock.Contains(TEXT("\"pressed body\"")));
    TestFalse(TEXT("Pressed entry does not carry the released body"),
        PressedBlock.Contains(TEXT("\"released body\"")));
    TestTrue(TEXT("Released entry carries the released body"),
        ReleasedBlock.Contains(TEXT("\"released body\"")));
    TestFalse(TEXT("Released entry does not carry the pressed body"),
        ReleasedBlock.Contains(TEXT("\"pressed body\"")));

    // Round trip: the two entries compile to two single-pin nodes that decompile identically.
    // Both nodes sit at the same @(x, y), so this also pins FindEntryPoints' tie order (graph
    // node order); the unstable sort it replaced emitted key_released first here.
    UBlueprint* RoundTripBP = CompilerTestUtils::CreateTransientTestBP(TEXT("InputKeyDualActiveRoundTripBP"));
    if (!TestNotNull(TEXT("Round-trip Blueprint was created"), RoundTripBP)) return false;
    if (!Compile(*this, RoundTripBP, First)) return false;
    TestEqual(TEXT("Round trip authors one node per entry"), FindInputKeyNodes(RoundTripBP).Num(), 2);
    TestEqual(TEXT("Round-trip decompile is byte-identical"), Decompile(*this, RoundTripBP), First);
    return true;
}
