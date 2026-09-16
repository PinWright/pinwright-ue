// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Math/BoxSphereBounds.h"

namespace MeshBoundsHelpers
{
    inline TSharedPtr<FJsonObject> BuildBoundsJson(const FBoxSphereBounds& Bounds)
    {
        TSharedPtr<FJsonObject> Origin = MakeShared<FJsonObject>();
        Origin->SetNumberField(TEXT("x"), Bounds.Origin.X);
        Origin->SetNumberField(TEXT("y"), Bounds.Origin.Y);
        Origin->SetNumberField(TEXT("z"), Bounds.Origin.Z);

        TSharedPtr<FJsonObject> Extent = MakeShared<FJsonObject>();
        Extent->SetNumberField(TEXT("x"), Bounds.BoxExtent.X);
        Extent->SetNumberField(TEXT("y"), Bounds.BoxExtent.Y);
        Extent->SetNumberField(TEXT("z"), Bounds.BoxExtent.Z);

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetObjectField(TEXT("origin"), Origin);
        Out->SetObjectField(TEXT("extent"), Extent);
        Out->SetNumberField(TEXT("sphereRadius"), Bounds.SphereRadius);
        return Out;
    }
}
