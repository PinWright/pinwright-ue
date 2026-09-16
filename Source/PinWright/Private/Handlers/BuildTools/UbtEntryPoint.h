// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

// Resolves the Unreal Build Tool entry-point script for system.run_ubt.
//
// There is NO RunUBT.bat in the Windows engine layout (only RunUBT.sh ships,
// for Mac/Linux). Hardcoding RunUBT.bat made the UBT spawn hand CreateProc a
// path to a nonexistent file, so every Windows invocation died CREATEPROC_FAILED
// before any platform validation (see B-pipeline-run-ubt-bad-exe-path, filed
// against the since-removed pipeline.run_ubt twin of this handler). The real
// cross-platform forwarding wrapper is Build.bat on Windows / Build.sh elsewhere.
// This is a free function so both the handler and its regression test exercise
// the identical resolution rather than a copy.
namespace EARG_Ubt
{
    inline FString ResolveUbtEntryPoint()
    {
#if PLATFORM_WINDOWS
        const FString UbtScript = TEXT("Build/BatchFiles/Build.bat");
#else
        const FString UbtScript = TEXT("Build/BatchFiles/Build.sh");
#endif
        return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / UbtScript);
    }
}
