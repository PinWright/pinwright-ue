// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "Math/RotationMatrix.h"

#include "PwValue.h"

namespace PwValueRead
{
    inline const FPwValue* FindValue(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        return Params.Find(FString(Name));
    }

    inline bool HasValue(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        return FindValue(Params, Name) != nullptr;
    }

    inline double GetNumber(const TMap<FString, FPwValue>& Params, const TCHAR* Name, double Default)
    {
        const FPwValue* Value = FindValue(Params, Name);
        return (Value && Value->Type == EPwValueType::Number) ? Value->Number : Default;
    }

    inline int32 GetInt(const TMap<FString, FPwValue>& Params, const TCHAR* Name, int32 Default)
    {
        const FPwValue* Value = FindValue(Params, Name);
        return (Value && Value->Type == EPwValueType::Number)
            ? static_cast<int32>(FMath::RoundToDouble(Value->Number))
            : Default;
    }

    inline bool GetBool(const TMap<FString, FPwValue>& Params, const TCHAR* Name, bool bDefault)
    {
        const FPwValue* Value = FindValue(Params, Name);
        return (Value && Value->Type == EPwValueType::Identifier) ? (Value->Text == TEXT("true")) : bDefault;
    }

    inline FString GetString(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        const FPwValue* Value = FindValue(Params, Name);
        return (Value && Value->Type == EPwValueType::String) ? Value->Text : FString();
    }

    inline FString GetIdentifier(const TMap<FString, FPwValue>& Params, const TCHAR* Name, const TCHAR* Default)
    {
        const FPwValue* Value = FindValue(Params, Name);
        return (Value && Value->Type == EPwValueType::Identifier) ? Value->Text : FString(Default);
    }

    inline FVector GetVector3(const TMap<FString, FPwValue>& Params, const TCHAR* Name, const FVector& Default)
    {
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::Tuple || Value->Tuple.Num() < 3)
        {
            return Default;
        }
        return FVector(Value->Tuple[0], Value->Tuple[1], Value->Tuple[2]);
    }

    inline FVector2D GetVector2(const TMap<FString, FPwValue>& Params, const TCHAR* Name, const FVector2D& Default)
    {
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::Tuple || Value->Tuple.Num() < 2)
        {
            return Default;
        }
        return FVector2D(Value->Tuple[0], Value->Tuple[1]);
    }

    inline FLinearColor GetColor(const TMap<FString, FPwValue>& Params, const TCHAR* Name,
                                 const FLinearColor& Default)
    {
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::Tuple || Value->Tuple.Num() < 4)
        {
            return Default;
        }
        return FLinearColor(
            static_cast<float>(Value->Tuple[0]), static_cast<float>(Value->Tuple[1]),
            static_cast<float>(Value->Tuple[2]), static_cast<float>(Value->Tuple[3]));
    }

    inline TArray<FVector2D> GetPointList2(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        TArray<FVector2D> Points;
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::TupleList)
        {
            return Points;
        }
        Points.Reserve(Value->TupleList.Num());
        for (const TArray<double>& Entry : Value->TupleList)
        {
            if (Entry.Num() >= 2)
            {
                Points.Add(FVector2D(Entry[0], Entry[1]));
            }
        }
        return Points;
    }

    inline TArray<FVector> GetPointList3(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        TArray<FVector> Points;
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::TupleList)
        {
            return Points;
        }
        Points.Reserve(Value->TupleList.Num());
        for (const TArray<double>& Entry : Value->TupleList)
        {
            if (Entry.Num() >= 3)
            {
                Points.Add(FVector(Entry[0], Entry[1], Entry[2]));
            }
        }
        return Points;
    }

    inline TArray<FLinearColor> GetPointList4(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        TArray<FLinearColor> Colors;
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::TupleList)
        {
            return Colors;
        }
        Colors.Reserve(Value->TupleList.Num());
        for (const TArray<double>& Entry : Value->TupleList)
        {
            if (Entry.Num() >= 4)
            {
                Colors.Add(FLinearColor(
                    static_cast<float>(Entry[0]), static_cast<float>(Entry[1]),
                    static_cast<float>(Entry[2]), static_cast<float>(Entry[3])));
            }
        }
        return Colors;
    }

    inline TArray<int32> GetIndexList(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        TArray<int32> Indices;
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::Tuple)
        {
            return Indices;
        }
        Indices.Reserve(Value->Tuple.Num());
        for (double Entry : Value->Tuple)
        {
            Indices.Add(static_cast<int32>(FMath::RoundToDouble(Entry)));
        }
        return Indices;
    }

    // The source format spells rotations as (roll, pitch, yaw), while FRotator's
    // constructor takes (pitch, yaw, roll). Keep this conversion in one place.
    inline FRotator MakeRotator(const FVector& RollPitchYaw)
    {
        return FRotator(RollPitchYaw.Y, RollPitchYaw.Z, RollPitchYaw.X);
    }

    inline FRotator GetRotator(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        return MakeRotator(GetVector3(Params, Name, FVector::ZeroVector));
    }

    inline TArray<FTransform> GetFrameList(const TMap<FString, FPwValue>& Params, const TCHAR* Name)
    {
        TArray<FTransform> Frames;
        const FPwValue* Value = FindValue(Params, Name);
        if (!Value || Value->Type != EPwValueType::TupleList)
        {
            return Frames;
        }
        Frames.Reserve(Value->TupleList.Num());
        for (const TArray<double>& Entry : Value->TupleList)
        {
            if (Entry.Num() >= 6)
            {
                Frames.Add(FTransform(
                    MakeRotator(FVector(Entry[3], Entry[4], Entry[5])),
                    FVector(Entry[0], Entry[1], Entry[2]),
                    FVector::OneVector));
            }
        }
        return Frames;
    }

    // ---- Aiming: turn two endpoints into a rotation ------------------------------------
    //
    // Every centre-placed generator in these formats is built along its OWN LOCAL +Z. Aiming one
    // at a target therefore means finding the rotation that carries local +Z onto a direction -
    // and that rotation is not unique. A direction fixes two degrees of freedom; the third, the
    // TWIST about the aimed axis, is left free, and picking it implicitly is the trap.
    //
    // The trap is invisible on a round primitive and load-bearing the moment the op carries a
    // non-uniform `scale=`, because the flattening axis is the primitive's local Y. Two ops aimed
    // at the same direction by two different-but-equally-valid rotations flatten along two
    // different world axes: a fan of flattened leaves comes out half flat and half on edge, with
    // every op's parameters correct and nothing to grep for. So the twist is pinned here, once,
    // and both overloads below state exactly what pins it.
    //
    // The reference direction is called `Up` because that is what an author is choosing - the
    // side the primitive's flat face turns towards - and never because world Z is special to the
    // math. It is not: any direction not parallel to the axis works.

    // Aim local +Z along Direction, with the twist pinned by Up.
    //
    // The rule, in full: local +Y is set PERPENDICULAR TO BOTH Up and the aim axis
    // (Y = normalize(Up x Axis)), and local X follows as Y x Axis. Equivalently, Up lies in the
    // plane the primitive's local X and Z axes span, so a flattening `scale=(1, k, 1)` squashes
    // the primitive TOWARDS Up rather than at some angle that depends on where it is pointing.
    //
    // Returns false, writing nothing, on the three inputs that name no frame: a zero-length
    // Direction, a zero-length Up, and an Up parallel to the axis. Callers must report those
    // rather than substitute a fallback - a substituted twist is exactly the silent wrong answer
    // this function exists to remove.
    inline bool MakeAimRotator(const FVector& Direction, const FVector& Up, FRotator& OutRotator)
    {
        const FVector Axis = Direction.GetSafeNormal();
        const FVector UpDirection = Up.GetSafeNormal();
        if (Axis.IsNearlyZero() || UpDirection.IsNearlyZero())
        {
            return false;
        }

        // Squared length of the cross product IS sin^2 of the angle between two unit vectors, so
        // this one test covers both parallel and antiparallel. The threshold is deliberately
        // coarse: a twist reference a quarter of a degree off the axis is numerically a frame and
        // authorially a mistake, and reporting it is cheaper than shipping the frame it implies.
        const FVector LocalY = FVector::CrossProduct(UpDirection, Axis);
        if (LocalY.SizeSquared() < 1e-6)
        {
            return false;
        }

        // MakeFromZY takes the Z axis and a Y REFERENCE, and re-derives an orthonormal frame from
        // them. LocalY is already exactly perpendicular to Axis, so nothing is re-derived here and
        // the frame is the one written above; passing Up straight in would instead land Up's
        // perpendicular component on local X, which is a different frame by a quarter turn.
        OutRotator = FRotationMatrix::MakeFromZY(Axis, LocalY.GetSafeNormal()).Rotator();
        return true;
    }

    // Aim local +Z along Direction with no twist reference given: roll = 0.
    //
    // For every Direction that is not vertical this is IDENTICAL to MakeAimRotator(Direction,
    // (0,0,1)) - roll = 0 is what "local Y is horizontal" means, and horizontal is what
    // perpendicular-to-world-Z means. The reason it is a separate closed form rather than a call
    // with a default argument is the case where the two differ: on a VERTICAL axis, world Z is
    // parallel to the aim and the general rule has no answer, while this one still does
    // (yaw = 0, so local +Y lands on world +Y). A vertical limb is ordinary, so the default must
    // not refuse it; an author who writes `up=` parallel to the axis has made a mistake, and that
    // one is refused. That asymmetry is the whole reason both exist.
    //
    // Derivation, so the next reader need not rebuild it: with roll = 0 an FRotator carries local
    // +Z to (-sin(pitch)*cos(yaw), -sin(pitch)*sin(yaw), cos(pitch)). Setting that equal to a unit
    // Direction gives cos(pitch) = z, so pitch = -acos(z) makes sin(pitch) negative and the
    // leading minus signs cancel, leaving yaw = atan2(y, x).
    inline FRotator MakeAimRotator(const FVector& Direction)
    {
        const FVector Axis = Direction.GetSafeNormal();
        const double Pitch = -FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Axis.Z, -1.0, 1.0)));
        const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(Axis.Y, Axis.X));
        return FRotator(Pitch, Yaw, 0.0);
    }

    inline FTransform ReadTransformParams(const TMap<FString, FPwValue>& Params)
    {
        const FVector At = GetVector3(Params, TEXT("at"), FVector::ZeroVector);
        const FVector Scale = GetVector3(Params, TEXT("scale"), FVector::OneVector);
        return FTransform(GetRotator(Params, TEXT("rotate")), At, Scale);
    }
}
