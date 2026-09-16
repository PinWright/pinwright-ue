// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRTextEmitter.h"

#include "Compat/EngineVersionCompat.h"
#include "CRIR/CRIRControlValueParser.h"
#include "CRIR/CRIRGrammar.h"
#include "IrCore/IrTextUtils.h"

#include "Rigs/RigHierarchyElements.h"
#include "UObject/UnrealType.h"

namespace
{
FString FormatPositionSuffix(const FCRIRPosition& Position)
{
    if (!Position.bSet)
    {
        return FString();
    }
    // Round to int for stable text output; CRIR positions are screen pixels and
    // sub-pixel precision in the source asset is editorial noise that would
    // churn diffs on every save. Routed through the shared IrCore helper so
    // CRIR's `@(x, y)` formatting matches AGIR/BTIR/MGIR/MSIR exactly.
    return FIrTextUtils::FormatPositionSuffix(
        FMath::RoundToInt(Position.X),
        FMath::RoundToInt(Position.Y));
}

FString FormatArg(const FCRIRArg& Arg)
{
    if (Arg.bIsLocalRef)
    {
        return FString::Printf(TEXT("%s=%s"),
            *Arg.Name,
            *FCRIRTextEmitter::FormatLocalRef(Arg.LocalRefNode, Arg.LocalRefPin));
    }
    return FString::Printf(TEXT("%s=%s"), *Arg.Name, *Arg.RawText);
}

FString JoinArgs(const TArray<FCRIRArg>& Args)
{
    if (Args.Num() == 0)
    {
        return FString();
    }
    TArray<FString> Parts;
    Parts.Reserve(Args.Num());
    for (const FCRIRArg& Arg : Args)
    {
        Parts.Add(FormatArg(Arg));
    }
    return FString::Join(Parts, TEXT(", "));
}
} // namespace

FString FCRIRTextEmitter::EmitRigGraphHeader(const FString& ModelName)
{
    return FString::Printf(TEXT("rig_graph %s {"), *FIrTextUtils::Quote(ModelName));
}

FString FCRIRTextEmitter::EmitRigHierarchyHeader()
{
    return TEXT("rig_hierarchy {");
}

FString FCRIRTextEmitter::EmitRigFunctionHeader(const FString& FunctionName)
{
    return FString::Printf(TEXT("rig_function %s {"), *FIrTextUtils::Quote(FunctionName));
}

FString FCRIRTextEmitter::EmitSubgraphHeader(const FString& NodeName)
{
    return FString::Printf(TEXT("rig_subgraph %s {"), *FIrTextUtils::Quote(NodeName));
}

FString FCRIRTextEmitter::EmitBlockFooter()
{
    return TEXT("}");
}

FString FCRIRTextEmitter::EmitUnit(
    const FString& LocalId,
    const FString& StructPath,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    return FString::Printf(
        TEXT("%%%s = unit %s(%s)%s"),
        *LocalId,
        *StructPath,
        *JoinArgs(Args),
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitVar(
    const FString& LocalId,
    const FString& VarName,
    const FString& VarType,
    const FString& VarDefault,
    const FCRIRPosition& Position)
{
    // Phase A var grammar: `%nN = var Name Type [= literal] [@(x,y)]`. There is
    // no trailing arg list — CRIRParser::ParseVarInstruction does not consume
    // one, so any extra would silently round-trip-drop. Per-pin overrides are
    // out of scope for Phase A.
    FString DefaultClause;
    if (!VarDefault.IsEmpty())
    {
        DefaultClause = FString::Printf(TEXT(" = %s"), *VarDefault);
    }

    return FString::Printf(
        TEXT("%%%s = var %s %s%s%s"),
        *LocalId,
        *VarName,
        *VarType,
        *DefaultClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitReroute(
    const FString& LocalId,
    const FString& CPPType,
    const FString& DefaultValue,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString DefaultClause;
    if (!DefaultValue.IsEmpty())
    {
        DefaultClause = FString::Printf(TEXT(" = %s"), *DefaultValue);
    }
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("%%%s = reroute %s%s%s%s"),
        *LocalId,
        *CPPType,
        *DefaultClause,
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitComment(
    const FString& Text,
    const FString& Size,
    const FString& Color,
    const FCRIRPosition& Position)
{
    FString SizeClause;
    if (!Size.IsEmpty())
    {
        SizeClause = FString::Printf(TEXT(" size=%s"), *Size);
    }
    FString ColorClause;
    if (!Color.IsEmpty())
    {
        ColorClause = FString::Printf(TEXT(" color=%s"), *Color);
    }
    return FString::Printf(
        TEXT("comment %s%s%s%s"),
        *FIrTextUtils::Quote(Text),
        *SizeClause,
        *ColorClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitIf(
    const FString& LocalId,
    const FString& CPPType,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("%%%s = if %s%s%s"),
        *LocalId,
        *CPPType,
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitSelect(
    const FString& LocalId,
    const FString& CPPType,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("%%%s = select %s%s%s"),
        *LocalId,
        *CPPType,
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitEnum(
    const FString& LocalId,
    const FString& EnumObjectPath,
    const FString& Value,
    const FCRIRPosition& Position)
{
    FString ValueClause;
    if (!Value.IsEmpty())
    {
        ValueClause = FString::Printf(TEXT(" = %s"), *Value);
    }
    return FString::Printf(
        TEXT("%%%s = enum %s%s%s"),
        *LocalId,
        *EnumObjectPath,
        *ValueClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitInvokeEntry(
    const FString& LocalId,
    const FString& EntryName,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("%%%s = invoke_entry %s%s%s"),
        *LocalId,
        *FIrTextUtils::FormatNameToken(EntryName),
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitTemplate(
    const FString& LocalId,
    const FString& Notation,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("%%%s = template %s%s%s"),
        *LocalId,
        *FIrTextUtils::FormatNameToken(Notation),
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitDispatch(
    const FString& LocalId,
    const FString& FactoryStructName,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("%%%s = dispatch %s%s%s"),
        *LocalId,
        *FIrTextUtils::FormatNameToken(FactoryStructName),
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitCollapseHeader(
    const FString& LocalId,
    const FString& NodeName,
    const FCRIRPosition& Position)
{
    return FString::Printf(
        TEXT("%%%s = collapse %s%s {"),
        *LocalId,
        *FIrTextUtils::Quote(NodeName),
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitFunctionRef(
    const FString& LocalId,
    const FString& FunctionName,
    const FString& HostPath,
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    FString Token;
    if (HostPath.IsEmpty())
    {
        Token = FIrTextUtils::FormatNameToken(FunctionName);
    }
    else
    {
        // Combined `Host::Name` token may contain `/`, `.`, and `:` — the
        // formatter routes through backtick quoting when needed.
        Token = FIrTextUtils::FormatNameToken(FString::Printf(TEXT("%s::%s"), *HostPath, *FunctionName));
    }
    return FString::Printf(
        TEXT("%%%s = function_ref %s%s%s"),
        *LocalId,
        *Token,
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitFunctionEntry(
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("function_entry%s%s"),
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitFunctionReturn(
    const TArray<FCRIRArg>& Args,
    const FCRIRPosition& Position)
{
    FString ArgsClause;
    if (Args.Num() > 0)
    {
        ArgsClause = FString::Printf(TEXT("(%s)"), *JoinArgs(Args));
    }
    return FString::Printf(
        TEXT("function_return%s%s"),
        *ArgsClause,
        *FormatPositionSuffix(Position));
}

FString FCRIRTextEmitter::EmitExposedPin(
    const FString& PinName,
    ECRIRExposedPinDirection Direction,
    const FString& CPPType,
    const FString& CPPTypeObjectPath,
    const FString& DefaultValue)
{
    const TCHAR* DirText =
        (Direction == ECRIRExposedPinDirection::Output) ? TEXT("output")
        : (Direction == ECRIRExposedPinDirection::IO) ? TEXT("io")
        : TEXT("input");
    FString ObjectClause;
    if (!CPPTypeObjectPath.IsEmpty())
    {
        ObjectClause = FString::Printf(TEXT(" object=%s"), *CPPTypeObjectPath);
    }
    FString DefaultClause;
    if (!DefaultValue.IsEmpty())
    {
        DefaultClause = FString::Printf(TEXT(" = %s"), *DefaultValue);
    }
    return FString::Printf(
        TEXT("exposed_pin %s: %s %s%s%s"),
        *PinName,
        DirText,
        *CPPType,
        *ObjectClause,
        *DefaultClause);
}

FString FCRIRTextEmitter::EmitElement(
    ECRIRElementKind Kind,
    const FString& Name,
    const FString& Parent,
    const TArray<TPair<FString, FString>>& Attributes)
{
    FString ParentClause;
    if (!Parent.IsEmpty())
    {
        ParentClause = FString::Printf(TEXT(" parent=%s"), *FIrTextUtils::FormatNameToken(Parent));
    }

    TArray<FString> AttrParts;
    AttrParts.Reserve(Attributes.Num());
    for (const TPair<FString, FString>& Attr : Attributes)
    {
        AttrParts.Add(FString::Printf(TEXT("%s=%s"), *Attr.Key, *Attr.Value));
    }
    FString AttrClause;
    if (AttrParts.Num() > 0)
    {
        AttrClause = FString(TEXT(" ")) + FString::Join(AttrParts, TEXT(" "));
    }

    return FString::Printf(
        TEXT("%s %s%s%s"),
        ElementKindToText(Kind),
        *FIrTextUtils::FormatNameToken(Name),
        *ParentClause,
        *AttrClause);
}

namespace
{

// UEnum-driven enum value rendering. Uses GetNameStringByValue, which returns
// the unscoped variant identifier ("AnimationControl"). Lowercase + snake-case
// is applied via CamelToSnakeIdentifier so emitted text matches the rest of
// CRIR's lowercase-with-underscores attribute vocabulary.
template <typename TEnum>
FString FormatEnumLowerSnake(TEnum Value)
{
    const UEnum* EnumObj = StaticEnum<TEnum>();
    if (!EnumObj)
    {
        return FString::FromInt(static_cast<int32>(Value));
    }
    FString Name = EnumObj->GetNameStringByValue(static_cast<int64>(Value));
    if (Name.IsEmpty())
    {
        return FString::FromInt(static_cast<int32>(Value));
    }
    return FIrTextUtils::CamelToSnakeIdentifier(Name);
}

// True when Value matches a default-constructed (all-zero storage)
// FRigControlValue for Type. Used to elide default min/max from emission
// without paying the cost of formatting both sides to FString.
bool IsControlValueDefaultStorage(ERigControlType Type, const FRigControlValue& Value)
{
    switch (Type)
    {
    case ERigControlType::Bool:
        return Value.Get<bool>() == false;
    case ERigControlType::Float:
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    case ERigControlType::ScaleFloat:  // ERigControlType::ScaleFloat is UE 5.4+
#endif
        return Value.Get<float>() == 0.f;
    case ERigControlType::Integer:
        return Value.Get<int32>() == 0;
    case ERigControlType::Vector2D:
    case ERigControlType::Position:
    case ERigControlType::Scale:
    case ERigControlType::Rotator:
    {
        const FVector3f V = Value.Get<FVector3f>();
        return V.X == 0.f && V.Y == 0.f && V.Z == 0.f;
    }
    case ERigControlType::Transform:
    {
        const FRigControlValue::FTransform_Float S = Value.Get<FRigControlValue::FTransform_Float>();
        return S.GetTranslation() == FVector3f::ZeroVector
            && S.GetRotation().Rotator() == FRotator::ZeroRotator
            && S.GetScale3D() == FVector3f::ZeroVector;
    }
    case ERigControlType::TransformNoScale:
    {
        const FRigControlValue::FTransformNoScale_Float S = Value.Get<FRigControlValue::FTransformNoScale_Float>();
        return S.GetTranslation() == FVector3f::ZeroVector
            && S.GetRotation().Rotator() == FRotator::ZeroRotator;
    }
    case ERigControlType::EulerTransform:
    {
        const FRigControlValue::FEulerTransform_Float S = Value.Get<FRigControlValue::FEulerTransform_Float>();
        return S.GetTranslation() == FVector3f::ZeroVector
            && S.GetRotator() == FRotator::ZeroRotator
            && S.GetScale3D() == FVector3f::ZeroVector;
    }
    default:
        return true;
    }
}
} // namespace

FString FCRIRTextEmitter::FormatControlValue(ERigControlType Type, const FRigControlValue& Value)
{
    // Get<T>() is a `const`-context call via GetRef<T>() const overload. The
    // emitter passes the value by const& so casting away const is unnecessary —
    // GetRef<T>() const returns a reference into the same storage blob.
    const TCHAR* Prefix = FCRIRControlValueParser::PrefixForType(Type);
    switch (Type)
    {
    case ERigControlType::Bool:
        return FString::Printf(TEXT("%s(%s)"), Prefix, Value.Get<bool>() ? TEXT("true") : TEXT("false"));
    case ERigControlType::Float:
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    case ERigControlType::ScaleFloat:  // ERigControlType::ScaleFloat is UE 5.4+
#endif
        return FString::Printf(TEXT("%s(%g)"), Prefix, Value.Get<float>());
    case ERigControlType::Integer:
        return FString::Printf(TEXT("%s(%d)"), Prefix, Value.Get<int32>());
    case ERigControlType::Vector2D:
    {
        const FVector3f V = Value.Get<FVector3f>();
        return FString::Printf(TEXT("%s(%g,%g)"), Prefix, V.X, V.Y);
    }
    case ERigControlType::Position:
    case ERigControlType::Scale:
    {
        const FVector3f V = Value.Get<FVector3f>();
        return FString::Printf(TEXT("%s(%g,%g,%g)"), Prefix, V.X, V.Y, V.Z);
    }
    case ERigControlType::Rotator:
    {
        const FVector3f V = Value.Get<FVector3f>();
        // Storage holds euler X=Pitch, Y=Yaw, Z=Roll.
        return FString::Printf(TEXT("%s(p=%g, y=%g, r=%g)"), Prefix, V.X, V.Y, V.Z);
    }
    case ERigControlType::Transform:
    {
        const FTransform Xf = Value.Get<FRigControlValue::FTransform_Float>().ToTransform();
        return FString::Printf(TEXT("%s(loc=%s, rot=%s, scale=%s)"),
            Prefix,
            *FCRIRTextEmitter::FormatVector(Xf.GetLocation()),
            *FCRIRTextEmitter::FormatRotator(Xf.GetRotation().Rotator()),
            *FCRIRTextEmitter::FormatVector(Xf.GetScale3D()));
    }
    case ERigControlType::TransformNoScale:
    {
        const FTransformNoScale Xf = Value.Get<FRigControlValue::FTransformNoScale_Float>().ToTransform();
        return FString::Printf(TEXT("%s(loc=%s, rot=%s)"),
            Prefix,
            *FCRIRTextEmitter::FormatVector(Xf.Location),
            *FCRIRTextEmitter::FormatRotator(Xf.Rotation.Rotator()));
    }
    case ERigControlType::EulerTransform:
    {
        const FEulerTransform Xf = Value.Get<FRigControlValue::FEulerTransform_Float>().ToTransform();
        return FString::Printf(TEXT("%s(loc=%s, rot=%s, scale=%s)"),
            Prefix,
            *FCRIRTextEmitter::FormatVector(Xf.Location),
            *FCRIRTextEmitter::FormatRotator(Xf.Rotation),
            *FCRIRTextEmitter::FormatVector(Xf.Scale));
    }
    default:
        return FString::Printf(TEXT("%s()"), Prefix);
    }
}

FString FCRIRTextEmitter::FormatControlSettingsSubBlock(const FRigControlSettings& Settings, ERigControlType Type)
{
    const FRigControlSettings Default;

    TArray<TPair<FString, FString>> Lines;

    // Alphabetical key emission. Each branch emits its key=value pair only
    // when the field diverges from a default-constructed FRigControlSettings
    // of the same ControlType (we don't reset Default.ControlType here — the
    // ~20 settings keys are independent of which type the value carries).
    if (Settings.AnimationType != Default.AnimationType)
    {
        Lines.Add({TEXT("animation_type"), FormatEnumLowerSnake(Settings.AnimationType)});
    }
    if (Settings.bDrawLimits != Default.bDrawLimits)
    {
        Lines.Add({TEXT("draw_limits"), Settings.bDrawLimits ? TEXT("true") : TEXT("false")});
    }
    if (!Settings.DisplayName.IsNone() && Settings.DisplayName != Default.DisplayName)
    {
        Lines.Add({TEXT("display_name"), FIrTextUtils::Quote(Settings.DisplayName.ToString())});
    }
    if (Settings.DrivenControls.Num() > 0)
    {
        TArray<FString> Names;
        for (const FRigElementKey& K : Settings.DrivenControls)
        {
            Names.Add(K.Name.ToString());
        }
        Lines.Add({TEXT("driven_controls"), FString::Printf(TEXT("[%s]"), *FString::Join(Names, TEXT(",")))});
    }
    if (Settings.FilteredChannels.Num() > 0)
    {
        TArray<FString> Chans;
        for (ERigControlTransformChannel C : Settings.FilteredChannels)
        {
            Chans.Add(FormatEnumLowerSnake(C));
        }
        Lines.Add({TEXT("filtered_channels"), FString::Printf(TEXT("[%s]"), *FString::Join(Chans, TEXT(",")))});
    }
    if (Settings.bGroupWithParentControl != Default.bGroupWithParentControl)
    {
        Lines.Add({TEXT("group_with_parent_control"), Settings.bGroupWithParentControl ? TEXT("true") : TEXT("false")});
    }
    if (Settings.bIsTransientControl != Default.bIsTransientControl)
    {
        Lines.Add({TEXT("is_transient_control"), Settings.bIsTransientControl ? TEXT("true") : TEXT("false")});
    }
    if (Settings.LimitEnabled.Num() > 0)
    {
        TArray<FString> Pairs;
        for (const FRigControlLimitEnabled& L : Settings.LimitEnabled)
        {
            Pairs.Add(FString::Printf(TEXT("(min=%s,max=%s)"),
                L.bMinimum ? TEXT("true") : TEXT("false"),
                L.bMaximum ? TEXT("true") : TEXT("false")));
        }
        Lines.Add({TEXT("limits"), FString::Printf(TEXT("[%s]"), *FString::Join(Pairs, TEXT(",")))});
    }
    // max / min — only emit if the underlying storage differs from a
    // default-constructed (all-zero) FRigControlValue. Compared typed instead
    // of via FormatControlValue to skip two Printf allocations per call.
    if (!IsControlValueDefaultStorage(Type, Settings.MaximumValue))
    {
        Lines.Add({TEXT("max"), FormatControlValue(Type, Settings.MaximumValue)});
    }
    if (!IsControlValueDefaultStorage(Type, Settings.MinimumValue))
    {
        Lines.Add({TEXT("min"), FormatControlValue(Type, Settings.MinimumValue)});
    }
    if (Settings.PrimaryAxis != Default.PrimaryAxis)
    {
        Lines.Add({TEXT("primary_axis"), FormatEnumLowerSnake(Settings.PrimaryAxis)});
    }
    if (Settings.bUsePreferredRotationOrder != Default.bUsePreferredRotationOrder)
    {
        Lines.Add({TEXT("use_preferred_rotation_order"), Settings.bUsePreferredRotationOrder ? TEXT("true") : TEXT("false")});
    }
    if (Settings.PreferredRotationOrder != Default.PreferredRotationOrder)
    {
        Lines.Add({TEXT("preferred_rotation_order"), FormatEnumLowerSnake(Settings.PreferredRotationOrder)});
    }
    if (Settings.bRestrictSpaceSwitching != Default.bRestrictSpaceSwitching)
    {
        Lines.Add({TEXT("restrict_space_switching"), Settings.bRestrictSpaceSwitching ? TEXT("true") : TEXT("false")});
    }
    if (Settings.ShapeColor != Default.ShapeColor)
    {
        Lines.Add({TEXT("shape_color"), FString::Printf(TEXT("(%g,%g,%g,%g)"),
            Settings.ShapeColor.R, Settings.ShapeColor.G, Settings.ShapeColor.B, Settings.ShapeColor.A)});
    }
    if (!Settings.ShapeName.IsNone() && Settings.ShapeName != Default.ShapeName)
    {
        Lines.Add({TEXT("shape_name"), FIrTextUtils::FormatNameToken(Settings.ShapeName.ToString())});
    }
    if (Settings.bShapeVisible != Default.bShapeVisible)
    {
        Lines.Add({TEXT("shape_visible"), Settings.bShapeVisible ? TEXT("true") : TEXT("false")});
    }
    if (Settings.ShapeVisibility != Default.ShapeVisibility)
    {
        Lines.Add({TEXT("shape_visibility"), FormatEnumLowerSnake(Settings.ShapeVisibility)});
    }
    if (Settings.ControlEnum != nullptr)
    {
        Lines.Add({TEXT("control_enum"), Settings.ControlEnum->GetPathName()});
    }

    if (Lines.Num() == 0)
    {
        return FString();
    }

    // Sort alphabetically by key for byte-stable output.
    Lines.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
    {
        return A.Key.Compare(B.Key) < 0;
    });

    TArray<FString> Out;
    Out.Reserve(Lines.Num());
    for (const TPair<FString, FString>& L : Lines)
    {
        Out.Add(FString::Printf(TEXT("%s=%s"), *L.Key, *L.Value));
    }
    return FString::Join(Out, TEXT("\n"));
}

FString FCRIRTextEmitter::EmitControlElement(
    const FString& Name,
    const FString& Parent,
    ERigControlType Type,
    const FString& ValueLiteral,
    const FString& ShapeLiteralOrEmpty,
    const TArray<TPair<FString, FString>>& OffsetTransformAttrs,
    const FString& SubBlockBodyOrEmpty)
{
    FString ParentClause;
    if (!Parent.IsEmpty())
    {
        ParentClause = FString::Printf(TEXT(" parent=%s"), *FIrTextUtils::FormatNameToken(Parent));
    }

    FString TypeClause = FString::Printf(TEXT(" type=%s"), FCRIRControlValueParser::PrefixForType(Type));
    FString ValueClause = FString::Printf(TEXT(" value=%s"), *ValueLiteral);

    FString ShapeClause;
    if (!ShapeLiteralOrEmpty.IsEmpty())
    {
        ShapeClause = FString::Printf(TEXT(" shape=%s"), *ShapeLiteralOrEmpty);
    }

    FString OffsetClause;
    for (const TPair<FString, FString>& A : OffsetTransformAttrs)
    {
        OffsetClause += FString::Printf(TEXT(" %s=%s"), *A.Key, *A.Value);
    }

    FString Line = FString::Printf(TEXT("control %s%s%s%s%s%s"),
        *FIrTextUtils::FormatNameToken(Name),
        *ParentClause,
        *TypeClause,
        *ValueClause,
        *ShapeClause,
        *OffsetClause);

    if (SubBlockBodyOrEmpty.IsEmpty())
    {
        return Line;
    }

    return FString::Printf(TEXT("%s\n{\n%s\n}"),
        *Line,
        *IndentBlockBody(SubBlockBodyOrEmpty));
}

FString FCRIRTextEmitter::FormatVector(const FVector& V)
{
    return FString::Printf(TEXT("(%g,%g,%g)"), V.X, V.Y, V.Z);
}

FString FCRIRTextEmitter::FormatRotator(const FRotator& R)
{
    return FString::Printf(TEXT("(%g,%g,%g)"), R.Pitch, R.Yaw, R.Roll);
}

FString FCRIRTextEmitter::MakeLocalId(int32 Counter)
{
    return FIrTextUtils::FormatNumericLocalId(Counter);
}

FString FCRIRTextEmitter::FormatLocalRef(const FString& Node, const FString& Pin)
{
    if (Pin.IsEmpty())
    {
        return FString::Printf(TEXT("%%%s"), *Node);
    }
    return FString::Printf(TEXT("%%%s.%s"), *Node, *Pin);
}

FString FCRIRTextEmitter::IndentBlockBody(const FString& Body)
{
    if (Body.IsEmpty())
    {
        return Body;
    }
    TArray<FString> Lines;
    Body.ParseIntoArray(Lines, TEXT("\n"), false);
    for (FString& Line : Lines)
    {
        if (!Line.IsEmpty())
        {
            Line = TEXT("    ") + Line;
        }
    }
    return FString::Join(Lines, TEXT("\n"));
}
