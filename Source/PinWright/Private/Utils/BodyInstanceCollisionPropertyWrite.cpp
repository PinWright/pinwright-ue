// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract, the measured defect and every engine line cited here: the header.

#include "Utils/BodyInstanceCollisionPropertyWrite.h"

#include "Utils/PropertyImport.h"
#include "PinWrightSubsystem.h" // LogPinWrightSubsystem

#include "Components/ActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/EngineTypes.h"
#include "Compat/EngineVersionCompat.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace PinWright
{
namespace
{
    // Prefixed names: the plugin builds with Unity on, so a file-local helper must not
    // collide with a sibling TU's anonymous namespace.

    // All four collision fields are declared `private` on FBodyInstance (BodyInstance.h:377,
    // :392, :568, :586), so GET_MEMBER_NAME_CHECKED cannot name them from outside. The
    // reflected name is the stable identity anyway - it is what the importer resolves to
    // when it recurses into the struct. Same reason as ShapeExtentPropertyWrite.cpp.
    const FName& PinWrightBodyInstanceFieldName()
    {
        static const FName Name(TEXT("BodyInstance"));
        return Name;
    }

    const FName& PinWrightCollisionEnabledFieldName()
    {
        static const FName Name(TEXT("CollisionEnabled"));
        return Name;
    }

    const FName& PinWrightCollisionProfileFieldName()
    {
        static const FName Name(TEXT("CollisionProfileName"));
        return Name;
    }

    const FName& PinWrightCollisionObjectTypeFieldName()
    {
        static const FName Name(TEXT("ObjectType"));
        return Name;
    }

    const FName& PinWrightCollisionResponsesFieldName()
    {
        static const FName Name(TEXT("CollisionResponses"));
        return Name;
    }

    // Same vocabulary CollisionSummaryUtils publishes, so the two never describe the same
    // component differently.
    FString PinWrightBodyCollisionEnabledName(ECollisionEnabled::Type Value)
    {
        switch (Value)
        {
        case ECollisionEnabled::NoCollision:     return TEXT("NoCollision");
        case ECollisionEnabled::QueryOnly:       return TEXT("QueryOnly");
        case ECollisionEnabled::PhysicsOnly:     return TEXT("PhysicsOnly");
        case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
        default:                                 return TEXT("Unknown");
        }
    }

    FString PinWrightBodyCollisionResponseName(ECollisionResponse Value)
    {
        if (const UEnum* Enum = StaticEnum<ECollisionResponse>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Value));
        }
        return FString::FromInt(static_cast<int32>(Value));
    }

    FString PinWrightBodyCollisionChannelName(ECollisionChannel Channel)
    {
        if (const UEnum* Enum = StaticEnum<ECollisionChannel>())
        {
            const FString Name = Enum->GetNameStringByValue(static_cast<int64>(Channel));
            if (!Name.IsEmpty())
            {
                return Name;
            }
        }
        return FString::Printf(TEXT("Channel%d"), static_cast<int32>(Channel));
    }

    // The four reflected collision sub-properties, resolved once per call. A missing or
    // reshaped field aborts the whole routing rather than half-applying it: the caller then
    // falls through to the existing raw-store path, so the worst outcome of a future engine
    // change is the old stale-body bug rather than a reinterpreted pointer.
    struct FPinWrightBodyCollisionFields
    {
        FByteProperty* Enabled = nullptr;
        FByteProperty* ObjectType = nullptr;
        FNameProperty* ProfileName = nullptr;
        FStructProperty* Responses = nullptr;

        bool IsComplete() const
        {
            return Enabled && ObjectType && ProfileName && Responses;
        }
    };

    FPinWrightBodyCollisionFields PinWrightResolveBodyCollisionFields()
    {
        FPinWrightBodyCollisionFields Fields;
        UScriptStruct* BodyStruct = FBodyInstance::StaticStruct();
        if (!BodyStruct)
        {
            return Fields;
        }
        Fields.Enabled = CastField<FByteProperty>(
            BodyStruct->FindPropertyByName(PinWrightCollisionEnabledFieldName()));
        Fields.ObjectType = CastField<FByteProperty>(
            BodyStruct->FindPropertyByName(PinWrightCollisionObjectTypeFieldName()));
        Fields.ProfileName = CastField<FNameProperty>(
            BodyStruct->FindPropertyByName(PinWrightCollisionProfileFieldName()));
        Fields.Responses = CastField<FStructProperty>(
            BodyStruct->FindPropertyByName(PinWrightCollisionResponsesFieldName()));
        if (Fields.Responses && Fields.Responses->Struct != FCollisionResponse::StaticStruct())
        {
            Fields.Responses = nullptr;
        }
        return Fields;
    }

    // The four values, as they stand in one FBodyInstance right now. Read through the
    // reflected pointers rather than the public accessors so snapshot and restore are
    // symmetric: GetCollisionProfileName() resolves an external profile body setup, which is
    // NOT the stored field, and restoring the resolved value would silently rewrite it.
    struct FPinWrightBodyCollisionValues
    {
        uint8 Enabled = 0;
        uint8 ObjectType = 0;
        FName ProfileName;
        FCollisionResponse Responses;
    };

    FPinWrightBodyCollisionValues PinWrightReadBodyCollisionValues(
        const FPinWrightBodyCollisionFields& Fields, FBodyInstance& Body)
    {
        // Explicit void* so the container overload is the struct-memory one rather than a
        // UObject* form; FBodyInstance is not a UObject and the properties come from its
        // UScriptStruct.
        void* const Container = static_cast<void*>(&Body);
        FPinWrightBodyCollisionValues Values;
        Values.Enabled = *Fields.Enabled->ContainerPtrToValuePtr<uint8>(Container);
        Values.ObjectType = *Fields.ObjectType->ContainerPtrToValuePtr<uint8>(Container);
        Values.ProfileName = *Fields.ProfileName->ContainerPtrToValuePtr<FName>(Container);
        Values.Responses =
            *Fields.Responses->ContainerPtrToValuePtr<FCollisionResponse>(Container);
        return Values;
    }

    void PinWrightWriteBodyCollisionValues(const FPinWrightBodyCollisionFields& Fields,
                                           FBodyInstance& Body,
                                           const FPinWrightBodyCollisionValues& Values)
    {
        void* const Container = static_cast<void*>(&Body);
        *Fields.Enabled->ContainerPtrToValuePtr<uint8>(Container) = Values.Enabled;
        *Fields.ObjectType->ContainerPtrToValuePtr<uint8>(Container) = Values.ObjectType;
        *Fields.ProfileName->ContainerPtrToValuePtr<FName>(Container) = Values.ProfileName;
        *Fields.Responses->ContainerPtrToValuePtr<FCollisionResponse>(Container) =
            Values.Responses;
    }

    // Channels whose response the request changed. Only these are reported: publishing all
    // 64 would bury the one the caller asked about, and a channel nobody wrote is not
    // evidence of anything.
    //
    // The bound is ECC_OverlapAll_Deprecated, NOT ECC_MAX. FCollisionResponseContainer holds
    // exactly 64 bytes (EngineTypes.h:1745, `uint8 EnumArray[64]`) covering ECC_WorldStatic
    // through ECC_GameTraceChannel50, and GetResponse indexes that array with no bound check
    // (:1762) - so ECC_MAX (two past the end) reads off the end of the struct. The header
    // marks the boundary in prose ("Add new serializeable channels above here"), and the
    // engine's own valid-channel predicate is `(x) < ECC_OverlapAll_Deprecated`
    // (Private/Collision/CollisionProfile.cpp:14).
    TArray<ECollisionChannel> PinWrightChangedResponseChannels(
        const FCollisionResponseContainer& Before, const FCollisionResponseContainer& After)
    {
        TArray<ECollisionChannel> Changed;
        for (int32 Index = 0; Index < static_cast<int32>(ECC_OverlapAll_Deprecated); ++Index)
        {
            const ECollisionChannel Channel = static_cast<ECollisionChannel>(Index);
            if (Before.GetResponse(Channel) != After.GetResponse(Channel))
            {
                Changed.Add(Channel);
            }
        }
        return Changed;
    }

    // One requested/measured row, before any body has been read.
    FBodyInstanceCollisionField PinWrightMakeCollisionFieldRow(const FString& Name,
                                                               const FString& Requested)
    {
        FBodyInstanceCollisionField Row;
        Row.Name = Name;
        Row.Requested = Requested;
        return Row;
    }

    // Does this request name a collision field at all? Decided BEFORE anything is imported,
    // so a BodyInstance write that only carries mass, damping or a sim flag keeps the exact
    // path it has today - one reflection store followed by PostEditChangeProperty, reported
    // in `notified`. Interception is not free: it moves the property from `notified` to
    // `applied` and skips the change hook, which is right for a field a typed setter
    // supersedes and wrong for one it does not.
    //
    // Only the object form is inspectable. A BodyInstance handed over as an ExportText string
    // falls through to the raw store with the pre-fix behaviour; that is stated in
    // docs/wiki-src/actor.md rather than guessed at by parsing the string here.
    bool PinWrightRequestNamesCollisionField(const TSharedPtr<FJsonValue>& ValueField)
    {
        if (!ValueField.IsValid() || ValueField->Type != EJson::Object)
        {
            return false;
        }
        const TSharedPtr<FJsonObject>& Object = ValueField->AsObject();
        if (!Object.IsValid())
        {
            return false;
        }
        const FName CollisionFieldNames[] = {
            PinWrightCollisionEnabledFieldName(), PinWrightCollisionProfileFieldName(),
            PinWrightCollisionObjectTypeFieldName(), PinWrightCollisionResponsesFieldName()};
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Object->Values)
        {
            for (const FName& FieldName : CollisionFieldNames)
            {
                if (Pair.Key.Equals(FieldName.ToString(), ESearchCase::IgnoreCase))
                {
                    return true;
                }
            }
        }
        return false;
    }
}

EBodyInstanceCollisionWrite ApplyBodyInstanceCollisionProperty(
    UActorComponent* Component,
    FProperty* Property,
    const TSharedPtr<FJsonValue>& ValueField,
    FBodyInstanceCollisionMeasurement& OutMeasurement,
    FString& OutError)
{
    UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
    if (!Primitive || !Property)
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }
    if (Property->GetFName() != PinWrightBodyInstanceFieldName())
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }
    const FStructProperty* BodyProperty = CastField<FStructProperty>(Property);
    if (!BodyProperty || BodyProperty->Struct != FBodyInstance::StaticStruct())
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }
    if (!PinWrightRequestNamesCollisionField(ValueField))
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }

    const FPinWrightBodyCollisionFields Fields = PinWrightResolveBodyCollisionFields();
    if (!Fields.IsComplete())
    {
        // A reshaped FBodyInstance: fall through to the raw store rather than guess.
        return EBodyInstanceCollisionWrite::NotApplicable;
    }

    FBodyInstance& Template = Primitive->BodyInstance;
    const FPinWrightBodyCollisionValues Before =
        PinWrightReadBodyCollisionValues(Fields, Template);

    // The shared importer does the whole struct, so every non-collision sub-field (mass,
    // damping, bNotifyRigidBodyCollision, ...) keeps exactly the behaviour it has today and
    // every accepted syntax and error string stays identical to a generic property write.
    if (!ApplyJsonValueToProperty(Component, Property, ValueField, OutError))
    {
        // Partial application is the importer's existing behaviour on a nested-object write;
        // the collision fields are restored so a rejected request cannot leave the template
        // describing a filter nothing ever pushed down.
        PinWrightWriteBodyCollisionValues(Fields, Template, Before);
        return EBodyInstanceCollisionWrite::Failed;
    }

    const FPinWrightBodyCollisionValues After =
        PinWrightReadBodyCollisionValues(Fields, Template);
    PinWrightWriteBodyCollisionValues(Fields, Template, Before);

    const bool bProfileChanged = After.ProfileName != Before.ProfileName;
    const bool bObjectTypeChanged = After.ObjectType != Before.ObjectType;
    const bool bEnabledChanged = After.Enabled != Before.Enabled;
    const TArray<ECollisionChannel> ChangedChannels = PinWrightChangedResponseChannels(
        Before.Responses.GetResponseContainer(), After.Responses.GetResponseContainer());

    if (!bProfileChanged && !bObjectTypeChanged && !bEnabledChanged && ChangedChannels.Num() == 0)
    {
        // A BodyInstance write that touched no collision field at all - the importer already
        // stored everything it named, and there is nothing to route or to measure.
        return EBodyInstanceCollisionWrite::Applied;
    }

    OutMeasurement.bCollisionWritten = true;
    OutMeasurement.ComponentName = Primitive->GetName();

    // bUseDefaultCollision makes a static-mesh component re-read its ENTIRE collision setup
    // from the mesh asset at every registration: UStaticMeshComponent::OnRegister ->
    // UpdateCollisionFromStaticMesh -> FBodyInstance::UseExternalCollisionProfile ->
    // LoadProfileData (StaticMeshComponent.cpp:812-826, :2175-2189; BodyInstance.cpp:835-841),
    // which resolves the profile off the mesh's UBodySetup and overwrites CollisionEnabled,
    // ObjectType and the response container. On a Blueprint component TEMPLATE that is every
    // spawned instance, and on a placed component every re-registration - so an explicit
    // collision write on such a component is discarded no matter how it was applied. The
    // engine clears the flag from UStaticMeshComponent::SetCollisionProfileName (:2618-2622)
    // but from NONE of the other three setters, so this has to be done here; the details
    // panel does exactly the same thing before any explicit collision edit
    // (FBodyInstanceCustomization::MarkAllBodiesDefaultCollision, BodyInstanceCustomization.cpp
    // :816-843, called from OnCollisionProfileChanged :855-892 and SetToDefaultProfile :937-945).
    // Cleared BEFORE the setters run because SetCollisionProfileName's early-out compares
    // against GetCollisionProfileName(), which resolves through the external body setup while
    // the flag's profile is still installed. USplineMeshComponent inherits both the flag and
    // this fix.
    if (UStaticMeshComponent* StaticMeshPrimitive = Cast<UStaticMeshComponent>(Primitive))
    {
        StaticMeshPrimitive->bUseDefaultCollision = false;
    }

    // Engine order. The profile goes first because SetCollisionProfileName loads the
    // profile's responses, object type and CollisionEnabled over whatever is there; an
    // explicitly requested response or enabled value is then replayed on top, so naming both
    // in one call means the explicit value wins rather than the profile's.
    if (bProfileChanged)
    {
        Primitive->SetCollisionProfileName(After.ProfileName);
        OutMeasurement.Fields.Add(PinWrightMakeCollisionFieldRow(
            PinWrightCollisionProfileFieldName().ToString(), After.ProfileName.ToString()));
    }
    if (bObjectTypeChanged)
    {
        const ECollisionChannel NewObjectType = static_cast<ECollisionChannel>(After.ObjectType);
        Primitive->SetCollisionObjectType(NewObjectType);
        OutMeasurement.Fields.Add(PinWrightMakeCollisionFieldRow(
            PinWrightCollisionObjectTypeFieldName().ToString(),
            PinWrightBodyCollisionChannelName(NewObjectType)));
    }
    if (ChangedChannels.Num() > 0)
    {
        Primitive->SetCollisionResponseToChannels(After.Responses.GetResponseContainer());
        for (const ECollisionChannel Channel : ChangedChannels)
        {
            OutMeasurement.Fields.Add(PinWrightMakeCollisionFieldRow(
                PinWrightBodyCollisionChannelName(Channel),
                PinWrightBodyCollisionResponseName(
                    After.Responses.GetResponseContainer().GetResponse(Channel))));
        }
    }
    if (bEnabledChanged)
    {
        const ECollisionEnabled::Type NewEnabled =
            static_cast<ECollisionEnabled::Type>(After.Enabled);
        // SetCollisionEnabled is the one setter that also creates or destroys the physics
        // state (EnsurePhysicsStateCreated, PrimitiveComponentPhysics.cpp:1384), which on an
        // instanced component rebuilds every instance body from the template. That rebuild is
        // the engine's own answer for this field and supersedes the propagation below.
        Primitive->SetCollisionEnabled(NewEnabled);
        OutMeasurement.Fields.Add(PinWrightMakeCollisionFieldRow(
            PinWrightCollisionEnabledFieldName().ToString(),
            PinWrightBodyCollisionEnabledName(NewEnabled)));
    }

    UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Primitive);
    if (!Instanced)
    {
        // A plain primitive collides through the component's own body, which the setters
        // above have already refiltered. Nothing to push down and nothing to measure that
        // the setters do not already guarantee.
        return EBodyInstanceCollisionWrite::Applied;
    }

    OutMeasurement.bInstancedComponent = true;

    // CopyRuntimeBodyInstancePropertiesFrom asserts the template has no deferred profile
    // pending. That flag is only ever set from a constructor (SetCollisionProfileNameDeferred,
    // PrimitiveComponentPhysics.cpp:1406) and cleared in PostInitProperties, so it is already
    // false here - applying it is a no-op that costs nothing and removes the assert as a
    // failure mode.
    Template.ApplyDeferredCollisionProfileName();

    const ECollisionChannel TemplateObjectType =
        static_cast<ECollisionChannel>(After.ObjectType);
    // UE 5.8 moved the per-instance body array behind GetInstanceBodies() and deprecated the
    // public member; older engines only expose the member.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    const TArray<FBodyInstance*>& InstanceBodies = Instanced->GetInstanceBodies();
#else
    const TArray<FBodyInstance*>& InstanceBodies = Instanced->InstanceBodies;
#endif
    for (FBodyInstance* Body : InstanceBodies)
    {
        if (!Body)
        {
            // A zero-scale instance gets a null body slot (InstancedMeshComponentBodies.cpp:97).
            continue;
        }
        ++OutMeasurement.BodyCount;
        if (bObjectTypeChanged)
        {
            // The one collision field CopyRuntimeBodyInstancePropertiesFrom does not carry.
            Body->SetObjectType(TemplateObjectType);
        }
        Body->CopyRuntimeBodyInstancePropertiesFrom(&Template);
        ++OutMeasurement.BodiesRefreshed;
    }

    if (OutMeasurement.BodyCount == 0)
    {
        OutMeasurement.NotInspectableReason = InstanceBodies.Num() == 0
            ? (Instanced->IsPhysicsStateCreated()
                   ? TEXT("the component has no instances")
                   : TEXT("the component has no physics state, so no per-instance bodies exist"))
            : TEXT("every per-instance body slot is null (zero-scale instances)");
        return EBodyInstanceCollisionWrite::Applied;
    }

    // THE READ-BACK, taken off the bodies the physics scene consults rather than off the
    // template the write landed on. GetCollisionEnabled(false) deliberately skips the owner
    // check: what is being measured is the value propagation put on the body, not the
    // actor-level override the engine applies on top of it.
    OutMeasurement.bBodiesInspectable = true;
    const FString EnabledRowName = PinWrightCollisionEnabledFieldName().ToString();
    const FString ProfileRowName = PinWrightCollisionProfileFieldName().ToString();
    const FString ObjectTypeRowName = PinWrightCollisionObjectTypeFieldName().ToString();
    for (FBodyInstanceCollisionField& Row : OutMeasurement.Fields)
    {
        // Which read this row needs is a property of the row, not of the body, so it is
        // resolved once rather than per body - these arrays run to thousands of entries on a
        // real scatter.
        const bool bEnabledRow = Row.Name == EnabledRowName;
        const bool bProfileRow = Row.Name == ProfileRowName;
        const bool bObjectTypeRow = Row.Name == ObjectTypeRowName;
        ECollisionChannel ResponseChannel = ECC_MAX;
        if (!bEnabledRow && !bProfileRow && !bObjectTypeRow)
        {
            for (const ECollisionChannel Channel : ChangedChannels)
            {
                if (PinWrightBodyCollisionChannelName(Channel) == Row.Name)
                {
                    ResponseChannel = Channel;
                    break;
                }
            }
            if (ResponseChannel == ECC_MAX)
            {
                continue;
            }
        }

        bool bFirst = true;
        for (FBodyInstance* Body : InstanceBodies)
        {
            if (!Body)
            {
                continue;
            }
            FString BodyValue;
            if (bEnabledRow)
            {
                BodyValue = PinWrightBodyCollisionEnabledName(Body->GetCollisionEnabled(false));
            }
            else if (bProfileRow)
            {
                BodyValue = Body->GetCollisionProfileName().ToString();
            }
            else if (bObjectTypeRow)
            {
                BodyValue = PinWrightBodyCollisionChannelName(Body->GetObjectType());
            }
            else
            {
                BodyValue = PinWrightBodyCollisionResponseName(
                    Body->GetResponseToChannel(ResponseChannel));
            }

            if (bFirst)
            {
                Row.Measured = BodyValue;
                Row.bMeasured = true;
                bFirst = false;
            }
            else if (Row.Measured != BodyValue)
            {
                Row.bUnanimous = false;
            }
        }
    }

    return EBodyInstanceCollisionWrite::Applied;
}

EBodyInstanceCollisionWrite ApplyBodyInstanceCollisionPropertyPath(
    UActorComponent* Component,
    const FString& PropertyPath,
    const TSharedPtr<FJsonValue>& ValueField,
    FBodyInstanceCollisionMeasurement& OutMeasurement,
    FString& OutError)
{
    if (!Component || !ValueField.IsValid())
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }

    TArray<FString> Segments;
    PropertyPath.ParseIntoArray(Segments, TEXT("."));
    if (Segments.Num() == 0 ||
        !Segments[0].Equals(PinWrightBodyInstanceFieldName().ToString(), ESearchCase::IgnoreCase))
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }

    FProperty* BodyProperty = Component->GetClass()
        ? Component->GetClass()->FindPropertyByName(PinWrightBodyInstanceFieldName())
        : nullptr;
    if (!BodyProperty)
    {
        return EBodyInstanceCollisionWrite::NotApplicable;
    }

    // Fold the tail back into the nested-object shape the whole-struct routine speaks:
    // "BodyInstance.CollisionResponses.ResponseToChannels" + value becomes
    // {"CollisionResponses": {"ResponseToChannels": value}}. Built inside-out so any depth
    // works, and so the collision-field test and the importer both see exactly the request
    // actor.set_component_properties would have produced for the same edit.
    TSharedPtr<FJsonValue> StructValue = ValueField;
    for (int32 Index = Segments.Num() - 1; Index >= 1; --Index)
    {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetField(Segments[Index], StructValue);
        StructValue = MakeShared<FJsonValueObject>(Wrapper);
    }

    return ApplyBodyInstanceCollisionProperty(Component, BodyProperty, StructValue,
                                              OutMeasurement, OutError);
}

void AddBodyInstanceCollisionReport(
    const FBodyInstanceCollisionMeasurement& Measurement,
    const TSharedPtr<FJsonObject>& Data,
    TArray<FString>& OutWarnings)
{
    if (!Measurement.bCollisionWritten || !Measurement.bInstancedComponent || !Data.IsValid())
    {
        return;
    }

    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    Block->SetNumberField(TEXT("count"), Measurement.BodyCount);
    Block->SetNumberField(TEXT("refreshed"), Measurement.BodiesRefreshed);

    TSharedPtr<FJsonObject> Requested = MakeShared<FJsonObject>();
    for (const FBodyInstanceCollisionField& Row : Measurement.Fields)
    {
        Requested->SetStringField(Row.Name, Row.Requested);
    }
    Block->SetObjectField(TEXT("requested"), Requested);

    if (Measurement.bBodiesInspectable)
    {
        TSharedPtr<FJsonObject> Measured = MakeShared<FJsonObject>();
        for (const FBodyInstanceCollisionField& Row : Measurement.Fields)
        {
            if (Row.bMeasured)
            {
                Measured->SetStringField(Row.Name, Row.Measured);
            }
        }
        Block->SetObjectField(TEXT("measured"), Measured);
    }
    // else: `measured` is omitted rather than filled from the template, which is exactly the
    // read that made the original defect look like a success.

    Data->SetObjectField(TEXT("instanceBodies"), Block);

    if (!Measurement.bBodiesInspectable)
    {
        const FString Warning = FString::Printf(
            TEXT("Collision write on instanced component '%s': the per-instance bodies could not be ")
            TEXT("measured (%s), so the value reported by the component is the template only and ")
            TEXT("says nothing about what the physics scene will use."),
            *Measurement.ComponentName,
            Measurement.NotInspectableReason.IsEmpty() ? TEXT("reason unknown")
                                                       : *Measurement.NotInspectableReason);
        OutWarnings.Add(Warning);
        UE_LOG(LogPinWrightSubsystem, Warning, TEXT("%s"), *Warning);
        return;
    }

    for (const FBodyInstanceCollisionField& Row : Measurement.Fields)
    {
        if (!Row.bMeasured)
        {
            continue;
        }
        if (Row.Measured == Row.Requested && Row.bUnanimous)
        {
            continue;
        }
        const FString Warning = FString::Printf(
            TEXT("Collision write on instanced component '%s': %s requested '%s' but the %d ")
            TEXT("per-instance bodies measure '%s'%s. The per-instance bodies are what the ")
            TEXT("physics scene consults."),
            *Measurement.ComponentName, *Row.Name, *Row.Requested, Measurement.BodyCount,
            *Row.Measured, Row.bUnanimous ? TEXT("") : TEXT(" (and disagree with each other)"));
        OutWarnings.Add(Warning);
        UE_LOG(LogPinWrightSubsystem, Warning, TEXT("%s"), *Warning);
    }
}
}
