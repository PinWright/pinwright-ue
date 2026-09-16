// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the system.inspect.search_classes handler
// (Handlers/System/ClassSearchHandler.cpp).
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

namespace
{
    // Walks a successful search_classes result and returns the matching row, or nullptr.
    static TSharedPtr<FJsonObject> FindRowByClassName(const FTestResponseCapture& Capture, const FString& ClassName)
    {
        if (!Capture.bSuccess || !Capture.Result.IsValid()) return nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("results"), Results) || !Results) return nullptr;
        for (const TSharedPtr<FJsonValue>& Val : *Results)
        {
            const TSharedPtr<FJsonObject> Row = Val->AsObject();
            if (!Row.IsValid()) continue;
            FString Name;
            if (Row->TryGetStringField(TEXT("className"), Name) && Name == ClassName)
            {
                return Row;
            }
        }
        return nullptr;
    }
}

// ============================================================================
// system.inspect.search_classes — Actor subclass search by parent class
// Counterfactual: if parentClass filter is omitted from query-time predicates,
// unrelated classes containing "StaticMesh" (e.g. UStaticMeshComponent) outrank
// AStaticMeshActor and the className assertion fails.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassSearchActorByParentClassTest,
    "PinWright.system.inspect.search_classes.ActorByParentClassReturnsKnownSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClassSearchActorByParentClassTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("StaticMesh"));
    Payload->SetStringField(TEXT("parentClass"), TEXT("Actor"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("system.inspect.search_classes"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    TSharedPtr<FJsonObject> Row = FindRowByClassName(Capture, TEXT("StaticMeshActor"));
    TestTrue(TEXT("StaticMeshActor row present"), Row.IsValid());
    if (Row.IsValid())
    {
        FString FullPath, Module;
        Row->TryGetStringField(TEXT("fullPath"), FullPath);
        Row->TryGetStringField(TEXT("module"), Module);
        TestTrue(TEXT("fullPath starts with /Script/Engine."),
            FullPath.StartsWith(TEXT("/Script/Engine.")));
        TestEqual(TEXT("module is Engine"), Module, FString(TEXT("Engine")));
    }
    return true;
}

// ============================================================================
// system.inspect.search_classes — UMG widget search by name + module filter
// Counterfactual: if moduleFilter case-folding is dropped, matching against the
// lowercased ModuleLower with the raw "UMG" filter mismatches and zero rows
// surface — assertion fails.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassSearchUmgWidgetByNameTest,
    "PinWright.system.inspect.search_classes.UmgTextBlockResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClassSearchUmgWidgetByNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("TextBlock"));
    Payload->SetStringField(TEXT("parentClass"), TEXT("Widget"));
    TArray<TSharedPtr<FJsonValue>> ModuleFilter;
    ModuleFilter.Add(MakeShared<FJsonValueString>(TEXT("UMG")));
    Payload->SetArrayField(TEXT("moduleFilter"), ModuleFilter);

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("system.inspect.search_classes"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    TSharedPtr<FJsonObject> TextBlockRow = FindRowByClassName(Capture, TEXT("TextBlock"));
    TSharedPtr<FJsonObject> RichRow = FindRowByClassName(Capture, TEXT("RichTextBlock"));
    TestTrue(TEXT("TextBlock or RichTextBlock present"), TextBlockRow.IsValid() || RichRow.IsValid());

    const TSharedPtr<FJsonObject> Row = TextBlockRow.IsValid() ? TextBlockRow : RichRow;
    if (Row.IsValid())
    {
        FString Module;
        Row->TryGetStringField(TEXT("module"), Module);
        TestEqual(TEXT("module is UMG"), Module, FString(TEXT("UMG")));
    }
    return true;
}

// ============================================================================
// system.inspect.search_classes — Abstract classes excluded by default
// Counterfactual: if the handler drops the CLASS_Abstract filter, APawn itself
// surfaces in the results and the per-row isAbstract assertion fails.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassSearchExcludesAbstractByDefaultTest,
    "PinWright.system.inspect.search_classes.ExcludesAbstractByDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClassSearchExcludesAbstractByDefaultTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("Pawn"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found"),
        InvokeHandlerWithCapture(TEXT("system.inspect.search_classes"), Payload, Capture));
    TestTrue(TEXT("Response captured"), Capture.bWasCalled);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    TestTrue(TEXT("results array present"),
        Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results);
    if (Results)
    {
        for (const TSharedPtr<FJsonValue>& Val : *Results)
        {
            const TSharedPtr<FJsonObject> Row = Val->AsObject();
            if (!Row.IsValid()) continue;
            bool bIsAbstract = false;
            Row->TryGetBoolField(TEXT("isAbstract"), bIsAbstract);
            TestFalse(TEXT("no row is abstract"), bIsAbstract);
        }
    }
    return true;
}
