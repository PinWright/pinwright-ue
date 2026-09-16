// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the three literal gaps that blocked a stem-player MetaSound graph:
//   1. Trigger / WaveAsset graph-input types were rejected by MakeDefaultLiteralForMetaSoundType.
//   2. No object literal existed anywhere, and set_metasound_default silently fell back to
//      Literal.Set(0.0f) when no value param was supplied.
//   3. There was no node-pin literal setter, so a Wave Player's "Wave Asset" pin could not be bound.
//
// Every test asserts the failure direction as well as the success one (rpc-design §12): the
// missing-value test in particular is the guard on the DELETED float-zero fallback — it asserts a
// previously-set value SURVIVES a valueless call, which the fallback would have overwritten with 0
// while reporting success.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"

#if __has_include("MetasoundSource.h")

#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundSource.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if MCP_HAS_METASOUND_LITERAL_HELPER

// Named (not anonymous) namespace: Unity merges TUs and these helper names would otherwise
// collide with the identically-purposed file-local copies in sibling test files.
namespace PinWrightMetaSoundLiteralGapTests
{
    // Creates a transient UMetaSoundSource whose document has its default page seeded, so the
    // priming builder the handlers construct does not assert on an empty PagedGraphs array.
    // OutPackagePath is the bare /Game/... path the handlers accept; OutObjectPath is the
    // Package.Object form the teardown helper needs.
    UMetaSoundSource* NewSeededMetaSound(const TCHAR* NamePrefix, FString& OutPackagePath, FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("%s%s"), NamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackagePath, *AssetName);

        UPackage* Package = CreatePackage(*OutPackagePath);
        UMetaSoundSource* Source = NewObject<UMetaSoundSource>(
            Package, UMetaSoundSource::StaticClass(), *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Source)
        {
            return nullptr;
        }

        // A freshly NewObject'd MetaSoundSource has zero PagedGraphs; seed the default page with
        // a non-priming builder first (mirrors TestMetaSoundIOMutation.cpp).
        {
            TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Source);
            FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
            SeedBuilder.InitDocument();
            PW_METASOUND_FINISH_BUILDING(SeedBuilder);
        }
        return Source;
    }

    // Creates a transient USoundWave resolvable by asset path, for objectValue binding.
    USoundWave* NewTransientSoundWave(FString& OutPackagePath, FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("SW_MSLiteralGap_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackagePath, *AssetName);

        UPackage* Package = CreatePackage(*OutPackagePath);
        return NewObject<USoundWave>(Package, USoundWave::StaticClass(), *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
    }

    // Reads a graph input's stored default straight off the document — the same independent path
    // the production handlers use for their readback, so a test cannot pass by re-reading the
    // setter's own argument.
    bool FindGraphInputDefaultOnDocument(UObject* MetaSound, const FString& InputName,
        FMetasoundFrontendLiteral& OutLiteral, FString& OutTypeName)
    {
        const IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSound);
        if (!DocInterface)
        {
            return false;
        }
        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        const FMetasoundFrontendClassInterface& ClassInterface =
            PinWright::MetaSound::GetClassDefaultInterface(Doc.RootGraph);
        const FName InputFName(*InputName);
        for (const FMetasoundFrontendClassInput& Input : ClassInterface.Inputs)
        {
            if (Input.Name != InputFName)
            {
                continue;
            }
            OutTypeName = Input.TypeName.ToString();
            if (const FMetasoundFrontendLiteral* Found = PinWright::MetaSound::FindClassInputDefault(Input))
            {
                OutLiteral = *Found;
                return true;
            }
            return false;
        }
        return false;
    }

    // Invokes add_metasound_input through the registered handler and returns whether it succeeded.
    bool AddGraphInput(const FString& PackagePath, const FString& InputName, const FString& InputType,
        FTestResponseCapture& OutCapture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("inputName"), InputName);
        Payload->SetStringField(TEXT("inputType"), InputType);
        Payload->SetBoolField(TEXT("save"), false);
        return InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_input"), Payload, OutCapture);
    }
}

// ============================================================================
// Gap 1 — Trigger graph inputs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundTriggerInputIsCreatedAndReadableTest,
    "PinWright.audio.authoring.add_metasound_input.TriggerInputIsCreatedAndReadable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundTriggerInputIsCreatedAndReadableTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;

    // The helper is the production seam the handler calls; assert it directly too, so a
    // reverted Trigger branch fails here even if the handler is refactored.
    FMetasoundFrontendLiteral TriggerLiteral;
    TestTrue(TEXT("MakeDefaultLiteralForMetaSoundType accepts 'Trigger'"),
        PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(TEXT("Trigger"), TriggerLiteral));
    TestTrue(TEXT("Trigger's default literal is Boolean-shaped (its registered ELiteralType)"),
        TriggerLiteral.GetType() == EMetasoundFrontendLiteralType::Boolean);

    FString PackagePath, ObjectPath;
    UMetaSoundSource* Source = NewSeededMetaSound(TEXT("MS_TriggerInput_"), PackagePath, ObjectPath);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    FTestResponseCapture Capture;
    TestTrue(TEXT("add_metasound_input handler registered"),
        AddGraphInput(PackagePath, TEXT("Play"), TEXT("Trigger"), Capture));
    TestTrue(TEXT("Trigger input is accepted (was INVALID_TYPE before this fix)"), Capture.bSuccess);
    TestNotEqual(TEXT("Trigger no longer errors as an unsupported type"),
        Capture.ErrorCode, FString(TEXT("INVALID_TYPE")));

    FMetasoundFrontendLiteral StoredLiteral;
    FString StoredTypeName;
    const bool bFound = FindGraphInputDefaultOnDocument(Source, TEXT("Play"), StoredLiteral, StoredTypeName);
    TestTrue(TEXT("The document carries a 'Play' input with a default literal"), bFound);
    TestEqual(TEXT("'Play' is stored under the registry's canonical 'Trigger' key"),
        StoredTypeName, FString(TEXT("Trigger")));
    if (bFound)
    {
        TestTrue(TEXT("'Play' default literal is Boolean-shaped"),
            StoredLiteral.GetType() == EMetasoundFrontendLiteralType::Boolean);
    }

    Source->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    return true;
}

// ============================================================================
// Gap 1 + 2 — WaveAsset graph inputs and object literals
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundWaveAssetInputAcceptsSoundWaveTest,
    "PinWright.audio.authoring.set_metasound_default.WaveAssetInputAcceptsSoundWave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundWaveAssetInputAcceptsSoundWaveTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;

    FMetasoundFrontendLiteral WaveAssetDefault;
    TestTrue(TEXT("MakeDefaultLiteralForMetaSoundType accepts 'WaveAsset'"),
        PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(TEXT("WaveAsset"), WaveAssetDefault));
    TestTrue(TEXT("WaveAsset's default literal is UObject-shaped"),
        WaveAssetDefault.GetType() == EMetasoundFrontendLiteralType::UObject);

    FString PackagePath, ObjectPath;
    UMetaSoundSource* Source = NewSeededMetaSound(TEXT("MS_WaveAssetInput_"), PackagePath, ObjectPath);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    FString WavePackagePath, WaveObjectPath;
    USoundWave* Wave = NewTransientSoundWave(WavePackagePath, WaveObjectPath);
    TestNotNull(TEXT("Transient USoundWave created"), Wave);
    if (!Wave)
    {
        Source->RemoveFromRoot();
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        return false;
    }
    Wave->AddToRoot();

    FTestResponseCapture Capture;
    TestTrue(TEXT("add_metasound_input handler registered"),
        AddGraphInput(PackagePath, TEXT("Stem"), TEXT("WaveAsset"), Capture));
    TestTrue(TEXT("WaveAsset input is accepted (was INVALID_TYPE before this fix)"), Capture.bSuccess);

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), PackagePath);
    SetPayload->SetStringField(TEXT("inputName"), TEXT("Stem"));
    SetPayload->SetStringField(TEXT("objectValue"), WavePackagePath);
    SetPayload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("set_metasound_default handler registered"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), SetPayload, Capture));
    TestTrue(TEXT("objectValue binds a USoundWave to a WaveAsset input"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // The verb must report the value it MEASURED off the document, not the one it was given.
        const TSharedPtr<FJsonObject>* StoredJson = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("storedDefault"), StoredJson) && StoredJson)
        {
            FString ReportedObjectPath;
            TestTrue(TEXT("storedDefault carries the bound objectPath"),
                (*StoredJson)->TryGetStringField(TEXT("objectPath"), ReportedObjectPath));
            TestEqual(TEXT("The document holds the wave we asked for"),
                ReportedObjectPath, Wave->GetPathName());
        }
        else
        {
            AddError(TEXT("set_metasound_default reported success without a storedDefault readback"));
        }
    }

    // Independent confirmation straight off the document.
    FMetasoundFrontendLiteral StoredLiteral;
    FString StoredTypeName;
    if (FindGraphInputDefaultOnDocument(Source, TEXT("Stem"), StoredLiteral, StoredTypeName))
    {
        TestEqual(TEXT("'Stem' is stored under the 'WaveAsset' registry key"),
            StoredTypeName, FString(TEXT("WaveAsset")));
        UObject* BoundObject = nullptr;
        TestTrue(TEXT("'Stem' default is a UObject literal"), StoredLiteral.TryGet(BoundObject));
        TestTrue(TEXT("'Stem' default is the SoundWave"), BoundObject == static_cast<UObject*>(Wave));
    }
    else
    {
        AddError(TEXT("Document carries no default for the 'Stem' WaveAsset input"));
    }

    Wave->RemoveFromRoot();
    Source->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WaveObjectPath);
    return true;
}

// ============================================================================
// Gap 2 — failure directions: unresolvable path, wrong class, no value at all
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundObjectValueFailureDirectionsTest,
    "PinWright.audio.authoring.set_metasound_default.ObjectValueFailureDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundObjectValueFailureDirectionsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;

    FString PackagePath, ObjectPath;
    UMetaSoundSource* Source = NewSeededMetaSound(TEXT("MS_ObjectValueFail_"), PackagePath, ObjectPath);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    FString WavePackagePath, WaveObjectPath;
    USoundWave* Wave = NewTransientSoundWave(WavePackagePath, WaveObjectPath);
    if (!Wave)
    {
        Source->RemoveFromRoot();
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        return false;
    }
    Wave->AddToRoot();

    FTestResponseCapture Capture;
    AddGraphInput(PackagePath, TEXT("Stem"), TEXT("WaveAsset"), Capture);
    TestTrue(TEXT("Baseline WaveAsset input added"), Capture.bSuccess);

    // Bind a real wave first, so both rejections below can be shown to change NOTHING.
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("assetPath"), PackagePath);
        SetPayload->SetStringField(TEXT("inputName"), TEXT("Stem"));
        SetPayload->SetStringField(TEXT("objectValue"), WavePackagePath);
        SetPayload->SetBoolField(TEXT("save"), false);
        InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), SetPayload, Capture);
        TestTrue(TEXT("Baseline bind succeeded"), Capture.bSuccess);
    }

    auto BoundObjectOnDocument = [Source]() -> UObject*
    {
        FMetasoundFrontendLiteral StoredLiteral;
        FString StoredTypeName;
        if (!FindGraphInputDefaultOnDocument(Source, TEXT("Stem"), StoredLiteral, StoredTypeName))
        {
            return nullptr;
        }
        UObject* BoundObject = nullptr;
        StoredLiteral.TryGet(BoundObject);
        return BoundObject;
    };
    TestTrue(TEXT("Baseline: the wave is bound"), BoundObjectOnDocument() == static_cast<UObject*>(Wave));

    // --- Unresolvable path -------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Stem"));
        Payload->SetStringField(TEXT("objectValue"),
            FString::Printf(TEXT("/Game/PinWrightTests/NoSuchWave_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("set_metasound_default handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), Payload, Capture));
        TestFalse(TEXT("An unresolvable objectValue is an error, not a silent no-op success"),
            Capture.bSuccess);
        TestEqual(TEXT("Unresolvable objectValue reports OBJECT_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("OBJECT_NOT_FOUND")));
        TestTrue(TEXT("A rejected path leaves the previous binding untouched"),
            BoundObjectOnDocument() == static_cast<UObject*>(Wave));
    }

    // --- Wrong class -------------------------------------------------------
    {
        // The MetaSound itself is a UObject that is emphatically not a USoundWave, so the
        // registry's IsValidUObjectForDataType must refuse it for a WaveAsset input.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Stem"));
        Payload->SetStringField(TEXT("objectValue"), PackagePath);
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("set_metasound_default handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), Payload, Capture));
        TestFalse(TEXT("A wrong-class objectValue is an error, not a success"), Capture.bSuccess);
        TestEqual(TEXT("Wrong-class objectValue reports INVALID_ASSET_TYPE"),
            Capture.ErrorCode, FString(TEXT("INVALID_ASSET_TYPE")));
        TestTrue(TEXT("A wrong-class rejection leaves the previous binding untouched"),
            BoundObjectOnDocument() == static_cast<UObject*>(Wave));
    }

    Wave->RemoveFromRoot();
    Source->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WaveObjectPath);
    return true;
}

// ============================================================================
// Gap 2 — the regression guard on the DELETED Literal.Set(0.0f) fallback
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundMissingValueIsAnErrorNotAZeroTest,
    "PinWright.audio.authoring.set_metasound_default.MissingValueIsAnErrorNotAZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundMissingValueIsAnErrorNotAZeroTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;

    FString PackagePath, ObjectPath;
    UMetaSoundSource* Source = NewSeededMetaSound(TEXT("MS_MissingValue_"), PackagePath, ObjectPath);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    FTestResponseCapture Capture;
    AddGraphInput(PackagePath, TEXT("Intensity"), TEXT("Float"), Capture);
    TestTrue(TEXT("Baseline Float input added"), Capture.bSuccess);

    // Set a value that is NOT zero, so the deleted fallback would be visible if it returned.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Intensity"));
        Payload->SetNumberField(TEXT("floatValue"), 0.75);
        Payload->SetBoolField(TEXT("save"), false);
        InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), Payload, Capture);
        TestTrue(TEXT("Baseline floatValue write succeeded"), Capture.bSuccess);
    }

    auto StoredFloat = [Source]() -> float
    {
        FMetasoundFrontendLiteral StoredLiteral;
        FString StoredTypeName;
        if (!FindGraphInputDefaultOnDocument(Source, TEXT("Intensity"), StoredLiteral, StoredTypeName))
        {
            return TNumericLimits<float>::Lowest();
        }
        float Value = TNumericLimits<float>::Lowest();
        StoredLiteral.TryGet(Value);
        return Value;
    };
    TestEqual(TEXT("Baseline: the document holds 0.75"), StoredFloat(), 0.75f);

    // The whole point: a call with NO value param must be rejected. The removed fallback
    // (Literal.Set(0.0f)) would have overwritten 0.75 with 0 and reported success.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("inputName"), TEXT("Intensity"));
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("set_metasound_default handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), Payload, Capture));
        TestFalse(TEXT("A valueless call is an error, not a silent float-zero write"), Capture.bSuccess);
        TestEqual(TEXT("A valueless call reports MISSING_VALUE"),
            Capture.ErrorCode, FString(TEXT("MISSING_VALUE")));
        TestEqual(TEXT("The previously set 0.75 survives (the float-zero fallback is gone)"),
            StoredFloat(), 0.75f);
    }

    Source->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    return true;
}

// ============================================================================
// Gap 3 — set_metasound_node_input_default
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundNodeInputDefaultBindsWavePlayerTest,
    "PinWright.audio.authoring.set_metasound_node_input_default.BindsWavePlayerWaveAssetPin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundNodeInputDefaultBindsWavePlayerTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;

    TestTrue(TEXT("set_metasound_node_input_default is registered"),
        IsHandlerRegistered(TEXT("audio.authoring.set_metasound_node_input_default")));

    FString PackagePath, ObjectPath;
    UMetaSoundSource* Source = NewSeededMetaSound(TEXT("MS_NodeInputDefault_"), PackagePath, ObjectPath);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    FString WavePackagePath, WaveObjectPath;
    USoundWave* Wave = NewTransientSoundWave(WavePackagePath, WaveObjectPath);
    if (!Wave)
    {
        Source->RemoveFromRoot();
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        return false;
    }
    Wave->AddToRoot();

    // Add a Wave Player node through the production verb, so the node id under test is the one
    // an agent would actually hold.
    FTestResponseCapture Capture;
    FString NodeId;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("nodeType"), TEXT("waveplayer"));
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("add_metasound_node handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_node"), Payload, Capture));
        TestTrue(TEXT("Wave Player node added"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
        }
    }

    if (NodeId.IsEmpty())
    {
        AddError(TEXT("add_metasound_node returned no nodeId; cannot exercise the pin binding"));
        Wave->RemoveFromRoot();
        Source->RemoveFromRoot();
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WaveObjectPath);
        return false;
    }

    // --- Failure direction first: an invented pin name must be refused ------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("nodeId"), NodeId);
        // The registry spells this pin with a space; the no-space guess is the usual mistake.
        Payload->SetStringField(TEXT("inputName"), TEXT("WaveAsset"));
        Payload->SetStringField(TEXT("objectValue"), WavePackagePath);
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("set_metasound_node_input_default handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_node_input_default"), Payload, Capture));
        TestFalse(TEXT("An unknown pin name is an error, not a success"), Capture.bSuccess);
        TestEqual(TEXT("An unknown pin name reports INPUT_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("INPUT_NOT_FOUND")));
    }

    // --- The binding itself ------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("nodeId"), NodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("Wave Asset"));
        Payload->SetStringField(TEXT("objectValue"), WavePackagePath);
        Payload->SetBoolField(TEXT("save"), false);
        TestTrue(TEXT("set_metasound_node_input_default handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_node_input_default"), Payload, Capture));
        TestTrue(TEXT("The Wave Player's \"Wave Asset\" pin accepts a USoundWave"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>* StoredJson = nullptr;
            if (Capture.Result->TryGetObjectField(TEXT("storedDefault"), StoredJson) && StoredJson)
            {
                FString ReportedObjectPath;
                (*StoredJson)->TryGetStringField(TEXT("objectPath"), ReportedObjectPath);
                TestEqual(TEXT("The pin literal read back off the document is the wave"),
                    ReportedObjectPath, Wave->GetPathName());
            }
            else
            {
                AddError(TEXT("set_metasound_node_input_default reported success without a storedDefault readback"));
            }
        }
    }

    // --- A second surface must see it: describe_metasound -------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        // describe_metasound resolves with StaticLoadObject, which needs the Package.Object form
        // for a never-saved transient package (a bare package path resolves to the UPackage).
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        TArray<TSharedPtr<FJsonValue>> NodeIds;
        NodeIds.Add(MakeShared<FJsonValueString>(NodeId));
        Payload->SetArrayField(TEXT("nodeIds"), NodeIds);
        TestTrue(TEXT("describe_metasound handler registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.describe_metasound"), Payload, Capture));
        TestTrue(TEXT("describe_metasound succeeds"), Capture.bSuccess);

        bool bDescribeShowsWave = false;
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
            {
                for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
                {
                    const TSharedPtr<FJsonObject>* NodeObj = nullptr;
                    if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj)
                    {
                        continue;
                    }
                    const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
                    if (!(*NodeObj)->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs)
                    {
                        continue;
                    }
                    for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
                    {
                        const TSharedPtr<FJsonObject>* InputObj = nullptr;
                        if (!InputValue.IsValid() || !InputValue->TryGetObject(InputObj) || !InputObj)
                        {
                            continue;
                        }
                        FString VertexName, DefaultLiteral;
                        (*InputObj)->TryGetStringField(TEXT("name"), VertexName);
                        (*InputObj)->TryGetStringField(TEXT("defaultLiteral"), DefaultLiteral);
                        if (VertexName == TEXT("Wave Asset") && DefaultLiteral.Contains(Wave->GetName()))
                        {
                            bDescribeShowsWave = true;
                        }
                    }
                }
            }
        }
        TestTrue(TEXT("describe_metasound reports the bound wave on the \"Wave Asset\" pin"),
            bDescribeShowsWave);
    }

    Wave->RemoveFromRoot();
    Source->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WaveObjectPath);
    return true;
}

// ============================================================================
// Gap 4 — array-typed inputs and pins could be CREATED but never POPULATED
// (F-metasound-array-inputs-cannot-be-populated)
//
// add_metasound_input accepted "WaveAsset:Array" and really made the input, but every value param
// was a scalar, so nothing could be written into it: objectValue was refused by the registry (an
// array data type sets only bIsProxyArrayParsable, while IsValidUObjectForDataType tests
// bIsProxyParsable) with advice to use floatValue / intValue / boolValue / stringValue, none of
// which can carry a list. Since no "make array" node exists and every Array.* node consumes an
// array, the whole family — Array.Random Get, Shuffle, Get, Set, Concat, and Random Get's
// Float:Array "Weights" pin — was unreachable from the RPC surface.
//
// The two tests below split at the seam: the first pins the pure literal-building contract (which
// is what BOTH set_metasound_default and set_metasound_node_input_default call, so a node pin is
// covered by the same assertions), the second drives the graph-input verb end to end and reads the
// result back off the document rather than off the setter's own argument.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundArrayLiteralShapeFollowsDataTypeTest,
    "PinWright.audio.authoring.metasound_array_literal.ShapeFollowsTheDataType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundArrayLiteralShapeFollowsDataTypeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;
    using namespace PinWright::MetaSound;

    // --- Array-type classification and the element name the registry does not provide ---
    TestTrue(TEXT("'Float:Array' classifies as an array data type"),
        IsMetaSoundArrayDataType(TEXT("Float:Array")));
    TestTrue(TEXT("'WaveAsset:Array' classifies as an array data type"),
        IsMetaSoundArrayDataType(TEXT("WaveAsset:Array")));
    TestFalse(TEXT("'Float' does NOT classify as an array data type"),
        IsMetaSoundArrayDataType(TEXT("Float")));
    TestEqual(TEXT("'WaveAsset:Array' yields the element type 'WaveAsset'"),
        GetMetaSoundArrayElementTypeName(TEXT("WaveAsset:Array")), FString(TEXT("WaveAsset")));
    TestEqual(TEXT("A scalar type yields no element type"),
        GetMetaSoundArrayElementTypeName(TEXT("Float")), FString());
    // The convenience aliases have to travel through the array name too, or "Int:Array" would be
    // accepted for a scalar spelling and rejected as unregistered for its array.
    TestEqual(TEXT("'Int:Array' canonicalizes through its element to 'Int32:Array'"),
        CanonicalizeMetaSoundTypeName(TEXT("Int:Array")), FString(TEXT("Int32:Array")));
    TestEqual(TEXT("'Boolean:Array' canonicalizes to 'Bool:Array'"),
        CanonicalizeMetaSoundTypeName(TEXT("Boolean:Array")), FString(TEXT("Bool:Array")));
    TestEqual(TEXT("An already-canonical array name is unchanged"),
        CanonicalizeMetaSoundTypeName(TEXT("WaveAsset:Array")), FString(TEXT("WaveAsset:Array")));

    // --- Float:Array: the shape of Array.Random Get's "Weights" pin ---
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Add(MakeShared<FJsonValueNumber>(0.25));
        Entries.Add(MakeShared<FJsonValueNumber>(0.75));

        FMetasoundFrontendLiteral Literal;
        FMetaSoundArrayLiteralOutcome Outcome;
        if (TestTrue(TEXT("A Float:Array literal is built from JSON numbers"),
                MakeArrayLiteralForMetaSoundType(Entries, TEXT("Float:Array"), Literal, Outcome)))
        {
            TestTrue(TEXT("The built literal is FloatArray-shaped"),
                Literal.GetType() == EMetasoundFrontendLiteralType::FloatArray);
            TArray<float> Values;
            if (TestTrue(TEXT("The literal reads back as a float array"), Literal.TryGet(Values))
                && TestEqual(TEXT("Both entries landed"), Values.Num(), 2))
            {
                TestEqual(TEXT("Entry 0 is 0.25"), Values[0], 0.25f);
                TestEqual(TEXT("Entry 1 is 0.75"), Values[1], 0.75f);
            }
        }
    }

    // --- Empty array is a legal value, not a failure: it is how a caller clears an array ---
    {
        const TArray<TSharedPtr<FJsonValue>> NoEntries;
        FMetasoundFrontendLiteral Literal;
        FMetaSoundArrayLiteralOutcome Outcome;
        if (TestTrue(TEXT("An empty JSON array builds an empty array literal"),
                MakeArrayLiteralForMetaSoundType(NoEntries, TEXT("Float:Array"), Literal, Outcome)))
        {
            TArray<float> Values;
            TestTrue(TEXT("The empty literal is still FloatArray-shaped"), Literal.TryGet(Values));
            TestEqual(TEXT("It holds no entries"), Values.Num(), 0);
        }
    }

    // --- Element form is decided by the DATA TYPE, not by the JSON that arrived ---
    {
        TArray<TSharedPtr<FJsonValue>> WrongKind;
        WrongKind.Add(MakeShared<FJsonValueString>(TEXT("0.25")));

        FMetasoundFrontendLiteral Literal;
        FMetaSoundArrayLiteralOutcome Outcome;
        TestFalse(TEXT("A Float:Array refuses a string entry rather than coercing it"),
            MakeArrayLiteralForMetaSoundType(WrongKind, TEXT("Float:Array"), Literal, Outcome));
        TestTrue(TEXT("The refusal is an element type mismatch"),
            Outcome.Result == EMetaSoundArrayLiteralResult::ElementTypeMismatch);
        TestEqual(TEXT("It names the offending entry index"), Outcome.FailedIndex, 0);
    }
    {
        // A non-integral number into Int32:Array would be a value the caller never asked for.
        TArray<TSharedPtr<FJsonValue>> Fractional;
        Fractional.Add(MakeShared<FJsonValueNumber>(1.7));

        FMetasoundFrontendLiteral Literal;
        FMetaSoundArrayLiteralOutcome Outcome;
        TestFalse(TEXT("An Int32:Array refuses a fractional entry rather than rounding it"),
            MakeArrayLiteralForMetaSoundType(Fractional, TEXT("Int32:Array"), Literal, Outcome));
        TestTrue(TEXT("The fractional refusal is an element type mismatch"),
            Outcome.Result == EMetaSoundArrayLiteralResult::ElementTypeMismatch);
    }

    // --- A scalar type is refused outright, so arrayValue cannot be written to the wrong target ---
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Add(MakeShared<FJsonValueNumber>(1.0));

        FMetasoundFrontendLiteral Literal;
        FMetaSoundArrayLiteralOutcome Outcome;
        TestFalse(TEXT("A scalar 'Float' refuses an array literal"),
            MakeArrayLiteralForMetaSoundType(Entries, TEXT("Float"), Literal, Outcome));
        TestTrue(TEXT("The refusal says the target is not an array type"),
            Outcome.Result == EMetaSoundArrayLiteralResult::NotAnArrayType);
    }

    // --- WaveAsset:Array: entries are asset paths, class-checked against the ELEMENT type ---
    {
        FString WavePackagePath, WaveObjectPath;
        USoundWave* Wave = NewTransientSoundWave(WavePackagePath, WaveObjectPath);
        if (TestNotNull(TEXT("Transient USoundWave created"), Wave))
        {
            Wave->AddToRoot();

            TArray<TSharedPtr<FJsonValue>> Entries;
            Entries.Add(MakeShared<FJsonValueString>(WavePackagePath));

            FMetasoundFrontendLiteral Literal;
            FMetaSoundArrayLiteralOutcome Outcome;
            if (TestTrue(TEXT("A WaveAsset:Array literal is built from an asset path"),
                    MakeArrayLiteralForMetaSoundType(Entries, TEXT("WaveAsset:Array"), Literal, Outcome)))
            {
                TestTrue(TEXT("The built literal is UObjectArray-shaped"),
                    Literal.GetType() == EMetasoundFrontendLiteralType::UObjectArray);
                TArray<UObject*> Objects;
                if (TestTrue(TEXT("The literal reads back as an object array"), Literal.TryGet(Objects))
                    && TestEqual(TEXT("One object landed"), Objects.Num(), 1))
                {
                    TestTrue(TEXT("The element is the SoundWave"),
                        Objects[0] == static_cast<UObject*>(Wave));
                }
            }

            // Failure direction: an entry that resolves to nothing must not become a null slot.
            TArray<TSharedPtr<FJsonValue>> MissingEntry;
            MissingEntry.Add(MakeShared<FJsonValueString>(TEXT("/Game/PinWrightTests/NoSuchWave_ZZZ")));
            FMetasoundFrontendLiteral MissingLiteral;
            FMetaSoundArrayLiteralOutcome MissingOutcome;
            TestFalse(TEXT("An unresolvable entry refuses the whole array"),
                MakeArrayLiteralForMetaSoundType(MissingEntry, TEXT("WaveAsset:Array"),
                    MissingLiteral, MissingOutcome));
            TestTrue(TEXT("The refusal is element-not-found"),
                MissingOutcome.Result == EMetaSoundArrayLiteralResult::ElementNotFound);

            Wave->RemoveFromRoot();
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WaveObjectPath);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundArrayInputAcceptsArrayValueTest,
    "PinWright.audio.authoring.set_metasound_default.ArrayInputAcceptsArrayValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundArrayInputAcceptsArrayValueTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightMetaSoundLiteralGapTests;

    FString PackagePath, ObjectPath;
    UMetaSoundSource* Source = NewSeededMetaSound(TEXT("MS_ArrayInput_"), PackagePath, ObjectPath);
    TestNotNull(TEXT("Transient UMetaSoundSource created"), Source);
    if (!Source)
    {
        return false;
    }
    Source->AddToRoot();

    FString WavePackagePath, WaveObjectPath;
    USoundWave* Wave = NewTransientSoundWave(WavePackagePath, WaveObjectPath);
    TestNotNull(TEXT("Transient USoundWave created"), Wave);
    if (!Wave)
    {
        Source->RemoveFromRoot();
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
        return false;
    }
    Wave->AddToRoot();

    FTestResponseCapture Capture;
    TestTrue(TEXT("add_metasound_input handler registered"),
        AddGraphInput(PackagePath, TEXT("Variants"), TEXT("WaveAsset:Array"), Capture));
    TestTrue(TEXT("A WaveAsset:Array graph input is created"), Capture.bSuccess);

    // The verb under test: the array input becomes populatable.
    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Add(MakeShared<FJsonValueString>(WavePackagePath));

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), PackagePath);
    SetPayload->SetStringField(TEXT("inputName"), TEXT("Variants"));
    SetPayload->SetArrayField(TEXT("arrayValue"), Entries);
    SetPayload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("set_metasound_default handler registered"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), SetPayload, Capture));
    TestTrue(*FString::Printf(TEXT("arrayValue populates a WaveAsset:Array input (error was '%s': %s)"),
        *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* StoredJson = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("storedDefault"), StoredJson) && StoredJson)
        {
            // The array readback shape: a caller verifying an array needs its length and its
            // elements, not one flattened ToString().
            double ReportedNum = 0.0;
            TestTrue(TEXT("storedDefault carries arrayNum"),
                (*StoredJson)->TryGetNumberField(TEXT("arrayNum"), ReportedNum));
            TestEqual(TEXT("arrayNum is 1"), static_cast<int32>(ReportedNum), 1);
            const TArray<TSharedPtr<FJsonValue>>* ReportedPaths = nullptr;
            if (TestTrue(TEXT("storedDefault carries objectPaths"),
                    (*StoredJson)->TryGetArrayField(TEXT("objectPaths"), ReportedPaths))
                && ReportedPaths && ReportedPaths->Num() == 1)
            {
                TestEqual(TEXT("objectPaths[0] is the wave we asked for"),
                    (*ReportedPaths)[0]->AsString(), Wave->GetPathName());
            }
        }
        else
        {
            AddError(TEXT("set_metasound_default reported success without a storedDefault readback"));
        }
    }

    // Independent confirmation straight off the document, not off the response.
    FMetasoundFrontendLiteral StoredLiteral;
    FString StoredTypeName;
    if (FindGraphInputDefaultOnDocument(Source, TEXT("Variants"), StoredLiteral, StoredTypeName))
    {
        TestEqual(TEXT("'Variants' is stored under the 'WaveAsset:Array' registry key"),
            StoredTypeName, FString(TEXT("WaveAsset:Array")));
        TArray<UObject*> BoundObjects;
        if (TestTrue(TEXT("'Variants' default is a UObject array literal"),
                StoredLiteral.TryGet(BoundObjects))
            && TestEqual(TEXT("It holds one element"), BoundObjects.Num(), 1))
        {
            TestTrue(TEXT("The element is the SoundWave"),
                BoundObjects[0] == static_cast<UObject*>(Wave));
        }
    }
    else
    {
        AddError(TEXT("Document carries no default for the 'Variants' WaveAsset:Array input"));
    }

    // Failure direction 1 — the exact call the ticket filed: objectValue on an array input. It
    // used to answer "accepts no object literal ... use floatValue / intValue / boolValue /
    // stringValue", advice no scalar param can satisfy. It must now name arrayValue instead.
    TSharedPtr<FJsonObject> ScalarPayload = MakeShared<FJsonObject>();
    ScalarPayload->SetStringField(TEXT("assetPath"), PackagePath);
    ScalarPayload->SetStringField(TEXT("inputName"), TEXT("Variants"));
    ScalarPayload->SetStringField(TEXT("objectValue"), WavePackagePath);
    ScalarPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture ScalarCapture;
    TestTrue(TEXT("set_metasound_default handler registered (scalar-on-array)"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"), ScalarPayload, ScalarCapture));
    TestFalse(TEXT("objectValue on an array-typed input is refused"), ScalarCapture.bSuccess);
    TestTrue(TEXT("The refusal points the caller at arrayValue"),
        ScalarCapture.Message.Contains(TEXT("arrayValue")));

    // Failure direction 2 — arrayValue on a scalar-typed input is refused, not silently ignored.
    TestTrue(TEXT("add_metasound_input handler registered (scalar input)"),
        AddGraphInput(PackagePath, TEXT("Gain"), TEXT("Float"), Capture));
    TestTrue(TEXT("A scalar Float graph input is created"), Capture.bSuccess);

    TArray<TSharedPtr<FJsonValue>> NumberEntries;
    NumberEntries.Add(MakeShared<FJsonValueNumber>(1.0));
    TSharedPtr<FJsonObject> WrongTargetPayload = MakeShared<FJsonObject>();
    WrongTargetPayload->SetStringField(TEXT("assetPath"), PackagePath);
    WrongTargetPayload->SetStringField(TEXT("inputName"), TEXT("Gain"));
    WrongTargetPayload->SetArrayField(TEXT("arrayValue"), NumberEntries);
    WrongTargetPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture WrongTargetCapture;
    TestTrue(TEXT("set_metasound_default handler registered (array-on-scalar)"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.set_metasound_default"),
            WrongTargetPayload, WrongTargetCapture));
    TestFalse(TEXT("arrayValue on a scalar-typed input is refused"), WrongTargetCapture.bSuccess);

    // ...and nothing was written by either refusal: the array default still holds the one wave.
    FMetasoundFrontendLiteral SurvivingLiteral;
    FString SurvivingTypeName;
    if (FindGraphInputDefaultOnDocument(Source, TEXT("Variants"), SurvivingLiteral, SurvivingTypeName))
    {
        TArray<UObject*> SurvivingObjects;
        TestTrue(TEXT("The array default survives the refused scalar write"),
            SurvivingLiteral.TryGet(SurvivingObjects) && SurvivingObjects.Num() == 1);
    }

    Wave->RemoveFromRoot();
    Source->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(WaveObjectPath);
    return true;
}

#endif // MCP_HAS_METASOUND_LITERAL_HELPER

#endif // __has_include("MetasoundSource.h")
