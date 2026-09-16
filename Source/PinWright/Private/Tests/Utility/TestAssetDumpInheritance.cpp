// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Compat/JsonKeyCompat.h"

#include "Components/SceneComponent.h"
#include "Components/ActorComponent.h"
#include "Components/InputComponent.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "PinWright_SCSHandlers.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/Guid.h"

namespace
{
    // ------------------------------------------------------------------------
    // Inherited-SCS fixture, synthesized in-test.
    //
    // The two tests below used to load /App/App/LevelBlueprints/DynamicObstacles/
    // B_SM_Floor3x3, a fixture from a different host project. There is no /App mount
    // here, so StaticLoadObject returned null on every run and both tests took an
    // AddInfo-and-pass skip having asserted nothing — leaving the ancestor-SCS emission
    // in PinWright_SCSHandlers.cpp (source=inherited-scs / inheritedFrom) and the
    // scs.txt sidecar emit in AssetDumpHandler.cpp with zero live coverage.
    //
    // The shape reproduced here is the minimum that exercises that loop: a parent BP
    // owning one named SCS node, and a child BP whose own SCS node is attached to the
    // parent's node BY NAME (USCS_Node::SetParent). Without the ancestor loop the child
    // dump emits a `parent: InheritedRoot` reference to a component that appears nowhere
    // in the components array — the dangling reference the loop exists to prevent.
    //
    // Modelled on FBlueprintScsSetPropertyInheritedScsChildOverrideTest in
    // Tests/Blueprint/TestBlueprintHandlers.cpp, which builds the same parent/child pair.
    // Packages are created in memory under /Engine/Transient/ and never saved, matching
    // the sibling dump fixtures in this directory (TestAssetDumpMaterialInstance.cpp,
    // TestAssetDumpTextSidecars.cpp); DumpSingleAsset resolves them through its
    // FindPackage in-memory check ahead of DoesPackageExist.
    // ------------------------------------------------------------------------

    const TCHAR* const ScsInheritParentNodeName = TEXT("InheritedRoot");
    const TCHAR* const ScsInheritChildNodeName  = TEXT("ChildProbe");

    struct FScsInheritanceFixture
    {
        FString     ParentPath;
        FString     ChildPath;
        UBlueprint* ParentBP   = nullptr;
        UBlueprint* ChildBP    = nullptr;
        USCS_Node*  ParentNode = nullptr;
        USCS_Node*  ChildNode  = nullptr;

        FString ChildObjectPath() const
        {
            return FString::Printf(TEXT("%s.%s"),
                *ChildPath, *FPackageName::GetLongPackageAssetName(ChildPath));
        }
    };

    UBlueprint* CreateScsInheritanceBlueprint(const FString& PackagePath, UClass* ParentClass)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        return FKismetEditorUtilities::CreateBlueprint(
            ParentClass,
            Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    bool BuildScsInheritanceFixture(FScsInheritanceFixture& Out, FString& OutError)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Out.ParentPath = FString::Printf(TEXT("/Engine/Transient/PwScsInheritParent_%s"), *Suffix);
        Out.ChildPath  = FString::Printf(TEXT("/Engine/Transient/PwScsInheritChild_%s"), *Suffix);

        Out.ParentBP = CreateScsInheritanceBlueprint(Out.ParentPath, AActor::StaticClass());
        if (!Out.ParentBP || !Out.ParentBP->SimpleConstructionScript)
        {
            OutError = FString::Printf(TEXT("parent Blueprint not created at %s"), *Out.ParentPath);
            return false;
        }

        // One named scene node on the parent. It becomes the BP's scene root, so
        // USimpleConstructionScript::ValidateSceneRootNodes drops the editor's placeholder
        // DefaultSceneRoot from the parent's node list and leaves exactly one inheritable
        // node — the child then has nothing to inherit except this one.
        Out.ParentNode = Out.ParentBP->SimpleConstructionScript->CreateNode(
            USceneComponent::StaticClass(), FName(ScsInheritParentNodeName));
        if (!Out.ParentNode)
        {
            OutError = TEXT("parent SCS node was not created");
            return false;
        }
        Out.ParentBP->SimpleConstructionScript->AddNode(Out.ParentNode);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Out.ParentBP);
        FKismetEditorUtilities::CompileBlueprint(Out.ParentBP);

        // Re-resolve after compilation: the compiler can replace the node objects.
        Out.ParentNode =
            Out.ParentBP->SimpleConstructionScript->FindSCSNode(FName(ScsInheritParentNodeName));
        if (!Out.ParentNode || !Out.ParentBP->GeneratedClass)
        {
            OutError = TEXT("parent SCS node did not survive compilation");
            return false;
        }

        Out.ChildBP = CreateScsInheritanceBlueprint(Out.ChildPath, Out.ParentBP->GeneratedClass);
        if (!Out.ChildBP || !Out.ChildBP->SimpleConstructionScript)
        {
            OutError = FString::Printf(TEXT("child Blueprint not created at %s"), *Out.ChildPath);
            return false;
        }

        Out.ChildNode = Out.ChildBP->SimpleConstructionScript->CreateNode(
            USceneComponent::StaticClass(), FName(ScsInheritChildNodeName));
        if (!Out.ChildNode)
        {
            OutError = TEXT("child SCS node was not created");
            return false;
        }
        Out.ChildBP->SimpleConstructionScript->AddNode(Out.ChildNode);
        // The load-bearing line: attach the child's node to the PARENT BP's node. SetParent
        // records ParentComponentOrVariableName + the owning BPGC name, which is what makes
        // the child dump reference a component it does not itself own.
        Out.ChildNode->SetParent(Out.ParentNode);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Out.ChildBP);
        FKismetEditorUtilities::CompileBlueprint(Out.ChildBP);

        Out.ChildNode =
            Out.ChildBP->SimpleConstructionScript->FindSCSNode(FName(ScsInheritChildNodeName));
        if (!Out.ChildNode)
        {
            OutError = TEXT("child SCS node did not survive compilation");
            return false;
        }
        if (Out.ChildNode->ParentComponentOrVariableName != FName(ScsInheritParentNodeName))
        {
            OutError = FString::Printf(
                TEXT("child node's inherited parent link was lost (got '%s')"),
                *Out.ChildNode->ParentComponentOrVariableName.ToString());
            return false;
        }

        return true;
    }

    // Teardown mirrors the WidgetBlueprint fixture at the bottom of this file: close any
    // auto-opened editor and drop the dirty flag so the transient packages never reach the
    // editor's save-pending set. No delete/GC pass — the packages are in-memory only.
    void CleanupScsInheritanceFixture(FScsInheritanceFixture& Fixture)
    {
        UBlueprint* Blueprints[] = { Fixture.ChildBP, Fixture.ParentBP };
        for (UBlueprint* Blueprint : Blueprints)
        {
            if (!Blueprint)
            {
                continue;
            }
            if (GEditor)
            {
                if (UAssetEditorSubsystem* AssetEditorSubsystem =
                        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
                {
                    AssetEditorSubsystem->CloseAllEditorsForAsset(Blueprint);
                }
            }
            if (UPackage* Outer = Blueprint->GetOutermost())
            {
                Outer->SetDirtyFlag(false);
            }
        }
        Fixture.ParentNode = nullptr;
        Fixture.ChildNode  = nullptr;
        Fixture.ParentBP   = nullptr;
        Fixture.ChildBP    = nullptr;
    }
}

// ============================================================================
// AssetDumpInheritance.OverrideDetection
// Verifies is_overridden_locally toggles correctly.
// Uses two USceneComponent instances as child/parent CDO stand-ins.
// When both have the same RelativeLocation -> not overridden.
// When they differ -> overridden.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceOverrideDetectionTest,
    "PinWright.Utils.AssetDumpInheritance.OverrideDetection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceOverrideDetectionTest::RunTest(const FString& Parameters)
{
    FProperty* RelLocProp = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("USceneComponent has RelativeLocation property"), RelLocProp);
    if (!RelLocProp) return false;

    USceneComponent* Child  = NewObject<USceneComponent>(GetTransientPackage());
    USceneComponent* Parent = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("Child component created"), Child);
    TestNotNull(TEXT("Parent component created"), Parent);
    if (!Child || !Parent) return false;

    // Same value -> not overridden.
    Child->SetRelativeLocation(FVector(1.0f, 2.0f, 3.0f));
    Parent->SetRelativeLocation(FVector(1.0f, 2.0f, 3.0f));

    TSharedPtr<FJsonObject> SameResult = ExportPropertyToJsonValueWithInheritance(
        Child, Parent, RelLocProp, USceneComponent::StaticClass());
    TestNotNull(TEXT("SameResult is valid"), SameResult.Get());
    if (!SameResult) return false;

    // Schema v2: not-overridden is conveyed by field absence.
    TestFalse(TEXT("is_overridden_locally field is absent for same-value case (schema v2)"),
        SameResult->Values.Contains(TEXT("is_overridden_locally")));

    // Different value -> overridden.
    Child->SetRelativeLocation(FVector(100.0f, 200.0f, 300.0f));

    TSharedPtr<FJsonObject> DiffResult = ExportPropertyToJsonValueWithInheritance(
        Child, Parent, RelLocProp, USceneComponent::StaticClass());
    TestNotNull(TEXT("DiffResult is valid"), DiffResult.Get());
    if (!DiffResult) return false;

    bool bDiffOverridden = false;
    TestTrue(TEXT("is_overridden_locally field exists for diff-value case"),
        DiffResult->TryGetBoolField(TEXT("is_overridden_locally"), bDiffOverridden));
    TestTrue(TEXT("Different value -> is_overridden_locally = true"), bDiffOverridden);

    return true;
}

// ============================================================================
// AssetDumpInheritance.InheritedFromField
// A property declared on UActorComponent (bAutoActivate) when walked with
// AssetClass=USceneComponent must have inherited_from="ActorComponent".
// A property declared on USceneComponent itself (RelativeLocation) must have
// inherited_from=null when AssetClass=USceneComponent.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceInheritedFromFieldTest,
    "PinWright.Utils.AssetDumpInheritance.InheritedFromField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceInheritedFromFieldTest::RunTest(const FString& Parameters)
{
    USceneComponent* CDO = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("CDO created"), CDO);
    if (!CDO) return false;

    // bAutoActivate is declared on UActorComponent, so inherited_from should be non-null.
    FProperty* AutoActivateProp = UActorComponent::StaticClass()->FindPropertyByName(TEXT("bAutoActivate"));
    TestNotNull(TEXT("UActorComponent has bAutoActivate"), AutoActivateProp);
    if (!AutoActivateProp) return false;

    TSharedPtr<FJsonObject> InheritedResult = ExportPropertyToJsonValueWithInheritance(
        CDO, nullptr, AutoActivateProp, USceneComponent::StaticClass());
    TestNotNull(TEXT("InheritedResult valid"), InheritedResult.Get());
    if (!InheritedResult) return false;

    const TSharedPtr<FJsonValue>* InheritedField = InheritedResult->Values.Find(TEXT("inherited_from"));
    TestNotNull(TEXT("inherited_from field exists"), InheritedField ? InheritedField->Get() : nullptr);
    if (!InheritedField) return false;

    // Must be a non-null string containing the declaring class name.
    TestTrue(TEXT("inherited_from is not null for bAutoActivate"),
        (*InheritedField)->Type == EJson::String);
    if ((*InheritedField)->Type == EJson::String)
    {
        const FString Val = (*InheritedField)->AsString();
        TestFalse(TEXT("inherited_from is non-empty"), Val.IsEmpty());
        TestTrue(TEXT("inherited_from contains 'ActorComponent'"),
            Val.Contains(TEXT("ActorComponent")));
    }

    // RelativeLocation is declared on USceneComponent -> inherited_from must be null.
    FProperty* RelLocProp = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("USceneComponent has RelativeLocation"), RelLocProp);
    if (!RelLocProp) return false;

    TSharedPtr<FJsonObject> OwnResult = ExportPropertyToJsonValueWithInheritance(
        CDO, nullptr, RelLocProp, USceneComponent::StaticClass());
    TestNotNull(TEXT("OwnResult valid"), OwnResult.Get());
    if (!OwnResult) return false;

    // Schema v2: own-class property -> inherited_from is absent (not null).
    TestFalse(TEXT("inherited_from field is absent for own-class property (schema v2)"),
        OwnResult->Values.Contains(TEXT("inherited_from")));

    return true;
}

// ============================================================================
// AssetDumpInheritance.FlagsWhitelist
// A property with CPF_Edit set (RelativeLocation) must appear in flags.
// A property without CPF_Transient should not have "Transient" in flags.
// CPF_HasGetValueTypeHash (not in whitelist) must never appear.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceFlagsWhitelistTest,
    "PinWright.Utils.AssetDumpInheritance.FlagsWhitelist",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceFlagsWhitelistTest::RunTest(const FString& Parameters)
{
    FProperty* RelLocProp = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("USceneComponent has RelativeLocation"), RelLocProp);
    if (!RelLocProp) return false;

    USceneComponent* CDO = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("CDO created"), CDO);
    if (!CDO) return false;

    TSharedPtr<FJsonObject> Result = ExportPropertyToJsonValueWithInheritance(
        CDO, nullptr, RelLocProp, USceneComponent::StaticClass());
    TestNotNull(TEXT("Result valid"), Result.Get());
    if (!Result) return false;

    // RelativeLocation has CPF_Edit, so the array must be present and contain "Edit"
    // (schema v2: flags field is omitted only when there are no whitelisted flags).
    const TArray<TSharedPtr<FJsonValue>>* FlagsArray = nullptr;
    TestTrue(TEXT("flags field is present and is an array (RelativeLocation has Edit flag)"),
        Result->TryGetArrayField(TEXT("flags"), FlagsArray));
    if (!FlagsArray) return false;

    bool bFoundEdit = false;
    for (const TSharedPtr<FJsonValue>& FlagVal : *FlagsArray)
    {
        if (FlagVal->AsString() == TEXT("Edit"))
        {
            bFoundEdit = true;
            break;
        }
    }
    TestTrue(TEXT("'Edit' flag appears in output when CPF_Edit is set"), bFoundEdit);

    // No flag name should be "HasGetValueTypeHash" — it's not in the whitelist.
    for (const TSharedPtr<FJsonValue>& FlagVal : *FlagsArray)
    {
        TestFalse(TEXT("Non-whitelisted flag 'HasGetValueTypeHash' must not appear"),
            FlagVal->AsString() == TEXT("HasGetValueTypeHash"));
    }

    // RelativeLocation is not transient — "Transient" must not appear.
    const bool bIsTransient = (RelLocProp->PropertyFlags & CPF_Transient) != 0;
    if (!bIsTransient)
    {
        for (const TSharedPtr<FJsonValue>& FlagVal : *FlagsArray)
        {
            TestFalse(TEXT("'Transient' must not appear when CPF_Transient is not set"),
                FlagVal->AsString() == TEXT("Transient"));
        }
    }

    return true;
}

// ============================================================================
// AssetDumpInheritance.BuildClassPropertyJsonCoversAllProperties
// BuildClassPropertyJson with ParentCDO=nullptr must contain a key for every
// emittable property from TFieldIterator (own + inherited), with no extras and
// no gaps. Emittability is decided by the shared ShouldEmitClassDumpProperty
// predicate, which the walk below reuses instead of keeping a second copy of the
// filter: Transient / DuplicateTransient / Deprecated / SkipSerialization fields
// are dropped as intentional noise reduction (see commit 88a5aac "feat: improve"
// — board ticket B-asset-dump-properties-spurious-override-on-bp-internal-bools
// and siblings; the Deprecated / SkipSerialization half arrived with d9b1b0ef
// "Stabilize asset dumps and expose progress"). Their values are runtime caches
// or legacy upgrade slots that UE itself declines to serialize, so dumping them
// produces run-to-run churn.
// (When a parent CDO IS supplied, non-overridden properties are also filtered
// out; that case is covered by BuildClassPropertyJsonFiltersNonOverridden
// below.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceBuildClassPropertyJsonCoversAllPropertiesTest,
    "PinWright.Utils.AssetDumpInheritance.BuildClassPropertyJsonCoversAllProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceBuildClassPropertyJsonCoversAllPropertiesTest::RunTest(const FString& Parameters)
{
    USceneComponent* CDO = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("CDO created"), CDO);
    if (!CDO) return false;

    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(CDO, nullptr);
    TestNotNull(TEXT("BuildClassPropertyJson returned non-null"), Result.Get());
    if (!Result) return false;

    TestTrue(TEXT("Result has at least one property"), Result->Values.Num() > 0);

    // Every emittable property from TFieldIterator must appear as a key. The walk and
    // the emitter share ShouldEmitClassDumpProperty so the two filters cannot drift:
    // this assertion previously kept its own CPF_Transient | CPF_DuplicateTransient copy
    // and over-counted once the emitter also began dropping CPF_Deprecated /
    // CPF_SkipSerialization fields.
    TArray<FString> ExpectedKeys;
    for (TFieldIterator<FProperty> It(USceneComponent::StaticClass(),
                                      EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        if (!ShouldEmitClassDumpProperty(*It))
        {
            continue;
        }
        ExpectedKeys.AddUnique((*It)->GetName());
    }

    for (const FString& PropName : ExpectedKeys)
    {
        TestTrue(
            FString::Printf(TEXT("Key '%s' present in BuildClassPropertyJson output"), *PropName),
            Result->Values.Contains(EARGCompat::JsonFieldKey(PropName)));
    }

    // Key count must match the emittable iterator count — no extras, no missing.
    TestEqual(TEXT("Key count matches emittable TFieldIterator count"),
        Result->Values.Num(), ExpectedKeys.Num());

    // Pin the filter's two directions by name so the shared predicate can't be gutted
    // without failing here. UActorComponent declares these three as
    // UPROPERTY() <name>_DEPRECATED; UHT strips the suffix from the engine name and sets
    // CPF_Deprecated, and UE's own FProperty::ShouldSerializeValue refuses to write them,
    // so they carry no authored state. Their live replacement (CreationMethod for the two
    // bools) is a plain UPROPERTY and is still dumped, so nothing is lost to consumers.
    const TCHAR* DeprecatedNames[] = {
        TEXT("bCreatedByConstructionScript"),
        TEXT("bInstanceComponent"),
        TEXT("UCSModifiedProperties"),
    };
    for (const TCHAR* DeprecatedName : DeprecatedNames)
    {
        FProperty* DeprecatedProp =
            UActorComponent::StaticClass()->FindPropertyByName(FName(DeprecatedName));
        TestNotNull(
            FString::Printf(TEXT("UActorComponent still declares '%s'"), DeprecatedName),
            DeprecatedProp);
        if (DeprecatedProp)
        {
            TestTrue(
                FString::Printf(TEXT("'%s' is CPF_Deprecated"), DeprecatedName),
                DeprecatedProp->HasAnyPropertyFlags(CPF_Deprecated));
        }
        TestFalse(
            FString::Printf(TEXT("Deprecated key '%s' is excluded from the dump"), DeprecatedName),
            Result->HasField(DeprecatedName));
    }

    // Positive side: a plain inherited UPROPERTY and a plain own-class UPROPERTY both
    // survive, proving the filter is targeted rather than blanket-dropping.
    TestTrue(TEXT("Inherited 'bAutoActivate' is present"),
        Result->HasField(TEXT("bAutoActivate")));
    TestTrue(TEXT("Own-class 'RelativeLocation' is present"),
        Result->HasField(TEXT("RelativeLocation")));
    TestTrue(TEXT("Deprecated-field replacement 'CreationMethod' is present"),
        Result->HasField(TEXT("CreationMethod")));

    return true;
}

// ============================================================================
// AssetDumpInheritance.BuildClassPropertyJsonFiltersNonOverridden
// When ParentCDO is supplied and the child CDO is identical to it (same class,
// untouched), BuildClassPropertyJson must emit zero properties — every entry
// would be a non-overridden inherited default.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceBuildClassPropertyJsonFiltersNonOverriddenTest,
    "PinWright.Utils.AssetDumpInheritance.BuildClassPropertyJsonFiltersNonOverridden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceBuildClassPropertyJsonFiltersNonOverriddenTest::RunTest(const FString& Parameters)
{
    UObject* CDO = USceneComponent::StaticClass()->GetDefaultObject();
    TestNotNull(TEXT("CDO is valid"), CDO);
    if (!CDO) return false;

    // Comparing the CDO against itself: every property is identical → all filtered out.
    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(CDO, CDO);
    TestNotNull(TEXT("BuildClassPropertyJson returned non-null"), Result.Get());
    if (!Result) return false;

    TestEqual(TEXT("All non-overridden defaults are filtered out"),
        Result->Values.Num(), 0);

    return true;
}

// ============================================================================
// AssetDumpInheritance.NullParentMeansOverridden
// When ParentContainer=nullptr, is_overridden_locally must be true.
// The call must not crash.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceNullParentMeansOverriddenTest,
    "PinWright.Utils.AssetDumpInheritance.NullParentMeansOverridden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceNullParentMeansOverriddenTest::RunTest(const FString& Parameters)
{
    FProperty* RelLocProp = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("USceneComponent has RelativeLocation"), RelLocProp);
    if (!RelLocProp) return false;

    USceneComponent* CDO = NewObject<USceneComponent>(GetTransientPackage());
    TestNotNull(TEXT("CDO created"), CDO);
    if (!CDO) return false;

    TSharedPtr<FJsonObject> Result = ExportPropertyToJsonValueWithInheritance(
        CDO, /*ParentContainer=*/nullptr, RelLocProp, USceneComponent::StaticClass());

    TestNotNull(TEXT("Result is valid (no crash with null parent)"), Result.Get());
    if (!Result) return false;

    bool bOverridden = false;
    TestTrue(TEXT("is_overridden_locally field present"),
        Result->TryGetBoolField(TEXT("is_overridden_locally"), bOverridden));
    TestTrue(TEXT("Null parent -> is_overridden_locally = true"), bOverridden);

    return true;
}

// ============================================================================
// AssetDumpInheritance.ParentWithoutPropertyDoesNotCrash
// Regression: before the IsChildOf guard, calling the walker with a property
// declared on the CHILD class (so the parent CDO has no slot for it at that
// offset) would crash inside Property->Identical on non-POD types.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceParentWithoutPropertyDoesNotCrashTest,
    "PinWright.Utils.AssetDumpInheritance.ParentWithoutPropertyDoesNotCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceParentWithoutPropertyDoesNotCrashTest::RunTest(const FString& Parameters)
{
    // RelativeLocation is declared on USceneComponent.
    // UInputComponent is a concrete sibling (UActorComponent subclass that does NOT
    // inherit from USceneComponent), so its class has no RelativeLocation slot.
    // UActorComponent itself is marked UCLASS(abstract) and cannot be NewObject'd
    // directly — the ensure in StaticAllocateObjectErrorTests fires on abstract.
    FProperty* RelLocProp = USceneComponent::StaticClass()->FindPropertyByName(TEXT("RelativeLocation"));
    TestNotNull(TEXT("RelativeLocation property found"), RelLocProp);
    if (!RelLocProp) return false;

    USceneComponent* Child = NewObject<USceneComponent>(GetTransientPackage());
    UInputComponent* Parent = NewObject<UInputComponent>(GetTransientPackage());
    TestNotNull(TEXT("Child created"), Child);
    TestNotNull(TEXT("Parent created"), Parent);
    if (!Child || !Parent) return false;

    // AssetClass = USceneComponent (where the property lives).
    // Parent's class = UActorComponent (does NOT own RelativeLocation).
    // Expected: no crash; is_overridden_locally = true (no valid parent baseline).
    TSharedPtr<FJsonObject> Result = ExportPropertyToJsonValueWithInheritance(
        Child, Parent, RelLocProp, USceneComponent::StaticClass());

    TestNotNull(TEXT("Result is valid"), Result.Get());
    if (!Result) return false;

    bool bOverridden = false;
    TestTrue(TEXT("is_overridden_locally field exists"),
        Result->TryGetBoolField(TEXT("is_overridden_locally"), bOverridden));
    TestTrue(TEXT("is_overridden_locally = true when parent class lacks the property"),
        bOverridden);

    return true;
}

// ============================================================================
// AssetDumpInheritance.ParentScsNodesEmitted
// GetBlueprintSCS on a child Blueprint must include both the child's own SCS
// component (ChildProbe) and the node it inherits from its parent Blueprint
// (InheritedRoot), the latter carrying source == "inherited-scs" and an
// inheritedFrom equal to the parent Blueprint's path. Every "parent" reference
// in the array must resolve to a name that is also in the array — the
// dangling-reference invariant that the ancestor-SCS loop in
// PinWright_SCSHandlers.cpp exists to satisfy.
//
// Counterfactual: delete that loop and InheritedRoot vanishes from the array
// while ChildProbe keeps pointing at it, failing both the invariant and the
// source/inheritedFrom assertions below.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceParentScsNodesEmittedTest,
    "PinWright.Utils.AssetDumpInheritance.ParentScsNodesEmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceParentScsNodesEmittedTest::RunTest(const FString& Parameters)
{
    FScsInheritanceFixture Fixture;
    ON_SCOPE_EXIT { CleanupScsInheritanceFixture(Fixture); };

    // The fixture is synthesized in-test, so a build failure is a real failure — never a skip.
    FString FixtureError;
    if (!BuildScsInheritanceFixture(Fixture, FixtureError))
    {
        AddError(FString::Printf(
            TEXT("ParentScsNodesEmitted: inherited-SCS fixture could not be built (%s)"),
            *FixtureError));
        return false;
    }

    // Precondition: the inherited node must NOT be owned by the child's own SCS, otherwise
    // the local-SCS loop would emit it as source == "scs" and the assertions below would
    // pass without the ancestor loop ever running.
    TestNull(TEXT("child SCS does not own the inherited node locally"),
        Fixture.ChildBP->SimpleConstructionScript->FindSCSNode(FName(ScsInheritParentNodeName)));

    TSharedPtr<FJsonObject> ScsResult = FSCSHandlers::GetBlueprintSCS(Fixture.ChildPath);
    TestNotNull(TEXT("GetBlueprintSCS returned a result"), ScsResult.Get());
    if (!ScsResult.IsValid()) return false;

    // Assertion 1: success == true
    bool bSuccess = false;
    TestTrue(TEXT("result.success is true"),
        ScsResult->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess);
    if (!bSuccess) return false;

    const TArray<TSharedPtr<FJsonValue>>* ComponentsArray = nullptr;
    TestTrue(TEXT("result.components is an array"),
        ScsResult->TryGetArrayField(TEXT("components"), ComponentsArray));
    if (!ComponentsArray) return false;

    // Assertion 2: build a name set from every component in the array.
    TSet<FString> NameSet;
    for (const TSharedPtr<FJsonValue>& Entry : *ComponentsArray)
    {
        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Entry->TryGetObject(ObjPtr) || !ObjPtr) continue;
        FString Name;
        if ((*ObjPtr)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
        {
            NameSet.Add(Name);
        }
    }

    // Assertion 3: dangling-reference invariant — every non-empty "parent" must
    // resolve to a name present in the same components array.
    for (const TSharedPtr<FJsonValue>& Entry : *ComponentsArray)
    {
        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Entry->TryGetObject(ObjPtr) || !ObjPtr) continue;
        FString ParentName;
        if ((*ObjPtr)->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
        {
            TestTrue(
                FString::Printf(TEXT("Parent ref '%s' exists in the name set (no dangling ref)"),
                    *ParentName),
                NameSet.Contains(ParentName));
        }
    }

    // Assertion 4: own SCS component and inherited node are both present.
    TestTrue(TEXT("ChildProbe (own SCS component) is in the name set"),
        NameSet.Contains(ScsInheritChildNodeName));
    TestTrue(TEXT("InheritedRoot (inherited from the parent Blueprint) is in the name set"),
        NameSet.Contains(ScsInheritParentNodeName));

    // Assertion 5: the inherited node carries source == "inherited-scs" and an
    // inheritedFrom equal to the parent Blueprint's own path; the child's node points at
    // it by name. Both literals come from PinWright_SCSHandlers.cpp (KSourceInherited /
    // ParentBP->GetPathName()).
    bool bFoundInheritedRoot = false;
    bool bFoundChildProbe    = false;
    for (const TSharedPtr<FJsonValue>& Entry : *ComponentsArray)
    {
        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Entry->TryGetObject(ObjPtr) || !ObjPtr) continue;
        FString Name;
        if (!(*ObjPtr)->TryGetStringField(TEXT("name"), Name)) continue;

        if (Name == ScsInheritParentNodeName)
        {
            FString Source;
            TestTrue(TEXT("InheritedRoot has 'source' field"),
                (*ObjPtr)->TryGetStringField(TEXT("source"), Source));
            TestEqual(TEXT("InheritedRoot source == 'inherited-scs'"),
                Source, FString(TEXT("inherited-scs")));

            FString InheritedFrom;
            TestTrue(TEXT("InheritedRoot has 'inheritedFrom' field"),
                (*ObjPtr)->TryGetStringField(TEXT("inheritedFrom"), InheritedFrom));
            TestEqual(TEXT("inheritedFrom is the parent Blueprint's path"),
                InheritedFrom, Fixture.ParentBP->GetPathName());

            bFoundInheritedRoot = true;
        }
        else if (Name == ScsInheritChildNodeName)
        {
            FString Source;
            TestTrue(TEXT("ChildProbe has 'source' field"),
                (*ObjPtr)->TryGetStringField(TEXT("source"), Source));
            TestEqual(TEXT("ChildProbe source == 'scs'"), Source, FString(TEXT("scs")));

            FString ParentName;
            TestTrue(TEXT("ChildProbe has 'parent' field"),
                (*ObjPtr)->TryGetStringField(TEXT("parent"), ParentName));
            TestEqual(TEXT("ChildProbe parent is the inherited node"),
                ParentName, FString(ScsInheritParentNodeName));

            bFoundChildProbe = true;
        }
    }
    TestTrue(TEXT("InheritedRoot entry was found in the components array"),
        bFoundInheritedRoot);
    TestTrue(TEXT("ChildProbe entry was found in the components array"),
        bFoundChildProbe);

    return true;
}

// ============================================================================
// AssetDumpInheritance.ScsTxtSidecarEmitted
// Dumping the child Blueprint must emit the scs.txt sidecar next to scs.json
// (AssetDumpHandler's AddStringFile(DumpFileNames::ScsTxt, SCSTextEmitter::BuildText))
// and that text must carry the inherited node's metadata plus the nested children
// block that the emitter reconstructs from the `parent` links.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpInheritanceScsTxtSidecarEmittedTest,
    "PinWright.Utils.AssetDumpInheritance.ScsTxtSidecarEmitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpInheritanceScsTxtSidecarEmittedTest::RunTest(const FString& Parameters)
{
    FScsInheritanceFixture Fixture;
    ON_SCOPE_EXIT { CleanupScsInheritanceFixture(Fixture); };

    FString FixtureError;
    if (!BuildScsInheritanceFixture(Fixture, FixtureError))
    {
        AddError(FString::Printf(
            TEXT("ScsTxtSidecarEmitted: inherited-SCS fixture could not be built (%s)"),
            *FixtureError));
        return false;
    }
    const FString FixturePath = Fixture.ChildObjectPath();

    const FString OutRoot = FPaths::Combine(
        FPaths::ProjectIntermediateDir(),
        TEXT("PinWrightTests"),
        TEXT("ScsTxtSidecar"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().DeleteDirectory(*OutRoot, /*RequireExists=*/false, /*Tree=*/true);
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*OutRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    AssetDumpHandler::FDumpSingleResult DumpResult =
        AssetDumpHandler::DumpSingleAsset(FixturePath, OutRoot);
    TestTrue(TEXT("asset dump succeeds"), DumpResult.ErrorCode.IsEmpty());
    if (!DumpResult.ErrorCode.IsEmpty())
    {
        AddError(DumpResult.ErrorMessage);
        return false;
    }

    auto HasWrittenFile = [&DumpResult](const FString& FileName)
    {
        for (FString WrittenPath : DumpResult.WrittenPaths)
        {
            FPaths::NormalizeFilename(WrittenPath);
            if (WrittenPath.EndsWith(FString(TEXT("/")) + FileName))
            {
                return true;
            }
        }
        return false;
    };

    TestTrue(TEXT("scs.json remains emitted"),
        HasWrittenFile(DumpFileNames::Scs));
    TestTrue(TEXT("scs.txt is emitted"),
        HasWrittenFile(DumpFileNames::ScsTxt));

    FString ScsText;
    const FString ScsTextPath = DumpResult.DumpDir / DumpFileNames::ScsTxt;
    TestTrue(TEXT("scs.txt is readable"),
        FFileHelper::LoadFileToString(ScsText, *ScsTextPath));
    TestTrue(TEXT("scs.txt has non-trivial content"), ScsText.Len() > 0);
    TestTrue(TEXT("scs.txt emits the inherited node as the tree root"),
        ScsText.Contains(FString::Printf(TEXT("component(%s) {"), ScsInheritParentNodeName)));
    // The child sits INSIDE the inherited node's children block: SCSTextEmitter resolves the
    // `parent` link against the components array, which only works because the ancestor-SCS
    // loop put the inherited node there. Indentation is the emitter's own (2 spaces per level,
    // so a child of a root node lands at 4).
    TestTrue(TEXT("scs.txt contains nested children block"),
        ScsText.Contains(FString::Printf(TEXT("children {\n    component(%s) {"),
            ScsInheritChildNodeName)));
    TestTrue(TEXT("scs.txt preserves inherited source metadata"),
        ScsText.Contains(TEXT("source: inherited-scs")));
    TestTrue(TEXT("scs.txt names the Blueprint the node was inherited from"),
        ScsText.Contains(FString::Printf(TEXT("inherits: %s"), *Fixture.ParentBP->GetPathName())));

    return true;
}

// ============================================================================
// AssetDumpInheritance.SuppressesUserWidgetCompilerFlags
// Regression for B-asset-dump-properties-spurious-override-on-bp-internal-bools.
// WidgetBlueprintCompiler unconditionally rewrites bHasScriptImplementedPaint /
// bHasScriptImplementedTick / bAutomaticallyRegisterInputOnConstruction on every
// WBP CDO from the BP's graph contents. The byte-compare against the parent
// UUserWidget CDO therefore always reports them as "overridden" — pure noise on
// every widget BP. The IsCompilerManagedUserWidgetFlag helper in PropertyUtils
// suppresses this; verify the three flags drop from BuildClassPropertyJson output
// while a deliberately-overridden non-managed UPROPERTY (Priority) still appears.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpPropertiesSuppressesUserWidgetCompilerFlagsTest,
    "PinWright.utils.asset_dump_inheritance.SuppressesUserWidgetCompilerFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpPropertiesSuppressesUserWidgetCompilerFlagsTest::RunTest(const FString& Parameters)
{
    // Build a transient WBP under /Game/_Test/ — same convention as the other widget
    // tests in the suite. Unique GUID suffix avoids collisions across test reruns.
    const FString PackagePath = FString::Printf(TEXT("/Game/_Test/WBP_CompilerFlagSuppression_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);

    UPackage* Package = CreatePackage(*PackagePath);
    TestNotNull(TEXT("transient package created"), Package);
    if (!Package) return false;

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        Package,
        FName(*AssetName),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));
    TestNotNull(TEXT("WidgetBlueprint allocated"), WBP);
    if (!WBP) return false;

    // Compile so the WBPGC CDO carries the compiler-set values. Default factory
    // output has no ReceiveTick/ReceivePaint event in the graph — that's the
    // desired starting state for this regression.
    FKismetEditorUtilities::CompileBlueprint(WBP);

    UClass* GeneratedClass = WBP->GeneratedClass;
    TestNotNull(TEXT("WBP has GeneratedClass"), GeneratedClass);
    if (!GeneratedClass) return false;

    UObject* ChildCDO = GeneratedClass->GetDefaultObject();
    UObject* ParentCDO = GeneratedClass->GetSuperClass()->GetDefaultObject();
    TestNotNull(TEXT("child CDO present"), ChildCDO);
    TestNotNull(TEXT("parent CDO present"), ParentCDO);
    if (!ChildCDO || !ParentCDO) return false;

    // Sanity-check seed: deliberately override Priority (int32 declared on
    // UUserWidget, default 0) so we can assert non-managed overrides still flow
    // through. This proves the filter is targeted, not blanket-suppressing.
    FProperty* PriorityProp = UUserWidget::StaticClass()->FindPropertyByName(TEXT("Priority"));
    TestNotNull(TEXT("UUserWidget has Priority property"), PriorityProp);
    if (!PriorityProp) return false;
    if (FIntProperty* IntProp = CastField<FIntProperty>(PriorityProp))
    {
        IntProp->SetPropertyValue_InContainer(ChildCDO, 7);
    }

    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(ChildCDO, ParentCDO);
    TestNotNull(TEXT("BuildClassPropertyJson returned non-null"), Result.Get());
    if (!Result) return false;

    // Counterfactual: if IsCompilerManagedUserWidgetFlag in PropertyUtils.cpp is reverted to always-false, this test fails because UUserWidget C++ CDO holds bHasScriptImplementedPaint=true while the WBPGC CDO holds false (compiler-set), so Property->Identical() returns false and the field is emitted as overridden.
    TestFalse(TEXT("bHasScriptImplementedPaint suppressed (compiler-managed)"),
        Result->HasField(TEXT("bHasScriptImplementedPaint")));
    TestFalse(TEXT("bHasScriptImplementedTick suppressed (compiler-managed)"),
        Result->HasField(TEXT("bHasScriptImplementedTick")));
    TestFalse(TEXT("bAutomaticallyRegisterInputOnConstruction suppressed (compiler-managed)"),
        Result->HasField(TEXT("bAutomaticallyRegisterInputOnConstruction")));

    // Targeted-filter sanity: a deliberately-overridden non-compiler-managed UPROPERTY
    // (Priority) must still flow through.
    TestTrue(TEXT("Priority (deliberately overridden, not compiler-managed) is present"),
        Result->HasField(TEXT("Priority")));

    // Cleanup: close any auto-opened editor and drop the transient package's dirty
    // flag so it doesn't pollute the editor's save-pending set.
    if (GEditor)
    {
        if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
        {
            AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
        }
    }
    if (UPackage* Outer = WBP->GetOutermost())
    {
        Outer->SetDirtyFlag(false);
    }

    return true;
}
