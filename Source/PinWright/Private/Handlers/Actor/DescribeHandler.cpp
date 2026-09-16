// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Utils/ActorDescribeBuilder.h"
#include "Utils/ActorUtils.h"
#include "Utils/ComponentReadFilter.h"

#include "GameFramework/Actor.h"

REGISTER_RPC_HANDLER("actor.describe", "actor", "Read a compact JSON description of one live placed actor, including identity, transform, sparse modified actor properties, every component, scene attachment data, and sparse modified component properties. Pass fields=[...] to project down to specific top-level keys, or includeComponents=false to drop the component array, when the full shape would spill.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label or name of the actor in the active editor world. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted.")),
        RPC_PARAM_OPT("nameMatch", "string", "Case-insensitive substring filter on component name. Snake_case name_match accepted."),
        RPC_PARAM_OPT("name_match", "string", "Snake_case alias for nameMatch."),
        RPC_PARAM_OPT("componentClass", "classref", "Component UClass name or path; matches that class and subclasses. Snake_case component_class accepted."),
        RPC_PARAM_OPT("component_class", "classref", "Snake_case alias for componentClass."),
        RPC_PARAM_OPT("fields", "array|string", "Case-insensitive allow-list of top-level keys to return (valid keys: name, label, path, class, level, folder, guid, tags, transform, properties, components; e.g. [\"label\",\"transform\"] or [\"properties\"]); schema/storage always retained. Omit for the full shape. A single string is also accepted, under this key or the singular field. Use this to confirm one block without spilling the full component tree."),
        RPC_PARAM_OPT("field", "string", "Singular spelling of fields, for the one-key case."),
        RPC_PARAM_OPT("includeComponents", "bool", "When false, omits the components array entirely (shorthand for identity+transform reads). Ignored when fields is supplied. Snake_case include_components / componentsMode:\"none\" accepted."),
        RPC_PARAM_OPT("include_components", "bool", "Snake_case alias for includeComponents."),
        RPC_PARAM_OPT("componentsMode", "string", "\"none\" is an equivalent shorthand for includeComponents:false. Ignored when fields is supplied.")
    ))
{
    FString TargetName;
    if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName))
    {
        return true;
    }

    FComponentReadFilter ComponentFilter;
    FString FilterErrorCode;
    FString FilterErrorMessage;
    if (!TryParseComponentReadFilter(Ctx.GetRawPayload(), ComponentFilter,
        FilterErrorCode, FilterErrorMessage))
    {
        Ctx.SendError(FilterErrorCode, FilterErrorMessage);
        return true;
    }

    FActorDescribeOptions Options;

    // fields: accept a JSON array of strings, or a single bare string. Parsed via
    // Ctx.GetArray, mirroring property.list's propertyNames allow-list reader.
    if (const TArray<TSharedPtr<FJsonValue>>* FieldArray = Ctx.GetArray(TEXT("fields")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *FieldArray)
        {
            FString Field;
            if (Value.IsValid() && Value->TryGetString(Field))
            {
                Field.TrimStartAndEndInline();
                if (!Field.IsEmpty())
                {
                    Options.Fields.Add(Field);
                }
            }
        }
    }
    else
    {
        const FString SingleField =
            Ctx.GetStringFirstOf({TEXT("fields"), TEXT("field")});
        if (!SingleField.IsEmpty())
        {
            Options.Fields.Add(SingleField);
        }
    }

    // includeComponents (alias include_components); componentsMode:"none" is an
    // equivalent shorthand. Only consulted when no explicit fields allow-list.
    if (!Options.HasFieldProjection())
    {
        const FString ComponentsMode = Ctx.GetString(TEXT("componentsMode")).ToLower();
        if (ComponentsMode == TEXT("none"))
        {
            Options.bIncludeComponents = false;
        }
        else
        {
            Options.bIncludeComponents =
                Ctx.GetBoolFirstOf({TEXT("includeComponents"), TEXT("include_components")}, true);
        }
    }

    // Resolve with an explicit ambiguity verdict: a label matching several actors must not
    // be reported as "not found", and must never silently pick one.
    AActor* Found = nullptr;
    if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found))
    {
        return true;
    }

    Ctx.SendSuccess(ActorDescribeBuilder::BuildActorJson(Found, TEXT("live"), ComponentFilter, Options));
    return true;
}
