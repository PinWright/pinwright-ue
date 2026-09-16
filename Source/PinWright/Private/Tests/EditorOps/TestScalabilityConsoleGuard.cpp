// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-console-command-sg-cvar-pin-freezes-scalability.
//
// THE DEFECT. `system.console_command` / `editor.console_command` executed an `sg.<Group> N`
// line verbatim and answered `success: true`. The console set writes at ECVF_SetByConsole
// (IConsoleManager.h:187), the highest of the fifteen priorities and one that never decays for
// the life of the process, while the editor's own Settings > Engine Scalability Settings panel
// writes through Scalability::SetQualityLevels at ECVF_SetByScalability (Scalability.cpp:907),
// the second-lowest. FConsoleVariableBase::CanChange (ConsoleManager.cpp:275-311) therefore
// discards every later change the HUMAN USER makes to that group until the editor restarts.
// Measured live: seven groups pinned by an agent, five untouched, which read to the user as the
// editor force-resetting quality on its own.
//
// WHAT THESE TESTS PIN. Both verbs refuse a leading `sg.` token with
// SCALABILITY_CVAR_USE_TYPED_VERB and steer to `performance.set_scalability`; `force: true`
// gets past the guard; and the aggregate `scalability N` — which routes through
// SetQualityLevels at the panel's own priority and is harmless on this axis — is NOT refused.
//
// WHY NO REAL GROUP IS EVER SET HERE, and this is load-bearing rather than fastidious: a test
// that pinned a real `sg.*` group would corrupt every later test in the same process with
// exactly the defect under repair, and the pin has no unset path short of a restart. So:
//   - the refusal cases name a group that does not exist (`sg.__PinWright_NoSuchGroup__`), so
//     that if the guard is ever reverted the line reaches Exec, finds no console object
//     (ConsoleManager.cpp:3116-3120 returns false without setting anything) and pins nothing;
//   - the `force: true` case passes a REAL group name with NO value, which is a read:
//     ProcessUserConsoleInput takes the `bShowCurrentState` branch when the argument list is
//     empty and never calls Set;
//   - the not-refused case passes bare `scalability`, which Scalability::ProcessCommand answers
//     by printing usage and current settings (Scalability.cpp:844-857) — deliberately not
//     `scalability 2`, which WOULD write all eleven groups and SaveState to the host's editor
//     ini. The `scalability 2` spelling the ticket names is asserted on the predicate instead,
//     where it costs nothing.
//
// SECOND TICKET, SAME FILE: B-console-member-cvar-pin-freezes-scalability. The rule above named
// the group; the pin is done by its MEMBERS. A scalability group is a list of ordinary cvars in a
// `[<Group>@N]` ini section, and `r.ViewDistanceScale 0.6` takes the identical
// ConsoleManager.cpp:3228 `Set(..., ECVF_SetByConsole)` that `sg.ViewDistanceQuality 1` does — so
// the `sg.` prefix rule refused the spelling almost nobody types and allowed the vocabulary of
// every performance pass. Two members were measured pinned in a live editor running the guarded
// build. The rule is now a FLAG test on the resolved console object, and the tests for it are at
// the bottom of this file.
//
// THE SAME HYGIENE PROBLEM, ONE STEP HARDER, AND HOW IT IS SOLVED. The sg. cases could name a
// group that does not exist, because the old rule was a spelling test and a fake name matched it.
// A flag test cannot be exercised by a fake name: the token has to RESOLVE and carry
// ECVF_Scalability, or the predicate correctly returns false. Naming a real one
// (`r.MaxAnisotropy 8`) would mean that a reverted guard pins a real TextureQuality member at
// ECVF_SetByConsole for the life of the process — inflicting the defect under repair on every
// later test and on the human using this editor, with no unset path short of a restart. So the
// handler-level tests register their OWN console variable carrying ECVF_Scalability
// (FScopedScalabilityProbeCVar below) and unregister it afterwards: indistinguishable to the
// guard, and worthless if it is ever pinned. The PREDICATE tests name the real engine cvars,
// which is safe because the predicate only reads the registry and never calls Set.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"

#include "Handlers/ScalabilityConsoleGuard.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) namespace, and prefixed: this file is compiled into a unity blob with
// every other test in the bucket, so a bare `AssertGuard` would collide on the next author who
// picks the same obvious name.
namespace ScalabilityConsoleGuardTestSupport
{
    const TCHAR* const PinWrightScalabilityRefusalCode = TEXT("SCALABILITY_CVAR_USE_TYPED_VERB");

    // The throwaway ECVF_Scalability variable the handler-level member-cvar tests point at. Named
    // distinctively because the console registry is process-global and shared with the host
    // project.
    const TCHAR* const PinWrightScalabilityProbeCVarName = TEXT("PinWright.Test.ScalabilityGuardProbe");

    // A token that resolves to no console object at all, used for the "an unresolvable line must
    // still reach Exec" case.
    const TCHAR* const PinWrightUnresolvableConsoleToken = TEXT("PinWrightNoSuchExecCommand_ScalabilityGuard");

    // Registers PinWrightScalabilityProbeCVarName with ECVF_Scalability for one test and removes
    // it again. See the header comment for why the refusal cases may not name a real scalability
    // cvar. Unregistering with bKeepState=false actually removes the object
    // (ConsoleManager.cpp:2602-2630) rather than flagging it ECVF_Unregistered, so nothing of it
    // survives into a later test — including a pin, if the guard is ever reverted and the force
    // case's Exec reaches the Set.
    class FScopedScalabilityProbeCVar
    {
    public:
        FScopedScalabilityProbeCVar()
        {
            // Defensive: an earlier aborted run in the same process could have left one behind,
            // and RegisterConsoleVariable over a live name is not a supported shape.
            IConsoleManager::Get().UnregisterConsoleObject(
                PinWrightScalabilityProbeCVarName, /*bKeepState=*/false);
            // (int32)0, not a bare 0: RegisterConsoleVariable is overloaded on bool / int32 /
            // float / const TCHAR* default, and a literal 0 is also a null pointer constant.
            IConsoleManager::Get().RegisterConsoleVariable(
                PinWrightScalabilityProbeCVarName, (int32)0,
                TEXT("PinWright automation probe. Registered and removed by one test."),
                ECVF_Scalability);
        }

        ~FScopedScalabilityProbeCVar()
        {
            IConsoleManager::Get().UnregisterConsoleObject(
                PinWrightScalabilityProbeCVarName, /*bKeepState=*/false);
        }

        FScopedScalabilityProbeCVar(const FScopedScalabilityProbeCVar&) = delete;
        FScopedScalabilityProbeCVar& operator=(const FScopedScalabilityProbeCVar&) = delete;

        bool IsRegistered() const
        {
            const IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(
                PinWrightScalabilityProbeCVarName, /*bTrackFrequentCalls=*/false);
            return Object != nullptr && Object->TestFlags(ECVF_Scalability);
        }
    };

    // True when this host really registers `Name` as a variable carrying ECVF_Scalability. The
    // predicate assertions on real engine cvars are gated on this rather than assumed: a host
    // that stripped a module would otherwise fail on its own registry rather than on the rule.
    bool IsLiveScalabilityCVar(const TCHAR* Name)
    {
        IConsoleObject* Object =
            IConsoleManager::Get().FindConsoleObject(Name, /*bTrackFrequentCalls=*/false);
        return Object != nullptr && Object->AsVariable() != nullptr
            && Object->TestFlags(ECVF_Scalability);
    }

    // The whole sg.* contract for one console verb. Shared so the two verbs cannot drift: the
    // ticket's failure mode was precisely that one file knew about the hazard and its siblings
    // did not.
    void AssertScalabilityConsoleContract(FAutomationTestBase& Test, const FString& Method)
    {
        // 1. An sg.* line is refused, and the refusal carries the route out.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), TEXT("sg.__PinWright_NoSuchGroup__ 3"));
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestFalse(*FString::Printf(TEXT("%s refuses an sg.* line rather than succeeding"),
                    *Method), Capture.bSuccess);
            Test.TestEqual(*FString::Printf(TEXT("%s refuses with the typed-verb code"), *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
            Test.TestTrue(*FString::Printf(TEXT("%s refusal names performance.set_scalability"),
                    *Method),
                Capture.Message.Contains(TEXT("performance.set_scalability")));
            Test.TestTrue(*FString::Printf(TEXT("%s refusal states the ECVF_SetByConsole pin"),
                    *Method),
                Capture.Message.Contains(TEXT("ECVF_SetByConsole")));
        }

        // 2. force:true gets past the guard. Asserted as "not the refusal code" rather than as a
        //    success, because what happens after the guard depends on whether this host has an
        //    editor and a world — EDITOR_NOT_AVAILABLE / EDITOR_WORLD_NOT_AVAILABLE / EXEC_FAILED
        //    are all past it, and all of them are the escape hatch staying reachable.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), TEXT("sg.FoliageQuality"));
            Payload->SetBoolField(TEXT("force"), true);
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered (force)"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestNotEqual(
                *FString::Printf(TEXT("%s with force:true is not stopped by the guard"), *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
        }

        // 3. The aggregate scalability command is not an sg.* line and must not be refused.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), TEXT("scalability"));
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered (aggregate)"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestNotEqual(
                *FString::Printf(TEXT("%s does not refuse the aggregate scalability command"),
                    *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
        }
    }

    // The member-cvar half of the contract for one console verb, i.e. what
    // B-console-member-cvar-pin-freezes-scalability asks for. Everything here points at the
    // throwaway probe rather than a real scalability member; see the file header for why that is
    // not fastidiousness.
    void AssertScalabilityMemberCVarContract(FAutomationTestBase& Test, const FString& Method)
    {
        FScopedScalabilityProbeCVar Probe;
        if (!Probe.IsRegistered())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("scalability-probe-cvar-not-registered"),
                FString::Printf(TEXT("IConsoleManager refused to register %s with ECVF_Scalability, "
                                     "so the flag rule cannot be exercised without naming a real "
                                     "scalability cvar, which this test may not pin."),
                    PinWrightScalabilityProbeCVarName));
            return;
        }

        const FString ProbeSet =
            FString::Printf(TEXT("%s 1"), PinWrightScalabilityProbeCVarName);

        // 1. Setting an ECVF_Scalability cvar that is NOT an sg.* group is refused. This is the
        //    whole ticket: before the fix the predicate was a `sg.` prefix test, this line does
        //    not start with `sg.`, and it was allowed.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), ProbeSet);
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered (member set)"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestEqual(
                *FString::Printf(TEXT("%s refuses a set of a non-sg. ECVF_Scalability cvar"),
                    *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
            Test.TestTrue(
                *FString::Printf(TEXT("%s member refusal names performance.set_scalability"),
                    *Method),
                Capture.Message.Contains(TEXT("performance.set_scalability")));
        }

        // 2. force:true is the SAME escape hatch, widened rather than duplicated.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), ProbeSet);
            Payload->SetBoolField(TEXT("force"), true);
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered (member force)"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestNotEqual(
                *FString::Printf(TEXT("%s with force:true runs a member set anyway"), *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
        }

        // 3. READING the same cvar pins nothing (ConsoleManager.cpp:3189 takes the
        //    bShowCurrentState branch and never calls Set), so it must not be refused. The sg.
        //    rule over-refuses bare reads on purpose; this rule may not, because reading a member
        //    cvar is normal and frequent.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"), PinWrightScalabilityProbeCVarName);
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered (member read)"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestNotEqual(
                *FString::Printf(TEXT("%s does not refuse a bare read of a scalability cvar"),
                    *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
        }

        // 4. A line whose first token names nothing at all — a typo, or an exec command owned by
        //    a module — must still reach Exec. Whatever verdict Exec produces (EXEC_FAILED,
        //    EDITOR_NOT_AVAILABLE, EDITOR_WORLD_NOT_AVAILABLE) is past the guard; only the
        //    refusal code would mean the guard swallowed it.
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("command"),
                FString::Printf(TEXT("%s 1"), PinWrightUnresolvableConsoleToken));
            FTestResponseCapture Capture;
            Test.TestTrue(*FString::Printf(TEXT("%s is registered (unresolvable)"), *Method),
                InvokeHandlerWithCapture(Method, Payload, Capture));
            Test.TestNotEqual(
                *FString::Printf(TEXT("%s does not refuse an unresolvable console token"), *Method),
                Capture.ErrorCode, FString(PinWrightScalabilityRefusalCode));
        }
    }
}

// ============================================================================
// The detection rule itself. Both verbs share this predicate, so the semantics
// (first token only, case-insensitive, `sg.` and not `scalability`) are pinned once here and
// the two handler tests below only have to prove each verb consults it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScalabilityConsoleGuardFirstTokenTest,
    "PinWright.core.scalability_console_guard.FirstTokenPrefixOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScalabilityConsoleGuardFirstTokenTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("'sg.FoliageQuality 3' is a scalability group set"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("sg.FoliageQuality 3")));
    TestTrue(TEXT("the match is case-insensitive"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("SG.FoliageQuality 3")));
    TestTrue(TEXT("leading whitespace does not hide the prefix"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("   sg.ShadowQuality 1")));
    TestTrue(TEXT("a bare sg.* line matches too - the rule is the token, not the argument"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("sg.FoliageQuality")));

    // The aggregate form routes through Scalability::ProcessCommand -> SetQualityLevels at
    // ECVF_SetByScalability, the same priority the editor's own panel writes at, so it cannot
    // create the pin and refusing it would be a false refusal.
    TestFalse(TEXT("'scalability 2' is NOT a scalability group set"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("scalability 2")));
    TestFalse(TEXT("bare 'scalability' is NOT a scalability group set"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("scalability")));

    // Only the FIRST token counts: the prefix inside a value or a quoted string is not a set.
    TestFalse(TEXT("'r.Foo sg.Bar' does not match on a later token"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("r.Foo sg.Bar")));
    TestFalse(TEXT("'log LogConsoleManager sg.' does not match on a trailing token"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("log LogConsoleManager sg.")));
    TestFalse(TEXT("'sgfoo 1' does not match - the dot is part of the prefix"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("sgfoo 1")));
    TestFalse(TEXT("an empty line does not match"),
        ScalabilityConsoleGuard::IsScalabilityGroupLine(TEXT("")));

    const FString Refusal =
        ScalabilityConsoleGuard::MakeScalabilityTypedVerbRefusal(TEXT("sg.FoliageQuality 3"));
    TestTrue(TEXT("the refusal names the typed verb"),
        Refusal.Contains(TEXT("performance.set_scalability")));
    TestTrue(TEXT("the refusal states the priority that causes the pin"),
        Refusal.Contains(TEXT("ECVF_SetByConsole")));
    TestTrue(TEXT("the refusal names the priority the editor's panel writes at"),
        Refusal.Contains(TEXT("ECVF_SetByScalability")));
    TestTrue(TEXT("the refusal echoes the line it refused"),
        Refusal.Contains(TEXT("sg.FoliageQuality 3")));
    return true;
}

// ============================================================================
// system.console_command — the process / GEngine-scope verb.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleCommandScalabilityGuardTest,
    "PinWright.system.console_command.ScalabilityCvarRefusedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleCommandScalabilityGuardTest::RunTest(const FString& Parameters)
{
    ScalabilityConsoleGuardTestSupport::AssertScalabilityConsoleContract(
        *this, TEXT("system.console_command"));
    return true;
}

// ============================================================================
// editor.console_command — the editor / PIE-world verb. Identical contract: the ticket's
// measured session used one of these two, and either one leaves the same pin.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandScalabilityGuardTest,
    "PinWright.editor.console_command.ScalabilityCvarRefusedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandScalabilityGuardTest::RunTest(const FString& Parameters)
{
    ScalabilityConsoleGuardTestSupport::AssertScalabilityConsoleContract(
        *this, TEXT("editor.console_command"));
    return true;
}

// The `force` slot being DECLARED (not merely read) is deliberately not asserted here:
// InvokeHandlerWithCapture never runs FRpcDispatcher::ValidateHandlerParams, and the registry
// walk PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams already fails on a body
// that reads a key its RPC_PARAMS omits. A copy here would only duplicate that walk.

// ============================================================================
// B-console-member-cvar-pin-freezes-scalability: the rule is the FLAG, not the spelling.
//
// Safe to name the real engine cvars here: IsScalabilityPinningLine only reads the console
// registry and never calls Set, so nothing in this test can pin anything. Each real-cvar
// assertion is gated on that cvar actually being registered with ECVF_Scalability on this host,
// so a stripped host reports a skip rather than a red on its own registry.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScalabilityConsoleGuardFlaggedCVarTest,
    "PinWright.core.scalability_console_guard.ScalabilityFlaggedCVarSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScalabilityConsoleGuardFlaggedCVarTest::RunTest(const FString& Parameters)
{
    using namespace ScalabilityConsoleGuardTestSupport;

    // The tokenizer the rule is built on, asserted directly so a later change to it is visible
    // here rather than only through its consequences.
    {
        FString Token;
        FString Remainder;
        ScalabilityConsoleGuard::SplitFirstConsoleToken(
            TEXT("  r.ViewDistanceScale  0.6 "), Token, Remainder);
        TestEqual(TEXT("the first token stops at whitespace"), Token,
            FString(TEXT("r.ViewDistanceScale")));
        TestEqual(TEXT("the remainder is what follows it, trimmed"), Remainder,
            FString(TEXT("0.6")));

        ScalabilityConsoleGuard::SplitFirstConsoleToken(TEXT("r.ViewDistanceScale"), Token, Remainder);
        TestTrue(TEXT("a value-less line has an empty remainder"), Remainder.IsEmpty());
    }

    // The old rule was `sg.` only, so every one of these was ALLOWED and pinned the cvar at
    // ECVF_SetByConsole. r.ScreenPercentage and r.VSync are the two the ticket singles out
    // because neither has a BaseScalability.ini row — an ini-derived name list misses both, and
    // only the declaration flag catches them.
    const TCHAR* const ScalabilityMembers[] = {
        TEXT("r.ViewDistanceScale"),
        TEXT("r.Streaming.PoolSize"),
        TEXT("r.MaxAnisotropy"),
        TEXT("r.ScreenPercentage"),
        TEXT("r.VSync"),
    };
    int32 Exercised = 0;
    for (const TCHAR* Name : ScalabilityMembers)
    {
        if (!IsLiveScalabilityCVar(Name))
        {
            continue;
        }
        ++Exercised;
        TestTrue(*FString::Printf(TEXT("setting %s is refused"), Name),
            ScalabilityConsoleGuard::IsScalabilityPinningLine(
                FString::Printf(TEXT("%s 1"), Name)));
        TestFalse(*FString::Printf(TEXT("READING %s is not refused - a read calls no Set"), Name),
            ScalabilityConsoleGuard::IsScalabilityPinningLine(Name));
        TestFalse(*FString::Printf(TEXT("the help form of %s is not refused"), Name),
            ScalabilityConsoleGuard::IsScalabilityPinningLine(
                FString::Printf(TEXT("%s ?"), Name)));
        TestTrue(*FString::Printf(
                TEXT("'%s? 1' is refused - the engine strips the trailing ? and still Sets"), Name),
            ScalabilityConsoleGuard::IsScalabilityPinningLine(
                FString::Printf(TEXT("%s? 1"), Name)));
    }
    if (Exercised == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-scalability-cvars-registered"),
            TEXT("none of r.ViewDistanceScale / r.Streaming.PoolSize / r.MaxAnisotropy / "
                 "r.ScreenPercentage / r.VSync is registered with ECVF_Scalability on this host"));
    }

    // The sg.* rule is kept as a fallback, so everything the old predicate refused is still
    // refused — including an sg.* group that is not registered at guard time, which the flag test
    // alone cannot see.
    TestTrue(TEXT("an unregistered sg.* group still matches through the prefix fallback"),
        ScalabilityConsoleGuard::IsScalabilityPinningLine(
            TEXT("sg.__PinWright_NoSuchGroup__ 3")));

    // Not refused, and each for its own engine reason.
    TestFalse(TEXT("'scalability 2' is not a console object at all - it is a UEngine::Exec branch "
                   "routing to SetQualityLevels at the panel's own priority"),
        ScalabilityConsoleGuard::IsScalabilityPinningLine(TEXT("scalability 2")));
    TestFalse(TEXT("an unresolvable token must still reach Exec"),
        ScalabilityConsoleGuard::IsScalabilityPinningLine(
            FString::Printf(TEXT("%s 1"), PinWrightUnresolvableConsoleToken)));
    TestFalse(TEXT("an empty line does not match the widened rule either"),
        ScalabilityConsoleGuard::IsScalabilityPinningLine(TEXT("")));

    // A registered cvar with no scalability flag is a plain console set and is nobody's business
    // to refuse. r.TextureStreaming is ECVF_Default | ECVF_RenderThreadSafe
    // (TextureStreamingHelpers.cpp:105, flags :110).
    if (IConsoleManager::Get().FindConsoleObject(TEXT("r.TextureStreaming"),
            /*bTrackFrequentCalls=*/false) != nullptr)
    {
        TestFalse(TEXT("a registered non-scalability cvar set is not refused"),
            ScalabilityConsoleGuard::IsScalabilityPinningLine(TEXT("r.TextureStreaming 1")));
    }

    // The refusal text has to explain the widened rule, not only the sg.* one, or a caller who
    // hits it on r.ViewDistanceScale is told about a group they never mentioned.
    const FString Refusal =
        ScalabilityConsoleGuard::MakeScalabilityTypedVerbRefusal(TEXT("r.ViewDistanceScale 0.6"));
    TestTrue(TEXT("the member refusal echoes the line it refused"),
        Refusal.Contains(TEXT("r.ViewDistanceScale 0.6")));
    TestTrue(TEXT("the member refusal names the flag the rule tests"),
        Refusal.Contains(TEXT("ECVF_Scalability")));
    TestTrue(TEXT("the member refusal still names the typed verb"),
        Refusal.Contains(TEXT("performance.set_scalability")));
    return true;
}

// ============================================================================
// The two console verbs, member-cvar half. Same shape as the sg.* pair above and deliberately a
// separate id: a red here says "the flag rule did not reach this verb", which is a different
// finding from "the sg. rule did not".
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemConsoleCommandScalabilityMemberGuardTest,
    "PinWright.system.console_command.ScalabilityMemberCvarRefusedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemConsoleCommandScalabilityMemberGuardTest::RunTest(const FString& Parameters)
{
    ScalabilityConsoleGuardTestSupport::AssertScalabilityMemberCVarContract(
        *this, TEXT("system.console_command"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorConsoleCommandScalabilityMemberGuardTest,
    "PinWright.editor.console_command.ScalabilityMemberCvarRefusedWithoutForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorConsoleCommandScalabilityMemberGuardTest::RunTest(const FString& Parameters)
{
    ScalabilityConsoleGuardTestSupport::AssertScalabilityMemberCVarContract(
        *this, TEXT("editor.console_command"));
    return true;
}
