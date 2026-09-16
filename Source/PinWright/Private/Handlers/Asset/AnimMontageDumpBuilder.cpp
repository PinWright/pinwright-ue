// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/AnimMontageDumpBuilder.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimLinkableElement.h"
#include "AlphaBlend.h"
#include "Handlers/Asset/AnimSequenceDumpBuilder.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

TSharedPtr<FJsonObject> AnimMontageDumpBuilder::BuildAnimMontageJson(const UAnimMontage* Montage)
{
    if (!Montage)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    // sections[] — FCompositeSection extends FAnimLinkableElement. LinkValue is protected; expose
    // the publicly-accessible GetLinkMethod() so callers can interpret startTime themselves (the raw
    // LinkValue is just startTime re-expressed in the section's own link reference frame).
    TArray<TSharedPtr<FJsonValue>> SectionsArr;
    SectionsArr.Reserve(Montage->CompositeSections.Num());
    for (const FCompositeSection& Section : Montage->CompositeSections)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("sectionName"), Section.SectionName.ToString());
        Entry->SetStringField(TEXT("nextSectionName"), Section.NextSectionName.ToString());
        Entry->SetNumberField(TEXT("startTime"), Section.GetTime());
        Entry->SetNumberField(TEXT("linkValue"), Section.GetTime(Section.GetLinkMethod()));
        SectionsArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("sections"), SectionsArr);

    // slots[] — each FSlotAnimationTrack has a SlotName + FAnimTrack with FAnimSegments[].
    TArray<TSharedPtr<FJsonValue>> SlotsArr;
    SlotsArr.Reserve(Montage->SlotAnimTracks.Num());
    for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
    {
        TSharedRef<FJsonObject> SlotObj = MakeShared<FJsonObject>();
        SlotObj->SetStringField(TEXT("slotName"), Slot.SlotName.ToString());

        TArray<TSharedPtr<FJsonValue>> SegmentsArr;
        SegmentsArr.Reserve(Slot.AnimTrack.AnimSegments.Num());
        for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
        {
            TSharedRef<FJsonObject> SegObj = MakeShared<FJsonObject>();
            SegObj->SetStringField(TEXT("animReference"), JsonBuilders::GetObjectPathSafe(Segment.GetAnimReference()));
            SegObj->SetNumberField(TEXT("startPos"), Segment.StartPos);
            SegObj->SetNumberField(TEXT("animStartTime"), Segment.AnimStartTime);
            SegObj->SetNumberField(TEXT("animEndTime"), Segment.AnimEndTime);
            SegObj->SetNumberField(TEXT("animPlayRate"), Segment.AnimPlayRate);
            SegObj->SetNumberField(TEXT("loopingCount"), Segment.LoopingCount);
            SegmentsArr.Add(MakeShared<FJsonValueObject>(SegObj));
        }
        SlotObj->SetArrayField(TEXT("segments"), SegmentsArr);
        SlotsArr.Add(MakeShared<FJsonValueObject>(SlotObj));
    }
    Root->SetArrayField(TEXT("slots"), SlotsArr);

    // notifies[] — reuse the shared anim-sequence notifies builder (takes UAnimSequenceBase*).
    Root->SetArrayField(TEXT("notifies"), AnimSequenceDumpBuilder::BuildNotifiesArrayJson(Montage));

    // blendIn / blendOut / bEnableAutoBlendOut — exposed UPROPERTYs on UAnimMontage.
    TSharedRef<FJsonObject> BlendIn = MakeShared<FJsonObject>();
    BlendIn->SetNumberField(TEXT("blendTime"), Montage->BlendIn.GetBlendTime());
    Root->SetObjectField(TEXT("blendIn"), BlendIn);

    TSharedRef<FJsonObject> BlendOut = MakeShared<FJsonObject>();
    BlendOut->SetNumberField(TEXT("blendTime"), Montage->BlendOut.GetBlendTime());
    Root->SetObjectField(TEXT("blendOut"), BlendOut);

    Root->SetBoolField(TEXT("bEnableAutoBlendOut"), Montage->bEnableAutoBlendOut);

    return Root;
}

namespace
{
    UClass* GetAnimMontageSidecarClass()
    {
        return UAnimMontage::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildAnimMontageSidecar(UObject* Asset)
    {
        return AnimMontageDumpBuilder::BuildAnimMontageJson(Cast<UAnimMontage>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("anim_montage"), DumpFileNames::AnimMontage,
    &GetAnimMontageSidecarClass, &BuildAnimMontageSidecar,
    nullptr, nullptr, nullptr, 100);
