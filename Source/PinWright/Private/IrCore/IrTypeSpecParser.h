// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/StringView.h"
#include "IrCore/IrTypeSpec.h"
#include "Templates/Function.h"

namespace IrTypeSpecParser
{
    enum class EIrTypeAcceptForm : uint8
    {
        Bare,
        Tagged,
        CppPrefix,
        PointerSuffix,
        // Arity-2 tagged form "<First, Second>" (currently only delegate /
        // mcdelegate; mirrors the map<K, V> two-arg shape). Querying with this
        // form returns true only for kinds whose grammar entry explicitly
        // accepts the comma-separated tagged shape, gating the second-arg
        // parse path so unrelated tagged kinds (object<UClass>, etc.) keep
        // rejecting trailing commas.
        TaggedTwoArg,
    };

    struct FIrTypeGrammar
    {
        TFunction<bool(FStringView Ident, EIrTypeAcceptForm Form, EIrTypeKind& OutKind, FName& OutCanonicalInnerName)> TryFindByAlias;
        TFunction<bool(FStringView Ident, EPinContainerType& OutContainer)> TryFindContainerByAlias;
        TFunction<FStringView(EIrTypeKind Kind)> GetCanonicalText;
        TFunction<FStringView(EIrTypeKind Kind)> GetTaggedPrefix;
        TFunction<FStringView(EPinContainerType Container)> GetContainerText;
    };

    PINWRIGHT_API bool ParseTypeSpec(
        const FString& Source,
        const FIrTypeGrammar& Grammar,
        FIrTypeSpec& OutSpec,
        FString& OutError,
        int32& OutErrorColumn);

    PINWRIGHT_API FString TypeSpecToText(const FIrTypeSpec& Spec, const FIrTypeGrammar& Grammar);

    PINWRIGHT_API FString FormatTypeSpecErrorDetail(const FString& ParseError, int32 ParseErrCol);
}
