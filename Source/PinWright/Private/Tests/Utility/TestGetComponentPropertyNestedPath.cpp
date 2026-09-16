// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the dotted-nested-path read added to
// actor.get_component_property (Handlers/Actor/ComponentHandler.cpp).
//
// The handler matches a component by its friendly runtime name and, when the
// requested propertyName contains a '.', resolves into the struct/object member
// via Utils/PropertyInspection.cpp::ResolveNestedPropertyPath and exports only
// the leaf via Utils/PropertyExport.cpp::ExportPropertyToJsonValue — so reading
// one scalar of a large struct (e.g. FBodyInstance.CollisionEnabled) returns
// that single value instead of the whole decomposed struct (which would exceed
// the HTTP spill threshold).
//
// This test exercises the SAME production functions the handler now calls, on a
// real USceneComponent. It asserts the dotted leaf 'PrimaryComponentTick.bCanEverTick'
// resolves to a single JSON boolean — NOT the whole decomposed
// FActorComponentTickFunction object. If the dotted-path routing in the handler
// is reverted (so the verb falls back to a single FindPropertyByName + whole-struct
// export), the leaf read this test proves would no longer be available.
#include "Misc/AutomationTest.h"
#include "Utils/PropertyInspection.h"
#include "Utils/PropertyExport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetComponentPropertyNestedPathTest,
    "PinWright.actor.get_component_property.NestedStructLeaf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetComponentPropertyNestedPathTest::RunTest(const FString& Parameters)
{
    USceneComponent* Component = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient SceneComponent created"), Component);
    if (!Component) return false;

    // Set a known, non-default leaf value so the test asserts a concrete scalar,
    // not just structural shape. bCanEverTick lives inside the PrimaryComponentTick
    // struct member (FActorComponentTickFunction), the same shape of nested-field
    // read the BodyInstance.CollisionEnabled case needs.
    Component->PrimaryComponentTick.bCanEverTick = true;

    // --- Dotted path: resolve into the struct member and export only the leaf ---
    // This mirrors what the handler does when propertyName contains '.'.
    {
        void* Container = nullptr;
        FString ResolveError;
        FProperty* LeafProp = ResolveNestedPropertyPath(
            Component, TEXT("PrimaryComponentTick.bCanEverTick"), Container, ResolveError);

        TestNotNull(TEXT("Dotted path resolves the leaf FProperty"), LeafProp);
        TestTrue(TEXT("Dotted path yields a non-null container"), Container != nullptr);
        TestEqual(TEXT("No resolve error for valid dotted path"), ResolveError, FString());
        if (!LeafProp || !Container) return false;

        // The leaf must be the bool itself, not the enclosing struct property.
        TestNotNull(TEXT("Leaf is an FBoolProperty"), CastField<FBoolProperty>(LeafProp));

        TSharedPtr<FJsonValue> LeafValue = ExportPropertyToJsonValue(Container, LeafProp);
        TestTrue(TEXT("Leaf export returned a value"), LeafValue.IsValid());
        if (!LeafValue.IsValid()) return false;

        // The whole point of the fix: the response is ONE scalar boolean,
        // not a decomposed struct object.
        TestEqual(TEXT("Leaf exports as a JSON boolean (scalar), not a struct object"),
            (int32)LeafValue->Type, (int32)EJson::Boolean);

        bool LeafBool = false;
        TestTrue(TEXT("Leaf bool readable"), LeafValue->TryGetBool(LeafBool));
        TestTrue(TEXT("Leaf bCanEverTick == true round-trips"), LeafBool);
    }

    // --- Counterfactual: the non-dotted whole-property read returns the whole
    // decomposed struct (a JSON object with many members). This is exactly the
    // large response the dotted-path read avoids; asserting it here documents the
    // contrast and fails to be a struct only if PropertyExport behavior regresses. ---
    {
        FProperty* TickProp = UActorComponent::StaticClass()->FindPropertyByName(TEXT("PrimaryComponentTick"));
        TestNotNull(TEXT("PrimaryComponentTick FProperty resolved"), TickProp);
        if (!TickProp) return false;

        TSharedPtr<FJsonValue> WholeStruct = ExportPropertyToJsonValue(Component, TickProp);
        TestTrue(TEXT("Whole-struct export returned a value"), WholeStruct.IsValid());
        if (!WholeStruct.IsValid()) return false;

        TestEqual(TEXT("Whole-property read is a JSON object (decomposed struct), not a scalar"),
            (int32)WholeStruct->Type, (int32)EJson::Object);

        const TSharedPtr<FJsonObject>* StructObj = nullptr;
        TestTrue(TEXT("Whole struct TryGetObject"), WholeStruct->TryGetObject(StructObj));
        if (StructObj && *StructObj)
        {
            // The whole struct carries bCanEverTick among many other members —
            // confirming the leaf the dotted read isolated really is buried inside.
            TestTrue(TEXT("Whole struct contains the bCanEverTick member"),
                (*StructObj)->HasField(TEXT("bCanEverTick")));
            TestTrue(TEXT("Whole struct carries more than the single requested field"),
                (*StructObj)->Values.Num() > 1);
        }
    }

    // --- A bad nested segment must report an error, not silently fall back ---
    {
        void* Container = nullptr;
        FString ResolveError;
        FProperty* Missing = ResolveNestedPropertyPath(
            Component, TEXT("PrimaryComponentTick.NoSuchField"), Container, ResolveError);
        TestNull(TEXT("Bad nested segment resolves to null"), Missing);
        TestFalse(TEXT("Bad nested segment populates an error"), ResolveError.IsEmpty());
    }

    return true;
}
