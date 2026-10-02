// Copyright (c) 2026 Alexander Penkin. MIT License.

// Per-script compile readback shared by niagara.create_module_script and
// niagara.get_compiled_script. Everything here reads FNiagaraVMExecutableData as the last compile
// left it; nothing compiles.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class UNiagaraScript;

namespace PinWrightNiagara
{
    // Wire spelling of the script's LastCompileStatus: "succeeded", "succeededWithWarnings",
    // "failed", "dirty", "compiling", or "notCompiled" (NCS_Unknown, the engine's pre-compile
    // sentinel). Never claims success for a non-terminal status.
    FString DescribeScriptCompileStatus(const UNiagaraScript& Script);

    // LastCompileEvents as [{severity, message, nodeGuid?, pinGuid?}]. nodeGuid / pinGuid are
    // present only when the engine attributed the event; an HLSL error raised by the VM backend
    // compiler carries neither (NiagaraCompiler.cpp adds those events with bare text). When the
    // script failed with no events, ErrorMsg is reported as one error so a failure is never empty.
    TArray<TSharedPtr<FJsonValue>> BuildScriptCompileEventsJson(const UNiagaraScript& Script, bool bErrorsOnly);
}
