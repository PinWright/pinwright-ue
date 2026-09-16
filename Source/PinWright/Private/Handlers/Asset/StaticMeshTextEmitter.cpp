// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/StaticMeshTextEmitter.h"

#include "AssetTextEmitterHelpers.h"
#include "Dom/JsonValue.h"

namespace
{
    using AssetTextEmitterHelpers::FormatNumber;
    using AssetTextEmitterHelpers::AppendIndent;
    using AssetTextEmitterHelpers::AppendScalar;
    using AssetTextEmitterHelpers::AppendOptionalString;
    using AssetTextEmitterHelpers::AppendOptionalNumber;

    FString FormatNumberArray(const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        TArray<FString> Parts;
        Parts.Reserve(Values.Num());
        for (const TSharedPtr<FJsonValue>& Value : Values)
        {
            if (Value.IsValid() && Value->Type == EJson::Number)
            {
                Parts.Add(FormatNumber(Value->AsNumber()));
            }
        }
        return FString::Printf(TEXT("[%s]"), *FString::Join(Parts, TEXT(", ")));
    }

    FString FormatVectorObject(const TSharedPtr<FJsonObject>& Object)
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        const bool bHasX = Object.IsValid() && Object->TryGetNumberField(TEXT("x"), X);
        const bool bHasY = Object.IsValid() && Object->TryGetNumberField(TEXT("y"), Y);
        const bool bHasZ = Object.IsValid() && Object->TryGetNumberField(TEXT("z"), Z);
        return FString::Printf(TEXT("(x=%s, y=%s, z=%s)"),
            bHasX ? *FormatNumber(X) : TEXT("0"),
            bHasY ? *FormatNumber(Y) : TEXT("0"),
            bHasZ ? *FormatNumber(Z) : TEXT("0"));
    }

    void AppendBounds(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TSharedPtr<FJsonObject>* Bounds = nullptr;
        if (!Root->TryGetObjectField(TEXT("bounds"), Bounds) || !Bounds || !Bounds->IsValid())
        {
            return;
        }

        AppendIndent(Out, 1);
        Out += TEXT("bounds {\n");

        const TSharedPtr<FJsonObject>* Origin = nullptr;
        if ((*Bounds)->TryGetObjectField(TEXT("origin"), Origin) && Origin && Origin->IsValid())
        {
            AppendScalar(Out, 2, TEXT("origin"), FormatVectorObject(*Origin));
        }

        const TSharedPtr<FJsonObject>* Extent = nullptr;
        if ((*Bounds)->TryGetObjectField(TEXT("extent"), Extent) && Extent && Extent->IsValid())
        {
            AppendScalar(Out, 2, TEXT("extent"), FormatVectorObject(*Extent));
        }

        AppendOptionalNumber(Out, 2, *Bounds, TEXT("sphereRadius"));
        AppendIndent(Out, 1);
        Out += TEXT("}\n");
    }

    void AppendMaterials(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
        if (!Root->TryGetArrayField(TEXT("materials"), Materials) || !Materials || Materials->IsEmpty())
        {
            return;
        }

        AppendIndent(Out, 1);
        Out += TEXT("materials {\n");
        for (const TSharedPtr<FJsonValue>& Value : *Materials)
        {
            TSharedPtr<FJsonObject> Material = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
            if (!Material.IsValid())
            {
                continue;
            }

            FString Slot;
            FString Path;
            Material->TryGetStringField(TEXT("slot"), Slot);
            Material->TryGetStringField(TEXT("path"), Path);

            AppendIndent(Out, 2);
            Out += TEXT("material { slot: ");
            Out += Slot.IsEmpty() ? TEXT("None") : Slot;
            Out += TEXT(", path: ");
            Out += Path.IsEmpty() ? TEXT("None") : Path;
            Out += TEXT(" }\n");
        }
        AppendIndent(Out, 1);
        Out += TEXT("}\n");
    }

    void AppendNumberArray(FString& Out, const TSharedPtr<FJsonObject>& Root, const TCHAR* Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Root->TryGetArrayField(Field, Values) && Values)
        {
            AppendScalar(Out, 1, Field, FormatNumberArray(*Values));
        }
    }

    void AppendOptionalBool(FString& Out, int32 Indent, const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        bool bValue = false;
        if (Object.IsValid() && Object->TryGetBoolField(Field, bValue))
        {
            AppendScalar(Out, Indent, Field, bValue ? TEXT("true") : TEXT("false"));
        }
    }

    void AppendSections(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!Root->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->IsEmpty())
        {
            return;
        }

        AppendIndent(Out, 1);
        Out += TEXT("sections {\n");
        for (const TSharedPtr<FJsonValue>& Value : *Sections)
        {
            const TSharedPtr<FJsonObject> Section = Value.IsValid() && Value->Type == EJson::Object
                ? Value->AsObject() : nullptr;
            if (!Section.IsValid())
            {
                continue;
            }

            AppendIndent(Out, 2);
            Out += TEXT("section {\n");
            AppendOptionalNumber(Out, 3, Section, TEXT("lodIndex"));
            AppendOptionalNumber(Out, 3, Section, TEXT("index"));
            AppendOptionalNumber(Out, 3, Section, TEXT("materialIndex"));
            AppendOptionalString(Out, 3, Section, TEXT("materialSlotName"));
            AppendOptionalNumber(Out, 3, Section, TEXT("firstIndex"));
            AppendOptionalNumber(Out, 3, Section, TEXT("numTriangles"));
            AppendOptionalNumber(Out, 3, Section, TEXT("minVertexIndex"));
            AppendOptionalNumber(Out, 3, Section, TEXT("maxVertexIndex"));
            AppendOptionalBool(Out, 3, Section, TEXT("bEnableCollision"));
            AppendOptionalBool(Out, 3, Section, TEXT("bCastShadow"));
            AppendIndent(Out, 2);
            Out += TEXT("}\n");
        }
        AppendIndent(Out, 1);
        Out += TEXT("}\n");
    }

    void AppendSlotUsage(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TArray<TSharedPtr<FJsonValue>>* SlotUsage = nullptr;
        if (!Root->TryGetArrayField(TEXT("slotUsage"), SlotUsage) || !SlotUsage || SlotUsage->IsEmpty())
        {
            return;
        }

        AppendIndent(Out, 1);
        Out += TEXT("slotUsage {\n");
        for (const TSharedPtr<FJsonValue>& Value : *SlotUsage)
        {
            const TSharedPtr<FJsonObject> Slot = Value.IsValid() && Value->Type == EJson::Object
                ? Value->AsObject() : nullptr;
            if (!Slot.IsValid())
            {
                continue;
            }

            AppendIndent(Out, 2);
            Out += TEXT("slot {\n");
            AppendOptionalNumber(Out, 3, Slot, TEXT("materialIndex"));
            AppendOptionalString(Out, 3, Slot, TEXT("materialSlotName"));
            AppendOptionalNumber(Out, 3, Slot, TEXT("lod0TriangleCount"));
            AppendOptionalNumber(Out, 3, Slot, TEXT("lod0TriangleFraction"));
            AppendIndent(Out, 2);
            Out += TEXT("}\n");
        }
        AppendIndent(Out, 1);
        Out += TEXT("}\n");
    }

    void AppendCollision(FString& Out, const TSharedPtr<FJsonObject>& Root)
    {
        const TSharedPtr<FJsonObject>* Collision = nullptr;
        if (!Root->TryGetObjectField(TEXT("collision"), Collision) || !Collision || !Collision->IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* Elements = nullptr;
        if (!(*Collision)->TryGetObjectField(TEXT("elements"), Elements) || !Elements || !Elements->IsValid())
        {
            return;
        }

        AppendIndent(Out, 1);
        Out += TEXT("collision {\n");
        AppendOptionalNumber(Out, 2, *Elements, TEXT("sphere"));
        AppendOptionalNumber(Out, 2, *Elements, TEXT("box"));
        AppendOptionalNumber(Out, 2, *Elements, TEXT("sphyl"));
        AppendOptionalNumber(Out, 2, *Elements, TEXT("convex"));
        AppendOptionalNumber(Out, 2, *Elements, TEXT("taperedCapsule"));
        AppendIndent(Out, 1);
        Out += TEXT("}\n");
    }
}

namespace StaticMeshTextEmitter
{

FString BuildText(const TSharedPtr<FJsonObject>& StaticMeshJson)
{
    if (!StaticMeshJson.IsValid())
    {
        return FString();
    }

    FString Out;
    Out += TEXT("static_mesh {\n");
    AppendBounds(Out, StaticMeshJson);
    AppendMaterials(Out, StaticMeshJson);
    AppendOptionalNumber(Out, 1, StaticMeshJson, TEXT("lods"));
    AppendNumberArray(Out, StaticMeshJson, TEXT("trianglesByLod"));
    AppendNumberArray(Out, StaticMeshJson, TEXT("verticesByLod"));
    AppendSections(Out, StaticMeshJson);
    AppendSlotUsage(Out, StaticMeshJson);
    AppendNumberArray(Out, StaticMeshJson, TEXT("uvChannelsByLod"));
    AppendOptionalNumber(Out, 1, StaticMeshJson, TEXT("lightmapResolution"));
    AppendOptionalNumber(Out, 1, StaticMeshJson, TEXT("lightMapCoordinateIndex"));
    AppendOptionalString(Out, 1, StaticMeshJson, TEXT("collisionTraceFlag"));
    AppendCollision(Out, StaticMeshJson);
    Out += TEXT("}\n");
    return Out;
}

}
