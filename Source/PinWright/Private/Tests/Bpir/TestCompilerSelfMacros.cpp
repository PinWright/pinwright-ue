// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_MacroInstance.h"

using namespace CompilerTestUtils;

namespace
{
    UK2Node_MacroInstance* FindMacroInstanceBoundTo(UBlueprint* Blueprint, UEdGraph* MacroGraph)
    {
        if (!Blueprint || !MacroGraph)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph)
            {
                continue;
            }

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_MacroInstance* MacroInstance = Cast<UK2Node_MacroInstance>(Node);
                if (MacroInstance && MacroInstance->GetMacroGraph() == MacroGraph)
                {
                    return MacroInstance;
                }
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompilerSelfMacroResolutionTest,
    "PinWright.bpir.compiler.SelfMacroResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCompilerSelfMacroResolutionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SelfMacroResolutionBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP)
    {
        return false;
    }

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry macro MyMacro() {\n")
            TEXT("    call PrintString(InString: \"From local macro\")\n")
            TEXT("}"));
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
            {
                AddError(FString::Printf(TEXT("Macro compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Local macro compile succeeded"), Result.bSuccess);
        if (!Result.bSuccess)
        {
            return false;
        }
    }

    UEdGraph* LocalMacroGraph = FindMacroGraph(BP, TEXT("MyMacro"));
    TestNotNull(TEXT("Local macro graph exists"), LocalMacroGraph);
    if (!LocalMacroGraph)
    {
        return false;
    }

    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %local = macro MyMacro()\n")
            TEXT("}"));
        if (!Result.bSuccess)
        {
            for (const FCompileError& Err : Result.Errors)
            {
                AddError(FString::Printf(TEXT("Caller compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Caller compile succeeded"), Result.bSuccess);
        if (!Result.bSuccess)
        {
            return false;
        }
    }

    UK2Node_MacroInstance* Caller = FindMacroInstanceBoundTo(BP, LocalMacroGraph);
    TestNotNull(TEXT("Caller macro instance is bound to local macro graph"), Caller);
    if (!Caller)
    {
        return false;
    }

    TestEqual(TEXT("Caller resolves MyMacro to same-Blueprint macro graph"),
        Caller->GetMacroGraph(), LocalMacroGraph);
    return true;
}
