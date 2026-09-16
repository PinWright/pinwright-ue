// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the `.pwmodel` half of the set_vertex_color channel mask
// (F-set-vertex-color-no-channel-mask).
//
// The RPC and the model DSL are two front-ends over one op, and the failure this file exists to
// catch is them DISAGREEING rather than either being broken on its own. Two ways that happened
// and are now pinned:
//
//  1. The compiler dropped `channels=` entirely. Nothing in a compile response reflects a vertex
//     colour, so a mask that never reached GeometryOps::FSetVertexColorParams would compile clean,
//     report the same counts, and silently overwrite the per-part tint the generators wrote. Only
//     reading the BAKED asset's colours catches it, which is what the first test does.
//  2. The compiler gated the parse on `!ChannelSpec.IsEmpty()`, so `channels=""` fell through to
//     "all four" here while the RPC refused it — the same document meaning two different things
//     depending on which surface ran it. The second test pins the refusal.
#include "Misc/AutomationTest.h"

#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.

const TCHAR* const PwColorChan_OutputRoot = TEXT("/Game/PinWrightTests/PwModelVertexColorChannels");

FPwModelCompileResult PwColorChan_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FString PwColorChan_Describe(const FPwModelCompileResult& Result)
{
    TArray<FString> Diagnostics;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        Diagnostics.Add(Diagnostic.ToString());
    }
    return FString::Printf(TEXT("diagnostics [%s]"), *FString::Join(Diagnostics, TEXT(" | ")));
}

bool PwColorChan_AnyDiagnosticContains(const FPwModelCompileResult& Result, const TCHAR* Needle)
{
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        if (Diagnostic.Message.Contains(Needle))
        {
            return true;
        }
    }
    return false;
}
}

// ============================================================================
// The mask reaches the compiler, and the tint it did not name survives
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelVertexColorChannelsMaskTest,
    "PinWright.Model.VertexColorChannels.MaskReachesTheCompilerAndPreservesTheTint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelVertexColorChannelsMaskTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("%s/SM_PwColorChanMask_%s"),
        PwColorChan_OutputRoot, *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // The generator writes a strictly increasing R < G < B tint; the masked write that follows
    // carries a FLAT 0.9 RGB alongside the alpha it means to set. If the compiler drops the mask,
    // that 0.9 lands on all three and the ordering is destroyed - which is exactly what a
    // per-part albedo ladder losing its ladder looks like. The ordering is the assertion rather
    // than the values because every colour-space conversion the StaticMesh bake can apply is
    // monotonic, so R < G < B survives whatever encoding lands.
    FPwModelCompileOptions Options;
    Options.OutputAssetPath = AssetPath;
    Options.SourcePath = TEXT("Tests/PwModelVertexColorChannels.pwmodel");
    Options.bOverwrite = true;
    Options.bSave = true;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100) color=(0.25, 0.5, 0.75, 1)\n")
        TEXT("    set_vertex_color set_all=true color=(0.9, 0.9, 0.9, 0.25) channels=\"a\"\n")
        TEXT("}\n"),
        Options);

    if (!TestTrue(*FString::Printf(TEXT("a document carrying channels= compiles. %s"),
            *PwColorChan_Describe(Result)), Result.bSuccess))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!TestNotNull(TEXT("the compile produced a UStaticMesh to read colours from"), Mesh))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const FMeshDescription* Baked = Mesh->GetMeshDescription(0);
    if (!TestNotNull(TEXT("the baked asset has a LOD0 mesh description"), Baked))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const FStaticMeshConstAttributes Attributes(*Baked);
    const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
    if (!TestTrue(TEXT("the bake carried a vertex colour attribute"),
            Colors.IsValid() && Colors.GetNumElements() > 0))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    int32 Checked = 0;
    for (const FVertexInstanceID InstanceID : Baked->VertexInstances().GetElementIDs())
    {
        const FVector4f Colour = Colors[InstanceID];
        TestTrue(*FString::Printf(
            TEXT("the alpha-masked write left the generator's R<G<B tint intact (got %.3f, %.3f, %.3f)"),
            Colour.X, Colour.Y, Colour.Z),
            Colour.X < Colour.Y && Colour.Y < Colour.Z);
        ++Checked;
        break;
    }
    TestTrue(TEXT("the baked mesh description has a vertex instance to read a colour from"), Checked > 0);

    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// Both front-ends refuse the same spellings
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelVertexColorChannelsRefusalTest,
    "PinWright.Model.VertexColorChannels.EmptyAndUnreadableSpecsAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelVertexColorChannelsRefusalTest::RunTest(const FString& Parameters)
{
    // A readable mask compiles. This is also what proves the parser DECLARES the parameter: an
    // undeclared one is rejected by the op table before any of the rest matters.
    const FPwModelCompileResult Accepted = PwColorChan_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100) color=(0.25, 0.5, 0.75, 1)\n")
        TEXT("    set_vertex_color set_all=true color=(1, 1, 1, 0.25) channels=\"a\"\n")
        TEXT("}\n"));
    TestTrue(*FString::Printf(TEXT("channels=\"a\" is accepted. %s"),
        *PwColorChan_Describe(Accepted)), Accepted.bSuccess);

    // An EMPTY mask must fail here exactly as it does on the RPC. Gating the parse on
    // IsEmpty() made this compile and write all four - the two surfaces disagreeing about what
    // one document means, which is worse than either refusing or accepting it consistently.
    const FPwModelCompileResult Empty = PwColorChan_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    set_vertex_color set_all=true color=(1, 1, 1, 0.25) channels=\"\"\n")
        TEXT("}\n"));
    TestFalse(*FString::Printf(TEXT("channels=\"\" is refused rather than read as all four. %s"),
        *PwColorChan_Describe(Empty)), Empty.bSuccess);

    // An unreadable mask reaches ParseColorChannels and is refused with the letters named. This
    // is the assertion that the compiler READS the value rather than merely tolerating the key.
    const FPwModelCompileResult Unreadable = PwColorChan_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    set_vertex_color set_all=true color=(1, 1, 1, 0.25) channels=\"x\"\n")
        TEXT("}\n"));
    TestFalse(*FString::Printf(TEXT("channels=\"x\" is refused. %s"),
        *PwColorChan_Describe(Unreadable)), Unreadable.bSuccess);
    TestTrue(*FString::Printf(TEXT("the refusal names the legal letters. %s"),
        *PwColorChan_Describe(Unreadable)),
        PwColorChan_AnyDiagnosticContains(Unreadable, TEXT("r, g, b, a")));

    return true;
}
