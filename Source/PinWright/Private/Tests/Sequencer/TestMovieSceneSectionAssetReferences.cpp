// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Animation/AnimSequence.h"
#include "Animation/MirrorDataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "UObject/Package.h"

#include "Compat/EngineVersionCompat.h"
#include "Utils/MovieSceneJsonUtils.h"

namespace
{
    bool TestJsonNullField(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName)
    {
        const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(FieldName) : nullptr;
        Test.TestTrue(FString::Printf(TEXT("%s is present"), FieldName), Value.IsValid());
        if (!Value.IsValid())
        {
            return false;
        }
        Test.TestEqual(FString::Printf(TEXT("%s is explicit JSON null"), FieldName),
            Value->Type, EJson::Null);
        return Value->Type == EJson::Null;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMovieSceneSectionAssetReferencesTest,
    "PinWright.Sequencer.SectionAssetReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMovieSceneSectionAssetReferencesTest::RunTest(const FString& Parameters)
{
    UMovieSceneSkeletalAnimationSection* Section = NewObject<UMovieSceneSkeletalAnimationSection>(
        GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("transient skeletal animation section"), Section))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Unassigned = MovieSceneJsonUtils::BuildSectionJson(Section);
    TestJsonNullField(*this, Unassigned, TEXT("animationPath"));
    TestJsonNullField(*this, Unassigned, TEXT("mirrorDataTablePath"));

    UAnimSequence* Animation = NewObject<UAnimSequence>(
        GetTransientPackage(), FName(TEXT("PW_SequencerAssetReferenceAnimation")), RF_Transient);
    UMirrorDataTable* MirrorDataTable = NewObject<UMirrorDataTable>(
        GetTransientPackage(), FName(TEXT("PW_SequencerAssetReferenceMirror")), RF_Transient);
    if (!TestNotNull(TEXT("transient animation asset"), Animation)
        || !TestNotNull(TEXT("transient mirror data table"), MirrorDataTable))
    {
        return false;
    }

    Section->Params.Animation = Animation;
    Section->Params.FirstLoopStartFrameOffset = FFrameNumber(7);
    Section->Params.StartFrameOffset = FFrameNumber(11);
    Section->Params.EndFrameOffset = FFrameNumber(13);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    Section->Params.PlayRate = 0.5;
#else
    Section->Params.PlayRate = 0.5f;
#endif
    Section->Params.bReverse = true;
    Section->Params.SlotName = FName(TEXT("DefaultSlot"));
    Section->Params.MirrorDataTable = MirrorDataTable;
    Section->Params.bSkipAnimNotifiers = true;
    Section->Params.bForceCustomMode = true;

    const TSharedPtr<FJsonObject> Assigned = MovieSceneJsonUtils::BuildSectionJson(Section);
    FString AnimationPath;
    TestTrue(TEXT("assigned animation path is present"),
        Assigned->TryGetStringField(TEXT("animationPath"), AnimationPath));
    TestEqual(TEXT("assigned animation path round-trips"), AnimationPath, Animation->GetPathName());

    FString MirrorPath;
    TestTrue(TEXT("assigned mirror data table path is present"),
        Assigned->TryGetStringField(TEXT("mirrorDataTablePath"), MirrorPath));
    TestEqual(TEXT("assigned mirror data table path round-trips"),
        MirrorPath, MirrorDataTable->GetPathName());

    int32 FirstLoopStartFrameOffset = 0;
    int32 StartFrameOffset = 0;
    int32 EndFrameOffset = 0;
    TestTrue(TEXT("first loop start offset is present"),
        Assigned->TryGetNumberField(TEXT("firstLoopStartFrameOffset"), FirstLoopStartFrameOffset));
    TestTrue(TEXT("start offset is present"),
        Assigned->TryGetNumberField(TEXT("startFrameOffset"), StartFrameOffset));
    TestTrue(TEXT("end offset is present"),
        Assigned->TryGetNumberField(TEXT("endFrameOffset"), EndFrameOffset));
    TestEqual(TEXT("first loop start offset round-trips"), FirstLoopStartFrameOffset, 7);
    TestEqual(TEXT("start offset round-trips"), StartFrameOffset, 11);
    TestEqual(TEXT("end offset round-trips"), EndFrameOffset, 13);

    double PlayRate = 0.0;
    TestTrue(TEXT("play rate is present"), Assigned->TryGetNumberField(TEXT("playRate"), PlayRate));
    TestEqual(TEXT("play rate round-trips"), PlayRate, 0.5);

    bool bReverse = false;
    bool bSkipAnimNotifiers = false;
    bool bForceCustomMode = false;
    TestTrue(TEXT("reverse flag is present"), Assigned->TryGetBoolField(TEXT("bReverse"), bReverse));
    TestTrue(TEXT("skip-notifiers flag is present"),
        Assigned->TryGetBoolField(TEXT("bSkipAnimNotifiers"), bSkipAnimNotifiers));
    TestTrue(TEXT("custom-mode flag is present"),
        Assigned->TryGetBoolField(TEXT("bForceCustomMode"), bForceCustomMode));
    TestTrue(TEXT("reverse flag round-trips"), bReverse);
    TestTrue(TEXT("skip-notifiers flag round-trips"), bSkipAnimNotifiers);
    TestTrue(TEXT("custom-mode flag round-trips"), bForceCustomMode);

    FString SlotName;
    TestTrue(TEXT("slot name is present"), Assigned->TryGetStringField(TEXT("slotName"), SlotName));
    TestEqual(TEXT("slot name round-trips"), SlotName, FString(TEXT("DefaultSlot")));

    return true;
}
