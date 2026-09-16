// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UStateTree;

namespace StateTreeDumpBuilder
{
    // Walks UStateTree::EditorData (UStateTreeEditorData) and serializes the authored
    // topology — state hierarchy (SubTrees -> Children), per-state EnterConditions /
    // Tasks / SingleTask / Transitions (with trigger + OnEvent tag + gating conditions),
    // plus tree-scoped Evaluators and GlobalTasks. Each FStateTreeEditorNode's Node /
    // Instance FInstancedStructs are expanded to their inner script-struct fields so the
    // dump is reviewable instead of an opaque EditorData object pointer.
    //
    // Returns nullptr when StateTree authoring headers are unavailable at compile time,
    // when the asset is null, or when its EditorData is missing/not a UStateTreeEditorData
    // (RunRegisteredJsonSidecars records the NullDiagnostic and skips the sidecar).
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildStateTreeJson(const UStateTree* StateTree);
}
