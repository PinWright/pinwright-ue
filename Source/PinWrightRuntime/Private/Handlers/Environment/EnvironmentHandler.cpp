// Copyright (c) 2026 Alexander Penkin. MIT License.

// EnvironmentHandler.cpp (runtime half) - live-world environment.control.* setters and the
// read-only system.inspect.* object/actor/class inspectors. The level-authoring
// environment.build.* / spawn verbs and the editor-viewport / selection inspectors stay in
// the editor module's EnvironmentHandler.cpp.
#include "Compat/SceneComponentCompat.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Environment/EnvironmentDirtyUtils.h"
#include "Dispatch/PinWrightWorld.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "Utils/ActorUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/JsonBuilders.h"
#include "Utils/NameMatchFilter.h"
#include "Utils/PropertyExport.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"


// =====================================================================
// environment.control.* sub-actions
// =====================================================================

// ---- environment.control.set_time_of_day ----
REGISTER_RPC_HANDLER("environment.control.set_time_of_day", "environment.control", "Set time of day via directional light rotation",
    RPC_PARAMS(
        RPC_PARAM_REQ("hour", "number", "Hour of day (0-24)")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    double Hour = 0.0;
    const bool bHasHour = Payload->TryGetNumberField(TEXT("hour"), Hour);
    if (!bHasHour)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing hour parameter"));
        return true;
    }

    // Editor: the editor world (never PIE), as before. Game process: the live game world.
    UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
    if (!World)
    {
        Ctx.SendError(TEXT("WORLD_NOT_AVAILABLE"), TEXT("Editor world is unavailable"));
        return true;
    }

    ADirectionalLight* SunLight = nullptr;
    for (TActorIterator<ADirectionalLight> It(World); It; ++It)
    {
        if (ADirectionalLight* Light = *It)
        {
            if (IsValid(Light))
            {
                SunLight = Light;
                break;
            }
        }
    }
    if (!SunLight)
    {
        Ctx.SendError(TEXT("SUN_NOT_FOUND"), TEXT("No directional light found"));
        return true;
    }

    const float ClampedHour = FMath::Clamp(static_cast<float>(Hour), 0.0f, 24.0f);
    const float SolarPitch = (ClampedHour / 24.0f) * 360.0f - 90.0f;

    SunLight->Modify();
    FRotator NewRotation = SunLight->GetActorRotation();
    NewRotation.Pitch = SolarPitch;
    SunLight->SetActorRotation(NewRotation);

    if (UDirectionalLightComponent* LightComp =
            Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()))
    {
        LightComp->MarkRenderStateDirty();
    }

    // SetActorRotation stores the orientation as an FQuat, so an out-of-[-90,90]
    // SolarPitch (every afternoon/evening hour) is normalized back into canonical
    // Euler space — pitch re-expressed into [-90,90] with yaw/roll flipped 180 deg.
    // Echo the value the actor ACTUALLY holds after the set (read back via
    // GetActorRotation), not the pre-normalization SolarPitch the handler attempted,
    // so the response equals what actor.get_transform / property.get return for the
    // same actor and is usable as a self-verification.
    const FRotator AppliedRotation = SunLight->GetActorRotation();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("hour"), ClampedHour);
    Result->SetNumberField(TEXT("pitch"), AppliedRotation.Pitch);
    Result->SetNumberField(TEXT("yaw"), AppliedRotation.Yaw);
    Result->SetNumberField(TEXT("roll"), AppliedRotation.Roll);
    Result->SetStringField(TEXT("actor"), SunLight->GetPathName());
    AddActorVerification(Result, SunLight);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- environment.control.set_sun_intensity ----
REGISTER_RPC_HANDLER("environment.control.set_sun_intensity", "environment.control", "Set directional light (sun) intensity",
    RPC_PARAMS(
        RPC_PARAM_REQ("intensity", "number", "Sun intensity value")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    double Intensity = 0.0;
    if (!Payload->TryGetNumberField(TEXT("intensity"), Intensity))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing intensity parameter"));
        return true;
    }

    // Editor: the editor world (never PIE), as before. Game process: the live game world.
    UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
    if (!World)
    {
        Ctx.SendError(TEXT("WORLD_NOT_AVAILABLE"), TEXT("Editor world is unavailable"));
        return true;
    }

    ADirectionalLight* SunLight = nullptr;
    for (TActorIterator<ADirectionalLight> It(World); It; ++It)
    {
        if (ADirectionalLight* Light = *It)
        {
            if (IsValid(Light))
            {
                SunLight = Light;
                break;
            }
        }
    }
    if (!SunLight)
    {
        Ctx.SendError(TEXT("SUN_NOT_FOUND"), TEXT("No directional light found"));
        return true;
    }

    bool bStaticMobility = false;
    FString MobilityName = TEXT("Unknown");

    if (UDirectionalLightComponent* LightComp =
            Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()))
    {
        // Matches set_time_of_day, which Modify()s the actor before mutating.
        // MarkRenderStateDirty alone refreshes the render thread and leaves the package
        // clean, so the intensity was lost on close.
        PinWright::MarkLevelActorModified(SunLight, LightComp);

        bStaticMobility = LightComp->IsRegistered()
            && PinWrightGetMobility(*LightComp) == EComponentMobility::Static;
        MobilityName = StaticEnum<EComponentMobility::Type>()
            ->GetNameStringByValue(static_cast<int64>(PinWrightGetMobility(*LightComp)));

        LightComp->SetIntensity(static_cast<float>(Intensity));

        // ULightComponent::SetIntensity is gated on AreDynamicDataChangesAllowed()
        // (SceneComponent.h:1363-1366), which is FALSE for a registered Static-mobility
        // component — on a Static sun the setter above silently wrote nothing while this
        // verb still returned success with the new value echoed back. Assigning the
        // UPROPERTY directly persists the value on every mobility; on Movable/Stationary
        // it re-assigns what SetIntensity just wrote, so the engine fast path is kept and
        // behaviour is unchanged. Deliberately NOT SetMobility(Movable)-then-restore: that
        // reregisters the component twice and invalidates the level's baked lighting.
        LightComp->Intensity = static_cast<float>(Intensity);

        LightComp->MarkRenderStateDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("intensity"), Intensity);
    Result->SetStringField(TEXT("actor"), SunLight->GetPathName());
    // Additive: a Static light's intensity is baked into the level's lightmaps, so the
    // stored value now differs from what the viewport shows until lighting is rebuilt.
    Result->SetStringField(TEXT("mobility"), MobilityName);
    Result->SetBoolField(TEXT("requiresLightingRebuild"), bStaticMobility);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- environment.control.set_skylight_intensity ----
REGISTER_RPC_HANDLER("environment.control.set_skylight_intensity", "environment.control", "Set skylight intensity",
    RPC_PARAMS(
        RPC_PARAM_REQ("intensity", "number", "Skylight intensity value")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    double Intensity = 0.0;
    if (!Payload->TryGetNumberField(TEXT("intensity"), Intensity))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing intensity parameter"));
        return true;
    }

    // Editor: the editor world (never PIE), as before. Game process: the live game world.
    UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
    if (!World)
    {
        Ctx.SendError(TEXT("WORLD_NOT_AVAILABLE"), TEXT("Editor world is unavailable"));
        return true;
    }

    ASkyLight* SkyActor = nullptr;
    for (TActorIterator<ASkyLight> It(World); It; ++It)
    {
        if (ASkyLight* Sky = *It)
        {
            if (IsValid(Sky))
            {
                SkyActor = Sky;
                break;
            }
        }
    }
    if (!SkyActor)
    {
        Ctx.SendError(TEXT("SKYLIGHT_NOT_FOUND"), TEXT("No skylight found"));
        return true;
    }

    bool bStaticMobility = false;
    FString MobilityName = TEXT("Unknown");

    if (USkyLightComponent* SkyComp = SkyActor->GetLightComponent())
    {
        PinWright::MarkLevelActorModified(SkyActor, SkyComp);

        bStaticMobility = SkyComp->IsRegistered()
            && PinWrightGetMobility(*SkyComp) == EComponentMobility::Static;
        MobilityName = StaticEnum<EComponentMobility::Type>()
            ->GetNameStringByValue(static_cast<int64>(PinWrightGetMobility(*SkyComp)));

        SkyComp->SetIntensity(static_cast<float>(Intensity));

        // USkyLightComponent::SetIntensity (SkyLightComponent.cpp:971-980) is gated on
        // AreDynamicDataChangesAllowed(); see set_sun_intensity above for the full
        // rationale. Note the gate's bIgnoreStationary default is true, so a default
        // skylight (Stationary, SkyLightComponent.cpp:312) is unaffected — only an
        // author-set Static one was silently dropped.
        SkyComp->Intensity = static_cast<float>(Intensity);

        SkyComp->MarkRenderStateDirty();
        SkyActor->MarkComponentsRenderStateDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("intensity"), Intensity);
    Result->SetStringField(TEXT("actor"), SkyActor->GetPathName());
    Result->SetStringField(TEXT("mobility"), MobilityName);
    Result->SetBoolField(TEXT("requiresLightingRebuild"), bStaticMobility);
    Ctx.SendSuccess(Result);
    return true;
}

// =====================================================================
// system.inspect.* sub-actions
// =====================================================================

// ---- system.inspect.list_objects ----
REGISTER_RPC_HANDLER("system.inspect.list_objects", "system.inspect", "Enumerate AActors in the resolved world (PIE-first in 'auto') as {name, label, path, class} entries — name is the internal object name (GetName), label is the editor display label. Read-only counterpart to actor.list (note actor.* twins put the label in `name`; here `name` stays the internal key). On any populated level the full list exceeds the inline display budget and spills to a file; narrow it inline with filter= (matched against internal name AND class name, case-INSENSITIVE substring by default — pair with matchMode/caseSensitive for anchored or case-exact counting), limit= (max rows; totalMatches keeps reporting the untruncated total, truncated flips true when rows were elided), or namesOnly=true / fields=[...] to drop the per-row path.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Pattern matched against each actor's internal name AND class name; an actor is kept if either matches. NOT matched against the display label — use actor.list for that. Default semantics are a case-INSENSITIVE SUBSTRING match, so filter:\"SH_\" also matches \"Brush_0\" (the lowercase sh_ inside it) — pass matchMode:'prefix' and/or caseSensitive:true to count by naming-convention prefix. Omit to return all actors."),
        NameMatch::MatchModeParam(TEXT("filter")),
        NameMatch::CaseSensitiveParam(TEXT("filter")),
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto"),
        RPC_PARAM_DEF("limit", "number", "Max objects to return after filtering. 0 (default) = all. totalMatches always reports the full untruncated match count so elision is detectable.", "0"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("fields"), TEXT("array|string"), TEXT("Case-insensitive allow-list of per-object keys to return. Valid keys: label, name, path, class — the same four returned when fields is omitted; e.g. [\"name\",\"class\"] to drop the verbose path. Any other key is REJECTED by name with INVALID_PARAMS naming the valid set; it is never silently dropped, because a projection made only of unrecognised keys would return rows that are empty objects. This verb projects only its own four columns: for the Outliner folder use actor.list with fields=[\"folder\"], and for properties, transform or components use system.inspect.inspect_object. A single string is also accepted, under this key or the singular 'field'."), /*bRequired=*/false, TArray<FString>{TEXT("fields"), TEXT("field")}),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("namesOnly"), TEXT("bool"), TEXT("When true, returns only label+name+class per object (drops the longest per-row field, path) — shorthand for the common 'just list what's here so I can pick' read. Snake_case names_only accepted. Ignored when fields is supplied."), /*bRequired=*/false, TArray<FString>{TEXT("namesOnly"), TEXT("names_only")})
    ))
{
    // Same filter contract as its actor.list twin, from the same shared parser: the
    // default stays case-insensitive substring (byte-identical to the prior behaviour)
    // and matchMode/caseSensitive are the opt-ins. See Utils/NameMatchFilter.h.
    NameMatch::FFilter Filter;
    if (!NameMatch::Require(Ctx, TArray<FString>{TEXT("filter")}, Filter))
    {
        return true;
    }

    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

    // Per-row field projection: an explicit fields allow-list (array or bare
    // string) wins; otherwise namesOnly drops the verbose path. Default keeps all
    // three keys so the unprojected output is byte-identical to the prior shape.
    // Shared with actor.list / system.console.search via
    // FHandlerContext::ReadFieldProjection; the namesOnly column set (everything
    // except the verbose path) is the argument.
    const TSet<FString> Fields = Ctx.ReadFieldProjection({TEXT("label"), TEXT("name"), TEXT("class")});

    // Every key the row builder below can actually emit, lowercase to match the
    // lowercased set ReadFieldProjection returns. Nothing is added here beyond the
    // four columns this verb already had: the per-actor keys a caller reaches for
    // next (folder, transform, properties, components) are each already projectable
    // on a sibling that is a better fit — actor.list carries folder, and
    // system.inspect.inspect_object carries the rest — so widening the rows of a
    // verb that already spills on any populated level would buy nothing.
    static const TArray<FString> EmittableFields = {
        TEXT("label"), TEXT("name"), TEXT("path"), TEXT("class")};

    // A key this handler cannot emit is refused by name, never dropped. Dropping it
    // silently switched projection ON (any entry raises Fields.Num()) while matching
    // no column, so fields:["folder"] answered every matched actor with an empty {}
    // row — indistinguishable from "this actor has no such data" — and
    // fields:["name","folder"] quietly returned one column of the two asked for.
    // This is the courtesy the dispatcher's UNKNOWN_PARAMS gate already extends to
    // top-level keys; that gate only ever sees parameter names, never the values
    // inside an array.
    TArray<FString> UnknownFields;
    for (const FString& Field : Fields)
    {
        if (!EmittableFields.Contains(Field))
        {
            UnknownFields.Add(Field);
        }
    }
    if (UnknownFields.Num() > 0)
    {
        UnknownFields.Sort();
        // Deliberately a raw literal rather than the registry constant: this file spells
        // all 40 of its codes by hand and is not on the registry's grandfathered baseline,
        // so one constant reference would flip it to "adopting" and fail
        // PinWright.core.error_codes.RegistryAdoptingFilesUseConstantsOnly. (That test
        // scans source text without stripping comments, so naming the constant here -
        // even to explain avoiding it - is itself enough to trip it. Board ticket
        // B-error-code-adoption-test-scans-comments.)
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(
                TEXT("Unknown fields[] entry(s) for 'system.inspect.list_objects': [%s]. ")
                TEXT("Valid fields: [%s]. Rejected rather than dropped: a projection made ")
                TEXT("only of unrecognised keys returns rows that are empty objects. For ")
                TEXT("the Outliner folder use actor.list with fields=[\"folder\"]; for ")
                TEXT("properties, transform or components use ")
                TEXT("system.inspect.inspect_object."),
                *FString::Join(UnknownFields, TEXT(", ")),
                *FString::Join(EmittableFields, TEXT(", "))));
        return true;
    }

    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key)
    {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantLabel = Wants(TEXT("label"));
    const bool bWantName = Wants(TEXT("name"));
    const bool bWantPath = Wants(TEXT("path"));
    const bool bWantClass = Wants(TEXT("class"));

    // limit truncates the returned array after filtering; 0 = all. totalMatches
    // below always reports the full filtered count regardless of the cap.
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

    TArray<TSharedPtr<FJsonValue>> ObjectsArray;
    int32 TotalMatched = 0;
    if (World)
    {
        const bool bFiltering = Filter.IsActive();
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            // Materialize name/class only when the filter or projection consumes
            // them; the filter still matches on both. The display label is additive
            // (it is NOT a filter input — filter parity with the prior shape is
            // preserved), so it is materialized at emit time below, not here.
            const FString Name = (bFiltering || bWantName) ? Actor->GetName() : FString();
            const FString ClassName = (bFiltering || bWantClass)
                ? Actor->GetClass()->GetName() : FString();
            if (bFiltering && !Filter.MatchesEither(Name, ClassName))
            {
                continue;
            }

            ++TotalMatched;
            if (Limit > 0 && ObjectsArray.Num() >= Limit)
            {
                continue; // keep counting TotalMatched, but stop appending rows
            }

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            if (bWantName)
            {
                Obj->SetStringField(TEXT("name"), Name);
            }
            if (bWantLabel)
            {
                // Display label alongside the internal `name` (the actor.list twin
                // emits both the same way); `name` stays the collision-safe key.
                // Materialized here (after the limit guard) so over-limit rows that
                // are counted but not emitted never pay for GetActorLabel().
                Obj->SetStringField(TEXT("label"), ActorNameParamUtils::ActorLabelOrName(Actor));
            }
            if (bWantPath)
            {
                Obj->SetStringField(TEXT("path"), Actor->GetPathName());
            }
            if (bWantClass)
            {
                Obj->SetStringField(TEXT("class"), ClassName);
            }
            ObjectsArray.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("objects"), ObjectsArray);
    // count = rows returned (unchanged semantics); totalMatches = full untruncated
    // match count so a caller can tell the list was capped by limit. totalMatches +
    // truncated is the shared filter+limit vocabulary of the sibling readers.
    Resp->SetNumberField(TEXT("count"), ObjectsArray.Num());
    Resp->SetNumberField(TEXT("totalMatches"), TotalMatched);
    Resp->SetBoolField(TEXT("truncated"), ObjectsArray.Num() < TotalMatched);
    Resp->SetStringField(TEXT("world"), ResolvedMode);
    Resp->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
    // Echoes filter + the resolved matchMode/caseSensitive (only when filtering, so an
    // unfiltered response keeps its exact prior shape).
    NameMatch::AddFilterEcho(Resp, Filter, TEXT("filter"));
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}

// Build the read-only inspect row shared by find_by_class / find_by_tag.
// name = the collision-safe internal object name (GetName); label = the editor
// display label, surfaced alongside (not in place of) name so a human-facing audit
// gets both without re-calling the write twin. The actor.* twins put the label in
// `name`; here `name` stays internal and `label` carries the display string.
static TSharedPtr<FJsonObject> MakeInspectActorRow(AActor* Actor)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Actor->GetName());
    Obj->SetStringField(TEXT("label"), ActorNameParamUtils::ActorLabelOrName(Actor));
    Obj->SetStringField(TEXT("path"), Actor->GetPathName());
    Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    return Obj;
}

// Build the read-only inspect row for an arbitrary (possibly non-actor) UObject —
// emitted by system.inspect.find_objects_by_class. Unlike the actor row there is no
// editor display label; `path` is the full object path that object.call_function and
// system.inspect.inspect_object consume, and `outer` surfaces the owning object's path
// (empty for top-level objects) so a widget-owned helper's ownership chain is visible.
static TSharedPtr<FJsonObject> MakeInspectUObjectRow(UObject* Object)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Object->GetName());
    Obj->SetStringField(TEXT("path"), Object->GetPathName());
    Obj->SetStringField(TEXT("class"), Object->GetClass()->GetName());
    if (UObject* Outer = Object->GetOuter())
    {
        Obj->SetStringField(TEXT("outer"), Outer->GetPathName());
    }
    return Obj;
}

// ---- system.inspect.find_by_class ----
REGISTER_RPC_HANDLER("system.inspect.find_by_class", "system.inspect", "Find every actor in the resolved world (PIE-first in 'auto') whose class name (or path) matches the query. Match is case-insensitive on the leaf class name and substring on the full path. Each row carries name (internal object name, GetName), label (editor display label), path, and class. Read-only counterpart to actor.find_by_class (which puts the label in `name`; here `name` stays the internal key).",
    RPC_PARAMS(
        RPC_PARAM_REQ("className", "classref", "Class name (e.g. 'StaticMeshActor') or path fragment to search for."),
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    FString ClassName;
    Payload->TryGetStringField(TEXT("className"), ClassName);

    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

    TArray<TSharedPtr<FJsonValue>> ObjectsArray;
    if (World && !ClassName.IsEmpty())
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->GetClass()->GetName().Equals(ClassName, ESearchCase::IgnoreCase) ||
                Actor->GetClass()->GetPathName().Contains(ClassName))
            {
                ObjectsArray.Add(MakeShared<FJsonValueObject>(MakeInspectActorRow(Actor)));
            }
        }
    }
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("objects"), ObjectsArray);
    Resp->SetNumberField(TEXT("count"), ObjectsArray.Num());
    Resp->SetStringField(TEXT("world"), ResolvedMode);
    Resp->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- system.inspect.find_objects_by_class ----
// The non-actor-capable analog of find_by_class. list_objects / find_by_class /
// find_by_tag all iterate TActorIterator<AActor>, so a live NON-actor UObject (a
// UMG-widget-owned helper, a component, a subsystem, a plain UObject) is invisible to
// MCP — leaving no way to obtain the object path that object.call_function needs. This
// verb enumerates live instances of a class via GetObjectsOfClass (the global object
// hash), completing the discovery -> invocation chain for non-actors.
REGISTER_RPC_HANDLER("system.inspect.find_objects_by_class", "system.inspect",
    "Enumerate live UObject INSTANCES of a class across the whole object graph — including "
    "non-actors (UMG-widget-owned helpers, components, subsystems, plain UObjects) that the "
    "actor-only list_objects/find_by_class/find_by_tag can never reach. Each row is {name, "
    "path, class, outer}; `path` is the reference object.call_function / inspect_object "
    "consume, so this is the missing discovery half for invoking a UFUNCTION on a non-actor. "
    "Class-default (CDO) and archetype objects are excluded unless includeDefaults=true, and "
    "garbage/unreachable objects are always skipped. GetObjectsOfClass spans all worlds so "
    "results can be large: the default limit=100 caps returned rows (totalMatches reports the "
    "untruncated count, truncated flips true when rows were elided); narrow with filter=, "
    "includeDerived=false (exact class only), or world scoping.",
    RPC_PARAMS(
        RPC_PARAM_REQ("className", "classref", "Class whose live instances to enumerate. Accepts a short native name (e.g. 'UserWidget'), a full script path ('/Script/UMG.UserWidget'), or a content class/Blueprint path. Resolved via the shared class resolver; an unresolvable class returns CLASS_NOT_FOUND."),
        RPC_PARAM_DEF("world", "string", "World scope: 'any' (default — every world plus transient/asset objects, the widest discovery net), or 'editor'/'pie'/'auto' to keep only instances owned by (GetWorld, or outer-chain IsIn) that world.", "any"),
        RPC_PARAM_OPT("filter", "string", "Pattern matched against each instance's name, class name, and full path; an instance is kept if any of the three matches. Default semantics are a case-INSENSITIVE SUBSTRING match — pass matchMode:'prefix' and/or caseSensitive:true to count by naming-convention prefix. Omit to return all instances of the class."),
        NameMatch::MatchModeParam(TEXT("filter")),
        NameMatch::CaseSensitiveParam(TEXT("filter")),
        RPC_PARAM_DEF("limit", "number", "Max rows to return after filtering. Default 100 (object counts are large); 0 = all. totalMatches always reports the full untruncated match count so elision is detectable via the truncated flag.", "100"),
        RPC_PARAM_DEF("includeDerived", "bool", "When true (default), instances of subclasses of className are included too; false restricts to the exact class.", "true"),
        RPC_PARAM_OPT("includeDefaults", "bool", "When true, class-default objects (CDOs) and archetypes are included. Default false — they are noise for a live-instance survey and a footgun for object.call_function, which refuses CDO targets.")
    ))
{
    FString ClassNameOrPath;
    if (!Ctx.RequireString(TEXT("className"), ClassNameOrPath))
    {
        return true;
    }

    UClass* TargetClass = ResolveClassByName(ClassNameOrPath);
    if (!TargetClass)
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Could not resolve a UClass from '%s'. Pass a short native name, a /Script/Module.Class path, or a content class/Blueprint path."),
                *ClassNameOrPath));
        return true;
    }

    // Same shared filter contract as list_objects / actor.list; default unchanged.
    NameMatch::FFilter Filter;
    if (!NameMatch::Require(Ctx, TArray<FString>{TEXT("filter")}, Filter))
    {
        return true;
    }
    const bool bFiltering = Filter.IsActive();
    const int32 Limit = Ctx.GetInt(TEXT("limit"), 100);
    const bool bIncludeDerived = Ctx.GetBool(TEXT("includeDerived"), true);
    const bool bIncludeDefaults = Ctx.GetBool(TEXT("includeDefaults"), false);

    // 'any' (the default) skips world scoping — GetObjectsOfClass already spans every
    // world; only resolve a concrete world when the caller asks to narrow to one.
    const FString WorldArg = Ctx.GetString(TEXT("world"), TEXT("any"));
    const bool bScopeWorld = !WorldArg.IsEmpty() && !WorldArg.Equals(TEXT("any"), ESearchCase::IgnoreCase);
    FString ResolvedMode = TEXT("any");
    UWorld* ScopeWorld = bScopeWorld
        ? McpActorUtils::ResolveQueryWorld(WorldArg, ResolvedMode) : nullptr;

    // GetObjectsOfClass excludes the CDO by default (RF_ClassDefaultObject); keep that
    // exclusion unless the caller opts in. Archetypes and garbage are filtered below.
    TArray<UObject*> Candidates;
    const EObjectFlags ExclusionFlags = bIncludeDefaults ? RF_NoFlags : RF_ClassDefaultObject;
    GetObjectsOfClass(TargetClass, Candidates, bIncludeDerived, ExclusionFlags);

    TArray<TSharedPtr<FJsonValue>> ObjectsArray;
    int32 TotalMatched = 0;
    for (UObject* Object : Candidates)
    {
        // Never surface null / garbage / unreachable objects — a dangling path is
        // useless to the caller and unsafe to hand to object.call_function.
        if (!IsValid(Object))
        {
            continue;
        }
        if (!bIncludeDefaults && Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
        {
            continue;
        }
        if (bScopeWorld)
        {
            // A world scope was requested but none resolved -> nothing is in scope.
            if (!ScopeWorld)
            {
                continue;
            }
            const UWorld* ObjectWorld = Object->GetWorld();
            if (ObjectWorld != ScopeWorld && !Object->IsIn(ScopeWorld))
            {
                continue;
            }
        }

        const FString Name = Object->GetName();
        const FString ClassName = Object->GetClass()->GetName();
        // Kept as a short-circuit chain rather than a MatchesAnyOf(...) call so the
        // expensive GetPathName() is still only built when name and class both miss.
        if (bFiltering
            && !Filter.Matches(Name)
            && !Filter.Matches(ClassName)
            && !Filter.Matches(Object->GetPathName()))
        {
            continue;
        }

        ++TotalMatched;
        if (Limit > 0 && ObjectsArray.Num() >= Limit)
        {
            continue; // keep counting TotalMatched, but stop appending rows
        }
        ObjectsArray.Add(MakeShared<FJsonValueObject>(MakeInspectUObjectRow(Object)));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("objects"), ObjectsArray);
    Resp->SetNumberField(TEXT("count"), ObjectsArray.Num());
    Resp->SetNumberField(TEXT("totalMatches"), TotalMatched);
    Resp->SetBoolField(TEXT("truncated"), ObjectsArray.Num() < TotalMatched);
    Resp->SetStringField(TEXT("class"), TargetClass->GetPathName());
    Resp->SetStringField(TEXT("world"), ResolvedMode);
    if (bScopeWorld)
    {
        Resp->SetStringField(TEXT("worldPath"), ScopeWorld ? ScopeWorld->GetPathName() : FString());
    }
    NameMatch::AddFilterEcho(Resp, Filter, TEXT("filter"));
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- system.inspect.find_by_tag ----
REGISTER_RPC_HANDLER("system.inspect.find_by_tag", "system.inspect", "Find every actor in the resolved world (PIE-first in 'auto') that has the given Tags entry. Each row carries name (internal object name, GetName), label (editor display label), path, and class. Read-only counterpart to actor.find_by_tag (which puts the label in `name`; here `name` stays the internal key).",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Actor tag (FName) to look up in AActor::Tags."),
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    FString Tag;
    Payload->TryGetStringField(TEXT("tag"), Tag);

    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

    TArray<TSharedPtr<FJsonValue>> ObjectsArray;
    if (World && !Tag.IsEmpty())
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->ActorHasTag(FName(*Tag)))
            {
                ObjectsArray.Add(MakeShared<FJsonValueObject>(MakeInspectActorRow(Actor)));
            }
        }
    }
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("objects"), ObjectsArray);
    Resp->SetNumberField(TEXT("count"), ObjectsArray.Num());
    Resp->SetStringField(TEXT("world"), ResolvedMode);
    Resp->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
    return true;
}

// Shared census emitter for the sibling system.inspect.list_actor_* handlers
// (list_actor_tags, list_actor_classes). This is the single copy of the census
// wire contract those two must mirror: the (count desc, key asc) sort, the
// limit>0 row cap, and the {<ArrayField>, count, distinctCount, truncated, world,
// worldPath, success} envelope. count = rows actually returned; distinctCount =
// the full untruncated distinct-key total so a limit-elided list is detectable
// (truncated flips true when rows < distinctCount). distinctCount is named that,
// not list_objects' totalMatches, on purpose: a census has no query to "match", so
// the total is a distinct-key count and a caller cannot blind-copy that reader.
// Keys stay FName through the tally (allocation-free on the per-actor hot path) and
// are stringified exactly once per distinct row that is actually returned.
static void SendCountCensus(
    FHandlerContext& Ctx,
    TArray<TPair<FName, int32>>& Counts,
    const TCHAR* ArrayField,
    const TCHAR* RowKeyField,
    int32 Limit,
    const FString& ResolvedMode,
    UWorld* World)
{
    // Stable, useful ordering: most-common keys first, ties broken by key name so
    // the output is deterministic for a given world.
    Counts.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B)
    {
        if (A.Value != B.Value)
        {
            return A.Value > B.Value;
        }
        return A.Key.Compare(B.Key) < 0;
    });

    const int32 DistinctCount = Counts.Num();

    TArray<TSharedPtr<FJsonValue>> RowsArray;
    for (const TPair<FName, int32>& Pair : Counts)
    {
        if (Limit > 0 && RowsArray.Num() >= Limit)
        {
            break;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(RowKeyField, Pair.Key.ToString());
        Row->SetNumberField(TEXT("count"), Pair.Value);
        RowsArray.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(ArrayField, RowsArray);
    Resp->SetNumberField(TEXT("count"), RowsArray.Num());
    Resp->SetNumberField(TEXT("distinctCount"), DistinctCount);
    Resp->SetBoolField(TEXT("truncated"), RowsArray.Num() < DistinctCount);
    Resp->SetStringField(TEXT("world"), ResolvedMode);
    Resp->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
}

// ---- system.inspect.list_actor_tags ----
// Census counterpart to system.inspect.find_by_tag: find_by_tag needs a tag you
// already know, so a "what is tagged for review/cleanup?" audit could otherwise
// only guess tag names (silently missing unguessed ones) or scan every actor. This
// unions AActor::Tags across the resolved world into distinct {tag, count} rows
// (count = number of actors carrying the tag), mirroring the read-only
// system.inspect.list_subsystems "no accessor exists for this value space" census.
REGISTER_RPC_HANDLER("system.inspect.list_actor_tags", "system.inspect", "Enumerate the distinct AActor::Tags in use across the resolved world (PIE-first in 'auto') as {tag, count} rows, count = how many actors carry that tag. This is the census find_by_tag can't give you: use it to discover which tags exist before querying one (e.g. a 'is anything tagged for review/cleanup?' audit that would otherwise have to guess tag names or scan every actor). Rows are sorted by count descending, then tag name ascending. distinctCount always reports the full untruncated number of distinct tags; pass limit= to cap the returned rows (truncated flips true when rows were elided).",
    RPC_PARAMS(
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto"),
        RPC_PARAM_DEF("limit", "number", "Max tag rows to return (sorted by count desc). 0 (default) = all. distinctCount always reports the full untruncated distinct-tag count so elision is detectable.", "0")
    ))
{
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

    // Union each actor's Tags into a per-tag actor count. Dedupe within a single
    // actor (a duplicate Tags entry on one actor must not inflate the census) and
    // skip NAME_None (an empty/placeholder Tags slot is not a real tag). SeenOnActor
    // is hoisted above the loop and Reset() (not reconstructed) each iteration so its
    // backing storage is reused across the whole scan instead of being reallocated
    // per tagged actor on the census hot path.
    TMap<FName, int32> TagCounts;
    if (World)
    {
        TSet<FName> SeenOnActor;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            // Append dedupes this actor's Tags into the set; drop the NAME_None
            // placeholder so it is never counted as a real tag.
            SeenOnActor.Reset();
            SeenOnActor.Append(Actor->Tags);
            SeenOnActor.Remove(NAME_None);
            for (const FName& Tag : SeenOnActor)
            {
                TagCounts.FindOrAdd(Tag)++;
            }
        }
    }

    // Emit the census envelope through the shared SendCountCensus helper (the single
    // copy of the sort + limit + distinctCount/truncated elision contract this and
    // list_actor_classes must mirror). Keys stay FName until the helper stringifies
    // the distinct rows it actually returns.
    TArray<TPair<FName, int32>> SortedTags = TagCounts.Array();
    SendCountCensus(Ctx, SortedTags, TEXT("tags"), TEXT("tag"),
        Ctx.GetInt(TEXT("limit"), 0), ResolvedMode, World);
    return true;
}

// ---- system.inspect.list_actor_classes ----
// Scene-composition census: find_by_class needs a class you already know, so a
// "what is this level made of?" survey could otherwise only dump every list_objects
// row (which spills on a populated level) and tally counts-by-class client-side.
// This unions actor classes
// across the resolved world into distinct {class, count} rows (count = number of
// actors of that class), the class counterpart to system.inspect.list_actor_tags and
// mirroring its "no aggregate reader for a value space" census shape and elision
// contract.
REGISTER_RPC_HANDLER("system.inspect.list_actor_classes", "system.inspect", "Enumerate the distinct actor classes present in the resolved world (PIE-first in 'auto') as {class, count} rows, count = how many actors are of that class — the 'what is this level made of?' scene-composition census. This is the per-class breakdown list_objects (per-actor rows that spill on a populated level) can't give you directly: use it to survey a level's makeup in one inline call instead of dumping every actor row and tallying counts-by-class client-side. class is the leaf class name (GetClass()->GetName()), the same key list_objects / find_by_class report, so a row feeds straight back into find_by_class. Rows are sorted by count descending, then class name ascending. distinctCount always reports the full untruncated number of distinct classes; pass limit= to cap the returned rows (truncated flips true when rows were elided).",
    RPC_PARAMS(
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto"),
        RPC_PARAM_DEF("limit", "number", "Max class rows to return (sorted by count desc). 0 (default) = all. distinctCount always reports the full untruncated distinct-class count so elision is detectable.", "0")
    ))
{
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

    // Tally each actor by its leaf class in a single pass over the resolved world.
    // Keyed by the allocation-free FName (GetFName()) rather than GetName()'s FString
    // so the per-actor census hot path costs no heap allocation; the shared helper
    // stringifies the leaf name (GetName() is GetFName().ToString(), so grouping and
    // the emitted strings are unchanged) exactly once per distinct class in the rows
    // it returns. That leaf name is the same key list_objects / find_by_class report,
    // so a census row feeds straight back into find_by_class to enumerate the actors
    // behind a count.
    TMap<FName, int32> ClassCounts;
    if (World)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            ClassCounts.FindOrAdd(Actor->GetClass()->GetFName())++;
        }
    }

    // Emit the census envelope through the shared SendCountCensus helper (the single
    // copy of the sort + limit + distinctCount/truncated elision contract this and
    // list_actor_tags must mirror).
    TArray<TPair<FName, int32>> SortedClasses = ClassCounts.Array();
    SendCountCensus(Ctx, SortedClasses, TEXT("classes"), TEXT("class"),
        Ctx.GetInt(TEXT("limit"), 0), ResolvedMode, World);
    return true;
}

// ---- system.inspect.inspect_class ----
REGISTER_RPC_HANDLER("system.inspect.inspect_class", "system.inspect", "Resolve a UClass by short name, U/A-prefixed name, /Script/ path, or /Game/ Blueprint path and return its name, full path, parent class, inheritanceChain, properties, functions, and interfaces. Errors CLASS_NOT_FOUND if no match.",
    RPC_PARAMS(
        RPC_PARAM_REQ("className", "classref", "Class name (e.g. 'StaticMeshActor', 'AStaticMeshActor') or path (e.g. '/Game/MyBP.MyBP_C').")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    FString ClassName;
    Payload->TryGetStringField(TEXT("className"), ClassName);
    if (ClassName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("className is required for inspect_class"));
        return true;
    }

    // Unified resolution: short names, U/A-prefix, /Script/ paths, and /Game/ BP classes.
    UClass* TargetClass = ResolveUClass(ClassName);
    if (TargetClass)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("className"), TargetClass->GetName());
        Resp->SetStringField(TEXT("classPath"), TargetClass->GetPathName());
        Resp->SetStringField(TEXT("parentClass"),
            TargetClass->GetSuperClass() ? TargetClass->GetSuperClass()->GetName() : TEXT("None"));

        // Iterate the authoritative (non-REINST/non-skeleton) class so Blueprint `_C`
        // resolutions surface the latest member set.
        UClass* AuthClass = TargetClass->GetAuthoritativeClass();
        if (!AuthClass)
        {
            AuthClass = TargetClass;
        }

        TArray<TSharedPtr<FJsonValue>> Functions;
        for (TFieldIterator<UFunction> FuncIt(AuthClass); FuncIt; ++FuncIt)
        {
            UFunction* Func = *FuncIt;
            if (!Func || Func->HasAnyFunctionFlags(FUNC_Delegate))
            {
                continue;
            }
            Functions.Add(MakeShared<FJsonValueObject>(FunctionToInspectJson(Func)));
        }
        Resp->SetArrayField(TEXT("functions"), Functions);

        TArray<TSharedPtr<FJsonValue>> Properties;
        for (TFieldIterator<FProperty> PropIt(AuthClass); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop)
            {
                continue;
            }
            Properties.Add(MakeShared<FJsonValueObject>(PropertyToInspectJson(Prop)));
        }
        Resp->SetArrayField(TEXT("properties"), Properties);

        TArray<TSharedPtr<FJsonValue>> Interfaces;
        for (const FImplementedInterface& Iface : AuthClass->Interfaces)
        {
            if (Iface.Class)
            {
                Interfaces.Add(MakeShared<FJsonValueString>(Iface.Class->GetName()));
            }
        }
        Resp->SetArrayField(TEXT("interfaces"), Interfaces);

        TArray<TSharedPtr<FJsonValue>> Chain;
        for (UClass* SC = AuthClass; SC; SC = SC->GetSuperClass())
        {
            Chain.Add(MakeShared<FJsonValueString>(SC->GetName()));
        }
        Resp->SetArrayField(TEXT("inheritanceChain"), Chain);

        Resp->SetBoolField(TEXT("success"), true);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Class not found: %s"), *ClassName));
    }
    return true;
}

// ---- system.inspect (object inspection - requires objectPath) ----
REGISTER_RPC_HANDLER("system.inspect.inspect_object", "system.inspect", "Inspect a UObject (typically an actor) by path or display name and report its properties, transform, components, and class. The general-purpose read-only inspector — broader than blueprint.inspect, narrower than actor.get.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Object path (preferred) or actor display label to inspect.")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("Payload missing"));
        return true;
    }

    FString ObjectPath;
    if (!Payload->TryGetStringField(TEXT("objectPath"), ObjectPath) || ObjectPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("objectPath required"));
        return true;
    }

    // Find the object
    UObject* TargetObject = nullptr;
    TargetObject = FindObject<UObject>(nullptr, *ObjectPath);

    // If not found, try to find actor by name/label
    if (!TargetObject)
    {
        UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
        if (World)
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                AActor* Actor = *It;
                if (Actor && (ActorNameParamUtils::ActorLabelOrName(Actor).Equals(ObjectPath, ESearchCase::IgnoreCase) ||
                              Actor->GetName().Equals(ObjectPath, ESearchCase::IgnoreCase)))
                {
                    TargetObject = Actor;
                    break;
                }
            }
        }
    }

    if (!TargetObject)
    {
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"),
            FString::Printf(TEXT("Object not found: %s"), *ObjectPath));
        return true;
    }

    // Build inspection result
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();

    // Basic object info
    Resp->SetStringField(TEXT("objectPath"), TargetObject->GetPathName());
    Resp->SetStringField(TEXT("objectName"), TargetObject->GetName());
    const FString ClassShortName = TargetObject->GetClass()->GetName();
    // The leaf class short-name is emitted under both `class` and `className`. `class` is the
    // canonical key shared with this method's own nested components[] and every sibling
    // enumerator (list_objects / find_by_class / find_by_tag); `className` is retained as a
    // back-compat alias so parsers keyed on either spelling keep working — a caller who
    // chained the audit and learned `class` would otherwise silently miss it here (board
    // E-inspect-object-class-key-drift).
    Resp->SetStringField(TEXT("class"), ClassShortName);
    Resp->SetStringField(TEXT("className"), ClassShortName);
    Resp->SetStringField(TEXT("classPath"), TargetObject->GetClass()->GetPathName());

    // Reflected property dump for every target type (fulfills the doc's "properties"
    // promise). Passing nullptr as the parent CDO emits all properties rather than a
    // CDO-vs-parent diff. Transient properties are kept (flagged `Transient`): on a live
    // object they are usually the runtime state being inspected. Whatever the dump filter
    // still drops (deprecated, editor-regenerated) is named in omittedProperties, never
    // silently absent (board B-inspect-object-omits-transient-props).
    const TSharedPtr<FJsonObject> Properties = BuildClassPropertyJson(TargetObject, nullptr, /*bIncludeNonPersistent=*/true);
    TArray<TSharedPtr<FJsonValue>> Omitted;
    for (TFieldIterator<FProperty> It(TargetObject->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        if (!Properties->HasField(It->GetName()))
        {
            Omitted.Add(MakeShared<FJsonValueString>(It->GetName()));
        }
    }
    Resp->SetObjectField(TEXT("properties"), Properties);
    Resp->SetArrayField(TEXT("omittedProperties"), Omitted);

    // If it's an actor, add actor-specific info
    if (AActor* Actor = Cast<AActor>(TargetObject))
    {
        Resp->SetStringField(TEXT("actorLabel"), ActorNameParamUtils::ActorLabelOrName(Actor));
        Resp->SetBoolField(TEXT("isActor"), true);
        Resp->SetBoolField(TEXT("isHidden"), Actor->IsHidden());
        Resp->SetBoolField(TEXT("isSelected"), Actor->IsSelected());

        // Transform info
        TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
        const FTransform& Transform = Actor->GetActorTransform();

        TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
        LocationObj->SetNumberField(TEXT("x"), Transform.GetLocation().X);
        LocationObj->SetNumberField(TEXT("y"), Transform.GetLocation().Y);
        LocationObj->SetNumberField(TEXT("z"), Transform.GetLocation().Z);
        TransformObj->SetObjectField(TEXT("location"), LocationObj);

        TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
        FRotator Rotator = Transform.GetRotation().Rotator();
        RotationObj->SetNumberField(TEXT("pitch"), Rotator.Pitch);
        RotationObj->SetNumberField(TEXT("yaw"), Rotator.Yaw);
        RotationObj->SetNumberField(TEXT("roll"), Rotator.Roll);
        TransformObj->SetObjectField(TEXT("rotation"), RotationObj);

        TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
        ScaleObj->SetNumberField(TEXT("x"), Transform.GetScale3D().X);
        ScaleObj->SetNumberField(TEXT("y"), Transform.GetScale3D().Y);
        ScaleObj->SetNumberField(TEXT("z"), Transform.GetScale3D().Z);
        TransformObj->SetObjectField(TEXT("scale"), ScaleObj);

        Resp->SetObjectField(TEXT("transform"), TransformObj);

        // Components info
        TArray<TSharedPtr<FJsonValue>> ComponentsArray;
        TInlineComponentArray<UActorComponent*> Components;
        Actor->GetComponents(Components);

        for (UActorComponent* Component : Components)
        {
            if (Component)
            {
                TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
                CompObj->SetStringField(TEXT("name"), Component->GetName());
                CompObj->SetStringField(TEXT("class"), Component->GetClass()->GetName());
                CompObj->SetBoolField(TEXT("isActive"), Component->IsActive());

                if (USceneComponent* SceneComp = Cast<USceneComponent>(Component))
                {
                    CompObj->SetBoolField(TEXT("isSceneComponent"), true);
                    CompObj->SetBoolField(TEXT("isVisible"), SceneComp->IsVisible());
                }

                if (UStaticMeshComponent* MeshComp = Cast<UStaticMeshComponent>(Component))
                {
                    CompObj->SetBoolField(TEXT("isStaticMesh"), true);
                    if (MeshComp->GetStaticMesh())
                    {
                        CompObj->SetStringField(TEXT("staticMesh"), MeshComp->GetStaticMesh()->GetName());
                    }
                }

                ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
            }
        }
        Resp->SetArrayField(TEXT("components"), ComponentsArray);
        Resp->SetNumberField(TEXT("componentCount"), ComponentsArray.Num());
    }
    else
    {
        Resp->SetBoolField(TEXT("isActor"), false);

        // Scene components carry a transform even though they are not actors.
        if (USceneComponent* SceneComp = Cast<USceneComponent>(TargetObject))
        {
            Resp->SetObjectField(TEXT("transform"),
                JsonBuilders::BuildTransformJson(SceneComp->GetComponentTransform()));
        }
    }

    // Tags (only for AActor-derived objects)
    TArray<TSharedPtr<FJsonValue>> TagsArray;
    if (AActor* AsActor = Cast<AActor>(TargetObject))
    {
        for (const FName& Tag : AsActor->Tags)
        {
            TagsArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
    }
    Resp->SetArrayField(TEXT("tags"), TagsArray);

    Ctx.SendSuccess(Resp);
    return true;
}
