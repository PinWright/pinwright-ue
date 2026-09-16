// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Audio domain handlers (AudioAuthoringHandler.cpp and AudioHandler.cpp).
// Each test invokes a single handler via the auto-registration table and asserts no crash.
// MissingRequiredParam tests are the exception: they route an EMPTY payload through a real
// FRpcDispatcher (AudioDispatcherGate below) because the required-param gate lives in the
// dispatcher, not in any handler body, and InvokeHandler reaches neither call site.
// ValidParamsNoCrash tests supply realistic fake values to exercise the happy path as far
// as possible without an active UE editor session.
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/ActorUtils.h"

#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#if __has_include("Sound/DialogueVoice.h")
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
#define MCP_TEST_HAS_DIALOGUE 1
#else
#define MCP_TEST_HAS_DIALOGUE 0
#endif
#if __has_include("Sound/ReverbEffect.h")
#include "Sound/ReverbEffect.h"
#define MCP_TEST_HAS_REVERB 1
#else
#define MCP_TEST_HAS_REVERB 0
#endif
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Dispatch/RpcDispatcher.h"

namespace
{
    USoundCue* NewTransientAudioAuthoringSoundCue(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("SC_AudioDescribe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        USoundCue* Cue = NewObject<USoundCue>(
            Package,
            USoundCue::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Cue)
        {
            return nullptr;
        }

        USoundNodeMixer* MixerNode = Cue->ConstructSoundNode<USoundNodeMixer>();
        USoundNodeWavePlayer* PlayerNode = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
        USoundWave* SoundWave = NewObject<USoundWave>(GetTransientPackage(), USoundWave::StaticClass(), NAME_None, RF_Transient);
        PlayerNode->SetSoundWave(SoundWave);

        MixerNode->ChildNodes.Add(PlayerNode);
        Cue->FirstNode = MixerNode;

        Cue->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Cue;
    }

    // Single source of the transient-audio-asset convention shared by every audio-handler
    // fixture that needs a UObject resolvable by path (package + RF flags + AddToRoot +
    // AssetCreated registration). One change here (e.g. MarkPackageDirty, a different RF flag,
    // AddToRoot lifetime) applies to all wrappers below instead of being re-applied per copy.
    template <typename T>
    T* NewTransientAudioAsset(const TCHAR* NamePrefix, FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("%s%s"), NamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        T* Asset = NewObject<T>(
            Package,
            T::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Asset)
        {
            return nullptr;
        }
        Asset->AddToRoot();
        FAssetRegistryModule::AssetCreated(Asset);
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Asset;
    }

    // Sibling of NewTransientAudioAuthoringSoundCue for tests that need a resolvable
    // USoundBase by path but not a full SoundCue graph.
    USoundWave* NewTransientAudioSoundWave(FString& OutObjectPath)
    {
        return NewTransientAudioAsset<USoundWave>(TEXT("SW_AmbientSpawn_"), OutObjectPath);
    }

    // Sibling of the SoundCue/SoundWave fixtures for the describe_attenuation test.
    // Produces a USoundAttenuation resolvable by path so the configure_* and
    // describe_attenuation handlers can load it through LoadSoundAttenuationFromPath.
    USoundAttenuation* NewTransientAudioAttenuation(FString& OutObjectPath)
    {
        return NewTransientAudioAsset<USoundAttenuation>(TEXT("ATT_AudioDescribe_"), OutObjectPath);
    }
}

#if __has_include("MetasoundSource.h")
#include "Metasound.h"
#include "MetasoundSource.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"

namespace
{
    UMetaSoundPatch* NewTransientAudioAuthoringMetaSoundPatch(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("MS_AudioDescribe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UMetaSoundPatch* Patch = NewObject<UMetaSoundPatch>(
            Package,
            UMetaSoundPatch::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Patch)
        {
            return nullptr;
        }

        Patch->AddToRoot();

        // A freshly NewObject'd MetaSoundPatch has an empty FMetasoundFrontendDocument
        // with zero PagedGraphs. Production code (e.g. MetaSoundDumpBuilder::BuildMetaSoundJson)
        // walks the default page via FindConstGraphChecked() and asserts. Real assets always go
        // through the document builder during creation, so mirror that by seeding the default
        // page first with a non-priming builder that runs InitDocument().
        {
            TScriptInterface<IMetaSoundDocumentInterface> SeedInterface(Patch);
            FMetaSoundFrontendDocumentBuilder SeedBuilder(SeedInterface);
            SeedBuilder.InitDocument();
            PW_METASOUND_FINISH_BUILDING(SeedBuilder);
        }

        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Patch;
    }
}
#endif

// ============================================================================
// audio.authoring.create_sound_cue
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundCueValidParamsTest,
    "PinWright.audio.authoring.create_sound_cue.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundCueValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestCue"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Audio/Cues"));
    Payload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.authoring.create_sound_cue"), Payload));
    CleanupTestAsset(TEXT("/Game/Audio/Cues/TestCue"));
    return true;
}

// ============================================================================
// audio.authoring.connect_cue_nodes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringConnectCueNodesPartialParamsTest,
    "PinWright.audio.authoring.connect_cue_nodes.MissingTargetNodeId",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringConnectCueNodesPartialParamsTest::RunTest(const FString& Parameters)
{
    // Provide assetPath and sourceNodeId but omit targetNodeId.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Audio/Cues/TestCue"));
    Payload->SetStringField(TEXT("sourceNodeId"), TEXT("NodeA"));
    // Asserted, not merely invoked. The registration lookup alone is satisfied by a
    // handler whose body is `return true;`, so the early-return contract this test is
    // named for is only held by the two assertions below.
    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.connect_cue_nodes"), Payload, Capture));
    TestTrue(TEXT("the handler answered rather than returning silently"), Capture.bWasCalled);
    TestFalse(TEXT("a missing required parameter is an error, never a fake success"),
        Capture.bSuccess);
    return true;
}

// ============================================================================
// audio.authoring.create_metasound
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateMetaSoundValidParamsTest,
    "PinWright.audio.authoring.create_metasound.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateMetaSoundValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestMetaSound"));
    Payload->SetStringField(TEXT("path"), TEXT("/Game/Audio/MetaSounds"));
    Payload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.authoring.create_metasound"), Payload));
    // Discard the freshly-created MetaSoundSource via GC rather than force-delete:
    // ForceDeleteObjects on a never-reloaded MetaSound crashes the suite under -unattended.
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(TEXT("/Game/Audio/MetaSounds/TestMetaSound.TestMetaSound"));
    return true;
}

// ============================================================================
// audio.authoring.connect_metasound_nodes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringConnectMetaSoundNodesPartialParamsTest,
    "PinWright.audio.authoring.connect_metasound_nodes.MissingTargetParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringConnectMetaSoundNodesPartialParamsTest::RunTest(const FString& Parameters)
{
    // Provide source params but omit target node id and input name.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Audio/MetaSounds/TestMS"));
    Payload->SetStringField(TEXT("sourceNodeId"), TEXT("node-guid-a"));
    Payload->SetStringField(TEXT("sourceOutputName"), TEXT("Out Float"));
    // Asserted, not merely invoked. The registration lookup alone is satisfied by a
    // handler whose body is `return true;`, so the early-return contract this test is
    // named for is only held by the two assertions below.
    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.connect_metasound_nodes"), Payload, Capture));
    TestTrue(TEXT("the handler answered rather than returning silently"), Capture.bWasCalled);
    TestFalse(TEXT("a missing required parameter is an error, never a fake success"),
        Capture.bSuccess);
    return true;
}

// ============================================================================
// audio.authoring.create_sound_class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundClassValidParamsTest,
    "PinWright.audio.authoring.create_sound_class.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundClassValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSoundClass"));
    Payload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.authoring.create_sound_class"), Payload));
    CleanupTestAsset(TEXT("/Game/Audio/Classes/TestSoundClass"));
    return true;
}

// ============================================================================
// audio.authoring.set_class_parent
// ============================================================================

// Regression test for B-sound-class-parent-no-child-link: create_sound_class (with parentClass)
// and set_class_parent must maintain BOTH sides of the USoundClass hierarchy — the child's
// ParentClass pointer AND the parent's ChildClasses array — and on reparent remove the child from
// the old parent. Before the fix the handlers did a raw `SoundClass->ParentClass = Parent;` write,
// leaving every parent's ChildClasses empty. Each TestTrue on a *->ChildClasses.Contains(...)
// assertion below regresses to false if the fix is reverted to the raw write.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringSetClassParentMaintainsChildClassesTest,
    "PinWright.audio.authoring.set_class_parent.MaintainsChildClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringSetClassParentMaintainsChildClassesTest::RunTest(const FString& Parameters)
{
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = FString::Printf(TEXT("/Game/PinWrightTests/SoundClassHierarchy_%s"), *Stamp);

    const FString MasterName = TEXT("Master");
    const FString GameplayName = TEXT("Gameplay");
    const FString SfxName = TEXT("SFX");

    const FString MasterPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *MasterName, *MasterName);
    const FString GameplayPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *GameplayName, *GameplayName);
    const FString SfxPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *SfxName, *SfxName);

    auto CreateClass = [&](const FString& Name, const FString& ParentClassPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("path"), TestPath);
        if (!ParentClassPath.IsEmpty())
        {
            Payload->SetStringField(TEXT("parentClass"), ParentClassPath);
        }
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(FString::Printf(TEXT("create_sound_class(%s) dispatched"), *Name),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_class"), Payload, Cap));
        TestTrue(FString::Printf(TEXT("create_sound_class(%s) success"), *Name), Cap.bSuccess);
    };

    // --- 1. Master (no parent), Gameplay (child of Master via create_sound_class parentClass) ---
    CreateClass(MasterName, FString());
    CreateClass(GameplayName, MasterPath);

    USoundClass* Master = FindObject<USoundClass>(nullptr, *MasterPath);
    USoundClass* Gameplay = FindObject<USoundClass>(nullptr, *GameplayPath);
    TestNotNull(TEXT("Master loaded"), Master);
    TestNotNull(TEXT("Gameplay loaded"), Gameplay);

    if (Master && Gameplay)
    {
        // create_sound_class parentClass must wire BOTH sides.
        TestEqual(TEXT("Gameplay->ParentClass == Master after create"), Gameplay->ParentClass.Get(), Master);
        TestTrue(TEXT("Master->ChildClasses contains Gameplay after create"),
            Master->ChildClasses.Contains(Gameplay));
    }

    // --- 2. SFX created under Master, then reparented under Gameplay via set_class_parent ---
    CreateClass(SfxName, MasterPath);
    USoundClass* Sfx = FindObject<USoundClass>(nullptr, *SfxPath);
    TestNotNull(TEXT("SFX loaded"), Sfx);

    if (Master && Sfx)
    {
        TestTrue(TEXT("Master->ChildClasses contains SFX after create under Master"),
            Master->ChildClasses.Contains(Sfx));
    }

    if (Sfx && Gameplay && Master)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SfxPath);
        Payload->SetStringField(TEXT("parentPath"), GameplayPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_class_parent (reparent SFX->Gameplay) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_class_parent"), Payload, Cap));
        TestTrue(TEXT("set_class_parent (reparent) success"), Cap.bSuccess);

        // Child pointer updated, new parent gains the child, old parent loses it.
        TestEqual(TEXT("SFX->ParentClass == Gameplay after reparent"), Sfx->ParentClass.Get(), Gameplay);
        TestTrue(TEXT("Gameplay->ChildClasses contains SFX after reparent"),
            Gameplay->ChildClasses.Contains(Sfx));
        TestFalse(TEXT("Master->ChildClasses no longer contains SFX after reparent"),
            Master->ChildClasses.Contains(Sfx));
    }

    // --- 3. Clearing the parent removes the child from the old parent's ChildClasses ---
    if (Sfx && Gameplay)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SfxPath);
        Payload->SetStringField(TEXT("parentPath"), TEXT(""));
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_class_parent (clear) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_class_parent"), Payload, Cap));
        TestTrue(TEXT("set_class_parent (clear) success"), Cap.bSuccess);
        TestNull(TEXT("SFX->ParentClass null after clear"), Sfx->ParentClass.Get());
        TestFalse(TEXT("Gameplay->ChildClasses no longer contains SFX after clear"),
            Gameplay->ChildClasses.Contains(Sfx));
    }

    // --- 4. Cycle guard: parenting Master under its own descendant Gameplay must be rejected ---
    if (Master && Gameplay)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MasterPath);
        Payload->SetStringField(TEXT("parentPath"), GameplayPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_class_parent (cycle) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_class_parent"), Payload, Cap));
        TestFalse(TEXT("set_class_parent (cycle) rejected"), Cap.bSuccess);
        TestEqual(TEXT("set_class_parent (cycle) error code"), Cap.ErrorCode, FString(TEXT("PARENT_CYCLE")));
        // Master must remain a root — the rejected reparent left its parent untouched.
        TestNull(TEXT("Master->ParentClass still null after rejected cycle"), Master->ParentClass.Get());
    }

    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *SfxName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *GameplayName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *MasterName));
    return true;
}

// ============================================================================
// audio.authoring.create_sound_mix
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundMixValidParamsTest,
    "PinWright.audio.authoring.create_sound_mix.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundMixValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestSoundMix"));
    Payload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.authoring.create_sound_mix"), Payload));
    CleanupTestAsset(TEXT("/Game/Audio/Mixes/TestSoundMix"));
    return true;
}

// Regression test for E-create-sound-mix-seed-applytochildren-false: the create_sound_mix
// classAdjusters seed loop built each FSoundClassAdjuster setting only SoundClassObject/
// VolumeAdjuster/PitchAdjuster, never bApplyToChildren — so a seeded adjuster kept the struct's
// zero-init default (false), silently diverging from add_mix_modifier's documented Default:true for
// the same per-class adjuster, and the schema exposed no knob to override it. The fix reads an
// optional per-entry applyToChildren defaulting true (matching add_mix_modifier). This test seeds
// two adjusters — one omitting applyToChildren (must default true) and one explicitly false (must
// stay false) — then reads the live FSoundClassAdjuster entries off the created USoundMix. Before
// the fix the omitted-key adjuster regresses to false (struct default), failing the first assert;
// the explicit-false adjuster guards against a fix that hardcodes true instead of honoring the key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateSoundMixSeedApplyToChildrenTest,
    "PinWright.audio.authoring.create_sound_mix.SeedApplyToChildrenDefaultsTrue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateSoundMixSeedApplyToChildrenTest::RunTest(const FString& Parameters)
{
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = FString::Printf(TEXT("/Game/PinWrightTests/MixApplyChildren_%s"), *Stamp);

    const FString MixName = TEXT("Mix_A2CProbe");
    const FString ClassDefaultName = TEXT("SC_A2CDefault");  // seeded with applyToChildren omitted
    const FString ClassExplicitName = TEXT("SC_A2CExplicit"); // seeded with applyToChildren:false
    const FString MixPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *MixName, *MixName);
    const FString ClassDefaultPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ClassDefaultName, *ClassDefaultName);
    const FString ClassExplicitPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ClassExplicitName, *ClassExplicitName);

    // --- create the two SoundClasses the seed adjusters target ---
    for (const FString& ClassName : { ClassDefaultName, ClassExplicitName })
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ClassName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_class dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_class"), Payload, Cap));
        TestTrue(TEXT("create_sound_class success"), Cap.bSuccess);
    }

    // --- create the SoundMix, seeding both adjusters via classAdjusters ---
    {
        // First adjuster: applyToChildren omitted — the seed loop must default it true (the fix).
        TSharedPtr<FJsonObject> AdjDefault = MakeShared<FJsonObject>();
        AdjDefault->SetStringField(TEXT("soundClass"), ClassDefaultPath);
        AdjDefault->SetNumberField(TEXT("volumeAdjuster"), 0.4);
        AdjDefault->SetNumberField(TEXT("pitchAdjuster"), 1.0);

        // Second adjuster: applyToChildren explicitly false — the seed loop must honor the key,
        // proving the fix reads the per-entry value rather than blanket-forcing true.
        TSharedPtr<FJsonObject> AdjExplicit = MakeShared<FJsonObject>();
        AdjExplicit->SetStringField(TEXT("soundClass"), ClassExplicitPath);
        AdjExplicit->SetNumberField(TEXT("volumeAdjuster"), 0.6);
        AdjExplicit->SetNumberField(TEXT("pitchAdjuster"), 1.0);
        AdjExplicit->SetBoolField(TEXT("applyToChildren"), false);

        TArray<TSharedPtr<FJsonValue>> Adjusters;
        Adjusters.Add(MakeShared<FJsonValueObject>(AdjDefault));
        Adjusters.Add(MakeShared<FJsonValueObject>(AdjExplicit));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), MixName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetArrayField(TEXT("classAdjusters"), Adjusters);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_mix dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_mix"), Payload, Cap));
        TestTrue(TEXT("create_sound_mix success"), Cap.bSuccess);
    }

    // --- read the seeded adjusters back off the live USoundMix (production write path) ---
    USoundMix* Mix = FindObject<USoundMix>(nullptr, *MixPath);
    TestNotNull(TEXT("Mix loaded"), Mix);
    if (Mix)
    {
        const FSoundClassAdjuster* DefaultAdj = Mix->SoundClassEffects.FindByPredicate(
            [&](const FSoundClassAdjuster& A)
            { return A.SoundClassObject && A.SoundClassObject->GetPathName() == ClassDefaultPath; });
        const FSoundClassAdjuster* ExplicitAdj = Mix->SoundClassEffects.FindByPredicate(
            [&](const FSoundClassAdjuster& A)
            { return A.SoundClassObject && A.SoundClassObject->GetPathName() == ClassExplicitPath; });

        TestNotNull(TEXT("seeded default adjuster present"), DefaultAdj);
        TestNotNull(TEXT("seeded explicit adjuster present"), ExplicitAdj);

        // The omitted-key adjuster must default to true (the divergence fix). Pre-fix this is false.
        if (DefaultAdj)
            TestTrue(TEXT("omitted applyToChildren seeds true (matches add_mix_modifier)"),
                DefaultAdj->bApplyToChildren != 0);
        // The explicit-false adjuster must honor the supplied key, not be forced to true.
        if (ExplicitAdj)
            TestFalse(TEXT("explicit applyToChildren:false is honored on the seed path"),
                ExplicitAdj->bApplyToChildren != 0);
    }

    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *MixName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ClassDefaultName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ClassExplicitName));
    return true;
}

// ============================================================================
// audio.authoring.add_mix_modifier
// ============================================================================

// Regression test for B-add-mix-modifier-fade-params-dropped: add_mix_modifier registers
// fadeInTime/fadeOutTime but previously never read them — the documented params were silently
// dropped because the per-class FSoundClassAdjuster has no fade members. The fade is mix-level
// (USoundMix::FadeInTime/FadeOutTime), so the fix writes them onto the loaded USoundMix and
// echoes them back. This test creates a SoundMix + SoundClass (save=false), calls add_mix_modifier
// with fade values, and asserts the mix asset carries them and the result echoes them. Before the
// fix Mix->FadeInTime/FadeOutTime stay 0 and the result has no fade fields — both regress to false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringAddMixModifierPersistsFadeTest,
    "PinWright.audio.authoring.add_mix_modifier.PersistsFade",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringAddMixModifierPersistsFadeTest::RunTest(const FString& Parameters)
{
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = FString::Printf(TEXT("/Game/PinWrightTests/MixFade_%s"), *Stamp);

    const FString MixName = TEXT("Mix_FadeProbe");
    const FString ClassName = TEXT("SC_FadeProbe");
    const FString MixPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *MixName, *MixName);
    const FString ClassPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ClassName, *ClassName);

    // --- create the SoundMix (no fade) and the SoundClass the modifier targets ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), MixName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_mix dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_mix"), Payload, Cap));
        TestTrue(TEXT("create_sound_mix success"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ClassName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_class dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_class"), Payload, Cap));
        TestTrue(TEXT("create_sound_class success"), Cap.bSuccess);
    }

    USoundMix* Mix = FindObject<USoundMix>(nullptr, *MixPath);
    TestNotNull(TEXT("Mix loaded"), Mix);

    // --- add_mix_modifier with fade values; the fades must land on the mix asset ---
    if (Mix)
    {
        // USoundMix's constructor seeds FadeInTime/FadeOutTime to the engine factory default
        // (0.2s each, see Engine/Private/SoundMix.cpp), not 0. The persistence proof below
        // passes 1.5/2.0 — distinct from that default — so a successful match still proves the
        // params were read and written rather than coincidentally matching a pre-existing value.
        TestEqual(TEXT("Mix->FadeInTime starts at engine factory default 0.2"), Mix->FadeInTime, 0.2f);
        TestEqual(TEXT("Mix->FadeOutTime starts at engine factory default 0.2"), Mix->FadeOutTime, 0.2f);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MixPath);
        Payload->SetStringField(TEXT("soundClassPath"), ClassPath);
        Payload->SetNumberField(TEXT("volumeAdjuster"), 0.35);
        Payload->SetNumberField(TEXT("fadeInTime"), 1.5);
        Payload->SetNumberField(TEXT("fadeOutTime"), 2.0);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("add_mix_modifier dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_mix_modifier"), Payload, Cap));
        TestTrue(TEXT("add_mix_modifier success"), Cap.bSuccess);

        // The mix asset must carry the fade times that were passed (the core silent-drop fix).
        TestEqual(TEXT("Mix->FadeInTime persisted from fadeInTime param"), Mix->FadeInTime, 1.5f);
        TestEqual(TEXT("Mix->FadeOutTime persisted from fadeOutTime param"), Mix->FadeOutTime, 2.0f);

        // The result payload must echo the persisted fades so success is truthful, not silent.
        if (Cap.Result.IsValid())
        {
            double EchoIn = -1.0;
            double EchoOut = -1.0;
            TestTrue(TEXT("result echoes fadeInTime"), Cap.Result->TryGetNumberField(TEXT("fadeInTime"), EchoIn));
            TestTrue(TEXT("result echoes fadeOutTime"), Cap.Result->TryGetNumberField(TEXT("fadeOutTime"), EchoOut));
            TestEqual(TEXT("echoed fadeInTime == 1.5"), EchoIn, 1.5);
            TestEqual(TEXT("echoed fadeOutTime == 2.0"), EchoOut, 2.0);
        }
    }

    // --- a second add_mix_modifier that omits the fades must not clobber the persisted values ---
    if (Mix)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MixPath);
        Payload->SetStringField(TEXT("soundClassPath"), ClassPath);
        Payload->SetNumberField(TEXT("volumeAdjuster"), 0.5);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("add_mix_modifier (no fade) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_mix_modifier"), Payload, Cap));
        TestTrue(TEXT("add_mix_modifier (no fade) success"), Cap.bSuccess);
        TestEqual(TEXT("Mix->FadeInTime unchanged when fade omitted"), Mix->FadeInTime, 1.5f);
        TestEqual(TEXT("Mix->FadeOutTime unchanged when fade omitted"), Mix->FadeOutTime, 2.0f);
    }

    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *MixName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ClassName));
    return true;
}

// ============================================================================
// audio.authoring.create_attenuation_settings
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringCreateAttenuationSettingsValidParamsTest,
    "PinWright.audio.authoring.create_attenuation_settings.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringCreateAttenuationSettingsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("TestAttenuation"));
    Payload->SetBoolField(TEXT("save"), false);
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.authoring.create_attenuation_settings"), Payload));
    CleanupTestAsset(TEXT("/Game/Audio/Attenuation/TestAttenuation"));
    return true;
}

// ============================================================================
// audio.authoring.create_reverb_effect  (authoring namespace)
// ============================================================================

#if MCP_TEST_HAS_REVERB
// Regression test for E-get-audio-info-reverb-type-unknown: get_audio_info had cast branches
// for SoundCue/Wave/Class/Mix/Submix/Attenuation/DialogueWave/DialogueVoice but NO UReverbEffect
// branch, so a ReverbEffect (the type create_reverb_effect authors) fell through to the final
// else and reported type:"Unknown" — even though assetClass was correctly "ReverbEffect" in the
// same payload. The fix adds a thin UReverbEffect recognition branch that also echoes the float
// UPROPERTYs create_reverb_effect writes. This test builds a transient UReverbEffect, sets
// distinctive DecayTime/Gain, drives the PRODUCTION get_audio_info handler through the dispatcher,
// and asserts type:"ReverbEffect" plus the echoed reverb fields. If the fix is reverted the type
// regresses to "Unknown" (first assertion fails) and the decayTime/gain fields disappear.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringGetAudioInfoRecognizesReverbEffectTest,
    "PinWright.audio.authoring.get_audio_info.RecognizesReverbEffect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringGetAudioInfoRecognizesReverbEffectTest::RunTest(const FString& Parameters)
{
    FString ReverbPath;
    UReverbEffect* Reverb = NewTransientAudioAsset<UReverbEffect>(TEXT("RE_TypeProbe_"), ReverbPath);
    TestNotNull(TEXT("Transient ReverbEffect created"), Reverb);
    if (!Reverb)
    {
        return false;
    }

    // Distinctive (non-default) values so a successful echo proves the fields were read off this
    // asset rather than coincidentally matching the UReverbEffect constructor defaults.
    Reverb->DecayTime = 3.75f;
    Reverb->Gain = 0.42f;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ReverbPath);
    FTestResponseCapture Cap;
    TestTrue(TEXT("get_audio_info dispatched"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.get_audio_info"), Payload, Cap));
    TestTrue(TEXT("get_audio_info succeeded"), Cap.bSuccess);
    // Asserted, not merely tested for below: every field check in this regression test lives
    // inside the guard, so a success carrying no structured result would skip all of them and
    // leave the test green over a reverted fix.
    TestTrue(TEXT("get_audio_info returned a structured result"), Cap.Result.IsValid());
    if (Cap.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>& R = Cap.Result;
        // The core fix: a ReverbEffect must report type:"ReverbEffect", not the pre-fix "Unknown".
        TestEqual(TEXT("type recognized as ReverbEffect"),
            R->GetStringField(TEXT("type")), FString(TEXT("ReverbEffect")));
        // assetClass was already correct pre-fix; assert it stays consistent with type.
        TestEqual(TEXT("assetClass is ReverbEffect"),
            R->GetStringField(TEXT("assetClass")), FString(TEXT("ReverbEffect")));
        // The enriched readback fields the branch adds (echoing create_reverb_effect's writes).
        double DecayTime = -1.0;
        double Gain = -1.0;
        TestTrue(TEXT("decayTime echoed"), R->TryGetNumberField(TEXT("decayTime"), DecayTime));
        TestTrue(TEXT("gain echoed"), R->TryGetNumberField(TEXT("gain"), Gain));
        TestEqual(TEXT("decayTime matches written value"), DecayTime, 3.75);
        TestEqual(TEXT("gain matches written value"), Gain, static_cast<double>(0.42f));
    }

    Reverb->RemoveFromRoot();
    return true;
}
#endif

// ============================================================================
// audio.authoring.describe_metasound
// ============================================================================

#if __has_include("MetasoundSource.h")
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeMetaSoundReturnsGraphJsonTest,
    "PinWright.audio.authoring.describe_metasound.ReturnsGraphJson",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeMetaSoundReturnsGraphJsonTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientAudioAuthoringMetaSoundPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.describe_metasound handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.describe_metasound"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.describe_metasound sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("audio.authoring.describe_metasound succeeded"), Capture.bSuccess);
    TestTrue(TEXT("audio.authoring.describe_metasound result present"), Capture.Result.IsValid());

    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("assetKind"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("MetaSoundPatch")));
        TestTrue(TEXT("rootGraph object exists"), Capture.Result->HasTypedField<EJson::Object>(TEXT("rootGraph")));
        TestTrue(TEXT("nodes array exists"), Capture.Result->HasTypedField<EJson::Array>(TEXT("nodes")));
        TestTrue(TEXT("edges array exists"), Capture.Result->HasTypedField<EJson::Array>(TEXT("edges")));
        TestTrue(TEXT("variables array exists"), Capture.Result->HasTypedField<EJson::Array>(TEXT("variables")));
    }

    Patch->RemoveFromRoot();
    return true;
}
#endif

// ============================================================================
// audio.authoring.describe_sound_cue
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeSoundCueReturnsGraphTest,
    "PinWright.audio.authoring.describe_sound_cue.ReturnsGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeSoundCueReturnsGraphTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundCue* Cue = NewTransientAudioAuthoringSoundCue(ObjectPath);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.describe_sound_cue handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.describe_sound_cue"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.describe_sound_cue sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("audio.authoring.describe_sound_cue succeeded"), Capture.bSuccess);
    TestTrue(TEXT("audio.authoring.describe_sound_cue result present"), Capture.Result.IsValid());

    if (Capture.Result.IsValid())
    {
        TestEqual(TEXT("assetKind"), Capture.Result->GetStringField(TEXT("assetKind")), FString(TEXT("SoundCue")));
        TestTrue(TEXT("nodes array exists"), Capture.Result->HasTypedField<EJson::Array>(TEXT("nodes")));

        const FString FirstNode = Capture.Result->GetStringField(TEXT("firstNode"));
        TestFalse(TEXT("firstNode is populated"), FirstNode.IsEmpty());

        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        TestTrue(TEXT("nodes array can be read"), Capture.Result->TryGetArrayField(TEXT("nodes"), Nodes));
        TestTrue(TEXT("fixture graph contains nodes"), Nodes && Nodes->Num() >= 2);

        bool bFoundEdge = false;
        if (Nodes)
        {
            for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
            {
                const TSharedPtr<FJsonObject> NodeObject = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
                if (NodeObject.IsValid() && NodeObject->TryGetArrayField(TEXT("edges"), Edges) && Edges && Edges->Num() > 0)
                {
                    bFoundEdge = true;
                    break;
                }
            }
        }
        TestTrue(TEXT("fixture graph contains at least one edge"), bFoundEdge);
    }

    Cue->RemoveFromRoot();
    return true;
}

// ============================================================================
// audio.authoring.describe_attenuation
// ============================================================================

// Regression test for F-rpc-audio-describe-attenuation: the four configure_* verbs write
// ~12 FSoundAttenuationSettings fields but get_audio_info echoes only falloffDistance +
// spatialize. describe_attenuation must echo back the full configured surface. This test
// drives the production configure_* handlers through the dispatcher to set each field, then
// invokes describe_attenuation and asserts every value round-trips. If the describe_attenuation
// handler is reverted, InvokeHandlerWithCapture returns false (handler not found) and the
// first TestTrue fails; if the readback drops a field, the per-field assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeAttenuationEchoesConfiguredFieldsTest,
    "PinWright.audio.authoring.describe_attenuation.EchoesConfiguredFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeAttenuationEchoesConfiguredFieldsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundAttenuation* Atten = NewTransientAudioAttenuation(ObjectPath);
    TestNotNull(TEXT("Transient SoundAttenuation created"), Atten);
    if (!Atten)
    {
        return false;
    }

    // Helper that invokes a configure_* verb through the production dispatcher with save=false.
    auto Configure = [&](const FString& Method, const TFunctionRef<void(TSharedPtr<FJsonObject>&)>& Fill)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetBoolField(TEXT("save"), false);
        Fill(Payload);
        FTestResponseCapture Cap;
        TestTrue(FString::Printf(TEXT("%s dispatched"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Cap));
        TestTrue(FString::Printf(TEXT("%s succeeded"), *Method), Cap.bSuccess);
    };

    Configure(TEXT("audio.authoring.configure_distance_attenuation"), [](TSharedPtr<FJsonObject>& P)
    {
        P->SetNumberField(TEXT("innerRadius"), 400.0);
        P->SetNumberField(TEXT("falloffDistance"), 6000.0);
        P->SetStringField(TEXT("distanceAlgorithm"), TEXT("naturalsound"));
    });
    Configure(TEXT("audio.authoring.configure_spatialization"), [](TSharedPtr<FJsonObject>& P)
    {
        P->SetBoolField(TEXT("spatialize"), true);
        P->SetStringField(TEXT("spatializationAlgorithm"), TEXT("hrtf"));
    });
    Configure(TEXT("audio.authoring.configure_occlusion"), [](TSharedPtr<FJsonObject>& P)
    {
        P->SetBoolField(TEXT("enableOcclusion"), true);
        P->SetNumberField(TEXT("occlusionLowPassFilterFrequency"), 800.0);
        P->SetNumberField(TEXT("occlusionVolumeAttenuation"), 0.4);
        P->SetNumberField(TEXT("occlusionInterpolationTime"), 0.2);
    });
    Configure(TEXT("audio.authoring.configure_reverb_send"), [](TSharedPtr<FJsonObject>& P)
    {
        P->SetBoolField(TEXT("enableReverbSend"), true);
        P->SetNumberField(TEXT("reverbWetLevelMin"), 0.0);
        P->SetNumberField(TEXT("reverbWetLevelMax"), 0.9);
        P->SetNumberField(TEXT("reverbDistanceMin"), 500.0);
        P->SetNumberField(TEXT("reverbDistanceMax"), 5000.0);
    });

    // Full readback through the new RPC.
    TSharedPtr<FJsonObject> DescribePayload = MakeShared<FJsonObject>();
    DescribePayload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.describe_attenuation handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.describe_attenuation"), DescribePayload, Capture));
    TestTrue(TEXT("audio.authoring.describe_attenuation sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("audio.authoring.describe_attenuation succeeded"), Capture.bSuccess);
    TestTrue(TEXT("audio.authoring.describe_attenuation result present"), Capture.Result.IsValid());

    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>& R = Capture.Result;

        TestEqual(TEXT("type"), R->GetStringField(TEXT("type")), FString(TEXT("SoundAttenuation")));

        // Distance attenuation — these are exactly the fields get_audio_info omits.
        TestEqual(TEXT("distanceAlgorithm"), R->GetStringField(TEXT("distanceAlgorithm")), FString(TEXT("naturalsound")));
        TestEqual(TEXT("falloffDistance"), R->GetNumberField(TEXT("falloffDistance")), 6000.0);
        // innerRadius lives in AttenuationShapeExtents.X, not a RadiusMin field.
        TestEqual(TEXT("innerRadius"), R->GetNumberField(TEXT("innerRadius")), 400.0);

        // Spatialization.
        TestTrue(TEXT("spatialize"), R->GetBoolField(TEXT("spatialize")));
        TestEqual(TEXT("spatializationAlgorithm"), R->GetStringField(TEXT("spatializationAlgorithm")), FString(TEXT("hrtf")));

        // Occlusion.
        TestTrue(TEXT("enableOcclusion"), R->GetBoolField(TEXT("enableOcclusion")));
        TestEqual(TEXT("occlusionLowPassFilterFrequency"), R->GetNumberField(TEXT("occlusionLowPassFilterFrequency")), 800.0);
        TestEqual(TEXT("occlusionVolumeAttenuation"), R->GetNumberField(TEXT("occlusionVolumeAttenuation")), 0.4);
        TestEqual(TEXT("occlusionInterpolationTime"), R->GetNumberField(TEXT("occlusionInterpolationTime")), 0.2);

        // Reverb send.
        TestTrue(TEXT("enableReverbSend"), R->GetBoolField(TEXT("enableReverbSend")));
        TestEqual(TEXT("reverbWetLevelMin"), R->GetNumberField(TEXT("reverbWetLevelMin")), 0.0);
        TestEqual(TEXT("reverbWetLevelMax"), R->GetNumberField(TEXT("reverbWetLevelMax")), 0.9);
        TestEqual(TEXT("reverbDistanceMin"), R->GetNumberField(TEXT("reverbDistanceMin")), 500.0);
        TestEqual(TEXT("reverbDistanceMax"), R->GetNumberField(TEXT("reverbDistanceMax")), 5000.0);
    }

    Atten->RemoveFromRoot();
    return true;
}

// ============================================================================
// audio.authoring.describe_sound_class
// ============================================================================

// Regression test for E-audio-get-info-soundclass-mix-readback-thin (SoundClass half):
// set_class_properties writes voiceCenterChannelVolume / lowPassFilterFrequency / lfeBleed
// and set_class_parent maintains both the child's ParentClass AND the parent's ChildClasses,
// but get_audio_info echoes only volume/pitch/parentClass/outputSubmix — never
// voiceCenterChannelVolume nor the child-side childClasses list. describe_sound_class must
// echo the full surface. This test drives the production set_class_properties / set_class_parent
// handlers through the dispatcher, then invokes describe_sound_class on both the child (to
// confirm voiceCenterChannelVolume + parentClass) and the parent (to confirm the childClasses
// array). If describe_sound_class is reverted, InvokeHandlerWithCapture returns false (handler
// not found) and the first TestTrue fails; if a field is dropped, the per-field assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeSoundClassEchoesWrittenFieldsTest,
    "PinWright.audio.authoring.describe_sound_class.EchoesWrittenFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeSoundClassEchoesWrittenFieldsTest::RunTest(const FString& Parameters)
{
    FString ParentPath;
    USoundClass* Parent = NewTransientAudioAsset<USoundClass>(TEXT("SCL_Parent_"), ParentPath);
    TestNotNull(TEXT("Transient parent SoundClass created"), Parent);
    FString ChildPath;
    USoundClass* Child = NewTransientAudioAsset<USoundClass>(TEXT("SCL_Child_"), ChildPath);
    TestNotNull(TEXT("Transient child SoundClass created"), Child);
    if (!Parent || !Child)
    {
        if (Parent) Parent->RemoveFromRoot();
        if (Child) Child->RemoveFromRoot();
        return false;
    }

    // Write voiceCenterChannelVolume (+ siblings get_audio_info also omits) via the
    // production handler, save=false (no on-disk write under -unattended).
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), ChildPath);
        P->SetBoolField(TEXT("save"), false);
        P->SetNumberField(TEXT("voiceCenterChannelVolume"), 0.75);
        P->SetNumberField(TEXT("lowPassFilterFrequency"), 8000.0);
        P->SetNumberField(TEXT("lfeBleed"), 0.25);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_class_properties dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_class_properties"), P, Cap));
        TestTrue(TEXT("set_class_properties succeeded"), Cap.bSuccess);
    }

    // Parent the child — set_class_parent maintains BOTH sides of the link.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), ChildPath);
        P->SetStringField(TEXT("parentPath"), ParentPath);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_class_parent dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_class_parent"), P, Cap));
        TestTrue(TEXT("set_class_parent succeeded"), Cap.bSuccess);
    }

    // describe_sound_class on the CHILD — confirms the leaf-property tune + parentClass.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), ChildPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("describe_sound_class (child) handler found"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.describe_sound_class"), P, Cap));
        TestTrue(TEXT("describe_sound_class (child) succeeded"), Cap.bSuccess);
        // Asserted, not merely tested for below: every field check in this regression
        // test lives inside the guard, so a success carrying no structured result would
        // skip all of them and leave the test green over a reverted fix.
        TestTrue(TEXT("describe_sound_class (child) returned a structured result"), Cap.Result.IsValid());
        if (Cap.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>& R = Cap.Result;
            TestEqual(TEXT("type"), R->GetStringField(TEXT("type")), FString(TEXT("SoundClass")));
            // The exact field get_audio_info omits.
            TestEqual(TEXT("voiceCenterChannelVolume"), R->GetNumberField(TEXT("voiceCenterChannelVolume")), 0.75);
            TestEqual(TEXT("lowPassFilterFrequency"), R->GetNumberField(TEXT("lowPassFilterFrequency")), 8000.0);
            TestEqual(TEXT("lfeBleed"), R->GetNumberField(TEXT("lfeBleed")), 0.25);
            TestEqual(TEXT("parentClass"), R->GetStringField(TEXT("parentClass")), Parent->GetPathName());
        }
    }

    // describe_sound_class on the PARENT — confirms the child-side childClasses array
    // get_audio_info never emits (the B-sound-class-parent-no-child-link readback half).
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), ParentPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("describe_sound_class (parent) handler found"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.describe_sound_class"), P, Cap));
        TestTrue(TEXT("describe_sound_class (parent) succeeded"), Cap.bSuccess);
        // Asserted, not merely tested for below: every field check in this regression
        // test lives inside the guard, so a success carrying no structured result would
        // skip all of them and leave the test green over a reverted fix.
        TestTrue(TEXT("describe_sound_class (parent) returned a structured result"), Cap.Result.IsValid());
        if (Cap.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
            TestTrue(TEXT("childClasses array present"),
                Cap.Result->TryGetArrayField(TEXT("childClasses"), Children));
            if (Children)
            {
                TestEqual(TEXT("childClasses has one entry"), Children->Num(), 1);
                if (Children->Num() == 1)
                {
                    TestEqual(TEXT("childClasses[0] is the child path"),
                        (*Children)[0]->AsString(), Child->GetPathName());
                }
            }
        }
    }

    Child->RemoveFromRoot();
    Parent->RemoveFromRoot();
    return true;
}

// ============================================================================
// audio.authoring.describe_sound_mix
// ============================================================================

// Regression test for E-audio-get-info-soundclass-mix-readback-thin (SoundMix half):
// add_mix_modifier / create_sound_mix classAdjusters write per-class
// soundClass/volumeAdjuster/pitchAdjuster/applyToChildren into SoundClassEffects, but
// get_audio_info echoes ONLY modifierCount — never the per-adjuster values nor any EQ band.
// describe_sound_mix must echo an adjusters[] array with each entry's values AND an eqSettings
// object carrying the 4-band ladder configure_mix_eq writes. This test seeds a mix, adds an
// adjuster via add_mix_modifier, tunes a band via configure_mix_eq, then invokes
// describe_sound_mix and asserts both the adjuster and the EQ band round-trip with their written
// values. If describe_sound_mix is reverted, the first TestTrue fails; if the adjusters array or
// the eqSettings ladder is dropped, the per-adjuster / per-band assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeSoundMixEchoesAdjustersTest,
    "PinWright.audio.authoring.describe_sound_mix.EchoesAdjusters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeSoundMixEchoesAdjustersTest::RunTest(const FString& Parameters)
{
    // A resolvable SoundClass for the adjusters to point at.
    FString ClassPath;
    USoundClass* SC = NewTransientAudioAsset<USoundClass>(TEXT("SCL_MixTarget_"), ClassPath);
    TestNotNull(TEXT("Transient SoundClass created"), SC);
    // A resolvable SoundMix the production handlers can load by path.
    FString MixPath;
    USoundMix* Mix = NewTransientAudioAsset<USoundMix>(TEXT("SMX_Describe_"), MixPath);
    TestNotNull(TEXT("Transient SoundMix created"), Mix);
    if (!SC || !Mix)
    {
        if (SC) SC->RemoveFromRoot();
        if (Mix) Mix->RemoveFromRoot();
        return false;
    }

    // add_mix_modifier writes one FSoundClassAdjuster (soundClass/volumeAdjuster/
    // pitchAdjuster/applyToChildren) into the existing mix's SoundClassEffects.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), MixPath);
        P->SetStringField(TEXT("soundClassPath"), ClassPath);
        P->SetNumberField(TEXT("volumeAdjuster"), 0.4);
        P->SetNumberField(TEXT("pitchAdjuster"), 1.2);
        P->SetBoolField(TEXT("applyToChildren"), false);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("add_mix_modifier dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_mix_modifier"), P, Cap));
        TestTrue(TEXT("add_mix_modifier succeeded"), Cap.bSuccess);
    }

    // configure_mix_eq writes the mix-level applyEQ/eqPriority + a tuned 4-band
    // FEQSettings ladder. describe_sound_mix must read that ladder back (the EQ-band
    // readback gap E-audio-...-readback-thin set out to close, one writer over).
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), MixPath);
        P->SetBoolField(TEXT("applyEQ"), true);
        P->SetNumberField(TEXT("eqPriority"), 3.0);
        TSharedPtr<FJsonObject> EQ = MakeShared<FJsonObject>();
        EQ->SetNumberField(TEXT("frequencyCenter0"), 500.0);
        EQ->SetNumberField(TEXT("gain0"), 2.0);
        EQ->SetNumberField(TEXT("bandwidth0"), 1.5);
        P->SetObjectField(TEXT("eqSettings"), EQ);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("configure_mix_eq dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.configure_mix_eq"), P, Cap));
        TestTrue(TEXT("configure_mix_eq succeeded"), Cap.bSuccess);
    }

    // describe_sound_mix — confirms the per-adjuster values get_audio_info drops.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), MixPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("describe_sound_mix handler found"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.describe_sound_mix"), P, Cap));
        TestTrue(TEXT("describe_sound_mix succeeded"), Cap.bSuccess);
        // Asserted, not merely tested for below: every field check in this regression
        // test lives inside the guard, so a success carrying no structured result would
        // skip all of them and leave the test green over a reverted fix.
        TestTrue(TEXT("describe_sound_mix returned a structured result"), Cap.Result.IsValid());
        if (Cap.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>& R = Cap.Result;
            TestEqual(TEXT("type"), R->GetStringField(TEXT("type")), FString(TEXT("SoundMix")));
            TestEqual(TEXT("modifierCount"), (int32)R->GetNumberField(TEXT("modifierCount")), 1);

            const TArray<TSharedPtr<FJsonValue>>* Adjusters = nullptr;
            TestTrue(TEXT("adjusters array present"),
                R->TryGetArrayField(TEXT("adjusters"), Adjusters));
            if (Adjusters && Adjusters->Num() == 1)
            {
                const TSharedPtr<FJsonObject> A = (*Adjusters)[0]->AsObject();
                TestTrue(TEXT("adjusters[0] is an object"), A.IsValid());
                if (A.IsValid())
                {
                    TestEqual(TEXT("adjusters[0].soundClass"), A->GetStringField(TEXT("soundClass")), SC->GetPathName());
                    TestEqual(TEXT("adjusters[0].volumeAdjuster"), A->GetNumberField(TEXT("volumeAdjuster")), 0.4);
                    TestEqual(TEXT("adjusters[0].pitchAdjuster"), A->GetNumberField(TEXT("pitchAdjuster")), 1.2);
                    TestFalse(TEXT("adjusters[0].applyToChildren"), A->GetBoolField(TEXT("applyToChildren")));
                }
            }
            else
            {
                TestEqual(TEXT("adjusters has one entry"), Adjusters ? Adjusters->Num() : -1, 1);
            }

            // Mix-level EQ readback: applyEQ/eqPriority at top level plus the full
            // band ladder under eqSettings (the writer configure_mix_eq just tuned).
            TestTrue(TEXT("applyEQ"), R->GetBoolField(TEXT("applyEQ")));
            TestEqual(TEXT("eqPriority"), R->GetNumberField(TEXT("eqPriority")), 3.0);
            const TSharedPtr<FJsonObject>* EQ = nullptr;
            TestTrue(TEXT("eqSettings object present"),
                R->TryGetObjectField(TEXT("eqSettings"), EQ));
            if (EQ && (*EQ).IsValid())
            {
                TestEqual(TEXT("eqSettings.frequencyCenter0"), (*EQ)->GetNumberField(TEXT("frequencyCenter0")), 500.0);
                TestEqual(TEXT("eqSettings.gain0"), (*EQ)->GetNumberField(TEXT("gain0")), 2.0);
                TestEqual(TEXT("eqSettings.bandwidth0"), (*EQ)->GetNumberField(TEXT("bandwidth0")), 1.5);
            }
        }
    }

    Mix->RemoveFromRoot();
    SC->RemoveFromRoot();
    return true;
}

// ============================================================================
// audio.authoring.describe_dialogue_voice / describe_dialogue_wave
// ============================================================================

#if MCP_TEST_HAS_DIALOGUE
// Regression test for E-audio-dialogue-no-readback-get-info-unknown: get_audio_info
// recognizes neither UDialogueVoice nor UDialogueWave (both fall to its final else and
// return type:"Unknown"), and there was no in-namespace readback for the fields the
// dialogue write verbs persist (Gender/Plurality on the voice; SpokenText + the
// speaker/target context wiring on the wave). describe_dialogue_voice and
// describe_dialogue_wave are the Dialogue members of the describe_* family. This test
// builds two transient DialogueVoices + one DialogueWave, sets the persisted fields,
// drives the PRODUCTION set_dialogue_context handler through the dispatcher to wire the
// context, then invokes both describe verbs and asserts every field round-trips. If
// either describe verb is reverted, InvokeHandlerWithCapture returns false (handler not
// found) and its TestTrue fails; if a field is dropped, the per-field assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDescribeDialogueEchoesWrittenFieldsTest,
    "PinWright.audio.authoring.describe_dialogue.EchoesWrittenFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDescribeDialogueEchoesWrittenFieldsTest::RunTest(const FString& Parameters)
{
    FString SpeakerPath;
    UDialogueVoice* Speaker = NewTransientAudioAsset<UDialogueVoice>(TEXT("DV_Speaker_"), SpeakerPath);
    TestNotNull(TEXT("Transient speaker DialogueVoice created"), Speaker);
    FString TargetPath;
    UDialogueVoice* Target = NewTransientAudioAsset<UDialogueVoice>(TEXT("DV_Target_"), TargetPath);
    TestNotNull(TEXT("Transient target DialogueVoice created"), Target);
    FString WavePath;
    UDialogueWave* Wave = NewTransientAudioAsset<UDialogueWave>(TEXT("DW_Greeting_"), WavePath);
    TestNotNull(TEXT("Transient DialogueWave created"), Wave);
    if (!Speaker || !Target || !Wave)
    {
        if (Speaker) Speaker->RemoveFromRoot();
        if (Target) Target->RemoveFromRoot();
        if (Wave) Wave->RemoveFromRoot();
        return false;
    }

    // Persist exactly the fields the write verbs set: create_dialogue_voice writes
    // Gender/Plurality, create_dialogue_wave writes SpokenText.
    Speaker->Gender = EGrammaticalGender::Feminine;
    Speaker->Plurality = EGrammaticalNumber::Plural;
    Wave->SpokenText = FString(TEXT("Well met, traveler."));

    // Drive the PRODUCTION set_dialogue_context handler to wire speaker + target.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), WavePath);
        P->SetStringField(TEXT("speakerPath"), SpeakerPath);
        TArray<TSharedPtr<FJsonValue>> Targets;
        Targets.Add(MakeShareable(new FJsonValueString(TargetPath)));
        P->SetArrayField(TEXT("targetVoices"), Targets);
        P->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_dialogue_context dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_dialogue_context"), P, Cap));
        TestTrue(TEXT("set_dialogue_context succeeded"), Cap.bSuccess);
    }

    // describe_dialogue_voice — confirms gender/plurality get_audio_info never echoes.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), SpeakerPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("describe_dialogue_voice handler found"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.describe_dialogue_voice"), P, Cap));
        TestTrue(TEXT("describe_dialogue_voice succeeded"), Cap.bSuccess);
        // Asserted, not merely tested for below: every field check in this regression
        // test lives inside the guard, so a success carrying no structured result would
        // skip all of them and leave the test green over a reverted fix.
        TestTrue(TEXT("describe_dialogue_voice returned a structured result"), Cap.Result.IsValid());
        if (Cap.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>& R = Cap.Result;
            TestEqual(TEXT("voice type"), R->GetStringField(TEXT("type")), FString(TEXT("DialogueVoice")));
            TestEqual(TEXT("voice gender"), R->GetStringField(TEXT("gender")), FString(TEXT("Feminine")));
            TestEqual(TEXT("voice plurality"), R->GetStringField(TEXT("plurality")), FString(TEXT("Plural")));
        }
    }

    // describe_dialogue_wave — confirms spokenText + the speaker/target context wiring.
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), WavePath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("describe_dialogue_wave handler found"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.describe_dialogue_wave"), P, Cap));
        TestTrue(TEXT("describe_dialogue_wave succeeded"), Cap.bSuccess);
        // Asserted, not merely tested for below: every field check in this regression
        // test lives inside the guard, so a success carrying no structured result would
        // skip all of them and leave the test green over a reverted fix.
        TestTrue(TEXT("describe_dialogue_wave returned a structured result"), Cap.Result.IsValid());
        if (Cap.Result.IsValid())
        {
            const TSharedPtr<FJsonObject>& R = Cap.Result;
            TestEqual(TEXT("wave type"), R->GetStringField(TEXT("type")), FString(TEXT("DialogueWave")));
            TestEqual(TEXT("wave spokenText"), R->GetStringField(TEXT("spokenText")),
                FString(TEXT("Well met, traveler.")));
            const TArray<TSharedPtr<FJsonValue>>* Contexts = nullptr;
            TestTrue(TEXT("contexts array present"), R->TryGetArrayField(TEXT("contexts"), Contexts));
            if (Contexts)
            {
                // UDialogueWave's constructor seeds one default, empty FDialogueContextMapping
                // (Engine/Private/DialogueWave.cpp: ContextMappings.Add(FDialogueContextMapping())),
                // so a freshly-created wave reports that default mapping plus the one
                // set_dialogue_context appended — describe_dialogue_wave faithfully surfaces both.
                // Locate the mapping we wired by its speaker rather than assuming index/count.
                TSharedPtr<FJsonObject> WiredCtx;
                for (const TSharedPtr<FJsonValue>& CtxVal : *Contexts)
                {
                    const TSharedPtr<FJsonObject> C = CtxVal->AsObject();
                    if (C.IsValid() && C->GetStringField(TEXT("speaker")) == Speaker->GetPathName())
                    {
                        WiredCtx = C;
                        break;
                    }
                }
                TestNotNull(TEXT("context with wired speaker present"), WiredCtx.Get());
                if (WiredCtx.IsValid())
                {
                    const TArray<TSharedPtr<FJsonValue>>* CtxTargets = nullptr;
                    TestTrue(TEXT("context targets present"),
                        WiredCtx->TryGetArrayField(TEXT("targets"), CtxTargets));
                    if (CtxTargets)
                    {
                        // FDialogueContext's constructor seeds one zeroed (null) target
                        // (Engine/Private/DialogueTypes.cpp: Targets.AddZeroed()), so the
                        // production set_dialogue_context handler's appended target lands at
                        // index 1 behind that stray null — the B-dialogue-context-null-target-
                        // prepended bug, which describe_dialogue_wave correctly surfaces as a
                        // leading empty-string target path. Assert our real target is present
                        // (round-trip verified) rather than assuming a single clean entry.
                        bool bFoundTarget = false;
                        for (const TSharedPtr<FJsonValue>& T : *CtxTargets)
                        {
                            if (T->AsString() == Target->GetPathName())
                            {
                                bFoundTarget = true;
                                break;
                            }
                        }
                        TestTrue(TEXT("wired target voice path present in targets"), bFoundTarget);
                    }
                }
            }
        }
    }

    Wave->RemoveFromRoot();
    Target->RemoveFromRoot();
    Speaker->RemoveFromRoot();
    return true;
}

#endif // MCP_TEST_HAS_DIALOGUE

// ============================================================================
// audio.authoring.decompile_sound_cue
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringDecompileSoundCueReturnsSCIRTest,
    "PinWright.audio.authoring.decompile_sound_cue.ReturnsSCIR",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringDecompileSoundCueReturnsSCIRTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    USoundCue* Cue = NewTransientAudioAuthoringSoundCue(ObjectPath);
    TestNotNull(TEXT("Transient SoundCue created"), Cue);
    if (!Cue)
    {
        return false;
    }

    FString ContentObjectPath = ObjectPath;
    ContentObjectPath.RemoveFromStart(TEXT("/Game/"));
    ContentObjectPath = TEXT("/Content/") + ContentObjectPath;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ContentObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.decompile_sound_cue handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.decompile_sound_cue"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.decompile_sound_cue sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("audio.authoring.decompile_sound_cue succeeded"), Capture.bSuccess);
    TestTrue(TEXT("audio.authoring.decompile_sound_cue result present"), Capture.Result.IsValid());

    if (Capture.Result.IsValid())
    {
        FString Ir;
        TestTrue(TEXT("ir field exists"), Capture.Result->TryGetStringField(TEXT("ir"), Ir));
        TestTrue(TEXT("SCIR contains sound_cue entry"), Ir.Contains(TEXT("sound_cue")));
        TestTrue(TEXT("SCIR contains root mixer"), Ir.Contains(TEXT("root mixer")));
        TestTrue(TEXT("SCIR contains child wave_player"), Ir.Contains(TEXT("child wave_player")));
        TestTrue(TEXT("warnings array exists"), Capture.Result->HasTypedField<EJson::Array>(TEXT("warnings")));
        TestEqual(TEXT("assetPath is normalized"), Capture.Result->GetStringField(TEXT("assetPath")), ObjectPath);
    }

    Cue->RemoveFromRoot();
    return true;
}

// ============================================================================
// audio.authoring.get_audio_info
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioAuthoringGetAudioInfoValidParamsTest,
    "PinWright.audio.authoring.get_audio_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioAuthoringGetAudioInfoValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/Audio/Waves/TestWave"));
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.authoring.get_audio_info"), Payload));
    return true;
}

// ============================================================================
// AudioHandler.cpp — audio namespace
// ============================================================================

// ---- audio.play_sound_at_location ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioPlaySoundAtLocationValidParamsTest,
    "PinWright.audio.play_sound_at_location.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioPlaySoundAtLocationValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("soundPath"), TEXT("/Game/Audio/Waves/TestWave"));
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.play_sound_at_location"), Payload));
    return true;
}

// ---- audio.play_sound_2d ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioPlaySound2DValidParamsTest,
    "PinWright.audio.play_sound_2d.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioPlaySound2DValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("soundPath"), TEXT("/Game/Audio/Waves/TestWave"));
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandler(TEXT("audio.play_sound_2d"), Payload));
    return true;
}

// ---- audio.push_sound_mix ----

// ---- audio.pop_sound_mix ----

// ---- audio.set_sound_mix_class_override ----

// ---- audio.play_sound_attached ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioPlaySoundAttachedMissingActorNameTest,
    "PinWright.audio.play_sound_attached.MissingActorName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioPlaySoundAttachedMissingActorNameTest::RunTest(const FString& Parameters)
{
    // soundPath present but actorName absent.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("soundPath"), TEXT("/Game/Audio/Waves/TestWave"));
    // Asserted, not merely invoked. The registration lookup alone is satisfied by a
    // handler whose body is `return true;`, so the early-return contract this test is
    // named for is only held by the two assertions below.
    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler found and invoked"),
        InvokeHandlerWithCapture(TEXT("audio.play_sound_attached"), Payload, Capture));
    TestTrue(TEXT("the handler answered rather than returning silently"), Capture.bWasCalled);
    TestFalse(TEXT("a missing required parameter is an error, never a fake success"),
        Capture.bSuccess);
    return true;
}

// ---- audio.fade_sound_out ----

// ---- audio.fade_sound_in ----

// ---- audio.create_ambient_sound ----

// Regression test for B-create-ambient-sound-no-actor: the handler's doc promises
// "Spawn an AAmbientSound actor ... stays in the level until deleted", but it used
// to call UGameplayStatics::SpawnSoundAtLocation, which creates only a transient
// UAudioComponent parented to the world — no actor. This test drives the handler
// against the live editor world and asserts a real AAmbientSound *actor* now exists
// in the level carrying the requested sound. It fails (no actor spawned) if the fix
// is reverted to the SpawnSoundAtLocation behavior.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioCreateAmbientSoundSpawnsActorTest,
    "PinWright.audio.create_ambient_sound.SpawnsAmbientSoundActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioCreateAmbientSoundSpawnsActorTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No GEditor; skipping editor-world spawn assertion."));
        return true;
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world; skipping editor-world spawn assertion."));
        return true;
    }

    // Create a registered transient SoundWave so ResolveSoundAsset finds it by path.
    FString ObjectPath;
    USoundWave* SoundWave = NewTransientAudioSoundWave(ObjectPath);
    TestNotNull(TEXT("transient SoundWave created"), SoundWave);
    if (!SoundWave)
    {
        return true;
    }

    // Resolve the (MinimalAPI) AAmbientSound class once by reflection; used to find
    // ambient actors before/after so the test attributes the new actor to this handler.
    UClass* AmbientClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AmbientSound"));
    TestNotNull(TEXT("AAmbientSound class resolves by reflection"), AmbientClass);

    // Single world walk that returns every AAmbientSound actor. AAmbientSound is
    // MinimalAPI — its StaticClass() is not exported, so TActorIterator<AAmbientSound>
    // / Cast<AAmbientSound> would fail to link; match by reflection-resolved class
    // + IsA instead. Both the before count and the post-spawn count/find derive from
    // one call, so the predicate lives in exactly one place.
    auto FindAmbientActors = [World, AmbientClass]() -> TArray<AActor*>
    {
        TArray<AActor*> Out;
        if (AmbientClass)
        {
            McpActorUtils::ForEachActor(World, [&Out, AmbientClass](AActor* Actor)
            {
                if (Actor && Actor->IsA(AmbientClass)) { Out.Add(Actor); }
                return true;
            });
        }
        return Out;
    };
    const int32 AmbientBefore = FindAmbientActors().Num();

    // Guard records the level's actor set and restores it on scope exit, destroying
    // the AAmbientSound the handler spawns so the open map is left untouched.
    {
        FScopedEditorWorldActorGuard WorldGuard;

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("soundPath"), ObjectPath);
        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(200.0));
        Loc.Add(MakeShared<FJsonValueNumber>(100.0));
        Loc.Add(MakeShared<FJsonValueNumber>(120.0));
        Payload->SetArrayField(TEXT("location"), Loc);
        Payload->SetNumberField(TEXT("volume"), 0.4);

        FTestResponseCapture Capture;
        TestTrue(TEXT("create_ambient_sound handler found"),
            InvokeHandlerWithCapture(TEXT("audio.create_ambient_sound"), Payload, Capture));
        TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        TestTrue(TEXT("handler succeeded (no SPAWN_FAILED / ASSET_NOT_FOUND)"), Capture.bSuccess);
        TestTrue(TEXT("handler returned a payload"), Capture.Result.IsValid());

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            // The promised actor must actually exist in the level.
            FString ActorClass;
            TestTrue(TEXT("response reports actorClass"),
                Capture.Result->TryGetStringField(TEXT("actorClass"), ActorClass));
            TestEqual(TEXT("spawned an AmbientSound actor, not a bare component"),
                ActorClass, FString(TEXT("AmbientSound")));

            // One world walk yields both the post-spawn count assertion and the
            // spawned actor; the handler must have placed exactly one NEW AAmbientSound.
            const TArray<AActor*> AmbientActors = FindAmbientActors();
            TestEqual(TEXT("one new AAmbientSound actor was spawned"),
                AmbientActors.Num(), AmbientBefore + 1);

            AActor* Spawned = AmbientActors.Num() > 0 ? AmbientActors[0] : nullptr;
            TestNotNull(TEXT("an AAmbientSound actor exists in the editor world"), Spawned);

            if (Spawned)
            {
                UAudioComponent* AudioComp = Spawned->FindComponentByClass<UAudioComponent>();
                TestNotNull(TEXT("ambient actor has an AudioComponent"), AudioComp);
                if (AudioComp)
                {
                    TestTrue(TEXT("ambient actor's AudioComponent carries the requested sound"),
                        static_cast<USoundBase*>(AudioComp->Sound) == static_cast<USoundBase*>(SoundWave));
                }
            }
        }
    }

    // Teardown: drop the transient SoundWave the way never-saved assets are discarded.
    SoundWave->RemoveFromRoot();
    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    return true;
}

// ---- audio.spawn_sound_at_location ----

// ---- audio.clear_sound_mix_class_override ----

// ---- audio.set_base_sound_mix ----

// ---- audio.prime_sound ----

// ---- audio.create_audio_component ----
