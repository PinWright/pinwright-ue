// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

// Case-insensitive property lookup on a UStruct. Native FindPropertyByName
// is already case-insensitive via FName; the TFieldIterator fallback guards
// against rare paths (e.g. native name collisions) where the fast path misses.
FProperty* FindPropertyCI(UStruct* Struct, const FString& Name);

// Where a change notification for a resolved path has to be dispatched, and the path
// relative to that object.
//
// A dotted path whose segment is an FObjectProperty hops onto a DIFFERENT UObject, and
// the leaf then lives in THAT object's memory. It is that object's
// PostEditChangeProperty override that recomputes from the leaf, so an event addressed
// to the object the path started from reaches nothing no matter how well-formed it is
// (B-property-set-object-hop-notification-noop: a PCG node was notified for a write that
// landed on its settings sub-object, and the settings delegate that regenerates the graph
// never fired). A path that crosses no FObjectProperty reports RootObject and the path
// unchanged, so struct hops and single-segment names keep exactly their RootObject-relative
// behaviour.
struct FPropertyNotifyTarget
{
    // The last UObject the walk crossed; RootObject when it crossed none.
    UObject* Object = nullptr;
    // The tail of the path relative to Object. "SettingsInterface.LowerBound" resolved
    // from a node yields "LowerBound" against the settings object; a path with no object
    // hop yields the input path unchanged.
    FString RelativePath;
};

// OutNotifyTarget, when supplied, receives the notification target described above. It is
// seeded with {RootObject, PropertyPath} before the walk starts and updated only when the
// walk resolves a leaf, so a failed resolution leaves the caller with the root-relative
// fallback rather than a half-written target.
FProperty* ResolveNestedPropertyPath(UObject* RootObject,
                                     const FString& PropertyPath,
                                     void*& OutContainerPtr,
                                     FString& OutError,
                                     FPropertyNotifyTarget* OutNotifyTarget = nullptr);

// The object a change notification for PropertyPath must be dispatched to, plus the path
// relative to it. For callers that have already resolved the leaf and need only the
// notification target; it re-walks the path (a handful of FindFProperty lookups) through
// the SAME resolver rather than re-deriving the traversal rules. Returns
// {RootObject, PropertyPath} for a simple name and for a path that does not resolve, so a
// caller always has something to notify.
FPropertyNotifyTarget ResolvePropertyNotifyTarget(UObject* RootObject,
                                                  const FString& PropertyPath);

// Resolve a property by name on RootObject, dispatching once on whether the name
// is a dotted nested path or a simple name:
//   - A dotted name ("BodyInstance.CollisionEnabled") walks into the struct/object
//     member via ResolveNestedPropertyPath and yields the leaf FProperty plus the
//     container that holds it.
//   - A simple name does a FindPropertyByName on RootObject's class, with the
//     container being RootObject itself.
// Returns the resolved FProperty (and sets OutContainerPtr) on success; returns
// nullptr with OutError populated on failure. This is the single shared dispatch
// primitive that the various property.* handlers wrap with their own error codes.
// PINWRIGHT_API because the gated integration sub-modules link it too: PinWrightPCG's
// pcg.set_node_property resolves against a UPCGSettings object through this, rather than
// carrying a second copy of the dotted-path traversal rules.
PINWRIGHT_API FProperty* ResolvePropertyOnObject(UObject* RootObject,
                                   const FString& PropertyName,
                                   void*& OutContainerPtr,
                                   FString& OutError);

// Build the FProperty C++ type string WITH its templated parameter types intact.
// FProperty::GetCPPType() with no out-param returns the bare container token for
// containers ("TMap"/"TArray"/"TSet") — the <K,V>/<T>/<E> parameter types are only
// emitted into the ExtendedTypeText out-param. This helper passes that out-param and
// concatenates, so a map renders as "TMap<FName,FIKRetargetPose>", an array as
// "TArray<FVector>", a set as "TSet<FName>". Non-container properties write nothing to
// the out-param, so for them this equals the plain GetCPPType().
PINWRIGHT_API FString GetPropertyCppTypeWithParams(const FProperty* Property);

// Decode an EPropertyFlags bitfield into UE-style string tags (e.g. "EditAnywhere",
// "BlueprintReadWrite", "Transient"). Refines CPF_Edit via
// CPF_DisableEditOnInstance/CPF_EditConst into EditDefaultsOnly/VisibleAnywhere, and
// CPF_BlueprintVisible via CPF_BlueprintReadOnly into the Read{Only,Write} variants.
PINWRIGHT_API TArray<FString> DecodePropertyFlags(EPropertyFlags Flags);

// Decode an EFunctionFlags bitfield into UE-style string tags (e.g. "BlueprintCallable",
// "Server", "Reliable").
PINWRIGHT_API TArray<FString> DecodeFunctionFlags(EFunctionFlags Flags);

// Build a JSON object describing a single FProperty:
// { name, cppType, flags: [string...] }.
PINWRIGHT_API TSharedPtr<FJsonObject> PropertyToInspectJson(FProperty* Property);

// Build a JSON object describing a single UFunction:
// { name, returnType, params: [{name, cppType, direction}], flags: [string...] }.
// Parameter direction is one of "in" / "out" / "return"; a CPF_ReturnParm parameter
// also populates returnType. returnType defaults to "void" when no return parm exists.
PINWRIGHT_API TSharedPtr<FJsonObject> FunctionToInspectJson(UFunction* Function);
