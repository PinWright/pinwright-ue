// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the GatewayAuthToken util: token file creation/persistence,
// the test-only root override seam, tmp-file hygiene, and constant-time compare.
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Utils/GatewayAuthToken.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightGatewayAuthTokenTest
{
    // Unique per-test scratch dir under <ProjectSavedDir>/PinWright/TestTemp/<Guid>.
    FString MakeTokenTestRoot()
    {
        FString Root = FPaths::ProjectSavedDir() /
            TEXT("PinWright/TestTemp") /
            FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }

    // The default (override-cleared) token root: <ProjectSavedDir>/PinWright.
    FString DefaultTokenRoot()
    {
        FString Root = FPaths::ProjectSavedDir() / TEXT("PinWright");
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }

    bool IsLowercaseHex64(const FString& Token)
    {
        if (Token.Len() != 64)
        {
            return false;
        }
        for (const TCHAR C : Token)
        {
            const bool bIsLowerHex =
                (C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'));
            if (!bIsLowerHex)
            {
                return false;
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayAuthTokenCreatesTokenFileTest,
    "PinWright.infra.gateway_auth_token.CreatesTokenFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayAuthTokenCreatesTokenFileTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayAuthTokenTest::MakeTokenTestRoot();
    GatewayAuthToken::SetTokenRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayAuthToken::SetTokenRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString Token = GatewayAuthToken::GetOrCreateToken();

    TestTrue(TEXT("Token is 64 lowercase hex characters"),
        PinWrightGatewayAuthTokenTest::IsLowercaseHex64(Token));

    TestTrue(TEXT("Token file exists at GetTokenFilePath()"),
        IFileManager::Get().FileExists(*GatewayAuthToken::GetTokenFilePath()));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayAuthTokenStableAcrossCallsTest,
    "PinWright.infra.gateway_auth_token.StableAcrossCalls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayAuthTokenStableAcrossCallsTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayAuthTokenTest::MakeTokenTestRoot();
    GatewayAuthToken::SetTokenRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayAuthToken::SetTokenRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString First = GatewayAuthToken::GetOrCreateToken();
    const FString Second = GatewayAuthToken::GetOrCreateToken();

    TestFalse(TEXT("First token is non-empty"), First.IsEmpty());
    TestEqual(TEXT("Second call returns the identical token"), Second, First);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayAuthTokenHonorsOverrideRootTest,
    "PinWright.infra.gateway_auth_token.HonorsOverrideRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayAuthTokenHonorsOverrideRootTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayAuthTokenTest::MakeTokenTestRoot();
    GatewayAuthToken::SetTokenRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayAuthToken::SetTokenRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    // With the override set, the token path lives under the scratch dir.
    const FString OverriddenPath = GatewayAuthToken::GetTokenFilePath();
    TestTrue(TEXT("Token path is under the override root"),
        OverriddenPath.StartsWith(Root + TEXT("/")));

    // Clearing the override returns the path to the default <Saved>/PinWright root.
    GatewayAuthToken::SetTokenRootOverrideForTests(FString());
    const FString DefaultPath = GatewayAuthToken::GetTokenFilePath();
    const FString DefaultRoot = PinWrightGatewayAuthTokenTest::DefaultTokenRoot();
    TestTrue(TEXT("Cleared path is back under <Saved>/PinWright"),
        DefaultPath.StartsWith(DefaultRoot + TEXT("/")));
    TestFalse(TEXT("Cleared path is no longer under the scratch dir"),
        DefaultPath.StartsWith(Root + TEXT("/")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayAuthTokenNoTmpLeftBehindTest,
    "PinWright.infra.gateway_auth_token.NoTmpLeftBehind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayAuthTokenNoTmpLeftBehindTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightGatewayAuthTokenTest::MakeTokenTestRoot();
    GatewayAuthToken::SetTokenRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayAuthToken::SetTokenRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    GatewayAuthToken::GetOrCreateToken();

    const FString TmpPath = GatewayAuthToken::GetTokenFilePath() + TEXT(".tmp");
    TestFalse(TEXT("No leftover .tmp file after atomic write"),
        IFileManager::Get().FileExists(*TmpPath));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGatewayAuthTokenConstantTimeEqualsCasesTest,
    "PinWright.infra.gateway_auth_token.ConstantTimeEqualsCases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGatewayAuthTokenConstantTimeEqualsCasesTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Equal strings compare true"),
        GatewayAuthToken::ConstantTimeEquals(TEXT("abc123"), TEXT("abc123")));
    TestFalse(TEXT("Equal-length different strings compare false"),
        GatewayAuthToken::ConstantTimeEquals(TEXT("abc123"), TEXT("abc124")));
    TestFalse(TEXT("Different-length strings compare false"),
        GatewayAuthToken::ConstantTimeEquals(TEXT("abc"), TEXT("abc123")));
    TestFalse(TEXT("Empty vs non-empty compares false"),
        GatewayAuthToken::ConstantTimeEquals(TEXT(""), TEXT("abc")));
    TestTrue(TEXT("Empty vs empty compares true"),
        GatewayAuthToken::ConstantTimeEquals(TEXT(""), TEXT("")));

    return true;
}

#endif
