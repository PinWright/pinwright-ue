// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector2D.h"
#include "Templates/Function.h"

#include "Handlers/Drive/DriveTypes.h"          // EDriveSurface, EDriveSettleOutcome, FDriveCondition
#include "Handlers/Drive/DriveLiveResolver.h"   // FDriveRootSelector
#include "Handlers/Drive/DriveEditorChrome.h"   // FDriveWindowSelector
#include "Handlers/Drive/DriveSettleDriver.h"   // FGetElements / FIsWaitForMet typedefs

class FHandlerContext;
class FJsonObject;

// How much observation detail an async action / wait response carries back. The
// action / wait verbs DEFAULT to None (compact result): a full element list and
// screenshot is opt-in via observe=list / observe=list+screenshot, because rich HUDs
// serialize 170-200+ verbose elements that an action caller did not ask for.
enum class EDriveObserveMode : uint8
{
    // Resolve the request with no observation payload (settle result + diff only). DEFAULT.
    None,
    // Attach the post-action element list, no screenshot.
    List,
    // Attach the element list plus a Set-of-Mark screenshot.
    ListAndScreenshot
};

// Shared orchestration for the asynchronous drive.* ACTION verbs (click / type /
// key / scroll / drag / hover) and the building blocks drive.wait_for reuses. The
// action flow re-resolves the target handle against the live UI right before
// acting (the stale-state guard), injects the caller-supplied input at the
// element center, then runs FDriveSettleDriver across engine frames and resolves
// the request late through an async token. Every synchronous validation failure
// answers immediately via Ctx; only the post-action observation resolves async.
class FDriveActionCommon
{
public:
    // Injection callback invoked once synchronously after the target is resolved
    // and validated, before the settle driver starts. Receives the resolved
    // target element's absolute screen-space center (zero when the action has no
    // target, e.g. drive.key without a handle). Returns false on an injection
    // failure (reported as a clean INPUT_FAILED error).
    using FInject = TFunction<bool(const FVector2D& TargetCenter)>;

    // Run one action end-to-end against the handle resolved from Ctx args. Reads
    // the common settle / observe / journal params from Ctx, sends the response
    // itself (synchronous error or async success), and the calling handler simply
    // returns true afterwards. Pass an empty Handle for a target-less action
    // (drive.key without a handle): the resolve/validate step is skipped and the
    // injection runs against the focused widget, with a live-UI pre-check so the
    // no-PIE case is a clean error rather than a hang.
    //
    // InputPath, when set, is echoed back as the response's `input_path` field so a
    // caller can see WHICH injection layer ran ("slate" vs "os_x11") instead of
    // inferring it from the request. Null (the default) omits the field for the verbs
    // that only ever take one path.
    static void RunAction(FHandlerContext& Ctx, const FString& Handle, const FInject& Inject,
        const TCHAR* InputPath = nullptr);

    // Whether a freshly re-resolved element may still be acted on: it must be drawn, enabled,
    // and carry a rect Slate measured this frame. The geometry term matters on its own - a
    // stale element's rect is zeroed, so "its center" is desktop (0,0), and an ungated action
    // would silently click or release in the corner of the screen instead of refusing. Shared
    // by RunAction's press-target gate and drive.drag's release-target gate so the two cannot
    // drift, and pure so the rule is testable without a live target.
    static bool IsActionable(const FDriveElement& Element);

    // Parse the `observe` mode param (none | list | list+screenshot). A MISSING param
    // resolves to None (the compact default for action / wait verbs); an explicit but
    // unrecognized token resolves to ListAndScreenshot.
    static EDriveObserveMode ParseObserveMode(const FHandlerContext& Ctx);

    // Wire-string form of a settle outcome (snake_case), e.g. "settled_changed".
    static FString SettleOutcomeToString(EDriveSettleOutcome Outcome);

    // A GetElements sampler bound to one surface/root; returns an empty list on
    // any sampling failure so the settle loop keeps polling instead of throwing.
    // WindowSelector picks the editor window for the EditorChrome surface (ignored
    // for game); it defaults to the active window so game-only callers are unchanged.
    static FDriveSettleDriver::FGetElements MakeGetElements(
        EDriveSurface Surface, const FDriveRootSelector& Selector,
        const FDriveWindowSelector& WindowSelector = FDriveWindowSelector());

    // An IsWaitForMet predicate that evaluates Condition over the current surface
    // elements plus (for journal conditions) the live journal tail.
    static FDriveSettleDriver::FIsWaitForMet MakeIsWaitForMet(
        EDriveSurface Surface, const FDriveRootSelector& Selector, const FDriveCondition& Condition,
        const FDriveWindowSelector& WindowSelector = FDriveWindowSelector());

    // Build the `observation` JSON field honoring ObserveMode. Returns null for
    // EDriveObserveMode::None or when the observation cannot be built (journal
    // is attached separately, never inside the observation here).
    static TSharedPtr<FJsonObject> BuildObservationField(
        EDriveSurface Surface, const FDriveRootSelector& Selector,
        EDriveObserveMode ObserveMode, int32 MarkCap,
        const FDriveWindowSelector& WindowSelector = FDriveWindowSelector());

    // When bIncludeJournal, attach a top-level `journal` field with the delta
    // since JournalSince (empty delta when no live tail). No-op otherwise.
    static void MaybeAttachJournal(
        const TSharedPtr<FJsonObject>& Resp, bool bIncludeJournal, uint64 JournalSince);
};
