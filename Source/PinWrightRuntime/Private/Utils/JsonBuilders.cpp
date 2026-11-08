// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/JsonBuilders.h"

#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "UObject/Package.h"


namespace JsonBuilders
{
    FString EnumMemberName(const UEnum* Enum, int64 Value)
    {
        if (!Enum)
        {
            return FString::FromInt(Value);
        }
        // GetNameStringByValue returns the fully-qualified "EEnum::Member";
        // drop the type prefix to leave the bare member name.
        FString Name = Enum->GetNameStringByValue(Value);
        int32 ScopeSep = INDEX_NONE;
        if (Name.FindLastChar(TEXT(':'), ScopeSep) && ScopeSep + 1 < Name.Len())
        {
            Name.MidInline(ScopeSep + 1);
        }
        return Name.IsEmpty() ? FString::FromInt(Value) : Name;
    }

    TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Transform)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("location"), BuildVectorJson(Transform.GetLocation()));
        Obj->SetObjectField(TEXT("rotation"), BuildRotatorJson(Transform.GetRotation().Rotator()));
        Obj->SetObjectField(TEXT("scale"), BuildVectorJson(Transform.GetScale3D()));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildStringArrayJson(const TArray<FString>& Strings, bool bSort)
    {
        TArray<FString> Ordered(Strings);
        if (bSort)
        {
            Ordered.Sort();
        }

        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Ordered.Num());
        for (const FString& Str : Ordered)
        {
            Result.Add(MakeShared<FJsonValueString>(Str));
        }
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> BuildNameArrayJson(const TArray<FName>& Names)
    {
        TArray<FString> NameStrings;
        NameStrings.Reserve(Names.Num());
        for (const FName& Name : Names)
        {
            NameStrings.Add(Name.ToString());
        }
        return BuildStringArrayJson(NameStrings);
    }

    FString GetActorLevelPackageName(const AActor* Actor)
    {
        const ULevel* Level = Actor ? Actor->GetLevel() : nullptr;
        return Level && Level->GetPackage() ? Level->GetPackage()->GetName() : FString();
    }

    FString GetObjectPathSafe(const UObject* Object)
    {
        return Object ? Object->GetPathName() : FString();
    }
}
