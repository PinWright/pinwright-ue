// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// COUNTERFACTUAL: if the parm-walk omits the post-ProcessEvent return-value
// export, `void == true` and `returnValue` is absent — assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionReturnsValueTest,
    "PinWright.object.call_function.ReturnsValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionReturnsValueTest::RunTest(const FString& Parameters)
{
    // GetShouldUpdatePhysicsVolume is a BlueprintGetter UFUNCTION on
    // USceneComponent: no params, returns bool. Stable across UE versions.
    USceneComponent* Target = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient component created"), Target);
    if (!Target) return false;

    const TStrongObjectPtr<USceneComponent> TargetGuard(Target);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Target->GetPathName());
    Payload->SetStringField(TEXT("function"), TEXT("GetShouldUpdatePhysicsVolume"));

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), Payload, Capture);
    TestTrue(TEXT("Handler registered"), bInvoked);
    if (!bInvoked) return false;

    TestTrue(TEXT("Handler reported success"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("Handler error: %s — %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    TestTrue(TEXT("Result payload present"), Capture.Result.IsValid());
    if (!Capture.Result.IsValid()) return false;

    bool bVoid = true;
    Capture.Result->TryGetBoolField(TEXT("void"), bVoid);
    TestFalse(TEXT("Function is not void"), bVoid);

    const TSharedPtr<FJsonValue> ReturnField = Capture.Result->TryGetField(TEXT("returnValue"));
    TestTrue(TEXT("returnValue field present"), ReturnField.IsValid());
    return true;
}

// COUNTERFACTUAL: reverting the raw-container fix passes the ProcessEvent frame
// into component ownership logic and crashes before these response assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FObjectCallFunctionComponentReturnPathTest,
    "PinWright.object.call_function.ComponentReturnPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectCallFunctionComponentReturnPathTest::RunTest(const FString& Parameters)
{
    USceneComponent* Parent = NewObject<USceneComponent>(GetTransientPackage());
    USceneComponent* Child = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Parent component created"), Parent);
    TestNotNull(TEXT("Child component created"), Child);
    if (!Parent || !Child) return false;

    const TStrongObjectPtr<USceneComponent> ParentGuard(Parent);
    const TStrongObjectPtr<USceneComponent> ChildGuard(Child);
    Child->SetupAttachment(Parent);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Child->GetPathName());
    Payload->SetStringField(TEXT("function"), TEXT("GetAttachParent"));

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), Payload, Capture);
    TestTrue(TEXT("Handler registered"), bInvoked);
    if (!bInvoked) return false;

    TestTrue(TEXT("Handler reported success"), Capture.bSuccess);
    TestTrue(TEXT("Successful response contains no error"), Capture.ErrorCode.IsEmpty());
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return false;

    bool bVoid = true;
    TestTrue(TEXT("void field present"), Capture.Result->TryGetBoolField(TEXT("void"), bVoid));
    TestFalse(TEXT("GetAttachParent is not void"), bVoid);

    const TSharedPtr<FJsonValue> ReturnField = Capture.Result->TryGetField(TEXT("returnValue"));
    TestTrue(TEXT("returnValue field present"), ReturnField.IsValid());
    if (!ReturnField.IsValid()) return false;

    TestEqual(TEXT("returnValue is a string"), ReturnField->Type, EJson::String);
    FString ReturnPath;
    TestTrue(TEXT("returnValue string is readable"), ReturnField->TryGetString(ReturnPath));
    TestEqual(TEXT("returnValue is the attach-parent path"), ReturnPath, Parent->GetPathName());
    return true;
}

// COUNTERFACTUAL: regressing to a zero-init parm buffer (the actor.call_function
// behavior) means the call succeeds with a garbage zero scale rather than
// erroring — assertion fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionMissingParamErrorsTest,
    "PinWright.object.call_function.MissingParamErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionMissingParamErrorsTest::RunTest(const FString& Parameters)
{
    USceneComponent* Target = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient component created"), Target);
    if (!Target) return false;
    const TStrongObjectPtr<USceneComponent> TargetGuard(Target);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Target->GetPathName());
    Payload->SetStringField(TEXT("function"), TEXT("SetWorldScale3D"));
    Payload->SetObjectField(TEXT("args"), MakeShared<FJsonObject>()); // empty

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), Payload, Capture);
    TestTrue(TEXT("Handler registered"), bInvoked);
    if (!bInvoked) return false;

    TestFalse(TEXT("Handler must error"), Capture.bSuccess);
    TestEqual(TEXT("Error code is MISSING_PARAM"),
        Capture.ErrorCode, FString(TEXT("MISSING_PARAM")));
    return true;
}

// COUNTERFACTUAL: if the aligned production parameter frame is reverted, setter
// success may still pass but actor scale remains unchanged and getter reads zeroed data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionAppliesActorVectorParamTest,
    "PinWright.object.call_function.AppliesActorVectorParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionAppliesActorVectorParamTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TestNotNull(TEXT("Editor world exists"), World);
    if (!World) return false;

    AActor* Actor = World->SpawnActor<AActor>();
    TestNotNull(TEXT("Actor spawned"), Actor);
    if (!Actor) return false;
    ON_SCOPE_EXIT
    {
        if (Actor && !Actor->IsActorBeingDestroyed())
        {
            Actor->Destroy();
        }
    };

    USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("RootComponent"));
    TestNotNull(TEXT("Root component created"), Root);
    if (!Root) return false;
    Actor->SetRootComponent(Root);
    Actor->AddInstanceComponent(Root);
    Root->RegisterComponent();

    TSharedPtr<FJsonObject> NewScale = MakeShared<FJsonObject>();
    NewScale->SetNumberField(TEXT("x"), 2.0);
    NewScale->SetNumberField(TEXT("y"), 3.0);
    NewScale->SetNumberField(TEXT("z"), 4.0);

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetObjectField(TEXT("NewScale3D"), NewScale);

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("objectPath"), Actor->GetPathName());
    SetPayload->SetStringField(TEXT("function"), TEXT("SetActorScale3D"));
    SetPayload->SetObjectField(TEXT("args"), Args);

    FTestResponseCapture SetCapture;
    const bool bSetInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), SetPayload, SetCapture);
    TestTrue(TEXT("SetActorScale3D handler registered"), bSetInvoked);
    if (!bSetInvoked) return false;

    TestTrue(TEXT("SetActorScale3D reported success"), SetCapture.bSuccess);
    if (!SetCapture.bSuccess)
    {
        AddError(FString::Printf(TEXT("SetActorScale3D error: %s — %s"),
            *SetCapture.ErrorCode, *SetCapture.Message));
        return false;
    }

    const FVector ExpectedScale(2.0, 3.0, 4.0);
    TestTrue(TEXT("Actor scale changed"),
        Actor->GetActorScale3D().Equals(ExpectedScale, 0.01));

    TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
    GetPayload->SetStringField(TEXT("objectPath"), Actor->GetPathName());
    GetPayload->SetStringField(TEXT("function"), TEXT("GetActorScale3D"));

    FTestResponseCapture GetCapture;
    const bool bGetInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), GetPayload, GetCapture);
    TestTrue(TEXT("GetActorScale3D handler registered"), bGetInvoked);
    if (!bGetInvoked) return false;

    TestTrue(TEXT("GetActorScale3D reported success"), GetCapture.bSuccess);
    if (!GetCapture.bSuccess)
    {
        AddError(FString::Printf(TEXT("GetActorScale3D error: %s — %s"),
            *GetCapture.ErrorCode, *GetCapture.Message));
        return false;
    }

    TestTrue(TEXT("GetActorScale3D payload present"), GetCapture.Result.IsValid());
    if (!GetCapture.Result.IsValid()) return false;

    bool bVoid = true;
    TestTrue(TEXT("void field present"), GetCapture.Result->TryGetBoolField(TEXT("void"), bVoid));
    TestFalse(TEXT("GetActorScale3D is not void"), bVoid);

    const TSharedPtr<FJsonValue> ReturnField = GetCapture.Result->TryGetField(TEXT("returnValue"));
    TestTrue(TEXT("returnValue field present"), ReturnField.IsValid());
    if (!ReturnField.IsValid()) return false;

    if (!TestEqual(TEXT("returnValue is array"), ReturnField->Type, EJson::Array))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& ReturnArray = ReturnField->AsArray();
    TestEqual(TEXT("returnValue vector component count"), ReturnArray.Num(), 3);
    if (ReturnArray.Num() != 3) return false;

    TestTrue(TEXT("returnValue X"), FMath::IsNearlyEqual(ReturnArray[0]->AsNumber(), 2.0, 0.01));
    TestTrue(TEXT("returnValue Y"), FMath::IsNearlyEqual(ReturnArray[1]->AsNumber(), 3.0, 0.01));
    TestTrue(TEXT("returnValue Z"), FMath::IsNearlyEqual(ReturnArray[2]->AsNumber(), 4.0, 0.01));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionUnknownObjectErrorsTest,
    "PinWright.object.call_function.UnknownObjectErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionUnknownObjectErrorsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), TEXT("/Engine/DoesNotExist.NopeNopeNope"));
    Payload->SetStringField(TEXT("function"), TEXT("GetName"));

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), Payload, Capture);
    TestTrue(TEXT("Handler registered"), bInvoked);
    if (!bInvoked) return false;

    TestFalse(TEXT("Handler must error"), Capture.bSuccess);
    TestEqual(TEXT("Error code is OBJECT_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("OBJECT_NOT_FOUND")));
    return true;
}

// ---------------------------------------------------------------------------
// Numeric argument fidelity.
//
// Board ticket B-call-function-float-arg-loses-decimal alleged that a fractional
// float argument (0.18) reaches the callee as a different number while 0.4 and
// 0.2 land. The tests below pin what the marshalling layer actually does, so the
// claim is machine-checkable instead of re-argued from a field report: a JSON
// number travels as a double from the reader into ApplyJsonValueToProperty
// (Utils/PropertyImport.cpp) with no string round trip, and the only value change
// the contract permits is the single double -> float narrowing an FFloatProperty
// destination requires.
// ---------------------------------------------------------------------------

// COUNTERFACTUAL: reintroduce any string round trip (SanitizeFloat on the way in,
// FCString::Atof/Atod on the way out) or a locale-sensitive parse in the
// JSON-number -> FFloatProperty path and at least one of these fractions lands as
// a different float — the exact comparison fails and prints both values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionFloatParamArrivesExactTest,
    "PinWright.object.call_function.FloatParamArrivesExact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionFloatParamArrivesExactTest::RunTest(const FString& Parameters)
{
    // UActorComponent::SetComponentTickInterval(float) is the narrowest reflected
    // float sink the engine offers: its whole body is one store with no clamp and
    // no side effect, and GetComponentTickInterval() reads that same field back.
    // Any difference between the JSON literal and the stored float is therefore
    // produced by the marshalling layer and nothing else.
    USceneComponent* Target = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient component created"), Target);
    if (!Target) return false;
    const TStrongObjectPtr<USceneComponent> TargetGuard(Target);

    // Two-fraction-digit values (the shape the ticket reported as broken), a
    // sub-millisecond value, a negative, and integer-valued inputs. 0.0 goes last
    // so it cannot be mistaken for the field never having been written.
    const double Inputs[] = { 0.18, 0.4, 0.2, 0.075, 1.25, 1.0e-3, -0.18, 2.0, 0.0 };

    for (const double In : Inputs)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetNumberField(TEXT("TickInterval"), In);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Target->GetPathName());
        Payload->SetStringField(TEXT("function"), TEXT("SetComponentTickInterval"));
        Payload->SetObjectField(TEXT("args"), Args);

        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("object.call_function"), Payload, Capture);
        TestTrue(TEXT("Handler registered"), bInvoked);
        if (!bInvoked) return false;

        if (!Capture.bSuccess)
        {
            AddError(FString::Printf(TEXT("SetComponentTickInterval(%.17f) errored: %s - %s"),
                In, *Capture.ErrorCode, *Capture.Message));
            return false;
        }

        // Exact, not near: the double -> float narrowing at the FFloatProperty
        // destination is the only transform allowed, so the stored float must equal
        // that same narrowing applied here.
        const float Expected = static_cast<float>(In);
        const float Actual = Target->GetComponentTickInterval();
        if (Actual != Expected)
        {
            AddError(FString::Printf(
                TEXT("Float argument mis-marshalled: sent %.17f, callee received %.17f (expected %.17f)"),
                In, static_cast<double>(Actual), static_cast<double>(Expected)));
        }
    }

    return true;
}

// COUNTERFACTUAL: narrow the FDoubleProperty sub-property writes to float, or route
// them through a text import, and 0.18 lands as 0.18000000715255737 — the exact
// comparison fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionDoubleParamArrivesExactTest,
    "PinWright.object.call_function.DoubleParamArrivesExact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionDoubleParamArrivesExactTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TestNotNull(TEXT("Editor world exists"), World);
    if (!World) return false;

    AActor* Actor = World->SpawnActor<AActor>();
    TestNotNull(TEXT("Actor spawned"), Actor);
    if (!Actor) return false;
    ON_SCOPE_EXIT
    {
        if (Actor && !Actor->IsActorBeingDestroyed())
        {
            Actor->Destroy();
        }
    };

    USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("RootComponent"));
    TestNotNull(TEXT("Root component created"), Root);
    if (!Root) return false;
    Actor->SetRootComponent(Root);
    Actor->AddInstanceComponent(Root);
    Root->RegisterComponent();

    // FVector components are FDoubleProperty under LWC, so the object form of a
    // vector argument is how a caller reaches the scalar double branch of
    // ApplyJsonValueToProperty through this verb. An unattached root stores the
    // relative scale it is handed without composing anything into it.
    const double X = 0.18;
    const double Y = -0.075;
    const double Z = 1.0e-3;

    TSharedPtr<FJsonObject> NewScale = MakeShared<FJsonObject>();
    NewScale->SetNumberField(TEXT("x"), X);
    NewScale->SetNumberField(TEXT("y"), Y);
    NewScale->SetNumberField(TEXT("z"), Z);

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetObjectField(TEXT("NewScale3D"), NewScale);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Actor->GetPathName());
    Payload->SetStringField(TEXT("function"), TEXT("SetActorScale3D"));
    Payload->SetObjectField(TEXT("args"), Args);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), Payload, Capture);
    TestTrue(TEXT("Handler registered"), bInvoked);
    if (!bInvoked) return false;

    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("SetActorScale3D errored: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    const FVector Stored = Root->GetRelativeScale3D();
    if (Stored.X != X || Stored.Y != Y || Stored.Z != Z)
    {
        AddError(FString::Printf(
            TEXT("Double vector argument mis-marshalled: sent (%.17f, %.17f, %.17f), stored (%.17f, %.17f, %.17f)"),
            X, Y, Z, Stored.X, Stored.Y, Stored.Z));
    }

    return true;
}

// COUNTERFACTUAL: restore the `(float)` casts the array-literal Vector/Rotator branch
// of ApplyJsonValueToProperty used to apply and every component lands on its float32
// neighbour (0.18 -> 0.18000000715255737) while the call still reports success — the
// exact comparison fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FObjectCallFunctionVectorArrayKeepsDoublePrecisionTest,
    "PinWright.object.call_function.VectorArrayParamKeepsDoublePrecision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FObjectCallFunctionVectorArrayKeepsDoublePrecisionTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TestNotNull(TEXT("Editor world exists"), World);
    if (!World) return false;

    AActor* Actor = World->SpawnActor<AActor>();
    TestNotNull(TEXT("Actor spawned"), Actor);
    if (!Actor) return false;
    ON_SCOPE_EXIT
    {
        if (Actor && !Actor->IsActorBeingDestroyed())
        {
            Actor->Destroy();
        }
    };

    USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("RootComponent"));
    TestNotNull(TEXT("Root component created"), Root);
    if (!Root) return false;
    Actor->SetRootComponent(Root);
    Actor->AddInstanceComponent(Root);
    Root->RegisterComponent();

    // Same values as the object-form test above: the two spellings of a vector
    // argument must land the same number. Vectors are exported as arrays
    // (Utils/PropertyExport.cpp), so this is also the shape an export -> import
    // round trip travels in.
    const double X = 0.18;
    const double Y = -0.075;
    const double Z = 1.0e-3;

    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Z));

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetArrayField(TEXT("NewScale3D"), ScaleArray);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Actor->GetPathName());
    Payload->SetStringField(TEXT("function"), TEXT("SetActorScale3D"));
    Payload->SetObjectField(TEXT("args"), Args);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("object.call_function"), Payload, Capture);
    TestTrue(TEXT("Handler registered"), bInvoked);
    if (!bInvoked) return false;

    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("SetActorScale3D (array form) errored: %s - %s"),
            *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    const FVector Stored = Root->GetRelativeScale3D();
    if (Stored.X != X || Stored.Y != Y || Stored.Z != Z)
    {
        AddError(FString::Printf(
            TEXT("Array-form vector argument lost precision: sent (%.17f, %.17f, %.17f), stored (%.17f, %.17f, %.17f)"),
            X, Y, Z, Stored.X, Stored.Y, Stored.Z));
    }

    return true;
}
