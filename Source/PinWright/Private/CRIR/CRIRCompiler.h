// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIRCompiler.h
//
// Compiles CRIR (Control Rig IR) text back into a `UControlRigBlueprint`'s
// RigVM graph models. Mirrors `FAGIRCompiler` shape.
//
// Scope: `unit` and `var` opcodes inside `rig_graph "<ModelName>" { ... }` blocks,
// and `bone` / `null` / `socket` elements inside `rig_hierarchy { ... }` blocks
// (round-trip via `URigHierarchyController`). The `control` element remains
// decompile-only — a non-empty `control` instruction returns
// `CRIR_HIERARCHY_NOT_WRITABLE` (tracked under F-crir-control-mutation-write).

#pragma once

#include "CoreMinimal.h"

enum class ECRIRCompileMode : uint8
{
    Replace,
    Extend,
};

struct FCRIRCompileOptions
{
    FString TargetAssetPath;
    ECRIRCompileMode Mode = ECRIRCompileMode::Replace;
    bool bRunLayout = true;
    bool bSave = false;
};

struct FCRIRCompileResult
{
    bool bSuccess = false;
    FString ErrorCode;
    FString ErrorMessage;
    FString AssetPath;
    int32 BlocksCompiled = 0;
    int32 NodesCreated = 0;
    TArray<FString> Warnings;

    static FCRIRCompileResult MakeError(const FString& InCode, const FString& InMessage)
    {
        FCRIRCompileResult Result;
        Result.ErrorCode = InCode;
        Result.ErrorMessage = InMessage;
        return Result;
    }
};

class PINWRIGHT_API FCRIRCompiler
{
public:
    static FCRIRCompileResult Compile(FStringView Text, const FCRIRCompileOptions& Options);
};
