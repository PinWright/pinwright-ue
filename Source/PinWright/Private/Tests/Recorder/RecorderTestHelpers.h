// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared JSON field getters for the Recorder/ test cluster. Previously
// RecorderQueryEngineTests.cpp and RecorderSegmentsTests.cpp each defined an
// identical GetNum in their own anonymous namespaces; with bUseUnity = true,
// Unity build can stitch the adjacent TUs together and the duplicate
// definitions collide. One named-namespace definition per program fixes that.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

namespace RecorderTestHelpers
{
    inline double GetNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Default = -999.0)
    {
        double V = Default;
        if (Obj.IsValid()) Obj->TryGetNumberField(Field, V);
        return V;
    }

    inline FString GetStr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString V;
        if (Obj.IsValid()) Obj->TryGetStringField(Field, V);
        return V;
    }

    inline bool GetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        bool V = false;
        if (Obj.IsValid()) Obj->TryGetBoolField(Field, V);
        return V;
    }
}
