// Copyright (c) 2026 Alexander Penkin. MIT License.

// Object/property resolution shared by both halves of UtilityPropertyHandler.cpp: the runtime
// module's read verbs (property.get/list, container.*.get/has_key/get_keys/contains) and the
// editor module's mutators (property.set/reset, container.* writes). Implemented in
// PinWrightRuntime/Private/Handlers/Utility/UtilityPropertyHandler.cpp.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Utils/PropertyExport.h"

class FHandlerContext;
class UObject;
class FProperty;
struct FPropertyNotifyTarget;

namespace PropertyHandlerShared
{
    constexpr const TCHAR* ClassCdoDefaultSource = TEXT("class_cdo");
    constexpr const TCHAR* ParentTemplateDefaultSource = TEXT("parent_template");
    constexpr const TCHAR* ParentClassCdoDefaultSource = TEXT("parent_class_cdo");

    struct FResolvedDefaultProperty
    {
        FPropertyExportSource ExportSource = FPropertyExportSource::FromRaw(nullptr);
        FProperty* Property = nullptr;
        FString Source = ClassCdoDefaultSource;
        FString Error;
    };

    // Resolve an object from path or actor name; a Blueprint or class path resolves to its CDO.
    // Returns nullptr when nothing resolves (the caller sends OBJECT_NOT_FOUND).
    PINWRIGHTRUNTIME_API UObject* ResolveObjectForProperty(FHandlerContext& Ctx, const FString& ObjectPath);

    // Resolve a (possibly dotted) property on RootObject. Returns false after sending
    // PROPERTY_NOT_FOUND.
    PINWRIGHTRUNTIME_API bool ResolvePropertyFromObject(FHandlerContext& Ctx, UObject* RootObject,
        const FString& PropertyName, const FString& ObjectPath,
        void*& OutContainer, FProperty*& OutProperty,
        FPropertyNotifyTarget* OutNotifyTarget = nullptr);

    // Actor verification for actors, asset verification for everything else.
    PINWRIGHTRUNTIME_API void AddObjectVerification(FHandlerContext& Ctx, TSharedPtr<FJsonObject>& Result, UObject* Obj);

    PINWRIGHTRUNTIME_API bool TryParseContainerInt32(const TSharedPtr<FJsonValue>& Value,
        int32& OutValue, FString& OutError);

    // The per-property default the editor's reset arrow would use: the parent component
    // template, the parent-class CDO of a Blueprint CDO, else the class CDO.
    PINWRIGHTRUNTIME_API FResolvedDefaultProperty ResolveDefaultPropertyFromObject(
        UObject* RootObject, const FString& PropertyName);

    // property.list's asset.dump hint. asset.dump is editor-only, so the editor module installs
    // the provider at static init; with none installed (game process) the hint is omitted.
    using FDumpHintProvider = FString (*)(const FString& PackageName, bool bIsMap);
    PINWRIGHTRUNTIME_API void SetDumpHintProvider(FDumpHintProvider Provider);
    PINWRIGHTRUNTIME_API FDumpHintProvider GetDumpHintProvider();
}

// ===========================================================================
// Container handler preamble: resolve object + property to array/map/set, sending errors on failure
// ===========================================================================

#define RESOLVE_ARRAY_PROPERTY(HandlerName) \
    FString ObjectPath = Ctx.GetString(TEXT("objectPath")); \
    FString PropertyName = Ctx.GetString(TEXT("propertyName")); \
    if (ObjectPath.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT(#HandlerName " requires objectPath.")); \
        return true; \
    } \
    if (PropertyName.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT(#HandlerName " requires propertyName.")); \
        return true; \
    } \
    UObject* RootObject = PropertyHandlerShared::ResolveObjectForProperty(Ctx, ObjectPath); \
    if (!RootObject) { \
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("Object not found: %s"), *ObjectPath)); \
        return true; \
    } \
    void* TargetContainer = nullptr; \
    FProperty* Property = nullptr; \
    if (!PropertyHandlerShared::ResolvePropertyFromObject(Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property)) \
        return true; \
    FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property); \
    if (!ArrayProp) { \
        Ctx.SendError(TEXT("NOT_AN_ARRAY"), TEXT("Property is not an array.")); \
        return true; \
    }

#define RESOLVE_MAP_PROPERTY(HandlerName) \
    FString ObjectPath = Ctx.GetString(TEXT("objectPath")); \
    FString PropertyName = Ctx.GetString(TEXT("propertyName")); \
    if (ObjectPath.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT(#HandlerName " requires objectPath.")); \
        return true; \
    } \
    if (PropertyName.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT(#HandlerName " requires propertyName.")); \
        return true; \
    } \
    UObject* RootObject = PropertyHandlerShared::ResolveObjectForProperty(Ctx, ObjectPath); \
    if (!RootObject) { \
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("Object not found: %s"), *ObjectPath)); \
        return true; \
    } \
    void* TargetContainer = nullptr; \
    FProperty* Property = nullptr; \
    if (!PropertyHandlerShared::ResolvePropertyFromObject(Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property)) \
        return true; \
    FMapProperty* MapProp = CastField<FMapProperty>(Property); \
    if (!MapProp) { \
        Ctx.SendError(TEXT("NOT_A_MAP"), TEXT("Property is not a map.")); \
        return true; \
    }

#define RESOLVE_SET_PROPERTY(HandlerName) \
    FString ObjectPath = Ctx.GetString(TEXT("objectPath")); \
    FString PropertyName = Ctx.GetString(TEXT("propertyName")); \
    if (ObjectPath.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT(#HandlerName " requires objectPath.")); \
        return true; \
    } \
    if (PropertyName.TrimStartAndEnd().IsEmpty()) { \
        Ctx.SendError(TEXT("INVALID_PROPERTY"), TEXT(#HandlerName " requires propertyName.")); \
        return true; \
    } \
    UObject* RootObject = PropertyHandlerShared::ResolveObjectForProperty(Ctx, ObjectPath); \
    if (!RootObject) { \
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"), FString::Printf(TEXT("Object not found: %s"), *ObjectPath)); \
        return true; \
    } \
    void* TargetContainer = nullptr; \
    FProperty* Property = nullptr; \
    if (!PropertyHandlerShared::ResolvePropertyFromObject(Ctx, RootObject, PropertyName, ObjectPath, TargetContainer, Property)) \
        return true; \
    FSetProperty* SetProp = CastField<FSetProperty>(Property); \
    if (!SetProp) { \
        Ctx.SendError(TEXT("NOT_A_SET"), TEXT("Property is not a set.")); \
        return true; \
    }
