// Copyright (c) 2026 Alexander Penkin. MIT License.

// MetaSoundSearchHandler.cpp
// Implements audio.authoring.search_metasound_nodes — a catalog RPC that walks the
// MetaSound Frontend class registry and returns filtered, scored results.
// Scoped entirely to Private/Handlers/Audio/MetaSound/ per sprint constraint.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Compat/EngineVersionCompat.h"


#if __has_include("MetasoundFrontendSearchEngine.h")
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundFrontendDocument.h"
// FMetaSoundClassInfo (used by the 5.8+ FindAllClasses overload) is only
// forward-declared by the search-engine header; its definition lives here.
#include "MetasoundFrontendQuery.h"
#define MCP_HAS_METASOUND_SEARCH_ENGINE 1
#else
#define MCP_HAS_METASOUND_SEARCH_ENGINE 0
#endif


REGISTER_RPC_HANDLER("audio.authoring.search_metasound_nodes", "audio.authoring",
    "Search the MetaSound Frontend class registry. Returns node classes matching the "
    "optional query/category/version filters with a relevance score. Requires MetaSound "
    "frontend search support.",
    RPC_PARAMS(
        RPC_PARAM_OPT("query",            "string", "Case-insensitive substring matched against ClassName and DisplayName. Omit for all."),
        RPC_PARAM_OPT("category",         "string", "Case-insensitive substring matched against the joined category hierarchy (e.g. \"Math\", \"Filters\")."),
        RPC_PARAM_OPT("versionMajor",     "integer", "Exact major version to match. Both versionMajor and versionMinor must be present to filter by version."),
        RPC_PARAM_OPT("versionMinor",     "integer", "Exact minor version to match. Both versionMajor and versionMinor must be present to filter by version."),
        RPC_PARAM_DEF("includeDeprecated","bool",    "Include deprecated node classes in results.", "false"),
        RPC_PARAM_DEF("limit",            "integer", "Maximum number of results to return.", "50")
    ))
{
#if MCP_HAS_METASOUND_SEARCH_ENGINE

    // --- Read params ---
    const FString QueryStr    = Ctx.GetString(TEXT("query"),    TEXT(""));
    const FString CategoryStr = Ctx.GetString(TEXT("category"), TEXT(""));
    const bool bIncludeDeprecated = Ctx.GetBool(TEXT("includeDeprecated"), false);
    const int32 Limit         = FMath::Clamp(Ctx.GetInt(TEXT("limit"), 50), 1, 10000);

    // Version filter: only active when BOTH major and minor are supplied.
    const int32 RawMajor = Ctx.GetInt(TEXT("versionMajor"), -1);
    const int32 RawMinor = Ctx.GetInt(TEXT("versionMinor"), -1);
    const bool bFilterVersion = (RawMajor >= 0 && RawMinor >= 0);

    // --- Fetch all registered classes ---
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    // 5.8 deprecated FindAllClasses(bool) returning full class definitions. Enumerate the
    // lightweight class infos, then resolve each unique class name back to its full
    // definitions via the non-deprecated FindClassesWithName(Name, bSortByVersion) overload.
    TArray<FMetasoundFrontendClass> AllClasses;
    {
        using namespace Metasound::Frontend;
        ISearchEngine& Engine = ISearchEngine::Get();
        const TArray<FMetaSoundClassInfo> Infos =
            Engine.FindAllClasses(ISearchEngine::EResultVersion::All);
        TSet<FString> SeenNames;
        for (const FMetaSoundClassInfo& Info : Infos)
        {
            bool bAlreadySeen = false;
            SeenNames.Add(Info.ClassName.ToString(), &bAlreadySeen);
            if (!bAlreadySeen)
            {
                AllClasses.Append(Engine.FindClassesWithName(Info.ClassName, /*bInSortByVersion=*/false));
            }
        }
    }
#else
    TArray<FMetasoundFrontendClass> AllClasses =
        Metasound::Frontend::ISearchEngine::Get().FindAllClasses(true);
#endif

    // --- Per-result scored entry ---
    struct FScoredEntry
    {
        const FMetasoundFrontendClass* Class = nullptr;
        int32 Score = 0;
        FString CategoryJoined;   // pre-computed; reused for filter and JSON output
        FString SortKey;          // pre-computed ClassName string for stable sort
    };

    TArray<FScoredEntry> Scored;
    Scored.Reserve(AllClasses.Num());

    const FString QueryLower    = QueryStr.ToLower();
    const FString CategoryLower = CategoryStr.ToLower();

    for (const FMetasoundFrontendClass& Class : AllClasses)
    {
        const FMetasoundFrontendClassMetadata& Meta = Class.Metadata;

        // --- Deprecated filter ---
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        if (!bIncludeDeprecated && EnumHasAnyFlags(Meta.GetAccessFlags(), EMetasoundFrontendClassAccessFlags::Deprecated))
#else
        if (!bIncludeDeprecated && Meta.GetIsDeprecated())
#endif
        {
            continue;
        }

        // --- Version filter ---
        if (bFilterVersion)
        {
            const FMetasoundFrontendVersionNumber& Ver = Meta.GetVersion();
            if (Ver.Major != RawMajor || Ver.Minor != RawMinor)
            {
                continue;
            }
        }

        // --- Build category join (used for filter and JSON output) ---
        FString CategoryJoined;
        for (const FText& Cat : Meta.GetCategoryHierarchy())
        {
            if (!CategoryJoined.IsEmpty()) CategoryJoined.AppendChar(TEXT('.'));
            CategoryJoined.Append(Cat.ToString());
        }

        // --- Category filter ---
        if (!CategoryLower.IsEmpty())
        {
            if (!CategoryJoined.ToLower().Contains(CategoryLower))
            {
                continue;
            }
        }

        // --- Score on query ---
        int32 Score = 0;
        if (!QueryLower.IsEmpty())
        {
            const FString ClassNameStr    = Meta.GetClassName().ToString().ToLower();
            const FString DisplayNameStr  = Meta.GetDisplayName().ToString().ToLower();

            if (ClassNameStr == QueryLower || DisplayNameStr == QueryLower)
            {
                // Exact match — highest priority.
                Score = (ClassNameStr == QueryLower) ? 10 : 8;
            }
            else if (ClassNameStr.StartsWith(QueryLower) || DisplayNameStr.StartsWith(QueryLower))
            {
                Score = 5;
            }
            else if (ClassNameStr.Contains(QueryLower) || DisplayNameStr.Contains(QueryLower))
            {
                Score = 1;
            }
            else
            {
                // No match — skip.
                continue;
            }
        }
        else
        {
            // No query — include everything that passed earlier filters.
            Score = 0;
        }

        FScoredEntry Entry;
        Entry.Class = &Class;
        Entry.Score = Score;
        Entry.CategoryJoined = MoveTemp(CategoryJoined);
        Entry.SortKey = Meta.GetClassName().ToString();
        Scored.Add(Entry);
    }

    // --- Sort descending by score, then stable by ClassName for deterministic output ---
    Scored.Sort([](const FScoredEntry& A, const FScoredEntry& B)
    {
        if (A.Score != B.Score) return A.Score > B.Score;
        return A.SortKey < B.SortKey;
    });

    const int32 TotalMatches = Scored.Num();

    // --- Build JSON results ---
    TArray<TSharedPtr<FJsonValue>> ResultsArray;
    const int32 NumToReturn = FMath::Min(Limit, TotalMatches);

    for (int32 i = 0; i < NumToReturn; ++i)
    {
        const FMetasoundFrontendClass& Class = *Scored[i].Class;
        const FMetasoundFrontendClassMetadata& Meta = Class.Metadata;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

        // Identity
        Entry->SetStringField(TEXT("className"),   Meta.GetClassName().ToString());
        Entry->SetStringField(TEXT("displayName"), Meta.GetDisplayName().ToString());
        Entry->SetStringField(TEXT("description"), Meta.GetDescription().ToString());
        Entry->SetStringField(TEXT("author"),      Meta.GetAuthor());

        // Category (joined by ".")
        Entry->SetStringField(TEXT("category"), Scored[i].CategoryJoined);

        // Version
        {
            TSharedPtr<FJsonObject> VerObj = MakeShared<FJsonObject>();
            VerObj->SetNumberField(TEXT("major"), Meta.GetVersion().Major);
            VerObj->SetNumberField(TEXT("minor"), Meta.GetVersion().Minor);
            Entry->SetObjectField(TEXT("version"), VerObj);
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        Entry->SetBoolField(TEXT("deprecated"), EnumHasAnyFlags(Meta.GetAccessFlags(), EMetasoundFrontendClassAccessFlags::Deprecated));
#else
        Entry->SetBoolField(TEXT("deprecated"), Meta.GetIsDeprecated());
#endif

        // Inputs
        {
            TArray<TSharedPtr<FJsonValue>> InputsArr;
            // 5.6 deprecated Class.Interface in favor of GetDefaultInterface(); helper picks per version.
            for (const FMetasoundFrontendClassInput& Input : PinWright::MetaSound::GetClassDefaultInterface(Class).Inputs)
            {
                TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
                Pin->SetStringField(TEXT("name"), Input.Name.ToString());
                Pin->SetStringField(TEXT("type"), Input.TypeName.ToString());
                InputsArr.Add(MakeShared<FJsonValueObject>(Pin));
            }
            Entry->SetArrayField(TEXT("inputs"), InputsArr);
        }

        // Outputs
        {
            TArray<TSharedPtr<FJsonValue>> OutputsArr;
            for (const FMetasoundFrontendClassOutput& Output : PinWright::MetaSound::GetClassDefaultInterface(Class).Outputs)
            {
                TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
                Pin->SetStringField(TEXT("name"), Output.Name.ToString());
                Pin->SetStringField(TEXT("type"), Output.TypeName.ToString());
                OutputsArr.Add(MakeShared<FJsonValueObject>(Pin));
            }
            Entry->SetArrayField(TEXT("outputs"), OutputsArr);
        }

        Entry->SetNumberField(TEXT("score"), Scored[i].Score);

        ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), ResultsArray);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Result);

#else
    // MetaSound Frontend search engine not available on this engine version or configuration.
    Ctx.SendError(TEXT("METASOUND_SEARCH_NOT_AVAILABLE"),
        TEXT("MetaSound frontend search engine not available on this UE version"));
#endif // MCP_HAS_METASOUND_SEARCH_ENGINE

    return true;
}
