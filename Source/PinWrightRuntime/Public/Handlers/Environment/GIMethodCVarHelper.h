// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared helper: applies the dynamic global-illumination method string to the
// matching CVars (live viewport effect, no persistence). Lifted verbatim from
// lighting.setup_global_illumination so lighting.* and rendering.* share one
// source of truth for the {Method string → CVar integer} mapping.
//
// Returns true when Method matched a known token and CVars were issued.
// Caller is responsible for emitting INVALID_GI_METHOD on false.

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

inline bool ApplyDynamicGIMethodToCVars(const FString& Method)
{
    if (Method == TEXT("LumenGI"))
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (CVar) CVar->Set(1);
        IConsoleVariable* CVarRefl = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ReflectionMethod"));
        if (CVarRefl) CVarRefl->Set(1);
        return true;
    }
    if (Method == TEXT("ScreenSpace"))
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (CVar) CVar->Set(2);
        return true;
    }
    if (Method == TEXT("None"))
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (CVar) CVar->Set(0);
        return true;
    }
    if (Method == TEXT("RayTraced"))
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (CVar) CVar->Set(3);
        return true;
    }
    if (Method == TEXT("Lightmass"))
    {
        IConsoleVariable* CVarGI = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (CVarGI) CVarGI->Set(0);
        return true;
    }
    return false;
}
