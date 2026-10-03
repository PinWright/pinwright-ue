// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FHandlerContext;

namespace PinWrightEQS
{
    // bOverwrite is read by eqs.create's registration only; the ai.create_eqs_query shim keeps false.
    bool HandleCreate(FHandlerContext& Ctx, bool bOverwrite = false);
    bool HandleAddGenerator(FHandlerContext& Ctx);
    bool HandleAddTest(FHandlerContext& Ctx);
    bool HandleSetContextClass(FHandlerContext& Ctx);
    bool HandleSetTestFilter(FHandlerContext& Ctx);
    bool HandleSetTestScoring(FHandlerContext& Ctx);
}
