// Copyright (c) 2026 Alexander Penkin. MIT License.

// Ratchet on blanket log suppression in tests (board B-tests-blanket-log-suppression).
//
// FAutomationTestBase::bSuppressLogErrors / bSuppressLogWarnings / bElevateLogWarningsToErrors
// switch off log checking for a WHOLE test, so the test also ignores every engine error it causes
// by accident - a verb or fixture defect a user would hit. The replacement is an exact
// AddExpectedErrorPlain / AddExpectedMessagePlain declaration of what the test deliberately
// provokes (docs/rpc-design.md s12). This test scans every test source under the plugin's Source/
// tree and fails when a file assigns one of those statics more often than its allowance below, so
// a new blanket setter cannot land silently. Tests/AutomationSuiteMaintenance.cpp is exempt: it is
// the per-test reset that restores the configured defaults.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

namespace BlanketLogSuppressionRatchet
{
    // Files that may keep a blanket setter, with the justification at the site. Empty: every
    // former site was converted to exact expectations or removed. An entry here must carry its
    // exact count; a count that drops below the allowance fails too, so the list cannot go stale.
    const TPair<const TCHAR*, int32> AllowedBlanketSetters[] = {
        {TEXT(""), 0},
    };

    const TCHAR* const ExemptFiles[] = {
        TEXT("Source/PinWright/Private/Tests/AutomationSuiteMaintenance.cpp"),
        TEXT("Source/PinWright/Private/Tests/Infra/TestBlanketLogSuppressionRatchet.cpp"),
    };

    int32 CountSetters(const FString& Contents)
    {
        int32 Count = 0;
        FRegexMatcher Matcher(FRegexPattern(
            TEXT("\\b(bSuppressLogErrors|bSuppressLogWarnings|bElevateLogWarningsToErrors)\\s*=[^=]")),
            Contents);
        while (Matcher.FindNext())
        {
            ++Count;
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlanketLogSuppressionRatchetTest,
    "PinWright.infra.contract.LogSuppression.NoUnlistedBlanketSetters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlanketLogSuppressionRatchetTest::RunTest(const FString& Parameters)
{
    using namespace BlanketLogSuppressionRatchet;

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolves"), Plugin.IsValid()))
    {
        return true;
    }
    const FString PluginRoot = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
    const FString SourceRoot = PluginRoot / TEXT("Source");

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *SourceRoot, TEXT("*.cpp"), true, false, false);
    IFileManager::Get().FindFilesRecursive(Files, *SourceRoot, TEXT("*.h"), true, false, false);

    TMap<FString, int32> Allowed;
    for (const TPair<const TCHAR*, int32>& Entry : AllowedBlanketSetters)
    {
        if (FCString::Strlen(Entry.Key) > 0)
        {
            Allowed.Add(Entry.Key, Entry.Value);
        }
    }

    int32 ScannedTestFiles = 0;
    for (const FString& File : Files)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(File);
        FPaths::MakePathRelativeTo(Relative, *(PluginRoot / TEXT("")));
        Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (!Relative.Contains(TEXT("/Tests/")))
        {
            continue;
        }
        bool bExempt = false;
        for (const TCHAR* Exempt : ExemptFiles)
        {
            bExempt |= Relative.Equals(Exempt, ESearchCase::IgnoreCase);
        }
        if (bExempt)
        {
            continue;
        }

        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *File))
        {
            AddError(FString::Printf(TEXT("could not read %s"), *Relative));
            continue;
        }
        ++ScannedTestFiles;

        const int32 Count = CountSetters(Contents);
        const int32 Allowance = Allowed.FindRef(Relative);
        if (Count > Allowance)
        {
            AddError(FString::Printf(
                TEXT("%s assigns a blanket log-suppression static %d time(s) (allowed %d). Declare the ")
                TEXT("exact provoked lines with AddExpectedErrorPlain/AddExpectedMessagePlain instead ")
                TEXT("(docs/rpc-design.md s12), or fix the defect that logs them."),
                *Relative, Count, Allowance));
        }
        else if (Count < Allowance)
        {
            AddError(FString::Printf(
                TEXT("%s now assigns a blanket log-suppression static %d time(s) but is allowed %d: ")
                TEXT("lower its entry in AllowedBlanketSetters."), *Relative, Count, Allowance));
        }
    }

    // A scan that found no test files measured nothing.
    TestTrue(FString::Printf(TEXT("scanned test sources (%d files)"), ScannedTestFiles),
        ScannedTestFiles > 100);
    return true;
}
