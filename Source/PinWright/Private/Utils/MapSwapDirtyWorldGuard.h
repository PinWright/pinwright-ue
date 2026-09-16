// Copyright (c) 2026 Alexander Penkin. MIT License.

// MapSwapDirtyWorldGuard.h - the refusable precondition on FEditorFileUtils::LoadMap.
//
// Why this exists (board B-level-load-dirty-world-fatal /
// B-level-load-dirty-world-memory-leak-fatal): `level.load` killed a shared editor
// outright, taking six attached agents' sessions with it. The fatal is NOT the
// tick-reentrancy assertion Dispatch/SafePoint.h already closed - the shipped callstack
// shows the safe-point deferral firing and doing its job - it is a second, independent
// appError one frame later, inside UEditorEngine::Map_Load.
//
// THE ENGINE MECHANISM, read from source (UE 5.8 EditorServer.cpp; identical shape on
// 5.3-5.7, only the line numbers move).
//
// Map_Load resolves the map it was asked to open to a long package name and asks whether
// that package is ALREADY RESIDENT (:2399 `ExistingPackage = FindPackage(nullptr,
// *LongTempFname)`, :2403 world-or-redirector lookup). A resident target is normal - it is
// what happens whenever a map was opened, or created and edited, earlier in the session.
// The engine's plan for it is to unload it and re-read it from disk:
//
//   :2480  EditorDestroyWorld(Context, ..., NewWorld)     tear the OUTGOING world down
//   :2485  TObjectIterator<UWorld> -> WorldPackages       every other resident world
//   :2494  UPackageTools::UnloadPackages(WorldPackages)   unload them
//   :2498  re-find ExistingPackage / ExistingWorld        did the target actually go?
//   :2520  if it did not, dump reference chains and
//   :2544  UE_LOGF(Fatal) "World Memory Leaks"            <- unconditional appError
//
// `UPackageTools::UnloadPackages` refuses to unload a DIRTY package by policy, not by
// reachability (PackageTools.cpp:381 `if (!Params.bUnloadDirtyPackages &&
// TopLevelPackage->IsDirty())`); it collects those into a message and skips them. So a
// resident-and-dirty target survives step :2494 by design, and step :2544 then kills the
// process. Interactively the skipped-packages message is a modal the user reads and acts
// on ("The following assets have been modified and cannot be unloaded"). Under the
// unattended operation every PinWright RPC runs in, that modal is auto-answered Ok, which
// removes the only thing standing between the caller and the appError. Unattended mode
// converts a user-refusable state into a process kill.
//
// WHAT BLOCKS AND WHAT DOES NOT. The leak check reads the TARGET package only, so this
// guard does too. Precisely (the facts FTargetWorldState carries, mirroring Map_Load's own
// branches):
//
//   * target package not resident        -> nothing to unload, never fatal.
//   * resident, but the world is already
//     unrooted AND has no RF_Standalone   -> GARBAGE-IN-WAITING. `EditorDestroyWorld` at :2480
//                                           runs Cleanse -> CollectGarbage(GARBAGE_COLLECTION_
//                                           KEEPFLAGS) BEFORE the `TObjectIterator<UWorld>`
//                                           sweep at :2485, so this world is already gone by
//                                           the time the sweep, the unload and the leak check
//                                           run. Its dirty flag cannot reach the fatal. This
//                                           is the engine's own discriminator, printed
//                                           verbatim in the shipped crash log for the world
//                                           that DID leak: "is not currently reachable but it
//                                           does have some of GARBAGE_COLLECTION_KEEPFLAGS
//                                           set" (= RF_Standalone in the editor,
//                                           GarbageCollection.h:28).
//   * resident, clean                    -> UnloadPackages takes it; not blocked here. It
//                                           can still fail to GC when something holds a
//                                           real reference, which no flag predicts; that
//                                           residual is unchanged by this guard.
//   * resident, dirty, world uninitialized -> Map_Load KEEPS it as `NewWorld` and reuses it
//                                           (:2470 IsWorldValidForReuse = !HasEverBeenInitialized),
//                                           so it is never unloaded and never checked. Not
//                                           blocked - refusing here would break the ordinary
//                                           level.create -> level.load flow.
//   * resident, dirty, world initialized -> FATAL at :2544 (the shipped crash).
//   * resident, dirty, package holds no
//     world at all                       -> FATAL at :2544 via its second branch
//                                           (:2520 `ExistingPackage && !ExistingWorld`):
//                                           :2500-2510 tries a targeted unload of that
//                                           package first, and dirty defeats that one too.
//
// The OUTGOING world is deliberately out of scope. EditorDestroyWorld strips it explicitly
// (EditorServer.cpp:1990-1991 `ClearFlags(RF_Standalone | RF_Transactional)` +
// `RemoveFromRoot()`, then :2009 `SetFlags(RF_Transient)`), so its dirty flag does not keep it
// alive and it is collected normally - which the shipped log confirms: the outgoing world
// (T_AI) cleaned up fine while the dirty resident target (FPS_Compound) did not. Blocking
// on "any dirty world package" would therefore refuse the common, safe swap - editing map A
// and then opening map B - and make the verb unusable in exactly the multi-agent editor the
// ticket is about.
//
// Also excluded: a resident target that IS the current editor world. Reached through a
// redirector whose destination is the live world (level.load's own already-loaded early-out
// compares names and does not follow redirectors), it takes the EditorDestroyWorld path
// above rather than the unload sweep.
//
// THE GARBAGE-IN-WAITING EXCLUSION IS NOT A CONVENIENCE, it is the difference between the
// defect and its most common look-alike, and leaving it out breaks working callers. Any verb
// that swaps the active world by hand ends with `UWorld::DestroyWorld`, which does
// `RemoveFromRoot()` + `ClearFlags(RF_Standalone)` (World.cpp:2792-2793) but runs NO collect -
// `level.structure.create_level` (LevelStructureHandler.cpp) is the in-tree example, and the
// test harness's map-restore guard calls level.load straight afterwards. The previous map is
// therefore resident, initialized, possibly dirty, and completely harmless: the next
// Map_Load's own CollectGarbage reclaims it. Reading only the dirty flag refused those loads
// and stranded the editor on a torn-down world.
//
// WHY REFUSE RATHER THAN SAVE BY DEFAULT. Saving is a write to someone else's file. In the
// configuration this ticket came from, the dirty map usually belongs to a DIFFERENT agent,
// and a silent save publishes their half-finished work. So the default is a typed refusal
// naming the package, and SaveBlockingWorldPackage is opt-in - the same shape asset.save
// uses for SAVE_DISK_STATE_DIVERGED / overwriteDiskChanges, and level.delete for
// ASSET_IN_USE / force.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace PinWrightMapSwapGuard
{
    // The facts Map_Load's leak check reads, plus the package name it reads them from.
    // Gathered by ProbeTargetWorld; judged by WouldMapLoadFatal.
    struct FTargetWorldState
    {
        // Long package name Map_Load will FindPackage, redirector-followed so it names the
        // package that actually has to go. Empty when the path has no mounted form, in
        // which case Map_Load's own conversion fails too and nothing is at risk.
        FString PackageName;

        // A UPackage under PackageName is live in memory right now.
        bool bPackageResident = false;

        // ...and UPackageTools::UnloadPackages will refuse to unload it.
        bool bPackageDirty = false;

        // A UWorld was found inside it (directly or through a world redirector).
        bool bWorldFound = false;

        // The world is rooted or carries RF_Standalone, so it will still be there after the
        // CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS) that EditorDestroyWorld runs BEFORE the
        // unload sweep. False means garbage-in-waiting: already reclaimed by the time the leak
        // check looks, so it cannot be the leak. Meaningless when bWorldFound is false.
        bool bWorldSurvivesEditorCollect = false;

        // UWorld::HasEverBeenInitialized. False means Map_Load may keep and reuse the world
        // instead of unloading it, which is the one dirty case that is NOT fatal.
        bool bWorldEverInitialized = false;

        // The resident world is the live editor world, which EditorDestroyWorld tears down
        // on a path the dirty flag does not obstruct.
        bool bIsCurrentEditorWorld = false;
    };

    // The pure decision. Reproduces the reachability of EditorServer.cpp:2520-2544 exactly;
    // every field of FTargetWorldState corresponds to one branch cited in the header comment
    // above. Takes the struct rather than a row of positional bools deliberately: the facts
    // are near-synonymous booleans and a caller that transposes two of them gets a silently
    // wrong verdict on a crash-preventing guard. Pure, so a test builds a state by name and
    // needs no world, no map and no editor.
    PINWRIGHT_API bool WouldMapLoadFatal(const FTargetWorldState& State);

    // Read the live state of the map FEditorFileUtils::LoadMap is about to be handed.
    // FileOrPackagePath is the SAME string the LoadMap call will receive (a .umap filename
    // or a package path), resolved through FPackageName::TryConvertToMountedPath - the very
    // conversion LoadMap performs - so the probe and the engine can never disagree about
    // which package is at stake. Reads only: FindPackage, never LoadPackage.
    PINWRIGHT_API FTargetWorldState ProbeTargetWorld(const FString& FileOrPackagePath);

    // Caller-facing sentence for a blocked state: what is in the way and what to do about
    // it. SaveFailureReason is appended when an opted-in save was attempted and did not
    // clear the block; pass an empty string otherwise.
    PINWRIGHT_API FString DescribeRefusal(
        const FTargetWorldState& State,
        const FString& SaveFailureReason);

    // Opt-in remedy: write the blocking package to disk so UnloadPackages will take it, then
    // RE-PROBE and report whether the block actually cleared. The verdict is the re-probe,
    // never PromptForCheckoutAndSave's return code - a save that reports success but leaves
    // the package dirty must not be allowed to wave LoadMap through. InOutState is replaced
    // with the post-save reading either way. Returns true only when the map swap is now
    // safe; OutFailureReason is non-empty on every false.
    PINWRIGHT_API bool SaveBlockingWorldPackage(
        FTargetWorldState& InOutState,
        FString& OutFailureReason);

    // ---------------------------------------------------------------------------
    // The SECOND fatal on this path, and the one the target-package guard above cannot
    // see (board B-dirty-world-guard-only-covers-requested-map,
    // B-open-level-fatal-python-held-pie-world).
    //
    // Map_Load -> EditorDestroyWorld -> Cleanse -> UEditorEngine::CheckForWorldGCLeaks
    // (EditorServer.cpp:1911-1957) walks EVERY resident UWorld after the collect and
    // errors on any that is not the world being kept, is not one of the three world
    // types the editor keeps across a swap, and belongs to no FWorldContext:
    //
    //     :1918  Inactive / EditorPreview / GamePreview  -> kept, never counted
    //     :1925  UEngine::WorldHasValidContext(World)    -> live, never counted
    //     :1927  everything else                          -> counted as a leak
    //     :1949  Editor.CheckForWorldGCLeaksAreFatal      -> Fatal (default) or Error
    //     :1951  UE_LOGF(Fatal) "World Memory Leaks"      -> process kill
    //
    // The severity is CVAR-GATED, not unconditional: `Editor.CheckForWorldGCLeaksAreFatal`
    // defaults true, and setting it false degrades the kill to a logged Error. That is an
    // operator lever for a session that must survive a swap it cannot otherwise make; it is
    // not a fix, because the leaked world stays leaked. This guard exists because the
    // default is what every editor actually runs with.
    //
    // Nothing in that predicate reads a dirty flag. An ordinary dirty outgoing map
    // package is NOT what reaches it - EditorDestroyWorld strips the outgoing world's
    // keep-flags (:1990-1991) and collects it - which is why a guard built on package
    // dirtiness both misses the real killers and refuses safe swaps. What reaches it is a
    // world that some referencer keeps alive past the collect. The two observed holders:
    //
    //   * a PIE world whose FWorldContext EndPlayMap destroyed, still wrapped by the
    //     Python plugin's reference collector after a python.execute touched one of its
    //     objects (FPyReferenceCollector::AddReferencedObjects names it in the crash log);
    //   * a world some verb tore down by hand and something still holds.
    //
    // So the guard is a PROBE, not a flag read: ask every listener to release the dead
    // worlds, run the same collect Map_Load will run, and see what is still there. Only
    // an actual post-collect survivor refuses; anything the collect reclaims is silent.
    //
    // WHAT THIS PROBE STRUCTURALLY CANNOT SEE, and must not be read as covering: the
    // OUTGOING world itself, and its package. At probe time that world still owns its
    // FWorldContext, so the engine's own predicate excludes it and so does this one; it
    // only becomes a candidate after EditorDestroyWorld has cleared the context and run
    // Cleanse, which is inside the call we are trying to decide about. The engine checks it
    // separately in the same pass (the WorldPackage half, EditorServer.cpp:1932-1945). A
    // pre-flight cannot answer that question without performing the teardown, so the
    // outgoing direction remains a residual risk that no refusal here covers - it is not
    // "guarded, and clean".
    // ---------------------------------------------------------------------------

    // One resident world CheckForWorldGCLeaks would count, with enough identity for the
    // caller to act on it.
    struct FResidentWorldSurvivor
    {
        // UObject path name, e.g. /Temp/Untitled_3.L_Arena - the same spelling the engine
        // prints in the crash log's reference-chain root.
        FString WorldPath;

        FString PackageName;

        // LexToString(EWorldType::Type): PIE / Editor / Game / None / ...
        FString WorldType;

        // Already marked garbage and still reachable, which is the shape the Python
        // wrapper holder produces.
        bool bGarbage = false;

        // Shortest reference chain to a root, from FReferenceChainSearch - or, when no chain
        // exists because the holder is not a UObject (a ref-counted strong pointer, which is
        // what the Python wrapper registry uses), a sentence saying so and why. Empty only
        // when the report was skipped by the per-refusal cap.
        FString ReferencedBy;
    };

    struct FWorldSurvivorProbeResult
    {
        // Non-empty means the map swap must be refused.
        TArray<FResidentWorldSurvivor> Survivors;

        // Dead worlds the probe asked every listener to release before collecting.
        int32 PurgedWorldCount = 0;

        // A collect actually ran. False on the fast path, where no world was counted in
        // the first place and there was nothing to purge or reclaim.
        bool bRanCollect = false;

        // The probe could not run at all, because collecting right now would itself be
        // illegal (a collect already in flight, or a synchronous load on this stack -
        // GarbageCollection.cpp asserts check(!IsLoading()) on that one). Non-empty
        // UnavailableReason. The caller must NOT treat this as "clear to swap": it measured
        // nothing. Retry once the editor is idle.
        bool bProbeUnavailable = false;
        FString UnavailableReason;

        bool IsBlocked() const { return Survivors.Num() > 0; }
    };

    // CheckForWorldGCLeaks's predicate (EditorServer.cpp:1918-1927), plus two narrowings.
    // Both only ever REMOVE candidates, so either can hide a survivor the engine would
    // count; neither can invent one.
    //   * A streaming sublevel whose owning world still has a context goes with its owner.
    //     Borrowed from UEngine::CheckAndHandleStaleWorldObjectReferences
    //     (UnrealEngine.cpp:17388) - a DIFFERENT predicate, on a different code path;
    //     CheckForWorldGCLeaks has no such clause. It is here because a sublevel world
    //     built by hand is not the EWorldType::Inactive that spares a disk-loaded one.
    //   * The map being opened is Map_Load's own business: it may keep an uninitialized
    //     copy as NewWorld, and WouldMapLoadFatal refuses the dirty case ahead of this.
    // TargetPackageName may be empty when there is no incoming package.
    PINWRIGHT_API bool IsWorldCountedByLeakCheck(UWorld* World, const FString& TargetPackageName);

    // The refusable precondition. Broadcasts FEditorSupportDelegates::
    // PrepareToCleanseEditorObject for every counted world - the same delegate
    // EditorDestroyWorld broadcasts for the world it is about to tear down, and the only
    // public way to reach FPyReferenceCollector::PurgeUnrealObjectReferences - then flushes
    // async loading and asset compilation and runs CollectGarbage (whose pre-GC delegate
    // drives the interpreter's own gc.collect), then re-enumerates. Survivors are what the
    // engine's leak check is about to find. Costs one UWorld iteration and nothing else
    // when no world is counted; sets bProbeUnavailable rather than collecting when a
    // collect would be illegal on this stack.
    //
    // bTransactionBufferWillBeCleared says whether the CALLER's engine entry point resets
    // the undo buffer before the leak check runs, which decides whether a world held only
    // by that buffer is safe. It is true for Map_Load (ResetTransaction at
    // EditorServer.cpp:2456, before EditorDestroyWorld at :2480) and FALSE for NewMap,
    // which calls EditorDestroyWorld at :2207 and only resets at :2256, long after
    // CheckForWorldGCLeaks has already fired.
    PINWRIGHT_API FWorldSurvivorProbeResult ProbeResidentWorldSurvivors(
        const FString& TargetPackageName,
        bool bTransactionBufferWillBeCleared);

    // Caller-facing sentence for a blocked probe: which worlds are in the way, what is
    // holding them, and what the probe already did to the editor on the way to finding out.
    PINWRIGHT_API FString DescribeSurvivorRefusal(const FWorldSurvivorProbeResult& Result);

    // The one error payload for both refusal shapes, so every map-swapping verb reports the
    // same fields. Built here rather than per handler because the check is shared.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSurvivorErrorData(
        const FString& LevelPath,
        const FWorldSurvivorProbeResult& Result);
}
