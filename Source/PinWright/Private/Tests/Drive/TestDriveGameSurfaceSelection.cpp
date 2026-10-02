// Copyright (c) 2026 Alexander Penkin. MIT License.

// Game-surface root and PIE-instance selection for drive.*:
//  - E-drive-observe-multi-root-default: with no instance_name / root_index the walk spans every
//    live UMG root of the viewport (z-order, per-element `root`), instead of AMBIGUOUS_LIVE_ROOT.
//  - F-drive-pie-instance-selector: the `world` selector (editor.console_command grammar) picks the
//    PIE instance; omitted, the only instance with a game viewport, TARGET_AMBIGUOUS when several.
// The instance rule is unit-tested over fake instances; a live multi-instance (listen + clients)
// PIE session is not started here, so the live test covers the single-instance path only.
#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveTypes.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Guid.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tests/Bpir/TestWidgetWithStructBIE.h"
#include "Tests/Drive/HostNeutralPie.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/PieState.h"

namespace DriveGameSurfaceTestHelpers
{
    FDrivePieInstance MakeInstance(int32 PieInstance, ENetMode NetMode, bool bHasGameViewport)
    {
        FDrivePieInstance Instance;
        Instance.Context.PieInstance = PieInstance;
        Instance.Context.NetMode = NetMode;
        Instance.bHasGameViewport = bHasGameViewport;
        return Instance;
    }

    // Expect World to select Expected (an index) or fail with ExpectedCode.
    void ExpectPick(FAutomationTestBase& Test, const TCHAR* Case, const TArray<FDrivePieInstance>& Instances,
        const FString& World, int32 Expected, const TCHAR* ExpectedCode = TEXT(""))
    {
        int32 Index = INDEX_NONE;
        FString Code;
        FString Message;
        const bool bOk = FDriveLiveResolver::SelectPieInstance(World, Instances, Index, Code, Message);
        const FString Label = FString::Printf(TEXT("%s: world '%s'"), Case, *World);
        if (Expected != INDEX_NONE)
        {
            Test.TestTrue(*FString::Printf(TEXT("%s resolves (%s %s)"), *Label, *Code, *Message), bOk);
            Test.TestEqual(*FString::Printf(TEXT("%s picks index %d"), *Label, Expected), Index, Expected);
        }
        else
        {
            Test.TestFalse(*FString::Printf(TEXT("%s is refused"), *Label), bOk);
            Test.TestEqual(*FString::Printf(TEXT("%s error code"), *Label), Code, FString(ExpectedCode));
            Test.TestEqual(*FString::Printf(TEXT("%s leaves no index"), *Label), Index, static_cast<int32>(INDEX_NONE));
        }
    }

    struct FState
    {
        TStrongObjectPtr<UUserWidget> Bottom;
        TStrongObjectPtr<UUserWidget> Top;
        FString BottomName;
        FString TopName;
        double Deadline = 0.0;
        uint64 AssertNotBeforeFrame = 0;
        double CleanupDeadline = 0.0;
    };

    // A concrete native UUserWidget (UUserWidget itself is abstract) gets a transient WidgetTree in
    // Initialize(); give it one named button. No Widget Blueprint asset is created.
    UUserWidget* MakeProbe(APlayerController* Owner, const FString& Name)
    {
        UUserWidget* Widget = CreateWidget<UUserWidget>(Owner, UTestWidgetWithStructBIE::StaticClass(), FName(*Name));
        if (Widget && Widget->WidgetTree)
        {
            Widget->WidgetTree->RootWidget =
                Widget->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), FName(TEXT("PWDriveRootProbeButton")));
        }
        return Widget;
    }

    int32 FirstIndexOfRoot(const TArray<FDriveElement>& Elements, const FString& Root)
    {
        return Elements.IndexOfByPredicate([&Root](const FDriveElement& Element) { return Element.Root == Root; });
    }

    const FDriveElement* FindButton(const TArray<FDriveElement>& Elements, const FString& Root)
    {
        return Elements.FindByPredicate([&Root](const FDriveElement& Element)
        {
            return Element.Root == Root && Element.Type.Contains(TEXT("SButton"));
        });
    }

    void AssertTwoRoots(FAutomationTestBase& Test, FState& State)
    {
        // 1) No selector: both roots are walked, nothing is ambiguous.
        TArray<FDriveElement> Elements;
        FString RootName;
        FString Code;
        FString Message;
        const bool bBuilt = FDriveLiveResolver::BuildElementList(FDriveRootSelector{}, Elements, RootName, Code, Message);
        if (!Test.TestTrue(*FString::Printf(TEXT("bare element list over two roots succeeds (%s %s)"), *Code, *Message), bBuilt))
        {
            return;
        }
        Test.TestTrue(TEXT("root_name names the bottom root"), RootName.Contains(State.BottomName));
        Test.TestTrue(TEXT("root_name names the top root"), RootName.Contains(State.TopName));

        const FDriveElement* BottomButton = FindButton(Elements, State.BottomName);
        const FDriveElement* TopButton = FindButton(Elements, State.TopName);
        if (!Test.TestNotNull(TEXT("bottom root's button is listed with its root"), BottomButton)
            || !Test.TestNotNull(TEXT("top root's button is listed with its root"), TopButton))
        {
            return;
        }
        Test.TestTrue(TEXT("roots are walked bottom-most first"),
            FirstIndexOfRoot(Elements, State.BottomName) < FirstIndexOfRoot(Elements, State.TopName));

        TSet<FString> Handles;
        for (const FDriveElement& Element : Elements)
        {
            bool bDuplicate = false;
            Handles.Add(Element.Handle, &bDuplicate);
            Test.TestFalse(*FString::Printf(TEXT("handle '%s' is unique across roots"), *Element.Handle), bDuplicate);
        }
        Test.TestNotEqual(TEXT("same-named buttons in two roots get distinct handles"),
            BottomButton->Handle, TopButton->Handle);

        // 2) A bare handle resolves into the root it was listed under.
        for (const FDriveElement* Listed : { BottomButton, TopButton })
        {
            const FDriveResolveResult Resolved = FDriveLiveResolver::ResolveHandle(FDriveRootSelector{}, Listed->Handle);
            Test.TestEqual(*FString::Printf(TEXT("'%s' resolves with no selector"), *Listed->Handle),
                static_cast<int32>(Resolved.Status), static_cast<int32>(EDriveResolveStatus::Found));
            Test.TestEqual(*FString::Printf(TEXT("'%s' resolves into its root"), *Listed->Handle),
                Resolved.Element.Root, Listed->Root);
        }

        // 3) instance_name still narrows to one root.
        FDriveRootSelector Narrow;
        Narrow.InstanceName = State.TopName;
        TArray<FDriveElement> Narrowed;
        Test.TestTrue(TEXT("instance_name narrows"),
            FDriveLiveResolver::BuildElementList(Narrow, Narrowed, RootName, Code, Message));
        Test.TestEqual(TEXT("narrowed root_name"), RootName, State.TopName);
        Test.TestTrue(TEXT("narrowed list holds only the named root"), Narrowed.Num() > 0
            && !Narrowed.ContainsByPredicate([&State](const FDriveElement& E) { return E.Root != State.TopName; }));

        // 4) The wire: drive.observe reports the instance and each element's root; world selects.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("screenshot"), false);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("drive.observe"), Payload, Capture);
        if (Test.TestTrue(*FString::Printf(TEXT("bare drive.observe succeeds (%s %s)"), *Capture.ErrorCode, *Capture.Message),
                Capture.bSuccess && Capture.Result.IsValid()))
        {
            FString WorldLabel;
            Test.TestTrue(TEXT("observation names its PIE instance"),
                Capture.Result->TryGetStringField(TEXT("world"), WorldLabel) && WorldLabel.StartsWith(TEXT("pie:")));
            bool bRootOnWire = false;
            const TArray<TSharedPtr<FJsonValue>>* WireElements = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("elements"), WireElements))
            {
                for (const TSharedPtr<FJsonValue>& Value : *WireElements)
                {
                    FString WireRoot;
                    bRootOnWire |= Value->AsObject()->TryGetStringField(TEXT("root"), WireRoot) && WireRoot == State.TopName;
                }
            }
            Test.TestTrue(TEXT("elements carry their root on the wire"), bRootOnWire);
        }

        Payload->SetStringField(TEXT("world"), TEXT("server"));
        InvokeHandlerWithCapture(TEXT("drive.observe"), Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("world 'server' selects the sole standalone instance (%s %s)"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);

        Payload->SetStringField(TEXT("world"), TEXT("client:1"));
        InvokeHandlerWithCapture(TEXT("drive.observe"), Payload, Capture);
        Test.TestEqual(TEXT("world 'client:1' without a client is WORLD_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("WORLD_NOT_FOUND")));
    }
}

// ============================================================================
// Pure: PIE-instance selection over fake instances
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveGameSurfacePieInstanceSelectionTest,
    "PinWright.drive.game_surface.PieInstanceSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveGameSurfacePieInstanceSelectionTest::RunTest(const FString& Parameters)
{
    using namespace DriveGameSurfaceTestHelpers;
    ExpectPick(*this, TEXT("no PIE"), {}, TEXT(""), INDEX_NONE, TEXT("PIE_NOT_RUNNING"));

    const TArray<FDrivePieInstance> Standalone = { MakeInstance(0, NM_Standalone, true) };
    ExpectPick(*this, TEXT("standalone"), Standalone, TEXT(""), 0);
    ExpectPick(*this, TEXT("standalone"), Standalone, TEXT("server"), 0);
    ExpectPick(*this, TEXT("standalone"), Standalone, TEXT("client"), INDEX_NONE, TEXT("WORLD_NOT_FOUND"));
    ExpectPick(*this, TEXT("standalone"), Standalone, TEXT("editor"), INDEX_NONE, TEXT("INVALID_ARGUMENT"));
    ExpectPick(*this, TEXT("standalone"), Standalone, TEXT("client:0"), INDEX_NONE, TEXT("INVALID_ARGUMENT"));
    ExpectPick(*this, TEXT("standalone"), Standalone, TEXT("bogus"), INDEX_NONE, TEXT("INVALID_ARGUMENT"));

    // Listen server + one client: both render UI, so an omitted world must not pick one.
    const TArray<FDrivePieInstance> Listen = {
        MakeInstance(0, NM_ListenServer, true), MakeInstance(1, NM_Client, true) };
    ExpectPick(*this, TEXT("listen"), Listen, TEXT(""), INDEX_NONE, TEXT("TARGET_AMBIGUOUS"));
    ExpectPick(*this, TEXT("listen"), Listen, TEXT("server"), 0);
    ExpectPick(*this, TEXT("listen"), Listen, TEXT("pie:0"), 0);
    ExpectPick(*this, TEXT("listen"), Listen, TEXT("client"), 1);
    ExpectPick(*this, TEXT("listen"), Listen, TEXT(" Client:1 "), 1);
    ExpectPick(*this, TEXT("listen"), Listen, TEXT("pie:1"), 1);
    ExpectPick(*this, TEXT("listen"), Listen, TEXT("client:2"), INDEX_NONE, TEXT("WORLD_NOT_FOUND"));
    {
        int32 Index = INDEX_NONE;
        FString Code;
        FString Message;
        FDriveLiveResolver::SelectPieInstance(TEXT(""), Listen, Index, Code, Message);
        TestTrue(TEXT("ambiguity lists every candidate instance"),
            Message.Contains(TEXT("pie:0 server")) && Message.Contains(TEXT("pie:1 client (client:1)")));
    }

    // Dedicated server renders no UI: the one client is the only drivable instance.
    const TArray<FDrivePieInstance> Dedicated = {
        MakeInstance(0, NM_DedicatedServer, false), MakeInstance(1, NM_Client, true) };
    ExpectPick(*this, TEXT("dedicated + 1"), Dedicated, TEXT(""), 1);
    ExpectPick(*this, TEXT("dedicated + 1"), Dedicated, TEXT("server"), INDEX_NONE, TEXT("GAME_VIEWPORT_NOT_FOUND"));
    ExpectPick(*this, TEXT("dedicated + 1"), Dedicated, TEXT("client:1"), 1);

    const TArray<FDrivePieInstance> DedicatedTwo = {
        MakeInstance(0, NM_DedicatedServer, false), MakeInstance(1, NM_Client, true), MakeInstance(2, NM_Client, true) };
    ExpectPick(*this, TEXT("dedicated + 2"), DedicatedTwo, TEXT(""), INDEX_NONE, TEXT("TARGET_AMBIGUOUS"));
    ExpectPick(*this, TEXT("dedicated + 2"), DedicatedTwo, TEXT("client:2"), 2);

    // PIE running but nothing has a viewport (e.g. Simulate In Editor).
    ExpectPick(*this, TEXT("no viewport"), { MakeInstance(0, NM_Standalone, false) }, TEXT(""),
        INDEX_NONE, TEXT("PIE_NOT_RUNNING"));
    return true;
}

// ============================================================================
// Live: two UMG roots on one PIE viewport
// ============================================================================

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FRunDriveTwoRootsCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveGameSurfaceTestHelpers::FState>, State);

bool FRunDriveTwoRootsCommand::Update()
{
    using namespace DriveGameSurfaceTestHelpers;
    UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
    UWorld* World = Viewport ? Viewport->GetWorld() : nullptr;
    APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
    if (!GEditor || !GEditor->PlayWorld || !Player)
    {
        if (FPlatformTime::Seconds() < State->Deadline)
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*Test, TEXT("owned_pie_player_unavailable"),
            TEXT("The owned PIE session did not bind a game viewport with a player controller."));
        return true;
    }

    if (!State->Bottom.IsValid())
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        State->BottomName = FString::Printf(TEXT("PWDriveRootBottom_%s"), *Suffix);
        State->TopName = FString::Printf(TEXT("PWDriveRootTop_%s"), *Suffix);
        State->Bottom.Reset(MakeProbe(Player, State->BottomName));
        State->Top.Reset(MakeProbe(Player, State->TopName));
        if (!State->Bottom.IsValid() || !State->Top.IsValid())
        {
            Test->AddError(TEXT("Could not create the two probe widgets."));
            State->Bottom.Reset();
            State->Top.Reset();
            return true;
        }
        // High z-orders sit above any host overlay; Bottom below Top.
        State->Bottom->AddToViewport(900000);
        State->Top->AddToViewport(900001);
        State->AssertNotBeforeFrame = GFrameCounter + 5;
        return false;
    }
    if (GFrameCounter < State->AssertNotBeforeFrame)
    {
        return false;
    }

    AssertTwoRoots(*Test, *State);
    // The probes are outered to the PIE game instance: detach them and drop the strong refs
    // BEFORE FEndPlayMapCommand, or the end-of-PIE leak check finds the game instance still
    // referenced through them ("not cleaned up by GC").
    State->Bottom->RemoveFromParent();
    State->Top->RemoveFromParent();
    State->Bottom.Reset();
    State->Top.Reset();
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FCleanupDriveTwoRootsCommand,
    FAutomationTestBase*, Test, TSharedRef<DriveGameSurfaceTestHelpers::FState>, State);

bool FCleanupDriveTwoRootsCommand::Update()
{
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return true;
    }
    if (State->CleanupDeadline == 0.0)
    {
        State->CleanupDeadline = FPlatformTime::Seconds() + 15.0;
    }
    if (FPlatformTime::Seconds() >= State->CleanupDeadline)
    {
        Test->AddError(TEXT("Timed out waiting for the owned PIE session to stop."));
        return true;
    }
    return false;
}

// Counterfactual: before the fix a bare BuildElementList over two roots failed AMBIGUOUS_LIVE_ROOT,
// and same-named leaves in two separately-walked roots could share a handle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveGameSurfaceTwoRootsTest,
    "PinWright.drive.game_surface.TwoUmgRootsObservedTogether",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveGameSurfaceTwoRootsTest::RunTest(const FString& Parameters)
{
    using DriveGameSurfaceTestHelpers::FState;
    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor_or_slate_unavailable"),
            TEXT("Owned PIE requires GEditor and Slate."));
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
            TEXT("PIE in the level viewport requires an active level viewport."));
        return true;
    }

    const TSharedRef<FState> State = MakeShared<FState>();
    State->Deadline = FPlatformTime::Seconds() + 20.0;
    ADD_LATENT_AUTOMATION_COMMAND(FStartHostNeutralPieCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FRunDriveTwoRootsCommand(this, State));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FCleanupDriveTwoRootsCommand(this, State));
    return true;
}
