// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract test for docs/SCHEMA.md § Maintenance, which requires that creating,
// renaming or deleting a maintainer doc updates docs/index.md and docs/tags.md.
// Nothing enforced that, and the two indexes had silently drifted: SCHEMA.md
// itself - the page defining the rules both indexes follow - was in neither, and
// two Internal-section docs were reachable from index.md but from no tag row.
//
// The invariant checked here is the mechanical half of the mandate: every
// maintainer doc is reachable from BOTH indexes. A doc absent from an index is
// not findable by the reader who needs it, and nothing about the repository
// looks wrong.
//
// NOT checked, and left as review items because mechanising them produces
// failures that are wrong more often than they are right:
//   - Whether a doc's frontmatter tags each have their own tags.md row. Rows are
//     curated, not exhaustive; SCHEMA says tags must be "specific enough to be
//     useful in tags.md", which is a judgement. 60 frontmatter tags currently
//     have no row and most of them should not get one.
//   - Whether index.md's one-line description still describes the doc.
//   - Alphabetical order of the tags.md rows.
//
// Exclusions come from SCHEMA.md § Frontmatter and § Directory Layout, not from
// convenience: wiki-src/ is overlay source indexed by the live renderer, and
// logs/ is investigation output explicitly "not part of the wiki index". adr/ is
// deliberately NOT excluded - SCHEMA requires ADRs be indexed in both files.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"

namespace
{
    FString DocsIndex_ResolveDocsDir()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir() / TEXT("docs");
    }

    // docs-relative, forward-slashed, which is how both indexes spell a link
    // target: [title](adr/0001-one-file-one-asset.md).
    FString DocsIndex_RelativePath(const FString& DocsDir, const FString& AbsolutePath)
    {
        FString Normalized = AbsolutePath;
        FPaths::NormalizeFilename(Normalized);
        FString NormalizedRoot = DocsDir;
        FPaths::NormalizeFilename(NormalizedRoot);
        if (!NormalizedRoot.EndsWith(TEXT("/")))
        {
            NormalizedRoot.AppendChar(TEXT('/'));
        }
        return Normalized.StartsWith(NormalizedRoot)
            ? Normalized.RightChop(NormalizedRoot.Len())
            : Normalized;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDocsIndexCoverageTest,
    "PinWright.core.docs_schema.EveryMaintainerDocIsIndexed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDocsIndexCoverageTest::RunTest(const FString& Parameters)
{
    const FString DocsDir = DocsIndex_ResolveDocsDir();
    if (!TestFalse(TEXT("Resolved the Docs directory"), DocsDir.IsEmpty()))
    {
        return false;
    }
    if (!TestTrue(TEXT("docs directory exists on disk"),
            IFileManager::Get().DirectoryExists(*DocsDir)))
    {
        return false;
    }

    FString IndexContents;
    FString TagsContents;
    if (!TestTrue(TEXT("Read docs/index.md"),
            FFileHelper::LoadFileToString(IndexContents, *(DocsDir / TEXT("index.md")))))
    {
        return false;
    }
    if (!TestTrue(TEXT("Read docs/tags.md"),
            FFileHelper::LoadFileToString(TagsContents, *(DocsDir / TEXT("tags.md")))))
    {
        return false;
    }

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *DocsDir, TEXT("*.md"), true, false, false);
    Files.Sort();
    if (!TestTrue(TEXT("Found maintainer docs to check"), Files.Num() > 0))
    {
        return false;
    }

    int32 Checked = 0;
    int32 Missing = 0;

    for (const FString& File : Files)
    {
        const FString Relative = DocsIndex_RelativePath(DocsDir, File);

        // SCHEMA.md § Directory Layout: wiki-src/ is overlay source served by the
        // live renderer, logs/ is investigation output outside the wiki index.
        if (Relative.StartsWith(TEXT("wiki-src/")) || Relative.StartsWith(TEXT("logs/")))
        {
            continue;
        }
        // index.md does not link itself; tags.md does, through the `docs` row.
        if (Relative.Equals(TEXT("index.md")))
        {
            continue;
        }

        ++Checked;
        const FString LinkTarget = FString::Printf(TEXT("(%s)"), *Relative);

        if (!IndexContents.Contains(LinkTarget))
        {
            ++Missing;
            AddError(FString::Printf(
                TEXT("docs/%s is not linked from docs/index.md. SCHEMA.md § Maintenance requires ")
                TEXT("index.md be updated when a maintainer doc is created or renamed; a doc missing ")
                TEXT("from the catalog is not findable."),
                *Relative));
        }

        if (!TagsContents.Contains(LinkTarget))
        {
            ++Missing;
            AddError(FString::Printf(
                TEXT("docs/%s is reachable from no row in docs/tags.md. Add it to the row for a tag ")
                TEXT("it carries, or add a row for one."),
                *Relative));
        }
    }

    // Guards the walk: an exclusion that swallowed everything would make the
    // comparison above pass while checking nothing.
    if (!TestTrue(TEXT("Walk found maintainer docs after exclusions"), Checked > 0))
    {
        return false;
    }

    return TestEqual(
        TEXT("Every maintainer doc is linked from both docs/index.md and docs/tags.md"),
        Missing, 0);
}
