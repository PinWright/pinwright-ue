// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestMetaSoundVariables.cpp
// Exercises the AddGraphVariable / RemoveGraphVariable engine API path that
// audio.authoring.add_metasound_variable / remove_metasound_variable rely on.
//
// Counterfactual: if Builder.AddGraphVariable is reverted or removed in
// production, the handler returns VARIABLE_FAILED, and this test (which
// directly exercises the engine API) would still compile — but the assertion
// TestNotNull("AddGraphVariable returned variable") would fail, proving that
// the engine API the handler relies on is no longer reachable.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"

#if __has_include("MetasoundSource.h")

#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundSource.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "UObject/Package.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // Creates a rooted transient UMetaSoundSource with its default page seeded,
    // so the V2 builder's BeginBuilding() (which walks the default page via
    // FindConstGraphChecked() and asserts on an empty PagedGraphs) is safe to use.
    // A freshly NewObject'd source has zero PagedGraphs; real assets always go
    // through the document builder during creation, so mirror that with a
    // non-priming SeedBuilder that runs InitDocument(). Returns nullptr (after
    // recording the failed assertion) if creation fails; the caller owns the
    // matching RemoveFromRoot().
    UMetaSoundSource* MakeSeededTransientMetaSound(FAutomationTestBase& Test, const TCHAR* NamePrefix)
    {
        const FString AssetName = FString::Printf(TEXT("%s_%s"), NamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(
            TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UMetaSoundSource* MetaSound = NewObject<UMetaSoundSource>(
            Package,
            UMetaSoundSource::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);

        Test.TestNotNull(TEXT("Transient UMetaSoundSource created"), MetaSound);
        if (!MetaSound)
        {
            return nullptr;
        }
        MetaSound->AddToRoot();

        TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(MetaSound);
        FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
        SeedBuilder.InitDocument();
        PW_METASOUND_FINISH_BUILDING(SeedBuilder);

        return MetaSound;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddRemoveMetaSoundVariableTest,
    "PinWright.Assets.AddRemoveMetaSoundVariable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddRemoveMetaSoundVariableTest::RunTest(const FString& Parameters)
{
    // Create a rooted, default-page-seeded transient UMetaSoundSource to test against.
    UMetaSoundSource* MetaSound = MakeSeededTransientMetaSound(*this, TEXT("MS_VarTest"));
    if (!MetaSound)
    {
        return false;
    }

    // Build a Float literal — same API path as the production handler when
    // no explicit value param is provided (falls back to default 0.0f).
    FMetasoundFrontendLiteral Literal;
    Literal.Set(0.0f);

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);

    // Builder ctor gained a bPrimeCache 3rd arg in 5.6; macro selects the right form.
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // The builder graph-variable API (AddGraphVariable / RemoveGraphVariable) is a
    // UE 5.6 feature with no 5.4/5.5 equivalent — nothing to exercise here, so the
    // test is a graceful no-op on those versions (matches the handler's behavior).
    (void)Literal;
    PW_METASOUND_FINISH_BUILDING(Builder);
    MetaSound->RemoveFromRoot();
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("MetaSound graph-variable builder API requires UE 5.6+; test skipped."));
    return true;
#else
    const FMetasoundFrontendVariable* Variable =
        Builder.AddGraphVariable(FName("Phase"), FName("Float"), &Literal);

    TestNotNull(TEXT("AddGraphVariable returns non-null"), Variable);
    if (Variable)
    {
        // Engine API present and working — verify the remove path as well.
        bool bRemoved = Builder.RemoveGraphVariable(FName("Phase"));
        TestTrue(TEXT("RemoveGraphVariable returns true for an existing variable"), bRemoved);
    }

    PW_METASOUND_FINISH_BUILDING(Builder);

    MetaSound->RemoveFromRoot();
    return true;
#endif
}

// Regression for B-metasound-variable-int-type-rejected: the documented
// variableType "Int" must reach the builder as the registry's canonical key
// "Int32". The production handler now canonicalizes via
// CanonicalizeMetaSoundTypeName before AddGraphVariable.
//
// Counterfactual: if CanonicalizeMetaSoundTypeName is reverted to pass "Int"
// through unchanged, FName("Int") is unregistered in the MetaSound data-type
// registry, AddGraphVariable returns null, and the "Int (canonicalized) adds a
// variable" assertion below fails — exactly the bug this ticket reports. The
// pre-flight assertion that raw "Int" is rejected by the builder anchors the
// test to the real engine contract so the fix isn't a no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddMetaSoundVariableIntAliasTest,
    "PinWright.Assets.AddMetaSoundVariableIntAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddMetaSoundVariableIntAliasTest::RunTest(const FString& Parameters)
{
    // The production canonicalizer must rewrite the documented "Int" to the
    // registry key "Int32" (case-insensitively) and leave the others alone.
    using namespace PinWright::MetaSound;
    TestEqual(TEXT("'Int' canonicalizes to 'Int32'"),
        CanonicalizeMetaSoundTypeName(TEXT("Int")), FString(TEXT("Int32")));
    TestEqual(TEXT("'int' canonicalizes to 'Int32' (case-insensitive)"),
        CanonicalizeMetaSoundTypeName(TEXT("int")), FString(TEXT("Int32")));
    TestEqual(TEXT("'Int32' passes through unchanged"),
        CanonicalizeMetaSoundTypeName(TEXT("Int32")), FString(TEXT("Int32")));
    TestEqual(TEXT("'Float' passes through unchanged"),
        CanonicalizeMetaSoundTypeName(TEXT("Float")), FString(TEXT("Float")));
    TestEqual(TEXT("'Bool' passes through unchanged (registry key is 'Bool')"),
        CanonicalizeMetaSoundTypeName(TEXT("Bool")), FString(TEXT("Bool")));
    TestEqual(TEXT("'String' passes through unchanged"),
        CanonicalizeMetaSoundTypeName(TEXT("String")), FString(TEXT("String")));

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // AddGraphVariable is a UE 5.6 builder feature — the canonicalizer assertions
    // above already cover the fix on older engines; the builder path is skipped.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("MetaSound graph-variable builder API requires UE 5.6+; builder path skipped."));
    return true;
#else
    UMetaSoundSource* MetaSound = MakeSeededTransientMetaSound(*this, TEXT("MS_VarIntTest"));
    if (!MetaSound)
    {
        return false;
    }

    FMetasoundFrontendLiteral Literal;
    Literal.Set(static_cast<int32>(3));

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Pre-flight: the raw documented name "Int" is rejected by the engine
    // registry (the bug). This anchors the test to the real contract — if the
    // engine ever started accepting "Int", the fix would be unnecessary.
    const FMetasoundFrontendVariable* RawIntVar =
        Builder.AddGraphVariable(FName("IntRaw"), FName("Int"), &Literal);
    TestNull(TEXT("Raw 'Int' is rejected by the builder registry"), RawIntVar);

    // The fix: canonicalize the documented "Int" before AddGraphVariable.
    const FString Canonical = CanonicalizeMetaSoundTypeName(TEXT("Int"));
    const FMetasoundFrontendVariable* IntVar =
        Builder.AddGraphVariable(FName("IntCanon"), FName(*Canonical), &Literal);
    TestNotNull(TEXT("Canonicalized 'Int' adds an integer graph variable"), IntVar);

    PW_METASOUND_FINISH_BUILDING(Builder);

    MetaSound->RemoveFromRoot();
    return true;
#endif
}

#endif // __has_include("MetasoundSource.h")
