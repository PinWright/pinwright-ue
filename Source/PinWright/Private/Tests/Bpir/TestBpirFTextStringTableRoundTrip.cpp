// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextKey.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

#include "BpirGraphTestHelpers.h"
#include "Compiler/BpirValueResolver.h"
#include "Compiler/CodePinResolver.h"
#include "Utils/PropertyUtils.h"

#define EDITOR_AUTOMATION_TEST_TABLE_ID "PinWrightTests/Common"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFTextStringTableRoundTripTest,
    "PinWright.bpir.round_trip.FTextStringTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFTextStringTableRoundTripTest::RunTest(const FString& Parameters)
{
    const FName TableId = FName(TEXT(EDITOR_AUTOMATION_TEST_TABLE_ID));
    const FString KeyOk = TEXT("OK");
    const FString DisplayOk = TEXT("OK display");

    // Register a transient string table for the duration of the test.
    if (!FStringTableRegistry::Get().FindStringTable(TableId))
    {
        LOCTABLE_NEW(EDITOR_AUTOMATION_TEST_TABLE_ID, EDITOR_AUTOMATION_TEST_TABLE_ID);
        LOCTABLE_SETSTRING(EDITOR_AUTOMATION_TEST_TABLE_ID, "OK", "OK display");
    }
    ON_SCOPE_EXIT
    {
        FStringTableRegistry::Get().UnregisterStringTable(TableId);
    };

    // Compile-side: production CoerceStringToPersistedFText must accept
    // LOCTABLE(...) and produce a string-table-backed FText.
    {
        FText OutText;
        FString OutError;
        const FString Input = TEXT("LOCTABLE(\"") TEXT(EDITOR_AUTOMATION_TEST_TABLE_ID) TEXT("\", \"OK\")");
        const bool bOk = CoerceStringToPersistedFText(Input, nullptr, OutText, OutError);
        TestTrue(FString::Printf(TEXT("CoerceStringToPersistedFText accepts LOCTABLE (err=%s)"), *OutError), bOk);
        TestTrue(TEXT("Coerced FText IsFromStringTable"), OutText.IsFromStringTable());

        FName ParsedTable;
        FString ParsedKey;
        const bool bGotIds = FTextInspector::GetTableIdAndKey(OutText, ParsedTable, ParsedKey);
        TestTrue(TEXT("GetTableIdAndKey succeeded"), bGotIds);
        TestEqual(TEXT("Coerced TableId matches"), ParsedTable, TableId);
        TestEqual(TEXT("Coerced Key matches"), ParsedKey, KeyOk);
    }

    // Production decompile path: stamp a string-table-backed FText onto a real
    // PrintText pin default, run FBpirDecompiler, and assert the LOCTABLE wire
    // form survives.
    UBlueprint* DecompiledBP = nullptr;
    {
        UBlueprint* BP = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
        TestNotNull(TEXT("Blueprint created"), BP);
        if (!BP) return false;
        if (BP->UbergraphPages.Num() == 0)
        {
            AddError(TEXT("Blueprint has no event graph"));
            return false;
        }
        UEdGraph* EventGraph = BP->UbergraphPages[0];

        UK2Node_Event* BeginPlayNode = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
        TestNotNull(TEXT("BeginPlay node placed"), BeginPlayNode);
        if (!BeginPlayNode) return false;

        UK2Node_CallFunction* PrintTextNode = BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(EventGraph);
        PrintTextNode->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText),
            UKismetSystemLibrary::StaticClass());
        PrintTextNode->ReconstructNode();

        BpirGraphTestHelpers::WireExec(BeginPlayNode, PrintTextNode);

        UEdGraphPin* InTextPin = PrintTextNode->FindPin(FName(TEXT("InText")), EGPD_Input);
        TestNotNull(TEXT("PrintText 'InText' pin"), InTextPin);
        if (!InTextPin) return false;

        // Stamp a string-table-backed FText directly onto the pin default.
        InTextPin->DefaultTextValue = FText::FromStringTable(TableId, KeyOk);
        TestTrue(TEXT("Stamped DefaultTextValue is from string table"),
            InTextPin->DefaultTextValue.IsFromStringTable());

        FBpirDecompiler Decompiler(BP);
        FBpirDecompileResult Result = Decompiler.Decompile();
        TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

        // BPIR escapes the literal's outer quotes; the inner LOCTABLE form has
        // its quotes preserved as-is by EscapeBpirStringInner (no quote chars
        // in TableId or Key here). The substring assertion targets the inner
        // wire form.
        TestTrue(TEXT("BPIR text contains LOCTABLE wire form"),
            Result.BpirText.Contains(TEXT("LOCTABLE(\\\"") TEXT(EDITOR_AUTOMATION_TEST_TABLE_ID) TEXT("\\\", \\\"OK\\\")")));

        DecompiledBP = BP;
    }

    // Compile-side resolver round-trip on a fresh PC_Text pin.
    // Note: SetPinDefaultValue takes an *unescaped* literal — its caller in
    // FBpirValueResolver::GetLiteralText strips outer quotes and unescapes
    // inner sequences first, so pass raw LOCTABLE(...) text here.
    {
        UEdGraph* TempGraph = NewObject<UEdGraph>(GetTransientPackage());
        UEdGraphNode* TempNode = NewObject<UEdGraphNode>(TempGraph);
        TempGraph->AddNode(TempNode);
        UEdGraphPin* TempPin = TempNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, FName(TEXT("In")));
        TestNotNull(TEXT("Round-trip PC_Text pin created"), TempPin);
        if (!TempPin) return false;

        const FString CompileInput = TEXT("LOCTABLE(\"") TEXT(EDITOR_AUTOMATION_TEST_TABLE_ID) TEXT("\", \"OK\")");
        FString Err;
        const bool bApplied = FCodePinResolver::SetPinDefaultValue(TempPin, CompileInput, &Err);
        TestTrue(FString::Printf(TEXT("SetPinDefaultValue accepts LOCTABLE (err=%s)"), *Err), bApplied);
        TestTrue(TEXT("Round-trip pin's DefaultTextValue is from string table"),
            TempPin->DefaultTextValue.IsFromStringTable());

        FName RoundTripTable;
        FString RoundTripKey;
        const bool bGotIds = FTextInspector::GetTableIdAndKey(TempPin->DefaultTextValue, RoundTripTable, RoundTripKey);
        TestTrue(TEXT("Round-trip GetTableIdAndKey succeeded"), bGotIds);
        TestEqual(TEXT("Round-trip TableId matches original"), RoundTripTable, TableId);
        TestEqual(TEXT("Round-trip Key matches original"), RoundTripKey, KeyOk);
    }
    return true;
}

#undef EDITOR_AUTOMATION_TEST_TABLE_ID
