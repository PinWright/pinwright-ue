// Copyright (c) 2026 Alexander Penkin. MIT License.

// LandscapeHandler.cpp - Migrated from PinWright_LandscapeHandlers.cpp
// Landscape creation, heightmap modification, sculpting, painting, material, and grass type handlers

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Handlers/Environment/LandscapeBrush.h"
#include "Handlers/Environment/LandscapeHeightStats.h"
#include "Handlers/Environment/LandscapeShapeMetrics.h"
#include "Handlers/Environment/ScalabilityDensityCVars.h"
#include "Dispatch/SafePoint.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/AssetUtils.h"
#include "ScopedTransaction.h"

#include "Dom/JsonValue.h"
// ULevel — CaptureLandscapeReadDirtyState captures the level's package explicitly,
// which under OFPA is a different package from the landscape actor's.
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0) && __has_include("LandscapeEditLayer.h")
#include "LandscapeEditLayer.h"  // ULandscapeEditLayerBase::GetGuid (5.6+ per-layer GUID)
#endif
#include "LandscapeEditorObject.h"
#include "LandscapeEditorUtils.h"
#include "LandscapeGrassType.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
// ULandscapeSettings — the auto-created target-layer info honours GetDefaultLayerInfoObject()
// and the blend method. Reached transitively today via ScopedLandscapeLayerDialog.h; declared
// here because the -StrictIncludes -DisableUnity packaging build does not inherit it.
#include "LandscapeSettings.h"
#include "LandscapeStreamingProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/SavePackage.h"
// FPropertyChangedEvent / FindFProperty / GET_MEMBER_NAME_CHECKED — explicit include:
// unity builds inherit these from sibling TUs, the -StrictIncludes -DisableUnity
// Rocket packaging build does not.
#include "UObject/UnrealType.h"
#include "Utils/PackageDirtyUtils.h"
#include "Utils/ScopedLandscapeLayerDialog.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

// ---- Why every landscape READ here opts out of dirtying (B-landscape-get-heights-dirties-map) ----
//
// FLandscapeEditDataInterface is an EDIT interface even when nothing is written to it.
// GetHeightData -> GetHeightDataTempl (LandscapeEditInterface.cpp:1358) ->
// GetHeightDataInternal (:859) -> GetHeightMapColor (:815) reaches
// FLandscapeTextureDataInterface::GetTextureDataInfo (:3451-3457), which constructs an
// FLandscapeTextureDataInfo whose ctor calls Texture->Modify(bShouldDirtyPackage)
// (:4028-4043) with the interface's default bShouldDirtyPackage = true (:88-90). The
// heightmap/weightmap texture is outered to the landscape proxy actor
// (ALandscapeProxy::CreateLandscapeTexture, LandscapeEdit.cpp:7954-7957), so
// UObject::Modify (Obj.cpp:1652-1680) marks the PROXY's package dirty — which on a
// classic (non-OFPA) level is the MAP package. That is the entire defect: one 5x5
// landscape.get_heights took editor.list_dirty_packages from 0 to 1 on a fresh boot.
// The engine's own comment at :4036-4038 admits the Modify is unwanted for read-only use
// and that removing it properly would mean rebuilding TLandscapeEditCache.
//
// bUploadTextureChangesToGPU=false does NOT make the interface read-only — it only gates
// the GPU upload inside Flush() (:96-124). SetShouldDirtyPackage(false) is the engine's
// supported opt-out and is what actually removes the dirty: with bAlwaysMarkDirty=false,
// UObject::Modify reaches MarkPackageDirty only when the caller asked for it.
//
// Safe to apply to the read halves of the WRITE verbs too, because every write verb in
// this file dirties explicitly and independently — landscape.sculpt and landscape.edit
// call Landscape->MarkPackageDirty(), landscape.create_procedural_terrain calls
// PinWright::MarkLevelActorModified — so a non-dirtying read can never silence a real
// write. It does mean a landscape.sculpt refused with LANDSCAPE_SCULPT_NO_CHANGE now
// leaves the level exactly as clean as it found it, which is the honest outcome.
static void MakeLandscapeEditInterfaceReadOnly(FLandscapeEditDataInterface& EditInterface)
{
  EditInterface.SetShouldDirtyPackage(false);
}

// Capture the dirty flag of every package a landscape read could touch, so the read
// restores whatever it found. Belt-and-braces on top of MakeLandscapeEditInterfaceReadOnly:
// that removes the known cause, this makes "a read changes no dirty flag" a property of
// the verb rather than a property of an engine internal that could move again. A restore
// that actually fires logs a Warning naming the package (PackageDirtyUtils.h), so the
// guard reports rather than hides a regression.
//
// The set is the landscape actor, every registered proxy (each owns the textures for its
// own components), and the level — under OFPA those are three different packages, on a
// classic map they collapse to one and the duplicate captures are dropped.
static void CaptureLandscapeReadDirtyState(
    PinWright::PackageDirty::FScopedPackageDirtyRestore& Guard,
    ALandscape* Landscape, ULandscapeInfo* LandscapeInfo)
{
  if (Landscape) {
    Guard.Capture(Landscape);
    if (ULevel* Level = Landscape->GetLevel()) {
      Guard.Capture(Level);
    }
  }
  if (LandscapeInfo) {
    LandscapeInfo->ForEachLandscapeProxy([&Guard](ALandscapeProxy* Proxy) -> bool {
      Guard.Capture(Proxy);
      return true;
    });
  }
}

// ---- Helper: Find landscape by name/path ----
static ALandscape* FindLandscapeByNameOrPath(
    const FString& LandscapePath, const FString& LandscapeName,
    FString& OutErrorMessage)
{
  // PRIORITY 1: Find landscape in current world by name (works for transient actors)
  ALandscape* Landscape = nullptr;
  if (GEditor) {
    if (UEditorActorSubsystem *ActorSS =
            GEditor->GetEditorSubsystem<UEditorActorSubsystem>()) {
      TArray<AActor *> AllActors = ActorSS->GetAllLevelActors();
      for (AActor *A : AllActors) {
        if (ALandscape *L = Cast<ALandscape>(A)) {
          // Match by landscapeName if provided (actor label)
          if (!LandscapeName.IsEmpty() &&
              L->GetActorLabel().Equals(LandscapeName, ESearchCase::IgnoreCase)) {
            Landscape = L;
            break;
          }
          // Match by path: compare asset path from the landscape's package
          if (!LandscapePath.IsEmpty()) {
            FString ActorAssetPath = L->GetPackage()->GetPathName();
            FString NormalizedRequest = LandscapePath;
            FString NormalizedActor = ActorAssetPath;
            NormalizedRequest.ReplaceInline(TEXT("\\"), TEXT("/"));
            NormalizedActor.ReplaceInline(TEXT("\\"), TEXT("/"));
            if (NormalizedActor.EndsWith(TEXT(".uasset"))) {
              NormalizedActor = NormalizedActor.LeftChop(7);
            }
            if (NormalizedActor.Equals(NormalizedRequest, ESearchCase::IgnoreCase)) {
              Landscape = L;
              break;
            }
          }
        }
      }
    }
  }

  // PRIORITY 2: Try to load from disk (for saved landscape assets)
  if (!Landscape && !LandscapePath.IsEmpty()) {
    Landscape = Cast<ALandscape>(
        StaticLoadObject(ALandscape::StaticClass(), nullptr, *LandscapePath));
  }

  if (!Landscape) {
    OutErrorMessage = LandscapeName.IsEmpty()
        ? FString::Printf(TEXT("Landscape not found at path: %s"), *LandscapePath)
        : FString::Printf(TEXT("Landscape '%s' not found (path: %s)"), *LandscapeName, *LandscapePath);
  }
  return Landscape;
}

// ---- Helper: pick the edit layer a height write should target ----
// On UE 5.7 edit-layer landscapes, FLandscapeEditDataInterface's shared-layer mode
// resolves its target via ALandscape::GetEditingLayer(). Outside Landscape Ed Mode
// (as in an RPC / automation) NO layer is being edited, so GetEditingLayer() is
// invalid and a SetHeightData write does not land in any persistent edit layer.
// A subsequent full layer regeneration then composites the (unchanged) edit layers
// over the base heightmap and the write is lost — the readback comes back flat.
// Returns the GUID of the landscape's default (index 0) edit layer, or an invalid
// FGuid if the landscape is not edit-layer based (older non-layer path: the write
// goes straight to the base heightmap and this scoping is unnecessary).
static FGuid GetDefaultEditLayerGuid(ALandscape* Landscape)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
  // GetLayerCount() is UE_DEPRECATED(5.6) and removed in 5.8;
  // GetLayersConst().Num() is the live edit-layer count accessor from 5.6 on.
  if (Landscape && Landscape->GetLayersConst().Num() > 0)
#else
  // 5.3-5.5: GetLayersConst() doesn't exist yet; GetLayerCount() is current.
  if (Landscape && Landscape->GetLayerCount() > 0)
#endif
  {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // 5.6+: the per-layer GUID lives on the ULandscapeEditLayerBase object.
    if (const ULandscapeEditLayerBase* EditLayer = Landscape->GetEditLayerConst(0))
    {
      return EditLayer->GetGuid();
    }
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // 5.5: the GUID is a direct member of the FLandscapeLayer struct.
    if (const FLandscapeLayer* Layer = Landscape->GetLayerConst(0))
    {
      return Layer->Guid;
    }
#else
    // 5.3–5.4: GetLayerConst doesn't exist yet; GetLayer is the accessor.
    if (const FLandscapeLayer* Layer = Landscape->GetLayer(0))
    {
      return Layer->Guid;
    }
#endif
  }
  return FGuid();
}

// ---- Helper: settle the deferred edit-layer regen so bounds/collision refresh now ----
// On UE 5.7's edit-layer landscapes, a height write only requests a DEFERRED layer
// regeneration; the component's CachedLocalBox (hence render/collision bounds) is only
// recomputed later, when RegenerateLayersHeightmaps -> UpdateForChangedHeightmaps ->
// ULandscapeComponent::UpdateCachedBounds() ticks. Without forcing that pass, a bounds
// readback in the very next RPC still reports the pre-edit flat Z.
//
// One ForceUpdateLayersContent() is NOT sufficient to make CachedLocalBox current.
// UpdateCachedBounds() reads the FINAL (merged, non-editing-layer) heightmap via
// FLandscapeComponentDataInterface(..., bWorkOnEditingLayer=false); that texture is
// produced by a GPU merge whose CPU readback (FLandscapeEditLayerReadback) resolves a
// frame LATER than the regen that kicked it. So the first update regenerates + enqueues
// the readback, and the merged heights (hence bounds) only land once the readback
// resolves on a subsequent update. A single call left Max.Z at the flat baseline.
//
// We therefore PUMP ForceUpdateLayersContent until ALandscape::IsUpToDate() reports
// no pending LayerContentUpdateModes AND no outstanding readback work — i.e. the merge
// + readback + UpdateForChangedHeightmaps -> UpdateCachedBounds chain has fully drained
// and the bounds reflect the new relief before this call returns. The loop is bounded
// so a landscape that can never settle (e.g. a stuck async resource) cannot hang the RPC.
// The height write itself must already have landed in a real edit layer AND been
// uploaded to the GPU (see GetDefaultEditLayerGuid + FScopedSetLandscapeEditingLayer +
// the bUploadTextureChangesToGPU=true Flush at the call sites), or the merge composites
// stale/empty layer data over the base and the write never shows up in the readback.
// Both height-write handlers (landscape.sculpt / landscape.edit) share this settle step
// so the two sites cannot drift.
static void SettleLandscapeLayers(ALandscape* Landscape)
{
  if (!Landscape)
  {
    return;
  }

  // Pump the update to completion: each pass resolves the readback enqueued by the
  // previous one, so the merged final heightmap (and thus UpdateCachedBounds) is only
  // guaranteed current once IsUpToDate() is true. Bounded so we never spin forever.
  constexpr int32 MaxSettlePasses = 16;
  for (int32 Pass = 0; Pass < MaxSettlePasses && !Landscape->IsUpToDate(); ++Pass)
  {
    Landscape->ForceUpdateLayersContent();
  }
}

// Assign LandscapeMaterial and notify the engine the way the property editor does.
//
// A BARE PostEditChange() IS NOT ENOUGH, and its failure is silent. PostEditChange()
// builds an EMPTY FPropertyChangedEvent, so MemberPropertyName is NAME_None and the
// GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial) branch never matches —
// for ANY assignment, not merely a same-pointer one. Neither override has a
// null-property catch-all, so the material-changed work is simply skipped:
//   * ALandscapeProxy::PostEditChangeProperty (LandscapeEdit.cpp:6143) clears the
//     parents out of MaterialInstanceConstantMap, empties it, and calls
//     UpdateAllComponentMaterialInstances() (:6175)
//   * ALandscape::PostEditChangeProperty (:6629) sets bMaterialChanged, which drives
//     UpdateAllComponentMaterialInstances() (:6792), the Nanite/mobile invalidation,
//     and propagation to the streaming proxies
// Skip those and every ULandscapeComponent keeps its previously built MIC — with the
// OLD shader map — so no material edit reaches the screen, topology or constant-value.
// Callers were working around this by assigning a different material and then assigning
// the intended one back, which only worked because the second assignment happened to
// run while the components had already been torn down by the first.
//
// The engine's own setter, ALandscapeProxy::EditorSetLandscapeMaterial
// (LandscapeBlueprintSupport.cpp:98), does exactly this. It is NOT callable from here:
// ALandscapeProxy is UCLASS(MinimalAPI) and the setter carries no LANDSCAPE_API, so
// only its type info is exported. PostEditChangeProperty is virtual, so calling it
// through the vtable needs no exported symbol — which is why this reconstructs the
// event rather than delegating. ALandscape::PostEditChangeProperty chains to
// Super::PostEditChangeProperty, so one virtual call runs both branches above.
static bool SetLandscapeMaterialAndNotify(ALandscape* Landscape, UMaterialInterface* Mat)
{
  if (!Landscape)
  {
    return false;
  }

  Landscape->LandscapeMaterial = Mat;

  FProperty* MaterialProperty = FindFProperty<FProperty>(
      ALandscapeProxy::StaticClass(),
      GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial));
  if (!MaterialProperty)
  {
    // Should be unreachable (the UPROPERTY is engine-serialized data), but a bare
    // PostEditChange() is strictly better than dropping the notification entirely.
    Landscape->PostEditChange();
    return false;
  }

  FPropertyChangedEvent PropertyChangedEvent(MaterialProperty);
  Landscape->PostEditChangeProperty(PropertyChangedEvent);
  return true;
}

// UMaterialExpressionLandscapeVisibilityMask::ParameterName — the engine's hole-mask
// pseudo-layer. It appears in a material's cached target-layer list beside real
// weight-blended layers, but it is NOT paintable through a weight write: the engine binds
// it to the ALandscapeProxy::VisibilityLayer singleton, not to a per-landscape
// ULandscapeLayerInfoObject. Filtered out of the paintable set so the "available layers"
// listing in LAYER_NOT_FOUND never advertises a name that cannot in fact be painted.
// (A landscape whose target_layers hold ONLY this name is exactly the reported repro:
// a material with no LandscapeLayerBlend at all.) Spelled as a literal rather than
// referencing the expression class so no new module linkage is introduced; the name is
// serialized engine data and is identical on 5.3-5.8. Behind a function-local static so
// the FName is built on first call rather than during static initialization.
static const FName& GetLandscapeVisibilityLayerName()
{
  static const FName VisibilityLayerName(TEXT("__LANDSCAPE_VISIBILITY__"));
  return VisibilityLayerName;
}

// ---- Helper: the target layers a landscape's material(s) actually declare ----
// This is the authoritative answer to "does layer X exist on this landscape".
// ULandscapeInfo::Layers is only a CACHE rebuilt from this set by UpdateLayerInfoMap();
// it can be stale, and nothing stops an entry being added for a name no material
// declares — which is precisely how landscape.create_procedural_terrain used to record a
// paint into a layer that no shader ever samples and report success
// (B-create-procedural-terrain-paints-nothing). Reading the material is what makes the
// LAYER_NOT_FOUND rejection trustworthy.
static TArray<FName> GetPaintableTargetLayerNames(ALandscape* Landscape)
{
  TArray<FName> Names;
  if (!Landscape)
  {
    return Names;
  }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
  // 5.5+: RetrieveTargetLayerNamesFromMaterials() unions the proxy material, per-component
  // materials, LOD materials and the hole material. GetLayersFromMaterial() still exists
  // but is UE_DEPRECATED(5.8) in favour of this.
  Names = Landscape->RetrieveTargetLayerNamesFromMaterials();
#else
  // 5.3-5.4: RetrieveTargetLayerNamesFromMaterials does not exist yet; GetLayersFromMaterial
  // is current (and returns by const-ref on these versions, so this copies).
  Names = Landscape->GetLayersFromMaterial();
#endif
  const FName& VisibilityLayerName = GetLandscapeVisibilityLayerName();
  Names.RemoveAll([&VisibilityLayerName](const FName& Name)
  {
    return Name.IsNone() || Name == VisibilityLayerName;
  });
  Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
  return Names;
}

// Renders a layer-name list for an error message: "Rock, Sand, Snow" (or "(none)").
// Capped so a pathological material cannot blow the response budget; the uncapped total
// travels in the error data's availableLayerCount.
static FString FormatLayerNameList(const TArray<FName>& Names)
{
  constexpr int32 MaxListed = 32;
  if (Names.Num() == 0)
  {
    return TEXT("(none)");
  }
  TArray<FString> Parts;
  const int32 Listed = FMath::Min(Names.Num(), MaxListed);
  Parts.Reserve(Listed);
  for (int32 i = 0; i < Listed; ++i)
  {
    Parts.Add(Names[i].ToString());
  }
  FString Out = FString::Join(Parts, TEXT(", "));
  if (Names.Num() > MaxListed)
  {
    Out += FString::Printf(TEXT(", ... (%d more)"), Names.Num() - MaxListed);
  }
  return Out;
}

// ---- landscape.create ----
REGISTER_RPC_HANDLER("landscape.create", "landscape", "Spawn a new ALandscape actor in the active world with the given component grid and starter material. Component grid defaults to 8x8 of 63x63 quads (~505x505 vertex resolution). A world-unit sizeX / sizeY request is SNAPPED to whole components (8064 cm each at the defaults); the response reports the measured worldSizeX / worldSizeY and drawScale beside requestedSizeX / requestedSizeY, and warns when the snap moved the result. Use landscape.sculpt / landscape.edit afterward to shape the heightmap.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("name", "string", "Display label for the new landscape actor.", "landscapeName"),
        RPC_PARAM_OPT("location", "object|array", "World location {x, y, z} for the landscape origin in centimeters. A 3-element array is also accepted, as are the flat top-level x / y / z below."),
        RPC_PARAM_OPT("x", "number", "Flat alternative to location.x, in centimeters. Read only when all three of x/y/z are supplied; otherwise location wins."),
        RPC_PARAM_OPT("y", "number", "Flat alternative to location.y, in centimeters. Read only when all three of x/y/z are supplied; otherwise location wins."),
        RPC_PARAM_OPT("z", "number", "Flat alternative to location.z, in centimeters. Read only when all three of x/y/z are supplied; otherwise location wins."),
        RPC_PARAM_OPT("componentsX", "number", "Number of landscape components along the X axis. Defaults to 8."),
        RPC_PARAM_OPT("componentsY", "number", "Number of landscape components along the Y axis. Defaults to 8."),
        RPC_PARAM_OPT("componentCount", "integer", "Square shorthand: applied to whichever of componentsX / componentsY was not supplied explicitly."),
        RPC_PARAM_OPT("sizeX", "number", "Requested landscape extent along X in world centimeters. A landscape's extent is QUANTISED to whole components, so this is snapped, not honoured verbatim: componentsX = max(1, round(sizeX / (componentSizeQuads * drawScale.x))). One component spans 8064 cm at the defaults (63 quads x 1 subsection x the ALandscapeProxy draw scale of 128), so that is also the step size. The response reports the MEASURED worldSizeX beside requestedSizeX and warns when they differ. Ignored when componentsX is supplied."),
        RPC_PARAM_OPT("sizeY", "number", "Requested landscape extent along Y in world centimeters. Snapped to whole components the same way sizeX is; see there. The response reports the measured worldSizeY beside requestedSizeY. Ignored when componentsY is supplied."),
        RPC_PARAM_OPT_ALIAS("quadsPerComponent", "number", "Quads per subsection edge (the landscape SubsectionSizeQuads); must be one of 7,15,31,63,127,255 (SubsectionSizeQuads+1 must be a power of two). The whole-component quad count is this times the subsection grid. Rejected with INVALID_ARGUMENT otherwise. Defaults to 63.", "quadsPerSection"),
        RPC_PARAM_OPT("sectionsPerComponent", "number", "Total sections per component: 1 (1x1 grid -> NumSubsections 1) or 4 (2x2 grid -> NumSubsections 2). Higher gives finer LODs. Rejected with INVALID_ARGUMENT otherwise. Defaults to 1."),
        RPC_PARAM_OPT("materialPath", "path", "Object path to a UMaterialInterface to apply; defaults to /Engine/EngineMaterials/WorldGridMaterial.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("create_landscape payload missing"));
    return true;
  }

  // Parse inputs (accept multiple shapes)
  double X = 0.0, Y = 0.0, Z = 0.0;
  if (!Payload->TryGetNumberField(TEXT("x"), X) ||
      !Payload->TryGetNumberField(TEXT("y"), Y) ||
      !Payload->TryGetNumberField(TEXT("z"), Z)) {
    const TSharedPtr<FJsonObject> *LocObj = nullptr;
    if (Payload->TryGetObjectField(TEXT("location"), LocObj) && LocObj) {
      (*LocObj)->TryGetNumberField(TEXT("x"), X);
      (*LocObj)->TryGetNumberField(TEXT("y"), Y);
      (*LocObj)->TryGetNumberField(TEXT("z"), Z);
    } else {
      const TArray<TSharedPtr<FJsonValue>> *LocArr = nullptr;
      if (Payload->TryGetArrayField(TEXT("location"), LocArr) && LocArr &&
          LocArr->Num() >= 3) {
        X = (*LocArr)[0]->AsNumber();
        Y = (*LocArr)[1]->AsNumber();
        Z = (*LocArr)[2]->AsNumber();
      }
    }
  }

  int32 ComponentsX = 8, ComponentsY = 8;
  bool bHasCX = Payload->TryGetNumberField(TEXT("componentsX"), ComponentsX);
  bool bHasCY = Payload->TryGetNumberField(TEXT("componentsY"), ComponentsY);

  int32 ComponentCount = 0;
  Payload->TryGetNumberField(TEXT("componentCount"), ComponentCount);
  if (!bHasCX && ComponentCount > 0) { ComponentsX = ComponentCount; }
  if (!bHasCY && ComponentCount > 0) { ComponentsY = ComponentCount; }

  // sizeX / sizeY are WORLD-UNIT requests, and the component count they map to
  // depends on the world span of one component -- ComponentSizeQuads * DrawScale --
  // so neither term is known here. Only capture the request; the conversion runs on
  // the game thread below, against the scale the spawned actor actually carries.
  //
  // The conversion used to be a literal floor(size / 1000), asserting 1000 uu per
  // component. Nothing made that true: this handler never sets an actor scale, so the
  // landscape keeps the ALandscapeProxy CDO's 128 (engine Landscape.cpp:1762,
  // "Old default scale, preserved for compatibility"), and one default component spans
  // 63 * 128 = 8064 uu. Every requested extent was therefore built 8.064x too LARGE,
  // and the response echoed only quad counts, so nothing in it could reveal that.
  double SizeXUnits = 0.0, SizeYUnits = 0.0;
  const bool bHasSizeX =
      Payload->TryGetNumberField(TEXT("sizeX"), SizeXUnits) && SizeXUnits > 0 && !bHasCX;
  const bool bHasSizeY =
      Payload->TryGetNumberField(TEXT("sizeY"), SizeYUnits) && SizeYUnits > 0 && !bHasCY;

  int32 QuadsPerComponent = 63;
  if (!Payload->TryGetNumberField(TEXT("quadsPerComponent"), QuadsPerComponent)) {
    Payload->TryGetNumberField(TEXT("quadsPerSection"), QuadsPerComponent);
  }

  int32 SectionsPerComponent = 1;
  Payload->TryGetNumberField(TEXT("sectionsPerComponent"), SectionsPerComponent);

  // Derive the three landscape geometry fields from one consistent source so the
  // engine invariant ComponentSizeQuads == SubsectionSizeQuads * NumSubsections
  // always holds. UE's own model (FLandscapeConfig in LandscapeConfigHelper.cpp):
  //   SubsectionSizeQuadsValues = {7,15,31,63,127,255}  (per-subsection quad count)
  //   NumSectionValues          = {1,2}                 (subsections along each axis)
  //   ComponentSizeQuads        = NumSubsections * SubsectionSizeQuads
  // `quadsPerComponent` is the per-subsection size (NOT the whole-component size),
  // and `sectionsPerComponent` is the TOTAL section count: 1 (1x1 grid -> 1
  // subsection) or 4 (2x2 grid -> 2 subsections). Reject anything that does not
  // map onto a valid landscape rather than silently truncating via integer
  // division and reporting success (the old derivation produced a structurally
  // invalid actor whose component init asserts ComponentSizeQuads ==
  // NumSubsections * SubsectionSizeQuads in ULandscapeComponent::Init).
  // Validated here, before the editor-world check, so the rejection depends only
  // on the arguments.
  const int32 SubsectionSizeQuadsField = QuadsPerComponent;
  static const int32 AllowedSubsectionSizes[] = {7, 15, 31, 63, 127, 255};
  if (!MakeArrayView(AllowedSubsectionSizes).Contains(SubsectionSizeQuadsField)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(
            TEXT("quadsPerComponent must be one of 7, 15, 31, 63, 127, 255 ")
            TEXT("(SubsectionSizeQuads+1 must be a power of two); got %d"),
            SubsectionSizeQuadsField));
    return true;
  }

  // Map total section count -> NumSubsections (subsections per axis): 1 -> 1,
  // 4 -> 2. Accept the per-axis value 2 as an alias for the 2x2 grid so callers
  // that already think in NumSubsections terms are not surprised.
  int32 NumSubsectionsField;
  if (SectionsPerComponent == 1) {
    NumSubsectionsField = 1;
  } else if (SectionsPerComponent == 4 || SectionsPerComponent == 2) {
    NumSubsectionsField = 2;
  } else {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(
            TEXT("sectionsPerComponent must be 1 (1x1 grid) or 4 (2x2 grid); got %d"),
            SectionsPerComponent));
    return true;
  }

  const int32 ComponentSizeQuadsField = NumSubsectionsField * SubsectionSizeQuadsField;

  FString MaterialPath;
  Payload->TryGetStringField(TEXT("materialPath"), MaterialPath);
  if (MaterialPath.IsEmpty()) {
    MaterialPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial");
  }

  if (!GEditor || !GEditor->GetEditorWorldContext().World()) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor world not available"));
    return true;
  }

  FString NameOverride;
  if (!Payload->TryGetStringField(TEXT("name"), NameOverride) || NameOverride.IsEmpty()) {
    Payload->TryGetStringField(TEXT("landscapeName"), NameOverride);
  }

  if (NameOverride.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("name or landscapeName parameter is required for create_landscape"));
    return true;
  }

  if (NameOverride.Contains(TEXT("/")) || NameOverride.Contains(TEXT("\\")) ||
      NameOverride.Contains(TEXT(":")) || NameOverride.Contains(TEXT("*")) ||
      NameOverride.Contains(TEXT("?")) || NameOverride.Contains(TEXT("\"")) ||
      NameOverride.Contains(TEXT("<")) || NameOverride.Contains(TEXT(">")) ||
      NameOverride.Contains(TEXT("|"))) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("name contains invalid characters (/, \\, :, *, ?, \", <, >, |)"));
    return true;
  }

  if (NameOverride.Len() > 128) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("name exceeds maximum length of 128 characters"));
    return true;
  }

  const int32 CaptComponentsX = ComponentsX;
  const int32 CaptComponentsY = ComponentsY;
  const int32 CaptComponentSizeQuads = ComponentSizeQuadsField;
  const int32 CaptSubsectionSizeQuads = SubsectionSizeQuadsField;
  const int32 CaptNumSubsections = NumSubsectionsField;
  const bool CaptHasSizeX = bHasSizeX;
  const bool CaptHasSizeY = bHasSizeY;
  const double CaptSizeXUnits = SizeXUnits;
  const double CaptSizeYUnits = SizeYUnits;
  const FVector CaptLocation(X, Y, Z);
  const FString CaptMaterialPath = MaterialPath;
  const FString CaptName = NameOverride;

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("HandleCreateLandscape: Captured name '%s' (from override '%s')"),
         *CaptName, *NameOverride);

  return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("landscape.create"),
      [CaptComponentsX, CaptComponentsY, CaptComponentSizeQuads,
       CaptSubsectionSizeQuads, CaptNumSubsections, CaptHasSizeX, CaptHasSizeY,
       CaptSizeXUnits, CaptSizeYUnits, CaptLocation, CaptMaterialPath, CaptName]
      (const PinWrightSafePoint::FSafePointResponder& Responder) {
    if (!GEditor) return;
    UWorld *World = GEditor->GetEditorWorldContext().World();
    if (!World) return;

    // Keep the automatic-edit-layer creation (e.g. Water) from raising a blocking modal.
    FScopedLandscapeLayerDialog SuppressLayerDialog;

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ALandscape *Landscape =
        World->SpawnActor<ALandscape>(ALandscape::StaticClass(), CaptLocation,
                                      FRotator::ZeroRotator, SpawnParams);
    if (!Landscape) {
      Responder.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn landscape actor"));
      return;
    }

    // Assign a valid LandscapeGuid immediately after spawn, before any other actor
    // operation. A freshly spawned ALandscape has an invalid (zero) LandscapeGuid, and
    // operations like SetActorLabel below trigger landscape registration/build callbacks
    // that route through ALandscapeProxy::GetLandscapeInfo() -> ULandscapeInfo::Find(),
    // which asserts check(LandscapeGuid.IsValid()) (Landscape.cpp:4788 on UE 5.3). On 5.3
    // GetLandscapeInfo() is unguarded, so the editor dies before we ever reach the GUID
    // assignment if it lives further down. Setting it first keeps every later lookup valid
    // on all engine versions.
    if (!Landscape->GetLandscapeGuid().IsValid()) {
      Landscape->SetLandscapeGuid(FGuid::NewGuid());
    }

    // ---- sizeX / sizeY -> component count, against the REAL draw scale. ----------
    // A landscape's world extent is not free: it is
    // ComponentCount * ComponentSizeQuads * DrawScale, and only ComponentCount is an
    // integer this verb can choose. So a world-unit request is snapped to the nearest
    // reachable count (round, not floor: flooring made every request fall short by up
    // to a whole component on top of the divisor error, and floor(x/8064) is 0 -> 1 for
    // anything under 8064). The residual quantisation is reported in the response,
    // measured, so the caller can see the extent they actually got.
    //
    // The scale is read off the spawned actor rather than assumed: this handler sets no
    // scale, so the actor carries the ALandscapeProxy CDO's (128, 128, 256) -- NOT the
    // (100, 100, 100) the landscape editor's New Landscape panel defaults to
    // (ULandscapeEditorObject::NewLandscape_DefaultScale). Reading it keeps the
    // conversion true if either ever changes.
    const FVector SpawnDrawScale = Landscape->GetActorScale3D();
    const double DrawScaleX = FMath::Abs(SpawnDrawScale.X) > UE_DOUBLE_SMALL_NUMBER
                                  ? FMath::Abs(SpawnDrawScale.X) : 1.0;
    const double DrawScaleY = FMath::Abs(SpawnDrawScale.Y) > UE_DOUBLE_SMALL_NUMBER
                                  ? FMath::Abs(SpawnDrawScale.Y) : 1.0;
    const double ComponentSpanX = CaptComponentSizeQuads * DrawScaleX;
    const double ComponentSpanY = CaptComponentSizeQuads * DrawScaleY;

    int32 ComponentsX = CaptComponentsX;
    int32 ComponentsY = CaptComponentsY;
    if (CaptHasSizeX && ComponentSpanX > UE_DOUBLE_SMALL_NUMBER) {
      ComponentsX = FMath::Max(1, FMath::RoundToInt(CaptSizeXUnits / ComponentSpanX));
    }
    if (CaptHasSizeY && ComponentSpanY > UE_DOUBLE_SMALL_NUMBER) {
      ComponentsY = FMath::Max(1, FMath::RoundToInt(CaptSizeYUnits / ComponentSpanY));
    }

    // Set the shared landscape dimensions before any operation (SetActorLabel,
    // CreateLandscapeInfo, etc.) that can register the actor with a ULandscapeInfo.
    // The first RegisterActor seeds the info's ComponentSizeQuads/NumSubsections/
    // SubsectionSizeQuads from the proxy and every later registration asserts equality
    // (check(ComponentSizeQuads == Proxy->ComponentSizeQuads), Landscape.cpp:4966 on UE 5.3).
    // If these were left at their spawn defaults during an early registration and only set
    // afterwards, the second registration would mismatch and kill the editor.
    // All three fields come from the single consistent source computed above, so
    // ComponentSizeQuads == SubsectionSizeQuads * NumSubsections holds by
    // construction (the invariant ULandscapeComponent::Init asserts).
    Landscape->ComponentSizeQuads = CaptComponentSizeQuads;
    Landscape->SubsectionSizeQuads = CaptSubsectionSizeQuads;
    Landscape->NumSubsections = CaptNumSubsections;

    // Set the material before Import so the imported components pick it up; this
    // assignment alone does not register the actor.
    if (!CaptMaterialPath.IsEmpty()) {
      UMaterialInterface *Mat = LoadObject<UMaterialInterface>(nullptr, *CaptMaterialPath);
      if (Mat) {
        Landscape->LandscapeMaterial = Mat;
      }
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // 5.5+ MUST NOT register the actor (SetActorLabel / CreateLandscapeInfo) before
    // Import. Registration of a component-less, edit-layer-less ALandscape triggers
    // ULandscapeInfo::RegisterLandscapeActorWithProxyInternal ->
    // ALandscape::ConvertNonEditLayerLandscape, which auto-creates a default edit
    // layer while LandscapeComponents is still empty. ALandscapeProxy::AddLayer then
    // initializes per-component FLandscapeLayerComponentData by looping over an EMPTY
    // component array, so no layer data is created. Import later builds the components
    // but, finding edit layers already present, takes its reimport-into-edit-layers
    // branch and asserts check(ComponentLayerData != nullptr) (LandscapeEdit.cpp:3917)
    // because those freshly built components never got default-layer data
    // (B-landscape-create-hollow-no-components crash). Deferring all registration to
    // Import keeps the order correct: Import builds components first, then its own
    // RegisterAllComponents() triggers the conversion AFTER components exist, so
    // AddLayer populates each component's default-layer data. The actor label is set
    // after Import below.
#else
    if (!CaptName.IsEmpty()) {
      Landscape->SetActorLabel(CaptName);
    } else {
      Landscape->SetActorLabel(FString::Printf(TEXT("Landscape_%dx%d"), ComponentsX, ComponentsY));
    }

    Landscape->CreateLandscapeInfo();
#endif

    // The heightmap vertex grid spans the whole-component quad count, not the
    // per-subsection size: each component contributes CaptComponentSizeQuads
    // quads along each axis.
    const int32 VertX = ComponentsX * CaptComponentSizeQuads + 1;
    const int32 VertY = ComponentsY * CaptComponentSizeQuads + 1;
    TArray<uint16> HeightArray;
    HeightArray.Init(32768, VertX * VertY);

    const int32 InMinX = 0;
    const int32 InMinY = 0;
    const int32 InMaxX = ComponentsX * CaptComponentSizeQuads;
    const int32 InMaxY = ComponentsY * CaptComponentSizeQuads;

    TMap<FGuid, TArray<uint16>> ImportHeightData;
    ImportHeightData.Add(FGuid(), HeightArray);
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> ImportLayerInfos;
    ImportLayerInfos.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
    TArray<FLandscapeLayer> EditLayers;

    {
      const FScopedTransaction Transaction(FText::FromString(TEXT("Create Landscape")));
      Landscape->Modify();

      // Build the ULandscapeComponent grid via ALandscapeProxy::Import on EVERY
      // engine version. Import is the only path that actually instantiates and
      // registers the component grid: it sets ComponentSizeQuads/NumSubsections/
      // SubsectionSizeQuads and loops NewObject<ULandscapeComponent> + Init(...),
      // populating LandscapeComponents and ULandscapeInfo::XYtoComponentMap
      // (engine LandscapeEdit.cpp ALandscapeProxy::Import). The earlier 5.5+/5.7
      // branches only did CreateDefaultLayer() + FLandscapeEditDataInterface::
      // SetHeightData, which writes through XYtoComponentMap into ALREADY-existing
      // components and never creates any. With no components Import produces, the
      // SetHeightData call was a guaranteed no-op: the spawned ALandscape stayed
      // hollow (zero ULandscapeComponents, empty XYtoComponentMap, all-zero
      // bounding box), so create reported success:true while every downstream
      // sculpt/edit/flatten/paint RPC was dead on the actor
      // (B-landscape-create-hollow-no-components).
      //
      // Import is NOT deprecated on UE 5.7 (LandscapeProxy.h:1397-1400 carries no
      // UE_DEPRECATED), and the legacy path already relied on it on older
      // versions, so a single unified call is correct on 5.3-5.7. The
      // PRAGMA_DISABLE_DEPRECATION_WARNINGS wrap is retained only to keep the
      // older engines (where the symbol may be deprecation-marked) warning-clean;
      // it is a no-op on 5.7.
      //
      // Reuse the GUID already assigned above (and registered via
      // CreateLandscapeInfo) rather than minting a fresh one: Import calls
      // SetLandscapeGuid internally, and a new GUID here would orphan the
      // ULandscapeInfo registered under the original GUID, leaving later
      // GetLandscapeInfo() lookups without a matching entry.
      PRAGMA_DISABLE_DEPRECATION_WARNINGS
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
      // 5.5+ takes the edit layers as a const TArrayView<const FLandscapeLayer>&
      // (a required argument). TArray<FLandscapeLayer> converts implicitly to the
      // view; passing the (empty) EditLayers directly yields an empty view, which
      // is the same "no edit layers" semantics the pre-5.5 nullptr expressed.
      Landscape->Import(Landscape->GetLandscapeGuid(), InMinX, InMinY, InMaxX, InMaxY, CaptNumSubsections, CaptSubsectionSizeQuads, ImportHeightData, nullptr, ImportLayerInfos, ELandscapeImportAlphamapType::Layered, EditLayers);
#else
      // Pre-5.5 takes the edit layers as a const TArray<FLandscapeLayer>* that
      // defaults to nullptr; pass nullptr when there are no layers to import.
      Landscape->Import(Landscape->GetLandscapeGuid(), InMinX, InMinY, InMaxX, InMaxY, CaptNumSubsections, CaptSubsectionSizeQuads, ImportHeightData, nullptr, ImportLayerInfos, ELandscapeImportAlphamapType::Layered, EditLayers.Num() > 0 ? &EditLayers : nullptr);
#endif
      PRAGMA_ENABLE_DEPRECATION_WARNINGS

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
      // Only create the default edit layer if Import did not already establish one.
      // On 5.5+ Import's RegisterAllComponents() triggers the non-edit-layer
      // auto-conversion (which creates and populates the default layer over the
      // now-existing components), so LandscapeEditLayers is non-empty here; calling
      // CreateDefaultLayer() again would hit its check(LandscapeEditLayers.Num() == 0)
      // and crash. On engines where Import leaves no layer, this still creates one.
      //
      // Edit-layer count accessor differs by engine: 5.6+ renamed the getter to
      // GetLayersConst() (and deprecated GetLayerCount()), while 5.5 only has the
      // non-deprecated GetLayerCount(). Branch so each engine uses its live API.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
      if (Landscape->GetLayersConst().Num() == 0) {
        Landscape->CreateDefaultLayer();
      }
#else
      if (Landscape->GetLayerCount() == 0) {
        Landscape->CreateDefaultLayer();
      }
#endif
#else
      Landscape->CreateDefaultLayer();
#endif
    }

    if (CaptName.IsEmpty()) {
      Landscape->SetActorLabel(FString::Printf(TEXT("Landscape_%dx%d"), ComponentsX, ComponentsY));
    } else {
      Landscape->SetActorLabel(CaptName);
      UE_LOG(LogPinWrightSubsystem, Display,
             TEXT("HandleCreateLandscape: Set ActorLabel to '%s'"), *CaptName);
    }

    if (!CaptMaterialPath.IsEmpty()) {
      UMaterialInterface *Mat = LoadObject<UMaterialInterface>(nullptr, *CaptMaterialPath);
      if (Mat) {
        // Same property-named notification landscape.set_material uses — a bare
        // PostEditChange() here skipped the material branch too. See the helper.
        SetLandscapeMaterialAndNotify(Landscape, Mat);
      }
    }

    if (Landscape->GetRootComponent() && !Landscape->GetRootComponent()->IsRegistered()) {
      Landscape->RegisterAllComponents();
    }

    if (IsValid(Landscape)) {
      Landscape->PostEditChange();
    }

    // ---- Measure the extent the caller actually got. --------------------------
    // A caller who asked in world units has to be answered in world units, and the
    // answer has to be a measurement rather than an echo of the request: the response
    // used to carry component and quad counts only, so an extent 8x the one asked for
    // read exactly like a correct one. The quad extent comes from ULandscapeInfo (the
    // built component grid, so a landscape that came out hollow measures nothing
    // rather than reporting the size it was supposed to be), the draw scale from the
    // actor.
    const FVector MeasuredScale = Landscape->GetActorScale3D();
    double MeasuredSizeX = 0.0, MeasuredSizeY = 0.0;
    bool bExtentMeasured = false;
    if (ULandscapeInfo *Info = Landscape->GetLandscapeInfo()) {
      int32 ExtMinX = 0, ExtMinY = 0, ExtMaxX = 0, ExtMaxY = 0;
      if (Info->GetLandscapeExtent(ExtMinX, ExtMinY, ExtMaxX, ExtMaxY)) {
        MeasuredSizeX = (ExtMaxX - ExtMinX) * FMath::Abs(MeasuredScale.X);
        MeasuredSizeY = (ExtMaxY - ExtMinY) * FMath::Abs(MeasuredScale.Y);
        bExtentMeasured = true;
      }
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("landscapePath"), Landscape->GetPathName());
    Resp->SetStringField(TEXT("actorLabel"), Landscape->GetActorLabel());
    Resp->SetNumberField(TEXT("componentsX"), ComponentsX);
    Resp->SetNumberField(TEXT("componentsY"), ComponentsY);
    // Report the actual derived geometry so the caller can see how the inputs
    // mapped onto the engine fields (quadsPerComponent is the per-subsection
    // size; componentSizeQuads is the whole-component quad count).
    Resp->SetNumberField(TEXT("quadsPerComponent"), CaptSubsectionSizeQuads);
    Resp->SetNumberField(TEXT("subsectionSizeQuads"), CaptSubsectionSizeQuads);
    Resp->SetNumberField(TEXT("numSubsections"), CaptNumSubsections);
    Resp->SetNumberField(TEXT("componentSizeQuads"), CaptComponentSizeQuads);

    // The scale that turns quads into centimetres. Publishing it is what lets a
    // caller reproduce the conversion instead of guessing at a divisor.
    TSharedPtr<FJsonObject> DrawScaleJson = MakeShared<FJsonObject>();
    DrawScaleJson->SetNumberField(TEXT("x"), MeasuredScale.X);
    DrawScaleJson->SetNumberField(TEXT("y"), MeasuredScale.Y);
    DrawScaleJson->SetNumberField(TEXT("z"), MeasuredScale.Z);
    Resp->SetObjectField(TEXT("drawScale"), DrawScaleJson);
    Resp->SetBoolField(TEXT("worldExtentMeasured"), bExtentMeasured);
    // Omitted, never zeroed, when the grid could not be measured: a 0 here would be
    // indistinguishable from a measured empty landscape.
    if (bExtentMeasured) {
      Resp->SetNumberField(TEXT("worldSizeX"), MeasuredSizeX);
      Resp->SetNumberField(TEXT("worldSizeY"), MeasuredSizeY);
    } else {
      Warnings.Add(MakeShared<FJsonValueString>(TEXT(
          "The landscape's world extent could not be measured (no ULandscapeInfo extent for the "
          "built component grid), so worldSizeX / worldSizeY are omitted rather than reported as "
          "0. Read the extent back with actor.get_bounding_box before computing against it.")));
    }

    // sizeX / sizeY are requests; name them as such beside the measurement, and say so
    // whenever the quantisation to whole components moved the result.
    if (CaptHasSizeX) {
      Resp->SetNumberField(TEXT("requestedSizeX"), CaptSizeXUnits);
    }
    if (CaptHasSizeY) {
      Resp->SetNumberField(TEXT("requestedSizeY"), CaptSizeYUnits);
    }
    if (bExtentMeasured &&
        ((CaptHasSizeX && !FMath::IsNearlyEqual(MeasuredSizeX, CaptSizeXUnits, 0.5)) ||
         (CaptHasSizeY && !FMath::IsNearlyEqual(MeasuredSizeY, CaptSizeYUnits, 0.5)))) {
      Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
          "Requested %.1f x %.1f cm, built %.1f x %.1f cm. A landscape's extent is quantised to "
          "whole components (%.1f x %.1f cm each: componentSizeQuads %d x drawScale %.3f / %.3f), "
          "so the request was snapped to the nearest reachable size — %d x %d components. Use "
          "componentsX / componentsY to pick the count directly; the world size follows from it."),
          CaptHasSizeX ? CaptSizeXUnits : MeasuredSizeX,
          CaptHasSizeY ? CaptSizeYUnits : MeasuredSizeY,
          MeasuredSizeX, MeasuredSizeY, ComponentSpanX, ComponentSpanY,
          CaptComponentSizeQuads, DrawScaleX, DrawScaleY, ComponentsX, ComponentsY)));
    }

    if (Warnings.Num() > 0) {
      Resp->SetArrayField(TEXT("warnings"), Warnings);
    }

    Responder.SendSuccess(TEXT("Landscape created successfully"), Resp);
  });
}

// ---- landscape.sculpt ----
// The summary below claims the stroke boundary CURVES rather than stepping along cell
// edges. That is a statement about this verb's own output, so it verifies nothing on its
// own: landscape.audit_shape (end of this file) is what measures it, on this verb's output
// or on any other terrain.
//
// A swept-brush height edit. The unit of work is a STROKE, not a stamp: the caller hands
// over the curve they are drawing and the verb rasterises it in one call.
//
// What this replaced, and why (rpc-design.md §8, "when the unit of work has a shape, take
// the shape as a parameter"): the previous verb took one `location`, rounded it to the
// nearest heightfield vertex with `FMath::RoundToInt`, and evaluated one circular
// piecewise-linear falloff around it. Drawing a traced boundary therefore meant ~40 round
// trips per 20,000 uu at the default radius, each one re-snapped to the lattice, and the
// continuity BETWEEN the stamps - the thing that makes an edge read as one cliff rather
// than a row of scallops - fell in the gaps between the calls where no per-call parameter
// could reach it. Two terrains were rejected on sight for the 90-degree treads that
// produces.
//
// Three structural changes carry the fix:
//
//  1. **The centre is fractional** (LandscapeBrush.h). Nothing rounds unless the caller
//     asks for it with `snapToVertex`, and when they do the response says so.
//  2. **The stroke is a distance field, not a series of stamps.** A vertex's brush weight
//     comes from its distance to the nearest point of the POLYLINE, so the edited region
//     is the exact offset curve of the path. There is no sample spacing to tune, no
//     accumulation where consecutive stamps overlap (which is what beads a raise), and
//     the boundary is a smooth curve rasterised at sub-cell accuracy.
//  3. **Cost stays linear in path length.** Each segment sweeps only its own clamped
//     footprint into a shared max-alpha buffer, so a 200-segment stroke is 200 small
//     rectangles rather than 200 passes over the union box.
//
// The reporting half was the worse half and is fixed here too:
//
//  * `modifiedVertices` used to be `HeightData.Num()` - the size of the clamped rectangle
//    the brush covered. A stamp that moved terrain by nothing reported the same number as
//    one that worked (rpc-design.md §16 lists it in the table of metrics that could not
//    fail). It is now the count of vertices whose stored height DIFFERS, measured by
//    reading the heightmap back through a fresh FLandscapeEditDataInterface after the
//    write has flushed and settled - not by counting the writer's own intentions. That
//    distinction is load-bearing on this engine (§4): an edit-layer write that lands in no
//    persistent layer is composited away by the regeneration, and the planned-vs-measured
//    disagreement is the only thing that can see it.
//  * An unrecognised `toolMode` used to match nothing, leave the delta at zero, and return
//    `success: true` with 0. It is now LANDSCAPE_INVALID_TOOL_MODE, listing the valid
//    spellings.
//  * A call that changed nothing is LANDSCAPE_SCULPT_NO_CHANGE rather than a zero-item
//    success (§3, the NO_ACTORS_MATCHED rule) - a mistyped radius, a strength of 0 or a
//    stroke off the edge of the landscape otherwise looks exactly like a clean run. Pass
//    `allowNoChange: true` when a no-op is a legitimate outcome you want to observe.
//  * `brushRadius` used to be converted through ScaleX alone, so the brush was an ellipse
//    on any landscape with ScaleX != ScaleY while the response reported a circle. Distance
//    is now measured in world centimetres through both draw scales.
//  * `skipFlush` deferred only the settle here while the identically-named parameter on
//    landscape.edit also skipped the flush itself. The honest spelling is `deferSettle`;
//    `skipFlush` still works and now says in `warnings[]` that it means the narrower
//    thing, and the response reports `flushed` / `settled` / `verified` as measured facts.
REGISTER_RPC_HANDLER("landscape.sculpt", "landscape", "Sweep a sculpt brush (Raise / Lower / Flatten / Smooth) along a world-space path, or stamp it at a single world location, in ONE call. Brush weight comes from each vertex's distance to the nearest point of the path, so the edited region is the exact offset curve of the stroke and its boundary curves rather than stepping along cell edges; the centre is sub-cell accurate unless snapToVertex is set. modifiedVertices is a real post-write heightmap readback count, not the size of the brush rectangle, and a call that changes nothing is refused with LANDSCAPE_SCULPT_NO_CHANGE. Prefer landscape.edit when you already have a heightfield to install.",
    RPC_PARAMS(
        RPC_PARAM_OPT("landscapePath", "path", "Object path to a specific landscape asset; if omitted the actor is resolved by landscapeName."),
        RPC_PARAM_OPT("landscapeName", "string", "Display label of the landscape actor in the level. Used when landscapePath is empty."),
        RPC_PARAM_OPT("location", "object", "World location {x, y, z} of a single brush stamp, in centimeters. Mutually exclusive with 'path'; exactly one of the two is required."),
        RPC_PARAM_OPT("position", "object", "Alias for 'location'."),
        RPC_PARAM_OPT("path", "array", "World-space polyline [{x, y, z}, ...] in centimeters to sweep the brush along, applied in one call. brushRadius is the stroke half-width. Each point's z is the Flatten target at that point and is interpolated along the stroke, so one call can cut a graded bed or a sloped road. Mutually exclusive with 'location'."),
        RPC_PARAM_OPT("toolMode", "string", "Sculpt operation: 'Raise', 'Lower', 'Flatten' or 'Smooth'. Defaults to 'Raise'. An unrecognised value is refused with LANDSCAPE_INVALID_TOOL_MODE, never silently ignored."),
        RPC_PARAM_OPT("brushRadius", "number", "Brush radius (stroke half-width) in world centimeters. Defaults to 1000. Measured in world units through both X and Y draw scales, so the brush is a true circle on a non-uniformly scaled landscape."),
        RPC_PARAM_OPT("brushFalloff", "number", "Fraction 0..1 of the radius occupied by the falloff ramp: 0 is a hard edge, 1 ramps from the centre. Defaults to 0.5."),
        RPC_PARAM_OPT("falloffProfile", "string", "Ramp shape: 'linear' (default, the historical ramp), 'smooth' (smoothstep, alias 'smoothstep'), 'spherical' (dome shoulder) or 'tip' (spike). The four curves are the engine's own Landscape Ed Mode brush falloffs."),
        RPC_PARAM_OPT("strength", "number", "Per-call strength multiplier. For Raise/Lower the peak vertex moves strength*100 centimeters; for Flatten/Smooth it is the 0..1 fraction of the way to the target. Defaults to 0.1."),
        RPC_PARAM_OPT("heightDelta", "number", "Raise/Lower only: move the peak of the stroke by exactly this many world centimeters, instead of deriving it from strength. Sign is taken from toolMode, so a positive value lowers under 'Lower'."),
        RPC_PARAM_OPT("targetHeight", "number", "Flatten only: world Z in centimeters to flatten toward, overriding the z of the location/path points."),
        RPC_PARAM_OPT("snapToVertex", "boolean", "Round the brush centre to the nearest heightfield vertex (the pre-2026-08 behaviour). Defaults to false — sub-cell placement is the default and is what keeps a traced boundary from stepping. Echoed back as snappedToVertex."),
        RPC_PARAM_OPT("smoothRadiusVerts", "number", "Smooth only: half-width in heightfield vertices of the averaging kernel. Defaults to 2. This is a local box filter over the read-back heights, not the engine's Smooth tool."),
        RPC_PARAM_OPT("deferSettle", "boolean", "Defer the edit-layer settle so a burst of calls pays it once. The heightmap flush still runs. Deferring the settle also disables the post-write readback, so the response reports verified=false and omits the measured counts rather than emitting zeros. Defaults to false."),
        RPC_PARAM_OPT("skipFlush", "boolean", "Deprecated alias for deferSettle. Named for a flush it never skipped; kept working, and a warning says so."),
        RPC_PARAM_OPT("allowNoChange", "boolean", "Accept a call that changed no vertex as a success instead of refusing it with LANDSCAPE_SCULPT_NO_CHANGE. Defaults to false.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("landscape.sculpt payload missing"));
    return true;
  }

  FString LandscapePath;
  Payload->TryGetStringField(TEXT("landscapePath"), LandscapePath);
  FString LandscapeName;
  Payload->TryGetStringField(TEXT("landscapeName"), LandscapeName);

  if (!LandscapePath.IsEmpty()) {
    FString SafePath = SanitizeProjectRelativePath(LandscapePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe landscape path: %s"), *LandscapePath));
      return true;
    }
    LandscapePath = SafePath;
  }

  // ---- The stroke. `location`/`position` is the one-point degenerate case and goes
  //      through the identical code below, so the point form and the path form cannot
  //      drift apart (rpc-design.md §2, one writer for the shape).
  TArray<FVector> WorldPath;
  const TArray<TSharedPtr<FJsonValue>>* PathArray = nullptr;
  const bool bHasPath = Payload->TryGetArrayField(TEXT("path"), PathArray) && PathArray;
  const TSharedPtr<FJsonObject>* LocObj = nullptr;
  const bool bHasLocation =
      (Payload->TryGetObjectField(TEXT("location"), LocObj) && LocObj) ||
      (Payload->TryGetObjectField(TEXT("position"), LocObj) && LocObj);

  if (bHasPath && bHasLocation) {
    // Two different shapes for the same edit; picking one would silently discard the
    // caller's other intent.
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("Pass either 'location' (a single stamp) or 'path' (a polyline swept in one call), not both."));
    return true;
  }

  if (bHasPath) {
    if (PathArray->Num() == 0) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          TEXT("'path' is empty. Pass at least one {x, y, z} point, or use 'location' for a single stamp."));
      return true;
    }
    // Bounded so a pathological document cannot make the sweep unbounded; the cost of a
    // stroke is (segments x footprint) and both halves are capped (rpc-design.md §9).
    constexpr int32 MaxPathPoints = 4096;
    if (PathArray->Num() > MaxPathPoints) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("'path' has %d points; the maximum is %d. Split the stroke across calls."),
              PathArray->Num(), MaxPathPoints));
      return true;
    }
    WorldPath.Reserve(PathArray->Num());
    for (int32 i = 0; i < PathArray->Num(); ++i) {
      const TSharedPtr<FJsonObject>* PtObj = nullptr;
      if (!(*PathArray)[i].IsValid() || !(*PathArray)[i]->TryGetObject(PtObj) || !PtObj) {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("path[%d] is not an object. Each point is {x, y, z} in world centimeters."), i));
        return true;
      }
      double Px = 0, Py = 0, Pz = 0;
      (*PtObj)->TryGetNumberField(TEXT("x"), Px);
      (*PtObj)->TryGetNumberField(TEXT("y"), Py);
      (*PtObj)->TryGetNumberField(TEXT("z"), Pz);
      WorldPath.Add(FVector(Px, Py, Pz));
    }
  } else if (bHasLocation) {
    double LocX = 0, LocY = 0, LocZ = 0;
    (*LocObj)->TryGetNumberField(TEXT("x"), LocX);
    (*LocObj)->TryGetNumberField(TEXT("y"), LocY);
    (*LocObj)->TryGetNumberField(TEXT("z"), LocZ);
    WorldPath.Add(FVector(LocX, LocY, LocZ));
  } else {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("location or path required. Example: {\"location\": {\"x\": 0, \"y\": 0, \"z\": 100}} or {\"path\": [{\"x\":0,\"y\":0,\"z\":0},{\"x\":5000,\"y\":2000,\"z\":0}]}"));
    return true;
  }

  // ---- Enums: an unrecognised spelling is refused, never defaulted (rpc-design.md §3).
  FString ToolModeStr = TEXT("Raise");
  Payload->TryGetStringField(TEXT("toolMode"), ToolModeStr);
  PinWright::LandscapeBrush::EToolMode ToolMode = PinWright::LandscapeBrush::EToolMode::Raise;
  if (!PinWright::LandscapeBrush::ParseToolMode(ToolModeStr, ToolMode)) {
    TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
    ErrData->SetStringField(TEXT("toolMode"), ToolModeStr);
    TArray<TSharedPtr<FJsonValue>> Valid;
    Valid.Add(MakeShared<FJsonValueString>(TEXT("Raise")));
    Valid.Add(MakeShared<FJsonValueString>(TEXT("Lower")));
    Valid.Add(MakeShared<FJsonValueString>(TEXT("Flatten")));
    Valid.Add(MakeShared<FJsonValueString>(TEXT("Smooth")));
    ErrData->SetArrayField(TEXT("validToolModes"), Valid);
    Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_INVALID_TOOL_MODE,
        FString::Printf(TEXT("Unrecognised toolMode '%s'. Valid values: %s."),
            *ToolModeStr, *PinWright::LandscapeBrush::ValidToolModes()),
        ErrData);
    return true;
  }

  FString FalloffProfileStr = TEXT("linear");
  Payload->TryGetStringField(TEXT("falloffProfile"), FalloffProfileStr);
  PinWright::LandscapeBrush::EFalloffProfile FalloffProfile = PinWright::LandscapeBrush::EFalloffProfile::Linear;
  if (!PinWright::LandscapeBrush::ParseFalloffProfile(FalloffProfileStr, FalloffProfile)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("Unrecognised falloffProfile '%s'. Valid values: %s."),
            *FalloffProfileStr, *PinWright::LandscapeBrush::ValidFalloffProfiles()));
    return true;
  }

  double BrushRadius = 1000.0;
  Payload->TryGetNumberField(TEXT("brushRadius"), BrushRadius);
  if (!(BrushRadius > 0.0)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("brushRadius must be greater than 0 (got %f). It is the stroke half-width in world centimeters."), BrushRadius));
    return true;
  }

  double BrushFalloff = 0.5;
  Payload->TryGetNumberField(TEXT("brushFalloff"), BrushFalloff);
  BrushFalloff = FMath::Clamp(BrushFalloff, 0.0, 1.0);

  double Strength = 0.1;
  Payload->TryGetNumberField(TEXT("strength"), Strength);

  double HeightDeltaCm = 0.0;
  const bool bHasHeightDelta = Payload->TryGetNumberField(TEXT("heightDelta"), HeightDeltaCm);

  double TargetHeightCm = 0.0;
  const bool bHasTargetHeight = Payload->TryGetNumberField(TEXT("targetHeight"), TargetHeightCm);

  bool bSnapToVertex = false;
  Payload->TryGetBoolField(TEXT("snapToVertex"), bSnapToVertex);

  int32 SmoothRadiusVerts = 2;
  Payload->TryGetNumberField(TEXT("smoothRadiusVerts"), SmoothRadiusVerts);
  SmoothRadiusVerts = FMath::Clamp(SmoothRadiusVerts, 1, 32);

  bool bDeferSettle = false;
  const bool bUsedLegacySkipFlush = Payload->TryGetBoolField(TEXT("skipFlush"), bDeferSettle);
  bool bDeferSettleExplicit = false;
  if (Payload->TryGetBoolField(TEXT("deferSettle"), bDeferSettleExplicit)) {
    bDeferSettle = bDeferSettleExplicit;
  }

  bool bAllowNoChange = false;
  Payload->TryGetBoolField(TEXT("allowNoChange"), bAllowNoChange);

  return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("landscape.sculpt"),
      [LandscapePath, LandscapeName, WorldPath = MoveTemp(WorldPath), ToolMode, ToolModeStr,
       FalloffProfile, BrushRadius, BrushFalloff, Strength, bHasHeightDelta, HeightDeltaCm,
       bHasTargetHeight, TargetHeightCm, bSnapToVertex, SmoothRadiusVerts, bDeferSettle,
       bUsedLegacySkipFlush, bAllowNoChange]
      (const PinWrightSafePoint::FSafePointResponder& Responder) {
    using namespace PinWright::LandscapeBrush;

    FString ErrorMsg;
    ALandscape* Landscape = FindLandscapeByNameOrPath(LandscapePath, LandscapeName, ErrorMsg);
    if (!Landscape) {
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NOT_FOUND, ErrorMsg);
      return;
    }

    ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
    if (!LandscapeInfo) {
      Responder.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Landscape has no info"));
      return;
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (bUsedLegacySkipFlush) {
      Warnings.Add(MakeShared<FJsonValueString>(TEXT(
          "'skipFlush' on landscape.sculpt defers only the edit-layer settle — the heightmap flush always runs, "
          "unlike the identically-named parameter on landscape.edit. Use 'deferSettle', which says what it does. "
          "Deferring the settle also disables the post-write readback, so modifiedVertices is not measured.")));
    }

    const FVector ActorScale = Landscape->GetActorScale3D();
    const double ScaleX = FMath::Abs(ActorScale.X) > UE_DOUBLE_SMALL_NUMBER ? FMath::Abs(ActorScale.X) : 1.0;
    const double ScaleY = FMath::Abs(ActorScale.Y) > UE_DOUBLE_SMALL_NUMBER ? FMath::Abs(ActorScale.Y) : 1.0;
    const double ScaleZ = FMath::Abs(ActorScale.Z) > UE_DOUBLE_SMALL_NUMBER ? ActorScale.Z : 1.0;
    // 1 raw heightmap unit is ScaleZ/128 world centimetres; this is its inverse.
    const double RawPerCm = 128.0 / ScaleZ;

    if (FMath::Abs(FMath::Abs(ActorScale.X) - FMath::Abs(ActorScale.Y)) > UE_DOUBLE_KINDA_SMALL_NUMBER) {
      Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
          "Landscape draw scale is non-uniform in XY (%.4f x %.4f). brushRadius is measured in world "
          "centimeters through both axes, so the brush is a circle in the world and spans a different "
          "number of heightfield vertices along each axis."), ActorScale.X, ActorScale.Y)));
    }

    // ---- Stroke into landscape-local fractional vertex coordinates. -----------------
    // Nothing rounds here unless the caller asked for it; that single change is what
    // stops a shallow diagonal snapping to the lattice.
    const FTransform ActorTransform = Landscape->GetActorTransform();
    TArray<FStrokeVertex> Stroke;
    Stroke.Reserve(WorldPath.Num());
    for (const FVector& WorldPoint : WorldPath) {
      const FVector Local = ActorTransform.InverseTransformPosition(WorldPoint);
      FStrokeVertex V;
      V.X = bSnapToVertex ? (double)FMath::RoundToInt(Local.X) : Local.X;
      V.Y = bSnapToVertex ? (double)FMath::RoundToInt(Local.Y) : Local.Y;
      V.WorldZ = WorldPoint.Z;
      Stroke.Add(V);
    }

    int32 LMinX = 0, LMinY = 0, LMaxX = 0, LMaxY = 0;
    const bool bHasExtent = LandscapeInfo->GetLandscapeExtent(LMinX, LMinY, LMaxX, LMaxY);
    if (!bHasExtent) {
      if (LandscapeInfo->XYtoComponentMap.Num() == 0) {
        Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NO_COMPONENTS,
            FString::Printf(TEXT("Landscape '%s' has no registered ULandscapeComponents (hollow landscape), so it has no editable extent. Recreate it via landscape.create."),
                *Landscape->GetActorLabel()));
        return;
      }
      Responder.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Failed to get landscape extent"));
      return;
    }

    // ---- Union footprint of the whole stroke, clamped to the landscape. --------------
    double UMinX = TNumericLimits<double>::Max(), UMinY = TNumericLimits<double>::Max();
    double UMaxX = TNumericLimits<double>::Lowest(), UMaxY = TNumericLimits<double>::Lowest();
    const int32 SegmentCount = FMath::Max(1, Stroke.Num() - 1);
    for (int32 s = 0; s < SegmentCount; ++s) {
      const FStrokeVertex& A = Stroke[s];
      const FStrokeVertex& B = Stroke[FMath::Min(s + 1, Stroke.Num() - 1)];
      double SegMinX, SegMinY, SegMaxX, SegMaxY;
      SegmentBounds(A, B, BrushRadius, ScaleX, ScaleY, SegMinX, SegMinY, SegMaxX, SegMaxY);
      UMinX = FMath::Min(UMinX, SegMinX);
      UMinY = FMath::Min(UMinY, SegMinY);
      UMaxX = FMath::Max(UMaxX, SegMaxX);
      UMaxY = FMath::Max(UMaxY, SegMaxY);
    }

    // Smooth reads a neighbourhood, so its source rectangle is wider than its brush.
    const int32 ReadMargin = (ToolMode == EToolMode::Smooth) ? SmoothRadiusVerts : 0;
    int32 MinX = FMath::Max(FMath::FloorToInt(UMinX) - ReadMargin, LMinX);
    int32 MinY = FMath::Max(FMath::FloorToInt(UMinY) - ReadMargin, LMinY);
    int32 MaxX = FMath::Min(FMath::CeilToInt(UMaxX) + ReadMargin, LMaxX);
    int32 MaxY = FMath::Min(FMath::CeilToInt(UMaxY) + ReadMargin, LMaxY);

    if (MinX > MaxX || MinY > MaxY) {
      Responder.SendError(ErrorCodes::ERR_OUT_OF_BOUNDS,
          FString::Printf(TEXT("Stroke lies entirely outside the landscape. Landscape extent is heightmap pixels x[%d..%d] y[%d..%d]; the stroke footprint at brushRadius %.1f spans x[%.1f..%.1f] y[%.1f..%.1f]."),
              LMinX, LMaxX, LMinY, LMaxY, BrushRadius, UMinX, UMaxX, UMinY, UMaxY));
      return;
    }

    const int64 SizeX = (int64)(MaxX - MinX + 1);
    const int64 SizeY = (int64)(MaxY - MinY + 1);
    const int64 VertexCount = SizeX * SizeY;

    // rpc-design.md §9: bound a synchronous verb on its cost driver and state the
    // arithmetic in the refusal rather than making the caller guess where the wall is.
    constexpr int64 MaxVertices = 4194304;  // 2048 x 2048 heightfield vertices
    if (VertexCount > MaxVertices) {
      Responder.SendError(ErrorCodes::ERR_OUT_OF_BOUNDS,
          FString::Printf(TEXT("Stroke footprint is %lld x %lld = %lld heightfield vertices, over the %lld limit for one synchronous call. Reduce brushRadius (%.1f) or split the path; the handler cannot yield mid-write, so a larger sweep would outlive its own response timeout."),
              SizeX, SizeY, VertexCount, MaxVertices, BrushRadius));
      return;
    }

    const int32 Width = (int32)SizeX;
    const int32 Count = (int32)VertexCount;

    // ---- Pre-edit snapshot. This is the baseline the honest count is measured against.
    TArray<uint16> Before;
    Before.SetNumZeroed(Count);
    {
      int32 RMinX = MinX, RMinY = MinY, RMaxX = MaxX, RMaxY = MaxY;
      FLandscapeEditDataInterface ReadInterface(LandscapeInfo, /*bUploadTextureChangesToGPU=*/false);
      // Pre-edit snapshot: a read. The write below dirties on its own, and a sculpt
      // refused with LANDSCAPE_SCULPT_NO_CHANGE must not leave the level dirty.
      MakeLandscapeEditInterfaceReadOnly(ReadInterface);
      ReadInterface.GetHeightData(RMinX, RMinY, RMaxX, RMaxY, Before.GetData(), 0);
    }

    // ---- Sweep the stroke into a max-alpha field. -----------------------------------
    // Max rather than sum: overlapping coverage must not compound, or a raise beads
    // where consecutive parts of the stroke overlap — exactly the artifact a caller
    // looping single stamps could not avoid.
    TArray<float> Alpha;
    Alpha.SetNumZeroed(Count);
    TArray<float> TargetWorldZ;
    if (ToolMode == EToolMode::Flatten) {
      TargetWorldZ.SetNumZeroed(Count);
    }

    for (int32 s = 0; s < SegmentCount; ++s) {
      const FStrokeVertex& A = Stroke[s];
      const FStrokeVertex& B = Stroke[FMath::Min(s + 1, Stroke.Num() - 1)];
      double SegMinX, SegMinY, SegMaxX, SegMaxY;
      SegmentBounds(A, B, BrushRadius, ScaleX, ScaleY, SegMinX, SegMinY, SegMaxX, SegMaxY);

      // Each segment visits only its own footprint, so total cost is linear in path
      // length rather than (segments x union area).
      const int32 SegX0 = FMath::Max(FMath::FloorToInt(SegMinX), MinX);
      const int32 SegX1 = FMath::Min(FMath::CeilToInt(SegMaxX), MaxX);
      const int32 SegY0 = FMath::Max(FMath::FloorToInt(SegMinY), MinY);
      const int32 SegY1 = FMath::Min(FMath::CeilToInt(SegMaxY), MaxY);

      for (int32 Y = SegY0; Y <= SegY1; ++Y) {
        for (int32 X = SegX0; X <= SegX1; ++X) {
          double DistUu = 0.0, ClosestZ = 0.0;
          ClosestPointOnSegment((double)X, (double)Y, A, B, ScaleX, ScaleY, DistUu, ClosestZ);
          const float A2 = EvaluateFalloff(FalloffProfile, DistUu, BrushRadius, BrushFalloff);
          if (A2 <= 0.0f) {
            continue;
          }
          const int32 Index = (Y - MinY) * Width + (X - MinX);
          if (A2 > Alpha[Index]) {
            Alpha[Index] = A2;
            if (TargetWorldZ.Num() > 0) {
              TargetWorldZ[Index] = (float)ClosestZ;
            }
          }
        }
      }
    }

    // ---- Apply. ---------------------------------------------------------------------
    TArray<uint16> Planned = Before;
    const double ActorZ = Landscape->GetActorLocation().Z;
    // Raise/Lower move the peak by heightDelta centimetres when given, else by the
    // historical strength*100 centimetres — unchanged so existing calls keep their shape.
    const double PeakRawDelta = bHasHeightDelta ? (FMath::Abs(HeightDeltaCm) * RawPerCm)
                                                : (Strength * 100.0 * RawPerCm);
    int32 VerticesInBrush = 0;

    for (int32 Index = 0; Index < Count; ++Index) {
      const float W = Alpha[Index];
      if (W <= 0.0f) {
        continue;
      }
      ++VerticesInBrush;

      const double Current = (double)Before[Index];
      double Delta = 0.0;

      switch (ToolMode) {
      case EToolMode::Raise:
        Delta = PeakRawDelta * W;
        break;
      case EToolMode::Lower:
        Delta = -PeakRawDelta * W;
        break;
      case EToolMode::Flatten: {
        const double TargetZ = bHasTargetHeight ? TargetHeightCm : (double)TargetWorldZ[Index];
        const double TargetRaw = (TargetZ - ActorZ) / ScaleZ * 128.0 + 32768.0;
        Delta = (TargetRaw - Current) * Strength * W;
        break;
      }
      case EToolMode::Smooth: {
        // Local box average over the read-back neighbourhood. Deliberately NOT the
        // engine's FLandscapeToolStrokeSmooth, which lives inside the LandscapeEditor
        // module behind an FEdModeLandscape and is not reachable headlessly; saying so
        // is better than implying a capability the verb does not have.
        const int32 Vx = MinX + (Index % Width);
        const int32 Vy = MinY + (Index / Width);
        double Sum = 0.0;
        int32 Samples = 0;
        for (int32 Ky = -SmoothRadiusVerts; Ky <= SmoothRadiusVerts; ++Ky) {
          const int32 Sy = Vy + Ky;
          if (Sy < MinY || Sy > MaxY) { continue; }
          for (int32 Kx = -SmoothRadiusVerts; Kx <= SmoothRadiusVerts; ++Kx) {
            const int32 Sx = Vx + Kx;
            if (Sx < MinX || Sx > MaxX) { continue; }
            Sum += (double)Before[(Sy - MinY) * Width + (Sx - MinX)];
            ++Samples;
          }
        }
        if (Samples > 0) {
          Delta = ((Sum / (double)Samples) - Current) * FMath::Clamp(Strength, 0.0, 1.0) * W;
        }
        break;
      }
      }

      Planned[Index] = (uint16)FMath::Clamp(FMath::RoundToInt(Current + Delta), 0, 65535);
    }

    int32 PlannedChanged = 0;
    for (int32 Index = 0; Index < Count; ++Index) {
      if (Planned[Index] != Before[Index]) {
        ++PlannedChanged;
      }
    }

    // ---- Build the response skeleton shared by the success and the no-change paths, so
    //      a refusal carries the same diagnostics a success does. -----------------------
    auto BuildBaseResponse = [&]() {
      TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
      R->SetStringField(TEXT("toolMode"), ToolModeName(ToolMode));
      R->SetStringField(TEXT("falloffProfile"), FalloffProfileName(FalloffProfile));
      R->SetNumberField(TEXT("brushRadius"), BrushRadius);
      R->SetNumberField(TEXT("brushFalloff"), BrushFalloff);
      R->SetBoolField(TEXT("snappedToVertex"), bSnapToVertex);
      R->SetNumberField(TEXT("pathPointCount"), Stroke.Num());
      R->SetNumberField(TEXT("segmentCount"), Stroke.Num() > 1 ? Stroke.Num() - 1 : 0);
      R->SetNumberField(TEXT("verticesInBrush"), VerticesInBrush);
      R->SetNumberField(TEXT("verticesConsidered"), Count);
      R->SetNumberField(TEXT("plannedVertices"), PlannedChanged);

      // The fractional centre actually used, so a caller can see that nothing snapped.
      TSharedPtr<FJsonObject> Centre = MakeShared<FJsonObject>();
      Centre->SetNumberField(TEXT("x"), Stroke[0].X);
      Centre->SetNumberField(TEXT("y"), Stroke[0].Y);
      R->SetObjectField(TEXT("firstCentreVertexFractional"), Centre);

      TSharedPtr<FJsonObject> RegionObj = MakeShared<FJsonObject>();
      RegionObj->SetNumberField(TEXT("minX"), MinX);
      RegionObj->SetNumberField(TEXT("minY"), MinY);
      RegionObj->SetNumberField(TEXT("maxX"), MaxX);
      RegionObj->SetNumberField(TEXT("maxY"), MaxY);
      R->SetObjectField(TEXT("region"), RegionObj);
      return R;
    };

    // ---- A stroke that plans no change never reaches the write. ---------------------
    // rpc-design.md §3: an empty result set is an error, not a zero-item success. A
    // mistyped radius, strength 0, a Flatten already at its target and a stroke off the
    // edge all look identical to a clean run otherwise.
    if (PlannedChanged == 0 && !bAllowNoChange) {
      TSharedPtr<FJsonObject> ErrData = BuildBaseResponse();
      ErrData->SetNumberField(TEXT("modifiedVertices"), 0);
      ErrData->SetBoolField(TEXT("changed"), false);
      FString Why;
      if (VerticesInBrush == 0) {
        Why = TEXT("no heightfield vertex fell inside the brush — brushRadius may be smaller than one cell, or the stroke may lie between vertices");
      } else if (ToolMode == EToolMode::Flatten) {
        Why = TEXT("every vertex under the brush already sits at the flatten target");
      } else if (ToolMode == EToolMode::Smooth) {
        Why = TEXT("the heights under the brush are already locally flat, so smoothing has nothing to move");
      } else if (!bHasHeightDelta && FMath::IsNearlyZero(Strength)) {
        Why = TEXT("strength is 0, so the computed delta rounds to no height units");
      } else {
        Why = TEXT("the computed delta rounds to less than one raw height unit at every vertex under the brush");
      }
      ErrData->SetStringField(TEXT("reason"), Why);
      if (Warnings.Num() > 0) {
        ErrData->SetArrayField(TEXT("warnings"), Warnings);
      }
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_SCULPT_NO_CHANGE,
          FString::Printf(TEXT("Sculpt would change no vertex: %s. %d of %d vertices in the region fell under the brush. Pass allowNoChange=true if a no-op is an acceptable outcome here."),
              *Why, VerticesInBrush, Count),
          ErrData);
      return;
    }

    // ---- Write. ---------------------------------------------------------------------
    bool bFlushed = false;
    if (PlannedChanged > 0) {
      int32 WMinX = MinX, WMinY = MinY, WMaxX = MaxX, WMaxY = MaxY;
      // bUploadTextureChangesToGPU = true: on edit-layer landscapes the deferred
      // regeneration MERGES the edit-layer heightmap on the GPU, so Flush() must upload
      // the CPU write first or the merge composites stale data and the write is lost.
      FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo, /*bUploadTextureChangesToGPU=*/true);
      const FGuid EditLayerGuid = GetDefaultEditLayerGuid(Landscape);
      {
        // Target the landscape's default edit layer and request an all-modes
        // regeneration when the scope CLOSES; outside Landscape Ed Mode nothing is being
        // edited, so without this the shared-layer write lands in no persistent layer.
        FScopedSetLandscapeEditingLayer EditingLayerScope(Landscape, EditLayerGuid,
            [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
        LandscapeEdit.SetHeightData(WMinX, WMinY, WMaxX, WMaxY, Planned.GetData(), 0, true);
        LandscapeEdit.Flush();
        bFlushed = true;
      }
      Landscape->MarkPackageDirty();
    }

    // ---- Verify against the heightmap, not against the buffer we just wrote. --------
    // rpc-design.md §4: the check has to read something the write path cannot fake. A
    // fresh FLandscapeEditDataInterface after the settle asks the engine what the
    // heightmap now holds; the planned buffer only says what this handler intended. On
    // an edit-layer landscape those two genuinely disagree when the write lands in no
    // persistent layer and the regeneration composites it away — which is the failure
    // this verb previously reported as a full-rectangle success.
    bool bSettled = false;
    bool bVerified = false;
    int32 MeasuredChanged = 0;
    double MaxAbsDeltaRaw = 0.0;
    double SumAbsDeltaRaw = 0.0;

    if (!bDeferSettle) {
      SettleLandscapeLayers(Landscape);
      bSettled = true;

      TArray<uint16> After;
      After.SetNumZeroed(Count);
      int32 VMinX = MinX, VMinY = MinY, VMaxX = MaxX, VMaxY = MaxY;
      FLandscapeEditDataInterface VerifyInterface(LandscapeInfo, /*bUploadTextureChangesToGPU=*/false);
      // Verification readback: a read. Landscape->MarkPackageDirty() above already
      // recorded the write, so this must add nothing of its own.
      MakeLandscapeEditInterfaceReadOnly(VerifyInterface);
      VerifyInterface.GetHeightData(VMinX, VMinY, VMaxX, VMaxY, After.GetData(), 0);
      bVerified = true;

      for (int32 Index = 0; Index < Count; ++Index) {
        if (After[Index] != Before[Index]) {
          ++MeasuredChanged;
          const double D = FMath::Abs((double)After[Index] - (double)Before[Index]);
          MaxAbsDeltaRaw = FMath::Max(MaxAbsDeltaRaw, D);
          SumAbsDeltaRaw += D;
        }
      }
    }

    TSharedPtr<FJsonObject> Resp = BuildBaseResponse();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("flushed"), bFlushed);
    Resp->SetBoolField(TEXT("settled"), bSettled);
    Resp->SetBoolField(TEXT("verified"), bVerified);

    if (bVerified) {
      // Measured. `modifiedVertices` keeps its name because callers read it, but it is
      // now a readback count rather than the rectangle's area.
      Resp->SetNumberField(TEXT("modifiedVertices"), MeasuredChanged);
      Resp->SetBoolField(TEXT("changed"), MeasuredChanged > 0);
      Resp->SetNumberField(TEXT("maxHeightDeltaCm"), MaxAbsDeltaRaw * ScaleZ / 128.0);
      Resp->SetNumberField(TEXT("meanAbsHeightDeltaCm"),
          MeasuredChanged > 0 ? (SumAbsDeltaRaw / (double)MeasuredChanged) * ScaleZ / 128.0 : 0.0);

      if (MeasuredChanged == 0 && PlannedChanged > 0) {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "Planned %d changed vertices but the heightmap reads back unchanged. On an edit-layer landscape "
            "this is the signature of a write that landed in no persistent layer and was composited away by "
            "the regeneration; the terrain has NOT moved."), PlannedChanged)));
      } else if (MeasuredChanged < PlannedChanged) {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT(
            "Planned %d changed vertices, measured %d after the settle. Some of the write did not survive the "
            "edit-layer merge, or was clamped at the 0/65535 height limits."), PlannedChanged, MeasuredChanged)));
      }
    } else {
      // rpc-design.md §1: an unmeasured report omits its numbers rather than emitting
      // zeros that read as measurements. `plannedVertices` is still there and is honestly
      // named — it is what the handler intended, not what the heightmap holds.
      Resp->SetBoolField(TEXT("changed"), PlannedChanged > 0);
      Warnings.Add(MakeShared<FJsonValueString>(TEXT(
          "Settle deferred, so no post-write readback was taken: modifiedVertices, maxHeightDeltaCm and "
          "meanAbsHeightDeltaCm are omitted rather than reported as zero. Issue the last call of the burst "
          "without deferSettle, or confirm with landscape.get_heights.")));
    }

    if (Warnings.Num() > 0) {
      Resp->SetArrayField(TEXT("warnings"), Warnings);
    }
    AddActorVerification(Resp, Landscape);

    Responder.SendSuccess(TEXT("Landscape sculpted"), Resp);
  });
}

// ---- landscape.set_material ----
REGISTER_RPC_HANDLER("landscape.set_material", "landscape", "Replace the LandscapeMaterial on a landscape actor with the named material asset and rebuild the per-component material instances, which is what makes a landscape material edit reach the screen. Re-assigning the material the landscape already holds is a supported refresh, not a no-op. Triggers a recompile of the landscape proxies; can be slow on large landscapes. Response echoes previousMaterialPath and changed so a refresh is distinguishable from a real assignment.",
    RPC_PARAMS(
        RPC_PARAM_OPT("landscapePath", "path", "Object path of the landscape; either this or landscapeName must be provided."),
        RPC_PARAM_OPT("landscapeName", "string", "Display label of the landscape actor in the level."),
        RPC_PARAM_REQ("materialPath", "path", "Object path to the UMaterialInterface to assign as the landscape material.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("set_landscape_material payload missing"));
    return true;
  }

  FString LandscapePath;
  Payload->TryGetStringField(TEXT("landscapePath"), LandscapePath);
  FString LandscapeName;
  Payload->TryGetStringField(TEXT("landscapeName"), LandscapeName);
  FString MaterialPath;
  if (!Payload->TryGetStringField(TEXT("materialPath"), MaterialPath) || MaterialPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("materialPath required"));
    return true;
  }

  FString SafeMaterialPath = SanitizeProjectRelativePath(MaterialPath);
  if (SafeMaterialPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
        FString::Printf(TEXT("Invalid or unsafe material path: %s"), *MaterialPath));
    return true;
  }
  MaterialPath = SafeMaterialPath;

  return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("landscape.set_material"),
      [LandscapePath, LandscapeName, MaterialPath]
      (const PinWrightSafePoint::FSafePointResponder& Responder) {
    FString ErrorMsg;
    ALandscape* Landscape = FindLandscapeByNameOrPath(LandscapePath, LandscapeName, ErrorMsg);
    if (!Landscape) {
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NOT_FOUND, ErrorMsg);
      return;
    }

    UMaterialInterface *Mat = Cast<UMaterialInterface>(
        StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *MaterialPath, nullptr, LOAD_NoWarn));
    if (!Mat) {
      if (!ResolveAsset(MaterialPath).bExists) {
        Responder.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Material asset not found: %s"), *MaterialPath));
      } else {
        Responder.SendError(ErrorCodes::ERR_LOAD_FAILED,
            TEXT("Failed to load material (invalid type?)"));
      }
      return;
    }

    // Siblings sculpt / edit / create_procedural_terrain all MarkPackageDirty; this
    // one did not, so a material assignment survived only until the editor closed.
    // Before the write so the Modify() is state-neutral if the verb is ever wrapped
    // in a transaction (landscape.create already uses one).
    PinWright::MarkLevelActorModified(Landscape);

    // Reported so a caller can tell a real assignment from a refresh of the material
    // already held. Both rebuild the component MICs — re-assigning the same material
    // is the normal iteration step — but the response used to echo only the requested
    // path, so the two were indistinguishable.
    UMaterialInterface* PreviousMat = Landscape->LandscapeMaterial;
    const bool bMaterialPointerChanged = (PreviousMat != Mat);

    // Names the LandscapeMaterial property in the change event, which is what makes
    // the engine rebuild the per-component material instances. See the helper.
    SetLandscapeMaterialAndNotify(Landscape, Mat);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
    Resp->SetStringField(TEXT("landscapeName"), Landscape->GetActorLabel());
    Resp->SetStringField(TEXT("materialPath"), MaterialPath);
    Resp->SetStringField(TEXT("previousMaterialPath"),
        PreviousMat ? PreviousMat->GetPathName() : FString());
    Resp->SetBoolField(TEXT("changed"), bMaterialPointerChanged);

    Responder.SendSuccess(
        bMaterialPointerChanged ? TEXT("Landscape material set")
                                : TEXT("Landscape material re-applied (same material) and "
                                       "component material instances rebuilt"),
        Resp);
  });
}

// ---- landscape.create_grass_type ----
REGISTER_RPC_HANDLER("landscape.create_grass_type", "landscape", "Create a ULandscapeGrassType asset configured to scatter the given static mesh as grass. Reference this asset from a landscape grass node in the landscape material to render grass. `density` is the value stored on the asset; `effectiveDensity` is what the grass builder will actually use, because the grass.densityScale cvar (driven by sg.FoliageQuality - 0.4 at FoliageQuality@1, 0 at @0) multiplies it and the asset still reads back the unscaled number. `densityScalingEnabled` is the type's own bEnableDensityScaling opt-out (defaults true), `grassDensityScaleCVar` the measured cvar, and `cvarWarning` names the remedy when the two densities differ. effectiveDensity is OMITTED, never guessed, when the cvar is not in this host's registry. Does not change the cvar.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string",
            "BARE asset name for the new ULandscapeGrassType - not a path. A name carrying '/' (or "
            "any other character UObject naming rejects) is refused INVALID_ARGUMENT naming the "
            "engine's own reason; use savePath to choose the folder."),
        RPC_PARAM_REQ("meshPath", "path", "Object path to the UStaticMesh to scatter as grass instances."),
        RPC_PARAM_OPT("savePath", "path",
            "Content folder for the created grass type (default: /Game/Landscape). Must be under a "
            "mounted root; the effective folder is always echoed as `save_path`."),
        RPC_PARAM_OPT("density", "number", "Number of instances per 10m x 10m square. Defaults to 1.0."),
        RPC_PARAM_OPT("minScale", "number", "Minimum random scale applied per-instance. Defaults to 0.8."),
        RPC_PARAM_OPT("maxScale", "number", "Maximum random scale applied per-instance. Defaults to 1.2.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("create_landscape_grass_type payload missing"));
    return true;
  }

  FString Name;
  if (!Payload->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("name required"));
    return true;
  }

  // Destination folder. Spelled and validated exactly as foliage.add_type and
  // foliage.create_procedural spell it (savePath -> SanitizeProjectRelativePath ->
  // SECURITY_VIOLATION, effective folder echoed as save_path), so the three asset-creating verbs
  // in this family choose a folder the same way. It exists because a hardcoded destination is WHY
  // a caller put a path in `name` in the first place: refusing the path without offering this one
  // leaves the caller with nothing to try.
  FString PackagePath = TEXT("/Game/Landscape");
  FString RequestedSavePath;
  if (Payload->TryGetStringField(TEXT("savePath"), RequestedSavePath) &&
      !RequestedSavePath.IsEmpty()) {
    FString SafeSavePath = SanitizeProjectRelativePath(RequestedSavePath);
    if (SafeSavePath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe savePath: %s"), *RequestedSavePath));
      return true;
    }
    SafeSavePath.RemoveFromEnd(TEXT("/"));
    PackagePath = SafeSavePath;
  }

  // COMPOSED AND CHECKED HERE, on the calling thread and ahead of everything the operation body does,
  // for two reasons. (1) The path is what kills the process: `name` used to flow straight into a
  // Printf onto "/Game/Landscape" inside the operation body below and then into CreatePackage, and a
  // name beginning with '/' produced "/Game/Landscape//..." - CreatePackage's Fatal, i.e. editor
  // death, the shape measured on B-foliage-add-type-name-with-slash-kills-the-editor.
  // (2) The ordering is load-bearing for the regression test: it drives a slash-bearing name with
  // a meshPath that does not resolve, so on a build where this check is absent or moved below the
  // mesh load the call is refused for the WRONG reason and the test goes red - instead of reaching
  // CreatePackage and taking the test host down with it.
  const FString AssetName = Name;
  FString FullPackagePath;
  FString PathError;
  if (!PinWrightComposeAssetPackagePath(PackagePath, AssetName, FullPackagePath, PathError)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with savePath "
                             "(default /Game/Landscape)."), *PathError));
    return true;
  }

  FString MeshPath;
  if (!Payload->TryGetStringField(TEXT("meshPath"), MeshPath) || MeshPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("meshPath required"));
    return true;
  }

  FString SafeMeshPath = SanitizeProjectRelativePath(MeshPath);
  if (SafeMeshPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
        FString::Printf(TEXT("Invalid or unsafe mesh path: %s"), *MeshPath));
    return true;
  }
  MeshPath = SafeMeshPath;

  double Density = 1.0;
  Payload->TryGetNumberField(TEXT("density"), Density);
  double MinScale = 0.8;
  Payload->TryGetNumberField(TEXT("minScale"), MinScale);
  double MaxScale = 1.2;
  Payload->TryGetNumberField(TEXT("maxScale"), MaxScale);

  return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("landscape.create_grass_type"),
      [AssetName, PackagePath, FullPackagePath, MeshPath, Density, MinScale, MaxScale]
      (const PinWrightSafePoint::FSafePointResponder& Responder) {
    UStaticMesh *StaticMesh = Cast<UStaticMesh>(StaticLoadObject(
        UStaticMesh::StaticClass(), nullptr, *MeshPath, nullptr, LOAD_NoWarn));
    if (!StaticMesh) {
      Responder.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
          FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
      return;
    }

    // PackagePath / AssetName / FullPackagePath were composed and checked on the calling thread,
    // before this task was queued. Nothing in this lambda may rebuild the path from raw arguments.
    if (UObject *ExistingAsset = StaticLoadObject(
            ULandscapeGrassType::StaticClass(), nullptr, *FullPackagePath)) {
      TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("asset_path"), ExistingAsset->GetPathName());
      Resp->SetStringField(TEXT("message"), TEXT("Asset already exists"));
      // Echoed on this branch too: the folder the verb searched is the same fact whether it
      // created the asset or found one, and a caller that omitted savePath cannot otherwise tell
      // which folder answered.
      Resp->SetStringField(TEXT("save_path"), PackagePath);
      Responder.SendSuccess(TEXT("Landscape grass type already exists"), Resp);
      return;
    }

    UPackage *Package = CreatePackage(*FullPackagePath);
    ULandscapeGrassType *GrassType = NewObject<ULandscapeGrassType>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
    if (!GrassType) {
      Responder.SendError(ErrorCodes::ERR_CREATION_FAILED, TEXT("Failed to create grass type asset"));
      return;
    }

    // The variety MUST be default-constructed, not AddZeroed. FGrassVariety::FGrassVariety()
    // (Runtime/Landscape/Private/LandscapeGrass.cpp) is where every non-zero default lives, and
    // two of them are what the engine gates grass rendering on:
    //   - EndCullDistance / EndCullDistanceQuality (10000): the scatter loop only builds a
    //     cluster when GrassMesh && GetDensity() > 0 && GetEndCullDistance() > 0.
    //   - AllowedDensityRange (0,1) on 5.5+: both placement loops keep an instance only when
    //     Weight > Min && Weight <= Max, which no weight satisfies once it is memset to (0,0).
    // Zeroing them yields an asset that opens with populated-looking fields and scatters nothing.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    FGrassVariety& Variety = GrassType->GrassVarieties.AddDefaulted_GetRef();
#else
    // FGrassVariety is declared without LANDSCAPE_API before UE 5.4, so its out-of-line default
    // constructor is not exported and AddDefaulted_GetRef leaves an unresolved external in this
    // DLL. The generated StaticStruct() IS exported on every supported version, and
    // InitializeStruct runs that same constructor through the Landscape module's own struct ops.
    FGrassVariety& Variety = GrassType->GrassVarieties[GrassType->GrassVarieties.AddZeroed()];
    FGrassVariety::StaticStruct()->InitializeStruct(&Variety);
#endif
    Variety.GrassMesh = StaticMesh;
    // Both density slots carry the caller's value: GetDensity() reads GrassDensityQuality on
    // hosts with GEngine->UseGrassVarityPerQualityLevels and GrassDensity everywhere else, so
    // writing only one leaves the constructor's 400 in force on half the hosts.
    Variety.GrassDensity.Default = static_cast<float>(Density);
    Variety.GrassDensityQuality.Default = static_cast<float>(Density);
    Variety.ScaleX = FFloatInterval(static_cast<float>(MinScale), static_cast<float>(MaxScale));
    Variety.ScaleY = FFloatInterval(static_cast<float>(MinScale), static_cast<float>(MaxScale));
    Variety.ScaleZ = FFloatInterval(static_cast<float>(MinScale), static_cast<float>(MaxScale));
    Variety.RandomRotation = true;
    Variety.AlignToSurface = true;

    McpSafeAssetSave(GrassType);
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("asset_path"), GrassType->GetPathName());
    // Always echoed, supplied or defaulted - the destination used to be a literal a caller could
    // neither choose nor observe. Same field name foliage.add_type and foliage.create_procedural
    // publish.
    Resp->SetStringField(TEXT("save_path"), PackagePath);
    // Read the two numbers the engine gates on back off the stored variety rather than echoing
    // the request, so a variety that would scatter nothing is visible in the response instead of
    // hiding behind a bare success. Both are reported from .Default because the handler keeps the
    // per-platform and per-quality slots in agreement above.
    Resp->SetNumberField(TEXT("density"), Variety.GrassDensity.Default);
    Resp->SetNumberField(TEXT("end_cull_distance"), Variety.EndCullDistance.Default);

    // grass.densityScale does not veto the write, it RESCALES it, and the asset still reads back
    // the unscaled number - the spawn_sky_light shape from
    // B-lighting-writes-vetoed-by-scalability-cvars, one namespace over. FGrassBuilderBase
    // multiplies the variety's density by the cvar before any instance is scattered
    // (LandscapeGrass.cpp:2040-2041), and ULandscapeGrassType::bEnableDensityScaling - the only
    // opt-out - defaults TRUE (:1567), so a grass type created here IS scaled by default.
    // BaseScalability.ini drives the cvar from sg.FoliageQuality, which is 0.4 at @1: the echoed
    // `density` is then 2.5x the effective one, and 0 at @0 scatters nothing at all. `density` is
    // the stored value, `effectiveDensity` what the builder will use. Does not change the cvar.
    const bool bGrassOptedIn = GrassType->bEnableDensityScaling != 0;
    const PinWrightDensityScalability::FDensityScaleReading GrassScaleReading =
        PinWrightDensityScalability::ReadDensityScaleCVar(
            PinWrightDensityScalability::GrassDensityScaleCVarName);
    float GrassScale = 1.0f;
    const bool bGrassScaleMeasured = PinWrightDensityScalability::ResolveGrassDensityScale(
        GrassScaleReading, bGrassOptedIn, GrassScale);

    Resp->SetBoolField(TEXT("densityScalingEnabled"), bGrassOptedIn);
    Resp->SetObjectField(TEXT("grassDensityScaleCVar"),
        PinWrightDensityScalability::MakeDensityScaleCVarJson(
            PinWrightDensityScalability::GrassDensityScaleCVarName, GrassScaleReading));
    // Omitted rather than echoed unscaled when the cvar was not measured - an unmeasured
    // effective value would read as a measured one.
    if (bGrassScaleMeasured)
    {
      Resp->SetNumberField(TEXT("effectiveDensity"),
          Variety.GrassDensity.Default * GrassScale);
    }

    if (bGrassScaleMeasured && !FMath::IsNearlyEqual(GrassScale, 1.0f))
    {
      Resp->SetStringField(TEXT("cvarWarning"), FString::Printf(
          TEXT("grass.densityScale is %.4g, not 1, so the density written to this grass type is ")
          TEXT("NOT the density the grass builder will use: FGrassBuilderBase multiplies the ")
          TEXT("variety's density by it before scattering, and the asset still reads back the ")
          TEXT("unscaled value. Tuning grass density against a viewport measured now tunes ")
          TEXT("against a scaled builder%s. BaseScalability.ini drives this cvar from ")
          TEXT("sg.FoliageQuality (0 at @0, 0.4 at @1, 0.8 at @2, 1.0 at @3), so restore it for ")
          TEXT("this session with system.console_command \"sg.FoliageQuality 3\", or pin ")
          TEXT("grass.densityScale=1 under [SystemSettings] in the project's DefaultEngine.ini. ")
          TEXT("Clearing bEnableDensityScaling on the grass type exempts it instead. This verb ")
          TEXT("does not change the cvar."),
          GrassScale,
          GrassScale <= 0.0f ? TEXT(" - at 0 this grass type scatters NOTHING") : TEXT("")));
    }
    else if (!GrassScaleReading.bFound && bGrassOptedIn)
    {
      Resp->SetStringField(TEXT("cvarWarning"),
          TEXT("grass.densityScale is not in this host's console registry, so whether the written ")
          TEXT("density is the effective one was not measured. `density` here reports only the ")
          TEXT("stored value, and effectiveDensity is omitted."));
    }

    Responder.SendSuccess(TEXT("Landscape grass type created"), Resp);
  });
}

// ---- landscape.edit (modify_heightmap) ----
REGISTER_RPC_HANDLER("landscape.edit", "landscape", "Bulk-modify a landscape's heightmap by writing raw uint16 height samples (operation='set') or by applying raise/lower/flatten across a region. Coordinates are in landscape-local heightmap pixels, not world units.",
    RPC_PARAMS(
        RPC_PARAM_OPT("landscapePath", "path", "Object path of the landscape; either this or landscapeName must be provided."),
        RPC_PARAM_OPT("landscapeName", "string", "Display label of the landscape actor in the level."),
        RPC_PARAM_OPT("operation", "string", "Edit kind: 'set' (write heightData), 'raise', 'lower', 'flatten'. Defaults to 'set'."),
        RPC_PARAM_OPT("heightData", "array", "Row-major uint16 heightmap samples. Required for operation='set'; length must match the region size."),
        RPC_PARAM_OPT("region", "object", "Heightmap-pixel region {minX, minY, maxX, maxY} to modify. Defaults to the full landscape."),
        RPC_PARAM_OPT("skipFlush", "boolean", "Defer the GPU heightmap flush so multiple edit calls are batched. Defaults to false.")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("modify_heightmap payload missing"));
    return true;
  }

  FString LandscapePath;
  Payload->TryGetStringField(TEXT("landscapePath"), LandscapePath);
  FString LandscapeName;
  Payload->TryGetStringField(TEXT("landscapeName"), LandscapeName);

  if (!LandscapePath.IsEmpty()) {
    FString SafePath = SanitizeProjectRelativePath(LandscapePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe landscape path: %s"), *LandscapePath));
      return true;
    }
    LandscapePath = SafePath;
  }

  FString Operation = TEXT("set");
  Payload->TryGetStringField(TEXT("operation"), Operation);

  // Parse the optional region as explicit TOptionals so an absent field (default to full extent)
  // is distinguished from a legitimately-negative coordinate; the read verb (landscape.get_heights)
  // resolves the same way via LandscapeHeightStats::ResolveHeightRegion so the two never diverge.
  TOptional<int32> ReqMinX, ReqMinY, ReqMaxX, ReqMaxY;
  const TSharedPtr<FJsonObject> *RegionObj = nullptr;
  if (Payload->TryGetObjectField(TEXT("region"), RegionObj) && RegionObj) {
    int32 V;
    if ((*RegionObj)->TryGetNumberField(TEXT("minX"), V)) { ReqMinX = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("minY"), V)) { ReqMinY = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("maxX"), V)) { ReqMaxX = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("maxY"), V)) { ReqMaxY = V; }
  }

  const TArray<TSharedPtr<FJsonValue>> *HeightDataArray = nullptr;
  const bool bHasHeightData = Payload->TryGetArrayField(TEXT("heightData"), HeightDataArray) &&
                              HeightDataArray && HeightDataArray->Num() > 0;

  if (!bHasHeightData && Operation.Equals(TEXT("set"), ESearchCase::IgnoreCase)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("heightData array required for 'set' operation"));
    return true;
  }

  bool bSkipFlush = false;
  Payload->TryGetBoolField(TEXT("skipFlush"), bSkipFlush);

  TArray<uint16> HeightValues;
  if (bHasHeightData) {
    for (const TSharedPtr<FJsonValue> &Val : *HeightDataArray) {
      if (Val.IsValid() && Val->Type == EJson::Number) {
        HeightValues.Add(static_cast<uint16>(FMath::Clamp(Val->AsNumber(), 0.0, 65535.0)));
      }
    }
  }

  return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("landscape.edit"),
      [LandscapePath, LandscapeName, Operation, ReqMinX, ReqMinY, ReqMaxX, ReqMaxY,
       HeightValues = MoveTemp(HeightValues), bSkipFlush]
      (const PinWrightSafePoint::FSafePointResponder& Responder) {
    FString ErrorMsg;
    ALandscape* Landscape = FindLandscapeByNameOrPath(LandscapePath, LandscapeName, ErrorMsg);
    if (!Landscape) {
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NOT_FOUND, ErrorMsg);
      return;
    }

    ULandscapeInfo *LandscapeInfo = Landscape->GetLandscapeInfo();
    if (!LandscapeInfo) {
      Responder.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Landscape has no info"));
      return;
    }

    FScopedSlowTask SlowTask(2.0f, FText::FromString(TEXT("Modifying heightmap...")));

    int32 FullMinX, FullMinY, FullMaxX, FullMaxY;
    if (!LandscapeInfo->GetLandscapeExtent(FullMinX, FullMinY, FullMaxX, FullMaxY)) {
      // GetLandscapeExtent walks ULandscapeInfo::XYtoComponentMap; it returns
      // false when the map is empty, i.e. the landscape has no registered
      // ULandscapeComponents (a "hollow" landscape — e.g. a manually-spawned
      // ALandscape, a partially-loaded streaming proxy, or any actor whose grid
      // was never built/registered). Emit a diagnostic that names that cause and
      // a recovery instead of the bare extent symptom, which reads like a
      // transient/addressing failure and drives caller trial-and-error.
      if (LandscapeInfo->XYtoComponentMap.Num() == 0) {
        Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NO_COMPONENTS,
            FString::Printf(
                TEXT("Landscape '%s' has no registered ULandscapeComponents (hollow landscape — XYtoComponentMap is empty), so it has no editable extent. It was likely spawned without a component grid; recreate it via landscape.create or verify the actor before editing."),
                *Landscape->GetActorLabel()));
        return;
      }
      Responder.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Failed to get landscape extent"));
      return;
    }

    const LandscapeHeightStats::FResolvedHeightRegion Region = LandscapeHeightStats::ResolveHeightRegion(
        ReqMinX, ReqMinY, ReqMaxX, ReqMaxY, FullMinX, FullMinY, FullMaxX, FullMaxY);
    if (!Region.bValid) {
      Responder.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Empty height region after clamping to landscape extent"));
      return;
    }
    // Non-const: FLandscapeEditDataInterface::GetHeightData / SetHeightData take these by
    // int32& and write back the actual sampled sub-region.
    int32 MinX = Region.MinX;
    int32 MinY = Region.MinY;
    int32 MaxX = Region.MaxX;
    int32 MaxY = Region.MaxY;

    const int32 SizeX = (MaxX - MinX + 1);
    const int32 SizeY = (MaxY - MinY + 1);
    const int32 RegionSize = SizeX * SizeY;

    SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("Reading current heightmap data")));

    TArray<uint16> CurrentHeights;
    CurrentHeights.SetNumZeroed(RegionSize);
    FLandscapeEditDataInterface LandscapeEditRead(LandscapeInfo, false);
    // Read half of the read-modify-write. LandscapeEditWrite below carries the default
    // dirtying, and Landscape->MarkPackageDirty() runs unconditionally after the write.
    MakeLandscapeEditInterfaceReadOnly(LandscapeEditRead);
    LandscapeEditRead.GetHeightData(MinX, MinY, MaxX, MaxY, CurrentHeights.GetData(), 0);

    TArray<uint16> OutputHeights;
    OutputHeights.SetNumUninitialized(RegionSize);

    const uint16 SingleValue = HeightValues.Num() > 0 ? HeightValues[0] : 32768;
    const int16 Delta = static_cast<int16>(SingleValue) - 32768;

    int32 ModifiedCount = 0;
    for (int32 i = 0; i < RegionSize; ++i) {
      uint16 NewHeight = CurrentHeights[i];

      if (Operation.Equals(TEXT("raise"), ESearchCase::IgnoreCase)) {
        NewHeight = FMath::Clamp(static_cast<int32>(CurrentHeights[i]) + FMath::Abs(Delta) / 10, 0, 65535);
        ModifiedCount++;
      } else if (Operation.Equals(TEXT("lower"), ESearchCase::IgnoreCase)) {
        NewHeight = FMath::Clamp(static_cast<int32>(CurrentHeights[i]) - FMath::Abs(Delta) / 10, 0, 65535);
        ModifiedCount++;
      } else if (Operation.Equals(TEXT("flatten"), ESearchCase::IgnoreCase)) {
        NewHeight = SingleValue;
        ModifiedCount++;
      } else {
        if (HeightValues.Num() == RegionSize) {
          NewHeight = HeightValues[i];
        } else {
          NewHeight = SingleValue;
        }
        ModifiedCount++;
      }

      OutputHeights[i] = NewHeight;
    }

    SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("Writing heightmap data")));

    // bUploadTextureChangesToGPU = true (default): on edit-layer landscapes the GPU
    // merge reads the GPU edit-layer texture, so Flush() must upload the CPU write or
    // the merge composites stale data (same mechanism as landscape.sculpt).
    FLandscapeEditDataInterface LandscapeEditWrite(LandscapeInfo, /*bUploadTextureChangesToGPU=*/true);
    const FGuid EditLayerGuid = GetDefaultEditLayerGuid(Landscape);
    {
      // Target the landscape's default edit layer for the write (same reason as
      // landscape.sculpt): outside Landscape Ed Mode nothing is being edited, so the
      // shared-layer write would land in no persistent layer and the settle regen
      // would composite it away. On a non-layer landscape the GUID is invalid and
      // the scope is a no-op. The completion callback requests the regen on scope close.
      FScopedSetLandscapeEditingLayer EditingLayerScope(Landscape, EditLayerGuid,
          [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
      LandscapeEditWrite.SetHeightData(MinX, MinY, MaxX, MaxY, OutputHeights.GetData(), SizeX, false);
      if (!bSkipFlush) {
        SlowTask.EnterProgressFrame(1.0f, FText::FromString(TEXT("Flushing changes to GPU")));
        LandscapeEditWrite.Flush();
      }
    }
    if (!bSkipFlush) {
      // Settle the deferred edit-layer regen (requested by the scope's completion
      // callback above) so bounds refresh before return; see SettleLandscapeLayers.
      SettleLandscapeLayers(Landscape);
    }

    Landscape->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
    Resp->SetStringField(TEXT("landscapeName"), Landscape->GetActorLabel());
    Resp->SetStringField(TEXT("operation"), Operation);
    Resp->SetNumberField(TEXT("modifiedVertices"), ModifiedCount);
    Resp->SetNumberField(TEXT("regionSizeX"), SizeX);
    Resp->SetNumberField(TEXT("regionSizeY"), SizeY);
    Resp->SetBoolField(TEXT("flushSkipped"), bSkipFlush);
    AddActorVerification(Resp, Landscape);

    Responder.SendSuccess(TEXT("Heightmap modified successfully"), Resp);
  });
}

// ---- landscape.get_heights ----
REGISTER_RPC_HANDLER("landscape.get_heights", "landscape", "Read back a landscape's heightmap over a region — the read counterpart to the height-writing verbs landscape.sculpt / landscape.edit. Returns per-region raw uint16 min/max/mean height and their world-space Z (centimeters) so a sculpt/edit round-trip is verifiable per region (e.g. central region raised above baseline, pad flattened to a constant Z, ridge raised less than the hill); pass includeSamples=true for the raw row-major uint16 samples. Coordinates are landscape-local heightmap pixels, not world units. Every number returned describes measured terrain only: \"region\" is the rectangle actually sampled, \"requestedRegion\" is what was asked for, and samples with no landscape component behind them are omitted and counted in omittedSampleCount rather than reported at the engine's neutral fill value.",
    RPC_PARAMS(
        RPC_PARAM_OPT("landscapePath", "path", "Object path of the landscape; either this or landscapeName must be provided."),
        RPC_PARAM_OPT("landscapeName", "string", "Display label of the landscape actor in the level. Used when landscapePath is empty."),
        RPC_PARAM_OPT("region", "object", "Heightmap-pixel region {minX, minY, maxX, maxY} to sample. Each coordinate independently defaults to the corresponding full-landscape extent. A region that overhangs the extent is clamped, and the clamp is reported in warnings[] with the rectangle that was read; a region that lies entirely outside the extent is rejected with INVALID_ARGUMENT rather than collapsing onto the nearest edge pixel and reporting it as a measurement."),
        RPC_PARAM_DEF("includeSamples", "boolean", "Return the raw row-major uint16 height samples (capped by maxSamples). Defaults to false — only the region aggregates are returned.", "false"),
        RPC_PARAM_DEF("maxSamples", "number", "Cap on the number of raw samples returned when includeSamples=true. Defaults to 4096.", "4096")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("get_heights payload missing"));
    return true;
  }

  FString LandscapePath;
  Payload->TryGetStringField(TEXT("landscapePath"), LandscapePath);
  FString LandscapeName;
  Payload->TryGetStringField(TEXT("landscapeName"), LandscapeName);

  if (!LandscapePath.IsEmpty()) {
    FString SafePath = SanitizeProjectRelativePath(LandscapePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe landscape path: %s"), *LandscapePath));
      return true;
    }
    LandscapePath = SafePath;
  }

  // Parse the optional region as explicit TOptionals so an absent field (default to full extent)
  // is distinguished from a legitimately-negative coordinate; ResolveHeightRegion applies the
  // defaulting + clamp identically for the read and write verbs (see F-landscape-height-readback).
  TOptional<int32> ReqMinX, ReqMinY, ReqMaxX, ReqMaxY;
  const TSharedPtr<FJsonObject> *RegionObj = nullptr;
  if (Payload->TryGetObjectField(TEXT("region"), RegionObj) && RegionObj) {
    int32 V;
    if ((*RegionObj)->TryGetNumberField(TEXT("minX"), V)) { ReqMinX = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("minY"), V)) { ReqMinY = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("maxX"), V)) { ReqMaxX = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("maxY"), V)) { ReqMaxY = V; }
  }

  bool bIncludeSamples = false;
  Payload->TryGetBoolField(TEXT("includeSamples"), bIncludeSamples);
  int32 MaxSamples = 4096;
  Payload->TryGetNumberField(TEXT("maxSamples"), MaxSamples);
  // No negative-clamp needed: BuildHeightStatsJson treats any MaxSamples <= 0 as "return all".

  FString ErrorMsg;
  ALandscape* Landscape = FindLandscapeByNameOrPath(LandscapePath, LandscapeName, ErrorMsg);
  if (!Landscape) {
    Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NOT_FOUND, ErrorMsg);
    return true;
  }

  ULandscapeInfo *LandscapeInfo = Landscape->GetLandscapeInfo();
  if (!LandscapeInfo) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Landscape has no info"));
    return true;
  }

  int32 FullMinX, FullMinY, FullMaxX, FullMaxY;
  if (!LandscapeInfo->GetLandscapeExtent(FullMinX, FullMinY, FullMaxX, FullMaxY)) {
    // Same hollow-landscape diagnosis as landscape.edit: an empty XYtoComponentMap means the
    // actor has no registered ULandscapeComponents, hence no readable extent.
    if (LandscapeInfo->XYtoComponentMap.Num() == 0) {
      Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NO_COMPONENTS,
          FString::Printf(
              TEXT("Landscape '%s' has no registered ULandscapeComponents (hollow landscape — XYtoComponentMap is empty), so it has no readable extent. It was likely spawned without a component grid; recreate it via landscape.create or verify the actor before reading."),
              *Landscape->GetActorLabel()));
      return true;
    }
    Ctx.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Failed to get landscape extent"));
    return true;
  }

  const LandscapeHeightStats::FResolvedHeightRegion Region = LandscapeHeightStats::ResolveHeightRegion(
      ReqMinX, ReqMinY, ReqMaxX, ReqMaxY, FullMinX, FullMinY, FullMaxX, FullMaxY);
  // The rectangle the caller asked for, with any absent coordinate defaulted to the extent. Kept
  // separately because a read that did not cover the request has to say what the request was.
  const int32 WantMinX = ReqMinX.Get(FullMinX);
  const int32 WantMinY = ReqMinY.Get(FullMinY);
  const int32 WantMaxX = ReqMaxX.Get(FullMaxX);
  const int32 WantMaxY = ReqMaxY.Get(FullMaxY);
  if (!Region.bValid) {
    // A region wholly outside the extent used to survive this gate: the clamp pulled all four
    // coordinates onto the nearest edge, and the verb answered with a one-pixel reading of
    // terrain the caller never named. Both rectangles go into the refusal so it can be re-asked.
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(
            TEXT("Region [%d,%d]..[%d,%d] %s the landscape extent [%d,%d]..[%d,%d], so no height in it can be read."),
            WantMinX, WantMinY, WantMaxX, WantMaxY,
            Region.bOverlapsExtent ? TEXT("is empty after clamping to") : TEXT("lies entirely outside"),
            FullMinX, FullMinY, FullMaxX, FullMaxY));
    return true;
  }
  // const: this is the rectangle the engine is ASKED for. GetHeightData takes its bounds by
  // int32& and OVERWRITES them with the sub-rectangle it actually found components for, having
  // seeded them with INT_MAX / INT_MIN first — so echoing the post-call values published
  // 2147483647 as a coordinate on every read that found nothing. Request and result are now
  // separate variables and only the result decides what is reported as measured.
  const int32 AskMinX = Region.MinX;
  const int32 AskMinY = Region.MinY;
  const int32 AskMaxX = Region.MaxX;
  const int32 AskMaxY = Region.MaxY;

  const int32 SizeX = (AskMaxX - AskMinX + 1);
  const int32 SizeY = (AskMaxY - AskMinY + 1);
  const int32 RegionSize = SizeX * SizeY;

  // Same GetHeightData read the write path performs (the read half of landscape.edit's
  // read-modify-write), so no new access mechanism is introduced — but it is an EDIT
  // interface and by default it dirties the map package. This verb changes nothing, so
  // it must leave the dirty flag exactly as it found it: PRESERVE, never clear. Both
  // halves are needed and they do different jobs — MakeLandscapeEditInterfaceReadOnly
  // removes the known cause (Texture->Modify), the guard restores anything else that
  // dirties and logs that it had to. See the read-dirtying note at the top of this file.
  //
  // The guard is declared FIRST inside the block so it outlives the interface: the
  // interface's destructor runs Flush(), and the flags must be restored after that.
  TArray<uint16> Heights;
  Heights.SetNumZeroed(RegionSize);
  int32 GotMinX = AskMinX, GotMinY = AskMinY, GotMaxX = AskMaxX, GotMaxY = AskMaxY;
  {
    PinWright::PackageDirty::FScopedPackageDirtyRestore DirtyGuard;
    CaptureLandscapeReadDirtyState(DirtyGuard, Landscape, LandscapeInfo);

    FLandscapeEditDataInterface LandscapeEditRead(LandscapeInfo, false);
    MakeLandscapeEditInterfaceReadOnly(LandscapeEditRead);
    LandscapeEditRead.GetHeightData(GotMinX, GotMinY, GotMaxX, GotMaxY, Heights.GetData(), 0);
  }

  const double ScaleZ = Landscape->GetActorScale3D().Z;
  const double ActorLocationZ = Landscape->GetActorLocation().Z;

  const LandscapeHeightStats::FMeasuredHeightRegion Measured =
      LandscapeHeightStats::ResolveMeasuredRegion(AskMinX, AskMinY, AskMaxX, AskMaxY,
          GotMinX, GotMinY, GotMaxX, GotMaxY);
  if (!Measured.bAnyMeasured) {
    // Nothing in the buffer is terrain — it is all CalcMissingValues fill, and the neutral fill
    // value 32768 converts to the actor's own Z plane, which reads exactly like flat ground.
    // A refusal is the only honest answer: there is no measurement to report.
    Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NO_HEIGHT_DATA,
        FString::Printf(
            TEXT("No landscape component covers region [%d,%d]..[%d,%d] on '%s', so all %d samples came back as interpolated fill rather than terrain (the neutral fill publishes as world Z %.1f, indistinguishable from flat ground). Landscape extent is [%d,%d]..[%d,%d]."),
            AskMinX, AskMinY, AskMaxX, AskMaxY, *Landscape->GetActorLabel(), RegionSize,
            LandscapeHeightStats::HeightToWorldZ(32768.0, ScaleZ, ActorLocationZ),
            FullMinX, FullMinY, FullMaxX, FullMaxY));
    return true;
  }

  // Only the measured sub-rectangle is terrain. Trim to it rather than publishing the fill under
  // a flag: dropping the fabricated samples keeps heights[] a rectangle of real data and keeps
  // min/max/mean honest, and what was dropped is counted in the response instead of inferred.
  TArray<uint16> Samples;
  int32 SampledSizeX = SizeX;
  int32 SampledSizeY = SizeY;
  if (Measured.bFullyMeasured) {
    Samples = MoveTemp(Heights);
  } else {
    SampledSizeX = Measured.MaxX - Measured.MinX + 1;
    SampledSizeY = Measured.MaxY - Measured.MinY + 1;
    if (!LandscapeHeightStats::ExtractSubRegion(Heights, AskMinX, AskMinY, SizeX, SizeY,
            Measured.MinX, Measured.MinY, Measured.MaxX, Measured.MaxY, Samples)) {
      Ctx.SendError(ErrorCodes::ERR_HEIGHT_READ_FAILED,
          FString::Printf(
              TEXT("GetHeightData reported measuring [%d,%d]..[%d,%d], which is not inside the [%d,%d]..[%d,%d] buffer it was given."),
              Measured.MinX, Measured.MinY, Measured.MaxX, Measured.MaxY,
              AskMinX, AskMinY, AskMaxX, AskMaxY));
      return true;
    }
  }

  FString StatsError;
  TSharedPtr<FJsonObject> Resp = LandscapeHeightStats::BuildHeightStatsJson(
      Samples, SampledSizeX, SampledSizeY, ScaleZ, ActorLocationZ, bIncludeSamples, MaxSamples, StatsError);
  if (!Resp.IsValid()) {
    Ctx.SendError(ErrorCodes::ERR_HEIGHT_READ_FAILED, StatsError);
    return true;
  }

  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
  Resp->SetStringField(TEXT("landscapeName"), Landscape->GetActorLabel());
  // "region" is what the numbers above describe: the rectangle that was actually measured.
  TSharedPtr<FJsonObject> RegionResp = MakeShared<FJsonObject>();
  RegionResp->SetNumberField(TEXT("minX"), Measured.MinX);
  RegionResp->SetNumberField(TEXT("minY"), Measured.MinY);
  RegionResp->SetNumberField(TEXT("maxX"), Measured.MaxX);
  RegionResp->SetNumberField(TEXT("maxY"), Measured.MaxY);
  Resp->SetObjectField(TEXT("region"), RegionResp);
  // "requestedRegion" is what was asked for. Always emitted, so a caller assembling a heightmap
  // can compare the two without having to remember its own request.
  TSharedPtr<FJsonObject> RequestedResp = MakeShared<FJsonObject>();
  RequestedResp->SetNumberField(TEXT("minX"), WantMinX);
  RequestedResp->SetNumberField(TEXT("minY"), WantMinY);
  RequestedResp->SetNumberField(TEXT("maxX"), WantMaxX);
  RequestedResp->SetNumberField(TEXT("maxY"), WantMaxY);
  Resp->SetObjectField(TEXT("requestedRegion"), RequestedResp);
  // Computed in double because the requested rectangle is unclamped caller input and its area can
  // exceed int64 before the clamp; sampleCount above already counts only the measured samples.
  const double RequestedSamples = ((double)WantMaxX - (double)WantMinX + 1.0) *
                                  ((double)WantMaxY - (double)WantMinY + 1.0);
  Resp->SetNumberField(TEXT("requestedSampleCount"), RequestedSamples);
  Resp->SetNumberField(TEXT("omittedSampleCount"),
      RequestedSamples - (double)Measured.MeasuredSampleCount);

  TArray<TSharedPtr<FJsonValue>> Warnings;
  if (Region.bClamped) {
    Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
        TEXT("Requested region was clamped to the landscape extent [%d,%d]..[%d,%d]; read [%d,%d]..[%d,%d]."),
        FullMinX, FullMinY, FullMaxX, FullMaxY, AskMinX, AskMinY, AskMaxX, AskMaxY)));
  }
  if (!Measured.bFullyMeasured) {
    Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
        TEXT("%lld of %d samples in [%d,%d]..[%d,%d] had no landscape component and came back as interpolated fill; they are excluded rather than reported as heights. Measured [%d,%d]..[%d,%d]."),
        Measured.FabricatedSampleCount, RegionSize, AskMinX, AskMinY, AskMaxX, AskMaxY,
        Measured.MinX, Measured.MinY, Measured.MaxX, Measured.MaxY)));
  }
  if (Warnings.Num() > 0) {
    Resp->SetArrayField(TEXT("warnings"), Warnings);
  }

  Ctx.SendSuccess(Resp);
  return true;
}

// ---- The ALLOCATION side of the layer census (B-paint-erases-orphaned-layer) ----
//
// Every other "which layers does this landscape have" question in this file is answered from the
// REGISTRATION side — ULandscapeInfo::Layers, or the names the material declares. An ORPHANED
// layer is exactly the one that side cannot see: weight is allocated on the components and no
// registration points at it, so a registration-side enumeration reports it as ABSENT right up
// until the moment it is destroyed. That is why the all-layers census in
// landscape.create_procedural_terrain can report otherLayerTexelsLost 0 over a landscape-wide
// erase — it enumerates LandscapeInfo->Layers and additionally skips entries with a null
// LayerInfoObj, so it is blind twice over. This walk enumerates what the weightmaps actually
// CONTAIN instead.
//
// The predicate is the merge's own, not an approximation of it. PerformLayersWeightmapsBatchedMerge
// takes RequestedWeightmapLayerNames = ALandscape::GetTargetLayerNames(/*visibility=*/true)
// (LandscapeEditLayers.cpp:5883), which Algo::TransformIf's LandscapeInfo->Layers under
// LayerInfoObj != nullptr (LandscapeEdit.cpp:8330-8336); those names resolve back to LayerInfo
// pointers, and ReallocateLayersWeightmaps then removes every component allocation whose
// Alloc.LayerInfo is not among them (:5738-5757) — by POINTER identity, which is why this
// compares pointers rather than names. The removal reaches every component of every loaded proxy
// and is independent of `region` and of `strength`; its only trace is a VeryVerbose log line.
//
// Two shapes, both caught here:
//   A. Live but unregistered LayerInfo — what the Target Layers panel's RENAME produces
//      (ALandscapeProxy::RemoveTargetLayer iterates no components, Landscape.cpp:7725-7746), what
//      a delete with World Partition proxies unloaded leaves behind, and what any tool that paints
//      without CreateTargetLayerSettingsFor mints (FLandscapeEditDataInterface::SetAlphaData
//      creates the allocation with no registration check at all). This weight is real, readable,
//      and is what a paint destroys.
//   B. Null LayerInfo — ULandscapeInfo::ReplaceLayer(X, nullptr) assigns
//      Allocation.LayerInfo = nullptr (LandscapeEditInterface.cpp:1723), leaving an allocation
//      that still holds a texture channel with no object behind it. Nothing samples it and
//      GetWeightDataFast has no object to read it by, so it is REPORTED and never refused on.
//
// Cost: components x allocations pointer comparisons against a TSet, over resident TArrays of
// 10-byte structs. Nothing dereferences a texture, streams, or touches disk. It is deliberately
// NOT gated on `verify` and NOT gated on the census texel cap — both of those exist for the
// GetWeightDataFast passes, which are orders of magnitude more expensive than this walk.
//
// COVERAGE LIMIT, reported rather than hidden: ForEachLandscapeProxy (Landscape.cpp:6319) visits
// the parent ALandscape and the RESIDENT ALandscapeStreamingProxy list, so on a World Partition
// landscape an unloaded proxy's components are never examined — the same blind spot that makes an
// unloaded-proxy delete an orphan source in the first place. The scan can only ever answer "no
// orphans among loaded components", so its counts travel with an explicit flag saying so instead
// of reading as a clean zero it has not earned.
struct FLandscapeOrphanedWeightAllocation
{
  FName LayerName;
  FString LayerInfoPath;
  int32 ComponentCount = 0;
  bool bLive = false;
};

struct FLandscapeOrphanedWeightScan
{
  TArray<FLandscapeOrphanedWeightAllocation> Orphans;
  int32 ComponentsScanned = 0;
  int32 ProxiesScanned = 0;
  int32 LiveOrphanCount = 0;
  int32 DeadOrphanComponents = 0;
};

static void ScanLandscapeOrphanedWeightAllocations(ULandscapeInfo* LandscapeInfo,
                                                   FLandscapeOrphanedWeightScan& OutScan)
{
  if (!LandscapeInfo)
  {
    return;
  }

  // The merge's survival set. Only non-null LayerInfoObj entries make it into
  // GetTargetLayerNames, so only those are added here.
  TSet<ULandscapeLayerInfoObject*> RegisteredLayerInfos;
  RegisteredLayerInfos.Reserve(LandscapeInfo->Layers.Num() + 1);
  for (const FLandscapeInfoLayerSettings& Setting : LandscapeInfo->Layers)
  {
    if (ULandscapeLayerInfoObject* Registered = Setting.LayerInfoObj)
    {
      RegisteredLayerInfos.Add(Registered);
    }
  }
  // The hole mask is bound to a process-wide singleton rather than to a per-landscape layer info,
  // and the merge requests it explicitly (bInIncludeVisibilityLayer = true), so its allocation is
  // never at risk and must not be reported as an orphan.
  if (ALandscapeProxy::VisibilityLayer)
  {
    RegisteredLayerInfos.Add(ALandscapeProxy::VisibilityLayer);
  }

  // Keyed by LayerInfo POINTER, not by name: a rename leaves two distinct objects carrying the
  // same LayerName, and collapsing them by name would under-report the damage.
  TMap<ULandscapeLayerInfoObject*, int32> LiveOrphanComponentCounts;

  LandscapeInfo->ForEachLandscapeProxy([&RegisteredLayerInfos, &LiveOrphanComponentCounts, &OutScan](ALandscapeProxy* Proxy) -> bool
  {
    if (!Proxy)
    {
      return true;
    }
    ++OutScan.ProxiesScanned;
    for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
    {
      if (!Component)
      {
        continue;
      }
      ++OutScan.ComponentsScanned;
      // The BASE allocation array — the one ReallocateLayersWeightmaps rewrites.
      const TArray<FWeightmapLayerAllocationInfo>& Allocations =
          Component->GetWeightmapLayerAllocations(/*InReturnEditingWeightmap=*/false);
      bool bComponentCarriesDeadOrphan = false;
      for (const FWeightmapLayerAllocationInfo& Alloc : Allocations)
      {
        // An entry with no texture index/channel holds no weight, so there is nothing to lose.
        if (!Alloc.IsAllocated())
        {
          continue;
        }
        ULandscapeLayerInfoObject* AllocLayerInfo = Alloc.LayerInfo;
        if (AllocLayerInfo == nullptr)
        {
          bComponentCarriesDeadOrphan = true;
          continue;
        }
        if (RegisteredLayerInfos.Contains(AllocLayerInfo))
        {
          continue;
        }
        int32& Count = LiveOrphanComponentCounts.FindOrAdd(AllocLayerInfo);
        ++Count;
      }
      if (bComponentCarriesDeadOrphan)
      {
        ++OutScan.DeadOrphanComponents;
      }
    }
    return true;
  });

  OutScan.Orphans.Reserve(LiveOrphanComponentCounts.Num());
  for (const TPair<ULandscapeLayerInfoObject*, int32>& Pair : LiveOrphanComponentCounts)
  {
    FLandscapeOrphanedWeightAllocation Entry;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
    // LayerName is UE_DEPRECATED(5.7) in favour of a getter that does not exist on 5.3-5.6;
    // reading the member under the pragma is the shape this plugin already uses for it.
    Entry.LayerName = Pair.Key->LayerName;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
    Entry.LayerInfoPath = Pair.Key->GetPathName();
    Entry.ComponentCount = Pair.Value;
    Entry.bLive = true;
    OutScan.Orphans.Add(MoveTemp(Entry));
  }
  OutScan.Orphans.Sort([](const FLandscapeOrphanedWeightAllocation& A,
                          const FLandscapeOrphanedWeightAllocation& B)
  {
    return A.LayerName.LexicalLess(B.LayerName);
  });
  OutScan.LiveOrphanCount = OutScan.Orphans.Num();

  if (OutScan.DeadOrphanComponents > 0)
  {
    // Shape B has no object, so it has no name and no path to report — only a component count.
    // Emitted as a single entry rather than one per component; the fields it cannot fill are
    // omitted rather than written as empty strings.
    FLandscapeOrphanedWeightAllocation DeadEntry;
    DeadEntry.ComponentCount = OutScan.DeadOrphanComponents;
    DeadEntry.bLive = false;
    OutScan.Orphans.Add(MoveTemp(DeadEntry));
  }
}

// orphanedLayers[] — the same array on the refusal's error data and on the success response, so
// the number exists whether or not the call proceeded.
static TArray<TSharedPtr<FJsonValue>> MakeLandscapeOrphanedLayerJson(const FLandscapeOrphanedWeightScan& Scan)
{
  TArray<TSharedPtr<FJsonValue>> Out;
  Out.Reserve(Scan.Orphans.Num());
  for (const FLandscapeOrphanedWeightAllocation& Entry : Scan.Orphans)
  {
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (Entry.bLive)
    {
      Obj->SetStringField(TEXT("layerName"), Entry.LayerName.ToString());
      Obj->SetStringField(TEXT("layerInfoPath"), Entry.LayerInfoPath);
    }
    Obj->SetBoolField(TEXT("live"), Entry.bLive);
    Obj->SetNumberField(TEXT("componentCount"), Entry.ComponentCount);
    Out.Add(MakeShared<FJsonValueObject>(Obj));
  }
  return Out;
}

// "'Rock' (/Game/L.LayerInfo_Rock, 4 components), ..." for the refusal message. Capped the same
// way FormatLayerNameList is; the uncapped total travels in orphanedLayers[].
static FString FormatLandscapeOrphanedLayerSummary(const FLandscapeOrphanedWeightScan& Scan)
{
  constexpr int32 MaxListedOrphans = 16;
  TArray<FString> Parts;
  int32 LiveSeen = 0;
  for (const FLandscapeOrphanedWeightAllocation& Entry : Scan.Orphans)
  {
    if (!Entry.bLive)
    {
      continue;
    }
    ++LiveSeen;
    if (Parts.Num() >= MaxListedOrphans)
    {
      continue;
    }
    Parts.Add(FString::Printf(TEXT("'%s' (%s, %d component%s)"),
        *Entry.LayerName.ToString(), *Entry.LayerInfoPath,
        Entry.ComponentCount, Entry.ComponentCount == 1 ? TEXT("") : TEXT("s")));
  }
  if (Parts.Num() == 0)
  {
    return TEXT("(none)");
  }
  FString Out = FString::Join(Parts, TEXT(", "));
  if (LiveSeen > Parts.Num())
  {
    Out += FString::Printf(TEXT(", ... (%d more)"), LiveSeen - Parts.Num());
  }
  return Out;
}

// ---- The BLAST RADIUS of a paint (B-paint-blanks-unallocated-layers-component-wide) ----
//
// The all-layers census answers "which layer lost weightmap BYTES". This answers a question
// the census cannot express at all: WHICH COMPONENTS gained a weightmap allocation, and
// therefore which material-declared layers stop being sampled across the whole of them.
//
// SOURCED on 5.8. A landscape component's material permutation is built from THAT
// COMPONENT's own allocation list and nothing else: ULandscapeComponent::GetLayerAllocationKey
// hashes the allocations into the MIC key (LandscapeEdit.cpp:549) and GetCombinationMaterial
// adds one FStaticTerrainLayerWeightParameter per allocation, keyed by
// Allocation.GetLayerName() (:638-648). A layer absent from that per-component set finds no
// match in FHLSLMaterialTranslator::StaticTerrainLayerWeight (HLSLMaterialTranslator.cpp:9030,
// INDEX_NONE at :9089-9101), and UMaterialExpressionLandscapeLayerSample::Compile turns that
// into Compiler->Constant(0.f) under the engine's own comment "layer is not used in this
// component, sample value is 0" (MaterialExpressionLandscapeLayerSample.cpp:39-51). So the
// FIRST allocation a virgin component gains drives every other material-declared layer to a
// constant zero across the WHOLE component. Measured: a 51x51-texel paint request removed the
// landscape grass from 6300-uu components, 6.1x the requested area.
//
// NOTHING IS LOST when that happens, and that is why both of this verb's existing honesty
// instruments report clean over it, correctly:
//   * otherLayerTexelsLost is max(0, OutsideBefore - OutsideAfter) over non-zero weightmap
//     bytes. The paint writes no sibling byte, so both counts are unmoved and 0 is the true
//     answer. GetWeightDataFast also returns byte 0 for BOTH "no allocation" and "allocated,
//     weight 0", so the deciding state is not representable in the census's alphabet.
//   * ScanLandscapeOrphanedWeightAllocations skips any allocation whose LayerInfo IS
//     registered, and the layer being painted is registered by construction.
// Report and warn, never refuse: no weightmap byte changes, the map file is unchanged, and
// repainting the sibling reverses it exactly.
//
// Keyed by layer NAME, deliberately, where ScanLandscapeOrphanedWeightAllocations is keyed by
// POINTER. That scan mirrors ReallocateLayersWeightmaps, which compares Alloc.LayerInfo by
// pointer; this one mirrors the shader's static parameter set, which is keyed by FName
// (LandscapeEdit.cpp:645). Two engine mechanisms, two correct keys.
struct FLandscapeComponentAllocationCoverage
{
  // One entry per LOADED component: the layer names it holds an ALLOCATED weightmap channel
  // for. Component pointer is the key, so a before/after pair diffs directly.
  // The component count for this same walk is already published as orphanScanComponents,
  // alongside orphanScanLoadedProxiesOnly, so it is not duplicated here.
  TMap<const ULandscapeComponent*, TSet<FName>> PerComponent;
};

struct FLandscapeAllocationBlastRadius
{
  int32 ComponentsGainingAllocation = 0;
  int32 ComponentsGainingFirstAllocation = 0;
  // Material-declared layer name -> how many gaining components hold NO allocation for it
  // afterwards, i.e. how many components compile it to Constant(0) across their whole span.
  TArray<TPair<FName, int32>> LayersWithoutAllocation;
  FBox FootprintBox = FBox(ForceInit);
  bool bFootprintMeasured = false;
};

static void ScanLandscapeComponentAllocationCoverage(ULandscapeInfo* LandscapeInfo,
                                                     FLandscapeComponentAllocationCoverage& OutCoverage)
{
  if (!LandscapeInfo)
  {
    return;
  }
  LandscapeInfo->ForEachLandscapeProxy([&OutCoverage](ALandscapeProxy* Proxy) -> bool
  {
    if (!Proxy)
    {
      return true;
    }
    for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
    {
      if (!Component)
      {
        continue;
      }
      // FindOrAdd even when the component carries nothing: "present with an empty set" is
      // what distinguishes a virgin component from one the walk never reached, and the
      // first-allocation transition is defined on exactly that difference.
      TSet<FName>& Names = OutCoverage.PerComponent.FindOrAdd(Component);
      // The BASE allocation array — the one the merge rewrites and the one
      // GetCombinationMaterial reads when it builds the permutation.
      for (const FWeightmapLayerAllocationInfo& Alloc :
           Component->GetWeightmapLayerAllocations(/*InReturnEditingWeightmap=*/false))
      {
        if (!Alloc.IsAllocated() || Alloc.LayerInfo == nullptr)
        {
          continue;
        }
PRAGMA_DISABLE_DEPRECATION_WARNINGS
        // LayerName is UE_DEPRECATED(5.7) in favour of a getter that does not exist on
        // 5.3-5.6; reading the member under the pragma is the shape this file already uses.
        // FWeightmapLayerAllocationInfo::GetLayerName() is not an option: the struct carries
        // no LANDSCAPE_API and that member is defined out of line (LandscapeComponent.cpp:20),
        // so calling it from this module would not link.
        const FName AllocName = Alloc.LayerInfo->LayerName;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
        if (!AllocName.IsNone())
        {
          Names.Add(AllocName);
        }
      }
    }
    return true;
  });
}

// Diffs two coverage snapshots and reports what the paint's allocation changes cost the
// layers the MATERIAL declares — not the layers ULandscapeInfo registers. The material's set
// is the right universe here because it is the shader's: a declared layer with no
// ULandscapeLayerInfoObject asset yet still compiles a LandscapeLayerSample node, and that
// node is what goes to Constant(0).
static void DiffLandscapeComponentAllocationCoverage(
    const FLandscapeComponentAllocationCoverage& Before,
    const FLandscapeComponentAllocationCoverage& After,
    const TArray<FName>& MaterialTargetLayers,
    FLandscapeAllocationBlastRadius& OutRadius)
{
  TMap<FName, int32> ZeroSampledCounts;
  for (const TPair<const ULandscapeComponent*, TSet<FName>>& AfterEntry : After.PerComponent)
  {
    const TSet<FName>* BeforeNames = Before.PerComponent.Find(AfterEntry.Key);
    bool bGained = false;
    for (const FName& Name : AfterEntry.Value)
    {
      if (!BeforeNames || !BeforeNames->Contains(Name))
      {
        bGained = true;
        break;
      }
    }
    if (!bGained)
    {
      continue;
    }
    ++OutRadius.ComponentsGainingAllocation;
    if (!BeforeNames || BeforeNames->Num() == 0)
    {
      // The expensive transition: before this, the component had no per-layer static
      // parameter at all, so nothing on it was being driven to Constant(0) by a sibling.
      ++OutRadius.ComponentsGainingFirstAllocation;
    }
    for (const FName& Declared : MaterialTargetLayers)
    {
      if (!AfterEntry.Value.Contains(Declared))
      {
        ++ZeroSampledCounts.FindOrAdd(Declared);
      }
    }
    if (const ULandscapeComponent* Component = AfterEntry.Key)
    {
      // World-space bounds, so the footprint travels in the units a caller compares against
      // `region` — nothing else in the response says how large a component is.
      OutRadius.FootprintBox += Component->Bounds.GetBox();
      OutRadius.bFootprintMeasured = true;
    }
  }

  OutRadius.LayersWithoutAllocation.Reserve(ZeroSampledCounts.Num());
  for (const TPair<FName, int32>& Pair : ZeroSampledCounts)
  {
    OutRadius.LayersWithoutAllocation.Add(Pair);
  }
  OutRadius.LayersWithoutAllocation.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B)
  {
    return A.Key.LexicalLess(B.Key);
  });
}

// ---- The auto-created target-layer info's BLEND METHOD
//      (B-paint-auto-created-layer-never-weight-blended) ----
//
// ULandscapeLayerInfoObject's constructor takes its method from project settings —
// BlendMethod = GetDefault<ULandscapeSettings>()->GetTargetLayerDefaultBlendMethod()
// (LandscapeLayerInfoObject.cpp:27) — whose compiled default is
// ELandscapeTargetLayerBlendMethod::None (LandscapeSettings.h:167), display name
// "No Weight Blending". A layer on None is excluded from the merge's group normalisation: the
// final weight-blending pass (FLandscapeEditLayersWeightmapsPerformFinalWeightBlendingPS,
// LandscapeEditLayers.cpp:791) admits a layer only under
// GetBlendMethod() == FinalWeightBlending (:3594). So a bare NewObject here manufactured a
// layer that could never take part in the normalisation this verb's own summary advertises,
// on the default path, with no field in the response naming the method.
//
// WHY UE::Landscape::CreateTargetLayerInfo IS NOT CALLED, having been read rather than
// assumed. It is the engine's blessed factory (LandscapeUtils.cpp:286/292, used by the Target
// Layers panel, the import-layers path, LandscapeEditorObject and the world browser) but its
// FIRST act is CreatePackage(InFilePath/InFileName) and its last are
// FAssetRegistryModule::AssetCreated + MarkPackageDirty: it always mints a /Game .uasset at a
// caller-supplied path. This verb creates a PRIVATE per-landscape LayerInfo outered to the
// landscape actor — the behaviour its own warnings[] line documents — so adopting the factory
// would add a required path parameter, a new asset on disk and a dirtied package to fix a
// defect that is one enum wide. What IS adopted is the half the factory uniquely holds and
// the ticket names as the disconnected control: honouring
// ULandscapeSettings::GetDefaultLayerInfoObject() by DUPLICATION (LandscapeUtils.cpp:288-302),
// so a project that configures a weight-blended template gets it here too.
struct FLandscapePaintLayerInfoCreation
{
  ULandscapeLayerInfoObject* LayerInfo = nullptr;
  // Empty when ULandscapeSettings::DefaultLayerInfoObject is unset, which is the engine default.
  FString TemplatePath;
  bool bDuplicatedFromTemplate = false;
};

static FLandscapePaintLayerInfoCreation CreateLandscapePaintTargetLayerInfo(
    ALandscape* Landscape, const FName& RequestedLayer)
{
  FLandscapePaintLayerInfoCreation Out;
  if (!Landscape)
  {
    return Out;
  }
  const FName ObjectName(*FString::Printf(TEXT("LayerInfo_%s"), *RequestedLayer.ToString()));

  if (const ULandscapeSettings* Settings = GetDefault<ULandscapeSettings>())
  {
    if (ULandscapeLayerInfoObject* Template = Settings->GetDefaultLayerInfoObject().LoadSynchronous())
    {
      // Outer stays the landscape actor, unlike the engine factory's package: the template
      // supplies the property VALUES (blend method, blend group, physical material, usage
      // colour), not the packaging decision.
      Out.LayerInfo = DuplicateObject<ULandscapeLayerInfoObject>(Template, Landscape, ObjectName);
      if (Out.LayerInfo)
      {
        Out.LayerInfo->SetFlags(RF_Public | RF_Transactional);
        Out.bDuplicatedFromTemplate = true;
        Out.TemplatePath = Template->GetPathName();
      }
    }
  }
  if (!Out.LayerInfo)
  {
    Out.LayerInfo = NewObject<ULandscapeLayerInfoObject>(Landscape, ObjectName,
                                                        RF_Public | RF_Transactional);
  }
  return Out;
}

// The measured blend method, for the response. Empty for a null layer info, so the field is
// omitted rather than reported as a guess.
static FString LandscapeTargetLayerBlendMethodName(const ULandscapeLayerInfoObject* LayerInfo)
{
  if (!LayerInfo)
  {
    return FString();
  }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
  switch (LayerInfo->GetBlendMethod())
  {
    case ELandscapeTargetLayerBlendMethod::None:                       return TEXT("None");
    case ELandscapeTargetLayerBlendMethod::FinalWeightBlending:        return TEXT("FinalWeightBlending");
    case ELandscapeTargetLayerBlendMethod::PremultipliedAlphaBlending: return TEXT("PremultipliedAlphaBlending");
    default:                                                          break;
  }
  return TEXT("Unknown");
#else
  // Pre-5.7 there is no BlendMethod enum; bNoWeightBlend is the whole switch, and PostLoad
  // migrates it as (bNoWeightBlend ? None : FinalWeightBlending)
  // (LandscapeLayerInfoObject.cpp:276), so the two names map exactly.
  return LayerInfo->bNoWeightBlend ? TEXT("None") : TEXT("FinalWeightBlending");
#endif
}

// Whether this layer takes part in the merge's group normalisation at all.
static bool LandscapeTargetLayerIsWeightBlended(const ULandscapeLayerInfoObject* LayerInfo)
{
  if (!LayerInfo)
  {
    return false;
  }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
  return LayerInfo->GetBlendMethod() != ELandscapeTargetLayerBlendMethod::None;
#else
  return !LayerInfo->bNoWeightBlend;
#endif
}

// ---- landscape.create_procedural_terrain ----
REGISTER_RPC_HANDLER("landscape.create_procedural_terrain", "landscape", "Paint a weight-blended layer on a landscape across an optional region — a misnomer-named alias for layer paint. The named layer must already be a TARGET LAYER on the landscape material (i.e. declared by a LandscapeLayerBlend / LandscapeLayerWeight node); a layer no material declares is rejected with LAYER_NOT_FOUND listing the layers that ARE available, never reported as a successful paint. COSTS THE SIBLING LAYERS, BUT ONLY IF THEY ARE WEIGHT-BLENDED: weight-blended layers normalize as a group, so painting one at strength 1.0 drives every other layer in that blend group toward zero WITHIN the painted region — that is the engine's behaviour, not a defect, and it means multi-layer terrain is built by painting each layer over the region it should own, never by painting one layer over the full extent and then another. A layer whose ULandscapeLayerInfoObject is on blend method None takes no part in that at all, so the response publishes the MEASURED method per layer (layerBlendMethod, layersAffected[].blendMethod) and warns when the painted layer is not in the group. A LayerInfo this verb auto-creates is made FinalWeightBlending — or duplicated from the project's ULandscapeSettings::DefaultLayerInfoObject template when one is configured — instead of inheriting the engine's None default; pass weightBlended=false for a layer that must not normalize. The paint is one undoable transaction, and when verify is on the response carries a before/after census of every REGISTERED layer (layersAffected[], otherLayerTexelsLost, otherLayerTexelsLostInRegion) so the WEIGHT cost to the other layers is a reported number instead of a discovery in Landscape Ed Mode. THE BLAST RADIUS IS THE COMPONENT, NOT THE REGION: a component's material permutation is built from THAT component's own weightmap allocation list, so the first allocation a virgin component gains makes every other material-declared layer it holds no allocation for sample as a constant 0 across the WHOLE component — far outside the requested region, and invisible to the census because no weightmap byte changes. That is measured and reported separately as componentsGainingAllocation / componentsGainingFirstAllocation / layersNowSampledZeroOnTouchedComponents[] / allocationFootprintUU, with componentSizeUU beside them; paint the base layer over the FULL extent first, then accents over their regions, so no component takes its first allocation from an accent. REFUSES ON ORPHANED WEIGHT: a landscape carrying weightmap weight for a layer that is NOT a registered target layer (a Target Layers panel rename, a delete with proxies unloaded, a third-party paint) is rejected with LANDSCAPE_ORPHANED_LAYER_WEIGHT rather than painted, because the merge would erase that weight landscape-wide with no undo and the census — which reads the registration side — could not see the loss. That scan always runs, reports orphanedLayers[] / orphanScanComponents / orphanScanProxies over LOADED proxies only, and is overridden with allowOrphanedLayerLoss=true.",
    RPC_PARAMS(
        RPC_PARAM_OPT("landscapePath", "path", "Object path of the landscape; either this or landscapeName must be provided."),
        RPC_PARAM_OPT("landscapeName", "string", "Display label of the landscape actor in the level."),
        RPC_PARAM_REQ("layerName", "string", "Name of the landscape layer (FName) to paint. Must be a target layer declared by the landscape material; anything else returns LAYER_NOT_FOUND with the available names in the message and in error data availableLayers[]."),
        RPC_PARAM_DEF("strength", "number", "Layer weight 0..1 to write into the region. Values outside the range are clamped and the clamp is reported in warnings[].", "1.0"),
        RPC_PARAM_OPT("region", "object", "Heightmap-pixel region {minX, minY, maxX, maxY} to paint. Each coordinate independently defaults to the corresponding full-landscape extent and is clamped into it; a region that is empty after clamping is rejected with INVALID_ARGUMENT rather than painting nothing and reporting success."),
        RPC_PARAM_DEF("skipFlush", "boolean", "Defer the GPU layer flush so multiple paint calls are batched. Implies verify=false.", "false"),
        RPC_PARAM_DEF("verify", "boolean", "Settle the deferred edit-layer regeneration, read the painted layer's weightmap back (sampledTexels / texelsWithWeight / texelsAtRequestedWeight), and census every REGISTERED layer over the FULL landscape extent before and after the write (layersAffected[], otherLayerTexelsLost, otherLayerTexelsLostInRegion). A non-zero otherLayerTexelsLost means another layer lost weight OUTSIDE the region and is reported in warnings[]. NOT every layer: the census enumerates the registration side (ULandscapeInfo::Layers, skipping null layer infos), so weight allocated for a layer that is NOT a registered target layer is invisible to it and otherLayerTexelsLost would read 0 over its erasure. That case is covered by the separate orphaned-allocation scan, which always runs and reports orphanedLayers[] / orphanScanComponents / orphanScanProxies regardless of this flag. AND NOT EVERY KIND OF DAMAGE: otherLayerTexelsLost is a DIFFERENCE OF TWO TEXEL COUNTS and cannot express a loss where no texel changed, so a clean census is not a promise that the frame is unchanged — the per-component allocation change this same pass measures (componentsGainingAllocation, componentsGainingFirstAllocation, layersNowSampledZeroOnTouchedComponents[], allocationFootprintUU) changes which shader permutation a component compiles without moving a single weightmap byte, and otherLayerTexelsLost reads 0 truthfully over it. The census costs one full-extent read per layer, twice, and is skipped with a warning on a landscape larger than 4194304 texels; the allocation-side measurement is a pointer walk and is NOT subject to that cap, only to this flag. Pass false (or skipFlush=true) to skip the regeneration, the census and the allocation-side measurement in a batch — those fields are then OMITTED rather than reported as zeros.", "true"),
        RPC_PARAM_DEF("weightBlended", "boolean", "Blend method for a ULandscapeLayerInfoObject this call AUTO-CREATES (layerInfoAutoCreated=true). true sets ELandscapeTargetLayerBlendMethod::FinalWeightBlending, so the layer takes part in the merge's group normalization; false sets None, so its weight is unaffected by its siblings and theirs by it. Only meaningful on the auto-create path: an existing LayerInfo is a shared asset and this verb never rewrites its blend method — supplying the parameter for a layer that already has one is reported in warnings[] rather than silently ignored. Omitting it does NOT mean None: when the project configures ULandscapeSettings::DefaultLayerInfoObject the created layer is duplicated from that template and keeps the template's method (reported as layerInfoTemplatePath); otherwise it is FinalWeightBlending. The engine's own constructor default is None, which is what this parameter exists to stop being the silent outcome.", "true"),
        RPC_PARAM_DEF("allowOrphanedLayerLoss", "boolean", "Proceed even when the landscape carries weightmap weight for a layer that is NOT a registered target layer. By default such a paint is REFUSED with LANDSCAPE_ORPHANED_LAYER_WEIGHT, because the edit-layer merge it triggers deletes every unrequested allocation from every component of every loaded proxy — landscape-wide, independent of region and strength — and editor.undo does not reverse it (the merge runs after this call's transaction closes). Pass true to accept that erasure deliberately; the refusal becomes a warnings[] line and the layers are still reported in orphanedLayers[]. An allocation whose ULandscapeLayerInfoObject is null never refuses either way: nothing can sample or read it back, so it is only reported.", "false")
    ))
{
  auto* Payload = Ctx.GetRawPayload().Get();
  if (!Payload) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_PAYLOAD, TEXT("paint_landscape_layer payload missing"));
    return true;
  }

  FString LandscapePath;
  Payload->TryGetStringField(TEXT("landscapePath"), LandscapePath);
  FString LandscapeName;
  Payload->TryGetStringField(TEXT("landscapeName"), LandscapeName);

  if (!LandscapePath.IsEmpty()) {
    FString SafePath = SanitizeProjectRelativePath(LandscapePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe landscape path: %s"), *LandscapePath));
      return true;
    }
    LandscapePath = SafePath;
  }

  FString LayerName;
  if (!Payload->TryGetStringField(TEXT("layerName"), LayerName) || LayerName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("layerName required"));
    return true;
  }

  // Explicit TOptionals, NOT the old int32 -1 sentinel. The sentinel silently replaced any
  // negative coordinate with the full extent, so on a landscape whose extent starts below
  // zero a caller could not address the negative half of the coordinate space and got a
  // full-landscape repaint instead of the region asked for — the same defect
  // F-landscape-height-readback fixed for the height verbs. ResolveHeightRegion applies the
  // per-coordinate defaulting + clamp and is shared with landscape.edit /
  // landscape.get_heights so the three cannot drift.
  TOptional<int32> ReqMinX, ReqMinY, ReqMaxX, ReqMaxY;
  const TSharedPtr<FJsonObject> *RegionObj = nullptr;
  if (Payload->TryGetObjectField(TEXT("region"), RegionObj) && RegionObj) {
    int32 V;
    if ((*RegionObj)->TryGetNumberField(TEXT("minX"), V)) { ReqMinX = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("minY"), V)) { ReqMinY = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("maxX"), V)) { ReqMaxX = V; }
    if ((*RegionObj)->TryGetNumberField(TEXT("maxY"), V)) { ReqMaxY = V; }
  }

  double RequestedStrength = 1.0;
  const bool bStrengthSupplied = Payload->TryGetNumberField(TEXT("strength"), RequestedStrength);
  const double Strength = FMath::Clamp(RequestedStrength, 0.0, 1.0);

  bool bSkipFlush = false;
  Payload->TryGetBoolField(TEXT("skipFlush"), bSkipFlush);

  bool bVerify = true;
  Payload->TryGetBoolField(TEXT("verify"), bVerify);
  // A deferred flush means the write has not been uploaded, so a readback would measure
  // the wrong thing; a batching caller has already opted out of per-call settle cost.
  const bool bVerifySkippedByFlush = bVerify && bSkipFlush;
  if (bSkipFlush) { bVerify = false; }

  // Deliberately NOT tied to verify or skipFlush: the orphan scan is a structural precondition
  // on the write, not a measurement of it, and it costs nothing the census costs.
  bool bAllowOrphanedLayerLoss = false;
  Payload->TryGetBoolField(TEXT("allowOrphanedLayerLoss"), bAllowOrphanedLayerLoss);

  // Whether it was SUPPLIED is carried separately from its value: an explicit request always
  // wins, while an omission lets a configured DefaultLayerInfoObject template keep its own
  // method rather than being overwritten by this parameter's default.
  bool bWeightBlended = true;
  const bool bWeightBlendedSupplied = Payload->TryGetBoolField(TEXT("weightBlended"), bWeightBlended);

  return PinWrightSafePoint::RunAtSafePoint(Ctx, TEXT("landscape.create_procedural_terrain"),
      [LandscapePath, LandscapeName, LayerName, ReqMinX, ReqMinY, ReqMaxX, ReqMaxY,
       RequestedStrength, bStrengthSupplied, Strength, bSkipFlush, bVerify,
       bVerifySkippedByFlush, bAllowOrphanedLayerLoss, bWeightBlended, bWeightBlendedSupplied]
      (const PinWrightSafePoint::FSafePointResponder& Responder) {
    // Additive, emitted only when non-empty: a clean paint keeps a byte-identical response.
    TArray<TSharedPtr<FJsonValue>> Warnings;
    auto NoteWarning = [&Warnings](const FString& Text) {
      Warnings.Add(MakeShared<FJsonValueString>(Text));
    };

    FString ErrorMsg;
    ALandscape* Landscape = FindLandscapeByNameOrPath(LandscapePath, LandscapeName, ErrorMsg);
    if (!Landscape) {
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NOT_FOUND, ErrorMsg);
      return;
    }

    ULandscapeInfo *LandscapeInfo = Landscape->GetLandscapeInfo();
    if (!LandscapeInfo) {
      Responder.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Landscape has no info"));
      return;
    }

    const FString ActorLabel = Landscape->GetActorLabel();
    const FName RequestedLayer(*LayerName);

    // ================================================================================
    // Pre-flight. Every branch below used to be a path where the paint elided and the
    // response still said "Layer painted successfully"
    // (B-create-procedural-terrain-paints-nothing). The layer checks run FIRST, before
    // the structural extent check, so the common "wrong layer name" diagnosis is never
    // masked by an unrelated landscape problem.
    // ================================================================================

    // No material assigned at all. GetLandscapeMaterial() never returns null (it falls
    // back to the engine default surface material, which declares zero target layers), so
    // the assigned-material field is what has to be tested to tell these two apart.
    if (Landscape->LandscapeMaterial == nullptr) {
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NO_MATERIAL,
          FString::Printf(
              TEXT("Landscape '%s' has no landscape material assigned, so it has no target layers and layer '%s' cannot be painted. ")
              TEXT("Assign one with landscape.set_material (create a suitable one with material.authoring.create_landscape_material)."),
              *ActorLabel, *LayerName));
      return;
    }

    const FString MaterialPath = Landscape->LandscapeMaterial->GetPathName();
    const TArray<FName> PaintableLayers = GetPaintableTargetLayerNames(Landscape);

    // The reported repro: material has no LandscapeLayerBlend, so target_layers holds
    // only __LANDSCAPE_VISIBILITY__ (filtered out by GetPaintableTargetLayerNames) and
    // there is no weightmap layer to paint into at all.
    if (PaintableLayers.Num() == 0) {
      TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
      ErrData->SetStringField(TEXT("landscapeName"), ActorLabel);
      ErrData->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
      ErrData->SetStringField(TEXT("layerName"), LayerName);
      ErrData->SetStringField(TEXT("materialPath"), MaterialPath);
      ErrData->SetArrayField(TEXT("availableLayers"), TArray<TSharedPtr<FJsonValue>>());
      ErrData->SetNumberField(TEXT("availableLayerCount"), 0);
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_MATERIAL_NO_LAYERS,
          FString::Printf(
              TEXT("Landscape '%s' cannot be painted: its material '%s' declares no paintable target layers, so there is no weightmap layer named '%s' — or any other — to paint into. ")
              TEXT("A landscape material must contain a LandscapeLayerBlend (or LandscapeLayerWeight / LandscapeLayerSample) node with named layers before ANY layer painting can work. ")
              TEXT("Build one with material.authoring.create_landscape_material and assign it with landscape.set_material, then re-run."),
              *ActorLabel, *MaterialPath, *LayerName),
          ErrData);
      return;
    }

    // The layer name is spelled wrong, or belongs to a different material. Listing the
    // names that ARE available is the entire point of this branch: without it the caller
    // has no way to tell a typo from a missing material short of opening the editor.
    if (!PaintableLayers.Contains(RequestedLayer)) {
      TArray<TSharedPtr<FJsonValue>> AvailableJson;
      AvailableJson.Reserve(PaintableLayers.Num());
      for (const FName& Name : PaintableLayers) {
        AvailableJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
      }

      TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
      ErrData->SetStringField(TEXT("landscapeName"), ActorLabel);
      ErrData->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
      ErrData->SetStringField(TEXT("layerName"), LayerName);
      ErrData->SetStringField(TEXT("materialPath"), MaterialPath);
      ErrData->SetArrayField(TEXT("availableLayers"), AvailableJson);
      ErrData->SetNumberField(TEXT("availableLayerCount"), PaintableLayers.Num());

      FString Message = FString::Printf(
          TEXT("Layer '%s' is not a target layer on landscape '%s'. Available layers: %s. ")
          TEXT("Layer names come from the landscape material's LandscapeLayerBlend / LandscapeLayerWeight nodes (material: %s); ")
          TEXT("either paint one of the names above, or add '%s' to that material and re-run."),
          *LayerName, *ActorLabel, *FormatLayerNameList(PaintableLayers), *MaterialPath, *LayerName);
      if (RequestedLayer == GetLandscapeVisibilityLayerName()) {
        Message += FString::Printf(
            TEXT(" ('%s' is the engine's hole mask, not a weight-blended layer — it is bound to the ")
            TEXT("ALandscapeProxy::VisibilityLayer singleton and cannot be painted through this verb.)"),
            *GetLandscapeVisibilityLayerName().ToString());
      }
      Responder.SendError(ErrorCodes::ERR_LAYER_NOT_FOUND, Message, ErrData);
      return;
    }

    // Structural: a hollow landscape (no registered ULandscapeComponents) has no extent.
    // The old code ignored GetLandscapeExtent's return value, so on a hollow actor it
    // carried MinX=MAX_int32 / MaxX=MIN_int32 straight into the region arithmetic — a
    // signed overflow feeding TArray::Init, i.e. worse than a silent no-op. Same
    // diagnostic landscape.edit / landscape.get_heights emit.
    int32 FullMinX = 0, FullMinY = 0, FullMaxX = 0, FullMaxY = 0;
    if (!LandscapeInfo->GetLandscapeExtent(FullMinX, FullMinY, FullMaxX, FullMaxY)) {
      if (LandscapeInfo->XYtoComponentMap.Num() == 0) {
        Responder.SendError(ErrorCodes::ERR_LANDSCAPE_NO_COMPONENTS,
            FString::Printf(
                TEXT("Landscape '%s' has no registered ULandscapeComponents (hollow landscape — XYtoComponentMap is empty), so it has no paintable extent. It was likely spawned without a component grid; recreate it via landscape.create or verify the actor before painting."),
                *ActorLabel));
        return;
      }
      Responder.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Failed to get landscape extent"));
      return;
    }

    const LandscapeHeightStats::FResolvedHeightRegion Region = LandscapeHeightStats::ResolveHeightRegion(
        ReqMinX, ReqMinY, ReqMaxX, ReqMaxY, FullMinX, FullMinY, FullMaxX, FullMaxY);
    if (!Region.bValid) {
      // An inverted or wholly out-of-bounds region used to produce a degenerate
      // SetAlphaData rect that touched nothing and still returned success.
      Responder.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(
              TEXT("Empty paint region after clamping to landscape extent [%d,%d]..[%d,%d] — nothing would be painted."),
              FullMinX, FullMinY, FullMaxX, FullMaxY));
      return;
    }

    const int32 PaintMinX = Region.MinX;
    const int32 PaintMinY = Region.MinY;
    const int32 PaintMaxX = Region.MaxX;
    const int32 PaintMaxY = Region.MaxY;
    const int32 RegionSizeX = (PaintMaxX - PaintMinX + 1);
    const int32 RegionSizeY = (PaintMaxY - PaintMinY + 1);
    const int32 RegionTexels = RegionSizeX * RegionSizeY;

    // Report a region the caller asked for but did not get, instead of silently
    // substituting the clamped one.
    if ((ReqMinX.IsSet() && ReqMinX.GetValue() != PaintMinX) ||
        (ReqMinY.IsSet() && ReqMinY.GetValue() != PaintMinY) ||
        (ReqMaxX.IsSet() && ReqMaxX.GetValue() != PaintMaxX) ||
        (ReqMaxY.IsSet() && ReqMaxY.GetValue() != PaintMaxY)) {
      NoteWarning(FString::Printf(
          TEXT("Requested region was clamped to the landscape extent [%d,%d]..[%d,%d]; painted [%d,%d]..[%d,%d]."),
          FullMinX, FullMinY, FullMaxX, FullMaxY, PaintMinX, PaintMinY, PaintMaxX, PaintMaxY));
    }

    if (bStrengthSupplied && !FMath::IsNearlyEqual(RequestedStrength, Strength)) {
      NoteWarning(FString::Printf(
          TEXT("strength %g is outside 0..1 and was clamped to %g."), RequestedStrength, Strength));
    }

    // ---- The unit the blast radius is measured in ----
    // Nothing else in the response says how large a component is, so a caller could not
    // derive the reach of a per-component effect even after being told it matters. The scale
    // is read off the actor rather than assumed, the same way landscape.create reads it.
    const FVector PaintDrawScale = Landscape->GetActorScale3D();
    const double PaintDrawScaleX = FMath::Abs(PaintDrawScale.X);
    const double PaintDrawScaleY = FMath::Abs(PaintDrawScale.Y);
    const int32 LandscapeComponentSizeQuads = Landscape->ComponentSizeQuads;
    const double ComponentSizeUUX = LandscapeComponentSizeQuads * PaintDrawScaleX;
    const double ComponentSizeUUY = LandscapeComponentSizeQuads * PaintDrawScaleY;
    // The REQUEST, named separately from anything measured: the region's texel count times
    // the draw scale, which is the span the caller asked to change.
    const double RequestedFootprintUUX = RegionSizeX * PaintDrawScaleX;
    const double RequestedFootprintUUY = RegionSizeY * PaintDrawScaleY;

    // ---- Resolve the ULandscapeLayerInfoObject the write binds to ----
    // The layer is known to exist on the material at this point, so ULandscapeInfo::Layers
    // should carry it — but that map is a cache; refresh it before the scan so a material
    // edited earlier in the same session is reflected.
    LandscapeInfo->UpdateLayerInfoMap();

    // ---- Orphaned-allocation scan: the ALLOCATION side, before anything is written ----
    // Runs HERE, before the auto-create branch below registers anything and before the
    // transaction, so a refusal leaves the landscape exactly as it found it. Its verdict is
    // defined against the registration UpdateLayerInfoMap() has just refreshed, and it is
    // deliberately not gated on `verify` or on the census texel cap — see the helper's comment.
    FLandscapeOrphanedWeightScan OrphanScan;
    ScanLandscapeOrphanedWeightAllocations(LandscapeInfo, OrphanScan);
    const UWorld* LandscapeWorld = Landscape->GetWorld();
    const bool bPartitionedLandscapeWorld = LandscapeWorld && LandscapeWorld->IsPartitionedWorld();

    if (OrphanScan.LiveOrphanCount > 0 && !bAllowOrphanedLayerLoss) {
      const FString OrphanSummary = FormatLandscapeOrphanedLayerSummary(OrphanScan);
      TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
      ErrData->SetStringField(TEXT("landscapeName"), ActorLabel);
      ErrData->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
      ErrData->SetStringField(TEXT("layerName"), LayerName);
      ErrData->SetArrayField(TEXT("orphanedLayers"), MakeLandscapeOrphanedLayerJson(OrphanScan));
      ErrData->SetNumberField(TEXT("orphanedLayerCount"), OrphanScan.LiveOrphanCount);
      ErrData->SetNumberField(TEXT("orphanScanComponents"), OrphanScan.ComponentsScanned);
      ErrData->SetNumberField(TEXT("orphanScanProxies"), OrphanScan.ProxiesScanned);
      ErrData->SetBoolField(TEXT("orphanScanLoadedProxiesOnly"), true);
      ErrData->SetBoolField(TEXT("worldPartitioned"), bPartitionedLandscapeWorld);

      FString Message = FString::Printf(
          TEXT("Landscape '%s' carries weightmap weight for %d layer(s) that are NOT registered target layers: %s. ")
          TEXT("Painting '%s' triggers the edit-layer weightmap merge, which requests only the REGISTERED layers and deletes every other allocation from every component of every loaded proxy — that weight would be erased LANDSCAPE-WIDE, independent of region and of strength, and editor.undo would NOT restore it because the merge runs after this call's transaction closes. ")
          TEXT("The all-layers census cannot see the loss either: it enumerates the registration side, so it would report otherLayerTexelsLost 0 over it. ")
          TEXT("Re-register the layer first (assign its ULandscapeLayerInfoObject to the matching slot in Landscape Ed Mode's Target Layers panel), or pass allowOrphanedLayerLoss=true to accept the erasure deliberately."),
          *ActorLabel, OrphanScan.LiveOrphanCount, *OrphanSummary, *LayerName);
      if (bPartitionedLandscapeWorld) {
        Message += FString::Printf(
            TEXT(" This is a World Partition world and the scan covered the %d LOADED proxy/proxies (%d components) only, so the listed set is a lower bound."),
            OrphanScan.ProxiesScanned, OrphanScan.ComponentsScanned);
      }
      Responder.SendError(ErrorCodes::ERR_LANDSCAPE_ORPHANED_LAYER_WEIGHT, Message, ErrData);
      return;
    }

    if (OrphanScan.LiveOrphanCount > 0) {
      // Reached only with the opt-in. The refusal downgrades to a warning, never to silence.
      NoteWarning(FString::Printf(
          TEXT("allowOrphanedLayerLoss=true: %d unregistered layer allocation(s) — %s — will be ERASED from every component of every loaded proxy by the weightmap merge this paint triggers. The loss is landscape-wide, unrelated to region and strength, NOT reversible with editor.undo, and NOT counted by otherLayerTexelsLost (the census enumerates registered layers only). See orphanedLayers[]."),
          OrphanScan.LiveOrphanCount, *FormatLandscapeOrphanedLayerSummary(OrphanScan)));
    }
    if (OrphanScan.DeadOrphanComponents > 0) {
      // Shape B: reported, never refused on. Nothing samples it, GetWeightDataFast has no object
      // to read it by, and the engine's own FixupWeightmaps deletes it as a matter of policy.
      NoteWarning(FString::Printf(
          TEXT("%d component(s) carry a weightmap allocation whose ULandscapeLayerInfoObject is null; the merge this paint triggers will drop them. Nothing can sample or read that channel back, so this is reported rather than refused on. See orphanedLayers[]."),
          OrphanScan.DeadOrphanComponents));
    }
    if (bPartitionedLandscapeWorld) {
      NoteWarning(FString::Printf(
          TEXT("This landscape is in a World Partition world; the orphaned-allocation scan covered the %d LOADED proxy/proxies (%d components) only. An unloaded proxy's weightmap allocations were not examined, so the scan's verdict is about LOADED COMPONENTS rather than about the landscape — orphanedLayers[] is a lower bound, and its absence is not proof there is none. Load the landscape's regions before relying on either."),
          OrphanScan.ProxiesScanned, OrphanScan.ComponentsScanned));
    }

    // ---- Per-component allocation coverage: the "before" half ----
    // Bound to `verify`, not to the census texel cap: this is a pointer walk over resident
    // arrays, not a weightmap read, so a landscape over the cap still gets it. Without
    // `verify` there is no settle, the edit-layer merge has not rewritten the BASE allocation
    // arrays yet, and an "after" read would measure the pre-merge state — so the whole
    // measurement is OMITTED rather than published as zeros that would read as "no component
    // changed". Taken before the write, and before the auto-create branch, for the same
    // reason the orphan scan is.
    FLandscapeComponentAllocationCoverage AllocationBefore;
    if (bVerify) {
      ScanLandscapeComponentAllocationCoverage(LandscapeInfo, AllocationBefore);
    }

    int32 ExistingLayerIndex = INDEX_NONE;
    ULandscapeLayerInfoObject *LayerInfo = nullptr;
    for (int32 i = 0; i < LandscapeInfo->Layers.Num(); ++i) {
      if (LandscapeInfo->Layers[i].LayerName == RequestedLayer) {
        ExistingLayerIndex = i;
        LayerInfo = LandscapeInfo->Layers[i].LayerInfoObj;
        break;
      }
    }

    // A material target layer with no ULandscapeLayerInfoObject asset yet is a legitimate
    // state (the engine shows it as an unassigned target layer in Landscape Ed Mode), so
    // manufacture one. Unlike the old auto-create this can only be reached for a layer the
    // material really declares — it can no longer invent a layer nobody samples — and it
    // fills the EXISTING placeholder slot instead of appending a duplicate entry with the
    // same name.
    bool bLayerInfoAutoCreated = false;
    FString LayerInfoTemplatePath;
    if (!LayerInfo) {
      // Honours ULandscapeSettings::DefaultLayerInfoObject by duplication when the project
      // sets one, and otherwise constructs — the two steps UE::Landscape::CreateTargetLayerInfo
      // takes, minus the /Game package it always mints. See the helper's comment for why the
      // engine factory is not called outright.
      const FLandscapePaintLayerInfoCreation Creation =
          CreateLandscapePaintTargetLayerInfo(Landscape, RequestedLayer);
      ULandscapeLayerInfoObject* NewLayerInfo = Creation.LayerInfo;

      if (!NewLayerInfo) {
        Responder.SendError(ErrorCodes::ERR_LAYER_CREATION_FAILED,
            FString::Printf(TEXT("Failed to create a ULandscapeLayerInfoObject for layer '%s'"), *LayerName));
        return;
      }
      LayerInfoTemplatePath = Creation.TemplatePath;

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
      NewLayerInfo->SetLayerName(RequestedLayer, true);
#else
      PRAGMA_DISABLE_DEPRECATION_WARNINGS
      NewLayerInfo->LayerName = RequestedLayer;
      PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif

      // The blend method, which nothing in this plugin used to name at all. Left alone ONLY
      // when the object came from a project template and the caller did not ask: that
      // template IS the project's answer to this question, and overwriting it would
      // re-disconnect the control this branch just reconnected. Otherwise the constructor's
      // value comes from ULandscapeSettings::TargetLayerDefaultBlendMethod, whose compiled
      // default is None, and a None layer can never join the group normalisation this verb
      // advertises — so it is set explicitly here rather than inherited.
      if (bWeightBlendedSupplied || !Creation.bDuplicatedFromTemplate) {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        NewLayerInfo->SetBlendMethod(bWeightBlended
                ? ELandscapeTargetLayerBlendMethod::FinalWeightBlending
                : ELandscapeTargetLayerBlendMethod::None,
            /*bInModify=*/false);
#else
        NewLayerInfo->bNoWeightBlend = !bWeightBlended;
#endif
      }

      // Bind the new LayerInfo where the binding PERSISTS, before touching
      // ULandscapeInfo::Layers. Layers is derived state: from 5.5 on,
      // UpdateLayerInfoMapInternal swaps it out and rebuilds it PURELY from
      // ALandscape::TargetLayers (Landscape.cpp:4582-4595) — it no longer harvests
      // component weightmap allocations the way 5.3/5.4 did — so a binding written only
      // into Layers survives exactly until the next UpdateLayerInfoMap(), which this
      // handler itself calls at the top of every paint. Once unbound, the layer drops out
      // of ALandscape::GetTargetLayerNames() (LandscapeEdit.cpp:8323, which filters on
      // LayerInfoObj != nullptr), so the next weightmap merge does not request it
      // (LandscapeEditLayers.cpp:5883) and ReallocateLayersWeightmaps DELETES its
      // allocation from every resolved component (:5744-5755). That is the destruction in
      // B-paint-layer-destroys-other-layer-weights, and it explains the reach the region
      // and SetAlphaData could not: painting layer B unbinds layer A and the following
      // merge erases A's weight over the WHOLE landscape, with no relation to `region`.
      // CreateTargetLayerSettingsFor is the engine's own helper for this
      // (Landscape.cpp:4478 — Update- or AddTargetLayer per proxy); the Target Layers
      // panel and ALandscapeProxy::Import (LandscapeEdit.cpp:3797, right after its own
      // SetAlphaData) register through the same API.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
      LandscapeInfo->CreateTargetLayerSettingsFor(NewLayerInfo);
#else
      // 5.3/5.4 have no TargetLayers map; the equivalent persistent home is the proxy's
      // EditorLayerSettings. (CreateTargetLayerSettingsFor does not exist there. The reverse
      // is also true and this comment used to get it wrong: CreateLayerEditorSettingsFor is
      // NOT a deprecated empty stub on 5.5+ — a recursive grep over C:/UE_5.8/Engine/Source
      // finds the symbol nowhere at all. It was removed outright, so this call has no 5.8
      // fallback and must stay inside the pre-5.5 branch.)
      LandscapeInfo->CreateLayerEditorSettingsFor(NewLayerInfo);
#endif

      // Re-scan rather than reuse ExistingLayerIndex: the registration above runs
      // PostEditChangeProperty on the landscape, which can rebuild Layers and invalidate
      // the index taken before it.
      ExistingLayerIndex = INDEX_NONE;
      for (int32 i = 0; i < LandscapeInfo->Layers.Num(); ++i) {
        if (LandscapeInfo->Layers[i].LayerName == RequestedLayer) {
          ExistingLayerIndex = i;
          break;
        }
      }
      if (ExistingLayerIndex != INDEX_NONE) {
        LandscapeInfo->Layers[ExistingLayerIndex].LayerInfoObj = NewLayerInfo;
      } else {
        LandscapeInfo->Layers.Add(FLandscapeInfoLayerSettings(NewLayerInfo, Landscape));
      }
      LayerInfo = NewLayerInfo;
      bLayerInfoAutoCreated = true;

      UE_LOG(LogPinWrightSubsystem, Display,
             TEXT("landscape.create_procedural_terrain: auto-created a LayerInfo for material target layer '%s' on '%s'"),
             *LayerName, *ActorLabel);
      NoteWarning(FString::Printf(
          TEXT("Target layer '%s' had no ULandscapeLayerInfoObject; one was created inside the landscape actor's package rather than as a shared /Game LayerInfo asset. ")
          TEXT("Create a proper asset with material.authoring.add_landscape_layer and assign it if this layer is shared between landscapes."),
          *LayerName));
      if (!LayerInfoTemplatePath.IsEmpty()) {
        NoteWarning(FString::Printf(
            TEXT("It was DUPLICATED from the project's ULandscapeSettings::DefaultLayerInfoObject template '%s', so it carries that template's blend method, blend group and physical material rather than freshly constructed defaults."),
            *LayerInfoTemplatePath));
      }
    } else if (bWeightBlendedSupplied) {
      // Supplied for a layer that already has a LayerInfo. That object is a shared asset —
      // rewriting its blend method here would change every landscape referencing it, from a
      // paint call that never said so. Report the no-op instead of performing it silently.
      NoteWarning(FString::Printf(
          TEXT("weightBlended was supplied but layer '%s' already has a ULandscapeLayerInfoObject, so it was NOT applied: this verb never rewrites the blend method of an existing (potentially shared) LayerInfo asset. The measured method is reported as layerBlendMethod; change it with material.authoring.add_landscape_layer or in Landscape Ed Mode's Target Layers panel."),
          *LayerName));
    }

    // SetAlphaData asserts check(LayerInfo != nullptr); never let a resolution miss reach it.
    if (!LayerInfo) {
      Responder.SendError(ErrorCodes::ERR_LAYER_CREATION_FAILED,
          FString::Printf(TEXT("Could not resolve a ULandscapeLayerInfoObject for layer '%s'"), *LayerName));
      return;
    }

    // ---- The blend method, MEASURED off the object the write will bind to ----
    // Read after the auto-create branch so it describes the layer actually used, whether this
    // call manufactured it, a template supplied it, or it was already on the landscape. A
    // layer on None is excluded from the merge's final weight-blending pass
    // (LandscapeEditLayers.cpp:3594), so the group normalisation this verb's summary describes
    // simply does not happen for it — which is a fact about the result, not about the request.
    const FString LayerBlendMethod = LandscapeTargetLayerBlendMethodName(LayerInfo);
    if (!LandscapeTargetLayerIsWeightBlended(LayerInfo)) {
      NoteWarning(FString::Printf(
          TEXT("Layer '%s' has blend method '%s', so it takes NO part in group normalisation: the merge's final weight-blending pass admits only FinalWeightBlending layers, and painting this one at strength 1.0 will NOT reduce any sibling's weight. Two layers can therefore both hold 255 on the same texel, which a normalised weightmap cannot. Recreate the layer with weightBlended=true, or set Blend Method on its ULandscapeLayerInfoObject."),
          *LayerName, *LayerBlendMethod));
    }

    // ---- All-layers, whole-extent weight census: the "before" half ----
    // The verify block below used to read exactly ONE layer (the requested one) over
    // exactly ONE rectangle (the requested region), so it was structurally blind on both
    // axes along which a weight paint can destroy data: another ULandscapeLayerInfoObject,
    // and a texel outside `region`. texelsWithWeight == paintedTexels was therefore green
    // in the same run that emptied every sibling layer
    // (B-paint-layer-destroys-other-layer-weights).
    //
    // Painting a weight-blended layer IS a blend-group operation. The engine's own paint
    // stroke (FLandscapeToolStrokePaint::Apply / GetAffectedTargetLayersForTarget,
    // LandscapeEdModePaintTools.cpp) enumerates the target layer's blend group and writes
    // EVERY member of it — the painted layer up, the siblings down — strictly inside the
    // brush bounds. So siblings losing weight INSIDE the region is the engine's
    // normalization working as designed and is reported as a number, not a warning.
    // Siblings losing weight OUTSIDE the region is the defect, and otherLayerTexelsLost is
    // the field that reveals it.
    struct FLayerWeightCensus {
      FName LayerName;
      ULandscapeLayerInfoObject* Info = nullptr;
      int32 TotalBefore = 0;
      int32 TotalAfter = 0;
      int32 OutsideBefore = 0;
      int32 OutsideAfter = 0;
    };
    TArray<FLayerWeightCensus> Census;

    const int64 FullTexels =
        (int64)(FullMaxX - FullMinX + 1) * (int64)(FullMaxY - FullMinY + 1);
    // One full-extent read per layer, taken twice, is the honest census; on a large
    // landscape it is also the dominant cost of the call. Cap it and SAY the census was
    // skipped rather than quietly turning every paint on a big terrain into a long scan.
    constexpr int64 MaxCensusTexels = 4 * 1024 * 1024;
    const bool bCensus = bVerify && FullTexels > 0 && FullTexels <= MaxCensusTexels;

    // Tallies one layer's texels carrying any weight over the FULL extent, split by
    // whether they fall inside the painted region.
    auto SampleLayerWeights = [&](ULandscapeLayerInfoObject* Info, int32& OutTotal, int32& OutOutside) {
      OutTotal = 0;
      OutOutside = 0;
      const int32 SizeX = FullMaxX - FullMinX + 1;
      const int32 SizeY = FullMaxY - FullMinY + 1;
      TArray<uint8> Weights;
      Weights.SetNumZeroed(SizeX * SizeY);
      FLandscapeEditDataInterface CensusRead(LandscapeInfo, /*bUploadTextureChangesToGPU=*/false);
      // A read: same non-dirtying treatment as the single-layer readback below, and this
      // half runs BEFORE the write, so a read-dirty here would be a real regression.
      MakeLandscapeEditInterfaceReadOnly(CensusRead);
      CensusRead.GetWeightDataFast(Info, FullMinX, FullMinY, FullMaxX, FullMaxY,
                                   Weights.GetData(), /*Stride=*/0);
      for (int32 Y = 0; Y < SizeY; ++Y) {
        for (int32 X = 0; X < SizeX; ++X) {
          if (Weights[X + Y * SizeX] == 0) { continue; }
          ++OutTotal;
          const int32 WorldX = FullMinX + X;
          const int32 WorldY = FullMinY + Y;
          if (WorldX < PaintMinX || WorldX > PaintMaxX ||
              WorldY < PaintMinY || WorldY > PaintMaxY) {
            ++OutOutside;
          }
        }
      }
    };

    if (bCensus) {
      // Settle first, or the two halves are not comparable: a preceding skipFlush batch
      // leaves a regeneration pending, so an unsettled "before" read would be attributed
      // to THIS paint as sibling loss. A no-op when nothing is outstanding.
      SettleLandscapeLayers(Landscape);

      const FName& CensusVisibilityLayer = GetLandscapeVisibilityLayerName();
      for (const FLandscapeInfoLayerSettings& Setting : LandscapeInfo->Layers) {
        // The visibility layer is the engine's hole mask, not a weight-blended layer: it
        // is not in any blend group and normalization never touches it.
        if (!Setting.LayerInfoObj || Setting.LayerName == CensusVisibilityLayer) { continue; }
        FLayerWeightCensus Entry;
        Entry.LayerName = Setting.LayerName;
        Entry.Info = Setting.LayerInfoObj;
        SampleLayerWeights(Entry.Info, Entry.TotalBefore, Entry.OutsideBefore);
        Census.Add(Entry);
      }
    } else if (bVerify) {
      NoteWarning(FString::Printf(
          TEXT("Landscape extent is %lld texels, above the %lld-texel all-layers census cap, so layersAffected[] and otherLayerTexelsLost were NOT measured for this call. ")
          TEXT("Painting a weight-blended layer renormalizes its whole blend group; check the other layers in Landscape Ed Mode before trusting this result."),
          FullTexels, MaxCensusTexels));
    }

    FScopedSlowTask SlowTask(1.0f, FText::FromString(TEXT("Painting landscape layer...")));

    const uint8 PaintValue = static_cast<uint8>(FMath::RoundToInt(Strength * 255.0));
    if (PaintValue == 0) {
      // Weight 0 is a legitimate ERASE, but it is not what a caller who asked for a faint
      // layer expects — a strength below 1/510 quantizes to zero. Say so either way.
      NoteWarning(bStrengthSupplied && Strength > 0.0
          ? FString::Printf(
                TEXT("strength %g quantizes to weightmap value 0 (weights are 8-bit), so this call ERASES layer '%s' over the region rather than painting it. Use strength >= %g to write a non-zero weight."),
                Strength, *LayerName, 1.0 / 510.0)
          : FString::Printf(
                TEXT("strength 0 writes weightmap value 0, which ERASES layer '%s' over the region rather than painting it."),
                *LayerName));
    }

    // The whole write, in ONE transaction. MarkLevelActorModified is Modify() +
    // MarkPackageDirty(), and a bare Modify() outside a transaction records nothing, so
    // until this scope existed editor.undo could not reverse a paint — the verb where
    // that costs the most, because the write renormalizes a whole blend group
    // (B-paint-layer-destroys-other-layer-weights). Modify() still runs BEFORE the
    // mutation so the pre-edit state is what lands in the buffer. Deliberately no
    // MarkRenderStateDirty(): SetAlphaData + Flush is an engine path that pushes its own
    // render state, and EnvironmentDirtyUtils.h reserves MarkComponentRenderStateDirty
    // for raw field assignments. The transaction closes before the settle below, which is
    // a regeneration rather than part of the edit.
    {
      const FScopedTransaction Transaction(FText::FromString(TEXT("Paint Landscape Layer")));
      PinWright::MarkLevelActorModified(Landscape);

      TArray<uint8> AlphaData;
      AlphaData.Init(PaintValue, RegionTexels);

      // bUploadTextureChangesToGPU = true (was false): on an edit-layer landscape the GPU
      // weightmap merge reads the GPU edit-layer texture, so Flush() must upload the CPU
      // write or the merge composites stale data — the same reason landscape.edit passes
      // true for heights.
      FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo, /*bUploadTextureChangesToGPU=*/true);
      const FGuid EditLayerGuid = GetDefaultEditLayerGuid(Landscape);
      {
        // Scope the write to the landscape's default edit layer. Outside Landscape Ed Mode
        // nothing is "being edited", so ALandscape::GetEditingLayer() is invalid and a
        // shared-layer write lands in NO persistent edit layer; the next weightmap
        // regeneration then composites the (unchanged) edit layers over the base and the
        // paint disappears. That is the identical mechanism GetDefaultEditLayerGuid
        // documents for heights — weightmaps regenerate through the same edit-layer merge —
        // and it is the second way this verb could report a paint that never landed. On a
        // non-edit-layer landscape the GUID is invalid and the scope is a no-op.
        //
        // The completion callback asks for a WEIGHTMAP recomposite, NOT
        // RequestLayersContentUpdateForceAll(). ForceAll walks every proxy and calls
        // RequestHeightmapUpdate + RequestWeightmapUpdate(bUpdateAll=true) on EVERY
        // component (LandscapeEditLayers.cpp:6739-6766) — a forced heightmap AND weightmap
        // regeneration of the entire landscape in response to a write bounded to `region`,
        // and a step whose reach is not the region. (It was NOT, as this comment used to
        // claim, the ONLY such step, and it was not the one that destroyed data: that was
        // the LayerInfo binding never reaching ALandscape::TargetLayers — see the
        // CreateTargetLayerSettingsFor note in the auto-create branch above. Narrowing
        // this callback is still correct; it just was not the bug.) It was redundant
        // as well as wide: SetAlphaData already calls Component->RequestWeightmapUpdate()
        // on exactly the components it wrote to (LandscapeEditInterface.cpp:2229, 2369),
        // which schedules the merge and is what SettleLandscapeLayers below drains.
        // Update_Weightmap_All is the mode the engine itself passes from inside an
        // editing-layer scope after a weightmap edit (LandscapeEditLayers.cpp:8454,
        // LandscapeEdMode.cpp:3470).
        FScopedSetLandscapeEditingLayer EditingLayerScope(Landscape, EditLayerGuid,
            [Landscape] { Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_Weightmap_All); });
        LandscapeEdit.SetAlphaData(LayerInfo, PaintMinX, PaintMinY, PaintMaxX,
                                   PaintMaxY, AlphaData.GetData(), RegionSizeX);
        if (!bSkipFlush) {
          LandscapeEdit.Flush();
        }
      }
    }

    // ---- Verification readback ----
    // The previous bare-Flush comment argued a weight paint needs no settle because it
    // cannot move bounds, and asked that any future weight-dependent readback route
    // through the settle helper AND ship its own counterfactual test. That is exactly what
    // this is: the settle drains the deferred edit-layer regeneration the scope above
    // requested, so the composited weightmap is current before it is sampled. Opt out with
    // verify:false (or skipFlush:true) to keep the old cost profile in a batch.
    int32 SampledTexels = 0;
    int32 TexelsWithWeight = 0;
    int32 TexelsAtRequestedWeight = 0;
    int32 OtherLayerTexelsLost = 0;
    int32 OtherLayerTexelsLostInRegion = 0;
    FLandscapeAllocationBlastRadius BlastRadius;
    if (bVerify) {
      SettleLandscapeLayers(Landscape);

      TArray<uint8> ReadBack;
      ReadBack.SetNumZeroed(RegionTexels);
      FLandscapeEditDataInterface LandscapeRead(LandscapeInfo, /*bUploadTextureChangesToGPU=*/false);
      // Weightmap verification readback: a read. GetWeightDataFast reaches the same
      // GetTextureDataInfo -> Texture->Modify path as the height read
      // (LandscapeEditInterface.cpp:2650,2662), and MarkLevelActorModified above already
      // recorded the paint.
      MakeLandscapeEditInterfaceReadOnly(LandscapeRead);
      LandscapeRead.GetWeightDataFast(LayerInfo, PaintMinX, PaintMinY, PaintMaxX, PaintMaxY,
                                      ReadBack.GetData(), /*Stride=*/0);
      SampledTexels = ReadBack.Num();
      for (uint8 Sample : ReadBack) {
        if (Sample != 0) { ++TexelsWithWeight; }
        if (Sample == PaintValue) { ++TexelsAtRequestedWeight; }
      }

      // The honest silent-elision signal. Deliberately keyed on "any weight at all" rather
      // than an exact match: the composited weightmap normalizes the whole weight-blended
      // blend group (FLandscapeEditLayersWeightmapsPerformFinalWeightBlendingPS,
      // LandscapeEditLayers.cpp:791), so an intermediate strength legitimately reads back
      // at a different value — but a paint that landed can never read back as all-zero.
      // NOTE the correction: SetAlphaData itself does NOT renormalize. On 5.8 the
      // single-layer overload (LandscapeEditInterface.cpp:2106) writes only
      // LayerDataPtrs[UpdateLayerIdx] and never touches a sibling channel, and the
      // 10-argument overload (:2378) discards bWeightAdjust / bTotalWeightAdjust and
      // forwards to it. The renormalization is the edit-layer merge's, not the write's.
      if (PaintValue > 0 && TexelsWithWeight == 0) {
        NoteWarning(FString::Printf(
            TEXT("Weightmap readback found 0 of %d texels carrying any weight for layer '%s' after the paint. ")
            TEXT("The layer is declared by the material, so the write was accepted — inspect the layer in Landscape Ed Mode before trusting this result."),
            SampledTexels, *LayerName));
      }

      // ---- All-layers, whole-extent weight census: the "after" half ----
      // Same read, same layers, same extent as the "before" half above, so the two are
      // comparable texel for texel.
      for (FLayerWeightCensus& Entry : Census) {
        SampleLayerWeights(Entry.Info, Entry.TotalAfter, Entry.OutsideAfter);
      }
      for (const FLayerWeightCensus& Entry : Census) {
        if (Entry.LayerName == RequestedLayer) { continue; }
        OtherLayerTexelsLost += FMath::Max(0, Entry.OutsideBefore - Entry.OutsideAfter);
        OtherLayerTexelsLostInRegion += FMath::Max(0,
            (Entry.TotalBefore - Entry.OutsideBefore) - (Entry.TotalAfter - Entry.OutsideAfter));
      }

      // Loss inside the region is the blend group renormalizing and is reported as a
      // number only. Loss OUTSIDE the region is weight the caller never put at risk, and
      // it is the whole point of this census.
      if (OtherLayerTexelsLost > 0) {
        NoteWarning(FString::Printf(
            TEXT("%d texels of weight on OTHER layers were lost OUTSIDE the painted region [%d,%d]..[%d,%d]. ")
            TEXT("A region-bounded paint must not change any layer outside its region; see layersAffected[] for the per-layer before/after counts. ")
            TEXT("editor.undo will NOT recover this: the loss happens in the edit-layer weightmap merge, which runs inside the settle AFTER this call's transaction has closed, so only the SetAlphaData write is in the undo buffer. Re-paint the affected layers from a source you still have."),
            OtherLayerTexelsLost, PaintMinX, PaintMinY, PaintMaxX, PaintMaxY));
      }

      // ---- Per-component allocation coverage: the "after" half ----
      // Taken after the settle, because the BASE allocation arrays are rewritten by the
      // edit-layer merge the settle drains, not by SetAlphaData.
      FLandscapeComponentAllocationCoverage AllocationAfter;
      ScanLandscapeComponentAllocationCoverage(LandscapeInfo, AllocationAfter);
      DiffLandscapeComponentAllocationCoverage(AllocationBefore, AllocationAfter,
                                               PaintableLayers, BlastRadius);

      // The census above just certified that no layer lost a texel. This is the damage that
      // certificate cannot cover: gaining an allocation costs the layers that did NOT gain
      // one, across the whole of every component involved. Warn only when a material-declared
      // layer actually ends up unallocated somewhere the paint touched — a component that
      // already carried every declared layer gains nothing and loses nothing.
      if (BlastRadius.LayersWithoutAllocation.Num() > 0) {
        TArray<FString> ZeroedParts;
        ZeroedParts.Reserve(BlastRadius.LayersWithoutAllocation.Num());
        for (const TPair<FName, int32>& Pair : BlastRadius.LayersWithoutAllocation) {
          ZeroedParts.Add(FString::Printf(TEXT("'%s' (%d component%s)"),
              *Pair.Key.ToString(), Pair.Value, Pair.Value == 1 ? TEXT("") : TEXT("s")));
        }
        FString BlastMessage = FString::Printf(
            TEXT("This paint gave %d component(s) a new weightmap allocation, %d of them their FIRST. ")
            TEXT("A landscape component's material permutation is built from THAT component's own allocation list, ")
            TEXT("so on those components every material-declared layer holding no allocation there now samples as a ")
            TEXT("constant 0 across the WHOLE component rather than only inside the painted region: %s."),
            BlastRadius.ComponentsGainingAllocation, BlastRadius.ComponentsGainingFirstAllocation,
            *FString::Join(ZeroedParts, TEXT(", ")));
        if (BlastRadius.bFootprintMeasured) {
          const FVector FootprintSize = BlastRadius.FootprintBox.GetSize();
          const double RequestedArea = RequestedFootprintUUX * RequestedFootprintUUY;
          const double MeasuredArea = FootprintSize.X * FootprintSize.Y;
          BlastMessage += FString::Printf(
              TEXT(" MEASURED footprint of those components: %.0f x %.0f uu at [%.0f,%.0f]..[%.0f,%.0f]; ")
              TEXT("the region REQUESTED covers %.0f x %.0f uu (one component is %.0f x %.0f uu)."),
              FootprintSize.X, FootprintSize.Y,
              BlastRadius.FootprintBox.Min.X, BlastRadius.FootprintBox.Min.Y,
              BlastRadius.FootprintBox.Max.X, BlastRadius.FootprintBox.Max.Y,
              RequestedFootprintUUX, RequestedFootprintUUY, ComponentSizeUUX, ComponentSizeUUY);
          // Measured and requested disagreeing is the whole finding, so it is stated as a
          // ratio rather than left for the caller to divide.
          if (RequestedArea > UE_DOUBLE_SMALL_NUMBER && MeasuredArea > RequestedArea) {
            BlastMessage += FString::Printf(TEXT(" That is %.1fx the requested area."),
                MeasuredArea / RequestedArea);
          }
        }
        BlastMessage +=
            TEXT(" NOTHING WAS ERASED: no weightmap byte changed, editor.undo is not needed, and repainting the")
            TEXT(" affected layers over those components restores them exactly. otherLayerTexelsLost reports 0")
            TEXT(" TRUTHFULLY over this — it is a difference of two texel counts and cannot express an")
            TEXT(" allocation-side change. Build multi-layer terrain by painting the base layer over the FULL")
            TEXT(" extent first, then accents over their regions, so no component takes its first allocation")
            TEXT(" from an accent.");
        NoteWarning(BlastMessage);
      }
    } else if (bVerifySkippedByFlush) {
      NoteWarning(TEXT("verify was requested but skipped because skipFlush=true: the write has not been flushed, so a weightmap readback would not reflect it. Re-run the last paint of the batch with skipFlush=false to verify."));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("landscapePath"), Landscape->GetPackage()->GetPathName());
    Resp->SetStringField(TEXT("landscapeName"), ActorLabel);
    Resp->SetStringField(TEXT("layerName"), LayerName);
    Resp->SetStringField(TEXT("materialPath"), MaterialPath);
    Resp->SetNumberField(TEXT("strength"), Strength);
    Resp->SetNumberField(TEXT("paintValue"), PaintValue);
    Resp->SetBoolField(TEXT("layerInfoAutoCreated"), bLayerInfoAutoCreated);
    // MEASURED off the ULandscapeLayerInfoObject the write bound to, never echoed from the
    // weightBlended request: an existing layer keeps whatever method its asset carries.
    // Omitted rather than guessed if the name could not be read.
    if (!LayerBlendMethod.IsEmpty()) {
      Resp->SetStringField(TEXT("layerBlendMethod"), LayerBlendMethod);
    }
    // Present only when the auto-create duplicated the project's DefaultLayerInfoObject, so
    // its absence means "constructed fresh", not "template unknown".
    if (!LayerInfoTemplatePath.IsEmpty()) {
      Resp->SetStringField(TEXT("layerInfoTemplatePath"), LayerInfoTemplatePath);
    }
    Resp->SetBoolField(TEXT("flushSkipped"), bSkipFlush);
    Resp->SetBoolField(TEXT("verified"), bVerify);
    Resp->SetNumberField(TEXT("regionSizeX"), RegionSizeX);
    Resp->SetNumberField(TEXT("regionSizeY"), RegionSizeY);
    Resp->SetNumberField(TEXT("paintedTexels"), RegionTexels);
    // The orphan scan always runs, so its counts always travel — and they travel WITH the
    // loaded-proxies-only flag, because a bare 0 here would read as "this landscape has no
    // orphaned weight" when what was measured is "none among the components that were resident".
    Resp->SetNumberField(TEXT("orphanScanComponents"), OrphanScan.ComponentsScanned);
    Resp->SetNumberField(TEXT("orphanScanProxies"), OrphanScan.ProxiesScanned);
    Resp->SetBoolField(TEXT("orphanScanLoadedProxiesOnly"), true);
    if (OrphanScan.Orphans.Num() > 0) {
      Resp->SetArrayField(TEXT("orphanedLayers"), MakeLandscapeOrphanedLayerJson(OrphanScan));
      Resp->SetNumberField(TEXT("orphanedLayerCount"), OrphanScan.LiveOrphanCount);
    }
    // The unit the per-component blast radius is measured in, and the request it is compared
    // against. Both are properties of the landscape and the call rather than of the write, so
    // they travel whether or not `verify` measured anything — a caller who batches with
    // verify:false still needs them to reason about the order of work.
    if (LandscapeComponentSizeQuads > 0) {
      Resp->SetNumberField(TEXT("componentSizeQuads"), LandscapeComponentSizeQuads);
      TSharedPtr<FJsonObject> ComponentSizeJson = MakeShared<FJsonObject>();
      ComponentSizeJson->SetNumberField(TEXT("x"), ComponentSizeUUX);
      ComponentSizeJson->SetNumberField(TEXT("y"), ComponentSizeUUY);
      Resp->SetObjectField(TEXT("componentSizeUU"), ComponentSizeJson);
    }
    TSharedPtr<FJsonObject> RequestedFootprintJson = MakeShared<FJsonObject>();
    RequestedFootprintJson->SetNumberField(TEXT("sizeX"), RequestedFootprintUUX);
    RequestedFootprintJson->SetNumberField(TEXT("sizeY"), RequestedFootprintUUY);
    Resp->SetObjectField(TEXT("requestedFootprintUU"), RequestedFootprintJson);
    if (bVerify) {
      Resp->SetNumberField(TEXT("sampledTexels"), SampledTexels);
      Resp->SetNumberField(TEXT("texelsWithWeight"), TexelsWithWeight);
      Resp->SetNumberField(TEXT("texelsAtRequestedWeight"), TexelsAtRequestedWeight);
      // The ALLOCATION side. Deliberately not inside the bCensus block below: this is a
      // pointer walk, so the census texel cap does not apply to it and a landscape too big
      // for layersAffected[] still gets its blast radius measured.
      Resp->SetNumberField(TEXT("componentsGainingAllocation"), BlastRadius.ComponentsGainingAllocation);
      Resp->SetNumberField(TEXT("componentsGainingFirstAllocation"), BlastRadius.ComponentsGainingFirstAllocation);
      if (BlastRadius.LayersWithoutAllocation.Num() > 0) {
        TArray<TSharedPtr<FJsonValue>> ZeroSampled;
        ZeroSampled.Reserve(BlastRadius.LayersWithoutAllocation.Num());
        for (const TPair<FName, int32>& Pair : BlastRadius.LayersWithoutAllocation) {
          TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
          Entry->SetStringField(TEXT("layerName"), Pair.Key.ToString());
          Entry->SetNumberField(TEXT("componentCount"), Pair.Value);
          ZeroSampled.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Resp->SetArrayField(TEXT("layersNowSampledZeroOnTouchedComponents"), ZeroSampled);
      }
      // Omitted, not zeroed, when no component gained an allocation: an all-zero box would
      // read as a measured footprint at the world origin.
      if (BlastRadius.bFootprintMeasured) {
        const FVector FootprintSize = BlastRadius.FootprintBox.GetSize();
        TSharedPtr<FJsonObject> FootprintJson = MakeShared<FJsonObject>();
        FootprintJson->SetNumberField(TEXT("minX"), BlastRadius.FootprintBox.Min.X);
        FootprintJson->SetNumberField(TEXT("minY"), BlastRadius.FootprintBox.Min.Y);
        FootprintJson->SetNumberField(TEXT("maxX"), BlastRadius.FootprintBox.Max.X);
        FootprintJson->SetNumberField(TEXT("maxY"), BlastRadius.FootprintBox.Max.Y);
        FootprintJson->SetNumberField(TEXT("sizeX"), FootprintSize.X);
        FootprintJson->SetNumberField(TEXT("sizeY"), FootprintSize.Y);
        Resp->SetObjectField(TEXT("allocationFootprintUU"), FootprintJson);
      }
    }
    // The census travels only when it actually ran, so a landscape over the cap reports no
    // number rather than a zero that would read as "nothing was lost".
    if (bCensus) {
      TArray<TSharedPtr<FJsonValue>> LayersAffected;
      LayersAffected.Reserve(Census.Num());
      for (const FLayerWeightCensus& Entry : Census) {
        TSharedPtr<FJsonObject> LayerJson = MakeShared<FJsonObject>();
        LayerJson->SetStringField(TEXT("layerName"), Entry.LayerName.ToString());
        LayerJson->SetBoolField(TEXT("painted"), Entry.LayerName == RequestedLayer);
        LayerJson->SetNumberField(TEXT("texelsWithWeightBefore"), Entry.TotalBefore);
        LayerJson->SetNumberField(TEXT("texelsWithWeightAfter"), Entry.TotalAfter);
        LayerJson->SetNumberField(TEXT("texelsWithWeightOutsideRegionBefore"), Entry.OutsideBefore);
        LayerJson->SetNumberField(TEXT("texelsWithWeightOutsideRegionAfter"), Entry.OutsideAfter);
        // Whether this sibling is even eligible for the group normalisation the before/after
        // counts are read against. Without it a caller cannot tell "the paint did not displace
        // this layer" from "this layer is not in the blend group at all".
        const FString EntryBlendMethod = LandscapeTargetLayerBlendMethodName(Entry.Info);
        if (!EntryBlendMethod.IsEmpty()) {
          LayerJson->SetStringField(TEXT("blendMethod"), EntryBlendMethod);
        }
        LayersAffected.Add(MakeShared<FJsonValueObject>(LayerJson));
      }
      Resp->SetArrayField(TEXT("layersAffected"), LayersAffected);
      Resp->SetNumberField(TEXT("censusTexels"), (double)FullTexels);
      Resp->SetNumberField(TEXT("otherLayerTexelsLost"), OtherLayerTexelsLost);
      Resp->SetNumberField(TEXT("otherLayerTexelsLostInRegion"), OtherLayerTexelsLostInRegion);
    }
    TSharedPtr<FJsonObject> RegionResp = MakeShared<FJsonObject>();
    RegionResp->SetNumberField(TEXT("minX"), PaintMinX);
    RegionResp->SetNumberField(TEXT("minY"), PaintMinY);
    RegionResp->SetNumberField(TEXT("maxX"), PaintMaxX);
    RegionResp->SetNumberField(TEXT("maxY"), PaintMaxY);
    Resp->SetObjectField(TEXT("region"), RegionResp);
    if (Warnings.Num() > 0) {
      Resp->SetArrayField(TEXT("warnings"), Warnings);
    }
    AddActorVerification(Resp, Landscape);

    Responder.SendSuccess(TEXT("Layer painted successfully"), Resp);
  });
}

// ---- landscape.audit_shape ----
//
// The first verifier for a claim landscape.sculpt has been making about itself: that its
// strokes follow offset curves "rather than stepping along cell edges". That sentence was a
// statement about the verb's own output with nothing measuring it on arbitrary content.
//
// Read-only, and it must STAY read-only in the strict sense: see the read-dirtying note at the
// top of this file. It uses the same GetHeightData path as landscape.get_heights and therefore
// needs both halves of that fix.
//
// A shape defect is CONTENT, never a transport error: this verb returns SendSuccess with
// pass:false and structured findings. It sends an RPC error only for a malformed request.
REGISTER_RPC_HANDLER("landscape.audit_shape", "landscape", "Measure whether a landscape's height-change boundaries CURVE or follow the heightfield's own cell lattice — the shape verifier for landscape.sculpt's claim that its strokes do not step along cell edges, and the check that distinguishes an organically sculpted slope from terrain stamped one tier value per cell. Returns axisFraction (fraction of stride-K band-boundary chords running along a cell axis) and stepFraction (share of the region's height rise carried by isolated single-sample risers), plus per-check findings. Read-only: the level's dirty flag is preserved exactly, clean or dirty. A shape defect returns success with pass:false, never an RPC error.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("landscapePath"), TEXT("path"),
            TEXT("Object path of the landscape; either this or landscapeName must be provided. Snake_case landscape_path accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("landscapePath"), TEXT("landscape_path")})),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("landscapeName"), TEXT("string"),
            TEXT("Display label of the landscape actor in the level. Used when landscapePath is empty. Snake_case landscape_name accepted."),
            /*bRequired=*/false, TArray<FString>({TEXT("landscapeName"), TEXT("landscape_name")})),
        RPC_PARAM_OPT("region", "object", "Heightmap-pixel region {minX, minY, maxX, maxY} to measure. Defaults to the full landscape extent. A region no landscape component covers, wholly or partly, is rejected rather than measured over the engine's interpolated fill — a verdict derived partly from invented terrain would be a different measurement wearing this one's name."),
        RPC_PARAM_OPT("checks", "array", "Wire ids of the checks to run: axis_locked, stepped_profile. Both run by default. An unknown id is rejected with INVALID_ARGUMENT rather than silently skipped — a typo that ran nothing would be indistinguishable from terrain that passed."),
        RPC_PARAM_OPT("thresholds", "object", "Any of {stride, epsDeg, marginCells, bandCount, bandHeight, minChords, minStepUnits, maxAxisFraction, maxStepFraction}. Every value used is echoed in the response."),
        RPC_PARAM_DEF("failOn", "string", "Severity that makes pass false: error (default), any, or none. An unrunnable check makes pass false regardless.", "error"),
        RPC_PARAM_DEF("includeHistogram", "boolean", "Emit the 18-bucket chord-angle histogram (5 degrees per bucket over 0..90) alongside axisFraction.", "false")
    ))
{
  FString LandscapePath = Ctx.GetStringFirstOf({TEXT("landscapePath"), TEXT("landscape_path")}, TEXT(""));
  const FString LandscapeName = Ctx.GetStringFirstOf({TEXT("landscapeName"), TEXT("landscape_name")}, TEXT(""));
  if (!LandscapePath.IsEmpty()) {
    const FString SafePath = SanitizeProjectRelativePath(LandscapePath);
    if (SafePath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe landscape path: %s"), *LandscapePath));
      return true;
    }
    LandscapePath = SafePath;
  }

  // ---- checks ----
  uint32 SelectedChecks = LandscapeShape::DefaultCheckMask();
  if (const TArray<TSharedPtr<FJsonValue>>* CheckArray = Ctx.GetArray(TEXT("checks"))) {
    SelectedChecks = 0;
    for (const TSharedPtr<FJsonValue>& Value : *CheckArray) {
      FString Id;
      LandscapeShape::ECheck Check;
      if (!Value.IsValid() || !Value->TryGetString(Id) || !LandscapeShape::ParseCheckId(Id, Check)) {
        const FString Valid = PinWrightAudit::ValidCheckIdList(LandscapeShape::AllChecks());
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown check '%s'. Valid ids: %s."), *Id, *Valid));
        return true;
      }
      SelectedChecks |= LandscapeShape::CheckBit(Check);
    }
    if (SelectedChecks == 0) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          TEXT("`checks` was empty, so nothing would be measured. Omit it to run every check."));
      return true;
    }
  }

  const FString FailOnToken = Ctx.GetString(TEXT("failOn"), TEXT("error")).ToLower();
  PinWrightAudit::EFailOn FailOnMode;
  if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOnMode)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("Unknown failOn '%s'. Valid: error, any, none."), *FailOnToken));
    return true;
  }
  const FString FailOn = PinWrightAudit::FailOnToWire(FailOnMode);

  // ---- thresholds ----
  LandscapeShape::FParams Params;
  if (const TSharedPtr<FJsonObject> T = Ctx.GetObject(TEXT("thresholds"))) {
    int32 I; double D;
    if (T->TryGetNumberField(TEXT("stride"), I))          { Params.Stride = FMath::Max(I, 1); }
    if (T->TryGetNumberField(TEXT("marginCells"), I))     { Params.MarginCells = FMath::Max(I, 0); }
    if (T->TryGetNumberField(TEXT("bandCount"), I))       { Params.BandCount = FMath::Max(I, 1); }
    if (T->TryGetNumberField(TEXT("bandHeight"), I))      { Params.BandHeight = FMath::Max(I, 0); }
    if (T->TryGetNumberField(TEXT("minChords"), I))       { Params.MinChords = FMath::Max(I, 1); }
    if (T->TryGetNumberField(TEXT("minStepUnits"), I))    { Params.MinStepUnits = FMath::Max(I, 1); }
    if (T->TryGetNumberField(TEXT("epsDeg"), D))          { Params.EpsDeg = D; }
    if (T->TryGetNumberField(TEXT("maxAxisFraction"), D)) { Params.MaxAxisFraction = D; }
    if (T->TryGetNumberField(TEXT("maxStepFraction"), D)) { Params.MaxStepFraction = D; }
  }

  // ---- resolve the landscape and the region ----
  FString ErrorMsg;
  ALandscape* Landscape = FindLandscapeByNameOrPath(LandscapePath, LandscapeName, ErrorMsg);
  if (!Landscape) {
    Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NOT_FOUND, ErrorMsg);
    return true;
  }
  ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
  if (!LandscapeInfo) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Landscape has no info"));
    return true;
  }
  int32 FullMinX, FullMinY, FullMaxX, FullMaxY;
  if (!LandscapeInfo->GetLandscapeExtent(FullMinX, FullMinY, FullMaxX, FullMaxY)) {
    if (LandscapeInfo->XYtoComponentMap.Num() == 0) {
      Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NO_COMPONENTS,
          FString::Printf(TEXT("Landscape '%s' has no registered ULandscapeComponents (hollow landscape), so it has no readable extent."),
              *Landscape->GetActorLabel()));
      return true;
    }
    Ctx.SendError(ErrorCodes::ERR_INVALID_LANDSCAPE, TEXT("Failed to get landscape extent"));
    return true;
  }

  TOptional<int32> ReqMinX, ReqMinY, ReqMaxX, ReqMaxY;
  if (const TSharedPtr<FJsonObject> RegionObj = Ctx.GetObject(TEXT("region"))) {
    int32 V;
    if (RegionObj->TryGetNumberField(TEXT("minX"), V)) { ReqMinX = V; }
    if (RegionObj->TryGetNumberField(TEXT("minY"), V)) { ReqMinY = V; }
    if (RegionObj->TryGetNumberField(TEXT("maxX"), V)) { ReqMaxX = V; }
    if (RegionObj->TryGetNumberField(TEXT("maxY"), V)) { ReqMaxY = V; }
  }
  const LandscapeHeightStats::FResolvedHeightRegion Region = LandscapeHeightStats::ResolveHeightRegion(
      ReqMinX, ReqMinY, ReqMaxX, ReqMaxY, FullMinX, FullMinY, FullMaxX, FullMaxY);
  if (!Region.bValid) {
    // A region wholly outside the extent used to clamp onto the nearest edge pixel and be
    // measured as if the caller had asked for it. Both rectangles go into the refusal.
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(
            TEXT("Region [%d,%d]..[%d,%d] %s the landscape extent [%d,%d]..[%d,%d], so there is nothing to measure."),
            ReqMinX.Get(FullMinX), ReqMinY.Get(FullMinY), ReqMaxX.Get(FullMaxX), ReqMaxY.Get(FullMaxY),
            Region.bOverlapsExtent ? TEXT("is empty after clamping to") : TEXT("lies entirely outside"),
            FullMinX, FullMinY, FullMaxX, FullMaxY));
    return true;
  }
  // const: GetHeightData writes the sub-rectangle it actually found components for back through
  // these four bounds, having seeded them with INT_MAX / INT_MIN first. Keeping the request in
  // separate constants is what makes it structurally impossible to echo those seeds as a region.
  const int32 MinX = Region.MinX, MinY = Region.MinY, MaxX = Region.MaxX, MaxY = Region.MaxY;
  const int32 SizeX = MaxX - MinX + 1;
  const int32 SizeY = MaxY - MinY + 1;
  // A ceiling, not a truncation. The path tracer keys lattice nodes by int32, and a partial
  // sweep of a region the caller asked for in full would be a measurement of something else
  // wearing the answer's name - so the request is refused with the number to ask for instead.
  constexpr int64 MaxRegionSamples = 4 * 1024 * 1024;
  if (int64(SizeX) * int64(SizeY) > MaxRegionSamples) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("Region %dx%d is %lld samples, over the %lld-sample ceiling. Ask for a smaller region: a partial sweep would be a different measurement wearing this one's name."),
            SizeX, SizeY, int64(SizeX) * int64(SizeY), MaxRegionSamples));
    return true;
  }

  // Same read path as landscape.get_heights, so the same two-part dirty fix is mandatory:
  // MakeLandscapeEditInterfaceReadOnly removes the known cause (Texture->Modify), the scoped
  // guard restores anything else that dirties and logs that it had to. The contract is
  // PRESERVE, not clear. The guard is declared first so it outlives the interface's Flush().
  TArray<uint16> Heights;
  Heights.SetNumZeroed(SizeX * SizeY);
  int32 GotMinX = MinX, GotMinY = MinY, GotMaxX = MaxX, GotMaxY = MaxY;
  {
    PinWright::PackageDirty::FScopedPackageDirtyRestore DirtyGuard;
    CaptureLandscapeReadDirtyState(DirtyGuard, Landscape, LandscapeInfo);

    FLandscapeEditDataInterface LandscapeEditRead(LandscapeInfo, false);
    MakeLandscapeEditInterfaceReadOnly(LandscapeEditRead);
    LandscapeEditRead.GetHeightData(GotMinX, GotMinY, GotMaxX, GotMaxY, Heights.GetData(), 0);
  }

  // The rule the sample ceiling above already states, applied to the other way a sweep can come
  // back partial: samples with no landscape component behind them are CalcMissingValues
  // interpolation, not terrain, and a shape verdict computed over invented terrain is a different
  // measurement wearing this one's name. Refused with the rectangle to ask for instead.
  const LandscapeHeightStats::FMeasuredHeightRegion Measured =
      LandscapeHeightStats::ResolveMeasuredRegion(MinX, MinY, MaxX, MaxY,
          GotMinX, GotMinY, GotMaxX, GotMaxY);
  if (!Measured.bAnyMeasured) {
    Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NO_HEIGHT_DATA,
        FString::Printf(TEXT("No landscape component covers region [%d,%d]..[%d,%d] on '%s', so all %d samples came back as interpolated fill rather than terrain. Landscape extent is [%d,%d]..[%d,%d]."),
            MinX, MinY, MaxX, MaxY, *Landscape->GetActorLabel(), SizeX * SizeY,
            FullMinX, FullMinY, FullMaxX, FullMaxY));
    return true;
  }
  if (!Measured.bFullyMeasured) {
    Ctx.SendError(ErrorCodes::ERR_LANDSCAPE_NO_HEIGHT_DATA,
        FString::Printf(TEXT("%lld of %d samples in region [%d,%d]..[%d,%d] have no landscape component behind them and came back as interpolated fill. Ask for [%d,%d]..[%d,%d] instead — that is the part of the request that exists."),
            Measured.FabricatedSampleCount, SizeX * SizeY, MinX, MinY, MaxX, MaxY,
            Measured.MinX, Measured.MinY, Measured.MaxX, Measured.MaxY));
    return true;
  }

  LandscapeShape::FMeasurement M;
  LandscapeShape::Measure(Heights, SizeX, SizeY, Params, M);
  TArray<LandscapeShape::FFinding> Findings;
  LandscapeShape::Evaluate(M, Params, SelectedChecks, Findings);

  PinWrightAudit::FVerdict Verdict;
  uint32 FlaggedMask = 0, UnrunnableMask = 0;
  TArray<TSharedPtr<FJsonValue>> FindingRows;
  for (const LandscapeShape::FFinding& F : Findings) {
    const bool bUnrunnable = (F.Status == LandscapeShape::EFindingStatus::Unrunnable);
    if (bUnrunnable) { ++Verdict.UnrunnableCount; UnrunnableMask |= LandscapeShape::CheckBit(F.Check); }
    else {
      FlaggedMask |= LandscapeShape::CheckBit(F.Check);
      if (F.Severity == LandscapeShape::ESeverity::Error) { ++Verdict.ErrorCount; }
      else { ++Verdict.WarningCount; }
    }
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("check"), LandscapeShape::CheckInfo(F.Check).Id);
    Row->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(F.Status));
    Row->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(F.Severity));
    Row->SetStringField(TEXT("code"), F.Code);
    Row->SetStringField(TEXT("message"), F.Message);
    FindingRows.Add(MakeShared<FJsonValueObject>(Row));
  }

  // The verdict is the shared one (Audit/AuditFramework.h): an unrunnable check makes pass
  // false whatever failOn says. No truncation term - the region ceiling above REFUSES an
  // oversized request rather than measuring part of it, so this sweep always covers the region
  // it reports on. Verdict.bTruncated is therefore left false rather than a second rule.
  const bool bPass = Verdict.DerivePass(FailOnMode);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetBoolField(TEXT("pass"), bPass);
  Resp->SetStringField(TEXT("failOn"), FailOn);
  Resp->SetStringField(TEXT("passRule"),
      PinWrightAudit::PassRuleText(/*bIncludeTruncation=*/false,
          TEXT("An unmeasurable region never reads as a pass.")));
  Resp->SetStringField(TEXT("landscapePath"), Landscape->GetPathName());
  Resp->SetStringField(TEXT("landscapeName"), Landscape->GetActorLabel());

  TSharedPtr<FJsonObject> RegionResp = MakeShared<FJsonObject>();
  RegionResp->SetNumberField(TEXT("minX"), MinX);
  RegionResp->SetNumberField(TEXT("minY"), MinY);
  RegionResp->SetNumberField(TEXT("maxX"), MaxX);
  RegionResp->SetNumberField(TEXT("maxY"), MaxY);
  RegionResp->SetNumberField(TEXT("sizeX"), M.SizeX);
  RegionResp->SetNumberField(TEXT("sizeY"), M.SizeY);
  RegionResp->SetNumberField(TEXT("sampleCount"), Heights.Num());
  Resp->SetObjectField(TEXT("region"), RegionResp);

  // Every number the verdict was derived from, whether or not it produced a finding: the
  // trend below the bar is the useful part, and a scalar nobody can audit is a scalar nobody
  // can argue with.
  TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
  if (M.AxisFraction.IsSet()) { Metrics->SetNumberField(TEXT("axisFraction"), M.AxisFraction.GetValue()); }
  else { Metrics->SetField(TEXT("axisFraction"), MakeShared<FJsonValueNull>()); }
  if (M.StepFraction.IsSet()) { Metrics->SetNumberField(TEXT("stepFraction"), M.StepFraction.GetValue()); }
  else { Metrics->SetField(TEXT("stepFraction"), MakeShared<FJsonValueNull>()); }
  Metrics->SetNumberField(TEXT("boundarySegments"), M.Edges);
  Metrics->SetNumberField(TEXT("segmentsSkippedMargin"), M.EdgesSkippedMargin);
  Metrics->SetNumberField(TEXT("paths"), M.Paths);
  Metrics->SetNumberField(TEXT("chords"), M.Chords);
  Metrics->SetNumberField(TEXT("bandHeight"), M.BandHeight);
  Metrics->SetNumberField(TEXT("occupiedBands"), M.OccupiedBands);
  Metrics->SetNumberField(TEXT("minHeight"), M.MinHeight);
  Metrics->SetNumberField(TEXT("maxHeight"), M.MaxHeight);
  Metrics->SetNumberField(TEXT("heightRange"), M.MaxHeight - M.MinHeight);
  Metrics->SetNumberField(TEXT("totalRise"), M.TotalRise);
  Metrics->SetNumberField(TEXT("steppedRise"), M.SteppedRise);
  Metrics->SetNumberField(TEXT("isolatedSteps"), M.IsolatedSteps);
  Metrics->SetNumberField(TEXT("subThresholdSteps"), M.SubThresholdSteps);
  if (Ctx.GetBool(TEXT("includeHistogram"), false)) {
    TArray<TSharedPtr<FJsonValue>> Hist;
    for (int32 Bucket : M.AngleHistogram) { Hist.Add(MakeShared<FJsonValueNumber>(Bucket)); }
    Metrics->SetArrayField(TEXT("angleHistogram"), Hist);
    Metrics->SetStringField(TEXT("angleHistogramBuckets"),
        TEXT("18 buckets of 5 degrees over 0..90, chord angle modulo 90."));
  }
  Resp->SetObjectField(TEXT("metrics"), Metrics);

  TSharedPtr<FJsonObject> Thresholds = MakeShared<FJsonObject>();
  Thresholds->SetNumberField(TEXT("stride"), Params.Stride);
  Thresholds->SetNumberField(TEXT("epsDeg"), Params.EpsDeg);
  Thresholds->SetNumberField(TEXT("marginCells"), Params.MarginCells);
  Thresholds->SetNumberField(TEXT("bandCount"), Params.BandCount);
  Thresholds->SetNumberField(TEXT("bandHeight"), Params.BandHeight);
  Thresholds->SetNumberField(TEXT("minChords"), Params.MinChords);
  Thresholds->SetNumberField(TEXT("minStepUnits"), Params.MinStepUnits);
  Thresholds->SetNumberField(TEXT("maxAxisFraction"), Params.MaxAxisFraction);
  Thresholds->SetNumberField(TEXT("maxStepFraction"), Params.MaxStepFraction);
  Resp->SetObjectField(TEXT("thresholds"), Thresholds);

  // Every check, selected or not, with the outcome it reached. A check that is not listed is
  // how a check silently stops running.
  TArray<TSharedPtr<FJsonValue>> CheckRows;
  for (const LandscapeShape::FCheckInfo& Info : LandscapeShape::AllChecks()) {
    const bool bSelected = LandscapeShape::HasCheck(SelectedChecks, Info.Check);
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("id"), Info.Id);
    Row->SetStringField(TEXT("code"), Info.Code);
    Row->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Info.Severity));
    Row->SetBoolField(TEXT("selected"), bSelected);
    Row->SetBoolField(TEXT("defaultOn"), Info.bDefaultOn);
    Row->SetStringField(TEXT("status"),
        !bSelected ? TEXT("not_selected")
        : LandscapeShape::HasCheck(UnrunnableMask, Info.Check) ? TEXT("unrunnable")
        : LandscapeShape::HasCheck(FlaggedMask, Info.Check) ? TEXT("flagged")
        : TEXT("clean"));
    Row->SetStringField(TEXT("summary"), Info.Summary);
    CheckRows.Add(MakeShared<FJsonValueObject>(Row));
  }
  Resp->SetArrayField(TEXT("checks"), CheckRows);
  Resp->SetArrayField(TEXT("findings"), FindingRows);

  TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
  Summary->SetNumberField(TEXT("findings"), FindingRows.Num());
  Summary->SetNumberField(TEXT("errors"), Verdict.ErrorCount);
  Summary->SetNumberField(TEXT("warnings"), Verdict.WarningCount);
  Summary->SetNumberField(TEXT("unrunnable"), Verdict.UnrunnableCount);
  Resp->SetObjectField(TEXT("summary"), Summary);

  // Stated limits, not oversights. A green here means less than it looks like it means, and
  // the verb says so in its own output rather than only in the wiki.
  TArray<TSharedPtr<FJsonValue>> Limits;
  Limits.Add(MakeShared<FJsonValueString>(FString::Printf(
      TEXT("axisFraction cannot see a staircase whose risers are shorter than the %d-cell stride: it averages out to the diagonal it approximates. The retired Python gate measured an 8x-upsampled mask at ~0.41. stepped_profile is the check that covers that case, which is why both are on by default."),
      Params.Stride)));
  Limits.Add(MakeShared<FJsonValueString>(
      TEXT("A genuinely axis-aligned shape - a rectangular pad, a plinth, paving - scores HIGH here and is correct. This verb measures shape, not intent; a high axisFraction on deliberately rectangular terrain is a true reading, not a defect.")));
  Limits.Add(MakeShared<FJsonValueString>(FString::Printf(
      TEXT("stepFraction ignores isolated risers below minStepUnits (%d height units), because a slope shallower than one height unit per sample quantises into flat runs with single-unit risers. Terrain stamped in tiers finer than that reads low; %d such riser(s) were excluded from the numerator here."),
      Params.MinStepUnits, M.SubThresholdSteps)));
  Limits.Add(MakeShared<FJsonValueString>(
      TEXT("This measures the stored heightfield only. Runtime displacement, tessellation and material-driven offset are invisible to it, and so is anything a landscape edit layer has not yet flushed.")));
  Resp->SetArrayField(TEXT("limits"), Limits);

  Ctx.SendSuccess(Resp);
  return true;
}
