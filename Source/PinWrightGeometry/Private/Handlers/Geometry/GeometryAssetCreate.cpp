// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryAssetCreate.cpp - see GeometryAssetCreate.h for why this replaces
// CreateNewStaticMeshAssetFromMesh rather than wrapping it.
#include "Handlers/Geometry/GeometryAssetCreate.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "PwSource/PwSourceRecompileGuard.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Materials/MaterialInterface.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "UDynamicMesh.h"

// UE::AssetUtils::CreateStaticMeshAsset (ModelingComponentsEditorOnly) and
// UE::AssetUtils::GenerateNewMaterialSlotName (ModelingComponents).
#include "AssetUtils/CreateStaticMeshUtil.h"
#include "AssetUtils/StaticMeshMaterialUtil.h"

namespace
{
    // Prefixed because Unity merges this TU with the sibling geometry handler files.
    //
    // Only for the argument-validation returns that run BEFORE any result state exists. Once
    // the caller-visible result has started accumulating, use AssetCreateFailInPlace.
    FStaticMeshCreateResult AssetCreateFail(const TCHAR* Code, FString Message)
    {
        FStaticMeshCreateResult Result;
        Result.ErrorCode = Code;
        Result.ErrorMessage = MoveTemp(Message);
        return Result;
    }

    // Fail WITHOUT discarding what has already accumulated into Out.
    //
    // The warnings are a promise the error path has to keep too: the "no usable UV channel 0"
    // note, the unloadable-material notes and UnboundSlots all land in Out before the last
    // failure return. Constructing a fresh FStaticMeshCreateResult at that point - which is what
    // the helper above does, correctly, for the argument checks that precede Out - silently
    // dropped every one of them, leaving the caller a bare "failed to create" for a failure the
    // warnings would have explained.
    FStaticMeshCreateResult& AssetCreateFailInPlace(FStaticMeshCreateResult& Out,
        const TCHAR* Code, FString Message)
    {
        Out.bSuccess = false;
        Out.Asset = nullptr;
        Out.ErrorCode = Code;
        Out.ErrorMessage = MoveTemp(Message);
        return Out;
    }

    // Reads the provenance stamp off an asset already at the target path. Null when the asset is
    // not a StaticMesh, or is one this file never generated.
    const UPwModelAssetUserData* ReadProvenanceStamp(UObject* ExistingObject)
    {
        UStaticMesh* ExistingMesh = Cast<UStaticMesh>(ExistingObject);
        if (!ExistingMesh)
        {
            return nullptr;
        }
        return Cast<UPwModelAssetUserData>(
            ExistingMesh->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()));
    }

    FPwSourceStateEntry StaticMeshState(
        FString Key, FString Field, FString Value, FString Origin, bool bAssetPath = false)
    {
        FPwSourceStateEntry Entry;
        Entry.Key = MoveTemp(Key);
        Entry.Field = MoveTemp(Field);
        Entry.Value = MoveTemp(Value);
        Entry.Origin = MoveTemp(Origin);
        Entry.bAssetPath = bAssetPath;
        return Entry;
    }

    TArray<FPwSourceStateEntry> DesiredStaticMeshState(const FStaticMeshCreateSpec& Spec)
    {
        TArray<FPwSourceStateEntry> State;
        for (int32 Index = 0; Index < Spec.MaterialSlots.Num(); ++Index)
        {
            const FString& Slot = Spec.MaterialSlots[Index];
            const FString* Binding = Spec.MaterialBindings.Find(Slot);
            State.Add(StaticMeshState(FString::Printf(TEXT("material:%d"), Index),
                FString::Printf(TEXT("material[%s]"), *Slot), Binding ? *Binding : FString(),
                TEXT("materials can be written by static-mesh authoring verbs or the Static Mesh editor"),
                /*bAssetPath=*/true));
        }
        if (Spec.LightMapChannel != INDEX_NONE)
        {
            State.Add(StaticMeshState(TEXT("lightmap_channel"), TEXT("lightmapChannel"),
                FString::FromInt(Spec.LightMapChannel),
                TEXT("lightmap settings can be changed in the Static Mesh editor")));
        }
        if (Spec.LightMapResolution != INDEX_NONE)
        {
            State.Add(StaticMeshState(TEXT("lightmap_resolution"), TEXT("lightmapResolution"),
                FString::FromInt(Spec.LightMapResolution),
                TEXT("lightmap settings can be changed in the Static Mesh editor")));
        }
        State.Add(StaticMeshState(TEXT("collision_trace"), TEXT("collisionTrace"),
            FString::FromInt(static_cast<int32>(Spec.CollisionTrace)),
            TEXT("collision can be written by collision authoring verbs or the Static Mesh editor")));
        State.Add(StaticMeshState(TEXT("collision_elements"), TEXT("collisionElements"),
            FString::FromInt(Spec.SimpleCollision.IsSet()
                ? Spec.SimpleCollision->GetElementCount() : 0),
            TEXT("collision can be written by collision authoring verbs or the Static Mesh editor")));
        return State;
    }

    TArray<FPwSourceStateEntry> CurrentStaticMeshState(const UStaticMesh* Mesh)
    {
        TArray<FPwSourceStateEntry> State;
        if (!Mesh)
        {
            return State;
        }
        const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
        for (int32 Index = 0; Index < Materials.Num(); ++Index)
        {
            const FStaticMaterial& Material = Materials[Index];
            const FString Slot = Material.MaterialSlotName.ToString();
            State.Add(StaticMeshState(FString::Printf(TEXT("material:%d"), Index),
                FString::Printf(TEXT("material[%s]"), *Slot),
                Material.MaterialInterface ? Material.MaterialInterface->GetPathName() : FString(),
                TEXT("materials can be written by static-mesh authoring verbs or the Static Mesh editor"),
                /*bAssetPath=*/true));
        }
        State.Add(StaticMeshState(TEXT("lightmap_channel"), TEXT("lightmapChannel"),
            FString::FromInt(Mesh->GetLightMapCoordinateIndex()),
            TEXT("lightmap settings can be changed in the Static Mesh editor")));
        State.Add(StaticMeshState(TEXT("lightmap_resolution"), TEXT("lightmapResolution"),
            FString::FromInt(Mesh->GetLightMapResolution()),
            TEXT("lightmap settings can be changed in the Static Mesh editor")));
        State.Add(StaticMeshState(TEXT("lod_count"), TEXT("lodCount"),
            FString::FromInt(Mesh->GetNumSourceModels()),
            TEXT("LODs can be added by static-mesh authoring verbs or the Static Mesh editor")));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        const bool bNaniteEnabled = Mesh->GetNaniteSettings().bEnabled;
#else
        const bool bNaniteEnabled = Mesh->NaniteSettings.bEnabled;
#endif
        State.Add(StaticMeshState(TEXT("nanite"), TEXT("naniteEnabled"),
            bNaniteEnabled ? TEXT("true") : TEXT("false"),
            TEXT("Nanite settings can be changed in the Static Mesh editor")));
        for (const UStaticMeshSocket* Socket : Mesh->Sockets)
        {
            if (Socket)
            {
                State.Add(StaticMeshState(TEXT("socket:") + Socket->SocketName.ToString(),
                    FString::Printf(TEXT("socket[%s]"), *Socket->SocketName.ToString()),
                    Socket->RelativeLocation.ToCompactString(),
                    TEXT("sockets can be written by static-mesh socket verbs or the Static Mesh editor")));
            }
        }
        if (const UBodySetup* Body = Mesh->GetBodySetup())
        {
            State.Add(StaticMeshState(TEXT("collision_trace"), TEXT("collisionTrace"),
                FString::FromInt(static_cast<int32>(Body->CollisionTraceFlag)),
                TEXT("collision can be written by collision authoring verbs or the Static Mesh editor")));
            State.Add(StaticMeshState(TEXT("collision_elements"), TEXT("collisionElements"),
                FString::FromInt(Body->AggGeom.GetElementCount()),
                TEXT("collision can be written by collision authoring verbs or the Static Mesh editor")));
        }
        return State;
    }
}

FStaticMeshCreateResult CreateStaticMesh(UDynamicMesh* Mesh, const FStaticMeshCreateSpec& Spec)
{
    if (!Mesh)
    {
        return AssetCreateFail(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("No source mesh supplied"));
    }
    if (Spec.AssetPath.IsEmpty())
    {
        return AssetCreateFail(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath required"));
    }

    FText BadPackageNameReason;
    if (!FPackageName::IsValidTextForLongPackageName(Spec.AssetPath, &BadPackageNameReason))
    {
        return AssetCreateFail(ErrorCodes::ERR_INVALID_ASSET_PATH,
            FString::Printf(TEXT("Invalid asset path '%s': %s"),
                *Spec.AssetPath, *BadPackageNameReason.ToString()));
    }
    // The engine's own content is not a place a generated model may land. The overwrite branch of
    // geometry.convert_to_static_mesh refuses this for the same reason; this is the create side.
    if (Spec.AssetPath.StartsWith(TEXT("/Engine/")) && !Spec.AssetPath.StartsWith(TEXT("/Engine/Transient")))
    {
        return AssetCreateFail(ErrorCodes::ERR_SECURITY_VIOLATION,
            TEXT("Refusing to write a generated StaticMesh into /Engine/ - target a /Game/ path"));
    }

    if (Mesh->GetTriangleCount() == 0)
    {
        return AssetCreateFail(ErrorCodes::ERR_ASSET_CREATION_FAILED,
            TEXT("Failed to create StaticMesh asset - the source mesh has no triangles"));
    }

    FStaticMeshCreateResult Out;

    // ---- Resolve the target path FIRST, before anything is mutated or written.
    //
    // Resolve is also the single occupant lookup: it runs the StaticFindObject-then-load probe
    // itself and hands the occupant back as FResolution::Existing, so the provenance check below
    // reads it from there instead of repeating the same two calls forty lines earlier.
    //
    // Resolve is what performs a replacement WITHOUT a dialog. IAssetTools::CreateAsset would
    // chain three modals here (overwrite prompt -> delete reference check -> "asset is referenced"
    // prompt), each owning the game thread in a nested Slate loop, which wedges every in-flight
    // RPC from every client. It also refuses a class mismatch outright - whatever `overwrite`
    // says - and refuses to overwrite an asset that still has referencers.
    const FString PackageName = Spec.AssetPath;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);

    // ---- PERMISSION and MECHANISM are separate axes, and this call wants exactly one mechanism.
    //
    // bOverwriteRequested is hardcoded FALSE. Spec.bOverwrite is NOT dropped - it is read as
    // PERMISSION forty lines below ("replace content at this path that I did not generate") and
    // never as a choice of mechanism, because the only mechanism worth having here is the
    // in-place rebuild: it preserves every referencer, which the delete-then-recreate branch
    // orphans.
    //
    // Feeding the flag in here conflated the two and turned the two gates into a DEADLOCK on the
    // first compile onto an existing, referenced, unstamped asset:
    //
    //   overwrite=true  -> Resolve took delete-then-recreate and refused ASSET_IN_USE the moment
    //                      the registry saw one referencer, advising "drop overwrite".
    //   overwrite omitted -> the provenance gate below refused the unstamped occupant, advising
    //                      "pass overwrite=true".
    //
    // Each rejection named the other as its remedy, so no argument existed that worked - and that
    // state is not an edge case, it is every migration of existing placed content onto .pwmodel,
    // which is what the format exists for. Both messages were individually accurate; neither
    // could observe the other firing. See docs/lessons.md, "Two guards that each name the other
    // as the remedy".
    //
    // A class mismatch still rejects ASSET_ALREADY_EXISTS, which stays correct: a non-StaticMesh
    // occupant cannot be rebuilt in place. ASSET_IN_USE is now unreachable from this surface,
    // which is the point - a referenced asset is the normal case, not an error.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        PackageName, AssetName, UStaticMesh::StaticClass(), /*bOverwriteRequested=*/false);
    if (Resolution.IsRejected())
    {
        return AssetCreateFailInPlace(Out, *Resolution.ErrorCode, Resolution.ErrorMessage);
    }

    const bool bUpdateInPlace = Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace;
    const UPwModelAssetUserData* ExistingStamp = bUpdateInPlace
        ? ReadProvenanceStamp(Resolution.Existing) : nullptr;
    const bool bSameSource = ExistingStamp && !Spec.SourcePath.IsEmpty() &&
        ExistingStamp->SourcePath == Spec.SourcePath;
    UStaticMesh* ExistingMesh = bUpdateInPlace ? Cast<UStaticMesh>(Resolution.Existing) : nullptr;
    if (bUpdateInPlace && !ExistingMesh)
    {
        return AssetCreateFailInPlace(Out, ErrorCodes::ERR_ASSET_CREATION_FAILED,
            FString::Printf(TEXT("The existing asset at %s could not be loaded as a StaticMesh"),
                *PackageName));
    }

    bool bGuardAllowsWrite = true;
    if (bUpdateInPlace)
    {
        FPwSourceRecompileGuardRequest Guard;
        Guard.FormatName = TEXT(".pwmodel");
        Guard.AssetPath = PackageName;
        Guard.bSameSourceRecompile = bSameSource;
        Guard.bTakeover = !bSameSource;
        Guard.bOverwrite = Spec.bOverwrite;
        Guard.CurrentSourcePath = ExistingStamp ? ExistingStamp->SourcePath : FString();
        Guard.RequestedSourcePath = Spec.SourcePath;
        Guard.BaselineVersion = ExistingStamp ? ExistingStamp->GeneratedStateVersion : 0;
        Guard.Baseline = ExistingStamp ? &ExistingStamp->GeneratedState : nullptr;
        Guard.Current = CurrentStaticMeshState(ExistingMesh);
        Guard.Desired = DesiredStaticMeshState(Spec);
        bGuardAllowsWrite = PwSourceRecompileGuard::Check(Guard, Out.Diagnostics);
    }

    // ---- The provenance rule, on the only branch that can reach it: an occupant now always
    // survives Resolve, so this is the SINGLE gate that reads Spec.bOverwrite, and it reads it
    // as permission. Granting permission does not change what happens next - the rebuild below
    // is in place either way, so the occupant's referencers survive an overwrite too.
    if (bUpdateInPlace)
    {
        if (!bSameSource && !Spec.bOverwrite)
        {
            FString Why;
            if (!ExistingStamp)
            {
                Why = FString::Printf(
                    TEXT("A %s already exists at %s and carries no PinWright Model provenance stamp"),
                    *Resolution.Existing->GetClass()->GetName(), *PackageName);
            }
            else
            {
                Why = FString::Printf(
                    TEXT("The asset at %s was generated from '%s', not '%s'"),
                    *PackageName, *ExistingStamp->SourcePath, *Spec.SourcePath);
            }
            // The remedy this names has to be one that WORKS on a referenced asset, because a
            // referenced asset is the normal occupant. It does now: overwrite=true rebuilds the
            // occupant in place rather than deleting it, so nothing pointing at it is orphaned.
            if (!bGuardAllowsWrite && Out.Diagnostics.Num() > 0)
            {
                Why += TEXT(". ") + Out.Diagnostics.Last().Message;
            }
            else
            {
                Why += TEXT(" - pass overwrite=true to rebuild it in place (its referencers are preserved)");
            }
            return AssetCreateFailInPlace(
                Out, ErrorCodes::ERR_ASSET_ALREADY_EXISTS, MoveTemp(Why));
        }
    }

    if (!bGuardAllowsWrite)
    {
        return AssetCreateFailInPlace(Out, ErrorCodes::ERR_ASSET_DATA_INVALID,
            Out.Diagnostics.Last().Message);
    }

    // ---- UV guard, BEFORE creation: a size-0 UV array crashes the async build worker inside
    // MikkT. GeometryUtils::EnsureMeshHasUVs carries the full history and is grow-only, so it
    // cannot truncate an authored lightmap channel out from under the lightmap index written
    // below - which is the only reason this file used to keep a near-verbatim private copy.
    //
    // Deliberately below every refusal above: it box-projects UVs onto the CALLER's mesh, and a
    // call that writes no asset must not leave that behind either.
    const bool bHasUVs = GeometryUtils::EnsureMeshHasUVs(Mesh);
    if (!bHasUVs)
    {
        // Belt and braces: with no usable UVs both recomputes stay off so MikkT is never invoked.
        Out.Warnings.Add(TEXT("Mesh carries no usable UV channel 0; normal/tangent recompute disabled for the bake"));
    }
    const bool bRecomputeNormals = Spec.bRecomputeNormals && bHasUVs;
    const bool bRecomputeTangents = Spec.bRecomputeTangents && bHasUVs;

    // ---- Resolve the material bindings. Unbound slots keep the default surface material and are
    // reported rather than silently substituted.
    const int32 NumMaterialSlots = FMath::Max(1, Spec.MaterialSlots.Num());
    TArray<UMaterialInterface*> SlotMaterials;
    SlotMaterials.Init(nullptr, NumMaterialSlots);
    for (int32 SlotIndex = 0; SlotIndex < Spec.MaterialSlots.Num(); ++SlotIndex)
    {
        const FString& SlotName = Spec.MaterialSlots[SlotIndex];
        const FString* BoundPath = Spec.MaterialBindings.Find(SlotName);
        if (!BoundPath || BoundPath->IsEmpty())
        {
            Out.UnboundSlots.Add(SlotName);
            continue;
        }

        UMaterialInterface* Bound = LoadObject<UMaterialInterface>(nullptr, **BoundPath);
        if (!Bound)
        {
            Out.UnboundSlots.Add(SlotName);
            Out.Warnings.Add(FString::Printf(
                TEXT("Material slot '%s' is bound to '%s', which could not be loaded; using the default material"),
                *SlotName, **BoundPath));
            continue;
        }
        SlotMaterials[SlotIndex] = Bound;
    }

    // UDynamicMesh may become asynchronously editable, so the engine's own callers copy rather
    // than hand the live mesh across the create call. Do the same; the copy outlives the call.
    UE::Geometry::FDynamicMesh3 MeshCopy;
    Mesh->ProcessMesh([&MeshCopy](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        MeshCopy = ReadMesh;
    });

    UE::AssetUtils::FStaticMeshAssetOptions Options;
    // Update-in-place: hand the util the package the occupant already lives in. It builds with
    // NewObject<UStaticMesh>(Package, <the same name>, ...), and StaticAllocateObject replaces a
    // same-class occupant "without affecting the original's address or index"
    // (UObjectGlobals.cpp:3568), destroying and reconstructing it through the normal
    // ConditionalBeginDestroy path so its render resources are released - yet every actor and
    // asset still pointing at that UStaticMesh keeps resolving to the rebuilt one. Deleting the
    // asset and creating a fresh object orphans them, and is refused outright the moment the
    // registry sees a single referencer.
    //
    // This is now the ONLY mechanism this file uses for an occupied path - overwrite=true takes
    // it too. It is strictly safer and nothing about compiling a .pwmodel wants the orphaning
    // branch, so the flag chooses permission, not mechanism (see Resolve above).
    Options.UsePackage =
        (bUpdateInPlace && Resolution.Existing) ? Resolution.Existing->GetPackage() : nullptr;
    Options.NewAssetPath = PackageName;
    Options.NumSourceModels = 1;
    Options.NumMaterialSlots = NumMaterialSlots;
    Options.AssetMaterials = SlotMaterials;
    Options.bEnableRecomputeNormals = bRecomputeNormals;
    Options.bEnableRecomputeTangents = bRecomputeTangents;
    Options.bGenerateNaniteEnabledMesh = false;
    // Creates the UBodySetup and stamps CollisionType onto it (CreateStaticMeshUtil.cpp:89-94),
    // so only the AggGeom below is left for this file to write.
    Options.bCreatePhysicsBody = true;
    Options.CollisionType = Spec.CollisionTrace;
    // Left false deliberately even when a lightmap channel is requested: generating lightmap UVs
    // would repack the destination channel, discarding an authored one, and it also inflates the
    // NumUVs that EnforceLightmapRestrictions clamps against (StaticMesh.cpp:9785-9788) - which
    // would turn "you asked for a channel this mesh does not have" into a silent success.
    Options.bGenerateLightmapUVs = false;
    Options.SourceMeshes.DynamicMeshes.Add(&MeshCopy);
    // The whole point: the returned asset has never been built, so everything below is written
    // BEFORE the first build instead of patched onto a built asset.
    Options.bDeferPostEditChange = true;

    UE::AssetUtils::FStaticMeshResults CreateResults;
    const UE::AssetUtils::ECreateStaticMeshResult CreateOutcome =
        UE::AssetUtils::CreateStaticMeshAsset(Options, CreateResults);
    if (CreateOutcome != UE::AssetUtils::ECreateStaticMeshResult::Ok || !CreateResults.StaticMesh)
    {
        return AssetCreateFailInPlace(Out, ErrorCodes::ERR_ASSET_CREATION_FAILED,
            FString::Printf(TEXT("Failed to create StaticMesh asset at %s"), *PackageName));
    }

    UStaticMesh* NewMesh = CreateResults.StaticMesh;

    // ---- Deferred gap: everything the caller asked for, written pre-build.

    // Material slot names. CreateStaticMeshAsset derives them from the material and offers no way
    // to supply them, so the document's slot names are written back over the generated ones. The
    // section info map it built holds material INDICES, so renaming preserves it.
    if (Spec.MaterialSlots.Num() > 0)
    {
        TArray<FStaticMaterial> StaticMaterials = NewMesh->GetStaticMaterials();
        const int32 NamedCount = FMath::Min(Spec.MaterialSlots.Num(), StaticMaterials.Num());
        for (int32 SlotIndex = 0; SlotIndex < NamedCount; ++SlotIndex)
        {
            FStaticMaterial& Slot = StaticMaterials[SlotIndex];
            const FString& RequestedName = Spec.MaterialSlots[SlotIndex];
            // An empty FName slot breaks the Static Mesh editor, so an unnamed slot falls back to
            // the engine's own generator, run against the already-renamed prefix so it cannot
            // collide with a name the document chose.
            const FName SlotName = RequestedName.IsEmpty()
                ? UE::AssetUtils::GenerateNewMaterialSlotName(StaticMaterials, Slot.MaterialInterface, SlotIndex)
                : FName(*RequestedName);
            Slot.MaterialSlotName = SlotName;
            Slot.ImportedMaterialSlotName = SlotName;
        }
        NewMesh->SetStaticMaterials(StaticMaterials);
    }

    // Lightmap. The channel and the resolution are independent requests carrying independent
    // INDEX_NONE sentinels: a spec that set only the resolution used to have it dropped without a
    // word, because the write sat inside the channel branch.
    //
    // Both halves of each are needed: the build setting drives the build, the asset property is
    // what the renderer reads, and CreateStaticMeshAsset writes neither unless it is generating
    // lightmap UVs (which it is not, see above).
    if (Spec.LightMapResolution != INDEX_NONE)
    {
        NewMesh->GetSourceModel(0).BuildSettings.MinLightmapResolution = Spec.LightMapResolution;
        NewMesh->SetLightMapResolution(Spec.LightMapResolution);
    }
    if (Spec.LightMapChannel != INDEX_NONE)
    {
        NewMesh->GetSourceModel(0).BuildSettings.DstLightmapIndex = Spec.LightMapChannel;
        NewMesh->SetLightMapCoordinateIndex(Spec.LightMapChannel);
    }

    // Simple collision, straight into the pre-build UBodySetup the create options already made
    // and already stamped with CollisionTrace. No InvalidatePhysicsData and no component
    // physics-state dance: nothing has been cooked yet, which is the entire reason the collision
    // is an input here rather than a patch applied afterwards.
    if (Spec.SimpleCollision.IsSet())
    {
        if (UBodySetup* BodySetup = NewMesh->GetBodySetup())
        {
            BodySetup->AggGeom = Spec.SimpleCollision.GetValue();
            Out.CollisionElements = BodySetup->AggGeom.GetElementCount();
        }
    }

    // ---- Exactly one build.
    NewMesh->PostEditChange();

    // Everything below reads the BUILT asset, so the async build has to have landed first.
    // Hoisted out of the lightmap branch below, which used to be the only read-back and
    // therefore the only place that waited.
    if (NewMesh->IsCompiling())
    {
        FStaticMeshCompilingManager::Get().FinishCompilation({NewMesh});
    }

    // Provenance and the last generated semantic state are written only after the build has
    // settled, because several requested values can be clamped during PostEditChange.
    if (!Spec.SourcePath.IsEmpty())
    {
        UPwModelAssetUserData* Stamp = NewObject<UPwModelAssetUserData>(NewMesh);
        Stamp->SourcePath = Spec.SourcePath;
        Stamp->SourceHash = Spec.SourceHash;
        Stamp->GeneratedStateVersion = PwSourceRecompileGuard::StateVersion;
        Stamp->GeneratedState = PwSourceRecompileGuard::MakeStateMap(
            CurrentStaticMeshState(NewMesh));
        NewMesh->AddAssetUserData(Stamp);
    }

    // The lightmap index only survives if the mesh really has that channel:
    // EnforceLightmapRestrictions clamps it to [0, NumUVs - 1] during the build
    // (StaticMesh.cpp:9812). Report the clamp instead of letting the caller believe the request
    // landed.
    if (Spec.LightMapChannel != INDEX_NONE)
    {
        const int32 ActualLightMapChannel = NewMesh->GetLightMapCoordinateIndex();
        if (ActualLightMapChannel != Spec.LightMapChannel)
        {
            Out.Warnings.Add(FString::Printf(
                TEXT("Requested lightmap channel %d, but the mesh does not carry that UV channel; the asset uses channel %d"),
                Spec.LightMapChannel, ActualLightMapChannel));
        }
    }

    // The same read-back for the resolution, which the build can also move: the first thing
    // EnforceLightmapRestrictions does is SetLightMapResolution(FMath::Max(current, 4))
    // (StaticMesh.cpp:9732), so a request below 4 lands as 4 rather than as what was asked for.
    if (Spec.LightMapResolution != INDEX_NONE)
    {
        const int32 ActualLightMapResolution = NewMesh->GetLightMapResolution();
        if (ActualLightMapResolution != Spec.LightMapResolution)
        {
            Out.Warnings.Add(FString::Printf(
                TEXT("Requested lightmap resolution %d, but the build stored %d"),
                Spec.LightMapResolution, ActualLightMapResolution));
        }
    }
    // A lightmap channel with no resolution is the silent-waste case this warning exists for:
    // the authored lightmap UVs are real and the channel is honoured, but the asset keeps
    // UStaticMesh's constructor default of 4 (StaticMesh.cpp:4738) and bakes them at 4x4 - which
    // reads as a clean success everywhere. Not emitted when no channel was requested at all, so
    // a caller who never mentioned a lightmap is not nagged about one.
    else if (Spec.LightMapChannel != INDEX_NONE)
    {
        Out.Warnings.Add(FString::Printf(
            TEXT("Lightmap channel %d was requested with no lightmap resolution, so the asset keeps the UStaticMesh default of 4 and the authored lightmap UVs bake at 4x4. Set one with `lightmap channel=%d resolution=<4..4096>`"),
            Spec.LightMapChannel, Spec.LightMapChannel));
    }

    // CreateStaticMeshAsset does not publish the asset, so the content browser and every registry
    // query would miss it until a restart. Re-announcing an updated-in-place asset refreshes its
    // FAssetData rather than duplicating it.
    FAssetRegistryModule::AssetCreated(NewMesh);

    // Persist for real. SaveAssetToDiskReportingPresence captures timestamp, size and the dirty
    // flag BEFORE the save, so a throttle-skipped write is distinguishable from a real one - a
    // bare existence probe is satisfied by the .uasset a PREVIOUS save wrote. Deliberately not
    // the shared mark-dirty wrapper, which returns void on purpose and makes nothing durable.
    if (Spec.bSave)
    {
        // OutState, not just the bool: an intermittent saved:false on this exact path is what
        // this reporting exists for, and the bool cannot say whether a flush is the remedy.
        Out.bSavedToDisk =
            SaveAssetToDiskReportingPresence(NewMesh, /*bForce=*/true, &Out.PackageName,
                                             &Out.SizeBytes, &Out.SaveState);
    }
    else
    {
        NewMesh->MarkPackageDirty();
        Out.PackageName = NewMesh->GetOutermost() ? NewMesh->GetOutermost()->GetName() : FString();
        Out.SaveState = EAssetSaveState::NotRequested;
    }
    Out.bPendingFlush = Spec.bSave && !Out.bSavedToDisk;

    Out.Asset = NewMesh;
    Out.bUpdatedInPlace = bUpdateInPlace;

    // The ASSET's LOD0 counts, read back off the built render data - not MeshCopy's, which is
    // what this returned before and which disagrees with the asset in BOTH directions:
    //
    //   Triangles go DOWN. FStaticMeshBuilder::BuildVertexBuffer skips any triangle with two
    //     corners closer than THRESH_POINTS_ARE_SAME and never emits it into a section
    //     (StaticMeshBuilder.cpp:1644 for the threshold, :1666-1674 for the skip), driven by
    //     FMeshBuildSettings::bRemoveDegenerates, which defaults true and nothing here turns
    //     off.
    //   Vertices go UP. The build splits a position into one render vertex per distinct
    //     normal/tangent/UV/color, so a seam costs extra vertices.
    //
    // NORMATIVE STATEMENT, WITH THE MEASURED NUMBERS, is in docs/pwmodel-format.md under
    // "Four counts: two for the mesh, two for the asset". Worked per-example pairs are deliberately
    // NOT restated here. They were hand-copied into this comment and three others, every copy
    // went stale against a reworked example corpus, and byte-identical copies made the drift
    // invisible to comparison. Cite the doc; do not re-inline a number.
    //
    // Both numbers therefore have to come from the asset, because neither is derivable from
    // the mesh. The pre-build counts are still available to any caller that wants them - they
    // are the mesh it passed in.
    //
    // GetNumTriangles/GetNumVertices return 0 when RenderData is absent, which is why the
    // FinishCompilation above is unconditional rather than inside the lightmap branch.
    Out.TriangleCount = NewMesh->GetNumTriangles(0);
    Out.VertexCount = NewMesh->GetNumVertices(0);
    Out.bSuccess = true;
    return Out;
}
