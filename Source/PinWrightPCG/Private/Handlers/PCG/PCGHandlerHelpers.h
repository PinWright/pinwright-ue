// Copyright (c) 2026 Alexander Penkin. MIT License.

// PCGHandlerHelpers.h
//
// Shared helpers used across the PCG handler family:
//   PCGGraphAuthoring.cpp, PCGGraphInspect.cpp, PCGAddSlopeFilter.cpp,
//   PCGAddNoiseFilter.cpp, PCGAddSubgraph.cpp, PCGSetSelfPruningSettings.cpp.
//
// Previously each .cpp carried its own copy of LoadGraphOrError (and a couple
// also of FindNodeByName). Under unity builds this risks ODR collisions and
// already wastes lines. Consolidated here into a single named namespace
// (PinWrightPCG) — chosen as a slight generalization of the existing
// PinWrightPCGAuthoring namespace so that non-authoring handlers
// (inspect, filters, subgraph, pruning) can share it without semantic mismatch.
//
// The entire header is gated on PCGGraph.h availability so the plugin still
// compiles cleanly when the PCG plugin is not enabled.

#pragma once

#include "CoreMinimal.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Interfaces/IPluginManager.h"
#include "ModuleDescriptor.h"
#include "PluginReferenceDescriptor.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectGlobals.h"

namespace PinWrightPCG
{
    // Parse a JSON number without accepting strings, null, or non-finite values. This is the
    // scalar primitive used by strict structured-value readers in PCG handlers.
    inline bool TryReadFiniteJsonNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
    {
        if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Out))
        {
            return false;
        }
        return FMath::IsFinite(Out);
    }

    inline TSharedPtr<FJsonValue> FindVectorComponent(
        const TSharedPtr<FJsonObject>& Object, const TCHAR* UpperKey, const TCHAR* LowerKey)
    {
        if (!Object.IsValid())
        {
            return nullptr;
        }

        if (TSharedPtr<FJsonValue> Value = Object->TryGetField(UpperKey))
        {
            return Value;
        }
        return Object->TryGetField(LowerKey);
    }

    // Parse exactly three finite JSON numbers from [X,Y,Z] or an object carrying all three
    // X/Y/Z axes. Object keys are accepted in either casing (and therefore in mixed casing).
    inline bool TryReadStrictVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
            if (Values.Num() != 3)
            {
                return false;
            }

            double Components[3] = {};
            for (int32 Index = 0; Index < 3; ++Index)
            {
                if (!TryReadFiniteJsonNumber(Values[Index], Components[Index]))
                {
                    return false;
                }
            }

            Out = FVector(Components[0], Components[1], Components[2]);
            return true;
        }

        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            if (!TryReadFiniteJsonNumber(FindVectorComponent(Object, TEXT("X"), TEXT("x")), X)
                || !TryReadFiniteJsonNumber(FindVectorComponent(Object, TEXT("Y"), TEXT("y")), Y)
                || !TryReadFiniteJsonNumber(FindVectorComponent(Object, TEXT("Z"), TEXT("z")), Z))
            {
                return false;
            }

            Out = FVector(X, Y, Z);
            return true;
        }

        return false;
    }

    // Load a UPCGGraph by path; on failure, send GRAPH_NOT_FOUND and return null.
    // Caller should bail (return true) when this returns null.
    inline UPCGGraph* LoadGraphOrError(FHandlerContext& Ctx, const FString& Path)
    {
        UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *Path);
        if (!Graph)
        {
            Ctx.SendError(ErrorCodes::ERR_GRAPH_NOT_FOUND,
                FString::Printf(TEXT("Could not load PCG graph: %s"), *Path));
        }
        return Graph;
    }

    // Resolve a node by UPCGNode::GetName() across the graph's user nodes.
    // Implicit input/output nodes are not searched here; callers that need
    // them should use FindNodeByNameIncludingImplicit.
    inline UPCGNode* FindNodeByName(UPCGGraph* Graph, const FString& Name)
    {
        for (UPCGNode* Node : Graph->GetNodes())
        {
            if (Node && Node->GetName() == Name)
            {
                return Node;
            }
        }
        return nullptr;
    }

    // Resolve endpoints exposed by pcg.inspect, including implicit graph nodes.
    inline UPCGNode* FindNodeByNameIncludingImplicit(UPCGGraph* Graph, const FString& Name)
    {
        if (UPCGNode* InputNode = Graph->GetInputNode())
        {
            if (InputNode->GetName() == Name)
            {
                return InputNode;
            }
        }

        if (UPCGNode* OutputNode = Graph->GetOutputNode())
        {
            if (OutputNode->GetName() == Name)
            {
                return OutputNode;
            }
        }

        return FindNodeByName(Graph, Name);
    }

    // Explain an unresolvable /Script/<Module>.<Class> path when <Module> belongs to an
    // engine plugin this project has disabled: sends PLUGIN_DISABLED and returns true.
    // Returns false — sending nothing — when the module maps to no discovered plugin or to
    // an enabled one, leaving the caller's own CLASS_NOT_FOUND as the correct answer.
    //
    // WHY THIS IS A RUNTIME CHECK AND NOT A COMPILE-TIME PROBE. The pcg surface takes engine
    // class paths as parameter VALUES (pcg.add_node's `nodeClass`), so the dependency is
    // neither a namespace nor a method prefix, and IntegrationGates — which keys on exactly
    // those two — cannot express it. A build-time probe cannot express it either: both
    // PinWright.Build.cs's TryAddConditionalModule and this module's own BuildException guard
    // test whether the BUILD MACHINE has the plugin on disk, never whether the CONSUMER
    // PROJECT enabled it. UE 5.8's Procedural Vegetation Editor is the case that separates
    // the two — it ships with every stock install but is "IsExperimentalVersion": true and
    // "EnabledByDefault": false, so an __has_include probe answers "present" on precisely the
    // hosts where /Script/ProceduralVegetation was never loaded. Only IPluginManager, at run
    // time, tells them apart.
    //
    // Adds no header include of the optional plugin and no link dependency on it: this is the
    // linkage-free optional-plugin pattern of AIHandler.cpp's EnsureOptionalPluginEnabled /
    // ResolveOptionalPluginClass, applied one layer deeper than the gate reaches.
    inline bool SendPluginDisabledForScriptPath(FHandlerContext& Ctx, const FString& ScriptPath)
    {
        const FString ScriptPrefix(TEXT("/Script/"));
        if (!ScriptPath.StartsWith(ScriptPrefix))
        {
            return false;
        }

        FString ModuleName = ScriptPath.Mid(ScriptPrefix.Len());
        int32 DotIndex = INDEX_NONE;
        if (ModuleName.FindChar(TEXT('.'), DotIndex))
        {
            ModuleName.LeftInline(DotIndex);
        }
        if (ModuleName.IsEmpty())
        {
            return false;
        }

        const FName ModuleFName(*ModuleName);
        for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
        {
            const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
            const bool bOwnsModule = Descriptor.Modules.ContainsByPredicate(
                [&ModuleFName](const FModuleDescriptor& Module) { return Module.Name == ModuleFName; });
            if (!bOwnsModule || Plugin->IsEnabled())
            {
                continue;
            }

            // Two facts a caller needs beyond the plugin's name: whether enabling it opts the
            // project into experimental engine code, and which other plugins come on with it.
            // Both are read off the descriptor rather than hardcoded, so this generalizes to
            // every optional engine plugin without a table to keep in sync.
            FString Detail;
            if (Descriptor.bIsExperimentalVersion)
            {
                Detail += TEXT(" That plugin is marked experimental.");
            }

            TArray<FString> AlsoEnabled;
            for (const FPluginReferenceDescriptor& Reference : Descriptor.Plugins)
            {
                if (Reference.bEnabled)
                {
                    AlsoEnabled.Add(Reference.Name);
                }
            }
            if (AlsoEnabled.Num() > 0)
            {
                Detail += FString::Printf(TEXT(" Enabling it also enables: %s."),
                    *FString::Join(AlsoEnabled, TEXT(", ")));
            }

            // Same sentence shape as RpcDispatcher's method-level PLUGIN_DISABLED so the two
            // read as one vocabulary.
            Ctx.SendError(ErrorCodes::ERR_PLUGIN_DISABLED,
                FString::Printf(
                    TEXT("Class '%s' is unavailable because the '%s' engine plugin is disabled in this project; enable it and restart the editor.%s"),
                    *ScriptPath, *Plugin->GetName(), *Detail));
            return true;
        }

        return false;
    }
}

#endif // __has_include("PCGGraph.h")
