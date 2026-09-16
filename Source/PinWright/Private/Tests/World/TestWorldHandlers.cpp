// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for World domain handlers (WorldPartitionHandler.cpp)
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

// ============================================================================
// world_partition.load_cells
// All params are optional, so a single no-crash test with an empty payload is
// sufficient to verify the handler is registered and does not throw.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldLoadCellsValidParamsNoCrashTest,
    "PinWright.world_partition.load_cells.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldLoadCellsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Empty payload — both origin and extent are optional and will use defaults
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("world_partition.load_cells handler is registered and callable"),
        InvokeHandler(TEXT("world_partition.load_cells"), Payload));
    return true;
}

// ============================================================================
// world_partition.load_cells — with explicit origin and extent arrays
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldLoadCellsWithRegionNoCrashTest,
    "PinWright.world_partition.load_cells.WithRegionNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldLoadCellsWithRegionNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    // Provide a concrete bounding region to exercise the array-parsing branch
    TArray<TSharedPtr<FJsonValue>> OriginArr = {
        MakeShared<FJsonValueNumber>(0.0),
        MakeShared<FJsonValueNumber>(0.0),
        MakeShared<FJsonValueNumber>(0.0)
    };
    TArray<TSharedPtr<FJsonValue>> ExtentArr = {
        MakeShared<FJsonValueNumber>(10000.0),
        MakeShared<FJsonValueNumber>(10000.0),
        MakeShared<FJsonValueNumber>(10000.0)
    };
    Payload->SetArrayField(TEXT("origin"), OriginArr);
    Payload->SetArrayField(TEXT("extent"), ExtentArr);

    TestTrue(TEXT("world_partition.load_cells handler found with region payload"),
        InvokeHandler(TEXT("world_partition.load_cells"), Payload));
    return true;
}

// ============================================================================
// world_partition.create_datalayer — valid params, no crash
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldCreateDatalayerValidParamsNoCrashTest,
    "PinWright.world_partition.create_datalayer.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldCreateDatalayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("TestLayer_AutoTest"));

    TestTrue(TEXT("world_partition.create_datalayer handler found with valid payload"),
        InvokeHandler(TEXT("world_partition.create_datalayer"), Payload));
    return true;
}

// ============================================================================
// world_partition.set_datalayer — both required params absent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldSetDatalayerMissingBothParamsTest,
    "PinWright.world_partition.set_datalayer.MissingBothRequiredParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldSetDatalayerMissingBothParamsTest::RunTest(const FString& Parameters)
{
    // Omit both "actorPath" and "dataLayerName"
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("world_partition.set_datalayer handler is registered and callable"),
        InvokeHandler(TEXT("world_partition.set_datalayer"), Payload));
    return true;
}

// ============================================================================
// world_partition.set_datalayer — actorPath present, dataLayerName absent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldSetDatalayerMissingDatalayerNameTest,
    "PinWright.world_partition.set_datalayer.MissingDatalayerName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldSetDatalayerMissingDatalayerNameTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("/Game/Maps/TestLevel.TestLevel:PersistentLevel.StaticMeshActor_0"));
    // "dataLayerName" intentionally omitted

    TestTrue(TEXT("world_partition.set_datalayer handler found with only actorPath"),
        InvokeHandler(TEXT("world_partition.set_datalayer"), Payload));
    return true;
}

// ============================================================================
// world_partition.set_datalayer — dataLayerName present, actorPath absent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldSetDatalayerMissingActorPathTest,
    "PinWright.world_partition.set_datalayer.MissingActorPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldSetDatalayerMissingActorPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // "actorPath" intentionally omitted
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("TestLayer_AutoTest"));

    TestTrue(TEXT("world_partition.set_datalayer handler found with only dataLayerName"),
        InvokeHandler(TEXT("world_partition.set_datalayer"), Payload));
    return true;
}

// ============================================================================
// world_partition.set_datalayer — valid params, no crash
// The handler will fail to find the actor in a test environment; what matters
// is that no exception or access violation occurs.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldSetDatalayerValidParamsNoCrashTest,
    "PinWright.world_partition.set_datalayer.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldSetDatalayerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorPath"), TEXT("/Game/Maps/TestLevel.TestLevel:PersistentLevel.StaticMeshActor_0"));
    Payload->SetStringField(TEXT("dataLayerName"), TEXT("TestLayer_AutoTest"));

    TestTrue(TEXT("world_partition.set_datalayer handler found with all required params"),
        InvokeHandler(TEXT("world_partition.set_datalayer"), Payload));
    return true;
}

// ============================================================================
// world_partition.cleanup_invalid_datalayers — no params required
// The handler walks data layer instances and removes invalid ones; it must not
// crash when called with an empty payload in a test context.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldCleanupInvalidDatalayersValidParamsNoCrashTest,
    "PinWright.world_partition.cleanup_invalid_datalayers.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWorldCleanupInvalidDatalayersValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("world_partition.cleanup_invalid_datalayers handler is registered and callable"),
        InvokeHandler(TEXT("world_partition.cleanup_invalid_datalayers"), Payload));
    return true;
}
