// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AssetCreatePolicy.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// File-scope helper names are prefixed because Unity merges translation units:
// a plain `CollectReferencers` would collide with a same-named static elsewhere.

// Inbound referencers of PackageName, self-reference excluded, capped into
// OutCapped. Returns the UNCAPPED count so the caller can report both.
static int32 AssetCreatePolicy_CollectReferencers(const FString& PackageName, TArray<FString>& OutCapped)
{
    OutCapped.Reset();

    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FName> Referencers;
    Registry.GetReferencers(
        FName(*PackageName),
        Referencers,
        UE::AssetRegistry::EDependencyCategory::Package,
        UE::AssetRegistry::FDependencyQuery());

    int32 Count = 0;
    for (const FName& Ref : Referencers)
    {
        const FString RefStr = Ref.ToString();
        if (RefStr == PackageName)
        {
            continue;
        }
        ++Count;
        if (OutCapped.Num() < AssetCreatePolicy::MaxReportedReferencers)
        {
            OutCapped.AddUnique(RefStr);
        }
    }
    return Count;
}

// "Label (ClassName) in /Game/Maps/L_Foo" - enough for a caller to find the actor in the
// World Outliner without a second round trip.
//
// GetActorNameOrLabel, NOT GetActorLabel: the latter defaults to bCreateIfNone=true and
// MUTATES the actor to mint a label (Actor.h:2734). An error path must not write to the
// level it is reporting on. GetLevel()->GetOutermost() names the MAP package even for a
// One-File-Per-Actor actor, whose own package is the external actor file.
static FString AssetCreatePolicy_DescribeActor(const AActor* Actor)
{
    const FString Label = Actor->GetActorNameOrLabel();
    const FString ClassName = Actor->GetClass()->GetName();
    const ULevel* Level = Actor->GetLevel();
    const FString LevelName = Level ? Level->GetOutermost()->GetName() : FString();
    return LevelName.IsEmpty()
        ? FString::Printf(TEXT("%s (%s)"), *Label, *ClassName)
        : FString::Printf(TEXT("%s (%s) in %s"), *Label, *ClassName, *LevelName);
}

// Live in-memory referencers of Occupant that resolve to a level actor, deduplicated and
// capped into OutCapped. Returns the UNCAPPED actor count.
//
// ObjectTools::GatherObjectReferencersForDeletion is the SAME call DeleteSingleObject just
// made to refuse the delete (UE 5.8 ObjectTools.cpp:3498), so the set reported here is the
// set that blocked it rather than a second opinion from a different algorithm.
// bInRequireReferencingProperties stays false: only the referencer objects are needed, and
// passing true re-runs the whole graph walk a second time through IsReferenced() to fill in
// FProperty lists this payload does not report.
//
// Called ONLY on the refusal branch. It is a full object-graph walk (FReferencerFinder),
// which is why it must never run on the path where the delete succeeded.
static int32 AssetCreatePolicy_CollectReferencingActors(UObject* Occupant, TArray<FString>& OutCapped)
{
    OutCapped.Reset();
    if (!Occupant)
    {
        return 0;
    }

    FReferencerInformationList Refs;
    bool bIsReferenced = false;
    bool bIsReferencedByUndo = false;
    ObjectTools::GatherObjectReferencersForDeletion(Occupant, bIsReferenced, bIsReferencedByUndo,
        &Refs, /*bInRequireReferencingProperties=*/false);

    TSet<const AActor*> Seen;
    int32 Count = 0;
    for (const FReferencerInformation& Info : Refs.ExternalReferences)
    {
        if (!IsValid(Info.Referencer))
        {
            continue;
        }
        // A UStaticMeshComponent referencer is reported as its owning actor. GetTypedOuter
        // starts at GetOuter() and so never returns the object itself (UObjectBaseUtility.cpp:314),
        // hence the Cast for a referencer that already IS the actor.
        const AActor* Actor = Info.Referencer->GetTypedOuter<AActor>();
        if (!Actor)
        {
            Actor = Cast<AActor>(Info.Referencer);
        }
        if (!Actor)
        {
            continue;
        }

        bool bAlreadySeen = false;
        Seen.Add(Actor, &bAlreadySeen);
        if (bAlreadySeen)
        {
            continue;
        }

        ++Count;
        if (OutCapped.Num() < AssetCreatePolicy::MaxReportedReferencers)
        {
            OutCapped.Add(AssetCreatePolicy_DescribeActor(Actor));
        }
    }
    return Count;
}

AssetCreatePolicy::FResolution AssetCreatePolicy::Resolve(
    const FString& PackageName, const FString& AssetName,
    UClass* ExpectedClass, bool bOverwriteRequested, bool bRequireExactClass)
{
    FResolution Resolution;

    if (PackageName.IsEmpty() || AssetName.IsEmpty())
    {
        return Resolution;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

    // StaticFindObject first. An asset authored earlier in THIS session can be live in
    // memory while the asset registry has not caught up, and that is precisely the
    // object CanCreateAsset finds and prompts about - the registry-only
    // UEditorAssetLibrary::DoesAssetExist pre-check the create verbs used before does
    // not see it.
    UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);

    // Then disk: an on-disk-but-unloaded asset must be loaded and considered, not
    // silently clobbered by a fresh CreatePackage / CreateAsset.
    if (!Existing && FPackageName::DoesPackageExist(PackageName))
    {
        Existing = LoadObject<UObject>(nullptr, *ObjectPath);
    }

    if (!Existing)
    {
        return Resolution;
    }

    Resolution.bExistingFound = true;
    Resolution.Existing = Existing;
    Resolution.ExistingClass = Existing->GetClass()->GetPathName();
    Resolution.ExistingPath = Existing->GetPathName();

    const bool bClassMatches = ExpectedClass != nullptr
        && (bRequireExactClass ? (Existing->GetClass() == ExpectedClass)
                               : Existing->IsA(ExpectedClass));

    if (!bClassMatches)
    {
        // A different asset type is never replaced, whatever `overwrite` says: the
        // caller asked for one kind of asset and something else owns the name.
        Resolution.Action = EAction::Rejected;
        Resolution.Existing = nullptr;
        Resolution.ErrorCode = ErrorCodes::ERR_ASSET_ALREADY_EXISTS;
        Resolution.ErrorMessage = FString::Printf(
            TEXT("'%s' already exists and is a %s, not a %s. Rename or delete it first, or create at a different path."),
            *Resolution.ExistingPath,
            *Existing->GetClass()->GetName(),
            ExpectedClass ? *ExpectedClass->GetName() : TEXT("<unspecified class>"));
        return Resolution;
    }

    if (!bOverwriteRequested)
    {
        Resolution.Action = EAction::UpdateInPlace;
        return Resolution;
    }

    // overwrite:true. Pre-flight the referencers ourselves rather than letting
    // ObjectTools discover them and prompt: the reference-check prompt and the
    // "referenced by other content" notice are two of the three modals this policy
    // exists to prevent.
    Resolution.ReferencerCount = AssetCreatePolicy_CollectReferencers(PackageName, Resolution.Referencers);
    if (Resolution.ReferencerCount > 0)
    {
        Resolution.Action = EAction::Rejected;
        Resolution.Existing = nullptr;
        Resolution.ErrorCode = ErrorCodes::ERR_ASSET_IN_USE;
        Resolution.ErrorMessage = FString::Printf(
            TEXT("'%s' cannot be overwritten: %d package(s) still reference it. ")
            TEXT("Drop overwrite (omit it, or pass overwrite=false) to update the existing asset in place - ")
            TEXT("that keeps the referencers pointing at it and is the normal re-run. ")
            TEXT("Only repoint or delete the referencers first if you genuinely need a fresh asset."),
            *Resolution.ExistingPath, Resolution.ReferencerCount);
        return Resolution;
    }

    // Nothing references it on disk. Delete it so the create path below starts from a
    // clear package. The scope covers the in-memory reference check ObjectTools still
    // performs, whose refusal prompt would otherwise block the game thread.
    bool bDeleted = false;
    {
        FScopedUnattendedRpc UnattendedScope;
        bDeleted = ObjectTools::DeleteSingleObject(Existing);
    }

    if (!bDeleted)
    {
        // ObjectTools refused (or its suppressed prompt defaulted to "no"). Report the
        // refusal instead of walking into CanCreateAsset with the object still there.
        //
        // The dominant cause is a LEVEL ACTOR spawned from this asset - the iterate-render-
        // iterate loop - which the registry pre-check above cannot see, because an unsaved
        // level reference is not on disk to be indexed. The message used to say "Close any
        // editor holding it", which sent callers hunting for an asset editor that is not
        // open; DeleteSingleObject has in fact ALREADY closed every asset editor for this
        // object by this point (UE 5.8 ObjectTools.cpp:3470), so that can never be the
        // surviving cause. Name the actors instead, and lead with the remedy that works.
        Resolution.ReferencingActorCount =
            AssetCreatePolicy_CollectReferencingActors(Existing, Resolution.ReferencingActors);

        FString Holders;
        if (Resolution.ReferencingActorCount > 0)
        {
            FString Listed = FString::Join(Resolution.ReferencingActors, TEXT(", "));
            const int32 Hidden = Resolution.ReferencingActorCount - Resolution.ReferencingActors.Num();
            if (Hidden > 0)
            {
                Listed += FString::Printf(TEXT(" (+%d more)"), Hidden);
            }
            Holders = FString::Printf(TEXT("Held by %d level actor(s): %s. ")
                TEXT("Deleting it instead would mean removing those references first."),
                Resolution.ReferencingActorCount, *Listed);
        }
        else
        {
            Holders = TEXT("No referencing level actor was found, so an open asset editor, ")
                      TEXT("a Blueprint default, or the undo buffer may be holding it.");
        }

        Resolution.Action = EAction::Rejected;
        Resolution.Existing = nullptr;
        Resolution.ErrorCode = ErrorCodes::ERR_ASSET_IN_USE;
        Resolution.ErrorMessage = FString::Printf(
            TEXT("'%s' exists and could not be deleted for overwrite: it is still referenced in memory. ")
            TEXT("Drop overwrite (omit it, or pass overwrite=false) to update the existing asset in place - ")
            TEXT("recompiling your own source onto its own output is the normal iteration loop, needs no ")
            TEXT("delete, and leaves every reference intact. %s"),
            *Resolution.ExistingPath, *Holders);
        return Resolution;
    }

    // Deleted. The pointer is garbage from here on, so it is not handed back.
    Resolution.Existing = nullptr;
    Resolution.Action = EAction::Create;
    return Resolution;
}

void AssetCreatePolicy::AddCreateReport(const TSharedPtr<FJsonObject>& Out, const FResolution& Resolution)
{
    if (!Out.IsValid())
    {
        return;
    }

    const bool bUpdated = (Resolution.Action == EAction::UpdateInPlace);
    Out->SetBoolField(TEXT("existing"), Resolution.bExistingFound);
    Out->SetStringField(TEXT("mode"), bUpdated ? TEXT("updated_in_place") : TEXT("created"));
}

TSharedPtr<FJsonObject> AssetCreatePolicy::MakeErrorData(const FResolution& Resolution)
{
    if (Resolution.Action != EAction::Rejected)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("existingPath"), Resolution.ExistingPath);
    Data->SetStringField(TEXT("existingClass"), Resolution.ExistingClass);

    if (Resolution.ErrorCode == ErrorCodes::ERR_ASSET_IN_USE)
    {
        TArray<TSharedPtr<FJsonValue>> Refs;
        Refs.Reserve(Resolution.Referencers.Num());
        for (const FString& Ref : Resolution.Referencers)
        {
            Refs.Add(MakeShared<FJsonValueString>(Ref));
        }
        Data->SetArrayField(TEXT("referencers"), Refs);
        Data->SetNumberField(TEXT("referencerCount"), static_cast<double>(Resolution.ReferencerCount));

        // Distinctly named because the two lists answer different questions:
        // referencers[] are on-disk PACKAGES the asset registry indexed, referencingActors[]
        // are LIVE level actors found in memory. Both pairs are always emitted so the
        // payload shape is the same on either refusal branch; the inapplicable pair is [] / 0.
        TArray<TSharedPtr<FJsonValue>> ActorRefs;
        ActorRefs.Reserve(Resolution.ReferencingActors.Num());
        for (const FString& Actor : Resolution.ReferencingActors)
        {
            ActorRefs.Add(MakeShared<FJsonValueString>(Actor));
        }
        Data->SetArrayField(TEXT("referencingActors"), ActorRefs);
        Data->SetNumberField(TEXT("referencingActorCount"),
            static_cast<double>(Resolution.ReferencingActorCount));
    }

    return Data;
}

bool AssetCreatePolicy::SendRejection(FHandlerContext& Ctx, const FResolution& Resolution)
{
    Ctx.SendError(Resolution.ErrorCode, Resolution.ErrorMessage, MakeErrorData(Resolution));
    return true;
}
