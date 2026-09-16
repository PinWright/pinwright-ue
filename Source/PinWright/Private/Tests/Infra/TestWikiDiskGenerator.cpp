// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Catalog/WikiDiskGenerator.h"
#include "Catalog/WikiHandler.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Infra/WikiDocTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AtomicFileWriter.h"

// The disk generator owns no render logic of its own: every generated file is
// WikiHandler::RenderPage output wrapped in a fixed generated-file header. These
// tests drive Generate() and assert the on-disk pages stay byte-identical to the
// live render, that the root file is the index (not the "wiki" topic page), and
// that a second Generate() over unchanged inputs reproduces the same bytes.
//
// The one place the generator does reshape a render: a page over
// kSectionSplitBudget is redistributed into a section index plus one file per
// `## ` section, so the byte-identity assertion above holds for every page the
// split leaves alone. OversizedPageSplitsIntoSectionIndex covers that path.

namespace
{
    // Strip the deterministic header the generator prepends (a single
    // "<!-- GENERATED ... -->" comment line followed by a blank line) so the
    // remainder can be compared against the raw RenderPage body. Returns false
    // when the expected header shape is absent.
    bool TestWikiDiskGenerator_StripHeader(const FString& FileContent, FString& OutBody)
    {
        const FString Marker = TEXT("-->\n\n");
        int32 MarkerIdx;
        if (!FileContent.FindChar(TEXT('<'), MarkerIdx) || !FileContent.StartsWith(TEXT("<!-- GENERATED")))
        {
            return false;
        }

        int32 BodyStart = FileContent.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        if (BodyStart == INDEX_NONE)
        {
            return false;
        }

        OutBody = FileContent.RightChop(BodyStart + Marker.Len());
        return true;
    }

    // Column-0 `## ` headings outside fenced code blocks — exactly the boundaries
    // the generator splits an oversized page on. A `## ` inside a ``` block is
    // quoted markdown, not a heading, and does not count.
    int32 TestWikiDiskGenerator_CountSections(const FString& Body)
    {
        TArray<FString> Lines;
        Body.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

        int32 Count = 0;
        bool bInFence = false;
        for (const FString& Line : Lines)
        {
            if (Line.TrimStart().StartsWith(TEXT("```")))
            {
                bInFence = !bInFence;
                continue;
            }
            if (!bInFence && Line.StartsWith(TEXT("## ")) && !Line.Mid(3).TrimStartAndEnd().IsEmpty())
            {
                ++Count;
            }
        }
        return Count;
    }

    // Link targets listed under a split page's `## Sections` index, in order.
    // Returns false when the page carries no such index (i.e. it was not split).
    // The link target is read from the LAST "](" on the line so a heading that
    // itself contains brackets or parentheses cannot fool the parse.
    bool TestWikiDiskGenerator_ParseSectionIndex(const FString& Body, TArray<FString>& OutTargets)
    {
        TArray<FString> Lines;
        Body.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

        int32 Idx = INDEX_NONE;
        for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
        {
            if (Lines[LineIdx].TrimEnd() == TEXT("## Sections"))
            {
                Idx = LineIdx + 1;
                break;
            }
        }
        if (Idx == INDEX_NONE)
        {
            return false;
        }

        for (; Idx < Lines.Num() && !Lines[Idx].StartsWith(TEXT("## ")); ++Idx)
        {
            const FString& Line = Lines[Idx];
            if (!Line.StartsWith(TEXT("- [")))
            {
                continue;
            }

            const int32 LinkStart = Line.Find(TEXT("]("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            int32 LinkEnd = INDEX_NONE;
            if (LinkStart == INDEX_NONE || !Line.FindLastChar(TEXT(')'), LinkEnd) || LinkEnd <= LinkStart + 2)
            {
                continue;
            }
            OutTargets.Add(Line.Mid(LinkStart + 2, LinkEnd - LinkStart - 2));
        }
        return true;
    }

#if WITH_DEV_AUTOMATION_TESTS
    FString TestWikiDiskGenerator_MakeScratchRoot()
    {
        FString Root = FPaths::ProjectSavedDir() / TEXT("PinWright/TestTemp/WikiDiskGenerator")
            / FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }

    FString TestWikiDiskGenerator_BuildOwnershipManifest(
        TArray<FString> Filenames,
        const FString& Owner = TEXT("PinWright.WikiDiskGenerator"),
        int32 SchemaVersion = 1,
        bool bSortFilenames = true)
    {
        if (bSortFilenames)
        {
            Filenames.Sort();
        }

        FString Out = TEXT("{\n");
        Out += FString::Printf(TEXT("    \"owner\": \"%s\",\n"), *Owner);
        Out += FString::Printf(TEXT("    \"schemaVersion\": %d,\n"), SchemaVersion);
        Out += TEXT("    \"files\": [\n");
        for (int32 Index = 0; Index < Filenames.Num(); ++Index)
        {
            Out += FString::Printf(TEXT("        \"%s\"%s\n"), *Filenames[Index],
                Index + 1 < Filenames.Num() ? TEXT(",") : TEXT(""));
        }
        Out += TEXT("    ]\n}\n");
        return Out;
    }

    TMap<FString, FString> TestWikiDiskGenerator_MakeDesiredFiles()
    {
        TMap<FString, FString> DesiredFiles;
        DesiredFiles.Add(TEXT("current.md"), TEXT("<!-- GENERATED -->\n\n# current\n"));
        DesiredFiles.Add(TEXT("registry.json"), TEXT("{}\n"));
        return DesiredFiles;
    }

    bool TestWikiDiskGenerator_WriteText(const FString& Path, FStringView Text)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
        return FFileHelper::SaveStringToFile(Text, *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    bool TestWikiDiskGenerator_WriteBytes(const FString& Path, const TArray<uint8>& Bytes)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
        return FFileHelper::SaveArrayToFile(Bytes, *Path);
    }

    FString TestWikiDiskGenerator_PadManifestToBytes(FString Manifest, int32 ByteCount)
    {
        check(Manifest.Len() <= ByteCount);
        Manifest += FString::ChrN(ByteCount - Manifest.Len(), TEXT(' '));
        return Manifest;
    }

    TArray<FString> TestWikiDiskGenerator_MakeManifestEntries(int32 Count)
    {
        check(Count > 0);
        TArray<FString> Filenames;
        Filenames.Reserve(Count);
        for (int32 Index = 0; Index < Count - 1; ++Index)
        {
            Filenames.Add(FString::Printf(TEXT("entry-%08d.md"), Index));
        }
        Filenames.Add(TEXT("managed-old.md"));
        Filenames.Sort();
        return Filenames;
    }

    TMap<FString, FString> TestWikiDiskGenerator_MakeLargeDesiredFiles(
        int32 Count,
        int32 FilenamePadding)
    {
        TMap<FString, FString> DesiredFiles;
        const FString Padding = FString::ChrN(FilenamePadding, TEXT('a'));
        for (int32 Index = 0; Index < Count; ++Index)
        {
            DesiredFiles.Add(
                FString::Printf(TEXT("out-%08d-%s.md"), Index, *Padding),
                FString());
        }
        return DesiredFiles;
    }

    int32 TestWikiDiskGenerator_CountTempFiles(const FString& Root)
    {
        TArray<FString> TempFiles;
        IFileManager::Get().FindFilesRecursive(
            TempFiles, *Root, TEXT("*.tmp"), /*Files=*/true, /*Directories=*/false);
        return TempFiles.Num();
    }

    int32 TestWikiDiskGenerator_CountSiblingStageDirectories(const FString& OutDir)
    {
        TArray<FString> StageDirectories;
        const FString Pattern = FPaths::GetPath(OutDir)
            / FString::Printf(TEXT(".%s.pinwright-wiki-stage-*"),
                *FPaths::GetCleanFilename(OutDir));
        IFileManager::Get().FindFiles(
            StageDirectories, *Pattern, /*Files=*/false, /*Directories=*/true);
        return StageDirectories.Num();
    }
#endif
}

// ============================================================================
// Generated bodies match the live render: for the root and a couple enumerated
// slugs, the on-disk file (minus its generated header) equals RenderPage output.
// Counterfactual: if the generator diverged from the handler (e.g. injected its
// own formatting or a per-run timestamp), the stripped body would differ and the
// equality assertion fails. Skips with AddInfo when the plugin is unresolved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorBodyMatchesLiveTest,
    "PinWright.infra.wiki_disk_generator.GeneratedBodyMatchesLiveRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorBodyMatchesLiveTest::RunTest(const FString& Parameters)
{
    if (WikiDiskGenerator::OutputDirectory().IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Plugin unresolved (no output directory); skipping disk-generator round-trip"));
        return true;
    }

    WikiDiskGenerator::Generate();

    TArray<FString> Slugs;
    if (!WikiHandler::EnumerateAllSlugs(Slugs))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    // The root file lives under RootIndexSlug() but its live body is the empty-path
    // render (the index), not the "wiki" topic page.
    TArray<TPair<FString, FString>> Cases;
    Cases.Emplace(WikiDiskGenerator::RootIndexSlug(), TEXT(""));

    for (const FString& Slug : Slugs)
    {
        if (Slug == WikiDiskGenerator::RootIndexSlug())
        {
            continue;
        }
        Cases.Emplace(Slug, Slug);
        if (Cases.Num() >= 3)
        {
            break;
        }
    }

    for (const TPair<FString, FString>& Case : Cases)
    {
        const FString Path = WikiDiskGenerator::PagePath(Case.Key);

        FString FileContent;
        if (!FFileHelper::LoadFileToString(FileContent, *Path))
        {
            AddError(FString::Printf(TEXT("generated page missing on disk: %s"), *Path));
            continue;
        }

        FString StrippedBody;
        if (!TestWikiDiskGenerator_StripHeader(FileContent, StrippedBody))
        {
            AddError(FString::Printf(TEXT("generated page lacks the expected header: %s"), *Path));
            continue;
        }

        FString LiveBody;
        if (!WikiHandler::RenderPage(Case.Value, LiveBody))
        {
            AddError(FString::Printf(TEXT("live render failed for slug '%s'"), *Case.Value));
            continue;
        }

        TestEqual(FString::Printf(TEXT("generated body matches live render for slug '%s'"), *Case.Key),
            StrippedBody, LiveBody);
    }

    return true;
}

// ============================================================================
// The root index and the "wiki" usage-guide topic are distinct on-disk pages.
// The generated RootIndexSlug() file (index.md) carries the root-index marker
// "## Namespaces"; the generated wiki.md is the standalone usage guide, which has
// no "## Namespaces" marker and differs from the index.
// Counterfactual / regression guard: if the root index claimed the "wiki" slug
// (the pre-fix behavior), wiki.md would BE the index — it would carry
// "## Namespaces" and equal the root body, and call("wiki") would no longer serve
// the usage guide. Both assertions below fail in that case.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorRootIsIndexTest,
    "PinWright.infra.wiki_disk_generator.RootSlugIsIndexNotTopic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorRootIsIndexTest::RunTest(const FString& Parameters)
{
    if (WikiDiskGenerator::OutputDirectory().IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Plugin unresolved (no output directory); skipping root-index check"));
        return true;
    }

    WikiDiskGenerator::Generate();

    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug())))
    {
        AddError(TEXT("generated root index page missing on disk"));
        return false;
    }

    FString RootBody;
    if (!TestWikiDiskGenerator_StripHeader(FileContent, RootBody))
    {
        AddError(TEXT("generated root index page lacks the expected header"));
        return false;
    }

    TestTrue(TEXT("root file is the namespace index (contains '## Namespaces')"),
        RootBody.Contains(TEXT("## Namespaces")));

    // The "wiki" usage-guide topic must still be materialized under its own slug
    // (regression guard: the root index must not claim wiki.md).
    FString WikiTopicFile;
    if (!FFileHelper::LoadFileToString(WikiTopicFile, *WikiDiskGenerator::PagePath(TEXT("wiki"))))
    {
        AddError(TEXT("generated wiki usage-guide topic page missing on disk"));
        return false;
    }

    FString WikiTopicBody;
    if (!TestWikiDiskGenerator_StripHeader(WikiTopicFile, WikiTopicBody))
    {
        AddError(TEXT("generated wiki topic page lacks the expected header"));
        return false;
    }

    TestFalse(TEXT("wiki topic is the usage guide, not the namespace index"),
        WikiTopicBody.Contains(TEXT("## Namespaces")));
    TestNotEqual(TEXT("root index and wiki usage-guide topic are distinct pages"),
        RootBody, WikiTopicBody);
    return true;
}

// ============================================================================
// Every guide the generated root index advertises is materialized on disk under
// its own slug, so a reader who finds a page in the "## Task guides" list can
// actually open it with a filesystem tool (the documented discovery workflow) —
// and a stale entry pointing at a page the generator no longer writes is caught
// here rather than by the user.
// Counterfactual: if the guide list were hand-maintained (or the enrolment that
// feeds it diverged from the slug enumeration the generator writes), an entry
// would name a slug with no <slug>.md beside index.md and the file-exists
// assertion fails. Skips with AddInfo when the plugin is unresolved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorGuideListReachableTest,
    "PinWright.infra.wiki_disk_generator.RootGuideListPagesExistOnDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorGuideListReachableTest::RunTest(const FString& Parameters)
{
    if (WikiDiskGenerator::OutputDirectory().IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Plugin unresolved (no output directory); skipping guide-list reachability check"));
        return true;
    }

    WikiDiskGenerator::Generate();

    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug())))
    {
        AddError(TEXT("generated root index page missing on disk"));
        return false;
    }

    FString RootBody;
    if (!TestWikiDiskGenerator_StripHeader(FileContent, RootBody))
    {
        AddError(TEXT("generated root index page lacks the expected header"));
        return false;
    }

    FString Section;
    if (!TestTrue(TEXT("generated root index carries the '## Task guides' section"),
            WikiDocTestHelpers::ExtractSection(RootBody, TEXT("Task guides"), Section)))
    {
        return false;
    }

    TArray<TPair<FString, FString>> Entries;
    WikiDocTestHelpers::ParseSlugBullets(Section, Entries);
    if (!TestTrue(TEXT("generated guide list is not empty"), Entries.Num() > 0))
    {
        return false;
    }

    for (const TPair<FString, FString>& Entry : Entries)
    {
        FString GuideFile;
        TestTrue(*FString::Printf(TEXT("listed guide '%s' exists on disk"), *Entry.Key),
            FFileHelper::LoadFileToString(GuideFile, *WikiDiskGenerator::PagePath(Entry.Key)));
    }
    return true;
}

// ============================================================================
// Generate() is idempotent: a second run over unchanged inputs reproduces the
// root page byte-for-byte.
// Counterfactual: if the header or body carried per-run data (timestamp, GUID,
// machine path), the second capture would differ and the byte-equality fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorIdempotentTest,
    "PinWright.infra.wiki_disk_generator.GenerateIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorIdempotentTest::RunTest(const FString& Parameters)
{
    if (WikiDiskGenerator::OutputDirectory().IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Plugin unresolved (no output directory); skipping idempotency check"));
        return true;
    }

    const FString RootPath = WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug());

    WikiDiskGenerator::Generate();
    FString First;
    if (!FFileHelper::LoadFileToString(First, *RootPath))
    {
        AddError(TEXT("generated root index page missing after first Generate()"));
        return false;
    }

    WikiDiskGenerator::Generate();
    FString Second;
    if (!FFileHelper::LoadFileToString(Second, *RootPath))
    {
        AddError(TEXT("generated root index page missing after second Generate()"));
        return false;
    }

    TestEqual(TEXT("root page is byte-identical across two Generate() runs"), Second, First);
    return true;
}

// ============================================================================
// Oversized pages are split rather than written whole: no generated page is left
// over kSectionSplitBudget while it still has `## ` boundaries to split on, and
// every page that carries a `## Sections` index lists section pages that really
// exist beside it. The failure this guards is a reader opening the one 67 KB
// guide (model.authoring) in a single call and stalling on it.
// Counterfactual: delete the split from Generate() and model.authoring comes back
// as one 67 KB file with 15 `## ` sections — it trips the oversized invariant, and
// with no index emitted anywhere the "at least one page was split" assertion fails
// too. Weaken the split to emit an index without the section files and the
// existence assertion fails. Skips with AddInfo when the plugin is unresolved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorOversizedSplitTest,
    "PinWright.infra.wiki_disk_generator.OversizedPageSplitsIntoSectionIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorOversizedSplitTest::RunTest(const FString& Parameters)
{
    const FString OutDir = WikiDiskGenerator::OutputDirectory();
    if (OutDir.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Plugin unresolved (no output directory); skipping oversized-page split check"));
        return true;
    }

    WikiDiskGenerator::Generate();

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(OutDir / TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
    if (!TestTrue(TEXT("generator wrote pages to disk"), Files.Num() > 0))
    {
        return false;
    }

    int32 SplitPages = 0;
    for (const FString& File : Files)
    {
        const FString Slug = FPaths::GetBaseFilename(File);

        FString FileContent;
        if (!FFileHelper::LoadFileToString(FileContent, *(OutDir / File)))
        {
            AddError(FString::Printf(TEXT("generated page unreadable: %s"), *File));
            continue;
        }

        FString Body;
        if (!TestWikiDiskGenerator_StripHeader(FileContent, Body))
        {
            AddError(FString::Printf(TEXT("generated page lacks the expected header: %s"), *File));
            continue;
        }

        // The root index is the entry point every reader starts from and is never
        // split; every other page must have given up its sections. A page that is
        // still oversized has to be one the split could not help - a single section
        // with no `## ` boundary inside it.
        if (Slug != WikiDiskGenerator::RootIndexSlug() && Body.Len() > WikiDiskGenerator::kSectionSplitBudget)
        {
            TestTrue(*FString::Printf(
                         TEXT("oversized page '%s' (%d chars) has fewer than 2 '## ' sections left to split on"),
                         *Slug, Body.Len()),
                     TestWikiDiskGenerator_CountSections(Body) < 2);
        }

        TArray<FString> Targets;
        if (!TestWikiDiskGenerator_ParseSectionIndex(Body, Targets))
        {
            continue;
        }

        ++SplitPages;
        TestTrue(*FString::Printf(TEXT("section index of '%s' lists at least 2 sections"), *Slug),
                 Targets.Num() >= 2);

        for (const FString& Target : Targets)
        {
            FString SectionContent;
            const bool bLoaded = FFileHelper::LoadFileToString(SectionContent, *(OutDir / Target));
            TestTrue(*FString::Printf(TEXT("section page '%s' listed by '%s' exists on disk"), *Target, *Slug),
                     bLoaded);
            if (bLoaded)
            {
                TestTrue(*FString::Printf(TEXT("section page '%s' carries a body"), *Target),
                         SectionContent.Len() > 0);
            }
        }
    }

    // The plugin ships docs/wiki-src/model.authoring.md at ~67 KB, so a tree with
    // the split working always produces at least one index. If that guide is ever
    // shortened below the budget on purpose, this is the assertion to revisit.
    TestTrue(TEXT("at least one oversized page was emitted as a section index"), SplitPages > 0);
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS

// ============================================================================
// Pruning is authorized only by the previous valid ownership manifest. A file
// that merely looks generated is not owned, and an unsafe manifest fails closed.
// Counterfactual: wildcard pruning deletes both sentinels; header-based ownership
// deletes header-only.md; accepting an unsafe manifest permits path escape.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorOwnedManifestPruneTest,
    "PinWright.infra.wiki_disk_generator.PruneOnlyManifestOwnedFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorOwnedManifestPruneTest::RunTest(const FString& Parameters)
{
    const FString Root = TestWikiDiskGenerator_MakeScratchRoot();
    IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString ManifestFilename = WikiDiskGenerator::Testing::OwnershipManifestFilename();
    const FString ManifestPath = Root / ManifestFilename;
    const FString ManagedOldPath = Root / TEXT("managed-old.md");
    const FString UnmanagedPath = Root / TEXT("unmanaged.md");
    const FString HeaderOnlyPath = Root / TEXT("header-only.md");
    const FString BomPagePath = Root / TEXT("bom.md");
    const FString BomOnlyPath = Root / TEXT("bom-only.md");
    const FString UnmanagedBytes = TEXT("# handwritten\n\nDo not delete.\n");
    const FString HeaderOnlyBytes =
        TEXT("<!-- GENERATED from docs/wiki-src overlays + handler registry at editor launch. ")
        TEXT("Do not edit here; edit the matching source under docs/wiki-src/. -->\n\n")
        TEXT("# copied generated-looking file\n");
    const TArray<uint8> BomPageBytes =
        { 0xef, 0xbb, 0xbf, 's', 'a', 'm', 'e', '\n' };
    const TArray<uint8> BomOnlyBytes = { 0xef, 0xbb, 0xbf };

    TestTrue(TEXT("seed managed stale page"),
        TestWikiDiskGenerator_WriteText(ManagedOldPath, TEXT("old generated page")));
    TestTrue(TEXT("seed unmanaged page"),
        TestWikiDiskGenerator_WriteText(UnmanagedPath, UnmanagedBytes));
    TestTrue(TEXT("seed generated-header-only page"),
        TestWikiDiskGenerator_WriteText(HeaderOnlyPath, HeaderOnlyBytes));
    TestTrue(TEXT("seed BOM-encoded equal-looking page"),
        TestWikiDiskGenerator_WriteBytes(BomPagePath, BomPageBytes));
    TestTrue(TEXT("seed BOM-only file for empty desired content"),
        TestWikiDiskGenerator_WriteBytes(BomOnlyPath, BomOnlyBytes));
    TestTrue(TEXT("seed valid previous ownership manifest"),
        TestWikiDiskGenerator_WriteText(ManifestPath,
            TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("managed-old.md") })));

    const TMap<FString, FString> DesiredFiles = TestWikiDiskGenerator_MakeDesiredFiles();
    TMap<FString, FString> CanonicalByteDesiredFiles = DesiredFiles;
    CanonicalByteDesiredFiles.Add(TEXT("bom.md"), TEXT("same\n"));
    CanonicalByteDesiredFiles.Add(TEXT("bom-only.md"), FString());
    TArray<FString> OperationOrder;
    WikiDiskGenerator::Testing::FOperations OrderedOperations;
    OrderedOperations.WriteFile = [&OperationOrder](
        const FString& Path, TArrayView<const uint8> Bytes, FString& OutError)
    {
        OperationOrder.Add(FString(TEXT("publish:")) + FPaths::GetCleanFilename(Path));
        const AtomicFileWriter::FResult Result = AtomicFileWriter::WriteBytes(
            Path, Bytes, AtomicFileWriter::EExistingFilePolicy::ReplaceExisting);
        OutError = Result.Error;
        return Result.IsSuccess();
    };
    OrderedOperations.DeleteFile = [&OperationOrder](const FString& Path)
    {
        OperationOrder.Add(FString(TEXT("prune:")) + FPaths::GetCleanFilename(Path));
        return IFileManager::Get().Delete(*Path, /*RequireExists=*/false,
            /*EvenReadOnly=*/false, /*Quiet=*/true);
    };
    TestFalse(TEXT("empty explicit output directory is rejected before path resolution"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(FString(), DesiredFiles));
    TestTrue(TEXT("manifest-owned reconciliation succeeds"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(
            Root, CanonicalByteDesiredFiles, OrderedOperations));

    TestFalse(TEXT("manifest-owned stale page is pruned"),
        IFileManager::Get().FileExists(*ManagedOldPath));

    FString ActualUnmanaged;
    TestTrue(TEXT("unmanaged page survives"),
        FFileHelper::LoadFileToString(ActualUnmanaged, *UnmanagedPath));
    TestEqual(TEXT("unmanaged page remains byte-identical"), ActualUnmanaged, UnmanagedBytes);

    FString ActualHeaderOnly;
    TestTrue(TEXT("generated-header-only page survives"),
        FFileHelper::LoadFileToString(ActualHeaderOnly, *HeaderOnlyPath));
    TestEqual(TEXT("generated-header-only page remains byte-identical"),
        ActualHeaderOnly, HeaderOnlyBytes);

    FString NewManifest;
    TestTrue(TEXT("new ownership manifest is readable"),
        FFileHelper::LoadFileToString(NewManifest, *ManifestPath));
    TestEqual(TEXT("new ownership manifest excludes the successfully pruned candidate"),
        NewManifest, TestWikiDiskGenerator_BuildOwnershipManifest(
            { TEXT("bom-only.md"), TEXT("bom.md"), TEXT("current.md"),
                TEXT("registry.json") }));
    const int32 PruneOrder = OperationOrder.IndexOfByKey(TEXT("prune:managed-old.md"));
    const int32 ManifestPublishOrder = OperationOrder.IndexOfByKey(
        TEXT("publish:.pinwright-wiki-manifest.json"));
    TestTrue(TEXT("managed stale file prune is recorded"), PruneOrder != INDEX_NONE);
    TestTrue(TEXT("final manifest publication follows pruning"),
        ManifestPublishOrder != INDEX_NONE && ManifestPublishOrder > PruneOrder);
    TestEqual(TEXT("final manifest publication is the last reconciliation operation"),
        OperationOrder.IsEmpty() ? FString() : OperationOrder.Last(),
        FString(TEXT("publish:.pinwright-wiki-manifest.json")));

    TArray<uint8> CanonicalBomPageBytes;
    TestTrue(TEXT("canonicalized BOM page is readable"),
        FFileHelper::LoadFileToArray(CanonicalBomPageBytes, *BomPagePath));
    const TArray<uint8> ExpectedCanonicalBomPageBytes = { 's', 'a', 'm', 'e', '\n' };
    TestTrue(TEXT("equal-looking BOM page is rewritten as exact UTF-8 without BOM"),
        CanonicalBomPageBytes == ExpectedCanonicalBomPageBytes);
    TArray<uint8> CanonicalEmptyBytes;
    TestTrue(TEXT("canonicalized empty page is readable"),
        FFileHelper::LoadFileToArray(CanonicalEmptyBytes, *BomOnlyPath));
    TestEqual(TEXT("BOM-only page is rewritten as an exact empty file"),
        CanonicalEmptyBytes.Num(), 0);
    TestEqual(TEXT("successful reconciliation leaves no atomic temp files"),
        TestWikiDiskGenerator_CountTempFiles(Root), 0);
    TestEqual(TEXT("successful reconciliation removes its sibling staging directory"),
        TestWikiDiskGenerator_CountSiblingStageDirectories(Root), 0);

    const int32 MaxManifestBytes = static_cast<int32>(
        WikiDiskGenerator::Testing::MaxOwnershipManifestBytesForTests());
    const int32 MaxManifestEntries =
        WikiDiskGenerator::Testing::MaxOwnershipManifestEntriesForTests();
    TestEqual(TEXT("ownership manifest entry ceiling is 8192"),
        MaxManifestEntries, 8192);
    TestEqual(TEXT("the first rejected ownership count is 8193"),
        MaxManifestEntries + 1, 8193);

    const FString ByteBoundaryRoot = Root / TEXT("byte-boundary");
    const FString ByteBoundaryOldPath = ByteBoundaryRoot / TEXT("managed-old.md");
    const FString ByteBoundaryManifestPath = ByteBoundaryRoot / ManifestFilename;
    TestTrue(TEXT("seed byte-boundary stale page"),
        TestWikiDiskGenerator_WriteText(ByteBoundaryOldPath, TEXT("owned at byte boundary")));
    TestTrue(TEXT("seed manifest at exact byte boundary"),
        TestWikiDiskGenerator_WriteText(ByteBoundaryManifestPath,
            TestWikiDiskGenerator_PadManifestToBytes(
                TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("managed-old.md") }),
                MaxManifestBytes)));
    TestEqual(TEXT("manifest fixture reaches the exact byte boundary"),
        IFileManager::Get().FileSize(*ByteBoundaryManifestPath),
        static_cast<int64>(MaxManifestBytes));
    TestTrue(TEXT("manifest at exact byte boundary remains valid"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(ByteBoundaryRoot, DesiredFiles));
    TestFalse(TEXT("byte-boundary valid manifest authorizes owned prune"),
        IFileManager::Get().FileExists(*ByteBoundaryOldPath));

    const FString EntryBoundaryRoot = Root / TEXT("entry-boundary");
    const FString EntryBoundaryOldPath = EntryBoundaryRoot / TEXT("managed-old.md");
    const FString EntryBoundaryManifestPath = EntryBoundaryRoot / ManifestFilename;
    TestTrue(TEXT("seed entry-boundary stale page"),
        TestWikiDiskGenerator_WriteText(EntryBoundaryOldPath, TEXT("owned at entry boundary")));
    TestTrue(TEXT("seed manifest at exact entry boundary"),
        TestWikiDiskGenerator_WriteText(EntryBoundaryManifestPath,
            TestWikiDiskGenerator_BuildOwnershipManifest(
                TestWikiDiskGenerator_MakeManifestEntries(MaxManifestEntries))));
    TestTrue(TEXT("manifest at exact entry boundary remains valid"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(EntryBoundaryRoot, DesiredFiles));
    TestFalse(TEXT("entry-boundary valid manifest authorizes owned prune"),
        IFileManager::Get().FileExists(*EntryBoundaryOldPath));

    const FString NoncanonicalRoot = Root / TEXT("noncanonical-output");
    const FString NoncanonicalOwnedRoot = Root / TEXT("different-canonical-root");
    const FString NoncanonicalOldPath = NoncanonicalRoot / TEXT("managed-old.md");
    const FString NoncanonicalManifestPath = NoncanonicalRoot / ManifestFilename;
    TestTrue(TEXT("seed noncanonical stale managed page"),
        TestWikiDiskGenerator_WriteText(NoncanonicalOldPath, TEXT("must not prune")));
    TestTrue(TEXT("seed valid manifest in noncanonical output"),
        TestWikiDiskGenerator_WriteText(NoncanonicalManifestPath,
            TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("managed-old.md") })));
    const WikiDiskGenerator::Testing::FReconcileResult NoncanonicalResult =
        WikiDiskGenerator::Testing::ReconcileOutputForTestsDetailed(
            NoncanonicalRoot, NoncanonicalOwnedRoot, DesiredFiles);
    TestTrue(TEXT("noncanonical output still generates successfully"),
        NoncanonicalResult.bSucceeded);
    TestTrue(TEXT("noncanonical output reports pruneSkipped"),
        NoncanonicalResult.bPruneSkipped);
    TestEqual(TEXT("noncanonical output reports the stable prune-skip reason"),
        NoncanonicalResult.PruneSkipReason,
        WikiDiskGenerator::Testing::OutsideOwnedRootPruneSkipReason());
    TestTrue(TEXT("valid manifest never enables prune outside canonical root"),
        IFileManager::Get().FileExists(*NoncanonicalOldPath));

    struct FInvalidManifestCase
    {
        FString Name;
        bool bWriteManifest = true;
        FString Manifest;
    };

    TArray<FInvalidManifestCase> InvalidCases;
    InvalidCases.Add({ TEXT("missing"), false, FString() });
    InvalidCases.Add({ TEXT("malformed"), true, TEXT("{not-json") });
    InvalidCases.Add({ TEXT("wrong-owner"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest(
            { TEXT("managed-old.md") }, TEXT("Another.Owner")) });
    InvalidCases.Add({ TEXT("wrong-schema"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest(
            { TEXT("managed-old.md") }, TEXT("PinWright.WikiDiskGenerator"), 2) });
    InvalidCases.Add({ TEXT("absolute"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("c:/outside.md") }) });
    InvalidCases.Add({ TEXT("path-containing"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("nested/managed-old.md") }) });
    InvalidCases.Add({ TEXT("traversal"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("../managed-old.md") }) });
    InvalidCases.Add({ TEXT("unsafe-character"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("managed old.md") }) });
    InvalidCases.Add({ TEXT("unsorted"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest(
            { TEXT("z.md"), TEXT("managed-old.md") },
            TEXT("PinWright.WikiDiskGenerator"), 1, /*bSortFilenames=*/false) });
    InvalidCases.Add({ TEXT("duplicate"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest(
            { TEXT("managed-old.md"), TEXT("managed-old.md") }) });
    InvalidCases.Add({ TEXT("oversize-bytes"), true,
        TestWikiDiskGenerator_PadManifestToBytes(
            TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("managed-old.md") }),
            MaxManifestBytes + 1) });
    InvalidCases.Add({ TEXT("oversize-entries"), true,
        TestWikiDiskGenerator_BuildOwnershipManifest(
            TestWikiDiskGenerator_MakeManifestEntries(MaxManifestEntries + 1)) });

    for (const FInvalidManifestCase& Case : InvalidCases)
    {
        const FString CaseRoot = Root / Case.Name;
        const FString CaseOldPath = CaseRoot / TEXT("managed-old.md");
        const FString CaseManifestPath = CaseRoot / ManifestFilename;
        TestTrue(*FString::Printf(TEXT("%s: seed stale-looking page"), *Case.Name),
            TestWikiDiskGenerator_WriteText(CaseOldPath, TEXT("must survive")));
        if (Case.bWriteManifest)
        {
            TestTrue(*FString::Printf(TEXT("%s: seed manifest"), *Case.Name),
                TestWikiDiskGenerator_WriteText(CaseManifestPath, Case.Manifest));
        }

        TestTrue(*FString::Printf(TEXT("%s: reconciliation still publishes desired output"), *Case.Name),
            WikiDiskGenerator::Testing::ReconcileOutputForTests(CaseRoot, DesiredFiles));
        FString SurvivingBytes;
        TestTrue(*FString::Printf(TEXT("%s: invalid ownership authorizes zero deletion"), *Case.Name),
            FFileHelper::LoadFileToString(SurvivingBytes, *CaseOldPath));
        TestEqual(*FString::Printf(TEXT("%s: pre-existing page remains byte-identical"), *Case.Name),
            SurvivingBytes, FString(TEXT("must survive")));
        TestEqual(*FString::Printf(TEXT("%s: no atomic temp files remain"), *Case.Name),
            TestWikiDiskGenerator_CountTempFiles(CaseRoot), 0);
    }

    // A deletion failure retains ownership so the next generation can retry it.
    const FString RetryRoot = Root / TEXT("delete-failure");
    const FString RetryPath = RetryRoot / TEXT("managed-retry.md");
    const FString RetryManifestPath = RetryRoot / ManifestFilename;
    TestTrue(TEXT("seed retryable managed page"),
        TestWikiDiskGenerator_WriteText(RetryPath, TEXT("retry me")));
    TestTrue(TEXT("seed retry ownership manifest"),
        TestWikiDiskGenerator_WriteText(RetryManifestPath,
            TestWikiDiskGenerator_BuildOwnershipManifest({ TEXT("managed-retry.md") })));

    WikiDiskGenerator::Testing::FOperations RetryOperations;
    RetryOperations.DeleteFile = [&RetryPath](const FString& Path)
    {
        return Path != RetryPath
            && IFileManager::Get().Delete(*Path, /*RequireExists=*/false,
                /*EvenReadOnly=*/false, /*Quiet=*/true);
    };
    TestFalse(TEXT("failed managed deletion reports incomplete reconciliation"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(
            RetryRoot, DesiredFiles, RetryOperations));
    TestTrue(TEXT("failed managed deletion leaves the file for retry"),
        IFileManager::Get().FileExists(*RetryPath));

    FString RetryManifest;
    TestTrue(TEXT("retry ownership manifest is readable"),
        FFileHelper::LoadFileToString(RetryManifest, *RetryManifestPath));
    TestEqual(TEXT("failed deletion remains in the next ownership manifest"), RetryManifest,
        TestWikiDiskGenerator_BuildOwnershipManifest(
            { TEXT("current.md"), TEXT("managed-retry.md"), TEXT("registry.json") }));
    return true;
}

// ============================================================================
// A late staging failure returns before any final publication or prune. A per-call
// staging callback succeeds once, then fails the later registry stage without a
// global hook. Counterfactual: publishing while still staging changes the first
// final page before the later failure is known.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiDiskGeneratorWriteFailureSkipsPruneTest,
    "PinWright.infra.wiki_disk_generator.WriteFailureSkipsPruneAndManifestCommit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiDiskGeneratorWriteFailureSkipsPruneTest::RunTest(const FString& Parameters)
{
    const FString Root = TestWikiDiskGenerator_MakeScratchRoot();
    IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString CurrentPath = Root / TEXT("current.md");
    const FString RegistryPath = Root / TEXT("registry.json");
    const FString ManagedOldPath = Root / TEXT("managed-old.md");
    const FString ManifestPath = Root / WikiDiskGenerator::Testing::OwnershipManifestFilename();
    const FString OldCurrentBytes = TEXT("old current page");
    const FString OldRegistryBytes = TEXT("old registry");
    const FString OldManagedBytes = TEXT("old managed page");
    const FString OldManifestBytes = TestWikiDiskGenerator_BuildOwnershipManifest(
        { TEXT("current.md"), TEXT("managed-old.md"), TEXT("registry.json") });

    TestTrue(TEXT("seed existing current page"),
        TestWikiDiskGenerator_WriteText(CurrentPath, OldCurrentBytes));
    TestTrue(TEXT("seed managed stale page"),
        TestWikiDiskGenerator_WriteText(ManagedOldPath, OldManagedBytes));
    TestTrue(TEXT("seed existing registry"),
        TestWikiDiskGenerator_WriteText(RegistryPath, OldRegistryBytes));
    TestTrue(TEXT("seed previous ownership manifest"),
        TestWikiDiskGenerator_WriteText(ManifestPath, OldManifestBytes));

    TArray<uint8> OldCurrentFileBytes;
    TArray<uint8> OldRegistryFileBytes;
    TArray<uint8> OldManagedFileBytes;
    TArray<uint8> OldManifestFileBytes;
    TestTrue(TEXT("capture exact existing page bytes"),
        FFileHelper::LoadFileToArray(OldCurrentFileBytes, *CurrentPath));
    TestTrue(TEXT("capture exact existing registry bytes"),
        FFileHelper::LoadFileToArray(OldRegistryFileBytes, *RegistryPath));
    TestTrue(TEXT("capture exact managed stale bytes"),
        FFileHelper::LoadFileToArray(OldManagedFileBytes, *ManagedOldPath));
    TestTrue(TEXT("capture exact ownership manifest bytes"),
        FFileHelper::LoadFileToArray(OldManifestFileBytes, *ManifestPath));

    WikiDiskGenerator::Testing::FOperations Operations;
    int32 SuccessfulStageWrites = 0;
    Operations.StageFile = [&SuccessfulStageWrites](
        const FString& Path, TArrayView<const uint8> Bytes, FString& OutError)
    {
        if (FPaths::GetCleanFilename(Path) == TEXT("registry.json"))
        {
            OutError = TEXT("injected late wiki staging failure");
            return false;
        }

        const AtomicFileWriter::FResult Result = AtomicFileWriter::WriteBytes(
            Path, Bytes, AtomicFileWriter::EExistingFilePolicy::FailIfExists);
        if (Result.IsSuccess())
        {
            ++SuccessfulStageWrites;
        }
        OutError = Result.Error;
        return Result.IsSuccess();
    };
    bool bFinalPublishAttempted = false;
    Operations.WriteFile = [&bFinalPublishAttempted](
        const FString&, TArrayView<const uint8>, FString& OutError)
    {
        bFinalPublishAttempted = true;
        OutError = TEXT("final publication must not run after staging failure");
        return false;
    };

    TestFalse(TEXT("injected late staging failure aborts reconciliation"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(
            Root, TestWikiDiskGenerator_MakeDesiredFiles(), Operations));
    TestEqual(TEXT("one earlier file was staged before the late failure"),
        SuccessfulStageWrites, 1);
    TestFalse(TEXT("late staging failure prevents every final publication"),
        bFinalPublishAttempted);

    TArray<uint8> CurrentFileBytes;
    TestTrue(TEXT("existing page remains readable after failed staging"),
        FFileHelper::LoadFileToArray(CurrentFileBytes, *CurrentPath));
    TestTrue(TEXT("existing page remains byte-identical after failed staging"),
        CurrentFileBytes == OldCurrentFileBytes);

    TArray<uint8> RegistryFileBytes;
    TestTrue(TEXT("registry remains readable after failed staging"),
        FFileHelper::LoadFileToArray(RegistryFileBytes, *RegistryPath));
    TestTrue(TEXT("registry remains byte-identical after failed staging"),
        RegistryFileBytes == OldRegistryFileBytes);

    TArray<uint8> ManagedFileBytes;
    TestTrue(TEXT("managed stale page is not pruned after output failure"),
        FFileHelper::LoadFileToArray(ManagedFileBytes, *ManagedOldPath));
    TestTrue(TEXT("managed stale page remains byte-identical"),
        ManagedFileBytes == OldManagedFileBytes);

    TArray<uint8> ManifestFileBytes;
    TestTrue(TEXT("old ownership manifest remains readable"),
        FFileHelper::LoadFileToArray(ManifestFileBytes, *ManifestPath));
    TestTrue(TEXT("old ownership manifest remains byte-identical"),
        ManifestFileBytes == OldManifestFileBytes);
    TestEqual(TEXT("failed staging leaves no atomic temporary files"),
        TestWikiDiskGenerator_CountTempFiles(Root), 0);
    TestEqual(TEXT("failed staging removes its sibling staging directory"),
        TestWikiDiskGenerator_CountSiblingStageDirectories(Root), 0);

    const FString AbsentOutputRoot = Root / TEXT("absent-output");
    TestFalse(TEXT("absent-output fixture starts without a final output root"),
        IFileManager::Get().DirectoryExists(*AbsentOutputRoot));
    SuccessfulStageWrites = 0;
    bFinalPublishAttempted = false;
    TestFalse(TEXT("late staging failure also aborts for an initially absent output root"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(
            AbsentOutputRoot, TestWikiDiskGenerator_MakeDesiredFiles(), Operations));
    TestEqual(TEXT("absent-output failure occurs after an earlier staged file"),
        SuccessfulStageWrites, 1);
    TestFalse(TEXT("absent-output staging failure attempts no final publication"),
        bFinalPublishAttempted);
    TestFalse(TEXT("late staging failure does not create the absent final output root"),
        IFileManager::Get().DirectoryExists(*AbsentOutputRoot));
    TestEqual(TEXT("absent-output failure removes its sibling staging directory"),
        TestWikiDiskGenerator_CountSiblingStageDirectories(AbsentOutputRoot), 0);

    const int32 MaxManifestEntries =
        WikiDiskGenerator::Testing::MaxOwnershipManifestEntriesForTests();
    const int64 MaxManifestBytes =
        WikiDiskGenerator::Testing::MaxOwnershipManifestBytesForTests();
    const FString LimitManifestText = TestWikiDiskGenerator_BuildOwnershipManifest(
        { TEXT("managed-old.md") });

    const FString OverEntryRoot = Root / TEXT("outgoing-over-entry");
    const FString OverEntryOldPath = OverEntryRoot / TEXT("managed-old.md");
    const FString OverEntryManifestPath =
        OverEntryRoot / WikiDiskGenerator::Testing::OwnershipManifestFilename();
    TestTrue(TEXT("seed outgoing-entry-limit stale page"),
        TestWikiDiskGenerator_WriteText(OverEntryOldPath, OldManagedBytes));
    TestTrue(TEXT("seed outgoing-entry-limit prior manifest"),
        TestWikiDiskGenerator_WriteText(OverEntryManifestPath, LimitManifestText));
    TArray<uint8> LimitManifestFileBytes;
    TestTrue(TEXT("capture outgoing-limit prior manifest bytes"),
        FFileHelper::LoadFileToArray(LimitManifestFileBytes, *OverEntryManifestPath));
    TestFalse(TEXT("8,193-entry outgoing ownership set is rejected before prune"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(
            OverEntryRoot,
            TestWikiDiskGenerator_MakeLargeDesiredFiles(MaxManifestEntries, 0)));

    TArray<uint8> OverEntryOldFileBytes;
    TArray<uint8> OverEntryManifestBytes;
    TestTrue(TEXT("entry-limit stale page remains readable"),
        FFileHelper::LoadFileToArray(OverEntryOldFileBytes, *OverEntryOldPath));
    TestTrue(TEXT("entry-limit stale page remains byte-identical"),
        OverEntryOldFileBytes == OldManagedFileBytes);
    TestTrue(TEXT("entry-limit prior manifest remains readable"),
        FFileHelper::LoadFileToArray(OverEntryManifestBytes, *OverEntryManifestPath));
    TestTrue(TEXT("entry-limit prior manifest remains byte-identical"),
        OverEntryManifestBytes == LimitManifestFileBytes);

    const FString OverByteRoot = Root / TEXT("outgoing-over-byte");
    const FString OverByteOldPath = OverByteRoot / TEXT("managed-old.md");
    const FString OverByteManifestPath =
        OverByteRoot / WikiDiskGenerator::Testing::OwnershipManifestFilename();
    TestTrue(TEXT("seed outgoing-byte-limit stale page"),
        TestWikiDiskGenerator_WriteText(OverByteOldPath, OldManagedBytes));
    TestTrue(TEXT("seed outgoing-byte-limit prior manifest"),
        TestWikiDiskGenerator_WriteText(OverByteManifestPath, LimitManifestText));

    TMap<FString, FString> OverByteDesiredFiles =
        TestWikiDiskGenerator_MakeLargeDesiredFiles(MaxManifestEntries - 1, 512);
    TArray<FString> OverByteProspectiveFilenames;
    OverByteDesiredFiles.GenerateKeyArray(OverByteProspectiveFilenames);
    OverByteProspectiveFilenames.Add(TEXT("managed-old.md"));
    const FString OverByteProspectiveManifest =
        TestWikiDiskGenerator_BuildOwnershipManifest(OverByteProspectiveFilenames);
    const FTCHARToUTF8 OverByteProspectiveUtf8(
        *OverByteProspectiveManifest, OverByteProspectiveManifest.Len());
    TestEqual(TEXT("outgoing byte-limit fixture remains within the entry limit"),
        OverByteProspectiveFilenames.Num(), MaxManifestEntries);
    TestTrue(TEXT("outgoing byte-limit fixture exceeds the exact UTF-8 byte limit"),
        static_cast<int64>(OverByteProspectiveUtf8.Length()) > MaxManifestBytes);
    TestFalse(TEXT("outgoing ownership set above byte limit is rejected before prune"),
        WikiDiskGenerator::Testing::ReconcileOutputForTests(
            OverByteRoot, OverByteDesiredFiles));

    TArray<uint8> OverByteOldFileBytes;
    TArray<uint8> OverByteManifestBytes;
    TestTrue(TEXT("byte-limit stale page remains readable"),
        FFileHelper::LoadFileToArray(OverByteOldFileBytes, *OverByteOldPath));
    TestTrue(TEXT("byte-limit stale page remains byte-identical"),
        OverByteOldFileBytes == OldManagedFileBytes);
    TestTrue(TEXT("byte-limit prior manifest remains readable"),
        FFileHelper::LoadFileToArray(OverByteManifestBytes, *OverByteManifestPath));
    TestTrue(TEXT("byte-limit prior manifest remains byte-identical"),
        OverByteManifestBytes == LimitManifestFileBytes);
    return true;
}

#endif
