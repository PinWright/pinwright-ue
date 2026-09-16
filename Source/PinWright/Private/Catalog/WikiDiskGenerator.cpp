// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Catalog/WikiDiskGenerator.h"
#include "Catalog/WikiHandler.h"
#include "PinWrightProjectSettings.h"
#include "Utils/AtomicFileWriter.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogWikiDiskGenerator, Log, All);

namespace WikiDiskGenerator
{
    namespace
    {
        constexpr const TCHAR* OwnershipManifestFile = TEXT(".pinwright-wiki-manifest.json");
        constexpr const TCHAR* OwnershipManifestOwner = TEXT("PinWright.WikiDiskGenerator");
        constexpr int32 OwnershipManifestSchemaVersion = 1;
        constexpr const TCHAR* RegistryFile = TEXT("registry.json");
        constexpr const TCHAR* OutsideOwnedRootReason =
            TEXT("output directory is outside the canonical Project/Saved/PinWright/wiki root");

        // Bound untrusted ownership metadata before JSON parsing or entry iteration can consume
        // unbounded memory and startup time.
        constexpr int64 MaxOwnershipManifestBytes = 4 * 1024 * 1024;
        constexpr int32 MaxOwnershipManifestEntries = 8192;

        enum class EOwnershipManifestState : uint8
        {
            Missing,
            Valid,
            Invalid
        };

        struct FReconcileOperations
        {
            TFunction<bool(const FString&, TArrayView<const uint8>, FString&)> StageFile;
            TFunction<bool(const FString&, TArrayView<const uint8>, FString&)> WriteFile;
            TFunction<bool(const FString&)> DeleteFile;
        };

        struct FReconcileResult
        {
            bool bSucceeded = false;
            bool bPruneSkipped = false;
            FString PruneSkipReason;
        };

        FString WikiDiskGenerator_CanonicalOwnedOutputDirectory()
        {
            FString Dir = FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("wiki");
            Dir = FPaths::ConvertRelativePathToFull(Dir);
            FPaths::NormalizeDirectoryName(Dir);
            return Dir;
        }

        // Deterministic header prepended to every generated file (no per-run data)
        // so unchanged launches byte-match the on-disk content and skip the write.
        // The source pointer stays generic on purpose: a namespace/topic page maps to
        // docs/wiki-src/<slug>.md, but a method page's editorial lives as an H3 section in
        // its namespace file, and the root index is assembled from every namespace
        // prelude — naming a single per-slug source file would be wrong for those.
        FString WikiDiskGenerator_WithHeader(const FString& Body)
        {
            return TEXT("<!-- GENERATED from docs/wiki-src overlays + handler registry at editor launch. ")
                TEXT("Do not edit here; edit the matching source under docs/wiki-src/. -->\n\n") + Body;
        }

        // Assemble the full slug -> markdown map for every page the live wiki can
        // serve: the root index plus every namespace, method, and topic page.
        // Values are raw render bodies; the generated-file header is prepended by
        // Generate() after the oversized-page split has settled which slugs exist.
        void WikiDiskGenerator_BuildPages(TMap<FString, FString>& Out)
        {
            // Root index (empty path renders the index, not the "wiki" topic page).
            FString B;
            if (WikiHandler::RenderPage(TEXT(""), B))
            {
                Out.Add(RootIndexSlug(), B);
            }

            TArray<FString> Slugs;
            if (!WikiHandler::EnumerateAllSlugs(Slugs))
            {
                return;
            }

            for (const FString& Slug : Slugs)
            {
                // The root index owns RootIndexSlug() (rendered from the empty path
                // above); skip it here so an enumerated slug can never overwrite it.
                // "wiki" is a normal topic page (the agent usage guide) and IS generated.
                if (Slug == RootIndexSlug())
                {
                    continue;
                }

                FString Body;
                if (WikiHandler::RenderPage(Slug, Body))
                {
                    Out.Add(Slug, Body);
                }
            }
        }

        // One `## ` section of a page being split.
        struct FWikiPageSection
        {
            FString Heading; // heading text, "## " stripped
            FString Body;    // section body, heading line excluded, trailing blank lines trimmed
        };

        // Headings the renderer generates rather than the overlay author. They are
        // what makes a namespace landing page navigable, so they stay on the landing
        // page when the rest of it is split out.
        bool WikiDiskGenerator_IsGeneratedIndexHeading(const FString& Heading)
        {
            return Heading == TEXT("Subgroups") || Heading == TEXT("Methods");
        }

        // Split Body at column-0 `## ` headings. OutHead takes everything above the
        // first heading (the H1 and the page's opening prose). A `## ` line inside a
        // fenced code block is content, not a heading — several pages quote markdown
        // in ``` blocks, and tearing the page at one of those would split mid-example.
        void WikiDiskGenerator_SplitIntoSections(const FString& Body, FString& OutHead,
                                                 TArray<FWikiPageSection>& OutSections)
        {
            TArray<FString> Lines;
            Body.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

            FString Heading;     // empty while the head is still being collected
            FString Accumulated;
            bool bInFence = false;

            // Close off whatever is being accumulated: the head on the first call,
            // one section on every call after it.
            auto Flush = [&OutHead, &OutSections, &Heading, &Accumulated]()
            {
                if (Heading.IsEmpty())
                {
                    OutHead = Accumulated.TrimEnd();
                }
                else
                {
                    FWikiPageSection Section;
                    Section.Heading = Heading;
                    Section.Body = Accumulated.TrimEnd();
                    OutSections.Add(MoveTemp(Section));
                }
                Accumulated.Reset();
            };

            for (const FString& Line : Lines)
            {
                if (Line.TrimStart().StartsWith(TEXT("```")))
                {
                    bInFence = !bInFence;
                }
                else if (!bInFence && Line.StartsWith(TEXT("## ")))
                {
                    // A bare `## ` with no text names no section; treat it as content
                    // so the head is not silently overwritten by the flush below.
                    const FString HeadingText = Line.Mid(3).TrimStartAndEnd();
                    if (!HeadingText.IsEmpty())
                    {
                        Flush();
                        Heading = HeadingText;
                        continue;
                    }
                }

                Accumulated += Line;
                Accumulated += TEXT("\n");
            }

            Flush();
        }

        // Filename-safe leaf for a section page: the heading lowercased with every
        // run of non-alphanumerics collapsed to a single '-'. A long heading is cut
        // at a word boundary rather than mid-word so the filename stays readable
        // and greppable.
        FString WikiDiskGenerator_SectionSlugLeaf(const FString& Heading)
        {
            constexpr int32 MaxLen = 60;

            FString Leaf;
            Leaf.Reserve(Heading.Len());
            for (const TCHAR Ch : Heading)
            {
                if (FChar::IsAlnum(Ch))
                {
                    Leaf.AppendChar(FChar::ToLower(Ch));
                }
                else if (!Leaf.IsEmpty() && Leaf[Leaf.Len() - 1] != TEXT('-'))
                {
                    Leaf.AppendChar(TEXT('-'));
                }
            }
            while (Leaf.EndsWith(TEXT("-"), ESearchCase::CaseSensitive))
            {
                Leaf.LeftChopInline(1);
            }

            if (Leaf.Len() > MaxLen)
            {
                int32 Boundary = INDEX_NONE;
                const FString Head = Leaf.Left(MaxLen + 1);
                Leaf = (Head.FindLastChar(TEXT('-'), Boundary) && Boundary > 0)
                    ? Leaf.Left(Boundary)
                    : Leaf.Left(MaxLen);
            }
            return Leaf;
        }

        // Slug for one section page: <page slug>.<heading leaf>. A leaf that would
        // collide with a page the renderer already owns, or with a section already
        // emitted on this run, takes a numeric suffix — two headings that slugify
        // the same must not overwrite each other.
        FString WikiDiskGenerator_SectionSlug(const FString& PageSlug, const FString& Heading, int32 Ordinal,
                                              const TMap<FString, FString>& Pages,
                                              const TMap<FString, FString>& SectionPages)
        {
            FString Leaf = WikiDiskGenerator_SectionSlugLeaf(Heading);
            if (Leaf.IsEmpty())
            {
                Leaf = FString::Printf(TEXT("section-%d"), Ordinal);
            }

            FString Candidate = PageSlug + TEXT(".") + Leaf;
            for (int32 Suffix = 2; Pages.Contains(Candidate) || SectionPages.Contains(Candidate); ++Suffix)
            {
                Candidate = FString::Printf(TEXT("%s.%s-%d"), *PageSlug, *Leaf, Suffix);
            }
            return Candidate;
        }

        // Rewrite Pages so no page is left oversized while it still has sections to
        // give up: an oversized page keeps its opening prose and gains a `## Sections`
        // index, and each of its `## ` sections becomes a page beside it. This is the
        // same shape a namespace file's `### ` method sections already take — those
        // are lifted into their own method pages — applied to `## ` boundaries.
        //
        // Never applied to the root index: that is the entry point every reader
        // starts from, and its "## Namespaces" / "## Task guides" indexes are the
        // navigation itself. A single section larger than the budget is left whole
        // (there is no `## ` boundary inside it to split on); the landing page is
        // still bounded, so the reader is no longer forced to read it to find out.
        void WikiDiskGenerator_SplitOversizedPages(TMap<FString, FString>& Pages)
        {
            TArray<FString> Slugs;
            Pages.GenerateKeyArray(Slugs);
            Slugs.Sort();

            TMap<FString, FString> SectionPages;

            for (const FString& Slug : Slugs)
            {
                const FString Body = Pages.FindChecked(Slug);
                if (Slug == RootIndexSlug() || Body.Len() <= kSectionSplitBudget)
                {
                    continue;
                }

                FString Head;
                TArray<FWikiPageSection> Sections;
                WikiDiskGenerator_SplitIntoSections(Body, Head, Sections);

                int32 Extractable = 0;
                for (const FWikiPageSection& Section : Sections)
                {
                    if (!WikiDiskGenerator_IsGeneratedIndexHeading(Section.Heading))
                    {
                        ++Extractable;
                    }
                }
                // An index pointing at one page costs a hop and saves nothing.
                if (Extractable < 2)
                {
                    continue;
                }

                FString Landing = Head;
                FString Index;
                int32 Ordinal = 0;
                for (const FWikiPageSection& Section : Sections)
                {
                    ++Ordinal;
                    if (WikiDiskGenerator_IsGeneratedIndexHeading(Section.Heading))
                    {
                        Landing += FString::Printf(TEXT("\n\n## %s\n\n%s"), *Section.Heading, *Section.Body);
                        continue;
                    }

                    const FString SectionSlug =
                        WikiDiskGenerator_SectionSlug(Slug, Section.Heading, Ordinal, Pages, SectionPages);

                    FString SectionPage = FString::Printf(TEXT("# %s — %s\n\n"), *Slug, *Section.Heading);
                    SectionPage += FString::Printf(
                        TEXT("One section of the [`%s`](%s.md) guide; that page indexes every section.\n\n"),
                        *Slug, *Slug);
                    SectionPage += Section.Body;
                    SectionPage += TEXT("\n");
                    SectionPages.Add(SectionSlug, MoveTemp(SectionPage));

                    Index += FString::Printf(TEXT("- [%s](%s.md) — %d chars\n"),
                                             *Section.Heading, *SectionSlug, Section.Body.Len());
                }

                Landing += TEXT("\n\n## Sections\n\n");
                Landing += TEXT("This page is too large to read in one call, so every `## ` section below is a ")
                           TEXT("separate file beside this one — open only the section you need.\n\n");
                Landing += Index;

                // TrimStart covers the one page shape with no head at all (a body
                // that opens on its first `## `), which would otherwise start with
                // the two blank lines meant to separate head from index.
                Pages[Slug] = Landing.TrimStart();
            }

            Pages.Append(MoveTemp(SectionPages));
        }

        bool WikiDiskGenerator_IsSafeManagedFilename(const FString& Filename)
        {
            if (Filename.IsEmpty()
                || Filename != Filename.ToLower()
                || Filename == OwnershipManifestFile
                || !FPaths::IsRelative(Filename)
                || FPaths::GetCleanFilename(Filename) != Filename
                || Filename.Contains(TEXT("/"))
                || Filename.Contains(TEXT("\\"))
                || Filename.Contains(TEXT(":")))
            {
                return false;
            }

            for (const TCHAR Ch : Filename)
            {
                if (!FChar::IsAlnum(Ch)
                    && Ch != TEXT('.')
                    && Ch != TEXT('_')
                    && Ch != TEXT('-'))
                {
                    return false;
                }
            }

            return Filename == RegistryFile
                || (Filename.Len() > 3
                    && Filename.EndsWith(TEXT(".md"), ESearchCase::CaseSensitive)
                    && !Filename.StartsWith(TEXT("."), ESearchCase::CaseSensitive));
        }

        bool WikiDiskGenerator_TryBuildOwnershipManifest(
            TArray<FString> Filenames,
            FString& OutManifest,
            FString& OutReason)
        {
            OutManifest.Reset();
            OutReason.Reset();
            if (Filenames.Num() > MaxOwnershipManifestEntries)
            {
                OutReason = FString::Printf(
                    TEXT("ownership manifest exceeds the %d-entry limit"),
                    MaxOwnershipManifestEntries);
                return false;
            }

            Filenames.Sort();
            for (int32 Index = 0; Index < Filenames.Num(); ++Index)
            {
                if (!WikiDiskGenerator_IsSafeManagedFilename(Filenames[Index]))
                {
                    OutReason = TEXT("ownership manifest contains an unsafe generated filename");
                    return false;
                }
                if (Index > 0 && Filenames[Index - 1] == Filenames[Index])
                {
                    OutReason = TEXT("ownership manifest contains duplicate generated filenames");
                    return false;
                }
            }

            OutManifest = TEXT("{\n");
            OutManifest += FString::Printf(TEXT("    \"owner\": \"%s\",\n"), OwnershipManifestOwner);
            OutManifest += FString::Printf(TEXT("    \"schemaVersion\": %d,\n"),
                OwnershipManifestSchemaVersion);
            OutManifest += TEXT("    \"files\": [\n");
            for (int32 Index = 0; Index < Filenames.Num(); ++Index)
            {
                OutManifest += FString::Printf(TEXT("        \"%s\"%s\n"), *Filenames[Index],
                    Index + 1 < Filenames.Num() ? TEXT(",") : TEXT(""));
            }
            OutManifest += TEXT("    ]\n}\n");

            const FTCHARToUTF8 Utf8(*OutManifest, OutManifest.Len());
            if (Utf8.Length() > MaxOwnershipManifestBytes)
            {
                OutReason = FString::Printf(
                    TEXT("ownership manifest exceeds the %lld-byte limit"),
                    static_cast<long long>(MaxOwnershipManifestBytes));
                OutManifest.Reset();
                return false;
            }
            return true;
        }

        EOwnershipManifestState WikiDiskGenerator_LoadOwnershipManifest(
            const FString& ManifestPath,
            TArray<FString>& OutFilenames,
            FString& OutReason)
        {
            OutFilenames.Reset();
            OutReason.Reset();

            IFileManager& FileManager = IFileManager::Get();
            // Size and content come from one archive so replacing the pathname cannot bypass
            // the bound between a metadata query and the read.
            TUniquePtr<FArchive> ManifestReader(
                FileManager.CreateFileReader(*ManifestPath, FILEREAD_Silent));
            if (!ManifestReader)
            {
                if (!FileManager.FileExists(*ManifestPath))
                {
                    return EOwnershipManifestState::Missing;
                }
                OutReason = TEXT("ownership manifest could not be opened for reading");
                return EOwnershipManifestState::Invalid;
            }

            const int64 ManifestBytes = ManifestReader->TotalSize();
            if (ManifestBytes < 0)
            {
                ManifestReader->Close();
                OutReason = TEXT("ownership manifest size could not be read");
                return EOwnershipManifestState::Invalid;
            }
            if (ManifestBytes > MaxOwnershipManifestBytes)
            {
                ManifestReader->Close();
                OutReason = FString::Printf(
                    TEXT("ownership manifest exceeds the %lld-byte limit"),
                    static_cast<long long>(MaxOwnershipManifestBytes));
                return EOwnershipManifestState::Invalid;
            }

            TArray<uint8> ManifestBuffer;
            ManifestBuffer.SetNumUninitialized(static_cast<int32>(ManifestBytes));
            if (ManifestBytes > 0)
            {
                ManifestReader->Serialize(ManifestBuffer.GetData(), ManifestBytes);
            }
            const bool bReadError = ManifestReader->IsError();
            const bool bClosed = ManifestReader->Close();
            ManifestReader.Reset();
            if (bReadError || !bClosed)
            {
                OutReason = TEXT("ownership manifest could not be read");
                return EOwnershipManifestState::Invalid;
            }

            FString Text;
            if (ManifestBuffer.Num() > 0)
            {
                FFileHelper::BufferToString(
                    Text, ManifestBuffer.GetData(), ManifestBuffer.Num());
            }

            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
            if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
            {
                OutReason = TEXT("ownership manifest is not valid JSON");
                return EOwnershipManifestState::Invalid;
            }

            FString Owner;
            double SchemaVersion = 0.0;
            const TArray<TSharedPtr<FJsonValue>>* FileValues = nullptr;
            if (!Root->TryGetStringField(TEXT("owner"), Owner)
                || Owner != OwnershipManifestOwner
                || !Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
                || SchemaVersion != static_cast<double>(OwnershipManifestSchemaVersion)
                || !Root->TryGetArrayField(TEXT("files"), FileValues)
                || FileValues == nullptr)
            {
                OutReason = TEXT("ownership manifest owner, schemaVersion, or files field is invalid");
                return EOwnershipManifestState::Invalid;
            }

            if (FileValues->Num() > MaxOwnershipManifestEntries)
            {
                OutReason = FString::Printf(
                    TEXT("ownership manifest exceeds the %d-entry limit"),
                    MaxOwnershipManifestEntries);
                return EOwnershipManifestState::Invalid;
            }

            FString PreviousFilename;
            for (const TSharedPtr<FJsonValue>& Value : *FileValues)
            {
                FString Filename;
                if (!Value.IsValid() || !Value->TryGetString(Filename)
                    || !WikiDiskGenerator_IsSafeManagedFilename(Filename))
                {
                    OutReason = TEXT("ownership manifest contains an unsafe generated filename");
                    OutFilenames.Reset();
                    return EOwnershipManifestState::Invalid;
                }

                if (!PreviousFilename.IsEmpty()
                    && PreviousFilename.Compare(Filename, ESearchCase::CaseSensitive) >= 0)
                {
                    OutReason = TEXT("ownership manifest filenames are not strictly sorted and unique");
                    OutFilenames.Reset();
                    return EOwnershipManifestState::Invalid;
                }
                PreviousFilename = Filename;
                OutFilenames.Add(MoveTemp(Filename));
            }

            return EOwnershipManifestState::Valid;
        }

        TArray<uint8> WikiDiskGenerator_EncodeUtf8(FStringView Content)
        {
            const TCHAR* ContentData = Content.Len() == 0 ? TEXT("") : Content.GetData();
            const FTCHARToUTF8 Utf8(ContentData, Content.Len());
            TArray<uint8> Bytes;
            if (Utf8.Length() > 0)
            {
                Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            }
            return Bytes;
        }

        bool WikiDiskGenerator_WriteBytes(
            const FString& Path,
            TArrayView<const uint8> Bytes,
            const TFunction<bool(const FString&, TArrayView<const uint8>, FString&)>& WriteFile,
            AtomicFileWriter::EExistingFilePolicy ExistingFilePolicy,
            FString& OutError)
        {
            if (WriteFile)
            {
                return WriteFile(Path, Bytes, OutError);
            }

            const AtomicFileWriter::FResult Result = AtomicFileWriter::WriteBytes(
                Path, Bytes, ExistingFilePolicy);
            if (!Result.IsSuccess())
            {
                OutError = Result.Error;
                return false;
            }
            return true;
        }

        bool WikiDiskGenerator_PublishChangedBytes(
            const FString& Path,
            TArrayView<const uint8> DesiredBytes,
            const FReconcileOperations& Operations,
            FString& OutError)
        {
            TArray<uint8> ExistingBytes;
            if (FFileHelper::LoadFileToArray(ExistingBytes, *Path)
                && ExistingBytes.Num() == DesiredBytes.Num()
                && (DesiredBytes.Num() == 0
                    || FMemory::Memcmp(
                        ExistingBytes.GetData(), DesiredBytes.GetData(), DesiredBytes.Num()) == 0))
            {
                return true;
            }

            return WikiDiskGenerator_WriteBytes(
                Path,
                DesiredBytes,
                Operations.WriteFile,
                AtomicFileWriter::EExistingFilePolicy::ReplaceExisting,
                OutError);
        }

        FReconcileResult WikiDiskGenerator_ReconcileOutput(
            const FString& InOutDir,
            const FString& InCanonicalOwnedRoot,
            const TMap<FString, FString>& DesiredFiles,
            const FReconcileOperations& Operations)
        {
            FReconcileResult ReconcileResult;
            if (InOutDir.IsEmpty())
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation has no output directory."));
                return ReconcileResult;
            }

            FString OutDir = FPaths::ConvertRelativePathToFull(InOutDir);
            FPaths::NormalizeDirectoryName(OutDir);
            if (OutDir.IsEmpty())
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation has no resolvable output directory."));
                return ReconcileResult;
            }

            FString CanonicalOwnedRoot;
            if (!InCanonicalOwnedRoot.IsEmpty())
            {
                CanonicalOwnedRoot = FPaths::ConvertRelativePathToFull(InCanonicalOwnedRoot);
                FPaths::NormalizeDirectoryName(CanonicalOwnedRoot);
            }
            const bool bCanonicalOwnedRoot = !CanonicalOwnedRoot.IsEmpty()
                && FPaths::IsSamePath(OutDir, CanonicalOwnedRoot);

            TArray<FString> DesiredFilenames;
            DesiredFiles.GenerateKeyArray(DesiredFilenames);
            DesiredFilenames.Sort();
            for (const FString& Filename : DesiredFilenames)
            {
                if (!WikiDiskGenerator_IsSafeManagedFilename(Filename))
                {
                    UE_LOG(LogWikiDiskGenerator, Error,
                        TEXT("Wiki generation refused unsafe output filename: %s"), *Filename);
                    return ReconcileResult;
                }
            }

            IFileManager& FileManager = IFileManager::Get();
            const FString ManifestPath = OutDir / OwnershipManifestFile;
            TArray<FString> PreviousManagedFiles;
            FString ManifestReason;
            const EOwnershipManifestState ManifestState = WikiDiskGenerator_LoadOwnershipManifest(
                ManifestPath, PreviousManagedFiles, ManifestReason);

            const bool bManifestAuthorizesPrune = bCanonicalOwnedRoot
                && ManifestState == EOwnershipManifestState::Valid;
            if (!bCanonicalOwnedRoot)
            {
                ReconcileResult.bPruneSkipped = true;
                ReconcileResult.PruneSkipReason = OutsideOwnedRootReason;
                UE_LOG(LogWikiDiskGenerator, Warning,
                    TEXT("Wiki pruning skipped for %s: %s."),
                    *OutDir, OutsideOwnedRootReason);
            }
            else if (ManifestState == EOwnershipManifestState::Missing)
            {
                ReconcileResult.bPruneSkipped = true;
                ReconcileResult.PruneSkipReason = TEXT("ownership manifest is missing");
            }
            else if (ManifestState == EOwnershipManifestState::Invalid)
            {
                ReconcileResult.bPruneSkipped = true;
                ReconcileResult.PruneSkipReason = TEXT("ownership manifest is invalid");
                UE_LOG(LogWikiDiskGenerator, Warning,
                    TEXT("Wiki output directory is not manifest-owned (%s): %s. No files were pruned."),
                    *ManifestPath, *ManifestReason);
            }

            TArray<FString> PruneCandidates;
            if (bManifestAuthorizesPrune)
            {
                for (const FString& PreviousFilename : PreviousManagedFiles)
                {
                    if (!DesiredFiles.Contains(PreviousFilename)
                        && FileManager.FileExists(*(OutDir / PreviousFilename)))
                    {
                        PruneCandidates.Add(PreviousFilename);
                    }
                }
            }

            // Stage the largest possible post-prune manifest before any final publication. This
            // proves both limits and the staging write even though the committed manifest is rebuilt
            // after pruning to retain only deletions that actually failed.
            TArray<FString> ProspectiveManagedFiles = DesiredFilenames;
            ProspectiveManagedFiles.Append(PruneCandidates);
            FString ProspectiveManifest;
            FString ManifestBuildError;
            if (!WikiDiskGenerator_TryBuildOwnershipManifest(
                    MoveTemp(ProspectiveManagedFiles), ProspectiveManifest, ManifestBuildError))
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation refused an oversized ownership manifest: %s. No output was changed or pruned."),
                    *ManifestBuildError);
                return ReconcileResult;
            }

            TMap<FString, TArray<uint8>> StagedBytes;
            for (const FString& Filename : DesiredFilenames)
            {
                StagedBytes.Add(Filename,
                    WikiDiskGenerator_EncodeUtf8(DesiredFiles.FindChecked(Filename)));
            }
            StagedBytes.Add(OwnershipManifestFile,
                WikiDiskGenerator_EncodeUtf8(ProspectiveManifest));

            FString OutputLeaf = FPaths::GetCleanFilename(OutDir);
            if (OutputLeaf.IsEmpty())
            {
                OutputLeaf = TEXT("wiki");
            }
            const FString StageParent = FPaths::GetPath(OutDir);
            if (StageParent.IsEmpty()
                || (!FileManager.DirectoryExists(*StageParent)
                    && !FileManager.MakeDirectory(*StageParent, /*Tree=*/true)))
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation could not create staging parent: %s"),
                    *StageParent);
                return ReconcileResult;
            }

            const FString StageDirectory = StageParent
                / FString::Printf(TEXT(".%s.pinwright-wiki-stage-%s"),
                    *OutputLeaf, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            if (!FileManager.MakeDirectory(*StageDirectory, /*Tree=*/false))
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation could not create staging directory: %s"),
                    *StageDirectory);
                return ReconcileResult;
            }
            ON_SCOPE_EXIT
            {
                if (!FileManager.DeleteDirectory(
                        *StageDirectory, /*RequireExists=*/false, /*Tree=*/true)
                    && FileManager.DirectoryExists(*StageDirectory))
                {
                    UE_LOG(LogWikiDiskGenerator, Warning,
                        TEXT("Wiki generation could not remove staging directory: %s"),
                        *StageDirectory);
                }
            };

            auto StageOne = [&](const FString& Filename) -> bool
            {
                FString Error;
                if (WikiDiskGenerator_WriteBytes(
                        StageDirectory / Filename,
                        StagedBytes.FindChecked(Filename),
                        Operations.StageFile,
                        AtomicFileWriter::EExistingFilePolicy::FailIfExists,
                        Error))
                {
                    return true;
                }

                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation failed to stage %s: %s. Final output and pruning were unchanged."),
                    *Filename, *Error);
                return false;
            };

            for (const FString& Filename : DesiredFilenames)
            {
                if (!StageOne(Filename))
                {
                    return ReconcileResult;
                }
            }
            if (!StageOne(OwnershipManifestFile))
            {
                return ReconcileResult;
            }

            if (!FileManager.DirectoryExists(*OutDir)
                && !FileManager.MakeDirectory(*OutDir, /*Tree=*/true))
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation could not create output directory after staging: %s"),
                    *OutDir);
                return ReconcileResult;
            }

            // Publish verified desired outputs per file. A whole-directory swap would make open
            // Windows readers block the directory rename; any output publication failure still
            // stops before pruning, but earlier per-file publications cannot be rolled back.
            auto PublishDesiredOne = [&](const FString& Filename) -> bool
            {
                FString Error;
                if (WikiDiskGenerator_PublishChangedBytes(
                        OutDir / Filename,
                        StagedBytes.FindChecked(Filename),
                        Operations,
                        Error))
                {
                    return true;
                }

                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation failed to publish staged %s: %s. Pruning was skipped; earlier per-file publications may remain."),
                    *Filename, *Error);
                return false;
            };

            for (const FString& Filename : DesiredFilenames)
            {
                if (!PublishDesiredOne(Filename))
                {
                    return ReconcileResult;
                }
            }

            bool bAllDeletesSucceeded = true;
            TArray<FString> FailedPruneFiles;
            for (const FString& PreviousFilename : PruneCandidates)
            {
                const FString PreviousPath = OutDir / PreviousFilename;
                if (!FileManager.FileExists(*PreviousPath))
                {
                    continue;
                }

                if (Operations.DeleteFile)
                {
                    Operations.DeleteFile(PreviousPath);
                }
                else
                {
                    FileManager.Delete(*PreviousPath, /*RequireExists=*/false,
                        /*EvenReadOnly=*/false, /*Quiet=*/true);
                }
                if (FileManager.FileExists(*PreviousPath))
                {
                    bAllDeletesSucceeded = false;
                    FailedPruneFiles.Add(PreviousFilename);
                    UE_LOG(LogWikiDiskGenerator, Warning,
                        TEXT("Wiki generation could not prune managed stale file %s; ownership was retained for retry."),
                        *PreviousPath);
                }
            }

            TArray<FString> FinalManagedFiles = DesiredFilenames;
            FinalManagedFiles.Append(FailedPruneFiles);
            FString FinalManifest;
            if (!WikiDiskGenerator_TryBuildOwnershipManifest(
                    MoveTemp(FinalManagedFiles), FinalManifest, ManifestBuildError))
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation could not build the final ownership manifest after pruning: %s. Earlier per-file publications and prunes may remain."),
                    *ManifestBuildError);
                return ReconcileResult;
            }

            const TArray<uint8> FinalManifestBytes =
                WikiDiskGenerator_EncodeUtf8(FinalManifest);
            FString FinalManifestError;
            if (!WikiDiskGenerator_PublishChangedBytes(
                    ManifestPath, FinalManifestBytes, Operations, FinalManifestError))
            {
                UE_LOG(LogWikiDiskGenerator, Error,
                    TEXT("Wiki generation failed to publish the final ownership manifest after pruning: %s. Earlier per-file publications and prunes may remain."),
                    *FinalManifestError);
                return ReconcileResult;
            }

            ReconcileResult.bSucceeded = bAllDeletesSucceeded;
            return ReconcileResult;
        }

        // Serialize the registry manifest by hand rather than through FJsonObject:
        // FJsonObject stores its fields in a TMap (arbitrary key order) and writes
        // every number through the double path, both of which would break the fixed,
        // byte-stable shape downstream consumers read. Slugs and tiers are lowercase
        // identifiers, so no string escaping is needed.
        FString WikiDiskGenerator_BuildRegistryJson(const TArray<WikiHandler::FRegistryNamespaceStat>& Namespaces,
                                                    int32 Operations)
        {
            FString Out = TEXT("{\n");
            Out += FString::Printf(TEXT("    \"operations\": %d,\n"), Operations);
            Out += FString::Printf(TEXT("    \"namespaceCount\": %d,\n"), Namespaces.Num());
            Out += TEXT("    \"namespaces\": [\n");
            for (int32 Index = 0; Index < Namespaces.Num(); ++Index)
            {
                const WikiHandler::FRegistryNamespaceStat& Ns = Namespaces[Index];
                Out += FString::Printf(TEXT("        { \"slug\": \"%s\", \"tier\": \"%s\", \"methods\": %d }%s\n"),
                                       *Ns.Slug, *Ns.Tier, Ns.Methods,
                                       (Index + 1 < Namespaces.Num()) ? TEXT(",") : TEXT(""));
            }
            Out += TEXT("    ]\n}\n");
            return Out;
        }

    }

#if WITH_DEV_AUTOMATION_TESTS
    bool Testing::ReconcileOutputForTests(
        const FString& OutDir,
        const TMap<FString, FString>& DesiredFiles,
        const FOperations& Operations)
    {
        FReconcileOperations InternalOperations;
        InternalOperations.StageFile = Operations.StageFile;
        InternalOperations.WriteFile = Operations.WriteFile;
        InternalOperations.DeleteFile = Operations.DeleteFile;
        return WikiDiskGenerator_ReconcileOutput(
            OutDir, OutDir, DesiredFiles, InternalOperations).bSucceeded;
    }

    Testing::FReconcileResult Testing::ReconcileOutputForTestsDetailed(
        const FString& OutDir,
        const FString& CanonicalOwnedRoot,
        const TMap<FString, FString>& DesiredFiles,
        const FOperations& Operations)
    {
        FReconcileOperations InternalOperations;
        InternalOperations.StageFile = Operations.StageFile;
        InternalOperations.WriteFile = Operations.WriteFile;
        InternalOperations.DeleteFile = Operations.DeleteFile;
        const auto InternalResult =
            WikiDiskGenerator_ReconcileOutput(
                OutDir, CanonicalOwnedRoot, DesiredFiles, InternalOperations);

        Testing::FReconcileResult Result;
        Result.bSucceeded = InternalResult.bSucceeded;
        Result.bPruneSkipped = InternalResult.bPruneSkipped;
        Result.PruneSkipReason = InternalResult.PruneSkipReason;
        return Result;
    }

    FString Testing::OwnershipManifestFilename()
    {
        return OwnershipManifestFile;
    }

    FString Testing::OutsideOwnedRootPruneSkipReason()
    {
        return OutsideOwnedRootReason;
    }

    int64 Testing::MaxOwnershipManifestBytesForTests()
    {
        return MaxOwnershipManifestBytes;
    }

    int32 Testing::MaxOwnershipManifestEntriesForTests()
    {
        return MaxOwnershipManifestEntries;
    }
#endif

    FString OutputDirectory()
    {
        // Project-settings override wins (relative paths resolve against the
        // project dir); otherwise default under the project's Saved folder.
        FString Dir = GetDefault<UPinWrightProjectSettings>()->WikiOutputDirectory;
        if (Dir.IsEmpty())
        {
            return WikiDiskGenerator_CanonicalOwnedOutputDirectory();
        }
        else if (FPaths::IsRelative(Dir))
        {
            Dir = FPaths::ProjectDir() / Dir;
        }
        FPaths::NormalizeDirectoryName(Dir);
        return FPaths::ConvertRelativePathToFull(Dir);
    }

    FString RootIndexSlug()
    {
        // Dedicated filename for the assembled namespace index. Deliberately NOT
        // "wiki": docs/wiki-src/wiki.md is the standalone agent usage-guide topic reached
        // via call("wiki"), so the root index must not shadow it on disk.
        return TEXT("index");
    }

    FString PagePath(const FString& Slug)
    {
        if (Slug.IsEmpty())
        {
            return FString();
        }
        const FString Dir = OutputDirectory();
        if (Dir.IsEmpty())
        {
            return FString();
        }
        return Dir / (Slug + TEXT(".md"));
    }

    void Generate()
    {
        const FString OutDir = OutputDirectory();
        if (OutDir.IsEmpty())
        {
            return; // no resolvable output directory — nothing to write
        }

        TMap<FString, FString> Pages;
        WikiDiskGenerator_BuildPages(Pages);
        if (Pages.Num() == 0)
        {
            return;
        }

        // Split before the header is prepended: the split works on render bodies,
        // and the section pages it adds need the same header as every other file.
        WikiDiskGenerator_SplitOversizedPages(Pages);
        for (TPair<FString, FString>& Page : Pages)
        {
            Page.Value = WikiDiskGenerator_WithHeader(Page.Value);
        }

        TArray<WikiHandler::FRegistryNamespaceStat> Namespaces;
        int32 Operations = 0;
        if (!WikiHandler::GetRegistryStats(Namespaces, Operations))
        {
            UE_LOG(LogWikiDiskGenerator, Error,
                TEXT("Wiki generation could not build registry.json; no output was changed."));
            return;
        }

        TMap<FString, FString> DesiredFiles;
        for (TPair<FString, FString>& Page : Pages)
        {
            DesiredFiles.Add(Page.Key + TEXT(".md"), MoveTemp(Page.Value));
        }
        DesiredFiles.Add(RegistryFile,
            WikiDiskGenerator_BuildRegistryJson(Namespaces, Operations));

        WikiDiskGenerator_ReconcileOutput(
            OutDir,
            WikiDiskGenerator_CanonicalOwnedOutputDirectory(),
            DesiredFiles,
            FReconcileOperations());
    }
}
