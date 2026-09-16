// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#if __has_include("MetasoundSource.h")

#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundSource.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddMetaSoundInputLiteralPopulatesDefaultsTest,
    "PinWright.Assets.AddMetaSoundInputLiteralPopulatesDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddMetaSoundInputLiteralPopulatesDefaultsTest::RunTest(const FString& Parameters)
{
#if MCP_HAS_METASOUND_LITERAL_HELPER

    // --- 1. Verify the helper itself works for a known type ---
    FMetasoundFrontendLiteral Literal;
    bool bMade = PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(TEXT("Float"), Literal);
    TestTrue(TEXT("MakeDefaultLiteralForMetaSoundType returns true for Float"), bMade);
    TestTrue(TEXT("Float literal type is not Invalid"),
        Literal.GetType() != EMetasoundFrontendLiteralType::Invalid);

    // --- 2. Verify the helper rejects unknown types ---
    FMetasoundFrontendLiteral BadLiteral;
    bool bBadMade = PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(TEXT("UnknownType"), BadLiteral);
    TestFalse(TEXT("MakeDefaultLiteralForMetaSoundType returns false for UnknownType"), bBadMade);

    // --- 3. Create a transient UMetaSoundSource ---
    const FString AssetName = FString::Printf(TEXT("MS_InputLiteralTest_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    UMetaSoundSource* Source = NewObject<UMetaSoundSource>(
        Package,
        UMetaSoundSource::StaticClass(),
        *AssetName,
        RF_Public | RF_Standalone | RF_Transient);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    // A freshly NewObject'd MetaSoundSource has an empty FMetasoundFrontendDocument
    // with zero PagedGraphs. The V2 builder's BeginBuilding() (called from its ctor with
    // bPrimeCache=true) walks the default page via FindConstGraphChecked() and asserts.
    // Real assets always go through the document builder during creation, so mirror that
    // by seeding the default page first with a non-priming builder that runs InitDocument().
    {
        TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Source);
        FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
        SeedBuilder.InitDocument();
        PW_METASOUND_FINISH_BUILDING(SeedBuilder);
    }

    // --- 4. Build a ClassInput the same way the production handler does ---
    FMetasoundFrontendClassInput ClassInput;
    ClassInput.Name = FName(TEXT("TestInput"));
    ClassInput.TypeName = FName(TEXT("Float"));
    ClassInput.VertexID = FGuid::NewGuid();
    ClassInput.NodeID = FGuid::NewGuid();
    ClassInput.AccessType = EMetasoundFrontendVertexAccessType::Reference;

    FMetasoundFrontendLiteral FloatLiteral;
    bool bLiteralOk = PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(
        TEXT("Float"), FloatLiteral);
    TestTrue(TEXT("Float literal prepared for ClassInput"), bLiteralOk);
    // 5.6 paged input defaults: InitDefault() on 5.6+, DefaultLiteral on 5.4/5.5.
    PinWright::MetaSound::SetClassInputDefault(ClassInput, FloatLiteral);

    // --- 5. Run the builder and assert AddGraphInput succeeds without crashing ---
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(Source);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    const FMetasoundFrontendNode* InputNode = Builder.AddGraphInput(ClassInput);
    TestNotNull(TEXT("Builder.AddGraphInput returns non-null node"), InputNode);

    PW_METASOUND_FINISH_BUILDING(Builder);
    Source->RemoveFromRoot();
    return true;

#else
    // MetaSound literal helper is not available; skip silently.
    return true;
#endif // MCP_HAS_METASOUND_LITERAL_HELPER
}

#endif // __has_include("MetasoundSource.h")
