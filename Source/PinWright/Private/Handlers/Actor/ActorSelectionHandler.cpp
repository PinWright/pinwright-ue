// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorSelectionHandler.cpp - Select actors in the editor by name

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "GameFramework/Actor.h"

// ---- actor.select ----
// Accepts the singular actorName scalar in addition to the actorNames array (via the
// shared McpActorUtils::CollectActorNames dual-accept, also used by actor.delete), so a
// caller crossing from the per-actor verbs (actor.get / set_transform / add_tag) with a
// single target in hand does not have to re-spell actorName:"X" as actorNames:["X"]. The
// empty actorNames array still clears the selection; the singular fallback only fires
// when the array key is absent.
REGISTER_RPC_HANDLER("actor.select", "actor", "Select actors in the level editor by name. Provide actorName for a single actor or actorNames (an array) for several; an empty actorNames array clears the selection.",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Display label or name of one actor to select; ignored if actorNames is also provided."),
        RPC_PARAM_OPT("actorNames", "array", "Array of actor names to select (empty array clears selection). Preferred for selecting several actors at once.")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("NO_EDITOR"), TEXT("Editor is not available"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // Shared dual-accept gather (actorNames array, else singular actorName scalar).
    // A present-but-empty actorNames array deliberately keeps the "clears selection"
    // semantics, so the empty-names case is only an error when the array key was absent
    // entirely (neither identity slot supplied) — name both accepted forms in the error
    // so a wrong-arity caller self-corrects from the text without a retry.
    TArray<FString> RequestedNames;
    bool bHasNamesArray = false;
    if (!McpActorUtils::CollectActorNames(Payload, RequestedNames, bHasNamesArray) && !bHasNamesArray)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName or actorNames required"));
        return true;
    }

    TArray<AActor*> ActorsToSelect;
    TArray<FString> NotFound;
    TArray<TSharedPtr<FJsonValue>> Ambiguous;

    for (const FString& Name : RequestedNames)
    {
        // A name matching several actors is not "not found" - saying so would be false,
        // and selecting whichever matched first would silently hand the user a different
        // actor than the one they named.
        const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(nullptr, Name);
        if (Resolution.IsAmbiguous())
        {
            TSharedPtr<FJsonObject> AmbObj = MakeShared<FJsonObject>();
            AmbObj->SetStringField(TEXT("requestedName"), Name);
            TArray<TSharedPtr<FJsonValue>> Cands;
            for (AActor* Candidate : Resolution.Candidates)
            {
                if (!Candidate)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
                C->SetStringField(TEXT("label"), Candidate->GetActorLabel());
                C->SetStringField(TEXT("name"), Candidate->GetName());
                C->SetStringField(TEXT("path"), Candidate->GetPathName());
                Cands.Add(MakeShared<FJsonValueObject>(C));
            }
            AmbObj->SetArrayField(TEXT("candidates"), Cands);
            Ambiguous.Add(MakeShared<FJsonValueObject>(AmbObj));
        }
        else if (AActor* Found = Resolution.Actor)
        {
            ActorsToSelect.Add(Found);
        }
        else
        {
            NotFound.Add(Name);
        }
    }

    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    ActorSS->SetSelectedLevelActors(ActorsToSelect);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetNumberField(TEXT("selectedCount"), ActorsToSelect.Num());

    if (NotFound.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarningsArray;
        for (const FString& Name : NotFound)
        {
            WarningsArray.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("Actor '%s' not found"), *Name)));
        }
        Resp->SetArrayField(TEXT("warnings"), WarningsArray);
    }
    if (Ambiguous.Num() > 0)
    {
        // Kept out of warnings[]: an ambiguous name has a different recovery from a missing
        // one - pass a unique internal object name from candidates[].name.
        Resp->SetArrayField(TEXT("ambiguous"), Ambiguous);
        Resp->SetNumberField(TEXT("ambiguousCount"), Ambiguous.Num());
    }

    // selectedActors keeps GetActorNameOrLabel() for back-compat, but that accessor returns
    // the label when one is set and the internal name otherwise, so a caller cannot tell
    // which identity it received — and a label is not unique, so re-using it as a lookup key
    // can resolve a different actor. The parallel 'selected' rows state both explicitly:
    // objectName is GetName(), unique within the level and safe to look up by.
    TArray<TSharedPtr<FJsonValue>> SelectedArray;
    TArray<TSharedPtr<FJsonValue>> SelectedRows;
    for (const AActor* Actor : ActorsToSelect)
    {
        SelectedArray.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Entry->SetStringField(TEXT("objectName"), Actor->GetName());
        Entry->SetStringField(TEXT("path"), Actor->GetPathName());
        SelectedRows.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Resp->SetArrayField(TEXT("selectedActors"), SelectedArray);
    Resp->SetArrayField(TEXT("selected"), SelectedRows);
    Ctx.SendSuccess(Resp);
    return true;
}
