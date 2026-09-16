// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the shared asset-path resolver the four read-only
// dependency/reference verbs now go through (ResolveAssetPathToPackage,
// Utils/AssetUtils.h). Two distinct defects, one resolver:
//
// 1. An UNRESOLVABLE path was a silent empty answer, not an error.
//    asset.get_dependencies / asset.get_dependencies_classified normalised the
//    caller string with FPackageName::ObjectPathToPackageName - a pure string
//    operation that returns its input unchanged when there is no '.' - and then
//    asked the registry for that node's dependencies. A node that does not exist
//    has no dependencies, so the verbs answered {"dependencies":[]} with
//    isError:false. That is indistinguishable from a genuine "nothing depends on
//    this", and it sits on the delete path: a caller checking before removing an
//    asset reads the empty list as permission to proceed.
//
// 2. The SHORT package-path form was accepted for some assets and rejected for
//    others. asset.references / asset.dependencies looked up
//    GetAssetByObjectPath(FSoftObjectPath(AssetPath)), which needs a full object
//    path: FSoftObjectPath::SetPath parses "/Game/Foo/Bar" as
//    {PackageName=/Game/Foo/Bar, AssetName=None} ("No delimiter, package name
//    only", SoftObjectPath.cpp) and no FAssetRegistryState row is keyed on a None
//    asset name. The only reason it ever worked is the FindObject fast path at
//    the top of UAssetRegistryImpl::GetAssetByObjectPath: for an already-LOADED
//    package that finds the UPackage itself and FAssetData(UPackage) comes back
//    valid (PackageName and AssetName both set, so IsValid() passes). Load state
//    is not something a caller controls, hence "works for some assets".
//    asset.exists took the same string happily because
//    UEditorAssetSubsystem::DoesAssetExist runs it through
//    EditorScriptingHelpers::ConvertAnyPathToObjectPath first, which infers the
//    missing object name from the package short name.
//
// The equivalence test below therefore insists on an UNLOADED package. A loaded
// one hides defect 2 completely, which is exactly how it survived.
//
// Content-agnostic: both live-registry tests warn-and-pass on a registry that
// offers no suitable asset, matching TestAssetDependenciesObjectPath.cpp.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace PwAssetPathResolutionTest
{
    // A path that is a syntactically valid long package name under a mounted
    // root but names nothing. Syntax validation alone cannot reject it, which is
    // the point: only an existence probe can.
    static const TCHAR* UnresolvablePath =
        TEXT("/Game/PwNoSuchFolderZZZQQ/PwNoSuchAssetZZZQQ");

    // Assert the honest-failure contract for one verb and one bad path: an error
    // with the registered ASSET_NOT_FOUND code, and explicitly NOT a success
    // carrying an empty dependency list.
    inline void TestUnresolvablePathIsError(FAutomationTestBase& Test, const FString& Method)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), UnresolvablePath);

        FTestResponseCapture Capture;
        Test.TestTrue(*FString::Printf(TEXT("%s handler found"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("%s responded"), *Method), Capture.bWasCalled);

        // The load-bearing pair. bSuccess false is what the old code got wrong;
        // the code assertion is what stops a future change from erroring with an
        // unregistered or unrelated spelling.
        Test.TestFalse(*FString::Printf(
            TEXT("%s must NOT report success for a path that resolves to nothing"), *Method),
            Capture.bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("%s reports the registered ASSET_NOT_FOUND code"), *Method),
            Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));

        // The specific shape the defect produced: isError:false plus an empty
        // dependencies array. Asserted separately so a regression that keeps the
        // error code but re-adds the empty payload is still caught.
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Deps = nullptr;
            Test.AddError(FString::Printf(
                TEXT("%s answered a success for '%s'%s"), *Method, UnresolvablePath,
                (Capture.Result->TryGetArrayField(TEXT("dependencies"), Deps) && Deps && Deps->Num() == 0)
                    ? TEXT(" with an empty dependencies array - the exact silent-empty defect")
                    : TEXT("")));
        }
    }

    // Find a /Game registry row whose PACKAGE is NOT currently loaded, and return
    // both spellings of its path. Prefers a package that has at least one
    // dependency so the equivalence comparison has content to compare, but any
    // unloaded row will do - the assertion is that the two spellings agree, and
    // agreeing on an empty list is still agreement.
    inline bool FindUnloadedGameAsset(FString& OutPackagePath, FString& OutObjectPath)
    {
        IAssetRegistry& AssetRegistry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAssets(Filter, AssetDataList);

        FString FallbackPackage, FallbackObject;
        for (const FAssetData& Data : AssetDataList)
        {
            const FString PackagePath = Data.PackageName.ToString();
            // FindPackage does no disk I/O; it only answers "is this package
            // resident right now".
            if (FindPackage(nullptr, *PackagePath) != nullptr)
            {
                continue;
            }
            const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
                *PackagePath, *Data.AssetName.ToString());

            TArray<FName> Deps;
            AssetRegistry.GetDependencies(Data.PackageName, Deps,
                UE::AssetRegistry::EDependencyCategory::Package,
                UE::AssetRegistry::EDependencyQuery::Hard);
            if (Deps.Num() > 0)
            {
                OutPackagePath = PackagePath;
                OutObjectPath = ObjectPath;
                return true;
            }
            if (FallbackPackage.IsEmpty())
            {
                FallbackPackage = PackagePath;
                FallbackObject = ObjectPath;
            }
        }

        if (!FallbackPackage.IsEmpty())
        {
            OutPackagePath = FallbackPackage;
            OutObjectPath = FallbackObject;
            return true;
        }
        return false;
    }

    // Read the count field a reference-direction verb publishes, or -1.
    inline int32 ReadCount(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        int32 Value = -1;
        if (Result.IsValid())
        {
            Result->TryGetNumberField(Field, Value);
        }
        return Value;
    }

    inline FString ReadString(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        FString Value;
        if (Result.IsValid())
        {
            Result->TryGetStringField(Field, Value);
        }
        return Value;
    }
}

// ============================================================================
// asset.get_dependencies - an unresolvable path is an ERROR, never an empty list
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetDependenciesUnresolvablePathTest,
    "PinWright.asset.get_dependencies.UnresolvablePathIsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetGetDependenciesUnresolvablePathTest::RunTest(const FString& Parameters)
{
    PwAssetPathResolutionTest::TestUnresolvablePathIsError(*this, TEXT("asset.get_dependencies"));
    return true;
}

// ============================================================================
// asset.get_dependencies_classified - same contract for the classified sibling,
// which shared the same normalise-without-checking code.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetGetDependenciesClassifiedUnresolvablePathTest,
    "PinWright.asset.get_dependencies_classified.UnresolvablePathIsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetGetDependenciesClassifiedUnresolvablePathTest::RunTest(const FString& Parameters)
{
    PwAssetPathResolutionTest::TestUnresolvablePathIsError(*this,
        TEXT("asset.get_dependencies_classified"));
    return true;
}

// ============================================================================
// asset.references / asset.dependencies already errored on an unresolvable path;
// these lock the CODE in so routing them through the shared resolver did not
// change the verdict a caller sees.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReferencesUnresolvablePathTest,
    "PinWright.asset.references.UnresolvablePathIsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetReferencesUnresolvablePathTest::RunTest(const FString& Parameters)
{
    PwAssetPathResolutionTest::TestUnresolvablePathIsError(*this, TEXT("asset.references"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDependenciesUnresolvablePathTest,
    "PinWright.asset.dependencies.UnresolvablePathIsError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDependenciesUnresolvablePathTest::RunTest(const FString& Parameters)
{
    PwAssetPathResolutionTest::TestUnresolvablePathIsError(*this, TEXT("asset.dependencies"));
    return true;
}

// ============================================================================
// The short "/Game/Foo/Bar" form must resolve for asset.references and
// asset.dependencies exactly as it does for asset.exists - on an UNLOADED
// package, which is the only state in which the defect is observable.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReferenceShortPathFormTest,
    "PinWright.asset.references.ShortPackagePathFormResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetReferenceShortPathFormTest::RunTest(const FString& Parameters)
{
    FString PackagePath, ObjectPath;
    if (!PwAssetPathResolutionTest::FindUnloadedGameAsset(PackagePath, ObjectPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("No unloaded /Game asset found in the asset registry; the short-form / "
                 "long-form equivalence of asset.references and asset.dependencies could "
                 "not be exercised (a LOADED package resolves either way and proves "
                 "nothing about this defect)."));
        return true;
    }

    // Calibration: asset.exists accepts the short string. This is the asymmetry
    // that was reported - the same spelling one verb took and another refused.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.exists handler found"),
            InvokeHandlerWithCapture(TEXT("asset.exists"), Payload, Capture));
        bool bExists = false;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(TEXT("exists"), bExists);
        }
        TestTrue(*FString::Printf(TEXT("asset.exists accepts the short form %s"), *PackagePath),
            bExists);
    }

    const TCHAR* Verbs[] = { TEXT("asset.references"), TEXT("asset.dependencies") };
    const TCHAR* CountFields[] = { TEXT("dependencyCount"), TEXT("referencerCount") };

    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Verbs); ++Index)
    {
        const FString Verb = Verbs[Index];

        // Short package-path form - the one that used to be refused whenever the
        // package was not resident.
        TSharedPtr<FJsonObject> ShortPayload = MakeShared<FJsonObject>();
        ShortPayload->SetStringField(TEXT("assetPath"), PackagePath);
        FTestResponseCapture ShortCapture;
        TestTrue(*FString::Printf(TEXT("%s handler found (short form)"), *Verb),
            InvokeHandlerWithCapture(Verb, ShortPayload, ShortCapture));
        TestTrue(*FString::Printf(
            TEXT("%s accepts the short package-path form %s that asset.exists accepts"),
            *Verb, *PackagePath), ShortCapture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s short form emits no error code"), *Verb),
            ShortCapture.ErrorCode, FString());

        // Full object-path form - the spelling that always worked.
        TSharedPtr<FJsonObject> LongPayload = MakeShared<FJsonObject>();
        LongPayload->SetStringField(TEXT("assetPath"), ObjectPath);
        FTestResponseCapture LongCapture;
        TestTrue(*FString::Printf(TEXT("%s handler found (object-path form)"), *Verb),
            InvokeHandlerWithCapture(Verb, LongPayload, LongCapture));
        TestTrue(*FString::Printf(TEXT("%s accepts the object-path form %s"), *Verb, *ObjectPath),
            LongCapture.bSuccess);

        // Both spellings name the same package, so both must answer identically.
        TestEqual(*FString::Printf(TEXT("%s resolves both spellings to the same package"), *Verb),
            PwAssetPathResolutionTest::ReadString(ShortCapture.Result, TEXT("packageName")),
            PwAssetPathResolutionTest::ReadString(LongCapture.Result, TEXT("packageName")));
        TestEqual(*FString::Printf(TEXT("%s returns the same %s for both spellings"),
            *Verb, CountFields[Index]),
            PwAssetPathResolutionTest::ReadCount(ShortCapture.Result, CountFields[Index]),
            PwAssetPathResolutionTest::ReadCount(LongCapture.Result, CountFields[Index]));
    }

    return true;
}
