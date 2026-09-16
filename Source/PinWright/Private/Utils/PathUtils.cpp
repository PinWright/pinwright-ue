// Copyright (c) 2026 Alexander Penkin. MIT License.

// Path sanitization and validation utilities for PinWright
#include "Utils/PathUtils.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PinWrightSubsystem.h"

// ASK THE ENGINE WHICH MOUNT POINT THE PATH NAMES, and treat "none" as the only failure.
//
// This used to short-circuit TRUE on Path.StartsWith("/Game"|"/Engine"|"/Script") before ever
// reaching the engine, which is a PREFIX test wearing a mount point's name: "/GameFoo/Bar",
// "/Enginexyz/A" and "/Scriptable/Junk" all answered TRUE while naming roots that are not
// mounted, and 20-odd callers trust this verdict. GetPackageMountPoint walks the registered
// mount tree and confirms the hit with FPathViews::IsParentPathOf (PackageName.cpp:1957-1963),
// which compares whole path SEGMENTS, so "GameFoo" is no longer "Game".
//
// IT MUST BE GetPackageMountPoint AND NOT FPackageName::IsValidLongPackageName, and that is the
// load-bearing choice here. IsValidLongPackageName runs IsValidTextForLongPackageName first
// (PackageName.cpp:1762), which rejects every character in INVALID_LONGPACKAGE_CHARACTERS - '.'
// and ':' among them (Core/Public/UObject/NameTypes.h). GetPackageMountPoint skips that text
// pass entirely and only answers the mount question, so an OBJECT path
// ("/Script/UMG.UserWidget"), a subobject path ("/Game/A/BP.BP:Comp") and a generated-class path
// still answer TRUE. Most of this predicate's real callers hand it exactly those shapes; the
// obvious IsValidLongPackageName rewrite would start refusing them.
//
// THE FLIP SIDE, STATED PLAINLY: skipping that text pass also skips the engine's "//" rule
// (PackageName.cpp:1702-1706), and FPathViews::IsParentPathOf deliberately strips duplicate
// separators that follow the parent (PathViews.cpp:468-473). So "/Game//X" answers TRUE here,
// exactly as it did before. THIS PREDICATE IS NOT THE "//" GUARD and must not be made into one:
// callers spell their fallback `if (!IsValidMountPoint(P)) P = "/Game" / P;`, so refusing
// "/Game//X" here would only make them compose "/Game/Game//X". The "//" guard is
// CanReachCreatePackageFatal below and the dispatch-boundary type gate.
bool IsValidMountPoint(const FString& Path)
{
    return !FPackageName::GetPackageMountPoint(Path).IsNone();
}

// Out of line, not inline in the header, on purpose: this is the single place a debugger
// breakpoint catches every path that would have ended the process. See PathUtils.h for why "//"
// is the whole rule and why it is safe on every input shape.
bool CanReachCreatePackageFatal(const FString& InPath)
{
    return InPath.Contains(TEXT("//"));
}

FString SanitizeIncomingJson(const FString& In)
{
    FString Out;
    Out.Reserve(In.Len());
    for (int32 i = 0; i < In.Len(); ++i)
    {
        const TCHAR C = In[i];
        if (C >= 32)
            Out.AppendChar(C);
    }
    return Out;
}

FString SanitizeProjectRelativePath(const FString& InPath)
{
    if (InPath.IsEmpty())
        return FString();

    FString CleanPath = InPath;

    // Reject Windows absolute paths early (contain drive letter colon)
    if (CleanPath.Len() >= 2 && CleanPath[1] == TEXT(':'))
    {
        UE_LOG(
            LogPinWrightSubsystem, Warning,
            TEXT("SanitizeProjectRelativePath: Rejected Windows absolute path: %s"),
            *InPath);
        return FString();
    }

    FPaths::NormalizeFilename(CleanPath);

    // CRITICAL: FPaths::NormalizeFilename converts / to \ on Windows
    // We need to convert back to forward slashes for UE asset paths
    CleanPath.ReplaceInline(TEXT("\\"), TEXT("/"));

    // Normalize double slashes (prevents the CreatePackage Fatal on paths like /Game//Test).
    // FPaths::RemoveDuplicateSlashes (Core/Private/Misc/Paths.cpp:1374) collapses every run in a
    // single in-place pass, including runs of three or more; the Replace loop it replaces
    // reallocated the whole string once per pass.
    FPaths::RemoveDuplicateSlashes(CleanPath);

    // Reject paths containing traversal
    if (CleanPath.Contains(TEXT("..")))
    {
        UE_LOG(
            LogPinWrightSubsystem, Warning,
            TEXT("SanitizeProjectRelativePath: Rejected path containing '..': %s"),
            *InPath);
        return FString();
    }

    // Ensure path starts with a slash
    if (!CleanPath.StartsWith(TEXT("/")))
    {
        CleanPath = TEXT("/") + CleanPath;
    }

    // Validate against registered mount points (covers /Game, /Engine, /Script, plugins, DLC, etc.)
    if (!IsValidMountPoint(CleanPath))
    {
        UE_LOG(
            LogPinWrightSubsystem, Warning,
            TEXT("SanitizeProjectRelativePath: Rejected path without valid mount point: %s"),
            *InPath);
        return FString();
    }

    return CleanPath;
}

// Lifted from AudioAuthoringHandler.cpp's file-local NormalizeAudioPath, which was the only one of
// the five per-cluster copies that collapsed an interior "//" (it delegates to
// SanitizeProjectRelativePath, which the anim and texture copies did not call).
//
// ITS "/Content" -> "/Game" REWRITE WAS DELETED HERE, and it must not be added back. It was
// unreachable: it ran only on a string that had already passed SanitizeProjectRelativePath, which
// returns empty unless IsValidMountPoint holds, and "/Content" is not a mount point. UE registers
// "/Config/", "/Engine/", "/Game/", "/Script/", "/Memory/" and "/Temp/" (PackageName.cpp:803-808)
// plus one root per plugin NAMED AFTER THE PLUGIN - a plugin's Content directory is mounted AS
// "/PluginName/", never as "/Content/". So the branch was dead in NormalizeAudioPath too, before
// this fold, and removing it changes no observable behaviour: "/Content/Audio/X" returned empty
// then and returns empty now. Had a plugin literally been named "Content", the branch would have
// CORRUPTED its valid paths into "/Game/...", so deleting it is strictly safer than keeping it.
//
// A caller that genuinely needs "/Content/..." accepted needs the rewrite ABOVE the sanitizer, not
// below it - which is exactly what SoundCueDumpBuilder::NormalizeSoundCuePath does, and why that
// one cannot fold into this function. See PathUtils.h.
FString NormalizeContentAssetPath(const FString& InPath)
{
    // SECURITY: First validate path for traversal attacks
    FString Sanitized = SanitizeProjectRelativePath(InPath);
    if (Sanitized.IsEmpty() && !InPath.IsEmpty())
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("NormalizeContentAssetPath: Rejected malicious path: %s"), *InPath);
        return FString();
    }

    FString Normalized = Sanitized;

    Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

    while (Normalized.EndsWith(TEXT("/")))
    {
        Normalized.LeftChopInline(1);
    }

    return Normalized;
}

FString SanitizeProjectFilePath(const FString& InPath)
{
    if (InPath.IsEmpty())
        return FString();

    FString CleanPath = InPath;

    // SECURITY: Reject Windows absolute paths (contain drive letter colon anywhere)
    // Use Contains() for robust detection - handles X:\, X:/, /X:\, and edge cases
    if (CleanPath.Contains(TEXT(":")))
    {
        UE_LOG(
            LogPinWrightSubsystem, Warning,
            TEXT("SanitizeProjectFilePath: Rejected Windows absolute path (contains ':'): %s"),
            *InPath);
        return FString();
    }

    FPaths::NormalizeFilename(CleanPath);

    // Convert backslashes to forward slashes
    CleanPath.ReplaceInline(TEXT("\\"), TEXT("/"));

    // Normalize double slashes (FPaths::RemoveDuplicateSlashes, Core/Private/Misc/Paths.cpp:1374:
    // one in-place pass, handles runs of three or more)
    FPaths::RemoveDuplicateSlashes(CleanPath);

    // Reject paths containing traversal (CRITICAL for security)
    if (CleanPath.Contains(TEXT("..")))
    {
        UE_LOG(
            LogPinWrightSubsystem, Warning,
            TEXT("SanitizeProjectFilePath: Rejected path containing '..': %s"),
            *InPath);
        return FString();
    }

    // Ensure path starts with a slash (project-relative)
    if (!CleanPath.StartsWith(TEXT("/")))
    {
        CleanPath = TEXT("/") + CleanPath;
    }

    // Reject empty filename
    if (CleanPath.Len() <= 1)
    {
        UE_LOG(
            LogPinWrightSubsystem, Warning,
            TEXT("SanitizeProjectFilePath: Rejected empty path"));
        return FString();
    }

    // All validation passed - the path is safe for file operations.
    // Unlike asset paths, file paths are permissive and allow any project-relative
    // location (/Temp, /Saved, /Config, etc.) as long as they don't escape the project.
    return CleanPath;
}

bool IsValidAssetPath(const FString& Path)
{
    return !Path.IsEmpty() &&
           Path.StartsWith(TEXT("/")) &&
           !Path.Contains(TEXT("..")) &&
           !Path.Contains(TEXT("//")) &&
           !Path.Contains(TEXT(":"));  // Reject Windows absolute paths
}

bool NormalizeToObjectPath(const FString& InPath, FString& OutObjectPath, FString& OutError)
{
    OutObjectPath.Reset();
    OutError.Reset();

    const FString Path = InPath.TrimStartAndEnd();
    if (Path.IsEmpty())
    {
        OutError = TEXT("The path is empty.");
        return false;
    }
    // A subobject path carries a ':' suffix ("/Game/A/BP_X.BP_X:Component"). That suffix is the
    // asset registry's business, not this helper's -- but IsValidAssetPath rejects EVERY ':' by
    // contract, because rejecting the colon is how it turns away a Windows drive letter. So split
    // the suffix off before validating and re-attach it after. A drive letter is still rejected:
    // splitting "C:/Game/A/SM_X" leaves "C", which has no leading slash.
    FString AssetPart = Path;
    FString SubObject;
    int32 ColonIndex = INDEX_NONE;
    if (Path.FindChar(TEXT(':'), ColonIndex))
    {
        AssetPart = Path.Left(ColonIndex);
        SubObject = Path.Mid(ColonIndex);  // keeps the ':' itself
        if (SubObject.Len() <= 1)
        {
            OutError = FString::Printf(
                TEXT("'%s' ends in ':' with no subobject name after it."), *Path);
            return false;
        }
    }

    if (!IsValidAssetPath(AssetPart))
    {
        OutError = FString::Printf(
            TEXT("'%s' is not a content path. Expected a mounted path such as ")
            TEXT("/Game/Folder/AssetName or /Game/Folder/AssetName.AssetName."), *Path);
        return false;
    }

    int32 LastSlash = INDEX_NONE;
    AssetPart.FindLastChar(TEXT('/'), LastSlash);
    // IsValidAssetPath already required a leading '/', so LastSlash is always found.
    const FString LeafName = AssetPart.Mid(LastSlash + 1);
    if (LeafName.IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("'%s' names a folder rather than an asset (it ends in '/')."), *Path);
        return false;
    }

    // A dot anywhere ABOVE the leaf is a malformed path, not an object name: the object name is
    // only ever the part after the LAST slash. Checking the leaf alone would accept
    // /Game/A.B/C and hand a broken path to the registry.
    if (AssetPart.Left(LastSlash).Contains(TEXT(".")))
    {
        OutError = FString::Printf(
            TEXT("'%s' has a '.' in a folder component. The object name follows the last '/'."),
            *Path);
        return false;
    }

    int32 DotIndex = INDEX_NONE;
    if (LeafName.FindChar(TEXT('.'), DotIndex))
    {
        // Already an object path (or a subobject path, whose ':' suffix is the registry's own
        // business). Only the emptiness of the object name is ours to reject.
        if (DotIndex == LeafName.Len() - 1)
        {
            OutError = FString::Printf(
                TEXT("'%s' ends in '.' with no object name after it."), *Path);
            return false;
        }
        if (DotIndex == 0)
        {
            OutError = FString::Printf(
                TEXT("'%s' has no package name before the '.'."), *Path);
            return false;
        }
        OutObjectPath = AssetPart + SubObject;
        return true;
    }

    // The package form. UE's own convention for the primary asset in a package is that the
    // object shares the package's short name, which is what every content path in this project
    // uses and what the asset registry indexes.
    OutObjectPath = AssetPart + TEXT(".") + LeafName + SubObject;
    return true;
}

FString SanitizeAssetName(const FString& InName)
{
    if (InName.IsEmpty())
    {
        return TEXT("Asset");
    }

    FString Trimmed = InName.TrimStartAndEnd();

    // SQL injection chars: ; ' " ` plus invalid UE asset name chars:
    // @ # % $ & * ( ) + = [ ] { } < > ? | \ / . : ~ ! and whitespace
    //
    // '/' and '.' are the two that were missing, and their absence made this function's name
    // false: SanitizeAssetName("Foo/Bar") returned "Foo/Bar", so a caller composing
    // "<folder>/<sanitized name>" got a folder separator inside its leaf. Both are in the
    // engine's own INVALID_OBJECTNAME_CHARACTERS (Core/Public/UObject/NameTypes.h:191).
    static const FString InvalidChars = TEXT(";'\"`@#%$&*()+=[]{}<>?|\\/.:~! ");

    // Single-pass: replace invalid chars with underscore, collapsing consecutive replacements
    FString Result;
    Result.Reserve(Trimmed.Len());
    TCHAR PrevChar = 0;
    for (int32 i = 0; i < Trimmed.Len(); ++i)
    {
        const TCHAR Ch = Trimmed[i];

        // Check for SQL "--" sequence (both chars become a single underscore)
        const bool bIsDoubleDash = (Ch == TEXT('-') && i + 1 < Trimmed.Len() && Trimmed[i + 1] == TEXT('-'));
        const bool bIsInvalid = InvalidChars.Contains(FString(1, &Ch));

        if (bIsInvalid || bIsDoubleDash)
        {
            // Collapse consecutive invalid chars into a single underscore
            if (PrevChar != TEXT('_'))
            {
                Result.AppendChar(TEXT('_'));
                PrevChar = TEXT('_');
            }
            // Skip the second dash of "--"
            if (bIsDoubleDash)
            {
                ++i;
            }
        }
        else
        {
            Result.AppendChar(Ch);
            PrevChar = Ch;
        }
    }

    // Strip leading/trailing underscores
    while (Result.Len() > 0 && Result[0] == TEXT('_'))
    {
        Result.RemoveAt(0);
    }
    while (Result.Len() > 0 && Result[Result.Len() - 1] == TEXT('_'))
    {
        Result.RemoveAt(Result.Len() - 1);
    }

    if (Result.IsEmpty())
    {
        return TEXT("Asset");
    }

    // Ensure name starts with a letter or underscore
    if (!FChar::IsAlpha(Result[0]) && Result[0] != TEXT('_'))
    {
        Result = TEXT("Asset_") + Result;
    }

    // Truncate to reasonable length (64 chars is UE max for asset names)
    if (Result.Len() > 64)
    {
        Result.LeftInline(64);
    }

    return Result;
}

bool ValidateAssetCreationPath(
    const FString& FolderPath,
    const FString& AssetName,
    FString& OutFullPath,
    FString& OutError)
{
    // Sanitize and validate folder path
    FString SanitizedFolder = SanitizeProjectRelativePath(FolderPath);
    if (SanitizedFolder.IsEmpty())
    {
        OutError = TEXT("Invalid folder path: contains traversal or invalid characters");
        return false;
    }

    // There was a "/Game" auto-prepend here, guarded by !IsValidMountPoint. It was dead:
    // SanitizeProjectRelativePath returns empty for exactly the inputs IsValidMountPoint rejects,
    // so the early return above had already fired. It was also a manufactured "//" waiting to
    // happen, because SanitizedFolder always starts with '/' by the time it would have run.

    // Sanitize asset name
    FString SanitizedName = SanitizeAssetName(AssetName);
    if (SanitizedName.IsEmpty())
    {
        OutError = TEXT("Invalid asset name after sanitization");
        return false;
    }

    // Build full path
    OutFullPath = SanitizedFolder / SanitizedName;

    // Final validation
    if (!IsValidAssetPath(OutFullPath))
    {
        OutError = FString::Printf(TEXT("Invalid asset path after normalization: %s"), *OutFullPath);
        return false;
    }

    return true;
}
