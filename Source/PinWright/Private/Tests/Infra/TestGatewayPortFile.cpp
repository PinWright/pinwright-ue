// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the GatewayPortFile util: port file write/overwrite, path shape
// under the test-only root override, and tmp-file hygiene.
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Utils/GatewayPortFile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightGatewayPortFileTest
{
    // Unique per-test scratch dir under <ProjectSavedDir>/PinWright/TestTemp/<Guid>.
    FString MakePortFileTestRoot()
    {
        FString Root = FPaths::ProjectSavedDir() /
            TEXT("PinWright/TestTemp") /
            FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayPortFileWritesPortFileTest,
    "PinWright.infra.gateway_port_file.WritesPortFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayPortFileWritesPortFileTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayPortFileTest::MakePortFileTestRoot();
    GatewayPortFile::SetRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    TestTrue(TEXT("WritePortFile succeeds"), GatewayPortFile::WritePortFile(23456));

    const FString PortPath = GatewayPortFile::GetPortFilePath();
    TestTrue(TEXT("Port file exists at GetPortFilePath()"),
        IFileManager::Get().FileExists(*PortPath));

    FString Content;
    TestTrue(TEXT("Port file loads as a string"),
        FFileHelper::LoadFileToString(Content, *PortPath));
    TestEqual(TEXT("Port file content is the decimal port"), Content, FString(TEXT("23456")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayPortFileOverwritesOnSecondWriteTest,
    "PinWright.infra.gateway_port_file.OverwritesOnSecondWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayPortFileOverwritesOnSecondWriteTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayPortFileTest::MakePortFileTestRoot();
    GatewayPortFile::SetRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    TestTrue(TEXT("First write succeeds"), GatewayPortFile::WritePortFile(23456));
    TestTrue(TEXT("Second write succeeds"), GatewayPortFile::WritePortFile(34567));

    FString Content;
    TestTrue(TEXT("Port file loads as a string"),
        FFileHelper::LoadFileToString(Content, *GatewayPortFile::GetPortFilePath()));
    TestEqual(TEXT("Second write overwrites the content"), Content, FString(TEXT("34567")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayPortFilePathUnderOverrideRootNoTmpTest,
    "PinWright.infra.gateway_port_file.PathUnderOverrideRootNoTmp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayPortFilePathUnderOverrideRootNoTmpTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayPortFileTest::MakePortFileTestRoot();
    GatewayPortFile::SetRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString PortPath = GatewayPortFile::GetPortFilePath();
    TestTrue(TEXT("Port path is under the override root"),
        PortPath.StartsWith(Root + TEXT("/")));
    TestTrue(TEXT("Port path ends with gateway-port"),
        PortPath.EndsWith(TEXT("gateway-port")));

    TestTrue(TEXT("WritePortFile succeeds"), GatewayPortFile::WritePortFile(23456));
    TestFalse(TEXT("No leftover .tmp file after atomic write"),
        IFileManager::Get().FileExists(*(PortPath + TEXT(".tmp"))));

    return true;
}

#endif
