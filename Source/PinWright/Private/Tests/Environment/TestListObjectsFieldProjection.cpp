// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for board ticket B-environment-list-fields-silently-dropped.
//
// system.inspect.list_objects read its caller's `fields` set verbatim through
// FHandlerContext::ReadFieldProjection and then probed it for exactly four keys
// (label/name/path/class). Any other entry was accepted, matched nothing, and was never
// mentioned again — while still raising Fields.Num(), which is what switches projection ON.
// Two grades of damage followed:
//
//   1. fields:["name","folder"] returned rows carrying only `name`. Two columns asked for,
//      one delivered, nothing in the response saying so.
//   2. fields:["folder"] returned rows that were EMPTY JSON OBJECTS, alongside count and
//      totalMatches reporting the matched actors — a success-shaped answer indistinguishable
//      from "this actor has no such data".
//
// The fix validates the projection against the set the row builder can actually emit and
// refuses an unknown entry by name with INVALID_PARAMS — the courtesy the dispatcher's
// UNKNOWN_PARAMS gate already extends to top-level parameter names but cannot extend to
// values inside an array. No key was ADDED: `folder` is projectable on the actor.list twin
// and the wider per-actor keys on system.inspect.inspect_object, so widening the rows of a
// verb that already spills on any populated level would buy nothing. The rejection message
// names both siblings so the refusal routes the caller instead of just stopping them.
//
// Counterfactual: before the fix every case below returned bSuccess:true — the all-unknown
// projection with rows that were `{}`, the mixed projection with silently missing columns.
//
// No world fixture: the allow-list is validated before the actor walk, so every assertion
// here holds on any host, with or without an open level.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

namespace ListObjectsFieldProjectionTestUtils
{
    // limit=1 keeps the success cases from building a row for every actor in whatever
    // level the host happens to have open; world=editor pins the resolution so the
    // outcome does not depend on a PIE session left running by a sibling test.
    TSharedPtr<FJsonObject> MakeBasePayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetNumberField(TEXT("limit"), 1);
        return Payload;
    }

    TSharedPtr<FJsonObject> MakeFieldsPayload(const TArray<FString>& Fields)
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& Field : Fields)
        {
            Arr.Add(MakeShared<FJsonValueString>(Field));
        }
        Payload->SetArrayField(TEXT("fields"), Arr);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FListObjectsFieldProjectionUnknownFieldRejectedTest,
    "PinWright.system.inspect.list_objects.FieldProjection.UnknownFieldIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FListObjectsFieldProjectionUnknownFieldRejectedTest::RunTest(const FString& Parameters)
{
    using namespace ListObjectsFieldProjectionTestUtils;

    auto Invoke = [&](const TSharedPtr<FJsonObject>& Payload) -> FTestResponseCapture
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("system.inspect.list_objects handler is registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.list_objects"), Payload, Capture));
        return Capture;
    };

    auto ListWithFields = [&](const TArray<FString>& Fields) -> FTestResponseCapture
    {
        return Invoke(MakeFieldsPayload(Fields));
    };

    // Grade 2 from the ticket: a projection made ENTIRELY of keys the handler cannot emit.
    // This used to return success with `{}` rows for every matched actor.
    {
        FTestResponseCapture Capture = ListWithFields({TEXT("folder")});
        TestTrue(TEXT("an all-unknown projection is answered"), Capture.bWasCalled);
        TestFalse(TEXT("an all-unknown projection is NOT a success"), Capture.bSuccess);
        TestEqual(TEXT("an unknown fields entry is rejected as INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(TEXT("the message names the offending key"),
            Capture.Message.Contains(TEXT("folder")));
        TestTrue(TEXT("the message names the valid set"),
            Capture.Message.Contains(TEXT("Valid fields")));
        for (const TCHAR* Valid : {TEXT("label"), TEXT("name"), TEXT("path"), TEXT("class")})
        {
            TestTrue(*FString::Printf(TEXT("the valid set lists '%s'"), Valid),
                Capture.Message.Contains(Valid));
        }
        // The refusal routes rather than just stopping: `folder` IS projectable, on the
        // sibling verb, and the message has to say so or the caller has nowhere to go.
        TestTrue(TEXT("the message points at the sibling that can answer"),
            Capture.Message.Contains(TEXT("actor.list")));
    }

    // Grade 1: a MIXED projection must fail too. Honouring the recognised half and dropping
    // the rest is what silently answered a two-column question with one column.
    {
        FTestResponseCapture Capture = ListWithFields({TEXT("name"), TEXT("folder")});
        TestFalse(TEXT("a partially-unknown projection is NOT a success"), Capture.bSuccess);
        TestEqual(TEXT("a partially-unknown projection is rejected as INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(TEXT("the message names only the offending key"),
            Capture.Message.Contains(TEXT("[folder]")));
    }

    // A typo is the same defect wearing different clothes: 'clas' matched nothing and was
    // eaten, while the correctly-spelled 'Label' folds to a valid key by case.
    {
        FTestResponseCapture Capture = ListWithFields({TEXT("Label"), TEXT("clas")});
        TestFalse(TEXT("a mistyped fields entry is NOT a success"), Capture.bSuccess);
        TestTrue(TEXT("the message names the typo and nothing else"),
            Capture.Message.Contains(TEXT("[clas]")));
    }

    // ReadFieldProjection has a second entry point — a bare string under `fields` or the
    // singular `field`. It feeds the same set, so it must hit the same gate; validating only
    // the array branch would leave the shorthand silently dropping keys.
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        Payload->SetStringField(TEXT("field"), TEXT("transform"));
        FTestResponseCapture Capture = Invoke(Payload);
        TestFalse(TEXT("the singular 'field' shorthand is validated too"), Capture.bSuccess);
        TestEqual(TEXT("the shorthand is rejected as INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(TEXT("the message names the offending shorthand key"),
            Capture.Message.Contains(TEXT("[transform]")));
    }

    // The gate must not over-reject. Every emittable key, including the mixed case the
    // allow-list folds to lowercase, still succeeds.
    {
        FTestResponseCapture Capture = ListWithFields(
            {TEXT("label"), TEXT("Name"), TEXT("path"), TEXT("class")});
        TestTrue(TEXT("a projection of only emittable keys still succeeds"), Capture.bSuccess);
    }

    // The unprojected call is untouched: no fields, no rejection.
    {
        FTestResponseCapture Capture = Invoke(MakeBasePayload());
        TestTrue(TEXT("an unprojected call still succeeds"), Capture.bSuccess);
    }

    // namesOnly expands to the handler's own {label,name,class} column set inside
    // ReadFieldProjection, so it flows through the same validation. If that set and the
    // emittable set ever drift apart, the shorthand starts rejecting itself — this pins
    // them together.
    {
        TSharedPtr<FJsonObject> Payload = MakeBasePayload();
        Payload->SetBoolField(TEXT("namesOnly"), true);
        FTestResponseCapture Capture = Invoke(Payload);
        TestTrue(TEXT("namesOnly=true is not rejected by the new gate"), Capture.bSuccess);
    }

    return true;
}
