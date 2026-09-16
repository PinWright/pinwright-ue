// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the audio submix routing surface introduced by F-audio-submix-asset-authoring.
// Covers: create_sound_submix with parentSubmix wiring, set_submix_parent clear-then-restore,
// set_class_properties parentSubmix, and get_audio_info readback parity for both new fields.
//
// Counterfactual (per ticket): if the ParentSubmix assignment inside CreateSoundSubmixAsset is
// reverted to a bare NewObject<USoundSubmix>, the Child->ParentSubmix == Parent assertion fails
// because the field is left at its default nullptr.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"
#include "Handlers/HandlerContext.h"

#include "Sound/SoundClass.h"

#if __has_include("Sound/SoundSubmix.h")
#include "Sound/SoundSubmix.h"
#define MCP_HAS_SUBMIX 1
#else
#define MCP_HAS_SUBMIX 0
#endif

#if MCP_HAS_SUBMIX

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioSubmixRoutingTest,
    "PinWright.Audio.SubmixRouting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAudioSubmixRoutingTest::RunTest(const FString& Parameters)
{
    // Unique-per-run names to avoid collisions across repeated runs in the same editor session.
    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TestPath = TEXT("/Game/PinWrightTests/SubmixRouting");
    const FString ParentName = FString::Printf(TEXT("Submix_Parent_%s"), *Stamp);
    const FString ChildName = FString::Printf(TEXT("Submix_Child_%s"), *Stamp);
    const FString ClassName = FString::Printf(TEXT("SoundClass_Routed_%s"), *Stamp);

    const FString ParentPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ParentName, *ParentName);
    const FString ChildPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ChildName, *ChildName);
    const FString ClassPath = FString::Printf(TEXT("%s/%s.%s"), *TestPath, *ClassName, *ClassName);

    // --- 1. Create parent submix (no parent wired) ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ParentName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_submix (parent) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_submix"), Payload, Cap));
        TestTrue(TEXT("create_sound_submix (parent) success"), Cap.bSuccess);
    }

    USoundSubmix* Parent = FindObject<USoundSubmix>(nullptr, *ParentPath);
    TestNotNull(TEXT("Parent submix loaded after create"), Parent);
    if (!Parent)
    {
        CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ParentName));
        return false;
    }

    // --- 2. Create child submix with parentSubmix wired ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ChildName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetStringField(TEXT("parentSubmix"), ParentPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_submix (child) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_submix"), Payload, Cap));
        TestTrue(TEXT("create_sound_submix (child) success"), Cap.bSuccess);
    }

    USoundSubmix* Child = FindObject<USoundSubmix>(nullptr, *ChildPath);
    TestNotNull(TEXT("Child submix loaded after create"), Child);
    if (Child)
    {
        // Counterfactual assertion: regresses if helper drops the ParentSubmix assignment.
        TestEqual(TEXT("Child->ParentSubmix == Parent after create_sound_submix"), Child->ParentSubmix.Get(), static_cast<USoundSubmixBase*>(Parent));
        // Parent-side link (B-sound-class-parent-no-child-link submix symmetry): create_sound_submix
        // must route through SetParentSubmix so the parent's ChildSubmixes gains the child. Regresses
        // to false if reverted to a raw `NewSubmix->ParentSubmix = Parent;` write.
        TestTrue(TEXT("Parent->ChildSubmixes contains Child after create_sound_submix"),
            Parent->ChildSubmixes.Contains(static_cast<USoundSubmixBase*>(Child)));
    }

    // --- 3. set_submix_parent with empty parentPath → clears to null ---
    if (Child)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ChildPath);
        Payload->SetStringField(TEXT("parentPath"), TEXT(""));
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_submix_parent (clear) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_submix_parent"), Payload, Cap));
        TestTrue(TEXT("set_submix_parent (clear) success"), Cap.bSuccess);
        TestNull(TEXT("Child->ParentSubmix is null after clear"), Child->ParentSubmix.Get());
        // Old parent must drop the child from ChildSubmixes on clear (regresses if set_submix_parent
        // reverts to a raw `Submix->ParentSubmix = nullptr;` write that skips the parent side).
        TestFalse(TEXT("Parent->ChildSubmixes no longer contains Child after clear"),
            Parent->ChildSubmixes.Contains(static_cast<USoundSubmixBase*>(Child)));
    }

    // --- 4. set_submix_parent re-wire to parent ---
    if (Child)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ChildPath);
        Payload->SetStringField(TEXT("parentPath"), ParentPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_submix_parent (restore) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_submix_parent"), Payload, Cap));
        TestTrue(TEXT("set_submix_parent (restore) success"), Cap.bSuccess);
        TestEqual(TEXT("Child->ParentSubmix restored"), Child->ParentSubmix.Get(), static_cast<USoundSubmixBase*>(Parent));
        // New parent must regain the child in ChildSubmixes on re-wire (parent-side link).
        TestTrue(TEXT("Parent->ChildSubmixes contains Child after restore"),
            Parent->ChildSubmixes.Contains(static_cast<USoundSubmixBase*>(Child)));
    }

    // --- 5. SoundClass routed into a submix via set_class_properties parentSubmix ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), ClassName);
        Payload->SetStringField(TEXT("path"), TestPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_sound_class dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_class"), Payload, Cap));
        TestTrue(TEXT("create_sound_class success"), Cap.bSuccess);
    }

    USoundClass* SoundClass = FindObject<USoundClass>(nullptr, *ClassPath);
    TestNotNull(TEXT("SoundClass loaded after create"), SoundClass);

    if (SoundClass && Parent)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ClassPath);
        Payload->SetStringField(TEXT("parentSubmix"), ParentPath);
        Payload->SetBoolField(TEXT("save"), false);
        FTestResponseCapture Cap;
        TestTrue(TEXT("set_class_properties (parentSubmix) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.set_class_properties"), Payload, Cap));
        TestTrue(TEXT("set_class_properties (parentSubmix) success"), Cap.bSuccess);
        TestEqual(TEXT("SoundClass.Properties.DefaultSubmix == Parent"),
            SoundClass->Properties.DefaultSubmix.Get(), Parent);
    }

    // --- 6. get_audio_info readback parity ---
    if (Child)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ChildPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("get_audio_info (submix) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.get_audio_info"), Payload, Cap));
        TestTrue(TEXT("get_audio_info (submix) success"), Cap.bSuccess);
        if (Cap.Result.IsValid())
        {
            FString TypeField;
            TestTrue(TEXT("get_audio_info type field present"), Cap.Result->TryGetStringField(TEXT("type"), TypeField));
            TestEqual(TEXT("get_audio_info type == SoundSubmix"), TypeField, FString(TEXT("SoundSubmix")));
            FString ParentSubmixField;
            TestTrue(TEXT("get_audio_info emits parentSubmix"),
                Cap.Result->TryGetStringField(TEXT("parentSubmix"), ParentSubmixField));
            // The exact path, not merely "something": the branch emits
            // Submix->ParentSubmix->GetPathName() (AudioAuthoringHandler.cpp:3342), and a
            // non-empty check would be satisfied by the child's OWN path just as well as by
            // the parent's - which is the copy-paste this readback can ship.
            TestEqual(TEXT("parentSubmix names the parent, not just any asset"),
                ParentSubmixField, ParentPath);
        }
    }

    if (SoundClass)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ClassPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("get_audio_info (class) dispatched"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.get_audio_info"), Payload, Cap));
        TestTrue(TEXT("get_audio_info (class) success"), Cap.bSuccess);
        if (Cap.Result.IsValid())
        {
            FString OutputSubmixField;
            TestTrue(TEXT("get_audio_info emits outputSubmix for routed SoundClass"),
                Cap.Result->TryGetStringField(TEXT("outputSubmix"), OutputSubmixField));
            // Same reason as parentSubmix above: the routing target is the point, and only an
            // equality against the submix set in §5 can tell a correct emit from any emit.
            TestEqual(TEXT("outputSubmix names the submix the class was routed into"),
                OutputSubmixField, ParentPath);
        }
    }

    // Best-effort cleanup. Assets were created with save=false so packages stay dirty/in-memory.
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ChildName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ParentName));
    CleanupTestAsset(FString::Printf(TEXT("%s/%s"), *TestPath, *ClassName));

    return true;
}

#endif // MCP_HAS_SUBMIX
