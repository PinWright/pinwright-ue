// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for material.authoring.auto_layout.
//
// Each test builds a material in a sandbox package with two transactional
// UMaterialExpressionConstant nodes and invokes the handler via the dispatcher.
// - ReflowsUnpositionedExpressions: both at (0,0) -> movedCount 2, and moved[]
//   matches the expressions' read-back positions.
// - PositionedGraphReportsZeroMoved: both already placed -> movedCount 0, no
//   transaction. Fails if the verb reports the total expression count again.
// - UndoRestoresPositions: one editor undo puts both back at (0,0). Fails if the
//   moves leave the FScopedTransaction / Modify() path.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace MaterialAutoLayoutTest
{
    const TCHAR* TransactionName = TEXT("PinWright: material.authoring.auto_layout");

    // A sandbox material holding two constants at the given positions.
    struct FFixture
    {
        FString AssetPath;
        UMaterial* Material = nullptr;
        TArray<UMaterialExpressionConstant*> Constants;

        bool Create(const FIntPoint& A, const FIntPoint& B)
        {
            AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/AutoLayout_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            UPackage* Pkg = CreatePackage(*AssetPath);
            if (!Pkg)
            {
                return false;
            }
            Material = NewObject<UMaterial>(Pkg, FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
                RF_Public | RF_Standalone | RF_Transactional);
            for (const FIntPoint& Position : { A, B })
            {
                UMaterialExpressionConstant* Constant =
                    NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
                Constant->MaterialExpressionGuid = FGuid::NewGuid();
                Constant->MaterialExpressionEditorX = Position.X;
                Constant->MaterialExpressionEditorY = Position.Y;
                Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Constant);
                Constants.Add(Constant);
            }
            Material->PostEditChange();
            FAssetRegistryModule::AssetCreated(Material);
            return true;
        }

        ~FFixture()
        {
            if (!AssetPath.IsEmpty())
            {
                CleanupTestAsset(AssetPath);
            }
        }

        bool Invoke(FAutomationTestBase& Test, FTestResponseCapture& Capture) const
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("assetPath"), AssetPath);
            Test.TestTrue(TEXT("handler registered"),
                InvokeHandlerWithCapture(TEXT("material.authoring.auto_layout"), Payload, Capture));
            return Test.TestTrue(TEXT("response reports success"), Capture.bSuccess && Capture.Result.IsValid());
        }

        FIntPoint PositionOf(int32 Index) const
        {
            return FIntPoint(Constants[Index]->MaterialExpressionEditorX, Constants[Index]->MaterialExpressionEditorY);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAutoLayoutTest,
    "PinWright.material.authoring.auto_layout.ReflowsUnpositionedExpressions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringAutoLayoutTest::RunTest(const FString& Parameters)
{
    using namespace MaterialAutoLayoutTest;
    FFixture Fixture;
    if (!TestTrue(TEXT("fixture created"), Fixture.Create(FIntPoint(0, 0), FIntPoint(0, 0))))
    {
        return false;
    }
    FTestResponseCapture Capture;
    if (!Fixture.Invoke(*this, Capture))
    {
        return false;
    }
    TestEqual(TEXT("target echoes assetPath"), Capture.Result->GetStringField(TEXT("target")), Fixture.AssetPath);
    TestEqual(TEXT("transaction is named"), Capture.Result->GetStringField(TEXT("transaction")), FString(TransactionName));

    // Load-bearing: both constants left (0,0), and the report matches that readback.
    int32 ActuallyMoved = 0;
    for (int32 Index = 0; Index < Fixture.Constants.Num(); ++Index)
    {
        ActuallyMoved += Fixture.PositionOf(Index) != FIntPoint(0, 0) ? 1 : 0;
    }
    TestEqual(TEXT("both constants were repositioned"), ActuallyMoved, 2);
    TestEqual(TEXT("movedCount matches the readback"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("movedCount"))), ActuallyMoved);
    TestEqual(TEXT("unchangedCount"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("unchangedCount"))), 0);

    const TArray<TSharedPtr<FJsonValue>>& Moved = Capture.Result->GetArrayField(TEXT("moved"));
    TestEqual(TEXT("moved[] has one entry per moved expression"), Moved.Num(), ActuallyMoved);
    for (const TSharedPtr<FJsonValue>& Value : Moved)
    {
        const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
        const FString NodeId = Entry->GetStringField(TEXT("nodeId"));
        const int32 Index = Fixture.Constants.IndexOfByPredicate([&NodeId](const UMaterialExpressionConstant* C)
        {
            return C->MaterialExpressionGuid.ToString() == NodeId;
        });
        if (!TestTrue(TEXT("moved[].nodeId names a fixture expression"), Index != INDEX_NONE))
        {
            continue;
        }
        const TSharedPtr<FJsonObject>& From = Entry->GetObjectField(TEXT("from"));
        const TSharedPtr<FJsonObject>& To = Entry->GetObjectField(TEXT("to"));
        TestTrue(TEXT("moved[].from is the origin"), From->GetNumberField(TEXT("x")) == 0 && From->GetNumberField(TEXT("y")) == 0);
        TestTrue(TEXT("moved[].to is the read-back position"),
            FIntPoint(static_cast<int32>(To->GetNumberField(TEXT("x"))), static_cast<int32>(To->GetNumberField(TEXT("y"))))
                == Fixture.PositionOf(Index));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAutoLayoutPositionedTest,
    "PinWright.material.authoring.auto_layout.PositionedGraphReportsZeroMoved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringAutoLayoutPositionedTest::RunTest(const FString& Parameters)
{
    using namespace MaterialAutoLayoutTest;
    FFixture Fixture;
    const FIntPoint A(-400, 32);
    const FIntPoint B(-400, 256);
    if (!TestTrue(TEXT("fixture created"), Fixture.Create(A, B)))
    {
        return false;
    }
    const int32 QueueBefore = GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
    FTestResponseCapture Capture;
    if (!Fixture.Invoke(*this, Capture))
    {
        return false;
    }
    TestEqual(TEXT("movedCount is 0 on a positioned graph"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("movedCount"))), 0);
    TestEqual(TEXT("unchangedCount covers every expression"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("unchangedCount"))), 2);
    TestEqual(TEXT("moved[] is empty"), Capture.Result->GetArrayField(TEXT("moved")).Num(), 0);
    TestFalse(TEXT("no transaction is reported"), Capture.Result->HasField(TEXT("transaction")));
    TestTrue(TEXT("positions untouched"), Fixture.PositionOf(0) == A && Fixture.PositionOf(1) == B);
    if (GEditor && GEditor->Trans)
    {
        TestEqual(TEXT("no undo entry is recorded"), GEditor->Trans->GetQueueLength(), QueueBefore);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringAutoLayoutUndoTest,
    "PinWright.material.authoring.auto_layout.UndoRestoresPositions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAuthoringAutoLayoutUndoTest::RunTest(const FString& Parameters)
{
    using namespace MaterialAutoLayoutTest;
    if (!TestTrue(TEXT("editor transactor available"), GEditor && GEditor->Trans))
    {
        return false;
    }
    FFixture Fixture;
    if (!TestTrue(TEXT("fixture created"), Fixture.Create(FIntPoint(0, 0), FIntPoint(0, 0))))
    {
        return false;
    }
    FTestResponseCapture Capture;
    if (!Fixture.Invoke(*this, Capture))
    {
        return false;
    }
    if (!TestEqual(TEXT("movedCount"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("movedCount"))), 2))
    {
        return false;
    }
    TestEqual(TEXT("the latest undo entry is the auto_layout transaction"),
        GEditor->Trans->GetUndoContext(false).Title.ToString(), FString(TransactionName));
    TestTrue(TEXT("undo succeeds"), GEditor->UndoTransaction(/*bCanRedo=*/false));
    TestTrue(TEXT("undo restores both positions"),
        Fixture.PositionOf(0) == FIntPoint(0, 0) && Fixture.PositionOf(1) == FIntPoint(0, 0));
    return true;
}
