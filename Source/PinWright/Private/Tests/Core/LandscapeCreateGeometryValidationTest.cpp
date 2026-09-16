// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-landscape-create-inconsistent-subsection-geometry.
//
// landscape.create used to derive the three landscape geometry fields
// independently from the raw inputs:
//   ComponentSizeQuads  = quadsPerComponent
//   SubsectionSizeQuads = quadsPerComponent / sectionsPerComponent  (int division)
//   NumSubsections      = sectionsPerComponent
// With a whole-component-sized quadsPerComponent like 62 and sectionsPerComponent=4
// this silently truncated SubsectionSizeQuads (62/4 = 15) and stored
// NumSubsections=4 (UE supports only 1 or 2), leaving the proxy with
// 62 != 15*4 = 60 — a structurally invalid landscape that violates UE's
// ComponentSizeQuads == SubsectionSizeQuads * NumSubsections invariant (asserted
// in ULandscapeComponent::Init). The handler still reported success:true.
//
// The fix (per the ticket's "interpret sectionsPerComponent as the total section
// count" option) reinterprets quadsPerComponent as the per-subsection
// SubsectionSizeQuads and validates the inputs synchronously, before the
// AsyncTask that spawns the actor, rejecting non-conforming combinations with
// INVALID_ARGUMENT:
//   - quadsPerComponent must be one of {7,15,31,63,127,255} (the SubsectionSizeQuads set)
//   - sectionsPerComponent must be 1 (1x1 grid) or 4 (2x2 grid)
// Under this contract quadsPerComponent=31 with sectionsPerComponent=4 is the
// VALID 31-per-subsection 2x2 grid (ComponentSizeQuads=62), so the repro below
// uses a quadsPerComponent that is NOT in the allowed subsection set — the
// whole-component-sized value the old int-division silently accepted. These
// assertions hit that synchronous validation path; they need no editor world,
// actor spawn, or async pump because Ctx.SendError fires inline.
//
// Counterfactual: if the derivation reverts to the independent int-division, the
// repro inputs no longer produce a synchronous INVALID_ARGUMENT (they fall
// through to the async spawn), and the first assertion below fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

// ---- landscape.create rejects geometry that breaks the UE invariant ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGeometryValidationTest,
    "PinWright.landscape.create.GeometryValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGeometryValidationTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));

    // Case 1: the ticket repro shape — a whole-component-sized quadsPerComponent
    // (62, not in the {7,15,31,63,127,255} subsection set) with a 2x2 grid. Before
    // the fix the independent int-division silently truncated SubsectionSizeQuads
    // to 15 and stored NumSubsections=4, returning success:true and building a
    // 62/15/4 actor (62 != 15*4 = 60). Under the per-subsection contract 62 is not
    // a valid SubsectionSizeQuads, so the handler must reject it synchronously.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), TEXT("MCP_GeomValidate_Repro"));
        Payload->SetNumberField(TEXT("componentsX"), 4);
        Payload->SetNumberField(TEXT("componentsY"), 4);
        Payload->SetNumberField(TEXT("quadsPerComponent"), 62);
        Payload->SetNumberField(TEXT("sectionsPerComponent"), 4);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("landscape.create"), Payload, Capture);
        TestTrue(TEXT("repro: handler invoked"), bFound);
        TestTrue(TEXT("repro: handler responded synchronously"), Capture.bWasCalled);
        TestFalse(TEXT("repro: 62x4 no longer reports success"), Capture.bSuccess);
        TestEqual(TEXT("repro: rejected with INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // Case 2: quadsPerComponent not in the SubsectionSizeQuads allowed set.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), TEXT("MCP_GeomValidate_BadQuads"));
        Payload->SetNumberField(TEXT("quadsPerComponent"), 30);
        Payload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("landscape.create"), Payload, Capture);
        TestTrue(TEXT("bad-quads: handler responded synchronously"), Capture.bWasCalled);
        TestFalse(TEXT("bad-quads: not reported as success"), Capture.bSuccess);
        TestEqual(TEXT("bad-quads: rejected with INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // Case 3: sectionsPerComponent that maps to no valid subsection grid.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), TEXT("MCP_GeomValidate_BadSections"));
        Payload->SetNumberField(TEXT("quadsPerComponent"), 63);
        Payload->SetNumberField(TEXT("sectionsPerComponent"), 3);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("landscape.create"), Payload, Capture);
        TestTrue(TEXT("bad-sections: handler responded synchronously"), Capture.bWasCalled);
        TestFalse(TEXT("bad-sections: not reported as success"), Capture.bSuccess);
        TestEqual(TEXT("bad-sections: rejected with INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // Case 4 (over-rejection guard): a valid default-shaped combination must NOT
    // be rejected by the synchronous validation. quadsPerComponent=63 is in the
    // allowed set and sectionsPerComponent=1 is a valid 1x1 grid, so the handler
    // falls through to the async spawn path — no synchronous INVALID_ARGUMENT.
    // (A missing editor world later short-circuits the async lambda harmlessly;
    // we only assert that no synchronous validation error was emitted.)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), TEXT("MCP_GeomValidate_Valid"));
        Payload->SetNumberField(TEXT("componentsX"), 1);
        Payload->SetNumberField(TEXT("componentsY"), 1);
        Payload->SetNumberField(TEXT("quadsPerComponent"), 63);
        Payload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("landscape.create"), Payload, Capture);
        // Either nothing was captured synchronously (work deferred to async), or
        // a non-validation error fired (e.g. EDITOR_NOT_AVAILABLE). In no case
        // should a valid combination be rejected as INVALID_ARGUMENT.
        TestNotEqual(TEXT("valid combo not rejected as INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    return true;
}
