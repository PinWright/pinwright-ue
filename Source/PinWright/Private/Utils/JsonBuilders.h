// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/Class.h"

class AActor;
class UObject;

// Shared JSON-builder helpers used across handler/.cpp files. Centralized here
// to avoid ODR collisions when these helpers previously lived in anonymous
// namespaces of multiple .cpp files that could share a unity-build translation
// unit. All helpers are file-internal in intent (no module export).
namespace JsonBuilders
{
    // Tiny, hot helpers are inlined in the header. Their bodies match the
    // canonical definitions previously duplicated across the consumer files.

    inline TSharedPtr<FJsonObject> MakeObject()
    {
        return MakeShared<FJsonObject>();
    }

    // Non-finite doubles (NaN, +/-Inf) have no JSON literal: UE's number writer
    // emits a bare `nan`/`inf` token, which makes the ENTIRE response unparseable —
    // a strict JSON client (e.g. Python's json.loads) rejects it and mis-reports a
    // live editor as unreachable. Collapse any non-finite component to 0 so a
    // degenerate value (e.g. the extent of an empty/inverted FBox, which overflows
    // to -inf) can never corrupt the wire. NdjsonSessionWriter::FormatNumber guards
    // the same non-finite case but intentionally diverges on the sentinel: it emits
    // JSON `null`, whereas this collapses to 0 (not null) because these feed numeric
    // x/y/z, pitch/yaw/roll and r/g/b/a fields whose consumers expect a number, and
    // the empty-box contract the callers rely on is already zeroed.
    inline double SanitizeFinite(double Value)
    {
        return FMath::IsFinite(Value) ? Value : 0.0;
    }

    inline TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), SanitizeFinite(Value.X));
        Obj->SetNumberField(TEXT("y"), SanitizeFinite(Value.Y));
        Obj->SetNumberField(TEXT("z"), SanitizeFinite(Value.Z));
        return Obj;
    }

    inline TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), SanitizeFinite(Value.Pitch));
        Obj->SetNumberField(TEXT("yaw"), SanitizeFinite(Value.Yaw));
        Obj->SetNumberField(TEXT("roll"), SanitizeFinite(Value.Roll));
        return Obj;
    }

    inline TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("r"), SanitizeFinite(Value.R));
        Obj->SetNumberField(TEXT("g"), SanitizeFinite(Value.G));
        Obj->SetNumberField(TEXT("b"), SanitizeFinite(Value.B));
        Obj->SetNumberField(TEXT("a"), SanitizeFinite(Value.A));
        return Obj;
    }

    // Additive dump-merge: copies every Src field that Dest has not already set into
    // Dest (existing Dest fields take precedence). The "delegate to a shared dump
    // builder, then layer in the fields the handler hasn't authored by hand" idiom
    // that the get_animation_info montage/blend-space branches share. No-ops on a
    // null Dest or Src.
    inline void MergeMissingFields(const TSharedPtr<FJsonObject>& Dest, const TSharedPtr<FJsonObject>& Src)
    {
        if (!Dest || !Src)
        {
            return;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Src->Values)
        {
            if (!Dest->HasField(Pair.Key))
            {
                Dest->SetField(Pair.Key, Pair.Value);
            }
        }
    }

    // Resolves a UENUM value to its name string; returns empty for non-reflected enums.
    template <typename TEnum>
    inline FString EnumValueToString(TEnum Value)
    {
        if (UEnum* Meta = StaticEnum<TEnum>())
        {
            return Meta->GetNameStringByValue(static_cast<int64>(Value));
        }
        return FString();
    }

    // Larger / dependency-pulling helpers live in JsonBuilders.cpp.

    // Resolves a namespaced UENUM value to its bare member name (the "EEnum::"
    // type prefix stripped via BlueprintEnumHelpers::StripEnumScope), so dumps
    // carry human-readable tokens (Filter / Range / Patrol) rather than raw
    // integers. Falls back to the integer string when the value has no name.
    // Defined in JsonBuilders.cpp to keep the StripEnumScope dependency out of
    // this header. Single home for the prefix-strip logic the EnvQuery and
    // StateTree dump builders previously each copied.
    FString EnumMemberName(const UEnum* Enum, int64 Value);

    TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Transform);

    // Wraps a string list as JSON string values, sorted by default. The canonical
    // "sort a string list, emit as a JSON string array" body; BuildNameArrayJson
    // delegates to it after stringifying the FNames.
    TArray<TSharedPtr<FJsonValue>> BuildStringArrayJson(const TArray<FString>& Strings, bool bSort = true);

    TArray<TSharedPtr<FJsonValue>> BuildNameArrayJson(const TArray<FName>& Names);

    FString GetActorLevelPackageName(const AActor* Actor);

    FString GetObjectPathSafe(const UObject* Object);
}
