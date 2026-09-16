// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrCompileDiagnostic.h"

enum class EMGIROpcode : uint8
{
    Call,
    Output,
    Reroute,
    FunctionCall,
    LayerStack,
    Constant,
    // `property <Name>: <Value>` - material-level UMaterial state (blend mode, shading
    // model, two-sided, domain, translucency lighting mode, ...) rather than a graph node.
    // Valid only inside an `entry material` block.
    Property,
};

enum class EMGIREntryKind : uint8
{
    Material,
    Function,
};

enum class EMGIRValueType : uint8
{
    Unknown,
    Float1,
    Float2,
    Float3,
    Float4,
    Texture2D,
    MaterialAttributes,
};

struct FMGIRArg
{
    FString Name;
    FString Value;
};

struct FMGIRParseError : public FIrCompileDiagnostic
{
    FString Code;

    FMGIRParseError() = default;
    FMGIRParseError(int32 InLine, const FString& InMessage, const FString& InCode = FString())
        : FIrCompileDiagnostic(InLine, InMessage)
        , Code(InCode)
    {
    }
};

struct FMGIRInstruction
{
    EMGIROpcode Opcode = EMGIROpcode::Call;
    FString ResultName;
    FString SymbolName;
    FString OutputName;
    FString Value;
    FString TypeText;
    EMGIRValueType ValueType = EMGIRValueType::Unknown;
    TArray<FMGIRArg> Args;
    int32 SourceLine = -1;
    bool bHasPosition = false;
    FVector2D Position = FVector2D::ZeroVector;
};

struct FMGIREntryBlock
{
    EMGIREntryKind Kind = EMGIREntryKind::Material;
    FString Name;
    TArray<FMGIRInstruction> Instructions;
    TMap<FString, int32> ValueIndex;
};
