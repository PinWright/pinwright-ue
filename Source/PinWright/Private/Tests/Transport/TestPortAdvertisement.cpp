// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pins the gateway-port advertisement contract (Transport/PortAdvertisement.h).
//
// THE DEFECT THESE PIN. The port file was written on a successful bind and never retracted,
// on a stated "last-known-good survives failures" policy. An editor whose bind lost therefore
// kept advertising a port from an earlier session while serving nothing, and the stdio proxy
// re-resolves the endpoint from that file before every call - so a caller was aimed at a dead
// endpoint by a live editor that knew it was not serving. The retry schedule
// (Tests/Transport/TestBindRetryPolicy.cpp) fixes the editor half; nothing pinned this half.
//
// The failure direction is deliberately BOTH ways, because the obvious fix is wrong. Deleting
// the file on every failed bind breaks the normal multi-editor case, where a second editor
// loses the bind precisely because the first one holds that port and is serving on it.
// RetainsAnAdvertisementSomethingIsServing fails if anyone "simplifies" the reconcile into an
// unconditional delete; RetractsAnAdvertisementNothingIsServing fails if it goes back to
// leaving the stale claim alone.

#include "Misc/AutomationTest.h"

#include "Transport/PortAdvertisement.h"
#include "Utils/GatewayPortFile.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightPortAdvertisementTest
{
    // Unique per-test scratch dir, matching Tests/Infra/TestGatewayPortFile.cpp so two tests
    // running in the same editor never share an advertisement.
    FString MakeTestRoot()
    {
        FString Root = FPaths::ProjectSavedDir() /
            TEXT("PinWright/TestTemp") /
            FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }

    // A real loopback listener, so "something is listening" is a fact rather than a stub.
    // Binds port 0 and reports what the OS handed out; the caller owns Close().
    struct FLoopbackListener
    {
        ISocketSubsystem* Sub = nullptr;
        FSocket* Socket = nullptr;
        int32 Port = 0;

        bool Open()
        {
            Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            if (!Sub)
            {
                return false;
            }
            Socket = Sub->CreateSocket(NAME_Stream, TEXT("PinWrightAdvertisementTestListener"), false);
            if (!Socket)
            {
                return false;
            }
            const TSharedRef<FInternetAddr> Addr = Sub->CreateInternetAddr();
            Addr->SetLoopbackAddress();
            Addr->SetPort(0); // Ephemeral: never collides with a real editor's port.
            if (!Socket->Bind(*Addr) || !Socket->Listen(4))
            {
                Close();
                return false;
            }
            Port = Socket->GetPortNo();
            return Port != 0;
        }

        void Close()
        {
            if (Socket && Sub)
            {
                Socket->Close();
                Sub->DestroySocket(Socket);
            }
            Socket = nullptr;
        }
    };

    // A port that is provably not being served: bind one, read it back, then release it.
    // Reusing a just-released ephemeral port is safe here because the probe only has to
    // observe that nothing accepts a connection on it.
    bool FindUnservedPort(int32& OutPort)
    {
        FLoopbackListener Listener;
        if (!Listener.Open())
        {
            return false;
        }
        OutPort = Listener.Port;
        Listener.Close();
        return true;
    }
}

// ---------------------------------------------------------------------------
// The pure verdict. No socket, no file - the decision itself is what regressed.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortAdvertisementVerdictTest,
    "PinWright.transport.port_advertisement.VerdictRetractsOnlyWhatIsProvablyDead",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPortAdvertisementVerdictTest::RunTest(const FString& Parameters)
{
    using namespace PortAdvertisement;

    TestTrue(TEXT("no advertisement is no claim"),
             Judge(/*bHasClaim=*/false, /*bSomethingListening=*/false) == EVerdict::NoClaim);
    // Even a "listening" reading is irrelevant when nothing is advertised.
    TestTrue(TEXT("no advertisement stays no claim regardless of the probe"),
             Judge(/*bHasClaim=*/false, /*bSomethingListening=*/true) == EVerdict::NoClaim);

    // The whole point of the split: an advertised port that something is serving belongs to
    // whoever is serving it, and is not ours to delete just because our own bind lost.
    TestTrue(TEXT("a served advertisement is retained"),
             Judge(/*bHasClaim=*/true, /*bSomethingListening=*/true) == EVerdict::Retain);

    TestTrue(TEXT("an unserved advertisement is retracted"),
             Judge(/*bHasClaim=*/true, /*bSomethingListening=*/false) == EVerdict::Retract);

    return true;
}

// ---------------------------------------------------------------------------
// The probe that supplies the verdict's only input, against real sockets.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortAdvertisementProbeTest,
    "PinWright.transport.port_advertisement.ProbeSeparatesAListenerFromAnEmptyPort",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPortAdvertisementProbeTest::RunTest(const FString& Parameters)
{
    PinWrightPortAdvertisementTest::FLoopbackListener Listener;
    if (!Listener.Open())
    {
        // No socket subsystem at all: the probe's documented fail-safe is "report listening",
        // which is what keeps an inconclusive answer from deleting a live editor's file.
        TestTrue(TEXT("an unprobeable port is reported as listening, never as dead"),
                 PortAdvertisement::IsSomethingListening(19880));
        return true;
    }

    TestTrue(TEXT("a bound listener is detected"),
             PortAdvertisement::IsSomethingListening(Listener.Port));
    const int32 ServedPort = Listener.Port;
    Listener.Close();

    // Same port, no listener. This is the reading that authorises a retraction, so it has to
    // flip - a probe that always said "listening" would make the retraction unreachable.
    TestFalse(TEXT("the same port reads as dead once the listener is gone"),
              PortAdvertisement::IsSomethingListening(ServedPort));

    TestFalse(TEXT("port 0 is not a port and is never reported as listening"),
              PortAdvertisement::IsSomethingListening(0));

    return true;
}

// ---------------------------------------------------------------------------
// End to end against the real file: what a caller resolving the endpoint sees.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortAdvertisementRetractsDeadTest,
    "PinWright.transport.port_advertisement.RetractsAnAdvertisementNothingIsServing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPortAdvertisementRetractsDeadTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightPortAdvertisementTest::MakeTestRoot();
    GatewayPortFile::SetRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    int32 DeadPort = 0;
    if (!PinWrightPortAdvertisementTest::FindUnservedPort(DeadPort))
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no socket subsystem, so no port could be "
                        "proven unserved and the retraction path was not exercised."));
        return true;
    }

    TestTrue(TEXT("a stale advertisement is on disk"), GatewayPortFile::WritePortFile(DeadPort));
    int32 ReadBack = 0;
    TestTrue(TEXT("and it reads back"), GatewayPortFile::ReadPortFile(ReadBack));
    TestEqual(TEXT("as the port written"), ReadBack, DeadPort);

    // Retraction is a corrective action for a stale, unreachable endpoint, but the runtime keeps
    // it at Error severity so the event remains visible outside tests. Consume only this exact
    // expected diagnostic; the assertions below still verify the actual verdict and file state.
    // AddExpectedErrorPlain is exactly this call: the engine implements it as
    // AddExpectedError(..., IsRegex=false). Spelling it as the Plain variant keeps 5.4+ behaviour
    // byte-identical while picking up the 5.3 shim in Compat/EngineVersionCompat.h, since the
    // IsRegex parameter itself does not exist before 5.4.
    AddExpectedErrorPlain(TEXT("Retracting a false MCP advertisement"),
                          EAutomationExpectedErrorFlags::Contains, 1);
    TestTrue(TEXT("a claim nothing serves is judged retractable"),
             PortAdvertisement::ReconcileWhileNotServing() == PortAdvertisement::EVerdict::Retract);

    // The assertion that matters to a caller: nothing points at the dead endpoint any more.
    TestFalse(TEXT("the advertisement is gone"),
              IFileManager::Get().FileExists(*GatewayPortFile::GetPortFilePath()));
    int32 Unused = 0;
    TestFalse(TEXT("and reads as absent, not as a port"), GatewayPortFile::ReadPortFile(Unused));

    // Reconciling again with nothing advertised must be a no-op, not an error.
    TestTrue(TEXT("a second reconcile finds no claim"),
             PortAdvertisement::ReconcileWhileNotServing() == PortAdvertisement::EVerdict::NoClaim);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortAdvertisementRetainsLiveTest,
    "PinWright.transport.port_advertisement.RetainsAnAdvertisementSomethingIsServing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPortAdvertisementRetainsLiveTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightPortAdvertisementTest::MakeTestRoot();
    GatewayPortFile::SetRootOverrideForTests(Root);
    PinWrightPortAdvertisementTest::FLoopbackListener Listener;
    ON_SCOPE_EXIT
    {
        Listener.Close();
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    if (!Listener.Open())
    {
        AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: no socket subsystem, so no live listener "
                        "could stand in for a second editor and the retain path was not exercised."));
        return true;
    }

    // Stands in for the normal multi-editor case: this editor lost the bind because another
    // editor of the same project holds that port and is serving callers on it.
    TestTrue(TEXT("the served port is advertised"), GatewayPortFile::WritePortFile(Listener.Port));

    TestTrue(TEXT("a served claim is retained"),
             PortAdvertisement::ReconcileWhileNotServing() == PortAdvertisement::EVerdict::Retain);

    int32 StillAdvertised = 0;
    TestTrue(TEXT("the advertisement survives"), GatewayPortFile::ReadPortFile(StillAdvertised));
    TestEqual(TEXT("naming the same port"), StillAdvertised, Listener.Port);

    return true;
}

// ---------------------------------------------------------------------------
// Reading back an advertisement, including the shapes that are not one.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortAdvertisementReadRejectsJunkTest,
    "PinWright.transport.port_advertisement.UnreadableClaimIsNoClaim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPortAdvertisementReadRejectsJunkTest::RunTest(const FString& Parameters)
{
    const FString Root = PinWrightPortAdvertisementTest::MakeTestRoot();
    GatewayPortFile::SetRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        GatewayPortFile::SetRootOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString Path = GatewayPortFile::GetPortFilePath();
    int32 Port = 0;

    TestFalse(TEXT("an absent file is not a claim"), GatewayPortFile::ReadPortFile(Port));

    // Every one of these would resolve to a bogus endpoint if it were accepted, and the
    // retraction would then delete a file it never understood.
    const TArray<FString> NotPorts = {
        TEXT(""), TEXT("   "), TEXT("0"), TEXT("65536"), TEXT("-1"),
        TEXT("http://127.0.0.1:19880"), TEXT("19880 19881"), TEXT("1988a")
    };
    for (const FString& Junk : NotPorts)
    {
        FFileHelper::SaveStringToFile(Junk, *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        Port = -1;
        TestFalse(*FString::Printf(TEXT("'%s' is not a port"), *Junk),
                  GatewayPortFile::ReadPortFile(Port));
    }

    // Surrounding whitespace is what a hand-edited or newline-terminated file looks like, and
    // is the one non-canonical shape that must still resolve.
    FFileHelper::SaveStringToFile(TEXT("  24281\r\n"), *Path,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    TestTrue(TEXT("a padded port still reads"), GatewayPortFile::ReadPortFile(Port));
    TestEqual(TEXT("as its value"), Port, 24281);

    TestTrue(TEXT("retraction removes it"), GatewayPortFile::RemovePortFile());
    TestFalse(TEXT("and the file is gone"), IFileManager::Get().FileExists(*Path));
    TestTrue(TEXT("retracting nothing succeeds"), GatewayPortFile::RemovePortFile());

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
