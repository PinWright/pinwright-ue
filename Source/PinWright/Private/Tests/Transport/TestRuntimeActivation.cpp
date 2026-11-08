// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins the serving rules of docs/runtime-module.md on UPinWrightRuntimeSubsystem:
//   - activation gate: a game process serves only with -PinWright or -PinWrightPort=<N|0>;
//     -PinWrightPort=0 binds an ephemeral port; a -PinWrightPort instance never writes gateway-port;
//   - instance file <root>/instances/<pid>.json: written after a successful bind with the contract
//     schema, user-only permissions, deleted on clean shutdown;
//   - the instance-file token authorizes requests to that instance.
// Every case runs the real activation through ActivateForTesting with a canned command line and
// process kind, against a private root (instance files and gateway-port), so the live editor's
// own Saved/PinWright files are never read, written or retracted.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "PinWrightRuntimeSubsystem.h"
#include "Utils/GatewayPortFile.h"
#include "Utils/InstanceFile.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Transport/SocketTestClient.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <aclapi.h>
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_LINUX
#include <sys/stat.h>
#endif

namespace PinWrightRuntimeActivationTest
{
    // Stands in for <ProjectSaved>/PinWright for the instance file and gateway-port.
    struct FScopedPinWrightRoot
    {
        FString Root;

        FScopedPinWrightRoot()
        {
            Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
                TEXT("PinWright/TestTemp") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
            FPaths::NormalizeDirectoryName(Root);
            PinWrightInstanceFile::SetRootOverrideForTests(Root);
            GatewayPortFile::SetRootOverrideForTests(Root);
        }

        ~FScopedPinWrightRoot()
        {
            PinWrightInstanceFile::SetRootOverrideForTests(FString());
            GatewayPortFile::SetRootOverrideForTests(FString());
            IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
        }

        // The contract's location, spelled independently of the code under test.
        FString InstanceFilePath() const
        {
            return Root / TEXT("instances") /
                FString::Printf(TEXT("%u.json"), FPlatformProcess::GetCurrentProcessId());
        }

        FString GatewayPortPath() const { return Root / TEXT("gateway-port"); }
    };

    // A never-Initialize()d subsystem activated with a canned command line; deactivated (the
    // clean-shutdown path) when the scope ends.
    struct FScopedActivation
    {
        TStrongObjectPtr<UPinWrightRuntimeSubsystem> Subsystem;
        bool bServing = false;

        FScopedActivation(const TCHAR* CommandLine, bool bGameProcess)
            : Subsystem(NewObject<UPinWrightRuntimeSubsystem>())
        {
            bServing = Subsystem->ActivateForTesting(CommandLine, bGameProcess);
        }

        ~FScopedActivation() { Subsystem->DeactivateForTesting(); }

        int32 Port() const { return Subsystem->GetBoundHttpPort(); }
    };

    TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path))
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Obj;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Obj);
        return Obj;
    }

    bool IsLowercaseHex64(const FString& Token)
    {
        if (Token.Len() != 64)
        {
            return false;
        }
        for (const TCHAR C : Token)
        {
            if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'))))
            {
                return false;
            }
        }
        return true;
    }

    // HTTP status of a ping POSTed to 127.0.0.1:Port/mcp, or -1 when no response arrived.
    int32 PostPing(int32 Port, const FString& AuthorizationValue)
    {
        using namespace PinWrightSocketTest;
        FSocketTestClient Client;
        if (!Client.Connect(static_cast<uint32>(Port)))
        {
            return -1;
        }
        TArray<TPair<FString, FString>> Headers;
        Headers.Add(TPair<FString, FString>(TEXT("Content-Type"), TEXT("application/json")));
        if (!AuthorizationValue.IsEmpty())
        {
            Headers.Add(TPair<FString, FString>(TEXT("Authorization"), AuthorizationValue));
        }
        if (!Client.SendString(BuildHttpRequest(TEXT("POST"), TEXT("/mcp"), Headers, BuildPingBody(1)),
                               nullptr))
        {
            return -1;
        }
        FParsedHttpResponse Response;
        int32 NextOffset = 0;
        return RecvHttpResponse(Client, nullptr, Response, NextOffset) ? Response.Code : -1;
    }

    // Empty when only the owner can reach Path, else what grants wider access.
    FString DescribeBroaderThanUserAccess(const FString& Path)
    {
#if PLATFORM_WINDOWS
        PACL Dacl = nullptr;
        PSECURITY_DESCRIPTOR Descriptor = nullptr;
        if (GetNamedSecurityInfoW(*Path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                  &Dacl, nullptr, &Descriptor) != ERROR_SUCCESS)
        {
            return TEXT("the DACL could not be read");
        }
        ON_SCOPE_EXIT { LocalFree(Descriptor); };
        if (!Dacl)
        {
            return TEXT("a null DACL grants everyone full access");
        }
        LPWSTR Sddl = nullptr;
        if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(Descriptor, SDDL_REVISION_1,
                DACL_SECURITY_INFORMATION, &Sddl, nullptr))
        {
            return TEXT("the DACL could not be rendered as SDDL");
        }
        const FString Text(Sddl);
        LocalFree(Sddl);
        // Everyone, Authenticated Users, Users, Guests: any ACE for them, inherited or explicit,
        // lets another account on the machine read the token.
        for (const TCHAR* Group : { TEXT(";;;WD)"), TEXT(";;;AU)"), TEXT(";;;BU)"), TEXT(";;;BG)") })
        {
            if (Text.Contains(Group))
            {
                return FString::Printf(TEXT("DACL %s grants a shared group (%s)"), *Text, Group);
            }
        }
        return FString();
#elif PLATFORM_LINUX
        struct stat Info;
        if (stat(TCHAR_TO_UTF8(*Path), &Info) != 0)
        {
            return TEXT("stat failed");
        }
        const int32 Mode = static_cast<int32>(Info.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));
        return (Mode & (S_IRWXG | S_IRWXO)) == 0
            ? FString()
            : FString::Printf(TEXT("mode %o grants group or other access"), Mode);
#else
        return FString();
#endif
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeActivationGameWithoutSwitchTest,
    "PinWright.transport.runtime_activation.GameWithoutSwitchDoesNotBind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRuntimeActivationGameWithoutSwitchTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const FScopedPinWrightRoot Root;

    // Plain -game, plus switches that share the prefix or opt an editor test run in, none of which
    // is the game opt-in.
    for (const TCHAR* CommandLine : {
             TEXT("X.uproject -game"),
             TEXT("X.uproject -game -PinWrightLaunchReason=probe"),
             TEXT("X.uproject -game -PinWrightTransport"),
             TEXT("X.uproject -game -PinWrightX") })
    {
        const FScopedActivation Activation(CommandLine, /*bGameProcess=*/true);
        TestFalse(FString::Printf(TEXT("[%s] does not serve"), CommandLine), Activation.bServing);
        TestEqual(FString::Printf(TEXT("[%s] binds no port"), CommandLine), Activation.Port(), 0);
        TestFalse(FString::Printf(TEXT("[%s] writes no instance file"), CommandLine),
                  FPaths::FileExists(Root.InstanceFilePath()));
        TestFalse(FString::Printf(TEXT("[%s] writes no gateway-port"), CommandLine),
                  FPaths::FileExists(Root.GatewayPortPath()));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeActivationGamePortZeroTest,
    "PinWright.transport.runtime_activation.GamePortZeroBindsEphemeral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRuntimeActivationGamePortZeroTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const FScopedPinWrightRoot Root;
    const FScopedActivation Activation(TEXT("X.uproject -game -PinWrightPort=0"), /*bGameProcess=*/true);

    TestTrue(TEXT("-PinWrightPort=0 serves in a game process"), Activation.bServing);
    TestTrue(TEXT("the OS picked a real port"), Activation.Port() > 0);
    // The ephemeral port is known only after the bind, so a matching file proves write-after-bind.
    const TSharedPtr<FJsonObject> Instance = ReadJsonFile(Root.InstanceFilePath());
    if (!TestTrue(TEXT("instance file exists and parses"), Instance.IsValid()))
    {
        return false;
    }
    int32 FilePort = 0;
    TestTrue(TEXT("instance file names the bound port"),
             Instance->TryGetNumberField(TEXT("port"), FilePort) && FilePort == Activation.Port());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeActivationGameBareSwitchTest,
    "PinWright.transport.runtime_activation.GameBareSwitchBinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRuntimeActivationGameBareSwitchTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const FScopedPinWrightRoot Root;
    // While this suite runs the editor holds the project's derived port, so a bare -PinWright game
    // that tried it would lose the bind: a second instance must not need a free derived port.
    const FScopedActivation Activation(TEXT("X.uproject -game -PinWright"), /*bGameProcess=*/true);
    TestTrue(TEXT("-PinWright serves in a game process"), Activation.bServing);
    TestTrue(TEXT("it bound a port"), Activation.Port() > 0);
    TestTrue(TEXT("it wrote its instance file"), FPaths::FileExists(Root.InstanceFilePath()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeActivationGatewayPortUntouchedTest,
    "PinWright.transport.runtime_activation.PortSwitchNeverPublishesGatewayPort",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRuntimeActivationGatewayPortUntouchedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const FScopedPinWrightRoot Root;

    // gateway-port stays the primary editor's: neither a second editor nor a game started with
    // -PinWrightPort may write it, or the MCP proxy would follow it away from the primary.
    for (const bool bGameProcess : { false, true })
    {
        const TCHAR* Kind = bGameProcess ? TEXT("game") : TEXT("editor");
        const FScopedActivation Activation(
            bGameProcess ? TEXT("X.uproject -game -PinWrightPort=0") : TEXT("X.uproject -PinWrightPort=0"),
            bGameProcess);
        TestTrue(FString::Printf(TEXT("%s with -PinWrightPort=0 serves"), Kind), Activation.bServing);
        TestFalse(FString::Printf(TEXT("%s with -PinWrightPort=0 writes no gateway-port"), Kind),
                  FPaths::FileExists(Root.GatewayPortPath()));
        const TSharedPtr<FJsonObject> Instance = ReadJsonFile(Root.InstanceFilePath());
        FString FileKind;
        TestTrue(FString::Printf(TEXT("%s instance file says kind=%s"), Kind, Kind),
                 Instance.IsValid() && Instance->TryGetStringField(TEXT("kind"), FileKind) && FileKind == Kind);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceFileSchemaTest,
    "PinWright.transport.instance_file.WrittenAfterBindWithContractSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstanceFileSchemaTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const FScopedPinWrightRoot Root;
    TestTrue(TEXT("GetPath() is <root>/instances/<pid>.json"),
             FPaths::IsSamePath(PinWrightInstanceFile::GetPath(), Root.InstanceFilePath()));

    const FScopedActivation Activation(
        TEXT("X.uproject -game -PinWrightPort=0 -PinWrightLaunchReason=instance-file-test"),
        /*bGameProcess=*/true);
    if (!TestTrue(TEXT("fixture serves"), Activation.bServing))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Instance = ReadJsonFile(Root.InstanceFilePath());
    if (!TestTrue(TEXT("instance file exists and parses"), Instance.IsValid()))
    {
        return false;
    }

    int64 Pid = 0;
    TestTrue(TEXT("pid is this process"), Instance->TryGetNumberField(TEXT("pid"), Pid)
        && Pid == static_cast<int64>(FPlatformProcess::GetCurrentProcessId()));
    int32 Port = 0;
    TestTrue(TEXT("port is the bound port"),
             Instance->TryGetNumberField(TEXT("port"), Port) && Port == Activation.Port());
    FString Token;
    TestTrue(TEXT("token is 32 random bytes as lowercase hex"),
             Instance->TryGetStringField(TEXT("token"), Token) && IsLowercaseHex64(Token));
    FString Kind;
    TestTrue(TEXT("kind is game"), Instance->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("game"));
    FString Project;
    TestTrue(TEXT("project is this .uproject, absolute"),
             Instance->TryGetStringField(TEXT("project"), Project)
             && !FPaths::IsRelative(Project)
             && FPaths::IsSamePath(Project, FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())));
    // The process start or the bind time, either way not in the future and not from a previous day.
    FString StartTimeText;
    FDateTime StartTime;
    const FDateTime Now = FDateTime::UtcNow();
    const bool bStartTimeOk = Instance->TryGetStringField(TEXT("startTime"), StartTimeText)
        && FDateTime::ParseIso8601(*StartTimeText, StartTime)
        && StartTime >= Now - FTimespan::FromDays(1.0)
        && StartTime <= Now + FTimespan::FromSeconds(5.0);
    TestTrue(FString::Printf(TEXT("startTime is ISO 8601 UTC from this process (%s)"), *StartTimeText),
             bStartTimeOk);
    FString Reason;
    TestTrue(TEXT("reason is the -PinWrightLaunchReason value"),
             Instance->TryGetStringField(TEXT("reason"), Reason) && Reason == TEXT("instance-file-test"));
    FString Map;
    TestTrue(TEXT("map is present (a string, possibly empty)"), Instance->TryGetStringField(TEXT("map"), Map));
    FString Engine;
    TestTrue(TEXT("engine is <major>.<minor>"), Instance->TryGetStringField(TEXT("engine"), Engine)
        && Engine == FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceFilePermissionsTest,
    "PinWright.transport.instance_file.UserOnlyPermissions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstanceFilePermissionsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
#if !(PLATFORM_WINDOWS || PLATFORM_LINUX)
    PinWrightTestSkip::SkipAssertions(*this, TEXT("no_permission_probe"),
        TEXT("no file-permission probe for this platform."));
    return true;
#else
    const FScopedPinWrightRoot Root;
    const FScopedActivation Activation(TEXT("X.uproject -game -PinWrightPort=0"), /*bGameProcess=*/true);
    if (!TestTrue(TEXT("instance file written"), Activation.bServing && FPaths::FileExists(Root.InstanceFilePath())))
    {
        return false;
    }
    // The file carries a bearer token: another account on the machine must not be able to read it.
    const FString Broader = DescribeBroaderThanUserAccess(Root.InstanceFilePath());
    TestTrue(FString::Printf(TEXT("instance file is user-only (%s)"), *Broader), Broader.IsEmpty());
    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceFileDeletedOnShutdownTest,
    "PinWright.transport.instance_file.DeletedOnCleanShutdown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstanceFileDeletedOnShutdownTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const FScopedPinWrightRoot Root;
    {
        const FScopedActivation Activation(TEXT("X.uproject -game -PinWrightPort=0"), /*bGameProcess=*/true);
        if (!TestTrue(TEXT("instance file written while serving"),
                      Activation.bServing && FPaths::FileExists(Root.InstanceFilePath())))
        {
            return false;
        }
    }
    TestFalse(TEXT("clean shutdown deletes the instance file"), FPaths::FileExists(Root.InstanceFilePath()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstanceTokenAuthorizesTest,
    "PinWright.transport.instance_token.InstanceTokenAuthorizes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstanceTokenAuthorizesTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRuntimeActivationTest;
    const PinWrightSocketTest::FScopedAuthRequirement RequireAuth(true);
    const FScopedPinWrightRoot Root;
    const FScopedActivation Activation(TEXT("X.uproject -game -PinWrightPort=0"), /*bGameProcess=*/true);
    if (!TestTrue(TEXT("fixture serves"), Activation.bServing && Activation.Port() > 0))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Instance = ReadJsonFile(Root.InstanceFilePath());
    FString Token;
    if (!TestTrue(TEXT("instance file carries a token"),
                  Instance.IsValid() && Instance->TryGetStringField(TEXT("token"), Token) && !Token.IsEmpty()))
    {
        return false;
    }

    TestEqual(TEXT("the instance-file token authorizes"),
              PostPing(Activation.Port(), TEXT("Bearer ") + Token), 200);
    // Same length, last character changed: the comparison is over the whole value.
    FString Wrong = Token;
    Wrong[Wrong.Len() - 1] = Wrong[Wrong.Len() - 1] == TEXT('0') ? TEXT('1') : TEXT('0');
    TestEqual(TEXT("a wrong token is refused"), PostPing(Activation.Port(), TEXT("Bearer ") + Wrong), 401);
    TestEqual(TEXT("no Authorization header is refused"), PostPing(Activation.Port(), FString()), 401);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
