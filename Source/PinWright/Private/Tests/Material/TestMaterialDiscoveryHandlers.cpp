// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for material.graph.list_expression_types and
// material.graph.search_expression_types — see MaterialDiscoveryHandler.cpp.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerContext.h"
#include "Tests/TestUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpression.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"


namespace
{
// Walks the expressions[] array and returns the first record whose className matches.
const TSharedPtr<FJsonObject>* FindRecordByClassName(
    const TArray<TSharedPtr<FJsonValue>>* Records, const FString& ClassName)
{
    if (!Records) return nullptr;
    for (const TSharedPtr<FJsonValue>& Val : *Records)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Val->TryGetObject(Obj) || !Obj) continue;
        FString Name;
        (*Obj)->TryGetStringField(TEXT("className"), Name);
        if (Name == ClassName)
        {
            return Obj;
        }
    }
    return nullptr;
}

bool ArrayContainsString(const TArray<TSharedPtr<FJsonValue>>* Arr, const FString& Needle)
{
    // Delegate to the shared string-array membership walk in TestUtils.h.
    return JsonValueArrayContainsString(Arr, Needle);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialListExpressionTypesTest,
    "PinWright.material.graph.list_expression_types.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialListExpressionTypesTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("material.graph.list_expression_types"), Payload, Capture);

    TestTrue(TEXT("handler found"), bInvoked);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    const int32 Count = Capture.Result->GetIntegerField(TEXT("count"));
    TestTrue(TEXT("count >= 50"), Count >= 50);

    const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("expressions"), Expressions) || !Expressions)
    {
        AddError(TEXT("Response missing 'expressions' array"));
        return true;
    }

    const TSharedPtr<FJsonObject>* MultiplyObj =
        FindRecordByClassName(Expressions, TEXT("MaterialExpressionMultiply"));
    if (!TestNotNull(TEXT("Multiply entry present"), MultiplyObj))
    {
        return true;
    }

    FString MultiplyShort;
    (*MultiplyObj)->TryGetStringField(TEXT("shortName"), MultiplyShort);
    TestEqual(TEXT("Multiply shortName"), MultiplyShort, FString(TEXT("Multiply")));

    FString MultiplyCategory;
    (*MultiplyObj)->TryGetStringField(TEXT("category"), MultiplyCategory);
    TestTrue(TEXT("Multiply category contains 'Math'"),
        MultiplyCategory.Contains(TEXT("Math"), ESearchCase::IgnoreCase));

    const TArray<TSharedPtr<FJsonValue>>* MulInputs = nullptr;
    (*MultiplyObj)->TryGetArrayField(TEXT("inputPins"), MulInputs);
    TestTrue(TEXT("Multiply inputs contain A"), ArrayContainsString(MulInputs, TEXT("A")));
    TestTrue(TEXT("Multiply inputs contain B"), ArrayContainsString(MulInputs, TEXT("B")));

    const TSharedPtr<FJsonObject>* FresnelObj =
        FindRecordByClassName(Expressions, TEXT("MaterialExpressionFresnel"));
    if (TestNotNull(TEXT("Fresnel entry present"), FresnelObj))
    {
        FString FresnelDesc;
        (*FresnelObj)->TryGetStringField(TEXT("description"), FresnelDesc);
        TestTrue(TEXT("Fresnel description non-empty"), !FresnelDesc.IsEmpty());
    }

    return true;
}

// Regression guard for E-material-list-expression-types-limit: list_expression_types
// must honor the actor.list-style narrowing levers (limit + fields/namesOnly) and report
// the untruncated totalMatches + truncated flag. On the pre-fix handler limit/namesOnly
// were silently ignored, so every assertion below fails (no cap, heavy fields always emitted,
// no totalMatches/truncated fields).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialListExpressionTypesLimitProjectionTest,
    "PinWright.material.graph.list_expression_types.LimitAndNamesOnlyProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialListExpressionTypesLimitProjectionTest::RunTest(const FString& Parameters)
{
    // Shared invoke-and-check preamble: each sub-case differs only in payload and
    // its case-specific assertions, so the dispatch + found/success checks live here.
    auto Invoke = [&](const TSharedPtr<FJsonObject>& Payload) -> FTestResponseCapture
    {
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.list_expression_types"), Payload, Capture);
        TestTrue(TEXT("handler found"), bInvoked);
        TestTrue(TEXT("success"), Capture.bSuccess);
        return Capture;
    };

    // --- Default (no projection) stays byte-identical: heavy fields present,
    //     totalMatches == count, not truncated. ---
    int32 FullCount = 0;
    {
        FTestResponseCapture Capture = Invoke(MakeShared<FJsonObject>());
        if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

        FullCount = Capture.Result->GetIntegerField(TEXT("count"));
        const int32 TotalMatches = Capture.Result->GetIntegerField(TEXT("totalMatches"));
        TestEqual(TEXT("default totalMatches == count"), TotalMatches, FullCount);
        TestFalse(TEXT("default not truncated"),
            Capture.Result->GetBoolField(TEXT("truncated")));

        const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
        Capture.Result->TryGetArrayField(TEXT("expressions"), Expressions);
        const TSharedPtr<FJsonObject>* MultiplyObj =
            FindRecordByClassName(Expressions, TEXT("MaterialExpressionMultiply"));
        if (TestNotNull(TEXT("default Multiply present"), MultiplyObj))
        {
            TestTrue(TEXT("default record keeps heavy 'description' field"),
                (*MultiplyObj)->HasField(TEXT("description")));
            TestTrue(TEXT("default record keeps 'inputPins' field"),
                (*MultiplyObj)->HasField(TEXT("inputPins")));
        }
    }

    // --- limit caps the array; totalMatches reports the full count; truncated set. ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("limit"), 3.0);
        FTestResponseCapture Capture = Invoke(Payload);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const int32 Count = Capture.Result->GetIntegerField(TEXT("count"));
            const int32 TotalMatches = Capture.Result->GetIntegerField(TEXT("totalMatches"));
            TestEqual(TEXT("limit=3 returns exactly 3"), Count, 3);
            TestEqual(TEXT("totalMatches reports full untruncated count"), TotalMatches, FullCount);
            TestTrue(TEXT("totalMatches > returned count"), TotalMatches > Count);
            TestTrue(TEXT("truncated flag set when capped"),
                Capture.Result->GetBoolField(TEXT("truncated")));
        }
    }

    // --- namesOnly drops the heavy fields while keeping the identity columns. ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("namesOnly"), true);
        FTestResponseCapture Capture = Invoke(Payload);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
            Capture.Result->TryGetArrayField(TEXT("expressions"), Expressions);
            const TSharedPtr<FJsonObject>* MultiplyObj =
                FindRecordByClassName(Expressions, TEXT("MaterialExpressionMultiply"));
            if (TestNotNull(TEXT("namesOnly Multiply present"), MultiplyObj))
            {
                // Light identity columns kept...
                TestTrue(TEXT("namesOnly keeps className"),
                    (*MultiplyObj)->HasField(TEXT("className")));
                TestTrue(TEXT("namesOnly keeps shortName"),
                    (*MultiplyObj)->HasField(TEXT("shortName")));
                TestTrue(TEXT("namesOnly keeps category"),
                    (*MultiplyObj)->HasField(TEXT("category")));
                // ...heavy fields dropped.
                TestFalse(TEXT("namesOnly drops description"),
                    (*MultiplyObj)->HasField(TEXT("description")));
                TestFalse(TEXT("namesOnly drops caption"),
                    (*MultiplyObj)->HasField(TEXT("caption")));
                TestFalse(TEXT("namesOnly drops keywords"),
                    (*MultiplyObj)->HasField(TEXT("keywords")));
                TestFalse(TEXT("namesOnly drops inputPins"),
                    (*MultiplyObj)->HasField(TEXT("inputPins")));
                TestFalse(TEXT("namesOnly drops outputPins"),
                    (*MultiplyObj)->HasField(TEXT("outputPins")));
            }
        }
    }

    // --- explicit fields allow-list returns exactly the requested keys. ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> FieldList;
        FieldList.Add(MakeShared<FJsonValueString>(TEXT("className")));
        FieldList.Add(MakeShared<FJsonValueString>(TEXT("category")));
        Payload->SetArrayField(TEXT("fields"), FieldList);
        FTestResponseCapture Capture = Invoke(Payload);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
            Capture.Result->TryGetArrayField(TEXT("expressions"), Expressions);
            const TSharedPtr<FJsonObject>* MultiplyObj =
                FindRecordByClassName(Expressions, TEXT("MaterialExpressionMultiply"));
            if (TestNotNull(TEXT("fields Multiply present"), MultiplyObj))
            {
                TestTrue(TEXT("fields keeps className"),
                    (*MultiplyObj)->HasField(TEXT("className")));
                TestTrue(TEXT("fields keeps category"),
                    (*MultiplyObj)->HasField(TEXT("category")));
                TestFalse(TEXT("fields drops shortName (not requested)"),
                    (*MultiplyObj)->HasField(TEXT("shortName")));
                TestFalse(TEXT("fields drops description (not requested)"),
                    (*MultiplyObj)->HasField(TEXT("description")));
            }
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSearchExpressionTypesTest,
    "PinWright.material.graph.search_expression_types.KeywordRank",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSearchExpressionTypesTest::RunTest(const FString& Parameters)
{
    // --- query "fresnel" ranks MaterialExpressionFresnel top ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("fresnel"));
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.search_expression_types"), Payload, Capture);
        TestTrue(TEXT("fresnel handler found"), bInvoked);
        TestTrue(TEXT("fresnel success"), Capture.bSuccess);

        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetArrayField(TEXT("results"), Results) &&
            Results && Results->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* Top = nullptr;
            (*Results)[0]->TryGetObject(Top);
            FString TopName;
            if (Top) (*Top)->TryGetStringField(TEXT("className"), TopName);
            TestEqual(TEXT("fresnel top result"), TopName, FString(TEXT("MaterialExpressionFresnel")));

            double Score = 0.0;
            if (Top) (*Top)->TryGetNumberField(TEXT("score"), Score);
            TestTrue(TEXT("fresnel top score > 0"), Score > 0.0);
        }
        else
        {
            AddError(TEXT("fresnel: missing/empty results"));
        }
    }

    // --- query "multiply" ranks MaterialExpressionMultiply top ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("multiply"));
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.search_expression_types"), Payload, Capture);
        TestTrue(TEXT("multiply handler found"), bInvoked);
        TestTrue(TEXT("multiply success"), Capture.bSuccess);

        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetArrayField(TEXT("results"), Results) &&
            Results && Results->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* Top = nullptr;
            (*Results)[0]->TryGetObject(Top);
            FString TopName;
            if (Top) (*Top)->TryGetStringField(TEXT("className"), TopName);
            TestEqual(TEXT("multiply top result"), TopName, FString(TEXT("MaterialExpressionMultiply")));
        }
        else
        {
            AddError(TEXT("multiply: missing/empty results"));
        }
    }

    // --- query "math" with limit=5: <=5 entries, all category contains "Math" ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("math"));
        Payload->SetNumberField(TEXT("limit"), 5.0);
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(
            TEXT("material.graph.search_expression_types"), Payload, Capture);
        TestTrue(TEXT("math handler found"), bInvoked);
        TestTrue(TEXT("math success"), Capture.bSuccess);

        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results)
        {
            TestTrue(TEXT("math results <= 5"), Results->Num() <= 5);
            for (const TSharedPtr<FJsonValue>& V : *Results)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (!V->TryGetObject(Obj) || !Obj) continue;
                FString Cat;
                (*Obj)->TryGetStringField(TEXT("category"), Cat);
                TestTrue(TEXT("math result category contains 'Math'"),
                    Cat.Contains(TEXT("Math"), ESearchCase::IgnoreCase));
            }
        }
        else
        {
            AddError(TEXT("math: missing results array"));
        }
    }

    return true;
}

// Regression guard: pin names are DERIVED, never the raw FName. FExpressionOutput::OutputName
// and UMaterialExpression::GetInputName are NAME_None for most classes, so reading them raw
// reported the literal string "None" for 267 of 346 classes -- five times over on VertexColor,
// whose outputs are built as FExpressionOutput(TEXT(""), mask...) in
// Engine/Private/Materials/MaterialExpressions.cpp:10027-10031. A "None" name is unusable as
// connect_nodes' sourcePin, which forced callers to guess an index by counting positions.
// See Material/MaterialPinNames.h for the engine derivation these assertions pin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialListExpressionTypesDerivedPinNamesTest,
    "PinWright.material.graph.list_expression_types.DerivedPinNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialListExpressionTypesDerivedPinNamesTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("material.graph.list_expression_types"), MakeShared<FJsonObject>(), Capture);
    TestTrue(TEXT("handler found"), bInvoked);
    TestTrue(TEXT("success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

    const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("expressions"), Expressions) || !Expressions)
    {
        AddError(TEXT("Response missing 'expressions' array"));
        return true;
    }

    // Reads one record's pin array into a plain FString array so the expectations below can be
    // written as whole-array equality -- order is load-bearing: element i is sourceOutputIndex i.
    auto ReadPins = [&](const FString& ClassName, const TCHAR* Field, TArray<FString>& Out) -> bool
    {
        Out.Reset();
        const TSharedPtr<FJsonObject>* Record = FindRecordByClassName(Expressions, ClassName);
        if (!Record) return false;
        const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
        if (!(*Record)->TryGetArrayField(Field, Pins) || !Pins) return false;
        for (const TSharedPtr<FJsonValue>& Val : *Pins)
        {
            Out.Add(Val->AsString());
        }
        return true;
    };

    auto CheckPins = [&](const FString& ClassName, const TCHAR* Field, const TArray<FString>& Expected)
    {
        TArray<FString> Actual;
        if (!ReadPins(ClassName, Field, Actual))
        {
            AddError(FString::Printf(TEXT("%s: missing record or '%s' array"), *ClassName, Field));
            return;
        }
        TestEqual(*FString::Printf(TEXT("%s %s"), *ClassName, Field),
            FString::Join(Actual, TEXT(",")), FString::Join(Expected, TEXT(",")));
    };

    // Unnamed masked outputs: the channel bits, in RGBA order. This is the reported defect.
    CheckPins(TEXT("MaterialExpressionVertexColor"), TEXT("outputPins"),
        {TEXT("RGB"), TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")});
    // A two-channel mask generalises the same rule rather than needing a table entry.
    CheckPins(TEXT("MaterialExpressionConstant2Vector"), TEXT("outputPins"),
        {TEXT("RG"), TEXT("R"), TEXT("G")});
    // Declared names are untouched -- TextureSample names all six outputs itself.
    CheckPins(TEXT("MaterialExpressionTextureSample"), TEXT("outputPins"),
        {TEXT("RGB"), TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A"), TEXT("RGBA")});
    // Unnamed AND unmasked input: the engine's own graph-pin fallback stem.
    CheckPins(TEXT("MaterialExpressionGetMaterialAttributes"), TEXT("inputPins"), {TEXT("Input")});
    // Named inputs stay verbatim.
    CheckPins(TEXT("MaterialExpressionMultiply"), TEXT("inputPins"), {TEXT("A"), TEXT("B")});

    // BreakMaterialAttributes names every output; spot-check the first so a regression that
    // replaced declared names with derived ones is caught here too.
    {
        TArray<FString> BreakOutputs;
        if (ReadPins(TEXT("MaterialExpressionBreakMaterialAttributes"), TEXT("outputPins"), BreakOutputs)
            && BreakOutputs.Num() > 0)
        {
            TestEqual(TEXT("BreakMaterialAttributes outputPins[0]"), BreakOutputs[0], FString(TEXT("BaseColor")));
        }
    }

    // No class anywhere may report the literal "None" (or an empty string) for a pin -- that is
    // what the raw FName stringifies to, and it is never a name a caller can pass back in.
    int32 NonePins = 0;
    FString FirstOffender;
    for (const TSharedPtr<FJsonValue>& Val : *Expressions)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Val->TryGetObject(Obj) || !Obj) continue;
        for (const TCHAR* Field : {TEXT("inputPins"), TEXT("outputPins")})
        {
            const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
            if (!(*Obj)->TryGetArrayField(Field, Pins) || !Pins) continue;
            for (const TSharedPtr<FJsonValue>& Pin : *Pins)
            {
                const FString PinName = Pin->AsString();
                if (PinName.Equals(TEXT("None")) || PinName.IsEmpty())
                {
                    ++NonePins;
                    if (FirstOffender.IsEmpty())
                    {
                        FString Cls;
                        (*Obj)->TryGetStringField(TEXT("className"), Cls);
                        FirstOffender = FString::Printf(TEXT("%s.%s"), *Cls, Field);
                    }
                }
            }
        }
    }
    TestEqual(*FString::Printf(TEXT("no empty/None pin names (first offender: %s)"),
        FirstOffender.IsEmpty() ? TEXT("-") : *FirstOffender), NonePins, 0);

    // Cross-check against the engine's own published derivation
    // (UMaterialEditingLibrary::GetMaterialExpressionOutputNames, MaterialEditingLibrary.cpp:1247,
    // whose body is the static GetExpressionOutputName at :1159). The engine returns an empty
    // string for a multi-channel mask and stops there, so the contract asserted is: wherever the
    // engine produces a name we produce the same one, and where it gives up we still produce
    // something usable.
    //
    // The engine only PUBLISHES that derivation from UE 5.8 - GetMaterialExpressionOutputNames
    // does not exist on 5.3-5.7 (its body, the file-static GetExpressionOutputName, is private
    // there). The assertions above, which are this test's actual subject, run on every engine;
    // only the cross-check against the engine's own answer is version-gated, because on older
    // engines there is no published answer to compare with.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    for (const TCHAR* ClassName : {TEXT("MaterialExpressionVertexColor"),
                                   TEXT("MaterialExpressionTextureSample"),
                                   TEXT("MaterialExpressionBreakMaterialAttributes")})
    {
        UClass* Cls = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), ClassName));
        UMaterialExpression* Cdo = Cls ? Cast<UMaterialExpression>(Cls->GetDefaultObject()) : nullptr;
        if (!Cdo)
        {
            AddError(FString::Printf(TEXT("no CDO for %s"), ClassName));
            continue;
        }

        const TArray<FString> EngineNames = UMaterialEditingLibrary::GetMaterialExpressionOutputNames(Cdo);
        TArray<FString> Reported;
        if (!ReadPins(ClassName, TEXT("outputPins"), Reported)) continue;

        TestEqual(*FString::Printf(TEXT("%s output count matches engine"), ClassName),
            Reported.Num(), EngineNames.Num());
        for (int32 i = 0; i < FMath::Min(Reported.Num(), EngineNames.Num()); ++i)
        {
            if (EngineNames[i].IsEmpty())
            {
                TestFalse(*FString::Printf(TEXT("%s[%d] filled in where engine gave up"), ClassName, i),
                    Reported[i].IsEmpty());
            }
            else
            {
                TestEqual(*FString::Printf(TEXT("%s[%d] matches engine derivation"), ClassName, i),
                    Reported[i], EngineNames[i]);
            }
        }
    }
#endif

    return true;
}

// The reported defect verbatim: search_expression_types for VertexColor used to answer
// outputPins: ["None","None","None","None","None"], leaving sourcePin unusable for the node.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSearchExpressionTypesVertexColorPinsTest,
    "PinWright.material.graph.search_expression_types.VertexColorOutputPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSearchExpressionTypesVertexColorPinsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("query"), TEXT("VertexColor"));
    Payload->SetNumberField(TEXT("limit"), 5.0);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(
        TEXT("material.graph.search_expression_types"), Payload, Capture);
    TestTrue(TEXT("handler found"), bInvoked);
    TestTrue(TEXT("success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid()) return true;

    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("results"), Results) || !Results)
    {
        AddError(TEXT("Response missing 'results' array"));
        return true;
    }

    const TSharedPtr<FJsonObject>* Record =
        FindRecordByClassName(Results, TEXT("MaterialExpressionVertexColor"));
    if (!TestNotNull(TEXT("VertexColor in results"), Record)) return true;

    const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
    if (!(*Record)->TryGetArrayField(TEXT("outputPins"), Pins) || !Pins)
    {
        AddError(TEXT("VertexColor record missing 'outputPins'"));
        return true;
    }

    TArray<FString> Names;
    for (const TSharedPtr<FJsonValue>& Pin : *Pins)
    {
        Names.Add(Pin->AsString());
    }
    TestEqual(TEXT("VertexColor outputPins"), FString::Join(Names, TEXT(",")),
        FString(TEXT("RGB,R,G,B,A")));
    TestFalse(TEXT("no None among VertexColor outputPins"),
        ArrayContainsString(Pins, TEXT("None")));

    return true;
}
