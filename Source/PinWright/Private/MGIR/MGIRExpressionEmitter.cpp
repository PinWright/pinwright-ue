// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRExpressionEmitter.h"


#include "MGIR/MGIRPinResolver.h"
#include "MGIR/MGIRHelpers.h"
#include "MGIR/MGIRExpressionUtils.h"
#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Material/MaterialLayerStackHelpers.h"
#include "Utils/GuardedLoad.h"

namespace
{
using MGIRHelpers::NormalizeSymbolName;

TSharedPtr<FJsonObject> EmptyProperties()
{
    return MakeShared<FJsonObject>();
}

TSharedPtr<FJsonObject> ConstantProperties(const FMGIRConstantSpec& Spec)
{
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    const double R = Spec.Values.IsValidIndex(0) ? Spec.Values[0] : 0.0;
    const double G = Spec.Values.IsValidIndex(1) ? Spec.Values[1] : 0.0;
    const double B = Spec.Values.IsValidIndex(2) ? Spec.Values[2] : 0.0;
    const double A = Spec.Values.IsValidIndex(3) ? Spec.Values[3] : 1.0;

    if (Spec.Values.Num() <= 1)
    {
        Properties->SetNumberField(TEXT("R"), R);
    }
    else if (Spec.Values.Num() == 2)
    {
        Properties->SetNumberField(TEXT("R"), R);
        Properties->SetNumberField(TEXT("G"), G);
    }
    else
    {
        TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
        Color->SetNumberField(TEXT("R"), R);
        Color->SetNumberField(TEXT("G"), G);
        Color->SetNumberField(TEXT("B"), B);
        Color->SetNumberField(TEXT("A"), A);
        Properties->SetObjectField(TEXT("Constant"), Color);
    }

    return Properties;
}

const TCHAR* ConstantClassNameForValueCount(int32 ValueCount)
{
    if (ValueCount <= 1)
    {
        return TEXT("Constant");
    }
    if (ValueCount == 2)
    {
        return TEXT("Constant2Vector");
    }
    if (ValueCount == 3)
    {
        return TEXT("Constant3Vector");
    }
    return TEXT("Constant4Vector");
}

FCreateResult CreateTargetExpression(
    UMaterial* Material,
    UMaterialFunction* Function,
    UClass* ExpressionClass,
    const TSharedPtr<FJsonObject>& Properties,
    const FVector2D& Position)
{
    return Function
        ? FMaterialExpressionFactory::Create(Function, ExpressionClass, Properties, Position)
        : FMaterialExpressionFactory::Create(Material, ExpressionClass, Properties, Position);
}

FCreateResult CreateTargetExpression(
    UMaterial* Material,
    UMaterialFunction* Function,
    const FString& ExpressionClassName,
    const TSharedPtr<FJsonObject>& Properties,
    const FVector2D& Position)
{
    return Function
        ? FMaterialExpressionFactory::Create(Function, ExpressionClassName, Properties, Position)
        : FMaterialExpressionFactory::Create(Material, ExpressionClassName, Properties, Position);
}
}

FMGIRExpressionEmitter::FMGIRExpressionEmitter(UMaterial* InMaterial)
    : Material(InMaterial)
{
}

FMGIRExpressionEmitter::FMGIRExpressionEmitter(UMaterialFunction* InFunction)
    : Function(InFunction)
{
}

FMGIREmitResult FMGIRExpressionEmitter::MakeError(const TCHAR* ErrorCode, const FString& ErrorMessage) const
{
    FMGIREmitResult Result;
    Result.ErrorCode = ErrorCode;
    Result.ErrorMessage = ErrorMessage;
    return Result;
}

UMaterialExpression* FMGIRExpressionEmitter::FindSymbol(const FString& Name) const
{
    UMaterialExpression* const* Expression = Symbols.Find(NormalizeSymbolName(Name));
    return Expression ? *Expression : nullptr;
}

void FMGIRExpressionEmitter::GuardExistingExpressionHandles()
{
    bGuardExistingHandles = true;

    TArray<UMaterialExpression*> Existing;
    if (Function)
    {
        MGIRExpressionUtils::CopyFunctionExpressions(Function, Existing);
    }
    else
    {
        MGIRExpressionUtils::CopyMaterialExpressions(Material, Existing);
    }

    for (UMaterialExpression* Expression : Existing)
    {
        if (!Expression)
        {
            continue;
        }

        const FString Handle =
            MGIRHelpers::ExpressionHandleName(Expression->MaterialExpressionGuid);
        if (!Handle.IsEmpty())
        {
            PreExistingHandles.Add(NormalizeSymbolName(Handle), Expression);
        }
    }
}

bool FMGIRExpressionEmitter::ValidateNewSymbol(const FString& Name, FMGIREmitResult& OutError) const
{
    const FString NormalizedName = NormalizeSymbolName(Name);
    if (!NormalizedName.IsEmpty() && Symbols.Contains(NormalizedName))
    {
        OutError = MakeError(TEXT("MGIR_DUPLICATE_SYMBOL"),
            FString::Printf(TEXT("MGIR symbol '%%%s' is already defined."), *Name));
        return false;
    }

    // Extend has no update-by-handle path: every Emit* below creates. A document naming an
    // expression that already exists is therefore asking for an update this mode cannot
    // perform, and the old behaviour was to create a duplicate and report success -- the
    // caller's intended value landed on a new orphan while the handle they named kept its
    // old value and stayed wired to the graph. Refuse instead, and name the remedy.
    if (bGuardExistingHandles && !NormalizedName.IsEmpty())
    {
        if (const UMaterialExpression* const* Existing = PreExistingHandles.Find(NormalizedName))
        {
            OutError = MakeError(TEXT("MGIR_EXTEND_CANNOT_UPDATE"),
                FString::Printf(
                    TEXT("MGIR symbol '%%%s' already exists in the target graph (%s). Extend mode ")
                    TEXT("only adds new expressions; it cannot update an existing one, and creating ")
                    TEXT("a second expression under that handle would leave the original wired and ")
                    TEXT("unchanged. Recompile the whole document in Append mode, or replace the node ")
                    TEXT("with material.graph.remove_node + add_expression + connect_nodes. If it is a ")
                    TEXT("parameter expression, material.authoring.set_*_parameter_value edits it in place."),
                    *Name,
                    *(*Existing)->GetClass()->GetName()));
            return false;
        }
    }

    return true;
}

FMGIREmitResult FMGIRExpressionEmitter::RegisterExpression(const FString& Name, UMaterialExpression* Expression)
{
    if (!Expression)
    {
        return MakeError(TEXT("MGIR_CREATE_FAILED"), TEXT("Expression creation failed."));
    }

    const FString NormalizedName = NormalizeSymbolName(Name);
    if (!NormalizedName.IsEmpty())
    {
        Symbols.Add(NormalizedName, Expression);
    }

    if (UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
    {
        // Conditional, never forced. Outside a cook ConditionallyGenerateId(true) is a fresh
        // FGuid::NewGuid(), which throws away the identity Append captured from the pin of the
        // same name — and that identity is the only thing an already-saved caller's wire into
        // this input survives on. The factory has already initialised a genuinely new pin.
        FunctionInput->ConditionallyGenerateId(false);
        FunctionInput->ValidateName();
    }

    FMGIREmitResult Result;
    Result.Expression = Expression;
    return Result;
}

FMGIREmitResult FMGIRExpressionEmitter::EmitExpression(const FMGIRExpressionSpec& Spec)
{
    FMGIREmitResult SymbolError;
    if (!ValidateNewSymbol(Spec.Name, SymbolError))
    {
        return SymbolError;
    }

    FCreateResult CreateResult = Spec.ExpressionClass
        ? CreateTargetExpression(Material, Function, Spec.ExpressionClass, Spec.Properties, Spec.Position)
        : CreateTargetExpression(Material, Function, Spec.ExpressionClassName, Spec.Properties, Spec.Position);
    if (!CreateResult.IsSuccess())
    {
        return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }

    if (UMaterialExpressionCollectionParameter* CollectionParameter =
        Cast<UMaterialExpressionCollectionParameter>(CreateResult.Expression))
    {
        const FGuid ParameterId = CollectionParameter->Collection
            ? CollectionParameter->Collection->GetParameterId(CollectionParameter->ParameterName)
            : FGuid();
        if (!ParameterId.IsValid())
        {
            const FString ErrorMessage = CollectionParameter->Collection
                ? FString::Printf(
                    TEXT("Parameter '%s' does not exist on collection '%s' for MGIR symbol '%%%s'."),
                    *CollectionParameter->ParameterName.ToString(),
                    *CollectionParameter->Collection->GetPathName(),
                    *Spec.Name)
                : FString::Printf(
                    TEXT("CollectionParameter MGIR symbol '%%%s' requires a valid Collection property."),
                    *Spec.Name);
            if (Function)
            {
                FMaterialExpressionFactory::DiscardExpression(Function, CreateResult.Expression);
            }
            else
            {
                FMaterialExpressionFactory::DiscardExpression(Material, CreateResult.Expression);
            }
            return MakeError(
                ErrorCodes::ERR_INVALID_PARAMS,
                ErrorMessage);
        }

        CollectionParameter->ParameterId = ParameterId;
    }

    return RegisterExpression(Spec.Name, CreateResult.Expression);
}

FMGIREmitResult FMGIRExpressionEmitter::EmitConstant(const FMGIRConstantSpec& Spec)
{
    FMGIREmitResult SymbolError;
    if (!ValidateNewSymbol(Spec.Name, SymbolError))
    {
        return SymbolError;
    }

    FCreateResult CreateResult = CreateTargetExpression(
        Material,
        Function,
        ConstantClassNameForValueCount(Spec.Values.Num()),
        ConstantProperties(Spec),
        Spec.Position);
    if (!CreateResult.IsSuccess())
    {
        return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }

    return RegisterExpression(Spec.Name, CreateResult.Expression);
}

FMGIREmitResult FMGIRExpressionEmitter::EmitFunctionCall(const FMGIRFunctionCallSpec& Spec)
{
    FMGIREmitResult SymbolError;
    if (!ValidateNewSymbol(Spec.Name, SymbolError))
    {
        return SymbolError;
    }

    // Spec.FunctionPath is parser output from the caller's MGIR text, which the dispatch boundary
    // must keep typed `string` - so the guard has to be here, at the load.
    FString FunctionRefusal;
    UMaterialFunctionInterface* LoadedFunction =
        PinWrightGuardedLoad::LoadObjectChecked<UMaterialFunctionInterface>(
            Spec.FunctionPath, &FunctionRefusal);
    if (!LoadedFunction)
    {
        return MakeError(TEXT("MGIR_FUNCTION_NOT_FOUND"),
            FunctionRefusal.IsEmpty()
                ? FString::Printf(TEXT("Could not load material function: %s"), *Spec.FunctionPath)
                : FunctionRefusal);
    }

    FCreateResult CreateResult = CreateTargetExpression(
        Material,
        Function,
        UMaterialExpressionMaterialFunctionCall::StaticClass(),
        EmptyProperties(),
        Spec.Position);
    if (!CreateResult.IsSuccess())
    {
        return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }

    UMaterialExpressionMaterialFunctionCall* FunctionCall =
        Cast<UMaterialExpressionMaterialFunctionCall>(CreateResult.Expression);
    if (!FunctionCall || !FunctionCall->SetMaterialFunction(LoadedFunction))
    {
        if (this->Function)
        {
            FMaterialExpressionFactory::DiscardExpression(this->Function, CreateResult.Expression);
        }
        else
        {
            FMaterialExpressionFactory::DiscardExpression(Material, CreateResult.Expression);
        }
        return MakeError(TEXT("MGIR_FUNCTION_SETUP_FAILED"),
            FString::Printf(TEXT("Failed to configure material function call: %s"), *Spec.FunctionPath));
    }

    FunctionCall->UpdateFromFunctionResource(true);
    return RegisterExpression(Spec.Name, FunctionCall);
}

FMGIREmitResult FMGIRExpressionEmitter::EmitNamedRerouteDeclaration(const FMGIRRerouteDeclarationSpec& Spec)
{
    FMGIREmitResult SymbolError;
    if (!ValidateNewSymbol(Spec.Name, SymbolError))
    {
        return SymbolError;
    }

    FMGIRResolvedPin Source;
    const bool bHasSource = !Spec.SourceReference.IsEmpty();
    if (bHasSource)
    {
        FMGIRWireResult ResolveResult = FMGIRPinResolver::ResolveReference(Spec.SourceReference, Symbols, Source);
        if (!ResolveResult.IsSuccess())
        {
            return MakeError(*ResolveResult.ErrorCode, ResolveResult.ErrorMessage);
        }
    }

    FCreateResult CreateResult = CreateTargetExpression(
        Material,
        Function,
        UMaterialExpressionNamedRerouteDeclaration::StaticClass(),
        EmptyProperties(),
        Spec.Position);
    if (!CreateResult.IsSuccess())
    {
        return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }

    UMaterialExpressionNamedRerouteDeclaration* Declaration =
        Cast<UMaterialExpressionNamedRerouteDeclaration>(CreateResult.Expression);
    if (!Declaration)
    {
        return MakeError(TEXT("MGIR_CREATE_FAILED"), TEXT("Failed to create named reroute declaration."));
    }

    Declaration->Name = FName(*Spec.Name);
    if (bHasSource)
    {
        FMGIRPinResolver::ApplyResolvedPin(Declaration->Input, Source);
    }

    FMGIREmitResult Result = RegisterExpression(Spec.Name, Declaration);
    if (Result.IsSuccess())
    {
        RerouteDeclarations.Add(NormalizeSymbolName(Spec.Name), Declaration);
    }
    return Result;
}

FMGIREmitResult FMGIRExpressionEmitter::EmitNamedRerouteUsage(const FMGIRRerouteUsageSpec& Spec)
{
    FMGIREmitResult SymbolError;
    if (!ValidateNewSymbol(Spec.Name, SymbolError))
    {
        return SymbolError;
    }

    UMaterialExpressionNamedRerouteDeclaration* const* Declaration =
        RerouteDeclarations.Find(NormalizeSymbolName(Spec.DeclarationName));
    if (!Declaration || !*Declaration)
    {
        return MakeError(TEXT("MGIR_REROUTE_NOT_FOUND"),
            FString::Printf(TEXT("Named reroute declaration '%%%s' was not found."), *Spec.DeclarationName));
    }

    FCreateResult CreateResult = CreateTargetExpression(
        Material,
        Function,
        UMaterialExpressionNamedRerouteUsage::StaticClass(),
        EmptyProperties(),
        Spec.Position);
    if (!CreateResult.IsSuccess())
    {
        return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }

    UMaterialExpressionNamedRerouteUsage* Usage =
        Cast<UMaterialExpressionNamedRerouteUsage>(CreateResult.Expression);
    if (!Usage)
    {
        return MakeError(TEXT("MGIR_CREATE_FAILED"), TEXT("Failed to create named reroute usage."));
    }

    Usage->Declaration = *Declaration;
    Usage->DeclarationGuid = (*Declaration)->VariableGuid;
    return RegisterExpression(Spec.Name, Usage);
}

FMGIREmitResult FMGIRExpressionEmitter::EmitLayerStack(const FMGIRLayerStackSpec& Spec)
{
    FMGIREmitResult SymbolError;
    if (!ValidateNewSymbol(Spec.Name, SymbolError))
    {
        return SymbolError;
    }

    TArray<UMaterialFunctionInterface*> Layers;
    for (const FString& LayerPath : Spec.LayerFunctionPaths)
    {
        FString LayerRefusal;
        UMaterialFunctionInterface* Layer =
            PinWrightGuardedLoad::LoadObjectChecked<UMaterialFunctionInterface>(
                LayerPath, &LayerRefusal);
        if (!Layer)
        {
            return MakeError(TEXT("MGIR_FUNCTION_NOT_FOUND"),
                LayerRefusal.IsEmpty()
                    ? FString::Printf(TEXT("Could not load layer function: %s"), *LayerPath)
                    : LayerRefusal);
        }
        Layers.Add(Layer);
    }

    TArray<UMaterialFunctionInterface*> Blends;
    for (const FString& BlendPath : Spec.BlendFunctionPaths)
    {
        FString BlendRefusal;
        UMaterialFunctionInterface* Blend =
            PinWrightGuardedLoad::LoadObjectChecked<UMaterialFunctionInterface>(
                BlendPath, &BlendRefusal);
        if (!Blend)
        {
            return MakeError(TEXT("MGIR_FUNCTION_NOT_FOUND"),
                BlendRefusal.IsEmpty()
                    ? FString::Printf(TEXT("Could not load blend function: %s"), *BlendPath)
                    : BlendRefusal);
        }
        Blends.Add(Blend);
    }

    FMGIRResolvedPin InputSource;
    const bool bHasInput = !Spec.InputReference.IsEmpty();
    if (bHasInput)
    {
        FMGIRWireResult ResolveResult = FMGIRPinResolver::ResolveReference(Spec.InputReference, Symbols, InputSource);
        if (!ResolveResult.IsSuccess())
        {
            return MakeError(*ResolveResult.ErrorCode, ResolveResult.ErrorMessage);
        }
    }

    FCreateResult CreateResult = CreateTargetExpression(
        Material,
        Function,
        UMaterialExpressionMaterialAttributeLayers::StaticClass(),
        EmptyProperties(),
        Spec.Position);
    if (!CreateResult.IsSuccess())
    {
        return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }

    UMaterialExpressionMaterialAttributeLayers* LayersExpression =
        Cast<UMaterialExpressionMaterialAttributeLayers>(CreateResult.Expression);
    if (!LayersExpression)
    {
        return MakeError(TEXT("MGIR_CREATE_FAILED"), TEXT("Failed to create material attribute layer stack."));
    }

    ApplyMaterialLayerStack(LayersExpression, Layers, Blends);

    if (bHasInput)
    {
        FMGIRPinResolver::ApplyResolvedPin(LayersExpression->Input, InputSource);
    }

    return RegisterExpression(Spec.Name, LayersExpression);
}

FMGIREmitResult FMGIRExpressionEmitter::WireExpressionInput(const FMGIRConnectionSpec& Spec)
{
    UMaterialExpression* TargetExpression = FindSymbol(Spec.TargetName);
    if (!TargetExpression)
    {
        return MakeError(TEXT("MGIR_SYMBOL_NOT_FOUND"),
            FString::Printf(TEXT("MGIR symbol '%%%s' was not found."), *Spec.TargetName));
    }

    FMGIRWireResult WireResult = FMGIRPinResolver::WireExpressionInput(
        TargetExpression,
        Spec.InputName,
        Spec.SourceReference,
        Symbols);
    if (!WireResult.IsSuccess())
    {
        return MakeError(*WireResult.ErrorCode, WireResult.ErrorMessage);
    }

    FMGIREmitResult Result;
    Result.Expression = TargetExpression;
    return Result;
}

FMGIREmitResult FMGIRExpressionEmitter::WireRootOutput(const FMGIRRootOutputSpec& Spec)
{
    FMGIRResolvedPin Source;
    FMGIRWireResult ResolveResult = FMGIRPinResolver::ResolveReference(
        Spec.SourceReference,
        Symbols,
        Source);
    if (!ResolveResult.IsSuccess())
    {
        return MakeError(*ResolveResult.ErrorCode, ResolveResult.ErrorMessage);
    }

    if (Function)
    {
        FCreateResult CreateResult = FMaterialExpressionFactory::Create(
            Function,
            UMaterialExpressionFunctionOutput::StaticClass(),
            EmptyProperties(),
            Spec.Position);
        if (!CreateResult.IsSuccess())
        {
            return MakeError(*CreateResult.ErrorCode, CreateResult.ErrorMessage);
        }

        UMaterialExpressionFunctionOutput* FunctionOutput =
            Cast<UMaterialExpressionFunctionOutput>(CreateResult.Expression);
        if (!FunctionOutput)
        {
            FMaterialExpressionFactory::DiscardExpression(Function, CreateResult.Expression);
            return MakeError(TEXT("MGIR_CREATE_FAILED"), TEXT("Failed to create material function output."));
        }

        FunctionOutput->OutputName = FName(*Spec.OutputName);
        // Conditional, never forced — same reason as the FunctionInput case in
        // RegisterExpression: a forced regenerate mints a new GUID and disconnects every caller
        // already saved against the output of this name.
        FunctionOutput->ConditionallyGenerateId(false);
        FMGIRPinResolver::ApplyResolvedPin(FunctionOutput->A, Source);
        FunctionOutput->ValidateName();

        FMGIREmitResult Result;
        Result.Expression = FunctionOutput;
        return Result;
    }

    FMGIRWireResult WireResult = FMGIRPinResolver::WireMaterialOutput(
        Material,
        Spec.OutputName,
        Source);
    if (!WireResult.IsSuccess())
    {
        return MakeError(*WireResult.ErrorCode, WireResult.ErrorMessage);
    }

    FMGIREmitResult Result;
    Result.Expression = Source.Expression;
    return Result;
}
