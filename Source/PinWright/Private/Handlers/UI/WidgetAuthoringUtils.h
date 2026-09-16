// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Layout/Visibility.h"

class UMovieScene;
class UWidgetAnimation;

namespace WidgetAuthoringHelpers
{
    FLinearColor GetColorFromJsonWidget(const TSharedPtr<FJsonObject>& ColorObj, const FLinearColor& Default = FLinearColor::White);

    TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName);

    const TArray<TSharedPtr<FJsonValue>>* GetArrayField(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName);

    // Returns nullptr - logging one Warning, never an Error - for a path containing "//", which
    // would reach CreatePackage's Fatal through the StaticLoadObject fallbacks and end the editor
    // PROCESS (board B-createpackage-unvalidated-paths-plugin-wide). Also returns nullptr for a
    // "_C" generated-class path and for a path that resolves to no widget blueprint; callers get
    // one failure shape and must not distinguish these.
    UWidgetBlueprint* LoadWidgetBlueprint(const FString& WidgetPath);

    // Strict variant: returns false if the input is not one of the five canonical
    // visibility strings ("Visible", "Collapsed", "Hidden", "HitTestInvisible",
    // "SelfHitTestInvisible"); leaves Out untouched on failure. Case-insensitive.
    bool TryParseVisibility(const FString& In, ESlateVisibility& Out);

    // Lenient variant for back-compat: unknown strings fall back to ESlateVisibility::Visible.
    ESlateVisibility GetVisibility(const FString& VisibilityStr);

    FString VisibilityToString(ESlateVisibility Visibility);

    void AddWidgetToParentOrRoot(UWidgetBlueprint* WidgetBP, UWidget* NewWidget, const FString& ParentSlot, bool bSetAsRootIfEmpty = false);

    UClass* ResolveWidgetClass(const FString& ClassName);

    UWidget* ConstructWidgetForAuthoring(UWidgetBlueprint* WidgetBP, UClass* WidgetClass,
        const FName& WidgetName, FString* OutError = nullptr);

    void DiscardConstructedWidgetForAuthoring(UWidgetBlueprint* WidgetBP, UWidget* Widget);

    // Per-widget snapshot of the original designer-eye and runtime-visibility
    // state, captured before the override is applied so revert can restore
    // exactly what was there.
    struct FTransientDesignerOverride
    {
        TWeakObjectPtr<UWidget> Widget;
        bool bHadHiddenOverride = false;
        bool bOriginalHidden = false;
        bool bHadVisibilityOverride = false;
        ESlateVisibility OriginalVisibility = ESlateVisibility::Visible;
    };

    struct FTransientDesignerOverrides
    {
        TArray<FTransientDesignerOverride> Entries;

        int32 NumHiddenOverrides() const;
        int32 NumVisibilityOverrides() const;
    };

    // Apply transient designer-eye + runtime-Visibility overrides to the children of PreviewRoot
    // (a live UUserWidget instance owned by the open Designer preview, NOT the asset's WidgetTree).
    // Captures original state into OutGuard for use with RevertTransientDesignerOverrides.
    // Returns false on unresolved widget name or unknown visibility string, with the offending value in OutError.
    bool ApplyTransientDesignerOverrides(
        UUserWidget* PreviewRoot,
        const TArray<FString>& ShowOnly,
        const TArray<FString>& Hide,
        const TMap<FString, ESlateVisibility>& VisibilityOverrides,
        FTransientDesignerOverrides& OutGuard,
        FString& OutError);

    void RevertTransientDesignerOverrides(const FTransientDesignerOverrides& Guard);

    UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBP, const FString& WidgetName);

    // Matches by either UObject name or FName.ToString(), case-insensitive.
    UWidgetAnimation* FindAnimationByName(UWidgetBlueprint* WidgetBP, const FString& AnimationName);

    // What EnsureAnimationMovieScene had to do. Both flags are measurements of the work
    // performed, so a caller can report them without re-deriving anything.
    struct FAnimationMovieSceneResult
    {
        UMovieScene* MovieScene = nullptr;
        // A MovieScene was absent and one was constructed.
        bool bCreated = false;
        // A MovieScene existed under a name that would not bind, and was renamed.
        bool bRenamed = false;
    };

    // Returns Animation's MovieScene, creating it when absent, and guarantees
    // MovieScene->GetFName() == Animation->GetFName().
    //
    // That equality is what makes an animation playable, and nothing reports its absence.
    // The UMG compiler names the generated animation property after the ANIMATION
    // (WidgetBlueprintCompiler.cpp: `AnimVariableDesc.VarName = Animation->GetFName()`),
    // while UWidgetBlueprintGeneratedClass::BindAnimationsStatic looks that property up by
    // the MOVIE SCENE's name
    // (WidgetBlueprintGeneratedClass.cpp: `InPropertyMap.Find(Animation->GetMovieScene()->GetFName())`).
    // Under any other MovieScene name the blueprint compiles clean, the generated property
    // exists and is readable, and it is permanently null — so every PlayAnimation call is a
    // silent no-op. The engine's own designer upholds the same equality at create, rename and
    // duplicate (UMGEditor TabFactory/AnimationTabSummoner.cpp).
    //
    // Passing a null Animation returns an all-default result rather than asserting.
    FAnimationMovieSceneResult EnsureAnimationMovieScene(UWidgetAnimation* Animation);

    bool IsDescendantWidget(UWidget* CandidateParent, UWidget* PossibleDescendant);

    int32 ResolveWidgetInsertIndex(UWidgetBlueprint* WidgetBP, UPanelWidget* TargetParent, UWidget* SourceWidget,
        const TSharedPtr<FJsonObject>& Placement, FString& OutError);

    bool AttachToParentOrRoot(UWidgetBlueprint* WidgetBP, UWidget* ChildWidget, const FString& ParentSlotName,
        const TSharedPtr<FJsonObject>& Placement = nullptr,
        int32* OutInsertIndex = nullptr,
        FString* OutErrorCode = nullptr,
        FString* OutError = nullptr);

    PINWRIGHT_API bool TryGetInvalidPanelSlotReason(UPanelWidget* Panel, int32 SlotIndex,
        UWidget* RemovedWidget, FString& OutReason);

    // Drops every null entry from Panel's Slots array; returns how many were removed.
    //
    // Null entries are reachable: Slots is a reflected UPROPERTY, so a caller-supplied property
    // payload can put one there. They must NOT be removed through UPanelWidget::RemoveChildAt,
    // which dereferences Slots[Index] before null-checking it through UE 5.3 (5.4 moved that body
    // inside an `if (PanelSlot)`); on 5.3 the deref is a process-killing access violation rather
    // than a refusal, and ClearChildren() walks straight into it. Compaction therefore goes
    // through the same reflected array the property writer used to create the entry.
    PINWRIGHT_API int32 RemoveNullPanelSlots(UPanelWidget* Panel);

    PINWRIGHT_API int32 CompactInvalidPanelSlots(UWidgetBlueprint* WidgetBP, UWidget* RemovedWidget = nullptr);

    void EnsureWidgetVariableGuid(UWidgetBlueprint* WidgetBP, const FName& VariableName);

    void RemoveWidgetVariableGuid(UWidgetBlueprint* WidgetBP, const FName& VariableName);

    void EnsureAllWidgetVariableGuids(UWidgetBlueprint* WidgetBP);

    // Collect (bindName, widgetTypeName) for every REQUIRED meta=(BindWidget) property on
    // WidgetBP's parent-class chain whose variable name is in AffectedNames — the names a
    // remove/rename is about to invalidate. Optional binds (BindWidgetOptional / OptionalWidget)
    // are excluded because they never fail compile. Mirrors the UMG compiler's own required-bind
    // check (FWidgetBlueprintEditorUtils::IsBindWidgetProperty + WidgetBlueprintCompiler.cpp), so
    // callers can warn up front that the next blueprint.compile would report
    // "A required widget binding \"X\" of type Y was not found."
    PINWRIGHT_API void CollectBrokenRequiredBinds(const UWidgetBlueprint* WidgetBP,
        const TSet<FName>& AffectedNames, TArray<TPair<FName, FString>>& OutBroken);

    // Copy safe editable FProperty values without clobbering widget hierarchy refs.
    void CopyMatchingProperties(UObject* Src, UObject* Dst);

    // Swap OldTarget's class to NewClass while preserving children and parent-slot
    // linkage. Non-root swaps go through UPanelWidget::ReplaceChild (which reuses
    // the parent's existing UPanelSlot, so anchors/offsets/alignment carry across
    // without a hand-copy). Root swaps rewrite WidgetTree->RootWidget. Returns the
    // replacement widget on success, nullptr on failure (fills OutError).
    PINWRIGHT_API UWidget* ReplaceWidgetClass(UWidgetBlueprint* WidgetBP, UWidget* OldTarget,
        UClass* NewClass, bool bPreserveProperties,
        bool& bOutWasRoot, FString* OutError = nullptr);
}
