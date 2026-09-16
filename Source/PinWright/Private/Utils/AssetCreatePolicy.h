// Copyright (c) 2026 Alexander Penkin. MIT License.

// Non-interactive policy for "something already occupies the path this create verb
// wants to write". Every entry point here is guaranteed never to open a dialog.
//
// Why this exists (B0 / B-asset-create-modal-deadlock): IAssetTools::CreateAsset
// funnels through UAssetToolsImpl::CanCreateAsset, which on finding a live object at
// the target path runs StaticFindObject -> "Overwrite Existing Object" prompt ->
// ObjectTools::DeleteSingleObject reference-check prompt -> "asset is referenced by
// other content" prompt (UE 5.8 AssetTools.cpp:4884-4934). Each of those owns the
// game thread in a nested Slate loop, which wedges every in-flight and subsequent RPC
// from every client - indistinguishable from a crash to an unattended agent.
//
// FScopedUnattendedRpc (Dispatch/ScopedUnattendedRpc.h) covers the FMessageDialog
// members of that chain, but it is NOT sufficient on its own: on UE 5.5/5.6 the
// overwrite prompt is a raw SMessageDialog::ShowModal with no CanShowDialogs() guard
// (5.5 AssetTools.cpp:4606-4618), so GIsRunningUnattendedScript does not reach it.
// The only portable fix is to never let an existing object survive into
// CanCreateAsset, which is what Resolve() guarantees.
#pragma once

#include "CoreMinimal.h"

class FHandlerContext;
class FJsonObject;

namespace AssetCreatePolicy
{
    enum class EAction : uint8
    {
        // Path is clear - the handler runs its normal creation path unchanged.
        Create,
        // A same-class asset is already there. Reuse it instead of rebuilding, which
        // preserves its referencers - what an idempotent re-run actually wants.
        UpdateInPlace,
        // The handler must abort and report ErrorCode / ErrorMessage.
        Rejected
    };

    // Referencer lists are capped so an ASSET_IN_USE payload cannot blow the response
    // budget on a heavily-referenced asset; ReferencerCount stays uncapped.
    inline constexpr int32 MaxReportedReferencers = 25;

    struct FResolution
    {
        EAction Action = EAction::Create;

        // Valid only when Action == UpdateInPlace. Cleared on every other outcome,
        // including the overwrite path where the object was deleted.
        UObject* Existing = nullptr;

        // True when an object occupied the target path at Resolve() time - including
        // the overwrite case that deleted it and returned Create.
        bool bExistingFound = false;

        FString ErrorCode;
        FString ErrorMessage;

        // ASSET_IN_USE only: referencing package names from the ASSET REGISTRY (capped),
        // plus the real count. On-disk referencers - a different axis from the live
        // in-memory ones below, which the registry cannot see.
        TArray<FString> Referencers;
        int32 ReferencerCount = 0;

        // ASSET_IN_USE only, and only on the in-memory refusal branch: LIVE level actors
        // holding the occupant, described as "Label (ClassName) in /Game/Maps/L_Foo"
        // (capped), plus the real count. A spawned actor is the everyday cause of that
        // refusal - the registry list above is empty for it, because an unsaved level
        // reference is not on disk to be indexed.
        TArray<FString> ReferencingActors;
        int32 ReferencingActorCount = 0;

        // Populated whenever bExistingFound: the class and object path found.
        FString ExistingClass;
        FString ExistingPath;

        bool IsRejected() const { return Action == EAction::Rejected; }
    };

    // Decide what a create verb should do about the target path. Opens no dialog on
    // any branch. PackageName is the long package name ("/Game/Foo/M_Bar") and
    // AssetName the leaf ("M_Bar"); ExpectedClass is the class the verb is about to
    // create. bRequireExactClass rejects a subclass occupant as a class mismatch -
    // pass it when a base-class IsA() match would silently return the wrong shape
    // (animation.create_blend_space's 1D-derives-from-2D case).
    //
    // Rules, in order:
    //   1. nothing at the path                          -> Create
    //   2. class matches, overwrite not requested        -> UpdateInPlace
    //   3. class does NOT match                          -> Rejected ASSET_ALREADY_EXISTS
    //   4. overwrite requested, registry referencers      -> Rejected ASSET_IN_USE
    //      overwrite requested, no registry referencers   -> delete; if the delete is
    //          refused because something still holds the object IN MEMORY (typically a
    //          level actor spawned from it) -> Rejected ASSET_IN_USE naming those actors
    //      overwrite requested, delete succeeded          -> Create
    PINWRIGHT_API FResolution Resolve(const FString& PackageName, const FString& AssetName,
                                      UClass* ExpectedClass, bool bOverwriteRequested,
                                      bool bRequireExactClass = false);

    // Additive success fields: existing (bool) and mode ("created" | "updated_in_place").
    PINWRIGHT_API void AddCreateReport(const TSharedPtr<FJsonObject>& Out, const FResolution& Resolution);

    // Error data for a Rejected resolution: referencers / referencerCount (on-disk
    // packages) and referencingActors / referencingActorCount (live level actors) for
    // ASSET_IN_USE, existingClass / existingPath for ASSET_ALREADY_EXISTS. Both
    // ASSET_IN_USE pairs are always emitted so the payload shape does not change
    // between the two refusal branches; the pair that does not apply reads [] / 0.
    // Returns an invalid pointer when Resolution is not Rejected.
    PINWRIGHT_API TSharedPtr<FJsonObject> MakeErrorData(const FResolution& Resolution);

    // Sends the rejection through Ctx with its error data attached. Always returns
    // true so a handler can `return AssetCreatePolicy::SendRejection(Ctx, R);`.
    PINWRIGHT_API bool SendRejection(FHandlerContext& Ctx, const FResolution& Resolution);
}
