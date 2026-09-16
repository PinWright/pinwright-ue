// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPropertySetReportsMarkDirtyNotSaved.cpp
// Regression test for B-property-set-saved-true-not-persisted.
//
// property.set is a mark-dirty-only mutator: it writes the value in-memory and marks the
// target package dirty, but it NEVER saves to disk (persist with editor.save_all / asset.save).
// The pre-fix handler nonetheless hardcoded `saved:true` on every successful write at all five
// emit sites (the four AActor setter branches — ActorLocation/ActorRotation/ActorScale/bHidden —
// and the generic reflected path), a false persistence signal: an agent that trusts saved:true
// skips the explicit save and loses the edit on editor restart.
//
// The fix drops the `saved` field and instead reports the honest signals used by the plugin's
// other mutators: `applied:true` (the in-memory write succeeded) and `markedDirty` (matching
// AssetMetadataHandler's mark-dirty-only convention). Since the follow-up fix for
// B-property-set-markdirty-false-still-dirties, `markedDirty` reports the OBSERVED package dirty
// state after the write, NOT the requested markDirty param — the handler stamps
// `TargetPackage->IsDirty()`, so a caller can never be told markedDirty:false while the package is
// genuinely dirty. This test drives the real registered property.set handler end-to-end over an
// in-code fixture and asserts, on both the generic reflected path (a transient Blueprint CDO int
// member) and an actor path (a spawned AStaticMeshActor's ActorLocation), that: the response
// reports applied:true; markedDirty matches the package state each fixture can actually reach; and
// the response never claims saved:true.
//
// This file's generic-path fixture is built in GetTransientPackage(), which can never be dirtied,
// so it cannot tell the two markDirty polarities apart. Real-package dirty semantics for BOTH
// polarities (and the same guarantee for property.reset) are covered by the sibling
// Tests/Utility/TestPropertyMarkDirtyRespected.cpp, which uses a saved /Game asset fixture — that
// is where the discriminating coverage lives.
//
// Counterfactual: revert any of the five emit sites to `SetBoolField(TEXT("saved"), true)` and
// the "must not claim saved:true" assertion for that path fails.

#include "Misc/AutomationTest.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"
#include "Tests/TestUtils.h"

namespace
{
    FName MakeUniquePropertySetFixtureName(const TCHAR* Prefix)
    {
        return FName(*FString::Printf(
            TEXT("%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }

    // Asserts the honest mark-dirty-only mutator response contract that replaced the
    // hardcoded saved:true: applied is present and true, markedDirty is present and equals the
    // package dirty state the caller expects the handler to OBSERVE for this fixture (which is
    // not necessarily the markDirty param it passed), and the response never claims saved:true.
    void AssertHonestMutatorResponse(FAutomationTestBase& Test, const FString& Label,
        const TSharedPtr<FJsonObject>& Result, bool bExpectedMarkedDirty)
    {
        bool bApplied = false;
        Test.TestTrue(*FString::Printf(TEXT("%s: applied present"), *Label),
            Result->TryGetBoolField(TEXT("applied"), bApplied));
        Test.TestTrue(*FString::Printf(TEXT("%s: applied is true"), *Label), bApplied);

        bool bMarkedDirty = !bExpectedMarkedDirty;
        Test.TestTrue(*FString::Printf(TEXT("%s: markedDirty present"), *Label),
            Result->TryGetBoolField(TEXT("markedDirty"), bMarkedDirty));
        Test.TestEqual(
            *FString::Printf(TEXT("%s: markedDirty reports observed package dirty state"), *Label),
            bMarkedDirty, bExpectedMarkedDirty);

        // The counterfactual assertion: property.set does not persist, so it must NEVER report
        // saved:true. Passes whether `saved` is absent (post-fix) or present-and-false; fails
        // only on the pre-fix hardcoded saved:true.
        bool bSaved = false;
        const bool bHasSaved = Result->TryGetBoolField(TEXT("saved"), bSaved);
        Test.TestFalse(
            *FString::Printf(TEXT("%s: must not claim saved:true (mark-dirty-only, not persisted)"), *Label),
            bHasSaved && bSaved);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetReportsMarkDirtyNotSavedTest,
    "PinWright.property.set.MarkDirtyMutatorReportsAppliedNotSaved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetReportsMarkDirtyNotSavedTest::RunTest(const FString& Parameters)
{
    // ---- Fixture: transient BP (subclass of AActor) with an int member variable. Driving
    // property.set on its CDO with a non-actor-special property name exercises the GENERIC
    // reflected emit path. ----
    const FName BPName = MakeUniquePropertySetFixtureName(TEXT("BP_PropertySetHonest"));
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        BPName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("fixture blueprint created"), BP);
    if (!BP)
    {
        return false;
    }

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const bool bVarAdded =
        FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("SetScalar"), IntType);
    TestTrue(TEXT("SetScalar member variable added to fixture BP"), bVarAdded);
    if (!bVarAdded)
    {
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(BP);
    TestNotNull(TEXT("fixture generated class after compile"), BP->GeneratedClass.Get());
    if (!BP->GeneratedClass)
    {
        return false;
    }

    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
    TestNotNull(TEXT("fixture CDO available"), CDO);
    if (!CDO)
    {
        return false;
    }
    const FString CdoPath = CDO->GetPathName();

    // ---- Generic reflected path, markDirty defaulted (true). ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), CdoPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("SetScalar"));
        Payload->SetNumberField(TEXT("value"), 7);

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.set handler found (generic, markDirty default)"),
            InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
        TestTrue(TEXT("property.set succeeded (generic, markDirty default)"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        // Expected markedDirty is FALSE even though markDirty defaulted to true, and that is
        // correct — do not "fix" it back. The fixture CDO is outered to GetTransientPackage(),
        // which is structurally undirtyable: UObjectBaseUtility::MarkPackageDirty() walks the
        // outer chain and returns early on the first RF_Transient outer (the transient package is
        // created with RF_Transient), and even if it got that far UPackage::SetDirtyFlag() skips
        // its entire body when `GetOutermost() != GetTransientPackage()` fails. So the package is
        // never dirty, and the handler — which stamps markedDirty from TargetPackage->IsDirty(),
        // not from the request param — honestly reports false. Pre-fix this read true only
        // because the field echoed the parameter and never looked at reality.
        AssertHonestMutatorResponse(*this, TEXT("generic markDirty=true"), Capture.Result,
            /*bExpectedMarkedDirty=*/false);

        double SetValue = 0.0;
        TestTrue(TEXT("generic: response echoes value"),
            Capture.Result->TryGetNumberField(TEXT("value"), SetValue));
        TestEqual(TEXT("generic: response value is the 7 we set"),
            static_cast<int32>(SetValue), 7);

        // The reflected property actually changed on the CDO (the write really applied).
        FIntProperty* IntProp =
            CastField<FIntProperty>(CDO->GetClass()->FindPropertyByName(TEXT("SetScalar")));
        TestNotNull(TEXT("SetScalar property resolvable on CDO"), IntProp);
        if (IntProp)
        {
            TestEqual(TEXT("generic: CDO SetScalar is now 7"),
                IntProp->GetPropertyValue_InContainer(CDO), 7);
        }
    }

    // ---- Generic reflected path, markDirty=false. The expectation stays false, but it now
    // passes for the transient-package reason above (the package cannot be dirty) rather than
    // because the response echoes the param — on this fixture both polarities observe false, so
    // this case no longer discriminates between them. The discriminating coverage is in
    // Tests/Utility/TestPropertyMarkDirtyRespected.cpp, on a real saved /Game package. ----
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), CdoPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("SetScalar"));
        Payload->SetNumberField(TEXT("value"), 11);
        Payload->SetBoolField(TEXT("markDirty"), false);

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.set handler found (generic, markDirty=false)"),
            InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
        TestTrue(TEXT("property.set succeeded (generic, markDirty=false)"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            AssertHonestMutatorResponse(*this, TEXT("generic markDirty=false"), Capture.Result,
                /*bExpectedMarkedDirty=*/false);
        }
    }

    // ---- Actor path (ActorLocation): exercises one of the four AActor setter emit branches.
    // Requires the editor world; a missing world is a hard failure, not a skip. ----
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    TestNotNull(TEXT("editor world required for the actor-path fixture"), World);
    if (!World)
    {
        return false;
    }

    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
    TestNotNull(TEXT("spawned AStaticMeshActor fixture"), Actor);
    if (!Actor)
    {
        return false;
    }
    // Movable so SetActorLocation moves it cleanly (no static-mobility warning) in the editor world.
    if (USceneComponent* Root = Actor->GetRootComponent())
    {
        Root->SetMobility(EComponentMobility::Movable);
    }

    {
        TSharedPtr<FJsonObject> ValObj = MakeShared<FJsonObject>();
        ValObj->SetNumberField(TEXT("x"), 120.0);
        ValObj->SetNumberField(TEXT("y"), 240.0);
        ValObj->SetNumberField(TEXT("z"), 360.0);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Actor->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), TEXT("ActorLocation"));
        Payload->SetObjectField(TEXT("value"), ValObj);

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.set handler found (actor ActorLocation)"),
            InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
        TestTrue(TEXT("property.set succeeded (actor ActorLocation)"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            AssertHonestMutatorResponse(*this, TEXT("actor ActorLocation"), Capture.Result,
                /*bExpectedMarkedDirty=*/true);

            const FVector Loc = Actor->GetActorLocation();
            TestTrue(TEXT("actor moved to the requested location"),
                Loc.Equals(FVector(120.0, 240.0, 360.0), 0.5));
        }
    }

    Actor->Destroy();
    return true;
}
