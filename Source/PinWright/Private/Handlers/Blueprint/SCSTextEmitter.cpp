// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Blueprint/SCSTextEmitter.h"

#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "Utils/SortedJsonWriter.h"
#include "Compat/JsonKeyCompat.h"

namespace
{
    struct FScsTextComponent
    {
        TSharedPtr<FJsonObject> Object;
        FString Name;
        FString ClassName;
        FString Source;
        FString Parent;
        FString InheritedFrom;
        int32 InputIndex = 0;
        TArray<int32> Children;
    };

    int32 SourceRank(const FString& Source)
    {
        if (Source == TEXT("scs")) return 0;
        if (Source == TEXT("native")) return 1;
        if (Source == TEXT("inherited-override")) return 2;
        if (Source == TEXT("inherited-scs")) return 3;
        return 4;
    }

    FString OneLine(FString Value)
    {
        Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
        Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
        return Value;
    }

    FString SerializeJsonValue(const TSharedPtr<FJsonValue>& Value)
    {
        FString Out;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        SortedJsonWriter::WriteSortedValue(Writer, Value);
        Writer->Close();
        return Out;
    }

    void AppendIndent(FString& Out, int32 Indent)
    {
        for (int32 Index = 0; Index < Indent; ++Index)
        {
            Out += TEXT("  ");
        }
    }

    void AppendScalarField(FString& Out, int32 Indent, const TCHAR* Field, const FString& Value)
    {
        if (Value.IsEmpty())
        {
            return;
        }

        AppendIndent(Out, Indent);
        Out += Field;
        Out += TEXT(": ");
        Out += OneLine(Value);
        Out += TEXT("\n");
    }

    void AppendObjectFields(FString& Out, int32 Indent, const TCHAR* BlockName, const TSharedPtr<FJsonObject>& Object)
    {
        if (!Object.IsValid() || Object->Values.Num() == 0)
        {
            return;
        }

        TArray<FString> Keys;
        for (const auto& Pair : Object->Values)
        {
            Keys.Add(EARGCompat::JsonKeyToString(Pair.Key));
        }
        Keys.Sort();

        AppendIndent(Out, Indent);
        Out += BlockName;
        Out += TEXT(" {\n");
        for (const FString& Key : Keys)
        {
            AppendIndent(Out, Indent + 1);
            Out += Key;
            Out += TEXT(": ");
            Out += SerializeJsonValue(Object->Values[EARGCompat::JsonFieldKey(Key)]);
            Out += TEXT("\n");
        }
        AppendIndent(Out, Indent);
        Out += TEXT("}\n");
    }

    bool ComponentLess(const TArray<FScsTextComponent>& Components, int32 A, int32 B)
    {
        const FScsTextComponent& Left = Components[A];
        const FScsTextComponent& Right = Components[B];
        const int32 LeftRank = SourceRank(Left.Source);
        const int32 RightRank = SourceRank(Right.Source);
        if (LeftRank != RightRank)
        {
            return LeftRank < RightRank;
        }

        const int32 NameCompare = Left.Name.Compare(Right.Name, ESearchCase::CaseSensitive);
        if (NameCompare != 0)
        {
            return NameCompare < 0;
        }
        return Left.InputIndex < Right.InputIndex;
    }

    void AppendComponent(FString& Out, const TArray<FScsTextComponent>& Components, int32 ComponentIndex, int32 Indent)
    {
        const FScsTextComponent& Component = Components[ComponentIndex];

        AppendIndent(Out, Indent);
        Out += TEXT("component(");
        Out += Component.Name;
        Out += TEXT(") {\n");

        AppendScalarField(Out, Indent + 1, TEXT("type"), Component.ClassName);
        AppendScalarField(Out, Indent + 1, TEXT("source"), Component.Source);
        AppendScalarField(Out, Indent + 1, TEXT("inherits"), Component.InheritedFrom);
        if (!Component.Parent.IsEmpty())
        {
            bool bParentResolved = false;
            for (const FScsTextComponent& Candidate : Components)
            {
                if (Candidate.Name == Component.Parent)
                {
                    bParentResolved = true;
                    break;
                }
            }
            if (!bParentResolved)
            {
                AppendScalarField(Out, Indent + 1, TEXT("parent"), Component.Parent);
            }
        }

        const TSharedPtr<FJsonObject>* TransformObj = nullptr;
        if (Component.Object->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj)
        {
            AppendObjectFields(Out, Indent + 1, TEXT("transform"), *TransformObj);
        }

        const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
        if (Component.Object->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj)
        {
            AppendObjectFields(Out, Indent + 1, TEXT("properties"), *PropertiesObj);
        }

        if (!Component.Children.IsEmpty())
        {
            TArray<int32> SortedChildren = Component.Children;
            SortedChildren.Sort([&Components](int32 A, int32 B)
            {
                return ComponentLess(Components, A, B);
            });

            AppendIndent(Out, Indent + 1);
            Out += TEXT("children {\n");
            for (int32 ChildIndex : SortedChildren)
            {
                AppendComponent(Out, Components, ChildIndex, Indent + 2);
            }
            AppendIndent(Out, Indent + 1);
            Out += TEXT("}\n");
        }

        AppendIndent(Out, Indent);
        Out += TEXT("}\n");
    }
}

namespace SCSTextEmitter
{

FString BuildText(const TSharedPtr<FJsonObject>& ScsJson)
{
    if (!ScsJson.IsValid())
    {
        return FString();
    }

    const TArray<TSharedPtr<FJsonValue>>* ComponentValues = nullptr;
    if (!ScsJson->TryGetArrayField(TEXT("components"), ComponentValues) || !ComponentValues || ComponentValues->IsEmpty())
    {
        return FString();
    }

    TArray<FScsTextComponent> Components;
    TMap<FString, int32> NameToIndex;
    Components.Reserve(ComponentValues->Num());

    for (const TSharedPtr<FJsonValue>& Value : *ComponentValues)
    {
        TSharedPtr<FJsonObject> Object =
            Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        if (!Object.IsValid())
        {
            continue;
        }

        FScsTextComponent Component;
        Component.Object = Object;
        Component.InputIndex = Components.Num();
        if (!Object->TryGetStringField(TEXT("name"), Component.Name) || Component.Name.IsEmpty())
        {
            continue;
        }
        Object->TryGetStringField(TEXT("class"), Component.ClassName);
        Object->TryGetStringField(TEXT("source"), Component.Source);
        Object->TryGetStringField(TEXT("parent"), Component.Parent);
        Object->TryGetStringField(TEXT("inheritedFrom"), Component.InheritedFrom);

        NameToIndex.Add(Component.Name, Components.Num());
        Components.Add(MoveTemp(Component));
    }

    if (Components.IsEmpty())
    {
        return FString();
    }

    TArray<int32> Roots;
    for (int32 Index = 0; Index < Components.Num(); ++Index)
    {
        const FString& Parent = Components[Index].Parent;
        if (!Parent.IsEmpty())
        {
            if (const int32* ParentIndex = NameToIndex.Find(Parent))
            {
                Components[*ParentIndex].Children.Add(Index);
                continue;
            }
        }
        Roots.Add(Index);
    }

    Roots.Sort([&Components](int32 A, int32 B)
    {
        return ComponentLess(Components, A, B);
    });

    FString Out;
    for (int32 RootIndex : Roots)
    {
        AppendComponent(Out, Components, RootIndex, 0);
    }
    return Out;
}

}
