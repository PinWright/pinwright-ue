// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Renders the full wiki tree to disk on editor launch so the generated pages
// under OutputDirectory() stay in lock-step with the live call-wiki output.
// Every page is produced by reusing WikiHandler::RenderPage against the live
// handler registry — the generator owns no render logic of its own, so a
// generated file is byte-identical to what the dispatcher serves live.
//
// One exception, and it is deliberate: a rendered page over kSectionSplitBudget
// is written as a section index plus one file per `## ` section rather than as a
// single file (see kSectionSplitBudget). The reader-facing content is the same
// text, redistributed — every section page is reachable by filename from the
// index, and the transport resolves call("<page>.<section>") against the file on
// disk, so both discovery paths still land on it.
//
// The output dir (Saved/PinWright/wiki by default, overridable via
// UPinWrightProjectSettings::WikiOutputDirectory) is deliberately
// separate from the overlay source dir (docs/wiki-src): the source is
// hand-authored prose, the output is the assembled full pages.
//
// Alongside the pages, a machine-readable `registry.json` is written into the same
// directory carrying the operation count, top-level namespace count, and per-namespace
// method counts + maturity tiers — the single generated source of truth for the
// numbers published on the website and store listing.
//
// Writes are deterministic and idempotent: content carries no timestamp / GUID
// / absolute machine path, an unchanged page is skipped at final publication
// (byte-compared against the file on disk), and only stale files recorded in the
// previous validated ownership manifest are pruned. Pruning is restricted to the
// canonical Saved/PinWright/wiki root. An unchanged launch produces zero final
// replacements and zero git diff.

namespace WikiDiskGenerator
{
    // A rendered page longer than this is not written as one file: it is emitted
    // as its opening prose plus a `## Sections` index, with each `## ` section
    // written beside it as <slug>.<section-slug>.md. A reader then pays for the
    // section it needs instead of the whole guide — the failure that motivated
    // this was a 67 KB page (model.authoring) stalling a reader that opened it
    // whole. The budget sits well above every other generated page on purpose
    // (the next largest is ~36 KB): it splits an outlier, it does not reformat
    // the wiki. Measured in TCHARs; these pages are ASCII-dominated, so it
    // tracks the on-disk byte size closely.
    constexpr int32 kSectionSplitBudget = 40 * 1024;

    // Absolute path to the generated wiki output directory. Defaults to
    // <ProjectSavedDir>/PinWright/wiki; overridable via
    // UPinWrightProjectSettings::WikiOutputDirectory (relative
    // values resolve against the project directory).
    FString OutputDirectory();

    // Slug that owns the root index page (wiki.md). The live wiki serves the
    // root index for the empty path, so "wiki.md" is the canonical root page.
    FString RootIndexSlug();

    // Absolute path to the generated page for Slug (<OutputDirectory>/<slug>.md),
    // or empty when Slug is empty. Does not check existence — the caller
    // decides whether to fall back to a live render.
    FString PagePath(const FString& Slug);

    // Render the full wiki tree and reconcile it with OutputDirectory():
    // stage the complete output set, publish changed pages, skip unchanged ones,
    // and prune manifest-owned orphans only in the canonical output root.
    // Best-effort — failures are logged but never abort editor startup. A staging
    // failure changes no final output; a desired-output publication failure
    // suppresses pruning, while the final manifest publishes after pruning.
    void Generate();

#if WITH_DEV_AUTOMATION_TESTS
    namespace Testing
    {
        // Per-call seams for isolated reconciliation tests. Empty callbacks use
        // the production AtomicFileWriter and IFileManager delete paths.
        struct FOperations
        {
            TFunction<bool(const FString&, TArrayView<const uint8>, FString&)> StageFile;
            TFunction<bool(const FString&, TArrayView<const uint8>, FString&)> WriteFile;
            TFunction<bool(const FString&)> DeleteFile;
        };

        struct FReconcileResult
        {
            bool bSucceeded = false;
            bool bPruneSkipped = false;
            FString PruneSkipReason;
        };

        bool ReconcileOutputForTests(
            const FString& OutDir,
            const TMap<FString, FString>& DesiredFiles,
            const FOperations& Operations = FOperations());

        FReconcileResult ReconcileOutputForTestsDetailed(
            const FString& OutDir,
            const FString& CanonicalOwnedRoot,
            const TMap<FString, FString>& DesiredFiles,
            const FOperations& Operations = FOperations());

        FString OwnershipManifestFilename();
        FString OutsideOwnedRootPruneSkipReason();
        int64 MaxOwnershipManifestBytesForTests();
        int32 MaxOwnershipManifestEntriesForTests();
    }
#endif
}
