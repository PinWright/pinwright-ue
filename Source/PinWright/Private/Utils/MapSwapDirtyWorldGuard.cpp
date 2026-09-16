// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/MapSwapDirtyWorldGuard.h"

#include "Compat/EngineVersionCompat.h"
#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorSupportDelegates.h"
#include "Editor/Transactor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Selection.h"
#include "UObject/GCObjectInfo.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightMapSwapGuard, Log, All);

namespace PinWrightMapSwapGuard
{

bool WouldMapLoadFatal(const FTargetWorldState& State)
{
    if (!State.bPackageResident || !State.bPackageDirty || State.bIsCurrentEditorWorld)
    {
        // Nothing to unload, or nothing UnloadPackages will refuse, or the world leaves by
        // the EditorDestroyWorld route that clears its own keep-flags.
        return false;
    }

    if (!State.bWorldFound)
    {
        // Package resident with no world in it: Map_Load's targeted unload (:2500-2510) is
        // defeated by the dirty flag and it reaches the fatal's second branch.
        return true;
    }

    if (!State.bWorldSurvivesEditorCollect)
    {
        // Garbage-in-waiting. EditorDestroyWorld's CollectGarbage runs before the sweep, so
        // this world is gone by the time anything looks for it. The dirty flag is moot.
        return false;
    }

    // A world Map_Load may keep and reuse is never unloaded and never leak-checked; a dirty
    // initialized one that survives the collect reaches the unconditional appError.
    return State.bWorldEverInitialized;
}

FTargetWorldState ProbeTargetWorld(const FString& FileOrPackagePath)
{
    FTargetWorldState State;

    // The same conversion FEditorFileUtils::LoadMap performs on its argument
    // (FileHelpers.cpp: TryConvertToMountedPath -> LongMapPackageName), which Map_Load then
    // re-derives and hands to FindPackage. Sharing it is what keeps this probe and the
    // engine's leak check pointed at the same package.
    FString LongPackageName;
    if (!FPackageName::TryConvertToMountedPath(FileOrPackagePath, /*OutLocalPathNoExtension=*/nullptr,
            &LongPackageName, /*OutObjectName=*/nullptr, /*OutSubObjectName=*/nullptr,
            /*OutExtension=*/nullptr))
    {
        // Map_Load's own conversion fails identically and it never reaches the leak check.
        return State;
    }
    State.PackageName = LongPackageName;

    // FindPackage, never LoadPackage: an absent package cannot block anything, and loading
    // one here would be an unrequested side effect on the game thread.
    UPackage* Package = FindPackage(nullptr, *LongPackageName);
    if (!Package)
    {
        return State;
    }

    // Mirrors Map_Load's FindWorldInPackageOrFollowRedirector (EditorServer.cpp:2352): a
    // world redirector moves the examined package to the destination world's own package,
    // which is the one that has to be unloaded. Same idiom the engine uses in
    // World.cpp:4523-4533.
    UWorld* World = UWorld::FindWorldInPackage(Package);
    if (!World)
    {
        World = UWorld::FollowWorldRedirectorInPackage(Package);
        if (World)
        {
            Package = World->GetOutermost();
        }
    }

    State.PackageName = Package->GetName();
    State.bPackageResident = true;
    State.bPackageDirty = Package->IsDirty();
    State.bWorldFound = World != nullptr;
    // GARBAGE_COLLECTION_KEEPFLAGS is RF_Standalone in the editor (GarbageCollection.h:28),
    // so this is literally the set the Cleanse inside EditorDestroyWorld will spare, plus the
    // root set. A world already stripped of both by an earlier hand-rolled world swap
    // (UWorld::DestroyWorld, World.cpp:2792-2793) does not survive to be leak-checked.
    State.bWorldSurvivesEditorCollect =
        World != nullptr && (World->IsRooted() || World->HasAnyFlags(RF_Standalone));
    State.bWorldEverInitialized = World != nullptr && World->HasEverBeenInitialized();
    State.bIsCurrentEditorWorld =
        World != nullptr && GEditor != nullptr && GEditor->GetEditorWorldContext().World() == World;
    return State;
}

FString DescribeRefusal(const FTargetWorldState& State, const FString& SaveFailureReason)
{
    const TCHAR* const Holder = State.bWorldFound
        ? TEXT("an initialized world")
        : TEXT("no world (only a stale package)");

    FString Message = FString::Printf(
        TEXT("Refusing the map swap: '%s' is already loaded in this editor with unsaved changes, "
             "and it holds %s. UEditorEngine::Map_Load must unload that package before it can "
             "re-read the map from disk, UPackageTools::UnloadPackages refuses to unload a dirty "
             "package, and Map_Load then hits an unconditional Fatal ('World Memory Leaks', "
             "EditorServer.cpp) that kills the whole editor process and every session attached to "
             "it. Nothing has been changed. Remedies: save that map (level.load with "
             "saveDirtyTargetWorld:true saves exactly this one package and then loads, keeping the "
             "unsaved edits; editor.save_all saves it along with everything else that is dirty), "
             "or discard its in-memory edits before retrying. editor.list_dirty_packages shows "
             "the full dirty set."),
        *State.PackageName, Holder);

    if (!SaveFailureReason.IsEmpty())
    {
        Message += FString::Printf(
            TEXT(" saveDirtyTargetWorld was requested and did not clear the block: %s"),
            *SaveFailureReason);
    }
    return Message;
}

bool SaveBlockingWorldPackage(FTargetWorldState& InOutState, FString& OutFailureReason)
{
    OutFailureReason.Reset();

    UPackage* Package = FindPackage(nullptr, *InOutState.PackageName);
    if (!Package)
    {
        OutFailureReason = FString::Printf(
            TEXT("package '%s' is no longer loaded"), *InOutState.PackageName);
        return false;
    }

    // bCheckDirty=false because the caller already measured the dirty flag; bPromptToSave=false
    // is what keeps the save dialog out. Same call shape Utils/RedirectorFixupPolicy.cpp uses,
    // and the same one FEditorFileUtils::SaveDirtyPackages funnels map packages through, so a
    // map that is not the active editor world is saved correctly.
    TArray<UPackage*> ToSave;
    ToSave.Add(Package);
    TArray<UPackage*> FailedToSave;
    const FEditorFileUtils::EPromptReturnCode SaveCode =
        FEditorFileUtils::PromptForCheckoutAndSave(ToSave, /*bCheckDirty=*/false,
            /*bPromptToSave=*/false, &FailedToSave);

    // The VERDICT is the re-probe, not SaveCode. A save that reports success but leaves the
    // package dirty (read-only file, source-control refusal, a save handler that re-dirties)
    // must not be allowed to wave LoadMap through into the appError.
    const FString PackageName = InOutState.PackageName;
    InOutState = ProbeTargetWorld(PackageName);
    if (!WouldMapLoadFatal(InOutState))
    {
        UE_LOG(LogPinWrightMapSwapGuard, Log,
            TEXT("Saved blocking world package '%s' before the map swap (save code %d)."),
            *PackageName, static_cast<int32>(SaveCode));
        return true;
    }

    OutFailureReason = SaveCode == FEditorFileUtils::PR_Success
        ? FString::Printf(TEXT("the save reported success but '%s' is still dirty"), *PackageName)
        : FString::Printf(TEXT("FEditorFileUtils::PromptForCheckoutAndSave returned %d for '%s'"),
            static_cast<int32>(SaveCode), *PackageName);
    UE_LOG(LogPinWrightMapSwapGuard, Warning,
        TEXT("Map swap still blocked after the opted-in save: %s"), *OutFailureReason);
    return false;
}

// ---------------------------------------------------------------------------
// The post-cleanse survivor probe (see the header for the engine mechanism).
// ---------------------------------------------------------------------------

// Payload bound. A refusal exists to be read, and a reference chain for the tenth dead
// world helps nobody; the first few name the holder.
static constexpr int32 MapSwapMaxChainReports = 4;
static constexpr int32 MapSwapMaxChainChars = 1500;

// Holders the engine empties BEFORE its own leak check, so a world reachable only through
// one of them is released by the swap itself and must NOT be refused. Read off the live
// objects rather than naming classes, so a class rename cannot silently turn the exemption
// off.
//
// The two holders do NOT have the same reach, and getting that wrong is a crash:
//
//   * The SELECTION sets are cleared inside EditorDestroyWorld itself (SelectNone, plus
//     Cleanse's DeselectAll), so they are cleared before CheckForWorldGCLeaks on EVERY
//     entry point. Always exempt.
//   * The UNDO buffer is not. Map_Load calls ResetTransaction at EditorServer.cpp:2456,
//     BEFORE EditorDestroyWorld at :2480 - cleared in time. NewMap calls EditorDestroyWorld
//     at :2207 and ResetTransaction only at :2256, i.e. AFTER the leak check has already
//     fired. So on the NewMap entry points a world held by the undo buffer is a genuine
//     killer and exempting it would hand the caller the crash this guard exists to stop.
//     Hence the flag rather than a fixed set.
static TSet<FName> GatherClearedHolderClasses(bool bTransactionBufferWillBeCleared)
{
    TSet<FName> Classes;
    if (!GEditor)
    {
        return Classes;
    }
    if (bTransactionBufferWillBeCleared && GEditor->Trans)
    {
        Classes.Add(GEditor->Trans->GetClass()->GetFName());
    }
    if (USelection* Selection = GEditor->GetSelectedActors())
    {
        Classes.Add(Selection->GetClass()->GetFName());
    }
    return Classes;
}

static bool MapSwapChainPassesThroughClearedHolder(
    const FReferenceChainSearch::FReferenceChain& Chain, const TSet<FName>& ClearedClasses)
{
    for (int32 Index = 0; Index < Chain.Num(); ++Index)
    {
        const FReferenceChainSearch::FGraphNode* Node = Chain.GetNode(Index);
        const FGCObjectInfo* Info = Node ? Node->ObjectInfo : nullptr;
        const FGCObjectInfo* ClassInfo = Info ? Info->GetClass() : nullptr;
        if (!ClassInfo)
        {
            continue;
        }
        // FGCObjectInfo::GetName() is 5.8-only. Pre-5.8 the stored FName is private and the class
        // name is reachable only through the OWNER's GetClassName(), which returns that same
        // Class->Name as a string (GCObjectInfo.h) - the null check above is its check(Class).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        const FName ClassName = ClassInfo->GetName();
#else
        const FName ClassName(*Info->GetClassName());
#endif
        if (ClearedClasses.Contains(ClassName))
        {
            return true;
        }
    }
    return false;
}

// Only an info the search built for itself is safe to read. FGCObjectInfo::GetClassName is a
// bare check(Class) (GCObjectInfo.h:129), GetPathName walks Outer->Class unguarded
// (GCObjectInfo.cpp:49) and TryResolveObject dereferences Class (:25) - and Class is filled in
// by FGCObjectInfo::FindOrAddInfoHelper (:88), never by the public constructor (GCObjectInfo.h:32),
// so an info from anywhere else can kill the process on a field read.
static bool MapSwapNodeInfoIsFullyDescribed(const FGCObjectInfo* Info)
{
    if (!Info)
    {
        return false;
    }
    for (const FGCObjectInfo* Current = Info; Current != nullptr; Current = Current->GetOuter())
    {
        if (Current->GetClass() == nullptr)
        {
            return false;
        }
    }
    return true;
}

// What the engine's own no-chain diagnostic reports (ReferenceChainSearch.cpp:1736-1764), read
// off the live world instead of an FGCObjectInfo. "No reference chain" is not "no holder": a
// ref count keeps an object past GC with no UObject pointing at it, which is exactly the shape
// of TStrongObjectPtr (it calls UObject::AddRef/ReleaseRef, StrongObjectPtr.cpp) and of the
// Python plugin's wrapper registry - so the search has nothing to name and the refusal has to
// say so itself.
static FString DescribeUnreferencedHolder(const UWorld* World)
{
    if (World->IsRooted())
    {
        return TEXT("no reference chain: the world is in the GC root set (AddToRoot)");
    }
    if (World->HasAnyFlags(GARBAGE_COLLECTION_KEEPFLAGS))
    {
        return TEXT("no reference chain: the world carries GARBAGE_COLLECTION_KEEPFLAGS "
                    "(RF_Standalone in the editor), which spares it from the collect");
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    const int32 RefCount = World->GetRefCount();
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // UObjectBaseUtility::GetRefCount arrived in 5.7 and is exactly this lookup; the FUObjectItem
    // has carried the count since 5.5.
    const FUObjectItem* WorldItem =
        GUObjectArray.IndexToObject(static_cast<int32>(World->GetUniqueID()));
    const int32 RefCount = WorldItem ? WorldItem->GetRefCount() : 0;
#else
    // UObject ref counting (UObject::AddRef / FUObjectItem::GetRefCount) does not exist before
    // 5.5: TStrongObjectPtr is an FGCObject there, which the reference-chain search CAN name, so
    // on 5.4 there is no unattributable count to report.
    const int32 RefCount = 0;
#endif
    if (RefCount > 0)
    {
        return FString::Printf(
            TEXT("no reference chain: %d non-UObject strong reference(s) hold this world "
                 "(TStrongObjectPtr / UObject::AddRef, as the Python wrapper registry does), "
                 "and a reference chain search cannot name that kind of holder"),
            RefCount);
    }
    return TEXT("no reference chain, no root flag and no ref count: the world was still "
                "reachable at collect time from a referencer the search could not attribute");
}

bool IsWorldCountedByLeakCheck(UWorld* World, const FString& TargetPackageName)
{
    if (!World)
    {
        return false;
    }

    // EditorServer.cpp:1918 - the world types the editor keeps across a map swap. A world
    // loaded from a package is EWorldType::Inactive by default (World.cpp:1693), which is
    // why an ordinary resident map - dirty or not - is never counted here.
    switch (World->WorldType)
    {
    case EWorldType::Inactive:
    case EWorldType::EditorPreview:
    case EWorldType::GamePreview:
        return false;
    default:
        break;
    }

    // EditorServer.cpp:1925. The outgoing editor world still owns its FWorldContext at
    // probe time and is excluded by this, which is what we want: EditorDestroyWorld tears
    // it down on a path of its own, and whether it survives that is not readable from
    // here.
    if (!GEngine || GEngine->GetWorldContextFromWorld(World) != nullptr)
    {
        return false;
    }

    // UnrealEngine.cpp:17388 - a streaming sublevel world belongs to its owner and goes
    // with it. CheckForWorldGCLeaks omits this narrowing only because a sublevel loaded
    // from disk is already Inactive and never reaches its predicate; one built by hand is
    // not.
    if (World->PersistentLevel && World->PersistentLevel->OwningWorld
        && World->PersistentLevel->OwningWorld != World
        && GEngine->GetWorldContextFromWorld(World->PersistentLevel->OwningWorld) != nullptr)
    {
        return false;
    }

    // The map being opened is Map_Load's own business: it may keep an uninitialized copy
    // as NewWorld (EditorServer.cpp:2470) and runs a separate leak check on the rest
    // (:2520), which WouldMapLoadFatal already refuses ahead of. Counting it here would
    // refuse the ordinary level.create -> level.load flow.
    if (!TargetPackageName.IsEmpty())
    {
        const UPackage* WorldPackage = World->GetPackage();
        if (WorldPackage && WorldPackage->GetName() == TargetPackageName)
        {
            return false;
        }
    }
    return true;
}

FWorldSurvivorProbeResult ProbeResidentWorldSurvivors(
    const FString& TargetPackageName, bool bTransactionBufferWillBeCleared)
{
    FWorldSurvivorProbeResult Result;

    // The probe's whole method is "collect, then look", so a stack where collecting is
    // illegal cannot be measured - and must not be reported as clean. CollectGarbage
    // asserts check(!IsLoading()) outright (GarbageCollection.cpp), and a reentrant collect
    // is its own crash. The dispatcher's own GC gate does not cover this: a cross-dispatched
    // call (editor.open_level -> level.load) bypasses ProcessRequest.
    if (IsGarbageCollecting())
    {
        Result.bProbeUnavailable = true;
        Result.UnavailableReason =
            TEXT("a garbage collection is already in flight, so the pre-swap collect this "
                 "check depends on cannot run");
        return Result;
    }
    if (IsLoading())
    {
        Result.bProbeUnavailable = true;
        Result.UnavailableReason =
            TEXT("a synchronous package load is on this call stack; CollectGarbage asserts "
                 "check(!IsLoading()) there, so the pre-swap collect this check depends on "
                 "cannot run");
        return Result;
    }

    TArray<UWorld*> Candidates;
    for (TObjectIterator<UWorld> It; It; ++It)
    {
        if (IsWorldCountedByLeakCheck(*It, TargetPackageName))
        {
            Candidates.Add(*It);
        }
    }

    // The common path by a wide margin: nothing the leak check would count is resident, so
    // nothing can survive a collect. One UWorld iteration, no GC, no chain search.
    if (Candidates.Num() == 0)
    {
        return Result;
    }

    // The only public way to reach the Python plugin's wrapper registry: its
    // FPythonScriptPlugin::OnPrepareToCleanseEditorObject calls
    // FPyReferenceCollector::PurgeUnrealObjectReferences(Object, /*inners=*/true), and
    // both that class and its header are private to that plugin. EditorDestroyWorld
    // broadcasts the same delegate for the world it is about to tear down
    // (EditorServer.cpp:2043), so this is the engine's own release notification applied to
    // worlds the engine has already stopped tracking.
    for (UWorld* World : Candidates)
    {
        FEditorSupportDelegates::PrepareToCleanseEditorObject.Broadcast(World);
    }
    Result.PurgedWorldCount = Candidates.Num();
    Candidates.Reset();

    // Both drains precede the collect, following the folder-sweep release step in
    // Handlers/Asset/AssetDumpHandler.cpp (RunFolderDumpReleaseStep): a
    // collect that frees an object with an in-flight async load or DDC build tears it out
    // from under a worker thread, and GFlushStreamingOnGC defaults to 0 so CollectGarbage
    // does not do this for us.
    FlushAsyncLoading();
    FAssetCompilingManager::Get().FinishAllCompilation();

    // The collect Map_Load is about to run anyway (Cleanse -> CollectGarbage). The
    // interpreter's own gc.collect goes first, off
    // FCoreUObjectDelegates::GetPreGarbageCollectDelegate, which the Python plugin hooks -
    // that is what releases wrappers trapped in Python reference cycles.
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);
    Result.bRanCollect = true;

    TArray<UWorld*> Remaining;
    TArray<UObject*> RemainingObjects;
    for (TObjectIterator<UWorld> It; It; ++It)
    {
        if (IsWorldCountedByLeakCheck(*It, TargetPackageName))
        {
            Remaining.Add(*It);
            RemainingObjects.Add(*It);
        }
    }
    if (Remaining.Num() == 0)
    {
        return Result;
    }

    // One graph build for every survivor, not one per survivor: the search is expensive and
    // only runs on this path, where the alternative is a killed editor.
    const TSet<FName> ClearedHolders =
        GatherClearedHolderClasses(bTransactionBufferWillBeCleared);

    // The mode the engine's own stale-reference reporter composes
    // (ReferenceChainSearch.cpp:1951-1959). GCOnly drops the disregard-for-GC set, whose
    // members are never collected and whose outgoing references GC does not even walk, and a
    // target that is already garbage needs ShortestToGarbage - Shortest only reaches roots, so
    // a dead world held by another dead object gets no chain at all under it.
    bool bAnyGarbage = false;
    for (const UObject* RemainingObject : RemainingObjects)
    {
        bAnyGarbage = bAnyGarbage || !IsValid(RemainingObject);
    }
    const EReferenceChainSearchMode SearchMode = EReferenceChainSearchMode::GCOnly
        | (bAnyGarbage ? EReferenceChainSearchMode::ShortestToGarbage
                       : EReferenceChainSearchMode::Shortest);
    FReferenceChainSearch Search(RemainingObjects, SearchMode);
    const TArray<FReferenceChainSearch::FReferenceChain*>& Chains = Search.GetReferenceChains();

    for (UWorld* World : Remaining)
    {
        const FString WorldPath = World->GetPathName();

        // FReferenceChain::GetNode(0) is the chain's target object, so this is how a
        // multi-target search is read back per target.
        bool bHasChain = false;
        bool bEveryChainCleared = true;
        bool bRootPathWillFindThisWorld = false;
        for (const FReferenceChainSearch::FReferenceChain* Chain : Chains)
        {
            if (!Chain || Chain->Num() == 0)
            {
                continue;
            }
            const FReferenceChainSearch::FGraphNode* TargetNode = Chain->GetNode(0);
            FGCObjectInfo* TargetInfo = TargetNode ? TargetNode->ObjectInfo : nullptr;
            if (!MapSwapNodeInfoIsFullyDescribed(TargetInfo)
                || TargetInfo->GetPathName() != WorldPath)
            {
                continue;
            }
            bHasChain = true;
            // GetRootPath's own predicate (ReferenceChainSearch.cpp:1797). A path match is not
            // enough to stand in for it: a garbage world does not resolve by name, and asking
            // for a root path the search cannot produce is fatal, not empty.
            bRootPathWillFindThisWorld =
                bRootPathWillFindThisWorld || TargetInfo->TryResolveObject() == World;
            if (!MapSwapChainPassesThroughClearedHolder(*Chain, ClearedHolders))
            {
                bEveryChainCleared = false;
                break;
            }
        }
        if (bHasChain && bEveryChainCleared)
        {
            continue;
        }

        FResidentWorldSurvivor& Survivor = Result.Survivors.AddDefaulted_GetRef();
        Survivor.WorldPath = WorldPath;
        Survivor.PackageName = World->GetPackage() ? World->GetPackage()->GetName() : FString();
        Survivor.WorldType = LexToString(World->WorldType);
        Survivor.bGarbage = !IsValid(World);
        if (Result.Survivors.Num() <= MapSwapMaxChainReports)
        {
            // GetRootPath is only callable once a chain for this exact object is known to
            // exist. Its not-found branch builds an FGCObjectInfo from the public constructor
            // (ReferenceChainSearch.cpp:1814), which leaves Class null, then calls
            // GetFullName -> GetClassName -> check(Class) (GCObjectInfo.h:129) and takes the
            // process down - and the case that reaches it is precisely a world held with no
            // UObject referencing it, which is the case this probe exists to report. The
            // engine gates its own call the same way (:1976-1978).
            Survivor.ReferencedBy = bRootPathWillFindThisWorld
                ? Search.GetRootPath(World).Left(MapSwapMaxChainChars)
                : DescribeUnreferencedHolder(World);
        }
    }

    if (Result.IsBlocked())
    {
        UE_LOG(LogPinWrightMapSwapGuard, Warning,
            TEXT("Map swap refused: %d resident world(s) survived the pre-load purge and collect; "
                 "first is '%s' (%s)."),
            Result.Survivors.Num(), *Result.Survivors[0].WorldPath, *Result.Survivors[0].WorldType);
    }
    return Result;
}

FString DescribeSurvivorRefusal(const FWorldSurvivorProbeResult& Result)
{
    if (!Result.IsBlocked())
    {
        return FString();
    }

    FString Listing;
    for (const FResidentWorldSurvivor& Survivor : Result.Survivors)
    {
        Listing += FString::Printf(TEXT("\n  %s [%s%s]"), *Survivor.WorldPath, *Survivor.WorldType,
            Survivor.bGarbage ? TEXT(", already garbage") : TEXT(""));
        if (!Survivor.ReferencedBy.IsEmpty())
        {
            Listing += FString::Printf(TEXT(" held by: %s"), *Survivor.ReferencedBy);
        }
    }

    return FString::Printf(
        TEXT("Refusing the map swap: %d dead world(s) are still resident after a full garbage "
             "collect, so the engine's post-cleanse leak check (CheckForWorldGCLeaks, "
             "EditorServer.cpp) would count them and log 'World Memory Leaks' at Fatal - which "
             "kills the whole editor process and every session attached to it. Still resident:%s"
             "\nThe map swap itself did NOT happen and no world was loaded or torn down, but this "
             "check is not side-effect free: reaching the verdict broadcast "
             "PrepareToCleanseEditorObject for %d dead world(s) (listeners drop references to "
             "them, and asset editors opened on them close), flushed async loading and asset "
             "compilation, and ran one full-purge garbage collection."
             "\nThis is not about unsaved changes - a dirty package is either unloaded or kept by "
             "the swap itself and never reaches this check; something is holding these worlds. "
             "The known holder is the Python plugin's wrapper registry after python.execute "
             "touched objects of a PIE session. Those references were already purged and the "
             "collect re-run before this refusal, so a world still listed here needs the editor "
             "restarted before the next map swap. The severity is the engine cvar "
             "Editor.CheckForWorldGCLeaksAreFatal (default true); setting it false downgrades the "
             "kill to a logged Error, which lets a session finish a swap it otherwise cannot make "
             "but leaks the world - it is an operator escape hatch, not a fix."),
        Result.Survivors.Num(), *Listing, Result.PurgedWorldCount);
}

TSharedPtr<FJsonObject> BuildSurvivorErrorData(
    const FString& LevelPath, const FWorldSurvivorProbeResult& Result)
{
    TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
    ErrorData->SetStringField(TEXT("levelPath"), LevelPath);
    ErrorData->SetNumberField(TEXT("survivingWorldCount"), Result.Survivors.Num());
    ErrorData->SetNumberField(TEXT("purgedWorldCount"), Result.PurgedWorldCount);
    ErrorData->SetBoolField(TEXT("ranCollect"), Result.bRanCollect);
    if (Result.bProbeUnavailable)
    {
        ErrorData->SetBoolField(TEXT("probeUnavailable"), true);
        ErrorData->SetStringField(TEXT("probeUnavailableReason"), Result.UnavailableReason);
    }

    TArray<TSharedPtr<FJsonValue>> Worlds;
    for (const FResidentWorldSurvivor& Survivor : Result.Survivors)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("worldPath"), Survivor.WorldPath);
        Entry->SetStringField(TEXT("packageName"), Survivor.PackageName);
        Entry->SetStringField(TEXT("worldType"), Survivor.WorldType);
        Entry->SetBoolField(TEXT("garbage"), Survivor.bGarbage);
        if (!Survivor.ReferencedBy.IsEmpty())
        {
            Entry->SetStringField(TEXT("referencedBy"), Survivor.ReferencedBy);
        }
        Worlds.Add(MakeShared<FJsonValueObject>(Entry));
    }
    ErrorData->SetArrayField(TEXT("survivingWorlds"), Worlds);

    // blockingPackage keeps its meaning across both refusal shapes on this error code: the
    // one package name a caller acts on first.
    if (Result.Survivors.Num() > 0)
    {
        ErrorData->SetStringField(TEXT("blockingPackage"), Result.Survivors[0].PackageName);
    }
    return ErrorData;
}

}  // namespace PinWrightMapSwapGuard
