// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetCreateHandler.cpp
// Widget creation handlers migrated from _WidgetAuthoringHandlers.cpp

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "ScopedTransaction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/CanvasPanel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace WidgetAuthoringHelpers;

// ---- widget.create_widget_blueprint ----
REGISTER_RPC_HANDLER("widget.create_widget_blueprint", "widget", "Author a new UWidgetBlueprint asset (UMG widget). Distinct from ui.create_hud which instantiates an existing blueprint at runtime.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new UWidgetBlueprint."),
        RPC_PARAM_OPT("folder", "path", "Content-browser folder for the new asset. Defaults to /Game/UI."),
        RPC_PARAM_OPT("parentClass", "classref", "Parent UUserWidget subclass; defaults to UUserWidget itself."),
        RPC_PARAM_DEF("save", "boolean", "Write the asset to disk; defaults to true. Set false to leave the package dirty.", "true")
    ))
{
    const double StartedAt = FPlatformTime::Seconds();
    FString Name = Ctx.GetString(TEXT("name"));
    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"), TEXT("Missing required parameter: name"));
        return true;
    }

    // `name` is the one argument here with no sanitizer of its own, and a few lines below it is
    // concatenated onto the folder raw and handed to CreatePackage. CreatePackage logs at Fatal on
    // a package name containing "//" (UObjectGlobals.cpp:1094-1096), a verbosity that is not
    // compiled out in any configuration - so `name: "a//b"` does not fail the call, it ends the
    // editor PROCESS and every unsaved package in it, and the `if (!Package)` below is never
    // reached. Board B-createpackage-unvalidated-paths-plugin-wide; the mechanism was measured on
    // B-foliage-add-type-name-with-slash-kills-the-editor.
    //
    // CHECKED HERE, ABOVE THE FOLDER SANITIZER, and the ordering is load-bearing twice over.
    // (1) The folder already has a guard and the name has none, so answering the name first names
    // the argument the caller actually has to fix. (2) The regression test pairs every bad `name`
    // with a folder SanitizeProjectRelativePath rejects, so a build WITHOUT this check answers
    // SECURITY_VIOLATION from the folder - ABOVE the concatenation - and goes red while the process
    // lives, instead of reaching CreatePackage and taking the suite host down with it. Do NOT move
    // this below the folder block: see Tests/UI/TestWidgetCreatePackagePathSafety.cpp.
    //
    // The rule is the engine's own object-naming rule rather than a hand-rolled character list;
    // the composed path is checked against the engine's package rule further down. It closes both
    // of CreatePackage's Fatals rather than only the famous one: INVALID_OBJECTNAME_CHARACTERS
    // carries '.' and ':' as well as '/', so `name: ".."` - which ResolveName2 chews down to an
    // empty name and CreatePackage logs at Fatal on its own line (:1118) - is refused here too.
    FText WidgetNameReason;
    if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &WidgetNameReason))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("'%s' is not a bare asset name: %s Choose the destination with 'folder'."),
                *Name, *WidgetNameReason.ToString()));
        return true;
    }

    FString Folder = Ctx.GetString(TEXT("folder"), TEXT("/Game/UI"));

    // SECURITY: Validate folder path for traversal attacks
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
    FString ParentClass = Ctx.GetString(TEXT("parentClass"), TEXT("UserWidget"));
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), true);

    // Build full path. Second half of the guard above: a folder that is safe on its own can still
    // compose a path CreatePackage refuses - INVALID_LONGPACKAGE_CHARACTERS (which is where '\' is
    // caught), a trailing slash, an unmounted root - so the composed candidate goes through the
    // shared checker too and both engine reason texts are surfaced verbatim. The trailing slash is
    // popped first because the sanitizer keeps one and the checker composes with Printf, where a
    // trailing slash on the left would itself produce the "//" this exists to refuse.
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
    FString WidgetPathError;
    if (!PinWrightComposeAssetPackagePath(PackageFolder, Name, FullPath, WidgetPathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the destination with 'folder'."),
                *WidgetPathError));
        return true;
    }

    // Resolve an explicit parent before creating a package. An unresolved or unusable request
    // must not silently fall back to UUserWidget or leave an orphan package behind.
    UClass* ParentUClass = UUserWidget::StaticClass();
    if (bParentClassProvided)
    {
        UClass* FoundClass = ParentClass.Equals(TEXT("UserWidget"), ESearchCase::IgnoreCase)
            ? UUserWidget::StaticClass()
            : ResolveUClass(ParentClass);
        if (!FoundClass)
        {
            Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
                FString::Printf(TEXT("Parent class '%s' could not be resolved."), *ParentClass));
            return true;
        }
        // UUserWidget itself is an abstract Blueprint parent by design; only abstract subclasses
        // (and deprecated/superseded classes) are rejected here.
        if (!FoundClass->IsChildOf(UUserWidget::StaticClass())
            || (FoundClass != UUserWidget::StaticClass()
                && FoundClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)))
        {
            Ctx.SendError(TEXT("CLASS_NOT_INSTANTIABLE"),
                FString::Printf(TEXT("Parent class '%s' resolves to '%s', which cannot be used as a UUserWidget parent."),
                    *ParentClass, *FoundClass->GetPathName()));
            return true;
        }
        ParentUClass = FoundClass;
    }

    // Create package
    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: widget.create_widget_blueprint")));
    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    // Create widget blueprint
    UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        ParentUClass,
        Package,
        FName(*Name),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()
    ));

    if (!WidgetBlueprint)
    {
        Ctx.SendError(TEXT("CREATION_ERROR"), TEXT("Failed to create widget blueprint"));
        return true;
    }

    bool bRootCreated = false;
    if (WidgetBlueprint->WidgetTree && !WidgetBlueprint->WidgetTree->RootWidget)
    {
        UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(),
            TEXT("RootCanvas"));
        if (!RootCanvas)
        {
            Ctx.SendError(TEXT("CREATION_ERROR"), TEXT("Failed to create default root canvas panel"));
            return true;
        }
        WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
        bRootCreated = true;

        // Version-guarded helper: registers the GUID on UE 5.5+, no-op on 5.4
        // (which has no WidgetVariableNameToGuidMap / OnVariableAdded).
        EnsureWidgetVariableGuid(WidgetBlueprint, RootCanvas->GetFName());
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

    EAssetSaveState SaveState = bSaveRequested
        ? EAssetSaveState::Failed
        : EAssetSaveState::NotRequested;
    bool bSavedToDisk = false;
    if (bSaveRequested)
    {
        Package->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(WidgetBlueprint);
        bSavedToDisk = SaveAssetToDiskReportingPresence(WidgetBlueprint, /*bForce=*/true,
            nullptr, nullptr, &SaveState);
    }
    else
    {
        McpSafeAssetSave(WidgetBlueprint);
    }

    FString ObjectPath = WidgetBlueprint->GetPathName();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Created widget blueprint: %s"), *Name));
    Result->SetStringField(TEXT("widgetPath"), ObjectPath);
    Result->SetStringField(TEXT("parentClass"),
        WidgetBlueprint->ParentClass ? WidgetBlueprint->ParentClass->GetPathName() : FString());
    Result->SetBoolField(TEXT("rootCreated"), bRootCreated);
    Result->SetStringField(TEXT("rootWidget"),
        (WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget)
            ? WidgetBlueprint->WidgetTree->RootWidget->GetName()
            : TEXT(""));
    Result->SetNumberField(TEXT("changesApplied"), bRootCreated ? 2 : 1);
    Result->SetNumberField(TEXT("durationMs"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk, SaveState);
    AddAssetVerification(Result, WidgetBlueprint);

    Ctx.SendSuccess(Result);
    return true;
}
