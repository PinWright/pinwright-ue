// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Input domain handlers (InputHandler.cpp)
// Handlers tested: input.create_input_action, input.create_input_mapping_context,
//   input.add_mapping, input.remove_mapping, input.get_input_info
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Tests/TestUtils.h"


namespace TestInputHandlersHelpers
{
    const TCHAR* const FixtureFolder = TEXT("/Game/PinWrightTests/Input");

    // An Input Action + Input Mapping Context pair that lives until the owning test's
    // scope ends. Paths are set before creation so the destructor also removes a
    // partially created pair.
    struct FInputFixture
    {
        FString ActionPath;
        FString ContextPath;

        explicit FInputFixture(const FString& Suffix)
            : ActionPath(FString::Printf(TEXT("%s/IA_%s"), FixtureFolder, *Suffix))
            , ContextPath(FString::Printf(TEXT("%s/IMC_%s"), FixtureFolder, *Suffix))
        {
            // A previous aborted run may have left the fixture on disk.
            CleanupTestAsset(ContextPath);
            CleanupTestAsset(ActionPath);
        }

        ~FInputFixture()
        {
            CleanupTestAsset(ContextPath);
            CleanupTestAsset(ActionPath);
        }

        // Creates both assets through the production create verbs and asserts they exist.
        bool Create(FAutomationTestBase& Test) const
        {
            FTestResponseCapture Capture;
            TSharedPtr<FJsonObject> ActionPayload = MakeShared<FJsonObject>();
            ActionPayload->SetStringField(TEXT("name"), FPackageName::GetShortName(ActionPath));
            ActionPayload->SetStringField(TEXT("path"), FixtureFolder);
            InvokeHandlerWithCapture(TEXT("input.create_input_action"), ActionPayload, Capture);
            if (!Test.TestTrue(TEXT("fixture Input Action created"), Capture.bSuccess))
            {
                return false;
            }

            TSharedPtr<FJsonObject> ContextPayload = MakeShared<FJsonObject>();
            ContextPayload->SetStringField(TEXT("name"), FPackageName::GetShortName(ContextPath));
            ContextPayload->SetStringField(TEXT("path"), FixtureFolder);
            InvokeHandlerWithCapture(TEXT("input.create_input_mapping_context"), ContextPayload, Capture);
            if (!Test.TestTrue(TEXT("fixture Input Mapping Context created"), Capture.bSuccess))
            {
                return false;
            }

            return Test.TestNotNull(TEXT("fixture Input Action loads"),
                    Cast<UInputAction>(UEditorAssetLibrary::LoadAsset(ActionPath)))
                && Test.TestNotNull(TEXT("fixture Input Mapping Context loads"),
                    LoadContext());
        }

        UInputMappingContext* LoadContext() const
        {
            return Cast<UInputMappingContext>(UEditorAssetLibrary::LoadAsset(ContextPath));
        }

        // Binds Key to the fixture action through input.add_mapping and asserts success.
        bool AddMapping(FAutomationTestBase& Test, const FString& Key, FTestResponseCapture& Capture) const
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("contextPath"), ContextPath);
            Payload->SetStringField(TEXT("actionPath"), ActionPath);
            Payload->SetStringField(TEXT("key"), Key);
            Test.TestTrue(TEXT("input.add_mapping handler found and invoked"),
                InvokeHandlerWithCapture(TEXT("input.add_mapping"), Payload, Capture));
            return Test.TestTrue(TEXT("input.add_mapping succeeded on the live fixture"), Capture.bSuccess)
                && Test.TestTrue(TEXT("input.add_mapping returned a result"), Capture.Result.IsValid());
        }
    };
} // namespace TestInputHandlersHelpers

// ============================================================================
// input.create_input_action
// ============================================================================

// Both required params present — handler creates the asset at the requested path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputCreateInputActionValidParamsNoCrashTest,
    "PinWright.input.create_input_action.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputCreateInputActionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("%s/IA_InputHandlersCreate"),
        TestInputHandlersHelpers::FixtureFolder);
    CleanupTestAsset(AssetPath);
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), FPackageName::GetShortName(AssetPath));
    Payload->SetStringField(TEXT("path"), TestInputHandlersHelpers::FixtureFolder);
    FTestResponseCapture Capture;
    TestTrue(TEXT("input.create_input_action handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("input.create_input_action"), Payload, Capture));
    TestTrue(TEXT("input.create_input_action succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("created asset is a loadable UInputAction"),
        Cast<UInputAction>(UEditorAssetLibrary::LoadAsset(AssetPath)));
    return true;
}

// ============================================================================
// input.create_input_mapping_context
// ============================================================================

// Both required params present — handler creates the asset at the requested path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputCreateInputMappingContextValidParamsNoCrashTest,
    "PinWright.input.create_input_mapping_context.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputCreateInputMappingContextValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("%s/IMC_InputHandlersCreate"),
        TestInputHandlersHelpers::FixtureFolder);
    CleanupTestAsset(AssetPath);
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), FPackageName::GetShortName(AssetPath));
    Payload->SetStringField(TEXT("path"), TestInputHandlersHelpers::FixtureFolder);
    FTestResponseCapture Capture;
    TestTrue(TEXT("input.create_input_mapping_context handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("input.create_input_mapping_context"), Payload, Capture));
    TestTrue(TEXT("input.create_input_mapping_context succeeded"), Capture.bSuccess);
    TestNotNull(TEXT("created asset is a loadable UInputMappingContext"),
        Cast<UInputMappingContext>(UEditorAssetLibrary::LoadAsset(AssetPath)));
    return true;
}

// ============================================================================
// input.add_mapping
// ============================================================================

// Live action + context fixture: the mapping must be reported and stored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputAddMappingValidParamsNoCrashTest,
    "PinWright.input.add_mapping.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputAddMappingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    const TestInputHandlersHelpers::FInputFixture Fixture(TEXT("InputHandlersAddMapping"));
    if (!Fixture.Create(*this))
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!Fixture.AddMapping(*this, TEXT("SpaceBar"), Capture))
    {
        return false;
    }
    TestEqual(TEXT("response key"), Capture.Result->GetStringField(TEXT("key")), FString(TEXT("SpaceBar")));
    TestTrue(TEXT("response actionPath names the fixture action"),
        Capture.Result->GetStringField(TEXT("actionPath")).StartsWith(Fixture.ActionPath + TEXT(".")));

    UInputMappingContext* Context = Fixture.LoadContext();
    if (!TestNotNull(TEXT("in-memory context resolves"), Context)
        || !TestFalse(TEXT("context package saved"), Context->GetOutermost()->IsDirty())
        || !TestEqual(TEXT("one mapping stored"), Context->GetMappings().Num(), 1))
    {
        return false;
    }
    const FEnhancedActionKeyMapping& Mapping = Context->GetMappings()[0];
    TestEqual(TEXT("stored key"), Mapping.Key.ToString(), FString(TEXT("SpaceBar")));
    TestTrue(TEXT("stored mapping targets the fixture action"),
        Mapping.Action == Cast<UInputAction>(UEditorAssetLibrary::LoadAsset(Fixture.ActionPath)));
    return true;
}

// ============================================================================
// input.remove_mapping
// ============================================================================

// Live fixture with one mapping: remove must report and clear it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputRemoveMappingValidParamsNoCrashTest,
    "PinWright.input.remove_mapping.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputRemoveMappingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    const TestInputHandlersHelpers::FInputFixture Fixture(TEXT("InputHandlersRemoveMapping"));
    FTestResponseCapture Capture;
    if (!Fixture.Create(*this) || !Fixture.AddMapping(*this, TEXT("SpaceBar"), Capture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("contextPath"), Fixture.ContextPath);
    Payload->SetStringField(TEXT("actionPath"), Fixture.ActionPath);
    TestTrue(TEXT("input.remove_mapping handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("input.remove_mapping"), Payload, Capture));
    if (!TestTrue(TEXT("input.remove_mapping succeeded on the live fixture"), Capture.bSuccess)
        || !TestTrue(TEXT("input.remove_mapping returned a result"), Capture.Result.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("keysRemoved"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("keysRemoved"))), 1);
    const TArray<TSharedPtr<FJsonValue>>* RemovedKeys = nullptr;
    Capture.Result->TryGetArrayField(TEXT("removedKeys"), RemovedKeys);
    TestTrue(TEXT("removedKeys lists SpaceBar"), JsonValueArrayContainsString(RemovedKeys, TEXT("SpaceBar")));

    UInputMappingContext* Context = Fixture.LoadContext();
    if (TestNotNull(TEXT("in-memory context resolves"), Context))
    {
        TestFalse(TEXT("context package saved"), Context->GetOutermost()->IsDirty());
        TestEqual(TEXT("no mapping left in the context"), Context->GetMappings().Num(), 0);
    }
    return true;
}

// ============================================================================
// input.get_input_info
// ============================================================================

// Live fixture with one mapping: readback must describe both assets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputGetInputInfoValidParamsNoCrashTest,
    "PinWright.input.get_input_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputGetInputInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    const TestInputHandlersHelpers::FInputFixture Fixture(TEXT("InputHandlersGetInfo"));
    FTestResponseCapture Capture;
    if (!Fixture.Create(*this) || !Fixture.AddMapping(*this, TEXT("SpaceBar"), Capture))
    {
        return false;
    }

    TSharedPtr<FJsonObject> ActionPayload = MakeShared<FJsonObject>();
    ActionPayload->SetStringField(TEXT("assetPath"), Fixture.ActionPath);
    TestTrue(TEXT("input.get_input_info handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("input.get_input_info"), ActionPayload, Capture));
    if (TestTrue(TEXT("action readback succeeded"), Capture.bSuccess)
        && TestTrue(TEXT("action readback returned a result"), Capture.Result.IsValid()))
    {
        TestEqual(TEXT("action readback type"), Capture.Result->GetStringField(TEXT("type")),
            FString(TEXT("InputAction")));
    }

    TSharedPtr<FJsonObject> ContextPayload = MakeShared<FJsonObject>();
    ContextPayload->SetStringField(TEXT("assetPath"), Fixture.ContextPath);
    TestTrue(TEXT("input.get_input_info handler found for the context"),
        InvokeHandlerWithCapture(TEXT("input.get_input_info"), ContextPayload, Capture));
    if (TestTrue(TEXT("context readback succeeded"), Capture.bSuccess)
        && TestTrue(TEXT("context readback returned a result"), Capture.Result.IsValid()))
    {
        TestEqual(TEXT("context readback type"), Capture.Result->GetStringField(TEXT("type")),
            FString(TEXT("InputMappingContext")));
        TestEqual(TEXT("context readback mappingCount"),
            static_cast<int32>(Capture.Result->GetNumberField(TEXT("mappingCount"))), 1);
        TestTrue(TEXT("context readback lists the SpaceBar mapping"),
            JsonArrayHasObjectWithStringField(Capture.Result, TEXT("mappings"), TEXT("key"), TEXT("SpaceBar")));
    }
    return true;
}

// Regression for E-get-input-info-consume-field-name-mismatch: get_input_info must
// emit the consume flag under the documented key "bConsumeInput", not the legacy
// "consumeInput", so callers keying by the wiki-documented name find it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputGetInputInfoEmitsDocumentedConsumeKeyTest,
    "PinWright.input.get_input_info.EmitsDocumentedConsumeKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInputGetInputInfoEmitsDocumentedConsumeKeyTest::RunTest(const FString& Parameters)
{
    // The documented field name lives in the handler summary that feeds the wiki.
    const FString Summary = GetRegisteredSummary(TEXT("input.get_input_info"));
    TestTrue(TEXT("summary documents the bConsumeInput field"),
        Summary.Contains(TEXT("bConsumeInput")));

    // Create a real UInputAction so the handler enters its UInputAction branch and
    // serializes the consume flag. The create handler is the production path; the
    // scoped cleanup keeps the asset alive across the inspect call.
    const FString ActionName = TEXT("IA_ConsumeKeyRegression");
    const FString ActionFolder = TestInputHandlersHelpers::FixtureFolder;
    const FString ActionPackage = FString::Printf(TEXT("%s/%s"), *ActionFolder, *ActionName);
    CleanupTestAsset(ActionPackage);
    ON_SCOPE_EXIT { CleanupTestAsset(ActionPackage); };

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), ActionName);
    CreatePayload->SetStringField(TEXT("path"), ActionFolder);
    InvokeHandlerWithCapture(TEXT("input.create_input_action"), CreatePayload, Capture);
    if (!TestTrue(TEXT("fixture Input Action created"), Capture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), ActionPackage);

    const bool bFound = InvokeHandlerWithCapture(TEXT("input.get_input_info"), InfoPayload, Capture);
    TestTrue(TEXT("input.get_input_info handler found"), bFound);
    if (TestTrue(TEXT("input.get_input_info succeeded on the live fixture"), Capture.bSuccess)
        && TestTrue(TEXT("input.get_input_info returned a result"), Capture.Result.IsValid()))
    {
        TestTrue(TEXT("response carries the documented bConsumeInput key"),
            Capture.Result->HasField(TEXT("bConsumeInput")));
        TestFalse(TEXT("response does NOT carry the legacy consumeInput key"),
            Capture.Result->HasField(TEXT("consumeInput")));
    }

    return true;
}
