// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the Niagara parameter `type` vocabulary
// (B-niagara-set-parameter-type-name-rejects-its-own-type).
//
// The defect had two halves, and both came from the same root: the wire `type` was interpreted
// twice, by two tables that disagreed.
//
//  1. `niagara.inspect` reports a parameter's type as `{"name": "NiagaraInt32", "struct":
//     "/Script/Niagara.NiagaraInt32"}`. Feeding that name back to `niagara.set_parameter` failed
//     twice: NiagaraEditTypes.cpp's ValidateTypedValue keyed off the raw string, matched no scalar
//     branch, fell through to "is it a registered script struct?" -- true, NiagaraInt32 IS a
//     struct -- and demanded a JSON object for an int; and NiagaraEditHandler.cpp then resolved
//     the same string through a registry walk that matches candidates by their STORAGE struct, so
//     every registered ENUM type (whose storage struct is FNiagaraInt32) answers to "NiagaraInt32"
//     and to "/Script/Niagara.NiagaraInt32", making the winner registration order. When an enum
//     won, the type comparison failed and the message printed the existing type's display name
//     against the caller's RAW request string -- "exists with type 'NiagaraInt32', not requested
//     type 'NiagaraInt32'". Only the undocumented alias "int32" worked.
//  2. The same ticket reported an inspect `type` OBJECT answering `success: true` and writing
//     nothing. That shape is refused twice on this tree and is asserted below so it stays refused.
//
// Every assertion here runs against production code: PinWrightNiagara::ResolveNiagaraParameterType
// (the single resolver both verbs now share), the registered handlers, and a real FRpcDispatcher.

#include "Misc/AutomationTest.h"

#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraParameterTypeResolver.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with identically-shaped helpers in the sibling
// Niagara tests.
namespace NiagaraParameterTypeNamesTestLocal
{
    TSharedPtr<FJsonObject> MakeUserScopePayload(const FString& AssetPath, const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);
        Params->SetStringField(TEXT("scope"), TEXT("user"));
        Params->SetStringField(TEXT("name"), Name);
        Params->SetBoolField(TEXT("compile"), false);
        Params->SetBoolField(TEXT("save"), false);
        return Params;
    }

    // The `type` object niagara.inspect returns, field for field (NiagaraJsonHelpers::BuildTypeModel).
    TSharedPtr<FJsonObject> MakeInspectTypeObject()
    {
        TSharedPtr<FJsonObject> TypeObject = MakeShared<FJsonObject>();
        TypeObject->SetStringField(TEXT("name"), TEXT("NiagaraInt32"));
        TypeObject->SetNumberField(TEXT("sizeBytes"), 4);
        TypeObject->SetBoolField(TEXT("isDataInterface"), false);
        TypeObject->SetBoolField(TEXT("isUObject"), false);
        TypeObject->SetBoolField(TEXT("isEnum"), false);
        TypeObject->SetStringField(TEXT("struct"), TEXT("/Script/Niagara.NiagaraInt32"));
        return TypeObject;
    }
}

// ============================================================================
// 1. Resolver: every canonical name niagara.inspect prints resolves to the engine definition it
//    names, with the value kind that decides the accepted value shape. The half-vector types are
//    the load-bearing rows -- FNiagaraTypeRegistry never registers HalfVec2/3/4, so nothing but
//    the alias table can answer their canonical names, and the struct-path row is what proves an
//    enum can no longer answer for the int32 storage struct.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraParameterTypeNamesResolveTest,
    "PinWright.niagara.parameter_types.InspectNamesResolveToTheirEngineTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraParameterTypeNamesResolveTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagara;

    struct FCase
    {
        const TCHAR* TypeName;
        FNiagaraTypeDefinition Expected;
        ENiagaraParameterValueKind Kind;
    };

    const TArray<FCase> Cases =
    {
        {TEXT("NiagaraFloat"), FNiagaraTypeDefinition::GetFloatDef(), ENiagaraParameterValueKind::Float},
        {TEXT("NiagaraInt32"), FNiagaraTypeDefinition::GetIntDef(), ENiagaraParameterValueKind::Int},
        {TEXT("NiagaraBool"), FNiagaraTypeDefinition::GetBoolDef(), ENiagaraParameterValueKind::Bool},
        {TEXT("Vector2f"), FNiagaraTypeDefinition::GetVec2Def(), ENiagaraParameterValueKind::Vec2},
        {TEXT("Vector3f"), FNiagaraTypeDefinition::GetVec3Def(), ENiagaraParameterValueKind::Vec3},
        {TEXT("LinearColor"), FNiagaraTypeDefinition::GetColorDef(), ENiagaraParameterValueKind::LinearColor},
        {TEXT("NiagaraPosition"), FNiagaraTypeDefinition::GetPositionDef(), ENiagaraParameterValueKind::Position},
        {TEXT("NiagaraID"), FNiagaraTypeDefinition::GetIDDef(), ENiagaraParameterValueKind::NiagaraID},
        {TEXT("NiagaraHalf"), FNiagaraTypeDefinition::GetHalfDef(), ENiagaraParameterValueKind::Half},
        {TEXT("NiagaraHalfVector2"), FNiagaraTypeDefinition::GetHalfVec2Def(), ENiagaraParameterValueKind::HalfVec2},
        {TEXT("NiagaraHalfVector3"), FNiagaraTypeDefinition::GetHalfVec3Def(), ENiagaraParameterValueKind::HalfVec3},
        {TEXT("NiagaraHalfVector4"), FNiagaraTypeDefinition::GetHalfVec4Def(), ENiagaraParameterValueKind::HalfVec4},
        // The short aliases the verbs have always accepted must keep resolving to the same types.
        {TEXT("int32"), FNiagaraTypeDefinition::GetIntDef(), ENiagaraParameterValueKind::Int},
        {TEXT("float"), FNiagaraTypeDefinition::GetFloatDef(), ENiagaraParameterValueKind::Float},
        {TEXT("vec3"), FNiagaraTypeDefinition::GetVec3Def(), ENiagaraParameterValueKind::Vec3},
        {TEXT("color"), FNiagaraTypeDefinition::GetColorDef(), ENiagaraParameterValueKind::LinearColor},
    };

    for (const FCase& Case : Cases)
    {
        FNiagaraResolvedParameterType Resolved;
        if (!TestTrue(FString::Printf(TEXT("'%s' resolves"), Case.TypeName),
                ResolveNiagaraParameterType(Case.TypeName, Resolved)))
        {
            continue;
        }
        TestTrue(FString::Printf(TEXT("'%s' resolves to %s"), Case.TypeName, *Case.Expected.GetName()),
            Resolved.Definition.IsSameBaseDefinition(Case.Expected));
        TestEqual(FString::Printf(TEXT("'%s' value kind"), Case.TypeName),
            static_cast<int32>(Resolved.Kind), static_cast<int32>(Case.Kind));
    }

    // The struct-path spelling inspect publishes as `type.struct`. Every registered ENUM type's
    // storage struct is FNiagaraInt32, so this path matches them all; the resolver must still
    // answer with the int32 type itself.
    FNiagaraResolvedParameterType FromStructPath;
    if (TestTrue(TEXT("'/Script/Niagara.NiagaraInt32' resolves"),
            ResolveNiagaraParameterType(TEXT("/Script/Niagara.NiagaraInt32"), FromStructPath)))
    {
        TestTrue(TEXT("the struct path resolves to the int32 type"),
            FromStructPath.Definition.IsSameBaseDefinition(FNiagaraTypeDefinition::GetIntDef()));
        TestFalse(TEXT("the struct path does not resolve to an enum type"),
            FromStructPath.Definition.IsEnum());
    }

    // An enum type is still reachable by its own identity name, and is not confused with its
    // storage struct. ENiagaraExecutionState is registered by the Niagara module itself.
    FNiagaraResolvedParameterType EnumType;
    if (ResolveNiagaraParameterType(TEXT("ENiagaraExecutionState"), EnumType))
    {
        TestTrue(TEXT("an enum type name resolves to an enum type"), EnumType.Definition.IsEnum());
        TestNotEqual(TEXT("the enum type is not the int32 type"),
            PinWrightNiagara::DescribeNiagaraType(EnumType.Definition),
            PinWrightNiagara::DescribeNiagaraType(FNiagaraTypeDefinition::GetIntDef()));
    }
    else
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-enum-type-not-registered"),
            TEXT("ENiagaraExecutionState is not in FNiagaraTypeRegistry on this host, so the ")
            TEXT("enum-identity assertions were not run."));
    }

    // A type the parameter-store edit path cannot write stays refused rather than resolving to
    // something arbitrary.
    FNiagaraResolvedParameterType Unresolved;
    TestFalse(TEXT("an unknown type name does not resolve"),
        ResolveNiagaraParameterType(TEXT("NotANiagaraType_PinWrightProbe"), Unresolved));

    // Descriptions carry the path, which is what keeps a mismatch message from printing the same
    // text on both sides.
    TestTrue(TEXT("a described type carries its path"),
        PinWrightNiagara::DescribeNiagaraType(FNiagaraTypeDefinition::GetIntDef())
            .Contains(TEXT("/Script/Niagara.NiagaraInt32")));

    return true;
}

// ============================================================================
// 2. set_parameter accepts the canonical name with a SCALAR value and the write lands. Asserted on
//    the store, not on the response: a response echo cannot distinguish "wrote" from "wrote
//    nothing". Before the fix this call was refused INVALID_VALUE ("requires a JSON object value")
//    because the shape rule ran before the type resolved.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetParameterCanonicalTypeNameTest,
    "PinWright.niagara.set_parameter.CanonicalTypeNameWritesScalar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetParameterCanonicalTypeNameTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraParameterTypeNamesTestLocal;

    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    if (!TestNotNull(TEXT("Transient Niagara system created"), System))
    {
        return false;
    }

    const FName ParameterName(TEXT("User.PinWrightCanonicalInt"));
    const FNiagaraVariable IntVariable(FNiagaraTypeDefinition::GetIntDef(), ParameterName);
    System->GetExposedParameters().SetParameterValue(1, IntVariable, /*bAdd=*/true);

    // The canonical spelling niagara.inspect prints, with the scalar value the type actually holds.
    TSharedPtr<FJsonObject> CanonicalPayload = MakeUserScopePayload(ObjectPath, ParameterName.ToString());
    CanonicalPayload->SetStringField(TEXT("type"), TEXT("NiagaraInt32"));
    CanonicalPayload->SetNumberField(TEXT("value"), 90);

    FTestResponseCapture Capture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"), CanonicalPayload, Capture);
    TestEqual(FString::Printf(TEXT("canonical type name wrote the value (error was '%s': %s)"),
            *Capture.ErrorCode, *Capture.Message),
        System->GetExposedParameters().GetParameterValueOrDefault<int32>(IntVariable, 0), 90);

    // The short alias keeps working: this fix widens the accepted set, it does not move it.
    TSharedPtr<FJsonObject> AliasPayload = MakeUserScopePayload(ObjectPath, ParameterName.ToString());
    AliasPayload->SetStringField(TEXT("type"), TEXT("int32"));
    AliasPayload->SetNumberField(TEXT("value"), 91);

    FTestResponseCapture AliasCapture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.set_parameter"), AliasPayload, AliasCapture);
    TestEqual(TEXT("the short alias still writes"),
        System->GetExposedParameters().GetParameterValueOrDefault<int32>(IntVariable, 0), 91);

    // niagara.add_parameter resolves `type` through the same table, so the canonical name has to
    // create a parameter there too -- the two verbs sharing one resolver is the fix.
    const FName AddedName(TEXT("User.PinWrightCanonicalFloat"));
    const FNiagaraVariable AddedVariable(FNiagaraTypeDefinition::GetFloatDef(), AddedName);
    TSharedPtr<FJsonObject> AddPayload = MakeUserScopePayload(ObjectPath, AddedName.ToString());
    AddPayload->SetStringField(TEXT("type"), TEXT("NiagaraFloat"));
    AddPayload->SetNumberField(TEXT("defaultValue"), 1.5);

    FTestResponseCapture AddCapture;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_parameter"), AddPayload, AddCapture);
    TestEqual(FString::Printf(TEXT("add_parameter takes the canonical name too (error was '%s': %s)"),
            *AddCapture.ErrorCode, *AddCapture.Message),
        System->GetExposedParameters().GetParameterValueOrDefault<float>(AddedVariable, 0.0f), 1.5f);

    System->RemoveFromRoot();
    return true;
}

// ============================================================================
// 3. A real type mismatch names both RESOLVED types, so the two sides of the refusal can never be
//    the same text. The request uses the short alias "int32" on purpose: the message has to print
//    what the alias resolved TO, not the string the caller typed.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetParameterTypeMismatchMessageTest,
    "PinWright.niagara.set_parameter.TypeMismatchNamesResolvedTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetParameterTypeMismatchMessageTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraParameterTypeNamesTestLocal;

    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    if (!TestNotNull(TEXT("Transient Niagara system created"), System))
    {
        return false;
    }

    const FName ParameterName(TEXT("User.PinWrightMismatchFloat"));
    const FNiagaraVariable FloatVariable(FNiagaraTypeDefinition::GetFloatDef(), ParameterName);
    System->GetExposedParameters().SetParameterValue(2.0f, FloatVariable, /*bAdd=*/true);

    TSharedPtr<FJsonObject> Payload = MakeUserScopePayload(ObjectPath, ParameterName.ToString());
    Payload->SetStringField(TEXT("type"), TEXT("int32"));
    Payload->SetNumberField(TEXT("value"), 5);

    FTestResponseCapture Capture;
    TestTrue(TEXT("niagara.set_parameter handler found"),
        InvokeHandlerWithCapture(FString(TEXT("niagara.set_parameter")), Payload, Capture));
    TestFalse(TEXT("a type mismatch fails"), Capture.bSuccess);
    TestEqual(TEXT("it fails as PARAMETER_TYPE_MISMATCH"), Capture.ErrorCode, FString(TEXT("PARAMETER_TYPE_MISMATCH")));
    TestTrue(FString::Printf(TEXT("the message names the existing type's path (was '%s')"), *Capture.Message),
        Capture.Message.Contains(TEXT("/Script/Niagara.NiagaraFloat")));
    TestTrue(FString::Printf(TEXT("the message names the RESOLVED requested type (was '%s')"), *Capture.Message),
        Capture.Message.Contains(TEXT("/Script/Niagara.NiagaraInt32")));
    TestEqual(TEXT("the refused call wrote nothing"),
        System->GetExposedParameters().GetParameterValueOrDefault<float>(FloatVariable, 0.0f), 2.0f);

    System->RemoveFromRoot();
    return true;
}

// ============================================================================
// 4. An inspect `type` OBJECT is refused, at both layers, and writes nothing. The dispatcher's
//    declared-type gate refuses it first (`type` is declared `string`); ParseParameterPayload is
//    the second layer and is reached by any caller that bypasses the gate -- it must say the field
//    was mis-shaped, not that it was missing, because "Missing parameter 'type'" is false about a
//    payload that sent one.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSetParameterObjectTypeRefusedTest,
    "PinWright.niagara.set_parameter.ObjectTypeIsRefusedNotSilentlyAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSetParameterObjectTypeRefusedTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraParameterTypeNamesTestLocal;

    FString ObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(ObjectPath);
    if (!TestNotNull(TEXT("Transient Niagara system created"), System))
    {
        return false;
    }

    const FName ParameterName(TEXT("User.PinWrightObjectTypeInt"));
    const FNiagaraVariable IntVariable(FNiagaraTypeDefinition::GetIntDef(), ParameterName);
    System->GetExposedParameters().SetParameterValue(1, IntVariable, /*bAdd=*/true);

    TSharedPtr<FJsonObject> Payload = MakeUserScopePayload(ObjectPath, ParameterName.ToString());
    Payload->SetObjectField(TEXT("type"), MakeInspectTypeObject());
    Payload->SetNumberField(TEXT("value"), 90);

    // Layer 1: through a real dispatcher, which is where the declared-type gate lives.
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("niagara.set_parameter"),
        TEXT("req-niagara-set-parameter-object-type"), Payload, bSuccess, ErrorCode);
    TestFalse(TEXT("an object 'type' does not report success"), bSuccess);
    TestEqual(TEXT("the dispatcher refuses it as PARAM_TYPE_MISMATCH"),
        ErrorCode, FString(ErrorCodes::ERR_PARAM_TYPE_MISMATCH));
    TestTrue(FString::Printf(TEXT("the refusal names the offending field (was '%s')"), *Sink->Message),
        Sink->Message.Contains(TEXT("'type'")));
    TestEqual(TEXT("the refused call wrote nothing"),
        System->GetExposedParameters().GetParameterValueOrDefault<int32>(IntVariable, 0), 1);

    // Layer 2: the payload parser itself, reached by any caller that does not go through the gate.
    FNiagaraParameterEditPayload Parsed;
    const FNiagaraEditError ParseError = NiagaraEdit::ParseParameterPayload(
        Payload, ENiagaraEditOperation::SetParameter, Parsed);
    TestTrue(TEXT("the parser refuses an object 'type'"), ParseError.HasError());
    TestEqual(TEXT("the parser refuses it as INVALID_ARGUMENT"), ParseError.Code, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(FString::Printf(TEXT("the parser names the field (was '%s')"), *ParseError.Message),
        ParseError.Message.Contains(TEXT("'type'")));
    TestTrue(FString::Printf(TEXT("the parser points at the object's 'name' field (was '%s')"), *ParseError.Message),
        ParseError.Message.Contains(TEXT("'name'")));

    System->RemoveFromRoot();
    return true;
}
