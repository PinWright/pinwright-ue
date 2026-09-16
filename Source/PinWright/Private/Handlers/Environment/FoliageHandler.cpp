// Copyright (c) 2026 Alexander Penkin. MIT License.

// FoliageHandler.cpp - Migrated from PinWright_FoliageHandlers.cpp
// Foliage painting, removal, instances, types, and procedural foliage handlers

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Environment/FoliageClusterTreeState.h"
#include "Handlers/Environment/ScalabilityDensityCVars.h"
#include "Handlers/Spatial/GroundPlacementUtils.h"
#include "Handlers/Volume/VolumeBrushGeometry.h"
#include "Utils/GuardedLoad.h"
#include "Utils/AssetUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "FoliageTypeObject.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Optional.h"
#include "ProceduralFoliageComponent.h"
#include "ProceduralFoliageSpawner.h"
#include "ProceduralFoliageVolume.h"
#include "UObject/SavePackage.h"
#include "WorldPartition/WorldPartition.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

// Add these includes unconditionally for Editor builds
#include "ActorPartition/ActorPartitionSubsystem.h"
#include "EditorBuildUtils.h"

// The package-path composer this file's four CreatePackage sites go through - and the reason they
// cannot reach either of CreatePackage's Fatals - now lives in Handlers/PackagePathCompose.h,
// included above. It was lifted out of this file when the same defect turned up in
// landscape.create_grass_type (B-landscape-create-grass-type-name-with-slash-kills-the-editor):
// a second namespace needing it makes it shared code, not a file-local helper.

// ---- Helper: Get or create foliage actor safely ----
static AInstancedFoliageActor *
GetOrCreateFoliageActorForWorldSafe(UWorld *World, bool bCreateIfNone) {
  if (!World) {
    return nullptr;
  }

  if (UWorldPartition *WorldPartition = World->GetWorldPartition()) {
    // Check if the world is actually using the Actor Partition Subsystem to
    // avoid crashes in non-partitioned levels that happen to have a WP object.
    if (UActorPartitionSubsystem *ActorPartitionSubsystem =
            World->GetSubsystem<UActorPartitionSubsystem>()) {
      if (ActorPartitionSubsystem->IsLevelPartition()) {
        return AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(
            World, bCreateIfNone);
      }
    }
  }

  // Non-partitioned worlds: avoid ActorPartitionSubsystem ensures by finding or
  // spawning a foliage actor manually.
  TActorIterator<AInstancedFoliageActor> It(World);
  if (It) {
    return *It;
  }

  if (!bCreateIfNone) {
    return nullptr;
  }

  FActorSpawnParameters SpawnParams;
  SpawnParams.ObjectFlags |= RF_Transactional;
  SpawnParams.OverrideLevel = World->PersistentLevel;
  return World->SpawnActor<AInstancedFoliageActor>(SpawnParams);
}

// ---- Helper: resolve or create the auto foliage type for a static mesh ----
// foliage.paint and foliage.add_instances both accept a STATIC MESH path and stand up a
// UFoliageType for it on the fly. Both used to build package "/Game/Foliage/Auto_<Mesh>" while
// naming the object inside it "<Mesh>", so the package handle - the form the content browser
// shows, the form asset.list rows carry, the form a caller writes by hand - normalised to
// "Auto_<Mesh>.Auto_<Mesh>" and resolved to NOTHING
// (B-add-instances-auto-foliage-type-name-mismatch). asset.exists answered false on an asset the
// level was actively drawing, asset.save and foliage.remove answered ASSET_NOT_FOUND, and
// foliage.get_instances answered success with an empty list over eight live instances.
//
// The object now takes the package stem, "Auto_<Mesh>", which is the convention foliage.add_type
// and foliage.create_procedural already follow. Auto_ stays on BOTH halves rather than being
// dropped from the package: "/Game/Foliage/<Mesh>" would collide with a hand-authored type of
// that name from foliage.add_type, and the prefix is the only thing marking the asset as
// generated.
//
// Assets created under the old split are NOT orphaned. An existing "Auto_<Mesh>.<Mesh>" is found
// and reused before anything is created, so a level painted before this fix keeps addressing its
// type by the dotted path it already has (which always resolved), and the package never ends up
// holding two top-level assets - which is what a blind create would have produced.
//
// One helper for both verbs, not one edit each: they have to agree about what a given mesh's
// auto-type is called, or a level painted by one is unaddressable by the other.
//
// OutResolvedPath is always the object's own GetPathName(), never the package handle, so every
// caller publishes a string that round-trips through asset.exists / asset.save /
// foliage.get_instances / foliage.remove.
static UFoliageType *
PinWrightResolveOrCreateAutoFoliageTypeForMesh(UStaticMesh *StaticMesh,
                                               const FString &MeshPath,
                                               FString &OutResolvedPath) {
  if (!StaticMesh) {
    return nullptr;
  }

  const FString BaseName = FPaths::GetBaseFilename(MeshPath);
  const FString AutoName = FString::Printf(TEXT("Auto_%s"), *BaseName);
  // AutoName is derived from the mesh BASENAME, so a caller's slashes cannot reach it - this is
  // the one CreatePackage site in this file the add_type crash could not be driven through. The
  // guard is still applied so "every CreatePackage here is composed through the checked helper"
  // is a property of the file rather than of one reading of it; a mesh whose basename is empty or
  // carries INVALID_LONGPACKAGE_CHARACTERS now yields a null return the callers already handle.
  FString AutoPackagePath;
  FString AutoPathError;
  if (!PinWrightComposeAssetPackagePath(TEXT("/Game/Foliage"), AutoName, AutoPackagePath,
                                        AutoPathError)) {
    return nullptr;
  }
  const FString AutoObjectPath =
      FString::Printf(TEXT("%s.%s"), *AutoPackagePath, *AutoName);
  // Pre-fix layout: package stem Auto_<Mesh>, object <Mesh>. Probed second so a package
  // holding both forms prefers the conventional one.
  const FString LegacyObjectPath =
      FString::Printf(TEXT("%s.%s"), *AutoPackagePath, *BaseName);

  const FString ExistingCandidates[] = {AutoObjectPath, LegacyObjectPath};
  for (const FString &Candidate : ExistingCandidates) {
    if (!ResolveAsset(Candidate).bExists) {
      continue;
    }
    if (UFoliageType *Existing =
            LoadObject<UFoliageType>(nullptr, *Candidate, nullptr, LOAD_NoWarn)) {
      OutResolvedPath = Existing->GetPathName();
      return Existing;
    }
  }

  UPackage *FTPackage = CreatePackage(*AutoPackagePath);
  if (!FTPackage) {
    return nullptr;
  }
  UFoliageType_InstancedStaticMesh *AutoFT =
      NewObject<UFoliageType_InstancedStaticMesh>(FTPackage, FName(*AutoName),
                                                  RF_Public | RF_Standalone);
  if (!AutoFT) {
    return nullptr;
  }
  AutoFT->SetStaticMesh(StaticMesh);
  AutoFT->Density = 100.0f;
  AutoFT->ReapplyDensity = true;
  // MARK DIRTY, deliberately - not SaveAssetToDiskReportingPresence. Three reasons, in order of
  // weight (B-foliage-auto-type-no-disk-write):
  //   1. A scatter needs TWO packages to survive a restart: this type asset AND the level package
  //      holding the AInstancedFoliageActor's instances. Neither foliage.paint nor
  //      foliage.add_instances saves the level. Force-writing only the type would move `saved` to
  //      true for the half nothing can be read back without the other half - a MORE misleading
  //      report than the honest "both are dirty, flush them".
  //   2. Neither verb takes a `save` flag. Writing a .uasset as an unrequested side effect of a
  //      paint is an unannounced mutation; the house model is mark dirty and let the caller flush
  //      (docs/rpc-design.md §5, docs/wiki-src/safe-mutation-save.md).
  //   3. McpSafeAssetSave is also the mark-dirty path foliage.add_type and foliage.create_procedural
  //      already use, so all three foliage creates stay in one persistence regime.
  // The defect was never this call - it was that NOTHING in either response said the type is only
  // dirty. PinWrightReportAutoFoliageTypePersistence below is the other half of this decision and
  // is not optional: without it this line is a silent non-persistence.
  McpSafeAssetSave(AutoFT);
  OutResolvedPath = AutoFT->GetPathName();
  return AutoFT;
}

// ---- Helper: measured presence of the foliage actor a verb just touched ----
// foliage.paint, foliage.remove, foliage.get_instances and foliage.add_instances all closed with
//   Resp->SetStringField("foliageActorPath", IFA->GetPathName());
//   Resp->SetBoolField("existsAfter", true);
// on a branch where IFA was already known non-null, so existsAfter was a CONSTANT published under
// a measurement name - the shape docs/rpc-design.md §1 exists to kill, and the shape that let a
// caller read "existsAfter:true" as proof its scatter was durable
// (B-foliage-auto-type-no-disk-write).
//
// existsAfter is now the round trip the write path cannot fake: the very string the response
// PUBLISHES is resolved BACK through FindObject and compared against the actor it names, so a
// handle that does not address the actor it claims to reports false. That is the same failure the
// sibling ticket B-add-instances-auto-foliage-type-name-mismatch shipped on the type-asset side -
// a published path that resolved to nothing - and it was undetectable here by construction.
//
// One helper for all four verbs so the field cannot drift back to a literal in one of them, and
// so the foliageActorPath spelling lives in one place instead of four.
static void PinWrightWriteFoliageActorPresence(const TSharedPtr<FJsonObject> &Resp,
                                               AInstancedFoliageActor *IFA) {
  if (!Resp.IsValid()) {
    return;
  }
  if (!IsValid(IFA)) {
    // OMIT rather than assert. With no actor there is nothing to measure, and writing false here
    // would be a claim about a level this verb never reached.
    return;
  }
  const FString ActorPath = IFA->GetPathName();
  Resp->SetStringField(TEXT("foliageActorPath"), ActorPath);
  Resp->SetBoolField(TEXT("existsAfter"),
      FindObject<AInstancedFoliageActor>(nullptr, *ActorPath) == IFA);
}

// ---- Helper: honest persistence report for an AUTO-CREATED foliage type ----
// Emitted by foliage.paint and foliage.add_instances only on the branch that actually stood a type
// up from a static-mesh path. A caller who passed a real UFoliageType path had no asset created or
// dirtied on its behalf, so it gets none of these fields: OMIT rather than assert.
//
// Publishes the plugin's two existing measured contracts rather than a third spelling of them:
//   AddMarkDirtySaveReport      -> {saveRequested, markedForSave, saved, pendingFlush}, with
//                                  `saved` MEASURED via IsAssetPersistedToDisk. A freshly created
//                                  type answers saved:false + pendingFlush:true (the truth the old
//                                  response never carried at all); a REUSED type that is already
//                                  written and clean answers saved:true. The same measurement
//                                  therefore covers both branches of the resolve-or-create helper
//                                  with no created/reused flag to keep in sync.
//   AddAssetVerificationNested  -> the measured existsAfter / existsOnDisk / pendingSave triple for
//                                  the TYPE, under its own "foliageType" object so it can never be
//                                  confused with the top-level existsAfter, which is about the
//                                  foliage ACTOR.
// saveRequested names the REQUEST separately from the measurement, per the same contract the
// audio / niagara / metasound creates publish, so a caller never has to know which save family ran.
//
// Finally, when the measurement and the response's own upbeat top-level fields disagree, it says so
// in warnings[] and names the remedy. Appended to any warnings[] the verb already wrote rather than
// replacing it.
static void PinWrightReportAutoFoliageTypePersistence(const TSharedPtr<FJsonObject> &Resp,
                                                      UFoliageType *AutoType) {
  if (!Resp.IsValid() || !AutoType) {
    return;
  }

  AddMarkDirtySaveReport(Resp, AutoType, /*bSaveRequested=*/true);
  AddAssetVerificationNested(Resp, TEXT("foliageType"), AutoType);

  if (IsAssetPersistedToDisk(AutoType)) {
    return;
  }

  TArray<TSharedPtr<FJsonValue>> Warnings;
  const TArray<TSharedPtr<FJsonValue>> *ExistingWarnings = nullptr;
  if (Resp->TryGetArrayField(TEXT("warnings"), ExistingWarnings) && ExistingWarnings) {
    Warnings = *ExistingWarnings;
  }
  Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
      TEXT("The foliage type '%s' was AUTO-CREATED for this call and is DIRTY IN MEMORY, NOT YET "
           "WRITTEN: no .uasset exists for it yet, so these instances lose the type they reference "
           "on the next editor restart. Call asset.save on that path, and save the level that holds "
           "the instances, before quitting. Read saved / pendingFlush and foliageType.existsOnDisk "
           "for durability - existsAfter is about the foliage ACTOR, not this asset."),
      *AutoType->GetPathName())));
  Resp->SetArrayField(TEXT("warnings"), Warnings);
}

// ---- Helper: per-type scale range and normal alignment ----
// foliage.add_type and foliage.create_procedural both accept minScale / maxScale /
// alignToNormal and write them onto the same UFoliageType_InstancedStaticMesh. The read,
// the validation and the write live here so the two verbs cannot drift: create_procedural
// used to accept the three keys and write none of them, producing a scatter where every
// plant was the same size and vertical
// (B-create-procedural-ignores-scale-and-normal-fields).
struct FFoliageScaleAndAlignInput {
  double MinScale = 1.0;
  double MaxScale = 1.0;
  bool bAlignToNormal = true;
};

// Reads the three optional keys off Source, leaving each at its default when absent.
// Returns false with OutError set when the scales are non-positive or inverted; the
// caller decides whether that rejects the whole request (add_type, one type per call) or
// skips one entry (create_procedural, a batch with a skipped[] channel).
static bool ReadFoliageScaleAndAlign(const FJsonObject &Source,
                                     FFoliageScaleAndAlignInput &OutInput,
                                     FString &OutError) {
  Source.TryGetNumberField(TEXT("minScale"), OutInput.MinScale);
  Source.TryGetNumberField(TEXT("maxScale"), OutInput.MaxScale);
  Source.TryGetBoolField(TEXT("alignToNormal"), OutInput.bAlignToNormal);

  if (OutInput.MinScale <= 0.0 || OutInput.MaxScale <= 0.0) {
    OutError = TEXT("Scales must be positive");
    return false;
  }
  if (OutInput.MinScale > OutInput.MaxScale) {
    OutError = FString::Printf(
        TEXT("minScale (%f) cannot be greater than maxScale (%f)"),
        OutInput.MinScale, OutInput.MaxScale);
    return false;
  }
  return true;
}

// Writes validated inputs onto a foliage type. Uniform scaling with one interval driving
// all three axes is what foliage.add_type has always written; keeping it here means the
// procedural path produces types identical to hand-authored ones.
static void ApplyFoliageScaleAndAlign(UFoliageType_InstancedStaticMesh *FoliageType,
                                      const FFoliageScaleAndAlignInput &Input) {
  if (!FoliageType) {
    return;
  }
  FoliageType->Scaling = EFoliageScaling::Uniform;
  FoliageType->ScaleX.Min = static_cast<float>(Input.MinScale);
  FoliageType->ScaleX.Max = static_cast<float>(Input.MaxScale);
  FoliageType->ScaleY.Min = static_cast<float>(Input.MinScale);
  FoliageType->ScaleY.Max = static_cast<float>(Input.MaxScale);
  FoliageType->ScaleZ.Min = static_cast<float>(Input.MinScale);
  FoliageType->ScaleZ.Max = static_cast<float>(Input.MaxScale);
  FoliageType->AlignToNormal = Input.bAlignToNormal;
}

// ---- Helper: per-type PROCEDURAL simulation properties ----
// foliage.create_procedural drives UProceduralFoliageSpawner's tile simulation, and every
// number that simulation reads lives on the UFoliageType the verb builds. The handler used to
// write only the PAINTING half of that UClass - Density / ReapplyDensity plus
// ApplyFoliageScaleAndAlign's ScaleX/Y/Z - so fourteen Category=Procedural properties stayed
// at their CDO values with no way for a caller to reach them
// (F-procedural-foliage-simulation-knobs-unreachable), and the one per-type knob the verb DID
// advertise, `density`, landed on the paint brush while the seeding loop read
// InitialSeedDensity (B-create-procedural-density-writes-paint-density).
//
// These deliberately do NOT go into ApplyFoliageScaleAndAlign. That helper is shared because
// foliage.add_type accepts the same three keys and the two verbs could drift on them.
// add_type accepts none of the keys below and runs no simulation - its `density` really is
// the paint brush's instances-per-1000x1000uu - so there is no shared surface here to drift.
//
// Every field is optional and unset means "leave the engine CDO alone", which is why they are
// TOptional rather than defaulted copies of the CDO: a hardcoded default here would silently
// stop tracking the engine.
struct FFoliageProceduralSimInput {
  TOptional<double> InitialSeedDensity;
  TOptional<double> CollisionRadius;
  TOptional<double> ShadeRadius;
  TOptional<double> NumSteps;
  TOptional<double> SeedsPerStep;
  TOptional<double> AverageSpreadDistance;
  TOptional<double> MaxAge;
  TOptional<double> MaxInitialAge;
  TOptional<double> OverlapPriority;
  TOptional<double> RandomPitchAngle;
  TOptional<bool> bCanGrowInShade;
  TOptional<bool> bSpawnsInShade;
  TOptional<FFloatInterval> ProceduralScale;
  TOptional<FFloatInterval> Height;
  TOptional<FFloatInterval> GroundSlopeAngle;
};

// FFloatInterval-valued key, accepted as {min, max} or [min, max] (the same two shapes
// bounds.size already takes). Absent leaves OutValue unset.
static bool ReadFoliageSimIntervalField(const FJsonObject &Source, const TCHAR *Key,
                                        TOptional<FFloatInterval> &OutValue,
                                        FString &OutError) {
  double Min = 0.0;
  double Max = 0.0;
  const TSharedPtr<FJsonObject> *IntervalObj = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *IntervalArr = nullptr;
  if (Source.TryGetObjectField(Key, IntervalObj) && IntervalObj) {
    if (!(*IntervalObj)->TryGetNumberField(TEXT("min"), Min) ||
        !(*IntervalObj)->TryGetNumberField(TEXT("max"), Max)) {
      OutError = FString::Printf(TEXT("%s requires both min and max"), Key);
      return false;
    }
  } else if (Source.TryGetArrayField(Key, IntervalArr) && IntervalArr) {
    if (IntervalArr->Num() < 2) {
      OutError = FString::Printf(TEXT("%s as an array needs [min, max]"), Key);
      return false;
    }
    Min = (*IntervalArr)[0]->AsNumber();
    Max = (*IntervalArr)[1]->AsNumber();
  } else {
    // Present but in neither shape (a bare scalar, a string) is a caller error, not an
    // absence - dropping it would be the silent-write class these tickets are about.
    if (Source.HasField(FString(Key))) {
      OutError =
          FString::Printf(TEXT("%s must be an object {min, max} or an array [min, max]"), Key);
      return false;
    }
    return true;
  }
  if (Min > Max) {
    OutError = FString::Printf(TEXT("%s min (%f) cannot be greater than max (%f)"), Key, Min,
                               Max);
    return false;
  }
  OutValue = FFloatInterval(static_cast<float>(Min), static_cast<float>(Max));
  return true;
}

// Scalar key with an inclusive accepted range (the engine's own ClampMin/ClampMax where it
// declares one). Absent leaves OutValue unset.
static bool ReadFoliageSimScalarField(const FJsonObject &Source, const TCHAR *Key,
                                      double MinAllowed, double MaxAllowed,
                                      TOptional<double> &OutValue, FString &OutError) {
  double Value = 0.0;
  if (!Source.TryGetNumberField(Key, Value)) {
    if (Source.HasField(FString(Key))) {
      OutError = FString::Printf(TEXT("%s must be a number"), Key);
      return false;
    }
    return true;
  }
  // Only the violated side is named: the "no upper bound" case passes DBL_MAX in, and
  // printing that at a caller would be noise rather than information.
  if (Value < MinAllowed) {
    OutError = FString::Printf(TEXT("%s (%f) must be at least %f"), Key, Value, MinAllowed);
    return false;
  }
  if (Value > MaxAllowed) {
    OutError = FString::Printf(TEXT("%s (%f) must be at most %f"), Key, Value, MaxAllowed);
    return false;
  }
  OutValue = Value;
  return true;
}

static bool ReadFoliageSimBoolField(const FJsonObject &Source, const TCHAR *Key,
                                    TOptional<bool> &OutValue, FString &OutError) {
  bool Value = false;
  if (Source.TryGetBoolField(Key, Value)) {
    OutValue = Value;
    return true;
  }
  if (Source.HasField(FString(Key))) {
    OutError = FString::Printf(TEXT("%s must be a boolean"), Key);
    return false;
  }
  return true;
}

// Reads every procedural key off one foliageTypes[] entry. Returns false with OutError set on
// a value the engine would clamp or reject; the caller routes that through NoteSkippedType so
// one bad entry does not fail the batch.
static bool ReadFoliageProceduralSimConfig(const FJsonObject &Source,
                                           FFoliageProceduralSimInput &Out,
                                           FString &OutError) {
  const double NoBound = TNumericLimits<double>::Max();
  if (!ReadFoliageSimScalarField(Source, TEXT("initialSeedDensity"), 0.0, NoBound,
                                 Out.InitialSeedDensity, OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("collisionRadius"), 0.0, NoBound,
                                 Out.CollisionRadius, OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("shadeRadius"), 0.0, NoBound, Out.ShadeRadius,
                                 OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("numSteps"), 0.0, NoBound, Out.NumSteps,
                                 OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("seedsPerStep"), 0.0, NoBound, Out.SeedsPerStep,
                                 OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("averageSpreadDistance"), 0.0, NoBound,
                                 Out.AverageSpreadDistance, OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("maxAge"), 0.0, NoBound, Out.MaxAge, OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("maxInitialAge"), 0.0, NoBound, Out.MaxInitialAge,
                                 OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("overlapPriority"), -NoBound, NoBound,
                                 Out.OverlapPriority, OutError) ||
      !ReadFoliageSimScalarField(Source, TEXT("randomPitchAngle"), 0.0, 359.0,
                                 Out.RandomPitchAngle, OutError)) {
    return false;
  }
  if (!ReadFoliageSimIntervalField(Source, TEXT("proceduralScale"), Out.ProceduralScale,
                                   OutError) ||
      !ReadFoliageSimIntervalField(Source, TEXT("height"), Out.Height, OutError) ||
      !ReadFoliageSimIntervalField(Source, TEXT("groundSlopeAngle"), Out.GroundSlopeAngle,
                                   OutError)) {
    return false;
  }
  // The engine declares ClampMin 0.001 on ProceduralScale; a zero minimum makes
  // GetScaleForAge return 0 at age 0, i.e. an invisible instance rather than a small one.
  if (Out.ProceduralScale.IsSet() && Out.ProceduralScale.GetValue().Min < 0.001f) {
    OutError = TEXT("proceduralScale min must be at least 0.001");
    return false;
  }
  if (!ReadFoliageSimBoolField(Source, TEXT("canGrowInShade"), Out.bCanGrowInShade, OutError) ||
      !ReadFoliageSimBoolField(Source, TEXT("spawnsInShade"), Out.bSpawnsInShade, OutError)) {
    return false;
  }
  return true;
}

// Which decision produced each of the two DERIVED values, so the response can say where a
// number the caller never typed came from.
struct FFoliageProceduralSimResolution {
  FString SeedDensitySource = TEXT("derived_from_density");
  FString MaxInitialAgeSource = TEXT("default");
};

static FFoliageProceduralSimResolution
ApplyFoliageProceduralSimConfig(UFoliageType_InstancedStaticMesh *FoliageType,
                                const FFoliageProceduralSimInput &Input, double PaintDensity) {
  FFoliageProceduralSimResolution Resolution;
  if (!FoliageType) {
    return Resolution;
  }

  // SEEDING. FProceduralFoliageTile::Simulate takes its seed count from
  // GetSeedDensitySquared() * (TileSize^2 / 1000^2) and never opens Density, so the
  // paint-brush knob this verb used to write was inert on the only path it has. The two are
  // not the same unit: `density` is instances per 1000x1000 uu, InitialSeedDensity is seeds
  // along 10 m IMPLICITLY SQUARED over 10 m x 10 m - so the derivation is sqrt, and a raw copy
  // would turn density 100 into 10 000 seeds per tile. An explicit initialSeedDensity wins and
  // is taken verbatim; either way the effective value is echoed with its source.
  if (Input.InitialSeedDensity.IsSet()) {
    FoliageType->InitialSeedDensity = static_cast<float>(Input.InitialSeedDensity.GetValue());
    Resolution.SeedDensitySource = TEXT("supplied");
  } else {
    FoliageType->InitialSeedDensity =
        static_cast<float>(FMath::Sqrt(FMath::Max(0.0, PaintDensity)));
    Resolution.SeedDensitySource = TEXT("derived_from_density");
  }

  if (Input.CollisionRadius.IsSet()) {
    FoliageType->CollisionRadius = static_cast<float>(Input.CollisionRadius.GetValue());
  }
  if (Input.ShadeRadius.IsSet()) {
    FoliageType->ShadeRadius = static_cast<float>(Input.ShadeRadius.GetValue());
  }
  // RoundToInt32, not RoundToInt: the double overload of the latter returns int64 and
  // narrowing it into these int32 properties is a warning the build treats as an error.
  if (Input.NumSteps.IsSet()) {
    FoliageType->NumSteps = FMath::RoundToInt32(Input.NumSteps.GetValue());
  }
  if (Input.SeedsPerStep.IsSet()) {
    FoliageType->SeedsPerStep = FMath::RoundToInt32(Input.SeedsPerStep.GetValue());
  }
  if (Input.AverageSpreadDistance.IsSet()) {
    FoliageType->AverageSpreadDistance =
        static_cast<float>(Input.AverageSpreadDistance.GetValue());
  }
  if (Input.OverlapPriority.IsSet()) {
    FoliageType->OverlapPriority = static_cast<float>(Input.OverlapPriority.GetValue());
  }
  if (Input.bCanGrowInShade.IsSet()) {
    FoliageType->bCanGrowInShade = Input.bCanGrowInShade.GetValue();
  }
  if (Input.bSpawnsInShade.IsSet()) {
    FoliageType->bSpawnsInShade = Input.bSpawnsInShade.GetValue();
  }
  if (Input.RandomPitchAngle.IsSet()) {
    FoliageType->RandomPitchAngle = static_cast<float>(Input.RandomPitchAngle.GetValue());
  }
  if (Input.Height.IsSet()) {
    FoliageType->Height = Input.Height.GetValue();
  }
  if (Input.GroundSlopeAngle.IsSet()) {
    FoliageType->GroundSlopeAngle = Input.GroundSlopeAngle.GetValue();
  }
  // Written alongside ScaleX/Y/Z rather than instead of them: the procedural branch of
  // FPotentialInstance::PlaceInstance takes GetScaleForAge (which interpolates
  // ProceduralScale) while the painting branch takes GetRandomScale (which reads ScaleX/Y/Z),
  // and foliage.add_type's painting path genuinely needs the latter.
  if (Input.ProceduralScale.IsSet()) {
    FoliageType->ProceduralScale = Input.ProceduralScale.GetValue();
  }
  if (Input.MaxAge.IsSet()) {
    FoliageType->MaxAge = static_cast<float>(Input.MaxAge.GetValue());
  }

  // ORDER MATTERS HERE, and this is the one place in the verb where it does: MaxInitialAge is
  // resolved LAST, after MaxAge and ProceduralScale have been written, because it depends on
  // both. GetInitAge returns MaxInitialAge * RandomStream.GetFraction(), so at the engine
  // default of 0 every seed starts at age exactly 0; GetNextAge then advances by integer 1 per
  // step over NumSteps steps, and GetScaleForAge evaluates the scale curve at Age / MaxAge. A
  // caller who supplies a proceduralScale range therefore gets NumSteps+1 discrete sizes out
  // of it - measured as {1.0, 1.2, 1.4, 1.6} on a live level - so their correct input reads as
  // a no-op. Raising MaxInitialAge to the EFFECTIVE MaxAge (which is why MaxAge must already
  // be written) spreads the initial ages continuously over [0, MaxAge], which is what makes
  // the requested range observable at all. Only done when the caller asked for a non-degenerate
  // range and did not pin the age themselves; an explicit maxInitialAge - 0 included - is
  // honoured verbatim, and the response names which of the three happened.
  if (Input.MaxInitialAge.IsSet()) {
    FoliageType->MaxInitialAge = static_cast<float>(Input.MaxInitialAge.GetValue());
    Resolution.MaxInitialAgeSource = TEXT("supplied");
  } else if (Input.ProceduralScale.IsSet() &&
             FoliageType->ProceduralScale.Max > FoliageType->ProceduralScale.Min &&
             FoliageType->MaxInitialAge <= 0.0f) {
    FoliageType->MaxInitialAge = FoliageType->MaxAge;
    Resolution.MaxInitialAgeSource = TEXT("raised_for_procedural_scale");
  } else {
    Resolution.MaxInitialAgeSource = TEXT("default");
  }
  return Resolution;
}

// Measured, not requested: every value here is read back off the type the handler just built,
// so a derived seed density or a raised initial age is visible without opening the generated
// asset.
static TSharedPtr<FJsonObject>
MakeFoliageProceduralSimEcho(const UFoliageType_InstancedStaticMesh *FoliageType,
                             const FFoliageProceduralSimResolution &Resolution) {
  TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
  if (!FoliageType) {
    return Row;
  }
  auto SetIntervalField = [&Row](const TCHAR *Key, const FFloatInterval &Interval) {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("min"), Interval.Min);
    Obj->SetNumberField(TEXT("max"), Interval.Max);
    Row->SetObjectField(Key, Obj);
  };
  // Both density numbers, because they are different units on the same asset and only one of
  // them decides the scatter.
  Row->SetNumberField(TEXT("density"), FoliageType->Density);
  Row->SetNumberField(TEXT("initial_seed_density"), FoliageType->InitialSeedDensity);
  Row->SetStringField(TEXT("initial_seed_density_source"), Resolution.SeedDensitySource);
  SetIntervalField(TEXT("procedural_scale"), FoliageType->ProceduralScale);
  Row->SetNumberField(TEXT("max_age"), FoliageType->MaxAge);
  Row->SetNumberField(TEXT("max_initial_age"), FoliageType->MaxInitialAge);
  Row->SetStringField(TEXT("max_initial_age_source"), Resolution.MaxInitialAgeSource);
  Row->SetNumberField(TEXT("num_steps"), FoliageType->NumSteps);
  Row->SetNumberField(TEXT("seeds_per_step"), FoliageType->SeedsPerStep);
  Row->SetNumberField(TEXT("average_spread_distance"), FoliageType->AverageSpreadDistance);
  Row->SetNumberField(TEXT("collision_radius"), FoliageType->CollisionRadius);
  Row->SetNumberField(TEXT("shade_radius"), FoliageType->ShadeRadius);
  // The number the overlapPriority ordering rule is actually about: UFoliageType::GetMaxRadius
  // is max(CollisionRadius, ShadeRadius), and it decides how many overlap contests a species
  // enters.
  Row->SetNumberField(TEXT("effective_radius"),
                      FMath::Max(FoliageType->CollisionRadius, FoliageType->ShadeRadius));
  Row->SetNumberField(TEXT("overlap_priority"), FoliageType->OverlapPriority);
  Row->SetBoolField(TEXT("can_grow_in_shade"), FoliageType->bCanGrowInShade);
  Row->SetBoolField(TEXT("spawns_in_shade"), FoliageType->bSpawnsInShade);
  // GetSpawnsInShade() is bCanGrowInShade && bSpawnsInShade, so spawnsInShade alone stores a
  // true the simulation never reads. Report what will actually be read, not only what was
  // stored.
  Row->SetBoolField(TEXT("spawns_in_shade_effective"), FoliageType->GetSpawnsInShade());
  Row->SetNumberField(TEXT("random_pitch_angle"), FoliageType->RandomPitchAngle);
  SetIntervalField(TEXT("height"), FoliageType->Height);
  SetIntervalField(TEXT("ground_slope_angle"), FoliageType->GroundSlopeAngle);
  return Row;
}

// ---- foliage.paint ----
// The verb's name is a promise the code has to keep. Engine foliage painting is trace-driven
// placement - FPotentialInstance::PlaceInstance takes a hit location, a hit normal and a hit
// component and derives everything from them - while this handler used to hand-build an
// FFoliageInstance and call FFoliageInfo::AddInstance, which is the STORAGE call and applies no
// placement at all. Every instance landed at the literal XYZ, pointing straight up, and the
// response could not reveal it (B-foliage-paint-does-no-ground-projection).
//
// Projection now runs through GroundPlacement::SeatInstance - the same measure/seat solve
// spatial.ground_instances uses - so the two verbs cannot disagree about where the ground is or
// what "seated" means. The seat is taken as a DRY RUN and the write is done through the foliage
// bookkeeping (see the loop), because SeatInstance's own write is
// UInstancedStaticMeshComponent::UpdateInstanceTransform, which would move the render instance
// while leaving FFoliageInfo::Instances and its location hash pointing at the old Z.
//
// It is opt-in by `surface` rather than on by default, and that is the deliberate half: the
// ground module refuses to guess what counts as ground, so a default-on projection would have had
// to invent a preset. What actually closes the ticket is the `projected` field and the warning -
// the response can no longer be read as "seated" when nothing was measured.
REGISTER_RPC_HANDLER("foliage.paint", "foliage",
    "Paint foliage instances at the supplied locations. Supply `surface` and every XY is SEATED onto that ground through the same measure/seat solve spatial.ground_instances uses, with the Z each instance actually reached, the signed drop, and a groundProvenance block naming WHAT answered the probe (which collision representation, and the component and class of the primitive) reported per instance in placed[] - the same block, from the same writer, the spatial ground verbs publish. WITHOUT `surface` the verb writes the literal z - no trace, no projection, no normal alignment - and says so in the response (projected:false plus a warnings[] line), because a green instancesPlaced over unprojected foliage used to be indistinguishable from a seated one. Entries the parser cannot use, and columns with no accepted ground under them, are reported in skipped[] ({index, reason}, capped) with the true total in skippedCount; an instance whose column found no ground is WITHDRAWN rather than left floating. ROTATION comes from the resolved foliage type, not from a zero rotator: RandomYaw and RandomPitchAngle are applied on both branches (they need no surface) and AlignToNormal only while projecting, against the ground normal the seat solve already measured. The `rotation` block echoes what the type asked for next to the measured alignedCount, placed[] carries the pitch/yaw/roll actually written per instance on BOTH branches, and an AlignToNormal that could not be honoured is stated in warnings[] rather than dropped. Instance SCALE and ZOffset are still hardcoded to 1 and 0 and do NOT read the type's ScaleX/Y/Z or ZOffset ranges. foliage.add_instances is the richer literal-placement verb (per-instance rotation and scale). instancesPlaced is the editor-side LEDGER count; expectedDrawnInstances is how many of them the renderer is expected to draw, because the foliage.DensityScale cvar (driven by sg.FoliageQuality) culls a random fraction of an opted-in type's instances out of the HISM cluster tree without removing them. densityScalingEnabled is the type's bEnableDensityScaling, foliageDensityScaleCVar the measured cvar, and cvarWarning names the remedy when the two counts differ. Both effective fields are OMITTED, never guessed, when the cvar is not in this host's registry. Does not change the cvar. The verb REBUILDS the HISM cluster tree after the batch on BOTH branches - the engine's add path suppresses that rebuild and does not restore it, and what used to save the projecting branch was a side effect of the seat solve's PostMoveInstances that the unprojected branch never had. builtInstanceCount then reports the component's NumBuiltInstances (instances actually in the built tree) and clusterTreeUpToDate its IsTreeFullyBuilt(); both are OMITTED, never zeroed, when there is no cluster tree to read, and clusterTreeWarning fires when the tree holds fewer than were stored. That is cluster-tree MEMBERSHIP, a different layer from expectedDrawnInstances.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("foliageTypePath", "path", "Path to foliage type or static mesh asset", "foliageType"),
        RPC_PARAM_OPT_ALIAS("locations", "array", "Array of {x,y,z} positions to paint. x, y and z are all REQUIRED per entry: an entry missing any of them places nothing and is reported in skipped[], instead of being placed at the world origin. While projecting, z only says where the ground probe starts - the placed Z is measured, not echoed.", "location"),
        RPC_PARAM_OPT("position", "object", "Single {x,y,z} position (alternative to locations)"),
        RPC_PARAM_OPT("surface", "object",
            "What counts as ground. Supplying it turns projection ON. {preset, channel?, "
            "traceComplex?, excludeEffectGeometry?, onlyClasses?, excludeClasses?, excludeNames?, "
            "ignoreActors?, maxLayers?, maxDrop?, probeLift?}. preset is 'landscape' (only "
            "LandscapeProxy - the right answer for terrain, because a landscape heightfield is "
            "single-valued per column so nothing can shadow it), 'any_solid' (anything blocking "
            "except foliage and effect geometry) or 'custom' (your filters only). Explicit lists "
            "ADD to the preset. There is no default surface: a wrong one is the cause of every "
            "floating-vegetation bug this projection exists to prevent, so the verb will not "
            "guess one."),
        RPC_PARAM_OPT_ALIAS("projectToGround", "boolean",
            "Seat each instance onto `surface` instead of writing the literal z. There is no fixed "
            "default: it is TRUE when `surface` is supplied and FALSE when it is not, so naming a "
            "ground surface is the whole opt-in. true without a `surface` is INVALID_SURFACE_SPEC "
            "rather than a guessed preset; false with one places literally and still reports "
            "projected:false. Also accepted as 'project_to_ground'.",
            "project_to_ground")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("paint_foliage payload missing"));
    return true;
  }

  FString FoliageTypePath;
  if (!Payload->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath)) {
    // Accept alternate key used by some clients
    Payload->TryGetStringField(TEXT("foliageType"), FoliageTypePath);
  }
  if (FoliageTypePath.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("foliageTypePath (or foliageType) required"));
    return true;
  }

  // Security: Validate path format
  FString SafePath = SanitizeProjectRelativePath(FoliageTypePath);
  if (SafePath.IsEmpty()) {
    Ctx.SendError(TEXT("SECURITY_VIOLATION"),
        FString::Printf(TEXT("Invalid or unsafe foliage type path: %s"), *FoliageTypePath));
    return true;
  }
  FoliageTypePath = SafePath;

  // Auto-resolve simple name
  if (!FoliageTypePath.IsEmpty() &&
      FPaths::GetPath(FoliageTypePath).IsEmpty()) {
    FoliageTypePath =
        FString::Printf(TEXT("/Game/Foliage/%s"), *FoliageTypePath);
  }

  // Validate after auto-resolve
  if (FoliageTypePath.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("foliageTypePath (or foliageType) required"));
    return true;
  }

  // Partial-success reporting, in the same skipped / skippedCount / skippedTruncated shape
  // foliage.add_instances and actor.spawn_batch already speak. Two entry classes used to vanish
  // here: a non-object entry fell through the Type == EJson::Object test with no record at all,
  // and an entry carrying no x/y/z kept the zero initialisers and was placed at the WORLD ORIGIN
  // - a position the caller never supplied and could not detect afterwards.
  constexpr int32 MaxSkippedDetail = 32;
  TArray<TSharedPtr<FJsonValue>> SkippedArray;
  int32 SkippedCount = 0;
  auto NoteSkipped = [&](int32 EntryIndex, const FString &Reason) {
    ++SkippedCount;
    if (SkippedArray.Num() < MaxSkippedDetail) {
      TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
      SkipObj->SetNumberField(TEXT("index"), EntryIndex);
      SkipObj->SetStringField(TEXT("reason"), Reason);
      SkippedArray.Add(MakeShared<FJsonValueObject>(SkipObj));
    }
  };

  // All three axes are required. A missing field used to leave the 0 initialiser standing, which
  // is indistinguishable from a caller who meant 0 - so the names of the missing ones are
  // returned rather than substituted for.
  auto ReadPoint = [](const TSharedPtr<FJsonObject> &Obj, FVector &OutPoint,
                      FString &OutMissing) {
    static const TCHAR *const Keys[3] = {TEXT("x"), TEXT("y"), TEXT("z")};
    double Values[3] = {0.0, 0.0, 0.0};
    TArray<FString> Missing;
    for (int32 Axis = 0; Axis < 3; ++Axis) {
      if (!Obj->TryGetNumberField(Keys[Axis], Values[Axis])) {
        Missing.Add(Keys[Axis]);
      }
    }
    if (Missing.Num() > 0) {
      OutMissing = FString::Join(Missing, TEXT(", "));
      return false;
    }
    OutPoint = FVector(Values[0], Values[1], Values[2]);
    return true;
  };

  // Accept single 'position' or array of 'locations'. LocationIndices keeps each accepted point
  // tied to the index the caller sent it at, so a skip raised later - by the ground probe - names
  // the caller's entry rather than a post-filter position.
  TArray<FVector> Locations;
  TArray<int32> LocationIndices;
  const TArray<TSharedPtr<FJsonValue>> *LocationsArray = nullptr;
  if ((Payload->TryGetArrayField(TEXT("locations"), LocationsArray) ||
       Payload->TryGetArrayField(TEXT("location"), LocationsArray)) &&
      LocationsArray && LocationsArray->Num() > 0) {
    for (int32 EntryIndex = 0; EntryIndex < LocationsArray->Num(); ++EntryIndex) {
      const TSharedPtr<FJsonValue> &Val = (*LocationsArray)[EntryIndex];
      const TSharedPtr<FJsonObject> *Obj = nullptr;
      if (!Val.IsValid() || Val->Type != EJson::Object || !Val->TryGetObject(Obj) || !Obj) {
        NoteSkipped(EntryIndex, TEXT("locations[] entry is not an object; supply {x,y,z}"));
        continue;
      }
      FVector Point = FVector::ZeroVector;
      FString Missing;
      if (!ReadPoint(*Obj, Point, Missing)) {
        NoteSkipped(EntryIndex, FString::Printf(
            TEXT("locations[] entry has no %s; x, y and z are all required, and an entry missing "
                 "one is reported here rather than placed at the world origin"), *Missing));
        continue;
      }
      Locations.Add(Point);
      LocationIndices.Add(EntryIndex);
    }
  } else {
    // Try a single 'position' object
    const TSharedPtr<FJsonObject> *PosObj = nullptr;
    if ((Payload->TryGetObjectField(TEXT("position"), PosObj) ||
         Payload->TryGetObjectField(TEXT("location"), PosObj)) &&
        PosObj) {
      FVector Point = FVector::ZeroVector;
      FString Missing;
      if (!ReadPoint(*PosObj, Point, Missing)) {
        // A single position has no array to report a skip into, so an unusable one is the whole
        // request failing rather than a zero-instance success.
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(
            TEXT("'position' has no %s; x, y and z are all required."), *Missing));
        return true;
      }
      Locations.Add(Point);
      LocationIndices.Add(0);
    }
  }

  if (Locations.Num() == 0) {
    if (SkippedCount > 0) {
      Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(
          TEXT("All %d supplied location entries were unusable, so nothing was painted. Each "
               "entry must be an object carrying x, y and z."), SkippedCount));
      return true;
    }
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("locations array or position required"));
    return true;
  }

  if (!GEditor || !GEditor->GetEditorWorldContext().World()) {
    Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
    return true;
  }

  UWorld *World = GEditor->GetEditorWorldContext().World();

  // ---- Projection ----
  // Naming a ground surface IS the opt-in: bProjectToGround starts as "a surface was supplied".
  // The ground module's standing rule is that nothing may guess what counts as ground - a wrong
  // surface is the cause of every floating-prop bug it exists to prevent - so projecting by
  // default would have meant inventing a preset, and refusing by default would have meant every
  // existing caller of a verb that never projected suddenly getting an error. What closes the
  // defect either way is that the response now states which of the two happened.
  //
  // Read BEFORE the foliage type is resolved, because that step can create and save
  // /Game/Foliage/Auto_<Mesh>: a rejected surface must not leave an asset behind.
  const TSharedPtr<FJsonObject> *SurfaceObj = nullptr;
  const bool bHasSurface =
      Payload->TryGetObjectField(TEXT("surface"), SurfaceObj) && SurfaceObj != nullptr;

  bool bProjectToGround = bHasSurface;
  if (!Payload->TryGetBoolField(TEXT("projectToGround"), bProjectToGround)) {
    Payload->TryGetBoolField(TEXT("project_to_ground"), bProjectToGround);
  }

  GroundPlacement::FGroundSurfaceSpec Surface;
  if (bProjectToGround) {
    // Only reachable when the caller explicitly asked for projection, since the default is the
    // surface's own presence. An explicit request with nothing to project onto is an error, not
    // a silent downgrade to literal placement.
    if (!bHasSurface) {
      Ctx.SendError(TEXT("INVALID_SURFACE_SPEC"),
          TEXT("projectToGround was requested but 'surface' is missing. State what counts as "
               "ground: {\"preset\":\"landscape\"} for terrain, {\"preset\":\"any_solid\"} for "
               "anything solid except foliage and effect geometry, or {\"preset\":\"custom\", "
               "\"onlyClasses\":[...]} to say it exactly. There is no default - a wrong ground "
               "surface is the cause of every floating-vegetation bug."));
      return true;
    }
    TArray<FString> UnresolvedIgnores;
    FString SurfaceError;
    if (!GroundPlacement::ParseSurfaceJson(*SurfaceObj, World, Surface, UnresolvedIgnores,
                                           SurfaceError)) {
      Ctx.SendError(TEXT("INVALID_SURFACE_SPEC"), SurfaceError);
      return true;
    }
  }

  // Try to load as FoliageType first
  UFoliageType *FoliageType = nullptr;
  if (ResolveAsset(FoliageTypePath).bExists) {
    FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
  }

  // If not a FoliageType, try loading as StaticMesh and auto-create FoliageType
  // Gates the persistence report at the tail: a caller that passed a real UFoliageType path had no
  // asset created or dirtied on its behalf, so it gets no save fields at all rather than fields
  // about work this call did not do.
  bool bAutoResolvedFoliageType = false;
  if (!FoliageType) {
    UStaticMesh *StaticMesh = LoadObject<UStaticMesh>(nullptr, *FoliageTypePath);
    if (StaticMesh) {
      // Auto-create (or reuse) the FoliageType through the helper foliage.add_instances also
      // calls, so the two verbs cannot disagree about what this mesh's auto-type is called.
      // The helper always hands back the OBJECT path: the already-exists branch here used to
      // publish the bare package handle instead, so the same verb answered the same mesh with
      // two different strings and only one of them resolved
      // (B-add-instances-auto-foliage-type-name-mismatch).
      FString AutoResolvedPath;
      FoliageType = PinWrightResolveOrCreateAutoFoliageTypeForMesh(
          StaticMesh, FoliageTypePath, AutoResolvedPath);
      if (FoliageType) {
        FoliageTypePath = AutoResolvedPath;
        bAutoResolvedFoliageType = true;
        UE_LOG(LogPinWrightSubsystem, Display,
               TEXT("HandlePaintFoliage: Auto-created FoliageType from StaticMesh: %s"), *FoliageTypePath);
      }
    }
  }

  if (!FoliageType) {
    Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
        FString::Printf(TEXT("Foliage type asset not found: %s (also tried as StaticMesh)"),
                        *FoliageTypePath));
    return true;
  }

  AInstancedFoliageActor *IFA =
      GetOrCreateFoliageActorForWorldSafe(World, true);
  if (!IFA) {
    Ctx.SendError(TEXT("FOLIAGE_ACTOR_FAILED"), TEXT("Failed to get foliage actor"));
    return true;
  }

  // Resolved once instead of per instance: FoliageInfos holds each FFoliageInfo in a TUniqueObj,
  // so the pointer survives every later add, and the projection path needs a stable handle to
  // read the instance index and the instanced component off.
  FFoliageInfo *Info = IFA->FindInfo(FoliageType);
  if (!Info) {
    IFA->AddFoliageType(FoliageType);
    Info = IFA->FindInfo(FoliageType);
  }
  if (!Info) {
    Ctx.SendError(TEXT("FOLIAGE_ACTOR_FAILED"),
        FString::Printf(TEXT("Foliage type '%s' could not be registered on %s, so nothing was "
                             "painted."), *FoliageTypePath, *IFA->GetName()));
    return true;
  }

  // Only mesh foliage carries an instanced component, and the seat solve measures an instance's
  // footprint off that component. An actor foliage type has none - FFoliageInfo::
  // GetImplementationType is exactly this IsA test - so projection is refused BEFORE anything is
  // written rather than silently degrading into literal placement. The component itself cannot
  // be checked here: it is created lazily by the first AddInstance, so it is still null now.
  if (bProjectToGround && !FoliageType->IsA<UFoliageType_InstancedStaticMesh>()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"),
        FString::Printf(TEXT("Foliage type '%s' is not an instanced static mesh type (actor "
                             "foliage), so its instances have no footprint to seat. Pass "
                             "projectToGround:false to write the literal coordinates."),
                        *FoliageTypePath));
    return true;
  }

  // Rest ON the surface rather than bedded into it: engine painting puts the instance exactly at
  // the trace hit, and vegetation sunk a fraction of its height into the terrain is a different,
  // unrequested result.
  GroundPlacement::FGroundSeatConfig SeatConfig;
  SeatConfig.EmbedFraction = 0.0;
  SeatConfig.EmbedDepthCm = 0.0;

  // ---- Rotation, read off the type the caller named ----
  // The loop below used to write FRotator::ZeroRotator for every instance while the UFoliageType
  // it resolves - and, on the auto-create path above, the one it WRITES - carries AlignToNormal
  // and RandomYaw true straight out of UFoliageType::UFoliageType. Four plants on a 33.9-degree
  // slope all stood vertical and identically oriented, so the verb saved a type describing
  // placement it then refused to perform
  // (B-foliage-paint-ignores-align-to-normal-and-random-yaw). These four fields, and the order
  // they are applied in, are FPotentialInstance::PlaceInstance's non-procedural branch
  // (Runtime/Foliage/Private/InstancedFoliage.cpp:5525-5548) verbatim, so a painted instance is
  // oriented the way engine foliage painting orients it.
  const bool bTypeRandomYaw = FoliageType->RandomYaw != 0;
  const float TypeRandomPitchAngle = FoliageType->RandomPitchAngle;
  const bool bTypeAlignToNormal = FoliageType->AlignToNormal != 0;
  const float TypeAlignMaxAngle = FoliageType->AlignMaxAngle;
  // MEASURED: how many instances the ground normal was actually available for. Compared against
  // InstancesPlaced below, so a type flag that could not be honoured is reported rather than
  // dropped.
  int32 AlignedCount = 0;

  constexpr int32 MaxPlacedDetail = 32;
  TArray<TSharedPtr<FJsonValue>> PlacedArray;
  int32 InstancesPlaced = 0;
  int32 ProjectedCount = 0;

  // The per-instance readback, published on BOTH branches: the rotation actually written, read
  // back off FFoliageInfo::Instances after every write, rather than the rotation the type asked
  // for. Seat is null on the literal branch, where nothing was dropped and no ground was named.
  auto AppendPaintedInstanceRow =
      [&](int32 EntryIndex, const FVector &RequestedLocation, const FFoliageInstance &Written,
          const GroundPlacement::FGroundInstanceSeatResult *Seat) {
        if (PlacedArray.Num() >= MaxPlacedDetail) {
          return;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), EntryIndex);
        Row->SetNumberField(TEXT("x"), Written.Location.X);
        Row->SetNumberField(TEXT("y"), Written.Location.Y);
        Row->SetNumberField(TEXT("z"), Written.Location.Z);
        Row->SetNumberField(TEXT("pitch"), Written.Rotation.Pitch);
        Row->SetNumberField(TEXT("yaw"), Written.Rotation.Yaw);
        Row->SetNumberField(TEXT("roll"), Written.Rotation.Roll);
        // The engine's own record of whether the instance was tilted to a surface - set by
        // FFoliageInstance::AlignToNormal, never by this handler - so a caller can tell an
        // aligned instance on level ground from one that was never aligned at all.
        Row->SetBoolField(TEXT("alignedToNormal"),
            (Written.Flags & FOLIAGE_AlignToNormal) != 0);
        if (Seat) {
          // Signed drop from the requested Z. Zero means the request already sat on the surface,
          // and only this number can tell that apart from no projection having happened.
          Row->SetNumberField(TEXT("deltaZCm"), Written.Location.Z - RequestedLocation.Z);
          // WHAT this instance was seated on, in the same block and the same words the spatial
          // ground verbs publish for the same solve - it is literally the same writer. Without it
          // a projected batch says where every instance ended up but not what answered the probe,
          // so vegetation seated on a neighbouring scatter reads exactly like vegetation seated on
          // terrain. Gated on the same condition the ground verbs gate on: the block describes
          // supported columns, and with none it would describe nothing.
          if (Seat->Seat.Contact.SupportedColumns > 0) {
            Row->SetObjectField(TEXT("groundProvenance"),
                GroundPlacement::MakeProvenanceJson(Seat->Seat.Contact));
          }
        }
        PlacedArray.Add(MakeShared<FJsonValueObject>(Row));
      };

  for (int32 Index = 0; Index < Locations.Num(); ++Index) {
    const FVector &Location = Locations[Index];
    const int32 SourceIndex = LocationIndices[Index];

    FFoliageInstance Instance;
    Instance.Location = Location;
    // Yaw and pitch need no surface, so they are applied on BOTH branches and before the instance
    // is stored - AddInstance builds the render instance from GetInstanceWorldTransform(), so a
    // rotation set here reaches the component without a second write. FMath::FRand is the draw
    // engine painting itself uses; foliage.paint has no seed surface and inventing one was not
    // asked for, so yaws differ per instance and per call exactly as engine-painted ones do.
    Instance.Rotation = FRotator(FMath::FRand() * TypeRandomPitchAngle, 0.0f, 0.0f);
    if (bTypeRandomYaw) {
      Instance.Rotation.Yaw = FMath::FRand() * 360.0f;
    } else {
      // Not cosmetic: FOLIAGE_NoRandomYaw is what stops a later reapply from re-randomising a
      // deliberately fixed yaw.
      Instance.Flags |= FOLIAGE_NoRandomYaw;
    }
    // Still hardcoded, and named as such: the type's ScaleX/Y/Z range and ZOffset range are two
    // more fields this verb does not read. They are the scale/offset half of
    // B-foliage-paint-ignores-align-to-normal-and-random-yaw and are NOT fixed here.
    Instance.DrawScale3D = FVector3f(1.0f);
    Instance.ZOffset = 0.0f;

    const int32 InstanceIndex = Info->Instances.Num();
    Info->AddInstance(FoliageType, Instance, /*InBaseComponent*/ nullptr);
    if (!Info->Instances.IsValidIndex(InstanceIndex)) {
      NoteSkipped(SourceIndex, TEXT("the foliage actor did not accept the instance"));
      continue;
    }

    FVector Placed = Location;
    if (bProjectToGround) {
      // Fetched here, not before the loop: FFoliageStaticMesh creates its HISM lazily inside the
      // first AddInstance (PreAddInstances -> Initialize -> CreateNewComponent), so before that
      // call GetComponent() is null even for a perfectly valid mesh foliage type.
      UHierarchicalInstancedStaticMeshComponent *FoliageComponent = Info->GetComponent();
      if (!FoliageComponent) {
        Info->RemoveInstances(MakeArrayView(&InstanceIndex, 1), /*RebuildFoliageTree*/ false);
        NoteSkipped(SourceIndex,
            TEXT("the foliage type produced no instanced component, so there was no footprint to "
                 "measure against the ground"));
        continue;
      }

      // Dry run: the canonical solve produces the transform and nothing is written by it. Its
      // probe ignores the whole foliage actor, so an instance can never be seated onto a sibling
      // painted moments earlier.
      const GroundPlacement::FGroundInstanceSeatResult Seat = GroundPlacement::SeatInstance(
          World, FoliageComponent, InstanceIndex, Surface, SeatConfig, /*bApply*/ false);
      if (!Seat.ProposedTransform.IsSet()) {
        // Nothing accepted under this column, so nothing is painted here. Leaving the instance at
        // the requested Z is precisely the floating vegetation the projection exists to prevent,
        // so it is withdrawn - it is the last index, which RemoveInstances handles as a plain
        // truncation with no index shuffle.
        Info->RemoveInstances(MakeArrayView(&InstanceIndex, 1), /*RebuildFoliageTree*/ false);
        NoteSkipped(SourceIndex, Seat.Seat.Reason.IsEmpty()
            ? FString(TEXT("no accepted ground under this column"))
            : Seat.Seat.Reason);
        continue;
      }

      Placed = Seat.ProposedTransform->GetTranslation();
      // PreMove/PostMove is what keeps the three representations in step: PreMoveInstances pulls
      // the instance out of the location hash, and PostMoveInstances pushes the new transform
      // into the component and re-inserts the hash entry. Writing Instances[i].Location alone
      // would leave both stale, and writing through the component alone (which is what
      // SeatInstance's applying path does) would leave the foliage record stale.
      Info->PreMoveInstances(MakeArrayView(&InstanceIndex, 1));
      Info->Instances[InstanceIndex].Location = Placed;
      // AlignToNormal is the half that NEEDS the ground, and this is the only point in the verb
      // where a normal exists: FGroundContactReport::AverageNormal is the mean of the accepted
      // ground normals under this instance's own footprint - the same figure GroundPlacement
      // aligns actors to - taken from the seat solve that already ran rather than from a second
      // trace. Written INSIDE the bracket alongside the location, because PostUpdateInstances is
      // what pushes GetInstanceWorldTransform() - rotation included - into the component; a
      // rotation written after PostMoveInstances would leave the drawn instance vertical.
      if (bTypeAlignToNormal) {
        const FVector GroundNormal = Seat.Seat.Contact.AverageNormal.GetSafeNormal();
        if (Seat.Seat.Contact.SupportedColumns > 0 && !GroundNormal.IsNearlyZero()) {
          Info->Instances[InstanceIndex].AlignToNormal(GroundNormal, TypeAlignMaxAngle);
          ++AlignedCount;
        }
      }
      Info->PostMoveInstances(MakeArrayView(&InstanceIndex, 1), /*bFinished*/ true);
      ++ProjectedCount;

      AppendPaintedInstanceRow(SourceIndex, Location, Info->Instances[InstanceIndex], &Seat);
    } else {
      AppendPaintedInstanceRow(SourceIndex, Location, Info->Instances[InstanceIndex],
          /*Seat*/ nullptr);
    }
    ++InstancesPlaced;
  }

  IFA->Modify();

  // Unconditional, and on the projecting branch too. Painting has the same unrebuilt cluster tree
  // add_instances has - Info->AddInstance above goes through the same suppressed bracket - and
  // what has been saving the projecting branch is a SIDE EFFECT of PostMoveInstances, which
  // reaches HISM::UpdateInstanceTransform's own BuildTreeIfOutdated only because the editor never
  // takes that function's in-place branch (bAllowInPlaceUpdateForRotationOrScaleChange =
  // bIsGameWorld). That is an accident of an unrelated engine gate and must not stay load-bearing;
  // the unprojected branch never had it at all. See FoliageClusterTreeState.h.
  PinWrightFoliageClusterTree::FFoliageClusterTreeScope TreeScope;
  PinWrightFoliageClusterTree::RebuildFoliageClusterTreeAfterAdd(*Info);
  PinWrightFoliageClusterTree::NoteFoliageClusterTreeForInfo(TreeScope, *Info);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("foliageTypePath"), FoliageTypePath);
  Resp->SetNumberField(TEXT("instancesPlaced"), InstancesPlaced);
  // MEASURED off the rebuilt tree, not off the array instancesPlaced counted.
  PinWrightFoliageClusterTree::AddFoliageClusterTreeReport(Resp, TreeScope,
      TEXT("instancesPlaced"));

  // instancesPlaced is the editor-side LEDGER count, and on an opted-in type that is not what the
  // frame contains: foliage.DensityScale (driven by sg.FoliageQuality) makes
  // UHierarchicalInstancedStaticMeshComponent drop a random fraction of the instances from its
  // cluster tree, so a caller who paints 1000 and sees 400 drawn had no way to learn why. The two
  // numbers are published apart rather than one that means neither. Does not change the cvar.
  PinWrightDensityScalability::AddFoliageDensityScaleReport(Resp,
      PinWrightDensityScalability::MakeSingleTypeFoliageDensityScope(
          FoliageType->bEnableDensityScaling != 0, InstancesPlaced),
      TEXT("instancesPlaced"));

  // The deciding fact, published on BOTH branches. Without it a batch that never touched the
  // ground is indistinguishable from one that seated every instance, which is what let this verb
  // return a green instancesPlaced over buried and floating foliage.
  Resp->SetBoolField(TEXT("projected"), bProjectToGround);
  TArray<TSharedPtr<FJsonValue>> Warnings;
  if (bProjectToGround) {
    Resp->SetNumberField(TEXT("projectedCount"), ProjectedCount);
    Resp->SetStringField(TEXT("surfacePreset"),
        GroundPlacement::SurfacePresetToString(Surface.Preset));
  } else {
    Warnings.Add(MakeShared<FJsonValueString>(
        TEXT("NO GROUND PROJECTION was performed: every instance was written at the literal z "
             "supplied, with no trace, no surface projection, no normal alignment and no "
             "collision check. Nothing in this response says the instances are on the ground, "
             "because they were never measured against it - they are wherever the numbers put "
             "them. Supply `surface` (e.g. {\"preset\":\"landscape\"} or "
             "{\"preset\":\"any_solid\"}) to seat each instance on the ground instead.")));
  }

  // placed[] is published on BOTH branches: it is the only per-instance readback of the rotation
  // the verb actually wrote, and an unprojected batch has a rotation too - random yaw and random
  // pitch need no surface. deltaZCm and groundProvenance stay projected-only, because on the
  // literal branch there is no drop and nothing was measured to name.
  Resp->SetArrayField(TEXT("placed"), PlacedArray);
  if (PlacedArray.Num() < InstancesPlaced) {
    Resp->SetBoolField(TEXT("placedTruncated"), true);
  }

  // What the TYPE asked for, next to what was MEASURED as applied. alignedCount is the measured
  // half and is emitted only where the flag is on, so a 0 there is a real "asked for and not
  // done" rather than an absent feature reported as a zero.
  TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
  RotationObj->SetBoolField(TEXT("randomYaw"), bTypeRandomYaw);
  RotationObj->SetNumberField(TEXT("randomPitchAngleDeg"), TypeRandomPitchAngle);
  RotationObj->SetBoolField(TEXT("alignToNormal"), bTypeAlignToNormal);
  if (bTypeAlignToNormal) {
    RotationObj->SetNumberField(TEXT("alignMaxAngleDeg"), TypeAlignMaxAngle);
    RotationObj->SetNumberField(TEXT("alignedCount"), AlignedCount);
  }
  Resp->SetObjectField(TEXT("rotation"), RotationObj);

  // A type flag the verb could not honour is REPORTED, never dropped. Silently ignoring
  // AlignToNormal is the whole of B-foliage-paint-ignores-align-to-normal-and-random-yaw;
  // ignoring it with a reason is a legitimate result.
  if (bTypeAlignToNormal && AlignedCount < InstancesPlaced) {
    Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
        TEXT("alignToNormal is enabled on foliage type '%s' but was applied to %d of %d placed "
             "instance(s): %s. Random yaw and random pitch WERE applied - they need no surface - "
             "but nothing was tilted onto the ground it stands on."),
        *FoliageTypePath, AlignedCount, InstancesPlaced,
        bProjectToGround
            ? TEXT("no ground normal was measured under the rest")
            : TEXT("no `surface` was supplied, so no ground normal was measured anywhere"))));
  }
  if (Warnings.Num() > 0) {
    Resp->SetArrayField(TEXT("warnings"), Warnings);
  }

  // Always emitted, so instancesPlaced + skippedCount == entries supplied holds without the
  // caller having to guess whether an absent key means zero.
  Resp->SetNumberField(TEXT("skippedCount"), SkippedCount);
  if (SkippedArray.Num() > 0) {
    Resp->SetArrayField(TEXT("skipped"), SkippedArray);
    if (SkippedArray.Num() < SkippedCount) {
      Resp->SetBoolField(TEXT("skippedTruncated"), true);
    }
  }

  // Add verification data - MEASURED, not asserted (B-foliage-auto-type-no-disk-write).
  PinWrightWriteFoliageActorPresence(Resp, IFA);
  Resp->SetStringField(TEXT("foliageActorName"), IFA->GetName());
  // Only when this call actually stood the type up from a static-mesh path. A caller who named a
  // real UFoliageType gets no save fields, because this call neither created nor dirtied an asset.
  if (bAutoResolvedFoliageType) {
    PinWrightReportAutoFoliageTypePersistence(Resp, FoliageType);
  }

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- Helper: how many instances a foliage info actually draws ----
// FFoliageInfo has two representations of the same instance and they are written by different
// calls: FFoliageInfo::Instances is the editor-side bookkeeping array, while what the level draws
// lives on Info.Implementation (a HISM for mesh foliage, actors for actor foliage). This returns
// the second one - the exact quantity FFoliageInfo::CheckValid compares Instances.Num() against.
// Implementation->GetInstanceCount() rather than GetComponent()->GetInstanceCount() because
// GetComponent() is null for every non-static-mesh impl type, which would read as "draws nothing".
static int32 CountRenderedFoliageInstances(const FFoliageInfo &Info) {
  return Info.IsInitialized() ? Info.Implementation->GetInstanceCount() : 0;
}

// ---- Helper: remove every instance a foliage info holds, returning how many went ----
// Goes through the engine's removal API so the drawn instances go with the ledger. Emptying
// FFoliageInfo::Instances directly discards only the bookkeeping array: RemoveInstancesImpl
// (InstancedFoliage.cpp) is what also withdraws each instance from the component, the location
// hash, the component hash and the selection set. Divergence is not inert - the engine reconciles
// it in the ledger's favour from FFoliageStaticMesh::Reapply, which trims the component down to
// the array on the next PostEditUndo. foliage.paint above already withdraws single instances
// through this same call.
static int32 RemoveAllFoliageInstances(FFoliageInfo &Info) {
  const int32 Count = Info.Instances.Num();
  if (Count == 0) {
    return 0;
  }

  // RemoveInstancesImpl opens with check(IsInitialized()). Instances under an uninitialized info
  // is a state the engine repairs in AInstancedFoliageActor::PostLoad, so it should not reach an
  // RPC - but nothing is drawn for it either, which makes the array the whole state and emptying
  // it the complete removal rather than the half one this helper exists to prevent.
  if (!Info.IsInitialized()) {
    Info.Instances.Empty();
    return Count;
  }

  TArray<int32> AllIndices;
  AllIndices.Reserve(Count);
  for (int32 Index = 0; Index < Count; ++Index) {
    AllIndices.Add(Index);
  }
  Info.RemoveInstances(AllIndices, /*RebuildFoliageTree*/ true);
  return Count;
}

// ---- foliage.remove ----
REGISTER_RPC_HANDLER("foliage.remove", "foliage", "Remove foliage instances by type or all",
    RPC_PARAMS(
        RPC_PARAM_OPT("foliageTypePath", "path", "Path to foliage type to remove (omit with removeAll for all)"),
        RPC_PARAM_OPT("removeAll", "boolean", "Remove all foliage instances of all types")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("remove_foliage payload missing"));
    return true;
  }

  FString FoliageTypePath;
  Payload->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath);

  // Security: Validate path format if provided
  if (!FoliageTypePath.IsEmpty()) {
    FString SafePath = SanitizeProjectRelativePath(FoliageTypePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(TEXT("SECURITY_VIOLATION"),
          FString::Printf(TEXT("Invalid or unsafe foliage type path: %s"), *FoliageTypePath));
      return true;
    }
    FoliageTypePath = SafePath;
  }

  // Auto-resolve simple name
  if (!FoliageTypePath.IsEmpty() &&
      FPaths::GetPath(FoliageTypePath).IsEmpty()) {
    FoliageTypePath =
        FString::Printf(TEXT("/Game/Foliage/%s"), *FoliageTypePath);
  }

  bool bRemoveAll = false;
  Payload->TryGetBoolField(TEXT("removeAll"), bRemoveAll);

  // Honesty gates for ambiguous / edge inputs (E-foliage-remove-silent-edge-inputs).
  // Pure caller-input validation, grouped under a single scoped-mode gate: they only
  // apply when a specific type was named (not removeAll). They run before the
  // world/IFA lookup because LoadObject resolves the asset from disk and does not need
  // the editor world. When removeAll wins, a co-supplied path is intentionally ignored
  // (its interpretation is surfaced by the mode field below), so it is neither resolved
  // nor required here.
  UFoliageType *FoliageType = nullptr;
  if (!bRemoveAll) {
    // Under-specified call: neither a scope nor removeAll was given. Reject instead of
    // the former silent success:0 that hid the fact nothing was targeted.
    if (FoliageTypePath.IsEmpty()) {
      Ctx.SendError(TEXT("INVALID_ARGUMENT"),
          TEXT("specify foliageTypePath or set removeAll:true"));
      return true;
    }
    // Resolve the actual UFoliageType (not a bare existence check): a path that is
    // missing, or resolves to some other asset type, is a typo — not a scoped removal.
    // Reject with ASSET_NOT_FOUND so a bad path is not falsely confirmed as a removal,
    // and so an existing-but-wrong-type path is not the silent success:0 this ticket
    // set out to kill. The pointer is reused by the removal branch below (one lookup).
    FoliageType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath);
    if (!FoliageType) {
      Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
          FString::Printf(TEXT("Foliage type asset not found: %s"), *FoliageTypePath));
      return true;
    }
  }

  if (!GEditor || !GEditor->GetEditorWorldContext().World()) {
    Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
    return true;
  }

  UWorld *World = GEditor->GetEditorWorldContext().World();
  AInstancedFoliageActor *IFA =
      GetOrCreateFoliageActorForWorldSafe(World, false);
  if (!IFA) {
    Ctx.SendError(TEXT("FOLIAGE_ACTOR_NOT_FOUND"), TEXT("No foliage actor found"));
    return true;
  }

  int32 RemovedCount = 0;

  // Modify() before the write, not after it: the undo buffer has to capture the pre-removal
  // state. RemoveInstancesImpl calls IFA->Modify() itself as well, which is idempotent.
  if (bRemoveAll) {
    IFA->Modify();
    IFA->ForEachFoliageInfo([&](UFoliageType *Type, FFoliageInfo &Info) {
      RemovedCount += RemoveAllFoliageInstances(Info);
      return true;
    });
  } else if (FFoliageInfo *Info = IFA->FindInfo(FoliageType)) {
    // FoliageType was resolved above (non-null in this branch). A valid type with no
    // instances painted on this IFA honestly removes 0.
    IFA->Modify();
    RemovedCount = RemoveAllFoliageInstances(*Info);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetNumberField(TEXT("instancesRemoved"), RemovedCount);
  // Case 3 — echo which scope was actually applied so removeAll:true co-supplied with
  // a foliageTypePath is not a silent wholesale wipe: mode:"all" tells the caller the
  // named scope was ignored and every type was cleared; mode:"type" confirms a scoped
  // removal of only the named type.
  Resp->SetStringField(TEXT("mode"), bRemoveAll ? TEXT("all") : TEXT("type"));

  // Add verification data - MEASURED, not asserted (B-foliage-auto-type-no-disk-write).
  PinWrightWriteFoliageActorPresence(Resp, IFA);

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- foliage.get_instances ----
REGISTER_RPC_HANDLER("foliage.get_instances", "foliage", "Get foliage instances, optionally filtered by type. Returns instances[] plus count, and alongside them orphanedInstanceCount (0 on a healthy level): instances the level still holds under a foliage type that was deleted out from under it, which have no type path to report and are therefore excluded from instances[] and count. instances[]/count/orphanedInstanceCount are read off the editor-side FFoliageInfo::Instances bookkeeping array; renderedInstanceCount is what the level actually draws over the same scope, read off the foliage components. ledgerMatchesRendered is renderedInstanceCount == count + orphanedInstanceCount - false means the two representations have diverged and neither number alone describes the level. A foliageTypePath that does not resolve to a UFoliageType is REFUSED with ASSET_NOT_FOUND, never answered with an empty instances[]: an empty read and a misaddressed one used to be indistinguishable on the wire. expectedDrawnInstances is the third representation and the only one about the FRAME: the foliage.DensityScale cvar (driven by sg.FoliageQuality) culls a random fraction of an opted-in type's instances out of the HISM cluster tree without removing them from the component, so renderedInstanceCount can exceed what is drawn. densityScalingEnabled says whether anything in scope opts in, foliageDensityScaleCVar carries the measured cvar, and expectedDrawnInstances / effectiveDensityScale are OMITTED, never guessed, when the cvar is absent or an orphaned info makes the scope unmeasurable. Does not change the cvar. builtInstanceCount is the FOURTH representation and the one the viewport reads: the component's NumBuiltInstances, how many instances are in the built HISM cluster tree, with clusterTreeUpToDate carrying IsTreeFullyBuilt(). renderedInstanceCount and count are two arrays a single AddInstance writes together, so ledgerMatchesRendered CANNOT go false on a write that left the tree unbuilt - builtInstanceCount can, and clusterTreeWarning says so. It is cluster-tree MEMBERSHIP, a different layer from expectedDrawnInstances (the density cull applied to instances already in the tree); neither is a camera or frustum figure. Both cluster-tree fields are OMITTED, never zeroed, when anything in scope draws from something other than a HISM. This verb never rebuilds a stale tree it finds.",
    RPC_PARAMS(
        RPC_PARAM_OPT("foliageTypePath", "path", "Path to foliage type to filter by (omit for all). Must resolve to a UFoliageType - a missing path, or one naming some other asset type, is ASSET_NOT_FOUND rather than an empty result. The bare package handle is accepted as well as the Package.AssetName object path.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("get_foliage_instances payload missing"));
    return true;
  }

  FString FoliageTypePath;
  Payload->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath);

  // Security: Validate path format if provided
  if (!FoliageTypePath.IsEmpty()) {
    FString SafePath = SanitizeProjectRelativePath(FoliageTypePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(TEXT("SECURITY_VIOLATION"),
          FString::Printf(TEXT("Invalid or unsafe foliage type path: %s"), *FoliageTypePath));
      return true;
    }
    FoliageTypePath = SafePath;
  }

  // Auto-resolve simple name
  if (!FoliageTypePath.IsEmpty() &&
      FPaths::GetPath(FoliageTypePath).IsEmpty()) {
    FoliageTypePath =
        FString::Printf(TEXT("/Game/Foliage/%s"), *FoliageTypePath);
  }

  // Resolve the filter BEFORE the world lookup, the same order foliage.remove uses: a filter
  // that names nothing is a caller error whatever the level holds, and resolving it out of
  // world state would make the verdict depend on whether the map happens to own a foliage
  // actor. Resolution is LoadObject rather than the registry probe this branch used to gate
  // on, because a path that EXISTS but names something other than a UFoliageType produced the
  // same silent zero (B-add-instances-auto-foliage-type-name-mismatch); LoadObject also
  // accepts the bare package handle, which the registry probe normalises to
  // "Package.PackageStem" and misses.
  UFoliageType *FilterType = nullptr;
  if (!FoliageTypePath.IsEmpty()) {
    // LOAD_NoWarn: a filter that does not resolve is a reported outcome of this verb, not an
    // engine-level problem, and an automation host scores a stray load warning as a failure.
    FilterType = LoadObject<UFoliageType>(nullptr, *FoliageTypePath, nullptr, LOAD_NoWarn);
    if (!FilterType) {
      // A filter that does not resolve used to answer success:true with instances:[] - a false
      // negative dressed as a clean read, indistinguishable from a genuinely empty type, which
      // is what let a caller conclude its own scatter never happened over eight live instances.
      // Refuse instead. The hint names the shape problem specifically, because the failure is in
      // the ADDRESS, not the asset: ASSET_NOT_FOUND alone sends the reader hunting a missing
      // asset that is in fact loaded and drawing.
      FString PackageStem;
      FString ObjectName;
      const bool bHasObjectName = FoliageTypePath.Split(
          TEXT("."), &PackageStem, &ObjectName, ESearchCase::CaseSensitive,
          ESearchDir::FromEnd);
      const FString Hint =
          bHasObjectName
              ? FString(TEXT("Pass a UFoliageType asset path, or omit foliageTypePath to read "
                             "every type in the level."))
              : FString::Printf(
                    TEXT("That is a package handle, not an object path - address the asset "
                         "inside it as '%s.%s'."),
                    *FoliageTypePath, *FPaths::GetBaseFilename(FoliageTypePath));
      Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
          FString::Printf(
              TEXT("foliageTypePath did not resolve to a UFoliageType: %s. %s No instances were "
                   "read - this is NOT a report that the type has none."),
              *FoliageTypePath, *Hint));
      return true;
    }
  }

  if (!GEditor || !GEditor->GetEditorWorldContext().World()) {
    Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
    return true;
  }

  UWorld *World = GEditor->GetEditorWorldContext().World();
  AInstancedFoliageActor *IFA =
      GetOrCreateFoliageActorForWorldSafe(World, false);
  if (!IFA) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetArrayField(TEXT("instances"), TArray<TSharedPtr<FJsonValue>>());
    Ctx.SendSuccess(Resp);
    return true;
  }

  TArray<TSharedPtr<FJsonValue>> InstancesArray;
  // Instances left behind under a null (deleted) foliage type by the unfiltered branch below.
  int32 OrphanedInstanceCount = 0;
  // What the level DRAWS across the same scope. instances[]/count/orphanedInstanceCount all come
  // off FFoliageInfo::Instances, which is bookkeeping; a reader that only ever saw that array
  // cannot tell an empty level from one whose components are still full, and a mutator that
  // wrote one side and not the other would agree with this verb while disagreeing with the
  // viewport. Reported alongside so the two are comparable instead of interchangeable.
  int32 RenderedInstanceCount = 0;

  // And the layer BELOW renderedInstanceCount, which is the one the viewport reads. Both counts
  // above come off arrays a single AddInstance writes together, so ledgerMatchesRendered is
  // structurally incapable of going false on an add; a HISM draws from its cluster tree instead,
  // and an add that left the tree unbuilt is invisible to every field above this line.
  // builtInstanceCount is measured off the component's own NumBuiltInstances so it CAN disagree.
  PinWrightFoliageClusterTree::FFoliageClusterTreeScope ClusterTreeScope;

  // renderedInstanceCount counts the instances the level's foliage COMPONENTS hold, which is
  // still not the number the frame contains: foliage.DensityScale culls a fraction of an opted-in
  // component's instances out of the HISM cluster tree without removing them from
  // PerInstanceSMData. Accumulated per foliage type, because the opt-in
  // (UFoliageType::bEnableDensityScaling) is per type and an unfiltered read spans several.
  PinWrightDensityScalability::FFoliageDensityScope DensityScope;
  DensityScope.Reading = PinWrightDensityScalability::ReadDensityScaleCVar(
      PinWrightDensityScalability::FoliageDensityScaleCVarName);
  DensityScope.bMeasured = true;
  bool bSawScaledFoliage = false;
  bool bSawExemptFoliage = false;
  auto NoteDensityScopeForInfo = [&](const UFoliageType *Type, int32 ComponentInstances) {
    if (ComponentInstances <= 0) {
      return;
    }
    if (!Type) {
      // An orphaned info: the type that carried the opt-in is gone, so whether these instances
      // are culled is unknowable. Unmeasured, not assumed unscaled.
      DensityScope.bMeasured = false;
      return;
    }
    const bool bOptedIn = Type->bEnableDensityScaling != 0;
    float Scale = 1.0f;
    if (!PinWrightDensityScalability::ResolveFoliageDensityScale(
            DensityScope.Reading, bOptedIn, Scale)) {
      DensityScope.bMeasured = false;
      return;
    }
    if (bOptedIn) {
      bSawScaledFoliage = true;
      DensityScope.bAnyTypeOptedIn = true;
      DensityScope.EffectiveScale = Scale;
    } else {
      bSawExemptFoliage = true;
    }
    DensityScope.ExpectedDrawn += FMath::RoundToInt(ComponentInstances * Scale);
  };

  // One flat per-instance transform serializer shared by both read branches so they
  // stay byte-for-byte consistent: the filtered and unfiltered branches previously
  // drifted (the unfiltered one dropped rotation + scale), which this single
  // definition makes structurally impossible. Echoes the location, rotation, and
  // scale (DrawScale3D) that add_instances wrote, so a scatter-then-verify workflow
  // can confirm the transform it supplied. The wire schema is flat keys directly on
  // the instance object (not the nested {x,y,z} sub-objects of JsonBuilders), so a
  // small local helper is the right reuse unit here.
  auto FillInstanceTransform = [](const FFoliageInstance &Inst,
                                  const TSharedPtr<FJsonObject> &InstObj) {
    InstObj->SetNumberField(TEXT("x"), Inst.Location.X);
    InstObj->SetNumberField(TEXT("y"), Inst.Location.Y);
    InstObj->SetNumberField(TEXT("z"), Inst.Location.Z);
    InstObj->SetNumberField(TEXT("pitch"), Inst.Rotation.Pitch);
    InstObj->SetNumberField(TEXT("yaw"), Inst.Rotation.Yaw);
    InstObj->SetNumberField(TEXT("roll"), Inst.Rotation.Roll);
    InstObj->SetNumberField(TEXT("scaleX"), Inst.DrawScale3D.X);
    InstObj->SetNumberField(TEXT("scaleY"), Inst.DrawScale3D.Y);
    InstObj->SetNumberField(TEXT("scaleZ"), Inst.DrawScale3D.Z);
  };

  if (FilterType) {
    // Resolved above, before the world lookup - an unresolvable filter never reaches here, so a
    // zero from this branch is a real zero: the type exists and the level holds none of it.
    FFoliageInfo *Info = IFA->FindInfo(FilterType);
    if (Info) {
      const int32 DrawnForInfo = CountRenderedFoliageInstances(*Info);
      RenderedInstanceCount += DrawnForInfo;
      NoteDensityScopeForInfo(FilterType, DrawnForInfo);
      PinWrightFoliageClusterTree::NoteFoliageClusterTreeForInfo(ClusterTreeScope, *Info);
      for (const FFoliageInstance &Inst : Info->Instances) {
        TSharedPtr<FJsonObject> InstObj = MakeShared<FJsonObject>();
        FillInstanceTransform(Inst, InstObj);
        InstancesArray.Add(MakeShared<FJsonValueObject>(InstObj));
      }
    }
  } else {
    // DEFENSIVE, not a reproduced crash. ForEachFoliageInfo hands out AInstancedFoliageActor's
    // raw map key, and that key can legitimately be null: force-deleting a UFoliageType the
    // level still references rewrites the key to null in place (ObjectTools::ForceReplaceReferences
    // reaches it through AInstancedFoliageActor::Serialize, which serializes FoliageInfos for every
    // archive including reference collectors, and TSet rehashes afterwards), and a stale entry also
    // survives a map load whose type asset is gone. The engine treats this as a real state - it
    // ships AInstancedFoliageActor::CleanupDeletedFoliageType() and prunes null keys again in
    // PostLoad. In a stock editor that cleanup runs synchronously off AssetRegistry.OnAssetRemoved
    // (FFoliageEditModule::NotifyAssetRemoved) before any RPC can observe the gap, but that repair
    // belongs to the FoliageEdit editor module rather than to this plugin, so a read verb must not
    // stake a hard editor crash on it being loaded. Skip the orphan instead of dereferencing its
    // key - it has no type path to report - and count what was dropped so `count` is not read as
    // the whole story.
    IFA->ForEachFoliageInfo([&](UFoliageType *Type, FFoliageInfo &Info) {
      // Before the orphan skip: an orphaned info's instances are still drawn, so leaving them out
      // of the rendered total would report a divergence the level does not have.
      const int32 DrawnForInfo = CountRenderedFoliageInstances(Info);
      RenderedInstanceCount += DrawnForInfo;
      NoteDensityScopeForInfo(Type, DrawnForInfo);
      // Before the orphan skip for the same reason: an orphaned info's instances are still drawn
      // from a cluster tree, so its built count belongs in the total.
      PinWrightFoliageClusterTree::NoteFoliageClusterTreeForInfo(ClusterTreeScope, Info);
      if (!Type) {
        OrphanedInstanceCount += Info.Instances.Num();
        return true;
      }
      for (const FFoliageInstance &Inst : Info.Instances) {
        TSharedPtr<FJsonObject> InstObj = MakeShared<FJsonObject>();
        InstObj->SetStringField(TEXT("foliageType"), Type->GetPathName());
        FillInstanceTransform(Inst, InstObj);
        InstancesArray.Add(MakeShared<FJsonValueObject>(InstObj));
      }
      return true;
    });
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetArrayField(TEXT("instances"), InstancesArray);
  Resp->SetNumberField(TEXT("count"), InstancesArray.Num());
  // Always present (0 on a healthy level) so a caller can branch on it without probing for
  // an optional key: instances whose foliage type was deleted out from under the level are
  // not reportable by path and are excluded from instances[]/count.
  Resp->SetNumberField(TEXT("orphanedInstanceCount"), OrphanedInstanceCount);
  // The render side of the same scope, and the verdict on whether the two agree. count excludes
  // orphans and renderedInstanceCount includes them, so the accounting the caller would otherwise
  // have to reconstruct is stated here instead. The engine holds these equal as an invariant
  // (FFoliageInfo::CheckValid) but only asserts it under DO_FOLIAGE_CHECK, which ships at 0, so a
  // mismatch is a real divergence in the level rather than a reporting artefact.
  Resp->SetNumberField(TEXT("renderedInstanceCount"), RenderedInstanceCount);
  Resp->SetBoolField(TEXT("ledgerMatchesRendered"),
      RenderedInstanceCount == InstancesArray.Num() + OrphanedInstanceCount);

  // The third representation, and the one neither count above describes: how many of those
  // component-held instances the renderer will put in the frame once foliage.DensityScale is
  // applied. The cull acts on the component-held instances, so renderedInstanceCount is the
  // figure the warning is about and the one expectedDrawnInstances is measured against.
  DensityScope.LedgerCount = RenderedInstanceCount;
  DensityScope.bEffectiveScaleUniform = !(bSawScaledFoliage && bSawExemptFoliage);
  PinWrightDensityScalability::AddFoliageDensityScaleReport(Resp, DensityScope,
      TEXT("renderedInstanceCount"));

  // The fourth representation, and the only one a stale cluster tree can move. Deliberately read
  // AFTER the density block and measured against renderedInstanceCount, not against it: this is
  // cluster-tree MEMBERSHIP (is the instance in the built tree at all) while
  // expectedDrawnInstances is the density CULL applied to instances that are already in it.
  // Read-only - this verb never rebuilds a tree it finds stale, because a read verb that repaired
  // the level would hide the writer that broke it.
  PinWrightFoliageClusterTree::AddFoliageClusterTreeReport(Resp, ClusterTreeScope,
      TEXT("renderedInstanceCount"));

  // Add verification data - MEASURED, not asserted (B-foliage-auto-type-no-disk-write).
  PinWrightWriteFoliageActorPresence(Resp, IFA);

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- foliage.add_type ----
REGISTER_RPC_HANDLER("foliage.add_type", "foliage", "Create a new foliage type asset from a static mesh. The response reports densityScalingEnabled - the type's bEnableDensityScaling, which decides whether the foliage.DensityScale scalability cvar culls this type's instances at render time - because a caller had no way to tell which regime a type is in.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string",
            "BARE asset name for the new foliage type - not a path. A name carrying '/' (or any "
            "other character UObject naming rejects) is refused INVALID_ARGUMENT naming the "
            "engine's own reason; use savePath to choose the folder."),
        RPC_PARAM_REQ("meshPath", "path", "Path to the static mesh to use"),
        RPC_PARAM_OPT("savePath", "path",
            "Content folder for the created foliage type (default: /Game/Foliage). Must be under "
            "a mounted root; the effective folder is always echoed as `save_path`."),
        RPC_PARAM_OPT("density", "number", "Foliage density (default: 100)"),
        RPC_PARAM_OPT("minScale", "number", "Minimum scale (default: 1.0)"),
        RPC_PARAM_OPT("maxScale", "number", "Maximum scale (default: 1.0)"),
        RPC_PARAM_OPT("alignToNormal", "boolean", "Align to surface normal (default: true)"),
        RPC_PARAM_OPT("randomYaw", "boolean", "Random yaw rotation (default: true)"),
        RPC_PARAM_OPT("enableDensityScaling", "boolean",
            "Sets UFoliageType::bEnableDensityScaling, the type's opt-IN to the Foliage "
            "scalability group. With it true the renderer draws only foliage.DensityScale of this "
            "type's instances (0.4 at sg.FoliageQuality 1, 0 at 0) while the instances stay stored; "
            "with it false the type is exempt at every scalability level. Engine default: false. "
            "Enable it for detail meshes without collision, leave it off for anything gameplay "
            "depends on.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("add_foliage_type payload missing"));
    return true;
  }

  FString Name;
  if (!Payload->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("name required"));
    return true;
  }

  // Destination folder. Spelled and validated exactly as foliage.create_procedural spells it
  // (savePath -> SanitizeProjectRelativePath -> SECURITY_VIOLATION, effective folder echoed as
  // save_path), so the two type-creating verbs choose a folder the same way. It exists because a
  // hardcoded destination is WHY a caller put a path in `name` in the first place: refusing the
  // path without offering this one leaves the caller with nothing to try.
  FString PackagePath = TEXT("/Game/Foliage");
  FString RequestedSavePath;
  if (Payload->TryGetStringField(TEXT("savePath"), RequestedSavePath) &&
      !RequestedSavePath.IsEmpty()) {
    FString SafeSavePath = SanitizeProjectRelativePath(RequestedSavePath);
    if (SafeSavePath.IsEmpty()) {
      Ctx.SendError(TEXT("SECURITY_VIOLATION"),
          FString::Printf(TEXT("Invalid or unsafe savePath: %s"), *RequestedSavePath));
      return true;
    }
    SafeSavePath.RemoveFromEnd(TEXT("/"));
    PackagePath = SafeSavePath;
  }

  // COMPOSED AND CHECKED HERE, ahead of the meshPath resolution below, for two reasons. (1) The
  // path is what kills the process: `name` used to flow straight into a Printf onto the folder and
  // then into CreatePackage, and a name beginning with '/' produced "/Game/Foliage//Game/..." -
  // CreatePackage's Fatal, i.e. editor death (B-foliage-add-type-name-with-slash-kills-the-editor).
  // (2) The ordering is load-bearing for the regression test: it drives a slash-bearing name with
  // a meshPath that does not resolve, so on a build where this check is absent or moved below the
  // mesh load the call is refused for the WRONG reason and the test goes red - instead of
  // reaching CreatePackage and taking the test host down with it.
  FString AssetName = Name;
  FString FullPackagePath;
  FString PathError;
  if (!PinWrightComposeAssetPackagePath(PackagePath, AssetName, FullPackagePath, PathError)) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"),
        FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with savePath "
                             "(default /Game/Foliage)."), *PathError));
    return true;
  }

  FString MeshPath;
  if (!Payload->TryGetStringField(TEXT("meshPath"), MeshPath) ||
      MeshPath.IsEmpty() ||
      MeshPath.Equals(TEXT("undefined"), ESearchCase::IgnoreCase)) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("valid meshPath required"));
    return true;
  }

  double Density = 100.0;
  if (Payload->TryGetNumberField(TEXT("density"), Density)) {
    if (Density < 0.0) {
      Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("density must be non-negative"));
      return true;
    }
  }

  FFoliageScaleAndAlignInput ScaleAndAlign;
  FString ScaleError;
  if (!ReadFoliageScaleAndAlign(*Payload, ScaleAndAlign, ScaleError)) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), ScaleError);
    return true;
  }

  bool RandomYaw = true;
  Payload->TryGetBoolField(TEXT("randomYaw"), RandomYaw);

  // Defaults to the engine's own false (UFoliageType::UFoliageType, InstancedFoliage.cpp:669)
  // rather than to a value of this plugin's choosing, so an unrequested type behaves exactly as
  // one created in the editor does. Reaching the flag at all is new: the whole Foliage
  // scalability group was unreachable through this surface, so a caller could neither opt a
  // detail mesh in nor discover which regime a type was in.
  bool bEnableDensityScaling = false;
  Payload->TryGetBoolField(TEXT("enableDensityScaling"), bEnableDensityScaling);

  // Use Silent load to avoid engine warnings
  UStaticMesh *StaticMesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
  if (!StaticMesh) {
    // Try finding it if it's just a short name or missing extension
    if (FPackageName::IsValidLongPackageName(MeshPath)) {
      StaticMesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
    }

    if (!StaticMesh) {
      // Try assuming it's in /Game/ if not specified (naive fallback)
      if (!MeshPath.StartsWith(TEXT("/"))) {
        FString GamePath = FString::Printf(TEXT("/Game/%s"), *MeshPath);
        StaticMesh = LoadObject<UStaticMesh>(nullptr, *GamePath);
        if (!StaticMesh) {
          // Try with inferred name: /Game/Path/Values.Values
          FString BaseName = FPaths::GetBaseFilename(MeshPath);
          GamePath = FString::Printf(TEXT("/Game/%s.%s"), *MeshPath, *BaseName);
          StaticMesh = LoadObject<UStaticMesh>(nullptr, *GamePath);
        }
      }
    }
  }

  if (!StaticMesh) {
    if (!FPackageName::IsValidLongPackageName(MeshPath)) {
      Ctx.SendError(TEXT("INVALID_ARGUMENT"),
          FString::Printf(TEXT("Invalid package path: %s"), *MeshPath));
    } else {
      Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
          FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
    }
    return true;
  }

  // PackagePath / AssetName / FullPackagePath were composed and checked above, before the mesh
  // was touched. Nothing between here and this call may rebuild the path from raw arguments.
  UPackage *Package = CreatePackage(*FullPackagePath);
  if (!Package) {
    Ctx.SendError(TEXT("PACKAGE_CREATION_FAILED"), TEXT("Failed to create package"));
    return true;
  }

  UFoliageType_InstancedStaticMesh *FoliageType = nullptr;
  if (ResolveAsset(FullPackagePath).bExists) {
    FoliageType =
        LoadObject<UFoliageType_InstancedStaticMesh>(Package, *AssetName);
  }
  if (!FoliageType) {
    FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
  }
  if (!FoliageType) {
    Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create foliage type"));
    return true;
  }

  FoliageType->SetStaticMesh(StaticMesh);
  FoliageType->Density = static_cast<float>(Density);
  ApplyFoliageScaleAndAlign(FoliageType, ScaleAndAlign);
  FoliageType->RandomYaw = RandomYaw;
  FoliageType->ReapplyDensity = true;
  FoliageType->bEnableDensityScaling = bEnableDensityScaling ? 1 : 0;

  McpSafeAssetSave(FoliageType);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetBoolField(TEXT("created"), true);
  // exists_after is written at the tail instead, mirrored from the measurement AddAssetVerification
  // makes. It used to be the literal `true` published beside that helper's MEASURED existsAfter -
  // two fields spelling one name, agreeing with each other while both could disagree with the disk
  // (docs/rpc-design.md §1, B-foliage-auto-type-no-disk-write).
  Resp->SetStringField(TEXT("asset_path"), FoliageType->GetPathName());
  Resp->SetStringField(TEXT("used_mesh"), MeshPath);
  // Always echoed, supplied or defaulted - the destination used to be a literal a caller could
  // neither choose nor observe. Same field name foliage.create_procedural publishes.
  Resp->SetStringField(TEXT("save_path"), PackagePath);
  Resp->SetStringField(TEXT("method"), TEXT("native_asset_creation"));
  // Read back off the asset, not echoed from the request: the flag decides whether every
  // instance count reported for this type later means what it says, and an existing asset
  // reloaded above may already carry the opposite value.
  Resp->SetBoolField(TEXT("densityScalingEnabled"), FoliageType->bEnableDensityScaling != 0);

  // Add verification data
  AddAssetVerification(Resp, FoliageType);
  // McpSafeAssetSave above marks the package dirty and writes NOTHING, and this verb said nothing
  // about that. Same {saveRequested, markedForSave, saved, pendingFlush} triple the rest of the
  // plugin's mark-dirty creates publish, with `saved` measured rather than assumed.
  AddMarkDirtySaveReport(Resp, FoliageType, /*bSaveRequested=*/true);
  // The snake_case alias existing callers read, now MIRRORED from the measured existsAfter above
  // instead of being a second, contradicting literal.
  bool bMeasuredExistsAfter = false;
  Resp->TryGetBoolField(TEXT("existsAfter"), bMeasuredExistsAfter);
  Resp->SetBoolField(TEXT("exists_after"), bMeasuredExistsAfter);

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- foliage.add_instances ----
REGISTER_RPC_HANDLER("foliage.add_instances", "foliage", "Add foliage instances with full transform support. Entries the parser cannot use are reported in skipped[] ({index, reason}, capped) with the true total in skippedCount - they are never dropped silently. instances_count is the editor-side LEDGER count; expectedDrawnInstances is how many of them the renderer is expected to draw once the foliage.DensityScale cvar (driven by sg.FoliageQuality) culls an opted-in type. densityScalingEnabled, foliageDensityScaleCVar and cvarWarning carry the type's opt-in, the measured cvar and the remedy; the effective fields are OMITTED, never guessed, when the cvar is absent. Does not change the cvar. The verb REBUILDS the HISM cluster tree after the batch, which the engine's add path suppresses and does not restore, and then MEASURES the result: builtInstanceCount is the component's own NumBuiltInstances - the instances actually in the built tree, which is what a HISM draws from - and clusterTreeUpToDate is its IsTreeFullyBuilt(). Both are OMITTED, never zeroed, when the type's foliage has no cluster tree to read; clusterTreeWarning fires when the tree holds fewer instances than were stored. builtInstanceCount is cluster-tree MEMBERSHIP and expectedDrawnInstances is the density CULL applied to instances already in the tree - different layers, do not compare them to each other.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("foliageTypePath", "path", "Path to foliage type or static mesh asset", "foliageType"),
        RPC_PARAM_OPT("transforms", "array", "Array of {location, rotation, scale} transforms. location is REQUIRED per entry (object {x,y,z} or array [x,y,z]); an entry without one places nothing and is reported in the response's skipped[] array."),
        RPC_PARAM_OPT("locations", "array", "Array of {x,y,z} positions (legacy, default rotation/scale). Used only when transforms yields nothing; non-object entries are reported in skipped[]. If transforms wins, every entry here is dropped - reported in skipped[] and totalled in ignoredLocationCount.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("add_foliage_instances payload missing"));
    return true;
  }

  FString FoliageTypePath;
  if (!Payload->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath)) {
    Payload->TryGetStringField(TEXT("foliageType"), FoliageTypePath);
  }
  if (FoliageTypePath.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("foliageType or foliageTypePath required"));
    return true;
  }

  // Security: Validate path format
  FString SafePath = SanitizeProjectRelativePath(FoliageTypePath);
  if (SafePath.IsEmpty()) {
    Ctx.SendError(TEXT("SECURITY_VIOLATION"),
        FString::Printf(TEXT("Invalid or unsafe foliage type path: %s"), *FoliageTypePath));
    return true;
  }
  FoliageTypePath = SafePath;

  // Auto-resolve simple name
  if (!FoliageTypePath.IsEmpty() &&
      FPaths::GetPath(FoliageTypePath).IsEmpty()) {
    FoliageTypePath =
        FString::Printf(TEXT("/Game/Foliage/%s"), *FoliageTypePath);
  }

  // Parse transforms with full location, rotation, and scale support
  struct FFoliageTransformData {
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Scale = FVector::OneVector;
  };
  TArray<FFoliageTransformData> ParsedTransforms;

  // Partial-success reporting: an entry the parser cannot use is reported with its
  // index and a reason instead of vanishing. Same skipped/skippedCount/skippedTruncated
  // shape actor.spawn_batch returns, and the same count/total/truncated capping
  // vocabulary the bounded list handlers use, so a large bad batch cannot blow the
  // 10,000-char response budget. Reasons name their source array because the legacy
  // `locations` fallback shares this index space.
  constexpr int32 MaxSkippedDetail = 32;
  TArray<TSharedPtr<FJsonValue>> SkippedArray;
  int32 SkippedCount = 0;
  auto NoteSkipped = [&](int32 EntryIndex, const TCHAR *Reason) {
    ++SkippedCount;
    if (SkippedArray.Num() < MaxSkippedDetail) {
      TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
      SkipObj->SetNumberField(TEXT("index"), EntryIndex);
      SkipObj->SetStringField(TEXT("reason"), Reason);
      SkippedArray.Add(MakeShared<FJsonValueObject>(SkipObj));
    }
  };

  const TArray<TSharedPtr<FJsonValue>> *Transforms = nullptr;
  if (Payload->TryGetArrayField(TEXT("transforms"), Transforms) && Transforms) {
    for (int32 EntryIndex = 0; EntryIndex < Transforms->Num(); ++EntryIndex) {
      const TSharedPtr<FJsonValue> &V = (*Transforms)[EntryIndex];
      if (!V.IsValid() || V->Type != EJson::Object) {
        NoteSkipped(EntryIndex, TEXT("transforms[] entry is not an object"));
        continue;
      }
      const TSharedPtr<FJsonObject> *TObj = nullptr;
      if (!V->TryGetObject(TObj) || !TObj) {
        NoteSkipped(EntryIndex, TEXT("transforms[] entry is not an object"));
        continue;
      }

      FFoliageTransformData TransformData;

      // Parse location (object or array format)
      const TSharedPtr<FJsonObject> *LocObj = nullptr;
      if ((*TObj)->TryGetObjectField(TEXT("location"), LocObj) && LocObj) {
        (*LocObj)->TryGetNumberField(TEXT("x"), TransformData.Location.X);
        (*LocObj)->TryGetNumberField(TEXT("y"), TransformData.Location.Y);
        (*LocObj)->TryGetNumberField(TEXT("z"), TransformData.Location.Z);
      } else {
        // Accept location as array [x,y,z]
        const TArray<TSharedPtr<FJsonValue>> *LocArr = nullptr;
        if ((*TObj)->TryGetArrayField(TEXT("location"), LocArr) && LocArr &&
            LocArr->Num() >= 3) {
          TransformData.Location.X = (*LocArr)[0]->AsNumber();
          TransformData.Location.Y = (*LocArr)[1]->AsNumber();
          TransformData.Location.Z = (*LocArr)[2]->AsNumber();
        } else {
          NoteSkipped(EntryIndex, TEXT("transforms[] entry has no valid location; supply an object {x,y,z} or an array [x,y,z]"));
          continue; // Skip transforms without valid location
        }
      }

      // Parse rotation if provided (object format)
      const TSharedPtr<FJsonObject> *RotObj = nullptr;
      if ((*TObj)->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj) {
        double Pitch = 0, Yaw = 0, Roll = 0;
        (*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
        (*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
        (*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
        TransformData.Rotation = FRotator(Pitch, Yaw, Roll);
      } else {
        // Accept rotation as array [pitch, yaw, roll]
        const TArray<TSharedPtr<FJsonValue>> *RotArr = nullptr;
        if ((*TObj)->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr &&
            RotArr->Num() >= 3) {
          TransformData.Rotation.Pitch = (*RotArr)[0]->AsNumber();
          TransformData.Rotation.Yaw = (*RotArr)[1]->AsNumber();
          TransformData.Rotation.Roll = (*RotArr)[2]->AsNumber();
        }
      }

      // Parse scale if provided (object, array, or uniform scalar)
      const TSharedPtr<FJsonObject> *ScaleObj = nullptr;
      if ((*TObj)->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj) {
        (*ScaleObj)->TryGetNumberField(TEXT("x"), TransformData.Scale.X);
        (*ScaleObj)->TryGetNumberField(TEXT("y"), TransformData.Scale.Y);
        (*ScaleObj)->TryGetNumberField(TEXT("z"), TransformData.Scale.Z);
      } else {
        const TArray<TSharedPtr<FJsonValue>> *ScaleArr = nullptr;
        if ((*TObj)->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr &&
            ScaleArr->Num() >= 3) {
          TransformData.Scale.X = (*ScaleArr)[0]->AsNumber();
          TransformData.Scale.Y = (*ScaleArr)[1]->AsNumber();
          TransformData.Scale.Z = (*ScaleArr)[2]->AsNumber();
        } else {
          // Check for uniformScale scalar
          double UniformScale = 1.0;
          if ((*TObj)->TryGetNumberField(TEXT("uniformScale"), UniformScale)) {
            TransformData.Scale = FVector(UniformScale);
          }
        }
      }

      ParsedTransforms.Add(TransformData);
    }
  }

  // Positions handed over in `locations` that the transforms-wins precedence rule
  // discards. Stays 0 on every other path, and the response fields it gates are then
  // omitted entirely, so responses that never hit the collision are byte-identical.
  int32 IgnoredLocationCount = 0;

  if (ParsedTransforms.Num() == 0) {
    // Fallback to 'locations' if provided (legacy support, default rotation/scale)
    const TArray<TSharedPtr<FJsonValue>> *LocationsArray = nullptr;
    if (Payload->TryGetArrayField(TEXT("locations"), LocationsArray) &&
        LocationsArray) {
      for (int32 LocIndex = 0; LocIndex < LocationsArray->Num(); ++LocIndex) {
        const TSharedPtr<FJsonValue> &Val = (*LocationsArray)[LocIndex];
        const TSharedPtr<FJsonObject> *Obj = nullptr;
        if (!Val.IsValid() || Val->Type != EJson::Object ||
            !Val->TryGetObject(Obj) || !Obj) {
          NoteSkipped(LocIndex, TEXT("locations[] entry is not an object"));
          continue;
        }
        FFoliageTransformData TransformData;
        (*Obj)->TryGetNumberField(TEXT("x"), TransformData.Location.X);
        (*Obj)->TryGetNumberField(TEXT("y"), TransformData.Location.Y);
        (*Obj)->TryGetNumberField(TEXT("z"), TransformData.Location.Z);
        ParsedTransforms.Add(TransformData);
      }
    }
  } else {
    // `transforms` yielded instances, so the documented precedence drops every
    // `locations` entry. Precedence is unchanged - what changes is that the drop is
    // now visible: a caller who supplied both used to get no field, no count and no
    // warning back, which is the silent-elision failure mode the skipped[] machinery
    // exists to close. Routing these through the same NoteSkipped seam the parser
    // uses keeps one reporting shape instead of two (same 32-entry detail cap, same
    // skippedTruncated flag, same index space - reasons already name their source
    // array) and restores the documented identity instances_count + skippedCount ==
    // entries sent, which the collision case silently violated.
    const TArray<TSharedPtr<FJsonValue>> *IgnoredLocations = nullptr;
    if (Payload->TryGetArrayField(TEXT("locations"), IgnoredLocations) &&
        IgnoredLocations) {
      IgnoredLocationCount = IgnoredLocations->Num();
      for (int32 LocIndex = 0; LocIndex < IgnoredLocationCount; ++LocIndex) {
        NoteSkipped(LocIndex,
            TEXT("locations[] entry ignored because transforms was also supplied - transforms takes precedence"));
      }
    }
  }

  if (!GEditor || !GEditor->GetEditorWorldContext().World()) {
    Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
    return true;
  }

  UWorld *World = GEditor->GetEditorWorldContext().World();

  // Try to load as FoliageType first
  UFoliageType *FoliageType = Cast<UFoliageType>(
      StaticLoadObject(UFoliageType::StaticClass(), nullptr, *FoliageTypePath,
                       nullptr, LOAD_NoWarn));

  // If not a FoliageType, try loading as StaticMesh and auto-create FoliageType
  // Gates the persistence report at the tail: a caller that passed a real UFoliageType path had no
  // asset created or dirtied on its behalf, so it gets no save fields at all rather than fields
  // about work this call did not do.
  bool bAutoResolvedFoliageType = false;
  if (!FoliageType) {
    UStaticMesh *StaticMesh = LoadObject<UStaticMesh>(nullptr, *FoliageTypePath);
    if (StaticMesh) {
      // Auto-create (or reuse) the FoliageType through the helper foliage.paint also calls,
      // so the two verbs cannot disagree about what this mesh's auto-type is called, and the
      // echoed foliageTypePath below is an object path that round-trips
      // (B-add-instances-auto-foliage-type-name-mismatch). It also stops re-running this verb
      // from re-constructing the type object in place on every call.
      FString AutoResolvedPath;
      FoliageType = PinWrightResolveOrCreateAutoFoliageTypeForMesh(
          StaticMesh, FoliageTypePath, AutoResolvedPath);
      if (FoliageType) {
        FoliageTypePath = AutoResolvedPath;
        bAutoResolvedFoliageType = true;
        UE_LOG(LogPinWrightSubsystem, Display,
               TEXT("HandleAddFoliageInstances: Auto-created FoliageType from StaticMesh: %s"), *FoliageTypePath);
      }
    }
  }

  if (!FoliageType) {
    Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
        FString::Printf(TEXT("Foliage type asset not found: %s (also tried as StaticMesh)"),
                        *FoliageTypePath));
    return true;
  }

  AInstancedFoliageActor *IFA =
      GetOrCreateFoliageActorForWorldSafe(World, true);
  if (!IFA) {
    Ctx.SendError(TEXT("FOLIAGE_ACTOR_FAILED"), TEXT("Failed to get foliage actor"));
    return true;
  }

  int32 Added = 0;
  for (const FFoliageTransformData &TransformData : ParsedTransforms) {
    FFoliageInstance Instance;
    Instance.Location = TransformData.Location;
    Instance.Rotation = TransformData.Rotation;
    Instance.DrawScale3D = FVector3f(TransformData.Scale);

    if (FFoliageInfo *Info = IFA->FindInfo(FoliageType)) {
      Info->AddInstance(FoliageType, Instance, nullptr);
    } else {
      IFA->AddFoliageType(FoliageType);
      if (FFoliageInfo *NewInfo = IFA->FindInfo(FoliageType)) {
        NewInfo->AddInstance(FoliageType, Instance, nullptr);
      }
    }
    ++Added;
  }
  IFA->Modify();

  // The rebuild the engine's add path hands to its caller. Every AddInstance above ran inside
  // FFoliageInfo::AddInstancesImpl's BeginUpdate/EndUpdate bracket, which holds
  // bAutoRebuildTreeOnInstanceChanges false for the whole batch and restores it WITHOUT
  // rebuilding, so without this the instances are stored, reported, and drawn by nothing: a
  // fresh info exits with NumBuiltInstances == 0 and a HISM draws from the cluster tree, not
  // from PerInstanceSMData. See FoliageClusterTreeState.h for the full derivation and for why
  // this is the engine's contract rather than an engine bug.
  PinWrightFoliageClusterTree::FFoliageClusterTreeScope TreeScope;
  if (FFoliageInfo *AddedInfo = IFA->FindInfo(FoliageType)) {
    PinWrightFoliageClusterTree::RebuildFoliageClusterTreeAfterAdd(*AddedInfo);
    PinWrightFoliageClusterTree::NoteFoliageClusterTreeForInfo(TreeScope, *AddedInfo);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetNumberField(TEXT("instances_count"), Added);
  // MEASURED after the rebuild above, off the component's own NumBuiltInstances - so the field
  // that says the scatter is on screen is read from the structure that puts it there, not from
  // the array instances_count already counted. Omitted, never zeroed, when this type's foliage
  // has no cluster tree to read.
  PinWrightFoliageClusterTree::AddFoliageClusterTreeReport(Resp, TreeScope,
      TEXT("instances_count"));
  // Same divergence foliage.paint publishes: instances_count is the editor-side ledger, and on a
  // type that opts in to density scaling the renderer draws only a fraction of it.
  PinWrightDensityScalability::AddFoliageDensityScaleReport(Resp,
      PinWrightDensityScalability::MakeSingleTypeFoliageDensityScope(
          FoliageType->bEnableDensityScaling != 0, Added),
      TEXT("instances_count"));
  // skippedCount is always present (0 on a clean batch) so a caller can branch on it
  // without probing for an optional key; the detail array and its truncation flag
  // appear only when something was actually dropped.
  Resp->SetNumberField(TEXT("skippedCount"), SkippedCount);
  if (SkippedCount > 0) {
    Resp->SetArrayField(TEXT("skipped"), SkippedArray);
    Resp->SetBoolField(TEXT("skippedTruncated"), SkippedArray.Num() < SkippedCount);
  }
  // The per-entry rows above can be capped away by an already-full skipped[] detail
  // budget, so the exact drop total gets its own uncapped field, and the reason is
  // stated once in prose rather than repeated across N rows a reader may never see.
  // Both keys are absent unless the collision actually happened.
  if (IgnoredLocationCount > 0) {
    Resp->SetNumberField(TEXT("ignoredLocationCount"), IgnoredLocationCount);
    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
        TEXT("`locations` was ignored because `transforms` was also supplied - `transforms` takes precedence. %d position(s) were dropped. Pass only one of the two."),
        IgnoredLocationCount)));
    Resp->SetArrayField(TEXT("warnings"), Warnings);
  }

  // Add verification data - MEASURED, not asserted (B-foliage-auto-type-no-disk-write).
  PinWrightWriteFoliageActorPresence(Resp, IFA);
  Resp->SetStringField(TEXT("foliageTypePath"), FoliageTypePath);
  // Only when this call actually stood the type up from a static-mesh path. A caller who named a
  // real UFoliageType gets no save fields, because this call neither created nor dirtied an asset.
  if (bAutoResolvedFoliageType) {
    PinWrightReportAutoFoliageTypePersistence(Resp, FoliageType);
  }

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- foliage.create_procedural ----
REGISTER_RPC_HANDLER("foliage.create_procedural", "foliage", "Create a procedural foliage volume with spawner",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the procedural foliage spawner"),
        RPC_PARAM_REQ("bounds", "object", "Volume bounds with location and size"),
        RPC_PARAM_REQ("foliageTypes", "array",
            "Array of per-species configs. PAINT-SIDE keys: `meshPath` (required), `density` "
            "(instances per 1000x1000 uu, default 10), `minScale` / `maxScale` (one uniform "
            "interval across X/Y/Z, default 1) and `alignToNormal` (default true). "
            "SIMULATION-SIDE keys, all optional and all left at the engine default when absent: "
            "`initialSeedDensity` (seeds along 10 m, IMPLICITLY SQUARED over 10 m x 10 m - this "
            "is the number the tile simulation seeds from, NOT `density`; when you omit it the "
            "verb derives sqrt(density) so the two units line up, and the response names which "
            "of the two it used), `collisionRadius`, `shadeRadius`, `numSteps`, `seedsPerStep`, "
            "`averageSpreadDistance`, `maxAge`, `maxInitialAge`, `overlapPriority`, "
            "`canGrowInShade`, `spawnsInShade`, `randomPitchAngle`, and the interval-valued "
            "`proceduralScale` / `height` / `groundSlopeAngle`, each written {min, max} or "
            "[min, max]. FOUR INTERACTIONS THAT MAKE A CORRECT-LOOKING INPUT DO NOTHING. (1) "
            "`proceduralScale` is what the procedural placement path reads (`GetScaleForAge`); "
            "`minScale`/`maxScale` drive the PAINTING path only. (2) `maxInitialAge` defaults to "
            "0, at which every seed starts at age 0 and `proceduralScale` collapses to "
            "numSteps+1 discrete sizes - so when you supply a `proceduralScale` range and no "
            "`maxInitialAge` the verb raises it to `maxAge` and reports "
            "max_initial_age_source:'raised_for_procedural_scale'; pass `maxInitialAge` "
            "explicitly (0 included) to override that. (3) `overlapPriority` MUST be ordered the "
            "same way as effective radius max(collisionRadius, shadeRadius) - the loser of an "
            "overlap is the lower priority, so a small species at equal-or-higher priority wipes "
            "a large one (observed: trees 46 -> 0 placed). The verb compares your entries and "
            "adds a warnings[] line if the two orders disagree. (4) `spawnsInShade` is read as "
            "`canGrowInShade && spawnsInShade`, so it does nothing on its own. `height` and "
            "`groundSlopeAngle` are placement FILTERS applied after the terrain-free tile "
            "simulation, not elevation niches: they thin an already-simulated set rather than "
            "stratifying it. Unread keys you pass are echoed in `ignoredFields`."),
        RPC_PARAM_OPT("seed", "number", "Random seed (default: 12345)"),
        RPC_PARAM_OPT("tileSize", "number",
            "Simulation tile edge length in cm (default: 1000). The engine's own default is "
            "10000 (100 m); this verb's 1000 is kept for compatibility with existing callers, "
            "and ten unique 10 m tiles repeat visibly across a large volume - raise it toward "
            "the volume's own extent for a big scatter."),
        RPC_PARAM_OPT("numUniqueTiles", "number", "Number of unique tiles the simulation combines (default: 10)"),
        RPC_PARAM_OPT("tileOverlap", "number",
            "Overlap in cm between neighbouring simulation tiles (default: 0). Non-zero lets "
            "instances near a tile edge compete with the neighbouring tile's, which softens the "
            "seam a tiled scatter leaves. Was hardcoded to 0 and echoed nowhere; the effective "
            "value is now always reported as `tile_overlap`."),
        RPC_PARAM_OPT("savePath", "path",
            "Content folder for the generated spawner and `_FT_<n>` types (default: "
            "/Game/ProceduralFoliage). Must be under a mounted root; the effective folder is "
            "always echoed as `save_path`.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("create_procedural_foliage payload missing"));
    return true;
  }

  FString Name;
  if (!Payload->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("name required"));
    return true;
  }

  const TSharedPtr<FJsonObject> *BoundsObj = nullptr;
  if (!Payload->TryGetObjectField(TEXT("bounds"), BoundsObj) || !BoundsObj) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bounds required"));
    return true;
  }

  FVector Location(0, 0, 0);
  FVector Size(1000, 1000, 1000);

  const TSharedPtr<FJsonObject> *LocObj = nullptr;
  if ((*BoundsObj)->TryGetObjectField(TEXT("location"), LocObj) && LocObj) {
    (*LocObj)->TryGetNumberField(TEXT("x"), Location.X);
    (*LocObj)->TryGetNumberField(TEXT("y"), Location.Y);
    (*LocObj)->TryGetNumberField(TEXT("z"), Location.Z);
  }

  const TSharedPtr<FJsonObject> *SizeObj = nullptr;
  if ((*BoundsObj)->TryGetObjectField(TEXT("size"), SizeObj) && SizeObj) {
    (*SizeObj)->TryGetNumberField(TEXT("x"), Size.X);
    (*SizeObj)->TryGetNumberField(TEXT("y"), Size.Y);
    (*SizeObj)->TryGetNumberField(TEXT("z"), Size.Z);
  }
  // If size is array
  const TArray<TSharedPtr<FJsonValue>> *SizeArr = nullptr;
  if ((*BoundsObj)->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr &&
      SizeArr->Num() >= 3) {
    Size.X = (*SizeArr)[0]->AsNumber();
    Size.Y = (*SizeArr)[1]->AsNumber();
    Size.Z = (*SizeArr)[2]->AsNumber();
  }

  const TArray<TSharedPtr<FJsonValue>> *FoliageTypesArr = nullptr;
  if (!Payload->TryGetArrayField(TEXT("foliageTypes"), FoliageTypesArr)) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("foliageTypes array required"));
    return true;
  }

  int32 Seed = 12345;
  Payload->TryGetNumberField(TEXT("seed"), Seed);

  // Tiling controls. Both were hardcoded and echoed nowhere, so the grid the simulation
  // actually ran on was neither settable nor observable: every call simulated ten 10 m
  // tiles and repeated that set across `bounds` (visible tiling on a large volume, a
  // single tile - i.e. no variation at all - on one under 10 m). They are inputs now, and
  // the effective values are always reported below whether supplied or defaulted.
  double TileSize = 1000.0;
  if (Payload->TryGetNumberField(TEXT("tileSize"), TileSize) && TileSize <= 0.0) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("tileSize must be positive"));
    return true;
  }
  int32 NumUniqueTiles = 10;
  if (Payload->TryGetNumberField(TEXT("numUniqueTiles"), NumUniqueTiles) &&
      NumUniqueTiles < 1) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("numUniqueTiles must be at least 1"));
    return true;
  }
  // Third member of the same family: UProceduralFoliageComponent::TileOverlap was hardcoded
  // to 0 and, unlike the pair above, was not even echoed - so a caller could not observe it,
  // let alone set it. Both halves are fixed here.
  double TileOverlap = 0.0;
  if (Payload->TryGetNumberField(TEXT("tileOverlap"), TileOverlap) && TileOverlap < 0.0) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("tileOverlap cannot be negative"));
    return true;
  }

  if (!GEditor) {
    Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
    return true;
  }

  // Create Spawner Asset. The output folder used to be a hardcoded constant, so every call
  // wrote into one shared folder with names derived from `name`; it is a parameter now, and
  // the effective folder is echoed as `save_path`.
  FString PackagePath = TEXT("/Game/ProceduralFoliage");
  FString RequestedSavePath;
  if (Payload->TryGetStringField(TEXT("savePath"), RequestedSavePath) &&
      !RequestedSavePath.IsEmpty()) {
    FString SafeSavePath = SanitizeProjectRelativePath(RequestedSavePath);
    if (SafeSavePath.IsEmpty()) {
      Ctx.SendError(TEXT("SECURITY_VIOLATION"),
          FString::Printf(TEXT("Invalid or unsafe savePath: %s"), *RequestedSavePath));
      return true;
    }
    SafeSavePath.RemoveFromEnd(TEXT("/"));
    PackagePath = SafeSavePath;
  }
  // savePath was sanitized above; `name` was not, and it reaches the identical concatenation.
  // "/Game/X/Y" here composes "/Game/ProceduralFoliage//Game/X/Y_Spawner" and hits the same
  // CreatePackage Fatal that killed an editor through foliage.add_type - the second site named on
  // B-foliage-add-type-name-with-slash-kills-the-editor, fixed in the same change rather than
  // left as the one remaining way to end the process.
  FString AssetName = Name + TEXT("_Spawner");
  FString FullPackagePath;
  FString PathError;
  if (!PinWrightComposeAssetPackagePath(PackagePath, AssetName, FullPackagePath, PathError)) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"),
        FString::Printf(TEXT("%s Pass a bare name and choose the folder with savePath "
                             "(default /Game/ProceduralFoliage)."), *PathError));
    return true;
  }

  UPackage *Package = CreatePackage(*FullPackagePath);
  UProceduralFoliageSpawner *Spawner = NewObject<UProceduralFoliageSpawner>(
      Package, FName(*AssetName), RF_Public | RF_Standalone);
  if (!Spawner) {
    Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create spawner asset"));
    return true;
  }

  Spawner->TileSize = static_cast<float>(TileSize);
  Spawner->NumUniqueTiles = NumUniqueTiles;
  Spawner->RandomSeed = Seed;

  // Add foliage types to spawner. Every entry that contributes no foliage type is
  // reported with its index and a reason (same skipped/skippedCount/skippedTruncated
  // shape actor.spawn_batch and foliage.add_instances use) instead of vanishing behind
  // a foliage_types_count that only ever counted successes. Per-entry keys this verb
  // does NOT read are echoed back in ignoredFields so a caller who set them learns they
  // had no effect.
  int32 TypeIndex = 0;
  constexpr int32 MaxSkippedDetail = 32;
  TArray<TSharedPtr<FJsonValue>> SkippedTypes;
  int32 SkippedTypeCount = 0;
  TArray<FString> IgnoredFieldNames;
  auto NoteSkippedType = [&](int32 EntryIndex, const TCHAR *Reason) {
    ++SkippedTypeCount;
    if (SkippedTypes.Num() < MaxSkippedDetail) {
      TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
      SkipObj->SetNumberField(TEXT("index"), EntryIndex);
      SkipObj->SetStringField(TEXT("reason"), Reason);
      SkippedTypes.Add(MakeShared<FJsonValueObject>(SkipObj));
    }
  };
  // Read by foliage.add_type, NOT by this verb - see the create_procedural overlay.
  // minScale / maxScale / alignToNormal used to sit here: they were accepted, echoed as
  // ignored, and dropped. They are honoured now (ReadFoliageScaleAndAlign /
  // ApplyFoliageScaleAndAlign below), so they must NOT be listed - a key that is both
  // applied and reported as ignored is the same dishonesty pointing the other way. The
  // mechanism stays for randomYaw, which this verb still does not read, and now also carries
  // the Category=Procedural properties that remain unreachable after
  // F-procedural-foliage-simulation-knobs-unreachable: a caller who guesses one of these
  // engine names is told it had no effect instead of having it silently dropped.
  // UFoliageType properties only - MinimumQuadTreeSize lives on the SPAWNER, so a caller who
  // passes it is already refused by the dispatcher's top-level unknown-param gate.
  static const TCHAR *const UnreadPerTypeFields[] = {
      TEXT("randomYaw"), TEXT("spreadVariance"), TEXT("distributionSeed"),
      TEXT("maxInitialSeedOffset"), TEXT("scaleCurve")};

  // Per-type effective values, plus the two numbers the overlapPriority ordering rule is
  // evaluated on. Collected in the loop and reported below - a per-species simulation config
  // the caller cannot see is the same defect the tiling pair had one level up.
  TArray<TSharedPtr<FJsonValue>> TypeEchoRows;
  TArray<TSharedPtr<FJsonValue>> Warnings;
  struct FFoliageOverlapOrderEntry {
    int32 EntryIndex = 0;
    float EffectiveRadius = 0.0f;
    float OverlapPriority = 0.0f;
  };
  TArray<FFoliageOverlapOrderEntry> OverlapOrder;
  int32 ExplicitOverlapPriorityCount = 0;
  int32 PlacementFilterEntryCount = 0;

  for (int32 EntryIndex = 0; EntryIndex < FoliageTypesArr->Num(); ++EntryIndex) {
    const TSharedPtr<FJsonValue> &Val = (*FoliageTypesArr)[EntryIndex];
    const TSharedPtr<FJsonObject> *TypeObj = nullptr;
    if (!Val.IsValid() || !Val->TryGetObject(TypeObj) || !TypeObj) {
      NoteSkippedType(EntryIndex, TEXT("foliageTypes[] entry is not an object"));
    } else {
      for (const TCHAR *IgnoredField : UnreadPerTypeFields) {
        if ((*TypeObj)->HasField(FString(IgnoredField))) {
          IgnoredFieldNames.AddUnique(FString(IgnoredField));
        }
      }
      FString MeshPath;
      (*TypeObj)->TryGetStringField(TEXT("meshPath"), MeshPath);
      double TypeDensity = 10.0;
      (*TypeObj)->TryGetNumberField(TEXT("density"), TypeDensity);

      if (MeshPath.IsEmpty()) {
        NoteSkippedType(EntryIndex, TEXT("foliageTypes[] entry has no meshPath"));
      } else {
        // GUARDED, because meshPath sits inside an ARRAY ELEMENT. The dispatch-boundary type
        // gate reads top-level params only, so `foliageTypes[].meshPath` never met it - and
        // adopting FParamSpec::NestedKeys here would break this verb's documented open-key-set
        // contract (UnreadPerTypeFields above echoes unread keys). Board
        // B-nested-path-values-reach-createpackage-fatal.
        FString MeshRefusal;
        UStaticMesh *Mesh =
            PinWrightGuardedLoad::LoadObjectChecked<UStaticMesh>(MeshPath, &MeshRefusal);
        if (!Mesh) {
          NoteSkippedType(EntryIndex, MeshRefusal.IsEmpty()
                                          ? TEXT("meshPath did not load as a UStaticMesh")
                                          : *MeshRefusal);
        } else {
          // Same read + validation foliage.add_type uses. A bad range skips this one
          // entry with a reason rather than failing the whole batch, matching how the
          // other unusable-entry cases above are reported; the index is not consumed, so
          // the generated _FT_<n> names stay contiguous.
          FFoliageScaleAndAlignInput ScaleAndAlign;
          FString ScaleError;
          if (!ReadFoliageScaleAndAlign(**TypeObj, ScaleAndAlign, ScaleError)) {
            NoteSkippedType(EntryIndex, *ScaleError);
            continue;
          }

          // Simulation half of the entry. Read and validated BEFORE the asset is created,
          // for the same reason the scale range is: a rejected entry must not consume a
          // _FT_<n> index.
          FFoliageProceduralSimInput SimInput;
          FString SimError;
          if (!ReadFoliageProceduralSimConfig(**TypeObj, SimInput, SimError)) {
            NoteSkippedType(EntryIndex, *SimError);
            continue;
          }

          // Create FoliageType asset. The third CreatePackage the add_type crash reaches: these
          // names are built from the SAME AssetName the spawner used, so a path-shaped `name`
          // composed "/Game/ProceduralFoliage//Game/X/Y_Spawner_FT_0" here too. AssetName and
          // PackagePath are both checked above, so this guard cannot fire on today's code - it is
          // kept so the invariant is restated where CreatePackage is actually called rather than
          // living 250 lines away, and a future edit that reintroduces an unchecked name is
          // skipped with a reason instead of ending the process.
          FString FTName =
              FString::Printf(TEXT("%s_FT_%d"), *AssetName, TypeIndex++);
          FString FTPackagePath;
          FString FTPathError;
          if (!PinWrightComposeAssetPackagePath(PackagePath, FTName, FTPackagePath,
                                                FTPathError)) {
            NoteSkippedType(EntryIndex, *FTPathError);
            continue;
          }
          UPackage *FTPackage = CreatePackage(*FTPackagePath);
          UFoliageType_InstancedStaticMesh *FT =
              NewObject<UFoliageType_InstancedStaticMesh>(
                  FTPackage, FName(*FTName), RF_Public | RF_Standalone);
          FT->SetStaticMesh(Mesh);
          FT->Density = (float)TypeDensity;
          FT->ReapplyDensity = true;
          ApplyFoliageScaleAndAlign(FT, ScaleAndAlign);
          const FFoliageProceduralSimResolution SimResolution =
              ApplyFoliageProceduralSimConfig(FT, SimInput, TypeDensity);

          TSharedPtr<FJsonObject> TypeEcho = MakeFoliageProceduralSimEcho(FT, SimResolution);
          TypeEcho->SetNumberField(TEXT("index"), EntryIndex);
          TypeEcho->SetStringField(TEXT("asset_path"), FT->GetPathName());
          TypeEchoRows.Add(MakeShared<FJsonValueObject>(TypeEcho));

          FFoliageOverlapOrderEntry OrderEntry;
          OrderEntry.EntryIndex = EntryIndex;
          OrderEntry.EffectiveRadius = FMath::Max(FT->CollisionRadius, FT->ShadeRadius);
          OrderEntry.OverlapPriority = FT->OverlapPriority;
          OverlapOrder.Add(OrderEntry);
          if (SimInput.OverlapPriority.IsSet()) {
            ++ExplicitOverlapPriorityCount;
          }
          if (SimInput.Height.IsSet() || SimInput.GroundSlopeAngle.IsSet()) {
            ++PlacementFilterEntryCount;
          }
          // GetSpawnsInShade() ANDs the two, so a lone spawnsInShade:true stores a value the
          // simulation never reads. Named per entry rather than aggregated because the
          // remedy is per entry.
          if (SimInput.bSpawnsInShade.IsSet() && SimInput.bSpawnsInShade.GetValue() &&
              !FT->bCanGrowInShade) {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("foliageTypes[%d] set spawnsInShade without canGrowInShade. The engine "
                     "reads them as canGrowInShade && spawnsInShade, so this entry does not "
                     "spawn in shade; set canGrowInShade:true as well."),
                EntryIndex)));
          }
          if (SimResolution.MaxInitialAgeSource == TEXT("raised_for_procedural_scale")) {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("foliageTypes[%d] supplied proceduralScale with no maxInitialAge, which "
                     "would have collapsed the range to %d discrete sizes (every seed starts "
                     "at age 0). maxInitialAge was raised to maxAge (%f); pass it explicitly "
                     "to override."),
                EntryIndex, FT->NumSteps + 1, FT->MaxAge)));
          } else if (SimInput.ProceduralScale.IsSet() &&
                     FT->ProceduralScale.Max > FT->ProceduralScale.Min &&
                     FT->MaxInitialAge <= 0.0f) {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("foliageTypes[%d] supplied a proceduralScale range with maxInitialAge 0, "
                     "so the range collapses to %d discrete sizes. Raise maxInitialAge to "
                     "spread the initial ages."),
                EntryIndex, FT->NumSteps + 1)));
          }

          FTPackage->MarkPackageDirty();
          FAssetRegistryModule::AssetCreated(FT);

          // Add to Spawner using Reflection (since FoliageTypes is private)
          FArrayProperty *FoliageTypesProp = FindFProperty<FArrayProperty>(
              Spawner->GetClass(), TEXT("FoliageTypes"));
          if (FoliageTypesProp) {
            FScriptArrayHelper Helper(
                FoliageTypesProp,
                FoliageTypesProp->ContainerPtrToValuePtr<void>(Spawner));
            int32 Index = Helper.AddValue();
            void* RawData = Helper.GetRawPtr(Index);
            McpSafeAssetSave(FT);
            UScriptStruct *Struct = FFoliageTypeObject::StaticStruct();

            FObjectProperty *ObjProp = FindFProperty<FObjectProperty>(
                Struct, TEXT("FoliageTypeObject"));
            if (ObjProp) {
              ObjProp->SetObjectPropertyValue(
                  ObjProp->ContainerPtrToValuePtr<void>(RawData), FT);
            }

            FBoolProperty *BoolProp =
                FindFProperty<FBoolProperty>(Struct, TEXT("bIsAsset"));
            if (BoolProp) {
              BoolProp->SetPropertyValue(
                  BoolProp->ContainerPtrToValuePtr<void>(RawData), true);
            }
          }
        }
      }
    }
  }

  // OVERLAP-PRIORITY ORDERING CHECK. The engine's rule is that the instance with the LOWER
  // OverlapPriority is the one removed when two overlap (FoliageType.h), and the contest is
  // entered once per overlapping pair - so a small-radius species at equal-or-higher priority
  // than a large-radius one wins vastly more contests than its share of the area and wipes the
  // large one out (observed on a four-species level: trees 46 -> 0 placed, then 85/1/2/0 when
  // the correction over-shot). Priority must therefore run in the SAME order as effective
  // radius, max(CollisionRadius, ShadeRadius). This verb is the only place that can check it,
  // because it holds every species' radii and priority in one call. It warns rather than
  // refuses: a deliberate inversion is a legitimate, if rare, authoring choice, and refusing
  // would fail a whole batch over a judgement call. Only checked when the caller actually set
  // priorities - all-default zeroes are not an ordering claim.
  if (ExplicitOverlapPriorityCount > 0 && OverlapOrder.Num() > 1) {
    for (int32 A = 0; A < OverlapOrder.Num(); ++A) {
      for (int32 B = A + 1; B < OverlapOrder.Num(); ++B) {
        const FFoliageOverlapOrderEntry &Larger =
            OverlapOrder[A].EffectiveRadius >= OverlapOrder[B].EffectiveRadius ? OverlapOrder[A]
                                                                              : OverlapOrder[B];
        const FFoliageOverlapOrderEntry &Smaller =
            OverlapOrder[A].EffectiveRadius >= OverlapOrder[B].EffectiveRadius ? OverlapOrder[B]
                                                                              : OverlapOrder[A];
        if (Larger.EffectiveRadius > Smaller.EffectiveRadius &&
            Larger.OverlapPriority <= Smaller.OverlapPriority) {
          Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
              TEXT("overlapPriority inverts the radius order: foliageTypes[%d] has the larger "
                   "effective radius (%f vs %f) but priority %f <= %f on foliageTypes[%d]. The "
                   "lower priority loses every overlap, so the larger species will be thinned "
                   "or wiped out. Assign priorities in the same order as effective radius."),
              Larger.EntryIndex, Larger.EffectiveRadius, Smaller.EffectiveRadius,
              Larger.OverlapPriority, Smaller.OverlapPriority, Smaller.EntryIndex)));
        }
      }
    }
  }
  // height / groundSlopeAngle look like elevation and slope NICHES and are not: the tile
  // simulation is terrain-free and the results are projected afterwards, so both only reject
  // members of an already-simulated set. A caller expecting stratification gets thinning.
  if (PlacementFilterEntryCount > 0) {
    Warnings.Add(MakeShared<FJsonValueString>(
        TEXT("height / groundSlopeAngle are placement FILTERS applied after the terrain-free "
             "tile simulation, not elevation niches - excluded seeds are discarded, so a band "
             "comes out sparse rather than differently populated.")));
  }

  Package->MarkPackageDirty();
  FAssetRegistryModule::AssetCreated(Spawner);

  // Spawn Volume
  AProceduralFoliageVolume *Volume = Cast<AProceduralFoliageVolume>(
      SpawnActorInActiveWorld<AActor>(AProceduralFoliageVolume::StaticClass(),
                                      Location, FRotator::ZeroRotator, Name));
  if (!Volume) {
    Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn volume"));
    return true;
  }
  McpSafeAssetSave(Spawner);

  // AProceduralFoliageVolume is an AVolume, and a volume's shape lives in a UModel brush,
  // not in its transform. A raw SpawnActor leaves that model null, so the volume occupies
  // no space at all: the simulation samples an empty region and reports
  // instances_spawned: 0 as if the ground were bare. The requested size used to be written
  // as SetActorScale3D(Size / 200) against a "default extent of 100 units" the never-built
  // brush does not have — the scale landed on nothing, and had it later gained geometry
  // that leftover scale would have multiplied it ~100x. Build the box at the requested
  // FULL size instead and leave the actor at identity scale.
  // B-spawned-volumes-have-no-brush-geometry.
  VolumeBrushGeometry::BuildBoxBrushGeometry(Volume, Size);

  // Split every foliage instance in the world by the one field that distinguishes a
  // simulated instance from a hand-painted one: FFoliageInstance::ProceduralGuid. A
  // procedural scatter stamps the SPAWNING COMPONENT's guid onto every instance it
  // produces, along an unconditional chain — ProceduralFoliageComponent mints the guid in
  // its constructor, GetGenerateProceduralContentParams() hands it to the simulation,
  // ProceduralFoliageTile::ExtractDesiredInstances stamps each desired instance, and
  // FEdModeFoliage::AddInstances copies it onto the real one. A hand-painted instance
  // carries an invalid guid.
  //
  // THIS USED TO ACCUMULATE FFoliageInfo::GetPlacedInstanceCount() and difference it
  // across the resimulation, on the reading that "placed" meant "actually landed". It
  // does not. The engine counts an instance as placed only when
  // !ProceduralGuid.IsValid() (InstancedFoliage.cpp) — placed BY HAND, the exact
  // complement of what this verb produces. Both terms of that delta were therefore blind
  // to every instance the scatter made, the difference was zero for any correct run, and
  // FMath::Max(0, ...) clamped away the only other reachable value. instances_spawned had
  // one reachable value and could never report a success.
  // B-create-procedural-spawned-count-always-zero.
  //
  // The caller MUST pass a valid ProceduralGuid: with an invalid one the first branch
  // would swallow the hand-placed instances instead of the simulated ones.
  auto MeasureFoliageInstancesByProceduralGuid =
      [](UWorld *World, const FGuid &ProceduralGuid, int32 &OutProceduralCount,
         int32 &OutHandPlacedCount) {
        OutProceduralCount = 0;
        OutHandPlacedCount = 0;
        if (!World || !ProceduralGuid.IsValid()) {
          return;
        }
        // The world-wide outer loop is required: the procedural bake spreads across
        // every AInstancedFoliageActor. The per-actor inner walk uses the same
        // ForEachFoliageInfo idiom as foliage.remove / foliage.get_instances (this file);
        // the guid match mirrors AInstancedFoliageActor::
        // ContainsInstancesFromProceduralFoliageComponent, which is what the engine's own
        // editor UI decides its "Unable to spawn instances" toast on.
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It) {
          It->ForEachFoliageInfo([&](UFoliageType *, FFoliageInfo &Info) {
            for (const FFoliageInstance &Instance : Info.Instances) {
              if (Instance.ProceduralGuid == ProceduralGuid) {
                ++OutProceduralCount;
              } else if (!Instance.ProceduralGuid.IsValid()) {
                ++OutHandPlacedCount;
              }
            }
            return true;
          });
        }
      };

  bool bResimulated = false;
  // Measured AFTER the resimulation and attributed by guid, so the number is what THIS
  // call produced rather than a delta that would also absorb a concurrent foliage edit.
  // Both stay unmeasured — and are therefore OMITTED from the response rather than
  // reported as 0 — when there is nothing to measure: no procedural component, no world,
  // or a component carrying no guid. A missing field means "not counted"; a reported 0
  // means "nothing landed", and the two must not collapse onto the same value.
  bool bInstanceCountsMeasured = false;
  int32 InstancesSpawned = 0;
  int32 HandPlacedInstances = 0;
  // Effective overlap, defaulted or supplied. Read back off the component below rather than
  // from the local, so the echo describes what the simulation actually ran with - and stays
  // ABSENT rather than reading 0 if there is no component to write it to, because a 0 there
  // would contradict a caller who supplied 100.
  float EffectiveTileOverlap = 0.0f;
  bool bTileOverlapApplied = false;
  if (UProceduralFoliageComponent *ProcComp = Volume->ProceduralComponent) {
    ProcComp->FoliageSpawner = Spawner;
    ProcComp->TileOverlap = static_cast<float>(TileOverlap);
    EffectiveTileOverlap = ProcComp->TileOverlap;
    bTileOverlapApplied = true;

    UWorld *ProcWorld = ProcComp->GetWorld();

    // Run the procedural simulation and ACTUALLY place the generated instances.
    // UProceduralFoliageComponent::ResimulateProceduralFoliage takes a callback
    // (named AddInstancesFunc in the engine) that is the ONLY path that places the
    // generated DesiredFoliageInstances into the world — the component does not add
    // them itself, so an empty callback discards every instance. The engine's own
    // reference callback fills it with FEdModeFoliage::AddInstances(...), but that
    // static (and the FFoliagePaintingGeometryFilter it takes) live only in
    // FoliageEdit's PRIVATE FoliageEdMode.h and carry no *_API export, so they are
    // not linkable cross-module from this plugin. Instead invoke the public
    // BlueprintCallable UProceduralFoliageEditorLibrary::ResimulateProceduralFoliageComponents,
    // which wraps ResimulateProceduralFoliage with the real AddInstances callback.
    // That library class is UCLASS() with no *_API export either, so it cannot be
    // referenced by direct linkage (StaticClass()/NewObject would be a link error);
    // resolve it by reflection (FindObject on the /Script path) and invoke the
    // reflected UFUNCTION via ProcessEvent — the project's documented cross-module
    // convention for engine types that lack an export macro.
    UClass *LibClass = FindObject<UClass>(
        nullptr, TEXT("/Script/FoliageEdit.ProceduralFoliageEditorLibrary"));
    UFunction *ResimFn = LibClass ? LibClass->FindFunctionByName(
                                        TEXT("ResimulateProceduralFoliageComponents"))
                                  : nullptr;
    if (ResimFn) {
      // The UFUNCTION's single parameter is a TArray<UProceduralFoliageComponent*>;
      // this struct mirrors that parameter layout for ProcessEvent.
      struct FResimParams {
        TArray<UProceduralFoliageComponent *> Components;
      } Params;
      Params.Components.Add(ProcComp);
      // Static UFUNCTION — invoke on the class default object. The library function
      // is void (no sim-success return), so bResimulated reports only that the
      // FoliageEdit reflection path was reachable and DISPATCHED — it goes false when
      // FoliageEdit isn't loaded or the class/CDO can't be resolved. It does NOT
      // indicate whether any foliage was placed; that is instances_spawned's job.
      if (UObject *CDO = LibClass->GetDefaultObject()) {
        CDO->ProcessEvent(ResimFn, &Params);
        bResimulated = true;
      }
    }

    // Report what actually landed. instances_spawned counts the instances carrying THIS
    // component's ProceduralGuid — the ones this call produced — while resimulated only
    // says the resim path was dispatched. hand_placed_instances_in_world is the world's
    // hand-painted total, published beside it so the two can never be read as one number:
    // it is what the old counter was actually measuring, and this verb produces none of
    // it. A reported 0 on instances_spawned is now a real "nothing was placed".
    const FGuid &SpawnedProceduralGuid = ProcComp->GetProceduralGuid();
    if (ProcWorld && SpawnedProceduralGuid.IsValid()) {
      MeasureFoliageInstancesByProceduralGuid(ProcWorld, SpawnedProceduralGuid,
                                              InstancesSpawned, HandPlacedInstances);
      bInstanceCountsMeasured = true;
    }

    // Measured against what the dispatch implies: the resim path ran to completion, so
    // something was expected to land. Nothing did. Same condition the engine's editor UI
    // raises its "Unable to spawn instances" toast on
    // (ProceduralFoliageEditorLibrary.cpp), surfaced here because an MCP caller never
    // sees that toast.
    if (bInstanceCountsMeasured && bResimulated && InstancesSpawned == 0) {
      Warnings.Add(MakeShared<FJsonValueString>(
          TEXT("the resimulation ran but placed nothing: no instance in the world carries "
               "this volume's ProceduralGuid. Ensure a large enough surface exists inside "
               "the volume - the tile simulation is terrain-free and its results are "
               "projected onto world geometry afterwards, so an empty volume scatters "
               "zero.")));
      // Deliberately NOT also a UE_LOG(Warning): a host with the engine default
      // bElevateLogWarningsToErrors=true promotes an automation-run log warning into a test
      // error, and this branch is the normal outcome of a surfaceless test fixture. The
      // response warnings[] entry is the caller-facing channel and carries the same fact.
    }
  }

  // Distinguishes "not counted" from "nothing landed" in as many words, because the two
  // fields below are absent rather than 0 in this case.
  if (!bInstanceCountsMeasured) {
    Warnings.Add(MakeShared<FJsonValueString>(
        TEXT("instances_spawned / hand_placed_instances_in_world are omitted, not zero: "
             "the spawned volume had no procedural component (or no world, or no "
             "ProceduralGuid) to attribute placed instances to, so nothing was counted.")));
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("volume_actor"), Volume->GetActorLabel());
  Resp->SetStringField(TEXT("spawner_path"), Spawner->GetPathName());
  // Always present, supplied or defaulted: these two decide the scatter's grid, so a
  // caller must be able to see what it ran on without opening the spawner asset.
  Resp->SetNumberField(TEXT("tile_size"), Spawner->TileSize);
  Resp->SetNumberField(TEXT("num_unique_tiles"), Spawner->NumUniqueTiles);
  // Same rule, third grid number: it was hardcoded AND unreported, so a caller could not even
  // observe the seam behaviour it controls. Measured off the component, so it is omitted when
  // there was no component to measure.
  if (bTileOverlapApplied) {
    Resp->SetNumberField(TEXT("tile_overlap"), EffectiveTileOverlap);
  }
  // Where the generated assets actually landed, defaulted or supplied.
  Resp->SetStringField(TEXT("save_path"), PackagePath);
  Resp->SetNumberField(TEXT("foliage_types_count"), TypeIndex);
  Resp->SetNumberField(TEXT("foliage_types_requested"), FoliageTypesArr->Num());
  Resp->SetNumberField(TEXT("skippedCount"), SkippedTypeCount);
  if (SkippedTypeCount > 0) {
    Resp->SetArrayField(TEXT("skipped"), SkippedTypes);
    Resp->SetBoolField(TEXT("skippedTruncated"), SkippedTypes.Num() < SkippedTypeCount);
  }
  if (IgnoredFieldNames.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> IgnoredArr;
    for (const FString &IgnoredName : IgnoredFieldNames) {
      IgnoredArr.Add(MakeShared<FJsonValueString>(IgnoredName));
    }
    Resp->SetArrayField(TEXT("ignoredFields"), IgnoredArr);
  }
  // One row per type actually built, carrying the simulation values as WRITTEN. The two
  // derived ones (initial_seed_density, max_initial_age) name their source, because a number
  // the caller never typed is exactly the kind a response must not present as a request echo.
  if (TypeEchoRows.Num() > 0) {
    Resp->SetArrayField(TEXT("foliage_types"), TypeEchoRows);
  }
  // Only when something is actually worth saying; an empty warnings[] is noise.
  if (Warnings.Num() > 0) {
    Resp->SetArrayField(TEXT("warnings"), Warnings);
  }
  Resp->SetBoolField(TEXT("resimulated"), bResimulated);
  // Both measured off the foliage actors after the resimulation, and both omitted rather
  // than reported as 0 when there was nothing to measure - see bInstanceCountsMeasured.
  if (bInstanceCountsMeasured) {
    Resp->SetNumberField(TEXT("instances_spawned"), InstancesSpawned);
    Resp->SetNumberField(TEXT("hand_placed_instances_in_world"), HandPlacedInstances);
  }

  // Add verification data
  AddActorVerification(Resp, Volume);
  AddAssetVerification(Resp, Spawner);

  Ctx.SendSuccess(Resp);
  return true;
}
