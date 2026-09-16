// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PinWrightSubsystem.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    FString MakeUniqueDispatcherTestPath()
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BP_AddDispatcher_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddDispatcherCreatesSignatureGraphTest,
    "PinWright.blueprint.add_dispatcher.CreatesSignatureGraphAndProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddDispatcherCreatesSignatureGraphTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueDispatcherTestPath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UPackage* Package = CreatePackage(*AssetPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("name"), TEXT("OnFoo"));

    TArray<TSharedPtr<FJsonValue>> Params;
    TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
    Param->SetStringField(TEXT("name"), TEXT("bEnabled"));
    Param->SetStringField(TEXT("type"), TEXT("bool"));
    Params.Add(MakeShared<FJsonValueObject>(Param));
    Payload->SetArrayField(TEXT("params"), Params);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.add_dispatcher handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_dispatcher"), Payload, Capture));
    TestTrue(TEXT("blueprint.add_dispatcher responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("blueprint.add_dispatcher succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_dispatcher error: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    const FBPVariableDescription* DispatcherVariable = nullptr;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == FName(TEXT("OnFoo")))
        {
            DispatcherVariable = &Variable;
            break;
        }
    }
    if (TestNotNull(TEXT("NewVariables contains OnFoo"), DispatcherVariable))
    {
        TestEqual(TEXT("OnFoo is an MCDelegate variable"),
            DispatcherVariable->VarType.PinCategory,
            UEdGraphSchema_K2::PC_MCDelegate);
        TestTrue(TEXT("OnFoo is BlueprintAssignable"),
            (DispatcherVariable->PropertyFlags & CPF_BlueprintAssignable) != 0);
        TestTrue(TEXT("OnFoo is BlueprintCallable"),
            (DispatcherVariable->PropertyFlags & CPF_BlueprintCallable) != 0);
    }

    UEdGraph* SignatureGraph = nullptr;
    bool bFoundWrongNamedGraph = false;
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        if (Graph->GetName() == TEXT("OnFoo"))
        {
            SignatureGraph = Graph;
        }
        if (Graph->GetName() == TEXT("OnFoo__DelegateSignature"))
        {
            bFoundWrongNamedGraph = true;
        }
    }
    TestNotNull(TEXT("DelegateSignatureGraphs contains graph named OnFoo"), SignatureGraph);
    TestFalse(TEXT("DelegateSignatureGraphs does not contain OnFoo__DelegateSignature graph"), bFoundWrongNamedGraph);

    UK2Node_FunctionEntry* EntryNode = nullptr;
    if (SignatureGraph)
    {
        for (UEdGraphNode* Node : SignatureGraph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryNode = Entry;
                break;
            }
        }
    }
    if (TestNotNull(TEXT("signature graph has function entry"), EntryNode))
    {
        TestNotNull(TEXT("entry has requested param pin"),
            EntryNode->FindPin(TEXT("bEnabled"), EGPD_Output));
    }

    UClass* GeneratedClass = Blueprint->GeneratedClass.Get();
    if (TestNotNull(TEXT("generated class exists"), GeneratedClass))
    {
        FMulticastDelegateProperty* DelegateProperty =
            FindFProperty<FMulticastDelegateProperty>(GeneratedClass, TEXT("OnFoo"));
        if (TestNotNull(TEXT("GeneratedClass has FMulticastDelegateProperty OnFoo"), DelegateProperty))
        {
            TestTrue(TEXT("generated OnFoo is BlueprintAssignable"),
                DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable));
            TestTrue(TEXT("generated OnFoo is BlueprintCallable"),
                DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintCallable));
            TestNotNull(TEXT("OnFoo property has SignatureFunction"),
                DelegateProperty->SignatureFunction.Get());
        }

        UFunction* SignatureFunction =
            GeneratedClass->FindFunctionByName(TEXT("OnFoo__DelegateSignature"));
        if (TestNotNull(TEXT("GeneratedClass has OnFoo__DelegateSignature"), SignatureFunction))
        {
            FBoolProperty* ParamProperty = FindFProperty<FBoolProperty>(SignatureFunction, TEXT("bEnabled"));
            if (TestNotNull(TEXT("signature function has bEnabled bool param"), ParamProperty))
            {
                TestTrue(TEXT("bEnabled is a function param"),
                    ParamProperty->HasAnyPropertyFlags(CPF_Parm));
            }
        }
    }

    if (Capture.Result.IsValid())
    {
        FString ResponseGraph;
        Capture.Result->TryGetStringField(TEXT("signatureGraph"), ResponseGraph);
        TestEqual(TEXT("response signatureGraph is OnFoo"), ResponseGraph, FString(TEXT("OnFoo")));

        FString ResponseFunction;
        Capture.Result->TryGetStringField(TEXT("signatureFunction"), ResponseFunction);
        TestEqual(TEXT("response signatureFunction is generated function name"),
            ResponseFunction,
            FString(TEXT("OnFoo__DelegateSignature")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddDispatcherAcceptsBlueprintPathOnlyTest,
    "PinWright.blueprint.add_dispatcher.AcceptsBlueprintPathOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddDispatcherAcceptsBlueprintPathOnlyTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueDispatcherTestPath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UPackage* Package = CreatePackage(*AssetPath);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), AssetPath);
    Payload->SetStringField(TEXT("name"), TEXT("OnBlueprintPathOnly"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.add_dispatcher handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_dispatcher"), Payload, Capture));
    TestTrue(TEXT("blueprint.add_dispatcher responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("blueprint.add_dispatcher succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_dispatcher error: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    const FBPVariableDescription* DispatcherVariable = nullptr;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == FName(TEXT("OnBlueprintPathOnly")))
        {
            DispatcherVariable = &Variable;
            break;
        }
    }
    if (TestNotNull(TEXT("NewVariables contains OnBlueprintPathOnly"), DispatcherVariable))
    {
        TestEqual(TEXT("OnBlueprintPathOnly is an MCDelegate variable"),
            DispatcherVariable->VarType.PinCategory,
            UEdGraphSchema_K2::PC_MCDelegate);
    }

    UEdGraph* SignatureGraph = nullptr;
    for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
    {
        if (Graph && Graph->GetName() == TEXT("OnBlueprintPathOnly"))
        {
            SignatureGraph = Graph;
            break;
        }
    }
    TestNotNull(TEXT("DelegateSignatureGraphs contains OnBlueprintPathOnly"), SignatureGraph);

    return true;
}

// Regression guard for B-ensure-exists-create-missing-name: blueprint.ensure_exists'
// auto-create branch must actually create the missing asset. It cross-dispatches to
// blueprint.create through the live subsystem (Ctx.GetSubsystem()->DispatchMethod).
// Pre-fix, ensure_exists built the create payload with a `blueprintPath` field and no
// `name`/`savePath`, so blueprint.create's required-`name` validation rejected it with
// MISSING_REQUIRED_PARAM and nothing was created — the method's primary purpose was
// dead. The subsystem-less context that InvokeHandler builds can't cross-dispatch, so
// this test drives the handler through the real UPinWrightSubsystem
// (GEditor->GetEditorSubsystem) and asserts the asset now exists. Reverting the payload
// split (name/savePath back to blueprintPath) makes the create fail its required-param
// check, so DoesAssetExist stays false and this test fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintEnsureExistsAutoCreatesMissingTest,
    "PinWright.blueprint.ensure_exists.AutoCreatesMissingAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintEnsureExistsAutoCreatesMissingTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem =
        GEditor ? GEditor->GetEditorSubsystem<UPinWrightSubsystem>() : nullptr;
    if (!Subsystem)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-live-subsystem"),
            TEXT("No PinWright subsystem (commandlet context); ensure_exists "
                "cross-dispatch to blueprint.create has no live subsystem to create "
                "through. Skipping the live auto-create assertion."));
        return true;
    }

    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BP_EnsureExists_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    // Precondition: the target must not exist, so ensure_exists takes its create branch.
    TestFalse(TEXT("target asset does not exist before ensure_exists"),
        UEditorAssetLibrary::DoesAssetExist(AssetPath));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("parentClass"), TEXT("Actor"));

    // Drive through the live subsystem so ensure_exists' cross-dispatch to
    // blueprint.create carries the real subsystem in its handler context.
    TestTrue(TEXT("blueprint.ensure_exists dispatched"),
        Subsystem->DispatchMethod(TEXT("blueprint.ensure_exists"), TEXT("test-id"), Payload));

    // The core assertion: the missing asset must now exist. Pre-fix, the create
    // dispatch failed its required-`name` validation and created nothing.
    TestTrue(TEXT("blueprint.ensure_exists auto-created the missing asset"),
        UEditorAssetLibrary::DoesAssetExist(AssetPath));

    return true;
}
