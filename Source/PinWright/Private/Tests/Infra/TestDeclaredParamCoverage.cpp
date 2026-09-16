// Copyright (c) 2026 Alexander Penkin. MIT License.

// Registry-wide guard for one silent defect class: a verb whose handler READS a wire key its
// RPC_PARAMS never DECLARES.
//
// WHY IT IS SILENT. FRpcDispatcher::ValidateHandlerParams (Dispatch/RpcDispatcher.cpp) builds the
// accepted-name set from the registration alone -- Spec.Name, Spec.Aliases, Spec.TypedAliases[].Name
// -- and refuses any field outside it with UNKNOWN_PARAMS before the handler body runs. A body that
// reads an undeclared key therefore contains working code no caller can ever reach, and the verb's
// own tests do not notice: Tests/TestUtils.h's InvokeHandler calls Reg.Func(Ctx) directly and runs
// no part of the dispatcher, so the gate that produces the refusal is never exercised. Two
// contradictory statements about the same key -- the handler honours it, the wire rejects it -- and
// nothing in the suite compares them.
//
// It is not hypothetical. B-niagara-set-parameter-emitter-scope-unreachable was exactly this shape:
// three of niagara.set_parameter's six scopes live on an emitter, the whole resolution pipeline for
// them worked, only RPC_PARAM_OPT("emitter", ...) was missing, and the verb's tests were green.
// Sweeping that one namespace by hand found two more (niagara.add_parameter,
// niagara.remove_parameter). This test is what replaces the hand sweep.
//
// WHAT IT DOES. Reads the plugin's own handler sources off disk (the same technique as
// PinWright.core.error_codes.AllEmittedCodesAreRegistered and
// PinWright.infra.wiki_src.SourcePagesFollowRenderingRules, both of which already lint source text
// from an automation test), extracts every REGISTER_RPC_HANDLER body, collects the literal keys its
// body passes to Ctx.Get*/Ctx.Require*, and compares them against the ACCEPTED names of the live
// registration for that method. The declaration side comes from the registry rather than from the
// source text, so alias factories, shared param helpers and version-guarded declarations all
// resolve exactly rather than being re-parsed.
//
// WHY A BASELINE INSTEAD OF A CLEAN ASSERT. The sweep that opened this file recorded 66 (verb,
// key) pairs of this shape across sixteen files and about as many owners. Each is somebody's verb
// and some are deliberate -- a snake_case spelling never declared, an alias the author meant to
// support, a dead fallback -- so this test does not decide them. It records them, fails on anything
// NEW, and warns when a recorded pair stops reproducing so the list gets pruned rather than
// accreting. That is the ratchet: the count can only go down.
//
// THE SHAPES SCANNED, AND WHY THERE ARE FIVE OF THEM. The first version of this test matched ONE
// read shape -- Ctx.<accessor>("literal") -- and its baseline reached zero. An empty baseline on a
// one-shape scan reads as "the class is gone" while saying nothing about the other ways a body
// reads a wire key, and board B-declared-param-guard-blind-spots measured ~140 live pairs behind
// it. The four in-body shapes then landed and the baseline fell to 8 -- which read as
// near-eradicated while 434 pairs stood one call frame out (board
// B-declared-param-guard-blind-to-helpers). What is scanned now:
//   1. Ctx.<accessor>(key) for every key-taking FHandlerContext accessor (KeyTakingAccessors).
//   2. Ctx.ReadFieldProjection(...), which reads four wire keys of its own that appear at no call
//      site (FieldProjectionKeys). Its braced argument is COLUMN names and is deliberately not
//      read as keys.
//   3. Reads off a local bound from Ctx.GetRawPayload() -- a taint over assignment, no call graph
//      -- plus the unbound Ctx.GetRawPayload()->...Field(key) form, which needs no taint at all.
//   4. ONE BOUNDED CALL HOP. Keys read inside a helper the body hands Ctx (or the raw payload) to,
//      closed transitively over helper-to-helper forwarding of those same objects. See
//      FHelperIndex below for the resolution rules; KNOWN LIMITS for what it still cannot see.
//   5. Nothing else. See KNOWN LIMITS.
//
// KNOWN LIMITS, stated rather than hidden.
//   * FLOW INSENSITIVE. A helper that branches on a discriminator its caller fixes
//     (HandleAttachBTSubNode(Ctx, /*bDecorator=*/true)) contributes EVERY key it reads to EVERY
//     caller, including the keys that caller's branch cannot reach. Those pairs are real source
//     text and unreachable code at the same time; they are recorded in the baseline and marked
//     there rather than silently dropped, because dropping them needs per-call-site branch
//     evaluation the scanner does not do.
//   * ONE HELPER SHAPE. A call is followed only when an argument IS Ctx, IS the raw payload, or IS
//     a local bound from it -- not when it merely mentions one. Passing a NESTED object
//     (GetVectorFromJsonLS(GetObjectFieldLS(Payload, TEXT("instanceLocation")))) is deliberately
//     not followed: an earlier sweep that counted it produced 15 false x/y/z/pitch/roll/yaw pairs
//     on level.structure.*. A call whose name resolves to more than one candidate definition after
//     scope matching, or to none, is skipped rather than guessed at -- false negatives, never
//     false attributions.
//   * A key assembled at runtime (Ctx.GetString(SomeVariable)) is invisible where the variable is
//     a bare local -- the key is not in the source text at that site. It is NOT permanently
//     invisible as a class: 65 of the 94 such sites pass a named key-list factory
//     (MaterialHandlerUtils::MaterialAssetPathKeys(), WidgetAssetPathParamNames(), ...) whose
//     literals ARE in source and already feed the declaration side, so they surface nothing new.
//     Substituting call-site literals into a helper's own key parameter (stratum C on the board
//     ticket, 9 pairs) is not implemented here.
//   * NESTED KEYS ARE OUT OF SCOPE FOR THIS DIRECTION, AND MUST STAY THAT WAY. A read off an
//     object obtained from an object/array-typed parameter (Payload->GetObjectField(...) then
//     reading that) is not a top-level wire name, so diffing it against RPC_PARAMS would report
//     every nested key as an undeclared parameter -- the exact failure the 15 false
//     level.structure.* pairs were. Only reads whose receiver IS the payload are counted here.
//     Nested keys are collected SEPARATELY and checked by
//     PinWright.infra.declared_params.NestedKeysMatchTheirParameterDescriptions below.
//   * AT RUNTIME A NESTED KEY IS VALIDATED ONLY WHERE ITS PARAMETER DECLARES ONE. The first three
//     passes of FRpcDispatcher::ValidateHandlerParams iterate `for (const auto& Field :
//     Params->Values)` (Dispatch/RpcDispatcher.cpp) -- strictly one level, never descending into an
//     object or array value. A FOURTH pass reads FParamSpec::NestedKeys and refuses
//     UNKNOWN_NESTED_PARAMS one level down (Handlers/NestedParamKeyCheck.h), but ONLY for the
//     parameters that populate it: an empty NestedKeys means "undeclared", not "empty set", because
//     closing an object is a compatibility break that lands one parameter at a time. So for the
//     ~315 object/array parameters that have not adopted, the exclusion above is still not merely a
//     scanner limit -- it is the point at which the plugin stops checking caller input at all. The
//     adoption set is enumerated by PinWright.infra.dispatcher.NestedParamKeyGate.AdoptionSetIsRatcheted.
//     Board B-declared-param-guard-blind-to-nested-keys carries both halves; the test below is the
//     documentation half, which covers every verb rather than only the adopters.
//   * ONLY ONE OF THE TWO DIRECTIONS IS CHECKED AT THE TOP LEVEL. This test asks "is every key the
//     body reads declared". It does not ask "is every declared key read". A tree-wide measurement
//     of that inverse (blank every RPC_PARAM_* span, then ask whether the declared name occurs as
//     a literal anywhere in Source) returned exactly one hit across 1,218 verbs --
//     gas.set_ability_input:abilitySetPath, honestly labelled "only honored when
//     bindOn=ability_set" and unreachable because that mode returns NOT_IMPLEMENTED -- so the
//     direction is cheap but nearly empty at the top level; DeclaredParamsHaveSourceUse below now
//     implements it with that one measured exception as its ratchet baseline.
//   * A method not in the live registry is skipped, not failed: an integration sub-module whose
//     engine plugin is disabled on this host ships its source but registers nothing. A host with
//     integrations disabled therefore measures less than a full one and still reports a pass.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/Char.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with identically-shaped helpers in sibling
// tests (same convention as Tests/Infra/ParamSpecTestHelpers.h).
namespace DeclaredParamCoverageTestLocal
{
    // ------------------------------------------------------------------------
    // Source text normalization
    // ------------------------------------------------------------------------
    //
    // NeutralizeSourceText lives in Tests/TestUtils.h. It is shared with
    // PinWright.core.error_codes.RegistryAdoptingFilesUseConstantsOnly, which lints the same
    // handler sources and had the same comment blindness (board
    // B-error-code-adoption-test-scans-comments). Comment bodies and raw-string bodies are
    // blanked; ordinary string literals survive, which is what the read-site keys below are
    // collected from.

    // Index just past the delimiter matching the Open at Start, or INDEX_NONE. String literals are
    // still present in the neutralized text, so they are skipped here rather than counted.
    int32 MatchDelimiter(const FString& S, int32 Start, TCHAR Open, TCHAR Close)
    {
        const int32 Len = S.Len();
        int32 Depth = 0;
        for (int32 i = Start; i < Len; ++i)
        {
            const TCHAR C = S[i];
            if (C == TEXT('"') || C == TEXT('\''))
            {
                const TCHAR Quote = C;
                ++i;
                while (i < Len)
                {
                    if (S[i] == TEXT('\\'))
                    {
                        ++i;
                    }
                    else if (S[i] == Quote)
                    {
                        break;
                    }
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
                    return i + 1;
                }
            }
        }
        return INDEX_NONE;
    }

    // ------------------------------------------------------------------------
    // Read-site extraction
    // ------------------------------------------------------------------------

    // Every FHandlerContext accessor that takes a wire key as its first argument. The `*FirstOf`
    // members take a braced list of keys and are the shape most of the aliasing lives in.
    // ReadFieldProjection is deliberately absent -- its braced argument is COLUMN names, not wire
    // keys, so matching it here would report every projectable column as an undeclared parameter;
    // the four wire keys it reads internally are contributed by FieldProjectionKeys() below.
    // Ctx.GetRawPayload() is absent for the opposite reason -- it hands the body the whole payload
    // and there is no key on the call site; the raw-payload patterns below cover those reads.
    // The accessor names live in a list rather than inline in the pattern so the scan and the
    // ScannerSeesEveryCoveredReadShape assertion below read the SAME set: that test fails when a
    // key-taking accessor is added to Handlers/HandlerContext.h without being added here, which is
    // what stops this list from silently narrowing the way it did for GetJsonValueFirstOf.
    const TArray<FString>& KeyTakingAccessors()
    {
        static const TArray<FString> Names = {
            TEXT("GetString"), TEXT("GetNumber"), TEXT("GetBool"), TEXT("GetInt"),
            TEXT("GetVector"), TEXT("GetRotator"), TEXT("GetObject"), TEXT("GetArray"),
            TEXT("GetStringSet"), TEXT("GetStringFirstOf"), TEXT("GetBoolFirstOf"),
            TEXT("GetIntOr"), TEXT("GetIntFirstOf"), TEXT("GetJsonValueFirstOf"), TEXT("RequireString"),
            TEXT("RequireAssetPath"), TEXT("RequireInt"), TEXT("RequireNumber"),
            TEXT("RequireBool"), TEXT("RequireObject"), TEXT("RequireArray")
        };
        return Names;
    }

    // Receiver is the name the FHandlerContext is bound to: "Ctx" inside a REGISTER_RPC_HANDLER
    // body, the parameter's own name inside a helper that takes FHandlerContext&.
    FString MakeReadSitePattern(const FString& Receiver = TEXT("Ctx"))
    {
        return FString(TEXT("\\b")) + Receiver + TEXT("\\s*\\.\\s*(")
            + FString::Join(KeyTakingAccessors(), TEXT("|"))
            + TEXT(")\\s*\\(\\s*(\\{[^}]*\\}|TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")");
    }

    // FHandlerContext::ReadFieldProjection reads four wire keys of its own (HandlerContext.cpp:
    // `fields` and the singular `field` as the allow-list, `namesOnly`/`names_only` as the
    // shorthand). None of them appears at any call site, so a verb that calls the accessor accepts
    // exactly this set whether or not it declares it.
    const TCHAR* const FieldProjectionCallPattern =
        TEXT("\\bCtx\\s*\\.\\s*ReadFieldProjection\\s*\\(");

    FString MakeFieldProjectionPattern(const FString& Receiver)
    {
        return FString(TEXT("\\b")) + Receiver + TEXT("\\s*\\.\\s*ReadFieldProjection\\s*\\(");
    }

    const TArray<FString>& FieldProjectionKeys()
    {
        static const TArray<FString> Keys = {
            TEXT("fields"), TEXT("field"), TEXT("namesOnly"), TEXT("names_only")
        };
        return Keys;
    }

    // A body that binds Ctx.GetRawPayload() to a local reads its wire keys off that local instead
    // of through an accessor -- 34% of handler bodies touch it. Binding the variable and matching
    // only reads whose receiver IS that variable is a one-variable taint, not a call graph: a
    // nested sub-object read binds a different variable and drops out, which is why this stays
    // free of the false attributions that rule out resolving shared helpers (KNOWN LIMITS).
    const TCHAR* const RawPayloadBindPattern =
        TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*Ctx\\s*\\.\\s*GetRawPayload\\s*\\(\\s*\\)");

    // <boundvar>->HasField / TryGetStringField / GetNumberField / ... (TEXT("key"))
    const TCHAR* const RawPayloadMemberReadPattern =
        TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*->\\s*(?:Has|TryGet|Get)[A-Za-z]*Field\\s*\\(\\s*")
        TEXT("(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")");

    // Free functions that take the object first: GetJsonStringField(Payload, TEXT("key")) and its
    // siblings, plus the locally-named wrappers Niagara/NiagaraEditTypes.cpp reads everything
    // through (GetStringField(Payload, ...), TryGetIntField(...)). The pattern was
    // GetJson[A-Za-z]*Field until the helper sweep found the wrapper spellings; widening it costs
    // nothing at the top level (measured: still exactly the same 8 in-body pairs) and is what
    // makes the payload-helper side of the call hop see anything at all.
    const TCHAR* const RawPayloadHelperReadPattern =
        TEXT("\\b(?:Try)?Get[A-Za-z]*Field\\s*\\(\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*,\\s*")
        TEXT("(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")");

    FString MakeRawPayloadBindPattern(const FString& Receiver)
    {
        return FString(TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*")) + Receiver
            + TEXT("\\s*\\.\\s*GetRawPayload\\s*\\(\\s*\\)");
    }

    // The unbound form: <Receiver>.GetRawPayload()->HasField(TEXT("key")) read straight off the
    // call, with no local to taint. Needs no receiver check -- the receiver IS the payload.
    FString MakeRawPayloadChainedPattern(const FString& Receiver)
    {
        return Receiver
            + TEXT("\\s*\\.\\s*GetRawPayload\\s*\\(\\s*\\)\\s*->\\s*(?:Has|TryGet|Get)[A-Za-z]*Field")
            + TEXT("\\s*\\(\\s*(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")");
    }

    // One local aliasing another: `TSharedPtr<FJsonObject> Other = Payload;`. The taint follows it
    // so the payload keys read off the alias are not lost.
    const TCHAR* const LocalAliasPattern =
        TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*;");

    // Every double-quoted literal in Region, unescaped enough for a parameter name.
    void CollectQuotedLiterals(const FString& Region, TSet<FString>& OutKeys)
    {
        const int32 Len = Region.Len();
        for (int32 i = 0; i < Len; ++i)
        {
            if (Region[i] != TEXT('"'))
            {
                continue;
            }
            FString Literal;
            bool bClosed = false;
            ++i;
            while (i < Len)
            {
                if (Region[i] == TEXT('\\') && i + 1 < Len)
                {
                    Literal.AppendChar(Region[i + 1]);
                    i += 2;
                    continue;
                }
                if (Region[i] == TEXT('"'))
                {
                    bClosed = true;
                    break;
                }
                Literal.AppendChar(Region[i]);
                ++i;
            }
            if (bClosed && !Literal.IsEmpty())
            {
                OutKeys.Add(Literal);
            }
        }
    }

    void CollectReadKeys(const FRegexPattern& Pattern, const FString& Body, TSet<FString>& OutKeys)
    {
        FRegexMatcher Matcher(Pattern, Body);
        while (Matcher.FindNext())
        {
            CollectQuotedLiterals(Matcher.GetCaptureGroup(2), OutKeys);
        }
    }

    // Every literal key read off the object bound to VarName, in either the member form
    // (Var->TryGetStringField(TEXT("k"), ...)) or the free-function form (GetStringField(Var,
    // TEXT("k"))). Used for a raw payload local and for a helper's own TSharedPtr<FJsonObject>
    // parameter alike.
    void CollectKeysReadOffObject(const FRegexPattern& MemberPattern,
                                  const FRegexPattern& HelperPattern,
                                  const TSet<FString>& VarNames,
                                  const FString& Body, TSet<FString>& OutKeys)
    {
        if (VarNames.Num() == 0)
        {
            return;
        }
        {
            FRegexMatcher Matcher(MemberPattern, Body);
            while (Matcher.FindNext())
            {
                if (VarNames.Contains(Matcher.GetCaptureGroup(1)))
                {
                    CollectQuotedLiterals(Matcher.GetCaptureGroup(2), OutKeys);
                }
            }
        }
        {
            FRegexMatcher Matcher(HelperPattern, Body);
            while (Matcher.FindNext())
            {
                if (VarNames.Contains(Matcher.GetCaptureGroup(1)))
                {
                    CollectQuotedLiterals(Matcher.GetCaptureGroup(2), OutKeys);
                }
            }
        }
    }

    // The raw-payload patterns together: take the unbound chained reads outright, then bind the
    // locals assigned from <Receiver>.GetRawPayload(), follow plain local-to-local assignment so
    // an aliased payload stays tainted, and keep only the field reads whose receiver is one of
    // them. Still a taint over assignment, not a call graph -- a nested sub-object binds a
    // different variable through a DIFFERENT shape (GetObjectField) and drops out, which is what
    // keeps this free of the false attributions in KNOWN LIMITS.
    void CollectRawPayloadKeys(const FRegexPattern& MemberPattern,
                               const FRegexPattern& HelperPattern,
                               const FString& Receiver,
                               const FString& Body, TSet<FString>& OutKeys)
    {
        {
            const FRegexPattern ChainedPattern(MakeRawPayloadChainedPattern(Receiver));
            FRegexMatcher Matcher(ChainedPattern, Body);
            while (Matcher.FindNext())
            {
                CollectQuotedLiterals(Matcher.GetCaptureGroup(1), OutKeys);
            }
        }

        TSet<FString> Bound;
        {
            const FRegexPattern BindPattern(MakeRawPayloadBindPattern(Receiver));
            FRegexMatcher Matcher(BindPattern, Body);
            while (Matcher.FindNext())
            {
                Bound.Add(Matcher.GetCaptureGroup(1));
            }
        }
        if (Bound.Num() == 0)
        {
            return;
        }

        // `TSharedPtr<FJsonObject> Other = Payload;` -- bounded, four rounds is far more than any
        // real body needs and cannot loop forever on a self-assignment.
        const FRegexPattern AliasPattern(LocalAliasPattern);
        for (int32 Round = 0; Round < 4; ++Round)
        {
            bool bGrew = false;
            FRegexMatcher Matcher(AliasPattern, Body);
            while (Matcher.FindNext())
            {
                const FString Dest = Matcher.GetCaptureGroup(1);
                const FString Src = Matcher.GetCaptureGroup(2);
                if (Bound.Contains(Src) && !Bound.Contains(Dest))
                {
                    Bound.Add(Dest);
                    bGrew = true;
                }
            }
            if (!bGrew)
            {
                break;
            }
        }

        CollectKeysReadOffObject(MemberPattern, HelperPattern, Bound, Body, OutKeys);
    }

    // ------------------------------------------------------------------------
    // One-pass delimiter tables
    // ------------------------------------------------------------------------
    //
    // MatchDelimiter above is linear per opener, which is fine for the ~1,200 registration bodies
    // it is asked about. The call hop asks about every parenthesis in every source file, so it
    // takes one linear pass per file and answers from a map afterwards. String and character
    // literals are skipped exactly as MatchDelimiter skips them.
    struct FDelimiterTables
    {
        TMap<int32, int32> Paren;   // index of '(' -> index just past its ')'
        TMap<int32, int32> Brace;   // index of '{' -> index just past its '}'
    };

    void BuildDelimiterTables(const FString& S, FDelimiterTables& Out)
    {
        const int32 Len = S.Len();
        TArray<int32> ParenStack;
        TArray<int32> BraceStack;
        for (int32 i = 0; i < Len; ++i)
        {
            const TCHAR C = S[i];
            if (C == TEXT('"') || C == TEXT('\''))
            {
                const TCHAR Quote = C;
                ++i;
                while (i < Len)
                {
                    if (S[i] == TEXT('\\'))
                    {
                        ++i;
                    }
                    else if (S[i] == Quote)
                    {
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (C == TEXT('('))
            {
                ParenStack.Push(i);
            }
            else if (C == TEXT(')'))
            {
                if (ParenStack.Num() > 0)
                {
                    Out.Paren.Add(ParenStack.Pop(), i + 1);
                }
            }
            else if (C == TEXT('{'))
            {
                BraceStack.Push(i);
            }
            else if (C == TEXT('}'))
            {
                if (BraceStack.Num() > 0)
                {
                    Out.Brace.Add(BraceStack.Pop(), i + 1);
                }
            }
        }
    }

    // Split a parameter or argument list on its top-level commas. Angle brackets are tracked so
    // TMap<FString, int32> stays one item, but only when '<' abuts an identifier (so `a < b` is a
    // comparison, not a template) and only when '>' is not the tail of "->" (so Obj->Field() does
    // not unbalance the count -- that bug silently merged every argument list containing a member
    // call and cost the hop most of its attributions).
    void SplitTopLevelList(const FString& Inner, TArray<FString>& Out)
    {
        int32 Depth = 0;
        int32 Angle = 0;
        FString Cur;
        const int32 Len = Inner.Len();
        for (int32 i = 0; i < Len; ++i)
        {
            const TCHAR C = Inner[i];
            if (C == TEXT('"') || C == TEXT('\''))
            {
                const TCHAR Quote = C;
                const int32 Start = i;
                ++i;
                while (i < Len)
                {
                    if (Inner[i] == TEXT('\\'))
                    {
                        i += 2;
                        continue;
                    }
                    if (Inner[i] == Quote)
                    {
                        break;
                    }
                    ++i;
                }
                const int32 Stop = FMath::Min(i, Len - 1);
                Cur += Inner.Mid(Start, Stop - Start + 1);
                continue;
            }
            if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{'))
            {
                ++Depth;
            }
            else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}'))
            {
                if (Depth > 0)
                {
                    --Depth;
                }
            }
            else if (C == TEXT('<'))
            {
                const TCHAR Prev = (i > 0) ? Inner[i - 1] : TEXT(' ');
                if (FChar::IsAlnum(Prev) || Prev == TEXT('_') || Prev == TEXT('>'))
                {
                    ++Angle;
                }
            }
            else if (C == TEXT('>'))
            {
                const TCHAR Prev = (i > 0) ? Inner[i - 1] : TEXT(' ');
                if (Angle > 0 && Prev != TEXT('-'))
                {
                    --Angle;
                }
            }
            if (C == TEXT(',') && Depth == 0 && Angle == 0)
            {
                Out.Add(Cur.TrimStartAndEnd());
                Cur.Empty();
                continue;
            }
            Cur.AppendChar(C);
        }
        const FString Last = Cur.TrimStartAndEnd();
        if (!Last.IsEmpty())
        {
            Out.Add(Last);
        }
    }

    // Whitespace-stripped form, so `Ctx . GetRawPayload ()` and `Ctx.GetRawPayload()` compare equal.
    FString NormalizeExpr(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());
        for (int32 i = 0; i < In.Len(); ++i)
        {
            if (!FChar::IsWhitespace(In[i]))
            {
                Out.AppendChar(In[i]);
            }
        }
        return Out;
    }

    // Names that are followed by '(' but are not calls or definitions.
    const TSet<FString>& NonCallKeywords()
    {
        static const TSet<FString> Names = {
            TEXT("if"), TEXT("for"), TEXT("while"), TEXT("switch"), TEXT("catch"), TEXT("return"),
            TEXT("sizeof"), TEXT("do"), TEXT("else"), TEXT("case"), TEXT("and"), TEXT("or"),
            TEXT("not"), TEXT("decltype"), TEXT("static_assert"), TEXT("alignof"), TEXT("throw"),
            TEXT("new"), TEXT("delete")
        };
        return Names;
    }

    // ------------------------------------------------------------------------
    // The one bounded call hop
    // ------------------------------------------------------------------------
    //
    // RESOLUTION RULES, because "resolve the callee by name" is what produced the false
    // attributions two earlier offline sweeps were discredited for:
    //   * A qualified call (PinWright::MetaSound::BuildMetaSoundLiteralFromParams) must
    //     SCOPE-MATCH: the qualifier's segments must appear contiguously in the definition's
    //     enclosing namespace/class chain. `namespace A::B {` contributes two segments.
    //   * A definition that is `static` or sits in an anonymous namespace is file-local and is
    //     only a candidate for calls from the SAME file.
    //   * Definitions in the caller's own file win over definitions elsewhere.
    //   * More than one candidate left, or none, means the call is SKIPPED. Never guessed.
    //   * Obj.Name(...) / Obj->Name(...) is a member call on some other object and is not a call
    //     to the free or namespaced helper of that name.
    // The call must also actually hand over the object: an argument must BE Ctx, BE the raw
    // payload, or BE a local bound from it. Merely mentioning it is not enough.
    enum class EHelperParamKind : uint8
    {
        Other,
        Context,
        Payload
    };

    struct FScannedHelper
    {
        FString Name;
        FString File;
        bool bFileLocal = false;                    // static, or in an anonymous namespace
        TArray<FString> Scope;                      // enclosing namespace / class chain
        TArray<EHelperParamKind> ParamKinds;
        TArray<FString> ParamNames;
        FString Body;
        TMap<FString, TSet<FString>> KeysByParam;   // wire keys read off each ctx/payload param
    };

    struct FScopeBlock
    {
        int32 Open = 0;
        int32 Close = 0;
        TArray<FString> Names;                      // {""} for an anonymous namespace
    };

    void CollectScopeBlocks(const FString& Text, const FDelimiterTables& Tables,
                            TArray<FScopeBlock>& Out)
    {
        const FRegexPattern NamespacePattern(
            TEXT("\\bnamespace\\s+((?:[A-Za-z_][A-Za-z0-9_]*\\s*::\\s*)*[A-Za-z_][A-Za-z0-9_]*)\\s*\\{"));
        const FRegexPattern RecordPattern(
            TEXT("\\b(?:class|struct)\\s+(?:[A-Z][A-Z0-9_]*_API\\s+)?")
            TEXT("([A-Za-z_][A-Za-z0-9_]*)\\s*(?::[^;{}()]*)?\\{"));
        const FRegexPattern AnonymousPattern(TEXT("\\bnamespace\\s*\\{"));

        auto AddBlock = [&Out, &Tables](int32 OpenBrace, TArray<FString>&& Names)
        {
            const int32* const End = Tables.Brace.Find(OpenBrace);
            if (!End)
            {
                return;
            }
            FScopeBlock Block;
            Block.Open = OpenBrace;
            Block.Close = *End;
            Block.Names = MoveTemp(Names);
            Out.Add(MoveTemp(Block));
        };

        {
            FRegexMatcher Matcher(NamespacePattern, Text);
            while (Matcher.FindNext())
            {
                TArray<FString> Segments;
                NormalizeExpr(Matcher.GetCaptureGroup(1)).ParseIntoArray(Segments, TEXT("::"), true);
                AddBlock(Matcher.GetMatchEnding() - 1, MoveTemp(Segments));
            }
        }
        {
            FRegexMatcher Matcher(RecordPattern, Text);
            while (Matcher.FindNext())
            {
                AddBlock(Matcher.GetMatchEnding() - 1, { Matcher.GetCaptureGroup(1) });
            }
        }
        {
            FRegexMatcher Matcher(AnonymousPattern, Text);
            while (Matcher.FindNext())
            {
                AddBlock(Matcher.GetMatchEnding() - 1, { FString() });
            }
        }
        Out.Sort([](const FScopeBlock& A, const FScopeBlock& B) { return A.Open < B.Open; });
    }

    void EnclosingScope(const TArray<FScopeBlock>& Blocks, int32 At, TArray<FString>& Out,
                        bool& bOutAnonymous)
    {
        bOutAnonymous = false;
        for (const FScopeBlock& Block : Blocks)
        {
            if (Block.Open < At && At < Block.Close)
            {
                if (Block.Names.Num() == 1 && Block.Names[0].IsEmpty())
                {
                    bOutAnonymous = true;
                }
                Out.Append(Block.Names);
            }
        }
    }

    void ClassifyHelperParam(const FString& Param, EHelperParamKind& OutKind, FString& OutName)
    {
        static const FRegexPattern ContextPattern(
            TEXT("\\bFHandlerContext\\s*&\\s*([A-Za-z_][A-Za-z0-9_]*)"));
        static const FRegexPattern PayloadPattern(
            TEXT("\\bTSharedPtr\\s*<\\s*(?:const\\s+)?FJsonObject\\s*>\\s*(?:&\\s*)?")
            TEXT("([A-Za-z_][A-Za-z0-9_]*)"));
        static const FRegexPattern TrailingNamePattern(
            TEXT("([A-Za-z_][A-Za-z0-9_]*)\\s*(?:=[^,]*)?$"));

        {
            FRegexMatcher Matcher(ContextPattern, Param);
            if (Matcher.FindNext())
            {
                OutKind = EHelperParamKind::Context;
                OutName = Matcher.GetCaptureGroup(1);
                return;
            }
        }
        {
            FRegexMatcher Matcher(PayloadPattern, Param);
            if (Matcher.FindNext())
            {
                OutKind = EHelperParamKind::Payload;
                OutName = Matcher.GetCaptureGroup(1);
                return;
            }
        }
        OutKind = EHelperParamKind::Other;
        OutName.Empty();
        FRegexMatcher Matcher(TrailingNamePattern, Param);
        while (Matcher.FindNext())
        {
            OutName = Matcher.GetCaptureGroup(1);
        }
    }

    struct FResolvedCall
    {
        TArray<FString> Qualifier;
        FString Name;
        TArray<FString> Args;
    };

    // Every call written in Text, with its qualifier segments and top-level arguments.
    void CollectCalls(const FString& Text, const FDelimiterTables& Tables,
                      TArray<FResolvedCall>& Out)
    {
        const FRegexPattern CallPattern(
            TEXT("\\b((?:[A-Za-z_][A-Za-z0-9_]*\\s*::\\s*)*)([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
        FRegexMatcher Matcher(CallPattern, Text);
        while (Matcher.FindNext())
        {
            const FString Name = Matcher.GetCaptureGroup(2);
            if (NonCallKeywords().Contains(Name))
            {
                continue;
            }
            int32 Before = Matcher.GetMatchBeginning() - 1;
            while (Before >= 0 && FChar::IsWhitespace(Text[Before]))
            {
                --Before;
            }
            if (Before >= 0 && (Text[Before] == TEXT('.') || Text[Before] == TEXT('>')))
            {
                continue;   // Obj.Name(...) / Obj->Name(...)
            }
            const int32 OpenParen = Matcher.GetMatchEnding() - 1;
            const int32* const ClosePast = Tables.Paren.Find(OpenParen);
            if (!ClosePast)
            {
                continue;
            }
            FResolvedCall Call;
            NormalizeExpr(Matcher.GetCaptureGroup(1)).ParseIntoArray(Call.Qualifier, TEXT("::"), true);
            Call.Name = Name;
            SplitTopLevelList(Text.Mid(OpenParen + 1, *ClosePast - OpenParen - 2), Call.Args);
            Out.Add(MoveTemp(Call));
        }
    }

    bool ScopeChainMatches(const TArray<FString>& Chain, const TArray<FString>& Qualifier)
    {
        if (Qualifier.Num() == 0)
        {
            return true;
        }
        for (int32 i = 0; i + Qualifier.Num() <= Chain.Num(); ++i)
        {
            bool bAll = true;
            for (int32 j = 0; j < Qualifier.Num(); ++j)
            {
                if (!Chain[i + j].Equals(Qualifier[j]))
                {
                    bAll = false;
                    break;
                }
            }
            if (bAll)
            {
                return true;
            }
        }
        return false;
    }

    struct FHelperIndex
    {
        TArray<FScannedHelper> Helpers;
        TMap<FString, TArray<int32>> ByName;

        void AddFile(const FString& RelativeFile, const FString& NeutralizedText);
        void Close();
        int32 Resolve(const FString& Name, const TArray<FString>& Qualifier,
                      const FString& CallerFile) const;
        void CollectKeysForBody(const FString& Body, const FDelimiterTables& Tables,
                                const FString& CallerFile, const TSet<FString>& PayloadExprs,
                                TSet<FString>& OutKeys, TMap<FString, FString>* OutOrigin) const;
        void AttributeCall(const FScannedHelper& Callee, const TArray<FString>& Args,
                           const FString& ContextExpr, const TSet<FString>& PayloadExprs,
                           TSet<FString>& OutKeys) const;
    };

    void FHelperIndex::AddFile(const FString& RelativeFile, const FString& Text)
    {
        // Cheap pre-filter: no context and no payload in the file means no helper of either shape.
        if (!Text.Contains(TEXT("FHandlerContext"), ESearchCase::CaseSensitive)
            && !Text.Contains(TEXT("FJsonObject"), ESearchCase::CaseSensitive))
        {
            return;
        }

        FDelimiterTables Tables;
        BuildDelimiterTables(Text, Tables);
        TArray<FScopeBlock> Blocks;
        CollectScopeBlocks(Text, Tables, Blocks);

        const int32 Len = Text.Len();
        const FRegexPattern DefinitionPattern(TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
        const FRegexPattern QualifierPattern(
            TEXT("((?:[A-Za-z_][A-Za-z0-9_]*\\s*::\\s*)+)$"));

        FRegexMatcher Matcher(DefinitionPattern, Text);
        while (Matcher.FindNext())
        {
            const FString Name = Matcher.GetCaptureGroup(1);
            if (NonCallKeywords().Contains(Name))
            {
                continue;
            }
            const int32 OpenParen = Matcher.GetMatchEnding() - 1;
            const int32* const ClosePast = Tables.Paren.Find(OpenParen);
            if (!ClosePast)
            {
                continue;
            }
            const FString ParamsText = Text.Mid(OpenParen + 1, *ClosePast - OpenParen - 2);
            if (!ParamsText.Contains(TEXT("FHandlerContext"), ESearchCase::CaseSensitive)
                && !ParamsText.Contains(TEXT("FJsonObject"), ESearchCase::CaseSensitive))
            {
                continue;
            }

            int32 P = *ClosePast;
            while (P < Len && FChar::IsWhitespace(Text[P]))
            {
                ++P;
            }
            static const TCHAR* const TrailingKeywords[] = { TEXT("const"), TEXT("noexcept") };
            for (const TCHAR* const Keyword : TrailingKeywords)
            {
                const int32 KeywordLen = FCString::Strlen(Keyword);
                if (P + KeywordLen <= Len
                    && FCString::Strncmp(*Text + P, Keyword, KeywordLen) == 0)
                {
                    P += KeywordLen;
                    while (P < Len && FChar::IsWhitespace(Text[P]))
                    {
                        ++P;
                    }
                }
            }
            if (P >= Len || Text[P] != TEXT('{'))
            {
                continue;   // a declaration, or a call -- not a definition
            }
            const int32* const BodyEnd = Tables.Brace.Find(P);
            if (!BodyEnd)
            {
                continue;
            }

            FScannedHelper Helper;
            TArray<FString> Params;
            SplitTopLevelList(ParamsText, Params);
            bool bInteresting = false;
            for (const FString& Param : Params)
            {
                EHelperParamKind Kind = EHelperParamKind::Other;
                FString ParamName;
                ClassifyHelperParam(Param, Kind, ParamName);
                Helper.ParamKinds.Add(Kind);
                Helper.ParamNames.Add(ParamName);
                bInteresting |= (Kind != EHelperParamKind::Other);
            }
            if (!bInteresting)
            {
                continue;
            }

            Helper.Name = Name;
            Helper.File = RelativeFile;
            Helper.Body = Text.Mid(P, *BodyEnd - P);

            bool bAnonymous = false;
            EnclosingScope(Blocks, Matcher.GetMatchBeginning(), Helper.Scope, bAnonymous);
            // An out-of-line definition writes its own qualifier: void FCommonParams::Extract(...).
            const int32 PreStart = FMath::Max(0, Matcher.GetMatchBeginning() - 200);
            const FString Preceding = Text.Mid(PreStart, Matcher.GetMatchBeginning() - PreStart);
            FRegexMatcher QualifierMatcher(QualifierPattern, Preceding);
            while (QualifierMatcher.FindNext())
            {
                TArray<FString> Segments;
                NormalizeExpr(QualifierMatcher.GetCaptureGroup(1))
                    .ParseIntoArray(Segments, TEXT("::"), true);
                Helper.Scope.Append(Segments);
            }

            int32 LineStart = Matcher.GetMatchBeginning();
            while (LineStart > 0 && Text[LineStart - 1] != TEXT('\n'))
            {
                --LineStart;
            }
            const FString LinePrefix = Text.Mid(LineStart, Matcher.GetMatchBeginning() - LineStart);
            // The word boundary needs a DOUBLE backslash: a single one is the C++ backspace escape,
            // so the pattern compiles to <BS>static<BS>, never matches, and silently leaves every
            // `static` definition non-file-local -- which is how a file-local helper became a
            // resolution candidate for a caller in another file.
            static const FRegexPattern StaticKeywordPattern(TEXT("\\bstatic\\b"));
            FRegexMatcher StaticMatcher(StaticKeywordPattern, LinePrefix);
            Helper.bFileLocal = bAnonymous || StaticMatcher.FindNext();

            ByName.FindOrAdd(Name).Add(Helpers.Num());
            Helpers.Add(MoveTemp(Helper));
        }
    }

    int32 FHelperIndex::Resolve(const FString& Name, const TArray<FString>& Qualifier,
                                const FString& CallerFile) const
    {
        const TArray<int32>* const Candidates = ByName.Find(Name);
        if (!Candidates)
        {
            return INDEX_NONE;
        }
        TArray<int32> Keep;
        for (const int32 Index : *Candidates)
        {
            const FScannedHelper& Helper = Helpers[Index];
            if (Helper.bFileLocal && !Helper.File.Equals(CallerFile))
            {
                continue;
            }
            if (!ScopeChainMatches(Helper.Scope, Qualifier))
            {
                continue;
            }
            Keep.Add(Index);
        }
        TArray<int32> SameFile;
        for (const int32 Index : Keep)
        {
            if (Helpers[Index].File.Equals(CallerFile))
            {
                SameFile.Add(Index);
            }
        }
        const TArray<int32>& Final = SameFile.Num() > 0 ? SameFile : Keep;
        return Final.Num() == 1 ? Final[0] : INDEX_NONE;
    }

    void FHelperIndex::AttributeCall(const FScannedHelper& Callee, const TArray<FString>& Args,
                                     const FString& ContextExpr, const TSet<FString>& PayloadExprs,
                                     TSet<FString>& OutKeys) const
    {
        if (Args.Num() == Callee.ParamKinds.Num())
        {
            for (int32 i = 0; i < Args.Num(); ++i)
            {
                const FString Arg = NormalizeExpr(Args[i]);
                const FString& ParamName = Callee.ParamNames[i];
                if (Callee.ParamKinds[i] == EHelperParamKind::Context
                    && !ContextExpr.IsEmpty() && Arg.Equals(ContextExpr))
                {
                    if (const TSet<FString>* const Keys = Callee.KeysByParam.Find(ParamName))
                    {
                        OutKeys.Append(*Keys);
                    }
                }
                else if (Callee.ParamKinds[i] == EHelperParamKind::Payload
                    && PayloadExprs.Contains(Arg))
                {
                    if (const TSet<FString>* const Keys = Callee.KeysByParam.Find(ParamName))
                    {
                        OutKeys.Append(*Keys);
                    }
                }
            }
            return;
        }
        // Arity mismatch (default arguments, or a macro-expanded call): match only the context
        // parameter, and only by identity of one of the arguments.
        if (ContextExpr.IsEmpty())
        {
            return;
        }
        bool bPassesContext = false;
        for (const FString& Arg : Args)
        {
            bPassesContext |= NormalizeExpr(Arg).Equals(ContextExpr);
        }
        if (!bPassesContext)
        {
            return;
        }
        for (int32 i = 0; i < Callee.ParamKinds.Num(); ++i)
        {
            if (Callee.ParamKinds[i] == EHelperParamKind::Context)
            {
                if (const TSet<FString>* const Keys = Callee.KeysByParam.Find(Callee.ParamNames[i]))
                {
                    OutKeys.Append(*Keys);
                }
            }
        }
    }

    void FHelperIndex::Close()
    {
        const FRegexPattern MemberPattern(RawPayloadMemberReadPattern);
        const FRegexPattern HelperPattern(RawPayloadHelperReadPattern);
        const FRegexPattern AliasPattern(LocalAliasPattern);

        // Direct reads first: what each helper reads off its own context / payload parameters.
        for (FScannedHelper& Helper : Helpers)
        {
            for (int32 i = 0; i < Helper.ParamKinds.Num(); ++i)
            {
                const FString& ParamName = Helper.ParamNames[i];
                if (ParamName.IsEmpty())
                {
                    continue;
                }
                if (Helper.ParamKinds[i] == EHelperParamKind::Context)
                {
                    TSet<FString>& Keys = Helper.KeysByParam.FindOrAdd(ParamName);
                    const FRegexPattern ReadPattern(MakeReadSitePattern(ParamName));
                    CollectReadKeys(ReadPattern, Helper.Body, Keys);
                    const FRegexPattern ProjectionPattern(MakeFieldProjectionPattern(ParamName));
                    FRegexMatcher ProjectionMatcher(ProjectionPattern, Helper.Body);
                    if (ProjectionMatcher.FindNext())
                    {
                        for (const FString& Key : FieldProjectionKeys())
                        {
                            Keys.Add(Key);
                        }
                    }
                    CollectRawPayloadKeys(MemberPattern, HelperPattern, ParamName,
                        Helper.Body, Keys);
                }
                else if (Helper.ParamKinds[i] == EHelperParamKind::Payload)
                {
                    TSet<FString>& Keys = Helper.KeysByParam.FindOrAdd(ParamName);
                    TSet<FString> Vars;
                    Vars.Add(ParamName);
                    FRegexMatcher AliasMatcher(AliasPattern, Helper.Body);
                    while (AliasMatcher.FindNext())
                    {
                        if (AliasMatcher.GetCaptureGroup(2).Equals(ParamName))
                        {
                            Vars.Add(AliasMatcher.GetCaptureGroup(1));
                        }
                    }
                    CollectKeysReadOffObject(MemberPattern, HelperPattern, Vars, Helper.Body, Keys);
                }
            }
        }

        // Then close over helper-to-helper forwarding of the same objects. Six rounds is far more
        // than the deepest chain in the tree and terminates regardless.
        TArray<TArray<FResolvedCall>> CallsPerHelper;
        CallsPerHelper.SetNum(Helpers.Num());
        for (int32 h = 0; h < Helpers.Num(); ++h)
        {
            FDelimiterTables BodyTables;
            BuildDelimiterTables(Helpers[h].Body, BodyTables);
            CollectCalls(Helpers[h].Body, BodyTables, CallsPerHelper[h]);
        }

        for (int32 Round = 0; Round < 6; ++Round)
        {
            bool bGrew = false;
            for (int32 h = 0; h < Helpers.Num(); ++h)
            {
                FString ContextName;
                TArray<FString> OwnPayloads;
                for (int32 i = 0; i < Helpers[h].ParamKinds.Num(); ++i)
                {
                    if (Helpers[h].ParamKinds[i] == EHelperParamKind::Context)
                    {
                        ContextName = Helpers[h].ParamNames[i];
                    }
                    else if (Helpers[h].ParamKinds[i] == EHelperParamKind::Payload)
                    {
                        OwnPayloads.Add(Helpers[h].ParamNames[i]);
                    }
                }

                TSet<FString> PayloadExprs;
                if (!ContextName.IsEmpty())
                {
                    PayloadExprs.Add(ContextName + TEXT(".GetRawPayload()"));
                    const FRegexPattern BindPattern(MakeRawPayloadBindPattern(ContextName));
                    FRegexMatcher BindMatcher(BindPattern, Helpers[h].Body);
                    while (BindMatcher.FindNext())
                    {
                        PayloadExprs.Add(BindMatcher.GetCaptureGroup(1));
                    }
                }

                for (const FResolvedCall& Call : CallsPerHelper[h])
                {
                    if (Call.Qualifier.Num() == 0 && Call.Name.Equals(Helpers[h].Name))
                    {
                        continue;
                    }
                    const int32 CalleeIndex = Resolve(Call.Name, Call.Qualifier, Helpers[h].File);
                    if (CalleeIndex == INDEX_NONE || CalleeIndex == h)
                    {
                        continue;
                    }
                    if (!ContextName.IsEmpty())
                    {
                        TSet<FString> Got;
                        AttributeCall(Helpers[CalleeIndex], Call.Args, ContextName, PayloadExprs, Got);
                        TSet<FString>& Keys = Helpers[h].KeysByParam.FindOrAdd(ContextName);
                        for (const FString& Key : Got)
                        {
                            bool bAlready = false;
                            Keys.Add(Key, &bAlready);
                            bGrew |= !bAlready;
                        }
                    }
                    for (const FString& PayloadName : OwnPayloads)
                    {
                        TSet<FString> Forward;
                        Forward.Add(PayloadName);
                        TSet<FString> Got;
                        AttributeCall(Helpers[CalleeIndex], Call.Args, FString(), Forward, Got);
                        TSet<FString>& Keys = Helpers[h].KeysByParam.FindOrAdd(PayloadName);
                        for (const FString& Key : Got)
                        {
                            bool bAlready = false;
                            Keys.Add(Key, &bAlready);
                            bGrew |= !bAlready;
                        }
                    }
                }
            }
            if (!bGrew)
            {
                break;
            }
        }
    }

    void FHelperIndex::CollectKeysForBody(const FString& Body, const FDelimiterTables& Tables,
                                          const FString& CallerFile,
                                          const TSet<FString>& PayloadExprs,
                                          TSet<FString>& OutKeys,
                                          TMap<FString, FString>* OutOrigin) const
    {
        TArray<FResolvedCall> Calls;
        CollectCalls(Body, Tables, Calls);
        for (const FResolvedCall& Call : Calls)
        {
            const int32 CalleeIndex = Resolve(Call.Name, Call.Qualifier, CallerFile);
            if (CalleeIndex == INDEX_NONE)
            {
                continue;
            }
            TSet<FString> Got;
            AttributeCall(Helpers[CalleeIndex], Call.Args, TEXT("Ctx"), PayloadExprs, Got);
            for (const FString& Key : Got)
            {
                OutKeys.Add(Key);
                if (OutOrigin && !OutOrigin->Contains(Key))
                {
                    OutOrigin->Add(Key, FString::Printf(TEXT("%s (%s)"),
                        *Helpers[CalleeIndex].Name, *Helpers[CalleeIndex].File));
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // Nested keys: collected, never diffed against RPC_PARAMS
    // ------------------------------------------------------------------------
    //
    // A nested key lives in a different namespace from the accepted-name set the dispatcher
    // builds, so it MUST NOT be compared to it (KNOWN LIMITS). It is collected here so the
    // nested-schema test below can compare it against what the owning parameter's DESCRIPTION
    // promises -- the de facto schema, and the only thing a caller or an agent has to go on.
    struct FNestedRead
    {
        FString OwnerKey;     // the top-level parameter the nested object came out of
        FString NestedKey;

        bool operator==(const FNestedRead& Other) const
        {
            return OwnerKey.Equals(Other.OwnerKey) && NestedKey.Equals(Other.NestedKey);
        }

        // Hidden friend, matching the FZFQuantizedPoint precedent in MeshAuditUtils.cpp: it is
        // invisible to ordinary lookup, so the FString calls below cannot resolve back to it and
        // need no `::` qualifier -- which would break them, since FString's own GetTypeHash is
        // reachable only through ADL.
        friend uint32 GetTypeHash(const FNestedRead& Read)
        {
            return HashCombine(GetTypeHash(Read.OwnerKey), GetTypeHash(Read.NestedKey));
        }
    };

    // Exactly one literal in Region, or empty: a `{a, b}` first-of list names no single owner.
    FString SingleQuotedLiteral(const FString& Region)
    {
        TSet<FString> Literals;
        CollectQuotedLiterals(Region, Literals);
        return Literals.Num() == 1 ? *Literals.CreateConstIterator() : FString();
    }

    void CollectNestedReads(const FString& Body, const TSet<FString>& PayloadLocals,
                            TSet<FNestedRead>& Out)
    {
        // local name -> the top-level parameter its object was read out of
        TMap<FString, FString> Owners;

        {
            const FRegexPattern Pattern(
                TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*Ctx\\s*\\.\\s*(?:GetObject|GetArray)")
                TEXT("\\s*\\(\\s*(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")"));
            FRegexMatcher Matcher(Pattern, Body);
            while (Matcher.FindNext())
            {
                const FString Key = SingleQuotedLiteral(Matcher.GetCaptureGroup(2));
                if (!Key.IsEmpty())
                {
                    Owners.Add(Matcher.GetCaptureGroup(1), Key);
                }
            }
        }
        {
            const FRegexPattern Pattern(
                TEXT("\\bCtx\\s*\\.\\s*(?:RequireObject|RequireArray)\\s*\\(\\s*")
                TEXT("(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")\\s*,\\s*\\*?&?\\s*")
                TEXT("([A-Za-z_][A-Za-z0-9_]*)"));
            FRegexMatcher Matcher(Pattern, Body);
            while (Matcher.FindNext())
            {
                const FString Key = SingleQuotedLiteral(Matcher.GetCaptureGroup(1));
                if (!Key.IsEmpty())
                {
                    Owners.Add(Matcher.GetCaptureGroup(2), Key);
                }
            }
        }
        TArray<FString> SortedLocals = PayloadLocals.Array();
        SortedLocals.Sort();
        for (const FString& Local : SortedLocals)
        {
            {
                const FRegexPattern Pattern(FString(TEXT("\\b")) + Local
                    + TEXT("\\s*->\\s*(?:Try)?Get(?:Object|Array)Field\\s*\\(\\s*")
                    + TEXT("(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")\\s*,\\s*\\*?&?\\s*")
                    + TEXT("([A-Za-z_][A-Za-z0-9_]*)"));
                FRegexMatcher Matcher(Pattern, Body);
                while (Matcher.FindNext())
                {
                    const FString Key = SingleQuotedLiteral(Matcher.GetCaptureGroup(1));
                    if (!Key.IsEmpty())
                    {
                        Owners.Add(Matcher.GetCaptureGroup(2), Key);
                    }
                }
            }
            {
                const FRegexPattern Pattern(TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*") + Local
                    + TEXT("\\s*->\\s*(?:Try)?Get(?:Object|Array)Field\\s*\\(\\s*")
                    + TEXT("(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")"));
                FRegexMatcher Matcher(Pattern, Body);
                while (Matcher.FindNext())
                {
                    const FString Key = SingleQuotedLiteral(Matcher.GetCaptureGroup(2));
                    if (!Key.IsEmpty())
                    {
                        Owners.Add(Matcher.GetCaptureGroup(1), Key);
                    }
                }
            }
        }

        // An element taken out of an owning array, or a loop variable over one, stays attributed
        // to the same top-level parameter.
        const FRegexPattern ElementPattern(
            TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*")
            TEXT("(?:\\[[^\\]]*\\]\\s*)?(?:->|\\.)\\s*")
            TEXT("(?:AsObject|AsArray|Get|ToSharedRef|ToSharedPtr)\\s*\\("));
        const FRegexPattern RangeForPattern(
            TEXT("\\bfor\\s*\\(\\s*[^;()]*?\\b([A-Za-z_][A-Za-z0-9_]*)\\s*:\\s*")
            TEXT("\\*?\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*\\)"));
        for (int32 Round = 0; Round < 3; ++Round)
        {
            bool bGrew = false;
            {
                FRegexMatcher Matcher(ElementPattern, Body);
                while (Matcher.FindNext())
                {
                    const FString Dest = Matcher.GetCaptureGroup(1);
                    const FString* const Src = Owners.Find(Matcher.GetCaptureGroup(2));
                    if (Src && !Owners.Contains(Dest))
                    {
                        Owners.Add(Dest, *Src);
                        bGrew = true;
                    }
                }
            }
            {
                FRegexMatcher Matcher(RangeForPattern, Body);
                while (Matcher.FindNext())
                {
                    const FString Dest = Matcher.GetCaptureGroup(1);
                    const FString* const Src = Owners.Find(Matcher.GetCaptureGroup(2));
                    if (Src && !Owners.Contains(Dest))
                    {
                        Owners.Add(Dest, *Src);
                        bGrew = true;
                    }
                }
            }
            if (!bGrew)
            {
                break;
            }
        }

        TArray<FString> OwnerVars;
        Owners.GetKeys(OwnerVars);
        OwnerVars.Sort();
        for (const FString& Var : OwnerVars)
        {
            const FString& OwnerKey = Owners[Var];
            {
                const FRegexPattern Pattern(FString(
                    TEXT("(?:\\(\\s*\\*\\s*)?\\b")) + Var
                    + TEXT("\\s*\\)?\\s*->\\s*(?:Has|TryGet|Get)[A-Za-z]*Field\\s*\\(\\s*")
                    + TEXT("(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")"));
                FRegexMatcher Matcher(Pattern, Body);
                while (Matcher.FindNext())
                {
                    TSet<FString> Keys;
                    CollectQuotedLiterals(Matcher.GetCaptureGroup(1), Keys);
                    for (const FString& Key : Keys)
                    {
                        Out.Add(FNestedRead{ OwnerKey, Key });
                    }
                }
            }
            {
                const FRegexPattern Pattern(FString(
                    TEXT("\\b(?:Try)?Get[A-Za-z]*Field\\s*\\(\\s*\\*?\\s*")) + Var
                    + TEXT("\\s*,\\s*(TEXT\\s*\\(\\s*\"[^\"]*\"\\s*\\)|\"[^\"]*\")"));
                FRegexMatcher Matcher(Pattern, Body);
                while (Matcher.FindNext())
                {
                    TSet<FString> Keys;
                    CollectQuotedLiterals(Matcher.GetCaptureGroup(1), Keys);
                    for (const FString& Key : Keys)
                    {
                        Out.Add(FNestedRead{ OwnerKey, Key });
                    }
                }
            }
        }
    }

    // Nested keys a parameter's DESCRIPTION promises. A nested key has no declaration to diff
    // against -- RPC_PARAMS names top-level wire params only -- so the description IS the schema,
    // which is what a caller and an agent both read it as. Only a braced list is taken:
    // "{name, animation, isEntry, isExit}", "{ spacing: cm between lines, extent: cm half-size }".
    // Three deliberate narrowings, each of which removes a false-positive class:
    //   * braces containing another brace or a quote are skipped, so "Map of {ParamName: {r,g,b,a}}"
    //     yields r/g/b/a from the inner group and never reads ParamName as a promise;
    //   * an item must be a bare identifier, optionally followed by ": <prose>";
    //   * an identifier must be at least two characters, so the x/y/z of a vector shape are not
    //     mined as promises (they are not per-verb schema, they are the shared vector contract).
    // Backticked names are NOT read: descriptions backtick error codes and type names too.
    void PromisedNestedKeys(const FString& Description, TSet<FString>& Out)
    {
        static const FRegexPattern BracePattern(TEXT("\\{([^{}\"]{0,300})\\}"));
        static const FRegexPattern ItemPattern(
            TEXT("^([A-Za-z_][A-Za-z0-9_]{1,39})\\s*(?::[\\s\\S]*)?$"));
        FRegexMatcher Matcher(BracePattern, Description);
        while (Matcher.FindNext())
        {
            TArray<FString> Items;
            Matcher.GetCaptureGroup(1).ParseIntoArray(Items, TEXT(","), false);
            for (const FString& Raw : Items)
            {
                FString Item = Raw.TrimStartAndEnd();
                while (Item.StartsWith(TEXT("."), ESearchCase::CaseSensitive))
                {
                    Item.RightChopInline(1);
                }
                while (Item.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
                {
                    Item.LeftChopInline(1);
                }
                FRegexMatcher ItemMatcher(ItemPattern, Item);
                if (ItemMatcher.FindNext())
                {
                    Out.Add(ItemMatcher.GetCaptureGroup(1));
                }
            }
        }
    }

    void BlankRpcParamInvocations(FString& Text)
    {
        const FString Prefix(TEXT("RPC_PARAM_"));
        int32 SearchFrom = 0;
        while (SearchFrom < Text.Len())
        {
            const int32 Start = Text.Find(Prefix, ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchFrom);
            if (Start == INDEX_NONE)
            {
                break;
            }
            if (Start > 0 && (FChar::IsAlnum(Text[Start - 1]) || Text[Start - 1] == TEXT('_')))
            {
                SearchFrom = Start + Prefix.Len();
                continue;
            }

            int32 Open = Start + Prefix.Len();
            while (Open < Text.Len() && (FChar::IsAlnum(Text[Open]) || Text[Open] == TEXT('_')))
            {
                ++Open;
            }
            while (Open < Text.Len() && FChar::IsWhitespace(Text[Open]))
            {
                ++Open;
            }
            if (Open >= Text.Len() || Text[Open] != TEXT('('))
            {
                SearchFrom = Open;
                continue;
            }

            const int32 End = MatchDelimiter(Text, Open, TEXT('('), TEXT(')'));
            if (End == INDEX_NONE)
            {
                break;
            }
            for (int32 Index = Start; Index < End; ++Index)
            {
                if (Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
                {
                    Text[Index] = TEXT(' ');
                }
            }
            SearchFrom = End;
        }
    }

    bool FirstOuterNestedSchemaKeys(const FString& Description, TSet<FString>& Out)
    {
        Out.Reset();
        const int32 Open = Description.Find(TEXT("{"), ESearchCase::CaseSensitive);
        if (Open == INDEX_NONE)
        {
            return false;
        }
        const int32 End = MatchDelimiter(Description, Open, TEXT('{'), TEXT('}'));
        if (End == INDEX_NONE)
        {
            return false;
        }

        static const FRegexPattern KeyPattern(TEXT("^([A-Za-z_][A-Za-z0-9_]*)\\s*(?::|$)"));
        int32 ItemStart = Open + 1;
        int32 NestedDepth = 0;
        TCHAR Quote = 0;
        auto CollectItem = [&Description, &Out](int32 Start, int32 Stop)
        {
            const FString Item = Description.Mid(Start, Stop - Start).TrimStartAndEnd();
            FRegexMatcher Matcher(KeyPattern, Item);
            if (Matcher.FindNext())
            {
                Out.Add(Matcher.GetCaptureGroup(1).ToLower());
            }
        };

        for (int32 Index = Open + 1; Index < End - 1; ++Index)
        {
            const TCHAR C = Description[Index];
            if (Quote != 0)
            {
                if (C == TEXT('\\'))
                {
                    ++Index;
                }
                else if (C == Quote)
                {
                    Quote = 0;
                }
                continue;
            }
            if (C == TEXT('"') || C == TEXT('\''))
            {
                Quote = C;
            }
            else if (C == TEXT('{') || C == TEXT('['))
            {
                ++NestedDepth;
            }
            else if (C == TEXT('}') || C == TEXT(']'))
            {
                --NestedDepth;
            }
            else if (C == TEXT(',') && NestedDepth == 0)
            {
                CollectItem(ItemStart, Index);
                ItemStart = Index + 1;
            }
        }
        CollectItem(ItemStart, End - 1);
        return Out.Num() > 0;
    }

    // ------------------------------------------------------------------------
    // Registration extraction
    // ------------------------------------------------------------------------

    struct FScannedVerb
    {
        FString Method;
        FString File;
        TSet<FString> ReadKeys;                 // top-level wire keys read in the body
        TSet<FString> HelperReadKeys;           // top-level wire keys read one call frame out
        TMap<FString, FString> HelperOrigin;    // key -> "HelperName (file)"
        TSet<FNestedRead> NestedReads;          // NEVER diffed against RPC_PARAMS
    };

    // Helpers is optional: null scans the four in-body shapes only, which is what the shape
    // assertions below want when they feed the scanner one synthetic file at a time.
    void ScanFile(const FString& RelativeFile, const FString& RawText, TArray<FScannedVerb>& Out,
                  const FHelperIndex* Helpers = nullptr)
    {
        const FString Text = NeutralizeSourceText(RawText);
        const FString Macro(TEXT("REGISTER_RPC_HANDLER"));
        const int32 TextLen = Text.Len();

        // Compiled once per file rather than once per registration: the tree carries ~1200 of
        // them and ICU pattern compilation is the expensive half of this scan.
        const FRegexPattern ReadPattern(MakeReadSitePattern());
        const FRegexPattern MethodPattern(TEXT("^\\(\\s*\"([A-Za-z0-9_.]+)\""));
        const FRegexPattern FieldProjectionPattern(FieldProjectionCallPattern);
        const FRegexPattern RawBindPattern(RawPayloadBindPattern);
        const FRegexPattern RawMemberPattern(RawPayloadMemberReadPattern);
        const FRegexPattern RawHelperPattern(RawPayloadHelperReadPattern);

        int32 At = Text.Find(Macro, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
        while (At != INDEX_NONE)
        {
            const int32 Next = At + Macro.Len();
            const bool bLeftOk = (At == 0)
                || !(FChar::IsAlnum(Text[At - 1]) || Text[At - 1] == TEXT('_'));

            // Step past a trailing identifier tail (REGISTER_RPC_HANDLER_INNER) and whitespace.
            int32 P = Next;
            while (P < TextLen && (FChar::IsAlnum(Text[P]) || Text[P] == TEXT('_')))
            {
                ++P;
            }
            while (P < TextLen && FChar::IsWhitespace(Text[P]))
            {
                ++P;
            }

            if (bLeftOk && P < TextLen && Text[P] == TEXT('('))
            {
                const int32 ArgsEnd = MatchDelimiter(Text, P, TEXT('('), TEXT(')'));
                if (ArgsEnd != INDEX_NONE)
                {
                    // First macro argument is the dotted method name literal. Anything else (the
                    // _INNER form, whose first argument is a macro parameter) drops out here.
                    const FString MacroArgs = Text.Mid(P, ArgsEnd - P);
                    FRegexMatcher MethodMatcher(MethodPattern, MacroArgs);
                    if (MethodMatcher.FindNext())
                    {
                        int32 B = ArgsEnd;
                        while (B < TextLen && FChar::IsWhitespace(Text[B]))
                        {
                            ++B;
                        }
                        if (B < TextLen && Text[B] == TEXT('{'))
                        {
                            const int32 BodyEnd = MatchDelimiter(Text, B, TEXT('{'), TEXT('}'));
                            if (BodyEnd != INDEX_NONE)
                            {
                                const FString Body = Text.Mid(B, BodyEnd - B);
                                FScannedVerb Verb;
                                Verb.Method = MethodMatcher.GetCaptureGroup(1);
                                Verb.File = RelativeFile;
                                CollectReadKeys(ReadPattern, Body, Verb.ReadKeys);
                                CollectRawPayloadKeys(RawMemberPattern, RawHelperPattern,
                                    TEXT("Ctx"), Body, Verb.ReadKeys);
                                FRegexMatcher ProjectionMatcher(FieldProjectionPattern, Body);
                                if (ProjectionMatcher.FindNext())
                                {
                                    for (const FString& Key : FieldProjectionKeys())
                                    {
                                        Verb.ReadKeys.Add(Key);
                                    }
                                }

                                // The locals assigned DIRECTLY from Ctx.GetRawPayload(). The
                                // alias taint inside CollectRawPayloadKeys deliberately does not
                                // feed this set: handing a helper an aliased payload is a shape
                                // nothing in the tree uses, and widening identity is how a hop
                                // starts following objects that are not the payload.
                                TSet<FString> PayloadLocals;
                                {
                                    FRegexMatcher BindMatcher(RawBindPattern, Body);
                                    while (BindMatcher.FindNext())
                                    {
                                        PayloadLocals.Add(BindMatcher.GetCaptureGroup(1));
                                    }
                                }

                                // Shape 4: one call hop. The payload-identity set is exactly
                                // Ctx.GetRawPayload() plus those locals -- a call that hands over
                                // a NESTED object is not followed (KNOWN LIMITS).
                                if (Helpers)
                                {
                                    TSet<FString> PayloadExprs = PayloadLocals;
                                    PayloadExprs.Add(TEXT("Ctx.GetRawPayload()"));
                                    FDelimiterTables BodyTables;
                                    BuildDelimiterTables(Body, BodyTables);
                                    Helpers->CollectKeysForBody(Body, BodyTables, RelativeFile,
                                        PayloadExprs, Verb.HelperReadKeys, &Verb.HelperOrigin);
                                }

                                // Nested reads are collected for the nested-schema test and are
                                // deliberately absent from ReadKeys / HelperReadKeys.
                                CollectNestedReads(Body, PayloadLocals, Verb.NestedReads);
                                Out.Add(MoveTemp(Verb));
                            }
                        }
                    }
                }
            }

            At = Text.Find(Macro, ESearchCase::CaseSensitive, ESearchDir::FromStart, Next);
        }
    }

    // ------------------------------------------------------------------------
    // The recorded baseline
    // ------------------------------------------------------------------------

    // "<method>:<undeclared key the body reads>". Produced by a full scan of the tree on
    // 2026-08-28 and verified read-site by read-site against the source. Every entry is a verb
    // whose handler honours a key the dispatcher refuses; NONE of them is endorsed here.
    //
    // TO FIX ONE: add the key to that verb's RPC_PARAMS -- as an alias on the slot it feeds when
    // it is an alternate spelling of an existing parameter (RPC_PARAM_* plus Aliases, or the
    // ParamAliasUtils factories), as its own RPC_PARAM_OPT when it is a distinct input -- or delete
    // the dead read. Then remove the line here. Removing a fixed entry is required, not optional:
    // a stale entry is reported below so the list cannot rot into a permanent exemption.
    //
    // WHY THIS LIST IS NOT EMPTY AGAIN. All 66 pairs of the original sweep were decided, and for a
    // while the list stood empty -- which read as "the class is eradicated" while it was only the
    // one read shape the scanner could see that was clean (board
    // B-declared-param-guard-blind-spots). Widening the scan to the omitted accessors, the
    // ReadFieldProjection key set and raw-payload reads surfaced 77 more pairs; 69 were decided in
    // the same pass (11 verbs' names_only/field, sequencer.set_playhead's frame/time aliases,
    // twelve level.* fallback spellings, landscape.create's grid, effect.draw_debug_shape's
    // per-shape geometry, actor.set_collision, behavior_tree.attach_decorator/attach_service).
    // These eight are what is left, and they are one verb: environment.build forwards its
    // sub-action payload wholesale, so every forwarded key is refused before the body runs. That is
    // its own defect with its own ticket (B-environment-build-dispatcher-rejects-forwarded-params)
    // and a different fix -- the verb needs a payload-forwarding contract, not eight
    // RPC_PARAM_OPT lines -- so it is recorded here rather than papered over.
    const TArray<FString>& KnownUndeclaredReads()
    {
        static const TArray<FString> Entries = {
            TEXT("environment.build:foliageType"),
            TEXT("environment.build:name"),
            TEXT("environment.build:names"),
            TEXT("environment.build:removeAll"),
            TEXT("environment.build:transforms"),
            TEXT("environment.build:x"),
            TEXT("environment.build:y"),
            TEXT("environment.build:z"),
        };
        return Entries;
    }

    // "<method>:<undeclared key a helper reads on the verb's behalf>". The same shape as the list
    // above and the same ratchet, kept separate only so the two measurements stay distinguishable:
    // these are the pairs the four in-body shapes could never see, and the reason the 8-entry list
    // above read as near-eradication for a week (board B-declared-param-guard-blind-to-helpers).
    //
    // THIS LIST IS LARGE ON PURPOSE, AND IT IS NOT A LICENCE. Each entry is a verb whose handler
    // honours a key FRpcDispatcher::ValidateHandlerParams refuses. None is endorsed. Shrinking it
    // is per-namespace work with a decision behind every line -- declare the key on the slot it
    // feeds, or delete the read -- and NOT a blanket sweep of RPC_PARAM_OPT lines, which would
    // manufacture a contract the verbs do not honour. The families, largest first:
    //
    //   * niagara.* (287). Eight Parse*Payload helpers all route through ParseTargetSpec
    //     (Niagara/NiagaraEditTypes.cpp), which reads the FLAT form of the target descriptor off
    //     the top-level payload as a fallback -- targetKind, emitter, emitterName,
    //     scriptUsage, scriptType, nodeId, node, pin, pinName, entryId, moduleId,
    //     rendererIndex, toIndex. No niagara verb declares more than a handful. This is ONE design
    //     decision, not 287 bugs: the flat target form is either declared once through a shared
    //     RPC_PARAMS factory on every verb that routes through the parser, or deleted in favour of
    //     the nested `target` object, which is the only reachable form today.
    //     Two of the flat keys are already off this list and must not come back: `scope` was
    //     deleted (ResolveParameterStore is its only consumer and every verb that reaches it
    //     builds its spec from its own parser's `scope`, so the fallback fed nobody), and `index`
    //     moved out of ParseTargetSpec into ParseRendererOrdinalPayload -- the wrapper only
    //     niagara.remove_renderer / niagara.move_renderer call, which is narrower than
    //     ParseRendererPayload (niagara.add_renderer calls that too, appends, and never reads
    //     Target.Index). Board B-declared-param-guard-blind-to-helpers.
    //     FOUR MORE NIAGARA PAIRS ARE THE FLOW-INSENSITIVITY ARTEFACT named in KNOWN LIMITS -- a
    //     shared parser reads the key unconditionally and only the SIBLING operation consumes it,
    //     so the key cannot arrive at the recorded verb and be honoured. Each with the sibling that
    //     owns the read:
    //       - niagara.add_event_handler:index -- ParseEventHandlerPayload's identity read, owned by
    //         niagara.remove_event_handler (which declares `index` as an alias on eventHandlerIndex);
    //         the add body appends and never reads FNiagaraEventHandlerEditPayload::Index.
    //       - niagara.add_simulation_stage:index -- ParseSimulationStagePayload's identity read,
    //         owned by niagara.remove_simulation_stage (alias on stageIndex); the add body inserts
    //         with its own declared `atIndex` and never reads ::Index.
    //       - niagara.remove_event_handler:source -- ParseEventHandlerPayload's event-config read,
    //         owned by niagara.add_event_handler, which declares `source` and parses it into
    //         FNiagaraEventScriptProperties::SourceEmitterID; the remove body never reads ::Source.
    //       - niagara.remove_parameter:type -- ParseParameterPayload's type read, owned by
    //         niagara.set_parameter / niagara.add_parameter, which declare `type` REQUIRED;
    //         ValidateParameterPayload and ApplyParameterMutation both return on RemoveParameter
    //         before touching ::Type, and removal matches by name alone.
    //     Guarding those reads with an `if` would NOT clear them: this scan is flow-insensitive by
    //     design, so only moving a read into a function the sibling alone calls removes a pair --
    //     which is what the renderer `index` split above did, and what these four would need.
    //   * drive.* (31), game_framework.* (14), chooser.* (19), eqs.*/ai.* (43). Shared entry-point
    //     helpers whose callers declare different subsets. chooser is self-indicting: LoadChooser's
    //     own error text says "chooserPath, assetPath, tablePath, or path is required" and three of
    //     the four spellings it names are refused at the wire. game_framework's `path` is off this
    //     list and must not come back: FCommonParams::Extract read it for all eight callers while
    //     CreateGameFrameworkBlueprint -- inside GF_CREATE_CLASS_HANDLER, the only family that
    //     declares it -- was its sole consumer, so it moved to FCommonParams::ExtractSavePath,
    //     which only that macro calls.
    //   * behavior_tree.attach_decorator / attach_service (8). FOUR OF THESE EIGHT ARE THE
    //     FLOW-INSENSITIVITY ARTEFACT named in KNOWN LIMITS: HandleAttachBTSubNode branches on the
    //     bDecorator literal its caller passes, so attach_decorator is credited with the service
    //     spellings and attach_service with the decorator ones. They are recorded rather than
    //     dropped because dropping them needs per-call-site branch evaluation this scanner does
    //     not do -- and because silently dropping them is how two earlier offline sweeps ended up
    //     unfalsifiable. blueprint.graph.list_graphs/list_node_types:graphName is the same class
    //     one rung down: ResolveBlueprintAndGraph returns before the read when bGraphRequired is
    //     false, which is exactly what those two callers pass.
    //   * render.capture_* (8), audio.* (8), blueprint.* (4), state_tree.* (2), camera.* (2),
    //     geometry.* (2). Individually decidable; three of the render pairs already carry source
    //     comments admitting the wire refuses them.
    //
    // A host with an integration's engine plugin disabled registers fewer methods and will report
    // the chooser and geometry entries as stale rather than failing -- that is the documented
    // behaviour of the stale check, not a regression.
    const TArray<FString>& KnownUndeclaredHelperReads()
    {
        static const TArray<FString> Entries = {
            TEXT("ai.add_eqs_context:contextClass"),
            TEXT("ai.add_eqs_context:generatorIndex"),
            TEXT("ai.add_eqs_context:propertyName"),
            TEXT("ai.add_eqs_context:save"),
            TEXT("ai.add_eqs_context:testIndex"),
            TEXT("ai.add_eqs_generator:save"),
            TEXT("ai.add_eqs_test:generatorIndex"),
            TEXT("ai.add_eqs_test:purpose"),
            TEXT("ai.add_eqs_test:save"),
            TEXT("ai.configure_test_scoring:clampMax"),
            TEXT("ai.configure_test_scoring:clampMaxType"),
            TEXT("ai.configure_test_scoring:clampMin"),
            TEXT("ai.configure_test_scoring:clampMinType"),
            TEXT("ai.configure_test_scoring:curve"),
            TEXT("ai.configure_test_scoring:equation"),
            TEXT("ai.configure_test_scoring:factor"),
            TEXT("ai.configure_test_scoring:generatorIndex"),
            TEXT("ai.configure_test_scoring:referenceValue"),
            TEXT("ai.configure_test_scoring:save"),
            TEXT("ai.configure_test_scoring:scoreClampMax"),
            TEXT("ai.configure_test_scoring:scoreClampMin"),
            TEXT("ai.configure_test_scoring:scoring"),
            TEXT("ai.configure_test_scoring:scoringEquation"),
            TEXT("ai.configure_test_scoring:scoringFactor"),
            TEXT("ai.create_eqs_query:save"),

            TEXT("audio.authoring.add_metasound_variable:objectPath"),
            TEXT("audio.authoring.set_metasound_default:objectPath"),
            TEXT("audio.authoring.set_metasound_node_input_default:objectPath"),
            TEXT("audio.authoring.set_metasound_variable_default:objectPath"),
            TEXT("audio.synth.discard:candidateId"),
            TEXT("audio.synth.discard:candidateIds"),
            TEXT("audio.synth.discard:candidate_id"),
            TEXT("audio.synth.discard:candidate_ids"),

            TEXT("behavior_tree.attach_decorator:class"),
            TEXT("behavior_tree.attach_decorator:decoratorType"),
            TEXT("behavior_tree.attach_decorator:serviceClass"),
            TEXT("behavior_tree.attach_decorator:serviceType"),
            TEXT("behavior_tree.attach_service:class"),
            TEXT("behavior_tree.attach_service:decoratorClass"),
            TEXT("behavior_tree.attach_service:decoratorType"),
            TEXT("behavior_tree.attach_service:serviceType"),

            TEXT("blueprint.add_struct_field:name"),
            TEXT("blueprint.add_struct_field:type"),
            TEXT("blueprint.graph.list_graphs:graphName"),
            TEXT("blueprint.graph.list_node_types:graphName"),

            TEXT("camera.animation_shots:point"),
            TEXT("camera.frame_actor:point"),

            TEXT("chooser.add_column:assetPath"),
            TEXT("chooser.add_column:chooserPath"),
            TEXT("chooser.add_column:tablePath"),
            TEXT("chooser.add_row:assetPath"),
            TEXT("chooser.add_row:chooserPath"),
            TEXT("chooser.add_row:tablePath"),
            TEXT("chooser.compile:assetPath"),
            TEXT("chooser.compile:chooserPath"),
            TEXT("chooser.compile:tablePath"),
            TEXT("chooser.create:folder"),
            TEXT("chooser.create:savePath"),
            TEXT("chooser.set_cell:assetPath"),
            TEXT("chooser.set_cell:cellValue"),
            TEXT("chooser.set_cell:chooserPath"),
            TEXT("chooser.set_cell:columnIndex"),
            TEXT("chooser.set_cell:tablePath"),
            TEXT("chooser.set_result:assetPath"),
            TEXT("chooser.set_result:chooserPath"),
            TEXT("chooser.set_result:tablePath"),

            TEXT("drive.click:browser_index"),
            TEXT("drive.click:instanceName"),
            TEXT("drive.click:rootIndex"),
            TEXT("drive.click:wait_for_timeout_ms"),
            TEXT("drive.drag:browser_index"),
            TEXT("drive.drag:instanceName"),
            TEXT("drive.drag:rootIndex"),
            TEXT("drive.drag:wait_for_timeout_ms"),
            TEXT("drive.expect:browser_index"),
            TEXT("drive.hover:browser_index"),
            TEXT("drive.hover:instanceName"),
            TEXT("drive.hover:rootIndex"),
            TEXT("drive.hover:wait_for_timeout_ms"),
            TEXT("drive.key:browser_index"),
            TEXT("drive.key:instanceName"),
            TEXT("drive.key:rootIndex"),
            TEXT("drive.key:wait_for_timeout_ms"),
            TEXT("drive.observe:browser_index"),
            TEXT("drive.scroll:browser_index"),
            TEXT("drive.scroll:instanceName"),
            TEXT("drive.scroll:rootIndex"),
            TEXT("drive.scroll:wait_for_timeout_ms"),
            TEXT("drive.type:browser_index"),
            TEXT("drive.type:instanceName"),
            TEXT("drive.type:rootIndex"),
            TEXT("drive.type:wait_for_timeout_ms"),
            TEXT("drive.wait_for:browser_index"),
            TEXT("drive.wait_for:instanceName"),
            TEXT("drive.wait_for:rootIndex"),
            TEXT("drive.wait_for:wait_for"),
            TEXT("drive.wait_for:wait_for_timeout_ms"),

            TEXT("eqs.set_context_class:contextType"),
            TEXT("eqs.set_test_filter:filterType"),
            TEXT("eqs.set_test_filter:kind"),
            TEXT("eqs.set_test_filter:max"),
            TEXT("eqs.set_test_filter:min"),
            TEXT("eqs.set_test_filter:value"),
            TEXT("eqs.set_test_scoring:clampMax"),
            TEXT("eqs.set_test_scoring:clampMaxType"),
            TEXT("eqs.set_test_scoring:clampMin"),
            TEXT("eqs.set_test_scoring:clampMinType"),
            TEXT("eqs.set_test_scoring:curve"),
            TEXT("eqs.set_test_scoring:equation"),
            TEXT("eqs.set_test_scoring:factor"),
            TEXT("eqs.set_test_scoring:referenceValue"),
            TEXT("eqs.set_test_scoring:scoreClampMax"),
            TEXT("eqs.set_test_scoring:scoreClampMin"),
            TEXT("eqs.set_test_scoring:scoringEquation"),
            TEXT("eqs.set_test_scoring:scoringFactor"),

            TEXT("game_framework.configure_game_rules:blueprintPath"),
            TEXT("game_framework.configure_game_rules:name"),
            TEXT("game_framework.configure_round_system:blueprintPath"),
            TEXT("game_framework.configure_round_system:name"),
            TEXT("game_framework.configure_scoring_system:blueprintPath"),
            TEXT("game_framework.configure_scoring_system:name"),
            TEXT("game_framework.configure_spawn_system:blueprintPath"),
            TEXT("game_framework.configure_spawn_system:name"),
            TEXT("game_framework.configure_spectating:blueprintPath"),
            TEXT("game_framework.configure_spectating:name"),
            TEXT("game_framework.configure_team_system:blueprintPath"),
            TEXT("game_framework.configure_team_system:name"),
            TEXT("game_framework.set_respawn_rules:blueprintPath"),
            TEXT("game_framework.set_respawn_rules:name"),

            TEXT("geometry.bind_skin_weights:skeletonFromMesh"),
            TEXT("geometry.convert_to_skeletal_mesh:skeletonFromMesh"),

            TEXT("niagara.add_data_interface:classPath"),
            TEXT("niagara.add_data_interface:dataInterfaceClassPath"),
            TEXT("niagara.add_data_interface:emitterName"),
            TEXT("niagara.add_event_handler:emitterName"),
            TEXT("niagara.add_event_handler:entryId"),
            TEXT("niagara.add_event_handler:eventHandlerId"),
            TEXT("niagara.add_event_handler:eventHandlerIndex"),
            TEXT("niagara.add_event_handler:index"),
            TEXT("niagara.add_event_handler:randomSpawnNumber"),
            TEXT("niagara.add_module:emitterName"),
            TEXT("niagara.add_module:enabled"),
            TEXT("niagara.add_module:entryId"),
            TEXT("niagara.add_module:inputName"),
            TEXT("niagara.add_module:moduleId"),
            TEXT("niagara.add_module:node"),
            TEXT("niagara.add_module:nodeId"),
            TEXT("niagara.add_module:pin"),
            TEXT("niagara.add_module:pinName"),
            TEXT("niagara.add_module:preserveOverrides"),
            TEXT("niagara.add_module:rendererIndex"),
            TEXT("niagara.add_module:scriptPath"),
            TEXT("niagara.add_module:scriptType"),
            TEXT("niagara.add_module:scriptVersion"),
            TEXT("niagara.add_module:targetKind"),
            TEXT("niagara.add_module:toIndex"),
            TEXT("niagara.add_parameter:emitterName"),
            TEXT("niagara.add_parameter:parameterName"),
            TEXT("niagara.add_parameter:parameterType"),
            TEXT("niagara.add_renderer:classPath"),
            TEXT("niagara.add_renderer:emitterName"),
            TEXT("niagara.add_renderer:entryId"),
            TEXT("niagara.add_renderer:moduleId"),
            TEXT("niagara.add_renderer:node"),
            TEXT("niagara.add_renderer:nodeId"),
            TEXT("niagara.add_renderer:pin"),
            TEXT("niagara.add_renderer:pinName"),
            TEXT("niagara.add_renderer:rendererIndex"),
            TEXT("niagara.add_renderer:scriptType"),
            TEXT("niagara.add_renderer:scriptUsage"),
            TEXT("niagara.add_renderer:targetKind"),
            TEXT("niagara.add_renderer:toIndex"),
            TEXT("niagara.add_simulation_stage:classPath"),
            TEXT("niagara.add_simulation_stage:emitterName"),
            TEXT("niagara.add_simulation_stage:entryId"),
            TEXT("niagara.add_simulation_stage:index"),
            TEXT("niagara.add_simulation_stage:stageClassPath"),
            TEXT("niagara.add_simulation_stage:stageId"),
            TEXT("niagara.add_simulation_stage:stageIndex"),
            // niagara.bind_curve_asset's four pairs (classPath / dataInterfaceClass /
            // dataInterfaceClassPath / emitterName) were pruned when the verb stopped routing
            // through ParseDataInterfacePayload: it now parses its own two addressing forms and
            // reads only declared keys. Same for niagara.set_curve_keys below.
            TEXT("niagara.clear_module_overrides:emitterName"),
            TEXT("niagara.clear_module_overrides:enabled"),
            TEXT("niagara.clear_module_overrides:inputName"),
            TEXT("niagara.clear_module_overrides:moduleId"),
            TEXT("niagara.clear_module_overrides:modulePath"),
            TEXT("niagara.clear_module_overrides:node"),
            TEXT("niagara.clear_module_overrides:nodeId"),
            TEXT("niagara.clear_module_overrides:pin"),
            TEXT("niagara.clear_module_overrides:pinName"),
            TEXT("niagara.clear_module_overrides:preserveOverrides"),
            TEXT("niagara.clear_module_overrides:rendererIndex"),
            TEXT("niagara.clear_module_overrides:scriptPath"),
            TEXT("niagara.clear_module_overrides:scriptType"),
            TEXT("niagara.clear_module_overrides:scriptVersion"),
            TEXT("niagara.clear_module_overrides:targetKind"),
            TEXT("niagara.clear_module_overrides:toIndex"),
            TEXT("niagara.connect_pin:emitterName"),
            TEXT("niagara.connect_pin:entryId"),
            TEXT("niagara.connect_pin:moduleId"),
            TEXT("niagara.connect_pin:node"),
            TEXT("niagara.connect_pin:nodeId"),
            TEXT("niagara.connect_pin:pin"),
            TEXT("niagara.connect_pin:pinName"),
            TEXT("niagara.connect_pin:rendererIndex"),
            TEXT("niagara.connect_pin:scriptType"),
            TEXT("niagara.connect_pin:targetKind"),
            TEXT("niagara.connect_pin:toIndex"),
            TEXT("niagara.disconnect_pin:emitterName"),
            TEXT("niagara.disconnect_pin:entryId"),
            TEXT("niagara.disconnect_pin:moduleId"),
            TEXT("niagara.disconnect_pin:node"),
            TEXT("niagara.disconnect_pin:nodeId"),
            TEXT("niagara.disconnect_pin:pin"),
            TEXT("niagara.disconnect_pin:pinName"),
            TEXT("niagara.disconnect_pin:rendererIndex"),
            TEXT("niagara.disconnect_pin:scriptType"),
            TEXT("niagara.disconnect_pin:targetKind"),
            TEXT("niagara.disconnect_pin:toIndex"),
            TEXT("niagara.move_module:emitterName"),
            TEXT("niagara.move_module:enabled"),
            TEXT("niagara.move_module:inputName"),
            TEXT("niagara.move_module:moduleId"),
            TEXT("niagara.move_module:modulePath"),
            TEXT("niagara.move_module:node"),
            TEXT("niagara.move_module:nodeId"),
            TEXT("niagara.move_module:pin"),
            TEXT("niagara.move_module:pinName"),
            TEXT("niagara.move_module:preserveOverrides"),
            TEXT("niagara.move_module:rendererIndex"),
            TEXT("niagara.move_module:scriptPath"),
            TEXT("niagara.move_module:scriptType"),
            TEXT("niagara.move_module:scriptVersion"),
            TEXT("niagara.move_module:targetKind"),
            TEXT("niagara.move_renderer:classPath"),
            TEXT("niagara.move_renderer:emitterName"),
            TEXT("niagara.move_renderer:entryId"),
            TEXT("niagara.move_renderer:moduleId"),
            TEXT("niagara.move_renderer:node"),
            TEXT("niagara.move_renderer:nodeId"),
            TEXT("niagara.move_renderer:pin"),
            TEXT("niagara.move_renderer:pinName"),
            TEXT("niagara.move_renderer:rendererClassPath"),
            TEXT("niagara.move_renderer:rendererIndex"),
            TEXT("niagara.move_renderer:scriptType"),
            TEXT("niagara.move_renderer:scriptUsage"),
            TEXT("niagara.move_renderer:targetKind"),
            TEXT("niagara.remove_data_interface:classPath"),
            TEXT("niagara.remove_data_interface:dataInterfaceClass"),
            TEXT("niagara.remove_data_interface:dataInterfaceClassPath"),
            TEXT("niagara.remove_data_interface:emitterName"),
            TEXT("niagara.remove_event_handler:bRandomSpawnNumber"),
            TEXT("niagara.remove_event_handler:emitterName"),
            TEXT("niagara.remove_event_handler:entryId"),
            TEXT("niagara.remove_event_handler:executionMode"),
            TEXT("niagara.remove_event_handler:maxEventsPerFrame"),
            TEXT("niagara.remove_event_handler:minSpawnNumber"),
            TEXT("niagara.remove_event_handler:randomSpawnNumber"),
            TEXT("niagara.remove_event_handler:source"),
            TEXT("niagara.remove_event_handler:sourceEventName"),
            TEXT("niagara.remove_event_handler:spawnNumber"),
            TEXT("niagara.remove_module:emitterName"),
            TEXT("niagara.remove_module:enabled"),
            TEXT("niagara.remove_module:inputName"),
            TEXT("niagara.remove_module:moduleId"),
            TEXT("niagara.remove_module:modulePath"),
            TEXT("niagara.remove_module:node"),
            TEXT("niagara.remove_module:nodeId"),
            TEXT("niagara.remove_module:pin"),
            TEXT("niagara.remove_module:pinName"),
            TEXT("niagara.remove_module:preserveOverrides"),
            TEXT("niagara.remove_module:rendererIndex"),
            TEXT("niagara.remove_module:scriptPath"),
            TEXT("niagara.remove_module:scriptType"),
            TEXT("niagara.remove_module:scriptVersion"),
            TEXT("niagara.remove_module:targetKind"),
            TEXT("niagara.remove_module:toIndex"),
            TEXT("niagara.remove_parameter:emitterName"),
            TEXT("niagara.remove_parameter:parameterName"),
            TEXT("niagara.remove_parameter:parameterType"),
            TEXT("niagara.remove_parameter:type"),
            TEXT("niagara.remove_renderer:classPath"),
            TEXT("niagara.remove_renderer:emitterName"),
            TEXT("niagara.remove_renderer:entryId"),
            TEXT("niagara.remove_renderer:moduleId"),
            TEXT("niagara.remove_renderer:node"),
            TEXT("niagara.remove_renderer:nodeId"),
            TEXT("niagara.remove_renderer:pin"),
            TEXT("niagara.remove_renderer:pinName"),
            TEXT("niagara.remove_renderer:rendererClassPath"),
            TEXT("niagara.remove_renderer:rendererIndex"),
            TEXT("niagara.remove_renderer:scriptType"),
            TEXT("niagara.remove_renderer:scriptUsage"),
            TEXT("niagara.remove_renderer:targetKind"),
            TEXT("niagara.remove_renderer:toIndex"),
            TEXT("niagara.remove_simulation_stage:atIndex"),
            TEXT("niagara.remove_simulation_stage:classPath"),
            TEXT("niagara.remove_simulation_stage:emitterName"),
            TEXT("niagara.remove_simulation_stage:entryId"),
            TEXT("niagara.remove_simulation_stage:stageClass"),
            TEXT("niagara.remove_simulation_stage:stageClassPath"),
            TEXT("niagara.reset_module_input:emitterName"),
            TEXT("niagara.reset_module_input:enabled"),
            TEXT("niagara.reset_module_input:moduleId"),
            TEXT("niagara.reset_module_input:modulePath"),
            TEXT("niagara.reset_module_input:node"),
            TEXT("niagara.reset_module_input:nodeId"),
            TEXT("niagara.reset_module_input:pin"),
            TEXT("niagara.reset_module_input:pinName"),
            TEXT("niagara.reset_module_input:preserveOverrides"),
            TEXT("niagara.reset_module_input:rendererIndex"),
            TEXT("niagara.reset_module_input:scriptPath"),
            TEXT("niagara.reset_module_input:scriptType"),
            TEXT("niagara.reset_module_input:scriptVersion"),
            TEXT("niagara.reset_module_input:targetKind"),
            TEXT("niagara.reset_module_input:toIndex"),
            // niagara.set_curve_keys' four pairs pruned for the same reason as
            // niagara.bind_curve_asset's above.
            TEXT("niagara.set_module_input:emitterName"),
            TEXT("niagara.set_module_input:enabled"),
            TEXT("niagara.set_module_input:moduleId"),
            TEXT("niagara.set_module_input:modulePath"),
            TEXT("niagara.set_module_input:node"),
            TEXT("niagara.set_module_input:nodeId"),
            TEXT("niagara.set_module_input:pin"),
            TEXT("niagara.set_module_input:pinName"),
            TEXT("niagara.set_module_input:preserveOverrides"),
            TEXT("niagara.set_module_input:rendererIndex"),
            TEXT("niagara.set_module_input:scriptPath"),
            TEXT("niagara.set_module_input:scriptType"),
            TEXT("niagara.set_module_input:scriptVersion"),
            TEXT("niagara.set_module_input:targetKind"),
            TEXT("niagara.set_module_input:toIndex"),
            TEXT("niagara.set_module_script:emitterName"),
            TEXT("niagara.set_module_script:enabled"),
            TEXT("niagara.set_module_script:inputName"),
            TEXT("niagara.set_module_script:moduleId"),
            TEXT("niagara.set_module_script:modulePath"),
            TEXT("niagara.set_module_script:node"),
            TEXT("niagara.set_module_script:nodeId"),
            TEXT("niagara.set_module_script:pin"),
            TEXT("niagara.set_module_script:pinName"),
            TEXT("niagara.set_module_script:rendererIndex"),
            TEXT("niagara.set_module_script:scriptType"),
            TEXT("niagara.set_module_script:targetKind"),
            TEXT("niagara.set_module_script:toIndex"),
            TEXT("niagara.set_parameter:emitterName"),
            TEXT("niagara.set_parameter:parameterName"),
            TEXT("niagara.set_parameter:parameterType"),
            TEXT("niagara.set_pin_default:emitterName"),
            TEXT("niagara.set_pin_default:entryId"),
            TEXT("niagara.set_pin_default:fromNode"),
            TEXT("niagara.set_pin_default:fromPin"),
            TEXT("niagara.set_pin_default:moduleId"),
            TEXT("niagara.set_pin_default:node"),
            TEXT("niagara.set_pin_default:pinName"),
            TEXT("niagara.set_pin_default:rendererIndex"),
            TEXT("niagara.set_pin_default:scriptType"),
            TEXT("niagara.set_pin_default:targetKind"),
            TEXT("niagara.set_pin_default:toIndex"),
            TEXT("niagara.set_pin_default:toNode"),
            TEXT("niagara.set_pin_default:toPin"),
            TEXT("niagara.set_property:emitter"),
            TEXT("niagara.set_property:emitterName"),
            TEXT("niagara.set_property:entryId"),
            TEXT("niagara.set_property:moduleId"),
            TEXT("niagara.set_property:node"),
            TEXT("niagara.set_property:nodeId"),
            TEXT("niagara.set_property:pin"),
            TEXT("niagara.set_property:pinName"),
            TEXT("niagara.set_property:rendererIndex"),
            TEXT("niagara.set_property:scriptType"),
            TEXT("niagara.set_property:scriptUsage"),
            TEXT("niagara.set_property:targetKind"),
            TEXT("niagara.set_property:toIndex"),
            TEXT("niagara.set_scalability_property:emitter"),
            TEXT("niagara.set_scalability_property:emitterName"),
            TEXT("niagara.set_scalability_property:entryId"),
            TEXT("niagara.set_scalability_property:moduleId"),
            TEXT("niagara.set_scalability_property:node"),
            TEXT("niagara.set_scalability_property:nodeId"),
            TEXT("niagara.set_scalability_property:pin"),
            TEXT("niagara.set_scalability_property:pinName"),
            TEXT("niagara.set_scalability_property:rendererIndex"),
            TEXT("niagara.set_scalability_property:scriptType"),
            TEXT("niagara.set_scalability_property:scriptUsage"),
            TEXT("niagara.set_scalability_property:targetKind"),
            TEXT("niagara.set_scalability_property:toIndex"),
            TEXT("niagara.set_stack_enabled:emitterName"),
            TEXT("niagara.set_stack_enabled:inputName"),
            TEXT("niagara.set_stack_enabled:moduleId"),
            TEXT("niagara.set_stack_enabled:modulePath"),
            TEXT("niagara.set_stack_enabled:node"),
            TEXT("niagara.set_stack_enabled:nodeId"),
            TEXT("niagara.set_stack_enabled:pin"),
            TEXT("niagara.set_stack_enabled:pinName"),
            TEXT("niagara.set_stack_enabled:preserveOverrides"),
            TEXT("niagara.set_stack_enabled:rendererIndex"),
            TEXT("niagara.set_stack_enabled:scriptPath"),
            TEXT("niagara.set_stack_enabled:scriptType"),
            TEXT("niagara.set_stack_enabled:scriptVersion"),
            TEXT("niagara.set_stack_enabled:targetKind"),
            TEXT("niagara.set_stack_enabled:toIndex"),
            TEXT("niagara.set_static_switch:emitterName"),
            TEXT("niagara.set_static_switch:enabled"),
            TEXT("niagara.set_static_switch:moduleId"),
            TEXT("niagara.set_static_switch:modulePath"),
            TEXT("niagara.set_static_switch:node"),
            TEXT("niagara.set_static_switch:nodeId"),
            TEXT("niagara.set_static_switch:pin"),
            TEXT("niagara.set_static_switch:pinName"),
            TEXT("niagara.set_static_switch:preserveOverrides"),
            TEXT("niagara.set_static_switch:rendererIndex"),
            TEXT("niagara.set_static_switch:scriptPath"),
            TEXT("niagara.set_static_switch:scriptType"),
            TEXT("niagara.set_static_switch:scriptVersion"),
            TEXT("niagara.set_static_switch:targetKind"),
            TEXT("niagara.set_static_switch:toIndex"),

            TEXT("render.capture_animation_preview:point"),
            TEXT("render.capture_annotated:allowBlank"),
            TEXT("render.capture_annotated:point"),
            TEXT("render.capture_asset_preview:hideEditorSprites"),
            TEXT("render.capture_asset_preview:point"),
            TEXT("render.capture_asset_preview:viewDistanceScale"),

            TEXT("state_tree.add_condition:fromState"),
            TEXT("state_tree.set_transition_trigger:stateName"),
        };
        return Entries;
    }

    // ------------------------------------------------------------------------
    // The nested-schema baselines
    // ------------------------------------------------------------------------

    // "<method>:<parameter>.<nested key>" -- a nested key the parameter's own DESCRIPTION promises
    // inside a {...} shape, whose literal occurs nowhere in the plugin's Source at all, so nothing
    // reads it: not the body, not a helper, not a key-list factory. Structurally free of every
    // false-positive class the shape is usually accused of, which is why the strict "absent
    // everywhere" formulation is the one implemented.
    const TArray<FString>& KnownUnreadPromisedNestedKeys()
    {
        static const TArray<FString> Entries = {
            // NOT A DEFECT, recorded so the number stays honest: ParamName in
            // "Map of {ParamName: number}" is a METAVARIABLE, not a wire key -- these maps are
            // keyed by caller-chosen material parameter names. The extractor cannot tell a
            // metavariable from a promise, so these three are exempted here by name rather than by
            // weakening the extractor into missing real promises.
            TEXT("material.authoring.set_material_instance_parameters:scalar.ParamName"),
            TEXT("material.authoring.set_material_instance_parameters:staticSwitch.ParamName"),
            TEXT("material.authoring.set_material_instance_parameters:texture.ParamName"),
        };
        return Entries;
    }

    // "<method>:<parameter>.<nested key>" -- the other direction: a nested key the handler body
    // READS out of an object/array parameter whose description never names it, in a {...} promise
    // or even in prose. The caller has no documented way to learn the spelling, and nothing at
    // runtime rejects a wrong one, so a typo is a silent no-op.
    //
    // TO FIX ONE: name the key in the owning parameter's description (a {a, b, c} list is what the
    // extractor reads, and what a caller reads too), then delete the line here. Closing the object
    // at runtime as well is the other half, and it is now a declaration rather than a per-verb
    // helper: populate FParamSpec::NestedKeys on that parameter (RPC_PARAM_OPT_NESTED) and the
    // dispatcher refuses everything else with UNKNOWN_NESTED_PARAMS. Do that only when the accepted
    // set is genuinely closed and the description says so -- it is a compatibility break, and the
    // adopters are enumerated in Tests/Infra/TestNestedParamKeyGate.cpp.
    const TArray<FString>& KnownUnpromisedNestedReads()
    {
        static const TArray<FString> Entries = {
            TEXT("audio.authoring.configure_mix_eq:eqSettings.bandwidth1"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.bandwidth2"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.bandwidth3"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.frequencyCenter1"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.frequencyCenter2"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.frequencyCenter3"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.gain1"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.gain2"),
            TEXT("audio.authoring.configure_mix_eq:eqSettings.gain3"),

            TEXT("blueprint.build_api_index:classFilter.className"),
            TEXT("blueprint.build_api_index:classFilter.functions"),

            TEXT("effect.set_niagara_parameter:value.x"),
            TEXT("effect.set_niagara_parameter:value.y"),
            TEXT("effect.set_niagara_parameter:value.z"),

            TEXT("landscape.sculpt:position.x"),
            TEXT("landscape.sculpt:position.y"),
            TEXT("landscape.sculpt:position.z"),

            TEXT("lighting.spawn_light:properties.innerConeAngle"),
            TEXT("lighting.spawn_light:properties.outerConeAngle"),
            TEXT("lighting.spawn_light:properties.sourceHeight"),
            TEXT("lighting.spawn_light:properties.sourceWidth"),
            TEXT("lighting.spawn_light:properties.useAsAtmosphereSunLight"),

            TEXT("niagara.modify_parameter:value.a"),
            TEXT("niagara.modify_parameter:value.b"),
            TEXT("niagara.modify_parameter:value.g"),
            TEXT("niagara.modify_parameter:value.r"),
            TEXT("niagara.modify_parameter:value.x"),
            TEXT("niagara.modify_parameter:value.y"),
            TEXT("niagara.modify_parameter:value.z"),

            TEXT("volume.create_post_process_volume:postProcessSettings.bloomEnabled"),
            TEXT("volume.create_post_process_volume:postProcessSettings.contrast"),
            TEXT("volume.create_post_process_volume:postProcessSettings.exposureBias"),
            TEXT("volume.create_post_process_volume:postProcessSettings.gamma"),
            TEXT("volume.create_post_process_volume:postProcessSettings.saturation"),
            TEXT("volume.create_post_process_volume:postProcessSettings.vignetteIntensity"),
        };
        return Entries;
    }

    // ------------------------------------------------------------------------
    // Shared tree scan
    // ------------------------------------------------------------------------

    // Reads the plugin's shipped sources once and returns every registration it recovers, with the
    // helper index already closed. Returns false when there is no readable Source tree (a
    // binary-only install), in which case nothing is measurable and a pass would be a lie.
    bool ScanPluginTree(TArray<FScannedVerb>& OutVerbs, FString& OutSourceDir,
                        TSet<FString>* OutSourceLiterals = nullptr)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        OutSourceDir = Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
        if (OutSourceDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*OutSourceDir))
        {
            return false;
        }

        const FString FullSourceDir = FPaths::ConvertRelativePathToFull(OutSourceDir) / TEXT("");
        auto Relativize = [&FullSourceDir](const FString& File)
        {
            FString Relative = FPaths::ConvertRelativePathToFull(File);
            FPaths::MakePathRelativeTo(Relative, *FullSourceDir);
            return Relative.Replace(TEXT("\\"), TEXT("/"));
        };

        // The helper index spans headers too -- BuildMetaSoundLiteralFromParams is defined in
        // Audio/MetaSound/MetaSoundLiteralParams.h, and four audio.authoring.* verbs read
        // objectPath through it.
        TArray<FString> HeaderFiles;
        TArray<FString> SourceFiles;
        IFileManager::Get().FindFilesRecursive(HeaderFiles, *OutSourceDir, TEXT("*.h"), true, false, false);
        IFileManager::Get().FindFilesRecursive(SourceFiles, *OutSourceDir, TEXT("*.cpp"), true, false, false);

        FHelperIndex Helpers;
        const TArray<FString>* const FileGroups[] = { &HeaderFiles, &SourceFiles };
        for (const TArray<FString>* const Group : FileGroups)
        {
            for (const FString& File : *Group)
            {
                const FString Relative = Relativize(File);
                // Test sources register fixture verbs and fixture helpers of their own; they are
                // not shipped surface.
                if (Relative.Contains(TEXT("/Tests/")))
                {
                    continue;
                }
                FString Text;
                if (!FFileHelper::LoadFileToString(Text, *File))
                {
                    continue;
                }
                const FString Neutralized = NeutralizeSourceText(Text);
                if (OutSourceLiterals)
                {
                    CollectQuotedLiterals(Neutralized, *OutSourceLiterals);
                }
                Helpers.AddFile(Relative, Neutralized);
            }
        }
        Helpers.Close();

        for (const FString& File : SourceFiles)
        {
            const FString Relative = Relativize(File);
            if (Relative.Contains(TEXT("/Tests/")))
            {
                continue;
            }
            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *File))
            {
                continue;
            }
            if (!Text.Contains(TEXT("REGISTER_RPC_HANDLER"), ESearchCase::CaseSensitive))
            {
                continue;
            }
            ScanFile(Relative, Text, OutVerbs, &Helpers);
        }
        return true;
    }

    bool CollectProductionLiteralsOutsideParamDeclarations(
        TSet<FString>& OutLiterals, FString& OutSourceDir, int32& OutFileCount)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        OutSourceDir = Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
        OutFileCount = 0;
        OutLiterals.Reset();
        if (OutSourceDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*OutSourceDir))
        {
            return false;
        }

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *OutSourceDir, TEXT("*.h"), true, false, false);
        IFileManager::Get().FindFilesRecursive(Files, *OutSourceDir, TEXT("*.cpp"), true, false, false);
        for (const FString& File : Files)
        {
            FString Relative = File;
            const FString FullSourceDir = FPaths::ConvertRelativePathToFull(OutSourceDir);
            FPaths::MakePathRelativeTo(Relative, *FullSourceDir);
            Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
            if (Relative.Contains(TEXT("/Tests/")))
            {
                continue;
            }

            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *File))
            {
                continue;
            }
            Text = NeutralizeSourceText(Text);
            BlankRpcParamInvocations(Text);
            CollectQuotedLiterals(Text, OutLiterals);
            ++OutFileCount;
        }
        return true;
    }

    // Collect accepted parameter spellings that have no source literal outside the declaration
    // span. Keep the historical canonical form (`method:name`) stable; aliases include their
    // owning canonical name so a finding is unambiguous when a route accepts the same spelling on
    // more than one slot (`method:canonical/alias`).
    void CollectMissingAcceptedParamSourceUses(const FString& Method, const FParamSpec& Spec,
                                               const TSet<FString>& SourceLiterals,
                                               TSet<FString>& OutMissing)
    {
        auto Check = [&Method, &Spec, &SourceLiterals, &OutMissing](const FString& Name,
                                                                      bool bCanonical)
        {
            if (SourceLiterals.Contains(Name))
            {
                return;
            }
            OutMissing.Add(bCanonical
                ? FString::Printf(TEXT("%s:%s"), *Method, *Name)
                : FString::Printf(TEXT("%s:%s/%s"), *Method, *Spec.Name, *Name));
        };

        Check(Spec.Name, true);
        for (const FString& Alias : Spec.Aliases)
        {
            Check(Alias, false);
        }
        for (const FParamAliasSpec& Alias : Spec.TypedAliases)
        {
            Check(Alias.Name, false);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeclaredParamCoverageTest,
    "PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeclaredParamCoverageTest::RunTest(const FString& Parameters)
{
    using namespace DeclaredParamCoverageTestLocal;

    TArray<FScannedVerb> Verbs;
    FString SourceDir;
    if (!ScanPluginTree(Verbs, SourceDir))
    {
        // A binary-only install ships no Source tree. Nothing is measurable, and reporting a pass
        // would be indistinguishable from a clean scan.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("plugin-source-tree-absent"),
            FString::Printf(TEXT("no readable Source directory under the PinWright plugin (resolved '%s')"),
                *SourceDir));
        return true;
    }

    // Vacuity guard: a scanner that silently stopped matching would report zero violations and
    // read as a clean pass. The floor is derived from the live registry rather than hard-coded so
    // it tracks the tree, and it is deliberately loose (half) because a host with integration
    // sub-modules disabled registers fewer methods than the source tree declares, never more.
    const int32 RegisteredCount = FAutoRegisterHandler::GetPendingRegistrations().Num();
    if (!TestTrue(
            *FString::Printf(TEXT("the source scan recovered handler bodies (found %d registrations in source, %d live)"),
                Verbs.Num(), RegisteredCount),
            Verbs.Num() >= RegisteredCount / 2))
    {
        return false;
    }

    TSet<FString> RegisteredMethods;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        RegisteredMethods.Add(Reg.MethodName);
    }

    TSet<FString> Found;
    TMap<FString, FString> PairToFile;
    TMap<FString, FString> PairToHelper;
    for (const FScannedVerb& Verb : Verbs)
    {
        // Not registered here means an integration sub-module whose engine plugin is disabled on
        // this host: its source is present and its declarations are not, so there is nothing to
        // compare against and a comparison would report every read as undeclared.
        if (!RegisteredMethods.Contains(Verb.Method))
        {
            continue;
        }

        const TSet<FString> Accepted =
            ParamSpecTestHelpers::CollectAcceptedParamNames(Verb.Method);
        TSet<FString> AllKeys = Verb.ReadKeys;
        AllKeys.Append(Verb.HelperReadKeys);
        for (const FString& Key : AllKeys)
        {
            if (Accepted.Contains(Key))
            {
                continue;
            }
            const FString Pair = Verb.Method + TEXT(":") + Key;
            Found.Add(Pair);
            PairToFile.Add(Pair, Verb.File);
            // A key the body reads directly is the body's own; only a key that arrives ONLY
            // through the hop is attributed to the helper, so the message names the read site the
            // reader has to open.
            if (!Verb.ReadKeys.Contains(Key))
            {
                if (const FString* const Origin = Verb.HelperOrigin.Find(Key))
                {
                    PairToHelper.Add(Pair, *Origin);
                }
            }
        }
    }

    TSet<FString> Baseline;
    for (const FString& Entry : KnownUndeclaredReads())
    {
        Baseline.Add(Entry);
    }
    for (const FString& Entry : KnownUndeclaredHelperReads())
    {
        Baseline.Add(Entry);
    }

    TArray<FString> Regressions = Found.Difference(Baseline).Array();
    Regressions.Sort();
    for (const FString& Pair : Regressions)
    {
        const FString* const Helper = PairToHelper.Find(Pair);
        AddError(FString::Printf(
            TEXT("%s -- the handler reads this key%s but its RPC_PARAMS does not declare it, so ")
            TEXT("FRpcDispatcher::ValidateHandlerParams refuses every real caller with ")
            TEXT("UNKNOWN_PARAMS before the body runs (%s). Declare it (as an alias on the slot it ")
            TEXT("feeds, or as its own parameter) or delete the read."),
            *Pair,
            Helper ? *FString::Printf(TEXT(" through %s"), **Helper) : TEXT(""),
            *PairToFile.FindRef(Pair)));
    }

    TArray<FString> Stale = Baseline.Difference(Found).Array();
    Stale.Sort();
    if (Stale.Num() > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d baseline entr%s in TestDeclaredParamCoverage.cpp no longer reproduce and ")
            TEXT("should be deleted (fixed, or the verb is not registered on this host): %s"),
            Stale.Num(), Stale.Num() == 1 ? TEXT("y") : TEXT("ies"),
            *FString::Join(Stale, TEXT(", "))));
    }

    return true;
}

// ============================================================================
// The scanner's own coverage.
//
// The scan above is a source matcher, so a shape it stops matching costs it nothing and reports
// nothing: the verb simply looks clean. That is how a one-shape scan reached an empty baseline
// while ~140 pairs stood (board B-declared-param-guard-blind-spots). This test pins each covered
// read shape against synthetic source, pins the two shapes that must NOT be collected, and fails
// when a key-taking accessor is added to FHandlerContext without being added to the scan.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeclaredParamScannerShapesTest,
    "PinWright.infra.declared_params.ScannerSeesEveryCoveredReadShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeclaredParamScannerShapesTest::RunTest(const FString& Parameters)
{
    using namespace DeclaredParamCoverageTestLocal;

    // One registration per read shape, each reduced to the smallest body that still exercises the
    // matcher. The keys are deliberately unlike any real parameter so a match cannot come from
    // somewhere else.
    const FString Source = TEXT(R"CPP(
REGISTER_RPC_HANDLER("probe.accessor", "probe", "s", RPC_NO_PARAMS)
{
    const FString A = Ctx.GetString(TEXT("probe_literal"));
    Ctx.GetJsonValueFirstOf({TEXT("probe_json_a"), TEXT("probe_json_b")});
    Ctx.GetString(RuntimeVariableKey);
}

REGISTER_RPC_HANDLER("probe.projection", "probe", "s", RPC_NO_PARAMS)
{
    const TSet<FString> Fields = Ctx.ReadFieldProjection({TEXT("probe_column")});
}

REGISTER_RPC_HANDLER("probe.raw", "probe", "s", RPC_NO_PARAMS)
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString Out;
    Payload->TryGetStringField(TEXT("probe_raw_member"), Out);
    const bool bFlag = GetJsonBoolField(Payload, TEXT("probe_raw_helper"), false);
    const bool bChained = Ctx.GetRawPayload()->HasField(TEXT("probe_raw_chained"));
    const TSharedPtr<FJsonObject>* Nested = nullptr;
    if (Payload->TryGetObjectField(TEXT("probe_raw_nested_owner"), Nested))
    {
        (*Nested)->TryGetStringField(TEXT("probe_raw_nested"), Out);
    }
}
)CPP");

    TArray<FScannedVerb> Verbs;
    ScanFile(TEXT("Synthetic/ScannerShapes.cpp"), Source, Verbs);

    TMap<FString, const FScannedVerb*> ByMethod;
    for (const FScannedVerb& Verb : Verbs)
    {
        ByMethod.Add(Verb.Method, &Verb);
    }

    auto Keys = [&ByMethod](const TCHAR* Method) -> TSet<FString>
    {
        const FScannedVerb* const* Found = ByMethod.Find(Method);
        return Found && *Found ? (*Found)->ReadKeys : TSet<FString>();
    };

    TestEqual(TEXT("the synthetic source yields one FScannedVerb per registration"),
        Verbs.Num(), 3);

    // Shape 1: the original literal-argument accessor read, kept as the control.
    const TSet<FString> AccessorKeys = Keys(TEXT("probe.accessor"));
    TestTrue(TEXT("a literal-key accessor read is collected"),
        AccessorKeys.Contains(TEXT("probe_literal")));

    // Shape 2: GetJsonValueFirstOf, the accessor the first version of the pattern omitted -- which
    // is what hid sequencer.set_playhead's five time/frame aliases.
    TestTrue(TEXT("GetJsonValueFirstOf's first candidate key is collected"),
        AccessorKeys.Contains(TEXT("probe_json_a")));
    TestTrue(TEXT("GetJsonValueFirstOf's later candidate keys are collected"),
        AccessorKeys.Contains(TEXT("probe_json_b")));

    // PERMANENT blind spot, asserted so it stays stated rather than assumed: a key assembled at
    // runtime is not in the source text at all.
    TestEqual(TEXT("a non-literal key contributes nothing (permanently unknowable)"),
        AccessorKeys.Num(), 3);

    // Shape 3: ReadFieldProjection contributes its own four wire keys and NOT its column argument.
    const TSet<FString> ProjectionKeys = Keys(TEXT("probe.projection"));
    for (const FString& Key : FieldProjectionKeys())
    {
        TestTrue(*FString::Printf(
                TEXT("ReadFieldProjection contributes the wire key '%s'"), *Key),
            ProjectionKeys.Contains(Key));
    }
    TestFalse(TEXT("ReadFieldProjection's braced argument is column names, not wire keys"),
        ProjectionKeys.Contains(TEXT("probe_column")));

    // Shape 4: reads off a local bound from Ctx.GetRawPayload(), both the member form and the
    // GetJson*Field(Object, Key) helper form -- and NOT a read off a nested sub-object.
    const TSet<FString> RawKeys = Keys(TEXT("probe.raw"));
    TestTrue(TEXT("a raw-payload member read is collected"),
        RawKeys.Contains(TEXT("probe_raw_member")));
    TestTrue(TEXT("a raw-payload GetJson*Field helper read is collected"),
        RawKeys.Contains(TEXT("probe_raw_helper")));
    TestTrue(TEXT("an unbound Ctx.GetRawPayload()->...Field read is collected"),
        RawKeys.Contains(TEXT("probe_raw_chained")));
    TestTrue(TEXT("the top-level key naming a nested object is collected"),
        RawKeys.Contains(TEXT("probe_raw_nested_owner")));
    TestFalse(TEXT("a read off a nested sub-object is not a top-level wire key"),
        RawKeys.Contains(TEXT("probe_raw_nested")));

    // ... and it is not merely dropped, it is collected on the OTHER side of the wall, where the
    // nested-schema test can compare it against what the owning parameter's description promises.
    // Both halves are asserted so a future widening cannot quietly move a nested key into the
    // top-level set, which is the failure that produced 15 false level.structure.* pairs.
    const FScannedVerb* const* RawVerb = ByMethod.Find(TEXT("probe.raw"));
    const TSet<FNestedRead> RawNested =
        (RawVerb && *RawVerb) ? (*RawVerb)->NestedReads : TSet<FNestedRead>();
    TestTrue(TEXT("a read off a nested sub-object IS collected as a nested read"),
        RawNested.Contains(FNestedRead{ TEXT("probe_raw_nested_owner"), TEXT("probe_raw_nested") }));
    TestFalse(TEXT("the top-level key naming the nested object is not itself a nested read"),
        RawNested.Contains(FNestedRead{ TEXT("probe_raw_nested_owner"),
            TEXT("probe_raw_nested_owner") }));

    // Shape 5: the one bounded call hop, and the three things it must refuse to follow.
    const FString HelperSource = TEXT(R"CPP(
namespace ProbeHelpers
{
    bool ProbeReadsThroughContext(FHandlerContext& Ctx, bool bFlag)
    {
        return Ctx.GetBool(TEXT("probe_helper_ctx"), false);
    }

    bool ProbeReadsThroughPayload(const TSharedPtr<FJsonObject>& Payload)
    {
        return Payload->HasField(TEXT("probe_helper_payload"));
    }

    bool ProbeForwardsToBoth(FHandlerContext& Ctx)
    {
        const TSharedPtr<FJsonObject>& Local = Ctx.GetRawPayload();
        return ProbeReadsThroughContext(Ctx, true) && ProbeReadsThroughPayload(Local);
    }
}

static bool ProbeFileLocalHelper(FHandlerContext& Ctx)
{
    return Ctx.GetBool(TEXT("probe_file_local"), false);
}
)CPP");

    const FString CallerSource = TEXT(R"CPP(
REGISTER_RPC_HANDLER("probe.hop", "probe", "s", RPC_NO_PARAMS)
{
    ProbeHelpers::ProbeForwardsToBoth(Ctx);
}

REGISTER_RPC_HANDLER("probe.nested_argument", "probe", "s", RPC_NO_PARAMS)
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    ProbeHelpers::ProbeReadsThroughPayload(Payload->GetObjectField(TEXT("probe_nested_owner")));
}

REGISTER_RPC_HANDLER("probe.wrong_scope", "probe", "s", RPC_NO_PARAMS)
{
    SomeOtherNamespace::ProbeReadsThroughContext(Ctx, true);
}

REGISTER_RPC_HANDLER("probe.other_file_local", "probe", "s", RPC_NO_PARAMS)
{
    ProbeFileLocalHelper(Ctx);
}
)CPP");

    FHelperIndex ProbeHelpers;
    ProbeHelpers.AddFile(TEXT("Synthetic/ProbeHelpers.h"), NeutralizeSourceText(HelperSource));
    ProbeHelpers.Close();

    TArray<FScannedVerb> HopVerbs;
    ScanFile(TEXT("Synthetic/ScannerHop.cpp"), CallerSource, HopVerbs, &ProbeHelpers);

    TMap<FString, const FScannedVerb*> HopByMethod;
    for (const FScannedVerb& Verb : HopVerbs)
    {
        HopByMethod.Add(Verb.Method, &Verb);
    }
    auto HopKeys = [&HopByMethod](const TCHAR* Method) -> TSet<FString>
    {
        const FScannedVerb* const* Found = HopByMethod.Find(Method);
        return Found && *Found ? (*Found)->HelperReadKeys : TSet<FString>();
    };

    TestEqual(TEXT("the synthetic hop source yields one FScannedVerb per registration"),
        HopVerbs.Num(), 4);

    const TSet<FString> HopVerbKeys = HopKeys(TEXT("probe.hop"));
    TestTrue(TEXT("a key read through a helper the body hands Ctx to is collected"),
        HopVerbKeys.Contains(TEXT("probe_helper_ctx")));
    TestTrue(TEXT("helper-to-helper forwarding of the raw payload is followed transitively"),
        HopVerbKeys.Contains(TEXT("probe_helper_payload")));
    TestFalse(TEXT("a helper-sourced key is not silently merged into the in-body read set"),
        (HopByMethod.Contains(TEXT("probe.hop"))
            && HopByMethod[TEXT("probe.hop")]->ReadKeys.Contains(TEXT("probe_helper_ctx"))));

    TestFalse(TEXT("passing a NESTED object to a payload helper is not followed -- counting it is ")
            TEXT("what produced 15 false level.structure.* pairs"),
        HopKeys(TEXT("probe.nested_argument")).Contains(TEXT("probe_helper_payload")));
    TestFalse(TEXT("a qualified call whose qualifier does not match the definition's scope chain ")
            TEXT("resolves to nothing"),
        HopKeys(TEXT("probe.wrong_scope")).Contains(TEXT("probe_helper_ctx")));
    TestFalse(TEXT("a static definition in another file is not a candidate"),
        HopKeys(TEXT("probe.other_file_local")).Contains(TEXT("probe_file_local")));

    // Vacuity guard for the hop: an index that stopped recognising helper definitions would make
    // every assertion above pass by finding nothing at all.
    TestTrue(TEXT("the helper index recovered the synthetic helper definitions"),
        ProbeHelpers.Helpers.Num() >= 4);

    // The recurrence guard: every FHandlerContext member whose first argument is a wire key must be
    // named in KeyTakingAccessors(), or the scan narrows silently the next time one is added.
    // Send*/Make* are the response helpers and the test-context factories -- they take an error
    // code or a request id in that position, not a wire key.
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    const FString HeaderPath = Plugin.IsValid()
        ? (Plugin->GetBaseDir() / TEXT("Source/PinWright/Private/Handlers/HandlerContext.h"))
        : FString();
    FString HeaderText;
    if (HeaderPath.IsEmpty() || !FFileHelper::LoadFileToString(HeaderText, *HeaderPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("handler-context-header-absent"),
            FString::Printf(TEXT("could not read HandlerContext.h (resolved '%s'), so the ")
                TEXT("accessor-coverage half of this test did not run"), *HeaderPath));
        return true;
    }

    const FString Header = NeutralizeSourceText(HeaderText);
    const FRegexPattern DeclPattern(
        TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\(\\s*const\\s+")
        TEXT("(?:FString|TArray\\s*<\\s*FString\\s*>)\\s*&\\s*[A-Za-z_][A-Za-z0-9_]*"));
    const TSet<FString> Covered(KeyTakingAccessors());

    int32 Checked = 0;
    FRegexMatcher DeclMatcher(DeclPattern, Header);
    while (DeclMatcher.FindNext())
    {
        const FString Name = DeclMatcher.GetCaptureGroup(1);
        if (Name.StartsWith(TEXT("Send")) || Name.StartsWith(TEXT("Make")))
        {
            continue;
        }
        ++Checked;
        if (Name.Equals(TEXT("ReadFieldProjection")))
        {
            // Covered by FieldProjectionKeys() instead of by the alternation, on purpose.
            continue;
        }
        TestTrue(*FString::Printf(
                TEXT("FHandlerContext::%s takes a wire key and is named in KeyTakingAccessors(); ")
                TEXT("an accessor missing from that list is a silent false-negative for every ")
                TEXT("verb that uses it"), *Name),
            Covered.Contains(Name));
    }

    // Vacuity guard: a regex that stopped matching the header would pass the loop above trivially.
    TestTrue(TEXT("the header scan recovered the key-taking accessor declarations"),
        Checked >= Covered.Num());

    return true;
}

// ============================================================================
    // The inverse top-level contract: every accepted spelling must appear outside its declaration.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeclaredParamsHaveSourceUseTest,
    "PinWright.infra.declared_params.DeclaredParamsHaveSourceUse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeclaredParamsHaveSourceUseTest::RunTest(const FString& Parameters)
{
    using namespace DeclaredParamCoverageTestLocal;

    TSet<FString> SourceLiterals;
    FString SourceDir;
    int32 SourceFileCount = 0;
    if (!CollectProductionLiteralsOutsideParamDeclarations(
            SourceLiterals, SourceDir, SourceFileCount))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("plugin-source-tree-absent"),
            FString::Printf(TEXT("no readable Source directory under the PinWright plugin (resolved '%s')"),
                *SourceDir));
        return true;
    }

    TSet<FString> Found;
    int32 RegistrationCount = 0;
    int32 CanonicalParamCount = 0;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName.StartsWith(TEXT("_test.")))
        {
            continue;
        }
        ++RegistrationCount;
        for (const FParamSpec& Spec : Reg.Params)
        {
            ++CanonicalParamCount;
            CollectMissingAcceptedParamSourceUses(Reg.MethodName, Spec, SourceLiterals, Found);
        }
    }

    if (!TestTrue(*FString::Printf(TEXT("walked production registrations (found %d)"),
            RegistrationCount), RegistrationCount >= 500)
        || !TestTrue(*FString::Printf(TEXT("walked canonical parameter declarations (found %d)"),
            CanonicalParamCount), CanonicalParamCount >= 1000)
        || !TestTrue(*FString::Printf(TEXT("scanned production source files (found %d)"),
            SourceFileCount), SourceFileCount >= 100)
        || !TestTrue(*FString::Printf(TEXT("collected non-declaration source literals (found %d)"),
            SourceLiterals.Num()), SourceLiterals.Num() >= 500))
    {
        return false;
    }

    const TSet<FString> Baseline = {
        // The only consumer mode, bindOn=ability_set, returns NOT_IMPLEMENTED before this value
        // could be read. The registration says so explicitly, so this is honest dead surface.
        TEXT("gas.set_ability_input:abilitySetPath")
    };

    TArray<FString> Regressions = Found.Difference(Baseline).Array();
    Regressions.Sort();
    for (const FString& Entry : Regressions)
    {
        AddError(FString::Printf(
            TEXT("%s is an accepted live RPC parameter spelling whose literal appears nowhere "
                 "outside RPC_PARAM_* declarations in production Source. Read it, remove it, or "
                 "document and ratchet a deliberately unreachable contract."), *Entry));
    }

    TArray<FString> Stale = Baseline.Difference(Found).Array();
    Stale.Sort();
    if (Stale.Num() > 0)
    {
        AddWarning(FString::Printf(
            TEXT("DeclaredParamsHaveSourceUse baseline entries no longer reproduce and should be "
                 "removed: %s"), *FString::Join(Stale, TEXT(", "))));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeclaredParamSourceUseScannerShapeTest,
    "PinWright.infra.declared_params.DeclaredParamSourceUseScannerBlanksDeclarations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeclaredParamSourceUseScannerShapeTest::RunTest(const FString& Parameters)
{
    using namespace DeclaredParamCoverageTestLocal;

    FString Probe = TEXT(R"CPP(
REGISTER_RPC_HANDLER("_test.source_use_probe", "test", "probe",
    RPC_PARAMS(
        RPC_PARAM_REQ("declaredOnly", "string", "declaration-only sentinel"),
        RPC_PARAM_REQ("usedAfterDeclaration", "string", "live-use sentinel")
    ))
{
    Ctx.GetString("usedAfterDeclaration");
}
)CPP");
    Probe = NeutralizeSourceText(Probe);

    TSet<FString> BeforeBlanking;
    CollectQuotedLiterals(Probe, BeforeBlanking);
    TestTrue(TEXT("counterfactual contains the declaration-only literal before blanking"),
        BeforeBlanking.Contains(TEXT("declaredOnly")));

    BlankRpcParamInvocations(Probe);
    TSet<FString> AfterBlanking;
    CollectQuotedLiterals(Probe, AfterBlanking);
    TestFalse(TEXT("declaration-only literal is removed by the production blanker"),
        AfterBlanking.Contains(TEXT("declaredOnly")));
    TestTrue(TEXT("literal used after its declaration remains visible to the source-use scan"),
        AfterBlanking.Contains(TEXT("usedAfterDeclaration")));

    FParamSpec SyntheticSpec{TEXT("canonicalName"), TEXT("string"), TEXT("synthetic"), false,
        TEXT("")};
    SyntheticSpec.Aliases = { TEXT("legacyName") };
    SyntheticSpec.TypedAliases = {
        FParamAliasSpec{ TEXT("typedName"), TEXT("array"), TEXT("typed synthetic") }
    };
    TSet<FString> SyntheticSourceLiterals = { TEXT("canonicalName") };
    TSet<FString> MissingAccepted;
    CollectMissingAcceptedParamSourceUses(TEXT("_test.source_use_probe"), SyntheticSpec,
        SyntheticSourceLiterals, MissingAccepted);
    TestFalse(TEXT("the canonical spelling is present in the synthetic source-use set"),
        MissingAccepted.Contains(TEXT("_test.source_use_probe:canonicalName")));
    TestTrue(TEXT("an untyped alias is checked by the source-use helper"),
        MissingAccepted.Contains(TEXT("_test.source_use_probe:canonicalName/legacyName")));
    TestTrue(TEXT("a typed alias name is checked by the source-use helper"),
        MissingAccepted.Contains(TEXT("_test.source_use_probe:canonicalName/typedName")));
    return true;
}

// ============================================================================
// The nested half of the same contract.
//
// A key nested inside an object- or array-typed parameter is invisible to the top-level scan above.
// The dispatcher checks it only when FParamSpec::NestedKeys declares a closed one-level schema;
// non-adopters remain unchecked. Board B-declared-param-guard-blind-to-nested-keys measured five
// verbs accepting a documented nested key and discarding it silently.
//
// The nested keys must NOT be diffed against RPC_PARAMS -- that is a category error and it is what
// produced 15 false level.structure.* pairs in an earlier sweep. What they CAN be diffed against
// is the parameter's own DESCRIPTION, which is the de facto schema: it is what the wiki renders,
// what an agent reads, and what the five confirmed instances were found by reading. Both
// directions are checked, because neither implies the other:
//
//   1. PROMISED BUT UNREAD. The description names a nested key inside a {...} shape and the
//      literal occurs NOWHERE in the plugin's Source -- so nothing reads it: not the body, not a
//      helper, not a key-list factory. Every false-positive class this shape is usually accused of
//      (a read on one branch, a read inside a shared helper, a key assembled from a factory) is
//      structurally suppressed by the "absent everywhere" formulation. Error direction: false
//      negatives only.
//   2. READ BUT UNPROMISED. The body reads a nested key out of an object/array parameter whose
//      description never names it, in a braced list or even in prose. The caller has no documented
//      way to learn the spelling and nothing at runtime rejects a wrong one.
//
// Both are ratcheted against a recorded baseline for the same reason the top-level scan is: these
// are dozens of verbs and several owners, and some entries are deliberate.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeclaredParamNestedSchemaTest,
    "PinWright.infra.declared_params.NestedKeysMatchTheirParameterDescriptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDeclaredParamNestedSchemaTest::RunTest(const FString& Parameters)
{
    using namespace DeclaredParamCoverageTestLocal;

    TArray<FScannedVerb> Verbs;
    TSet<FString> SourceLiterals;
    FString SourceDir;
    if (!ScanPluginTree(Verbs, SourceDir, &SourceLiterals))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("plugin-source-tree-absent"),
            FString::Printf(TEXT("no readable Source directory under the PinWright plugin (resolved '%s')"),
                *SourceDir));
        return true;
    }

    TMap<FString, const FHandlerRegistration*> RegistrationByMethod;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        RegistrationByMethod.Add(Reg.MethodName, &Reg);
    }

    TSet<FString> UnreadPromises;
    TSet<FString> UnpromisedReads;
    TMap<FString, FString> EntryToFile;
    int32 NestedSurfaceVerbs = 0;
    int32 NestedReadingVerbs = 0;
    int32 DeclaredSchemaParams = 0;

    for (const FScannedVerb& Verb : Verbs)
    {
        // Not registered here means an integration sub-module whose engine plugin is disabled on
        // this host: there is no declaration to read a description off.
        const FHandlerRegistration* const* Found = RegistrationByMethod.Find(Verb.Method);
        if (!Found || !*Found)
        {
            continue;
        }
        const FHandlerRegistration& Reg = **Found;

        bool bHasNestedSurface = false;
        for (const FParamSpec& Spec : Reg.Params)
        {
            const bool bNestedType = Spec.Type.Contains(TEXT("object"))
                || Spec.Type.Contains(TEXT("array"));
            if (!bNestedType)
            {
                continue;
            }
            bHasNestedSurface = true;

            if (Spec.NestedKeys.Num() > 0)
            {
                ++DeclaredSchemaParams;
                TSet<FString> Documented;
                if (!FirstOuterNestedSchemaKeys(Spec.Description, Documented))
                {
                    AddError(FString::Printf(
                        TEXT("%s:%s declares NestedKeys but its description has no first outer "
                             "{...} schema to compare against."), *Verb.Method, *Spec.Name));
                }
                TSet<FString> Declared;
                for (const FString& Key : Spec.NestedKeys)
                {
                    Declared.Add(Key.ToLower());
                }

                TArray<FString> Undocumented = Declared.Difference(Documented).Array();
                Undocumented.Sort();
                for (const FString& Key : Undocumented)
                {
                    AddError(FString::Printf(
                        TEXT("%s:%s NestedKeys contains '%s', but the first documented {...} "
                             "schema does not. The runtime allow-list and caller contract drifted."),
                        *Verb.Method, *Spec.Name, *Key));
                }

                TArray<FString> Undeclared = Documented.Difference(Declared).Array();
                Undeclared.Sort();
                for (const FString& Key : Undeclared)
                {
                    AddError(FString::Printf(
                        TEXT("%s:%s documents nested key '%s' in its first {...} schema, but "
                             "NestedKeys does not accept it."), *Verb.Method, *Spec.Name, *Key));
                }
            }

            TSet<FString> Promised;
            PromisedNestedKeys(Spec.Description, Promised);
            for (const FString& Key : Promised)
            {
                if (SourceLiterals.Contains(Key))
                {
                    continue;
                }
                const FString Entry = FString::Printf(TEXT("%s:%s.%s"),
                    *Verb.Method, *Spec.Name, *Key);
                UnreadPromises.Add(Entry);
                EntryToFile.Add(Entry, Verb.File);
            }
        }
        NestedSurfaceVerbs += bHasNestedSurface ? 1 : 0;
        NestedReadingVerbs += (Verb.NestedReads.Num() > 0) ? 1 : 0;

        for (const FNestedRead& Read : Verb.NestedReads)
        {
            // The owner key may be an alias; the description lives on the canonical spec. Exact
            // names win over aliases, and the two passes are separate so the answer does not
            // depend on declaration order.
            const FParamSpec* Owner = nullptr;
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.Name.Equals(Read.OwnerKey))
                {
                    Owner = &Spec;
                    break;
                }
            }
            for (int32 i = 0; !Owner && i < Reg.Params.Num(); ++i)
            {
                const FParamSpec& Spec = Reg.Params[i];
                if (Spec.Aliases.Contains(Read.OwnerKey))
                {
                    Owner = &Spec;
                    break;
                }
                for (const FParamAliasSpec& Alias : Spec.TypedAliases)
                {
                    if (Alias.Name.Equals(Read.OwnerKey))
                    {
                        Owner = &Spec;
                        break;
                    }
                }
            }
            if (!Owner)
            {
                // The body reads a nested object out of a key the registration never declares at
                // all. That is the TOP-LEVEL defect, which the scan above already reports; adding
                // it here would double-count one fault.
                continue;
            }

            TSet<FString> Promised;
            PromisedNestedKeys(Owner->Description, Promised);
            if (Promised.Contains(Read.NestedKey))
            {
                continue;
            }
            // Named in prose rather than in a braced list still counts as documented.
            const FRegexPattern WordPattern(FString(TEXT("\\b")) + Read.NestedKey + TEXT("\\b"));
            FRegexMatcher WordMatcher(WordPattern, Owner->Description);
            if (WordMatcher.FindNext())
            {
                continue;
            }
            const FString Entry = FString::Printf(TEXT("%s:%s.%s"),
                *Verb.Method, *Read.OwnerKey, *Read.NestedKey);
            UnpromisedReads.Add(Entry);
            EntryToFile.Add(Entry, Verb.File);
        }
    }

    // Vacuity guards, both directions. A collector that silently stopped matching would report no
    // findings and turn the whole baseline stale -- which is a warning, not a failure, and would
    // read as progress. The floors are a fraction of the measured population (317 verbs carrying a
    // nested surface, 50 reading one in-body) so they track the tree without being brittle.
    if (!TestTrue(
            *FString::Printf(TEXT("the registry exposes an object/array parameter surface (found %d verbs)"),
                NestedSurfaceVerbs),
            NestedSurfaceVerbs >= 100))
    {
        return false;
    }
    if (!TestTrue(
            *FString::Printf(TEXT("the nested-read collector recovered nested reads (found %d verbs)"),
                NestedReadingVerbs),
            NestedReadingVerbs >= 20))
    {
        return false;
    }
    if (!TestTrue(
            *FString::Printf(TEXT("production nested-schema adopters were compared (found %d params)"),
                DeclaredSchemaParams),
            DeclaredSchemaParams >= 3))
    {
        return false;
    }

    struct FNestedDirection
    {
        const TCHAR* Label;
        const TSet<FString>* Found;
        const TArray<FString>* Baseline;
        const TCHAR* RegressionText;
    };
    const FNestedDirection Directions[] = {
        { TEXT("promised-but-unread"), &UnreadPromises, &KnownUnreadPromisedNestedKeys(),
          TEXT("the parameter's description promises this nested key and the literal occurs ")
          TEXT("nowhere in the plugin's Source, so nothing reads it: a caller sending exactly ")
          TEXT("what the description advertises gets a success and no effect. Read the key, or ")
          TEXT("stop promising it.") },
        { TEXT("read-but-unpromised"), &UnpromisedReads, &KnownUnpromisedNestedReads(),
          TEXT("the handler reads this nested key and the owning parameter's description never ")
          TEXT("names it, so no caller can learn the spelling and nothing at runtime rejects a ")
          TEXT("wrong one. Name it in the description (a {a, b, c} list), or stop reading it.") },
    };

    for (const FNestedDirection& Direction : Directions)
    {
        TSet<FString> Baseline;
        for (const FString& Entry : *Direction.Baseline)
        {
            Baseline.Add(Entry);
        }
        TArray<FString> Regressions = Direction.Found->Difference(Baseline).Array();
        Regressions.Sort();
        for (const FString& Entry : Regressions)
        {
            AddError(FString::Printf(TEXT("%s [%s] -- %s (%s)"),
                *Entry, Direction.Label, Direction.RegressionText, *EntryToFile.FindRef(Entry)));
        }

        TArray<FString> Stale = Baseline.Difference(*Direction.Found).Array();
        Stale.Sort();
        if (Stale.Num() > 0)
        {
            AddWarning(FString::Printf(
                TEXT("%d %s baseline entr%s in TestDeclaredParamCoverage.cpp no longer reproduce ")
                TEXT("and should be deleted (fixed, or the verb is not registered on this host): %s"),
                Stale.Num(), Direction.Label, Stale.Num() == 1 ? TEXT("y") : TEXT("ies"),
                *FString::Join(Stale, TEXT(", "))));
        }
    }

    return true;
}
