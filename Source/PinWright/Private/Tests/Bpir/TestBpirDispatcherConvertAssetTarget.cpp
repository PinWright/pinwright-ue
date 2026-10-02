// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-dispatcher-target-convertasset-output.
//
// The decompiler prints a dispatcher op whose Target is a Resolve Soft Reference
// output as `clear_dispatcher X(Target: %n1.Output)` with
// `%n1 = call K2Node_ConvertAsset(Input: $soft)`. The compiler used to read the
// ConvertAsset Output pin's class at emit time, while it was still a wildcard
// (its Input is linked later in the data-pin pass), so the dispatcher owner class
// resolved to null and the op failed with "Failed to create delegate node".
// The round trip also covers the decompiler: it used to print the bind's
// CreateDelegate as an extra `%n = call Create_Event(...)` line the compiler rejects.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Components/AudioComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_ConvertAsset.h"
#include "Internationalization/Regex.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDispatcherConvertAssetTargetTest,
    "PinWright.bpir.compiler.integration.Dispatcher_ConvertAssetOutputTargetRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDispatcherConvertAssetTargetTest::RunTest(const FString& Parameters)
{
    // BP with `Ext: softobject<AudioComponent>`; OnAudioFinished is a param-less
    // multicast delegate on UAudioComponent, not on the BP's own class.
    auto MakeBP = [this](const TCHAR* Name) -> UBlueprint*
    {
        UBlueprint* BP = CreateTransientTestBP(Name);
        if (BP)
        {
            FEdGraphPinType SoftType;
            SoftType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
            SoftType.PinSubCategoryObject = UAudioComponent::StaticClass();
            FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Ext"), SoftType);
            FKismetEditorUtilities::CompileBlueprint(BP);
        }
        return BP;
    };
    auto CompileOrReport = [this](UBlueprint* BP, const FString& Text, const TCHAR* What) -> FCompileResult
    {
        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(Text);
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("%s L%d: %s"), What, Err.Line, *Err.Message));
        }
        TestTrue(FString::Printf(TEXT("%s succeeded"), What), Result.bSuccess);
        return Result;
    };

    UBlueprint* BP = MakeBP(TEXT("DispatcherConvertAssetTargetBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    const FCompileResult Result = CompileOrReport(BP,
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %n1 = call K2Node_ConvertAsset(Input: $Ext)\n")
        TEXT("    clear_dispatcher OnAudioFinished(Target: %n1.Output)\n")
        TEXT("    bind_dispatcher OnAudioFinished(Target: %n1.Output, event: @Handler)\n")
        TEXT("}\n")
        TEXT("entry custom_event Handler() {\n")
        TEXT("}"),
        TEXT("BPIR compile"));
    if (!Result.bSuccess) return false;

    UK2Node_ConvertAsset* ConvertNode = nullptr;
    UK2Node_ClearDelegate* ClearNode = nullptr;
    UK2Node_AddDelegate* AddNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!ConvertNode) ConvertNode = Cast<UK2Node_ConvertAsset>(Node);
            if (!ClearNode)   ClearNode   = Cast<UK2Node_ClearDelegate>(Node);
            if (!AddNode)     AddNode     = Cast<UK2Node_AddDelegate>(Node);
        }
    }
    TestNotNull(TEXT("ConvertAsset node exists"), ConvertNode);
    TestNotNull(TEXT("ClearDelegate node exists"), ClearNode);
    TestNotNull(TEXT("AddDelegate node exists"), AddNode);
    if (!ConvertNode || !ClearNode || !AddNode) return false;

    UEdGraphPin* ConvertOut = ConvertNode->FindPin(TEXT("Output"), EGPD_Output);
    for (UK2Node* DelegateNode : { static_cast<UK2Node*>(ClearNode), static_cast<UK2Node*>(AddNode) })
    {
        UEdGraphPin* SelfPin = DelegateNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
        TestTrue(FString::Printf(TEXT("%s Target is wired to the ConvertAsset Output"), *DelegateNode->GetClass()->GetName()),
            SelfPin && ConvertOut && SelfPin->LinkedTo.Contains(ConvertOut));
    }

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    for (const FString& Err : Diagnostics.Errors)
    {
        AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);

    // Round trip: the decompiled text of this graph must compile again.
    FBpirDecompiler Decompiler(BP);
    const FBpirDecompileResult Decompiled = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Decompiled.bSuccess);
    if (!Decompiled.bSuccess) return false;
    TestTrue(TEXT("Decompiled text keeps the ConvertAsset call"), Decompiled.BpirText.Contains(TEXT("call K2Node_ConvertAsset(")));
    TestTrue(TEXT("Decompiled text targets the ConvertAsset Output"), Decompiled.BpirText.Contains(TEXT(".Output")));
    // bind_dispatcher already carries `event: @Handler`; a separate Create_Event line does not compile.
    TestFalse(TEXT("Decompiled text has no Create_Event line for the folded bind delegate"),
        Decompiled.BpirText.Contains(TEXT("Create_Event")));

    FString Stripped;
    {
        const FRegexPattern PositionPattern(TEXT("\\s*@\\(\\s*-?\\d+\\s*,\\s*-?\\d+\\s*\\)"));
        FRegexMatcher Matcher(PositionPattern, Decompiled.BpirText);
        int32 Cursor = 0;
        while (Matcher.FindNext())
        {
            Stripped.Append(Decompiled.BpirText.Mid(Cursor, Matcher.GetMatchBeginning() - Cursor));
            Cursor = Matcher.GetMatchEnding();
        }
        Stripped.Append(Decompiled.BpirText.Mid(Cursor));
    }

    UBlueprint* BPRecompile = MakeBP(TEXT("DispatcherConvertAssetTargetRecompileBP"));
    TestNotNull(TEXT("Recompile Blueprint was created"), BPRecompile);
    if (!BPRecompile) return false;
    const FCompileResult Recompiled = CompileOrReport(BPRecompile, Stripped, TEXT("Recompile of decompiled text"));
    if (!Recompiled.bSuccess)
    {
        AddInfo(FString::Printf(TEXT("Decompiled text:\n%s"), *Decompiled.BpirText));
    }
    return Recompiled.bSuccess;
}
