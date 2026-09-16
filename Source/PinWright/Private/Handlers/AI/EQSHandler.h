// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FHandlerContext;

namespace PinWrightEQS
{
    bool HandleCreate(FHandlerContext& Ctx);
    bool HandleAddGenerator(FHandlerContext& Ctx);
    bool HandleAddTest(FHandlerContext& Ctx);
    bool HandleSetContextClass(FHandlerContext& Ctx);
    bool HandleSetTestFilter(FHandlerContext& Ctx);
    bool HandleSetTestScoring(FHandlerContext& Ctx);
}
