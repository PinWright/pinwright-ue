// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"


struct FMSIRClassRef
{
    FString Namespace;
    FString Name;
    int32 Major = 1;
    int32 Minor = 0;
};

struct FMSIRInput
{
    FString TypeName;
    FString Name;
    bool bHasDefault = false;
    FString DefaultLiteral;
};

struct FMSIROutput
{
    FString TypeName;
    FString Name;
};

struct FMSIRVariable
{
    FString TypeName;
    FString Name;
    FString Sigil;
    FString Literal;
};

struct FMSIRNodePortLiteral
{
    FString InputName;
    FString Literal;
};

struct FMSIRNode
{
    FString Local;
    FMSIRClassRef ClassRef;
    bool bHasPosition = false;
    int32 X = 0;
    int32 Y = 0;
    TArray<FMSIRNodePortLiteral> InputLiterals;
};

struct FMSIRWire
{
    FString From;
    FString To;
};

struct FMSIRInterfaceVersion
{
    FString Namespace;
    FString Name;
    int32 Major = 1;
    int32 Minor = 0;
};

struct FMSIRDocument
{
    FString LocalName;
    FString Kind;
    bool bIsPreset = false;
    FString BasedOn;
    TArray<FMSIRInterfaceVersion> Interfaces;
    TArray<FMSIRInput> Inputs;
    TArray<FMSIROutput> Outputs;
    TArray<FMSIRVariable> Variables;
    TArray<FMSIRNode> Nodes;
    TArray<FMSIRWire> Wires;
    TArray<FMSIRInput> PresetOverrides;
};
