// Copyright (c) 2026 Alexander Penkin. MIT License.

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

#include "PCGGraph.h"

// ---------------------------------------------------------------------------
// pcg graph user-parameter CRUD round trip.
//
// Drives the real registered handlers (pcg.add_graph_parameter /
// list_graph_parameters / remove_graph_parameter) through the dispatch-capture
// seam — InvokeHandlerWithCapture invokes the production handler body — then
// verifies the effect against the graph's own FInstancedPropertyBag via the
// engine API, not a re-implementation.
//
// Differential: the three handlers are net-new. Reverting the handler .cpp
// unregisters them, so InvokeHandlerWithCapture returns false and the
// "handler registered" TestTrue fails (Result={Fail}) — the test cannot pass
// without the fix in the tree.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGraphParameterCrudRoundTripTest,
    "PinWright.pcg.graph_parameter.AddListRemoveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGraphParameterCrudRoundTripTest::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;
    const FString GraphPath = Graph->GetPathName();

    // 1. ADD a float parameter with an initial value.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), TEXT("Density"));
        Payload->SetStringField(TEXT("type"), TEXT("float"));
        Payload->SetStringField(TEXT("value"), TEXT("0.25"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("add_graph_parameter registered"),
            InvokeHandlerWithCapture(TEXT("pcg.add_graph_parameter"), Payload, Capture));
        if (!TestTrue(TEXT("add succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_graph_parameter error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
        double ResponseDensity = 0.0;
        TestTrue(TEXT("add response carries the stored float value"),
            Capture.Result.IsValid()
            && Capture.Result->TryGetNumberField(TEXT("value"), ResponseDensity));
        TestEqual(TEXT("add response float value == 0.25"), ResponseDensity, 0.25);
    }

    // Production-state proof: the graph's user-parameter bag now holds Density == 0.25.
    // 0.25 is exactly representable in binary float, so the equality is exact.
    {
        const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
        TestNotNull(TEXT("user parameters struct exists after add"), Bag);
        if (Bag)
        {
            TestNotNull(TEXT("Density descriptor present in the bag"),
                Bag->FindPropertyDescByName(FName(TEXT("Density"))));
            TValueOrError<float, EPropertyBagResult> V = Bag->GetValueFloat(FName(TEXT("Density")));
            TestTrue(TEXT("Density value is readable as float"), V.IsValid());
            if (V.IsValid())
            {
                TestEqual(TEXT("Density value == 0.25"), V.GetValue(), 0.25f);
            }
        }
    }

    // 2. LIST reports the parameter with its type.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_graph_parameters registered"),
            InvokeHandlerWithCapture(TEXT("pcg.list_graph_parameters"), Payload, Capture));
        TestTrue(TEXT("list succeeded"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(
                Capture.Result, TEXT("parameters"), TEXT("name"), TEXT("Density"));
            TestTrue(TEXT("Density appears in the parameters list"), Entry.IsValid());
            if (Entry.IsValid())
            {
                FString Type;
                TestTrue(TEXT("list entry carries a type"), Entry->TryGetStringField(TEXT("type"), Type));
                TestEqual(TEXT("listed type is float"), Type, FString(TEXT("float")));
            }
        }
    }

    // 3. REMOVE the parameter.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), TEXT("Density"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("remove_graph_parameter registered"),
            InvokeHandlerWithCapture(TEXT("pcg.remove_graph_parameter"), Payload, Capture));
        TestTrue(TEXT("remove succeeded"), Capture.bSuccess);
    }

    // Production-state proof: the descriptor is gone from the bag after removal.
    {
        const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
        TestNotNull(TEXT("user parameters struct exists after remove"), Bag);
        if (Bag)
        {
            TestNull(TEXT("Density descriptor gone after remove"),
                Bag->FindPropertyDescByName(FName(TEXT("Density"))));
        }
    }

    // 4. REMOVE of a nonexistent parameter fails loud (no fake success).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), TEXT("NoSuchParam"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("remove_graph_parameter registered (missing case)"),
            InvokeHandlerWithCapture(TEXT("pcg.remove_graph_parameter"), Payload, Capture));
        TestFalse(TEXT("remove of a missing parameter is refused"), Capture.bSuccess);
        TestEqual(TEXT("error code is PARAMETER_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("PARAMETER_NOT_FOUND")));
    }

    // 5. ADD of an unsupported type is refused with INVALID_PARAM_TYPE.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), TEXT("SomeStruct"));
        Payload->SetStringField(TEXT("type"), TEXT("struct"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("add_graph_parameter registered (bad-type case)"),
            InvokeHandlerWithCapture(TEXT("pcg.add_graph_parameter"), Payload, Capture));
        TestFalse(TEXT("unsupported type is refused"), Capture.bSuccess);
        TestEqual(TEXT("error code is INVALID_PARAM_TYPE"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAM_TYPE")));
    }

    // 6. Names that need sanitization round-trip end to end. FInstancedPropertyBag
    // folds spaces (and other InvalidNameCharacters) to '_' when storing a new
    // property, so "My Density" is stored as "My_Density". The handler must add the
    // value successfully (not spuriously SET_FAILED against the raw name), echo the
    // stored name, list it under the stored name, and remove it when the caller
    // passes the same raw name they added.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), TEXT("My Density"));
        Payload->SetStringField(TEXT("type"), TEXT("float"));
        Payload->SetStringField(TEXT("value"), TEXT("0.5"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("add_graph_parameter registered (sanitized-name case)"),
            InvokeHandlerWithCapture(TEXT("pcg.add_graph_parameter"), Payload, Capture));
        // Pre-fix, the value-set path looked up the raw "My Density" and returned
        // SET_FAILED here even though the property was created.
        if (!TestTrue(TEXT("add of a space-bearing name succeeds"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_graph_parameter (sanitized) error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
        }
        else if (Capture.Result.IsValid())
        {
            FString EchoedName;
            TestTrue(TEXT("add echoes a name"), Capture.Result->TryGetStringField(TEXT("name"), EchoedName));
            TestEqual(TEXT("echoed name is the sanitized/stored name"),
                EchoedName, FString(TEXT("My_Density")));
        }
    }

    // Production-state proof: the bag holds the sanitized name with the value, and
    // does NOT hold the raw space-bearing name.
    {
        const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
        TestNotNull(TEXT("user parameters struct exists after sanitized add"), Bag);
        if (Bag)
        {
            TestNotNull(TEXT("sanitized descriptor 'My_Density' present"),
                Bag->FindPropertyDescByName(FName(TEXT("My_Density"))));
            TestNull(TEXT("raw 'My Density' is not a distinct descriptor"),
                Bag->FindPropertyDescByName(FName(TEXT("My Density"))));
            TValueOrError<float, EPropertyBagResult> V = Bag->GetValueFloat(FName(TEXT("My_Density")));
            TestTrue(TEXT("sanitized value is readable as float"), V.IsValid());
            if (V.IsValid())
            {
                TestEqual(TEXT("sanitized param value == 0.5"), V.GetValue(), 0.5f);
            }
        }
    }

    // LIST reports the stored (sanitized) name.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_graph_parameters registered (sanitized case)"),
            InvokeHandlerWithCapture(TEXT("pcg.list_graph_parameters"), Payload, Capture));
        TestTrue(TEXT("list succeeded (sanitized case)"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(
                Capture.Result, TEXT("parameters"), TEXT("name"), TEXT("My_Density"));
            TestTrue(TEXT("'My_Density' appears in the parameters list"), Entry.IsValid());
        }
    }

    // REMOVE by the ORIGINAL raw name the caller added — sanitized to the stored key.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), TEXT("My Density"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("remove_graph_parameter registered (sanitized case)"),
            InvokeHandlerWithCapture(TEXT("pcg.remove_graph_parameter"), Payload, Capture));
        if (!TestTrue(TEXT("remove by the raw added name succeeds"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("remove_graph_parameter (sanitized) error: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
        }
    }

    {
        const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
        if (Bag)
        {
            TestNull(TEXT("'My_Density' descriptor gone after remove"),
                Bag->FindPropertyDescByName(FName(TEXT("My_Density"))));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGGraphParameterMalformedScalarRejectedTest,
    "PinWright.pcg.graph_parameter.InvalidScalarRejectedWithoutMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGGraphParameterMalformedScalarRejectedTest::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(),
        NAME_None, RF_Transient);
    TestNotNull(TEXT("transient graph created"), Graph);
    if (!Graph) return true;
    const FString GraphPath = Graph->GetPathName();

    auto AddParameter = [&](const TCHAR* Name, const TCHAR* Type, const TCHAR* Value,
        FTestResponseCapture& Capture) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("graphPath"), GraphPath);
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("type"), Type);
        Payload->SetStringField(TEXT("value"), Value);
        return InvokeHandlerWithCapture(TEXT("pcg.add_graph_parameter"), Payload, Capture);
    };

    FTestResponseCapture Capture;
    TestTrue(TEXT("initial float parameter is registered"),
        AddParameter(TEXT("Density"), TEXT("float"), TEXT("0.25"), Capture));
    TestTrue(TEXT("initial float parameter succeeds"), Capture.bSuccess);

    const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
    TestNotNull(TEXT("user parameters struct exists"), Bag);
    if (!Bag) return true;
    TValueOrError<float, EPropertyBagResult> InitialDensity =
        Bag->GetValueFloat(FName(TEXT("Density")));
    TestTrue(TEXT("initial float value is readable"), InitialDensity.IsValid());
    if (InitialDensity.IsValid())
    {
        TestEqual(TEXT("initial float value is 0.25"), InitialDensity.GetValue(), 0.25f);
    }

    // Trailing junk must be rejected before the upsert can replace the existing value.
    TestTrue(TEXT("malformed float handler is registered"),
        AddParameter(TEXT("Density"), TEXT("float"), TEXT("0.25junk"), Capture));
    TestFalse(TEXT("malformed float is refused"), Capture.bSuccess);
    TestEqual(TEXT("malformed float returns INVALID_VALUE"),
        Capture.ErrorCode, FString(TEXT("INVALID_VALUE")));
    Bag = Graph->GetUserParametersStruct();
    if (Bag)
    {
        TValueOrError<float, EPropertyBagResult> UnchangedDensity =
            Bag->GetValueFloat(FName(TEXT("Density")));
        TestTrue(TEXT("float value remains readable after rejection"), UnchangedDensity.IsValid());
        if (UnchangedDensity.IsValid())
        {
            TestEqual(TEXT("malformed float leaves the value unchanged"),
                UnchangedDensity.GetValue(), 0.25f);
        }
    }

    // Non-finite text is also rejected, rather than becoming a platform-dependent value.
    TestTrue(TEXT("non-finite float handler is registered"),
        AddParameter(TEXT("Density"), TEXT("float"), TEXT("nan"), Capture));
    TestFalse(TEXT("non-finite float is refused"), Capture.bSuccess);
    TestEqual(TEXT("non-finite float returns INVALID_VALUE"),
        Capture.ErrorCode, FString(TEXT("INVALID_VALUE")));
    Bag = Graph->GetUserParametersStruct();
    if (Bag)
    {
        TValueOrError<float, EPropertyBagResult> UnchangedDensityAfterNan =
            Bag->GetValueFloat(FName(TEXT("Density")));
        TestTrue(TEXT("float value remains readable after non-finite rejection"),
            UnchangedDensityAfterNan.IsValid());
        if (UnchangedDensityAfterNan.IsValid())
        {
            TestEqual(TEXT("non-finite float leaves the value unchanged"),
                UnchangedDensityAfterNan.GetValue(), 0.25f);
        }
    }

    TestTrue(TEXT("initial bool parameter is registered"),
        AddParameter(TEXT("Enabled"), TEXT("bool"), TEXT("true"), Capture));
    TestTrue(TEXT("initial bool parameter succeeds"), Capture.bSuccess);

    TestTrue(TEXT("malformed bool handler is registered"),
        AddParameter(TEXT("Enabled"), TEXT("bool"), TEXT("maybe"), Capture));
    TestFalse(TEXT("malformed bool is refused"), Capture.bSuccess);
    TestEqual(TEXT("malformed bool returns INVALID_VALUE"),
        Capture.ErrorCode, FString(TEXT("INVALID_VALUE")));
    Bag = Graph->GetUserParametersStruct();
    if (Bag)
    {
        TValueOrError<bool, EPropertyBagResult> UnchangedEnabled =
            Bag->GetValueBool(FName(TEXT("Enabled")));
        TestTrue(TEXT("bool value remains readable after rejection"), UnchangedEnabled.IsValid());
        if (UnchangedEnabled.IsValid())
        {
            TestTrue(TEXT("malformed bool leaves the value unchanged"), UnchangedEnabled.GetValue());
        }
    }

    TestTrue(TEXT("invalid new parameter handler is registered"),
        AddParameter(TEXT("NewDensity"), TEXT("float"), TEXT(""), Capture));
    TestFalse(TEXT("invalid new parameter is refused"), Capture.bSuccess);
    TestEqual(TEXT("invalid new parameter returns INVALID_VALUE"),
        Capture.ErrorCode, FString(TEXT("INVALID_VALUE")));
    Bag = Graph->GetUserParametersStruct();
    if (Bag)
    {
        TestNull(TEXT("invalid new parameter leaves no descriptor"),
            Bag->FindPropertyDescByName(FName(TEXT("NewDensity"))));
    }

    return true;
}

#endif // __has_include("PCGGraph.h")
