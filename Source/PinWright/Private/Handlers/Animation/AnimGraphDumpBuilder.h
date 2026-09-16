// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UAnimBlueprint;

namespace AnimGraphDumpBuilder
{
    // Phase 1 read-only aspect for asset.dump on UAnimBlueprint.
    // Returns a JSON object with three top-level arrays:
    //   - pages[]            { name, guid, kind, parent }
    //   - state_machines[]   { name, page, states[], transitions[], conduits[] }
    //   - anim_node_classes[] { class, count }
    // Caller writes via the standard SortedJsonWriter path; keys are alphabetised on serialize.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildAnimGraphJson(UAnimBlueprint* AnimBP);
}
