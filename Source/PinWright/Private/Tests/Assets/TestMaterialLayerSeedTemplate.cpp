// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red / acceptance test for F-material-layer-seed-default-template:
// material.authoring.create_material_layer and .create_material_layer_blend must seed the same
// validation-passing default template UE's content-browser workflow produces — a layer whose body
// has a MakeMaterialAttributes (or SetMaterialAttributes) feeding a FunctionOutput, and a blend
// whose body has two MaterialAttributes FunctionInputs (Bottom/Top) feeding a
// BlendMaterialAttributes into a FunctionOutput.
//
// Pre-fix both handlers create the concrete asset via the engine factory (which only NewObject()s +
// SetMaterialFunctionUsage()s — see EditorFactories.cpp) and then set nothing but Description /
// bExposeToLibrary, so the function body is EMPTY (GetExpressions().Num() == 0). A layer stack built
// from these hollow assets fails compile_material ("Blend 0 must have two MaterialAttributes inputs"
// then "Missing function input 'BottomLayer'/'TopLayer'") until the caller hand-authors ~25 nodes.
//
// This test drives the two production create verbs and asserts the created function bodies carry the
// seeded template. It fails today precisely because the bodies are empty; once the create verbs seed
// the default template, the expression-shape assertions below pass.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Compat/EngineVersionCompat.h"
#include "Tests/TestUtils.h"

#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionBlendMaterialAttributes.h"

namespace
{
    // Count the expressions in a material function's body that are (a subclass of) Cls.
    // Named distinctively so an anonymous-namespace copy can't ODR-collide with a sibling TU
    // when Unity merges files.
    int32 CountLayerSeedExprOfClass(UMaterialFunctionInterface* Function, UClass* Cls)
    {
        int32 Count = 0;
        if (!Function || !Cls) return Count;
        for (const TObjectPtr<UMaterialExpression>& Expr : Function->GetExpressions())
        {
            if (Expr && Expr->IsA(Cls))
            {
                ++Count;
            }
        }
        return Count;
    }

    // First expression in the body that is (a subclass of) Cls, or null. Used to reach into the
    // seeded graph and assert node properties / wiring, not just counts.
    UMaterialExpression* FindFirstLayerSeedExprOfClass(UMaterialFunctionInterface* Function, UClass* Cls)
    {
        if (!Function || !Cls) return nullptr;
        for (const TObjectPtr<UMaterialExpression>& Expr : Function->GetExpressions())
        {
            if (Expr && Expr->IsA(Cls))
            {
                return Expr;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialLayerSeedTemplate,
    "PinWright.Material.Authoring.LayerSeedTemplate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialLayerSeedTemplate::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Dir = TEXT("/Game/PinWrightTests");

    const FString LayerName = FString::Printf(TEXT("ML_SeedTemplate_%s"), *Suffix);
    const FString BlendName = FString::Printf(TEXT("MLB_SeedTemplate_%s"), *Suffix);
    const FString LayerPackagePath = FString::Printf(TEXT("%s/%s"), *Dir, *LayerName);
    const FString BlendPackagePath = FString::Printf(TEXT("%s/%s"), *Dir, *BlendName);
    const FString LayerObjectPath = FString::Printf(TEXT("%s.%s"), *LayerPackagePath, *LayerName);
    const FString BlendObjectPath = FString::Printf(TEXT("%s.%s"), *BlendPackagePath, *BlendName);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(LayerPackagePath);
        CleanupTestAsset(BlendPackagePath);
    };

    // ---- create_material_layer -> UMaterialFunctionMaterialLayer -----------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), LayerName);
        Payload->SetStringField(TEXT("path"), Dir);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("create_material_layer handler registered"),
            InvokeHandlerWithCapture(TEXT("material.authoring.create_material_layer"), Payload, Capture));
        TestTrue(TEXT("create_material_layer succeeded"), Capture.bSuccess);
    }

    UMaterialFunctionMaterialLayer* Layer =
        LoadObject<UMaterialFunctionMaterialLayer>(nullptr, *LayerObjectPath);
    TestNotNull(TEXT("material layer asset created"), Layer);
    if (Layer)
    {
        // Pre-fix: the factory asset has an empty body -> 0 expressions.
        TestTrue(TEXT("layer body is seeded with a default template (non-empty expression graph)"),
            Layer->GetExpressions().Num() > 0);
        TestTrue(TEXT("layer template has a function output"),
            CountLayerSeedExprOfClass(Layer, UMaterialExpressionFunctionOutput::StaticClass()) >= 1);
        const int32 AttrSourceCount =
            CountLayerSeedExprOfClass(Layer, UMaterialExpressionMakeMaterialAttributes::StaticClass()) +
            CountLayerSeedExprOfClass(Layer, UMaterialExpressionSetMaterialAttributes::StaticClass());
        TestTrue(TEXT("layer template seeds a MakeMaterialAttributes / SetMaterialAttributes source"),
            AttrSourceCount >= 1);

        // Counts alone can't prove the graph compiles: assert the input's compile-critical properties
        // and the actual wiring. A regression that drops the preview-default or miswires the graph
        // (ConnectMaterialExpressions silently no-ops on a bad pin name) leaves counts green but fails
        // the ticket's real acceptance (drops into set_material_layer_stack + compiles first try).
        UMaterialExpressionFunctionInput* LayerInput = Cast<UMaterialExpressionFunctionInput>(
            FindFirstLayerSeedExprOfClass(Layer, UMaterialExpressionFunctionInput::StaticClass()));
        TestNotNull(TEXT("layer template has a FunctionInput"), LayerInput);
        if (LayerInput)
        {
            TestEqual(TEXT("layer input is typed MaterialAttributes"),
                (int32)LayerInput->InputType, (int32)FunctionInput_MaterialAttributes);
            TestTrue(TEXT("layer input uses preview value as default (else 'Missing function input')"),
                LayerInput->bUsePreviewValueAsDefault != 0);
            TestTrue(TEXT("layer input has a valid (engine-seeded) Id"), LayerInput->Id.IsValid());
        }

        UMaterialExpressionSetMaterialAttributes* LayerSet = Cast<UMaterialExpressionSetMaterialAttributes>(
            FindFirstLayerSeedExprOfClass(Layer, UMaterialExpressionSetMaterialAttributes::StaticClass()));
        UMaterialExpressionFunctionOutput* LayerOut = Cast<UMaterialExpressionFunctionOutput>(
            FindFirstLayerSeedExprOfClass(Layer, UMaterialExpressionFunctionOutput::StaticClass()));
        if (LayerInput && LayerSet && LayerSet->Inputs.Num() > 0)
        {
            TestTrue(TEXT("SetMaterialAttributes base input wired from the layer FunctionInput"),
                LayerSet->Inputs[0].Expression == (UMaterialExpression*)LayerInput);
        }
        if (LayerSet && LayerOut)
        {
            TestTrue(TEXT("layer FunctionOutput wired from SetMaterialAttributes"),
                LayerOut->A.Expression == (UMaterialExpression*)LayerSet);
        }
    }

    // ---- create_material_layer_blend -> UMaterialFunctionMaterialLayerBlend ------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), BlendName);
        Payload->SetStringField(TEXT("path"), Dir);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("create_material_layer_blend handler registered"),
            InvokeHandlerWithCapture(TEXT("material.authoring.create_material_layer_blend"), Payload, Capture));
        TestTrue(TEXT("create_material_layer_blend succeeded"), Capture.bSuccess);
    }

    UMaterialFunctionMaterialLayerBlend* Blend =
        LoadObject<UMaterialFunctionMaterialLayerBlend>(nullptr, *BlendObjectPath);
    TestNotNull(TEXT("material layer blend asset created"), Blend);
    if (Blend)
    {
        // Pre-fix: the factory asset has an empty body -> 0 expressions.
        TestTrue(TEXT("blend body is seeded with a default template (non-empty expression graph)"),
            Blend->GetExpressions().Num() > 0);
        TestTrue(TEXT("blend template has a BlendMaterialAttributes node"),
            CountLayerSeedExprOfClass(Blend, UMaterialExpressionBlendMaterialAttributes::StaticClass()) >= 1);
        TestTrue(TEXT("blend template has a function output"),
            CountLayerSeedExprOfClass(Blend, UMaterialExpressionFunctionOutput::StaticClass()) >= 1);
        // The layer-blend validator requires two MaterialAttributes inputs (Bottom/Top).
        TestTrue(TEXT("blend template has two MaterialAttributes function inputs"),
            CountLayerSeedExprOfClass(Blend, UMaterialExpressionFunctionInput::StaticClass()) >= 2);

        // The layer stack binds the two blend inputs by name (Top Layer / Bottom Layer) and, on
        // UE 5.7+, by BlendInputRelevance (MaterialExpressions.cpp / MaterialExpressionLayerStack.cpp);
        // counts don't prove either. Pre-5.7 engines have neither the BlendInputRelevance field nor
        // the Top/BottomMaterialBlendInputName constants; the stack binds by literal name alone.
        // Locate each input (by relevance on 5.7+, by name pre-5.7) and assert its name, type,
        // preview-default, and a valid distinct Id, then assert the A/B wiring so a miswire or
        // dropped relevance turns this red.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        const FName ExpectedTopName(TopMaterialBlendInputName);
        const FName ExpectedBottomName(BottomMaterialBlendInputName);
#else
        const FName ExpectedTopName(TEXT("Top Layer"));
        const FName ExpectedBottomName(TEXT("Bottom Layer"));
#endif
        UMaterialExpressionFunctionInput* BlendTop = nullptr;
        UMaterialExpressionFunctionInput* BlendBottom = nullptr;
        for (const TObjectPtr<UMaterialExpression>& Expr : Blend->GetExpressions())
        {
            if (UMaterialExpressionFunctionInput* FnInput = Cast<UMaterialExpressionFunctionInput>(Expr))
            {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
                if (FnInput->BlendInputRelevance == EBlendInputRelevance::Top) BlendTop = FnInput;
                else if (FnInput->BlendInputRelevance == EBlendInputRelevance::Bottom) BlendBottom = FnInput;
#else
                if (FnInput->InputName == ExpectedTopName) BlendTop = FnInput;
                else if (FnInput->InputName == ExpectedBottomName) BlendBottom = FnInput;
#endif
            }
        }
        TestNotNull(TEXT("blend has a Top-relevance FunctionInput"), BlendTop);
        TestNotNull(TEXT("blend has a Bottom-relevance FunctionInput"), BlendBottom);
        if (BlendTop)
        {
            TestTrue(TEXT("blend Top input named 'Top Layer'"),
                BlendTop->InputName == ExpectedTopName);
            TestEqual(TEXT("blend Top input is typed MaterialAttributes"),
                (int32)BlendTop->InputType, (int32)FunctionInput_MaterialAttributes);
            TestTrue(TEXT("blend Top input uses preview value as default"),
                BlendTop->bUsePreviewValueAsDefault != 0);
            TestTrue(TEXT("blend Top input has a valid Id"), BlendTop->Id.IsValid());
        }
        if (BlendBottom)
        {
            TestTrue(TEXT("blend Bottom input named 'Bottom Layer'"),
                BlendBottom->InputName == ExpectedBottomName);
            TestEqual(TEXT("blend Bottom input is typed MaterialAttributes"),
                (int32)BlendBottom->InputType, (int32)FunctionInput_MaterialAttributes);
            TestTrue(TEXT("blend Bottom input uses preview value as default"),
                BlendBottom->bUsePreviewValueAsDefault != 0);
            TestTrue(TEXT("blend Bottom input has a valid Id"), BlendBottom->Id.IsValid());
        }
        if (BlendTop && BlendBottom)
        {
            // The two inputs must not collide on Id (the engine's per-node deterministic seeding).
            TestTrue(TEXT("blend Top/Bottom inputs have distinct Ids"),
                BlendTop->Id != BlendBottom->Id);
        }

        UMaterialExpressionBlendMaterialAttributes* BlendNode = Cast<UMaterialExpressionBlendMaterialAttributes>(
            FindFirstLayerSeedExprOfClass(Blend, UMaterialExpressionBlendMaterialAttributes::StaticClass()));
        UMaterialExpressionFunctionOutput* BlendOut = Cast<UMaterialExpressionFunctionOutput>(
            FindFirstLayerSeedExprOfClass(Blend, UMaterialExpressionFunctionOutput::StaticClass()));
        if (BlendNode && BlendBottom && BlendTop)
        {
            TestTrue(TEXT("BlendMaterialAttributes A wired from the Bottom input"),
                BlendNode->A.Expression == (UMaterialExpression*)BlendBottom);
            TestTrue(TEXT("BlendMaterialAttributes B wired from the Top input"),
                BlendNode->B.Expression == (UMaterialExpression*)BlendTop);
        }
        if (BlendNode && BlendOut)
        {
            TestTrue(TEXT("blend FunctionOutput wired from BlendMaterialAttributes"),
                BlendOut->A.Expression == (UMaterialExpression*)BlendNode);
        }
    }

    return true;
}
