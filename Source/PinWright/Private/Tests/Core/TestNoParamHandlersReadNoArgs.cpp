// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract test enforcing the param-spec discipline for zero-param handlers.
// Scans every handler source under Private/Handlers/ for REGISTER_RPC_HANDLER
// registrations whose 4th (params) macro argument is RPC_NO_PARAMS or an empty
// RPC_PARAMS() -- both compile to a zero-length TArray<FParamSpec> -- and fails
// if such a handler's body reads a caller-supplied argument through any
// FHandlerContext getter. A handler that reads args must declare them via
// RPC_PARAM_REQ/OPT/DEF so the params are documented in the wiki AND validated
// at dispatch time.
//
// Why a static scan and not runtime validation: FRpcDispatcher::
// ValidateHandlerParams only rejects unknown caller params when the registration
// declares at least one param (`Reg.Params.Num() > 0`). For a NO_PARAMS handler
// that check is skipped entirely, so an undeclared arg sails through validation
// and the handler silently reads it. Once params are declared, that runtime
// unknown-param rejection is the caller-side backstop; this test is the guard
// that forces the declaration to exist in the first place.
//
// Anti-pattern this prevents: recorder.list_sessions historically registered
// RPC_NO_PARAMS while its body read a `limit` arg -- so `limit` was neither
// documented nor validated. See its regression test
// Tests/Recorder/RecorderListSessionsLimitTest.cpp.
//
// Known limitation (same class the error-code registry test tolerates): this is
// a text heuristic scanning each handler's own brace body. A handler that reads
// caller args indirectly -- through a helper function defined elsewhere, or via a
// local alias of GetRawPayload() -- evades this scan. It catches the common,
// direct form only.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

// Named (not anonymous) namespace: under the plugin's Unity build these file-local
// scaffold helpers would otherwise ODR-collide with the identically-named anonymous
// helpers in TestErrorCodeRegistry.cpp (e.g. ResolveHandlersSourceDir) once the TUs
// merge. A unique namespace keeps them distinct.
namespace NoParamHandlerScan
{
    FString ResolveHandlersSourceDir()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright")
            / TEXT("Private") / TEXT("Handlers");
    }

    bool IsWhitespace(TCHAR C)
    {
        return C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\r')
            || C == TEXT('\n') || C == TEXT('\f') || C == TEXT('\v');
    }

    // From OpenIdx (which must index an Open delimiter) returns the index of the
    // matching Close delimiter, skipping string/char literals and //, /* */
    // comments so delimiters inside them do not affect the depth. INDEX_NONE if
    // unbalanced.
    int32 MatchDelimiter(const FString& S, int32 OpenIdx, TCHAR Open, TCHAR Close)
    {
        const int32 N = S.Len();
        int32 Depth = 0;
        for (int32 i = OpenIdx; i < N; )
        {
            const TCHAR C = S[i];
            if (C == TEXT('/') && i + 1 < N && S[i + 1] == TEXT('/'))
            {
                i += 2;
                while (i < N && S[i] != TEXT('\n')) { ++i; }
                continue;
            }
            if (C == TEXT('/') && i + 1 < N && S[i + 1] == TEXT('*'))
            {
                i += 2;
                while (i + 1 < N && !(S[i] == TEXT('*') && S[i + 1] == TEXT('/'))) { ++i; }
                i += 2;
                continue;
            }
            if (C == TEXT('"') || C == TEXT('\''))
            {
                const TCHAR Quote = C;
                ++i;
                while (i < N)
                {
                    if (S[i] == TEXT('\\')) { i += 2; continue; }
                    if (S[i] == Quote) { ++i; break; }
                    ++i;
                }
                continue;
            }
            if (C == Open)
            {
                ++Depth;
            }
            else if (C == Close)
            {
                --Depth;
                if (Depth == 0)
                {
                    return i;
                }
            }
            ++i;
        }
        return INDEX_NONE;
    }

    // Splits Inner (the text strictly between the macro's outer parens) on
    // top-level commas, skipping nested () [] {}, strings/chars, and comments so
    // commas inside RPC_PARAMS(...) or inside a quoted summary do not split.
    TArray<FString> SplitTopLevelArgs(const FString& Inner)
    {
        TArray<FString> Args;
        const int32 N = Inner.Len();
        int32 Depth = 0;
        int32 Start = 0;
        for (int32 i = 0; i < N; )
        {
            const TCHAR C = Inner[i];
            if (C == TEXT('/') && i + 1 < N && Inner[i + 1] == TEXT('/'))
            {
                i += 2;
                while (i < N && Inner[i] != TEXT('\n')) { ++i; }
                continue;
            }
            if (C == TEXT('/') && i + 1 < N && Inner[i + 1] == TEXT('*'))
            {
                i += 2;
                while (i + 1 < N && !(Inner[i] == TEXT('*') && Inner[i + 1] == TEXT('/'))) { ++i; }
                i += 2;
                continue;
            }
            if (C == TEXT('"') || C == TEXT('\''))
            {
                const TCHAR Quote = C;
                ++i;
                while (i < N)
                {
                    if (Inner[i] == TEXT('\\')) { i += 2; continue; }
                    if (Inner[i] == Quote) { ++i; break; }
                    ++i;
                }
                continue;
            }
            if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{'))
            {
                ++Depth;
            }
            else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}'))
            {
                --Depth;
            }
            else if (C == TEXT(',') && Depth == 0)
            {
                Args.Add(Inner.Mid(Start, i - Start));
                Start = i + 1;
            }
            ++i;
        }
        Args.Add(Inner.Mid(Start));
        return Args;
    }

    // From StartIdx, skips whitespace and comments to the first '{' and returns
    // the balanced brace block (braces included). Empty string when the next
    // significant token is not a '{' (e.g. the REGISTER_RPC_HANDLER macro
    // definition, whose ')' is followed by a line-continuation, not a body).
    FString ExtractBraceBody(const FString& S, int32 StartIdx)
    {
        const int32 N = S.Len();
        for (int32 i = StartIdx; i < N; )
        {
            const TCHAR C = S[i];
            if (IsWhitespace(C))
            {
                ++i;
                continue;
            }
            if (C == TEXT('/') && i + 1 < N && S[i + 1] == TEXT('/'))
            {
                i += 2;
                while (i < N && S[i] != TEXT('\n')) { ++i; }
                continue;
            }
            if (C == TEXT('/') && i + 1 < N && S[i + 1] == TEXT('*'))
            {
                i += 2;
                while (i + 1 < N && !(S[i] == TEXT('*') && S[i + 1] == TEXT('/'))) { ++i; }
                i += 2;
                continue;
            }
            if (C == TEXT('{'))
            {
                const int32 Close = MatchDelimiter(S, i, TEXT('{'), TEXT('}'));
                if (Close == INDEX_NONE)
                {
                    return FString();
                }
                return S.Mid(i, Close - i + 1);
            }
            return FString();
        }
        return FString();
    }

    // True when Trimmed is exactly RPC_PARAMS() with only whitespace inside the
    // parens (the empty-params spelling that also yields zero FParamSpecs).
    bool IsEmptyRpcParams(const FString& Trimmed)
    {
        const FString Prefix(TEXT("RPC_PARAMS("));
        if (!Trimmed.StartsWith(Prefix, ESearchCase::CaseSensitive)
            || !Trimmed.EndsWith(TEXT(")"), ESearchCase::CaseSensitive))
        {
            return false;
        }
        const FString Middle = Trimmed.Mid(Prefix.Len(), Trimmed.Len() - Prefix.Len() - 1);
        return Middle.TrimStartAndEnd().IsEmpty();
    }

    // Collects the distinct caller-argument reads found in a handler body. Every
    // token is a FHandlerContext getter keyed on a caller field name. The
    // non-arg accessors Ctx.GetSubsystem(, Ctx.GetRequestId(, Ctx.GetMethod( are
    // intentionally absent -- they read request plumbing, not caller args, so
    // they must not be flagged. Each token carries its trailing '(' so
    // Ctx.GetString( cannot match GetStringField/GetStringFirstOf/GetStringSet,
    // and anchoring on the Ctx. receiver keeps a plain .GetStringField( on some
    // other object from matching.
    void CollectBodyOffenses(const FString& Body, TArray<FString>& OutGetters)
    {
        static const TCHAR* const SimpleGetters[] =
        {
            TEXT("Ctx.GetString("), TEXT("Ctx.GetNumber("), TEXT("Ctx.GetBool("),
            TEXT("Ctx.GetInt("), TEXT("Ctx.GetVector("), TEXT("Ctx.GetRotator("),
            TEXT("Ctx.GetObject("), TEXT("Ctx.GetArray("),
            TEXT("Ctx.RequireString("), TEXT("Ctx.RequireAssetPath("),
            TEXT("Ctx.RequireInt("), TEXT("Ctx.RequireNumber("),
            TEXT("Ctx.RequireBool("), TEXT("Ctx.RequireObject("),
            TEXT("Ctx.RequireArray("),
            TEXT("Ctx.GetStringFirstOf("), TEXT("Ctx.GetBoolFirstOf("),
            TEXT("Ctx.GetIntFirstOf("), TEXT("Ctx.GetJsonValueFirstOf("),
            TEXT("Ctx.GetStringSet("), TEXT("Ctx.ReadFieldProjection(")
        };
        for (const TCHAR* const Token : SimpleGetters)
        {
            if (Body.Contains(Token, ESearchCase::CaseSensitive))
            {
                FString Name(Token);
                Name.RemoveFromEnd(TEXT("("), ESearchCase::CaseSensitive);
                OutGetters.AddUnique(Name);
            }
        }

        // Direct raw-payload field reads: Ctx.GetRawPayload()-><ident>Field(...)
        // (e.g. TryGetStringField, GetStringField, HasField) or ->Values.
        const FRegexPattern RawPattern(TEXT(
            "Ctx\\.GetRawPayload\\s*\\(\\s*\\)\\s*->\\s*([A-Za-z_][A-Za-z0-9_]*Field|Values)"));
        FRegexMatcher Matcher(RawPattern, Body);
        while (Matcher.FindNext())
        {
            OutGetters.AddUnique(FString::Printf(
                TEXT("Ctx.GetRawPayload()->%s"), *Matcher.GetCaptureGroup(1)));
        }
    }

    // Scans one source file for flagged registrations and their body offenses.
    void ScanFile(const FString& File, const FString& Contents,
        int32& FlaggedCount, int32& OffenderCount, TArray<FString>& Failures)
    {
        static const FString Token(TEXT("REGISTER_RPC_HANDLER("));
        const int32 TokenLen = Token.Len();
        int32 SearchStart = 0;
        while (true)
        {
            const int32 Idx = Contents.Find(Token, ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchStart);
            if (Idx == INDEX_NONE)
            {
                break;
            }
            // The token ends in '(', so the open paren is its last character.
            const int32 OpenParen = Idx + TokenLen - 1;
            const int32 CloseParen = MatchDelimiter(Contents, OpenParen, TEXT('('), TEXT(')'));
            if (CloseParen == INDEX_NONE)
            {
                SearchStart = Idx + TokenLen;
                continue;
            }
            SearchStart = CloseParen + 1;

            const FString Inner = Contents.Mid(OpenParen + 1, CloseParen - OpenParen - 1);
            const TArray<FString> Args = SplitTopLevelArgs(Inner);
            if (Args.Num() < 4)
            {
                continue; // not a well-formed (Method, Category, Summary, Params) call
            }

            // The params arg is the last macro argument (the 4th, for the fixed
            // 4-arg macro). Flag only the two zero-FParamSpec spellings.
            const FString ParamsArg = Args.Last().TrimStartAndEnd();
            const bool bNoParams =
                ParamsArg.Equals(TEXT("RPC_NO_PARAMS"), ESearchCase::CaseSensitive);
            const bool bEmptyParams = IsEmptyRpcParams(ParamsArg);
            if (!bNoParams && !bEmptyParams)
            {
                continue;
            }

            ++FlaggedCount;

            const FString Body = ExtractBraceBody(Contents, CloseParen + 1);
            if (Body.IsEmpty())
            {
                continue; // no brace body follows (e.g. the macro definition itself)
            }

            TArray<FString> Getters;
            CollectBodyOffenses(Body, Getters);
            if (Getters.Num() == 0)
            {
                continue;
            }

            FString Method = Args[0].TrimStartAndEnd();
            Method.RemoveFromStart(TEXT("\""), ESearchCase::CaseSensitive);
            Method.RemoveFromEnd(TEXT("\""), ESearchCase::CaseSensitive);

            const TCHAR* const Spelling =
                bNoParams ? TEXT("RPC_NO_PARAMS") : TEXT("an empty RPC_PARAMS()");
            for (const FString& Getter : Getters)
            {
                ++OffenderCount;
                Failures.Add(FString::Printf(
                    TEXT("Handler '%s' (%s) declares %s but reads caller arg via %s; "
                         "declare it with RPC_PARAM_REQ/OPT/DEF so it is documented and validated."),
                    *Method, *File, Spelling, *Getter));
            }
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoParamHandlersReadNoArgsTest,
    "PinWright.core.param_specs.NoParamHandlersReadNoArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoParamHandlersReadNoArgsTest::RunTest(const FString& Parameters)
{
    const FString HandlersDir = NoParamHandlerScan::ResolveHandlersSourceDir();
    if (!TestFalse(TEXT("Resolved handler source dir"), HandlersDir.IsEmpty()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Handler source dir exists on disk"),
            IFileManager::Get().DirectoryExists(*HandlersDir)))
    {
        return false;
    }

    TArray<FString> SourceFiles;
    IFileManager::Get().FindFilesRecursive(SourceFiles, *HandlersDir, TEXT("*.cpp"), true, false);
    IFileManager::Get().FindFilesRecursive(SourceFiles, *HandlersDir, TEXT("*.h"), true, false, false);

    int32 FlaggedCount = 0;
    int32 OffenderCount = 0;
    TArray<FString> Failures;

    for (const FString& File : SourceFiles)
    {
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *File))
        {
            continue;
        }
        NoParamHandlerScan::ScanFile(File, Contents, FlaggedCount, OffenderCount, Failures);
    }

    // A broken scanner (wrong dir, changed macro name) would pass vacuously with
    // nothing to check; assert we actually found zero-param registrations, as the
    // error-code registry test asserts it found literals to validate.
    TestTrue(TEXT("Found RPC_NO_PARAMS / empty RPC_PARAMS() registrations to validate"),
        FlaggedCount > 0);

    Failures.Sort();
    for (const FString& Line : Failures)
    {
        AddError(Line);
    }

    return TestEqual(
        TEXT("No RPC_NO_PARAMS / empty RPC_PARAMS() handler reads caller-supplied args"),
        OffenderCount, 0);
}
