// Copyright (c) 2026 Alexander Penkin. MIT License.

// Reflection / structural metadata utilities for PinWright
#include "Utils/PropertyInspection.h"

#include "UObject/UnrealType.h"

FString GetPropertyCppTypeWithParams(const FProperty* Property)
{
    if (!Property)
    {
        return FString();
    }
    // GetCPPType writes container <K,V>/<T> params into ExtendedTypeText; concat
    // reconstructs the full type. See header for the contract and rationale.
    FString ExtendedTypeText;
    FString BaseType = Property->GetCPPType(&ExtendedTypeText, 0);
    return BaseType + ExtendedTypeText;
}

FProperty* FindPropertyCI(UStruct* Struct, const FString& Name)
{
    if (!Struct)
    {
        return nullptr;
    }
    if (FProperty* Exact = Struct->FindPropertyByName(FName(*Name)))
    {
        return Exact;
    }
    for (TFieldIterator<FProperty> It(Struct); It; ++It)
    {
        if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }
    return nullptr;
}

namespace
{
    // Split a single dotted path segment into its base property name and an
    // optional trailing array subscript. "Keys[2]" -> ("Keys", 2, true);
    // "Asset" -> ("Asset", -1, false). Returns false only on a malformed
    // bracket ("Foo[", "Foo[x]", "Foo[-1]"), with OutError describing the fault.
    bool ParsePathSegment(const FString& Segment, FString& OutName,
                          int32& OutIndex, bool& bOutHasIndex, FString& OutError)
    {
        OutName = Segment;
        OutIndex = INDEX_NONE;
        bOutHasIndex = false;

        int32 OpenBracket = INDEX_NONE;
        if (!Segment.FindChar(TEXT('['), OpenBracket))
        {
            return true;
        }

        if (!Segment.EndsWith(TEXT("]")))
        {
            OutError = FString::Printf(
                TEXT("Malformed array subscript in segment '%s' (expected 'Name[N]')"), *Segment);
            return false;
        }

        OutName = Segment.Left(OpenBracket);
        const FString IndexText =
            Segment.Mid(OpenBracket + 1, Segment.Len() - OpenBracket - 2);
        if (IndexText.IsEmpty() || !IndexText.IsNumeric())
        {
            OutError = FString::Printf(
                TEXT("Array subscript '[%s]' in segment '%s' is not a non-negative integer"),
                *IndexText, *Segment);
            return false;
        }

        OutIndex = FCString::Atoi(*IndexText);
        if (OutIndex < 0)
        {
            OutError = FString::Printf(
                TEXT("Array subscript '[%s]' in segment '%s' is not a non-negative integer"),
                *IndexText, *Segment);
            return false;
        }
        bOutHasIndex = true;
        return true;
    }

    // A whole segment that is a bare non-negative integer ("0", "12") — the
    // dotted-numeric array-index form "SensesConfig.0.Field". Distinct from a
    // trailing "[N]" subscript on a named segment.
    bool IsNumericIndexSegment(const FString& Segment, int32& OutIndex)
    {
        if (Segment.IsEmpty() || !Segment.IsNumeric() || Segment.StartsWith(TEXT("-")))
        {
            return false;
        }
        OutIndex = FCString::Atoi(*Segment);
        return OutIndex >= 0;
    }

    // Step into element Index of the array CurrentProperty describes, held at
    // ArrayContainer. On success CurrentContainer/OutInnerProperty are advanced
    // to address that element (the inner property at element raw ptr); the inner
    // property's offset is 0 so the existing (FProperty*, container) export/import
    // contract addresses the element value directly. Returns false with OutError
    // set on a non-array property or an out-of-range index. The caller decides the
    // inner type scope via DrillIntoArrayElement (which owns CurrentTypeScope from
    // the live instance), so this step deliberately does not set it.
    bool StepIntoArrayElement(FProperty* CurrentProperty, void* ArrayContainer,
                              int32 Index, void*& CurrentContainer,
                              FProperty*& OutInnerProperty, FString& OutError)
    {
        FArrayProperty* ArrayProp = CastField<FArrayProperty>(CurrentProperty);
        if (!ArrayProp)
        {
            OutError = FString::Printf(
                TEXT("Property '%s' of type '%s' is not an array; cannot index with [%d]"),
                *CurrentProperty->GetName(), *CurrentProperty->GetClass()->GetName(), Index);
            return false;
        }

        FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ArrayContainer));
        if (!Helper.IsValidIndex(Index))
        {
            OutError = FString::Printf(
                TEXT("Array index %d out of range on property '%s' (length %d)"),
                Index, *ArrayProp->GetName(), Helper.Num());
            return false;
        }

        CurrentContainer = Helper.GetRawPtr(Index);
        OutInnerProperty = ArrayProp->Inner;
        return true;
    }

    // Outcome of indexing into an array element and deciding what to do next.
    enum class EElementDrill
    {
        Leaf,      // The element is the resolved leaf; OutContainer holds its value ptr.
        Continue,  // Advanced into a struct/object element; resolver loop should continue.
        Error,     // OutError is set.
    };

    // Index into element Index of the array CurrentProperty describes (held at
    // ArrayContainer) and, when more segments follow (bIsLastSegment == false),
    // drill one level in: struct elements set CurrentTypeScope to the element
    // struct, object elements deref to the live instance (null is an error). On a
    // leaf, OutContainerPtr is set to the element value ptr. ElementLabel is woven
    // into traversal errors so the bracket form ("Items[2]") and dotted form
    // ("Items.2") keep distinct wording. SegmentCtx is the trailing "(segment N of
    // M)" suffix shared by every error here.
    // OutHoppedObject reports the instance an object-element deref landed on, and is null
    // for every other outcome: that hop moves the change-notification target, and only the
    // caller knows which segment it happened at.
    EElementDrill DrillIntoArrayElement(int32 Index, const FString& ElementLabel,
                                        const FString& SegmentCtx, bool bIsLastSegment,
                                        void*& CurrentContainer, FProperty*& CurrentProperty,
                                        UStruct*& CurrentTypeScope, void*& OutContainerPtr,
                                        UObject*& OutHoppedObject, FString& OutError)
    {
        OutHoppedObject = nullptr;
        void* ElementContainer = nullptr;
        FProperty* InnerProperty = nullptr;
        if (!StepIntoArrayElement(CurrentProperty, CurrentContainer, Index,
                                  ElementContainer, InnerProperty, OutError))
        {
            return EElementDrill::Error;
        }
        CurrentContainer = ElementContainer;
        CurrentProperty = InnerProperty;

        if (bIsLastSegment)
        {
            OutContainerPtr = CurrentContainer;
            return EElementDrill::Leaf;
        }

        // The inner element must itself be drilled into by the next segment.
        if (FStructProperty* StructProp = CastField<FStructProperty>(CurrentProperty))
        {
            // CurrentContainer already points at the struct element (inner offset 0),
            // so it is the struct value pointer to drill into.
            CurrentTypeScope = StructProp->Struct;
            return EElementDrill::Continue;
        }
        if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(CurrentProperty))
        {
            UObject* NextObject = ObjectProp->GetObjectPropertyValue(CurrentContainer);
            if (!NextObject)
            {
                OutError = FString::Printf(
                    TEXT("Array element %s is a null object %s"), *ElementLabel, *SegmentCtx);
                return EElementDrill::Error;
            }
            CurrentContainer = NextObject;
            CurrentTypeScope = NextObject->GetClass();
            OutHoppedObject = NextObject;
            return EElementDrill::Continue;
        }
        OutError = FString::Printf(
            TEXT("Cannot traverse into array element %s (inner type '%s') %s"),
            *ElementLabel, *CurrentProperty->GetClass()->GetName(), *SegmentCtx);
        return EElementDrill::Error;
    }
}

FProperty* ResolveNestedPropertyPath(UObject* RootObject,
                                     const FString& PropertyPath,
                                     void*& OutContainerPtr,
                                     FString& OutError,
                                     FPropertyNotifyTarget* OutNotifyTarget)
{
    OutError.Empty();
    OutContainerPtr = nullptr;
    if (OutNotifyTarget)
    {
        // Seeded root-relative so a path that fails to resolve still leaves the caller
        // with the target it would have used before object hops were tracked.
        OutNotifyTarget->Object = RootObject;
        OutNotifyTarget->RelativePath = PropertyPath;
    }

    if (!RootObject)
    {
        OutError = TEXT("Root object is null");
        return nullptr;
    }

    if (PropertyPath.IsEmpty())
    {
        OutError = TEXT("Property path is empty");
        return nullptr;
    }

    TArray<FString> PathSegments;
    PropertyPath.ParseIntoArray(PathSegments, TEXT("."), true);

    if (PathSegments.Num() == 0)
    {
        OutError = TEXT("Invalid property path format");
        return nullptr;
    }

    UStruct* CurrentTypeScope = RootObject->GetClass();
    void* CurrentContainer = RootObject;
    FProperty* CurrentProperty = nullptr;

    // The last UObject the walk crosses, and the segment it was crossed at. Only an
    // FObjectProperty hop moves these; a struct hop stays inside the same object's memory
    // and leaves them at RootObject / INDEX_NONE, which reproduces the root-relative
    // target exactly. See FPropertyNotifyTarget in the header.
    UObject* LastCrossedObject = RootObject;
    int32 LastObjectHopSegment = INDEX_NONE;

    // Records the resolved leaf's notification target: the last object crossed, plus the
    // path segments that follow the hop joined back into a path relative to it.
    auto FillNotifyTarget = [&]()
    {
        if (!OutNotifyTarget)
        {
            return;
        }
        OutNotifyTarget->Object = LastCrossedObject;
        if (LastObjectHopSegment == INDEX_NONE)
        {
            OutNotifyTarget->RelativePath = PropertyPath;
            return;
        }
        FString Tail;
        for (int32 Seg = LastObjectHopSegment + 1; Seg < PathSegments.Num(); ++Seg)
        {
            if (!Tail.IsEmpty())
            {
                Tail.AppendChar(TEXT('.'));
            }
            Tail += PathSegments[Seg];
        }
        OutNotifyTarget->RelativePath = Tail;
    };

    for (int32 i = 0; i < PathSegments.Num(); ++i)
    {
        const FString& Segment = PathSegments[i];
        const bool bIsLastSegment = (i == PathSegments.Num() - 1);

        // Dotted-numeric array index ("SensesConfig.0.Field"): a bare integer
        // segment indexes the array property resolved by the PREVIOUS segment.
        int32 DottedIndex = INDEX_NONE;
        if (i > 0 && IsNumericIndexSegment(Segment, DottedIndex))
        {
            if (!CurrentProperty)
            {
                OutError = FString::Printf(
                    TEXT("Numeric index segment '%s' has no preceding array property (segment %d of %d)"),
                    *Segment, i + 1, PathSegments.Num());
                return nullptr;
            }

            // CurrentContainer is the container holding the array property (the
            // previous iteration left it as an intermediate hop's container).
            const FString ElementLabel = FString::Printf(
                TEXT("[%d] of '%s'"), DottedIndex, *CurrentProperty->GetName());
            const FString SegmentCtx =
                FString::Printf(TEXT("(segment %d of %d)"), i + 1, PathSegments.Num());
            UObject* HoppedObject = nullptr;
            switch (DrillIntoArrayElement(DottedIndex, ElementLabel, SegmentCtx, bIsLastSegment,
                                          CurrentContainer, CurrentProperty, CurrentTypeScope,
                                          OutContainerPtr, HoppedObject, OutError))
            {
            case EElementDrill::Leaf:
                FillNotifyTarget();
                return CurrentProperty;
            case EElementDrill::Continue:
                if (HoppedObject)
                {
                    LastCrossedObject = HoppedObject;
                    LastObjectHopSegment = i;
                }
                continue;
            case EElementDrill::Error:    return nullptr;
            }
        }

        // Named segment, optionally carrying a trailing "[N]" subscript.
        FString SegmentName;
        int32 BracketIndex = INDEX_NONE;
        bool bHasBracket = false;
        if (!ParsePathSegment(Segment, SegmentName, BracketIndex, bHasBracket, OutError))
        {
            return nullptr;
        }

        if (!CurrentTypeScope)
        {
            OutError = FString::Printf(
                TEXT("Property '%s' cannot be resolved: preceding segment has no struct/object scope (segment %d of %d)"),
                *SegmentName, i + 1, PathSegments.Num());
            return nullptr;
        }

        CurrentProperty = FindFProperty<FProperty>(CurrentTypeScope, FName(*SegmentName));

        if (!CurrentProperty)
        {
            OutError = FString::Printf(
                TEXT("Property '%s' not found in scope '%s' (segment %d of %d)"),
                *SegmentName, *CurrentTypeScope->GetName(), i + 1, PathSegments.Num());
            return nullptr;
        }

        // If the segment carries "[N]", index into the array element now, before
        // deciding whether this is the leaf.
        if (bHasBracket)
        {
            const FString ElementLabel =
                FString::Printf(TEXT("'%s[%d]'"), *SegmentName, BracketIndex);
            const FString SegmentCtx =
                FString::Printf(TEXT("(segment %d of %d)"), i + 1, PathSegments.Num());
            UObject* HoppedObject = nullptr;
            switch (DrillIntoArrayElement(BracketIndex, ElementLabel, SegmentCtx, bIsLastSegment,
                                          CurrentContainer, CurrentProperty, CurrentTypeScope,
                                          OutContainerPtr, HoppedObject, OutError))
            {
            case EElementDrill::Leaf:
                FillNotifyTarget();
                return CurrentProperty;
            case EElementDrill::Continue:
                if (HoppedObject)
                {
                    LastCrossedObject = HoppedObject;
                    LastObjectHopSegment = i;
                }
                continue;
            case EElementDrill::Error:    return nullptr;
            }
        }

        if (bIsLastSegment)
        {
            OutContainerPtr = CurrentContainer;
            FillNotifyTarget();
            return CurrentProperty;
        }

        // Traverse deeper into a struct/object member. An FArrayProperty here
        // (no inline "[N]") is valid only if the NEXT segment is a numeric index
        // — leave CurrentProperty/CurrentContainer as-is so that branch sees them.
        if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(CurrentProperty))
        {
            UObject* NextObject =
                ObjectProp->GetObjectPropertyValue_InContainer(CurrentContainer);
            if (!NextObject)
            {
                OutError = FString::Printf(
                    TEXT("Object property '%s' is null (segment %d of %d)"), *SegmentName,
                    i + 1, PathSegments.Num());
                return nullptr;
            }
            CurrentContainer = NextObject;
            CurrentTypeScope = NextObject->GetClass();
            LastCrossedObject = NextObject;
            LastObjectHopSegment = i;
        }
        else if (FStructProperty* StructProp = CastField<FStructProperty>(CurrentProperty))
        {
            CurrentContainer =
                StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
            CurrentTypeScope = StructProp->Struct;
        }
        else if (CastField<FArrayProperty>(CurrentProperty))
        {
            // An array reached as an intermediate hop without a bracket: only
            // legal when the next segment supplies the index. Keep CurrentContainer
            // as the array's container so the numeric-index branch can index it.
            int32 PeekIndex = INDEX_NONE;
            if (i + 1 < PathSegments.Num() && IsNumericIndexSegment(PathSegments[i + 1], PeekIndex))
            {
                // CurrentContainer already holds the container for this array
                // property; the next loop iteration's numeric branch consumes it.
                continue;
            }
            OutError = FString::Printf(
                TEXT("Cannot traverse into array property '%s' without an element index "
                     "(use '%s[N]' or '%s.N.<field>') (segment %d of %d)"),
                *SegmentName, *SegmentName, *SegmentName, i + 1, PathSegments.Num());
            return nullptr;
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Cannot traverse into property '%s' of type '%s'"), *SegmentName,
                *CurrentProperty->GetClass()->GetName());
            return nullptr;
        }
    }

    OutError = TEXT("Unexpected end of property path resolution");
    return nullptr;
}

FPropertyNotifyTarget ResolvePropertyNotifyTarget(UObject* RootObject,
                                                  const FString& PropertyPath)
{
    FPropertyNotifyTarget Target;
    Target.Object = RootObject;
    Target.RelativePath = PropertyPath;

    // Only a dotted path can cross an FObjectProperty, so a simple name is answered
    // without a walk.
    if (!RootObject || !PropertyPath.Contains(TEXT(".")))
    {
        return Target;
    }

    void* Container = nullptr;
    FString Error;
    ResolveNestedPropertyPath(RootObject, PropertyPath, Container, Error, &Target);
    return Target;
}

FProperty* ResolvePropertyOnObject(UObject* RootObject,
                                   const FString& PropertyName,
                                   void*& OutContainerPtr,
                                   FString& OutError)
{
    OutError.Empty();
    OutContainerPtr = nullptr;

    if (!RootObject)
    {
        OutError = TEXT("Root object is null");
        return nullptr;
    }

    if (PropertyName.Contains(TEXT(".")))
    {
        FProperty* Resolved =
            ResolveNestedPropertyPath(RootObject, PropertyName, OutContainerPtr, OutError);
        if (!Resolved || !OutContainerPtr)
        {
            OutContainerPtr = nullptr;
            if (OutError.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("Failed to resolve nested property path '%s'"), *PropertyName);
            }
            return nullptr;
        }
        return Resolved;
    }

    FProperty* Resolved = RootObject->GetClass()->FindPropertyByName(*PropertyName);
    if (!Resolved)
    {
        OutError = FString::Printf(TEXT("Property '%s' not found"), *PropertyName);
        return nullptr;
    }
    OutContainerPtr = RootObject;
    return Resolved;
}

TArray<FString> DecodePropertyFlags(EPropertyFlags Flags)
{
    TArray<FString> Out;

    // CPF_Edit edit-visibility family. Order of refinement matters: VisibleAnywhere
    // (CPF_Edit + CPF_EditConst) takes precedence; otherwise CPF_DisableEditOnInstance
    // means EditDefaultsOnly; otherwise plain EditAnywhere.
    if (Flags & CPF_Edit)
    {
        if (Flags & CPF_EditConst)
        {
            Out.Add(TEXT("VisibleAnywhere"));
        }
        else if (Flags & CPF_DisableEditOnInstance)
        {
            Out.Add(TEXT("EditDefaultsOnly"));
        }
        else
        {
            Out.Add(TEXT("EditAnywhere"));
        }
    }

    if (Flags & CPF_BlueprintVisible)
    {
        if (Flags & CPF_BlueprintReadOnly)
        {
            Out.Add(TEXT("BlueprintReadOnly"));
        }
        else
        {
            Out.Add(TEXT("BlueprintReadWrite"));
        }
    }

    if (Flags & CPF_BlueprintAssignable) { Out.Add(TEXT("BlueprintAssignable")); }
    if (Flags & CPF_BlueprintCallable)   { Out.Add(TEXT("BlueprintCallable")); }
    if (Flags & CPF_Net)                 { Out.Add(TEXT("Replicated")); }
    if (Flags & CPF_RepNotify)           { Out.Add(TEXT("RepNotify")); }
    if (Flags & CPF_Transient)           { Out.Add(TEXT("Transient")); }
    if (Flags & CPF_Config)              { Out.Add(TEXT("Config")); }
    if (Flags & CPF_SaveGame)            { Out.Add(TEXT("SaveGame")); }
    if (Flags & CPF_InstancedReference)  { Out.Add(TEXT("Instanced")); }

    // EditConst is also surfaced as a standalone tag (independent of CPF_Edit) so
    // const-but-not-edit-visible properties still announce their constness.
    if ((Flags & CPF_EditConst) && !(Flags & CPF_Edit))
    {
        Out.Add(TEXT("EditConst"));
    }

    return Out;
}

TArray<FString> DecodeFunctionFlags(EFunctionFlags Flags)
{
    TArray<FString> Out;

    if (Flags & FUNC_BlueprintCallable) { Out.Add(TEXT("BlueprintCallable")); }
    if (Flags & FUNC_BlueprintPure)     { Out.Add(TEXT("BlueprintPure")); }
    if (Flags & FUNC_BlueprintEvent)    { Out.Add(TEXT("BlueprintEvent")); }

    // Net role: only one of Server/Client/NetMulticast applies; classify and then
    // attach reliability tag.
    if (Flags & FUNC_Net)
    {
        if (Flags & FUNC_NetServer)         { Out.Add(TEXT("Server")); }
        else if (Flags & FUNC_NetClient)    { Out.Add(TEXT("Client")); }
        else if (Flags & FUNC_NetMulticast) { Out.Add(TEXT("NetMulticast")); }

        Out.Add((Flags & FUNC_NetReliable) ? TEXT("Reliable") : TEXT("Unreliable"));
    }

    if (Flags & FUNC_Exec)      { Out.Add(TEXT("Exec")); }
    if (Flags & FUNC_Static)    { Out.Add(TEXT("Static")); }
    if (Flags & FUNC_Const)     { Out.Add(TEXT("Const")); }
    if (Flags & FUNC_Public)    { Out.Add(TEXT("Public")); }
    if (Flags & FUNC_Protected) { Out.Add(TEXT("Protected")); }
    if (Flags & FUNC_Private)   { Out.Add(TEXT("Private")); }

    return Out;
}

TSharedPtr<FJsonObject> PropertyToInspectJson(FProperty* Property)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Property)
    {
        return Obj;
    }
    Obj->SetStringField(TEXT("name"), Property->GetName());
    Obj->SetStringField(TEXT("cppType"), GetPropertyCppTypeWithParams(Property));

    TArray<TSharedPtr<FJsonValue>> FlagValues;
    for (const FString& Tag : DecodePropertyFlags(Property->PropertyFlags))
    {
        FlagValues.Add(MakeShared<FJsonValueString>(Tag));
    }
    Obj->SetArrayField(TEXT("flags"), FlagValues);
    return Obj;
}

TSharedPtr<FJsonObject> FunctionToInspectJson(UFunction* Function)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Function)
    {
        return Obj;
    }
    Obj->SetStringField(TEXT("name"), Function->GetName());

    FString ReturnType = TEXT("void");
    TArray<TSharedPtr<FJsonValue>> ParamValues;

    // Walk every reflected FProperty child; UFunction param order matches declaration.
    for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
    {
        FProperty* Param = *ParamIt;
        if (!Param)
        {
            continue;
        }

        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("name"), Param->GetName());
        ParamObj->SetStringField(TEXT("cppType"), GetPropertyCppTypeWithParams(Param));

        const EPropertyFlags PFlags = Param->PropertyFlags;
        FString Direction;
        if (PFlags & CPF_ReturnParm)
        {
            Direction = TEXT("return");
            ReturnType = GetPropertyCppTypeWithParams(Param);
        }
        else if (PFlags & CPF_OutParm)
        {
            Direction = TEXT("out");
        }
        else
        {
            Direction = TEXT("in");
        }
        ParamObj->SetStringField(TEXT("direction"), Direction);
        ParamValues.Add(MakeShared<FJsonValueObject>(ParamObj));
    }

    Obj->SetStringField(TEXT("returnType"), ReturnType);
    Obj->SetArrayField(TEXT("params"), ParamValues);

    TArray<TSharedPtr<FJsonValue>> FlagValues;
    for (const FString& Tag : DecodeFunctionFlags(Function->FunctionFlags))
    {
        FlagValues.Add(MakeShared<FJsonValueString>(Tag));
    }
    Obj->SetArrayField(TEXT("flags"), FlagValues);
    return Obj;
}
