// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "AGIR/AGIROpcodes.h"

// Line-oriented parser for AGIR (AnimGraph IR) text. Mirrors `FMGIRParser` /
// `FBpirParser` in shape but adds a brace-stack so nested blocks
// (`state_machine { state { ... } transition ... }` etc.) collapse into the
// `FAGIRInstruction::Children` tree.
//
// Pure text processing — no UEdGraph / UAnimBlueprint dependencies. The
// `bSkipReferenceValidation` flag is reserved for future cross-reference
// validation (e.g. transition `from`/`to` referencing a declared `state`); it
// is parsed and stored on the API for parity with `FBpirParser::Parse` but
// not yet consumed.
class PINWRIGHT_API FAGIRParser
{
public:
    static bool Parse(
        FStringView Code,
        TArray<FAGIREntryBlock>& OutBlocks,
        TArray<FAGIRParseError>& OutErrors,
        bool bSkipReferenceValidation = false);
};
