// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared render-and-assert helper for wiki-doc regression tests (the tests that
// render a docs/wiki-src page through WikiHandler::RenderPage and assert overlay
// markers survive). Extracted from the inline RenderOrFail helpers previously
// duplicated across Tests/Infra/TestAnimGraphNodeNamingDocs.cpp and
// Tests/Infra/TestGeometryPrimitiveOrientationDocs.cpp. Required because the
// plugin's tests share a single module under Unity builds: anonymous-namespace
// helpers with the same name across .cpp files produce ODR / redefinition errors
// when Unity merges them into one translation unit.
//
// Conventions match Tests/Infra/DispatcherTestHelpers.h: named namespace + inline
// functions, no module API macro.

#include "Misc/AutomationTest.h"
#include "Catalog/WikiHandler.h"
#include "Utils/StringUtils.h"

namespace WikiDocTestHelpers
{
    // Render a wiki page or fail the test with a clear reason. Returns false when the
    // wiki renderer is unavailable (no editor subsystem/dispatcher in this run).
    inline bool RenderOrFail(FAutomationTestBase& Test, const TCHAR* Path, FString& OutText)
    {
        if (!WikiHandler::RenderPage(Path, OutText))
        {
            Test.AddError(FString::Printf(TEXT("Wiki renderer unavailable for '%s' (no editor subsystem/dispatcher)"), Path));
            return false;
        }
        Test.TestFalse(*FString::Printf(TEXT("%s page is not a Not-Found page"), Path),
            OutText.Contains(TEXT("# Not found:")));
        return true;
    }

    // Extract the body of a "## <Heading>" section from a rendered page: everything
    // after the heading line, up to the next "## " heading or the end of the page.
    // Returns false when the page carries no such section.
    inline bool ExtractSection(const FString& Page, const TCHAR* Heading, FString& OutSection)
    {
        const FString HeadingLine = FString::Printf(TEXT("## %s\n"), Heading);
        const int32 Start = Page.Find(HeadingLine, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        if (Start == INDEX_NONE)
        {
            return false;
        }
        const int32 BodyStart = Start + HeadingLine.Len();
        const int32 Next = Page.Find(TEXT("\n## "), ESearchCase::CaseSensitive, ESearchDir::FromStart, BodyStart);
        OutSection = (Next == INDEX_NONE) ? Page.Mid(BodyStart) : Page.Mid(BodyStart, Next - BodyStart);
        return true;
    }

    // Parse the "- `slug` — summary" bullet lines of a section body, in render order.
    // Summary is empty for a bare "- `slug`" entry.
    inline void ParseSlugBullets(const FString& Section, TArray<TPair<FString, FString>>& OutEntries)
    {
        const FString Marker(TEXT("- `"));
        const FString Separator(TEXT(" — "));

        TArray<FString> Lines;
        Section.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
        for (const FString& Line : Lines)
        {
            if (!Line.StartsWith(Marker))
            {
                continue;
            }
            const int32 Close = Line.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Marker.Len());
            if (Close == INDEX_NONE)
            {
                continue;
            }
            FString Summary;
            const int32 Dash = Line.Find(Separator, ESearchCase::CaseSensitive, ESearchDir::FromStart, Close);
            if (Dash != INDEX_NONE)
            {
                Summary = Line.Mid(Dash + Separator.Len()).TrimStartAndEnd();
            }
            OutEntries.Emplace(Line.Mid(Marker.Len(), Close - Marker.Len()), Summary);
        }
    }

    // Assert the shared "finding a container target" discovery recipe + limitation note.
    // The recipe (property.list -> cppType scan) and the "nameMatch narrows by name only,
    // no cppType/container-kind filter" caveat are container-kind-agnostic and live once on
    // the parent container.md overlay (## Cross-cluster overlap), not on each child page.
    inline void AssertDiscoveryRecipe(FAutomationTestBase& Test, const TCHAR* PageLabel, const FString& Text)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s gives the property.list -> cppType scan recipe"), PageLabel),
            Text.Contains(TEXT("cppType")) && Text.Contains(TEXT("property.list")));
        Test.TestTrue(*FString::Printf(TEXT("%s notes property.list has no cppType/container-kind filter"), PageLabel),
            Text.Contains(TEXT("nameMatch")) && Text.Contains(TEXT("container-kind filter")));
    }

    // Assert a wiki example uses the live param keys the handlers actually read and
    // does NOT carry stale/phantom keys that would make a copy-pasted call hard-reject.
    // This is the recurring "doc example uses live camelCase keys, not stale snake_case
    // keys" check (E-*-example-*-params tickets) lifted out of the per-test bodies.
    //
    // LiveKeys are matched case-insensitively (the camelCase spellings the handlers read).
    // StaleKeys must be passed in their `<key>:` JSON spelling and are matched
    // case-sensitively, so the negative assertions pin the param key without colliding
    // with method names (add_montage_section) or error-code prose (NODE_CLASS_NOT_FOUND)
    // elsewhere on the page. This encodes the `<key>:`/CaseSensitive matching convention
    // once instead of re-deriving it in a comment on every test.
    inline void AssertExampleUsesParamKeys(
        FAutomationTestBase& Test,
        const TCHAR* PageLabel,
        const FString& Text,
        TArrayView<const TCHAR* const> LiveKeys,
        TArrayView<const TCHAR* const> StaleKeysWithColon)
    {
        for (const TCHAR* Live : LiveKeys)
        {
            Test.TestTrue(*FString::Printf(TEXT("%s example uses the %s key"), PageLabel, Live),
                Text.Contains(Live));
        }
        for (const TCHAR* Stale : StaleKeysWithColon)
        {
            Test.TestFalse(*FString::Printf(TEXT("%s example does not use the stale %s key"), PageLabel, Stale),
                Text.Contains(Stale, ESearchCase::CaseSensitive));
        }
    }

    // Assert a forbidden `<key>:` does not appear EXCEPT as the suffix of a legitimate
    // key (e.g. `endTime:` must not appear on its own, but the legitimate `blendTime:`
    // ends in that substring). Robust against formatting changes in the example: rather
    // than matching a position-dependent literal (" endTime:" with a hard-coded leading
    // space), it asserts every occurrence of the forbidden token is the tail of an
    // allowed token by comparing their case-sensitive occurrence counts. If the example
    // ever reformats the allowed key (quoted spelling, different surrounding whitespace),
    // the counts still track and the assertion stays meaningful.
    inline void AssertForbiddenKeyOnlyAsSuffix(
        FAutomationTestBase& Test,
        const TCHAR* PageLabel,
        const FString& Text,
        const TCHAR* ForbiddenWithColon,
        const TCHAR* AllowedSuffixWithColon)
    {
        const int32 ForbiddenCount = PinWright::CountSubstring(Text, ForbiddenWithColon);
        const int32 AllowedCount = PinWright::CountSubstring(Text, AllowedSuffixWithColon);
        Test.TestEqual(
            *FString::Printf(TEXT("%s example uses %s only as the tail of %s"), PageLabel, ForbiddenWithColon, AllowedSuffixWithColon),
            ForbiddenCount, AllowedCount);
    }

    // Assert a page points at the camera read-back. Every editor.* camera-moving verb's
    // per-method page (and editor.status) must name the read-back method
    // system.inspect.get_viewport_info; the camera-moving verbs additionally name at least
    // one of the camera fields it returns. editor.status carries no camera field of its own,
    // so pass bExpectCameraFields=false for it.
    inline void AssertNamesViewportInfoReadback(FAutomationTestBase& Test, const TCHAR* PageLabel, const FString& Text, bool bExpectCameraFields = true)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s page points to system.inspect.get_viewport_info as the read-back"), PageLabel),
            Text.Contains(TEXT("system.inspect.get_viewport_info")));
        if (bExpectCameraFields)
        {
            Test.TestTrue(*FString::Printf(TEXT("%s page names the camera fields the read-back returns"), PageLabel),
                Text.Contains(TEXT("cameraLocation")) || Text.Contains(TEXT("cameraRotation")));
        }
    }

    // Assert a namespace overlay documents the async ticket -> system.job_status poll
    // contract for one or more long-running verbs (the E-*-async-poll-undocumented ticket
    // family: navigation.rebuild_navigation, level.build_lighting, run_ubt, run_benchmark,
    // render-nanite rebuild …). The shared contract a page must carry is: it names each
    // long-running verb, says the verb is async / returns a ticket, and cross-links
    // system.job_status as the poll follow-up. Lifted out of the per-test bodies so the
    // required markers live in one place; per-page extras (e.g. the level page's stale
    // "blocks the editor" gotcha negative checks) stay inline in their own test.
    inline void AssertAsyncTicketPollContract(
        FAutomationTestBase& Test,
        const TCHAR* PageLabel,
        const FString& Text,
        TArrayView<const TCHAR* const> VerbNames)
    {
        for (const TCHAR* Verb : VerbNames)
        {
            Test.TestTrue(*FString::Printf(TEXT("%s page names %s"), PageLabel, Verb),
                Text.Contains(Verb));
        }
        // "ticketed job" is accepted alongside "async" because the level.save / level.save_as
        // pages were corrected to the precise two-response rule: the verb is always ticketed,
        // but a streaming-capable client gets the finished result inline and never sees the
        // ticket, so calling it flatly "async" was the less accurate description. The bar is
        // unchanged - the page must still name a ticket AND characterise the verb - and every
        // other caller of this helper still satisfies it via "async".
        Test.TestTrue(*FString::Printf(TEXT("%s page says the verb returns a ticket / is async"), PageLabel),
            Text.Contains(TEXT("ticket"))
                && (Text.Contains(TEXT("async")) || Text.Contains(TEXT("ticketed job"))));
        Test.TestTrue(*FString::Printf(TEXT("%s page points the async verb at the system.job_status poll"), PageLabel),
            Text.Contains(TEXT("system.job_status")));
    }
}
