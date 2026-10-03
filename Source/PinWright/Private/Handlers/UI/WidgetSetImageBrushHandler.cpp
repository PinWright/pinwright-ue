// Copyright (c) 2026 Alexander Penkin. MIT License.

// Ergonomic shortcut for setting an FSlateBrush UPROPERTY on a widget instance
// from a brush resource path (texture, material, or texture-atlas object such as
// a Paper2D sprite) + optional ImageSize/Tint/DrawAs/Margin. Avoids the
// hand-spelled FSlateBrush ExportText literal that widget.set would otherwise
// require (see F-image-brush-from-texture).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Utils/PropertyUtils.h"
#include "Utils/TransactionUtils.h"
#include "ScopedTransaction.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Slate/SlateTextureAtlasInterface.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Layout/Margin.h"
#include "Misc/Char.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "UObject/UnrealType.h"
#include "Compat/JsonKeyCompat.h"

using namespace WidgetAuthoringHelpers;

namespace
{
    // Parse "#RRGGBB" or "#RRGGBBAA" (optional leading '#') into a sRGB FLinearColor.
    // Pre-validates exact length 6 or 8 hex digits so we don't silently accept the
    // 3-char "#RGB" shorthand that FColor::FromHex would otherwise consume.
    bool TryParseHexColor(const FString& In, FLinearColor& Out)
    {
        FString S = In;
        if (S.StartsWith(TEXT("#")))
        {
            S = S.RightChop(1);
        }
        if (S.Len() != 6 && S.Len() != 8)
        {
            return false;
        }
        for (TCHAR C : S)
        {
            if (!FChar::IsHexDigit(C))
            {
                return false;
            }
        }
        Out = FLinearColor(FColor::FromHex(S));
        return true;
    }

    bool TryGetNumberCI(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
    {
        if (!Obj.IsValid()) return false;
        for (const auto& Pair : Obj->Values)
        {
            if (EARGCompat::JsonKeyToString(Pair.Key).Equals(Key, ESearchCase::IgnoreCase) &&
                Pair.Value.IsValid() &&
                (Pair.Value->Type == EJson::Number || Pair.Value->Type == EJson::String))
            {
                OutValue = Pair.Value->AsNumber();
                return true;
            }
        }
        return false;
    }
}

REGISTER_RPC_HANDLER("widget.set_image_brush", "widget",
    "Set an FSlateBrush UPROPERTY (default 'Brush') on a widget instance from a texture/material/atlas resource path + optional ImageSize/Tint/DrawAs/Margin.",
    RPC_PARAMS(
        RPC_PARAM_REQ("widgetPath",    "path", "Path to the widget blueprint"),
        RPC_PARAM_REQ("widgetName",    "string", "Name of the target widget instance"),
        RPC_PARAM_REQ_ALIAS("resourcePath", "path", "Object path to the brush resource: any UTexture, UMaterialInterface (material or material instance), or ISlateTextureAtlasInterface object (e.g. a Paper2D sprite). Anything else is refused with INVALID_ASSET_TYPE. Alias: texturePath", "texturePath"),
        RPC_PARAM_OPT("imageSize",     "object", "{X,Y} — override ImageSize; defaults to the texture's imported size (UTexture2D), surface size (other textures) or atlas source size. A material has no intrinsic size: pass imageSize, otherwise the FSlateBrush default (32x32) is kept and a warning is returned"),
        RPC_PARAM_OPT("tint",          "object", "{R,G,B,A} in 0..1 OR \"#RRGGBBAA\" hex string; default white"),
        RPC_PARAM_OPT("drawAs",        "string", "Image|Box|Border|RoundedBox|NoDrawType; default Image"),
        RPC_PARAM_OPT("margin",        "object", "{Left,Top,Right,Bottom}; meaningful for Box/Border; default zero"),
        RPC_PARAM_OPT("brushProperty", "string", "Target struct property name; default \"Brush\"")
    ))
{
    FString WidgetPath, WidgetName;
    if (!Ctx.RequireString(TEXT("widgetPath"),  WidgetPath))  return true;
    if (!Ctx.RequireString(TEXT("widgetName"),  WidgetName))  return true;
    const FString ResourcePath = Ctx.GetStringFirstOf({TEXT("resourcePath"), TEXT("texturePath")});
    if (ResourcePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Missing required string field: resourcePath (alias texturePath)"));
        return true;
    }

    FString BrushPropertyName = Ctx.GetString(TEXT("brushProperty"), TEXT("Brush"));
    if (BrushPropertyName.IsEmpty())
    {
        BrushPropertyName = TEXT("Brush");
    }

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* Widget = FindWidgetByName(WidgetBP, WidgetName);
    if (!Widget)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget not found: ") + WidgetName);
        return true;
    }

    UObject* Resource = LoadObject<UObject>(nullptr, *ResourcePath);
    if (!Resource)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Brush resource not found at '%s'"), *ResourcePath));
        return true;
    }
    // Mirror FSlateRHIRenderer::CanRenderResource: anything else trips the ensure in
    // FSlateBrush::SetResourceObject and is silently dropped from the brush.
    UTexture* ResourceTexture = Cast<UTexture>(Resource);
    ISlateTextureAtlasInterface* ResourceAtlas = Cast<ISlateTextureAtlasInterface>(Resource);
    if (!ResourceTexture && !ResourceAtlas && !Cast<UMaterialInterface>(Resource))
    {
        Ctx.SendError(TEXT("INVALID_ASSET_TYPE"),
            FString::Printf(TEXT("'%s' is a %s, which Slate cannot draw as a brush resource "
                "(expected a Texture, MaterialInterface or SlateTextureAtlasInterface object)"),
                *ResourcePath, *Resource->GetClass()->GetName()));
        return true;
    }

    FProperty* Prop = FindPropertyCI(Widget->GetClass(), BrushPropertyName);
    if (!Prop)
    {
        Ctx.SendError(TEXT("INVALID_PROPERTY"),
            FString::Printf(TEXT("Brush property not found: %s"), *BrushPropertyName));
        return true;
    }
    FStructProperty* StructProp = CastField<FStructProperty>(Prop);
    if (!StructProp || !StructProp->Struct ||
        !StructProp->Struct->IsChildOf(TBaseStructure<FSlateBrush>::Get()))
    {
        Ctx.SendError(TEXT("INVALID_PROPERTY"),
            FString::Printf(TEXT("Property '%s' is not an FSlateBrush"), *BrushPropertyName));
        return true;
    }

    // Compose the brush before opening the transaction; bail with structured
    // errors on bad payload shapes (drawAs, tint hex) before mutating state.
    FSlateBrush Brush;
    Brush.SetResourceObject(Resource);

    // ImageSize: payload override wins; otherwise default to the resource's own size.
    // A material has none, so it keeps the FSlateBrush default.
    bool bImageSizeFromResource = true;
    if (UTexture2D* Tex2D = Cast<UTexture2D>(Resource))
    {
        const FIntPoint ImportedSize = Tex2D->GetImportedSize();
        Brush.ImageSize = FVector2D(ImportedSize.X, ImportedSize.Y);
    }
    else if (ResourceTexture)
    {
        Brush.ImageSize = FVector2D(ResourceTexture->GetSurfaceWidth(), ResourceTexture->GetSurfaceHeight());
    }
    else if (ResourceAtlas)
    {
        Brush.ImageSize = ResourceAtlas->GetSlateAtlasData().GetSourceDimensions();
    }
    else
    {
        bImageSizeFromResource = false;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> ImageSizeObj = GetObjectField(Payload, TEXT("imageSize"));
    if (ImageSizeObj)
    {
        double X = Brush.ImageSize.X;
        double Y = Brush.ImageSize.Y;
        TryGetNumberCI(ImageSizeObj, TEXT("X"), X);
        TryGetNumberCI(ImageSizeObj, TEXT("Y"), Y);
        Brush.ImageSize = FVector2D(X, Y);
    }

    // drawAs — resolve via the UENUM reflection table so the accepted set stays in
    // sync with engine changes (GetValueByNameString is case-insensitive).
    const FString DrawAsStr = Ctx.GetString(TEXT("drawAs"));
    if (!DrawAsStr.IsEmpty())
    {
        const UEnum* DrawAsEnum = StaticEnum<ESlateBrushDrawType::Type>();
        const int64 EnumVal = DrawAsEnum ? DrawAsEnum->GetValueByNameString(DrawAsStr) : INDEX_NONE;
        if (EnumVal == INDEX_NONE)
        {
            Ctx.SendError(TEXT("INVALID_DRAW_AS"),
                FString::Printf(TEXT("Unknown drawAs '%s' (expected Image|Box|Border|RoundedBox|NoDrawType)"),
                    *DrawAsStr));
            return true;
        }
        Brush.DrawAs = static_cast<ESlateBrushDrawType::Type>(EnumVal);
    }
    else
    {
        Brush.DrawAs = ESlateBrushDrawType::Image;
    }

    // tint — accept JSON object {R,G,B,A} or "#RRGGBBAA" / "#RRGGBB" hex string.
    FLinearColor TintColor = FLinearColor::White;
    TSharedPtr<FJsonValue> TintValue;
    if (Payload.IsValid())
    {
        for (const auto& Pair : Payload->Values)
        {
            if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("tint"), ESearchCase::IgnoreCase))
            {
                TintValue = Pair.Value;
                break;
            }
        }
    }
    if (TintValue.IsValid())
    {
        if (TintValue->Type == EJson::Object)
        {
            TintColor = GetColorFromJsonWidget(TintValue->AsObject(), FLinearColor::White);
        }
        else if (TintValue->Type == EJson::String)
        {
            const FString HexStr = TintValue->AsString();
            if (!TryParseHexColor(HexStr, TintColor))
            {
                Ctx.SendError(TEXT("INVALID_PROPERTY"),
                    FString::Printf(TEXT("tint hex string '%s' not in #RRGGBB or #RRGGBBAA form"), *HexStr));
                return true;
            }
        }
    }
    Brush.TintColor = FSlateColor(TintColor);

    // margin
    if (TSharedPtr<FJsonObject> MarginObj = GetObjectField(Payload, TEXT("margin")))
    {
        double L = 0.0, T = 0.0, R = 0.0, B = 0.0;
        TryGetNumberCI(MarginObj, TEXT("Left"),   L);
        TryGetNumberCI(MarginObj, TEXT("Top"),    T);
        TryGetNumberCI(MarginObj, TEXT("Right"),  R);
        TryGetNumberCI(MarginObj, TEXT("Bottom"), B);
        Brush.Margin = FMargin(L, T, R, B);
    }

    {
        FScopedTransaction Transaction(NSLOCTEXT("PinWright",
            "SetImageBrushTransaction", "MCP: widget.set_image_brush"));
        PinWrightTransactionUtils::PrepareTransactionalSnapshot(Widget);
        *StructProp->ContainerPtrToValuePtr<FSlateBrush>(Widget) = Brush;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("propertyName"), StructProp->GetName());
    Result->SetStringField(TEXT("resourceClass"), Resource->GetClass()->GetName());
    TSharedPtr<FJsonObject> SizeJson = MakeShared<FJsonObject>();
    SizeJson->SetNumberField(TEXT("X"), Brush.ImageSize.X);
    SizeJson->SetNumberField(TEXT("Y"), Brush.ImageSize.Y);
    Result->SetObjectField(TEXT("imageSize"), SizeJson);
    if (!ImageSizeObj && !bImageSizeFromResource)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%s has no intrinsic size; ImageSize kept the FSlateBrush default %gx%g. Pass imageSize {X,Y}."),
            *Resource->GetClass()->GetName(), Brush.ImageSize.X, Brush.ImageSize.Y)));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }
    Result->SetBoolField(TEXT("requiresCompile"), true);
    Ctx.SendSuccess(Result);
    return true;
}
