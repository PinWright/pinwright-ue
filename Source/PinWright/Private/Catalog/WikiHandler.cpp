// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Catalog/WikiHandler.h"
#include "Catalog/MarkdownHelpers.h"
#include "Catalog/SuggestionHelpers.h"
#include "Catalog/WikiOverlay.h"
#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "IntegrationGates.h"
#include "PinWrightSubsystem.h"
#include "Handlers/HandlerRegistration.h"

#include "Algo/Sort.h"
#include "Containers/Set.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogWikiHandler, Log, All);

namespace WikiHandler
{
    namespace
    {
        TSharedPtr<FRpcDispatcher> GetLiveDispatcher();

        // Tier rendered for a namespace docs/wiki-src/maturity.json does not
        // classify, or classifies with a value outside the known set. The
        // maturity map fails CLOSED: an omission renders as the least-trusted
        // tier and says so by name, never as the most-trusted one. Until
        // B-maturity-unmapped-namespace-fails-open an unmapped namespace rendered
        // bare, and the root-index legend reads bare as "core" - so a brand-new
        // or forgotten namespace advertised itself as solid primary surface.
        // `unclassified` is a render-time fallback only; it is never a legal
        // value in maturity.json, which must classify every namespace explicitly.
        const TCHAR* const UnclassifiedTier = TEXT("unclassified");

        bool IsKnownTier(const FString& Tier)
        {
            return Tier == TEXT("core") || Tier == TEXT("experimental") || Tier == TEXT("internal");
        }

        FString RootIntroFromPrelude(const FString& Prelude)
        {
            if (Prelude.IsEmpty()) return FString();

            TArray<FString> Lines;
            Prelude.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

            int32 Start = 0;
            while (Start < Lines.Num() && Lines[Start].TrimStartAndEnd().IsEmpty())
            {
                ++Start;
            }

            int32 End = Lines.Num();
            for (int32 Index = Start; Index < Lines.Num(); ++Index)
            {
                const FString TrimmedStart = Lines[Index].TrimStart();
                if (TrimmedStart.StartsWith(TEXT("## ")) || TrimmedStart.StartsWith(TEXT("### ")))
                {
                    End = Index;
                    break;
                }
            }

            while (End > Start && Lines[End - 1].TrimStartAndEnd().IsEmpty())
            {
                --End;
            }

            if (End <= Start)
            {
                return FString();
            }

            FString Intro;
            for (int32 Index = Start; Index < End; ++Index)
            {
                if (!Intro.IsEmpty())
                {
                    Intro += TEXT("\n");
                }
                Intro += Lines[Index];
            }
            return Intro;
        }

        FString RenderRootNamespaceEntry(const FString& Namespace, const FString& RootIntro, const FString& Tier)
        {
            // ONLY `core` renders bare (see the root-intro legend). Every other
            // tier is flagged inline right after the name - including the
            // `unclassified` fallback, so a namespace missing from maturity.json
            // cannot pass itself off as core. Callers resolve through ResolveTier
            // and never pass an empty string; the fallback here is belt-and-braces.
            const FString Marker = (Tier == TEXT("core"))
                ? FString()
                : FString::Printf(TEXT(" (%s)"), Tier.IsEmpty() ? UnclassifiedTier : *Tier);

            if (RootIntro.IsEmpty())
            {
                return FString::Printf(TEXT("- `%s`%s\n"), *Namespace, *Marker);
            }

            TArray<FString> Lines;
            RootIntro.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
            if (Lines.Num() == 1)
            {
                return FString::Printf(TEXT("- `%s`%s — %s\n"), *Namespace, *Marker, *Lines[0].TrimStartAndEnd());
            }

            FString Out = FString::Printf(TEXT("- `%s`%s — %s\n"), *Namespace, *Marker, *Lines[0].TrimStartAndEnd());
            for (int32 Index = 1; Index < Lines.Num(); ++Index)
            {
                if (Lines[Index].TrimStartAndEnd().IsEmpty())
                {
                    Out += TEXT("  \n");
                }
                else
                {
                    Out += TEXT("  ");
                    Out += Lines[Index];
                    Out += TEXT("\n");
                }
            }
            return Out;
        }

        // Cached views over the immutable registry. The registry is fixed after
        // DrainAutoRegistrations runs at subsystem init; this cache is built lazily
        // on the first RenderPage call and reused for every subsequent call.
        struct FWikiCache
        {
            TSet<FString> AllNodes;                                              // every dotted prefix that is a tree node
            TSet<FString> CategoryNodes;                                         // exactly the set of registered Categories
            TSet<FString> TopicNodes;                                            // lowercased standalone topic-page slugs, exclusive of any registered Category
            TMap<FString, const FHandlerRegistration*> MethodsByLowerName;       // lowercased MethodName -> registration
            TMap<FString, TArray<const FHandlerRegistration*>> MethodsByCategory; // lowercased Category -> methods
            TArray<FString> SuggestionPool;                                      // method full names + namespace nodes, lowercase
            TMap<FString, FString> MaturityBySlug;                               // top-level namespace slug -> "core"|"experimental"|"internal"
        };

        // Load docs/wiki-src/maturity.json: a flat JSON object mapping each
        // top-level namespace slug to its maturity tier. Loaded once per wiki
        // cache build; drift against the registered namespace set is warned
        // about in GetWikiCache. Missing or malformed file yields an empty map,
        // and every namespace then renders `unclassified` - the least-trusted
        // tier - rather than silently reading as core (see ResolveTier).
        TMap<FString, FString> LoadMaturityMap()
        {
            TMap<FString, FString> Out;
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
            if (!Plugin.IsValid())
            {
                return Out;
            }
            const FString FilePath = Plugin->GetBaseDir() / TEXT("docs") / TEXT("wiki-src") / TEXT("maturity.json");

            FString FileBody;
            if (!FFileHelper::LoadFileToString(FileBody, *FilePath))
            {
                UE_LOG(LogWikiHandler, Warning, TEXT("Failed to read maturity map at %s"), *FilePath);
                return Out;
            }

            TSharedPtr<FJsonObject> Root;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileBody);
            if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
            {
                UE_LOG(LogWikiHandler, Warning, TEXT("Failed to parse maturity map at %s"), *FilePath);
                return Out;
            }

            for (const auto& Pair : Root->Values)
            {
                FString Tier;
                if (Pair.Value.IsValid() && Pair.Value->TryGetString(Tier))
                {
                    // FString(*Pair.Key): JSON object keys are FString on UE <= 5.7 but
                    // UE::TSharedString<TCHAR> on 5.8; both dereference to const TCHAR*.
                    Out.Add(FString(*Pair.Key).ToLower(), Tier.ToLower());
                }
            }
            return Out;
        }

        // The single place the maturity map is read for rendering. Fails CLOSED:
        // a slug with no maturity.json entry - or an entry carrying a value
        // outside the known set, e.g. a typo - resolves to `unclassified`, never
        // to `core` and never to empty. Every render site (root index entry,
        // namespace Stability line, registry.json row) goes through here so the
        // three cannot disagree about what an omission means.
        FString ResolveTier(const FWikiCache& Cache, const FString& TopLevelSlug)
        {
            const FString* Tier = Cache.MaturityBySlug.Find(TopLevelSlug);
            return (Tier && IsKnownTier(*Tier)) ? *Tier : FString(UnclassifiedTier);
        }

        const FWikiCache& GetWikiCache(const FRpcDispatcher& Dispatcher)
        {
            const TMap<FString, FHandlerRegistration>& Registry = Dispatcher.GetAutoRegisteredHandlers();
            const uint32 Generation = Dispatcher.GetRegistryGeneration();

            // Key the cache on (source dispatcher, generation). A different dispatcher
            // (e.g. a fresh test fixture) or a bumped generation (post-init registrant)
            // invalidates the snapshot and forces a rebuild.
            static FWikiCache Cache;
            static const FRpcDispatcher* CachedDispatcher = nullptr;
            static uint32 CachedGeneration = 0;
            static bool bBuilt = false;
            if (bBuilt && CachedDispatcher == &Dispatcher && CachedGeneration == Generation)
            {
                return Cache;
            }

            // Stale or first build: clear any prior snapshot before repopulating.
            Cache = FWikiCache();
            CachedDispatcher = &Dispatcher;
            CachedGeneration = Generation;

            for (const auto& Pair : Registry)
            {
                const FHandlerRegistration& Reg = Pair.Value;
                Cache.MethodsByLowerName.Add(Reg.MethodName.ToLower(), &Reg);

                const FString CatLower = Reg.Category.ToLower();
                if (CatLower.IsEmpty())
                {
                    continue;
                }
                Cache.CategoryNodes.Add(CatLower);
                Cache.MethodsByCategory.FindOrAdd(CatLower).Add(&Reg);

                // Walk dotted prefixes left-to-right; each becomes a tree node.
                int32 SearchStart = 0;
                while (true)
                {
                    const int32 Found = CatLower.Find(TEXT("."), ESearchCase::IgnoreCase,
                                                      ESearchDir::FromStart, SearchStart);
                    if (Found == INDEX_NONE)
                    {
                        break;
                    }
                    Cache.AllNodes.Add(CatLower.Left(Found));
                    SearchStart = Found + 1;
                }
                Cache.AllNodes.Add(CatLower);
            }

            // Maturity tiers for index markers and Stability lines. Drift is
            // checked only against the live dispatcher's registry — fixture
            // dispatchers in tests carry arbitrary partial registries and would
            // spam a spurious warning per map entry.
            Cache.MaturityBySlug = LoadMaturityMap();
            if (GetLiveDispatcher().Get() == &Dispatcher)
            {
                TSet<FString> TopLevels;
                for (const FString& Node : Cache.AllNodes)
                {
                    if (!Node.Contains(TEXT(".")))
                    {
                        TopLevels.Add(Node);
                    }
                }
                for (const FString& Ns : TopLevels)
                {
                    const FString* Tier = Cache.MaturityBySlug.Find(Ns);
                    if (!Tier)
                    {
                        UE_LOG(LogWikiHandler, Warning,
                            TEXT("maturity.json has no tier for registered namespace '%s' - it renders as '%s'; ")
                            TEXT("classify it in docs/wiki-src/maturity.json"), *Ns, UnclassifiedTier);
                    }
                    else if (!IsKnownTier(*Tier))
                    {
                        UE_LOG(LogWikiHandler, Warning,
                            TEXT("maturity.json gives namespace '%s' the unknown tier '%s' - it renders as '%s'; ")
                            TEXT("use core, experimental or internal"), *Ns, **Tier, UnclassifiedTier);
                    }
                }
                for (const auto& KV : Cache.MaturityBySlug)
                {
                    if (!TopLevels.Contains(KV.Key))
                    {
                        UE_LOG(LogWikiHandler, Warning,
                            TEXT("maturity.json key '%s' matches no registered namespace"), *KV.Key);
                    }
                }
            }

            // Sort method lists per category so renders are deterministic.
            for (auto& KV : Cache.MethodsByCategory)
            {
                Algo::Sort(KV.Value, [](const FHandlerRegistration* A, const FHandlerRegistration* B)
                {
                    return A->MethodName < B->MethodName;
                });
            }

            // Suggestion pool = every method full name + every namespace node, lowercase.
            Cache.SuggestionPool.Reserve(Cache.MethodsByLowerName.Num() + Cache.AllNodes.Num());
            for (const auto& KV : Cache.MethodsByLowerName)
            {
                Cache.SuggestionPool.Add(KV.Key);
            }
            for (const FString& Node : Cache.AllNodes)
            {
                Cache.SuggestionPool.Add(Node);
            }

            // Enumerate standalone topic pages under <plugin>/docs/wiki-src/*.md so they
            // route via call("<slug>") and surface as fuzzy suggestions. Slugs that
            // collide with a registered Category are skipped — Category pages win.
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
            if (Plugin.IsValid())
            {
                const FString WikiDir = Plugin->GetBaseDir() / TEXT("docs") / TEXT("wiki-src");
                TArray<FString> WikiFiles;
                IFileManager::Get().FindFiles(WikiFiles, *(WikiDir / TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
                for (const FString& FileName : WikiFiles)
                {
                    const FString Slug = FPaths::GetBaseFilename(FileName).ToLower();
                    // Skip directory documentation (README*.md) — not topic content.
                    if (Slug.StartsWith(TEXT("readme"))) continue;
                    if (Cache.CategoryNodes.Contains(Slug)) continue;
                    if (Cache.TopicNodes.Contains(Slug)) continue;
                    Cache.TopicNodes.Add(Slug);
                    Cache.SuggestionPool.Add(Slug);
                }
            }

            bBuilt = true;
            return Cache;
        }

        EWikiNodeKind ClassifyNode(const FString& PathLower, const FWikiCache& Cache)
        {
            // Method check first — O(1) lookup against the lowercased index.
            if (Cache.MethodsByLowerName.Contains(PathLower))
            {
                return EWikiNodeKind::Method;
            }

            // Topic pages are checked after Method (method wins) but before any
            // Category checks; discovery already excludes Category collisions.
            if (Cache.TopicNodes.Contains(PathLower))
            {
                return EWikiNodeKind::Topic;
            }

            const bool bIsCategory = Cache.CategoryNodes.Contains(PathLower);

            // Strict prefix means another Category starts with PathLower + "."
            const FString PrefixProbe = PathLower + TEXT(".");
            bool bHasDeeperCategory = false;
            for (const FString& Cat : Cache.CategoryNodes)
            {
                if (Cat.StartsWith(PrefixProbe))
                {
                    bHasDeeperCategory = true;
                    break;
                }
            }

            if (bIsCategory && bHasDeeperCategory) return EWikiNodeKind::Hybrid;
            if (bIsCategory)                       return EWikiNodeKind::LeafNamespace;
            if (bHasDeeperCategory)                return EWikiNodeKind::Branch;
            return EWikiNodeKind::NotFound;
        }

        // Lowercase + collapse legacy MCP tool names (e.g. "actor_spawn_from_blueprint")
        // into the dotted namespace.method form. Only the first underscore is treated
        // as the namespace separator, and only when the input has no dots — otherwise
        // a canonical name like "actor.spawn_from_blueprint" would be corrupted into
        // "actor.spawn.from.blueprint" and miss the method lookup.
        FString NormalizeQuery(const FString& In)
        {
            FString Out = In.ToLower();
            if (!Out.Contains(TEXT(".")))
            {
                int32 FirstUnderscore = INDEX_NONE;
                if (Out.FindChar(TEXT('_'), FirstUnderscore))
                {
                    Out = Out.Left(FirstUnderscore) + TEXT(".") + Out.Mid(FirstUnderscore + 1);
                }
            }
            return Out;
        }

        // Resolve an incoming path to the canonical lowercased slug the renderer
        // should classify. The legacy underscore->dot collapse (NormalizeQuery) is
        // applied only as a FALLBACK: if the verbatim lowercased input already names
        // a registered node (a real namespace/method/topic, e.g. "game_framework"),
        // it is returned unchanged so a legitimately underscore-named namespace is not
        // mangled into "game.framework" and lost. Only when the verbatim form does not
        // resolve do we collapse the first underscore (e.g. the legacy flat tool name
        // "actor_spawn" -> "actor.spawn"). This keeps both paths working: real
        // underscore namespaces classify verbatim, legacy flattened names still
        // collapse to their dotted form.
        FString ResolveCanonicalLower(const FString& In, const FWikiCache& Cache)
        {
            const FString Verbatim = In.ToLower();
            if (ClassifyNode(Verbatim, Cache) != EWikiNodeKind::NotFound)
            {
                return Verbatim;
            }
            return NormalizeQuery(In);
        }

        // Children of Path: immediate next-segment paths derived from any tree node that strictly
        // descends from Path. For root (empty Path), returns top-level segments.
        TArray<FString> ImmediateChildren(const FString& PathLower, const FWikiCache& Cache)
        {
            TSet<FString> Children;
            const FString Prefix = PathLower.IsEmpty() ? FString() : (PathLower + TEXT("."));
            const int32 PrefixLen = Prefix.Len();

            auto AddImmediate = [&](const FString& NodeLower)
            {
                if (PathLower.IsEmpty())
                {
                    int32 DotIdx = INDEX_NONE;
                    NodeLower.FindChar(TEXT('.'), DotIdx);
                    Children.Add(DotIdx == INDEX_NONE ? NodeLower : NodeLower.Left(DotIdx));
                    return;
                }
                if (!NodeLower.StartsWith(Prefix)) return;
                const FString Tail = NodeLower.Mid(PrefixLen);
                int32 DotIdx = INDEX_NONE;
                Tail.FindChar(TEXT('.'), DotIdx);
                const FString FirstSeg = (DotIdx == INDEX_NONE) ? Tail : Tail.Left(DotIdx);
                if (!FirstSeg.IsEmpty())
                {
                    Children.Add(PathLower + TEXT(".") + FirstSeg);
                }
            };

            for (const FString& Node : Cache.AllNodes)
            {
                AddImmediate(Node);
            }

            TArray<FString> Out = Children.Array();
            Out.Sort();
            return Out;
        }

        // ---- Standalone guide index (root page) --------------------------------
        // Derived from the same TopicNodes discovery that makes these pages
        // navigable, so a page dropped into docs/wiki-src/ is listed on the root the
        // next time it renders. Nothing is hand-maintained here: the defect this
        // replaces was exactly that a guide was only findable if some namespace
        // prelude happened to link it.

        // One guide entry is one line. The root index already spends its budget on
        // the 2-sentence namespace preludes below, so an over-long opening sentence
        // is cut back to its headline clause instead of pasting the page's prelude.
        constexpr int32 kGuideSummarySoftBudget = 140;
        constexpr int32 kGuideSummaryHardCap = 200;
        // A clause head shorter than this is a fragment, not a headline ("Foo — the
        // thing that..."), so such a sentence is hard-truncated instead.
        constexpr int32 kGuideSummaryMinClauseHead = 24;

        // First paragraph of Text as a single line (source line breaks become
        // spaces); the paragraph ends at the first blank line.
        FString FirstParagraphOneLine(const FString& Text)
        {
            TArray<FString> Lines;
            Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

            FString Out;
            for (const FString& Line : Lines)
            {
                const FString Trimmed = Line.TrimStartAndEnd();
                if (Trimmed.IsEmpty())
                {
                    if (!Out.IsEmpty())
                    {
                        break; // end of the first paragraph
                    }
                    continue;  // leading blank lines
                }
                if (!Out.IsEmpty())
                {
                    Out += TEXT(" ");
                }
                Out += Trimmed;
            }
            return Out;
        }

        // True when the word ending at a sentence-final period is a known
        // abbreviation, so "e.g. a level" does not read as a sentence end. These
        // pages use the short forms freely in their opening paragraph.
        bool EndsWithAbbreviation(const FString& Head)
        {
            static const TCHAR* const Abbreviations[] = {
                TEXT("e.g"), TEXT("i.e"), TEXT("etc"), TEXT("vs"), TEXT("cf"), TEXT("fig"), TEXT("approx") };

            int32 SpaceIdx = INDEX_NONE;
            const FString Word = Head.FindLastChar(TEXT(' '), SpaceIdx) ? Head.Mid(SpaceIdx + 1) : Head;
            const FString WordLower = Word.ToLower();
            for (const TCHAR* Abbreviation : Abbreviations)
            {
                if (WordLower == Abbreviation)
                {
                    return true;
                }
            }
            return false;
        }

        // Leading sentence of a one-line paragraph. A '.', '!' or '?' closes the
        // sentence only when the next non-markup character is whitespace (or the
        // line ends), which leaves dotted method names (`system.job_status`) and
        // decimals intact; punctuation inside a `code span` never closes it.
        FString FirstSentence(const FString& Line)
        {
            bool bInCode = false;
            for (int32 Index = 0; Index < Line.Len(); ++Index)
            {
                const TCHAR Current = Line[Index];
                if (Current == TEXT('`'))
                {
                    bInCode = !bInCode;
                    continue;
                }
                if (bInCode || (Current != TEXT('.') && Current != TEXT('!') && Current != TEXT('?')))
                {
                    continue;
                }

                // Closing markup can sit between the punctuation and the space
                // ("**... never submit it yourself.**"); keep it with the sentence.
                int32 End = Index + 1;
                while (End < Line.Len() && (Line[End] == TEXT('*') || Line[End] == TEXT('`')
                                            || Line[End] == TEXT(')') || Line[End] == TEXT(']')
                                            || Line[End] == TEXT('"') || Line[End] == TEXT('\'')))
                {
                    ++End;
                }
                if (End < Line.Len() && !FChar::IsWhitespace(Line[End]))
                {
                    continue;
                }
                if (Current == TEXT('.') && EndsWithAbbreviation(Line.Left(Index)))
                {
                    continue;
                }
                return Line.Left(End);
            }
            return Line;
        }

        // Headline half of a "<headline> — <elaboration>" / "<headline>: <detail>"
        // opening sentence. Only used to pull an over-long sentence back inside the
        // one-line budget, never to shorten a sentence that already fits.
        FString ClauseHead(const FString& Sentence)
        {
            static const TCHAR* const Separators[] = { TEXT(" — "), TEXT(" - "), TEXT(": ") };

            int32 Cut = INDEX_NONE;
            for (const TCHAR* Separator : Separators)
            {
                const int32 At = Sentence.Find(Separator, ESearchCase::CaseSensitive, ESearchDir::FromStart);
                if (At != INDEX_NONE && (Cut == INDEX_NONE || At < Cut))
                {
                    Cut = At;
                }
            }
            return (Cut == INDEX_NONE) ? Sentence : Sentence.Left(Cut);
        }

        // One-line summary for a guide entry, taken from the page itself: the
        // opening sentence of its prelude, clause-trimmed and then hard-truncated
        // when it still overruns. Empty when the page has no prose prelude, in which
        // case the entry renders as the bare slug.
        FString GuideSummary(const FString& Slug)
        {
            const FString Intro = RootIntroFromPrelude(WikiOverlay::LoadGroupPrelude(Slug));
            FString Summary = FirstSentence(FirstParagraphOneLine(Intro)).TrimStartAndEnd();
            if (Summary.IsEmpty())
            {
                return Summary;
            }

            if (Summary.Len() > kGuideSummarySoftBudget)
            {
                const FString Head = ClauseHead(Summary).TrimEnd();
                if (Head.Len() >= kGuideSummaryMinClauseHead && Head.Len() < Summary.Len())
                {
                    Summary = Head;
                }
            }

            if (Summary.Len() > kGuideSummaryHardCap)
            {
                FString Cut = Summary.Left(kGuideSummaryHardCap);
                int32 LastSpace = INDEX_NONE;
                if (Cut.FindLastChar(TEXT(' '), LastSpace) && LastSpace > kGuideSummaryHardCap / 2)
                {
                    Cut = Cut.Left(LastSpace);
                }
                Summary = Cut.TrimEnd() + TEXT("...");
            }
            return Summary;
        }

        // Guide slugs for the root index: topic pages whose slug carries no dot. A
        // dotted topic (blueprint.bpir-gotchas) is reference material owned by a
        // namespace and is linked from that namespace's page, so it stays off the
        // root; an undotted one belongs to no namespace and the root index is the
        // only place it can be found. Registered namespaces and methods are never
        // enrolled as topics, so neither can leak into this list. `workflows` is
        // itself the index of guides, so it leads; the rest sort alphabetically.
        TArray<FString> GuideSlugs(const FWikiCache& Cache)
        {
            TArray<FString> Out;
            for (const FString& Topic : Cache.TopicNodes)
            {
                if (!Topic.Contains(TEXT(".")))
                {
                    Out.Add(Topic);
                }
            }
            Out.Sort();

            const FString WorkflowsSlug(TEXT("workflows"));
            const int32 WorkflowsIdx = Out.IndexOfByKey(WorkflowsSlug);
            if (WorkflowsIdx > 0)
            {
                Out.RemoveAt(WorkflowsIdx);
                Out.Insert(WorkflowsSlug, 0);
            }
            return Out;
        }

        FString RenderGuideIndex(const FWikiCache& Cache)
        {
            const TArray<FString> Guides = GuideSlugs(Cache);
            if (Guides.Num() == 0)
            {
                return FString();
            }

            FString Out = TEXT("## Task guides\n\n");
            Out += TEXT("Task-shaped pages: start here when you know the job but not which namespace owns the calls. ")
                   TEXT("These are walkthroughs, not API reference - the per-namespace reference is under Namespaces below. ")
                   TEXT("Read one with `call({ path: \"<slug>\" })`.\n\n");
            for (const FString& Slug : Guides)
            {
                const FString Summary = GuideSummary(Slug);
                Out += Summary.IsEmpty()
                    ? FString::Printf(TEXT("- `%s`\n"), *Slug)
                    : FString::Printf(TEXT("- `%s` — %s\n"), *Slug, *Summary);
            }
            Out += TEXT("\n");
            return Out;
        }

        FString RenderRoot(const FWikiCache& Cache)
        {
            const TArray<FString> TopLevels = ImmediateChildren(FString(), Cache);

            FString Out = TEXT("# PinWright Wiki\n\n");
            Out += TEXT("Use `call({ path: \"<namespace>\" })` to drill into a namespace, or `call({ path: \"<namespace.method>\", args: { ... } })` to execute a method.\n");
            Out += TEXT("A method that takes no arguments still needs an explicit empty `args: {}` to run - omitting `args` returns the wiki page instead of executing.\n\n");
            // Guides first: they are the pages a newcomer needs and the only ones
            // with no index of their own to be found from.
            Out += RenderGuideIndex(Cache);
            Out += TEXT("## Namespaces\n\n");
            // The tier legend sits with the list it annotates, not in the intro.
            Out += TEXT("Namespaces marked (experimental) work but are less complete and still changing; (internal) is plumbing not meant for direct use; (unclassified) has no maturity entry at all, so treat it as the least stable of the four and expect anything; unmarked namespaces are core.\n\n");
            for (const FString& Ns : TopLevels)
            {
                Out += RenderRootNamespaceEntry(Ns, RootIntroFromPrelude(WikiOverlay::LoadGroupPrelude(Ns)),
                    ResolveTier(Cache, Ns));
            }
            // Support pointer: surfaced at the root so an agent that hits an
            // unworkable bug (or a user asking to report one) discovers the
            // channel without being told it exists.
            Out += TEXT("\n## Support\n\n");
            Out += TEXT("Hit a plugin bug you cannot work around, or want to request a feature? Reports go to the issue tracker at `https://github.com/PinWright/pinwright-ue/issues`. Call `call({ path: \"support\" })` for the guided flow that drafts a high-quality report from what actually happened this session and opens a prefilled GitHub draft for the user to review and submit - it never submits on its own.\n");
            return Out;
        }

        FString RenderMethodSignatureLine(const FHandlerRegistration& Reg)
        {
            return FString::Printf(TEXT("- `%s` — %s\n"),
                *Reg.MethodName,
                *MarkdownHelpers::EscapeMarkdownText(Reg.Summary));
        }

        FString RenderChildIndex(const TArray<FString>& Children)
        {
            if (Children.Num() == 0) return FString();
            FString Out = TEXT("## Subgroups\n\n");
            for (const FString& Child : Children)
            {
                Out += FString::Printf(TEXT("- `%s`\n"), *Child);
            }
            Out += TEXT("\n");
            return Out;
        }

        FString RenderMethodList(const FString& PathLower, const FWikiCache& Cache)
        {
            const TArray<const FHandlerRegistration*>* Methods = Cache.MethodsByCategory.Find(PathLower);
            if (!Methods || Methods->Num() == 0) return FString();
            FString Out = TEXT("## Methods\n\n");
            for (const FHandlerRegistration* Reg : *Methods)
            {
                Out += RenderMethodSignatureLine(*Reg);
            }
            Out += TEXT("\n");
            return Out;
        }

        FString RenderMethodPage(const FHandlerRegistration& Reg)
        {
            FString Out = FString::Printf(TEXT("# `%s`\n\n"), *Reg.MethodName);
            Out += FString::Printf(TEXT("Namespace: `%s`\n\n"), *Reg.Category.ToLower());
            Out += MarkdownHelpers::EscapeMarkdownText(Reg.Summary);
            Out += TEXT("\n\n");
            Out += MarkdownHelpers::RenderParamList(Reg.Params);

            const FString MethodSection = WikiOverlay::LoadMethodSection(Reg.MethodName);
            if (!MethodSection.IsEmpty())
            {
                Out += TEXT("\n## Notes\n\n");
                Out += MethodSection;
                Out += TEXT("\n");
            }
            return Out;
        }

        FString RenderNotFound(const FString& OriginalQuery,
                               const TArray<FString>& Suggestions)
        {
            FString Out = FString::Printf(TEXT("# Not found: `%s`\n\n"), *OriginalQuery);
            if (Suggestions.Num() == 0)
            {
                Out += TEXT("No close matches. Call `call()` with no path to see the namespace index.\n");
                return Out;
            }
            Out += TEXT("Did you mean:\n\n");
            for (const FString& S : Suggestions)
            {
                Out += FString::Printf(TEXT("- `%s`\n"), *S);
            }
            return Out;
        }
        // Build the leading H1 + optional overlay prelude block for a namespace page.
        // Curated context comes first, between the heading and the auto-generated
        // subgroup / method index.
        FString RenderNamespaceHeader(const FString& Namespace, const FWikiCache& Cache)
        {
            FString Out = FString::Printf(TEXT("# `%s`\n\n"), *Namespace);

            // Tier lookup keys on the first dotted segment so nested namespace
            // pages inherit their top-level tier. EVERY namespace page gets a
            // Stability line: an unmapped namespace says so rather than staying
            // silent, because silence here reads as core against the root legend.
            int32 DotIdx = INDEX_NONE;
            Namespace.FindChar(TEXT('.'), DotIdx);
            const FString TopLevel = (DotIdx == INDEX_NONE) ? Namespace : Namespace.Left(DotIdx);
            const FString Tier = ResolveTier(Cache, TopLevel);
            if (Tier == TEXT("core"))
            {
                Out += TEXT("Stability: core — solid, primary surface.\n\n");
            }
            else if (Tier == TEXT("experimental"))
            {
                Out += TEXT("Stability: experimental — works but is less complete and still changing.\n\n");
            }
            else if (Tier == TEXT("internal"))
            {
                Out += TEXT("Stability: internal — plumbing, not intended for direct use.\n\n");
            }
            else
            {
                Out += TEXT("Stability: unclassified — this namespace has no maturity entry, so nothing here is promised. ")
                       TEXT("Treat it as less stable than experimental until it is classified.\n\n");
            }

            const FString Prelude = WikiOverlay::LoadGroupPrelude(Namespace);
            if (!Prelude.IsEmpty())
            {
                Out += Prelude;
                Out += TEXT("\n\n");
            }
            return Out;
        }

        // ParseInto strips the file's H1, so re-emit one using the slug verbatim —
        // the slug is the canonical title shape for a standalone topic page.
        FString RenderTopicPage(const FString& Slug)
        {
            FString Out = FString::Printf(TEXT("# %s\n\n"), *Slug);
            Out += WikiOverlay::LoadGroupPrelude(Slug);
            return Out;
        }

        // The live subsystem's dispatcher, or null when the editor subsystem or its
        // dispatcher is not yet available (e.g. early startup). Single source for the
        // subsystem->dispatcher hop the parameterless public overloads share.
        TSharedPtr<FRpcDispatcher> GetLiveDispatcher()
        {
            UPinWrightSubsystem* Subsystem =
                GEditor ? GEditor->GetEditorSubsystem<UPinWrightSubsystem>() : nullptr;
            return Subsystem ? Subsystem->GetDispatcher() : nullptr;
        }
    } // anonymous namespace
} // namespace WikiHandler

namespace WikiHandler
{
    bool RenderPage(const FString& OriginalPath, FString& OutMarkdown)
    {
        OutMarkdown.Reset();

        const TSharedPtr<FRpcDispatcher> Dispatcher = GetLiveDispatcher();
        if (!Dispatcher.IsValid())
        {
            return false;
        }

        return RenderPage(*Dispatcher, OriginalPath, OutMarkdown);
    }

    bool RenderPage(const FRpcDispatcher& Dispatcher, const FString& OriginalPath, FString& OutMarkdown)
    {
        OutMarkdown.Reset();

        const FWikiCache& Cache = GetWikiCache(Dispatcher);

        if (OriginalPath.IsEmpty())
        {
            OutMarkdown = RenderRoot(Cache);
            return true;
        }

        const FString Normalized = ResolveCanonicalLower(OriginalPath, Cache);
        const EWikiNodeKind Kind = ClassifyNode(Normalized, Cache);

        if (Kind == EWikiNodeKind::Method)
        {
            // Classification proved this exists; the find cannot miss.
            OutMarkdown = RenderMethodPage(**Cache.MethodsByLowerName.Find(Normalized));
        }
        else if (Kind == EWikiNodeKind::Branch)
        {
            OutMarkdown = RenderNamespaceHeader(Normalized, Cache);
            OutMarkdown += RenderChildIndex(ImmediateChildren(Normalized, Cache));
        }
        else if (Kind == EWikiNodeKind::LeafNamespace)
        {
            OutMarkdown = RenderNamespaceHeader(Normalized, Cache);
            OutMarkdown += RenderMethodList(Normalized, Cache);
        }
        else if (Kind == EWikiNodeKind::Hybrid)
        {
            OutMarkdown = RenderNamespaceHeader(Normalized, Cache);
            OutMarkdown += RenderChildIndex(ImmediateChildren(Normalized, Cache));
            OutMarkdown += RenderMethodList(Normalized, Cache);
        }
        else if (Kind == EWikiNodeKind::Topic)
        {
            OutMarkdown = RenderTopicPage(Normalized);
            // A namespace owned by a skipped integration sub-module registers no
            // handlers, so its wiki-src overlay renders as a plain topic page —
            // banner why the methods are missing.
            const FString DisabledPlugin = IntegrationGates::FindSkippedByNamespace(Normalized);
            if (!DisabledPlugin.IsEmpty())
            {
                OutMarkdown = FString::Printf(
                    TEXT("> **Unavailable:** this namespace is unavailable in this project because the '%s' engine plugin is disabled; enable it and restart the editor.\n\n"),
                    *DisabledPlugin) + OutMarkdown;
            }
        }
        else
        {
            const TArray<FString> Suggestions = SuggestionHelpers::RankSuggestions(Normalized, Cache.SuggestionPool, 5);
            OutMarkdown = RenderNotFound(OriginalPath, Suggestions);
        }

        return true;
    }

    FString NormalizeSlug(const FRpcDispatcher& Dispatcher, const FString& In)
    {
        // Resolve through the same cache-aware path RenderPage uses so the on-disk
        // page filename the transport probes (game_framework.md) matches the slug
        // EnumerateAllSlugs emitted and the generator wrote. Without this, a real
        // underscore namespace would be collapsed to game.framework, miss the file,
        // and fall into the cold-start re-mangle.
        return ResolveCanonicalLower(In, GetWikiCache(Dispatcher));
    }

    FString NormalizeSlug(const FString& In)
    {
        if (const TSharedPtr<FRpcDispatcher> Dispatcher = GetLiveDispatcher())
        {
            return NormalizeSlug(*Dispatcher, In);
        }
        // No live dispatcher (e.g. early startup): fall back to the pure legacy
        // collapse so the slug is still well-formed.
        return NormalizeQuery(In);
    }

    bool GetRegistryStats(TArray<FRegistryNamespaceStat>& OutNamespaces, int32& OutOperations)
    {
        OutNamespaces.Reset();
        OutOperations = 0;

        const TSharedPtr<FRpcDispatcher> Dispatcher = GetLiveDispatcher();
        if (!Dispatcher.IsValid())
        {
            return false;
        }

        // MethodsByCategory is keyed on the registered Category (already lowercased
        // on ingest), so reading it here is what keeps a method like
        // sequence.add_keyframe counted under its real "sequencer" namespace.
        const FWikiCache& Cache = GetWikiCache(*Dispatcher);

        // Fold nested categories into their root: "audio.metasound" counts toward "audio".
        TMap<FString, int32> CountsBySlug;
        for (const auto& KV : Cache.MethodsByCategory)
        {
            int32 DotIdx = INDEX_NONE;
            KV.Key.FindChar(TEXT('.'), DotIdx);
            const FString TopLevel = (DotIdx == INDEX_NONE) ? KV.Key : KV.Key.Left(DotIdx);
            if (TopLevel == TEXT("_test"))
            {
                continue;
            }
            CountsBySlug.FindOrAdd(TopLevel) += KV.Value.Num();
        }

        OutNamespaces.Reserve(CountsBySlug.Num());
        for (const auto& KV : CountsBySlug)
        {
            FRegistryNamespaceStat Stat;
            Stat.Slug = KV.Key;
            Stat.Methods = KV.Value;
            // Never empty: an unmapped namespace publishes "unclassified" so a
            // downstream consumer of registry.json reads the omission as a tier
            // rather than as a blank it is free to interpret as core.
            Stat.Tier = ResolveTier(Cache, KV.Key);
            OutNamespaces.Add(MoveTemp(Stat));
            OutOperations += KV.Value;
        }

        // TMap iteration order is arbitrary; sort so the emitted manifest is byte-stable.
        Algo::Sort(OutNamespaces, [](const FRegistryNamespaceStat& A, const FRegistryNamespaceStat& B)
        {
            return A.Slug < B.Slug;
        });
        return true;
    }

    bool EnumerateAllSlugs(TArray<FString>& OutSlugs)
    {
        OutSlugs.Reset();

        const TSharedPtr<FRpcDispatcher> Dispatcher = GetLiveDispatcher();
        if (!Dispatcher.IsValid())
        {
            return false;
        }

        return EnumerateAllSlugs(*Dispatcher, OutSlugs);
    }

    bool EnumerateAllSlugs(const FRpcDispatcher& Dispatcher, TArray<FString>& OutSlugs)
    {
        OutSlugs.Reset();

        const FWikiCache& Cache = GetWikiCache(Dispatcher);

        TSet<FString> Slugs;
        Slugs.Append(Cache.AllNodes);
        Slugs.Append(Cache.CategoryNodes);
        for (const auto& KV : Cache.MethodsByLowerName)
        {
            Slugs.Add(KV.Key);
        }
        // Topic pages (including the "wiki" agent usage guide) are already in
        // TopicNodes; the root index is not a path-addressable slug, so it is
        // materialized separately by the generator and not enumerated here.
        Slugs.Append(Cache.TopicNodes);

        OutSlugs = Slugs.Array();
        OutSlugs.Sort();
        return true;
    }
} // namespace WikiHandler
