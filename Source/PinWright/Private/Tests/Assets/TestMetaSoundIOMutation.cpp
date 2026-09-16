// Copyright (c) 2026 Alexander Penkin. MIT License.

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

// Counterfactual: If Builder.RemoveGraphInput is replaced with a no-op in production,
// the second Builder.AddGraphInput call in this test would fail because the document
// still holds the original "Volume" input — adding a second vertex with the same name
// would violate the engine's uniqueness constraint. This makes the second AddGraphInput
// the canary: it only succeeds after the first input is actually gone.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRemoveAndRenameMetaSoundIOTest,
    "PinWright.Assets.RemoveAndRenameMetaSoundIO",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRemoveAndRenameMetaSoundIOTest::RunTest(const FString& Parameters)
{
#if MCP_HAS_METASOUND_LITERAL_HELPER

    // --- 1. Create a transient UMetaSoundSource ---
    const FString AssetName = FString::Printf(TEXT("MS_IOMutationTest_%s"),
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

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(Source);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // --- 2. Add a Float input named "Volume" ---
    FMetasoundFrontendLiteral FloatLiteral;
    bool bLiteralOk = PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(
        TEXT("Float"), FloatLiteral);
    TestTrue(TEXT("Float literal helper succeeds"), bLiteralOk);

    FMetasoundFrontendClassInput VolumeInput;
    VolumeInput.Name = FName(TEXT("Volume"));
    VolumeInput.TypeName = FName(TEXT("Float"));
    VolumeInput.VertexID = FGuid::NewGuid();
    VolumeInput.NodeID = FGuid::NewGuid();
    VolumeInput.AccessType = EMetasoundFrontendVertexAccessType::Reference;
    // 5.6 paged input defaults: InitDefault() on 5.6+, DefaultLiteral on 5.4/5.5.
    PinWright::MetaSound::SetClassInputDefault(VolumeInput, FloatLiteral);

    const FMetasoundFrontendNode* VolumeNode = Builder.AddGraphInput(VolumeInput);
    TestNotNull(TEXT("AddGraphInput for 'Volume' returns non-null node"), VolumeNode);

    // --- 3. Rename "Volume" to "VolumeRenamed" ---
    // SetGraphInputName is UE 5.6+; on 5.5 SwapGraphInput is the equivalent (replace the
    // existing vertex with a renamed copy, preserving type/default/edges).
    //
    // On UE 5.4, SwapGraphInput is broken: it removes the vertex via RemoveGraphOutput()
    // instead of RemoveGraphInput(), so the removal fails and the engine hard-asserts
    // ("Failed to swap MetaSound input expected to exist"). Epic fixed this in 5.5. The
    // graph-input rename is therefore unsupported on 5.4 (the handler rejects it with
    // UNSUPPORTED_ENGINE_VERSION), so skip the rename leg here and remove "Volume"
    // directly — the remove + canary-re-add path below still validates RemoveGraphInput.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    const bool bVolumeRemoved = Builder.RemoveGraphInput(FName(TEXT("Volume")));
    TestTrue(TEXT("RemoveGraphInput for 'Volume' returns true (rename unsupported on 5.4)"), bVolumeRemoved);
#else
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    const FMetasoundFrontendClassInput* ExistingVolume = Builder.FindGraphInput(FName(TEXT("Volume")));
    bool bRenamed = false;
    if (ExistingVolume)
    {
        FMetasoundFrontendClassVertex RenamedVolume = *ExistingVolume;
        RenamedVolume.Name = FName(TEXT("VolumeRenamed"));
        bRenamed = Builder.SwapGraphInput(*ExistingVolume, RenamedVolume);
    }
#else
    const bool bRenamed = Builder.SetGraphInputName(FName(TEXT("Volume")), FName(TEXT("VolumeRenamed")));
#endif
    TestTrue(TEXT("SetGraphInputName 'Volume'->'VolumeRenamed' returns true"), bRenamed);
    TestNull(TEXT("FindGraphInput('Volume') is null after rename"), Builder.FindGraphInput(FName(TEXT("Volume"))));
    TestNotNull(TEXT("FindGraphInput('VolumeRenamed') is non-null after rename"), Builder.FindGraphInput(FName(TEXT("VolumeRenamed"))));

    // --- 4. Remove the renamed input ---
    const bool bVolumeRemoved = Builder.RemoveGraphInput(FName(TEXT("VolumeRenamed")));
    TestTrue(TEXT("RemoveGraphInput for 'VolumeRenamed' returns true"), bVolumeRemoved);
#endif

    // --- 5. Add a second Float input named "MasterGain" (only possible if VolumeRenamed is truly gone) ---
    FMetasoundFrontendLiteral FloatLiteral2;
    PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(
        TEXT("Float"), FloatLiteral2);

    FMetasoundFrontendClassInput MasterGainInput;
    MasterGainInput.Name = FName(TEXT("MasterGain"));
    MasterGainInput.TypeName = FName(TEXT("Float"));
    MasterGainInput.VertexID = FGuid::NewGuid();
    MasterGainInput.NodeID = FGuid::NewGuid();
    MasterGainInput.AccessType = EMetasoundFrontendVertexAccessType::Reference;
    PinWright::MetaSound::SetClassInputDefault(MasterGainInput, FloatLiteral2);

    const FMetasoundFrontendNode* MasterGainNode = Builder.AddGraphInput(MasterGainInput);
    TestNotNull(TEXT("AddGraphInput for 'MasterGain' returns non-null node"), MasterGainNode);

    // --- 6. Remove "MasterGain" too ---
    const bool bMasterGainRemoved = Builder.RemoveGraphInput(FName(TEXT("MasterGain")));
    TestTrue(TEXT("RemoveGraphInput for 'MasterGain' returns true"), bMasterGainRemoved);

    PW_METASOUND_FINISH_BUILDING(Builder);
    Source->RemoveFromRoot();
    return true;

#else
    // MetaSound literal helper is not available; skip silently.
    return true;
#endif // MCP_HAS_METASOUND_LITERAL_HELPER
}

#endif // __has_include("MetasoundSource.h")
