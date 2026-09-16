// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/AssertionMacros.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "IPythonScriptPlugin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace BlueprintCompileGarbageCollectionTest
{
    struct FScopedObservation
    {
        FScopedObservation()
        {
            PreGarbageCollectHandle =
                FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda(
                    [this]() { ++PreGarbageCollectCount; });
            PreviousEnsureHandler = SetEnsureHandler(
                [this](const FEnsureHandlerArgs&) -> bool
                {
                    ++HandledEnsureCount;
                    return true;
                });
        }

        ~FScopedObservation()
        {
            FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(
                PreGarbageCollectHandle);
            SetEnsureHandler(MoveTemp(PreviousEnsureHandler));
        }

        int32 PreGarbageCollectCount = 0;
        int32 HandledEnsureCount = 0;

    private:
        FDelegateHandle PreGarbageCollectHandle;
        TFunction<bool(const FEnsureHandlerArgs&)> PreviousEnsureHandler;
    };
}

DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(
    FWaitForScheduledBlueprintGarbageCollection,
    FAutomationTestBase*, Test,
    TSharedRef<BlueprintCompileGarbageCollectionTest::FScopedObservation>, Observation,
    double, Deadline);

bool FWaitForScheduledBlueprintGarbageCollection::Update()
{
    if (Observation->PreGarbageCollectCount == 0 && GEngine)
    {
        GEngine->ConditionalCollectGarbage();
    }
    if (Observation->PreGarbageCollectCount == 0 && FPlatformTime::Seconds() < Deadline)
    {
        return false;
    }

    Test->TestEqual(TEXT("The scheduled later GC broadcasts pre-GC exactly once"),
        Observation->PreGarbageCollectCount, 1);
    Test->TestEqual(TEXT("Python callback and later GC complete without handled ensures"),
        Observation->HandledEnsureCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFullCompileRoutesSurvivePythonThenLaterGarbageCollectionTest,
    "PinWright.blueprint.compile.FullCompileRoutesSurvivePythonThenLaterGarbageCollection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintFullCompileRoutesSurvivePythonThenLaterGarbageCollectionTest::RunTest(
    const FString& Parameters)
{
    if (!IPythonScriptPlugin::Get())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("python-plugin-not-loaded"),
            TEXT("PythonScriptPlugin is not loaded; the temporal crash sequence cannot be exercised."));
        return true;
    }

    TStrongObjectPtr<UBlueprint> Blueprint(
        CompilerTestUtils::CreateTransientTestBP(TEXT("FullCompileAfterPython")));
    UBlueprint* BP = Blueprint.Get();
    if (!TestNotNull(TEXT("Transient Actor Blueprint created"), BP))
    {
        return true;
    }

    const FName PropertyName(TEXT("bCompileFlag"));
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    if (!TestTrue(TEXT("Boolean member variable added"),
            FBlueprintEditorUtils::AddMemberVariable(BP, PropertyName, BoolPinType)))
    {
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(
        BP, EBlueprintCompileOptions::SkipGarbageCollection);
    if (!TestNotNull(TEXT("Fixture setup produced a generated class"), BP->GeneratedClass.Get()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> PythonPayload = MakeShared<FJsonObject>();
    PythonPayload->SetStringField(TEXT("mode"), TEXT("evaluate_statement"));
    PythonPayload->SetStringField(TEXT("code"), TEXT("1 + 1"));
    FTestResponseCapture PythonCapture;
    if (!TestTrue(TEXT("python.execute handler found"),
            InvokeHandlerWithCapture(TEXT("python.execute"), PythonPayload, PythonCapture)))
    {
        return false;
    }
    if (!PythonCapture.bSuccess)
    {
        if (PythonCapture.ErrorCode == TEXT("PYTHON_NOT_AVAILABLE") ||
            PythonCapture.ErrorCode == TEXT("PYTHON_INIT_FAILED"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("python-interpreter-unavailable"),
                FString::Printf(TEXT("python.execute answered %s"), *PythonCapture.ErrorCode));
            return true;
        }
        AddError(FString::Printf(TEXT("python.execute failed: %s — %s"),
            *PythonCapture.ErrorCode, *PythonCapture.Message));
        return false;
    }
    if (!TestTrue(TEXT("python.execute returned a result"), PythonCapture.Result.IsValid()))
    {
        return false;
    }
    bool bPythonSucceeded = false;
    TestTrue(TEXT("Python evaluation reports success:true"),
        PythonCapture.Result->TryGetBoolField(TEXT("success"), bPythonSucceeded));
    TestTrue(TEXT("Python evaluation succeeded"), bPythonSucceeded);
    FString PythonResult;
    TestTrue(TEXT("Python evaluation response carries result"),
        PythonCapture.Result->TryGetStringField(TEXT("result"), PythonResult));
    TestEqual(TEXT("Python evaluation returns 2"), PythonResult, FString(TEXT("2")));

    if (!TestNotNull(TEXT("Engine available to consume the scheduled full GC"), GEngine))
    {
        return false;
    }
    TSharedRef<BlueprintCompileGarbageCollectionTest::FScopedObservation> Observation =
        MakeShared<BlueprintCompileGarbageCollectionTest::FScopedObservation>();

    FTestResponseCapture CompileCapture;
    TSharedPtr<FJsonObject> CompilePayload = MakeShared<FJsonObject>();
    CompilePayload->SetStringField(TEXT("path"), BP->GetPathName());
    TestTrue(TEXT("blueprint.compile handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile"), CompilePayload, CompileCapture));
    TestTrue(TEXT("blueprint.compile succeeds"), CompileCapture.bSuccess);

    FTestResponseCapture SetDefaultCapture;
    TSharedPtr<FJsonObject> SetDefaultPayload = MakeShared<FJsonObject>();
    SetDefaultPayload->SetStringField(TEXT("path"), BP->GetPathName());
    SetDefaultPayload->SetStringField(TEXT("propertyName"), PropertyName.ToString());
    SetDefaultPayload->SetField(TEXT("value"), MakeShared<FJsonValueBoolean>(true));
    TestTrue(TEXT("blueprint.set_default handler found"),
        InvokeHandlerWithCapture(
            TEXT("blueprint.set_default"), SetDefaultPayload, SetDefaultCapture));
    TestTrue(TEXT("blueprint.set_default succeeds"), SetDefaultCapture.bSuccess);

    if (TestTrue(TEXT("blueprint.compile returned a result"), CompileCapture.Result.IsValid()))
    {
        bool bCompiled = false;
        TestTrue(TEXT("blueprint.compile response carries compiled"),
            CompileCapture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
        TestTrue(TEXT("blueprint.compile reports compiled:true"), bCompiled);
    }

    if (TestTrue(TEXT("blueprint.set_default returned a result"),
            SetDefaultCapture.Result.IsValid()))
    {
        bool bCompiled = false;
        TestTrue(TEXT("blueprint.set_default response carries compiled"),
            SetDefaultCapture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
        TestTrue(TEXT("blueprint.set_default reports compiled:true"), bCompiled);

        bool bValue = false;
        TestTrue(TEXT("blueprint.set_default response carries the read-back value"),
            SetDefaultCapture.Result->TryGetBoolField(TEXT("value"), bValue));
        TestTrue(TEXT("blueprint.set_default reads back the requested Boolean"), bValue);
    }

    // Without SkipGarbageCollection, a handler broadcasts before the scheduled phase below.
    TestEqual(TEXT("Full Blueprint compile handlers do not broadcast pre-GC synchronously"),
        Observation->PreGarbageCollectCount, 0);

    // A later engine GC consumes the full-purge request queued by both handlers.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForScheduledBlueprintGarbageCollection(
        this, Observation, FPlatformTime::Seconds() + 10.0));
    return true;
}
