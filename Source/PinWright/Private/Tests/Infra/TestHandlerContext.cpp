// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for FHandlerContext typed param getters and require validators
#include "Misc/AutomationTest.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Utils/JsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Helper: create a FHandlerContext with the given Payload (no Subsystem needed
// for getter tests -- SendError gracefully no-ops when Subsystem is null).
// ============================================================================
namespace
{
    FHandlerContext MakeTestContext(const TSharedPtr<FJsonObject>& Payload)
    {
        return FHandlerContext::MakeTestContext(TEXT("test-id"), TEXT("test.method"), Payload);
    }
}

// ============================================================================
// GetString
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetStringTest,
    "PinWright.infra.handler_context.GetString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetStringTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestActor"));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Existing field returns value
    TestEqual(TEXT("GetString returns field value"),
        Ctx.GetString(TEXT("name")), TEXT("TestActor"));

    // Missing field returns default
    TestEqual(TEXT("GetString returns empty default for missing field"),
        Ctx.GetString(TEXT("missing")), TEXT(""));

    // Missing field returns custom default
    TestEqual(TEXT("GetString returns custom default for missing field"),
        Ctx.GetString(TEXT("missing"), TEXT("fallback")), TEXT("fallback"));

    return true;
}

// ============================================================================
// GetNumber
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetNumberTest,
    "PinWright.infra.handler_context.GetNumber",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetNumberTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("scale"), 2.5);

    FHandlerContext Ctx = MakeTestContext(Payload);

    TestEqual(TEXT("GetNumber returns field value"),
        Ctx.GetNumber(TEXT("scale")), 2.5);

    TestEqual(TEXT("GetNumber returns 0 default for missing field"),
        Ctx.GetNumber(TEXT("missing")), 0.0);

    TestEqual(TEXT("GetNumber returns custom default for missing field"),
        Ctx.GetNumber(TEXT("missing"), 99.0), 99.0);

    return true;
}

// ============================================================================
// GetBool
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetBoolTest,
    "PinWright.infra.handler_context.GetBool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetBoolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("visible"), true);

    FHandlerContext Ctx = MakeTestContext(Payload);

    TestTrue(TEXT("GetBool returns true for true field"),
        Ctx.GetBool(TEXT("visible")));

    TestFalse(TEXT("GetBool returns false default for missing field"),
        Ctx.GetBool(TEXT("missing")));

    TestTrue(TEXT("GetBool returns custom default for missing field"),
        Ctx.GetBool(TEXT("missing"), true));

    return true;
}

// ============================================================================
// GetInt
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetIntTest,
    "PinWright.infra.handler_context.GetInt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetIntTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("count"), 42.0);

    FHandlerContext Ctx = MakeTestContext(Payload);

    TestEqual(TEXT("GetInt returns field value"),
        Ctx.GetInt(TEXT("count")), 42);

    TestEqual(TEXT("GetInt returns 0 default for missing field"),
        Ctx.GetInt(TEXT("missing")), 0);

    TestEqual(TEXT("GetInt returns custom default for missing field"),
        Ctx.GetInt(TEXT("missing"), -1), -1);

    return true;
}

// ============================================================================
// GetInt strict rejection
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetIntStrictTest,
    "PinWright.infra.handler_context.GetIntRejectsFractionalAndOutOfRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetIntStrictTest::RunTest(const FString& Parameters)
{
    auto AssertRejected = [this](double Value, const FString& Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("count"), Value);
        FTestResponseCapture Capture;
        FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
            TEXT("get-int-strict"), TEXT("test.method"), Payload, &Capture);

        TestEqual(*FString::Printf(TEXT("%s returns the fallback"), *Label),
            Ctx.GetInt(TEXT("count"), 17), 17);
        TestTrue(*FString::Printf(TEXT("%s sends a response"), *Label), Capture.bWasCalled);
        TestFalse(*FString::Printf(TEXT("%s is rejected"), *Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s uses INVALID_PARAMS"), *Label),
            Capture.ErrorCode, ErrorCodes::ERR_INVALID_PARAMS);
    };

    AssertRejected(1.5, TEXT("fractional value"));
    AssertRejected(2147483648.0, TEXT("positive overflow"));
    AssertRejected(-2147483649.0, TEXT("negative overflow"));

    TSharedPtr<FJsonObject> ValidPayload = MakeShared<FJsonObject>();
    ValidPayload->SetNumberField(TEXT("count"), 7.0);
    FTestResponseCapture ValidCapture;
    FHandlerContext ValidCtx = FHandlerContext::MakeTestContextWithCapture(
        TEXT("get-int-valid"), TEXT("test.method"), ValidPayload, &ValidCapture);
    const TOptional<int32> Valid = ValidCtx.GetIntOr(TEXT("count"));
    TestTrue(TEXT("integral value is returned by GetIntOr"), Valid.IsSet());
    TestEqual(TEXT("integral value is preserved"), Valid.GetValue(), 7);
    TestFalse(TEXT("valid GetIntOr does not send an error"), ValidCapture.bWasCalled);

    TSharedPtr<FJsonObject> InvalidOptionalPayload = MakeShared<FJsonObject>();
    InvalidOptionalPayload->SetNumberField(TEXT("count"), 1.5);
    FTestResponseCapture InvalidOptionalCapture;
    FHandlerContext InvalidOptionalCtx = FHandlerContext::MakeTestContextWithCapture(
        TEXT("get-int-or-invalid"), TEXT("test.method"), InvalidOptionalPayload,
        &InvalidOptionalCapture);
    const TOptional<int32> Invalid = InvalidOptionalCtx.GetIntOr(TEXT("count"));
    TestFalse(TEXT("fractional GetIntOr is unset"), Invalid.IsSet());
    TestEqual(TEXT("fractional GetIntOr uses INVALID_PARAMS"),
        InvalidOptionalCapture.ErrorCode, ErrorCodes::ERR_INVALID_PARAMS);

    return true;
}

// ============================================================================
// GetVector
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetVectorTest,
    "PinWright.infra.handler_context.GetVector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetVectorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    // Add a valid vector as sub-object with x, y, z keys
    TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>();
    VecObj->SetNumberField(TEXT("x"), 1.0);
    VecObj->SetNumberField(TEXT("y"), 2.0);
    VecObj->SetNumberField(TEXT("z"), 3.0);
    Payload->SetObjectField(TEXT("location"), VecObj);

    FHandlerContext Ctx = MakeTestContext(Payload);

    FVector Result = Ctx.GetVector(TEXT("location"));
    TestEqual(TEXT("GetVector X"), (double)Result.X, 1.0);
    TestEqual(TEXT("GetVector Y"), (double)Result.Y, 2.0);
    TestEqual(TEXT("GetVector Z"), (double)Result.Z, 3.0);

    // Missing field returns default
    FVector Default(10.0, 20.0, 30.0);
    FVector Missing = Ctx.GetVector(TEXT("missing"), Default);
    TestEqual(TEXT("GetVector missing X"), (double)Missing.X, 10.0);
    TestEqual(TEXT("GetVector missing Y"), (double)Missing.Y, 20.0);
    TestEqual(TEXT("GetVector missing Z"), (double)Missing.Z, 30.0);

    // Zero default
    FVector Zero = Ctx.GetVector(TEXT("missing"));
    TestEqual(TEXT("GetVector zero default X"), (double)Zero.X, 0.0);
    TestEqual(TEXT("GetVector zero default Y"), (double)Zero.Y, 0.0);
    TestEqual(TEXT("GetVector zero default Z"), (double)Zero.Z, 0.0);

    return true;
}

// ============================================================================
// GetRotator
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetRotatorTest,
    "PinWright.infra.handler_context.GetRotator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetRotatorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    RotObj->SetNumberField(TEXT("pitch"), 15.0);
    RotObj->SetNumberField(TEXT("yaw"), 90.0);
    RotObj->SetNumberField(TEXT("roll"), 45.0);
    Payload->SetObjectField(TEXT("rotation"), RotObj);

    FHandlerContext Ctx = MakeTestContext(Payload);

    FRotator Result = Ctx.GetRotator(TEXT("rotation"));
    TestEqual(TEXT("GetRotator Pitch"), (double)Result.Pitch, 15.0);
    TestEqual(TEXT("GetRotator Yaw"), (double)Result.Yaw, 90.0);
    TestEqual(TEXT("GetRotator Roll"), (double)Result.Roll, 45.0);

    // Missing field returns default
    FRotator Default(5.0, 10.0, 15.0);
    FRotator Missing = Ctx.GetRotator(TEXT("missing"), Default);
    TestEqual(TEXT("GetRotator missing Pitch"), (double)Missing.Pitch, 5.0);
    TestEqual(TEXT("GetRotator missing Yaw"), (double)Missing.Yaw, 10.0);
    TestEqual(TEXT("GetRotator missing Roll"), (double)Missing.Roll, 15.0);

    // Zero default
    FRotator Zero = Ctx.GetRotator(TEXT("missing"));
    TestEqual(TEXT("GetRotator zero default Pitch"), (double)Zero.Pitch, 0.0);
    TestEqual(TEXT("GetRotator zero default Yaw"), (double)Zero.Yaw, 0.0);
    TestEqual(TEXT("GetRotator zero default Roll"), (double)Zero.Roll, 0.0);

    return true;
}

// ============================================================================
// GetObject
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetObjectTest,
    "PinWright.infra.handler_context.GetObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
    Inner->SetStringField(TEXT("key"), TEXT("value"));
    Payload->SetObjectField(TEXT("config"), Inner);

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Existing object field
    TSharedPtr<FJsonObject> Result = Ctx.GetObject(TEXT("config"));
    TestTrue(TEXT("GetObject returns valid pointer"), Result.IsValid());
    TestEqual(TEXT("GetObject inner value"),
        Result->GetStringField(TEXT("key")), TEXT("value"));

    // Missing field
    TSharedPtr<FJsonObject> Missing = Ctx.GetObject(TEXT("missing"));
    TestFalse(TEXT("GetObject returns null for missing field"), Missing.IsValid());

    return true;
}

// ============================================================================
// GetArray
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetArrayTest,
    "PinWright.infra.handler_context.GetArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetArrayTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueString>(TEXT("a")));
    Arr.Add(MakeShared<FJsonValueString>(TEXT("b")));
    Payload->SetArrayField(TEXT("items"), Arr);

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Existing array field
    const TArray<TSharedPtr<FJsonValue>>* Result = Ctx.GetArray(TEXT("items"));
    TestTrue(TEXT("GetArray returns non-null for existing field"), Result != nullptr);
    if (Result)
    {
        TestEqual(TEXT("GetArray has 2 elements"), Result->Num(), 2);
    }

    // Missing field
    const TArray<TSharedPtr<FJsonValue>>* Missing = Ctx.GetArray(TEXT("missing"));
    TestTrue(TEXT("GetArray returns null for missing field"), Missing == nullptr);

    return true;
}

// ============================================================================
// RequireString
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireStringTest,
    "PinWright.infra.handler_context.RequireString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireStringTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestActor"));
    Payload->SetStringField(TEXT("empty"), TEXT(""));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Present and valid
    FString Out;
    bool bResult = Ctx.RequireString(TEXT("name"), Out);
    TestTrue(TEXT("RequireString succeeds for present field"), bResult);
    TestEqual(TEXT("RequireString output value"), Out, TEXT("TestActor"));

    // Missing field produces error (returns false)
    FString MissingOut;
    bool bMissing = Ctx.RequireString(TEXT("nonexistent"), MissingOut);
    TestFalse(TEXT("RequireString fails for missing field"), bMissing);

    // Empty string produces error (returns false)
    FString EmptyOut;
    bool bEmpty = Ctx.RequireString(TEXT("empty"), EmptyOut);
    TestFalse(TEXT("RequireString fails for empty string field"), bEmpty);

    return true;
}

// ============================================================================
// RequireInt
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireIntTest,
    "PinWright.infra.handler_context.RequireInt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireIntTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("count"), 7.0);

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Present and valid
    int32 Out = 0;
    bool bResult = Ctx.RequireInt(TEXT("count"), Out);
    TestTrue(TEXT("RequireInt succeeds for present field"), bResult);
    TestEqual(TEXT("RequireInt output value"), Out, 7);

    // Missing field
    int32 MissingOut = -1;
    bool bMissing = Ctx.RequireInt(TEXT("nonexistent"), MissingOut);
    TestFalse(TEXT("RequireInt fails for missing field"), bMissing);

    return true;
}

// ============================================================================
// RequireAssetPath.GamePathAndTraversal - /Game path accepted, traversal + missing rejected
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireAssetPathGamePathTraversalTest,
    "PinWright.infra.handler_context.RequireAssetPath.GamePathAndTraversal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireAssetPathGamePathTraversalTest::RunTest(const FString& Parameters)
{
    // Valid path
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), TEXT("/Game/MyFolder/MyAsset"));

        FHandlerContext Ctx = MakeTestContext(Payload);

        FString Out;
        bool bResult = Ctx.RequireAssetPath(TEXT("path"), Out);
        TestTrue(TEXT("RequireAssetPath succeeds for valid /Game path"), bResult);
        TestTrue(TEXT("RequireAssetPath output starts with /Game"),
            Out.StartsWith(TEXT("/Game")));
    }

    // Traversal attack
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), TEXT("/Game/../../../etc/passwd"));

        FHandlerContext Ctx = MakeTestContext(Payload);

        FString Out;
        bool bResult = Ctx.RequireAssetPath(TEXT("path"), Out);
        TestFalse(TEXT("RequireAssetPath rejects traversal path"), bResult);
    }

    // Missing field
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

        FHandlerContext Ctx = MakeTestContext(Payload);

        FString Out;
        bool bResult = Ctx.RequireAssetPath(TEXT("path"), Out);
        TestFalse(TEXT("RequireAssetPath fails for missing field"), bResult);
    }

    return true;
}

// ============================================================================
// NullPayload.GetString — null Payload returns default
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadGetStringTest,
    "PinWright.infra.handler_context.NullPayload.GetString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadGetStringTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    TestEqual(TEXT("Returns default"), Ctx.GetString(TEXT("any"), TEXT("def")), TEXT("def"));
    return true;
}

// ============================================================================
// NullPayload.GetNumber
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadGetNumberTest,
    "PinWright.infra.handler_context.NullPayload.GetNumber",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadGetNumberTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    TestEqual(TEXT("Returns default"), Ctx.GetNumber(TEXT("any"), 42.0), 42.0);
    return true;
}

// ============================================================================
// NullPayload.RequireString — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireStringTest,
    "PinWright.infra.handler_context.NullPayload.RequireString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireStringTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    FString Out;
    TestFalse(TEXT("RequireString fails with null payload"), Ctx.RequireString(TEXT("key"), Out));
    return true;
}

// ============================================================================
// NullPayload.RequireInt — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireIntTest,
    "PinWright.infra.handler_context.NullPayload.RequireInt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireIntTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    int32 Out = 0;
    TestFalse(TEXT("RequireInt fails with null payload"), Ctx.RequireInt(TEXT("key"), Out));
    return true;
}

// ============================================================================
// NullPayload.RequireAssetPath — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireAssetPathTest,
    "PinWright.infra.handler_context.NullPayload.RequireAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireAssetPathTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    FString Out;
    TestFalse(TEXT("RequireAssetPath fails with null payload"), Ctx.RequireAssetPath(TEXT("key"), Out));
    return true;
}

// ============================================================================
// SendSuccess.NullSubsystem — no crash
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextSendSuccessNullSubsystemTest,
    "PinWright.infra.handler_context.SendSuccess.NullSubsystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextSendSuccessNullSubsystemTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(
        TEXT("t"), TEXT("m"), MakeShared<FJsonObject>(), nullptr);
    // Should not crash — SendSuccess no-ops when Subsystem is null
    Ctx.SendSuccess(TEXT("ok"));
    TestTrue(TEXT("No crash on SendSuccess with null subsystem"), true);
    return true;
}

// ============================================================================
// SendError.NullSubsystem — no crash
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextSendErrorNullSubsystemTest,
    "PinWright.infra.handler_context.SendError.NullSubsystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextSendErrorNullSubsystemTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(
        TEXT("t"), TEXT("m"), MakeShared<FJsonObject>(), nullptr);
    Ctx.SendError(TEXT("ERR"), TEXT("test error"));
    TestTrue(TEXT("No crash on SendError with null subsystem"), true);
    return true;
}

// ============================================================================
// TypeMismatch.GetObjectOnString — GetObject on string field returns nullptr
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetObjectOnStringTest,
    "PinWright.infra.handler_context.TypeMismatch.GetObjectOnString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetObjectOnStringTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("value"));

    FHandlerContext Ctx = MakeTestContext(Payload);
    TSharedPtr<FJsonObject> Result = Ctx.GetObject(TEXT("name"));
    TestFalse(TEXT("GetObject on string field returns null"), Result.IsValid());
    return true;
}

// ============================================================================
// TypeMismatch.GetArrayOnNumber — GetArray on number field returns nullptr
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetArrayOnNumberTest,
    "PinWright.infra.handler_context.TypeMismatch.GetArrayOnNumber",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetArrayOnNumberTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("count"), 5.0);

    FHandlerContext Ctx = MakeTestContext(Payload);
    const TArray<TSharedPtr<FJsonValue>>* Result = Ctx.GetArray(TEXT("count"));
    TestTrue(TEXT("GetArray on number field returns null"), Result == nullptr);
    return true;
}

// ============================================================================
// RequireAssetPath.EnginePath — /Engine/... paths are accepted
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireAssetPathEngineTest,
    "PinWright.infra.handler_context.RequireAssetPath.EnginePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireAssetPathEngineTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("/Engine/Materials/Default"));

    FHandlerContext Ctx = MakeTestContext(Payload);
    FString Out;
    bool bResult = Ctx.RequireAssetPath(TEXT("path"), Out);
    TestTrue(TEXT("Engine path accepted"), bResult);
    return true;
}

// ============================================================================
// RequireAssetPath.OnlySlashes — "///" sanitizes to empty, returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireAssetPathSlashesTest,
    "PinWright.infra.handler_context.RequireAssetPath.OnlySlashes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireAssetPathSlashesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), TEXT("///"));

    FHandlerContext Ctx = MakeTestContext(Payload);
    FString Out;
    bool bResult = Ctx.RequireAssetPath(TEXT("path"), Out);
    TestFalse(TEXT("Only-slashes path rejected"), bResult);
    return true;
}

// ============================================================================
// RequireNumber
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireNumberTest,
    "PinWright.infra.handler_context.RequireNumber",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireNumberTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("scale"), 3.14);
    Payload->SetStringField(TEXT("name"), TEXT("not_a_number"));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Present and valid
    double Out = 0.0;
    bool bResult = Ctx.RequireNumber(TEXT("scale"), Out);
    TestTrue(TEXT("RequireNumber succeeds for present field"), bResult);
    TestEqual(TEXT("RequireNumber output value"), Out, 3.14);

    // Missing field
    double MissingOut = 0.0;
    bool bMissing = Ctx.RequireNumber(TEXT("nonexistent"), MissingOut);
    TestFalse(TEXT("RequireNumber fails for missing field"), bMissing);

    // Wrong type (string instead of number)
    double WrongOut = 0.0;
    bool bWrong = Ctx.RequireNumber(TEXT("name"), WrongOut);
    TestFalse(TEXT("RequireNumber fails for string field"), bWrong);

    return true;
}

// ============================================================================
// RequireBool
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireBoolTest,
    "PinWright.infra.handler_context.RequireBool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireBoolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), true);
    Payload->SetStringField(TEXT("name"), TEXT("not_a_bool"));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Present and valid
    bool Out = false;
    bool bResult = Ctx.RequireBool(TEXT("enabled"), Out);
    TestTrue(TEXT("RequireBool succeeds for present field"), bResult);
    TestTrue(TEXT("RequireBool output value is true"), Out);

    // Missing field
    bool MissingOut = false;
    bool bMissing = Ctx.RequireBool(TEXT("nonexistent"), MissingOut);
    TestFalse(TEXT("RequireBool fails for missing field"), bMissing);

    // Wrong type (string instead of bool)
    bool WrongOut = false;
    bool bWrong = Ctx.RequireBool(TEXT("name"), WrongOut);
    TestFalse(TEXT("RequireBool fails for string field"), bWrong);

    return true;
}

// ============================================================================
// RequireObject
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireObjectTest,
    "PinWright.infra.handler_context.RequireObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
    Inner->SetStringField(TEXT("key"), TEXT("value"));
    Payload->SetObjectField(TEXT("config"), Inner);
    Payload->SetStringField(TEXT("name"), TEXT("not_an_object"));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Present and valid
    TSharedPtr<FJsonObject> Out;
    bool bResult = Ctx.RequireObject(TEXT("config"), Out);
    TestTrue(TEXT("RequireObject succeeds for present field"), bResult);
    TestTrue(TEXT("RequireObject output is valid"), Out.IsValid());
    TestEqual(TEXT("RequireObject inner value"),
        Out->GetStringField(TEXT("key")), TEXT("value"));

    // Missing field
    TSharedPtr<FJsonObject> MissingOut;
    bool bMissing = Ctx.RequireObject(TEXT("nonexistent"), MissingOut);
    TestFalse(TEXT("RequireObject fails for missing field"), bMissing);

    // Wrong type (string instead of object)
    TSharedPtr<FJsonObject> WrongOut;
    bool bWrong = Ctx.RequireObject(TEXT("name"), WrongOut);
    TestFalse(TEXT("RequireObject fails for string field"), bWrong);

    return true;
}

// ============================================================================
// RequireArray
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextRequireArrayTest,
    "PinWright.infra.handler_context.RequireArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextRequireArrayTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueString>(TEXT("a")));
    Arr.Add(MakeShared<FJsonValueString>(TEXT("b")));
    Payload->SetArrayField(TEXT("items"), Arr);
    Payload->SetStringField(TEXT("name"), TEXT("not_an_array"));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // Present and valid
    const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
    bool bResult = Ctx.RequireArray(TEXT("items"), Out);
    TestTrue(TEXT("RequireArray succeeds for present field"), bResult);
    TestTrue(TEXT("RequireArray output is non-null"), Out != nullptr);
    if (Out)
    {
        TestEqual(TEXT("RequireArray has 2 elements"), Out->Num(), 2);
    }

    // Missing field
    const TArray<TSharedPtr<FJsonValue>>* MissingOut = nullptr;
    bool bMissing = Ctx.RequireArray(TEXT("nonexistent"), MissingOut);
    TestFalse(TEXT("RequireArray fails for missing field"), bMissing);

    // Wrong type (string instead of array)
    const TArray<TSharedPtr<FJsonValue>>* WrongOut = nullptr;
    bool bWrong = Ctx.RequireArray(TEXT("name"), WrongOut);
    TestFalse(TEXT("RequireArray fails for string field"), bWrong);

    return true;
}

// ============================================================================
// GetStringFirstOf
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextGetStringFirstOfTest,
    "PinWright.infra.handler_context.GetStringFirstOf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextGetStringFirstOfTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("class_name"), TEXT("StaticMeshActor"));
    Payload->SetStringField(TEXT("className"), TEXT("PointLight"));

    FHandlerContext Ctx = MakeTestContext(Payload);

    // First key exists — returns its value
    {
        TArray<FString> Keys = { TEXT("class_name"), TEXT("className") };
        FString Result = Ctx.GetStringFirstOf(Keys);
        TestEqual(TEXT("GetStringFirstOf returns first match"),
            Result, TEXT("StaticMeshActor"));
    }

    // First key missing, second exists — returns second
    {
        TArray<FString> Keys = { TEXT("classPath"), TEXT("className") };
        FString Result = Ctx.GetStringFirstOf(Keys);
        TestEqual(TEXT("GetStringFirstOf falls through to second key"),
            Result, TEXT("PointLight"));
    }

    // No keys exist — returns default
    {
        TArray<FString> Keys = { TEXT("foo"), TEXT("bar") };
        FString Result = Ctx.GetStringFirstOf(Keys, TEXT("fallback"));
        TestEqual(TEXT("GetStringFirstOf returns default when no keys match"),
            Result, TEXT("fallback"));
    }

    // No keys exist, no custom default — returns empty
    {
        TArray<FString> Keys = { TEXT("foo"), TEXT("bar") };
        FString Result = Ctx.GetStringFirstOf(Keys);
        TestEqual(TEXT("GetStringFirstOf returns empty when no keys match and no default"),
            Result, TEXT(""));
    }

    return true;
}

// ============================================================================
// NullPayload.RequireNumber — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireNumberTest,
    "PinWright.infra.handler_context.NullPayload.RequireNumber",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireNumberTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    double Out = 0.0;
    TestFalse(TEXT("RequireNumber fails with null payload"), Ctx.RequireNumber(TEXT("key"), Out));
    return true;
}

// ============================================================================
// NullPayload.RequireBool — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireBoolTest,
    "PinWright.infra.handler_context.NullPayload.RequireBool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireBoolTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    bool Out = false;
    TestFalse(TEXT("RequireBool fails with null payload"), Ctx.RequireBool(TEXT("key"), Out));
    return true;
}

// ============================================================================
// NullPayload.RequireObject — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireObjectTest,
    "PinWright.infra.handler_context.NullPayload.RequireObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireObjectTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    TSharedPtr<FJsonObject> Out;
    TestFalse(TEXT("RequireObject fails with null payload"), Ctx.RequireObject(TEXT("key"), Out));
    return true;
}

// ============================================================================
// NullPayload.RequireArray — returns false
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHandlerContextNullPayloadRequireArrayTest,
    "PinWright.infra.handler_context.NullPayload.RequireArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHandlerContextNullPayloadRequireArrayTest::RunTest(const FString& Parameters)
{
    FHandlerContext Ctx = FHandlerContext::MakeTestContext(TEXT("t"), TEXT("m"), nullptr);
    const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
    TestFalse(TEXT("RequireArray fails with null payload"), Ctx.RequireArray(TEXT("key"), Out));
    return true;
}
