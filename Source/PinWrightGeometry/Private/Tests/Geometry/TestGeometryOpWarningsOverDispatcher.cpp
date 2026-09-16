// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the geometry warnings-at-the-wrapper-boundary defect.
//
// GeometryOps::FOpResult has carried a Warnings array since the op extraction, and every clamp
// in the ops layer appends to it ("a clamp reports itself" is a contract rule for all five op
// families). NO RPC wrapper read it. The consequence was measurable and silent: a caller passing
// `segments: 1` to geometry.create_cylinder got a bare success response and never learned the
// engine had used 3, and every other clamp - the sphere's subdivision floor, the revolve step
// floor, the subdivide iteration cap - was equally invisible. The only consumer was the .pwmodel
// compiler, which routes the same warnings to PWMODEL_STAGE_WARNING.
//
// The fix is GeometryOps::AddOpWarnings (Handlers/Geometry/GeometryOpWarnings.h), called by every
// geometry.* wrapper immediately before its SendSuccess.
//
// These tests drive the REAL dispatcher with JSON, because that is the surface the defect lived
// on: the ops-layer tests in TestGeometryOpsPrimitives.cpp already asserted Op.Warnings was
// populated and passed the entire time the field was being discarded one layer up. Asserting the
// op is not enough - the assertion has to be made on the response.
//
// The negative control matters as much as the positives. `warnings` is emitted ONLY when the op
// produced at least one, which is what keeps this additive: the ~146 automation tests that drive
// the dispatcher and assert on response fields all make warning-free calls, so their responses
// are byte-identical. AbsentWhenNothingWasClamped is what would fail if someone switched the
// field to always-present.
//
// Counterfactual: remove the AddOpWarnings call from any one wrapper and that verb's case here
// fails on the missing `warnings` field while everything else stays green.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/World.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

namespace
{
    // Collect the response's `warnings` array as plain strings. Returns false when the field is
    // absent, which is the shape a warning-free call must keep.
    bool GeomWarnReadResponseArray(const TSharedPtr<FJsonObject>& Result, TArray<FString>& Out)
    {
        Out.Reset();
        if (!Result.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Result->TryGetArrayField(TEXT("warnings"), Values) || Values == nullptr)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (Value.IsValid())
            {
                Out.Add(Value->AsString());
            }
        }
        return true;
    }

    // True when some warning names both the value the caller passed and the value the op used.
    // Substring matching on the pair, not on the whole sentence: the assertion is about the two
    // numbers being reported, not about the wording of ClampRangeWarn's format string.
    bool GeomWarnMentionsClamp(const TArray<FString>& Warnings, const TCHAR* Label, int32 From, int32 To)
    {
        const FString Expected = FString::Printf(TEXT("%s clamped from %d to %d"), Label, From, To);
        for (const FString& Warning : Warnings)
        {
            if (Warning.Contains(Expected))
            {
                return true;
            }
        }
        return false;
    }
}

// A clamped input on a create verb is reported in the response, on three verbs whose floors have
// three different shapes: an engine floor above the clamp helper's own (cylinder segments 3), a
// "<= 0 means unset" substitution that also fixes a lying echo (stairs numSteps), and a floor of
// 0 where 0 is legal (plane subdivisions).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateVerbsReportClampsInResponseTest,
    "PinWright.geometry.warnings.CreateVerbsReportClampsInResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateVerbsReportClampsInResponseTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping geometry warning-response test"));
        return true;
    }

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TArray<FString> Labels;
    auto Run = [&](const TCHAR* Method, const TFunction<void(TSharedPtr<FJsonObject>&)>& Fill,
                   bool& bOutSuccess, TSharedPtr<FJsonObject>& OutResult)
    {
        const FString Label = FString::Printf(TEXT("PW_GeomWarnProbe_%s_%d"), *Suffix, Labels.Num());
        Labels.Add(Label);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        Fill(Params);

        FString ErrorCode;
        Dispatch(Dispatcher, Sink, Method, FString::Printf(TEXT("req-geom-warn-%s"), Method),
            Params, bOutSuccess, OutResult, ErrorCode);
    };

    // --- geometry.create_cylinder: segments below the engine's AngleSamples floor of 3. -----
    {
        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        Run(TEXT("geometry.create_cylinder"),
            [](TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("segments"), 1.0); },
            bSuccess, Result);

        TestTrue(TEXT("create_cylinder with segments=1 still succeeds"), bSuccess);
        TArray<FString> Warnings;
        if (TestTrue(TEXT("create_cylinder response carries a warnings array"),
                GeomWarnReadResponseArray(Result, Warnings)))
        {
            TestTrue(TEXT("create_cylinder reports segments 1 -> 3"),
                GeomWarnMentionsClamp(Warnings, TEXT("segments"), 1, 3));
        }
    }

    // --- geometry.create_stairs: numSteps=0. The echo used to say 0 for a 1-step staircase. --
    {
        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        Run(TEXT("geometry.create_stairs"),
            [](TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("numSteps"), 0.0); },
            bSuccess, Result);

        TestTrue(TEXT("create_stairs with numSteps=0 still succeeds"), bSuccess);
        TArray<FString> Warnings;
        if (TestTrue(TEXT("create_stairs response carries a warnings array"),
                GeomWarnReadResponseArray(Result, Warnings)))
        {
            TestTrue(TEXT("create_stairs reports numSteps 0 -> 8"),
                GeomWarnMentionsClamp(Warnings, TEXT("numSteps"), 0, 8));
        }
        // The echo is the EFFECTIVE count, not the request. This is the one place the old echo
        // was an outright lie rather than an omission.
        double Echoed = -1.0;
        if (Result.IsValid() && TestTrue(TEXT("create_stairs echoes numSteps"),
                Result->TryGetNumberField(TEXT("numSteps"), Echoed)))
        {
            TestEqual(TEXT("create_stairs echoes the count the geometry was built with"),
                static_cast<int32>(Echoed), 8);
        }
    }

    // --- geometry.create_plane: a negative subdivision count, whose floor is 0 and not 3. -----
    {
        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        Run(TEXT("geometry.create_plane"),
            [](TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("widthSubdivisions"), -5.0); },
            bSuccess, Result);

        TestTrue(TEXT("create_plane with widthSubdivisions=-5 still succeeds"), bSuccess);
        TArray<FString> Warnings;
        if (TestTrue(TEXT("create_plane response carries a warnings array"),
                GeomWarnReadResponseArray(Result, Warnings)))
        {
            TestTrue(TEXT("create_plane reports widthSubdivisions -5 -> 0"),
                GeomWarnMentionsClamp(Warnings, TEXT("widthSubdivisions"), -5, 0));
        }
    }

    // --- geometry.revolve: steps below the floor, which follows `angle`. ----------------------
    //
    // `angle` defaults to 360, so the default request is a CLOSED revolution and its floor is 3,
    // not the engine's 2. The second block is the partial sweep, where 2 is legal and is what a
    // caller gets. Both are here because one number would not distinguish the two, and the echo
    // has to carry whichever floor actually ran.
    {
        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        Run(TEXT("geometry.revolve"),
            [](TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("steps"), 1.0); },
            bSuccess, Result);

        TestTrue(TEXT("revolve with steps=1 still succeeds"), bSuccess);
        TArray<FString> Warnings;
        if (TestTrue(TEXT("revolve response carries a warnings array"),
                GeomWarnReadResponseArray(Result, Warnings)))
        {
            TestTrue(TEXT("revolve reports steps 1 -> 3 at the default angle of 360"),
                GeomWarnMentionsClamp(Warnings, TEXT("steps"), 1, 3));
        }
        double Echoed = -1.0;
        if (Result.IsValid() && TestTrue(TEXT("revolve echoes steps"),
                Result->TryGetNumberField(TEXT("steps"), Echoed)))
        {
            TestEqual(TEXT("revolve echoes the step count the sweep was built with"),
                static_cast<int32>(Echoed), 3);
        }
    }

    // --- geometry.revolve: a PARTIAL sweep keeps the engine's floor of 2. ---------------------
    {
        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        Run(TEXT("geometry.revolve"),
            [](TSharedPtr<FJsonObject>& P)
            {
                P->SetNumberField(TEXT("steps"), 1.0);
                P->SetNumberField(TEXT("angle"), 180.0);
            },
            bSuccess, Result);

        TestTrue(TEXT("a half revolve with steps=1 still succeeds"), bSuccess);
        TArray<FString> Warnings;
        if (TestTrue(TEXT("the half revolve carries a warnings array"),
                GeomWarnReadResponseArray(Result, Warnings)))
        {
            TestTrue(TEXT("a partial revolve reports steps 1 -> 2, not 1 -> 3"),
                GeomWarnMentionsClamp(Warnings, TEXT("steps"), 1, 2));
        }
        double Echoed = -1.0;
        if (Result.IsValid() && TestTrue(TEXT("the half revolve echoes steps"),
                Result->TryGetNumberField(TEXT("steps"), Echoed)))
        {
            TestEqual(TEXT("and the echo is the partial sweep's floor"),
                static_cast<int32>(Echoed), 2);
        }
    }

    for (const FString& Label : Labels)
    {
        DestroyActorsWithLabel(Label);
    }
    return true;
}

// The modify family surfaces its warnings too - the fix is not a PrimitiveHandler detail.
// geometry.subdivide clamps iterations into [1, GEOM_MAX_SUBDIVIDE_ITERATIONS] inside
// GeometryOps::Subdivide (MeshOpsHandler.cpp's wrapper), and echoed the clamped count while
// saying nothing about the clamp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySubdivideReportsIterationClampTest,
    "PinWright.geometry.warnings.SubdivideReportsIterationClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySubdivideReportsIterationClampTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping subdivide warning-response test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_GeomWarnSubdiv_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("name"), Label);
        bool bCreated = false;
        FString CreateErr;
        Dispatch(Dispatcher, Sink, TEXT("geometry.create_box"),
            TEXT("req-geom-warn-subdiv-create"), CreateParams, bCreated, CreateErr);
        if (!TestTrue(TEXT("geometry.create_box spawned the probe DynamicMeshActor"), bCreated))
        {
            DestroyActorsWithLabel(Label);
            return true;
        }
    }

    // 0 iterations, not 99: the clamp is the thing under test, and one subdivision pass keeps
    // the test cheap where six would quadruple the box's triangle count six times over.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Label);
    Params->SetNumberField(TEXT("iterations"), 0.0);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.subdivide"), TEXT("req-geom-warn-subdiv"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("subdivide with iterations=0 still succeeds"), bSuccess);
    TArray<FString> Warnings;
    if (TestTrue(TEXT("subdivide response carries a warnings array"),
            GeomWarnReadResponseArray(Result, Warnings)))
    {
        bool bMentionsTheClamp = false;
        for (const FString& Warning : Warnings)
        {
            bMentionsTheClamp |= Warning.Contains(TEXT("iterations clamped from 0 to 1"));
        }
        TestTrue(TEXT("subdivide reports iterations 0 -> 1"), bMentionsTheClamp);
    }

    DestroyActorsWithLabel(Label);
    return true;
}

// The negative control for the emission convention: a call that trips no clamp carries NO
// `warnings` key at all. This is what makes the change additive - every existing dispatcher test
// makes calls of exactly this shape, and their responses must be byte-identical to before.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryWarningsAbsentWhenNothingWasClampedTest,
    "PinWright.geometry.warnings.AbsentWhenNothingWasClamped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryWarningsAbsentWhenNothingWasClampedTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping warning-absence test"));
        return true;
    }

    const FString Label = FString::Printf(TEXT("PW_GeomNoWarnProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), Label);
    Params->SetNumberField(TEXT("segments"), 16.0);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("geometry.create_cylinder"), TEXT("req-geom-nowarn"),
        Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("create_cylinder with in-range params succeeds"), bSuccess);
    if (TestTrue(TEXT("create_cylinder returns a result object"), Result.IsValid()))
    {
        TestFalse(TEXT("an unclamped create emits no warnings field at all"),
            Result->HasField(TEXT("warnings")));
    }

    DestroyActorsWithLabel(Label);
    return true;
}
