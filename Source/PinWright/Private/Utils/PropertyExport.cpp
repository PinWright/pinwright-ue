// Copyright (c) 2026 Alexander Penkin. MIT License.

// Property-to-JSON export utilities for PinWright
#include "Utils/PropertyExport.h"
#include "Utils/PropertyInspection.h"
#include "Utils/SortedJsonWriter.h"

#include "Animation/AnimCurveTypes.h"
#include "Components/ActorComponent.h"
#include "Containers/ScriptArray.h"
#include "UObject/FieldPathProperty.h"
// UStruct::GetOutermost() yields UPackage*, dereferenced below for GetFName().
#include "UObject/Package.h"
#include "UObject/ScriptInterface.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
// Engine version macros (ENGINE_MAJOR_VERSION/ENGINE_MINOR_VERSION) for the
// version-conditional FOptionalProperty include below.
#include "Runtime/Launch/Resources/Version.h"
#include "Compat/EngineVersionCompat.h"
// FOptionalProperty arrived in UE 5.5 (PropertyOptional.h). Guarded so the plugin
// continues to compile on 5.4 where the header doesn't exist.
#if __has_include("UObject/PropertyOptional.h") && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    #include "UObject/PropertyOptional.h"
    #define PINWRIGHT_HAS_OPTIONAL_PROPERTY 1
#else
    #define PINWRIGHT_HAS_OPTIONAL_PROPERTY 0
#endif
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
// FInstancedStruct is a type-erased struct wrapper (a UScriptStruct* plus a heap
// memory block) with no reflected payload UPROPERTYs, so the generic field-iterator
// walk in StructToJsonValue yields {} for it. Including the header lets the walker
// detect it and recurse into the inner script struct via GetScriptStruct()/GetMemory().
// EARG_HAS_INSTANCED_STRUCT resolves the version-straddling header relocation.
#include "Compat/InstancedStructCompat.h"

namespace
{
    bool IsOwnedComponentSubobjectForSerialization(const UObject* OwnerObject, const UActorComponent* Component)
    {
        if (!OwnerObject || !Component)
        {
            return false;
        }
        if (Component->IsIn(OwnerObject) || Component->GetOwner() == OwnerObject)
        {
            return true;
        }
        UClass* OwnerClass = OwnerObject->GetClass();
        return Component->IsTemplate()
            && OwnerClass
            && Component->GetTypedOuter<UClass>() == OwnerClass;
    }

    // BP-added (SCS-tree) component variables never get populated on the CDO —
    // the BlueprintGeneratedClass installs them into the actor instance at
    // construction time. Reading the UPROPERTY through GetObjectPropertyValue
    // on the CDO therefore yields null. This helper walks the BPGC chain,
    // finds the matching SCS node by InternalVariableName (== Property->GetFName()),
    // and returns the ICH-aware archetype template so the property dumper can
    // recurse into the component's authored state.
    UActorComponent* ResolveSCSComponentTemplate(UObject* Container, FObjectPropertyBase* Property)
    {
        if (!Container || !Property)
        {
            return nullptr;
        }
        if (!Property->PropertyClass || !Property->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
        {
            return nullptr;
        }
        UClass* LeafClass = Container->GetClass();
        if (!Cast<UBlueprintGeneratedClass>(LeafClass))
        {
            return nullptr;
        }
        const FName VarName = Property->GetFName();
        for (UClass* Class = LeafClass; Class; Class = Class->GetSuperClass())
        {
            UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class);
            if (!BPGC || !BPGC->SimpleConstructionScript)
            {
                continue;
            }
            if (USCS_Node* Node = BPGC->SimpleConstructionScript->FindSCSNode(VarName))
            {
                return Cast<UActorComponent>(Node->GetActualComponentTemplate(Cast<UBlueprintGeneratedClass>(LeafClass)));
            }
        }
        return nullptr;
    }

    bool ShouldExpandObjectPropertyValue(
        const FObjectPropertyBase* Property,
        const UObject* OwnerObject,
        UObject* Value)
    {
        if (!Property || !Value)
        {
            return false;
        }
        if (!OwnerObject)
        {
            return false;
        }
        // UActorComponent is UCLASS(DefaultToInstanced), so UHT auto-stamps
        // CPF_InstancedReference onto every UPROPERTY that references a
        // component subclass — even plain UPROPERTY() pointers holding an
        // external component that the host does not own. Trusting that flag
        // for component-typed properties would recurse into external refs
        // that should stay path-shaped, so component properties route
        // through the ownership check instead.
        const UActorComponent* Component = Cast<UActorComponent>(Value);
        const bool bIsComponentProperty = Component
            && Property->PropertyClass
            && Property->PropertyClass->IsChildOf(UActorComponent::StaticClass());
        if (!bIsComponentProperty)
        {
            return Property->HasAnyPropertyFlags(CPF_PersistentInstance | CPF_InstancedReference);
        }
        UClass* OwnerClass = Property->GetOwnerClass();
        if (!OwnerClass)
        {
            return false;
        }
        return OwnerObject->IsA(OwnerClass)
            && IsOwnedComponentSubobjectForSerialization(OwnerObject, Component);
    }

    UObject* GetObjectPropertyValueFromContainer(
        const FObjectPropertyBase* Property,
        const void* TargetContainer)
    {
        const void* ValueAddress = Property->ContainerPtrToValuePtr<void>(TargetContainer);
        return Property->GetObjectPropertyValue(ValueAddress);
    }

    // Build a typed-marker JSON object: {"_kind": "<TypeName>", "value": "<ExportText>"}.
    // Used for supported property kinds we deliberately don't decompose (field
    // paths and delegates) so the consumer can branch on `_kind` instead of
    // trying to parse a string blob.
    TSharedPtr<FJsonValue> MakeTypedMarker(const TCHAR* Kind, const FString& ExportText)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("_kind"), Kind);
        Obj->SetStringField(TEXT("value"), ExportText);
        return MakeShared<FJsonValueObject>(Obj);
    }

    TSharedPtr<FJsonObject> MakeDelegateBindingObject(const UObject* Object, const FName FunctionName)
    {
        TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
        if (Object)
        {
            Binding->SetStringField(TEXT("object"), Object->GetPathName());
        }
        else
        {
            Binding->SetField(TEXT("object"), MakeShared<FJsonValueNull>());
        }
        if (!FunctionName.IsNone())
        {
            Binding->SetStringField(TEXT("function"), FunctionName.ToString());
        }
        else
        {
            Binding->SetField(TEXT("function"), MakeShared<FJsonValueNull>());
        }
        return Binding;
    }

    void AddParsedDelegateExportBindings(const FString& Exported, TArray<TSharedPtr<FJsonValue>>& Bindings)
    {
        FString Inner = Exported.TrimStartAndEnd();
        if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
        {
            Inner = Inner.Mid(1, Inner.Len() - 2);
        }
        if (Inner.TrimStartAndEnd().IsEmpty())
        {
            return;
        }

        TArray<FString> Entries;
        Inner.ParseIntoArray(Entries, TEXT(","), true);
        for (FString Entry : Entries)
        {
            Entry = Entry.TrimStartAndEnd();
            int32 DotIndex = INDEX_NONE;
            if (!Entry.FindLastChar(TEXT('.'), DotIndex) || DotIndex <= 0 || DotIndex >= Entry.Len() - 1)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
            Binding->SetStringField(TEXT("object"), Entry.Left(DotIndex));
            Binding->SetStringField(TEXT("function"), Entry.Mid(DotIndex + 1));
            Bindings.Add(MakeShared<FJsonValueObject>(Binding));
        }
    }

    TSharedPtr<FJsonValue> MakeMulticastDelegateMarker(FMulticastDelegateProperty* Property, const TCHAR* Kind, const void* Container)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("_kind"), Kind);
        Obj->SetStringField(TEXT("type"), GetPropertyCppTypeWithParams(Property));

        TArray<TSharedPtr<FJsonValue>> Bindings;
        const void* ValuePtr = Property && Container ? Property->ContainerPtrToValuePtr<void>(Container) : nullptr;
        const FMulticastScriptDelegate* Delegate = (Property && ValuePtr) ? Property->GetMulticastDelegate(ValuePtr) : nullptr;
        if (Delegate && Delegate->IsBound())
        {
            FString Exported;
            Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
            AddParsedDelegateExportBindings(Exported, Bindings);
        }

        Obj->SetArrayField(TEXT("bindings"), Bindings);
        Obj->SetStringField(TEXT("bindingStatus"),
            (!Delegate || !Delegate->IsBound()) ? TEXT("empty") :
            (Bindings.Num() > 0 ? TEXT("bound") : TEXT("unreadable")));
        return MakeShared<FJsonValueObject>(Obj);
    }

    TSharedPtr<FJsonValue> MakeSingleDelegateMarker(FDelegateProperty* Property, const TCHAR* Kind, const void* Container)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("_kind"), Kind);
        Obj->SetStringField(TEXT("type"), GetPropertyCppTypeWithParams(Property));

        TArray<TSharedPtr<FJsonValue>> Bindings;
        const FScriptDelegate* Delegate = Property && Container
            ? Property->ContainerPtrToValuePtr<FScriptDelegate>(Container)
            : nullptr;
        if (Delegate && Delegate->IsBound())
        {
            Bindings.Add(MakeShared<FJsonValueObject>(
                MakeDelegateBindingObject(Delegate->GetUObject(), Delegate->GetFunctionName())));
        }

        Obj->SetArrayField(TEXT("bindings"), Bindings);
        Obj->SetStringField(TEXT("bindingStatus"), Bindings.Num() > 0 ? TEXT("bound") : TEXT("empty"));
        return MakeShared<FJsonValueObject>(Obj);
    }

    constexpr int32 InstancedSubobjectMaxDepth = 3;

    struct FInstancedRecursionContext
    {
        TSet<const UObject*> Seen;
        int32 Depth = 0;
    };

    // Thread-local because property serialization is single-threaded per dump, but
    // nested ExportPropertyToJsonValue calls need shared state without threading
    // it through the public signature.
    FInstancedRecursionContext& GetRecursionContext()
    {
        thread_local FInstancedRecursionContext Ctx;
        return Ctx;
    }

    FPropertyExportResult ExportPropertyToJsonValueInternal(
        FPropertyExportSource Source,
        FProperty* Property);

    // Typed marker for a property kind the exporter cannot decompose, so an
    // unsupported value stays distinguishable from one that genuinely holds null.
    TSharedPtr<FJsonValue> MakeUnsupportedMarker(FProperty* Property)
    {
        TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
        Marker->SetStringField(TEXT("_kind"), TEXT("unsupported"));
        Marker->SetStringField(TEXT("cpp_type"), GetPropertyCppTypeWithParams(Property));
        return MakeShared<FJsonValueObject>(Marker);
    }

    // Every recursion into a member, element, or map value goes through here. On a
    // marker-tolerant source (dump export) an unsupported member becomes an inline
    // marker so the enclosing struct/collection still emits everything it can decompose;
    // on a strict source (RPC response slots) the unsupported verdict propagates and the
    // caller rejects the whole shape.
    FPropertyExportResult ExportNestedPropertyToJsonValue(
        FPropertyExportSource Source,
        FProperty* Property)
    {
        const FPropertyExportResult Result = ExportPropertyToJsonValueInternal(Source, Property);
        if (Result.bSupported && Result.Value.IsValid())
        {
            return Result;
        }
        if (!Source.AllowsUnsupportedMarkers())
        {
            return {};
        }
        return FPropertyExportResult::Supported(MakeUnsupportedMarker(Property));
    }

    FPropertyExportResult BuildSparseFieldDiffJsonWithResult(
        UObject* Instance,
        UObject* ResolvedBaseline,
        const TSet<FName>& SkipProperties,
        EPropertyFlags ExtraSkipFlags,
        TFunctionRef<FPropertyExportResult(FProperty*)> EmitLeaf);

    // Forward declaration -- normal struct export keeps the full field walk and
    // returns the support verdict alongside the object.
    FPropertyExportResult StructToJsonValue(
        UStruct* Struct,
        FPropertyExportSource Source);

    UObject* ResolveInstancedSubobjectBaseline(UObject* Subobject)
    {
        if (!Subobject)
        {
            return nullptr;
        }

        UClass* SubobjectClass = Subobject->GetClass();
        UObject* Baseline = Subobject->GetArchetype();
        if (!Baseline || !SubobjectClass || !Baseline->GetClass()->IsChildOf(SubobjectClass))
        {
            Baseline = SubobjectClass ? SubobjectClass->GetDefaultObject() : nullptr;
        }
        return Baseline;
    }

    FPropertyExportResult SparseInstancedSubobjectToJsonValue(
        UObject* Subobject,
        bool bAllowUnsupportedMarkers)
    {
        // Instanced subobjects diff against their archetype (CDO fallback) and skip the
        // duplicate/text-export transients that a subobject template carries. The
        // expansion starts a fresh object-rooted source, so marker tolerance is carried
        // across explicitly instead of riding ForNestedContainer.
        return BuildSparseFieldDiffJsonWithResult(
            Subobject,
            ResolveInstancedSubobjectBaseline(Subobject),
            TSet<FName>(),
            CPF_DuplicateTransient | CPF_TextExportTransient,
            [Subobject, bAllowUnsupportedMarkers](FProperty* Field)
            {
                const FPropertyExportSource SubobjectSource =
                    FPropertyExportSource::FromObject(Subobject);
                return ExportNestedPropertyToJsonValue(
                    bAllowUnsupportedMarkers
                        ? SubobjectSource.WithUnsupportedMarkers()
                        : SubobjectSource,
                    Field);
            });
    }

    FPropertyExportResult ExpandInstancedSubobject(UObject* Subobject, bool bAllowUnsupportedMarkers)
    {
        if (!Subobject) return MakeShared<FJsonValueNull>();

        FInstancedRecursionContext& Ctx = GetRecursionContext();

        if (Ctx.Depth >= InstancedSubobjectMaxDepth)
        {
            TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
            Marker->SetStringField(TEXT("_kind"), TEXT("max_depth"));
            Marker->SetStringField(TEXT("object"), Subobject->GetPathName());
            return MakeShared<FJsonValueObject>(Marker);
        }

        const UObject* Key = Subobject;
        if (Ctx.Seen.Contains(Key))
        {
            TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
            Marker->SetStringField(TEXT("_kind"), TEXT("cycle"));
            Marker->SetStringField(TEXT("object"), Subobject->GetPathName());
            return MakeShared<FJsonValueObject>(Marker);
        }

        Ctx.Seen.Add(Key);
        ++Ctx.Depth;
        FPropertyExportResult SparseResult =
            SparseInstancedSubobjectToJsonValue(Subobject, bAllowUnsupportedMarkers);
        --Ctx.Depth;
        Ctx.Seen.Remove(Key);
        if (!SparseResult.bSupported || !SparseResult.Value.IsValid())
        {
            return {};
        }

        const TSharedPtr<FJsonObject> Obj = SparseResult.Value->AsObject();
        if (!Obj.IsValid())
        {
            return {};
        }

        // Tag the sparse payload with the concrete subobject class so a polymorphic
        // Instanced UObject element is still identifiable when its fields match the
        // CDO (a defaults-only element would otherwise collapse to a bare `{}`,
        // indistinguishable from an absent element and erasing which subclass is
        // attached — e.g. which USmartObjectBehaviorDefinition fills a slot). This
        // mirrors the `_kind` discriminator convention used by the FInstancedStruct
        // branch in StructToJsonValue. The path name matches what the configure /
        // write-time RPCs echo as the class identity.
        Obj->SetStringField(TEXT("_kind"), Subobject->GetClass()->GetPathName());

        return MakeShared<FJsonValueObject>(Obj);
    }

    // Run ExportTextItem_Direct on the property value inside Container, then wrap
    // the resulting string in a typed marker for property kinds that intentionally
    // remain opaque.
    TSharedPtr<FJsonValue> MakeExportedMarker(FProperty* Property, const TCHAR* Kind, const void* Container)
    {
        FString Exported;
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
        Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
        return MakeTypedMarker(Kind, Exported);
    }

    // Walk every property of a struct value and dispatch each field through the
    // internal exporter so unsupported nested fields propagate directly.
    FPropertyExportResult StructToJsonValue(
        UStruct* Struct,
        FPropertyExportSource Source)
    {
        const void* StructPtr = Source.GetContainer();
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        if (!Struct || !StructPtr)
        {
            return MakeShared<FJsonValueObject>(Result);
        }

#if EARG_HAS_INSTANCED_STRUCT
        // FInstancedStruct stores its payload type-erased: a UScriptStruct* plus a
        // separately heap-allocated memory block, neither of which is a reflected
        // UPROPERTY. The generic field-iterator walk below would therefore see no
        // fields and emit {}, dropping the entire authored payload (Chooser
        // ColumnsStructs/ResultsStructs/FallbackResult, StateTree nodes, PoseSearch
        // entries, etc.). Detect the wrapper, read its current inner type, and recurse
        // on the inner struct so the real fields surface. The inner type is tagged with
        // the established `_kind` marker convention (see MakeTypedMarker) so callers can
        // tell which column/result kind a cell holds. An empty wrapper (null inner type)
        // carries `_kind: null`.
        if (Struct == FInstancedStruct::StaticStruct())
        {
            const FInstancedStruct* Instanced = static_cast<const FInstancedStruct*>(StructPtr);
            const UScriptStruct* InnerType = Instanced->GetScriptStruct();
            if (InnerType && Instanced->GetMemory())
            {
                FPropertyExportResult InnerResult = StructToJsonValue(
                    const_cast<UScriptStruct*>(InnerType),
                    Source.ForNestedContainer(
                        const_cast<void*>(static_cast<const void*>(Instanced->GetMemory()))));
                if (!InnerResult.bSupported || !InnerResult.Value.IsValid())
                {
                    return {};
                }
                Result = InnerResult.Value->AsObject();
                if (!Result.IsValid())
                {
                    return {};
                }
                Result->SetStringField(TEXT("_kind"), InnerType->GetStructCPPName());
            }
            else
            {
                Result->SetField(TEXT("_kind"), MakeShared<FJsonValueNull>());
            }
            return MakeShared<FJsonValueObject>(Result);
        }
#endif

        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Field = *It;
            if (!Field) continue;
            if (Field->HasAnyPropertyFlags(
                    CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated | CPF_SkipSerialization)
                || ShouldSkipNonSemanticDumpProperty(Field))
            {
                continue;
            }
            const FPropertyExportResult FieldResult = ExportNestedPropertyToJsonValue(
                Source, Field);
            if (!FieldResult.bSupported || !FieldResult.Value.IsValid())
            {
                return {};
            }
            Result->SetField(Field->GetName(), FieldResult.Value);
        }
        return MakeShared<FJsonValueObject>(Result);
    }

    TSharedPtr<FJsonObject> MakeRichCurveKeySummary(const FRichCurveKey& Key)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("time"), Key.Time);
        Obj->SetNumberField(TEXT("value"), Key.Value);
        return Obj;
    }

    int32 CountTransformCurveKeys(const FTransformCurve& TransformCurve)
    {
        return FMath::Max3(
            TransformCurve.TranslationCurve.GetNumKeys(),
            TransformCurve.RotationCurve.GetNumKeys(),
            TransformCurve.ScaleCurve.GetNumKeys());
    }

    TSharedPtr<FJsonValue> ExportRawCurveTracksSummary(const FRawCurveTracks& RawCurveTracks)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("_kind"), TEXT("FRawCurveTracks"));
        Obj->SetNumberField(TEXT("floatCurveCount"), RawCurveTracks.FloatCurves.Num());
        Obj->SetNumberField(TEXT("transformCurveCount"), RawCurveTracks.TransformCurves.Num());

        int32 TotalKeyCount = 0;
        TArray<TSharedPtr<FJsonValue>> FloatCurves;
        FloatCurves.Reserve(RawCurveTracks.FloatCurves.Num());

        for (const FFloatCurve& FloatCurve : RawCurveTracks.FloatCurves)
        {
            const TArray<FRichCurveKey>& Keys = FloatCurve.FloatCurve.GetConstRefOfKeys();
            TotalKeyCount += Keys.Num();

            TSharedPtr<FJsonObject> CurveObj = MakeShared<FJsonObject>();
            CurveObj->SetStringField(TEXT("name"), FloatCurve.GetName().ToString());
            CurveObj->SetStringField(TEXT("type"), TEXT("float"));
            CurveObj->SetNumberField(TEXT("curveTypeFlags"), FloatCurve.GetCurveTypeFlags());
            CurveObj->SetNumberField(TEXT("keyCount"), Keys.Num());
            if (!Keys.IsEmpty())
            {
                CurveObj->SetObjectField(TEXT("firstKey"), MakeRichCurveKeySummary(Keys[0]));
                CurveObj->SetObjectField(TEXT("lastKey"), MakeRichCurveKeySummary(Keys.Last()));
            }

            FloatCurves.Add(MakeShared<FJsonValueObject>(CurveObj));
        }

        for (const FTransformCurve& TransformCurve : RawCurveTracks.TransformCurves)
        {
            TotalKeyCount += CountTransformCurveKeys(TransformCurve);
        }

        Obj->SetNumberField(TEXT("totalKeyCount"), TotalKeyCount);
        Obj->SetArrayField(TEXT("floatCurves"), FloatCurves);
        return MakeShared<FJsonValueObject>(Obj);
    }
}

namespace
{
FPropertyExportResult BuildSparseFieldDiffJsonWithResult(
    UObject* Instance,
    UObject* ResolvedBaseline,
    const TSet<FName>& SkipProperties,
    EPropertyFlags ExtraSkipFlags,
    TFunctionRef<FPropertyExportResult(FProperty*)> EmitLeaf)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (!Instance)
    {
        return MakeShared<FJsonValueObject>(Result);
    }

    UClass* InstanceClass = Instance->GetClass();

    TArray<FProperty*> Fields;
    for (TFieldIterator<FProperty> It(InstanceClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        FProperty* Field = *It;
        if (!Field)
        {
            continue;
        }
        if (!Field->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
        {
            continue;
        }
        // Allowlisted oversized fields must survive the noise filter so the emit loop
        // below can still summarize them. UInstancedStaticMeshComponent::PerInstanceSMData
        // is UPROPERTY(EditAnywhere, SkipSerialization) because the component bulk-
        // serializes the array by hand, so CPF_SkipSerialization would otherwise drop the
        // one field the placeholder exists for. Matches BuildClassPropertyJson, which
        // already runs its oversized check ahead of the same flag filter.
        if (!IsKnownOversizedProperty(Field)
            && (Field->HasAnyPropertyFlags(
                    CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated | CPF_SkipSerialization | ExtraSkipFlags)
                || ShouldSkipNonSemanticDumpProperty(Field)))
        {
            continue;
        }
        if (SkipProperties.Contains(Field->GetFName()))
        {
            continue;
        }
        Fields.Add(Field);
    }

    Fields.Sort([](const FProperty& A, const FProperty& B)
    {
        return A.GetName() < B.GetName();
    });

    for (FProperty* Field : Fields)
    {
        bool bDifferent = true;
        UClass* OwnerClass = Field->GetOwnerClass();
        if (ResolvedBaseline && OwnerClass && ResolvedBaseline->GetClass()->IsChildOf(OwnerClass))
        {
            void* InstanceValue = Field->ContainerPtrToValuePtr<void>(Instance);
            void* BaselineValue = Field->ContainerPtrToValuePtr<void>(ResolvedBaseline);
            bDifferent = !Field->Identical(InstanceValue, BaselineValue, PPF_DeepComparison);
        }
        if (!bDifferent)
        {
            continue;
        }

        if (const FOmissionReason* Reason = IsKnownOversizedProperty(Field))
        {
            Result->SetField(
                Field->GetName(),
                MakeShared<FJsonValueObject>(BuildOmissionPlaceholder(Field, Instance, *Reason)));
            continue;
        }

        const FPropertyExportResult FieldResult = EmitLeaf(Field);
        if (!FieldResult.bSupported)
        {
            return {};
        }
        if (FieldResult.Value.IsValid())
        {
            Result->SetField(Field->GetName(), FieldResult.Value);
        }
    }

    return MakeShared<FJsonValueObject>(Result);
}
}

TSharedPtr<FJsonObject> BuildSparseFieldDiffJson(
    UObject* Instance,
    UObject* ResolvedBaseline,
    const TSet<FName>& SkipProperties,
    EPropertyFlags ExtraSkipFlags,
    TFunctionRef<TSharedPtr<FJsonValue>(FProperty*)> EmitLeaf)
{
    const FPropertyExportResult ExportResult = BuildSparseFieldDiffJsonWithResult(
        Instance,
        ResolvedBaseline,
        SkipProperties,
        ExtraSkipFlags,
        [&EmitLeaf](FProperty* Field)
        {
            return FPropertyExportResult::Supported(EmitLeaf(Field));
        });
    return ExportResult.Value.IsValid()
        ? ExportResult.Value->AsObject()
        : MakeShared<FJsonObject>();
}

namespace
{
FPropertyExportResult ExportPropertyToJsonValueInternal(
    FPropertyExportSource Source,
    FProperty* Property)
{
    void* TargetContainer = Source.GetContainer();
    UObject* OwnerObject = Source.GetObject();
    if (!TargetContainer || !Property)
        return {};

    // Strings
    if (FStrProperty* Str = CastField<FStrProperty>(Property))
    {
        return MakeShared<FJsonValueString>(
            Str->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
    {
        FString Exported;
        const void* ValuePtr = TextProp->ContainerPtrToValuePtr<void>(TargetContainer);
        TextProp->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
        return MakeShared<FJsonValueString>(Exported);
    }

    // Names
    if (FNameProperty* NP = CastField<FNameProperty>(Property))
    {
        return MakeShared<FJsonValueString>(
            NP->GetPropertyValue_InContainer(TargetContainer).ToString());
    }

    // Booleans
    if (FBoolProperty* BP = CastField<FBoolProperty>(Property))
    {
        return MakeShared<FJsonValueBoolean>(
            BP->GetPropertyValue_InContainer(TargetContainer));
    }

    // Numeric (handle concrete numeric property types)
    if (FFloatProperty* FP = CastField<FFloatProperty>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)FP->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FDoubleProperty* DP = CastField<FDoubleProperty>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)DP->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FIntProperty* IP = CastField<FIntProperty>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)IP->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FInt64Property* I64P = CastField<FInt64Property>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)I64P->GetPropertyValue_InContainer(TargetContainer));
    }
    // Less-common signed/unsigned widths reach this path on common UE runtime
    // counters (e.g. CacheMeshDescription* on UPrimitiveComponent). Without these
    // they fall through to the unsupported sentinel.
    if (FInt8Property* I8P = CastField<FInt8Property>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)I8P->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FInt16Property* I16P = CastField<FInt16Property>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)I16P->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FUInt16Property* U16P = CastField<FUInt16Property>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)U16P->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FUInt32Property* U32P = CastField<FUInt32Property>(Property))
    {
        return MakeShared<FJsonValueNumber>(
            (double)U32P->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FUInt64Property* U64P = CastField<FUInt64Property>(Property))
    {
        // uint64 past 2^53 loses precision in IEEE-754 double; consumers comparing
        // exact values should treat this number as approximate above that bound.
        return MakeShared<FJsonValueNumber>(
            (double)U64P->GetPropertyValue_InContainer(TargetContainer));
    }
    if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
    {
        const uint8 ByteVal = ByteProp->GetPropertyValue_InContainer(TargetContainer);
        if (UEnum* Enum = ByteProp->Enum)
        {
            const FString EnumName = Enum->GetNameStringByValue(ByteVal);
            if (!EnumName.IsEmpty())
            {
                return MakeShared<FJsonValueString>(EnumName);
            }
        }
        return MakeShared<FJsonValueNumber>((double)ByteVal);
    }

    // Enum property (newer engine versions use FEnumProperty instead of FByteProperty)
    if (FEnumProperty* EP = CastField<FEnumProperty>(Property))
    {
        if (UEnum* Enum = EP->GetEnum())
        {
            void* ValuePtr = EP->ContainerPtrToValuePtr<void>(TargetContainer);
            if (FNumericProperty* UnderlyingProp = EP->GetUnderlyingProperty())
            {
                const int64 EnumVal = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
                const FString EnumName = Enum->GetNameStringByValue(EnumVal);
                if (!EnumName.IsEmpty())
                {
                    return MakeShared<FJsonValueString>(EnumName);
                }
                return MakeShared<FJsonValueNumber>((double)EnumVal);
            }
        }
        return MakeShared<FJsonValueNumber>(0.0);
    }

    // Class reference (TSubclassOf<>) — must come BEFORE FObjectProperty since
    // FClassProperty inherits from FObjectProperty.
    if (FClassProperty* CP = CastField<FClassProperty>(Property))
    {
        UObject* Raw = GetObjectPropertyValueFromContainer(CP, TargetContainer);
        UClass* AsClass = Cast<UClass>(Raw);
        if (AsClass)
            return MakeShared<FJsonValueString>(AsClass->GetPathName());
        return MakeShared<FJsonValueNull>();
    }

    // Object references. Instanced subobjects (UPROPERTY(Instanced)) carry their
    // configuration on the referenced object itself, so emit a recursed JSON
    // object instead of just the subobject's path string.
    if (FObjectProperty* OP = CastField<FObjectProperty>(Property))
    {
        FObjectPropertyBase* ObjectProperty = OP;
        UObject* O = GetObjectPropertyValueFromContainer(ObjectProperty, TargetContainer);
        if (!O)
        {
            // BP-added SCS component variables are null on the CDO — the BPGC
            // populates them at instance construction, not at CDO build time.
            // Look the template up via the SCS so the dump captures the
            // authored component state (transforms, meshes, collision, ...).
            if (OwnerObject && OwnerObject->HasAnyFlags(RF_ClassDefaultObject))
            {
                O = ResolveSCSComponentTemplate(OwnerObject, ObjectProperty);
            }
        }
        if (O && ShouldExpandObjectPropertyValue(ObjectProperty, OwnerObject, O))
        {
            return ExpandInstancedSubobject(O, Source.AllowsUnsupportedMarkers());
        }
        if (O)
            return MakeShared<FJsonValueString>(O->GetPathName());
        return MakeShared<FJsonValueNull>();
    }

    // Interface reference: emit underlying object's path or null.
    if (FInterfaceProperty* IFP = CastField<FInterfaceProperty>(Property))
    {
        const FScriptInterface* Iface =
            IFP->ContainerPtrToValuePtr<FScriptInterface>(TargetContainer);
        if (Iface)
        {
            if (UObject* O = Iface->GetObject())
            {
                return MakeShared<FJsonValueString>(O->GetPathName());
            }
        }
        return MakeShared<FJsonValueNull>();
    }

    // Soft object references
    if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Property))
    {
        const void* ValuePtr = SOP->ContainerPtrToValuePtr<void>(TargetContainer);
        const FSoftObjectPtr* SoftObjPtr = static_cast<const FSoftObjectPtr*>(ValuePtr);
        if (SoftObjPtr && !SoftObjPtr->IsNull())
        {
            return MakeShared<FJsonValueString>(
                SoftObjPtr->ToSoftObjectPath().ToString());
        }
        return MakeShared<FJsonValueNull>();
    }

    // Soft class references
    if (FSoftClassProperty* SCP = CastField<FSoftClassProperty>(Property))
    {
        const void* ValuePtr = SCP->ContainerPtrToValuePtr<void>(TargetContainer);
        const FSoftObjectPtr* SoftClassPtr = static_cast<const FSoftObjectPtr*>(ValuePtr);
        if (SoftClassPtr && !SoftClassPtr->IsNull())
        {
            return MakeShared<FJsonValueString>(
                SoftClassPtr->ToSoftObjectPath().ToString());
        }
        return MakeShared<FJsonValueNull>();
    }

    // Weak/Lazy object refs share the FObjectPropertyBase API: emit a path string
    // or null. They never own their target (cycle-safe) so no instanced expansion.
    if (FWeakObjectProperty* WOP = CastField<FWeakObjectProperty>(Property))
    {
        if (UObject* O = GetObjectPropertyValueFromContainer(WOP, TargetContainer))
        {
            return MakeShared<FJsonValueString>(O->GetPathName());
        }
        return MakeShared<FJsonValueNull>();
    }
    if (FLazyObjectProperty* LOP = CastField<FLazyObjectProperty>(Property))
    {
        if (UObject* O = GetObjectPropertyValueFromContainer(LOP, TargetContainer))
        {
            return MakeShared<FJsonValueString>(O->GetPathName());
        }
        return MakeShared<FJsonValueNull>();
    }

#if PINWRIGHT_HAS_OPTIONAL_PROPERTY
    // TOptional<T> property (UE 5.5+). When unset emit null; when set, dispatch
    // through the inner property type. Inner's Offset_Internal is 0 inside
    // FOptionalProperty's value layout, so the value pointer doubles as the
    // synthetic container for the recursive ContainerPtrToValuePtr dispatch.
    if (FOptionalProperty* OptP = CastField<FOptionalProperty>(Property))
    {
        void* ValuePtr = OptP->ContainerPtrToValuePtr<void>(TargetContainer);
        if (!ValuePtr)
        {
            return MakeShared<FJsonValueNull>();
        }
        void* InnerContainer = const_cast<void*>(OptP->GetValuePointerForReadIfSet(ValuePtr));
        if (!InnerContainer)
        {
            return MakeShared<FJsonValueNull>();
        }
        FProperty* Inner = OptP->GetValueProperty();
        if (!Inner)
        {
            return MakeShared<FJsonValueNull>();
        }
        return ExportNestedPropertyToJsonValue(
            Source.ForNestedContainer(InnerContainer),
            Inner);
    }
#endif

    // Delegates and field paths: emit a typed marker so the consumer can branch on
    // `_kind` rather than trying to parse an opaque ExportText string.
    // Multicast variants must be checked before the base FMulticastDelegateProperty
    // since the inline/sparse classes derive from it.
    if (FMulticastInlineDelegateProperty* MIDP = CastField<FMulticastInlineDelegateProperty>(Property))
    {
        return MakeMulticastDelegateMarker(MIDP, TEXT("FMulticastInlineDelegateProperty"), TargetContainer);
    }
    if (FMulticastSparseDelegateProperty* MSDP = CastField<FMulticastSparseDelegateProperty>(Property))
    {
        return MakeMulticastDelegateMarker(MSDP, TEXT("FMulticastSparseDelegateProperty"), TargetContainer);
    }
    if (FMulticastDelegateProperty* MDP = CastField<FMulticastDelegateProperty>(Property))
    {
        return MakeMulticastDelegateMarker(MDP, TEXT("FMulticastDelegateProperty"), TargetContainer);
    }
    if (FDelegateProperty* DP = CastField<FDelegateProperty>(Property))
    {
        return MakeSingleDelegateMarker(DP, TEXT("FDelegateProperty"), TargetContainer);
    }
    if (FFieldPathProperty* FPP = CastField<FFieldPathProperty>(Property))
    {
        return MakeExportedMarker(FPP, TEXT("FFieldPathProperty"), TargetContainer);
    }

    // Structs: FVector and FRotator common cases
    if (FStructProperty* SP = CastField<FStructProperty>(Property))
    {
        if (SP->Struct == FRawCurveTracks::StaticStruct())
        {
            const FRawCurveTracks* RawCurveTracks = SP->ContainerPtrToValuePtr<FRawCurveTracks>(TargetContainer);
            return ExportRawCurveTracksSummary(*RawCurveTracks);
        }

        const FString TypeName = SP->Struct ? SP->Struct->GetName() : FString();
        if (TypeName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
        {
            const FVector* V = SP->ContainerPtrToValuePtr<FVector>(TargetContainer);
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Add(MakeShared<FJsonValueNumber>(V->X));
            Arr.Add(MakeShared<FJsonValueNumber>(V->Y));
            Arr.Add(MakeShared<FJsonValueNumber>(V->Z));
            return MakeShared<FJsonValueArray>(Arr);
        }
        else if (TypeName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
        {
            const FRotator* R = SP->ContainerPtrToValuePtr<FRotator>(TargetContainer);
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Add(MakeShared<FJsonValueNumber>(R->Pitch));
            Arr.Add(MakeShared<FJsonValueNumber>(R->Yaw));
            Arr.Add(MakeShared<FJsonValueNumber>(R->Roll));
            return MakeShared<FJsonValueArray>(Arr);
        }

        return StructToJsonValue(
            SP->Struct,
            Source.ForNestedContainer(SP->ContainerPtrToValuePtr<void>(TargetContainer)));
    }

    // Arrays
    if (FArrayProperty* AP = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(
            AP, AP->ContainerPtrToValuePtr<void>(TargetContainer));
        TArray<TSharedPtr<FJsonValue>> Out;
        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            void* ElemPtr = Helper.GetRawPtr(i);
            FProperty* Inner = AP->Inner;
            if (!Inner)
            {
                return {};
            }
            const FPropertyExportResult ElementResult = ExportNestedPropertyToJsonValue(
                Source.ForNestedContainer(ElemPtr), Inner);
            if (!ElementResult.bSupported || !ElementResult.Value.IsValid())
            {
                return {};
            }
            Out.Add(ElementResult.Value);
        }
        return MakeShared<FJsonValueArray>(Out);
    }

    // Maps: export as JSON object with key-value pairs
    if (FMapProperty* MP = CastField<FMapProperty>(Property))
    {
        TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
        FScriptMapHelper Helper(MP, MP->ContainerPtrToValuePtr<void>(TargetContainer));

        // Bound by GetMaxIndex(), not Num() -- see container.map.get for the sparse-gap
        // rationale; matches the soft-world map scan in BuildMapReferencesJson, which
        // already uses GetMaxIndex().
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i))
                continue;

            uint8* PairPtr = Helper.GetPairPtr(i);
            const uint8* KeyPtr = Helper.GetKeyPtr(i);

            // Convert key to string
            FString KeyStr;
            FProperty* KeyProp = MP->KeyProp;
            if (!KeyProp)
            {
                return {};
            }
            // Validate support through the strict exporter, but keep the emitted key on
            // the canonical text path below. In particular, int64/uint64 output never
            // passes through the exporter's JSON number, and struct keys such as FGuid
            // keep their established digits representation.
            const FPropertyExportResult KeyResult = ExportNestedPropertyToJsonValue(
                Source.ForNestedContainer(PairPtr), KeyProp);
            if (!KeyResult.bSupported || !KeyResult.Value.IsValid())
            {
                return {};
            }

            if (CastField<FStrProperty>(KeyProp))
            {
                KeyStr = *reinterpret_cast<const FString*>(KeyPtr);
            }
            else if (CastField<FNameProperty>(KeyProp))
            {
                KeyStr = reinterpret_cast<const FName*>(KeyPtr)->ToString();
            }
            else if (CastField<FIntProperty>(KeyProp))
            {
                KeyStr = FString::FromInt(*reinterpret_cast<const int32*>(KeyPtr));
            }
            else
            {
                // Struct keys have a canonical atom representation (FGuid digits and
                // soft paths are common cases). Other integer widths also stay exact.
                KeyProp->ExportTextItem_Direct(KeyStr, KeyPtr, nullptr, nullptr, PPF_None);
            }

            FProperty* ValueProp = MP->ValueProp;
            if (!ValueProp)
            {
                return {};
            }
            const FPropertyExportResult ValueResult = ExportNestedPropertyToJsonValue(
                Source.ForNestedContainer(PairPtr), ValueProp);
            if (!ValueResult.bSupported || !ValueResult.Value.IsValid())
            {
                return {};
            }
            MapObj->SetField(KeyStr, ValueResult.Value);
        }

        return MakeShared<FJsonValueObject>(MapObj);
    }

    // Sets: export as JSON array
    if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(TargetContainer));

        // Bound by GetMaxIndex(), not Num() -- see container.set.remove for the
        // sparse-gap rationale; matches the soft-world set scan in BuildMapReferencesJson,
        // which already uses GetMaxIndex().
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i))
                continue;

            uint8* ElemPtr = Helper.GetElementPtr(i);
            FProperty* ElemProp = SetProp->ElementProp;
            if (!ElemProp)
            {
                return {};
            }
            const FPropertyExportResult ElementResult = ExportNestedPropertyToJsonValue(
                Source.ForNestedContainer(ElemPtr), ElemProp);
            if (!ElementResult.bSupported || !ElementResult.Value.IsValid())
            {
                return {};
            }
            Out.Add(ElementResult.Value);
        }

        // FScriptSetHelper exposes hash-table slot order, which is not part of
        // the authored value and can differ across loads. Sort by each element's
        // canonical JSON representation so scalar, reference, and fallback
        // element types all share the same deterministic ordering rule.
        Out.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            return SortedJsonWriter::SerializeSortedJsonValue(A)
                < SortedJsonWriter::SerializeSortedJsonValue(B);
        });

        return MakeShared<FJsonValueArray>(Out);
    }

    return {};
}

}

FPropertyExportSource FPropertyExportSource::FromResolvedContainer(
    void* InContainer,
    UObject* CandidateOwnerObject)
{
    return CandidateOwnerObject && static_cast<void*>(CandidateOwnerObject) == InContainer
        ? FromObject(CandidateOwnerObject)
        : FromRaw(InContainer);
}

TSharedPtr<FJsonValue> ExportPropertyToJsonValue(FPropertyExportSource Source, FProperty* Property)
{
    if (!Source.GetContainer() || !Property)
    {
        return nullptr;
    }
    // Dump-style export: unsupported members become inline markers in place, so a
    // struct or collection with one undecodable field still carries every other value.
    const FPropertyExportResult Result = ExportNestedPropertyToJsonValue(
        Source.WithUnsupportedMarkers(), Property);
    return Result.Value;
}

FPropertyExportResult ExportPropertyToJsonValueStrict(
    FPropertyExportSource Source, FProperty* Property)
{
    return ExportPropertyToJsonValueInternal(Source, Property);
}

namespace
{
    struct FFlagEntry { EPropertyFlags Mask; const TCHAR* Name; };

    static const FFlagEntry GPropertyFlagWhitelist[] = {
        { CPF_BlueprintAssignable,    TEXT("BlueprintAssignable") },
        { CPF_BlueprintCallable,      TEXT("BlueprintCallable") },
        { CPF_BlueprintReadOnly,      TEXT("BlueprintReadOnly") },
        { CPF_Config,                 TEXT("Config") },
        { CPF_Deprecated,             TEXT("Deprecated") },
        { CPF_DuplicateTransient,     TEXT("DuplicateTransient") },
        { CPF_Edit,                   TEXT("Edit") },
        { CPF_EditConst,              TEXT("EditConst") },
        { CPF_EditorOnly,             TEXT("EditorOnly") },
        { CPF_ExposeOnSpawn,          TEXT("ExposeOnSpawn") },
        { CPF_InstancedReference,     TEXT("Instanced") },
        { CPF_Net,                    TEXT("Net") },
        { CPF_NonTransactional,       TEXT("NonTransactional") },
        { CPF_RepNotify,              TEXT("RepNotify") },
        { CPF_RepSkip,                TEXT("RepSkip") },
        { CPF_SaveGame,               TEXT("SaveGame") },
        { CPF_SkipSerialization,      TEXT("SkipSerialization") },
        { CPF_TextExportTransient,    TEXT("TextExportTransient") },
        { CPF_Transient,              TEXT("Transient") },
    };

    TArray<FString> CollectPropertyFlags(FProperty* Property)
    {
        TArray<FString> Out;
        const EPropertyFlags Flags = Property->PropertyFlags;
        if ((Flags & CPF_BlueprintVisible) && !(Flags & CPF_BlueprintReadOnly))
        {
            Out.Add(TEXT("BlueprintReadWrite"));
        }
        for (const FFlagEntry& Entry : GPropertyFlagWhitelist)
        {
            if (Flags & Entry.Mask)
            {
                Out.Add(Entry.Name);
            }
        }
        Out.Sort();
        return Out;
    }
}

// FTextureSource default-constructs to a useless all-zero-GUID placeholder
// (Id=000...,NumLayers=1,BlockDataOffsets=(0)) for every UTexture subclass that
// doesn't populate it (MediaTexture, BinkMediaTexture, etc.); UTexture2D already
// emits its source data via a dedicated Texture2DDumpBuilder sidecar. So the
// Source FProperty carries no signal in properties.json for any UTexture descendant.
static bool IsTextureSourceNoisyProperty(const FProperty* Property)
{
    if (!Property) return false;
    static const FName NSource(TEXT("Source"));
    if (Property->GetFName() != NSource) return false;
    return Property->GetOwnerClass() == UTexture::StaticClass();
}

bool ShouldSkipNonSemanticDumpProperty(const FProperty* Property)
{
    if (!Property)
    {
        return false;
    }

    const UStruct* Owner = Property->GetOwnerStruct();
    if (!Owner)
    {
        return false;
    }

    const FName OwnerName = Owner->GetFName();
    const FName OwnerPackageName = Owner->GetOutermost()->GetFName();
    const FName PropertyName = Property->GetFName();
    static const FName EnginePackage(TEXT("/Script/Engine"));
    static const FName MovieScenePackage(TEXT("/Script/MovieScene"));
    static const FName NiagaraPackage(TEXT("/Script/Niagara"));

    // FStaticMeshSourceModel persists these as editor build caches. They may hold
    // real counts or MAX_uint32 depending on whether the mesh description was loaded.
    if (OwnerPackageName == EnginePackage && OwnerName == FName(TEXT("StaticMeshSourceModel")))
    {
        return PropertyName == FName(TEXT("CacheMeshDescriptionTrianglesCount"))
            || PropertyName == FName(TEXT("CacheMeshDescriptionVerticesCount"));
    }

    // Regenerated identity/compiled-data fields: equivalent authored state is not
    // guaranteed to reproduce the same value or ordering after an editor load.
    if (OwnerPackageName == MovieScenePackage && OwnerName == FName(TEXT("MovieSceneSignedObject")))
    {
        return PropertyName == FName(TEXT("Signature"));
    }
    if (OwnerPackageName == EnginePackage && OwnerName == FName(TEXT("MaterialInterface")))
    {
        return PropertyName == FName(TEXT("TextureStreamingData"));
    }
    if (OwnerPackageName == NiagaraPackage && OwnerName == FName(TEXT("NiagaraSystem")))
    {
        return PropertyName == FName(TEXT("ScriptRuntimeCompiledDataForEditor"))
            || PropertyName == FName(TEXT("SystemCompiledData"));
    }

    // A material instance's per-parameter ExpressionGUID is the cached link to the parent
    // material's parameter expression, not authored state: it is all-zero as serialized
    // until something reconciles the instance against its parent (opening it in the
    // material editor, UMaterialEditingLibrary::update_material_instance, a parent change),
    // after which the same unchanged .uasset dumps resolved GUIDs. Whichever value a sweep
    // captured therefore recorded that editor session, not the package. The parameter's
    // durable identity is ParameterInfo.Name, which stays.
    if (OwnerPackageName == EnginePackage && PropertyName == FName(TEXT("ExpressionGUID")))
    {
        // FStaticParameterBase declares the field for every static switch / component-mask
        // parameter; the others each declare their own.
        static const TSet<FName> ParameterValueStructs = {
            FName(TEXT("StaticParameterBase")),
            FName(TEXT("ScalarParameterValue")),
            FName(TEXT("VectorParameterValue")),
            FName(TEXT("DoubleVectorParameterValue")),
            FName(TEXT("TextureParameterValue")),
            FName(TEXT("TextureCollectionParameterValue")),
            FName(TEXT("ParameterCollectionParameterValue")),
            FName(TEXT("RuntimeVirtualTextureParameterValue")),
            FName(TEXT("SparseVolumeTextureParameterValue")),
            FName(TEXT("FontParameterValue")),
        };
        return ParameterValueStructs.Contains(OwnerName);
    }

    return false;
}

const FOmissionReason* IsKnownOversizedProperty(const FProperty* Property)
{
    if (!Property) return nullptr;
    UClass* OwnerClass = Property->GetOwnerClass();
    if (!OwnerClass) return nullptr;

    // Resolve UClass* pointers once at first call. FindObject returns nullptr for
    // classes whose module is disabled (e.g. Synthesis plugin off); those entries
    // simply don't get registered and the lookup misses for those owners.
    static const TMap<UClass*, TMap<FName, FOmissionReason>> Table = []()
    {
        TMap<UClass*, TMap<FName, FOmissionReason>> Map;
        auto AddEntry = [&Map](const TCHAR* ClassPath, FName PropName, FOmissionReason Reason)
        {
            if (UClass* Cls = FindObject<UClass>(nullptr, ClassPath))
            {
                Map.FindOrAdd(Cls).Add(PropName, Reason);
            }
        };
        AddEntry(TEXT("/Script/Synthesis.AudioImpulseResponse"),
            FName(TEXT("ImpulseResponse")),
            FOmissionReason{TEXT("TArray<float>"), TEXT("audio samples, %d elements")});
        AddEntry(TEXT("/Script/Engine.BodySetup"),
            FName(TEXT("AggGeom")),
            FOmissionReason{TEXT("FKAggregateGeom"), TEXT("physics geometry (convex/tri-mesh sub-arrays)")});
        AddEntry(TEXT("/Script/Engine.InstancedStaticMeshComponent"),
            FName(TEXT("PerInstanceSMData")),
            FOmissionReason{TEXT("TArray<FInstancedStaticMeshInstanceData>"), TEXT("per-instance transforms, %d elements")});
        return Map;
    }();

    if (const TMap<FName, FOmissionReason>* Inner = Table.Find(OwnerClass))
    {
        if (const FOmissionReason* Found = Inner->Find(Property->GetFName()))
        {
            return Found;
        }
    }
    return nullptr;
}

TSharedPtr<FJsonObject> BuildOmissionPlaceholder(
    FProperty* Property, const void* ContainerPtr, const FOmissionReason& Reason)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("type"), Reason.TypeStr ? Reason.TypeStr : TEXT(""));

    FString Summary;
    if (Reason.SummaryFmt)
    {
        FString Fmt(Reason.SummaryFmt);
        if (Fmt.Contains(TEXT("%d")) && Property && ContainerPtr)
        {
            if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Property))
            {
                const void* ValuePtr = ArrProp->ContainerPtrToValuePtr<void>(ContainerPtr);
                FScriptArrayHelper Helper(ArrProp, ValuePtr);
                Summary = Fmt.Replace(TEXT("%d"), *FString::FromInt(Helper.Num()));
            }
            else
            {
                Summary = Fmt.Replace(TEXT("%d"), TEXT("?"));
            }
        }
        else
        {
            Summary = Fmt;
        }
    }
    Obj->SetStringField(TEXT("$omitted"), Summary);
    Obj->SetStringField(TEXT("$reason"), TEXT("exceeds-llm-budget"));
    return Obj;
}

// UUserWidget CDO seeds these flags to true; WidgetBlueprintCompiler unconditionally rewrites them on every WBP CDO from the BP's graph contents (WidgetBlueprintCompiler.cpp:658-682), so they always look "overridden" relative to the parent CDO.
static bool IsCompilerManagedUserWidgetFlag(const FProperty* Property)
{
    if (!Property) return false;
    UClass* OwnerClass = Property->GetOwnerClass();
    if (!OwnerClass || OwnerClass != UUserWidget::StaticClass())
    {
        return false;
    }
    const FName Name = Property->GetFName();
    static const FName N1(TEXT("bHasScriptImplementedPaint"));
    static const FName N2(TEXT("bHasScriptImplementedTick"));
    static const FName N3(TEXT("bAutomaticallyRegisterInputOnConstruction"));
    return Name == N1 || Name == N2 || Name == N3;
}

TSharedPtr<FJsonObject> ExportPropertyToJsonValueWithInheritance(
    UObject* ChildContainer, UObject* ParentContainer, FProperty* Property, UClass* AssetClass)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    FString TypeStr = GetPropertyCppTypeWithParams(Property);
    // FSoftClassProperty::GetCPPType returns "TSoftClassPtr<X> " with a trailing space (UE engine quirk).
    TypeStr.TrimEndInline();
    Result->SetStringField(TEXT("type"), TypeStr);

    // Defensive: ChildContainer's class must own this property; otherwise
    // ContainerPtrToValuePtr reads garbage memory for non-POD types (crash).
    UClass* OwnerClass = Property->GetOwnerClass();
    if (ChildContainer && OwnerClass)
    {
        UClass* ChildClass = ChildContainer->GetClass();
        if (ChildClass && !ChildClass->IsChildOf(OwnerClass))
        {
            // Pruned shape (see AssetDumpBuilder.cpp dumpSchemaVersion).
            Result->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
            if (OwnerClass != AssetClass)
            {
                Result->SetStringField(TEXT("inherited_from"), OwnerClass->GetName());
            }
            Result->SetBoolField(TEXT("is_overridden_locally"), true);
            return Result;
        }
    }

    TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(ChildContainer, Property);
    if (Value.IsValid())
    {
        Result->SetField(TEXT("value"), Value);
    }
    else
    {
        Result->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
    }

    if (OwnerClass && OwnerClass != AssetClass)
    {
        Result->SetStringField(TEXT("inherited_from"), OwnerClass->GetName());
    }

    // Only compare against the parent CDO when the parent's class actually declares
    // or inherits this property. Properties declared on the leaf asset class don't
    // exist in the parent CDO's memory layout — reading through
    // ContainerPtrToValuePtr(ParentContainer) would land in unrelated memory and
    // crash on non-POD property types (FSoftObjectPath, FText, TArray, etc.).
    bool bOverridden = true;
    if (ParentContainer && OwnerClass)
    {
        UClass* ParentClass = ParentContainer->GetClass();
        if (ParentClass && ParentClass->IsChildOf(OwnerClass))
        {
            void* ChildPtr  = Property->ContainerPtrToValuePtr<void>(ChildContainer);
            void* ParentPtr = Property->ContainerPtrToValuePtr<void>(ParentContainer);
            bOverridden = !Property->Identical(ChildPtr, ParentPtr, PPF_DeepComparison);
        }
    }
    if (bOverridden && IsCompilerManagedUserWidgetFlag(Property))
    {
        bOverridden = false;
    }
    if (bOverridden)
    {
        Result->SetBoolField(TEXT("is_overridden_locally"), bOverridden);
    }

    TArray<FString> FlagNames = CollectPropertyFlags(Property);
    if (FlagNames.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> FlagsJson;
        for (const FString& Flag : FlagNames)
        {
            FlagsJson.Add(MakeShared<FJsonValueString>(Flag));
        }
        Result->SetArrayField(TEXT("flags"), FlagsJson);
    }

    return Result;
}

bool ShouldEmitClassDumpProperty(const FProperty* Property)
{
    if (!Property)
    {
        return false;
    }
    if (IsTextureSourceNoisyProperty(Property) || ShouldSkipNonSemanticDumpProperty(Property))
    {
        return false;
    }
    // Allowlisted oversized fields still get a key — a placeholder rather than a value —
    // so they outrank the non-persistent-flag filter below.
    if (IsKnownOversizedProperty(Property))
    {
        return true;
    }
    // Omit fields Unreal marks as non-persistent or obsolete. Their runtime values
    // are not part of the authored asset state and can vary between editor loads.
    // The mask mirrors FProperty::ShouldSerializeValue's own skip set; UHT strips the
    // `_DEPRECATED` suffix from a property's engine name and sets CPF_Deprecated, so
    // legacy upgrade fields such as UActorComponent::bInstanceComponent land here.
    if (Property->HasAnyPropertyFlags(
            CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated | CPF_SkipSerialization))
    {
        return false;
    }
    // Never reported as overridden, so they never reach the output either way.
    return !IsCompilerManagedUserWidgetFlag(Property);
}

TSharedPtr<FJsonObject> BuildClassPropertyJson(UObject* CDO, UObject* ParentCDO)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (!CDO)
    {
        return Result;
    }

    UClass* AssetClass = CDO->GetClass();
    UObject* ChildContainer  = CDO;
    UObject* ParentContainer = ParentCDO;

    // Collect all properties first so we can sort by name.
    TArray<FProperty*> Properties;
    for (TFieldIterator<FProperty> It(AssetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        Properties.Add(*It);
    }
    Properties.Sort([](const FProperty& A, const FProperty& B)
    {
        return A.GetName() < B.GetName();
    });

    for (FProperty* Prop : Properties)
    {
        if (!ShouldEmitClassDumpProperty(Prop))
        {
            continue;
        }
        if (const FOmissionReason* Reason = IsKnownOversizedProperty(Prop))
        {
            TSharedPtr<FJsonObject> Placeholder =
                BuildOmissionPlaceholder(Prop, ChildContainer, *Reason);
            Result->SetObjectField(Prop->GetName(), Placeholder);
            continue;
        }

        TSharedPtr<FJsonObject> PropObj =
            ExportPropertyToJsonValueWithInheritance(ChildContainer, ParentContainer, Prop, AssetClass);

        // Skip properties that match the parent CDO. ExportPropertyToJsonValueWithInheritance
        // sets `is_overridden_locally` only when the value differs from the parent (or no parent
        // was supplied — in which case every property is treated as overridden, preserving the
        // full-dump behavior callers get when passing ParentCDO=nullptr).
        if (!PropObj.IsValid() || !PropObj->HasField(TEXT("is_overridden_locally")))
        {
            continue;
        }

        Result->SetObjectField(Prop->GetName(), PropObj);
    }

    return Result;
}

namespace
{
    // Returns true if the soft-object property targets a UWorld (or subclass).
    // The IsChildOf(UWorld::StaticClass()) gate is the WHY: it filters out unrelated
    // soft refs (textures, materials, etc.) that share the FSoftObjectProperty class.
    bool IsSoftWorldProperty(const FSoftObjectProperty* SOP)
    {
        return SOP && SOP->PropertyClass && SOP->PropertyClass->IsChildOf(UWorld::StaticClass());
    }

    void AppendIfNonNull(
        TArray<TSharedPtr<FJsonValue>>& OutEntries,
        const FString& PropertyName,
        const FSoftObjectPtr* SoftPtr)
    {
        if (!SoftPtr || SoftPtr->IsNull())
        {
            return;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("property"), PropertyName);
        Entry->SetStringField(TEXT("path"), SoftPtr->ToSoftObjectPath().ToString());
        Entry->SetStringField(TEXT("source"), TEXT("soft-uworld"));
        OutEntries.Add(MakeShared<FJsonValueObject>(Entry));
    }
}

namespace
{
    // Per-class cache of FProperty* that can hold soft-UWorld references (direct or via
    // Array/Set/Map containers). Keyed by UClass* so two unrelated classes never share a
    // cache slot. Empty TArray = "this class has no qualifying properties" — O(1) bail
    // on every subsequent asset of the same class during folder sweeps.
    //
    // Thread-safety: the RPC dispatcher enforces game-thread execution
    // (see RpcDispatcher) and asset.dump_folder iterates assets serially, so no lock is
    // needed. If that contract changes, wrap accesses in a FCriticalSection.
    static TMap<TWeakObjectPtr<UClass>, TArray<FProperty*>> GSoftWorldPropertyCache;

    // True if this property either *is* a soft-UWorld ref, or is an Array/Set/Map whose
    // key or value is a soft-UWorld ref. Mirrors the dispatch logic in
    // BuildMapReferencesJson — keep in sync.
    bool PropertyHoldsSoftWorld(FProperty* Property)
    {
        if (!Property)
        {
            return false;
        }
        if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Property))
        {
            return IsSoftWorldProperty(SOP);
        }
        if (FArrayProperty* AP = CastField<FArrayProperty>(Property))
        {
            return IsSoftWorldProperty(CastField<FSoftObjectProperty>(AP->Inner));
        }
        if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
        {
            return IsSoftWorldProperty(CastField<FSoftObjectProperty>(SetProp->ElementProp));
        }
        if (FMapProperty* MP = CastField<FMapProperty>(Property))
        {
            return IsSoftWorldProperty(CastField<FSoftObjectProperty>(MP->KeyProp))
                || IsSoftWorldProperty(CastField<FSoftObjectProperty>(MP->ValueProp));
        }
        return false;
    }

    // Returns the cached qualifying-property list for AssetClass, populating it on first
    // access. Empty list = "no soft-UWorld props anywhere on this class" — caller bails.
    const TArray<FProperty*>& GetCachedSoftWorldProperties(UClass* AssetClass)
    {
        if (TArray<FProperty*>* Existing = GSoftWorldPropertyCache.Find(AssetClass))
        {
            return *Existing;
        }
        TArray<FProperty*>& Cached = GSoftWorldPropertyCache.Add(AssetClass);
        for (TFieldIterator<FProperty> It(AssetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            if (PropertyHoldsSoftWorld(*It))
            {
                Cached.Add(*It);
            }
        }
        return Cached;
    }
}

TSharedPtr<FJsonObject> BuildMapReferencesJson(UObject* Asset)
{
    if (!Asset)
    {
        return nullptr;
    }

    UClass* AssetClass = Asset->GetClass();
    const TArray<FProperty*>& SoftWorldProps = GetCachedSoftWorldProperties(AssetClass);
    if (SoftWorldProps.Num() == 0)
    {
        // Fast-path: this class has no soft-UWorld properties at all.
        return nullptr;
    }

    TArray<TSharedPtr<FJsonValue>> Entries;

    for (FProperty* Property : SoftWorldProps)
    {
        if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Property))
        {
            const FSoftObjectPtr* SoftPtr =
                SOP->ContainerPtrToValuePtr<FSoftObjectPtr>(Asset);
            AppendIfNonNull(Entries, Property->GetName(), SoftPtr);
            continue;
        }

        if (FArrayProperty* AP = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(AP, AP->ContainerPtrToValuePtr<void>(Asset));
            for (int32 i = 0; i < Helper.Num(); ++i)
            {
                const FSoftObjectPtr* SoftPtr =
                    reinterpret_cast<const FSoftObjectPtr*>(Helper.GetRawPtr(i));
                AppendIfNonNull(Entries, FString::Printf(TEXT("%s[%d]"), *Property->GetName(), i), SoftPtr);
            }
            continue;
        }

        if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
        {
            FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(Asset));
            int32 Emitted = 0;
            for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
            {
                if (!Helper.IsValidIndex(i))
                {
                    continue;
                }
                const FSoftObjectPtr* SoftPtr =
                    reinterpret_cast<const FSoftObjectPtr*>(Helper.GetElementPtr(i));
                AppendIfNonNull(Entries, FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Emitted++), SoftPtr);
            }
            continue;
        }

        if (FMapProperty* MP = CastField<FMapProperty>(Property))
        {
            FSoftObjectProperty* InnerKey = CastField<FSoftObjectProperty>(MP->KeyProp);
            FSoftObjectProperty* InnerVal = CastField<FSoftObjectProperty>(MP->ValueProp);
            const bool bKeyIsWorld = IsSoftWorldProperty(InnerKey);
            const bool bValIsWorld = IsSoftWorldProperty(InnerVal);
            FScriptMapHelper Helper(MP, MP->ContainerPtrToValuePtr<void>(Asset));
            int32 Emitted = 0;
            for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
            {
                if (!Helper.IsValidIndex(i))
                {
                    continue;
                }
                if (bKeyIsWorld)
                {
                    const FSoftObjectPtr* SoftPtr =
                        reinterpret_cast<const FSoftObjectPtr*>(Helper.GetKeyPtr(i));
                    AppendIfNonNull(Entries, FString::Printf(TEXT("%s.key[%d]"), *Property->GetName(), Emitted), SoftPtr);
                }
                if (bValIsWorld)
                {
                    const FSoftObjectPtr* SoftPtr =
                        reinterpret_cast<const FSoftObjectPtr*>(Helper.GetValuePtr(i));
                    AppendIfNonNull(Entries, FString::Printf(TEXT("%s.value[%d]"), *Property->GetName(), Emitted), SoftPtr);
                }
                ++Emitted;
            }
            continue;
        }
    }

    if (Entries.Num() == 0)
    {
        return nullptr;
    }

    // Sort by property name. Pull the name out once per entry into a parallel TArray,
    // then sort indices and rebuild — avoids two GetStringField() calls + FString
    // allocations per comparison pair (2*N*log N → N).
    TArray<FString> SortKeys;
    SortKeys.Reserve(Entries.Num());
    for (const TSharedPtr<FJsonValue>& Value : Entries)
    {
        SortKeys.Add(Value->AsObject()->GetStringField(TEXT("property")));
    }
    TArray<int32> Indices;
    Indices.Reserve(Entries.Num());
    for (int32 i = 0; i < Entries.Num(); ++i)
    {
        Indices.Add(i);
    }
    Indices.Sort([&SortKeys](const int32& A, const int32& B)
    {
        return SortKeys[A] < SortKeys[B];
    });
    TArray<TSharedPtr<FJsonValue>> Sorted;
    Sorted.Reserve(Entries.Num());
    for (int32 Idx : Indices)
    {
        Sorted.Add(MoveTemp(Entries[Idx]));
    }
    Entries = MoveTemp(Sorted);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("schemaVersion"), 1);
    Result->SetArrayField(TEXT("references"), Entries);
    return Result;
}

// The cached TArray<FProperty*> outlive their owning UClass: the TWeakObjectPtr key
// goes stale on a collect, but the raw FProperty* in the value do not, and a
// Blueprint-generated class freed by GC takes its FField chain with it. Any caller
// about to collect must drop the cache first.
void ClearSoftWorldPropertyCache()
{
    GSoftWorldPropertyCache.Empty();
}
