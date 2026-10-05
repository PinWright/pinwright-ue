// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-texture-no-srgb-setter (GitHub #208).
//
// The texture namespace reported srgb on describe but had no verb to write it, so a mask or noise
// texture could only be made linear through a raw property.set SRGB. set_compression_settings to
// TC_Masks also left srgb:true, because it stored CompressionSettings raw and skipped the engine
// validation that turns sRGB off for linear compression (Texture.cpp:771-777). A material sampling
// such a texture as SAMPLERTYPE_Masks then refused to compile, two verbs later.
//
// Counterfactuals: without texture.set_srgb the handler lookup fails; with the old raw store in
// set_compression_settings the texture keeps SRGB=true after TC_Masks; and if set_srgb stored the
// flag raw instead of through the engine's edit path, srgb:true on a TC_Masks texture would land
// and be reported as a success instead of refused with DERIVED_PROPERTY.
//
// The same change makes set_compression_settings and set_texture_group (GitHub #365) resolve
// names against the engine enums. Both used to fall back silently (TC_Default / TEXTUREGROUP_World)
// for any name outside a short string table, and set_texture_group matched by substring.

#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"

#include "Dispatch/SafePoint.h"
#include "Tests/TestUtils.h"

namespace TextureSetSrgbTestHelpers
{
    static const TCHAR* const SrgbTestFolder = TEXT("/Game/PinWrightTests/Texture");

    FString MakeSrgbAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("T_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FString SrgbAssetPath(const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s"), SrgbTestFolder, *AssetName);
    }

    // An in-memory (save:false) noise texture, which create_noise_texture makes sRGB.
    UTexture2D* CreateSrgbFixture(FAutomationTestBase& Test, const FString& AssetName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), SrgbTestFolder);
        Payload->SetNumberField(TEXT("width"), 16);
        Payload->SetNumberField(TEXT("height"), 16);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("texture.create_noise_texture is registered"),
            InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"), Payload, Capture));
        if (!Test.TestTrue(TEXT("texture.create_noise_texture succeeded"), Capture.bSuccess))
        {
            return nullptr;
        }
        return Cast<UTexture2D>(StaticFindObject(UTexture2D::StaticClass(), nullptr,
            *ToObjectPath(SrgbAssetPath(AssetName))));
    }

    bool InvokeSetter(const TCHAR* Method, const FString& AssetName, TFunctionRef<void(FJsonObject&)> Fill,
        FTestResponseCapture& OutCapture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SrgbAssetPath(AssetName));
        Payload->SetBoolField(TEXT("save"), false);
        Fill(*Payload);
        return InvokeHandlerWithCapture(Method, Payload, OutCapture);
    }

    bool EchoedSrgb(const FTestResponseCapture& Capture, bool bDefault)
    {
        bool bValue = bDefault;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("srgb"), bValue);
        }
        return bValue;
    }

    FString DerivedWriteField(const FTestResponseCapture& Capture, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        FString Value;
        if (Capture.Result.IsValid() && Capture.Result->TryGetObjectField(TEXT("derivedWrite"), Block) && Block)
        {
            (*Block)->TryGetStringField(Field, Value);
        }
        return Value;
    }

    // save:false wrote nothing to disk, so clearing the dirty flag is the whole teardown.
    void DeDirtySrgbFixture(UTexture2D* Texture)
    {
        if (Texture)
        {
            Texture->GetOutermost()->SetDirtyFlag(false);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetSrgbWritesBothWaysTest,
    "PinWright.texture.set_srgb.WritesTheFlagBothWays",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetSrgbWritesBothWaysTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("SetSrgb"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }
    TestTrue(TEXT("precondition: a fresh noise texture is sRGB"), Texture->SRGB != 0);

    FTestResponseCapture OffCapture;
    TestTrue(TEXT("texture.set_srgb is registered"), InvokeSetter(TEXT("texture.set_srgb"), AssetName,
        [](FJsonObject& P) { P.SetBoolField(TEXT("srgb"), false); }, OffCapture));
    TestTrue(TEXT("srgb:false succeeded"), OffCapture.bSuccess);
    TestFalse(TEXT("the texture is now linear"), Texture->SRGB != 0);
    TestFalse(TEXT("the response reports the stored srgb:false"), EchoedSrgb(OffCapture, true));

    FTestResponseCapture OnCapture;
    InvokeSetter(TEXT("texture.set_srgb"), AssetName,
        [](FJsonObject& P) { P.SetBoolField(TEXT("srgb"), true); }, OnCapture);
    TestTrue(TEXT("srgb:true succeeded on TC_Default compression"), OnCapture.bSuccess);
    TestTrue(TEXT("the texture is sRGB again"), Texture->SRGB != 0);
    TestTrue(TEXT("the response reports the stored srgb:true"), EchoedSrgb(OnCapture, false));

    DeDirtySrgbFixture(Texture);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetCompressionMasksClearsSrgbTest,
    "PinWright.texture.set_compression_settings.LinearCompressionClearsSrgb",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetCompressionMasksClearsSrgbTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("MasksSrgb"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }
    TestTrue(TEXT("precondition: a fresh noise texture is sRGB"), Texture->SRGB != 0);

    FTestResponseCapture Capture;
    InvokeSetter(TEXT("texture.set_compression_settings"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("compressionSettings"), TEXT("TC_Masks")); }, Capture);
    TestTrue(TEXT("set_compression_settings TC_Masks succeeded"), Capture.bSuccess);
    TestEqual(TEXT("compression is TC_Masks"),
        static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_Masks));
    TestFalse(TEXT("TC_Masks turned sRGB off, as the texture editor does"), Texture->SRGB != 0);
    TestFalse(TEXT("the response reports srgb:false"), EchoedSrgb(Capture, true));

    DeDirtySrgbFixture(Texture);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetSrgbRefusedUnderLinearCompressionTest,
    "PinWright.texture.set_srgb.RefusedWhileCompressionForcesLinear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetSrgbRefusedUnderLinearCompressionTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("SrgbRefused"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }

    // Set the compression directly, so this test does not depend on set_compression_settings.
    Texture->PreEditChange(nullptr);
    Texture->CompressionSettings = TC_Masks;
    Texture->PostEditChange();
    TestFalse(TEXT("precondition: TC_Masks keeps the texture linear"), Texture->SRGB != 0);

    Texture->GetOutermost()->SetDirtyFlag(false);

    FTestResponseCapture Capture;
    InvokeSetter(TEXT("texture.set_srgb"), AssetName,
        [](FJsonObject& P) { P.SetBoolField(TEXT("srgb"), true); }, Capture);
    TestTrue(TEXT("texture.set_srgb responded"), Capture.bWasCalled);
    TestFalse(TEXT("the refused call left the package clean"), Texture->GetOutermost()->IsDirty());
    TestFalse(TEXT("srgb:true under TC_Masks is not reported as a success"), Capture.bSuccess);
    TestEqual(TEXT("refused with DERIVED_PROPERTY"), Capture.ErrorCode, FString(TEXT("DERIVED_PROPERTY")));
    TestEqual(TEXT("the refusal names set_compression_settings as the remedy"),
        DerivedWriteField(Capture, TEXT("authoritativeVerb")), FString(TEXT("texture.set_compression_settings")));
    TestTrue(TEXT("the refusal names the compression that forces linear"),
        DerivedWriteField(Capture, TEXT("derivedFrom")).Contains(TEXT("TC_Masks")));
    TestFalse(TEXT("the texture is still linear"), Texture->SRGB != 0);

    DeDirtySrgbFixture(Texture);
    return true;
}

// Both a LUT group and a linear compression keep sRGB off; the refusal names both, so fixing one
// does not lead straight into a second refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetSrgbRefusalNamesEverySettingTest,
    "PinWright.texture.set_srgb.RefusalNamesEveryForcingSetting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetSrgbRefusalNamesEverySettingTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("SrgbLutMasks"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }
    Texture->PreEditChange(nullptr);
    Texture->LODGroup = TEXTUREGROUP_ColorLookupTable;
    Texture->CompressionSettings = TC_Masks;
    Texture->PostEditChange();
    TestFalse(TEXT("precondition: LUT group + TC_Masks keep the texture linear"), Texture->SRGB != 0);

    FTestResponseCapture Capture;
    InvokeSetter(TEXT("texture.set_srgb"), AssetName,
        [](FJsonObject& P) { P.SetBoolField(TEXT("srgb"), true); }, Capture);
    TestEqual(TEXT("refused with DERIVED_PROPERTY"), Capture.ErrorCode, FString(TEXT("DERIVED_PROPERTY")));
    const FString DerivedFrom = DerivedWriteField(Capture, TEXT("derivedFrom"));
    TestTrue(TEXT("the refusal names the LUT group"), DerivedFrom.Contains(TEXT("ColorLookupTable")));
    TestTrue(TEXT("the refusal names the compression"), DerivedFrom.Contains(TEXT("TC_Masks")));
    TestTrue(TEXT("the explanation names both remedies"),
        Capture.Message.Contains(TEXT("texture.set_texture_group"))
        && Capture.Message.Contains(TEXT("texture.set_compression_settings")));

    DeDirtySrgbFixture(Texture);
    return true;
}

// An unknown compression name is refused instead of stored as TC_Default, and an engine entry
// the old string table lacked lands exactly, matched in any case.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetCompressionUnknownNameRefusedTest,
    "PinWright.texture.set_compression_settings.UnknownNameIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetCompressionUnknownNameRefusedTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("CompressionName"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }
    Texture->PreEditChange(nullptr);
    Texture->CompressionSettings = TC_BC7;
    Texture->PostEditChange();
    TestEqual(TEXT("precondition: compression is TC_BC7"),
        static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_BC7));

    FTestResponseCapture BogusCapture;
    InvokeSetter(TEXT("texture.set_compression_settings"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("compressionSettings"), TEXT("TC_Bogus")); }, BogusCapture);
    TestFalse(TEXT("TC_Bogus is not reported as a success"), BogusCapture.bSuccess);
    TestEqual(TEXT("TC_Bogus is refused with INVALID_ARGUMENT"), BogusCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the refusal lists the valid names"), BogusCapture.Message.Contains(TEXT("TC_HalfFloat")));
    TestEqual(TEXT("the refused call left the compression unchanged"),
        static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_BC7));

    // UMETA(Hidden) and engine-internal: the texture editor does not offer it, so neither does the verb.
    FTestResponseCapture HiddenCapture;
    InvokeSetter(TEXT("texture.set_compression_settings"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("compressionSettings"), TEXT("TC_EncodedReflectionCapture")); }, HiddenCapture);
    TestEqual(TEXT("TC_EncodedReflectionCapture is refused with INVALID_ARGUMENT"),
        HiddenCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestFalse(TEXT("the valid list omits the hidden entry"), HiddenCapture.Message.Contains(TEXT("TC_EncodedReflectionCapture, ")));
    TestEqual(TEXT("the refused hidden name left the compression unchanged"),
        static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_BC7));

    FTestResponseCapture HalfFloatCapture;
    InvokeSetter(TEXT("texture.set_compression_settings"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("compressionSettings"), TEXT("tc_halffloat")); }, HalfFloatCapture);
    TestTrue(TEXT("tc_halffloat succeeded"), HalfFloatCapture.bSuccess);
    TestEqual(TEXT("tc_halffloat stored TC_HalfFloat"),
        static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_HalfFloat));

    DeDirtySrgbFixture(Texture);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetTextureGroupUnknownNameRefusedTest,
    "PinWright.texture.set_texture_group.UnknownNameIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetTextureGroupUnknownNameRefusedTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("GroupName"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }
    Texture->PreEditChange(nullptr);
    Texture->LODGroup = TEXTUREGROUP_UI;
    Texture->PostEditChange();
    TestEqual(TEXT("precondition: group is TEXTUREGROUP_UI"),
        static_cast<int32>(Texture->LODGroup), static_cast<int32>(TEXTUREGROUP_UI));

    FTestResponseCapture Capture;
    InvokeSetter(TEXT("texture.set_texture_group"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("textureGroup"), TEXT("TEXTUREGROUP_Wrold")); }, Capture);
    TestFalse(TEXT("a misspelled group is not reported as a success"), Capture.bSuccess);
    TestEqual(TEXT("refused with INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the refusal lists the valid names"), Capture.Message.Contains(TEXT("TEXTUREGROUP_ColorLookupTable")));
    TestEqual(TEXT("the refused call left the group unchanged"),
        static_cast<int32>(Texture->LODGroup), static_cast<int32>(TEXTUREGROUP_UI));

    DeDirtySrgbFixture(Texture);
    return true;
}

// An exact name lands exactly (no first-substring match), and the engine applies the group's own
// settings: 16BitData sets TC_HDR and turns sRGB off (UE 5.8 Texture.cpp:878-883).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureSetTextureGroupExactNameTest,
    "PinWright.texture.set_texture_group.ExactNameAndEngineSideEffects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureSetTextureGroupExactNameTest::RunTest(const FString& Parameters)
{
    using namespace TextureSetSrgbTestHelpers;

    const FString AssetName = MakeSrgbAssetName(TEXT("GroupExact"));
    UTexture2D* Texture = CreateSrgbFixture(*this, AssetName);
    if (!TestNotNull(TEXT("the fixture texture exists"), Texture))
    {
        return false;
    }
    TestTrue(TEXT("precondition: a fresh noise texture is sRGB"), Texture->SRGB != 0);

    FTestResponseCapture NormalCapture;
    InvokeSetter(TEXT("texture.set_texture_group"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("textureGroup"), TEXT("TEXTUREGROUP_CharacterNormalMap")); }, NormalCapture);
    TestTrue(TEXT("TEXTUREGROUP_CharacterNormalMap succeeded"), NormalCapture.bSuccess);
    TestEqual(TEXT("TEXTUREGROUP_CharacterNormalMap landed exactly, not as Character"),
        static_cast<int32>(Texture->LODGroup), static_cast<int32>(TEXTUREGROUP_CharacterNormalMap));

    FTestResponseCapture DataCapture;
    InvokeSetter(TEXT("texture.set_texture_group"), AssetName,
        [](FJsonObject& P) { P.SetStringField(TEXT("textureGroup"), TEXT("16BitData")); }, DataCapture);
    TestTrue(TEXT("16BitData (prefix omitted) succeeded"), DataCapture.bSuccess);
    TestEqual(TEXT("the group is TEXTUREGROUP_16BitData"),
        static_cast<int32>(Texture->LODGroup), static_cast<int32>(TEXTUREGROUP_16BitData));
    TestEqual(TEXT("the engine set TC_HDR for the 16-bit data group"),
        static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_HDR));
    TestFalse(TEXT("the engine turned sRGB off for the 16-bit data group"), Texture->SRGB != 0);
    TestFalse(TEXT("the response reports the stored srgb:false"), EchoedSrgb(DataCapture, true));

    DeDirtySrgbFixture(Texture);
    return true;
}

// The three verbs write through the texture edit path, whose material notification recreates render
// state and flushes rendering commands (safe-point family I), so the dispatcher must park them
// outside the world tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureEditPathVerbsAreTickUnsafeTest,
    "PinWright.texture.set_srgb.EditPathVerbsAreTickUnsafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureEditPathVerbsAreTickUnsafeTest::RunTest(const FString& Parameters)
{
    for (const TCHAR* Method : { TEXT("texture.set_srgb"), TEXT("texture.set_compression_settings"),
             TEXT("texture.set_texture_group") })
    {
        TestTrue(*FString::Printf(TEXT("%s is tick-unsafe"), Method), PinWrightSafePoint::IsTickUnsafeMethod(Method));
    }
    return true;
}
