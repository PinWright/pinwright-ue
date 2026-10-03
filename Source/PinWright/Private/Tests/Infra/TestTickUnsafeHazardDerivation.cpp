// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tick-unsafety DERIVED from source, not only declared. A verb is gated because someone
// remembered to register it with REGISTER_RPC_HANDLER_TICK_UNSAFE (or list it in the legacy
// Dispatch/SafePoint.cpp table); every gap so far - four level verbs, the capture family,
// material.authoring.compile_material, 59 Blueprint mutators - was found by an editor death,
// because the only check (TickUnsafeMethodsAreRegistered) catches typos, not omissions.
//
// This scan reads plugin source (Tests/ excluded) and finds every registered verb whose
// handler body reaches one of the hazard calls the safe-point families are written around,
// directly or through ONE call hop into a helper defined in the plugin:
//   CompileBlueprintWithDiagnostics - the only full-compile site (L/K; the direct
//       FKismetEditorUtilities::CompileBlueprint calls are pinned to it by
//       PinWright.infra.tick_safety.HandlerHazardsStayGated)
//   CollectGarbage, EditorDestroyWorld, NewMap (A), FlushRenderingCommands, PumpViewport,
//   Viewport->Draw (B, H.4, I), ReloadPackages (J).
// A body that calls RunAtSafePoint / DeferJobToSafePoint / DeferRequestToSafePoint /
// DeferToSafePoint is treated as gated in-handler, and so is a helper that does. Such a verb
// must be IsTickUnsafeMethod(); the ones that are not are recorded in KnownUngatedHazardVerbs
// below, so the count can only go down.
//
// Limits, stated rather than hidden: one hop only (a hazard two helpers deep is not seen);
// member calls (. / ->) are not followed; a helper is matched by its qualified name
// (Class::Name or the enclosing namespace/struct/class), an unqualified call only by an
// unqualified definition. Handler-to-handler cross dispatch (FRpcDispatcher::DispatchMethod)
// bypasses the dispatcher gate however the callee is declared - see Dispatch/SafePoint.h.

#include "Misc/AutomationTest.h"
#include "Algo/Count.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"

#include "Dispatch/SafePoint.h"
#include "Tests/TestUtils.h"

namespace TickUnsafeHazardDerivation
{
    // Verbs that reach a hazard ungated today. Each is a known gap, not an exemption: fix it
    // (REGISTER_RPC_HANDLER_TICK_UNSAFE, or an in-handler safe-point gate) and delete the entry.
    // Empty when seeded: the one verb the first sweep found, editor.jump_to_bookmark
    // (ForceRedrawViewportClient -> Viewport->Draw()), was gated in the same change.
    const TArray<FString>& KnownUngatedHazardVerbs()
    {
        static const TArray<FString> Verbs;
        return Verbs;
    }

    // Scanner sanity floor: 70 verbs reach a hazard at the time of writing, 58 of them through
    // the Blueprint compile chokepoint. Far fewer means the scanner went blind, not that the
    // plugin got safer.
    constexpr int32 MinimumHazardReachingVerbs = 60;

    struct FSource
    {
        FString Path;
        FString Text;   // NeutralizeSourceText: comments blanked, string literals intact
        FString Code;   // Text with string/char literal contents blanked too
    };

    bool IsIdent(TCHAR C)
    {
        return FChar::IsAlnum(C) || C == TEXT('_');
    }

    FString MaskLiterals(const FString& In)
    {
        FString Out = In;
        const int32 N = In.Len();
        int32 I = 0;
        while (I < N)
        {
            const TCHAR C = In[I];
            if (C == TEXT('"') || C == TEXT('\''))
            {
                ++I;
                while (I < N)
                {
                    if (In[I] == TEXT('\\') && I + 1 < N)
                    {
                        Out[I] = TEXT(' ');
                        Out[I + 1] = TEXT(' ');
                        I += 2;
                        continue;
                    }
                    if (In[I] == C)
                    {
                        ++I;
                        break;
                    }
                    if (In[I] != TEXT('\n'))
                    {
                        Out[I] = TEXT(' ');
                    }
                    ++I;
                }
                continue;
            }
            ++I;
        }
        return Out;
    }

    FSource MakeSource(const FString& Path, const FString& Raw)
    {
        FSource Source;
        Source.Path = Path;
        Source.Text = NeutralizeSourceText(Raw);
        Source.Code = MaskLiterals(Source.Text);
        return Source;
    }

    int32 Match(const FString& Code, int32 Open, TCHAR OpenChar, TCHAR CloseChar)
    {
        int32 Depth = 0;
        for (int32 I = Open; I < Code.Len(); ++I)
        {
            if (Code[I] == OpenChar)
            {
                ++Depth;
            }
            else if (Code[I] == CloseChar && --Depth == 0)
            {
                return I;
            }
        }
        return INDEX_NONE;
    }

    // The identifier that a '(' at ParenIdx calls, with only whitespace between them.
    bool IdentBefore(const FString& Code, int32 ParenIdx, int32& OutStart, FString& OutName)
    {
        int32 P = ParenIdx - 1;
        while (P >= 0 && FChar::IsWhitespace(Code[P]))
        {
            --P;
        }
        const int32 End = P;
        while (P >= 0 && IsIdent(Code[P]))
        {
            --P;
        }
        OutStart = P + 1;
        if (OutStart > End || FChar::IsDigit(Code[OutStart]))
        {
            return false;
        }
        OutName = Code.Mid(OutStart, End - OutStart + 1);
        return true;
    }

    // X in `X::Name(`, else empty.
    FString Qualifier(const FString& Code, int32 NameStart)
    {
        int32 P = NameStart - 1;
        while (P >= 0 && FChar::IsWhitespace(Code[P]))
        {
            --P;
        }
        if (P < 1 || Code[P] != TEXT(':') || Code[P - 1] != TEXT(':'))
        {
            return FString();
        }
        P -= 2;
        while (P >= 0 && FChar::IsWhitespace(Code[P]))
        {
            --P;
        }
        const int32 End = P;
        while (P >= 0 && IsIdent(Code[P]))
        {
            --P;
        }
        return Code.Mid(P + 1, End - P);
    }

    FString ReadIdent(const FString& Code, int32 Start, int32 Limit)
    {
        int32 P = Start;
        if (P >= Limit || !(FChar::IsAlpha(Code[P]) || Code[P] == TEXT('_')))
        {
            return FString();
        }
        while (P < Limit && IsIdent(Code[P]))
        {
            ++P;
        }
        return Code.Mid(Start, P - Start);
    }

    bool IsApiMacro(const FString& Word)
    {
        if (Word.Len() < 5 || !FChar::IsUpper(Word[0]) || !Word.EndsWith(TEXT("_API"), ESearchCase::CaseSensitive))
        {
            return false;
        }
        for (const TCHAR C : Word)
        {
            if (!(FChar::IsUpper(C) || FChar::IsDigit(C) || C == TEXT('_')))
            {
                return false;
            }
        }
        return true;
    }

    // Name declared by the last `namespace|struct|class [X_API] Name` in Code[From, To).
    FString LastScopeName(const FString& Code, int32 From, int32 To)
    {
        static const TCHAR* const Keywords[] = { TEXT("namespace"), TEXT("struct"), TEXT("class") };
        FString Last;
        int32 J = From;
        while (J < To)
        {
            bool bMatched = false;
            for (const TCHAR* Keyword : Keywords)
            {
                const int32 KeywordLen = FCString::Strlen(Keyword);
                if (J + KeywordLen > To || FCString::Strncmp(*Code + J, Keyword, KeywordLen) != 0 ||
                    (J > 0 && IsIdent(Code[J - 1])))
                {
                    continue;
                }
                int32 K = J + KeywordLen;
                if (K >= To || !FChar::IsWhitespace(Code[K]))
                {
                    continue;
                }
                while (K < To && FChar::IsWhitespace(Code[K]))
                {
                    ++K;
                }
                FString Name = ReadIdent(Code, K, To);
                if (Name.IsEmpty())
                {
                    continue;
                }
                int32 MatchEnd = K + Name.Len();
                if (IsApiMacro(Name) && MatchEnd < To && FChar::IsWhitespace(Code[MatchEnd]))
                {
                    int32 W = MatchEnd;
                    while (W < To && FChar::IsWhitespace(Code[W]))
                    {
                        ++W;
                    }
                    const FString Next = ReadIdent(Code, W, To);
                    if (!Next.IsEmpty())
                    {
                        Name = Next;
                        MatchEnd = W + Next.Len();
                    }
                }
                Last = Name;
                J = MatchEnd;
                bMatched = true;
                break;
            }
            if (!bMatched)
            {
                ++J;
            }
        }
        return Last;
    }

    // '{' offset -> innermost named namespace/struct/class around it (empty if none).
    TMap<int32, FString> BraceScopes(const FString& Code)
    {
        TMap<int32, FString> Out;
        TArray<FString> Stack;
        int32 Boundary = -1;
        for (int32 I = 0; I < Code.Len(); ++I)
        {
            const TCHAR C = Code[I];
            if (C == TEXT('{'))
            {
                FString Enclosing;
                for (int32 S = Stack.Num() - 1; S >= 0; --S)
                {
                    if (!Stack[S].IsEmpty())
                    {
                        Enclosing = Stack[S];
                        break;
                    }
                }
                Out.Add(I, Enclosing);
                Stack.Add(LastScopeName(Code, Boundary + 1, I));
                Boundary = I;
            }
            else if (C == TEXT('}'))
            {
                if (Stack.Num() > 0)
                {
                    Stack.Pop();
                }
                Boundary = I;
            }
            else if (C == TEXT(';'))
            {
                Boundary = I;
            }
        }
        return Out;
    }

    bool IsGateName(const FString& Name)
    {
        return Name == TEXT("RunAtSafePoint") || Name == TEXT("DeferJobToSafePoint") ||
            Name == TEXT("DeferRequestToSafePoint") || Name == TEXT("DeferToSafePoint");
    }

    bool IsHazardCall(const FString& Code, int32 NameStart, const FString& Name)
    {
        if (Name == TEXT("Draw"))
        {
            return NameStart >= 10 && Code.Mid(NameStart - 10, 10) == TEXT("Viewport->");
        }
        return Name == TEXT("CompileBlueprintWithDiagnostics") || Name == TEXT("CollectGarbage") ||
            Name == TEXT("EditorDestroyWorld") || Name == TEXT("NewMap") ||
            Name == TEXT("FlushRenderingCommands") || Name == TEXT("PumpViewport") ||
            Name == TEXT("ReloadPackages");
    }

    // The first hazard a body calls, or empty when it calls none or calls a safe-point gate.
    FString FirstHazard(const FString& Body)
    {
        FString Hazard;
        for (int32 I = 0; I < Body.Len(); ++I)
        {
            int32 Start = 0;
            FString Name;
            if (Body[I] != TEXT('(') || !IdentBefore(Body, I, Start, Name))
            {
                continue;
            }
            if (IsGateName(Name))
            {
                return FString();
            }
            if (Hazard.IsEmpty() && IsHazardCall(Body, Start, Name))
            {
                Hazard = Name;
            }
        }
        return Hazard;
    }

    bool CallsGate(const FString& Body)
    {
        for (int32 I = 0; I < Body.Len(); ++I)
        {
            int32 Start = 0;
            FString Name;
            if (Body[I] == TEXT('(') && IdentBefore(Body, I, Start, Name) && IsGateName(Name))
            {
                return true;
            }
        }
        return false;
    }

    // Every function definition in the source whose body reaches a hazard: key -> hazard.
    void IndexHazardHelpers(const FSource& Source, TMap<FString, FString>& Helpers)
    {
        static const TSet<FString> NotFunctions = {
            TEXT("if"), TEXT("for"), TEXT("while"), TEXT("switch"), TEXT("catch"), TEXT("return"),
            TEXT("sizeof"), TEXT("decltype"), TEXT("alignof"), TEXT("static_assert"), TEXT("TEXT"),
            TEXT("defined"), TEXT("else"), TEXT("constexpr") };
        static const TCHAR* const Qualifiers[] = {
            TEXT("const"), TEXT("override"), TEXT("final"), TEXT("noexcept"), TEXT("mutable") };

        const FString& Code = Source.Code;
        const TMap<int32, FString> Scopes = BraceScopes(Code);
        for (int32 I = 0; I < Code.Len(); ++I)
        {
            int32 Start = 0;
            FString Name;
            if (Code[I] != TEXT('(') || !IdentBefore(Code, I, Start, Name) ||
                NotFunctions.Contains(Name) || Name.StartsWith(TEXT("REGISTER_RPC"), ESearchCase::CaseSensitive) ||
                Name.StartsWith(TEXT("IMPLEMENT_"), ESearchCase::CaseSensitive))
            {
                continue;
            }
            const int32 Close = Match(Code, I, TEXT('('), TEXT(')'));
            if (Close == INDEX_NONE)
            {
                continue;
            }
            int32 P = Close + 1;
            while (true)
            {
                while (P < Code.Len() && FChar::IsWhitespace(Code[P]))
                {
                    ++P;
                }
                bool bSkipped = false;
                for (const TCHAR* Qual : Qualifiers)
                {
                    const int32 QualLen = FCString::Strlen(Qual);
                    if (P + QualLen <= Code.Len() && FCString::Strncmp(*Code + P, Qual, QualLen) == 0 &&
                        (P + QualLen >= Code.Len() || !IsIdent(Code[P + QualLen])))
                    {
                        P += QualLen;
                        bSkipped = true;
                        break;
                    }
                }
                if (!bSkipped)
                {
                    break;
                }
            }
            if (P >= Code.Len() || Code[P] != TEXT('{'))
            {
                continue;
            }
            const int32 BodyClose = Match(Code, P, TEXT('{'), TEXT('}'));
            if (BodyClose == INDEX_NONE)
            {
                continue;
            }
            const FString Hazard = FirstHazard(Code.Mid(P, BodyClose - P + 1));
            if (Hazard.IsEmpty())
            {
                continue;
            }
            FString Scope = Qualifier(Code, Start);
            if (Scope.IsEmpty())
            {
                Scope = Scopes.FindRef(P);
            }
            const FString Key = Scope.IsEmpty() ? Name : Scope + TEXT("::") + Name;
            if (!Helpers.Contains(Key))
            {
                Helpers.Add(Key, Hazard);
            }
        }
    }

    // Registered verbs in Source whose body reaches a hazard: verb -> why.
    void ScanRegistrations(const FSource& Source, const TMap<FString, FString>& Helpers,
                           TMap<FString, FString>& OutReaching)
    {
        static const TSet<FString> CallPrefixWords = {
            TEXT("return"), TEXT("else"), TEXT("new"), TEXT("throw"), TEXT("case"),
            TEXT("co_return"), TEXT("do") };

        const FString& Code = Source.Code;
        int32 Search = 0;
        while (true)
        {
            int32 Open = INDEX_NONE;
            if (FindNextRpcRegistration(Code, Search, &Open) == INDEX_NONE)
            {
                break;
            }
            Search = Open + 1;
            const FString Method = RpcRegistrationMethod(Source.Text, Open);
            const int32 Close = Match(Code, Open, TEXT('('), TEXT(')'));
            if (Method.IsEmpty() || Close == INDEX_NONE)
            {
                continue;
            }
            int32 P = Close + 1;
            while (P < Code.Len() && FChar::IsWhitespace(Code[P]))
            {
                ++P;
            }
            if (P >= Code.Len() || Code[P] != TEXT('{'))
            {
                continue;
            }
            const int32 BodyClose = Match(Code, P, TEXT('{'), TEXT('}'));
            if (BodyClose == INDEX_NONE)
            {
                continue;
            }
            const FString Body = Code.Mid(P, BodyClose - P + 1);
            FString Why = FirstHazard(Body);
            if (Why.IsEmpty() && !CallsGate(Body))
            {
                for (int32 I = 0; I < Body.Len() && Why.IsEmpty(); ++I)
                {
                    int32 Start = 0;
                    FString Name;
                    if (Body[I] != TEXT('(') || !IdentBefore(Body, I, Start, Name))
                    {
                        continue;
                    }
                    int32 S = Start - 1;
                    while (S >= 0 && FChar::IsWhitespace(Body[S]))
                    {
                        --S;
                    }
                    if (S >= 0 && (Body[S] == TEXT('.') || Body[S] == TEXT('>')))
                    {
                        continue;   // member call (. / ->) or a `Type<...> Name(` declaration
                    }
                    if (S >= 0 && IsIdent(Body[S]))
                    {
                        int32 W = S;
                        while (W >= 0 && IsIdent(Body[W]))
                        {
                            --W;
                        }
                        if (!CallPrefixWords.Contains(Body.Mid(W + 1, S - W)))
                        {
                            continue;   // `Type Name(args)`: a declaration, not a call
                        }
                    }
                    const FString Qual = Qualifier(Body, Start);
                    const FString Key = Qual.IsEmpty() ? Name : Qual + TEXT("::") + Name;
                    if (const FString* Hazard = Helpers.Find(Key))
                    {
                        Why = Key + TEXT(" -> ") + *Hazard;
                    }
                }
            }
            if (!Why.IsEmpty() && !OutReaching.Contains(Method))
            {
                OutReaching.Add(Method, FString::Printf(TEXT("%s (%s)"), *Why, *Source.Path));
            }
        }
    }

    TMap<FString, FString> Scan(const TArray<FSource>& Sources)
    {
        TMap<FString, FString> Helpers;
        for (const FSource& Source : Sources)
        {
            IndexHazardHelpers(Source, Helpers);
        }
        TMap<FString, FString> Reaching;
        for (const FSource& Source : Sources)
        {
            ScanRegistrations(Source, Helpers, Reaching);
        }
        return Reaching;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTickUnsafeHazardReachingVerbsAreGatedTest,
    "PinWright.infra.tick_safety.HazardReachingVerbsAreGated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTickUnsafeHazardReachingVerbsAreGatedTest::RunTest(const FString& Parameters)
{
    using namespace TickUnsafeHazardDerivation;

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    const FString SourceRoot = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Source") : FString();
    if (!TestTrue(TEXT("PinWright source tree is on disk"),
                  !SourceRoot.IsEmpty() && IFileManager::Get().DirectoryExists(*SourceRoot)))
    {
        return false;
    }

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *SourceRoot, TEXT("*.cpp"), true, false, false);
    IFileManager::Get().FindFilesRecursive(Files, *SourceRoot, TEXT("*.h"), true, false, false);
    Files.Sort();

    const FString FullRoot = FPaths::ConvertRelativePathToFull(SourceRoot) / TEXT("");
    TArray<FSource> Sources;
    for (const FString& File : Files)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(File);
        FPaths::MakePathRelativeTo(Relative, *FullRoot);
        Relative = Relative.Replace(TEXT("\\"), TEXT("/"));
        if ((TEXT("/") + Relative).Contains(TEXT("/Tests/")))
        {
            continue;
        }
        FString Raw;
        if (!FFileHelper::LoadFileToString(Raw, *File))
        {
            AddError(FString::Printf(TEXT("Failed to read plugin source file: %s"), *File));
            continue;
        }
        Sources.Add(MakeSource(Relative, Raw));
    }

    const TMap<FString, FString> Reaching = Scan(Sources);
    TestTrue(*FString::Printf(TEXT("the scan finds at least %d hazard-reaching verbs (found %d)"),
                              MinimumHazardReachingVerbs, Reaching.Num()),
             Reaching.Num() >= MinimumHazardReachingVerbs);

    TSet<FString> Ungated;
    for (const TPair<FString, FString>& Pair : Reaching)
    {
        if (IsHandlerRegistered(Pair.Key) && !PinWrightSafePoint::IsTickUnsafeMethod(Pair.Key))
        {
            Ungated.Add(Pair.Key);
        }
    }

    TSet<FString> Baseline;
    for (const FString& Verb : KnownUngatedHazardVerbs())
    {
        Baseline.Add(Verb);
    }

    TArray<FString> Regressions = Ungated.Difference(Baseline).Array();
    Regressions.Sort();
    for (const FString& Verb : Regressions)
    {
        AddError(FString::Printf(
            TEXT("%s reaches a tick-unsafe hazard but is not gated: %s. Register it with ")
            TEXT("REGISTER_RPC_HANDLER_TICK_UNSAFE (Handlers/HandlerRegistration.h), or gate the ")
            TEXT("hazard in-handler with RunAtSafePoint / DeferJobToSafePoint when a cross-dispatch ")
            TEXT("caller can reach it (Dispatch/SafePoint.h)."),
            *Verb, *Reaching.FindRef(Verb)));
    }

    TArray<FString> Stale = Baseline.Difference(Ungated).Array();
    Stale.Sort();
    if (Stale.Num() > 0)
    {
        AddWarning(FString::Printf(
            TEXT("%d KnownUngatedHazardVerbs entr%s in TestTickUnsafeHazardDerivation.cpp no longer ")
            TEXT("reproduce and should be deleted (gated now, or not registered on this host): %s"),
            Stale.Num(), Stale.Num() == 1 ? TEXT("y") : TEXT("ies"),
            *FString::Join(Stale, TEXT(", "))));
    }
    return true;
}

// The scan above is a source matcher, so a shape it stops matching costs it nothing and reports
// nothing: the verb simply looks safe. This pins each followed shape and each shape that must
// NOT count against synthetic source. The macro names are spliced in at runtime so this file's
// own text never looks like a registration to the plugin's other source scanners.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTickUnsafeHazardScannerShapesTest,
    "PinWright.infra.tick_safety.HazardScannerSeesEveryFollowedShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTickUnsafeHazardScannerShapesTest::RunTest(const FString& Parameters)
{
    using namespace TickUnsafeHazardDerivation;

    const FString Plain = RpcRegistrationMacroNames()[0];
    const FString Mutating = RpcRegistrationMacroNames()[1];
    const FString TickUnsafe = RpcRegistrationMacroNames()[2];

    const FString Helpers =
        TEXT("static void FileHelper(FHandlerContext& Ctx) { FlushRenderingCommands(); }\n")
        TEXT("namespace NsA { void Nested() { CollectGarbage(RF_NoFlags); } }\n")
        TEXT("struct PINWRIGHT_API FKlass { static void Run() { PumpViewport(); } };\n")
        TEXT("bool FOwner::Compile(UBlueprint* BP) const { return CompileBlueprintWithDiagnostics(BP); }\n")
        TEXT("static void GatedHelper(FHandlerContext& Ctx) { RunAtSafePoint(Ctx, TEXT(\"x\"), []() { FlushRenderingCommands(); }); }\n");

    struct FCase
    {
        const TCHAR* Verb;
        const FString* Macro;
        const TCHAR* Body;
        bool bReaches;
    };
    const FCase Cases[] = {
        { TEXT("t.direct"), &Plain, TEXT("{ FlushRenderingCommands(); return true; }"), true },
        { TEXT("t.hop"), &Mutating, TEXT("{ FileHelper(Ctx); return true; }"), true },
        { TEXT("t.hop_return"), &Plain, TEXT("{ return FileHelper(Ctx); }"), true },
        { TEXT("t.namespace_hop"), &TickUnsafe, TEXT("{ NsA::Nested(); return true; }"), true },
        { TEXT("t.class_hop"), &Plain, TEXT("{ FKlass::Run(); return true; }"), true },
        { TEXT("t.qualified_definition"), &Plain, TEXT("{ return FOwner::Compile(BP); }"), true },
        { TEXT("t.viewport_draw"), &Plain, TEXT("{ SceneViewport->Draw(); return true; }"), true },
        { TEXT("t.member_call"), &Plain, TEXT("{ Obj.FileHelper(Ctx); Ptr->FileHelper(Ctx); return true; }"), false },
        { TEXT("t.declaration"), &Plain, TEXT("{ FFoo FileHelper(Ctx); return true; }"), false },
        { TEXT("t.other_namespace"), &Plain, TEXT("{ NsB::Nested(); Compile(BP); return true; }"), false },
        { TEXT("t.comment_and_string"), &Plain,
          TEXT("{ // FlushRenderingCommands();\n Log(TEXT(\"FlushRenderingCommands()\")); return true; }"), false },
        { TEXT("t.gated_body"), &Plain,
          TEXT("{ RunAtSafePoint(Ctx, TEXT(\"t\"), []() { FlushRenderingCommands(); }); return true; }"), false },
        { TEXT("t.gated_helper"), &Plain, TEXT("{ GatedHelper(Ctx); return true; }"), false },
    };

    FString Sample = Helpers;
    for (const FCase& Case : Cases)
    {
        // The method literal on the next line is the multi-line registration shape.
        Sample += FString::Printf(TEXT("%s(\n    \"%s\", \"t\", \"Summary\", RPC_NO_PARAMS)\n%s\n"),
                                  **Case.Macro, Case.Verb, Case.Body);
    }

    TArray<FSource> Sources;
    Sources.Add(MakeSource(TEXT("Synthetic.cpp"), Sample));
    const TMap<FString, FString> Reaching = Scan(Sources);

    for (const FCase& Case : Cases)
    {
        TestEqual(*FString::Printf(TEXT("%s reaches a hazard"), Case.Verb),
                  Reaching.Contains(Case.Verb), Case.bReaches);
    }
    TestEqual(TEXT("nothing outside the cases is reported"), Reaching.Num(),
              static_cast<int32>(Algo::CountIf(Cases, [](const FCase& Case) { return Case.bReaches; })));
    return true;
}
