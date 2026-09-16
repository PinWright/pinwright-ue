// Copyright (c) 2026 Alexander Penkin. MIT License.

// Round-trip guard for derived material pin names: a name that a discovery / inspection verb
// REPORTS must be a name that connect_nodes ACCEPTS. Reporting one vocabulary and resolving
// another is worse than reporting "None" — the caller believes the wire landed on the pin it
// named while ApplyConnection silently fell through to output 0.
// Derivation and the engine citations behind it: Material/MaterialPinNames.h.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "MaterialExpressionIO.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialDerivedSourcePinRoundTripTest,
    "PinWright.material.graph.connect_nodes.DerivedSourcePinRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialDerivedSourcePinRoundTripTest::RunTest(const FString& Parameters)
{
    // --- The names under test come from the verb, not from a literal in this file, so the
    //     assertion is genuinely "what we report is what we accept". ---
    TArray<FString> ReportedPins;
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("VertexColor"));
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.search_expression_types"), Payload, Capture);
        TestTrue(TEXT("search_expression_types found"), bInvoked);
        if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        Capture.Result->TryGetArrayField(TEXT("results"), Results);
        if (!Results)
        {
            AddError(TEXT("search_expression_types returned no results array"));
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Val : *Results)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Val->TryGetObject(Obj) || !Obj) continue;
            FString ClassName;
            (*Obj)->TryGetStringField(TEXT("className"), ClassName);
            if (ClassName != TEXT("MaterialExpressionVertexColor")) continue;

            const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
            (*Obj)->TryGetArrayField(TEXT("outputPins"), Pins);
            if (Pins)
            {
                for (const TSharedPtr<FJsonValue>& Pin : *Pins)
                {
                    ReportedPins.Add(Pin->AsString());
                }
            }
            break;
        }
    }

    if (!TestEqual(TEXT("VertexColor reports 5 output pins"), ReportedPins.Num(), 5))
    {
        return true;
    }

    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/DerivedSourcePin_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg)) return true;

    UMaterial* Material = NewObject<UMaterial>(
        Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UMaterialExpressionVertexColor* VertexColor = NewObject<UMaterialExpressionVertexColor>(Material);
    if (!TestNotNull(TEXT("VertexColor created"), VertexColor))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    if (!VertexColor->MaterialExpressionGuid.IsValid())
    {
        VertexColor->MaterialExpressionGuid = FGuid::NewGuid();
    }
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(VertexColor);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    const FString SourceGuid = VertexColor->MaterialExpressionGuid.ToString();

    // Wire each reported pin into main BaseColor by NAME ONLY (no sourceOutputIndex), and assert
    // the resolved OutputIndex is that pin's position. Pre-fix, every name was "None" and every
    // one of these fell through to the OutputIndex = 0 default.
    for (int32 Index = 0; Index < ReportedPins.Num(); ++Index)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
        Payload->SetStringField(TEXT("sourcePin"), ReportedPins[Index]);

        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("material.graph.connect_nodes found"), bInvoked);
        TestTrue(*FString::Printf(TEXT("connect_nodes succeeded for sourcePin '%s'"),
            *ReportedPins[Index]), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("sourcePin '%s' resolves to output index %d"),
            *ReportedPins[Index], Index),
            Material->GetEditorOnlyData()->BaseColor.OutputIndex, Index);
    }

    // Lowercase spellings resolve too — the match is documented case-insensitive. Deliberately
    // index 1, not the last one: the loop above left OutputIndex on the LAST index, so asserting
    // the last index again would pass even if nothing resolved.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
        Payload->SetStringField(TEXT("sourcePin"), ReportedPins[1].ToLower());

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("lowercase sourcePin succeeded"), Capture.bSuccess);
        TestEqual(TEXT("lowercase sourcePin resolves to index 1"),
            Material->GetEditorOnlyData()->BaseColor.OutputIndex, 1);
    }

    // Control: a name that is NOT in the reported vocabulary must fall back to the documented
    // sourceOutputIndex default of 0 rather than sticking on the previous index. Without this,
    // the assertions above would also pass on a resolver that ignored sourcePin entirely and
    // happened to leave OutputIndex where the loop left it.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceGuid);
        Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
        Payload->SetStringField(TEXT("sourcePin"), TEXT("NotAPinNameOnThisNode"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("unknown sourcePin still succeeds (documented index fallback)"), Capture.bSuccess);
        TestEqual(TEXT("unknown sourcePin falls back to output index 0"),
            Material->GetEditorOnlyData()->BaseColor.OutputIndex, 0);
    }

    CleanupTestAsset(AssetPath);
    return true;
}

// Input side of the same contract. UMaterialExpressionCustom::GetInputName is NAME_None for its
// single input, so get_node_details used to report inputs[0].name == "None" and the only string
// that resolved was the literal "None". The derived stem ("Input", after
// UMaterialGraphNode::CreateInputPins' CreateUniquePinName fallback) must be both reported and
// accepted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialDerivedInputPinRoundTripTest,
    "PinWright.material.graph.connect_nodes.DerivedInputPinRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialDerivedInputPinRoundTripTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/DerivedInputPin_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg)) return true;

    UMaterial* Material = NewObject<UMaterial>(
        Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UMaterialExpressionVertexColor* VertexColor = NewObject<UMaterialExpressionVertexColor>(Material);
    UMaterialExpressionCustom* CustomExpr = NewObject<UMaterialExpressionCustom>(Material);
    if (!TestNotNull(TEXT("VertexColor created"), VertexColor) ||
        !TestNotNull(TEXT("Custom created"), CustomExpr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    if (!VertexColor->MaterialExpressionGuid.IsValid()) VertexColor->MaterialExpressionGuid = FGuid::NewGuid();
    if (!CustomExpr->MaterialExpressionGuid.IsValid())  CustomExpr->MaterialExpressionGuid  = FGuid::NewGuid();

    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(VertexColor);
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(CustomExpr);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // The engine leaves this input unnamed — the precondition the whole test rests on.
    TestTrue(TEXT("Custom input 0 is unnamed in the engine"),
        CustomExpr->GetInputName(0).IsNone());

    // get_node_details reports the derived stem, not "None".
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("nodeId"), CustomExpr->MaterialExpressionGuid.ToString());

        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.get_node_details"), Payload, Capture);
        TestTrue(TEXT("get_node_details found"), bInvoked);
        TestTrue(TEXT("get_node_details succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
            Capture.Result->TryGetArrayField(TEXT("inputs"), Inputs);
            if (Inputs && Inputs->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                (*Inputs)[0]->TryGetObject(First);
                FString Name;
                if (First) (*First)->TryGetStringField(TEXT("name"), Name);
                TestEqual(TEXT("Custom inputs[0].name"), Name, FString(TEXT("Input")));
            }
            else
            {
                AddError(TEXT("get_node_details returned no inputs for Custom"));
            }

            // Every output entry carries its own index, so a caller never counts positions.
            const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
            Capture.Result->TryGetArrayField(TEXT("outputs"), Outputs);
            if (Outputs && Outputs->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* FirstOut = nullptr;
                (*Outputs)[0]->TryGetObject(FirstOut);
                double OutIndex = -1.0;
                if (FirstOut) (*FirstOut)->TryGetNumberField(TEXT("index"), OutIndex);
                TestEqual(TEXT("outputs[0].index"), static_cast<int32>(OutIndex), 0);
            }
        }
    }

    // ...and the same string is accepted as inputName.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("sourceNodeId"), VertexColor->MaterialExpressionGuid.ToString());
        Payload->SetStringField(TEXT("targetNodeId"), CustomExpr->MaterialExpressionGuid.ToString());
        Payload->SetStringField(TEXT("inputName"), TEXT("Input"));
        Payload->SetStringField(TEXT("sourcePin"), TEXT("B"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("material.graph.connect_nodes"), Payload, Capture);
        TestTrue(TEXT("connect_nodes accepted the derived inputName"), Capture.bSuccess);

        FExpressionInput* Wired = CustomExpr->GetInput(0);
        if (TestNotNull(TEXT("Custom input 0 reachable"), Wired))
        {
            TestTrue(TEXT("Custom input 0 wired to VertexColor"),
                Wired->Expression == static_cast<UMaterialExpression*>(VertexColor));
            // VertexColor's outputs are RGB, R, G, B, A — "B" is index 3.
            TestEqual(TEXT("sourcePin 'B' resolved to output index 3"), Wired->OutputIndex, 3);
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}
