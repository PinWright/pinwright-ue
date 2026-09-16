// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract test for the shared PinWright source-diagnostic catalog in
// docs/pwmodel-format.md. The catalog spans the shared PwSource registry and
// the .pwmodel registry, so a new source format cannot silently introduce a
// code that is absent from the published table.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

namespace
{
    const TCHAR* PwDiagnosticCatalog_CodePattern()
    {
        return TEXT("((PWMODEL|PWSRC)_[A-Z][A-Z0-9_]*)");
    }

    FString PwModelCatalog_ResolvePluginDir()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
    }

    TArray<FString> PwModelCatalog_ResolveModuleDirs(const FString& PluginDir)
    {
        TArray<FString> ModuleDirs;
        if (PluginDir.IsEmpty())
        {
            return ModuleDirs;
        }

        const FString SourceDir = PluginDir / TEXT("Source");
        IFileManager::Get().FindFiles(ModuleDirs,
            *(SourceDir / TEXT("PinWright*")), /*Files=*/false, /*Directories=*/true);
        ModuleDirs.Sort();
        return ModuleDirs;
    }

    // Every module directory that ships .pwmodel pipeline source. Discovered,
    // not listed, so a second geometry module is covered when it lands.
    TArray<FString> PwModelCatalog_ResolveModelSourceDirs(const FString& PluginDir)
    {
        TArray<FString> Roots;
        for (const FString& ModuleDir : PwModelCatalog_ResolveModuleDirs(PluginDir))
        {
            const FString ModelDir = PluginDir / TEXT("Source") / ModuleDir
                / TEXT("Private") / TEXT("Model");
            if (IFileManager::Get().DirectoryExists(*ModelDir))
            {
                Roots.Add(ModelDir);
            }
        }
        return Roots;
    }

    // The shared source core is a separate implementation root. Keeping it in
    // the same discovery contract is what makes lexical PWSRC_* emissions visible.
    TArray<FString> PwModelCatalog_ResolveSourceCoreDirs(const FString& PluginDir)
    {
        TArray<FString> Roots;
        for (const FString& ModuleDir : PwModelCatalog_ResolveModuleDirs(PluginDir))
        {
            const FString SourceCoreDir = PluginDir / TEXT("Source") / ModuleDir
                / TEXT("Private") / TEXT("PwSource");
            if (IFileManager::Get().DirectoryExists(*SourceCoreDir))
            {
                Roots.Add(SourceCoreDir);
            }
        }
        return Roots;
    }

    TArray<FString> PwModelCatalog_ResolveRegistryHeaders(
        const TArray<FString>& ModelDirs, const TArray<FString>& SourceCoreDirs)
    {
        TArray<FString> Headers;
        for (const FString& Dir : SourceCoreDirs)
        {
            const FString Candidate = Dir / TEXT("PwDiagnostic.h");
            if (IFileManager::Get().FileExists(*Candidate))
            {
                Headers.Add(Candidate);
            }
        }
        for (const FString& Dir : ModelDirs)
        {
            const FString Candidate = Dir / TEXT("PwModelDiagnostic.h");
            if (IFileManager::Get().FileExists(*Candidate))
            {
                Headers.Add(Candidate);
            }
        }
        return Headers;
    }

    TSet<FString> PwModelCatalog_CollectRegistered(const TArray<FString>& HeaderPaths)
    {
        TSet<FString> Registered;
        const FRegexPattern Pattern(
            TEXT("((PWMODEL|PWSRC)_[A-Z][A-Z0-9_]*)\\s*\\[\\s*\\]\\s*=\\s*TEXT\\("));
        for (const FString& HeaderPath : HeaderPaths)
        {
            FString Contents;
            if (!FFileHelper::LoadFileToString(Contents, *HeaderPath))
            {
                continue;
            }
            FRegexMatcher Matcher(Pattern, Contents);
            while (Matcher.FindNext())
            {
                Registered.Add(Matcher.GetCaptureGroup(1));
            }
        }
        return Registered;
    }

    TSet<FString> PwModelCatalog_CollectEmitted(const TArray<FString>& PipelineDirs)
    {
        TSet<FString> Emitted;
        TArray<FString> SourceFiles;
        for (const FString& Dir : PipelineDirs)
        {
            IFileManager::Get().FindFilesRecursive(
                SourceFiles, *Dir, TEXT("*.cpp"), true, false, false);
        }

        const FRegexPattern Pattern(PwDiagnosticCatalog_CodePattern());
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
                Emitted.Add(Matcher.GetCaptureGroup(1));
            }
        }
        return Emitted;
    }

    TSet<FString> PwModelCatalog_CollectDocumented(const FString& Contents)
    {
        TSet<FString> Documented;
        TArray<FString> Lines;
        Contents.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

        const FRegexPattern RowPattern(
            TEXT("^\\|\\s*`((PWMODEL|PWSRC)_[A-Z][A-Z0-9_]*)`\\s*\\|"));
        for (const FString& Line : Lines)
        {
            FRegexMatcher Matcher(RowPattern, Line);
            if (Matcher.FindNext())
            {
                Documented.Add(Matcher.GetCaptureGroup(1));
            }
        }
        return Documented;
    }

    TSet<FString> PwModelCatalog_CollectMentioned(const FString& Contents)
    {
        TSet<FString> Mentioned;
        const FRegexPattern Pattern(PwDiagnosticCatalog_CodePattern());
        FRegexMatcher Matcher(Pattern, Contents);
        while (Matcher.FindNext())
        {
            Mentioned.Add(Matcher.GetCaptureGroup(1));
        }
        return Mentioned;
    }

    TArray<FString> PwModelCatalog_SortedDifference(
        const TSet<FString>& Left, const TSet<FString>& Right)
    {
        TArray<FString> Only = Left.Difference(Right).Array();
        Only.Sort();
        return Only;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelDiagnosticCatalogMatchesSourceTest,
    "PinWright.core.pwmodel_diagnostics.DocumentedCodesMatchEmittedCodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelDiagnosticCatalogMatchesSourceTest::RunTest(const FString& Parameters)
{
    const FString PluginDir = PwModelCatalog_ResolvePluginDir();
    if (!TestFalse(TEXT("Resolved the PinWright plugin directory"), PluginDir.IsEmpty()))
    {
        return false;
    }

    const TArray<FString> ModelDirs = PwModelCatalog_ResolveModelSourceDirs(PluginDir);
    const TArray<FString> SourceCoreDirs = PwModelCatalog_ResolveSourceCoreDirs(PluginDir);
    if (!TestTrue(TEXT("Discovered at least one .pwmodel pipeline source directory"),
            ModelDirs.Num() > 0))
    {
        return false;
    }
    if (!TestTrue(TEXT("Discovered at least one shared source-core directory"),
            SourceCoreDirs.Num() > 0))
    {
        return false;
    }

    const TArray<FString> RegistryHeaders =
        PwModelCatalog_ResolveRegistryHeaders(ModelDirs, SourceCoreDirs);
    if (!TestTrue(TEXT("Found both shared and model diagnostic registries"),
            RegistryHeaders.Num() >= 2))
    {
        return false;
    }

    const FString DocPath = PluginDir / TEXT("docs") / TEXT("pwmodel-format.md");
    FString DocContents;
    if (!TestTrue(TEXT("Read docs/pwmodel-format.md"),
            FFileHelper::LoadFileToString(DocContents, *DocPath)))
    {
        return false;
    }

    TArray<FString> PipelineDirs = SourceCoreDirs;
    PipelineDirs.Append(ModelDirs);
    const TSet<FString> Registered =
        PwModelCatalog_CollectRegistered(RegistryHeaders);
    const TSet<FString> Emitted =
        PwModelCatalog_CollectEmitted(PipelineDirs);
    const TSet<FString> Documented = PwModelCatalog_CollectDocumented(DocContents);
    const TSet<FString> Mentioned = PwModelCatalog_CollectMentioned(DocContents);

    // Guards the scans themselves: an input pattern or root that stops matching
    // must not turn every comparison below into a vacuous pass.
    if (!TestTrue(TEXT("The combined registries declare at least one code"),
            Registered.Num() > 0)
        || !TestTrue(TEXT("The combined pipeline roots emit at least one code"),
            Emitted.Num() > 0)
        || !TestTrue(TEXT("The diagnostics table carries at least one code row"),
            Documented.Num() > 0)
        || !TestTrue(TEXT("The shared registry contributes a PWSRC code"),
            Registered.Contains(TEXT("PWSRC_BAD_VALUE")))
        || !TestTrue(TEXT("The shared source core emits a PWSRC code"),
            Emitted.Contains(TEXT("PWSRC_BAD_VALUE")))
        || !TestTrue(TEXT("The diagnostics table documents a PWSRC code"),
            Documented.Contains(TEXT("PWSRC_BAD_VALUE")))
    )
    {
        return false;
    }

    int32 Mismatches = 0;
    for (const FString& Code : PwModelCatalog_SortedDifference(Emitted, Documented))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("'%s' is emitted by a PinWright source pipeline but has no row in "
                 "docs/pwmodel-format.md."), *Code));
    }
    for (const FString& Code : PwModelCatalog_SortedDifference(Documented, Emitted))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("docs/pwmodel-format.md documents '%s' but no discovered source "
                 "pipeline emits it."), *Code));
    }
    for (const FString& Code : PwModelCatalog_SortedDifference(Registered, Emitted))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("'%s' is registered but no discovered source pipeline emits it."), *Code));
    }
    for (const FString& Code : PwModelCatalog_SortedDifference(Emitted, Registered))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("'%s' is emitted but is absent from the shared or model registry."), *Code));
    }
    for (const FString& Code : PwModelCatalog_SortedDifference(Mentioned, Registered))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("docs/pwmodel-format.md names unregistered code '%s'."), *Code));
    }

    return TestEqual(
        TEXT("The shared/model registries, emit sites and diagnostics table agree"),
        Mismatches, 0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSourceSharedCodesAreNotFormatPrefixedTest,
    "PinWright.core.pwmodel_diagnostics.SharedCodesAreNotFormatPrefixed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwSourceSharedCodesAreNotFormatPrefixedTest::RunTest(const FString& Parameters)
{
    const FString PluginDir = PwModelCatalog_ResolvePluginDir();
    if (!TestFalse(TEXT("Resolved the PinWright plugin directory"), PluginDir.IsEmpty()))
    {
        return false;
    }

    const TArray<FString> SourceCoreDirs = PwModelCatalog_ResolveSourceCoreDirs(PluginDir);
    if (!TestTrue(TEXT("Discovered the shared source-core directory"),
            SourceCoreDirs.Num() > 0))
    {
        return false;
    }

    TArray<FString> SourceFiles;
    for (const FString& Dir : SourceCoreDirs)
    {
        IFileManager::Get().FindFilesRecursive(
            SourceFiles, *Dir, TEXT("*.*"), true, false, false);
    }

    const FRegexPattern LegacyPattern(TEXT("(PWMODEL_[A-Z][A-Z0-9_]*)"));
    const FRegexPattern SharedPattern(TEXT("(PWSRC_[A-Z][A-Z0-9_]*)"));
    TSet<FString> Legacy;
    TSet<FString> Emitted;
    for (const FString& File : SourceFiles)
    {
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *File))
        {
            continue;
        }

        FRegexMatcher LegacyMatcher(LegacyPattern, Contents);
        while (LegacyMatcher.FindNext())
        {
            Legacy.Add(LegacyMatcher.GetCaptureGroup(1));
        }

        if (FPaths::GetExtension(File).Equals(TEXT("cpp"), ESearchCase::IgnoreCase))
        {
            FRegexMatcher SharedMatcher(SharedPattern, Contents);
            while (SharedMatcher.FindNext())
            {
                Emitted.Add(SharedMatcher.GetCaptureGroup(1));
            }
        }
    }

    for (const FString& Code : Legacy)
    {
        AddError(FString::Printf(
            TEXT("Shared source-core code still carries the model prefix: '%s'."), *Code));
    }

    const bool bNoLegacy = TestEqual(
        TEXT("The shared source core contains no PWMODEL_ diagnostic token"), Legacy.Num(), 0);
    const bool bHasSharedEmission = TestTrue(
        TEXT("The shared source core emits at least one PWSRC_ diagnostic"), Emitted.Num() > 0);
    return bNoLegacy && bHasSharedEmission;
}
