// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the LODBias / wrap / virtual-texture / filter readback gap
// in TextureDumpBuilder::BuildTextureJson (ticket E-texture-describe-omits-lodbias-wrap).
//
// texture.describe and the texture.json dump sidecar both delegate to the exported
// BuildTextureJson, but the shared builder used to omit four fields that sibling
// texture setters write: LODBias (set_lod_bias), AddressX/AddressY (set_texture_wrap),
// VirtualTextureStreaming (configure_virtual_texture), and Filter (set_texture_filter).
// That forced a write-then-confirm round-trip onto the legacy get_texture_info reader
// (which lacks filter/wrap entirely and hard-casts to UTexture2D).
//
// This test constructs a transient UTexture2D, sets all four setter-written fields to
// non-default values, then asserts BuildTextureJson emits each with the matching value.
//
// Counterfactual: reverting the BuildTextureJson change drops lodBias / addressX /
// addressY / virtualTextureStreaming / filter from the JSON, so every TestTrue below fails.
#include "Misc/AutomationTest.h"

#include "Handlers/Asset/TextureDumpBuilder.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureDescribeEmitsSamplingFieldsTest,
    "PinWright.texture.describe.EmitsSamplingFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureDescribeEmitsSamplingFieldsTest::RunTest(const FString& Parameters)
{
    UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage());
    if (!TestNotNull(TEXT("Constructed transient UTexture2D"), Tex))
    {
        return true;
    }

    // Non-default values for every setter-written field, distinct from the engine
    // defaults (LODBias=0, Address*=TA_Wrap, VirtualTextureStreaming=false, Filter=TF_Default)
    // so each assertion proves the field was actually emitted from the live object.
    Tex->LODBias = 2;
    Tex->AddressX = TA_Clamp;
    Tex->AddressY = TA_Mirror;
    Tex->VirtualTextureStreaming = 1;
    Tex->Filter = TF_Nearest;

    TSharedPtr<FJsonObject> Json = TextureDumpBuilder::BuildTextureJson(Tex);
    if (!TestTrue(TEXT("BuildTextureJson returned a valid object"), Json.IsValid()))
    {
        return true;
    }

    double LodBias = 0.0;
    TestTrue(TEXT("lodBias field present"), Json->TryGetNumberField(TEXT("lodBias"), LodBias));
    TestEqual(TEXT("lodBias matches LODBias"), static_cast<int32>(LodBias), 2);

    FString AddressX;
    TestTrue(TEXT("addressX field present"), Json->TryGetStringField(TEXT("addressX"), AddressX));
    TestEqual(TEXT("addressX matches AddressX (TA_Clamp)"), AddressX, FString(TEXT("TA_Clamp")));

    FString AddressY;
    TestTrue(TEXT("addressY field present"), Json->TryGetStringField(TEXT("addressY"), AddressY));
    TestEqual(TEXT("addressY matches AddressY (TA_Mirror)"), AddressY, FString(TEXT("TA_Mirror")));

    bool bVirtualTextureStreaming = false;
    TestTrue(TEXT("virtualTextureStreaming field present"),
        Json->TryGetBoolField(TEXT("virtualTextureStreaming"), bVirtualTextureStreaming));
    TestTrue(TEXT("virtualTextureStreaming is true"), bVirtualTextureStreaming);

    FString Filter;
    TestTrue(TEXT("filter field present"), Json->TryGetStringField(TEXT("filter"), Filter));
    TestEqual(TEXT("filter matches Filter (TF_Nearest)"), Filter, FString(TEXT("TF_Nearest")));

    return true;
}
