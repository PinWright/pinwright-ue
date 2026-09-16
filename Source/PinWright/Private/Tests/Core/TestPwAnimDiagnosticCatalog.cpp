// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract test for the .pwanim diagnostic catalog. It compares the format registry, the
// source emit sites and docs/pwanim-format.md in every direction, while keeping the shared
// PWSRC_* registry in the same comparison as the animation-specific PWANIM_* registry.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

namespace
{
const TCHAR* PwAnimCatalog_CodePattern()
{
    return TEXT("((PWANIM|PWSRC)_[A-Z][A-Z0-9_]*)");
}

FString PwAnimCatalog_ResolvePluginDir()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
}

TArray<FString> PwAnimCatalog_ResolveModuleDirs(const FString& PluginDir)
{
    TArray<FString> ModuleDirs;
    if (PluginDir.IsEmpty())
    {
        return ModuleDirs;
    }

    IFileManager::Get().FindFiles(ModuleDirs,
        *(PluginDir / TEXT("Source") / TEXT("PinWright*")), false, true);
    ModuleDirs.Sort();
    return ModuleDirs;
}

TArray<FString> PwAnimCatalog_ResolveAnimSourceDirs(const FString& PluginDir)
{
    TArray<FString> Roots;
    for (const FString& ModuleDir : PwAnimCatalog_ResolveModuleDirs(PluginDir))
    {
        const FString AnimDir = PluginDir / TEXT("Source") / ModuleDir
            / TEXT("Private") / TEXT("PwAnim");
        if (IFileManager::Get().DirectoryExists(*AnimDir))
        {
            Roots.Add(AnimDir);
        }
    }
    return Roots;
}

TArray<FString> PwAnimCatalog_ResolveSourceCoreDirs(const FString& PluginDir)
{
    TArray<FString> Roots;
    for (const FString& ModuleDir : PwAnimCatalog_ResolveModuleDirs(PluginDir))
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

TArray<FString> PwAnimCatalog_ResolveRegistryHeaders(
    const TArray<FString>& AnimDirs, const TArray<FString>& SourceCoreDirs)
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
    for (const FString& Dir : AnimDirs)
    {
        const FString Candidate = Dir / TEXT("PwAnimDiagnostic.h");
        if (IFileManager::Get().FileExists(*Candidate))
        {
            Headers.Add(Candidate);
        }
    }
    return Headers;
}

TSet<FString> PwAnimCatalog_CollectRegistered(const TArray<FString>& HeaderPaths)
{
    TSet<FString> Registered;
    const FRegexPattern Pattern(
        TEXT("((PWANIM|PWSRC)_[A-Z][A-Z0-9_]*)\\s*\\[\\s*\\]\\s*=\\s*TEXT\\("));
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

TSet<FString> PwAnimCatalog_CollectEmitted(const TArray<FString>& PipelineDirs)
{
    TSet<FString> Emitted;
    TArray<FString> SourceFiles;
    for (const FString& Dir : PipelineDirs)
    {
        IFileManager::Get().FindFilesRecursive(SourceFiles, *Dir, TEXT("*.cpp"), true, false, false);
    }

    const FRegexPattern Pattern(PwAnimCatalog_CodePattern());
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

TSet<FString> PwAnimCatalog_CollectDocumented(const FString& Contents)
{
    TSet<FString> Documented;
    TArray<FString> Lines;
    Contents.ParseIntoArrayLines(Lines, false);

    const FRegexPattern RowPattern(
        TEXT("^\\|\\s*`((PWANIM|PWSRC)_[A-Z][A-Z0-9_]*)`\\s*\\|"));
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

TSet<FString> PwAnimCatalog_CollectMentioned(const FString& Contents)
{
    TSet<FString> Mentioned;
    const FRegexPattern Pattern(PwAnimCatalog_CodePattern());
    FRegexMatcher Matcher(Pattern, Contents);
    while (Matcher.FindNext())
    {
        Mentioned.Add(Matcher.GetCaptureGroup(1));
    }
    return Mentioned;
}

TArray<FString> PwAnimCatalog_SortedDifference(
    const TSet<FString>& Left, const TSet<FString>& Right)
{
    TArray<FString> Only = Left.Difference(Right).Array();
    Only.Sort();
    return Only;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimDiagnosticCatalogMatchesSourceTest,
    "PinWright.core.pwanim_diagnostics.DocumentedCodesMatchEmittedCodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAnimDiagnosticCatalogMatchesSourceTest::RunTest(const FString& Parameters)
{
    const FString PluginDir = PwAnimCatalog_ResolvePluginDir();
    if (!TestFalse(TEXT("Resolved the PinWright plugin directory"), PluginDir.IsEmpty()))
    {
        return false;
    }

    const TArray<FString> AnimDirs = PwAnimCatalog_ResolveAnimSourceDirs(PluginDir);
    const TArray<FString> SourceCoreDirs = PwAnimCatalog_ResolveSourceCoreDirs(PluginDir);
    if (!TestTrue(TEXT("Discovered at least one .pwanim pipeline source directory"),
            AnimDirs.Num() > 0)
        || !TestTrue(TEXT("Discovered at least one shared source-core directory"),
            SourceCoreDirs.Num() > 0))
    {
        return false;
    }

    const TArray<FString> RegistryHeaders =
        PwAnimCatalog_ResolveRegistryHeaders(AnimDirs, SourceCoreDirs);
    if (!TestTrue(TEXT("Found both shared and animation diagnostic registries"),
            RegistryHeaders.Num() >= 2))
    {
        return false;
    }

    const FString DocPath = PluginDir / TEXT("docs") / TEXT("pwanim-format.md");
    FString DocContents;
    if (!TestTrue(TEXT("Read docs/pwanim-format.md"),
            FFileHelper::LoadFileToString(DocContents, *DocPath)))
    {
        return false;
    }

    TArray<FString> PipelineDirs = SourceCoreDirs;
    PipelineDirs.Append(AnimDirs);
    const TSet<FString> Registered = PwAnimCatalog_CollectRegistered(RegistryHeaders);
    const TSet<FString> Emitted = PwAnimCatalog_CollectEmitted(PipelineDirs);
    const TSet<FString> Documented = PwAnimCatalog_CollectDocumented(DocContents);
    const TSet<FString> Mentioned = PwAnimCatalog_CollectMentioned(DocContents);

    if (!TestTrue(TEXT("The combined registries declare at least one code"), Registered.Num() > 0)
        || !TestTrue(TEXT("The combined pipeline roots emit at least one code"), Emitted.Num() > 0)
        || !TestTrue(TEXT("The diagnostics table carries at least one code row"), Documented.Num() > 0)
        || !TestTrue(TEXT("The shared registry contributes PWSRC_BAD_VALUE"),
            Registered.Contains(TEXT("PWSRC_BAD_VALUE")))
        || !TestTrue(TEXT("The animation registry contributes PWANIM_WRONG_FORMAT"),
            Registered.Contains(TEXT("PWANIM_WRONG_FORMAT")))
        || !TestTrue(TEXT("The shared source core emits PWSRC_BAD_VALUE"),
            Emitted.Contains(TEXT("PWSRC_BAD_VALUE")))
        || !TestTrue(TEXT("The animation pipeline emits PWANIM_WRONG_FORMAT"),
            Emitted.Contains(TEXT("PWANIM_WRONG_FORMAT")))
        || !TestTrue(TEXT("The diagnostics table documents PWSRC_BAD_VALUE"),
            Documented.Contains(TEXT("PWSRC_BAD_VALUE")))
        || !TestTrue(TEXT("The diagnostics table documents PWANIM_WRONG_FORMAT"),
            Documented.Contains(TEXT("PWANIM_WRONG_FORMAT"))))
    {
        return false;
    }

    int32 Mismatches = 0;
    for (const FString& Code : PwAnimCatalog_SortedDifference(Emitted, Documented))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("'%s' is emitted by a PinWright source pipeline but has no row in "
                 "docs/pwanim-format.md."), *Code));
    }
    for (const FString& Code : PwAnimCatalog_SortedDifference(Documented, Emitted))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("docs/pwanim-format.md documents '%s' but no discovered source pipeline emits it."),
            *Code));
    }
    for (const FString& Code : PwAnimCatalog_SortedDifference(Registered, Emitted))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("'%s' is registered but no discovered source pipeline emits it."), *Code));
    }
    for (const FString& Code : PwAnimCatalog_SortedDifference(Emitted, Registered))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("'%s' is emitted but is absent from the shared or animation registry."), *Code));
    }
    for (const FString& Code : PwAnimCatalog_SortedDifference(Mentioned, Registered))
    {
        ++Mismatches;
        AddError(FString::Printf(
            TEXT("docs/pwanim-format.md names unregistered code '%s'."), *Code));
    }

    return TestEqual(
        TEXT("The shared/animation registries, emit sites and diagnostics table agree"),
        Mismatches, 0);
}
