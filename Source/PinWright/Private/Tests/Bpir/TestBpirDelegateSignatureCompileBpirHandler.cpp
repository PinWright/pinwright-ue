// Copyright (c) 2026 Alexander Penkin. MIT License.

// Drives blueprint.compile_bpir through the dispatcher (the same entrypoint MCP uses)
// to assert that delegate / mcdelegate function-entry parameters compile into
// FDelegateProperty / FMulticastDelegateProperty with non-null SignatureFunction.
// The compiler-only path in TestBpirDelegateSignatureFunctionParams skipped the
// widget-BP pre-compile branch + JSON payload plumbing that the real verifier path
// exercises, so this test closes the gap that let prior fixes pass their own tests
// while still failing MCP verification.
//
// Counterfactual: UHT strips the leading `F` from dynamic delegate type names —
// `DECLARE_DYNAMIC_DELEGATE_RetVal(bool, FGetBool)` declared inside `class UWidget`
// registers a UFunction named `GetBool__DelegateSignature` outered to `UWidget`, not
// `FGetBool__DelegateSignature` outered to `/Script/UMG`. If `ResolveDelegateSignatureFunction`
// stops F-stripping the input or stops walking the OwnerClass super chain via
// `FindObject<UFunction>(Class, "GetBool__DelegateSignature")`, both `FGetBool__DelegateSignature`
// and `FOnButtonClickedEvent__DelegateSignature` resolve to null. `ConvertTypeSpecToPinType`
// returns false, `BpirCompiler::SetupFunction` accumulates the structured 'Could not
// resolve delegate signature' error, the dispatcher returns `success=false` with that
// message in `errors[]`, and this test fails on
// `TestTrue("blueprint.compile_bpir reported success", Capture.bSuccess)` and the
// subsequent `TestNotNull` assertions on `Handler->SignatureFunction.Get()` and
// `MultiHandler->SignatureFunction.Get()`.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Button.h"
#include "Components/Widget.h"
#include "Dispatch/RpcDispatcher.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"


namespace
{
struct FDispatcherCapture
{
    bool bCompletionFired = false;
    bool bSuccess = false;
    FString Message;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
};

FDispatcherCapture DispatchCompileBpirViaDispatcher(
    const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload)
{
    FDispatcherCapture Capture;
    FRpcDispatcher Dispatcher;
    // Initialize BEFORE draining so the bridge lambdas capture a live sink.
    Dispatcher.Initialize(FResponseSink(
        [&Capture]
        (const FString&, bool bInSuccess, const FString& InMessage,
         const TSharedPtr<FJsonObject>& InResult, const FString& InErrorCode)
        {
            Capture.bCompletionFired = true;
            Capture.bSuccess = bInSuccess;
            Capture.Message = InMessage;
            Capture.Result = InResult;
            Capture.ErrorCode = InErrorCode;
        }));
    Dispatcher.DrainAutoRegistrations(nullptr);

    Dispatcher.ProcessRequest(RequestId, TEXT("blueprint.compile_bpir"),
        Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
    return Capture;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDelegateSignatureCompileBpirHandlerTest,
    "PinWright.bpir.handler.DelegateSignatureCompileBpirHandler",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDelegateSignatureCompileBpirHandlerTest::RunTest(const FString& Parameters)
{
    // On-disk path required: LoadBlueprintAsset (called by the handler) cannot see
    // unregistered transient-package assets.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/W_BpirDelegateSigTest_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));

    if (!TestNotNull(TEXT("Widget blueprint was created"), WBP))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }

    FAssetRegistryModule::AssetCreated(WBP);
    Pkg->MarkPackageDirty();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    Payload->SetStringField(TEXT("code"),
        TEXT("entry function VerifyDelegates(delegate<Widget, FGetBool__DelegateSignature> Handler, mcdelegate<Button, FOnButtonClickedEvent__DelegateSignature> MultiHandler) { }"));

    const FDispatcherCapture Capture = DispatchCompileBpirViaDispatcher(
        TEXT("req-bpir-delegate-sig"), Payload);

    TestTrue(TEXT("dispatcher completion fired"), Capture.bCompletionFired);
    TestTrue(TEXT("blueprint.compile_bpir reported success"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(
            TEXT("compile_bpir error: code=%s message=%s"), *Capture.ErrorCode, *Capture.Message));
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return false;
    }

    if (!TestTrue(TEXT("response result payload present"), Capture.Result.IsValid()))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return false;
    }

    bool bResultSuccess = false;
    const bool bHasSuccess = Capture.Result->TryGetBoolField(TEXT("success"), bResultSuccess);
    TestTrue(TEXT("result has 'success' field"), bHasSuccess);
    TestTrue(TEXT("result.success is true"), bResultSuccess);

    bool bResultCompiled = false;
    const bool bHasCompiled = Capture.Result->TryGetBoolField(TEXT("compiled"), bResultCompiled);
    TestTrue(TEXT("result has 'compiled' field"), bHasCompiled);
    TestTrue(TEXT("result.compiled is true"), bResultCompiled);

    const TArray<TSharedPtr<FJsonValue>>* ErrorsArr = nullptr;
    if (Capture.Result->TryGetArrayField(TEXT("errors"), ErrorsArr) && ErrorsArr)
    {
        TestEqual(TEXT("result.errors is empty"), ErrorsArr->Num(), 0);
        for (const TSharedPtr<FJsonValue>& ErrVal : *ErrorsArr)
        {
            const TSharedPtr<FJsonObject>* ErrObj = nullptr;
            if (ErrVal.IsValid() && ErrVal->TryGetObject(ErrObj) && ErrObj && ErrObj->IsValid())
            {
                FString ErrMessage;
                (*ErrObj)->TryGetStringField(TEXT("message"), ErrMessage);
                AddError(FString::Printf(TEXT("compile_bpir reported error: %s"), *ErrMessage));
            }
        }
    }

    UClass* GeneratedClass = WBP->GeneratedClass.Get();
    if (!TestNotNull(TEXT("WBP GeneratedClass exists after compile"), GeneratedClass))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return false;
    }

    UFunction* GeneratedFunction = GeneratedClass->FindFunctionByName(
        FName(TEXT("VerifyDelegates")), EIncludeSuperFlag::ExcludeSuper);
    if (!TestNotNull(TEXT("VerifyDelegates UFunction exists on GeneratedClass"), GeneratedFunction))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return false;
    }

    FDelegateProperty* HandlerProperty = CastField<FDelegateProperty>(
        GeneratedFunction->FindPropertyByName(FName(TEXT("Handler"))));
    FMulticastDelegateProperty* MultiHandlerProperty = CastField<FMulticastDelegateProperty>(
        GeneratedFunction->FindPropertyByName(FName(TEXT("MultiHandler"))));
    TestNotNull(TEXT("Handler created as FDelegateProperty"), HandlerProperty);
    TestNotNull(TEXT("MultiHandler created as FMulticastDelegateProperty"), MultiHandlerProperty);

    if (HandlerProperty)
    {
        TestNotNull(TEXT("Handler->SignatureFunction is non-null"),
            HandlerProperty->SignatureFunction.Get());
    }
    if (MultiHandlerProperty)
    {
        TestNotNull(TEXT("MultiHandler->SignatureFunction is non-null"),
            MultiHandlerProperty->SignatureFunction.Get());
    }

    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    return true;
}
