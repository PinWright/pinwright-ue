// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace PwModelReservedSurfaceTestPrivate
{
    bool Parse(const TCHAR* Source, FPwModelDocument& OutDocument,
               TArray<FPwDiagnostic>& OutDiagnostics)
    {
        return FPwModelParser::Parse(Source, OutDocument, OutDiagnostics);
    }

    const FPwDiagnostic* FindDiagnostic(const TArray<FPwDiagnostic>& Diagnostics,
                                             const TCHAR* Code)
    {
        for (const FPwDiagnostic& Diagnostic : Diagnostics)
        {
            if (Diagnostic.Code == Code)
            {
                return &Diagnostic;
            }
        }
        return nullptr;
    }

    TSet<FString> CollectCodes(const FString& Contents, const FRegexPattern& Pattern)
    {
        TSet<FString> Codes;
        FRegexMatcher Matcher(Pattern, Contents);
        while (Matcher.FindNext())
        {
            Codes.Add(Matcher.GetCaptureGroup(1));
        }
        return Codes;
    }

    FString ResolvePluginDir()
    {
        // This test is compiled into the project plugin itself. Resolving from the
        // host's Plugins directory keeps the scan independent of a Projects-module
        // link and still follows the actual checkout used by the suite.
        return FPaths::ProjectPluginsDir() / TEXT("PinWright");
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReservedSurfaceHeaderTest,
    "PinWright.Model.ReservedSurface.DocumentCarriesTheSharedHeaderAndUses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelReservedSurfaceHeaderTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = PwModelReservedSurfaceTestPrivate::Parse(
        TEXT("pwmodel 0\n")
        TEXT("use skeleton from \"/Game/Rigs/Hero.Hero\"\n")
        TEXT("part body {\n")
        TEXT("    box size=(1, 1, 1)\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestTrue(TEXT("a model with a shared header and a use parses"), bParsed);
    TestEqual(TEXT("the model keeps its shared format keyword"),
        Document.Header.FormatKeyword, FString(TEXT("pwmodel")));
    TestEqual(TEXT("the model keeps its version in the shared header"),
        Document.Header.Version, 0);
    TestEqual(TEXT("the model carries one document-level use"), Document.Uses.Num(), 1);
    TestEqual(TEXT("a clean shared-surface model has no diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReservedSurfaceSignpostTest,
    "PinWright.Model.ReservedSurface.ReservedBlockIsAKeywordSignpost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelReservedSurfaceSignpostTest::RunTest(const FString& Parameters)
{
    // The body intentionally contains another model construct. A parser that resumed
    // model parsing inside a reserved block would manufacture a part instead of keeping
    // the reserved construct as a signpost for its own file format.
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = PwModelReservedSurfaceTestPrivate::Parse(
        TEXT("pwmodel 0\n")
        TEXT("skeleton {\n")
        TEXT("    part must_not_escape {\n")
        TEXT("        box size=(1, 1, 1)\n")
        TEXT("    }\n")
        TEXT("}\n")
        // A real part outside the signpost keeps document validation from adding
        // PWMODEL_NO_PARTS and lets the assertion distinguish the two locations.
        TEXT("part body {\n")
        TEXT("    box size=(1, 1, 1)\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestTrue(TEXT("a balanced reserved block remains parseable"), bParsed);
    TestEqual(TEXT("the reserved block has no parser diagnostics"), Diagnostics.Num(), 0);
    TestEqual(TEXT("the body becomes one reserved-block signpost"),
        Document.ReservedBlocks.Num(), 1);
    if (Document.ReservedBlocks.Num() == 1)
    {
        TestEqual(TEXT("the reserved keyword is retained"),
            Document.ReservedBlocks[0].Keyword, FString(TEXT("skeleton")));
    }
    TestEqual(TEXT("reserved-block contents do not become an extra model part"),
        Document.Parts.Num(), 1);
    if (Document.Parts.Num() == 1)
    {
        TestEqual(TEXT("the outside part is the only model part"),
            Document.Parts[0].Name, FString(TEXT("body")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReservedSurfaceUsePathTest,
    "PinWright.Model.ReservedSurface.UseRetainsTheCorePathShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelReservedSurfaceUsePathTest::RunTest(const FString& Parameters)
{
    const FString ExactPath = TEXT("/Game/Rigs/../Hero.Hero");
    const FString Source = FString::Printf(
        TEXT("pwmodel 0\nuse skeleton from \"%s\"\npart body {\nbox size=(1, 1, 1)\n}\n"),
        *ExactPath);

    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = PwModelReservedSurfaceTestPrivate::Parse(
        *Source, Document, Diagnostics);

    TestTrue(TEXT("a use with an asset-shaped path parses"), bParsed);
    TestEqual(TEXT("the model keeps one use"), Document.Uses.Num(), 1);
    if (Document.Uses.Num() == 1)
    {
        TestEqual(TEXT("the model keeps the exact core path, without normalising /Game or .."),
            Document.Uses[0].Path, ExactPath);
        TestEqual(TEXT("the model keeps the use kind from the core"),
            Document.Uses[0].Kind, FString(TEXT("skeleton")));
    }
    TestEqual(TEXT("the exact-path fixture has no diagnostics"), Diagnostics.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReservedSurfaceSourceCodesTest,
    "PinWright.Model.ReservedSurface.SharedCodesStayInTheSourceCore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelReservedSurfaceSourceCodesTest::RunTest(const FString& Parameters)
{
    const FString PluginDir = PwModelReservedSurfaceTestPrivate::ResolvePluginDir();
    if (!TestFalse(TEXT("resolved the PinWright plugin directory"), PluginDir.IsEmpty()))
    {
        return false;
    }

    const FString SourceDir = PluginDir / TEXT("Source") / TEXT("PinWright")
        / TEXT("Private") / TEXT("PwSource");
    if (!TestTrue(TEXT("the shared source-core directory exists"),
            IFileManager::Get().DirectoryExists(*SourceDir)))
    {
        return false;
    }

    TArray<FString> SourceFiles;
    IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceDir, TEXT("*.*"),
        /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/true);
    if (!TestTrue(TEXT("the source-core scan found files"), SourceFiles.Num() > 0))
    {
        return false;
    }

    const FRegexPattern CodePattern(TEXT("(PWSRC_[A-Z][A-Z0-9_]*)"));
    const FRegexPattern LegacyPattern(TEXT("(PWMODEL_[A-Z][A-Z0-9_]*)"));
    TSet<FString> Registered;
    TSet<FString> Emitted;
    TSet<FString> Legacy;
    int32 FilesRead = 0;

    for (const FString& File : SourceFiles)
    {
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *File))
        {
            continue;
        }
        ++FilesRead;

        for (const FString& Code : PwModelReservedSurfaceTestPrivate::CollectCodes(
                 Contents, LegacyPattern))
        {
            Legacy.Add(Code);
        }

        if (FPaths::GetCleanFilename(File).Equals(TEXT("PwDiagnostic.h"),
                ESearchCase::CaseSensitive))
        {
            for (const FString& Code : PwModelReservedSurfaceTestPrivate::CollectCodes(
                     Contents, CodePattern))
            {
                Registered.Add(Code);
            }
        }
        else if (FPaths::GetExtension(File).Equals(TEXT("cpp"), ESearchCase::IgnoreCase))
        {
            for (const FString& Code : PwModelReservedSurfaceTestPrivate::CollectCodes(
                     Contents, CodePattern))
            {
                Emitted.Add(Code);
            }
        }
    }

    TestEqual(TEXT("all discovered source-core files were readable"), FilesRead, SourceFiles.Num());
    if (!TestTrue(TEXT("the source diagnostic header declares shared codes"), Registered.Num() > 0))
    {
        return false;
    }
    if (!TestTrue(TEXT("source-core implementation files emit shared codes"), Emitted.Num() > 0))
    {
        return false;
    }
    TestEqual(TEXT("the source core contains no legacy model diagnostic token"), Legacy.Num(), 0);

    TArray<FString> Unregistered;
    for (const FString& Code : Emitted)
    {
        if (!Registered.Contains(Code))
        {
            Unregistered.Add(Code);
        }
    }
    Unregistered.Sort();
    for (const FString& Code : Unregistered)
    {
        AddError(FString::Printf(TEXT("source-core implementation emits unregistered code '%s'"), *Code));
    }
    TestEqual(TEXT("every emitted source-core code is in the shared registry"),
        Unregistered.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReservedSurfaceVocabularyTest,
    "PinWright.Model.ReservedSurface.ReservedVocabularyHasNoPrefixCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelReservedSurfaceVocabularyTest::RunTest(const FString& Parameters)
{
    // Exact keyword matching is important here: a StartsWith/contains check would
    // silently turn a future operation such as `skeletonized` into the reserved
    // skeleton signpost and hide the author's real typo.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = PwModelReservedSurfaceTestPrivate::Parse(
            TEXT("pwmodel 0\n")
            TEXT("skeletonized {\n}\n"),
            Document, Diagnostics);

        TestFalse(TEXT("a near-miss reserved keyword is rejected"), bParsed);
        TestEqual(TEXT("a near-miss does not create a reserved block"),
            Document.ReservedBlocks.Num(), 0);
        TestNotNull(TEXT("the near-miss produces a real diagnostic"),
            PwModelReservedSurfaceTestPrivate::FindDiagnostic(
                Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN));
    }

    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = PwModelReservedSurfaceTestPrivate::Parse(
            TEXT("pwmodel 0\n")
            TEXT("skeleton { }\n")
            TEXT("skin {\n")
            TEXT("    smooth max_influences=4 stiffness=0.2 method=direct_distance voxel_resolution=128\n")
            TEXT("}\n")
            TEXT("animation walk { }\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n"),
            Document, Diagnostics);

        TestTrue(TEXT("the exact signpost vocabulary remains accepted"), bParsed);
        TestEqual(TEXT("skeleton and animation each create one signpost"),
            Document.ReservedBlocks.Num(), 2);
        if (Document.ReservedBlocks.Num() == 2)
        {
            TestEqual(TEXT("the first signpost is skeleton"),
                Document.ReservedBlocks[0].Keyword, FString(TEXT("skeleton")));
            TestEqual(TEXT("the second signpost is animation"),
                Document.ReservedBlocks[1].Keyword, FString(TEXT("animation")));
        }
        TestTrue(TEXT("skin is a real model-level construct, not a signpost"),
            Document.Skin.IsSet());
        TestEqual(TEXT("the exact vocabulary has no prefix collision with a near miss"),
            Diagnostics.Num(), 0);
    }
    return true;
}
