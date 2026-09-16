// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the actor.find_by_name subsystem guard
// (Handlers/Actor/ActorSubsystemUtils.h, consumed by Handlers/Actor/QueryHandler.cpp).
//
// The defect: actor.find_by_name read
//
//     UEditorActorSubsystem *ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
//     const TArray<AActor *> AllActors = ActorSS->GetAllLevelActors();
//
// with no check on either pointer. A null GEditor faults on the first line inside
// GetEditorSubsystem (UEditorEngine::EditorSubsystemCollection is a member read,
// EditorEngine.h:3383-3389); a null subsystem is a member call through a null this on
// the second. A read-only query answered with an access violation instead of a response.
//
// Why the test is shaped this way: a live editor ALWAYS has UEditorActorSubsystem, so the
// null branch cannot be reached by dispatching the verb - there is no payload, no world
// state and no fixture that makes GetEditorSubsystem return nullptr. A test that merely
// calls actor.find_by_name and checks it succeeded would pass verbatim against the
// crashing code and prove nothing. So the guard was extracted into a pure seam that takes
// the subsystem pointer as an ARGUMENT (ActorSubsystemUtils::QueryAllLevelActors), and
// these tests pass nullptr to it directly. Same habit as the codebase's other shared
// ...Utils.h seams (ActorQueryParamUtils, PostProcessVolumeUtils).
//
// Red before green, by construction: pre-fix there is no branch anywhere in the tree that
// answers "subsystem is null" with anything at all - the two lines quoted above are the
// entire code path. Transplant them verbatim into the seam body
// (`return ActorSS->GetAllLevelActors();`) and NullSubsystemIsATypedErrorNotACrash cannot
// pass: it is a member call through nullptr, undefined behaviour that either faults or -
// because UEditorActorSubsystem::GetAllLevelActors touches no member of this
// (EditorActorSubsystem.cpp:369-396) - falls through to GEditor and returns a POPULATED
// array with bOk unset and ErrorCode empty. Every assertion below (bOk false, code
// EDITOR_ACTOR_SUBSYSTEM_MISSING, zero actors, error dispatched through the context)
// fails on that body. It is exactly the branch the fix adds.
//
// The second test closes the other half of the contract: the code the guard emits must be
// a registered ERR_* spelling in Handlers/ErrorCodes.h, so the guard can never be the
// thing that fails PinWright.core.error_codes.AllEmittedCodesAreRegistered.
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Handlers/Actor/ActorSubsystemUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Tests/TestUtils.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with the identically shaped helpers in
// the sibling Tests/Actor/*.cpp files.
namespace ActorFindByNameSubsystemGuardTestLocal
{
    // The one code this guard is allowed to emit. Declared as a literal rather than
    // reused from ErrorCodes:: so the registration test below compares two independently
    // written spellings - a rename of the constant that forgot the callsite still fails.
    const TCHAR* ExpectedGuardCode()
    {
        return TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING");
    }

    // Handlers/ErrorCodes.h on disk, resolved the same way TestErrorCodeRegistry.cpp
    // resolves it (through IPluginManager, not a relative path).
    FString ErrorCodesHeaderPath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / TEXT("ErrorCodes.h");
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameNullSubsystemIsTypedErrorTest,
    "PinWright.actor.find_by_name.NullSubsystemIsATypedErrorNotACrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameNullSubsystemIsTypedErrorTest::RunTest(const FString& Parameters)
{
    const FString ExpectedCode(ActorFindByNameSubsystemGuardTestLocal::ExpectedGuardCode());

    // ---- the pure seam, called with the pointer the handler never checked ----
    const ActorSubsystemUtils::FLevelActorsQuery NullQuery =
        ActorSubsystemUtils::QueryAllLevelActors(nullptr);

    TestFalse(TEXT("a null EditorActorSubsystem is a failure, not an empty success"),
        NullQuery.bOk);
    TestEqual(TEXT("the null branch reports EDITOR_ACTOR_SUBSYSTEM_MISSING"),
        NullQuery.ErrorCode, ExpectedCode);
    TestEqual(TEXT("the null branch yields no actors at all"),
        NullQuery.Actors.Num(), 0);
    TestFalse(TEXT("the null branch carries a message, not a bare code"),
        NullQuery.ErrorMessage.IsEmpty());
    // The message must stay greppable alongside the sibling guards that already emit
    // this code (LightingHandler, NiagaraHandler, EffectHandler, PhysicsHandler).
    TestTrue(TEXT("the message names the missing subsystem"),
        NullQuery.ErrorMessage.Contains(TEXT("EditorActorSubsystem")));

    // ---- the handler-facing form: the failure must be DISPATCHED, not just returned ----
    // OutActors is pre-seeded with a bogus entry so "left untouched" cannot masquerade as
    // "reset": a caller that ignored the bool must still iterate nothing.
    FTestResponseCapture Capture;
    FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
        TEXT("test-find-by-name-null-subsystem"), TEXT("actor.find_by_name"),
        MakeShared<FJsonObject>(), &Capture);

    TArray<AActor*> OutActors;
    OutActors.Add(nullptr);
    const bool bRequireOk =
        ActorSubsystemUtils::RequireAllLevelActors(Ctx, nullptr, OutActors);

    TestFalse(TEXT("RequireAllLevelActors refuses a null subsystem"), bRequireOk);
    TestEqual(TEXT("the out array is cleared on refusal"), OutActors.Num(), 0);
    TestTrue(TEXT("the refusal actually sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("the response is an error, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("the dispatched code is EDITOR_ACTOR_SUBSYSTEM_MISSING"),
        Capture.ErrorCode, ExpectedCode);
    TestFalse(TEXT("the dispatched message is non-empty"), Capture.Message.IsEmpty());

    // ---- the guard must not have inverted: a present subsystem still returns rows ----
    // Without this, a seam hard-coded to fail would pass everything above.
    UEditorActorSubsystem* LiveSS = ActorSubsystemUtils::GetEditorActorSubsystem();
    if (LiveSS)
    {
        const ActorSubsystemUtils::FLevelActorsQuery LiveQuery =
            ActorSubsystemUtils::QueryAllLevelActors(LiveSS);
        TestTrue(TEXT("a present EditorActorSubsystem is not refused"), LiveQuery.bOk);
        TestTrue(TEXT("the success branch carries no error code"),
            LiveQuery.ErrorCode.IsEmpty());
    }
    else
    {
        AddWarning(TEXT("No UEditorActorSubsystem in this process; skipped the "
                        "present-subsystem half of the guard contract."));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameSubsystemGuardCodeRegisteredTest,
    "PinWright.actor.find_by_name.SubsystemGuardCodeIsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameSubsystemGuardCodeRegisteredTest::RunTest(const FString& Parameters)
{
    const FString ExpectedCode(ActorFindByNameSubsystemGuardTestLocal::ExpectedGuardCode());

    TestTrue(TEXT("actor.find_by_name is registered"),
        IsHandlerRegistered(TEXT("actor.find_by_name")));

    // The constant the guard actually emits must BE that spelling. Guards against a
    // rename that changes the wire code while every other assertion still passes.
    TestEqual(TEXT("ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING is the code the guard emits"),
        FString(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING), ExpectedCode);

    // ...and that spelling must be declared in the registry header on disk, which is what
    // PinWright.core.error_codes.AllEmittedCodesAreRegistered enforces globally. Asserted
    // here too so a guard emitting an unregistered code fails in the file that owns it.
    const FString HeaderPath = ActorFindByNameSubsystemGuardTestLocal::ErrorCodesHeaderPath();
    if (!TestFalse(TEXT("resolved Handlers/ErrorCodes.h through IPluginManager"),
            HeaderPath.IsEmpty()))
    {
        return false;
    }

    FString Contents;
    if (!TestTrue(TEXT("read Handlers/ErrorCodes.h from disk"),
            FFileHelper::LoadFileToString(Contents, *HeaderPath)))
    {
        return false;
    }

    const FString Declaration =
        FString::Printf(TEXT("ERR_%s[]"), *ExpectedCode);
    TestTrue(TEXT("ErrorCodes.h declares ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING"),
        Contents.Contains(Declaration));
    TestTrue(TEXT("that declaration carries the exact emitted literal"),
        Contents.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *ExpectedCode)));

    return true;
}
