// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/FormatterRegistration.h"
#include "Handlers/FormatterJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

using FormatterJsonUtils::TryGetArrayField;

static bool FormatBlueprintInspect(const TSharedPtr<FJsonObject>& R, FString& OutText)
{
    if (!R.IsValid())
    {
        return false;
    }

    TArray<FString> Lines;

    FString AssetPath;
    R->TryGetStringField(TEXT("assetPath"), AssetPath);

    // Trailing path segment doubles as the asset name when none was provided.
    FString Name;
    if (!AssetPath.IsEmpty())
    {
        int32 SlashIdx = INDEX_NONE;
        if (AssetPath.FindLastChar('/', SlashIdx))
        {
            Name = AssetPath.Mid(SlashIdx + 1);
        }
        else
        {
            Name = AssetPath;
        }
    }
    if (Name.IsEmpty()) Name = TEXT("Unknown");
    Lines.Add(Name);
    Lines.Add(FString::Printf(TEXT("Path: %s"), *AssetPath));

    FString ParentClass;
    if (R->TryGetStringField(TEXT("parentClass"), ParentClass) && !ParentClass.IsEmpty())
    {
        FString NativeParent;
        FString ParentLine = ParentClass;
        if (R->TryGetStringField(TEXT("nativeParentClass"), NativeParent) &&
            !NativeParent.IsEmpty() && !NativeParent.Equals(ParentClass))
        {
            ParentLine = FString::Printf(TEXT("%s -> %s"), *ParentClass, *NativeParent);
        }
        Lines.Add(FString::Printf(TEXT("Parent: %s"), *ParentLine));
    }

    {
        FString BpType;
        if (!R->TryGetStringField(TEXT("blueprintType"), BpType))
        {
            BpType = TEXT("Unknown");
        }
        FString TypeLine = FString::Printf(TEXT("Type: %s"), *BpType);
        double FileSizeKB = 0.0;
        if (R->TryGetNumberField(TEXT("fileSizeKB"), FileSizeKB))
        {
            TypeLine += FString::Printf(TEXT(" | Size: %lldKB"), (int64)FMath::RoundToDouble(FileSizeKB));
        }
        Lines.Add(TypeLine);
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Interfaces = TryGetArrayField(R, TEXT("interfaces")))
    {
        if (Interfaces->Num() > 0)
        {
            TArray<FString> Names;
            for (const TSharedPtr<FJsonValue>& V : *Interfaces)
            {
                Names.Add(V.IsValid() ? V->AsString() : FString());
            }
            Lines.Add(FString::Printf(TEXT("Interfaces: %s"), *FString::Join(Names, TEXT(", "))));
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Components = TryGetArrayField(R, TEXT("components")))
    {
        if (Components->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Components (%d):"), Components->Num()));
            for (const TSharedPtr<FJsonValue>& V : *Components)
            {
                const TSharedPtr<FJsonObject>* C = nullptr;
                if (V.IsValid() && V->TryGetObject(C) && C && (*C).IsValid())
                {
                    FString Cls, Var;
                    (*C)->TryGetStringField(TEXT("componentClass"), Cls);
                    (*C)->TryGetStringField(TEXT("variableName"), Var);
                    Lines.Add(FString::Printf(TEXT("  %s %s"), *Cls, *Var));
                }
            }
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Variables = TryGetArrayField(R, TEXT("variables")))
    {
        if (Variables->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Variables (%d):"), Variables->Num()));
            for (const TSharedPtr<FJsonValue>& V : *Variables)
            {
                const TSharedPtr<FJsonObject>* Var = nullptr;
                if (!V.IsValid() || !V->TryGetObject(Var) || !Var || !(*Var).IsValid()) continue;

                FString VarName, VarType;
                (*Var)->TryGetStringField(TEXT("name"), VarName);
                (*Var)->TryGetStringField(TEXT("type"), VarType);

                FString Line = FString::Printf(TEXT("  %s: %s"), *VarName, *VarType);
                TArray<FString> Flags;
                bool bFlag = false;
                if ((*Var)->TryGetBoolField(TEXT("editable"), bFlag) && bFlag) Flags.Add(TEXT("editable"));
                if ((*Var)->TryGetBoolField(TEXT("instanceEditable"), bFlag) && bFlag) Flags.Add(TEXT("instanceEditable"));
                if ((*Var)->TryGetBoolField(TEXT("replicated"), bFlag) && bFlag) Flags.Add(TEXT("replicated"));
                if ((*Var)->TryGetBoolField(TEXT("readOnly"), bFlag) && bFlag) Flags.Add(TEXT("readOnly"));
                if ((*Var)->TryGetBoolField(TEXT("private"), bFlag) && bFlag) Flags.Add(TEXT("private"));
                FString VCategory;
                if ((*Var)->TryGetStringField(TEXT("category"), VCategory) && !VCategory.IsEmpty())
                {
                    Flags.Add(FString::Printf(TEXT("category=%s"), *VCategory));
                }
                if (Flags.Num() > 0)
                {
                    Line += FString::Printf(TEXT(" [%s]"), *FString::Join(Flags, TEXT(", ")));
                }
                Lines.Add(Line);
            }
        }
    }

    auto FormatParamList = [](const TArray<TSharedPtr<FJsonValue>>& Params) -> FString
    {
        TArray<FString> Pieces;
        for (const TSharedPtr<FJsonValue>& V : Params)
        {
            const TSharedPtr<FJsonObject>* P = nullptr;
            if (!V.IsValid() || !V->TryGetObject(P) || !P || !(*P).IsValid()) continue;
            FString PName, PType;
            (*P)->TryGetStringField(TEXT("name"), PName);
            (*P)->TryGetStringField(TEXT("type"), PType);
            Pieces.Add(FString::Printf(TEXT("%s: %s"), *PName, *PType));
        }
        return FString::Join(Pieces, TEXT(", "));
    };

    if (const TArray<TSharedPtr<FJsonValue>>* Functions = TryGetArrayField(R, TEXT("functions")))
    {
        if (Functions->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Functions (%d):"), Functions->Num()));
            for (const TSharedPtr<FJsonValue>& V : *Functions)
            {
                const TSharedPtr<FJsonObject>* F = nullptr;
                if (!V.IsValid() || !V->TryGetObject(F) || !F || !(*F).IsValid()) continue;

                FString FName;
                (*F)->TryGetStringField(TEXT("name"), FName);

                FString Line = FString::Printf(TEXT("  %s("), *FName);
                if (const TArray<TSharedPtr<FJsonValue>>* Inputs = TryGetArrayField(*F, TEXT("inputs")))
                {
                    Line += FormatParamList(*Inputs);
                }
                Line += TEXT(")");

                if (const TArray<TSharedPtr<FJsonValue>>* Outputs = TryGetArrayField(*F, TEXT("outputs")))
                {
                    if (Outputs->Num() == 1)
                    {
                        const TSharedPtr<FJsonObject>* O = nullptr;
                        if ((*Outputs)[0].IsValid() && (*Outputs)[0]->TryGetObject(O) && O && (*O).IsValid())
                        {
                            FString OType;
                            (*O)->TryGetStringField(TEXT("type"), OType);
                            Line += FString::Printf(TEXT(" -> %s"), *OType);
                        }
                    }
                    else if (Outputs->Num() > 1)
                    {
                        Line += FString::Printf(TEXT(" -> (%s)"), *FormatParamList(*Outputs));
                    }
                }

                TArray<FString> Flags;
                bool bFlag = false;
                if ((*F)->TryGetBoolField(TEXT("pure"), bFlag) && bFlag) Flags.Add(TEXT("pure"));
                if ((*F)->TryGetBoolField(TEXT("const"), bFlag) && bFlag) Flags.Add(TEXT("const"));
                if ((*F)->TryGetBoolField(TEXT("callInEditor"), bFlag) && bFlag) Flags.Add(TEXT("callInEditor"));
                FString FCategory;
                if ((*F)->TryGetStringField(TEXT("category"), FCategory) && !FCategory.IsEmpty())
                {
                    Flags.Add(FString::Printf(TEXT("category=%s"), *FCategory));
                }
                if (Flags.Num() > 0)
                {
                    Line += FString::Printf(TEXT(" [%s]"), *FString::Join(Flags, TEXT(", ")));
                }
                Lines.Add(Line);
            }
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Events = TryGetArrayField(R, TEXT("events")))
    {
        if (Events->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Events (%d):"), Events->Num()));
            for (const TSharedPtr<FJsonValue>& V : *Events)
            {
                const TSharedPtr<FJsonObject>* E = nullptr;
                if (!V.IsValid() || !V->TryGetObject(E) || !E || !(*E).IsValid()) continue;

                FString EName, EvType;
                (*E)->TryGetStringField(TEXT("name"), EName);

                FString Line = FString::Printf(TEXT("  %s("), *EName);
                if (const TArray<TSharedPtr<FJsonValue>>* Params = TryGetArrayField(*E, TEXT("parameters")))
                {
                    Line += FormatParamList(*Params);
                }
                Line += TEXT(")");

                TArray<FString> EventFlags;
                if ((*E)->TryGetStringField(TEXT("eventType"), EvType) && EvType.Equals(TEXT("custom")))
                {
                    EventFlags.Add(TEXT("custom"));
                }
                // Mark inert default stubs (e.g. the disabled ReceiveTick /
                // ReceiveActorBeginOverlap ghosts) so the text readback does not
                // present them as live handlers.
                bool bEnabled = true;
                if ((*E)->TryGetBoolField(TEXT("enabled"), bEnabled) && !bEnabled)
                {
                    EventFlags.Add(TEXT("disabled"));
                }
                if (EventFlags.Num() > 0)
                {
                    Line += FString::Printf(TEXT(" [%s]"), *FString::Join(EventFlags, TEXT(", ")));
                }
                Lines.Add(Line);
            }
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Graphs = TryGetArrayField(R, TEXT("graphs")))
    {
        if (Graphs->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Graphs (%d):"), Graphs->Num()));
            for (const TSharedPtr<FJsonValue>& V : *Graphs)
            {
                const TSharedPtr<FJsonObject>* G = nullptr;
                if (!V.IsValid() || !V->TryGetObject(G) || !G || !(*G).IsValid()) continue;
                FString GName;
                double NodeCount = 0.0;
                (*G)->TryGetStringField(TEXT("name"), GName);
                (*G)->TryGetNumberField(TEXT("nodeCount"), NodeCount);
                Lines.Add(FString::Printf(TEXT("  %s (%d nodes)"), *GName, (int32)NodeCount));
            }
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Warnings = TryGetArrayField(R, TEXT("performanceWarnings")))
    {
        if (Warnings->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(TEXT("Warnings:"));
            for (const TSharedPtr<FJsonValue>& V : *Warnings)
            {
                Lines.Add(FString::Printf(TEXT("  - %s"), *(V.IsValid() ? V->AsString() : FString())));
            }
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Refs = TryGetArrayField(R, TEXT("references")))
    {
        if (Refs->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("References (%d):"), Refs->Num()));
            for (const TSharedPtr<FJsonValue>& V : *Refs)
            {
                Lines.Add(FString::Printf(TEXT("  %s"), *(V.IsValid() ? V->AsString() : FString())));
            }
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* RefBy = TryGetArrayField(R, TEXT("referencedBy")))
    {
        if (RefBy->Num() > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Referenced By (%d):"), RefBy->Num()));
            for (const TSharedPtr<FJsonValue>& V : *RefBy)
            {
                Lines.Add(FString::Printf(TEXT("  %s"), *(V.IsValid() ? V->AsString() : FString())));
            }
        }
    }

    OutText = FString::Join(Lines, TEXT("\n"));
    return true;
}

REGISTER_RPC_FORMATTER("blueprint.inspect", &FormatBlueprintInspect);
