// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins B-port-in-use-status-unreachable: a startup bind that loses its port must report
// EMcpServerStatus::PortInUse, not Disabled. The FMcpTransport -> FSocketHttpServer rewrite
// once dropped the conflict wire and GetServerStatus() could only say Disabled/Listening,
// which left the setup screen's red conflict banner and the module's show-on-problem gate
// dead. Nothing went red because the enum's only coverage was a UI branch; this test holds
// a real port, runs the real startup bind against it, and reads the status back.

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "PinWrightSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Utils/GatewayPortFile.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServerStatusHeldPortReportsPortInUseTest,
    "PinWright.transport.bind.Status.HeldPortReportsPortInUse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FServerStatusHeldPortReportsPortInUseTest::RunTest(const FString& Parameters)
{
    ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sub)
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no socket subsystem, so no port could be "
                        "held and the bind-conflict status was not exercised."));
        return true;
    }

    // Hold an ephemeral loopback port (never a real editor's) the way a second editor would.
    FSocket* Holder = Sub->CreateSocket(NAME_Stream, TEXT("PinWrightPortInUseTestHolder"), false);
    if (!TestNotNull(TEXT("holder socket created"), Holder))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        Holder->Close();
        Sub->DestroySocket(Holder);
    };
    const TSharedRef<FInternetAddr> Addr = Sub->CreateInternetAddr();
    Addr->SetLoopbackAddress();
    Addr->SetPort(0);
    if (!TestTrue(TEXT("holder bound and listening (fixture precondition)"),
                  Holder->Bind(*Addr) && Holder->Listen(4)))
    {
        return false;
    }
    const int32 HeldPort = Holder->GetPortNo();
    if (!TestTrue(TEXT("holder reports its port"), HeldPort > 0))
    {
        return false;
    }

    UPinWrightSubsystem* Subsystem = NewObject<UPinWrightSubsystem>();
    if (!TestNotNull(TEXT("subsystem fixture"), Subsystem))
    {
        return false;
    }

    // A lost bind reconciles the gateway-port advertisement. Point it at an empty private root
    // so the test never reads, retracts or races the real Saved/PinWright/gateway-port file;
    // with nothing advertised the reconcile is a Log-level no-op.
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
        TEXT("PinWright/TestTemp") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
    GatewayPortFile::SetRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    // The runtime keeps the lost bind at Error severity; consume exactly those diagnostics.
    AddExpectedErrorPlain(TEXT("could not be bound"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedErrorPlain(TEXT("Transport failed to bind port"),
                          EAutomationExpectedErrorFlags::Contains, 1);

    TestFalse(TEXT("the bind against a held port fails"),
              Subsystem->StartTransportForTesting(HeldPort));
    const EMcpServerStatus Status = Subsystem->GetServerStatus();
    TestTrue(FString::Printf(TEXT("a lost bind reports PortInUse, not Disabled (got %s)"),
                             *UEnum::GetValueAsString(Status)),
             Status == EMcpServerStatus::PortInUse);
    TestEqual(TEXT("the contested port is the held one"),
              Subsystem->GetContestedHttpPort(), HeldPort);
    TestFalse(TEXT("the bridge is not active"), Subsystem->IsBridgeActive());
    TestEqual(TEXT("no port is reported as bound"), Subsystem->GetBoundHttpPort(), 0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
