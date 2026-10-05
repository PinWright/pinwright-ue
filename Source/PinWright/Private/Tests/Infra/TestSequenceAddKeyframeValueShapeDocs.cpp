// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-sequence-add-keyframe-per-axis-value-shape-undocumented: callers of
// sequence.add_keyframe had to read SequenceHandler.cpp to learn that property="Transform" takes a
// NESTED {location,rotation,scale} value while "Location"/"Rotation"/"Scale" take a FLAT {x,y,z}
// (rotation also {roll,pitch,yaw}) or [x,y,z] value. The sequencer.add_keyframe H3 also still
// claimed both keyframe writers shared one method name with registration-order dispatch, which
// has not been true since the frame-numbered writer kept the `sequence.add_keyframe` name.
//
// Both pages render through WikiHandler::RenderPage, the path the gateway serves. The
// sequence.add_keyframe page has no overlay (its prefix `sequence` has no wiki-src file), so its
// value shapes live in the `property` / `value` param descriptions; the sequencer.add_keyframe
// page carries them in its `## Notes` H3 overlay.

#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAddKeyframeValueShapeParamDocTest,
    "PinWright.infra.wiki_handler.MethodPage.SequenceAddKeyframeValueShapeParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequenceAddKeyframeValueShapeParamDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("sequence.add_keyframe"), Text))
    {
        return false;
    }

    // Before the fix the value param read only "Value for the keyframe".
    TestTrue(TEXT("value param states the nested Transform shape"),
        Text.Contains(TEXT("Transform: NESTED {location:{x,y,z}, rotation:{roll,pitch,yaw}, scale:{x,y,z}}")));
    TestTrue(TEXT("value param states the flat Location/Scale shape"),
        Text.Contains(TEXT("Scale: FLAT {x,y,z} or [x,y,z]")));
    TestTrue(TEXT("value param states the flat Rotation shape and its x/y/z mapping"),
        Text.Contains(TEXT("Rotation: FLAT {roll,pitch,yaw}, {x,y,z} (x=roll, y=pitch, z=yaw)")));
    TestTrue(TEXT("value param names the refusal for a mismatched shape"),
        Text.Contains(TEXT("writes no key and fails with UNSUPPORTED_PROPERTY")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddKeyframeWriterSplitDocTest,
    "PinWright.infra.wiki_handler.MethodPage.SequencerAddKeyframeValueShapeNotes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddKeyframeWriterSplitDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("sequencer.add_keyframe"), Text))
    {
        return false;
    }

    FString Notes;
    if (!TestTrue(TEXT("sequencer.add_keyframe page carries the overlay `## Notes` block"),
            WikiDocTestHelpers::ExtractSection(Text, TEXT("Notes"), Notes)))
    {
        return false;
    }

    // The stale claim: one method name, dispatch decided by registration order.
    TestFalse(TEXT("Notes no longer claim registration-order dispatch"),
        Notes.Contains(TEXT("registration order")));
    TestTrue(TEXT("Notes name the frame-numbered writer by its real method name"),
        Notes.Contains(TEXT("`sequence.add_keyframe` (no `r`)")));
    // The per-property value-shape table.
    TestTrue(TEXT("Notes give the nested Transform value shape"),
        Notes.Contains(TEXT("| `Transform` | nested, any subset of `{location:{x,y,z}, rotation:{roll,pitch,yaw}, scale:{x,y,z}}`")));
    TestTrue(TEXT("Notes give the flat Location/Scale value shape"),
        Notes.Contains(TEXT("| `Location`, `Scale` | flat `{x,y,z}` or `[x,y,z]`")));
    TestTrue(TEXT("Notes give the flat Rotation value shape"),
        Notes.Contains(TEXT("| `Rotation` | flat `{roll,pitch,yaw}`")));
    TestTrue(TEXT("Notes say a mismatched shape is refused with UNSUPPORTED_PROPERTY"),
        Notes.Contains(TEXT("writes no key and fails with `UNSUPPORTED_PROPERTY`")));
    return true;
}
