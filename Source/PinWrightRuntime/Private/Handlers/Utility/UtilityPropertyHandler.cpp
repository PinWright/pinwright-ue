// Copyright (c) 2026 Alexander Penkin. MIT License.

// property.get, property.list, container.array.get, container.map.get/has_key/get_keys,
// container.set.contains: the game-safe half of the editor module's UtilityPropertyHandler.cpp,
// which keeps the mutators (property.set/reset, container.* writes) and asset.references /
// asset.dependencies. Also implements the resolution helpers both halves share
// (Handlers/Utility/PropertyHandlerShared.h).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Utility/PropertyHandlerShared.h"
#include "PinWrightRuntimeSubsystem.h"
#include "Utils/AssetResolveUtils.h"
#include "Utils/PropertyUtils.h"
#include "Utils/JsonUtils.h"

#include "Dom/JsonObject.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

using namespace PropertyHandlerShared;

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
UObject* PropertyHandlerShared::ResolveObjectForProperty(FHandlerContext& Ctx, const FString& ObjectPath)
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

    // Class path (`…/BP_Foo.BP_Foo_C`, `/Script/Engine.Actor`) → CDO too. The UClass
    // itself holds no member values, so callers asking for "the class default object"
    // via a class path got PROPERTY_NOT_FOUND (E-property-cdo-path-trap-docs).
    if (UClass* AsClass = Cast<UClass>(Resolved))
    {
        return AsClass->GetDefaultObject();
    }

    return Resolved;
}

// Resolve a property from an object, handling both nested and simple paths.
// Thin FHandlerContext wrapper over the shared ResolvePropertyOnObject dispatch
// primitive (Utils/PropertyInspection.h): it does the dotted-vs-simple branch,
// and this wrapper only maps a null return to the PROPERTY_NOT_FOUND error.
// Returns true on success, false on failure (error already sent).
bool PropertyHandlerShared::ResolvePropertyFromObject(FHandlerContext& Ctx, UObject* RootObject,
    const FString& PropertyName, const FString& ObjectPath,
    void*& OutContainer, FProperty*& OutProperty,
    FPropertyNotifyTarget* OutNotifyTarget)
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

// Add verification based on object type
void PropertyHandlerShared::AddObjectVerification(FHandlerContext& Ctx, TSharedPtr<FJsonObject>& Result, UObject* Obj)
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

bool PropertyHandlerShared::TryParseContainerInt32(const TSharedPtr<FJsonValue>& Value,
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

namespace
{
    PropertyHandlerShared::FDumpHintProvider GDumpHintProvider = nullptr;

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

    struct FDefaultPropertyLookupCache
    {
        TMap<FName, FProperty*> PropertiesByName;
    };
}

void PropertyHandlerShared::SetDumpHintProvider(FDumpHintProvider Provider)
{
    GDumpHintProvider = Provider;
}

PropertyHandlerShared::FDumpHintProvider PropertyHandlerShared::GetDumpHintProvider()
{
    return GDumpHintProvider;
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

#if WITH_EDITOR
    // UInheritableComponentHandler::FindKey/FindBestArchetype are editor-only; a game process
    // falls through to the archetype check below.
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
#endif

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

FResolvedDefaultProperty PropertyHandlerShared::ResolveDefaultPropertyFromObject(
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
// property.get
// ===========================================================================
REGISTER_RPC_HANDLER("property.get", "property", "Read a UPROPERTY value from a UObject by name and return it as JSON. Read-side counterpart to property.set.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path or actor name. A Blueprint asset path (/Game/X/BP_Foo), its generated-class path (BP_Foo.BP_Foo_C) and BP_Foo.Default__BP_Foo_C all target the class default object; so does a native class path (/Script/Engine.Actor)"),
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
        RPC_PARAM_REQ("objectPath", "path", "Object path or actor name. A Blueprint asset path (/Game/X/BP_Foo), its generated-class path (BP_Foo.BP_Foo_C) and BP_Foo.Default__BP_Foo_C all target the class default object; so does a native class path (/Script/Engine.Actor)"),
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
            const FDumpHintProvider DumpHintProvider = GetDumpHintProvider();
            if (DumpHintProvider && Pkg != GetTransientPackage() && !PkgName.StartsWith(TEXT("/Script")) && !PkgName.StartsWith(TEXT("/Engine/Transient")))
            {
                if (FString DumpHint = DumpHintProvider(PkgName, Pkg->ContainsMap()); !DumpHint.IsEmpty())
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
