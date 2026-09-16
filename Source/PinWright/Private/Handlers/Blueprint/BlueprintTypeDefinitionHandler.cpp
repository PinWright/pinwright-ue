// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintTypeDefinitionHandler.cpp
// User-defined Blueprint enum/struct asset authoring handlers

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Compiler/CodePinResolver.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Handlers/Asset/UserDefinedStructDumpBuilder.h"

#include "Handlers/ErrorCodes.h"
#include "Engine/UserDefinedEnum.h"
// UserDefinedStruct.h moved from Engine/ to StructUtils/ (CoreUObject) in UE 5.8.
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h"
#elif __has_include("Engine/UserDefinedStruct.h")
#include "Engine/UserDefinedStruct.h"
#endif
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "EditorAssetLibrary.h"
#include "Utils/AssetUtils.h"
#include "Utils/PieState.h"
#if __has_include("UserDefinedStructure/UserDefinedStructEditorData.h")
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#endif
#if __has_include("Kismet2/EnumEditorUtils.h")
#include "Kismet2/EnumEditorUtils.h"
#define MCP_HAS_ENUM_EDITOR_UTILS 1
#else
#define MCP_HAS_ENUM_EDITOR_UTILS 0
#endif
#if __has_include("Kismet2/StructureEditorUtils.h")
#include "Kismet2/StructureEditorUtils.h"
#define MCP_HAS_STRUCTURE_EDITOR_UTILS 1
#else
#define MCP_HAS_STRUCTURE_EDITOR_UTILS 0
#endif

using namespace BlueprintHandlerUtils;

namespace
{
    static bool ParseAssetPath(
        const FString& InPath,
        FString& OutNormalizedPackagePath,
        FString& OutPackageFolder,
        FString& OutAssetName,
        FString& OutError)
    {
        OutError.Reset();
        OutNormalizedPackagePath.Reset();
        OutPackageFolder.Reset();
        OutAssetName.Reset();

        const FNormalizedAssetPath Normalized = NormalizeAssetPath(InPath);
        if (!Normalized.bIsValid)
        {
            OutError = Normalized.ErrorMessage.IsEmpty()
                ? FString::Printf(TEXT("Invalid asset path '%s'"), *InPath)
                : Normalized.ErrorMessage;
            return false;
        }

        OutNormalizedPackagePath = Normalized.Path;
        OutPackageFolder = FPackageName::GetLongPackagePath(OutNormalizedPackagePath);
        OutAssetName = FPackageName::GetLongPackageAssetName(OutNormalizedPackagePath);
        if (OutPackageFolder.IsEmpty() || OutAssetName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Path '%s' must include an asset name."), *OutNormalizedPackagePath);
            return false;
        }

        return true;
    }

    static FString ToObjectPath(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        if (AssetName.IsEmpty())
        {
            return PackagePath;
        }
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }

    static UObject* LoadAssetByRequestPath(const FString& Path, FString& OutNormalizedPath, FString& OutError)
    {
        FString PackageFolder;
        FString AssetName;
        if (!ParseAssetPath(Path, OutNormalizedPath, PackageFolder, AssetName, OutError))
        {
            return nullptr;
        }

        UObject* Asset = ResolveAsset(ToObjectPath(OutNormalizedPath), /*bLoadObject=*/true).Object;
        if (!Asset)
        {
            Asset = ResolveAsset(OutNormalizedPath, /*bLoadObject=*/true).Object;
        }
        if (!Asset)
        {
            OutError = FString::Printf(TEXT("Failed to load asset '%s'"), *OutNormalizedPath);
            return nullptr;
        }
        return Asset;
    }

    // Rich per-entry/per-field specs lifted from the JSON payload. Optionals stay
    // unset when the caller uses the legacy bare-string / {name,type}-only shapes
    // so we never overwrite editor state that wasn't requested.
    struct FEnumEntrySpec
    {
        FString Name;
        TOptional<FString> DisplayName;
        TOptional<FString> Tooltip;
        TOptional<bool> bHidden;
    };

    struct FStructFieldSpec
    {
        FString Name;
        FString Type;
        TOptional<FString> DefaultValue;
        TOptional<FString> Tooltip;
        TMap<FString, FString> MetaData;
        TOptional<bool> bDontEditOnInstance;
        TOptional<bool> bEnableSaveGame;
        TOptional<bool> bMultiLineText;
        TOptional<bool> bEnable3dWidget;
    };

    static bool ReadEnumEntriesFromPayload(const TSharedPtr<FJsonObject>& Payload, TArray<FEnumEntrySpec>& OutEntries, FString& OutError)
    {
        OutEntries.Reset();
        OutError.Reset();
        const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
        if (!Payload->TryGetArrayField(TEXT("entries"), EntriesArray) || !EntriesArray)
        {
            OutError = TEXT("entries array is required");
            return false;
        }

        TSet<FString> SeenNames;
        for (const TSharedPtr<FJsonValue>& Value : *EntriesArray)
        {
            if (!Value.IsValid())
            {
                OutError = TEXT("entries must contain non-empty strings or objects");
                return false;
            }

            FEnumEntrySpec Spec;

            if (Value->Type == EJson::String)
            {
                Spec.Name = Value->AsString().TrimStartAndEnd();
            }
            else if (Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Obj = Value->AsObject();
                if (!Obj.IsValid() || !Obj->TryGetStringField(TEXT("name"), Spec.Name))
                {
                    OutError = TEXT("entries objects must contain a 'name' string");
                    return false;
                }
                Spec.Name = Spec.Name.TrimStartAndEnd();

                FString DisplayName;
                if (Obj->TryGetStringField(TEXT("displayName"), DisplayName))
                {
                    Spec.DisplayName = DisplayName;
                }
                FString Tooltip;
                if (Obj->TryGetStringField(TEXT("tooltip"), Tooltip))
                {
                    Spec.Tooltip = Tooltip;
                }
                bool bHidden = false;
                if (Obj->TryGetBoolField(TEXT("hidden"), bHidden))
                {
                    Spec.bHidden = bHidden;
                }
            }
            else
            {
                OutError = TEXT("entries must contain strings or {name,...} objects");
                return false;
            }

            if (Spec.Name.IsEmpty())
            {
                OutError = TEXT("entries must contain non-empty names");
                return false;
            }
            if (SeenNames.Contains(Spec.Name))
            {
                OutError = FString::Printf(TEXT("Duplicate enum entry '%s'"), *Spec.Name);
                return false;
            }
            SeenNames.Add(Spec.Name);
            OutEntries.Add(MoveTemp(Spec));
        }

        if (OutEntries.Num() == 0)
        {
            OutError = TEXT("entries array must contain at least one entry");
            return false;
        }
        return true;
    }

    static bool ReadStructFieldSpec(const TSharedPtr<FJsonObject>& Obj, FStructFieldSpec& OutSpec, FString& OutError)
    {
        OutSpec = FStructFieldSpec();
        OutError.Reset();
        if (!Obj.IsValid())
        {
            OutError = TEXT("field entry must be an object");
            return false;
        }

        Obj->TryGetStringField(TEXT("name"), OutSpec.Name);
        Obj->TryGetStringField(TEXT("type"), OutSpec.Type);

        FString DefaultValue;
        if (Obj->TryGetStringField(TEXT("defaultValue"), DefaultValue))
        {
            OutSpec.DefaultValue = DefaultValue;
        }
        FString Tooltip;
        if (Obj->TryGetStringField(TEXT("tooltip"), Tooltip))
        {
            OutSpec.Tooltip = Tooltip;
        }

        const TSharedPtr<FJsonObject>* MetaDataObj = nullptr;
        if (Obj->TryGetObjectField(TEXT("metaData"), MetaDataObj) && MetaDataObj && (*MetaDataObj).IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>> KV : (*MetaDataObj)->Values)
            {
                if (!KV.Value.IsValid())
                {
                    continue;
                }
                FString MetaValue;
                if (KV.Value->TryGetString(MetaValue))
                {
                    OutSpec.MetaData.Add(KV.Key, MetaValue);
                }
            }
        }

        const TSharedPtr<FJsonObject>* FlagsObj = nullptr;
        if (Obj->TryGetObjectField(TEXT("flags"), FlagsObj) && FlagsObj && (*FlagsObj).IsValid())
        {
            bool bVal = false;
            if ((*FlagsObj)->TryGetBoolField(TEXT("dontEditOnInstance"), bVal))
            {
                OutSpec.bDontEditOnInstance = bVal;
            }
            if ((*FlagsObj)->TryGetBoolField(TEXT("enableSaveGame"), bVal))
            {
                OutSpec.bEnableSaveGame = bVal;
            }
            if ((*FlagsObj)->TryGetBoolField(TEXT("multiLineText"), bVal))
            {
                OutSpec.bMultiLineText = bVal;
            }
            if ((*FlagsObj)->TryGetBoolField(TEXT("enable3dWidget"), bVal))
            {
                OutSpec.bEnable3dWidget = bVal;
            }
        }

        return true;
    }

    static FString SanitizeEnumEntryName(const FString& InName)
    {
        FString Name = InName.TrimStartAndEnd();
        Name.ReplaceInline(TEXT(" "), TEXT("_"));
        return Name;
    }

    static FString StripEnumScope(const FString& InName)
    {
        FString Name = InName;
        int32 ScopeSep = INDEX_NONE;
        if (Name.FindLastChar(TEXT(':'), ScopeSep) && ScopeSep >= 0 && ScopeSep + 1 < Name.Len())
        {
            Name = Name.Mid(ScopeSep + 1);
        }
        return Name;
    }

    static TArray<FString> GetEnumEntryNames(UUserDefinedEnum* EnumAsset)
    {
        TArray<FString> Names;
        if (!EnumAsset)
        {
            return Names;
        }

        const int32 NumEnums = EnumAsset->NumEnums();
        for (int32 Index = 0; Index < NumEnums; ++Index)
        {
            FString Name = EnumAsset->GetNameStringByIndex(Index);
            if (Name.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            Name = StripEnumScope(Name);

            if (!Name.IsEmpty())
            {
                Names.Add(Name);
            }
        }
        return Names;
    }

    static bool ValidateAndSanitizeEnumEntries(
        UUserDefinedEnum* EnumAsset,
        const TArray<FEnumEntrySpec>& EntrySpecs,
        TArray<FString>& OutSanitizedNames,
        FString& OutError)
    {
        OutError.Reset();
        OutSanitizedNames.Reset();

        if (EntrySpecs.Num() == 0)
        {
            OutError = TEXT("At least one enum entry is required");
            return false;
        }

        TSet<FString> SeenNames;
        OutSanitizedNames.Reserve(EntrySpecs.Num());
        for (const FEnumEntrySpec& Spec : EntrySpecs)
        {
            const FString Sanitized = SanitizeEnumEntryName(Spec.Name);
            if (Sanitized.IsEmpty())
            {
                OutError = TEXT("Enum entries must be non-empty after sanitization");
                return false;
            }
            if (Sanitized.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
            {
                OutError = FString::Printf(TEXT("Enum entry '%s' is reserved"), *Sanitized);
                return false;
            }

            const FString DedupKey = Sanitized.ToLower();
            if (SeenNames.Contains(DedupKey))
            {
                OutError = FString::Printf(
                    TEXT("Duplicate enum entry '%s' after sanitization"),
                    *Sanitized);
                return false;
            }

#if MCP_HAS_ENUM_EDITOR_UTILS
            if (EnumAsset && !FEnumEditorUtils::IsProperNameForUserDefinedEnumerator(EnumAsset, Sanitized))
            {
                OutError = FString::Printf(
                    TEXT("Enum entry '%s' is not a valid user-defined enum enumerator name"),
                    *Sanitized);
                return false;
            }
#endif

            SeenNames.Add(DedupKey);
            OutSanitizedNames.Add(Sanitized);
        }

        return true;
    }

    static bool ApplyEnumEntries(UUserDefinedEnum* EnumAsset, const TArray<FEnumEntrySpec>& EntrySpecs, FString& OutError)
    {
        OutError.Reset();
        if (!EnumAsset)
        {
            OutError = TEXT("Enum asset is null");
            return false;
        }
        if (EntrySpecs.Num() == 0)
        {
            OutError = TEXT("At least one enum entry is required");
            return false;
        }

        TArray<FString> SanitizedEntries;
        if (!ValidateAndSanitizeEnumEntries(EnumAsset, EntrySpecs, SanitizedEntries, OutError))
        {
            return false;
        }

        TArray<TPair<FName, int64>> NewNames;
        NewNames.Reserve(SanitizedEntries.Num());
        for (int32 Index = 0; Index < SanitizedEntries.Num(); ++Index)
        {
            const FString FullEnumEntryName = EnumAsset->GenerateFullEnumName(*SanitizedEntries[Index]);
            NewNames.Emplace(FName(*FullEnumEntryName), Index);
        }

        EnumAsset->Modify();
        TArray<TPair<FName, int64>> MutableNames = NewNames;
        // UE 5.8 inserted an EUnderlyingType parameter and changed the trailing
        // bAddMaxKeyIfMissing from a bool to UEnum::EAddMaxKeyIfMissing. User-defined
        // enums are byte enums, matching the engine's own FEnumEditorUtils calls.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        if (!EnumAsset->SetEnums(
                MutableNames,
                UEnum::ECppForm::Namespaced,
                UEnum::EUnderlyingType::uint8,
                EEnumFlags::None,
                UEnum::EAddMaxKeyIfMissing::Yes))
#else
        if (!EnumAsset->SetEnums(
                MutableNames,
                UEnum::ECppForm::Namespaced,
                EEnumFlags::None,
                /*bAddMaxKeyIfMissing=*/true))
#endif
        {
            OutError = TEXT("Failed to update enum entries");
            return false;
        }

#if MCP_HAS_ENUM_EDITOR_UTILS
        for (int32 Index = 0; Index < SanitizedEntries.Num(); ++Index)
        {
            const FEnumEntrySpec& Spec = EntrySpecs[Index];
            // Display name: caller-supplied wins; otherwise fall back to the raw entry name
            // so the editor's display column stays readable instead of showing the namespaced
            // internal name. Matches prior behaviour.
            const FString DisplayName = Spec.DisplayName.IsSet()
                ? Spec.DisplayName.GetValue()
                : Spec.Name.TrimStartAndEnd();
            if (!DisplayName.IsEmpty())
            {
                FEnumEditorUtils::SetEnumeratorDisplayName(EnumAsset, Index, FText::FromString(DisplayName));
            }
            if (Spec.Tooltip.IsSet())
            {
                EnumAsset->SetMetaData(TEXT("ToolTip"), *Spec.Tooltip.GetValue(), Index);
            }
            if (Spec.bHidden.IsSet())
            {
                if (Spec.bHidden.GetValue())
                {
                    EnumAsset->SetMetaData(TEXT("Hidden"), TEXT("true"), Index);
                }
                else
                {
                    EnumAsset->RemoveMetaData(TEXT("Hidden"), Index);
                }
            }
        }
        FEnumEditorUtils::EnsureAllDisplayNamesExist(EnumAsset);
#endif

        const TArray<FString> AppliedEntries = GetEnumEntryNames(EnumAsset);
        if (AppliedEntries.Num() != SanitizedEntries.Num())
        {
            OutError = FString::Printf(
                TEXT("Enum verification failed: requested %d entries but asset has %d"),
                SanitizedEntries.Num(),
                AppliedEntries.Num());
            return false;
        }
        for (int32 Index = 0; Index < SanitizedEntries.Num(); ++Index)
        {
            if (!AppliedEntries[Index].Equals(SanitizedEntries[Index], ESearchCase::IgnoreCase))
            {
                OutError = FString::Printf(
                    TEXT("Enum verification failed at index %d: expected '%s' but found '%s'"),
                    Index,
                    *SanitizedEntries[Index],
                    *AppliedEntries[Index]);
                return false;
            }
        }

        EnumAsset->MarkPackageDirty();
        McpSafeAssetSave(EnumAsset);
        return true;
    }

    // The name says NormalizedPath, and every caller today reaches here through ParseAssetPath ->
    // NormalizeAssetPath, which returns bIsValid only for a path FPackageName::IsValidLongPackageName
    // already accepted - so the check below is a backstop rather than the reachable refusal (that
    // one is INVALID_ASSET_PATH, raised by the handlers). It is here because the invariant is
    // enforced two call frames up and nothing at this line says so: CreatePackage
    // (UObjectGlobals.cpp:1094-1096) logs at Fatal - not compiled out in any configuration - on a
    // name containing "//", so a future caller that composes a path itself and skips ParseAssetPath
    // would not get a bug here, it would end the editor PROCESS and every unsaved package in it.
    // Board: B-createpackage-unvalidated-paths-plugin-wide.
    static UPackage* CreateAssetPackage(const FString& NormalizedPath)
    {
        FText PathReason;
        if (!FPackageName::IsValidLongPackageName(NormalizedPath, /*bIncludeReadOnlyRoots=*/true, &PathReason))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("Refusing to create a package at '%s': %s"),
                *NormalizedPath, *PathReason.ToString());
            return nullptr;
        }

        UPackage* Package = CreatePackage(*NormalizedPath);
        if (Package)
        {
            Package->FullyLoad();
        }
        return Package;
    }

    static UUserDefinedStruct* CreateUserDefinedStructAsset(const FString& NormalizedPath, const FString& AssetName)
    {
#if MCP_HAS_STRUCTURE_EDITOR_UTILS
        UPackage* Package = CreateAssetPackage(NormalizedPath);
        if (!Package)
        {
            return nullptr;
        }
        return FStructureEditorUtils::CreateUserDefinedStruct(
            Package,
            FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
#else
        return nullptr;
#endif
    }

    static bool ResolveStructFieldPinType(const FString& FieldType, FEdGraphPinType& OutPinType, FString& OutError)
    {
        OutError.Reset();

        BlueprintHandlerUtils::MakePinTypeFromBpirText(FieldType.TrimStartAndEnd(), OutPinType);
        if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
        {
            OutError = FString::Printf(TEXT("Could not resolve fieldType '%s'"), *FieldType);
            return false;
        }

        return true;
    }

    static bool DoesStructFieldNameExist(
        UUserDefinedStruct* StructAsset,
        const FString& FieldName,
        const FGuid* IgnoredGuid = nullptr)
    {
        if (!StructAsset)
        {
            return false;
        }

        const TArray<FStructVariableDescription>& ExistingVars = FStructureEditorUtils::GetVarDesc(StructAsset);
        for (const FStructVariableDescription& VarDesc : ExistingVars)
        {
            if (IgnoredGuid && VarDesc.VarGuid == *IgnoredGuid)
            {
                continue;
            }

            if (VarDesc.FriendlyName.Equals(FieldName, ESearchCase::IgnoreCase) ||
                VarDesc.VarName.ToString().Equals(FieldName, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }

        return false;
    }

    static UUserDefinedEnum* CreateUserDefinedEnumAsset(const FString& NormalizedPath, const FString& AssetName)
    {
        UPackage* Package = CreateAssetPackage(NormalizedPath);
        if (!Package)
        {
            return nullptr;
        }
        UEnum* NewEnum = FEnumEditorUtils::CreateUserDefinedEnum(
            Package,
            FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        return Cast<UUserDefinedEnum>(NewEnum);
    }

#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    // Apply optional rich-metadata fields to a struct variable that's already been
    // added + renamed. Skipping unset optionals leaves whatever the prior state was
    // intact — guarantees back-compat with the legacy {name,type}-only payloads.
    static void ApplyStructFieldOptionals(
        UUserDefinedStruct* StructAsset,
        const FGuid& VarGuid,
        const FStructFieldSpec& Spec)
    {
        if (!StructAsset || !VarGuid.IsValid())
        {
            return;
        }

        if (Spec.Tooltip.IsSet())
        {
            FStructureEditorUtils::ChangeVariableTooltip(StructAsset, VarGuid, Spec.Tooltip.GetValue());
        }
        if (Spec.DefaultValue.IsSet())
        {
            FStructureEditorUtils::ChangeVariableDefaultValue(StructAsset, VarGuid, Spec.DefaultValue.GetValue());
        }
        if (Spec.bDontEditOnInstance.IsSet())
        {
            // Engine API toggles "editable on instance"; ticket exposes the inverse flag,
            // so flip the bool before forwarding.
            FStructureEditorUtils::ChangeEditableOnBPInstance(StructAsset, VarGuid, !Spec.bDontEditOnInstance.GetValue());
        }
        if (Spec.bEnableSaveGame.IsSet())
        {
            FStructureEditorUtils::ChangeSaveGameEnabled(StructAsset, VarGuid, Spec.bEnableSaveGame.GetValue());
        }
        if (Spec.bMultiLineText.IsSet() && FStructureEditorUtils::CanEnableMultiLineText(StructAsset, VarGuid))
        {
            FStructureEditorUtils::ChangeMultiLineTextEnabled(StructAsset, VarGuid, Spec.bMultiLineText.GetValue());
        }
        if (Spec.bEnable3dWidget.IsSet() && FStructureEditorUtils::CanEnable3dWidget(StructAsset, VarGuid))
        {
            FStructureEditorUtils::Change3dWidgetEnabled(StructAsset, VarGuid, Spec.bEnable3dWidget.GetValue());
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        // FStructureEditorUtils::SetMetaData was added in UE 5.5; on 5.4 struct field
        // metadata cannot be set through this API — skip silently
        for (const TPair<FString, FString>& Kv : Spec.MetaData)
        {
            FStructureEditorUtils::SetMetaData(StructAsset, VarGuid, FName(*Kv.Key), Kv.Value);
        }
#endif
    }

    // Finalize a freshly added/reclaimed struct field: apply the rich-metadata
    // optionals, notify the structure changed, then dirty + save exactly once. Both
    // the seed-reclaim and append paths in AddStructField end here so the "how to
    // finalize a field change" sequence lives in one place.
    static void FinalizeStructFieldChange(
        UUserDefinedStruct* StructAsset,
        const FGuid& VarGuid,
        const FStructFieldSpec& Spec)
    {
        ApplyStructFieldOptionals(StructAsset, VarGuid, Spec);
        FStructureEditorUtils::OnStructureChanged(StructAsset);
        StructAsset->MarkPackageDirty();
        McpSafeAssetSave(StructAsset);
    }

    // True when `StructAsset` holds exactly the engine's untouched seed member and
    // nothing else — i.e. a freshly created struct that no edit has touched yet.
    // FStructureEditorUtils::CreateUserDefinedStruct always seeds one default bool
    // member named MemberVar_0 (UE forbids a zero-field user struct), so a struct
    // that was "created empty" actually carries this stray field. We reclaim it on
    // the first AddStructField call (rename/retype in place) instead of appending, so
    // both "create empty, then add N fields" and "create with N fields" (create_struct
    // routes every field through AddStructField) yield exactly N fields. The guard is
    // deliberately strict: only the pristine seed (single field, MemberVar_<n>
    // default-name shape, plain bool, no container, the engine's own "False" default
    // (the bool type default the struct compiler writes), no tooltip, no metadata)
    // qualifies — a user who already renamed/typed/edited their lone field (including
    // flipping the bool default to true) is never silently overwritten.
    static bool IsUntouchedSeedField(UUserDefinedStruct* StructAsset, FGuid& OutSeedGuid)
    {
        OutSeedGuid.Invalidate();
        if (!StructAsset)
        {
            return false;
        }

        const TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(StructAsset);
        if (Vars.Num() != 1)
        {
            return false;
        }

        const FStructVariableDescription& Seed = Vars[0];

        // Default-name shape: the engine names the seed MemberVar_<digits> and never
        // renames it, so FriendlyName still matches that pattern on an untouched seed.
        const FString FriendlyName = Seed.FriendlyName;
        if (!FriendlyName.StartsWith(TEXT("MemberVar_"), ESearchCase::CaseSensitive))
        {
            return false;
        }
        const FString Suffix = FriendlyName.RightChop(FString(TEXT("MemberVar_")).Len());
        if (Suffix.IsEmpty() || !Suffix.IsNumeric())
        {
            return false;
        }

        // Plain scalar bool — the engine's seed type. Any retype rules it out.
        if (Seed.Category != UEdGraphSchema_K2::PC_Boolean ||
            Seed.ContainerType != EPinContainerType::None)
        {
            return false;
        }

        // The engine seeds the lone bool with the type's default value string ("False"
        // from CompileStructure → PropertyValueToString), so a pristine seed has
        // DefaultValue/CurrentDefaultValue == "False", NOT empty. Accept empty or that
        // engine default; anything else (e.g. the user set the default to true) means
        // the field was edited and must not be silently reclaimed. The tooltip stays
        // empty on an untouched seed.
        auto IsSeedDefault = [](const FString& Value)
        {
            return Value.IsEmpty() || Value.Equals(TEXT("False"), ESearchCase::IgnoreCase);
        };
        if (!IsSeedDefault(Seed.DefaultValue) || !IsSeedDefault(Seed.CurrentDefaultValue) ||
            !Seed.ToolTip.IsEmpty())
        {
            return false;
        }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        // Per-field metadata storage arrived in 5.5; any user metadata means edited.
        if (Seed.MetaData.Num() > 0)
        {
            return false;
        }
#endif

        OutSeedGuid = Seed.VarGuid;
        return true;
    }

    static bool AddStructField(UUserDefinedStruct* StructAsset, const FStructFieldSpec& Spec, FString& OutError)
    {
        OutError.Reset();
        if (!StructAsset)
        {
            OutError = TEXT("Struct asset is null");
            return false;
        }

        const FString CleanFieldName = Spec.Name.TrimStartAndEnd();
        if (CleanFieldName.IsEmpty())
        {
            OutError = TEXT("fieldName is required");
            return false;
        }

        FEdGraphPinType PinType;
        if (!ResolveStructFieldPinType(Spec.Type, PinType, OutError))
        {
            return false;
        }

        if (DoesStructFieldNameExist(StructAsset, CleanFieldName))
        {
            OutError = FString::Printf(TEXT("Field '%s' already exists"), *CleanFieldName);
            return false;
        }

        // First add onto a freshly created empty struct: reclaim the engine's stray
        // MemberVar_0 seed (rename/retype in place) rather than appending, so N adds
        // produce exactly N fields. PinType is already resolved and the name collision
        // already checked above, so retype/rename the seed directly (no re-validation,
        // no intermediate save) and finalize once via FinalizeStructFieldChange.
        FGuid SeedGuid;
        if (IsUntouchedSeedField(StructAsset, SeedGuid))
        {
            // Only change what differs: the seed is a plain bool named MemberVar_<n>,
            // so retype iff the request isn't bool and rename iff the requested name
            // differs.
            const FStructVariableDescription& Seed = FStructureEditorUtils::GetVarDesc(StructAsset)[0];
            const bool bNeedsTypeChange = Seed.ToPinType() != PinType;
            const bool bNeedsRename = !Seed.FriendlyName.Equals(CleanFieldName, ESearchCase::IgnoreCase);
            StructAsset->Modify();
            if (bNeedsTypeChange &&
                !FStructureEditorUtils::ChangeVariableType(StructAsset, SeedGuid, PinType))
            {
                OutError = FString::Printf(TEXT("Failed to change type for field '%s'"), *CleanFieldName);
                return false;
            }
            if (bNeedsRename &&
                !FStructureEditorUtils::RenameVariable(StructAsset, SeedGuid, CleanFieldName))
            {
                OutError = FString::Printf(TEXT("Failed to rename struct field to '%s'"), *CleanFieldName);
                return false;
            }
            FinalizeStructFieldChange(StructAsset, SeedGuid, Spec);
            return true;
        }

        TArray<FStructVariableDescription>& ExistingVars = FStructureEditorUtils::GetVarDesc(StructAsset);
        StructAsset->Modify();
        const int32 PrevCount = ExistingVars.Num();
        if (!FStructureEditorUtils::AddVariable(StructAsset, PinType))
        {
            OutError = TEXT("Failed to add struct field");
            return false;
        }

        TArray<FStructVariableDescription>& UpdatedVars = FStructureEditorUtils::GetVarDesc(StructAsset);
        if (UpdatedVars.Num() <= PrevCount)
        {
            OutError = TEXT("Struct field add reported success but no variable was added");
            return false;
        }

        const FGuid NewVarGuid = UpdatedVars.Last().VarGuid;
        if (!FStructureEditorUtils::RenameVariable(StructAsset, NewVarGuid, CleanFieldName))
        {
            OutError = FString::Printf(TEXT("Failed to rename newly created field to '%s'"), *CleanFieldName);
            return false;
        }

        // Compile + notify so the FProperty exists before the optionals run (SetMetaData
        // mirrors onto it). FinalizeStructFieldChange then applies the rich-metadata
        // optionals, re-notifies, and dirties + saves once with the final field state.
        FStructureEditorUtils::CompileStructure(StructAsset);
        FStructureEditorUtils::OnStructureChanged(StructAsset);
        FinalizeStructFieldChange(StructAsset, NewVarGuid, Spec);
        return true;
    }
#endif
}

// ---- blueprint.create_enum ----
REGISTER_RPC_HANDLER("blueprint.create_enum", "blueprint", "Create a UUserDefinedEnum asset usable in Blueprint variables and pin defaults. Optionally seed with initial entries; later edit via blueprint.set_enum_entries.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Asset path for the new enum (e.g. /Game/Data/E_WeaponType)."),
        RPC_PARAM_OPT("entries", "array", "Initial array of entry name strings; preserves order. Empty/omitted leaves the enum without entries.")
    ))
{
    const FString Path = Ctx.GetString(TEXT("path"));
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path required"));
        return true;
    }

    FString NormalizedPath;
    FString PackageFolder;
    FString AssetName;
    FString PathError;
    if (!ParseAssetPath(Path, NormalizedPath, PackageFolder, AssetName, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ASSET_PATH"), PathError);
        return true;
    }

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(TEXT("PIE_ACTIVE"),
            TEXT("blueprint.create_enum cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    if (ResolveAsset(ToObjectPath(NormalizedPath)).bExists ||
        ResolveAsset(NormalizedPath).bExists)
    {
        Ctx.SendError(TEXT("ALREADY_EXISTS"),
            FString::Printf(TEXT("Asset already exists at '%s'"), *NormalizedPath));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.create_enum")));
    UUserDefinedEnum* EnumAsset = CreateUserDefinedEnumAsset(NormalizedPath, AssetName);
    if (!EnumAsset)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create user-defined enum asset"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FEnumEntrySpec> Entries;
    if (Payload->HasField(TEXT("entries")))
    {
        FString EntriesError;
        if (!ReadEnumEntriesFromPayload(Payload, Entries, EntriesError))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), EntriesError);
            return true;
        }
    }

    if (Payload->HasField(TEXT("entries")))
    {
        FString ApplyError;
        if (!ApplyEnumEntries(EnumAsset, Entries, ApplyError))
        {
            // The create handler is PIE-gated below, so this cleanup only runs in
            // the editor state where the library mutation is supported.
            UEditorAssetLibrary::DeleteAsset(ToObjectPath(NormalizedPath));
            UEditorAssetLibrary::DeleteAsset(NormalizedPath);
            Ctx.SendError(TEXT("ENUM_UPDATE_FAILED"), ApplyError);
            return true;
        }
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), EnumAsset->GetPathName());
    Response->SetArrayField(TEXT("entries"), [] (UUserDefinedEnum* InEnum)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& Entry : GetEnumEntryNames(InEnum))
        {
            Result.Add(MakeShared<FJsonValueString>(Entry));
        }
        return Result;
    }(EnumAsset));
    AddAssetVerification(Response, EnumAsset);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- blueprint.set_enum_entries ----
REGISTER_RPC_HANDLER("blueprint.set_enum_entries", "blueprint", "Replace user-defined Blueprint enum entries",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Enum asset path"),
        RPC_PARAM_REQ("entries", "array", "New array of enum entry names")
    ))
{
    const FString Path = Ctx.GetString(TEXT("path"));
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path required"));
        return true;
    }

    TArray<FEnumEntrySpec> Entries;
    FString EntriesError;
    if (!ReadEnumEntriesFromPayload(Ctx.GetRawPayload(), Entries, EntriesError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), EntriesError);
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UObject* Loaded = LoadAssetByRequestPath(Path, NormalizedPath, LoadError);
    UUserDefinedEnum* EnumAsset = Cast<UUserDefinedEnum>(Loaded);
    if (!EnumAsset)
    {
        Ctx.SendError(TEXT("ENUM_NOT_FOUND"), LoadError.IsEmpty() ? TEXT("Enum asset not found") : LoadError);
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.set_enum_entries")));
    FString ApplyError;
    if (!ApplyEnumEntries(EnumAsset, Entries, ApplyError))
    {
        Ctx.SendError(TEXT("ENUM_UPDATE_FAILED"), ApplyError);
        return true;
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), EnumAsset->GetPathName());
    TArray<TSharedPtr<FJsonValue>> EntryValues;
    for (const FString& Entry : GetEnumEntryNames(EnumAsset))
    {
        EntryValues.Add(MakeShared<FJsonValueString>(Entry));
    }
    Response->SetArrayField(TEXT("entries"), EntryValues);
    AddAssetVerification(Response, EnumAsset);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- blueprint.create_struct ----
REGISTER_RPC_HANDLER("blueprint.create_struct", "blueprint", "Create a UUserDefinedStruct asset usable in Blueprint variables and as a TMap value type. Optionally seed with fields at creation; edit afterwards via blueprint.add_struct_field / remove_struct_field.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Asset path for the new struct (e.g. /Game/Data/S_ItemData)."),
        RPC_PARAM_OPT("fields", "array", "Initial field array of {name, type} entries; type tokens match blueprint.add_variable's variableType. UE forbids a zero-field user struct, so the engine always seeds a default bool 'MemberVar_0'; the first entry here reclaims that seed (rename/retype in place) so you get exactly these fields. Omitting fields leaves the lone seed behind — the first blueprint.add_struct_field then reclaims it the same way.")
    ))
{
    const FString Path = Ctx.GetString(TEXT("path"));
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path required"));
        return true;
    }

    FString NormalizedPath;
    FString PackageFolder;
    FString AssetName;
    FString PathError;
    if (!ParseAssetPath(Path, NormalizedPath, PackageFolder, AssetName, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ASSET_PATH"), PathError);
        return true;
    }

    if (PinWrightPieState::IsPlayInEditorActive())
    {
        Ctx.SendError(TEXT("PIE_ACTIVE"),
            TEXT("blueprint.create_struct cannot run while the editor is in play mode; stop PIE and retry."));
        return true;
    }

    if (ResolveAsset(ToObjectPath(NormalizedPath)).bExists ||
        ResolveAsset(NormalizedPath).bExists)
    {
        Ctx.SendError(TEXT("ALREADY_EXISTS"),
            FString::Printf(TEXT("Asset already exists at '%s'"), *NormalizedPath));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.create_struct")));
    UUserDefinedStruct* StructAsset = CreateUserDefinedStructAsset(NormalizedPath, AssetName);
    if (!StructAsset)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create user-defined struct asset"));
        return true;
    }

#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
    if (Ctx.GetRawPayload()->TryGetArrayField(TEXT("fields"), Fields) && Fields)
    {
        for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
        {
            if (!FieldValue.IsValid() || FieldValue->Type != EJson::Object)
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("fields must contain objects with {name,type}"));
                return true;
            }
            FStructFieldSpec Spec;
            FString SpecError;
            if (!ReadStructFieldSpec(FieldValue->AsObject(), Spec, SpecError))
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), SpecError);
                return true;
            }
            // AddStructField reclaims the engine's stray seed member on its first call
            // (via IsUntouchedSeedField) and appends thereafter, so N fields yield N
            // members — no special-case seed handling needed here.
            FString FieldError;
            if (!AddStructField(StructAsset, Spec, FieldError))
            {
                Ctx.SendError(TEXT("STRUCT_FIELD_ADD_FAILED"), FieldError);
                return true;
            }
        }
    }
#endif

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), StructAsset->GetPathName());
    AddAssetVerification(Response, StructAsset);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- blueprint.add_struct_field ----
REGISTER_RPC_HANDLER("blueprint.add_struct_field", "blueprint", "Add a field to a user-defined Blueprint struct. Optional rich-metadata params mirror the read side (defaultValue, tooltip, metaData, flags).",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Struct asset path"),
        RPC_PARAM_REQ("fieldName", "string", "Struct field name"),
        RPC_PARAM_REQ("fieldType", "string", "Struct field type"),
        RPC_PARAM_OPT("defaultValue", "string", "Initial default value (engine-formatted string)."),
        RPC_PARAM_OPT("tooltip", "string", "Tooltip text for the field."),
        RPC_PARAM_OPT("metaData", "object", "Key/value metadata entries (each value coerced to string)."),
        RPC_PARAM_OPT("flags", "object", "Per-field flags: {dontEditOnInstance, enableSaveGame, multiLineText, enable3dWidget}.")
    ))
{
#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    const FString Path = Ctx.GetString(TEXT("path"));
    const FString FieldName = Ctx.GetString(TEXT("fieldName"));
    const FString FieldType = Ctx.GetString(TEXT("fieldType"));
    if (Path.IsEmpty() || FieldName.IsEmpty() || FieldType.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path, fieldName, and fieldType are required"));
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UObject* Loaded = LoadAssetByRequestPath(Path, NormalizedPath, LoadError);
    UUserDefinedStruct* StructAsset = Cast<UUserDefinedStruct>(Loaded);
    if (!StructAsset)
    {
        Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), LoadError.IsEmpty() ? TEXT("Struct asset not found") : LoadError);
        return true;
    }

    // Build the spec from the full payload so optional defaultValue/tooltip/metaData/flags
    // get carried into AddStructField alongside the required name/type.
    FStructFieldSpec Spec;
    FString SpecError;
    ReadStructFieldSpec(Ctx.GetRawPayload(), Spec, SpecError);
    Spec.Name = FieldName;
    Spec.Type = FieldType;

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.add_struct_field")));
    FString FieldError;
    if (!AddStructField(StructAsset, Spec, FieldError))
    {
        Ctx.SendError(TEXT("STRUCT_FIELD_ADD_FAILED"), FieldError);
        return true;
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), StructAsset->GetPathName());
    Response->SetStringField(TEXT("fieldName"), FieldName);
    Response->SetStringField(TEXT("fieldType"), FieldType);
    AddAssetVerification(Response, StructAsset);
    Ctx.SendSuccess(Response);
    return true;
#else
    Ctx.SendError(TEXT("NOT_AVAILABLE"), TEXT("Struct field editing utilities are unavailable in this build."));
    return true;
#endif
}

// ---- blueprint.remove_struct_field ----
REGISTER_RPC_HANDLER("blueprint.remove_struct_field", "blueprint", "Remove a field from a user-defined Blueprint struct",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Struct asset path"),
        RPC_PARAM_REQ("fieldName", "string", "Struct field name")
    ))
{
#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    const FString Path = Ctx.GetString(TEXT("path"));
    const FString FieldName = Ctx.GetString(TEXT("fieldName"));
    if (Path.IsEmpty() || FieldName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path and fieldName are required"));
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UObject* Loaded = LoadAssetByRequestPath(Path, NormalizedPath, LoadError);
    UUserDefinedStruct* StructAsset = Cast<UUserDefinedStruct>(Loaded);
    if (!StructAsset)
    {
        Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), LoadError.IsEmpty() ? TEXT("Struct asset not found") : LoadError);
        return true;
    }

    TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(StructAsset);
    FGuid TargetGuid;
    for (const FStructVariableDescription& VarDesc : Vars)
    {
        if (VarDesc.VarName.ToString().Equals(FieldName, ESearchCase::IgnoreCase))
        {
            TargetGuid = VarDesc.VarGuid;
            break;
        }
    }

    if (!TargetGuid.IsValid())
    {
        Ctx.SendError(TEXT("FIELD_NOT_FOUND"),
            FString::Printf(TEXT("Field '%s' was not found"), *FieldName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.remove_struct_field")));
    StructAsset->Modify();
    if (!FStructureEditorUtils::RemoveVariable(StructAsset, TargetGuid))
    {
        Ctx.SendError(TEXT("STRUCT_FIELD_REMOVE_FAILED"),
            FString::Printf(TEXT("Failed to remove field '%s'"), *FieldName));
        return true;
    }

    FStructureEditorUtils::OnStructureChanged(StructAsset);
    StructAsset->MarkPackageDirty();
    McpSafeAssetSave(StructAsset);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), StructAsset->GetPathName());
    Response->SetStringField(TEXT("fieldName"), FieldName);
    AddAssetVerification(Response, StructAsset);
    Ctx.SendSuccess(Response);
    return true;
#else
    Ctx.SendError(TEXT("NOT_AVAILABLE"), TEXT("Struct field editing utilities are unavailable in this build."));
    return true;
#endif
}

// ---- blueprint.list_struct_fields ----
REGISTER_RPC_HANDLER("blueprint.list_struct_fields", "blueprint", "List fields in a user-defined Blueprint struct",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Struct asset path")
    ))
{
#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    const FString Path = Ctx.GetString(TEXT("path"));
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path is required"));
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UObject* Loaded = LoadAssetByRequestPath(Path, NormalizedPath, LoadError);
    UUserDefinedStruct* StructAsset = Cast<UUserDefinedStruct>(Loaded);
    if (!StructAsset)
    {
        Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), LoadError.IsEmpty() ? TEXT("Struct asset not found") : LoadError);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> FieldsJson;
    const TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(StructAsset);
    for (const FStructVariableDescription& VarDesc : Vars)
    {
        TSharedPtr<FJsonObject> FieldJson = UserDefinedStructDumpBuilder::BuildUserDefinedStructFieldJson(VarDesc);
        if (!FieldJson.IsValid())
        {
            FieldJson = MakeShared<FJsonObject>();
            FieldJson->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
            FieldJson->SetStringField(TEXT("guid"), VarDesc.VarGuid.ToString());
        }
        FString RawType;
        if (FieldJson->TryGetStringField(TEXT("type"), RawType))
        {
            FieldJson->SetStringField(TEXT("rawType"), RawType);
        }
        FieldJson->SetStringField(TEXT("type"), DescribePinType(VarDesc.ToPinType()));
        FieldsJson.Add(MakeShared<FJsonValueObject>(FieldJson));
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), StructAsset->GetPathName());
    Response->SetStringField(TEXT("guid"), StructAsset->GetCustomGuid().ToString());
    Response->SetArrayField(TEXT("fields"), FieldsJson);
    Response->SetNumberField(TEXT("count"), FieldsJson.Num());
    AddAssetVerification(Response, StructAsset);
    Ctx.SendSuccess(Response);
    return true;
#else
    Ctx.SendError(TEXT("NOT_AVAILABLE"), TEXT("Struct field inspection utilities are unavailable in this build."));
    return true;
#endif
}

#if MCP_HAS_STRUCTURE_EDITOR_UTILS
namespace
{
    // Friendly-name-first, then internal-name lookup mirroring remove_struct_field.
    // Used by the post-add edit handlers below.
    static FGuid FindStructFieldGuidByName(UUserDefinedStruct* StructAsset, const FString& FieldName)
    {
        FGuid Result;
        if (!StructAsset)
        {
            return Result;
        }
        const TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(StructAsset);
        for (const FStructVariableDescription& VarDesc : Vars)
        {
            if (VarDesc.FriendlyName.Equals(FieldName, ESearchCase::IgnoreCase) ||
                VarDesc.VarName.ToString().Equals(FieldName, ESearchCase::IgnoreCase))
            {
                Result = VarDesc.VarGuid;
                break;
            }
        }
        return Result;
    }
}
#endif

// ---- blueprint.set_struct_field_default ----
REGISTER_RPC_HANDLER("blueprint.set_struct_field_default", "blueprint", "Set the default value for an existing user-defined struct field.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Struct asset path"),
        RPC_PARAM_REQ("fieldName", "string", "Friendly or internal name of the field"),
        RPC_PARAM_REQ("value", "string", "New default value (engine-formatted string)")
    ))
{
#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    const FString Path = Ctx.GetString(TEXT("path"));
    const FString FieldName = Ctx.GetString(TEXT("fieldName"));
    const FString Value = Ctx.GetString(TEXT("value"));
    if (Path.IsEmpty() || FieldName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path and fieldName are required"));
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UObject* Loaded = LoadAssetByRequestPath(Path, NormalizedPath, LoadError);
    UUserDefinedStruct* StructAsset = Cast<UUserDefinedStruct>(Loaded);
    if (!StructAsset)
    {
        Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), LoadError.IsEmpty() ? TEXT("Struct asset not found") : LoadError);
        return true;
    }

    const FGuid VarGuid = FindStructFieldGuidByName(StructAsset, FieldName);
    if (!VarGuid.IsValid())
    {
        Ctx.SendError(TEXT("FIELD_NOT_FOUND"),
            FString::Printf(TEXT("Field '%s' was not found"), *FieldName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.set_struct_field_default")));
    FStructureEditorUtils::ChangeVariableDefaultValue(StructAsset, VarGuid, Value);
    FStructureEditorUtils::OnStructureChanged(StructAsset);
    StructAsset->MarkPackageDirty();
    McpSafeAssetSave(StructAsset);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), StructAsset->GetPathName());
    Response->SetStringField(TEXT("fieldName"), FieldName);
    Response->SetStringField(TEXT("value"), Value);
    AddAssetVerification(Response, StructAsset);
    Ctx.SendSuccess(Response);
    return true;
#else
    Ctx.SendError(TEXT("NOT_AVAILABLE"), TEXT("Struct field editing utilities are unavailable in this build."));
    return true;
#endif
}

// ---- blueprint.set_struct_field_metadata ----
REGISTER_RPC_HANDLER("blueprint.set_struct_field_metadata", "blueprint", "Set or clear a metadata key on an existing user-defined struct field. Empty/missing value clears the key.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Struct asset path"),
        RPC_PARAM_REQ("fieldName", "string", "Friendly or internal name of the field"),
        RPC_PARAM_REQ("key", "string", "Metadata key"),
        RPC_PARAM_OPT("value", "string", "Metadata value; empty/missing clears the key")
    ))
{
#if MCP_HAS_STRUCTURE_EDITOR_UTILS
    const FString Path = Ctx.GetString(TEXT("path"));
    const FString FieldName = Ctx.GetString(TEXT("fieldName"));
    const FString Key = Ctx.GetString(TEXT("key"));
    const FString Value = Ctx.GetString(TEXT("value"));
    if (Path.IsEmpty() || FieldName.IsEmpty() || Key.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("path, fieldName, and key are required"));
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UObject* Loaded = LoadAssetByRequestPath(Path, NormalizedPath, LoadError);
    UUserDefinedStruct* StructAsset = Cast<UUserDefinedStruct>(Loaded);
    if (!StructAsset)
    {
        Ctx.SendError(TEXT("STRUCT_NOT_FOUND"), LoadError.IsEmpty() ? TEXT("Struct asset not found") : LoadError);
        return true;
    }

    const FGuid VarGuid = FindStructFieldGuidByName(StructAsset, FieldName);
    if (!VarGuid.IsValid())
    {
        Ctx.SendError(TEXT("FIELD_NOT_FOUND"),
            FString::Printf(TEXT("Field '%s' was not found"), *FieldName));
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.set_struct_field_metadata")));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // FStructureEditorUtils::SetMetaData was added in UE 5.5.
    // Engine SetMetaData natively interprets an empty Value as "remove the key" — see
    // FStructureEditorUtils::SetMetaData in UE 5.6+ (Editor/UnrealEd/StructureEditorUtils.cpp:978).
    FStructureEditorUtils::SetMetaData(StructAsset, VarGuid, FName(*Key), Value);
    StructAsset->MarkPackageDirty();
    McpSafeAssetSave(StructAsset);
#else
    // UE 5.4 has no FStructureEditorUtils::SetMetaData. Reject rather than report success
    // for a no-op — silently claiming the metadata was written would be a garbage stub.
    Ctx.SendUnsupportedEngineVersion(TEXT("5.5"), TEXT("Setting user-defined struct field metadata"));
    return true;
#endif

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("path"), StructAsset->GetPathName());
    Response->SetStringField(TEXT("fieldName"), FieldName);
    Response->SetStringField(TEXT("key"), Key);
    Response->SetStringField(TEXT("value"), Value);
    AddAssetVerification(Response, StructAsset);
    Ctx.SendSuccess(Response);
    return true;
#else
    Ctx.SendError(TEXT("NOT_AVAILABLE"), TEXT("Struct field editing utilities are unavailable in this build."));
    return true;
#endif
}
