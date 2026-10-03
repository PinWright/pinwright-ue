// Copyright (c) 2026 Alexander Penkin. MIT License.

// MaterialDiscoveryHandler.cpp
// Catalog + keyword search for UMaterialExpression subclasses, mirroring the
// Blueprint discovery pair (blueprint.graph.list_node_types / blueprint.search_api).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"

#include "Compat/EngineVersionCompat.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/Material.h"
#include "MaterialExpressionIO.h"
#include "MaterialDomain.h"
#include "Material/MaterialInputIterCompat.h"
#include "Material/MaterialPinNames.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/Package.h"


// Lightweight string-only snapshot for scoring without allocating JSON or walking pins.
struct FExpressionScoreFields
{
    UClass* Class = nullptr;
    UMaterialExpression* Cdo = nullptr;
    FString ClassName;
    FString ShortName;
    FString Category;
    FString Caption;
    FString Description;
    FString Keywords;
    bool bIsParameter = false;
};

static bool BuildScoreFields(UClass* Class, FExpressionScoreFields& Out)
{
    if (!Class) return false;
    UMaterialExpression* Cdo = Cast<UMaterialExpression>(Class->GetDefaultObject());
    if (!Cdo) return false;

    Out.Class = Class;
    Out.Cdo = Cdo;
    Out.ClassName = Class->GetName();
    Out.ShortName = Out.ClassName;
    Out.ShortName.RemoveFromStart(TEXT("MaterialExpression"));
    Out.Category = Cdo->MenuCategories.Num() > 0 ? Cdo->MenuCategories[0].ToString() : FString();
    // Description fallback chain: GetCreationDescription() (rarely overridden -- the
    // base impl returns FText::GetEmpty(), so most classes including
    // UMaterialExpressionFresnel produce nothing) -> class-level ToolTip metadata
    // synthesized by UHT from the leading doc comment (e.g. Fresnel's header carries
    // "Allows the artists to quickly set up a Fresnel term..." which
    // UClass::GetToolTipText() surfaces) -> GetDescription() (re-emits the caption,
    // last-resort so the field is never empty).
    Out.Description = Cdo->GetCreationDescription().ToString();
    if (Out.Description.IsEmpty())
    {
        Out.Description = Class->GetToolTipText(/*bShortTooltip=*/true).ToString();
    }
    if (Out.Description.IsEmpty())
    {
        Out.Description = Cdo->GetDescription();
    }

    TArray<FString> Caps;
#if (UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0) && UE_VERSION_OLDER_THAN(5, 6, 0))
    // UE 5.5 only: UMaterialExpressionObjectPositionWS::GetCaption() dereferences its
    // owning UMaterial* (Material->MaterialDomain) without a null guard. On a CDO,
    // Material is null, so calling GetCaption() here hard-crashes (EXCEPTION_ACCESS_VIOLATION
    // reading 0x3b0). 5.6+ guards it as "Material && Material->...". Skip the caption for
    // this one class on 5.5; every other expression's GetCaption is CDO-safe.
    if (Out.ClassName != TEXT("MaterialExpressionObjectPositionWS"))
    {
        Cdo->GetCaption(Caps);
    }
#else
    Cdo->GetCaption(Caps);
#endif
    Out.Caption = FString::Join(Caps, TEXT(" "));

    Out.Keywords = Cdo->GetKeywords().ToString();
    Out.bIsParameter =
        Class->IsChildOf(UMaterialExpressionParameter::StaticClass()) ||
        Class->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass());
    return true;
}

// Per-field emit decisions for BuildExpressionRecord, resolved ONCE before the
// per-class loop (the projection set is loop-invariant) so the record builder
// branches on cheap bools instead of probing the projection TSet — with a
// throwaway FString per key — for every emitted row. This matches the actor.list /
// system.console.search convention; default all-true = the full unprojected shape
// search_expression_types always wants.
struct FExpressionFieldMask
{
    bool bClassName = true;
    bool bShortName = true;
    bool bCategory = true;
    bool bDescription = true;
    bool bCaption = true;
    bool bKeywords = true;
    bool bInputPins = true;
    bool bOutputPins = true;
    bool bIsParameter = true;
};

// Collapse the lowercase allow-list from FHandlerContext::ReadFieldProjection into
// a per-field bool mask. An empty set means "no projection — emit every field" (the
// byte-identical default). Resolve each key to a bool once here, hoisting the
// throwaway-FString TSet probe out of the per-row loop. Probe keys are lowercase to
// match the lowercased set ReadFieldProjection returns.
static FExpressionFieldMask MakeFieldMask(const TSet<FString>& WantKeys)
{
    const bool bProject = WantKeys.Num() > 0;
    const auto Wants = [&WantKeys, bProject](const TCHAR* LowerKey) -> bool
    {
        return !bProject || WantKeys.Contains(FString(LowerKey));
    };

    FExpressionFieldMask Mask;
    Mask.bClassName   = Wants(TEXT("classname"));
    Mask.bShortName   = Wants(TEXT("shortname"));
    Mask.bCategory    = Wants(TEXT("category"));
    Mask.bDescription = Wants(TEXT("description"));
    Mask.bCaption     = Wants(TEXT("caption"));
    Mask.bKeywords    = Wants(TEXT("keywords"));
    Mask.bInputPins   = Wants(TEXT("inputpins"));
    Mask.bOutputPins  = Wants(TEXT("outputpins"));
    Mask.bIsParameter = Wants(TEXT("isparameter"));
    return Mask;
}

// Build the per-expression JSON record. Mask selects which fields to emit (default
// all-true emits every field, the shape search_expression_types always wants); the
// heavy inputPins/outputPins walks are skipped entirely when those keys are dropped.
static TSharedPtr<FJsonObject> BuildExpressionRecord(
    const FExpressionScoreFields& Fields, const FExpressionFieldMask& Mask = FExpressionFieldMask())
{
    if (!Fields.Class || !Fields.Cdo) return nullptr;

    TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
    if (Mask.bClassName)   Record->SetStringField(TEXT("className"), Fields.ClassName);
    if (Mask.bShortName)   Record->SetStringField(TEXT("shortName"), Fields.ShortName);
    if (Mask.bCategory)    Record->SetStringField(TEXT("category"), Fields.Category);
    if (Mask.bDescription) Record->SetStringField(TEXT("description"), Fields.Description);
    if (Mask.bCaption)     Record->SetStringField(TEXT("caption"), Fields.Caption);
    if (Mask.bKeywords)    Record->SetStringField(TEXT("keywords"), Fields.Keywords);

    // Both pin arrays stay positionally aligned with the engine's pin order — inputPins[i]
    // is the pin addressed as inputName, outputPins[i] the one addressed as
    // sourceOutputIndex == i — and both go through PinWright::MaterialPinNames so the
    // reported spelling is the one connect_nodes resolves. Reading FName fields raw here
    // emitted the literal "None" for 267 of 346 classes (every unnamed output, e.g.
    // VertexColor's five mask outputs), which made sourcePin unusable for them.
    if (Mask.bInputPins)
    {
        TArray<TSharedPtr<FJsonValue>> InputPins;
        for (const FString& Name : PinWright::MaterialPinNames::DeriveInputPinNames(Fields.Cdo))
        {
            InputPins.Add(MakeShared<FJsonValueString>(Name));
        }
        Record->SetArrayField(TEXT("inputPins"), InputPins);
    }

    if (Mask.bOutputPins)
    {
        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (const FString& Name : PinWright::MaterialPinNames::DeriveOutputPinNames(Fields.Cdo))
        {
            OutputPins.Add(MakeShared<FJsonValueString>(Name));
        }
        Record->SetArrayField(TEXT("outputPins"), OutputPins);
    }

    if (Mask.bIsParameter)
    {
        Record->SetBoolField(TEXT("isParameter"), Fields.bIsParameter);
    }
    return Record;
}

// The closed domainFilter vocabulary, in canonical spelling. Matching is case-insensitive; the
// canonical name is what a filtered response echoes and what a refusal lists.
namespace MaterialDiscoveryDomainFilter
{
    struct FDomainName { const TCHAR* Name; EMaterialDomain Domain; };
    static const FDomainName Names[] = {
        { TEXT("Surface"),       MD_Surface },
        { TEXT("PostProcess"),   MD_PostProcess },
        { TEXT("UI"),            MD_UI },
        { TEXT("DeferredDecal"), MD_DeferredDecal },
        { TEXT("LightFunction"), MD_LightFunction },
        { TEXT("Volume"),        MD_Volume },
    };

    static const FDomainName* Parse(const FString& Name)
    {
        for (const FDomainName& Entry : Names)
        {
            if (Name.Equals(Entry.Name, ESearchCase::IgnoreCase)) return &Entry;
        }
        return nullptr;
    }

    static FString AllowedList()
    {
        TArray<FString> Out;
        for (const FDomainName& Entry : Names) Out.Add(Entry.Name);
        return FString::Join(Out, TEXT(", "));
    }
}


// ---- material.graph.list_expression_types ----
REGISTER_RPC_HANDLER("material.graph.list_expression_types", "material.graph",
    "List all UMaterialExpression subclasses with class name, category, pin layout.",
    RPC_PARAMS(
        RPC_PARAM_OPT("category", "string", "Restrict to expressions whose first MenuCategory contains this string (case-insensitive)"),
        RPC_PARAM_OPT("domainFilter", "string", "Restrict to expressions valid for this MaterialDomain (Surface/PostProcess/UI/DeferredDecal/LightFunction/Volume, case-insensitive). Any other non-empty value is refused with INVALID_ARGUMENT, never treated as no filter; a filtered response echoes the canonical name as appliedDomainFilter. Needs UE 5.6+ (UMaterialExpression::IsAllowedIn); older engines refuse it with UNSUPPORTED_ENGINE_VERSION."),
        RPC_PARAM_OPT("includeAbstract", "boolean", "Include abstract base classes (default false)"),
        RPC_PARAM_OPT("parameterOnly", "boolean", "Only return parameter-type expressions (default false)"),
        RPC_PARAM_DEF("limit", "number", "Max expressions to return after filtering. 0 (default) = all. totalMatches always reports the full untruncated count so elision is detectable.", "0"),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-expression keys to return (valid keys: className, shortName, category, description, caption, keywords, inputPins, outputPins, isParameter); e.g. [\"className\",\"shortName\"] to drop the heavy description/caption/pin fields. Omit for all nine. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "boolean", "When true, returns only className+shortName+category+isParameter per expression (drops the heavy description/caption/keywords/inputPins/outputPins) — shorthand for the common 'list this category so I can pick a className' read that otherwise overflows the inline budget. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    const FString CategoryFilter = Ctx.GetString(TEXT("category"));
    const FString DomainName = Ctx.GetString(TEXT("domainFilter"));
    const bool bIncludeAbstract = Ctx.GetBool(TEXT("includeAbstract"), false);
    const bool bParameterOnly = Ctx.GetBool(TEXT("parameterOnly"), false);

    // Per-expression field projection mirroring the validated actor.list shape: an
    // explicit fields allow-list (array or bare string) wins; otherwise namesOnly
    // expands to the light identity columns and drops the heavy description/caption/
    // keywords/pin fields that make a full category listing spill. An empty set means
    // "no projection" — every field is emitted (byte-identical to the prior shape).
    const TSet<FString> WantKeys = Ctx.ReadFieldProjection(
        {TEXT("className"), TEXT("shortName"), TEXT("category"), TEXT("isParameter")});
    // Resolve the projection to a per-field bool mask once, before the per-class
    // loop, so each emitted row branches on cheap bools (the actor.list convention)
    // rather than re-probing the TSet per row.
    const FExpressionFieldMask FieldMask = MakeFieldMask(WantKeys);
    // limit truncates the returned array after filtering; 0 = all. totalMatches below
    // always reports the full filtered count regardless of the cap.
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

    // Omitted means no filter; a non-empty value that does not parse is a refusal. Falling back to
    // the unfiltered catalog would make a typo read as proof every row is valid for that domain.
    const MaterialDiscoveryDomainFilter::FDomainName* AppliedDomain = nullptr;
    if (!DomainName.IsEmpty())
    {
        AppliedDomain = MaterialDiscoveryDomainFilter::Parse(DomainName);
        if (!AppliedDomain)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Unknown domainFilter '%s'. Valid: %s."),
                    *DomainName, *MaterialDiscoveryDomainFilter::AllowedList()));
            return true;
        }
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        // UMaterialExpression::IsAllowedIn does not exist before 5.6, so the filter cannot be
        // measured; refusing beats returning the whole catalog under the requested domain's name.
        Ctx.SendUnsupportedEngineVersion(TEXT("5.6"),
            TEXT("material.graph.list_expression_types domainFilter (UMaterialExpression::IsAllowedIn)"));
        return true;
#endif
    }

    UMaterial* DomainProbe = nullptr;
    const bool bUseDomainFilter = AppliedDomain != nullptr;
    if (bUseDomainFilter)
    {
        DomainProbe = NewObject<UMaterial>(GetTransientPackage());
        if (DomainProbe)
        {
            DomainProbe->MaterialDomain = AppliedDomain->Domain;
            DomainProbe->AddToRoot();
        }
    }

    TArray<TSharedPtr<FJsonValue>> Expressions;
    Expressions.Reserve(256);
    int32 TotalMatched = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Class->IsChildOf(UMaterialExpression::StaticClass())) continue;
        if (Class == UMaterialExpression::StaticClass() && !bIncludeAbstract) continue;
        if (!bIncludeAbstract && Class->HasAnyClassFlags(CLASS_Abstract)) continue;

        const bool bIsParameter =
            Class->IsChildOf(UMaterialExpressionParameter::StaticClass()) ||
            Class->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass());
        if (bParameterOnly && !bIsParameter) continue;

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (bUseDomainFilter && DomainProbe)
        {
            UMaterialExpression* Cdo = Cast<UMaterialExpression>(Class->GetDefaultObject());
            if (!Cdo || !Cdo->IsAllowedIn(DomainProbe))
            {
                continue;
            }
        }
#endif

        FExpressionScoreFields Fields;
        if (!BuildScoreFields(Class, Fields)) continue;

        if (!CategoryFilter.IsEmpty() && !Fields.Category.Contains(CategoryFilter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        ++TotalMatched;
        if (Limit > 0 && Expressions.Num() >= Limit)
        {
            continue; // keep counting TotalMatched, but stop appending rows
        }

        TSharedPtr<FJsonObject> Record = BuildExpressionRecord(Fields, FieldMask);
        if (!Record.IsValid()) continue;

        Expressions.Add(MakeShared<FJsonValueObject>(Record));
    }

    if (DomainProbe)
    {
        DomainProbe->RemoveFromRoot();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("expressions"), Expressions);
    // count = rows returned (unchanged semantics); totalMatches = full untruncated
    // match count so a caller can tell the list was capped by limit. totalMatches +
    // truncated is the shared filter+limit vocabulary of the sibling search handler.
    Result->SetNumberField(TEXT("count"), Expressions.Num());
    Result->SetNumberField(TEXT("totalMatches"), TotalMatched);
    Result->SetBoolField(TEXT("truncated"), Expressions.Num() < TotalMatched);
    if (AppliedDomain)
    {
        Result->SetStringField(TEXT("appliedDomainFilter"), AppliedDomain->Name);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- material.graph.search_expression_types ----
REGISTER_RPC_HANDLER("material.graph.search_expression_types", "material.graph",
    "Keyword-ranked search across UMaterialExpression subclasses.",
    RPC_PARAMS(
        RPC_PARAM_REQ("query", "string", "Substring query, case-insensitive"),
        RPC_PARAM_OPT("category", "string", "Restrict to category"),
        RPC_PARAM_OPT("limit", "number", "Maximum number of results (default 20)")
    ))
{
    FString Query;
    if (!Ctx.RequireString(TEXT("query"), Query)) return true;

    const FString CategoryFilter = Ctx.GetString(TEXT("category"));
    int32 Limit = Ctx.GetInt(TEXT("limit"), 20);
    if (Limit <= 0) Limit = 20;

    struct FScoredFields
    {
        int32 Score;
        FExpressionScoreFields Fields;
    };
    TArray<FScoredFields> Scored;
    Scored.Reserve(256);

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Class->IsChildOf(UMaterialExpression::StaticClass())) continue;
        if (Class == UMaterialExpression::StaticClass()) continue;
        if (Class->HasAnyClassFlags(CLASS_Abstract)) continue;

        FExpressionScoreFields Fields;
        if (!BuildScoreFields(Class, Fields)) continue;

        if (!CategoryFilter.IsEmpty() && !Fields.Category.Contains(CategoryFilter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        int32 Score = 0;
        if (Fields.ShortName.Equals(Query, ESearchCase::IgnoreCase))            Score = 100;
        else if (Fields.ClassName.Equals(Query, ESearchCase::IgnoreCase))       Score = 80;
        else if (Fields.ShortName.StartsWith(Query, ESearchCase::IgnoreCase))   Score = 60;
        else if (Fields.ClassName.Contains(Query, ESearchCase::IgnoreCase))     Score = 40;
        else if (Fields.Category.Contains(Query, ESearchCase::IgnoreCase))      Score = 30;
        else if (Fields.Caption.Contains(Query, ESearchCase::IgnoreCase))       Score = 20;
        else if (Fields.Description.Contains(Query, ESearchCase::IgnoreCase) ||
                 Fields.Keywords.Contains(Query, ESearchCase::IgnoreCase))      Score = 10;
        else                                                                    Score = 0;

        if (Score <= 0) continue;

        Scored.Add({ Score, MoveTemp(Fields) });
    }

    const int32 TotalMatches = Scored.Num();
    Scored.Sort([](const FScoredFields& A, const FScoredFields& B) { return A.Score > B.Score; });

    const int32 Take = FMath::Min(Limit, Scored.Num());
    TArray<TSharedPtr<FJsonValue>> Results;
    Results.Reserve(Take);
    for (int32 i = 0; i < Take; ++i)
    {
        TSharedPtr<FJsonObject> Out = BuildExpressionRecord(Scored[i].Fields);
        if (!Out.IsValid()) continue;
        Out->SetNumberField(TEXT("score"), Scored[i].Score);
        Results.Add(MakeShared<FJsonValueObject>(Out));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), Results);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Ctx.SendSuccess(Result);
    return true;
}
