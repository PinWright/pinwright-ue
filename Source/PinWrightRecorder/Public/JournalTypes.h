// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

/**
 * Variant discriminator for a recorded value. Stored as a byte to keep per-change-point
 * memory small in long sessions. Ported from the Unity recorder's ValueKind.
 */
enum class EJournalKind : uint8
{
    Float,
    Int,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Rotator,
    Enum,
    String
};

/** Capture domain for a recorded value/event; emitted as the NDJSON `dom` tag. */
enum class EJournalDomain : uint8
{
    None,
    Physics,
    Render,
    Net,
    UI,
    Loading
};

/**
 * Event severity ordering. Numeric order is meaningful: query filters use a severity
 * floor as a `>=` threshold against these values. Mirrors the Unity recorder's Severity.
 */
enum class EJournalSeverity : uint8
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

/**
 * Tagged variant for a single recorded sample. Up to four `double` components cover every
 * numeric/vector kind; `S` carries string payloads (and the enum member name). Ported from
 * the Unity recorder's RecordedValue.
 */
struct FRecordedValue
{
    EJournalKind Kind = EJournalKind::Float;
    double F0 = 0.0;
    double F1 = 0.0;
    double F2 = 0.0;
    double F3 = 0.0;
    FString S;

    static FRecordedValue From(float Value) { return FRecordedValue{ EJournalKind::Float, Value }; }
    static FRecordedValue From(double Value) { return FRecordedValue{ EJournalKind::Float, Value }; }
    static FRecordedValue From(int32 Value) { return FRecordedValue{ EJournalKind::Int, static_cast<double>(Value) }; }
    static FRecordedValue From(int64 Value) { return FRecordedValue{ EJournalKind::Int, static_cast<double>(Value) }; }
    static FRecordedValue From(bool Value) { return FRecordedValue{ EJournalKind::Bool, Value ? 1.0 : 0.0 }; }
    static FRecordedValue From(const FVector2D& Value) { return FRecordedValue{ EJournalKind::Vec2, Value.X, Value.Y }; }
    static FRecordedValue From(const FVector& Value) { return FRecordedValue{ EJournalKind::Vec3, Value.X, Value.Y, Value.Z }; }
    static FRecordedValue From(const FVector4& Value) { return FRecordedValue{ EJournalKind::Vec4, Value.X, Value.Y, Value.Z, Value.W }; }
    static FRecordedValue From(const FQuat& Value) { return FRecordedValue{ EJournalKind::Quat, Value.X, Value.Y, Value.Z, Value.W }; }

    static FRecordedValue From(const FRotator& Value)
    {
        return FRecordedValue{ EJournalKind::Rotator, Value.Pitch, Value.Yaw, Value.Roll };
    }

    static FRecordedValue From(const FString& Value)
    {
        FRecordedValue Result;
        Result.Kind = EJournalKind::String;
        Result.S = Value;
        return Result;
    }

    /** Enum encoder: stores the underlying integer in F0 and the member name in S. */
    static FRecordedValue FromEnum(int64 UnderlyingValue, const FString& MemberName)
    {
        FRecordedValue Result;
        Result.Kind = EJournalKind::Enum;
        Result.F0 = static_cast<double>(UnderlyingValue);
        Result.S = MemberName;
        return Result;
    }

    /**
     * True when this value differs from `Other` by more than `Epsilon`. Vector/quat/rotator
     * kinds compare Euclidean magnitude of the component delta; scalars compare absolute delta;
     * string/enum/bool compare for any difference.
     */
    bool SignificantlyDiffers(const FRecordedValue& Other, double Epsilon) const
    {
        if (Kind != Other.Kind)
        {
            return true;
        }

        switch (Kind)
        {
        case EJournalKind::String:
            return !S.Equals(Other.S, ESearchCase::CaseSensitive);

        case EJournalKind::Enum:
        case EJournalKind::Bool:
            return F0 != Other.F0;

        case EJournalKind::Float:
        case EJournalKind::Int:
            return FMath::Abs(F0 - Other.F0) > Epsilon;

        case EJournalKind::Vec2:
        {
            const double Dx = F0 - Other.F0;
            const double Dy = F1 - Other.F1;
            return FMath::Sqrt(Dx * Dx + Dy * Dy) > Epsilon;
        }
        case EJournalKind::Vec3:
        case EJournalKind::Rotator:
        {
            const double Dx = F0 - Other.F0;
            const double Dy = F1 - Other.F1;
            const double Dz = F2 - Other.F2;
            return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz) > Epsilon;
        }
        case EJournalKind::Vec4:
        case EJournalKind::Quat:
        {
            const double Dx = F0 - Other.F0;
            const double Dy = F1 - Other.F1;
            const double Dz = F2 - Other.F2;
            const double Dw = F3 - Other.F3;
            return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz + Dw * Dw) > Epsilon;
        }
        default:
            return true;
        }
    }
};

/** Message-type discriminator for an enqueued `FRecordMsg`. */
enum class EJournalMsgType : uint8
{
    Value,
    Event,
    RegisterObject
};

/**
 * One lock-free producer-enqueued record. Producers capture the wall-clock timestamp at the
 * call site, build this struct, and enqueue it; the single game-thread consumer drains the
 * queue, performs object/variable upsert + change-point compression, and writes NDJSON.
 */
struct FRecordMsg
{
    EJournalMsgType MsgType = EJournalMsgType::Value;

    /** Wall-clock seconds captured at the producing call site (primary time axis). */
    double Ts = 0.0;

    /** Secondary domain stamp inherited from the producing thread (see FJournalRecorder::StampDomain). */
    EJournalDomain Domain = EJournalDomain::None;
    double DomainTime = 0.0;
    int64 DomainFrame = -1;

    /** Object identity key (catalog) and variable/event tag. */
    FName Key;
    FName Tag;

    /** Value payload (MsgType == Value). */
    FRecordedValue Value;

    /** Event payload (MsgType == Event). */
    EJournalSeverity Severity = EJournalSeverity::Info;
    TArray<TPair<FName, FRecordedValue>> Props;

    /** Object label used when MsgType == RegisterObject. */
    FString Label;
};
