// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Transport/PortAdvertisement.h"

#include "Utils/GatewayPortFile.h"

#include "Misc/ScopeExit.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

DEFINE_LOG_CATEGORY_STATIC(LogPortAdvertisement, Log, All);

namespace PortAdvertisement
{

bool IsSomethingListening(int32 Port, double TimeoutSeconds)
{
    static_cast<void>(TimeoutSeconds);

    if (Port < 1 || Port > 65535)
    {
        return false;
    }

    ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Sub)
    {
        // Inconclusive, and the verdict only retracts on proof.
        return true;
    }

    FSocket* Probe = Sub->CreateSocket(NAME_Stream, TEXT("PinWrightPortAdvertisementProbe"), false);
    if (!Probe)
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        Probe->Close();
        Sub->DestroySocket(Probe);
    };

    const TSharedRef<FInternetAddr> Addr = Sub->CreateInternetAddr();
    Addr->SetLoopbackAddress();
    Addr->SetPort(Port);

    // Binding the exact loopback endpoint is a direct occupancy check. A successful bind proves
    // that no TCP listener owns the advertised port at this instant. Any bind failure is
    // deliberately inconclusive: it may mean a listener is present, or it may be an unrelated
    // socket error, and either case must retain the advertisement. A non-blocking Connect followed
    // by GetConnectionState cannot provide this distinction on the Windows socket backend: a
    // refused connect remains EWOULDBLOCK/SCS_NotConnected on this engine build until the timeout.
    return !Probe->Bind(*Addr);
}

EVerdict ReconcileWhileNotServing()
{
    int32 Advertised = 0;
    const bool bHasClaim = GatewayPortFile::ReadPortFile(Advertised);
    const bool bListening = bHasClaim && IsSomethingListening(Advertised);

    const EVerdict Verdict = Judge(bHasClaim, bListening);
    switch (Verdict)
    {
    case EVerdict::NoClaim:
        UE_LOG(LogPortAdvertisement, Log,
               TEXT("This editor is not serving MCP and nothing is advertised in %s, so no caller ")
               TEXT("is being pointed anywhere. Nothing to retract."),
               *GatewayPortFile::GetPortFilePath());
        break;

    case EVerdict::Retain:
        UE_LOG(LogPortAdvertisement, Warning,
               TEXT("This editor is not serving MCP, but %s advertises port %d and something IS ")
               TEXT("listening there - most likely another editor of this project, which would be ")
               TEXT("serving callers correctly. The advertisement is left alone: deleting it would ")
               TEXT("break that editor. Callers reaching port %d are NOT reaching this process."),
               *GatewayPortFile::GetPortFilePath(), Advertised, Advertised);
        break;

    case EVerdict::Retract:
        UE_LOG(LogPortAdvertisement, Error,
               TEXT("Retracting a false MCP advertisement: %s named port %d, nothing is listening ")
               TEXT("there, and this editor did not bind either - so every proxy call was being ")
               TEXT("aimed at a dead endpoint. The file is now removed, which makes callers report ")
               TEXT("'editor not running' instead of a connection error against a port nobody owns."),
               *GatewayPortFile::GetPortFilePath(), Advertised);
        GatewayPortFile::RemovePortFile();
        break;
    }
    return Verdict;
}

} // namespace PortAdvertisement
