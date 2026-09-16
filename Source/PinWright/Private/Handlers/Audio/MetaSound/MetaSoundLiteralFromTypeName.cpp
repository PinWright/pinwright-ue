// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MetaSoundLiteralFromTypeName.h"

#if MCP_HAS_METASOUND_LITERAL_HELPER

#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Utils/AssetUtils.h"
#include "UObject/Class.h"
#include "UObject/Object.h"

#if PW_METASOUND_HAS_DATATYPE_REGISTRY
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendLiteral.h"
#endif

namespace PinWright::MetaSound
{
    // The registry's array-type name suffix. Engine spelling:
    // METASOUND_DATA_TYPE_NAME_ARRAY_TYPE_SPECIFIER (MetasoundGraphCore/Public/
    // MetasoundDataReferenceMacro.h), which Metasound::CreateArrayTypeNameFromElementTypeName
    // appends and CreateElementTypeNameFromArrayTypeName chops. Spelled here as a literal rather
    // than reached through those helpers so this TU takes no link dependency on
    // MetasoundGraphCore; the token is part of the serialized data-type name and has been stable
    // since the registry shipped.
    static const TCHAR* const PwMetaSoundArrayTypeSuffix = TEXT(":Array");

    // Names a JSON entry's kind for an error payload. Deliberately reports what the entry IS
    // rather than what it could be coerced into: an array whose elements are validated by kind
    // has to say which kind it got.
    static FString PwDescribeJsonValueKind(const TSharedPtr<FJsonValue>& Entry)
    {
        if (!Entry.IsValid())
        {
            return TEXT("missing");
        }
        switch (Entry->Type)
        {
        case EJson::Null:    return TEXT("null");
        case EJson::String:  return TEXT("string");
        case EJson::Number:  return TEXT("number");
        case EJson::Boolean: return TEXT("boolean");
        case EJson::Array:   return TEXT("array");
        case EJson::Object:  return TEXT("object");
        default:             return TEXT("unknown");
        }
    }

    // Renders an entry for the error payload: its string form when it has one, else its kind.
    static FString PwRenderJsonEntry(const TSharedPtr<FJsonValue>& Entry)
    {
        FString AsText;
        if (Entry.IsValid() && Entry->Type == EJson::String && Entry->TryGetString(AsText))
        {
            return AsText;
        }
        double AsNumber = 0.0;
        if (Entry.IsValid() && Entry->Type == EJson::Number && Entry->TryGetNumber(AsNumber))
        {
            return FString::SanitizeFloat(AsNumber);
        }
        bool bAsBool = false;
        if (Entry.IsValid() && Entry->Type == EJson::Boolean && Entry->TryGetBool(bAsBool))
        {
            return bAsBool ? TEXT("true") : TEXT("false");
        }
        return PwDescribeJsonValueKind(Entry);
    }

    bool MakeDefaultLiteralForMetaSoundType(const FString& TypeName, FMetasoundFrontendLiteral& OutLiteral)
    {
        if (TypeName.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
        {
            OutLiteral.Set(0.0f);
            return true;
        }
        if (TypeName.Equals(TEXT("Int"), ESearchCase::IgnoreCase) ||
            TypeName.Equals(TEXT("Int32"), ESearchCase::IgnoreCase))
        {
            OutLiteral.Set(static_cast<int32>(0));
            return true;
        }
        if (TypeName.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) ||
            TypeName.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase))
        {
            OutLiteral.Set(false);
            return true;
        }
        if (TypeName.Equals(TEXT("String"), ESearchCase::IgnoreCase))
        {
            OutLiteral.Set(FString());
            return true;
        }
        if (TypeName.Equals(TEXT("Audio"), ESearchCase::IgnoreCase))
        {
            // Null object literal — engine treats this as "no asset bound yet"
            // for audio-typed input pins.
            OutLiteral.Set(static_cast<UObject*>(nullptr));
            return true;
        }
        if (TypeName.Equals(TEXT("Trigger"), ESearchCase::IgnoreCase))
        {
            // Metasound::FTrigger registers with ELiteralType::Boolean
            // (MetasoundPrimitives.cpp:118), so a Trigger graph input's default literal
            // is a bool; false means "not fired at author time", which is what a
            // Play/Stop input wants until the caller triggers it at runtime.
            OutLiteral.Set(false);
            return true;
        }
        if (TypeName.Equals(TEXT("WaveAsset"), ESearchCase::IgnoreCase))
        {
            // Metasound::FWaveAsset registers as ELiteralType::UObjectProxy over USoundWave
            // (MetasoundEngineModule.cpp:51). A null object literal is the unbound state the
            // Wave Player's "Wave Asset" pin carries before a wave is assigned; bind a real
            // wave with set_metasound_default / set_metasound_node_input_default's objectValue.
            OutLiteral.Set(static_cast<UObject*>(nullptr));
            return true;
        }
        if (TypeName.Equals(TEXT("Time"), ESearchCase::IgnoreCase))
        {
            // Metasound::FTime registers with ELiteralType::Float (MetasoundPrimitives.cpp:119).
            // Wave Player's Loop Start / Loop Duration are Time-typed, so a stem-looping graph
            // needs this alongside Trigger and WaveAsset.
            OutLiteral.Set(0.0f);
            return true;
        }

#if PW_METASOUND_HAS_DATATYPE_REGISTRY
        // Everything else comes from the live registry rather than another branch here.
        // GetDesiredLiteralType is the registry's own answer for "what literal shape does
        // this data type construct from", and SetType initializes that shape to its default
        // value — so a registered type the table above does not name still produces a literal
        // AddGraphInput's FindConstDefaultChecked(check(Literal)) accepts. An unregistered
        // name still returns false, which is what makes an unknown type an error (§3) rather
        // than a silent fallback.
        const FName CanonicalName(*CanonicalizeMetaSoundTypeName(TypeName));
        Metasound::Frontend::IDataTypeRegistry& Registry = Metasound::Frontend::IDataTypeRegistry::Get();
        if (!Registry.IsRegistered(CanonicalName))
        {
            return false;
        }

        const EMetasoundFrontendLiteralType FrontendLiteralType =
            Metasound::Frontend::GetMetasoundFrontendLiteralType(Registry.GetDesiredLiteralType(CanonicalName));
        if (FrontendLiteralType == EMetasoundFrontendLiteralType::Invalid)
        {
            return false;
        }

        OutLiteral.SetType(FrontendLiteralType);
        return true;
#else
        return false;
#endif // PW_METASOUND_HAS_DATATYPE_REGISTRY
    }

    FString CanonicalizeMetaSoundTypeName(const FString& TypeName)
    {
        // An array name canonicalizes through its ELEMENT: the registry registers
        // TArray<TDataType> under the canonical element's name plus ":Array", so without this
        // the documented convenience spellings would work for a scalar ("Int") and silently fail
        // for its array ("Int:Array" names no registered type; "Int32:Array" does).
        const int32 ArraySuffixLen = FCString::Strlen(PwMetaSoundArrayTypeSuffix);
        if (TypeName.Len() > ArraySuffixLen
            && TypeName.EndsWith(PwMetaSoundArrayTypeSuffix, ESearchCase::IgnoreCase))
        {
            return CanonicalizeMetaSoundTypeName(TypeName.LeftChop(ArraySuffixLen))
                + PwMetaSoundArrayTypeSuffix;
        }

        // Convenience names whose registry key differs from the spelling callers
        // pass (MetasoundPrimitives.cpp registers the canonical keys). This must
        // cover every alias MakeDefaultLiteralForMetaSoundType accepts, or a name
        // the literal helper greenlights ("Boolean") would still reach the builder
        // unregistered. Float/Bool/String/Trigger/Time/WaveAsset already match their
        // registry keys verbatim.
        if (TypeName.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
        {
            return TEXT("Int32");
        }
        if (TypeName.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase))
        {
            return TEXT("Bool");
        }
        // Unknown / already-canonical names pass through so the builder can emit
        // its own "unregistered DataType" diagnostic for genuinely bad input.
        return TypeName;
    }

    bool IsMetaSoundArrayDataType(const FString& TypeName)
    {
        if (TypeName.IsEmpty())
        {
            return false;
        }

        const FString CanonicalName = CanonicalizeMetaSoundTypeName(TypeName);
#if PW_METASOUND_HAS_DATATYPE_REGISTRY
        Metasound::Frontend::FDataTypeRegistryInfo Info;
        if (Metasound::Frontend::IDataTypeRegistry::Get().GetDataTypeInfo(FName(*CanonicalName), Info))
        {
            // The registry's own verdict, so a type whose array-ness the name does not advertise
            // is still classified correctly.
            return Info.bIsArrayType;
        }
#endif
        // Unregistered name, or a host without the registry header: the name shape is what the
        // engine itself uses to derive an array type's name, so it is the honest fallback.
        return CanonicalName.EndsWith(PwMetaSoundArrayTypeSuffix, ESearchCase::IgnoreCase);
    }

    FString GetMetaSoundArrayElementTypeName(const FString& ArrayTypeName)
    {
        const FString CanonicalName = CanonicalizeMetaSoundTypeName(ArrayTypeName);
        const int32 ArraySuffixLen = FCString::Strlen(PwMetaSoundArrayTypeSuffix);
        if (CanonicalName.Len() <= ArraySuffixLen
            || !CanonicalName.EndsWith(PwMetaSoundArrayTypeSuffix, ESearchCase::IgnoreCase))
        {
            return FString();
        }
        return CanonicalName.LeftChop(ArraySuffixLen);
    }

    bool MakeArrayLiteralForMetaSoundType(
        const TArray<TSharedPtr<FJsonValue>>& Entries,
        const FString& DataTypeName,
        FMetasoundFrontendLiteral& OutLiteral,
        FMetaSoundArrayLiteralOutcome& OutOutcome)
    {
        OutOutcome = FMetaSoundArrayLiteralOutcome();

        if (!IsMetaSoundArrayDataType(DataTypeName))
        {
            OutOutcome.Result = EMetaSoundArrayLiteralResult::NotAnArrayType;
            return false;
        }
        OutOutcome.ElementTypeName = GetMetaSoundArrayElementTypeName(DataTypeName);

        // The ARRAY type's own registry literal shape decides the element form — never the shape
        // of the JSON that arrived. Same direction the scalar path takes.
        EMetasoundFrontendLiteralType ArrayLiteralType = EMetasoundFrontendLiteralType::Invalid;
#if PW_METASOUND_HAS_DATATYPE_REGISTRY
        {
            const FName CanonicalName(*CanonicalizeMetaSoundTypeName(DataTypeName));
            Metasound::Frontend::IDataTypeRegistry& Registry = Metasound::Frontend::IDataTypeRegistry::Get();
            if (Registry.IsRegistered(CanonicalName))
            {
                ArrayLiteralType = Metasound::Frontend::GetMetasoundFrontendLiteralType(
                    Registry.GetDesiredLiteralType(CanonicalName));
            }
        }
#endif

        auto FailEntry = [&OutOutcome](int32 Index, EMetaSoundArrayLiteralResult Result,
                                       const TCHAR* ExpectedKind, const TSharedPtr<FJsonValue>& Entry)
        {
            OutOutcome.Result = Result;
            OutOutcome.FailedIndex = Index;
            OutOutcome.ExpectedEntryKind = ExpectedKind;
            OutOutcome.FailedEntryText = PwRenderJsonEntry(Entry);
            return false;
        };

        switch (ArrayLiteralType)
        {
        case EMetasoundFrontendLiteralType::FloatArray:
        {
            TArray<float> Values;
            Values.Reserve(Entries.Num());
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                double AsNumber = 0.0;
                if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::Number
                    || !Entries[Index]->TryGetNumber(AsNumber))
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("number"), Entries[Index]);
                }
                Values.Add(static_cast<float>(AsNumber));
            }
            OutLiteral.Set(Values);
            return true;
        }

        case EMetasoundFrontendLiteralType::IntegerArray:
        {
            TArray<int32> Values;
            Values.Reserve(Entries.Num());
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                double AsNumber = 0.0;
                if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::Number
                    || !Entries[Index]->TryGetNumber(AsNumber))
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("integer"), Entries[Index]);
                }
                // Rounding 1.7 into an Int32 element would publish a value the caller never
                // asked for, which is the fallback this surface refuses everywhere else.
                if (!FMath::IsNearlyEqual(AsNumber, FMath::RoundToDouble(AsNumber))
                    || AsNumber < static_cast<double>(MIN_int32)
                    || AsNumber > static_cast<double>(MAX_int32))
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("integer"), Entries[Index]);
                }
                Values.Add(static_cast<int32>(FMath::RoundToDouble(AsNumber)));
            }
            OutLiteral.Set(Values);
            return true;
        }

        case EMetasoundFrontendLiteralType::BooleanArray:
        {
            TArray<bool> Values;
            Values.Reserve(Entries.Num());
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                bool bValue = false;
                if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::Boolean
                    || !Entries[Index]->TryGetBool(bValue))
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("boolean"), Entries[Index]);
                }
                Values.Add(bValue);
            }
            OutLiteral.Set(Values);
            return true;
        }

        case EMetasoundFrontendLiteralType::StringArray:
        {
            TArray<FString> Values;
            Values.Reserve(Entries.Num());
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                FString Value;
                if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::String
                    || !Entries[Index]->TryGetString(Value))
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("string"), Entries[Index]);
                }
                Values.Add(MoveTemp(Value));
            }
            OutLiteral.Set(Values);
            return true;
        }

        case EMetasoundFrontendLiteralType::UObjectArray:
        {
            TArray<UObject*> Values;
            Values.Reserve(Entries.Num());
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                FString ObjectPath;
                if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::String
                    || !Entries[Index]->TryGetString(ObjectPath))
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("asset path"), Entries[Index]);
                }

                // Resolve + class-check through the SAME helper objectValue uses, against the
                // ELEMENT type. The scratch literal it writes is discarded; only the resolved
                // object is kept, so the pairing (WaveAsset -> USoundWave) still comes from the
                // registry and is never hardcoded here.
                FMetasoundFrontendLiteral ScratchLiteral;
                UObject* ResolvedObject = nullptr;
                FString ExpectedClassPath;
                const EMetaSoundObjectLiteralResult ObjectResult = MakeObjectLiteralForMetaSoundType(
                    ObjectPath, OutOutcome.ElementTypeName, ScratchLiteral, ResolvedObject, ExpectedClassPath);
                if (ObjectResult != EMetaSoundObjectLiteralResult::Ok)
                {
                    OutOutcome.FailedIndex = Index;
                    OutOutcome.ExpectedEntryKind = TEXT("asset path");
                    OutOutcome.FailedEntryText = ObjectPath;
                    OutOutcome.ExpectedClassPath = ExpectedClassPath;
                    OutOutcome.ResolvedObject = ResolvedObject;
                    OutOutcome.Result = (ObjectResult == EMetaSoundObjectLiteralResult::WrongClass)
                        ? EMetaSoundArrayLiteralResult::ElementWrongClass
                        : EMetaSoundArrayLiteralResult::ElementNotFound;
                    return false;
                }
                Values.Add(ResolvedObject);
            }
            OutLiteral.Set(Values);
            return true;
        }

        case EMetasoundFrontendLiteralType::NoneArray:
        {
            // A NoneArray carries no per-element value at all — the literal is just a COUNT of
            // default-constructed elements. The only JSON that honestly means that is a list of
            // nulls, so anything else is refused rather than silently reduced to its length.
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::Null)
                {
                    return FailEntry(Index, EMetaSoundArrayLiteralResult::ElementTypeMismatch,
                        TEXT("null"), Entries[Index]);
                }
            }
            FMetasoundFrontendLiteral::FDefaultArray DefaultArray;
            DefaultArray.Num = Entries.Num();
            OutLiteral.Set(DefaultArray);
            return true;
        }

        default:
            OutOutcome.Result = EMetaSoundArrayLiteralResult::UnsupportedArrayShape;
            return false;
        }
    }

    bool IsMetaSoundDataTypeRegistered(const FString& TypeName)
    {
#if PW_METASOUND_HAS_DATATYPE_REGISTRY
        const FName CanonicalName(*CanonicalizeMetaSoundTypeName(TypeName));
        return Metasound::Frontend::IDataTypeRegistry::Get().IsRegistered(CanonicalName);
#else
        (void)TypeName;
        return false;
#endif
    }

    TArray<FString> SuggestMetaSoundDataTypeNames(const FString& TypeName, int32 MaxSuggestions)
    {
        TArray<FString> Suggestions;
#if PW_METASOUND_HAS_DATATYPE_REGISTRY
        if (TypeName.IsEmpty() || MaxSuggestions <= 0)
        {
            return Suggestions;
        }

        TArray<FName> RegisteredNames;
        Metasound::Frontend::IDataTypeRegistry::Get().GetRegisteredDataTypeNames(RegisteredNames);
        for (const FName& RegisteredName : RegisteredNames)
        {
            const FString AsText = RegisteredName.ToString();
            if (AsText.Contains(TypeName, ESearchCase::IgnoreCase))
            {
                Suggestions.Add(AsText);
            }
        }
        Suggestions.Sort();
        if (Suggestions.Num() > MaxSuggestions)
        {
            Suggestions.SetNum(MaxSuggestions);
        }
#else
        (void)TypeName;
        (void)MaxSuggestions;
#endif
        return Suggestions;
    }

    EMetaSoundObjectLiteralResult MakeObjectLiteralForMetaSoundType(
        const FString& ObjectPath,
        const FString& DataTypeName,
        FMetasoundFrontendLiteral& OutLiteral,
        UObject*& OutObject,
        FString& OutExpectedClassPath)
    {
        OutObject = nullptr;
        OutExpectedClassPath.Reset();

        // Same traversal sanitizer + /Content -> /Game rewrite every audio path goes through.
        FString ResolvePath = NormalizeContentAssetPath(ObjectPath);
        if (ResolvePath.IsEmpty())
        {
            return EMetaSoundObjectLiteralResult::PathRejected;
        }

        // Object resolution wants Package.Object; agents pass the bare /Game/Dir/Name form,
        // so append the short name when the path carries no object part.
        if (!ResolvePath.Contains(TEXT(".")))
        {
            FString ShortName;
            if (ResolvePath.Split(TEXT("/"), nullptr, &ShortName,
                    ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !ShortName.IsEmpty())
            {
                ResolvePath = ResolvePath + TEXT(".") + ShortName;
            }
        }

        // StaticFindObject first (already-loaded and never-saved transient objects), then
        // StaticLoadObject for on-disk assets — the shared resolver, not a local copy.
        FString ResolveError;
        UObject* Object = ::ResolveUObjectByPath(ResolvePath, ResolveError);
        if (!Object)
        {
            return EMetaSoundObjectLiteralResult::ObjectNotFound;
        }

#if PW_METASOUND_HAS_DATATYPE_REGISTRY
        if (!DataTypeName.IsEmpty())
        {
            const FName CanonicalName(*CanonicalizeMetaSoundTypeName(DataTypeName));
            Metasound::Frontend::IDataTypeRegistry& Registry = Metasound::Frontend::IDataTypeRegistry::Get();
            if (const UClass* ExpectedClass = Registry.GetUClassForDataType(CanonicalName))
            {
                OutExpectedClassPath = ExpectedClass->GetPathName();
            }
            if (!Registry.IsValidUObjectForDataType(CanonicalName, Object))
            {
                // Report the object that WAS found so the caller can name both sides.
                OutObject = Object;
                return EMetaSoundObjectLiteralResult::WrongClass;
            }
        }
#else
        (void)DataTypeName;
#endif

        OutObject = Object;
        OutLiteral.Set(Object);
        return EMetaSoundObjectLiteralResult::Ok;
    }

    void DescribeMetaSoundLiteral(const FMetasoundFrontendLiteral& Literal, const TSharedPtr<FJsonObject>& Out)
    {
        if (!Out.IsValid())
        {
            return;
        }

        FString TypeText = TEXT("Unknown");
        if (const UEnum* LiteralTypeEnum = StaticEnum<EMetasoundFrontendLiteralType>())
        {
            TypeText = LiteralTypeEnum->GetNameStringByValue(static_cast<int64>(Literal.GetType()));
        }
        Out->SetStringField(TEXT("literalType"), TypeText);
        Out->SetStringField(TEXT("value"), Literal.ToString());

        UObject* AsObject = nullptr;
        if (Literal.TryGet(AsObject))
        {
            // An unbound object literal is a real, distinct state (a Wave Player with no wave),
            // so emit the field with an empty path rather than omitting it.
            Out->SetStringField(TEXT("objectPath"), AsObject ? AsObject->GetPathName() : FString());
        }

        if (!Literal.IsArray())
        {
            return;
        }

        // An array's element count is the one fact ToString() buries: a caller that just wrote
        // five waves needs to see five, not parse a flattened string. Emitted for every array
        // literal, including the empty one (arrayNum:0 is how "cleared" reads).
        TArray<UObject*> AsObjects;
        if (Literal.TryGet(AsObjects))
        {
            TArray<TSharedPtr<FJsonValue>> ObjectPathsJson;
            ObjectPathsJson.Reserve(AsObjects.Num());
            for (const UObject* Element : AsObjects)
            {
                // An unbound slot keeps its position with an empty path, same reasoning as the
                // scalar objectPath above.
                ObjectPathsJson.Add(MakeShared<FJsonValueString>(
                    Element ? Element->GetPathName() : FString()));
            }
            Out->SetNumberField(TEXT("arrayNum"), AsObjects.Num());
            Out->SetArrayField(TEXT("objectPaths"), ObjectPathsJson);
            return;
        }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
        // FMetasoundFrontendLiteral::GetArrayNum() is a UE 5.4 addition; on 5.3 the count comes
        // from whichever typed getter accepts this literal.
        TArray<bool> AsBools;
        TArray<int32> AsInts;
        TArray<float> AsFloats;
        TArray<FString> AsStrings;
        const int32 ArrayNum =
              Literal.TryGet(AsBools)   ? AsBools.Num()
            : Literal.TryGet(AsInts)    ? AsInts.Num()
            : Literal.TryGet(AsFloats)  ? AsFloats.Num()
            : Literal.TryGet(AsStrings) ? AsStrings.Num()
            : 0;
        Out->SetNumberField(TEXT("arrayNum"), ArrayNum);
#else
        Out->SetNumberField(TEXT("arrayNum"), Literal.GetArrayNum());
#endif
    }
}

#endif // MCP_HAS_METASOUND_LITERAL_HELPER
