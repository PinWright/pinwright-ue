// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace GatewayPortFile
{
    // <root>/gateway-port, absolute + normalized. Root is <ProjectSavedDir>/PinWright.
    FString GetPortFilePath();

    // Writes the decimal port (no trailing newline, UTF-8 without BOM) atomically
    // via tmp-then-move. Returns false on any failure.
    //
    // The return value is NOT advisory. Publication is half of serving: a bound server
    // whose port never reached this file is unreachable by every proxy, which re-resolves
    // the endpoint from it before each call. Callers must treat false as "not serving yet".
    bool WritePortFile(int32 Port);

    // Reads the currently advertised port. False when the file is absent, unreadable or
    // does not hold a port in [1, 65535]; OutPort is only written on true.
    bool ReadPortFile(int32& OutPort);

    // Deletes the advertisement. Used to retract a claim this project's editors are
    // provably not honouring - see Transport/PortAdvertisement.h for who may call it and
    // why "provably" is doing the work in that sentence. True when the file is gone
    // afterwards, including when it was already absent.
    bool RemovePortFile();

#if WITH_DEV_AUTOMATION_TESTS
    // Redirects the port-file root for tests; an empty string clears the override.
    void SetRootOverrideForTests(const FString& Root);
#endif
}
