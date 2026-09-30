// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board E-material-verbs-have-no-shader-compile-signal, reopen #3/#4:
// material.compile_mgir with waitForShaderCompile:true (and save:true) returned NO `shaderCompile`
// key for a material whose shader compiled cleanly.
//
// ROOT CAUSE. compile_mgir folds every written material into one verdict with
// FState::Accumulate, and a default FState starts at NotMeasured. The fold was worst-status-wins
// only, and NotMeasured outranks Completed and OnDemand, so a document whose materials all compiled
// (or compile on demand) stayed NotMeasured and AddReport published nothing. A failed or still
// compiling material outranks NotMeasured, which is why the block appeared in #4's no-wait run
// (`outstanding`) and vanished in the waited runs (`completed`). `save` was incidental.
//
// COUNTERFACTUAL. With the pre-fix Accumulate every "block is present" assertion on a completed
// or onDemand verdict below fails: FoldPublishesHealthyVerdict deterministically, on any host,
// and the handler tests on the clean-material leg (completed) and the no-wait broken leg (onDemand).

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

namespace
{
TSharedPtr<FJsonObject> MakeMgirPayload(const FString& DocumentText, bool bSave, bool bWait)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), DocumentText);
    Payload->SetStringField(TEXT("mode"), TEXT("Append"));
    Payload->SetBoolField(TEXT("runLayout"), false);
    Payload->SetBoolField(TEXT("save"), bSave);
    Payload->SetBoolField(PinWright::MaterialShaderState::WaitParamName(), bWait);
    return Payload;
}

// Runs compile_mgir and returns its shaderCompile block, or nullptr (with a failed assertion)
// when the graph write itself failed or the block is missing.
const TSharedPtr<FJsonObject>* CompileAndReadBlock(FAutomationTestBase& Test, const FString& Label,
    const FString& DocumentText, bool bSave, bool bWait, FTestResponseCapture& Capture)
{
    Test.TestTrue(*FString::Printf(TEXT("[%s] compile_mgir is registered"), *Label),
        InvokeHandlerWithCapture(TEXT("material.compile_mgir"),
            MakeMgirPayload(DocumentText, bSave, bWait), Capture));
    if (!Test.TestTrue(*FString::Printf(TEXT("[%s] the graph write succeeds (%s: %s)"),
                *Label, *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess && Capture.Result.IsValid()))
    {
        return nullptr;
    }
    const TSharedPtr<FJsonObject>* Block = nullptr;
    Test.TestTrue(*FString::Printf(TEXT("[%s] response carries the shaderCompile block"), *Label),
        Capture.Result->TryGetObjectField(TEXT("shaderCompile"), Block) && Block);
    return Block;
}

FString BrokenHlslDocument(const FString& PackagePath)
{
    // Graph-valid, shader-invalid: LocalToWorld is not subscriptable on current engines, so the
    // platform compiler rejects the generated HLSL while MGIR places and wires every node.
    return FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%c = call `/Script/Engine.MaterialExpressionCustom`(Code: \"return GetPrimitiveData(Parameters).LocalToWorld[2].xyz;\", OutputType: \"CMOT_Float3\") @(-500, 0)\n")
        TEXT("    output EmissiveColor: %%c\n")
        TEXT("}\n"),
        *PackagePath);
}

FString CleanDocument(const FString& PackagePath)
{
    return FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%v = constant Float1(0.5) @(-500, 0)\n")
        TEXT("    output Metallic: %%v\n")
        TEXT("}\n"),
        *PackagePath);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMgirShaderFoldPublishesHealthyVerdictTest,
    "PinWright.material.compile_mgir.shader_compile.FoldPublishesHealthyVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileMgirShaderFoldPublishesHealthyVerdictTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::MaterialShaderState;

    for (const EStatus Healthy : {EStatus::Completed, EStatus::OnDemand})
    {
        FState One;
        One.Status = Healthy;
        FState Folded;
        Folded.Accumulate(TEXT("/Game/Test/M_A.M_A"), One);
        Folded.Accumulate(TEXT("/Game/Test/M_B.M_B"), One);
        TestEqual(TEXT("a fold of healthy materials keeps their status"),
            FString(ToWire(Folded.Status)), FString(ToWire(Healthy)));

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        AddReport(Result, Folded);
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (TestTrue(FString::Printf(TEXT("a %s fold publishes the block"), ToWire(Healthy)),
                Result->TryGetObjectField(TEXT("shaderCompile"), Block) && Block))
        {
            TestEqual(TEXT("the block carries the folded status"),
                (*Block)->GetStringField(TEXT("status")), FString(ToWire(Healthy)));
        }
    }

    FState Completed;
    Completed.Status = EStatus::Completed;
    FState Failed;
    Failed.Status = EStatus::Failed;
    Failed.Errors.Add(TEXT("error: x"));
    FState Mixed;
    Mixed.Accumulate(TEXT("/Game/Test/M_A.M_A"), Completed);
    Mixed.Accumulate(TEXT("/Game/Test/M_B.M_B"), Failed);
    TestTrue(TEXT("one failed material fails the fold"), Mixed.Failed());
    TestEqual(TEXT("the fold carries the failed material's errors"), Mixed.Errors.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMgirShaderCleanMaterialReportsCompletedTest,
    "PinWright.material.compile_mgir.shader_compile.CleanMaterialReportsCompletedWithAndWithoutSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileMgirShaderCleanMaterialReportsCompletedTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRShaderClean"));
    const FString Document = CleanDocument(Scratch.PackagePath);

    for (const bool bSave : {false, true})
    {
        const FString Label = bSave ? TEXT("save:true") : TEXT("save:false");
        FTestResponseCapture Capture;
        const TSharedPtr<FJsonObject>* Block =
            CompileAndReadBlock(*this, Label, Document, bSave, true, Capture);
        if (!Block)
        {
            continue;
        }
        const FString Status = (*Block)->GetStringField(TEXT("status"));
        TestNotEqual(*FString::Printf(TEXT("[%s] a clean material never reports failed"), *Label),
            Status, FString(TEXT("failed")));
        if (Status != TEXT("completed"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
                FString::Printf(TEXT("[%s] the waited compile ended '%s', not 'completed'; this "
                    "host did not finish the platform shader compile inside the bounded wait."),
                    *Label, *Status));
            continue;
        }
        TestTrue(*FString::Printf(TEXT("[%s] completed is a success"), *Label),
            (*Block)->GetBoolField(TEXT("succeeded")));
        TestFalse(*FString::Printf(TEXT("[%s] a clean compile raises no warnings"), *Label),
            Capture.Result->HasField(TEXT("warnings")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMgirShaderBrokenHlslReportsErrorsTest,
    "PinWright.material.compile_mgir.shader_compile.BrokenHlslReportsErrorsWithAndWithoutSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileMgirShaderBrokenHlslReportsErrorsTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRShaderBroken"));
    const FString Document = BrokenHlslDocument(Scratch.PackagePath);

    // The non-blocking probe first, on the freshly written material: whatever it reads (onDemand,
    // outstanding, failed), it must publish a block and must not call the shader a success.
    {
        FTestResponseCapture Capture;
        if (const TSharedPtr<FJsonObject>* Block =
                CompileAndReadBlock(*this, TEXT("no wait"), Document, false, false, Capture))
        {
            TestFalse(TEXT("[no wait] a broken shader is never reported as succeeded"),
                (*Block)->GetBoolField(TEXT("succeeded")));
        }
    }

    for (const bool bSave : {false, true})
    {
        const FString Label = bSave ? TEXT("save:true") : TEXT("save:false");
        FTestResponseCapture Capture;
        const TSharedPtr<FJsonObject>* Block =
            CompileAndReadBlock(*this, Label, Document, bSave, true, Capture);
        if (!Block)
        {
            continue;
        }
        TestFalse(*FString::Printf(TEXT("[%s] a broken shader is never reported as succeeded"),
            *Label), (*Block)->GetBoolField(TEXT("succeeded")));

        const FString Status = (*Block)->GetStringField(TEXT("status"));
        if (Status != TEXT("failed"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
                FString::Printf(TEXT("[%s] the waited compile ended '%s', not 'failed'; this host "
                    "did not run the platform shader compiler to a verdict inside the bounded wait."),
                    *Label, *Status));
            continue;
        }
        TestTrue(*FString::Printf(TEXT("[%s] failed is flagged"), *Label),
            (*Block)->GetBoolField(TEXT("failed")));
        const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
        TestTrue(*FString::Printf(TEXT("[%s] the HLSL errors are reported"), *Label),
            (*Block)->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() > 0);

        // The failure also reaches the top level, so a caller reading only blocksCompiled and
        // warnings[] cannot take the graph write for a working material.
        bool bWarned = false;
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
        {
            for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
            {
                bWarned |= Warning->AsString().Contains(TEXT("FAILED to compile"));
            }
        }
        TestTrue(*FString::Printf(TEXT("[%s] warnings[] names the shader failure"), *Label),
            bWarned);
    }
    return true;
}
