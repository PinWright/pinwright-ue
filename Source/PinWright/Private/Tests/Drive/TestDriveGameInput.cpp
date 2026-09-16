// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FDriveGameInput — the one delivery path for input aimed at a running PIE game —
// and for the editor.simulate_input key contract built on it
// (board B-simulate-input-key-events-never-reach-pie-pawn).
//
// What is covered WITHOUT a PIE session:
//   - ChooseTargetIndex, the pure default-destination rule (which PIE window receives a key).
//   - ResolveTargetIndex, explicit selector errors and the selected context/device identity.
//   - IsWidgetInFocusPath, the focus-path containment used to decide whether the game viewport
//     already holds keyboard focus, exercised against a real in-code Slate hierarchy.
//   - The verb's response contract: an explicit game target with no PIE errors PIE_NOT_ACTIVE
//     instead of the old hardcoded {success:true}, a bad key/target is refused by code, and the
//     editor destination reports route / handled / focusedWidget rather than asserting success.
//
// PiePlayerInputDelivery owns a real PIE session and proves the registered handler reaches
// that session's viewport and UPlayerInput on the following game tick.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Handlers/Drive/DriveGameInput.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Handlers/ErrorCodes.h"
#include "Input/Events.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utils/PieState.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"

namespace DriveGameInputTestHelpers
{
    class SKeyReceiver : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SKeyReceiver) {}
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs)
        {
            ChildSlot[SNullWidget::NullWidget];
        }

        virtual bool SupportsKeyboardFocus() const override { return true; }

        virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent) override
        {
            ++DownCount;
            LastKey = KeyEvent.GetKey();
            LastInputDevice = KeyEvent.GetInputDeviceId();
            LastUserIndex = KeyEvent.GetUserIndex();
            return FReply::Handled();
        }

        virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent) override
        {
            ++UpCount;
            LastKey = KeyEvent.GetKey();
            LastInputDevice = KeyEvent.GetInputDeviceId();
            LastUserIndex = KeyEvent.GetUserIndex();
            return FReply::Handled();
        }

        int32 DownCount = 0;
        int32 UpCount = 0;
        FKey LastKey;
        FInputDeviceId LastInputDevice = INPUTDEVICEID_NONE;
        uint32 LastUserIndex = 0;
    };

    struct FOwnedSlateFocusFixture
    {
        explicit FOwnedSlateFocusFixture(FSlateApplication& InSlateApp)
            : SlateApp(InSlateApp)
            , UserIndex(static_cast<uint32>(SlateApp.GetUserIndexForKeyboard()))
            , PreviousFocus(SlateApp.GetUserFocusedWidget(UserIndex))
        {
            Receiver = SNew(SKeyReceiver);
            Window = SNew(SWindow)
                .ClientSize(FVector2D(100.0f, 80.0f))
                .FocusWhenFirstShown(false)
                .CreateTitleBar(false);
            Window->SetContent(Receiver.ToSharedRef());
            SlateApp.AddWindow(Window.ToSharedRef(), /*bShowImmediately=*/true);
            SlateApp.Tick(ESlateTickType::All);
        }

        ~FOwnedSlateFocusFixture()
        {
            if (PreviousFocus.IsValid())
            {
                SlateApp.SetUserFocus(UserIndex, PreviousFocus, EFocusCause::SetDirectly);
            }
            else
            {
                SlateApp.ClearUserFocus(UserIndex, EFocusCause::SetDirectly);
            }
            SlateApp.RequestDestroyWindow(Window.ToSharedRef());
        }

        bool FocusReceiver()
        {
            return SlateApp.SetUserFocus(UserIndex, Receiver, EFocusCause::SetDirectly);
        }

        FSlateApplication& SlateApp;
        uint32 UserIndex;
        TSharedPtr<SWidget> PreviousFocus;
        TSharedPtr<SKeyReceiver> Receiver;
        TSharedPtr<SWindow> Window;
    };

    FDriveGameInputCandidate MakeCandidate(bool bHasViewportClient, bool bHasPlayerController, bool bIsCurrent)
    {
        FDriveGameInputCandidate Candidate;
        Candidate.bHasViewportClient = bHasViewportClient;
        Candidate.bHasPlayerController = bHasPlayerController;
        Candidate.bIsCurrentViewport = bIsCurrent;
        return Candidate;
    }

    PieWorldSelector::FPieContextInfo MakeContext(int32 PieInstance, ENetMode NetMode)
    {
        PieWorldSelector::FPieContextInfo Context;
        Context.PieInstance = PieInstance;
        Context.NetMode = NetMode;
        Context.World = nullptr;
        return Context;
    }

    // True when PIE is running, which makes the no-PIE response assertions inapplicable.
    bool IsPieRunning()
    {
        return PinWrightPieState::IsPlayInEditorActive();
    }

    bool FindSchemaLessStateTree(FString& OutStateTreePath)
    {
        static const FTopLevelAssetPath StateTreeClassPath(
            TEXT("/Script/StateTreeModule"), TEXT("StateTree"));
        static const FName SchemaTag(TEXT("Schema"));

        if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
        {
            TArray<FAssetData> StateTreeAssets;
            AssetRegistry->GetAssetsByClass(
                StateTreeClassPath, StateTreeAssets, /*bSearchSubClasses=*/true);
            for (const FAssetData& Asset : StateTreeAssets)
            {
                FString SchemaClassPath;
                if (!Asset.GetTagValue(SchemaTag, SchemaClassPath) || SchemaClassPath.IsEmpty())
                {
                    OutStateTreePath = Asset.GetSoftObjectPath().ToString();
                    return true;
                }
            }
        }

        UClass* StateTreeClass = FindObject<UClass>(
            nullptr, TEXT("/Script/StateTreeModule.StateTree"));
        if (!StateTreeClass)
        {
            return false;
        }

        const FObjectProperty* EditorDataProperty =
            FindFProperty<FObjectProperty>(StateTreeClass, TEXT("EditorData"));
        if (!EditorDataProperty)
        {
            return false;
        }

        TArray<UObject*> LoadedStateTrees;
        GetObjectsOfClass(StateTreeClass, LoadedStateTrees);
        for (UObject* StateTree : LoadedStateTrees)
        {
            if (!IsValid(StateTree))
            {
                continue;
            }

            UObject* EditorData =
                EditorDataProperty->GetObjectPropertyValue_InContainer(StateTree);
            const FObjectProperty* SchemaProperty = EditorData
                ? FindFProperty<FObjectProperty>(EditorData->GetClass(), TEXT("Schema"))
                : nullptr;
            if (!SchemaProperty || !SchemaProperty->GetObjectPropertyValue_InContainer(EditorData))
            {
                OutStateTreePath = StateTree->GetPathName();
                return true;
            }
        }
        return false;
    }

    class FConsumeF9InputProcessor : public IInputProcessor
    {
    public:
        virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp,
            TSharedRef<ICursor> Cursor) override
        {
        }

        virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
        {
            return Event.GetKey() == EKeys::F9;
        }
        virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
        {
            return Event.GetKey() == EKeys::F9;
        }
    };

    struct FPieDeliveryState
    {
        TSharedPtr<FConsumeF9InputProcessor> Consumer;
        TSharedPtr<FTestResponseCapture> DownCapture;
        TSharedPtr<FTestResponseCapture> UpCapture;
        double Deadline = 0.0;
        int32 Phase = 0;
        bool bConsumerRegistered = false;
        double CleanupDeadline = 0.0;
    };

    FRpcHandlerFunc FindSimulateInputHandler()
    {
        for (const FHandlerRegistration& Registration : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Registration.MethodName == TEXT("editor.simulate_input"))
            {
                return Registration.Func;
            }
        }
        return nullptr;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunOwnedPieInputDelivery,
    FAutomationTestBase*, Test, TSharedRef<DriveGameInputTestHelpers::FPieDeliveryState>, State);

bool FRunOwnedPieInputDelivery::Update()
{
    using namespace DriveGameInputTestHelpers;
    if (!GEditor || !GEditor->PlayWorld)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_input_unavailable"),
            TEXT("The owned PIE session did not create PlayWorld and a local UPlayerInput."));
        return true;
    }

    FDriveGameInputTarget Target;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FDriveGameInput::ResolveTarget(TEXT(""), Target, ErrorCode, ErrorMessage)
        || !Target.ViewportClient || !Target.PlayerController || !Target.PlayerController->PlayerInput)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_input_unavailable"),
            TEXT("The owned PIE session has no resolvable viewport, controller, or UPlayerInput."));
        return true;
    }

    const FRpcHandlerFunc Handler = FindSimulateInputHandler();
    Test->TestTrue(TEXT("production editor.simulate_input handler is registered"), Handler != nullptr);
    if (!Handler)
    {
        return true;
    }

    if (State->Phase == 0)
    {
        State->DownCapture = MakeShared<FTestResponseCapture>();
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("type"), TEXT("key_down"));
        Payload->SetStringField(TEXT("key"), TEXT("F9"));
        Payload->SetStringField(TEXT("target"), TEXT("game"));
        FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
            TEXT("pie-input-down"), TEXT("editor.simulate_input"), Payload, State->DownCapture.ToSharedRef());
        Handler(Ctx);
        State->Phase = 1;
        return false;
    }
    if (State->Phase == 1 && State->DownCapture->CallCount == 0)
    {
        if (FPlatformTime::Seconds() >= State->Deadline)
        {
            Test->AddError(TEXT("Timed out waiting for the key_down response."));
            return true;
        }
        return false;
    }
    if (State->Phase == 1)
    {
        Test->TestEqual(TEXT("key_down answers exactly once"), State->DownCapture->CallCount, 1);
        Test->TestTrue(TEXT("key_down is delivered to game"), State->DownCapture->bSuccess);
        bool bDelivered = false;
        FString Route;
        Test->TestTrue(TEXT("key_down reports authoritative delivery"),
            State->DownCapture->Result.IsValid()
            && State->DownCapture->Result->TryGetBoolField(TEXT("deliveredToGame"), bDelivered)
            && bDelivered);
        Test->TestTrue(TEXT("key_down names player_input as the consuming route"),
            State->DownCapture->Result.IsValid()
            && State->DownCapture->Result->TryGetStringField(TEXT("consumingRoute"), Route)
            && Route == TEXT("player_input"));
        Test->TestTrue(TEXT("selected UPlayerInput observes the F9 press"),
            Target.PlayerController->PlayerInput->WasJustPressed(EKeys::F9)
            && Target.PlayerController->PlayerInput->IsPressed(EKeys::F9));

        State->UpCapture = MakeShared<FTestResponseCapture>();
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("type"), TEXT("key_up"));
        Payload->SetStringField(TEXT("key"), TEXT("F9"));
        Payload->SetStringField(TEXT("target"), TEXT("game"));
        FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
            TEXT("pie-input-up"), TEXT("editor.simulate_input"), Payload, State->UpCapture.ToSharedRef());
        Handler(Ctx);
        State->Phase = 2;
        return false;
    }
    if (State->UpCapture->CallCount == 0)
    {
        if (FPlatformTime::Seconds() >= State->Deadline)
        {
            Test->AddError(TEXT("Timed out waiting for the key_up response."));
            return true;
        }
        return false;
    }

    Test->TestEqual(TEXT("key_up answers exactly once"), State->UpCapture->CallCount, 1);
    Test->TestTrue(TEXT("key_up is delivered to game"), State->UpCapture->bSuccess);
    bool bDelivered = false;
    FString Route;
    Test->TestTrue(TEXT("key_up reports authoritative delivery"),
        State->UpCapture->Result.IsValid()
        && State->UpCapture->Result->TryGetBoolField(TEXT("deliveredToGame"), bDelivered)
        && bDelivered);
    Test->TestTrue(TEXT("key_up names player_input as the consuming route"),
        State->UpCapture->Result.IsValid()
        && State->UpCapture->Result->TryGetStringField(TEXT("consumingRoute"), Route)
        && Route == TEXT("player_input"));
    Test->TestTrue(TEXT("selected UPlayerInput observes the F9 release"),
        Target.PlayerController->PlayerInput->WasJustReleased(EKeys::F9)
        && !Target.PlayerController->PlayerInput->IsPressed(EKeys::F9));
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupOwnedPieInputDelivery,
    FAutomationTestBase*, Test, TSharedRef<DriveGameInputTestHelpers::FPieDeliveryState>, State);

bool FCleanupOwnedPieInputDelivery::Update()
{
    if (State->bConsumerRegistered && FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().UnregisterInputPreProcessor(State->Consumer);
        State->bConsumerRegistered = false;
    }
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputPiePlayerInputDeliveryTest,
    "PinWright.editor.simulate_input.PiePlayerInputDelivery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputPiePlayerInputDeliveryTest::RunTest(const FString& Parameters)
{
    using namespace DriveGameInputTestHelpers;
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_unavailable"), TEXT("Owned PIE requires GEditor."));
        return true;
    }
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_unavailable"), TEXT("Owned PIE requires Slate."));
        return true;
    }
    if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The test never borrows or stops ambient PIE."));
        return true;
    }
    if (!GEditor->GetActiveViewport())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("level_viewport_unavailable"),
            TEXT("Standalone PIE requires an active level viewport."));
        return true;
    }
    FString SchemaLessStateTreePath;
    if (FindSchemaLessStateTree(SchemaLessStateTreePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-start-triggers-engine-ensure"),
            FString::Printf(TEXT("pie-start-triggers-engine-ensure: StateTree '%s' has no editor data/schema."),
                *SchemaLessStateTreePath));
        return true;
    }
    TestTrue(TEXT("production editor.simulate_input handler is registered"), FindSimulateInputHandler() != nullptr);
    if (!FindSimulateInputHandler())
    {
        return false;
    }

    const TSharedRef<FPieDeliveryState> State = MakeShared<FPieDeliveryState>();
    State->Consumer = MakeShared<FConsumeF9InputProcessor>();
    State->bConsumerRegistered = FSlateApplication::Get().RegisterInputPreProcessor(State->Consumer, 0);
    TestTrue(TEXT("negative Slate preprocessor fixture is registered"), State->bConsumerRegistered);
    if (!State->bConsumerRegistered)
    {
        return false;
    }
    State->Deadline = FPlatformTime::Seconds() + 15.0;
    State->CleanupDeadline = State->Deadline + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FRunOwnedPieInputDelivery(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupOwnedPieInputDelivery(this, State));
    return true;
}

// ============================================================================
// ChooseTargetIndex — the pure default-destination rule.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveGameInputChooseTargetTest,
    "PinWright.drive.game_input.ChooseTargetPriority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveGameInputChooseTargetTest::RunTest(const FString& Parameters)
{
    using namespace DriveGameInputTestHelpers;

    // Nothing to receive input.
    TestEqual(TEXT("no candidates -> INDEX_NONE"),
        FDriveGameInput::ChooseTargetIndex({}), INDEX_NONE);
    TestEqual(TEXT("a context with no game viewport is never chosen"),
        FDriveGameInput::ChooseTargetIndex({ MakeCandidate(false, true, true) }), INDEX_NONE);

    // The engine's current viewport wins when it can actually receive gameplay input.
    TestEqual(TEXT("current viewport with a player controller wins"),
        FDriveGameInput::ChooseTargetIndex({
            MakeCandidate(true, true, false),
            MakeCandidate(true, true, true) }), 1);

    // A window with a player beats one that is merely current but has none (a dedicated-server
    // PIE window has no player to drive).
    TestEqual(TEXT("player controller beats a current viewport without one"),
        FDriveGameInput::ChooseTargetIndex({
            MakeCandidate(true, false, true),
            MakeCandidate(true, true, false) }), 1);

    // No player anywhere: fall back to the current viewport, then to the first viewport.
    TestEqual(TEXT("current viewport when no context has a player controller"),
        FDriveGameInput::ChooseTargetIndex({
            MakeCandidate(true, false, false),
            MakeCandidate(true, false, true) }), 1);
    TestEqual(TEXT("first context with a viewport when nothing else applies"),
        FDriveGameInput::ChooseTargetIndex({
            MakeCandidate(false, false, false),
            MakeCandidate(true, false, false),
            MakeCandidate(true, false, false) }), 1);

    return true;
}

// ============================================================================
// ResolveTargetIndex — explicit PIE route selection and identity preservation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveGameInputGameRouteSelectionTest,
    "PinWright.drive.game_input.GameRouteSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveGameInputGameRouteSelectionTest::RunTest(const FString& Parameters)
{
    using namespace DriveGameInputTestHelpers;

    FDriveGameInputSelectionCandidate Server;
    Server.Context = MakeContext(0, NM_ListenServer);
    Server.Priority = MakeCandidate(true, true, false);
    Server.Target.InputDevice = FInputDeviceId::CreateFromInternalId(7);

    FDriveGameInputSelectionCandidate Client;
    Client.Context = MakeContext(1, NM_Client);
    Client.Priority = MakeCandidate(true, true, true);
    Client.Target.World = GetMutableDefault<UWorld>();
    Client.Target.ViewportClient = GetMutableDefault<UGameViewportClient>();
    Client.Target.LocalPlayer = GetMutableDefault<ULocalPlayer>();
    Client.Target.PlayerController = GetMutableDefault<APlayerController>();
    Client.Target.InputDevice = FInputDeviceId::CreateFromInternalId(42);
    const TArray<FDriveGameInputSelectionCandidate> Candidates = { Server, Client };

    int32 ChosenIndex = INDEX_NONE;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("client:1 selects a matching PIE target"),
        FDriveGameInput::ResolveTargetIndex(TEXT("client:1"), Candidates,
            ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("client:1 selects the first client context"), ChosenIndex, 1);
    if (Candidates.IsValidIndex(ChosenIndex))
    {
        const FDriveGameInputTarget& Selected = Candidates[ChosenIndex].Target;
        TestEqual(TEXT("selected viewport stays paired"), Selected.ViewportClient, Client.Target.ViewportClient);
        TestEqual(TEXT("selected local player stays paired"), Selected.LocalPlayer, Client.Target.LocalPlayer);
        TestEqual(TEXT("selected controller stays paired"), Selected.PlayerController, Client.Target.PlayerController);
        TestEqual(TEXT("selected device stays paired"), Selected.InputDevice.GetId(), 42);
    }

    TestTrue(TEXT("default selection uses the precomputed current viewport candidate"),
        FDriveGameInput::ResolveTargetIndex(TEXT(""), Candidates,
            ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("the current viewport candidate is selected"), ChosenIndex, 1);

    ErrorCode.Reset();
    ErrorMessage.Reset();
    TestFalse(TEXT("an unmatched explicit PIE world is refused"),
        FDriveGameInput::ResolveTargetIndex(TEXT("client:2"), Candidates,
            ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("an unmatched explicit world returns WORLD_NOT_FOUND"),
        ErrorCode, FString(ErrorCodes::ERR_WORLD_NOT_FOUND));

    ErrorCode.Reset();
    ErrorMessage.Reset();
    TestFalse(TEXT("a malformed PIE selector is refused"),
        FDriveGameInput::ResolveTargetIndex(TEXT("client:not-a-number"), Candidates,
            ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("a malformed selector returns INVALID_ARGUMENT"),
        ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    TArray<FDriveGameInputSelectionCandidate> NoPieCandidates;
    ErrorCode.Reset();
    ErrorMessage.Reset();
    TestFalse(TEXT("an empty target set cannot receive game input"),
        FDriveGameInput::ResolveTargetIndex(TEXT("server"), NoPieCandidates,
            ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("an empty target set returns PIE_NOT_ACTIVE"),
        ErrorCode, FString(ErrorCodes::ERR_PIE_NOT_ACTIVE));

    TestFalse(TEXT("a malformed selector wins over the missing-session response"),
        FDriveGameInput::ResolveTargetIndex(TEXT("client:not-a-number"),
            NoPieCandidates, ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("malformed selector without PIE is still INVALID_ARGUMENT"),
        ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    TestFalse(TEXT("world editor is rejected before the missing-session response"),
        FDriveGameInput::ResolveTargetIndex(TEXT("editor"), NoPieCandidates,
            ChosenIndex, ErrorCode, ErrorMessage));
    TestEqual(TEXT("world editor without PIE is INVALID_ARGUMENT"),
        ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    // Counterfactual: selecting metadata and receiver arrays independently could pair client:1
    // with Server's null receiver or device 7; the identity assertions above would fail.
    return true;
}

// ============================================================================
// IsWidgetInFocusPath — containment against a real Slate hierarchy. This is what
// decides whether the game viewport already holds keyboard focus (and therefore
// whether focus has to be moved onto it before the key is dispatched).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveGameInputFocusPathTest,
    "PinWright.drive.game_input.FocusPathContainment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveGameInputFocusPathTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; the widget-hierarchy fixture cannot be built."));
        return true;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();

    TSharedRef<SButton> Button = SNew(SButton);
    TSharedRef<SBorder> Border = SNew(SBorder)[Button];
    TSharedRef<SButton> Unrelated = SNew(SButton);

    TSharedRef<SWindow> Window = SNew(SWindow)
        .ScreenPosition(FVector2D(160.0f, 160.0f))
        .ClientSize(FVector2D(200.0f, 120.0f))
        .FocusWhenFirstShown(false)
        .CreateTitleBar(false)
        .SupportsMaximize(false)
        .SupportsMinimize(false);
    Window->SetContent(Border);
    SlateApp.AddWindow(Window, /*bShowImmediately=*/true);
    ON_SCOPE_EXIT { SlateApp.RequestDestroyWindow(Window); };
    SlateApp.Tick(ESlateTickType::All);

    TestTrue(TEXT("a widget is on its own focus path"),
        FDriveGameInput::IsWidgetInFocusPath(Button, Button));
    TestTrue(TEXT("a parent is on the focus path of its child"),
        FDriveGameInput::IsWidgetInFocusPath(Button, Border));
    TestTrue(TEXT("an ancestor several levels up is on the focus path"),
        FDriveGameInput::IsWidgetInFocusPath(Button, Window));
    TestFalse(TEXT("an unrelated widget is not on the focus path"),
        FDriveGameInput::IsWidgetInFocusPath(Button, Unrelated));
    TestFalse(TEXT("a child is not an ancestor of its parent"),
        FDriveGameInput::IsWidgetInFocusPath(Border, Button));
    TestFalse(TEXT("an invalid focus widget is never contained"),
        FDriveGameInput::IsWidgetInFocusPath(nullptr, Border));
    TestFalse(TEXT("an invalid ancestor never contains"),
        FDriveGameInput::IsWidgetInFocusPath(Button, nullptr));

    return true;
}

// ============================================================================
// editor.simulate_input — the key contract.
//
// Regression for B-simulate-input-key-events-never-reach-pie-pawn: the reverted
// branch dispatched a bare FSlateApplication::ProcessKeyDownEvent along the EDITOR
// focus path, discarded the handled bool and hardcoded bSuccess = true, so a key
// aimed at a game answered {success:true} while nothing in the game observed it.
// Asking for the game explicitly must now fail loudly when there is no game.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputKeyGameTargetWithoutPieTest,
    "PinWright.editor.simulate_input.KeyGameTargetWithoutPieErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputKeyGameTargetWithoutPieTest::RunTest(const FString& Parameters)
{
    if (DriveGameInputTestHelpers::IsPieRunning())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie_session_active"),
            TEXT("A PIE session is live, so target:\"game\" resolves a real destination and the ")
            TEXT("no-session refusal cannot be asserted."));
        return true;
    }

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("type"), TEXT("key_down"));
    Payload->SetStringField(TEXT("key"), TEXT("W"));
    Payload->SetStringField(TEXT("target"), TEXT("game"));
    TestTrue(TEXT("editor.simulate_input handler found"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), Payload, Capture));

    TestFalse(TEXT("a key aimed at a game that is not running is not a success"), Capture.bSuccess);
    TestEqual(TEXT("refusal names inactive PIE"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_PIE_NOT_ACTIVE));

    // Same refusal when the caller names a PIE world explicitly: 'auto' may fall back to the
    // editor, but not once a world selector says the game was meant.
    FTestResponseCapture WorldCapture;
    TSharedPtr<FJsonObject> WorldPayload = MakeShared<FJsonObject>();
    WorldPayload->SetStringField(TEXT("type"), TEXT("key_down"));
    WorldPayload->SetStringField(TEXT("key"), TEXT("W"));
    WorldPayload->SetStringField(TEXT("world"), TEXT("server"));
    TestTrue(TEXT("editor.simulate_input handler found (world selector)"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), WorldPayload, WorldCapture));
    TestFalse(TEXT("a named PIE world with no session is not a success"), WorldCapture.bSuccess);
    TestEqual(TEXT("world selector refusal names inactive PIE"),
        WorldCapture.ErrorCode, FString(ErrorCodes::ERR_PIE_NOT_ACTIVE));

    FTestResponseCapture MalformedWorldCapture;
    TSharedPtr<FJsonObject> MalformedWorldPayload = MakeShared<FJsonObject>();
    MalformedWorldPayload->SetStringField(TEXT("type"), TEXT("key_down"));
    MalformedWorldPayload->SetStringField(TEXT("key"), TEXT("W"));
    MalformedWorldPayload->SetStringField(TEXT("world"), TEXT("client:not-a-number"));
    TestTrue(TEXT("editor.simulate_input handler found (malformed world selector)"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), MalformedWorldPayload,
            MalformedWorldCapture));
    TestFalse(TEXT("a malformed world selector is not a success"),
        MalformedWorldCapture.bSuccess);
    TestEqual(TEXT("malformed selector is validated before the missing session"),
        MalformedWorldCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    FTestResponseCapture EditorWorldCapture;
    TSharedPtr<FJsonObject> EditorWorldPayload = MakeShared<FJsonObject>();
    EditorWorldPayload->SetStringField(TEXT("type"), TEXT("key_down"));
    EditorWorldPayload->SetStringField(TEXT("key"), TEXT("W"));
    EditorWorldPayload->SetStringField(TEXT("world"), TEXT("editor"));
    InvokeHandlerWithCapture(TEXT("editor.simulate_input"), EditorWorldPayload, EditorWorldCapture);
    TestEqual(TEXT("world editor is invalid even without PIE"),
        EditorWorldCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    return true;
}

// A bad key or destination is refused by code, not by a message inside a success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputKeyArgumentRefusalTest,
    "PinWright.editor.simulate_input.KeyArgumentsRefusedByCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputKeyArgumentRefusalTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture MissingKey;
    TSharedPtr<FJsonObject> MissingKeyPayload = MakeShared<FJsonObject>();
    MissingKeyPayload->SetStringField(TEXT("type"), TEXT("key_down"));
    TestTrue(TEXT("editor.simulate_input handler found (missing key)"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), MissingKeyPayload, MissingKey));
    TestFalse(TEXT("a key event with no key is not a success"), MissingKey.bSuccess);
    TestEqual(TEXT("missing key is INVALID_ARGUMENT"), MissingKey.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    FTestResponseCapture BadKey;
    TSharedPtr<FJsonObject> BadKeyPayload = MakeShared<FJsonObject>();
    BadKeyPayload->SetStringField(TEXT("type"), TEXT("key_down"));
    BadKeyPayload->SetStringField(TEXT("key"), TEXT("NotAKeyAtAll"));
    TestTrue(TEXT("editor.simulate_input handler found (bad key)"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), BadKeyPayload, BadKey));
    TestFalse(TEXT("an unknown FKey name is not a success"), BadKey.bSuccess);
    TestEqual(TEXT("unknown key is INVALID_KEY"), BadKey.ErrorCode, FString(TEXT("INVALID_KEY")));

    FTestResponseCapture BadTarget;
    TSharedPtr<FJsonObject> BadTargetPayload = MakeShared<FJsonObject>();
    BadTargetPayload->SetStringField(TEXT("type"), TEXT("key_down"));
    BadTargetPayload->SetStringField(TEXT("key"), TEXT("A"));
    BadTargetPayload->SetStringField(TEXT("target"), TEXT("pawn"));
    TestTrue(TEXT("editor.simulate_input handler found (bad target)"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), BadTargetPayload, BadTarget));
    TestFalse(TEXT("an unknown target token is not a success"), BadTarget.bSuccess);
    TestEqual(TEXT("unknown target is INVALID_ARGUMENT"), BadTarget.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));

    return true;
}

// The editor destination must report what actually happened: which route carried the event and
// whether anything consumed it. `handled` existing at all is the contract the old branch broke.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSimulateInputKeyEditorRouteShapeTest,
    "PinWright.editor.simulate_input.KeyEditorRouteReportsHandled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSimulateInputKeyEditorRouteShapeTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("slate_not_initialized"),
            TEXT("FSlateApplication is not initialized; no key can be injected to describe."));
        return true;
    }

    FSlateApplication& SlateApp = FSlateApplication::Get();
    DriveGameInputTestHelpers::FOwnedSlateFocusFixture Fixture(SlateApp);
    TestTrue(TEXT("the owned editor-route receiver takes keyboard focus"),
        Fixture.FocusReceiver());

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("type"), TEXT("key_down"));
    Payload->SetStringField(TEXT("key"), TEXT("A"));
    Payload->SetStringField(TEXT("target"), TEXT("editor"));
    TestTrue(TEXT("editor.simulate_input handler found"),
        InvokeHandlerWithCapture(TEXT("editor.simulate_input"), Payload, Capture));

    if (!TestTrue(TEXT("editor-targeted key injection returns a result object"), Capture.Result.IsValid()))
    {
        return true;
    }

    FString Target;
    TestTrue(TEXT("response names the destination"), Capture.Result->TryGetStringField(TEXT("target"), Target));
    TestEqual(TEXT("destination is the editor"), Target, FString(TEXT("editor")));

    FString Route;
    TestTrue(TEXT("response names the route"), Capture.Result->TryGetStringField(TEXT("route"), Route));

    bool bHandled = true;
    TestTrue(TEXT("response reports the handled result rather than assuming it"),
        Capture.Result->TryGetBoolField(TEXT("handled"), bHandled));

    FString FocusedWidget;
    TestTrue(TEXT("response names the widget that held keyboard focus"),
        Capture.Result->TryGetStringField(TEXT("focusedWidget"), FocusedWidget));
    TestFalse(TEXT("focused widget is reported, even as 'none'"), FocusedWidget.IsEmpty());
    TestEqual(TEXT("the editor route delivered only to the owned receiver"),
        Fixture.Receiver->DownCount, 1);

    // Balance the press so no key is left stuck down for later tests.
    TSharedPtr<FJsonObject> UpPayload = MakeShared<FJsonObject>();
    UpPayload->SetStringField(TEXT("type"), TEXT("key_up"));
    UpPayload->SetStringField(TEXT("key"), TEXT("A"));
    UpPayload->SetStringField(TEXT("target"), TEXT("editor"));
    InvokeHandler(TEXT("editor.simulate_input"), UpPayload);

    return true;
}
