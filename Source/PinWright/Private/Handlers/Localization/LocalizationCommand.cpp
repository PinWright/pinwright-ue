// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Localization/LocalizationCommand.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace PinWrightLocalization
{
    namespace
    {
        bool IsSafeTargetChar(const TCHAR Character)
        {
            return FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-');
        }

        bool HasPathTraversal(const FString& Path)
        {
            TArray<FString> Segments;
            Path.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/true);
            return Segments.Contains(TEXT(".."));
        }

        FString NormalizeSeparators(const FString& InPath)
        {
            FString Normalized = InPath;
            Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
            Normalized.TrimStartAndEndInline();
            return Normalized;
        }

        bool IsWithinProject(const FString& ProjectDir, const FString& FullPath)
        {
            FString ProjectRoot = FPaths::ConvertRelativePathToFull(ProjectDir);
            FPaths::NormalizeDirectoryName(ProjectRoot);
            if (!ProjectRoot.EndsWith(TEXT("/")))
            {
                ProjectRoot += TEXT("/");
            }

            FString NormalizedFull = FPaths::ConvertRelativePathToFull(FullPath);
            FPaths::NormalizeFilename(NormalizedFull);
            return NormalizedFull.StartsWith(ProjectRoot, ESearchCase::IgnoreCase);
        }
    }

    bool ValidateTarget(const FString& InTarget, FString& OutTarget, FString& OutError)
    {
        OutTarget = InTarget;
        OutTarget.TrimStartAndEndInline();
        OutError.Empty();

        if (OutTarget.IsEmpty())
        {
            OutError = TEXT("target must be a non-empty localization target name");
            return false;
        }
        if (OutTarget.Len() > 64)
        {
            OutError = TEXT("target must be at most 64 characters");
            return false;
        }

        for (const TCHAR Character : OutTarget)
        {
            if (!IsSafeTargetChar(Character))
            {
                OutError = TEXT("target may contain only letters, numbers, '-' and '_'");
                return false;
            }
        }
        return true;
    }

    bool ResolveConfig(const FString& ProjectDir, const FString& Target,
        EOperation Operation, const FString& ConfigInput,
        FString& OutRelativePath, FString& OutFullPath, FString& OutError)
    {
        OutRelativePath.Empty();
        OutFullPath.Empty();
        OutError.Empty();

        FString ValidatedTarget;
        if (!ValidateTarget(Target, ValidatedTarget, OutError))
        {
            return false;
        }

        FString RelativePath = NormalizeSeparators(ConfigInput);
        if (RelativePath.IsEmpty())
        {
            RelativePath = FString::Printf(TEXT("Config/Localization/%s_%s.ini"),
                *ValidatedTarget, OperationName(Operation));
        }

        // Config is intentionally constrained to the project's localization
        // config directory. This keeps the RPC from becoming an arbitrary
        // commandlet launcher or a path traversal primitive.
        if (RelativePath.StartsWith(TEXT("/")) || RelativePath.Contains(TEXT(":")) ||
            !FPaths::IsRelative(RelativePath) || HasPathTraversal(RelativePath))
        {
            OutError = TEXT("config must be a project-relative path without '..' or a drive/UNC prefix");
            return false;
        }

        FPaths::CollapseRelativeDirectories(RelativePath);
        if (!RelativePath.StartsWith(TEXT("Config/Localization/"), ESearchCase::IgnoreCase))
        {
            OutError = TEXT("config must be under Config/Localization");
            return false;
        }
        if (!FPaths::GetExtension(RelativePath).Equals(TEXT("ini"), ESearchCase::IgnoreCase))
        {
            OutError = TEXT("config must have an .ini extension");
            return false;
        }

        const FString FullPath = FPaths::ConvertRelativePathToFull(ProjectDir / RelativePath);
        if (!IsWithinProject(ProjectDir, FullPath))
        {
            OutError = TEXT("config resolved outside the project directory");
            return false;
        }
        if (!FPaths::FileExists(FullPath))
        {
            OutError = FString::Printf(TEXT("config file does not exist: %s"), *RelativePath);
            return false;
        }

        OutRelativePath = RelativePath;
        OutFullPath = FullPath;
        return true;
    }

    EConfigContentResult ValidateConfigContents(const FString& ConfigText,
        const FString& Target, const EOperation Operation)
    {
        const TCHAR* ExpectedMarker = ExpectedCommandletClass(Operation);
        bool bHasExpectedCommandlet = false;
        bool bHasTargetManifest = false;
        TArray<FString> ConfigLines;
        ConfigText.ParseIntoArrayLines(ConfigLines, /*CullEmpty=*/false);
        for (const FString& ConfigLine : ConfigLines)
        {
            const FString TrimmedLine = ConfigLine.TrimStartAndEnd();
            if (TrimmedLine.StartsWith(TEXT("CommandletClass="), ESearchCase::IgnoreCase) &&
                TrimmedLine.Contains(ExpectedMarker, ESearchCase::IgnoreCase))
            {
                bHasExpectedCommandlet = true;
            }
            if (TrimmedLine.StartsWith(TEXT("ManifestName="), ESearchCase::IgnoreCase))
            {
                // 13 == len("ManifestName=").
                FString ManifestName = TrimmedLine.Mid(13).TrimStartAndEnd();
                bHasTargetManifest = FPaths::GetBaseFilename(ManifestName)
                    .Equals(Target, ESearchCase::IgnoreCase);
            }
        }

        if (!bHasExpectedCommandlet)
        {
            return EConfigContentResult::OperationMismatch;
        }
        if (!bHasTargetManifest)
        {
            return EConfigContentResult::TargetMismatch;
        }
        return EConfigContentResult::Valid;
    }

    const TCHAR* ExpectedCommandletClass(const EOperation Operation)
    {
        return Operation == EOperation::Gather
            ? TEXT("GatherTextFrom")
            : TEXT("GenerateTextLocalizationResource");
    }

    const TCHAR* OperationName(const EOperation Operation)
    {
        return Operation == EOperation::Gather ? TEXT("Gather") : TEXT("Compile");
    }
}

