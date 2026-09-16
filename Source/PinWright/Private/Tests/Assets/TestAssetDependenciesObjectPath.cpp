// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-get-dependencies-object-path-empty.
//
// asset.get_dependencies / asset.get_dependencies_classified feed assetPath
// straight into AssetRegistry::GetDependencies, which is keyed on the *package*
// FName. Before the fix they passed the raw caller string, so the object-path
// form (/Game/.../SM_Gear.SM_Gear) — the form the sibling read verbs accept —
// found no package node and silently returned {"dependencies":[]} with
// isError:false. The handlers now normalize the .AssetName object suffix to a
// package name (via NormalizeAssetPath) before the lookup, so either form
// resolves to the same dependency list.
//
// These tests assert that equivalence against the live asset registry. They are
// content-agnostic: they scan the registry for any asset that actually has a
// hard package dependency, then compare the two path forms. If no such asset is
// present (empty headless registry) they warn-and-pass rather than fail, exactly
// like the existing asset.search.NativeSubclass live test.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Misc/PackageName.h"

namespace
{
    // Find a /Game asset that has at least one hard package dependency, and return
    // its package-name path plus the object-path (package + ".AssetName") form.
    // Returns false if the registry holds no suitable asset.
    bool FindAssetWithHardDependency(FString& OutPackagePath, FString& OutObjectPath)
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAssets(Filter, AssetDataList);

        for (const FAssetData& Data : AssetDataList)
        {
            TArray<FName> Deps;
            AssetRegistry.GetDependencies(Data.PackageName, Deps,
                UE::AssetRegistry::EDependencyCategory::Package,
                UE::AssetRegistry::EDependencyQuery::Hard);
            if (Deps.Num() > 0)
            {
                OutPackagePath = Data.PackageName.ToString();
                OutObjectPath = FString::Printf(TEXT("%s.%s"),
                    *OutPackagePath, *Data.AssetName.ToString());
                return true;
            }
        }
        return false;
    }

    // Find a /Game asset whose recursive hard-package-dependency closure is
    // strictly larger than its direct dependency set (at least one dep-of-a-dep
    // is not itself a direct dep). Returns the package-name path. Returns false
    // if the registry holds no such two-level chain.
    bool FindAssetWithTransitiveDependency(FString& OutPackagePath)
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAssets(Filter, AssetDataList);

        auto HardPackageDeps = [&AssetRegistry](const FName& Node)
        {
            TArray<FName> Deps;
            AssetRegistry.GetDependencies(Node, Deps,
                UE::AssetRegistry::EDependencyCategory::Package,
                UE::AssetRegistry::EDependencyQuery::Hard);
            return Deps;
        };

        for (const FAssetData& Data : AssetDataList)
        {
            const TArray<FName> Direct = HardPackageDeps(Data.PackageName);
            if (Direct.Num() == 0) continue;

            TSet<FName> DirectSet;
            DirectSet.Append(Direct);
            for (const FName& Dep : Direct)
            {
                for (const FName& Sub : HardPackageDeps(Dep))
                {
                    if (Sub != Data.PackageName && !DirectSet.Contains(Sub))
                    {
                        OutPackagePath = Data.PackageName.ToString();
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Pull the "dependencies" string array out of a captured handler response.
    TArray<FString> ExtractDependencies(const TSharedPtr<FJsonObject>& Result)
    {
        TArray<FString> Out;
        if (!Result.IsValid()) return Out;
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Result->TryGetArrayField(TEXT("dependencies"), Arr) && Arr)
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                if (V.IsValid()) Out.Add(V->AsString());
            }
        }
        Out.Sort();
        return Out;
    }
}

// ============================================================================
// asset.get_dependencies — object-path form must resolve like the package form
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetDependenciesObjectPathEquivalenceTest,
    "PinWright.asset.get_dependencies.ObjectPathEquivalence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGetDependenciesObjectPathEquivalenceTest::RunTest(const FString& Parameters)
{
    FString PackagePath, ObjectPath;
    if (!FindAssetWithHardDependency(PackagePath, ObjectPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("No /Game asset with a hard package dependency found in the asset registry; "
                 "object-path/package-path dependency equivalence could not be exercised."));
        return true;
    }

    // Package-name form (the form that always worked) — baseline.
    TSharedPtr<FJsonObject> PkgPayload = MakeShared<FJsonObject>();
    PkgPayload->SetStringField(TEXT("assetPath"), PackagePath);
    FTestResponseCapture PkgCapture;
    TestTrue(TEXT("asset.get_dependencies handler found (package form)"),
        InvokeHandlerWithCapture(TEXT("asset.get_dependencies"), PkgPayload, PkgCapture));
    const TArray<FString> PkgDeps = ExtractDependencies(PkgCapture.Result);
    TestTrue(TEXT("package-name form returns at least one dependency"), PkgDeps.Num() > 0);

    // Object-path form (package + .AssetName) — the form that used to silently
    // return []. After the fix it must yield the identical dependency list.
    TSharedPtr<FJsonObject> ObjPayload = MakeShared<FJsonObject>();
    ObjPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture ObjCapture;
    TestTrue(TEXT("asset.get_dependencies handler found (object-path form)"),
        InvokeHandlerWithCapture(TEXT("asset.get_dependencies"), ObjPayload, ObjCapture));
    const TArray<FString> ObjDeps = ExtractDependencies(ObjCapture.Result);

    TestEqual(*FString::Printf(
        TEXT("object-path form %s must return the same dependency count as package form %s"),
        *ObjectPath, *PackagePath),
        ObjDeps.Num(), PkgDeps.Num());
    TestTrue(TEXT("object-path and package-path dependency lists must match element-for-element"),
        ObjDeps == PkgDeps);
    return true;
}

// ============================================================================
// asset.get_dependencies_classified — same equivalence for the classified verb
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetDependenciesClassifiedObjectPathEquivalenceTest,
    "PinWright.asset.get_dependencies_classified.ObjectPathEquivalence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGetDependenciesClassifiedObjectPathEquivalenceTest::RunTest(const FString& Parameters)
{
    FString PackagePath, ObjectPath;
    if (!FindAssetWithHardDependency(PackagePath, ObjectPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("No /Game asset with a hard package dependency found in the asset registry; "
                 "classified object-path/package-path dependency equivalence could not be exercised."));
        return true;
    }

    TSharedPtr<FJsonObject> PkgPayload = MakeShared<FJsonObject>();
    PkgPayload->SetStringField(TEXT("assetPath"), PackagePath);
    FTestResponseCapture PkgCapture;
    TestTrue(TEXT("asset.get_dependencies_classified handler found (package form)"),
        InvokeHandlerWithCapture(TEXT("asset.get_dependencies_classified"), PkgPayload, PkgCapture));
    const TArray<FString> PkgDeps = ExtractDependencies(PkgCapture.Result);
    TestTrue(TEXT("classified package-name form returns at least one dependency"), PkgDeps.Num() > 0);

    TSharedPtr<FJsonObject> ObjPayload = MakeShared<FJsonObject>();
    ObjPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    FTestResponseCapture ObjCapture;
    TestTrue(TEXT("asset.get_dependencies_classified handler found (object-path form)"),
        InvokeHandlerWithCapture(TEXT("asset.get_dependencies_classified"), ObjPayload, ObjCapture));
    const TArray<FString> ObjDeps = ExtractDependencies(ObjCapture.Result);

    TestEqual(*FString::Printf(
        TEXT("classified object-path form %s must return the same dependency count as package form %s"),
        *ObjectPath, *PackagePath),
        ObjDeps.Num(), PkgDeps.Num());
    TestTrue(TEXT("classified object-path and package-path dependency lists must match element-for-element"),
        ObjDeps == PkgDeps);
    return true;
}

// ============================================================================
// asset.get_dependencies_classified — recursive=true must return the transitive
// closure, not just direct deps (regression for the dropped-recursion defect
// where `recursive` was echoed but never applied).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetDependenciesClassifiedRecursiveTransitiveTest,
    "PinWright.asset.get_dependencies_classified.RecursiveReturnsTransitive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetGetDependenciesClassifiedRecursiveTransitiveTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    if (!FindAssetWithTransitiveDependency(PackagePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("No /Game asset with a two-level hard dependency chain found in the asset "
                 "registry; recursive transitive traversal could not be exercised."));
        return true;
    }

    // recursive=false — direct deps only (baseline).
    TSharedPtr<FJsonObject> DirectPayload = MakeShared<FJsonObject>();
    DirectPayload->SetStringField(TEXT("assetPath"), PackagePath);
    DirectPayload->SetBoolField(TEXT("recursive"), false);
    FTestResponseCapture DirectCapture;
    TestTrue(TEXT("asset.get_dependencies_classified handler found (recursive=false)"),
        InvokeHandlerWithCapture(TEXT("asset.get_dependencies_classified"), DirectPayload, DirectCapture));
    const TArray<FString> DirectDeps = ExtractDependencies(DirectCapture.Result);

    // recursive=true — must include the transitive closure (a strict superset).
    TSharedPtr<FJsonObject> RecursivePayload = MakeShared<FJsonObject>();
    RecursivePayload->SetStringField(TEXT("assetPath"), PackagePath);
    RecursivePayload->SetBoolField(TEXT("recursive"), true);
    FTestResponseCapture RecursiveCapture;
    TestTrue(TEXT("asset.get_dependencies_classified handler found (recursive=true)"),
        InvokeHandlerWithCapture(TEXT("asset.get_dependencies_classified"), RecursivePayload, RecursiveCapture));
    const TArray<FString> RecursiveDeps = ExtractDependencies(RecursiveCapture.Result);

    TestTrue(*FString::Printf(
        TEXT("recursive dependency count (%d) must exceed direct count (%d) for %s"),
        RecursiveDeps.Num(), DirectDeps.Num(), *PackagePath),
        RecursiveDeps.Num() > DirectDeps.Num());

    // Every direct dep must still be present in the recursive result.
    bool bSuperset = true;
    for (const FString& Dep : DirectDeps)
    {
        if (!RecursiveDeps.Contains(Dep))
        {
            bSuperset = false;
            break;
        }
    }
    TestTrue(TEXT("recursive dependency list is a superset of the direct list"), bSuperset);
    return true;
}
