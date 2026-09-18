// Copyright (c) 2026 Alexander Penkin. MIT License.

// preview.png's aspect version, and the reason it does nothing yet.
//
// The widget Designer preview is now stamped opaque before encoding
// (WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng), so the aspect's serialized bytes changed
// for every widget with an uncovered region. Plugin CLAUDE.md's Aspect Version Bumping rule
// requires the bump in the same commit, and it is taken: GetAspectVersion now carries
// { TEXT("preview.png"), 2 }, where the aspect previously had no row and sat at
// AssetDumpDefaultAspectVersion.
//
// THE CLAIM THAT MOTIVATED THE BUMP IS FALSE, and this file records why rather than leaving the
// next reader to rediscover it. The bump was requested on the grounds that "every existing
// .dumpcache.json will keep serving the old transparent PNG as fresh". No .dumpcache.json has
// ever contained a preview.png entry:
//
//   * AssetDumpHandler.cpp's WriteDumpCacheForSuccessfulBaseline returns false immediately when
//     bIncludeWidgetScreenshot is set, so a dump that produced preview.png writes NO cache
//     record at all.
//   * AssetDumpCache.cpp's IsCacheFresh short-circuits to Stale("includeWidgetScreenshot
//     bypasses cache") before it compares anything, so a request that would want preview.png
//     never reads one either.
//
// So the row is inert today. It is kept because the rule is written against serialized bytes
// rather than against reachability, because the version is also the provenance marker written
// into the record for anything that ever does cache this aspect, and because the moment the
// bypass is lifted the row is the only thing standing between a caller and a stale transparent
// PNG. WidgetScreenshotDumpsBypassTheCacheEntirely is the tripwire: lift the bypass and it goes
// red, pointing at PreviewPngAspectVersionWouldInvalidatePreStampCaches for the mechanism that
// then starts carrying weight.

#include "Handlers/Asset/AssetDumpCache.h"
#include "Handlers/Asset/AssetDumpHandler.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
    FString PWAdpavMakeDir()
    {
        FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
            / TEXT("PinWrightTests")
            / TEXT("AssetDumpPreviewAspectVersion")
            / FGuid::NewGuid().ToString(EGuidFormats::Digits);
        FPaths::NormalizeDirectoryName(Dir);
        IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
        return Dir;
    }

    bool PWAdpavWriteStub(const FString& Path)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
        return FFileHelper::SaveStringToFile(
            TEXT("{}"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    AssetDumpCache::FAssetDumpSourceFingerprint PWAdpavMakeSource()
    {
        AssetDumpCache::FAssetDumpPackageFacts Facts;
        Facts.PackageName = TEXT("/Game/Test/WidgetPreviewAspect");
        Facts.CorrectCasePackageName = TEXT("/Game/Test/WidgetPreviewAspect");
        Facts.PackageExtension = TEXT("Asset");
        Facts.PackageSavedHash = TEXT("2222222222222222222222222222222222222222");
        Facts.RegistryDiskSize = 77;
        Facts.bHasPackageData = true;
        return AssetDumpCache::BuildSourceFingerprintFromFacts(Facts);
    }

    // A dump dir holding exactly meta.json and preview.png, plus the matching current-version
    // record. HasAllWrittenFiles and HasOnlyListedDumpFiles both walk the real directory, so the
    // stub files have to exist for any freshness answer to be about the fingerprints.
    struct FPWAdpavFixture
    {
        FString Dir;
        TArray<FString> WrittenFiles;
        AssetDumpCache::FAssetDumpSourceFingerprint Source;
        AssetDumpCache::FAssetDumpDumperFingerprint CurrentDumper;
        AssetDumpCache::FAssetDumpCacheRecord CurrentRecord;
    };

    bool PWAdpavBuildFixture(FPWAdpavFixture& Out)
    {
        Out.Dir = PWAdpavMakeDir();
        Out.WrittenFiles = {FString(DumpFileNames::Meta), FString(DumpFileNames::WidgetPreviewPng)};
        for (const FString& RelativeFile : Out.WrittenFiles)
        {
            if (!PWAdpavWriteStub(Out.Dir / RelativeFile))
            {
                return false;
            }
        }
        Out.Source = PWAdpavMakeSource();
        Out.CurrentDumper = AssetDumpCache::MakeCurrentDumperFingerprint(
            AssetDumpCache::MakeCurrentAspectVersions(Out.WrittenFiles));
        Out.CurrentRecord = AssetDumpCache::MakeCacheRecord(
            TEXT("/Game/Test/WidgetPreviewAspect"),
            TEXT("/Game/Test/WidgetPreviewAspect"),
            TEXT("/Game/Test/WidgetPreviewAspect"),
            Out.Source,
            Out.CurrentDumper,
            AssetDumpCache::FAssetDumpOptionsFingerprint(),
            Out.WrittenFiles);
        return true;
    }
}

// Counterfactual: delete the { TEXT("preview.png"), 2 } row from GetAspectVersion's Versions
// table and this test fails at its first assertion -- the "pre-stamp" record it builds becomes
// identical to the current one, so IsCacheFresh reports it fresh.
//
// Unable to fail if it had asserted GetAspectVersion("preview.png") == 2 and stopped there:
// that reads back the same literal the table sets and would pass on any number, the default
// included. The load-bearing assertion models a record written before the bump and requires the
// freshness comparison to reject it. It is also unable to fail if the two records differed in
// some OTHER field, which is what the fresh-control assertion at the end rules out.
//
// Reachability: this exercises the comparison in isolation, with the options fingerprint left
// at its default. Production never reaches it -- see WidgetScreenshotDumpsBypassTheCacheEntirely
// and the file header.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpPreviewPngAspectVersionWouldInvalidateTest,
    "PinWright.AssetDumpCache.PreviewPngAspectVersionWouldInvalidatePreStampCaches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpPreviewPngAspectVersionWouldInvalidateTest::RunTest(const FString& Parameters)
{
    FPWAdpavFixture Fx;
    if (!TestTrue(TEXT("fixture dump dir built"), PWAdpavBuildFixture(Fx)))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Fx.Dir, /*RequireExists=*/false, /*Tree=*/true);
    };

    // Every written file gets an entry, default or not. This is the property that makes adding
    // a first-ever row behave exactly like incrementing an existing one, and it is asserted
    // rather than assumed -- if MakeCurrentAspectVersions had skipped unlisted aspects, a
    // pre-stamp record would carry no preview.png key and 1 -> 2 would compare equal-by-absence.
    TestTrue(TEXT("MakeCurrentAspectVersions materialises an entry for preview.png"),
        Fx.CurrentDumper.AspectVersions.Contains(DumpFileNames::WidgetPreviewPng));

    const AssetDumpCache::FAssetDumpOptionsFingerprint Options;

    // A cache written before the opaque stamp: identical in every respect except that its
    // preview.png entry carries the default version, which is what GetAspectVersion returned
    // while the aspect had no row.
    AssetDumpCache::FAssetDumpCacheRecord PreStampRecord = Fx.CurrentRecord;
    PreStampRecord.Dumper.AspectVersions.Add(
        FString(DumpFileNames::WidgetPreviewPng),
        AssetDumpCache::AssetDumpDefaultAspectVersion);

    const AssetDumpCache::FAssetDumpFreshnessResult PreStamp = AssetDumpCache::IsCacheFresh(
        Fx.Dir, TEXT("/Game/Test/WidgetPreviewAspect"), Fx.Source, Fx.CurrentDumper, Options,
        PreStampRecord);
    TestFalse(
        FString::Printf(
            TEXT("a record carrying preview.png at the pre-stamp version is stale ")
            TEXT("(reason='%s')"),
            *PreStamp.Reason),
        PreStamp.bFresh);

    // Control. Without it, a fixture broken any other way -- missing stub file, mismatched
    // source, wrong options -- would report stale too and the assertion above would pass for
    // the wrong reason.
    const AssetDumpCache::FAssetDumpFreshnessResult Post = AssetDumpCache::IsCacheFresh(
        Fx.Dir, TEXT("/Game/Test/WidgetPreviewAspect"), Fx.Source, Fx.CurrentDumper, Options,
        Fx.CurrentRecord);
    TestTrue(
        FString::Printf(
            TEXT("the same record at the current aspect version IS fresh (reason='%s') -- so ")
            TEXT("the staleness above is attributable to preview.png's version alone"),
            *Post.Reason),
        Post.bFresh);

    return true;
}

// The tripwire. A widget-screenshot dump is not cached at either end, which is why the aspect
// version above cannot invalidate anything in production and why the "stale caches keep serving
// the old transparent PNG" premise for the bump does not hold.
//
// Counterfactual: remove the ExpectedOptions.bIncludeWidgetScreenshot short-circuit at the top
// of IsCacheFresh and this test fails -- at which point preview.png's aspect version starts
// carrying real weight and PreviewPngAspectVersionWouldInvalidatePreStampCaches becomes the
// test that matters. Whoever makes that change should read both.
//
// Unable to fail if it had only asserted "stale": a record differing in ANY field is stale.
// The two calls differ in exactly one input, the options flag, and the second is required to
// come back FRESH -- so the staleness is attributable to the flag and nothing else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetScreenshotBypassesCacheTest,
    "PinWright.AssetDumpCache.WidgetScreenshotDumpsBypassTheCacheEntirely",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpWidgetScreenshotBypassesCacheTest::RunTest(const FString& Parameters)
{
    FPWAdpavFixture Fx;
    if (!TestTrue(TEXT("fixture dump dir built"), PWAdpavBuildFixture(Fx)))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Fx.Dir, /*RequireExists=*/false, /*Tree=*/true);
    };

    AssetDumpCache::FAssetDumpOptionsFingerprint ScreenshotOptions;
    ScreenshotOptions.bIncludeWidgetScreenshot = true;

    // The record is a perfect match for the request in every field the freshness check reads.
    // It is still refused, before any fingerprint is compared.
    AssetDumpCache::FAssetDumpCacheRecord MatchingRecord = Fx.CurrentRecord;
    MatchingRecord.Options = ScreenshotOptions;

    const AssetDumpCache::FAssetDumpFreshnessResult Bypassed = AssetDumpCache::IsCacheFresh(
        Fx.Dir, TEXT("/Game/Test/WidgetPreviewAspect"), Fx.Source, Fx.CurrentDumper,
        ScreenshotOptions, MatchingRecord);
    TestFalse(
        FString::Printf(
            TEXT("a request with includeWidgetScreenshot is never served from cache ")
            TEXT("(reason='%s')"),
            *Bypassed.Reason),
        Bypassed.bFresh);

    const AssetDumpCache::FAssetDumpOptionsFingerprint PlainOptions;
    const AssetDumpCache::FAssetDumpFreshnessResult Plain = AssetDumpCache::IsCacheFresh(
        Fx.Dir, TEXT("/Game/Test/WidgetPreviewAspect"), Fx.Source, Fx.CurrentDumper,
        PlainOptions, Fx.CurrentRecord);
    TestTrue(
        FString::Printf(
            TEXT("the same dir and fingerprints ARE fresh without the flag (reason='%s') -- so ")
            TEXT("the refusal above is the flag, not the fixture"),
            *Plain.Reason),
        Plain.bFresh);

    return true;
}

// The number itself, recorded the way every other bumped aspect records it (see
// PinWright.AssetDumpCache.AnimSequenceAspectVersion / .BpirAspectVersion). A documentation
// pin, not evidence: it reads back the literal the table sets and cannot fail unless somebody
// changes the number without changing this line.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpPreviewPngAspectVersionNumberTest,
    "PinWright.AssetDumpCache.PreviewPngAspectVersionNumber",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAssetDumpPreviewPngAspectVersionNumberTest::RunTest(const FString& Parameters)
{
    // 2: the Designer preview capture is stamped opaque before encoding, so every pixel's alpha
    //    is 0xFF where the render target's transparent clear used to survive into the file.
    // 3: the preview is no longer sRGB-encoded twice, so every colour byte in every widget
    //    preview.png changes (B-screenshot-designer-double-srgb).
    TestEqual(TEXT("preview.png explicit aspect version"),
        AssetDumpCache::GetAspectVersion(DumpFileNames::WidgetPreviewPng),
        static_cast<int32>(3));
    TestTrue(TEXT("preview.png is no longer served at the default aspect version"),
        AssetDumpCache::GetAspectVersion(DumpFileNames::WidgetPreviewPng)
            != AssetDumpCache::AssetDumpDefaultAspectVersion);
    return true;
}
