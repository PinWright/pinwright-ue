// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared dump-file inspection helpers extracted from the per-test anonymous
// namespaces of the Assets/ test cluster. Previously each TestXxxDumpBuilder.cpp
// defined identical copies of HasDumpFile/FindDumpFile/LoadJsonFile in its own
// anonymous namespace; once the tests merged into the main module, Unity build
// stitched them into a single TU and the duplicate definitions tripped C2084.
// One named-namespace definition per program fixes that.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/SharedPointer.h"

namespace AssetDumpTestHelpers
{
    inline bool HasDumpFile(const TArray<FString>& Paths, const TCHAR* FileName)
    {
        for (const FString& Path : Paths)
        {
            if (Path.EndsWith(FileName))
            {
                return true;
            }
        }
        return false;
    }

    inline FString FindDumpFile(const TArray<FString>& Paths, const TCHAR* FileName)
    {
        for (const FString& Path : Paths)
        {
            if (Path.EndsWith(FileName))
            {
                return Path;
            }
        }
        return FString();
    }

    inline TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path)
    {
        FString Body;
        if (!FFileHelper::LoadFileToString(Body, *Path))
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Object;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
        if (!FJsonSerializer::Deserialize(Reader, Object))
        {
            return nullptr;
        }
        return Object;
    }
}
