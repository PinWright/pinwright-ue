// Copyright (c) 2026 Alexander Penkin. MIT License.

// effect.list_debug_shapes, effect.clear_debug_shapes, effect.draw_debug_shape,
// effect.activate_niagara, effect.deactivate_niagara, effect.advance_simulation, effect.cleanup:
// the game-safe half of the editor module's EffectHandler.cpp, which keeps
// effect.set_niagara_parameter, effect.step_and_capture and effect.spawn_niagara.
// Editor: the editor world (never PIE), as before. Game: the game world.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/VFX/EffectRuntimeUtils.h"
#include "Dispatch/PinWrightWorld.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"

#include "Dom/JsonObject.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"

#if WITH_EDITOR
#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#endif

// Editor: the editor world (never PIE). Game: the game world.
static UWorld* GetDebugDrawWorld()
{
    return PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
}

// Parse rotation array [pitch, yaw, roll] from payload
static FRotator ParseRotationArray(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName)
{
    FRotator Rot = FRotator::ZeroRotator;
    const TArray<TSharedPtr<FJsonValue>>* RA = nullptr;
    if (Payload->TryGetArrayField(FieldName, RA) && RA && RA->Num() >= 3)
    {
        Rot = FRotator((float)(*RA)[0]->AsNumber(), (float)(*RA)[1]->AsNumber(), (float)(*RA)[2]->AsNumber());
    }
    return Rot;
}

// ===========================================================================
// effect.list_debug_shapes
// ===========================================================================
REGISTER_RPC_HANDLER("effect.list_debug_shapes", "effect", "List available debug shape types",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> Shapes;
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("sphere")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("box")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("circle")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("line")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("point")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("coordinate")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("cylinder")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("cone")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("capsule")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("arrow")));
    Shapes.Add(MakeShared<FJsonValueString>(TEXT("plane")));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    // Self-describing key so the result corrects the method-name reading at the
    // call site: this is the static catalog of supported shape TYPES, NOT the
    // shapes currently drawn in the world (board
    // E-effect-list-debug-shapes-types-not-drawn). The name parallels the
    // sibling world-scoped verbs draw_debug_shape / clear_debug_shapes, so the
    // plural reads as "enumerate what's drawn" — it is not.
    Resp->SetArrayField(TEXT("shapeTypes"), Shapes);
    // Keep the legacy `shapes`/`count` keys for back-compat with existing callers.
    Resp->SetArrayField(TEXT("shapes"), Shapes);
    Resp->SetNumberField(TEXT("count"), Shapes.Num());
    Resp->SetStringField(TEXT("note"),
        TEXT("These are the supported shape TYPES, not the shapes currently drawn. "
             "To draw use effect.draw_debug_shape; to tear down use effect.clear_debug_shapes. "
             "There is no verb that enumerates currently-drawn shapes (per-draw success is the confirmation)."));
    Ctx.SendSuccess(Resp);
    return true;
}

// ===========================================================================
// effect.clear_debug_shapes
// ===========================================================================
REGISTER_RPC_HANDLER("effect.clear_debug_shapes", "effect", "Clear all persistent debug shapes",
    RPC_NO_PARAMS)
{
    if (UWorld* World = GetDebugDrawWorld())
    {
        FlushPersistentDebugLines(World);
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Ctx.SendSuccess(Resp);
        return true;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, PinWright::IsGameProcess()
            ? TEXT("Game world not available") : TEXT("Editor world not available"));
        return true;
    }
}

namespace PinWrightDrawDebugShape
{
    // Controls whose meaning depends on the shape. A supplied control the selected shape does not
    // draw with is refused, never dropped (rpc-design.md §21; board B-effect-debug-options-ignored).
    static const TCHAR* const ShapeControls[] = {
        TEXT("size"), TEXT("scale"), TEXT("rotation"), TEXT("color"), TEXT("thickness"), TEXT("boxSize"),
        TEXT("endLocation"), TEXT("direction"), TEXT("length"), TEXT("angle"), TEXT("halfHeight")};

    // Shape -> the ShapeControls it draws with. nullptr = unsupported shape.
    static const TArray<FString>* AcceptedControls(const FString& LowerShape)
    {
        static const TMap<FString, TArray<FString>> Table = {
            {TEXT("sphere"),     {TEXT("size"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("box"),        {TEXT("size"), TEXT("boxSize"), TEXT("rotation"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("plane"),      {TEXT("size"), TEXT("boxSize"), TEXT("rotation"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("circle"),     {TEXT("size"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("line"),       {TEXT("endLocation"), TEXT("color"), TEXT("thickness")}},
            {TEXT("point"),      {TEXT("size"), TEXT("scale"), TEXT("color")}},
            {TEXT("coordinate"), {TEXT("size"), TEXT("rotation"), TEXT("scale"), TEXT("thickness")}},
            {TEXT("cylinder"),   {TEXT("size"), TEXT("endLocation"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("cone"),       {TEXT("direction"), TEXT("length"), TEXT("angle"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("capsule"),    {TEXT("size"), TEXT("halfHeight"), TEXT("rotation"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
            {TEXT("arrow"),      {TEXT("size"), TEXT("endLocation"), TEXT("scale"), TEXT("color"), TEXT("thickness")}},
        };
        return Table.Find(LowerShape);
    }

    // Reads an optional [x,y,z] of finite non-negative numbers (or, when bAllowScalar, one number
    // applied to every axis). Absent leaves InOut untouched; anything malformed is an error.
    static bool ReadNonNegativeVec3(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Key,
        const bool bAllowScalar, FVector& InOut, FString& OutError)
    {
        const TSharedPtr<FJsonValue> Val = Payload->TryGetField(Key);
        if (!Val.IsValid())
        {
            return true;
        }
        TArray<double> Parts;
        if (Val->Type == EJson::Number && bAllowScalar)
        {
            Parts = {Val->AsNumber(), Val->AsNumber(), Val->AsNumber()};
        }
        else if (Val->Type == EJson::Array && Val->AsArray().Num() == 3)
        {
            for (const TSharedPtr<FJsonValue>& Elem : Val->AsArray())
            {
                if (!Elem.IsValid() || Elem->Type != EJson::Number)
                {
                    break;
                }
                Parts.Add(Elem->AsNumber());
            }
        }
        for (const double Part : Parts)
        {
            if (!FMath::IsFinite(Part) || Part < 0.0)
            {
                Parts.Reset();
                break;
            }
        }
        if (Parts.Num() != 3)
        {
            OutError = FString::Printf(TEXT("%s must be %s of finite non-negative numbers."), Key,
                bAllowScalar ? TEXT("one number or an [x,y,z] array") : TEXT("an [x,y,z] array"));
            return false;
        }
        InOut = FVector(Parts[0], Parts[1], Parts[2]);
        return true;
    }

    static TArray<TSharedPtr<FJsonValue>> VecJson(const FVector& V)
    {
        return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)};
    }
}

// ===========================================================================
// effect.draw_debug_shape (was "particle" sub-action of create_effect)
// ===========================================================================
REGISTER_RPC_HANDLER("effect.draw_debug_shape", "effect", "Draw a debug shape in the editor viewport",
    RPC_PARAMS(
        RPC_PARAM_REQ("preset", "string", "Preset name (unused but required for particle compat)"),
        RPC_PARAM_OPT("shapeType", "string", "Shape type: sphere, box, circle, line, point, coordinate, cylinder, cone, capsule, arrow, plane"),
        RPC_PARAM_OPT("location", "array|object", "Location [x,y,z] or {x,y,z}"),
        RPC_PARAM_OPT("rotation", "array", "Rotation [pitch,yaw,roll] of the box, plane, coordinate and capsule shapes; refused for other shapes"),
        RPC_PARAM_OPT("scale", "array|number", "Non-negative multiplier on the shape's dimensions: [x,y,z] per axis for box and plane extents; other shapes take one uniform factor (on size, halfHeight, cone length) and refuse a non-uniform one; refused for line. Never moves location/endLocation"),
        RPC_PARAM_OPT("color", "array", "Color [r,g,b,a] (0-255); refused for coordinate, which draws fixed axis colors"),
        RPC_PARAM_OPT("duration", "number", "Seconds before an autoDestroy shape disappears (default 5.0; 0 = one frame); refused with autoDestroy false"),
        RPC_PARAM_OPT("size", "number", "Size/radius (default 100.0); refused for line and cone, and together with boxSize"),
        RPC_PARAM_OPT("thickness", "number", "Line thickness (default 2.0); refused for point"),
        RPC_PARAM_OPT("autoDestroy", "boolean", "true (default): the shape disappears after duration seconds; false: it persists until effect.clear_debug_shapes"),
        RPC_PARAM_OPT("endLocation", "array|object", "Far end [x,y,z] or {x,y,z} of the line, cylinder and arrow shapes (default: location offset 100 units)"),
        RPC_PARAM_OPT("direction", "array|object", "Cone axis [x,y,z] or {x,y,z} (default: up)"),
        RPC_PARAM_OPT("length", "number", "Cone length in units (default 100.0)"),
        RPC_PARAM_OPT("angle", "number", "Cone half-angle in DEGREES, applied to both the width and height angles (default 45)"),
        RPC_PARAM_OPT("halfHeight", "number", "Capsule half-height in units (default: size)"),
        RPC_PARAM_OPT("boxSize", "array", "Box and plane half-extent [x,y,z] of finite non-negative numbers (default: size on every axis; plane default [size,size,1])")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();

    FString Preset = Ctx.GetString(TEXT("preset"));
    if (Preset.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("preset parameter required for particle spawning"));
        return true;
    }

    FString ShapeType = TEXT("sphere");
    LocalPayload->TryGetStringField(TEXT("shapeType"), ShapeType);
    const FString LowerShapeType = ShapeType.ToLower();

    const TArray<FString>* Accepted = PinWrightDrawDebugShape::AcceptedControls(LowerShapeType);
    if (!Accepted)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_SHAPE,
            FString::Printf(TEXT("Unsupported shape type: %s. Supported: sphere, box, circle, line, point, coordinate, cylinder, cone, capsule, arrow, plane"), *ShapeType));
        return true;
    }
    for (const TCHAR* Control : PinWrightDrawDebugShape::ShapeControls)
    {
        if (LocalPayload->HasField(Control) && !Accepted->Contains(Control))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("%s does not apply to shapeType '%s'; it would be ignored. Controls this shape draws with: %s."),
                    Control, *LowerShapeType, *FString::Join(*Accepted, TEXT(", "))));
            return true;
        }
    }

    const bool bAutoDestroy = LocalPayload->HasField(TEXT("autoDestroy"))
        ? GetJsonBoolField(LocalPayload, TEXT("autoDestroy"))
        : true;
    if (!bAutoDestroy && LocalPayload->HasField(TEXT("duration")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("duration does not apply with autoDestroy false: the shape persists until effect.clear_debug_shapes. Omit duration, or set autoDestroy true."));
        return true;
    }

    const float Duration = LocalPayload->HasField(TEXT("duration"))
        ? (float)GetJsonNumberField(LocalPayload, TEXT("duration"))
        : 5.0f;
    if (!FMath::IsFinite(Duration) || Duration < 0.0f)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("duration must be a finite number of seconds >= 0 (0 = one frame)."));
        return true;
    }
    // Engine semantics (DrawDebugHelpers.cpp GetDebugLineLifeTime): persistent lines live until
    // FlushPersistentDebugLines; otherwise LifeTime seconds, or one frame for LifeTime 0.
    const bool bPersistent = !bAutoDestroy;

    if (LocalPayload->HasField(TEXT("size")) && LocalPayload->HasField(TEXT("boxSize")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Pass size or boxSize, not both: boxSize replaces size for box and plane."));
        return true;
    }

    FString ParseError;
    FVector Scale = FVector::OneVector;
    if (!PinWrightDrawDebugShape::ReadNonNegativeVec3(LocalPayload, TEXT("scale"), true, Scale, ParseError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, ParseError);
        return true;
    }
    const bool bExtentShape = LowerShapeType == TEXT("box") || LowerShapeType == TEXT("plane");
    if (!bExtentShape && !(Scale.X == Scale.Y && Scale.Y == Scale.Z))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("A non-uniform scale applies only to box and plane; shapeType '%s' takes one uniform scale factor."), *LowerShapeType));
        return true;
    }
    const float UniformScale = (float)Scale.X;

    for (const TCHAR* Key : {TEXT("size"), TEXT("thickness"), TEXT("length"), TEXT("angle"), TEXT("halfHeight")})
    {
        if (LocalPayload->HasField(Key))
        {
            const double Value = GetJsonNumberField(LocalPayload, Key);
            if (!FMath::IsFinite(Value) || Value < 0.0)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("%s must be a finite non-negative number (got %g)."), Key, Value));
                return true;
            }
        }
    }

    // Location
    FVector Loc = PinWrightEffectRuntime::ParseLocationFromPayload(LocalPayload, TEXT("location"));

    const float Size = LocalPayload->HasField(TEXT("size"))
        ? (float)GetJsonNumberField(LocalPayload, TEXT("size"))
        : 100.0f;

    const float Thickness = LocalPayload->HasField(TEXT("thickness"))
        ? (float)GetJsonNumberField(LocalPayload, TEXT("thickness"))
        : 2.0f;

    // Color
    TArray<double> ColorArr = {255, 255, 255, 255};
    const TArray<TSharedPtr<FJsonValue>>* ColorJsonArr = nullptr;
    if (LocalPayload->TryGetArrayField(TEXT("color"), ColorJsonArr) && ColorJsonArr && ColorJsonArr->Num() >= 3)
    {
        ColorArr[0] = (*ColorJsonArr)[0]->AsNumber();
        ColorArr[1] = (*ColorJsonArr)[1]->AsNumber();
        ColorArr[2] = (*ColorJsonArr)[2]->AsNumber();
        if (ColorJsonArr->Num() >= 4)
        {
            ColorArr[3] = (*ColorJsonArr)[3]->AsNumber();
        }
    }

#if WITH_EDITOR
    if (GIsEditor && !GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available for debug drawing"));
        return true;
    }
#endif

    UWorld* World = GetDebugDrawWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No world available for debug drawing"));
        return true;
    }

    const FColor DebugColor((uint8)ColorArr[0], (uint8)ColorArr[1], (uint8)ColorArr[2], (uint8)ColorArr[3]);
    const FRotator Rot = ParseRotationArray(LocalPayload, TEXT("rotation"));

    // The effective geometry handed to DrawDebug*, echoed so an applied control is visible.
    TSharedPtr<FJsonObject> Geometry = MakeShared<FJsonObject>();

    if (LowerShapeType == TEXT("sphere"))
    {
        const float Radius = Size * UniformScale;
        Geometry->SetNumberField(TEXT("radius"), Radius);
        DrawDebugSphere(World, Loc, Radius, 16, DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else if (bExtentShape)
    {
        FVector BoxSize = LowerShapeType == TEXT("plane") ? FVector(Size, Size, 1.0f) : FVector(Size);
        if (!PinWrightDrawDebugShape::ReadNonNegativeVec3(LocalPayload, TEXT("boxSize"), false, BoxSize, ParseError))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, ParseError);
            return true;
        }
        const FVector Extent = BoxSize * Scale;
        Geometry->SetArrayField(TEXT("extent"), PinWrightDrawDebugShape::VecJson(Extent));
        Geometry->SetArrayField(TEXT("rotation"), PinWrightDrawDebugShape::VecJson(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll)));
        DrawDebugBox(World, Loc, Extent, Rot.Quaternion(), DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else if (LowerShapeType == TEXT("circle"))
    {
        const float Radius = Size * UniformScale;
        Geometry->SetNumberField(TEXT("radius"), Radius);
        DrawDebugCircle(World, Loc, Radius, 32, DebugColor, bPersistent, Duration, 0, Thickness, FVector::UpVector);
    }
    else if (LowerShapeType == TEXT("line"))
    {
        const FVector EndLoc = PinWrightEffectRuntime::ParseLocationFromPayload(LocalPayload, TEXT("endLocation"), Loc + FVector(100, 0, 0));
        Geometry->SetArrayField(TEXT("endLocation"), PinWrightDrawDebugShape::VecJson(EndLoc));
        DrawDebugLine(World, Loc, EndLoc, DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else if (LowerShapeType == TEXT("point"))
    {
        const float PointSize = Size * UniformScale;
        Geometry->SetNumberField(TEXT("size"), PointSize);
        DrawDebugPoint(World, Loc, PointSize, DebugColor, bPersistent, Duration);
    }
    else if (LowerShapeType == TEXT("coordinate"))
    {
        const float AxisLength = Size * UniformScale;
        Geometry->SetNumberField(TEXT("axisLength"), AxisLength);
        Geometry->SetArrayField(TEXT("rotation"), PinWrightDrawDebugShape::VecJson(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll)));
        DrawDebugCoordinateSystem(World, Loc, Rot, AxisLength, bPersistent, Duration, 0, Thickness);
    }
    else if (LowerShapeType == TEXT("cylinder"))
    {
        const FVector EndLoc = PinWrightEffectRuntime::ParseLocationFromPayload(LocalPayload, TEXT("endLocation"), Loc + FVector(0, 0, 100));
        const float Radius = Size * UniformScale;
        Geometry->SetNumberField(TEXT("radius"), Radius);
        Geometry->SetArrayField(TEXT("endLocation"), PinWrightDrawDebugShape::VecJson(EndLoc));
        DrawDebugCylinder(World, Loc, EndLoc, Radius, 16, DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else if (LowerShapeType == TEXT("cone"))
    {
        const FVector Direction = PinWrightEffectRuntime::ParseLocationFromPayload(LocalPayload, TEXT("direction"), FVector::UpVector);
        float Length = 100.0f;
        if (LocalPayload->HasField(TEXT("length")))
        {
            Length = (float)GetJsonNumberField(LocalPayload, TEXT("length"));
        }
        Length *= UniformScale;
        const float AngleDeg = LocalPayload->HasField(TEXT("angle"))
            ? (float)GetJsonNumberField(LocalPayload, TEXT("angle"))
            : 45.0f;
        const float AngleRad = FMath::DegreesToRadians(AngleDeg);
        Geometry->SetNumberField(TEXT("length"), Length);
        Geometry->SetNumberField(TEXT("angleDegrees"), AngleDeg);
        Geometry->SetArrayField(TEXT("direction"), PinWrightDrawDebugShape::VecJson(Direction));
        DrawDebugCone(World, Loc, Direction, Length, AngleRad, AngleRad, 16, DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else if (LowerShapeType == TEXT("capsule"))
    {
        float HalfHeight = Size;
        if (LocalPayload->HasField(TEXT("halfHeight")))
        {
            HalfHeight = (float)GetJsonNumberField(LocalPayload, TEXT("halfHeight"));
        }
        HalfHeight *= UniformScale;
        const float Radius = Size * UniformScale;
        Geometry->SetNumberField(TEXT("radius"), Radius);
        Geometry->SetNumberField(TEXT("halfHeight"), HalfHeight);
        Geometry->SetArrayField(TEXT("rotation"), PinWrightDrawDebugShape::VecJson(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll)));
        DrawDebugCapsule(World, Loc, HalfHeight, Radius, Rot.Quaternion(), DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else // arrow (AcceptedControls rejected every other shape above)
    {
        const FVector EndLoc = PinWrightEffectRuntime::ParseLocationFromPayload(LocalPayload, TEXT("endLocation"), Loc + FVector(100, 0, 0));
        const float ArrowSize = (Size > 0 ? Size : 10.0f) * UniformScale;
        Geometry->SetNumberField(TEXT("arrowSize"), ArrowSize);
        Geometry->SetArrayField(TEXT("endLocation"), PinWrightDrawDebugShape::VecJson(EndLoc));
        DrawDebugDirectionalArrow(World, Loc, EndLoc, ArrowSize, DebugColor, bPersistent, Duration, 0, Thickness);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("shapeType"), ShapeType);
    Resp->SetStringField(TEXT("location"), FString::Printf(TEXT("%.2f,%.2f,%.2f"), Loc.X, Loc.Y, Loc.Z));
    Resp->SetArrayField(TEXT("scale"), PinWrightDrawDebugShape::VecJson(Scale));
    Resp->SetObjectField(TEXT("geometry"), Geometry);
    Resp->SetBoolField(TEXT("autoDestroy"), bAutoDestroy);
    Resp->SetBoolField(TEXT("persistent"), bPersistent);
    if (bAutoDestroy)
    {
        Resp->SetNumberField(TEXT("duration"), Duration);
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ===========================================================================
// effect.activate_niagara
// ===========================================================================
REGISTER_RPC_HANDLER("effect.activate_niagara", "effect",
    "Activate a Niagara system on an actor. `active` is measured, not echoed: it is "
    "UNiagaraComponent::IsActive() read back AFTER the Activate call, so a system that silently "
    "refused to start reports the state that is actually in force rather than the one that was "
    "asked for. `requestedActive` carries what the call asked for, and `activationWarning` names "
    "the remedy when the two disagree. `active` is OMITTED — never reported false — when Activate "
    "destroyed the component and left nothing to measure.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemName", "string", "Actor label of the Niagara system"),
        RPC_PARAM_OPT("reset", "boolean", "Whether to reset on activate (default true)")
    ))
{
    FString SystemName = Ctx.GetString(TEXT("systemName"));
    bool bReset = Ctx.GetBool(TEXT("reset"), true);

    PinWrightEffectRuntime::FNiagaraTarget Target;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightEffectRuntime::ResolveTarget(
            SystemName, Target, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    if (!PinWrightEffectRuntime::Activate(
            Target, bReset, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    AActor* FoundActor = Target.Actor.Get();
    UNiagaraComponent* FoundComp = Target.Component.Get();
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("requestedActive"), true);
        // Read back off the component, never echoed from the request. UNiagaraComponent::Activate
        // returns void and ActivateInternal bails without ever setting the active flag on several
        // paths (NiagaraComponent.cpp): fx.NiagaraComponentsEnabled is 0, the component has no
        // asset, the host can never render, the component is not registered, the system is not
        // allowed to run, or the asset is not ready and the component is only left awaiting
        // readiness. Every one of them used to report active:true, which made the field an echo of
        // the request rather than a statement about what happened.
        if (IsValid(FoundComp))
        {
            const bool bMeasuredActive = FoundComp->IsActive();
            Resp->SetBoolField(TEXT("active"), bMeasuredActive);
            if (!bMeasuredActive)
            {
                Resp->SetStringField(TEXT("activationWarning"), FString::Printf(
                    TEXT("Activate was called on the Niagara component of '%s', but ")
                    TEXT("UNiagaraComponent::IsActive() reads false afterwards, so NOTHING is ")
                    TEXT("running. UNiagaraComponent::ActivateInternal returns without activating ")
                    TEXT("when the component has no asset, when the component is not registered, ")
                    TEXT("when the system is not allowed to run, when the asset is not ready yet ")
                    TEXT("(an outstanding compile leaves the component merely awaiting readiness), ")
                    TEXT("or when fx.NiagaraComponentsEnabled is 0. Check the asset with ")
                    TEXT("niagara.validate, confirm the component's Asset is set with ")
                    TEXT("actor.describe, and re-read the state with object.call_function ")
                    TEXT("IsActive."),
                    *SystemName));
            }
        }
        else
        {
            // Activate destroys the component outright when the host can never render (a
            // commandlet, or an editor launched with -NullRHI). There is nothing left to read, so
            // `active` is omitted rather than reported false — a false here would be
            // indistinguishable from a measured "the component is not running".
            Resp->SetStringField(TEXT("activationWarning"),
                TEXT("The Niagara component was destroyed by the Activate call, so its activation ")
                TEXT("state was not measured and `active` is omitted rather than reported as ")
                TEXT("false. UNiagaraComponent::ActivateInternal calls DestroyComponent when ")
                TEXT("FApp::CanEverRender() is false, which is the case in a commandlet or an ")
                TEXT("editor launched with -NullRHI."));
        }
        if (FoundActor)
        {
            AddActorVerification(Resp, FoundActor);
        }
        Ctx.SendSuccess(Resp);
    }
    return true;
}

// ===========================================================================
// effect.deactivate_niagara
// ===========================================================================
REGISTER_RPC_HANDLER("effect.deactivate_niagara", "effect",
    "Deactivate a Niagara system on an actor. `active` is measured, not echoed: it is "
    "UNiagaraComponent::IsActive() read back AFTER the Deactivate call, which stays true while the "
    "system finishes — Deactivate is a request to stop spawning, not an immediate kill. "
    "`requestedActive` carries what the call asked for, and `activationWarning` names the remedy "
    "when the two disagree. `active` is OMITTED — never reported false — when Deactivate destroyed "
    "the component and left nothing to measure.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("systemName", "string", "Actor label of the Niagara system (also accepts actorName)", "actorName")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();
    FString SystemName = Ctx.GetString(TEXT("systemName"));
    if (SystemName.IsEmpty())
        LocalPayload->TryGetStringField(TEXT("actorName"), SystemName);

    // Same lookup as before (first labelled actor with a Niagara component, else SYSTEM_NOT_FOUND).
    PinWrightEffectRuntime::FNiagaraTarget Target;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightEffectRuntime::ResolveTarget(
            SystemName, Target, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    UNiagaraComponent* FoundComp = Target.Component.Get();
    FoundComp->Deactivate();
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("actorName"), SystemName);
        Resp->SetBoolField(TEXT("requestedActive"), false);
        // Read back off the component, never echoed from the request. Deactivate is a request to
        // stop spawning, not a kill: UNiagaraComponent::DeactivateInternal ends with
        // SetActiveFlag(!SystemInstanceController->IsComplete()) (NiagaraComponent.cpp), so a
        // system whose particles are still living out their lifetime stays active, and a solo-mode
        // instance is not deactivated at all (Super::Deactivate is skipped so the system can reach
        // completion). Reporting a hardcoded false here described a stop that had not happened.
        if (IsValid(FoundComp))
        {
            const bool bMeasuredActive = FoundComp->IsActive();
            Resp->SetBoolField(TEXT("active"), bMeasuredActive);
            if (bMeasuredActive)
            {
                Resp->SetStringField(TEXT("activationWarning"), FString::Printf(
                    TEXT("Deactivate was called on the Niagara component of '%s', but ")
                    TEXT("UNiagaraComponent::IsActive() still reads true afterwards: the component ")
                    TEXT("stays active until the system completes, and a solo-mode instance is not ")
                    TEXT("deactivated at all. Advance the system to completion with ")
                    TEXT("effect.advance_simulation and read `active` again, or stop it outright ")
                    TEXT("with object.call_function DeactivateImmediate on the component."),
                    *SystemName));
            }
        }
        else
        {
            // Deactivate can complete the system and, on an auto-destroy component, destroy it.
            // There is nothing left to read, so `active` is omitted rather than reported false — a
            // false here would be indistinguishable from a measured "the component has stopped".
            Resp->SetStringField(TEXT("activationWarning"),
                TEXT("The Niagara component was destroyed by the Deactivate call, so its ")
                TEXT("activation state was not measured and `active` is omitted rather than ")
                TEXT("reported as false. A component with bAutoDestroy set is destroyed when the ")
                TEXT("system completes."));
        }
        Ctx.SendSuccess(Resp);
    }
    return true;
}

// ===========================================================================
// effect.advance_simulation
// ===========================================================================
REGISTER_RPC_HANDLER("effect.advance_simulation", "effect",
    "Advance Niagara simulation by bounded steps and report the actual simulated duration",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("systemName", "string", "Actor label of the Niagara system (also accepts actorName)", "actorName"),
        RPC_PARAM_OPT("deltaTime", "number", "Finite delta time per step, at least 0.0001 seconds (default 0.1)"),
        RPC_PARAM_OPT("steps", "number", "Positive whole number of steps from 1 through 10000 (default 1)")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();
    FString SystemName = Ctx.GetString(TEXT("systemName"));
    if (SystemName.IsEmpty())
        LocalPayload->TryGetStringField(TEXT("actorName"), SystemName);

    double DeltaTime = 0.1;
    LocalPayload->TryGetNumberField(TEXT("deltaTime"), DeltaTime);
    double RequestedSteps = 1.0;
    LocalPayload->TryGetNumberField(TEXT("steps"), RequestedSteps);

    PinWrightEffectStepAndCaptureLimits::FPreparedSimulation PreparedSimulation;
    const double RequestedSeconds = DeltaTime * RequestedSteps;
    FString ErrorCode;
    FString ErrorMessage;
    FString ValidationError;
    if (!PinWrightEffectStepAndCaptureLimits::Prepare(
            RequestedSteps, DeltaTime, PreparedSimulation, ValidationError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("%s %s"), *ValidationError,
                *PinWrightEffectStepAndCaptureLimits::Describe()));
        return true;
    }

    PinWrightEffectRuntime::FNiagaraTarget Target;
    if (!PinWrightEffectRuntime::ResolveTarget(
            SystemName, Target, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }
    // AdvanceSimulation(TickCount, DeltaSeconds) already advances TickCount whole
    // ticks in one call, so pass Steps directly. An earlier version wrapped this in a
    // Steps-length loop, advancing Steps*Steps ticks (steps=3 -> 9).
    PinWrightEffectRuntime::FAdvanceResult AdvanceResult;
    if (!PinWrightEffectRuntime::Advance(
            Target, PreparedSimulation.Steps, PreparedSimulation.SimulationDeltaTime,
            AdvanceResult, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), SystemName);
    Resp->SetNumberField(TEXT("steps"), PreparedSimulation.Steps);
    Resp->SetNumberField(TEXT("deltaTime"),
        static_cast<double>(PreparedSimulation.SimulationDeltaTime));
    Resp->SetNumberField(TEXT("requestedSeconds"), RequestedSeconds);
    Resp->SetNumberField(TEXT("simulatedSeconds"), AdvanceResult.SimulatedSeconds);
    if (AdvanceResult.bStoppedEarly)
    {
        Resp->SetBoolField(TEXT("stoppedEarly"), true);
        Resp->SetStringField(TEXT("stopReason"), AdvanceResult.StopReason);
    }
    Ctx.SendSuccess(Resp);
    return true;
}


// ===========================================================================
// effect.cleanup
// ===========================================================================
REGISTER_RPC_HANDLER("effect.cleanup", "effect", "Remove actors whose label starts with a filter string",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Label prefix filter for actors to remove")
    ))
{
    FString Filter = Ctx.GetString(TEXT("filter"));
    if (Filter.IsEmpty())
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetNumberField(TEXT("removed"), 0);
        Ctx.SendSuccess(Resp);
        return true;
    }

    // Editor: UEditorActorSubsystem list and destroy (undo, selection). Game: the game world, AActor::Destroy.
    TArray<AActor*> Actors;
#if WITH_EDITOR
    UEditorActorSubsystem* ActorSS = nullptr;
    if (GIsEditor)
    {
        if (!GEditor)
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
            return true;
        }
        ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
        if (!ActorSS)
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, TEXT("EditorActorSubsystem not available"));
            return true;
        }
        Actors = ActorSS->GetAllLevelActors();
    }
    else
#endif
    {
        UWorld* World = PinWright::GetTargetWorld();
        if (!World)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No game world available"));
            return true;
        }
        Actors = PinWrightEffectRuntime::GetWorldActors(*World);
    }
    TArray<FString> Removed;
    for (AActor* A : Actors)
    {
        if (!A)
            continue;
        FString Label = PinWrightEffectRuntime::GetActorLabel(*A);
        if (Label.IsEmpty())
            continue;
        if (!Label.StartsWith(Filter, ESearchCase::IgnoreCase))
            continue;
#if WITH_EDITOR
        bool bDel = ActorSS ? ActorSS->DestroyActor(A) : A->Destroy();
#else
        bool bDel = A->Destroy();
#endif
        if (bDel)
            Removed.Add(Label);
    }
    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FString& S : Removed)
        Arr.Add(MakeShared<FJsonValueString>(S));
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("removedActors"), Arr);
    Resp->SetNumberField(TEXT("removed"), Removed.Num());
    Ctx.SendSuccess(Resp);
    return true;
}
