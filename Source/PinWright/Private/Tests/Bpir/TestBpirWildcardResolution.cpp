// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for three BPIR compile defects around pin-type resolution:
//
// - B-bpir-array-find-wildcard-not-notified: Array_Find / Contains / AddUnique /
//   RemoveItem already resolve their element pin from TargetArray (they are emitted as
//   UK2Node_CallArrayFunction). What the reporter hit was a real type mismatch (a Pawn
//   into an array typed by GetAllActorsOfClass's DeterminesOutputType), reported as
//   "TryCreateConnection failed wiring data 'X' -> 'ItemToFind'" with no types, which
//   reads like an unresolved wildcard. The wiring error now names both pin types.
// - E-bpir-select-literal-options-stay-wildcard: a Select whose options are only
//   literals (or links to other such Selects) stayed wildcard and failed the Blueprint
//   compile. TypeUndeterminedSelects now types it after data wiring (not in a macro
//   graph, where such a Select is legitimately generic).
// - B-bpir-break-struct-emits-generic-node: `break<Rotator>` / `break<HitResult>` now
//   emit the struct's native break function instead of a generic UK2Node_BreakStruct.

#include "Misc/AutomationTest.h"

#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/HitResult.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Select.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Decompiler/BpirDecompiler.h"

using namespace CompilerTestUtils;

namespace TestBpirWildcardResolutionHelpers
{
    void AddObjectVariable(UBlueprint* BP, const TCHAR* Name, UClass* Class, bool bArray)
    {
        FEdGraphPinType Type;
        Type.PinCategory = UEdGraphSchema_K2::PC_Object;
        Type.PinSubCategoryObject = Class;
        Type.ContainerType = bArray ? EPinContainerType::Array : EPinContainerType::None;
        FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type);
    }

    void AddVariable(UBlueprint* BP, const TCHAR* Name, FName Category, UObject* SubCategoryObject = nullptr)
    {
        FEdGraphPinType Type;
        Type.PinCategory = Category;
        if (Category == UEdGraphSchema_K2::PC_Real)
        {
            Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
        }
        Type.PinSubCategoryObject = SubCategoryObject;
        FBlueprintEditorUtils::AddMemberVariable(BP, Name, Type);
    }

    UK2Node_CallFunction* FindCallTo(UBlueprint* BP, const TCHAR* FunctionName)
    {
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                if (Call && Call->FunctionReference.GetMemberName() == FName(FunctionName))
                {
                    return Call;
                }
            }
        }
        return nullptr;
    }

    // The Select whose named option pin carries DefaultValue.
    UK2Node_Select* FindSelectWithOptionDefault(UBlueprint* BP, const TCHAR* OptionName, const TCHAR* DefaultValue)
    {
        for (UEdGraph* Graph : BP->UbergraphPages)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_Select* Select = Cast<UK2Node_Select>(Node);
                const UEdGraphPin* Option = Select ? Select->FindPin(OptionName, EGPD_Input) : nullptr;
                if (Option && Option->DefaultValue == DefaultValue)
                {
                    return Select;
                }
            }
        }
        return nullptr;
    }

    // Compiles Code into BP, reporting every compile error as a test error under Label.
    bool CompileOrReport(FAutomationTestBase& Test, UBlueprint* BP, const FString& Code, const TCHAR* Label)
    {
        FBpirCompiler Compiler(BP);
        const FCompileResult Result = Compiler.Compile(Code);
        for (const FCompileError& Err : Result.Errors)
        {
            Test.AddError(FString::Printf(TEXT("%s L%d: %s"), Label, Err.Line, *Err.Message));
        }
        return Result.bSuccess;
    }

    FString DecompileOrReport(FAutomationTestBase& Test, UBlueprint* BP)
    {
        FBpirDecompiler Decompiler(BP);
        const FBpirDecompileResult Result = Decompiler.Decompile();
        if (!Result.bSuccess)
        {
            Test.AddError(TEXT("Decompile failed"));
            return FString();
        }
        return Result.BpirText;
    }

    void AddBreakFixtureVariables(UBlueprint* BP)
    {
        AddVariable(BP, TEXT("RotVar"), UEdGraphSchema_K2::PC_Struct, TBaseStructure<FRotator>::Get());
        AddVariable(BP, TEXT("HitVar"), UEdGraphSchema_K2::PC_Struct, FHitResult::StaticStruct());
        AddVariable(BP, TEXT("TransformVar"), UEdGraphSchema_K2::PC_Struct, TBaseStructure<FTransform>::Get());
        AddVariable(BP, TEXT("VecVar"), UEdGraphSchema_K2::PC_Struct, TBaseStructure<FVector>::Get());
        AddVariable(BP, TEXT("FloatVar"), UEdGraphSchema_K2::PC_Real);
        AddVariable(BP, TEXT("NameVar"), UEdGraphSchema_K2::PC_Name);
    }

    // The data-wiring error on the ItemToFind pin, or empty when there is none.
    FString FindItemToFindWireError(const FCompileResult& Result)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            if (Err.Message.Contains(TEXT("TryCreateConnection failed wiring data"))
                && Err.Message.Contains(TEXT("'ItemToFind'")))
            {
                return Err.Message;
            }
        }
        return FString();
    }

    // The macro-graph Select whose named option pin carries DefaultValue.
    UK2Node_Select* FindMacroSelectWithOptionDefault(UBlueprint* BP, const TCHAR* OptionName, const TCHAR* DefaultValue)
    {
        for (UEdGraph* Graph : BP->MacroGraphs)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_Select* Select = Cast<UK2Node_Select>(Node);
                const UEdGraphPin* Option = Select ? Select->FindPin(OptionName, EGPD_Input) : nullptr;
                if (Option && Option->DefaultValue == DefaultValue)
                {
                    return Select;
                }
            }
        }
        return nullptr;
    }

    void LogErrors(FAutomationTestBase& Test, const FCompileResult& Result)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            Test.AddInfo(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
    }
}

// ============================================================================
// Array element pins resolve from TargetArray for the whole wildcard-array family,
// so a subclass item wires into an array of its base class.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirArrayWildcardSubclassItemWiresTest,
    "PinWright.bpir.compiler.array_wildcard.SubclassItemWires",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirArrayWildcardSubclassItemWiresTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ArrayWildcardItemBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    TestBpirWildcardResolutionHelpers::AddObjectVariable(BP, TEXT("ActorArray"), AActor::StaticClass(), true);
    TestBpirWildcardResolutionHelpers::AddObjectVariable(BP, TEXT("Probe"), APawn::StaticClass(), false);

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %idx = call Array_Find(TargetArray: $ActorArray, ItemToFind: $Probe)\n")
        TEXT("    %has = call Array_Contains(TargetArray: $ActorArray, ItemToFind: $Probe)\n")
        TEXT("    call Array_AddUnique(TargetArray: $ActorArray, NewItem: $Probe)\n")
        TEXT("    call Array_RemoveItem(TargetArray: $ActorArray, Item: $Probe)\n")
        TEXT("}"));
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    const TPair<const TCHAR*, const TCHAR*> Expected[] = {
        { TEXT("Array_Find"), TEXT("ItemToFind") },
        { TEXT("Array_Contains"), TEXT("ItemToFind") },
        { TEXT("Array_AddUnique"), TEXT("NewItem") },
        { TEXT("Array_RemoveItem"), TEXT("Item") },
    };
    for (const TPair<const TCHAR*, const TCHAR*>& Entry : Expected)
    {
        UK2Node_CallFunction* Call = TestBpirWildcardResolutionHelpers::FindCallTo(BP, Entry.Key);
        TestTrue(FString::Printf(TEXT("%s is a UK2Node_CallArrayFunction"), Entry.Key),
            Call && Call->IsA<UK2Node_CallArrayFunction>());
        const UEdGraphPin* ItemPin = Call ? Call->FindPin(Entry.Value, EGPD_Input) : nullptr;
        TestNotNull(FString::Printf(TEXT("%s has pin %s"), Entry.Key, Entry.Value), ItemPin);
        if (!ItemPin) continue;
        TestTrue(FString::Printf(TEXT("%s.%s took the array's element type"), Entry.Key, Entry.Value),
            ItemPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
            && ItemPin->PinType.PinSubCategoryObject.Get() == AActor::StaticClass());
        TestEqual(FString::Printf(TEXT("%s.%s is wired"), Entry.Key, Entry.Value), ItemPin->LinkedTo.Num(), 1);
    }
    return true;
}

// ============================================================================
// A genuinely incompatible item (a Pawn into a Character array — a downcast) must
// fail, and the error must name both pin types so it cannot be mistaken for an
// unresolved wildcard. Fails before the fix: the message carried pin names only.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirArrayWildcardMismatchNamesTypesTest,
    "PinWright.bpir.compiler.array_wildcard.MismatchErrorNamesPinTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirArrayWildcardMismatchNamesTypesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("ArrayWildcardMismatchBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    TestBpirWildcardResolutionHelpers::AddObjectVariable(BP, TEXT("CharacterArray"), ACharacter::StaticClass(), true);
    TestBpirWildcardResolutionHelpers::AddObjectVariable(BP, TEXT("Probe"), APawn::StaticClass(), false);

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %idx = call Array_Find(TargetArray: $CharacterArray, ItemToFind: $Probe)\n")
        TEXT("    call PrintString(InString: \"x\")\n")
        TEXT("}"));
    TestBpirWildcardResolutionHelpers::LogErrors(*this, Result);

    TestFalse(TEXT("Pawn into a Character array is refused"), Result.bSuccess);
    const FString WireError = TestBpirWildcardResolutionHelpers::FindItemToFindWireError(Result);
    TestFalse(TEXT("Error names the wiring failure on ItemToFind"), WireError.IsEmpty());
    TestTrue(TEXT("Wiring error names the source pin type (Pawn Object Reference)"), WireError.Contains(TEXT("'Probe' (Pawn Object Reference)")));
    // ItemToFind is a const-ref param, so TypeToText appends " (by ref)": anchor on the pin name, not the closing paren.
    TestTrue(TEXT("Wiring error names the target pin type (Character Object Reference)"), WireError.Contains(TEXT("'ItemToFind' (Character Object Reference")));
    return true;
}

// ============================================================================
// The reporter's actual shape: GetAllActorsOfClass with a subclass literal types
// OutActors by its DeterminesOutputType metadata, so an Actor item into Array_Find
// over it is a downcast. Pins that typing and the type-naming error.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirArrayWildcardDynamicOutputMismatchTest,
    "PinWright.bpir.compiler.array_wildcard.DynamicOutputArrayMismatchNamesTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirArrayWildcardDynamicOutputMismatchTest::RunTest(const FString& Parameters)
{
    const TCHAR* GetAllLine = TEXT("    %all = call GetAllActorsOfClass(WorldContextObject: self, ActorClass: ACharacter)\n");

    // 1. Precondition, on a compile that succeeds (a failed compile rolls its nodes back):
    //    the class literal types OutActors as an array of Character.
    UBlueprint* TypingBP = CreateTransientTestBP(TEXT("ArrayWildcardDynamicOutputTypingBP"));
    TestNotNull(TEXT("Typing blueprint was created"), TypingBP);
    if (!TypingBP) return false;
    if (!TestBpirWildcardResolutionHelpers::CompileOrReport(*this, TypingBP,
        FString(TEXT("entry event BeginPlay() {\n")) + GetAllLine + TEXT("}"), TEXT("Typing compile")))
    {
        return false;
    }
    UK2Node_CallFunction* GetAll = TestBpirWildcardResolutionHelpers::FindCallTo(TypingBP, TEXT("GetAllActorsOfClass"));
    const UEdGraphPin* OutActors = GetAll ? GetAll->FindPin(TEXT("OutActors"), EGPD_Output) : nullptr;
    TestTrue(TEXT("OutActors is typed as an array of Character"),
        OutActors && OutActors->PinType.IsArray()
        && OutActors->PinType.PinSubCategoryObject.Get() == ACharacter::StaticClass());

    // 2. The reporter's line: an Actor item into Array_Find over that array is a downcast.
    UBlueprint* BP = CreateTransientTestBP(TEXT("ArrayWildcardDynamicOutputBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    // Named `Probe`, not `ActorProbe`: the assertions below must only match type text.
    TestBpirWildcardResolutionHelpers::AddObjectVariable(BP, TEXT("Probe"), AActor::StaticClass(), false);

    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        FString(TEXT("entry event BeginPlay() {\n")) + GetAllLine
        + TEXT("    %idx = call Array_Find(TargetArray: %all.OutActors, ItemToFind: $Probe)\n")
        + TEXT("}"));
    TestBpirWildcardResolutionHelpers::LogErrors(*this, Result);

    TestFalse(TEXT("Actor into an array of Character is refused"), Result.bSuccess);
    const FString WireError = TestBpirWildcardResolutionHelpers::FindItemToFindWireError(Result);
    TestFalse(TEXT("Error names the wiring failure on ItemToFind"), WireError.IsEmpty());
    TestTrue(TEXT("Wiring error names the source pin type (Actor Object Reference)"), WireError.Contains(TEXT("'Probe' (Actor Object Reference)")));
    // ItemToFind is a const-ref param, so TypeToText appends " (by ref)": anchor on the pin name, not the closing paren.
    TestTrue(TEXT("Wiring error names the target pin type (Character Object Reference)"), WireError.Contains(TEXT("'ItemToFind' (Character Object Reference")));
    return true;
}

// ============================================================================
// Selects whose options are only literals, or chain into other such Selects,
// are typed after wiring; a Select a typed consumer reaches keeps the consumer's
// type (no emit-time guess that would force a conversion node).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSelectLiteralOptionsResolveTypeTest,
    "PinWright.bpir.compiler.select_literal.OptionsResolveType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSelectLiteralOptionsResolveTypeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SelectLiteralTypeBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    TestBpirWildcardResolutionHelpers::AddVariable(BP, TEXT("bA"), UEdGraphSchema_K2::PC_Boolean);
    TestBpirWildcardResolutionHelpers::AddVariable(BP, TEXT("bB"), UEdGraphSchema_K2::PC_Boolean);
    TestBpirWildcardResolutionHelpers::AddVariable(BP, TEXT("FloatVar"), UEdGraphSchema_K2::PC_Real);

    // Distinct option literals let each Select be found by its defaults.
    FBpirCompiler Compiler(BP);
    const FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %i3 = select(Index: $bB, true: 3, false: -1)\n")
        TEXT("    %i2 = select(Index: $bA, true: 2, false: %i3)\n")
        TEXT("    %r = select(Index: $bA, true: 1.5f, false: 7)\n")
        TEXT("    %b = select(Index: $bB, true: true, false: false)\n")
        TEXT("    %c = select(Index: $bA, true: 1, false: 0)\n")
        TEXT("    set FloatVar = %c\n")
        TEXT("}"));
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    auto CheckSelect = [this](const TCHAR* Label, UK2Node_Select* Select, FName ExpectedCategory,
        const TCHAR* ExpectedOption0, const TCHAR* ExpectedOption1)
    {
        if (!Select)
        {
            AddError(FString::Printf(TEXT("%s: Select node not found"), Label));
            return;
        }
        const UEdGraphPin* ReturnPin = Select->GetReturnValuePin();
        const UEdGraphPin* Option0 = Select->FindPin(TEXT("Option 0"), EGPD_Input);
        const UEdGraphPin* Option1 = Select->FindPin(TEXT("Option 1"), EGPD_Input);
        if (!ReturnPin || !Option0 || !Option1)
        {
            AddError(FString::Printf(TEXT("%s: Return/Option pins not found"), Label));
            return;
        }
        const FString Expected = ExpectedCategory.ToString();
        TestEqual(FString::Printf(TEXT("%s Return Value category"), Label), ReturnPin->PinType.PinCategory.ToString(), Expected);
        TestEqual(FString::Printf(TEXT("%s Option 0 category"), Label), Option0->PinType.PinCategory.ToString(), Expected);
        TestEqual(FString::Printf(TEXT("%s Option 1 category"), Label), Option1->PinType.PinCategory.ToString(), Expected);
        if (ExpectedOption0)
        {
            TestEqual(FString::Printf(TEXT("%s Option 0 default"), Label), Option0->DefaultValue, ExpectedOption0);
        }
        if (ExpectedOption1)
        {
            TestEqual(FString::Printf(TEXT("%s Option 1 default"), Label), Option1->DefaultValue, ExpectedOption1);
        }
    };

    // Option 0 is the false branch, Option 1 the true branch.
    CheckSelect(TEXT("%i3 (all int literals)"), TestBpirWildcardResolutionHelpers::FindSelectWithOptionDefault(BP, TEXT("Option 0"), TEXT("-1")),
        UEdGraphSchema_K2::PC_Int, TEXT("-1"), TEXT("3"));
    CheckSelect(TEXT("%i2 (literal + chained select)"), TestBpirWildcardResolutionHelpers::FindSelectWithOptionDefault(BP, TEXT("Option 1"), TEXT("2")),
        UEdGraphSchema_K2::PC_Int, nullptr, TEXT("2"));
    CheckSelect(TEXT("%r (int + real literals)"), TestBpirWildcardResolutionHelpers::FindSelectWithOptionDefault(BP, TEXT("Option 0"), TEXT("7")),
        UEdGraphSchema_K2::PC_Real, TEXT("7"), TEXT("1.5"));
    CheckSelect(TEXT("%b (bool literals)"), TestBpirWildcardResolutionHelpers::FindSelectWithOptionDefault(BP, TEXT("Option 1"), TEXT("true")),
        UEdGraphSchema_K2::PC_Boolean, TEXT("false"), TEXT("true"));
    // %c's consumer is a real variable: it keeps the consumer's type, not int.
    CheckSelect(TEXT("%c (int literals, real consumer)"), TestBpirWildcardResolutionHelpers::FindSelectWithOptionDefault(BP, TEXT("Option 0"), TEXT("0")),
        UEdGraphSchema_K2::PC_Real, TEXT("0"), TEXT("1"));
    TestNull(TEXT("No int->real conversion node was inserted for %c"), TestBpirWildcardResolutionHelpers::FindCallTo(BP, TEXT("Conv_IntToDouble")));

    FKismetEditorUtilities::CompileBlueprint(BP);
    TestTrue(TEXT("Blueprint compile has no undetermined-type errors"), BP->Status != BS_Error);
    return true;
}

// ============================================================================
// Inside a macro graph a literal-only Select is legitimately generic: the instance
// types it. Neither a literal-only Select nor a two-Select chain feeding the wildcard
// output tunnel may be frozen to its literals' type. Fails before the fix: %a had no
// non-Select wildcard link, so it took int from its literals and %b took int from %a.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSelectLiteralMacroStaysWildcardTest,
    "PinWright.bpir.compiler.select_literal.MacroSelectsStayWildcard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSelectLiteralMacroStaysWildcardTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SelectLiteralMacroBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    if (!TestBpirWildcardResolutionHelpers::CompileOrReport(*this, BP,
        TEXT("entry macro PickWild(bool C, bool D) -> (wildcard Chained, wildcard Direct) {\n")
        TEXT("    %a = select(Index: $C, true: 1, false: 2)\n")
        TEXT("    %b = select(Index: $D, true: %a, false: 3)\n")
        TEXT("    %l = select(Index: $C, true: 5, false: 6)\n")
        TEXT("    return (Chained: %b, Direct: %l)\n")
        TEXT("}"), TEXT("Compile")))
    {
        return false;
    }
    TestEqual(TEXT("Precondition: one macro graph was created"), BP->MacroGraphs.Num(), 1);

    const TPair<const TCHAR*, const TCHAR*> Selects[] = {
        { TEXT("%a (literal-only, chained into %b)"), TEXT("2") },
        { TEXT("%b (chained, into the output tunnel)"), TEXT("3") },
        { TEXT("%l (literal-only, into the output tunnel)"), TEXT("6") },
    };
    for (const TPair<const TCHAR*, const TCHAR*>& Entry : Selects)
    {
        UK2Node_Select* Select = TestBpirWildcardResolutionHelpers::FindMacroSelectWithOptionDefault(BP, TEXT("Option 0"), Entry.Value);
        const UEdGraphPin* ReturnPin = Select ? Select->GetReturnValuePin() : nullptr;
        TestNotNull(FString::Printf(TEXT("%s: Select found in the macro graph"), Entry.Key), ReturnPin);
        if (!ReturnPin) continue;
        TestEqual(FString::Printf(TEXT("%s Return Value stays wildcard"), Entry.Key),
            ReturnPin->PinType.PinCategory.ToString(), UEdGraphSchema_K2::PC_Wildcard.ToString());
    }
    return true;
}

// ============================================================================
// break<T> on a HasNativeBreak struct emits the native break function, so the
// Kismet compile carries no "generic 'break' node" warning and FHitResult exposes
// its members. Fails before the fix: both emitted UK2Node_BreakStruct.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBreakNativeStructRoutesTest,
    "PinWright.bpir.compiler.break_native.RoutesToNativeBreakFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBreakNativeStructRoutesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BreakNativeStructBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    TestBpirWildcardResolutionHelpers::AddBreakFixtureVariables(BP);

    if (!TestBpirWildcardResolutionHelpers::CompileOrReport(*this, BP,
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %br = break<Rotator>($RotVar)\n")
        TEXT("    set FloatVar = %br.Yaw\n")
        TEXT("    %hb = break<HitResult>($HitVar)\n")
        TEXT("    set NameVar = %hb.HitBoneName\n")
        TEXT("}"), TEXT("Compile")))
    {
        return false;
    }

    TestEqual(TEXT("No generic UK2Node_BreakStruct was emitted"), CountNodesOfType<UK2Node_BreakStruct>(BP), 0);

    UK2Node_CallFunction* BreakRotator = TestBpirWildcardResolutionHelpers::FindCallTo(BP, TEXT("BreakRotator"));
    TestNotNull(TEXT("break<Rotator> emitted a BreakRotator call"), BreakRotator);
    const UEdGraphPin* YawPin = BreakRotator ? BreakRotator->FindPin(TEXT("Yaw"), EGPD_Output) : nullptr;
    TestTrue(TEXT("BreakRotator.Yaw feeds the FloatVar set"), YawPin && YawPin->LinkedTo.Num() == 1);
    const UEdGraphPin* InRotPin = BreakRotator ? BreakRotator->FindPin(TEXT("InRot"), EGPD_Input) : nullptr;
    TestTrue(TEXT("BreakRotator.InRot is wired from $RotVar"), InRotPin && InRotPin->LinkedTo.Num() == 1);

    UK2Node_CallFunction* BreakHit = TestBpirWildcardResolutionHelpers::FindCallTo(BP, TEXT("BreakHitResult"));
    TestNotNull(TEXT("break<HitResult> emitted a BreakHitResult call"), BreakHit);
    const UEdGraphPin* BonePin = BreakHit ? BreakHit->FindPin(TEXT("HitBoneName"), EGPD_Output) : nullptr;
    TestTrue(TEXT("BreakHitResult.HitBoneName feeds the NameVar set"), BonePin && BonePin->LinkedTo.Num() == 1);

    FKismetEditorUtilities::CompileBlueprint(BP);
    TestTrue(FString::Printf(TEXT("Blueprint compiles clean, no generic-break warning (status %d)"), static_cast<int32>(BP->Status)),
        BP->Status == BS_UpToDate);

    // Round trip: the native call decompiles as a call and recompiles to the same nodes.
    const FString Decompiled = TestBpirWildcardResolutionHelpers::DecompileOrReport(*this, BP);
    TestTrue(TEXT("Decompile prints the native break as `call BreakRotator(`"), Decompiled.Contains(TEXT("call BreakRotator(")));
    TestTrue(TEXT("Decompile prints the native break as `call BreakHitResult(`"), Decompiled.Contains(TEXT("call BreakHitResult(")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("BreakNativeStructRecompileBP"));
    TestNotNull(TEXT("Recompile blueprint was created"), BP2);
    if (!BP2 || Decompiled.IsEmpty()) return false;
    TestBpirWildcardResolutionHelpers::AddBreakFixtureVariables(BP2);
    if (!TestBpirWildcardResolutionHelpers::CompileOrReport(*this, BP2, Decompiled, TEXT("Recompile")))
    {
        AddInfo(Decompiled);
        return false;
    }
    TestEqual(TEXT("Recompile emitted no generic UK2Node_BreakStruct"), CountNodesOfType<UK2Node_BreakStruct>(BP2), 0);
    UK2Node_CallFunction* BreakRotator2 = TestBpirWildcardResolutionHelpers::FindCallTo(BP2, TEXT("BreakRotator"));
    const UEdGraphPin* YawPin2 = BreakRotator2 ? BreakRotator2->FindPin(TEXT("Yaw"), EGPD_Output) : nullptr;
    TestTrue(TEXT("Recompiled BreakRotator.Yaw is wired"), YawPin2 && YawPin2->LinkedTo.Num() == 1);
    UK2Node_CallFunction* BreakHit2 = TestBpirWildcardResolutionHelpers::FindCallTo(BP2, TEXT("BreakHitResult"));
    const UEdGraphPin* BonePin2 = BreakHit2 ? BreakHit2->FindPin(TEXT("HitBoneName"), EGPD_Output) : nullptr;
    TestTrue(TEXT("Recompiled BreakHitResult.HitBoneName is wired"), BonePin2 && BonePin2->LinkedTo.Num() == 1);
    return true;
}

// ============================================================================
// An existing generic Break Struct node on a native-break struct (FTransform) must
// round-trip as itself: decompile prints the F-prefixed form, which keeps the generic
// node and its Translation member. Fails before the emitter fix: the bare
// `break<Transform>` recompiled to BreakTransform, which has no Translation pin.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBreakNativeGenericNodeRoundTripTest,
    "PinWright.bpir.compiler.break_native.GenericNodeRoundTripsAsFForm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBreakNativeGenericNodeRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BreakGenericTransformBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;
    TestBpirWildcardResolutionHelpers::AddBreakFixtureVariables(BP);

    if (!TestBpirWildcardResolutionHelpers::CompileOrReport(*this, BP,
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %bt = break<FTransform>($TransformVar)\n")
        TEXT("    set VecVar = %bt.Translation\n")
        TEXT("}"), TEXT("Compile")))
    {
        return false;
    }
    TestEqual(TEXT("Precondition: break<FTransform> emitted the generic node"), CountNodesOfType<UK2Node_BreakStruct>(BP), 1);

    const FString Decompiled = TestBpirWildcardResolutionHelpers::DecompileOrReport(*this, BP);
    TestTrue(TEXT("Decompile prints the generic node as break<FTransform>"), Decompiled.Contains(TEXT("break<FTransform>(")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("BreakGenericTransformRecompileBP"));
    TestNotNull(TEXT("Recompile blueprint was created"), BP2);
    if (!BP2 || Decompiled.IsEmpty()) return false;
    TestBpirWildcardResolutionHelpers::AddBreakFixtureVariables(BP2);
    if (!TestBpirWildcardResolutionHelpers::CompileOrReport(*this, BP2, Decompiled, TEXT("Recompile")))
    {
        AddInfo(Decompiled);
        return false;
    }

    UK2Node_BreakStruct* Break2 = FindNodeOfType<UK2Node_BreakStruct>(BP2);
    TestNotNull(TEXT("Recompile kept the generic Break Struct node"), Break2);
    TestNull(TEXT("Recompile did not route to BreakTransform"),
        TestBpirWildcardResolutionHelpers::FindCallTo(BP2, TEXT("BreakTransform")));
    const UEdGraphPin* TranslationPin = Break2 ? Break2->FindPin(TEXT("Translation"), EGPD_Output) : nullptr;
    TestTrue(TEXT("Recompiled Translation member is wired"), TranslationPin && TranslationPin->LinkedTo.Num() == 1);
    return true;
}
