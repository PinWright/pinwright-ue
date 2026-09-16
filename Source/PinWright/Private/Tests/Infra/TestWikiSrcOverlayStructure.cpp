// Copyright (c) 2026 Alexander Penkin. MIT License.

// Structural lint for every authoring page under docs/wiki-src/.
//
// The overlay merger has hard rendering rules (docs/wiki-src/README.md, and the
// contract comments on Catalog/WikiOverlay.h). Break one and the page still
// looks correct in the source tree while part of it renders NOWHERE - there is
// no error, no warning, and no output diff a reviewer would notice. Roughly 190
// pages were governed by those rules with nothing enforcing any of them; two
// duplicated H3 sections and a stale run of invisible `##` sections is what
// prompted this.
//
// Four rules, each one a silent-loss failure mode rather than a style opinion:
//
//   1. An `### ` heading must be EXACTLY a slug the renderer serves. The merger
//      keys method sections on the bare dotted method name, so a decorated
//      heading (backticks, an em-dash gloss, a `### Params` sub-heading) is
//      keyed under a string nothing looks up. Its body reaches no page AND it
//      truncates the section above it, so one typo costs two sections. Not
//      enforced for a namespace whose gated integration sub-module is skipped
//      on this host (IntegrationGates): it registers no handlers, so NONE of
//      its slugs are served and the rule cannot distinguish a typo from a
//      disabled engine plugin.
//   2. No two `### ` headings in one file may share their text. The merger
//      returns the first match, so the second section is dead weight that reads
//      as live documentation - and when the two disagree, the wrong one may be
//      the survivor.
//   3. No `## ` heading may follow the first `### `. Namespace-page rendering
//      stops at the first `### `, so a `##` section after that point never
//      reaches the namespace page; it is swallowed into whichever method
//      section precedes it. Anchors pointing at it resolve to nothing.
//   4. A TOP-LEVEL overlay (a slug with no dot) needs non-empty prelude text
//      before its first `## ` or `### `. That prelude is what the generated root
//      index shows; without it the namespace renders as a bare root entry.
//      Restricted to top-level slugs on purpose - a nested page like
//      bpir.examples.if-else.md never reaches the root index, and 32 of them
//      legitimately open straight into a `## ` heading.
//
// NOT checked: the two-sentence prelude limit from README.md § Authoring Rules.
// Sentence counting over prose with abbreviations, code spans and dotted method
// names produces false failures, and the cost of a three-sentence prelude is
// verbosity rather than invisibility. It stays a review item.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"

#include "Catalog/WikiHandler.h"
#include "IntegrationGates.h"

namespace
{
    FString WikiSrcLint_ResolveDir()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir() / TEXT("docs") / TEXT("wiki-src");
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiSrcOverlayStructureTest,
    "PinWright.infra.wiki_src.SourcePagesFollowRenderingRules",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiSrcOverlayStructureTest::RunTest(const FString& Parameters)
{
    const FString WikiSrcDir = WikiSrcLint_ResolveDir();
    if (!TestFalse(TEXT("Resolved docs/wiki-src"), WikiSrcDir.IsEmpty()))
    {
        return false;
    }
    if (!TestTrue(TEXT("docs/wiki-src exists on disk"),
            IFileManager::Get().DirectoryExists(*WikiSrcDir)))
    {
        return false;
    }

    // The live renderer is the authority on what an `### ` heading can key to -
    // a hard-coded list would rot the same way the pages do.
    TArray<FString> SlugArray;
    if (!TestTrue(TEXT("Enumerated the slugs the live wiki renderer serves"),
            WikiHandler::EnumerateAllSlugs(SlugArray)))
    {
        return false;
    }
    const TSet<FString> Slugs(SlugArray);
    if (!TestTrue(TEXT("Renderer reported a non-empty slug set"), Slugs.Num() > 0))
    {
        return false;
    }

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(WikiSrcDir / TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
    Files.Sort();
    if (!TestTrue(TEXT("Found overlay pages to lint"), Files.Num() > 0))
    {
        return false;
    }

    int32 Violations = 0;

    for (const FString& FileName : Files)
    {
        // README*.md is authoring guidance for this directory, not an overlay;
        // the router skips it and so does this lint.
        if (FileName.StartsWith(TEXT("README")))
        {
            continue;
        }

        const FString Slug = FPaths::GetBaseFilename(FileName);
        const FString FullPath = WikiSrcDir / FileName;

        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *FullPath))
        {
            ++Violations;
            AddError(FString::Printf(TEXT("Could not read docs/wiki-src/%s."), *FileName));
            continue;
        }

        TArray<FString> Lines;
        Contents.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

        TMap<FString, int32> SeenHeadings;
        int32 FirstH3Line = INDEX_NONE;
        bool bPreludeHasText = false;
        bool bPreludeClosed = false;
        bool bSawH1 = false;

        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            const FString& Line = Lines[Index];
            const int32 LineNumber = Index + 1;

            if (Line.StartsWith(TEXT("### ")))
            {
                const FString Heading = Line.RightChop(4).TrimStartAndEnd();
                if (FirstH3Line == INDEX_NONE)
                {
                    FirstH3Line = LineNumber;
                }
                bPreludeClosed = true;

                if (const int32* Previous = SeenHeadings.Find(Heading))
                {
                    ++Violations;
                    AddError(FString::Printf(
                        TEXT("docs/wiki-src/%s: '### %s' appears twice, at line %d and line %d. ")
                        TEXT("The merger returns the first match, so the second section renders ")
                        TEXT("nowhere. Merge them."),
                        *FileName, *Heading, *Previous, LineNumber));
                }
                else
                {
                    SeenHeadings.Add(Heading, LineNumber);
                }

                // A gated integration (pose_search, geometry, pcg, chooser, the ui.* families)
                // registers no handlers when its owning engine plugin is disabled on the host,
                // so the renderer serves none of its slugs and every H3 on that page would read
                // as a violation. The page is still correct - it documents a conditionally
                // available namespace - so rule 1 is unenforceable here rather than broken.
                // The trade is that a typo inside a gated namespace goes uncaught on a host
                // where that plugin is off; it is still caught wherever the plugin is enabled.
                if (!Slugs.Contains(Heading)
                    && IntegrationGates::FindSkippedByMethod(Heading).IsEmpty())
                {
                    ++Violations;
                    AddError(FString::Printf(
                        TEXT("docs/wiki-src/%s:%d: '### %s' is not a slug the wiki renderer serves, ")
                        TEXT("so its body reaches no page and it truncates the section above it. An ")
                        TEXT("H3 must be exactly a bare dotted method name - use a bold label for a ")
                        TEXT("sub-heading."),
                        *FileName, LineNumber, *Heading));
                }
                continue;
            }

            if (Line.StartsWith(TEXT("## ")))
            {
                bPreludeClosed = true;
                if (FirstH3Line != INDEX_NONE)
                {
                    ++Violations;
                    AddError(FString::Printf(
                        TEXT("docs/wiki-src/%s:%d: '%s' follows the first '### ' (line %d). Namespace ")
                        TEXT("rendering stops at the first '### ', so this section never reaches the ")
                        TEXT("namespace page. Move it above the H3 block."),
                        *FileName, LineNumber, *Line.TrimStartAndEnd(), FirstH3Line));
                }
                continue;
            }

            if (!bPreludeClosed)
            {
                const FString Trimmed = Line.TrimStartAndEnd();
                if (Trimmed.IsEmpty())
                {
                    continue;
                }
                if (!bSawH1 && Trimmed.StartsWith(TEXT("# ")))
                {
                    bSawH1 = true;
                    continue;
                }
                if (Trimmed.StartsWith(TEXT("<!--")))
                {
                    continue;
                }
                bPreludeHasText = true;
            }
        }

        // Only a top-level overlay's prelude reaches the generated root index.
        if (!Slug.Contains(TEXT(".")) && !bPreludeHasText)
        {
            ++Violations;
            AddError(FString::Printf(
                TEXT("docs/wiki-src/%s: top-level overlay has no prelude text before its first ")
                TEXT("heading, so '%s' renders as a bare entry in the generated root index."),
                *FileName, *Slug));
        }
    }

    return TestEqual(
        TEXT("Every docs/wiki-src page follows the overlay rendering rules"),
        Violations, 0);
}
