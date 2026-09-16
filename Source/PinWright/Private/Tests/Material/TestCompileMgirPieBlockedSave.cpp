// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Handlers/ErrorCodes.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/FileHelper.h"
#include "Templates/UnrealTemplate.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "Tests/TestUtils.h"
#include "Utils/PieState.h"

namespace
{
TSharedPtr<FJsonObject> MakeCompilePayload(const FString& AssetPath, double Value)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%value = constant Float1(%g)\n")
        TEXT("    output Metallic: %%value\n")
        TEXT("}\n"),
        *AssetPath,
        Value));
    Payload->SetStringField(TEXT("mode"), TEXT("Append"));
    Payload->SetBoolField(TEXT("runLayout"), false);
    Payload->SetBoolField(TEXT("save"), true);
    return Payload;
}

UMaterialExpressionConstant* FindConstantExpression(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }
    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
        {
            return Constant;
        }
    }
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompileMgirReportsPieBlockedSaveTest,
    "PinWright.material.compile_mgir.ReportsPieBlockedSaveWithoutDiscardingCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCompileMgirReportsPieBlockedSaveTest::RunTest(const FString& Parameters)
{
    if (PinWrightPieState::IsPlayInEditorActive())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-already-active"),
            TEXT("This test must create its durable baseline before it simulates the PIE save gate."));
        return true;
    }

    IrTest::FScratchAsset Scratch(TEXT("M_MGIRPieBlockedSave"));
    const FString Filename = PackageFilenameFromAssetPath(Scratch.PackagePath);
    if (!TestFalse(TEXT("fixture path resolves to a .uasset filename"), Filename.IsEmpty()))
    {
        return false;
    }

    FTestResponseCapture BaselineCapture;
    TestTrue(TEXT("baseline compile_mgir handler is registered"),
        InvokeHandlerWithCapture(TEXT("material.compile_mgir"),
            MakeCompilePayload(Scratch.PackagePath, 0.25), BaselineCapture));
    TestTrue(TEXT("baseline compile responded"), BaselineCapture.bWasCalled);
    if (!TestTrue(TEXT("baseline compile and save succeeded"),
            BaselineCapture.bSuccess && BaselineCapture.Result.IsValid()))
    {
        return false;
    }

    bool bBaselineSaved = false;
    TestTrue(TEXT("baseline reports a durable save"),
        BaselineCapture.Result->TryGetBoolField(TEXT("saved"), bBaselineSaved) && bBaselineSaved);
    TestTrue(TEXT("baseline .uasset exists"), IFileManager::Get().FileSize(*Filename) > 0);
    TArray<uint8> BaselineBytes;
    if (!TestTrue(TEXT("baseline .uasset bytes can be read"),
            FFileHelper::LoadFileToArray(BaselineBytes, *Filename)))
    {
        return false;
    }

    FTestResponseCapture BlockedCapture;
    {
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        TestTrue(TEXT("compile_mgir handler remains callable under the simulated PIE gate"),
            InvokeHandlerWithCapture(TEXT("material.compile_mgir"),
                MakeCompilePayload(Scratch.PackagePath, 0.75), BlockedCapture));
        TestTrue(TEXT("PIE-blocked save still returns a response"), BlockedCapture.bWasCalled);
        if (!TestTrue(TEXT("PIE-blocked save preserves compile success and its payload"),
                BlockedCapture.bSuccess && BlockedCapture.Result.IsValid()))
        {
            return false;
        }

        double BlocksCompiled = 0.0;
        double ExpressionsCreated = 0.0;
        TestTrue(TEXT("success payload retains blocksCompiled"),
            BlockedCapture.Result->TryGetNumberField(TEXT("blocksCompiled"), BlocksCompiled)
                && BlocksCompiled == 1.0);
        TestTrue(TEXT("success payload retains expressionsCreated"),
            BlockedCapture.Result->TryGetNumberField(TEXT("expressionsCreated"), ExpressionsCreated)
                && ExpressionsCreated == 1.0);
        TestTrue(TEXT("success payload retains the compiled asset path"),
            JsonStringArrayContains(BlockedCapture.Result, TEXT("assetPaths"),
                FString::Printf(TEXT("%s.%s"), *Scratch.PackagePath, *Scratch.AssetName)));

        bool bSaveRequested = false;
        bool bSaved = true;
        bool bPendingFlush = true;
        bool bPieActive = false;
        FString SaveState;
        FString SaveError;
        FString EditorMode;
        TestTrue(TEXT("saveRequested remains true"),
            BlockedCapture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested)
                && bSaveRequested);
        TestTrue(TEXT("PIE-blocked save reports saved:false"),
            BlockedCapture.Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);
        TestTrue(TEXT("PIE-blocked save reports pendingFlush:false because a flush cannot write"),
            BlockedCapture.Result->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush)
                && !bPendingFlush);
        TestTrue(TEXT("PIE-blocked save uses the AssetSaveState wire value"),
            BlockedCapture.Result->TryGetStringField(TEXT("saveState"), SaveState)
                && SaveState == TEXT("blockedByPie"));
        TestTrue(TEXT("PIE-blocked save carries typed PIE_ACTIVE"),
            BlockedCapture.Result->TryGetStringField(TEXT("saveError"), SaveError)
                && SaveError == ErrorCodes::ERR_PIE_ACTIVE);
        TestTrue(TEXT("PIE diagnostic names the active gate"),
            BlockedCapture.Result->TryGetBoolField(TEXT("pieActive"), bPieActive) && bPieActive);
        TestTrue(TEXT("PIE diagnostic reports editorMode PIE"),
            BlockedCapture.Result->TryGetStringField(TEXT("editorMode"), EditorMode)
                && EditorMode == TEXT("PIE"));
        TestTrue(TEXT("PIE diagnostic includes the world list even when the simulation has none"),
            BlockedCapture.Result->HasTypedField<EJson::Array>(TEXT("pieWorlds")));
    }

    UMaterial* Material = LoadObject<UMaterial>(nullptr,
        *FString::Printf(TEXT("%s.%s"), *Scratch.PackagePath, *Scratch.AssetName));
    UMaterialExpressionConstant* Constant = FindConstantExpression(Material);
    TestNotNull(TEXT("compiled material keeps the new in-memory expression"), Constant);
    if (Constant)
    {
        TestEqual(TEXT("graph mutation landed in memory despite the blocked save"), Constant->R, 0.75f);
    }
    TestTrue(TEXT("blocked save leaves the changed package dirty"),
        Material && Material->GetOutermost()->IsDirty());

    TArray<uint8> BytesAfter;
    TestTrue(TEXT("baseline .uasset remains readable after the blocked save"),
        FFileHelper::LoadFileToArray(BytesAfter, *Filename));
    TestTrue(TEXT("PIE-blocked save leaves the on-disk revision byte-identical"),
        BytesAfter == BaselineBytes);

    // Counterfactual: restoring the old MGIR_SAVE_FAILED return makes the success and save
    // contract assertions above fail before this in-memory/disk split can be inspected.
    return true;
}
