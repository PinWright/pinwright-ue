// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace MetaSoundDumpBuilder
{
    // metasound.json's skipReason when the document carries no default-page graph, i.e. the
    // nodes/edges/variables arrays are absent rather than empty. Mirrors the skipped/skipReason/
    // skipMessage vocabulary the folder sweep already writes into a skipped asset's meta.json,
    // so one predicate ("skipped is true -> the data is not here, read skipReason") covers both.
    inline constexpr const TCHAR* SkipReasonGraphUnavailable = TEXT("METASOUND_GRAPH_UNAVAILABLE");

    // Opt-in size knobs for the describe_metasound inline-readback path. Defaults reproduce
    // the full snapshot byte-for-byte, so asset.dump's metasound.json sidecar (which calls the
    // no-options overload) is unaffected and needs no aspect bump.
    struct FMetaSoundDumpOptions
    {
        // When true, omit each node's per-vertex inputs/outputs arrays — the vertex GUIDs and
        // per-input literal lookup that dominate the payload. Node id/classID/name, edges, and
        // the rootGraph interface (with input defaults) are retained, which is enough to confirm
        // graph shape. Use NodeIdFilter to fetch full pin detail for a specific node on demand.
        bool bCompact = false;

        // When non-empty, restrict the emitted nodes[] to nodes whose id is in this set (kept at
        // full vertex detail), for the "I just added node X, what are its pin names?" case without
        // returning the whole graph. Node ids match the `id` field (FGuid::ToString) of a prior dump.
        TSet<FString> NodeIdFilter;

        bool IsDefault() const { return !bCompact && NodeIdFilter.Num() == 0; }
    };

    // Accepts UObject* so the dispatch site doesn't need to know whether the asset is a
    // UMetaSoundPatch or UMetaSoundSource; the builder downcasts to IMetaSoundDocumentInterface
    // internally. Returns nullptr when MetaSound modules are unavailable or the cast fails.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildMetaSoundJson(UObject* Asset);

    // Options-aware overload backing describe_metasound's compact/nodeIds knobs. With a default
    // FMetaSoundDumpOptions the output is identical to the no-options overload above; when any
    // option is set, nodes are compacted/filtered and a summary header (compact/nodeCount/
    // returnedNodeCount) is added so a caller can tell it received a reduced view.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildMetaSoundJson(UObject* Asset, const FMetaSoundDumpOptions& Options);
}
