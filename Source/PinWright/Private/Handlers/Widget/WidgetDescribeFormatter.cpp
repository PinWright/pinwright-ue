// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/FormatterRegistration.h"
#include "Handlers/FormatterJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Compat/JsonKeyCompat.h"

using FormatterJsonUtils::StringifyValue;

namespace
{
    static FString FormatSlotValue(const TSharedPtr<FJsonValue>& Value);

    static FString FormatSlotObject(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return TEXT("{}");
        }

        TArray<FString> Keys;
        for (const auto& Pair : Obj->Values)
        {
            Keys.Add(EARGCompat::JsonKeyToString(Pair.Key));
        }

        TArray<FString> Pieces;
        for (const FString& K : Keys)
        {
            Pieces.Add(FString::Printf(TEXT("%s=%s"), *K, *FormatSlotValue(Obj->Values[EARGCompat::JsonFieldKey(K)])));
        }
        return FString::Printf(TEXT("{%s}"), *FString::Join(Pieces, TEXT(", ")));
    }

    static FString FormatSlotArray(const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        TArray<FString> Pieces;
        for (const TSharedPtr<FJsonValue>& Value : Values)
        {
            Pieces.Add(FormatSlotValue(Value));
        }
        return FString::Printf(TEXT("[%s]"), *FString::Join(Pieces, TEXT(", ")));
    }

    static FString FormatSlotValue(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }

        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Value->TryGetObject(Obj) && Obj && (*Obj).IsValid())
        {
            return FormatSlotObject(*Obj);
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array)
        {
            return FormatSlotArray(*Array);
        }

        return StringifyValue(Value);
    }

    static bool TryGetStructuredSlot(const TSharedPtr<FJsonObject>& Node, const TSharedPtr<FJsonObject>*& OutSlotObj)
    {
        OutSlotObj = nullptr;
        return Node.IsValid() &&
               Node->TryGetObjectField(TEXT("slot"), OutSlotObj) &&
               OutSlotObj &&
               (*OutSlotObj).IsValid();
    }

    static void AppendSlotLine(const TSharedPtr<FJsonObject>& SlotObj, const FString& Indent, TArray<FString>& Lines, const TCHAR* PropsFieldName)
    {
        FString SlotType;
        SlotObj->TryGetStringField(TEXT("type"), SlotType);
        if (SlotType.IsEmpty())
        {
            SlotType = TEXT("Slot");
        }

        FString Line = FString::Printf(TEXT("%s  [slot] %s"), *Indent, *SlotType);

        const TSharedPtr<FJsonObject>* SlotProps = nullptr;
        if (SlotObj->TryGetObjectField(PropsFieldName, SlotProps) && SlotProps && (*SlotProps).IsValid())
        {
            Line += TEXT(" ");
            Line += FormatSlotObject(*SlotProps);
        }

        Lines.Add(Line);
    }

    static void AppendBindingsAndDelegates(
        const TSharedPtr<FJsonObject>& Node,
        const FString& Indent,
        TArray<FString>& Lines)
    {
        const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
        if (Node->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings)
        {
            for (const TSharedPtr<FJsonValue>& V : *Bindings)
            {
                const TSharedPtr<FJsonObject>* B = nullptr;
                if (V.IsValid() && V->TryGetObject(B) && B && (*B).IsValid())
                {
                    FString PName, FName;
                    (*B)->TryGetStringField(TEXT("propertyName"), PName);
                    (*B)->TryGetStringField(TEXT("functionName"), FName);
                    Lines.Add(FString::Printf(TEXT("%s  [bind] %s -> %s"), *Indent, *PName, *FName));
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* Delegates = nullptr;
        if (Node->TryGetArrayField(TEXT("delegates"), Delegates) && Delegates)
        {
            for (const TSharedPtr<FJsonValue>& V : *Delegates)
            {
                const TSharedPtr<FJsonObject>* D = nullptr;
                if (V.IsValid() && V->TryGetObject(D) && D && (*D).IsValid())
                {
                    FString PName, FName;
                    (*D)->TryGetStringField(TEXT("propertyName"), PName);
                    (*D)->TryGetStringField(TEXT("functionName"), FName);
                    Lines.Add(FString::Printf(TEXT("%s  [event] %s -> %s"), *Indent, *PName, *FName));
                }
            }
        }
    }

    static void FormatLiveNode(const TSharedPtr<FJsonObject>& Node, const FString& Indent, TArray<FString>& Lines)
    {
        if (!Node.IsValid()) return;

        FString SlateType;
        Node->TryGetStringField(TEXT("slate_type"), SlateType);
        if (SlateType.IsEmpty()) SlateType = TEXT("SWidget");

        FString DebugName;
        Node->TryGetStringField(TEXT("debug_name"), DebugName);

        const TSharedPtr<FJsonObject>* SourceObj = nullptr;
        const bool bHasSource = Node->TryGetObjectField(TEXT("source"), SourceObj) &&
                                SourceObj && (*SourceObj).IsValid();

        FString DisplayName;
        if (bHasSource)
        {
            FString WidgetName;
            (*SourceObj)->TryGetStringField(TEXT("widget_name"), WidgetName);
            if (!WidgetName.IsEmpty())
            {
                DisplayName = WidgetName;
            }
            else
            {
                FString OwningName;
                (*SourceObj)->TryGetStringField(TEXT("owning_user_widget"), OwningName);
                if (!OwningName.IsEmpty())
                {
                    DisplayName = OwningName;
                }
            }
        }
        if (DisplayName.IsEmpty())
        {
            DisplayName = DebugName;
        }

        FString Line = FString::Printf(TEXT("%s%s \"%s\""), *Indent, *SlateType, *DisplayName);

        const TSharedPtr<FJsonObject>* RuntimeObj = nullptr;
        if (Node->TryGetObjectField(TEXT("runtime_state"), RuntimeObj) && RuntimeObj && (*RuntimeObj).IsValid())
        {
            TArray<FString> StatePieces;

            FString Visibility;
            if ((*RuntimeObj)->TryGetStringField(TEXT("visibility"), Visibility) && !Visibility.IsEmpty())
            {
                StatePieces.Add(FString::Printf(TEXT("vis=%s"), *Visibility));
            }

            bool bEnabled = true;
            if ((*RuntimeObj)->TryGetBoolField(TEXT("enabled"), bEnabled) && !bEnabled)
            {
                StatePieces.Add(TEXT("disabled"));
            }

            FString Clipping;
            if ((*RuntimeObj)->TryGetStringField(TEXT("clipping"), Clipping) && !Clipping.IsEmpty())
            {
                StatePieces.Add(FString::Printf(TEXT("clip=%s"), *Clipping));
            }

            bool bFocused = false;
            if ((*RuntimeObj)->TryGetBoolField(TEXT("focused"), bFocused) && bFocused)
            {
                StatePieces.Add(TEXT("focused"));
            }

            if (StatePieces.Num() > 0)
            {
                Line += FString::Printf(TEXT(" (%s)"), *FString::Join(StatePieces, TEXT(", ")));
            }
        }

        if (bHasSource)
        {
            FString WidgetBp;
            if ((*SourceObj)->TryGetStringField(TEXT("widget_bp"), WidgetBp) && !WidgetBp.IsEmpty())
            {
                Line += FString::Printf(TEXT(" [bp: %s]"), *FPaths::GetBaseFilename(WidgetBp));
            }
        }

        Lines.Add(Line);

        const TSharedPtr<FJsonObject>* SlotObj = nullptr;
        if (Node->TryGetObjectField(TEXT("slot"), SlotObj) && SlotObj && (*SlotObj).IsValid())
        {
            // Guard: only emit a slot line when the live slot has a non-empty
            // type or a non-empty properties object. Avoids a confusing
            // "[slot] Slot" line for empty live slots.
            FString SlotType;
            const bool bHasType = (*SlotObj)->TryGetStringField(TEXT("type"), SlotType) && !SlotType.IsEmpty();
            const TSharedPtr<FJsonObject>* SlotProps = nullptr;
            const bool bHasProperties = (*SlotObj)->TryGetObjectField(TEXT("properties"), SlotProps) &&
                                        SlotProps && (*SlotProps).IsValid() && (*SlotProps)->Values.Num() > 0;
            if (bHasType || bHasProperties)
            {
                AppendSlotLine(*SlotObj, Indent, Lines, TEXT("properties"));
            }
        }

        AppendBindingsAndDelegates(Node, Indent, Lines);

        const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        if (Node->TryGetArrayField(TEXT("children"), Children) && Children)
        {
            const FString ChildIndent = Indent + TEXT("  ");
            for (const TSharedPtr<FJsonValue>& V : *Children)
            {
                const TSharedPtr<FJsonObject>* Child = nullptr;
                if (V.IsValid() && V->TryGetObject(Child) && Child && (*Child).IsValid())
                {
                    FormatLiveNode(*Child, ChildIndent, Lines);
                }
            }
        }
    }

    static bool FormatLiveSnapshot(const TSharedPtr<FJsonObject>& R, FString& OutText)
    {
        if (!R.IsValid())
        {
            return false;
        }

        TArray<FString> Lines;
        Lines.Add(TEXT("Live Widget Capture"));

        FString CaptureSource;
        if (!R->TryGetStringField(TEXT("capture_source"), CaptureSource) || CaptureSource.IsEmpty())
        {
            CaptureSource = TEXT("live");
        }
        Lines.Add(FString::Printf(TEXT("Source: %s"), *CaptureSource));

        const TSharedPtr<FJsonObject>* ViewportObj = nullptr;
        if (R->TryGetObjectField(TEXT("viewport_size"), ViewportObj) && ViewportObj && (*ViewportObj).IsValid())
        {
            double W = 0.0, H = 0.0;
            (*ViewportObj)->TryGetNumberField(TEXT("w"), W);
            (*ViewportObj)->TryGetNumberField(TEXT("h"), H);
            if (W > 0.0 && H > 0.0)
            {
                Lines.Add(FString::Printf(TEXT("Viewport: %dx%d"), (int32)W, (int32)H));
            }
        }

        const TSharedPtr<FJsonObject>* SummaryObj = nullptr;
        if (R->TryGetObjectField(TEXT("snapshot_summary"), SummaryObj) && SummaryObj && (*SummaryObj).IsValid())
        {
            double Total = 0.0, Backed = 0.0, MaxDepth = 0.0;
            (*SummaryObj)->TryGetNumberField(TEXT("total_node_count"), Total);
            (*SummaryObj)->TryGetNumberField(TEXT("backing_widget_node_count"), Backed);
            (*SummaryObj)->TryGetNumberField(TEXT("max_depth"), MaxDepth);
            if (Total > 0.0)
            {
                Lines.Add(FString::Printf(TEXT("Widgets: %d (backed: %d, depth: %d)"),
                                          (int32)Total, (int32)Backed, (int32)MaxDepth));
            }
        }

        const TSharedPtr<FJsonObject>* RootObj = nullptr;
        if (R->TryGetObjectField(TEXT("root"), RootObj) && RootObj && (*RootObj).IsValid())
        {
            Lines.Add(TEXT(""));
            FormatLiveNode(*RootObj, TEXT(""), Lines);
        }

        OutText = FString::Join(Lines, TEXT("\n"));
        return true;
    }

    // Walks a node object and appends formatted lines. Mirrors the JS
    // `formatNode(node, indent)` recursion.
    static void FormatNode(const TSharedPtr<FJsonObject>& Node, const FString& Indent, TArray<FString>& Lines)
    {
        if (!Node.IsValid()) return;

        FString NodeType, NodeName;
        Node->TryGetStringField(TEXT("type"), NodeType);
        Node->TryGetStringField(TEXT("name"), NodeName);

        FString Line = FString::Printf(TEXT("%s%s \"%s\""), *Indent, *NodeType, *NodeName);

        const TSharedPtr<FJsonObject>* SlotObj = nullptr;
        const bool bHasStructuredSlot = TryGetStructuredSlot(Node, SlotObj);

        const TSharedPtr<FJsonObject>* PropsObj = nullptr;
        const bool bHasProps = Node->TryGetObjectField(TEXT("props"), PropsObj) &&
                               PropsObj && (*PropsObj).IsValid();

        if (bHasProps)
        {
            TArray<FString> Keys;
            for (const auto& Pair : (*PropsObj)->Values)
            {
                Keys.Add(EARGCompat::JsonKeyToString(Pair.Key));
            }
            if (bHasStructuredSlot)
            {
                Keys.Remove(TEXT("Slot"));
            }

            const int32 NumKeys = Keys.Num();
            if (NumKeys > 0 && NumKeys <= 4)
            {
                TArray<FString> Pieces;
                for (const FString& K : Keys)
                {
                    FString VS = StringifyValue((*PropsObj)->Values[EARGCompat::JsonFieldKey(K)]);
                    if (VS.Len() > 40)
                    {
                        VS = VS.Left(37) + TEXT("...");
                    }
                    Pieces.Add(FString::Printf(TEXT("%s=%s"), *K, *VS));
                }
                Line += FString::Printf(TEXT(" {%s}"), *FString::Join(Pieces, TEXT(", ")));
                Lines.Add(Line);
            }
            else if (NumKeys > 4)
            {
                Line += FString::Printf(TEXT(" {%d props}"), NumKeys);
                Lines.Add(Line);
                for (const FString& K : Keys)
                {
                    FString VS = StringifyValue((*PropsObj)->Values[EARGCompat::JsonFieldKey(K)]);
                    if (VS.Len() > 80)
                    {
                        VS = VS.Left(77) + TEXT("...");
                    }
                    Lines.Add(FString::Printf(TEXT("%s  %s: %s"), *Indent, *K, *VS));
                }
            }
            else
            {
                // Empty props object — emit the bare line.
                Lines.Add(Line);
            }
        }
        else
        {
            Lines.Add(Line);
        }

        if (bHasStructuredSlot)
        {
            AppendSlotLine(*SlotObj, Indent, Lines, TEXT("props"));
        }

        AppendBindingsAndDelegates(Node, Indent, Lines);

        const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        if (Node->TryGetArrayField(TEXT("children"), Children) && Children)
        {
            const FString ChildIndent = Indent + TEXT("  ");
            for (const TSharedPtr<FJsonValue>& V : *Children)
            {
                const TSharedPtr<FJsonObject>* Child = nullptr;
                if (V.IsValid() && V->TryGetObject(Child) && Child && (*Child).IsValid())
                {
                    FormatNode(*Child, ChildIndent, Lines);
                }
            }
        }
    }
}

static bool FormatWidgetDescribe(const TSharedPtr<FJsonObject>& R, FString& OutText)
{
    if (!R.IsValid())
    {
        return false;
    }

    if (R->HasField(TEXT("capture_source")) || R->HasField(TEXT("root")))
    {
        return FormatLiveSnapshot(R, OutText);
    }

    TArray<FString> Lines;

    FString AssetPath;
    R->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty()) AssetPath = TEXT("Unknown");
    Lines.Add(FString::Printf(TEXT("Widget: %s"), *AssetPath));

    FString RootClass;
    if (R->TryGetStringField(TEXT("root_class"), RootClass) && !RootClass.IsEmpty())
    {
        Lines.Add(FString::Printf(TEXT("Parent: %s"), *RootClass));
    }

    double WidgetCount = 0.0;
    if (R->TryGetNumberField(TEXT("widget_count"), WidgetCount))
    {
        Lines.Add(FString::Printf(TEXT("Widgets: %d"), (int32)WidgetCount));
    }

    const TSharedPtr<FJsonObject>* TreeObj = nullptr;
    if (R->TryGetObjectField(TEXT("tree"), TreeObj) && TreeObj && (*TreeObj).IsValid())
    {
        Lines.Add(TEXT(""));
        FormatNode(*TreeObj, TEXT(""), Lines);
    }

    const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
    if (R->TryGetArrayField(TEXT("bindingTargets"), Targets) && Targets && Targets->Num() > 0)
    {
        Lines.Add(TEXT(""));
        Lines.Add(FString::Printf(TEXT("Binding Targets (%d):"), Targets->Num()));
        for (const TSharedPtr<FJsonValue>& V : *Targets)
        {
            const TSharedPtr<FJsonObject>* T = nullptr;
            if (!V.IsValid() || !V->TryGetObject(T) || !T || !(*T).IsValid()) continue;
            FString FName, EntryType, GraphName;
            (*T)->TryGetStringField(TEXT("functionName"), FName);
            (*T)->TryGetStringField(TEXT("entryType"), EntryType);
            (*T)->TryGetStringField(TEXT("graphName"), GraphName);
            Lines.Add(FString::Printf(TEXT("  %s [%s] in %s"), *FName, *EntryType, *GraphName));
        }
    }

    OutText = FString::Join(Lines, TEXT("\n"));
    return true;
}

REGISTER_RPC_FORMATTER("widget.describe", &FormatWidgetDescribe);
