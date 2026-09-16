// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UWidgetBlueprint;
class UWidget;
struct FWidgetGeometryResult;

namespace WidgetXmlExporter
{
    struct FGeomContext
    {
        bool bEnabled   = false;
        bool bAmbiguous = false;
        const FWidgetGeometryResult* Result = nullptr;
        const TMap<FName, UWidget*>* NameIndex = nullptr;
    };

    // Result of BuildWidgetTreeXmlWithDiagnostic — distinguishes "genuinely empty
    // by design" (no RootWidget on a native-tree or inherited-only widget) from
    // anomalies that should be reported as diagnostics.
    struct FWidgetTreeXmlResult
    {
        FString Xml;
        bool    bEmptyByDesign = false;
        FString Reason;
    };

    struct FWidgetTreeRootResolution
    {
        UWidget* StartWidget = nullptr;
        bool bEmptyByDesign = false;
        FString Reason;
        FString InheritedFromPath;
    };

    TMap<FString, TArray<TPair<FString, FString>>> BuildBindingMap(UWidgetBlueprint* WidgetBlueprint);

    PINWRIGHT_API FWidgetTreeRootResolution ResolveWidgetTreeRoot(
        UWidgetBlueprint* WidgetBlueprint);

    PINWRIGHT_API TArray<TPair<FString, FString>> CollectOverriddenAttributes(
        UObject* Instance,
        bool bIncludeDefaults,
        const FString& Prefix = FString(),
        const TSet<FString>* PropertyNameSkipSet = nullptr);

    PINWRIGHT_API FString BuildXmlString(
        UWidget* Widget,
        int32 Indent,
        bool bIncludeDefaults,
        const TMap<FString, TArray<TPair<FString, FString>>>& WidgetBindings,
        const TMap<FName, FGuid>* VariableGuidMap,
        int32& WidgetCount,
        const FGeomContext& Geom,
        bool bIsRoot,
        const TArray<TPair<FString, FString>>& ExtraRootAttrs,
        bool bOmitSlotChain = false);

    // Returns a rich result distinguishing null-WBP, null-WidgetTree, and
    // null-RootWidget cases so callers can emit diagnostics rather than silently
    // dropping tree.xml.
    PINWRIGHT_API FWidgetTreeXmlResult BuildWidgetTreeXmlWithDiagnostic(
        UWidgetBlueprint* WidgetBlueprint, bool bIncludeDefaults, bool bOmitSlotChain = false);

    // Back-compat overload — delegates to BuildWidgetTreeXmlWithDiagnostic and
    // returns only the Xml field. Existing callers (tests, AssetDumpBuilder) keep
    // working unchanged.
    PINWRIGHT_API FString BuildWidgetTreeXml(UWidgetBlueprint* WidgetBlueprint, bool bIncludeDefaults, bool bOmitSlotChain = false);
}
