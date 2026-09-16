// Copyright (c) 2026 Alexander Penkin. MIT License.

// UtilityPropertyHandler.cpp - Migrated from PinWright_PropertyHandlers.cpp
// Object property get/set, array/map/set container operations, asset dependency queries

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetDumpSuggestion.h"
#include "Utils/AutoActivateDisclosure.h"
#include "Utils/PropertyChangeNotify.h"
#include "Utils/PropertyUtils.h"
#include "Utils/JsonUtils.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
// A ULandscapeGrassType edit is durable but invisible until the landscapes that built
// grass from it are flushed; the reflected mutators below are the verbs that make such an
// edit, so they are the verbs that owe the flush (B-grass-varieties-edit-does-not-reach-renderer).
#include "Handlers/Environment/GrassTypeConsumers.h"
#include "Utils/DerivedStateReport.h"

#include "Dom/JsonObject.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Misc/PackageName.h"
#include "Compat/EngineVersionCompat.h"
// FOverridableManager (Overridable Serialization experimental feature) arrived in UE 5.4;
// the header doesn't exist on 5.3. Gate the include and all uses behind a feature macro so
// the value-reset path still works on 5.3 (it just skips the override-metadata bookkeeping).
#if __has_include("UObject/OverridableManager.h")
#include "UObject/OverridableManager.h"
#define MCP_HAS_OVERRIDABLE_MANAGER 1
#else
#define MCP_HAS_OVERRIDABLE_MANAGER 0
#endif
#if __has_include("UObject/PropertyVisitor.h")
#include "UObject/PropertyVisitor.h"
#define MCP_HAS_PROPERTY_VISITOR 1
#else
#define MCP_HAS_PROPERTY_VISITOR 0
#endif
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Utils/AssetUtils.h"
#include "Engine/Blueprint.h"

// ---------------------------------------------------------------------------
// Static helpers - common property resolution pattern
// ---------------------------------------------------------------------------

// True when FindObject/StaticLoadObject was handed a level-style subobject path
// (`Package.Asset:SubPath.Leaf`, or any `Outer.Sub` chain) but the trailing
// segment did not resolve, so UE's ResolveName climbed back to the last-resolved
// ancestor (e.g. the World) instead of returning null. The signature is: the
// object it returned is a strict *ancestor* of the requested path — its full
// path name is a prefix of the requested path followed by a path delimiter
// (`:` between package and first subobject, `.` between subobjects). An exact
// leaf hit has ResolvedName == RequestedPath and is NOT an ancestor fallback.
static bool IsAncestorFallback(const UObject* Resolved, const FString& RequestedPath)
{
    if (!Resolved)
    {
        return false;
    }

    const FString ResolvedName = Resolved->GetPathName();
    if (ResolvedName.Len() >= RequestedPath.Len())
    {
        // Same length (exact hit) or longer (cannot be an ancestor) — not a fallback.
        // Also guarantees the RequestedPath[ResolvedName.Len()] delimiter probe below
        // stays in bounds.
        return false;
    }

    // The requested path must extend the resolved path via a real subobject
    // delimiter for the resolved object to be a genuine ancestor of the request
    // rather than merely a name-prefix sibling (e.g. `…/Foo` vs `…/FooBar`).
    if (!RequestedPath.StartsWith(ResolvedName, ESearchCase::IgnoreCase))
    {
        return false;
    }

    const TCHAR NextChar = RequestedPath[ResolvedName.Len()];
    return NextChar == TEXT(':') || NextChar == TEXT('.');
}

// Resolve an object from path or actor name, returning nullptr on failure after sending error
static UObject* ResolveObjectForProperty(FHandlerContext& Ctx, const FString& ObjectPath)
{
    const FString TrimmedPath = ObjectPath.TrimStartAndEnd();
    UObject* RootObject = FindObject<UObject>(nullptr, *TrimmedPath);

    UObject* Resolved = nullptr;

    // If a direct lookup succeeded and it's not a package, use it — but reject the
    // silent ancestor fallback. For a `…:PersistentLevel.<Actor>.<bad-tail>` path
    // whose trailing segment does not exist, UE's ResolveName climbs to the last
    // resolvable outer (the World) and FindObject returns THAT instead of null.
    // Accepting it would hand the caller a different object than it asked for with
    // ok:true (the "silent success / wrong-object" bug). When the direct hit is a
    // strict ancestor of the requested path, drop it and fall through to the other
    // resolution modes; if none resolve, the caller gets OBJECT_NOT_FOUND.
    if (RootObject && !RootObject->IsA<UPackage>() && !IsAncestorFallback(RootObject, TrimmedPath))
    {
        Resolved = RootObject;
    }

    if (!Resolved)
    {
        TArray<FString> CandidatePaths;
        CandidatePaths.Reserve(4);
        if (!TrimmedPath.IsEmpty())
        {
            CandidatePaths.Add(TrimmedPath);

            if (!TrimmedPath.Contains(TEXT(".")))
            {
                FString Leaf = FPackageName::GetLongPackageAssetName(TrimmedPath);
                if (!Leaf.IsEmpty())
                {
                    CandidatePaths.Add(TrimmedPath + TEXT(".") + Leaf);
                }
            }
        }

        for (const FString& Candidate : CandidatePaths)
        {
            const bool bCanQueryAssetLibrary =
                Candidate.StartsWith(TEXT("/")) &&
                (FPackageName::IsValidObjectPath(Candidate) || FPackageName::IsValidLongPackageName(Candidate));

            if (bCanQueryAssetLibrary)
            {
                const FResolvedAsset AssetResolution =
                    ResolveAsset(Candidate, /*bLoadObject=*/true);
                // DoesAssetExist/LoadAsset strip a trailing subobject tail that does not
                // resolve and fall back to the package's primary asset (e.g. the World for
                // a `…:PersistentLevel.<Actor>.<bad-tail>` path) — the same silent ancestor
                // fallback FindObject/StaticLoadObject exhibit. Reject it so a bad tail
                // falls through to OBJECT_NOT_FOUND instead of resolving to the parent asset.
                if (AssetResolution.bExists && AssetResolution.Object)
                {
                    UObject* Loaded = AssetResolution.Object;
                    if (!Loaded->IsA<UPackage>() && !IsAncestorFallback(Loaded, Candidate))
                    {
                        Resolved = Loaded;
                        break;
                    }
                }
            }

            if (Candidate.StartsWith(TEXT("/")))
            {
                // StaticLoadObject runs the same ResolveName climb as FindObject, so a
                // bad subobject tail would resolve to the ancestor (World) here too —
                // reject the ancestor fallback so the bad path falls through to
                // OBJECT_NOT_FOUND instead of silently substituting a parent object.
                if (UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Candidate))
                {
                    if (!Loaded->IsA<UPackage>() && !IsAncestorFallback(Loaded, Candidate))
                    {
                        Resolved = Loaded;
                        break;
                    }
                }
            }
        }

        if (!Resolved)
        {
            if (AActor* FoundActor = Ctx.GetSubsystem()->FindActorByName(TrimmedPath))
            {
                Resolved = FoundActor;
            }
        }
    }

    // Blueprint asset path → resolve to CDO so callers get the configured
    // properties (Actions, DefaultPawnData, etc.) instead of UBlueprint internals.
    if (UBlueprint* BP = Cast<UBlueprint>(Resolved))
    {
        if (BP->GeneratedClass)
        {
            return BP->GeneratedClass->GetDefaultObject();
        }
    }

    return Resolved;
}

// Resolve a property from an object, handling both nested and simple paths.
// Thin FHandlerContext wrapper over the shared ResolvePropertyOnObject dispatch
// primitive (Utils/PropertyInspection.h): it does the dotted-vs-simple branch,
// and this wrapper only maps a null return to the PROPERTY_NOT_FOUND error.
// Returns true on success, false on failure (error already sent).
static bool ResolvePropertyFromObject(FHandlerContext& Ctx, UObject* RootObject,
    const FString& PropertyName, const FString& ObjectPath,
    void*& OutContainer, FProperty*& OutProperty,
    FPropertyNotifyTarget* OutNotifyTarget = nullptr)
{
    FString ResolveError;
    if (OutNotifyTarget)
    {
        OutNotifyTarget->Object = RootObject;
        OutNotifyTarget->RelativePath = PropertyName;
    }
    OutProperty = PropertyName.Contains(TEXT("."))
        ? ResolveNestedPropertyPath(
            RootObject, PropertyName, OutContainer, ResolveError, OutNotifyTarget)
        : ResolvePropertyOnObject(RootObject, PropertyName, OutContainer, ResolveError);
    if (!OutProperty)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            FString::Printf(TEXT("Failed to resolve property '%s' on object %s: %s"),
                *PropertyName, *ObjectPath, *ResolveError));
        return false;
    }
    return true;
}

// The TOP-LEVEL FProperty of NotifiedObject that contains the property named by a
// (possibly dotted, possibly subscripted) path: "BodyInstance.CollisionEnabled" ->
// BodyInstance, "SensesConfig[0].PeripheralVisionAngle" -> SensesConfig, a
// single-segment path -> that property itself. Returns null when the first segment
// does not name a property of NotifiedObject's class, which is the caller's cue to let
// FPropertyChangedEvent default MemberProperty to the leaf.
//
// Resolution deliberately stops at the FIRST segment: the member half of the event has
// to be a property the NOTIFIED object's own class declares, and deeper hops are already
// described by the leaf. The path handed in must therefore be relative to that object -
// FPropertyNotifyTarget::RelativePath, not the caller's original path. Passing the
// original path with the notified object changed is the failure this pairing exists to
// avoid: for "SettingsInterface.LowerBound" notified on the settings object the member is
// LowerBound (leaf == member), not SettingsInterface, which the settings class does not
// declare at all.
static FProperty* ResolveMemberPropertyForPath(UObject* NotifiedObject, const FString& PropertyName)
{
    if (!NotifiedObject)
    {
        return nullptr;
    }

    FString FirstSegment = PropertyName;
    int32 Delimiter = INDEX_NONE;
    if (FirstSegment.FindChar(TEXT('.'), Delimiter))
    {
        FirstSegment.LeftInline(Delimiter);
    }
    if (FirstSegment.FindChar(TEXT('['), Delimiter))
    {
        FirstSegment.LeftInline(Delimiter);
    }
    FirstSegment.TrimStartAndEndInline();
    if (FirstSegment.IsEmpty())
    {
        return nullptr;
    }

    return FindFProperty<FProperty>(NotifiedObject->GetClass(), FName(*FirstSegment));
}

// A named FPropertyChangedEvent is necessary but NOT sufficient for a renderer-backed
// component. UExponentialHeightFogComponent::PostEditChangeProperty, for one, only
// clamps and calls Super - every MarkRenderStateDirty() in that class lives in its
// generated Set* setters - and the editor's own refresh comes from the
// PreEditChange/PostEditChange reregister pair, not from the event. These verbs
// deliberately skip PreEditChange (it flushes rendering commands and reruns
// construction scripts; see Utils/PropertyChangeNotify.h), so the render state is
// pushed directly instead. Measured: a property.set of FogDensity that answered
// applied/markedDirty/pendingSave true left the renderer drawing the previous value
// indefinitely (B-property-set-container-empty-change-event).
// NotifiedObject is the object the change event was dispatched to, which on a path that
// hopped through an FObjectProperty is NOT the object the path started from: a path that
// reaches into a component sub-object must push render state on THAT component, or the
// notification and the render push disagree about what changed.
static void PushRenderStateForComponentTarget(UObject* NotifiedObject)
{
    // IsValid because this runs after a notification: an override is free to mark the
    // component pending kill, and pushing render state onto one is not harmless.
    if (!IsValid(NotifiedObject))
    {
        return;
    }
    if (UActorComponent* Component = Cast<UActorComponent>(NotifiedObject))
    {
        PinWright::MarkComponentRenderStateDirty(Component);
    }
}

// The same shape as PushRenderStateForComponentTarget, for the other asset whose change
// event is necessary but not sufficient: a ULandscapeGrassType.
//
// ULandscapeGrassType::PostEditChangeProperty invalidates the per-component grass-type
// SUMMARY and recomputes StateHash; on UE 5.4+ it stopped flushing the per-proxy grass
// cache that actually holds the built instances, so a GrassDensity or GrassMesh edit
// changed nothing on screen while property.set answered applied/markedDirty true.
// Measured at a fixed pose: meanLuminance 0.4444 -> 0.4439 for the edit, and 0.4439 ->
// 0.4287 once the cache was flushed (B-grass-varieties-edit-does-not-reach-renderer).
//
// Gated on the cast, so every non-grass property.set costs one failed Cast and nothing
// else - no actor iteration, no rebuild. OutReport is optional: the refresh must happen
// on every reflected mutator, but only property.set publishes the measurement, and a
// null report is a caller that does not surface it rather than one that skips it.
//
// AddKnownUnrefreshedGrassConsumers runs unconditionally once the cast succeeds and always
// adds entries, so a non-empty NotRefreshed is an exact "the target was a grass type" flag
// for the caller — including the run that found no editor world and therefore left
// bMeasured false, which has to reach the response as `measured: false` rather than as no
// block at all.
//
// THE REFRESH IS NON-DESTRUCTIVE, AND THAT MATTERS MOST HERE. This seam fires on every
// property.set / property.reset / container.* write to a grass type with no opt-in of any
// kind, so whatever it does is the DEFAULT cost of editing a ULandscapeGrassType at all.
// The first version flushed with bFlushGrassMaps=true and therefore deleted the landscape's
// per-component grass density maps on every such write (shipped d8f1bc32; measured live at
// 136 grass components -> 0, unrecovered). GrassTypeConsumers.h now passes false and the
// report carries the before/after component counts that prove it, which is the assertion
// the original tests could not make.
static void RefreshGrassConsumersForNotifiedTarget(UObject* NotifiedObject,
    PinWright::GrassConsumers::FGrassRefreshReport* OutReport)
{
    // IsValid for the same reason as above: this runs after a notification, and an
    // override is free to have marked the object pending kill.
    if (!IsValid(NotifiedObject))
    {
        return;
    }
    ULandscapeGrassType* GrassType = Cast<ULandscapeGrassType>(NotifiedObject);
    if (!GrassType)
    {
        return;
    }

    PinWright::GrassConsumers::FGrassRefreshReport LocalReport;
    PinWright::GrassConsumers::FGrassRefreshReport& Report = OutReport ? *OutReport : LocalReport;
    PinWright::GrassConsumers::RefreshGrassConsumers(GrassType, Report);
    PinWright::GrassConsumers::AddKnownUnrefreshedGrassConsumers(Report.Consumers);
}

// The notification every generic reflected mutator in this file owes the engine.
//
// This replaced a bare RootObject->PostEditChange(), which builds an EMPTY
// FPropertyChangedEvent (Obj.cpp:549-553: `FPropertyChangedEvent
// EmptyPropertyUpdateStruct(NULL)`) whose Property and MemberProperty are both null.
// GetPropertyName() then returns NAME_None, so every engine override written as
// `if (PropertyName == GET_MEMBER_NAME_CHECKED(Class, Field))` - which is nearly all
// of them - matched NOTHING, for any assignment. The store landed in the UPROPERTY,
// the read-back was correct, the verb reported success, and the derived state the
// override exists to recompute never ran.
//
// Non-chain form only: UInstancedStaticMeshComponent::PostEditChangeChainProperty
// dereferences PropertyChain.GetActiveMemberNode() unguarded
// (InstancedStaticMesh.cpp:5638), so a synthesised chain crashes the editor for any
// property outside its three known branches.
//
// Call BEFORE any markDirty=false restore: concrete overrides call
// Modify()/MarkPackageDirty() internally, so the dirty decision has to be the last
// thing that touches the flag (B-property-set-markdirty-false-still-dirties). A named
// event makes MORE overrides run, so more of them dirty the package.
//
// THE EVENT FOLLOWS THE WRITE, NOT THE PATH'S STARTING POINT. When a dotted path crosses
// an FObjectProperty the resolver hops onto a different UObject and the store lands in
// THAT object's memory, so it is that object's PostEditChangeProperty override that has
// to run. Notifying RootObject reached nothing however well-formed the event was, and no
// signal exposed it: applied, markedDirty and a separate property.get all read the inner
// object's (correct) memory (B-property-set-object-hop-notification-noop, measured on a
// PCG node whose settings sub-object never got the event that broadcasts
// OnSettingsChangedDelegate, PCGSettings.cpp:719/:744). The plain non-chain event the
// inner object needs is exactly what this already emits - the target was the only thing
// wrong - so this is a retarget, not a chain event the plugin cannot safely synthesise.
// A path crossing no object property resolves back to RootObject and the unchanged path,
// so struct hops and single-segment writes behave byte-identically to before.
//
// OutGrassRefresh is an optional measurement channel, not an opt-in to the refresh: the
// grass flush below runs for every reflected mutator in this file. It defaults to null so
// the fourteen container.* call sites are unchanged; property.set passes one because its
// response is the one a caller reads to decide whether the edit landed.
static void NotifyReflectedPropertyChanged(UObject* RootObject, const FString& PropertyName,
    FProperty* Property, EPropertyChangeType::Type ChangeType,
    PinWright::GrassConsumers::FGrassRefreshReport* OutGrassRefresh = nullptr)
{
    if (!RootObject || !Property)
    {
        return;
    }

    // Re-walks the path through the same resolver the write used rather than re-deriving
    // the traversal rules here; the cost is a handful of FindFProperty lookups, and the
    // path cannot have moved because only the leaf was written.
    const FPropertyNotifyTarget NotifyTarget =
        ResolvePropertyNotifyTarget(RootObject, PropertyName);
    UObject* const NotifiedObject = NotifyTarget.Object ? NotifyTarget.Object : RootObject;

    PinWright::NotifyPropertyChanged(NotifiedObject, Property,
        ResolveMemberPropertyForPath(NotifiedObject, NotifyTarget.RelativePath), ChangeType);

    PushRenderStateForComponentTarget(NotifiedObject);
    RefreshGrassConsumersForNotifiedTarget(NotifiedObject, OutGrassRefresh);
}

// Add verification based on object type
static void AddObjectVerification(FHandlerContext& Ctx, TSharedPtr<FJsonObject>& Result, UObject* Obj)
{
    if (AActor* AsActor = Cast<AActor>(Obj))
    {
        AddActorVerification(Result, AsActor);
    }
    else
    {
        AddAssetVerification(Result, Obj);
    }
}

static bool TryParseContainerInt32(const TSharedPtr<FJsonValue>& Value,
    int32& OutValue, FString& OutError)
{
    int64 ParsedValue = 0;
    if (!TryParseStrictJsonInteger(
            Value,
            static_cast<int64>(TNumericLimits<int32>::Min()),
            static_cast<int64>(TNumericLimits<int32>::Max()),
            ParsedValue,
            OutError))
    {
        return false;
    }

    OutValue = static_cast<int32>(ParsedValue);
    return true;
}

// The mark-dirty-only mutator read-back, shared by property.set and property.reset.
//
// markedDirty reports the OBSERVED package dirty state after the mutation, never the
// markDirty request parameter (B-property-set-markdirty-false-still-dirties): a caller
// that passed markDirty=false must not be told markedDirty:false while the package is
// genuinely dirty, and that contradiction is the only thing that makes the markDirty
// guarantee falsifiable from the response alone. Re-deriving the field from bMarkDirty
// at a call site reintroduces the request-echo, so both verbs stamp it from here.
//
// bRequestedMarkDirty is the fallback for a target with no package at all - there is no
// state to observe, so the request is the only answer available. In practice
// UObject::GetOutermost() always resolves (worst case the transient package), so this
// branch is defensive rather than reachable.
//
// Call AFTER the dirty decision (MarkPackageDirty / the clean-package restore), which is
// itself after the change notification: concrete PostEditChange overrides dirty the
// package on their own, so anything read before them measures the wrong moment.
static void StampMarkedDirty(TSharedPtr<FJsonObject>& Result, const UPackage* TargetPackage,
    bool bRequestedMarkDirty)
{
    Result->SetBoolField(TEXT("markedDirty"),
        TargetPackage ? TargetPackage->IsDirty() : bRequestedMarkDirty);
}

namespace
{
    constexpr const TCHAR* ClassCdoDefaultSource = TEXT("class_cdo");
    constexpr const TCHAR* ParentTemplateDefaultSource = TEXT("parent_template");
    constexpr const TCHAR* ParentClassCdoDefaultSource = TEXT("parent_class_cdo");

    struct FResolvedDefaultSource
    {
        UObject* Object = nullptr;
        FString Source = ClassCdoDefaultSource;
        // When the primary default Object is an *archetype* (parent component
        // template or parent-class CDO) that may not declare every property of
        // the target object, FallbackObject is the target's own class default.
        // Per-property resolution falls back to it for properties the archetype
        // does not declare (e.g. a Blueprint-local variable on a CDO root),
        // preserving the original class_cdo behavior for those. The fallback
        // always resolves the target's own class default, so the resulting
        // source is always ClassCdoDefaultSource by construction.
        UObject* FallbackObject = nullptr;
    };

    struct FResolvedDefaultProperty
    {
        FPropertyExportSource ExportSource = FPropertyExportSource::FromRaw(nullptr);
        FProperty* Property = nullptr;
        FString Source = ClassCdoDefaultSource;
        FString Error;
    };

    struct FDefaultPropertyLookupCache
    {
        TMap<FName, FProperty*> PropertiesByName;
    };
}

static UObject* ResolveClassDefaultObject(UObject* RootObject)
{
    if (!RootObject)
    {
        return nullptr;
    }

    UClass* ObjectClass = RootObject->GetClass();
    if (!ObjectClass)
    {
        return nullptr;
    }

    return ObjectClass->GetDefaultObject();
}

static bool ResolveInheritedComponentParentTemplate(UActorComponent* ComponentTemplate, UObject*& OutParentTemplate)
{
    OutParentTemplate = nullptr;
    if (!ComponentTemplate)
    {
        return false;
    }

    UBlueprintGeneratedClass* OwningGeneratedClass =
        Cast<UBlueprintGeneratedClass>(ComponentTemplate->GetOuter());
    if (OwningGeneratedClass)
    {
        if (UInheritableComponentHandler* Handler =
            OwningGeneratedClass->GetInheritableComponentHandler(false))
        {
            const FComponentKey ComponentKey = Handler->FindKey(ComponentTemplate);
            if (ComponentKey.IsValid())
            {
                UActorComponent* ParentTemplate =
                    Handler->FindBestArchetype(ComponentKey, ComponentTemplate->GetFName());
                if (ParentTemplate && ParentTemplate != ComponentTemplate)
                {
                    OutParentTemplate = ParentTemplate;
                    return true;
                }
            }
        }
    }

    UActorComponent* ArchetypeTemplate = Cast<UActorComponent>(ComponentTemplate->GetArchetype());
    if (ArchetypeTemplate && ArchetypeTemplate != ComponentTemplate &&
        ArchetypeTemplate != ResolveClassDefaultObject(ComponentTemplate) &&
        ComponentTemplate->HasAnyFlags(RF_InheritableComponentTemplate))
    {
        OutParentTemplate = ArchetypeTemplate;
        return true;
    }

    return false;
}

// For a Blueprint generated-class CDO, the meaningful per-property default for an
// inherited, locally-unauthored property is the parent-class CDO (the object's
// archetype), not the CDO itself. UObject::GetArchetype() on a CDO returns
// Class->GetArchetypeForCDO(); for a UBlueprintGeneratedClass with no
// OverridenArchetypeForCDO that is the super-class CDO. This mirrors the source
// the editor's per-property reset arrow consults for a CDO. Returns true and the
// archetype CDO only when it is a *distinct* object from RootObject; a CDO that is
// its own archetype (e.g. a native root class) has no parent default to resolve.
static bool ResolveBlueprintCdoArchetype(UObject* RootObject, UObject*& OutArchetype)
{
    OutArchetype = nullptr;
    if (!RootObject || !RootObject->HasAnyFlags(RF_ClassDefaultObject))
    {
        return false;
    }

    if (!Cast<UBlueprintGeneratedClass>(RootObject->GetClass()))
    {
        return false;
    }

    UObject* Archetype = RootObject->GetArchetype();
    OutArchetype = (Archetype && Archetype != RootObject) ? Archetype : nullptr;
    return OutArchetype != nullptr;
}

// When the primary archetype default source does not declare PropertyName, fall
// back to the target's own class default (DefaultSource.FallbackObject) so a
// property authored locally on the target (e.g. a Blueprint-local variable on a
// CDO) still resolves a default, preserving the original class_cdo behavior. The
// fallback always resolves the target's own class default, so on a hit the
// source is stamped ClassCdoDefaultSource. Routes nested paths through
// ResolveNestedPropertyPath and simple names through FindPropertyByName.
// Returns true and populates Result (ExportSource/Property/Source) only on a hit.
static bool TryResolveArchetypeFallbackProperty(
    const FResolvedDefaultSource& DefaultSource,
    UObject* DefaultObject,
    const FString& PropertyName,
    FResolvedDefaultProperty& Result)
{
    if (!DefaultSource.FallbackObject || DefaultSource.FallbackObject == DefaultObject)
    {
        return false;
    }

    if (PropertyName.Contains(TEXT(".")))
    {
        void* FallbackContainer = nullptr;
        FString FallbackError;
        FPropertyNotifyTarget ExportTarget;
        FProperty* FallbackProperty = ResolveNestedPropertyPath(
            DefaultSource.FallbackObject, PropertyName, FallbackContainer, FallbackError,
            &ExportTarget);
        if (FallbackProperty && FallbackContainer)
        {
            Result.ExportSource = FPropertyExportSource::FromResolvedContainer(
                FallbackContainer, ExportTarget.Object);
            Result.Property = FallbackProperty;
            Result.Source = ClassCdoDefaultSource;
            return true;
        }
        return false;
    }

    if (FProperty* FallbackProperty =
        DefaultSource.FallbackObject->GetClass()->FindPropertyByName(*PropertyName))
    {
        Result.ExportSource = FPropertyExportSource::FromObject(DefaultSource.FallbackObject);
        Result.Property = FallbackProperty;
        Result.Source = ClassCdoDefaultSource;
        return true;
    }

    return false;
}

static FResolvedDefaultSource ResolveDefaultSourceObject(UObject* RootObject)
{
    FResolvedDefaultSource Result;
    if (!RootObject)
    {
        return Result;
    }

    if (UActorComponent* ComponentTemplate = Cast<UActorComponent>(RootObject))
    {
        UObject* ParentTemplate = nullptr;
        if (ResolveInheritedComponentParentTemplate(ComponentTemplate, ParentTemplate))
        {
            Result.Object = ParentTemplate;
            Result.Source = ParentTemplateDefaultSource;
            return Result;
        }
    }

    UObject* ParentClassCdo = nullptr;
    if (ResolveBlueprintCdoArchetype(RootObject, ParentClassCdo))
    {
        Result.Object = ParentClassCdo;
        Result.Source = ParentClassCdoDefaultSource;
        // A property authored locally on this Blueprint CDO does not exist on the
        // parent-class CDO; fall back to the self CDO (class_cdo) for those so a
        // Blueprint-local variable still resolves a default (the prior behavior).
        Result.FallbackObject = RootObject;
        return Result;
    }

    Result.Object = ResolveClassDefaultObject(RootObject);
    Result.Source = ClassCdoDefaultSource;
    return Result;
}

static FResolvedDefaultProperty ResolveDefaultPropertyFromSource(
    const FResolvedDefaultSource& DefaultSource,
    const FString& PropertyName)
{
    FResolvedDefaultProperty Result;
    UObject* DefaultObject = DefaultSource.Object;
    Result.Source = DefaultSource.Source;
    if (!DefaultObject)
    {
        Result.Error = TEXT("Default source object is unavailable.");
        return Result;
    }

    if (PropertyName.Contains(TEXT(".")))
    {
        void* DefaultContainer = nullptr;
        FPropertyNotifyTarget ExportTarget;
        Result.Property = ResolveNestedPropertyPath(
            DefaultObject, PropertyName, DefaultContainer, Result.Error, &ExportTarget);
        if ((!Result.Property || !DefaultContainer) &&
            TryResolveArchetypeFallbackProperty(DefaultSource, DefaultObject, PropertyName, Result))
        {
            return Result;
        }
        if (!Result.Property || !DefaultContainer)
        {
            if (Result.Error.IsEmpty())
            {
                Result.Error = TEXT("Failed to resolve nested property on default source object.");
            }
            return Result;
        }
        Result.ExportSource = FPropertyExportSource::FromResolvedContainer(
            DefaultContainer, ExportTarget.Object);
        return Result;
    }

    Result.ExportSource = FPropertyExportSource::FromObject(DefaultObject);
    Result.Property = DefaultObject->GetClass()->FindPropertyByName(*PropertyName);
    if (!Result.Property &&
        TryResolveArchetypeFallbackProperty(DefaultSource, DefaultObject, PropertyName, Result))
    {
        return Result;
    }
    if (!Result.Property)
    {
        Result.Error = FString::Printf(
            TEXT("Property %s not found on default source object %s."),
            *PropertyName, *DefaultObject->GetClass()->GetName());
        return Result;
    }
    return Result;
}

static FResolvedDefaultProperty ResolveDefaultPropertyFromSource(
    const FResolvedDefaultSource& DefaultSource,
    const FProperty* CurrentProperty,
    FDefaultPropertyLookupCache& LookupCache)
{
    FResolvedDefaultProperty Result;
    UObject* DefaultObject = DefaultSource.Object;
    Result.Source = DefaultSource.Source;
    if (!DefaultObject)
    {
        Result.Error = TEXT("Default source object is unavailable.");
        return Result;
    }
    if (!CurrentProperty)
    {
        Result.Error = TEXT("Current property is unavailable.");
        return Result;
    }

    Result.ExportSource = FPropertyExportSource::FromObject(DefaultObject);
    if (FProperty** CachedProperty = LookupCache.PropertiesByName.Find(CurrentProperty->GetFName()))
    {
        Result.Property = *CachedProperty;
    }
    else
    {
        Result.Property = DefaultObject->GetClass()->FindPropertyByName(CurrentProperty->GetFName());
        LookupCache.PropertiesByName.Add(CurrentProperty->GetFName(), Result.Property);
    }
    if (!Result.Property &&
        TryResolveArchetypeFallbackProperty(
            DefaultSource, DefaultObject, CurrentProperty->GetName(), Result))
    {
        return Result;
    }
    if (!Result.Property)
    {
        Result.Error = FString::Printf(
            TEXT("Property %s not found on default source object %s."),
            *CurrentProperty->GetName(), *DefaultObject->GetClass()->GetName());
    }
    return Result;
}

static FResolvedDefaultProperty ResolveDefaultPropertyFromObject(
    UObject* RootObject,
    const FString& PropertyName)
{
    return ResolveDefaultPropertyFromSource(ResolveDefaultSourceObject(RootObject), PropertyName);
}

static void AddPropertyMetadataFields(TSharedPtr<FJsonObject>& ResultPayload, FProperty* Property)
{
    if (!ResultPayload.IsValid() || !Property)
    {
        return;
    }

    const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
    const bool bBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
    const bool bEditableOnInstance = bEditable && !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);
    const bool bTransient = Property->HasAnyPropertyFlags(CPF_Transient);

    ResultPayload->SetStringField(TEXT("cppType"), GetPropertyCppTypeWithParams(Property));
    ResultPayload->SetBoolField(TEXT("editable"), bEditable);
    ResultPayload->SetBoolField(TEXT("blueprintVisible"), bBlueprintVisible);
    ResultPayload->SetBoolField(TEXT("editableOnInstance"), bEditableOnInstance);

    // Structured flags sub-object lets callers filter client-side without re-deriving from top-level bools.
    TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
    Flags->SetBoolField(TEXT("edit"), bEditable);
    Flags->SetBoolField(TEXT("blueprintVisible"), bBlueprintVisible);
    Flags->SetBoolField(TEXT("editOnInstance"), bEditableOnInstance);
    Flags->SetBoolField(TEXT("transient"), bTransient);
    ResultPayload->SetObjectField(TEXT("flags"), Flags);
}

// When property.get resolves a CPF_Deprecated FProperty (the inert *_DEPRECATED
// shadow UHT leaves behind when a field migrates out of its owner — e.g.
// UMaterial's BaseColor/Opacity/Roughness, which moved to UMaterialEditorOnlyData
// in UE 5.5+), the exported value reads benign-empty (Expression: null) with no
// hint it is the dead shadow rather than the live field. This surfaces a
// `deprecated: true` marker UNCONDITIONALLY (independent of includeMetadata, which
// defaults off) so the caller is never silently handed the shadow as if it were
// live. Where the live data is one path segment away — the root exposes a child
// object property whose subobject class declares a same-named live property — it
// also emits a `movedTo` hint pointing at the resolvable nested path.
static void AddDeprecatedResolutionHint(
    TSharedPtr<FJsonObject>& ResultPayload,
    UObject* RootObject,
    const FString& PropertyName,
    FProperty* Property)
{
    if (!ResultPayload.IsValid() || !Property || !RootObject)
    {
        return;
    }
    if (!Property->HasAnyPropertyFlags(CPF_Deprecated))
    {
        return;
    }

    ResultPayload->SetBoolField(TEXT("deprecated"), true);

    // Only derive a movedTo hint for a top-level (non-nested) deprecated read —
    // a caller who already typed a dotted path resolved the field they meant.
    if (PropertyName.Contains(TEXT(".")))
    {
        return;
    }

    // Probe the owner→subobject migration pattern generically: for each child
    // object property on the root, see whether its target class exposes a live
    // (non-deprecated) property of the same name as the deprecated shadow. The
    // canonical instance is UMaterial's `EditorOnlyData` (UMaterialEditorOnlyData);
    // any field following the same migration convention falls out for free without
    // special-casing the owner-property name. The nested leaf is resolved through
    // the shared ResolvePropertyOnObject dispatch primitive — the same traversal
    // property.get itself uses (via ResolvePropertyFromObject) — so the suggested
    // `movedTo` path is guaranteed to be exactly what a follow-up read will resolve.
    for (TFieldIterator<FObjectProperty> It(RootObject->GetClass()); It; ++It)
    {
        FObjectProperty* OwnerProp = *It;
        if (!OwnerProp->GetObjectPropertyValue_InContainer(RootObject))
        {
            continue;
        }

        const FString CandidatePath =
            FString::Printf(TEXT("%s.%s"), *OwnerProp->GetName(), *PropertyName);

        void* LiveContainer = nullptr;
        FString ResolveError;
        FProperty* LiveProp =
            ResolvePropertyOnObject(RootObject, CandidatePath, LiveContainer, ResolveError);
        if (LiveProp && !LiveProp->HasAnyPropertyFlags(CPF_Deprecated))
        {
            ResultPayload->SetStringField(TEXT("movedTo"), CandidatePath);
            return;
        }
    }
}

#if MCP_HAS_PROPERTY_VISITOR
static bool BuildPropertyVisitorPathForProperty(
    UObject* RootObject,
    const FString& PropertyName,
    FPropertyVisitorPath& OutPath,
    FString& OutError)
{
    OutPath = FPropertyVisitorPath();
    OutError.Empty();

    if (!RootObject)
    {
        OutError = TEXT("Root object is null.");
        return false;
    }

    TArray<FString> PathSegments;
    PropertyName.ParseIntoArray(PathSegments, TEXT("."), true);
    if (PathSegments.Num() == 0)
    {
        OutError = TEXT("Property path is empty.");
        return false;
    }

    UStruct* CurrentTypeScope = RootObject->GetClass();
    void* CurrentContainer = RootObject;

    for (int32 Index = 0; Index < PathSegments.Num(); ++Index)
    {
        const FString& Segment = PathSegments[Index];
        FProperty* CurrentProperty = FindFProperty<FProperty>(CurrentTypeScope, FName(*Segment));
        if (!CurrentProperty)
        {
            OutError = FString::Printf(
                TEXT("Property '%s' not found in scope '%s'."), *Segment, *CurrentTypeScope->GetName());
            return false;
        }

        OutPath.Push(FPropertyVisitorInfo(CurrentProperty));

        if (Index == PathSegments.Num() - 1)
        {
            return true;
        }

        if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(CurrentProperty))
        {
            UObject* NextObject = ObjectProp->GetObjectPropertyValue_InContainer(CurrentContainer);
            if (!NextObject)
            {
                OutError = FString::Printf(TEXT("Object property '%s' is null."), *Segment);
                return false;
            }
            CurrentContainer = NextObject;
            CurrentTypeScope = NextObject->GetClass();
        }
        else if (FStructProperty* StructProp = CastField<FStructProperty>(CurrentProperty))
        {
            CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
            CurrentTypeScope = StructProp->Struct;
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Cannot traverse into property '%s' of type '%s'."),
                *Segment, *CurrentProperty->GetClass()->GetName());
            return false;
        }
    }

    OutError = TEXT("Unexpected end of property path resolution.");
    return false;
}

static void BuildEditPropertyChainFromVisitorPath(const FPropertyVisitorPath& PropertyPath, FEditPropertyChain& OutChain)
{
    FProperty* MemberProperty = nullptr;
    FProperty* LeafProperty = nullptr;

    for (const FPropertyVisitorInfo& Info : PropertyPath.GetPath())
    {
        FProperty* Property = const_cast<FProperty*>(Info.Property);
        if (!Property)
        {
            continue;
        }

        if (!MemberProperty)
        {
            MemberProperty = Property;
        }
        LeafProperty = Property;
        OutChain.AddTail(Property);
    }

    if (LeafProperty)
    {
        OutChain.SetActivePropertyNode(LeafProperty);
    }
    if (MemberProperty)
    {
        OutChain.SetActiveMemberPropertyNode(MemberProperty);
    }
}

static EOverriddenPropertyOperation GetExplicitOverrideOperation(
    UObject* RootObject,
    const FPropertyVisitorPath& PropertyPath)
{
    if (RootObject)
    {
        // FOverridableManager was reshaped in UE 5.6: it became a lazily created
        // singleton (TryGet()/Create()) whose accessors take TNotNull<UObject*>.
        // On UE 5.5 the manager is a static Get()-only singleton and the same
        // accessors take a UObject& reference instead.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (FOverridableManager* Manager = FOverridableManager::TryGet())
        {
            return Manager->GetOverriddenPropertyOperation(RootObject, PropertyPath);
        }
#else
        return FOverridableManager::Get().GetOverriddenPropertyOperation(*RootObject, PropertyPath);
#endif
    }
    return EOverriddenPropertyOperation::None;
}

// bOutNotified reports whether this call dispatched a property-change notification of
// its own. It is conditional (the manager may not exist, and the chain may have no
// active node), and the caller uses it to decide whether the reset still owes the
// object a notification — one write must produce exactly one engine change event.
static bool ClearExplicitOverrideState(UObject* RootObject, const FPropertyVisitorPath& PropertyPath,
    bool& bOutNotified)
{
    bOutNotified = false;
    if (RootObject)
    {
        // FOverridableManager was reshaped in UE 5.6: ClearOverriddenProperty()
        // takes TNotNull<UObject*> and the manager is fetched via TryGet(). On UE 5.5
        // the manager is a Get()-only singleton and the accessor takes a UObject&.
        // EPropertyChangeType::ResetToDefault was also added in 5.6 (1 << 10); on 5.5
        // the nearest semantic equivalent for a value reset is Unspecified.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        FOverridableManager* Manager = FOverridableManager::TryGet();
        if (!Manager)
        {
            return false;
        }
        const bool bCleared = Manager->ClearOverriddenProperty(RootObject, PropertyPath);
        const EPropertyChangeType::Type ResetChangeType = EPropertyChangeType::ResetToDefault;
#else
        const bool bCleared = FOverridableManager::Get().ClearOverriddenProperty(*RootObject, PropertyPath);
        const EPropertyChangeType::Type ResetChangeType = EPropertyChangeType::Unspecified;
#endif

        FEditPropertyChain PropertyChain;
        BuildEditPropertyChainFromVisitorPath(PropertyPath, PropertyChain);
        if (PropertyChain.GetActiveNode())
        {
            FPropertyChangedEvent PropertyEvent(
                PropertyChain.GetActiveNode()->GetValue(), ResetChangeType);
            FPropertyChangedChainEvent ChainEvent(PropertyChain, PropertyEvent);
            RootObject->PostEditChangeChainProperty(ChainEvent);
            bOutNotified = true;
        }

#if (UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0) && UE_VERSION_OLDER_THAN(5, 6, 0))
        // UE 5.5 only: PostEditChangeChainProperty routes through
        // FOverridableManager::PostOverrideProperty, whose NotifyPropertyChange re-marks
        // the leaf property as overridden (Operation = Replace) for the Unspecified change
        // type we are forced to use here (EPropertyChangeType::ResetToDefault, which 5.6's
        // override manager treats as a clear, does not exist on 5.5). That re-override undoes
        // the ClearOverriddenProperty above. Re-clear after the notification so the override
        // metadata ends up cleared. ClearOverriddenProperty only removes the node (no further
        // notification), so this cannot re-trigger the re-override. 5.6+ is unaffected: it
        // routes through the ResetToDefault clear branch and never re-marks the property.
        FOverridableManager::Get().ClearOverriddenProperty(*RootObject, PropertyPath);
#endif

        return bCleared;
    }
    return false;
}
#endif // MCP_HAS_PROPERTY_VISITOR

static TSharedPtr<FJsonValue> MakeVectorJsonValue(const FVector& Value)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), Value.X);
    Obj->SetNumberField(TEXT("y"), Value.Y);
    Obj->SetNumberField(TEXT("z"), Value.Z);
    return MakeShared<FJsonValueObject>(Obj);
}

static TSharedPtr<FJsonValue> MakeRotatorJsonValue(const FRotator& Value)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
    Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
    Obj->SetNumberField(TEXT("roll"), Value.Roll);
    return MakeShared<FJsonValueObject>(Obj);
}

static void AddSpecialActorPropertyMetadata(TSharedPtr<FJsonObject>& ResultPayload, const FString& PropertyName)
{
    if (!ResultPayload.IsValid())
    {
        return;
    }

    if (PropertyName.Equals(TEXT("ActorLocation"), ESearchCase::IgnoreCase) ||
        PropertyName.Equals(TEXT("ActorScale"), ESearchCase::IgnoreCase) ||
        PropertyName.Equals(TEXT("ActorScale3D"), ESearchCase::IgnoreCase))
    {
        ResultPayload->SetStringField(TEXT("cppType"), TEXT("FVector"));
    }
    else if (PropertyName.Equals(TEXT("ActorRotation"), ESearchCase::IgnoreCase))
    {
        ResultPayload->SetStringField(TEXT("cppType"), TEXT("FRotator"));
    }
    else if (PropertyName.Equals(TEXT("bHidden"), ESearchCase::IgnoreCase))
    {
        ResultPayload->SetStringField(TEXT("cppType"), TEXT("bool"));
    }
    else
    {
        ResultPayload->SetStringField(TEXT("cppType"), TEXT("unknown"));
    }

    ResultPayload->SetBoolField(TEXT("editable"), true);
    ResultPayload->SetBoolField(TEXT("blueprintVisible"), true);
    ResultPayload->SetBoolField(TEXT("editableOnInstance"), true);
}

static bool ShouldIncludePropertyInList(const FProperty* Property, bool bIncludeAll, bool bIncludeReadOnly)
{
    if (!Property)
    {
        return false;
    }

    const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
    const bool bBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
    const bool bEditableOnInstance = bEditable && !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);

    if (bIncludeAll)
    {
        return true;
    }

    if (!(bEditable || bBlueprintVisible))
    {
        return false;
    }

    if (!bIncludeReadOnly && !bEditableOnInstance)
    {
        return false;
    }

    return true;
}

static TSharedPtr<FJsonValue> ExportPropertyToJsonValueWithOversizedOmission(
    FPropertyExportSource Source,
    FProperty* Property,
    bool bOmitOversized)
{
    if (bOmitOversized)
    {
        if (const FOmissionReason* Reason = IsKnownOversizedProperty(Property))
        {
            return MakeShared<FJsonValueObject>(
                BuildOmissionPlaceholder(Property, Source.GetContainer(), *Reason));
        }
    }

    return ExportPropertyToJsonValue(Source, Property);
}

// ===========================================================================
// property.set
// ===========================================================================
REGISTER_RPC_HANDLER("property.set", "property", "Set a UPROPERTY value on a UObject by name using property reflection. Generic counterpart to typed setters; works on actors, components, asset CDOs, and arbitrary UObjects. A write that leaves a component's bAutoActivate false is disclosed in 'warnings' - it is a level override that outlives the session.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path or actor name"),
        RPC_PARAM_REQ("propertyName", "string", "Property name (supports nested paths with dots)"),
        RPC_PARAM_REQ("value", "any", "Value to set"),
        RPC_PARAM_OPT("markDirty", "boolean", "Mark package dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString ObjectPath = Ctx.GetString(TEXT("objectPath"));
    if (ObjectPath.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_OBJECT"), TEXT("set_object_property requires a non-empty objectPath."));
        return true;
    }

    FString PropertyName = Ctx.GetString(TEXT("propertyName"));
    if (PropertyName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT("set_object_property requires a non-empty propertyName."));
        return true;
    }

    const TSharedPtr<FJsonValue> ValueField = Payload->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("set_object_property payload missing value field."));
        return true;
    }

    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath);
    if (RootObject)
    {
        ObjectPath = RootObject->GetPathName();
    }
    if (!RootObject)
    {
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"),
            FString::Printf(TEXT("Unable to find object at path %s."), *ObjectPath));
        return true;
    }

    // property.set is a mark-dirty-only mutator: it writes the value in-memory and marks
    // the package dirty (default true) but never saves to disk. The response therefore
    // reports applied + markedDirty, NOT saved — persist with editor.save_all / asset.save.
    // Read markDirty once here so every emit path (the actor setters below and the generic
    // reflected path) shares the same decision.
    bool bMarkDirty = true;
    if (Payload->HasField(TEXT("markDirty")))
    {
        Payload->TryGetBoolField(TEXT("markDirty"), bMarkDirty);
    }

    // Dirty baseline for the markDirty=false restore (B-property-set-markdirty-false-still-dirties).
    // Sampled AFTER ResolveObjectForProperty so dirt produced by a cold asset load's PostLoad is
    // part of the baseline and survives the restore — that dirt is not this handler's mutation.
    UPackage* const TargetPackage = RootObject->GetOutermost();
    const bool bPackageWasDirty = TargetPackage && TargetPackage->IsDirty();

    // Shared response finalizer: apply the dirty decision and stamp the property.set
    // response contract (propertyName + applied + markedDirty) so all five emit paths
    // below — the four AActor special-case setters and the generic reflected path —
    // stay in lockstep. Change the contract here, not at each call site. (Actor is a
    // UObject, so one signature covers every path.) markDirty=false restores a
    // previously-clean package instead of merely skipping MarkPackageDirty, because
    // upstream engine code (Modify, PostEditChange overrides) dirties on its own; the
    // restore is skipped when the package was already dirty so it never clears dirt a
    // user or a concurrent editor action created.
    auto FinalizeApplied = [&](TSharedPtr<FJsonObject>& Result, UObject* Target)
    {
        if (bMarkDirty)
        {
            Target->MarkPackageDirty();
        }
        else if (TargetPackage && !bPackageWasDirty)
        {
            TargetPackage->SetDirtyFlag(false);
        }
        Result->SetStringField(TEXT("propertyName"), PropertyName);
        Result->SetBoolField(TEXT("applied"), true);
        // Observed package state, not the request parameter — shared with property.reset so
        // the two verbs cannot drift apart on what markedDirty means.
        StampMarkedDirty(Result, TargetPackage, bMarkDirty);
    };

    // Special handling for common AActor properties that require setters.
    // These use the AActor setter API and mark the package dirty, but (unlike the
    // generic reflected path below) do not run Modify()/PostEditChange(), so an
    // actor-property set here is not recorded as an individual undo transaction.
    if (AActor* Actor = Cast<AActor>(RootObject))
    {
        if (PropertyName.Equals(TEXT("ActorLocation"), ESearchCase::IgnoreCase))
        {
            FVector NewLoc = FVector::ZeroVector;
            if (ValueField->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>& Obj = ValueField->AsObject();
                double X = 0, Y = 0, Z = 0;
                Obj->TryGetNumberField(TEXT("x"), X);
                Obj->TryGetNumberField(TEXT("y"), Y);
                Obj->TryGetNumberField(TEXT("z"), Z);
                NewLoc = FVector(X, Y, Z);
            }
            else if (ValueField->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
                if (Arr.Num() >= 3)
                    NewLoc = FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
            }

            Actor->SetActorLocation(NewLoc);

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            FinalizeApplied(ResultPayload, Actor);
            AddActorVerification(ResultPayload, Actor);

            TSharedPtr<FJsonObject> ValObj = MakeShared<FJsonObject>();
            ValObj->SetNumberField(TEXT("x"), NewLoc.X);
            ValObj->SetNumberField(TEXT("y"), NewLoc.Y);
            ValObj->SetNumberField(TEXT("z"), NewLoc.Z);
            ResultPayload->SetField(TEXT("value"), MakeShared<FJsonValueObject>(ValObj));

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
        else if (PropertyName.Equals(TEXT("ActorRotation"), ESearchCase::IgnoreCase))
        {
            FRotator NewRot = FRotator::ZeroRotator;
            if (ValueField->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>& Obj = ValueField->AsObject();
                double P = 0, Y = 0, R = 0;
                Obj->TryGetNumberField(TEXT("pitch"), P);
                Obj->TryGetNumberField(TEXT("yaw"), Y);
                Obj->TryGetNumberField(TEXT("roll"), R);
                NewRot = FRotator(P, Y, R);
            }
            else if (ValueField->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
                if (Arr.Num() >= 3)
                    NewRot = FRotator(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
            }

            Actor->SetActorRotation(NewRot);

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            FinalizeApplied(ResultPayload, Actor);
            AddActorVerification(ResultPayload, Actor);

            TSharedPtr<FJsonObject> ValObj = MakeShared<FJsonObject>();
            ValObj->SetNumberField(TEXT("pitch"), NewRot.Pitch);
            ValObj->SetNumberField(TEXT("yaw"), NewRot.Yaw);
            ValObj->SetNumberField(TEXT("roll"), NewRot.Roll);
            ResultPayload->SetField(TEXT("value"), MakeShared<FJsonValueObject>(ValObj));

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
        else if (PropertyName.Equals(TEXT("ActorScale"), ESearchCase::IgnoreCase) ||
                 PropertyName.Equals(TEXT("ActorScale3D"), ESearchCase::IgnoreCase))
        {
            FVector NewScale = FVector::OneVector;
            if (ValueField->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>& Obj = ValueField->AsObject();
                double X = 1, Y = 1, Z = 1;
                Obj->TryGetNumberField(TEXT("x"), X);
                Obj->TryGetNumberField(TEXT("y"), Y);
                Obj->TryGetNumberField(TEXT("z"), Z);
                NewScale = FVector(X, Y, Z);
            }
            else if (ValueField->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
                if (Arr.Num() >= 3)
                    NewScale = FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
            }

            Actor->SetActorScale3D(NewScale);

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            FinalizeApplied(ResultPayload, Actor);
            AddActorVerification(ResultPayload, Actor);

            TSharedPtr<FJsonObject> ValObj = MakeShared<FJsonObject>();
            ValObj->SetNumberField(TEXT("x"), NewScale.X);
            ValObj->SetNumberField(TEXT("y"), NewScale.Y);
            ValObj->SetNumberField(TEXT("z"), NewScale.Z);
            ResultPayload->SetField(TEXT("value"), MakeShared<FJsonValueObject>(ValObj));

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
        else if (PropertyName.Equals(TEXT("bHidden"), ESearchCase::IgnoreCase))
        {
            bool bHidden = false;
            if (ValueField->Type == EJson::Boolean)
                bHidden = ValueField->AsBool();
            else if (ValueField->Type == EJson::Number)
                bHidden = ValueField->AsNumber() != 0;

            Actor->SetActorHiddenInGame(bHidden);

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            FinalizeApplied(ResultPayload, Actor);
            ResultPayload->SetBoolField(TEXT("value"), bHidden);
            AddActorVerification(ResultPayload, Actor);

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
    }

    // Generic property path resolution
    void* TargetContainer = nullptr;
    FProperty* Property = nullptr;
    FPropertyNotifyTarget ExportTarget;
    if (!ResolvePropertyFromObject(
            Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property, &ExportTarget))
        return true;

    FString ConversionError;
    void* StagedValue = nullptr;
    const bool bStageScalar = IsJsonScalarProperty(Property);
    FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
    const bool bStageArray = ArrayProperty != nullptr;
    if (bStageScalar)
    {
        if (!TryStageJsonValueForProperty(Property, ValueField, StagedValue, ConversionError))
        {
            Ctx.SendError(TEXT("PROPERTY_CONVERSION_FAILED"), ConversionError);
            return true;
        }
    }
    else if (bStageArray)
    {
        StagedValue = Property->AllocateAndInitializeValue();
        bool bStaged = StagedValue != nullptr;
        if (!bStaged)
        {
            ConversionError = FString::Printf(
                TEXT("Failed to allocate scratch storage for property '%s'"),
                *Property->GetName());
        }
        else
        {
            if (CastField<FTextProperty>(ArrayProperty->Inner))
            {
                void* LiveArrayValue = Property->ContainerPtrToValuePtr<void>(TargetContainer);
                if (!LiveArrayValue)
                {
                    ConversionError = TEXT("Failed to resolve live array value");
                    bStaged = false;
                }
                else
                {
                    // FText replacement needs the current identities to preserve
                    // namespace/key metadata; other arrays can stage from empty.
                    Property->CopySingleValue(StagedValue, LiveArrayValue);
                }
            }
            if (bStaged)
            {
                bStaged = ApplyJsonValueToArrayDirect(
                    ArrayProperty, StagedValue, ValueField, ConversionError);
            }
        }
        if (!bStaged)
        {
            if (StagedValue)
            {
                Property->DestroyValue(StagedValue);
                FMemory::Free(StagedValue);
            }
            Ctx.SendError(TEXT("PROPERTY_CONVERSION_FAILED"), ConversionError);
            return true;
        }
    }

    // Modify(bAlwaysMarkDirty) — with no open transaction SaveToTransactionBuffer returns false
    // and Modify() would fall back to MarkPackageDirty(); passing bMarkDirty suppresses that at
    // the source instead of undoing it. Scalar and array conversion was staged above, so malformed
    // input cannot dirty or notify the object before this point.
    RootObject->Modify(/*bAlwaysMarkDirty=*/bMarkDirty);

    if (bStageScalar || bStageArray)
    {
        Property->CopySingleValue(
            Property->ContainerPtrToValuePtr<void>(TargetContainer), StagedValue);
        Property->DestroyValue(StagedValue);
        FMemory::Free(StagedValue);
    }
    else if (!ApplyJsonValueToProperty(TargetContainer, Property, ValueField, ConversionError))
    {
        Ctx.SendError(TEXT("PROPERTY_CONVERSION_FAILED"), ConversionError);
        return true;
    }

    // The notification must run BEFORE FinalizeApplied: concrete overrides (UMaterial,
    // UBlueprint, Niagara, …) call Modify()/MarkPackageDirty() internally, so the
    // markDirty=false restore has to be the last thing that touches the dirty flag.
    // Property is the resolved leaf; the dotted path's top-level member is named too
    // (NotifyReflectedPropertyChanged) so member-matched branches also fire.
    PinWright::GrassConsumers::FGrassRefreshReport GrassRefresh;
    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ValueSet,
        &GrassRefresh);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    FinalizeApplied(ResultPayload, RootObject);
    AddObjectVerification(Ctx, ResultPayload, RootObject);

    // Emitted only for a ULandscapeGrassType write, which is the one target class where
    // `applied: true` was previously compatible with nothing changing on screen. The block
    // reports what the proxies' grass cache looked like before and after, so a caller can
    // tell an edit that reached three landscapes from one that reached none - and reads
    // `measured: false` rather than a zero when there was no editor world to look at.
    // NotRefreshed is populated by the refresh itself, so it doubles as the "this was a
    // grass type" flag without a second cast here.
    //
    // grassMaps rides the same gate. It is the block that separates an invalidation from a
    // destruction - consumerRefresh reads identically for both, which is how a version that
    // deleted this landscape's grass density maps on every property.set reported
    // `consumersRefreshed: 1` and looked correct.
    if (GrassRefresh.Consumers.bMeasured || GrassRefresh.Consumers.NotRefreshed.Num() > 0)
    {
        PinWright::DerivedState::AddConsumerRefreshReport(ResultPayload, GrassRefresh.Consumers);
        PinWright::GrassConsumers::AddGrassMapIntegrityReport(ResultPayload, GrassRefresh.GrassMaps);
    }

    if (TSharedPtr<FJsonValue> CurrentValue = ExportPropertyToJsonValue(
            FPropertyExportSource::FromResolvedContainer(TargetContainer, ExportTarget.Object),
            Property))
    {
        ResultPayload->SetField(TEXT("value"), CurrentValue);
    }

    // `value: false` states what the field now holds; it does not state that the component
    // will never start again on any later load of the level, which is what that particular
    // false means. Utils/AutoActivateDisclosure.h owns the text, shared with the two
    // actor.*component* verbs. Emitted only when there is something to say, so every other
    // property.set response is unchanged. Gated on the un-dotted single-segment path: a
    // deeper hop resolves into some other object's container, which is not this RootObject
    // and would make the read below describe the wrong thing.
    if (Property->GetName().Equals(TEXT("bAutoActivate"), ESearchCase::IgnoreCase)
        && !PropertyName.Contains(TEXT(".")))
    {
        const FString Disclosure = PinWright::MakeAutoActivateDisabledDisclosure(RootObject);
        if (!Disclosure.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> WarningsArray;
            WarningsArray.Add(MakeShared<FJsonValueString>(Disclosure));
            ResultPayload->SetArrayField(TEXT("warnings"), WarningsArray);
        }
    }

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// property.reset
// ===========================================================================
REGISTER_RPC_HANDLER("property.reset", "property", "Reset a UPROPERTY to its class default and clear explicit override metadata.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path or actor name"),
        RPC_PARAM_REQ("propertyName", "string", "Property name (supports nested paths with dots)"),
        RPC_PARAM_OPT("markDirty", "boolean", "Mark package dirty (default true)")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString ObjectPath = Ctx.GetString(TEXT("objectPath"));
    if (ObjectPath.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_OBJECT"), TEXT("property.reset requires a non-empty objectPath."));
        return true;
    }

    FString PropertyName = Ctx.GetString(TEXT("propertyName"));
    if (PropertyName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT("property.reset requires a non-empty propertyName."));
        return true;
    }

    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath);
    if (RootObject)
    {
        ObjectPath = RootObject->GetPathName();
    }
    if (!RootObject)
    {
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"),
            FString::Printf(TEXT("Unable to find object at path %s."), *ObjectPath));
        return true;
    }

    // Read markDirty and sample the dirty baseline before any Modify() call
    // (B-property-set-markdirty-false-still-dirties). Sampled AFTER ResolveObjectForProperty so
    // dirt produced by a cold asset load's PostLoad is part of the baseline and survives the
    // restore — that dirt is not this handler's mutation.
    bool bMarkDirty = true;
    if (Payload->HasField(TEXT("markDirty")))
    {
        Payload->TryGetBoolField(TEXT("markDirty"), bMarkDirty);
    }
    UPackage* const TargetPackage = RootObject->GetOutermost();
    const bool bPackageWasDirty = TargetPackage && TargetPackage->IsDirty();

    void* TargetContainer = nullptr;
    FProperty* Property = nullptr;
    FPropertyNotifyTarget ExportTarget;
    if (!ResolvePropertyFromObject(
            Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property, &ExportTarget))
    {
        return true;
    }

    const FResolvedDefaultProperty DefaultPropertyResult =
        ResolveDefaultPropertyFromObject(RootObject, PropertyName);
    if (!DefaultPropertyResult.Property ||
        !DefaultPropertyResult.ExportSource.GetContainer() ||
        !Property->SameType(DefaultPropertyResult.Property))
    {
        Ctx.SendError(TEXT("DEFAULT_PROPERTY_NOT_FOUND"),
            DefaultPropertyResult.Error.IsEmpty()
                ? FString::Printf(TEXT("Unable to resolve matching class default for %s."), *PropertyName)
                : DefaultPropertyResult.Error);
        return true;
    }

    const TSharedPtr<FJsonValue> OldValue = ExportPropertyToJsonValue(
        FPropertyExportSource::FromResolvedContainer(TargetContainer, ExportTarget.Object),
        Property);
    const TSharedPtr<FJsonValue> DefaultValue =
        ExportPropertyToJsonValue(DefaultPropertyResult.ExportSource, DefaultPropertyResult.Property);
    if (!OldValue.IsValid() || !DefaultValue.IsValid())
    {
        Ctx.SendError(TEXT("PROPERTY_EXPORT_FAILED"),
            FString::Printf(TEXT("Unable to export old/default value for %s."), *PropertyName));
        return true;
    }

    // Set by the override-clear path below when it already dispatched a named chain
    // event for this reset, so the notification further down is skipped rather than
    // firing every override a second time.
    bool bAlreadyNotified = false;

#if MCP_HAS_PROPERTY_VISITOR
    // UE 5.5+: FOverridableManager uses FPropertyVisitorPath.
    FPropertyVisitorPath PropertyPath;
    FString PropertyPathError;
    if (!BuildPropertyVisitorPathForProperty(RootObject, PropertyName, PropertyPath, PropertyPathError))
    {
        Ctx.SendError(TEXT("PROPERTY_PATH_FAILED"), PropertyPathError);
        return true;
    }

    const EOverriddenPropertyOperation PreviousOverrideOperation =
        GetExplicitOverrideOperation(RootObject, PropertyPath);
    const bool bWasOverridden =
        PreviousOverrideOperation != EOverriddenPropertyOperation::None ||
        !Property->Identical(
            Property->ContainerPtrToValuePtr<void>(TargetContainer),
            DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
                DefaultPropertyResult.ExportSource.GetContainer()),
            PPF_None);

    // Without an open transaction Modify() falls back to MarkPackageDirty(); pass bMarkDirty so
    // markDirty=false never dirties here. Mirrored in the 5.4 and 5.3 branches below.
    RootObject->Modify(/*bAlwaysMarkDirty=*/bMarkDirty);

    void* TargetValuePtr = Property->ContainerPtrToValuePtr<void>(TargetContainer);
    void* DefaultValuePtr =
        DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
            DefaultPropertyResult.ExportSource.GetContainer());
    Property->CopyCompleteValue(TargetValuePtr, DefaultValuePtr);
    ClearExplicitOverrideState(RootObject, PropertyPath, bAlreadyNotified);
    const EOverriddenPropertyOperation FinalOverrideOperation =
        GetExplicitOverrideOperation(RootObject, PropertyPath);
    if (FinalOverrideOperation != EOverriddenPropertyOperation::None)
    {
        Ctx.SendError(TEXT("OVERRIDE_CLEAR_FAILED"),
            FString::Printf(TEXT("Property %s still has explicit override metadata after reset."), *PropertyName));
        return true;
    }
#elif MCP_HAS_OVERRIDABLE_MANAGER
    // UE 5.4: FOverridableManager uses FPropertyChangedEvent + FEditPropertyChain.
    // 5.4 has no FPropertyVisitorPath, no TryGet()/Create() (Get() is a Meyers
    // singleton and always available), and no EPropertyChangeType::ResetToDefault
    // (that value was added in 5.5). ValueSet is the closest 5.4 change type for
    // the value-copy performed by a reset-to-default operation.
    FEditPropertyChain PropertyChainForNotify;
    PropertyChainForNotify.AddTail(Property);
    PropertyChainForNotify.SetActivePropertyNode(Property);
    PropertyChainForNotify.SetActiveMemberPropertyNode(Property);
    FPropertyChangedEvent PropertyEventForOverride(Property, EPropertyChangeType::ValueSet);

    FOverridableManager& OverrideManager = FOverridableManager::Get();
    const EOverriddenPropertyOperation PreviousOverrideOperation =
        OverrideManager.GetOverriddenPropertyOperation(*RootObject, PropertyEventForOverride, PropertyChainForNotify);
    const bool bWasOverridden =
        PreviousOverrideOperation != EOverriddenPropertyOperation::None ||
        !Property->Identical(
            Property->ContainerPtrToValuePtr<void>(TargetContainer),
            DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
                DefaultPropertyResult.ExportSource.GetContainer()),
            PPF_None);

    // See the 5.5+ branch: bMarkDirty keeps Modify()'s no-transaction fallback from dirtying.
    RootObject->Modify(/*bAlwaysMarkDirty=*/bMarkDirty);

    void* TargetValuePtr = Property->ContainerPtrToValuePtr<void>(TargetContainer);
    void* DefaultValuePtr =
        DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
            DefaultPropertyResult.ExportSource.GetContainer());
    Property->CopyCompleteValue(TargetValuePtr, DefaultValuePtr);

    OverrideManager.ClearOverriddenProperty(*RootObject, PropertyEventForOverride, PropertyChainForNotify);
    const EOverriddenPropertyOperation FinalOverrideOperation =
        OverrideManager.GetOverriddenPropertyOperation(*RootObject, PropertyEventForOverride, PropertyChainForNotify);
    if (FinalOverrideOperation != EOverriddenPropertyOperation::None)
    {
        Ctx.SendError(TEXT("OVERRIDE_CLEAR_FAILED"),
            FString::Printf(TEXT("Property %s still has explicit override metadata after reset."), *PropertyName));
        return true;
    }
#else
    // UE 5.3: Overridable Serialization (FOverridableManager) does not exist yet. Reset is a
    // plain value-copy from the class default; there is no per-property override metadata to
    // clear. bWasOverridden reduces to "current value differs from default".
    const bool bWasOverridden = !Property->Identical(
        Property->ContainerPtrToValuePtr<void>(TargetContainer),
        DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
            DefaultPropertyResult.ExportSource.GetContainer()),
        PPF_None);

    // See the 5.5+ branch: bMarkDirty keeps Modify()'s no-transaction fallback from dirtying.
    RootObject->Modify(/*bAlwaysMarkDirty=*/bMarkDirty);

    void* TargetValuePtr = Property->ContainerPtrToValuePtr<void>(TargetContainer);
    void* DefaultValuePtr =
        DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
            DefaultPropertyResult.ExportSource.GetContainer());
    Property->CopyCompleteValue(TargetValuePtr, DefaultValuePtr);
#endif // MCP_HAS_PROPERTY_VISITOR / MCP_HAS_OVERRIDABLE_MANAGER

    // The notification runs first: concrete overrides (UMaterial, UBlueprint, Niagara, …) call
    // Modify()/MarkPackageDirty() internally, so the dirty decision has to be the last thing that
    // touches the flag. The restore only fires on a package that was clean before this call, so it
    // never clears dirt a user or a concurrent editor action created.
    //
    // ResetToDefault is the change type a reset means; it arrived in 5.6 (1 << 10,
    // UnrealType.h:6969). ValueSet is the closest earlier equivalent for the value copy
    // performed above — never Unspecified, which no named branch matches.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    const EPropertyChangeType::Type ResetNotifyChangeType = EPropertyChangeType::ResetToDefault;
#else
    const EPropertyChangeType::Type ResetNotifyChangeType = EPropertyChangeType::ValueSet;
#endif
    if (bAlreadyNotified)
    {
        // The override-clear path already dispatched a named chain event for this reset.
        // Only the render-state push it does not do is still owed.
        PushRenderStateForComponentTarget(RootObject);
    }
    else
    {
        NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, ResetNotifyChangeType);
    }

    if (bMarkDirty)
    {
        RootObject->MarkPackageDirty();
    }
    else if (TargetPackage && !bPackageWasDirty)
    {
        TargetPackage->SetDirtyFlag(false);
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetField(TEXT("oldValue"), OldValue);
    ResultPayload->SetField(TEXT("defaultValue"), DefaultValue);
    ResultPayload->SetStringField(TEXT("defaultSource"), DefaultPropertyResult.Source);
    ResultPayload->SetBoolField(TEXT("wasOverridden"), bWasOverridden);
    ResultPayload->SetBoolField(TEXT("isOverridden"), false);
    // Same mark-dirty-only mutator read-back property.set reports, from the same helper
    // (E-property-reset-no-markeddirty). Without markedDirty the caller's only route to the
    // dirty outcome is editor.list_dirty_packages — a second, process-wide, race-prone call
    // in a shared editor — so the markDirty guarantee could not be checked from the response.
    ResultPayload->SetBoolField(TEXT("applied"), true);
    StampMarkedDirty(ResultPayload, TargetPackage, bMarkDirty);
    AddObjectVerification(Ctx, ResultPayload, RootObject);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// property.get
// ===========================================================================
REGISTER_RPC_HANDLER("property.get", "property", "Read a UPROPERTY value from a UObject by name and return it as JSON. Read-side counterpart to property.set.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path or actor name"),
        RPC_PARAM_REQ("propertyName", "string", "Property name (supports nested paths with dots)"),
        RPC_PARAM_OPT("includeDefault", "boolean", "Include class default value in response (default false)"),
        RPC_PARAM_OPT("includeOverrideState", "boolean", "Include isOverridden/hasDefaultValue in response (default false)"),
        RPC_PARAM_OPT("includeMetadata", "boolean", "Include property metadata (type/editability) in response (default false)"),
        RPC_PARAM_OPT("omitOversized", "boolean", "Emit placeholders for known oversized properties instead of full values (default false)")
    ))
{
    FString ObjectPath = Ctx.GetString(TEXT("objectPath"));
    if (ObjectPath.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_OBJECT"), TEXT("get_object_property requires a non-empty objectPath."));
        return true;
    }

    FString PropertyName = Ctx.GetString(TEXT("propertyName"));
    if (PropertyName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT("get_object_property requires a non-empty propertyName."));
        return true;
    }

    const bool bIncludeDefault = Ctx.GetBool(TEXT("includeDefault"), false);
    const bool bIncludeOverrideState = Ctx.GetBool(TEXT("includeOverrideState"), false);
    const bool bIncludeMetadata = Ctx.GetBool(TEXT("includeMetadata"), false);
    const bool bOmitOversized = Ctx.GetBool(TEXT("omitOversized"), false);

    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath);
    if (RootObject)
    {
        ObjectPath = RootObject->GetPathName();
    }
    if (!RootObject)
    {
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"),
            FString::Printf(TEXT("Unable to find object at path %s."), *ObjectPath));
        return true;
    }

    // Special handling for common AActor properties
    if (AActor* Actor = Cast<AActor>(RootObject))
    {
        if (PropertyName.Equals(TEXT("ActorLocation"), ESearchCase::IgnoreCase))
        {
            FVector Loc = Actor->GetActorLocation();
            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            AddActorVerification(ResultPayload, Actor);
            ResultPayload->SetField(TEXT("value"), MakeVectorJsonValue(Loc));
            if (bIncludeMetadata)
            {
                AddSpecialActorPropertyMetadata(ResultPayload, PropertyName);
            }

            bool bHasDefaultValue = false;
            bool bIsOverridden = false;
            if (AActor* DefaultActor = Cast<AActor>(ResolveClassDefaultObject(Actor)))
            {
                bHasDefaultValue = true;
                const FVector DefaultValue = DefaultActor->GetActorLocation();
                if (bIncludeDefault)
                {
                    ResultPayload->SetField(TEXT("defaultValue"), MakeVectorJsonValue(DefaultValue));
                }
                if (bIncludeOverrideState)
                {
                    bIsOverridden = !Loc.Equals(DefaultValue, KINDA_SMALL_NUMBER);
                }
            }
            else if (bIncludeDefault)
            {
                ResultPayload->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
            }
            if (bIncludeOverrideState)
            {
                ResultPayload->SetBoolField(TEXT("hasDefaultValue"), bHasDefaultValue);
                if (bHasDefaultValue)
                {
                    ResultPayload->SetBoolField(TEXT("isOverridden"), bIsOverridden);
                }
            }

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
        else if (PropertyName.Equals(TEXT("ActorRotation"), ESearchCase::IgnoreCase))
        {
            FRotator Rot = Actor->GetActorRotation();
            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            AddActorVerification(ResultPayload, Actor);
            ResultPayload->SetField(TEXT("value"), MakeRotatorJsonValue(Rot));
            if (bIncludeMetadata)
            {
                AddSpecialActorPropertyMetadata(ResultPayload, PropertyName);
            }

            bool bHasDefaultValue = false;
            bool bIsOverridden = false;
            if (AActor* DefaultActor = Cast<AActor>(ResolveClassDefaultObject(Actor)))
            {
                bHasDefaultValue = true;
                const FRotator DefaultValue = DefaultActor->GetActorRotation();
                if (bIncludeDefault)
                {
                    ResultPayload->SetField(TEXT("defaultValue"), MakeRotatorJsonValue(DefaultValue));
                }
                if (bIncludeOverrideState)
                {
                    bIsOverridden = !Rot.Equals(DefaultValue, KINDA_SMALL_NUMBER);
                }
            }
            else if (bIncludeDefault)
            {
                ResultPayload->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
            }
            if (bIncludeOverrideState)
            {
                ResultPayload->SetBoolField(TEXT("hasDefaultValue"), bHasDefaultValue);
                if (bHasDefaultValue)
                {
                    ResultPayload->SetBoolField(TEXT("isOverridden"), bIsOverridden);
                }
            }

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
        else if (PropertyName.Equals(TEXT("ActorScale"), ESearchCase::IgnoreCase) ||
                 PropertyName.Equals(TEXT("ActorScale3D"), ESearchCase::IgnoreCase))
        {
            FVector Scale = Actor->GetActorScale3D();
            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            AddActorVerification(ResultPayload, Actor);
            ResultPayload->SetField(TEXT("value"), MakeVectorJsonValue(Scale));
            if (bIncludeMetadata)
            {
                AddSpecialActorPropertyMetadata(ResultPayload, PropertyName);
            }

            bool bHasDefaultValue = false;
            bool bIsOverridden = false;
            if (AActor* DefaultActor = Cast<AActor>(ResolveClassDefaultObject(Actor)))
            {
                bHasDefaultValue = true;
                const FVector DefaultValue = DefaultActor->GetActorScale3D();
                if (bIncludeDefault)
                {
                    ResultPayload->SetField(TEXT("defaultValue"), MakeVectorJsonValue(DefaultValue));
                }
                if (bIncludeOverrideState)
                {
                    bIsOverridden = !Scale.Equals(DefaultValue, KINDA_SMALL_NUMBER);
                }
            }
            else if (bIncludeDefault)
            {
                ResultPayload->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
            }
            if (bIncludeOverrideState)
            {
                ResultPayload->SetBoolField(TEXT("hasDefaultValue"), bHasDefaultValue);
                if (bHasDefaultValue)
                {
                    ResultPayload->SetBoolField(TEXT("isOverridden"), bIsOverridden);
                }
            }

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
        else if (PropertyName.Equals(TEXT("bHidden"), ESearchCase::IgnoreCase))
        {
            const bool bHidden = Actor->IsHidden();
            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            ResultPayload->SetBoolField(TEXT("value"), bHidden);
            AddActorVerification(ResultPayload, Actor);
            if (bIncludeMetadata)
            {
                AddSpecialActorPropertyMetadata(ResultPayload, PropertyName);
            }

            bool bHasDefaultValue = false;
            bool bIsOverridden = false;
            if (AActor* DefaultActor = Cast<AActor>(ResolveClassDefaultObject(Actor)))
            {
                bHasDefaultValue = true;
                const bool bDefaultHidden = DefaultActor->IsHidden();
                if (bIncludeDefault)
                {
                    ResultPayload->SetBoolField(TEXT("defaultValue"), bDefaultHidden);
                }
                if (bIncludeOverrideState)
                {
                    bIsOverridden = (bHidden != bDefaultHidden);
                }
            }
            else if (bIncludeDefault)
            {
                ResultPayload->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
            }
            if (bIncludeOverrideState)
            {
                ResultPayload->SetBoolField(TEXT("hasDefaultValue"), bHasDefaultValue);
                if (bHasDefaultValue)
                {
                    ResultPayload->SetBoolField(TEXT("isOverridden"), bIsOverridden);
                }
            }

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
    }

    // Generic property path resolution
    void* TargetContainer = nullptr;
    FProperty* Property = nullptr;
    FPropertyNotifyTarget ExportTarget;
    if (!ResolvePropertyFromObject(
            Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property, &ExportTarget))
        return true;

    const TSharedPtr<FJsonValue> CurrentValue =
        ExportPropertyToJsonValueWithOversizedOmission(
            FPropertyExportSource::FromResolvedContainer(TargetContainer, ExportTarget.Object),
            Property,
            bOmitOversized);
    if (!CurrentValue.IsValid())
    {
        Ctx.SendError(TEXT("PROPERTY_EXPORT_FAILED"),
            FString::Printf(TEXT("Unable to export property %s."), *PropertyName));
        return true;
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetField(TEXT("value"), CurrentValue);
    AddObjectVerification(Ctx, ResultPayload, RootObject);
    // Deprecated-shadow signal is emitted unconditionally — includeMetadata defaults
    // off, and the whole point is that a silent deprecated read looks identical to a
    // live-but-empty one. A nested-path movedTo hint accompanies it when derivable.
    AddDeprecatedResolutionHint(ResultPayload, RootObject, PropertyName, Property);
    if (bIncludeMetadata)
    {
        AddPropertyMetadataFields(ResultPayload, Property);
    }

    if (bIncludeDefault || bIncludeOverrideState)
    {
        const FResolvedDefaultProperty DefaultPropertyResult =
            ResolveDefaultPropertyFromObject(RootObject, PropertyName);

        if (!DefaultPropertyResult.Property ||
            !DefaultPropertyResult.ExportSource.GetContainer() ||
            !Property->SameType(DefaultPropertyResult.Property))
        {
            if (bIncludeDefault)
            {
                ResultPayload->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
            }
            if (bIncludeOverrideState)
            {
                ResultPayload->SetBoolField(TEXT("hasDefaultValue"), false);
            }
            if (!DefaultPropertyResult.Error.IsEmpty())
            {
                ResultPayload->SetStringField(TEXT("defaultLookupError"), DefaultPropertyResult.Error);
            }
        }
        else
        {
            ResultPayload->SetStringField(TEXT("defaultSource"), DefaultPropertyResult.Source);
            if (bIncludeDefault)
            {
                const TSharedPtr<FJsonValue> DefaultValue =
                    ExportPropertyToJsonValueWithOversizedOmission(
                        DefaultPropertyResult.ExportSource,
                        DefaultPropertyResult.Property,
                        bOmitOversized);
                if (DefaultValue.IsValid())
                {
                    ResultPayload->SetField(TEXT("defaultValue"), DefaultValue);
                }
                else
                {
                    ResultPayload->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
                    ResultPayload->SetStringField(
                        TEXT("defaultLookupError"),
                        FString::Printf(TEXT("Unable to export default value for %s."), *PropertyName));
                }
            }

            if (bIncludeOverrideState)
            {
                const void* CurrentValuePtr = Property->ContainerPtrToValuePtr<void>(TargetContainer);
                const void* DefaultValuePtr =
                    DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
                        DefaultPropertyResult.ExportSource.GetContainer());
                const bool bIsOverridden = !Property->Identical(CurrentValuePtr, DefaultValuePtr, PPF_None);
                ResultPayload->SetBoolField(TEXT("hasDefaultValue"), true);
                ResultPayload->SetBoolField(TEXT("isOverridden"), bIsOverridden);
            }
        }
    }

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// property.list
// ===========================================================================
REGISTER_RPC_HANDLER("property.list", "property", "List properties on a UObject with current/default/override state",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path or actor name"),
        RPC_PARAM_OPT("includeAll", "boolean", "Deprecated alias: include all reflected properties (now default true; kept for back-compat)"),
        RPC_PARAM_OPT("editableOnly", "boolean", "Restrict to instance-editable properties (CPF_Edit||CPF_BlueprintVisible). Default false."),
        RPC_PARAM_OPT("includeReadOnly", "boolean", "Include non-instance-editable properties (default false)"),
        RPC_PARAM_OPT("includeTransient", "boolean", "Include transient properties (default false)"),
        RPC_PARAM_OPT("includeValues", "boolean", "Include current property values (default true)"),
        RPC_PARAM_OPT("includeDefault", "boolean", "Include class default values (default true)"),
        RPC_PARAM_OPT("includeOverrideState", "boolean", "Include isOverridden/hasDefaultValue fields (default true)"),
        RPC_PARAM_OPT("includeMetadata", "boolean", "Include property metadata (default true)"),
        RPC_PARAM_OPT("omitOversized", "boolean", "Emit placeholders for known oversized properties instead of full values (default false)"),
        RPC_PARAM_OPT("nameMatch", "string", "Case-insensitive substring filter on UPROPERTY FName; empty/missing = no filter."),
        RPC_PARAM_OPT("propertyNames", "array", "Exact UPROPERTY FName allow-list, matched case-INsensitively (FName lookup semantics), so 'bhidden' selects 'bHidden'. Whole-name equality, not substring - use nameMatch for substring. Empty/missing = no filter.")
    ))
{
    FString ObjectPath = Ctx.GetString(TEXT("objectPath"));
    if (ObjectPath.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_OBJECT"), TEXT("property.list requires a non-empty objectPath."));
        return true;
    }

    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath);
    if (!RootObject)
    {
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"),
            FString::Printf(TEXT("Unable to find object at path %s."), *ObjectPath));
        return true;
    }

    // Default flipped: now returns all reflected properties unless caller opts into the filtered view via editableOnly.
    // `includeAll` retained as a no-op alias (default true) so legacy callers passing includeAll:true still work.
    const bool bEditableOnly = Ctx.GetBool(TEXT("editableOnly"), false);
    const bool bIncludeAll = !bEditableOnly && Ctx.GetBool(TEXT("includeAll"), true);
    const bool bIncludeReadOnly = Ctx.GetBool(TEXT("includeReadOnly"), false);
    const bool bIncludeTransient = Ctx.GetBool(TEXT("includeTransient"), false);
    const bool bIncludeValues = Ctx.GetBool(TEXT("includeValues"), true);
    const bool bIncludeDefault = Ctx.GetBool(TEXT("includeDefault"), true);
    const bool bIncludeOverrideState = Ctx.GetBool(TEXT("includeOverrideState"), true);
    const bool bIncludeMetadata = Ctx.GetBool(TEXT("includeMetadata"), true);
    const bool bOmitOversized = Ctx.GetBool(TEXT("omitOversized"), false);

    const FString NameMatch = Ctx.GetString(TEXT("nameMatch")).TrimStartAndEnd();
    TSet<FString> PropertyNameFilter;
    if (const TArray<TSharedPtr<FJsonValue>>* PropertyNamesArray = Ctx.GetArray(TEXT("propertyNames")))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *PropertyNamesArray)
        {
            if (!Entry.IsValid() || Entry->Type != EJson::String)
            {
                continue;
            }
            FString Candidate = Entry->AsString();
            Candidate.TrimStartAndEndInline();
            if (!Candidate.IsEmpty())
            {
                PropertyNameFilter.Add(Candidate);
            }
        }
    }
    const bool bNeedsDefaultResolution = bIncludeDefault || bIncludeOverrideState;
    const FResolvedDefaultSource DefaultSource =
        bNeedsDefaultResolution ? ResolveDefaultSourceObject(RootObject) : FResolvedDefaultSource();
    FDefaultPropertyLookupCache DefaultPropertyLookupCache;

    TArray<TSharedPtr<FJsonValue>> PropertyArray;
    for (TFieldIterator<FProperty> It(RootObject->GetClass()); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }

        if (!ShouldIncludePropertyInList(Property, bIncludeAll, bIncludeReadOnly))
        {
            continue;
        }

        if (!bIncludeTransient && Property->HasAnyPropertyFlags(CPF_Transient))
        {
            continue;
        }

        const FString PropName = Property->GetName();
        if (!NameMatch.IsEmpty() && !PropName.Contains(NameMatch, ESearchCase::IgnoreCase))
        {
            continue;
        }
        if (PropertyNameFilter.Num() > 0 && !PropertyNameFilter.Contains(PropName))
        {
            continue;
        }

        TSharedPtr<FJsonObject> PropertyData = MakeShared<FJsonObject>();
        PropertyData->SetStringField(TEXT("name"), PropName);

        if (bIncludeMetadata)
        {
            AddPropertyMetadataFields(PropertyData, Property);
        }

        if (bIncludeValues)
        {
            if (const TSharedPtr<FJsonValue> CurrentValue =
                ExportPropertyToJsonValueWithOversizedOmission(RootObject, Property, bOmitOversized))
            {
                PropertyData->SetField(TEXT("value"), CurrentValue);
            }
            else
            {
                PropertyData->SetStringField(TEXT("valueExportError"), TEXT("Unable to export current value."));
            }
        }

        FResolvedDefaultProperty DefaultPropertyResult;
        bool bHasDefaultValue = false;
        if (bNeedsDefaultResolution)
        {
            DefaultPropertyResult = ResolveDefaultPropertyFromSource(
                DefaultSource, Property, DefaultPropertyLookupCache);
            bHasDefaultValue =
                DefaultPropertyResult.Property &&
                DefaultPropertyResult.ExportSource.GetContainer() &&
                Property->SameType(DefaultPropertyResult.Property);

            if (bHasDefaultValue)
            {
                PropertyData->SetStringField(TEXT("defaultSource"), DefaultPropertyResult.Source);
            }
            else if (!DefaultPropertyResult.Error.IsEmpty())
            {
                PropertyData->SetStringField(TEXT("defaultLookupError"), DefaultPropertyResult.Error);
            }
        }

        if (bIncludeDefault)
        {
            if (bHasDefaultValue)
            {
                if (const TSharedPtr<FJsonValue> DefaultValue =
                    ExportPropertyToJsonValueWithOversizedOmission(
                        DefaultPropertyResult.ExportSource,
                        DefaultPropertyResult.Property,
                        bOmitOversized))
                {
                    PropertyData->SetField(TEXT("defaultValue"), DefaultValue);
                }
                else
                {
                    PropertyData->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
                    PropertyData->SetStringField(TEXT("defaultValueError"), TEXT("Unable to export default value."));
                }
            }
            else
            {
                PropertyData->SetField(TEXT("defaultValue"), MakeShared<FJsonValueNull>());
            }
        }

        if (bIncludeOverrideState)
        {
            PropertyData->SetBoolField(TEXT("hasDefaultValue"), bHasDefaultValue);
            if (bHasDefaultValue)
            {
                const void* CurrentValuePtr = Property->ContainerPtrToValuePtr<void>(RootObject);
                const void* DefaultValuePtr =
                    DefaultPropertyResult.Property->ContainerPtrToValuePtr<void>(
                        DefaultPropertyResult.ExportSource.GetContainer());
                const bool bIsOverridden = !Property->Identical(CurrentValuePtr, DefaultValuePtr, PPF_None);
                PropertyData->SetBoolField(TEXT("isOverridden"), bIsOverridden);
            }
        }

        PropertyArray.Add(MakeShared<FJsonValueObject>(PropertyData));
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), RootObject->GetPathName());
    ResultPayload->SetStringField(TEXT("className"), RootObject->GetClass()->GetName());
    ResultPayload->SetArrayField(TEXT("properties"), PropertyArray);
    ResultPayload->SetNumberField(TEXT("count"), PropertyArray.Num());
    AddObjectVerification(Ctx, ResultPayload, RootObject);

    if (RootObject)
    {
        if (UPackage* Pkg = RootObject->GetOutermost())
        {
            const FString PkgName = Pkg->GetName();
            // Skip transient/script/engine-transient objects — nothing to dump.
            if (Pkg != GetTransientPackage() && !PkgName.StartsWith(TEXT("/Script")) && !PkgName.StartsWith(TEXT("/Engine/Transient")))
            {
                const AssetDumpSuggestion::EDumpSubjectKind Kind = Pkg->ContainsMap()
                    ? AssetDumpSuggestion::EDumpSubjectKind::Level
                    : AssetDumpSuggestion::EDumpSubjectKind::Asset;
                if (FString DumpHint = AssetDumpSuggestion::BuildDumpSuggestionHint(PkgName, Kind); !DumpHint.IsEmpty())
                {
                    ResultPayload->SetStringField(TEXT("hint"), DumpHint);
                }
            }
        }
    }

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// Helper macros for repetitive container handler preamble
// ===========================================================================

// Resolve object + property to array, sending errors on failure
#define RESOLVE_ARRAY_PROPERTY(HandlerName) \
    FString ObjectPath = Ctx.GetString(TEXT("objectPath")); \
    FString PropertyName = Ctx.GetString(TEXT("propertyName")); \
    if (ObjectPath.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT(#HandlerName " requires objectPath.")); \
        return true; \
    } \
    if (PropertyName.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT(#HandlerName " requires propertyName.")); \
        return true; \
    } \
    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath); \
    if (!RootObject) { \
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("Object not found: %s"), *ObjectPath)); \
        return true; \
    } \
    void* TargetContainer = nullptr; \
    FProperty* Property = nullptr; \
    if (!ResolvePropertyFromObject(Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property)) \
        return true; \
    FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property); \
    if (!ArrayProp) { \
        Ctx.SendError(TEXT("NOT_AN_ARRAY"), TEXT("Property is not an array.")); \
        return true; \
    }

#define RESOLVE_MAP_PROPERTY(HandlerName) \
    FString ObjectPath = Ctx.GetString(TEXT("objectPath")); \
    FString PropertyName = Ctx.GetString(TEXT("propertyName")); \
    if (ObjectPath.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT(#HandlerName " requires objectPath.")); \
        return true; \
    } \
    if (PropertyName.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT(#HandlerName " requires propertyName.")); \
        return true; \
    } \
    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath); \
    if (!RootObject) { \
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("Object not found: %s"), *ObjectPath)); \
        return true; \
    } \
    void* TargetContainer = nullptr; \
    FProperty* Property = nullptr; \
    if (!ResolvePropertyFromObject(Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property)) \
        return true; \
    FMapProperty* MapProp = CastField<FMapProperty>(Property); \
    if (!MapProp) { \
        Ctx.SendError(TEXT("NOT_A_MAP"), TEXT("Property is not a map.")); \
        return true; \
    }

#define RESOLVE_SET_PROPERTY(HandlerName) \
    FString ObjectPath = Ctx.GetString(TEXT("objectPath")); \
    FString PropertyName = Ctx.GetString(TEXT("propertyName")); \
    if (ObjectPath.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT(#HandlerName " requires objectPath.")); \
        return true; \
    } \
    if (PropertyName.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT(#HandlerName " requires propertyName.")); \
        return true; \
    } \
    UObject* RootObject = ResolveObjectForProperty(Ctx, ObjectPath); \
    if (!RootObject) { \
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("Object not found: %s"), *ObjectPath)); \
        return true; \
    } \
    void* TargetContainer = nullptr; \
    FProperty* Property = nullptr; \
    if (!ResolvePropertyFromObject(Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property)) \
        return true; \
    FSetProperty* SetProp = CastField<FSetProperty>(Property); \
    if (!SetProp) { \
        Ctx.SendError(TEXT("NOT_A_SET"), TEXT("Property is not a set.")); \
        return true; \
    }

// ===========================================================================
// container.array.append
// ===========================================================================
REGISTER_RPC_HANDLER("container.array.append", "container", "Append an element to an array property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Array property name"),
        RPC_PARAM_REQ("value", "any", "Value to append")
    ))
{
    RESOLVE_ARRAY_PROPERTY(array_append)

    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("array_append requires value field."));
        return true;
    }

    FProperty* Inner = ArrayProp->Inner;
    void* StagedValue = Inner->AllocateAndInitializeValue();
    FString ConversionError;
    if (!StagedValue
        || !ApplyJsonValueToProperty(StagedValue, Inner, ValueField, ConversionError))
    {
        if (StagedValue)
        {
            Inner->DestroyValue(StagedValue);
            FMemory::Free(StagedValue);
        }
        Ctx.SendError(TEXT("UNSUPPORTED_TYPE"),
            FString::Printf(TEXT("Unsupported array element type '%s': %s"),
                *Inner->GetClass()->GetName(),
                ConversionError.IsEmpty() ? TEXT("Failed to stage array element") : *ConversionError));
        return true;
    }

    RootObject->Modify();

    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(TargetContainer));
    const int32 NewIndex = Helper.AddValue();
    void* ElemPtr = Helper.GetRawPtr(NewIndex);
    Inner->CopySingleValue(ElemPtr, StagedValue);
    Inner->DestroyValue(StagedValue);
    FMemory::Free(StagedValue);

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayAdd);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("newIndex"), NewIndex);
    ResultPayload->SetNumberField(TEXT("newSize"), Helper.Num());
    AddObjectVerification(Ctx, ResultPayload, RootObject);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.array.remove
// ===========================================================================
REGISTER_RPC_HANDLER("container.array.remove", "container", "Remove an element from an array by index",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Array property name"),
        RPC_PARAM_REQ("index", "integer", "Index to remove")
    ))
{
    RESOLVE_ARRAY_PROPERTY(array_remove)

    int32 Index = -1;
    if (!Ctx.RequireInt(TEXT("index"), Index))
    {
        return true;
    }
    if (Index < 0)
    {
        Ctx.SendError(TEXT("INVALID_INDEX"), TEXT("array_remove requires valid index."));
        return true;
    }

    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(TargetContainer));
    if (Index >= Helper.Num())
    {
        Ctx.SendError(TEXT("INDEX_OUT_OF_RANGE"),
            FString::Printf(TEXT("Index %d out of range (size: %d)"), Index, Helper.Num()));
        return true;
    }

    RootObject->Modify();
    Helper.RemoveValues(Index, 1);

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayRemove);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("removedIndex"), Index);
    ResultPayload->SetNumberField(TEXT("newSize"), Helper.Num());
    AddObjectVerification(Ctx, ResultPayload, RootObject);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.array.clear
// ===========================================================================
REGISTER_RPC_HANDLER("container.array.clear", "container", "Clear all elements from an array property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Array property name")
    ))
{
    RESOLVE_ARRAY_PROPERTY(array_clear)

    RootObject->Modify();

    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(TargetContainer));
    const int32 PrevSize = Helper.Num();
    Helper.EmptyValues();

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayClear);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("previousSize"), PrevSize);
    ResultPayload->SetNumberField(TEXT("newSize"), 0);
    AddObjectVerification(Ctx, ResultPayload, RootObject);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.array.insert
// ===========================================================================
REGISTER_RPC_HANDLER("container.array.insert", "container", "Insert an element into an array at an index",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Array property name"),
        RPC_PARAM_REQ("index", "integer", "Index to insert at"),
        RPC_PARAM_REQ("value", "any", "Value to insert")
    ))
{
    RESOLVE_ARRAY_PROPERTY(array_insert)

    int32 Index = -1;
    if (!Ctx.RequireInt(TEXT("index"), Index))
    {
        return true;
    }
    if (Index < 0)
    {
        Ctx.SendError(TEXT("INVALID_INDEX"), TEXT("array_insert requires valid index."));
        return true;
    }

    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("array_insert requires value field."));
        return true;
    }

    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(TargetContainer));
    if (Index > Helper.Num())
    {
        Ctx.SendError(TEXT("INDEX_OUT_OF_RANGE"),
            FString::Printf(TEXT("Index %d out of range (size: %d)"), Index, Helper.Num()));
        return true;
    }

    FProperty* Inner = ArrayProp->Inner;
    void* StagedValue = Inner->AllocateAndInitializeValue();
    FString ElemError;
    if (!StagedValue
        || !ApplyJsonValueToProperty(StagedValue, Inner, ValueField, ElemError))
    {
        if (StagedValue)
        {
            Inner->DestroyValue(StagedValue);
            FMemory::Free(StagedValue);
        }
        Ctx.SendError(TEXT("UNSUPPORTED_TYPE"),
            FString::Printf(TEXT("Unsupported array element type '%s': %s"),
                *Inner->GetClass()->GetName(),
                ElemError.IsEmpty() ? TEXT("Failed to stage array element") : *ElemError));
        return true;
    }

    RootObject->Modify();

    Helper.InsertValues(Index, 1);
    void* ElemPtr = Helper.GetRawPtr(Index);
    Inner->CopySingleValue(ElemPtr, StagedValue);
    Inner->DestroyValue(StagedValue);
    FMemory::Free(StagedValue);

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayAdd);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("insertedAt"), Index);
    ResultPayload->SetNumberField(TEXT("newSize"), Helper.Num());

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.array.get
// ===========================================================================
REGISTER_RPC_HANDLER("container.array.get", "container", "Get an element from an array by index",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Array property name"),
        RPC_PARAM_REQ("index", "integer", "Index to retrieve")
    ))
{
    RESOLVE_ARRAY_PROPERTY(array_get_element)

    int32 Index = -1;
    if (!Ctx.RequireInt(TEXT("index"), Index))
    {
        return true;
    }
    if (Index < 0)
    {
        Ctx.SendError(TEXT("INVALID_INDEX"), TEXT("array_get_element requires valid index."));
        return true;
    }

    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(TargetContainer));
    if (Index >= Helper.Num())
    {
        Ctx.SendError(TEXT("INDEX_OUT_OF_RANGE"),
            FString::Printf(TEXT("Index %d out of range (size: %d)"), Index, Helper.Num()));
        return true;
    }

    void* ElemPtr = Helper.GetRawPtr(Index);
    FProperty* Inner = ArrayProp->Inner;

    // Export the element through the shared ExportPropertyToJsonValue path so struct /
    // object / soft-object / FText / enum / FName elements come back as structured JSON
    // instead of erroring (F-container-map-value-type-coverage generalizes across both
    // containers). An array element pointer IS a valid container for Inner: FArrayProperty
    // leaves Inner->Offset_Internal == 0, so ContainerPtrToValuePtr(ElemPtr) == ElemPtr.
    TSharedPtr<FJsonValue> ElemValue = ExportPropertyToJsonValue(ElemPtr, Inner);
    if (!ElemValue.IsValid())
    {
        Ctx.SendError(TEXT("UNSUPPORTED_TYPE"),
            FString::Printf(TEXT("Unsupported array element type '%s'."),
                *Inner->GetClass()->GetName()));
        return true;
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("index"), Index);
    ResultPayload->SetField(TEXT("value"), ElemValue);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.array.set
// ===========================================================================
REGISTER_RPC_HANDLER("container.array.set", "container", "Set an element in an array by index",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Array property name"),
        RPC_PARAM_REQ("index", "integer", "Index to set"),
        RPC_PARAM_REQ("value", "any", "Value to set")
    ))
{
    RESOLVE_ARRAY_PROPERTY(array_set_element)

    int32 Index = -1;
    if (!Ctx.RequireInt(TEXT("index"), Index))
    {
        return true;
    }
    if (Index < 0)
    {
        Ctx.SendError(TEXT("INVALID_INDEX"), TEXT("array_set_element requires valid index."));
        return true;
    }

    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("array_set_element requires value field."));
        return true;
    }

    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(TargetContainer));
    if (Index >= Helper.Num())
    {
        Ctx.SendError(TEXT("INDEX_OUT_OF_RANGE"),
            FString::Printf(TEXT("Index %d out of range (size: %d)"), Index, Helper.Num()));
        return true;
    }

    FProperty* Inner = ArrayProp->Inner;
    void* StagedValue = Inner->AllocateAndInitializeValue();
    FString ElemError;
    if (StagedValue)
    {
        // Preserve the existing element so partial struct/object conversion failures only
        // affect scratch storage. This also keeps omitted struct fields unchanged on success.
        Inner->CopyCompleteValue(StagedValue, Helper.GetRawPtr(Index));
        if (!ApplyJsonValueToProperty(StagedValue, Inner, ValueField, ElemError))
        {
            Inner->DestroyValue(StagedValue);
            FMemory::Free(StagedValue);
            StagedValue = nullptr;
        }
    }

    if (!StagedValue)
    {
        Ctx.SendError(TEXT("UNSUPPORTED_TYPE"),
            FString::Printf(TEXT("Unsupported array element type '%s': %s"),
                *Inner->GetClass()->GetName(),
                ElemError.IsEmpty() ? TEXT("Failed to stage array element") : *ElemError));
        return true;
    }

    RootObject->Modify();

    Inner->CopyCompleteValue(Helper.GetRawPtr(Index), StagedValue);
    Inner->DestroyValue(StagedValue);
    FMemory::Free(StagedValue);

    // ValueSet on the ARRAY property: the container's shape did not change, one element's
    // value did. The edited index is deliberately not attached — FPropertyChangedEvent
    // carries it only via SetArrayIndexPerObject, which takes a per-top-level-object map
    // the engine's own multi-object edit path builds; a single-object verb has nothing
    // truthful to put in it. Overrides that need the index re-read the array.
    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ValueSet);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("index"), Index);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.map.set
// ===========================================================================
REGISTER_RPC_HANDLER("container.map.set", "container", "Set a value in a map property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Map property name"),
        RPC_PARAM_REQ("key", "string", "Map key"),
        RPC_PARAM_REQ("value", "any", "Value to set")
    ))
{
    RESOLVE_MAP_PROPERTY(map_set_value)

    FString Key;
    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("map_set_value requires value field."));
        return true;
    }

    FProperty* KeyProp = MapProp->KeyProp;
    FProperty* ValueProp = MapProp->ValueProp;
    int32 ParsedIntKey = 0;
    if (CastField<FIntProperty>(KeyProp))
    {
        const TSharedPtr<FJsonValue> KeyField = Ctx.GetRawPayload()->TryGetField(TEXT("key"));
        FString KeyError;
        if (!TryParseContainerInt32(KeyField, ParsedIntKey, KeyError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'key' must be a valid int32: %s"), *KeyError));
            return true;
        }
        Key = FString::FromInt(ParsedIntKey);
    }
    else
    {
        Key = Ctx.GetString(TEXT("key"));
    }

    FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(TargetContainer));
    void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
    void* TempValue = FMemory::Malloc(ValueProp->GetSize(), ValueProp->GetMinAlignment());
    KeyProp->InitializeValue(TempKey);
    ValueProp->InitializeValue(TempValue);

    bool bSuccess = false;
    if (FStrProperty* StrKey = CastField<FStrProperty>(KeyProp))
    {
        *reinterpret_cast<FString*>(TempKey) = Key;
        bSuccess = true;
    }
    else if (FNameProperty* NameKey = CastField<FNameProperty>(KeyProp))
    {
        *reinterpret_cast<FName*>(TempKey) = FName(*Key);
        bSuccess = true;
    }
    else if (FIntProperty* IntKey = CastField<FIntProperty>(KeyProp))
    {
        *reinterpret_cast<int32*>(TempKey) = ParsedIntKey;
        bSuccess = true;
    }

    if (!bSuccess)
    {
        KeyProp->DestroyValue(TempKey);
        ValueProp->DestroyValue(TempValue);
        FMemory::Free(TempKey);
        FMemory::Free(TempValue);
        Ctx.SendError(TEXT("UNSUPPORTED_KEY_TYPE"), TEXT("Unsupported map key type."));
        return true;
    }

    // Write the value through the shared ApplyJsonValueToProperty path so struct /
    // object / soft-object / FText / enum / FName values can be set, not just the four
    // primitives (F-container-map-value-type-coverage). Build into a standalone TempValue
    // first and only AddPair once the write succeeds, so the live map is never touched
    // until the value is known-good — no insert-before-validate, no rollback, no clobber
    // of an existing key's value when the new value fails to parse. ApplyJsonValueToProperty
    // takes a CONTAINER base and re-derives the value via ContainerPtrToValuePtr (= base +
    // ValueProp->Offset_Internal), so feeding TempValue - Offset_Internal lands the write
    // exactly on the TempValue buffer (the same offset trick the map.get value path uses
    // against GetPairPtr).
    uint8* ValueContainer = static_cast<uint8*>(TempValue) - ValueProp->GetOffset_ForInternal();

    FString ValueError;
    if (!ApplyJsonValueToProperty(ValueContainer, ValueProp, ValueField, ValueError))
    {
        KeyProp->DestroyValue(TempKey);
        ValueProp->DestroyValue(TempValue);
        FMemory::Free(TempKey);
        FMemory::Free(TempValue);
        Ctx.SendError(TEXT("UNSUPPORTED_VALUE_TYPE"),
            FString::Printf(TEXT("Unsupported map value type '%s': %s"),
                *ValueProp->GetClass()->GetName(), *ValueError));
        return true;
    }

    RootObject->Modify();
    Helper.AddPair(TempKey, TempValue);

    KeyProp->DestroyValue(TempKey);
    ValueProp->DestroyValue(TempValue);
    FMemory::Free(TempKey);
    FMemory::Free(TempValue);

    // ArrayAdd, not ValueSet: AddPair grows the map for a new key and only overwrites
    // for an existing one, and UE's own property editor reports a map insertion as
    // ArrayAdd (PropertyHandleImpl.cpp:1911-1915). Reporting the shape change is the
    // safe direction — an override that rebuilds on ArrayAdd also handles a value it
    // has already seen; one told ValueSet may never rebuild for a key that appeared.
    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayAdd);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetStringField(TEXT("key"), Key);
    ResultPayload->SetNumberField(TEXT("mapSize"), Helper.Num());

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.map.get
// ===========================================================================
REGISTER_RPC_HANDLER("container.map.get", "container", "Get a value from a map property by key",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Map property name"),
        RPC_PARAM_REQ("key", "string", "Map key")
    ))
{
    RESOLVE_MAP_PROPERTY(map_get_value)

    FProperty* KeyProp = MapProp->KeyProp;
    FString Key;
    int32 ParsedIntKey = 0;
    if (CastField<FIntProperty>(KeyProp))
    {
        const TSharedPtr<FJsonValue> KeyField = Ctx.GetRawPayload()->TryGetField(TEXT("key"));
        FString KeyError;
        if (!TryParseContainerInt32(KeyField, ParsedIntKey, KeyError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'key' must be a valid int32: %s"), *KeyError));
            return true;
        }
        Key = FString::FromInt(ParsedIntKey);
    }
    else
    {
        Key = Ctx.GetString(TEXT("key"));
    }

    FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(TargetContainer));
    FProperty* ValueProp = MapProp->ValueProp;

    // Bound by GetMaxIndex(), not Num(): a script map has gaps in its internal index
    // space after a RemoveAt (non-compact sparse set frees the slot in place), so the
    // highest-indexed survivor can live at an internal index == Num(). Num() (the live
    // element count) would stop one index short and silently skip that survivor;
    // GetMaxIndex() (>= Num()) spans the full slot range. IsValidIndex skips the holes.
    for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
    {
        if (!Helper.IsValidIndex(i))
            continue;

        const uint8* KeyPtr = Helper.GetKeyPtr(i);
        FString KeyStr;

        if (CastField<FStrProperty>(KeyProp))
            KeyStr = *reinterpret_cast<const FString*>(KeyPtr);
        else if (CastField<FNameProperty>(KeyProp))
            KeyStr = reinterpret_cast<const FName*>(KeyPtr)->ToString();
        else if (CastField<FIntProperty>(KeyProp))
            KeyStr = FString::FromInt(*reinterpret_cast<const int32*>(KeyPtr));

        if (KeyStr.Equals(Key))
        {
            // Export the value through the shared ExportPropertyToJsonValue path so
            // struct / object / soft-object / FText / enum / FName values come back as
            // structured JSON instead of erroring — covering whatever property.get
            // covers (F-container-map-value-type-coverage), not just the four primitives.
            //
            // ExportPropertyToJsonValue takes a CONTAINER base and re-derives the value
            // via ValueProp->ContainerPtrToValuePtr (= base + ValueProp->Offset_Internal).
            // For a map, UE sets ValueProp->Offset_Internal == MapLayout.ValueOffset
            // (PropertyMap.cpp), and GetValuePtr(i) == GetPairPtr(i) + MapLayout.ValueOffset,
            // so the PAIR pointer is the correct container — passing it recovers exactly
            // the value pointer. (Passing GetValuePtr directly would double-add the offset.)
            uint8* PairPtr = Helper.GetPairPtr(i);
            TSharedPtr<FJsonValue> ValueJson =
                ExportPropertyToJsonValue(PairPtr, ValueProp);
            if (!ValueJson.IsValid())
            {
                Ctx.SendError(TEXT("UNSUPPORTED_VALUE_TYPE"),
                    FString::Printf(TEXT("Unsupported map value type '%s'."),
                        *ValueProp->GetClass()->GetName()));
                return true;
            }

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            ResultPayload->SetStringField(TEXT("key"), Key);
            ResultPayload->SetField(TEXT("value"), ValueJson);

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
    }

    Ctx.SendError(TEXT("KEY_NOT_FOUND"), FString::Printf(TEXT("Key '%s' not found in map."), *Key));
    return true;
}

// ===========================================================================
// container.map.remove
// ===========================================================================
REGISTER_RPC_HANDLER("container.map.remove", "container", "Remove a key from a map property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Map property name"),
        RPC_PARAM_REQ("key", "string", "Map key to remove")
    ))
{
    RESOLVE_MAP_PROPERTY(map_remove_key)

    FProperty* KeyProp = MapProp->KeyProp;
    FString Key;
    int32 ParsedIntKey = 0;
    if (CastField<FIntProperty>(KeyProp))
    {
        const TSharedPtr<FJsonValue> KeyField = Ctx.GetRawPayload()->TryGetField(TEXT("key"));
        FString KeyError;
        if (!TryParseContainerInt32(KeyField, ParsedIntKey, KeyError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'key' must be a valid int32: %s"), *KeyError));
            return true;
        }
        Key = FString::FromInt(ParsedIntKey);
    }
    else
    {
        Key = Ctx.GetString(TEXT("key"));
    }

    RootObject->Modify();

    FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(TargetContainer));

    // Bound by GetMaxIndex(), not Num() — see container.map.get: a prior RemoveAt
    // leaves index gaps, so Num() would skip a survivor at internal index == Num().
    for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
    {
        if (!Helper.IsValidIndex(i))
            continue;

        const uint8* KeyPtr = Helper.GetKeyPtr(i);
        FString KeyStr;

        if (CastField<FStrProperty>(KeyProp))
            KeyStr = *reinterpret_cast<const FString*>(KeyPtr);
        else if (CastField<FNameProperty>(KeyProp))
            KeyStr = reinterpret_cast<const FName*>(KeyPtr)->ToString();
        else if (CastField<FIntProperty>(KeyProp))
            KeyStr = FString::FromInt(*reinterpret_cast<const int32*>(KeyPtr));

        if (KeyStr.Equals(Key))
        {
            Helper.RemoveAt(i);

            NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayRemove);

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            ResultPayload->SetStringField(TEXT("key"), Key);
            ResultPayload->SetNumberField(TEXT("mapSize"), Helper.Num());

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
    }

    Ctx.SendError(TEXT("KEY_NOT_FOUND"), FString::Printf(TEXT("Key '%s' not found in map."), *Key));
    return true;
}

// ===========================================================================
// container.map.has_key
// ===========================================================================
REGISTER_RPC_HANDLER("container.map.has_key", "container", "Check if a map property contains a key",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Map property name"),
        RPC_PARAM_REQ("key", "string", "Key to check")
    ))
{
    RESOLVE_MAP_PROPERTY(map_has_key)

    FProperty* KeyProp = MapProp->KeyProp;
    FString Key;
    int32 ParsedIntKey = 0;
    if (CastField<FIntProperty>(KeyProp))
    {
        const TSharedPtr<FJsonValue> KeyField = Ctx.GetRawPayload()->TryGetField(TEXT("key"));
        FString KeyError;
        if (!TryParseContainerInt32(KeyField, ParsedIntKey, KeyError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'key' must be a valid int32: %s"), *KeyError));
            return true;
        }
        Key = FString::FromInt(ParsedIntKey);
    }
    else
    {
        Key = Ctx.GetString(TEXT("key"));
    }

    FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(TargetContainer));

    bool bHasKey = false;
    // Bound by GetMaxIndex(), not Num() — see container.map.get: a prior RemoveAt
    // leaves index gaps, so Num() would skip a survivor at internal index == Num().
    for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
    {
        if (!Helper.IsValidIndex(i))
            continue;

        const uint8* KeyPtr = Helper.GetKeyPtr(i);
        FString KeyStr;

        if (CastField<FStrProperty>(KeyProp))
            KeyStr = *reinterpret_cast<const FString*>(KeyPtr);
        else if (CastField<FNameProperty>(KeyProp))
            KeyStr = reinterpret_cast<const FName*>(KeyPtr)->ToString();
        else if (CastField<FIntProperty>(KeyProp))
            KeyStr = FString::FromInt(*reinterpret_cast<const int32*>(KeyPtr));

        if (KeyStr.Equals(Key))
        {
            bHasKey = true;
            break;
        }
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetStringField(TEXT("key"), Key);
    ResultPayload->SetBoolField(TEXT("hasKey"), bHasKey);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.map.get_keys
// ===========================================================================
REGISTER_RPC_HANDLER("container.map.get_keys", "container", "Get all keys from a map property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Map property name")
    ))
{
    RESOLVE_MAP_PROPERTY(map_get_keys)

    FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(TargetContainer));
    FProperty* KeyProp = MapProp->KeyProp;

    TArray<TSharedPtr<FJsonValue>> KeysArray;
    // Bound by GetMaxIndex(), not Num() — see container.map.get: a prior RemoveAt
    // leaves index gaps, so Num() would skip a survivor at internal index == Num()
    // (this is the path the live repro hit — get_keys silently dropping a key).
    for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
    {
        if (!Helper.IsValidIndex(i))
            continue;

        const uint8* KeyPtr = Helper.GetKeyPtr(i);

        if (CastField<FStrProperty>(KeyProp))
            KeysArray.Add(MakeShared<FJsonValueString>(*reinterpret_cast<const FString*>(KeyPtr)));
        else if (CastField<FNameProperty>(KeyProp))
            KeysArray.Add(MakeShared<FJsonValueString>(reinterpret_cast<const FName*>(KeyPtr)->ToString()));
        else if (CastField<FIntProperty>(KeyProp))
            KeysArray.Add(MakeShared<FJsonValueNumber>((double)*reinterpret_cast<const int32*>(KeyPtr)));
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetArrayField(TEXT("keys"), KeysArray);
    ResultPayload->SetNumberField(TEXT("keyCount"), KeysArray.Num());

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.map.clear
// ===========================================================================
REGISTER_RPC_HANDLER("container.map.clear", "container", "Clear all entries from a map property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Map property name")
    ))
{
    RESOLVE_MAP_PROPERTY(map_clear)

    RootObject->Modify();

    FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(TargetContainer));
    const int32 PrevSize = Helper.Num();
    Helper.EmptyValues();

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayClear);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("previousSize"), PrevSize);
    ResultPayload->SetNumberField(TEXT("newSize"), 0);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.set.add
// ===========================================================================
REGISTER_RPC_HANDLER("container.set.add", "container", "Add an element to a set property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Set property name"),
        RPC_PARAM_REQ("value", "any", "Value to add")
    ))
{
    RESOLVE_SET_PROPERTY(set_add)

    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("set_add requires value field."));
        return true;
    }

    FProperty* ElemProp = SetProp->ElementProp;
    void* StagedValue = nullptr;
    const bool bStageScalar = IsJsonScalarProperty(ElemProp);
    FString ElementError;
    if (bStageScalar
        && !TryStageJsonValueForProperty(ElemProp, ValueField, StagedValue, ElementError))
    {
        if (CastField<FIntProperty>(ElemProp))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'value' must be a valid int32: %s"),
                    *ElementError));
        }
        else
        {
            Ctx.SendError(TEXT("UNSUPPORTED_TYPE"),
                FString::Printf(TEXT("Unsupported set element type '%s': %s"),
                    *ElemProp->GetClass()->GetName(), *ElementError));
        }
        return true;
    }

    RootObject->Modify();

    FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(TargetContainer));
    if (bStageScalar)
    {
        Helper.AddElement(StagedValue);
        ElemProp->DestroyValue(StagedValue);
        FMemory::Free(StagedValue);
    }
    else
    {
        void* TempElem = FMemory::Malloc(ElemProp->GetSize(), ElemProp->GetMinAlignment());
        ElemProp->InitializeValue(TempElem);

        bool bSuccess = false;
        if (FStrProperty* StrElem = CastField<FStrProperty>(ElemProp))
        {
            *reinterpret_cast<FString*>(TempElem) = (ValueField->Type == EJson::String)
                ? ValueField->AsString() : FString::Printf(TEXT("%g"), ValueField->AsNumber());
            bSuccess = true;
        }
        else if (FNameProperty* NameElem = CastField<FNameProperty>(ElemProp))
        {
            *reinterpret_cast<FName*>(TempElem) = (ValueField->Type == EJson::String)
                ? FName(*ValueField->AsString()) : NAME_None;
            bSuccess = true;
        }

        if (!bSuccess)
        {
            ElemProp->DestroyValue(TempElem);
            FMemory::Free(TempElem);
            Ctx.SendError(TEXT("UNSUPPORTED_TYPE"), TEXT("Unsupported set element type."));
            return true;
        }

        Helper.AddElement(TempElem);

        ElemProp->DestroyValue(TempElem);
        FMemory::Free(TempElem);
    }

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayAdd);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("setSize"), Helper.Num());

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.set.remove
// ===========================================================================
REGISTER_RPC_HANDLER("container.set.remove", "container", "Remove an element from a set property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Set property name"),
        RPC_PARAM_REQ("value", "any", "Value to remove")
    ))
{
    RESOLVE_SET_PROPERTY(set_remove)

    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("set_remove requires value field."));
        return true;
    }

    FProperty* ElemProp = SetProp->ElementProp;
    int32 ParsedIntElement = 0;
    if (CastField<FIntProperty>(ElemProp))
    {
        FString ElementError;
        if (!TryParseContainerInt32(ValueField, ParsedIntElement, ElementError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'value' must be a valid int32: %s"), *ElementError));
            return true;
        }
    }

    RootObject->Modify();

    FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(TargetContainer));

    // Coerce the FName search value once: it is loop-invariant, and FName
    // construction (FString materialize + global name-table hash/lookup under a
    // lock) is the costliest of the four element-type coercions to repeat per element.
    const FName SearchName = (ValueField->Type == EJson::String)
        ? FName(*ValueField->AsString()) : NAME_None;

    // Bound by GetMaxIndex(), not Num(): a script set has index gaps after a prior
    // RemoveAt, so Num() (the element count) would skip a survivor living at internal
    // index == Num(). GetMaxIndex() (>= Num()) spans every slot; IsValidIndex skips holes.
    for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
    {
        if (!Helper.IsValidIndex(i))
            continue;

        const uint8* ElemPtr = Helper.GetElementPtr(i);
        bool bMatch = false;

        if (FStrProperty* StrElem = CastField<FStrProperty>(ElemProp))
        {
            const FString& ElemValue = *reinterpret_cast<const FString*>(ElemPtr);
            const FString SearchValue = (ValueField->Type == EJson::String)
                ? ValueField->AsString() : FString::Printf(TEXT("%g"), ValueField->AsNumber());
            bMatch = ElemValue.Equals(SearchValue);
        }
        else if (FIntProperty* IntElem = CastField<FIntProperty>(ElemProp))
        {
            const int32 ElemValue = *reinterpret_cast<const int32*>(ElemPtr);
            bMatch = (ElemValue == ParsedIntElement);
        }
        else if (FFloatProperty* FloatElem = CastField<FFloatProperty>(ElemProp))
        {
            const float ElemValue = *reinterpret_cast<const float*>(ElemPtr);
            const float SearchValue = (ValueField->Type == EJson::Number)
                ? (float)ValueField->AsNumber() : (float)FCString::Atod(*ValueField->AsString());
            bMatch = FMath::IsNearlyEqual(ElemValue, SearchValue);
        }
        else if (FNameProperty* NameElem = CastField<FNameProperty>(ElemProp))
        {
            // Mirror set.add's FName coercion (FName(*ValueField->AsString())) so a
            // TSet<FName> element can be matched. Without this branch the loop never
            // sets bMatch and a present FName falls through to ELEMENT_NOT_FOUND.
            const FName& ElemValue = *reinterpret_cast<const FName*>(ElemPtr);
            bMatch = (ElemValue == SearchName);
        }

        if (bMatch)
        {
            Helper.RemoveAt(i);

            NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayRemove);

            TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
            ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
            ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
            ResultPayload->SetNumberField(TEXT("setSize"), Helper.Num());

            Ctx.SendSuccess(ResultPayload);
            return true;
        }
    }

    Ctx.SendError(TEXT("ELEMENT_NOT_FOUND"), TEXT("Element not found in set."));
    return true;
}

// ===========================================================================
// container.set.contains
// ===========================================================================
REGISTER_RPC_HANDLER("container.set.contains", "container", "Check if a set property contains an element",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Set property name"),
        RPC_PARAM_REQ("value", "any", "Value to check")
    ))
{
    RESOLVE_SET_PROPERTY(set_contains)

    const TSharedPtr<FJsonValue> ValueField = Ctx.GetRawPayload()->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), TEXT("set_contains requires value field."));
        return true;
    }

    FProperty* ElemProp = SetProp->ElementProp;
    int32 ParsedIntElement = 0;
    if (CastField<FIntProperty>(ElemProp))
    {
        FString ElementError;
        if (!TryParseContainerInt32(ValueField, ParsedIntElement, ElementError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("Field 'value' must be a valid int32: %s"), *ElementError));
            return true;
        }
    }

    FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(TargetContainer));

    // Coerce the FName search value once: it is loop-invariant, and FName
    // construction (FString materialize + global name-table hash/lookup under a
    // lock) is the costliest of the four element-type coercions to repeat per element.
    const FName SearchName = (ValueField->Type == EJson::String)
        ? FName(*ValueField->AsString()) : NAME_None;

    bool bContains = false;
    // Bound by GetMaxIndex(), not Num() — see container.set.remove: a prior RemoveAt
    // leaves index gaps, so Num() would skip a survivor at internal index == Num().
    for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
    {
        if (!Helper.IsValidIndex(i))
            continue;

        const uint8* ElemPtr = Helper.GetElementPtr(i);

        if (FStrProperty* StrElem = CastField<FStrProperty>(ElemProp))
        {
            const FString& ElemValue = *reinterpret_cast<const FString*>(ElemPtr);
            const FString SearchValue = (ValueField->Type == EJson::String)
                ? ValueField->AsString() : FString::Printf(TEXT("%g"), ValueField->AsNumber());
            if (ElemValue.Equals(SearchValue)) { bContains = true; break; }
        }
        else if (FIntProperty* IntElem = CastField<FIntProperty>(ElemProp))
        {
            const int32 ElemValue = *reinterpret_cast<const int32*>(ElemPtr);
            if (ElemValue == ParsedIntElement) { bContains = true; break; }
        }
        else if (FFloatProperty* FloatElem = CastField<FFloatProperty>(ElemProp))
        {
            const float ElemValue = *reinterpret_cast<const float*>(ElemPtr);
            const float SearchValue = (ValueField->Type == EJson::Number)
                ? (float)ValueField->AsNumber() : (float)FCString::Atod(*ValueField->AsString());
            if (FMath::IsNearlyEqual(ElemValue, SearchValue)) { bContains = true; break; }
        }
        else if (FNameProperty* NameElem = CastField<FNameProperty>(ElemProp))
        {
            // Mirror set.add's FName coercion so membership of a TSet<FName> element
            // resolves true. Without this branch contains silently returns false for
            // every genuinely present FName member.
            const FName& ElemValue = *reinterpret_cast<const FName*>(ElemPtr);
            if (ElemValue == SearchName) { bContains = true; break; }
        }
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetBoolField(TEXT("contains"), bContains);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// container.set.clear
// ===========================================================================
REGISTER_RPC_HANDLER("container.set.clear", "container", "Clear all elements from a set property",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path"),
        RPC_PARAM_REQ("propertyName", "string", "Set property name")
    ))
{
    RESOLVE_SET_PROPERTY(set_clear)

    RootObject->Modify();

    FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(TargetContainer));
    const int32 PrevSize = Helper.Num();
    Helper.EmptyElements();

    NotifyReflectedPropertyChanged(RootObject, PropertyName, Property, EPropertyChangeType::ArrayClear);

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("objectPath"), ObjectPath);
    ResultPayload->SetStringField(TEXT("propertyName"), PropertyName);
    ResultPayload->SetNumberField(TEXT("previousSize"), PrevSize);
    ResultPayload->SetNumberField(TEXT("newSize"), 0);

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// asset.references
// ===========================================================================
REGISTER_RPC_HANDLER("asset.references", "asset", "Get assets this asset references (outbound dependencies / hard package dependencies). For the inverse (who references this asset) use asset.dependencies.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path to query. Both the /Game/Foo/Bar and /Game/Foo/Bar.Bar spellings resolve to the same package, exactly as asset.exists accepts them.")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ASSET"), TEXT("get_asset_references requires assetPath."));
        return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // This used to be GetAssetByObjectPath(FSoftObjectPath(AssetPath)), which
    // needs a FULL object path: the short "/Game/Foo/Bar" form parses to
    // {PackageName, AssetName=None} and matched a registry row only when the
    // package happened to already be loaded (GetAssetByObjectPath's FindObject
    // fast path returns the UPackage). So the same string asset.exists accepts
    // was rejected here for some assets and not others, with load state - not
    // anything the caller controls - deciding which. The shared resolver keys the
    // lookup on the package instead; see Utils/AssetUtils.h for the exact
    // accept/reject contract it applies, which is now identical across
    // asset.references, asset.dependencies, asset.get_dependencies and
    // asset.get_dependencies_classified.
    const FResolvedAssetPackage Resolved = ResolveAssetPathToPackage(AssetPath);
    if (!Resolved.bIsValid)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }

    TArray<FAssetIdentifier> Dependencies;
    AssetRegistry.GetDependencies(FAssetIdentifier(Resolved.PackageName), Dependencies);

    TArray<TSharedPtr<FJsonValue>> ReferencesArray;
    for (const FAssetIdentifier& Dep : Dependencies)
    {
        TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
        RefObj->SetStringField(TEXT("packageName"), Dep.PackageName.ToString());
        if (!Dep.ObjectName.IsNone())
        {
            RefObj->SetStringField(TEXT("objectName"), Dep.ObjectName.ToString());
        }
        ReferencesArray.Add(MakeShared<FJsonValueObject>(RefObj));
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ResultPayload->SetStringField(TEXT("packageName"), Resolved.PackageName.ToString());
    // Legacy keys (kept for wire compat) — these report the asset's OUTBOUND
    // dependencies (what this asset references), as the GetDependencies call above
    // produces. The legacy "references"/"referenceCount" names are direction-
    // misleading, so emit direction-true aliases alongside them.
    const int32 OutboundCount = ReferencesArray.Num();
    ResultPayload->SetNumberField(TEXT("referenceCount"), OutboundCount);
    ResultPayload->SetNumberField(TEXT("dependencyCount"), OutboundCount);
    ResultPayload->SetArrayField(TEXT("references"), ReferencesArray);
    // Direction-true alias is the last array write — move the now-dead local
    // rather than forcing a third copy of the element array.
    ResultPayload->SetArrayField(TEXT("dependencies"), MoveTemp(ReferencesArray));

    Ctx.SendSuccess(ResultPayload);
    return true;
}

// ===========================================================================
// asset.dependencies
// ===========================================================================
REGISTER_RPC_HANDLER("asset.dependencies", "asset", "Get assets that reference this asset (inbound referencers). For the inverse (what this asset references) use asset.references.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path to query. Both the /Game/Foo/Bar and /Game/Foo/Bar.Bar spellings resolve to the same package, exactly as asset.exists accepts them.")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ASSET"), TEXT("get_asset_dependencies requires assetPath."));
        return true;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // Same object-path-only lookup, same fix, as asset.references above - and it
    // matters more here, because this is the "who still uses this?" verb a caller
    // runs before deleting. See Utils/AssetUtils.h for the shared contract.
    const FResolvedAssetPackage Resolved = ResolveAssetPathToPackage(AssetPath);
    if (!Resolved.bIsValid)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), Resolved.ErrorMessage);
        return true;
    }

    TArray<FAssetIdentifier> Referencers;
    AssetRegistry.GetReferencers(FAssetIdentifier(Resolved.PackageName), Referencers);

    TArray<TSharedPtr<FJsonValue>> DependenciesArray;
    for (const FAssetIdentifier& Ref : Referencers)
    {
        TSharedPtr<FJsonObject> DepObj = MakeShared<FJsonObject>();
        DepObj->SetStringField(TEXT("packageName"), Ref.PackageName.ToString());
        if (!Ref.ObjectName.IsNone())
        {
            DepObj->SetStringField(TEXT("objectName"), Ref.ObjectName.ToString());
        }
        DependenciesArray.Add(MakeShared<FJsonValueObject>(DepObj));
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ResultPayload->SetStringField(TEXT("packageName"), Resolved.PackageName.ToString());
    // Legacy keys (kept for wire compat) — these report the asset's INBOUND
    // referencers (who references this asset), as the GetReferencers call above
    // produces. The legacy "dependencies"/"dependencyCount" names are direction-
    // misleading, so emit direction-true aliases alongside them.
    const int32 InboundCount = DependenciesArray.Num();
    ResultPayload->SetNumberField(TEXT("dependencyCount"), InboundCount);
    ResultPayload->SetNumberField(TEXT("referencerCount"), InboundCount);
    ResultPayload->SetArrayField(TEXT("dependencies"), DependenciesArray);
    // Direction-true alias is the last array write — move the now-dead local
    // rather than forcing a third copy of the element array.
    ResultPayload->SetArrayField(TEXT("referencers"), MoveTemp(DependenciesArray));

    Ctx.SendSuccess(ResultPayload);
    return true;
}
