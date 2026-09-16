// Copyright (c) 2026 Alexander Penkin. MIT License.

// Catalog contract for the .pwskel-specific diagnostics.  Shared lexical/source-shape codes
// remain in the PWSRC catalog; this test covers the structural PWSKEL set and keeps the format
// page, registry, and actual emit sites in lockstep.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

namespace PinWrightSkelDiagnosticCatalogTests
{
    TSet<FString> CollectRegistered(const FString& HeaderPath)
    {
        TSet<FString> Codes;
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *HeaderPath))
        {
            return Codes;
        }

        const FRegexPattern Pattern(
            TEXT("(PWSKEL_[A-Z][A-Z0-9_]*)\\s*\\[\\s*\\]\\s*=\\s*TEXT\\("));
        FRegexMatcher Matcher(Pattern, Contents);
        while (Matcher.FindNext())
        {
            Codes.Add(Matcher.GetCaptureGroup(1));
        }
        return Codes;
    }

    TSet<FString> CollectEmitted(const FString& SourceDir)
    {
        TSet<FString> Codes;
        TArray<FString> SourceFiles;
        IFileManager::Get().FindFilesRecursive(
            SourceFiles, *SourceDir, TEXT("*.cpp"), true, false, false);

        const FRegexPattern Pattern(TEXT("(PWSKEL_[A-Z][A-Z0-9_]*)"));
        for (const FString& File : SourceFiles)
        {
            FString Contents;
            if (!FFileHelper::LoadFileToString(Contents, *File))
            {
                continue;
            }

            FRegexMatcher Matcher(Pattern, Contents);
            while (Matcher.FindNext())
            {
                Codes.Add(Matcher.GetCaptureGroup(1));
            }
        }
        return Codes;
    }

    TSet<FString> CollectDocumented(const FString& DocPath)
    {
        TSet<FString> Codes;
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *DocPath))
        {
            return Codes;
        }

        TArray<FString> Lines;
        Contents.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
        const FRegexPattern Pattern(
            TEXT("^\\|\\s*`(PWSKEL_[A-Z][A-Z0-9_]*)`\\s*\\|"));
        for (const FString& Line : Lines)
        {
            FRegexMatcher Matcher(Pattern, Line);
            if (Matcher.FindNext())
            {
                Codes.Add(Matcher.GetCaptureGroup(1));
            }
        }
        return Codes;
    }

    TArray<FString> SortedDifference(const TSet<FString>& Left, const TSet<FString>& Right)
    {
        TArray<FString> Difference = Left.Difference(Right).Array();
        Difference.Sort();
        return Difference;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelDiagnosticCatalogTest,
    "PinWright.core.pwskel_diagnostics.DocumentedCodesMatchEmittedCodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSkelDiagnosticCatalogTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("Resolved the PinWright plugin directory"), Plugin.IsValid()))
    {
        return false;
    }

    const FString PluginDir = Plugin->GetBaseDir();
    const FString SourceDir = PluginDir / TEXT("Source/PinWright/Private/PwSkel");
    const FString RegistryPath = SourceDir / TEXT("PwSkelDiagnostic.h");
    const FString DocPath = PluginDir / TEXT("docs/pwskel-format.md");

    const TSet<FString> Registered =
        PinWrightSkelDiagnosticCatalogTests::CollectRegistered(RegistryPath);
    const TSet<FString> Emitted =
        PinWrightSkelDiagnosticCatalogTests::CollectEmitted(SourceDir);
    const TSet<FString> Documented =
        PinWrightSkelDiagnosticCatalogTests::CollectDocumented(DocPath);

    if (!TestTrue(TEXT("The skeleton registry declares at least one code"), Registered.Num() > 0)
        || !TestTrue(TEXT("The skeleton source emits at least one code"), Emitted.Num() > 0)
        || !TestTrue(TEXT("The skeleton format page documents at least one code"), Documented.Num() > 0))
    {
        return false;
    }

    int32 Mismatches = 0;
    for (const FString& Code : PinWrightSkelDiagnosticCatalogTests::SortedDifference(Emitted, Documented))
    {
        ++Mismatches;
        AddError(FString::Printf(TEXT("'%s' is emitted by .pwskel but has no documentation row."), *Code));
    }
    for (const FString& Code : PinWrightSkelDiagnosticCatalogTests::SortedDifference(Documented, Emitted))
    {
        ++Mismatches;
        AddError(FString::Printf(TEXT("docs/pwskel-format.md documents '%s' but .pwskel does not emit it."), *Code));
    }
    for (const FString& Code : PinWrightSkelDiagnosticCatalogTests::SortedDifference(Registered, Emitted))
    {
        ++Mismatches;
        AddError(FString::Printf(TEXT("'%s' is registered but .pwskel does not emit it."), *Code));
    }
    for (const FString& Code : PinWrightSkelDiagnosticCatalogTests::SortedDifference(Emitted, Registered))
    {
        ++Mismatches;
        AddError(FString::Printf(TEXT("'%s' is emitted but is absent from PwSkelDiagnostic.h."), *Code));
    }

    return TestEqual(TEXT("The .pwskel registry, emit sites, and catalog agree"), Mismatches, 0);
}
