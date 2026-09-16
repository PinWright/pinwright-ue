// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirTypeSpec.cpp - Unit tests for FBpirTypeSpec / BpirTypeSpecParser (parse + re-emit round trip, no UE pin conversion).

#include "Misc/AutomationTest.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"

// Small helper: parse, fail the test on error, stash the spec for the caller.
static bool ParseOrFail(FAutomationTestBase& Test, const FString& Source, FBpirTypeSpec& OutSpec)
{
    FString Err;
    int32 Col = INDEX_NONE;
    const bool bOk = BpirTypeSpecParser::ParseTypeSpec(Source, OutSpec, Err, Col);
    if (!bOk)
    {
        Test.AddError(FString::Printf(TEXT("ParseTypeSpec('%s') failed: %s (col=%d)"), *Source, *Err, Col));
    }
    return bOk;
}

// ============================================================================
// 1. Primitives round-trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecPrimitivesTest,
    "PinWright.bpir.type_spec.Primitives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecPrimitivesTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* Input;
        EBpirTypeKind ExpectedKind;
        const TCHAR* ExpectedCanonical;
    };

    const FCase Cases[] = {
        { TEXT("void"),    EBpirTypeKind::Void,   TEXT("void") },
        { TEXT("bool"),    EBpirTypeKind::Bool,   TEXT("bool") },
        { TEXT("boolean"), EBpirTypeKind::Bool,   TEXT("bool") },
        { TEXT("byte"),    EBpirTypeKind::Byte,   TEXT("byte") },
        { TEXT("int"),     EBpirTypeKind::Int,    TEXT("int") },
        { TEXT("int32"),   EBpirTypeKind::Int,    TEXT("int") },
        { TEXT("integer"), EBpirTypeKind::Int,    TEXT("int") },
        { TEXT("int64"),   EBpirTypeKind::Int64,  TEXT("int64") },
        { TEXT("float"),   EBpirTypeKind::Float,  TEXT("float") },
        { TEXT("double"),  EBpirTypeKind::Double, TEXT("double") },
        { TEXT("string"),  EBpirTypeKind::String, TEXT("string") },
        { TEXT("FString"), EBpirTypeKind::String, TEXT("string") },
        { TEXT("name"),    EBpirTypeKind::Name,   TEXT("name") },
        { TEXT("FName"),   EBpirTypeKind::Name,   TEXT("name") },
        { TEXT("text"),    EBpirTypeKind::Text,   TEXT("text") },
        { TEXT("FText"),   EBpirTypeKind::Text,   TEXT("text") },
    };

    for (const FCase& C : Cases)
    {
        FBpirTypeSpec Spec;
        if (!ParseOrFail(*this, C.Input, Spec)) continue;
        TestEqual(FString::Printf(TEXT("Kind for '%s'"), C.Input), (int32)Spec.Kind, (int32)C.ExpectedKind);
        const FString Emitted = BpirTypeSpecParser::TypeSpecToBpirText(Spec);
        TestEqual(FString::Printf(TEXT("Canonical for '%s'"), C.Input), Emitted, FString(C.ExpectedCanonical));
    }

    return true;
}

// ============================================================================
// 2. Tagged forms — struct<T>, object<T>, etc.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecTaggedFormsTest,
    "PinWright.bpir.type_spec.Tagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecTaggedFormsTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* Input;
        EBpirTypeKind ExpectedKind;
        const TCHAR* ExpectedInner; // empty string => NAME_None
        const TCHAR* ExpectedCanonical;
    };

    const FCase Cases[] = {
        { TEXT("struct<Vector>"),            EBpirTypeKind::Struct,     TEXT("Vector"),            TEXT("struct<Vector>") },
        { TEXT("object<Actor>"),             EBpirTypeKind::Object,     TEXT("Actor"),             TEXT("object<Actor>") },
        { TEXT("softobject<Actor>"),         EBpirTypeKind::SoftObject, TEXT("Actor"),             TEXT("softobject<Actor>") },
        { TEXT("class<Widget>"),             EBpirTypeKind::Class,      TEXT("Widget"),            TEXT("class<Widget>") },
        { TEXT("softclass<Widget>"),         EBpirTypeKind::SoftClass,  TEXT("Widget"),            TEXT("softclass<Widget>") },
        { TEXT("enum<EReplaySaveState>"),    EBpirTypeKind::Enum,       TEXT("EReplaySaveState"),  TEXT("enum<EReplaySaveState>") },
        { TEXT("interface<IFoo>"),           EBpirTypeKind::Interface,  TEXT("IFoo"),              TEXT("interface<IFoo>") },
        { TEXT("delegate"),                  EBpirTypeKind::Delegate,   TEXT(""),                  TEXT("delegate") },
        { TEXT("mcdelegate"),                EBpirTypeKind::McDelegate, TEXT(""),                  TEXT("mcdelegate") },
    };

    for (const FCase& C : Cases)
    {
        FBpirTypeSpec Spec;
        if (!ParseOrFail(*this, C.Input, Spec)) continue;
        TestEqual(FString::Printf(TEXT("Kind for '%s'"), C.Input), (int32)Spec.Kind, (int32)C.ExpectedKind);
        const FString ExpectedInner(C.ExpectedInner);
        if (ExpectedInner.IsEmpty())
        {
            TestTrue(FString::Printf(TEXT("InnerName NAME_None for '%s'"), C.Input), Spec.InnerName.IsNone());
        }
        else
        {
            TestEqual(FString::Printf(TEXT("InnerName for '%s'"), C.Input), Spec.InnerName.ToString(), ExpectedInner);
        }
        const FString Emitted = BpirTypeSpecParser::TypeSpecToBpirText(Spec);
        TestEqual(FString::Printf(TEXT("Canonical for '%s'"), C.Input), Emitted, FString(C.ExpectedCanonical));
    }

    return true;
}

// ============================================================================
// 3. C++-style prefix sugarings — `struct Foo`, `struct:Foo`, etc.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecPrefixSugaringsTest,
    "PinWright.bpir.type_spec.PrefixSugarings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecPrefixSugaringsTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* SugaredInput;
        const TCHAR* EquivalentAngleInput;
    };

    const FCase Cases[] = {
        { TEXT("struct Vector"),              TEXT("struct<Vector>") },
        { TEXT("struct:Vector"),              TEXT("struct<Vector>") },
        { TEXT("class Widget"),               TEXT("class<Widget>") },
        { TEXT("class:Widget"),               TEXT("class<Widget>") },
        { TEXT("enum EReplaySaveState"),      TEXT("enum<EReplaySaveState>") },
        { TEXT("enum:EReplaySaveState"),      TEXT("enum<EReplaySaveState>") },
    };

    for (const FCase& C : Cases)
    {
        FBpirTypeSpec Sugared;
        if (!ParseOrFail(*this, C.SugaredInput, Sugared)) continue;

        FBpirTypeSpec Canonical;
        if (!ParseOrFail(*this, C.EquivalentAngleInput, Canonical)) continue;

        TestTrue(FString::Printf(TEXT("Specs equal for '%s' vs '%s'"),
            C.SugaredInput, C.EquivalentAngleInput), Sugared.Equals(Canonical));

        // Re-emits as the angle-bracket form.
        const FString Emitted = BpirTypeSpecParser::TypeSpecToBpirText(Sugared);
        TestEqual(FString::Printf(TEXT("Canonical re-emit for '%s'"), C.SugaredInput),
            Emitted, FString(C.EquivalentAngleInput));
    }

    return true;
}

// ============================================================================
// 4. Pointer suffix — `UMyClass*`
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecPointerSuffixTest,
    "PinWright.bpir.type_spec.PointerSuffix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecPointerSuffixTest::RunTest(const FString& Parameters)
{
    FBpirTypeSpec Spec;
    if (!ParseOrFail(*this, TEXT("UMyClass*"), Spec)) return true;

    TestEqual(TEXT("Kind is Object"), (int32)Spec.Kind, (int32)EBpirTypeKind::Object);
    TestEqual(TEXT("InnerName"), Spec.InnerName.ToString(), FString(TEXT("UMyClass")));
    TestEqual(TEXT("Re-emit"),
        BpirTypeSpecParser::TypeSpecToBpirText(Spec),
        FString(TEXT("object<UMyClass>")));

    return true;
}

// ============================================================================
// 5. Qualifiers — const T, T&, const T&
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecQualifiersTest,
    "PinWright.bpir.type_spec.Qualifiers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecQualifiersTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* Input;
        bool bExpectConst;
        bool bExpectRef;
        const TCHAR* ExpectedCanonical;
    };

    const FCase Cases[] = {
        { TEXT("const int"),           true,  false, TEXT("const int") },
        { TEXT("int&"),                false, true,  TEXT("int&") },
        { TEXT("const int&"),          true,  true,  TEXT("const int&") },
        { TEXT("const struct<FVector>"),       true,  false, TEXT("const struct<FVector>") },
        { TEXT("struct<FVector>&"),            false, true,  TEXT("struct<FVector>&") },
        { TEXT("const struct<FVector>&"),      true,  true,  TEXT("const struct<FVector>&") },
    };

    for (const FCase& C : Cases)
    {
        FBpirTypeSpec Spec;
        if (!ParseOrFail(*this, C.Input, Spec)) continue;
        TestEqual(FString::Printf(TEXT("bIsConst for '%s'"), C.Input), Spec.bIsConst, C.bExpectConst);
        TestEqual(FString::Printf(TEXT("bIsReference for '%s'"), C.Input), Spec.bIsReference, C.bExpectRef);
        const FString Emitted = BpirTypeSpecParser::TypeSpecToBpirText(Spec);
        TestEqual(FString::Printf(TEXT("Canonical for '%s'"), C.Input), Emitted, FString(C.ExpectedCanonical));
    }

    return true;
}

// ============================================================================
// 6. Containers — array<T>, set<T>, map<K,V>
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecContainersTest,
    "PinWright.bpir.type_spec.Containers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecContainersTest::RunTest(const FString& Parameters)
{
    // array<int>
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("array<int>"), Spec))
        {
            TestEqual(TEXT("array<int> container"), (int32)Spec.Container, (int32)EPinContainerType::Array);
            TestTrue(TEXT("array<int> has ElementSpec"), Spec.ElementSpec.IsValid());
            if (Spec.ElementSpec.IsValid())
            {
                TestEqual(TEXT("array<int> element kind"), (int32)Spec.ElementSpec->Kind, (int32)EBpirTypeKind::Int);
            }
            TestEqual(TEXT("array<int> re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec), FString(TEXT("array<int>")));
        }
    }

    // array<struct<FVector>>
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("array<struct<FVector>>"), Spec))
        {
            TestEqual(TEXT("array<struct<FVector>> container"), (int32)Spec.Container, (int32)EPinContainerType::Array);
            if (Spec.ElementSpec.IsValid())
            {
                TestEqual(TEXT("element kind"), (int32)Spec.ElementSpec->Kind, (int32)EBpirTypeKind::Struct);
                TestEqual(TEXT("element inner"), Spec.ElementSpec->InnerName.ToString(), FString(TEXT("FVector")));
            }
            TestEqual(TEXT("re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec), FString(TEXT("array<struct<FVector>>")));
        }
    }

    // array<const struct<FVector>&>
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("array<const struct<FVector>&>"), Spec))
        {
            TestEqual(TEXT("container"), (int32)Spec.Container, (int32)EPinContainerType::Array);
            if (Spec.ElementSpec.IsValid())
            {
                TestTrue(TEXT("element bIsConst"), Spec.ElementSpec->bIsConst);
                TestTrue(TEXT("element bIsReference"), Spec.ElementSpec->bIsReference);
                TestEqual(TEXT("element kind"), (int32)Spec.ElementSpec->Kind, (int32)EBpirTypeKind::Struct);
            }
            TestEqual(TEXT("re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec),
                FString(TEXT("array<const struct<FVector>&>")));
        }
    }

    // set<int>
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("set<int>"), Spec))
        {
            TestEqual(TEXT("set<int> container"), (int32)Spec.Container, (int32)EPinContainerType::Set);
            TestEqual(TEXT("set<int> re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec), FString(TEXT("set<int>")));
        }
    }

    // map<string, int>
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("map<string, int>"), Spec))
        {
            TestEqual(TEXT("map container"), (int32)Spec.Container, (int32)EPinContainerType::Map);
            if (Spec.KeySpec.IsValid())
            {
                TestEqual(TEXT("map key kind"), (int32)Spec.KeySpec->Kind, (int32)EBpirTypeKind::String);
            }
            if (Spec.ElementSpec.IsValid())
            {
                TestEqual(TEXT("map value kind"), (int32)Spec.ElementSpec->Kind, (int32)EBpirTypeKind::Int);
            }
            TestEqual(TEXT("map re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec), FString(TEXT("map<string, int>")));
        }
    }

    // map<int, struct<FVector>>
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("map<int, struct<FVector>>"), Spec))
        {
            TestEqual(TEXT("map container"), (int32)Spec.Container, (int32)EPinContainerType::Map);
            if (Spec.KeySpec.IsValid())
            {
                TestEqual(TEXT("key kind"), (int32)Spec.KeySpec->Kind, (int32)EBpirTypeKind::Int);
            }
            if (Spec.ElementSpec.IsValid())
            {
                TestEqual(TEXT("value kind"), (int32)Spec.ElementSpec->Kind, (int32)EBpirTypeKind::Struct);
                TestEqual(TEXT("value inner"), Spec.ElementSpec->InnerName.ToString(), FString(TEXT("FVector")));
            }
            TestEqual(TEXT("map re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec),
                FString(TEXT("map<int, struct<FVector>>")));
        }
    }

    return true;
}

// ============================================================================
// 7. Unresolved fallback
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecUnresolvedTest,
    "PinWright.bpir.type_spec.Unresolved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecUnresolvedTest::RunTest(const FString& Parameters)
{
    // FVector is in the well-known struct table — resolves to Struct with InnerName=Vector.
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("FVector"), Spec))
        {
            TestEqual(TEXT("FVector kind"), (int32)Spec.Kind, (int32)EBpirTypeKind::Struct);
            TestEqual(TEXT("FVector inner"), Spec.InnerName.ToString(), FString(TEXT("Vector")));
        }
    }

    // Genuinely unknown identifier — stays Unresolved.
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("ECustomEnum"), Spec))
        {
            TestEqual(TEXT("ECustomEnum kind"), (int32)Spec.Kind, (int32)EBpirTypeKind::Unresolved);
            TestEqual(TEXT("ECustomEnum inner"), Spec.InnerName.ToString(), FString(TEXT("ECustomEnum")));
            TestEqual(TEXT("ECustomEnum re-emit"),
                BpirTypeSpecParser::TypeSpecToBpirText(Spec), FString(TEXT("ECustomEnum")));
        }
    }

    return true;
}

// ============================================================================
// 8. IsEmpty vs IsVoid — semantic distinction for "no return clause" vs "-> void"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecEmptyVsVoidTest,
    "PinWright.bpir.type_spec.EmptyVsVoid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecEmptyVsVoidTest::RunTest(const FString& Parameters)
{
    // Default-constructed: no type written at all (distinct from explicit void).
    {
        FBpirTypeSpec Spec;
        TestTrue(TEXT("Default-constructed spec IsEmpty"), Spec.IsEmpty());
        TestFalse(TEXT("Default-constructed spec is not IsVoid"), Spec.IsVoid());
    }

    // Parsed "void": explicit void return, not empty.
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("void"), Spec))
        {
            TestFalse(TEXT("Parsed 'void' is not IsEmpty"), Spec.IsEmpty());
            TestTrue(TEXT("Parsed 'void' IsVoid"), Spec.IsVoid());
        }
    }

    // Parsed "int": neither empty nor void.
    {
        FBpirTypeSpec Spec;
        if (ParseOrFail(*this, TEXT("int"), Spec))
        {
            TestFalse(TEXT("Parsed 'int' is not IsEmpty"), Spec.IsEmpty());
            TestFalse(TEXT("Parsed 'int' is not IsVoid"), Spec.IsVoid());
        }
    }

    return true;
}

// ============================================================================
// 9. Error quality — `array<const T&&>` is the pinning assertion
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTypeSpecErrorQualityTest,
    "PinWright.bpir.type_spec.ErrorQuality",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTypeSpecErrorQualityTest::RunTest(const FString& Parameters)
{
    FBpirTypeSpec Spec;
    FString Err;
    int32 Col = INDEX_NONE;
    const bool bOk = BpirTypeSpecParser::ParseTypeSpec(TEXT("array<const T&&>"), Spec, Err, Col);

    TestFalse(TEXT("Parse should fail"), bOk);
    TestTrue(FString::Printf(TEXT("Error msg contains 'unexpected '&' at column' (got '%s')"), *Err),
        Err.Contains(TEXT("unexpected '&' at column")));

    return true;
}
