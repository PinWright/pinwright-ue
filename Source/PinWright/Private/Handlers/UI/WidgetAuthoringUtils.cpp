// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "PinWrightHelpers.h"
#include "Utils/PropertyUtils.h"
#include "Compat/EngineVersionCompat.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogWidgetAuthoringUtils, Log, All);

namespace WidgetAuthoringHelpers
{
    FLinearColor GetColorFromJsonWidget(const TSharedPtr<FJsonObject>& ColorObj, const FLinearColor& Default)
    {
        if (!ColorObj.IsValid())
        {
            return Default;
        }
        FLinearColor Color = Default;
        Color.R = ColorObj->HasField(TEXT("r")) ? GetJsonNumberField(ColorObj, TEXT("r")) : Default.R;
        Color.G = ColorObj->HasField(TEXT("g")) ? GetJsonNumberField(ColorObj, TEXT("g")) : Default.G;
        Color.B = ColorObj->HasField(TEXT("b")) ? GetJsonNumberField(ColorObj, TEXT("b")) : Default.B;
        Color.A = ColorObj->HasField(TEXT("a")) ? GetJsonNumberField(ColorObj, TEXT("a")) : Default.A;
        return Color;
    }

    TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName)
    {
        if (Payload.IsValid() && Payload->HasTypedField<EJson::Object>(FieldName))
        {
            return Payload->GetObjectField(FieldName);
        }
        return nullptr;
    }

    const TArray<TSharedPtr<FJsonValue>>* GetArrayField(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName)
    {
        if (Payload.IsValid() && Payload->HasTypedField<EJson::Array>(FieldName))
        {
            return &Payload->GetArrayField(FieldName);
        }
        return nullptr;
    }

    UWidgetBlueprint* LoadWidgetBlueprint(const FString& WidgetPath)
    {
        FString Path = WidgetPath;

        // A "//" ANYWHERE IN THE INPUT IS AN EDITOR KILL, so it is refused here beside the _C
        // rejection below, above every resolution step. The two StaticLoadObject calls at the end
        // of this function reach CreatePackage's Fatal (StaticLoadObjectInternal ->
        // ResolveName2(..., Create=true) -> CreatePackage on the partial name), and Fatal is not
        // compiled out in any configuration: it ends the PROCESS, so no caller of this function
        // can defend itself with a null check - nothing after the call runs. The steps in between
        // are all safe (FindObject and FindPackage resolve with Create=false, the TObjectIterator
        // walk and the asset-registry GetAsset only touch what is already known), which is exactly
        // why the guard cannot sit lower: a malformed path simply misses all of them and falls
        // through to the two that are lethal.
        //
        // Refusal is a null return plus one Warning. That is this function's established contract
        // - the _C rejection directly below already answers a malformed shape the same way - and
        // it is what the 27 call sites across the 28 widget.* verbs already handle; adding an
        // out-error to all of them is a separate ticket. Warning, never Error: a malformed
        // argument is a refusal, not a plugin fault, and bElevateLogWarningsToErrors would turn an
        // Error into a test failure. Board B-createpackage-unvalidated-paths-plugin-wide.
        if (CanReachCreatePackageFatal(Path))
        {
            UE_LOG(LogWidgetAuthoringUtils, Warning,
                TEXT("LoadWidgetBlueprint refused '%s': a widget path may not contain '//'."),
                *WidgetPath);
            return nullptr;
        }

        // Reject _C class paths
        if (Path.EndsWith(TEXT("_C")))
        {
            return nullptr;
        }

        // Normalize: ensure starts with /Game/ or /
        if (!Path.StartsWith(TEXT("/")))
        {
            Path = TEXT("/Game/") + Path;
        }

        // Build object path and package path
        FString ObjectPath = Path;
        FString PackagePath = Path;

        if (Path.Contains(TEXT(".")))
        {
            // Already has object path format, extract package path
            PackagePath = Path.Left(Path.Find(TEXT(".")));
        }
        else
        {
            // Add .Name suffix for object path
            FString AssetName = FPaths::GetBaseFilename(Path);
            ObjectPath = Path + TEXT(".") + AssetName;
        }

        FString AssetName = FPaths::GetBaseFilename(PackagePath);

        // Method 1: FindObject with full object path (fastest for in-memory)
        if (UWidgetBlueprint* WB = FindObject<UWidgetBlueprint>(nullptr, *ObjectPath))
        {
            return WB;
        }

        // Method 2: Find package first, then find asset within it
        if (UPackage* Package = FindPackage(nullptr, *PackagePath))
        {
            if (UWidgetBlueprint* WB = FindObject<UWidgetBlueprint>(Package, *AssetName))
            {
                return WB;
            }
        }

        // Method 3: TObjectIterator fallback - iterate all widget blueprints to find by path
        for (TObjectIterator<UWidgetBlueprint> It; It; ++It)
        {
            UWidgetBlueprint* WB = *It;
            if (WB)
            {
                FString WBPath = WB->GetPathName();
                if (WBPath.Equals(ObjectPath, ESearchCase::IgnoreCase) ||
                    WBPath.Equals(PackagePath, ESearchCase::IgnoreCase) ||
                    WBPath.Equals(Path, ESearchCase::IgnoreCase))
                {
                    return WB;
                }
                FString WBPackagePath = WBPath;
                if (WBPackagePath.Contains(TEXT(".")))
                {
                    WBPackagePath = WBPackagePath.Left(WBPackagePath.Find(TEXT(".")));
                }
                if (WBPackagePath.Equals(PackagePath, ESearchCase::IgnoreCase))
                {
                    return WB;
                }
            }
        }

        // Method 4: Asset Registry lookup
        IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
        FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
        if (AssetData.IsValid())
        {
            if (UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(AssetData.GetAsset()))
            {
                return WB;
            }
        }

        // Method 5: StaticLoadObject with object path (for disk assets)
        if (UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *ObjectPath)))
        {
            return WB;
        }

        // Method 6: StaticLoadObject with package path
        return Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *PackagePath));
    }

    bool TryParseVisibility(const FString& In, ESlateVisibility& Out)
    {
        // Single source of truth for visibility-string parsing. GetVisibility()
        // delegates here and falls back to Visible on miss for back-compat.
        struct FEntry { const TCHAR* Name; ESlateVisibility Value; };
        static const FEntry Table[] = {
            { TEXT("Visible"),              ESlateVisibility::Visible },
            { TEXT("Collapsed"),            ESlateVisibility::Collapsed },
            { TEXT("Hidden"),               ESlateVisibility::Hidden },
            { TEXT("HitTestInvisible"),     ESlateVisibility::HitTestInvisible },
            { TEXT("SelfHitTestInvisible"), ESlateVisibility::SelfHitTestInvisible },
        };
        for (const FEntry& E : Table)
        {
            if (In.Equals(E.Name, ESearchCase::IgnoreCase))
            {
                Out = E.Value;
                return true;
            }
        }
        return false;
    }

    ESlateVisibility GetVisibility(const FString& VisibilityStr)
    {
        ESlateVisibility Parsed = ESlateVisibility::Visible;
        TryParseVisibility(VisibilityStr, Parsed);
        return Parsed;
    }

    FString VisibilityToString(ESlateVisibility Visibility)
    {
        switch (Visibility)
        {
        case ESlateVisibility::Collapsed:
            return TEXT("Collapsed");
        case ESlateVisibility::Hidden:
            return TEXT("Hidden");
        case ESlateVisibility::HitTestInvisible:
            return TEXT("HitTestInvisible");
        case ESlateVisibility::SelfHitTestInvisible:
            return TEXT("SelfHitTestInvisible");
        case ESlateVisibility::Visible:
        default:
            return TEXT("Visible");
        }
    }

    void AddWidgetToParentOrRoot(UWidgetBlueprint* WidgetBP, UWidget* NewWidget, const FString& ParentSlot, bool bSetAsRootIfEmpty)
    {
        if (!WidgetBP || !NewWidget || !WidgetBP->WidgetTree) return;

        bool bAdded = false;
        if (ParentSlot.IsEmpty())
        {
            if (bSetAsRootIfEmpty)
            {
                if (!WidgetBP->WidgetTree->RootWidget)
                {
                    WidgetBP->WidgetTree->RootWidget = NewWidget;
                    bAdded = true;
                }
            }
        }
        else
        {
            UWidget* ParentWidget = WidgetBP->WidgetTree->FindWidget(FName(*ParentSlot));
            if (ParentWidget)
            {
                UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
                if (ParentPanel)
                {
                    ParentPanel->AddChild(NewWidget);
                    bAdded = true;
                }
            }
        }

        if (bAdded)
        {
            EnsureWidgetVariableGuid(WidgetBP, NewWidget->GetFName());
        }
    }

    UClass* ResolveWidgetClass(const FString& ClassName)
    {
        UClass* Class = FindFirstObjectSafe<UClass>(*ClassName);
        if (Class) return Class;

        Class = FindFirstObjectSafe<UClass>(*(TEXT("U") + ClassName));
        if (Class) return Class;

        Class = FindFirstObjectSafe<UClass>(*(ClassName + TEXT("_C")));
        if (Class) return Class;

        static const TCHAR* Prefixes[] = {
            TEXT("/Script/UMG."),
            TEXT("/Script/SlateCore."),
            TEXT("/Script/Slate."),
            TEXT("/Script/Engine.")
        };
        for (const TCHAR* Prefix : Prefixes)
        {
            Class = FindObject<UClass>(nullptr, *(FString(Prefix) + ClassName));
            if (Class) return Class;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName().Equals(ClassName, ESearchCase::IgnoreCase) ||
                It->GetName().Equals(TEXT("U") + ClassName, ESearchCase::IgnoreCase))
            {
                return *It;
            }
        }

        // Final fallback: search the asset registry for a WidgetBlueprint by short name.
        // This handles Blueprint widget classes (e.g. W_AnimatedButton) that haven't been
        // loaded yet — their generated class only exists after the asset is loaded.
        {
            IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
            if (AssetRegistry.IsSearchAsync())
            {
                AssetRegistry.WaitForCompletion();
            }
            TArray<FAssetData> Assets;
            FARFilter Filter;
            Filter.bRecursiveClasses = true;
            Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
            AssetRegistry.GetAssets(Filter, Assets);

            // Strip _C suffix if present so we match the BP asset name, not the generated class name
            FString BaseName = ClassName.EndsWith(TEXT("_C")) ? ClassName.LeftChop(2) : ClassName;

            for (const FAssetData& Asset : Assets)
            {
                if (Asset.AssetName.ToString().Equals(BaseName, ESearchCase::IgnoreCase))
                {
                    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset.GetAsset());
                    if (WBP && WBP->GeneratedClass)
                        return WBP->GeneratedClass;
                }
            }
        }

        UE_LOG(LogWidgetAuthoringUtils, Warning, TEXT("ResolveWidgetClass: failed to resolve '%s'"), *ClassName);
        return nullptr;
    }

    void DiscardConstructedWidgetForAuthoring(UWidgetBlueprint* WidgetBP, UWidget* Widget)
    {
        if (!Widget)
        {
            return;
        }

        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            TArray<UWidget*> Children;
            const int32 ChildCount = Panel->GetChildrenCount();
            Children.Reserve(ChildCount);
            for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
            {
                Children.Add(Panel->GetChildAt(ChildIndex));
            }

            for (UWidget* Child : Children)
            {
                DiscardConstructedWidgetForAuthoring(WidgetBP, Child);
            }
        }

        if (WidgetBP && WidgetBP->WidgetTree)
        {
            WidgetBP->WidgetTree->RemoveWidget(Widget);
        }

        Widget->Rename(nullptr, GetTransientPackage(),
            REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);
    }

    UWidget* ConstructWidgetForAuthoring(UWidgetBlueprint* WidgetBP, UClass* WidgetClass,
        const FName& WidgetName, FString* OutError)
    {
        if (OutError)
        {
            OutError->Reset();
        }

        if (!WidgetBP || !WidgetBP->WidgetTree)
        {
            if (OutError)
            {
                *OutError = TEXT("Widget blueprint has no WidgetTree");
            }
            return nullptr;
        }

        if (!WidgetClass)
        {
            if (OutError)
            {
                *OutError = TEXT("Widget class is null");
            }
            return nullptr;
        }

        if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
        {
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("Class '%s' is not a UWidget subclass"),
                    *WidgetClass->GetName());
            }
            return nullptr;
        }

        if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
        {
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("Cannot instantiate abstract widget class '%s'"),
                    *WidgetClass->GetName());
            }
            return nullptr;
        }

        if (WidgetClass->IsChildOf(UUserWidget::StaticClass()))
        {
            UUserWidget* UserWidget = WidgetBP->WidgetTree->ConstructWidget<UUserWidget>(
                WidgetClass, WidgetName);
            if (!UserWidget)
            {
                if (OutError)
                {
                    *OutError = FString::Printf(TEXT("Failed to construct user widget of type '%s'"),
                        *WidgetClass->GetName());
                }
                return nullptr;
            }

            if (!UserWidget->WidgetTree)
            {
                DiscardConstructedWidgetForAuthoring(WidgetBP, UserWidget);
                if (OutError)
                {
                    *OutError = FString::Printf(
                        TEXT("Constructed user widget '%s' without an initialized WidgetTree"),
                        *WidgetClass->GetName());
                }
                return nullptr;
            }

            return UserWidget;
        }

        UWidget* Widget = WidgetBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
        if (!Widget)
        {
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("Failed to construct widget of type '%s'"),
                    *WidgetClass->GetName());
            }
            return nullptr;
        }

        return Widget;
    }

    namespace
    {
        // Each touched widget owns at most one FTransientDesignerOverride that may
        // carry a hidden override, a visibility override, or both.
        FTransientDesignerOverride& GetOrAddEntry(FTransientDesignerOverrides& Guard, UWidget* W)
        {
            for (FTransientDesignerOverride& E : Guard.Entries)
            {
                if (E.Widget.Get() == W)
                {
                    return E;
                }
            }
            FTransientDesignerOverride NewEntry;
            NewEntry.Widget = W;
            const int32 Idx = Guard.Entries.Add(NewEntry);
            return Guard.Entries[Idx];
        }

        bool ResolveOrError(UUserWidget* PreviewRoot, const FString& Name, UWidget*& OutWidget, FString& OutError)
        {
            OutWidget = PreviewRoot->GetWidgetFromName(FName(*Name));
            if (!OutWidget)
            {
                OutError = FString::Printf(TEXT("Widget not found in preview: %s"), *Name);
                return false;
            }
            return true;
        }
    }

    bool ApplyTransientDesignerOverrides(
        UUserWidget* PreviewRoot,
        const TArray<FString>& ShowOnly,
        const TArray<FString>& Hide,
        const TMap<FString, ESlateVisibility>& VisibilityOverrides,
        FTransientDesignerOverrides& OutGuard,
        FString& OutError)
    {
        OutError.Reset();
        if (!PreviewRoot)
        {
            OutError = TEXT("Preview UserWidget is null");
            return false;
        }

        // Resolve every input name up-front against the preview tree, before we mutate
        // anything. This way an unknown name fails the call cleanly with no overrides
        // applied — the caller's ON_SCOPE_EXIT will still see an empty guard.
        TArray<UWidget*> ShowOnlyResolved;
        ShowOnlyResolved.Reserve(ShowOnly.Num());
        for (const FString& Name : ShowOnly)
        {
            UWidget* W = nullptr;
            if (!ResolveOrError(PreviewRoot, Name, W, OutError))
            {
                return false;
            }
            ShowOnlyResolved.Add(W);
        }

        TArray<UWidget*> HideResolved;
        HideResolved.Reserve(Hide.Num());
        for (const FString& Name : Hide)
        {
            UWidget* W = nullptr;
            if (!ResolveOrError(PreviewRoot, Name, W, OutError))
            {
                return false;
            }
            HideResolved.Add(W);
        }

        TArray<TPair<UWidget*, ESlateVisibility>> VisibilityResolved;
        VisibilityResolved.Reserve(VisibilityOverrides.Num());
        for (const TPair<FString, ESlateVisibility>& Pair : VisibilityOverrides)
        {
            UWidget* W = nullptr;
            if (!ResolveOrError(PreviewRoot, Pair.Key, W, OutError))
            {
                return false;
            }
            VisibilityResolved.Emplace(W, Pair.Value);
        }

        // Build KeepSet = ⋃ {self ∪ ancestors} so showOnly targets and their ancestors
        // remain designer-eye-visible while every other widget under the preview root
        // gets eye-hidden.
        TSet<TWeakObjectPtr<UWidget>> KeepSet;
        for (UWidget* W : ShowOnlyResolved)
        {
            UWidget* Cursor = W;
            while (Cursor)
            {
                KeepSet.Add(Cursor);
                Cursor = Cursor->GetParent();
            }
        }

        // Walk the entire preview tree once and plan per-widget hidden overrides.
        // GetWidgetFromName doesn't iterate, so we go through the WidgetTree directly.
        if (UWidgetTree* PreviewTree = PreviewRoot->WidgetTree)
        {
            const bool bShowOnlyActive = ShowOnlyResolved.Num() > 0;
            PreviewTree->ForEachWidget([&](UWidget* W)
            {
                if (!W) return;
                // Don't eye-hide the preview root itself, even if showOnly is set.
                if (bShowOnlyActive && W != PreviewRoot && !KeepSet.Contains(W))
                {
                    FTransientDesignerOverride& Entry = GetOrAddEntry(OutGuard, W);
                    if (!Entry.bHadHiddenOverride)
                    {
                        Entry.bHadHiddenOverride = true;
                        Entry.bOriginalHidden = W->bHiddenInDesigner;
                    }
                }
            });
        }

        // Stack explicit `hide` on top of any showOnly-driven hides.
        for (UWidget* W : HideResolved)
        {
            FTransientDesignerOverride& Entry = GetOrAddEntry(OutGuard, W);
            if (!Entry.bHadHiddenOverride)
            {
                Entry.bHadHiddenOverride = true;
                Entry.bOriginalHidden = W->bHiddenInDesigner;
            }
        }

        // Plan visibility overrides on the same per-widget entries (snapshot only;
        // the new values are written through from VisibilityResolved below).
        for (const TPair<UWidget*, ESlateVisibility>& Pair : VisibilityResolved)
        {
            FTransientDesignerOverride& Entry = GetOrAddEntry(OutGuard, Pair.Key);
            if (!Entry.bHadVisibilityOverride)
            {
                Entry.bHadVisibilityOverride = true;
                Entry.OriginalVisibility = Pair.Key->GetVisibility();
            }
        }

        for (FTransientDesignerOverride& Entry : OutGuard.Entries)
        {
            if (UWidget* W = Entry.Widget.Get())
            {
                if (Entry.bHadHiddenOverride)
                {
                    W->bHiddenInDesigner = true;
                }
            }
        }
        for (const TPair<UWidget*, ESlateVisibility>& Pair : VisibilityResolved)
        {
            Pair.Key->SetVisibility(Pair.Value);
        }

        return true;
    }

    void RevertTransientDesignerOverrides(const FTransientDesignerOverrides& Guard)
    {
        // Iterate in reverse so any ordering effects (parent-vs-child eye flags,
        // visibility cascading) unwind in the same order they were applied.
        for (int32 Index = Guard.Entries.Num() - 1; Index >= 0; --Index)
        {
            const FTransientDesignerOverride& Entry = Guard.Entries[Index];
            UWidget* W = Entry.Widget.Get();
            if (!W)
            {
                continue;
            }
            if (Entry.bHadVisibilityOverride)
            {
                W->SetVisibility(Entry.OriginalVisibility);
            }
            if (Entry.bHadHiddenOverride)
            {
                W->bHiddenInDesigner = Entry.bOriginalHidden;
            }
        }
    }

    int32 FTransientDesignerOverrides::NumHiddenOverrides() const
    {
        int32 Count = 0;
        for (const FTransientDesignerOverride& E : Entries)
        {
            if (E.bHadHiddenOverride) ++Count;
        }
        return Count;
    }

    int32 FTransientDesignerOverrides::NumVisibilityOverrides() const
    {
        int32 Count = 0;
        for (const FTransientDesignerOverride& E : Entries)
        {
            if (E.bHadVisibilityOverride) ++Count;
        }
        return Count;
    }

    UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBP, const FString& WidgetName)
    {
        if (!WidgetBP || !WidgetBP->WidgetTree)
        {
            return nullptr;
        }

        UWidget* FoundWidget = nullptr;
        WidgetBP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            if (W && W->GetFName().ToString().Equals(WidgetName, ESearchCase::IgnoreCase))
            {
                FoundWidget = W;
            }
        });
        return FoundWidget;
    }

    UWidgetAnimation* FindAnimationByName(UWidgetBlueprint* WidgetBP, const FString& AnimationName)
    {
        if (!WidgetBP)
        {
            return nullptr;
        }

        for (UWidgetAnimation* Anim : WidgetBP->Animations)
        {
            if (Anim && (
                Anim->GetName().Equals(AnimationName, ESearchCase::IgnoreCase) ||
                Anim->GetFName().ToString().Equals(AnimationName, ESearchCase::IgnoreCase)))
            {
                return Anim;
            }
        }
        return nullptr;
    }

    namespace
    {
        // Free InName under InOuter so a freshly constructed object can take it. UObject
        // construction over an occupied name recycles the occupant in place, which would carry
        // state from a discarded animation into a new one; moving the occupant to the transient
        // package leaves the name clear. Redirectors are suppressed because these are inner
        // subobjects that nothing references by path from another package.
        // Distinctive name: this translation unit is merged with its neighbours under Unity.
        void EvictWidgetAnimationInnerName(UObject* InOuter, const FName InName, const UObject* Keep)
        {
            UObject* Occupant = StaticFindObjectFastSafe(UObject::StaticClass(), InOuter, InName);
            if (Occupant && Occupant != Keep)
            {
                Occupant->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
            }
        }
    }

    FAnimationMovieSceneResult EnsureAnimationMovieScene(UWidgetAnimation* Animation)
    {
        FAnimationMovieSceneResult Result;
        if (!Animation)
        {
            return Result;
        }

        // The animation's own FName is the identity the compiler generates the property from,
        // so it — never the caller's requested string — is what the MovieScene must be named.
        // They differ whenever object construction had to uniquify the requested name.
        const FName DesiredName = Animation->GetFName();
        UMovieScene* MovieScene = Animation->GetMovieScene();

        if (!MovieScene)
        {
            EvictWidgetAnimationInnerName(Animation, DesiredName, nullptr);
            MovieScene = NewObject<UMovieScene>(Animation, DesiredName, RF_Transactional);
            Animation->MovieScene = MovieScene;
            Result.bCreated = MovieScene != nullptr;
        }
        else if (MovieScene->GetFName() != DesiredName)
        {
            // Repair an animation minted before this invariant was enforced. Mirrors the
            // engine's rename path, which moves the animation and its MovieScene together.
            EvictWidgetAnimationInnerName(Animation, DesiredName, MovieScene);
            MovieScene->Modify();
            Result.bRenamed = MovieScene->Rename(*DesiredName.ToString(), nullptr, REN_DontCreateRedirectors);
        }

        Result.MovieScene = MovieScene;
        return Result;
    }

    bool IsDescendantWidget(UWidget* CandidateParent, UWidget* PossibleDescendant)
    {
        UPanelWidget* Panel = Cast<UPanelWidget>(CandidateParent);
        if (!Panel || !PossibleDescendant)
        {
            return false;
        }

        for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
        {
            UWidget* Child = Panel->GetChildAt(ChildIndex);
            if (Child == PossibleDescendant || IsDescendantWidget(Child, PossibleDescendant))
            {
                return true;
            }
        }
        return false;
    }

    int32 ResolveWidgetInsertIndex(UWidgetBlueprint* WidgetBP, UPanelWidget* TargetParent, UWidget* SourceWidget,
        const TSharedPtr<FJsonObject>& Placement, FString& OutError)
    {
        OutError.Reset();
        if (!TargetParent)
        {
            OutError = TEXT("Target parent is not a panel");
            return INDEX_NONE;
        }

        if (Placement.IsValid())
        {
            double JsonIndex = 0.0;
            if (Placement->TryGetNumberField(TEXT("index"), JsonIndex))
            {
                return FMath::Clamp(static_cast<int32>(JsonIndex), 0, TargetParent->GetChildrenCount());
            }

            FString AfterName;
            if (Placement->TryGetStringField(TEXT("after"), AfterName))
            {
                UWidget* AfterWidget = FindWidgetByName(WidgetBP, AfterName);
                const int32 AfterIndex = TargetParent->GetChildIndex(AfterWidget);
                if (AfterIndex == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Placement 'after' widget '%s' is not a child of the target parent"), *AfterName);
                    return INDEX_NONE;
                }
                return AfterIndex + 1;
            }

            FString BeforeName;
            if (Placement->TryGetStringField(TEXT("before"), BeforeName))
            {
                UWidget* BeforeWidget = FindWidgetByName(WidgetBP, BeforeName);
                const int32 BeforeIndex = TargetParent->GetChildIndex(BeforeWidget);
                if (BeforeIndex == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Placement 'before' widget '%s' is not a child of the target parent"), *BeforeName);
                    return INDEX_NONE;
                }
                return BeforeIndex;
            }
        }

        if (SourceWidget && SourceWidget->GetParent() == TargetParent)
        {
            const int32 SourceIndex = TargetParent->GetChildIndex(SourceWidget);
            if (SourceIndex != INDEX_NONE)
            {
                return SourceIndex + 1;
            }
        }

        return TargetParent->GetChildrenCount();
    }

    bool AttachToParentOrRoot(UWidgetBlueprint* WidgetBP, UWidget* ChildWidget, const FString& ParentSlotName,
        const TSharedPtr<FJsonObject>& Placement,
        int32* OutInsertIndex,
        FString* OutErrorCode,
        FString* OutError)
    {
        auto SetError = [&](const TCHAR* Code, const FString& Message)
        {
            if (OutErrorCode) { *OutErrorCode = Code; }
            if (OutError)     { *OutError = Message; }
        };

        if (OutError)     { OutError->Reset(); }
        if (OutErrorCode) { OutErrorCode->Reset(); }
        if (OutInsertIndex) { *OutInsertIndex = INDEX_NONE; }

        if (!WidgetBP || !WidgetBP->WidgetTree || !ChildWidget)
        {
            SetError(TEXT("INVALID_STATE"), TEXT("Invalid widget blueprint or child widget"));
            return false;
        }

        UPanelWidget* TargetPanel = nullptr;
        if (!ParentSlotName.IsEmpty())
        {
            UWidget* ParentWidget = WidgetBP->WidgetTree->FindWidget(FName(*ParentSlotName));
            if (!ParentWidget)
            {
                SetError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Parent widget '%s' not found"), *ParentSlotName));
                return false;
            }

            TargetPanel = Cast<UPanelWidget>(ParentWidget);
            if (!TargetPanel)
            {
                SetError(TEXT("INVALID_PARENT"), FString::Printf(TEXT("Parent widget '%s' is not a panel"), *ParentSlotName));
                return false;
            }
        }
        else
        {
            UWidget* RootWidget = WidgetBP->WidgetTree->RootWidget;
            if (!RootWidget)
            {
                UCanvasPanel* NewRoot = WidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
                WidgetBP->WidgetTree->RootWidget = NewRoot;
                EnsureWidgetVariableGuid(WidgetBP, NewRoot->GetFName());
                RootWidget = NewRoot;
            }

            TargetPanel = Cast<UPanelWidget>(RootWidget);
            if (!TargetPanel)
            {
                SetError(TEXT("INVALID_PARENT"), TEXT("Root widget is not a panel; specify parentSlot"));
                return false;
            }
        }

        if (ChildWidget == TargetPanel)
        {
            SetError(TEXT("INVALID_PARENT"), TEXT("Cannot add widget as child of itself"));
            return false;
        }

        FString PlacementError;
        const int32 InsertIndex = ResolveWidgetInsertIndex(WidgetBP, TargetPanel, nullptr, Placement, PlacementError);
        if (!PlacementError.IsEmpty())
        {
            SetError(TEXT("INVALID_PLACEMENT"), PlacementError);
            return false;
        }

        UPanelSlot* AddedSlot = TargetPanel->InsertChildAt(InsertIndex, ChildWidget);
        if (!AddedSlot)
        {
            SetError(TEXT("ATTACH_FAILED"), TEXT("Failed to attach widget to parent panel"));
            return false;
        }

        if (OutInsertIndex)
        {
            *OutInsertIndex = TargetPanel->GetChildIndex(ChildWidget);
        }
        EnsureWidgetVariableGuid(WidgetBP, ChildWidget->GetFName());
        return true;
    }

    bool TryGetInvalidPanelSlotReason(UPanelWidget* Panel, int32 SlotIndex, UWidget* RemovedWidget, FString& OutReason)
    {
        OutReason.Reset();
        if (!Panel)
        {
            OutReason = TEXT("panel is null");
            return true;
        }

        const TArray<UPanelSlot*>& Slots = Panel->GetSlots();
        if (!Slots.IsValidIndex(SlotIndex))
        {
            OutReason = TEXT("slot index is out of range");
            return true;
        }

        UPanelSlot* Slot = Slots[SlotIndex];
        if (!Slot)
        {
            OutReason = TEXT("slot is null");
            return true;
        }

        UWidget* Content = Slot->Content;
        if (!Content)
        {
            OutReason = TEXT("slot Content is null");
            return true;
        }

        if (RemovedWidget && Content == RemovedWidget)
        {
            OutReason = FString::Printf(TEXT("slot Content is removed widget '%s'"), *Content->GetName());
            return true;
        }

        if (Slot->Parent != Panel)
        {
            OutReason = FString::Printf(TEXT("slot Parent is '%s', expected panel"),
                Slot->Parent ? *Slot->Parent->GetName() : TEXT("<null>"));
            return true;
        }

        if (Content->Slot != Slot)
        {
            OutReason = FString::Printf(TEXT("content '%s' Slot back-reference does not point at this slot"),
                *Content->GetName());
            return true;
        }

        return false;
    }

    int32 RemoveNullPanelSlots(UPanelWidget* Panel)
    {
        if (!Panel)
        {
            return 0;
        }

        FArrayProperty* SlotsProperty =
            FindFProperty<FArrayProperty>(UPanelWidget::StaticClass(), TEXT("Slots"));
        FObjectPropertyBase* SlotElementProperty =
            SlotsProperty ? CastField<FObjectPropertyBase>(SlotsProperty->Inner) : nullptr;
        if (!SlotElementProperty)
        {
            return 0;
        }

        FScriptArrayHelper SlotsHelper(
            SlotsProperty, SlotsProperty->ContainerPtrToValuePtr<void>(Panel));

        int32 RemovedCount = 0;
        for (int32 SlotIndex = SlotsHelper.Num() - 1; SlotIndex >= 0; --SlotIndex)
        {
            if (SlotElementProperty->GetObjectPropertyValue(SlotsHelper.GetRawPtr(SlotIndex)) != nullptr)
            {
                continue;
            }
            if (RemovedCount == 0)
            {
                Panel->Modify();
            }
            SlotsHelper.RemoveValues(SlotIndex, 1);
            ++RemovedCount;
        }
        return RemovedCount;
    }

    int32 CompactInvalidPanelSlots(UWidgetBlueprint* WidgetBP, UWidget* RemovedWidget)
    {
        if (!WidgetBP || !WidgetBP->WidgetTree)
        {
            return 0;
        }

        TArray<UPanelWidget*> Panels;
        WidgetBP->WidgetTree->ForEachWidget([&Panels](UWidget* Widget)
        {
            if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
            {
                Panels.Add(Panel);
            }
        });

        int32 CompactedCount = 0;
        for (UPanelWidget* Panel : Panels)
        {
            if (!Panel)
            {
                continue;
            }

            // Ahead of the per-slot loop, because the loop's removal step is RemoveChildAt and
            // that call cannot survive a null entry on UE 5.3. See RemoveNullPanelSlots.
            CompactedCount += RemoveNullPanelSlots(Panel);

            for (int32 SlotIndex = Panel->GetSlots().Num() - 1; SlotIndex >= 0; --SlotIndex)
            {
                const TArray<UPanelSlot*>& Slots = Panel->GetSlots();
                UPanelSlot* Slot = Slots.IsValidIndex(SlotIndex) ? Slots[SlotIndex] : nullptr;
                UWidget* Content = Slot ? Slot->Content : nullptr;

                FString InvalidReason;
                if (!TryGetInvalidPanelSlotReason(Panel, SlotIndex, RemovedWidget, InvalidReason))
                {
                    continue;
                }

                Panel->Modify();
                if (Slot)
                {
                    Slot->Modify();
                }
                if (Content)
                {
                    Content->Modify();
                }
                const bool bPreserveContentSlot = Content && Content != RemovedWidget && Content->Slot != Slot;
                UPanelSlot* OriginalContentSlot = bPreserveContentSlot ? Content->Slot : nullptr;
                if (bPreserveContentSlot)
                {
                    Slot->Content = nullptr;
                }

                const int32 SlotsBeforeRemove = Panel->GetSlots().Num();
                const bool bRemoved = Panel->RemoveChildAt(SlotIndex);
                const bool bSlotWasRemoved = bRemoved || Panel->GetSlots().Num() < SlotsBeforeRemove;
                if (bPreserveContentSlot && Content && Content->Slot != OriginalContentSlot)
                {
                    Content->Slot = OriginalContentSlot;
                }
                if (!bSlotWasRemoved)
                {
                    if (bPreserveContentSlot && Slot)
                    {
                        Slot->Content = Content;
                    }
                    continue;
                }

                ++CompactedCount;
            }
        }

        return CompactedCount;
    }

    void EnsureWidgetVariableGuid(UWidgetBlueprint* WidgetBP, const FName& VariableName)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        // UE 5.4/5.5: UWidgetBlueprint has no WidgetVariableNameToGuidMap / OnVariableAdded.
        // The pre-5.6 compiler generates a variable purely from UWidget::bIsVariable, so the
        // GUID-registration step has no pre-5.6 equivalent and is a clean no-op here.
        (void)WidgetBP;
        (void)VariableName;
#else
        if (WidgetBP && !VariableName.IsNone() && !WidgetBP->WidgetVariableNameToGuidMap.Contains(VariableName))
        {
            WidgetBP->OnVariableAdded(VariableName);
        }
#endif
    }

    void RemoveWidgetVariableGuid(UWidgetBlueprint* WidgetBP, const FName& VariableName)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        // UE 5.4/5.5: no GUID map to prune; nothing to do.
        (void)WidgetBP;
        (void)VariableName;
#else
        if (WidgetBP && !VariableName.IsNone())
        {
            WidgetBP->WidgetVariableNameToGuidMap.Remove(VariableName);
        }
#endif
    }

    void EnsureAllWidgetVariableGuids(UWidgetBlueprint* WidgetBP)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        // UE 5.4/5.5: no WidgetVariableNameToGuidMap to populate or prune; the compiler
        // tracks variables via bIsVariable alone, so reconciliation is a no-op.
        (void)WidgetBP;
#else
        if (!WidgetBP || !WidgetBP->WidgetTree) return;

        TSet<FName> LiveNames;

        TFunction<void(UWidget*)> VisitWidget = [&](UWidget* Widget)
        {
            if (!Widget)
            {
                return;
            }

            LiveNames.Add(Widget->GetFName());
            EnsureWidgetVariableGuid(WidgetBP, Widget->GetFName());

            if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
            {
                const int32 ChildCount = Panel->GetChildrenCount();
                for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
                {
                    VisitWidget(Panel->GetChildAt(ChildIndex));
                }
            }
        };

        VisitWidget(WidgetBP->WidgetTree->RootWidget);

        for (UWidgetAnimation* Anim : WidgetBP->Animations)
        {
            if (Anim)
            {
                LiveNames.Add(Anim->GetFName());
            }
        }

        TArray<FName> OrphanedKeys;
        for (auto& Pair : WidgetBP->WidgetVariableNameToGuidMap)
        {
            if (!LiveNames.Contains(Pair.Key))
            {
                OrphanedKeys.Add(Pair.Key);
            }
        }
        for (const FName& Key : OrphanedKeys)
        {
            WidgetBP->WidgetVariableNameToGuidMap.Remove(Key);
        }
#endif
    }

    void CollectBrokenRequiredBinds(const UWidgetBlueprint* WidgetBP,
        const TSet<FName>& AffectedNames, TArray<TPair<FName, FString>>& OutBroken)
    {
        if (!WidgetBP || !WidgetBP->ParentClass || AffectedNames.Num() == 0) return;

        for (TFieldIterator<FObjectPropertyBase> PropIt(WidgetBP->ParentClass); PropIt; ++PropIt)
        {
            FObjectPropertyBase* ObjProp = *PropIt;
            if (!ObjProp || !ObjProp->PropertyClass) continue;
            if (!ObjProp->PropertyClass->IsChildOf(UWidget::StaticClass())) continue;

            // Required BindWidget only; the optional variants never fail compile. Defer to the
            // UMG compiler's own required-bind check so this advisory can never drift from the
            // engine's bind-widget metadata contract.
            bool bIsOptional = false;
            if (!FWidgetBlueprintEditorUtils::IsBindWidgetProperty(ObjProp, bIsOptional) || bIsOptional) continue;

            const FName BindName = ObjProp->GetFName();
            if (AffectedNames.Contains(BindName))
            {
                OutBroken.Emplace(BindName, ObjProp->PropertyClass->GetName());
            }
        }
    }

    void CopyMatchingProperties(UObject* Src, UObject* Dst)
    {
        FFilteredPropertyCopyOptions Options;
        Options.SkipPropertyNames.Add(FName(TEXT("Slot")));
        Options.SkipPropertyNames.Add(FName(TEXT("Parent")));
        Options.SkipPropertyNames.Add(FName(TEXT("Content")));
        CopyFilteredMatchingProperties(Src, Dst, Options);
    }

    UWidget* ReplaceWidgetClass(UWidgetBlueprint* WidgetBP, UWidget* OldTarget,
        UClass* NewClass, bool bPreserveProperties,
        bool& bOutWasRoot, FString* OutError)
    {
        bOutWasRoot = false;
        if (OutError)
        {
            OutError->Reset();
        }

        if (!WidgetBP || !WidgetBP->WidgetTree || !OldTarget || !NewClass)
        {
            if (OutError)
            {
                *OutError = TEXT("Invalid widget blueprint, target widget, or class");
            }
            return nullptr;
        }

        if (!NewClass->IsChildOf(UWidget::StaticClass()))
        {
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("Class '%s' is not a UWidget subclass"),
                    *NewClass->GetName());
            }
            return nullptr;
        }

        UWidgetTree* Tree = WidgetBP->WidgetTree;
        const FName TargetName = OldTarget->GetFName();
        const bool bWasRoot = (Tree->RootWidget == OldTarget);
        bOutWasRoot = bWasRoot;

        UPanelWidget* OldParent = OldTarget->GetParent();
        if (!bWasRoot && !OldParent)
        {
            if (OutError)
            {
                *OutError = TEXT("Target widget is neither root nor parented; nothing to replace");
            }
            return nullptr;
        }

        // Snapshot children if the old target is a panel. We cache both the
        // child widgets and their existing slot instances so the new panel can
        // reuse compatible slot templates.
        UPanelWidget* OldPanel = Cast<UPanelWidget>(OldTarget);
        TArray<UWidget*> OldChildren;
        TArray<UPanelSlot*> OldChildSlots;
        if (OldPanel)
        {
            OldChildren = OldPanel->GetAllChildren();
            OldChildSlots.Reserve(OldChildren.Num());
            for (UWidget* Child : OldChildren)
            {
                OldChildSlots.Add(Child ? Child->Slot : nullptr);
            }
        }

        const FName TempName = MakeUniqueObjectName(Tree, NewClass,
            FName(*FString::Printf(TEXT("%s_NEW"), *TargetName.ToString())));
        FString ConstructionError;
        UWidget* NewWidget = ConstructWidgetForAuthoring(WidgetBP, NewClass, TempName, &ConstructionError);
        if (!NewWidget)
        {
            if (OutError)
            {
                *OutError = ConstructionError.IsEmpty()
                    ? FString::Printf(TEXT("Failed to construct widget of type '%s'"), *NewClass->GetName())
                    : ConstructionError;
            }
            return nullptr;
        }

        // Rename old target away so the replacement can claim the original name.
        const FString ScratchName = FString::Printf(TEXT("%s_REPLACED_%s"),
            *TargetName.ToString(),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OldTarget->Rename(*ScratchName, nullptr,
            REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);

        // Keep the original name so BP event-graph nodes that resolve widget
        // variables by FProperty name still bind after recompile.
        if (!NewWidget->Rename(*TargetName.ToString(), nullptr,
            REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS))
        {
            OldTarget->Rename(*TargetName.ToString(), nullptr,
                REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);
            DiscardConstructedWidgetForAuthoring(WidgetBP, NewWidget);
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("Failed to rename replacement widget to '%s'"),
                    *TargetName.ToString());
            }
            return nullptr;
        }

        // Detach children from the old panel only after replacement
        // construction has succeeded. RemoveChild clears their Slot pointer;
        // we've already captured slot templates above for re-use.
        if (OldPanel)
        {
            for (UWidget* Child : OldChildren)
            {
                if (Child)
                {
                    OldPanel->RemoveChild(Child);
                }
            }
        }

        if (bPreserveProperties)
        {
            CopyMatchingProperties(OldTarget, NewWidget);
        }

        // Migrate children. If the new widget is a panel, re-add with the old
        // slot templates — UPanelWidget::AddChild(Content, SlotTemplate) reuses
        // slot-template properties when the panel's slot class matches; otherwise
        // it falls back to a default slot for the new panel type.
        UPanelWidget* NewPanel = Cast<UPanelWidget>(NewWidget);
        if (NewPanel)
        {
            for (int32 i = 0; i < OldChildren.Num(); ++i)
            {
                UWidget* Child = OldChildren[i];
                if (!Child)
                {
                    continue;
                }
                UPanelSlot* Template = OldChildSlots.IsValidIndex(i) ? OldChildSlots[i] : nullptr;
#if UE_VERSION_OLDER_THAN(5, 6, 0)
                // UE 5.4/5.5: UPanelWidget::AddChild has no SlotTemplate overload.
                // Re-add with a default slot; the child still migrates, only the
                // template-driven slot-property reuse is unavailable on these versions.
                (void)Template;
                NewPanel->AddChild(Child);
#else
                NewPanel->AddChild(Child, Template);
#endif
            }
        }
        else if (OldChildren.Num() > 0)
        {
            // Leaf widget replacement; children have nowhere to live.
            UE_LOG(LogWidgetAuthoringUtils, Warning,
                TEXT("ReplaceWidgetClass: new widget '%s' (%s) is not a panel; dropping %d child(ren)"),
                *TargetName.ToString(), *NewClass->GetName(), OldChildren.Num());
        }

        if (bWasRoot)
        {
            // Direct RootWidget write — the step python can't do because the
            // property is BlueprintProtected on the Blueprint/Python binding side.
            Tree->RootWidget = NewWidget;
        }
        else if (!OldParent->ReplaceChild(OldTarget, NewWidget))
        {
            // ReplaceChild reuses the parent's existing UPanelSlot (preserving
            // anchors/offsets/alignment); fall back to remove+add if it refuses.
            OldParent->RemoveChild(OldTarget);
            OldParent->AddChild(NewWidget);
        }

        NewWidget->bIsVariable = OldTarget->bIsVariable;

        // Swap variable GUIDs: drop the old one, register the new one under
        // the same name so downstream BP references re-bind on recompile.
        RemoveWidgetVariableGuid(WidgetBP, TargetName);
        EnsureWidgetVariableGuid(WidgetBP, NewWidget->GetFName());
        EnsureAllWidgetVariableGuids(WidgetBP);

        // Make the old widget GC-eligible: re-outer it to the transient package.
        OldTarget->Rename(nullptr, GetTransientPackage(),
            REN_DontCreateRedirectors | MCP_REN_NO_RESET_LOADERS);

        return NewWidget;
    }
}
