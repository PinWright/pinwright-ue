// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the BPIR type-string → FEdGraphPinType pipeline.
// Tests go through ParseTypeSpec -> ConvertTypeSpecToPinType using a
// local wildcard-on-miss adapter so each assertion checks a single string
// input end-to-end.
#include "Misc/AutomationTest.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Tests/TestUtils.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/CodePinResolver.h"
#include "EdGraphSchema_K2.h"


using namespace BlueprintHandlerUtils;

namespace
{
    // Test-local string-in shim: parses InType into an FBpirTypeSpec, runs
    // ConvertTypeSpecToPinType, and returns a PC_Wildcard pin on any failure.
    static FEdGraphPinType MakePinType_FromText(const FString& InType)
    {
        FEdGraphPinType PinType;
        FBpirTypeSpec Spec;
        FString Err;
        int32 Col = INDEX_NONE;
        if (!BpirTypeSpecParser::ParseTypeSpec(InType, Spec, Err, Col)
            || !FCodePinResolver::ConvertTypeSpecToPinType(Spec, PinType))
        {
            PinType = FEdGraphPinType{};
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }
        return PinType;
    }
}
#define MakePinType MakePinType_FromText

// ============================================================================
// Primitives
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeFloatTest,
    "PinWright.core.make_pin_type.Float",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeFloatTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("float"));
    TestEqual(TEXT("float category is PC_Real"), Result.PinCategory, UEdGraphSchema_K2::PC_Real);
    TestEqual(TEXT("float subcategory is PC_Float"), Result.PinSubCategory, UEdGraphSchema_K2::PC_Float);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeDoubleTest,
    "PinWright.core.make_pin_type.Double",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeDoubleTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("double"));
    TestEqual(TEXT("double category is PC_Real"), Result.PinCategory, UEdGraphSchema_K2::PC_Real);
    TestEqual(TEXT("double subcategory is PC_Double"), Result.PinSubCategory, UEdGraphSchema_K2::PC_Double);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeIntTest,
    "PinWright.core.make_pin_type.Int",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeIntTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("int"));
    TestEqual(TEXT("int resolves to PC_Int"), Result.PinCategory, UEdGraphSchema_K2::PC_Int);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeBoolTest,
    "PinWright.core.make_pin_type.Bool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeBoolTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("bool"));
    TestEqual(TEXT("bool resolves to PC_Boolean"), Result.PinCategory, UEdGraphSchema_K2::PC_Boolean);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeStringTest,
    "PinWright.core.make_pin_type.String",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeStringTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("string"));
    TestEqual(TEXT("string resolves to PC_String"), Result.PinCategory, UEdGraphSchema_K2::PC_String);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeFieldPathTest,
    "PinWright.core.make_pin_type.FieldPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeFieldPathTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("field_path"));
    TestEqual(TEXT("field_path resolves to PC_FieldPath"), Result.PinCategory, UEdGraphSchema_K2::PC_FieldPath);

    FEdGraphPinType LegacyAlias = MakePinType(TEXT("fieldpath"));
    TestEqual(TEXT("fieldpath legacy alias resolves to PC_FieldPath"),
        LegacyAlias.PinCategory, UEdGraphSchema_K2::PC_FieldPath);

    FEdGraphPinType ArrayResult = MakePinType(TEXT("array<field_path>"));
    TestEqual(TEXT("array<field_path> element is PC_FieldPath"),
        ArrayResult.PinCategory, UEdGraphSchema_K2::PC_FieldPath);
    TestEqual(TEXT("array<field_path> container is Array"),
        ArrayResult.ContainerType, EPinContainerType::Array);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeByteTest,
    "PinWright.core.make_pin_type.Byte",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeByteTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("byte"));
    TestEqual(TEXT("byte resolves to PC_Byte"), Result.PinCategory, UEdGraphSchema_K2::PC_Byte);
    return true;
}

// ============================================================================
// Structs by short name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeVectorTest,
    "PinWright.core.make_pin_type.StructVector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeVectorTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("Vector"));
    TestEqual(TEXT("Vector resolves to PC_Struct"), Result.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("Vector has non-null PinSubCategoryObject"), Result.PinSubCategoryObject.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeRotatorTest,
    "PinWright.core.make_pin_type.StructRotator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeRotatorTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("Rotator"));
    TestEqual(TEXT("Rotator resolves to PC_Struct"), Result.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("Rotator has non-null PinSubCategoryObject"), Result.PinSubCategoryObject.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeTransformTest,
    "PinWright.core.make_pin_type.StructTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeTransformTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("Transform"));
    TestEqual(TEXT("Transform resolves to PC_Struct"), Result.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("Transform has non-null PinSubCategoryObject"), Result.PinSubCategoryObject.IsValid());
    return true;
}

// ============================================================================
// Enum by short name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeEnumTest,
    "PinWright.core.make_pin_type.EnumETextJustify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeEnumTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("ETextJustify"));
    TestEqual(TEXT("ETextJustify resolves to PC_Byte"), Result.PinCategory, UEdGraphSchema_K2::PC_Byte);
    TestTrue(TEXT("ETextJustify has non-null PinSubCategoryObject"), Result.PinSubCategoryObject.IsValid());
    return true;
}

// ============================================================================
// Unknown type falls back to Wildcard
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeUnknownTest,
    "PinWright.core.make_pin_type.UnknownWildcard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeUnknownTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("ThisTypeDoesNotExist_XYZ_12345"));
    TestEqual(TEXT("Unknown type resolves to PC_Wildcard"), Result.PinCategory, UEdGraphSchema_K2::PC_Wildcard);
    return true;
}

// ============================================================================
// Fix regression tests: ConvertCppTypeToPinType / BlueprintHandlerUtils fixes
//
// Each test is paired with the pre-fix behaviour it inverts.
// ============================================================================

// Test 1 — struct resolution: FVector (hardcoded) and FLinearColor/LinearColor
// parity.
//
// Pre-fix: "LinearColor" (bare, no F prefix) could miss the struct-fallback
//   when ResolveUScriptStruct did not strip the F prefix — returning wildcard.
//   "FLinearColor" was hardcoded, so it always worked.
// Post-fix: ResolveUScriptStruct resolves both forms identically; MakePinType
//   should return the same PinSubCategoryObject for both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeStructFPrefixTest,
    "PinWright.core.make_pin_type.StructFPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeStructFPrefixTest::RunTest(const FString& Parameters)
{
    // FVector is hardcoded — verify it resolves to the correct struct.
    FEdGraphPinType VectorResult = MakePinType(TEXT("FVector"));
    TestEqual(TEXT("FVector category is PC_Struct"), VectorResult.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("FVector PinSubCategoryObject is non-null"), VectorResult.PinSubCategoryObject.IsValid());
    if (VectorResult.PinSubCategoryObject.IsValid())
    {
        TestEqual(TEXT("FVector PinSubCategoryObject is TBaseStructure<FVector>"),
            VectorResult.PinSubCategoryObject.Get(),
            (UObject*)TBaseStructure<FVector>::Get());
    }

    // FLinearColor and LinearColor must resolve to the same non-null pin type.
    // Pre-fix: "LinearColor" (without F) could return wildcard if ResolveUScriptStruct
    //   did not strip the leading F prefix during its iterator fallback.
    // Post-fix: both must produce PC_Struct with the same PinSubCategoryObject.
    FEdGraphPinType WithFResult    = MakePinType(TEXT("FLinearColor"));
    FEdGraphPinType WithoutFResult = MakePinType(TEXT("LinearColor"));

    TestEqual(TEXT("FLinearColor category is PC_Struct"), WithFResult.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("FLinearColor PinSubCategoryObject is non-null"), WithFResult.PinSubCategoryObject.IsValid());

    TestEqual(TEXT("LinearColor category is PC_Struct"), WithoutFResult.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("LinearColor PinSubCategoryObject is non-null"), WithoutFResult.PinSubCategoryObject.IsValid());

    // Both must point at the same struct object.
    if (WithFResult.PinSubCategoryObject.IsValid() && WithoutFResult.PinSubCategoryObject.IsValid())
    {
        TestTrue(TEXT("FLinearColor and LinearColor resolve to the same struct"),
            WithFResult.PinSubCategoryObject.Get() == WithoutFResult.PinSubCategoryObject.Get());
    }

    return true;
}

// Test 2 — pointer to built-in class: "UObject*"
//
// Pre-fix: the T* pointer branch may have called an older resolution path that
//   returned true (non-wildcard PC_Object) but left PinSubCategoryObject null.
// Post-fix: ResolveUClass("UObject") strips the U prefix → finds UObject class;
//   PinSubCategoryObject must be non-null and equal UObject::StaticClass().
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeBuiltinClassPointerTest,
    "PinWright.core.make_pin_type.BuiltinClassPointer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeBuiltinClassPointerTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("UObject*"));
    TestEqual(TEXT("UObject* category is PC_Object"), Result.PinCategory, UEdGraphSchema_K2::PC_Object);
    // Pre-fix assertion that inverts: pre-fix left PinSubCategoryObject null on silent success.
    TestTrue(TEXT("UObject* PinSubCategoryObject is non-null"), Result.PinSubCategoryObject.IsValid());
    if (Result.PinSubCategoryObject.IsValid())
    {
        TestEqual(TEXT("UObject* PinSubCategoryObject is UObject::StaticClass()"),
            Result.PinSubCategoryObject.Get(),
            (UObject*)UObject::StaticClass());
    }
    return true;
}

// Test 3 — pointer to a non-existent class must fall back to wildcard.
//
// Pre-fix: the pointer branch returned true (non-wildcard PC_Object) with a null
//   PinSubCategoryObject, so MakePinType produced a non-wildcard pin for a type
//   that could not be resolved — a silent corruption.
// Post-fix: ConvertCppTypeToPinType returns false on miss; MakePinType falls back
//   to wildcard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeClassPointerMissRejectsTest,
    "PinWright.core.make_pin_type.ClassPointerMissRejects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeClassPointerMissRejectsTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType Result = MakePinType(TEXT("UNonexistentClass_Xyz_Zzz*"));
    // Pre-fix: PinCategory was PC_Object with null PinSubCategoryObject (non-wildcard).
    // Post-fix: must be wildcard because ResolveUClass returns null → false → wildcard.
    TestEqual(TEXT("UNonexistentClass_Xyz_Zzz* resolves to PC_Wildcard"),
        Result.PinCategory, UEdGraphSchema_K2::PC_Wildcard);
    return true;
}

// Test 4 — "struct:Vector" colon-separator prefix form.
//
// Pre-fix: the keyword-strip code did not accept ':' as a separator, so
//   "struct:Vector" was not recognised and fell through to wildcard.
// Post-fix: the strip loop now accepts ':' as a separator; "struct:Vector"
//   resolves identically to bare "Vector".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeStructColonPrefixTest,
    "PinWright.core.make_pin_type.StructColonPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeStructColonPrefixTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType ColonForm = MakePinType(TEXT("struct:Vector"));
    FEdGraphPinType BareForm  = MakePinType(TEXT("Vector"));

    // Pre-fix: ColonForm would be PC_Wildcard (unrecognised prefix form).
    TestEqual(TEXT("struct:Vector category is PC_Struct"), ColonForm.PinCategory, UEdGraphSchema_K2::PC_Struct);
    TestTrue(TEXT("struct:Vector PinSubCategoryObject is non-null"), ColonForm.PinSubCategoryObject.IsValid());

    // Parity: both forms must produce the same pin type.
    TestEqual(TEXT("struct:Vector and Vector share PinCategory"),
        ColonForm.PinCategory, BareForm.PinCategory);
    if (ColonForm.PinSubCategoryObject.IsValid() && BareForm.PinSubCategoryObject.IsValid())
    {
        TestTrue(TEXT("struct:Vector and Vector resolve the same UScriptStruct"),
            ColonForm.PinSubCategoryObject.Get() == BareForm.PinSubCategoryObject.Get());
    }
    return true;
}

// Test 5 — BuildNamedPinDescriptor rejects a wildcard result.
//
// Pre-fix: MakePinType returned a wildcard for unresolvable types;
//   BuildNamedPinDescriptor did not check for wildcard and returned true,
//   so callers silently created untyped (wildcard) pins that get dropped
//   by ReconstructNode without any error.
// Post-fix: BuildNamedPinDescriptor checks for wildcard and returns false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildNamedPinDescriptorRejectsWildcardTest,
    "PinWright.core.make_pin_type.BuildNamedPinDescriptorRejectsWildcard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBuildNamedPinDescriptorRejectsWildcardTest::RunTest(const FString& Parameters)
{
    FNamedPinTypeDescriptor OutDescriptor;
    // "BogusType_Xyz" does not resolve to any known type. Post-refactor,
    // callers parse through BpirTypeSpecParser first. ParseTypeSpec succeeds
    // for a bare identifier (Kind=Unresolved) but ConvertTypeSpecToPinType
    // cannot resolve it to any UClass/UEnum/UScriptStruct, so
    // BuildNamedPinDescriptor must return false.
    FBpirTypeSpec BogusSpec;
    FString ParseErr;
    int32 ParseCol = INDEX_NONE;
    const bool bParsed = BpirTypeSpecParser::ParseTypeSpec(TEXT("BogusType_Xyz"), BogusSpec, ParseErr, ParseCol);
    TestTrue(TEXT("ParseTypeSpec accepts a bare identifier (Unresolved)"), bParsed);

    const bool bResult = BuildNamedPinDescriptor(TEXT("x"), BogusSpec, OutDescriptor);
    TestFalse(TEXT("BuildNamedPinDescriptor returns false for unresolvable type"), bResult);
    return true;
}

// ============================================================================
// enum<T> entry-signature param should route through the unified resolver and
// return PC_Byte with the enum as PinSubCategoryObject — matching the bare
// short-name form MakePinType("ESlateVisibility").
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMakePinTypeEnumParamFormTest,
    "PinWright.core.make_pin_type.EnumParamForm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMakePinTypeEnumParamFormTest::RunTest(const FString& Parameters)
{
    FEdGraphPinType ParamForm = MakePinType(TEXT("enum<ESlateVisibility>"));
    TestEqual(TEXT("enum<ESlateVisibility> category is PC_Byte"),
        ParamForm.PinCategory, UEdGraphSchema_K2::PC_Byte);
    TestTrue(TEXT("enum<ESlateVisibility> has non-null PinSubCategoryObject"),
        ParamForm.PinSubCategoryObject.IsValid());
    if (ParamForm.PinSubCategoryObject.IsValid())
    {
        TestEqual(TEXT("Sub-object is ESlateVisibility enum"),
            ParamForm.PinSubCategoryObject->GetName(), FString(TEXT("ESlateVisibility")));
    }

    // Parity with the bare short-name form — both must produce the same pin type.
    FEdGraphPinType BareForm = MakePinType(TEXT("ESlateVisibility"));
    TestEqual(TEXT("Bare and enum<T> forms share PinCategory"),
        ParamForm.PinCategory, BareForm.PinCategory);
    TestTrue(TEXT("Bare and enum<T> forms resolve the same UEnum"),
        ParamForm.PinSubCategoryObject.Get() == BareForm.PinSubCategoryObject.Get());
    return true;
}

// ============================================================================
// blueprint.add_variable advertises the set<T> / map<K,V> container wrappers.
//
// Regression for E-add-variable-set-map-wrappers-undocumented: the parser has
// long accepted set<T>/map<K,V> (alongside array<T>) but blueprint.add_variable
// advertised only array<T> in its variableType param description, so the
// capability was undiscoverable without reading the parser source. This ticket
// is a docs/advertisement gap, not a parser bug — the parse-path honesty
// (set<T>->TSet pin, map<K,V>->TMap pin via ParseTypeSpec -> ConvertTypeSpecToPinType)
// is already pinned by FCodePinResolverConvertSetSupportedTest /
// FCodePinResolverConvertMapSupportedTest in TestCompilerResolvers.cpp, so this
// test guards only the new contract: the registered variableType param
// description must name set<T> and map<K,V>. Reverting the param-string edit
// drops the 'set<'/'map<' substrings and fails this test.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddVariableAdvertisesSetMapWrappersTest,
    "PinWright.core.make_pin_type.AddVariableAdvertisesSetMapWrappers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddVariableAdvertisesSetMapWrappersTest::RunTest(const FString& Parameters)
{
    const FParamSpec* VarTypeParam =
        GetRegisteredParamSpec(TEXT("blueprint.add_variable"), TEXT("variableType"));
    TestNotNull(TEXT("blueprint.add_variable has a variableType param"), VarTypeParam);
    if (VarTypeParam)
    {
        TestTrue(TEXT("variableType description advertises set<T>"),
            VarTypeParam->Description.Contains(TEXT("set<")));
        TestTrue(TEXT("variableType description advertises map<K,V>"),
            VarTypeParam->Description.Contains(TEXT("map<")));
    }
    return true;
}

// ============================================================================
// Regression for B-rpc-input-class-path-silent-wildcard:
// networking.create_rpc_function / blueprint.add_function input/output pin parsing
// must REJECT a token that would degrade to a wildcard pin (e.g. the documented-
// but-unsupported 'class:/Script/Engine.Actor' colon-path form) instead of
// silently building a wildcard pin and returning success. The pre-fix behaviour
// parsed inputs in AllowWildcardFallback mode and only logged a warning, so a
// caller got success:true and a malformed BP that failed at a later compile.
//
// This drives the production helper FindFirstPinParamWildcardFallback through the
// production parse path (ParseNamedTypePinParams in AllowWildcardFallback mode —
// the exact mode both handlers use), so reverting either the helper or its wiring
// fails this test.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPinParamRejectsClassColonPathTest,
    "PinWright.core.make_pin_type.PinParamRejectsClassColonPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPinParamRejectsClassColonPathTest::RunTest(const FString& Parameters)
{
    auto MakePinParamArray = [](const FString& Name, const FString& Type)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("type"), Type);
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueObject>(Obj));
        return Arr;
    };

    // The documented-but-unsupported colon-path token: parses as a miss, then the
    // pre-fix path silently wildcarded. The helper must report a rejection.
    {
        TArray<FParsedPinParam> Parsed;
        FString ParseErr;
        ParseNamedTypePinParams(
            MakePinParamArray(TEXT("ClassRef"), TEXT("class:/Script/Engine.Actor")),
            Parsed, EParsedPinParamMode::AllowWildcardFallback, ParseErr, TEXT("input param"));
        TestEqual(TEXT("one input param parsed"), Parsed.Num(), 1);

        FString WildcardError;
        const bool bRejected =
            FindFirstPinParamWildcardFallback(Parsed, TEXT("input"), WildcardError);
        TestTrue(TEXT("class:/Script/Engine.Actor input is rejected (not silently wildcarded)"), bRejected);
        TestTrue(TEXT("rejection message names the offending token"),
            WildcardError.Contains(TEXT("class:/Script/Engine.Actor")));
        TestTrue(TEXT("rejection message lists accepted forms"),
            WildcardError.Contains(TEXT("object<T>")));
    }

    // A bare unknown identifier parses fine but resolves to no type — must also be
    // rejected (matches add_variable's PC_Wildcard check).
    {
        TArray<FParsedPinParam> Parsed;
        FString ParseErr;
        ParseNamedTypePinParams(
            MakePinParamArray(TEXT("Mystery"), TEXT("BogusType_Xyz_98765")),
            Parsed, EParsedPinParamMode::AllowWildcardFallback, ParseErr, TEXT("input param"));
        FString WildcardError;
        TestTrue(TEXT("unresolved bare identifier input is rejected"),
            FindFirstPinParamWildcardFallback(Parsed, TEXT("input"), WildcardError));
    }

    // The correct BPIR wrapper form resolves to a concrete pin — must be ACCEPTED
    // (helper returns false). Guards against an over-broad rejection that would
    // break the documented workaround.
    {
        TArray<FParsedPinParam> Parsed;
        FString ParseErr;
        ParseNamedTypePinParams(
            MakePinParamArray(TEXT("Instigator"), TEXT("object<Actor>")),
            Parsed, EParsedPinParamMode::AllowWildcardFallback, ParseErr, TEXT("input param"));
        FString WildcardError;
        TestFalse(TEXT("object<Actor> input is accepted (resolves to a concrete pin)"),
            FindFirstPinParamWildcardFallback(Parsed, TEXT("input"), WildcardError));
    }

    // An empty param list must be accepted (no tokens to reject).
    {
        TArray<FParsedPinParam> Empty;
        FString WildcardError;
        TestFalse(TEXT("empty param list is accepted"),
            FindFirstPinParamWildcardFallback(Empty, TEXT("input"), WildcardError));
    }

    return true;
}

#undef MakePinType
