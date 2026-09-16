// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/IrSidecarRegistry.h"
#include "Dom/JsonValue.h"

#if __has_include("PCGGraph.h")

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"

namespace
{
    UClass* GetPCGGraphClass()
    {
        return UPCGGraph::StaticClass();
    }

    IrSidecarRegistry::FIrSidecarResult BuildPCGIRGraphSidecar(UObject* Asset)
    {
        const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(Cast<UPCGGraph>(Asset));
        return {Result.PCGIRText, Result.Warnings, Result.bSuccess};
    }
}

REGISTER_DECOMPILE_IR(TEXT("pcgir.graph"), DumpFileNames::PcgIr,
    &GetPCGGraphClass, &BuildPCGIRGraphSidecar, 100);

REGISTER_RPC_HANDLER("pcg.decompile", "pcg",
    "Decompile a UPCGGraph asset into PCGIR text.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "PCG graph asset path"),
        RPC_PARAM_OPT("includeReferencedSubgraphs", "boolean", "Reserved for future use; subgraphs are always flattened in the current decompile direction")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *AssetPath);
    if (!Graph)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load PCG graph: %s"), *AssetPath));
        return true;
    }

    FPCGIRDecompileOptions DecompileOptions;
    DecompileOptions.bIncludeReferencedSubgraphs = Ctx.GetBool(TEXT("includeReferencedSubgraphs"), false);

    FPCGIRDecompileResult DecompileResult = FPCGIRDecompiler::DecompileGraph(Graph, DecompileOptions);
    if (!DecompileResult.bSuccess)
    {
        const FString Message = DecompileResult.Warnings.IsEmpty()
            ? TEXT("PCGIR decompile failed.")
            : FString::Join(DecompileResult.Warnings, TEXT("\n"));
        Ctx.SendError(TEXT("PCGIR_DECOMPILE_FAILED"), Message);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : DecompileResult.Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("text"), DecompileResult.PCGIRText);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
