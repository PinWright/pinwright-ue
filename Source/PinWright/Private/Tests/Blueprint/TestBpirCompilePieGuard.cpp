// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for the BPIR PIE preflight. The compile and insert handlers
// must refuse before loading or mutating a Blueprint, even when allowReinstancing is enabled.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Handlers/ErrorCodes.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Templates/UnrealTemplate.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
    FString MakePieGuardTestAssetPath()
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BpirCompilePieGuard_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    int32 CountBlueprintGraphNodes(UBlueprint* Blueprint)
    {
        if (!Blueprint)
        {
            return 0;
        }

        int32 NodeCount = 0;
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph)
            {
                NodeCount += Graph->Nodes.Num();
            }
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                NodeCount += Graph->Nodes.Num();
            }
        }
        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph)
            {
                NodeCount += Graph->Nodes.Num();
            }
        }
        return NodeCount;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompileRefusesDuringPieTest,
    "PinWright.blueprint.compile_bpir.RefusesDuringPie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompileRefusesDuringPieTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakePieGuardTestAssetPath();
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
    };

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }

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

    const int32 InitialNodeCount = CountBlueprintGraphNodes(Blueprint);
    Package->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("mode"), TEXT("append"));
    Payload->SetBoolField(TEXT("allowReinstancing"), true);
    Payload->SetStringField(TEXT("code"),
        TEXT("entry key_pressed Tab() {\n")
        TEXT("    call PrintString(InString: \"during pie\")\n")
        TEXT("}\n"));

    auto AssertPieRefusal =
        [this, Blueprint, Package, InitialNodeCount](
            const TCHAR* Route,
            const TCHAR* Label,
            const TSharedPtr<FJsonObject>& InvocationPayload)
    {
        Package->SetDirtyFlag(false);
        FTestResponseCapture Capture;
        this->TestTrue(Label,
            InvokeHandlerWithCapture(Route, InvocationPayload, Capture));
        this->TestTrue(*FString::Printf(TEXT("%s response was sent"), Route), Capture.bWasCalled);
        this->TestFalse(*FString::Printf(TEXT("%s refused during PIE"), Route), Capture.bSuccess);
        this->TestEqual(*FString::Printf(TEXT("%s returned PIE_ACTIVE"), Route),
            Capture.ErrorCode, FString(ErrorCodes::ERR_PIE_ACTIVE));
        this->TestTrue(*FString::Printf(TEXT("%s explains how to continue"), Route),
            Capture.Message.Contains(TEXT("stop PIE")));
        this->TestEqual(*FString::Printf(TEXT("%s did not mutate Blueprint graph"), Route),
            CountBlueprintGraphNodes(Blueprint), InitialNodeCount);
        this->TestFalse(*FString::Printf(TEXT("%s did not dirty the package"), Route),
            Package->IsDirty());
    };

    TSharedPtr<FJsonObject> InsertAtPayload = MakeShared<FJsonObject>();
    InsertAtPayload->SetStringField(TEXT("assetPath"), AssetPath);
    InsertAtPayload->SetStringField(TEXT("nodeId"), FGuid::NewGuid().ToString());
    InsertAtPayload->SetStringField(TEXT("code"),
        TEXT("call PrintString(InString: \"during pie\")\n"));
    InsertAtPayload->SetBoolField(TEXT("allowReinstancing"), true);

    TSharedPtr<FJsonObject> InsertBeforePayload = MakeShared<FJsonObject>();
    InsertBeforePayload->SetStringField(TEXT("assetPath"), AssetPath);
    InsertBeforePayload->SetStringField(TEXT("nodeId"), FGuid::NewGuid().ToString());
    InsertBeforePayload->SetStringField(TEXT("code"),
        TEXT("call PrintString(InString: \"during pie\")\n"));
    InsertBeforePayload->SetBoolField(TEXT("allowReinstancing"), true);

    {
        // This toggles the engine predicate without starting a real PIE session.
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        AssertPieRefusal(TEXT("blueprint.compile_bpir"), TEXT("compile_bpir handler found"), Payload);
        AssertPieRefusal(TEXT("blueprint.insert_bpir_at_node"),
            TEXT("insert_bpir_at_node handler found"), InsertAtPayload);
        AssertPieRefusal(TEXT("blueprint.insert_bpir_before_node"),
            TEXT("insert_bpir_before_node handler found"), InsertBeforePayload);
    }

    // Counterfactual: reverting the production gate lets this valid invocation proceed
    // past the guard, so the PIE_ACTIVE assertion fails and mutation evidence can differ.
    return true;
}
