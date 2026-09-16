// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for PropertyUtils::ApplyJsonValueToProperty fallbacks for FStructProperty.
//
// Previously only "JSON string containing JSON" worked for struct properties; nested
// JSON objects and ExportText literals both returned "Unsupported JSON type for struct
// property". The fix routes EJson::Object → recursive sub-field apply, and EJson::String
// → JsonObjectToUStruct attempt then ImportTextToProperty fallback.
//
// To avoid adding a new UCLASS to the tests module we exercise a real UE struct
// property that is readily available: USceneComponent::RelativeLocation (FVector).
#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/BoundsCopyComponent.h"
#include "Components/TextBlock.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/Package.h"


namespace
{
    // Finds the RelativeLocation FStructProperty on USceneComponent.
    // Returns nullptr if something is very wrong (should not happen on 5.6).
    FStructProperty* FindRelativeLocationProp()
    {
        FProperty* Prop = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
        return CastField<FStructProperty>(Prop);
    }

    // Build an FJsonObject shared value for a 3-component vector.
    TSharedPtr<FJsonValue> MakeXYZObjectValue(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("X"), X);
        Obj->SetNumberField(TEXT("Y"), Y);
        Obj->SetNumberField(TEXT("Z"), Z);
        return MakeShared<FJsonValueObject>(Obj);
    }
}

// ============================================================================
// PropertyUtils.StructNestedJsonObject
// Apply a JSON object {X,Y,Z} to an FStructProperty<FVector>. The fix recursively
// applies each sub-field of the JSON object to the matching UStruct property.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsStructNestedJsonObjectTest,
    "PinWright.utils.property_utils.StructNestedJsonObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsStructNestedJsonObjectTest::RunTest(const FString& Parameters)
{
    FStructProperty* RelLocProp = FindRelativeLocationProp();
    TestNotNull(TEXT("USceneComponent has RelativeLocation FStructProperty"), RelLocProp);
    if (!RelLocProp) return false;

    USceneComponent* Comp = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient SceneComponent created"), Comp);
    if (!Comp) return false;

    // Seed with a sentinel so the assertion can catch "untouched" false-positives.
    Comp->SetRelativeLocation(FVector(-999.0f, -999.0f, -999.0f));

    TSharedPtr<FJsonValue> JsonValue = MakeXYZObjectValue(1.0, 2.0, 3.0);
    FString Err;
    const bool bApplied = ApplyJsonValueToProperty(Comp, RelLocProp, JsonValue, Err);
    TestTrue(FString::Printf(TEXT("Apply returned true (err='%s')"), *Err), bApplied);

    const FVector Loc = Comp->GetRelativeLocation();
    TestEqual(TEXT("RelativeLocation.X"), Loc.X, 1.0);
    TestEqual(TEXT("RelativeLocation.Y"), Loc.Y, 2.0);
    TestEqual(TEXT("RelativeLocation.Z"), Loc.Z, 3.0);

    return true;
}

// ============================================================================
// PropertyUtils.StructExportTextLiteral
// Apply a JSON string with an ExportText struct literal "(X=...,Y=...,Z=...)"
// to an FStructProperty<FVector>. The fix's ImportTextToProperty fallback must
// handle this since the string is not valid JSON.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsStructExportTextLiteralTest,
    "PinWright.utils.property_utils.StructExportTextLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsStructExportTextLiteralTest::RunTest(const FString& Parameters)
{
    FStructProperty* RelLocProp = FindRelativeLocationProp();
    TestNotNull(TEXT("USceneComponent has RelativeLocation FStructProperty"), RelLocProp);
    if (!RelLocProp) return false;

    USceneComponent* Comp = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Transient SceneComponent created"), Comp);
    if (!Comp) return false;

    Comp->SetRelativeLocation(FVector(-999.0f, -999.0f, -999.0f));

    // FVector ExportText form. ImportText_Direct parses these natively.
    TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueString>(TEXT("(X=4.5,Y=5.25,Z=6.75)"));
    FString Err;
    const bool bApplied = ApplyJsonValueToProperty(Comp, RelLocProp, JsonValue, Err);
    TestTrue(FString::Printf(TEXT("Apply returned true (err='%s')"), *Err), bApplied);

    const FVector Loc = Comp->GetRelativeLocation();
    TestEqual(TEXT("RelativeLocation.X"), Loc.X, 4.5);
    TestEqual(TEXT("RelativeLocation.Y"), Loc.Y, 5.25);
    TestEqual(TEXT("RelativeLocation.Z"), Loc.Z, 6.75);

    return true;
}

// ============================================================================
// PropertyUtils.ApplyJsonValueToProperty.NSLOCTEXT
// NSLOCTEXT("ns", "key", "src") macro strings assigned to an FText property
// must be parsed into a properly localized FText (namespace+key+source),
// not stored as a CultureInvariant FText whose source string is the raw macro
// invocation. Plain strings are only valid when the existing FText already has
// a namespace+key that can be preserved.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApplyJsonValueToProperty_NSLOCTEXT_Test,
    "PinWright.utils.property_utils.ApplyJsonValueToProperty.NSLOCTEXT",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FApplyJsonValueToProperty_NSLOCTEXT_Test::RunTest(const FString& Parameters)
{
    FTextProperty* TextProp = FindFProperty<FTextProperty>(UTextBlock::StaticClass(), TEXT("Text"));
    TestNotNull(TEXT("UTextBlock has Text FTextProperty"), TextProp);
    if (!TextProp) return false;

    // Sub-case 1: NSLOCTEXT macro string -> parsed localized FText.
    {
        UTextBlock* Block = NewObject<UTextBlock>(GetTransientPackage());
        TestNotNull(TEXT("Transient TextBlock created"), Block);
        if (!Block) return false;

        TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueString>(
            TEXT("NSLOCTEXT(\"NS\", \"K\", \"Hello\")"));
        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Block, TextProp, JsonValue, Err);
        TestTrue(FString::Printf(TEXT("Apply returned true (err='%s')"), *Err), bApplied);

        const FText& Stored = TextProp->GetPropertyValue_InContainer(Block);
        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Stored);
        const TOptional<FString> Key = FTextInspector::GetKey(Stored);
        const FString* SourcePtr = FTextInspector::GetSourceString(Stored);

        TestTrue(TEXT("Namespace set"), Namespace.IsSet());
        if (Namespace.IsSet())
        {
            TestEqual(TEXT("Namespace value"), Namespace.GetValue(), FString(TEXT("NS")));
        }
        TestTrue(TEXT("Key set"), Key.IsSet());
        if (Key.IsSet())
        {
            TestEqual(TEXT("Key value"), Key.GetValue(), FString(TEXT("K")));
        }
        TestNotNull(TEXT("SourceString set"), SourcePtr);
        if (SourcePtr)
        {
            TestEqual(TEXT("SourceString value"), *SourcePtr, FString(TEXT("Hello")));
        }
    }

    // Sub-case 2: plain string on empty FText -> rejected.
    {
        UTextBlock* Block = NewObject<UTextBlock>(GetTransientPackage());
        TestNotNull(TEXT("Transient TextBlock created"), Block);
        if (!Block) return false;

        TSharedPtr<FJsonValue> JsonValue = MakeShared<FJsonValueString>(TEXT("Hello"));
        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Block, TextProp, JsonValue, Err);
        TestFalse(FString::Printf(TEXT("Apply returned false (err='%s')"), *Err), bApplied);
        TestTrue(TEXT("Error mentions namespace"), Err.Contains(TEXT("namespace")));
        TestTrue(TEXT("Error mentions key"), Err.Contains(TEXT("key")));
    }

    // Sub-case 3: plain string updates source while preserving existing namespace+key.
    {
        UTextBlock* Block = NewObject<UTextBlock>(GetTransientPackage());
        TestNotNull(TEXT("Transient TextBlock created"), Block);
        if (!Block) return false;

        TSharedPtr<FJsonValue> InitialValue = MakeShared<FJsonValueString>(
            TEXT("NSLOCTEXT(\"NS\", \"K\", \"Hello\")"));
        FString Err;
        const bool bInitialApplied = ApplyJsonValueToProperty(Block, TextProp, InitialValue, Err);
        TestTrue(FString::Printf(TEXT("Initial apply returned true (err='%s')"), *Err), bInitialApplied);
        if (!bInitialApplied) return false;

        TSharedPtr<FJsonValue> UpdatedValue = MakeShared<FJsonValueString>(TEXT("Goodbye"));
        Err.Reset();
        const bool bUpdated = ApplyJsonValueToProperty(Block, TextProp, UpdatedValue, Err);
        TestTrue(FString::Printf(TEXT("Update returned true (err='%s')"), *Err), bUpdated);

        const FText& Stored = TextProp->GetPropertyValue_InContainer(Block);
        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Stored);
        const TOptional<FString> Key = FTextInspector::GetKey(Stored);
        const FString* SourcePtr = FTextInspector::GetSourceString(Stored);

        TestTrue(TEXT("Namespace still set"), Namespace.IsSet());
        if (Namespace.IsSet())
        {
            TestEqual(TEXT("Namespace preserved"), Namespace.GetValue(), FString(TEXT("NS")));
        }
        TestTrue(TEXT("Key still set"), Key.IsSet());
        if (Key.IsSet())
        {
            TestEqual(TEXT("Key preserved"), Key.GetValue(), FString(TEXT("K")));
        }
        TestNotNull(TEXT("SourceString set"), SourcePtr);
        if (SourcePtr)
        {
            TestEqual(TEXT("SourceString updated"), *SourcePtr, FString(TEXT("Goodbye")));
        }
    }

    return true;
}

// ============================================================================
// PropertyUtils.ApplyJsonValueToProperty.NullSentinelClears
// Regression for E-set-default-none-clear-conversion-failed: the canonical null
// sentinels — "" / "None" / "null" (case-insensitive) and a JSON null — must
// clear an object/class/soft-object reference to null WITHOUT attempting a path
// load. Before the fix:
//   - FObjectProperty "None"  -> [CONVERSION_FAILED] Failed to load object at path: None
//   - FObjectProperty JSON null -> "Unsupported JSON type for object property"
//   - FClassProperty  "None"  -> Failed to resolve class at path: None
//   - FSoftObjectProperty "None" -> silently stored as FSoftObjectPath("None")
// This test exercises the real ApplyJsonValueToProperty branches against engine
// classes with these property kinds (StaticMesh = FObjectProperty,
// ChildActorClass = FClassProperty, BoundsSourceActor = FSoftObjectProperty).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApplyJsonValueToProperty_NullSentinelClears_Test,
    "PinWright.utils.property_utils.ApplyJsonValueToProperty.NullSentinelClears",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FApplyJsonValueToProperty_NullSentinelClears_Test::RunTest(const FString& Parameters)
{
    // --- FObjectProperty (UStaticMeshComponent::StaticMesh) ---
    FObjectProperty* MeshProp = FindFProperty<FObjectProperty>(
        UStaticMeshComponent::StaticClass(), TEXT("StaticMesh"));
    TestNotNull(TEXT("UStaticMeshComponent has StaticMesh FObjectProperty"), MeshProp);
    if (MeshProp)
    {
        // Every string/JSON-null sentinel must clear; seed non-null first.
        const TArray<TSharedPtr<FJsonValue>> Sentinels = {
            MakeShared<FJsonValueString>(TEXT("None")),
            MakeShared<FJsonValueString>(TEXT("none")),   // case-insensitive
            MakeShared<FJsonValueString>(TEXT("null")),
            MakeShared<FJsonValueString>(TEXT("")),
            MakeShared<FJsonValueNull>()
        };
        for (const TSharedPtr<FJsonValue>& Sentinel : Sentinels)
        {
            UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage());
            if (!Comp) { AddError(TEXT("Failed to create StaticMeshComponent")); return false; }
            // Seed the FObjectProperty non-null via reflection so a no-op would be
            // detectable. SetObjectPropertyValue_InContainer does not type-check the
            // assignee, so any live UObject serves as the "currently set" sentinel.
            UObject* SeedMesh = NewObject<UStaticMesh>(GetTransientPackage());
            MeshProp->SetObjectPropertyValue_InContainer(Comp, SeedMesh);
            TestNotNull(TEXT("StaticMesh seeded non-null"),
                MeshProp->GetObjectPropertyValue_InContainer(Comp));

            FString Err;
            const bool bApplied = ApplyJsonValueToProperty(Comp, MeshProp, Sentinel, Err);
            TestTrue(FString::Printf(TEXT("FObjectProperty sentinel apply ok (err='%s')"), *Err), bApplied);
            TestNull(TEXT("FObjectProperty cleared to null"),
                MeshProp->GetObjectPropertyValue_InContainer(Comp));
        }
    }

    // --- FClassProperty (UChildActorComponent::ChildActorClass) ---
    FClassProperty* ClassProp = FindFProperty<FClassProperty>(
        UChildActorComponent::StaticClass(), TEXT("ChildActorClass"));
    TestNotNull(TEXT("UChildActorComponent has ChildActorClass FClassProperty"), ClassProp);
    if (ClassProp)
    {
        UChildActorComponent* Comp = NewObject<UChildActorComponent>(GetTransientPackage());
        if (!Comp) { AddError(TEXT("Failed to create ChildActorComponent")); return false; }
        ClassProp->SetObjectPropertyValue_InContainer(Comp, AActor::StaticClass());
        TestNotNull(TEXT("ChildActorClass seeded non-null"),
            ClassProp->GetObjectPropertyValue_InContainer(Comp));

        TSharedPtr<FJsonValue> NoneVal = MakeShared<FJsonValueString>(TEXT("None"));
        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Comp, ClassProp, NoneVal, Err);
        TestTrue(FString::Printf(TEXT("FClassProperty 'None' apply ok (err='%s')"), *Err), bApplied);
        TestNull(TEXT("FClassProperty cleared to null"),
            ClassProp->GetObjectPropertyValue_InContainer(Comp));
    }

    // --- FSoftObjectProperty (UBoundsCopyComponent::BoundsSourceActor) ---
    FSoftObjectProperty* SoftProp = FindFProperty<FSoftObjectProperty>(
        UBoundsCopyComponent::StaticClass(), TEXT("BoundsSourceActor"));
    TestNotNull(TEXT("UBoundsCopyComponent has BoundsSourceActor FSoftObjectProperty"), SoftProp);
    if (SoftProp)
    {
        UBoundsCopyComponent* Comp = NewObject<UBoundsCopyComponent>(GetTransientPackage());
        if (!Comp) { AddError(TEXT("Failed to create BoundsCopyComponent")); return false; }
        // Seed a non-null soft path so a no-op would leave it set.
        FSoftObjectPtr* SeedPtr = static_cast<FSoftObjectPtr*>(
            SoftProp->ContainerPtrToValuePtr<void>(Comp));
        if (SeedPtr) { *SeedPtr = FSoftObjectPath(TEXT("/Game/Some/Path.Path")); }

        TSharedPtr<FJsonValue> NoneVal = MakeShared<FJsonValueString>(TEXT("None"));
        FString Err;
        const bool bApplied = ApplyJsonValueToProperty(Comp, SoftProp, NoneVal, Err);
        TestTrue(FString::Printf(TEXT("FSoftObjectProperty 'None' apply ok (err='%s')"), *Err), bApplied);

        const FSoftObjectPtr* ReadPtr = static_cast<const FSoftObjectPtr*>(
            SoftProp->ContainerPtrToValuePtr<void>(Comp));
        const bool bNull = ReadPtr && ReadPtr->IsNull();
        // Before the fix this stored the literal FSoftObjectPath("None"), not null.
        TestTrue(TEXT("FSoftObjectProperty 'None' cleared to null (not literal \"None\" path)"), bNull);
    }

    return true;
}
