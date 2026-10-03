// Copyright (c) 2026 Alexander Penkin. MIT License.

// Movie Render Queue preset AUTHORING: mrq.create_preset and mrq.set_preset_settings.
//
// WHY. The only lever on an MRQ render is a UMoviePipelinePrimaryConfig asset, and until these
// verbs the namespace could enumerate one (mrq.list_presets) and consume one (mrq.create_job) but
// not make one — so a project with no preset asset rendered at the engine's CDO defaults or
// dropped to python.execute (F-mrq-preset-authoring).
//
// EXPLICIT, NOT IMPLIED. The namespace's standing rule is that PinWright never applies a setting
// nobody asked for (no bitrate floor rewritten into a caller's config). These verbs are that rule's
// other half: the caller states every value, the verb writes exactly that, and a value it cannot
// write as given is REFUSED — an out-of-range count is not clamped, an unknown enumerator is not
// defaulted, and a block whose setting the preset lacks is not silently given one by
// mrq.set_preset_settings. Every check runs before the first mutation, so a refused call leaves
// the asset (or the absence of one) exactly as it was.
//
// ONE VOCABULARY. The `encoder` and `sampling` keys are PinWrightMRQ::EncoderSettingFields() /
// SamplingSettingFields() — the same tables the `encoderRequested` / `sampling` readback uses — and
// the response's `preflight` block is built by the same PinWrightMRQ::ReadPreflightContext that
// mrq.create_job uses. What these verbs wrote is stated in the words create_job will use.
#include "CoreMinimal.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Compat/JsonKeyCompat.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetSaveState.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"

// The first four headers are MRQHandler.cpp's MCP_HAS_MRQ gate, which defines the PinWrightMRQ
// readers this file calls; requiring them here keeps the two gates equal.
#if __has_include("MoviePipelineQueueSubsystem.h") && \
    __has_include("MoviePipelinePIEExecutor.h") && \
    __has_include("MoviePipelineQueue.h") && \
    __has_include("MoviePipelinePrimaryConfig.h") && \
    __has_include("MoviePipelineOutputSetting.h") && \
    __has_include("MoviePipelineVideoOutputBase.h") && \
    __has_include("MoviePipelineAntiAliasingSetting.h")
    #include "AssetRegistry/AssetRegistryModule.h"
    #include "MoviePipelinePrimaryConfig.h"
    #include "MoviePipelineOutputBase.h"
    #include "MoviePipelineOutputSetting.h"
    #include "MoviePipelineVideoOutputBase.h"
    #include "MoviePipelineAntiAliasingSetting.h"
    #include "Handlers/MRQ/MRQConfigDisclosure.h"
    #define PINWRIGHT_HAS_MRQ_PRESET_AUTHORING 1
#else
    #define PINWRIGHT_HAS_MRQ_PRESET_AUTHORING 0
#endif

#if PINWRIGHT_HAS_MRQ_PRESET_AUTHORING
namespace PinWrightMRQPresetLocal
{
    struct FRefusal
    {
        FString Code;
        FString Message;
    };

    // One validated reflection write, applied only after every check in the call has passed.
    struct FFieldWrite
    {
        FString Key;
        const FProperty* Property = nullptr;
        TSharedPtr<FJsonValue> Value;
        int64 EnumValue = 0;
    };

    // Everything a call will do, decided before anything is done.
    struct FPresetPlan
    {
        TArray<UClass*> AddClasses;
        UClass* VideoOutputClass = nullptr;
        bool bAddAntiAliasing = false;
        TArray<FFieldWrite> EncoderWrites;
        TArray<FFieldWrite> SamplingWrites;
        bool bEngineWarmUpRequested = false;

        TOptional<int32> Width;
        TOptional<int32> Height;
        TOptional<FString> OutputDirectory;
        TOptional<FString> FileNameFormat;
        TOptional<FFrameRate> FrameRate;
    };

    const UEnum* PropertyEnum(const FProperty* Property)
    {
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            return EnumProperty->GetEnum();
        }
        if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            return ByteProperty->Enum;
        }
        return nullptr;
    }

    // The enumerators a caller may name: the short names the readback publishes, minus the
    // generated _MAX entry.
    TArray<FString> EnumNames(const UEnum* Enum)
    {
        TArray<FString> Names;
        const int32 Count = Enum->ContainsExistingMax() ? Enum->NumEnums() - 1 : Enum->NumEnums();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Names.Add(Enum->GetNameStringByIndex(Index));
        }
        return Names;
    }

    bool ReadInteger(const TSharedPtr<FJsonValue>& Value, int64 Min, int64 Max, int64& Out)
    {
        double Number = 0.0;
        if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number)
            || !FMath::IsFinite(Number) || Number != FMath::RoundToDouble(Number)
            || Number < static_cast<double>(Min) || Number > static_cast<double>(Max))
        {
            return false;
        }
        Out = static_cast<int64>(Number);
        return true;
    }

    // The engine's own ClampMin/ClampMax metadata: a value the editor would clamp is refused here
    // instead, because a clamped write is a write of something the caller did not say.
    bool WithinPropertyClamp(const FProperty* Property, double Number, FString& OutBound)
    {
#if WITH_EDITORONLY_DATA
        if (Property->HasMetaData(TEXT("ClampMin"))
            && Number < FCString::Atod(*Property->GetMetaData(TEXT("ClampMin"))))
        {
            OutBound = FString::Printf(TEXT(">= %s"), *Property->GetMetaData(TEXT("ClampMin")));
            return false;
        }
        if (Property->HasMetaData(TEXT("ClampMax"))
            && Number > FCString::Atod(*Property->GetMetaData(TEXT("ClampMax"))))
        {
            OutBound = FString::Printf(TEXT("<= %s"), *Property->GetMetaData(TEXT("ClampMax")));
            return false;
        }
#endif
        return true;
    }

    // Validate one `encoder` / `sampling` block against the class it will be written to. Pure:
    // reads the class's reflection data only.
    bool PlanFieldBlock(const TSharedPtr<FJsonObject>& Block, const TCHAR* BlockName,
        TConstArrayView<PinWrightMRQ::FSettingField> Fields, const UClass* TargetClass,
        TArray<FFieldWrite>& OutWrites, FRefusal& OutRefusal)
    {
        for (const auto& Pair : Block->Values)
        {
            const FString PairKey = EARGCompat::JsonKeyToString(Pair.Key);
            const PinWrightMRQ::FSettingField* Field = Fields.FindByPredicate(
                [&PairKey](const PinWrightMRQ::FSettingField& Candidate)
                {
                    return PairKey.Equals(Candidate.Key, ESearchCase::CaseSensitive);
                });
            if (!Field)
            {
                continue; // unreachable: the dispatcher's nested-key gate refuses unknown keys
            }
            const FProperty* Property = TargetClass->FindPropertyByName(Field->Property);
            if (!Property)
            {
                OutRefusal = { ErrorCodes::ERR_PROPERTY_NOT_FOUND, FString::Printf(
                    TEXT("%s.%s maps to property '%s', which %s does not carry, so it cannot be ")
                    TEXT("written on this setting class."),
                    BlockName, Field->Key, Field->Property, *TargetClass->GetPathName()) };
                return false;
            }

            FFieldWrite Write;
            Write.Key = Field->Key;
            Write.Property = Property;
            Write.Value = Pair.Value;
            const TSharedPtr<FJsonValue>& Value = Pair.Value;
            FString Bound;
            double Number = 0.0;
            int64 Integer = 0;
            if (CastField<FBoolProperty>(Property))
            {
                if (!Value.IsValid() || Value->Type != EJson::Boolean)
                {
                    OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(TEXT("%s.%s must be a boolean."), BlockName, Field->Key) };
                    return false;
                }
            }
            else if (const UEnum* Enum = PropertyEnum(Property))
            {
                FString Name;
                const TArray<FString> Names = EnumNames(Enum);
                const int32 Index = (Value.IsValid() && Value->TryGetString(Name))
                    ? Names.IndexOfByPredicate([&Name](const FString& Candidate)
                        { return Candidate.Equals(Name, ESearchCase::IgnoreCase); })
                    : INDEX_NONE;
                if (Index == INDEX_NONE)
                {
                    OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                        TEXT("%s.%s must name one of: %s."), BlockName, Field->Key,
                        *FString::Join(Names, TEXT(", "))) };
                    return false;
                }
                Write.EnumValue = Enum->GetValueByIndex(Index);
            }
            else if (CastField<FIntProperty>(Property))
            {
                if (!ReadInteger(Value, MIN_int32, MAX_int32, Integer))
                {
                    OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(TEXT("%s.%s must be an integer."), BlockName, Field->Key) };
                    return false;
                }
                if (!WithinPropertyClamp(Property, static_cast<double>(Integer), Bound))
                {
                    OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                        TEXT("%s.%s = %lld is outside the engine's range for %s (%s); it is ")
                        TEXT("refused rather than clamped."),
                        BlockName, Field->Key, Integer, Field->Property, *Bound) };
                    return false;
                }
            }
            else if (CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property))
            {
                if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number)
                    || !FMath::IsFinite(Number))
                {
                    OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(TEXT("%s.%s must be a number."), BlockName, Field->Key) };
                    return false;
                }
                if (!WithinPropertyClamp(Property, Number, Bound))
                {
                    OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                        TEXT("%s.%s = %g is outside the engine's range for %s (%s); it is ")
                        TEXT("refused rather than clamped."),
                        BlockName, Field->Key, Number, Field->Property, *Bound) };
                    return false;
                }
            }
            else
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("%s.%s maps to %s.%s, whose type this verb does not write."),
                    BlockName, Field->Key, *TargetClass->GetName(), Field->Property) };
                return false;
            }
            OutWrites.Add(MoveTemp(Write));
        }
        return true;
    }

    void ApplyFieldWrites(UObject* Target, const TArray<FFieldWrite>& Writes)
    {
        for (const FFieldWrite& Write : Writes)
        {
            void* ValuePtr = Write.Property->ContainerPtrToValuePtr<void>(Target);
            double Number = 0.0;
            if (const FBoolProperty* Bool = CastField<FBoolProperty>(Write.Property))
            {
                Bool->SetPropertyValue(ValuePtr, Write.Value->AsBool());
            }
            else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Write.Property))
            {
                EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Write.EnumValue);
            }
            else if (const FByteProperty* Byte = CastField<FByteProperty>(Write.Property))
            {
                Byte->SetPropertyValue(ValuePtr, static_cast<uint8>(Write.EnumValue));
            }
            else if (const FIntProperty* Int = CastField<FIntProperty>(Write.Property))
            {
                Write.Value->TryGetNumber(Number);
                Int->SetPropertyValue(ValuePtr, static_cast<int32>(Number));
            }
            else if (const FFloatProperty* Float = CastField<FFloatProperty>(Write.Property))
            {
                Write.Value->TryGetNumber(Number);
                Float->SetPropertyValue(ValuePtr, static_cast<float>(Number));
            }
            else if (const FDoubleProperty* Double = CastField<FDoubleProperty>(Write.Property))
            {
                Write.Value->TryGetNumber(Number);
                Double->SetPropertyValue(ValuePtr, Number);
            }
        }
    }

    // `settings` / `addSettings`: concrete, primary-config-legal setting classes, no duplicates.
    bool ResolveSettingClasses(const TArray<TSharedPtr<FJsonValue>>& Values, const TCHAR* ParamName,
        TArray<UClass*>& OutClasses, FRefusal& OutRefusal)
    {
        // Through the base: UMoviePipelinePrimaryConfig re-declares CanSettingBeAdded protected,
        // UMoviePipelineConfigBase declares it public, and access follows the static type.
        const UMoviePipelineConfigBase* PrimaryDefaults = GetDefault<UMoviePipelinePrimaryConfig>();
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            FString ClassRef;
            if (!Values[Index].IsValid() || !Values[Index]->TryGetString(ClassRef) || ClassRef.IsEmpty())
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("%s[%d] must be a setting class path string."), ParamName, Index) };
                return false;
            }
            UClass* Class = ResolveUClass(ClassRef);
            if (!Class)
            {
                OutRefusal = { ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(
                    TEXT("%s[%d] '%s' did not resolve to a class (is its plugin enabled?)."),
                    ParamName, Index, *ClassRef) };
                return false;
            }
            const UMoviePipelineSetting* SettingDefaults = Cast<UMoviePipelineSetting>(
                Class->GetDefaultObject());
            if (!Class->IsChildOf(UMoviePipelineSetting::StaticClass())
                || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) || !SettingDefaults)
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_CLASS, FString::Printf(
                    TEXT("%s[%d] '%s' is not a concrete UMoviePipelineSetting subclass."),
                    ParamName, Index, *Class->GetPathName()) };
                return false;
            }
            if (Class->IsChildOf(UMoviePipelineOutputSetting::StaticClass()))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("%s[%d] names the output setting, which every primary config already ")
                    TEXT("owns; configure it through `output`."), ParamName, Index) };
                return false;
            }
            if (!PrimaryDefaults->CanSettingBeAdded(SettingDefaults))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_CLASS, FString::Printf(
                    TEXT("%s[%d] '%s' is a per-shot setting that a primary config does not ")
                    TEXT("accept."), ParamName, Index, *Class->GetPathName()) };
                return false;
            }
            if (OutClasses.Contains(Class))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("%s lists '%s' twice."), ParamName, *Class->GetPathName()) };
                return false;
            }
            OutClasses.Add(Class);
        }
        return true;
    }

    bool PlanOutputBlock(const TSharedPtr<FJsonObject>& Output, FPresetPlan& Plan, FRefusal& OutRefusal)
    {
        // Unknown keys never reach here: the dispatcher's nested-key gate refuses them.
        int64 Integer = 0;
        const TPair<const TCHAR*, TOptional<int32>*> Dimensions[] = {
            { TEXT("width"), &Plan.Width }, { TEXT("height"), &Plan.Height } };
        for (const TPair<const TCHAR*, TOptional<int32>*>& Dimension : Dimensions)
        {
            if (!Output->HasField(Dimension.Key))
            {
                continue;
            }
            if (!ReadInteger(Output->TryGetField(Dimension.Key), 1, 32768, Integer))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("output.%s must be an integer from 1 to 32768."), Dimension.Key) };
                return false;
            }
            *Dimension.Value = static_cast<int32>(Integer);
        }

        FString Text;
        if (Output->HasField(TEXT("outputDirectory")))
        {
            FString PathError;
            if (!Output->TryGetStringField(TEXT("outputDirectory"), Text)
                || !PinWrightMRQ::ValidateOutputDirectory(Text, PathError))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_PATH, PathError.IsEmpty()
                    ? FString(TEXT("output.outputDirectory must be a string.")) : PathError };
                return false;
            }
            Plan.OutputDirectory = Text;
        }
        if (Output->HasField(TEXT("fileNameFormat")))
        {
            if (!Output->TryGetStringField(TEXT("fileNameFormat"), Text) || Text.TrimStartAndEnd().IsEmpty())
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("output.fileNameFormat must be a non-empty string.") };
                return false;
            }
            Plan.FileNameFormat = Text;
        }

        int64 Denominator = 1;
        if (Output->HasField(TEXT("frameRateDenominator")))
        {
            if (!Output->HasField(TEXT("frameRate"))
                || !ReadInteger(Output->TryGetField(TEXT("frameRateDenominator")), 1, MAX_int32, Denominator))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("output.frameRateDenominator must be a positive integer and needs output.frameRate.") };
                return false;
            }
        }
        if (Output->HasField(TEXT("frameRate")))
        {
            if (!ReadInteger(Output->TryGetField(TEXT("frameRate")), 1, MAX_int32, Integer))
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
                    "output.frameRate must be a positive integer frame-rate numerator (30000 with "
                    "frameRateDenominator 1001 for 29.97).") };
                return false;
            }
            Plan.FrameRate = FFrameRate(static_cast<uint32>(Integer), static_cast<uint32>(Denominator));
        }
        return true;
    }

    // `sampling.antiAliasingMethod` only takes effect with `bOverrideAntiAliasing` set (its
    // EditCondition), so naming a method implies the override; contradicting it is refused rather
    // than written as an accepted-and-ignored value.
    bool AddImpliedAntiAliasingOverride(const TSharedPtr<FJsonObject>& Sampling, FPresetPlan& Plan,
        FRefusal& OutRefusal)
    {
        if (!Sampling->HasField(TEXT("antiAliasingMethod")))
        {
            return true;
        }
        bool bOverride = true;
        if (Sampling->TryGetBoolField(TEXT("overrideAntiAliasing"), bOverride))
        {
            if (!bOverride)
            {
                OutRefusal = { ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
                    "sampling.antiAliasingMethod has no effect with sampling.overrideAntiAliasing "
                    "false; drop one of the two.") };
                return false;
            }
            return true;
        }
        FFieldWrite Override;
        Override.Key = TEXT("overrideAntiAliasing");
        Override.Property = UMoviePipelineAntiAliasingSetting::StaticClass()->FindPropertyByName(
            TEXT("bOverrideAntiAliasing"));
        Override.Value = MakeShared<FJsonValueBoolean>(true);
        if (Override.Property)
        {
            Plan.SamplingWrites.Add(MoveTemp(Override));
        }
        return true;
    }

    // Shared by both verbs: plan the three value blocks against the classes that will be present.
    bool PlanValueBlocks(const FHandlerContext& Ctx, const UClass* VideoOutputClass,
        FPresetPlan& Plan, FRefusal& OutRefusal)
    {
        const TSharedPtr<FJsonObject>* Output = nullptr;
        const TSharedPtr<FJsonObject>* Encoder = nullptr;
        const TSharedPtr<FJsonObject>* Sampling = nullptr;
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (Payload->TryGetObjectField(TEXT("output"), Output) && Output && Output->IsValid()
            && !PlanOutputBlock(*Output, Plan, OutRefusal))
        {
            return false;
        }
        if (Payload->TryGetObjectField(TEXT("encoder"), Encoder) && Encoder && Encoder->IsValid()
            && !PlanFieldBlock(*Encoder, TEXT("encoder"), PinWrightMRQ::EncoderSettingFields(),
                VideoOutputClass, Plan.EncoderWrites, OutRefusal))
        {
            return false;
        }
        if (Payload->TryGetObjectField(TEXT("sampling"), Sampling) && Sampling && Sampling->IsValid())
        {
            if (!PlanFieldBlock(*Sampling, TEXT("sampling"), PinWrightMRQ::SamplingSettingFields(),
                    UMoviePipelineAntiAliasingSetting::StaticClass(), Plan.SamplingWrites, OutRefusal)
                || !AddImpliedAntiAliasingOverride(*Sampling, Plan, OutRefusal))
            {
                return false;
            }
            Plan.bEngineWarmUpRequested = (*Sampling)->HasField(TEXT("engineWarmUpCount"));
        }
        return true;
    }

    bool HasBlock(const FHandlerContext& Ctx, const TCHAR* Name)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        return Ctx.GetRawPayload()->TryGetObjectField(Name, Block) && Block && Block->IsValid();
    }

    // Apply a fully validated plan. Every refusal has already happened.
    void ApplyPlan(UMoviePipelinePrimaryConfig* Config, const FPresetPlan& Plan,
        TArray<FString>& OutAddedClassPaths)
    {
        Config->Modify();
        for (UClass* Class : Plan.AddClasses)
        {
            // Exact class, not IsA: every deferred pass derives from the deferred-pass base, so an
            // IsA lookup would hand back an existing sibling and silently drop the requested one.
            if (UMoviePipelineSetting* Added = Config->FindOrAddSettingByClass(
                Class, /*bIncludeDisabledSettings=*/false, /*bExactMatch=*/true))
            {
                OutAddedClassPaths.Add(Added->GetClass()->GetPathName());
            }
        }
        if (Plan.bAddAntiAliasing)
        {
            if (UMoviePipelineSetting* Added =
                Config->FindOrAddSettingByClass(UMoviePipelineAntiAliasingSetting::StaticClass(),
                    /*bIncludeDisabledSettings=*/false, /*bExactMatch=*/true))
            {
                OutAddedClassPaths.Add(Added->GetClass()->GetPathName());
            }
        }

        if (UMoviePipelineOutputSetting* OutputSetting = Config->FindSetting<UMoviePipelineOutputSetting>())
        {
            OutputSetting->Modify();
            if (Plan.Width.IsSet()) { OutputSetting->OutputResolution.X = Plan.Width.GetValue(); }
            if (Plan.Height.IsSet()) { OutputSetting->OutputResolution.Y = Plan.Height.GetValue(); }
            if (Plan.OutputDirectory.IsSet()) { OutputSetting->OutputDirectory.Path = Plan.OutputDirectory.GetValue(); }
            if (Plan.FileNameFormat.IsSet()) { OutputSetting->FileNameFormat = Plan.FileNameFormat.GetValue(); }
            if (Plan.FrameRate.IsSet())
            {
                OutputSetting->bUseCustomFrameRate = true;
                OutputSetting->OutputFrameRate = Plan.FrameRate.GetValue();
            }
        }
        if (Plan.EncoderWrites.Num() > 0)
        {
            if (UMoviePipelineSetting* Video = Config->FindSettingByClass(
                Plan.VideoOutputClass, /*bIncludeDisabledSettings=*/false, /*bExactMatch=*/true))
            {
                Video->Modify();
                ApplyFieldWrites(Video, Plan.EncoderWrites);
            }
        }
        if (Plan.SamplingWrites.Num() > 0)
        {
            if (UMoviePipelineSetting* AntiAliasing = Config->FindSettingByClass(
                UMoviePipelineAntiAliasingSetting::StaticClass(), /*bIncludeDisabledSettings=*/false,
                /*bExactMatch=*/true))
            {
                AntiAliasing->Modify();
                ApplyFieldWrites(AntiAliasing, Plan.SamplingWrites);
            }
        }
        Config->MarkPackageDirty();
    }

    // Save, read back, respond. The `preflight` block is read off the config AFTER the save, by
    // the same reader mrq.create_job uses, so it is a description of what is in the asset rather
    // than an echo of the request.
    void SaveAndRespond(FHandlerContext& Ctx, UMoviePipelinePrimaryConfig* Config,
        const FPresetPlan& Plan, const TArray<FString>& AddedClassPaths, bool bCreated,
        const TSharedPtr<FJsonObject>& Previous)
    {
        EAssetSaveState SaveState = EAssetSaveState::Failed;
        int64 SizeBytes = 0;
        const bool bSaved = SaveAssetToDiskReportingPresence(
            Config, /*bForce=*/true, nullptr, &SizeBytes, &SaveState);

        TArray<FString> Warnings;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), Config->GetPathName());
        Result->SetBoolField(TEXT("created"), bCreated);
        TArray<TSharedPtr<FJsonValue>> Added;
        for (const FString& ClassPath : AddedClassPaths)
        {
            Added.Add(MakeShared<FJsonValueString>(ClassPath));
        }
        Result->SetArrayField(TEXT("settingsAdded"), Added);
        Result->SetObjectField(TEXT("preflight"), PinWrightMRQ::BuildPreflightReport(
            PinWrightMRQ::ReadPreflightContext(Config), Warnings));
        if (Previous.IsValid())
        {
            Result->SetObjectField(TEXT("previous"), Previous);
        }
        AddAssetSaveReport(Result, /*bSaveRequested=*/true, bSaved, SaveState);
        AddAssetSaveSizeReport(Result, SizeBytes, bSaved);

        const UMoviePipelineAntiAliasingSetting* AntiAliasing =
            Config->FindSetting<UMoviePipelineAntiAliasingSetting>();
        if (Plan.bEngineWarmUpRequested && AntiAliasing && AntiAliasing->bUseCameraCutForWarmUp)
        {
            Warnings.Add(TEXT("sampling.engineWarmUpCount was written, but the preset has ")
                TEXT("useCameraCutForWarmUp true, so MRQ warms up from the camera cut and ignores ")
                TEXT("that count. Set sampling.useCameraCutForWarmUp false for the count to apply."));
        }
        if (Warnings.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> WarningValues;
            for (const FString& Warning : Warnings)
            {
                WarningValues.Add(MakeShared<FJsonValueString>(Warning));
            }
            Result->SetArrayField(TEXT("warnings"), WarningValues);
        }

        if (!bSaved)
        {
            Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED, FString::Printf(
                TEXT("The preset was %s in memory, but its save was not durable (saveState=%s); ")
                TEXT("the .uasset on disk does not hold these settings."),
                bCreated ? TEXT("created") : TEXT("edited"), AssetSaveStateToWire(SaveState)), Result);
            return;
        }
        Ctx.SendSuccess(Result);
    }

    bool RefuseIfPieBlocksSave(FHandlerContext& Ctx)
    {
        TSharedPtr<FJsonObject> Refusal = MakeShared<FJsonObject>();
        FString Description;
        if (AddPieSaveRefusalReport(Refusal, Description))
        {
            Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE, FString::Printf(
                TEXT("A play session (%s) refuses every asset save, so nothing was written; end ")
                TEXT("it and retry."), *Description), Refusal);
            return true;
        }
        return false;
    }

    UClass* FirstVideoOutputClass(const TArray<UClass*>& Classes)
    {
        for (UClass* Class : Classes)
        {
            if (Class->IsChildOf(UMoviePipelineVideoOutputBase::StaticClass()))
            {
                return Class;
            }
        }
        return nullptr;
    }
}
#endif // PINWRIGHT_HAS_MRQ_PRESET_AUTHORING

// ---- mrq.create_preset ----
REGISTER_RPC_HANDLER("mrq.create_preset", "mrq",
    "Create and save a new UMoviePipelinePrimaryConfig preset asset holding exactly the settings "
    "named, for mrq.create_job's presetPath. Every value is validated before anything is created: "
    "an unknown class, a per-shot-only setting, an out-of-range count (refused, never clamped), an "
    "unknown enumerator or an `encoder` block with no video output among `settings` is refused and "
    "no asset is made. Refuses ASSET_ALREADY_EXISTS for an occupied path (edit an existing preset "
    "with mrq.set_preset_settings). The response's `preflight` block is read off the asset after the save by "
    "the same reader mrq.create_job uses (`settings`, `outputs`, `sampling`, `encoderRequested`), "
    "plus the save report (`saved`, `saveState`); a save that is not durable is SAVE_FAILED with the "
    "same payload.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Preset asset path to create, e.g. /Game/Cine/MPC_Shot01"),
        RPC_PARAM_REQ("settings", "array", "Setting class paths to add, e.g. /Script/MovieRenderPipelineRenderPasses.MoviePipelineDeferredPassBase, /Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_PNG, /Script/MovieRenderPipelineMP4Encoder.MoviePipelineMP4EncoderOutput. Must include at least one file writer (a UMoviePipelineOutputBase subclass); the output setting is always present and is configured through `output`"),
        RPC_PARAM_OPT_NESTED("output", "object", "Output setting values: { width, height, outputDirectory, fileNameFormat, frameRate, frameRateDenominator }. frameRate (integer numerator; frameRateDenominator defaults to 1) turns the custom frame-rate override on. outputDirectory gets the same checks mrq.create_job applies. These keys are the whole schema; any other key is refused with UNKNOWN_NESTED_PARAMS",
            TEXT("width"), TEXT("height"), TEXT("outputDirectory"), TEXT("fileNameFormat"),
            TEXT("frameRate"), TEXT("frameRateDenominator")),
        RPC_PARAM_OPT_NESTED("encoder", "object", "Video output values, the encoderRequested vocabulary: { rateControl, constantRateFactor, averageBitrateMbps, maxBitrateMbps, includeAudio }. Written to the first video output in `settings`, which must exist. includeAudio:false drops the audio track the MP4 writer adds by default. These keys are the whole schema; any other key is refused with UNKNOWN_NESTED_PARAMS",
            TEXT("rateControl"), TEXT("constantRateFactor"), TEXT("averageBitrateMbps"),
            TEXT("maxBitrateMbps"), TEXT("includeAudio")),
        RPC_PARAM_OPT_NESTED("sampling", "object", "Anti-aliasing setting values, the preflight.sampling vocabulary: { spatialSampleCount, temporalSampleCount, engineWarmUpCount, renderWarmUpCount, useCameraCutForWarmUp, renderWarmUpFrames, overrideAntiAliasing, antiAliasingMethod }. Adds the anti-aliasing setting if `settings` does not list it. antiAliasingMethod (e.g. AAM_TSR, AAM_None) implies overrideAntiAliasing:true. These keys are the whole schema; any other key is refused with UNKNOWN_NESTED_PARAMS",
            TEXT("spatialSampleCount"), TEXT("temporalSampleCount"), TEXT("engineWarmUpCount"),
            TEXT("renderWarmUpCount"), TEXT("useCameraCutForWarmUp"), TEXT("renderWarmUpFrames"),
            TEXT("overrideAntiAliasing"), TEXT("antiAliasingMethod"))
    ))
{
#if PINWRIGHT_HAS_MRQ_PRESET_AUTHORING
    using namespace PinWrightMRQPresetLocal;
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) { return true; }
    const TArray<TSharedPtr<FJsonValue>>* SettingValues = nullptr;
    if (!Ctx.RequireArray(TEXT("settings"), SettingValues) || !SettingValues) { return true; }

    // --- path: a new package on a mounted root, named after its asset ---
    AssetPath.TrimStartAndEndInline();
    FString PackagePathIn = AssetPath;
    FString ObjectNameIn;
    AssetPath.Split(TEXT("."), &PackagePathIn, &ObjectNameIn);
    FString PackageName;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(FPackageName::GetLongPackagePath(PackagePathIn),
            FPackageName::GetShortName(PackagePathIn), PackageName, PathError)
        || FPackageName::GetPackageMountPoint(PackageName).IsNone())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError.IsEmpty()
            ? FString::Printf(TEXT("assetPath '%s' is not on a mounted content root."), *AssetPath)
            : PathError);
        return true;
    }
    const FString AssetName = FPackageName::GetShortName(PackageName);
    if (!ObjectNameIn.IsEmpty() && ObjectNameIn != AssetName)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, FString::Printf(
            TEXT("assetPath '%s' names object '%s' in package '%s'; a preset asset is named after ")
            TEXT("its package. Use '%s.%s'."), *AssetPath, *ObjectNameIn, *PackageName,
            *PackageName, *AssetName));
        return true;
    }
    const FString ObjectPath = PackageName + TEXT(".") + AssetName;
    if (FindObject<UObject>(nullptr, *ObjectPath) || FindPackage(nullptr, *PackageName)
        || FPackageName::DoesPackageExist(PackageName))
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_ALREADY_EXISTS, FString::Printf(
            TEXT("'%s' already exists. mrq.create_preset never overwrites; use ")
            TEXT("mrq.set_preset_settings to edit it, or pick a new assetPath."), *ObjectPath));
        return true;
    }

    // --- plan: every class and value checked before anything exists ---
    FPresetPlan Plan;
    FRefusal Refusal;
    if (!ResolveSettingClasses(*SettingValues, TEXT("settings"), Plan.AddClasses, Refusal))
    {
        Ctx.SendError(Refusal.Code, Refusal.Message);
        return true;
    }
    if (!Plan.AddClasses.ContainsByPredicate([](const UClass* Class)
        { return Class->IsChildOf(UMoviePipelineOutputBase::StaticClass()); }))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
            "`settings` names no file writer (a UMoviePipelineOutputBase subclass such as "
            "MoviePipelineImageSequenceOutput_PNG or MoviePipelineMP4EncoderOutput), so a render "
            "with this preset would write nothing at all."));
        return true;
    }
    Plan.VideoOutputClass = FirstVideoOutputClass(Plan.AddClasses);
    if (HasBlock(Ctx, TEXT("encoder")) && !Plan.VideoOutputClass)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
            "`encoder` was given but `settings` names no video output (a "
            "UMoviePipelineVideoOutputBase subclass such as MoviePipelineMP4EncoderOutput) to "
            "write it to."));
        return true;
    }
    Plan.bAddAntiAliasing = HasBlock(Ctx, TEXT("sampling"))
        && !Plan.AddClasses.Contains(UMoviePipelineAntiAliasingSetting::StaticClass());
    if (!PlanValueBlocks(Ctx, Plan.VideoOutputClass, Plan, Refusal))
    {
        Ctx.SendError(Refusal.Code, Refusal.Message);
        return true;
    }
    if (RefuseIfPieBlocksSave(Ctx))
    {
        return true;
    }

    // --- create, apply, save ---
    UPackage* Package = CreatePackage(*PackageName);
    UMoviePipelinePrimaryConfig* Config = Package
        ? NewObject<UMoviePipelinePrimaryConfig>(Package, UMoviePipelinePrimaryConfig::StaticClass(),
            FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional)
        : nullptr;
    if (!Config)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED,
            FString::Printf(TEXT("Could not allocate a UMoviePipelinePrimaryConfig at '%s'."), *ObjectPath));
        return true;
    }
    FAssetRegistryModule::AssetCreated(Config);
    TArray<FString> AddedClassPaths;
    ApplyPlan(Config, Plan, AddedClassPaths);
    SaveAndRespond(Ctx, Config, Plan, AddedClassPaths, /*bCreated=*/true, nullptr);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE, TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}

// ---- mrq.set_preset_settings ----
REGISTER_RPC_HANDLER("mrq.set_preset_settings", "mrq",
    "Change values on an existing UMoviePipelinePrimaryConfig preset asset and save it. Takes the "
    "same `output` / `encoder` / `sampling` blocks as mrq.create_preset; a block whose setting the "
    "preset does not carry (no enabled video output for `encoder`, no anti-aliasing setting for "
    "`sampling`) is REFUSED with MRQ_SETTING_NOT_PRESENT unless that class is listed in "
    "`addSettings`, so no setting is created that was not asked for. Every value is validated before "
    "the asset is touched. Returns `previous` (the preflight block before the edit) beside "
    "`preflight` (read off the asset after the save) and the save report; a save that is not durable is "
    "SAVE_FAILED with the same payload.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Existing preset asset path (mrq.list_presets enumerates them)"),
        RPC_PARAM_OPT("addSettings", "array", "Setting class paths to add first; refused if the preset already carries an instance of one"),
        RPC_PARAM_OPT_NESTED("output", "object", "Output setting values: { width, height, outputDirectory, fileNameFormat, frameRate, frameRateDenominator }. Same rules as mrq.create_preset. These keys are the whole schema; any other key is refused with UNKNOWN_NESTED_PARAMS",
            TEXT("width"), TEXT("height"), TEXT("outputDirectory"), TEXT("fileNameFormat"),
            TEXT("frameRate"), TEXT("frameRateDenominator")),
        RPC_PARAM_OPT_NESTED("encoder", "object", "Video output values: { rateControl, constantRateFactor, averageBitrateMbps, maxBitrateMbps, includeAudio }. Written to the preset's first enabled video output. These keys are the whole schema; any other key is refused with UNKNOWN_NESTED_PARAMS",
            TEXT("rateControl"), TEXT("constantRateFactor"), TEXT("averageBitrateMbps"),
            TEXT("maxBitrateMbps"), TEXT("includeAudio")),
        RPC_PARAM_OPT_NESTED("sampling", "object", "Anti-aliasing setting values: { spatialSampleCount, temporalSampleCount, engineWarmUpCount, renderWarmUpCount, useCameraCutForWarmUp, renderWarmUpFrames, overrideAntiAliasing, antiAliasingMethod }. These keys are the whole schema; any other key is refused with UNKNOWN_NESTED_PARAMS",
            TEXT("spatialSampleCount"), TEXT("temporalSampleCount"), TEXT("engineWarmUpCount"),
            TEXT("renderWarmUpCount"), TEXT("useCameraCutForWarmUp"), TEXT("renderWarmUpFrames"),
            TEXT("overrideAntiAliasing"), TEXT("antiAliasingMethod"))
    ))
{
#if PINWRIGHT_HAS_MRQ_PRESET_AUTHORING
    using namespace PinWrightMRQPresetLocal;
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) { return true; }
    AssetPath.TrimStartAndEndInline();

    // An empty addSettings adds nothing, so it counts as absent: otherwise `addSettings: []` alone
    // passes the "Nothing to change" check and re-saves an unchanged asset.
    const TArray<TSharedPtr<FJsonValue>>* AddField = nullptr;
    const bool bHasAdd = Ctx.GetRawPayload()->HasField(TEXT("addSettings"))
        && !(Ctx.GetRawPayload()->TryGetArrayField(TEXT("addSettings"), AddField) && AddField->Num() == 0);
    if (!bHasAdd && !HasBlock(Ctx, TEXT("output")) && !HasBlock(Ctx, TEXT("encoder"))
        && !HasBlock(Ctx, TEXT("sampling")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT(
            "Nothing to change: pass at least one of addSettings, output, encoder, sampling."));
        return true;
    }

    FString PathError;
    if (!PinWrightMRQ::ValidateAssetPath(AssetPath, TEXT("assetPath"), PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }
    UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Loaded)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(
            TEXT("assetPath '%s' did not load. mrq.list_presets enumerates the presets that exist."),
            *AssetPath));
        return true;
    }
    UMoviePipelinePrimaryConfig* Config = Cast<UMoviePipelinePrimaryConfig>(Loaded);
    if (!Config)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_WRONG_TYPE, FString::Printf(
            TEXT("assetPath '%s' is a %s, not a UMoviePipelinePrimaryConfig."),
            *AssetPath, *Loaded->GetClass()->GetName()));
        return true;
    }

    FPresetPlan Plan;
    FRefusal Refusal;
    if (bHasAdd)
    {
        const TArray<TSharedPtr<FJsonValue>>* AddValues = nullptr;
        if (!Ctx.RequireArray(TEXT("addSettings"), AddValues) || !AddValues) { return true; }
        if (!ResolveSettingClasses(*AddValues, TEXT("addSettings"), Plan.AddClasses, Refusal))
        {
            Ctx.SendError(Refusal.Code, Refusal.Message);
            return true;
        }
        for (UClass* Class : Plan.AddClasses)
        {
            if (const UMoviePipelineSetting* Existing = Config->FindSettingByClass(
                Class, /*bIncludeDisabledSettings=*/true, /*bExactMatch=*/true))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("The preset already carries a %s setting%s; drop it from addSettings%s."),
                    *Class->GetName(), Existing->IsEnabled() ? TEXT("") : TEXT(" (disabled)"),
                    Existing->IsEnabled() ? TEXT(" and set its values directly")
                        : TEXT(" and enable the existing one (its bEnabled) instead")));
                return true;
            }
        }
    }

    // The targets the blocks will be written to: what the preset carries, or what addSettings
    // adds — never something this call invents.
    const UMoviePipelineSetting* ExistingVideo =
        Config->FindSettingByClass(UMoviePipelineVideoOutputBase::StaticClass());
    Plan.VideoOutputClass = ExistingVideo ? ExistingVideo->GetClass()
        : FirstVideoOutputClass(Plan.AddClasses);
    // A setting that exists but is disabled would take the values and do nothing with them, and
    // adding a second one is refused above, so say which of the two situations this is.
    auto RefuseMissing = [&Ctx, Config](const TCHAR* Block, UClass* Class, const TCHAR* AddExample)
    {
        const UMoviePipelineSetting* Disabled =
            Config->FindSettingByClass(Class, /*bIncludeDisabledSettings=*/true);
        Ctx.SendError(ErrorCodes::ERR_MRQ_SETTING_NOT_PRESENT, Disabled
            ? FString::Printf(TEXT("`%s` was given but the preset's %s setting is DISABLED, so the ")
                TEXT("values would have no effect. Nothing was changed; enable that setting (its ")
                TEXT("bEnabled) first."), Block, *Disabled->GetClass()->GetName())
            : FString::Printf(TEXT("`%s` was given but the preset carries no %s setting. Nothing ")
                TEXT("was changed; add one deliberately with addSettings (e.g. %s)."),
                Block, *Class->GetName(), AddExample));
    };
    if (HasBlock(Ctx, TEXT("encoder")) && !Plan.VideoOutputClass)
    {
        RefuseMissing(TEXT("encoder"), UMoviePipelineVideoOutputBase::StaticClass(),
            TEXT("/Script/MovieRenderPipelineMP4Encoder.MoviePipelineMP4EncoderOutput"));
        return true;
    }
    if (HasBlock(Ctx, TEXT("sampling"))
        && !Config->FindSettingByClass(UMoviePipelineAntiAliasingSetting::StaticClass())
        && !Plan.AddClasses.Contains(UMoviePipelineAntiAliasingSetting::StaticClass()))
    {
        RefuseMissing(TEXT("sampling"), UMoviePipelineAntiAliasingSetting::StaticClass(),
            TEXT("/Script/MovieRenderPipelineCore.MoviePipelineAntiAliasingSetting"));
        return true;
    }
    if (!PlanValueBlocks(Ctx, Plan.VideoOutputClass, Plan, Refusal))
    {
        Ctx.SendError(Refusal.Code, Refusal.Message);
        return true;
    }
    if (RefuseIfPieBlocksSave(Ctx))
    {
        return true;
    }

    TArray<FString> UnusedWarnings;
    const TSharedPtr<FJsonObject> Previous = PinWrightMRQ::BuildPreflightReport(
        PinWrightMRQ::ReadPreflightContext(Config), UnusedWarnings);
    TArray<FString> AddedClassPaths;
    ApplyPlan(Config, Plan, AddedClassPaths);
    SaveAndRespond(Ctx, Config, Plan, AddedClassPaths, /*bCreated=*/false, Previous);
#else
    Ctx.SendError(ErrorCodes::ERR_MRQ_NOT_AVAILABLE, TEXT("MovieRenderPipeline plugin not enabled in this build"));
#endif
    return true;
}
