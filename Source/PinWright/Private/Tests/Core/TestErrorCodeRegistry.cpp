// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests enforcing the error-code registry (board ticket
// E-error-code-vocabulary-registry). Four tests live here:
//
//   1. AllEmittedCodesAreRegistered  - every code that reaches the wire has an ERR_<CODE>
//      constant in Handlers/ErrorCodes.h.
//   2. RegistryAdoptingFilesUseConstantsOnly - a file that has adopted the registry may not also
//      spell a code by hand.
//   3. AdoptionScanIgnoresComments - test 2's per-file decisions are made about CODE, not prose.
//   4. EmissionScanFollowsForwardedCodes - test 1's scan sees a code that reaches SendError
//      through a variable, and does not see one that only appears in a comment.
//
// The first two are SOURCE SCANS, because nothing observable at runtime distinguishes
// SendError(TEXT("SAVE_FAILED"), ...) from SendError(ErrorCodes::ERR_SAVE_FAILED, ...) - the wire
// bytes are identical. That is exactly why these defects survive: they are invisible to every
// response-shape test.
//
// Five things this scan deliberately does NOT narrow:
//   - Roots are discovered, not listed. Every Source/PinWright*/Private/Handlers directory is
//     scanned, so the gated integration sub-modules (Geometry, PCG, Chooser, PoseSearch, CommonUI)
//     are covered and a sixth one is covered the day it lands. Scanning only the main module made
//     every sub-module code structurally invisible to this contract.
//   - Both literal spellings count at a SendError call site. Much of the Sequencer tree passes
//     bare narrow literals (SendError("EDITOR_NOT_OPEN", ...)); matching only the TEXT()-wrapped
//     form let those codes escape the registry.
//   - INDIRECT emission counts. A code assigned to an error-code out-parameter or result field and
//     passed to SendError later as a VARIABLE never appears next to the word SendError at all.
//     Anchoring only on `SendError\s*\(` made that whole class structurally invisible: it is how
//     the raw literals in PinWrightCameraFrame::GetActiveLevelViewport went undetected, and a
//     measurement at the time this pattern was added found 63 codes emitted this way, 37 of them
//     with no registry entry whatsoever.
//   - A code carried inside an error VALUE counts. FNiagaraEditError::Make(TEXT("INVALID_OP"), ...)
//     builds the error in one function, returns it, and SendNiagaraEditError forwards it as
//     Ctx.SendError(Error.Code, Error.Message) somewhere else entirely - so the literal sits next
//     to neither the word SendError nor an assignment, and patterns 1-3 are all blind to it. 34
//     codes reached the wire that way with no registry entry and the suite was green on every one
//     of them (board B-error-code-registry-blind-to-variable-codes).
//   - The emission scan reads CODE, not prose. Its input is neutralized first, for the same reason
//     test 2's is: a commented-out example call (Actor/ActorNameParamUtils.h carries one, spelling
//     ACTOR_NOT_FOUND) is not an emission, and scoring it as one both misattributes the code to a
//     file that does not send it and would fail the walk on a code nothing emits.
// The registry header itself stays singular: sub-module handlers include the main module's
// Handlers/ErrorCodes.h through their PrivateIncludePaths.
//
// UNABLE TO FAIL IF - and every clause below is asserted, so none of them can quietly hold:
//   * the plugin does not resolve through IPluginManager, or the main handler tree is not on disk;
//   * fewer than two handler roots are discovered (the sub-modules stopped being found);
//   * the recursive file walk returns no sources;
//   * ErrorCodes.h parses to zero registered codes;
//   * ANY ONE of the emission patterns that still has live sites matches nothing anywhere in the
//     tree. Pattern 2 remains in the scan but is excluded from this liveness check because fully
//     converting bare SendError literals to registry constants legitimately reduces it to zero.
//     The remaining per-pattern checks still catch an ICU-less build, where FRegexMatcher::FindNext
//     never returns true and every regex here silently matches nothing.
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
    // Roots and paths
    // ------------------------------------------------------------------------

    // The main module's handler tree. It is one of the scanned roots and also
    // the home of the single registry header.
    FString ResolveHandlersSourceDir()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright")
            / TEXT("Private") / TEXT("Handlers");
    }

    FString ResolveModuleSourceRoot()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
    }

    // Every module directory that actually ships handler code: the main module
    // plus each integration sub-module. PinWrightRecorder has no Handlers tree
    // and drops out on the DirectoryExists check.
    TArray<FString> ResolveAllHandlerSourceDirs()
    {
        TArray<FString> Roots;
        const FString SourceDir = ResolveModuleSourceRoot();
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

    FString ResolveErrorCodesHeaderPath()
    {
        const FString HandlersDir = ResolveHandlersSourceDir();
        return HandlersDir.IsEmpty() ? FString() : (HandlersDir / TEXT("ErrorCodes.h"));
    }

    TArray<FString> CollectHandlerSourceFiles(const TArray<FString>& HandlerRoots)
    {
        TArray<FString> SourceFiles;
        for (const FString& Root : HandlerRoots)
        {
            IFileManager::Get().FindFilesRecursive(SourceFiles, *Root, TEXT("*.cpp"), true, false, false);
            IFileManager::Get().FindFilesRecursive(SourceFiles, *Root, TEXT("*.h"), true, false, false);
        }
        return SourceFiles;
    }

    // Path relative to <Plugin>/Source, forward-slashed, so it is stable to quote in a failure
    // message and stable to write into the baseline below. FString comparison and hashing are
    // case-insensitive in UE, so a drive-case difference cannot miss a baseline entry.
    // Both sides are converted to full paths first. Plugin->GetBaseDir() can hand back an
    // engine-relative path while FindFilesRecursive echoes whatever root it was given; leaving the
    // two in different forms makes MakePathRelativeTo return the input unchanged, which would
    // silently miss every baseline entry rather than erroring. The baseline-matched guard in
    // test 2 exists to catch that if it ever happens anyway.
    FString MakeSourceRelativePath(const FString& AnyPath, const FString& SourceDir)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(AnyPath);
        const FString FullSourceDir = FPaths::ConvertRelativePathToFull(SourceDir) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullSourceDir);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    // ------------------------------------------------------------------------
    // The four emission patterns
    // ------------------------------------------------------------------------

    // 1. SendError(TEXT("CODE"), ...)
    const TCHAR* const PatternWrapped =
        TEXT("SendError\\s*\\(\\s*TEXT\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*\\)");
    // 2. SendError("CODE", ...) - the bare narrow-string form. Anchored on the argument separator
    //    because SendError always takes a message after the code, which keeps it from matching a
    //    lone parenthesised string.
    const TCHAR* const PatternBare =
        TEXT("SendError\\s*\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*,");
    // 3. <something>ErrCode / <something>ErrorCode / <something>Code = TEXT("CODE")
    //    The indirect form. The code is stored, carried out of the function, and handed to
    //    SendError as a variable somewhere else entirely, so no SendError-anchored regex can see
    //    it. The literal is restricted to SCREAMING_SNAKE because that is the documented shape of
    //    an error code (CLAUDE.md, Conventions); the identifier suffix is what carries the intent.
    //    Known imprecision, accepted deliberately: a non-error field literally named `...Code`
    //    holding an uppercase string (`Code = TEXT("PNG")`) would be picked up. There is no such
    //    site in the tree today, and the failure direction is loud rather than silent.
    //    NOTE the capture group is 2 here - group 1 is the identifier suffix alternation.
    const TCHAR* const PatternIndirect =
        TEXT("[A-Za-z_][A-Za-z0-9_]*(ErrCode|ErrorCode|Code)\\s*=\\s*TEXT\\(\\s*\"([A-Z][A-Z0-9_]*)\"\\s*\\)");
    // 4. <...Err.../...Fail...>(TEXT("CODE"), ...) - the code handed to a function that BUILDS an
    //    error value, which is then returned and forwarded to SendError elsewhere. Pattern 3
    //    cannot see this shape because nothing is assigned: FNiagaraEditError::Make(TEXT("CODE"),
    //    Message) is a call, and the field it fills is named `Code` on a struct, which the
    //    `<prefix>Code =` regex does not reach either (the character before `Code` is a dot).
    //    154 Niagara sites, plus MakeFailure / MakeError / MakeCreateError / FailReplace / SetError
    //    elsewhere in the tree, all invisible until this pattern existed.
    //    The callee name carrying Err/Fail is the intent marker, exactly as the identifier suffix
    //    is in pattern 3; the trailing separator keeps a one-argument call that happens to take an
    //    uppercase word (MakeErrorGeo(TEXT("NOT_REACHED"))) out of it.
    //    The leading class excludes '.' and '>' ON PURPOSE, so Ctx.SendError / Ctx->SendError -
    //    which contain "Err" and are already covered by patterns 1 and 2 - do not match here as
    //    well. The per-pattern liveness guard in test 1 only means something while the patterns
    //    stay disjoint: were this one satisfied by the tree's 3000-odd SendError sites, it could
    //    no longer report that the factory form had stopped matching. There is no unqualified
    //    SendError(TEXT("..."), ...) call in the handler tree for it to miss.
    //    NOTE the capture group is 2 here - group 1 is the Err/Fail alternation.
    const TCHAR* const PatternFactory =
        TEXT("[^A-Za-z0-9_.>][A-Za-z0-9_:]*(Err|Fail)[A-Za-z0-9_:]*\\s*\\(\\s*TEXT\\(\\s*\"([A-Z][A-Z0-9_]*)\"\\s*\\)\\s*,");

    int32 MatchCodes(const TCHAR* Pattern, const FString& Contents, int32 CaptureGroup,
        TArray<FString>& OutCodes)
    {
        const FRegexPattern Compiled(Pattern);
        FRegexMatcher Matcher(Compiled, Contents);
        int32 Count = 0;
        while (Matcher.FindNext())
        {
            OutCodes.Add(Matcher.GetCaptureGroup(CaptureGroup));
            ++Count;
        }
        return Count;
    }

    // Every raw code literal in one file's text, across all four patterns.
    int32 CollectRawCodesInFile(const FString& Contents, TArray<FString>& OutCodes,
        int32& OutWrapped, int32& OutBare, int32& OutIndirect, int32& OutFactory)
    {
        OutWrapped = MatchCodes(PatternWrapped, Contents, 1, OutCodes);
        OutBare = MatchCodes(PatternBare, Contents, 1, OutCodes);
        OutIndirect = MatchCodes(PatternIndirect, Contents, 2, OutCodes);
        OutFactory = MatchCodes(PatternFactory, Contents, 2, OutCodes);
        return OutWrapped + OutBare + OutIndirect + OutFactory;
    }

    struct FEmissionScan
    {
        // Code -> the first source file (relative) that spells it, so a failure can name a place.
        TMap<FString, FString> CodeToFile;
        int32 FilesRead = 0;
        int32 WrappedMatches = 0;
        int32 BareMatches = 0;
        int32 IndirectMatches = 0;
        int32 FactoryMatches = 0;
    };

    FEmissionScan ScanEmittedCodes(const TArray<FString>& SourceFiles, const FString& HeaderPath,
        const FString& SourceDir)
    {
        FEmissionScan Scan;
        for (const FString& File : SourceFiles)
        {
            if (FPaths::IsSamePath(File, HeaderPath))
            {
                continue;
            }
            FString RawContents;
            if (!FFileHelper::LoadFileToString(RawContents, *File))
            {
                continue;
            }
            ++Scan.FilesRead;

            // Comments and raw-string bodies are blanked first, for the same reason test 2 does
            // it: whether a code is EMITTED is a fact about code, not about prose. Scanning raw
            // bytes scored a commented-out example call as a real emission - Actor/
            // ActorNameParamUtils.h carries one spelling ACTOR_NOT_FOUND - which attributes the
            // code to a file that never sends it and, for a code no live call site emits, fails
            // the walk on an emission that does not exist. Ordinary string literals are copied
            // through untouched by NeutralizeSourceText, which is what the patterns below read.
            const FString Contents = NeutralizeSourceText(RawContents);

            TArray<FString> Codes;
            int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
            CollectRawCodesInFile(Contents, Codes, Wrapped, Bare, Indirect, Factory);
            Scan.WrappedMatches += Wrapped;
            Scan.BareMatches += Bare;
            Scan.IndirectMatches += Indirect;
            Scan.FactoryMatches += Factory;

            const FString Relative = MakeSourceRelativePath(File, SourceDir);
            for (const FString& Code : Codes)
            {
                if (!Scan.CodeToFile.Contains(Code))
                {
                    Scan.CodeToFile.Add(Code, Relative);
                }
            }
        }
        return Scan;
    }

    // Collects the CODE from every ERR_<CODE>[] = TEXT("CODE") declaration in ErrorCodes.h.
    TSet<FString> CollectRegisteredCodes(const FString& HeaderPath)
    {
        TSet<FString> Registered;
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *HeaderPath))
        {
            return Registered;
        }
        const FRegexPattern Pattern(TEXT("ERR_[A-Za-z0-9_]+\\s*\\[\\s*\\]\\s*=\\s*TEXT\\(\\s*\"([A-Za-z_][A-Za-z0-9_]*)\"\\s*\\)"));
        FRegexMatcher Matcher(Pattern, Contents);
        while (Matcher.FindNext())
        {
            Registered.Add(Matcher.GetCaptureGroup(1));
        }
        return Registered;
    }

    // ------------------------------------------------------------------------
    // QUARANTINE: indirectly-emitted codes that have no registry entry today.
    //
    // The original 2026-08-21 measurement, when the indirect pattern was added, found 37 codes
    // reaching the wire with no ERR_* constant behind them. They were NOT approved; they were
    // recorded so that widening the scan could ship without turning a green suite red on a backlog
    // no single change was allowed to fix. Three have since been registered, leaving 34 quarantined
    // codes here; adding further registry entries remains a separate, deliberate decision.
    //
    // The list is a RATCHET, and its direction is the point: a quarantined code warns, while any
    // newly unregistered indirect code not listed here is a hard error. Shrinking the list is
    // always safe; growing it must be a conscious edit here.
    // ------------------------------------------------------------------------
    const TCHAR* const QuarantinedUnregisteredIndirectCodes[] = {
        TEXT("ADD_ROW_FAILED"),
        TEXT("AMBIGUOUS_HANDLE"),
        TEXT("AMBIGUOUS_LIVE_ROOT"),
        TEXT("AMBIGUOUS_TARGET"),
        TEXT("DUMP_IN_PROGRESS"),
        TEXT("EMPTY_EXPRESSIONS"),
        TEXT("EMPTY_TAGS"),
        TEXT("GAME_VIEWPORT_NOT_FOUND"),
        TEXT("HOST_NOT_FOUND"),
        TEXT("INVALID_ENUM_LITERAL"),
        TEXT("INVALID_FOLDER"),
        TEXT("INVALID_PIN_TYPE"),
        TEXT("INVALID_SYNC_GROUP_PAIR"),
        TEXT("INVALID_SYNC_GROUP_ROLE"),
        TEXT("LAYER_HOST_UNAVAILABLE"),
        TEXT("LAYER_TAG_INVALID"),
        TEXT("LIVE_ROOT_NOT_FOUND"),
        TEXT("LIVE_UI_NOT_FOUND"),
        TEXT("MAX_DEPTH_EXCEEDED"),
        TEXT("MISSING_TARGET"),
        TEXT("MIXED_PAYLOAD"),
        TEXT("NAMESPACE_CHANGE_UNSUPPORTED"),
        TEXT("NOT_A_STACK"),
        TEXT("NO_ACTIVE_WINDOW"),
        TEXT("NO_WINDOWS"),
        TEXT("PARAMETER_NAME_COLLISION"),
        TEXT("PATH_TOO_LONG"),
        TEXT("PIE_NOT_RUNNING"),
        TEXT("REGISTRY_LOADING"),
        TEXT("SCS_ERROR"),
        TEXT("STACK_NOT_FOUND"),
        TEXT("SYNC_GROUP_WRITE_FAILED"),
        TEXT("UNKNOWN_OP"),
        TEXT("UNKNOWN_TAG"),
    };

    // ------------------------------------------------------------------------
    // The adoption decision for one handler file, plus the text every other decision test 2 makes
    // about that file reads.
    //
    // It is made about CODE, not prose: NeutralizeSourceText (Tests/TestUtils.h) blanks comment
    // bodies and raw-string bodies first, leaving ordinary string literals alone because the
    // hand-spelled codes live in those. Scanning raw bytes made a COMMENT naming the constant
    // enough to flip a file to "adopting", which then failed it for every code it spells by hand -
    // board B-error-code-adoption-test-scans-comments, reproduced on a handler that deliberately
    // used a raw literal and said so in a comment. The obvious reading of that failure was
    // "convert this file", a 40-site change the file did not need. Test 3 below holds this to it.
    bool SourceAdoptsErrorCodeRegistry(const FString& RawContents, FString& OutScannableContents)
    {
        OutScannableContents = NeutralizeSourceText(RawContents);
        return OutScannableContents.Contains(TEXT("ErrorCodes::ERR_"));
    }

    // ------------------------------------------------------------------------
    // BASELINE: handler files that reference ErrorCodes::ERR_ and STILL spell some codes by hand.
    //
    // Measured 2026-08-21: 22 files. The tree at large is not converted - 3310 wrapped and 104
    // bare raw literals live across 208 handler files - so a blanket "no raw literals anywhere"
    // rule is a migration, not a test. What IS enforceable, and what actually regressed, is
    // backsliding: AnnotatedCaptureHandler.cpp used the registry in 15 places and still carried 8
    // raw literals, and no test noticed for the whole time they sat there.
    //
    // So the rule is per file: once a file cites the registry, it may not also hand-spell. Files
    // below are the grandfathered mid-conversion set. A file NOT listed here that adopts the
    // registry is held to zero, which is what makes the Annotated case fail today.
    //
    // Direction, deliberately asymmetric so a concurrent conversion cannot redden the suite: a
    // listed file that has since been cleaned only WARNS (prune the entry); an unlisted adopting
    // file that hand-spells is a hard ERROR.
    // ------------------------------------------------------------------------
    const TCHAR* const PartiallyConvertedHandlerFiles[] = {
        TEXT("PinWright/Private/Handlers/Actor/ActorFolderHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Actor/ActorLabelHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Actor/ActorNameParamUtils.h"),
        TEXT("PinWright/Private/Handlers/Actor/LifecycleHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Asset/StaticMeshSetCollisionComplexityHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Audio/MetaSound/MetaSoundPatchPresetHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Audio/MetaSound/MetaSoundVariableHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Blueprint/BlueprintEventHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Editor/EditorQuitHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Localization/LocalizationHandler.cpp"),
        TEXT("PinWright/Private/Handlers/Spatial/PlacementHandler.cpp"),
        TEXT("PinWright/Private/Handlers/UI/WidgetDesignerScreenshotHandler.cpp"),
        TEXT("PinWrightGeometry/Private/Handlers/Geometry/BooleanHandler.cpp"),
        TEXT("PinWrightGeometry/Private/Handlers/Geometry/MeshIOHandler.cpp"),
    };

    // Shared preflight for both tests: resolve roots, prove they are real, list the sources.
    // Returns false (with the failure already recorded) if the scan cannot be trusted to run.
    bool PreflightHandlerScan(FAutomationTestBase& Test, TArray<FString>& OutSourceFiles,
        FString& OutHeaderPath, FString& OutSourceDir)
    {
        const FString HandlersDir = ResolveHandlersSourceDir();
        if (!Test.TestFalse(TEXT("Resolved handler source dir"), HandlersDir.IsEmpty()))
        {
            return false;
        }
        if (!Test.TestTrue(TEXT("Handler source dir exists on disk"),
                IFileManager::Get().DirectoryExists(*HandlersDir)))
        {
            return false;
        }

        const TArray<FString> HandlerRoots = ResolveAllHandlerSourceDirs();
        if (!Test.TestTrue(TEXT("Main module handler tree is among the discovered roots"),
                HandlerRoots.Contains(HandlersDir)))
        {
            return false;
        }
        // Guards the discovery itself: if the sub-module trees ever stop being found,
        // this contract silently stops covering every sub-module error code.
        if (!Test.TestTrue(TEXT("Discovered integration sub-module handler trees as well"),
                HandlerRoots.Num() > 1))
        {
            return false;
        }

        OutSourceDir = ResolveModuleSourceRoot();
        if (!Test.TestFalse(TEXT("Resolved the plugin Source root"), OutSourceDir.IsEmpty()))
        {
            return false;
        }
        OutHeaderPath = ResolveErrorCodesHeaderPath();
        OutSourceFiles = CollectHandlerSourceFiles(HandlerRoots);
        // An empty walk must be an error, never a pass: a moved root or a broken recursive find
        // would otherwise leave every assertion below trivially satisfied.
        if (!Test.TestTrue(TEXT("Recursive walk found handler source files to scan"),
                OutSourceFiles.Num() > 0))
        {
            return false;
        }
        return true;
    }
}

// ============================================================================
// 1. Every emitted code - direct OR indirect - is registered in ErrorCodes.h.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorCodeRegistryAllEmittedCodesRegisteredTest,
    "PinWright.core.error_codes.AllEmittedCodesAreRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FErrorCodeRegistryAllEmittedCodesRegisteredTest::RunTest(const FString& Parameters)
{
    TArray<FString> SourceFiles;
    FString HeaderPath;
    FString SourceDir;
    if (!PreflightHandlerScan(*this, SourceFiles, HeaderPath, SourceDir))
    {
        return false;
    }

    const TSet<FString> Registered = CollectRegisteredCodes(HeaderPath);
    if (!TestTrue(TEXT("ErrorCodes.h declares at least one code"), Registered.Num() > 0))
    {
        return false;
    }

    const FEmissionScan Scan = ScanEmittedCodes(SourceFiles, HeaderPath, SourceDir);

    // The emptiness guard for patterns that still have live sites. Pattern 2 remains active in
    // CollectRawCodesInFile but is deliberately absent here: zero bare SendError literals is a
    // valid fully-normalized state, not evidence that its scanner is broken.
    if (!TestTrue(TEXT("Read at least one handler source file"), Scan.FilesRead > 0))
    {
        return false;
    }
    bool bPatternsAlive = true;
    bPatternsAlive &= TestTrue(
        TEXT("Pattern 1/4 SendError(TEXT(\"CODE\")) still matches somewhere in the handler tree"),
        Scan.WrappedMatches > 0);
    bPatternsAlive &= TestTrue(
        TEXT("Pattern 3/4 <...>ErrCode/ErrorCode/Code = TEXT(\"CODE\") still matches somewhere in "
             "the handler tree"),
        Scan.IndirectMatches > 0);
    bPatternsAlive &= TestTrue(
        TEXT("Pattern 4/4 <...Err/Fail...>(TEXT(\"CODE\"), ...) still matches somewhere in the "
             "handler tree"),
        Scan.FactoryMatches > 0);
    if (!bPatternsAlive)
    {
        AddError(TEXT("An emission pattern matched nothing at all. Either the regex is broken, the "
                      "tree moved, or this build has no ICU regex engine - in every one of those "
                      "cases the codes it was responsible for are now unchecked. This is a failure "
                      "and not a pass on purpose: an empty result set is the failure mode this "
                      "test exists to make loud."));
        return false;
    }

    TSet<FString> Quarantined;
    for (const TCHAR* const Code : QuarantinedUnregisteredIndirectCodes)
    {
        Quarantined.Add(FString(Code));
    }

    TArray<FString> Unregistered;
    TArray<FString> QuarantineHits;
    for (const TPair<FString, FString>& Pair : Scan.CodeToFile)
    {
        if (Registered.Contains(Pair.Key))
        {
            continue;
        }
        if (Quarantined.Contains(Pair.Key))
        {
            QuarantineHits.Add(FString::Printf(TEXT("%s (%s)"), *Pair.Key, *Pair.Value));
            continue;
        }
        Unregistered.Add(FString::Printf(TEXT("%s (%s)"), *Pair.Key, *Pair.Value));
    }
    Unregistered.Sort();
    QuarantineHits.Sort();

    for (const FString& Entry : Unregistered)
    {
        AddError(FString::Printf(
            TEXT("Error code %s is emitted but has no ERR_ constant in Handlers/ErrorCodes.h. "
                 "Add it there first. If it is reached through an error-code out-parameter rather "
                 "than a SendError literal, that is not an exemption - it still reaches the wire."),
            *Entry));
    }

    if (QuarantineHits.Num() > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d quarantined unregistered error code(s) still emitted. These are grandfathered "
                 "in QuarantinedUnregisteredIndirectCodes and warn rather than fail; register them "
                 "and delete the entries: %s"),
            QuarantineHits.Num(), *FString::Join(QuarantineHits, TEXT(", "))));
    }

    // The quarantine must not rot. An entry that is now registered, or no longer emitted anywhere,
    // is dead weight that would silently exempt the code if it ever came back.
    TArray<FString> StaleQuarantine;
    for (const TCHAR* const Code : QuarantinedUnregisteredIndirectCodes)
    {
        const FString Entry(Code);
        if (Registered.Contains(Entry) || !Scan.CodeToFile.Contains(Entry))
        {
            StaleQuarantine.Add(Entry);
        }
    }
    if (StaleQuarantine.Num() > 0)
    {
        StaleQuarantine.Sort();
        AddWarning(FString::Printf(
            TEXT("%d quarantine entr(y/ies) are stale - now registered or no longer emitted. "
                 "Delete them from QuarantinedUnregisteredIndirectCodes: %s"),
            StaleQuarantine.Num(), *FString::Join(StaleQuarantine, TEXT(", "))));
    }

    return TestEqual(TEXT("All emitted error codes are registered in ErrorCodes.h"),
        Unregistered.Num(), 0);
}

// ============================================================================
// 2. A file that has adopted the registry may not also spell a code by hand.
//
// This is the direction the registry walk above structurally cannot cover. That walk fails only on
// a code with NO ERR_ constant, so SendError(TEXT("INVALID_ARGUMENT"), ...) - a registered code,
// spelled by hand - passes it cleanly. Eight such literals sat in AnnotatedCaptureHandler.cpp
// while the suite stayed green the entire time, in a file that used ErrorCodes::ERR_ in fifteen
// other places.
//
// UNABLE TO FAIL IF the walk finds no files (asserted in the preflight), if no file in the tree
// cites the registry at all, or if the baseline swallows every adopting file so that nothing is
// actually held to zero. All three are asserted, because each of them turns this into a test that
// passes by having nothing to check.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorCodeRegistryAdoptingFilesUseConstantsOnlyTest,
    "PinWright.core.error_codes.RegistryAdoptingFilesUseConstantsOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FErrorCodeRegistryAdoptingFilesUseConstantsOnlyTest::RunTest(const FString& Parameters)
{
    TArray<FString> SourceFiles;
    FString HeaderPath;
    FString SourceDir;
    if (!PreflightHandlerScan(*this, SourceFiles, HeaderPath, SourceDir))
    {
        return false;
    }

    TSet<FString> Baseline;
    for (const TCHAR* const Path : PartiallyConvertedHandlerFiles)
    {
        Baseline.Add(FString(Path));
    }

    int32 AdoptingFiles = 0;
    int32 EnforcedFiles = 0;
    int32 BaselineMatched = 0;
    TSet<FString> BaselineStillDirty;
    TArray<FString> Offences;

    for (const FString& File : SourceFiles)
    {
        if (FPaths::IsSamePath(File, HeaderPath))
        {
            continue;
        }
        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *File))
        {
            continue;
        }
        // Comments and raw-string bodies are blanked before BOTH decisions below: whether this
        // file adopts the registry, and which codes it spells by hand. Neither is a fact about
        // the file's prose.
        FString Contents;
        if (!SourceAdoptsErrorCodeRegistry(RawContents, Contents))
        {
            // Not yet converted at all. Out of scope by design: holding the un-migrated tree to
            // this rule would be a 3000-site migration, not a regression test.
            continue;
        }
        ++AdoptingFiles;

        const FString Relative = MakeSourceRelativePath(File, SourceDir);
        TArray<FString> RawCodes;
        int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
        const int32 RawCount =
            CollectRawCodesInFile(Contents, RawCodes, Wrapped, Bare, Indirect, Factory);

        if (Baseline.Contains(Relative))
        {
            ++BaselineMatched;
            if (RawCount > 0)
            {
                BaselineStillDirty.Add(Relative);
            }
            continue;
        }

        ++EnforcedFiles;
        if (RawCount > 0)
        {
            TArray<FString> Unique;
            for (const FString& Code : RawCodes)
            {
                Unique.AddUnique(Code);
            }
            Unique.Sort();
            Offences.Add(FString::Printf(TEXT("%s -> %s"), *Relative, *FString::Join(Unique, TEXT(", "))));
        }
    }

    // Emptiness guards. Each one, if it held, would make this test pass without checking anything.
    if (!TestTrue(TEXT("At least one handler file cites ErrorCodes::ERR_"), AdoptingFiles > 0))
    {
        AddError(TEXT("No file in the handler tree references the registry. Either the scan is "
                      "broken or the registry is unused - both mean this contract is checking "
                      "nothing, so it fails rather than passes."));
        return false;
    }
    if (!TestTrue(TEXT("At least one adopting file is actually held to the no-hand-spelling rule"),
            EnforcedFiles > 0))
    {
        AddError(TEXT("Every registry-adopting file is grandfathered in "
                      "PartiallyConvertedHandlerFiles, so the rule constrains nothing. Shrink the "
                      "baseline or the test is decorative."));
        return false;
    }
    // If NOTHING matched the baseline, the relative-path form has drifted from what is written in
    // PartiallyConvertedHandlerFiles - every grandfathered file would then be reported as an
    // offence. Say that plainly instead of emitting 22 misleading errors.
    if (!TestTrue(TEXT("Baseline paths still resolve against the scanned file list"),
            BaselineMatched > 0))
    {
        AddError(TEXT("Not one entry in PartiallyConvertedHandlerFiles matched a scanned file. "
                      "The <Plugin>/Source-relative path form has drifted (or every listed file "
                      "was deleted); fix MakeSourceRelativePath or the baseline before reading "
                      "any offence below as real."));
        return false;
    }

    Offences.Sort();
    for (const FString& Offence : Offences)
    {
        AddError(FString::Printf(
            TEXT("%s uses the ErrorCodes registry AND still spells codes by hand. Replace each "
                 "literal with its ErrorCodes::ERR_<CODE> constant - a registered code spelled by "
                 "hand is still outside the registry as far as grep, IntelliSense and any future "
                 "rename are concerned, and the registry walk cannot see it because the code IS "
                 "registered."),
            *Offence));
    }

    // Baseline hygiene: a listed file that is now clean should leave the list. Warning only, so a
    // conversion landing from another workstream never turns this red.
    TArray<FString> StaleBaseline;
    for (const TCHAR* const Path : PartiallyConvertedHandlerFiles)
    {
        const FString Entry(Path);
        if (!BaselineStillDirty.Contains(Entry))
        {
            StaleBaseline.Add(Entry);
        }
    }
    if (StaleBaseline.Num() > 0)
    {
        StaleBaseline.Sort();
        AddWarning(FString::Printf(
            TEXT("%d baseline entr(y/ies) are stale - the file is clean, gone, or no longer cites "
                 "the registry. Delete them from PartiallyConvertedHandlerFiles so the ratchet "
                 "keeps tightening: %s"),
            StaleBaseline.Num(), *FString::Join(StaleBaseline, TEXT(", "))));
    }

    return TestEqual(TEXT("No registry-adopting handler file spells an error code by hand"),
        Offences.Num(), 0);
}

// ============================================================================
// 3. Test 2 decides about code, not prose.
//
// The regression this pins is board B-error-code-adoption-test-scans-comments: the adoption check
// was a raw Contains() over the file's bytes, so a COMMENT naming the constant - including one
// written specifically to explain why the file deliberately does NOT use it - flipped the file to
// "adopting" and the test then failed it for every code it spells by hand. It was reproduced on a
// real handler, and the obvious reading of the failure ("convert this file to constants") was a
// 40-site change in exactly the wrong direction.
//
// Fixtures rather than files on purpose: the tree currently has no file of this shape (the one
// that did was worked around by rewording the comment), so a disk scan cannot express the case.
// Both directions are asserted - a comment must not count, and real code must still count - so
// "neutralize everything" is not a way to make this pass.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorCodeRegistryAdoptionScanIgnoresCommentsTest,
    "PinWright.core.error_codes.AdoptionScanIgnoresComments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FErrorCodeRegistryAdoptionScanIgnoresCommentsTest::RunTest(const FString& Parameters)
{
    FString Scannable;

    // The exact shape that reproduced the defect: the file's ONLY occurrence of the constant is a
    // comment saying why the raw literal beside it is deliberate.
    const FString LineCommentOnly =
        TEXT("// Raw literal, not ErrorCodes::ERR_INVALID_PARAMS: this file spells all of its\n")
        TEXT("// codes by hand and is not on the registry's grandfathered baseline.\n")
        TEXT("void F() { Ctx.SendError(TEXT(\"INVALID_PARAMS\"), TEXT(\"nope\")); }\n");
    // Guard the fixture itself: if it stopped naming the constant, the assertion below would pass
    // for the wrong reason.
    TestTrue(TEXT("the line-comment fixture really does name the constant"),
        LineCommentOnly.Contains(TEXT("ErrorCodes::ERR_")));
    TestFalse(TEXT("a line comment naming the constant does not make the file adopting"),
        SourceAdoptsErrorCodeRegistry(LineCommentOnly, Scannable));

    const FString BlockCommentOnly =
        TEXT("/* Convert to ErrorCodes::ERR_NOT_FOUND once this file is migrated. */\n")
        TEXT("void F() { Ctx.SendError(TEXT(\"NOT_FOUND\"), TEXT(\"gone\")); }\n");
    TestTrue(TEXT("the block-comment fixture really does name the constant"),
        BlockCommentOnly.Contains(TEXT("ErrorCodes::ERR_")));
    TestFalse(TEXT("a block comment naming the constant does not make the file adopting"),
        SourceAdoptsErrorCodeRegistry(BlockCommentOnly, Scannable));

    // The other direction: a real reference still adopts, so the neutralizer cannot be widened
    // into blanking the code it is supposed to read.
    const FString RealReference =
        TEXT("void F() { Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, TEXT(\"gone\")); }\n");
    TestTrue(TEXT("a code reference to the constant still makes the file adopting"),
        SourceAdoptsErrorCodeRegistry(RealReference, Scannable));

    // The offence side of the same scan reads the same neutralized text: a hand-spelled code
    // QUOTED in a comment is not an offence, while the same literal in code still is.
    {
        FString CommentedText;
        const FString CommentedOffence =
            TEXT("// Historically this sent SendError(TEXT(\"SAVE_FAILED\"), ...).\n")
            TEXT("void F() { Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED, TEXT(\"x\")); }\n");
        TestTrue(TEXT("the quoted-offence fixture adopts the registry in code"),
            SourceAdoptsErrorCodeRegistry(CommentedOffence, CommentedText));
        TArray<FString> Codes;
        int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
        TestEqual(TEXT("a hand-spelled code quoted in a comment is not counted as an offence"),
            CollectRawCodesInFile(CommentedText, Codes, Wrapped, Bare, Indirect, Factory), 0);
    }
    {
        FString CodeText;
        const FString RealOffence =
            TEXT("void F() { Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED, TEXT(\"x\"));\n")
            TEXT("           Ctx.SendError(TEXT(\"SAVE_FAILED\"), TEXT(\"y\")); }\n");
        TestTrue(TEXT("the real-offence fixture adopts the registry in code"),
            SourceAdoptsErrorCodeRegistry(RealOffence, CodeText));
        TArray<FString> Codes;
        int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
        TestEqual(TEXT("a hand-spelled code in real code is still counted as an offence"),
            CollectRawCodesInFile(CodeText, Codes, Wrapped, Bare, Indirect, Factory), 1);
    }

    return true;
}

// ============================================================================
// 4. A code that reaches SendError through a variable is still seen.
//
// The regression this pins is board B-error-code-registry-blind-to-variable-codes. Test 1 matched
// a code literal only at the SendError call site or on a `...Code = TEXT(...)` assignment, so the
// Niagara family matched none of them: FNiagaraEditError::Make(TEXT("CODE"), Message) is built in
// one function, returned, and forwarded by SendNiagaraEditError as
// Ctx.SendError(Error.Code, Error.Message) somewhere else entirely. 34 codes reached the wire that
// way with no ERR_ constant behind them while the suite reported the registry contract green -
// worse than having no contract, because the test was consulted and answered a narrower question
// than the one it is named for.
//
// Fixtures rather than files, like test 3: pinning this to a real handler would make the test a
// hostage of whichever file happens to carry the shape on the day it is read.
//
// Every direction is asserted, so no widening that merely makes this pass would survive. The
// forwarded code must be FOUND; a literal handed to a call that is not building an error must NOT
// be (otherwise "collect every uppercase literal" passes, and the scan starts reporting JSON field
// names as unregistered error codes); and the SendError sites patterns 1 and 2 own must NOT also
// match pattern 4, because test 1's per-pattern liveness guard stops meaning anything the moment
// the patterns overlap.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorCodeRegistryEmissionScanFollowsForwardedCodesTest,
    "PinWright.core.error_codes.EmissionScanFollowsForwardedCodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FErrorCodeRegistryEmissionScanFollowsForwardedCodesTest::RunTest(const FString& Parameters)
{
    // The exact shape that shipped 34 unregistered codes: the literal appears beside neither the
    // word SendError nor an assignment to anything named `...Code`.
    {
        const FString ForwardedThroughAValue =
            TEXT("FNiagaraEditError Validate(const UEdGraphNode* Node)\n")
            TEXT("{\n")
            TEXT("    if (!Node)\n")
            TEXT("    {\n")
            TEXT("        return FNiagaraEditError::Make(TEXT(\"UNSUPPORTED_NODE_CLASS\"),\n")
            TEXT("            TEXT(\"that node class is not supported here\"));\n")
            TEXT("    }\n")
            TEXT("    return FNiagaraEditError();\n")
            TEXT("}\n")
            TEXT("bool Handler(FHandlerContext& Ctx, const UEdGraphNode* Node)\n")
            TEXT("{\n")
            TEXT("    const FNiagaraEditError Error = Validate(Node);\n")
            TEXT("    if (Error.HasError()) { Ctx.SendError(Error.Code, Error.Message); }\n")
            TEXT("    return true;\n")
            TEXT("}\n");
        TArray<FString> Codes;
        int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
        CollectRawCodesInFile(ForwardedThroughAValue, Codes, Wrapped, Bare, Indirect, Factory);
        // Guards the fixture itself: if one of the three original patterns could see this shape,
        // the assertion below would pass for a reason that has nothing to do with the fix.
        TestEqual(TEXT("the SendError-anchored patterns really are blind to the forwarded code"),
            Wrapped + Bare + Indirect, 0);
        TestEqual(TEXT("the error-value factory pattern matches the forwarded code exactly once"),
            Factory, 1);
        TestTrue(TEXT("a code forwarded to SendError through a variable is collected"),
            Codes.Contains(TEXT("UNSUPPORTED_NODE_CLASS")));
    }

    // Both negatives in one fixture. SetStringField carries an uppercase literal but builds no
    // error, and Ctx.SendError carries one that patterns 1 and 2 already own.
    {
        const FString NotAnErrorFactory =
            TEXT("void Build(FHandlerContext& Ctx, const TSharedRef<FJsonObject>& Out)\n")
            TEXT("{\n")
            TEXT("    Out->SetStringField(TEXT(\"FORMAT\"), TEXT(\"png\"));\n")
            TEXT("    Ctx.SendError(TEXT(\"SAVE_FAILED\"), TEXT(\"nope\"));\n")
            TEXT("}\n");
        TArray<FString> Codes;
        int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
        const int32 Total =
            CollectRawCodesInFile(NotAnErrorFactory, Codes, Wrapped, Bare, Indirect, Factory);
        TestEqual(TEXT("the SendError literal is still counted, once"), Wrapped, 1);
        TestEqual(TEXT("neither a non-error call nor a SendError site matches the factory pattern"),
            Factory, 0);
        TestEqual(TEXT("an uppercase literal handed to a non-error call is not an emitted code"),
            Total, 1);
    }

    // The second half of the same defect: the emission scan reads neutralized text, so a
    // commented-out example call is not scored as an emission. Actor/ActorNameParamUtils.h carries
    // exactly this line, and it was being counted as a live ACTOR_NOT_FOUND emission.
    {
        const FString CommentedOutEmission =
            TEXT("// if (!Found) { Ctx.SendError(TEXT(\"ACTOR_NOT_FOUND\"), Message); return true; }\n")
            TEXT("void F(FHandlerContext& Ctx) { Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND, Message); }\n");
        TArray<FString> RawCodes;
        int32 Wrapped = 0, Bare = 0, Indirect = 0, Factory = 0;
        // Guards the fixture: the raw bytes really do look like an emission, which is what makes
        // the neutralize step load-bearing rather than decorative.
        TestEqual(TEXT("the commented-out fixture does match when the raw bytes are scanned"),
            CollectRawCodesInFile(CommentedOutEmission, RawCodes, Wrapped, Bare, Indirect, Factory),
            1);

        TArray<FString> Codes;
        Wrapped = Bare = Indirect = Factory = 0;
        TestEqual(TEXT("a commented-out SendError is not counted as an emitted code"),
            CollectRawCodesInFile(NeutralizeSourceText(CommentedOutEmission), Codes,
                Wrapped, Bare, Indirect, Factory),
            0);
    }

    return true;
}
