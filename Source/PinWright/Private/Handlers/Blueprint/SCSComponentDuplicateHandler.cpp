// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
#include "PinWrightHelpers.h"
#include "Utils/PropertyUtils.h"
#include "ScopedTransaction.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"


namespace
{
    FString MakeNameKey(const FString& Name)
    {
        return Name.ToLower();
    }

    USCS_Node* FindScsNodeCaseInsensitive(USimpleConstructionScript* SCS, const FString& Name)
    {
        if (!SCS)
        {
            return nullptr;
        }

        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (Node && Node->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        return nullptr;
    }

    void SeedScsUsedNames(USimpleConstructionScript* SCS, TSet<FString>& UsedNames)
    {
        if (!SCS)
        {
            return;
        }

        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (Node)
            {
                UsedNames.Add(MakeNameKey(Node->GetVariableName().ToString()));
            }
        }
    }

    FString GenerateScsDuplicateName(const TSet<FString>& UsedNames, USCS_Node* SourceNode)
    {
        const FString BaseName = FString::Printf(TEXT("%s_Copy"), *SourceNode->GetVariableName().ToString());
        FString Candidate = BaseName;
        int32 Suffix = 1;
        while (UsedNames.Contains(MakeNameKey(Candidate)))
        {
            Candidate = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
        }
        return Candidate;
    }

    void AddScsMapping(TSharedPtr<FJsonObject> Mapping, USCS_Node* SourceNode, USCS_Node* TargetNode)
    {
        if (Mapping.IsValid() && SourceNode && TargetNode)
        {
            Mapping->SetStringField(SourceNode->GetVariableName().ToString(), TargetNode->GetVariableName().ToString());
        }
    }

    void CopyTemplateProperties(UActorComponent* SourceTemplate, UActorComponent* TargetTemplate)
    {
        if (!SourceTemplate || !TargetTemplate)
        {
            return;
        }

        TargetTemplate->Modify();

        FFilteredPropertyCopyOptions Options;
        AddSceneAttachmentPropertySkips(Options);
        CopyFilteredMatchingProperties(SourceTemplate, TargetTemplate, Options);

        if (USceneComponent* SourceScene = Cast<USceneComponent>(SourceTemplate))
        {
            if (USceneComponent* TargetScene = Cast<USceneComponent>(TargetTemplate))
            {
                TargetScene->SetRelativeTransform(SourceScene->GetRelativeTransform());
                TargetScene->ComponentTags = SourceScene->ComponentTags;
            }
        }
    }

    struct FScsComponentDuplicateOptions
    {
        FString DesiredName;
        bool bDuplicateChildren = true;
        bool bCopyProperties = true;
        bool bCopyAttachment = true;
    };

    struct FScsComponentDuplicateContext
    {
        TSharedPtr<FJsonObject> Mapping;
        TArray<FString>* Warnings = nullptr;
        TSet<FString> UsedNames;
    };

    void CopyScsAttachmentMetadata(USCS_Node* SourceNode, USCS_Node* TargetNode)
    {
        if (!SourceNode || !TargetNode)
        {
            return;
        }

        TargetNode->Modify();
        TargetNode->AttachToName = SourceNode->AttachToName;
        TargetNode->ParentComponentOrVariableName = SourceNode->ParentComponentOrVariableName;
        TargetNode->ParentComponentOwnerClassName = SourceNode->ParentComponentOwnerClassName;
        TargetNode->bIsParentComponentNative = SourceNode->bIsParentComponentNative;
    }

    void ClearSameScsParentMetadata(USCS_Node* Node)
    {
        if (!Node)
        {
            return;
        }

        Node->Modify();
        Node->ParentComponentOrVariableName = NAME_None;
        Node->ParentComponentOwnerClassName = NAME_None;
        Node->bIsParentComponentNative = false;
    }

    void AddSameScsChildNode(USCS_Node* ParentNode, USCS_Node* ChildNode)
    {
        if (!ParentNode || !ChildNode)
        {
            return;
        }

        ParentNode->AddChildNode(ChildNode);
        ClearSameScsParentMetadata(ChildNode);
    }

    USCS_Node* DuplicateScsNode(USimpleConstructionScript* SCS, USCS_Node* SourceNode,
        const FScsComponentDuplicateOptions& Options, FScsComponentDuplicateContext& DuplicateContext,
        FString& OutError)
    {
        if (!SCS || !SourceNode || !SourceNode->ComponentClass)
        {
            OutError = TEXT("Invalid source SCS node");
            return nullptr;
        }

        const FString TargetName = Options.DesiredName.IsEmpty() ? GenerateScsDuplicateName(DuplicateContext.UsedNames, SourceNode) : Options.DesiredName;
        const FString TargetNameKey = MakeNameKey(TargetName);
        if (DuplicateContext.UsedNames.Contains(TargetNameKey))
        {
            OutError = FString::Printf(TEXT("Component with name '%s' already exists"), *TargetName);
            return nullptr;
        }

        SCS->Modify();
        USCS_Node* NewNode = SCS->CreateNode(SourceNode->ComponentClass, FName(*TargetName));
        if (!NewNode)
        {
            OutError = FString::Printf(TEXT("Failed to create SCS node '%s'"), *TargetName);
            return nullptr;
        }
        DuplicateContext.UsedNames.Add(MakeNameKey(NewNode->GetVariableName().ToString()));

        NewNode->Modify();
        if (Options.bCopyProperties)
        {
            CopyTemplateProperties(SourceNode->ComponentTemplate, NewNode->ComponentTemplate);
        }

        if (Options.bCopyAttachment)
        {
            CopyScsAttachmentMetadata(SourceNode, NewNode);
        }

        AddScsMapping(DuplicateContext.Mapping, SourceNode, NewNode);

        if (Options.bDuplicateChildren)
        {
            for (USCS_Node* SourceChild : SourceNode->GetChildNodes())
            {
                if (!SourceChild)
                {
                    continue;
                }

                FString ChildError;
                FScsComponentDuplicateOptions ChildOptions = Options;
                ChildOptions.DesiredName.Reset();
                USCS_Node* NewChild = DuplicateScsNode(SCS, SourceChild, ChildOptions, DuplicateContext, ChildError);
                if (!NewChild)
                {
                    if (DuplicateContext.Warnings)
                    {
                        DuplicateContext.Warnings->Add(ChildError);
                    }
                    continue;
                }
                AddSameScsChildNode(NewNode, NewChild);
            }
        }

        return NewNode;
    }
}

REGISTER_RPC_HANDLER("blueprint.scs.duplicate_component", "blueprint", "Duplicate a Blueprint SCS component template, optionally copying children, properties, attachment, and relative transform.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Blueprint asset path."),
        RPC_PARAM_OPT("sourceName", "string", "Name of the source SCS component."),
        RPC_PARAM_OPT("componentName", "string", "Alias for sourceName."),
        RPC_PARAM_OPT("newName", "string", "Name for the duplicated root component; generated when omitted."),
        RPC_PARAM_OPT("targetParentName", "string", "Target parent SCS component; defaults to the source component's parent."),
        RPC_PARAM_OPT("duplicateChildren", "boolean", "Whether to recursively duplicate child SCS nodes. Defaults true."),
        RPC_PARAM_OPT("copyProperties", "boolean", "Whether to copy component template properties. Defaults true."),
        RPC_PARAM_OPT("copyAttachment", "boolean", "Whether to preserve attachment/socket metadata. Defaults true."),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath))
    {
        return true;
    }

    const FString SourceName = Ctx.GetStringFirstOf({TEXT("sourceName"), TEXT("componentName")});
    if (SourceName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sourceName is required"));
        return true;
    }

    FString NormalizedPath;
    FString LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, NormalizedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), LoadError.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"), *BlueprintPath)
            : LoadError);
        return true;
    }

    const BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey =
        BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);
    if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
            Ctx, Blueprint, ReinstancingSurvey, TEXT("blueprint.scs.duplicate_component")))
    {
        return true;
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        Ctx.SendError(TEXT("SCS_NOT_FOUND"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    USCS_Node* SourceNode = FindScsNodeCaseInsensitive(SCS, SourceName);
    if (!SourceNode)
    {
        Ctx.SendError(TEXT("SCS_COMPONENT_NOT_FOUND"), FString::Printf(TEXT("Source component not found: %s"), *SourceName));
        return true;
    }

    USCS_Node* TargetParent = nullptr;
    const FString TargetParentName = Ctx.GetString(TEXT("targetParentName"));
    if (!TargetParentName.IsEmpty())
    {
        TargetParent = FindScsNodeCaseInsensitive(SCS, TargetParentName);
        if (!TargetParent)
        {
            Ctx.SendError(TEXT("SCS_PARENT_NOT_FOUND"), FString::Printf(TEXT("Target parent component not found: %s"), *TargetParentName));
            return true;
        }
        if (TargetParent == SourceNode || TargetParent->IsChildOf(SourceNode))
        {
            Ctx.SendError(TEXT("INVALID_PARENT"), TEXT("Cannot duplicate an SCS component into itself or its descendant"));
            return true;
        }
    }
    else
    {
        TargetParent = SCS->FindParentNode(SourceNode);
    }

    const bool bDuplicateChildren = Ctx.GetBool(TEXT("duplicateChildren"), true);
    const bool bCopyProperties = Ctx.GetBool(TEXT("copyProperties"), true);
    const bool bCopyAttachment = Ctx.GetBool(TEXT("copyAttachment"), true);

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.scs.duplicate_component")));
    Blueprint->Modify();
    SCS->Modify();
    if (TargetParent)
    {
        TargetParent->Modify();
    }

    TSharedPtr<FJsonObject> Mapping = MakeShared<FJsonObject>();
    TArray<FString> Warnings;
    FString DuplicateError;
    FScsComponentDuplicateOptions DuplicateOptions;
    DuplicateOptions.DesiredName = Ctx.GetString(TEXT("newName"));
    DuplicateOptions.bDuplicateChildren = bDuplicateChildren;
    DuplicateOptions.bCopyProperties = bCopyProperties;
    DuplicateOptions.bCopyAttachment = bCopyAttachment;
    FScsComponentDuplicateContext DuplicateContext;
    DuplicateContext.Mapping = Mapping;
    DuplicateContext.Warnings = &Warnings;
    SeedScsUsedNames(SCS, DuplicateContext.UsedNames);
    USCS_Node* NewNode = DuplicateScsNode(SCS, SourceNode, DuplicateOptions, DuplicateContext, DuplicateError);
    if (!NewNode)
    {
        Ctx.SendError(TEXT("DUPLICATE_FAILED"), DuplicateError);
        return true;
    }

    if (TargetParent)
    {
        AddSameScsChildNode(TargetParent, NewNode);
    }
    else
    {
        SCS->Modify();
        SCS->AddNode(NewNode);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
    // Mark-dirty only (Blueprint). saved is reported below from a measurement; it used
    // to be McpSafeAssetSave's constant-true return.
    McpSafeAssetSave(Blueprint);

    TArray<TSharedPtr<FJsonValue>> WarningValues;
    for (const FString& Warning : Warnings)
    {
        WarningValues.Add(MakeShared<FJsonValueString>(Warning));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("sourceName"), SourceNode->GetVariableName().ToString());
    Result->SetStringField(TEXT("componentName"), NewNode->GetVariableName().ToString());
    Result->SetStringField(TEXT("componentClass"), NewNode->ComponentClass ? NewNode->ComponentClass->GetPathName() : TEXT(""));
    Result->SetObjectField(TEXT("mapping"), Mapping);
    Result->SetArrayField(TEXT("warnings"), WarningValues);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(
        Diagnostics, Result, TEXT("compileErrors"), TEXT("compileWarnings"));
    BlueprintReinstancingGuard::AddSurveyToJson(ReinstancingSurvey, Result);
    AddMarkDirtySaveReport(Result, Blueprint, /*bSaveRequested=*/true);
    Result->SetBoolField(TEXT("compileRequired"), true);
    Result->SetBoolField(TEXT("saveRequired"), true);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
