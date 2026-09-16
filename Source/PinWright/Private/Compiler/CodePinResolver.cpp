// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/CodePinResolver.h"

#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/BpirTypeGrammar.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "Utils/ClassUtils.h"
#include "Utils/PropertyUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodePinResolver, Log, All);

FCodePinResolver::FCodePinResolver()
{
}

void FCodePinResolver::RegisterVariable(const FString& Name, UEdGraphPin* Pin)
{
    PinMap.Add(Name, Pin);
}

void FCodePinResolver::RegisterLiteral(const FString& Name, const FString& Value)
{
    LiteralMap.Add(Name, Value);
}

UEdGraphPin* FCodePinResolver::ResolveVariable(const FString& Name) const
{
    UEdGraphPin* const* Found = PinMap.Find(Name);
    return Found ? *Found : nullptr;
}

bool FCodePinResolver::IsLiteral(const FString& Name) const
{
    return LiteralMap.Contains(Name);
}

FString FCodePinResolver::GetLiteralValue(const FString& Name) const
{
    const FString* Found = LiteralMap.Find(Name);
    return Found ? *Found : FString();
}

bool FCodePinResolver::HasVariable(const FString& Name) const
{
    return PinMap.Contains(Name) || LiteralMap.Contains(Name);
}

void FCodePinResolver::Clear()
{
    PinMap.Empty();
    LiteralMap.Empty();
}

bool FCodePinResolver::SetPinDefaultValue(UEdGraphPin* Pin, const FString& Value, FString* OutError)
{
    if (!Pin)
    {
        if (OutError) *OutError = TEXT("Pin is null");
        return false;
    }

    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
    {
        FText NewText;
        FString TextError;
        if (!CoerceStringToPersistedFText(Value, &Pin->DefaultTextValue, NewText, TextError))
        {
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("Text pin '%s' requires an FText namespace and key: %s"),
                    *Pin->PinName.ToString(), *TextError);
            }
            return false;
        }

        Pin->DefaultTextValue = MoveTemp(NewText);
        Pin->DefaultValue.Empty();
        Pin->DefaultObject = nullptr;
        if (UEdGraphNode* Node = Pin->GetOwningNode())
        {
            Node->PinDefaultValueChanged(Pin);
        }
        return true;
    }

    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_FieldPath)
    {
        Pin->DefaultValue = Value;
        Pin->DefaultObject = nullptr;
        Pin->DefaultTextValue = FText::GetEmpty();
        return true;
    }

    // nullptr literal → "None"
    if (Value == TEXT("nullptr") || Value == TEXT("NULL"))
    {
        Pin->DefaultValue = TEXT("None");
        return true;
    }

    // Boolean normalization
    if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
    {
        Pin->DefaultValue = TEXT("true");
        return true;
    }
    if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
    {
        Pin->DefaultValue = TEXT("false");
        return true;
    }

    // Positional struct-literal branches (FLinearColor / FVector / FRotator) lived
    // here historically; they were dead code since every production caller pre-runs
    // FBpirValueResolver::GetLiteralText, which now formats struct literals
    // reflectively via BpirStructLiteralUtils.

    // Enum::Value → resolve against enum-aware schema application for byte and enum pins.
    if (Value.Contains(TEXT("::")))
    {
        FString EnumName, ValueName;
        if (Value.Split(TEXT("::"), &EnumName, &ValueName))
        {
            // Multi-tier lookup via ResolveUEnum: covers engine enums, dynamically
            // generated enums (ETraceTypeQuery), and project-module enums (EReplaySaveState).
            UEnum* FoundEnum = ResolveUEnum(EnumName);
            if (FoundEnum)
            {
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte
                    || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
                {
                    const bool bHadOriginalEnumSubtype = Pin->PinType.PinSubCategoryObject.IsValid();
                    if (!bHadOriginalEnumSubtype)
                    {
                        Pin->PinType.PinSubCategoryObject = FoundEnum;
                    }

                    // Generic PC_Byte pin (e.g. EqualEqual_ByteByte B input) where we
                    // inferred the enum subtype ourselves: the K2 schema validator does
                    // not yet know about the inferred subtype, so TrySetDefaultValue
                    // would normalize the enum name back to its raw byte index. Write
                    // the canonical name string directly — once the BP recompile
                    // propagates the subtype, the validator accepts the name form.
                    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte
                        && !bHadOriginalEnumSubtype)
                    {
                        int64 RequestedEnumValue = INDEX_NONE;
                        if (BlueprintHandlerUtils::TryResolveEnumLiteralToValue(FoundEnum, ValueName, RequestedEnumValue)
                            || BlueprintHandlerUtils::TryResolveEnumLiteralToValue(FoundEnum, Value, RequestedEnumValue))
                        {
                            const FString CanonicalName = FoundEnum->GetNameStringByValue(RequestedEnumValue);
                            if (!CanonicalName.IsEmpty())
                            {
                                Pin->DefaultValue = CanonicalName;
                                return true;
                            }
                            Pin->DefaultValue = FString::Printf(TEXT("%lld"), RequestedEnumValue);
                            return true;
                        }
                    }

                    // Name-string path is preferred once an enum is in hand on a typed
                    // enum pin where the schema can validate against the known subtype.
                    FString AppliedLiteral;
                    FString ErrorCode;
                    FString ErrorMessage;
                    if (const UEdGraphSchema* Schema = Cast<UEdGraphSchema>(Pin->GetSchema()))
                    {
                        if (BlueprintHandlerUtils::TryApplyEnumPinDefaultValue(
                            Schema,
                            Pin,
                            Value,
                            AppliedLiteral,
                            ErrorCode,
                            ErrorMessage))
                        {
                            return true;
                        }
                    }
                }
                else
                {
                    // Non-enum pin (for example string): preserve the literal text.
                    Pin->DefaultValue = Value;
                    return true;
                }
            }
        }
    }

    // Quoted string → strip outer quotes and reverse the BPIR string-literal escape
    // (mirror of BpirStructLiteralUtils::EscapeBpirString via UnescapeBpirString).
    // General fix: any string-literal pin default with embedded quotes or backslashes
    // would otherwise land in DefaultValue with literal backslashes still present.
    if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
    {
        Pin->DefaultValue = BpirStructLiteralUtils::UnescapeBpirString(Value.Mid(1, Value.Len() - 2));
        return true;
    }

    // Numeric literals: set directly to avoid TrySetDefaultValue mishandling PC_Real/PC_Int pins
    {
        const FName& Category = Pin->PinType.PinCategory;
        if (Category == UEdGraphSchema_K2::PC_Real
            || Category == UEdGraphSchema_K2::PC_Int
            || Category == UEdGraphSchema_K2::PC_Int64
            || Category == UEdGraphSchema_K2::PC_Float
            || Category == UEdGraphSchema_K2::PC_Double)
        {
            // Strip trailing 'f' suffix (e.g. "50.0f" → "50.0")
            FString NumericValue = Value;
            if (NumericValue.EndsWith(TEXT("f")) || NumericValue.EndsWith(TEXT("F")))
            {
                NumericValue.LeftChopInline(1);
            }
            Pin->DefaultValue = NumericValue;
            return true;
        }
    }

    // Wildcard pins (e.g. MakeArray element pins): TrySetDefaultValue may reject
    // the value, so set DefaultValue directly to ensure it persists for round-trip.
    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
    {
        Pin->DefaultValue = Value;
        return true;
    }

    // Ask the K2 schema whether the literal is valid for this pin's type BEFORE applying it.
    // TrySetDefaultValue returns void in UE 5.6 and silently swallows invalid values (a bare
    // enum literal lands in DefaultValue, then fails at the next full BP recompile with
    // "Expected a valid unsigned number for a byte property"). IsPinDefaultValid is the
    // canonical pre-validation entry point: empty FString => accepted, non-empty => rejected.
    const UEdGraphSchema* Schema = Pin->GetSchema();
    const FString ValidationError = Schema
        ? Schema->IsPinDefaultValid(Pin, Value, nullptr, FText::GetEmpty())
        : FString();
    if (!ValidationError.IsEmpty())
    {
        const bool bIsByteOrEnumPin =
            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum;
        if (!bIsByteOrEnumPin)
        {
            // Preserve historical silent-swallow behavior for non-byte/enum pin categories:
            // many callers feed values the schema would reject (e.g. struct stringification
            // formats) but the BP recompile tolerates them. Apply and return success.
            if (Schema)
            {
                Schema->TrySetDefaultValue(*Pin, Value);
            }
            return true;
        }
        if (OutError)
        {
            *OutError = FString::Printf(
                TEXT("Value '%s' is not valid for byte/enum pin '%s' (schema rejected). ")
                TEXT("Use a qualified literal (`EnumName::Value`), convert via `enum_to_byte`, ")
                TEXT("or compare via `EqualEqual_IntInt` after converting the enum to int."),
                *Value, *Pin->PinName.ToString());
        }
        return false;
    }
    Schema->TrySetDefaultValue(*Pin, Value);
    return true;
}

static UFunction* ResolveDelegateSignatureFunction(const FString& OwnerHint, FName SignatureName);

// Fill a scalar (non-container) portion of OutType from a spec. The caller is
// responsible for recursing into containers before calling this for the
// element/key specs.
//
// Primitives and well-known structs are resolved via the unified grammar table
// (BpirTypeGrammar::ApplyKindToPinType). Tagged kinds whose InnerName is a
// user-supplied identifier fall back to the runtime UClass/UScriptStruct/UEnum
// resolver cascade. Unresolved (bare-identifier) kinds walk a UEnum ->
// UScriptStruct -> UClass priority cascade.
static bool ApplyScalarSpecToPinType(const FBpirTypeSpec& Spec, FEdGraphPinType& OutType)
{
    // Void on a parameter has no meaningful pin category.
    if (Spec.Kind == EBpirTypeKind::Void)
    {
        return false;
    }

    // Unresolved: bare identifier — walk Enum -> Struct -> Class in priority order.
    if (Spec.Kind == EBpirTypeKind::Unresolved)
    {
        const FString Ident = Spec.InnerName.ToString();
        if (UEnum* FoundEnum = ResolveUEnum(Ident))
        {
            OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            OutType.PinSubCategoryObject = FoundEnum;
            return true;
        }
        if (UScriptStruct* FoundStruct = ResolveUScriptStruct(Ident))
        {
            OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            OutType.PinSubCategoryObject = FoundStruct;
            return true;
        }
        if (UClass* FoundClass = ResolveClassByName(Ident))
        {
            OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
            OutType.PinSubCategoryObject = FoundClass;
            return true;
        }
        UE_LOG(LogCodePinResolver, Warning,
            TEXT("ConvertTypeSpecToPinType: unresolved identifier '%s'"), *Ident);
        return false;
    }

    // Table-driven mapping — succeeds for primitives (Bool/Int/Float/String/...)
    // and for Kind==Struct with a well-known InnerName (Vector/Rotator/...).
    if (BpirTypeGrammar::ApplyKindToPinType(Spec.Kind, Spec.InnerName, OutType))
    {
        return true;
    }

    // Tagged kinds with a user-supplied InnerName — resolve via the runtime cascade.
    // ApplyKindToPinType has already populated OutType.PinCategory; fill the subcategory
    // object per kind.
    switch (Spec.Kind)
    {
    case EBpirTypeKind::Struct:
    {
        // Kind==Struct reached here only if InnerName is non-well-known (or IsNone).
        if (Spec.InnerName.IsNone())
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: struct with no inner name"));
            return false;
        }
        UScriptStruct* FoundStruct = ResolveUScriptStruct(Spec.InnerName.ToString());
        if (!FoundStruct)
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: could not resolve struct '%s'"), *Spec.InnerName.ToString());
            return false;
        }
        OutType.PinSubCategoryObject = FoundStruct;
        return true;
    }

    case EBpirTypeKind::Object:
    case EBpirTypeKind::SoftObject:
    case EBpirTypeKind::Class:
    {
        // Bare "object"/"class" (and soft variants) with no inner name map to UObject.
        if (Spec.InnerName.IsNone())
        {
            OutType.PinSubCategoryObject = UObject::StaticClass();
            return true;
        }
        UClass* FoundClass = ResolveUClass(Spec.InnerName.ToString());
        if (!FoundClass)
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: could not resolve class '%s' for %s"),
                *Spec.InnerName.ToString(),
                *FString(BpirTypeGrammar::GetCanonicalText(Spec.Kind)));
            return false;
        }
        OutType.PinSubCategoryObject = FoundClass;
        return true;
    }

    case EBpirTypeKind::SoftClass:
    {
        // Bare "softclass" (no inner name) maps to UObject for parity with
        // the Object/SoftObject/Class branches above.
        if (Spec.InnerName.IsNone())
        {
            OutType.PinSubCategoryObject = UObject::StaticClass();
            return true;
        }
        UClass* FoundClass = ResolveUClass(Spec.InnerName.ToString());
        if (!FoundClass)
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: could not resolve class '%s' for softclass"), *Spec.InnerName.ToString());
            return false;
        }
        OutType.PinSubCategoryObject = FoundClass;
        return true;
    }

    case EBpirTypeKind::Enum:
    {
        if (UEnum* FoundEnum = ResolveUEnum(Spec.InnerName.ToString()))
        {
            OutType.PinSubCategoryObject = FoundEnum;
            return true;
        }
        UE_LOG(LogCodePinResolver, Warning,
            TEXT("ConvertTypeSpecToPinType: could not resolve enum '%s' for enum<T>"), *Spec.InnerName.ToString());
        return false;
    }

    case EBpirTypeKind::Interface:
    {
        UClass* FoundClass = ResolveUClass(Spec.InnerName.ToString());
        if (!FoundClass)
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: could not resolve class '%s' for interface"), *Spec.InnerName.ToString());
            return false;
        }
        OutType.PinSubCategoryObject = FoundClass;
        return true;
    }

    case EBpirTypeKind::Delegate:
    case EBpirTypeKind::McDelegate:
    {
        // Bare "delegate" / "mcdelegate" — empty MemberReference is the legacy
        // round-trip behavior; the PC_Delegate/PC_MCDelegate pin still works
        // for name-resolved dispatcher calls (bind/call/unbind by name).
        if (Spec.InnerName.IsNone() && Spec.SecondaryInnerName.IsNone())
        {
            return true;
        }
        if (Spec.InnerName.IsNone() || Spec.SecondaryInnerName.IsNone())
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: %s requires both signature-owner hint and signature name (got '%s' / '%s')"),
                *FString(BpirTypeGrammar::GetCanonicalText(Spec.Kind)),
                *Spec.InnerName.ToString(), *Spec.SecondaryInnerName.ToString());
            return false;
        }
        UFunction* SignatureFunction = ResolveDelegateSignatureFunction(
            Spec.InnerName.ToString(),
            Spec.SecondaryInnerName);
        if (!SignatureFunction)
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: could not resolve delegate signature '%s' with owner hint '%s' for %s"),
                *Spec.SecondaryInnerName.ToString(),
                *Spec.InnerName.ToString(),
                *FString(BpirTypeGrammar::GetCanonicalText(Spec.Kind)));
            return false;
        }

        FMemberReference::FillSimpleMemberReference<UFunction>(
            SignatureFunction,
            OutType.PinSubCategoryMemberReference);

        // UE's property creation path calls ResolveSimpleMemberReference<UFunction>() to
        // populate FDelegateProperty::SignatureFunction. The default MemberParent picked
        // by FillSimpleMemberReference doesn't always round-trip — package-owned native
        // signatures need MemberParent = UPackage (the signature's outermost); class-outered
        // signatures need MemberParent = UClass; pre-CL2412156 BPs may carry a null
        // MemberParent and rely on the engine's `__DelegateSignature` name-suffix branch
        // (MemberReference.cpp:490). Try each fallback until the round-trip succeeds; if
        // none does, the caller must skip pin creation rather than emit a malformed
        // delegate pin.
        if (!FMemberReference::ResolveSimpleMemberReference<UFunction>(
                OutType.PinSubCategoryMemberReference))
        {
            OutType.PinSubCategoryMemberReference.MemberParent = SignatureFunction->GetOutermost();
            OutType.PinSubCategoryMemberReference.MemberName = SignatureFunction->GetFName();
            OutType.PinSubCategoryMemberReference.MemberGuid.Invalidate();
        }
        if (!FMemberReference::ResolveSimpleMemberReference<UFunction>(
                OutType.PinSubCategoryMemberReference))
        {
            OutType.PinSubCategoryMemberReference.MemberParent = SignatureFunction->GetOwnerClass();
            OutType.PinSubCategoryMemberReference.MemberName = SignatureFunction->GetFName();
            OutType.PinSubCategoryMemberReference.MemberGuid.Invalidate();
        }
        if (!FMemberReference::ResolveSimpleMemberReference<UFunction>(
                OutType.PinSubCategoryMemberReference))
        {
            // Engine's __DelegateSignature name-suffix branch in
            // FMemberReference::ResolveMember<>: when MemberParent is null and the
            // member name ends in "__DelegateSignature", the engine falls back to a
            // global FindObject<UFunction>(MemberName) lookup. Required for
            // backward-compat with pre-CL2412156 BPs.
            OutType.PinSubCategoryMemberReference.MemberParent = nullptr;
            OutType.PinSubCategoryMemberReference.MemberName = SignatureFunction->GetFName();
            OutType.PinSubCategoryMemberReference.MemberGuid.Invalidate();
        }
        if (!FMemberReference::ResolveSimpleMemberReference<UFunction>(
                OutType.PinSubCategoryMemberReference))
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: delegate signature '%s' on '%s' failed to round-trip through ResolveSimpleMemberReference; skipping pin"),
                *Spec.SecondaryInnerName.ToString(),
                *Spec.InnerName.ToString());
            return false;
        }
        return true;
    }

    default:
        break;
    }

    // Kind was accepted by ApplyKindToPinType's pin-category pass but the switch
    // above didn't handle it — shouldn't happen given the full kind enumeration.
    return false;
}

static UPackage* ResolveDelegateSignaturePackageHint(const FString& Hint)
{
    if (Hint.IsEmpty())
    {
        return nullptr;
    }

    if (Hint.StartsWith(TEXT("/")))
    {
        return FindPackage(nullptr, *Hint);
    }

    if (UPackage* ScriptPackage = FindPackage(nullptr, *FString::Printf(TEXT("/Script/%s"), *Hint)))
    {
        return ScriptPackage;
    }

    if (UPackage* DirectPackage = FindPackage(nullptr, *Hint))
    {
        return DirectPackage;
    }

    for (TObjectIterator<UPackage> It; It; ++It)
    {
        UPackage* Package = *It;
        if (!Package)
        {
            continue;
        }

        const FString PackageName = Package->GetName();
        if (PackageName.Equals(Hint, ESearchCase::IgnoreCase)
            || PackageName.EndsWith(FString::Printf(TEXT("/%s"), *Hint), ESearchCase::IgnoreCase))
        {
            return Package;
        }
    }

    return nullptr;
}

static UFunction* FindDelegateSignatureInPackage(UPackage* Package, FName SignatureName)
{
    return Package && !SignatureName.IsNone()
        ? FindObject<UFunction>(Package, *SignatureName.ToString())
        : nullptr;
}

// UHT strips the leading 'F' from dynamic delegate type names: a header
// `DECLARE_DYNAMIC_DELEGATE_RetVal(bool, FGetBool)` declared inside `class UWidget`
// registers the UFunction as `GetBool__DelegateSignature` outered to UWidget — NOT
// `FGetBool__DelegateSignature` outered to /Script/UMG. BPIR repros and decompile
// output both surface the C++ type name (with F), so the resolver normalizes by
// trying the F-stripped form whenever the input matches `F[A-Z]…__DelegateSignature`.
static FName StripDelegateNameFPrefix(FName SignatureName)
{
    const FString Str = SignatureName.ToString();
    if (Str.Len() > 2 && Str[0] == TEXT('F') && FChar::IsUpper(Str[1]))
    {
        return FName(*Str.Mid(1));
    }
    return NAME_None;
}

static UFunction* FindDelegateSignatureOnClassChain(UClass* OwnerClass, FName SignatureName)
{
    if (!OwnerClass || SignatureName.IsNone())
    {
        return nullptr;
    }
    for (UClass* Cls = OwnerClass; Cls; Cls = Cls->GetSuperClass())
    {
        if (UFunction* Sig = FindObject<UFunction>(Cls, *SignatureName.ToString()))
        {
            return Sig;
        }
    }
    return nullptr;
}

static UFunction* ResolveDelegateSignatureFunction(const FString& OwnerHint, FName SignatureName)
{
    if (SignatureName.IsNone())
    {
        return nullptr;
    }

    const FName StrippedName = StripDelegateNameFPrefix(SignatureName);

    // Class-outered lookup is the common case for native UCLASS-scope dynamic delegates
    // (UWidget::FGetBool, UButton::FOnButtonClickedEvent, etc.). Walks the super chain so
    // BPs subclassing UWidget can name the inherited signature.
    if (UClass* OwnerClass = ResolveUClass(OwnerHint))
    {
        if (UFunction* Sig = FindDelegateSignatureOnClassChain(OwnerClass, SignatureName))
        {
            return Sig;
        }
        if (!StrippedName.IsNone())
        {
            if (UFunction* Sig = FindDelegateSignatureOnClassChain(OwnerClass, StrippedName))
            {
                return Sig;
            }
        }
        if (UFunction* ClassMember = OwnerClass->FindFunctionByName(SignatureName, EIncludeSuperFlag::IncludeSuper))
        {
            return ClassMember;
        }
        if (!StrippedName.IsNone())
        {
            if (UFunction* ClassMember = OwnerClass->FindFunctionByName(StrippedName, EIncludeSuperFlag::IncludeSuper))
            {
                return ClassMember;
            }
        }
        // Package-outered fallback for delegates declared at namespace scope.
        if (UFunction* PackageMember = FindDelegateSignatureInPackage(OwnerClass->GetOutermost(), SignatureName))
        {
            return PackageMember;
        }
        if (!StrippedName.IsNone())
        {
            if (UFunction* PackageMember = FindDelegateSignatureInPackage(OwnerClass->GetOutermost(), StrippedName))
            {
                return PackageMember;
            }
        }
    }

    // Engine canonical helper for `*__DelegateSignature` names not tied to a UCLASS.
    if (UFunction* EngineCanonical = ::FindDelegateSignature(SignatureName))
    {
        return EngineCanonical;
    }
    if (!StrippedName.IsNone())
    {
        if (UFunction* EngineCanonical = ::FindDelegateSignature(StrippedName))
        {
            return EngineCanonical;
        }
    }

    // Hint-as-package-name fallback (rare).
    if (UPackage* HintPackage = ResolveDelegateSignaturePackageHint(OwnerHint))
    {
        if (UFunction* Sig = FindDelegateSignatureInPackage(HintPackage, SignatureName))
        {
            return Sig;
        }
        if (!StrippedName.IsNone())
        {
            if (UFunction* Sig = FindDelegateSignatureInPackage(HintPackage, StrippedName))
            {
                return Sig;
            }
        }
    }

    return nullptr;
}

// Qualifier model note.
//
// `FBpirTypeSpec` carries `bIsConst` / `bIsReference` at every level: the outer
// container and each nested Element/Key spec may independently carry their own
// qualifiers (e.g. `const array<int>&` — outer only; `array<const int&>` —
// element only; `const map<const FName&, T>&` — outer + key). The pin type
// `FEdGraphPinType` has only one `bIsConst` / `bIsReference` pair for the whole
// type plus `PinValueType.bTerminalIsConst` for the map-value slot. There is
// no per-key or per-element-in-containers qualifier, nor a map-value reference
// bit. The conversion below therefore:
//   - Applies the OUTER `Spec.bIsConst` / `bIsReference` onto `OutType`.
//   - For Map: lifts `ElementSpec->bIsConst` onto `PinValueType.bTerminalIsConst`
//     (the only qualifier the terminal type can represent).
//   - Drops any remaining nested qualifiers (array/set element const/ref, map
//     key const/ref, map value reference) with a `UE_LOG` warning — they are
//     not representable on `FEdGraphPinType`.
bool FCodePinResolver::ConvertTypeSpecToPinType(const FBpirTypeSpec& Spec, FEdGraphPinType& OutType)
{
    if (Spec.Container == EPinContainerType::Array || Spec.Container == EPinContainerType::Set)
    {
        OutType.ContainerType = (Spec.Container == EPinContainerType::Array)
            ? EPinContainerType::Array
            : EPinContainerType::Set;
        if (!Spec.ElementSpec.IsValid())
        {
            UE_LOG(LogCodePinResolver, Warning, TEXT("ConvertTypeSpecToPinType: container has no element spec"));
            return false;
        }
        if (!ApplyScalarSpecToPinType(*Spec.ElementSpec, OutType))
        {
            return false;
        }
        // Outer container qualifiers land on OutType. Element qualifiers are
        // not representable (the container's single const/ref pair already
        // belongs to the outer spec). Collect every dropped qualifier into a
        // single log line.
        {
            TArray<const TCHAR*> DroppedQualifiers;
            if (Spec.ElementSpec->bIsConst)
            {
                DroppedQualifiers.Add(TEXT("element const"));
            }
            if (Spec.ElementSpec->bIsReference)
            {
                DroppedQualifiers.Add(TEXT("element reference"));
            }
            if (!DroppedQualifiers.IsEmpty())
            {
                UE_LOG(LogCodePinResolver, Warning,
                    TEXT("ConvertTypeSpecToPinType: dropped qualifiers on '%s' (FEdGraphPinType can't represent: %s)"),
                    *BpirTypeSpecParser::TypeSpecToBpirText(Spec),
                    *FString::Join(DroppedQualifiers, TEXT(", ")));
            }
        }
        OutType.bIsConst     = Spec.bIsConst;
        OutType.bIsReference = Spec.bIsReference;
        return true;
    }

    if (Spec.Container == EPinContainerType::Map)
    {
        if (!Spec.KeySpec.IsValid() || !Spec.ElementSpec.IsValid())
        {
            UE_LOG(LogCodePinResolver, Warning, TEXT("ConvertTypeSpecToPinType: map has no key/value spec"));
            return false;
        }
        OutType.ContainerType = EPinContainerType::Map;
        // Key populates PinCategory / PinSubCategoryObject on OutType; Value
        // populates PinValueType (see `FEdGraphTerminalType` in EdGraphNode.h
        // for the available terminal fields — only `bTerminalIsConst`, no
        // reference bit).
        if (!ApplyScalarSpecToPinType(*Spec.KeySpec, OutType))
        {
            return false;
        }
        FEdGraphPinType ValueShell;
        if (!ApplyScalarSpecToPinType(*Spec.ElementSpec, ValueShell))
        {
            return false;
        }
        OutType.PinValueType.TerminalCategory          = ValueShell.PinCategory;
        OutType.PinValueType.TerminalSubCategory       = ValueShell.PinSubCategory;
        OutType.PinValueType.TerminalSubCategoryObject = ValueShell.PinSubCategoryObject;

        // Map-value const survives onto PinValueType.bTerminalIsConst.
        if (Spec.ElementSpec->bIsConst)
        {
            OutType.PinValueType.bTerminalIsConst = true;
        }
        // Every other nested qualifier on a map has no slot in FEdGraphPinType /
        // FEdGraphTerminalType. Collect them and emit a single warning so the log
        // stays readable when multiple are dropped at once.
        TArray<const TCHAR*> DroppedQualifiers;
        if (Spec.ElementSpec->bIsReference)
        {
            DroppedQualifiers.Add(TEXT("map value reference"));
        }
        if (Spec.KeySpec->bIsConst)
        {
            DroppedQualifiers.Add(TEXT("map key const"));
        }
        if (Spec.KeySpec->bIsReference)
        {
            DroppedQualifiers.Add(TEXT("map key reference"));
        }
        if (!DroppedQualifiers.IsEmpty())
        {
            UE_LOG(LogCodePinResolver, Warning,
                TEXT("ConvertTypeSpecToPinType: dropped qualifiers on '%s' (FEdGraphPinType can't represent: %s)"),
                *BpirTypeSpecParser::TypeSpecToBpirText(Spec),
                *FString::Join(DroppedQualifiers, TEXT(", ")));
        }

        OutType.bIsConst     = Spec.bIsConst;
        OutType.bIsReference = Spec.bIsReference;
        return true;
    }

    // Scalar — no container. Apply spec directly and then the top-level qualifiers.
    if (!ApplyScalarSpecToPinType(Spec, OutType))
    {
        return false;
    }
    OutType.bIsConst     = Spec.bIsConst;
    OutType.bIsReference = Spec.bIsReference;
    return true;
}
