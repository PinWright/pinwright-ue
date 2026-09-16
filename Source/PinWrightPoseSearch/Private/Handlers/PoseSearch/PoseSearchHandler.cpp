// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightHelpers.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#if __has_include("PoseSearch/PoseSearchSchema.h") && __has_include("PoseSearch/PoseSearchDatabase.h") && __has_include("PoseSearch/PoseSearchFeatureChannel_Position.h")
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchSchema.h"
#define MCP_HAS_POSESEARCH 1
#else
#define MCP_HAS_POSESEARCH 0
#endif

#define LOCTEXT_NAMESPACE "PoseSearchHandler"

namespace PinWrightPoseSearch
{
namespace
{
FString NormalizeToken(FString Value)
{
    Value.TrimStartAndEndInline();
    Value.ReplaceInline(TEXT("-"), TEXT("_"));
    Value.ReplaceInline(TEXT(" "), TEXT("_"));
    return Value.ToLower();
}

FString NormalizePackagePath(const FString& RequestedPath, FString& OutError)
{
    FString Path = RequestedPath;
    Path.TrimStartAndEndInline();
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
    {
        Path.LeftChopInline(7);
    }

    FNormalizedAssetPath Normalized = NormalizeAssetPath(Path);
    if (!Normalized.bIsValid)
    {
        OutError = Normalized.ErrorMessage;
        return FString();
    }

    const FString SanitizedPath = SanitizeProjectRelativePath(Normalized.Path);
    if (SanitizedPath.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Invalid asset path: %s"), *RequestedPath);
        return FString();
    }
    return SanitizedPath;
}

FString PoseSearchAssetObjectPath(const FString& PackagePath)
{
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
}

bool BuildCreatePaths(FHandlerContext& Ctx, FString& OutPackagePath, FString& OutObjectPath)
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath))
    {
        return false;
    }

    FString NormalizeError;
    OutPackagePath = NormalizePackagePath(AssetPath, NormalizeError);
    if (OutPackagePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            NormalizeError.IsEmpty() ? FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath) : NormalizeError);
        return false;
    }

    FText Reason;
    if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &Reason))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), Reason.ToString());
        return false;
    }

    OutObjectPath = PoseSearchAssetObjectPath(OutPackagePath);
    return true;
}

bool EnsurePoseSearchAvailable(FHandlerContext& Ctx)
{
#if MCP_HAS_POSESEARCH
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("PoseSearch")))
    {
        Ctx.SendError(TEXT("PLUGIN_DISABLED"),
            TEXT("PoseSearch module is not loaded. Enable the PoseSearch plugin before using pose_search.* handlers."));
        return false;
    }
    return true;
#else
    Ctx.SendError(TEXT("PLUGIN_DISABLED"),
        TEXT("PoseSearch headers are not available in this engine build."));
    return false;
#endif
}

#if MCP_HAS_POSESEARCH

template <typename TAsset>
TAsset* LoadTypedAsset(FHandlerContext& Ctx, const FString& RequestedPath, const TCHAR* ErrorCode, const TCHAR* Label)
{
    auto TryResolveTyped = [](const FString& Path) -> TAsset*
    {
        if (Path.IsEmpty())
        {
            return nullptr;
        }
        FString ResolveError;
        UObject* Resolved = ResolveUObjectByPath(Path, ResolveError);
        return Cast<TAsset>(Resolved);
    };

    if (TAsset* LoadedByRequest = TryResolveTyped(RequestedPath))
    {
        return LoadedByRequest;
    }

    FString NormalizeError;
    const FString PackagePath = NormalizePackagePath(RequestedPath, NormalizeError);
    const FString ObjectPath = PackagePath.IsEmpty() ? FString() : PoseSearchAssetObjectPath(PackagePath);
    if (TAsset* LoadedByObjectPath = TryResolveTyped(ObjectPath))
    {
        return LoadedByObjectPath;
    }

    Ctx.SendError(ErrorCode, FString::Printf(TEXT("%s not found: %s"), Label, *RequestedPath));
    return nullptr;
}

// Finalize a pose-search asset: flush editor state via PostEditChange, then (when
// bSave) persist the .uasset to disk for real via SaveAssetToDiskReportingPresence —
// NOT the mark-dirty-only McpSafeAssetSave, which returns true without ever writing a
// package, silently losing the asset on cold restart (B-pose-search-create-save-no-disk-write).
// Returns whether the .uasset actually landed on disk (false when bSave is false).
bool FinishPoseSearchAsset(UObject* Asset, bool bSave)
{
    if (Asset)
    {
        Asset->PostEditChange();
    }
    if (bSave)
    {
        return SaveAssetToDiskReportingPresence(Asset, /*bForce=*/true);
    }
    return false;
}

bool TryGetNumberField(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Keys, float& OutValue)
{
    if (!Object.IsValid())
    {
        return false;
    }

    double Number = 0.0;
    for (const FString& Key : Keys)
    {
        if (Object->TryGetNumberField(Key, Number))
        {
            OutValue = static_cast<float>(Number);
            return true;
        }
    }
    return false;
}

bool ParseSamplingRange(FHandlerContext& Ctx, const TSharedPtr<FJsonValue>& Value, FFloatInterval& OutRange)
{
    OutRange = FFloatInterval(0.f, 0.f);
    if (!Value.IsValid() || Value->IsNull())
    {
        return true;
    }

    const TSharedPtr<FJsonObject>* Object = nullptr;
    if (Value->TryGetObject(Object) && Object && Object->IsValid())
    {
        float Min = 0.f;
        float Max = 0.f;
        TryGetNumberField(*Object, {TEXT("min"), TEXT("start"), TEXT("startTime")}, Min);
        TryGetNumberField(*Object, {TEXT("max"), TEXT("end"), TEXT("endTime")}, Max);
        OutRange = FFloatInterval(Min, Max);
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (Value->TryGetArray(Array) && Array && Array->Num() == 2)
    {
        OutRange = FFloatInterval(
            static_cast<float>((*Array)[0]->AsNumber()),
            static_cast<float>((*Array)[1]->AsNumber()));
        return true;
    }

    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("samplingRange must be {min,max}, {start,end}, or [min,max]."));
    return false;
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
bool BuildSequenceEntry(FHandlerContext& Ctx, const FString& SequencePath, const TSharedPtr<FJsonValue>& SamplingRangeValue, FPoseSearchDatabaseAnimationAsset& OutEntry)
#else
bool BuildSequenceEntry(FHandlerContext& Ctx, const FString& SequencePath, const TSharedPtr<FJsonValue>& SamplingRangeValue, FPoseSearchDatabaseSequence& OutEntry)
#endif
{
    UAnimSequence* Sequence = LoadTypedAsset<UAnimSequence>(Ctx, SequencePath, TEXT("SEQUENCE_NOT_FOUND"), TEXT("Animation sequence"));
    if (!Sequence)
    {
        return false;
    }

    FFloatInterval SamplingRange;
    if (!ParseSamplingRange(Ctx, SamplingRangeValue, SamplingRange))
    {
        return false;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    OutEntry.AnimAsset = Sequence;
#else
    OutEntry.Sequence = Sequence;
#endif
    OutEntry.SamplingRange = SamplingRange;
    return true;
}

bool ExtractSequenceSpec(FHandlerContext& Ctx, const TSharedPtr<FJsonValue>& AnimationValue, FString& OutSequencePath, TSharedPtr<FJsonValue>& OutSamplingRangeValue)
{
    OutSequencePath.Reset();
    OutSamplingRangeValue.Reset();

    if (AnimationValue.IsValid() && AnimationValue->Type == EJson::String)
    {
        OutSequencePath = AnimationValue->AsString();
    }
    else if (AnimationValue.IsValid())
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (AnimationValue->TryGetObject(Object) && Object && Object->IsValid())
        {
            (*Object)->TryGetStringField(TEXT("sequencePath"), OutSequencePath);
            if (OutSequencePath.IsEmpty())
            {
                (*Object)->TryGetStringField(TEXT("animationPath"), OutSequencePath);
            }
            if (OutSequencePath.IsEmpty())
            {
                (*Object)->TryGetStringField(TEXT("assetPath"), OutSequencePath);
            }
            if (OutSequencePath.IsEmpty())
            {
                (*Object)->TryGetStringField(TEXT("path"), OutSequencePath);
            }
            OutSamplingRangeValue = (*Object)->TryGetField(TEXT("samplingRange"));
        }
    }

    if (OutSequencePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Each animations[] entry must be a sequence path or object with sequencePath."));
        return false;
    }
    return true;
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
bool BuildValidatedSequenceEntry(FHandlerContext& Ctx, const UPoseSearchSchema* Schema, const FString& SequencePath, const TSharedPtr<FJsonValue>& SamplingRangeValue, FPoseSearchDatabaseAnimationAsset& OutEntry)
#else
bool BuildValidatedSequenceEntry(FHandlerContext& Ctx, const UPoseSearchSchema* Schema, const FString& SequencePath, const TSharedPtr<FJsonValue>& SamplingRangeValue, FPoseSearchDatabaseSequence& OutEntry)
#endif
{
    if (!Schema)
    {
        Ctx.SendError(TEXT("SCHEMA_NOT_SET"), TEXT("Pose Search database has no schema."));
        return false;
    }

    if (!BuildSequenceEntry(Ctx, SequencePath, SamplingRangeValue, OutEntry))
    {
        return false;
    }

    if (!OutEntry.IsSkeletonCompatible(Schema))
    {
        Ctx.SendError(TEXT("SKELETON_MISMATCH"),
            FString::Printf(TEXT("Animation sequence skeleton is not compatible with the Pose Search schema: %s"), *SequencePath));
        return false;
    }

    return true;
}

bool AddDatabaseAnimation(FHandlerContext& Ctx, UPoseSearchDatabase* Database, const FString& SequencePath, const TSharedPtr<FJsonValue>& SamplingRangeValue, int32& OutIndex)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    FPoseSearchDatabaseAnimationAsset SequenceEntry;
#else
    FPoseSearchDatabaseSequence SequenceEntry;
#endif
    if (!BuildValidatedSequenceEntry(Ctx, Database->Schema, SequencePath, SamplingRangeValue, SequenceEntry))
    {
        return false;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    Database->AddAnimationAsset(SequenceEntry);
#else
    Database->AddAnimationAsset(FInstancedStruct::Make(SequenceEntry));
#endif
    OutIndex = Database->GetNumAnimationAssets() - 1;
    return true;
}

bool AddChannelFromSpec(FHandlerContext& Ctx, UPoseSearchSchema* Schema, const TSharedPtr<FJsonValue>& ChannelValue, int32& OutChannelCount)
{
    FString Type = TEXT("position");
    TSharedPtr<FJsonObject> ChannelObject;

    if (ChannelValue.IsValid() && ChannelValue->Type == EJson::String)
    {
        Type = ChannelValue->AsString();
    }
    else if (ChannelValue.IsValid())
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (ChannelValue->TryGetObject(Object) && Object && Object->IsValid())
        {
            ChannelObject = *Object;
            ChannelObject->TryGetStringField(TEXT("type"), Type);
            if (Type.IsEmpty())
            {
                ChannelObject->TryGetStringField(TEXT("kind"), Type);
            }
        }
    }

    const FString NormalizedType = NormalizeToken(Type);
    if (NormalizedType != TEXT("position"))
    {
        Ctx.SendError(TEXT("UNSUPPORTED_CHANNEL"),
            FString::Printf(TEXT("Unsupported Pose Search channel '%s'. Supported channels: Position."), *Type));
        return false;
    }

    UPoseSearchFeatureChannel_Position* Channel = NewObject<UPoseSearchFeatureChannel_Position>(
        Schema, NAME_None, RF_Transactional);
    if (!Channel)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Pose Search Position channel."));
        return false;
    }

    if (ChannelObject.IsValid())
    {
        FString BoneName;
        if (ChannelObject->TryGetStringField(TEXT("bone"), BoneName) || ChannelObject->TryGetStringField(TEXT("boneName"), BoneName))
        {
            Channel->Bone.BoneName = FName(*BoneName);
        }

        FString OriginBoneName;
        if (ChannelObject->TryGetStringField(TEXT("originBone"), OriginBoneName) || ChannelObject->TryGetStringField(TEXT("originBoneName"), OriginBoneName))
        {
            Channel->OriginBone.BoneName = FName(*OriginBoneName);
        }

        double Number = 0.0;
        if (ChannelObject->TryGetNumberField(TEXT("sampleTimeOffset"), Number))
        {
            Channel->SampleTimeOffset = static_cast<float>(Number);
        }
        if (ChannelObject->TryGetNumberField(TEXT("originTimeOffset"), Number))
        {
            Channel->OriginTimeOffset = static_cast<float>(Number);
        }
        if (ChannelObject->TryGetNumberField(TEXT("weight"), Number))
        {
            Channel->Weight = static_cast<float>(Number);
        }
    }

    Schema->AddChannel(Channel);
    ++OutChannelCount;
    return true;
}

void AddPoseSearchAssetFields(TSharedPtr<FJsonObject> Result, UObject* Asset, const FString& ClassName)
{
    Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
    Result->SetStringField(TEXT("className"), ClassName);
    AddAssetVerification(Result, Asset);
}

bool HandleCreateSchema(FHandlerContext& Ctx)
{
    if (!EnsurePoseSearchAvailable(Ctx))
    {
        return true;
    }

    FString PackagePath;
    FString ObjectPath;
    if (!BuildCreatePaths(Ctx, PackagePath, ObjectPath))
    {
        return true;
    }

    FString SkeletonPath;
    if (!Ctx.RequireString(TEXT("skeleton"), SkeletonPath))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
    if (!Ctx.RequireArray(TEXT("channels"), Channels))
    {
        return true;
    }

    if (FindObject<UPoseSearchSchema>(nullptr, *ObjectPath) || LoadObject<UPoseSearchSchema>(nullptr, *ObjectPath))
    {
        Ctx.SendError(TEXT("ALREADY_EXISTS"), FString::Printf(TEXT("Pose Search schema already exists: %s"), *ObjectPath));
        return true;
    }

    USkeleton* Skeleton = LoadTypedAsset<USkeleton>(Ctx, SkeletonPath, TEXT("SKELETON_NOT_FOUND"), TEXT("Skeleton"));
    if (!Skeleton)
    {
        return true;
    }

    // PackagePath is safe to hand to CreatePackage because BuildCreatePaths already ran
    // SanitizeProjectRelativePath (collapses "//") AND FPackageName::IsValidLongPackageName
    // (rejects "//", empty/too-short, missing leading slash, trailing slash, invalid chars) over
    // the WHOLE caller-supplied assetPath. Both Fatal inputs of CreatePackage
    // (UObjectGlobals.cpp:1094-1096 and :1118) are therefore unreachable from here. Do not move
    // this call above BuildCreatePaths, and do not compose a package path from any other caller
    // string without the same guard - board B-createpackage-unvalidated-paths-plugin-wide.
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
        return true;
    }

    // Scope the undo transaction to creation + mutation only, and close it BEFORE the disk
    // save (matching the Niagara/Audio/Material sibling save-fix convention). Writing the
    // .uasset inside an open transaction would let a later Undo of "Create Pose Search
    // Schema" destroy the in-memory object while the written file stays orphaned on disk.
    UPoseSearchSchema* Schema = nullptr;
    {
        const FScopedTransaction Transaction(LOCTEXT("CreatePoseSearchSchema", "Create Pose Search Schema"));
        Schema = NewObject<UPoseSearchSchema>(
            Package,
            UPoseSearchSchema::StaticClass(),
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Schema)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Pose Search schema."));
            return true;
        }

        Schema->Modify(true);
        Schema->AddSkeleton(Skeleton);

        int32 RequestedChannelCount = 0;
        for (const TSharedPtr<FJsonValue>& ChannelValue : *Channels)
        {
            if (!AddChannelFromSpec(Ctx, Schema, ChannelValue, RequestedChannelCount))
            {
                return true;
            }
        }

        FinishPoseSearchAsset(Schema, false);
        if (RequestedChannelCount > 0 && Schema->GetChannels().Num() == 0)
        {
            Ctx.SendError(TEXT("SCHEMA_FINALIZE_FAILED"), TEXT("Pose Search schema did not finalize any channels."));
            return true;
        }
    }

    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);
    bool bSavedToDisk = false;
    if (bSaveRequested)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(Schema, /*bForce=*/true);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddPoseSearchAssetFields(Result, Schema, TEXT("UPoseSearchSchema"));
    Result->SetNumberField(TEXT("skeletonCount"), Schema->GetRoledSkeletons().Num());
    Result->SetNumberField(TEXT("channelCount"), Schema->GetChannels().Num());
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk);
    Ctx.SendSuccess(Result);
    return true;
}

bool HandleCreateDatabase(FHandlerContext& Ctx)
{
    if (!EnsurePoseSearchAvailable(Ctx))
    {
        return true;
    }

    FString PackagePath;
    FString ObjectPath;
    if (!BuildCreatePaths(Ctx, PackagePath, ObjectPath))
    {
        return true;
    }

    FString SchemaPath;
    if (!Ctx.RequireString(TEXT("schema"), SchemaPath))
    {
        return true;
    }

    if (FindObject<UPoseSearchDatabase>(nullptr, *ObjectPath) || LoadObject<UPoseSearchDatabase>(nullptr, *ObjectPath))
    {
        Ctx.SendError(TEXT("ALREADY_EXISTS"), FString::Printf(TEXT("Pose Search database already exists: %s"), *ObjectPath));
        return true;
    }

    UPoseSearchSchema* Schema = LoadTypedAsset<UPoseSearchSchema>(Ctx, SchemaPath, TEXT("SCHEMA_NOT_FOUND"), TEXT("Pose Search schema"));
    if (!Schema)
    {
        return true;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    TArray<FPoseSearchDatabaseAnimationAsset> PrevalidatedAnimations;
#else
    TArray<FPoseSearchDatabaseSequence> PrevalidatedAnimations;
#endif
    if (const TArray<TSharedPtr<FJsonValue>>* Animations = Ctx.GetArray(TEXT("animations")))
    {
        PrevalidatedAnimations.Reserve(Animations->Num());
        for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
        {
            FString SequencePath;
            TSharedPtr<FJsonValue> SamplingRangeValue;
            if (!ExtractSequenceSpec(Ctx, AnimationValue, SequencePath, SamplingRangeValue))
            {
                return true;
            }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
            FPoseSearchDatabaseAnimationAsset SequenceEntry;
#else
            FPoseSearchDatabaseSequence SequenceEntry;
#endif
            if (!BuildValidatedSequenceEntry(Ctx, Schema, SequencePath, SamplingRangeValue, SequenceEntry))
            {
                return true;
            }
            PrevalidatedAnimations.Add(SequenceEntry);
        }
    }

    // Guarded the same way HandleCreateSchema's CreatePackage is: BuildCreatePaths validated the
    // whole caller-supplied assetPath with SanitizeProjectRelativePath +
    // FPackageName::IsValidLongPackageName before this point, so neither CreatePackage Fatal
    // (double slash, empty name) is reachable here. See the note at the schema call site.
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
        return true;
    }

    // Scope the undo transaction to creation + mutation only; finalize + save (via
    // FinishPoseSearchAsset) AFTER it closes so a later Undo cannot orphan the written
    // .uasset — matching the Niagara/Audio/Material sibling save-fix convention.
    UPoseSearchDatabase* Database = nullptr;
    {
        const FScopedTransaction Transaction(LOCTEXT("CreatePoseSearchDatabase", "Create Pose Search Database"));
        Database = NewObject<UPoseSearchDatabase>(
            Package,
            UPoseSearchDatabase::StaticClass(),
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Database)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Pose Search database."));
            return true;
        }

        Database->Modify(true);
        Database->Schema = Schema;

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        for (const FPoseSearchDatabaseAnimationAsset& SequenceEntry : PrevalidatedAnimations)
        {
            Database->AddAnimationAsset(SequenceEntry);
        }
#else
        for (const FPoseSearchDatabaseSequence& SequenceEntry : PrevalidatedAnimations)
        {
            Database->AddAnimationAsset(FInstancedStruct::Make(SequenceEntry));
        }
#endif
    }

    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);
    const bool bSavedToDisk = FinishPoseSearchAsset(Database, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddPoseSearchAssetFields(Result, Database, TEXT("UPoseSearchDatabase"));
    Result->SetStringField(TEXT("schemaPath"), Schema->GetPathName());
    Result->SetNumberField(TEXT("animationCount"), Database->GetNumAnimationAssets());
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk);
    Ctx.SendSuccess(Result);
    return true;
}

bool HandleAddDatabaseAnimation(FHandlerContext& Ctx)
{
    if (!EnsurePoseSearchAvailable(Ctx))
    {
        return true;
    }

    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    FString SequencePath;
    if (!Ctx.RequireString(TEXT("sequencePath"), SequencePath))
    {
        return true;
    }

    UPoseSearchDatabase* Database = LoadTypedAsset<UPoseSearchDatabase>(Ctx, AssetPath, TEXT("DATABASE_NOT_FOUND"), TEXT("Pose Search database"));
    if (!Database)
    {
        return true;
    }

    // Scope the undo transaction to the mutation only; finalize + save (via
    // FinishPoseSearchAsset) AFTER it closes so a later Undo cannot orphan the written
    // .uasset — matching the Niagara/Audio/Material sibling save-fix convention.
    int32 AnimationIndex = INDEX_NONE;
    {
        const FScopedTransaction Transaction(LOCTEXT("AddPoseSearchDatabaseAnimation", "Add Pose Search Database Animation"));
        Database->Modify(true);

        if (!AddDatabaseAnimation(Ctx, Database, SequencePath, Ctx.GetJsonValueFirstOf({TEXT("samplingRange")}), AnimationIndex))
        {
            return true;
        }
    }

    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);
    const bool bSavedToDisk = FinishPoseSearchAsset(Database, bSaveRequested);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddPoseSearchAssetFields(Result, Database, TEXT("UPoseSearchDatabase"));
    Result->SetNumberField(TEXT("animationIndex"), AnimationIndex);
    Result->SetNumberField(TEXT("animationCount"), Database->GetNumAnimationAssets());
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk);
    Ctx.SendSuccess(Result);
    return true;
}

#else

bool HandleCreateSchema(FHandlerContext& Ctx)
{
    EnsurePoseSearchAvailable(Ctx);
    return true;
}

bool HandleCreateDatabase(FHandlerContext& Ctx)
{
    EnsurePoseSearchAvailable(Ctx);
    return true;
}

bool HandleAddDatabaseAnimation(FHandlerContext& Ctx)
{
    EnsurePoseSearchAvailable(Ctx);
    return true;
}

#endif
} // namespace
} // namespace PinWrightPoseSearch

REGISTER_RPC_HANDLER("pose_search.create_schema", "pose_search",
    "Create a UPoseSearchSchema asset, bind its skeleton, and append supported feature channels.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Full package or object path for the new Pose Search schema asset."),
        RPC_PARAM_REQ("skeleton", "path", "Skeleton asset path."),
        RPC_PARAM_REQ("channels", "array", "Channel specs. Supported kind: Position."),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk after creation.", "true")
    ))
{
    return PinWrightPoseSearch::HandleCreateSchema(Ctx);
}

REGISTER_RPC_HANDLER("pose_search.create_database", "pose_search",
    "Create a UPoseSearchDatabase asset, bind a schema, and optionally add sequence entries.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Full package or object path for the new Pose Search database asset."),
        RPC_PARAM_REQ("schema", "path", "Pose Search schema asset path."),
        RPC_PARAM_OPT("animations", "array", "Optional sequence paths or {sequencePath, samplingRange} objects."),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk after creation.", "true")
    ))
{
    return PinWrightPoseSearch::HandleCreateDatabase(Ctx);
}

REGISTER_RPC_HANDLER("pose_search.add_database_animation", "pose_search",
    "Append a UAnimSequence entry to an existing UPoseSearchDatabase.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Pose Search database asset path."),
        RPC_PARAM_REQ("sequencePath", "path", "Animation sequence asset path."),
        RPC_PARAM_OPT("samplingRange", "object|array", "Optional {min,max}, {start,end}, or [min,max] seconds. Defaults to [0,0], meaning the full sequence."),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk after mutation.", "true")
    ))
{
    return PinWrightPoseSearch::HandleAddDatabaseAnimation(Ctx);
}

#undef LOCTEXT_NAMESPACE
