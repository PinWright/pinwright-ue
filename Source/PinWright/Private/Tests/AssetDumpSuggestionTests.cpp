// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/AssetDumpSuggestion.h"
#include "Utils/AssetDumpWriter.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"

// Unit coverage for AssetDumpSuggestion::BuildDumpSuggestionHint.
//
// The helper answers "should the caller be nudged to (re)dump this subject?" by
// deriving the expected dump-mirror directory under <ProjectSavedDir>/PinWright/
// asset-dumps/ from a package path and reporting whether it exists / is fresh.
// A missing mirror yields a non-empty imperative hint naming asset.dump_folder
// targets; a fresh mirror yields "".
//
// These cases exercise only the missing-mirror and empty-input branches, which
// need no filesystem setup (a bogus package path is guaranteed to have no
// mirror). The fresh -> empty branch depends on a real dump existing on disk and
// is covered by manual runtime verification, not here. The per-file dirty-guard
// helpers those runtime checks lean on are file-local (anonymous namespace) to
// AssetDumpHandler.cpp and thus not linkable from a test TU — likewise verified
// at runtime rather than in this suite.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionEmptyPathTest,
    "PinWright.AssetDumpSuggestion.EmptyPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionEmptyPathTest::RunTest(const FString& Parameters)
{
    // No subject -> nothing to suggest.
    const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(
        TEXT(""), AssetDumpSuggestion::EDumpSubjectKind::Asset);
    TestTrue(TEXT("Empty package path yields no hint"), Hint.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionMissingPluginMountTest,
    "PinWright.AssetDumpSuggestion.MissingPluginMount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionMissingPluginMountTest::RunTest(const FString& Parameters)
{
    // A subject under a plugin mount with no dump mirror must produce a hint that
    // names the asset.dump_folder RPC, the mount root derived from the path, the
    // containing folder, and the always-suggested /Game default.
    //
    // Each target is asserted as a whole {"folderPath":"<x>"} payload, NOT as a bare
    // substring: the opener interpolates the subject's package path verbatim, so a bare
    // Contains("/PwSuggestTestPlugin") is satisfied by the echoed path alone and would
    // still pass with the MountRoot/ContainingFolder derivation blocks deleted.
    const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(
        TEXT("/PwSuggestTestPlugin/Foo/BP_Fake"), AssetDumpSuggestion::EDumpSubjectKind::Asset);
    TestFalse(TEXT("Missing plugin-mount mirror yields a hint"), Hint.IsEmpty());
    TestTrue(TEXT("Hint names the asset.dump_folder RPC"),
        Hint.Contains(TEXT("asset.dump_folder")));
    TestTrue(TEXT("Hint offers the derived mount root as a dump target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/PwSuggestTestPlugin\"}")));
    TestTrue(TEXT("Hint offers the derived containing folder as a dump target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/PwSuggestTestPlugin/Foo\"}")));
    TestTrue(TEXT("Hint always offers the /Game default as a dump target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/Game\"}")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionMissingGameMountTest,
    "PinWright.AssetDumpSuggestion.MissingGameMount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionMissingGameMountTest::RunTest(const FString& Parameters)
{
    // A subject under /Game with no dump mirror must still produce a hint naming
    // the asset.dump_folder RPC and the /Game target. As above, the targets are asserted
    // as whole {"folderPath":"<x>"} payloads because the opener echoes the package path
    // (which starts with /Game) and would satisfy a bare Contains("/Game") on its own.
    const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(
        TEXT("/Game/__PinWrightTest_NoSuchDir__/BP_Fake"), AssetDumpSuggestion::EDumpSubjectKind::Asset);
    TestFalse(TEXT("Missing /Game mirror yields a hint"), Hint.IsEmpty());
    TestTrue(TEXT("Hint names the asset.dump_folder RPC"),
        Hint.Contains(TEXT("asset.dump_folder")));
    TestTrue(TEXT("Hint offers the full-project /Game dump target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/Game\"}")));
    TestTrue(TEXT("Hint offers the derived containing folder as the narrower target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/Game/__PinWrightTest_NoSuchDir__\"}")));
    // The under-/Game branch is distinct from the plugin-mount branch: it must not
    // re-list /Game as a third fallback, so its plugin-specific wording is absent.
    TestFalse(TEXT("An under-/Game subject does not take the plugin-mount wording"),
        Hint.Contains(TEXT("this plugin, where you're working")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionLevelKindTest,
    "PinWright.AssetDumpSuggestion.LevelKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionLevelKindTest::RunTest(const FString& Parameters)
{
    // A Level subject with no dump mirror must produce a hint that opts levels in
    // via the includeLevels flag (levels are excluded from a default folder dump).
    const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(
        TEXT("/Game/__PinWrightTest_NoSuchDir__/MyLevel"), AssetDumpSuggestion::EDumpSubjectKind::Level);
    TestFalse(TEXT("Missing level mirror yields a hint"), Hint.IsEmpty());
    // includeLevels must ride on the folder target, not merely appear somewhere in the
    // prose — the flag is useless to a caller unless it is inside the dump_folder payload.
    TestTrue(TEXT("Level hint sets includeLevels on the derived folder target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/Game/__PinWrightTest_NoSuchDir__\",\"includeLevels\":true}")));
    TestTrue(TEXT("Level hint offers the single-subject asset.dump alternative"),
        Hint.Contains(TEXT("asset.dump({\"assetPath\":\"/Game/__PinWrightTest_NoSuchDir__/MyLevel\"})")));
    return true;
}

// The freshness/staleness discriminator — bMirrorExists and the fresh -> "" early return —
// is what makes the hint a nudge rather than noise. Without this case every other test in
// this file hits the missing-mirror branch, so BuildDumpSuggestionHint could be reduced to
// "always return a hint" and the whole file would stay green.
//
// Level subjects are used because a level mirror with no meta.json source stamp is gated by
// the directory alone, so the case needs a mkdir rather than a real registry-fresh
// asset dump. The Asset-kind fresh path additionally runs AssetDumpCache::IsDumpFresh and
// is still uncovered here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionExistingLevelMirrorIsSilentTest,
    "PinWright.AssetDumpSuggestion.ExistingLevelMirrorIsSilent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionExistingLevelMirrorIsSilentTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/__PinWrightTest_SuggestMirror_%s/MyLevel"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Same resolver the production hint uses, so the directory lands exactly where the
    // DirectoryExists probe looks.
    const FString DumpDir = AssetDumpWriter::ResolveDumpDir(PackagePath, TEXT(""));
    if (!TestFalse(TEXT("resolved a dump mirror directory"), DumpDir.IsEmpty()))
    {
        return true;
    }

    // Precondition: with no mirror on disk the same subject DOES get a hint. Asserting it
    // here makes the silence below attributable to the directory and nothing else.
    IFileManager::Get().DeleteDirectory(*DumpDir, /*RequireExists=*/false, /*Tree=*/true);
    TestFalse(TEXT("without a mirror the level subject yields a hint"),
        AssetDumpSuggestion::BuildDumpSuggestionHint(
            PackagePath, AssetDumpSuggestion::EDumpSubjectKind::Level).IsEmpty());

    if (!TestTrue(TEXT("created the dump mirror directory"),
            IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true)))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*DumpDir, /*RequireExists=*/false, /*Tree=*/true);
    };

    TestTrue(TEXT("an existing level mirror silences the nudge"),
        AssetDumpSuggestion::BuildDumpSuggestionHint(
            PackagePath, AssetDumpSuggestion::EDumpSubjectKind::Level).IsEmpty());
    return true;
}

// B-asset-dump-no-source-freshness-stamp: a level mirror is no longer judged fresh by its
// directory alone. Its meta.json `source.fileMd5` is compared with the map file now on
// disk; a mismatch (here: a recorded hash for a package whose file is gone) is STALE, a
// match (null == null, no file then or now) stays silent. Reverting the comparison makes
// the existing mirror silence the stale case and the first assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionLevelMirrorWithChangedMapFileIsStaleTest,
    "PinWright.AssetDumpSuggestion.LevelMirrorWithChangedMapFileIsStale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionLevelMirrorWithChangedMapFileIsStaleTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/__PinWrightTest_SuggestStamp_%s/MyLevel"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString DumpDir = AssetDumpWriter::ResolveDumpDir(PackagePath, TEXT(""));
    if (!TestTrue(TEXT("created the dump mirror directory"),
            IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true)))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*DumpDir, /*RequireExists=*/false, /*Tree=*/true);
    };
    const FString MetaPath = DumpDir / TEXT("meta.json");

    FFileHelper::SaveStringToFile(
        TEXT("{\"source\": {\"fileMd5\": \"0123456789abcdef0123456789abcdef\", \"unsavedChanges\": false}}"),
        *MetaPath);
    const FString StaleHint = AssetDumpSuggestion::BuildDumpSuggestionHint(
        PackagePath, AssetDumpSuggestion::EDumpSubjectKind::Level);
    TestTrue(TEXT("a recorded map hash that no longer matches is reported STALE"), StaleHint.Contains(TEXT("STALE")));
    TestTrue(TEXT("the stale hint names the single-level re-dump"),
        StaleHint.Contains(FString::Printf(TEXT("asset.dump({\"assetPath\":\"%s\"})"), *PackagePath)));

    FFileHelper::SaveStringToFile(TEXT("{\"source\": {\"fileMd5\": null, \"unsavedChanges\": false}}"), *MetaPath);
    TestTrue(TEXT("a recorded stamp that matches the file now on disk is silent"),
        AssetDumpSuggestion::BuildDumpSuggestionHint(
            PackagePath, AssetDumpSuggestion::EDumpSubjectKind::Level).IsEmpty());
    return true;
}
