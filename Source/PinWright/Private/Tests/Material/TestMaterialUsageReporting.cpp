// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the material-usage half of the shader-state report.
//
// THE DEFECT. `rendersDefaultMaterial` was computed on one axis - is there a complete game-thread
// shader map for the resource GetMaterialResource hands back - and published under a name that
// reads as a claim about the asset the caller named and about every mesh it is on. It is neither:
//
//   1. USAGE. A vertex factory refuses to compile its permutations for a usage the material does
//      not declare (GPUSkinVertexFactory.cpp gates on bIsUsedWithSkeletalMesh), so the shader map
//      is COMPLETE without them. A material missing bUsedWithSkeletalMesh therefore reported
//      `rendersDefaultMaterial:false` at the exact moment the skinned mesh it was assigned to drew
//      the engine Default Material - and no material verb emitted a usage flag of any kind, so
//      nothing else in any response separated the two states either.
//   2. SUBJECT. A material instance owns a shader only while bHasStaticPermutationResource is set.
//      Without it GetMaterialResource forwards to the parent, so the block describes the PARENT
//      while naming the instance.
//
// WHAT IS ASSERTED HERE:
//   1. The published block names its subject and its declared usages, with no compiler needed —
//      and a parent-inherited state with no parent omits `measuredMaterialPath` rather than
//      emitting an empty one, with a remedy that does not point at the omitted field.
//   1b. The multi-material fold has no single subject, so it must still carry a subject-free
//      `rendersDefaultMaterialScope` and per-row subjects/usages in `materials[]` — otherwise
//      `material.compile_mgir` over a multi-entry document ships exactly the bare boolean.
//   2. A material with a COMPLETE shader map and no skeletal usage: rendersDefaultMaterial is
//      false while the skeletal-consumer answer is true, and declaring the flag flips the second
//      one. This is the four-verb false green, reduced to one discriminating pair.
//   3. The `usage` block carries every EMaterialUsage with the UMaterial property behind it.
//   4. The subject resolver separates a parent-inherited instance from one that owns a static
//      permutation, and names the right object to compile in each case.
//
// COUNTERFACTUAL. Revert MaterialShaderState.h / MaterialUsageFlags.h and (1) fails on the absent
// measuredSubject / declaredUsages fields, (2) fails because RendersDefaultMaterialForUsage
// degenerates to RendersDefaultMaterial and returns false for the undeclared usage, (3) fails on
// the absent usage block, and (4) fails because ResolveMeasuredSubject / ResolveCompileSubject do
// not exist and the parent-inherited instance is measured and compiled as if it owned its shader.
// Make `rendersDefaultMaterialScope` conditional on a known subject again and (1b) fails while the
// rest still pass, which is the distinction that names which half broke.
//
// VACUITY. Only (2) needs the platform shader compiler: a host that cannot compile shaders reports
// notCompiled for every material, which makes rendersDefaultMaterial true for both flag states and
// the pair non-discriminating. That case is reported through PinWrightTestSkip rather than
// asserted past. (1), (3) and (4) are measured off object state and always run.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialInterface.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Material/MaterialShaderStateTestFixtures.h"
#include "Handlers/Material/MaterialCompileErrorCollector.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Handlers/Material/MaterialUsageFlags.h"

namespace
{
    bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
        const FString& Needle)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text == Needle)
            {
                return true;
            }
        }
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// 1. The block says what it measured
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialUsageShaderCompileSubjectTest,
    "PinWright.material.usage.ShaderCompileBlockNamesItsSubjectAndDeclaredUsages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialUsageShaderCompileSubjectTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material =
        PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
            TEXT("UsageSubject"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const PinWright::MaterialShaderState::FState State =
        PinWright::MaterialShaderState::Probe(Material);
    TestEqual(TEXT("a UMaterial probes its own resource"),
        static_cast<int32>(State.Subject),
        static_cast<int32>(PinWright::MaterialShaderState::EMeasuredSubject::BaseMaterial));
    TestEqual(TEXT("the measured path is the material itself"),
        State.MeasuredMaterialPath, Material->GetPathName());
    TestFalse(TEXT("a fresh material declares no skeletal usage"),
        State.DeclaredUsages.Contains(TEXT("SkeletalMesh")));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::MaterialShaderState::AddReport(Result, State);

    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (!TestTrue(TEXT("response carries the shaderCompile block"),
            Result->TryGetObjectField(TEXT("shaderCompile"), Block) && Block != nullptr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    FString Subject;
    if (TestTrue(TEXT("shaderCompile names the measured subject"),
            (*Block)->TryGetStringField(TEXT("measuredSubject"), Subject)))
    {
        TestEqual(TEXT("subject is the base material"), Subject, FString(TEXT("baseMaterial")));
    }

    FString MeasuredPath;
    if (TestTrue(TEXT("shaderCompile names the measured asset"),
            (*Block)->TryGetStringField(TEXT("measuredMaterialPath"), MeasuredPath)))
    {
        TestEqual(TEXT("measured asset is the material"), MeasuredPath, Material->GetPathName());
    }

    const TArray<TSharedPtr<FJsonValue>>* Usages = nullptr;
    TestTrue(TEXT("shaderCompile carries declaredUsages"),
        (*Block)->TryGetArrayField(TEXT("declaredUsages"), Usages) && Usages != nullptr);
    TestFalse(TEXT("declaredUsages does not claim skeletal usage"),
        JsonStringArrayContains(*Block, TEXT("declaredUsages"), TEXT("SkeletalMesh")));

    FString Scope;
    if (TestTrue(TEXT("rendersDefaultMaterial carries its scope"),
            (*Block)->TryGetStringField(TEXT("rendersDefaultMaterialScope"), Scope)))
    {
        TestTrue(TEXT("the scope names the usage gap the flag cannot see"),
            Scope.Contains(TEXT("EMaterialUsage")));
    }

    // The declared set is measured, not a constant: writing the flag changes it.
    PinWrightMaterialShaderStateTestFixtures::DeclareMaterialUsage(Material, MATUSAGE_SkeletalMesh);
    const PinWright::MaterialShaderState::FState AfterState =
        PinWright::MaterialShaderState::Probe(Material);
    TestTrue(TEXT("declaredUsages follows the flag"),
        AfterState.DeclaredUsages.Contains(TEXT("SkeletalMesh")));

    // A parent-inherited state with no parent has no asset to name. The field must be absent
    // rather than empty, and the remedy must not send the caller to read it.
    PinWright::MaterialShaderState::FState Orphan;
    Orphan.Status = PinWright::MaterialShaderState::EStatus::NotCompiled;
    Orphan.Subject = PinWright::MaterialShaderState::EMeasuredSubject::ParentInherited;
    Orphan.bRendersDefaultMaterial = true;

    TSharedPtr<FJsonObject> OrphanResult = MakeShared<FJsonObject>();
    PinWright::MaterialShaderState::AddReport(OrphanResult, Orphan);
    const TSharedPtr<FJsonObject>* OrphanBlock = nullptr;
    if (TestTrue(TEXT("parentless state still publishes the block"),
            OrphanResult->TryGetObjectField(TEXT("shaderCompile"), OrphanBlock) &&
            OrphanBlock != nullptr))
    {
        FString Unused;
        TestFalse(TEXT("no measuredMaterialPath naming nothing"),
            (*OrphanBlock)->TryGetStringField(TEXT("measuredMaterialPath"), Unused));
        FString OrphanHint;
        if (TestTrue(TEXT("the parentless case has its own remedy"),
                (*OrphanBlock)->TryGetStringField(TEXT("hint"), OrphanHint)))
        {
            TestTrue(TEXT("the remedy says the instance has no parent"),
                OrphanHint.Contains(TEXT("NO PARENT")));
            TestFalse(TEXT("and does not point at the omitted field"),
                OrphanHint.Contains(TEXT("measuredMaterialPath")));
        }
    }

    CleanupTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 1b. The multi-material fold must not ship the bare flag either
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialUsageMultiMaterialFoldTest,
    "PinWright.material.usage.MultiMaterialFoldKeepsRendersDefaultMaterialQualified",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialUsageMultiMaterialFoldTest::RunTest(const FString& Parameters)
{
    FString FirstPath;
    UMaterial* First = PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
        TEXT("UsageFoldA"), FirstPath);
    FString SecondPath;
    UMaterial* Second = PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
        TEXT("UsageFoldB"), SecondPath);
    if (!TestNotNull(TEXT("first material created"), First) ||
        !TestNotNull(TEXT("second material created"), Second))
    {
        CleanupTestAsset(FirstPath);
        CleanupTestAsset(SecondPath);
        return true;
    }

    // Different usage sets, so a row copied from the aggregate rather than measured per material
    // would be visible.
    PinWrightMaterialShaderStateTestFixtures::DeclareMaterialUsage(First, MATUSAGE_SkeletalMesh);

    PinWright::MaterialShaderState::FState Folded;
    Folded.Accumulate(First->GetPathName(), PinWright::MaterialShaderState::Probe(First));
    Folded.Accumulate(Second->GetPathName(), PinWright::MaterialShaderState::Probe(Second));

    TestEqual(TEXT("both materials folded"), Folded.PerMaterial.Num(), 2);
    TestEqual(TEXT("no single subject survives the fold"),
        static_cast<int32>(Folded.Subject),
        static_cast<int32>(PinWright::MaterialShaderState::EMeasuredSubject::None));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::MaterialShaderState::AddReport(Result, Folded);
    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (!TestTrue(TEXT("the fold publishes the block"),
            Result->TryGetObjectField(TEXT("shaderCompile"), Block) && Block != nullptr))
    {
        CleanupTestAsset(FirstPath);
        CleanupTestAsset(SecondPath);
        return true;
    }

    bool bRendersDefault = false;
    TestTrue(TEXT("the fold still emits rendersDefaultMaterial"),
        (*Block)->TryGetBoolField(TEXT("rendersDefaultMaterial"), bRendersDefault));

    // The point of the case: the flag is emitted, so its qualification must be too.
    FString Scope;
    if (TestTrue(TEXT("the fold qualifies rendersDefaultMaterial"),
            (*Block)->TryGetStringField(TEXT("rendersDefaultMaterialScope"), Scope)))
    {
        TestTrue(TEXT("the scope names the usage gap"), Scope.Contains(TEXT("EMaterialUsage")));
        TestTrue(TEXT("and routes to the per-material rows"),
            Scope.Contains(TEXT("materials[]")));
    }

    FString Unused;
    TestFalse(TEXT("no aggregate measuredSubject is invented"),
        (*Block)->TryGetStringField(TEXT("measuredSubject"), Unused));

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!TestTrue(TEXT("materials[] is present"),
            (*Block)->TryGetArrayField(TEXT("materials"), Rows) && Rows != nullptr))
    {
        CleanupTestAsset(FirstPath);
        CleanupTestAsset(SecondPath);
        return true;
    }
    TestEqual(TEXT("one row per folded material"), Rows->Num(), 2);

    int32 RowsWithSubject = 0;
    int32 RowsDeclaringSkeletal = 0;
    for (const TSharedPtr<FJsonValue>& Row : *Rows)
    {
        const TSharedPtr<FJsonObject>* RowObject = nullptr;
        if (!Row.IsValid() || !Row->TryGetObject(RowObject) || !RowObject)
        {
            continue;
        }
        FString RowSubject;
        if ((*RowObject)->TryGetStringField(TEXT("measuredSubject"), RowSubject) &&
            RowSubject == TEXT("baseMaterial"))
        {
            ++RowsWithSubject;
        }
        if (JsonStringArrayContains(*RowObject, TEXT("declaredUsages"), TEXT("SkeletalMesh")))
        {
            ++RowsDeclaringSkeletal;
        }
    }
    TestEqual(TEXT("every row names its own subject"), RowsWithSubject, 2);
    TestEqual(TEXT("declaredUsages is per row, not the aggregate's"), RowsDeclaringSkeletal, 1);

    CleanupTestAsset(FirstPath);
    CleanupTestAsset(SecondPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 2. rendersDefaultMaterial:false while the skinned consumer gets the Default Material
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialUsageSkeletalContradictionTest,
    "PinWright.material.usage.UndeclaredSkeletalUsageContradictsRendersDefaultMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialUsageSkeletalContradictionTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material =
        PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
            TEXT("UsageSkeletal"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    TestFalse(TEXT("the fixture starts without skeletal usage"),
        PinWright::MaterialUsage::DeclaresUsage(Material, MATUSAGE_SkeletalMesh));

    const PinWright::MaterialShaderState::FState State =
        PinWright::MaterialShaderState::ProbeAndWait(Material);
    if (State.Status != PinWright::MaterialShaderState::EStatus::Completed)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
            FString::Printf(TEXT("The material reported '%s' rather than 'completed', so this host "
                "did not run the platform shader compiler. Without a complete shader map "
                "rendersDefaultMaterial is true for both flag states and the contradiction this "
                "test asserts on cannot be produced."),
                PinWright::MaterialShaderState::ToWire(State.Status)));
        CleanupTestAsset(AssetPath);
        return true;
    }

    // The false green, verbatim: the shader-map axis says the material renders...
    TestFalse(TEXT("the shader-map answer is that this does not render the Default Material"),
        State.bRendersDefaultMaterial);
    // ...while the mesh it is assigned to draws the engine Default Material, because the skeletal
    // permutations were never compiled into that complete map.
    TestTrue(TEXT("the skeletal-consumer answer is that it does"),
        PinWright::MaterialShaderState::RendersDefaultMaterialForUsage(
            Material, MATUSAGE_SkeletalMesh));

    // Discriminating in the other direction: with the usage declared, the consumer answer
    // collapses back onto the shader-map answer instead of being a constant true.
    PinWrightMaterialShaderStateTestFixtures::DeclareMaterialUsage(Material, MATUSAGE_SkeletalMesh);
    TestTrue(TEXT("the flag is declared after the write"),
        PinWright::MaterialUsage::DeclaresUsage(Material, MATUSAGE_SkeletalMesh));
    TestTrue(TEXT("a declared usage leaves only the shader-map answer"),
        PinWright::MaterialShaderState::RendersDefaultMaterialForUsage(
            Material, MATUSAGE_SkeletalMesh) ==
        PinWright::MaterialShaderState::RendersDefaultMaterial(Material));

    CleanupTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 3. The usage block the read verbs publish
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialUsageBlockTest,
    "PinWright.material.usage.UsageBlockCarriesEveryFlagWithItsProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialUsageBlockTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material =
        PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
            TEXT("UsageBlock"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    PinWrightMaterialShaderStateTestFixtures::DeclareMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes);

    const TSharedPtr<FJsonObject> Block =
        PinWright::MaterialUsage::BuildUsageReport(Material);
    if (!TestTrue(TEXT("usage block built"), Block.IsValid()))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    TestTrue(TEXT("declared lists the flag that was set"),
        JsonStringArrayContains(Block, TEXT("declared"), TEXT("InstancedStaticMeshes")));
    TestFalse(TEXT("declared omits a flag that was not set"),
        JsonStringArrayContains(Block, TEXT("declared"), TEXT("SkeletalMesh")));

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!TestTrue(TEXT("usage carries the whole flag set"),
            Block->TryGetArrayField(TEXT("flags"), Rows) && Rows != nullptr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    TestEqual(TEXT("one row per EMaterialUsage"), Rows->Num(), static_cast<int32>(MATUSAGE_MAX));

    bool bFoundSkeletalRow = false;
    for (const TSharedPtr<FJsonValue>& Row : *Rows)
    {
        const TSharedPtr<FJsonObject>* RowObject = nullptr;
        if (!Row.IsValid() || !Row->TryGetObject(RowObject) || !RowObject)
        {
            continue;
        }
        FString Usage;
        if (!(*RowObject)->TryGetStringField(TEXT("usage"), Usage) ||
            Usage != TEXT("SkeletalMesh"))
        {
            continue;
        }
        bFoundSkeletalRow = true;

        bool bDeclared = true;
        (*RowObject)->TryGetBoolField(TEXT("declared"), bDeclared);
        TestFalse(TEXT("the skeletal row reports the flag off"), bDeclared);

        FString PropertyName;
        if (TestTrue(TEXT("the skeletal row names the property behind the flag"),
                (*RowObject)->TryGetStringField(TEXT("property"), PropertyName)))
        {
            TestEqual(TEXT("property is the one property.set takes"),
                PropertyName, FString(TEXT("bUsedWithSkeletalMesh")));
        }
    }
    TestTrue(TEXT("the skeletal usage has a row"), bFoundSkeletalRow);

    bool bAutoSet = false;
    TestTrue(TEXT("usage reports the editor's silent auto-set"),
        Block->TryGetBoolField(TEXT("autoSetInEditor"), bAutoSet));

    FString Hint;
    TestTrue(TEXT("usage names the write route"),
        Block->TryGetStringField(TEXT("hint"), Hint) && !Hint.IsEmpty());

    CleanupTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 4. An instance's subject: parent-inherited versus its own static permutation
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialShaderStateInstanceSubjectTest,
    "PinWright.material.shader_state.MeasuredSubjectDistinguishesInstanceFromParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialShaderStateInstanceSubjectTest::RunTest(const FString& Parameters)
{
    FString MaterialPath;
    UMaterial* Material =
        PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
            TEXT("SubjectParent"), MaterialPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(MaterialPath);
        return true;
    }

    FString InstancePath;
    UMaterialInstanceConstant* Instance =
        PinWrightMaterialShaderStateTestFixtures::MakeMaterialInstance(
            Material, TEXT("SubjectInstance"), InstancePath);
    if (!TestNotNull(TEXT("Instance created"), Instance))
    {
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(MaterialPath);
        return true;
    }

    TestFalse(TEXT("an override-free instance owns no static permutation"),
        Instance->bHasStaticPermutationResource != 0);
    TestEqual(TEXT("its shader state describes the parent"),
        static_cast<int32>(PinWright::MaterialShaderState::ResolveMeasuredSubject(Instance)),
        static_cast<int32>(PinWright::MaterialShaderState::EMeasuredSubject::ParentInherited));
    TestEqual(TEXT("and names the parent as the measured asset"),
        PinWright::MaterialShaderState::ResolveMeasuredMaterialPath(Instance),
        Material->GetPathName());
    TestTrue(TEXT("compiling it means compiling the parent"),
        MaterialCompileErrorCollector::ResolveCompileSubject(Instance) ==
        static_cast<UMaterialInterface*>(Material));

    // A base-property override differing from the parent is what UpdateStaticPermutation tests, so
    // this reaches the same bHasStaticPermutationResource=true state a static-switch override does.
    Instance->BasePropertyOverrides.bOverride_TwoSided = true;
    Instance->BasePropertyOverrides.TwoSided = !Material->TwoSided;
    Instance->PostEditChange();

    if (Instance->bHasStaticPermutationResource == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("static-permutation-unavailable"),
            TEXT("The base-property override did not produce a static permutation resource on this "
                 "host, so the instance-owned half of the subject split could not be exercised."));
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(MaterialPath);
        return true;
    }

    TestEqual(TEXT("an instance with a permutation describes itself"),
        static_cast<int32>(PinWright::MaterialShaderState::ResolveMeasuredSubject(Instance)),
        static_cast<int32>(
            PinWright::MaterialShaderState::EMeasuredSubject::InstanceStaticPermutation));
    TestEqual(TEXT("and names itself as the measured asset"),
        PinWright::MaterialShaderState::ResolveMeasuredMaterialPath(Instance),
        Instance->GetPathName());
    TestTrue(TEXT("compiling it means compiling the instance"),
        MaterialCompileErrorCollector::ResolveCompileSubject(Instance) ==
        static_cast<UMaterialInterface*>(Instance));

    CleanupTestAsset(InstancePath);
    CleanupTestAsset(MaterialPath);
    return true;
}
