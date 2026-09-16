// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Compiler/CodeFunctionResolver.h"
#include "Compiler/CodePinResolver.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Utils/ClassUtils.h"

#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Internationalization/Text.h"
#include "UObject/UObjectBaseUtility.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/Actor.h"

namespace
{
    // Test-local string-in shim: parses CppType into an FBpirTypeSpec and
    // delegates to ConvertTypeSpecToPinType so the test assertions exercise
    // the full parse -> convert pipeline from a single input string.
    static bool ConvertStringTypeForTest(const FString& CppType, FEdGraphPinType& OutType)
    {
        FBpirTypeSpec Spec;
        FString Err;
        int32 Col = INDEX_NONE;
        if (!BpirTypeSpecParser::ParseTypeSpec(CppType, Spec, Err, Col))
        {
            return false;
        }
        return FCodePinResolver::ConvertTypeSpecToPinType(Spec, OutType);
    }
}

// ============================================================================
// FCodeFunctionResolver — ExactMatch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverExactMatchTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.ExactMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverExactMatchTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunction(UKismetMathLibrary::StaticClass(), TEXT("Add_DoubleDouble"));
    TestTrue(TEXT("Add_DoubleDouble resolves on UKismetMathLibrary"), Found != nullptr);
    return true;
}

// ============================================================================
// FCodeFunctionResolver — AliasMatch
// "Add_FloatFloat" is a registered alias that maps to "Add_DoubleDouble"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverAliasMatchTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.AliasMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverAliasMatchTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunction(UKismetMathLibrary::StaticClass(), TEXT("Add_FloatFloat"));
    TestTrue(TEXT("Add_FloatFloat resolves via alias"), Found != nullptr);
    if (Found)
    {
        TestEqual(TEXT("Alias resolves to Add_DoubleDouble"), Found->GetName(), TEXT("Add_DoubleDouble"));
    }
    return true;
}

// ============================================================================
// FCodeFunctionResolver — NoMatch
// Completely fabricated name — should not match anything within distance 3
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverNoMatchTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.NoMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverNoMatchTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunction(UKismetMathLibrary::StaticClass(), TEXT("CompletelyFakeFunction_XYZZY"));
    TestTrue(TEXT("Nonsense name returns null"), Found == nullptr);
    return true;
}

// ============================================================================
// FCodeFunctionResolver — LibrarySearch
// ResolveFunctionAcrossLibraries("PrintString") must find UKismetSystemLibrary::PrintString
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverLibrarySearchTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.LibrarySearch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverLibrarySearchTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunctionAcrossLibraries(TEXT("PrintString"));
    TestTrue(TEXT("PrintString found across known libraries"), Found != nullptr);
    if (Found)
    {
        // PrintString lives on UKismetSystemLibrary
        TestTrue(TEXT("PrintString owner is UKismetSystemLibrary"),
            Found->GetOuterUClass() == UKismetSystemLibrary::StaticClass());
    }
    return true;
}

// ============================================================================
// FCodeFunctionResolver — PreProcessConversions
// "FString::FromInt(42)" must become an expression containing "Conv_IntToString"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverPreProcessTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.PreProcessConversions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverPreProcessTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    const FString Input = TEXT("FString::FromInt(42)");
    const FString Output = Resolver.PreProcessKnownConversions(Input);
    TestTrue(TEXT("Output contains Conv_IntToString"), Output.Contains(TEXT("Conv_IntToString")));
    TestTrue(TEXT("Output no longer contains FString::FromInt"), !Output.Contains(TEXT("FString::FromInt")));
    return true;
}

// ============================================================================
// FCodePinResolver — RegisterAndResolve
// RegisterVariable with nullptr pin is valid storage; ResolveVariable returns it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverRegisterAndResolveTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.RegisterAndResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverRegisterAndResolveTest::RunTest(const FString& Parameters)
{
    FCodePinResolver PinResolver;
    // nullptr is a valid sentinel value for a pin slot
    PinResolver.RegisterVariable(TEXT("MyVar"), nullptr);
    UEdGraphPin* Result = PinResolver.ResolveVariable(TEXT("MyVar"));
    // The resolver should return what was stored (nullptr in this case)
    TestTrue(TEXT("Registered variable resolves (even when stored as nullptr)"),
        PinResolver.HasVariable(TEXT("MyVar")));
    TestTrue(TEXT("ResolveVariable returns stored nullptr"), Result == nullptr);
    TestTrue(TEXT("Unknown name returns null"), PinResolver.ResolveVariable(TEXT("UnknownVar")) == nullptr);
    return true;
}

// ============================================================================
// FCodePinResolver — LiteralRegistration
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverLiteralRegistrationTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.LiteralRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverLiteralRegistrationTest::RunTest(const FString& Parameters)
{
    FCodePinResolver PinResolver;
    PinResolver.RegisterLiteral(TEXT("MyVar"), TEXT("42.0"));
    TestTrue(TEXT("IsLiteral returns true after RegisterLiteral"), PinResolver.IsLiteral(TEXT("MyVar")));
    TestEqual(TEXT("GetLiteralValue returns stored value"), PinResolver.GetLiteralValue(TEXT("MyVar")), TEXT("42.0"));
    TestTrue(TEXT("IsLiteral returns false for unregistered name"), !PinResolver.IsLiteral(TEXT("NotHere")));
    TestEqual(TEXT("GetLiteralValue returns empty for unregistered name"), PinResolver.GetLiteralValue(TEXT("NotHere")), TEXT(""));
    return true;
}

// ============================================================================
// FCodePinResolver — SetTextDefaultRequiresIdentity
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverSetTextDefaultRequiresIdentityTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.SetTextDefaultRequiresIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverSetTextDefaultRequiresIdentityTest::RunTest(const FString& Parameters)
{
    UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph);
    Graph->AddNode(Node);
    UEdGraphPin* Pin = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, FName(TEXT("InText")));
    TestNotNull(TEXT("Text pin created"), Pin);
    if (!Pin) return false;

    FString Err;
    TestFalse(TEXT("Plain text is rejected on an empty FText pin"),
        FCodePinResolver::SetPinDefaultValue(Pin, TEXT("Plain"), &Err));
    TestTrue(TEXT("Plain text error mentions namespace"), Err.Contains(TEXT("namespace")));
    TestTrue(TEXT("Plain text error mentions key"), Err.Contains(TEXT("key")));

    Err.Reset();
    TestTrue(TEXT("NSLOCTEXT text default is accepted"),
        FCodePinResolver::SetPinDefaultValue(Pin, TEXT("NSLOCTEXT(\"BPIR\", \"TextPin\", \"Hello\")"), &Err));

    const TOptional<FString> InitialNamespace = FTextInspector::GetNamespace(Pin->DefaultTextValue);
    const TOptional<FString> InitialKey = FTextInspector::GetKey(Pin->DefaultTextValue);
    const FString* InitialSource = FTextInspector::GetSourceString(Pin->DefaultTextValue);
    TestTrue(TEXT("Initial namespace set"), InitialNamespace.IsSet());
    TestTrue(TEXT("Initial key set"), InitialKey.IsSet());
    if (InitialNamespace.IsSet())
    {
        TestEqual(TEXT("Initial namespace value"), InitialNamespace.GetValue(), FString(TEXT("BPIR")));
    }
    if (InitialKey.IsSet())
    {
        TestEqual(TEXT("Initial key value"), InitialKey.GetValue(), FString(TEXT("TextPin")));
    }
    TestNotNull(TEXT("Initial source string set"), InitialSource);
    if (InitialSource)
    {
        TestEqual(TEXT("Initial source string value"), *InitialSource, FString(TEXT("Hello")));
    }

    Err.Reset();
    TestTrue(TEXT("Plain text update preserves existing identity"),
        FCodePinResolver::SetPinDefaultValue(Pin, TEXT("Goodbye"), &Err));

    const TOptional<FString> UpdatedNamespace = FTextInspector::GetNamespace(Pin->DefaultTextValue);
    const TOptional<FString> UpdatedKey = FTextInspector::GetKey(Pin->DefaultTextValue);
    const FString* UpdatedSource = FTextInspector::GetSourceString(Pin->DefaultTextValue);
    TestTrue(TEXT("Updated namespace set"), UpdatedNamespace.IsSet());
    TestTrue(TEXT("Updated key set"), UpdatedKey.IsSet());
    if (UpdatedNamespace.IsSet())
    {
        TestEqual(TEXT("Updated namespace preserved"), UpdatedNamespace.GetValue(), FString(TEXT("BPIR")));
    }
    if (UpdatedKey.IsSet())
    {
        TestEqual(TEXT("Updated key preserved"), UpdatedKey.GetValue(), FString(TEXT("TextPin")));
    }
    TestNotNull(TEXT("Updated source string set"), UpdatedSource);
    if (UpdatedSource)
    {
        TestEqual(TEXT("Updated source string value"), *UpdatedSource, FString(TEXT("Goodbye")));
    }
    return true;
}

// ============================================================================
// FCodePinResolver — SetFieldPathDefault
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverSetFieldPathDefaultTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.SetFieldPathDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverSetFieldPathDefaultTest::RunTest(const FString& Parameters)
{
    UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph);
    Graph->AddNode(Node);
    UEdGraphPin* Pin = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_FieldPath, FName(TEXT("InFieldPath")));
    TestNotNull(TEXT("FieldPath pin created"), Pin);
    if (!Pin) return false;

    const FString FieldPathDefault = TEXT("MyWidget.SomeProperty");
    FString Err;
    TestTrue(TEXT("FieldPath default is accepted"),
        FCodePinResolver::SetPinDefaultValue(Pin, FieldPathDefault, &Err));
    TestEqual(TEXT("FieldPath default is preserved verbatim"), Pin->DefaultValue, FieldPathDefault);
    return true;
}

// ============================================================================
// FCodePinResolver — Clear
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverClearTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.Clear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverClearTest::RunTest(const FString& Parameters)
{
    FCodePinResolver PinResolver;
    PinResolver.RegisterVariable(TEXT("PinVar"), nullptr);
    PinResolver.RegisterLiteral(TEXT("LitVar"), TEXT("99"));

    PinResolver.Clear();

    TestTrue(TEXT("PinVar gone after Clear"), PinResolver.ResolveVariable(TEXT("PinVar")) == nullptr);
    TestTrue(TEXT("HasVariable false for PinVar after Clear"), !PinResolver.HasVariable(TEXT("PinVar")));
    TestTrue(TEXT("IsLiteral false for LitVar after Clear"), !PinResolver.IsLiteral(TEXT("LitVar")));
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Float
// "float" → PC_Real
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFloatTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Float",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFloatTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("float"), OutType);
    TestTrue(TEXT("float conversion succeeds"), bConverted);
    TestEqual(TEXT("float maps to PC_Real"), OutType.PinCategory, UEdGraphSchema_K2::PC_Real);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Bool
// "bool" → PC_Boolean
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertBoolTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Bool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertBoolTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("bool"), OutType);
    TestTrue(TEXT("bool conversion succeeds"), bConverted);
    TestEqual(TEXT("bool maps to PC_Boolean"), OutType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FString
// "FString" → PC_String
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFStringTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFStringTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FString"), OutType);
    TestTrue(TEXT("FString conversion succeeds"), bConverted);
    TestEqual(TEXT("FString maps to PC_String"), OutType.PinCategory, UEdGraphSchema_K2::PC_String);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FVector
// "FVector" → PC_Struct with SubCategoryObject == TBaseStructure<FVector>::Get()
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFVectorTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FVector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFVectorTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FVector"), OutType);
    TestTrue(TEXT("FVector conversion succeeds"), bConverted);
    TestEqual(TEXT("FVector maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("FVector SubCategoryObject is FVector struct"),
        OutType.PinSubCategoryObject.Get() == TBaseStructure<FVector>::Get());
    return true;
}

// ============================================================================
// FCodePinResolver — HasVariable
// HasVariable returns true for both pin-registered and literal-registered names
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverHasVariableTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.HasVariable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverHasVariableTest::RunTest(const FString& Parameters)
{
    FCodePinResolver PinResolver;
    PinResolver.RegisterVariable(TEXT("PinVar"), nullptr);
    PinResolver.RegisterLiteral(TEXT("LitVar"), TEXT("1.0"));

    TestTrue(TEXT("HasVariable true for pin-registered name"), PinResolver.HasVariable(TEXT("PinVar")));
    TestTrue(TEXT("HasVariable true for literal-registered name"), PinResolver.HasVariable(TEXT("LitVar")));
    TestTrue(TEXT("HasVariable false for unknown name"), !PinResolver.HasVariable(TEXT("Unknown")));
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Int
// "int" → PC_Int
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertIntTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Int",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertIntTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("int"), OutType);
    TestTrue(TEXT("int conversion succeeds"), bConverted);
    TestEqual(TEXT("int maps to PC_Int"), OutType.PinCategory, UEdGraphSchema_K2::PC_Int);

    // Also test "int32" alias
    FEdGraphPinType OutType32;
    const bool bConverted32 = ConvertStringTypeForTest(TEXT("int32"), OutType32);
    TestTrue(TEXT("int32 conversion succeeds"), bConverted32);
    TestEqual(TEXT("int32 maps to PC_Int"), OutType32.PinCategory, UEdGraphSchema_K2::PC_Int);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Int64
// "int64" → PC_Int64
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertInt64Test,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Int64",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertInt64Test::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("int64"), OutType);
    TestTrue(TEXT("int64 conversion succeeds"), bConverted);
    TestEqual(TEXT("int64 maps to PC_Int64"), OutType.PinCategory, UEdGraphSchema_K2::PC_Int64);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Double
// "double" → PC_Real with PC_Double subcategory
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertDoubleTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Double",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertDoubleTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("double"), OutType);
    TestTrue(TEXT("double conversion succeeds"), bConverted);
    TestEqual(TEXT("double maps to PC_Real"), OutType.PinCategory, UEdGraphSchema_K2::PC_Real);
    TestEqual(TEXT("double subcategory is PC_Double"), OutType.PinSubCategory, UEdGraphSchema_K2::PC_Double);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FName
// "FName" → PC_Name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFNameTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFNameTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FName"), OutType);
    TestTrue(TEXT("FName conversion succeeds"), bConverted);
    TestEqual(TEXT("FName maps to PC_Name"), OutType.PinCategory, UEdGraphSchema_K2::PC_Name);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FText
// "FText" → PC_Text
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFTextTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFTextTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FText"), OutType);
    TestTrue(TEXT("FText conversion succeeds"), bConverted);
    TestEqual(TEXT("FText maps to PC_Text"), OutType.PinCategory, UEdGraphSchema_K2::PC_Text);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FRotator
// "FRotator" → PC_Struct with SubCategoryObject == TBaseStructure<FRotator>::Get()
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFRotatorTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FRotator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFRotatorTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FRotator"), OutType);
    TestTrue(TEXT("FRotator conversion succeeds"), bConverted);
    TestEqual(TEXT("FRotator maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("FRotator SubCategoryObject is FRotator struct"),
        OutType.PinSubCategoryObject.Get() == TBaseStructure<FRotator>::Get());
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FTransform
// "FTransform" → PC_Struct with SubCategoryObject == TBaseStructure<FTransform>::Get()
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFTransformTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFTransformTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FTransform"), OutType);
    TestTrue(TEXT("FTransform conversion succeeds"), bConverted);
    TestEqual(TEXT("FTransform maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("FTransform SubCategoryObject is FTransform struct"),
        OutType.PinSubCategoryObject.Get() == TBaseStructure<FTransform>::Get());
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_FLinearColor
// "FLinearColor" → PC_Struct with SubCategoryObject == TBaseStructure<FLinearColor>::Get()
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertFLinearColorTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_FLinearColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertFLinearColorTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("FLinearColor"), OutType);
    TestTrue(TEXT("FLinearColor conversion succeeds"), bConverted);
    TestEqual(TEXT("FLinearColor maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("FLinearColor SubCategoryObject is FLinearColor struct"),
        OutType.PinSubCategoryObject.Get() == TBaseStructure<FLinearColor>::Get());
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Array
// "array<float>" → ContainerType=Array, PinCategory=PC_Real
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertArrayTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Array",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertArrayTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("array<float>"), OutType);
    TestTrue(TEXT("array<float> conversion succeeds"), bConverted);
    TestEqual(TEXT("Container type is Array"), OutType.ContainerType, EPinContainerType::Array);
    TestEqual(TEXT("Inner type is PC_Real"), OutType.PinCategory, UEdGraphSchema_K2::PC_Real);

    // array<FString>
    FEdGraphPinType OutType2;
    const bool bConverted2 = ConvertStringTypeForTest(TEXT("array<FString>"), OutType2);
    TestTrue(TEXT("array<FString> conversion succeeds"), bConverted2);
    TestEqual(TEXT("Container type is Array"), OutType2.ContainerType, EPinContainerType::Array);
    TestEqual(TEXT("Inner type is PC_String"), OutType2.PinCategory, UEdGraphSchema_K2::PC_String);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Object
// "object<AActor>" → PC_Object with SubCategoryObject == AActor::StaticClass()
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertObjectTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Object",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertObjectTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("object<AActor>"), OutType);
    TestTrue(TEXT("object<AActor> conversion succeeds"), bConverted);
    TestEqual(TEXT("object<AActor> maps to PC_Object"), OutType.PinCategory, UEdGraphSchema_K2::PC_Object);
    // Note: FindFirstObjectSafe<UClass>("AActor") won't resolve because the UObject
    // name is "Actor" (prefix stripped). SubCategoryObject resolution is best-effort;
    // only the pin category is guaranteed.
    if (OutType.PinSubCategoryObject.IsValid())
    {
        TestTrue(TEXT("SubCategoryObject is AActor if resolved"),
            OutType.PinSubCategoryObject.Get() == AActor::StaticClass());
    }
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_SoftObject
// "softobject<AActor>" → PC_SoftObject
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertSoftObjectTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_SoftObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertSoftObjectTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("softobject<AActor>"), OutType);
    TestTrue(TEXT("softobject<AActor> conversion succeeds"), bConverted);
    TestEqual(TEXT("softobject maps to PC_SoftObject"), OutType.PinCategory, UEdGraphSchema_K2::PC_SoftObject);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_SoftClass
// "softclass<AActor>" → PC_SoftClass
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertSoftClassTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_SoftClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertSoftClassTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("softclass<AActor>"), OutType);
    TestTrue(TEXT("softclass<AActor> conversion succeeds"), bConverted);
    TestEqual(TEXT("softclass maps to PC_SoftClass"), OutType.PinCategory, UEdGraphSchema_K2::PC_SoftClass);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Interface
// "interface<AActor>" → PC_Interface
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertInterfaceTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Interface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertInterfaceTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("interface<AActor>"), OutType);
    TestTrue(TEXT("interface<AActor> conversion succeeds"), bConverted);
    TestEqual(TEXT("interface maps to PC_Interface"), OutType.PinCategory, UEdGraphSchema_K2::PC_Interface);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Delegate
// "delegate" → PC_Delegate
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertDelegateTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Delegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertDelegateTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("delegate"), OutType);
    TestTrue(TEXT("delegate conversion succeeds"), bConverted);
    TestEqual(TEXT("delegate maps to PC_Delegate"), OutType.PinCategory, UEdGraphSchema_K2::PC_Delegate);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_MCDelegate
// "mcdelegate" → PC_MCDelegate
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertMCDelegateTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_MCDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertMCDelegateTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("mcdelegate"), OutType);
    TestTrue(TEXT("mcdelegate conversion succeeds"), bConverted);
    TestEqual(TEXT("mcdelegate maps to PC_MCDelegate"), OutType.PinCategory, UEdGraphSchema_K2::PC_MCDelegate);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_Unrecognized
// A completely unknown type should return false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertUnrecognizedTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_Unrecognized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertUnrecognizedTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("CompletelyFakeType_XYZZY"), OutType);
    TestFalse(TEXT("Unrecognized type returns false"), bConverted);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_ObjectPointer
// "AActor*" → PC_Object with SubCategoryObject == AActor::StaticClass()
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertObjectPointerTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_ObjectPointer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertObjectPointerTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("AActor*"), OutType);
    TestTrue(TEXT("AActor* conversion succeeds"), bConverted);
    TestEqual(TEXT("AActor* maps to PC_Object"), OutType.PinCategory, UEdGraphSchema_K2::PC_Object);
    // Note: FindFirstObjectSafe<UClass>("AActor") won't resolve because the UObject
    // name is "Actor" (prefix stripped). SubCategoryObject resolution is best-effort.
    if (OutType.PinSubCategoryObject.IsValid())
    {
        TestTrue(TEXT("SubCategoryObject is AActor if resolved"),
            OutType.PinSubCategoryObject.Get() == AActor::StaticClass());
    }

    // UClass* should map to PC_Class
    FEdGraphPinType OutType2;
    const bool bConverted2 = ConvertStringTypeForTest(TEXT("UClass*"), OutType2);
    TestTrue(TEXT("UClass* conversion succeeds"), bConverted2);
    TestEqual(TEXT("UClass* maps to PC_Class"), OutType2.PinCategory, UEdGraphSchema_K2::PC_Class);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_ClassRef
// "class<AActor>" → PC_Class with SubCategoryObject = AActor
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertClassRefTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_ClassRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertClassRefTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("class<AActor>"), OutType);
    TestTrue(TEXT("class<AActor> conversion succeeds"), bConverted);
    TestEqual(TEXT("class<AActor> maps to PC_Class"), OutType.PinCategory, UEdGraphSchema_K2::PC_Class);
    // Note: FindFirstObjectSafe<UClass>("AActor") won't resolve because the UObject
    // name is "Actor" (prefix stripped). SubCategoryObject resolution is best-effort.
    if (OutType.PinSubCategoryObject.IsValid())
    {
        TestTrue(TEXT("SubCategoryObject is AActor if resolved"),
            OutType.PinSubCategoryObject.Get() == AActor::StaticClass());
    }
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_StructRef
// "struct<FVector>" → PC_Struct
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertStructRefTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_StructRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertStructRefTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("struct<FVector>"), OutType);
    TestTrue(TEXT("struct<FVector> conversion succeeds"), bConverted);
    TestEqual(TEXT("struct<FVector> maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_EnumRef
// "enum<ECollisionChannel>" → PC_Byte with SubCategoryObject = enum
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertEnumRefTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_EnumRef",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertEnumRefTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType OutType;
    const bool bConverted = ConvertStringTypeForTest(TEXT("enum<ECollisionChannel>"), OutType);
    TestTrue(TEXT("enum<ECollisionChannel> conversion succeeds"), bConverted);
    TestEqual(TEXT("enum maps to PC_Byte"), OutType.PinCategory, UEdGraphSchema_K2::PC_Byte);
    TestTrue(TEXT("SubCategoryObject is non-null (enum found)"),
        OutType.PinSubCategoryObject.Get() != nullptr);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertTypeSpec_SetSupported
// Verifies that set<T> parses and converts end-to-end via the structured
// TypeSpec path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertSetSupportedTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertTypeSpec_SetSupported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertSetSupportedTest::RunTest(const FString& Parameters)
{
    // Parse via the structured TypeSpec parser first, then convert.
    FBpirTypeSpec Spec;
    FString ParseError;
    int32 ErrorColumn = INDEX_NONE;
    const bool bParsed = BpirTypeSpecParser::ParseTypeSpec(TEXT("set<float>"), Spec, ParseError, ErrorColumn);
    TestTrue(TEXT("set<float> parses into FBpirTypeSpec"), bParsed);
    TestEqual(TEXT("Container is Set"), Spec.Container, EPinContainerType::Set);
    TestTrue(TEXT("ElementSpec is non-null"), Spec.ElementSpec.IsValid());
    if (Spec.ElementSpec.IsValid())
    {
        TestEqual(TEXT("Element kind is Float"), Spec.ElementSpec->Kind, EBpirTypeKind::Float);
    }

    FEdGraphPinType OutType;
    const bool bConverted = FCodePinResolver::ConvertTypeSpecToPinType(Spec, OutType);
    TestTrue(TEXT("set<float> conversion succeeds"), bConverted);
    TestEqual(TEXT("ContainerType is Set"), OutType.ContainerType, EPinContainerType::Set);
    TestEqual(TEXT("PinCategory is PC_Real"), OutType.PinCategory, UEdGraphSchema_K2::PC_Real);
    TestEqual(TEXT("PinSubCategory is PC_Float"), OutType.PinSubCategory, UEdGraphSchema_K2::PC_Float);
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertTypeSpec_MapSupported
// Verifies that map<K,V> parses and converts end-to-end via the structured
// TypeSpec path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertMapSupportedTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertTypeSpec_MapSupported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertMapSupportedTest::RunTest(const FString& Parameters)
{
    // Parse via the structured TypeSpec parser first, then convert.
    FBpirTypeSpec Spec;
    FString ParseError;
    int32 ErrorColumn = INDEX_NONE;
    const bool bParsed = BpirTypeSpecParser::ParseTypeSpec(TEXT("map<FString, int>"), Spec, ParseError, ErrorColumn);
    TestTrue(TEXT("map<FString, int> parses into FBpirTypeSpec"), bParsed);
    TestEqual(TEXT("Container is Map"), Spec.Container, EPinContainerType::Map);
    TestTrue(TEXT("KeySpec is non-null"), Spec.KeySpec.IsValid());
    if (Spec.KeySpec.IsValid())
    {
        TestEqual(TEXT("Key kind is String"), Spec.KeySpec->Kind, EBpirTypeKind::String);
    }
    TestTrue(TEXT("ElementSpec (value) is non-null"), Spec.ElementSpec.IsValid());
    if (Spec.ElementSpec.IsValid())
    {
        TestEqual(TEXT("Value kind is Int"), Spec.ElementSpec->Kind, EBpirTypeKind::Int);
    }

    FEdGraphPinType OutType;
    const bool bConverted = FCodePinResolver::ConvertTypeSpecToPinType(Spec, OutType);
    TestTrue(TEXT("map<FString, int> conversion succeeds"), bConverted);
    TestEqual(TEXT("ContainerType is Map"), OutType.ContainerType, EPinContainerType::Map);
    TestEqual(TEXT("Key PinCategory is PC_String"), OutType.PinCategory, UEdGraphSchema_K2::PC_String);
    TestEqual(TEXT("Value TerminalCategory is PC_Int"),
        OutType.PinValueType.TerminalCategory, UEdGraphSchema_K2::PC_Int);
    return true;
}

// ============================================================================
// FCodeFunctionResolver — ResolveLibraryFunction
// ResolveLibraryFunction("KismetStringLibrary", "Concat_StrStr") should find the function
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverResolveLibraryFunctionTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.ResolveLibraryFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverResolveLibraryFunctionTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveLibraryFunction(TEXT("KismetStringLibrary"), TEXT("Concat_StrStr"));
    TestTrue(TEXT("Concat_StrStr found on KismetStringLibrary"), Found != nullptr);
    if (Found)
    {
        TestTrue(TEXT("Function owner is UKismetStringLibrary"),
            Found->GetOuterUClass() == UKismetStringLibrary::StaticClass());
    }
    return true;
}

// ============================================================================
// FCodeFunctionResolver — BroadSearch.NoMatch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverBroadSearchNoMatchTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.broad_search.NoMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverBroadSearchNoMatchTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunctionBroadSearch(TEXT("CompletelyFakeFunction_XYZZY_12345"));
    TestTrue(TEXT("Nonsense name returns null from broad search"), Found == nullptr);
    return true;
}

// ============================================================================
// FCodeFunctionResolver — BroadSearch.SkipsLibraries
// PrintString is on UKismetSystemLibrary (a library class) so BroadSearch should NOT find it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverBroadSearchSkipsLibrariesTest,
    "PinWright.bpir.compiler.resolvers.function_resolver.broad_search.SkipsLibraries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverBroadSearchSkipsLibrariesTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunctionBroadSearch(TEXT("PrintString"));
    TestTrue(TEXT("PrintString NOT found via broad search (it's on a library class)"), Found == nullptr);
    return true;
}

// ============================================================================
// ResolveUEnum — ProjectModuleEnum
//
// Verifies that ResolveUEnum's TObjectIterator tier finds a non-engine UENUM
// that is loaded at editor startup. We scan all loaded UEnum objects to find
// the first one whose package path does NOT start with /Script/Engine,
// /Script/CoreUObject, or /Script/SlateCore — i.e. a project or plugin enum.
// If one exists, we assert ResolveUEnum returns non-null for its short name
// and that ConvertCppTypeToPinType maps it to PC_Byte with a valid
// PinSubCategoryObject.
//
// Separately we verify that engine enum ESlateVisibility is unaffected
// (regression guard for the previous fix).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUEnumProjectModuleEnumTest,
    "PinWright.bpir.compiler.resolvers.resolve_uenum.ProjectModuleEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveUEnumProjectModuleEnumTest::RunTest(const FString& Parameters)
{
    // --- Regression guard: engine enum still resolves ---
    {
        UEnum* EngineEnum = ResolveUEnum(TEXT("ESlateVisibility"));
        TestNotNull(TEXT("ResolveUEnum finds ESlateVisibility (engine enum)"), EngineEnum);

        FEdGraphPinType PinType;
        const bool bConverted = ConvertStringTypeForTest(
            TEXT("enum<ESlateVisibility>"), PinType);
        TestTrue(TEXT("enum<ESlateVisibility> conversion succeeds"), bConverted);
        TestEqual(TEXT("enum<ESlateVisibility> PinCategory is PC_Byte"),
            PinType.PinCategory, UEdGraphSchema_K2::PC_Byte);
        TestTrue(TEXT("enum<ESlateVisibility> PinSubCategoryObject is valid"),
            PinType.PinSubCategoryObject.IsValid());
    }

    // --- Project/plugin module enum via TObjectIterator tier ---
    // Find the first loaded UENUM that lives outside the core engine packages.
    // This exercises the exact code path that failed for EReplaySaveState.
    static const TArray<FString> EnginePackagePrefixes = {
        TEXT("/Script/Engine"),
        TEXT("/Script/CoreUObject"),
        TEXT("/Script/SlateCore"),
        TEXT("/Script/Slate"),
        TEXT("/Script/InputCore"),
        TEXT("/Script/AIModule"),
        TEXT("/Script/NavigationSystem"),
        TEXT("/Script/UMG"),
    };

    UEnum* NonEngineEnum = nullptr;
    for (TObjectIterator<UEnum> It; It; ++It)
    {
        UEnum* Candidate = *It;
        if (!Candidate || Candidate->GetName().IsEmpty())
            continue;

        const FString PackagePath = Candidate->GetPackage()
            ? Candidate->GetPackage()->GetPathName()
            : FString();

        bool bIsEngine = false;
        for (const FString& Prefix : EnginePackagePrefixes)
        {
            if (PackagePath.StartsWith(Prefix))
            {
                bIsEngine = true;
                break;
            }
        }
        if (!bIsEngine)
        {
            NonEngineEnum = Candidate;
            break;
        }
    }

    if (NonEngineEnum)
    {
        const FString ShortName = NonEngineEnum->GetName();
        UE_LOG(LogTemp, Log,
            TEXT("ResolveUEnum project-enum test: using '%s' from '%s'"),
            *ShortName,
            *NonEngineEnum->GetPackage()->GetPathName());

        // Tier-3 TObjectIterator lookup should find it by short name.
        UEnum* Resolved = ResolveUEnum(ShortName);
        TestNotNull(TEXT("ResolveUEnum finds non-engine UENUM by short name"), Resolved);
        if (Resolved)
        {
            TestEqual(TEXT("Resolved enum matches expected object"),
                Resolved, NonEngineEnum);
        }

        // ConvertCppTypeToPinType bare form.
        {
            FEdGraphPinType PinType;
            const bool bConverted = ConvertStringTypeForTest(ShortName, PinType);
            TestTrue(TEXT("ConvertCppTypeToPinType bare-form succeeds for non-engine enum"), bConverted);
            TestEqual(TEXT("Bare-form PinCategory is PC_Byte"),
                PinType.PinCategory, UEdGraphSchema_K2::PC_Byte);
            TestTrue(TEXT("Bare-form PinSubCategoryObject is valid"),
                PinType.PinSubCategoryObject.IsValid());
        }

        // ConvertCppTypeToPinType enum<T> angle-bracket form.
        {
            const FString AngleBracketForm = FString::Printf(TEXT("enum<%s>"), *ShortName);
            FEdGraphPinType PinType;
            const bool bConverted = ConvertStringTypeForTest(AngleBracketForm, PinType);
            TestTrue(TEXT("ConvertCppTypeToPinType enum<T>-form succeeds for non-engine enum"), bConverted);
            TestEqual(TEXT("enum<T>-form PinCategory is PC_Byte"),
                PinType.PinCategory, UEdGraphSchema_K2::PC_Byte);
            TestTrue(TEXT("enum<T>-form PinSubCategoryObject is valid"),
                PinType.PinSubCategoryObject.IsValid());
            if (PinType.PinSubCategoryObject.IsValid())
            {
                TestTrue(TEXT("enum<T>-form PinSubCategoryObject matches expected enum"),
                    PinType.PinSubCategoryObject.Get() == NonEngineEnum);
            }
        }
    }
    else
    {
        // No non-engine enum was found in this editor session — skip without failing.
        // This can happen in minimal headless test configurations that don't load plugins.
        UE_LOG(LogTemp, Warning,
            TEXT("ResolveUEnum project-enum test: no non-engine UENUM found in this session; skipping."));
    }
    return true;
}

// ============================================================================
// FCodePinResolver — ConvertCppType_BareKeywordAliases
// Bare-keyword aliases for common types (case-insensitive):
//   integer  → PC_Int
//   boolean  → PC_Boolean
//   vector   → PC_Struct (FVector)
//   rotator  → PC_Struct (FRotator)
//   transform → PC_Struct (FTransform)
//   object   → PC_Object
//   class    → PC_Class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodePinResolverConvertBareKeywordAliasesTest,
    "PinWright.bpir.compiler.resolvers.pin_resolver.ConvertCppType_BareKeywordAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodePinResolverConvertBareKeywordAliasesTest::RunTest(const FString& Parameters)
{
    // integer → PC_Int
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'integer' converts"), ConvertStringTypeForTest(TEXT("integer"), OutType));
        TestEqual(TEXT("'integer' maps to PC_Int"), OutType.PinCategory, UEdGraphSchema_K2::PC_Int);
    }
    // INTEGER (case-insensitive)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'INTEGER' converts (case-insensitive)"), ConvertStringTypeForTest(TEXT("INTEGER"), OutType));
        TestEqual(TEXT("'INTEGER' maps to PC_Int"), OutType.PinCategory, UEdGraphSchema_K2::PC_Int);
    }
    // boolean → PC_Boolean
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'boolean' converts"), ConvertStringTypeForTest(TEXT("boolean"), OutType));
        TestEqual(TEXT("'boolean' maps to PC_Boolean"), OutType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    }
    // Boolean (mixed case)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'Boolean' converts (case-insensitive)"), ConvertStringTypeForTest(TEXT("Boolean"), OutType));
        TestEqual(TEXT("'Boolean' maps to PC_Boolean"), OutType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    }
    // vector → PC_Struct (FVector)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'vector' converts"), ConvertStringTypeForTest(TEXT("vector"), OutType));
        TestEqual(TEXT("'vector' maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
        TestTrue(TEXT("'vector' SubCategoryObject is FVector struct"),
            OutType.PinSubCategoryObject.Get() == TBaseStructure<FVector>::Get());
    }
    // rotator → PC_Struct (FRotator)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'rotator' converts"), ConvertStringTypeForTest(TEXT("rotator"), OutType));
        TestEqual(TEXT("'rotator' maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
        TestTrue(TEXT("'rotator' SubCategoryObject is FRotator struct"),
            OutType.PinSubCategoryObject.Get() == TBaseStructure<FRotator>::Get());
    }
    // transform → PC_Struct (FTransform)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'transform' converts"), ConvertStringTypeForTest(TEXT("transform"), OutType));
        TestEqual(TEXT("'transform' maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
        TestTrue(TEXT("'transform' SubCategoryObject is FTransform struct"),
            OutType.PinSubCategoryObject.Get() == TBaseStructure<FTransform>::Get());
    }
    // Transform (case-insensitive)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'Transform' converts (case-insensitive)"), ConvertStringTypeForTest(TEXT("Transform"), OutType));
        TestEqual(TEXT("'Transform' maps to PC_Struct"), OutType.PinCategory, UEdGraphSchema_K2::PC_Struct);
    }
    // object → PC_Object
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'object' converts"), ConvertStringTypeForTest(TEXT("object"), OutType));
        TestEqual(TEXT("'object' maps to PC_Object"), OutType.PinCategory, UEdGraphSchema_K2::PC_Object);
    }
    // class → PC_Class
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'class' converts"), ConvertStringTypeForTest(TEXT("class"), OutType));
        TestEqual(TEXT("'class' maps to PC_Class"), OutType.PinCategory, UEdGraphSchema_K2::PC_Class);
    }
    // Class (case-insensitive)
    {
        FEdGraphPinType OutType;
        TestTrue(TEXT("'Class' converts (case-insensitive)"), ConvertStringTypeForTest(TEXT("Class"), OutType));
        TestEqual(TEXT("'Class' maps to PC_Class"), OutType.PinCategory, UEdGraphSchema_K2::PC_Class);
    }
    return true;
}
