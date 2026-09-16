// Copyright (c) 2026 Alexander Penkin. MIT License.

// THE RATCHET ON THE PATH-SHAPED DECLARED TYPES. Every path-shaped parameter in the handler trees
// must declare `path`, `classref` or `filepath` - not `string` - so the dispatcher's path-separator
// pass (Dispatch/RpcDispatcher.cpp, reading Handlers/ParamTypeCheck.h) actually sees it.
//
// WHY A TEST AND NOT A CONVENTION. A doubled slash in an asset path reaches CreatePackage, which
// logs at Fatal: it ends the PROCESS and every unsaved package in it, and no `if (!Result)` after
// the call is ever reached. CreatePackage is not the only door - StaticLoadObjectInternal calls
// ResolveName2(..., Create=true), which calls CreatePackage on the partial name - so ANY load on
// unvalidated caller text is the same editor kill, at ~385 sites. The plugin closed that class at
// the dispatch boundary rather than at the sites, which works only for as long as the declarations
// stay honest. A new verb spelling RPC_PARAM_REQ("assetPath", "string", ...) silently opts itself
// back out, and nothing else in the suite can see it: `string` and `path` produce byte-identical
// behaviour on every payload that does not contain '//', so no response-shape test can discriminate
// them. This is the only place the difference is observable.
//
// LISTED = WARN, UNLISTED = HARD ERROR, STALE = WARN. Modelled on
// Tests/Core/TestErrorCodeRegistry.cpp's baseline idiom, and for the same reason it is used there:
// the migration is being performed by several agents at once, and a test that failed on every
// not-yet-converted declaration would redden the suite for the whole conversion instead of guarding
// its end state. The baseline below is a MEASUREMENT taken when this test landed, not a design - it
// is expected to shrink to nothing, and each surviving entry warns until it does.
//
// THE BASELINE IS COUNTED, not merely named. An entry is "<source-relative file>|<param>|<count>".
// Naming the pair alone would let a NEW mis-typed `assetPath` land in a file that already has one,
// which is the single most likely way this regrows: the ratchet has to be able to say "this file
// had four and now has five". A count that DROPPED is the expected direction and warns.
//
// WHAT COUNTS AS PATH-SHAPED IS THE NAME SUFFIX, and nothing cleverer. A declaration is flagged when
// its wire name ends in `path` or `paths`, case-insensitively. Two deliberate carve-outs:
//   * a declaration whose every declared atom is number/integer/boolean/bool is skipped - it can
//     never carry a string, so it can never carry a path to a loader. `recursivePaths`,
//     `useAccelerationForPaths` and `includeNodeTypePath` are booleans whose `path` suffix is
//     incidental, and flagging them would mean asking for a meaningless retype;
//   * `propertyPath` is exempt by name: it is a REFLECTION property path ("Struct.Member"), resolved
//     through FProperty lookup and never through a package load. Typing it `path` would be a lie
//     about what it is, and would refuse nothing useful.
//
// CLASS REFERENCES ARE DELIBERATELY NOT LINTED. `className`, `parentClass`, `actorClass` and the
// bare `class` share no mechanical name shape, so a name rule over them would both miss most of them
// and misfire on the rest. `classref` is adopted by reading the verb; this test does not pretend to
// drive it. It does accept `classref` wherever a path-suffixed name declares it (`classPath`,
// `parentClassPath`, `rendererClassPath`), because that is the correct type for those.
//
// WHAT IT CANNOT CATCH, stated rather than implied: it knows NAMES, not semantics. A disk path that
// a sweep wrongly retyped from `string` to `path` starts refusing UNC input and this test is happy,
// because `filePath` typed `path` satisfies it exactly as `filepath` would. Choosing between the
// three is a per-parameter reading of the verb, and no name-based lint can do it.
//
// UNABLE TO FAIL IF - each asserted, because every one of them turns this into a test that passes by
// checking nothing:
//   * the plugin does not resolve through IPluginManager, or the main handler tree is not on disk;
//   * fewer than two handler roots are discovered (the integration sub-modules stopped being found);
//   * the recursive walk returns no source files;
//   * ANY ONE of the three declaration patterns matches nothing anywhere in the tree - checked per
//     pattern, not on the union, because a union stays non-empty as long as one regex still works,
//     which is precisely how a broken regex hides. It also catches an ICU-less build, where
//     FRegexMatcher::FindNext never returns true and every regex here silently matches nothing;
//   * the walk finds no path-shaped declaration at all;
//   * the baseline swallows every flagged declaration, so nothing is actually held to zero.
//
// THE SCAN READS CODE, NOT PROSE. Its input goes through NeutralizeSourceText (Tests/TestUtils.h)
// first. Handler files quote their own parameter shapes in comments, and a quoted
// RPC_PARAM_REQ("assetPath", "string", ...) scored as a real declaration would fail a file that
// contains no such declaration - that is board B-error-code-adoption-test-scans-comments, where one
// explanatory sentence flipped a file's verdict for all 40 codes it spelled by hand.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

#include "Tests/TestUtils.h"

namespace
{
    // ------------------------------------------------------------------------
    // Roots and paths. Same discovery as Tests/Core/TestErrorCodeRegistry.cpp: every
    // Source/PinWright*/Private/Handlers tree, so the gated integration sub-modules are covered even
    // on a host whose engine plugins are disabled and a registry walk could not see their verbs at
    // all. That is the reason this is a SOURCE scan rather than a walk of
    // FAutoRegisterHandler::GetPendingRegistrations().
    // ------------------------------------------------------------------------

    FString PathLintResolveModuleSourceRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
    }

    FString PathLintResolveMainHandlersDir()
    {
        const FString SourceDir = PathLintResolveModuleSourceRoot();
        return SourceDir.IsEmpty()
            ? FString()
            : (SourceDir / TEXT("PinWright") / TEXT("Private") / TEXT("Handlers"));
    }

    TArray<FString> PathLintResolveAllHandlerDirs()
    {
        TArray<FString> Roots;
        const FString SourceDir = PathLintResolveModuleSourceRoot();
        if (SourceDir.IsEmpty())
        {
            return Roots;
        }

        TArray<FString> ModuleDirNames;
        IFileManager::Get().FindFiles(ModuleDirNames,
            *(SourceDir / TEXT("PinWright*")), /*Files=*/false, /*Directories=*/true);
        ModuleDirNames.Sort();

        for (const FString& ModuleDirName : ModuleDirNames)
        {
            const FString HandlersDir =
                SourceDir / ModuleDirName / TEXT("Private") / TEXT("Handlers");
            if (IFileManager::Get().DirectoryExists(*HandlersDir))
            {
                Roots.Add(HandlersDir);
            }
        }
        return Roots;
    }

    // Path relative to <Plugin>/Source, forward-slashed, so it is stable to quote in a failure
    // message and stable to write into the baseline. Both sides are converted to full paths first:
    // IPlugin::GetBaseDir can hand back an engine-relative path while FindFilesRecursive echoes
    // whatever root it was given, and leaving the two in different forms makes MakePathRelativeTo
    // return its input unchanged - which would miss every baseline entry rather than erroring.
    FString PathLintMakeRelative(const FString& AnyPath, const FString& SourceDir)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(AnyPath);
        const FString FullSourceDir = FPaths::ConvertRelativePathToFull(SourceDir) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullSourceDir);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    // ------------------------------------------------------------------------
    // The three declaration patterns. Between them they cover every way an FParamSpec is built in
    // the handler trees, which is the point: matching only the RPC_PARAM_* macros would leave the
    // hand-written aggregates and the shared param-builder helpers structurally invisible, and a
    // path parameter declared through a helper is exactly as lethal as one declared through a macro.
    // ------------------------------------------------------------------------

    // 1. RPC_PARAM_REQ / OPT / DEF / *_NESTED ("name", "type", ...)
    const TCHAR* const PathLintPatternMacro =
        TEXT("RPC_PARAM_(?:REQ|OPT|DEF)(?:_NESTED)?\\s*\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*,\\s*\"([A-Za-z|][A-Za-z| ]*)\"");

    // 2. FParamSpec{TEXT("name"), TEXT("type"), ...} - the hand-written aggregate, used wherever a
    //    spec needs aliases or a nested schema the macros do not spell.
    const TCHAR* const PathLintPatternAggregate =
        TEXT("FParamSpec\\s*\\{\\s*TEXT\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*\\)\\s*,\\s*TEXT\\(\\s*\"([A-Za-z|][A-Za-z| ]*)\"\\s*\\)");

    // 3. AnyFactory(TEXT("name"), TEXT("type"), ...) - the shared param-builder helpers
    //    (ParamAliasUtils::MakeAliasParamSpec, MaterialHandlerUtils::MaterialAssetPathParamReq, ...).
    //    Anchored on the SECOND argument being a well-formed type expression, which is what keeps it
    //    from matching arbitrary two-string calls.
    const TCHAR* const PathLintPatternFactory =
        TEXT("[A-Za-z_][A-Za-z0-9_:]*\\s*\\(\\s*TEXT\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*\\)\\s*,\\s*TEXT\\(\\s*\"([A-Za-z|][A-Za-z| ]*)\"\\s*\\)");

    // Mirrors PinWrightIsKnownTypeAtom (Handlers/ParamTypeCheck.h).
    bool PathLintIsKnownAtom(const FString& Atom)
    {
        return Atom == TEXT("string") || Atom == TEXT("number") || Atom == TEXT("integer")
            || Atom == TEXT("boolean") || Atom == TEXT("bool") || Atom == TEXT("object")
            || Atom == TEXT("array") || Atom == TEXT("any")
            || Atom == TEXT("path") || Atom == TEXT("classref") || Atom == TEXT("filepath");
    }

    TArray<FString> PathLintAtomsOf(const FString& TypeExpr)
    {
        TArray<FString> Atoms;
        TypeExpr.ParseIntoArray(Atoms, TEXT("|"), true);
        for (FString& Atom : Atoms)
        {
            Atom = Atom.TrimStartAndEnd().ToLower();
        }
        return Atoms;
    }

    // A second argument that is a well-formed type expression is what makes a regex hit a parameter
    // declaration rather than an arbitrary pair of string literals.
    bool PathLintIsTypeExpr(const FString& TypeExpr)
    {
        const TArray<FString> Atoms = PathLintAtomsOf(TypeExpr);
        if (Atoms.Num() == 0)
        {
            return false;
        }
        for (const FString& Atom : Atoms)
        {
            if (!PathLintIsKnownAtom(Atom))
            {
                return false;
            }
        }
        return true;
    }

    bool PathLintIsPathShapedName(const FString& Name)
    {
        const FString Lower = Name.ToLower();
        return Lower.EndsWith(TEXT("path")) || Lower.EndsWith(TEXT("paths"));
    }

    // Already declared as one of the three path-shaped types. A union counts: `path|array` is how an
    // array-of-paths slot is spelled, and the dispatcher's rule reaches its string elements.
    bool PathLintDeclaresAPathType(const FString& TypeExpr)
    {
        for (const FString& Atom : PathLintAtomsOf(TypeExpr))
        {
            if (Atom == TEXT("path") || Atom == TEXT("classref") || Atom == TEXT("filepath"))
            {
                return true;
            }
        }
        return false;
    }

    // A slot no string can reach cannot carry a path to a loader, whatever it is named.
    bool PathLintCannotCarryAString(const FString& TypeExpr)
    {
        const TArray<FString> Atoms = PathLintAtomsOf(TypeExpr);
        if (Atoms.Num() == 0)
        {
            return false;
        }
        for (const FString& Atom : Atoms)
        {
            if (Atom != TEXT("number") && Atom != TEXT("integer")
                && Atom != TEXT("boolean") && Atom != TEXT("bool"))
            {
                return false;
            }
        }
        return true;
    }

    // Names whose `path` suffix is incidental. This list is for names that are NOT paths; it is not
    // a place to park a path somebody did not want to convert - that is what the baseline is for,
    // and the baseline warns where this list is silent.
    const TCHAR* const PathLintExemptNames[] = {
        // A reflection property path ("Struct.Member"), resolved through FProperty lookup and never
        // through a package load.
        TEXT("propertyPath"),
    };

    bool PathLintIsExemptName(const FString& Name)
    {
        for (const TCHAR* const Exempt : PathLintExemptNames)
        {
            if (Name.Equals(Exempt, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    struct FPathLintScan
    {
        int32 FilesRead = 0;
        int32 MacroMatches = 0;
        int32 AggregateMatches = 0;
        int32 FactoryMatches = 0;
        int32 PathShapedDeclarations = 0;
        // "<source-relative file>|<param name>" -> how many of its declarations are still NOT
        // declared with a path-shaped type.
        TMap<FString, int32> FlaggedCounts;
    };

    void PathLintApplyPattern(const FString& Contents, const FString& Relative,
        const TCHAR* Pattern, int32& OutMatchCount, FPathLintScan& Scan, TSet<int32>& SeenStarts)
    {
        const FRegexPattern Compiled(Pattern);
        FRegexMatcher Matcher(Compiled, Contents);
        while (Matcher.FindNext())
        {
            OutMatchCount++;

            const FString Name = Matcher.GetCaptureGroup(1);
            const FString TypeExpr = Matcher.GetCaptureGroup(2);
            if (!PathLintIsTypeExpr(TypeExpr))
            {
                continue;
            }

            // Two patterns can match the same span; count a declaration once, keyed on where its
            // NAME literal begins.
            const int32 NameStart = Matcher.GetCaptureGroupBeginning(1);
            if (SeenStarts.Contains(NameStart))
            {
                continue;
            }
            SeenStarts.Add(NameStart);

            if (!PathLintIsPathShapedName(Name) || PathLintIsExemptName(Name))
            {
                continue;
            }
            Scan.PathShapedDeclarations++;

            if (PathLintDeclaresAPathType(TypeExpr) || PathLintCannotCarryAString(TypeExpr))
            {
                continue;
            }

            Scan.FlaggedCounts.FindOrAdd(FString::Printf(TEXT("%s|%s"), *Relative, *Name))++;
        }
    }

    FPathLintScan PathLintScanFiles(const TArray<FString>& SourceFiles, const FString& SourceDir)
    {
        FPathLintScan Scan;
        for (const FString& File : SourceFiles)
        {
            FString RawContents;
            if (!FFileHelper::LoadFileToString(RawContents, *File))
            {
                continue;
            }
            Scan.FilesRead++;

            // Comments and raw-string bodies blanked, ordinary string literals kept - the parameter
            // names and type names live in those. See the file header.
            const FString Contents = NeutralizeSourceText(RawContents);
            const FString Relative = PathLintMakeRelative(File, SourceDir);

            TSet<int32> SeenStarts;
            PathLintApplyPattern(Contents, Relative, PathLintPatternMacro,
                Scan.MacroMatches, Scan, SeenStarts);
            PathLintApplyPattern(Contents, Relative, PathLintPatternAggregate,
                Scan.AggregateMatches, Scan, SeenStarts);
            PathLintApplyPattern(Contents, Relative, PathLintPatternFactory,
                Scan.FactoryMatches, Scan, SeenStarts);
        }
        return Scan;
    }

    // ------------------------------------------------------------------------
    // THE BASELINE. "<source-relative file>|<param name>|<declarations still mis-typed>". Every
    // entry WARNS; none of them fails. A declaration NOT listed here is a hard ERROR, which is the
    // whole ratchet: a new verb, or a new mis-typed declaration in a file that already had some,
    // cannot land quietly.
    //
    // MEASURED TWICE, and the second measurement is what is written here. The first seeding, taken
    // while the conversion wave was still running, held 116 file/param pairs over 225 declarations.
    // Re-measured once every cluster had finished: 17 pairs over 26 declarations. The 99 pairs that
    // dropped out were converted in between and are deliberately NOT left behind as warnings - a
    // stale entry is not merely noise, it is a hole. `NiagaraEditHandler.cpp|assetPath` baselined at
    // 20 would have let twenty new mis-typed declarations land in that file before the count
    // exceeded its ceiling and anything failed.
    //
    // Delete an entry as its file is converted, and lower the count when only some of a file's
    // declarations are. Every remaining entry below is a real judgement call somebody deferred, not
    // an oversight: most are disk paths or array-of-paths slots where `filepath` versus `path`, or
    // widening an `array` to `path|array`, needs the verb read rather than the name matched.
    // ------------------------------------------------------------------------
    const TCHAR* const PathLintUnconvertedBaseline[] = {
        TEXT("PinWright/Private/Handlers/AI/StateTreeAuthoringHandler.cpp|sourcePath|1"),
        TEXT("PinWright/Private/Handlers/AI/StateTreeAuthoringHandler.cpp|targetPath|1"),
        TEXT("PinWright/Private/Handlers/Actor/ActorFolderHandler.cpp|folderPath|1"),
        TEXT("PinWright/Private/Handlers/Actor/SpawnMaterialUtils.h|materialPaths|1"),
        TEXT("PinWright/Private/Handlers/Asset/AssetQueryHandler.cpp|packagePaths|1"),
        TEXT("PinWright/Private/Handlers/Asset/AssetWorkflowHandler.cpp|assetPaths|5"),
        TEXT("PinWright/Private/Handlers/Audio/AudioHandler.cpp|soundPath|1"),
        TEXT("PinWright/Private/Handlers/ControlRig/CRIRDecompileHandler.cpp|assetPath|1"),
        TEXT("PinWright/Private/Handlers/Debug/PerformanceHandler.cpp|outputPath|1"),
        TEXT("PinWright/Private/Handlers/Debug/TraceAnalysisHandler.cpp|tracePath|1"),
        TEXT("PinWright/Private/Handlers/Environment/LandscapeHandler.cpp|path|1"),
        TEXT("PinWright/Private/Handlers/Geometry/SplineHandler.cpp|materialPath|1"),
        TEXT("PinWright/Private/Handlers/Geometry/SplineHandler.cpp|meshPath|3"),
        TEXT("PinWright/Private/Handlers/Skeleton/SkeletonCompileHandler.cpp|filePath|2"),
        TEXT("PinWright/Private/Handlers/Skeleton/SkeletonCompileHandler.cpp|outputPath|1"),
        TEXT("PinWright/Private/Handlers/SourceControl/SourceControlHandler.cpp|assetPaths|3"),
        TEXT("PinWright/Private/Handlers/VFX/EffectHandler.cpp|systemPath|1"),
    };

    // "file|param" -> baselined count.
    TMap<FString, int32> PathLintParseBaseline(FAutomationTestBase& Test)
    {
        TMap<FString, int32> Baseline;
        for (const TCHAR* const Entry : PathLintUnconvertedBaseline)
        {
            const FString Line(Entry);
            FString Head;
            FString CountText;
            if (!Line.Split(TEXT("|"), &Head, &CountText, ESearchCase::CaseSensitive,
                            ESearchDir::FromEnd)
                || Head.IsEmpty() || !CountText.IsNumeric())
            {
                Test.AddError(FString::Printf(
                    TEXT("Malformed baseline entry '%s' (expected file|param|count)."), *Line));
                continue;
            }
            Baseline.Add(Head, FCString::Atoi(*CountText));
        }
        return Baseline;
    }
}

// ============================================================================
// Every path-shaped parameter declares path / classref / filepath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathParamTypeLintTest,
    "PinWright.infra.contract.PathParamTypes.PathShapedParamsDeclareAPathType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPathParamTypeLintTest::RunTest(const FString& Parameters)
{
    const FString MainHandlersDir = PathLintResolveMainHandlersDir();
    if (!TestFalse(TEXT("Resolved the main handler source dir"), MainHandlersDir.IsEmpty()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Main handler source dir exists on disk"),
            IFileManager::Get().DirectoryExists(*MainHandlersDir)))
    {
        return false;
    }

    const TArray<FString> HandlerRoots = PathLintResolveAllHandlerDirs();
    if (!TestTrue(TEXT("Main module handler tree is among the discovered roots"),
            HandlerRoots.Contains(MainHandlersDir)))
    {
        return false;
    }
    // Guards the discovery itself: if the sub-module trees stop being found, this contract silently
    // stops covering every sub-module path parameter.
    if (!TestTrue(TEXT("Discovered integration sub-module handler trees as well"),
            HandlerRoots.Num() > 1))
    {
        return false;
    }

    const FString SourceDir = PathLintResolveModuleSourceRoot();
    if (!TestFalse(TEXT("Resolved the plugin Source root"), SourceDir.IsEmpty()))
    {
        return false;
    }

    TArray<FString> SourceFiles;
    for (const FString& Root : HandlerRoots)
    {
        IFileManager::Get().FindFilesRecursive(SourceFiles, *Root, TEXT("*.cpp"), true, false, false);
        IFileManager::Get().FindFilesRecursive(SourceFiles, *Root, TEXT("*.h"), true, false, false);
    }
    if (!TestTrue(TEXT("Recursive walk found handler source files to scan"), SourceFiles.Num() > 0))
    {
        return false;
    }

    const FPathLintScan Scan = PathLintScanFiles(SourceFiles, SourceDir);

    if (!TestTrue(TEXT("Read at least one handler source file"), Scan.FilesRead > 0))
    {
        return false;
    }

    // The emptiness guard, PER PATTERN. Checking only the union would let a broken regex hide behind
    // a working one - the union stays non-empty and the contract silently narrows.
    bool bPatternsAlive = true;
    bPatternsAlive &= TestTrue(
        TEXT("Pattern 1/3 RPC_PARAM_*(\"name\", \"type\") still matches somewhere in the handler tree"),
        Scan.MacroMatches > 0);
    bPatternsAlive &= TestTrue(
        TEXT("Pattern 2/3 FParamSpec{TEXT(\"name\"), TEXT(\"type\")} still matches somewhere in the "
             "handler tree"),
        Scan.AggregateMatches > 0);
    bPatternsAlive &= TestTrue(
        TEXT("Pattern 3/3 Factory(TEXT(\"name\"), TEXT(\"type\")) still matches somewhere in the "
             "handler tree"),
        Scan.FactoryMatches > 0);
    if (!bPatternsAlive)
    {
        AddError(TEXT("A declaration pattern matched nothing at all. Either the regex is broken, the "
                      "tree moved, or this build has no ICU regex engine - and in every one of those "
                      "cases the declarations it was responsible for are now unchecked. This is a "
                      "failure and not a pass on purpose: an empty result set is the failure mode "
                      "this test exists to make loud."));
        return false;
    }

    // Non-vacuity on the thing actually being linted, not just on the scan. The registry carried
    // ~780 path-shaped declarations when this landed; a floor of 200 fails loudly if the name rule,
    // the type-expression filter or the walk ever stops seeing them, while leaving room for the
    // whole population to be renamed over time.
    if (!TestTrue(FString::Printf(
            TEXT("Found path-shaped parameter declarations to check (found %d, expected >= 200)"),
            Scan.PathShapedDeclarations), Scan.PathShapedDeclarations >= 200))
    {
        return false;
    }

    const TMap<FString, int32> Baseline = PathLintParseBaseline(*this);

    TArray<FString> Unlisted;
    TArray<FString> Regrown;
    TArray<FString> StillWaived;
    int32 BaselineMatched = 0;

    for (const TPair<FString, int32>& Flagged : Scan.FlaggedCounts)
    {
        const int32* Allowed = Baseline.Find(Flagged.Key);
        if (!Allowed)
        {
            Unlisted.Add(FString::Printf(TEXT("%s (%d)"), *Flagged.Key, Flagged.Value));
            continue;
        }

        BaselineMatched++;
        if (Flagged.Value > *Allowed)
        {
            Regrown.Add(FString::Printf(TEXT("%s (baseline %d, now %d)"),
                *Flagged.Key, *Allowed, Flagged.Value));
        }
        else
        {
            StillWaived.Add(FString::Printf(TEXT("%s (%d)"), *Flagged.Key, Flagged.Value));
        }
    }
    Unlisted.Sort();
    Regrown.Sort();
    StillWaived.Sort();

    // UNABLE TO FAIL IF the baseline swallows everything: with nothing flagged and nothing matched,
    // the two error loops below are vacuous and this test reports green over a scan that found no
    // declarations to hold to anything.
    if (Baseline.Num() > 0 && Scan.FlaggedCounts.Num() > 0 && BaselineMatched == 0)
    {
        AddError(TEXT("Not one baseline entry matched a flagged declaration, yet both sets are "
                      "non-empty. The relative-path spelling has drifted, so every baseline entry is "
                      "dead and every real declaration would read as unlisted."));
    }

    for (const FString& Entry : Unlisted)
    {
        AddError(FString::Printf(
            TEXT("%s is a path-shaped parameter still declared with a non-path type. Declare it "
                 "`path` (an asset/package/object path), `classref` (a class reference) or "
                 "`filepath` (a path on DISK - it carries no '//' rule, because a UNC path "
                 "normalises to //server/share). Until it does, the dispatcher's path-separator "
                 "pass does not see it and a '//' value reaches CreatePackage, which logs Fatal and "
                 "ends the editor process."),
            *Entry));
    }

    for (const FString& Entry : Regrown)
    {
        AddError(FString::Printf(
            TEXT("%s - this file/parameter pair grew NEW mis-typed declarations past its baseline. "
                 "The baseline is a ceiling that only moves down; declare the new one `path`, "
                 "`classref` or `filepath`."),
            *Entry));
    }

    if (StillWaived.Num() > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d file/parameter pair(s) still declare a path-shaped parameter with a non-path "
                 "type. These are baselined in PathLintUnconvertedBaseline and warn rather than "
                 "fail; convert them and delete the entries: %s"),
            StillWaived.Num(), *FString::Join(StillWaived, TEXT(", "))));
    }

    // The baseline must not rot. An entry whose file/parameter is now fully converted, renamed or
    // gone is dead weight that would silently exempt the pair if it ever came back.
    TArray<FString> StaleBaseline;
    for (const TPair<FString, int32>& Entry : Baseline)
    {
        const int32* Observed = Scan.FlaggedCounts.Find(Entry.Key);
        if (!Observed)
        {
            StaleBaseline.Add(FString::Printf(TEXT("%s (baseline %d, now none)"),
                *Entry.Key, Entry.Value));
        }
        else if (*Observed < Entry.Value)
        {
            StaleBaseline.Add(FString::Printf(TEXT("%s (baseline %d, now %d)"),
                *Entry.Key, Entry.Value, *Observed));
        }
    }
    if (StaleBaseline.Num() > 0)
    {
        StaleBaseline.Sort();
        AddWarning(FString::Printf(
            TEXT("%d baseline entr(y/ies) are stale - the pair is converted, shrunk, renamed or "
                 "gone. Delete or lower them in PathLintUnconvertedBaseline: %s"),
            StaleBaseline.Num(), *FString::Join(StaleBaseline, TEXT(", "))));
    }

    return TestEqual(
        TEXT("No path-shaped parameter declares a non-path type outside the baseline"),
        Unlisted.Num() + Regrown.Num(), 0);
}
