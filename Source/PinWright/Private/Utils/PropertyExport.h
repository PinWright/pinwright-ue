// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"
#include "UObject/UnrealType.h"

struct FPropertyExportResult
{
    TSharedPtr<FJsonValue> Value;
    bool bSupported = false;

    FPropertyExportResult() = default;

    FPropertyExportResult(TSharedPtr<FJsonValue> InValue)
        : Value(MoveTemp(InValue))
        , bSupported(Value.IsValid())
    {
    }

    template <typename TJsonValueType,
        typename TEnableIf<TIsDerivedFrom<TJsonValueType, FJsonValue>::Value, int>::Type = 0>
    FPropertyExportResult(TSharedPtr<TJsonValueType> InValue)
        : Value(MoveTemp(InValue))
        , bSupported(Value.IsValid())
    {
    }

    template <typename TJsonValueType,
        typename TEnableIf<TIsDerivedFrom<TJsonValueType, FJsonValue>::Value, int>::Type = 0>
    FPropertyExportResult(TSharedRef<TJsonValueType> InValue)
        : Value(MoveTemp(InValue))
        , bSupported(true)
    {
    }

    static FPropertyExportResult Supported(TSharedPtr<FJsonValue> InValue)
    {
        FPropertyExportResult Result;
        Result.Value = MoveTemp(InValue);
        Result.bSupported = true;
        return Result;
    }
};

// Identifies whether a property container is raw storage or an explicitly owned
// UObject. Typed UObject pointers select the owned constructor; void* and nullptr
// select the raw constructor without ambiguous pointer overloads.
struct FPropertyExportSource
{
    FPropertyExportSource(void* InContainer)
        : Container(InContainer)
    {
    }

    template <typename TObjectType,
        typename TEnableIf<TIsDerivedFrom<TObjectType, UObject>::Value, int>::Type = 0>
    FPropertyExportSource(TObjectType* InOwnerObject)
        : Container(InOwnerObject)
        , OwnerObject(InOwnerObject)
    {
    }

    static FPropertyExportSource FromRaw(void* InContainer)
    {
        return FPropertyExportSource(InContainer, nullptr);
    }

    static FPropertyExportSource FromObject(UObject* InOwnerObject)
    {
        return FPropertyExportSource(InOwnerObject, InOwnerObject);
    }

    // Use the candidate owner only when it is the resolved property container.
    // Struct and collection addresses therefore remain raw by construction.
    static FPropertyExportSource FromResolvedContainer(
        void* InContainer,
        UObject* CandidateOwnerObject);

    void* GetContainer() const
    {
        return Container;
    }

    UObject* GetObject() const
    {
        return OwnerObject;
    }

    // Recursive value storage is not itself a UObject, but it keeps the verified
    // owner provenance of the source it came from. A raw source therefore stays
    // raw, while a collection or struct reached from FromObject retains ownership.
    FPropertyExportSource ForNestedContainer(void* InContainer) const
    {
        return FPropertyExportSource(InContainer, OwnerObject, bUnsupportedMarkers);
    }

    // Dump-style export: a member the exporter cannot decompose becomes an inline
    // `_kind: unsupported` marker in place, so the rest of the enclosing struct,
    // array, map, or set still carries its real values. Strict export leaves this
    // off and rejects the whole shape instead.
    FPropertyExportSource WithUnsupportedMarkers() const
    {
        return FPropertyExportSource(Container, OwnerObject, true);
    }

    bool AllowsUnsupportedMarkers() const
    {
        return bUnsupportedMarkers;
    }

private:
    FPropertyExportSource(void* InContainer, UObject* InOwnerObject, bool bInUnsupportedMarkers = false)
        : Container(InContainer)
        , OwnerObject(InOwnerObject)
        , bUnsupportedMarkers(bInUnsupportedMarkers)
    {
    }

    void* Container = nullptr;
    UObject* OwnerObject = nullptr;
    bool bUnsupportedMarkers = false;
};

// A source created with FromRaw has no UObject owner, including throughout
// recursive struct, optional, and collection storage. Object references from it
// serialize as paths/null and never run ownership or SCS-template expansion.
// Recursive storage reached from FromObject retains that verified owner and may
// expand instanced objects and owned components.
PINWRIGHT_API TSharedPtr<FJsonValue> ExportPropertyToJsonValue(
    FPropertyExportSource Source, FProperty* Property);

// Strict export is for RPC response slots: unlike dump-style export, it reports
// unsupported shapes instead of accepting the legacy `_kind: unsupported` marker.
PINWRIGHT_API FPropertyExportResult ExportPropertyToJsonValueStrict(
    FPropertyExportSource Source, FProperty* Property);

// Shared sparse collect→sort→diff→oversized-placeholder skeleton used by both the
// instanced-subobject expansion (PropertyExport.cpp) and the actor.describe sparse
// diff (PropertyDiff.cpp's BuildSparsePropertyDiffJson). Walks Instance's edit/BP-visible
// properties (skipping CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated |
// CPF_SkipSerialization | ExtraSkipFlags, non-semantic derived fields, and any name
// in SkipProperties), sorts by name, diffs each against ResolvedBaseline
// (PPF_DeepComparison, guarded by class ownership), drops equal values, routes oversized
// fields through BuildOmissionPlaceholder, and emits the remainder via the EmitLeaf
// callback. Callers resolve their own baseline (archetype/CDO fallback) before calling.
PINWRIGHT_API TSharedPtr<FJsonObject> BuildSparseFieldDiffJson(
    UObject* Instance,
    UObject* ResolvedBaseline,
    const TSet<FName>& SkipProperties,
    EPropertyFlags ExtraSkipFlags,
    TFunctionRef<TSharedPtr<FJsonValue>(FProperty*)> EmitLeaf);

PINWRIGHT_API TSharedPtr<FJsonObject> ExportPropertyToJsonValueWithInheritance(
    UObject* ChildContainer, UObject* ParentContainer, FProperty* Property, UClass* AssetClass);

PINWRIGHT_API TSharedPtr<FJsonObject> BuildClassPropertyJson(UObject* CDO, UObject* ParentCDO);

// Single source of truth for BuildClassPropertyJson's property filter: true when the
// property earns a key in the output — its exported value, or a placeholder when it is
// on the oversized allowlist. False for the noise the dump deliberately drops:
// UTexture::Source, regenerated non-semantic fields, compiler-managed UUserWidget flags,
// and anything UE itself refuses to serialize (CPF_Transient | CPF_DuplicateTransient |
// CPF_Deprecated | CPF_SkipSerialization). Callers that enumerate the expected key set
// (tests, consumers auditing coverage) must use this instead of re-deriving the filter.
// Note: with a non-null ParentCDO, BuildClassPropertyJson additionally drops properties
// whose value matches the parent — a per-instance decision this predicate cannot make.
PINWRIGHT_API bool ShouldEmitClassDumpProperty(const FProperty* Property);

// Returns true for persisted-but-derived fields whose values are regenerated by the
// editor and therefore do not belong in the source-controlled properties.json mirror.
// Matches the declaring owner type and property name together; unrelated properties
// with the same name remain eligible for export.
PINWRIGHT_API bool ShouldSkipNonSemanticDumpProperty(const FProperty* Property);

// Known-huge property descriptor returned by IsKnownOversizedProperty for placeholder emission.
struct FOmissionReason
{
    const TCHAR* TypeStr = nullptr;
    const TCHAR* SummaryFmt = nullptr; // optional %d substituted with TArray element count
};

// Returns non-null when (OwnerClass, PropertyName) is in the oversized-skip allowlist
// (payloads regularly exceeding the LLM-read budget). Owner classes are resolved lazily
// via FindObject<UClass> so disabled-plugin classes (e.g. Synthesis) simply miss instead
// of hard-linking.
const FOmissionReason* IsKnownOversizedProperty(const FProperty* Property);

// Build a structured placeholder JSON object for a property elided by IsKnownOversizedProperty.
// Shape: {"type": "<TypeStr>", "$omitted": "<summary>", "$reason": "exceeds-llm-budget"}.
// ContainerPtr feeds the optional %d element-count substitution for FArrayProperty entries.
TSharedPtr<FJsonObject> BuildOmissionPlaceholder(
    FProperty* Property, const void* ContainerPtr, const FOmissionReason& Reason);

// Walks Asset's properties (including container inners — arrays, maps, sets) and emits
// every soft reference whose target type IsChildOf(UWorld). Used to surface map relationships
// from data assets that store maps as TSoftObjectPtr<UWorld> or TArray<TSoftObjectPtr<UWorld>>.
// Returns nullptr when no qualifying refs are found so AddJsonFile skips the sidecar.
PINWRIGHT_API TSharedPtr<FJsonObject> BuildMapReferencesJson(UObject* Asset);

// Drops BuildMapReferencesJson's per-class FProperty* cache. Mandatory before any
// garbage collection that may free Blueprint-generated classes — the cache holds raw
// field pointers that the collect invalidates without notifying anyone.
PINWRIGHT_API void ClearSoftWorldPropertyCache();
