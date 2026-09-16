// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestIrAndNestedLoadGuard.cpp - THE RATCHET ON THE POINT-OF-USE LOAD GUARD.
//
// Tests/Core/TestGuardedLoadPathSafety.cpp locks what PinWrightGuardedLoad::LoadObjectChecked
// (Utils/GuardedLoad.h) DOES. This file locks WHO GOES THROUGH IT, which is the other half of the
// claim and the half that rots: a guard nothing calls is a guard that is not there.
//
// WHY A SOURCE SCAN AND NOT A BEHAVIOURAL TEST. CreatePackage logs at Fatal on a package name
// containing "//" (UObjectGlobals.cpp:1094-1096) - not compiled out in any configuration - and
// every LOAD is a door to it, because StaticLoadObjectInternal calls ResolveName2(..., Create=true)
// (:1427), which calls CreatePackage on the partial name (:1310). A test that drove an unguarded
// site would therefore END the suite host instead of reporting a red, which is an ABSENCE of a
// signal rather than a failure one. Reading the source is the only way to assert this class
// without a suicide run.
//
// ---------------------------------------------------------------------------------------------
// THE TWO SCOPES, AND WHY THEY ARE ENFORCED DIFFERENTLY
// ---------------------------------------------------------------------------------------------
//
// SCOPE A - THE IR COMPILER TREES, HELD AT ZERO. AGIR / BTIR / CRIR / MGIR / MSIR / NIR / SCIR /
// Compiler (BPIR) / IrCore. Board B-ir-source-class-refs-reach-createpackage-fatal: a class or
// asset reference in these trees is a SUBSTRING of the `text` parameter, an IR document that must
// stay typed `string` at the dispatch boundary - typing it `path` is nonsense and `classref` would
// refuse every valid program - and no IR tokenizer filters '/' (the comment character is '#';
// FIrTextUtils::StripTrailingComment, FIrTokenizer and BpirParser all cut only at an unquoted '#',
// and Unquote returns its input verbatim). So EVERY load in these trees is on parser output from
// caller text, with no exception, which is what makes zero the honest number here rather than a
// baseline. A tenth IR compiler added next year inherits the rule by living in the tree.
//
// SCOPE B - THE CONFIRMED NESTED-VALUE FILES, HELD AT A COUNT. Board
// B-nested-path-values-reach-createpackage-fatal: a path inside an array element or in the VALUE
// half of a map is invisible to the dispatch-boundary type gate, which reads top-level params
// only. These files are NOT held at zero, and the distinction is real rather than laziness: their
// remaining loads read TOP-LEVEL parameters that the gate already types `path`, so converting them
// would be defence in depth rather than a fix, and doing it inside these two tickets would mean a
// 40-line drive-by in files other work is editing. The count is what stops a NEW nested reacher
// landing quietly in a file that already has loads - the failure mode a name-only list cannot see.
// Lower an entry as its file converts; a count that DROPPED warns rather than fails.
//
// A LITERAL ARGUMENT IS EXEMPT, IN BOTH SCOPES. `LoadObject<UBlueprint>(nullptr,
// TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"))` is a constant this
// plugin wrote; no caller text reaches it, and routing it through the guard would buy nothing but
// noise. The exemption is decided by looking at the ARGUMENTS, not by naming the site, so it
// cannot be used to park a variable somebody did not want to convert.
//
// UNABLE TO FAIL IF - each asserted, because every one of them turns this into a test that passes
// by checking nothing:
//   * the plugin does not resolve through IPluginManager, or Source/ is not on disk;
//   * a scoped root is missing from disk (a rename that silently drops a whole tree from the scan);
//   * the recursive walk returns no source files;
//   * either regex has stopped working - checked against a FIXED SAMPLE, not against the tree,
//     because Scope A is held at zero and "the scan found a call" is therefore not something a
//     clean tree can promise. It is also what an ICU-less build looks like, where
//     FRegexMatcher::FindNext never returns true and every regex here silently matches nothing;
//   * the guarded-call pattern matches nothing in the scanned files, i.e. the conversion this
//     ratchet guards was reverted wholesale and every site reads as "no loads here".
//
// THE SCAN READS CODE, NOT PROSE. Input goes through NeutralizeSourceText (Tests/TestUtils.h)
// first: these files discuss `LoadObject<UClass>` in their own comments, and a commented mention
// scored as a call would fail a tree that contains no such call. That is board
// B-error-code-adoption-test-scans-comments, where one explanatory sentence flipped a file's
// verdict.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

#include "Tests/TestUtils.h"

namespace IrLoadGuardHelpers
{
    FString ResolveSourceRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
    }

    // Path relative to <Plugin>/Source, forward-slashed. Both sides go to full paths first because
    // IPlugin::GetBaseDir can hand back an engine-relative path while FindFilesRecursive echoes
    // whatever root it was given, and two different forms make MakePathRelativeTo return its input
    // unchanged - which would miss every baseline entry rather than erroring.
    FString MakeRelative(const FString& AnyPath, const FString& SourceRoot)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(AnyPath);
        const FString FullRoot = FPaths::ConvertRelativePathToFull(SourceRoot) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullRoot);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    // SCOPE A. Every IR compiler tree in the main module. Source-relative, so they double as the
    // failure message's vocabulary.
    const TCHAR* const IrTreeRoots[] = {
        TEXT("PinWright/Private/AGIR"),
        TEXT("PinWright/Private/BTIR"),
        TEXT("PinWright/Private/CRIR"),
        TEXT("PinWright/Private/Compiler"),
        TEXT("PinWright/Private/IrCore"),
        TEXT("PinWright/Private/MGIR"),
        TEXT("PinWright/Private/MSIR"),
        TEXT("PinWright/Private/NIR"),
        TEXT("PinWright/Private/SCIR"),
    };

    // SCOPE B. "<source-relative file>|<unguarded loads still permitted>", MEASURED after the
    // nested-value conversion rather than chosen. A zero is a hard zero: every load in that file
    // was a nested reacher and all of them converted.
    //
    // Each entry names a file whose nested-value load is now guarded; the count is what its
    // TOP-LEVEL-parameter loads still add up to. Those are already covered at the dispatch boundary
    // by their `path` declarations, which is why they are permitted here and why the honest number
    // is not zero.
    const TCHAR* const NestedFileBaseline[] = {
        TEXT("PinWright/Private/Handlers/Audio/AudioMusicHandler.cpp|0"),
        TEXT("PinWright/Private/Handlers/Environment/FoliageHandler.cpp|12"),
        TEXT("PinWright/Private/Handlers/Material/MaterialGraphHandler.cpp|2"),
        TEXT("PinWright/Private/Handlers/Material/MaterialInstanceOverrides.h|0"),
        TEXT("PinWright/Private/Handlers/Systems/GASHandler.cpp|23"),
    };

    // A load call. `LoadObjectChecked<` does not match `LoadObject<`, so the guarded spelling is
    // invisible to this pattern by construction rather than by an exclusion that could drift.
    const TCHAR* const LoadCallPattern =
        TEXT("(?:LoadObject<|LoadClass<|StaticLoadObject\\s*\\()");

    // The guarded spelling, used only for the "did this test go blind" assertion.
    const TCHAR* const GuardedCallPattern = TEXT("PinWrightGuardedLoad::LoadObjectChecked");

    // Is the object-name argument of the call starting at MatchEnd a string LITERAL? Read up to the
    // statement terminator: a call whose arguments are all constants cannot carry caller text.
    // Deliberately crude and deliberately CONSERVATIVE in the safe direction - a call this
    // mis-reads as literal would have to have a TEXT("...") inside its own argument list, and the
    // cost of that miss is one unguarded site staying unflagged, never a false failure that blocks
    // a build.
    bool CallArgumentsAreLiteral(const FString& Contents, int32 MatchEnd)
    {
        const int32 Terminator = Contents.Find(TEXT(";"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, MatchEnd);
        const int32 End = (Terminator == INDEX_NONE) ? Contents.Len() : Terminator;
        const FString Args = Contents.Mid(MatchEnd, End - MatchEnd);
        return Args.Contains(TEXT("TEXT("));
    }

    struct FScan
    {
        int32 FilesRead = 0;
        int32 GuardedCallsSeen = 0;
        // source-relative file -> unguarded, non-literal load calls in it.
        TMap<FString, int32> UnguardedByFile;
    };

    void ScanFile(const FString& File, const FString& SourceRoot, FScan& Scan)
    {
        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *File))
        {
            return;
        }
        Scan.FilesRead++;

        const FString Contents = NeutralizeSourceText(RawContents);
        const FString Relative = MakeRelative(File, SourceRoot);

        {
            const FRegexPattern Guarded(GuardedCallPattern);
            FRegexMatcher Matcher(Guarded, Contents);
            while (Matcher.FindNext())
            {
                Scan.GuardedCallsSeen++;
            }
        }

        const FRegexPattern Compiled(LoadCallPattern);
        FRegexMatcher Matcher(Compiled, Contents);
        while (Matcher.FindNext())
        {
            if (CallArgumentsAreLiteral(Contents, Matcher.GetMatchEnding()))
            {
                continue;
            }
            Scan.UnguardedByFile.FindOrAdd(Relative)++;
        }
    }

    TArray<FString> CollectSourceFiles(const FString& Dir)
    {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.cpp"), true, false, false);
        IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.h"), true, false, false);
        return Files;
    }

    // THE BLINDNESS GUARD, RUN AGAINST A FIXED SAMPLE RATHER THAN AGAINST THE TREE. A "the scan
    // matched at least one call" assertion cannot be used for Scope A: that tree is held at zero
    // and its only remaining load is a literal, so removing that one literal would turn a
    // legitimately clean tree into a red. Matching a constant instead detects the failure that
    // actually matters - a broken pattern, or an ICU-less build where FRegexMatcher::FindNext never
    // returns true and every regex in this file silently matches nothing.
    bool PatternsAreFunctional(FAutomationTestBase& Test)
    {
        const FString Sample = NeutralizeSourceText(
            TEXT("UClass* A = LoadObject<UClass>(nullptr, *P);\n")
            TEXT("UClass* B = LoadClass<UObject>(nullptr, *P);\n")
            TEXT("UObject* C = StaticLoadObject(UObject::StaticClass(), nullptr, *P);\n")
            TEXT("UClass* D = PinWrightGuardedLoad::LoadObjectChecked<UClass>(P);\n"));

        int32 LoadHits = 0;
        {
            const FRegexPattern Compiled(LoadCallPattern);
            FRegexMatcher Matcher(Compiled, Sample);
            while (Matcher.FindNext())
            {
                LoadHits++;
            }
        }
        int32 GuardedHits = 0;
        {
            const FRegexPattern Compiled(GuardedCallPattern);
            FRegexMatcher Matcher(Compiled, Sample);
            while (Matcher.FindNext())
            {
                GuardedHits++;
            }
        }

        // Three, not "at least one": the guarded spelling must NOT be counted as a raw load, and a
        // count of four would mean this whole ratchet reports every converted site as a violation.
        Test.TestEqual(TEXT("The load-call pattern matches the three raw spellings and no more"),
            LoadHits, 3);
        Test.TestEqual(TEXT("The guarded-call pattern matches the guarded spelling"),
            GuardedHits, 1);
        return LoadHits == 3 && GuardedHits == 1;
    }

    TMap<FString, int32> ParseBaseline(FAutomationTestBase& Test)
    {
        TMap<FString, int32> Baseline;
        for (const TCHAR* const Entry : NestedFileBaseline)
        {
            const FString Line(Entry);
            FString Head;
            FString CountText;
            if (!Line.Split(TEXT("|"), &Head, &CountText, ESearchCase::CaseSensitive,
                            ESearchDir::FromEnd)
                || Head.IsEmpty() || !CountText.IsNumeric())
            {
                Test.AddError(FString::Printf(
                    TEXT("Malformed baseline entry '%s' (expected file|count)."), *Line));
                continue;
            }
            Baseline.Add(Head, FCString::Atoi(*CountText));
        }
        return Baseline;
    }
}

// ============================================================================
// Scope A: no IR compiler loads caller text raw
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIrCompilerLoadsAreGuardedTest,
    "PinWright.infra.contract.LoadGuard.IrCompilersLoadThroughTheGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIrCompilerLoadsAreGuardedTest::RunTest(const FString& Parameters)
{
    using namespace IrLoadGuardHelpers;

    const FString SourceRoot = ResolveSourceRoot();
    if (!TestFalse(TEXT("Resolved the plugin Source root"), SourceRoot.IsEmpty()))
    {
        return false;
    }

    FScan Scan;
    TArray<FString> AllFiles;
    for (const TCHAR* const Root : IrTreeRoots)
    {
        const FString Dir = SourceRoot / Root;
        // A root that vanished is a whole IR tree dropping out of the scan unnoticed, which is the
        // failure this ratchet is least able to survive: it would keep passing while covering less.
        if (!TestTrue(FString::Printf(TEXT("IR tree '%s' exists on disk"), Root),
                      IFileManager::Get().DirectoryExists(*Dir)))
        {
            return false;
        }
        AllFiles.Append(CollectSourceFiles(Dir));
    }

    if (!TestTrue(TEXT("Recursive walk found IR source files"), AllFiles.Num() > 0))
    {
        return false;
    }

    for (const FString& File : AllFiles)
    {
        ScanFile(File, SourceRoot, Scan);
    }

    if (!TestTrue(TEXT("Read at least one IR source file"), Scan.FilesRead > 0))
    {
        return false;
    }

    // BLINDNESS GUARD ONE: the patterns still work at all (see PatternsAreFunctional).
    if (!PatternsAreFunctional(*this))
    {
        return false;
    }
    // BLINDNESS GUARD TWO: the conversion this ratchet guards has not been reverted wholesale. If
    // every guarded call disappeared, "zero unguarded loads" would be true of a tree that no longer
    // loads anything through the guard - which is the shape of a revert, not of a clean tree.
    if (!TestTrue(TEXT("The IR trees still call the guard"), Scan.GuardedCallsSeen > 0))
    {
        return false;
    }

    for (const TPair<FString, int32>& Pair : Scan.UnguardedByFile)
    {
        AddError(FString::Printf(
            TEXT("%s performs %d load(s) on a non-literal path without "
                 "PinWrightGuardedLoad::LoadObjectChecked. Every string an IR compiler loads is "
                 "parser output from the caller's `text` parameter, which the dispatch boundary "
                 "cannot type as a path - so a doubled slash reaches CreatePackage's Fatal and ends "
                 "the editor process. Board B-ir-source-class-refs-reach-createpackage-fatal."),
            *Pair.Key, Pair.Value));
    }

    return true;
}

// ============================================================================
// Scope B: the confirmed nested-value files do not grow new unguarded loads
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNestedValueLoadsAreGuardedTest,
    "PinWright.infra.contract.LoadGuard.NestedValueFilesDoNotRegrow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNestedValueLoadsAreGuardedTest::RunTest(const FString& Parameters)
{
    using namespace IrLoadGuardHelpers;

    const FString SourceRoot = ResolveSourceRoot();
    if (!TestFalse(TEXT("Resolved the plugin Source root"), SourceRoot.IsEmpty()))
    {
        return false;
    }

    const TMap<FString, int32> Baseline = ParseBaseline(*this);
    if (!TestTrue(TEXT("The baseline parsed at least one entry"), Baseline.Num() > 0))
    {
        return false;
    }

    FScan Scan;
    for (const TPair<FString, int32>& Entry : Baseline)
    {
        const FString File = SourceRoot / Entry.Key;
        // A baselined file that no longer exists is a stale entry, and a stale entry is a hole:
        // the ratchet would keep passing while guarding nothing.
        if (!TestTrue(FString::Printf(TEXT("Baselined file '%s' exists on disk"), *Entry.Key),
                      IFileManager::Get().FileExists(*File)))
        {
            continue;
        }
        ScanFile(File, SourceRoot, Scan);
    }

    if (!TestTrue(TEXT("Read at least one baselined file"), Scan.FilesRead > 0))
    {
        return false;
    }
    if (!PatternsAreFunctional(*this))
    {
        return false;
    }
    // Every one of these files was converted, so at least one guarded call must be visible. Zero
    // means the conversion was reverted and the "unguarded" counts below are meaningless.
    if (!TestTrue(TEXT("The nested-value files still call the guard"), Scan.GuardedCallsSeen > 0))
    {
        return false;
    }

    for (const TPair<FString, int32>& Entry : Baseline)
    {
        const int32 Found = Scan.UnguardedByFile.FindRef(Entry.Key);
        if (Found > Entry.Value)
        {
            AddError(FString::Printf(
                TEXT("%s now performs %d unguarded load(s), up from the baselined %d. A load added "
                     "here must read its path through PinWrightGuardedLoad::LoadObjectChecked "
                     "(Utils/GuardedLoad.h) unless the path is a top-level parameter declared "
                     "`path` or `classref`. Board "
                     "B-nested-path-values-reach-createpackage-fatal."),
                *Entry.Key, Found, Entry.Value));
        }
        else if (Found < Entry.Value)
        {
            AddWarning(FString::Printf(
                TEXT("%s is down to %d unguarded load(s) from the baselined %d. Lower the entry in "
                     "NestedFileBaseline - a stale ceiling is a hole, not merely noise."),
                *Entry.Key, Found, Entry.Value));
        }
    }

    return true;
}
