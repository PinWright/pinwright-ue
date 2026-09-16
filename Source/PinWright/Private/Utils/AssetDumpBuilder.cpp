// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AssetDumpBuilder.h"


#include "Decompiler/BpirDecompiler.h"
#include "PinWright_SCSHandlers.h"
#include "Handlers/Asset/AssetDumpHandlerInternal.h"
#include "Handlers/UI/WidgetXmlExporter.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Misc/DateTime.h"
#include "Dom/JsonValue.h"
#include "WidgetBlueprint.h"
#include "UObject/ObjectRedirector.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Blueprint/UserWidget.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Animation/AnimInstance.h"
#include "Engine/GameInstance.h"

namespace
{
    // Maps a runtime UClass to the coarse "kind" string surfaced in meta.json. The four
    // checked roots (UUserWidget, AActor, UActorComponent, UAnimInstance) are independent
    // peer hierarchies — none inherits from another — so the ordering is fixed only for
    // determinism, with UUserWidget first because Widget BPs dominate the asset-dump corpus
    // in this codebase.
    FString ResolveRuntimeKind(const UClass* RuntimeClass)
    {
        if (!RuntimeClass)
        {
            return TEXT("Object");
        }
        if (RuntimeClass->IsChildOf(UUserWidget::StaticClass()))    return TEXT("Widget");
        if (RuntimeClass->IsChildOf(AActor::StaticClass()))         return TEXT("Actor");
        if (RuntimeClass->IsChildOf(UActorComponent::StaticClass()))return TEXT("Component");
        if (RuntimeClass->IsChildOf(UAnimInstance::StaticClass()))  return TEXT("AnimInstance");
        return TEXT("Object");
    }
}

namespace AssetDumpBuilder
{

FString ResolveRedirectorTarget(const UObjectRedirector* Redirector)
{
    return (Redirector != nullptr && Redirector->DestinationObject != nullptr)
        ? Redirector->DestinationObject->GetPathName()
        : FString();
}

TSharedPtr<FJsonObject> BuildMetaJson(UObject* Asset)
{
    TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
    if (!Asset) return Meta;

    Meta->SetStringField(TEXT("assetPath"), Asset->GetPathName());

    FString ClassName;
    FString ParentClass;
    FString Kind;
    FString BlueprintTypeStr;
    if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset))
    {
        ClassName   = WBP->GeneratedClass ? WBP->GeneratedClass->GetName() : (WBP->GetName() + TEXT("_C"));
        ParentClass = WBP->ParentClass    ? WBP->ParentClass->GetPathName() : FString();
        // UWidgetBlueprint is always a Widget runtime kind regardless of its C++ parent class
        // (e.g. LyraHUDLayout, LyraActivatableWidget, EditorUtilityWidget all derive UUserWidget).
        Kind = TEXT("Widget");
        BlueprintTypeStr = AssetDumpHandler::BlueprintTypeToStatusString(WBP);
    }
    else if (UBlueprint* BP = Cast<UBlueprint>(Asset))
    {
        ClassName   = BP->GeneratedClass ? BP->GeneratedClass->GetName() : (BP->GetName() + TEXT("_C"));
        ParentClass = BP->ParentClass    ? BP->ParentClass->GetPathName() : FString();
        BlueprintTypeStr = AssetDumpHandler::BlueprintTypeToStatusString(BP);
        switch (BP->BlueprintType)
        {
        case BPTYPE_MacroLibrary:    Kind = TEXT("MacroLibrary");    break;
        case BPTYPE_Interface:       Kind = TEXT("Interface");       break;
        case BPTYPE_FunctionLibrary: Kind = TEXT("FunctionLibrary"); break;
        default:
            // BPTYPE_Normal / BPTYPE_Const / BPTYPE_LevelScript: walk the generated class
            // (or ParentClass when GeneratedClass is null — covers stub-class repros like
            // B-asset-dump-bp-meta-degrades-when-genclass-null).
            Kind = ResolveRuntimeKind(BP->GeneratedClass ? BP->GeneratedClass.Get() : BP->ParentClass.Get());
            break;
        }
    }
    else
    {
        ClassName              = Asset->GetClass()->GetName();
        UClass* SuperClass     = Asset->GetClass()->GetSuperClass();
        ParentClass            = SuperClass ? SuperClass->GetPathName() : FString();
        Kind                   = ResolveRuntimeKind(Asset->GetClass());
    }
    Meta->SetStringField(TEXT("className"),   ClassName);
    Meta->SetStringField(TEXT("parentClass"), ParentClass);
    Meta->SetStringField(TEXT("kind"), Kind);
    // v7: `blueprintType` is uniformly present — string for Blueprint assets, JSON null
    // for non-Blueprint assets, so consumers can branch on field shape rather than
    // field presence (matches the `propertiesStatus` discipline).
    if (!BlueprintTypeStr.IsEmpty())
    {
        Meta->SetStringField(TEXT("blueprintType"), BlueprintTypeStr);
    }
    else
    {
        Meta->SetField(TEXT("blueprintType"), MakeShared<FJsonValueNull>());
    }

    // UObjectRedirector: surface the redirect target so cache consumers can follow it
    // without re-loading the asset. Field is only emitted on redirector assets — every
    // other asset type omits it. Empty string when DestinationObject is null (the only
    // serialized target field on UObjectRedirector; no `DestinationName` fallback exists).
    if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset))
    {
        Meta->SetStringField(TEXT("redirectsTo"), ResolveRedirectorTarget(Redirector));
    }

    // Schema versioning for the per-property output shape (see PropertyUtils.cpp
    // ExportPropertyToJsonValueWithInheritance). v2: omit fields whose value would
    // be the implicit default (inherited_from when defined-here, is_overridden_locally
    // when false, flags when no whitelisted entries). v3: BuildClassPropertyJson
    // emits only properties whose value differs from the parent CDO; non-overridden
    // inherited defaults are dropped entirely. When ParentCDO is null (some callers
    // pass nullptr), every property is treated as overridden — same shape as v2.
    // v4: removed `assetType` (redundant with `className`).
    // v5: removed `packageFlags` (degenerate field — empty on >99.9% of dumped assets, no consumer).
    // v6: added `kind` (always present, coarse runtime classification), `blueprintType`
    //     (BP-only, raw EBlueprintType enum string), `sidecarsEmitted` (sorted list of
    //     every file the dispatcher wrote — attached in BuildAllFilesForAsset),
    //     `propertiesStatus` for non-Blueprint assets (`{status:"n/a", reason:"non_blueprint_asset"}`
    //     so the field is uniformly present), and `compileStateAvailable`/`compileStateReason`
    //     on Niagara assets when compile state is deferred or uninitialized.
    // v7: `blueprintType` is now uniformly present — JSON null on non-Blueprint assets
    //     instead of being omitted entirely. Mirrors the v6 `propertiesStatus` discipline
    //     of always emitting the field so consumers branch on shape, not presence.
    // v8: removed `pluginVersion` and `dumpSchemaVersion` — versioning lives solely in
    //     .dumpcache.json (dumper.pluginVersion + aspectVersions["meta.json"]); meta.json
    //     content is now stable across plugin releases. This block stays as the history
    //     of BuildMetaJson's output shape even though no version field is emitted here.
    // v9: removed `compileStateAvailable` and `compileStateReason`. Persistent Niagara
    //     sidecars now describe authored state; live compile diagnostics stay on the
    //     niagara.inspect / niagara.validate response surfaces.
    return Meta;
}

// Emit one `# BPIR_ERROR:` line per Error-tagged warning and `# BPIR_WARN:` per Warn-tagged
// warning, so consumers can distinguish source-graph notes from decompiler-internal failures.
void FormatBpirWarningMarkers(const TArray<FBpirWarning>& Warnings, FString& Output)
{
    for (const FBpirWarning& W : Warnings)
    {
        const TCHAR* Prefix = (W.Severity == EBpirWarningSeverity::Error)
            ? TEXT("# BPIR_ERROR: ")
            : TEXT("# BPIR_WARN: ");
        Output += Prefix;
        Output += W.Text;
        Output += TEXT("\n");
    }
}

// Dump format adds deterministic alpha-sort + per-graph headers that
// FBpirDecompiler::Decompile() does not emit — so this is separate.
FString BuildBpirText(UBlueprint* Blueprint)
{
    if (!Blueprint) return FString();

    FBpirDecompiler Decompiler(Blueprint);

    TArray<FBpirWarning> Warnings;
    FString Output;

    auto AppendGraph = [&](UEdGraph* Graph, const FString& Kind)
    {
        if (!Graph) return;
        const FString GraphName = Graph->GetName();
        FBpirDecompileResult Result;
        if (Kind == TEXT("ubergraph"))
            Result = Decompiler.DecompileGraph(GraphName);
        else if (Kind == TEXT("function"))
            Result = Decompiler.DecompileFunction(GraphName);
        else
            Result = Decompiler.DecompileMacro(GraphName);

        Output += FString::Printf(TEXT("# ==== Graph: %s (%s) ====\n"), *GraphName, *Kind);
        Output += Result.BpirText;
        if (Result.Warnings.Num() > 0)
        {
            FormatBpirWarningMarkers(Result.Warnings, Output);
        }
        else if (Result.BpirText.IsEmpty())
        {
            // The decompiler classifies UnreachableGraph; the dump builder owns ZeroNodes
            // because Graph->Nodes is its purview, not the decompiler's. When BpirText is
            // empty the decompiler always reports a non-NotEmpty reason, so no NotEmpty
            // fallback is needed below.
            EBpirEmptyReason Reason = Result.EmptyReason;
            if (Graph->Nodes.Num() == 0)
            {
                Reason = EBpirEmptyReason::ZeroNodes;
            }
            switch (Reason)
            {
            case EBpirEmptyReason::ZeroNodes:
                Output += TEXT("# (graph has zero nodes)\n");
                break;
            case EBpirEmptyReason::UnreachableGraph:
                Output += TEXT("# (graph has no decompiled bodies)\n");
                break;
            case EBpirEmptyReason::NotEmpty:
                checkNoEntry();
                break;
            }
        }
        Output += TEXT("\n");
        Warnings.Append(Result.Warnings);
    };

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        AppendGraph(Graph, TEXT("ubergraph"));
    }

    auto SortAndAppend = [&](const TArray<UEdGraph*>& Graphs, const TCHAR* Kind)
    {
        TArray<UEdGraph*> Sorted(Graphs);
        Sorted.Sort([](const UEdGraph& A, const UEdGraph& B)
        {
            return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
        });
        for (UEdGraph* G : Sorted)
        {
            if (G) AppendGraph(G, Kind);
        }
    };

    SortAndAppend(Blueprint->FunctionGraphs, TEXT("function"));
    SortAndAppend(Blueprint->MacroGraphs,   TEXT("macro"));

    if (Warnings.Num() > 0)
    {
        Output += TEXT("# ==== Warnings ====\n");
        for (const FBpirWarning& W : Warnings)
        {
            Output += W.Text + TEXT("\n");
        }
    }

    return Output;
}

bool ShouldEmitBpirText(const UBlueprint* Blueprint)
{
    if (!Blueprint) return false;

    UClass* ParentClass = Blueprint->ParentClass;
    if (!ParentClass) return true; // fail-open when parent is unresolved

    auto AnyGraphHasNodes = [](const TArray<UEdGraph*>& Graphs) -> bool
    {
        for (const UEdGraph* G : Graphs)
        {
            if (G && G->Nodes.Num() > 0) return true;
        }
        return false;
    };

    if (AnyGraphHasNodes(Blueprint->UbergraphPages)) return true;
    if (AnyGraphHasNodes(Blueprint->FunctionGraphs)) return true;
    if (AnyGraphHasNodes(Blueprint->MacroGraphs))    return true;

    // All graphs empty. Keep bpir.txt only when the parent class is a known graph-bearing
    // root (so the empty-marker still disambiguates "decompile succeeded with empty body"
    // from "decompile failed silently" for the families that can host event logic).
    // List is minimal after IsChildOf-subsumption pruning: APlayerController/ULevelScriptActor
    // are AActor children, UHUD is AActor, etc. Adding them would be redundant.
    // AGameModeBase is intentionally absent — it's a child of AActor (via AInfo) and would
    // be subsumed by AActor::StaticClass() above.
    const UClass* const GraphBearingRoots[] = {
        AActor::StaticClass(),
        UActorComponent::StaticClass(),
        UUserWidget::StaticClass(),
        UBlueprintFunctionLibrary::StaticClass(),
        UAnimInstance::StaticClass(),
        UGameInstance::StaticClass(),
    };
    for (const UClass* Root : GraphBearingRoots)
    {
        if (Root && ParentClass->IsChildOf(Root)) return true;
    }
    return false;
}

FString BuildOverriddenWidgetXml(UWidgetBlueprint* WidgetBlueprint)
{
    return WidgetXmlExporter::BuildWidgetTreeXml(WidgetBlueprint, /*bIncludeDefaults=*/false, /*bOmitSlotChain=*/true);
}

TSharedPtr<FJsonObject> BuildScsJson(UBlueprint* Blueprint)
{
    if (!Blueprint) return nullptr;
    TSharedPtr<FJsonObject> Result = FSCSHandlers::GetBlueprintSCS(Blueprint->GetPathName());
    // Skip emitting the file when GetBlueprintSCS reports no components from any source.
    if (!Result.IsValid()) return nullptr;
    double Count = 0;
    if (Result->TryGetNumberField(TEXT("count"), Count) && Count <= 0) return nullptr;
    return Result;
}

} // namespace AssetDumpBuilder
