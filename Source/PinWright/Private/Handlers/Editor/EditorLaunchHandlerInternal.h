// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"

// Pure helper that builds the per-process command line for editor.launch_standalone.
// Exposed (DLL-exported) so unit tests in a separate TU can link to it directly
// without re-implementing the formatting logic.
PINWRIGHT_API FString BuildStandaloneCommandLine(
    const FString& ProjectPath, const FString& Map, int32 InstanceIndex,
    int32 NumClients, bool bListenServer, const FString& ExtraArgs);
