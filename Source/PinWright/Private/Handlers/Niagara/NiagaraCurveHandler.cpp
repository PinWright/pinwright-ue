// Copyright (c) 2026 Alexander Penkin. MIT License.

// Typed Niagara curve authoring (T4 / F-niagara-curve-authoring).
//
// Three handlers:
//   * niagara.set_curve_keys   — write FRichCurve key arrays onto an inline curve DI
//                                 (UNiagaraDataInterfaceCurve / Color / Vector / Vector2D / Vector4).
//   * niagara.get_curve_keys   — read those key arrays back.
//   * niagara.bind_curve_asset — assign a UCurveFloat / UCurveLinearColor to the DI's
//                                 inherited CurveAsset property and sync the inline curves.
//
// TWO ADDRESSING FORMS, and the caller picks by which keys it sends:
//   * parameter store — `scope` + `parameterName` (+ `emitter`). Reaches a DI something already
//     resolved into a store: User-namespace DIs, renderer bindings, add_data_interface results.
//   * module input    — `entryId` + `inputName` (+ `emitter`, `scriptUsage`), the same addressing
//     niagara.set_module_input takes. Reaches the DI on a stack module's input pin.
//
// The second form is not a convenience. Every curve in stock content — `ScaleSpriteSize`'s
// `Uniform Curve Sprite Scale`, `ScaleColor -> FloatFromCurve.FloatCurve` — lives on a module's
// override pin or in the module script's own defaults, and NO parameter store holds it: both
// rapid-iteration stores were measured on an emitter owning three curve DIs and carried not one
// data-interface entry. Store-only addressing therefore refused every spelling with
// DATA_INTERFACE_NOT_FOUND and no over-life size / alpha / colour ramp could be authored
// (B-niagara-set-curve-keys-unreachable-module-input-di). The (asset, emitter, stage, module,
// input) -> DI resolution is NOT duplicated here — it is NiagaraModuleInputDI, over the same
// NiagaraEdit::ResolveTarget module target set_module_input uses.
//
// Ticket text corrections applied here (see board file F-niagara-curve-authoring.md):
//   1) Engine member names use the SUFFIX form (RedCurve / XCurve / WCurve), not the
//      ticket's prefix form (CurveRed / CurveX). Verified against
//      Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraDataInterface{Color,Vector,Vector2D,Vector4}Curve.h.
//   2) There is no separate asset-binding DI flavor. UNiagaraDataInterfaceCurveBase already
//      owns `TObjectPtr<UCurveBase> CurveAsset` and a virtual SyncCurvesToAsset() that the
//      five concrete subclasses override to copy the asset's FRichCurve channels into
//      their inline curves. Binding = set CurveAsset + SyncCurvesToAsset() + UpdateLUT().
//      No class swap.
//
// Target-resolution and mutation-scope shape mirror the rest of the niagara surface: the same
// NiagaraEdit::ResolveTarget + BeginEmitterMutationScope/EndEmitterMutationScope sequence, so
// post-edit refresh of any open system view-model is consistent across it. The payload is parsed
// here rather than through ParseDataInterfacePayload because the two addressing forms have
// different required fields and only these verbs accept both.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraModuleInputDataInterface.h"

#include "Handlers/Niagara/NiagaraInstanceUtils.h"

#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraDataInterfaceVector2DCurve.h"
#include "NiagaraDataInterfaceVector4Curve.h"
#include "NiagaraDataInterfaceVectorCurve.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"

namespace
{
    using NiagaraJsonHelpers::SendNiagaraEditError;
    using NiagaraJsonHelpers::BeginEmitterMutationScope;
    using NiagaraJsonHelpers::EndEmitterMutationScope;

    // Two-pass lookup matching remove_data_interface (NiagaraAdvancedEditHandler.cpp:580):
    // first try the literal name (emitter-scoped stores etc.), then `User.<name>` so the
    // User redirection store also resolves. Returns the bound DI pointer via the parameter
    // store's offset table, plus the resolved FNiagaraVariable for diagnostic echoing.
    UNiagaraDataInterface* ResolveBoundDataInterface(
        FNiagaraParameterStore& Store,
        const FString& ParameterName,
        FNiagaraVariable& OutVariable)
    {
        const FName TargetName(*ParameterName);
        const FName UserNamespacedName(*FString::Printf(TEXT("User.%s"), *ParameterName));

        TArray<FNiagaraVariable> Existing;
        Store.GetParameters(Existing);
        for (const FNiagaraVariable& Var : Existing)
        {
            if (Var.GetName() == TargetName || Var.GetName() == UserNamespacedName)
            {
                const FNiagaraVariableWithOffset* WithOffset = Store.FindParameterVariable(Var);
                if (!WithOffset)
                {
                    continue;
                }
                const TArray<UNiagaraDataInterface*>& DIs = Store.GetDataInterfaces();
                if (WithOffset->Offset < 0 || WithOffset->Offset >= DIs.Num())
                {
                    continue;
                }
                OutVariable = Var;
                return DIs[WithOffset->Offset];
            }
        }
        return nullptr;
    }

    // One channel of an inline curve DI. Name is the wire selector ("" for the scalar DI, which
    // takes no channel).
    struct FCurveChannel
    {
        FString Name;
        FRichCurve* Curve = nullptr;
    };

    // The single channel table for the five inline curve DIs. Engine member names use the SUFFIX
    // form (RedCurve / XCurve / WCurve), not the prefix form. Returns false when DI is not one of
    // the five — callers map that to INCOMPATIBLE_DATA_INTERFACE. Both the write (PickCurveMember)
    // and the read (get_curve_keys) paths go through here so a channel cannot exist for one and
    // not the other.
    bool CollectCurveMembers(UNiagaraDataInterface* DI, TArray<FCurveChannel>& OutChannels)
    {
        OutChannels.Reset();
        if (!DI)
        {
            return false;
        }
        if (UNiagaraDataInterfaceCurve* Scalar = Cast<UNiagaraDataInterfaceCurve>(DI))
        {
            OutChannels.Add(FCurveChannel{FString(), &Scalar->Curve});
            return true;
        }
        if (UNiagaraDataInterfaceColorCurve* Color = Cast<UNiagaraDataInterfaceColorCurve>(DI))
        {
            OutChannels.Add(FCurveChannel{TEXT("r"), &Color->RedCurve});
            OutChannels.Add(FCurveChannel{TEXT("g"), &Color->GreenCurve});
            OutChannels.Add(FCurveChannel{TEXT("b"), &Color->BlueCurve});
            OutChannels.Add(FCurveChannel{TEXT("a"), &Color->AlphaCurve});
            return true;
        }
        if (UNiagaraDataInterfaceVectorCurve* Vec = Cast<UNiagaraDataInterfaceVectorCurve>(DI))
        {
            OutChannels.Add(FCurveChannel{TEXT("x"), &Vec->XCurve});
            OutChannels.Add(FCurveChannel{TEXT("y"), &Vec->YCurve});
            OutChannels.Add(FCurveChannel{TEXT("z"), &Vec->ZCurve});
            return true;
        }
        if (UNiagaraDataInterfaceVector2DCurve* Vec2 = Cast<UNiagaraDataInterfaceVector2DCurve>(DI))
        {
            OutChannels.Add(FCurveChannel{TEXT("x"), &Vec2->XCurve});
            OutChannels.Add(FCurveChannel{TEXT("y"), &Vec2->YCurve});
            return true;
        }
        if (UNiagaraDataInterfaceVector4Curve* Vec4 = Cast<UNiagaraDataInterfaceVector4Curve>(DI))
        {
            OutChannels.Add(FCurveChannel{TEXT("x"), &Vec4->XCurve});
            OutChannels.Add(FCurveChannel{TEXT("y"), &Vec4->YCurve});
            OutChannels.Add(FCurveChannel{TEXT("z"), &Vec4->ZCurve});
            OutChannels.Add(FCurveChannel{TEXT("w"), &Vec4->WCurve});
            return true;
        }
        return false;
    }

    // Pick the FRichCurve member matching `Channel` (case-insensitive). Returns nullptr if the
    // channel is missing or the DI is not a known curve type — callers map that to a
    // CHANNEL_MISMATCH / INCOMPATIBLE_DATA_INTERFACE error.
    //
    // OutChannelEcho is the canonical channel name written into the response so callers see
    // exactly which member was written (e.g. "" for scalar, "r" for red, "x" for X).
    FRichCurve* PickCurveMember(
        UNiagaraDataInterface* DI,
        const FString& Channel,
        FString& OutChannelEcho,
        FString& OutErrorMessage)
    {
        if (!DI)
        {
            OutErrorMessage = TEXT("Data interface is null.");
            return nullptr;
        }

        TArray<FCurveChannel> Channels;
        if (!CollectCurveMembers(DI, Channels))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Data interface class '%s' is not a known curve DI."), *DI->GetClass()->GetPathName());
            return nullptr;
        }

        if (Channels.Num() == 1 && Channels[0].Name.IsEmpty())
        {
            if (!Channel.IsEmpty())
            {
                OutErrorMessage = FString::Printf(
                    TEXT("Scalar curve DI does not accept a channel ('%s' supplied)."), *Channel);
                return nullptr;
            }
            OutChannelEcho = FString();
            return Channels[0].Curve;
        }

        const FString ChannelLower = Channel.ToLower();
        for (const FCurveChannel& Entry : Channels)
        {
            if (Entry.Name == ChannelLower)
            {
                OutChannelEcho = Entry.Name;
                return Entry.Curve;
            }
        }

        TArray<FString> ChannelNames;
        for (const FCurveChannel& Entry : Channels)
        {
            ChannelNames.Add(FString::Printf(TEXT("'%s'"), *Entry.Name));
        }
        OutErrorMessage = FString::Printf(
            TEXT("%s requires channel %s (got '%s')."),
            *DI->GetClass()->GetName(), *FString::Join(ChannelNames, TEXT("/")), *Channel);
        return nullptr;
    }

    bool ParseInterpMode(const FString& Text, ERichCurveInterpMode& OutMode, FString& OutError)
    {
        if (Text.IsEmpty()) { OutMode = RCIM_Linear; return true; }
        const FString Lower = Text.ToLower();
        if (Lower == TEXT("constant")) { OutMode = RCIM_Constant; return true; }
        if (Lower == TEXT("linear"))   { OutMode = RCIM_Linear;   return true; }
        if (Lower == TEXT("cubic"))    { OutMode = RCIM_Cubic;    return true; }
        OutError = FString::Printf(TEXT("Unknown interp mode '%s' (expected constant|linear|cubic)."), *Text);
        return false;
    }

    bool ParseTangentWeightMode(const FString& Text, ERichCurveTangentWeightMode& OutMode, FString& OutError)
    {
        if (Text.IsEmpty()) { OutMode = RCTWM_WeightedNone; return true; }
        const FString Lower = Text.ToLower();
        if (Lower == TEXT("none"))   { OutMode = RCTWM_WeightedNone;   return true; }
        if (Lower == TEXT("arrive")) { OutMode = RCTWM_WeightedArrive; return true; }
        if (Lower == TEXT("leave"))  { OutMode = RCTWM_WeightedLeave;  return true; }
        if (Lower == TEXT("both"))   { OutMode = RCTWM_WeightedBoth;   return true; }
        OutError = FString::Printf(TEXT("Unknown tangent weight mode '%s' (expected none|arrive|leave|both)."), *Text);
        return false;
    }

    // Read-side inverses of the two parsers above, so what get_curve_keys emits is what
    // set_curve_keys accepts.
    FString InterpModeToString(ERichCurveInterpMode Mode)
    {
        switch (Mode)
        {
        case RCIM_Constant: return TEXT("constant");
        case RCIM_Cubic:    return TEXT("cubic");
        case RCIM_Linear:   return TEXT("linear");
        default:            return TEXT("none");
        }
    }

    FString TangentWeightModeToString(ERichCurveTangentWeightMode Mode)
    {
        switch (Mode)
        {
        case RCTWM_WeightedArrive: return TEXT("arrive");
        case RCTWM_WeightedLeave:  return TEXT("leave");
        case RCTWM_WeightedBoth:   return TEXT("both");
        default:                   return TEXT("none");
        }
    }

    TSharedPtr<FJsonObject> BuildCurveKeyJson(const FRichCurveKey& Key)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("time"), Key.Time);
        Object->SetNumberField(TEXT("value"), Key.Value);
        Object->SetStringField(TEXT("interp"), InterpModeToString(Key.InterpMode));
        Object->SetStringField(TEXT("tangentWeightMode"), TangentWeightModeToString(Key.TangentWeightMode));
        Object->SetNumberField(TEXT("arriveTangent"), Key.ArriveTangent);
        Object->SetNumberField(TEXT("leaveTangent"), Key.LeaveTangent);
        Object->SetNumberField(TEXT("arriveTangentWeight"), Key.ArriveTangentWeight);
        Object->SetNumberField(TEXT("leaveTangentWeight"), Key.LeaveTangentWeight);
        return Object;
    }

    // ---------------------------------------------------------------------------
    // Shared addressing for the three curve verbs (see the file header for the two forms).
    // ---------------------------------------------------------------------------
    struct FCurveDataInterfaceTarget
    {
        FNiagaraResolvedTarget Target;
        FNiagaraEditOptions Options;
        UNiagaraDataInterface* DataInterface = nullptr;

        bool bModuleInput = false;
        bool bCreatedOverride = false;
        // False only for a module input still at its script default, whose DI belongs to the
        // module ASSET and is shared by every placement of it.
        bool bWritable = false;
        FString ValueMode;

        // Echoes, filled for whichever form the caller used.
        FString Scope;
        FString ParameterName;
        FString EntryId;
        FString InputName;
    };

    // Parse the payload and resolve the asset-side target. Mutates nothing. Returns false after
    // sending the error.
    bool ResolveCurveTargetAddress(FHandlerContext& Ctx, FCurveDataInterfaceTarget& Out)
    {
        const FString AssetPath = Ctx.GetString(TEXT("assetPath"));
        if (AssetPath.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
            return false;
        }
        const FString EmitterName = Ctx.GetString(TEXT("emitter"));

        Out.EntryId = Ctx.GetString(TEXT("entryId"));
        Out.InputName = Ctx.GetString(TEXT("inputName"));
        Out.Scope = Ctx.GetString(TEXT("scope"));
        Out.ParameterName = Ctx.GetString(TEXT("parameterName"));
        if (Out.ParameterName.IsEmpty())
        {
            Out.ParameterName = Ctx.GetString(TEXT("name"));
        }

        const bool bHasModuleKeys = !Out.EntryId.IsEmpty() || !Out.InputName.IsEmpty();
        const bool bHasStoreKeys = !Out.Scope.IsEmpty() || !Out.ParameterName.IsEmpty();
        if (bHasModuleKeys && bHasStoreKeys)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("Send EITHER the parameter-store form ('scope' + 'parameterName') or the module-input form ")
                TEXT("('entryId' + 'inputName'), not both — they address different objects."));
            return false;
        }
        if (!bHasModuleKeys && !bHasStoreKeys)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("Address the curve either by parameter store ('scope' + 'parameterName') or by stack module ")
                TEXT("input ('entryId' + 'inputName'). Stock module curves (ScaleSpriteSize, ScaleColor) are the ")
                TEXT("module-input form."));
            return false;
        }

        FNiagaraEditTargetSpec Spec;
        if (bHasModuleKeys)
        {
            if (Out.EntryId.IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Module-input form requires 'entryId'."));
                return false;
            }
            if (Out.InputName.IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Module-input form requires 'inputName'."));
                return false;
            }
            Out.bModuleInput = true;
            Spec.Kind = ENiagaraEditTargetKind::Module;
            Spec.KindText = NiagaraEdit::TargetKindToString(ENiagaraEditTargetKind::Module);
            Spec.EmitterName = EmitterName;
            Spec.ScriptUsage = Ctx.GetString(TEXT("scriptUsage"));
            // Accept the owner-qualified entryKey "<owner>:<nodeGuid>" in the same slot every
            // other module verb does; a bare node guid is unique only within one emitter.
            FString EntryOwner;
            FString BareEntryId;
            if (NiagaraEdit::TrySplitModuleEntryKey(Out.EntryId, EntryOwner, BareEntryId))
            {
                Spec.EntryOwner = EntryOwner;
                Spec.EntryId = BareEntryId;
            }
            else
            {
                Spec.EntryId = Out.EntryId;
            }
            Out.EntryId = Spec.EntryId;
        }
        else
        {
            if (Out.Scope.IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Parameter-store form requires 'scope'."));
                return false;
            }
            if (Out.ParameterName.IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Parameter-store form requires 'parameterName'."));
                return false;
            }
            Spec.Kind = ENiagaraEditTargetKind::ParameterStore;
            Spec.KindText = NiagaraEdit::TargetKindToString(ENiagaraEditTargetKind::ParameterStore);
            Spec.Scope = Out.Scope;
            Spec.EmitterName = EmitterName;
        }

        if (SendNiagaraEditError(Ctx, NiagaraEdit::ResolveTarget(AssetPath, Spec, Out.Target)))
        {
            return false;
        }
        if (!Out.bModuleInput && !Out.Target.ParameterStore)
        {
            Ctx.SendError(TEXT("PARAMETER_STORE_NOT_FOUND"),
                FString::Printf(TEXT("Parameter store for scope '%s' is not available."), *Out.Scope));
            return false;
        }
        return true;
    }

    // Resolve the DI itself. For the module form under bForWrite this CREATES the override pin
    // and DI when the input is still at its script default, so a write caller must already hold
    // the transaction, have run BeginEmitterMutationScope and Modify()'d the graph. Returns false
    // after sending the error.
    bool ResolveCurveDataInterface(FHandlerContext& Ctx, bool bForWrite, FCurveDataInterfaceTarget& InOut)
    {
        if (InOut.bModuleInput)
        {
            NiagaraModuleInputDI::FResolvedModuleInputDI Resolved;
            const NiagaraModuleInputDI::EResolveMode Mode = bForWrite
                ? NiagaraModuleInputDI::EResolveMode::WriteCreateOverride
                : NiagaraModuleInputDI::EResolveMode::Read;
            if (SendNiagaraEditError(Ctx,
                    NiagaraModuleInputDI::ResolveModuleInputDataInterface(InOut.Target, InOut.InputName, Mode, Resolved)))
            {
                return false;
            }
            InOut.DataInterface = Resolved.DataInterface;
            InOut.ValueMode = Resolved.ValueMode;
            InOut.bCreatedOverride = Resolved.bCreatedOverride;
            InOut.bWritable = Resolved.bWritable;
            InOut.InputName = Resolved.ResolvedInputName;
            return true;
        }

        FNiagaraVariable ResolvedVar;
        InOut.DataInterface = ResolveBoundDataInterface(*InOut.Target.ParameterStore, InOut.ParameterName, ResolvedVar);
        if (!InOut.DataInterface)
        {
            // Name the other addressing form rather than leaving the caller unable to tell a wrong
            // name from a wrong scope from a target no store can hold — the state this ticket was
            // filed for, after three reporters exhausted every spelling.
            Ctx.SendError(TEXT("DATA_INTERFACE_NOT_FOUND"),
                FString::Printf(
                    TEXT("Parameter '%s' was not found in scope '%s'. A curve on a stack module's input ")
                    TEXT("(ScaleSpriteSize's 'Uniform Curve Sprite Scale', ScaleColor's alpha curve) is not in any ")
                    TEXT("parameter store — address it with entryId + inputName instead."),
                    *InOut.ParameterName, *InOut.Scope));
            return false;
        }
        InOut.ValueMode = TEXT("data");
        InOut.bWritable = true;
        return true;
    }

    // Echo the addressing the call actually used, so a response never carries an empty `scope`
    // that reads as "the store form failed" on a module-input call.
    void WriteCurveAddressingEcho(const TSharedPtr<FJsonObject>& Result, const FCurveDataInterfaceTarget& CurveTarget)
    {
        Result->SetStringField(TEXT("addressing"),
            CurveTarget.bModuleInput ? TEXT("moduleInput") : TEXT("parameterStore"));
        Result->SetStringField(TEXT("valueMode"), CurveTarget.ValueMode);
        if (CurveTarget.bModuleInput)
        {
            Result->SetStringField(TEXT("entryId"), CurveTarget.EntryId);
            Result->SetStringField(TEXT("inputName"), CurveTarget.InputName);
            Result->SetBoolField(TEXT("createdOverride"), CurveTarget.bCreatedOverride);
        }
        else
        {
            Result->SetStringField(TEXT("scope"), CurveTarget.Scope);
            Result->SetStringField(TEXT("parameterName"), CurveTarget.ParameterName);
        }
    }

    // Shape-compatibility check for bind_curve_asset. The engine's per-subclass
    // SyncCurvesToAsset() implementations expect specific UCurve types:
    //   * scalar curve DI            -> UCurveFloat (single FloatCurve)
    //   * color / vector / vector2D / vector4 -> UCurveLinearColor (4 FloatCurves[0..3])
    // Mismatch produces a Sync that silently zeros channels, so reject up front with
    // INCOMPATIBLE_CURVE_ASSET. Returns true if compatible.
    bool IsCurveAssetShapeCompatible(UNiagaraDataInterfaceCurveBase* DI, UCurveBase* Curve, FString& OutErrorMessage)
    {
        if (!DI || !Curve)
        {
            OutErrorMessage = TEXT("Internal error: null DI or curve in shape check.");
            return false;
        }
        const bool bScalarDI = DI->IsA<UNiagaraDataInterfaceCurve>();
        if (bScalarDI)
        {
            if (!Curve->IsA<UCurveFloat>())
            {
                OutErrorMessage = FString::Printf(
                    TEXT("Scalar curve DI requires UCurveFloat asset (got %s)."), *Curve->GetClass()->GetPathName());
                return false;
            }
            return true;
        }
        // All multi-channel curve DIs read up to 4 channels from a UCurveLinearColor.
        if (!Curve->IsA<UCurveLinearColor>())
        {
            OutErrorMessage = FString::Printf(
                TEXT("Multi-channel curve DI requires UCurveLinearColor asset (got %s)."), *Curve->GetClass()->GetPathName());
            return false;
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// niagara.set_curve_keys
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("niagara.set_curve_keys", "niagara",
    "Replace the keys on an inline Niagara curve data interface (UNiagaraDataInterfaceCurve / Color / Vector / Vector2D / Vector4), addressed by parameter store or by stack module input.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("keys", "array", "Array of {time, value, interp?, arriveTangent?, leaveTangent?, arriveTangentWeight?, leaveTangentWeight?, tangentWeightMode?}"),
        RPC_PARAM_OPT("scope", "string", "Parameter-store form: parameter scope (user, rendererBindings, spawnRapidIteration, ...). Pair with `parameterName`. Mutually exclusive with entryId/inputName."),
        RPC_PARAM_OPT_ALIAS("parameterName", "string", "Parameter-store form: name of the bound curve DI within `scope`. The shorter spelling `name` is accepted for the same slot. NOTE: no parameter store holds a stack module's curve input - use entryId + inputName for those.", "name"),
        RPC_PARAM_OPT("entryId", "string", "Module-input form: module entry id, or the owner-qualified entryKey 'owner:id', exactly as niagara.set_module_input takes it. Pair with `inputName`."),
        RPC_PARAM_OPT("inputName", "string", "Module-input form: the module's curve input, e.g. 'Uniform Curve Sprite Scale' on ScaleSpriteSize. An input still at its script default gets its own override data interface (seeded from that default) before the write, so the module asset is never edited."),
        RPC_PARAM_OPT("emitter", "string", "Emitter name. Required for the emitter-scoped stores (rendererBindings, spawnRapidIteration, updateRapidIteration) and for module inputs on a Niagara System; the system-wide scopes (user, systemSpawnRapidIteration, systemUpdateRapidIteration) reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("scriptUsage", "string", "Module-input form only: script usage of the stack the module lives in. Omit to infer it from the resolved module."),
        RPC_PARAM_OPT("channel", "string", "Channel selector for multi-channel curves (r/g/b/a or x/y/z/w). Omit for scalar DIs."),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FCurveDataInterfaceTarget CurveTarget;
    if (!ResolveCurveTargetAddress(Ctx, CurveTarget)) return true;
    CurveTarget.Options.bCompile = Ctx.GetBool(TEXT("compile"), false);
    CurveTarget.Options.bSave = Ctx.GetBool(TEXT("save"), false);

    const FString Channel = Ctx.GetString(TEXT("channel"));

    const TArray<TSharedPtr<FJsonValue>>* KeysArray = Ctx.GetArray(TEXT("keys"));
    if (!KeysArray)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("'keys' must be an array."));
        return true;
    }

    FNiagaraResolvedTarget& Target = CurveTarget.Target;

    // Apply keys inside a transaction so the entire write is undoable as a unit. The DI is
    // resolved INSIDE it because the module-input form may have to create the override pin and
    // its data interface first, which is a graph mutation — the same ordering set_module_input
    // uses, and the reason a refusal cancels rather than merely returning.
    FString ChannelEcho;
    FString DataInterfaceClass;
    int32 WrittenCount = 0;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_curve_keys")));
        BeginEmitterMutationScope(Target);
        if (CurveTarget.bModuleInput && Target.Graph)
        {
            Target.Graph->Modify();
        }

        if (!ResolveCurveDataInterface(Ctx, /*bForWrite=*/true, CurveTarget))
        {
            Transaction.Cancel();
            return true;
        }

        UNiagaraDataInterface* DI = CurveTarget.DataInterface;
        DataInterfaceClass = DI->GetClass()->GetPathName();

        FString PickError;
        FRichCurve* Curve = PickCurveMember(DI, Channel, ChannelEcho, PickError);
        if (!Curve)
        {
            // Distinguish channel mismatch (known DI, wrong/missing channel) from incompatible class.
            TArray<FCurveChannel> Probe;
            const bool bIsCurveDI = CollectCurveMembers(DI, Probe);
            Transaction.Cancel();
            Ctx.SendError(bIsCurveDI ? TEXT("CHANNEL_MISMATCH") : TEXT("INCOMPATIBLE_DATA_INTERFACE"), PickError);
            return true;
        }

        UNiagaraDataInterfaceCurveBase* CurveDI = Cast<UNiagaraDataInterfaceCurveBase>(DI);
        // PickCurveMember already enforced this — guard anyway so a future refactor can't bypass it.
        if (!CurveDI)
        {
            Transaction.Cancel();
            Ctx.SendError(TEXT("INCOMPATIBLE_DATA_INTERFACE"),
                FString::Printf(TEXT("Resolved DI '%s' does not derive from UNiagaraDataInterfaceCurveBase."),
                    *DataInterfaceClass));
            return true;
        }

        CurveDI->Modify();

        // Reset first so old keys don't linger — this RPC is "set", not "append".
        Curve->Reset();

        for (int32 Index = 0; Index < KeysArray->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& KeyValue = (*KeysArray)[Index];
            const TSharedPtr<FJsonObject>* KeyObjectPtr = nullptr;
            if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObjectPtr) || !KeyObjectPtr || !KeyObjectPtr->IsValid())
            {
                Transaction.Cancel();
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("keys[%d] must be a JSON object."), Index));
                return true;
            }
            const TSharedPtr<FJsonObject>& KeyObject = *KeyObjectPtr;

            double Time = 0.0;
            double Value = 0.0;
            if (!KeyObject->TryGetNumberField(TEXT("time"), Time))
            {
                Transaction.Cancel();
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("keys[%d] is missing required field 'time'."), Index));
                return true;
            }
            if (!KeyObject->TryGetNumberField(TEXT("value"), Value))
            {
                Transaction.Cancel();
                Ctx.SendError(TEXT("INVALID_PARAMS"),
                    FString::Printf(TEXT("keys[%d] is missing required field 'value'."), Index));
                return true;
            }

            ERichCurveInterpMode InterpMode = RCIM_Linear;
            FString InterpText;
            if (KeyObject->TryGetStringField(TEXT("interp"), InterpText))
            {
                FString InterpError;
                if (!ParseInterpMode(InterpText, InterpMode, InterpError))
                {
                    Transaction.Cancel();
                    Ctx.SendError(TEXT("INVALID_PARAMS"),
                        FString::Printf(TEXT("keys[%d]: %s"), Index, *InterpError));
                    return true;
                }
            }

            ERichCurveTangentWeightMode TangentWeightMode = RCTWM_WeightedNone;
            FString WeightModeText;
            if (KeyObject->TryGetStringField(TEXT("tangentWeightMode"), WeightModeText))
            {
                FString WeightError;
                if (!ParseTangentWeightMode(WeightModeText, TangentWeightMode, WeightError))
                {
                    Transaction.Cancel();
                    Ctx.SendError(TEXT("INVALID_PARAMS"),
                        FString::Printf(TEXT("keys[%d]: %s"), Index, *WeightError));
                    return true;
                }
            }

            const FKeyHandle Handle = Curve->AddKey(static_cast<float>(Time), static_cast<float>(Value));
            FRichCurveKey& Key = Curve->GetKey(Handle);
            Key.InterpMode = InterpMode;
            Key.TangentWeightMode = TangentWeightMode;

            double Number = 0.0;
            if (KeyObject->TryGetNumberField(TEXT("arriveTangent"), Number))
            {
                Key.ArriveTangent = static_cast<float>(Number);
            }
            if (KeyObject->TryGetNumberField(TEXT("leaveTangent"), Number))
            {
                Key.LeaveTangent = static_cast<float>(Number);
            }
            if (KeyObject->TryGetNumberField(TEXT("arriveTangentWeight"), Number))
            {
                Key.ArriveTangentWeight = static_cast<float>(Number);
            }
            if (KeyObject->TryGetNumberField(TEXT("leaveTangentWeight"), Number))
            {
                Key.LeaveTangentWeight = static_cast<float>(Number);
            }

            ++WrittenCount;
        }

        // Refresh the LUT the runtime samplers use. Without this, the new keys edit only the
        // editor-side FRichCurve and live preview instances keep sampling stale values.
        CurveDI->UpdateLUT();

        // A created override pin is a graph change; the module-input form must announce it or the
        // open stack keeps showing the input at its script default.
        if (CurveTarget.bModuleInput && Target.Graph)
        {
            Target.Graph->NotifyGraphChanged();
        }
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("set_curve_keys"), Target, CurveTarget.Options);
    WriteCurveAddressingEcho(Result, CurveTarget);
    Result->SetStringField(TEXT("dataInterfaceClass"), DataInterfaceClass);
    Result->SetStringField(TEXT("channel"), ChannelEcho);
    Result->SetNumberField(TEXT("written"), WrittenCount);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// niagara.bind_curve_asset
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("niagara.bind_curve_asset", "niagara",
    "Bind a UCurveFloat / UCurveLinearColor asset to a Niagara curve data interface (sets CurveAsset + SyncCurvesToAsset + UpdateLUT).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("curveAssetPath", "path", "Path to the UCurveFloat or UCurveLinearColor asset"),
        RPC_PARAM_OPT("scope", "string", "Parameter-store form: parameter scope (user, rendererBindings, spawnRapidIteration, ...). Pair with `parameterName`. Mutually exclusive with entryId/inputName."),
        RPC_PARAM_OPT_ALIAS("parameterName", "string", "Parameter-store form: name of the bound curve DI within `scope`. The shorter spelling `name` is accepted for the same slot. NOTE: no parameter store holds a stack module's curve input - use entryId + inputName for those.", "name"),
        RPC_PARAM_OPT("entryId", "string", "Module-input form: module entry id, or the owner-qualified entryKey 'owner:id', exactly as niagara.set_module_input takes it. Pair with `inputName`."),
        RPC_PARAM_OPT("inputName", "string", "Module-input form: the module's curve input, e.g. 'Uniform Curve Sprite Scale' on ScaleSpriteSize. An input still at its script default gets its own override data interface before the bind, so the module asset is never edited."),
        RPC_PARAM_OPT("emitter", "string", "Emitter name. Required for the emitter-scoped stores (rendererBindings, spawnRapidIteration, updateRapidIteration) and for module inputs on a Niagara System; the system-wide scopes (user, systemSpawnRapidIteration, systemUpdateRapidIteration) reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("scriptUsage", "string", "Module-input form only: script usage of the stack the module lives in. Omit to infer it from the resolved module."),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FCurveDataInterfaceTarget CurveTarget;
    if (!ResolveCurveTargetAddress(Ctx, CurveTarget)) return true;
    CurveTarget.Options.bCompile = Ctx.GetBool(TEXT("compile"), false);
    CurveTarget.Options.bSave = Ctx.GetBool(TEXT("save"), false);

    FString CurveAssetPath;
    if (!Ctx.RequireString(TEXT("curveAssetPath"), CurveAssetPath)) return true;

    UCurveBase* LoadedCurve = LoadObject<UCurveBase>(nullptr, *CurveAssetPath);
    if (!LoadedCurve)
    {
        Ctx.SendError(TEXT("CURVE_ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load UCurveBase asset at '%s'."), *CurveAssetPath));
        return true;
    }

    FNiagaraResolvedTarget& Target = CurveTarget.Target;
    FString DataInterfaceClass;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.bind_curve_asset")));
        BeginEmitterMutationScope(Target);
        if (CurveTarget.bModuleInput && Target.Graph)
        {
            Target.Graph->Modify();
        }

        if (!ResolveCurveDataInterface(Ctx, /*bForWrite=*/true, CurveTarget))
        {
            Transaction.Cancel();
            return true;
        }

        UNiagaraDataInterface* DI = CurveTarget.DataInterface;
        DataInterfaceClass = DI->GetClass()->GetPathName();

        UNiagaraDataInterfaceCurveBase* CurveDI = Cast<UNiagaraDataInterfaceCurveBase>(DI);
        if (!CurveDI)
        {
            Transaction.Cancel();
            Ctx.SendError(TEXT("INCOMPATIBLE_DATA_INTERFACE"),
                FString::Printf(TEXT("Target resolves to '%s', which does not derive from UNiagaraDataInterfaceCurveBase."),
                    *DataInterfaceClass));
            return true;
        }

        FString ShapeError;
        if (!IsCurveAssetShapeCompatible(CurveDI, LoadedCurve, ShapeError))
        {
            Transaction.Cancel();
            Ctx.SendError(TEXT("INCOMPATIBLE_CURVE_ASSET"), ShapeError);
            return true;
        }

        CurveDI->Modify();

        // The base class carries CurveAsset as an editor-only TObjectPtr<UCurveBase>. Assigning
        // it and calling SyncCurvesToAsset() is the same path the editor detail customization uses
        // when the user picks an asset in the picker.
        CurveDI->CurveAsset = LoadedCurve;
        CurveDI->SyncCurvesToAsset();
        CurveDI->UpdateLUT();

        if (CurveTarget.bModuleInput && Target.Graph)
        {
            Target.Graph->NotifyGraphChanged();
        }
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("bind_curve_asset"), Target, CurveTarget.Options);
    WriteCurveAddressingEcho(Result, CurveTarget);
    Result->SetStringField(TEXT("dataInterfaceClass"), DataInterfaceClass);
    Result->SetStringField(TEXT("curveAsset"), LoadedCurve->GetPathName());
    Result->SetBoolField(TEXT("bound"), true);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// niagara.get_curve_keys
//
// The read half of the same addressing. Four of the nine encounters on
// B-niagara-set-curve-keys-unreachable-module-input-di are blocked by the read alone: neither
// niagara.inspect {includeGraphs:true}, nor asset.dump's nir.txt, nor niagara.decompile_nir emits
// curve samples, so a caller could not see the ramp it was failing to change — which turned an
// ordinary edit on a shared asset into a blind clobber.
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("niagara.get_curve_keys", "niagara",
    "Read the keys of an inline Niagara curve data interface, addressed by parameter store or by stack module input.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_OPT("scope", "string", "Parameter-store form: parameter scope (user, rendererBindings, spawnRapidIteration, ...). Pair with `parameterName`. Mutually exclusive with entryId/inputName."),
        RPC_PARAM_OPT_ALIAS("parameterName", "string", "Parameter-store form: name of the bound curve DI within `scope`. The shorter spelling `name` is accepted for the same slot.", "name"),
        RPC_PARAM_OPT("entryId", "string", "Module-input form: module entry id, or the owner-qualified entryKey 'owner:id'. Pair with `inputName`."),
        RPC_PARAM_OPT("inputName", "string", "Module-input form: the module's curve input, e.g. 'Uniform Curve Sprite Scale'. Reads the module script's own default object when nothing has overridden the input; the response says which via `valueMode`."),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for the emitter-scoped stores and for module inputs on a Niagara System"),
        RPC_PARAM_OPT("scriptUsage", "string", "Module-input form only: script usage of the stack the module lives in. Omit to infer it from the resolved module."),
        RPC_PARAM_OPT("channel", "string", "Return only this channel (r/g/b/a or x/y/z/w). Omit to return every channel the data interface has.")
    ))
{
    FCurveDataInterfaceTarget CurveTarget;
    if (!ResolveCurveTargetAddress(Ctx, CurveTarget)) return true;
    if (!ResolveCurveDataInterface(Ctx, /*bForWrite=*/false, CurveTarget)) return true;

    UNiagaraDataInterface* DI = CurveTarget.DataInterface;
    TArray<FCurveChannel> Channels;
    if (!CollectCurveMembers(DI, Channels))
    {
        Ctx.SendError(TEXT("INCOMPATIBLE_DATA_INTERFACE"),
            FString::Printf(TEXT("Data interface class '%s' is not a known curve DI."), *DI->GetClass()->GetPathName()));
        return true;
    }

    const FString Channel = Ctx.GetString(TEXT("channel"));
    if (!Channel.IsEmpty())
    {
        FString ChannelEcho;
        FString PickError;
        FRichCurve* Picked = PickCurveMember(DI, Channel, ChannelEcho, PickError);
        if (!Picked)
        {
            Ctx.SendError(TEXT("CHANNEL_MISMATCH"), PickError);
            return true;
        }
        Channels.Reset();
        Channels.Add(FCurveChannel{ChannelEcho, Picked});
    }

    TArray<TSharedPtr<FJsonValue>> CurvesJson;
    for (const FCurveChannel& Entry : Channels)
    {
        TSharedPtr<FJsonObject> CurveJson = MakeShared<FJsonObject>();
        CurveJson->SetStringField(TEXT("channel"), Entry.Name);
        TArray<TSharedPtr<FJsonValue>> KeysJson;
        if (Entry.Curve)
        {
            for (const FRichCurveKey& Key : Entry.Curve->Keys)
            {
                KeysJson.Add(NiagaraJsonHelpers::MakeObjectValue(BuildCurveKeyJson(Key)));
            }
        }
        CurveJson->SetArrayField(TEXT("keys"), KeysJson);
        CurvesJson.Add(NiagaraJsonHelpers::MakeObjectValue(CurveJson));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    WriteCurveAddressingEcho(Result, CurveTarget);
    Result->SetStringField(TEXT("dataInterfaceClass"), DI->GetClass()->GetPathName());
    Result->SetArrayField(TEXT("curves"), CurvesJson);
    // A "default" valueMode means these keys come from the module SCRIPT's shared object, so they
    // describe every placement of that module rather than this emitter's own authored ramp.
    Result->SetBoolField(TEXT("writable"), CurveTarget.bWritable);
    if (UNiagaraDataInterfaceCurveBase* CurveDI = Cast<UNiagaraDataInterfaceCurveBase>(DI))
    {
        Result->SetStringField(TEXT("curveAsset"),
            CurveDI->CurveAsset ? CurveDI->CurveAsset->GetPathName() : FString());
    }
    Ctx.SendSuccess(Result);
    return true;
}
