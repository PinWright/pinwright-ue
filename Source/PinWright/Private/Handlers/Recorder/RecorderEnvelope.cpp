// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderEnvelope.h"

namespace RecorderEnvelope
{

TSharedRef<FJsonObject> Build(const FEnvelopeMeta& Meta)
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

    if (Meta.bHasUnits)
    {
        Obj->SetStringField(TEXT("units"), Meta.Units);
    }
    if (Meta.bHasTimeDomain)
    {
        Obj->SetStringField(TEXT("timeDomain"), Meta.TimeDomain);
    }
    if (Meta.bHasTimeRange)
    {
        TArray<TSharedPtr<FJsonValue>> Range;
        Range.Add(MakeShared<FJsonValueNumber>(Meta.TimeRangeFrom));
        Range.Add(MakeShared<FJsonValueNumber>(Meta.TimeRangeTo));
        Obj->SetArrayField(TEXT("timeRangeCovered"), Range);
    }
    if (Meta.bHasReduction)
    {
        Obj->SetStringField(TEXT("reduction"), Meta.Reduction);
    }
    if (Meta.bHasThreshold)
    {
        Obj->SetNumberField(TEXT("thresholdApplied"), Meta.ThresholdApplied);
    }

    Obj->SetNumberField(TEXT("elided"), Meta.Elided);
    Obj->SetBoolField(TEXT("truncated"), Meta.bTruncated);
    Obj->SetNumberField(TEXT("totalCount"), Meta.TotalCount);

    if (Meta.bHasNextCursor)
    {
        Obj->SetStringField(TEXT("nextCursor"), Meta.NextCursor);
    }
    else
    {
        Obj->SetField(TEXT("nextCursor"), MakeShared<FJsonValueNull>());
    }

    Obj->SetNumberField(TEXT("schemaVersion"), SchemaVersion);
    return Obj;
}

} // namespace RecorderEnvelope
