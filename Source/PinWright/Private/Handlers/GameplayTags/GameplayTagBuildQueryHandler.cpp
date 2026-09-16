// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Utils/PropertyUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ScopedTransaction.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/UnrealType.h"

// Named namespace: Unity build ODR safety.
namespace GameplayTagBuildQueryHelpers
{
enum class EOpKind
{
    Unknown,
    AnyTagsMatch,
    AllTagsMatch,
    NoTagsMatch,
    AnyExprMatch,
    AllExprMatch,
    NoExprMatch,
};

EOpKind ParseOp(const FString& Op)
{
    if (Op.Equals(TEXT("any_tags_match"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("anyTagsMatch"), ESearchCase::IgnoreCase))
    {
        return EOpKind::AnyTagsMatch;
    }
    if (Op.Equals(TEXT("all_tags_match"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("allTagsMatch"), ESearchCase::IgnoreCase))
    {
        return EOpKind::AllTagsMatch;
    }
    if (Op.Equals(TEXT("no_tags_match"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("noTagsMatch"), ESearchCase::IgnoreCase))
    {
        return EOpKind::NoTagsMatch;
    }
    if (Op.Equals(TEXT("any_expressions_match"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("anyExpressionsMatch"), ESearchCase::IgnoreCase))
    {
        return EOpKind::AnyExprMatch;
    }
    if (Op.Equals(TEXT("all_expressions_match"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("allExpressionsMatch"), ESearchCase::IgnoreCase))
    {
        return EOpKind::AllExprMatch;
    }
    if (Op.Equals(TEXT("no_expressions_match"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("noExpressionsMatch"), ESearchCase::IgnoreCase))
    {
        return EOpKind::NoExprMatch;
    }
    return EOpKind::Unknown;
}

bool IsLeafOp(EOpKind Kind)
{
    return Kind == EOpKind::AnyTagsMatch || Kind == EOpKind::AllTagsMatch || Kind == EOpKind::NoTagsMatch;
}

bool IsCompositeOp(EOpKind Kind)
{
    return Kind == EOpKind::AnyExprMatch || Kind == EOpKind::AllExprMatch || Kind == EOpKind::NoExprMatch;
}

void ApplyOpKind(FGameplayTagQueryExpression& Expr, EOpKind Kind)
{
    switch (Kind)
    {
    case EOpKind::AnyTagsMatch: Expr.AnyTagsMatch(); break;
    case EOpKind::AllTagsMatch: Expr.AllTagsMatch(); break;
    case EOpKind::NoTagsMatch: Expr.NoTagsMatch(); break;
    case EOpKind::AnyExprMatch: Expr.AnyExprMatch(); break;
    case EOpKind::AllExprMatch: Expr.AllExprMatch(); break;
    case EOpKind::NoExprMatch: Expr.NoExprMatch(); break;
    default: break;
    }
}

bool BuildExpressionFromJson(
    const TSharedPtr<FJsonObject>& Node,
    FGameplayTagQueryExpression& Out,
    int32 Depth,
    int32 MaxDepth,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    if (Depth > MaxDepth)
    {
        OutErrorCode = TEXT("MAX_DEPTH_EXCEEDED");
        OutErrorMessage = FString::Printf(TEXT("Expression recursion exceeded maxDepth=%d."), MaxDepth);
        return false;
    }

    if (!Node.IsValid())
    {
        OutErrorCode = TEXT("UNKNOWN_OP");
        OutErrorMessage = TEXT("Expression node is missing or not an object.");
        return false;
    }

    FString Op;
    Node->TryGetStringField(TEXT("op"), Op);
    const EOpKind Kind = ParseOp(Op);
    if (Kind == EOpKind::Unknown)
    {
        OutErrorCode = TEXT("UNKNOWN_OP");
        OutErrorMessage = FString::Printf(TEXT("Unknown gameplay tag query op: '%s'."), *Op);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
    const bool bHasTags = Node->TryGetArrayField(TEXT("tags"), TagsArray) && TagsArray;
    const TArray<TSharedPtr<FJsonValue>>* ExprArray = nullptr;
    const bool bHasExprs = Node->TryGetArrayField(TEXT("expressions"), ExprArray) && ExprArray;

    if (IsLeafOp(Kind))
    {
        if (bHasExprs)
        {
            OutErrorCode = TEXT("MIXED_PAYLOAD");
            OutErrorMessage = FString::Printf(TEXT("Leaf op '%s' must not carry expressions."), *Op);
            return false;
        }
        if (!bHasTags || TagsArray->Num() == 0)
        {
            OutErrorCode = TEXT("EMPTY_TAGS");
            OutErrorMessage = FString::Printf(TEXT("Leaf op '%s' requires non-empty tags array."), *Op);
            return false;
        }

        ApplyOpKind(Out, Kind);

        UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
        for (const TSharedPtr<FJsonValue>& Value : *TagsArray)
        {
            FString TagString;
            if (!Value.IsValid() || !Value->TryGetString(TagString))
            {
                OutErrorCode = TEXT("UNKNOWN_TAG");
                OutErrorMessage = TEXT("Tag entries must be strings.");
                return false;
            }

            const FGameplayTag Resolved = Manager.RequestGameplayTag(FName(*TagString), false);
            if (!Resolved.IsValid())
            {
                OutErrorCode = TEXT("UNKNOWN_TAG");
                OutErrorMessage = FString::Printf(TEXT("Gameplay tag not registered: '%s'."), *TagString);
                return false;
            }
            Out.AddTag(Resolved);
        }
        return true;
    }

    if (IsCompositeOp(Kind))
    {
        if (bHasTags)
        {
            OutErrorCode = TEXT("MIXED_PAYLOAD");
            OutErrorMessage = FString::Printf(TEXT("Composite op '%s' must not carry tags."), *Op);
            return false;
        }
        if (!bHasExprs || ExprArray->Num() == 0)
        {
            OutErrorCode = TEXT("EMPTY_EXPRESSIONS");
            OutErrorMessage = FString::Printf(TEXT("Composite op '%s' requires non-empty expressions array."), *Op);
            return false;
        }

        ApplyOpKind(Out, Kind);

        for (const TSharedPtr<FJsonValue>& ChildValue : *ExprArray)
        {
            const TSharedPtr<FJsonObject>* ChildObject = nullptr;
            if (!ChildValue.IsValid() || !ChildValue->TryGetObject(ChildObject) || !ChildObject || !(*ChildObject).IsValid())
            {
                OutErrorCode = TEXT("UNKNOWN_OP");
                OutErrorMessage = TEXT("Expression children must be objects.");
                return false;
            }

            FGameplayTagQueryExpression Child;
            if (!BuildExpressionFromJson(*ChildObject, Child, Depth + 1, MaxDepth, OutErrorCode, OutErrorMessage))
            {
                return false;
            }
            Out.AddExpr(Child);
        }
        return true;
    }

    OutErrorCode = TEXT("UNKNOWN_OP");
    OutErrorMessage = FString::Printf(TEXT("Unhandled op kind for '%s'."), *Op);
    return false;
}

// FGameplayTagQuery::Build() does not populate AutoDescription (only the editor-side
// UEditableGameplayTagQuery path does). Synthesize a compact textual summary from the
// JSON expression so GetDescription() returns something meaningful when the caller
// omits the optional `description` parameter.
FString SynthesizeDescription(const TSharedPtr<FJsonObject>& Node, int32 Depth, int32 MaxDepth)
{
    if (!Node.IsValid() || Depth > MaxDepth)
    {
        return FString();
    }

    FString Op;
    Node->TryGetStringField(TEXT("op"), Op);
    const EOpKind Kind = ParseOp(Op);

    if (IsLeafOp(Kind))
    {
        const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
        TArray<FString> TagStrings;
        if (Node->TryGetArrayField(TEXT("tags"), TagsArray) && TagsArray)
        {
            for (const TSharedPtr<FJsonValue>& Value : *TagsArray)
            {
                FString TagString;
                if (Value.IsValid() && Value->TryGetString(TagString))
                {
                    TagStrings.Add(TagString);
                }
            }
        }
        return FString::Printf(TEXT("%s(%s)"), *Op, *FString::Join(TagStrings, TEXT(",")));
    }

    if (IsCompositeOp(Kind))
    {
        const TArray<TSharedPtr<FJsonValue>>* ExprArray = nullptr;
        TArray<FString> ChildDescriptions;
        if (Node->TryGetArrayField(TEXT("expressions"), ExprArray) && ExprArray)
        {
            for (const TSharedPtr<FJsonValue>& ChildValue : *ExprArray)
            {
                const TSharedPtr<FJsonObject>* ChildObject = nullptr;
                if (ChildValue.IsValid() && ChildValue->TryGetObject(ChildObject) && ChildObject && (*ChildObject).IsValid())
                {
                    ChildDescriptions.Add(SynthesizeDescription(*ChildObject, Depth + 1, MaxDepth));
                }
            }
        }
        return FString::Printf(TEXT("%s[%s]"), *Op, *FString::Join(ChildDescriptions, TEXT(",")));
    }

    return Op;
}
}

REGISTER_RPC_HANDLER("gameplay_tags.build_query", "gameplay_tags",
    "Build an FGameplayTagQuery from a recursive JSON expression tree and optionally write it into a target asset property",
    RPC_PARAMS(
        RPC_PARAM_REQ("expression", "object", "Root expression node: { op, tags?: string[], expressions?: [...] }"),
        RPC_PARAM_OPT("target", "object", "Optional { assetPath, propertyPath } write destination"),
        RPC_PARAM_DEF("maxDepth", "number", "Recursion depth guard for the expression tree", "12"),
        RPC_PARAM_OPT("description", "string", "User-provided description recorded on the FGameplayTagQuery")
    ))
{
    using namespace GameplayTagBuildQueryHelpers;

    TSharedPtr<FJsonObject> ExpressionNode;
    if (!Ctx.RequireObject(TEXT("expression"), ExpressionNode))
    {
        return true;
    }

    const int32 MaxDepth = FMath::Max(1, Ctx.GetInt(TEXT("maxDepth"), 12));
    FString Description = Ctx.GetString(TEXT("description"));

    FGameplayTagQueryExpression RootExpr;
    FString ErrorCode;
    FString ErrorMessage;
    if (!BuildExpressionFromJson(ExpressionNode, RootExpr, 0, MaxDepth, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    // Fall back to a synthesized summary when the caller omits a user description; without
    // this, FGameplayTagQuery::GetDescription() would return an empty string because Build()
    // does not populate AutoDescription.
    if (Description.IsEmpty())
    {
        Description = SynthesizeDescription(ExpressionNode, 0, MaxDepth);
    }

    FGameplayTagQuery Query;
    Query.Build(RootExpr, Description);

    TArray<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        Query.Serialize(Writer);
    }

    bool bWrote = false;
    FString ResolvedTargetPath;
    const TSharedPtr<FJsonObject> Target = Ctx.GetObject(TEXT("target"));
    if (Target.IsValid())
    {
        FString AssetPath;
        FString PropertyPath;
        Target->TryGetStringField(TEXT("assetPath"), AssetPath);
        Target->TryGetStringField(TEXT("propertyPath"), PropertyPath);

        if (AssetPath.IsEmpty() || PropertyPath.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                TEXT("target requires both assetPath and propertyPath."));
            return true;
        }

        UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath);
        if (!Loaded)
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
            return true;
        }

        // Blueprint targets resolve to the generated CDO so the FGameplayTagQuery
        // landing site is the configured-properties surface, not UBlueprint internals.
        UObject* RootObject = Loaded;
        if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
        {
            if (BP->GeneratedClass)
            {
                RootObject = BP->GeneratedClass->GetDefaultObject();
                ResolvedTargetPath = RootObject->GetPathName();
            }
        }

        void* ContainerPtr = nullptr;
        FString ResolveError;
        FProperty* Resolved = ResolveNestedPropertyPath(RootObject, PropertyPath, ContainerPtr, ResolveError);
        if (!Resolved || !ContainerPtr)
        {
            Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
                FString::Printf(TEXT("Failed to resolve property '%s' on '%s': %s"),
                    *PropertyPath, *AssetPath, *ResolveError));
            return true;
        }

        FStructProperty* StructProperty = CastField<FStructProperty>(Resolved);
        if (!StructProperty || StructProperty->Struct != FGameplayTagQuery::StaticStruct())
        {
            Ctx.SendError(TEXT("PROPERTY_WRONG_TYPE"),
                FString::Printf(TEXT("Property '%s' is not an FGameplayTagQuery struct."), *PropertyPath));
            return true;
        }

        const FScopedTransaction Transaction(
            NSLOCTEXT("PinWright", "BuildGameplayTagQuery", "Build Gameplay Tag Query"));
        RootObject->Modify();
        StructProperty->CopySingleValue(StructProperty->ContainerPtrToValuePtr<void>(ContainerPtr), &Query);
        RootObject->MarkPackageDirty();
        RootObject->PostEditChange();
        bWrote = true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("description"), Query.GetDescription());
    Result->SetNumberField(TEXT("tokenStreamBytes"), Bytes.Num());
    Result->SetBoolField(TEXT("wrote"), bWrote);
    if (!ResolvedTargetPath.IsEmpty())
    {
        Result->SetStringField(TEXT("resolvedTargetPath"), ResolvedTargetPath);
    }
    Ctx.SendSuccess(Result);
    return true;
}
