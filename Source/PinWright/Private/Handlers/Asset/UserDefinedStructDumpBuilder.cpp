// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/UserDefinedStructDumpBuilder.h"

#include "Dom/JsonValue.h"

#include "EdGraph/EdGraphPin.h"
#include "Kismet2/StructureEditorUtils.h"
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h"
#elif __has_include("Engine/UserDefinedStruct.h")
#include "Engine/UserDefinedStruct.h"
#endif
#include "UObject/Class.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace UserDefinedStructDumpBuilder
{
    namespace
    {
        FString ContainerTypeToString(EPinContainerType Type)
        {
            if (const UEnum* Enum = StaticEnum<EPinContainerType>())
            {
                return Enum->GetNameStringByValue(static_cast<int64>(Type));
            }
            return FString();
        }

    }

    TSharedPtr<FJsonObject> BuildUserDefinedStructFieldJson(const FStructVariableDescription& VarDesc)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
        Obj->SetStringField(TEXT("displayName"), VarDesc.FriendlyName);
        Obj->SetStringField(TEXT("guid"), VarDesc.VarGuid.ToString());
        Obj->SetStringField(TEXT("type"), VarDesc.Category.ToString());
        Obj->SetStringField(TEXT("subType"), VarDesc.SubCategory.ToString());
        Obj->SetStringField(TEXT("subTypeObject"), VarDesc.SubCategoryObject.ToString());
        Obj->SetStringField(TEXT("containerType"), ContainerTypeToString(VarDesc.ContainerType));
        Obj->SetStringField(TEXT("defaultValue"), VarDesc.DefaultValue);
        Obj->SetStringField(TEXT("currentDefaultValue"), VarDesc.CurrentDefaultValue);
        Obj->SetStringField(TEXT("tooltip"), VarDesc.ToolTip);

        TSharedRef<FJsonObject> FlagsObj = MakeShared<FJsonObject>();
        FlagsObj->SetBoolField(TEXT("dontEditOnInstance"), VarDesc.bDontEditOnInstance != 0);
        FlagsObj->SetBoolField(TEXT("enableSaveGame"), VarDesc.bEnableSaveGame != 0);
        FlagsObj->SetBoolField(TEXT("multiLineText"), VarDesc.bEnableMultiLineText != 0);
        FlagsObj->SetBoolField(TEXT("enable3dWidget"), VarDesc.bEnable3dWidget != 0);
        Obj->SetObjectField(TEXT("flags"), FlagsObj);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        // FStructVariableDescription::MetaData (TMap<FName,FString>) was added
        // in UE 5.5; omit the metaData field entirely on 5.4 to keep the rest
        // of the JSON dump intact.
        {
            TSharedRef<FJsonObject> MetaObj = MakeShared<FJsonObject>();
            TArray<FName> MetaKeys;
            VarDesc.MetaData.GetKeys(MetaKeys);
            MetaKeys.Sort([](const FName& A, const FName& B)
            {
                return A.LexicalLess(B);
            });
            for (const FName& Key : MetaKeys)
            {
                MetaObj->SetStringField(Key.ToString(), VarDesc.MetaData[Key]);
            }
            Obj->SetObjectField(TEXT("metaData"), MetaObj);
        }
#endif

        return Obj;
    }

    TSharedPtr<FJsonObject> BuildUserDefinedStructJson(const UUserDefinedStruct* Struct)
    {
        if (!Struct)
        {
            return nullptr;
        }
        if (!Struct->EditorData)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("assetKind"), TEXT("UserDefinedStruct"));
        Root->SetStringField(TEXT("path"), Struct->GetPathName());
        Root->SetStringField(TEXT("guid"), Struct->GetCustomGuid().ToString());

        const TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);

        TArray<const FStructVariableDescription*> Sorted;
        Sorted.Reserve(VarDescs.Num());
        for (const FStructVariableDescription& VarDesc : VarDescs)
        {
            Sorted.Add(&VarDesc);
        }
        Sorted.Sort([](const FStructVariableDescription& A, const FStructVariableDescription& B)
        {
            return A.VarName.LexicalLess(B.VarName);
        });

        TArray<TSharedPtr<FJsonValue>> Fields;
        Fields.Reserve(Sorted.Num());
        for (const FStructVariableDescription* VarDesc : Sorted)
        {
            Fields.Add(MakeShared<FJsonValueObject>(BuildUserDefinedStructFieldJson(*VarDesc)));
        }
        Root->SetArrayField(TEXT("fields"), Fields);

        return Root;
    }
}

namespace
{
    UClass* GetUserDefinedStructSidecarClass()
    {
        return UUserDefinedStruct::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildUserDefinedStructSidecar(UObject* Asset)
    {
        return UserDefinedStructDumpBuilder::BuildUserDefinedStructJson(Cast<UUserDefinedStruct>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("user_defined_struct"), DumpFileNames::UserDefinedStruct,
    &GetUserDefinedStructSidecarClass, &BuildUserDefinedStructSidecar,
    nullptr, nullptr, TEXT("UserDefinedStruct has no EditorData."), 100);
