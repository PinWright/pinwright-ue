// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"
#include "Curves/CurveFloat.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"

namespace
{
    FString MakeUniqueCanonicalNiagaraAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Bring a freshly-NewObject'd fixture up to the shape the production create verb leaves
    // its asset in. The generic overload is the no-op; only UNiagaraSystem needs work.
    void InitializeCanonicalTestAsset(UObject*) {}

    // UNiagaraSystem::PostInitProperties creates SystemSpawnScript / SystemUpdateScript but
    // does NOT give them a script source - that wiring lives in PostLoad (for a disk asset)
    // and in UNiagaraSystemFactoryNew::InitializeSystem (for a new one), which is exactly what
    // niagara.create_system calls. A bare NewObject system therefore has
    // SystemSpawnScript->GetLatestSource() == nullptr, which no asset a caller can actually
    // reach ever does, and UE 5.5 hard-crashes on it: GetAssetRegistryTags calls
    // EnsureFullyLoaded() -> UpdateSystemAfterLoad(), which dereferences that null source
    // unconditionally (NiagaraSystem.cpp:652). Any verb whose response carries the standard
    // asset verification block hits it, because that block probes the asset registry.
    // 5.6+ returns early from GetAssetRegistryTags while !bFullyLoaded and never reaches it,
    // which is why the same fixture was survivable there. Initialize the way production does
    // rather than special-casing the engine version: an uninitialized system is not a case
    // these tests are meant to cover.
    void InitializeCanonicalTestAsset(UNiagaraSystem* System)
    {
        if (System)
        {
            UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
        }
    }

    template <typename TObject>
    TObject* NewTransientCanonicalTestAsset(const TCHAR* Prefix, FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueCanonicalNiagaraAssetName(Prefix);
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        TObject* Asset = NewObject<TObject>(
            Package,
            TObject::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (Asset)
        {
            Asset->AddToRoot();
            InitializeCanonicalTestAsset(Asset);
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return Asset;
    }

    void EnsureEmitterGraphSource(UNiagaraEmitter* Emitter)
    {
        FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData || EmitterData->GraphSource)
        {
            return;
        }

        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Emitter, NAME_None, RF_Transactional);
        UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transactional);
        Source->NodeGraph = Graph;
        EmitterData->GraphSource = Source;
    }

    bool JsonArrayContainsIssueCode(const TSharedPtr<FJsonObject>& Object, const TCHAR* ArrayName, const TCHAR* Code)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(ArrayName, Values) || !Values)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                continue;
            }

            FString IssueCode;
            if (Value->AsObject()->TryGetStringField(TEXT("code"), IssueCode)
                && IssueCode.Equals(Code, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraValidateBasicNoEmittersWarningTest,
    "PinWright.niagara.validate.BasicNoEmittersWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateBasicNoEmittersWarningTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientCanonicalTestAsset<UNiagaraSystem>(TEXT("NS_ValidateBasic"), ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("level"), TEXT("basic"));

    TestTrue(TEXT("niagara.validate handler found"), InvokeHandlerWithCapture(TEXT("niagara.validate"), Payload, Capture));
    TestTrue(TEXT("niagara.validate sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.validate succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("niagara.validate result present"), Capture.Result.Get());
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("basic validation remains valid for warnings"), Capture.Result->GetBoolField(TEXT("valid")));
        TestEqual(TEXT("asset kind"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("NiagaraSystem")));
        TestTrue(TEXT("warnings contain NO_EMITTERS"), JsonArrayContainsIssueCode(Capture.Result, TEXT("warnings"), TEXT("NO_EMITTERS")));
        TestFalse(TEXT("errors do not contain NO_EMITTERS"), JsonArrayContainsIssueCode(Capture.Result, TEXT("errors"), TEXT("NO_EMITTERS")));
        TestTrue(TEXT("compile summary present"), Capture.Result->HasTypedField<EJson::Object>(TEXT("compile")));
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraValidateStrictNoEmittersErrorTest,
    "PinWright.niagara.validate.StrictNoEmittersError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateStrictNoEmittersErrorTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UNiagaraSystem* System = NewTransientCanonicalTestAsset<UNiagaraSystem>(TEXT("NS_ValidateStrict"), ObjectPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    if (!System)
    {
        return false;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), ObjectPath);
    Payload->SetStringField(TEXT("level"), TEXT("strict"));

    TestTrue(TEXT("niagara.validate handler found"), InvokeHandlerWithCapture(TEXT("niagara.validate"), Payload, Capture));
    TestTrue(TEXT("niagara.validate sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.validate succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("niagara.validate result present"), Capture.Result.Get());
    if (Capture.Result.IsValid())
    {
        TestFalse(TEXT("strict validation fails on no emitters"), Capture.Result->GetBoolField(TEXT("valid")));
        TestTrue(TEXT("errors contain NO_EMITTERS"), JsonArrayContainsIssueCode(Capture.Result, TEXT("errors"), TEXT("NO_EMITTERS")));
    }

    System->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraValidateUnsupportedAssetStructuredErrorTest,
    "PinWright.niagara.validate.UnsupportedAssetStructuredError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateUnsupportedAssetStructuredErrorTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UCurveFloat* Asset = NewTransientCanonicalTestAsset<UCurveFloat>(TEXT("Curve_ValidateUnsupported"), ObjectPath);
    TestNotNull(TEXT("Transient unsupported asset created"), Asset);
    if (!Asset)
    {
        return false;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    TestTrue(TEXT("niagara.validate handler found"), InvokeHandlerWithCapture(TEXT("niagara.validate"), Payload, Capture));
    TestTrue(TEXT("niagara.validate sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.validate succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("niagara.validate result present"), Capture.Result.Get());
    if (Capture.Result.IsValid())
    {
        TestFalse(TEXT("unsupported asset is invalid"), Capture.Result->GetBoolField(TEXT("valid")));
        TestEqual(TEXT("asset kind"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("Unsupported")));
        TestTrue(TEXT("errors contain UNSUPPORTED_ASSET"), JsonArrayContainsIssueCode(Capture.Result, TEXT("errors"), TEXT("UNSUPPORTED_ASSET")));
    }

    Asset->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraAddEmitterAddsSystemHandleTest,
    "PinWright.niagara.add_emitter.AddsSystemHandle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraAddEmitterAddsSystemHandleTest::RunTest(const FString& Parameters)
{
    FString SystemPath;
    UNiagaraSystem* System = NewTransientCanonicalTestAsset<UNiagaraSystem>(TEXT("NS_AddEmitter"), SystemPath);
    FString EmitterPath;
    UNiagaraEmitter* Emitter = NewTransientCanonicalTestAsset<UNiagaraEmitter>(TEXT("NE_AddEmitter"), EmitterPath);
    TestNotNull(TEXT("Transient Niagara system created"), System);
    TestNotNull(TEXT("Transient Niagara emitter created"), Emitter);
    if (!System || !Emitter)
    {
        if (System)
        {
            System->RemoveFromRoot();
        }
        if (Emitter)
        {
            Emitter->RemoveFromRoot();
        }
        return false;
    }

    EnsureEmitterGraphSource(Emitter);
    TestEqual(TEXT("initial emitter handle count"), System->GetEmitterHandles().Num(), 0);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemPath"), SystemPath);
    Payload->SetStringField(TEXT("emitterPath"), EmitterPath);
    Payload->SetStringField(TEXT("name"), TEXT("BridgeEmitter"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    TestTrue(TEXT("niagara.add_emitter handler found"), InvokeHandlerWithCapture(TEXT("niagara.add_emitter"), Payload, Capture));
    TestTrue(TEXT("niagara.add_emitter sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("niagara.add_emitter succeeded"), Capture.bSuccess);
    TestEqual(TEXT("system emitter handle count after add"), System->GetEmitterHandles().Num(), 1);

    if (System->GetEmitterHandles().Num() == 1)
    {
        TestEqual(TEXT("new emitter handle name"), System->GetEmitterHandles()[0].GetName().ToString(), FString(TEXT("BridgeEmitter")));
    }
    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("result operation"), Capture.Result->GetStringField(TEXT("operation")), FString(TEXT("add_emitter")));
        TestEqual(TEXT("result emitter count"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("emitterCount"))), 1);
        TestFalse(TEXT("compile was not requested"), Capture.Result->GetBoolField(TEXT("compileRequested")));
        TestFalse(TEXT("save was not requested"), Capture.Result->GetBoolField(TEXT("saveRequested")));
    }

    System->RemoveFromRoot();
    Emitter->RemoveFromRoot();
    return true;
}
