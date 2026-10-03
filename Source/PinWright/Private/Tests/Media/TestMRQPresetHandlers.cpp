// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for MRQ preset authoring (mrq.create_preset / mrq.set_preset_settings) and for the
// sampling disclosure on mrq.create_job's `preflight` block.
//
// THE DEFECTS THESE PIN. No verb could make a preset, so a project without one rendered at engine
// CDO defaults or dropped to python.execute (F-mrq-preset-authoring); and no verb could read the
// anti-aliasing sample / warm-up counts a preset carries (B-mrq-config-readback-omits-sampling).
//
// COUNTERFACTUALS:
//  - Authoring: the values are asserted twice — in the response's `preflight` block AND read
//    straight off the asset object after the save — and the package must exist on disk. A verb that echoed
//    the request passes the first and fails the second; one that never saved fails the third.
//  - Refusals: every bad payload must leave NO package behind, so a verb that created first and
//    validated after fails even when it returns the right error code.
//  - set_preset_settings: a `sampling` block on a preset without an anti-aliasing setting must be
//    refused AND leave the preset without one; a verb that silently added the setting fails.
//  - Vocabulary: the nested keys the two verbs declare must be exactly the readback tables' keys,
//    so the words that write a value and the words that report it cannot drift.
//  - create_job: `preflight.sampling` must carry the values planted on the preset, and be absent
//    for a preset with no anti-aliasing setting.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/MRQ/MRQArtifactReport.h"
#include "Handlers/ParamSpec.h"
#include "LevelSequence.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if __has_include("MoviePipelinePrimaryConfig.h") && \
    __has_include("MoviePipelineAntiAliasingSetting.h") && \
    __has_include("MoviePipelineQueueSubsystem.h")
    #include "Editor.h"
    #include "MoviePipelineAntiAliasingSetting.h"
    #include "MoviePipelineOutputSetting.h"
    #include "MoviePipelinePrimaryConfig.h"
    #include "MoviePipelineQueue.h"
    #include "MoviePipelineQueueSubsystem.h"
    #define PINWRIGHT_TEST_HAS_MRQ_PRESETS 1
#else
    #define PINWRIGHT_TEST_HAS_MRQ_PRESETS 0
#endif

#if PINWRIGHT_TEST_HAS_MRQ_PRESETS

namespace TestMRQPresetHandlersHelpers
{
    const TCHAR* PngWriterClassPath = TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_PNG");
    const TCHAR* Mp4WriterClassPath = TEXT("/Script/MovieRenderPipelineMP4Encoder.MoviePipelineMP4EncoderOutput");
    const TCHAR* AntiAliasingClassPath = TEXT("/Script/MovieRenderPipelineCore.MoviePipelineAntiAliasingSetting");
    const TCHAR* DeferredBaseClassPath = TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineDeferredPassBase");
    const TCHAR* DeferredUnlitClassPath = TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineDeferredPass_Unlit");

    struct FFixturePath
    {
        FString PackageName;
        FString ObjectPath;
    };

    FFixturePath NewFixturePath(const TCHAR* Prefix)
    {
        const FString Name = FString::Printf(TEXT("%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        FFixturePath Path;
        Path.PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *Name);
        Path.ObjectPath = FString::Printf(TEXT("%s.%s"), *Path.PackageName, *Name);
        return Path;
    }

    TArray<TSharedPtr<FJsonValue>> StringValues(std::initializer_list<const TCHAR*> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const TCHAR* Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    bool PackageLeftBehind(const FFixturePath& Path)
    {
        return FindPackage(nullptr, *Path.PackageName) != nullptr
            || FPackageName::DoesPackageExist(Path.PackageName);
    }

    bool ArrayHasString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
        {
            return false;
        }
        return Values->ContainsByPredicate([&Value](const TSharedPtr<FJsonValue>& Entry)
            { return Entry.IsValid() && Entry->AsString() == Value; });
    }

    TSharedPtr<FJsonObject> ChildObject(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Found = nullptr;
        return Parent.IsValid() && Parent->TryGetObjectField(Field, Found) && Found ? *Found : nullptr;
    }

    bool MovieRenderClassesLoaded()
    {
        return FindObject<UClass>(nullptr, PngWriterClassPath) != nullptr
            && FindObject<UClass>(nullptr, AntiAliasingClassPath) != nullptr;
    }

    // Create a PNG-writing preset through the verb under test; the base fixture for the edit tests.
    bool CreatePngPreset(FAutomationTestBase& Test, const FFixturePath& Path, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Path.ObjectPath);
        Payload->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath }));
        TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
        Output->SetNumberField(TEXT("width"), 320);
        Output->SetNumberField(TEXT("height"), 180);
        Payload->SetObjectField(TEXT("output"), Output);
        Test.TestTrue(TEXT("mrq.create_preset is registered"),
            InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Payload, Capture));
        return Test.TestTrue(FString::Printf(TEXT("base preset created ([%s] %s)"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess && Capture.Result.IsValid());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreatePresetSavesWhatCreateJobReadsTest,
    "PinWright.mrq.create_preset.SavesWhatCreateJobReads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreatePresetSavesWhatCreateJobReadsTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    if (!MovieRenderClassesLoaded())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_render_pass_classes_absent"),
            TEXT("The PNG writer or anti-aliasing setting class is not loaded, so no preset could "
                 "be authored from them."));
        return true;
    }
    const FFixturePath Path = NewFixturePath(TEXT("PW_MRQPreset"));
    ON_SCOPE_EXIT { CleanupTestAsset(Path.PackageName); };

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Directory = FString::Printf(TEXT("{project_dir}/Saved/PWPreset_%s/"), *Stamp);
    const FString FileName = FString::Printf(TEXT("PW_%s.{frame_number}"), *Stamp);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    Payload->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath }));
    TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
    Output->SetNumberField(TEXT("width"), 320);
    Output->SetNumberField(TEXT("height"), 180);
    Output->SetStringField(TEXT("outputDirectory"), Directory);
    Output->SetStringField(TEXT("fileNameFormat"), FileName);
    Output->SetNumberField(TEXT("frameRate"), 24);
    Payload->SetObjectField(TEXT("output"), Output);
    TSharedPtr<FJsonObject> Sampling = MakeShared<FJsonObject>();
    Sampling->SetNumberField(TEXT("spatialSampleCount"), 2);
    Sampling->SetNumberField(TEXT("temporalSampleCount"), 5);
    Sampling->SetNumberField(TEXT("engineWarmUpCount"), 17);
    Sampling->SetBoolField(TEXT("useCameraCutForWarmUp"), false);
    Sampling->SetStringField(TEXT("antiAliasingMethod"), TEXT("AAM_None"));
    Payload->SetObjectField(TEXT("sampling"), Sampling);

    FTestResponseCapture Capture;
    TestTrue(TEXT("mrq.create_preset is registered"),
        InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Payload, Capture));
    if (!TestTrue(FString::Printf(TEXT("the preset was created ([%s] %s)"), *Capture.ErrorCode,
            *Capture.Message), Capture.bSuccess && Capture.Result.IsValid()))
    {
        return true;
    }

    // 1. The response, read off the asset after the save by the create_job reader.
    TestTrue(TEXT("saved:true is reported"), Capture.Result->GetBoolField(TEXT("saved")));
    TestTrue(TEXT("created:true"), Capture.Result->GetBoolField(TEXT("created")));
    const TSharedPtr<FJsonObject> Preflight = ChildObject(Capture.Result, TEXT("preflight"));
    if (!TestTrue(TEXT("a preflight block is returned"), Preflight.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Resolution = ChildObject(Preflight, TEXT("resolution"));
    TestTrue(TEXT("resolution 320x180 disclosed"), Resolution.IsValid()
        && Resolution->GetNumberField(TEXT("width")) == 320 && Resolution->GetNumberField(TEXT("height")) == 180);
    TestEqual(TEXT("output directory disclosed"), Preflight->GetStringField(TEXT("outputDirectory")), Directory);
    TestEqual(TEXT("file name format disclosed"), Preflight->GetStringField(TEXT("fileNameFormat")), FileName);
    TestEqual(TEXT("the frame-rate override is on and 24"), Preflight->GetNumberField(TEXT("frameRateOverride")), 24.0);
    TestTrue(TEXT("outputs lists the PNG writer"), ArrayHasString(Preflight, TEXT("outputs"), PngWriterClassPath));
    TestTrue(TEXT("settings lists the anti-aliasing setting the sampling block added"),
        ArrayHasString(Preflight, TEXT("settings"), AntiAliasingClassPath));
    TestTrue(TEXT("settingsAdded names it too"), ArrayHasString(Capture.Result, TEXT("settingsAdded"), AntiAliasingClassPath));
    const TSharedPtr<FJsonObject> SamplingOut = ChildObject(Preflight, TEXT("sampling"));
    if (TestTrue(TEXT("preflight.sampling is published"), SamplingOut.IsValid()))
    {
        TestEqual(TEXT("spatial"), static_cast<int32>(SamplingOut->GetNumberField(TEXT("spatialSampleCount"))), 2);
        TestEqual(TEXT("temporal"), static_cast<int32>(SamplingOut->GetNumberField(TEXT("temporalSampleCount"))), 5);
        TestEqual(TEXT("engine warm-up"), static_cast<int32>(SamplingOut->GetNumberField(TEXT("engineWarmUpCount"))), 17);
        TestEqual(TEXT("AA method"), SamplingOut->GetStringField(TEXT("antiAliasingMethod")), FString(TEXT("AAM_None")));
        TestTrue(TEXT("naming a method turned the override on"), SamplingOut->GetBoolField(TEXT("overrideAntiAliasing")));
    }

    // 2. The asset itself, not the response.
    const UMoviePipelinePrimaryConfig* Config = FindObject<UMoviePipelinePrimaryConfig>(nullptr, *Path.ObjectPath);
    const UMoviePipelineAntiAliasingSetting* Setting = Config
        ? Config->FindSetting<UMoviePipelineAntiAliasingSetting>() : nullptr;
    if (TestNotNull(TEXT("the asset carries an anti-aliasing setting"), Setting))
    {
        TestEqual(TEXT("asset temporal count"), Setting->TemporalSampleCount, 5);
        TestEqual(TEXT("asset engine warm-up"), Setting->EngineWarmUpCount, 17);
        TestTrue(TEXT("asset AA override"), Setting->bOverrideAntiAliasing);
    }
    const UMoviePipelineOutputSetting* OutputSetting = Config ? Config->FindSetting<UMoviePipelineOutputSetting>() : nullptr;
    TestTrue(TEXT("asset frame rate is 24/1 with the override on"), OutputSetting
        && OutputSetting->bUseCustomFrameRate && OutputSetting->OutputFrameRate == FFrameRate(24, 1));

    // 3. Disk and discovery: the package file exists and mrq.list_presets enumerates it.
    TestTrue(TEXT("the .uasset is on disk"), FPackageName::DoesPackageExist(Path.PackageName));
    FTestResponseCapture List;
    InvokeHandlerWithCapture(TEXT("mrq.list_presets"), MakeShared<FJsonObject>(), List);
    const TArray<TSharedPtr<FJsonValue>>* Presets = nullptr;
    TestTrue(TEXT("mrq.list_presets enumerates the new preset"),
        List.bSuccess && List.Result.IsValid() && List.Result->TryGetArrayField(TEXT("presets"), Presets) && Presets
        && Presets->ContainsByPredicate([&Path](const TSharedPtr<FJsonValue>& Entry)
            { return Entry->AsObject()->GetStringField(TEXT("assetPath")) == Path.ObjectPath; }));

    // 4. A second create on the occupied path is refused and changes nothing.
    FTestResponseCapture Again;
    InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Payload, Again);
    TestFalse(TEXT("a second create on the same path fails"), Again.bSuccess);
    TestEqual(TEXT("with ASSET_ALREADY_EXISTS"), Again.ErrorCode, FString(ErrorCodes::ERR_ASSET_ALREADY_EXISTS));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreatePresetRefusesBeforeCreatingTest,
    "PinWright.mrq.create_preset.RefusesBeforeCreatingAnything",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreatePresetRefusesBeforeCreatingTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    if (!MovieRenderClassesLoaded())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_render_pass_classes_absent"),
            TEXT("The PNG writer or anti-aliasing setting class is not loaded, so the refusals "
                 "could not be exercised against a valid base payload."));
        return true;
    }
    bSuppressLogs = true;

    struct FCase
    {
        const TCHAR* Name;
        const TCHAR* ExpectedCode;
        TFunction<void(TSharedPtr<FJsonObject>&)> Mutate;
    };
    auto Block = [](TSharedPtr<FJsonObject>& Payload, const TCHAR* Name) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Payload->SetObjectField(Name, Out);
        return Out;
    };
    const FCase Cases[] = {
        { TEXT("unknown class"), ErrorCodes::ERR_CLASS_NOT_FOUND, [](TSharedPtr<FJsonObject>& P)
            { P->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath, TEXT("/Script/MovieRenderPipelineCore.NoSuchSetting_PW") })); } },
        { TEXT("not a setting class"), ErrorCodes::ERR_INVALID_CLASS, [](TSharedPtr<FJsonObject>& P)
            { P->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath, TEXT("/Script/Engine.Actor") })); } },
        { TEXT("no file writer"), ErrorCodes::ERR_INVALID_ARGUMENT, [](TSharedPtr<FJsonObject>& P)
            { P->SetArrayField(TEXT("settings"), StringValues({ AntiAliasingClassPath })); } },
        { TEXT("spatial count below ClampMin"), ErrorCodes::ERR_INVALID_ARGUMENT, [&Block](TSharedPtr<FJsonObject>& P)
            { Block(P, TEXT("sampling"))->SetNumberField(TEXT("spatialSampleCount"), 0); } },
        { TEXT("fractional count"), ErrorCodes::ERR_INVALID_ARGUMENT, [&Block](TSharedPtr<FJsonObject>& P)
            { Block(P, TEXT("sampling"))->SetNumberField(TEXT("temporalSampleCount"), 2.5); } },
        { TEXT("unknown AA method"), ErrorCodes::ERR_INVALID_ARGUMENT, [&Block](TSharedPtr<FJsonObject>& P)
            { Block(P, TEXT("sampling"))->SetStringField(TEXT("antiAliasingMethod"), TEXT("AAM_NoSuchMethod")); } },
        { TEXT("method with override explicitly off"), ErrorCodes::ERR_INVALID_ARGUMENT, [&Block](TSharedPtr<FJsonObject>& P)
            {
                TSharedPtr<FJsonObject> S = Block(P, TEXT("sampling"));
                S->SetStringField(TEXT("antiAliasingMethod"), TEXT("AAM_None"));
                S->SetBoolField(TEXT("overrideAntiAliasing"), false);
            } },
        { TEXT("encoder with no video output"), ErrorCodes::ERR_INVALID_ARGUMENT, [&Block](TSharedPtr<FJsonObject>& P)
            { Block(P, TEXT("encoder"))->SetStringField(TEXT("rateControl"), TEXT("VariableBitRate")); } },
        { TEXT("traversing output directory"), ErrorCodes::ERR_INVALID_PATH, [&Block](TSharedPtr<FJsonObject>& P)
            { Block(P, TEXT("output"))->SetStringField(TEXT("outputDirectory"), TEXT("{project_dir}/../Escaped/")); } },
        { TEXT("zero width"), ErrorCodes::ERR_INVALID_ARGUMENT, [&Block](TSharedPtr<FJsonObject>& P)
            { Block(P, TEXT("output"))->SetNumberField(TEXT("width"), 0); } },
    };

    for (const FCase& Case : Cases)
    {
        const FFixturePath Path = NewFixturePath(TEXT("PW_MRQPresetRefused"));
        ON_SCOPE_EXIT { CleanupTestAsset(Path.PackageName); };
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Path.ObjectPath);
        Payload->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath }));
        Case.Mutate(Payload);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Payload, Capture);
        TestFalse(FString::Printf(TEXT("%s: refused"), Case.Name), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s: error code"), Case.Name), Capture.ErrorCode, FString(Case.ExpectedCode));
        TestFalse(FString::Printf(TEXT("%s: no package was created"), Case.Name), PackageLeftBehind(Path));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreatePresetEncoderReachesVideoOutputTest,
    "PinWright.mrq.create_preset.EncoderBlockReachesTheVideoOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreatePresetEncoderReachesVideoOutputTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    if (!FindObject<UClass>(nullptr, Mp4WriterClassPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mp4_encoder_module_absent"),
            TEXT("UMoviePipelineMP4EncoderOutput is not loaded in this build, so the encoder block "
                 "was not written to a real video output."));
        return true;
    }
    const FFixturePath Path = NewFixturePath(TEXT("PW_MRQPresetMp4"));
    ON_SCOPE_EXIT { CleanupTestAsset(Path.PackageName); };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    Payload->SetArrayField(TEXT("settings"), StringValues({ Mp4WriterClassPath }));
    TSharedPtr<FJsonObject> Encoder = MakeShared<FJsonObject>();
    Encoder->SetStringField(TEXT("rateControl"), TEXT("VariableBitRate"));
    Encoder->SetNumberField(TEXT("averageBitrateMbps"), 40);
    Encoder->SetNumberField(TEXT("maxBitrateMbps"), 48);
    Encoder->SetBoolField(TEXT("includeAudio"), false);
    Payload->SetObjectField(TEXT("encoder"), Encoder);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Payload, Capture);
    if (!TestTrue(FString::Printf(TEXT("created ([%s] %s)"), *Capture.ErrorCode, *Capture.Message),
        Capture.bSuccess && Capture.Result.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Requested = ChildObject(ChildObject(Capture.Result, TEXT("preflight")), TEXT("encoderRequested"));
    if (!TestTrue(TEXT("encoderRequested read off the saved preset"), Requested.IsValid()))
    {
        return true;
    }
    // The engine default is Quality / includeAudio true, so each of these moved off it.
    TestEqual(TEXT("rate control written"), Requested->GetStringField(TEXT("rateControl")), FString(TEXT("VariableBitRate")));
    TestEqual(TEXT("average bitrate written"), Requested->GetNumberField(TEXT("averageBitrateMbps")), 40.0);
    TestEqual(TEXT("max bitrate written"), Requested->GetNumberField(TEXT("maxBitrateMbps")), 48.0);
    TestFalse(TEXT("audio track switched off"), Requested->GetBoolField(TEXT("includeAudio")));
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    const bool bUnboundedWarning = Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings
        && Warnings->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
            { return Value->AsString().Contains(TEXT("no lower bitrate bound")); });
    TestFalse(TEXT("a bounded rate control raises no unbounded-encode warning"), bUnboundedWarning);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQSetPresetSettingsRefusesMissingSettingTest,
    "PinWright.mrq.set_preset_settings.RefusesASettingThePresetLacks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQSetPresetSettingsRefusesMissingSettingTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    if (!MovieRenderClassesLoaded())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_render_pass_classes_absent"),
            TEXT("The PNG writer or anti-aliasing setting class is not loaded, so no base preset "
                 "could be authored to edit."));
        return true;
    }
    bSuppressLogs = true;
    const FFixturePath Path = NewFixturePath(TEXT("PW_MRQPresetEdit"));
    ON_SCOPE_EXIT { CleanupTestAsset(Path.PackageName); };
    FTestResponseCapture Created;
    if (!CreatePngPreset(*this, Path, Created))
    {
        return true;
    }
    const UMoviePipelinePrimaryConfig* Config = FindObject<UMoviePipelinePrimaryConfig>(nullptr, *Path.ObjectPath);
    if (!TestNotNull(TEXT("base preset is loaded"), Config)
        || !TestNull(TEXT("precondition: the base preset has no anti-aliasing setting"),
            Config->FindSetting<UMoviePipelineAntiAliasingSetting>(true)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Sampling = MakeShared<FJsonObject>();
    Sampling->SetNumberField(TEXT("temporalSampleCount"), 3);

    // 1. Without addSettings: refused, and the preset still has no AA setting.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    Payload->SetObjectField(TEXT("sampling"), Sampling);
    FTestResponseCapture Refused;
    TestTrue(TEXT("mrq.set_preset_settings is registered"),
        InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), Payload, Refused));
    TestFalse(TEXT("sampling on a preset without the setting is refused"), Refused.bSuccess);
    TestEqual(TEXT("with MRQ_SETTING_NOT_PRESENT"), Refused.ErrorCode, FString(ErrorCodes::ERR_MRQ_SETTING_NOT_PRESENT));
    TestNull(TEXT("and no anti-aliasing setting was created behind the refusal"),
        Config->FindSetting<UMoviePipelineAntiAliasingSetting>(true));

    // 2. Re-adding a setting the preset already has is refused too.
    TSharedPtr<FJsonObject> Duplicate = MakeShared<FJsonObject>();
    Duplicate->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    Duplicate->SetArrayField(TEXT("addSettings"), StringValues({ PngWriterClassPath }));
    FTestResponseCapture DuplicateRefused;
    InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), Duplicate, DuplicateRefused);
    TestEqual(TEXT("adding a class the preset carries is refused"),
        DuplicateRefused.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    // 3. Asked for explicitly: added, written, saved, and `previous` shows the state before.
    Payload->SetArrayField(TEXT("addSettings"), StringValues({ AntiAliasingClassPath }));
    TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
    Output->SetNumberField(TEXT("width"), 640);
    Payload->SetObjectField(TEXT("output"), Output);
    FTestResponseCapture Edited;
    InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), Payload, Edited);
    if (!TestTrue(FString::Printf(TEXT("edit with addSettings succeeds ([%s] %s)"), *Edited.ErrorCode,
        *Edited.Message), Edited.bSuccess && Edited.Result.IsValid()))
    {
        return true;
    }
    TestTrue(TEXT("saved:true"), Edited.Result->GetBoolField(TEXT("saved")));
    TestTrue(TEXT("settingsAdded names the AA setting"), ArrayHasString(Edited.Result, TEXT("settingsAdded"), AntiAliasingClassPath));
    const TSharedPtr<FJsonObject> Previous = ChildObject(Edited.Result, TEXT("previous"));
    const TSharedPtr<FJsonObject> Preflight = ChildObject(Edited.Result, TEXT("preflight"));
    if (TestTrue(TEXT("previous and preflight are both returned"), Previous.IsValid() && Preflight.IsValid()))
    {
        TestFalse(TEXT("previous has no sampling block"), Previous->HasField(TEXT("sampling")));
        TestEqual(TEXT("previous width was 320"),
            static_cast<int32>(ChildObject(Previous, TEXT("resolution"))->GetNumberField(TEXT("width"))), 320);
        TestEqual(TEXT("new width is 640"),
            static_cast<int32>(ChildObject(Preflight, TEXT("resolution"))->GetNumberField(TEXT("width"))), 640);
        TestEqual(TEXT("new temporal count is 3"),
            static_cast<int32>(ChildObject(Preflight, TEXT("sampling"))->GetNumberField(TEXT("temporalSampleCount"))), 3);
    }
    const UMoviePipelineAntiAliasingSetting* Setting = Config->FindSetting<UMoviePipelineAntiAliasingSetting>();
    TestTrue(TEXT("the asset now carries the AA setting with the written count"),
        Setting && Setting->TemporalSampleCount == 3);
    return true;
}

// Every deferred pass derives from the deferred-pass base, so an IsA lookup treats a subclass as
// "already there" for its base. Both directions are pinned: a create naming both must add both,
// and an edit adding the base to a preset holding only the subclass must not be refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQPresetKeepsBaseBesideSubclassTest,
    "PinWright.mrq.create_preset.KeepsABaseClassBesideItsSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQPresetKeepsBaseBesideSubclassTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    if (!MovieRenderClassesLoaded() || !FindObject<UClass>(nullptr, DeferredBaseClassPath)
        || !FindObject<UClass>(nullptr, DeferredUnlitClassPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_render_pass_classes_absent"),
            TEXT("The deferred-pass classes are not loaded, so the base/subclass pair could not be "
                 "authored."));
        return true;
    }
    auto CountExact = [](const UMoviePipelinePrimaryConfig* Config, const TCHAR* ClassPath)
    {
        int32 Count = 0;
        for (const UMoviePipelineSetting* Setting : Config->GetUserSettings())
        {
            Count += (Setting && Setting->GetClass()->GetPathName() == ClassPath) ? 1 : 0;
        }
        return Count;
    };

    // Create: subclass listed BEFORE its base, the order in which an IsA lookup drops the base.
    const FFixturePath Path = NewFixturePath(TEXT("PW_MRQPresetPasses"));
    ON_SCOPE_EXIT { CleanupTestAsset(Path.PackageName); };
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    Payload->SetArrayField(TEXT("settings"),
        StringValues({ PngWriterClassPath, DeferredUnlitClassPath, DeferredBaseClassPath }));
    FTestResponseCapture Created;
    InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Payload, Created);
    if (!TestTrue(FString::Printf(TEXT("created ([%s] %s)"), *Created.ErrorCode, *Created.Message),
        Created.bSuccess && Created.Result.IsValid()))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Preflight = ChildObject(Created.Result, TEXT("preflight"));
    TestTrue(TEXT("preflight.settings lists the subclass"), ArrayHasString(Preflight, TEXT("settings"), DeferredUnlitClassPath));
    TestTrue(TEXT("preflight.settings lists the base too"), ArrayHasString(Preflight, TEXT("settings"), DeferredBaseClassPath));
    const TArray<TSharedPtr<FJsonValue>>* Added = nullptr;
    TestTrue(TEXT("settingsAdded has three distinct entries"),
        Created.Result->TryGetArrayField(TEXT("settingsAdded"), Added) && Added && Added->Num() == 3
        && ArrayHasString(Created.Result, TEXT("settingsAdded"), DeferredBaseClassPath));
    const UMoviePipelinePrimaryConfig* Config = FindObject<UMoviePipelinePrimaryConfig>(nullptr, *Path.ObjectPath);
    if (TestNotNull(TEXT("asset loaded"), Config))
    {
        TestEqual(TEXT("asset holds exactly one base pass"), CountExact(Config, DeferredBaseClassPath), 1);
        TestEqual(TEXT("asset holds exactly one unlit pass"), CountExact(Config, DeferredUnlitClassPath), 1);
    }

    // Edit: a preset holding only the subclass accepts the base in addSettings.
    const FFixturePath EditPath = NewFixturePath(TEXT("PW_MRQPresetPassEdit"));
    ON_SCOPE_EXIT { CleanupTestAsset(EditPath.PackageName); };
    TSharedPtr<FJsonObject> Base = MakeShared<FJsonObject>();
    Base->SetStringField(TEXT("assetPath"), EditPath.ObjectPath);
    Base->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath, DeferredUnlitClassPath }));
    FTestResponseCapture BaseCreated;
    InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Base, BaseCreated);
    if (!TestTrue(TEXT("edit fixture created"), BaseCreated.bSuccess))
    {
        return true;
    }
    TSharedPtr<FJsonObject> AddBase = MakeShared<FJsonObject>();
    AddBase->SetStringField(TEXT("assetPath"), EditPath.ObjectPath);
    AddBase->SetArrayField(TEXT("addSettings"), StringValues({ DeferredBaseClassPath }));
    FTestResponseCapture Edited;
    InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), AddBase, Edited);
    TestTrue(FString::Printf(TEXT("adding the base beside its subclass is accepted ([%s] %s)"),
        *Edited.ErrorCode, *Edited.Message), Edited.bSuccess);
    TestTrue(TEXT("and the base is now in preflight.settings"),
        ArrayHasString(ChildObject(Edited.Result, TEXT("preflight")), TEXT("settings"), DeferredBaseClassPath));
    return true;
}

// Every value of an edit is checked before the asset is touched: one invalid key in a call that
// also carries a valid one leaves the valid one unwritten. And a setting that exists but is
// disabled is named as such rather than reported missing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQSetPresetValidatesBeforeTouchingTest,
    "PinWright.mrq.set_preset_settings.ValidatesEverythingBeforeTouchingTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQSetPresetValidatesBeforeTouchingTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    if (!MovieRenderClassesLoaded())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_render_pass_classes_absent"),
            TEXT("The PNG writer or anti-aliasing setting class is not loaded, so no base preset "
                 "could be authored to edit."));
        return true;
    }
    bSuppressLogs = true;
    const FFixturePath Path = NewFixturePath(TEXT("PW_MRQPresetAtomic"));
    ON_SCOPE_EXIT { CleanupTestAsset(Path.PackageName); };
    TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
    Create->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    Create->SetArrayField(TEXT("settings"), StringValues({ PngWriterClassPath, AntiAliasingClassPath }));
    TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
    Output->SetNumberField(TEXT("width"), 320);
    Create->SetObjectField(TEXT("output"), Output);
    FTestResponseCapture Created;
    InvokeHandlerWithCapture(TEXT("mrq.create_preset"), Create, Created);
    UMoviePipelinePrimaryConfig* Config = FindObject<UMoviePipelinePrimaryConfig>(nullptr, *Path.ObjectPath);
    UMoviePipelineAntiAliasingSetting* Setting = Config ? Config->FindSetting<UMoviePipelineAntiAliasingSetting>() : nullptr;
    if (!TestTrue(TEXT("fixture preset with an AA setting created"), Created.bSuccess && Setting != nullptr))
    {
        return true;
    }
    const int32 SpatialBefore = Setting->SpatialSampleCount;

    TSharedPtr<FJsonObject> Edit = MakeShared<FJsonObject>();
    Edit->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    TSharedPtr<FJsonObject> NewOutput = MakeShared<FJsonObject>();
    NewOutput->SetNumberField(TEXT("width"), 640);
    Edit->SetObjectField(TEXT("output"), NewOutput);
    TSharedPtr<FJsonObject> BadSampling = MakeShared<FJsonObject>();
    BadSampling->SetNumberField(TEXT("spatialSampleCount"), 0);
    Edit->SetObjectField(TEXT("sampling"), BadSampling);
    FTestResponseCapture Refused;
    InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), Edit, Refused);
    TestEqual(TEXT("the invalid sampling value refuses the whole call"),
        Refused.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    const UMoviePipelineOutputSetting* OutputSetting = Config->FindSetting<UMoviePipelineOutputSetting>();
    TestTrue(TEXT("the valid width in the same call was NOT written (still 320)"),
        OutputSetting && OutputSetting->OutputResolution.X == 320);
    TestEqual(TEXT("the spatial count is unchanged"), Setting->SpatialSampleCount, SpatialBefore);

    // An empty addSettings is nothing to change, not a re-save of the unchanged asset.
    TSharedPtr<FJsonObject> EmptyAdd = MakeShared<FJsonObject>();
    EmptyAdd->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    EmptyAdd->SetArrayField(TEXT("addSettings"), TArray<TSharedPtr<FJsonValue>>());
    FTestResponseCapture EmptyRefused;
    InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), EmptyAdd, EmptyRefused);
    TestEqual(TEXT("addSettings: [] alone is refused as nothing to change"),
        EmptyRefused.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    // Disabled, not missing: the refusal says which.
    Setting->SetIsEnabled(false);
    TSharedPtr<FJsonObject> Sampling = MakeShared<FJsonObject>();
    Sampling->SetNumberField(TEXT("temporalSampleCount"), 4);
    TSharedPtr<FJsonObject> OnDisabled = MakeShared<FJsonObject>();
    OnDisabled->SetStringField(TEXT("assetPath"), Path.ObjectPath);
    OnDisabled->SetObjectField(TEXT("sampling"), Sampling);
    FTestResponseCapture DisabledRefused;
    InvokeHandlerWithCapture(TEXT("mrq.set_preset_settings"), OnDisabled, DisabledRefused);
    TestEqual(TEXT("sampling on a disabled AA setting is refused"),
        DisabledRefused.ErrorCode, FString(ErrorCodes::ERR_MRQ_SETTING_NOT_PRESENT));
    TestTrue(TEXT("and the message says the setting is disabled, not absent"),
        DisabledRefused.Message.Contains(TEXT("DISABLED")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQPresetBlockKeysAreReadbackVocabularyTest,
    "PinWright.mrq.create_preset.BlockKeysAreTheReadbackVocabulary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQPresetBlockKeysAreReadbackVocabularyTest::RunTest(const FString& Parameters)
{
    auto TableKeys = [](TConstArrayView<PinWrightMRQ::FSettingField> Fields)
    {
        TArray<FString> Keys;
        for (const PinWrightMRQ::FSettingField& Field : Fields)
        {
            Keys.Add(Field.Key);
        }
        Keys.Sort();
        return Keys;
    };
    const TArray<FString> EncoderKeys = TableKeys(PinWrightMRQ::EncoderSettingFields());
    const TArray<FString> SamplingKeys = TableKeys(PinWrightMRQ::SamplingSettingFields());

    int32 Checked = 0;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("mrq.create_preset") && Reg.MethodName != TEXT("mrq.set_preset_settings"))
        {
            continue;
        }
        for (const FParamSpec& Spec : Reg.Params)
        {
            const TArray<FString>* Expected = Spec.Name == TEXT("encoder") ? &EncoderKeys
                : Spec.Name == TEXT("sampling") ? &SamplingKeys : nullptr;
            if (!Expected)
            {
                continue;
            }
            TArray<FString> Declared = Spec.NestedKeys;
            Declared.Sort();
            TestTrue(FString::Printf(TEXT("%s:%s declares exactly the readback keys (declared [%s], readback [%s])"),
                    *Reg.MethodName, *Spec.Name, *FString::Join(Declared, TEXT(", ")),
                    *FString::Join(*Expected, TEXT(", "))),
                Declared == *Expected);
            ++Checked;
        }
    }
    TestEqual(TEXT("both verbs' encoder and sampling params were checked"), Checked, 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMRQCreateJobPreflightDisclosesSamplingTest,
    "PinWright.mrq.create_job.PreflightDisclosesSamplingAndSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMRQCreateJobPreflightDisclosesSamplingTest::RunTest(const FString& Parameters)
{
    using namespace TestMRQPresetHandlersHelpers;
    UMoviePipelineQueueSubsystem* QSS = GEditor
        ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
    UMoviePipelineQueue* Queue = QSS ? QSS->GetQueue() : nullptr;
    if (!Queue)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("mrq_queue_unavailable"),
            TEXT("UMoviePipelineQueueSubsystem or its queue is unavailable on this host, so the "
                 "create_job sampling disclosure was not exercised."));
        return true;
    }
    bSuppressLogs = true;
    const int32 InitialJobs = Queue->GetJobs().Num();
    ON_SCOPE_EXIT
    {
        while (Queue->GetJobs().Num() > InitialJobs)
        {
            const int32 Before = Queue->GetJobs().Num();
            Queue->DeleteJob(Queue->GetJobs().Last());
            if (Queue->GetJobs().Num() >= Before) { break; }
        }
    };

    // Two in-memory presets: one with an anti-aliasing setting carrying planted counts, one without.
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TArray<FString> PresetPackages;
    ON_SCOPE_EXIT { for (const FString& Package : PresetPackages) { CleanupTestAsset(Package); } };
    auto MakePreset = [&PresetPackages, &Stamp](const TCHAR* Suffix, bool bWithAntiAliasing) -> UMoviePipelinePrimaryConfig*
    {
        const FString Name = FString::Printf(TEXT("PW_SamplingPreset%s_%s"), Suffix, *Stamp);
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *Name);
        PresetPackages.Add(PackageName);
        UPackage* Package = CreatePackage(*PackageName);
        UMoviePipelinePrimaryConfig* Preset = Package ? NewObject<UMoviePipelinePrimaryConfig>(Package,
            UMoviePipelinePrimaryConfig::StaticClass(), *Name, RF_Public | RF_Standalone | RF_Transient) : nullptr;
        if (Preset && bWithAntiAliasing)
        {
            if (UMoviePipelineAntiAliasingSetting* Setting = Cast<UMoviePipelineAntiAliasingSetting>(
                Preset->FindOrAddSettingByClass(UMoviePipelineAntiAliasingSetting::StaticClass())))
            {
                Setting->SpatialSampleCount = 3;
                Setting->TemporalSampleCount = 9;
                Setting->EngineWarmUpCount = 123;
            }
        }
        return Preset;
    };
    TStrongObjectPtr<UMoviePipelinePrimaryConfig> WithAA(MakePreset(TEXT("AA"), true));
    TStrongObjectPtr<UMoviePipelinePrimaryConfig> WithoutAA(MakePreset(TEXT("NoAA"), false));
    if (!TestTrue(TEXT("preset fixtures created"), WithAA.IsValid() && WithoutAA.IsValid())
        || !TestNotNull(TEXT("precondition: the AA fixture carries the setting"),
            WithAA->FindSetting<UMoviePipelineAntiAliasingSetting>()))
    {
        return true;
    }

    // create_job needs a real sequence and map; in-memory fixtures, as the sibling preflight test.
    UPackage* SequencePackage = CreatePackage(*FString::Printf(TEXT("/Temp/PinWrightTests/Sampling_%s_Sequence"), *Stamp));
    ULevelSequence* Sequence = NewObject<ULevelSequence>(SequencePackage, ULevelSequence::StaticClass(),
        *FString::Printf(TEXT("Sequence_%s"), *Stamp), RF_Public | RF_Standalone | RF_Transient);
    Sequence->Initialize();
    TStrongObjectPtr<ULevelSequence> SequenceGuard(Sequence);
    UPackage* MapPackage = CreatePackage(*FString::Printf(TEXT("/Temp/PinWrightTests/Sampling_%s_Map"), *Stamp));
    UWorld* Map = UWorld::CreateWorld(EWorldType::Editor, false, FName(*FString::Printf(TEXT("Map_%s"), *Stamp)), MapPackage);
    FScopedTransientWorldGuard MapGuard(Map);
    if (!TestNotNull(TEXT("transient map created"), Map))
    {
        return true;
    }

    auto QueueWithPreset = [&](const UMoviePipelinePrimaryConfig* Preset) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sequencePath"), Sequence->GetPathName());
        Payload->SetStringField(TEXT("levelPath"), Map->GetPathName());
        Payload->SetStringField(TEXT("presetPath"), Preset->GetPathName());
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("mrq.create_job"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("create_job queued %s ([%s] %s)"), *Preset->GetName(),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        return ChildObject(Capture.Result, TEXT("preflight"));
    };

    const TSharedPtr<FJsonObject> PreflightWith = QueueWithPreset(WithAA.Get());
    const TSharedPtr<FJsonObject> Sampling = ChildObject(PreflightWith, TEXT("sampling"));
    if (TestTrue(TEXT("preflight.sampling is published for a preset with the setting"), Sampling.IsValid()))
    {
        TestEqual(TEXT("spatial read off the queued config"), static_cast<int32>(Sampling->GetNumberField(TEXT("spatialSampleCount"))), 3);
        TestEqual(TEXT("temporal read off the queued config"), static_cast<int32>(Sampling->GetNumberField(TEXT("temporalSampleCount"))), 9);
        TestEqual(TEXT("engine warm-up read off the queued config"), static_cast<int32>(Sampling->GetNumberField(TEXT("engineWarmUpCount"))), 123);
    }
    TestTrue(TEXT("preflight.settings lists the anti-aliasing setting"),
        ArrayHasString(PreflightWith, TEXT("settings"), AntiAliasingClassPath));
    TestTrue(TEXT("preflight.settings lists the output setting, which `outputs` never did"),
        ArrayHasString(PreflightWith, TEXT("settings"), UMoviePipelineOutputSetting::StaticClass()->GetPathName()));

    const TSharedPtr<FJsonObject> PreflightWithout = QueueWithPreset(WithoutAA.Get());
    TestTrue(TEXT("a preset without the setting still returns a preflight"), PreflightWithout.IsValid());
    TestFalse(TEXT("and OMITS sampling rather than publishing defaults"),
        PreflightWithout.IsValid() && PreflightWithout->HasField(TEXT("sampling")));
    return true;
}

#endif // PINWRIGHT_TEST_HAS_MRQ_PRESETS
