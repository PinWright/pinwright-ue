// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightHelpers.h"
#include "ScopedTransaction.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Editor.h"
#include "EditorUtilityBlueprint.h"
#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectGlobals.h"

REGISTER_RPC_HANDLER("editor.create_utility_widget", "editor",
    "Create a new UEditorUtilityWidgetBlueprint asset (a Blutility widget). The new asset can be opened later via editor.spawn_utility_widget_tab.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new UEditorUtilityWidgetBlueprint."),
        RPC_PARAM_REQ("folder", "path", "Content-browser folder for the new asset (e.g. /Game/Tools)."),
        RPC_PARAM_OPT("parentClass", "classref", "Parent UEditorUtilityWidget subclass path; defaults to UEditorUtilityWidget itself."),
        RPC_PARAM_DEF("save", "boolean", "Write the asset to disk; defaults to true. Set false to leave the package dirty.", "true")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();

    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Folder;
    if (!Ctx.RequireString(TEXT("folder"), Folder)) return true;

    // SECURITY: validate folder path against traversal / invalid characters.
    FString SanitizedFolder = SanitizeProjectRelativePath(Folder);
    if (SanitizedFolder.IsEmpty() && !Folder.IsEmpty())
    {
        Ctx.SendError(TEXT("SECURITY_VIOLATION"),
            TEXT("Invalid folder path: path traversal or invalid characters detected"));
        return true;
    }
    Folder = SanitizedFolder;

    const bool bParentClassProvided = Ctx.GetRawPayload().IsValid()
        && Ctx.GetRawPayload()->HasField(TEXT("parentClass"));
    const FString ParentClassParam = Ctx.GetString(TEXT("parentClass"));
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    // Build full asset package path. `folder` passed a sanitizer above; `name` never did, and it
    // was concatenated on raw. CreatePackage logs at Fatal on a package name containing "//"
    // (UObjectGlobals.cpp:1094-1096) - not compiled out in any configuration - so `name: "a//b"`
    // did not fail this call, it ended the editor PROCESS with every unsaved package in it, and the
    // `if (!Package)` below was never reached. Board
    // B-createpackage-unvalidated-paths-plugin-wide; measured on
    // B-foliage-add-type-name-with-slash-kills-the-editor. The checker runs the engine's own rules:
    // INVALID_OBJECTNAME_CHARACTERS on the bare name, IsValidLongPackageName on the composed path,
    // both reason texts surfaced verbatim. The trailing slash is popped first because the sanitizer
    // keeps one and the checker composes with Printf, where a trailing slash on the left would
    // itself produce the "//" this exists to refuse.
    //
    // This block stays ABOVE the parent-class resolution below, and the ordering is load-bearing
    // for the regression test: it pairs every bad `name` with a well-formed parentClass naming no
    // existing class, so a build WITHOUT this check is refused CLASS_NOT_FOUND - above the
    // concatenation - and goes red while the process lives, instead of reaching CreatePackage and
    // taking the suite host down with it. See Tests/UI/TestWidgetCreatePackagePathSafety.cpp.
    FString PackageFolder = Folder;
    if (!IsValidMountPoint(PackageFolder))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P`. Concatenation MANUFACTURES a "//"
        // whenever P already carries a leading slash, and that byte sequence is what
        // CreatePackage logs Fatal on. FString::operator/ routes through PathAppend, which
        // absorbs the duplicate separator instead of adding one.
        PackageFolder = FString(TEXT("/Game")) / PackageFolder;
    }
    PackageFolder.RemoveFromEnd(TEXT("/"));

    FString FullPath;
    FString UtilityWidgetPathError;
    if (!PinWrightComposeAssetPackagePath(PackageFolder, Name, FullPath, UtilityWidgetPathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the destination with 'folder'."),
                *UtilityWidgetPathError));
        return true;
    }

    // Resolve an explicit parent before creating a package. An unresolved or unusable request
    // must not create an orphan package or silently fall back to UEditorUtilityWidget.
    UClass* ParentUClass = UEditorUtilityWidget::StaticClass();
    if (bParentClassProvided)
    {
        UClass* Loaded = ParentClassParam.Equals(TEXT("EditorUtilityWidget"), ESearchCase::IgnoreCase)
            || ParentClassParam.Equals(TEXT("UEditorUtilityWidget"), ESearchCase::IgnoreCase)
            ? UEditorUtilityWidget::StaticClass()
            : ResolveUClass(ParentClassParam);
        if (!Loaded)
        {
            Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
                FString::Printf(TEXT("Parent class '%s' could not be resolved."), *ParentClassParam));
            return true;
        }
        // UEditorUtilityWidget itself is an abstract Blueprint parent by design; only abstract
        // subclasses (and deprecated/superseded classes) are rejected here.
        if (!Loaded->IsChildOf(UEditorUtilityWidget::StaticClass())
            || (Loaded != UEditorUtilityWidget::StaticClass()
                && Loaded->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)))
        {
            Ctx.SendError(TEXT("CLASS_NOT_INSTANTIABLE"),
                FString::Printf(TEXT("Parent class '%s' resolves to '%s', which cannot be used as an UEditorUtilityWidget parent."),
                    *ParentClassParam, *Loaded->GetPathName()));
            return true;
        }
        ParentUClass = Loaded;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: editor.create_utility_widget")));
    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UEditorUtilityWidgetBlueprint* WidgetBP = Cast<UEditorUtilityWidgetBlueprint>(
        FKismetEditorUtilities::CreateBlueprint(
            ParentUClass,
            Package,
            FName(*Name),
            BPTYPE_Normal,
            UEditorUtilityWidgetBlueprint::StaticClass(),
            UWidgetBlueprintGeneratedClass::StaticClass(),
            TEXT("MCP_CreateUtilityWidget")));

    if (!WidgetBP)
    {
        Ctx.SendError(TEXT("CREATION_ERROR"), TEXT("Failed to create editor utility widget blueprint"));
        return true;
    }

    EAssetSaveState SaveState = bSaveRequested
        ? EAssetSaveState::Failed
        : EAssetSaveState::NotRequested;
    bool bSavedToDisk = false;
    if (bSaveRequested)
    {
        Package->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(WidgetBP);
        bSavedToDisk = SaveAssetToDiskReportingPresence(WidgetBP, /*bForce=*/true,
            nullptr, nullptr, &SaveState);
    }
    else
    {
        McpSafeAssetSave(WidgetBP);
    }

    const FString ObjectPath = WidgetBP->GetPathName();
    const FString ClassName = WidgetBP->ParentClass ? WidgetBP->ParentClass->GetPathName() : FString();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), ObjectPath);
    Result->SetStringField(TEXT("className"), ClassName);
    Result->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk, SaveState);
    AddAssetVerification(Result, WidgetBP);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("editor.spawn_utility_widget_tab", "editor",
    "Spawn and register a dockable tab for a UEditorUtilityWidgetBlueprint, returning the tab id for later closure.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path to the UEditorUtilityWidgetBlueprint asset.")
    ))
{
    FString Path;
    if (!Ctx.RequireString(TEXT("assetPath"), Path)) return true;

    UEditorUtilityWidgetBlueprint* WidgetBP = LoadObject<UEditorUtilityWidgetBlueprint>(nullptr, *Path);
    if (!WidgetBP)
    {
        // Distinguish "load returned a wrong type" from "nothing at that path".
        UObject* Probe = LoadObject<UObject>(nullptr, *Path);
        if (Probe)
        {
            Ctx.SendError(TEXT("NOT_A_UTILITY_WIDGET_BP"),
                FString::Printf(TEXT("Asset at '%s' is not a UEditorUtilityWidgetBlueprint."), *Path));
        }
        else
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("No UEditorUtilityWidgetBlueprint found at '%s'."), *Path));
        }
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("GEditor is not available in this context."));
        return true;
    }

    UEditorUtilitySubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();
    if (!Subsystem)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"),
            TEXT("UEditorUtilitySubsystem is unavailable."));
        return true;
    }

    FName TabId;
    UEditorUtilityWidget* Widget = Subsystem->SpawnAndRegisterTabAndGetID(WidgetBP, TabId);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), Widget != nullptr);
    Result->SetStringField(TEXT("tabId"), TabId.ToString());
    Result->SetStringField(TEXT("widgetPath"), WidgetBP->GetPathName());
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("editor.run_utility_blueprint", "editor",
    "Execute UEditorUtilitySubsystem::TryRun on a UEditorUtilityBlueprint or UEditorUtilityWidgetBlueprint (the 'Run Editor Utility Blueprint' action).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path to the UEditorUtilityBlueprint or UEditorUtilityWidgetBlueprint asset."),
        RPC_PARAM_OPT("params", "object", "Reserved for forward-compatibility — must be empty/omitted today.")
    ))
{
    FString Path;
    if (!Ctx.RequireString(TEXT("assetPath"), Path)) return true;

    // Reject non-empty params payloads up front so the API can grow argument forwarding without breaking callers.
    const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->TryGetObjectField(TEXT("params"), ParamsObj)
        && ParamsObj && ParamsObj->IsValid() && (*ParamsObj)->Values.Num() > 0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("'params' must be empty/omitted; argument forwarding to TryRun is not yet implemented."));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *Path);
    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("No asset found at '%s'."), *Path));
        return true;
    }
    if (!Asset->IsA<UEditorUtilityBlueprint>() && !Asset->IsA<UEditorUtilityWidgetBlueprint>())
    {
        Ctx.SendError(TEXT("NOT_A_UTILITY_BLUEPRINT"),
            FString::Printf(TEXT("Asset at '%s' is neither a UEditorUtilityBlueprint nor a UEditorUtilityWidgetBlueprint."), *Path));
        return true;
    }

    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("GEditor is not available in this context."));
        return true;
    }

    UEditorUtilitySubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();
    if (!Subsystem)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"),
            TEXT("UEditorUtilitySubsystem is unavailable."));
        return true;
    }

    const bool bRan = Subsystem->TryRun(Asset);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), bRan);
    Result->SetBoolField(TEXT("ran"), bRan);
    Result->SetStringField(TEXT("assetPath"), Path);
    Ctx.SendSuccess(Result);
    return true;
}
