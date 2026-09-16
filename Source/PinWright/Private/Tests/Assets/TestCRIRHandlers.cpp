// Copyright (c) 2026 Alexander Penkin. MIT License.

// RPC-envelope tests for controlrig.compile_crir / controlrig.decompile_crir.
//
// The underlying FCRIRCompiler / FCRIRDecompiler engine is covered by the other
// TestCRIR*.cpp files; these tests exercise only the handler shell: param
// extraction, the replace/extend mode parser, error-code mapping, and the
// success-payload shape, driven through the dispatcher via InvokeHandlerWithCapture.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Units/RigUnit.h"
#include "Utils/AssetUtils.h"
#include "Utils/ControlRigBlueprintCompat.h"

namespace
{
    // Creates a transient Control Rig BP and wires BeginExecution into the model so
    // a subsequent decompile yields non-trivial CRIR text. Returns the BP object path
    // through OutObjectPath, or nullptr (with a skip note) if BP creation is unavailable.
    UControlRigBlueprint* MakeWiredControlRigBP(FAutomationTestBase& Test, FString& OutPackagePath, FString& OutObjectPath)
    {
        const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString AssetName = FString::Printf(TEXT("CR_CRIRHandler_%s"), *Guid);
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackagePath, *AssetName);

        FString CreateError;
        UBlueprint* Raw = McpCreateControlRigBlueprint(
            AssetName, TEXT("/Game/PinWrightTests"), /*TargetSkeleton*/ nullptr, CreateError);
        UControlRigBlueprint* BP = Cast<UControlRigBlueprint>(Raw);
        if (!BP)
        {
            Test.AddInfo(FString::Printf(TEXT("could not create CR BP (%s) - test skipped."), *CreateError));
            return nullptr;
        }

        URigVMGraph* Model = nullptr;
        URigVMController* Controller = GetFirstModelController(BP, Model);
        if (!Controller || !Model)
        {
            Test.AddInfo(TEXT("model controller unavailable - test skipped."));
            return nullptr;
        }

        URigVMNode* BeginNode = Controller->AddUnitNodeFromStructPath(
            TEXT("/Script/ControlRig.RigUnit_BeginExecution"),
            FRigUnit::GetMethodName(),
            FVector2D(0, 0),
            /*InNodeName*/ FString(TEXT("BeginExecution")),
            /*bSetupUndoRedo*/ false,
            /*bPrintPythonCommand*/ false);
        if (!BeginNode)
        {
            Test.AddInfo(TEXT("BeginExecution node creation failed - test skipped."));
            return nullptr;
        }
        return BP;
    }

    // Returns the warnings array from a captured success payload, or nullptr if absent.
    const TArray<TSharedPtr<FJsonValue>>* GetWarningsArray(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (Result.IsValid())
        {
            Result->TryGetArrayField(TEXT("warnings"), Warnings);
        }
        return Warnings;
    }
}

// ============================================================================
// controlrig.compile_crir
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCompileBadModeTest,
    "PinWright.controlrig.compile_crir.BadMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRCompileBadModeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), TEXT("rig_graph {}"));
    Payload->SetStringField(TEXT("context"), TEXT("/Game/Nonexistent/CR_Unused"));
    Payload->SetStringField(TEXT("mode"), TEXT("invalid"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.compile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.compile_crir"), Payload, Capture));
    TestTrue(TEXT("compile_crir sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("compile_crir failed on bad mode"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMS"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(TEXT("error message mentions unknown CRIR compile mode"),
        Capture.Message.Contains(TEXT("Unknown CRIR compile mode")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCompileMissingTextTest,
    "PinWright.controlrig.compile_crir.MissingText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRCompileMissingTextTest::RunTest(const FString& Parameters)
{
    // Omits required "text"; "context" alone is not enough to pass RequireString.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("context"), TEXT("/Game/Nonexistent/CR_Unused"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.compile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.compile_crir"), Payload, Capture));
    TestTrue(TEXT("compile_crir sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("compile_crir failed on missing text"), Capture.bSuccess);
    TestEqual(TEXT("error code exists"), !Capture.ErrorCode.IsEmpty(), true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCompileMissingContextTest,
    "PinWright.controlrig.compile_crir.MissingContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRCompileMissingContextTest::RunTest(const FString& Parameters)
{
    // Omits required "context"; "text" alone is not enough to pass RequireString.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), TEXT("rig_graph {}"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.compile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.compile_crir"), Payload, Capture));
    TestTrue(TEXT("compile_crir sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("compile_crir failed on missing context"), Capture.bSuccess);
    TestEqual(TEXT("error code exists"), !Capture.ErrorCode.IsEmpty(), true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCompileValidModeReplaceTest,
    "PinWright.controlrig.compile_crir.ValidModeReplace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRCompileValidModeReplaceTest::RunTest(const FString& Parameters)
{
    FString SourcePackage, SourceObject;
    UControlRigBlueprint* SourceBP = MakeWiredControlRigBP(*this, SourcePackage, SourceObject);
    FString TargetPackage, TargetObject;
    UControlRigBlueprint* TargetBP = SourceBP ? MakeWiredControlRigBP(*this, TargetPackage, TargetObject) : nullptr;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePackage);
        CleanupTestAsset(TargetPackage);
    };

    if (!SourceBP || !TargetBP)
    {
        return true;
    }

    FCRIRDecompileResult Decompiled = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("source decompile produced text"), Decompiled.bSuccess && !Decompiled.CRIRText.IsEmpty());
    if (!Decompiled.bSuccess)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), Decompiled.CRIRText);
    Payload->SetStringField(TEXT("context"), TargetObject);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    Payload->SetBoolField(TEXT("runLayout"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.compile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.compile_crir"), Payload, Capture));
    TestTrue(TEXT("compile_crir sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("success response received"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("mode field is 'replace'"), Capture.Result->GetStringField(TEXT("mode")), FString(TEXT("replace")));
    TestEqual(TEXT("assetPath field present"), Capture.Result->HasField(TEXT("assetPath")), true);
    TestEqual(TEXT("blocksCompiled field present"), Capture.Result->HasField(TEXT("blocksCompiled")), true);
    TestTrue(TEXT("nodesCreated > 0"), Capture.Result->GetNumberField(TEXT("nodesCreated")) > 0);
    TestNotNull(TEXT("warnings array present"), GetWarningsArray(Capture.Result));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCompileValidModeExtendTest,
    "PinWright.controlrig.compile_crir.ValidModeExtend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRCompileValidModeExtendTest::RunTest(const FString& Parameters)
{
    FString SourcePackage, SourceObject;
    UControlRigBlueprint* SourceBP = MakeWiredControlRigBP(*this, SourcePackage, SourceObject);
    FString TargetPackage, TargetObject;
    UControlRigBlueprint* TargetBP = SourceBP ? MakeWiredControlRigBP(*this, TargetPackage, TargetObject) : nullptr;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePackage);
        CleanupTestAsset(TargetPackage);
    };

    if (!SourceBP || !TargetBP)
    {
        return true;
    }

    FCRIRDecompileResult Decompiled = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("source decompile produced text"), Decompiled.bSuccess && !Decompiled.CRIRText.IsEmpty());
    if (!Decompiled.bSuccess)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), Decompiled.CRIRText);
    Payload->SetStringField(TEXT("context"), TargetObject);
    Payload->SetStringField(TEXT("mode"), TEXT("extend"));
    Payload->SetBoolField(TEXT("runLayout"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.compile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.compile_crir"), Payload, Capture));
    TestTrue(TEXT("compile_crir sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("success response received"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("mode field is 'extend'"), Capture.Result->GetStringField(TEXT("mode")), FString(TEXT("extend")));
    TestEqual(TEXT("assetPath field present"), Capture.Result->HasField(TEXT("assetPath")), true);
    TestEqual(TEXT("assetPath matches target"), Capture.Result->GetStringField(TEXT("assetPath")), TargetObject);
    TestEqual(TEXT("blocksCompiled field present"), Capture.Result->HasField(TEXT("blocksCompiled")), true);
    TestEqual(TEXT("nodesCreated field present"), Capture.Result->HasField(TEXT("nodesCreated")), true);
    TestNotNull(TEXT("warnings array present"), GetWarningsArray(Capture.Result));
    return true;
}

// ============================================================================
// controlrig.decompile_crir
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRDecompileValidAssetTest,
    "PinWright.controlrig.decompile_crir.ValidAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRDecompileValidAssetTest::RunTest(const FString& Parameters)
{
    FString PackagePath, ObjectPath;
    UControlRigBlueprint* BP = MakeWiredControlRigBP(*this, PackagePath, ObjectPath);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    if (!BP)
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.decompile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.decompile_crir"), Payload, Capture));
    TestTrue(TEXT("decompile_crir sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("success response received"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("assetPath matches"), Capture.Result->GetStringField(TEXT("assetPath")), ObjectPath);
    TestTrue(TEXT("text field is non-empty"), !Capture.Result->GetStringField(TEXT("text")).IsEmpty());
    TestNotNull(TEXT("warnings array present"), GetWarningsArray(Capture.Result));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRDecompileMissingAssetPathTest,
    "PinWright.controlrig.decompile_crir.MissingAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRDecompileMissingAssetPathTest::RunTest(const FString& Parameters)
{
    // Omits required "assetPath".
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    FTestResponseCapture Capture;
    TestTrue(TEXT("controlrig.decompile_crir handler found"),
        InvokeHandlerWithCapture(TEXT("controlrig.decompile_crir"), Payload, Capture));
    TestTrue(TEXT("decompile_crir sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("decompile_crir failed on missing assetPath"), Capture.bSuccess);
    TestEqual(TEXT("error code exists"), !Capture.ErrorCode.IsEmpty(), true);
    return true;
}
