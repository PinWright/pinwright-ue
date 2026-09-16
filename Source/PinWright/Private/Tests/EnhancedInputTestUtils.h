// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Misc/AutomationTest.h"
#include "Compiler/CodeNodeEmitter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "InputAction.h"
#include "Misc/PackageName.h"
#include "UObject/NoExportTypes.h"
#include "UObject/Package.h"

namespace EnhancedInputTestUtils
{
    inline UInputAction* CreateAxis2DInputAction(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }

        UInputAction* Action = NewObject<UInputAction>(
            Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone);
        if (Action)
        {
            Action->ValueType = EInputActionValueType::Axis2D;
            FAssetRegistryModule::AssetCreated(Action);
        }
        return Action;
    }

    inline UEdGraphNode* FindEnhancedInputActionNode(UBlueprint* Blueprint)
    {
        UClass* NodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
        if (!Blueprint || !NodeClass)
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->IsA(NodeClass))
                {
                    return Node;
                }
            }
        }
        return nullptr;
    }

    inline int32 CountEnhancedInputActionNodes(
        UBlueprint* Blueprint,
        const UInputAction* ExpectedAction = nullptr)
    {
        UClass* NodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
        if (!Blueprint || !NodeClass)
        {
            return 0;
        }

        int32 Count = 0;
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->IsA(NodeClass)
                    && (!ExpectedAction || FCodeNodeEmitter::GetEnhancedInputAction(Node) == ExpectedAction))
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    inline void TestAxis2DNodeShape(
        FAutomationTestBase& Test,
        UEdGraphNode* Node,
        UInputAction* ExpectedAction)
    {
        Test.TestNotNull(TEXT("Enhanced Input node exists"), Node);
        if (!Node)
        {
            return;
        }

        Test.TestTrue(TEXT("Reflected InputAction binding matches fixture"),
            FCodeNodeEmitter::GetEnhancedInputAction(Node) == ExpectedAction);

        UEdGraphPin* ActionValue = Node->FindPin(TEXT("ActionValue"), EGPD_Output);
        Test.TestNotNull(TEXT("ActionValue output exists"), ActionValue);
        if (ActionValue)
        {
            Test.TestEqual(TEXT("ActionValue is a struct pin"),
                ActionValue->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
            Test.TestTrue(TEXT("Axis2D ActionValue is FVector2D"),
                ActionValue->PinType.PinSubCategoryObject.Get() == TBaseStructure<FVector2D>::Get());
        }

        UEdGraphPin* InputActionPin = Node->FindPin(TEXT("InputAction"), EGPD_Output);
        Test.TestNotNull(TEXT("InputAction output exists"), InputActionPin);
        if (InputActionPin)
        {
            Test.TestTrue(TEXT("InputAction output points to fixture"),
                InputActionPin->DefaultObject == ExpectedAction);
        }
    }
}
