// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "PinWrightHelpers.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/PathUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

#include "PCGGraph.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

REGISTER_RPC_HANDLER("pcg.create_graph", "pcg",
    "Create a new UPCGGraph asset at the given content path. graphClass selects a UPCGGraph "
    "subclass (e.g. an engine-plugin graph type) instead of the stock UPCGGraph.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name (without path)."),
        RPC_PARAM_REQ("savePath", "path", "Content folder, e.g. /Game/PCG."),
        RPC_PARAM_DEF("graphClass", "classref",
            "UPCGGraph subclass to instantiate: full path (/Script/<Module>.<Class>) or short class "
            "name. Resolved by reflection, so any loaded subclass works without PinWright linking "
            "its module. Rejected with CLASS_NOT_FOUND when it does not resolve, is not a UPCGGraph "
            "subclass, or is abstract. The resolved class is echoed back as graphClass.",
            "/Script/PCG.PCGGraph")
    ))
{
    FString AssetName;
    if (!Ctx.RequireString(TEXT("name"), AssetName)) return true;

    FString SavePath;
    if (!Ctx.RequireString(TEXT("savePath"), SavePath)) return true;

    // Resolve the graph class by reflection rather than by a compile-time NewObject<T>: the
    // useful subclasses ship in optional engine plugins (UE 5.8's UProceduralVegetationGraph),
    // and naming one here would put a hard link dependency on an off-by-default module into
    // PinWrightPCG. Resolved before the package is created so a bad class costs no side effects.
    UClass* GraphClass = UPCGGraph::StaticClass();
    const FString GraphClassInput = Ctx.GetString(TEXT("graphClass")).TrimStartAndEnd();
    if (!GraphClassInput.IsEmpty())
    {
        GraphClass = ResolveUClass(GraphClassInput);
        if (!GraphClass && PinWrightPCG::SendPluginDisabledForScriptPath(Ctx, GraphClassInput))
        {
            // Same reasoning as pcg.add_node's nodeClass: an unresolvable /Script/ path is a
            // disabled engine plugin as often as a typo, and the subclass this parameter exists
            // for — UE 5.8's UProceduralVegetationGraph — ships disabled by default.
            return true;
        }
        // CLASS_Abstract is checked here because NewObject's own guard is a checkf
        // (UObjectGlobals.cpp StaticConstructObject_Internal), i.e. a crash rather than a null.
        if (!GraphClass
            || !GraphClass->IsChildOf(UPCGGraph::StaticClass())
            || GraphClass->HasAnyClassFlags(CLASS_Abstract))
        {
            Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
                FString::Printf(TEXT("Could not resolve a concrete UPCGGraph subclass: %s"),
                    *GraphClassInput));
            return true;
        }
    }

    // Normalize the save folder to a valid /Game/... long package path.
    FNormalizedAssetPath NormalizedFolder = NormalizeAssetPath(SavePath);
    if (!NormalizedFolder.bIsValid)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, NormalizedFolder.ErrorMessage);
        return true;
    }
    const FString SanitizedFolder = SanitizeProjectRelativePath(NormalizedFolder.Path);
    if (SanitizedFolder.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("Invalid savePath: %s"), *SavePath));
        return true;
    }

    const FString FullPackagePath = FString::Printf(TEXT("%s/%s"), *SanitizedFolder, *AssetName);

    FText InvalidReason;
    if (!FPackageName::IsValidLongPackageName(FullPackagePath, false, &InvalidReason))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, InvalidReason.ToString());
        return true;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPackagePath, *AssetName);
    if (FindObject<UPCGGraph>(nullptr, *ObjectPath) || LoadObject<UPCGGraph>(nullptr, *ObjectPath))
    {
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("Asset already exists: %s"), *ObjectPath));
        return true;
    }

    UPackage* Package = CreatePackage(*FullPackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED,
            FString::Printf(TEXT("Failed to create package: %s"), *FullPackagePath));
        return true;
    }

    UPCGGraph* Graph = NewObject<UPCGGraph>(
        Package,
        GraphClass,
        FName(*AssetName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Graph)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED,
            FString::Printf(TEXT("Failed to create %s."), *GraphClass->GetPathName()));
        return true;
    }

    FAssetRegistryModule::AssetCreated(Graph);

    FString PackageName;
    int64 SizeBytes = 0;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    const bool bSavedToDisk = SaveAssetToDiskReportingPresence(
        Graph, /*bForce=*/true, &PackageName, &SizeBytes, &SaveState);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Result->SetStringField(TEXT("name"), AssetName);
    // Read off the created object, not off the request: a caller must be able to tell a
    // defaulted UPCGGraph from the subclass they asked for.
    Result->SetStringField(TEXT("graphClass"), Graph->GetClass()->GetPathName());
    AddAssetSaveReport(Result, /*bSaveRequested=*/true, bSavedToDisk, SaveState);
    AddAssetSaveSizeReport(Result, SizeBytes, bSavedToDisk);
    AddAssetVerification(Result, Graph);

    if (!bSavedToDisk)
    {
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            FString::Printf(TEXT("Created PCG graph '%s', but it was not persisted to disk: %s"),
                *Graph->GetPathName(), AssetSaveStateDetail(SaveState)),
            Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
