// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-ftext-localization-metadata-lost.
//
// Decompile path: when an FText pin default has a localization identity
// (namespace + key), BpirDecompiler emits a quoted NSLOCTEXT(...) BPIR string
// literal so the round-trip preserves namespace+key. Compile path:
// CoerceStringToPersistedFText already routes NSLOCTEXT(...) through
// FTextStringHelper::CreateFromBuffer, but the BPIR string-literal strip in
// FBpirValueResolver::GetLiteralText / FCodePinResolver::SetPinDefaultValue
// previously left literal backslashes in `\"` / `\\` sequences. The fix
// unescapes those after stripping the outer quotes.
//
// Coverage map (each scenario labelled honestly so reviewers can see what is
// actually exercised vs lifted into the test):
//   * Scenario 1 — compile-side resolution chain only. The decompile-side
//     formatter is mirrored in the test; the assertion that NSLOCTEXT round
//     trips is on the compile half.
//   * Scenario 2 — invariant compile-side fallthrough (bare quoted string).
//   * Scenario 3 — embedded-quote unescape on the PC_String compile path.
//   * Scenario 3b — backslash + quote unescape ordering.
//   * Scenario 4 — production decompile path. Builds a real BP with a
//     UK2Node_CallFunction whose FText input pin has a localized DefaultTextValue,
//     calls FBpirDecompiler::Decompile(), and asserts the resulting BPIR
//     contains the namespace + key strings. This is the only scenario that
//     would catch a revert of BpirDecompiler.cpp's namespace/key conditional;
//     scenarios 1-3b reach into the lifted formatter, not the production
//     decompiler.
//
// Counterfactual:
//   - If BpirDecompiler.cpp's namespace/key conditional emit is reverted,
//     scenario 4 fails because ResolveInputValue would emit only the bare
//     display string and the BPIR text would not contain the namespace or
//     key. (Scenarios 1-3b would still pass because they re-implement the
//     decompile-side format inside the test.)
//   - If the unescape step in FBpirValueResolver::GetLiteralText (or
//     FCodePinResolver) is reverted, scenario 1 fails because the
//     literal-backslashes form prevents FTextStringHelper::CreateFromBuffer
//     from parsing NSLOCTEXT(...). Scenario 3 fails because the resulting
//     DefaultValue keeps the literal backslashes.
//   - If the inner-substring pre-escape (introduced in fix iteration 1) is
//     reverted in BpirDecompiler.cpp, scenario 4b fails because the embedded
//     " inside the display string yields an unparseable NSLOCTEXT(...) form
//     that does not survive the BPIR transport-level escape round trip.

#include "Misc/AutomationTest.h"

#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextKey.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/BpirValueResolver.h"
#include "Compiler/CodePinResolver.h"
#include "Compiler/CompilerTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFTextLocalizationRoundTripTest,
    "PinWright.bpir.round_trip.FTextLocalization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFTextLocalizationRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace BpirStructLiteralUtils;

    // ------------------------------------------------------------------------
    // Scenario 1: Localized FText round-trip via the compiler-side path.
    //
    // We can't go through the full decompile path here because creating a
    // populated FText pin default and walking the BPIR emitter for that pin
    // alone would require duplicating the decompiler's per-pin emit harness.
    // Instead, exercise the two production primitives that together implement
    // the round-trip:
    //   * The decompile-side formatter that produces the BPIR string literal
    //     (FString::Printf + EscapeBpirString — same expression the decompiler
    //     uses, lifted into the test).
    //   * The compile-side resolution chain
    //     (FBpirValueResolver::GetLiteralText → FCodePinResolver::SetPinDefaultValue).
    // ------------------------------------------------------------------------
    {
        const FString OriginalNamespace = TEXT("BPIRRoundTrip");
        const FString OriginalKey = TEXT("HelloKey");
        // Display string deliberately has no embedded quotes; the NSLOCTEXT wire
        // format does not itself encode escaped inner quotes — that limitation is
        // separate from the BPIR transport-level escape this test exercises.
        const FString OriginalDisplay = TEXT("Hello World");

        FText OriginalText = FText::ChangeKey(
            FTextKey(OriginalNamespace),
            FTextKey(OriginalKey),
            FText::FromString(OriginalDisplay));

        // Sanity: FText::ChangeKey set the identity.
        const TOptional<FString> OriginalNs = FTextInspector::GetNamespace(OriginalText);
        const TOptional<FString> OriginalK = FTextInspector::GetKey(OriginalText);
        TestTrue(TEXT("Original FText has namespace"), OriginalNs.IsSet() && !OriginalNs.GetValue().IsEmpty());
        TestTrue(TEXT("Original FText has key"), OriginalK.IsSet() && !OriginalK.GetValue().IsEmpty());

        // ----- Decompile-side: format BPIR text exactly as BpirDecompiler does.
        const FString NsLocText = FString::Printf(
            TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"),
            *OriginalNs.GetValue(),
            *OriginalK.GetValue(),
            *OriginalText.ToString());
        const FString BpirLiteral = EscapeBpirString(NsLocText);

        // Outer quotes wrap the NSLOCTEXT form; inner quotes are escaped.
        TestTrue(TEXT("BPIR literal starts with quote"), BpirLiteral.StartsWith(TEXT("\"")));
        TestTrue(TEXT("BPIR literal ends with quote"), BpirLiteral.EndsWith(TEXT("\"")));
        TestTrue(TEXT("BPIR literal contains namespace text"), BpirLiteral.Contains(OriginalNamespace));
        TestTrue(TEXT("BPIR literal contains key text"), BpirLiteral.Contains(OriginalKey));
        TestTrue(TEXT("BPIR literal carries the NSLOCTEXT( prefix after strip"),
            BpirLiteral.Contains(TEXT("NSLOCTEXT(")));

        // ----- Compile-side: GetLiteralText strips outer quotes AND unescapes
        // inner \" / \\, producing a clean NSLOCTEXT(...) string that the FText
        // parser understands.
        const FString CompileInput = FBpirValueResolver::GetLiteralText(BpirLiteral);
        TestFalse(TEXT("GetLiteralText output has no leading quote"), CompileInput.StartsWith(TEXT("\"")));
        TestFalse(TEXT("GetLiteralText output has no escape backslashes"),
            CompileInput.Contains(TEXT("\\\"")));
        TestTrue(TEXT("GetLiteralText output begins with NSLOCTEXT("),
            CompileInput.StartsWith(TEXT("NSLOCTEXT(")));

        // Apply the BPIR-literal value to a fresh PC_Text pin via the production resolver.
        UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph);
        Graph->AddNode(Node);
        UEdGraphPin* Pin = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, FName(TEXT("InText")));
        TestNotNull(TEXT("Text pin created"), Pin);
        if (!Pin) return false;

        FString Err;
        const bool bApplied = FCodePinResolver::SetPinDefaultValue(Pin, CompileInput, &Err);
        TestTrue(FString::Printf(TEXT("SetPinDefaultValue accepts NSLOCTEXT BPIR literal (err=%s)"), *Err), bApplied);

        // The recovered FText should carry the original namespace and key.
        const TOptional<FString> RecoveredNs = FTextInspector::GetNamespace(Pin->DefaultTextValue);
        const TOptional<FString> RecoveredKey = FTextInspector::GetKey(Pin->DefaultTextValue);
        const FString* RecoveredSource = FTextInspector::GetSourceString(Pin->DefaultTextValue);
        TestTrue(TEXT("Recovered FText has namespace"), RecoveredNs.IsSet());
        TestTrue(TEXT("Recovered FText has key"), RecoveredKey.IsSet());
        if (RecoveredNs.IsSet())
        {
            TestEqual(TEXT("Round-trip namespace matches"), RecoveredNs.GetValue(), OriginalNamespace);
        }
        if (RecoveredKey.IsSet())
        {
            TestEqual(TEXT("Round-trip key matches"), RecoveredKey.GetValue(), OriginalKey);
        }
        TestNotNull(TEXT("Recovered FText source string set"), RecoveredSource);
        if (RecoveredSource)
        {
            TestEqual(TEXT("Round-trip display string matches"), *RecoveredSource, OriginalDisplay);
        }
    }

    // ------------------------------------------------------------------------
    // Scenario 2: Invariant FText (no namespace/key) — backward compat.
    //
    // The decompile-side namespace/key check must fall back to the bare quoted
    // display form. The compile-side resolver still requires an identity for
    // a fresh pin (no existing text to inherit from), but the produced literal
    // shape itself must be the bare quoted form (no NSLOCTEXT wrapper) so
    // existing tests / downstream consumers don't change behavior.
    // ------------------------------------------------------------------------
    {
        FText InvariantText = FText::FromString(TEXT("Invariant"));
        const TOptional<FString> NoNs = FTextInspector::GetNamespace(InvariantText);
        const TOptional<FString> NoKey = FTextInspector::GetKey(InvariantText);
        const bool bHasIdentity = NoNs.IsSet() && !NoNs.GetValue().IsEmpty()
            && NoKey.IsSet() && !NoKey.GetValue().IsEmpty();

        // Same branch the decompiler now uses: identity-present → NSLOCTEXT,
        // else bare quoted display string.
        FString BpirLiteral;
        if (bHasIdentity)
        {
            BpirLiteral = EscapeBpirString(FString::Printf(
                TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"),
                *NoNs.GetValue(), *NoKey.GetValue(), *InvariantText.ToString()));
        }
        else
        {
            BpirLiteral = EscapeBpirString(InvariantText.ToString());
        }

        TestFalse(TEXT("Invariant decompile path emits no NSLOCTEXT wrapper"),
            BpirLiteral.Contains(TEXT("NSLOCTEXT(")));
        TestEqual(TEXT("Invariant decompile yields bare quoted display string"),
            BpirLiteral, FString(TEXT("\"Invariant\"")));

        // Compile side: GetLiteralText still strips quotes cleanly. The pin
        // application would fail without an existing identity; that's the
        // pre-existing CoerceStringToPersistedFText contract, not a regression.
        const FString StripResult = FBpirValueResolver::GetLiteralText(BpirLiteral);
        TestEqual(TEXT("Invariant strip yields display string"), StripResult, FString(TEXT("Invariant")));
    }

    // ------------------------------------------------------------------------
    // Scenario 3: Embedded-quote unescape covers the CodePinResolver fix.
    //
    // Synthesize a string-literal pin default that round-trips through the BPIR
    // escape (mirroring what the decompile path emits for any string with a
    // literal quote in it), then drive it through CodePinResolver's PC_String
    // path (the secondary strip site at the bottom of the resolver). Assert
    // the resulting DefaultValue has unescaped quotes — not literal "\"".
    // ------------------------------------------------------------------------
    {
        const FString OriginalDisplay = TEXT("She said \"hi\" and went home.");
        const FString BpirLiteral = EscapeBpirString(OriginalDisplay);

        // Verify the escape produced the expected wire form: outer quotes,
        // inner backslash-escaped quotes.
        TestTrue(TEXT("Escaped form starts with \""), BpirLiteral.StartsWith(TEXT("\"")));
        TestTrue(TEXT("Escaped form ends with \""), BpirLiteral.EndsWith(TEXT("\"")));
        TestTrue(TEXT("Escaped form contains \\\""), BpirLiteral.Contains(TEXT("\\\"")));

        // Drive through CodePinResolver's PC_String fallback path.
        UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph);
        Graph->AddNode(Node);
        UEdGraphPin* StringPin = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, FName(TEXT("InString")));
        TestNotNull(TEXT("String pin created"), StringPin);
        if (!StringPin) return false;

        FString Err;
        const bool bApplied = FCodePinResolver::SetPinDefaultValue(StringPin, BpirLiteral, &Err);
        TestTrue(FString::Printf(TEXT("SetPinDefaultValue accepted escaped string (err=%s)"), *Err), bApplied);
        TestEqual(TEXT("DefaultValue has unescaped embedded quotes"),
            StringPin->DefaultValue, OriginalDisplay);

        // Also verify FBpirValueResolver::GetLiteralText (the primary strip
        // site used during compile) does the same unescape.
        const FString StripResult = FBpirValueResolver::GetLiteralText(BpirLiteral);
        TestEqual(TEXT("GetLiteralText unescapes embedded quotes"),
            StripResult, OriginalDisplay);
    }

    // ------------------------------------------------------------------------
    // Scenario 3b: backslash unescape ordering — verifies that legitimate \\\"
    // escape sequences (i.e. a literal backslash followed by a literal quote
    // in the source string) survive the unescape without double-collapsing.
    // ------------------------------------------------------------------------
    {
        const FString OriginalDisplay = TEXT("path\\\"trailing");  // backslash then quote
        const FString BpirLiteral = EscapeBpirString(OriginalDisplay);
        const FString StripResult = FBpirValueResolver::GetLiteralText(BpirLiteral);
        TestEqual(TEXT("Backslash + quote round-trips through escape/unescape"),
            StripResult, OriginalDisplay);
    }

    // ------------------------------------------------------------------------
    // Scenario 4: production decompile path — closes the counterfactual gap.
    //
    // Build a real Blueprint with a UK2Node_CallFunction (PrintText) whose FText
    // input pin carries an explicit namespace+key on its DefaultTextValue (set
    // via FText::ChangeKey, the same primitive the original repro uses). Wire
    // it onto BeginPlay so the production decompiler walks it as part of the
    // event chain, then call FBpirDecompiler::Decompile() — the same entry
    // point the blueprint.decompile RPC uses — and assert the produced BPIR
    // text contains both the namespace and key strings.
    //
    // If BpirDecompiler.cpp's namespace/key conditional emit (around the
    // ResolveInputValue FText branch) is reverted, this assertion fails because
    // the decompiler would emit only the bare display string and the BPIR
    // text would not contain the namespace or key. Scenarios 1-3 lift the
    // decompile-side formatter into the test body and would still pass.
    // ------------------------------------------------------------------------
    {
        const FString OriginalNamespace = TEXT("BPIRDecompProd");
        const FString OriginalKey = TEXT("PrintTextKey");
        const FString OriginalDisplay = TEXT("Prod path display");

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("FTextDecompProdBP_%d"), FMath::Rand())),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        TestNotNull(TEXT("Blueprint created"), BP);
        if (!BP) return false;
        if (BP->UbergraphPages.Num() == 0)
        {
            AddError(TEXT("Blueprint has no event graph"));
            return false;
        }
        UEdGraph* EventGraph = BP->UbergraphPages[0];

        // BeginPlay event so the call is on a walked exec chain.
        UK2Node_Event* BeginPlayNode = nullptr;
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
            {
                if (Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
                {
                    BeginPlayNode = Event;
                    break;
                }
            }
        }
        if (!BeginPlayNode)
        {
            BeginPlayNode = NewObject<UK2Node_Event>(EventGraph);
            BeginPlayNode->CreateNewGuid();
            BeginPlayNode->PostPlacedNewNode();
            BeginPlayNode->EventReference.SetExternalMember(
                TEXT("ReceiveBeginPlay"), AActor::StaticClass());
            BeginPlayNode->bOverrideFunction = true;
            BeginPlayNode->AllocateDefaultPins();
            EventGraph->AddNode(BeginPlayNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
            BeginPlayNode->ReconstructNode();
        }
        TestNotNull(TEXT("BeginPlay node placed"), BeginPlayNode);
        if (!BeginPlayNode) return false;

        // PrintText call — its InText pin is PC_Text with a localizable default.
        UK2Node_CallFunction* PrintTextNode = NewObject<UK2Node_CallFunction>(EventGraph);
        PrintTextNode->CreateNewGuid();
        PrintTextNode->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText),
            UKismetSystemLibrary::StaticClass());
        PrintTextNode->PostPlacedNewNode();
        PrintTextNode->AllocateDefaultPins();
        EventGraph->AddNode(PrintTextNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        PrintTextNode->ReconstructNode();

        // Wire BeginPlay.then -> PrintText.execute.
        UEdGraphPin* ThenPin = BeginPlayNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* ExecPin = PrintTextNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        TestNotNull(TEXT("BeginPlay 'then' pin"), ThenPin);
        TestNotNull(TEXT("PrintText 'execute' pin"), ExecPin);
        if (ThenPin && ExecPin)
        {
            ThenPin->MakeLinkTo(ExecPin);
        }

        UEdGraphPin* InTextPin = PrintTextNode->FindPin(FName(TEXT("InText")), EGPD_Input);
        TestNotNull(TEXT("PrintText 'InText' pin"), InTextPin);
        if (!InTextPin) return false;
        TestEqual(TEXT("'InText' is PC_Text"),
            InTextPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Text);

        // Stamp a localized default directly on the pin's DefaultTextValue.
        InTextPin->DefaultTextValue = FText::ChangeKey(
            FTextKey(OriginalNamespace),
            FTextKey(OriginalKey),
            FText::FromString(OriginalDisplay));

        // Sanity: the pin really carries the identity we are about to decompile.
        const TOptional<FString> StampedNs = FTextInspector::GetNamespace(InTextPin->DefaultTextValue);
        const TOptional<FString> StampedKey = FTextInspector::GetKey(InTextPin->DefaultTextValue);
        TestTrue(TEXT("Stamped namespace present"),
            StampedNs.IsSet() && StampedNs.GetValue() == OriginalNamespace);
        TestTrue(TEXT("Stamped key present"),
            StampedKey.IsSet() && StampedKey.GetValue() == OriginalKey);

        // Production decompile entry point — same one blueprint.decompile uses.
        FBpirDecompiler Decompiler(BP);
        FBpirDecompileResult Result = Decompiler.Decompile();
        TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);

        TestTrue(TEXT("BPIR text contains the FText namespace"),
            Result.BpirText.Contains(OriginalNamespace));
        TestTrue(TEXT("BPIR text contains the FText key"),
            Result.BpirText.Contains(OriginalKey));
        TestTrue(TEXT("BPIR text contains the NSLOCTEXT( wrapper"),
            Result.BpirText.Contains(TEXT("NSLOCTEXT(")));
    }

    // ------------------------------------------------------------------------
    // Scenario 4b: production decompile path with embedded " in the display
    // string. Exercises the inner-NSLOCTEXT pre-escape introduced in fix
    // iteration 1 — without it, the embedded quote inside the format string
    // produces an unparseable NSLOCTEXT(...) wire form. The round trip is
    // closed by re-parsing the inner BPIR string back through GetLiteralText
    // and feeding it to FCodePinResolver::SetPinDefaultValue, asserting the
    // recovered FText carries the original namespace + key.
    // ------------------------------------------------------------------------
    {
        const FString OriginalNamespace = TEXT("BPIRDecompProd");
        const FString OriginalKey = TEXT("EmbeddedQuoteKey");
        const FString OriginalDisplay = TEXT("She said \"hi\" today");

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("FTextDecompEmbedQuoteBP_%d"), FMath::Rand())),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!BP || BP->UbergraphPages.Num() == 0) return false;
        UEdGraph* EventGraph = BP->UbergraphPages[0];

        UK2Node_Event* BeginPlayNode = nullptr;
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
            {
                if (Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
                {
                    BeginPlayNode = Event;
                    break;
                }
            }
        }
        if (!BeginPlayNode)
        {
            BeginPlayNode = NewObject<UK2Node_Event>(EventGraph);
            BeginPlayNode->CreateNewGuid();
            BeginPlayNode->PostPlacedNewNode();
            BeginPlayNode->EventReference.SetExternalMember(
                TEXT("ReceiveBeginPlay"), AActor::StaticClass());
            BeginPlayNode->bOverrideFunction = true;
            BeginPlayNode->AllocateDefaultPins();
            EventGraph->AddNode(BeginPlayNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
            BeginPlayNode->ReconstructNode();
        }
        if (!BeginPlayNode) return false;

        UK2Node_CallFunction* PrintTextNode = NewObject<UK2Node_CallFunction>(EventGraph);
        PrintTextNode->CreateNewGuid();
        PrintTextNode->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText),
            UKismetSystemLibrary::StaticClass());
        PrintTextNode->PostPlacedNewNode();
        PrintTextNode->AllocateDefaultPins();
        EventGraph->AddNode(PrintTextNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        PrintTextNode->ReconstructNode();

        UEdGraphPin* ThenPin = BeginPlayNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* ExecPin = PrintTextNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (ThenPin && ExecPin)
        {
            ThenPin->MakeLinkTo(ExecPin);
        }

        UEdGraphPin* InTextPin = PrintTextNode->FindPin(FName(TEXT("InText")), EGPD_Input);
        if (!InTextPin) return false;
        InTextPin->DefaultTextValue = FText::ChangeKey(
            FTextKey(OriginalNamespace),
            FTextKey(OriginalKey),
            FText::FromString(OriginalDisplay));

        FBpirDecompiler Decompiler(BP);
        FBpirDecompileResult Result = Decompiler.Decompile();
        TestTrue(TEXT("Decompile succeeded (embedded-quote)"), Result.bSuccess);

        // The decompiler must emit a NSLOCTEXT(...) wire form whose inner
        // namespace/key/display literals are escaped. Round-trip the emitted
        // BPIR string through the compile-side strip (GetLiteralText) and
        // FCodePinResolver to a fresh PC_Text pin — the recovered FText must
        // carry the original namespace+key.
        // We don't feed the whole decompiled program back through the
        // compiler here — the production compile path requires more wiring;
        // exercising the pin-default round trip is sufficient to prove the
        // inner pre-escape produces a parseable NSLOCTEXT.
        // Locate the BPIR fragment for InText: search for the namespace
        // substring and walk outward to the surrounding "..." token.
        const int32 NsIdx = Result.BpirText.Find(OriginalNamespace);
        TestTrue(TEXT("Decompiled BPIR contains stamped namespace"), NsIdx != INDEX_NONE);
        if (NsIdx == INDEX_NONE) return false;

        // Find the opening quote of the BPIR string literal — scan backwards
        // for the first unescaped " before NsIdx that starts the literal.
        int32 OpenQuoteIdx = INDEX_NONE;
        for (int32 i = NsIdx; i >= 0; --i)
        {
            if (Result.BpirText[i] == TEXT('"'))
            {
                // Outer-quote of a BPIR literal is preceded by whitespace, ':'
                // or '(' — never by a backslash (otherwise it's an escaped
                // inner quote).
                if (i == 0 || Result.BpirText[i - 1] != TEXT('\\'))
                {
                    OpenQuoteIdx = i;
                    break;
                }
            }
        }
        TestTrue(TEXT("Found BPIR literal opening quote"), OpenQuoteIdx != INDEX_NONE);
        if (OpenQuoteIdx == INDEX_NONE) return false;

        // Find the matching closing unescaped quote.
        int32 CloseQuoteIdx = INDEX_NONE;
        for (int32 i = OpenQuoteIdx + 1; i < Result.BpirText.Len(); ++i)
        {
            if (Result.BpirText[i] == TEXT('"') && Result.BpirText[i - 1] != TEXT('\\'))
            {
                CloseQuoteIdx = i;
                break;
            }
        }
        TestTrue(TEXT("Found BPIR literal closing quote"), CloseQuoteIdx != INDEX_NONE);
        if (CloseQuoteIdx == INDEX_NONE) return false;

        const FString BpirLiteral = Result.BpirText.Mid(OpenQuoteIdx, CloseQuoteIdx - OpenQuoteIdx + 1);
        TestTrue(TEXT("BPIR literal carries the NSLOCTEXT( prefix"),
            BpirLiteral.Contains(TEXT("NSLOCTEXT(")));

        // Drive through the production compile-side resolver onto a fresh PC_Text pin.
        UEdGraph* TempGraph = NewObject<UEdGraph>(GetTransientPackage());
        UEdGraphNode* TempNode = NewObject<UEdGraphNode>(TempGraph);
        TempGraph->AddNode(TempNode);
        UEdGraphPin* TempPin = TempNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, FName(TEXT("In")));
        TestNotNull(TEXT("Temp PC_Text pin created"), TempPin);
        if (!TempPin) return false;

        const FString CompileInput = FBpirValueResolver::GetLiteralText(BpirLiteral);
        FString Err;
        const bool bApplied = FCodePinResolver::SetPinDefaultValue(TempPin, CompileInput, &Err);
        TestTrue(FString::Printf(TEXT("SetPinDefaultValue accepts decompiled NSLOCTEXT (err=%s)"), *Err), bApplied);

        const TOptional<FString> RecoveredNs = FTextInspector::GetNamespace(TempPin->DefaultTextValue);
        const TOptional<FString> RecoveredKey = FTextInspector::GetKey(TempPin->DefaultTextValue);
        const FString* RecoveredSource = FTextInspector::GetSourceString(TempPin->DefaultTextValue);
        TestTrue(TEXT("Recovered FText namespace matches"),
            RecoveredNs.IsSet() && RecoveredNs.GetValue() == OriginalNamespace);
        TestTrue(TEXT("Recovered FText key matches"),
            RecoveredKey.IsSet() && RecoveredKey.GetValue() == OriginalKey);
        TestNotNull(TEXT("Recovered FText source string"), RecoveredSource);
        if (RecoveredSource)
        {
            TestEqual(TEXT("Recovered FText display matches (embedded quote intact)"),
                *RecoveredSource, OriginalDisplay);
        }
    }

    // ------------------------------------------------------------------------
    // Scenario 5: identity-less FText (FText::FromString) — synthesized
    // NSLOCTEXT covers the bare-string round-trip break tracked in
    // B-bpir-format-text-bare-string-roundtrip-break.
    //
    // The F-require-ftext-localization-identity compile gate rejects bare
    // quoted FText defaults. The decompiler synthesizes a deterministic
    // <asset>/<nodeguid8>.<pin> identity for identity-less FText so the
    // BPIR re-compiles cleanly. This scenario covers:
    //   a) decompile of an identity-less FText pin produces NSLOCTEXT(...)
    //      with the synthesized namespace + key in the BPIR text.
    //   b) re-decompiling the same Blueprint produces the same identity
    //      (idempotency — same asset, same node GUID, same pin).
    //   c) the synthesized BPIR re-compiles cleanly through FBpirCompiler
    //      onto a fresh transient Blueprint, the F-require-ftext-localization
    //      -identity gate does not reject it, and the recompiled InText pin
    //      carries a non-empty namespace/key plus the original display text.
    // ------------------------------------------------------------------------
    {
        const FString OriginalDisplay = TEXT("Identity-less display");

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("FTextBareStringBP_%d"), FMath::Rand())),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        TestNotNull(TEXT("Bare-FText blueprint created"), BP);
        if (!BP || BP->UbergraphPages.Num() == 0) return false;
        UEdGraph* EventGraph = BP->UbergraphPages[0];

        UK2Node_Event* BeginPlayNode = nullptr;
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
            {
                if (Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
                {
                    BeginPlayNode = Event;
                    break;
                }
            }
        }
        if (!BeginPlayNode)
        {
            BeginPlayNode = NewObject<UK2Node_Event>(EventGraph);
            BeginPlayNode->CreateNewGuid();
            BeginPlayNode->PostPlacedNewNode();
            BeginPlayNode->EventReference.SetExternalMember(
                TEXT("ReceiveBeginPlay"), AActor::StaticClass());
            BeginPlayNode->bOverrideFunction = true;
            BeginPlayNode->AllocateDefaultPins();
            EventGraph->AddNode(BeginPlayNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
            BeginPlayNode->ReconstructNode();
        }
        if (!BeginPlayNode) return false;

        UK2Node_CallFunction* PrintTextNode = NewObject<UK2Node_CallFunction>(EventGraph);
        PrintTextNode->CreateNewGuid();
        PrintTextNode->FunctionReference.SetExternalMember(
            GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText),
            UKismetSystemLibrary::StaticClass());
        PrintTextNode->PostPlacedNewNode();
        PrintTextNode->AllocateDefaultPins();
        EventGraph->AddNode(PrintTextNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
        PrintTextNode->ReconstructNode();

        UEdGraphPin* ThenPin = BeginPlayNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        UEdGraphPin* ExecPin = PrintTextNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
        if (ThenPin && ExecPin) { ThenPin->MakeLinkTo(ExecPin); }

        UEdGraphPin* InTextPin = PrintTextNode->FindPin(FName(TEXT("InText")), EGPD_Input);
        TestNotNull(TEXT("PrintText 'InText' pin (bare-FText)"), InTextPin);
        if (!InTextPin) return false;

        // FText::FromString produces an FText with NO namespace/key identity,
        // which is the asset shape the round-trip break manifests on.
        InTextPin->DefaultTextValue = FText::FromString(OriginalDisplay);
        const TOptional<FString> StampedNs = FTextInspector::GetNamespace(InTextPin->DefaultTextValue);
        const TOptional<FString> StampedKey = FTextInspector::GetKey(InTextPin->DefaultTextValue);
        const bool bHasIdentity =
            StampedNs.IsSet() && !StampedNs.GetValue().IsEmpty()
            && StampedKey.IsSet() && !StampedKey.GetValue().IsEmpty();
        TestFalse(TEXT("Bare FText has no namespace/key identity"), bHasIdentity);

        // First decompile.
        FBpirDecompiler Decompiler(BP);
        FBpirDecompileResult Result = Decompiler.Decompile();
        TestTrue(TEXT("Decompile of bare-FText BP succeeded"), Result.bSuccess);
        if (!Result.bSuccess) return false;

        TestTrue(TEXT("Bare-FText decompile contains NSLOCTEXT( wrapper"),
            Result.BpirText.Contains(TEXT("NSLOCTEXT(")));
        TestTrue(TEXT("Bare-FText decompile contains display string"),
            Result.BpirText.Contains(OriginalDisplay));
        // Synthesized namespace == sanitized asset short name.
        const FString ExpectedNs = BP->GetName();
        TestTrue(TEXT("Bare-FText decompile contains synthesized namespace (asset short name)"),
            Result.BpirText.Contains(ExpectedNs));
        // Synthesized key contains the pin name segment.
        TestTrue(TEXT("Bare-FText decompile contains synthesized key with pin segment"),
            Result.BpirText.Contains(TEXT("InText")));

        // Idempotency: a second decompile of the same Blueprint must yield
        // the same NSLOCTEXT triple (same asset, same node GUID, same pin).
        FBpirDecompiler Decompiler2(BP);
        FBpirDecompileResult Result2 = Decompiler2.Decompile();
        TestTrue(TEXT("Second decompile of bare-FText BP succeeded"), Result2.bSuccess);
        TestEqual(TEXT("Bare-FText decompile is idempotent across re-runs"),
            Result2.BpirText, Result.BpirText);

        // Re-compile the synthesized BPIR onto a fresh transient AActor BP to
        // prove the F-require-ftext-localization-identity gate accepts the
        // decompiler-synthesized identity. Mirrors the recompile pattern used
        // by FBpirRoundTripFNamePinDefaultQuotedTest in TestBpirRoundTrip.cpp.
        UBlueprint* RecompileBP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            FName(*FString::Printf(TEXT("FTextBareStringRecompileBP_%d"), FMath::Rand())),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        TestNotNull(TEXT("Recompile-target blueprint created"), RecompileBP);
        if (!RecompileBP) return false;

        FBpirCompiler Compiler(RecompileBP);
        FCompileResult Recompile = Compiler.Compile(Result.BpirText);
        if (!Recompile.bSuccess)
        {
            for (const FCompileError& Err : Recompile.Errors)
            {
                AddError(FString::Printf(TEXT("Bare-FText re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Bare-FText synthesized BPIR re-compiles cleanly (gate accepts synthesized identity)"),
            Recompile.bSuccess);
        TestEqual(TEXT("Bare-FText re-compile produced no errors"), Recompile.Errors.Num(), 0);
        if (!Recompile.bSuccess) return false;

        // Locate the recompiled PrintText call and inspect its InText pin.
        UK2Node_CallFunction* RecompiledPrintText = nullptr;
        TArray<UEdGraph*> AllGraphs;
        AllGraphs.Append(RecompileBP->UbergraphPages);
        AllGraphs.Append(RecompileBP->FunctionGraphs);
        for (UEdGraph* G : AllGraphs)
        {
            if (!G) continue;
            for (UEdGraphNode* N : G->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N);
                if (Call && Call->FunctionReference.GetMemberName() ==
                    GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText))
                {
                    RecompiledPrintText = Call;
                    break;
                }
            }
            if (RecompiledPrintText) break;
        }
        TestNotNull(TEXT("Recompiled PrintText call exists"), RecompiledPrintText);
        if (!RecompiledPrintText) return false;

        UEdGraphPin* RecompiledInText = RecompiledPrintText->FindPin(FName(TEXT("InText")), EGPD_Input);
        TestNotNull(TEXT("Recompiled PrintText 'InText' pin exists"), RecompiledInText);
        if (!RecompiledInText) return false;

        // The synthesized identity must survive the compile gate — both
        // namespace and key non-empty on the recompiled pin.
        const TOptional<FString> RecompiledNs = FTextInspector::GetNamespace(RecompiledInText->DefaultTextValue);
        const TOptional<FString> RecompiledKey = FTextInspector::GetKey(RecompiledInText->DefaultTextValue);
        TestTrue(TEXT("Recompiled InText namespace is set"), RecompiledNs.IsSet());
        TestTrue(TEXT("Recompiled InText namespace is non-empty"),
            RecompiledNs.IsSet() && !RecompiledNs.GetValue().IsEmpty());
        TestTrue(TEXT("Recompiled InText key is set"), RecompiledKey.IsSet());
        TestTrue(TEXT("Recompiled InText key is non-empty"),
            RecompiledKey.IsSet() && !RecompiledKey.GetValue().IsEmpty());

        // Display string must match the original literal.
        const FString* RecompiledSource = FTextInspector::GetSourceString(RecompiledInText->DefaultTextValue);
        TestNotNull(TEXT("Recompiled InText source string is set"), RecompiledSource);
        if (RecompiledSource)
        {
            TestEqual(TEXT("Recompiled InText display string matches original literal"),
                *RecompiledSource, OriginalDisplay);
        }
    }
    return true;
}
