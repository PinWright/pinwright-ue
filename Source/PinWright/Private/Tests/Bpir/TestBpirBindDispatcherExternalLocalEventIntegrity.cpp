// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bpir-bind-dispatcher-external-target-local-event.
//
// The full BPIR → KismetCompile → integrity-gate pipeline has repeatedly flagged
// cross-object `bind_dispatcher` (external Target + local @Handler custom event)
// as a stale reference, rolling back valid user code. Prior fix cycles (P0-1..P0-5)
// never ran the full pipeline end-to-end in tests; existing coverage only exercises
// FBpirCompiler::Compile and never invokes ValidateBlueprintGraphIntegrity post
// full compile.
//
// This test reproduces the exact failing shape:
//   entry event BeginPlay() { bind_dispatcher <ExtDispatcher>(Target: $Ext, event: @Handler) }
//   entry custom_event Handler() { }
// - $Ext is a member variable of type UAudioComponent* (external-to-self class).
// - OnAudioFinished is a param-less DYNAMIC_MULTICAST_DELEGATE on UAudioComponent.
// - Handler is a local custom event on the BP's generated class (self).
//
// After BPIR emit, the test runs a full CompileBlueprintWithDiagnostics (matching
// the BpirCompilerHandler's pipeline) then calls ValidateBlueprintGraphIntegrity
// and asserts zero failures. On failure, every failure's Reason is dumped via
// AddError so the engine-level cause is captured in the log — that is the signal
// the prior fix cycles missed.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Components/AudioComponent.h"
#include "Components/Button.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBindDispatcherExternalLocalEventIntegrityTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_ExternalTargetLocalEventPassesIntegrityGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBindDispatcherExternalLocalEventIntegrityTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindExtLocalHandlerIntegrityBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Add a member variable of type UAudioComponent* — genuinely external (not self-class).
    FEdGraphPinType TargetType;
    TargetType.PinCategory = UEdGraphSchema_K2::PC_Object;
    TargetType.PinSubCategoryObject = UAudioComponent::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Ext"), TargetType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    // BPIR: local `Handler` custom event, plus an external-target bind_dispatcher of
    // UAudioComponent::OnAudioFinished (param-less DYNAMIC_MULTICAST_DELEGATE). The
    // historically failing shape.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    bind_dispatcher OnAudioFinished(Target: $Ext, event: @Handler)\n")
        TEXT("}\n")
        TEXT("entry custom_event Handler() {\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Sanity: verify AddDelegate + CreateDelegate pair emitted and wired.
    UK2Node_CreateDelegate* CreateDelegateNode = nullptr;
    UK2Node_AddDelegate* AddDelegateNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!CreateDelegateNode) CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node);
            if (!AddDelegateNode)    AddDelegateNode    = Cast<UK2Node_AddDelegate>(Node);
        }
    }
    TestNotNull(TEXT("CreateDelegate node exists"), CreateDelegateNode);
    TestNotNull(TEXT("AddDelegate node exists"), AddDelegateNode);

    // Mirror BpirCompilerHandler: full compile, then refresh, then integrity gate.
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (!Diagnostics.bCompiled)
    {
        for (const FString& Err : Diagnostics.Errors)
        {
            AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
        }
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);

    BlueprintHandlerUtils::RefreshBpirDelegateNodes(BP, Result.CreatedNodeGUIDs);

    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bIntegrityOk = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(BP, Failures);

    if (!bIntegrityOk)
    {
        // Surface the engine-level Reason so mcp-review / CI logs capture the exact
        // cause — previous fix cycles guessed without this evidence.
        for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& Fail : Failures)
        {
            AddError(FString::Printf(
                TEXT("IntegrityFailure nodeKind=%s graphName=%s reason=%s"),
                *Fail.NodeKind, *Fail.GraphName, *Fail.Reason));
        }
    }

    TestTrue(TEXT("ValidateBlueprintGraphIntegrity returns true for cross-object bind_dispatcher + local custom event"),
        bIntegrityOk);
    TestEqual(TEXT("No integrity failures recorded"), Failures.Num(), 0);
    return bIntegrityOk;
}

// Second variant: Target is a cast<> result, not a plain VariableGet. The field
// repros for this bug (host-project front-end widgets) all resolved Target via
// `%typed = cast<W_ReplayEditor_*>(%overlay) [success -> @ok, fail -> @done]`
// before `bind_dispatcher`. The cast's output pin has a different
// PinSubCategoryObject shape than a VariableGet, so it exercises an additional
// moving part in ResolveTargetClass / GetAuthoritativePinClass even though the
// downstream wiring (self → CreateDelegate.ObjectPin → HandleAnyChange refresh)
// is the same.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBindDispatcherCastResultLocalEventIntegrityTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_CastResultTargetLocalEventPassesIntegrityGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBindDispatcherCastResultLocalEventIntegrityTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BindCastLocalHandlerIntegrityBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    // Base member variable typed as UActorComponent* — the cast source. The BPIR
    // then narrows this to UAudioComponent via `cast<AudioComponent>`; that cast
    // output pin's PinSubCategoryObject is UAudioComponent, which drives
    // ResolveTargetClass for the bind_dispatcher emit.
    FEdGraphPinType BaseType;
    BaseType.PinCategory = UEdGraphSchema_K2::PC_Object;
    BaseType.PinSubCategoryObject = UActorComponent::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Base"), BaseType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %typed = cast<AudioComponent>($Base) [success -> @ok, fail -> @done]\n")
        TEXT("@ok:\n")
        TEXT("    bind_dispatcher OnAudioFinished(Target: %typed, event: @Handler)\n")
        TEXT("@done:\n")
        TEXT("}\n")
        TEXT("entry custom_event Handler() {\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CreateDelegate* CreateDelegateNode = nullptr;
    UK2Node_AddDelegate* AddDelegateNode = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!CreateDelegateNode) CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node);
            if (!AddDelegateNode)    AddDelegateNode    = Cast<UK2Node_AddDelegate>(Node);
        }
    }
    TestNotNull(TEXT("CreateDelegate node exists"), CreateDelegateNode);
    TestNotNull(TEXT("AddDelegate node exists"), AddDelegateNode);

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (!Diagnostics.bCompiled)
    {
        for (const FString& Err : Diagnostics.Errors)
        {
            AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
        }
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);

    BlueprintHandlerUtils::RefreshBpirDelegateNodes(BP, Result.CreatedNodeGUIDs);

    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bIntegrityOk = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(BP, Failures);

    if (!bIntegrityOk)
    {
        for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& Fail : Failures)
        {
            AddError(FString::Printf(
                TEXT("IntegrityFailure nodeKind=%s graphName=%s reason=%s"),
                *Fail.NodeKind, *Fail.GraphName, *Fail.Reason));
        }
    }

    TestTrue(TEXT("ValidateBlueprintGraphIntegrity returns true for cast-result-targeted bind_dispatcher + local custom event"),
        bIntegrityOk);
    TestEqual(TEXT("No integrity failures recorded"), Failures.Num(), 0);
    return bIntegrityOk;
}

// Third variant: the actual field-repro shape — a proper UWidgetBlueprint (not just
// a plain UBlueprint with UUserWidget parent) compiling `entry widget_event
// <Btn>.OnClicked()` with an inner `bind_dispatcher` to a local custom event. The
// live repros (host-project front-end widgets) all run through this path,
// which goes through the handler's widget-BP pre-compile branch
// (`BpirCompilerHandler.cpp:136` — `Cast<UWidgetBlueprint>(BP)`) and emits the
// entry via the `EBpirEntryKind::WidgetEvent` / `SetupWidgetEvent` path rather
// than `CustomEvent` / `Event`. Prior two variants cover AActor-based BPs with
// `entry event BeginPlay()` / `entry custom_event`, so this closes the
// widget-event-specific coverage gap the tester identified.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirBindDispatcherWidgetEventLocalHandlerIntegrityTest,
    "PinWright.bpir.compiler.integration.BindDispatcher_WidgetEventLocalHandlerPassesIntegrityGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirBindDispatcherWidgetEventLocalHandlerIntegrityTest::RunTest(const FString& Parameters)
{
    // Create a genuine UWidgetBlueprint via the same call pattern WidgetCreateHandler uses.
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("BindWidgetEventHandlerIntegrityBP_%d"), FMath::Rand())),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));

    TestNotNull(TEXT("Widget blueprint was created"), WBP);
    if (!WBP) return false;

    // UButton variable "Btn" backs the widget_event subject. UButton::OnClicked is a
    // param-less DYNAMIC_MULTICAST_DELEGATE, which keeps the signature-compat path
    // noise-free so any integrity failure points squarely at the GUID / scope path.
    FEdGraphPinType ButtonType;
    ButtonType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ButtonType.PinSubCategoryObject = UButton::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(WBP, TEXT("Btn"), ButtonType);

    // External target for bind_dispatcher — UAudioComponent lives in a different
    // class hierarchy than the widget's generated class, so this is genuinely
    // cross-object like the field repros.
    FEdGraphPinType ExtType;
    ExtType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ExtType.PinSubCategoryObject = UAudioComponent::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(WBP, TEXT("Ext"), ExtType);

    // Mirror the handler's widget-BP pre-compile (BpirCompilerHandler.cpp:136) so
    // PopulateBlueprintGeneratedVariables populates FProperties before BPIR resolves $Btn / $Ext.
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FBpirCompiler Compiler(WBP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry widget_event Btn.OnClicked() {\n")
        TEXT("    bind_dispatcher OnAudioFinished(Target: $Ext, event: @Handler)\n")
        TEXT("}\n")
        TEXT("entry custom_event Handler() {\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UK2Node_CreateDelegate* CreateDelegateNode = nullptr;
    UK2Node_AddDelegate* AddDelegateNode = nullptr;
    for (UEdGraph* Graph : WBP->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!CreateDelegateNode) CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node);
            if (!AddDelegateNode)    AddDelegateNode    = Cast<UK2Node_AddDelegate>(Node);
        }
    }
    TestNotNull(TEXT("CreateDelegate node exists"), CreateDelegateNode);
    TestNotNull(TEXT("AddDelegate node exists"), AddDelegateNode);

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(WBP);
    if (!Diagnostics.bCompiled)
    {
        for (const FString& Err : Diagnostics.Errors)
        {
            AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
        }
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);

    BlueprintHandlerUtils::RefreshBpirDelegateNodes(WBP, Result.CreatedNodeGUIDs);

    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bIntegrityOk = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(WBP, Failures);

    if (!bIntegrityOk)
    {
        for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& Fail : Failures)
        {
            AddError(FString::Printf(
                TEXT("IntegrityFailure nodeKind=%s graphName=%s reason=%s"),
                *Fail.NodeKind, *Fail.GraphName, *Fail.Reason));
        }
    }

    TestTrue(TEXT("ValidateBlueprintGraphIntegrity returns true for widget_event + bind_dispatcher + local custom event on UWidgetBlueprint"),
        bIntegrityOk);
    TestEqual(TEXT("No integrity failures recorded"), Failures.Num(), 0);
    return bIntegrityOk;
}
