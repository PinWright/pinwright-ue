// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorLabelHandler.cpp - actor.set_label
//
// The display label is the identity a user sees: it is what the World Outliner shows and
// what renaming in the outliner changes. Until this verb existed, PinWright could SET a
// label only as a side effect of creating an actor (actor.spawn* / actor.duplicate) and
// had no way to change one afterwards, so an agent could not rename what a user had
// placed, nor repair the auto-derived label a no-name spawn leaves behind.
//
// Two engine entry points, and the difference is the whole reason `unique` exists:
//   AActor::SetActorLabel                     - stores the label VERBATIM. It does NOT
//                                               uniquify (Engine/Private/ActorEditor.cpp:1291
//                                               validates, compares, assigns), so two actors
//                                               can end up sharing one label.
//   FActorLabelUtilities::SetActorLabelUnique - appends a numeric suffix until the label is
//                                               unused (EditorEngine.cpp:6579).
// Both mark the actor's package dirty via Modify(). Because SetActorLabel silently ignores
// a label that fails FActorEditorUtils::ValidateActorName (it logs a warning and leaves the
// old label in place), this verb never reports the label it asked for - it reads the label
// back off the actor afterwards and derives `applied` from that comparison, so a rejected
// label reports applied=false instead of echoing the request (rpc-design.md §1, §4).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
// FActorLabelUtilities::SetActorLabelUnique lives on the EditorEngine header
// (UnrealEd/Classes/Editor/EditorEngine.h:3429), not in ActorEditorUtils.
#include "Editor/EditorEngine.h"
#include "GameFramework/Actor.h"

REGISTER_RPC_HANDLER("actor.set_label", "actor",
    "Rename a placed actor's DISPLAY LABEL - the name the World Outliner shows - via SetActorLabel. This is the editor-only label, not the actor's internal object name (which stays whatever it was; UE does not let you rename it in place). Labels are NOT unique by default: pass unique=true to have UE append a numeric suffix instead of creating a duplicate. Reports the label read back off the actor after the write, so a label UE rejects as invalid reports applied=false rather than echoing your request.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Actor to rename, by display label, unique internal object name, or object path. The objectPath and actorPath aliases are also accepted." ACTORNAME_COLLISION_STEER)),
        RPC_PARAM_REQ("label", "string", "New display label to assign."),
        RPC_PARAM_DEF("unique", "bool", "When true, use SetActorLabelUnique so a label already in use gets a numeric suffix instead of colliding. When false (default) the label is stored verbatim and may duplicate another actor's.", "false")
    ))
{
    FString NewLabel;
    if (!Ctx.RequireString(TEXT("label"), NewLabel))
    {
        return true;
    }
    NewLabel = NewLabel.TrimStartAndEnd();
    if (NewLabel.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("'label' is empty. UE rejects an empty actor label, so there is no rename to perform."));
        return true;
    }

    AActor* Actor = nullptr;
    FString Requested;
    if (!ActorNameParamUtils::RequireResolvedActor(Ctx, nullptr, Actor, &Requested))
    {
        return true;
    }

    // Some actors cannot be relabelled at all (an actor inside a non-edited Level Instance
    // returns false from IsActorLabelEditable). Refuse rather than issue a write the engine
    // will drop and then report as a rename.
    if (!Actor->IsActorLabelEditable())
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_LABEL_NOT_EDITABLE,
            FString::Printf(
                TEXT("Actor '%s' does not allow label edits (AActor::IsActorLabelEditable is false - typically an ")
                TEXT("actor inside a Level Instance that is not currently being edited). Its label is unchanged."),
                *Actor->GetName()));
        return true;
    }

    const FString PreviousLabel = Actor->GetActorLabel();
    const bool bUnique = Ctx.GetBool(TEXT("unique"), false);

    if (bUnique)
    {
        FActorLabelUtilities::SetActorLabelUnique(Actor, NewLabel);
    }
    else
    {
        Actor->SetActorLabel(NewLabel);
    }

    // Measure, do not assert. SetActorLabel drops an invalid label with only a log warning,
    // and SetActorLabelUnique deliberately stores something other than what was asked for.
    // Reading the label back is the only honest report of what the outliner will now show.
    const FString AppliedLabel = Actor->GetActorLabel();
    const bool bApplied = AppliedLabel.Equals(NewLabel, ESearchCase::CaseSensitive);
    const bool bUniquified = bUnique && !bApplied && AppliedLabel.StartsWith(NewLabel);
    const bool bChanged = !AppliedLabel.Equals(PreviousLabel, ESearchCase::CaseSensitive);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("requestedLabel"), NewLabel);
    Data->SetStringField(TEXT("previousLabel"), PreviousLabel);
    // The authoritative value: what GetActorLabel() returns now, i.e. what the World
    // Outliner displays.
    Data->SetStringField(TEXT("label"), AppliedLabel);
    Data->SetStringField(TEXT("actorLabel"), AppliedLabel);
    // Unchanged by this verb - a label rename never touches the internal FName. Reported so
    // the caller keeps a collision-safe handle for follow-up calls.
    Data->SetStringField(TEXT("actorObjectName"), Actor->GetName());
    Data->SetStringField(TEXT("actorPath"), Actor->GetPathName());
    Data->SetStringField(TEXT("actorClass"), Actor->GetClass()->GetName());
    Data->SetBoolField(TEXT("applied"), bApplied);
    Data->SetBoolField(TEXT("changed"), bChanged);
    Data->SetBoolField(TEXT("labelWasUniquified"), bUniquified);
    Data->SetBoolField(TEXT("uniqueRequested"), bUnique);

    if (!bApplied && !bUniquified)
    {
        // The write did not take and it was not the documented uniquify substitution, so the
        // only remaining cause is UE rejecting the string. Fail loudly: a caller that reads
        // `label` and moves on would otherwise believe a rename happened.
        Ctx.SendError(ErrorCodes::ERR_INVALID_ACTOR_LABEL,
            FString::Printf(
                TEXT("UE rejected the label '%s' (FActorEditorUtils::ValidateActorName); the actor still shows ")
                TEXT("'%s'. Actor labels cannot be empty or contain characters UE reserves for object names."),
                *NewLabel, *AppliedLabel),
            Data);
        return true;
    }

    UE_LOG(LogPinWrightSubsystem, Display,
           TEXT("actor.set_label: %s relabelled '%s' -> '%s'"),
           *Actor->GetName(), *PreviousLabel, *AppliedLabel);
    Ctx.SendSuccess(Data);
    return true;
}
