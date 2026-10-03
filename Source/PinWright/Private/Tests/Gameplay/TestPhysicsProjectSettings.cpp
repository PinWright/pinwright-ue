// Copyright (c) 2026 Alexander Penkin. MIT License.

// physics.get_project_settings / set_project_settings / set_surface_types (board
// F-physics-project-settings). Every test restores the UPhysicsSettings CDO, the EPhysicalSurface
// display metadata and — for the two that reach the config writer — DefaultEngine.ini's bytes and
// GConfig, so the host's committed ini is never left modified.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ConfigContext.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Tests/TestUtils.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace TestPhysicsProjectSettingsHelpers
{
    // Indices the tests write. High enough that a host table is unlikely to use them; the restore
    // guard puts back whatever the host had either way.
    constexpr int32 FixtureIndexA = 61;
    constexpr int32 FixtureIndexB = 62;

    // Snapshot/restore of the surface table and of EPhysicalSurface's per-entry metadata.
    struct FScopedSurfaceTableRestore
    {
        TArray<FPhysicalSurfaceName> Table;
        TArray<bool> bHidden;
        TArray<bool> bHasDisplayName;
        TArray<FString> DisplayName;

        FScopedSurfaceTableRestore()
        {
            Table = GetDefault<UPhysicsSettings>()->PhysicalSurfaces;
            const UEnum* Enum = StaticEnum<EPhysicalSurface>();
            for (int32 i = 0; i < SurfaceType_Max; ++i)
            {
                bHidden.Add(Enum->HasMetaData(TEXT("Hidden"), i));
                bHasDisplayName.Add(Enum->HasMetaData(TEXT("DisplayName"), i));
                DisplayName.Add(Enum->GetMetaData(TEXT("DisplayName"), i));
            }
        }

        ~FScopedSurfaceTableRestore()
        {
            GetMutableDefault<UPhysicsSettings>()->PhysicalSurfaces = Table;
            UEnum* Enum = StaticEnum<EPhysicalSurface>();
            for (int32 i = 0; i < SurfaceType_Max; ++i)
            {
                if (bHidden[i]) { Enum->SetMetaData(TEXT("Hidden"), TEXT(""), i); }
                else { Enum->RemoveMetaData(TEXT("Hidden"), i); }
                if (bHasDisplayName[i]) { Enum->SetMetaData(TEXT("DisplayName"), *DisplayName[i], i); }
                else { Enum->RemoveMetaData(TEXT("DisplayName"), i); }
            }
        }
    };

    // Byte-exact snapshot of DefaultEngine.ini plus its read-only flag. On exit the bytes go back
    // and GConfig's Engine branch is reloaded from disk the way TryUpdateDefaultConfigFile itself
    // does, so neither the file nor the in-memory config carries the test's surface names.
    struct FScopedDefaultEngineIniRestore
    {
        FString Path;
        TArray<uint8> Bytes;
        bool bExisted = false;
        bool bWasReadOnly = false;
        // False when the file exists but could not be read: callers must not touch the file then,
        // and the destructor never writes, so a failed read can never truncate the host's ini.
        bool bSnapshotOk = false;

        explicit FScopedDefaultEngineIniRestore(const FString& InPath) : Path(InPath)
        {
            IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
            bExisted = PF.FileExists(*Path);
            bWasReadOnly = bExisted && PF.IsReadOnly(*Path);
            bSnapshotOk = !bExisted || FFileHelper::LoadFileToArray(Bytes, *Path);
        }

        ~FScopedDefaultEngineIniRestore()
        {
            if (!bSnapshotOk)
            {
                return;
            }
            IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
            TArray<uint8> Current;
            const bool bExistsNow = PF.FileExists(*Path);
            if (bExistsNow == bExisted && (!bExistsNow || (FFileHelper::LoadFileToArray(Current, *Path) && Current == Bytes)))
            {
                // Nothing reached the disk, so GConfig was never reloaded with test values either.
                PF.SetReadOnly(*Path, bWasReadOnly);
                return;
            }
            PF.SetReadOnly(*Path, false);
            if (bExisted)
            {
                FFileHelper::SaveArrayToFile(Bytes, *Path);
                PF.SetReadOnly(*Path, bWasReadOnly);
            }
            else
            {
                PF.DeleteFile(*Path);
            }
            // Same order as UObject::UpdateSingleSectionOfConfigFile: flush pending writes, then reload.
            GConfig->Flush(false, GEngineIni);
            FConfigContext Context = FConfigContext::ForceReloadIntoGConfig();
            Context.bWriteDestIni = false;
            Context.Load(TEXT("Engine"));
        }
    };

    TSharedPtr<FJsonObject> SurfaceEntry(int32 Index, const TCHAR* Name)
    {
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetNumberField(TEXT("index"), Index);
        E->SetStringField(TEXT("name"), Name);
        return E;
    }

    TSharedPtr<FJsonObject> SurfacePayload(const TArray<TSharedPtr<FJsonObject>>& Entries, bool bReplace, bool bSave)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const TSharedPtr<FJsonObject>& E : Entries)
        {
            Arr.Add(MakeShared<FJsonValueObject>(E));
        }
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetArrayField(TEXT("surfaces"), Arr);
        P->SetBoolField(TEXT("replace"), bReplace);
        P->SetBoolField(TEXT("save"), bSave);
        return P;
    }

    // The current effective table (last entry per index wins, as in LoadSurfaceType) as request
    // entries for replace:true payloads. A host ini may repeat an index; replace:true takes one
    // entry per index.
    TArray<TSharedPtr<FJsonObject>> CurrentEntries()
    {
        TMap<int32, FName> Effective;
        for (const FPhysicalSurfaceName& E : GetDefault<UPhysicsSettings>()->PhysicalSurfaces)
        {
            Effective.Add(static_cast<int32>(E.Type.GetValue()), E.Name);
        }
        Effective.KeySort(TLess<int32>());
        TArray<TSharedPtr<FJsonObject>> Out;
        for (const TPair<int32, FName>& Pair : Effective)
        {
            Out.Add(SurfaceEntry(Pair.Key, *Pair.Value.ToString()));
        }
        return Out;
    }

    int32 CountAt(int32 Index)
    {
        int32 Count = 0;
        for (const FPhysicalSurfaceName& E : GetDefault<UPhysicsSettings>()->PhysicalSurfaces)
        {
            Count += static_cast<int32>(E.Type.GetValue()) == Index ? 1 : 0;
        }
        return Count;
    }

    // Two indices (A > B) no physical material asset uses, so the in-use test's fixture is the only
    // user of A and dropping B is a genuine unused-index control on any host.
    bool PickIndicesNoMaterialUses(int32& OutA, int32& OutB)
    {
        TArray<FAssetData> Assets;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
            .GetAssetsByClass(UPhysicalMaterial::StaticClass()->GetClassPathName(), Assets, true);
        for (const FAssetData& Asset : Assets)
        {
            Asset.GetAsset();
        }
        TSet<int32> Used;
        for (TObjectIterator<UPhysicalMaterial> It; It; ++It)
        {
            if (It->IsAsset())
            {
                Used.Add(static_cast<int32>(It->SurfaceType.GetValue()));
            }
        }
        TArray<int32> Free;
        for (int32 i = SurfaceType_Max - 1; i >= 1 && Free.Num() < 2; --i)
        {
            if (!Used.Contains(i)) Free.Add(i);
        }
        if (Free.Num() < 2) return false;
        OutA = Free[0];
        OutB = Free[1];
        return true;
    }

    // The effective name of an index: the last entry wins, as in LoadSurfaceType.
    FName NameAt(int32 Index)
    {
        FName Name = NAME_None;
        for (const FPhysicalSurfaceName& E : GetDefault<UPhysicsSettings>()->PhysicalSurfaces)
        {
            if (static_cast<int32>(E.Type.GetValue()) == Index)
            {
                Name = E.Name;
            }
        }
        return Name;
    }
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsGetProjectSettingsReturnsLiveSurfaceTableTest,
    "PinWright.physics.get_project_settings.ReturnsLiveSurfaceTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsGetProjectSettingsReturnsLiveSurfaceTableTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    FScopedSurfaceTableRestore Restore;
    GetMutableDefault<UPhysicsSettings>()->PhysicalSurfaces.Add(
        FPhysicalSurfaceName(static_cast<EPhysicalSurface>(FixtureIndexA), TEXT("PwGetFixture")));

    FTestResponseCapture Capture;
    TestTrue(TEXT("dispatched"), InvokeHandlerWithCapture(TEXT("physics.get_project_settings"), MakeShared<FJsonObject>(), Capture));
    if (!TestTrue(TEXT("success"), Capture.bSuccess) || !Capture.Result.IsValid()) return false;

    const TSharedPtr<FJsonObject>* Settings = nullptr;
    if (!TestTrue(TEXT("settings object"), Capture.Result->TryGetObjectField(TEXT("settings"), Settings))) return false;
    TestTrue(TEXT("settings carries PhysicalSurfaces"), (*Settings)->HasField(TEXT("PhysicalSurfaces")));
    TestTrue(TEXT("settings carries bSuppressFaceRemapTable"), (*Settings)->HasField(TEXT("bSuppressFaceRemapTable")));
    TestEqual(TEXT("configSection names the ini section"),
        Capture.Result->GetStringField(TEXT("configSection")), FString(TEXT("/Script/Engine.PhysicsSettings")));
    TestTrue(TEXT("configFile is DefaultEngine.ini"),
        Capture.Result->GetStringField(TEXT("configFile")).EndsWith(TEXT("DefaultEngine.ini")));

    const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
    if (!TestTrue(TEXT("surfaceTypes array"), Capture.Result->TryGetArrayField(TEXT("surfaceTypes"), Types))) return false;
    bool bFound = false;
    for (const TSharedPtr<FJsonValue>& V : *Types)
    {
        const TSharedPtr<FJsonObject> O = V->AsObject();
        bFound |= O.IsValid() && O->GetIntegerField(TEXT("index")) == FixtureIndexA
            && O->GetStringField(TEXT("name")) == TEXT("PwGetFixture");
    }
    TestTrue(TEXT("surfaceTypes reports the live in-memory entry {61, PwGetFixture}"), bFound);
    return true;
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetProjectSettingsAppliesAndRefusesTest,
    "PinWright.physics.set_project_settings.AppliesInMemoryAndRefusesSurfaceTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsSetProjectSettingsAppliesAndRefusesTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    FScopedSurfaceTableRestore Restore;
    UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
    const bool bOriginal = S->bSuppressFaceRemapTable;
    ON_SCOPE_EXIT { GetMutableDefault<UPhysicsSettings>()->bSuppressFaceRemapTable = bOriginal; };
    const int32 TableBefore = S->PhysicalSurfaces.Num();

    TSharedPtr<FJsonObject> Updates = MakeShared<FJsonObject>();
    Updates->SetBoolField(TEXT("bSuppressFaceRemapTable"), !bOriginal);
    Updates->SetArrayField(TEXT("PhysicalSurfaces"), {});
    Updates->SetNumberField(TEXT("NoSuchPhysicsSetting"), 1);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("updates"), Updates);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("dispatched"), InvokeHandlerWithCapture(TEXT("physics.set_project_settings"), Payload, Capture));
    if (!TestTrue(TEXT("success"), Capture.bSuccess) || !Capture.Result.IsValid()) return false;

    TestTrue(TEXT("bool applied"), JsonStringArrayContains(Capture.Result, TEXT("applied"), TEXT("bSuppressFaceRemapTable")));
    TestEqual(TEXT("CDO holds the new value"), S->bSuppressFaceRemapTable, !bOriginal);

    const TSharedPtr<FJsonObject> Surf = JsonArrayFindObjectByStringField(Capture.Result, TEXT("rejected"), TEXT("name"), TEXT("PhysicalSurfaces"));
    if (TestTrue(TEXT("PhysicalSurfaces rejected"), Surf.IsValid()))
    {
        TestEqual(TEXT("routed to the typed verb"), Surf->GetStringField(TEXT("reason")), FString(TEXT("use_physics.set_surface_types")));
    }
    TestEqual(TEXT("surface table untouched"), S->PhysicalSurfaces.Num(), TableBefore);
    const TSharedPtr<FJsonObject> Unknown = JsonArrayFindObjectByStringField(Capture.Result, TEXT("rejected"), TEXT("name"), TEXT("NoSuchPhysicsSetting"));
    if (TestTrue(TEXT("unknown property rejected"), Unknown.IsValid()))
    {
        TestEqual(TEXT("unknown reason"), Unknown->GetStringField(TEXT("reason")), FString(TEXT("unknown_property")));
    }

    TestFalse(TEXT("save:false is not saved"), Capture.Result->GetBoolField(TEXT("saved")));
    TestEqual(TEXT("saveState notRequested"), Capture.Result->GetStringField(TEXT("saveState")), FString(TEXT("notRequested")));
    TestFalse(TEXT("no savedTo claim without a save"), Capture.Result->HasField(TEXT("savedTo")));
    TestTrue(TEXT("target section still reported"), Capture.Result->HasField(TEXT("configSection")));
    return true;
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetSurfaceTypesRegistersTest,
    "PinWright.physics.set_surface_types.RegistersAndRefreshesEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsSetSurfaceTypesRegistersTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    FScopedSurfaceTableRestore Restore;
    // A hand-edited ini can list several entries for one index. Seed such entries on every host,
    // interleaved: the listed index A twice (must collapse to one entry at its first old position)
    // and an unlisted index twice (must be kept verbatim, in order).
    constexpr int32 DuplicatedIndex = FixtureIndexA - 1;
    TArray<FPhysicalSurfaceName>& Seeded = GetMutableDefault<UPhysicsSettings>()->PhysicalSurfaces;
    Seeded.Add(FPhysicalSurfaceName(static_cast<EPhysicalSurface>(FixtureIndexA), TEXT("PwOldA1")));
    Seeded.Add(FPhysicalSurfaceName(static_cast<EPhysicalSurface>(DuplicatedIndex), TEXT("PwDupFirst")));
    Seeded.Add(FPhysicalSurfaceName(static_cast<EPhysicalSurface>(FixtureIndexA), TEXT("PwOldA2")));
    Seeded.Add(FPhysicalSurfaceName(static_cast<EPhysicalSurface>(DuplicatedIndex), TEXT("PwDupLast")));
    const TArray<FPhysicalSurfaceName> BeforeTable = Seeded;
    const int32 TableBefore = BeforeTable.Num();
    auto IsAOrB = [](const FPhysicalSurfaceName& E)
    {
        const int32 I = static_cast<int32>(E.Type.GetValue());
        return I == FixtureIndexA || I == FixtureIndexB;
    };
    // Where A's first old entry lands: after every earlier entry that survives (all of them except
    // repeats of B, the other listed index).
    int32 ExpectedAPos = 0;
    bool bSeenB = false;
    for (const FPhysicalSurfaceName& E : BeforeTable)
    {
        const int32 I = static_cast<int32>(E.Type.GetValue());
        if (I == FixtureIndexA) break;
        if (I == FixtureIndexB) { if (bSeenB) continue; bSeenB = true; }
        ++ExpectedAPos;
    }
    const int32 DuplicatedBefore = CountAt(DuplicatedIndex);
    // Each listed index collapses to exactly one entry; every other entry stays.
    const int32 ExpectedAfter = TableBefore - CountAt(FixtureIndexA) - CountAt(FixtureIndexB) + 2;

    FTestResponseCapture Capture;
    TestTrue(TEXT("dispatched"), InvokeHandlerWithCapture(TEXT("physics.set_surface_types"),
        SurfacePayload({ SurfaceEntry(FixtureIndexA, TEXT("PwSurfaceA")), SurfaceEntry(FixtureIndexB, TEXT("PwSurfaceB")) }, false, false), Capture));
    if (!TestTrue(TEXT("success"), Capture.bSuccess) || !Capture.Result.IsValid()) return false;

    TestEqual(TEXT("index 61 named"), NameAt(FixtureIndexA), FName(TEXT("PwSurfaceA")));
    TestEqual(TEXT("index 62 named"), NameAt(FixtureIndexB), FName(TEXT("PwSurfaceB")));
    TestEqual(TEXT("upsert kept the existing entries"), GetDefault<UPhysicsSettings>()->PhysicalSurfaces.Num(), ExpectedAfter);
    TestEqual(TEXT("upsert kept both entries of an untouched repeated index"), CountAt(DuplicatedIndex), DuplicatedBefore);
    TestEqual(TEXT("untouched repeated index keeps its effective name"), NameAt(DuplicatedIndex), FName(TEXT("PwDupLast")));
    const TArray<FPhysicalSurfaceName>& After = GetDefault<UPhysicsSettings>()->PhysicalSurfaces;
    TestEqual(TEXT("listed repeated index collapsed to one entry"), CountAt(FixtureIndexA), 1);
    const int32 APos = After.IndexOfByPredicate([](const FPhysicalSurfaceName& E) { return static_cast<int32>(E.Type.GetValue()) == FixtureIndexA; });
    TestEqual(TEXT("listed index's entry sits at its first old position"), APos, ExpectedAPos);
    TArray<FPhysicalSurfaceName> OthersBefore = BeforeTable.FilterByPredicate([&](const FPhysicalSurfaceName& E) { return !IsAOrB(E); });
    TArray<FPhysicalSurfaceName> OthersAfter = After.FilterByPredicate([&](const FPhysicalSurfaceName& E) { return !IsAOrB(E); });
    bool bSameSequence = OthersBefore.Num() == OthersAfter.Num();
    for (int32 i = 0; bSameSequence && i < OthersBefore.Num(); ++i)
    {
        bSameSequence = OthersBefore[i].Type == OthersAfter[i].Type && OthersBefore[i].Name == OthersAfter[i].Name;
    }
    TestTrue(TEXT("entries of unlisted indices keep their sequence"), bSameSequence);
    TestTrue(TEXT("enumRefreshed"), Capture.Result->GetBoolField(TEXT("enumRefreshed")));

    // Independent of the handler's own verdict: the enum metadata the editor dropdowns read.
    const UEnum* Enum = StaticEnum<EPhysicalSurface>();
    TestEqual(TEXT("DisplayName of 61"), Enum->GetMetaData(TEXT("DisplayName"), FixtureIndexA), FString(TEXT("PwSurfaceA")));
    TestFalse(TEXT("61 no longer hidden"), Enum->HasMetaData(TEXT("Hidden"), FixtureIndexA));

    const TArray<TSharedPtr<FJsonValue>>* Previous = nullptr;
    if (TestTrue(TEXT("previous[] present"), Capture.Result->TryGetArrayField(TEXT("previous"), Previous)))
    {
        TestEqual(TEXT("previous is the pre-call table"), Previous->Num(), TableBefore);
    }
    TestFalse(TEXT("save:false is not saved"), Capture.Result->GetBoolField(TEXT("saved")));
    TestFalse(TEXT("no savedTo without a save"), Capture.Result->HasField(TEXT("savedTo")));
    return true;
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetSurfaceTypesRejectsInvalidTest,
    "PinWright.physics.set_surface_types.RejectsInvalidEntries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsSetSurfaceTypesRejectsInvalidTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    FScopedSurfaceTableRestore Restore;
    const TArray<FPhysicalSurfaceName> Before = GetDefault<UPhysicsSettings>()->PhysicalSurfaces;

    struct FCase { const TCHAR* Label; TArray<TSharedPtr<FJsonObject>> Entries; const TCHAR* Code; };
    TSharedPtr<FJsonObject> Fractional = SurfaceEntry(FixtureIndexA, TEXT("PwFrac"));
    Fractional->SetNumberField(TEXT("index"), 1.5);
    const TArray<FCase> Cases = {
        { TEXT("index 0 (SurfaceType_Default)"), { SurfaceEntry(0, TEXT("PwZero")) }, TEXT("INVALID_ARGUMENT") },
        { TEXT("index 63 (SurfaceType_Max)"), { SurfaceEntry(63, TEXT("PwMax")) }, TEXT("INVALID_ARGUMENT") },
        { TEXT("fractional index"), { Fractional }, TEXT("INVALID_ARGUMENT") },
        { TEXT("empty name"), { SurfaceEntry(FixtureIndexA, TEXT("  ")) }, TEXT("INVALID_ARGUMENT") },
        { TEXT("same index twice"), { SurfaceEntry(FixtureIndexA, TEXT("PwX")), SurfaceEntry(FixtureIndexA, TEXT("PwY")) }, TEXT("INVALID_ARGUMENT") },
        { TEXT("same name on two indices (case-insensitive)"), { SurfaceEntry(FixtureIndexA, TEXT("PwDup")), SurfaceEntry(FixtureIndexB, TEXT("pwdup")) }, TEXT("DUPLICATE_NAME") },
    };
    for (const FCase& Case : Cases)
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("physics.set_surface_types"), SurfacePayload(Case.Entries, false, false), Capture);
        TestFalse(*FString::Printf(TEXT("%s refused"), Case.Label), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s code"), Case.Label), Capture.ErrorCode, FString(Case.Code));
        TestEqual(*FString::Printf(TEXT("%s left the table alone"), Case.Label),
            GetDefault<UPhysicsSettings>()->PhysicalSurfaces.Num(), Before.Num());
    }
    return true;
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetSurfaceTypesRefusesDroppingUsedIndexTest,
    "PinWright.physics.set_surface_types.RefusesDroppingIndexInUse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsSetSurfaceTypesRefusesDroppingUsedIndexTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    FScopedSurfaceTableRestore Restore;
    int32 IndexA = 0;
    int32 IndexB = 0;
    if (!TestTrue(TEXT("two surface indices no physical material uses"), PickIndicesNoMaterialUses(IndexA, IndexB))) return false;

    // Two registered fixture indices; a physical material uses A, nothing uses B.
    {
        FTestResponseCapture Setup;
        InvokeHandlerWithCapture(TEXT("physics.set_surface_types"),
            SurfacePayload({ SurfaceEntry(IndexA, TEXT("PwInUse")), SurfaceEntry(IndexB, TEXT("PwUnused")) }, false, false), Setup);
        if (!TestTrue(TEXT("fixture indices registered"), Setup.bSuccess)) return false;
    }

    const FString PackagePath = TEXT("/Game/PinWrightTests/PhysicsSettings/PM_PwSurfaceInUse");
    UPackage* Package = CreatePackage(*PackagePath);
    UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(Package, TEXT("PM_PwSurfaceInUse"), RF_Public | RF_Standalone);
    Material->SurfaceType = static_cast<EPhysicalSurface>(IndexA);
    FAssetRegistryModule::AssetCreated(Material);
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };
    if (!TestTrue(TEXT("fixture material is an asset using index A"),
        Material->IsAsset() && Material->SurfaceType == static_cast<EPhysicalSurface>(IndexA))) return false;

    // replace:true without A -> refused, naming the material; nothing changes.
    TArray<TSharedPtr<FJsonObject>> WithoutA;
    for (const TSharedPtr<FJsonObject>& E : CurrentEntries())
    {
        if (E->GetIntegerField(TEXT("index")) != IndexA) WithoutA.Add(E);
    }
    FTestResponseCapture Refused;
    InvokeHandlerWithCapture(TEXT("physics.set_surface_types"), SurfacePayload(WithoutA, true, false), Refused);
    TestFalse(TEXT("dropping a used index is refused"), Refused.bSuccess);
    TestEqual(TEXT("code"), Refused.ErrorCode, FString(TEXT("SURFACE_TYPE_IN_USE")));
    TestEqual(TEXT("index A still registered"), NameAt(IndexA), FName(TEXT("PwInUse")));
    TestEqual(TEXT("index B still registered (refusal is all-or-nothing)"), NameAt(IndexB), FName(TEXT("PwUnused")));
    bool bNamed = false;
    const TArray<TSharedPtr<FJsonValue>>* InUse = nullptr;
    if (Refused.Result.IsValid() && Refused.Result->TryGetArrayField(TEXT("inUse"), InUse))
    {
        for (const TSharedPtr<FJsonValue>& Row : *InUse)
        {
            const TSharedPtr<FJsonObject> O = Row->AsObject();
            bNamed |= O.IsValid() && O->GetIntegerField(TEXT("index")) == IndexA
                && JsonStringArrayContains(O, TEXT("physicalMaterials"), Material->GetPathName());
        }
    }
    TestTrue(TEXT("error data inUse[] names the material under index A"), bNamed);

    // Control: dropping only B (unused) succeeds and re-hides it.
    TArray<TSharedPtr<FJsonObject>> WithoutB;
    for (const TSharedPtr<FJsonObject>& E : CurrentEntries())
    {
        if (E->GetIntegerField(TEXT("index")) != IndexB) WithoutB.Add(E);
    }
    FTestResponseCapture Dropped;
    InvokeHandlerWithCapture(TEXT("physics.set_surface_types"), SurfacePayload(WithoutB, true, false), Dropped);
    if (!TestTrue(TEXT("dropping an unused index succeeds"), Dropped.bSuccess)) return false;
    TestEqual(TEXT("index B removed"), NameAt(IndexB), FName(NAME_None));
    const TArray<TSharedPtr<FJsonValue>>* Removed = nullptr;
    TestTrue(TEXT("removed[] is exactly [B]"), Dropped.Result->TryGetArrayField(TEXT("removed"), Removed)
        && Removed->Num() == 1 && static_cast<int32>((*Removed)[0]->AsNumber()) == IndexB);
    TestTrue(TEXT("B hidden again"), StaticEnum<EPhysicalSurface>()->HasMetaData(TEXT("Hidden"), IndexB));
    TestTrue(TEXT("enumRefreshed"), Dropped.Result->GetBoolField(TEXT("enumRefreshed")));
    return true;
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetSurfaceTypesPersistsTest,
    "PinWright.physics.set_surface_types.PersistsAndReloadsFromConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsSetSurfaceTypesPersistsTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    const FString ConfigFile = GetDefault<UPhysicsSettings>()->GetDefaultConfigFilename();
    // Declared before the table guard so the ini/GConfig restore runs after the CDO restore. It
    // also puts back the read-only flag, so a source-control-locked ini is unlocked only for the test.
    FScopedDefaultEngineIniRestore IniRestore(ConfigFile);
    if (!TestTrue(TEXT("DefaultEngine.ini snapshot taken before the write"), IniRestore.bSnapshotOk)) return false;
    FScopedSurfaceTableRestore Restore;
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ConfigFile, false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("dispatched"), InvokeHandlerWithCapture(TEXT("physics.set_surface_types"),
        SurfacePayload({ SurfaceEntry(FixtureIndexA, TEXT("PwPersistedSurface")) }, false, true), Capture));
    if (!TestTrue(TEXT("success"), Capture.bSuccess) || !Capture.Result.IsValid()) return false;

    TestTrue(TEXT("saved"), Capture.Result->GetBoolField(TEXT("saved")));
    TestEqual(TEXT("saveState written"), Capture.Result->GetStringField(TEXT("saveState")), FString(TEXT("written")));
    TestEqual(TEXT("savedTo is the default config file"), Capture.Result->GetStringField(TEXT("savedTo")), ConfigFile);
    TestEqual(TEXT("configSection"), Capture.Result->GetStringField(TEXT("configSection")), FString(TEXT("/Script/Engine.PhysicsSettings")));
    TestTrue(TEXT("PhysicalSurfaces reloaded from config"),
        JsonStringArrayContains(Capture.Result, TEXT("reloadVerified"), TEXT("PhysicalSurfaces")));

    // Independent of the handler: the bytes on disk carry the entry.
    FString Text;
    TestTrue(TEXT("ini readable"), FFileHelper::LoadFileToString(Text, *ConfigFile));
    TestTrue(TEXT("ini contains the registered name"), Text.Contains(TEXT("PwPersistedSurface")));
    return true;
}

// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsSetSurfaceTypesReadOnlyConfigTest,
    "PinWright.physics.set_surface_types.ReportsConfigSaveFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhysicsSetSurfaceTypesReadOnlyConfigTest::RunTest(const FString& Parameters)
{
    using namespace TestPhysicsProjectSettingsHelpers;
    const FString ConfigFile = GetDefault<UPhysicsSettings>()->GetDefaultConfigFilename();
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!TestTrue(TEXT("DefaultEngine.ini exists for the refusal test"), PF.FileExists(*ConfigFile))) return false;
    FScopedDefaultEngineIniRestore IniRestore(ConfigFile);
    if (!TestTrue(TEXT("DefaultEngine.ini snapshot taken before the write"), IniRestore.bSnapshotOk)) return false;
    FScopedSurfaceTableRestore Restore;
    if (!TestTrue(TEXT("DefaultEngine.ini made read-only"), PF.SetReadOnly(*ConfigFile, true))) return false;

    AddExpectedError(TEXT("is read-only and cannot be written to"), EAutomationExpectedErrorFlags::Contains, 1);
    FTestResponseCapture Capture;
    TestTrue(TEXT("dispatched"), InvokeHandlerWithCapture(TEXT("physics.set_surface_types"),
        SurfacePayload({ SurfaceEntry(FixtureIndexA, TEXT("PwReadOnlySurface")) }, false, true), Capture));
    TestFalse(TEXT("read-only config is not a success"), Capture.bSuccess);
    TestEqual(TEXT("typed error"), Capture.ErrorCode, FString(TEXT("SAVE_FAILED")));
    if (!TestTrue(TEXT("measured result attached"), Capture.Result.IsValid())) return false;
    TestTrue(TEXT("save was requested"), Capture.Result->GetBoolField(TEXT("saveRequested")));
    TestFalse(TEXT("not saved"), Capture.Result->GetBoolField(TEXT("saved")));
    TestEqual(TEXT("saveState failed"), Capture.Result->GetStringField(TEXT("saveState")), FString(TEXT("failed")));
    TestFalse(TEXT("no savedTo on failure"), Capture.Result->HasField(TEXT("savedTo")));
    TestEqual(TEXT("still applied in memory"), NameAt(FixtureIndexA), FName(TEXT("PwReadOnlySurface")));
    return true;
}
