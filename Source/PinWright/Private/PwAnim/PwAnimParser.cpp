// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwAnim/PwAnimParser.h"

#include "PwSource/PwParseCursor.h"
#include "PwSource/PwSuggest.h"
#include "PwSource/PwToken.h"
#include "PwSource/PwTokenizer.h"

namespace
{
    const TCHAR* PwAnimParser_AcceptedVersions()
    {
        return TEXT("0");
    }

    bool PwAnimParser_IsAcceptedVersion(int32 Version)
    {
        return Version == 0;
    }

    FPwParamSpec PwAnimParser_MakeParam(const TCHAR* Name, EPwParamType Type,
                                        bool bRequired, const TCHAR* Default,
                                        const TCHAR* Description)
    {
        FPwParamSpec Spec;
        Spec.Name = Name;
        Spec.Type = Type;
        Spec.bRequired = bRequired;
        Spec.Default = Default;
        Spec.Description = Description;
        return Spec;
    }

    TArray<FString> PwAnimParser_EaseValues()
    {
        TArray<FString> Values;
        Values.Add(TEXT("step"));
        Values.Add(TEXT("linear"));
        Values.Add(TEXT("ease_in"));
        Values.Add(TEXT("ease_out"));
        Values.Add(TEXT("ease_in_out"));
        return Values;
    }

    TArray<FPwParamSpec> PwAnimParser_BuildTimebaseParams()
    {
        TArray<FPwParamSpec> Params;

        FPwParamSpec Rate = PwAnimParser_MakeParam(
            TEXT("rate"), EPwParamType::Vector2, true, TEXT(""),
            TEXT("Rational frames per second as (numerator, denominator)."));
        Params.Add(MoveTemp(Rate));

        FPwParamSpec Frames = PwAnimParser_MakeParam(
            TEXT("frames"), EPwParamType::Integer, true, TEXT(""),
            TEXT("The inclusive animation frame count."));
        Frames.bHasRange = true;
        Frames.MinValue = 1.0;
        Frames.MaxValue = static_cast<double>(MAX_int32);
        Params.Add(MoveTemp(Frames));

        FPwParamSpec Loop = PwAnimParser_MakeParam(
            TEXT("loop"), EPwParamType::Bool, false, TEXT("false"),
            TEXT("Require the first and final samples to form a loop seam."));
        Params.Add(MoveTemp(Loop));

        return Params;
    }

    TArray<FPwParamSpec> PwAnimParser_BuildBoneHeaderParams()
    {
        TArray<FPwParamSpec> Params;
        FPwParamSpec Ease = PwAnimParser_MakeParam(
            TEXT("ease"), EPwParamType::Enum, false, TEXT("linear"),
            TEXT("Default interpolation for key segments in this bone."));
        Ease.AllowedValues = PwAnimParser_EaseValues();
        Params.Add(MoveTemp(Ease));
        return Params;
    }

    TArray<FPwParamSpec> PwAnimParser_BuildKeyParams()
    {
        TArray<FPwParamSpec> Params;

        Params.Add(PwAnimParser_MakeParam(
            TEXT("frame"), EPwParamType::Integer, true, TEXT(""),
            TEXT("Integer sample frame on the declared timebase.")));
        Params.Add(PwAnimParser_MakeParam(
            TEXT("at"), EPwParamType::Vector3, false, TEXT("reference pose"),
            TEXT("Local translation (x, y, z).")));
        Params.Add(PwAnimParser_MakeParam(
            TEXT("rotate"), EPwParamType::Vector3, false, TEXT("reference pose"),
            TEXT("Local rotation in roll, pitch, yaw degrees.")));
        Params.Add(PwAnimParser_MakeParam(
            TEXT("scale"), EPwParamType::Vector3, false, TEXT("reference pose"),
            TEXT("Local per-axis scale.")));

        FPwParamSpec Ease = PwAnimParser_MakeParam(
            TEXT("ease"), EPwParamType::Enum, false, TEXT("bone default"),
            TEXT("Interpolation for the segment from this key to the next."));
        Ease.AllowedValues = PwAnimParser_EaseValues();
        Params.Add(MoveTemp(Ease));

        return Params;
    }

    TArray<FPwParamSpec> PwAnimParser_BuildSyncMarkerParams()
    {
        TArray<FPwParamSpec> Params;
        Params.Add(PwAnimParser_MakeParam(
            TEXT("frame"), EPwParamType::Integer, true, TEXT(""),
            TEXT("Integer sample frame on the declared timebase.")));
        return Params;
    }

    FPwToken PwAnimParser_ValueToken(const FPwValue& Value)
    {
        FPwToken Token;
        Token.Line = Value.Line;
        Token.Column = Value.Column;
        return Token;
    }

    FPwToken PwAnimParser_OperationToken(const FPwOp& Op)
    {
        FPwToken Token;
        Token.Type = EPwTokenType::Identifier;
        Token.Text = Op.OpName;
        Token.Line = Op.Line;
        Token.Column = Op.Column;
        return Token;
    }

    bool PwAnimParser_TryWholeInt(const FPwValue* Value, int32& OutValue)
    {
        if (!Value || Value->Type != EPwValueType::Number
            || !FMath::IsNearlyEqual(Value->Number, FMath::RoundToDouble(Value->Number))
            || Value->Number < static_cast<double>(MIN_int32)
            || Value->Number > static_cast<double>(MAX_int32))
        {
            return false;
        }

        OutValue = FMath::RoundToInt(Value->Number);
        return true;
    }

    bool PwAnimParser_IsPositiveWhole(double Value)
    {
        return FMath::IsFinite(Value)
            && Value > 0.0
            && FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value))
            && Value <= static_cast<double>(MAX_int32);
    }
}

const FPwParamSpec* FPwAnimOpSpec::FindParam(const FString& ParamName) const
{
    return Params.FindByPredicate(
        [&ParamName](const FPwParamSpec& Candidate) { return Candidate.Name == ParamName; });
}

namespace PwAnimOpTable
{
    TArrayView<const FPwParamSpec> TimebaseParams()
    {
        static const TArray<FPwParamSpec> Params = PwAnimParser_BuildTimebaseParams();
        return Params;
    }

    TArrayView<const FPwParamSpec> BoneHeaderParams()
    {
        static const TArray<FPwParamSpec> Params = PwAnimParser_BuildBoneHeaderParams();
        return Params;
    }

    TArrayView<const FPwParamSpec> KeyParams()
    {
        static const TArray<FPwParamSpec> Params = PwAnimParser_BuildKeyParams();
        return Params;
    }

    TArrayView<const FPwParamSpec> SyncMarkerParams()
    {
        static const TArray<FPwParamSpec> Params = PwAnimParser_BuildSyncMarkerParams();
        return Params;
    }

    const TArray<FPwAnimOpSpec>& Get()
    {
        static const TArray<FPwAnimOpSpec> Specs = []()
        {
            FPwAnimOpSpec Key;
            Key.Name = TEXT("key");
            Key.Description = TEXT("A sparse transform sample on one integer animation frame.");
            Key.bAcceptsBlock = false;
            Key.bRequiresBlock = false;
            for (const FPwParamSpec& Param : KeyParams())
            {
                Key.Params.Add(Param);
            }

            TArray<FPwAnimOpSpec> Result;
            Result.Add(MoveTemp(Key));
            return Result;
        }();
        return Specs;
    }

    const FPwAnimOpSpec* Find(const FString& OpName)
    {
        return Get().FindByPredicate(
            [&OpName](const FPwAnimOpSpec& Candidate) { return Candidate.Name == OpName; });
    }

    TArray<FString> Names()
    {
        TArray<FString> Result;
        Result.Reserve(Get().Num());
        for (const FPwAnimOpSpec& Spec : Get())
        {
            Result.Add(Spec.Name);
        }
        return Result;
    }

    TArray<FString> TopLevelNames()
    {
        TArray<FString> Result;
        Result.Add(TEXT("use"));
        Result.Add(TEXT("timebase"));
        Result.Add(TEXT("bone"));
        Result.Add(TEXT("sync_marker"));
        return Result;
    }
}

namespace
{
    struct FPwAnimParserImpl final : FPwParseCursor, IPwStatementSink
    {
        FPwAnimDocument& Document;
        TMap<FString, int32> BoneDeclarationLines;
        TMap<int32, int32> KeyDeclarationLines;
        FPwAnimBone* ActiveBone = nullptr;
        FString CurrentBone;
        int32 FirstSkeletonUseLine = 0;
        int32 LastKeyFrame = 0;
        bool bHasLastKeyFrame = false;

        FPwAnimParserImpl(const TArray<FPwToken>& InTokens, FPwAnimDocument& InDocument,
                          TArray<FPwDiagnostic>& InDiagnostics)
            : FPwParseCursor(InTokens, InDiagnostics)
            , Document(InDocument)
        {
            SyncScope();
        }

        void SyncScope()
        {
            ScopeLabel = CurrentBone.IsEmpty() ? FString() : FString(TEXT("bone"));
            ScopeName = CurrentBone;
        }

        void ErrorScoped(const TCHAR* Code, const FPwToken& At, FString Message,
                         TArray<FString> Suggestions = {})
        {
            SyncScope();
            FPwParseCursor::Error(Code, At, MoveTemp(Message), MoveTemp(Suggestions));
        }

        void WarnScoped(const TCHAR* Code, const FPwToken& At, FString Message)
        {
            SyncScope();
            FPwParseCursor::Warn(Code, At, MoveTemp(Message));
        }

        void RestoreScope(const FString& PreviousBone)
        {
            CurrentBone = PreviousBone;
            SyncScope();
        }

        int32 DeclaredFrameCount() const
        {
            if (!Document.Timebase.IsSet())
            {
                return INDEX_NONE;
            }

            const FPwValue* Frames = Document.Timebase.GetValue().Params.Find(TEXT("frames"));
            int32 Result = INDEX_NONE;
            return PwAnimParser_TryWholeInt(Frames, Result) && Result > 0 ? Result : INDEX_NONE;
        }

        void ValidateRate(const FPwValue* Rate)
        {
            if (!Rate || Rate->Type != EPwValueType::Tuple || Rate->Tuple.Num() != 2)
            {
                return;
            }

            if (!PwAnimParser_IsPositiveWhole(Rate->Tuple[0])
                || !PwAnimParser_IsPositiveWhole(Rate->Tuple[1]))
            {
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE,
                    PwAnimParser_ValueToken(*Rate),
                    TEXT("Parameter 'rate' on 'timebase' expects two positive integer components, "
                         "for example (30, 1) or (30000, 1001)."));
            }
        }

        void ParseDocument()
        {
            CurrentBone.Reset();
            SyncScope();
            SkipNewlines();

            // A known sibling format deserves one precise diagnostic rather than a missing
            // header followed by a cascade of unknown constructs.  Unknown future formats
            // still use the shared missing-version diagnostic and remain format-neutral.
            if (CheckIdentifier(TEXT("pwmodel")) || CheckIdentifier(TEXT("pwskel")))
            {
                Error(PwAnimDiagnosticCodes::PWANIM_WRONG_FORMAT, Peek(),
                    TEXT("This source is not a .pwanim document; it starts with another "
                         "PinWright format header. Expected 'pwanim 0' on line 1."));
                return;
            }

            FPwParseCursor::ParseVersionHeader(TEXT("pwanim"), PwAnimParser_AcceptedVersions(),
                PwAnimParser_IsAcceptedVersion, Document.Header);

            while (true)
            {
                SkipNewlines();
                if (AtEnd())
                {
                    break;
                }

                const int32 Before = Pos;
                ParseTopLevelConstruct();
                if (Pos == Before)
                {
                    Advance();
                }
            }

            ValidateDocument();
        }

        void ParseTopLevelConstruct()
        {
            CurrentBone.Reset();
            SyncScope();

            if (!Check(EPwTokenType::Identifier))
            {
                UnexpectedToken(TEXT("use, timebase, bone or sync_marker"));
                SkipUnknownConstruct();
                return;
            }

            if (CheckIdentifier(TEXT("use")))
            {
                ParseUseStatement();
                return;
            }

            if (CheckIdentifier(TEXT("timebase")))
            {
                ParseTimebase();
                return;
            }

            if (CheckIdentifier(TEXT("sync_marker")))
            {
                ParseSyncMarker();
                return;
            }

            if (CheckIdentifier(TEXT("bone")))
            {
                ParseBone();
                return;
            }

            const FPwToken Keyword = Peek();
            const TArray<FString> ValidNames = PwAnimOpTable::TopLevelNames();
            TArray<FString> Suggestions;
            const FString Guess = PwSuggest::Closest(Keyword.Text, ValidNames);
            if (!Guess.IsEmpty())
            {
                Suggestions.Add(Guess);
            }

            ErrorScoped(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP, Keyword,
                FString::Printf(TEXT("Unknown .pwanim construct '%s'. Valid constructs: %s."),
                    *Keyword.Text, *FString::Join(ValidNames, TEXT(", "))),
                MoveTemp(Suggestions));
            SkipUnknownConstruct();
        }

        void ParseUseStatement()
        {
            TArray<FString> ValidKinds;
            ValidKinds.Add(TEXT("skeleton"));

            const int32 DiagnosticStart = Diags.Num();
            FPwUse Use;
            const bool bParsed = FPwParseCursor::ParseUse(
                TArrayView<const FString>(ValidKinds), Use);

            if (bParsed && !Use.Kind.IsEmpty() && Use.Kind != TEXT("skeleton"))
            {
                bool bReplaced = false;
                for (int32 Index = DiagnosticStart; Index < Diags.Num(); ++Index)
                {
                    FPwDiagnostic& Diagnostic = Diags[Index];
                    if (Diagnostic.Code == PwSourceDiagnosticCodes::PWSRC_BAD_VALUE
                        && Diagnostic.Line == Use.Line)
                    {
                        Diagnostic.Code = PwAnimDiagnosticCodes::PWANIM_UNSUPPORTED_USE_KIND;
                        Diagnostic.Message = FString::Printf(
                            TEXT("'.pwanim' does not support 'use %s'; only 'use skeleton from "
                                 "\"<asset path>\"' is valid."), *Use.Kind);
                        bReplaced = true;
                        break;
                    }
                }

                if (!bReplaced)
                {
                    FPwToken At;
                    At.Type = EPwTokenType::Identifier;
                    At.Text = Use.Kind;
                    At.Line = Use.Line;
                    At.Column = Use.Column;
                    ErrorScoped(PwAnimDiagnosticCodes::PWANIM_UNSUPPORTED_USE_KIND, At,
                        FString::Printf(
                            TEXT("'.pwanim' does not support 'use %s'; only 'use skeleton from "
                                 "\"<asset path>\"' is valid."), *Use.Kind));
                }
            }

            if (bParsed)
            {
                if (Use.Kind == TEXT("skeleton"))
                {
                    if (FirstSkeletonUseLine == 0)
                    {
                        FirstSkeletonUseLine = Use.Line;
                    }
                    else
                    {
                        FPwToken UseToken;
                        UseToken.Type = EPwTokenType::Identifier;
                        UseToken.Text = TEXT("use");
                        UseToken.Line = Use.Line;
                        UseToken.Column = Use.Column;
                        ErrorScoped(PwAnimDiagnosticCodes::PWANIM_DUPLICATE_SKELETON,
                            UseToken,
                            FString::Printf(
                                TEXT("A .pwanim document may reference only one skeleton; "
                                     "the first reference is on line %d and this duplicate is on line %d."),
                                FirstSkeletonUseLine, Use.Line));
                    }
                }

                Document.Uses.Add(MoveTemp(Use));
            }

            if (!Check(EPwTokenType::Newline) && !AtEnd())
            {
                UnexpectedToken(TEXT("end of line after a skeleton reference"));
                SkipToNextLine();
            }
        }

        void ParseTimebase()
        {
            const FPwToken Keyword = Advance();
            FPwOp Timebase;
            Timebase.OpName = Keyword.Text;
            Timebase.Line = Keyword.Line;
            Timebase.Column = Keyword.Column;

            const FString Owner(TEXT("timebase"));
            ParseParams(Timebase.Params, Owner);
            FPwParseCursor::ValidateParams(Keyword, Owner, PwAnimOpTable::TimebaseParams(),
                Timebase.Params);
            ValidateRate(Timebase.Params.Find(TEXT("rate")));

            if (Check(EPwTokenType::OpenBrace))
            {
                const FPwToken Brace = Advance();
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Brace,
                    TEXT("'timebase' does not accept a block."));
                SkipBalancedBlock(Brace);
            }

            if (Document.Timebase.IsSet())
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_DUPLICATE_TIMEBASE, Keyword,
                    FString::Printf(
                        TEXT("A .pwanim document may declare only one timebase; "
                             "the first declaration is on line %d."),
                        Document.Timebase.GetValue().Line));
            }
            else
            {
                Document.Timebase = MoveTemp(Timebase);
            }
        }

        void ParseSyncMarker()
        {
            const FPwToken Keyword = Advance();
            FPwAnimSyncMarker Marker;
            Marker.Line = Keyword.Line;
            Marker.Column = Keyword.Column;

            if (!Check(EPwTokenType::String))
            {
                UnexpectedToken(TEXT("a quoted sync marker name"));
                SkipUnknownConstruct();
                return;
            }

            const FPwToken NameToken = Advance();
            Marker.MarkerName = NameToken.Text;
            if (Marker.MarkerName.IsEmpty())
            {
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, NameToken,
                    TEXT("A sync marker name must not be an empty string."));
            }

            FPwOp Op;
            Op.OpName = Keyword.Text;
            Op.Line = Keyword.Line;
            Op.Column = Keyword.Column;
            const FString Owner = FString::Printf(TEXT("sync_marker \"%s\""), *Marker.MarkerName);
            ParseParams(Op.Params, Owner);
            FPwParseCursor::ValidateParams(Keyword, Owner,
                PwAnimOpTable::SyncMarkerParams(), Op.Params);

            if (const FPwValue* FrameValue = Op.Params.Find(TEXT("frame")))
            {
                int32 Frame = 0;
                if (PwAnimParser_TryWholeInt(FrameValue, Frame))
                {
                    Marker.Frame = Frame;
                }
            }

            if (Check(EPwTokenType::OpenBrace))
            {
                const FPwToken Brace = Advance();
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Brace,
                    TEXT("'sync_marker' does not accept a block."));
                SkipBalancedBlock(Brace);
            }

            // A syntactically recovered marker is retained for diagnostics, but a source with
            // any error never reaches the compiler. The optional array is the presence bit that
            // distinguishes an authored replacement from an old source with no marker syntax.
            if (!Document.SyncMarkers.IsSet())
            {
                Document.SyncMarkers = TArray<FPwAnimSyncMarker>();
            }
            Document.SyncMarkers->Add(MoveTemp(Marker));
        }

        bool ParseBone()
        {
            const FPwToken Keyword = Advance();
            FPwAnimBone Bone;
            Bone.Line = Keyword.Line;
            Bone.Column = Keyword.Column;

            if (!Check(EPwTokenType::String))
            {
                UnexpectedToken(TEXT("a quoted bone name"));
                SkipUnknownConstruct();
                return false;
            }

            const FPwToken NameToken = Advance();
            Bone.BoneName = NameToken.Text;
            if (Bone.BoneName.IsEmpty())
            {
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, NameToken,
                    TEXT("A bone name must not be an empty string."));
            }

            const FString PreviousBone = CurrentBone;
            CurrentBone = Bone.BoneName;
            SyncScope();

            if (!Bone.BoneName.IsEmpty() && BoneDeclarationLines.Contains(Bone.BoneName))
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_DUPLICATE_BONE, Keyword,
                    FString::Printf(
                        TEXT("Bone '%s' is declared more than once; the earlier declaration "
                             "is on line %d."),
                        *Bone.BoneName, BoneDeclarationLines.FindChecked(Bone.BoneName)));
            }
            else if (!Bone.BoneName.IsEmpty())
            {
                BoneDeclarationLines.Add(Bone.BoneName, Bone.Line);
            }

            const int32 HeaderStart = Pos;
            const bool bParamsParsed = ParseParams(Bone.Header,
                FString::Printf(TEXT("bone \"%s\""), *Bone.BoneName));

            FPwToken BraceToken;
            if (bParamsParsed)
            {
                if (!Check(EPwTokenType::OpenBrace))
                {
                    // See the matching note in PwSkelParser: bailing here used to skip the
                    // ValidateParams call below and drop every header diagnostic with it.
                    FPwParseCursor::ValidateParams(Keyword,
                        FString::Printf(TEXT("bone \"%s\""), *Bone.BoneName),
                        PwAnimOpTable::BoneHeaderParams(), Bone.Header);
                    ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Keyword,
                        MissingBlockMessage(TEXT("bone")));
                    SkipToNextLine();
                    RestoreScope(PreviousBone);
                    return false;
                }
                BraceToken = Advance();
            }
            else
            {
                // ParseParams recovers by consuming a malformed header line.  If that line
                // also contained the body brace, resume at that brace so one bad header value
                // does not consume the entire hierarchy.
                int32 Scan = HeaderStart;
                while (Scan < Pos && Tokens[Scan].Type != EPwTokenType::OpenBrace)
                {
                    ++Scan;
                }
                if (Scan >= Pos)
                {
                    RestoreScope(PreviousBone);
                    return false;
                }

                BraceToken = Tokens[Scan];
                Pos = Scan + 1;
            }

            FPwParseCursor::ValidateParams(Keyword,
                FString::Printf(TEXT("bone \"%s\""), *Bone.BoneName),
                PwAnimOpTable::BoneHeaderParams(), Bone.Header);

            FPwAnimBone* PreviousActiveBone = ActiveBone;
            ActiveBone = &Bone;
            KeyDeclarationLines.Reset();
            LastKeyFrame = 0;
            bHasLastKeyFrame = false;
            FPwParseCursor::ParseOpList(Bone.Keys, BraceToken, *this);
            ActiveBone = PreviousActiveBone;

            if (Bone.Keys.Num() == 0)
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_EMPTY_BONE, Keyword,
                    TEXT("A bone must contain at least one key; an empty animation track "
                         "would evaluate as identity."));
            }
            else
            {
                const FPwOp& LastKey = Bone.Keys.Last();
                if (const FPwValue* Ease = LastKey.Params.Find(TEXT("ease")))
                {
                    WarnScoped(PwAnimDiagnosticCodes::PWANIM_TRAILING_EASE,
                        PwAnimParser_ValueToken(*Ease),
                        TEXT("The final key's ease value governs no segment and has no effect."));
                }
            }

            Document.Bones.Add(MoveTemp(Bone));
            RestoreScope(PreviousBone);
            return true;
        }

        void ValidateKeyFrame(const FPwOp& Op)
        {
            const FPwValue* FrameValue = Op.Params.Find(TEXT("frame"));
            int32 Frame = 0;
            if (!PwAnimParser_TryWholeInt(FrameValue, Frame))
            {
                return;
            }

            const int32 MaxFrame = DeclaredFrameCount();
            if (Frame < 0 || (MaxFrame != INDEX_NONE && Frame > MaxFrame))
            {
                FString Limit;
                if (MaxFrame == INDEX_NONE)
                {
                    Limit = TEXT("the declared non-negative frame range");
                }
                else
                {
                    Limit = FString::Printf(TEXT("0 through %d"), MaxFrame);
                }
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE,
                    PwAnimParser_ValueToken(*FrameValue),
                    FString::Printf(TEXT("Parameter 'frame' on 'key' must be in %s, but found %d."),
                        *Limit, Frame));
            }

            if (const int32* ExistingLine = KeyDeclarationLines.Find(Frame))
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_DUPLICATE_KEY,
                    PwAnimParser_OperationToken(Op),
                    FString::Printf(
                        TEXT("Bone '%s' has more than one key at frame %d; the earlier key "
                             "is on line %d."),
                        *CurrentBone, Frame, *ExistingLine));
            }
            else
            {
                KeyDeclarationLines.Add(Frame, Op.Line);
            }

            if (bHasLastKeyFrame && Frame < LastKeyFrame)
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_KEYS_OUT_OF_ORDER,
                    PwAnimParser_OperationToken(Op),
                    FString::Printf(
                        TEXT("Keys for bone '%s' must be in ascending frame order; frame %d "
                             "follows frame %d."),
                        *CurrentBone, Frame, LastKeyFrame));
            }

            LastKeyFrame = Frame;
            bHasLastKeyFrame = true;
        }

        void ValidateSyncMarkerFrames()
        {
            if (!Document.SyncMarkers.IsSet())
            {
                return;
            }

            const int32 MaxFrame = DeclaredFrameCount();
            for (const FPwAnimSyncMarker& Marker : Document.SyncMarkers.GetValue())
            {
                if (Marker.Frame < 0 || (MaxFrame != INDEX_NONE && Marker.Frame > MaxFrame))
                {
                    const FString Limit = MaxFrame == INDEX_NONE
                        ? TEXT("the declared non-negative frame range")
                        : FString::Printf(TEXT("0 through %d"), MaxFrame);
                    FPwToken At;
                    At.Type = EPwTokenType::Identifier;
                    At.Text = TEXT("frame");
                    At.Line = Marker.Line;
                    At.Column = Marker.Column;
                    ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, At,
                        FString::Printf(
                            TEXT("Parameter 'frame' on 'sync_marker' must be in %s, but found %d."),
                            *Limit, Marker.Frame));
                }
            }
        }

        // IPwStatementSink implementation for key statements inside the active bone block.
        void OnStatement(const FPwOp& Op, bool bIsFirstInList, bool bHadBlock) override
        {
            if (!ActiveBone)
            {
                return;
            }

            if (Op.OpName != TEXT("key"))
            {
                const TArray<FString> ValidNames = PwAnimOpTable::Names();
                TArray<FString> Suggestions;
                const FString Guess = PwSuggest::Closest(Op.OpName, ValidNames);
                if (!Guess.IsEmpty())
                {
                    Suggestions.Add(Guess);
                }

                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP,
                    PwAnimParser_OperationToken(Op),
                    FString::Printf(
                        TEXT("Unknown .pwanim statement '%s' inside bone '%s'. Valid statements: %s."),
                        *Op.OpName, *CurrentBone, *FString::Join(ValidNames, TEXT(", "))),
                    MoveTemp(Suggestions));
                return;
            }

            const FPwToken At = PwAnimParser_OperationToken(Op);
            FPwParseCursor::ValidateParams(At, TEXT("key"), PwAnimOpTable::KeyParams(), Op.Params);

            if (bHadBlock)
            {
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, At,
                    TEXT("'key' does not accept a block."));
            }

            ValidateKeyFrame(Op);
            (void)bIsFirstInList;
        }

        IPwStatementSink& NestedSink() override
        {
            return *this;
        }

        void ValidateDocument()
        {
            FPwToken At;
            At.Line = Document.Header.VersionLine > 0 ? Document.Header.VersionLine : 1;
            At.Column = Document.Header.VersionColumn > 0 ? Document.Header.VersionColumn : 1;

            if (!Document.Timebase.IsSet())
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_MISSING_TIMEBASE, At,
                    TEXT("A .pwanim document requires one 'timebase rate=(…) frames=N' declaration."));
            }

            if (FirstSkeletonUseLine == 0)
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_MISSING_SKELETON, At,
                    TEXT("A .pwanim document requires one 'use skeleton from \"/Game/…\"' reference."));
            }

            if (Document.Bones.Num() == 0)
            {
                ErrorScoped(PwAnimDiagnosticCodes::PWANIM_NO_BONES, At,
                    TEXT("A .pwanim document declares at least one bone; there is no animation track to compile."));
            }

            ValidateSyncMarkerFrames();
        }
    };
}

bool FPwAnimParser::Parse(FStringView Source, FPwAnimDocument& OutDocument,
                          TArray<FPwDiagnostic>& OutDiagnostics)
{
    OutDocument = FPwAnimDocument();
    OutDiagnostics.Reset();

    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> LexDiagnostics;
    FPwTokenizer::Tokenize(Source, Tokens, LexDiagnostics);
    OutDiagnostics.Append(MoveTemp(LexDiagnostics));

    FPwAnimParserImpl Impl(Tokens, OutDocument, OutDiagnostics);
    Impl.ParseDocument();

    return !PwDiagnosticsHaveError(OutDiagnostics);
}
