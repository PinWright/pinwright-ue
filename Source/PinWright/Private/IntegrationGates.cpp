// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "IntegrationGates.h"

#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightIntegrations, Log, All);

namespace IntegrationGates
{
    namespace
    {
        struct FIntegrationEntry
        {
            const TCHAR* PluginName;     // owning engine plugin gating the integration
            const TCHAR* ModuleName;     // LoadingPhase=None sub-module to load
            const TCHAR* ShortName;      // greppable log label
            const TCHAR* NamespaceSlug;  // exclusive wiki namespace; empty when shared
            const TCHAR* MethodPrefix;   // dotted method prefix owned by the sub-module
        };

        const FIntegrationEntry GIntegrations[] =
        {
            { TEXT("GeometryScripting"), TEXT("PinWrightGeometry"),   TEXT("geometry"),    TEXT("geometry"),    TEXT("geometry.") },
            // PinWrightGeometry also owns the `model` namespace (the .pwmodel compiler runs
            // on the same Geometry Script surface), so it takes a second row. Without it,
            // model.* on a host with GeometryScripting disabled reports UNKNOWN_ACTION -
            // "no such verb" - instead of PLUGIN_DISABLED naming the plugin to enable.
            { TEXT("GeometryScripting"), TEXT("PinWrightGeometry"),   TEXT("model"),       TEXT("model"),       TEXT("model.") },
            { TEXT("PCG"),               TEXT("PinWrightPCG"),        TEXT("pcg"),         TEXT("pcg"),         TEXT("pcg.") },
            { TEXT("Chooser"),           TEXT("PinWrightChooser"),    TEXT("chooser"),     TEXT("chooser"),     TEXT("chooser.") },
            { TEXT("PoseSearch"),        TEXT("PinWrightPoseSearch"), TEXT("pose_search"), TEXT("pose_search"), TEXT("pose_search.") },
            // PinWrightCommonUI registers three method families in the shared
            // "ui" namespace, so it owns one row per prefix (module load is
            // idempotent; log labels are deduplicated).
            { TEXT("CommonUI"),          TEXT("PinWrightCommonUI"),   TEXT("ui"),          TEXT(""),            TEXT("ui.activatable_") },
            { TEXT("CommonUI"),          TEXT("PinWrightCommonUI"),   TEXT("ui"),          TEXT(""),            TEXT("ui.list_stack_widgets") },
            { TEXT("CommonUI"),          TEXT("PinWrightCommonUI"),   TEXT("ui"),          TEXT(""),            TEXT("ui.get_active_widget") },
        };

        // Indices into GIntegrations whose owning plugin was absent or disabled
        // at init. Written once on the game thread by LoadEnabledIntegrations,
        // read-only afterwards.
        TArray<int32> GSkippedIndices;
    }

    void LoadEnabledIntegrations()
    {
        GSkippedIndices.Reset();

        TArray<FString> Loaded;
        TArray<FString> Skipped;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(GIntegrations); ++Index)
        {
            const FIntegrationEntry& Entry = GIntegrations[Index];
            const TSharedPtr<IPlugin> Plugin =
                IPluginManager::Get().FindPlugin(Entry.PluginName);
            if (Plugin.IsValid() && Plugin->IsEnabled())
            {
                if (FModuleManager::Get().LoadModulePtr<IModuleInterface>(Entry.ModuleName))
                {
                    Loaded.AddUnique(Entry.ShortName);
                }
                else
                {
                    // Plugin enabled but the sub-module failed to load — not a
                    // "plugin disabled" condition, so it is NOT recorded as
                    // skipped; its methods will report UNKNOWN_ACTION.
                    UE_LOG(LogPinWrightIntegrations, Warning,
                           TEXT("Integration module %s failed to load although the %s plugin is enabled."),
                           Entry.ModuleName, Entry.PluginName);
                }
                continue;
            }
            GSkippedIndices.Add(Index);
            Skipped.AddUnique(FString::Printf(TEXT("%s(%s)"), Entry.ShortName, Entry.PluginName));
        }

        UE_LOG(LogPinWrightIntegrations, Log,
               TEXT("PinWright integrations: loaded=[%s] skipped=[%s]"),
               *FString::Join(Loaded, TEXT(",")), *FString::Join(Skipped, TEXT(",")));
    }

    FString FindSkippedByMethod(const FString& Method)
    {
        for (const int32 Index : GSkippedIndices)
        {
            if (Method.StartsWith(GIntegrations[Index].MethodPrefix))
            {
                return GIntegrations[Index].PluginName;
            }
        }
        return FString();
    }

    FString FindSkippedByNamespace(const FString& Slug)
    {
        for (const int32 Index : GSkippedIndices)
        {
            const TCHAR* NamespaceSlug = GIntegrations[Index].NamespaceSlug;
            if (NamespaceSlug[0] != TEXT('\0') && Slug == NamespaceSlug)
            {
                return GIntegrations[Index].PluginName;
            }
        }
        return FString();
    }
}
