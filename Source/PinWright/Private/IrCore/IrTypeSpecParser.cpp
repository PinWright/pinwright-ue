// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "IrCore/IrTypeSpecParser.h"

#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Misc/Char.h"
#include "Templates/UniquePtr.h"

namespace IrTypeSpecParser
{
namespace
{
    struct FCursor
    {
        const TCHAR* Data = nullptr;
        int32 Len = 0;
        int32 Pos = 0;

        bool Eof() const { return Pos >= Len; }
        TCHAR Peek() const { return Eof() ? TEXT('\0') : Data[Pos]; }

        void SkipWs()
        {
            while (!Eof() && FChar::IsWhitespace(Data[Pos]))
            {
                ++Pos;
            }
        }

        FString ReadIdentifier()
        {
            if (Eof()) return FString();
            const TCHAR C = Data[Pos];
            const bool bStartsIdent = FChar::IsAlpha(C) || C == TEXT('_');
            if (!bStartsIdent) return FString();
            const int32 Start = Pos;
            while (!Eof())
            {
                const TCHAR Ch = Data[Pos];
                if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
                {
                    ++Pos;
                }
                else
                {
                    break;
                }
            }
            return FString(Pos - Start, Data + Start);
        }
    };

    struct FParseState
    {
        FCursor Cur;
        FString Error;
        int32 ErrorColumn = INDEX_NONE;

        bool Fail(const FString& Msg, int32 Column)
        {
            if (Error.IsEmpty())
            {
                Error = Msg;
                ErrorColumn = Column;
            }
            return false;
        }

        bool FailUnexpected(TCHAR Ch, int32 Column)
        {
            return Fail(FString::Printf(TEXT("unexpected '%c' at column %d"), Ch, Column), Column);
        }
    };

    bool ParseSpec(FParseState& St, const FIrTypeGrammar& Grammar, FIrTypeSpec& Out);

    bool ApplyPrimitiveMapping(const FString& Ident, const FIrTypeGrammar& Grammar, FIrTypeSpec& OutSpec)
    {
        EIrTypeKind Kind = EIrTypeKind::Unresolved;
        FName CanonicalInnerName;
        if (!Grammar.TryFindByAlias
            || !Grammar.TryFindByAlias(FStringView(Ident), EIrTypeAcceptForm::Bare, Kind, CanonicalInnerName))
        {
            return false;
        }
        OutSpec.Kind = Kind;
        OutSpec.InnerName = CanonicalInnerName;
        return true;
    }

    bool TryFindTypeAlias(
        const FString& Ident,
        const FIrTypeGrammar& Grammar,
        EIrTypeAcceptForm Form,
        EIrTypeKind& OutKind,
        FName& OutCanonicalInnerName)
    {
        return Grammar.TryFindByAlias
            && Grammar.TryFindByAlias(FStringView(Ident), Form, OutKind, OutCanonicalInnerName);
    }

    bool TryFindContainerAlias(const FString& Ident, const FIrTypeGrammar& Grammar, EPinContainerType& OutContainer)
    {
        return Grammar.TryFindContainerByAlias
            && Grammar.TryFindContainerByAlias(FStringView(Ident), OutContainer);
    }

    bool ParseContainerSingleValue(FParseState& St, const FIrTypeGrammar& Grammar, FIrTypeSpec& Out, EPinContainerType ContainerKind)
    {
        St.Cur.SkipWs();
        if (St.Cur.Peek() != TEXT('<'))
        {
            return St.FailUnexpected(St.Cur.Peek(), St.Cur.Pos);
        }
        ++St.Cur.Pos;

        FIrTypeSpec Inner;
        if (!ParseSpec(St, Grammar, Inner))
        {
            return false;
        }

        St.Cur.SkipWs();
        if (St.Cur.Peek() != TEXT('>'))
        {
            return St.FailUnexpected(St.Cur.Peek(), St.Cur.Pos);
        }
        ++St.Cur.Pos;

        Out.Container = ContainerKind;
        Out.ElementSpec = MakeUnique<FIrTypeSpec>(MoveTemp(Inner));
        return true;
    }

    bool ParseContainerMap(FParseState& St, const FIrTypeGrammar& Grammar, FIrTypeSpec& Out)
    {
        St.Cur.SkipWs();
        if (St.Cur.Peek() != TEXT('<'))
        {
            return St.FailUnexpected(St.Cur.Peek(), St.Cur.Pos);
        }
        ++St.Cur.Pos;

        FIrTypeSpec Key;
        if (!ParseSpec(St, Grammar, Key))
        {
            return false;
        }

        St.Cur.SkipWs();
        if (St.Cur.Peek() != TEXT(','))
        {
            return St.FailUnexpected(St.Cur.Peek(), St.Cur.Pos);
        }
        ++St.Cur.Pos;

        FIrTypeSpec Value;
        if (!ParseSpec(St, Grammar, Value))
        {
            return false;
        }

        St.Cur.SkipWs();
        if (St.Cur.Peek() != TEXT('>'))
        {
            return St.FailUnexpected(St.Cur.Peek(), St.Cur.Pos);
        }
        ++St.Cur.Pos;

        Out.Container = EPinContainerType::Map;
        Out.KeySpec = MakeUnique<FIrTypeSpec>(MoveTemp(Key));
        Out.ElementSpec = MakeUnique<FIrTypeSpec>(MoveTemp(Value));
        return true;
    }

    bool ParseSpec(FParseState& St, const FIrTypeGrammar& Grammar, FIrTypeSpec& Out)
    {
        St.Cur.SkipWs();

        {
            const int32 Save = St.Cur.Pos;
            const FString Lead = St.Cur.ReadIdentifier();
            if (Lead == TEXT("const"))
            {
                Out.bIsConst = true;
                St.Cur.SkipWs();
            }
            else
            {
                St.Cur.Pos = Save;
            }
        }

        St.Cur.SkipWs();

        const FString Ident = St.Cur.ReadIdentifier();
        if (Ident.IsEmpty())
        {
            const TCHAR Bad = St.Cur.Peek();
            if (Bad == TEXT('\0'))
            {
                return St.Fail(TEXT("unexpected end of input"), St.Cur.Pos);
            }
            return St.FailUnexpected(Bad, St.Cur.Pos);
        }

        EPinContainerType ContainerKind = EPinContainerType::None;
        if (TryFindContainerAlias(Ident, Grammar, ContainerKind))
        {
            if (ContainerKind == EPinContainerType::Map)
            {
                if (!ParseContainerMap(St, Grammar, Out)) return false;
            }
            else
            {
                if (!ParseContainerSingleValue(St, Grammar, Out, ContainerKind)) return false;
            }
        }
        else
        {
            const int32 AfterIdentPos = St.Cur.Pos;
            St.Cur.SkipWs();
            const TCHAR Next = St.Cur.Peek();
            const bool bHasWhitespaceSeparator = (AfterIdentPos < St.Cur.Pos);

            EIrTypeKind TagKind = EIrTypeKind::Unresolved;
            FName CanonicalInnerName;
            const bool bAcceptsTagged = TryFindTypeAlias(
                Ident, Grammar, EIrTypeAcceptForm::Tagged, TagKind, CanonicalInnerName);
            if (bAcceptsTagged && Next == TEXT('<'))
            {
                ++St.Cur.Pos;
                St.Cur.SkipWs();
                const FString InnerIdent = St.Cur.ReadIdentifier();
                if (InnerIdent.IsEmpty())
                {
                    const TCHAR Bad = St.Cur.Peek();
                    if (Bad == TEXT('\0'))
                    {
                        return St.Fail(TEXT("unexpected end of input"), St.Cur.Pos);
                    }
                    return St.FailUnexpected(Bad, St.Cur.Pos);
                }
                St.Cur.SkipWs();

                // Arity-2 tagged form: "<First, Second>". Currently used by
                // delegate / mcdelegate to carry the owner class and the
                // UFunction signature name. Only consume the comma if the
                // grammar reports the kind as TaggedTwoArg-capable; otherwise
                // a comma here is a parse error (preserves the strict reject
                // for object<UClass, ...>, struct<X, Y>, etc.).
                FString SecondIdent;
                if (St.Cur.Peek() == TEXT(','))
                {
                    EIrTypeKind TwoArgKind = EIrTypeKind::Unresolved;
                    FName TwoArgInner;
                    const bool bAcceptsTwoArg = TryFindTypeAlias(
                        Ident, Grammar, EIrTypeAcceptForm::TaggedTwoArg, TwoArgKind, TwoArgInner);
                    if (!bAcceptsTwoArg)
                    {
                        return St.FailUnexpected(TEXT(','), St.Cur.Pos);
                    }
                    ++St.Cur.Pos;
                    St.Cur.SkipWs();
                    SecondIdent = St.Cur.ReadIdentifier();
                    if (SecondIdent.IsEmpty())
                    {
                        const TCHAR Bad = St.Cur.Peek();
                        if (Bad == TEXT('\0'))
                        {
                            return St.Fail(TEXT("unexpected end of input"), St.Cur.Pos);
                        }
                        return St.FailUnexpected(Bad, St.Cur.Pos);
                    }
                    St.Cur.SkipWs();
                }

                if (St.Cur.Peek() != TEXT('>'))
                {
                    return St.FailUnexpected(St.Cur.Peek(), St.Cur.Pos);
                }
                ++St.Cur.Pos;
                Out.Kind = TagKind;
                Out.InnerName = FName(*InnerIdent);
                if (!SecondIdent.IsEmpty())
                {
                    Out.SecondaryInnerName = FName(*SecondIdent);
                }
            }
            else
            {
                EIrTypeKind PrefixKind = EIrTypeKind::Unresolved;
                const bool bAcceptsCppPrefix = TryFindTypeAlias(
                    Ident, Grammar, EIrTypeAcceptForm::CppPrefix, PrefixKind, CanonicalInnerName);
                if (bAcceptsCppPrefix && (Next == TEXT(':') || bHasWhitespaceSeparator))
                {
                    if (Next == TEXT(':'))
                    {
                        ++St.Cur.Pos;
                        St.Cur.SkipWs();
                    }
                    const FString InnerIdent = St.Cur.ReadIdentifier();
                    if (InnerIdent.IsEmpty())
                    {
                        if (ApplyPrimitiveMapping(Ident, Grammar, Out))
                        {
                            St.Cur.Pos = AfterIdentPos;
                        }
                        else
                        {
                            const TCHAR Bad = St.Cur.Peek();
                            if (Bad == TEXT('\0'))
                            {
                                return St.Fail(TEXT("unexpected end of input"), St.Cur.Pos);
                            }
                            return St.FailUnexpected(Bad, St.Cur.Pos);
                        }
                    }
                    else
                    {
                        Out.Kind = PrefixKind;
                        Out.InnerName = FName(*InnerIdent);
                    }
                }
                else
                {
                    St.Cur.Pos = AfterIdentPos;

                    EIrTypeKind PointerKind = EIrTypeKind::Unresolved;
                    FName PointerInnerName;
                    if (St.Cur.Peek() == TEXT('*')
                        && TryFindTypeAlias(
                            Ident, Grammar, EIrTypeAcceptForm::PointerSuffix, PointerKind, PointerInnerName))
                    {
                        ++St.Cur.Pos;
                        Out.Kind = PointerKind;
                        Out.InnerName = PointerInnerName;
                    }
                    else if (ApplyPrimitiveMapping(Ident, Grammar, Out))
                    {
                    }
                    else
                    {
                        Out.Kind = EIrTypeKind::Unresolved;
                        Out.InnerName = FName(*Ident);
                    }
                }
            }
        }

        St.Cur.SkipWs();
        if (St.Cur.Peek() == TEXT('&'))
        {
            Out.bIsReference = true;
            ++St.Cur.Pos;
        }

        St.Cur.SkipWs();
        return true;
    }

    FString EmitBareType(const FIrTypeSpec& Spec, const FIrTypeGrammar& Grammar)
    {
        if (Spec.Container != EPinContainerType::None)
        {
            const FStringView ContainerText = Grammar.GetContainerText
                ? Grammar.GetContainerText(Spec.Container)
                : FStringView();
            if (ContainerText.IsEmpty())
            {
                return FString();
            }

            if (Spec.Container == EPinContainerType::Map)
            {
                const FString K = Spec.KeySpec.IsValid() ? TypeSpecToText(*Spec.KeySpec, Grammar) : FString();
                const FString V = Spec.ElementSpec.IsValid() ? TypeSpecToText(*Spec.ElementSpec, Grammar) : FString();
                return FString::Printf(TEXT("%s<%s, %s>"), *FString(ContainerText), *K, *V);
            }

            const FString Inner = Spec.ElementSpec.IsValid() ? TypeSpecToText(*Spec.ElementSpec, Grammar) : FString();
            return FString::Printf(TEXT("%s<%s>"), *FString(ContainerText), *Inner);
        }

        const FStringView TaggedPrefix = Grammar.GetTaggedPrefix
            ? Grammar.GetTaggedPrefix(Spec.Kind)
            : FStringView();
        if (!TaggedPrefix.IsEmpty())
        {
            if (Spec.InnerName.IsNone())
            {
                const FStringView Canon = Grammar.GetCanonicalText
                    ? Grammar.GetCanonicalText(Spec.Kind)
                    : FStringView();
                return Canon.IsEmpty() ? FString() : FString(Canon);
            }
            // Arity-2 tagged kinds (delegate / mcdelegate) carry a second
            // identifier in SecondaryInnerName; emit "<First, Second>".
            if (!Spec.SecondaryInnerName.IsNone())
            {
                return FString::Printf(TEXT("%s<%s, %s>"),
                    *FString(TaggedPrefix),
                    *Spec.InnerName.ToString(),
                    *Spec.SecondaryInnerName.ToString());
            }
            return FString::Printf(TEXT("%s<%s>"), *FString(TaggedPrefix), *Spec.InnerName.ToString());
        }

        const FStringView Canon = Grammar.GetCanonicalText
            ? Grammar.GetCanonicalText(Spec.Kind)
            : FStringView();
        if (!Canon.IsEmpty())
        {
            return FString(Canon);
        }

        if (Spec.Kind == EIrTypeKind::Unresolved)
        {
            return Spec.InnerName.IsNone() ? FString() : Spec.InnerName.ToString();
        }
        return FString();
    }
}

bool ParseTypeSpec(const FString& Source, const FIrTypeGrammar& Grammar, FIrTypeSpec& OutSpec, FString& OutError, int32& OutErrorColumn)
{
    OutSpec = FIrTypeSpec();
    OutError.Reset();
    OutErrorColumn = INDEX_NONE;

    FParseState St;
    St.Cur.Data = *Source;
    St.Cur.Len = Source.Len();
    St.Cur.Pos = 0;

    if (!ParseSpec(St, Grammar, OutSpec))
    {
        OutError = St.Error;
        OutErrorColumn = St.ErrorColumn;
        return false;
    }

    St.Cur.SkipWs();
    if (!St.Cur.Eof())
    {
        const TCHAR Bad = St.Cur.Peek();
        OutError = FString::Printf(TEXT("unexpected '%c' at column %d"), Bad, St.Cur.Pos);
        OutErrorColumn = St.Cur.Pos;
        OutSpec = FIrTypeSpec();
        return false;
    }

    return true;
}

FString TypeSpecToText(const FIrTypeSpec& Spec, const FIrTypeGrammar& Grammar)
{
    FString Out;
    if (Spec.bIsConst)
    {
        Out += TEXT("const ");
    }
    Out += EmitBareType(Spec, Grammar);
    if (Spec.bIsReference)
    {
        Out += TEXT("&");
    }
    return Out;
}

FString FormatTypeSpecErrorDetail(const FString& ParseError, int32 ParseErrCol)
{
    if (ParseErrCol >= 0)
    {
        return FString::Printf(TEXT("%s (col %d)"), *ParseError, ParseErrCol);
    }
    return ParseError;
}

} // namespace IrTypeSpecParser
