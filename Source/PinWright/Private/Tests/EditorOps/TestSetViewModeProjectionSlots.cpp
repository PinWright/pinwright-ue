// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestSetViewModeProjectionSlots.cpp - editor.set_view_mode against the viewport client's TWO
// view-mode slots.
//
// The defect these guard: FEditorViewportClient keeps PerspViewModeIndex and OrthoViewModeIndex
// separately (UE 5.8 Editor/UnrealEd/Public/EditorViewportClient.h:2302, :2305) and
// SetViewMode writes only the one matching the current projection
// (EditorViewportClient.cpp:6460-6485). A call made while the viewport was perspective returned
// {"success":true,"viewMode":"Lit"} and left every ORTHOGRAPHIC capture rendering the previous
// mode - which is exactly the reference-tile workflow, so placement could be compared and
// material, lighting and colour could not.
//
// Every assertion below is written in the failure direction: each one breaks if the verb goes
// back to writing one slot while reporting as though it had covered both. The slots are read
// straight off the client, never out of the response, so a handler that reports a write it did
// not perform cannot pass.
//
// Helper names carry a ViewSlotTest prefix and live in one file-scope anonymous namespace, per
// the Unity-build rule in CLAUDE.md.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/EngineBaseTypes.h" // EViewModeIndex / VMI_*
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

namespace
{
    // The active level-editor viewport client, or null when this run has no viewport (headless
    // automation). Same resolution SetViewModeExecTest uses, so the two skip under the same
    // conditions.
    FEditorViewportClient* ViewSlotTestClient()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        FLevelEditorModule& LevelEditorModule =
            FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
        TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            return nullptr;
        }
        return &ActiveViewport->GetAssetViewportClient();
    }

    TSharedPtr<FJsonObject> ViewSlotTestPayload(const TCHAR* ViewMode, const TCHAR* Projection)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("viewMode"), ViewMode);
        if (Projection)
        {
            Payload->SetStringField(TEXT("projection"), Projection);
        }
        // The collision report scans the whole level and is irrelevant here; these modes are not
        // collision modes so it would not run anyway, but stating it keeps the payload honest
        // about what it is exercising.
        Payload->SetBoolField(TEXT("collisionReport"), false);
        return Payload;
    }

    // Nested string field, or an empty string when any hop is missing. Returned by value so a
    // missing field fails the comparison rather than silently skipping the assertion.
    FString ViewSlotTestNested(const TSharedPtr<FJsonObject>& Result, const TCHAR* Object,
                               const TCHAR* Field)
    {
        if (!Result.IsValid())
        {
            return FString();
        }
        const TSharedPtr<FJsonObject>* Inner = nullptr;
        if (!Result->TryGetObjectField(Object, Inner) || !Inner || !Inner->IsValid())
        {
            return FString();
        }
        FString Value;
        (*Inner)->TryGetStringField(Field, Value);
        return Value;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetViewModeWritesBothProjectionSlotsTest,
    "PinWright.editor.set_view_mode.WritesBothProjectionSlots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetViewModeWritesBothProjectionSlotsTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ViewSlotTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so neither view-mode slot is readable."));
        return true;
    }

    const EViewModeIndex PrevPersp = Client->GetPerspViewMode();
    const EViewModeIndex PrevOrtho = Client->GetOrthoViewMode();
    ON_SCOPE_EXIT
    {
        // Global viewport state this test changed, put back on every exit path.
        Client->SetViewModes(PrevPersp, PrevOrtho);
    };

    // Drive both slots somewhere known first, so "Unlit landed in both" cannot be satisfied by a
    // slot that happened to already hold Unlit.
    Client->SetViewModes(VMI_Lit, VMI_Lit);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_view_mode handler registered"),
        InvokeHandlerWithCapture(TEXT("editor.set_view_mode"),
            ViewSlotTestPayload(TEXT("Unlit"), nullptr), Capture));
    TestTrue(TEXT("set_view_mode reports success"), Capture.bSuccess);

    // The load-bearing pair. Reverting the handler to a single SetViewMode leaves one of these
    // at VMI_Lit whichever projection the viewport happens to be in, so this fails either way.
    TestEqual(TEXT("perspective slot holds the requested mode"),
        static_cast<int32>(Client->GetPerspViewMode()), static_cast<int32>(VMI_Unlit));
    TestEqual(TEXT("orthographic slot holds the requested mode"),
        static_cast<int32>(Client->GetOrthoViewMode()), static_cast<int32>(VMI_Unlit));

    // ...and the response must describe that, per slot, rather than emitting one flat mode name.
    TestEqual(TEXT("applied.perspective reports the measured perspective slot"),
        ViewSlotTestNested(Capture.Result, TEXT("applied"), TEXT("perspective")),
        FString(TEXT("Unlit")));
    TestEqual(TEXT("applied.orthographic reports the measured orthographic slot"),
        ViewSlotTestNested(Capture.Result, TEXT("applied"), TEXT("orthographic")),
        FString(TEXT("Unlit")));

    // previous is what makes the change restorable by a caller; it must be the pre-call value,
    // not the new one.
    TestEqual(TEXT("previous.perspective reports the pre-call mode"),
        ViewSlotTestNested(Capture.Result, TEXT("previous"), TEXT("perspective")),
        FString(TEXT("Lit")));
    TestEqual(TEXT("previous.orthographic reports the pre-call mode"),
        ViewSlotTestNested(Capture.Result, TEXT("previous"), TEXT("orthographic")),
        FString(TEXT("Lit")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetViewModeActiveOnlyDoesNotClaimOtherProjectionTest,
    "PinWright.editor.set_view_mode.ActiveOnlyDoesNotClaimOtherProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetViewModeActiveOnlyDoesNotClaimOtherProjectionTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ViewSlotTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so neither view-mode slot is readable."));
        return true;
    }

    const EViewModeIndex PrevPersp = Client->GetPerspViewMode();
    const EViewModeIndex PrevOrtho = Client->GetOrthoViewMode();
    ON_SCOPE_EXIT
    {
        Client->SetViewModes(PrevPersp, PrevOrtho);
    };

    Client->SetViewModes(VMI_Lit, VMI_Lit);
    const bool bActiveIsOrtho = Client->IsOrtho();

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_view_mode handler registered"),
        InvokeHandlerWithCapture(TEXT("editor.set_view_mode"),
            ViewSlotTestPayload(TEXT("Unlit"), TEXT("active")), Capture));
    TestTrue(TEXT("set_view_mode reports success"), Capture.bSuccess);

    // This is the whole point of the fix expressed as an assertion: writing one slot is allowed,
    // reporting the OTHER slot as though it had been written is not. The untouched slot must
    // still measure Lit, and the response must say Lit for it.
    const EViewModeIndex UntouchedMeasured =
        bActiveIsOrtho ? Client->GetPerspViewMode() : Client->GetOrthoViewMode();
    TestEqual(TEXT("the projection that was not written still holds its old mode"),
        static_cast<int32>(UntouchedMeasured), static_cast<int32>(VMI_Lit));

    const FString UntouchedField = bActiveIsOrtho ? TEXT("perspective") : TEXT("orthographic");
    TestEqual(TEXT("the response reports the untouched projection as Lit, not as the request"),
        ViewSlotTestNested(Capture.Result, TEXT("applied"), *UntouchedField),
        FString(TEXT("Lit")));

    const FString WrittenField = bActiveIsOrtho ? TEXT("orthographic") : TEXT("perspective");
    TestEqual(TEXT("the response reports the written projection as Unlit"),
        ViewSlotTestNested(Capture.Result, TEXT("applied"), *WrittenField),
        FString(TEXT("Unlit")));

    // activeProjection names which slot `viewMode` refers to, so a caller reading the flat field
    // knows what it is a statement about.
    TestEqual(TEXT("applied.activeProjection names the rendering projection"),
        ViewSlotTestNested(Capture.Result, TEXT("applied"), TEXT("activeProjection")),
        FString(bActiveIsOrtho ? TEXT("orthographic") : TEXT("perspective")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetViewModeRejectsUnknownProjectionTest,
    "PinWright.editor.set_view_mode.RejectsUnknownProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetViewModeRejectsUnknownProjectionTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = ViewSlotTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so neither view-mode slot is readable."));
        return true;
    }

    const EViewModeIndex PrevPersp = Client->GetPerspViewMode();
    const EViewModeIndex PrevOrtho = Client->GetOrthoViewMode();
    ON_SCOPE_EXIT
    {
        Client->SetViewModes(PrevPersp, PrevOrtho);
    };

    Client->SetViewModes(VMI_Lit, VMI_Lit);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_view_mode handler registered"),
        InvokeHandlerWithCapture(TEXT("editor.set_view_mode"),
            ViewSlotTestPayload(TEXT("Unlit"), TEXT("sideways")), Capture));

    // An unrecognised value must error rather than degrade into a default - the rule that keeps
    // a typo from quietly becoming "active slot only", which is the behaviour being fixed.
    TestFalse(TEXT("an unknown projection is rejected"), Capture.bSuccess);
    TestEqual(TEXT("rejection carries INVALID_ARGUMENT"), Capture.ErrorCode,
        FString(TEXT("INVALID_ARGUMENT")));

    // And nothing may have been written on the way to the rejection.
    TestEqual(TEXT("perspective slot untouched by a rejected call"),
        static_cast<int32>(Client->GetPerspViewMode()), static_cast<int32>(VMI_Lit));
    TestEqual(TEXT("orthographic slot untouched by a rejected call"),
        static_cast<int32>(Client->GetOrthoViewMode()), static_cast<int32>(VMI_Lit));

    return true;
}
