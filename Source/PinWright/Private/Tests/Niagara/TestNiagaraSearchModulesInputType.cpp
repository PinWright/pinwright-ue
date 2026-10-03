// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-search-inputtype-ignored: niagara.search_modules declared
// `inputType` but never read it, so a float request and a vector request returned the same rows.
//
// Counterfactual: stop applying the inputType filter in the handler and the float run returns the
// whole unfiltered set (totalMatches equals the baseline, vector rows included), failing the
// "narrower than the baseline" and per-row type assertions below.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "NiagaraTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace TestNiagaraSearchModulesInputTypeHelpers
{
    struct FSearchRun
    {
        bool bSuccess = false;
        int32 TotalMatches = 0;
        TArray<FString> AssetPaths;
        TArray<FString> OutputTypes; // one per row; empty string when the row carried no output
    };

    FSearchRun Run(FAutomationTestBase& Test, const TCHAR* InputType)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("usage"), TEXT("DynamicInput"));
        Payload->SetStringField(TEXT("sourceFilter"), TEXT("engine"));
        // A narrow stock family keeps the per-candidate loads small: UniformRangedFloat,
        // UniformRangedVector, UniformRangedInt, ... ship with the Niagara plugin on 5.3-5.8.
        Payload->SetStringField(TEXT("query"), TEXT("UniformRanged"));
        Payload->SetNumberField(TEXT("limit"), 500);
        if (InputType)
        {
            Payload->SetStringField(TEXT("inputType"), InputType);
        }

        FSearchRun Out;
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("niagara.search_modules handler found"),
            InvokeHandlerWithCapture(TEXT("niagara.search_modules"), Payload, Capture));
        Out.bSuccess = Capture.bSuccess && Capture.Result.IsValid();
        if (!Out.bSuccess)
        {
            Test.AddError(FString::Printf(TEXT("search_modules(inputType=%s) failed: %s %s"),
                InputType ? InputType : TEXT("<none>"), *Capture.ErrorCode, *Capture.Message));
            return Out;
        }

        double Total = 0.0;
        Capture.Result->TryGetNumberField(TEXT("totalMatches"), Total);
        Out.TotalMatches = static_cast<int32>(Total);
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Results)
            {
                const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!Row.IsValid())
                {
                    continue;
                }
                Out.AssetPaths.Add(Row->GetStringField(TEXT("assetPath")));
                FString OutputType;
                const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
                if (Row->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs && Outputs->Num() == 1)
                {
                    const TSharedPtr<FJsonObject> Output = (*Outputs)[0]->AsObject();
                    if (Output.IsValid())
                    {
                        Output->TryGetStringField(TEXT("type"), OutputType);
                    }
                }
                Out.OutputTypes.Add(OutputType);
            }
        }
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesInputTypeFiltersTest,
    "PinWright.niagara.search_modules.InputTypeFiltersByOutputType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesInputTypeFiltersTest::RunTest(const FString& Parameters)
{
    using namespace TestNiagaraSearchModulesInputTypeHelpers;

    const FSearchRun Baseline = Run(*this, nullptr);
    if (!Baseline.bSuccess)
    {
        return false;
    }
    if (Baseline.TotalMatches < 2)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara_fixture_assets_absent"),
            FString::Printf(TEXT("stock UniformRanged* dynamic inputs not found (totalMatches=%d)"), Baseline.TotalMatches));
        return true;
    }
    for (const FString& Type : Baseline.OutputTypes)
    {
        TestTrue(TEXT("without inputType no row carries an output (nothing was loaded)"), Type.IsEmpty());
    }

    const FSearchRun Floats = Run(*this, TEXT("float"));
    const FSearchRun Vectors = Run(*this, TEXT("vector"));
    if (!Floats.bSuccess || !Vectors.bSuccess)
    {
        return false;
    }

    TestTrue(TEXT("float filter finds at least one dynamic input"), Floats.TotalMatches > 0);
    TestTrue(TEXT("vector filter finds at least one dynamic input"), Vectors.TotalMatches > 0);
    TestTrue(FString::Printf(TEXT("float filter narrows the baseline (%d < %d)"), Floats.TotalMatches, Baseline.TotalMatches),
        Floats.TotalMatches < Baseline.TotalMatches);
    TestTrue(FString::Printf(TEXT("vector filter narrows the baseline (%d < %d)"), Vectors.TotalMatches, Baseline.TotalMatches),
        Vectors.TotalMatches < Baseline.TotalMatches);

    const FString FloatName = FNiagaraTypeDefinition::GetFloatDef().GetName();
    for (int32 Index = 0; Index < Floats.OutputTypes.Num(); ++Index)
    {
        TestEqual(FString::Printf(TEXT("float row %s output type"), *Floats.AssetPaths[Index]),
            Floats.OutputTypes[Index], FloatName);
        TestFalse(FString::Printf(TEXT("float row %s is not also a vector row"), *Floats.AssetPaths[Index]),
            Vectors.AssetPaths.Contains(Floats.AssetPaths[Index]));
    }
    // The stack's assignability rule lets Position and Vector stand in for each other.
    const FString VectorName = FNiagaraTypeDefinition::GetVec3Def().GetName();
    const FString PositionName = FNiagaraTypeDefinition::GetPositionDef().GetName();
    for (int32 Index = 0; Index < Vectors.OutputTypes.Num(); ++Index)
    {
        const FString& Type = Vectors.OutputTypes[Index];
        TestTrue(FString::Printf(TEXT("vector row %s output type '%s' is Vector or Position"), *Vectors.AssetPaths[Index], *Type),
            Type == VectorName || Type == PositionName);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesInputTypeRefusalsTest,
    "PinWright.niagara.search_modules.InputTypeRefusals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesInputTypeRefusalsTest::RunTest(const FString& Parameters)
{
    struct FCase { const TCHAR* Usage; const TCHAR* InputType; const TCHAR* ExpectedCode; };
    const FCase Cases[] = {
        // inputType describes a Dynamic Input's output; on any other usage it cannot apply.
        { TEXT("Module"),       TEXT("float"),             TEXT("INVALID_ARGUMENT") },
        { TEXT("DynamicInput"), TEXT("NotANiagaraType_X"), TEXT("INVALID_PARAMETER_TYPE") },
    };
    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("usage"), Case.Usage);
        Payload->SetStringField(TEXT("inputType"), Case.InputType);
        Payload->SetNumberField(TEXT("limit"), 1);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.search_modules"), Payload, Capture);
        const FString Label = FString::Printf(TEXT("usage=%s inputType=%s"), Case.Usage, Case.InputType);
        TestTrue(*FString::Printf(TEXT("%s responded"), *Label), Capture.bWasCalled);
        TestFalse(*FString::Printf(TEXT("%s is refused"), *Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s error code"), *Label), Capture.ErrorCode, FString(Case.ExpectedCode));
    }
    return true;
}
