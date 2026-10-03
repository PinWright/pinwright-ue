// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-combine-textures-docs-omit-placement-size-alpha-limits.
//
// texture.combine_textures is a full-frame, flat-index blend (TextureHandler.cpp,
// SubAction "combine_textures"): no placement parameter, a loop bounded by FMath::Min3
// of the three pixel counts with no row-stride reconciliation, RGB-only blending with
// the base's alpha copied through. All three limits fail silently with success:true, and
// the method page used to be a one-line summary plus seven parameter lines that read as a
// general compositor. This test fails if the page stops stating any of them.
//
// Rendered through WikiHandler::RenderPage, so it exercises both the texture.md overlay
// (### texture.combine_textures under ## See also) and the registered param descriptions.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCombineTexturesMethodDocTest,
    "PinWright.infra.wiki_handler.MethodPage.TextureCombineTexturesLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCombineTexturesMethodDocTest::RunTest(const FString& Parameters)
{
    FString Page;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("texture.combine_textures"), Page))
    {
        return false;
    }

    // (1) No placement: a full-frame blend from pixel 0, not a compositor.
    TestTrue(TEXT("page says it is a full-frame blend, not a compositor"),
        Page.Contains(TEXT("full-frame blend, not a compositor")));
    TestTrue(TEXT("page says there is no placement"),
        Page.Contains(TEXT("no placement")));

    // (2) Same dimensions required; what a mismatch does to the output.
    TestTrue(TEXT("page requires same-dimension inputs"),
        Page.Contains(TEXT("same dimensions")));
    TestTrue(TEXT("page names the diagonal smear on a width mismatch"),
        Page.Contains(TEXT("diagonal smear")));
    TestTrue(TEXT("page says the unblended tail is transparent black, not the base"),
        Page.Contains(TEXT("zero-filled (transparent black)")));

    // (3) Overlay alpha discarded; opacity is the only transparency control.
    TestTrue(TEXT("page says overlay alpha is ignored"),
        Page.Contains(TEXT("Overlay alpha is ignored")));
    TestTrue(TEXT("page says output alpha comes from the base"),
        Page.Contains(TEXT("copied from the base")));
    TestTrue(TEXT("page says opacity is the only transparency control"),
        Page.Contains(TEXT("only transparency control")));

    // (4) The output is created before input validation with no existing-asset guard, so an
    // output naming an input wipes it (B-combine-textures-output-overwrites-input).
    TestTrue(TEXT("page warns the output must not name an existing asset"),
        Page.Contains(TEXT("The output must not name an existing asset")));
    TestTrue(TEXT("page says an input named as the output is wiped before it is read"),
        Page.Contains(TEXT("that input is wiped before it is read")));
    TestTrue(TEXT("page says a refused call leaves an empty output asset"),
        Page.Contains(TEXT("refused by the format checks below still leaves an empty output")));
    TestTrue(TEXT("page warns another class at the output name crashes the editor"),
        Page.Contains(TEXT("of any other class at that name crashes the editor")));
    TestTrue(TEXT("name param warns against naming an existing asset"),
        Page.Contains(TEXT("must not name an existing asset - it is re-created in place")));

    // The blend has no colour-space conversion; the old "(sRGB-encoded) bytes" wording was wrong.
    TestFalse(TEXT("page no longer claims the bytes are sRGB-encoded"),
        Page.Contains(TEXT("sRGB-encoded")));
    TestTrue(TEXT("page says there is no colour-space conversion"),
        Page.Contains(TEXT("stored 8-bit bytes as-is, with no colour-space conversion")));

    // The silent blendMode fallback lives in the registered param description, so this
    // also proves the RPC_PARAM text (not just the overlay) reached the page.
    TestTrue(TEXT("blendMode param documents the fallback to Normal"),
        Page.Contains(TEXT("an unrecognised name falls back to Normal")));

    return true;
}
