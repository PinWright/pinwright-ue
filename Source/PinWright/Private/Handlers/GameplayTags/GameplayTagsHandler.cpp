// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "ScopedTransaction.h"

namespace
{
FName NormalizeIniSourceName(const FString& Source)
{
    if (Source.IsEmpty())
    {
        return NAME_None;
    }

    return Source.EndsWith(TEXT(".ini")) ? FName(*Source) : FName(*(Source + TEXT(".ini")));
}

FString SourceTypeToString(EGameplayTagSourceType SourceType)
{
    switch (SourceType)
    {
    case EGameplayTagSourceType::Native:
        return TEXT("native");
    case EGameplayTagSourceType::DefaultTagList:
        return TEXT("default_ini");
    case EGameplayTagSourceType::TagList:
        return TEXT("ini");
    case EGameplayTagSourceType::RestrictedTagList:
        return TEXT("restricted_ini");
    case EGameplayTagSourceType::DataTable:
        return TEXT("data_table");
    default:
        return TEXT("invalid");
    }
}

bool GetTagEditorData(
    UGameplayTagsManager& Manager,
    const FName TagName,
    FString& OutComment,
    TArray<FName>& OutSources,
    bool& bOutIsExplicit,
    bool* bOutIsRestricted = nullptr)
{
    bool bIsRestrictedTag = false;
    bool bAllowNonRestrictedChildren = true;
    const bool bFound = Manager.GetTagEditorData(
        TagName,
        OutComment,
        OutSources,
        bOutIsExplicit,
        bIsRestrictedTag,
        bAllowNonRestrictedChildren);
    if (bOutIsRestricted)
    {
        *bOutIsRestricted = bIsRestrictedTag;
    }
    return bFound;
}

TSharedPtr<FJsonObject> MakeTagRow(
    UGameplayTagsManager& Manager,
    const FString& TagName,
    const FString& Comment,
    const FName SourceName,
    const bool bIsExplicit,
    const bool bWantName,
    const bool bWantSource,
    const bool bWantComment,
    const bool bWantIsExplicit,
    const bool bWantSourceType,
    const bool bWantConfigFile)
{
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    if (bWantName)
    {
        Row->SetStringField(TEXT("name"), TagName);
    }
    if (bWantSource)
    {
        Row->SetStringField(TEXT("source"), SourceName.ToString());
    }
    if (bWantComment)
    {
        Row->SetStringField(TEXT("comment"), Comment);
    }
    if (bWantIsExplicit)
    {
        Row->SetBoolField(TEXT("isExplicit"), bIsExplicit);
    }

    // FindTagSource is only worth calling when at least one source-derived column
    // (the byte-dominating configFile / sourceType) is actually projected.
    if (bWantSourceType || bWantConfigFile)
    {
        if (const FGameplayTagSource* Source = Manager.FindTagSource(SourceName))
        {
            if (bWantSourceType)
            {
                Row->SetStringField(TEXT("sourceType"), SourceTypeToString(Source->SourceType));
            }
            if (bWantConfigFile)
            {
                Row->SetStringField(TEXT("configFile"), Source->GetConfigFileName());
            }
        }
    }

    return Row;
}

struct FGameplayTagListCandidate
{
    FString TagName;
    FString Comment;
    FName SourceName;
    bool bIsExplicit = false;
};

bool GameplayTagListCandidateLess(const FGameplayTagListCandidate& Left, const FGameplayTagListCandidate& Right)
{
    if (Left.TagName != Right.TagName)
    {
        return Left.TagName < Right.TagName;
    }

    return Left.SourceName.ToString() < Right.SourceName.ToString();
}

bool SourceMatches(const TArray<FName>& Sources, const FName SourceFilter)
{
    return SourceFilter.IsNone() || Sources.Contains(SourceFilter);
}

bool AddTagToSpecificSource(UGameplayTagsManager& Manager, const FName TagName, const FName SourceName, const FString& Comment)
{
    FGameplayTagSource* TagSource = Manager.FindTagSource(SourceName);
    if (!TagSource)
    {
        if (!IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(SourceName.ToString(), FString()))
        {
            return false;
        }
        TagSource = Manager.FindTagSource(SourceName);
    }
    if (!TagSource || !TagSource->SourceTagList)
    {
        return false;
    }

    UGameplayTagsList* TagList = TagSource->SourceTagList;
    TagList->Modify();
    TagList->GameplayTagList.AddUnique(FGameplayTagTableRow(TagName, Comment));
    TagList->SortTags();
    TagList->TryUpdateDefaultConfigFile(TagList->ConfigFileName);
    GConfig->LoadFile(TagList->ConfigFileName);
    Manager.EditorRefreshGameplayTagTree();
    return true;
}

bool RemoveTagFromSpecificSource(UGameplayTagsManager& Manager, const FName TagName, const FName SourceName, const bool bIsRestricted)
{
    const FGameplayTagSource* TagSource = Manager.FindTagSource(SourceName);
    if (!TagSource)
    {
        return false;
    }

    UObject* TagListObject = nullptr;
    FString ConfigFileName;
    int32 NumRemoved = 0;

    if (bIsRestricted)
    {
        URestrictedGameplayTagsList* RestrictedTagList = TagSource->SourceRestrictedTagList;
        if (!RestrictedTagList)
        {
            return false;
        }

        RestrictedTagList->Modify();
        NumRemoved = RestrictedTagList->RestrictedGameplayTagList.RemoveAll([TagName](const FRestrictedGameplayTagTableRow& Row)
        {
            return Row.Tag == TagName;
        });
        TagListObject = RestrictedTagList;
        ConfigFileName = RestrictedTagList->ConfigFileName;
    }
    else
    {
        UGameplayTagsList* TagList = TagSource->SourceTagList;
        if (!TagList)
        {
            return false;
        }

        TagList->Modify();
        NumRemoved = TagList->GameplayTagList.RemoveAll([TagName](const FGameplayTagTableRow& Row)
        {
            return Row.Tag == TagName;
        });
        TagListObject = TagList;
        ConfigFileName = TagList->ConfigFileName;
    }

    if (NumRemoved <= 0 || !TagListObject)
    {
        return false;
    }

    TagListObject->TryUpdateDefaultConfigFile(ConfigFileName);
    GConfig->LoadFile(ConfigFileName);
    Manager.EditorRefreshGameplayTagTree();
    return true;
}
}

REGISTER_RPC_HANDLER("gameplay_tags.add", "gameplay_tags", "Add an explicit gameplay tag to an INI-backed tag source",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Dotted gameplay tag name, for example Ability.Attack.Melee"),
        RPC_PARAM_OPT("source", "string", "INI tag source filename; .ini is appended when omitted"),
        RPC_PARAM_OPT("comment", "string", "Developer comment stored with the tag")
    ))
{
    FString Tag;
    if (!Ctx.RequireString(TEXT("tag"), Tag))
    {
        return true;
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    FText ErrorText;
    FString FixedString;
    if (!Manager.IsValidGameplayTagString(Tag, &ErrorText, &FixedString))
    {
        Ctx.SendError(
            TEXT("INVALID_TAG"),
            FString::Printf(TEXT("Invalid gameplay tag '%s': %s"), *Tag, *ErrorText.ToString()));
        return true;
    }

    const FName TagName(*Tag);
    const FName RequestedSource = NormalizeIniSourceName(Ctx.GetString(TEXT("source")));
    const FString Comment = Ctx.GetString(TEXT("comment"));

    FString ExistingComment;
    TArray<FName> ExistingSources;
    bool bWasExplicit = false;
    GetTagEditorData(Manager, TagName, ExistingComment, ExistingSources, bWasExplicit);

    const bool bAlreadyExisted = bWasExplicit && SourceMatches(ExistingSources, RequestedSource);
    if (!bAlreadyExisted)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Add Gameplay Tag")));
        bool bAdded = false;
        if (!RequestedSource.IsNone() && bWasExplicit)
        {
            bAdded = AddTagToSpecificSource(Manager, TagName, RequestedSource, Comment);
        }
        else
        {
            bAdded = IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(Tag, Comment, RequestedSource);
        }
        if (!bAdded)
        {
            Ctx.SendError(TEXT("ADD_FAILED"), FString::Printf(TEXT("Failed to add gameplay tag: %s"), *Tag));
            return true;
        }
    }

    FString StoredComment;
    TArray<FName> Sources;
    bool bIsExplicit = false;
    GetTagEditorData(Manager, TagName, StoredComment, Sources, bIsExplicit);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tag"), Tag);
    Result->SetStringField(TEXT("source"), !RequestedSource.IsNone() ? RequestedSource.ToString() : (Sources.Num() > 0 ? Sources[0].ToString() : FString()));
    Result->SetStringField(TEXT("comment"), StoredComment.IsEmpty() ? Comment : StoredComment);
    Result->SetBoolField(TEXT("alreadyExisted"), bAlreadyExisted);
    Result->SetBoolField(TEXT("isExplicit"), bIsExplicit);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("gameplay_tags.remove", "gameplay_tags", "Remove an explicit gameplay tag from an INI-backed tag source",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Dotted gameplay tag name to remove"),
        RPC_PARAM_OPT("source", "string", "Expected INI tag source filename; .ini is appended when omitted")
    ))
{
    FString Tag;
    if (!Ctx.RequireString(TEXT("tag"), Tag))
    {
        return true;
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    const FName TagName(*Tag);
    const FName SourceFilter = NormalizeIniSourceName(Ctx.GetString(TEXT("source")));

    FString Comment;
    TArray<FName> Sources;
    bool bIsExplicit = false;
    bool bIsRestricted = false;
    if (!GetTagEditorData(Manager, TagName, Comment, Sources, bIsExplicit, &bIsRestricted) || !bIsExplicit || !SourceMatches(Sources, SourceFilter))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("tag"), Tag);
        Result->SetStringField(TEXT("source"), SourceFilter.ToString());
        Result->SetBoolField(TEXT("removed"), false);
        Ctx.SendSuccess(Result);
        return true;
    }

    if (SourceFilter.IsNone() && Sources.Num() > 1)
    {
        Ctx.SendError(TEXT("AMBIGUOUS_SOURCE"), FString::Printf(TEXT("Gameplay tag '%s' exists in multiple sources; pass source."), *Tag));
        return true;
    }

    bool bRemoved = false;
    if (!SourceFilter.IsNone() && Sources.Num() > 1)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Remove Gameplay Tag Source Entry")));
        bRemoved = RemoveTagFromSpecificSource(Manager, TagName, SourceFilter, bIsRestricted);
    }
    else
    {
        TSharedPtr<FGameplayTagNode> TagNode = Manager.FindTagNode(TagName);
        const FScopedTransaction Transaction(FText::FromString(TEXT("Remove Gameplay Tag")));
        bRemoved = IGameplayTagsEditorModule::Get().DeleteTagFromINI(TagNode);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tag"), Tag);
    Result->SetStringField(TEXT("source"), !SourceFilter.IsNone() ? SourceFilter.ToString() : (Sources.Num() > 0 ? Sources[0].ToString() : FString()));
    Result->SetBoolField(TEXT("removed"), bRemoved);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("gameplay_tags.list", "gameplay_tags", "List gameplay tags with source, comment, and explicitness metadata",
    RPC_PARAMS(
        RPC_PARAM_OPT("prefix", "string", "Only include tags whose name starts with this prefix"),
        RPC_PARAM_OPT("source", "string", "Only include tags from this INI source filename"),
        RPC_PARAM_DEF("includeImplicitParents", "boolean", "Include implicit parent tags", "true"),
        RPC_PARAM_DEF("limit", "number", "Maximum rows returned (clamped to [1, 500]). Default 50 keeps an un-prefixed overview inline; totalMatches always reports the full untruncated count so elision stays detectable.", "50"),
        RPC_PARAM_DEF("includeTotal", "boolean", "Continue scanning after limit to return exact totalMatches", "false"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-row keys to return (valid keys: name, source, comment, isExplicit, sourceType, configFile); e.g. [\"name\"] to drop the byte-dominating per-tag configFile path. Omit for all keys. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "bool", "When true, drops the byte-dominating per-row `configFile` path and `sourceType` (and `comment`) — keeps name+source+isExplicit — shorthand for the common 'just show me the tag names' overview that overflows the inline budget. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    const FString Prefix = Ctx.GetString(TEXT("prefix"));
    const FName SourceFilter = NormalizeIniSourceName(Ctx.GetString(TEXT("source")));
    const bool bIncludeImplicitParents = Ctx.GetBool(TEXT("includeImplicitParents"), true);
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), 50), 1, 500);
    const bool bIncludeTotal = Ctx.GetBool(TEXT("includeTotal"), false);

    // Per-row field projection: an explicit `fields` allow-list (array or bare
    // string) wins; otherwise namesOnly expands to name+source+isExplicit, dropping
    // the byte-dominating per-tag configFile path + sourceType + comment that push
    // the un-prefixed overview past the inline budget. An empty set means "no
    // projection" — the unprojected output is byte-identical to the prior shape.
    // Shared with actor.list / system.console.search via ReadFieldProjection.
    const TSet<FString> Fields = Ctx.ReadFieldProjection(
        {TEXT("name"), TEXT("source"), TEXT("isexplicit")});
    // Resolve each key to a bool once (the wanted-set never changes per row) instead
    // of probing the TSet — with a throwaway FString — for every emitted row. An empty
    // set means "no projection": want everything. The probe keys are lowercase to match
    // the lowercased set ReadFieldProjection returns.
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key)
    {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantName = Wants(TEXT("name"));
    const bool bWantSource = Wants(TEXT("source"));
    const bool bWantComment = Wants(TEXT("comment"));
    const bool bWantIsExplicit = Wants(TEXT("isexplicit"));
    const bool bWantSourceType = Wants(TEXT("sourcetype"));
    const bool bWantConfigFile = Wants(TEXT("configfile"));

    FGameplayTagContainer Container;
    Manager.RequestAllGameplayTags(Container, !bIncludeImplicitParents);

    TArray<FGameplayTagListCandidate> Candidates;
    Candidates.Reserve(Limit);
    bool bStoppedAtLimit = false;
    for (const FGameplayTag& GameplayTag : Container.GetGameplayTagArray())
    {
        const FString TagName = GameplayTag.ToString();
        if (!Prefix.IsEmpty() && !TagName.StartsWith(Prefix))
        {
            continue;
        }

        FString Comment;
        TArray<FName> Sources;
        bool bIsExplicit = false;
        if (!GetTagEditorData(Manager, GameplayTag.GetTagName(), Comment, Sources, bIsExplicit))
        {
            continue;
        }

        if (!SourceMatches(Sources, SourceFilter))
        {
            continue;
        }

        if (Sources.Num() == 0)
        {
            Candidates.Add({TagName, Comment, NAME_None, bIsExplicit});
            if (!bIncludeTotal && Candidates.Num() >= Limit)
            {
                bStoppedAtLimit = true;
                break;
            }
            continue;
        }

        for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
        {
            const FName SourceName = Sources[SourceIndex];
            if (!SourceFilter.IsNone() && SourceName != SourceFilter)
            {
                continue;
            }

            Candidates.Add({TagName, Comment, SourceName, bIsExplicit});
            if (!bIncludeTotal && Candidates.Num() >= Limit)
            {
                bStoppedAtLimit = true;
                break;
            }
        }

        if (bStoppedAtLimit)
        {
            break;
        }
    }

    Candidates.Sort(GameplayTagListCandidateLess);

    TArray<TSharedPtr<FJsonValue>> Rows;
    const int32 TotalMatches = Candidates.Num();
    const int32 RowsToReturn = FMath::Min(Limit, Candidates.Num());
    for (int32 CandidateIndex = 0; CandidateIndex < RowsToReturn; ++CandidateIndex)
    {
        const FGameplayTagListCandidate& Candidate = Candidates[CandidateIndex];
        Rows.Add(MakeShared<FJsonValueObject>(MakeTagRow(
            Manager, Candidate.TagName, Candidate.Comment, Candidate.SourceName, Candidate.bIsExplicit,
            bWantName, bWantSource, bWantComment, bWantIsExplicit, bWantSourceType, bWantConfigFile)));
    }

    const bool bTotalMatchesExact = bIncludeTotal || !bStoppedAtLimit;
    const bool bTruncated = !bTotalMatchesExact || TotalMatches > Rows.Num();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("tags"), Rows);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Result->SetBoolField(TEXT("totalMatchesExact"), bTotalMatchesExact);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("gameplay_tags.add_source", "gameplay_tags", "Add an INI gameplay tag source",
    RPC_PARAMS(
        RPC_PARAM_REQ("source", "string", "INI source filename; .ini is appended when omitted"),
        RPC_PARAM_DEF("kind", "string", "Only ini is supported for registry mutation", "ini"),
        RPC_PARAM_OPT("searchPath", "filepath", "Root directory to use for the source")
    ))
{
    FString Source;
    if (!Ctx.RequireString(TEXT("source"), Source))
    {
        return true;
    }

    const FString Kind = Ctx.GetString(TEXT("kind"), TEXT("ini"));
    if (!Kind.Equals(TEXT("ini"), ESearchCase::IgnoreCase))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_SOURCE_KIND"), TEXT("gameplay_tags.add_source currently supports only INI tag sources."));
        return true;
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    const FName SourceName = NormalizeIniSourceName(Source);
    const bool bAlreadyExisted = Manager.FindTagSource(SourceName) != nullptr;

    if (!bAlreadyExisted)
    {
        const FScopedTransaction Transaction(FText::FromString(TEXT("Add Gameplay Tag Source")));
        if (!IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(SourceName.ToString(), Ctx.GetString(TEXT("searchPath"))))
        {
            Ctx.SendError(TEXT("ADD_SOURCE_FAILED"), FString::Printf(TEXT("Failed to add gameplay tag source: %s"), *SourceName.ToString()));
            return true;
        }
    }

    const FGameplayTagSource* AddedSource = Manager.FindTagSource(SourceName);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), SourceName.ToString());
    Result->SetStringField(TEXT("kind"), TEXT("ini"));
    Result->SetBoolField(TEXT("created"), !bAlreadyExisted && AddedSource != nullptr);
    if (AddedSource)
    {
        Result->SetStringField(TEXT("sourceType"), SourceTypeToString(AddedSource->SourceType));
        Result->SetStringField(TEXT("configFile"), AddedSource->GetConfigFileName());
    }
    Ctx.SendSuccess(Result);
    return true;
}
