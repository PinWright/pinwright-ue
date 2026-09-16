// Copyright (c) 2026 Alexander Penkin. MIT License.

// Focused contract tests for localization.gather / localization.compile.
// These tests exercise validation and registration only; they deliberately do
// not launch the Unreal localization commandlet.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Localization/LocalizationCommand.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

using namespace PinWrightLocalization;

namespace
{
    // Names are prefixed because Unity merges these TUs and anonymous namespaces
    // collide across the merged translation unit.

    // A throwaway project root under Saved/. ResolveConfig takes the project
    // directory as a parameter, so the whole Config/Localization rule can be
    // exercised against a root this test owns instead of the host project, which
    // is not required to ship a localization config at all.
    FString PinWrightLocTest_MakeScratchProjectRoot()
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("Tests") /
            FString::Printf(TEXT("Localization_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }

    // Reports the verdict by name so a failure says which verdict was produced.
    FString PinWrightLocTest_ResultName(const EConfigContentResult Result)
    {
        switch (Result)
        {
        case EConfigContentResult::Valid:             return TEXT("Valid");
        case EConfigContentResult::OperationMismatch: return TEXT("OperationMismatch");
        case EConfigContentResult::TargetMismatch:    return TEXT("TargetMismatch");
        }
        return TEXT("Unknown");
    }

    // Minimal but realistic localization configs: one Gather step and one Compile
    // step, both declaring Game.manifest.
    const TCHAR* const PinWrightLocTest_GatherConfigText =
        TEXT("[CommonSettings]\nManifestName=Game.manifest\n\n")
        TEXT("[GatherTextStep0]\nCommandletClass=GatherTextFromSource\n");

    const TCHAR* const PinWrightLocTest_CompileConfigText =
        TEXT("[CommonSettings]\nManifestName=Game.manifest\n\n")
        TEXT("[CompileTextStep0]\nCommandletClass=GenerateTextLocalizationResource\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLocalizationGatherRegisteredTest,
    "PinWright.localization.gather.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLocalizationGatherRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("localization.gather is registered"),
        IsHandlerRegistered(TEXT("localization.gather")));

    const FParamSpec* Target = GetRegisteredParamSpec(
        TEXT("localization.gather"), TEXT("target"));
    const FParamSpec* Config = GetRegisteredParamSpec(
        TEXT("localization.gather"), TEXT("config"));
    TestNotNull(TEXT("gather exposes target"), Target);
    TestNotNull(TEXT("gather exposes config"), Config);
    if (Target)
    {
        TestTrue(TEXT("gather target is required"), Target->bRequired);
        TestEqual(TEXT("gather target type"), Target->Type, FString(TEXT("string")));
    }
    if (Config)
    {
        TestFalse(TEXT("gather config is optional"), Config->bRequired);
        TestEqual(TEXT("gather config type"), Config->Type, FString(TEXT("filepath")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLocalizationCompileRegisteredTest,
    "PinWright.localization.compile.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLocalizationCompileRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("localization.compile is registered"),
        IsHandlerRegistered(TEXT("localization.compile")));
    TestNotNull(TEXT("compile exposes target"), GetRegisteredParamSpec(
        TEXT("localization.compile"), TEXT("target")));
    TestNotNull(TEXT("compile exposes config"), GetRegisteredParamSpec(
        TEXT("localization.compile"), TEXT("config")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLocalizationTargetValidationTest,
    "PinWright.localization.Validation.Target",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLocalizationTargetValidationTest::RunTest(const FString& Parameters)
{
    FString Normalized;
    FString Error;
    TestTrue(TEXT("valid target accepted"),
        ValidateTarget(TEXT(" Game_2-Target "), Normalized, Error));
    TestEqual(TEXT("target trimmed"), Normalized, FString(TEXT("Game_2-Target")));

    TestFalse(TEXT("path target rejected"),
        ValidateTarget(TEXT("../Game"), Normalized, Error));
    TestFalse(TEXT("target with spaces rejected"),
        ValidateTarget(TEXT("Game Target"), Normalized, Error));
    TestFalse(TEXT("empty target rejected"),
        ValidateTarget(TEXT(""), Normalized, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLocalizationConfigPathValidationTest,
    "PinWright.localization.Validation.ConfigPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLocalizationConfigPathValidationTest::RunTest(const FString& Parameters)
{
    // The plugin does not own the host project's Config/Localization tree and a
    // host is not required to ship one, so the resolver is exercised against a
    // scratch project root this test stages and deletes.
    const FString ScratchRoot = PinWrightLocTest_MakeScratchProjectRoot();
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString GatherConfigFull = ScratchRoot / TEXT("Config/Localization/Game_Gather.ini");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(GatherConfigFull), /*Tree=*/true);
    if (!FFileHelper::SaveStringToFile(FString(PinWrightLocTest_GatherConfigText), *GatherConfigFull))
    {
        AddError(FString::Printf(TEXT("could not stage scratch config at %s"), *GatherConfigFull));
        return false;
    }

    FString RelativePath;
    FString FullPath;
    FString Error;

    TestTrue(TEXT("default Game Gather config resolves"),
        ResolveConfig(ScratchRoot, TEXT("Game"), EOperation::Gather, TEXT(""),
            RelativePath, FullPath, Error));
    TestEqual(TEXT("default gather config path"), RelativePath,
        FString(TEXT("Config/Localization/Game_Gather.ini")));
    TestEqual(TEXT("resolved full path is the staged config"),
        FPaths::ConvertRelativePathToFull(FullPath),
        FPaths::ConvertRelativePathToFull(GatherConfigFull));
    TestTrue(TEXT("resolved config exists"), FPaths::FileExists(FullPath));

    // A well-formed path under Config/Localization is still rejected when nothing
    // is there: Game_Compile.ini was deliberately not staged. This is what keeps
    // the success case above from being satisfied by a resolver that accepts
    // everything shaped like a config path.
    TestFalse(TEXT("missing config rejected"),
        ResolveConfig(ScratchRoot, TEXT("Game"), EOperation::Compile, TEXT(""),
            RelativePath, FullPath, Error));
    TestFalse(TEXT("missing config reports why"), Error.IsEmpty());

    TestFalse(TEXT("parent traversal rejected"),
        ResolveConfig(ScratchRoot, TEXT("Game"), EOperation::Gather,
            TEXT("Config/Localization/../DefaultEditor.ini"),
            RelativePath, FullPath, Error));
    TestFalse(TEXT("absolute config rejected"),
        ResolveConfig(ScratchRoot, TEXT("Game"), EOperation::Gather,
            ScratchRoot / TEXT("Config/Localization/Game_Gather.ini"),
            RelativePath, FullPath, Error));
    TestFalse(TEXT("config outside localization directory rejected"),
        ResolveConfig(ScratchRoot, TEXT("Game"), EOperation::Gather,
            TEXT("Config/DefaultEditor.ini"), RelativePath, FullPath, Error));
    TestFalse(TEXT("non-INI config rejected"),
        ResolveConfig(ScratchRoot, TEXT("Game"), EOperation::Gather,
            TEXT("Config/Localization/Game_Gather.txt"), RelativePath, FullPath, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLocalizationInvalidRequestRejectedTest,
    "PinWright.localization.Validation.InvalidRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLocalizationInvalidRequestRejectedTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("target"), TEXT("../Game"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("gather handler found"), InvokeHandlerWithCapture(
            TEXT("localization.gather"), Payload, Capture));
        TestFalse(TEXT("invalid target rejected"), Capture.bSuccess);
        TestEqual(TEXT("invalid target error"), Capture.ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("target"), TEXT("Game"));
        Payload->SetStringField(TEXT("config"), TEXT("Config/Localization/../DefaultEditor.ini"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("compile handler found"), InvokeHandlerWithCapture(
            TEXT("localization.compile"), Payload, Capture));
        TestFalse(TEXT("traversal config rejected"), Capture.bSuccess);
        TestEqual(TEXT("traversal config error"), Capture.ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
    }

    // The remaining two rejections are about config *contents*, not the request
    // envelope: reaching them through the handler would need a real .ini inside the
    // host project's Config/Localization, which the plugin does not own. They are
    // asserted against the validator the handler delegates to instead
    // (LocalizationHandler.cpp ParseRequest maps OperationMismatch ->
    // CONFIG_OPERATION_MISMATCH and TargetMismatch -> CONFIG_TARGET_MISMATCH).
    {
        const FString GatherConfig(PinWrightLocTest_GatherConfigText);
        const FString CompileConfig(PinWrightLocTest_CompileConfigText);

        TestEqual(TEXT("gather config accepted for gather"),
            PinWrightLocTest_ResultName(
                ValidateConfigContents(GatherConfig, TEXT("Game"), EOperation::Gather)),
            FString(TEXT("Valid")));
        TestEqual(TEXT("compile config accepted for compile"),
            PinWrightLocTest_ResultName(
                ValidateConfigContents(CompileConfig, TEXT("Game"), EOperation::Compile)),
            FString(TEXT("Valid")));

        // Operation mismatch, asserted in both directions so the check cannot pass
        // by always reporting one verdict.
        TestEqual(TEXT("gather config rejected by compile"),
            PinWrightLocTest_ResultName(
                ValidateConfigContents(GatherConfig, TEXT("Game"), EOperation::Compile)),
            FString(TEXT("OperationMismatch")));
        TestEqual(TEXT("compile config rejected by gather"),
            PinWrightLocTest_ResultName(
                ValidateConfigContents(CompileConfig, TEXT("Game"), EOperation::Gather)),
            FString(TEXT("OperationMismatch")));

        // Right commandlet, wrong target: the manifest names Game, the request asks
        // for EngineOverrides.
        TestEqual(TEXT("mismatched target config rejected"),
            PinWrightLocTest_ResultName(
                ValidateConfigContents(GatherConfig, TEXT("EngineOverrides"), EOperation::Gather)),
            FString(TEXT("TargetMismatch")));
    }
    return true;
}
