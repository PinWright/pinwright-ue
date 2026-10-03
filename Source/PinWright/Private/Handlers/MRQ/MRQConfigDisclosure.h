// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/MRQ/MRQArtifactReport.h"

class UMoviePipelinePrimaryConfig;

// Config readers shared by the queue verbs (MRQHandler.cpp) and the preset authoring verbs
// (MRQPresetHandler.cpp), so a preset written by mrq.create_preset is described in exactly the
// words mrq.create_job later uses for the job that consumes it. Defined in MRQHandler.cpp, and
// only when the engine's MovieRenderPipeline headers are available (MCP_HAS_MRQ there); every
// caller's gate requires at least MCP_HAS_MRQ's headers.
namespace PinWrightMRQ
{
    // Everything the `preflight` block discloses, read off a resolved configuration. A null
    // config yields an empty context.
    FPreflightContext ReadPreflightContext(const UMoviePipelinePrimaryConfig* Config);

    // The output-directory check mrq.create_job applies before queueing ({project_dir} expanded,
    // no traversal, nearest existing ancestor writable). False with OutError set on refusal.
    bool ValidateOutputDirectory(const FString& RequestedPath, FString& OutError);

    // The asset-path check mrq.create_job applies to sequencePath/levelPath/presetPath: a valid
    // long package/object path on a mounted root. False with OutError set on refusal.
    bool ValidateAssetPath(const FString& RequestedPath, const TCHAR* FieldName, FString& OutError);
}
