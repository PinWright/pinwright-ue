// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirStructLiteralUtils.cpp - Reflective formatter for positional struct literal tokens

#include "Compiler/BpirStructLiteralUtils.h"


#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Utils/ClassUtils.h"

namespace
{
    enum class EKnownPinStruct { Vector, Rotator, LinearColor, Unknown };

    EKnownPinStruct ClassifyByFName(FName StructFName)
    {
        if (StructFName == TEXT("Vector")) return EKnownPinStruct::Vector;
        if (StructFName == TEXT("Rotator")) return EKnownPinStruct::Rotator;
        if (StructFName == TEXT("LinearColor")) return EKnownPinStruct::LinearColor;
        return EKnownPinStruct::Unknown;
    }

    EKnownPinStruct ClassifyByDisplayName(const FString& StructName)
    {
        if (StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase) || StructName.Equals(TEXT("FVector"), ESearchCase::IgnoreCase)) return EKnownPinStruct::Vector;
        if (StructName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase) || StructName.Equals(TEXT("FRotator"), ESearchCase::IgnoreCase)) return EKnownPinStruct::Rotator;
        if (StructName.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase) || StructName.Equals(TEXT("FLinearColor"), ESearchCase::IgnoreCase)) return EKnownPinStruct::LinearColor;
        return EKnownPinStruct::Unknown;
    }

    // Split the inner argument list of `F<Identifier>(...)` on top-level commas.
    // Nested parens are tracked so e.g. `FBox((X=...),(X=...))` is split into 2
    // components rather than 4. Whitespace is trimmed off each component.
    bool SplitTopLevelArgs(const FString& Inner, TArray<FString>& OutComponents)
    {
        OutComponents.Reset();

        int32 Depth = 0;
        int32 SegmentStart = 0;
        for (int32 i = 0; i < Inner.Len(); ++i)
        {
            const TCHAR Ch = Inner[i];
            if (Ch == TEXT('(') || Ch == TEXT('[') || Ch == TEXT('{'))
            {
                ++Depth;
            }
            else if (Ch == TEXT(')') || Ch == TEXT(']') || Ch == TEXT('}'))
            {
                if (Depth <= 0)
                {
                    return false;
                }
                --Depth;
            }
            else if (Ch == TEXT(',') && Depth == 0)
            {
                OutComponents.Add(Inner.Mid(SegmentStart, i - SegmentStart).TrimStartAndEnd());
                SegmentStart = i + 1;
            }
        }

        if (Depth != 0)
        {
            return false;
        }

        OutComponents.Add(Inner.Mid(SegmentStart).TrimStartAndEnd());
        return true;
    }
}

namespace BpirStructLiteralUtils
{
    bool TryFormatPositionalStructLiteralAsPinText(const FString& Token, FString& OutPinText)
    {
        // Shape check: must be `F<Ident>(...)` with at least one char before `(`.
        if (Token.Len() < 4 || Token[0] != TEXT('F') || !Token.EndsWith(TEXT(")")))
        {
            return false;
        }

        int32 OpenParen = INDEX_NONE;
        if (!Token.FindChar(TEXT('('), OpenParen) || OpenParen <= 1)
        {
            return false;
        }

        // Identifier must be C++-identifier-shaped (no dots, spaces, etc).
        const FString StructName = Token.Mid(0, OpenParen);
        for (int32 i = 1; i < StructName.Len(); ++i)
        {
            const TCHAR Ch = StructName[i];
            const bool bIsIdentChar = FChar::IsAlpha(Ch) || FChar::IsDigit(Ch) || Ch == TEXT('_');
            if (!bIsIdentChar)
            {
                return false;
            }
        }

        UScriptStruct* Struct = ResolveUScriptStruct(StructName);
        if (!Struct)
        {
            return false;
        }

        const FString Inner = Token.Mid(OpenParen + 1, Token.Len() - OpenParen - 2);

        TArray<FString> Components;
        if (!SplitTopLevelArgs(Inner, Components))
        {
            return false;
        }

        // K2's pin-default validator (UEdGraphSchema_K2::IsPinDefaultValid →
        // FDefaultValueHelper::IsStringValid{Vector,Rotator,LinearColor}) hardcodes
        // a different grammar per struct, so emit the exact form each one accepts.
        switch (ClassifyByFName(Struct->GetFName()))
        {
        case EKnownPinStruct::Vector:
            if (Components.Num() == 3)
            {
                OutPinText = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *Components[0], *Components[1], *Components[2]);
                return true;
            }
            break;
        case EKnownPinStruct::Rotator:
            if (Components.Num() == 3)
            {
                // IsStringValidRotator delegates to IsStringValidVector, whose primary
                // path is positional CSV "p,y,r" — `(Pitch=,Yaw=,Roll=)` is rejected.
                OutPinText = FString::Printf(TEXT("%s,%s,%s"), *Components[0], *Components[1], *Components[2]);
                return true;
            }
            break;
        case EKnownPinStruct::LinearColor:
            if (Components.Num() == 4)
            {
                OutPinText = FString::Printf(TEXT("(R=%s,G=%s,B=%s,A=%s)"),
                    *Components[0], *Components[1], *Components[2], *Components[3]);
                return true;
            }
            break;
        case EKnownPinStruct::Unknown:
            break;
        }

        return false;
    }

    FString EscapeBpirStringInner(const FString& In)
    {
        return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
    }

    FString EscapeBpirString(const FString& In)
    {
        return FString::Printf(TEXT("\"%s\""), *EscapeBpirStringInner(In));
    }

    FString UnescapeBpirString(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("\\\""), TEXT("\""));
        Out.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
        return Out;
    }

    FString FormatStructComponentsAsBpir(const FString& StructName, TArrayView<const FString> Components)
    {
        // Only sugar the three positional pin-default forms the decompiler has
        // historically emitted. Everything else is left to the caller; an empty
        // return means "I did not recognise this — use your fallback".
        switch (ClassifyByDisplayName(StructName))
        {
        case EKnownPinStruct::Vector:
            if (Components.Num() == 3)
            {
                return FString::Printf(TEXT("FVector(%s,%s,%s)"), *Components[0], *Components[1], *Components[2]);
            }
            break;
        case EKnownPinStruct::Rotator:
            if (Components.Num() == 3)
            {
                return FString::Printf(TEXT("FRotator(%s,%s,%s)"), *Components[0], *Components[1], *Components[2]);
            }
            break;
        case EKnownPinStruct::LinearColor:
            if (Components.Num() == 4)
            {
                return FString::Printf(TEXT("FLinearColor(%s,%s,%s,%s)"), *Components[0], *Components[1], *Components[2], *Components[3]);
            }
            break;
        case EKnownPinStruct::Unknown:
            break;
        }
        return FString();
    }

    // Format a JSON number in a stable, parser-friendly form. Doubles render with
    // %g (UE default) which strips the trailing ".0"; that's fine for BPIR numeric
    // literals which already accept both integer and decimal forms.
    static FString FormatNumberAsBpir(double N)
    {
        if (FMath::IsNaN(N) || !FMath::IsFinite(N))
        {
            return TEXT("0");
        }
        const double Truncated = FMath::TruncToDouble(N);
        if (Truncated == N && FMath::Abs(N) < 1.0e15)
        {
            return FString::Printf(TEXT("%lld"), (int64)N);
        }
        return FString::SanitizeFloat(N);
    }

    FString FormatJsonValueAsBpir(const TSharedPtr<FJsonValue>& JsonValue, FProperty* Prop)
    {
        if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
        {
            return TEXT("nullptr");
        }

        switch (JsonValue->Type)
        {
        case EJson::Boolean:
            return JsonValue->AsBool() ? TEXT("true") : TEXT("false");

        case EJson::Number:
            return FormatNumberAsBpir(JsonValue->AsNumber());

        case EJson::String:
        {
            const FString Str = JsonValue->AsString();

            // A struct-typed property whose JSON value arrived as a string is the
            // ExportText fallback path (Utils/PropertyUtils.cpp:332): the string is
            // already in canonical UE `(Key=Val,...)` form, which BPIR's struct-literal
            // grammar accepts as-is (see Decompiler/BpirDecompiler.cpp:1656).
            if (Prop && CastField<FStructProperty>(Prop) && Str.StartsWith(TEXT("(")))
            {
                return Str;
            }

            // Object/class refs and FName/Path values arrive as plain strings; quote
            // them so the parser sees a single token rather than splitting on internal
            // whitespace or commas.
            return EscapeBpirString(Str);
        }

        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>& Items = JsonValue->AsArray();

            // FStructProperty + JSON array means a positional struct pin-default
            // (see Utils/PropertyUtils.cpp:312-329). Stringify the components first,
            // then route through the shared sugar helper so the FVector/FRotator/
            // FLinearColor formatting lives in exactly one place.
            if (FStructProperty* SP = CastField<FStructProperty>(Prop))
            {
                const FString StructName = SP->Struct ? SP->Struct->GetName() : FString();
                TArray<FString> Components;
                Components.Reserve(Items.Num());
                for (const TSharedPtr<FJsonValue>& Item : Items)
                {
                    Components.Add(FormatNumberAsBpir(Item.IsValid() ? Item->AsNumber() : 0.0));
                }
                const FString Sugared = FormatStructComponentsAsBpir(StructName, Components);
                if (!Sugared.IsEmpty())
                {
                    return Sugared;
                }
            }

            FProperty* InnerProp = nullptr;
            if (FArrayProperty* AP = CastField<FArrayProperty>(Prop))
            {
                InnerProp = AP->Inner;
            }

            TArray<FString> Parts;
            Parts.Reserve(Items.Num());
            for (const TSharedPtr<FJsonValue>& Item : Items)
            {
                Parts.Add(FormatJsonValueAsBpir(Item, InnerProp));
            }
            return FString::Printf(TEXT("[%s]"), *FString::Join(Parts, TEXT(", ")));
        }

        case EJson::Object:
        {
            const TSharedPtr<FJsonObject>& Obj = JsonValue->AsObject();
            if (!Obj.IsValid())
            {
                return TEXT("{}");
            }

            // Map property: format as `{ Key: Value, ... }` using the value-property
            // for nested formatting hints.
            FProperty* ValueProp = nullptr;
            if (FMapProperty* MP = CastField<FMapProperty>(Prop))
            {
                ValueProp = MP->ValueProp;
            }

            TArray<FString> Pairs;
            Pairs.Reserve(Obj->Values.Num());
            for (const auto& KV : Obj->Values)
            {
                Pairs.Add(FString::Printf(TEXT("%s: %s"),
                    *KV.Key,
                    *FormatJsonValueAsBpir(KV.Value, ValueProp)));
            }
            Pairs.Sort();
            return FString::Printf(TEXT("{ %s }"), *FString::Join(Pairs, TEXT(", ")));
        }

        default:
            return TEXT("nullptr");
        }
    }
}
