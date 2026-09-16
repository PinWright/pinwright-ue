// Copyright (c) 2026 Alexander Penkin. MIT License.

// CompilerTypes.h - Shared types for the Blueprint code compiler

#pragma once

#include "CoreMinimal.h"
#include "IrCore/IrCompileDiagnostic.h"

class UK2Node_Timeline;
class UEdGraphPin;

// Parameter info parsed from function signatures
struct FParameterInfo
{
    FString Type;
    FString Name;
};

// Holds pending timeline logic blocks for deferred processing
struct FTimelineLogicBlock
{
    UK2Node_Timeline* TimelineNode = nullptr;
    TArray<FString> OnUpdateCode;
    TArray<FString> OnFinishedCode;
    TMap<FString, UEdGraphPin*> LocalPinMap;
};

struct FCompileError : public FIrCompileDiagnostic
{
    FCompileError() = default;
    FCompileError(int32 InLine, const FString& InMessage)
        : FIrCompileDiagnostic(InLine, InMessage)
    {
    }
};

// Result of a compile operation
struct FCompileResult
{
    bool bSuccess = false;
    TArray<FCompileError> Errors;
    TArray<FString> Warnings;
    TArray<FGuid> CreatedNodeGUIDs;
    UEdGraphPin* LastExecOutputPin = nullptr;
    int32 OrphansRemoved = 0;

    static FCompileResult MakeError(int32 Line, const FString& Message)
    {
        FCompileResult Result;
        Result.bSuccess = false;
        Result.Errors.Add(FCompileError(Line, Message));
        return Result;
    }

    static FCompileResult MakeSuccess(const TArray<FGuid>& NodeGUIDs)
    {
        FCompileResult Result;
        Result.bSuccess = true;
        Result.CreatedNodeGUIDs = NodeGUIDs;
        return Result;
    }
};

// Classification of the parsed function/event signature
enum class ESignatureType : uint8
{
    Event,
    CustomEvent,
    ComponentEvent,
    WidgetEvent,
    KeyEvent,
    ConstructionScript,
    Timeline,
    Function
};

// Parsed signature data from the first line of a code block
struct FParsedSignature
{
    ESignatureType Type = ESignatureType::Function;
    FString Name;
    FString ReturnType;
    TArray<FParameterInfo> Params;
    FString ComponentName;
    FString WidgetName;
    FString KeyName;
    bool bKeyReleased = false;
};
