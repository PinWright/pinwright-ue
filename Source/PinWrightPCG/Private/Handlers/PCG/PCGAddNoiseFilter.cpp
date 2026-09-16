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
#include "Elements/PCGSpatialNoise.h"
#include "Elements/PCGAttributeNoise.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

namespace
{
    // Spatial noise mode strings -> PCGSpatialNoiseMode (no E prefix in UE 5.6).
    bool ParseSpatialNoiseMode(const FString& Name, PCGSpatialNoiseMode& Out)
    {
        if (Name == TEXT("Perlin2D"))               { Out = PCGSpatialNoiseMode::Perlin2D;               return true; }
        if (Name == TEXT("Caustic2D"))              { Out = PCGSpatialNoiseMode::Caustic2D;              return true; }
        if (Name == TEXT("Voronoi2D"))              { Out = PCGSpatialNoiseMode::Voronoi2D;              return true; }
        if (Name == TEXT("FractionalBrownian2D"))   { Out = PCGSpatialNoiseMode::FractionalBrownian2D;   return true; }
        if (Name == TEXT("EdgeMask2D"))             { Out = PCGSpatialNoiseMode::EdgeMask2D;             return true; }
        return false;
    }

    // Attribute noise mode strings -> EPCGAttributeNoiseMode.
    // Accepts both Min/Max (ticket spec aliases) and the engine's Minimum/Maximum names.
    bool ParseAttributeNoiseMode(const FString& Name, EPCGAttributeNoiseMode& Out)
    {
        if (Name == TEXT("Set"))                            { Out = EPCGAttributeNoiseMode::Set;     return true; }
        if (Name == TEXT("Min") || Name == TEXT("Minimum")) { Out = EPCGAttributeNoiseMode::Minimum; return true; }
        if (Name == TEXT("Max") || Name == TEXT("Maximum")) { Out = EPCGAttributeNoiseMode::Maximum; return true; }
        if (Name == TEXT("Add"))                            { Out = EPCGAttributeNoiseMode::Add;     return true; }
        if (Name == TEXT("Multiply"))                       { Out = EPCGAttributeNoiseMode::Multiply;return true; }
        return false;
    }
}

REGISTER_RPC_HANDLER("pcg.add_noise_filter", "pcg",
    "Append a spatial or attribute noise node to a UPCGGraph. kind=\"spatial\" -> "
    "UPCGSpatialNoiseSettings; kind=\"attribute\" -> UPCGAttributeNoiseSettings. "
    "Unknown noiseType values return INVALID_NOISE_TYPE before graph mutation.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("x", "integer", "Node X position."),
        RPC_PARAM_REQ("y", "integer", "Node Y position."),
        RPC_PARAM_REQ("kind", "string", "\"spatial\" or \"attribute\"."),
        RPC_PARAM_OPT("noiseType", "string", "Spatial: Perlin2D|Caustic2D|Voronoi2D|FractionalBrownian2D|EdgeMask2D. Attribute: Set|Min|Max|Add|Multiply."),
        RPC_PARAM_OPT("iterations", "integer", "Spatial only: fractal iteration count (1-100)."),
        RPC_PARAM_OPT("brightness", "number", "Spatial only."),
        RPC_PARAM_OPT("contrast", "number", "Spatial only."),
        RPC_PARAM_OPT("seed", "integer", "Common seed; written via UPCGSettings::Seed if present."),
        RPC_PARAM_OPT("noiseMin", "number", "Attribute only: lower bound of noise range."),
        RPC_PARAM_OPT("noiseMax", "number", "Attribute only: upper bound of noise range.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    int32 X = 0;
    if (!Ctx.RequireInt(TEXT("x"), X)) return true;
    int32 Y = 0;
    if (!Ctx.RequireInt(TEXT("y"), Y)) return true;

    FString Kind;
    if (!Ctx.RequireString(TEXT("kind"), Kind)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    const bool bSpatial = (Kind == TEXT("spatial"));
    const bool bAttribute = (Kind == TEXT("attribute"));
    if (!bSpatial && !bAttribute)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_KIND,
            FString::Printf(TEXT("kind must be \"spatial\" or \"attribute\", got: %s"), *Kind));
        return true;
    }

    const TSharedPtr<FJsonValue> NoiseTypeValue =
        Ctx.GetJsonValueFirstOf({TEXT("noiseType")});
    FString NoiseTypeStr;
    const bool bHasNoiseType = NoiseTypeValue.IsValid() && NoiseTypeValue->Type != EJson::Null;
    if (bHasNoiseType)
    {
        if (NoiseTypeValue->Type != EJson::String)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_NOISE_TYPE,
                TEXT("noiseType must be a string when provided"));
            return true;
        }
        NoiseTypeStr = NoiseTypeValue->AsString();
    }

    PCGSpatialNoiseMode SpatialNoiseMode = PCGSpatialNoiseMode::Perlin2D;
    EPCGAttributeNoiseMode AttributeNoiseMode = EPCGAttributeNoiseMode::Set;
    bool bHasParsedNoiseType = false;
    if (bSpatial && bHasNoiseType)
    {
        if (!ParseSpatialNoiseMode(NoiseTypeStr, SpatialNoiseMode))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_NOISE_TYPE,
                FString::Printf(TEXT("Unknown spatial noiseType: %s"), *NoiseTypeStr));
            return true;
        }
        bHasParsedNoiseType = true;
    }
    else if (bAttribute && bHasNoiseType)
    {
        if (!ParseAttributeNoiseMode(NoiseTypeStr, AttributeNoiseMode))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_NOISE_TYPE,
                FString::Printf(TEXT("Unknown attribute noiseType: %s"), *NoiseTypeStr));
            return true;
        }
        bHasParsedNoiseType = true;
    }

    // Resolve UClass* at runtime to avoid linking against the unexported GetPrivateStaticClass symbol.
    // UPCGSpatialNoiseSettings and UPCGAttributeNoiseSettings are declared without PCG_API,
    // so StaticClass() is not exported and would cause an unresolved external symbol at link time.
    UClass* SettingsClass = nullptr;
    if (bSpatial)
    {
        SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGSpatialNoiseSettings"));
        if (!SettingsClass)
        {
            Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
                TEXT("Could not resolve UPCGSpatialNoiseSettings via reflection"));
            return true;
        }
    }
    else if (bAttribute)
    {
        SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGAttributeNoiseSettings"));
        if (!SettingsClass)
        {
            Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
                TEXT("Could not resolve UPCGAttributeNoiseSettings via reflection"));
            return true;
        }
    }

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(
        TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
    if (!Node)
    {
        Ctx.SendError(ErrorCodes::ERR_ADD_NODE_FAILED,
            TEXT("AddNodeOfType returned null"));
        return true;
    }

    if (bSpatial)
    {
        // static_cast is safe here: AddNodeOfType created the object from SettingsClass,
        // which we resolved as UPCGSpatialNoiseSettings above. No link symbol required.
        UPCGSpatialNoiseSettings* Settings =
            (DefaultSettings && DefaultSettings->IsA(SettingsClass))
                ? static_cast<UPCGSpatialNoiseSettings*>(DefaultSettings)
                : nullptr;
        if (Settings)
        {
            if (bHasParsedNoiseType)
            {
                Settings->Mode = SpatialNoiseMode;
            }

            const TSharedPtr<FJsonValue> IterVal = Ctx.GetJsonValueFirstOf({TEXT("iterations")});
            if (IterVal.IsValid() && IterVal->Type != EJson::Null)
            {
                Settings->Iterations = static_cast<int32>(IterVal->AsNumber());
            }
            const TSharedPtr<FJsonValue> BrightVal = Ctx.GetJsonValueFirstOf({TEXT("brightness")});
            if (BrightVal.IsValid() && BrightVal->Type != EJson::Null)
            {
                Settings->Brightness = static_cast<float>(BrightVal->AsNumber());
            }
            const TSharedPtr<FJsonValue> ContrastVal = Ctx.GetJsonValueFirstOf({TEXT("contrast")});
            if (ContrastVal.IsValid() && ContrastVal->Type != EJson::Null)
            {
                Settings->Contrast = static_cast<float>(ContrastVal->AsNumber());
            }
        }
    }
    else // bAttribute
    {
        // static_cast is safe: object was created from the resolved UPCGAttributeNoiseSettings UClass*.
        UPCGAttributeNoiseSettings* Settings =
            (DefaultSettings && DefaultSettings->IsA(SettingsClass))
                ? static_cast<UPCGAttributeNoiseSettings*>(DefaultSettings)
                : nullptr;
        if (Settings)
        {
            if (bHasParsedNoiseType)
            {
                Settings->Mode = AttributeNoiseMode;
            }

            const TSharedPtr<FJsonValue> MinVal = Ctx.GetJsonValueFirstOf({TEXT("noiseMin")});
            if (MinVal.IsValid() && MinVal->Type != EJson::Null)
            {
                Settings->NoiseMin = static_cast<float>(MinVal->AsNumber());
            }
            const TSharedPtr<FJsonValue> MaxVal = Ctx.GetJsonValueFirstOf({TEXT("noiseMax")});
            if (MaxVal.IsValid() && MaxVal->Type != EJson::Null)
            {
                Settings->NoiseMax = static_cast<float>(MaxVal->AsNumber());
            }
        }
    }

    // Seed lives on UPCGSettings (the common base). Apply uniformly when caller passes it.
    const TSharedPtr<FJsonValue> SeedVal = Ctx.GetJsonValueFirstOf({TEXT("seed")});
    if (SeedVal.IsValid() && SeedVal->Type != EJson::Null && DefaultSettings)
    {
        if (FProperty* SeedProp = DefaultSettings->GetClass()->FindPropertyByName(TEXT("Seed")))
        {
            if (FIntProperty* IntProp = CastField<FIntProperty>(SeedProp))
            {
                IntProp->SetPropertyValue_InContainer(DefaultSettings,
                    static_cast<int32>(SeedVal->AsNumber()));
            }
        }
    }

    Node->SetNodePosition(X, Y);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), Node->GetName());
    Result->SetStringField(TEXT("kind"), Kind);
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
