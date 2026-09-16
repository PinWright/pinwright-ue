// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Catalog/WikiOverlay.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"

namespace WikiOverlay
{
    namespace
    {
        // Per-file cache entry. Lines and SectionStartLines are derived from Body
        // and stay valid as long as Body stays put — see ParseInto for layout.
        struct FCachedOverlay
        {
            FDateTime FileTimeStamp = FDateTime::MinValue();
            FString Prelude;                                  // body before first `### `, with H1 + leading comments stripped
            TMap<FString, FString> Sections;                  // method full name → section body
        };

        // Cache keyed by absolute file path (case-insensitive on Windows; we lowercase the key).
        // Mutex is held for the entire load — concurrent callers serialize on cache misses.
        // Acceptable here because the wiki path is low-volume and overlay files are small.
        FCriticalSection& GetMutex()
        {
            static FCriticalSection Mutex;
            return Mutex;
        }

        TMap<FString, FCachedOverlay>& GetCache()
        {
            static TMap<FString, FCachedOverlay> Cache;
            return Cache;
        }

        FString OverlayFilePath(const FString& Namespace)
        {
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
            if (!Plugin.IsValid())
            {
                return FString();
            }
            return Plugin->GetBaseDir() / TEXT("docs") / TEXT("wiki-src") / (Namespace + TEXT(".md"));
        }

        // True if the line is the H1 header for this overlay (`# something`, but not `## ` / `### `).
        bool IsH1Line(const FString& Line)
        {
            return Line.StartsWith(TEXT("# ")) && !Line.StartsWith(TEXT("## "));
        }

        // True if the line opens a markdown HTML comment (`<!--`). We treat anything
        // up to the closing `-->` as part of the leading comment block.
        bool IsCommentOpenLine(const FString& Line)
        {
            const FString Trimmed = Line.TrimStart();
            return Trimmed.StartsWith(TEXT("<!--"));
        }

        bool LineContainsCommentClose(const FString& Line)
        {
            return Line.Contains(TEXT("-->"));
        }

        // Walk the file's lines once, populating Out.Prelude and Out.Sections.
        // Lines are split on \r\n / \n; trailing \r is stripped per line.
        void ParseInto(const FString& FileBody, FCachedOverlay& Out)
        {
            TArray<FString> Lines;
            FileBody.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

            // ---- Phase 1: locate where the prelude content actually starts.
            // Skip a single H1, leading blank lines, and any HTML comment block(s)
            // (multi-line, terminated by `-->`).
            int32 Idx = 0;
            bool bH1Skipped = false;
            while (Idx < Lines.Num())
            {
                const FString& L = Lines[Idx];
                if (!bH1Skipped && IsH1Line(L))
                {
                    bH1Skipped = true;
                    ++Idx;
                    continue;
                }
                if (L.TrimStartAndEnd().IsEmpty())
                {
                    ++Idx;
                    continue;
                }
                if (IsCommentOpenLine(L))
                {
                    // Consume lines through the closing `-->` (may be on the same line).
                    if (LineContainsCommentClose(L))
                    {
                        ++Idx;
                        continue;
                    }
                    ++Idx;
                    while (Idx < Lines.Num() && !LineContainsCommentClose(Lines[Idx]))
                    {
                        ++Idx;
                    }
                    if (Idx < Lines.Num()) ++Idx; // skip the closing-comment line
                    continue;
                }
                break; // first content line
            }

            // ---- Phase 2: collect prelude up to (not including) the first `### ` line.
            const int32 PreludeStart = Idx;
            while (Idx < Lines.Num() && !Lines[Idx].StartsWith(TEXT("### ")))
            {
                ++Idx;
            }
            // Build prelude body, then trim trailing whitespace/blank lines.
            {
                FString Prelude;
                for (int32 i = PreludeStart; i < Idx; ++i)
                {
                    Prelude += Lines[i];
                    Prelude += TEXT("\n");
                }
                Out.Prelude = Prelude.TrimStartAndEnd();
            }

            // ---- Phase 3: scan H3 sections. Heading line is `### <full method name>`
            // (case-sensitive on the method name). Section ends at next `### ` or EOF.
            while (Idx < Lines.Num())
            {
                const FString& Heading = Lines[Idx];
                if (!Heading.StartsWith(TEXT("### ")))
                {
                    ++Idx;
                    continue;
                }
                const FString MethodName = Heading.Mid(4).TrimStartAndEnd(); // strip "### "
                ++Idx;

                FString Body;
                while (Idx < Lines.Num() && !Lines[Idx].StartsWith(TEXT("### ")))
                {
                    Body += Lines[Idx];
                    Body += TEXT("\n");
                    ++Idx;
                }

                if (!MethodName.IsEmpty())
                {
                    Out.Sections.Add(MethodName, Body.TrimStartAndEnd());
                }
            }
        }

        // Return cached entry, refreshing if the file's mtime has changed.
        // Returns nullptr if the file does not exist (treat as "no overlay").
        // Caller must hold the cache mutex.
        const FCachedOverlay* GetOrLoad_NoLock(const FString& AbsPathLower, const FString& AbsPath)
        {
            IFileManager& FileMgr = IFileManager::Get();
            const FDateTime CurrentTimeStamp = FileMgr.GetTimeStamp(*AbsPath);
            if (CurrentTimeStamp == FDateTime::MinValue())
            {
                // File missing — drop any stale cache entry and return null.
                GetCache().Remove(AbsPathLower);
                return nullptr;
            }

            FCachedOverlay* Existing = GetCache().Find(AbsPathLower);
            if (Existing && Existing->FileTimeStamp == CurrentTimeStamp)
            {
                return Existing;
            }

            FString FileBody;
            if (!FFileHelper::LoadFileToString(FileBody, *AbsPath))
            {
                GetCache().Remove(AbsPathLower);
                return nullptr;
            }

            FCachedOverlay Fresh;
            Fresh.FileTimeStamp = CurrentTimeStamp;
            ParseInto(FileBody, Fresh);

            FCachedOverlay& Slot = GetCache().Add(AbsPathLower, MoveTemp(Fresh));
            return &Slot;
        }
    } // anonymous namespace

    FString LoadGroupPrelude(const FString& Namespace)
    {
        const FString AbsPath = OverlayFilePath(Namespace);
        if (AbsPath.IsEmpty()) return FString();
        const FString AbsLower = AbsPath.ToLower();

        FScopeLock Lock(&GetMutex());
        const FCachedOverlay* Entry = GetOrLoad_NoLock(AbsLower, AbsPath);
        if (!Entry) return FString();
        return Entry->Prelude;
    }

    FString LoadMethodSection(const FString& FullMethodName)
    {
        // Derive enclosing Category by stripping the trailing leaf segment.
        int32 LastDot = INDEX_NONE;
        if (!FullMethodName.FindLastChar(TEXT('.'), LastDot))
        {
            return FString(); // un-namespaced method names have no overlay file
        }
        const FString Category = FullMethodName.Left(LastDot);
        if (Category.IsEmpty()) return FString();

        const FString AbsPath = OverlayFilePath(Category);
        if (AbsPath.IsEmpty()) return FString();
        const FString AbsLower = AbsPath.ToLower();

        FScopeLock Lock(&GetMutex());
        const FCachedOverlay* Entry = GetOrLoad_NoLock(AbsLower, AbsPath);
        if (!Entry) return FString();

        const FString* Body = Entry->Sections.Find(FullMethodName);
        return Body ? *Body : FString();
    }
} // namespace WikiOverlay
