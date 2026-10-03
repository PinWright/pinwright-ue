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
// A missing mirror yields a non-empty imperative hint that leads with asset.dump
// of the subject; a fresh mirror yields "".
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

// E-property-stale-dump-hint-suggests-whole-project: the staleness is per asset and the
// editor is often shared, so the Asset-kind hint leads with the single-asset asset.dump
// (interpolated, not templated), offers only the asset's own folder as the wider sweep with
// a cost line, and never names a project root such as /Game as a folder target.
namespace AssetDumpSuggestionTestsHelpers
{
    void ExpectNarrowAssetHint(FAutomationTestBase& Test, const FString& PackagePath, const FString& ContainingFolder)
    {
        const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(
            PackagePath, AssetDumpSuggestion::EDumpSubjectKind::Asset);
        Test.TestFalse(TEXT("Missing mirror yields a hint"), Hint.IsEmpty());

        const FString SingleAsset = FString::Printf(TEXT("asset.dump({\"assetPath\":\"%s\"})"), *PackagePath);
        const FString FolderSweep = FString::Printf(TEXT("asset.dump_folder({\"folderPath\":\"%s\"})"), *ContainingFolder);
        const int32 SingleAt = Hint.Find(SingleAsset, ESearchCase::CaseSensitive);
        const int32 FolderAt = Hint.Find(FolderSweep, ESearchCase::CaseSensitive);
        Test.TestTrue(TEXT("Hint names the single-asset asset.dump with this request's path"), SingleAt != INDEX_NONE);
        Test.TestTrue(TEXT("Hint offers the asset's own containing folder as the wider sweep"), FolderAt != INDEX_NONE);
        Test.TestTrue(TEXT("The single-asset dump leads; the folder sweep comes after it"),
            SingleAt != INDEX_NONE && FolderAt != INDEX_NONE && SingleAt < FolderAt);
        Test.TestTrue(TEXT("The folder sweep states its cost"), Hint.Contains(TEXT("dumps every non-level asset under that path")));

        // Count folderPath payloads: only the containing folder may appear, never /Game or a mount root.
        int32 FolderPayloads = 0;
        for (int32 At = Hint.Find(TEXT("\"folderPath\"")); At != INDEX_NONE;
             At = Hint.Find(TEXT("\"folderPath\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
        {
            ++FolderPayloads;
        }
        Test.TestEqual(TEXT("Exactly one folder target is offered"), FolderPayloads, 1);
        Test.TestFalse(TEXT("No copy-pasteable whole-project /Game dump"), Hint.Contains(TEXT("{\"folderPath\":\"/Game\"}")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionMissingPluginMountTest,
    "PinWright.AssetDumpSuggestion.MissingPluginMount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionMissingPluginMountTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = TEXT("/PwSuggestTestPlugin/Foo/BP_Fake");
    AssetDumpSuggestionTestsHelpers::ExpectNarrowAssetHint(*this, PackagePath, TEXT("/PwSuggestTestPlugin/Foo"));
    const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(PackagePath, AssetDumpSuggestion::EDumpSubjectKind::Asset);
    TestFalse(TEXT("The plugin mount root is not offered as a dump target"),
        Hint.Contains(TEXT("{\"folderPath\":\"/PwSuggestTestPlugin\"}")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionMissingGameMountTest,
    "PinWright.AssetDumpSuggestion.MissingGameMount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionMissingGameMountTest::RunTest(const FString& Parameters)
{
    AssetDumpSuggestionTestsHelpers::ExpectNarrowAssetHint(*this,
        TEXT("/Game/__PinWrightTest_NoSuchDir__/BP_Fake"), TEXT("/Game/__PinWrightTest_NoSuchDir__"));
    return true;
}

// A subject directly under a mount root has the mount root itself as its folder, so a folder
// sweep there is a whole-project (or whole-plugin) dump. Both the Asset and the missing-Level
// hint must then offer only the single-subject asset.dump and no folderPath at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpSuggestionMissingRootLevelAssetTest,
    "PinWright.AssetDumpSuggestion.MissingRootLevelAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDumpSuggestionMissingRootLevelAssetTest::RunTest(const FString& Parameters)
{
    struct FCase { const TCHAR* Path; AssetDumpSuggestion::EDumpSubjectKind Kind; const TCHAR* Label; };
    const FCase Cases[] = {
        { TEXT("/Game/__PwSuggestRootFake_BP"), AssetDumpSuggestion::EDumpSubjectKind::Asset, TEXT("asset") },
        { TEXT("/Game/__PwSuggestRootFake_Level"), AssetDumpSuggestion::EDumpSubjectKind::Level, TEXT("level") },
        { TEXT("/PwSuggestTestPlugin/BP_Fake"), AssetDumpSuggestion::EDumpSubjectKind::Asset, TEXT("plugin-root asset") },
    };
    for (const FCase& Case : Cases)
    {
        // Precondition: no mirror on disk, so the subject takes the missing-mirror branch.
        const FString DumpDir = AssetDumpWriter::ResolveDumpDir(Case.Path, TEXT(""));
        if (!TestFalse(FString::Printf(TEXT("%s: fixture path has no dump mirror"), Case.Label),
                IFileManager::Get().DirectoryExists(*DumpDir)))
        {
            continue;
        }
        const FString Hint = AssetDumpSuggestion::BuildDumpSuggestionHint(Case.Path, Case.Kind);
        TestTrue(FString::Printf(TEXT("%s: hint offers the single-subject asset.dump"), Case.Label),
            Hint.Contains(FString::Printf(TEXT("asset.dump({\"assetPath\":\"%s\"})"), Case.Path)));
        TestFalse(FString::Printf(TEXT("%s: hint offers no folder sweep of the mount root"), Case.Label),
            Hint.Contains(TEXT("\"folderPath\"")));
    }
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
    const int32 SingleAt = Hint.Find(TEXT("asset.dump({\"assetPath\":\"/Game/__PinWrightTest_NoSuchDir__/MyLevel\"})"));
    TestTrue(TEXT("Level hint offers the single-subject asset.dump"), SingleAt != INDEX_NONE);
    TestTrue(TEXT("Level hint leads with the single-subject asset.dump, before the folder sweep"),
        SingleAt != INDEX_NONE && SingleAt < Hint.Find(TEXT("asset.dump_folder")));
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
