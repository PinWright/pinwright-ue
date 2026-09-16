// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FRpcDispatcher;

// Wiki rendering for the PinWright HTTP gateway. The transport calls
// RenderPage() directly when a request body omits its "params" field.

namespace WikiHandler
{
    // Classification of a node in the namespace tree built from registered method Categories.
    enum class EWikiNodeKind : uint8
    {
        Method,         // Path equals a registered method's full name
        LeafNamespace,  // Path equals at least one Category and is not a strict prefix of any other Category
        Branch,         // Path is a strict prefix of at least one Category but not itself a Category
        Hybrid,         // Path is both a Category AND a strict prefix of a deeper Category
        Topic,          // Path equals a standalone docs/wiki-src/<slug>.md file whose slug does not match any registered Category.
        NotFound,       // Path matches nothing
    };

    // One top-level namespace row of the machine-readable registry manifest.
    struct FRegistryNamespaceStat
    {
        FString Slug;      // lowercased top-level namespace
        FString Tier;      // "core" | "experimental" | "internal" | "unclassified"; never empty.
                           // "unclassified" is the fail-closed fallback for a slug maturity.json
                           // has no entry for (or classifies with an unknown value) - an omission
                           // must not read as core. Not a legal maturity.json value.
        int32 Methods = 0; // registered methods under this namespace, nested categories included
    };

    // Aggregate the live registry into the counts the registry manifest publishes:
    // OutOperations is the total registered method count, OutNamespaces one row per
    // top-level namespace sorted by slug.
    //
    // Namespaces come from each registration's Category, NOT from the first dotted
    // segment of the method name — e.g. sequence.* methods register under the
    // "sequencer" Category and must not invent a "sequence" namespace. The dev-only
    // "_test" fixture namespace is excluded because it is stripped from shipping
    // packages, so counting it would overstate what a customer receives.
    //
    // Returns false only when the live editor subsystem or dispatcher is unavailable.
    bool GetRegistryStats(TArray<FRegistryNamespaceStat>& OutNamespaces, int32& OutOperations);

    // Render the wiki page for OriginalPath, writing the markdown body into OutMarkdown.
    // Empty OriginalPath renders the root index. Unknown paths render a Not-Found page
    // with fuzzy suggestions — still a successful render.
    // Returns false only when the live editor subsystem or dispatcher is unavailable.
    // Exported: wiki-doc tests in the split-out integration modules (PinWrightGeometry,
    // PinWrightChooser) render their namespace/method pages through this entry.
    PINWRIGHT_API bool RenderPage(const FString& OriginalPath, FString& OutMarkdown);

    // Render against an explicit dispatcher rather than the live subsystem one.
    // The cache is keyed on (source dispatcher, registry generation), so a handler
    // registered after a prior warm render becomes visible here. Always returns true.
    bool RenderPage(const FRpcDispatcher& Dispatcher, const FString& OriginalPath, FString& OutMarkdown);

    // Collect every slug the live renderer can serve — namespaces, methods, topics —
    // plus the root index slug. Returns false only when the subsystem or dispatcher
    // is unavailable.
    bool EnumerateAllSlugs(TArray<FString>& OutSlugs);

    // EnumerateAllSlugs against an explicit dispatcher; shares the same generation-keyed
    // cache as the dispatcher-parameter RenderPage overload. Always returns true.
    bool EnumerateAllSlugs(const FRpcDispatcher& Dispatcher, TArray<FString>& OutSlugs);

    // Apply the same normalization RenderPage applies to an incoming path so an
    // on-disk filename lookup matches the slug the renderer would resolve. This is
    // cache-aware: a verbatim input that names a registered node (e.g. an
    // underscore namespace like "game_framework") is preserved; only an input that
    // does not resolve verbatim is collapsed via the legacy underscore->dot rule.
    // The parameterless overload resolves against the live subsystem dispatcher
    // (falling back to the pure legacy collapse when no dispatcher is available).
    FString NormalizeSlug(const FString& In);

    // NormalizeSlug against an explicit dispatcher; shares the generation-keyed
    // cache used by RenderPage/EnumerateAllSlugs so the resolved slug agrees with
    // what those would classify and the disk generator wrote.
    FString NormalizeSlug(const FRpcDispatcher& Dispatcher, const FString& In);
}
