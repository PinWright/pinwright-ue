// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/AssetDumpCache.h"
#include "Handlers/Asset/AssetDumpHandlerInternal.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Asset/AssetPathParamUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/AssetDumpWriter.h"
#include "Utils/AssetDumpBuilder.h"
#include "Utils/PropertyUtils.h"
#include "Utils/ActorDescribeBuilder.h"
#include "Utils/IrSidecarRegistry.h"
#include "Utils/JsonSidecarRegistry.h"
#include "Utils/JsonBuilders.h"
#include "Utils/SortedJsonWriter.h"
#include "Handlers/UI/WidgetAnimationJsonSerializer.h"
#include "Handlers/UI/WidgetDesignerCaptureUtil.h"
#include "Handlers/UI/WidgetXmlExporter.h"
#include "Handlers/Blueprint/SCSTextEmitter.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Animation/AnimGraphDumpBuilder.h"
#include "Handlers/Asset/MetaSoundDumpBuilder.h"
#include "Animation/AnimBlueprint.h"
#include "State/PluginState.h"
#include "State/JobRegistry.h"
#include "PinWrightSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "FileHelpers.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelScriptBlueprint.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Actor.h"
#include "Engine/Level.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#include "AssetCompilingManager.h"
#include "BTIR/BTIRDecompiler.h"
#include "Editor.h"
#include "HAL/PlatformMemory.h"
#include "PinWrightSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Linker.h"
#include "UObject/UObjectHash.h"
#include "Utils/PropertyExport.h"
#include "WorldPartition/WorldPartition.h"
#include "Compat/EngineVersionCompat.h"
// UE 5.4 renamed the World Partition actor-descriptor iteration type from
// FWorldPartitionActorDesc to FWorldPartitionActorDescInstance (with a matching
// ForEachActorDescInstance helper) and added a public GetActorTransform() accessor.
// On 5.3 the older FWorldPartitionActorDesc / ForEachActorDesc API is used and there is
// no public transform getter, so the external-reference entry falls back to identity.
#if __has_include("WorldPartition/WorldPartitionActorDescInstance.h")
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#define MCP_WP_ACTOR_DESC_TYPE FWorldPartitionActorDescInstance
#define MCP_WP_FOR_EACH_ACTOR_DESC ForEachActorDescInstance
#define MCP_WP_HAS_DESC_TRANSFORM 1
#else
#include "WorldPartition/WorldPartitionActorDesc.h"
#define MCP_WP_ACTOR_DESC_TYPE FWorldPartitionActorDesc
#define MCP_WP_FOR_EACH_ACTOR_DESC ForEachActorDesc
#define MCP_WP_HAS_DESC_TRANSFORM 0
#endif
#include "WorldPartition/WorldPartitionHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"
#include "Containers/Ticker.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Notifications/INotificationWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#include "Metasound.h"
#define MCP_DISPATCH_HAS_METASOUND 1
#else
#define MCP_DISPATCH_HAS_METASOUND 0
#endif

namespace AssetDumpHandler
{
    FAssetData SelectPrimaryAssetData(
        const TArray<FAssetData>& PackageAssets,
        const FString& PackageName);
}

class FAssetDumpNotificationWidget final : public INotificationWidget
{
public:
    explicit FAssetDumpNotificationWidget(const FString& InTicketId)
        : TicketId(InTicketId)
    {
        Content =
            SNew(SBox)
            .HeightOverride(72.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SAssignNew(TitleText, STextBlock)
                        .Font(FAppStyle::Get().GetFontStyle(TEXT("NotificationList.FontBold")))
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(FMargin(0.0f, 5.0f, 0.0f, 0.0f))
                    [
                        SAssignNew(StatusText, STextBlock)
                        .TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>(
                            TEXT("NotificationList.WidgetText")))
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SAssignNew(AssetText, STextBlock)
                        .TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>(
                            TEXT("NotificationList.WidgetText")))
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    ]
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(FMargin(12.0f, 0.0f, 0.0f, 0.0f))
                .VAlign(VAlign_Bottom)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Cancel")))
                    .ToolTipText(FText::FromString(
                        TEXT("Cancel this asset dump after the current asset finishes.")))
                    .Visibility_Lambda([this]()
                    {
                        return bPending ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    .OnClicked_Lambda([this]()
                    {
                        if (bPending)
                        {
                            FPluginState::Get().GetJobRegistry().Cancel(TicketId);
                        }
                        return FReply::Handled();
                    })
                ]
            ];
    }

    void Update(const FText& InTitle, const FText& InStatus, const FText& InAsset)
    {
        TitleText->SetText(InTitle);
        StatusText->SetText(InStatus);
        AssetText->SetText(InAsset);
        AssetText->SetToolTipText(InAsset);
    }

    virtual void OnSetCompletionState(SNotificationItem::ECompletionState State) override
    {
        bPending = State == SNotificationItem::CS_Pending;
    }

    virtual TSharedRef<SWidget> AsWidget() override
    {
        return Content.ToSharedRef();
    }

private:
    FString TicketId;
    bool bPending = true;
    TSharedPtr<SWidget> Content;
    TSharedPtr<STextBlock> TitleText;
    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<STextBlock> AssetText;
};

namespace
{
    // WriteSortedObject / WriteSortedValue / SerializeSortedJsonObject have moved to
    // Utils/SortedJsonWriter.h so tests can exercise the same writer.

    using JsonBuilders::BuildTransformJson;
    using JsonBuilders::BuildNameArrayJson;
    using JsonBuilders::GetActorLevelPackageName;

    FString SanitizeActorFileBase(const FString& Raw)
    {
        FString Result = Raw;
        const TCHAR* InvalidChars = TEXT("\\/:*?\"<>|. ");
        for (int32 Index = 0; InvalidChars[Index] != TEXT('\0'); ++Index)
        {
            Result.ReplaceCharInline(InvalidChars[Index], TEXT('_'));
        }
        while (Result.Contains(TEXT("__")))
        {
            Result.ReplaceInline(TEXT("__"), TEXT("_"));
        }
        Result.RemoveFromStart(TEXT("_"));
        Result.RemoveFromEnd(TEXT("_"));
        return Result.IsEmpty() ? TEXT("Actor") : Result;
    }

    FString MakeUniqueActorFileName(const FString& ActorName, TMap<FString, int32>& UsedNames)
    {
        const FString BaseName = SanitizeActorFileBase(ActorName);
        int32& Count = UsedNames.FindOrAdd(BaseName);
        ++Count;
        return Count == 1
            ? FString::Printf(TEXT("actors/%s.json"), *BaseName)
            : FString::Printf(TEXT("actors/%s_%d.json"), *BaseName, Count);
    }

    bool IsSafeActorDumpFilePath(FString& InOutPath)
    {
        FPaths::NormalizeFilename(InOutPath);
        if (InOutPath.IsEmpty()
            || InOutPath.Contains(TEXT(":"))
            || InOutPath.StartsWith(TEXT("/"))
            || InOutPath.StartsWith(TEXT("\\"))
            || InOutPath.Contains(TEXT(".."))
            || !InOutPath.StartsWith(TEXT("actors/"))
            || InOutPath == DumpFileNames::ActorsManifest
            || FPaths::GetExtension(InOutPath) != TEXT("json"))
        {
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> BuildExternalActorReference(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("storage"), TEXT("external-reference"));
        Entry->SetStringField(TEXT("name"), Actor->GetName());
        Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Entry->SetStringField(TEXT("path"), Actor->GetPathName());
        Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
        Entry->SetStringField(TEXT("level"), GetActorLevelPackageName(Actor));
        Entry->SetObjectField(TEXT("transform"), BuildTransformJson(Actor->GetActorTransform()));
        Entry->SetArrayField(TEXT("tags"), BuildNameArrayJson(Actor->Tags));
        if (UPackage* ExternalPackage = Actor->GetExternalPackage())
        {
            Entry->SetStringField(TEXT("package"), ExternalPackage->GetName());
        }
        Entry->SetStringField(TEXT("guid"), Actor->GetActorGuid().ToString());
        return Entry;
    }

    TSharedPtr<FJsonObject> BuildExternalActorReference(const MCP_WP_ACTOR_DESC_TYPE* Desc)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("storage"), TEXT("external-reference"));
        Entry->SetStringField(TEXT("guid"), Desc->GetGuid().ToString());
        Entry->SetStringField(TEXT("package"), Desc->GetActorPackage().ToString());
        Entry->SetStringField(TEXT("path"), Desc->GetActorSoftPath().ToString());
        Entry->SetStringField(TEXT("name"), Desc->GetActorName().ToString());
        Entry->SetStringField(TEXT("label"), Desc->GetActorLabel().ToString());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        Entry->SetStringField(TEXT("class"), Desc->GetDisplayClassNameString());
#else
        // GetDisplayClassNameString() was added in UE 5.5; fall back to the
        // native class name obtained via the reflection-level accessor.
        {
            UClass* NativeClass = Desc->GetActorNativeClass();
            Entry->SetStringField(TEXT("class"), NativeClass ? NativeClass->GetName() : FString());
        }
#endif
        Entry->SetStringField(TEXT("folder"), Desc->GetFolderPath().ToString());
#if MCP_WP_HAS_DESC_TRANSFORM
        Entry->SetObjectField(TEXT("transform"), BuildTransformJson(Desc->GetActorTransform()));
#else
        // UE 5.3 FWorldPartitionActorDesc has no public transform getter; emit identity.
        Entry->SetObjectField(TEXT("transform"), BuildTransformJson(FTransform::Identity));
#endif
        Entry->SetArrayField(TEXT("tags"), BuildNameArrayJson(Desc->GetTags()));
        return Entry;
    }

    TArray<AssetDumpWriter::FDumpFile> BuildLevelActorDumpFiles(UWorld* World)
    {
        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TSharedPtr<FJsonObject>> ManifestEntries;
        TMap<FString, int32> UsedFileNames;
        TSet<FString> SeenExternalPackages;
        TSet<FGuid> SeenActorGuids;
        int32 EmbeddedCount = 0;
        int32 ExternalReferenceCount = 0;

        if (!World || !World->PersistentLevel)
        {
            return Files;
        }

        TArray<AActor*> Actors;
        Actors.Reserve(World->PersistentLevel->Actors.Num());
        for (AActor* Actor : World->PersistentLevel->Actors)
        {
            if (Actor)
            {
                Actors.Add(Actor);
            }
        }
        Actors.Sort([](const AActor& A, const AActor& B)
        {
            return A.GetPathName().Compare(B.GetPathName(), ESearchCase::CaseSensitive) < 0;
        });

        for (AActor* Actor : Actors)
        {
            if (!Actor || Actor->HasAnyFlags(RF_ClassDefaultObject))
            {
                continue;
            }

            if (Actor->GetActorGuid().IsValid())
            {
                SeenActorGuids.Add(Actor->GetActorGuid());
            }

            if (Actor->IsPackageExternal())
            {
                TSharedPtr<FJsonObject> Entry = BuildExternalActorReference(Actor);
                FString PackageName;
                if (Entry->TryGetStringField(TEXT("package"), PackageName))
                {
                    SeenExternalPackages.Add(PackageName);
                }
                ManifestEntries.Add(Entry);
                ++ExternalReferenceCount;
                continue;
            }

            const FString RelativeFileName = MakeUniqueActorFileName(Actor->GetName(), UsedFileNames);
            Files.Add({RelativeFileName, SortedJsonWriter::SerializeSortedJsonObject(
                ActorDescribeBuilder::BuildActorJson(Actor, TEXT("embedded")))});
            ManifestEntries.Add(ActorDescribeBuilder::BuildActorManifestEntry(Actor, RelativeFileName));
            ++EmbeddedCount;
        }

        if (UWorldPartition* WorldPartition = World->GetWorldPartition())
        {
            FWorldPartitionHelpers::MCP_WP_FOR_EACH_ACTOR_DESC(
                WorldPartition,
                AActor::StaticClass(),
                [&](const MCP_WP_ACTOR_DESC_TYPE* Desc)
                {
                    if (!Desc)
                    {
                        return true;
                    }
                    if (SeenActorGuids.Contains(Desc->GetGuid()))
                    {
                        return true;
                    }
                    if (SeenExternalPackages.Contains(Desc->GetActorPackage().ToString()))
                    {
                        return true;
                    }

                    ManifestEntries.Add(BuildExternalActorReference(Desc));
                    SeenExternalPackages.Add(Desc->GetActorPackage().ToString());
                    ++ExternalReferenceCount;
                    return true;
                });
        }

        ManifestEntries.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
        {
            FString APath;
            FString BPath;
            A->TryGetStringField(TEXT("path"), APath);
            B->TryGetStringField(TEXT("path"), BPath);
            return APath.Compare(BPath, ESearchCase::CaseSensitive) < 0;
        });

        TArray<TSharedPtr<FJsonValue>> ActorValues;
        for (const TSharedPtr<FJsonObject>& Entry : ManifestEntries)
        {
            ActorValues.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
        Manifest->SetNumberField(TEXT("schemaVersion"), 1);
        Manifest->SetStringField(TEXT("levelPath"), World->GetPathName());
        Manifest->SetNumberField(TEXT("embeddedActorCount"), EmbeddedCount);
        Manifest->SetNumberField(TEXT("externalActorReferenceCount"), ExternalReferenceCount);
        Manifest->SetArrayField(TEXT("actors"), ActorValues);
        Files.Insert(AssetDumpWriter::FDumpFile{
            DumpFileNames::ActorsManifest,
            SortedJsonWriter::SerializeSortedJsonObject(Manifest)
        }, 0);
        return Files;
    }

    void RecordAspectDiagnostic(
        const FString& AssetPath,
        const FString& AspectName,
        const TArray<FString>& Warnings,
        TArray<TPair<FString, FString>>* OutFileErrors)
    {
        const FString Message = Warnings.IsEmpty()
            ? FString::Printf(TEXT("%s generation failed."), *AspectName)
            : FString::Join(Warnings, TEXT("\n"));

        if (OutFileErrors)
        {
            OutFileErrors->Add(TPair<FString, FString>(AspectName, Message));
        }

        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("asset.dump: skipped aspect '%s' for '%s': %s"),
            *AspectName, *AssetPath, *Message);
    }

    void AddRegisteredIrSidecarFiles(
        UObject* Asset,
        TArray<AssetDumpWriter::FDumpFile>& Files,
        TArray<TPair<FString, FString>>* OutFileErrors)
    {
        if (!Asset)
        {
            return;
        }

        TSet<FString> HandledFileNames;
        const FString AssetPath = Asset->GetPathName();
        for (const IrSidecarRegistry::FIrSidecarSpec& Spec : IrSidecarRegistry::GetRegisteredIrSidecars())
        {
            if (!Spec.FileName || !Spec.ClassFn || !Spec.BuildFn)
            {
                continue;
            }

            const FString FileName(Spec.FileName);
            if (HandledFileNames.Contains(FileName))
            {
                continue;
            }

            UClass* Class = Spec.ClassFn();
            if (!Class || !Asset->IsA(Class))
            {
                continue;
            }

            HandledFileNames.Add(FileName);
            const IrSidecarRegistry::FIrSidecarResult Result = Spec.BuildFn(Asset);
            if (!Result.bSuccess)
            {
                RecordAspectDiagnostic(AssetPath, FileName, Result.Warnings, OutFileErrors);
                continue;
            }

            if (!Result.Text.IsEmpty())
            {
                Files.Add({FileName, Result.Text});
            }
        }
    }

    // Emits the class-default Properties sidecar (CDO-vs-class diff) then walks the JSON
    // sidecar registry. The shared Properties emission lives here so every registry-routed
    // asset reuses it instead of repeating the CDO lookup inline. Irregular asset kinds
    // (Niagara, MetaSound, Widget/Anim/Blueprint, World, Redirector) bypass this path and
    // emit their Properties + multi-builder sidecars in BuildAllFilesForAsset directly.
    void RunRegisteredJsonSidecars(
        UObject* Asset,
        TArray<AssetDumpWriter::FDumpFile>& Files,
        TArray<TPair<FString, FString>>* OutFileErrors)
    {
        if (!Asset)
        {
            return;
        }

        if (UObject* CDO = Asset->GetClass()->GetDefaultObject())
        {
            if (TSharedPtr<FJsonObject> Props = BuildClassPropertyJson(Asset, CDO))
            {
                Files.Add({DumpFileNames::Properties, SortedJsonWriter::SerializeSortedJsonObject(Props)});
            }
        }

        TSet<FString> HandledFileNames;
        const FString AssetPath = Asset->GetPathName();
        for (const JsonSidecarRegistry::FJsonSidecarSpec& Spec : JsonSidecarRegistry::GetRegisteredJsonSidecars())
        {
            if (!Spec.FileName || !Spec.ClassFn || !Spec.BuildFn)
            {
                continue;
            }

            const FString FileName(Spec.FileName);
            if (HandledFileNames.Contains(FileName))
            {
                continue;
            }

            UClass* Class = Spec.ClassFn();
            if (!Class || !Asset->IsA(Class))
            {
                continue;
            }

            HandledFileNames.Add(FileName);
            TSharedPtr<FJsonObject> Json = Spec.BuildFn(Asset);
            if (!Json.IsValid())
            {
                if (Spec.NullDiagnostic)
                {
                    RecordAspectDiagnostic(AssetPath, FileName, {Spec.NullDiagnostic}, OutFileErrors);
                }
                continue;
            }

            Files.Add({FileName, SortedJsonWriter::SerializeSortedJsonObject(Json)});

            if (Spec.TextEmitterFileName && Spec.TextEmitterFn)
            {
                const FString TextBody = Spec.TextEmitterFn(Json);
                if (!TextBody.IsEmpty())
                {
                    Files.Add({Spec.TextEmitterFileName, TextBody});
                }
            }
        }
    }

    // Sorts dispatcher-emitted file names alphabetically (stable schema across runs)
    // and writes them onto MetaJson as `sidecarsEmitted`. Caller passes the file list
    // *before* meta.json is inserted, so the array never self-references the meta file.
    void AttachSidecarsEmittedToMeta(
        const TSharedPtr<FJsonObject>& MetaJson,
        const TArray<AssetDumpWriter::FDumpFile>& Files)
    {
        if (!MetaJson.IsValid())
        {
            return;
        }
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Files.Num());
        for (const AssetDumpWriter::FDumpFile& F : Files)
        {
            Values.Add(MakeShared<FJsonValueString>(F.Name));
        }
        Values.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            return A->AsString() < B->AsString();
        });
        MetaJson->SetArrayField(TEXT("sidecarsEmitted"), Values);
    }

    // Carries the widget-preview aspect's alpha facts out of BuildAllFilesForAsset, which
    // otherwise returns only files. Named with the AssetDump prefix because this is an
    // anonymous namespace and Unity merges it with every other TU in the same blob.
    struct FAssetDumpWidgetPreviewAlphaFacts
    {
        bool   bCaptured = false;
        bool   bOpaqueStamped = false;
        double AlphaZeroFraction = 0.0;
    };

    TArray<AssetDumpWriter::FDumpFile> BuildAllFilesForAsset(
        UObject* Asset,
        TArray<TPair<FString, FString>>* OutFileErrors = nullptr,
        bool bIncludeWidgetScreenshot = false,
        TMap<FString, TArray<uint8>>* OutBinaryFiles = nullptr,
        FAssetDumpWidgetPreviewAlphaFacts* OutWidgetPreviewAlpha = nullptr)
    {
        TArray<AssetDumpWriter::FDumpFile> Files;

        auto AddJsonFile = [&](const FString& Name, const TSharedPtr<FJsonObject>& Obj)
        {
            if (!Obj.IsValid()) return;
            Files.Add({Name, SortedJsonWriter::SerializeSortedJsonObject(Obj)});
        };

        auto AddStringFile = [&](const FString& Name, const FString& Body)
        {
            if (Body.IsEmpty()) return;
            Files.Add({Name, Body});
        };

        auto AddScsFiles = [&](UBlueprint* Blueprint)
        {
            TSharedPtr<FJsonObject> ScsJson = AssetDumpBuilder::BuildScsJson(Blueprint);
            AddJsonFile(DumpFileNames::Scs, ScsJson);
            AddStringFile(DumpFileNames::ScsTxt, SCSTextEmitter::BuildText(ScsJson));
        };

        TSharedPtr<FJsonObject> MetaJson = AssetDumpBuilder::BuildMetaJson(Asset);
        auto AddMetaFile = [&]()
        {
            if (MetaJson.IsValid())
            {
                Files.Insert({DumpFileNames::Meta, SortedJsonWriter::SerializeSortedJsonObject(MetaJson)}, 0);
            }
        };
        AddJsonFile(DumpFileNames::MapReferences, BuildMapReferencesJson(Asset));

        // UObjectRedirector: emit a tiny properties.json with just `redirectsTo` and
        // short-circuit before the type-specialized aspect dispatchers. Walking the
        // redirector's UObject properties produces an opaque empty `{}`; the redirect
        // target gets its own dump under its own true path on a separate iteration.
        if (Cast<UObjectRedirector>(Asset))
        {
            AssetDumpHandler::BuildRedirectorPropertiesAspect_Internal(Asset, Files, OutFileErrors);
            // Schema parity: emit propertiesStatus as n/a so a single consumer check works for
            // both redirector and non-Blueprint asset dumps without branching on className.
            AssetDumpHandler::FBlueprintPropertiesAspectStatus RedirectorStatus;
            RedirectorStatus.Status = TEXT("n/a");
            RedirectorStatus.Reason = TEXT("non_blueprint_asset");
            AssetDumpHandler::AttachBlueprintPropertiesStatusToMeta(MetaJson, RedirectorStatus);
            AttachSidecarsEmittedToMeta(MetaJson, Files);
            AddMetaFile();
            return Files;
        }

        UObject* CDO = nullptr;
        UObject* ParentCDO = nullptr;
        AssetDumpHandler::FBlueprintPropertiesAspectStatus BlueprintPropertiesStatus;

        if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
        {
            CDO = Asset->GetClass()->GetDefaultObject();
            AddJsonFile(DumpFileNames::Properties, CDO ? BuildClassPropertyJson(Asset, CDO) : nullptr);
            AddJsonFile(DumpFileNames::NiagaraCompile, NiagaraDumpBuilder::BuildCompileJson(NiagaraSystem));
        }
        else if (UNiagaraEmitter* NiagaraEmitter = Cast<UNiagaraEmitter>(Asset))
        {
            CDO = Asset->GetClass()->GetDefaultObject();
            AddJsonFile(DumpFileNames::Properties, CDO ? BuildClassPropertyJson(Asset, CDO) : nullptr);
            AddJsonFile(DumpFileNames::NiagaraCompile, NiagaraDumpBuilder::BuildEmitterCompileJson(NiagaraEmitter));
        }
        else if (UNiagaraScript* NiagaraScript = Cast<UNiagaraScript>(Asset))
        {
            CDO = Asset->GetClass()->GetDefaultObject();
            AddJsonFile(DumpFileNames::Properties, CDO ? BuildClassPropertyJson(Asset, CDO) : nullptr);
            AddJsonFile(DumpFileNames::NiagaraGraphs, NiagaraDumpBuilder::BuildScriptGraphsJson(NiagaraScript));
            AddJsonFile(DumpFileNames::NiagaraCompile, NiagaraDumpBuilder::BuildScriptCompileJson(NiagaraScript));
        }
#if MCP_DISPATCH_HAS_METASOUND
        // Must run before the default-tail SoundWave branch: UMetaSoundSource derives from
        // USoundWaveProcedural : USoundWave, so without this short-circuit it gets misclassified
        // as a SoundWave and dumps useless sentinel duration/sampleRate values. Suppressing
        // RootMetasoundDocument here (not via PropertyUtils skip list) keeps blast radius minimal.
        else if (Asset->IsA<UMetaSoundPatch>() || Asset->IsA<UMetaSoundSource>())
        {
            CDO = Asset->GetClass()->GetDefaultObject();
            TSharedPtr<FJsonObject> Props = CDO ? BuildClassPropertyJson(Asset, CDO) : nullptr;
            if (Props.IsValid())
            {
                Props->RemoveField(TEXT("RootMetasoundDocument"));
            }
            AddJsonFile(DumpFileNames::Properties, Props);
            AddJsonFile(DumpFileNames::MetaSound, MetaSoundDumpBuilder::BuildMetaSoundJson(Asset));
        }
#endif
        else if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset))
        {
            AssetDumpHandler::BuildBlueprintPropertiesAspect_Internal(
                WBP, Files, OutFileErrors, &BlueprintPropertiesStatus);

            AssetDumpHandler::BuildWidgetTreeAspect_Internal(WBP, Files, OutFileErrors);

            if (AssetDumpBuilder::ShouldEmitBpirText(WBP))
            {
                AddStringFile(DumpFileNames::BpirTxt, AssetDumpBuilder::BuildBpirText(WBP));
            }
            // Always invoke ExportAnimations so widget_animations.json is unconditionally
            // present — the exporter produces a schema-compliant
            // {schema, widgetPath, animations:[]} document for zero-animation widgets,
            // which makes a missing sidecar an unambiguous hard-failure signal for
            // downstream consumers.
            {
                TSharedPtr<FJsonObject> AnimationDocument;
                TArray<FString> AnimationWarnings;
                FString AnimationError;
                if (WidgetAnimationJson::ExportAnimations(
                        WBP,
                        AnimationDocument,
                        AnimationWarnings,
                        AnimationError,
                        /*bIncludeEventMetadata=*/true)
                    && AnimationDocument.IsValid())
                {
                    AddJsonFile(DumpFileNames::WidgetAnimations, AnimationDocument);
                }
                else
                {
                    TArray<FString> Diagnostics = AnimationWarnings;
                    if (!AnimationError.IsEmpty())
                    {
                        Diagnostics.Insert(AnimationError, 0);
                    }
                    RecordAspectDiagnostic(
                        WBP->GetPathName(),
                        DumpFileNames::WidgetAnimations,
                        Diagnostics,
                        OutFileErrors);
                }
            }

            // Opt-in only: opening the Designer + Slate pump per asset is expensive,
            // so the screenshot aspect stays off unless the caller explicitly requests it.
            if (bIncludeWidgetScreenshot && OutBinaryFiles)
            {
                TArray<uint8> PngBytes;
                FString CaptureError;
                // Info is requested (rather than passing nullptr as this call used to) purely
                // so the alpha facts reach the response. CapturePreviewToPng stamps every
                // pixel opaque before encoding, and Info.AlphaZeroFraction is the only record
                // of how much transparency that destroyed -- measured pre-stamp, so it cannot
                // be recovered from preview.png afterwards.
                WidgetDesignerCaptureUtil::FCaptureInfo PreviewInfo;
                if (WidgetDesignerCaptureUtil::CapturePreviewToPng(
                        WBP, /*MaxSize=*/1024, PngBytes, CaptureError, &PreviewInfo))
                {
                    OutBinaryFiles->Add(DumpFileNames::WidgetPreviewPng, MoveTemp(PngBytes));
                    if (OutWidgetPreviewAlpha)
                    {
                        // Copied, never asserted: bOpaqueStamped is whatever the util reports,
                        // so a future util that stops stamping makes the response say so
                        // instead of continuing to claim a stamp that no longer happens.
                        OutWidgetPreviewAlpha->bCaptured = true;
                        OutWidgetPreviewAlpha->bOpaqueStamped = PreviewInfo.bOpaqueStamped;
                        OutWidgetPreviewAlpha->AlphaZeroFraction = PreviewInfo.AlphaZeroFraction;
                    }
                }
                else
                {
                    RecordAspectDiagnostic(
                        WBP->GetPathName(),
                        DumpFileNames::WidgetPreviewPng,
                        {CaptureError.IsEmpty() ? TEXT("preview capture failed") : CaptureError},
                        OutFileErrors);
                }
            }
        }
        else if (UAnimBlueprint* ABP = Cast<UAnimBlueprint>(Asset))
        {
            AssetDumpHandler::BuildBlueprintPropertiesAspect_Internal(
                ABP, Files, OutFileErrors, &BlueprintPropertiesStatus);
            if (AssetDumpBuilder::ShouldEmitBpirText(ABP))
            {
                AddStringFile(DumpFileNames::BpirTxt, AssetDumpBuilder::BuildBpirText(ABP));
            }
            AddScsFiles(ABP);
            AddJsonFile(DumpFileNames::AnimGraph, AnimGraphDumpBuilder::BuildAnimGraphJson(ABP));
        }
        else if (UBlueprint* BP = Cast<UBlueprint>(Asset))
        {
            AssetDumpHandler::BuildBlueprintPropertiesAspect_Internal(
                BP, Files, OutFileErrors, &BlueprintPropertiesStatus);
            if (AssetDumpBuilder::ShouldEmitBpirText(BP))
            {
                AddStringFile(DumpFileNames::BpirTxt, AssetDumpBuilder::BuildBpirText(BP));
            }
            AddScsFiles(BP);
        }
        else if (UWorld* World = Cast<UWorld>(Asset))
        {
            // World properties stay class-default based; map-specific state is emitted separately.
            CDO = Asset->GetClass()->GetDefaultObject();
            if (Asset->GetClass()->GetSuperClass())
            {
                ParentCDO = Asset->GetClass()->GetSuperClass()->GetDefaultObject();
            }
            AddJsonFile(DumpFileNames::Properties, CDO ? BuildClassPropertyJson(CDO, ParentCDO) : nullptr);

            // World Settings — diff against AWorldSettings defaults
            if (AWorldSettings* WS = World->GetWorldSettings())
            {
                UObject* WSParent = AWorldSettings::StaticClass()->GetDefaultObject();
                AddJsonFile(DumpFileNames::WorldSettings, BuildClassPropertyJson(WS, WSParent));
            }

            // Level Blueprint — skip if absent or empty
            if (World->PersistentLevel)
            {
                ULevelScriptBlueprint* LevelBP = World->PersistentLevel->GetLevelScriptBlueprint(false);
                if (LevelBP)
                {
                    AddStringFile(DumpFileNames::LevelBp, AssetDumpBuilder::BuildBpirText(LevelBP));
                }
            }

            // Streaming sublevels
            const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
            if (StreamingLevels.Num() > 0)
            {
                TArray<TSharedPtr<FJsonValue>> SubArr;
                for (ULevelStreaming* SL : StreamingLevels)
                {
                    if (!SL) continue;
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("name"),           SL->GetName());
                    Entry->SetStringField(TEXT("packageName"),    SL->GetWorldAssetPackageFName().ToString());
                    Entry->SetStringField(TEXT("streamingClass"), SL->GetClass()->GetName());
                    Entry->SetBoolField(TEXT("shouldBeLoaded"),  SL->ShouldBeLoaded());
                    Entry->SetBoolField(TEXT("shouldBeVisible"), SL->GetShouldBeVisibleFlag());
                    SubArr.Add(MakeShared<FJsonValueObject>(Entry));
                }
                TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
                SubObj->SetArrayField(TEXT("sublevels"), SubArr);
                AddJsonFile(DumpFileNames::Sublevels, SubObj);
            }

            Files.Append(BuildLevelActorDumpFiles(World));
        }
        else
        {
            // Registry-routed: class-default Properties sidecar + at most one JSON-builder
            // sidecar (optionally paired with a text twin) per matching FJsonSidecarSpec.
            // Asset types whose dump fits this "Properties + one builder" shape register a
            // REGISTER_DUMP_JSON_SIDECAR record next to their builder instead of an inline
            // branch here. Properties-only kinds (UMaterial, UMaterialFunction, UBehaviorTree,
            // UBlackboardData) register nothing and fall through to the shared Properties emit.
            RunRegisteredJsonSidecars(Asset, Files, OutFileErrors);
        }

        // Non-Blueprint paths leave BlueprintPropertiesStatus empty. Default to a uniform
        // n/a marker so consumers can read `meta.propertiesStatus.status` for every asset
        // kind without branching on className. The redirector branch above takes a separate
        // early return and sets its own n/a status.
        if (BlueprintPropertiesStatus.Status.IsEmpty())
        {
            BlueprintPropertiesStatus.Status = TEXT("n/a");
            BlueprintPropertiesStatus.Reason = TEXT("non_blueprint_asset");
        }
        AssetDumpHandler::AttachBlueprintPropertiesStatusToMeta(MetaJson, BlueprintPropertiesStatus);
        AddRegisteredIrSidecarFiles(Asset, Files, OutFileErrors);

        AttachSidecarsEmittedToMeta(MetaJson, Files);
        AddMetaFile();
        return Files;
    }

    bool LoadBaselineDumpFiles(const FString& DumpDir, TMap<FString, FString>& OutBaselineContents)
    {
        // Only the inline/non-registry aspects are hand-listed here. Registry-backed sidecars
        // (IR and JSON) are folded in from their registration records below, so a new
        // REGISTER_DUMP_IR_SIDECAR / REGISTER_DUMP_JSON_SIDECAR becomes baseline-visible
        // automatically without a parallel edit to this array.
        const TCHAR* FixedCanonical[] = {
            DumpFileNames::Meta,
            DumpFileNames::Properties,
            DumpFileNames::TreeXml,
            DumpFileNames::WidgetAnimations,
            DumpFileNames::BpirTxt,
            DumpFileNames::Scs,
            DumpFileNames::ScsTxt,
            DumpFileNames::WorldSettings,
            DumpFileNames::LevelBp,
            DumpFileNames::Sublevels,
            DumpFileNames::ActorsManifest,
            DumpFileNames::NiagaraGraphs,
            DumpFileNames::NiagaraCompile,
            DumpFileNames::AnimGraph,
            DumpFileNames::MetaSound,
            DumpFileNames::Nir,
            DumpFileNames::MapReferences,
            DumpFileNames::WidgetPreviewPng,
        };

        TArray<FString> Canonical;
        for (const TCHAR* Name : FixedCanonical)
        {
            Canonical.AddUnique(FString(Name));
        }
        for (const IrSidecarRegistry::FIrSidecarSpec& Spec : IrSidecarRegistry::GetRegisteredIrSidecars())
        {
            if (Spec.FileName)
            {
                Canonical.AddUnique(FString(Spec.FileName));
            }
        }
        for (const JsonSidecarRegistry::FJsonSidecarSpec& Spec : JsonSidecarRegistry::GetRegisteredJsonSidecars())
        {
            if (Spec.FileName)
            {
                Canonical.AddUnique(FString(Spec.FileName));
            }
            if (Spec.TextEmitterFileName)
            {
                Canonical.AddUnique(FString(Spec.TextEmitterFileName));
            }
        }

        for (const FString& Name : Canonical)
        {
            FString Path = DumpDir / Name;
            if (IFileManager::Get().FileExists(*Path))
            {
                FString Content;
                FFileHelper::LoadFileToString(Content, *Path);
                OutBaselineContents.Add(Name, MoveTemp(Content));
            }
        }

        if (const FString* ManifestContent = OutBaselineContents.Find(FString(DumpFileNames::ActorsManifest)))
        {
            TSharedPtr<FJsonObject> Manifest;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ManifestContent);
            if (FJsonSerializer::Deserialize(Reader, Manifest) && Manifest.IsValid())
            {
                const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
                if (Manifest->TryGetArrayField(TEXT("actors"), Actors))
                {
                    for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
                    {
                        TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
                        FString RelPath;
                        if (!ActorObj.IsValid()
                            || !ActorObj->TryGetStringField(TEXT("file"), RelPath)
                            || !IsSafeActorDumpFilePath(RelPath)
                            || OutBaselineContents.Contains(RelPath))
                        {
                            continue;
                        }

                        FString Content;
                        if (FFileHelper::LoadFileToString(Content, *(DumpDir / RelPath)))
                        {
                            OutBaselineContents.Add(RelPath, MoveTemp(Content));
                        }
                    }
                }
            }
        }

        return OutBaselineContents.Num() > 0;
    }

    bool IsMapOrWorldFolderAssetData(const FAssetData& Data)
    {
        static const FTopLevelAssetPath WorldClassPath(TEXT("/Script/Engine"), TEXT("World"));
        return Data.AssetClassPath == WorldClassPath
            || (Data.PackageFlags & PKG_ContainsMap) != 0;
    }

    // Logs why an otherwise-successful dump did not get a .dumpcache.json record.
    // Shared by the success-path writer and the skip-stub branch so the veto line
    // shape stays identical: package, reason token, source kind, dirty flag,
    // baseline membership, dump dir.
    void LogCacheEligibilityVeto(
        const FString& PackageName,
        const AssetDumpCache::FAssetDumpSourceFingerprint& Source,
        bool bInBaseline,
        const FString& DumpDir)
    {
        const bool bUncached =
            Source.Kind == AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached;
        const TCHAR* KindText = TEXT("uncached");
        switch (Source.Kind)
        {
        case AssetDumpCache::EAssetDumpSourceFingerprintKind::PackageSavedHash:
            KindText = TEXT("packageSavedHash");
            break;
        case AssetDumpCache::EAssetDumpSourceFingerprintKind::FileStat:
            KindText = TEXT("fileStat");
            break;
        default:
            break;
        }

        UE_LOG(LogPinWrightSubsystem, Log,
            TEXT("asset.dump: no cache record for '%s': reason=%s sourceKind=%s bPackageIsDirty=%s bInBaseline=%s dumpDir='%s'"),
            *PackageName,
            bUncached ? TEXT("uncached-source") : TEXT("user-dirty(in-baseline)"),
            KindText,
            Source.bPackageIsDirty ? TEXT("true") : TEXT("false"),
            bInBaseline ? TEXT("true") : TEXT("false"),
            *DumpDir);
    }

    // Snapshots the set of packages already dirty before a dump begins, so a
    // post-dump pass can clear only the packages the dump itself dirtied (via
    // LoadObject -> PostLoad) without disturbing edits the user had pending.
    // FNames survive GC across async dump ticks; raw UPackage* would not.
    TSet<FName> DumpDirtyGuard_Snapshot()
    {
        TArray<UPackage*> Dirty;
        FEditorFileUtils::GetDirtyContentPackages(Dirty);
        FEditorFileUtils::GetDirtyWorldPackages(Dirty);

        TSet<FName> Result;
        Result.Reserve(Dirty.Num());
        for (const UPackage* Package : Dirty)
        {
            if (Package)
            {
                Result.Add(Package->GetFName());
            }
        }
        return Result;
    }

    // Clears the dirty flag on every package that became dirty during the dump
    // (i.e. whose FName is not in Baseline). Packages the user already had dirty
    // stay dirty. No save/checkout is performed - only the in-memory flag resets.
    void DumpDirtyGuard_Restore(const TSet<FName>& Baseline)
    {
        TArray<UPackage*> Dirty;
        FEditorFileUtils::GetDirtyContentPackages(Dirty);
        FEditorFileUtils::GetDirtyWorldPackages(Dirty);

        for (UPackage* Package : Dirty)
        {
            if (Package && !Baseline.Contains(Package->GetFName()))
            {
                Package->SetDirtyFlag(false);
            }
        }
    }

    // RAII: snapshots dirty packages on construction and restores on scope exit.
    // Used by the synchronous asset.dump path; async dumps carry the baseline on
    // FAsyncFolderDumpState and restore per-tick instead.
    struct FScopedDumpDirtyRestore
    {
        TSet<FName> Baseline;
        FScopedDumpDirtyRestore() : Baseline(DumpDirtyGuard_Snapshot()) {}
        ~FScopedDumpDirtyRestore() { DumpDirtyGuard_Restore(Baseline); }
    };

    void BeginAsyncDumpPhase(
        FAsyncFolderDumpState& State,
        const FString& Phase,
        const FString& AssetPath = FString())
    {
        State.CurrentPhase = Phase;
        State.CurrentAsset = AssetPath;
        State.CurrentPhaseStartedAt = FDateTime::UtcNow();
        State.CurrentPhaseStartedSeconds = FPlatformTime::Seconds();
    }

    void AddAsyncDumpDiagnostics(
        const FAsyncFolderDumpState& State,
        const TSharedPtr<FJsonObject>& Object)
    {
        if (!Object.IsValid())
        {
            return;
        }

        Object->SetStringField(TEXT("currentAsset"), State.CurrentAsset);
        Object->SetStringField(TEXT("currentPhase"), State.CurrentPhase);
        Object->SetStringField(TEXT("phaseStartedAt"), State.CurrentPhaseStartedAt.ToIso8601());
        const double PhaseElapsed = State.CurrentPhaseStartedSeconds > 0.0
            ? FMath::Max(0.0, FPlatformTime::Seconds() - State.CurrentPhaseStartedSeconds)
            : 0.0;
        Object->SetNumberField(TEXT("phaseElapsedSeconds"), PhaseElapsed);
        Object->SetNumberField(TEXT("lastAssetElapsedSeconds"), State.LastAssetElapsedSeconds);
        Object->SetNumberField(TEXT("compileDeferralCount"), State.CompileDeferralCount);
        Object->SetNumberField(TEXT("compileTimeoutCount"), State.CompileTimeoutCount);
        Object->SetNumberField(TEXT("releaseStepCount"), State.ReleaseStepCount);
        Object->SetNumberField(TEXT("releasedPackageCount"), State.ReleasedPackageCount);
        Object->SetNumberField(TEXT("workingSetBytes"),
            static_cast<double>(FPlatformMemory::GetStats().UsedPhysical));
    }

    int32 GetAsyncDumpCompletedCount(const FAsyncFolderDumpState& State)
    {
        return FMath::Clamp(
            State.UnchangedCount + State.DumpedCount + State.SkipCount,
            0,
            State.TotalAssetCount);
    }

    bool TryGetAsyncDumpEstimatedRemainingSeconds(
        const FAsyncFolderDumpState& State,
        double& OutSeconds)
    {
        const int32 ProcessedQueuedAssets = State.DumpedCount + State.SkipCount;
        const int32 RemainingAssets = FMath::Max(
            0, State.TotalAssetCount - GetAsyncDumpCompletedCount(State));
        if (State.WorkStartedSeconds <= 0.0
            || ProcessedQueuedAssets <= 0
            || RemainingAssets <= 0)
        {
            return false;
        }

        const double ElapsedSeconds = FMath::Max(
            0.0, FPlatformTime::Seconds() - State.WorkStartedSeconds);
        OutSeconds = (ElapsedSeconds / ProcessedQueuedAssets) * RemainingAssets;
        return FMath::IsFinite(OutSeconds);
    }

    FString FormatAsyncDumpDuration(double Seconds)
    {
        const int32 WholeSeconds = FMath::Max(0, FMath::CeilToInt(Seconds));
        if (WholeSeconds < 60)
        {
            return FString::Printf(TEXT("%ds"), WholeSeconds);
        }

        const int32 WholeMinutes = WholeSeconds / 60;
        if (WholeMinutes < 60)
        {
            return FString::Printf(TEXT("%dm %02ds"), WholeMinutes, WholeSeconds % 60);
        }

        return FString::Printf(TEXT("%dh %02dm"),
            WholeMinutes / 60, WholeMinutes % 60);
    }

    FText BuildAsyncDumpNotificationTitle(const FAsyncFolderDumpState& State)
    {
        if (State.bPreflightPending)
        {
            if (!State.bRegistryScanComplete)
            {
                return FText::FromString(FString::Printf(
                    TEXT("PinWright asset dump: scanning %s"), *State.FolderPath));
            }
            return FText::FromString(FString::Printf(
                TEXT("PinWright asset dump setup: %d / %d"),
                State.PreflightProcessedCount, State.TotalAssetCount));
        }

        const int32 Completed = GetAsyncDumpCompletedCount(State);
        const double Percent = State.TotalAssetCount > 0
            ? 100.0 * Completed / State.TotalAssetCount
            : 100.0;
        return FText::FromString(FString::Printf(
            TEXT("PinWright asset dump: %d / %d (%.1f%%)"),
            Completed, State.TotalAssetCount, Percent));
    }

    FText BuildAsyncDumpNotificationStatusText(const FAsyncFolderDumpState& State)
    {
        double EstimatedRemainingSeconds = 0.0;
        const FString Eta = TryGetAsyncDumpEstimatedRemainingSeconds(
            State, EstimatedRemainingSeconds)
            ? FormatAsyncDumpDuration(EstimatedRemainingSeconds)
            : TEXT("estimating...");
        return FText::FromString(FString::Printf(TEXT("ETA %s"), *Eta));
    }

    FText BuildAsyncDumpNotificationAssetText(const FAsyncFolderDumpState& State)
    {
        if (!State.CurrentAsset.IsEmpty())
        {
            return FText::FromString(State.CurrentAsset);
        }
        if (State.bPreflightPending && !State.FolderPath.IsEmpty())
        {
            return FText::FromString(State.FolderPath);
        }
        return FText::FromString(TEXT("Waiting for first asset..."));
    }

    void UpdateAsyncDumpNotification(FAsyncFolderDumpState& State)
    {
        if (const TSharedPtr<SNotificationItem> Notification =
                State.ProgressNotification.Pin())
        {
            if (State.ProgressNotificationWidget.IsValid())
            {
                State.ProgressNotificationWidget->Update(
                    BuildAsyncDumpNotificationTitle(State),
                    BuildAsyncDumpNotificationStatusText(State),
                    BuildAsyncDumpNotificationAssetText(State));
            }
        }
    }

    void FinishAsyncDumpNotification(FAsyncFolderDumpState& State, bool bSuccess)
    {
        if (const TSharedPtr<SNotificationItem> Notification =
                State.ProgressNotification.Pin())
        {
            const int32 Completed = GetAsyncDumpCompletedCount(State);
            const TCHAR* Outcome = State.bCancelRequested
                ? TEXT("cancelled")
                : (bSuccess ? TEXT("complete") : TEXT("stopped"));
            if (State.ProgressNotificationWidget.IsValid())
            {
                State.ProgressNotificationWidget->Update(
                    FText::FromString(FString::Printf(
                        TEXT("PinWright asset dump %s: %d / %d"),
                        Outcome, Completed, State.TotalAssetCount)),
                    BuildAsyncDumpNotificationStatusText(State),
                    FText::FromString(State.FolderPath));
            }
            Notification->SetCompletionState(bSuccess
                ? SNotificationItem::CS_Success
                : SNotificationItem::CS_Fail);
            Notification->ExpireAndFadeout();
        }
        State.ProgressNotification.Reset();
        State.ProgressNotificationWidget.Reset();
    }

    void StartAsyncDumpNotification(FAsyncFolderDumpState& State)
    {
        if (State.Kind != EAsyncAssetDumpKind::Folder
            || State.JobTicketId.IsEmpty()
            || FApp::IsUnattended()
            || !FSlateApplication::IsInitialized())
        {
            return;
        }

        const FString TicketId = State.JobTicketId;
        State.ProgressNotificationWidget =
            MakeShared<FAssetDumpNotificationWidget>(TicketId);
        State.ProgressNotificationWidget->Update(
            BuildAsyncDumpNotificationTitle(State),
            BuildAsyncDumpNotificationStatusText(State),
            BuildAsyncDumpNotificationAssetText(State));
        FNotificationInfo Info(State.ProgressNotificationWidget);
        Info.bFireAndForget = false;
        Info.WidthOverride = 560.0f;

        State.ProgressNotification =
            FSlateNotificationManager::Get().AddNotification(Info);
        if (const TSharedPtr<SNotificationItem> Notification =
                State.ProgressNotification.Pin())
        {
            Notification->SetCompletionState(SNotificationItem::CS_Pending);
        }
    }

    TSharedPtr<FJsonObject> BuildAsyncDumpProgressPayload(const FAsyncFolderDumpState& State)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        const int32 Completed = GetAsyncDumpCompletedCount(State);
        Payload->SetNumberField(TEXT("assetCount"), State.TotalAssetCount);
        Payload->SetBoolField(TEXT("assetCountKnown"), State.bRegistryScanComplete);
        Payload->SetNumberField(TEXT("preflightProcessed"), State.PreflightProcessedCount);
        Payload->SetNumberField(TEXT("completed"), Completed);
        Payload->SetNumberField(TEXT("remaining"),
            FMath::Max(0, State.TotalAssetCount - Completed));
        Payload->SetNumberField(TEXT("queued"), State.QueuedCount);
        Payload->SetNumberField(TEXT("dumped"), State.DumpedCount);
        Payload->SetNumberField(TEXT("unchanged"), State.UnchangedCount);
        Payload->SetNumberField(TEXT("skipCount"), State.SkipCount);
        const double ElapsedSeconds = State.WorkStartedSeconds > 0.0
            ? FMath::Max(0.0, FPlatformTime::Seconds() - State.WorkStartedSeconds)
            : 0.0;
        Payload->SetNumberField(TEXT("elapsedSeconds"), ElapsedSeconds);

        // The MCP progress pair, read by FJobRegistry::RecordProgress and published on the
        // caller's notifications/progress frames. "completed" is already the numerator this
        // job's own Slate notification renders, so the wire and the editor UI now agree by
        // construction rather than by two hand-maintained counters.
        //
        // The denominator is published only once the registry pre-pass has established it:
        // before then TotalAssetCount is a partial scan, and a total that shrinks or grows
        // under a client is worse than no bar at all. Until it is known the frames carry a
        // numerator alone, which still shows the job moving.
        Payload->SetNumberField(TEXT("progress"), Completed);
        if (State.bRegistryScanComplete && State.TotalAssetCount > 0)
        {
            Payload->SetNumberField(TEXT("total"), State.TotalAssetCount);
        }

        double EstimatedRemainingSeconds = 0.0;
        if (TryGetAsyncDumpEstimatedRemainingSeconds(State, EstimatedRemainingSeconds))
        {
            Payload->SetNumberField(
                TEXT("estimatedRemainingSeconds"), EstimatedRemainingSeconds);
        }
        AddAsyncDumpDiagnostics(State, Payload);
        return Payload;
    }

    constexpr double CompileWaitProgressIntervalSeconds = 1.0;

    // Defined with the release step below; every path that discards the sweep state has
    // to retire an outstanding collect watch, or the reachability callback outlives the
    // objects it would restore RF_Standalone on.
    void AbortDumpReleaseGcWatch();

    void CancelAsyncDump(const FString& TicketId)
    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();
        if (State.JobTicketId != TicketId)
        {
            return;
        }

        State.bCancelRequested = true;
        FinishAsyncDumpNotification(State, /*bSuccess=*/false);
        State.bInProgress = false;
        State.PendingAssets.Reset();
        if (State.TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(State.TickerHandle);
            State.TickerHandle.Reset();
        }

        // Cancellation deliberately does not reconcile the mirror: a partial
        // sweep does not have a complete live-dir set and must never prune it.
        DumpDirtyGuard_Restore(State.BaselineDirty);
        if (!State.bTickActive)
        {
            AbortDumpReleaseGcWatch();
            State = FAsyncFolderDumpState{};
        }
    }

    static TSharedPtr<FJsonObject> BuildSingleDumpResultJson(const FAsyncFolderDumpState& State)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> SkippedArr;
        for (const TPair<FString, FString>& Pair : State.FileErrors)
        {
            TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
            SkipEntry->SetStringField(TEXT("path"),   Pair.Key);
            SkipEntry->SetStringField(TEXT("reason"), Pair.Value);
            SkippedArr.Add(MakeShared<FJsonValueObject>(SkipEntry));
        }

        ResultObj->SetStringField(TEXT("assetPath"), State.AssetPath);
        ResultObj->SetStringField(TEXT("dumpDir"), State.DumpDir);
        ResultObj->SetStringField(TEXT("mode"), State.Mode);
        ResultObj->SetNumberField(TEXT("writtenCount"), State.WrittenPaths.Num());
        ResultObj->SetArrayField(TEXT("skipped"), SkippedArr);
        AddAsyncDumpDiagnostics(State, ResultObj);
        return ResultObj;
    }

    static void FinalizeAsyncDump()
    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();

        if (State.bCancelRequested)
        {
            FinishAsyncDumpNotification(State, /*bSuccess=*/false);
            DumpDirtyGuard_Restore(State.BaselineDirty);
            AbortDumpReleaseGcWatch();
            State = FAsyncFolderDumpState{};
            return;
        }

        BeginAsyncDumpPhase(State, TEXT("finalizing"), State.CurrentAsset);
        UpdateAsyncDumpNotification(State);

        const bool bSuccess = State.ErrorCode.IsEmpty();

        if (!State.JobTicketId.IsEmpty())
        {
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            FString Error;

            if (State.Kind == EAsyncAssetDumpKind::SingleAsset)
            {
                Result = BuildSingleDumpResultJson(State);
                if (!bSuccess)
                {
                    Error = State.ErrorMessage.IsEmpty() ? State.ErrorCode : State.ErrorMessage;
                }
            }
            else
            {
                AssetDumpHandler::ReconcileMirrorSubtree(State.RootDir, State.LiveDumpDirs);
                Result->SetStringField(TEXT("rootDir"), State.RootDir);
                Result->SetNumberField(TEXT("assetCount"), State.TotalAssetCount);
                Result->SetNumberField(TEXT("queued"), State.QueuedCount);
                Result->SetNumberField(TEXT("dumped"), State.DumpedCount);
                Result->SetNumberField(TEXT("unchanged"), State.UnchangedCount);
                Result->SetNumberField(TEXT("skipCount"), State.SkipCount);
                TArray<TSharedPtr<FJsonValue>> SkippedAssets;
                for (const FAsyncDumpSkip& Skip : State.AssetSkips)
                {
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("assetPath"), Skip.AssetPath);
                    Entry->SetStringField(TEXT("code"), Skip.Code);
                    Entry->SetStringField(TEXT("message"), Skip.Message);
                    SkippedAssets.Add(MakeShared<FJsonValueObject>(Entry));
                }
                Result->SetArrayField(TEXT("skipped"), SkippedAssets);
                AddAsyncDumpDiagnostics(State, Result);
            }

            FPluginState::Get().GetJobRegistry()
                .Complete(State.JobTicketId, bSuccess, Result, Error);
            State.JobTicketId.Reset();
        }
        else if (State.Kind == EAsyncAssetDumpKind::Folder)
        {
            AssetDumpHandler::ReconcileMirrorSubtree(State.RootDir, State.LiveDumpDirs);
        }

        FinishAsyncDumpNotification(State, bSuccess);

        // Final flush: clear any packages the last tick's loads left dirty before
        // the baseline is discarded with the state reset.
        DumpDirtyGuard_Restore(State.BaselineDirty);

        State = FAsyncFolderDumpState{};
    }

    void BuildFolderDumpCandidates(
        IAssetRegistry& AssetRegistry,
        const FString& FolderPath,
        bool bRecursive,
        bool bIncludeLevels,
        TArray<FPendingDumpEntry>& OutCandidates)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*FolderPath));
        Filter.bRecursivePaths = bRecursive;
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FAssetData> Assets;
        AssetRegistry.GetAssets(Filter, Assets);

        TMap<FString, TArray<FAssetData>> PackageRows;
        TArray<FString> PackageOrder;
        PackageOrder.Reserve(Assets.Num());
        for (const FAssetData& Data : Assets)
        {
            if (AssetDumpHandler::ShouldSkipFolderDumpAsset(Data, bIncludeLevels))
            {
                continue;
            }
            const FString PackageName = Data.PackageName.ToString();
            TArray<FAssetData>& Rows = PackageRows.FindOrAdd(PackageName);
            if (Rows.IsEmpty())
            {
                PackageOrder.Add(PackageName);
            }
            Rows.Add(Data);
        }

        OutCandidates.Reset(PackageOrder.Num());
        for (const FString& PackageName : PackageOrder)
        {
            const TArray<FAssetData>& Rows = PackageRows.FindChecked(PackageName);
            const FAssetData Primary =
                AssetDumpHandler::SelectPrimaryAssetData(Rows, PackageName);

            FPendingDumpEntry Entry;
            Entry.ObjectPath = AssetDumpHandler::MakePendingDumpPath(Primary);
            Entry.PackageName = PackageName;
            Entry.ClassPath = Primary.AssetClassPath;
            for (const FAssetData& Row : Rows)
            {
                Entry.bIsMapOrWorld =
                    Entry.bIsMapOrWorld || IsMapOrWorldFolderAssetData(Row);
            }
            OutCandidates.Add(MoveTemp(Entry));
        }
    }

    void TickFolderDumpPreflight(
        FAsyncFolderDumpState& State,
        double TickStart,
        double BudgetSec)
    {
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AR = ARM.Get();

        if (!State.bRegistryScanComplete)
        {
            BeginAsyncDumpPhase(State, TEXT("scanning_registry"), State.FolderPath);
            UpdateAsyncDumpNotification(State);
            BuildFolderDumpCandidates(
                AR,
                State.FolderPath,
                State.bRecursive,
                State.bIncludeLevels,
                State.PreflightCandidates);
            State.TotalAssetCount = State.PreflightCandidates.Num();
            State.bRegistryScanComplete = true;
            BeginAsyncDumpPhase(State, TEXT("checking_cache"), State.FolderPath);
            UpdateAsyncDumpNotification(State);
        }

        while (State.PreflightProcessedCount < State.PreflightCandidates.Num()
               && (FPlatformTime::Seconds() - TickStart) < BudgetSec)
        {
            if (State.bCancelRequested)
            {
                return;
            }

            FPendingDumpEntry& Candidate =
                State.PreflightCandidates[State.PreflightProcessedCount];
            State.CurrentAsset = Candidate.ObjectPath;
            UpdateAsyncDumpNotification(State);

            FString DumpDir;
            const bool bCacheFresh = !State.bForce && AssetDumpCache::IsDumpFresh(
                AR,
                Candidate.PackageName,
                State.OutRoot,
                State.bIncludeWidgetScreenshot,
                Candidate.bIsMapOrWorld,
                DumpDir);
            if (bCacheFresh)
            {
                State.LiveDumpDirs.Add(FPaths::ConvertRelativePathToFull(DumpDir));
                ++State.UnchangedCount;
            }
            else
            {
                State.PendingAssets.Add(MoveTemp(Candidate));
                ++State.QueuedCount;
            }
            ++State.PreflightProcessedCount;
        }

        if (State.PreflightProcessedCount == State.PreflightCandidates.Num())
        {
            AssetDumpHandler::SortPendingAssetsForProcessing(State.PendingAssets);
            State.PreflightCandidates.Reset();
            State.bPreflightPending = false;
            BeginAsyncDumpPhase(State, TEXT("queued"));
            UpdateAsyncDumpNotification(State);
        }
    }

    // ---- Folder-sweep release step ------------------------------------------------
    //
    // asset.dump_folder's only load site is DumpSingleAsset's LoadObject and, before
    // this step existed, nothing on the dump path ever released a package: a forced
    // /Game sweep (22,446 assets) grew the resident set until
    // GetMemoryAvailableForAssetCompilation reported ~240 MiB free and the process died
    // at the 128 GiB virtual wall, twice. Board:
    // B-dump-folder-sweep-never-gcs-ooms-editor.
    //
    // A plain CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS) frees none of those packages:
    // assets loaded from disk carry RF_Standalone, which IS the editor's keep flag. They
    // have to be released explicitly, following the PackageTools.cpp:564-589 idiom minus
    // its CloseAllEditorsForAsset step - that call is a check() abort surface
    // (docs/lessons.md), which is why only packages the sweep itself loaded, and none
    // with an editor open on them, are ever touched here.

    // Ceiling on the wait for a requested collect. The flag is consumed by
    // ConditionalCollectGarbage at the end of the following frame, so this only bounds a
    // pathological case (a modal dialog, a GC-disabling scope): the sweep resumes and
    // keeps its own accounting rather than stalling forever.
    constexpr double ReleaseGcWaitTimeoutSeconds = 60.0;

    // Live state of one outstanding release collect. Raw UObject* is deliberate and
    // matches UPackageTools::ObjectsThatHadFlagsCleared: the only dereference happens
    // inside PostReachabilityAnalysis, where nothing has been purged yet.
    struct FDumpReleaseGcWatch
    {
        FDelegateHandle ReachabilityHandle;
        TSet<UObject*>  ClearedObjects;
        bool bReachabilityObserved = false;

        // Module teardown can discard the sweep without going through a cancel path.
        // CoreUObject outlives plugin DLLs, so an un-removed handle here would be a
        // callback into freed code on the editor's next collect.
        ~FDumpReleaseGcWatch()
        {
            if (ReachabilityHandle.IsValid())
            {
                FCoreUObjectDelegates::PostReachabilityAnalysis.Remove(ReachabilityHandle);
            }
        }
    };

    FDumpReleaseGcWatch& GetDumpReleaseGcWatch()
    {
        static FDumpReleaseGcWatch Watch;
        return Watch;
    }

    // Mirrors UPackageTools::RestoreStandaloneOnReachableObjects. A package that survives
    // the collect - something still references one of its objects - must get RF_Standalone
    // back on the survivors, or the package is left half-purged and a later LoadObject
    // finds a package marked fully loaded that no longer contains the object it wants.
    void OnDumpReleaseReachabilityAnalysis()
    {
        FDumpReleaseGcWatch& Watch = GetDumpReleaseGcWatch();
        Watch.bReachabilityObserved = true;
        for (UObject* Object : Watch.ClearedObjects)
        {
            if (Object && !Object->IsUnreachable())
            {
                Object->SetFlags(RF_Standalone);
            }
        }
    }

    void EndDumpReleaseGcWatch()
    {
        FDumpReleaseGcWatch& Watch = GetDumpReleaseGcWatch();
        if (Watch.ReachabilityHandle.IsValid())
        {
            FCoreUObjectDelegates::PostReachabilityAnalysis.Remove(Watch.ReachabilityHandle);
            Watch.ReachabilityHandle.Reset();
        }
        Watch.ClearedObjects.Empty();
        Watch.bReachabilityObserved = false;
    }

    // Cancellation while a collect is outstanding. If reachability already ran, the
    // callback restored the survivors and the remaining pointers may be purged, so they
    // are never touched; otherwise nothing has been freed yet and every cleared object
    // gets its flag back, leaving the editor exactly as the sweep found it.
    void AbortDumpReleaseGcWatch()
    {
        FDumpReleaseGcWatch& Watch = GetDumpReleaseGcWatch();
        if (!Watch.bReachabilityObserved)
        {
            for (UObject* Object : Watch.ClearedObjects)
            {
                if (Object)
                {
                    Object->SetFlags(RF_Standalone);
                }
            }
        }
        EndDumpReleaseGcWatch();
    }

    TSet<FName> CollectPackagesOpenInAssetEditors()
    {
        TSet<FName> Result;
        if (!GEditor)
        {
            return Result;
        }
        UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!Subsystem)
        {
            return Result;
        }
        for (const UObject* Asset : Subsystem->GetAllEditedAssets())
        {
            if (Asset)
            {
                if (const UPackage* Package = Asset->GetOutermost())
                {
                    Result.Add(Package->GetFName());
                }
            }
        }
        return Result;
    }

    FName GetEditorWorldPackageName()
    {
        if (GEditor)
        {
            if (const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
            {
                if (const UPackage* Package = EditorWorld->GetOutermost())
                {
                    return Package->GetFName();
                }
            }
        }
        return NAME_None;
    }

    double BytesToGiB(uint64 Bytes)
    {
        return static_cast<double>(Bytes) / (1024.0 * 1024.0 * 1024.0);
    }

    void RunFolderDumpReleaseStep(FAsyncFolderDumpState& State)
    {
        BeginAsyncDumpPhase(State, TEXT("releasing_memory"));
        UpdateAsyncDumpNotification(State);

        // Never stack two watches: a previous one must have been retired first.
        EndDumpReleaseGcWatch();
        FDumpReleaseGcWatch& Watch = GetDumpReleaseGcWatch();

        const uint64 BeforeBytes = FPlatformMemory::GetStats().UsedPhysical;
        const int32 TrackedCount = State.SweepLoadedPackages.Num();

        // 1. Drain compilation before anything is collected. An asset whose DDC build is
        //    still in flight is held by raw C++ references from a thread-pool task, and
        //    collecting it faults on that worker rather than here (docs/lessons.md, the
        //    USoundWave / FAudioCookInputs case). FAssetCompilingManager covers the sound
        //    wave manager along with texture, mesh and shader compilation, all of which
        //    this sweep starts.
        FAssetCompilingManager::Get().FinishAllCompilation();
        FlushAsyncLoading();

        // 2. Drop the caches that key raw FProperty* by UClass. Neither is ever
        //    invalidated, and a collect that frees a Blueprint-generated class takes its
        //    FField chain with it.
        ClearSoftWorldPropertyCache();
        BTIRDecompiler::ClearPropertyCaches();

        // 3. Release only what this sweep loaded, and only what nothing else has claimed
        //    since. Dirty packages, packages the user already had dirty, packages with an
        //    asset editor open, the editor world and anything rooted are all left alone.
        const TSet<FName> OpenInAssetEditors = CollectPackagesOpenInAssetEditors();
        const FName EditorWorldPackage = GetEditorWorldPackageName();

        TArray<UObject*> PackagesToReset;
        TArray<UObject*> ObjectsInPackage;
        PackagesToReset.Reserve(TrackedCount);
        for (const FName& PackageName : State.SweepLoadedPackages)
        {
            UPackage* Package = FindPackage(nullptr, *PackageName.ToString());
            if (!Package
                || Package->IsDirty()
                || Package->IsRooted()
                || PackageName == EditorWorldPackage
                || State.BaselineDirty.Contains(PackageName)
                || OpenInAssetEditors.Contains(PackageName))
            {
                continue;
            }

            ObjectsInPackage.Reset();
            GetObjectsWithPackage(Package, ObjectsInPackage);
            for (UObject* Object : ObjectsInPackage)
            {
                if (Object->HasAnyFlags(RF_Standalone))
                {
                    Object->ClearFlags(RF_Standalone);
                    Watch.ClearedObjects.Add(Object);
                }
            }
            if (Package->HasAnyFlags(RF_Standalone))
            {
                Package->ClearFlags(RF_Standalone);
                Watch.ClearedObjects.Add(Package);
            }

            PackagesToReset.Add(Package);
        }

        // ResetLoaders here, not from inside the collect: called during GC it invalidates
        // attached bulk data instead of paying its payloads in
        // (PackageTools.cpp:582-586).
        if (PackagesToReset.Num() > 0)
        {
            ResetLoaders(PackagesToReset);
        }

        State.SweepLoadedPackages.Reset();
        State.AssetsSinceRelease = 0;
        ++State.ReleaseStepCount;
        State.ReleasedPackageCount += PackagesToReset.Num();
        State.ReleaseWorkingSetBeforeBytes = BeforeBytes;

        // 4. Ask the editor for a full purge. ForceGarbageCollection only raises a flag,
        //    consumed by ConditionalCollectGarbage after bInTick clears and EndFrame runs
        //    - the one collect primitive documented safe from this ticker
        //    (Dispatch/SafePoint.h). The restore callback goes on first so it is armed
        //    whenever that collect lands.
        Watch.bReachabilityObserved = false;
        Watch.ReachabilityHandle = FCoreUObjectDelegates::PostReachabilityAnalysis.AddStatic(
            &OnDumpReleaseReachabilityAnalysis);
        State.bAwaitingReleaseGc = true;
        State.ReleaseGcRequestedSeconds = FPlatformTime::Seconds();

        if (GEditor)
        {
            GEditor->ForceGarbageCollection(true);
        }

        UE_LOG(LogPinWrightSubsystem, Log,
            TEXT("asset.dump_folder: release step %d requested: tracked=%d released=%d ")
            TEXT("retained=%d workingSetBefore=%.2f GiB"),
            State.ReleaseStepCount,
            TrackedCount,
            PackagesToReset.Num(),
            TrackedCount - PackagesToReset.Num(),
            BytesToGiB(BeforeBytes));

        BeginAsyncDumpPhase(State, TEXT("waiting_for_release_gc"));
        State.CurrentPhaseStartedSeconds = State.ReleaseGcRequestedSeconds;
        UpdateAsyncDumpNotification(State);
    }

    // Returns true when this tick belongs to the release step - a collect is still
    // outstanding, or one was just requested - and no asset work may run.
    bool TickFolderDumpRelease(FAsyncFolderDumpState& State)
    {
        if (State.Kind != EAsyncAssetDumpKind::Folder || State.bPreflightPending)
        {
            return false;
        }

        if (State.bAwaitingReleaseGc)
        {
            const FDumpReleaseGcWatch& Watch = GetDumpReleaseGcWatch();
            const double WaitedSeconds =
                FPlatformTime::Seconds() - State.ReleaseGcRequestedSeconds;
            // Reachability alone is not enough: the purge that actually returns the
            // memory runs incrementally over the following frames.
            const bool bCollected =
                Watch.bReachabilityObserved && !IsIncrementalPurgePending();
            if (!bCollected && WaitedSeconds < ReleaseGcWaitTimeoutSeconds)
            {
                UpdateAsyncDumpNotification(State);
                return true;
            }

            State.bAwaitingReleaseGc = false;
            EndDumpReleaseGcWatch();

            const uint64 AfterBytes = FPlatformMemory::GetStats().UsedPhysical;
            UE_LOG(LogPinWrightSubsystem, Log,
                TEXT("asset.dump_folder: release step %d %s after %.1fs: workingSet ")
                TEXT("%.2f -> %.2f GiB (delta %.2f GiB)"),
                State.ReleaseStepCount,
                bCollected ? TEXT("collected") : TEXT("timed out"),
                WaitedSeconds,
                BytesToGiB(State.ReleaseWorkingSetBeforeBytes),
                BytesToGiB(AfterBytes),
                BytesToGiB(State.ReleaseWorkingSetBeforeBytes) - BytesToGiB(AfterBytes));

            // Fall through: the rest of this tick may dump assets again.
            return false;
        }

        if (State.SweepLoadedPackages.Num() == 0)
        {
            return false;
        }

        // The sweep's last asset is done: release before finalizing so a completed sweep
        // does not leave its whole working set resident behind it.
        if (State.PendingAssets.Num() > 0)
        {
            const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
            const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
            if (!AssetDumpHandler::ShouldRunDumpReleaseStep(
                    State.AssetsSinceRelease,
                    Settings ? Settings->AssetDumpReleaseIntervalAssets : 0,
                    MemoryStats.UsedPhysical,
                    MemoryStats.TotalPhysical,
                    Settings ? Settings->AssetDumpReleaseMemoryWatermark : 0.0f))
            {
                return false;
            }
        }

        RunFolderDumpReleaseStep(State);
        return true;
    }

    static bool TickFolderDump(float /*DeltaSeconds*/)
    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();
        if (State.RootDir.IsEmpty())
        {
            return false;
        }

        State.bTickActive = true;

        const double BudgetSec = 0.008;
        const double TickStart = FPlatformTime::Seconds();

        // Lazy-cached AssetRegistry pointer for the skip-stub branch's cache-record
        // fingerprint. Hoisted out of the per-skip body to avoid a TMap lookup
        // (FModuleManager::LoadModuleChecked) on every failure. Initialized only on
        // the first failure within this tick so ticks with zero skips pay nothing.
        IAssetRegistry* CachedAssetRegistry = nullptr;

        // Release step first: it either consumes this whole tick (a collect is
        // outstanding, or one was just requested) or costs a single predicate.
        const bool bReleaseConsumedTick = TickFolderDumpRelease(State);

        if (State.Kind == EAsyncAssetDumpKind::Folder && State.bPreflightPending)
        {
            TickFolderDumpPreflight(State, TickStart, BudgetSec);
        }

        while (!bReleaseConsumedTick
               && !State.bPreflightPending
               && State.PendingAssets.Num() > 0
               && (FPlatformTime::Seconds() - TickStart) < BudgetSec)
        {
            const FPendingDumpEntry Entry = State.PendingAssets.Pop(EAllowShrinking::No);

            // This is the cooperative boundary: publish what is about to run
            // before entering synchronous UObject code. Once DumpSingleAsset
            // begins, the ticker cannot hard-preempt a slow engine call.
            const bool bRetryingAsyncCompilation =
                State.CompileWaitStartedSeconds.Contains(Entry.ObjectPath);
            if (!bRetryingAsyncCompilation)
            {
                BeginAsyncDumpPhase(State, TEXT("dumping"), Entry.ObjectPath);
                UpdateAsyncDumpNotification(State);
            }
            if (State.bCancelRequested)
            {
                AbortDumpReleaseGcWatch();
                State.bTickActive = false;
                State = FAsyncFolderDumpState{};
                return false;
            }

            // Release-step bookkeeping: a package absent from memory now is one this
            // sweep is about to load, and therefore one the sweep may release later.
            // Anything already resident belongs to the editor or the user and is never
            // touched. Recorded before the load, because after it every package looks
            // resident.
            if (State.Kind == EAsyncAssetDumpKind::Folder
                && FindPackage(nullptr, *Entry.PackageName) == nullptr)
            {
                State.SweepLoadedPackages.Add(FName(*Entry.PackageName));
            }

            const double AssetStartedSeconds = FPlatformTime::Seconds();
            AssetDumpHandler::FDumpSingleResult R =
                AssetDumpHandler::DumpSingleAsset(Entry.ObjectPath, State.OutRoot,
                    State.Kind == EAsyncAssetDumpKind::SingleAsset && State.bDiff,
                    State.bIncludeWidgetScreenshot,
                    State.BaselineDirty,
                    /*bDeferAsyncCompilation=*/true);
            State.LastAssetElapsedSeconds = FMath::Max(
                0.0, FPlatformTime::Seconds() - AssetStartedSeconds);

            if (R.bDeferredForCompilation)
            {
                const double NowSeconds = FPlatformTime::Seconds();
                double& WaitStarted = State.CompileWaitStartedSeconds.FindOrAdd(Entry.ObjectPath);
                if (WaitStarted <= 0.0)
                {
                    WaitStarted = NowSeconds;
                }

                if (!AssetDumpHandler::HasAsyncCompilationTimedOut(WaitStarted, NowSeconds))
                {
                    ++State.CompileDeferralCount;
                    State.CurrentAsset = Entry.ObjectPath;
                    State.CurrentPhase = TEXT("waiting_for_compilation");
                    State.CurrentPhaseStartedSeconds = WaitStarted;
                    State.CurrentPhaseStartedAt = FDateTime::UtcNow()
                        - FTimespan::FromSeconds(FMath::Max(0.0, NowSeconds - WaitStarted));
                    UpdateAsyncDumpNotification(State);

                    double& LastProgress =
                        State.CompileWaitLastProgressSeconds.FindOrAdd(Entry.ObjectPath);
                    const bool bShouldReportWait = LastProgress <= 0.0
                        || NowSeconds - LastProgress >= CompileWaitProgressIntervalSeconds;
                    if (bShouldReportWait && !State.JobTicketId.IsEmpty())
                    {
                        LastProgress = NowSeconds;
                        FPluginState::Get().GetJobRegistry().RecordProgress(
                            State.JobTicketId,
                            FString::Printf(TEXT("waiting for async compilation: %s"), *Entry.ObjectPath),
                            BuildAsyncDumpProgressPayload(State));
                    }

                    const bool bOtherWorkRemains = State.PendingAssets.Num() > 0;
                    State.PendingAssets.Insert(Entry, 0);
                    if (!bOtherWorkRemains || State.bCancelRequested)
                    {
                        break;
                    }
                    continue;
                }

                State.CompileWaitStartedSeconds.Remove(Entry.ObjectPath);
                State.CompileWaitLastProgressSeconds.Remove(Entry.ObjectPath);
                ++State.AssetsSinceRelease;
                ++State.CompileTimeoutCount;
                ++State.SkipCount;
                const FString TimeoutMessage = FString::Printf(
                    TEXT("Async compilation did not finish within %.0f seconds; prior dump preserved."),
                    AssetDumpHandler::AsyncCompilationTimeoutSeconds);
                State.AssetSkips.Add({
                    Entry.ObjectPath,
                    AssetDumpErrorCodes::AssetCompileTimeout,
                    TimeoutMessage});
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("asset dump skipped '%s': %s: %s"),
                    *Entry.ObjectPath,
                    AssetDumpErrorCodes::AssetCompileTimeout,
                    *TimeoutMessage);

                if (State.Kind == EAsyncAssetDumpKind::SingleAsset)
                {
                    State.ErrorCode = AssetDumpErrorCodes::AssetCompileTimeout;
                    State.ErrorMessage = TimeoutMessage;
                    State.DumpDir = State.RootDir;
                    State.PendingAssets.Reset();
                    break;
                }

                // Preserve an earlier baseline during final reconciliation.
                // No skip stub or cache record is written for a compile timeout.
                const FString ExistingDumpDir = AssetDumpWriter::ResolveDumpDir(
                    Entry.PackageName, State.OutRoot);
                if (IFileManager::Get().DirectoryExists(*ExistingDumpDir))
                {
                    State.LiveDumpDirs.Add(FPaths::ConvertRelativePathToFull(ExistingDumpDir));
                }
                continue;
            }

            State.CompileWaitStartedSeconds.Remove(Entry.ObjectPath);
            State.CompileWaitLastProgressSeconds.Remove(Entry.ObjectPath);
            // Counted here rather than at the bottom of the loop: the PATH_TOO_LONG and
            // skip-stub branches below leave via `continue`.
            ++State.AssetsSinceRelease;

            if (State.Kind == EAsyncAssetDumpKind::SingleAsset)
            {
                State.WrittenPaths = MoveTemp(R.WrittenPaths);
                State.FileErrors = MoveTemp(R.FileErrors);
                State.DumpDir = R.DumpDir.IsEmpty() ? State.RootDir : R.DumpDir;
                State.ErrorCode = R.ErrorCode;
                State.ErrorMessage = R.ErrorMessage;
                State.Mode = R.Mode.IsEmpty() ? (State.bDiff ? TEXT("diff") : TEXT("dump")) : R.Mode;
                if (State.ErrorCode.IsEmpty() && !State.DumpDir.IsEmpty())
                {
                    State.LiveDumpDirs.Add(FPaths::ConvertRelativePathToFull(State.DumpDir));
                }
                State.PendingAssets.Reset();
                break;
            }

            if (R.ErrorCode.IsEmpty() && !R.DumpDir.IsEmpty())
            {
                State.LiveDumpDirs.Add(FPaths::ConvertRelativePathToFull(R.DumpDir));
                State.DumpedCount++;
                for (const TPair<FString, FString>& Err : R.FileErrors)
                {
                    UE_LOG(LogPinWrightSubsystem, Warning,
                        TEXT("asset.dump_folder: diagnostic for '%s' aspect '%s': %s"),
                        *Entry.ObjectPath, *Err.Key, *Err.Value);
                }
            }
            else if (!R.ErrorCode.IsEmpty())
            {
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("asset.dump_folder: skipped '%s': %s: %s"),
                    *Entry.ObjectPath, *R.ErrorCode, *R.ErrorMessage);

                State.SkipCount++;
                State.AssetSkips.Add({Entry.ObjectPath, R.ErrorCode, R.ErrorMessage});

                // PATH_TOO_LONG cannot be written; the stub path itself would also exceed MAX_PATH.
                // Skip the on-disk stub for that error, but the SkipCount bump above still records it.
                if (R.ErrorCode == AssetDumpErrorCodes::PathTooLong)
                {
                    continue;
                }

                // R.DumpDir is empty on ASSET_LOAD_FAILED (returned before DumpDir is set), so
                // recompute the would-be dump dir from the package name — dump dirs are
                // package-shaped (DumpSingleAsset keys its dir off the loaded asset's
                // outermost package). ResolveDumpDir already returns an absolutized path,
                // as does R.DumpDir.
                const FString StubDumpDir = R.DumpDir.IsEmpty()
                    ? AssetDumpWriter::ResolveDumpDir(Entry.PackageName, State.OutRoot)
                    : R.DumpDir;

                // className comes from the queue entry's registry class path — stable per
                // package (primary-selection rule), unlike a fresh registry lookup whose
                // row order flips run to run. Empty when nothing was registered.
                const FString StubClassName = Entry.ClassPath.IsValid()
                    ? Entry.ClassPath.GetAssetName().ToString()
                    : FString();

                TSharedPtr<FJsonObject> StubMeta = MakeShared<FJsonObject>();
                StubMeta->SetStringField(TEXT("assetPath"), Entry.ObjectPath);
                StubMeta->SetStringField(TEXT("className"), StubClassName);
                StubMeta->SetBoolField(TEXT("skipped"), true);
                StubMeta->SetStringField(TEXT("skipReason"), R.ErrorCode);
                StubMeta->SetStringField(TEXT("skipMessage"), R.ErrorMessage);

                // Reuse the same atomic writer the success path uses. WriteAssetDump
                // wipes+recreates StubDumpDir, then atomically writes each entry via tmp+move,
                // so leftover sidecars from a previous successful dump are cleared in one call.
                TArray<AssetDumpWriter::FDumpFile> StubFiles;
                StubFiles.Add({DumpFileNames::Meta,
                    SortedJsonWriter::SerializeSortedJsonObject(StubMeta)});

                FString StubWriteErr;
                AssetDumpWriter::FWriteResult StubWriteResult = AssetDumpWriter::WriteAssetDump(
                    StubDumpDir, AssetDumpWriter::ResolveDumpRoot(State.OutRoot),
                    StubFiles, StubWriteErr);

                if (StubWriteErr.IsEmpty() && StubWriteResult.Errors.Num() == 0)
                {
                    // Skip-stub dir participates in the live set so ReconcileMirrorSubtree
                    // does not prune it; a successful re-dump on a later sweep overwrites it.
                    State.LiveDumpDirs.Add(StubDumpDir);

                    // Mirror the success-path cache write — registry fingerprint through the
                    // same eligibility gate — so an unchanged unloadable package stops
                    // re-queueing every sweep. The record lists the stub's single meta.json
                    // and is marked skipped with the error code. Screenshot sweeps bypass
                    // the cache entirely, matching WriteDumpCacheForSuccessfulBaseline.
                    if (!State.bIncludeWidgetScreenshot)
                    {
                        if (CachedAssetRegistry == nullptr)
                        {
                            FAssetRegistryModule& ARM =
                                FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
                            CachedAssetRegistry = &ARM.Get();
                        }
                        const AssetDumpCache::FAssetDumpSourceFingerprint StubSource =
                            AssetDumpCache::BuildSourceFingerprintFromRegistry(
                                *CachedAssetRegistry,
                                Entry.PackageName,
                                Entry.bIsMapOrWorld);
                        if (!AssetDumpHandler::IsCacheEligibleSource(
                                StubSource, Entry.PackageName, State.BaselineDirty))
                        {
                            LogCacheEligibilityVeto(
                                Entry.PackageName,
                                StubSource,
                                State.BaselineDirty.Contains(FName(*Entry.PackageName)),
                                StubDumpDir);
                        }
                        else
                        {
                            TArray<FString> StubWrittenFiles;
                            StubWrittenFiles.Add(DumpFileNames::Meta);

                            AssetDumpCache::FAssetDumpOptionsFingerprint StubOptions;
                            StubOptions.bIncludeWidgetScreenshot = false;

                            const TMap<FString, int32> StubAspectVersions =
                                AssetDumpCache::MakeCurrentAspectVersions(StubWrittenFiles);
                            const FString StubCorrectCase = StubSource.CorrectCasePackageName.IsEmpty()
                                ? Entry.PackageName
                                : StubSource.CorrectCasePackageName;
                            AssetDumpCache::FAssetDumpCacheRecord StubRecord =
                                AssetDumpCache::MakeCacheRecord(
                                    Entry.PackageName,
                                    Entry.PackageName,
                                    StubCorrectCase,
                                    StubSource,
                                    AssetDumpCache::MakeCurrentDumperFingerprint(StubAspectVersions),
                                    StubOptions,
                                    StubWrittenFiles);
                            StubRecord.bSkipped = true;
                            StubRecord.SkipReason = R.ErrorCode;

                            FString StubCacheErr;
                            if (!AssetDumpCache::WriteCacheRecord(StubDumpDir, StubRecord, StubCacheErr))
                            {
                                UE_LOG(LogPinWrightSubsystem, Warning,
                                    TEXT("asset.dump_folder: failed to write dump cache for skip stub '%s': %s"),
                                    *Entry.PackageName, *StubCacheErr);
                            }
                        }
                    }
                }
                else
                {
                    UE_LOG(LogPinWrightSubsystem, Warning,
                        TEXT("asset.dump_folder: failed to write skip stub meta.json at '%s': %s"),
                        *StubDumpDir,
                        StubWriteErr.IsEmpty() && StubWriteResult.Errors.Num() > 0
                            ? *StubWriteResult.Errors[0].Value
                            : *StubWriteErr);
                }
            }

            BeginAsyncDumpPhase(State, TEXT("between_assets"), Entry.ObjectPath);
        }

        // Clear the dirty flag on packages this tick's loads dirtied, so the
        // editor shows zero unsaved packages even while the sweep is mid-flight.
        DumpDirtyGuard_Restore(State.BaselineDirty);
        UpdateAsyncDumpNotification(State);

        if (!State.JobTicketId.IsEmpty())
        {
            const FString Msg = FString::Printf(TEXT("completed %d / %d"),
                GetAsyncDumpCompletedCount(State),
                State.TotalAssetCount > 0
                    ? State.TotalAssetCount
                    : State.DumpedCount + State.UnchangedCount + State.SkipCount + State.PendingAssets.Num());
            TSharedPtr<FJsonObject> ProgressPayload = BuildAsyncDumpProgressPayload(State);
            FPluginState::Get().GetJobRegistry()
                .RecordProgress(State.JobTicketId, Msg, ProgressPayload);
        }

        if (State.bCancelRequested)
        {
            AbortDumpReleaseGcWatch();
            State.bTickActive = false;
            State = FAsyncFolderDumpState{};
            return false;
        }

        if (!bReleaseConsumedTick && !State.bPreflightPending && State.PendingAssets.Num() == 0)
        {
            State.bTickActive = false;
            FinalizeAsyncDump();
            return false;
        }
        State.bTickActive = false;
        return true;
    }

} // anonymous namespace

namespace AssetDumpHandler
{
    void SortPendingAssetsForProcessing(TArray<FPendingDumpEntry>& PendingAssets)
    {
        PendingAssets.Sort([](const FPendingDumpEntry& A, const FPendingDumpEntry& B)
        {
            const int32 ObjectPathOrder = A.ObjectPath.Compare(
                B.ObjectPath, ESearchCase::CaseSensitive);
            if (ObjectPathOrder != 0)
            {
                return ObjectPathOrder > 0;
            }
            return A.PackageName.Compare(B.PackageName, ESearchCase::CaseSensitive) > 0;
        });
    }

    bool HasAsyncCompilationTimedOut(
        double WaitStartedSeconds,
        double NowSeconds,
        double TimeoutSeconds)
    {
        return WaitStartedSeconds > 0.0
            && NowSeconds - WaitStartedSeconds >= TimeoutSeconds;
    }

    bool ShouldRunDumpReleaseStep(
        int32 AssetsSinceRelease,
        int32 IntervalAssets,
        uint64 WorkingSetBytes,
        uint64 TotalPhysicalBytes,
        float WatermarkFraction)
    {
        if (AssetsSinceRelease <= 0)
        {
            return false;
        }

        if (IntervalAssets > 0 && AssetsSinceRelease >= IntervalAssets)
        {
            return true;
        }

        return WatermarkFraction > 0.0f
            && TotalPhysicalBytes > 0
            && AssetsSinceRelease >= DumpReleaseMinAssetsBetweenSteps
            && static_cast<double>(WorkingSetBytes)
                >= static_cast<double>(TotalPhysicalBytes) * static_cast<double>(WatermarkFraction);
    }

    FString BlueprintTypeToStatusString(const UBlueprint* BP)
    {
        if (!BP)
        {
            return FString();
        }

        switch (BP->BlueprintType)
        {
        case BPTYPE_FunctionLibrary: return TEXT("FunctionLibrary");
        case BPTYPE_MacroLibrary:    return TEXT("MacroLibrary");
        case BPTYPE_Interface:       return TEXT("Interface");
        case BPTYPE_LevelScript:     return TEXT("LevelScript");
        case BPTYPE_Const:           return TEXT("Const");
        case BPTYPE_Normal:          return TEXT("Normal");
        default:                     return TEXT("Unknown");
        }
    }

    bool IsNoUPropertyBlueprintType(const UBlueprint* BP)
    {
        return BP &&
            (BP->BlueprintType == BPTYPE_FunctionLibrary ||
             BP->BlueprintType == BPTYPE_MacroLibrary ||
             BP->BlueprintType == BPTYPE_Interface);
    }

    TSharedPtr<FJsonObject> BuildBlueprintPropertiesStatusJson(
        const FBlueprintPropertiesAspectStatus& Status)
    {
        if (Status.Status.IsEmpty())
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("status"), Status.Status);
        if (!Status.Reason.IsEmpty())
        {
            Result->SetStringField(TEXT("reason"), Status.Reason);
        }
        if (Status.Status == TEXT("ok"))
        {
            Result->SetNumberField(TEXT("propertyCount"), Status.PropertyCount);
        }
        if (!Status.BlueprintType.IsEmpty())
        {
            Result->SetStringField(TEXT("blueprintType"), Status.BlueprintType);
        }
        return Result;
    }

    void AttachBlueprintPropertiesStatusToMeta(
        const TSharedPtr<FJsonObject>& Meta,
        const FBlueprintPropertiesAspectStatus& Status)
    {
        if (!Meta.IsValid())
        {
            return;
        }

        TSharedPtr<FJsonObject> StatusJson = BuildBlueprintPropertiesStatusJson(Status);
        if (StatusJson.IsValid())
        {
            Meta->SetObjectField(TEXT("propertiesStatus"), StatusJson);
        }
    }

    bool IsCacheEligibleSource(
        const AssetDumpCache::FAssetDumpSourceFingerprint& Source,
        const FString& PackageName,
        const TSet<FName>& BaselineDirty)
    {
        // A dirty package vetoes the record only when the user already had it
        // dirty before the dump started (in the baseline). Dirt the dump itself
        // induced (compile-on-load re-dirtying Blueprint packages) still matches
        // the on-disk bytes the fingerprint hashed, so the record stays valid.
        return Source.Kind != AssetDumpCache::EAssetDumpSourceFingerprintKind::Uncached
            && (!Source.bPackageIsDirty || !BaselineDirty.Contains(FName(*PackageName)));
    }

    bool WriteDumpCacheForSuccessfulBaseline(
        const FString& PackageName,
        const FString& DumpDir,
        TConstArrayView<FString> WrittenPaths,
        bool bIncludeWidgetScreenshot,
        const TSet<FName>& BaselineDirty)
    {
        if (bIncludeWidgetScreenshot)
        {
            return false;
        }

        TArray<FString> RelativeWrittenFiles =
            AssetDumpCache::MakeRelativeWrittenFiles(DumpDir, WrittenPaths);
        if (RelativeWrittenFiles.IsEmpty())
        {
            return false;
        }

        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AR = ARM.Get();

        AssetDumpCache::FAssetDumpSourceFingerprint Source =
            AssetDumpCache::BuildSourceFingerprintFromRegistry(
                AR,
                PackageName,
                /*bIsMapOrWorldPackage=*/false);
        if (!IsCacheEligibleSource(Source, PackageName, BaselineDirty))
        {
            LogCacheEligibilityVeto(
                PackageName,
                Source,
                BaselineDirty.Contains(FName(*PackageName)),
                DumpDir);
            return false;
        }

        AssetDumpCache::FAssetDumpOptionsFingerprint Options;
        Options.bIncludeWidgetScreenshot = bIncludeWidgetScreenshot;

        const TMap<FString, int32> AspectVersions =
            AssetDumpCache::MakeCurrentAspectVersions(RelativeWrittenFiles);
        const AssetDumpCache::FAssetDumpDumperFingerprint Dumper =
            AssetDumpCache::MakeCurrentDumperFingerprint(AspectVersions);
        const FString CorrectCasePackageName = Source.CorrectCasePackageName.IsEmpty()
            ? PackageName
            : Source.CorrectCasePackageName;
        const AssetDumpCache::FAssetDumpCacheRecord Record =
            AssetDumpCache::MakeCacheRecord(
                PackageName,
                PackageName,
                CorrectCasePackageName,
                Source,
                Dumper,
                Options,
                RelativeWrittenFiles);

        FString CacheError;
        if (!AssetDumpCache::WriteCacheRecord(DumpDir, Record, CacheError))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("asset.dump: failed to write dump cache for '%s': %s"),
                *PackageName,
                *CacheError);
            return false;
        }
        return true;
    }

    FAssetData SelectPrimaryAssetData(const TArray<FAssetData>& PackageAssets, const FString& PackageName)
    {
        if (PackageAssets.IsEmpty())
        {
            return FAssetData();
        }
        if (PackageAssets.Num() == 1)
        {
            return PackageAssets[0];
        }

        // Prefer the row named after the package tail (the conventional primary
        // asset), then the registry's own IsUAsset marker (tail match modulo
        // case), then fall back to the total-order minimum over
        // (AssetName, AssetClassPath) so packages with no tail-named row
        // (e.g. "Bake Out Materials" GUID outputs) resolve to the same primary
        // on every sweep regardless of registry row order.
        const FString ShortName = FPackageName::GetShortName(PackageName);
        for (const FAssetData& Data : PackageAssets)
        {
            if (Data.AssetName.ToString() == ShortName)
            {
                return Data;
            }
        }

        for (const FAssetData& Data : PackageAssets)
        {
            if (Data.IsUAsset())
            {
                return Data;
            }
        }

        const FAssetData* Primary = &PackageAssets[0];
        for (int32 Index = 1; Index < PackageAssets.Num(); ++Index)
        {
            const FAssetData& Row = PackageAssets[Index];
            const int32 NameOrder = Row.AssetName.ToString().Compare(
                Primary->AssetName.ToString(), ESearchCase::CaseSensitive);
            if (NameOrder < 0
                || (NameOrder == 0
                    && Row.AssetClassPath.ToString().Compare(
                        Primary->AssetClassPath.ToString(), ESearchCase::CaseSensitive) < 0))
            {
                Primary = &Row;
            }
        }
        return *Primary;
    }

    FDumpSingleResult DumpSingleAsset(
        const FString& NormalizedPath,
        const FString& OutRoot,
        bool bDiff,
        bool bIncludeWidgetScreenshot,
        const TSet<FName>& BaselineDirty,
        bool bDeferAsyncCompilation)
    {
        FDumpSingleResult Result;

        // Distinguish "source .uasset truly absent" (orphan-stub / baker residue) from
        // "package present but LoadObject returned null" (genuine load failure). The
        // existence check is also cheaper than the full LoadObject path when it fails.
        // ObjectPathToPackageName strips the trailing `.<asset name>` so the package-name
        // lookup matches /Game/Foo → Content/Foo.uasset; passing the full object path
        // through some FPackageName overloads ensure()'s on the `.` separator.
        //
        // Skip the on-disk check when the package is already in memory — transient test
        // packages and newly created (unsaved) editor assets have no .uasset file yet,
        // but their UPackage is live and LoadObject can resolve the asset. Orphan-stub
        // detection still fires for asset-registry residue because those packages are
        // not in memory either.
        const FString PackageNameForExistsCheck = FPackageName::ObjectPathToPackageName(NormalizedPath);
        const UPackage* InMemoryPackage = FindPackage(nullptr, *PackageNameForExistsCheck);
        if (!InMemoryPackage && !FPackageName::DoesPackageExist(PackageNameForExistsCheck))
        {
            Result.ErrorCode    = AssetDumpErrorCodes::AssetFileMissing;
            Result.ErrorMessage = FString::Printf(
                TEXT("No package file on disk for asset path '%s'"), *NormalizedPath);
            return Result;
        }

        UObject* Asset = LoadObject<UObject>(nullptr, *NormalizedPath);
        if (!Asset)
        {
            Result.ErrorCode    = AssetDumpErrorCodes::AssetLoadFailed;
            Result.ErrorMessage = FString::Printf(
                TEXT("Failed to load asset at path '%s'"), *NormalizedPath);
            return Result;
        }

        if (bDeferAsyncCompilation
            && Asset->GetClass()->ImplementsInterface(UInterface_AsyncCompilation::StaticClass()))
        {
            const IInterface_AsyncCompilation* AsyncCompilation =
                Cast<IInterface_AsyncCompilation>(Asset);
            if (AsyncCompilation && AsyncCompilation->IsCompiling())
            {
                Result.bDeferredForCompilation = true;
                return Result;
            }
        }

        const FString DumpPath = Asset->GetOutermost()
            ? Asset->GetOutermost()->GetName()
            : NormalizedPath;

        FString DumpRoot = AssetDumpWriter::ResolveDumpRoot(OutRoot);
        FString DumpDir  = AssetDumpWriter::ResolveDumpDir(DumpPath, OutRoot);

        FString PathErr;
        if (!AssetDumpWriter::CheckPathLength(DumpDir, PathErr))
        {
            Result.ErrorCode    = AssetDumpErrorCodes::PathTooLong;
            Result.ErrorMessage = PathErr;
            return Result;
        }

        Result.DumpDir = DumpDir;

        if (bDiff)
        {
            // Baseline reads stay on the mirror DumpDir; every diff-mode artifact
            // (including the fresh meta.json) is written to the per-asset diff dir
            // instead, so the (possibly git-committed) mirror is never touched.
            TMap<FString, FString> Baseline;
            if (!LoadBaselineDumpFiles(DumpDir, Baseline))
            {
                Result.ErrorCode    = AssetDumpErrorCodes::AssetNoBaseline;
                Result.ErrorMessage = FString::Printf(
                    TEXT("No baseline dump at '%s'. Run asset.dump without diff=true first."),
                    *DumpDir);
                return Result;
            }

            const FString DiffRoot = AssetDumpWriter::ResolveDiffRoot();
            const FString DiffDir  = AssetDumpWriter::ResolveDiffDir(DumpPath);
            if (!AssetDumpWriter::CheckPathLength(DiffDir, PathErr))
            {
                Result.ErrorCode    = AssetDumpErrorCodes::PathTooLong;
                Result.ErrorMessage = PathErr;
                return Result;
            }
            Result.DumpDir = DiffDir;

            TArray<TPair<FString, FString>> BuildErrors;
            TMap<FString, TArray<uint8>> BuildBinaries;
            // Diff mode rewrites the binary artifact into the diff dir, so it captures the
            // preview exactly as dump mode does and must report the same alpha facts.
            FAssetDumpWidgetPreviewAlphaFacts DiffPreviewAlpha;
            TArray<AssetDumpWriter::FDumpFile> Built = BuildAllFilesForAsset(
                Asset, &BuildErrors, bIncludeWidgetScreenshot, &BuildBinaries, &DiffPreviewAlpha);
            Result.FileErrors.Append(BuildErrors);
            Result.bWidgetPreviewCaptured = DiffPreviewAlpha.bCaptured;
            Result.bWidgetPreviewOpaqueStamped = DiffPreviewAlpha.bOpaqueStamped;
            Result.WidgetPreviewAlphaZeroFraction = DiffPreviewAlpha.AlphaZeroFraction;

            FString LatestMeta;
            for (const AssetDumpWriter::FDumpFile& F : Built)
            {
                if (F.Name == DumpFileNames::Meta) { LatestMeta = F.Content; break; }
            }

            TSet<FString> AspectNames;
            for (const TPair<FString, FString>& Pair : Baseline)
            {
                if (Pair.Key != DumpFileNames::Meta)
                {
                    AspectNames.Add(Pair.Key);
                }
            }
            for (const AssetDumpWriter::FDumpFile& F : Built)
            {
                if (F.Name != DumpFileNames::Meta)
                {
                    AspectNames.Add(F.Name);
                }
            }

            TArray<FString> NonMetaCanonical = AspectNames.Array();
            NonMetaCanonical.Sort();

            TArray<AssetDumpWriter::FBaselineDumpFile> Aspects;
            for (const FString& Name : NonMetaCanonical)
            {
                AssetDumpWriter::FBaselineDumpFile B;
                B.Name = Name;
                const FString* Old = Baseline.Find(Name);
                B.OldContent = Old ? *Old : FString();
                for (const AssetDumpWriter::FDumpFile& F : Built)
                {
                    if (F.Name == Name) { B.NewContent = F.Content; break; }
                }
                if (B.OldContent.IsEmpty() && B.NewContent.IsEmpty())
                {
                    continue;
                }
                Aspects.Add(MoveTemp(B));
            }

            FString WriteErr;
            AssetDumpWriter::FWriteResult WriteResult = AssetDumpWriter::WriteAssetDumpDiff(
                DiffDir, DiffRoot, LatestMeta, Aspects, WriteErr);

            Result.WrittenPaths = MoveTemp(WriteResult.WrittenPaths);
            for (const TPair<FString, FString>& Err : WriteResult.Errors)
            {
                Result.FileErrors.Add(Err);
            }
            // Diff mode emits *_new and *_diff sidecars; PNG bytes don't have a
            // sensible textual diff form, so we just rewrite the binary artifact
            // into the diff dir alongside the diff text aspects.
            for (const TPair<FString, TArray<uint8>>& Bin : BuildBinaries)
            {
                FString AbsPath;
                FString BinErr;
                if (AssetDumpWriter::WriteAssetDumpBinaryFile(
                        DiffDir, Bin.Key, Bin.Value, AbsPath, BinErr))
                {
                    Result.WrittenPaths.Add(AbsPath);
                }
                else
                {
                    Result.FileErrors.Add(TPair<FString, FString>(Bin.Key, BinErr));
                }
            }
            if (!WriteErr.IsEmpty())
            {
                Result.ErrorCode    = AssetDumpErrorCodes::DumpWriteFailed;
                Result.ErrorMessage = WriteErr;
            }
            Result.Mode = TEXT("diff");
            return Result;
        }

        TMap<FString, TArray<uint8>> BinaryFiles;
        FAssetDumpWidgetPreviewAlphaFacts PreviewAlpha;
        TArray<AssetDumpWriter::FDumpFile> Files = BuildAllFilesForAsset(
            Asset, &Result.FileErrors, bIncludeWidgetScreenshot, &BinaryFiles, &PreviewAlpha);
        Result.bWidgetPreviewCaptured = PreviewAlpha.bCaptured;
        Result.bWidgetPreviewOpaqueStamped = PreviewAlpha.bOpaqueStamped;
        Result.WidgetPreviewAlphaZeroFraction = PreviewAlpha.AlphaZeroFraction;

        // Direct asset.dump may be the first write into a fresh root; folder
        // sweeps seed at their own entry point.
        AssetDumpWriter::EnsureDumpRootScaffold(DumpRoot);

        FString WriteErr;
        AssetDumpWriter::FWriteResult WriteResult =
            AssetDumpWriter::WriteAssetDump(DumpDir, DumpRoot, Files, BinaryFiles, WriteErr);

        Result.WrittenPaths = MoveTemp(WriteResult.WrittenPaths);
        for (const TPair<FString, FString>& Err : WriteResult.Errors)
        {
            Result.FileErrors.Add(Err);
        }

        if (!WriteErr.IsEmpty())
        {
            Result.ErrorCode    = AssetDumpErrorCodes::DumpWriteFailed;
            Result.ErrorMessage = WriteErr;
        }

        if (Result.ErrorCode.IsEmpty() && Result.FileErrors.IsEmpty())
        {
            WriteDumpCacheForSuccessfulBaseline(
                DumpPath,
                DumpDir,
                Result.WrittenPaths,
                bIncludeWidgetScreenshot,
                BaselineDirty);

            // A fresh baseline supersedes any diff artifacts from earlier diff runs;
            // best-effort delete of this asset's diff dir.
            IFileManager::Get().DeleteDirectory(
                *AssetDumpWriter::ResolveDiffDir(DumpPath), /*RequireExists=*/false, /*Tree=*/true);
        }
        Result.Mode = TEXT("dump");

        return Result;
    }

    TArray<FString> ReconcileMirrorSubtree(const FString& SweptRoot,
                                            const TSet<FString>& LiveDirs)
    {
        TArray<FString> DeletedPaths;

        if (!IFileManager::Get().DirectoryExists(*SweptRoot))
        {
            return DeletedPaths;
        }

        TArray<FString> AllMetaFiles;
        IFileManager::Get().FindFilesRecursive(
            AllMetaFiles, *SweptRoot, DumpFileNames::Meta,
            /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/true);

        for (const FString& MetaPath : AllMetaFiles)
        {
            FString DirPath = FPaths::ConvertRelativePathToFull(FPaths::GetPath(MetaPath));

            // Only delete strict subpaths of SweptRoot — never SweptRoot itself.
            if (!DirPath.StartsWith(SweptRoot + TEXT("/")))
            {
                continue;
            }

            if (!LiveDirs.Contains(DirPath))
            {
                IFileManager::Get().DeleteDirectory(*DirPath, /*RequireExists=*/false, /*Tree=*/true);
                DeletedPaths.Add(DirPath);
            }
        }

        // Post-order empty-dir prune: deletes directories that became empty after
        // the asset-level prune, or that never contained any meta.json. Preserves
        // SweptRoot itself.
        TFunction<void(const FString&)> PostOrderPruneEmpty = [&](const FString& Dir)
        {
            TArray<FString> SubDirs;
            IFileManager::Get().FindFiles(SubDirs, *(Dir / TEXT("*")), /*Files=*/false, /*Directories=*/true);
            for (const FString& Sub : SubDirs)
            {
                PostOrderPruneEmpty(Dir / Sub);
            }

            if (Dir == SweptRoot) { return; }

            if (IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/false))
            {
                DeletedPaths.Add(FPaths::ConvertRelativePathToFull(Dir));
            }
        };
        PostOrderPruneEmpty(SweptRoot);

        return DeletedPaths;
    }

    FString MakePendingDumpPath(const FAssetData& Data)
    {
        return Data.GetObjectPathString();
    }

    bool ShouldSkipFolderDumpAsset(const FAssetData& Data, bool bIncludeLevels)
    {
        TArray<FString> PackagePathParts;
        Data.PackagePath.ToString().ParseIntoArray(PackagePathParts, TEXT("/"), /*CullEmpty=*/true);

        auto HasAdjacentSegments = [&PackagePathParts](const FString& First, const FString& Second) -> bool
        {
            for (int32 Index = 0; Index + 1 < PackagePathParts.Num(); ++Index)
            {
                if (PackagePathParts[Index] == First && PackagePathParts[Index + 1] == Second)
                {
                    return true;
                }
            }
            return false;
        };

        if (PackagePathParts.Contains(TEXT("__ExternalActors__"))
            || PackagePathParts.Contains(TEXT("__ExternalObjects__")))
        {
            return true;
        }

        if (HasAdjacentSegments(TEXT("Maps"), TEXT("_GENERATED")))
        {
            return true;
        }

        if (bIncludeLevels)
        {
            return false;
        }

        static const FTopLevelAssetPath WorldClassPath(TEXT("/Script/Engine"), TEXT("World"));
        if (Data.AssetClassPath == WorldClassPath)
        {
            return true;
        }

        static const FTopLevelAssetPath MapBuildDataRegistryClassPath(TEXT("/Script/Engine"), TEXT("MapBuildDataRegistry"));
        if (Data.AssetClassPath == MapBuildDataRegistryClassPath)
        {
            return true;
        }

        // Non-World rows whose package is a map (PKG_ContainsMap) — typically a
        // MapBuildDataRegistry embedded in the level's own package — would queue
        // the same package and LoadObject would resolve it back to the World,
        // defeating the class-path filter above. Skip them too.
        return (Data.PackageFlags & PKG_ContainsMap) != 0;
    }

    bool IsWorldAssetPath(const FString& NormalizedPath)
    {
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AR = ARM.Get();

        TArray<FAssetData> Assets;
        AR.GetAssetsByPackageName(FName(*NormalizedPath), Assets, /*bIncludeOnlyOnDiskAssets=*/true);

        static const FTopLevelAssetPath WorldClassPath(TEXT("/Script/Engine"), TEXT("World"));
        for (const FAssetData& Data : Assets)
        {
            if (Data.AssetClassPath == WorldClassPath)
            {
                return true;
            }
        }

        FString PackageFilename;
        if (FPackageName::DoesPackageExist(NormalizedPath, &PackageFilename)
            && FPackageName::IsMapPackageExtension(*FPaths::GetExtension(PackageFilename, /*bIncludeDot=*/true)))
        {
            return true;
        }
        return false;
    }

    FSingleAssetDumpStart StartAsyncSingleAssetDump(
        const FString& NormalizedPath,
        const FString& OutRoot,
        bool bDiff,
        bool bIncludeWidgetScreenshot)
    {
        FSingleAssetDumpStart Result;
        Result.AssetPath = NormalizedPath;
        // Diff mode writes to the diff tree, never the mirror; the start ticket
        // must advertise the same dir the completed result will carry.
        Result.DumpDir = bDiff
            ? AssetDumpWriter::ResolveDiffDir(NormalizedPath)
            : AssetDumpWriter::ResolveDumpDir(NormalizedPath, OutRoot);
        Result.StartedAt = FDateTime::UtcNow();
        Result.bDiff = bDiff;

        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();

        if (State.bInProgress || !State.JobTicketId.IsEmpty())
        {
            Result.ErrorCode = TEXT("DUMP_IN_PROGRESS");
            Result.ErrorMessage = State.Kind == EAsyncAssetDumpKind::SingleAsset
                ? FString::Printf(TEXT("Another asset dump is running for '%s' -> '%s'."),
                    *State.AssetPath, *State.RootDir)
                : FString::Printf(TEXT("Another folder dump is running for '%s' -> '%s'."),
                    *State.FolderPath, *State.RootDir);
            return Result;
        }

        FString PathErr;
        if (!AssetDumpWriter::CheckPathLength(Result.DumpDir, PathErr))
        {
            Result.ErrorCode = TEXT("PATH_TOO_LONG");
            Result.ErrorMessage = PathErr;
            return Result;
        }

        State.Kind = EAsyncAssetDumpKind::SingleAsset;
        State.RootDir = Result.DumpDir;
        State.AssetPath = NormalizedPath;
        State.OutRoot = OutRoot;
        State.StartedAt = Result.StartedAt;
        State.bInProgress = true;
        State.bDiff = bDiff;
        State.bIncludeWidgetScreenshot = bIncludeWidgetScreenshot;
        State.TotalAssetCount = 1;
        State.QueuedCount = 1;
        State.DumpedCount = 0;
        State.UnchangedCount = 0;
        State.SkipCount = 0;
        State.PendingAssets.Reset();
        {
            // StartAsyncSingleAssetDump is world-only (asset.dump's IsWorldAssetPath
            // gate); worlds load fine by package path and never take the skip-stub
            // branch, so ClassPath stays unset.
            FPendingDumpEntry Entry;
            Entry.ObjectPath = NormalizedPath;
            Entry.PackageName = FPackageName::ObjectPathToPackageName(NormalizedPath);
            Entry.bIsMapOrWorld = true;
            State.PendingAssets.Add(MoveTemp(Entry));
        }
        State.LiveDumpDirs.Reset();
        State.BaselineDirty = DumpDirtyGuard_Snapshot();
        BeginAsyncDumpPhase(State, TEXT("queued"));
        State.TickerHandle =
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickFolderDump));

        return Result;
    }

    FFolderDumpStart StartAsyncFolderDump(
        const FString& FolderPath,
        bool bRecursive,
        const FString& OutRoot,
        bool bIncludeLevels,
        bool bIncludeWidgetScreenshot,
        bool bForce,
        bool bDeferPreflight)
    {
        FFolderDumpStart Result;

        if (!FolderPath.StartsWith(TEXT("/")))
        {
            Result.ErrorCode = TEXT("INVALID_FOLDER");
            Result.ErrorMessage = FString::Printf(
                TEXT("folderPath must start with '/' (e.g. /Game/UI). Got: %s"), *FolderPath);
            return Result;
        }

        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();

        if (State.bInProgress || !State.JobTicketId.IsEmpty())
        {
            Result.ErrorCode = TEXT("DUMP_IN_PROGRESS");
            Result.ErrorMessage = FString::Printf(
                TEXT("Another folder dump is running for '%s' -> '%s'."),
                *State.FolderPath, *State.RootDir);
            Result.RootDir    = State.RootDir;
            Result.FolderPath = State.FolderPath;
            Result.StartedAt  = State.StartedAt;
            return Result;
        }

        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AR = ARM.Get();

        if (AR.IsLoadingAssets())
        {
            Result.ErrorCode = TEXT("REGISTRY_LOADING");
            Result.ErrorMessage = TEXT("Asset registry is still loading its initial scan. Retry shortly.");
            return Result;
        }

        const FString RootDir = AssetDumpWriter::ResolveDumpDir(FolderPath, OutRoot);
        const FDateTime StartedAt = FDateTime::UtcNow();

        // Seed root scaffolding even when every asset turns out cache-fresh, so
        // an existing mirror gains the files without a forced re-dump.
        AssetDumpWriter::EnsureDumpRootScaffold(AssetDumpWriter::ResolveDumpRoot(OutRoot));

        Result.RootDir    = RootDir;
        Result.FolderPath = FolderPath;
        Result.StartedAt  = StartedAt;

        if (bDeferPreflight)
        {
            // The RPC path creates and attaches its public job immediately after
            // this returns. Registry discovery and cache checks then run from the
            // ticker, so the otherwise expensive setup is visible and cancellable
            // at cooperative boundaries instead of blocking before a ticket exists.
            State.RootDir = RootDir;
            State.Kind = EAsyncAssetDumpKind::Folder;
            State.FolderPath = FolderPath;
            State.OutRoot = OutRoot;
            State.StartedAt = StartedAt;
            State.bInProgress = true;
            State.bDiff = false;
            State.bIncludeWidgetScreenshot = bIncludeWidgetScreenshot;
            State.bRecursive = bRecursive;
            State.bIncludeLevels = bIncludeLevels;
            State.bForce = bForce;
            State.bPreflightPending = true;
            State.BaselineDirty = DumpDirtyGuard_Snapshot();
            State.WorkStartedSeconds = FPlatformTime::Seconds();
            BeginAsyncDumpPhase(State, TEXT("scanning_registry"), FolderPath);
            State.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateStatic(&TickFolderDump));
            return Result;
        }

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*FolderPath));
        Filter.bRecursivePaths = bRecursive;
        // Only return on-disk assets; without this flag AR also returns in-memory
        // sub-objects from loaded levels (e.g. Map.Map:PersistentLevel.ActorFoo_C_8)
        // which cannot be loaded standalone and balloon the pending list by 10x.
        Filter.bIncludeOnlyOnDiskAssets = true;

        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);

        // Group surviving registry rows per package, then pick one deterministic
        // primary row each (SelectPrimaryAssetData). Multi-asset packages
        // (e.g. "Bake Out Materials" GUID outputs: an MI_* instance plus a T_*
        // texture, neither named after the package tail) must queue a real object
        // path — loading the bare package name fails when no inner object matches
        // the tail.
        TMap<FString, TArray<FAssetData>> PackageRows;
        TArray<FString> PackageOrder;
        PackageOrder.Reserve(Assets.Num());
        for (const FAssetData& Data : Assets)
        {
            if (ShouldSkipFolderDumpAsset(Data, bIncludeLevels))
            {
                continue;
            }
            const FString PackageName = Data.PackageName.ToString();
            TArray<FAssetData>& Rows = PackageRows.FindOrAdd(PackageName);
            if (Rows.IsEmpty())
            {
                PackageOrder.Add(PackageName);
            }
            Rows.Add(Data);
        }

        TArray<FPendingDumpEntry> Candidates;
        Candidates.Reserve(PackageOrder.Num());
        for (const FString& PackageName : PackageOrder)
        {
            const TArray<FAssetData>& Rows = PackageRows.FindChecked(PackageName);
            const FAssetData Primary = SelectPrimaryAssetData(Rows, PackageName);

            FPendingDumpEntry Entry;
            Entry.ObjectPath = MakePendingDumpPath(Primary);
            Entry.PackageName = PackageName;
            Entry.ClassPath = Primary.AssetClassPath;
            for (const FAssetData& Row : Rows)
            {
                Entry.bIsMapOrWorld = Entry.bIsMapOrWorld || IsMapOrWorldFolderAssetData(Row);
            }
            Candidates.Add(MoveTemp(Entry));
        }

        Result.AssetCount = Candidates.Num();

        if (Candidates.Num() == 0)
        {
            ReconcileMirrorSubtree(RootDir, TSet<FString>{});
            return Result;
        }

        TArray<FPendingDumpEntry> Pending;
        Pending.Reserve(Candidates.Num());
        TSet<FString> LiveDumpDirs;
        int32 UnchangedCount = 0;
        for (FPendingDumpEntry& Candidate : Candidates)
        {
            FString DumpDir;
            const bool bCacheFresh = !bForce && AssetDumpCache::IsDumpFresh(
                AR,
                Candidate.PackageName,
                OutRoot,
                bIncludeWidgetScreenshot,
                Candidate.bIsMapOrWorld,
                DumpDir);
            if (bCacheFresh)
            {
                LiveDumpDirs.Add(FPaths::ConvertRelativePathToFull(DumpDir));
                ++UnchangedCount;
                continue;
            }
            Pending.Add(MoveTemp(Candidate));
        }

        SortPendingAssetsForProcessing(Pending);

        Result.QueuedCount = Pending.Num();
        Result.UnchangedCount = UnchangedCount;

        State.RootDir    = RootDir;
        State.Kind       = EAsyncAssetDumpKind::Folder;
        State.FolderPath = FolderPath;
        State.OutRoot    = OutRoot;
        State.StartedAt  = StartedAt;
        State.bInProgress = true;
        State.bDiff = false;
        State.bIncludeWidgetScreenshot = bIncludeWidgetScreenshot;
        State.TotalAssetCount = Candidates.Num();
        State.QueuedCount = Pending.Num();
        State.DumpedCount = 0;
        State.UnchangedCount = UnchangedCount;
        State.SkipCount = 0;
        State.PendingAssets = MoveTemp(Pending);
        State.LiveDumpDirs = MoveTemp(LiveDumpDirs);
        State.BaselineDirty = DumpDirtyGuard_Snapshot();
        State.WorkStartedSeconds = FPlatformTime::Seconds();
        BeginAsyncDumpPhase(State, TEXT("queued"));
        State.TickerHandle =
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickFolderDump));

        return Result;
    }

    bool AttachJobTicketToAsyncDump(const FString& TicketId)
    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();
        if (TicketId.IsEmpty() || !State.bInProgress || !State.JobTicketId.IsEmpty())
        {
            return false;
        }

        State.JobTicketId = TicketId;
        FPluginState::Get().GetJobRegistry().SetCancelCallback(
            TicketId,
            [TicketId]()
            {
                CancelAsyncDump(TicketId);
            });
        StartAsyncDumpNotification(State);
        return true;
    }

    FFolderDumpStatus GetAsyncFolderDumpStatus()
    {
        const FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();
        FFolderDumpStatus Status;
        Status.bInProgress = State.bInProgress || !State.JobTicketId.IsEmpty();
        if (Status.bInProgress)
        {
            Status.RootDir    = State.RootDir;
            Status.FolderPath = State.FolderPath;
            Status.AssetPath  = State.AssetPath;
            Status.StartedAt  = State.StartedAt;
            Status.CurrentAsset = State.CurrentAsset;
            Status.CurrentPhase = State.CurrentPhase;
            Status.CurrentPhaseStartedAt = State.CurrentPhaseStartedAt;
            Status.CurrentPhaseElapsedSeconds = State.CurrentPhaseStartedSeconds > 0.0
                ? FMath::Max(0.0, FPlatformTime::Seconds() - State.CurrentPhaseStartedSeconds)
                : 0.0;
            Status.LastAssetElapsedSeconds = State.LastAssetElapsedSeconds;
        }
        return Status;
    }

    void BuildWidgetTreeAspect_Internal(
        UWidgetBlueprint* WBP,
        TArray<AssetDumpWriter::FDumpFile>& OutFiles,
        TArray<TPair<FString, FString>>* OutFileErrors)
    {
        WidgetXmlExporter::FWidgetTreeXmlResult TreeResult =
            WidgetXmlExporter::BuildWidgetTreeXmlWithDiagnostic(WBP, /*bIncludeDefaults=*/false, /*bOmitSlotChain=*/true);
        if (!TreeResult.Xml.IsEmpty())
        {
            OutFiles.Add({DumpFileNames::TreeXml, TreeResult.Xml});
        }
        else if (TreeResult.bEmptyByDesign)
        {
            OutFiles.Add({DumpFileNames::TreeXml,
                TEXT("<!-- empty: WidgetTree has no RootWidget on this WBP -->\n")});
        }
        else
        {
            RecordAspectDiagnostic(WBP ? WBP->GetPathName() : FString(), DumpFileNames::TreeXml,
                {TreeResult.Reason}, OutFileErrors);
        }
    }

    void BuildRedirectorPropertiesAspect_Internal(
        UObject* Asset,
        TArray<AssetDumpWriter::FDumpFile>& OutFiles,
        TArray<TPair<FString, FString>>* OutFileErrors)
    {
        UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset);
        if (!Redirector)
        {
            return;
        }

        // properties.json carries only the redirect target. The deep property walk
        // would yield an empty `{}` and obscure the fact that this is a redirector.
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetStringField(TEXT("redirectsTo"),
            AssetDumpBuilder::ResolveRedirectorTarget(Redirector));

        OutFiles.Add({DumpFileNames::Properties,
            SortedJsonWriter::SerializeSortedJsonObject(Properties)});
    }

    void BuildBlueprintPropertiesAspect_Internal(
        UBlueprint* BP,
        TArray<AssetDumpWriter::FDumpFile>& OutFiles,
        TArray<TPair<FString, FString>>* OutFileErrors,
        FBlueprintPropertiesAspectStatus* OutPropertiesStatus)
    {
        if (!BP)
        {
            return;
        }

        UObject* CDO = nullptr;
        UObject* ParentCDO = nullptr;
        if (BP->GeneratedClass)
        {
            CDO = BP->GeneratedClass->GetDefaultObject();
            if (BP->GeneratedClass->GetSuperClass())
            {
                ParentCDO = BP->GeneratedClass->GetSuperClass()->GetDefaultObject();
            }
        }

        TSharedPtr<FJsonObject> PropertiesObj = CDO
            ? BuildClassPropertyJson(CDO, ParentCDO)
            : nullptr;
        const int32 PropertyCount = PropertiesObj.IsValid() ? PropertiesObj->Values.Num() : 0;

        if (PropertiesObj.IsValid())
        {
            OutFiles.Add({DumpFileNames::Properties,
                SortedJsonWriter::SerializeSortedJsonObject(PropertiesObj)});
        }

        if (OutPropertiesStatus)
        {
            OutPropertiesStatus->PropertyCount = PropertyCount;
            if (!CDO)
            {
                OutPropertiesStatus->Status = TEXT("error");
                OutPropertiesStatus->Reason = TEXT("generated_class_missing");
            }
            else if (PropertyCount > 0)
            {
                OutPropertiesStatus->Status = TEXT("ok");
            }
            else if (IsNoUPropertyBlueprintType(BP))
            {
                OutPropertiesStatus->Status = TEXT("empty");
                OutPropertiesStatus->Reason = TEXT("no_uproperties_on_blueprint_type");
                OutPropertiesStatus->BlueprintType = BlueprintTypeToStatusString(BP);
            }
            else
            {
                OutPropertiesStatus->Status = TEXT("empty");
                OutPropertiesStatus->Reason = TEXT("no_overrides");
            }
        }

        if (!CDO)
        {
            RecordAspectDiagnostic(
                BP->GetPathName(),
                DumpFileNames::Properties,
                {TEXT("Blueprint has no GeneratedClass; CDO unavailable.")},
                OutFileErrors);
        }
    }
} // namespace AssetDumpHandler

REGISTER_RPC_HANDLER("asset.dump", "asset",
    "Dump an asset's full inspectable state to disk as separate text files "
    "(meta.json, properties.json, mgir.txt, tree.xml, widget_animations.json, bpir.txt, scs.json, scs.txt, "
    "anim_graph.json, agir.txt, data_table.json, world_settings.json, level_bp.txt, sublevels.json, actors/manifest.json, "
    "map_references.json and actors/*.json as applicable; preview.png with includeWidgetScreenshot=true). Non-level assets return synchronously; "
    "UWorld / .umap assets return a job ticket and complete through system.job_status.",
    RPC_PARAMS(
        AssetPathParamUtils::AssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Package path (e.g. /Game/UI/WBP_HUD). Alias: path.")),
        RPC_PARAM_OPT("outRoot",   "filepath", "Override dump root directory; relative paths resolve against the project dir (default <ProjectSavedDir>/PinWright/asset-dumps; configurable in Project Settings -> Plugins -> PinWright (Project))"),
        RPC_PARAM_DEF("diff",      "boolean", "Diff mode: keep the baseline mirror untouched; write meta.json plus _new and _diff files for changed aspects under <ProjectSavedDir>/PinWright/asset-dump-diffs/<PackagePath>.", "false"),
        RPC_PARAM_DEF("includeWidgetScreenshot", "boolean", "Render the Widget Blueprint Designer preview to preview.png inside the dump folder. Off by default — opening designer + Slate pump per asset is expensive.", "false")
    ))
{
    // Synchronous (non-level) dumps LoadObject the asset, which dirties packages
    // via PostLoad; restore on scope exit so the editor never prompts to save.
    // World assets take the async branch below and are covered by the per-tick
    // baseline restore instead.
    FScopedDumpDirtyRestore DirtyGuard;

    FString AssetPathRaw;
    if (!AssetPathParamUtils::RequireAssetPathRaw(Ctx, AssetPathRaw)) return true;

    FString OutRoot = Ctx.GetString(TEXT("outRoot"), FString());
    bool bDiff = Ctx.GetBool(TEXT("diff"), false);
    bool bIncludeWidgetScreenshot = Ctx.GetBool(TEXT("includeWidgetScreenshot"), false);

    FString NormalizeErr;
    FString NormalizedPath = TryResolveAssetPath(AssetPathRaw, nullptr, &NormalizeErr);
    if (NormalizedPath.IsEmpty())
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not resolve asset path '%s': %s"),
                *AssetPathRaw, *NormalizeErr));
        return true;
    }

    if (AssetDumpHandler::IsWorldAssetPath(NormalizedPath))
    {
        AssetDumpHandler::FSingleAssetDumpStart R =
            AssetDumpHandler::StartAsyncSingleAssetDump(NormalizedPath, OutRoot, bDiff, bIncludeWidgetScreenshot);

        if (!R.ErrorCode.IsEmpty())
        {
            Ctx.SendError(R.ErrorCode, R.ErrorMessage);
            return true;
        }

        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();

        FJobBindArgs Args;
        Args.Method = TEXT("asset.dump");
        Args.StartedPayload = MakeShared<FJsonObject>();
        Args.StartedPayload->SetStringField(TEXT("assetPath"), R.AssetPath);
        Args.StartedPayload->SetStringField(TEXT("dumpDir"), R.DumpDir);
        Args.StartedPayload->SetStringField(TEXT("mode"), bDiff ? TEXT("diff") : TEXT("dump"));
        Args.StartedPayload->SetBoolField(TEXT("diff"), bDiff);
        Args.StartedPayload->SetStringField(TEXT("currentAsset"), State.CurrentAsset);
        Args.StartedPayload->SetStringField(TEXT("currentPhase"), State.CurrentPhase);
        Args.StartedPayload->SetStringField(TEXT("phaseStartedAt"), State.CurrentPhaseStartedAt.ToIso8601());
        Args.StartedPayload->SetStringField(TEXT("message"),
            TEXT("Level dump STARTED asynchronously in the background — this response means started, not done. "
                 "Large levels can take several minutes. To wait, poll system.job_status with this 'ticket_id' "
                 "(its reply carries 'status'), or match the terminal line in the JSONL at 'monitor_path' on "
                 "\"event\":\"completed\" (also \"failed\" / \"cancelled\"). The JSONL has NO 'status' field — "
                 "matching \"status\":\"completed\" there never fires."));
        // The ticker drives completion via FJobRegistry::Complete — no native delegate needed.
        Args.BindNativeDelegate = [](FJobOnComplete /*OnComplete*/) {};

        const FString TicketId = Ctx.StartJob(Args);
        if (!AssetDumpHandler::AttachJobTicketToAsyncDump(TicketId))
        {
            FPluginState::Get().GetJobRegistry().Complete(
                TicketId, false, nullptr, TEXT("Failed to attach the async dump cancellation callback."));
        }
        return true;
    }

    // NormalizeAssetPath collapses object paths ("/Game/Pkg.Inner") to the bare
    // package name for every caller; asset.dump must not lose the inner object.
    // Multi-asset packages (e.g. "Bake Out Materials" GUID outputs) have no object
    // named after the package tail, so the collapsed path cannot load. Re-attach a
    // caller-provided explicit suffix; bare package paths resolve to the
    // deterministic primary registry row instead. Trailing '.uasset'/'.umap' are
    // file extensions, not object names, and keep the collapsed-package behavior.
    FString ObjectPathToLoad = NormalizedPath;
    {
        FString CleanRaw = AssetPathRaw;
        while (CleanRaw.EndsWith(TEXT("/")))
        {
            CleanRaw.RemoveAt(CleanRaw.Len() - 1);
        }
        int32 SuffixDot = INDEX_NONE;
        if (CleanRaw.FindChar(TEXT('.'), SuffixDot)
            && !CleanRaw.EndsWith(TEXT(".uasset"))
            && !CleanRaw.EndsWith(TEXT(".umap")))
        {
            ObjectPathToLoad = NormalizedPath + CleanRaw.Mid(SuffixDot);
        }
        else
        {
            FAssetRegistryModule& ARM =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FAssetData> PackageAssets;
            ARM.Get().GetAssetsByPackageName(
                FName(*NormalizedPath), PackageAssets, /*bIncludeOnlyOnDiskAssets=*/true);
            if (PackageAssets.Num() > 0)
            {
                ObjectPathToLoad = AssetDumpHandler::SelectPrimaryAssetData(
                    PackageAssets, NormalizedPath).GetObjectPathString();
            }
        }
    }

    AssetDumpHandler::FDumpSingleResult SingleResult =
        AssetDumpHandler::DumpSingleAsset(
            ObjectPathToLoad,
            OutRoot,
            bDiff,
            bIncludeWidgetScreenshot,
            DirtyGuard.Baseline,
            /*bDeferAsyncCompilation=*/true);

    if (SingleResult.bDeferredForCompilation)
    {
        Ctx.SendError(TEXT("ASSET_COMPILING"),
            FString::Printf(
                TEXT("Asset '%s' is still compiling platform data. Retry asset.dump after compilation finishes."),
                *ObjectPathToLoad));
        return true;
    }

    if (!SingleResult.ErrorCode.IsEmpty())
    {
        Ctx.SendError(SingleResult.ErrorCode, SingleResult.ErrorMessage);
        return true;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> WrittenArr;
    for (const FString& P : SingleResult.WrittenPaths)
    {
        WrittenArr.Add(MakeShared<FJsonValueString>(P));
    }
    ResultObj->SetArrayField(TEXT("writtenPaths"), WrittenArr);

    TArray<TSharedPtr<FJsonValue>> SkippedArr;
    for (const TPair<FString, FString>& Pair : SingleResult.FileErrors)
    {
        TSharedPtr<FJsonObject> SkipEntry = MakeShared<FJsonObject>();
        SkipEntry->SetStringField(TEXT("path"),   Pair.Key);
        SkipEntry->SetStringField(TEXT("reason"), Pair.Value);
        SkippedArr.Add(MakeShared<FJsonValueObject>(SkipEntry));
    }
    ResultObj->SetArrayField(TEXT("skipped"), SkippedArr);
    ResultObj->SetStringField(TEXT("dumpDir"), SingleResult.DumpDir);
    ResultObj->SetStringField(TEXT("mode"),    SingleResult.Mode);

    // ---- widgetPreview ----
    // Present ONLY when the widget-preview aspect actually ran: includeWidgetScreenshot=true,
    // the asset is a UWidgetBlueprint, and the capture succeeded. Every other dump has nothing
    // to say here, and a block of false/0 on a texture dump would be noise in the response of
    // the single most-called verb in the plugin. A capture that FAILED already reports itself
    // in `skipped` with preview.png as the path, so the absence is never silent.
    //
    // The block exists because this capture is now stamped opaque before encoding, which
    // destroys the widget's real transparency. `alphaZeroFraction` is the only surviving
    // record of how much there was -- a caller compositing preview.png over a background
    // needs it to know whether the opaque rectangle it now gets is the widget or the
    // widget plus a stamped-over hole. Measured pre-stamp inside the util; unrecoverable
    // from the file.
    if (SingleResult.bWidgetPreviewCaptured)
    {
        TSharedPtr<FJsonObject> WidgetPreview = MakeShared<FJsonObject>();
        WidgetPreview->SetStringField(TEXT("file"), DumpFileNames::WidgetPreviewPng);
        WidgetPreview->SetBoolField(TEXT("opaqueStamped"), SingleResult.bWidgetPreviewOpaqueStamped);
        if (SingleResult.bWidgetPreviewOpaqueStamped)
        {
            // Omitted rather than zeroed when nothing was stamped: the fraction is measured
            // by the stamp pass, so with no stamp there is no measurement, and a 0.0 here
            // would read as "measured, fully opaque" -- a different claim entirely. Same
            // shape as viewport.exposure's adaptedMeasured/adapted pair.
            WidgetPreview->SetNumberField(TEXT("alphaZeroFraction"),
                SingleResult.WidgetPreviewAlphaZeroFraction);
        }
        ResultObj->SetObjectField(TEXT("widgetPreview"), WidgetPreview);
    }

    Ctx.SendSuccess(ResultObj);
    return true;
}

REGISTER_RPC_HANDLER("asset.dump_folder", "asset",
    "Start an async dump of every asset under a content folder. Streaming MCP calls block and "
    "stream progress by default; pass wait=false to return a ticket immediately, then poll "
    "system.job_status with the ticket_id. "
    "Progress identifies the asset and phase before synchronous engine work begins; cancellation "
    "stops at cooperative asset boundaries. Uses private .dumpcache.json metadata to skip unchanged "
    "assets unless force=true.",
    RPC_PARAMS(
        RPC_PARAM_REQ("folderPath",    "path",  "Content folder (e.g. /Game/UI)"),
        RPC_PARAM_DEF("recursive",     "boolean", "Recurse into subfolders", "true"),
        RPC_PARAM_OPT("outRoot",       "filepath",  "Override dump root; relative paths resolve against the project dir"),
        RPC_PARAM_DEF("includeLevels", "boolean", "Include level (.umap / UWorld) assets in the dump. Levels are slow to dump and are skipped by default; set true to restore the legacy include-everything behavior.", "false"),
        RPC_PARAM_DEF("includeWidgetScreenshot", "boolean", "Render the Widget Blueprint Designer preview to preview.png inside the dump folder. Off by default — opening designer + Slate pump per asset is expensive.", "false"),
        RPC_PARAM_DEF("force", "boolean", "Ignore .dumpcache.json freshness and re-dump all matching assets.", "false")
    ))
{
    FString FolderPath;
    if (!Ctx.RequireString(TEXT("folderPath"), FolderPath)) return true;

    const bool bRecursive     = Ctx.GetBool(TEXT("recursive"), true);
    const FString OutRoot     = Ctx.GetString(TEXT("outRoot"), FString());
    const bool bIncludeLevels = Ctx.GetBool(TEXT("includeLevels"), false);
    const bool bIncludeWidgetScreenshot = Ctx.GetBool(TEXT("includeWidgetScreenshot"), false);
    const bool bForce = Ctx.GetBool(TEXT("force"), false);

    AssetDumpHandler::FFolderDumpStart R =
        AssetDumpHandler::StartAsyncFolderDump(
            FolderPath,
            bRecursive,
            OutRoot,
            bIncludeLevels,
            bIncludeWidgetScreenshot,
            bForce,
            /*bDeferPreflight=*/true);

    if (!R.ErrorCode.IsEmpty())
    {
        Ctx.SendError(R.ErrorCode, R.ErrorMessage);
        return true;
    }

    {
        FAsyncFolderDumpState& State = FPluginState::Get().GetFolderDump();

        FJobBindArgs Args;
        Args.Method = TEXT("asset.dump_folder");
        Args.StartedPayload = MakeShared<FJsonObject>();
        Args.StartedPayload->SetStringField(TEXT("rootDir"),    State.RootDir);
        Args.StartedPayload->SetStringField(TEXT("folderPath"), State.FolderPath);
        Args.StartedPayload->SetNumberField(TEXT("assetCount"), State.TotalAssetCount);
        Args.StartedPayload->SetBoolField(TEXT("assetCountKnown"), false);
        Args.StartedPayload->SetNumberField(TEXT("queued"), State.QueuedCount);
        Args.StartedPayload->SetNumberField(TEXT("unchanged"), State.UnchangedCount);
        Args.StartedPayload->SetStringField(TEXT("currentAsset"), State.CurrentAsset);
        Args.StartedPayload->SetStringField(TEXT("currentPhase"), State.CurrentPhase);
        Args.StartedPayload->SetStringField(TEXT("phaseStartedAt"), State.CurrentPhaseStartedAt.ToIso8601());
        Args.StartedPayload->SetStringField(TEXT("message"),
            FString::Printf(
                TEXT("Folder dump STARTED asynchronously in the background — this response means started, not done. "
                     "Discovering assets under '%s'; registry discovery and cache checks report progress before dumping. "
                     "Sweeps run in small per-tick batches; expect tens of seconds for a project-sized subtree "
                     "(measured: 1890 assets / 1838 dumped in ~39s), minutes only with includeLevels=true. "
                     "A ticket means the call did not block: on a streaming client, omitting 'wait' returns the "
                     "finished result and needs no waiter. To wait on this ticket, poll system.job_status with this "
                     "'ticket_id' (its reply carries 'status'), or match the terminal line in the JSONL at "
                     "'monitor_path' on \"event\":\"completed\" (also \"failed\" / \"cancelled\"); 'ticket_id' and "
                     "'event' are on the same line. The JSONL has NO 'status' field — matching "
                     "\"status\":\"completed\" there never fires."),
                *State.FolderPath));
        // The ticker drives completion via FJobRegistry::Complete — no native delegate needed.
        Args.BindNativeDelegate = [](FJobOnComplete /*OnComplete*/) {};

        const FString TicketId = Ctx.StartJob(Args);
        if (!AssetDumpHandler::AttachJobTicketToAsyncDump(TicketId))
        {
            FPluginState::Get().GetJobRegistry().Complete(
                TicketId, false, nullptr, TEXT("Failed to attach the async dump cancellation callback."));
        }
    }
    return true;
}
