// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRCompiler.h"


#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Material/MaterialExpressionFactory.h"
#include "MaterialExpressionIO.h"
#include "MGIR/MGIRDynamicInputs.h"
#include "MGIR/MGIRExpressionEmitter.h"
#include "MGIR/MGIRHelpers.h"
#include "MGIR/MGIRLayoutEngine.h"
#include "MGIR/MGIRMaterialAttributeUtils.h"
#include "MGIR/MGIRMaterialProperties.h"
#include "MGIR/MGIRParser.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "MaterialShared.h"
#include "Handlers/Material/MaterialFunctionIdentity.h"
#include "Handlers/Material/MaterialLandscapeConsumers.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"
#include "Utils/GuardedLoad.h"

namespace
{
struct FMGIRPendingWire
{
    FString TargetName;
    FString InputName;
    FString SourceReference;
    int32 SourceLine = -1;
};

struct FMGIRPendingOutput
{
    FString OutputName;
    FString SourceReference;
    int32 SourceLine = -1;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIRCompiledBlock
{
    FString AssetPath;
    int32 ExpressionsCreated = 0;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;

    // Measured push of this block's compiled master into the consumers caching instances
    // derived from it. Only material blocks fill it; a function block leaves it at the
    // default "nothing measured", which is the honest value for a path that has no
    // landscape consumer of its own.
    PinWright::DerivedState::FConsumerRefreshReport ConsumerRefresh;

    // Non-fatal facts about what this block changed that a caller has to act on.
    TArray<FString> Warnings;
};

// Every string literal in the document is read through this one function, and it is the
// exact inverse of the decompiler's encoder. It used to be a local un-quoter that reversed
// only `\"` and `\\`, so the other half of the escape set the decompiler writes -- `\n`,
// `\r`, `\t`, `\uNNNN` -- survived into the compiled asset as literal backslash pairs.
using MGIRHelpers::DecodeStringLiteral;

bool IsPinReference(const FString& Value)
{
    return Value.TrimStartAndEnd().StartsWith(TEXT("%"));
}

bool TryParseNumberLiteral(const FString& Value, double& OutNumber)
{
    return LexTryParseString(OutNumber, *Value.TrimStartAndEnd());
}

TSharedPtr<FJsonValue> LiteralToJsonValue(const FString& Literal)
{
    const FString Trimmed = Literal.TrimStartAndEnd();
    if (Trimmed.StartsWith(TEXT("\"")) && Trimmed.EndsWith(TEXT("\"")))
    {
        return MakeShared<FJsonValueString>(DecodeStringLiteral(Trimmed));
    }

    if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase))
    {
        return MakeShared<FJsonValueBoolean>(true);
    }
    if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase))
    {
        return MakeShared<FJsonValueBoolean>(false);
    }

    if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2);
        TArray<FString> Parts;
        Inner.ParseIntoArray(Parts, TEXT(","), false);
        for (const FString& Part : Parts)
        {
            Values.Add(LiteralToJsonValue(Part));
        }
        return MakeShared<FJsonValueArray>(Values);
    }

    double Number = 0.0;
    if (TryParseNumberLiteral(Trimmed, Number))
    {
        return MakeShared<FJsonValueNumber>(Number);
    }

    return MakeShared<FJsonValueString>(DecodeStringLiteral(Trimmed));
}

TArray<FString> LiteralToStringArray(const FString& Literal)
{
    TArray<FString> Values;
    const TSharedPtr<FJsonValue> JsonValue = LiteralToJsonValue(Literal);
    if (!JsonValue.IsValid())
    {
        return Values;
    }

    if (JsonValue->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue>& Item : JsonValue->AsArray())
        {
            if (Item.IsValid())
            {
                Values.Add(Item->AsString());
            }
        }
    }
    else
    {
        Values.Add(JsonValue->AsString());
    }
    return Values;
}

FString NormalizeFunctionInputTypeLiteral(const FString& Literal)
{
    const FString Value = DecodeStringLiteral(Literal).TrimStartAndEnd();
    if (Value.Equals(TEXT("Float1"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Scalar"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_Scalar");
    }
    if (Value.Equals(TEXT("Float2"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Vector2"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_Vector2");
    }
    if (Value.Equals(TEXT("Float3"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Vector3"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_Vector3");
    }
    if (Value.Equals(TEXT("Float4"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Vector4"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_Vector4");
    }
    if (Value.Equals(TEXT("Texture2D"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_Texture2D");
    }
    if (Value.Equals(TEXT("MaterialAttributes"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_MaterialAttributes");
    }
    if (Value.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_Bool");
    }
    if (Value.Equals(TEXT("StaticBool"), ESearchCase::IgnoreCase))
    {
        return TEXT("FunctionInput_StaticBool");
    }
    return Value;
}

bool IsFunctionInputExpressionClassName(const FString& SymbolName)
{
    const FString Name = DecodeStringLiteral(SymbolName).TrimStartAndEnd();
    return Name.Equals(TEXT("FunctionInput"), ESearchCase::IgnoreCase)
        || Name.Equals(TEXT("MaterialExpressionFunctionInput"), ESearchCase::IgnoreCase)
        || Name.EndsWith(TEXT(".MaterialExpressionFunctionInput"), ESearchCase::IgnoreCase)
        || Name.EndsWith(TEXT("/MaterialExpressionFunctionInput"), ESearchCase::IgnoreCase);
}

TSharedPtr<FJsonValue> CallArgToJsonValue(const FMGIRInstruction& Instruction, const FMGIRArg& Arg)
{
    const bool bFunctionInput = IsFunctionInputExpressionClassName(Instruction.SymbolName);
    if (bFunctionInput && Arg.Name.Equals(TEXT("InputType"), ESearchCase::IgnoreCase))
    {
        return MakeShared<FJsonValueString>(NormalizeFunctionInputTypeLiteral(Arg.Value));
    }

    return LiteralToJsonValue(Arg.Value);
}

bool TryReadConstantValues(const FMGIRInstruction& Instruction, TArray<double>& OutValues, FString& OutError)
{
    OutValues.Reset();
    for (const FMGIRArg& Arg : Instruction.Args)
    {
        double Number = 0.0;
        if (!TryParseNumberLiteral(Arg.Value, Number))
        {
            OutError = FString::Printf(
                TEXT("constant at line %d contains a non-numeric value: %s"),
                Instruction.SourceLine,
                *Arg.Value);
            return false;
        }
        OutValues.Add(Number);
    }

    if (OutValues.IsEmpty())
    {
        OutError = FString::Printf(TEXT("constant at line %d has no values."), Instruction.SourceLine);
        return false;
    }

    return true;
}

void SplitCallArgs(
    const FMGIRInstruction& Instruction,
    TSharedPtr<FJsonObject>& OutProperties,
    TArray<FMGIRPendingWire>& OutWires,
    TFunctionRef<bool(const FMGIRArg&)> TryHandleSpecialArg)
{
    OutProperties = MakeShared<FJsonObject>();
    for (const FMGIRArg& Arg : Instruction.Args)
    {
        if (Arg.Name.IsEmpty())
        {
            continue;
        }

        if (TryHandleSpecialArg(Arg))
        {
            continue;
        }

        if (IsPinReference(Arg.Value))
        {
            FMGIRPendingWire Wire;
            Wire.TargetName = Instruction.ResultName;
            Wire.InputName = Arg.Name;
            Wire.SourceReference = Arg.Value;
            Wire.SourceLine = Instruction.SourceLine;
            OutWires.Add(MoveTemp(Wire));
            continue;
        }

        OutProperties->SetField(Arg.Name, CallArgToJsonValue(Instruction, Arg));
    }
}

void SplitCallArgs(
    const FMGIRInstruction& Instruction,
    TSharedPtr<FJsonObject>& OutProperties,
    TArray<FMGIRPendingWire>& OutWires)
{
    SplitCallArgs(
        Instruction,
        OutProperties,
        OutWires,
        [](const FMGIRArg&) { return false; });
}

void SplitSetMaterialAttributesArgs(
    const FMGIRInstruction& Instruction,
    TSharedPtr<FJsonObject>& OutProperties,
    TArray<FMGIRPendingWire>& OutWires,
    TArray<FString>& OutAttributeNames,
    TArray<FString>& OutLegacyAttributeTypes)
{
    SplitCallArgs(
        Instruction,
        OutProperties,
        OutWires,
        [&OutAttributeNames, &OutLegacyAttributeTypes](const FMGIRArg& Arg)
        {
            if (Arg.Name.Equals(TEXT("Attributes"), ESearchCase::IgnoreCase))
            {
                OutAttributeNames = LiteralToStringArray(Arg.Value);
                return true;
            }

            if (Arg.Name.Equals(TEXT("AttributeSetTypes"), ESearchCase::IgnoreCase))
            {
                OutLegacyAttributeTypes = LiteralToStringArray(Arg.Value);
                return true;
            }

            return false;
        });
}

FString NormalizeEntryTarget(const FMGIREntryBlock& Block, const FMGIRCompileOptions& Options)
{
    if (!Block.Name.IsEmpty())
    {
        return DecodeStringLiteral(Block.Name);
    }
    return Options.Context;
}

void SplitPackageAndAssetName(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
    int32 DotIndex = INDEX_NONE;
    if (AssetPath.FindChar(TEXT('.'), DotIndex))
    {
        OutPackagePath = AssetPath.Left(DotIndex);
        OutAssetName = AssetPath.Mid(DotIndex + 1);
        return;
    }

    OutPackagePath = AssetPath;
    OutAssetName = FPackageName::GetLongPackageAssetName(AssetPath);
}

FMGIRCompileResult GetOrCreateMaterial(const FString& AssetPath, UMaterial*& OutMaterial)
{
    // Guarded, and the ORDER is why it matters: AssetPath is the MGIR block name (raw IR text)
    // when the block names its own target, and the IsValidLongPackageName check below - which does
    // refuse "//" - sits AFTER this load. A raw load here reached CreatePackage's Fatal before the
    // validation that was supposed to prevent it ever ran.
    FString TargetRefusal;
    OutMaterial = PinWrightGuardedLoad::LoadObjectChecked<UMaterial>(AssetPath, &TargetRefusal);
    if (OutMaterial)
    {
        return FMGIRCompileResult();
    }
    if (!TargetRefusal.IsEmpty())
    {
        return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TargetRefusal);
    }

    FString PackagePath;
    FString AssetName;
    SplitPackageAndAssetName(AssetPath, PackagePath, AssetName);

    FText Reason;
    if (AssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackagePath, true, &Reason))
    {
        return FMGIRCompileResult::MakeError(
            TEXT("MGIR_INVALID_TARGET"),
            FString::Printf(TEXT("Invalid material target '%s': %s"), *AssetPath, *Reason.ToString()));
    }

    UPackage* Package = CreatePackage(*PackagePath);
    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    OutMaterial = Cast<UMaterial>(Factory->FactoryCreateNew(
        UMaterial::StaticClass(),
        Package,
        FName(*AssetName),
        RF_Public | RF_Standalone,
        nullptr,
        GWarn));

    if (!OutMaterial)
    {
        return FMGIRCompileResult::MakeError(
            TEXT("MGIR_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create material: %s"), *AssetPath));
    }

    FAssetRegistryModule::AssetCreated(OutMaterial);
    return FMGIRCompileResult();
}

FMGIRCompileResult GetOrCreateMaterialFunction(const FString& AssetPath, UMaterialFunction*& OutFunction)
{
    // Guarded ahead of the IsValidLongPackageName check below, for the reason spelled out in
    // GetOrCreateMaterial: the validation runs after the load, so the load was the first door.
    FString TargetRefusal;
    OutFunction =
        PinWrightGuardedLoad::LoadObjectChecked<UMaterialFunction>(AssetPath, &TargetRefusal);
    if (OutFunction)
    {
        return FMGIRCompileResult();
    }
    if (!TargetRefusal.IsEmpty())
    {
        return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TargetRefusal);
    }

    FString PackagePath;
    FString AssetName;
    SplitPackageAndAssetName(AssetPath, PackagePath, AssetName);

    FText Reason;
    if (AssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackagePath, true, &Reason))
    {
        return FMGIRCompileResult::MakeError(
            TEXT("MGIR_INVALID_TARGET"),
            FString::Printf(TEXT("Invalid material function target '%s': %s"), *AssetPath, *Reason.ToString()));
    }

    UPackage* Package = CreatePackage(*PackagePath);
    UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
    OutFunction = Cast<UMaterialFunction>(Factory->FactoryCreateNew(
        UMaterialFunction::StaticClass(),
        Package,
        FName(*AssetName),
        RF_Public | RF_Standalone,
        nullptr,
        GWarn));

    if (!OutFunction)
    {
        return FMGIRCompileResult::MakeError(
            TEXT("MGIR_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create material function: %s"), *AssetPath));
    }

    FAssetRegistryModule::AssetCreated(OutFunction);
    return FMGIRCompileResult();
}

void ClearExpressionInput(FExpressionInput& Input)
{
    Input.Expression = nullptr;
    Input.OutputIndex = 0;
    Input.Mask = 0;
    Input.MaskR = 0;
    Input.MaskG = 0;
    Input.MaskB = 0;
    Input.MaskA = 0;
}

void ClearMaterialGraph(UMaterial* Material)
{
    if (!Material)
    {
        return;
    }

    if (UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData())
    {
        EditorOnly->ExpressionCollection.Expressions.Empty();
        EditorOnly->ExpressionCollection.EditorComments.Empty();

        const EMaterialProperty Properties[] =
        {
            MP_EmissiveColor,
            MP_Opacity,
            MP_OpacityMask,
            MP_BaseColor,
            MP_Metallic,
            MP_Specular,
            MP_Roughness,
            MP_Anisotropy,
            MP_Normal,
            MP_Tangent,
            MP_WorldPositionOffset,
            MP_SubsurfaceColor,
            MP_AmbientOcclusion,
            MP_Refraction,
            MP_PixelDepthOffset,
            MP_MaterialAttributes,
            MP_FrontMaterial,
            MP_SurfaceThickness,
            MP_Displacement,
            // The decompiler emits these three as `output ClearCoat` /
            // `output ClearCoatRoughness` / `output ShadingModel`, so Append mode has to
            // clear them too. Left out, they kept raw FExpressionInput pointers into the
            // expression collection this function has just emptied.
            MP_CustomData0,
            MP_CustomData1,
            MP_ShadingModel,
        };

        for (EMaterialProperty Property : Properties)
        {
            if (FExpressionInput* Input = Material->GetExpressionInputForProperty(Property))
            {
                ClearExpressionInput(*Input);
            }
        }

        for (FExpressionInput& Input : EditorOnly->CustomizedUVs)
        {
            ClearExpressionInput(Input);
        }
    }
}

void ClearMaterialFunctionGraph(UMaterialFunction* Function)
{
    if (!Function)
    {
        return;
    }

    if (UMaterialFunctionEditorOnlyData* EditorOnly = Function->GetEditorOnlyData())
    {
        EditorOnly->ExpressionCollection.Expressions.Empty();
        EditorOnly->ExpressionCollection.EditorComments.Empty();
    }
}

FMGIRCompileResult FinalizeMaterial(UMaterial* Material, const FMGIRCompileOptions& Options)
{
    if (!Material)
    {
        return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Material is null."));
    }

    if (Options.bRunLayout)
    {
        FMGIRLayoutEngine::Layout(Material);
    }

    FMGIRCompileResult Finalized;

    // Was a bare PreEditChange(nullptr) + PostEditChange() + ForceRecompileForRendering().
    // That regenerates the master's StateId and rebuilds its shader maps, and reaches
    // NOTHING that caches instances derived from it: no FMaterialUpdateContext, so every
    // dependent UMaterialInstance keeps its old static permutation, and no landscape
    // rebuild, so ALandscapeProxy::MaterialInstanceConstantMap keeps the combination MICs
    // it built against the previous graph. In Append mode this function has just emptied
    // and rebuilt the entire expression collection (ClearMaterialGraph above), which is
    // precisely the layer-allocation change those cached MICs are keyed on — so the
    // recommended bulk authoring path wrote a correct .uasset to disk and left the terrain
    // rendering the old shader map, with every field of the response saying success.
    // Same defect and same fix as material.authoring.compile_material (0fe35187); this is
    // the door that fix did not cover.
    PinWright::MaterialConsumers::NotifyMasterMaterialChanged(Material);
    Material->ForceRecompileForRendering();
    // After ForceRecompileForRendering, so the components build their instances against a
    // master whose shader maps are already current.
    PinWright::MaterialConsumers::RefreshLandscapeConsumers(Material, Finalized.ConsumerRefresh);
    PinWright::MaterialConsumers::AddKnownUnrefreshedConsumers(Finalized.ConsumerRefresh);

    // MarkPackageDirty (including PreEditChange's call) is suppressed while
    // GIsPlayInEditorWorld is true. compile_mgir deliberately retains this editor-asset edit
    // when PIE blocks the save, so set the package flag directly before the save attempt.
    Material->GetOutermost()->SetDirtyFlag(true);

    if (Options.bSave)
    {
        // Was `if (!McpSafeAssetSave(Material))` — an error branch that could never be
        // taken, because that helper returned the literal true for any non-null pointer
        // (Material is null-checked at the top of this function). So bSave promised a
        // save, performed a mark-dirty, and reported success unconditionally.
        // Materials are not Blueprints, so the bulkdata-corruption vector that forces
        // the deferred mark-dirty elsewhere does not apply here — the same reasoning
        // MaterialAuthoringHandler.cpp:286-303 records for SaveMaterialAssetToDisk.
        // Do the write for real and let the on-disk probe decide.
        const bool bSavedToDisk = SaveAssetToDiskReportingPresence(
            Material, /*bForce=*/true, nullptr, nullptr, &Finalized.SaveState);
        if (!bSavedToDisk && Finalized.SaveState != EAssetSaveState::BlockedByPie)
        {
            return FMGIRCompileResult::MakeError(
                TEXT("MGIR_SAVE_FAILED"),
                FString::Printf(TEXT("Material compiled but no .uasset reached disk: %s"), *Material->GetPathName()));
        }
    }

    return Finalized;
}

FMGIRCompileResult FinalizeMaterialFunction(UMaterialFunction* Function, const FMGIRCompileOptions& Options)
{
    if (!Function)
    {
        return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Material function is null."));
    }

    if (Options.bRunLayout)
    {
        FMGIRLayoutEngine::Layout(Function);
    }

    Function->PreEditChange(nullptr);
    Function->UpdateFromFunctionResource();
    {
        FMaterialUpdateContext UpdateContext;
        Function->ForceRecompileForRendering(UpdateContext, Cast<UMaterial>(Function->GetPreviewMaterial()));
    }
    Function->PostEditChange();

    // Same retained-edit contract as the material path above.
    Function->GetOutermost()->SetDirtyFlag(true);

    if (Options.bSave)
    {
        // Same dead-branch fix as the material path above: McpSafeAssetSave's bool was a
        // constant, so this could never report a failure. Material functions are not
        // Blueprints either (MaterialAuthoringHandler.cpp:305-307 saves them through the
        // same disk-gated helper), so write for real and gate on the .uasset landing.
        FMGIRCompileResult Finalized;
        const bool bSavedToDisk = SaveAssetToDiskReportingPresence(
            Function, /*bForce=*/true, nullptr, nullptr, &Finalized.SaveState);
        if (!bSavedToDisk && Finalized.SaveState != EAssetSaveState::BlockedByPie)
        {
            return FMGIRCompileResult::MakeError(
                TEXT("MGIR_SAVE_FAILED"),
                FString::Printf(TEXT("Material function compiled but no .uasset reached disk: %s"), *Function->GetPathName()));
        }
        return Finalized;
    }

    return FMGIRCompileResult();
}

FMGIRCompileResult EmitInstruction(
    const FMGIRInstruction& Instruction,
    FMGIRExpressionEmitter& Emitter,
    TArray<FMGIRPendingWire>& PendingWires,
    TArray<FMGIRPendingOutput>& PendingOutputs,
    int32& ExpressionsCreated)
{
    FMGIREmitResult EmitResult;

    switch (Instruction.Opcode)
    {
    case EMGIROpcode::Call:
    {
        TSharedPtr<FJsonObject> Properties;
        TArray<FString> AttributeNames;
        TArray<FString> LegacyAttributeTypes;
        TArray<FString> DeclaredInputNames;
        UClass* ExpressionClass = FMaterialExpressionFactory::ResolveExpressionClass(Instruction.SymbolName);
        const bool bSetMaterialAttributes =
            ExpressionClass == UMaterialExpressionSetMaterialAttributes::StaticClass();
        const bool bDeclaresDynamicInputs =
            MGIRDynamicInputs::DeclaresInputsFromCallArgs(ExpressionClass);
        if (bSetMaterialAttributes)
        {
            SplitSetMaterialAttributesArgs(
                Instruction,
                Properties,
                PendingWires,
                AttributeNames,
                LegacyAttributeTypes);
        }
        else
        {
            if (bDeclaresDynamicInputs)
            {
                // The named pin arguments ARE this class's input-pin declaration, so they are
                // read here before SplitCallArgs turns them into wires - the pins have to
                // exist by the time the wire pass runs.
                const FString InputArrayArgName =
                    MGIRDynamicInputs::GetInputArrayPropertyName(ExpressionClass).ToString();
                for (const FMGIRArg& Arg : Instruction.Args)
                {
                    if (Arg.Name.Equals(InputArrayArgName, ESearchCase::IgnoreCase))
                    {
                        return FMGIRCompileResult::MakeError(
                            TEXT("MGIR_INVALID_INPUT_DECLARATION"),
                            FString::Printf(
                                TEXT("Line %d: '%s' is not a settable property on %s. Its input pins are ")
                                TEXT("declared by the call's named pin arguments (e.g. `UV: %%uv`), which ")
                                TEXT("name and wire each one; a decompiled document no longer restates them ")
                                TEXT("as an array. Drop the argument and re-decompile if the text is pinned."),
                                Instruction.SourceLine,
                                *InputArrayArgName,
                                *ExpressionClass->GetName()));
                    }

                    if (!Arg.Name.IsEmpty() && IsPinReference(Arg.Value))
                    {
                        DeclaredInputNames.Add(Arg.Name);
                    }
                }
            }

            SplitCallArgs(Instruction, Properties, PendingWires);
        }

        FMGIRExpressionSpec Spec;
        Spec.Name = Instruction.ResultName;
        Spec.ExpressionClassName = Instruction.SymbolName;
        Spec.ExpressionClass = ExpressionClass;
        Spec.Properties = Properties;
        Spec.Position = Instruction.Position;
        EmitResult = Emitter.EmitExpression(Spec);
        if (EmitResult.IsSuccess() && bSetMaterialAttributes)
        {
            UMaterialExpressionSetMaterialAttributes* SetAttributes =
                Cast<UMaterialExpressionSetMaterialAttributes>(EmitResult.Expression);
            if (!SetAttributes)
            {
                return FMGIRCompileResult::MakeError(
                    TEXT("MGIR_CREATE_FAILED"),
                    TEXT("SetMaterialAttributes instruction did not create a SetMaterialAttributes expression."));
            }

            FString AttributeError;
            if (!AttributeNames.IsEmpty())
            {
                if (!FMGIRMaterialAttributeUtils::ApplyAttributeNames(SetAttributes, AttributeNames, AttributeError))
                {
                    return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_ATTRIBUTE"), AttributeError);
                }
            }
            else if (!LegacyAttributeTypes.IsEmpty())
            {
                if (!FMGIRMaterialAttributeUtils::ApplyAttributeNames(SetAttributes, LegacyAttributeTypes, AttributeError))
                {
                    return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_ATTRIBUTE"), AttributeError);
                }
            }
            else if (!SetAttributes->AttributeSetTypes.IsEmpty())
            {
                FMGIRMaterialAttributeUtils::RebuildInputsFromAttributeSetTypes(SetAttributes);
            }
        }

        if (EmitResult.IsSuccess() && bDeclaresDynamicInputs)
        {
            FString InputDeclarationError;
            if (!MGIRDynamicInputs::ApplyDeclaredInputNames(
                    EmitResult.Expression,
                    DeclaredInputNames,
                    InputDeclarationError))
            {
                return FMGIRCompileResult::MakeError(
                    TEXT("MGIR_INVALID_INPUT_DECLARATION"),
                    FString::Printf(TEXT("Line %d: %s"), Instruction.SourceLine, *InputDeclarationError));
            }
        }
        break;
    }
    case EMGIROpcode::Constant:
    {
        TArray<double> Values;
        FString Error;
        if (!TryReadConstantValues(Instruction, Values, Error))
        {
            return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_CONSTANT"), Error);
        }

        FMGIRConstantSpec Spec;
        Spec.Name = Instruction.ResultName;
        Spec.Values = Values;
        Spec.Position = Instruction.Position;
        EmitResult = Emitter.EmitConstant(Spec);
        break;
    }
    case EMGIROpcode::FunctionCall:
    {
        FMGIRFunctionCallSpec Spec;
        Spec.Name = Instruction.ResultName;
        Spec.FunctionPath = DecodeStringLiteral(Instruction.SymbolName);
        Spec.Position = Instruction.Position;
        EmitResult = Emitter.EmitFunctionCall(Spec);

        for (const FMGIRArg& Arg : Instruction.Args)
        {
            if (!Arg.Name.IsEmpty() && IsPinReference(Arg.Value))
            {
                FMGIRPendingWire Wire;
                Wire.TargetName = Instruction.ResultName;
                Wire.InputName = Arg.Name;
                Wire.SourceReference = Arg.Value;
                Wire.SourceLine = Instruction.SourceLine;
                PendingWires.Add(MoveTemp(Wire));
            }
        }
        break;
    }
    case EMGIROpcode::Reroute:
    {
        FString SourceReference;
        FString DisplayName = Instruction.SymbolName;
        for (const FMGIRArg& Arg : Instruction.Args)
        {
            if (Arg.Name.Equals(TEXT("Source"), ESearchCase::IgnoreCase))
            {
                SourceReference = Arg.Value;
            }
            else if (Arg.Name.Equals(TEXT("DisplayName"), ESearchCase::IgnoreCase))
            {
                DisplayName = DecodeStringLiteral(Arg.Value);
            }
        }

        if (!SourceReference.IsEmpty())
        {
            FMGIRRerouteDeclarationSpec Spec;
            Spec.Name = Instruction.ResultName;
            Spec.SourceReference = SourceReference;
            Spec.Position = Instruction.Position;
            EmitResult = Emitter.EmitNamedRerouteDeclaration(Spec);
        }
        else
        {
            FMGIRRerouteUsageSpec Spec;
            Spec.Name = Instruction.ResultName;
            Spec.DeclarationName = DisplayName;
            Spec.Position = Instruction.Position;
            EmitResult = Emitter.EmitNamedRerouteUsage(Spec);
        }
        break;
    }
    case EMGIROpcode::LayerStack:
    {
        FMGIRLayerStackSpec Spec;
        Spec.Name = Instruction.ResultName;
        Spec.Position = Instruction.Position;

        for (const FMGIRArg& Arg : Instruction.Args)
        {
            if (Arg.Name.StartsWith(TEXT("Layer"), ESearchCase::IgnoreCase))
            {
                Spec.LayerFunctionPaths.Add(DecodeStringLiteral(Arg.Value));
            }
            else if (Arg.Name.StartsWith(TEXT("Blend"), ESearchCase::IgnoreCase))
            {
                Spec.BlendFunctionPaths.Add(DecodeStringLiteral(Arg.Value));
            }
            else if (Arg.Name.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
            {
                Spec.InputReference = Arg.Value;
            }
        }

        EmitResult = Emitter.EmitLayerStack(Spec);
        break;
    }
    case EMGIROpcode::Property:
        // Material-level properties are applied by CompileMaterialBlock, before the graph is
        // emitted and against the material itself; there is nothing to emit here. Not counted
        // as an expression - expressionsCreated stays a count of graph nodes.
        return FMGIRCompileResult();
    case EMGIROpcode::Output:
    {
        FMGIRPendingOutput Output;
        Output.OutputName = Instruction.OutputName;
        Output.SourceReference = Instruction.Value;
        Output.SourceLine = Instruction.SourceLine;
        Output.Position = Instruction.Position;
        PendingOutputs.Add(MoveTemp(Output));
        return FMGIRCompileResult();
    }
    default:
        return FMGIRCompileResult::MakeError(
            TEXT("MGIR_UNSUPPORTED_OPCODE"),
            FString::Printf(TEXT("Unsupported MGIR opcode at line %d."), Instruction.SourceLine));
    }

    if (!EmitResult.IsSuccess())
    {
        return FMGIRCompileResult::MakeError(EmitResult.ErrorCode, EmitResult.ErrorMessage);
    }

    ++ExpressionsCreated;
    return FMGIRCompileResult();
}

FMGIRCompileResult CompileBlockInstructions(
    const FMGIREntryBlock& Block,
    FMGIRExpressionEmitter& Emitter,
    int32& ExpressionsCreated)
{
    TArray<FMGIRPendingWire> PendingWires;
    TArray<FMGIRPendingOutput> PendingOutputs;
    ExpressionsCreated = 0;

    for (const FMGIRInstruction& Instruction : Block.Instructions)
    {
        FMGIRCompileResult EmitResult = EmitInstruction(
            Instruction,
            Emitter,
            PendingWires,
            PendingOutputs,
            ExpressionsCreated);
        if (!EmitResult.ErrorCode.IsEmpty())
        {
            return EmitResult;
        }
    }

    for (const FMGIRPendingWire& Wire : PendingWires)
    {
        FString InputName = Wire.InputName;
        if (UMaterialExpressionSetMaterialAttributes* SetAttributes =
            Cast<UMaterialExpressionSetMaterialAttributes>(Emitter.FindSymbol(Wire.TargetName)))
        {
            FString ResolvedInputName;
            if (FMGIRMaterialAttributeUtils::TryGetWireInputName(SetAttributes, Wire.InputName, ResolvedInputName))
            {
                InputName = ResolvedInputName;
            }
        }

        FMGIREmitResult WireResult = Emitter.WireExpressionInput({
            Wire.TargetName,
            InputName,
            Wire.SourceReference,
        });
        if (!WireResult.IsSuccess())
        {
            return FMGIRCompileResult::MakeError(WireResult.ErrorCode, WireResult.ErrorMessage);
        }
    }

    for (const FMGIRPendingOutput& Output : PendingOutputs)
    {
        FMGIREmitResult WireResult = Emitter.WireRootOutput({
            Output.OutputName,
            Output.SourceReference,
            Output.Position,
        });
        if (!WireResult.IsSuccess())
        {
            return FMGIRCompileResult::MakeError(WireResult.ErrorCode, WireResult.ErrorMessage);
        }
    }

    return FMGIRCompileResult();
}

// Validates every `property` line in the block WITHOUT touching an asset. Append mode empties
// the target's expression collection before anything else runs, so a document with one bad
// property name must fail before that happens rather than half-way through.
FMGIRCompileResult CollectMaterialProperties(
    const FMGIREntryBlock& Block,
    TArray<MGIRMaterialProperties::FParsed>& OutProperties)
{
    for (const FMGIRInstruction& Instruction : Block.Instructions)
    {
        if (Instruction.Opcode != EMGIROpcode::Property)
        {
            continue;
        }

        MGIRMaterialProperties::FParsed Parsed;
        const MGIRMaterialProperties::FResult ParseResult =
            MGIRMaterialProperties::ParseProperty(Instruction.SymbolName, Instruction.Value, Parsed);
        if (!ParseResult.IsSuccess())
        {
            return FMGIRCompileResult::MakeError(
                ParseResult.ErrorCode,
                FString::Printf(TEXT("Line %d: %s"), Instruction.SourceLine, *ParseResult.ErrorMessage));
        }

        OutProperties.Add(Parsed);
    }

    return FMGIRCompileResult();
}

FMGIRCompileResult ApplyMaterialProperties(
    UMaterial* Material,
    const TArray<MGIRMaterialProperties::FParsed>& Properties)
{
    if (Properties.IsEmpty())
    {
        return FMGIRCompileResult();
    }

    for (const MGIRMaterialProperties::FParsed& Parsed : Properties)
    {
        const MGIRMaterialProperties::FResult ApplyResult =
            MGIRMaterialProperties::ApplyProperty(Material, Parsed);
        if (!ApplyResult.IsSuccess())
        {
            return FMGIRCompileResult::MakeError(ApplyResult.ErrorCode, ApplyResult.ErrorMessage);
        }
    }

    return FMGIRCompileResult();
}

FMGIRCompileResult CompileMaterialBlock(
    const FMGIREntryBlock& Block,
    const FMGIRCompileOptions& Options,
    FMGIRCompiledBlock& OutBlock)
{
    TArray<MGIRMaterialProperties::FParsed> Properties;
    FMGIRCompileResult PropertyParseResult = CollectMaterialProperties(Block, Properties);
    if (!PropertyParseResult.ErrorCode.IsEmpty())
    {
        return PropertyParseResult;
    }

    const FString AssetPath = NormalizeEntryTarget(Block, Options);
    if (AssetPath.IsEmpty())
    {
        return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Material entry has no target asset path."));
    }

    UMaterial* Material = nullptr;
    FMGIRCompileResult LoadResult = GetOrCreateMaterial(AssetPath, Material);
    if (!LoadResult.ErrorCode.IsEmpty())
    {
        return LoadResult;
    }

    if (Options.Mode == EMGIRCompileMode::Append)
    {
        ClearMaterialGraph(Material);
    }

    FMGIRExpressionEmitter Emitter(Material);
    if (Options.Mode == EMGIRCompileMode::Extend)
    {
        Emitter.GuardExistingExpressionHandles();
    }

    int32 ExpressionsCreated = 0;
    FMGIRCompileResult InstructionsResult = CompileBlockInstructions(Block, Emitter, ExpressionsCreated);
    if (!InstructionsResult.ErrorCode.IsEmpty())
    {
        return InstructionsResult;
    }

    // Applied after the instructions, in BOTH modes, and only for properties the document
    // actually names: an absent property leaves the target's current value alone, so a
    // hand-written fragment cannot silently reset a master's blend mode to the UMaterial
    // default. A decompiled document names all of them, which is what makes the round trip
    // exact. After rather than before, so an instruction that fails - an Extend handle that
    // already exists, an unknown class - leaves the material's settings untouched instead of
    // half-written by a call that reported failure.
    FMGIRCompileResult PropertyApplyResult = ApplyMaterialProperties(Material, Properties);
    if (!PropertyApplyResult.ErrorCode.IsEmpty())
    {
        return PropertyApplyResult;
    }

    // The derived shading-model field is read off the expressions when ShadingModel is
    // MSM_FromMaterialExpression, so it needs the graph; and it must run before
    // FinalizeMaterial, which notifies every consumer caching instances of this master.
    if (!Properties.IsEmpty())
    {
        MGIRMaterialProperties::FinalizeAppliedProperties(Material);
    }

    FMGIRCompileResult FinalizeResult = FinalizeMaterial(Material, Options);
    if (!FinalizeResult.ErrorCode.IsEmpty())
    {
        return FinalizeResult;
    }

    OutBlock.AssetPath = Material->GetPathName();
    OutBlock.ExpressionsCreated = ExpressionsCreated;
    OutBlock.SaveState = FinalizeResult.SaveState;
    OutBlock.ConsumerRefresh = FinalizeResult.ConsumerRefresh;
    return FMGIRCompileResult();
}

FMGIRCompileResult CompileFunctionBlock(
    const FMGIREntryBlock& Block,
    const FMGIRCompileOptions& Options,
    FMGIRCompiledBlock& OutBlock)
{
    for (const FMGIRInstruction& Instruction : Block.Instructions)
    {
        if (Instruction.Opcode == EMGIROpcode::Property)
        {
            return FMGIRCompileResult::MakeError(
                TEXT("MGIR_UNKNOWN_PROPERTY"),
                FString::Printf(
                    TEXT("Line %d: 'property' is only valid in an entry material block; a ")
                    TEXT("material function has no material-level properties."),
                    Instruction.SourceLine));
        }
    }

    const FString AssetPath = NormalizeEntryTarget(Block, Options);
    if (AssetPath.IsEmpty())
    {
        return FMGIRCompileResult::MakeError(TEXT("MGIR_INVALID_TARGET"), TEXT("Function entry has no target asset path."));
    }

    UMaterialFunction* Function = nullptr;
    FMGIRCompileResult LoadResult = GetOrCreateMaterialFunction(AssetPath, Function);
    if (!LoadResult.ErrorCode.IsEmpty())
    {
        return LoadResult;
    }

    // A function's inputs and outputs are addressed by GUID, not by name or index: a caller caches
    // FFunctionExpressionInput::ExpressionInputId / FFunctionExpressionOutput::ExpressionOutputId
    // and re-links purely by them in UpdateFromFunctionResource, dropping any wire whose GUID no
    // longer matches. Append empties the graph and re-emits it, so every pin comes back as a NEW
    // object with a NEW GUID (ConditionallyGenerateId is FGuid::NewGuid outside a cook) — which
    // silently disconnects every material already saved against this function, without touching
    // those materials or producing any error. The pin NAME is the one handle a rebuild preserves,
    // and the engine already treats a rename as identity-preserving in the other direction, so
    // identity is carried across the rebuild by name.
    PinWright::MaterialFunctionIdentity::FIdSnapshot PinIds;
    if (Options.Mode == EMGIRCompileMode::Append)
    {
        PinWright::MaterialFunctionIdentity::CaptureIds(Function, PinIds);
        ClearMaterialFunctionGraph(Function);
    }

    FMGIRExpressionEmitter Emitter(Function);
    if (Options.Mode == EMGIRCompileMode::Extend)
    {
        Emitter.GuardExistingExpressionHandles();
    }

    int32 ExpressionsCreated = 0;
    FMGIRCompileResult InstructionsResult = CompileBlockInstructions(Block, Emitter, ExpressionsCreated);
    if (!InstructionsResult.ErrorCode.IsEmpty())
    {
        return InstructionsResult;
    }

    // Before Finalize: FinalizeMaterialFunction is what propagates the rebuilt graph to loaded
    // callers, so the pins must already carry their previous identity by then.
    TArray<FString> OrphanedPins;
    PinWright::MaterialFunctionIdentity::RestoreIds(Function, PinIds, &OrphanedPins);
    PinWright::MaterialFunctionIdentity::EnsurePersistentIds(Function);
    if (OrphanedPins.Num() > 0)
    {
        // A pin the rebuild renamed or dropped keeps no identity, so this compile has just
        // disconnected every material already saved against it - silently, in those materials,
        // on their next load. Naming it here is the only moment the disconnection is visible.
        OutBlock.Warnings.Add(FString::Printf(
            TEXT("%s: %s existed before this rebuild and no pin of that name exists after it, so ")
            TEXT("the previous pin identity could not be carried over. Every material already saved ")
            TEXT("against %s pin(s) loses that wire on its next load, with no error. Restore the ")
            TEXT("name in the document, or re-wire and re-save each calling material."),
            *Function->GetPathName(),
            *FString::Join(OrphanedPins, TEXT(", ")),
            OrphanedPins.Num() == 1 ? TEXT("that") : TEXT("those")));
    }

    FMGIRCompileResult FinalizeResult = FinalizeMaterialFunction(Function, Options);
    if (!FinalizeResult.ErrorCode.IsEmpty())
    {
        return FinalizeResult;
    }

    OutBlock.AssetPath = Function->GetPathName();
    OutBlock.ExpressionsCreated = ExpressionsCreated;
    OutBlock.SaveState = FinalizeResult.SaveState;
    return FMGIRCompileResult();
}
}

FMGIRCompileResult FMGIRCompiler::Compile(const FString& Text, const FMGIRCompileOptions& Options)
{
    FMGIRParser Parser;
    TArray<FMGIREntryBlock> Blocks;
    TArray<FMGIRParseError> Errors;
    if (!Parser.Parse(Text, Blocks, Errors))
    {
        const FMGIRParseError& Error = Errors.IsValidIndex(0) ? Errors[0] : FMGIRParseError();
        return FMGIRCompileResult::MakeError(
            Error.Code.IsEmpty() ? TEXT("MGIR_PARSE_ERROR") : Error.Code,
            Error.Message.IsEmpty() ? TEXT("Failed to parse MGIR text.") : Error.Message);
    }

    FMGIRCompileResult Result;
    for (const FMGIREntryBlock& Block : Blocks)
    {
        FMGIRCompiledBlock CompiledBlock;
        FMGIRCompileResult BlockResult;
        if (Block.Kind == EMGIREntryKind::Material)
        {
            BlockResult = CompileMaterialBlock(Block, Options, CompiledBlock);
        }
        else
        {
            BlockResult = CompileFunctionBlock(Block, Options, CompiledBlock);
        }

        if (!BlockResult.ErrorCode.IsEmpty())
        {
            return BlockResult;
        }

        Result.AssetPaths.Add(CompiledBlock.AssetPath);
        Result.ExpressionsCreated += CompiledBlock.ExpressionsCreated;
        Result.ConsumerRefresh.Accumulate(CompiledBlock.ConsumerRefresh);
        Result.Warnings.Append(CompiledBlock.Warnings);
        if (Result.SaveState == EAssetSaveState::NotRequested
            || CompiledBlock.SaveState == EAssetSaveState::BlockedByPie
            || (Result.SaveState == EAssetSaveState::AlreadyCurrent
                && CompiledBlock.SaveState == EAssetSaveState::Written))
        {
            Result.SaveState = CompiledBlock.SaveState;
        }
        ++Result.BlocksCompiled;
    }

    Result.bSuccess = true;
    return Result;
}
