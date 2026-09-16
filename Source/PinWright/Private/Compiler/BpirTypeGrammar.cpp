// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/BpirTypeGrammar.h"

#include "Compiler/BpirTypeSpec.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Math/Box.h"
#include "Math/Color.h"
#include "Math/IntPoint.h"
#include "Math/IntVector.h"
#include "Math/Plane.h"
#include "Math/Quat.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace BpirTypeGrammar
{
namespace
{
    // Alias arrays — declared at namespace scope so TArrayView can reference them
    // across translation units via the table. One array per entry that needs aliases.
    // Keep case-insensitive; the lookup compares via FStringView::Equals(..., IgnoreCase).
    const TCHAR* const BoolAliases[]   = { TEXT("boolean") };
    const TCHAR* const IntAliases[]    = { TEXT("int32"), TEXT("integer") };
    const TCHAR* const StringAliases[] = { TEXT("FString") };
    const TCHAR* const NameAliases[]   = { TEXT("FName") };
    const TCHAR* const TextAliases[]   = { TEXT("FText") };
    const TCHAR* const FieldPathAliases[] = { TEXT("fieldpath") };

    const TCHAR* const VectorAliases[]      = { TEXT("vector"),      TEXT("FVector") };
    const TCHAR* const RotatorAliases[]     = { TEXT("rotator"),     TEXT("FRotator") };
    const TCHAR* const TransformAliases[]   = { TEXT("transform"),   TEXT("FTransform") };
    const TCHAR* const LinearColorAliases[] = { TEXT("linearcolor"), TEXT("FLinearColor") };
    const TCHAR* const ColorAliases[]       = { TEXT("color"),       TEXT("FColor") };
    const TCHAR* const IntPointAliases[]    = { TEXT("intpoint"),    TEXT("FIntPoint") };
    const TCHAR* const IntVectorAliases[]   = { TEXT("intvector"),   TEXT("FIntVector") };
    const TCHAR* const QuatAliases[]        = { TEXT("quat"),        TEXT("FQuat") };
    const TCHAR* const PlaneAliases[]       = { TEXT("plane"),       TEXT("FPlane") };
    const TCHAR* const BoxAliases[]         = { TEXT("box"),         TEXT("FBox") };

    // Well-known struct getters. These are thin wrappers around TBaseStructure so
    // the entries in the table can store plain function pointers of a single
    // signature, avoiding per-entry template instantiation differences.
    UScriptStruct* GetVectorStruct()      { return TBaseStructure<FVector>::Get(); }
    UScriptStruct* GetRotatorStruct()     { return TBaseStructure<FRotator>::Get(); }
    UScriptStruct* GetTransformStruct()   { return TBaseStructure<FTransform>::Get(); }
    UScriptStruct* GetLinearColorStruct() { return TBaseStructure<FLinearColor>::Get(); }
    UScriptStruct* GetColorStruct()       { return TBaseStructure<FColor>::Get(); }
    UScriptStruct* GetIntPointStruct()    { return TBaseStructure<FIntPoint>::Get(); }
    UScriptStruct* GetIntVectorStruct()   { return TBaseStructure<FIntVector>::Get(); }
    UScriptStruct* GetQuatStruct()        { return TBaseStructure<FQuat>::Get(); }
    UScriptStruct* GetPlaneStruct()       { return TBaseStructure<FPlane>::Get(); }
    UScriptStruct* GetBoxStruct()
    {
        // UE 5.6 exposes Box as a CoreUObject script struct rather than via TBaseStructure<FBox>.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        static UScriptStruct* BoxStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.Box"), EFindObjectFlags::ExactClass);
#else
        static UScriptStruct* BoxStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.Box"), true);
#endif
        return BoxStruct;
    }

} // namespace

// Proper table construction — built once and cached. Placed outside the anon
// namespace to avoid the namespace-alias syntax issue; still file-local via static.
static const TArray<FBpirTypeGrammarEntry>& GetTable()
{
    auto Build = []() -> TArray<FBpirTypeGrammarEntry>
    {
        TArray<FBpirTypeGrammarEntry> T;
        T.Reserve(32);

        // ---- Primitive / keyword entries (Bare only). ----

        T.Add({ EBpirTypeKind::Void, TEXT("void"),
                TArrayView<const TCHAR* const>(),
                NAME_None, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Bool, TEXT("bool"),
                TArrayView<const TCHAR* const>(BoolAliases, UE_ARRAY_COUNT(BoolAliases)),
                UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Byte, TEXT("byte"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Byte, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Int, TEXT("int"),
                TArrayView<const TCHAR* const>(IntAliases, UE_ARRAY_COUNT(IntAliases)),
                UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Int64, TEXT("int64"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Int64, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Float, TEXT("float"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Double, TEXT("double"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::String, TEXT("string"),
                TArrayView<const TCHAR* const>(StringAliases, UE_ARRAY_COUNT(StringAliases)),
                UEdGraphSchema_K2::PC_String, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Name, TEXT("name"),
                TArrayView<const TCHAR* const>(NameAliases, UE_ARRAY_COUNT(NameAliases)),
                UEdGraphSchema_K2::PC_Name, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Text, TEXT("text"),
                TArrayView<const TCHAR* const>(TextAliases, UE_ARRAY_COUNT(TextAliases)),
                UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::FieldPath, TEXT("field_path"),
                TArrayView<const TCHAR* const>(FieldPathAliases, UE_ARRAY_COUNT(FieldPathAliases)),
                UEdGraphSchema_K2::PC_FieldPath, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare });

        // Delegate / McDelegate accept both bare ("delegate") and arity-2 tagged
        // ("delegate<OwnerHint, SignatureName>") forms. The two-arg tagged form
        // mirrors map<K, V>'s precedent and lets the compiler restore
        // PinSubCategoryMemberReference by resolving an actual UFunction signature.
        // Bare form keeps the existing PC_Delegate pin with empty MemberReference.
        T.Add({ EBpirTypeKind::Delegate, TEXT("delegate"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Delegate, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare | EAcceptForm::Tagged | EAcceptForm::TaggedTwoArg });

        T.Add({ EBpirTypeKind::McDelegate, TEXT("mcdelegate"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_MCDelegate, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare | EAcceptForm::Tagged | EAcceptForm::TaggedTwoArg });

        // ---- Tagged-only entries. `object` and `class` also accept bare form. ----

        T.Add({ EBpirTypeKind::Struct, TEXT("struct"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Struct, NAME_None, nullptr, NAME_None,
                EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        T.Add({ EBpirTypeKind::Object, TEXT("object"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Object, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare | EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        T.Add({ EBpirTypeKind::SoftObject, TEXT("softobject"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_SoftObject, NAME_None, nullptr, NAME_None,
                EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        T.Add({ EBpirTypeKind::Class, TEXT("class"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Class, NAME_None, nullptr, NAME_None,
                EAcceptForm::Bare | EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        T.Add({ EBpirTypeKind::SoftClass, TEXT("softclass"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_SoftClass, NAME_None, nullptr, NAME_None,
                EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        // Enum back-store category on BP pins is PC_Byte with a UEnum subcategory object.
        T.Add({ EBpirTypeKind::Enum, TEXT("enum"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Byte, NAME_None, nullptr, NAME_None,
                EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        T.Add({ EBpirTypeKind::Interface, TEXT("interface"),
                TArrayView<const TCHAR* const>(),
                UEdGraphSchema_K2::PC_Interface, NAME_None, nullptr, NAME_None,
                EAcceptForm::Tagged | EAcceptForm::CppPrefix });

        // ---- Well-known structs (Bare). Kind==Struct, carry a canonical inner name. ----

        T.Add({ EBpirTypeKind::Struct, TEXT("Vector"),
                TArrayView<const TCHAR* const>(VectorAliases, UE_ARRAY_COUNT(VectorAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetVectorStruct, FName(TEXT("Vector")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("Rotator"),
                TArrayView<const TCHAR* const>(RotatorAliases, UE_ARRAY_COUNT(RotatorAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetRotatorStruct, FName(TEXT("Rotator")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("Transform"),
                TArrayView<const TCHAR* const>(TransformAliases, UE_ARRAY_COUNT(TransformAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetTransformStruct, FName(TEXT("Transform")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("LinearColor"),
                TArrayView<const TCHAR* const>(LinearColorAliases, UE_ARRAY_COUNT(LinearColorAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetLinearColorStruct, FName(TEXT("LinearColor")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("Color"),
                TArrayView<const TCHAR* const>(ColorAliases, UE_ARRAY_COUNT(ColorAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetColorStruct, FName(TEXT("Color")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("IntPoint"),
                TArrayView<const TCHAR* const>(IntPointAliases, UE_ARRAY_COUNT(IntPointAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetIntPointStruct, FName(TEXT("IntPoint")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("IntVector"),
                TArrayView<const TCHAR* const>(IntVectorAliases, UE_ARRAY_COUNT(IntVectorAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetIntVectorStruct, FName(TEXT("IntVector")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("Quat"),
                TArrayView<const TCHAR* const>(QuatAliases, UE_ARRAY_COUNT(QuatAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetQuatStruct, FName(TEXT("Quat")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("Plane"),
                TArrayView<const TCHAR* const>(PlaneAliases, UE_ARRAY_COUNT(PlaneAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetPlaneStruct, FName(TEXT("Plane")),
                EAcceptForm::Bare });

        T.Add({ EBpirTypeKind::Struct, TEXT("Box"),
                TArrayView<const TCHAR* const>(BoxAliases, UE_ARRAY_COUNT(BoxAliases)),
                UEdGraphSchema_K2::PC_Struct, NAME_None, &GetBoxStruct, FName(TEXT("Box")),
                EAcceptForm::Bare });

        return T;
    };

    static const TArray<FBpirTypeGrammarEntry> Table = Build();
    return Table;
}

const FBpirTypeGrammarEntry* FindByAlias(FStringView Ident, EAcceptForm RequiredAcceptForms)
{
    if (Ident.IsEmpty())
    {
        return nullptr;
    }
    const TArray<FBpirTypeGrammarEntry>& Table = GetTable();
    for (const FBpirTypeGrammarEntry& E : Table)
    {
        // Gate on the requested accept-form bits before doing any text compare.
        // RequiredAcceptForms==None means "any form".
        if (RequiredAcceptForms != EAcceptForm::None
            && !EnumHasAllFlags(E.AcceptForms, RequiredAcceptForms))
        {
            continue;
        }
        if (Ident.Equals(FStringView(E.CanonicalText), ESearchCase::IgnoreCase))
        {
            return &E;
        }
        for (const TCHAR* Alias : E.Aliases)
        {
            if (Ident.Equals(FStringView(Alias), ESearchCase::IgnoreCase))
            {
                return &E;
            }
        }
    }
    return nullptr;
}

const FBpirTypeGrammarEntry* FindByKind(EBpirTypeKind Kind)
{
    // Prefer the base entry for tagged kinds (CanonicalInnerName==None) so Struct
    // consumers that don't care about the well-known flavor get a stable answer.
    const TArray<FBpirTypeGrammarEntry>& Table = GetTable();
    for (const FBpirTypeGrammarEntry& E : Table)
    {
        if (E.Kind == Kind && E.CanonicalInnerName.IsNone())
        {
            return &E;
        }
    }
    // Fallback: any entry with that Kind (covers well-known-struct lookup where
    // the base Struct entry isn't present for some reason — defensive).
    for (const FBpirTypeGrammarEntry& E : Table)
    {
        if (E.Kind == Kind)
        {
            return &E;
        }
    }
    return nullptr;
}

const FBpirTypeGrammarEntry* FindByWellKnownStructName(FName InnerName)
{
    if (InnerName.IsNone())
    {
        return nullptr;
    }
    const TArray<FBpirTypeGrammarEntry>& Table = GetTable();
    for (const FBpirTypeGrammarEntry& E : Table)
    {
        if (E.CanonicalInnerName == InnerName)
        {
            return &E;
        }
    }
    return nullptr;
}

const FBpirTypeGrammarEntry* FindByPinCategory(
    FName PinCategory, FName PinSubCategory, UObject* SubCategoryObject)
{
    const TArray<FBpirTypeGrammarEntry>& Table = GetTable();

    // 1. PC_Real splits into Float/Double by PinSubCategory.
    if (PinCategory == UEdGraphSchema_K2::PC_Real)
    {
        const EBpirTypeKind Want = (PinSubCategory == UEdGraphSchema_K2::PC_Double)
            ? EBpirTypeKind::Double
            : EBpirTypeKind::Float;
        return FindByKind(Want);
    }

    // 2. PC_Byte + UEnum subcategory -> Enum. Bare PC_Byte -> Byte primitive.
    if (PinCategory == UEdGraphSchema_K2::PC_Byte)
    {
        if (SubCategoryObject && SubCategoryObject->IsA<UEnum>())
        {
            return FindByKind(EBpirTypeKind::Enum);
        }
        return FindByKind(EBpirTypeKind::Byte);
    }

    // 3. PC_Struct: match well-known structs by object-identity against the entry's
    //    BaseStruct() pointer; otherwise return the base Struct tagged entry.
    if (PinCategory == UEdGraphSchema_K2::PC_Struct)
    {
        if (UScriptStruct* Provided = Cast<UScriptStruct>(SubCategoryObject))
        {
            for (const FBpirTypeGrammarEntry& E : Table)
            {
                if (E.Kind == EBpirTypeKind::Struct && E.BaseStruct != nullptr)
                {
                    if (E.BaseStruct() == Provided)
                    {
                        return &E;
                    }
                }
            }
        }
        return FindByKind(EBpirTypeKind::Struct);
    }

    // 4. Tagged pin categories that don't depend on SubCategoryObject for routing.
    if (PinCategory == UEdGraphSchema_K2::PC_Object)     return FindByKind(EBpirTypeKind::Object);
    if (PinCategory == UEdGraphSchema_K2::PC_SoftObject) return FindByKind(EBpirTypeKind::SoftObject);
    if (PinCategory == UEdGraphSchema_K2::PC_Class)      return FindByKind(EBpirTypeKind::Class);
    if (PinCategory == UEdGraphSchema_K2::PC_SoftClass)  return FindByKind(EBpirTypeKind::SoftClass);
    if (PinCategory == UEdGraphSchema_K2::PC_Interface)  return FindByKind(EBpirTypeKind::Interface);

    // 5. Primitives / delegates.
    if (PinCategory == UEdGraphSchema_K2::PC_Boolean)     return FindByKind(EBpirTypeKind::Bool);
    if (PinCategory == UEdGraphSchema_K2::PC_Int)         return FindByKind(EBpirTypeKind::Int);
    if (PinCategory == UEdGraphSchema_K2::PC_Int64)       return FindByKind(EBpirTypeKind::Int64);
    if (PinCategory == UEdGraphSchema_K2::PC_String)      return FindByKind(EBpirTypeKind::String);
    if (PinCategory == UEdGraphSchema_K2::PC_Name)        return FindByKind(EBpirTypeKind::Name);
    if (PinCategory == UEdGraphSchema_K2::PC_Text)        return FindByKind(EBpirTypeKind::Text);
    if (PinCategory == UEdGraphSchema_K2::PC_FieldPath)   return FindByKind(EBpirTypeKind::FieldPath);
    if (PinCategory == UEdGraphSchema_K2::PC_Delegate)    return FindByKind(EBpirTypeKind::Delegate);
    if (PinCategory == UEdGraphSchema_K2::PC_MCDelegate)  return FindByKind(EBpirTypeKind::McDelegate);

    return nullptr;
}

bool ApplyKindToPinType(EBpirTypeKind Kind, FName InnerName, FEdGraphPinType& OutType)
{
    // Well-known struct: Kind==Struct with a canonical inner name that matches
    // one of the table's well-known entries. Populates the pin fully.
    if (Kind == EBpirTypeKind::Struct && !InnerName.IsNone())
    {
        if (const FBpirTypeGrammarEntry* Well = FindByWellKnownStructName(InnerName))
        {
            if (Well->BaseStruct != nullptr)
            {
                OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                OutType.PinSubCategory = NAME_None;
                OutType.PinSubCategoryObject = Well->BaseStruct();
                return true;
            }
        }
        // Unknown struct inner — caller must resolve by name.
        OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        return false;
    }

    const FBpirTypeGrammarEntry* Entry = FindByKind(Kind);
    if (!Entry)
    {
        return false;
    }

    // Tagged forms without an inner name still report the pin category; the caller
    // decides whether the absence of InnerName is acceptable (e.g. bare "object").
    OutType.PinCategory = Entry->PinCategory;
    OutType.PinSubCategory = Entry->PinSubCategory;

    // Primitive (non-tagged) kinds have fully-defined mappings — return true.
    const bool bTagged = EnumHasAnyFlags(Entry->AcceptForms, EAcceptForm::Tagged);
    if (!bTagged)
    {
        return true;
    }

    // Tagged: caller must resolve the user-supplied inner name into a UClass /
    // UScriptStruct / UEnum / UInterface. Signal "not fully specified".
    return false;
}

FStringView GetCanonicalText(EBpirTypeKind Kind)
{
    if (const FBpirTypeGrammarEntry* E = FindByKind(Kind))
    {
        return FStringView(E->CanonicalText);
    }
    return FStringView();
}

FStringView GetTaggedPrefix(EBpirTypeKind Kind)
{
    if (const FBpirTypeGrammarEntry* E = FindByKind(Kind))
    {
        if (EnumHasAnyFlags(E->AcceptForms, EAcceptForm::Tagged))
        {
            return FStringView(E->CanonicalText);
        }
    }
    return FStringView();
}

} // namespace BpirTypeGrammar
