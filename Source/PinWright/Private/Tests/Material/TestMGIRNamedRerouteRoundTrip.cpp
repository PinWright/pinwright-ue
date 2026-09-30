// Copyright (c) 2026 Alexander Penkin. MIT License.

// Named reroutes through decompile -> compile -> decompile -> compile.
// Counterfactuals: without FMGIRRerouteDeclarationSpec::DisplayName the declaration is renamed
// to its GUID handle (WiredDeclarationKeepsName); without the head-equals-result declaration
// discriminator an unwired declaration is compiled as a usage and fails MGIR_REROUTE_NOT_FOUND
// (UnwiredDeclarationRoundTrips); without the usage -> declaration edge in
// ComputeExpressionDepth the usage decompiles ahead of its wired declaration and fails
// MGIR_REROUTE_NOT_FOUND (UsageBindsToItsDeclaration); a usage's DisplayName used to be read
// as the lookup key and fail MGIR_REROUTE_NOT_FOUND instead of MGIR_BAD_PROPERTY
// (UsageRejectsDisplayName).
#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"

namespace MGIRNamedRerouteTest
{
const FName WiredName(TEXT("Roughness"));
const FName UnwiredName(TEXT("Unwired Slot"));

template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
    T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
    Expression->MaterialExpressionEditorX = X;
    Expression->MaterialExpressionEditorY = Y;
    Material->GetEditorOnlyData()->ExpressionCollection.AddExpression(Expression);
    return Expression;
}

// Source graph built with engine objects, so the first decompile reads authentic data.
UMaterial* CreateSourceMaterial(const IrTest::FScratchAsset& Scratch, bool bWired, bool bUnwired, bool bUsage)
{
    UPackage* Package = CreatePackage(*Scratch.PackagePath);
    UMaterial* Material = Package
        ? NewObject<UMaterial>(Package, FName(*Scratch.AssetName), RF_Public | RF_Standalone)
        : nullptr;
    if (!Material || !Material->GetEditorOnlyData())
    {
        return nullptr;
    }
    FAssetRegistryModule::AssetCreated(Material);

    UMaterialExpressionNamedRerouteDeclaration* Wired = nullptr;
    if (bWired)
    {
        UMaterialExpressionConstant* Constant = AddExpression<UMaterialExpressionConstant>(Material, -600, 0);
        Constant->R = 0.25f;
        Wired = AddExpression<UMaterialExpressionNamedRerouteDeclaration>(Material, -300, 0);
        Wired->Name = WiredName;
        Wired->Input.Connect(0, Constant);
    }
    if (bUnwired)
    {
        UMaterialExpressionNamedRerouteDeclaration* Unwired =
            AddExpression<UMaterialExpressionNamedRerouteDeclaration>(Material, -300, 200);
        Unwired->Name = UnwiredName;
    }
    if (bUsage && Wired)
    {
        UMaterialExpressionNamedRerouteUsage* Usage =
            AddExpression<UMaterialExpressionNamedRerouteUsage>(Material, 0, 0);
        Usage->Declaration = Wired;
        Usage->DeclarationGuid = Wired->VariableGuid;
    }
    return Material;
}

// Decompiles From, recompiles the text into To (Append, unsaved) and returns To's material.
UMaterial* Hop(FAutomationTestBase& Test, UMaterial* From, const IrTest::FScratchAsset& FromScratch,
    const IrTest::FScratchAsset& ToScratch)
{
    const FMGIRDecompileResult Decompiled = FMGIRDecompiler::DecompileMaterial(From);
    Test.TestTrue(TEXT("decompile succeeds"), Decompiled.bSuccess);
    if (!Decompiled.bSuccess)
    {
        return nullptr;
    }

    FMGIRCompileOptions Options;
    Options.bRunLayout = false;
    Options.bSave = false;
    const FMGIRCompileResult Compiled = FMGIRCompiler::Compile(
        IrTest::ReplaceScratchAssetName(Decompiled.MGIRText, FromScratch, ToScratch), Options);
    Test.TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')\n%s"),
        *Compiled.ErrorCode, *Compiled.ErrorMessage, *Decompiled.MGIRText), Compiled.bSuccess);
    return Compiled.bSuccess ? LoadObject<UMaterial>(nullptr, *ToScratch.PackagePath) : nullptr;
}

// Source -> first compile -> second compile. Owns the scratch assets for the test's lifetime.
struct FRoundTrip
{
    IrTest::FScratchAsset SourceScratch;
    IrTest::FScratchAsset FirstScratch;
    IrTest::FScratchAsset SecondScratch;

    explicit FRoundTrip(const TCHAR* Tag)
        : SourceScratch(*FString::Printf(TEXT("M_MGIRReroute%sSource"), Tag))
        , FirstScratch(*FString::Printf(TEXT("M_MGIRReroute%sFirst"), Tag))
        , SecondScratch(*FString::Printf(TEXT("M_MGIRReroute%sSecond"), Tag))
    {
    }

    UMaterial* Run(FAutomationTestBase& Test, bool bWired, bool bUnwired, bool bUsage)
    {
        UMaterial* Source = CreateSourceMaterial(SourceScratch, bWired, bUnwired, bUsage);
        Test.TestNotNull(TEXT("source material created"), Source);
        UMaterial* First = Source ? Hop(Test, Source, SourceScratch, FirstScratch) : nullptr;
        return First ? Hop(Test, First, FirstScratch, SecondScratch) : nullptr;
    }
};

template <typename T>
TArray<T*> FindAll(UMaterial* Material)
{
    TArray<T*> Found;
    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (T* Typed = Cast<T>(Expression))
        {
            Found.Add(Typed);
        }
    }
    return Found;
}
} // namespace MGIRNamedRerouteTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRNamedRerouteWiredDeclarationKeepsNameTest,
    "PinWright.material.mgir.NamedReroute.WiredDeclarationKeepsName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRNamedRerouteWiredDeclarationKeepsNameTest::RunTest(const FString& Parameters)
{
    using namespace MGIRNamedRerouteTest;

    FRoundTrip RoundTrip(TEXT("Wired"));
    UMaterial* Result = RoundTrip.Run(*this, true, false, false);
    if (!Result)
    {
        return false;
    }

    const TArray<UMaterialExpressionNamedRerouteDeclaration*> Declarations =
        FindAll<UMaterialExpressionNamedRerouteDeclaration>(Result);
    TestEqual(TEXT("one declaration survives"), Declarations.Num(), 1);
    if (Declarations.Num() != 1)
    {
        return false;
    }

    TestEqual(TEXT("author-given name survives two round trips"), Declarations[0]->Name, WiredName);
    const UMaterialExpressionConstant* Source = Cast<UMaterialExpressionConstant>(Declarations[0]->Input.Expression);
    TestNotNull(TEXT("declaration stays wired to its constant"), Source);
    if (Source)
    {
        TestEqual(TEXT("wired constant value survives"), Source->R, 0.25f);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRNamedRerouteUnwiredDeclarationRoundTripsTest,
    "PinWright.material.mgir.NamedReroute.UnwiredDeclarationRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRNamedRerouteUnwiredDeclarationRoundTripsTest::RunTest(const FString& Parameters)
{
    using namespace MGIRNamedRerouteTest;

    FRoundTrip RoundTrip(TEXT("Unwired"));
    UMaterial* Result = RoundTrip.Run(*this, false, true, false);
    if (!Result)
    {
        return false;
    }

    const TArray<UMaterialExpressionNamedRerouteDeclaration*> Declarations =
        FindAll<UMaterialExpressionNamedRerouteDeclaration>(Result);
    TestEqual(TEXT("unwired declaration compiles as a declaration"), Declarations.Num(), 1);
    TestEqual(TEXT("no usage is invented"),
        FindAll<UMaterialExpressionNamedRerouteUsage>(Result).Num(), 0);
    if (Declarations.Num() != 1)
    {
        return false;
    }

    TestEqual(TEXT("unwired declaration keeps its name"), Declarations[0]->Name, UnwiredName);
    TestTrue(TEXT("unwired declaration stays unwired"), Declarations[0]->Input.Expression == nullptr);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRNamedRerouteUsageBindsToItsDeclarationTest,
    "PinWright.material.mgir.NamedReroute.UsageBindsToItsDeclaration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRNamedRerouteUsageBindsToItsDeclarationTest::RunTest(const FString& Parameters)
{
    using namespace MGIRNamedRerouteTest;

    FRoundTrip RoundTrip(TEXT("Usage"));
    UMaterial* Result = RoundTrip.Run(*this, true, true, true);
    if (!Result)
    {
        return false;
    }

    const TArray<UMaterialExpressionNamedRerouteUsage*> Usages =
        FindAll<UMaterialExpressionNamedRerouteUsage>(Result);
    TestEqual(TEXT("one usage survives"), Usages.Num(), 1);
    const TArray<UMaterialExpressionNamedRerouteDeclaration*> Declarations =
        FindAll<UMaterialExpressionNamedRerouteDeclaration>(Result);
    TestEqual(TEXT("both declarations survive"), Declarations.Num(), 2);
    if (Usages.Num() != 1 || !Usages[0]->Declaration)
    {
        AddError(TEXT("usage is not bound to a declaration"));
        return false;
    }

    const UMaterialExpressionNamedRerouteDeclaration* Declaration = Usages[0]->Declaration;
    TestEqual(TEXT("usage binds the declaration it named, by name"), Declaration->Name, WiredName);
    TestTrue(TEXT("bound declaration lives in the compiled material"),
        Declarations.Contains(Declaration));
    TestEqual(TEXT("usage guid matches its declaration"), Usages[0]->DeclarationGuid, Declaration->VariableGuid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRNamedRerouteUsageRejectsDisplayNameTest,
    "PinWright.material.mgir.NamedReroute.UsageRejectsDisplayName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRNamedRerouteUsageRejectsDisplayNameTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRRerouteUsageDisplayName"));
    const FString Text = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%c = constant Float1(1) @(0, 0)\n")
        TEXT("    %%r = reroute r (Source: %%c, DisplayName: \"Roughness\") @(200, 0)\n")
        TEXT("    %%u = reroute r (DisplayName: \"Other\") @(400, 0)\n")
        TEXT("}\n"),
        *Scratch.PackagePath);

    FMGIRCompileOptions Options;
    Options.bRunLayout = false;
    Options.bSave = false;
    const FMGIRCompileResult Result = FMGIRCompiler::Compile(Text, Options);
    TestFalse(TEXT("a usage carrying DisplayName is refused"), Result.bSuccess);
    TestEqual(TEXT("refusal code"), Result.ErrorCode, FString(TEXT("MGIR_BAD_PROPERTY")));
    TestTrue(TEXT("refusal names the usage"), Result.ErrorMessage.Contains(TEXT("%u")));
    return true;
}
