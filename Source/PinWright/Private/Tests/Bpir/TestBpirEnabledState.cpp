// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirEnabledState.cpp - BPIR node enabled-state parser and round-trip coverage.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "BpirGraphTestHelpers.h"
#include "CompilerTestUtils.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirParser.h"
#include "Compiler/BpirTypes.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

using namespace CompilerTestUtils;
using BpirGraphTestHelpers::FindFirstNodeOfType;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirEnabledStateSuffixParserTest,
    "PinWright.bpir.parser.EnabledStateSuffix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEnabledStateSuffixParserTest::RunTest(const FString& Parameters)
{
    const FString Code = TEXT(
        "entry event BeginPlay() disabled @(10, 20) {\n"
        "    call PrintString(InString: \"development\") devonly @(30, 40)\n"
        "    set bIsReady = true @(50, 60) disabled # state after the position marker\n"
        "    parent_call Actor::ReceiveBeginPlay() disabled\n"
        "}");

    FBpirParser Parser;
    TArray<FBpirEntryBlock> Blocks;
    TArray<FCompileError> Errors;
    const bool bParsed = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/true);

    TestTrue(TEXT("BPIR with enabled-state markers parses"), bParsed);
    TestEqual(TEXT("One entry block parsed"), Blocks.Num(), 1);
    if (Blocks.Num() != 1)
    {
        return false;
    }

    const FBpirEntryBlock& Block = Blocks[0];
    TestTrue(TEXT("Entry records an explicit enabled state"), Block.bHasEnabledState);
    TestEqual(TEXT("Entry state is Disabled"), Block.EnabledState, EBpirNodeEnabledState::Disabled);
    TestTrue(TEXT("Entry position is preserved"), Block.bHasAuthoredEntryPosition);
    TestEqual(TEXT("Entry X position"), Block.AuthoredEntryPosition.X, 10.0);
    TestEqual(TEXT("Entry Y position"), Block.AuthoredEntryPosition.Y, 20.0);

    TestEqual(TEXT("Three body instructions parsed"), Block.Instructions.Num(), 3);
    if (Block.Instructions.Num() == 3)
    {
        const FBpirInstruction& DevelopmentOnlyInstruction = Block.Instructions[0];
        TestTrue(TEXT("First instruction records an explicit enabled state"),
            DevelopmentOnlyInstruction.bHasEnabledState);
        TestEqual(TEXT("First instruction state is DevelopmentOnly"),
            DevelopmentOnlyInstruction.EnabledState,
            EBpirNodeEnabledState::DevelopmentOnly);
        TestTrue(TEXT("First instruction position is preserved"),
            DevelopmentOnlyInstruction.bHasAuthoredPosition);

        const FBpirInstruction& DisabledInstruction = Block.Instructions[1];
        TestTrue(TEXT("Second instruction records state after position"),
            DisabledInstruction.bHasEnabledState);
        TestEqual(TEXT("Second instruction state is Disabled"),
            DisabledInstruction.EnabledState,
            EBpirNodeEnabledState::Disabled);
        TestTrue(TEXT("Second instruction position is preserved"),
            DisabledInstruction.bHasAuthoredPosition);

        const FBpirInstruction& ParentInstruction = Block.Instructions[2];
        TestTrue(TEXT("Parent call keyword is preserved"), ParentInstruction.bParentCall);
        TestEqual(TEXT("Parent call class is preserved"), ParentInstruction.TypeArg, FString(TEXT("Actor")));
        TestEqual(TEXT("Parent call state is preserved"), ParentInstruction.EnabledState, EBpirNodeEnabledState::Disabled);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirEnabledStateRoundTripTest,
    "PinWright.bpir.round_trip.EnabledState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirEnabledStateRoundTripTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BPIR_EnabledState_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    UPackage* Package = CreatePackage(*PackagePath);
    TestNotNull(TEXT("Saved test package created"), Package);
    if (!Package) return false;
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("Saved test Blueprint created"), Blueprint);
    if (!Blueprint) return false;
    FAssetRegistryModule::AssetCreated(Blueprint);
    Package->MarkPackageDirty();

    const FString Input = TEXT(
        "entry custom_event DisabledEvent() disabled {\n"
        "    call PrintString(InString: \"disabled body\") disabled\n"
        "    parent_call Actor::K2_DestroyActor() disabled\n"
        "}\n"
        "entry custom_event DevOnlyEvent() devonly {\n"
        "    call PrintString(InString: \"devonly body\") devonly\n"
        "}");
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));
    Payload->SetStringField(TEXT("code"), Input);
    FTestResponseCapture Capture;
    TestTrue(TEXT("compile_bpir handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
    TestTrue(FString::Printf(TEXT("compile_bpir succeeds: %s"), *Capture.Message), Capture.bSuccess);
    if (!Capture.bSuccess) return false;

    TSharedPtr<FJsonObject> SavePayload = MakeShared<FJsonObject>();
    SavePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    SavePayload->SetBoolField(TEXT("force"), true);
    FTestResponseCapture SaveCapture;
    TestTrue(TEXT("asset.save handler found"),
        InvokeHandlerWithCapture(TEXT("asset.save"), SavePayload, SaveCapture));
    TestTrue(TEXT("asset.save succeeds"), SaveCapture.bSuccess);
    if (!SaveCapture.bSuccess) return false;

    TSharedPtr<FJsonObject> DecompilePayload = MakeShared<FJsonObject>();
    DecompilePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture DecompileCapture;
    TestTrue(TEXT("blueprint.decompile handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.decompile"), DecompilePayload, DecompileCapture));
    TestTrue(TEXT("blueprint.decompile succeeds"), DecompileCapture.bSuccess);
    FString Decompiled;
    if (DecompileCapture.Result.IsValid())
        DecompileCapture.Result->TryGetStringField(TEXT("bpir"), Decompiled);
    TestTrue(TEXT("saved BPIR contains disabled marker"), Decompiled.Contains(TEXT("DisabledEvent() disabled")));
    TestTrue(TEXT("saved BPIR contains development-only marker"), Decompiled.Contains(TEXT("DevOnlyEvent() devonly")));

    Payload->SetStringField(TEXT("code"), Decompiled);
    FTestResponseCapture RecompileCapture;
    TestTrue(TEXT("decompiled compile_bpir handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, RecompileCapture));
    TestTrue(TEXT("decompiled BPIR recompiles"), RecompileCapture.bSuccess);
    if (!RecompileCapture.bSuccess) return false;
    TestTrue(TEXT("recompiled asset.save succeeds"),
        InvokeHandlerWithCapture(TEXT("asset.save"), SavePayload, SaveCapture) && SaveCapture.bSuccess);

    TSharedPtr<FJsonObject> ReloadPayload = MakeShared<FJsonObject>();
    ReloadPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture ReloadCapture;
    TestTrue(TEXT("asset.reload handler found"),
        InvokeHandlerWithCapture(TEXT("asset.reload"), ReloadPayload, ReloadCapture));
    TestTrue(TEXT("asset.reload succeeds"), ReloadCapture.bSuccess);
    UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
    TestNotNull(TEXT("reloaded Blueprint exists"), Reloaded);
    if (!Reloaded) return false;
    int32 DisabledEntries = 0, DevOnlyEntries = 0, DisabledCalls = 0, DevOnlyCalls = 0, DisabledParentCalls = 0;
    for (UEdGraph* Graph : Reloaded->UbergraphPages)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node))
            {
                if (Event->CustomFunctionName == TEXT("DisabledEvent") && Event->GetDesiredEnabledState() == ENodeEnabledState::Disabled) ++DisabledEntries;
                if (Event->CustomFunctionName == TEXT("DevOnlyEvent") && Event->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly) ++DevOnlyEntries;
            }
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->IsA<UK2Node_CallParentFunction>()
                    && Call->GetDesiredEnabledState() == ENodeEnabledState::Disabled)
                {
                    ++DisabledParentCalls;
                }
                if (Call->GetTargetFunction() && Call->GetTargetFunction()->GetName() == TEXT("PrintString"))
                {
                    if (Call->GetDesiredEnabledState() == ENodeEnabledState::Disabled) ++DisabledCalls;
                    if (Call->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly) ++DevOnlyCalls;
                }
            }
        }
    }
    TestEqual(TEXT("reloaded disabled entry state"), DisabledEntries, 1);
    TestEqual(TEXT("reloaded devonly entry state"), DevOnlyEntries, 1);
    TestEqual(TEXT("reloaded disabled body state"), DisabledCalls, 1);
    TestEqual(TEXT("reloaded devonly body state"), DevOnlyCalls, 1);
    TestEqual(TEXT("reloaded parent call keeps node class and disabled state"), DisabledParentCalls, 1);

    return true;
}
