// Copyright (c) 2026 Alexander Penkin. MIT License.

// Handler-level coverage for the animation.authoring curve verbs: get_curve_keys, set_curve_keys,
// remove_curve_key, remove_curve and rename_curve. Every assertion reads the data model directly
// as well as the response, so a response that claims a write the model does not hold fails.

#include "Misc/AutomationTest.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/TestUtils.h"

namespace PwCurveEditingTest
{
    // Keys in the set_curve_keys wire shape. A negative Frame sends time instead.
    struct FKeySpec
    {
        int32 Frame;
        double Time;
        float Value;
        const TCHAR* InterpMode;
        const TCHAR* TangentMode;
        float ArriveTangent;
        float LeaveTangent;
    };

    TSharedPtr<FJsonValue> MakeKey(const FKeySpec& Spec)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        if (Spec.Frame >= 0)
        {
            Key->SetNumberField(TEXT("frame"), Spec.Frame);
        }
        else
        {
            Key->SetNumberField(TEXT("time"), Spec.Time);
        }
        Key->SetNumberField(TEXT("value"), Spec.Value);
        Key->SetStringField(TEXT("interpMode"), Spec.InterpMode);
        // nullptr leaves tangentMode out, so the handler's default applies.
        if (Spec.TangentMode)
        {
            Key->SetStringField(TEXT("tangentMode"), Spec.TangentMode);
        }
        Key->SetNumberField(TEXT("arriveTangent"), Spec.ArriveTangent);
        Key->SetNumberField(TEXT("leaveTangent"), Spec.LeaveTangent);
        return MakeShared<FJsonValueObject>(Key);
    }

    TSharedPtr<FJsonValue> MakeFrameSelector(int32 Frame)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("frame"), Frame);
        return MakeShared<FJsonValueObject>(Key);
    }

    TArray<TSharedPtr<FJsonValue>> MakeNames(std::initializer_list<const TCHAR*> Names)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const TCHAR* Name : Names)
        {
            Out.Add(MakeShared<FJsonValueString>(Name));
        }
        return Out;
    }

    bool Invoke(FAutomationTestBase& Test, const TCHAR* Method, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        return Test.TestTrue(*FString::Printf(TEXT("%s is registered"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
    }

    bool SetKeys(FAutomationTestBase& Test, const FString& AssetPath, const TCHAR* CurveName,
        const TCHAR* Mode, const TArray<FKeySpec>& Keys, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("curveName"), CurveName);
        Payload->SetStringField(TEXT("mode"), Mode);
        Payload->SetBoolField(TEXT("save"), false);
        TArray<TSharedPtr<FJsonValue>> KeyValues;
        for (const FKeySpec& Spec : Keys)
        {
            KeyValues.Add(MakeKey(Spec));
        }
        Payload->SetArrayField(TEXT("keys"), KeyValues);
        return Invoke(Test, TEXT("animation.authoring.set_curve_keys"), Payload, Capture);
    }

    const FRichCurve* StoredCurve(const UAnimSequence* Sequence, const TCHAR* CurveName)
    {
        const FFloatCurve* Curve = Sequence->GetDataModel()->FindFloatCurve(
            FAnimationCurveIdentifier(FName(CurveName), ERawCurveTrackTypes::RCT_Float));
        return Curve ? &Curve->FloatCurve : nullptr;
    }

    const TArray<FKeySpec> ThreeKeys = {
        {0, 0.0, 1.0f, TEXT("linear"), TEXT("auto"), 0.0f, 0.0f},
        {1, 0.0, 2.5f, TEXT("constant"), TEXT("auto"), 0.0f, 0.0f},
        {-1, 2.0 / 30.0, -1.0f, TEXT("cubic"), TEXT("user"), 0.5f, -0.25f},
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCurveEditingBatchWriteReadBackTest,
    "PinWright.animation.authoring.curve_editing.BatchWriteReadsBackIdentical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimCurveEditingBatchWriteReadBackTest::RunTest(const FString& Parameters)
{
    using namespace PwCurveEditingTest;
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    if (!TestTrue(TEXT("registered animation fixture created"), Fixture.Create(TEXT("AS_CurveBatch"))))
    {
        return false;
    }
    const FString& Path = Fixture.SequenceObjectPath;

    FTestResponseCapture SetCapture;
    SetKeys(*this, Path, TEXT("Blink"), TEXT("replace"), ThreeKeys, SetCapture);
    TestTrue(*FString::Printf(TEXT("set_curve_keys succeeds: %s %s"), *SetCapture.ErrorCode, *SetCapture.Message),
        SetCapture.bSuccess);
    if (SetCapture.Result.IsValid())
    {
        TestTrue(TEXT("a missing curve is created and reported"), SetCapture.Result->GetBoolField(TEXT("created")));
        TestEqual(TEXT("response keyCount"), static_cast<int32>(SetCapture.Result->GetNumberField(TEXT("keyCount"))), 3);
    }
    const FRichCurve* Stored = StoredCurve(Fixture.Sequence, TEXT("Blink"));
    if (!TestNotNull(TEXT("the model holds the curve"), Stored))
    {
        return false;
    }
    if (!TestEqual(TEXT("the model holds three keys"), Stored->GetNumKeys(), 3))
    {
        return false;
    }

    TSharedPtr<FJsonObject> GetPayload = MakeShared<FJsonObject>();
    GetPayload->SetStringField(TEXT("assetPath"), Path);
    GetPayload->SetArrayField(TEXT("curveNames"), MakeNames({TEXT("Blink")}));
    FTestResponseCapture GetCapture;
    Invoke(*this, TEXT("animation.authoring.get_curve_keys"), GetPayload, GetCapture);
    TestTrue(TEXT("get_curve_keys succeeds"), GetCapture.bSuccess);
    const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
    if (!GetCapture.Result.IsValid() || !GetCapture.Result->TryGetArrayField(TEXT("curves"), Curves)
        || !TestEqual(TEXT("one curve read"), Curves->Num(), 1))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& Keys = (*Curves)[0]->AsObject()->GetArrayField(TEXT("keys"));
    if (!TestEqual(TEXT("three keys read back"), Keys.Num(), 3))
    {
        return false;
    }
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const FKeySpec& Sent = ThreeKeys[Index];
        const TSharedPtr<FJsonObject> Read = Keys[Index]->AsObject();
        const FString Label = FString::Printf(TEXT("key %d"), Index);
        TestEqual(*(Label + TEXT(" frame")), Read->GetNumberField(TEXT("frame")), static_cast<double>(Index));
        TestEqual(*(Label + TEXT(" value")), static_cast<float>(Read->GetNumberField(TEXT("value"))), Sent.Value);
        TestEqual(*(Label + TEXT(" interpMode")), Read->GetStringField(TEXT("interpMode")), FString(Sent.InterpMode));
        TestEqual(*(Label + TEXT(" tangentMode")), Read->GetStringField(TEXT("tangentMode")), FString(Sent.TangentMode));
        TestEqual(*(Label + TEXT(" time matches the model")),
            static_cast<float>(Read->GetNumberField(TEXT("time"))), Stored->GetConstRefOfKeys()[Index].Time);
    }
    const TSharedPtr<FJsonObject> UserKey = Keys[2]->AsObject();
    TestEqual(TEXT("user arriveTangent survives"), static_cast<float>(UserKey->GetNumberField(TEXT("arriveTangent"))), 0.5f);
    TestEqual(TEXT("user leaveTangent survives"), static_cast<float>(UserKey->GetNumberField(TEXT("leaveTangent"))), -0.25f);

    // merge overwrites the key at frame 1 and keeps the other two.
    FTestResponseCapture MergeCapture;
    SetKeys(*this, Path, TEXT("Blink"), TEXT("merge"),
        TArray<FKeySpec>{FKeySpec{1, 0.0, 9.0f, TEXT("linear"), TEXT("auto"), 0.0f, 0.0f}}, MergeCapture);
    TestTrue(TEXT("merge succeeds"), MergeCapture.bSuccess);
    Stored = StoredCurve(Fixture.Sequence, TEXT("Blink"));
    if (TestNotNull(TEXT("curve still present after merge"), Stored)
        && TestEqual(TEXT("merge keeps the key count"), Stored->GetNumKeys(), 3))
    {
        TestEqual(TEXT("merge overwrote frame 1"), Stored->GetConstRefOfKeys()[1].Value, 9.0f);
        TestEqual(TEXT("merge kept frame 0"), Stored->GetConstRefOfKeys()[0].Value, 1.0f);
    }
    if (MergeCapture.Result.IsValid())
    {
        TestFalse(TEXT("an existing curve is not reported created"), MergeCapture.Result->GetBoolField(TEXT("created")));
    }

    // replace leaves exactly the sent keys.
    FTestResponseCapture ReplaceCapture;
    SetKeys(*this, Path, TEXT("Blink"), TEXT("replace"),
        TArray<FKeySpec>{FKeySpec{2, 0.0, 4.0f, TEXT("linear"), TEXT("auto"), 0.0f, 0.0f}}, ReplaceCapture);
    TestTrue(TEXT("replace succeeds"), ReplaceCapture.bSuccess);
    Stored = StoredCurve(Fixture.Sequence, TEXT("Blink"));
    if (TestNotNull(TEXT("curve still present after replace"), Stored))
    {
        TestEqual(TEXT("replace leaves one key"), Stored->GetNumKeys(), 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCurveEditingRemoveKeyAndCurveTest,
    "PinWright.animation.authoring.curve_editing.RemoveKeyAndCurve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimCurveEditingRemoveKeyAndCurveTest::RunTest(const FString& Parameters)
{
    using namespace PwCurveEditingTest;
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    if (!TestTrue(TEXT("registered animation fixture created"), Fixture.Create(TEXT("AS_CurveRemove"))))
    {
        return false;
    }
    const FString& Path = Fixture.SequenceObjectPath;
    FTestResponseCapture SetCapture;
    SetKeys(*this, Path, TEXT("Jaw"), TEXT("replace"), ThreeKeys, SetCapture);
    if (!TestTrue(TEXT("seed keys written"), SetCapture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RemoveKey = MakeShared<FJsonObject>();
    RemoveKey->SetStringField(TEXT("assetPath"), Path);
    RemoveKey->SetStringField(TEXT("curveName"), TEXT("Jaw"));
    RemoveKey->SetBoolField(TEXT("save"), false);
    RemoveKey->SetArrayField(TEXT("keys"), {MakeFrameSelector(1)});
    FTestResponseCapture RemoveKeyCapture;
    Invoke(*this, TEXT("animation.authoring.remove_curve_key"), RemoveKey, RemoveKeyCapture);
    TestTrue(*FString::Printf(TEXT("remove_curve_key succeeds: %s %s"), *RemoveKeyCapture.ErrorCode,
        *RemoveKeyCapture.Message), RemoveKeyCapture.bSuccess);
    if (RemoveKeyCapture.Result.IsValid())
    {
        TestEqual(TEXT("removedCount is measured"),
            static_cast<int32>(RemoveKeyCapture.Result->GetNumberField(TEXT("removedCount"))), 1);
    }
    const FRichCurve* Stored = StoredCurve(Fixture.Sequence, TEXT("Jaw"));
    if (!TestNotNull(TEXT("curve survives a key removal"), Stored))
    {
        return false;
    }
    if (!TestEqual(TEXT("two keys remain"), Stored->GetNumKeys(), 2))
    {
        return false;
    }
    TestEqual(TEXT("frame 0 key remains"), Stored->GetConstRefOfKeys()[0].Value, 1.0f);
    TestEqual(TEXT("frame 2 key remains"), Stored->GetConstRefOfKeys()[1].Value, -1.0f);

    // A batch naming one missing key removes nothing.
    RemoveKey->SetArrayField(TEXT("keys"), {MakeFrameSelector(0), MakeFrameSelector(1)});
    FTestResponseCapture MissingCapture;
    Invoke(*this, TEXT("animation.authoring.remove_curve_key"), RemoveKey, MissingCapture);
    TestFalse(TEXT("removing an absent key is refused"), MissingCapture.bSuccess);
    TestEqual(TEXT("absent key uses KEY_NOT_FOUND"), MissingCapture.ErrorCode, FString(ErrorCodes::ERR_KEY_NOT_FOUND));
    TestEqual(TEXT("the refused batch removed nothing"), Stored->GetNumKeys(), 2);

    TSharedPtr<FJsonObject> RemoveCurve = MakeShared<FJsonObject>();
    RemoveCurve->SetStringField(TEXT("assetPath"), Path);
    RemoveCurve->SetArrayField(TEXT("curveNames"), MakeNames({TEXT("Jaw")}));
    RemoveCurve->SetBoolField(TEXT("save"), false);
    FTestResponseCapture RemoveCurveCapture;
    Invoke(*this, TEXT("animation.authoring.remove_curve"), RemoveCurve, RemoveCurveCapture);
    TestTrue(*FString::Printf(TEXT("remove_curve succeeds: %s %s"), *RemoveCurveCapture.ErrorCode,
        *RemoveCurveCapture.Message), RemoveCurveCapture.bSuccess);
    TestNull(TEXT("the model no longer holds the curve"), StoredCurve(Fixture.Sequence, TEXT("Jaw")));
    if (RemoveCurveCapture.Result.IsValid())
    {
        TestEqual(TEXT("removed lists the curve"),
            RemoveCurveCapture.Result->GetArrayField(TEXT("removed")).Num(), 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCurveEditingRenameKeepsKeysTest,
    "PinWright.animation.authoring.curve_editing.RenameKeepsKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimCurveEditingRenameKeepsKeysTest::RunTest(const FString& Parameters)
{
    using namespace PwCurveEditingTest;
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    if (!TestTrue(TEXT("registered animation fixture created"), Fixture.Create(TEXT("AS_CurveRename"))))
    {
        return false;
    }
    const FString& Path = Fixture.SequenceObjectPath;
    FTestResponseCapture SeedA;
    FTestResponseCapture SeedB;
    SetKeys(*this, Path, TEXT("CurveA"), TEXT("replace"), ThreeKeys, SeedA);
    SetKeys(*this, Path, TEXT("CurveB"), TEXT("replace"),
        TArray<FKeySpec>{FKeySpec{0, 0.0, 7.0f, TEXT("linear"), TEXT("auto"), 0.0f, 0.0f}}, SeedB);
    if (!TestTrue(TEXT("seed curves written"), SeedA.bSuccess && SeedB.bSuccess
        && StoredCurve(Fixture.Sequence, TEXT("CurveA")) != nullptr))
    {
        return false;
    }
    const TArray<FRichCurveKey> KeysBefore = StoredCurve(Fixture.Sequence, TEXT("CurveA"))->GetConstRefOfKeys();

    TSharedPtr<FJsonObject> Rename = MakeShared<FJsonObject>();
    Rename->SetStringField(TEXT("assetPath"), Path);
    Rename->SetStringField(TEXT("curveName"), TEXT("CurveA"));
    Rename->SetStringField(TEXT("newName"), TEXT("CurveC"));
    Rename->SetBoolField(TEXT("save"), false);
    FTestResponseCapture RenameCapture;
    Invoke(*this, TEXT("animation.authoring.rename_curve"), Rename, RenameCapture);
    TestTrue(*FString::Printf(TEXT("rename_curve succeeds: %s %s"), *RenameCapture.ErrorCode,
        *RenameCapture.Message), RenameCapture.bSuccess);
    TestNull(TEXT("the old name is gone"), StoredCurve(Fixture.Sequence, TEXT("CurveA")));
    const FRichCurve* Renamed = StoredCurve(Fixture.Sequence, TEXT("CurveC"));
    if (TestNotNull(TEXT("the new name holds a curve"), Renamed))
    {
        const TArray<FRichCurveKey>& KeysAfter = Renamed->GetConstRefOfKeys();
        if (TestEqual(TEXT("rename keeps the key count"), KeysAfter.Num(), KeysBefore.Num()))
        {
            for (int32 Index = 0; Index < KeysAfter.Num(); ++Index)
            {
                TestEqual(*FString::Printf(TEXT("key %d time"), Index), KeysAfter[Index].Time, KeysBefore[Index].Time);
                TestEqual(*FString::Printf(TEXT("key %d value"), Index), KeysAfter[Index].Value, KeysBefore[Index].Value);
            }
        }
    }

    // Renaming onto a taken name would leave two curves sharing it; it is refused.
    Rename->SetStringField(TEXT("curveName"), TEXT("CurveC"));
    Rename->SetStringField(TEXT("newName"), TEXT("CurveB"));
    FTestResponseCapture CollisionCapture;
    Invoke(*this, TEXT("animation.authoring.rename_curve"), Rename, CollisionCapture);
    TestFalse(TEXT("rename onto an existing curve is refused"), CollisionCapture.bSuccess);
    TestEqual(TEXT("collision uses ALREADY_EXISTS"), CollisionCapture.ErrorCode, FString(ErrorCodes::ERR_ALREADY_EXISTS));
    TestNotNull(TEXT("the source curve is untouched"), StoredCurve(Fixture.Sequence, TEXT("CurveC")));
    const FRichCurve* Target = StoredCurve(Fixture.Sequence, TEXT("CurveB"));
    if (TestNotNull(TEXT("the target curve is untouched"), Target))
    {
        TestEqual(TEXT("the target keeps its single key"), Target->GetNumKeys(), 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCurveEditingUnknownCurveTest,
    "PinWright.animation.authoring.curve_editing.UnknownCurveIsTypedError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimCurveEditingUnknownCurveTest::RunTest(const FString& Parameters)
{
    using namespace PwCurveEditingTest;
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    if (!TestTrue(TEXT("registered animation fixture created"), Fixture.Create(TEXT("AS_CurveUnknown"))))
    {
        return false;
    }
    const FString& Path = Fixture.SequenceObjectPath;
    const FString CurveNotFound(ErrorCodes::ERR_CURVE_NOT_FOUND);

    TSharedPtr<FJsonObject> Get = MakeShared<FJsonObject>();
    Get->SetStringField(TEXT("assetPath"), Path);
    Get->SetArrayField(TEXT("curveNames"), MakeNames({TEXT("Nope")}));
    FTestResponseCapture GetCapture;
    Invoke(*this, TEXT("animation.authoring.get_curve_keys"), Get, GetCapture);
    TestEqual(TEXT("get_curve_keys on an unknown curve"), GetCapture.ErrorCode, CurveNotFound);
    TestTrue(TEXT("the error lists the existing curves"),
        GetCapture.Result.IsValid() && GetCapture.Result->HasField(TEXT("curves")));

    TSharedPtr<FJsonObject> RemoveKey = MakeShared<FJsonObject>();
    RemoveKey->SetStringField(TEXT("assetPath"), Path);
    RemoveKey->SetStringField(TEXT("curveName"), TEXT("Nope"));
    RemoveKey->SetArrayField(TEXT("keys"), {MakeFrameSelector(0)});
    FTestResponseCapture RemoveKeyCapture;
    Invoke(*this, TEXT("animation.authoring.remove_curve_key"), RemoveKey, RemoveKeyCapture);
    TestEqual(TEXT("remove_curve_key on an unknown curve"), RemoveKeyCapture.ErrorCode, CurveNotFound);

    TSharedPtr<FJsonObject> RemoveCurve = MakeShared<FJsonObject>();
    RemoveCurve->SetStringField(TEXT("assetPath"), Path);
    RemoveCurve->SetArrayField(TEXT("curveNames"), MakeNames({TEXT("Nope")}));
    FTestResponseCapture RemoveCurveCapture;
    Invoke(*this, TEXT("animation.authoring.remove_curve"), RemoveCurve, RemoveCurveCapture);
    TestEqual(TEXT("remove_curve on an unknown curve"), RemoveCurveCapture.ErrorCode, CurveNotFound);

    TSharedPtr<FJsonObject> Rename = MakeShared<FJsonObject>();
    Rename->SetStringField(TEXT("assetPath"), Path);
    Rename->SetStringField(TEXT("curveName"), TEXT("Nope"));
    Rename->SetStringField(TEXT("newName"), TEXT("Other"));
    FTestResponseCapture RenameCapture;
    Invoke(*this, TEXT("animation.authoring.rename_curve"), Rename, RenameCapture);
    TestEqual(TEXT("rename_curve on an unknown curve"), RenameCapture.ErrorCode, CurveNotFound);

    // set_curve_keys refuses to create when told not to, and refuses bad input before writing.
    TSharedPtr<FJsonObject> Set = MakeShared<FJsonObject>();
    Set->SetStringField(TEXT("assetPath"), Path);
    Set->SetStringField(TEXT("curveName"), TEXT("Nope"));
    Set->SetStringField(TEXT("mode"), TEXT("merge"));
    Set->SetBoolField(TEXT("createIfMissing"), false);
    Set->SetBoolField(TEXT("save"), false);
    Set->SetArrayField(TEXT("keys"), {MakeKey(ThreeKeys[0])});
    FTestResponseCapture NoCreateCapture;
    Invoke(*this, TEXT("animation.authoring.set_curve_keys"), Set, NoCreateCapture);
    TestEqual(TEXT("createIfMissing false on an unknown curve"), NoCreateCapture.ErrorCode, CurveNotFound);

    Set->SetBoolField(TEXT("createIfMissing"), true);
    Set->SetArrayField(TEXT("keys"), {MakeKey(ThreeKeys[0]), MakeKey(ThreeKeys[0])});
    FTestResponseCapture DuplicateCapture;
    Invoke(*this, TEXT("animation.authoring.set_curve_keys"), Set, DuplicateCapture);
    TestEqual(TEXT("two keys at one time are refused"), DuplicateCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMS));

    Set->SetStringField(TEXT("mode"), TEXT("append"));
    Set->SetArrayField(TEXT("keys"), {MakeKey(ThreeKeys[0])});
    FTestResponseCapture ModeCapture;
    Invoke(*this, TEXT("animation.authoring.set_curve_keys"), Set, ModeCapture);
    TestEqual(TEXT("an unknown mode is refused"), ModeCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_MODE));

    TestNull(TEXT("no refused call created the curve"), StoredCurve(Fixture.Sequence, TEXT("Nope")));
    return true;
}

// An omitted tangentMode must round-trip as the documented default "auto". The one engine exception
// (the AnimationData sequencer model stores a cubic key right after a linear key as "break") must be
// named in engineAdjustedTangentModes, so the response never disagrees silently with the read-back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCurveEditingOmittedTangentModeTest,
    "PinWright.animation.authoring.curve_editing.OmittedTangentModeRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimCurveEditingOmittedTangentModeTest::RunTest(const FString& Parameters)
{
    using namespace PwCurveEditingTest;
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    if (!TestTrue(TEXT("registered animation fixture created"), Fixture.Create(TEXT("AS_CurveTangentDefault"))))
    {
        return false;
    }
    const FString& Path = Fixture.SequenceObjectPath;

    auto ReadTangentModes = [this, &Path](TArray<FString>& OutModes)
    {
        TSharedPtr<FJsonObject> Get = MakeShared<FJsonObject>();
        Get->SetStringField(TEXT("assetPath"), Path);
        Get->SetArrayField(TEXT("curveNames"), MakeNames({TEXT("Ease")}));
        FTestResponseCapture Capture;
        Invoke(*this, TEXT("animation.authoring.get_curve_keys"), Get, Capture);
        const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
        if (!TestTrue(TEXT("get_curve_keys succeeds"), Capture.bSuccess && Capture.Result.IsValid()
                && Capture.Result->TryGetArrayField(TEXT("curves"), Curves) && Curves->Num() == 1))
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Key : (*Curves)[0]->AsObject()->GetArrayField(TEXT("keys")))
        {
            OutModes.Add(Key->AsObject()->GetStringField(TEXT("tangentMode")));
        }
    };

    // Cubic keys with no linear predecessor: every data model keeps the default.
    FTestResponseCapture CubicCapture;
    SetKeys(*this, Path, TEXT("Ease"), TEXT("replace"), TArray<FKeySpec>{
        FKeySpec{0, 0.0, 0.0f, TEXT("cubic"), nullptr, 0.0f, 0.0f},
        FKeySpec{1, 0.0, 1.0f, TEXT("cubic"), nullptr, 0.0f, 0.0f},
        FKeySpec{2, 0.0, 0.0f, TEXT("linear"), nullptr, 0.0f, 0.0f}}, CubicCapture);
    TestTrue(TEXT("cubic keys written"), CubicCapture.bSuccess);
    if (CubicCapture.Result.IsValid())
    {
        TestEqual(TEXT("no key reported adjusted"),
            CubicCapture.Result->GetArrayField(TEXT("engineAdjustedTangentModes")).Num(), 0);
    }
    TArray<FString> Modes;
    ReadTangentModes(Modes);
    if (TestEqual(TEXT("three keys read back"), Modes.Num(), 3))
    {
        TestEqual(TEXT("omitted tangentMode on key 0 reads back auto"), Modes[0], FString(TEXT("auto")));
        TestEqual(TEXT("omitted tangentMode on key 1 reads back auto"), Modes[1], FString(TEXT("auto")));
        TestEqual(TEXT("omitted tangentMode on key 2 reads back auto"), Modes[2], FString(TEXT("auto")));
    }

    // A cubic key after a linear key: stored auto, or break and reported as adjusted.
    FTestResponseCapture MixedCapture;
    SetKeys(*this, Path, TEXT("Ease"), TEXT("replace"), TArray<FKeySpec>{
        FKeySpec{0, 0.0, 0.0f, TEXT("linear"), nullptr, 0.0f, 0.0f},
        FKeySpec{1, 0.0, 1.0f, TEXT("cubic"), nullptr, 0.0f, 0.0f}}, MixedCapture);
    TestTrue(TEXT("mixed keys written"), MixedCapture.bSuccess);
    Modes.Reset();
    ReadTangentModes(Modes);
    if (!MixedCapture.Result.IsValid() || !TestEqual(TEXT("two keys read back"), Modes.Num(), 2))
    {
        return false;
    }
    TestEqual(TEXT("the linear key keeps auto"), Modes[0], FString(TEXT("auto")));
    const TArray<TSharedPtr<FJsonValue>>& Adjusted =
        MixedCapture.Result->GetArrayField(TEXT("engineAdjustedTangentModes"));
    if (Modes[1] == TEXT("auto"))
    {
        TestEqual(TEXT("an unchanged key is not reported adjusted"), Adjusted.Num(), 0);
    }
    else
    {
        TestEqual(TEXT("the only engine rewrite is to break"), Modes[1], FString(TEXT("break")));
        if (TestEqual(TEXT("the rewritten key is reported"), Adjusted.Num(), 1))
        {
            const TSharedPtr<FJsonObject> Entry = Adjusted[0]->AsObject();
            TestEqual(TEXT("reported sent mode"), Entry->GetStringField(TEXT("sent")), FString(TEXT("auto")));
            TestEqual(TEXT("reported stored mode"), Entry->GetStringField(TEXT("stored")), FString(TEXT("break")));
        }
    }
    return true;
}
