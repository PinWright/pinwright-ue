// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc-vs-source reconciliation for the shipped .pwmodel example corpus, in the shape of
// TestPwModelDiagnosticCatalog.cpp beside it.
//
// THE DEFECT THIS GUARDS IS DUPLICATION, NOT A WRONG NUMBER. Three mesh -> asset triangle pairs
// and three vertex pairs describing the shipped examples were hand-copied into
// docs/pwmodel-format.md and four C++ comment blocks. All five copies were byte-identical, so no
// comparison between them could ever surface drift - and all five drifted together as the examples
// were reworked, ending up contradicted by the dated corpus table by more than a factor of ten in
// one case before anyone noticed. The pairs are withdrawn from the doc and the four comment blocks
// now cite the section instead of restating it.
//
// DELIBERATELY NOT AN EXACT-COUNT ASSERTION. A shipped example's triangle count is a function of
// the engine's tessellation and of the example itself; it can move legitimately between UE
// versions and between reworks, so a pinned number here would fail honestly-passing builds and
// would itself become a sixth hand-copy. What is asserted is version-independent:
//
//   1. The published corpus table and the shipped files name the same set of examples.
//   2. The doc section the source comments cite still exists, under the title they cite.
//   3. No source comment pairs a shipped example's name with a count again.
//
// (3) is the one that stops regrowth. (2) is what stops a doc rename from quietly orphaning every
// citation (1) and (3) push authors towards.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

namespace
{
    // The H3 the four comment blocks cite by title. Written once here so the test fails loudly if
    // the doc heading is renamed, rather than leaving four comments pointing at nothing.
    const TCHAR* PwExampleCatalog_CitedSectionTitle()
    {
        return TEXT("Four counts: two for the mesh, two for the asset");
    }

    FString PwExampleCatalog_ResolvePluginDir()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
    }

    // Base names of every shipped Examples/pwmodel/*.pwmodel, discovered rather than listed, so a
    // new example is covered the moment it lands.
    TArray<FString> PwExampleCatalog_ShippedExampleNames(const FString& PluginDir)
    {
        TArray<FString> FileNames;
        IFileManager::Get().FindFiles(FileNames,
            *(PluginDir / TEXT("Examples") / TEXT("pwmodel") / TEXT("*.pwmodel")),
            /*Files=*/true, /*Directories=*/false);

        TArray<FString> Names;
        for (const FString& FileName : FileNames)
        {
            Names.Add(FPaths::GetBaseFilename(FileName));
        }
        Names.Sort();
        return Names;
    }

    // A skeletal example needs a skeleton asset to compile and is not part of the static-mesh
    // corpus table, so it is exempt from the row reconciliation and checked separately.
    bool PwExampleCatalog_IsSkeletal(const FString& PluginDir, const FString& Name)
    {
        FString Contents;
        const FString Path = PluginDir / TEXT("Examples") / TEXT("pwmodel")
            / (Name + TEXT(".pwmodel"));
        if (!FFileHelper::LoadFileToString(Contents, *Path))
        {
            return false;
        }
        return Contents.Contains(TEXT("use skeleton"));
    }

    // The first column of the corpus table in docs/wiki-src/model.examples.md - the table whose
    // header row is "| Example | Tris | ...". Rows are read only from that table so a later table
    // on the same page cannot contribute phantom names.
    TArray<FString> PwExampleCatalog_TabulatedNames(const FString& DocContents)
    {
        TArray<FString> Lines;
        DocContents.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

        const FRegexPattern HeaderPattern(TEXT("^\\|\\s*Example\\s*\\|"));
        const FRegexPattern RowPattern(TEXT("^\\|\\s*`([A-Za-z0-9_]+)`\\s*\\|"));

        TArray<FString> Names;
        bool bInTable = false;
        for (const FString& Line : Lines)
        {
            if (!bInTable)
            {
                FRegexMatcher HeaderMatcher(HeaderPattern, Line);
                bInTable = HeaderMatcher.FindNext();
                continue;
            }

            if (!Line.StartsWith(TEXT("|")))
            {
                break;
            }

            FRegexMatcher RowMatcher(RowPattern, Line);
            if (RowMatcher.FindNext())
            {
                Names.Add(RowMatcher.GetCaptureGroup(1));
            }
        }
        Names.Sort();
        return Names;
    }

    TArray<FString> PwExampleCatalog_SourceFiles(const FString& PluginDir)
    {
        TArray<FString> Files;
        const FString SourceDir = PluginDir / TEXT("Source");
        IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*.cpp"),
            /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/false);
        IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*.h"),
            /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/false);
        Files.Sort();
        return Files;
    }

    FString PwExampleCatalog_NameAlternation(const TArray<FString>& Names)
    {
        return FString::Join(Names, TEXT("|"));
    }

    // Two shapes, because the withdrawn copies used both. A standalone three-or-more-digit count
    // straight after the name ("gothic_window 8174"), and an explicit pair whose first term may be
    // small ("origami_crane 50 -> 162"). Requiring whitespace immediately after the name is what
    // keeps "crystal_cluster.pwmodel answered 17,815 characters" - a real comment about response
    // size, not a mesh count - out of the match.
    void PwExampleCatalog_FindCountCopies(
        const FString& PluginDir, const TArray<FString>& Names, TArray<FString>& OutOffenders,
        int32& OutCitingFiles)
    {
        const FString Alternation = PwExampleCatalog_NameAlternation(Names);
        const FRegexPattern StandalonePattern(FString::Printf(
            TEXT("(%s)\\s+[^A-Za-z0-9]{0,4}([0-9]{1,3}(,[0-9]{3})+|[0-9]{3,})"), *Alternation));
        const FRegexPattern PairPattern(FString::Printf(
            TEXT("(%s)\\s+[^A-Za-z0-9]{0,4}[0-9][0-9,]*\\s*(->|against|versus|vs)\\s*[0-9]"),
            *Alternation));

        const FString CitedTitle = PwExampleCatalog_CitedSectionTitle();
        OutCitingFiles = 0;

        for (const FString& File : PwExampleCatalog_SourceFiles(PluginDir))
        {
            FString Contents;
            if (!FFileHelper::LoadFileToString(Contents, *File))
            {
                continue;
            }

            // This file is itself the statement of the forbidden shape, so its own patterns and
            // worked counter-examples must not be read as violations - and it must not count as
            // one of the citing files either, or "somebody still cites the doc" passes vacuously
            // on nothing but this test.
            if (FPaths::GetCleanFilename(File) == TEXT("TestPwModelExampleCatalog.cpp"))
            {
                continue;
            }

            if (Contents.Contains(CitedTitle))
            {
                ++OutCitingFiles;
            }

            bool bMentionsAnyExample = false;
            for (const FString& Name : Names)
            {
                if (Contents.Contains(Name))
                {
                    bMentionsAnyExample = true;
                    break;
                }
            }
            if (!bMentionsAnyExample)
            {
                continue;
            }

            TArray<FString> Lines;
            Contents.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
            for (int32 Index = 0; Index < Lines.Num(); ++Index)
            {
                FRegexMatcher StandaloneMatcher(StandalonePattern, Lines[Index]);
                FRegexMatcher PairMatcher(PairPattern, Lines[Index]);
                if (StandaloneMatcher.FindNext() || PairMatcher.FindNext())
                {
                    OutOffenders.Add(FString::Printf(TEXT("%s:%d  %s"),
                        *FPaths::GetCleanFilename(File), Index + 1, *Lines[Index].TrimStartAndEnd()));
                }
            }
        }
    }

    TArray<FString> PwExampleCatalog_SortedDifference(
        const TArray<FString>& Left, const TArray<FString>& Right)
    {
        TArray<FString> Only;
        for (const FString& Item : Left)
        {
            if (!Right.Contains(Item))
            {
                Only.Add(Item);
            }
        }
        return Only;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelExampleCorpusMatchesTheTableTest,
    "PinWright.core.pwmodel_examples.ShippedFilesMatchThePublishedTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelExampleCorpusMatchesTheTableTest::RunTest(const FString& Parameters)
{
    const FString PluginDir = PwExampleCatalog_ResolvePluginDir();
    if (!TestFalse(TEXT("Resolved the PinWright plugin directory"), PluginDir.IsEmpty()))
    {
        return false;
    }

    const TArray<FString> Shipped = PwExampleCatalog_ShippedExampleNames(PluginDir);
    if (!TestTrue(TEXT("Found shipped .pwmodel examples on disk"), Shipped.Num() > 0))
    {
        return false;
    }

    const FString DocPath = PluginDir / TEXT("docs") / TEXT("wiki-src")
        / TEXT("model.examples.md");
    FString DocContents;
    if (!TestTrue(TEXT("Read docs/wiki-src/model.examples.md"),
            FFileHelper::LoadFileToString(DocContents, *DocPath)))
    {
        return false;
    }

    const TArray<FString> Tabulated = PwExampleCatalog_TabulatedNames(DocContents);
    // Guards the scan itself: a table header or row shape that stops matching must not turn the
    // comparisons below into a vacuous pass.
    if (!TestTrue(TEXT("The corpus table carries at least one example row"), Tabulated.Num() > 0))
    {
        return false;
    }

    FString SkeletalDocContents;
    const FString FormatDocPath = PluginDir / TEXT("docs") / TEXT("pwmodel-format.md");
    if (!TestTrue(TEXT("Read docs/pwmodel-format.md"),
            FFileHelper::LoadFileToString(SkeletalDocContents, *FormatDocPath)))
    {
        return false;
    }

    TArray<FString> ExpectedRows;
    for (const FString& Name : Shipped)
    {
        if (PwExampleCatalog_IsSkeletal(PluginDir, Name))
        {
            // Exempt from the static-mesh corpus table, but not from being documented at all.
            TestTrue(*FString::Printf(
                TEXT("Skeletal example '%s' is named somewhere in docs/pwmodel-format.md"), *Name),
                SkeletalDocContents.Contains(Name));
            continue;
        }
        ExpectedRows.Add(Name);
    }

    for (const FString& Missing : PwExampleCatalog_SortedDifference(ExpectedRows, Tabulated))
    {
        AddError(FString::Printf(
            TEXT("Examples/pwmodel/%s.pwmodel ships but has no row in the corpus table of ")
            TEXT("docs/wiki-src/model.examples.md. Add the row (with the date it was measured) ")
            TEXT("rather than leaving the corpus and its published table disagreeing."), *Missing));
    }

    for (const FString& Phantom : PwExampleCatalog_SortedDifference(Tabulated, ExpectedRows))
    {
        AddError(FString::Printf(
            TEXT("The corpus table in docs/wiki-src/model.examples.md has a row for '%s', but ")
            TEXT("Examples/pwmodel/%s.pwmodel is not a shipped non-skeletal example. Remove the ")
            TEXT("row or restore the file."), *Phantom, *Phantom));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelExampleCountsAreCitedNotCopiedTest,
    "PinWright.core.pwmodel_examples.SourceCitesTheCountTableRatherThanCopyingIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelExampleCountsAreCitedNotCopiedTest::RunTest(const FString& Parameters)
{
    const FString PluginDir = PwExampleCatalog_ResolvePluginDir();
    if (!TestFalse(TEXT("Resolved the PinWright plugin directory"), PluginDir.IsEmpty()))
    {
        return false;
    }

    const TArray<FString> Shipped = PwExampleCatalog_ShippedExampleNames(PluginDir);
    if (!TestTrue(TEXT("Found shipped .pwmodel examples on disk"), Shipped.Num() > 0))
    {
        return false;
    }

    const FString FormatDocPath = PluginDir / TEXT("docs") / TEXT("pwmodel-format.md");
    FString FormatDocContents;
    if (!TestTrue(TEXT("Read docs/pwmodel-format.md"),
            FFileHelper::LoadFileToString(FormatDocContents, *FormatDocPath)))
    {
        return false;
    }

    const FString CitedTitle = PwExampleCatalog_CitedSectionTitle();
    TestTrue(*FString::Printf(
        TEXT("docs/pwmodel-format.md still carries the heading source comments cite: \"%s\""),
        *CitedTitle),
        FormatDocContents.Contains(FString(TEXT("# ")) + CitedTitle));

    TArray<FString> Offenders;
    int32 CitingFiles = 0;
    PwExampleCatalog_FindCountCopies(PluginDir, Shipped, Offenders, CitingFiles);

    TestTrue(TEXT("At least one source comment cites the count section by title rather than ")
        TEXT("restating its numbers"), CitingFiles > 0);

    for (const FString& Offender : Offenders)
    {
        AddError(FString::Printf(
            TEXT("A shipped example's count is written into source again: %s\n")
            TEXT("Counts for the shipped examples live in exactly two places - the mesh column of ")
            TEXT("docs/wiki-src/model.examples.md, and a live model.compile response for the asset ")
            TEXT("side. Cite \"%s\" in docs/pwmodel-format.md instead; a hand-copied count in a ")
            TEXT("comment cannot be reconciled with anything and has gone stale every time."),
            *Offender, *CitedTitle));
    }

    return true;
}
