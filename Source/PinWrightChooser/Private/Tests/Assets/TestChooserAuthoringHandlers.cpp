// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#include "BoolColumn.h"
#include "Chooser.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectChooser_Class.h"
#include "Tests/TestUtils.h"

namespace
{
bool InvokeChooserHandler(
    FAutomationTestBase& Test,
    const FString& Method,
    const TSharedPtr<FJsonObject>& Payload,
    FTestResponseCapture& Capture)
{
    const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
    Test.TestTrue(FString::Printf(TEXT("%s handler found"), *Method), bFound);
    Test.TestTrue(FString::Printf(TEXT("%s sent a response"), *Method), Capture.bWasCalled);
    Test.TestTrue(FString::Printf(TEXT("%s succeeded"), *Method), Capture.bSuccess);
    Test.TestTrue(FString::Printf(TEXT("%s returned a result"), *Method), Capture.Result.IsValid());
    return bFound && Capture.bWasCalled && Capture.bSuccess && Capture.Result.IsValid();
}

bool InvokeChooserHandlerExpectError(
    FAutomationTestBase& Test,
    const FString& Method,
    const TSharedPtr<FJsonObject>& Payload,
    FTestResponseCapture& Capture)
{
    const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
    Test.TestTrue(FString::Printf(TEXT("%s handler found"), *Method), bFound);
    Test.TestTrue(FString::Printf(TEXT("%s sent a response"), *Method), Capture.bWasCalled);
    Test.TestFalse(FString::Printf(TEXT("%s failed"), *Method), Capture.bSuccess);
    Test.TestFalse(FString::Printf(TEXT("%s returned an error code"), *Method), Capture.ErrorCode.IsEmpty());
    return bFound && Capture.bWasCalled && !Capture.bSuccess && !Capture.ErrorCode.IsEmpty();
}

FString MakeChooserPackagePath()
{
    return FString::Printf(
        TEXT("/Game/PinWrightTests/CH_BoolClass_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserAuthoringBoolClassRoundTripTest,
    "PinWright.chooser.BoolClassRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserAuthoringBoolClassRoundTripTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserPackagePath();
    const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    CleanupTestAsset(PackagePath);

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), PackagePath);
    CreatePayload->SetBoolField(TEXT("save"), false);
    if (!InvokeChooserHandler(*this, TEXT("chooser.create"), CreatePayload, Capture))
    {
        return false;
    }

    UChooserTable* Chooser = LoadObject<UChooserTable>(nullptr, *ObjectPath);
    TestNotNull(TEXT("chooser.create creates loadable UChooserTable"), Chooser);
    if (!Chooser)
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddColumnPayload = MakeShared<FJsonObject>();
    AddColumnPayload->SetStringField(TEXT("path"), ObjectPath);
    AddColumnPayload->SetStringField(TEXT("kind"), TEXT("bool"));
    if (!InvokeChooserHandler(*this, TEXT("chooser.add_column"), AddColumnPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    TestEqual(TEXT("add_column returns columnIndex 0"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("columnIndex"))), 0);

    TSharedPtr<FJsonObject> AddRowPayload = MakeShared<FJsonObject>();
    AddRowPayload->SetStringField(TEXT("path"), ObjectPath);
    if (!InvokeChooserHandler(*this, TEXT("chooser.add_row"), AddRowPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    TestEqual(TEXT("add_row returns row 0"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("row"))), 0);

    TSharedPtr<FJsonObject> SetCellPayload = MakeShared<FJsonObject>();
    SetCellPayload->SetStringField(TEXT("path"), ObjectPath);
    SetCellPayload->SetNumberField(TEXT("row"), 0);
    SetCellPayload->SetNumberField(TEXT("column"), 0);
    SetCellPayload->SetBoolField(TEXT("value"), true);
    if (!InvokeChooserHandler(*this, TEXT("chooser.set_cell"), SetCellPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestEqual(TEXT("chooser has one bool column"), Chooser->ColumnsStructs.Num(), 1);
    const FBoolColumn* BoolColumn = Chooser->ColumnsStructs[0].GetPtr<FBoolColumn>();
    TestNotNull(TEXT("stored column is FBoolColumn"), BoolColumn);
    if (BoolColumn)
    {
        TestEqual(TEXT("bool row value count"), BoolColumn->RowValuesWithAny.Num(), 1);
        TestEqual(TEXT("set_cell stores MatchTrue"),
            static_cast<int32>(BoolColumn->RowValuesWithAny[0]),
            static_cast<int32>(EBoolColumnCellValue::MatchTrue));
    }

    TSharedPtr<FJsonObject> SetResultPayload = MakeShared<FJsonObject>();
    SetResultPayload->SetStringField(TEXT("path"), ObjectPath);
    SetResultPayload->SetNumberField(TEXT("row"), 0);
    SetResultPayload->SetStringField(TEXT("resultKind"), TEXT("class"));
    SetResultPayload->SetStringField(TEXT("value"), TEXT("/Script/Engine.Actor"));
    if (!InvokeChooserHandler(*this, TEXT("chooser.set_result"), SetResultPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestEqual(TEXT("chooser has one result row"), Chooser->ResultsStructs.Num(), 1);
    const FClassChooser* ClassChooser = Chooser->ResultsStructs[0].GetPtr<FClassChooser>();
    TestNotNull(TEXT("set_result stores FClassChooser"), ClassChooser);
    if (ClassChooser)
    {
        TestEqual(TEXT("class chooser stores Actor"), ClassChooser->Class.Get(), AActor::StaticClass());
    }

    TSharedPtr<FJsonObject> CompilePayload = MakeShared<FJsonObject>();
    CompilePayload->SetStringField(TEXT("path"), ObjectPath);
    if (!InvokeChooserHandler(*this, TEXT("chooser.compile"), CompilePayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    // UChooserTable::Compile arrived in UE 5.4. On 5.3 the table is evaluated straight from its
    // data, so HandleCompile deliberately reports compiled:false plus a compileSkippedReason
    // (ChooserAuthoringHandler.cpp) rather than claiming a step that never ran. Assert the
    // contract this engine actually publishes instead of skipping the check.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    TestTrue(TEXT("compile reports compiled"), Capture.Result->GetBoolField(TEXT("compiled")));
#else
    TestFalse(TEXT("compile reports not compiled on an engine with no compile step"),
        Capture.Result->GetBoolField(TEXT("compiled")));
    TestFalse(TEXT("and does not claim a clean compile either"),
        Capture.Result->GetBoolField(TEXT("compileClean")));
    TestTrue(TEXT("and names why the step was skipped"),
        Capture.Result->HasTypedField<EJson::String>(TEXT("compileSkippedReason")));
#endif

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserCreateInvalidOutputClassDoesNotAllocateObjectTest,
    "PinWright.chooser.CreateInvalidOutputClassDoesNotAllocateObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserCreateInvalidOutputClassDoesNotAllocateObjectTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserPackagePath();
    const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    CleanupTestAsset(PackagePath);

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), PackagePath);
    CreatePayload->SetStringField(TEXT("outputObjectType"), TEXT("/Script/Engine.DoesNotExist"));
    if (!InvokeChooserHandlerExpectError(*this, TEXT("chooser.create"), CreatePayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestEqual(TEXT("invalid output class reports CLASS_NOT_FOUND"),
        Capture.ErrorCode,
        FString(TEXT("CLASS_NOT_FOUND")));
    TestNull(TEXT("invalid output class does not allocate chooser object"),
        FindObject<UChooserTable>(nullptr, *ObjectPath));

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserAddColumnInvalidTypeFieldsDoNotMutateTest,
    "PinWright.chooser.AddColumnInvalidTypeFieldsDoNotMutate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserAddColumnInvalidTypeFieldsDoNotMutateTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserPackagePath();
    const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    CleanupTestAsset(PackagePath);

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), PackagePath);
    CreatePayload->SetBoolField(TEXT("save"), false);
    if (!InvokeChooserHandler(*this, TEXT("chooser.create"), CreatePayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    UChooserTable* Chooser = LoadObject<UChooserTable>(nullptr, *ObjectPath);
    TestNotNull(TEXT("chooser.create creates loadable UChooserTable"), Chooser);
    if (!Chooser)
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddEnumColumnPayload = MakeShared<FJsonObject>();
    AddEnumColumnPayload->SetStringField(TEXT("path"), ObjectPath);
    AddEnumColumnPayload->SetStringField(TEXT("kind"), TEXT("enum"));
    AddEnumColumnPayload->SetStringField(TEXT("enumType"), TEXT("/Script/Engine.DoesNotExist"));
    if (!InvokeChooserHandlerExpectError(*this, TEXT("chooser.add_column"), AddEnumColumnPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    TestEqual(TEXT("invalid enumType does not add a column"), Chooser->ColumnsStructs.Num(), 0);

    TSharedPtr<FJsonObject> AddObjectColumnPayload = MakeShared<FJsonObject>();
    AddObjectColumnPayload->SetStringField(TEXT("path"), ObjectPath);
    AddObjectColumnPayload->SetStringField(TEXT("kind"), TEXT("object"));
    AddObjectColumnPayload->SetStringField(TEXT("allowedClass"), TEXT("/Script/Engine.DoesNotExist"));
    if (!InvokeChooserHandlerExpectError(*this, TEXT("chooser.add_column"), AddObjectColumnPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    TestEqual(TEXT("invalid allowedClass does not add a column"), Chooser->ColumnsStructs.Num(), 0);

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserSetCellInvalidValueDoesNotResizeRowsTest,
    "PinWright.chooser.SetCellInvalidValueDoesNotResizeRows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserSetCellInvalidValueDoesNotResizeRowsTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserPackagePath();
    const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    CleanupTestAsset(PackagePath);

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), PackagePath);
    CreatePayload->SetBoolField(TEXT("save"), false);
    if (!InvokeChooserHandler(*this, TEXT("chooser.create"), CreatePayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    UChooserTable* Chooser = LoadObject<UChooserTable>(nullptr, *ObjectPath);
    TestNotNull(TEXT("chooser.create creates loadable UChooserTable"), Chooser);
    if (!Chooser)
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddColumnPayload = MakeShared<FJsonObject>();
    AddColumnPayload->SetStringField(TEXT("path"), ObjectPath);
    AddColumnPayload->SetStringField(TEXT("kind"), TEXT("bool"));
    if (!InvokeChooserHandler(*this, TEXT("chooser.add_column"), AddColumnPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddRowPayload = MakeShared<FJsonObject>();
    AddRowPayload->SetStringField(TEXT("path"), ObjectPath);
    if (!InvokeChooserHandler(*this, TEXT("chooser.add_row"), AddRowPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FBoolColumn* BoolColumn = Chooser->ColumnsStructs[0].GetMutablePtr<FBoolColumn>();
    TestNotNull(TEXT("stored column is FBoolColumn"), BoolColumn);
    if (!BoolColumn)
    {
        CleanupTestAsset(PackagePath);
        return false;
    }
    BoolColumn->RowValuesWithAny.Reset();

    TSharedPtr<FJsonObject> SetCellPayload = MakeShared<FJsonObject>();
    SetCellPayload->SetStringField(TEXT("path"), ObjectPath);
    SetCellPayload->SetNumberField(TEXT("row"), 0);
    SetCellPayload->SetNumberField(TEXT("column"), 0);
    SetCellPayload->SetStringField(TEXT("value"), TEXT("not_bool"));
    if (!InvokeChooserHandlerExpectError(*this, TEXT("chooser.set_cell"), SetCellPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestEqual(TEXT("invalid set_cell does not resize bool row values"), BoolColumn->RowValuesWithAny.Num(), 0);

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserSetResultFailurePreservesExistingRowTest,
    "PinWright.chooser.SetResultFailurePreservesExistingRow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserSetResultFailurePreservesExistingRowTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = MakeChooserPackagePath();
    const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    CleanupTestAsset(PackagePath);

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("path"), PackagePath);
    CreatePayload->SetStringField(TEXT("resultType"), TEXT("class"));
    if (!InvokeChooserHandler(*this, TEXT("chooser.create"), CreatePayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    UChooserTable* Chooser = LoadObject<UChooserTable>(nullptr, *ObjectPath);
    TestNotNull(TEXT("chooser.create creates loadable UChooserTable"), Chooser);
    if (!Chooser)
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> AddRowPayload = MakeShared<FJsonObject>();
    AddRowPayload->SetStringField(TEXT("path"), ObjectPath);
    if (!InvokeChooserHandler(*this, TEXT("chooser.add_row"), AddRowPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> SetClassResultPayload = MakeShared<FJsonObject>();
    SetClassResultPayload->SetStringField(TEXT("path"), ObjectPath);
    SetClassResultPayload->SetNumberField(TEXT("row"), 0);
    SetClassResultPayload->SetStringField(TEXT("resultKind"), TEXT("class"));
    SetClassResultPayload->SetStringField(TEXT("value"), TEXT("/Script/Engine.Actor"));
    if (!InvokeChooserHandler(*this, TEXT("chooser.set_result"), SetClassResultPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TSharedPtr<FJsonObject> SetMissingAssetResultPayload = MakeShared<FJsonObject>();
    SetMissingAssetResultPayload->SetStringField(TEXT("path"), ObjectPath);
    SetMissingAssetResultPayload->SetNumberField(TEXT("row"), 0);
    SetMissingAssetResultPayload->SetStringField(TEXT("resultKind"), TEXT("asset"));
    SetMissingAssetResultPayload->SetStringField(TEXT("value"), TEXT("/Game/PinWrightTests/Missing.Missing"));
    if (!InvokeChooserHandlerExpectError(*this, TEXT("chooser.set_result"), SetMissingAssetResultPayload, Capture))
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    TestEqual(TEXT("missing asset result reports NOT_FOUND"),
        Capture.ErrorCode,
        FString(TEXT("NOT_FOUND")));
    TestEqual(TEXT("chooser still has one result row"), Chooser->ResultsStructs.Num(), 1);
    const FClassChooser* ClassChooser = Chooser->ResultsStructs[0].GetPtr<FClassChooser>();
    TestNotNull(TEXT("failed asset result preserves FClassChooser row"), ClassChooser);
    if (ClassChooser)
    {
        TestEqual(TEXT("failed asset result preserves Actor class"), ClassChooser->Class.Get(), AActor::StaticClass());
    }

    CleanupTestAsset(PackagePath);
    return true;
}

// Regression test for E-chooser-column-binding-undocumented: chooser.add_column must surface a
// non-fatal warning when a property-driven column is added without the context class +
// propertyBinding it needs to compile clean — instead of silently succeeding and only failing later
// at chooser.compile with "No Property Bound". A bound column (context class + propertyBinding) and a
// non-property-driven randomize column must NOT emit a warning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserAddColumnUnboundEmitsHintTest,
    "PinWright.chooser.AddColumnUnboundEmitsHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserAddColumnUnboundEmitsHintTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;

    // Building the hint compiles the column binding to read the engine's authoritative per-column
    // verdict (FChooserPropertyBinding::Compile), which UE_ASSET_LOGs "Missing property binding." at
    // Error level for the unbound bool column in Case 1. That engine diagnostic is the expected,
    // desired signal here — declare it so the automation harness does not count it as a failure.
    // The hint (and thus the engine compile + its diagnostic) only exists on UE 5.6+, where
    // FChooserParameterBase::HasCompileErrors is available; pre-5.6 the handler emits no hint and
    // the engine logs nothing, so declaring the expected error there would leave it unmatched.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    AddExpectedError(TEXT("Missing property binding"), EAutomationExpectedErrorFlags::Contains, 1);
#endif

    // Case 1: no contextObjectType on create + no propertyBinding on a bool column -> hint expected.
    {
        const FString PackagePath = MakeChooserPackagePath();
        const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
        CleanupTestAsset(PackagePath);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("path"), PackagePath);
        if (!InvokeChooserHandler(*this, TEXT("chooser.create"), CreatePayload, Capture))
        {
            CleanupTestAsset(PackagePath);
            return false;
        }

        TSharedPtr<FJsonObject> AddColumnPayload = MakeShared<FJsonObject>();
        AddColumnPayload->SetStringField(TEXT("path"), ObjectPath);
        AddColumnPayload->SetStringField(TEXT("kind"), TEXT("bool"));
        if (!InvokeChooserHandler(*this, TEXT("chooser.add_column"), AddColumnPayload, Capture))
        {
            CleanupTestAsset(PackagePath);
            return false;
        }

        // The per-column "unbound" verdict comes from FChooserParameterBase::HasCompileErrors,
        // added in UE 5.6. Pre-5.6 the handler has no authoritative engine verdict to surface, so
        // BuildColumnBindingHint returns empty and no warning is emitted — assert that instead.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        const bool bHasWarnings = Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings && Warnings->Num() > 0;
        TestTrue(TEXT("unbound property-driven column returns a warning"), bHasWarnings);
        if (bHasWarnings)
        {
            TestTrue(TEXT("warning names the No Property Bound failure mode"),
                (*Warnings)[0]->AsString().Contains(TEXT("No Property Bound")));
        }
#else
        TestFalse(TEXT("pre-5.6 has no per-column compile verdict, so no warning is emitted"),
            Capture.Result->HasField(TEXT("warnings")));
#endif

        CleanupTestAsset(PackagePath);
    }

    // Case 2: contextObjectType on create + propertyBinding on the column -> no hint.
    {
        const FString PackagePath = MakeChooserPackagePath();
        const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
        CleanupTestAsset(PackagePath);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("path"), PackagePath);
        CreatePayload->SetStringField(TEXT("contextObjectType"), TEXT("/Script/Engine.StaticMeshComponent"));
        if (!InvokeChooserHandler(*this, TEXT("chooser.create"), CreatePayload, Capture))
        {
            CleanupTestAsset(PackagePath);
            return false;
        }

        TSharedPtr<FJsonObject> AddColumnPayload = MakeShared<FJsonObject>();
        AddColumnPayload->SetStringField(TEXT("path"), ObjectPath);
        AddColumnPayload->SetStringField(TEXT("kind"), TEXT("bool"));
        AddColumnPayload->SetStringField(TEXT("propertyBinding"), TEXT("bVisible"));
        if (!InvokeChooserHandler(*this, TEXT("chooser.add_column"), AddColumnPayload, Capture))
        {
            CleanupTestAsset(PackagePath);
            return false;
        }
        TestFalse(TEXT("bound column emits no warning"), Capture.Result->HasField(TEXT("warnings")));

        // randomize is not property-driven: no warning even with no context + no binding.
        TSharedPtr<FJsonObject> AddRandomizePayload = MakeShared<FJsonObject>();
        AddRandomizePayload->SetStringField(TEXT("path"), ObjectPath);
        AddRandomizePayload->SetStringField(TEXT("kind"), TEXT("randomize"));
        if (!InvokeChooserHandler(*this, TEXT("chooser.add_column"), AddRandomizePayload, Capture))
        {
            CleanupTestAsset(PackagePath);
            return false;
        }
        TestFalse(TEXT("randomize column emits no warning"), Capture.Result->HasField(TEXT("warnings")));

        CleanupTestAsset(PackagePath);
    }

    return true;
}
