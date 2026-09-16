// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/BpirTypeSpecParser.h"

#include "Compiler/BpirTypeGrammar.h"
#include "Compiler/BpirTypeSpec.h"
#include "Containers/StringView.h"
#include "IrCore/IrTypeSpecParser.h"

namespace BpirTypeSpecParser
{
namespace
{
    BpirTypeGrammar::EAcceptForm ToBpirAcceptForm(IrTypeSpecParser::EIrTypeAcceptForm Form)
    {
        switch (Form)
        {
        case IrTypeSpecParser::EIrTypeAcceptForm::Bare:
            return BpirTypeGrammar::EAcceptForm::Bare;
        case IrTypeSpecParser::EIrTypeAcceptForm::Tagged:
            return BpirTypeGrammar::EAcceptForm::Tagged;
        case IrTypeSpecParser::EIrTypeAcceptForm::CppPrefix:
            return BpirTypeGrammar::EAcceptForm::CppPrefix;
        case IrTypeSpecParser::EIrTypeAcceptForm::PointerSuffix:
            return BpirTypeGrammar::EAcceptForm::PointerSuffix;
        case IrTypeSpecParser::EIrTypeAcceptForm::TaggedTwoArg:
            return BpirTypeGrammar::EAcceptForm::TaggedTwoArg;
        }
        return BpirTypeGrammar::EAcceptForm::None;
    }

    IrTypeSpecParser::FIrTypeGrammar MakeGrammar()
    {
        return IrTypeSpecParser::FIrTypeGrammar{
            [](FStringView Ident, IrTypeSpecParser::EIrTypeAcceptForm Form, EIrTypeKind& OutKind, FName& OutCanonicalInnerName) -> bool
            {
                if (Form == IrTypeSpecParser::EIrTypeAcceptForm::PointerSuffix)
                {
                    OutKind = Ident.Equals(FStringView(TEXT("UClass")), ESearchCase::CaseSensitive)
                        ? EBpirTypeKind::Class
                        : EBpirTypeKind::Object;
                    OutCanonicalInnerName = (OutKind == EBpirTypeKind::Class) ? NAME_None : FName(*FString(Ident));
                    return true;
                }

                const BpirTypeGrammar::FBpirTypeGrammarEntry* Entry =
                    BpirTypeGrammar::FindByAlias(Ident, ToBpirAcceptForm(Form));
                if (!Entry)
                {
                    return false;
                }
                OutKind = Entry->Kind;
                OutCanonicalInnerName = Entry->CanonicalInnerName;
                return true;
            },
            [](FStringView Ident, EPinContainerType& OutContainer) -> bool
            {
                if (Ident.Equals(FStringView(TEXT("array")), ESearchCase::IgnoreCase))
                {
                    OutContainer = EPinContainerType::Array;
                    return true;
                }
                if (Ident.Equals(FStringView(TEXT("set")), ESearchCase::IgnoreCase))
                {
                    OutContainer = EPinContainerType::Set;
                    return true;
                }
                if (Ident.Equals(FStringView(TEXT("map")), ESearchCase::IgnoreCase))
                {
                    OutContainer = EPinContainerType::Map;
                    return true;
                }
                return false;
            },
            [](EIrTypeKind Kind) -> FStringView
            {
                return BpirTypeGrammar::GetCanonicalText(Kind);
            },
            [](EIrTypeKind Kind) -> FStringView
            {
                return BpirTypeGrammar::GetTaggedPrefix(Kind);
            },
            [](EPinContainerType Container) -> FStringView
            {
                if (Container == EPinContainerType::Array) return FStringView(TEXT("array"));
                if (Container == EPinContainerType::Set) return FStringView(TEXT("set"));
                if (Container == EPinContainerType::Map) return FStringView(TEXT("map"));
                return FStringView();
            }
        };
    }

} // namespace

bool ParseTypeSpec(const FString& Source, FBpirTypeSpec& OutSpec, FString& OutError, int32& OutErrorColumn)
{
    return IrTypeSpecParser::ParseTypeSpec(Source, MakeGrammar(), OutSpec, OutError, OutErrorColumn);
}

FString TypeSpecToBpirText(const FBpirTypeSpec& Spec)
{
    return IrTypeSpecParser::TypeSpecToText(Spec, MakeGrammar());
}

FString FormatTypeSpecErrorDetail(const FString& ParseError, int32 ParseErrCol)
{
    return IrTypeSpecParser::FormatTypeSpecErrorDetail(ParseError, ParseErrCol);
}

} // namespace BpirTypeSpecParser
