// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for IsTextureSourceNoisyProperty in PropertyUtils.cpp.
//
// FTextureSource default-constructs to an all-zero-GUID placeholder that the
// generic FStructProperty ExportText fallback re-emits as
// "(Id=000...,NumLayers=1,BlockDataOffsets=(0))" — no signal, all noise.
// BuildClassPropertyJson now skips the `Source` FProperty on owner class
// UTexture::StaticClass(), so every UTexture descendant (UTexture2D,
// UMediaTexture, UBinkMediaTexture) drops the field from properties.json.
//
// Counterfactual: reverting IsTextureSourceNoisyProperty in PropertyUtils.cpp
// restores the placeholder via the generic FStructProperty fallback at
// PropertyUtils.cpp:460-465.
#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"

#include "Engine/Texture2D.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsTextureSourceSkipTest,
    "PinWright.utils.property_utils.TextureSourceSkip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsTextureSourceSkipTest::RunTest(const FString& Parameters)
{
    // UTexture2D inherits the FTextureSource Source UPROPERTY from UTexture, exactly
    // the same way UMediaTexture and UBinkMediaTexture do. ParentCDO=nullptr forces
    // every property to be treated as overridden, so the only reason `Source` can be
    // absent from the output is the IsTextureSourceNoisyProperty skip.
    UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage());
    TestNotNull(TEXT("Constructed transient UTexture2D"), Tex);
    if (!Tex) return false;

    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(Tex, nullptr);
    TestTrue(TEXT("BuildClassPropertyJson returned a valid object"), Result.IsValid());
    if (!Result.IsValid()) return false;

    TestFalse(TEXT("'Source' property is skipped on UTexture descendants"),
              Result->HasField(TEXT("Source")));

    // Positive control: the skip must be TARGETED, not a blanket drop of UTexture-owned
    // properties. The assertion above is purely negative, so removing the property-name
    // guard at PropertyExport.cpp:1183 — leaving only the owner-class test — would make
    // IsTextureSourceNoisyProperty true for EVERY property declared on UTexture and
    // silently erase LODGroup, CompressionSettings, SRGB and the rest from every
    // texture's properties.json, with this test still green.
    //
    // All three are plain UPROPERTYs on UTexture itself (Engine/Classes/Engine/Texture.h
    // :1512, :1544, :1573) with no transient/skip-serialization flag, and
    // ShouldSkipNonSemanticDumpProperty (PropertyExport.cpp:1187) has no UTexture rule,
    // so with ParentCDO=nullptr they must all be emitted.
    TestTrue(TEXT("Sibling UTexture property 'LODGroup' survives the targeted skip"),
             Result->HasField(TEXT("LODGroup")));
    TestTrue(TEXT("Sibling UTexture property 'CompressionSettings' survives the targeted skip"),
             Result->HasField(TEXT("CompressionSettings")));
    TestTrue(TEXT("Sibling UTexture property 'SRGB' survives the targeted skip"),
             Result->HasField(TEXT("SRGB")));

    return true;
}
