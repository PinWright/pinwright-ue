// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectGlobals.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "Elements/PCGNormalToDensity.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"
#include "Utils/JsonBuilders.h"

REGISTER_RPC_HANDLER("pcg.add_slope_filter", "pcg",
    "Append a normal-to-density (slope) filter node to a UPCGGraph with typed knobs. "
    "UE 5.6 has no slope-specific filter class; UPCGNormalToDensitySettings is the canonical analog. "
    "A supplied normal must contain exactly three finite numeric components.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("x", "integer", "Node X position."),
        RPC_PARAM_REQ("y", "integer", "Node Y position."),
        RPC_PARAM_OPT("normal", "array|object", "Reference normal as exactly three finite numeric values in [X,Y,Z] or {X,Y,Z}. Default: world up."),
        RPC_PARAM_OPT("offset", "number", "Bias against the reference normal."),
        RPC_PARAM_OPT("strength", "number", "Density curve strength; Result = Result^(1/Strength). Default 1.0.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    int32 X = 0;
    if (!Ctx.RequireInt(TEXT("x"), X)) return true;
    int32 Y = 0;
    if (!Ctx.RequireInt(TEXT("y"), Y)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    const TSharedPtr<FJsonValue> NormalValue =
        Ctx.GetJsonValueFirstOf({TEXT("normal")});
    // Read only after TryReadStrictVector has written it, but the compiler cannot see that
    // correlation with bHasNormal, so give it a value rather than leave the read indeterminate.
    FVector RequestedNormal = FVector::ZeroVector;
    const bool bHasNormal = NormalValue.IsValid() && NormalValue->Type != EJson::Null;
    if (bHasNormal && !PinWrightPCG::TryReadStrictVector(NormalValue, RequestedNormal))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("normal must be exactly three finite numeric values as an array or an object with X, Y, and Z"));
        return true;
    }

    // Resolve UClass* at runtime to avoid linking against the unexported GetPrivateStaticClass symbol.
    // UPCGNormalToDensitySettings is declared without PCG_API, so StaticClass() would cause an
    // unresolved external symbol at link time on UE 5.4+.
    UClass* NormalToDensityClass = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGNormalToDensitySettings"));
    if (!NormalToDensityClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
            TEXT("Could not resolve UPCGNormalToDensitySettings via reflection"));
        return true;
    }

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(
        TSubclassOf<UPCGSettings>(NormalToDensityClass),
        DefaultSettings);
    if (!Node)
    {
        Ctx.SendError(ErrorCodes::ERR_ADD_NODE_FAILED,
            TEXT("AddNodeOfType returned null for UPCGNormalToDensitySettings"));
        return true;
    }

    // static_cast is safe: AddNodeOfType created the object from NormalToDensityClass above.
    UPCGNormalToDensitySettings* Settings =
        (DefaultSettings && DefaultSettings->IsA(NormalToDensityClass))
            ? static_cast<UPCGNormalToDensitySettings*>(DefaultSettings)
            : nullptr;
    if (Settings)
    {
        if (bHasNormal)
        {
            Settings->Normal = RequestedNormal;
        }

        const TSharedPtr<FJsonValue> OffsetVal = Ctx.GetJsonValueFirstOf({TEXT("offset")});
        if (OffsetVal.IsValid() && OffsetVal->Type != EJson::Null)
        {
            Settings->Offset = OffsetVal->AsNumber();
        }

        const TSharedPtr<FJsonValue> StrengthVal = Ctx.GetJsonValueFirstOf({TEXT("strength")});
        if (StrengthVal.IsValid() && StrengthVal->Type != EJson::Null)
        {
            Settings->Strength = StrengthVal->AsNumber();
        }
    }

    Node->SetNodePosition(X, Y);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Node->GetName());
    if (Settings)
    {
        Result->SetObjectField(TEXT("normal"), JsonBuilders::BuildVectorJson(Settings->Normal));
        Result->SetNumberField(TEXT("offset"), Settings->Offset);
        Result->SetNumberField(TEXT("strength"), Settings->Strength);
    }
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
