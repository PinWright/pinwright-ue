// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared helpers for environment "spawn actor + apply component properties"
// handlers (sky atmosphere, volumetric cloud, reflection capture). The three
// handlers diverge only in component type and the optional `shape` field; the
// location/rotation parse and the properties→apply loop are identical.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PinWrightHelpers.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

// Parse optional `location` / `rotation` payload objects into UE math types.
// Missing fields default to zero. Returns true if both keys were either parsed
// or omitted cleanly; the call sites currently treat malformed input as zero
// rather than erroring, so the bool is informational only.
inline bool ReadLocationRotationFromPayload(const FJsonObject& Payload,
                                            FVector& OutLocation,
                                            FRotator& OutRotation)
{
    OutLocation = FVector::ZeroVector;
    const TSharedPtr<FJsonObject>* LocPtr;
    if (Payload.TryGetObjectField(TEXT("location"), LocPtr))
    {
        OutLocation.X = GetJsonNumberField((*LocPtr), TEXT("x"));
        OutLocation.Y = GetJsonNumberField((*LocPtr), TEXT("y"));
        OutLocation.Z = GetJsonNumberField((*LocPtr), TEXT("z"));
    }

    OutRotation = FRotator::ZeroRotator;
    const TSharedPtr<FJsonObject>* RotPtr;
    if (Payload.TryGetObjectField(TEXT("rotation"), RotPtr))
    {
        OutRotation.Pitch = GetJsonNumberField((*RotPtr), TEXT("pitch"));
        OutRotation.Yaw = GetJsonNumberField((*RotPtr), TEXT("yaw"));
        OutRotation.Roll = GetJsonNumberField((*RotPtr), TEXT("roll"));
    }

    return true;
}

// Walk a free-form `properties` JSON object and apply each key to the matching
// UPROPERTY on `Comp`, appending `{name, reason}` objects to `OutRejected` for
// unknown or unparseable keys. Returns the count of successfully-applied
// properties so the caller can decide whether to MarkRenderStateDirty.
template <typename TComponent>
inline int32 ApplyPropertiesJsonToComponent(TComponent* Comp,
                                            const TSharedPtr<FJsonObject>& PropsObject,
                                            TArray<TSharedPtr<FJsonValue>>& OutRejected)
{
    if (!Comp || !PropsObject.IsValid())
    {
        return 0;
    }

    int32 Applied = 0;
    for (const auto& Pair : PropsObject->Values)
    {
        FProperty* Prop = FindPropertyCI(Comp->GetClass(), EARGCompat::JsonKeyToString(Pair.Key));
        if (!Prop)
        {
            TSharedPtr<FJsonObject> Reject = MakeShared<FJsonObject>();
            Reject->SetStringField(TEXT("name"), EARGCompat::JsonKeyToString(Pair.Key));
            Reject->SetStringField(TEXT("reason"), TEXT("unknown_property"));
            OutRejected.Add(MakeShared<FJsonValueObject>(Reject));
            continue;
        }
        FString ApplyError;
        if (!ApplyJsonValueToProperty(Comp, Prop, Pair.Value, ApplyError))
        {
            TSharedPtr<FJsonObject> Reject = MakeShared<FJsonObject>();
            Reject->SetStringField(TEXT("name"), EARGCompat::JsonKeyToString(Pair.Key));
            Reject->SetStringField(TEXT("reason"), ApplyError);
            OutRejected.Add(MakeShared<FJsonValueObject>(Reject));
            continue;
        }
        ++Applied;
    }
    return Applied;
}
