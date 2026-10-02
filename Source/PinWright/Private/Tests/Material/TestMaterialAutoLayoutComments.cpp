// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestMaterialAutoLayoutComments.cpp - material.authoring.auto_layout and comment boxes.
//
// A comment boxes a positioned constant just left of the material output, exactly where the
// unpositioned constant feeding Base Color would land. The verb must keep the moved constant out
// of the comment, leave the comment's rect alone (its only member did not move), and report an
// empty commentsRefit[] plus sizeSource counts. Fails if comments are ignored by the layout (the
// constant lands inside the box) or if the response drops either field.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Layout/PwGraphLayoutMaterial.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAutoLayoutCommentObstacleTest,
    "PinWright.material.authoring.auto_layout.CommentAroundPositionedNodesIsObstacle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAutoLayoutCommentObstacleTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("/Game/PinWrightTests/__PW_GatewayTests/AutoLayoutComment_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("sandbox package"), Package))
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };
    UMaterial* Material = NewObject<UMaterial>(Package, FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        RF_Public | RF_Standalone | RF_Transactional);
    UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData();

    auto AddConstant = [Material, EditorOnly](int32 X, int32 Y)
    {
        UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transactional);
        Constant->MaterialExpressionGuid = FGuid::NewGuid();
        Constant->MaterialExpressionEditorX = X;
        Constant->MaterialExpressionEditorY = Y;
        EditorOnly->ExpressionCollection.Expressions.Add(Constant);
        return Constant;
    };
    UMaterialExpressionConstant* Unpositioned = AddConstant(0, 0);
    EditorOnly->BaseColor.Expression = Unpositioned;
    AddConstant(-560, 160);

    UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Material, NAME_None, RF_Transactional);
    Comment->MaterialExpressionGuid = FGuid::NewGuid();
    Comment->MaterialExpressionEditorX = -600;
    Comment->MaterialExpressionEditorY = -120;
    Comment->SizeX = 560;
    Comment->SizeY = 420;
    EditorOnly->ExpressionCollection.EditorComments.Add(Comment);
    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);
    const FIntRect CommentRect(-600, -120, -40, 300);

    // Without the comment, the constant lands inside the box: the fixture exercises the obstacle.
    {
        PwGraphLayout::FMaterialModel Probe = PwGraphLayout::BuildMaterialModel(Material, nullptr);
        Probe.Layout.Comments.Reset();
        PwGraphLayout::Arrange(Probe.Layout, PwGraphLayout::FSpacing());
        const int32 Index = Probe.Expressions.IndexOfByKey(Unpositioned);
        const PwGraphLayout::FLayoutNode& Node = Probe.Layout.Nodes[Index];
        TestTrue(TEXT("without the comment the constant would land inside it"),
            Node.Position.X < CommentRect.Max.X && CommentRect.Min.X < Node.Position.X + Node.Size.X
            && Node.Position.Y < CommentRect.Max.Y && CommentRect.Min.Y < Node.Position.Y + Node.Size.Y);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"), InvokeHandlerWithCapture(TEXT("material.authoring.auto_layout"), Payload, Capture));
    if (!TestTrue(TEXT("response reports success"), Capture.bSuccess && Capture.Result.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("the unpositioned constant moved"), static_cast<int32>(Capture.Result->GetNumberField(TEXT("movedCount"))), 1);

    const TArray<TSharedPtr<FJsonValue>>* Refit = nullptr;
    TestTrue(TEXT("commentsRefit[] is reported, empty: the comment's only member did not move"),
        Capture.Result->TryGetArrayField(TEXT("commentsRefit"), Refit) && Refit->Num() == 0);
    TestTrue(TEXT("the comment keeps its rect"),
        FIntRect(Comment->MaterialExpressionEditorX, Comment->MaterialExpressionEditorY,
            Comment->MaterialExpressionEditorX + Comment->SizeX, Comment->MaterialExpressionEditorY + Comment->SizeY) == CommentRect);
    const TSharedPtr<FJsonObject>* SizeSource = nullptr;
    TestTrue(TEXT("sizeSource reports the estimated material node sizes"),
        Capture.Result->TryGetObjectField(TEXT("sizeSource"), SizeSource)
        && (*SizeSource)->GetNumberField(TEXT("measured")) == 0 && (*SizeSource)->GetNumberField(TEXT("estimated")) > 0);

    const PwGraphLayout::FMaterialModel After = PwGraphLayout::BuildMaterialModel(Material, nullptr);
    const PwGraphLayout::FLayoutNode& Moved = After.Layout.Nodes[After.Expressions.IndexOfByKey(Unpositioned)];
    TestFalse(TEXT("the moved constant stays out of the comment"),
        Moved.Position.X < CommentRect.Max.X && CommentRect.Min.X < Moved.Position.X + Moved.Size.X
        && Moved.Position.Y < CommentRect.Max.Y && CommentRect.Min.Y < Moved.Position.Y + Moved.Size.Y);
    return true;
}
