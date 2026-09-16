// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Handlers/Asset/TextureDumpBuilder.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#include "Misc/Guid.h"
#include "PixelFormat.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
    class FTextureDescribeAsyncCompilationProbe : public IInterface_AsyncCompilation
    {
    public:
#if WITH_EDITOR
        virtual bool IsCompiling() const override
        {
            return bIsCompiling;
        }
#endif

        bool bIsCompiling = true;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureStableReadCompileGuardTest,
    "PinWright.texture.describe.CompileGuardHelper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureStableReadCompileGuardTest::RunTest(const FString& Parameters)
{
    FTextureDescribeAsyncCompilationProbe Probe;
    TestTrue(TEXT("Compiling texture is rejected"),
        TextureDumpBuilder::IsAsyncCompilationInProgress(&Probe));
    Probe.bIsCompiling = false;
    TestFalse(TEXT("Completed texture is accepted"),
        TextureDumpBuilder::IsAsyncCompilationInProgress(&Probe));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureDescribeGenericTextureCoversNonTexture2DTest,
    "PinWright.texture.describe.CoversNonTexture2D",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: if texture.describe is missing or uses the legacy UTexture2D load path,
// this render-target cube request either has no handler or returns an error before fields are emitted.
bool FTextureDescribeGenericTextureCoversNonTexture2DTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString AssetName = FString::Printf(TEXT("RT_TextureDescribeCube_%s"), *Suffix);
    const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/%s"), *AssetName);
    // RF_Standalone survives the periodic suite GC; detach so a later /Engine/Transient
    // asset-registry rescan cannot see the fixture.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }
    Package->SetFlags(RF_Transient);

    UTextureRenderTargetCube* RenderTarget = NewObject<UTextureRenderTargetCube>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("TextureRenderTargetCube created"), RenderTarget))
    {
        return true;
    }
    RenderTarget->Init(64, PF_FloatRGBA);

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("texture.describe handler found"),
        InvokeHandlerWithCapture(TEXT("texture.describe"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("response reports success"), Capture.bSuccess);
    if (!TestTrue(TEXT("result object present"), Capture.Result.IsValid()))
    {
        return true;
    }

    FString Kind;
    TestTrue(TEXT("kind present"), Capture.Result->TryGetStringField(TEXT("kind"), Kind));
    TestEqual(TEXT("kind equals class name"), Kind, RenderTarget->GetClass()->GetName());

    const TSharedPtr<FJsonObject>* SizePtr = nullptr;
    if (TestTrue(TEXT("size field present"), Capture.Result->TryGetObjectField(TEXT("size"), SizePtr)))
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        TestTrue(TEXT("size.x present"), (*SizePtr)->TryGetNumberField(TEXT("x"), X));
        TestTrue(TEXT("size.y present"), (*SizePtr)->TryGetNumberField(TEXT("y"), Y));
        TestTrue(TEXT("size.z present"), (*SizePtr)->TryGetNumberField(TEXT("z"), Z));
        TestEqual(TEXT("size.x matches GetSurfaceWidth()"),
            static_cast<int32>(X), static_cast<int32>(RenderTarget->GetSurfaceWidth()));
        TestEqual(TEXT("size.y matches GetSurfaceHeight()"),
            static_cast<int32>(Y), static_cast<int32>(RenderTarget->GetSurfaceHeight()));
        TestEqual(TEXT("size.z matches GetSurfaceDepth()"),
            static_cast<int32>(Z), static_cast<int32>(RenderTarget->GetSurfaceDepth()));
    }

    double ArraySize = 0.0;
    TestTrue(TEXT("arraySize present"), Capture.Result->TryGetNumberField(TEXT("arraySize"), ArraySize));
    TestEqual(TEXT("arraySize matches GetSurfaceArraySize()"),
        static_cast<int32>(ArraySize), static_cast<int32>(RenderTarget->GetSurfaceArraySize()));

    FString PixelFormat;
    TestTrue(TEXT("pixelFormat present"), Capture.Result->TryGetStringField(TEXT("pixelFormat"), PixelFormat));
    TestEqual(TEXT("pixelFormat matches GetFormat()"),
        PixelFormat, FString(GetPixelFormatString(RenderTarget->GetFormat())));

    const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
    if (TestTrue(TEXT("source field present"), Capture.Result->TryGetObjectField(TEXT("source"), SourcePtr)))
    {
        FString SourceKind;
        FString SourceFormat;
        TestTrue(TEXT("source.sourceKind present"), (*SourcePtr)->TryGetStringField(TEXT("sourceKind"), SourceKind));
        TestEqual(TEXT("source.sourceKind equals renderTarget"), SourceKind, FString(TEXT("renderTarget")));
        TestTrue(TEXT("source.format present"), (*SourcePtr)->TryGetStringField(TEXT("format"), SourceFormat));
        TestTrue(TEXT("source.format is TSF_*"), SourceFormat.StartsWith(TEXT("TSF_")));
    }

    return true;
}
