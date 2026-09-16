// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared fixture helpers for asset-dump tests in this folder. Extracted from per-test
// anonymous namespaces so Unity-merged TUs in Tests/Utility/ do not collide on
// LoadJsonFile / MakeFixture redefinitions.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "TestAssetDumpObjectRefsFixture.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDumpFixtureHelpers
{
    // Reads Path off disk and parses it as a JSON object. Returns null on either read
    // failure or parse failure (callers TestTrue on .IsValid()).
    inline TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path)
    {
        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *Path))
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Object;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
        if (!FJsonSerializer::Deserialize(Reader, Object))
        {
            return nullptr;
        }
        return Object;
    }

    // Constructs the property-mix fixture used by the object-refs / map-references tests.
    inline UTestAssetDumpObjectRefsFixture* MakeFixture()
    {
        return NewObject<UTestAssetDumpObjectRefsFixture>(GetTransientPackage());
    }

    // Returns true if any path in WrittenPaths ends with SidecarFileName.
    inline bool WrittenPathsContains(const TArray<FString>& WrittenPaths, const FString& SidecarFileName)
    {
        for (const FString& Path : WrittenPaths)
        {
            if (Path.EndsWith(SidecarFileName))
            {
                return true;
            }
        }
        return false;
    }
}
