// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Containers/StringView.h"
#include "Misc/EnumClassFlags.h"
#include "UObject/NameTypes.h"
#include "Compiler/BpirTypeSpec.h"

class UScriptStruct;
struct FEdGraphPinType;

// Single source of truth for the BPIR type-grammar keyword set. Every
// identifier/Kind mapping used by the parser, the pin-type emitter, and the
// decompiler reverse lookup flows through this table so they can't drift.
namespace BpirTypeGrammar
{
    // Accept-form bits. Bare: keyword alone ("bool", "int", "FVector").
    // Tagged: "<Keyword><Inner>" ("struct<Vector>"). CppPrefix: "Keyword Name"
    // or "Keyword:Name" ("struct Vector", "class:MyClass"). PointerSuffix:
    // "Name*" ("UMyClass*") — reserved for future use; no entry currently sets it.
    // TaggedTwoArg: arity-2 tagged form "<First, Second>" (delegate/mcdelegate
    // signature-owner hint + signature name; mirrors map<K, V>). When set, the
    // entry must also set Tagged so the single-arg path stays compatible.
    enum class EAcceptForm : uint8
    {
        None          = 0,
        Bare          = 1 << 0,
        Tagged        = 1 << 1,
        CppPrefix     = 1 << 2,
        PointerSuffix = 1 << 3,
        TaggedTwoArg  = 1 << 4,
    };
    ENUM_CLASS_FLAGS(EAcceptForm)

    struct FBpirTypeGrammarEntry
    {
        EBpirTypeKind Kind;
        const TCHAR* CanonicalText;            // emit form (e.g. "int", "struct"); never null
        TArrayView<const TCHAR* const> Aliases;// extra accept forms, case-insensitive; may be empty
        FName PinCategory;                     // UEdGraphSchema_K2::PC_*
        FName PinSubCategory;                  // NAME_None unless needed (PC_Real -> PC_Float/PC_Double)
        UScriptStruct* (*BaseStruct)();        // well-known structs only; nullptr otherwise
        FName CanonicalInnerName;              // well-known structs: "Vector"...; NAME_None otherwise
        EAcceptForm AcceptForms;               // bitmask of EAcceptForm bits
    };

    // Case-insensitive lookup by any accepted identifier (primary for parser).
    // When RequiredAcceptForms != None, only returns entries whose AcceptForms
    // contain every bit in the mask; otherwise returns any text-match.
    PINWRIGHT_API const FBpirTypeGrammarEntry* FindByAlias(
        FStringView Ident, EAcceptForm RequiredAcceptForms = EAcceptForm::None);

    // Lookup by Kind for canonical emission from FBpirTypeSpec.
    // For the well-known-struct family (Kind==Struct with a canonical inner), callers must
    // still use the per-entry lookup (FindByWellKnownStructName below); FindByKind returns
    // the base 'Struct' tagged entry.
    PINWRIGHT_API const FBpirTypeGrammarEntry* FindByKind(EBpirTypeKind Kind);

    // Look up a well-known-struct entry by its CanonicalInnerName (e.g. "Vector").
    // Returns nullptr if the name isn't a well-known struct.
    PINWRIGHT_API const FBpirTypeGrammarEntry* FindByWellKnownStructName(FName InnerName);

    // Reverse lookup from FEdGraphPinType fields (used by decompiler emitter).
    PINWRIGHT_API const FBpirTypeGrammarEntry* FindByPinCategory(
        FName PinCategory, FName PinSubCategory, UObject* SubCategoryObject);

    // Populates PinCategory/PinSubCategory/PinSubCategoryObject from a Kind+InnerName pair.
    // Returns true if a definitive mapping was applied. Returns false for tagged kinds
    // (Struct/Object/Class/etc.) whose InnerName is a user-supplied identifier — the caller
    // must then resolve via ResolveUClass/ResolveUScriptStruct/ResolveUEnum.
    PINWRIGHT_API bool ApplyKindToPinType(
        EBpirTypeKind Kind, FName InnerName, FEdGraphPinType& OutType);

    PINWRIGHT_API FStringView GetCanonicalText(EBpirTypeKind Kind);
    PINWRIGHT_API FStringView GetTaggedPrefix(EBpirTypeKind Kind);
}
