// Copyright (c) 2026 Alexander Penkin. MIT License.

// A material function call links to the function's pins by GUID and by nothing else, so a pin that
// changes (or never had) its GUID silently disconnects every already-saved caller on the caller's
// next load. This exercises the three production guarantees that prevent it, on real packages:
//   1. add_function_input gives a new INPUT a persistent Id (add_function_output already did the
//      output half), so a saved caller's wire into that input re-links after a disk round trip.
//   2. material.compile_mgir Append rebuilds a function graph WITHOUT renumbering its pins.
//   3. get_material_info reports a call node whose cached pin GUID no longer names a pin of the
//      function, which is the only warning a caller gets before the wire disappears.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Handlers/Material/MaterialFinders.h"
#include "IrCore/IrTextUtils.h"
#include "MGIR/MGIRCompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace PinWrightMaterialFunctionPinIdentity
{
    bool Invoke(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& OutCapture)
    {
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, OutCapture);
        Test.TestTrue(*FString::Printf(TEXT("%s registered"), Method), bFound);
        Test.TestTrue(*FString::Printf(TEXT("%s delivered a response"), Method), OutCapture.bWasCalled);
        Test.TestTrue(*FString::Printf(TEXT("%s succeeded: %s %s"),
            Method, *OutCapture.ErrorCode, *OutCapture.Message), OutCapture.bSuccess);
        return bFound && OutCapture.bWasCalled && OutCapture.bSuccess;
    }

    bool InvokeForNodeId(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload, FString& OutNodeId)
    {
        FTestResponseCapture Capture;
        if (!Invoke(Test, Method, Payload, Capture))
        {
            return false;
        }
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("nodeId"), OutNodeId);
        }
        return Test.TestFalse(TEXT("Creation returned a nodeId"), OutNodeId.IsEmpty());
    }

    TSharedPtr<FJsonObject> NodePayload(const FString& AssetPath, double X, double Y)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetNumberField(TEXT("x"), X);
        Payload->SetNumberField(TEXT("y"), Y);
        return Payload;
    }

    bool Save(FAutomationTestBase& Test, UObject* Asset)
    {
        const FString Filename = PackageFilenameFromAssetPath(Asset->GetOutermost()->GetName());
        if (!Test.TestTrue(TEXT("Package has a disk filename"), !Filename.IsEmpty()))
        {
            return false;
        }
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args);
        Test.TestTrue(TEXT("Fixture package saved"), bSaved);
        const bool bHasBytes = IFileManager::Get().FileSize(*Filename) > 0;
        Test.TestTrue(TEXT("Saved fixture has bytes on disk"), bHasBytes);
        return bSaved && bHasBytes;
    }

    UMaterialExpressionFunctionInput* FindInputByName(UMaterialFunction* Function, const FName Name)
    {
        for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
        {
            UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression);
            if (Input && Input->InputName == Name)
            {
                return Input;
            }
        }
        return nullptr;
    }

    UMaterialExpressionFunctionOutput* FindOutputByName(UMaterialFunction* Function, const FName Name)
    {
        for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
        {
            UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression);
            if (Output && Output->OutputName == Name)
            {
                return Output;
            }
        }
        return nullptr;
    }

    /** The `functionCallIdentity.unstable[]` reasons get_material_info reports for a material. */
    void ReadIdentityReasons(FAutomationTestBase& Test, const FString& MaterialObjectPath,
        TArray<FString>& OutReasons, TArray<FString>& OutPinKinds)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialObjectPath);
        FTestResponseCapture Capture;
        if (!Invoke(Test, TEXT("material.authoring.get_material_info"), Payload, Capture)
            || !Capture.Result.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Capture.Result->TryGetObjectField(TEXT("functionCallIdentity"), Block) || !Block)
        {
            return;
        }
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!(*Block)->TryGetArrayField(TEXT("unstable"), Entries) || !Entries)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Entries)
        {
            const TSharedPtr<FJsonObject> EntryObj = Entry.IsValid() ? Entry->AsObject() : nullptr;
            if (!EntryObj.IsValid())
            {
                continue;
            }
            OutReasons.Add(EntryObj->GetStringField(TEXT("reason")));
            OutPinKinds.Add(EntryObj->GetStringField(TEXT("pinKind")));
        }
    }

    void RunFixture(FAutomationTestBase& Test)
    {
        const FString FixtureGuid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Folder = TEXT("/Game/PinWrightTests/") + FixtureGuid;
        const FString FunctionPackagePath = Folder + TEXT("/MF_PinIdentity");
        const FString MaterialPackagePath = Folder + TEXT("/M_PinIdentityConsumer");
        const FString FunctionObjectPath = FunctionPackagePath + TEXT(".MF_PinIdentity");
        const FString MaterialObjectPath = MaterialPackagePath + TEXT(".M_PinIdentityConsumer");
        TStrongObjectPtr<UMaterialFunction> Function;
        TStrongObjectPtr<UMaterial> Material;
        ON_SCOPE_EXIT
        {
            Material.Reset();
            Function.Reset();
            CleanupTestAsset(MaterialPackagePath);
            CleanupTestAsset(FunctionPackagePath);

            // Remove only this GUID directory, and only if empty. Never recurse into the shared root.
            FString FixtureFilename;
            FString RootFilename;
            if (FPackageName::TryConvertLongPackageNameToFilename(MaterialPackagePath, FixtureFilename)
                && FPackageName::TryConvertLongPackageNameToFilename(
                    FString(TEXT("/Game/PinWrightTests")), RootFilename))
            {
                const FString FixtureDirectory = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FixtureFilename));
                const FString TestRoot = FPaths::ConvertRelativePathToFull(RootFilename);
                if (FPaths::IsSamePath(FPaths::GetPath(FixtureDirectory), TestRoot)
                    && FPaths::GetCleanFilename(FixtureDirectory) == FixtureGuid)
                {
                    IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, false);
                }
            }
        };

        UPackage* FunctionPackage = CreatePackage(*FunctionPackagePath);
        UPackage* MaterialPackage = CreatePackage(*MaterialPackagePath);
        if (!Test.TestNotNull(TEXT("Function package created"), FunctionPackage)
            || !Test.TestNotNull(TEXT("Material package created"), MaterialPackage))
        {
            return;
        }
        Function.Reset(NewObject<UMaterialFunction>(FunctionPackage, TEXT("MF_PinIdentity"), RF_Public | RF_Standalone));
        Material.Reset(NewObject<UMaterial>(MaterialPackage, TEXT("M_PinIdentityConsumer"), RF_Public | RF_Standalone));
        if (!Test.TestNotNull(TEXT("Function created"), Function.Get())
            || !Test.TestNotNull(TEXT("Material created"), Material.Get()))
        {
            return;
        }
        FAssetRegistryModule::AssetCreated(Function.Get());
        FAssetRegistryModule::AssetCreated(Material.Get());

        // --- The function: one input, one output, input wired straight through. ---
        TSharedPtr<FJsonObject> Payload = NodePayload(FunctionObjectPath, -300, 0);
        Payload->SetStringField(TEXT("inputName"), TEXT("Scale"));
        Payload->SetStringField(TEXT("inputType"), TEXT("Vector3"));
        FString InputNodeId;
        if (!InvokeForNodeId(Test, TEXT("material.authoring.add_function_input"), Payload, InputNodeId))
        {
            return;
        }
        UMaterialExpressionFunctionInput* Input =
            Cast<UMaterialExpressionFunctionInput>(FindExpressionByIdOrName(Function.Get(), InputNodeId));
        if (!Test.TestNotNull(TEXT("Input is in the owning function"), Input))
        {
            return;
        }
        // Direct reversion detector: nothing in this setup initialises the Id itself.
        Test.TestTrue(TEXT("Production-created function input immediately has a persistent ID"),
            Input->Id.IsValid());
        const FGuid AuthoredInputId = Input->Id;

        Payload = NodePayload(FunctionObjectPath, 200, 0);
        Payload->SetStringField(TEXT("inputName"), TEXT("Result"));
        FString OutputNodeId;
        if (!InvokeForNodeId(Test, TEXT("material.authoring.add_function_output"), Payload, OutputNodeId))
        {
            return;
        }
        UMaterialExpressionFunctionOutput* Output =
            Cast<UMaterialExpressionFunctionOutput>(FindExpressionByIdOrName(Function.Get(), OutputNodeId));
        if (!Test.TestNotNull(TEXT("Output is in the owning function"), Output))
        {
            return;
        }
        Test.TestTrue(TEXT("Production-created function output immediately has a persistent ID"),
            Output->Id.IsValid());
        const FGuid AuthoredOutputId = Output->Id;
        Output->A.Connect(0, Input);

        // --- The consumer: a call node wired on BOTH pins, which is the shape that regressed. ---
        Payload = NodePayload(MaterialObjectPath, 0, 0);
        Payload->SetStringField(TEXT("functionPath"), FunctionObjectPath);
        FString CallNodeId;
        if (!InvokeForNodeId(Test, TEXT("material.authoring.use_material_function"), Payload, CallNodeId))
        {
            return;
        }
        UMaterialExpressionMaterialFunctionCall* Call =
            Cast<UMaterialExpressionMaterialFunctionCall>(FindExpressionByIdOrName(Material.Get(), CallNodeId));
        if (!Test.TestNotNull(TEXT("Call is in the consumer"), Call)
            || !Test.TestEqual(TEXT("Call exposes one input"), Call->FunctionInputs.Num(), 1)
            || !Test.TestEqual(TEXT("Call exposes one output"), Call->FunctionOutputs.Num(), 1))
        {
            return;
        }
        Test.TestEqual(TEXT("Caller cached the persistent input ID"),
            Call->FunctionInputs[0].ExpressionInputId, AuthoredInputId);
        Test.TestEqual(TEXT("Caller cached the persistent output ID"),
            Call->FunctionOutputs[0].ExpressionOutputId, AuthoredOutputId);

        UMaterialExpressionConstant3Vector* Feed =
            NewObject<UMaterialExpressionConstant3Vector>(Material.Get());
        Feed->Material = Material.Get();
        Feed->Constant = FLinearColor(0.2f, 0.4f, 0.6f);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Feed);
        const FString FeedNodeId = Feed->MaterialExpressionGuid.ToString();

        Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), FeedNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), CallNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("Scale"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 0);
        FTestResponseCapture Capture;
        if (!Invoke(Test, TEXT("material.authoring.connect_nodes"), Payload, Capture))
        {
            return;
        }

        Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), CallNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), TEXT("Main"));
        Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 0);
        Capture.Reset();
        if (!Invoke(Test, TEXT("material.graph.connect_nodes"), Payload, Capture))
        {
            return;
        }
        Test.TestTrue(TEXT("Call input is wired before the round trip"),
            Call->FunctionInputs[0].Input.Expression == Feed);
        Test.TestTrue(TEXT("BaseColor reads the call before the round trip"),
            Material->GetEditorOnlyData()->BaseColor.Expression == Call);

        // --- Disk round trip: dependency first, then consumer, then genuine eviction. ---
        if (!Save(Test, Function.Get()) || !Save(Test, Material.Get()))
        {
            return;
        }
        const TArray<TWeakObjectPtr<UObject>> FormerObjects =
            { Function.Get(), Material.Get(), Input, Output, Call, Feed };
        const TArray<FString> FormerPaths = { FunctionObjectPath, MaterialObjectPath,
            Input->GetPathName(), Output->GetPathName(), Call->GetPathName(), Feed->GetPathName() };
        TArray<UPackage*> Packages = { Material->GetOutermost(), Function->GetOutermost() };
        Material.Reset();
        Function.Reset();
        // Native unload drains compilation/rendering before ResetLoaders and GC. ResetLoaders alone
        // would leave live objects behind and turn LoadObject into a resident-memory lookup.
        FText UnloadError;
        bool bEvicted = UPackageTools::UnloadPackages(Packages, UnloadError, false);
        Test.TestTrue(*FString::Printf(TEXT("Both fixture packages unloaded: %s"), *UnloadError.ToString()), bEvicted);
        Packages.Reset();
        for (int32 Index = 0; Index < FormerObjects.Num(); ++Index)
        {
            bEvicted &= Test.TestFalse(TEXT("Former object is no longer resident"), FormerObjects[Index].IsValid());
            bEvicted &= Test.TestNull(TEXT("Former path cannot resolve before disk load"),
                FindObject<UObject>(nullptr, *FormerPaths[Index]));
        }
        if (!bEvicted)
        {
            return;
        }

        Material.Reset(LoadObject<UMaterial>(nullptr, *MaterialObjectPath));
        if (!Test.TestNotNull(TEXT("Consumer reloaded from disk"), Material.Get()))
        {
            return;
        }
        Function.Reset(FindObject<UMaterialFunction>(nullptr, *FunctionObjectPath));
        if (!Test.TestNotNull(TEXT("Consumer load loaded its function dependency"), Function.Get()))
        {
            return;
        }
        // Reacquire by saved expression GUIDs; never read any pre-unload raw pointer again.
        Call = Cast<UMaterialExpressionMaterialFunctionCall>(FindExpressionByIdOrName(Material.Get(), CallNodeId));
        Feed = Cast<UMaterialExpressionConstant3Vector>(FindExpressionByIdOrName(Material.Get(), FeedNodeId));
        Input = FindInputByName(Function.Get(), TEXT("Scale"));
        Output = FindOutputByName(Function.Get(), TEXT("Result"));
        if (!Test.TestNotNull(TEXT("Reloaded call found in material"), Call)
            || !Test.TestNotNull(TEXT("Reloaded feed found in material"), Feed)
            || !Test.TestNotNull(TEXT("Reloaded input found in function"), Input)
            || !Test.TestNotNull(TEXT("Reloaded output found in function"), Output)
            || !Test.TestEqual(TEXT("Reloaded call exposes one input"), Call->FunctionInputs.Num(), 1)
            || !Test.TestEqual(TEXT("Reloaded call exposes one output"), Call->FunctionOutputs.Num(), 1))
        {
            return;
        }

        Test.TestEqual(TEXT("Function input ID survives the disk round trip"), Input->Id, AuthoredInputId);
        Test.TestEqual(TEXT("Function output ID survives the disk round trip"), Output->Id, AuthoredOutputId);
        Test.TestEqual(TEXT("Reloaded caller retains the input ID"),
            Call->FunctionInputs[0].ExpressionInputId, AuthoredInputId);
        Test.TestEqual(TEXT("Reloaded caller retains the output ID"),
            Call->FunctionOutputs[0].ExpressionOutputId, AuthoredOutputId);
        // The pin the ticket lost. Without a persistent input Id, PostLoad mints a new one,
        // FindInputById matches nothing, and UpdateFromFunctionResource drops this connection.
        Test.TestTrue(TEXT("Reloaded call input is still wired to its feed"),
            Call->FunctionInputs[0].Input.Expression == Feed);
        Test.TestTrue(TEXT("Reloaded BaseColor still reads the call"),
            Material->GetEditorOnlyData()->BaseColor.Expression == Call);
        Test.TestEqual(TEXT("Reloaded BaseColor keeps output zero"),
            Material->GetEditorOnlyData()->BaseColor.OutputIndex, 0);

        TArray<FString> Reasons;
        TArray<FString> PinKinds;
        ReadIdentityReasons(Test, MaterialObjectPath, Reasons, PinKinds);
        Test.TestEqual(TEXT("An intact caller reports no unstable pins"), Reasons.Num(), 0);

        // --- Append rebuild of the function must not renumber its pins. ---
        FMGIRCompileOptions Options;
        Options.Mode = EMGIRCompileMode::Append;
        Options.bRunLayout = false;
        Options.bSave = false;
        const FString Document = FString::Printf(
            TEXT("entry function %s {\n")
            TEXT("    %%scale = call %s(InputName: \"Scale\", InputType: \"Float3\")\n")
            TEXT("    output Result: %%scale\n")
            TEXT("}"),
            *FIrTextUtils::FormatNameToken(FunctionObjectPath),
            *FIrTextUtils::FormatNameToken(UMaterialExpressionFunctionInput::StaticClass()->GetPathName()));
        const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(Document, Options);
        if (!Test.TestTrue(*FString::Printf(TEXT("Append recompile of the function succeeds: %s %s"),
                *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess))
        {
            return;
        }
        UMaterialExpressionFunctionInput* RebuiltInput = FindInputByName(Function.Get(), TEXT("Scale"));
        UMaterialExpressionFunctionOutput* RebuiltOutput = FindOutputByName(Function.Get(), TEXT("Result"));
        if (Test.TestNotNull(TEXT("Rebuilt input exists"), RebuiltInput)
            && Test.TestNotNull(TEXT("Rebuilt output exists"), RebuiltOutput))
        {
            Test.TestTrue(TEXT("Append actually replaced the pin objects"), RebuiltInput != Input);
            Test.TestEqual(TEXT("Rebuilt input keeps its previous persistent ID"),
                RebuiltInput->Id, AuthoredInputId);
            Test.TestEqual(TEXT("Rebuilt output keeps its previous persistent ID"),
                RebuiltOutput->Id, AuthoredOutputId);
        }
        Test.TestTrue(TEXT("BaseColor survives the function rebuild"),
            Material->GetEditorOnlyData()->BaseColor.Expression == Call);

        // --- The detector fires on a caller whose cached GUID no longer names a pin. ---
        if (RebuiltOutput)
        {
            RebuiltOutput->ConditionallyGenerateId(/*bForce=*/true);
            Reasons.Reset();
            PinKinds.Reset();
            ReadIdentityReasons(Test, MaterialObjectPath, Reasons, PinKinds);
            Test.TestTrue(TEXT("A caller holding a GUID no pin carries is reported"),
                Reasons.Contains(TEXT("stale-persistent-id")));
            Test.TestTrue(TEXT("The report names the output pin"), PinKinds.Contains(TEXT("output")));
        }
    }

    /**
     * The asset shape the ticket was returned over: a material function whose pins were saved with
     * NO persistent GUID, by an authoring path that predates the guard. Built here by hand rather
     * than through a handler, because every production route now initialises the GUID and the whole
     * point is the state that already sits on disk in existing projects.
     *
     * The trap this closes: after such a function is loaded, PostLoad has ALREADY minted a valid
     * GUID in memory, so an in-memory "does this need repairing?" check answers no for exactly the
     * asset that needs it, and the .uasset keeps its zeros. Binding must therefore write the
     * function back unconditionally, or the next load mints a different GUID again and the caller
     * loses its wires on every single load.
     */
    void RunLegacyPoisonedFunctionFixture(FAutomationTestBase& Test)
    {
        const FString FixtureGuid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Folder = TEXT("/Game/PinWrightTests/") + FixtureGuid;
        const FString FunctionPackagePath = Folder + TEXT("/MF_Legacy");
        const FString MaterialPackagePath = Folder + TEXT("/M_LegacyConsumer");
        const FString FunctionObjectPath = FunctionPackagePath + TEXT(".MF_Legacy");
        const FString MaterialObjectPath = MaterialPackagePath + TEXT(".M_LegacyConsumer");
        TStrongObjectPtr<UMaterialFunction> Function;
        TStrongObjectPtr<UMaterial> Material;
        ON_SCOPE_EXIT
        {
            Material.Reset();
            Function.Reset();
            CleanupTestAsset(MaterialPackagePath);
            CleanupTestAsset(FunctionPackagePath);

            FString FixtureFilename;
            FString RootFilename;
            if (FPackageName::TryConvertLongPackageNameToFilename(MaterialPackagePath, FixtureFilename)
                && FPackageName::TryConvertLongPackageNameToFilename(
                    FString(TEXT("/Game/PinWrightTests")), RootFilename))
            {
                const FString FixtureDirectory = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FixtureFilename));
                const FString TestRoot = FPaths::ConvertRelativePathToFull(RootFilename);
                if (FPaths::IsSamePath(FPaths::GetPath(FixtureDirectory), TestRoot)
                    && FPaths::GetCleanFilename(FixtureDirectory) == FixtureGuid)
                {
                    IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, false);
                }
            }
        };

        UPackage* FunctionPackage = CreatePackage(*FunctionPackagePath);
        if (!Test.TestNotNull(TEXT("Legacy function package created"), FunctionPackage))
        {
            return;
        }
        Function.Reset(NewObject<UMaterialFunction>(FunctionPackage, TEXT("MF_Legacy"), RF_Public | RF_Standalone));
        if (!Test.TestNotNull(TEXT("Legacy function created"), Function.Get()))
        {
            return;
        }
        FAssetRegistryModule::AssetCreated(Function.Get());

        UMaterialExpressionFunctionInput* Input = NewObject<UMaterialExpressionFunctionInput>(Function.Get());
        Input->Function = Function.Get();
        Input->InputName = TEXT("Scale");
        Input->InputType = EFunctionInputType::FunctionInput_Vector3;
        UMaterialExpressionFunctionOutput* Output = NewObject<UMaterialExpressionFunctionOutput>(Function.Get());
        Output->Function = Function.Get();
        Output->OutputName = TEXT("Result");
        Output->A.Connect(0, Input);
        Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Input);
        Function->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Output);
        // The premise of the fixture: this is what a pre-guard authoring path wrote.
        if (!Test.TestFalse(TEXT("Hand-built input starts with no persistent ID"), Input->Id.IsValid())
            || !Test.TestFalse(TEXT("Hand-built output starts with no persistent ID"), Output->Id.IsValid()))
        {
            return;
        }
        if (!Save(Test, Function.Get()))
        {
            return;
        }

        // Evict the function so the bind below is a genuine disk load and PostLoad really runs.
        const TWeakObjectPtr<UObject> FormerFunction = Function.Get();
        TArray<UPackage*> FunctionPackages = { Function->GetOutermost() };
        Function.Reset();
        Input = nullptr;
        Output = nullptr;
        FText UnloadError;
        const bool bFunctionUnloaded = UPackageTools::UnloadPackages(FunctionPackages, UnloadError, false);
        FunctionPackages.Reset();
        if (!Test.TestTrue(*FString::Printf(TEXT("Legacy function unloaded: %s"), *UnloadError.ToString()),
                bFunctionUnloaded)
            || !Test.TestFalse(TEXT("Legacy function is no longer resident"), FormerFunction.IsValid()))
        {
            return;
        }

        UPackage* MaterialPackage = CreatePackage(*MaterialPackagePath);
        if (!Test.TestNotNull(TEXT("Legacy consumer package created"), MaterialPackage))
        {
            return;
        }
        Material.Reset(NewObject<UMaterial>(MaterialPackage, TEXT("M_LegacyConsumer"), RF_Public | RF_Standalone));
        if (!Test.TestNotNull(TEXT("Legacy consumer created"), Material.Get()))
        {
            return;
        }
        FAssetRegistryModule::AssetCreated(Material.Get());

        TSharedPtr<FJsonObject> Payload = NodePayload(MaterialObjectPath, 0, 0);
        Payload->SetStringField(TEXT("functionPath"), FunctionObjectPath);
        FTestResponseCapture Capture;
        if (!Invoke(Test, TEXT("material.authoring.use_material_function"), Payload, Capture)
            || !Capture.Result.IsValid())
        {
            return;
        }
        FString CallNodeId;
        Capture.Result->TryGetStringField(TEXT("nodeId"), CallNodeId);
        if (!Test.TestFalse(TEXT("Bind returned a nodeId"), CallNodeId.IsEmpty()))
        {
            return;
        }
        // The fix, read off the wire: binding writes the function back, so the GUIDs this node
        // just cached are on disk. Reverted, this reports nothing and the .uasset keeps its zeros.
        const TSharedPtr<FJsonObject>* IdentityBlock = nullptr;
        if (Test.TestTrue(TEXT("Bind reports what it did to the function"),
                Capture.Result->TryGetObjectField(TEXT("functionIdentity"), IdentityBlock) && IdentityBlock))
        {
            Test.TestEqual(TEXT("Bind names the function it wrote"),
                (*IdentityBlock)->GetStringField(TEXT("assetPath")), FunctionObjectPath);
            Test.TestTrue(TEXT("Bind persisted the function's pin identity"),
                (*IdentityBlock)->GetBoolField(TEXT("saved")));
        }

        Function.Reset(FindObject<UMaterialFunction>(nullptr, *FunctionObjectPath));
        UMaterialExpressionMaterialFunctionCall* Call =
            Cast<UMaterialExpressionMaterialFunctionCall>(FindExpressionByIdOrName(Material.Get(), CallNodeId));
        if (!Test.TestNotNull(TEXT("Bind loaded the legacy function"), Function.Get())
            || !Test.TestNotNull(TEXT("Call is in the legacy consumer"), Call)
            || !Test.TestEqual(TEXT("Legacy call exposes one input"), Call->FunctionInputs.Num(), 1)
            || !Test.TestEqual(TEXT("Legacy call exposes one output"), Call->FunctionOutputs.Num(), 1))
        {
            return;
        }
        const FGuid BoundInputId = Call->FunctionInputs[0].ExpressionInputId;
        const FGuid BoundOutputId = Call->FunctionOutputs[0].ExpressionOutputId;

        UMaterialExpressionConstant3Vector* Feed =
            NewObject<UMaterialExpressionConstant3Vector>(Material.Get());
        Feed->Material = Material.Get();
        Feed->Constant = FLinearColor(0.3f, 0.5f, 0.7f);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Feed);
        const FString FeedNodeId = Feed->MaterialExpressionGuid.ToString();

        Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), FeedNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), CallNodeId);
        Payload->SetStringField(TEXT("inputName"), TEXT("Scale"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 0);
        Capture.Reset();
        if (!Invoke(Test, TEXT("material.authoring.connect_nodes"), Payload, Capture))
        {
            return;
        }
        Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), MaterialObjectPath);
        Payload->SetStringField(TEXT("sourceNodeId"), CallNodeId);
        Payload->SetStringField(TEXT("targetNodeId"), TEXT("Main"));
        Payload->SetStringField(TEXT("inputName"), TEXT("BaseColor"));
        Payload->SetNumberField(TEXT("sourceOutputIndex"), 0);
        Capture.Reset();
        if (!Invoke(Test, TEXT("material.graph.connect_nodes"), Payload, Capture))
        {
            return;
        }
        if (!Save(Test, Material.Get()))
        {
            return;
        }

        const TArray<TWeakObjectPtr<UObject>> FormerObjects = { Function.Get(), Material.Get(), Call, Feed };
        TArray<UPackage*> Packages = { Material->GetOutermost(), Function->GetOutermost() };
        Material.Reset();
        Function.Reset();
        bool bEvicted = UPackageTools::UnloadPackages(Packages, UnloadError, false);
        Test.TestTrue(*FString::Printf(TEXT("Legacy fixture packages unloaded: %s"), *UnloadError.ToString()), bEvicted);
        Packages.Reset();
        for (const TWeakObjectPtr<UObject>& Former : FormerObjects)
        {
            bEvicted &= Test.TestFalse(TEXT("Former legacy object is no longer resident"), Former.IsValid());
        }
        if (!bEvicted)
        {
            return;
        }

        Material.Reset(LoadObject<UMaterial>(nullptr, *MaterialObjectPath));
        if (!Test.TestNotNull(TEXT("Legacy consumer reloaded from disk"), Material.Get()))
        {
            return;
        }
        Function.Reset(FindObject<UMaterialFunction>(nullptr, *FunctionObjectPath));
        Call = Cast<UMaterialExpressionMaterialFunctionCall>(FindExpressionByIdOrName(Material.Get(), CallNodeId));
        Feed = Cast<UMaterialExpressionConstant3Vector>(FindExpressionByIdOrName(Material.Get(), FeedNodeId));
        if (!Test.TestNotNull(TEXT("Reloaded legacy function found"), Function.Get())
            || !Test.TestNotNull(TEXT("Reloaded legacy call found"), Call)
            || !Test.TestNotNull(TEXT("Reloaded legacy feed found"), Feed)
            || !Test.TestEqual(TEXT("Reloaded legacy call exposes one input"), Call->FunctionInputs.Num(), 1)
            || !Test.TestEqual(TEXT("Reloaded legacy call exposes one output"), Call->FunctionOutputs.Num(), 1))
        {
            return;
        }
        // The GUIDs on disk are the ones the bind wrote, so they still match after a second load.
        Test.TestEqual(TEXT("Legacy input GUID is stable across the second load"),
            Call->FunctionInputs[0].ExpressionInputId, BoundInputId);
        Test.TestEqual(TEXT("Legacy output GUID is stable across the second load"),
            Call->FunctionOutputs[0].ExpressionOutputId, BoundOutputId);
        Test.TestTrue(TEXT("Legacy call input is still wired after reload"),
            Call->FunctionInputs[0].Input.Expression == Feed);
        Test.TestTrue(TEXT("Legacy BaseColor still reads the call after reload"),
            Material->GetEditorOnlyData()->BaseColor.Expression == Call);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionPinIdentitySurvivesReloadTest,
    "PinWright.material.authoring.use_material_function.FunctionPinIdentitySurvivesReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialFunctionPinIdentitySurvivesReloadTest::RunTest(const FString& Parameters)
{
    PinWrightMaterialFunctionPinIdentity::RunFixture(*this);
    PinWrightMaterialFunctionPinIdentity::RunLegacyPoisonedFunctionFixture(*this);
    return true;
}
