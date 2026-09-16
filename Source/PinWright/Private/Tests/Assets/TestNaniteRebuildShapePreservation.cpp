// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral coverage for asset.nanite_rebuild_mesh's shape-preservation contract.
//
// The verb used to expose a single `preserveArea` boolean over UE 5.7+'s three-valued
// ENaniteShapePreservation (EngineTypes.h): Voxelize was unreachable, and because the boolean
// defaulted to true every call - including one made only to toggle Nanite - rewrote the mesh off
// the engine's None default and forced PositionPrecision to 8. The response echoed the parsed
// request, so none of that was observable from the wire.
//
// Every assertion below reads FMeshNaniteSettings back off the asset rather than off the response:
// a test written against the response alone passes against the echo.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"

#include "Compat/EngineVersionCompat.h"
#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNaniteRebuildShapePreservationTest,
    "PinWright.asset.nanite_rebuild_mesh.ShapePreservationSelectsEngineMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNaniteRebuildShapePreservationTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_NaniteShapePreservation_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);

    TestNotNull(TEXT("Temporary StaticMesh created"), Mesh);
    if (!Mesh)
    {
        return false;
    }

    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Package->SetDirtyFlag(false);
        Mesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    // The verb ends every successful write with NotifyNaniteSettingsChanged(), whose
    // PostEditChangeProperty reaches UStaticMesh::CanBuild(). A fixture mesh carries no source
    // models, so CanBuild logs "Static mesh has no source models" and returns false before any
    // build work happens -- and the automation framework elevates a captured log warning to a test
    // error by default (UAutomationControllerSettings::bElevateLogWarningsToErrors). Negative
    // occurrence = optional: how many times it logs is an engine detail, and the settings
    // assertions below do the real verification.
    AddExpectedMessagePlain(TEXT("has no source models"),
        ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

    // The declaration, not just the body: InvokeHandlerWithCapture skips the dispatcher's
    // unknown-param gate, so a wire name the body reads but RPC_PARAMS omits would work here and
    // be refused UNKNOWN_PARAMS for every real caller.
    TestTrue(TEXT("shapePreservation is a declared parameter"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("asset.nanite_rebuild_mesh"), TEXT("shapePreservation")));
    TestTrue(TEXT("positionPrecision is a declared parameter"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("asset.nanite_rebuild_mesh"), TEXT("positionPrecision")));
    TestTrue(TEXT("the deprecated preserveArea alias is still declared"),
        ParamSpecTestHelpers::IsParamAccepted(TEXT("asset.nanite_rebuild_mesh"), TEXT("preserveArea")));

    auto Rebuild = [Mesh](const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture) -> bool
    {
        Payload->SetStringField(TEXT("meshPath"), Mesh->GetPathName());
        return InvokeHandlerWithCapture(TEXT("asset.nanite_rebuild_mesh"), Payload, Capture);
    };

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    struct FShapeCase
    {
        const TCHAR* WireValue;
        ENaniteShapePreservation Expected;
    };
    const FShapeCase Cases[] = {
        { TEXT("voxelize"),      ENaniteShapePreservation::Voxelize },
        { TEXT("preserve_area"), ENaniteShapePreservation::PreserveArea },
        { TEXT("none"),          ENaniteShapePreservation::None },
    };

    for (const FShapeCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("shapePreservation"), Case.WireValue);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestTrue(*FString::Printf(TEXT("shapePreservation=%s succeeds"), Case.WireValue),
            Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        // Ground truth: the setting on the asset, not the number that went in.
        TestEqual(*FString::Printf(TEXT("shapePreservation=%s writes the matching ENaniteShapePreservation"), Case.WireValue),
            static_cast<int32>(Mesh->GetNaniteSettings().ShapePreservation),
            static_cast<int32>(Case.Expected));

        FString ReportedShape;
        TestTrue(TEXT("response carries shapePreservation"),
            Capture.Result->TryGetStringField(TEXT("shapePreservation"), ReportedShape));
        TestEqual(*FString::Printf(TEXT("response reads shapePreservation=%s back off the asset"), Case.WireValue),
            ReportedShape, FString(Case.WireValue));
    }

    // Omitting the setting must leave the mesh's stored technique and precision alone. This is the
    // half a response-only assertion cannot see: the old body forced PreserveArea and
    // PositionPrecision=8 on every call and reported the request either way.
    {
        FMeshNaniteSettings Seeded = Mesh->GetNaniteSettings();
        Seeded.ShapePreservation = ENaniteShapePreservation::Voxelize;
        Seeded.PositionPrecision = 3;
        Mesh->SetNaniteSettings(Seeded);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("trianglePercent"), 50.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestTrue(TEXT("a triangle-percentage-only call succeeds"), Capture.bSuccess);
        TestEqual(TEXT("omitting shapePreservation leaves the stored technique untouched"),
            static_cast<int32>(Mesh->GetNaniteSettings().ShapePreservation),
            static_cast<int32>(ENaniteShapePreservation::Voxelize));
        TestEqual(TEXT("omitting positionPrecision leaves the stored precision untouched"),
            Mesh->GetNaniteSettings().PositionPrecision, 3);
        TestEqual(TEXT("trianglePercent still writes KeepPercentTriangles"),
            Mesh->GetNaniteSettings().KeepPercentTriangles, 0.5f, KINDA_SMALL_NUMBER);
    }

    // positionPrecision is now a parameter rather than a hardcoded 8.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("positionPrecision"), 11.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestTrue(TEXT("positionPrecision call succeeds"), Capture.bSuccess);
        TestEqual(TEXT("positionPrecision writes the requested precision"),
            Mesh->GetNaniteSettings().PositionPrecision, 11);
    }

    // The deprecated boolean keeps its two-state meaning for existing callers.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("preserveArea"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestTrue(TEXT("preserveArea=false succeeds"), Capture.bSuccess);
        TestEqual(TEXT("preserveArea=false still maps to None"),
            static_cast<int32>(Mesh->GetNaniteSettings().ShapePreservation),
            static_cast<int32>(ENaniteShapePreservation::None));

        Payload->SetBoolField(TEXT("preserveArea"), true);
        FTestResponseCapture TrueCapture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, TrueCapture));
        TestTrue(TEXT("preserveArea=true succeeds"), TrueCapture.bSuccess);
        TestEqual(TEXT("preserveArea=true still maps to PreserveArea"),
            static_cast<int32>(Mesh->GetNaniteSettings().ShapePreservation),
            static_cast<int32>(ENaniteShapePreservation::PreserveArea));
    }

    // An unresolvable value must be refused by name and must not write anything.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("shapePreservation"), TEXT("preserve_volume"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestFalse(TEXT("an unknown shapePreservation is rejected"), Capture.bSuccess);
        TestEqual(TEXT("an unknown shapePreservation returns INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestEqual(TEXT("a rejected shapePreservation leaves the mesh unchanged"),
            static_cast<int32>(Mesh->GetNaniteSettings().ShapePreservation),
            static_cast<int32>(ENaniteShapePreservation::PreserveArea));
    }
#else
    // 5.3-5.6 carry only FMeshNaniteSettings::bPreserveArea, so the two expressible states must
    // round-trip and voxelize must be refused rather than silently downgraded.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("shapePreservation"), TEXT("preserve_area"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestTrue(TEXT("shapePreservation=preserve_area succeeds"), Capture.bSuccess);
        TestTrue(TEXT("shapePreservation=preserve_area sets bPreserveArea"),
            Mesh->NaniteSettings.bPreserveArea != 0);

        Payload->SetStringField(TEXT("shapePreservation"), TEXT("none"));
        FTestResponseCapture NoneCapture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, NoneCapture));
        TestTrue(TEXT("shapePreservation=none succeeds"), NoneCapture.bSuccess);
        TestFalse(TEXT("shapePreservation=none clears bPreserveArea"),
            Mesh->NaniteSettings.bPreserveArea != 0);
    }

    {
        Mesh->NaniteSettings.PositionPrecision = 3;

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("shapePreservation"), TEXT("voxelize"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.nanite_rebuild_mesh is registered"), Rebuild(Payload, Capture));
        TestFalse(TEXT("voxelize is refused below UE 5.7"), Capture.bSuccess);
        TestEqual(TEXT("a refused voxelize leaves positionPrecision untouched"),
            Mesh->NaniteSettings.PositionPrecision, 3);
    }
#endif

    return true;
}
