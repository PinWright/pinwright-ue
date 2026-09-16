// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

#include "PwDocument.h"
#include "PwParamSpec.h"
#include "PwToken.h"
#include "PwDiagnostic.h"
#include "PwValue.h"

enum class EPwParamAction : uint8
{
    Continue,
    Handled
};

struct FPwParamPolicy
{
    TFunction<EPwParamAction(const FString& Owner, const FString& Key, const FPwValue&)> OnParam;
    TFunction<EPwParamAction(const FString& Owner, const FPwParamSpec&, const FPwValue&)> OnValue;
    TFunction<void(const FString& Owner, const FPwParamSpec&, const FPwValue&)> OnValueChecked;
    const TCHAR* UnknownHint = nullptr;
};

struct IPwStatementSink
{
    virtual ~IPwStatementSink() = default;
    virtual void OnStatement(const FPwOp&, bool bIsFirstInList, bool bHadBlock) = 0;
    virtual IPwStatementSink& NestedSink() = 0;
};

struct PINWRIGHT_API FPwParseCursor
{
    const TArray<FPwToken>& Tokens;
    TArray<FPwDiagnostic>& Diags;
    int32 Pos = 0;
    FString ScopeLabel;
    FString ScopeName;

    FPwParseCursor(const TArray<FPwToken>& InTokens, TArray<FPwDiagnostic>& InDiags);

    const FPwToken& Peek(int32 Offset = 0) const;
    bool Check(EPwTokenType Type, int32 Offset = 0) const;
    bool CheckIdentifier(const TCHAR* Text, int32 Offset = 0) const;
    bool AtEnd() const;
    const FPwToken& Advance();
    bool Consume(EPwTokenType Type);
    void SkipNewlines();
    void SkipToNextLine();

    void Emit(EPwSeverity Severity, const TCHAR* Code, const FPwToken& At, FString Message,
              TArray<FString> Suggestions = {});
    void Error(const TCHAR* Code, const FPwToken& At, FString Message,
               TArray<FString> Suggestions = {});
    void Warn(const TCHAR* Code, const FPwToken& At, FString Message);
    void ErrorAtLine(const TCHAR* Code, int32 Line, int32 Column, FString Message);
    bool WasReportedByTokenizer() const;
    void UnexpectedToken(FString Expected);

    bool ParseNumberInto(TArray<double>& Out);
    bool ParseTupleBody(TArray<double>& Out);
    bool ParseValue(FPwValue& Out);
    bool ParseParams(TMap<FString, FPwValue>& OutParams, const FString& OwnerLabel);

    static int32 ExpectedTupleArity(EPwParamType Type);
    void BadValue(const FPwValue& Value, const FString& Owner, const FPwParamSpec& Spec);
    void ValidateRange(const FString& Owner, const FPwParamSpec& Spec, const FPwValue& Value);
    void ValidateValue(const FString& Owner, const FPwParamSpec& Spec, const FPwValue& Value,
                       const FPwParamPolicy& Policy = {});
    void ValidateParams(const FPwToken& MissingAt, const FString& Owner,
                        TArrayView<const FPwParamSpec> Specs,
                        const TMap<FString, FPwValue>& Params,
                        const FPwParamPolicy& Policy = {});

    // Builds the PWSRC_BAD_BLOCK message for a construct whose '{' is missing at the cursor.
    // These formats end a statement at the line break, so a '{' on the NEXT line is a new
    // statement rather than this construct's block -- and saying "requires a block" there is
    // false, because the block the author wrote is sitting one line below. Distinguishing the
    // two is the difference between an author who can fix the file and one who cannot.
    FString MissingBlockMessage(const TCHAR* Construct) const;

    void ForEachBlockEntry(const FPwToken& OpenBrace, TFunctionRef<void()> Body);
    void SkipBalancedBlock(const FPwToken& OpenBrace);
    void SkipUnknownConstruct();

    bool ParseOpStatement(FPwOp& OutOp, bool bIsFirstInList, IPwStatementSink& Sink);
    void ParseOpList(TArray<FPwOp>& OutOps, const FPwToken& OpenBrace, IPwStatementSink& Sink);

    bool ParseVersionHeader(const TCHAR* Keyword, const TCHAR* AcceptedVersions,
                            TFunctionRef<bool(int32)> IsAccepted, FPwDocumentHeader& OutHeader);
    bool ParseUse(TArrayView<const FString> ValidKinds, FPwUse& OutUse);
};
