// Copyright (c) 2026 Alexander Penkin. MIT License.

// EffectHandler.cpp - Migrated from PinWright_EffectHandlers.cpp
// Debug shapes, Niagara spawning/parameters, dynamic lights, cleanup, Niagara modules

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/OpenLevelCapture.h"
#include "Handlers/VFX/EffectRuntimeUtils.h"
#include "Handlers/VFX/EffectSpawnTestHooks.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/ScopedWorldTimeDilation.h"

#include <cmath>

#include "Dom/JsonObject.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#if __has_include("NiagaraActor.h")
#include "NiagaraActor.h"
#endif
#if __has_include("NiagaraComponent.h")
#include "NiagaraComponent.h"
#endif
#if __has_include("NiagaraSystem.h")
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#endif
#if __has_include("NiagaraEmitter.h")
#include "NiagaraEmitter.h"
#endif
#if __has_include("NiagaraScript.h")
#include "NiagaraScript.h"
#endif
#if __has_include("NiagaraDataInterface.h")
#include "NiagaraDataInterface.h"
#endif
#if __has_include("NiagaraSimulationStageBase.h")
#include "NiagaraSimulationStageBase.h"
#endif
#if __has_include("NiagaraRendererProperties.h")
#include "NiagaraRendererProperties.h"
#endif
#if __has_include("Engine/PointLight.h")
#include "Engine/PointLight.h"
#endif
#if __has_include("Engine/SpotLight.h")
#include "Engine/SpotLight.h"
#endif
#if __has_include("Engine/DirectionalLight.h")
#include "Engine/DirectionalLight.h"
#endif
#if __has_include("Engine/RectLight.h")
#include "Engine/RectLight.h"
#endif
#if __has_include("Components/LightComponent.h")
#include "Components/LightComponent.h"
#endif
#if __has_include("Components/PointLightComponent.h")
#include "Components/PointLightComponent.h"
#endif
#if __has_include("Components/SpotLightComponent.h")
#include "Components/SpotLightComponent.h"
#endif
#if __has_include("Components/RectLightComponent.h")
#include "Components/RectLightComponent.h"
#endif
#if __has_include("Components/DirectionalLightComponent.h")
#include "Components/DirectionalLightComponent.h"
#endif
#include "Utils/RenderingAvailability.h"

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

namespace PinWrightEffectStepAndCaptureLimits
{
    constexpr double MinActualStepDeltaSeconds = 1.0e-4;
    constexpr int32 MaxSteps = 10000;
    constexpr double MaxSimulatedSeconds = 60.0;

    struct FPreparedSimulation
    {
        int32 Steps = 0;
        float SimulationDeltaTime = 0.0f;
    };

    static FString Describe()
    {
        return TEXT("Limits: actual per-step delta >= 0.0001 seconds, steps <= 10000, "
                    "total simulated duration <= 60 seconds.");
    }

    static bool Prepare(const double RequestedSteps, const double StepDeltaSeconds,
        FPreparedSimulation& OutSimulation, FString& OutError)
    {
        if (!FMath::IsFinite(RequestedSteps) || RequestedSteps < 1.0 ||
            RequestedSteps > static_cast<double>(MaxSteps) ||
            RequestedSteps != FMath::RoundToDouble(RequestedSteps))
        {
            OutError = TEXT("steps must be a finite positive whole number within the supported step count.");
            return false;
        }

        const int32 Steps = static_cast<int32>(FMath::RoundToDouble(RequestedSteps));
        if (!FMath::IsFinite(StepDeltaSeconds) ||
            StepDeltaSeconds < MinActualStepDeltaSeconds)
        {
            OutError = TEXT("deltaTime is non-finite or below the supported minimum.");
            return false;
        }

        if (StepDeltaSeconds > MaxSimulatedSeconds / static_cast<double>(Steps))
        {
            OutError = TEXT("steps multiplied by deltaTime exceeds the supported total simulated duration.");
            return false;
        }

        // AdvanceSimulation accepts float. Keep the actual value at or above the minimum;
        // otherwise round down when nearest-float conversion would cross the requested duration
        // or the duration cap.
        float SimulationDeltaTime = static_cast<float>(StepDeltaSeconds);
        if (static_cast<double>(SimulationDeltaTime) < MinActualStepDeltaSeconds)
        {
            SimulationDeltaTime = std::nextafter(SimulationDeltaTime, 1.0f);
        }
        else if (static_cast<double>(SimulationDeltaTime) > StepDeltaSeconds)
        {
            SimulationDeltaTime = std::nextafter(SimulationDeltaTime, 0.0f);
        }

        const double ActualStepDeltaSeconds = static_cast<double>(SimulationDeltaTime);
        if (!FMath::IsFinite(SimulationDeltaTime) ||
            ActualStepDeltaSeconds < MinActualStepDeltaSeconds)
        {
            OutError = TEXT("the actual float per-step delta is below the supported minimum.");
            return false;
        }

        if (ActualStepDeltaSeconds > MaxSimulatedSeconds / static_cast<double>(Steps))
        {
            OutError = TEXT("the actual step count and per-step delta exceed the supported total simulated duration.");
            return false;
        }

        OutSimulation.Steps = Steps;
        OutSimulation.SimulationDeltaTime = SimulationDeltaTime;
        return true;
    }
}

// Parse a location from the payload - supports array [x,y,z] or object {x,y,z}
static FVector ParseLocationFromPayload(const TSharedPtr<FJsonObject>& Payload, const FString& FieldName, FVector Default = FVector::ZeroVector)
{
    if (!Payload.IsValid() || !Payload->HasField(FieldName))
        return Default;

    const TSharedPtr<FJsonValue> Val = Payload->TryGetField(FieldName);
    if (!Val.IsValid())
        return Default;

    if (Val->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = Val->AsArray();
        if (Arr.Num() >= 3)
            return FVector((float)Arr[0]->AsNumber(), (float)Arr[1]->AsNumber(), (float)Arr[2]->AsNumber());
    }
    else if (Val->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> O = Val->AsObject();
        if (O.IsValid())
            return FVector(
                (float)(O->HasField(TEXT("x")) ? GetJsonNumberField(O, TEXT("x")) : Default.X),
                (float)(O->HasField(TEXT("y")) ? GetJsonNumberField(O, TEXT("y")) : Default.Y),
                (float)(O->HasField(TEXT("z")) ? GetJsonNumberField(O, TEXT("z")) : Default.Z));
    }
    return Default;
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

// Parse scale from payload - supports array [x,y,z] or single number
static FVector ParseScale(const TSharedPtr<FJsonObject>& Payload)
{
    TArray<double> ScaleArr = {1, 1, 1};
    const TArray<TSharedPtr<FJsonValue>>* ScaleJsonArr = nullptr;
    if (Payload->TryGetArrayField(TEXT("scale"), ScaleJsonArr) && ScaleJsonArr && ScaleJsonArr->Num() >= 3)
    {
        ScaleArr[0] = (*ScaleJsonArr)[0]->AsNumber();
        ScaleArr[1] = (*ScaleJsonArr)[1]->AsNumber();
        ScaleArr[2] = (*ScaleJsonArr)[2]->AsNumber();
    }
    else if (Payload->TryGetNumberField(TEXT("scale"), ScaleArr[0]))
    {
        ScaleArr[1] = ScaleArr[2] = ScaleArr[0];
    }
    return FVector(ScaleArr[0], ScaleArr[1], ScaleArr[2]);
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
    if (GEditor && GEditor->GetEditorWorldContext().World())
    {
        FlushPersistentDebugLines(GEditor->GetEditorWorldContext().World());
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Ctx.SendSuccess(Resp);
        return true;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("Editor world not available"));
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
    FVector Loc = ParseLocationFromPayload(LocalPayload, TEXT("location"));

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

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available for debug drawing"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
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
        const FVector EndLoc = ParseLocationFromPayload(LocalPayload, TEXT("endLocation"), Loc + FVector(100, 0, 0));
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
        const FVector EndLoc = ParseLocationFromPayload(LocalPayload, TEXT("endLocation"), Loc + FVector(0, 0, 100));
        const float Radius = Size * UniformScale;
        Geometry->SetNumberField(TEXT("radius"), Radius);
        Geometry->SetArrayField(TEXT("endLocation"), PinWrightDrawDebugShape::VecJson(EndLoc));
        DrawDebugCylinder(World, Loc, EndLoc, Radius, 16, DebugColor, bPersistent, Duration, 0, Thickness);
    }
    else if (LowerShapeType == TEXT("cone"))
    {
        const FVector Direction = ParseLocationFromPayload(LocalPayload, TEXT("direction"), FVector::UpVector);
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
        const FVector EndLoc = ParseLocationFromPayload(LocalPayload, TEXT("endLocation"), Loc + FVector(100, 0, 0));
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
// effect.set_niagara_parameter
// ===========================================================================
REGISTER_RPC_HANDLER("effect.set_niagara_parameter", "effect", "Set a parameter on a Niagara component",
    RPC_PARAMS(
        RPC_PARAM_OPT("systemName", "string", "Actor label of the Niagara system"),
        RPC_PARAM_REQ("parameterName", "string", "Name of the parameter to set"),
        RPC_PARAM_OPT("parameterType", "string", "Type: Float, Vector, Color, Bool (default Float)"),
        RPC_PARAM_OPT("value", "any", "Value to set")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();

    FString SystemName = Ctx.GetString(TEXT("systemName"));
    FString ParameterName = Ctx.GetString(TEXT("parameterName"));
    FString ParameterType = Ctx.GetString(TEXT("parameterType"));
    if (ParameterName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("parameterName required"));
        return true;
    }
    if (ParameterType.IsEmpty())
        ParameterType = TEXT("Float");

    UE_LOG(LogPinWrightSubsystem, Verbose,
           TEXT("SetNiagaraParameter: Looking for actor '%s' to set param '%s'"),
           *SystemName, *ParameterName);

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (!ActorSS)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, TEXT("EditorActorSubsystem not available"));
        return true;
    }

    const FName ParamName(*ParameterName);
    const TSharedPtr<FJsonValue> ValueField = LocalPayload->TryGetField(TEXT("value"));

    TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
    bool bApplied = false;
    bool bActorFound = false;
    bool bComponentFound = false;
    bool bParameterExists = false;

    for (AActor* Actor : AllActors)
    {
        if (!Actor)
            continue;
        if (!Actor->GetActorLabel().Equals(SystemName, ESearchCase::IgnoreCase))
            continue;

        bActorFound = true;
        UNiagaraComponent* NiComp = Actor->FindComponentByClass<UNiagaraComponent>();
        if (!NiComp)
        {
            bComponentFound = false;
            break;
        }
        bComponentFound = true;

        // Reject names that aren't declared on the system. SetVariable* forwards to
        // OverrideParameters.SetParameterValue(..., bAdd=true), which silently CREATES an
        // entry for any unknown name, so without this check a typo'd / wrong-prefix /
        // wrong-type name would write nothing yet still report applied:true. A single
        // classification distinguishes a bad type from an unknown name for the error ladder
        // below, so neither needs to re-run the type allow-list.
        const PinWrightNiagara::EParameterLookup ParamLookup =
            PinWrightNiagara::ClassifyComponentParameter(NiComp, ParamName, ParameterType);
        if (ParamLookup != PinWrightNiagara::EParameterLookup::Found)
        {
            break;
        }
        bParameterExists = true;

        if (ParameterType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
        {
            double NumberValue = 0.0;
            bool bHasNumber = LocalPayload->TryGetNumberField(TEXT("value"), NumberValue);
            if (!bHasNumber && ValueField.IsValid())
            {
                if (ValueField->Type == EJson::Number)
                {
                    NumberValue = ValueField->AsNumber();
                    bHasNumber = true;
                }
                else if (ValueField->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject> Obj = ValueField->AsObject();
                    if (Obj.IsValid())
                        bHasNumber = Obj->TryGetNumberField(TEXT("v"), NumberValue);
                }
            }
            if (bHasNumber)
            {
                NiComp->SetVariableFloat(ParamName, static_cast<float>(NumberValue));
                bApplied = true;
            }
        }
        else if (ParameterType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
        {
            const TArray<TSharedPtr<FJsonValue>>* ArrValue = nullptr;
            const TSharedPtr<FJsonObject>* ObjValue = nullptr;
            if (LocalPayload->TryGetArrayField(TEXT("value"), ArrValue) && ArrValue && ArrValue->Num() >= 3)
            {
                const float X = static_cast<float>((*ArrValue)[0]->AsNumber());
                const float Y = static_cast<float>((*ArrValue)[1]->AsNumber());
                const float Z = static_cast<float>((*ArrValue)[2]->AsNumber());
                NiComp->SetVariableVec3(ParamName, FVector(X, Y, Z));
                bApplied = true;
            }
            else if (LocalPayload->TryGetObjectField(TEXT("value"), ObjValue) && ObjValue)
            {
                double VX = 0, VY = 0, VZ = 0;
                (*ObjValue)->TryGetNumberField(TEXT("x"), VX);
                (*ObjValue)->TryGetNumberField(TEXT("y"), VY);
                (*ObjValue)->TryGetNumberField(TEXT("z"), VZ);
                NiComp->SetVariableVec3(ParamName, FVector((float)VX, (float)VY, (float)VZ));
                bApplied = true;
            }
        }
        else if (ParameterType.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
        {
            const TArray<TSharedPtr<FJsonValue>>* ArrValue = nullptr;
            if (LocalPayload->TryGetArrayField(TEXT("value"), ArrValue) && ArrValue && ArrValue->Num() >= 3)
            {
                const float R = static_cast<float>((*ArrValue)[0]->AsNumber());
                const float G = static_cast<float>((*ArrValue)[1]->AsNumber());
                const float B = static_cast<float>((*ArrValue)[2]->AsNumber());
                const float Alpha = ArrValue->Num() > 3 ? static_cast<float>((*ArrValue)[3]->AsNumber()) : 1.0f;
                NiComp->SetVariableLinearColor(ParamName, FLinearColor(R, G, B, Alpha));
                bApplied = true;
            }
        }
        else if (ParameterType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
        {
            bool bValue = false;
            bool bHasBool = LocalPayload->TryGetBoolField(TEXT("value"), bValue);
            if (bHasBool)
            {
                NiComp->SetVariableBool(ParamName, bValue);
                bApplied = true;
            }
        }

        break;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), bApplied);
    Resp->SetBoolField(TEXT("applied"), bApplied);
    Resp->SetStringField(TEXT("actorName"), SystemName);
    Resp->SetStringField(TEXT("parameterName"), ParameterName);
    Resp->SetStringField(TEXT("parameterType"), ParameterType);

    if (bApplied)
    {
        Ctx.SendSuccess(Resp);
    }
    else
    {
        FString ErrMsg = TEXT("Niagara parameter not applied");
        FString ErrCode = ErrorCodes::ERR_SET_NIAGARA_PARAM_FAILED;
        if (!bActorFound)
        {
            ErrMsg = FString::Printf(TEXT("Actor '%s' not found"), *SystemName);
            ErrCode = ErrorCodes::ERR_ACTOR_NOT_FOUND;
        }
        else if (!bComponentFound)
        {
            ErrMsg = FString::Printf(TEXT("Actor '%s' has no Niagara component"), *SystemName);
            ErrCode = ErrorCodes::ERR_COMPONENT_NOT_FOUND;
        }
        else if (!PinWrightNiagara::IsSupportedRuntimeParameterType(ParameterType))
        {
            ErrMsg = FString::Printf(TEXT("Invalid parameter type: %s"), *ParameterType);
            ErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        }
        else if (!bParameterExists)
        {
            ErrMsg = FString::Printf(
                TEXT("Niagara system on '%s' has no %s user parameter named '%s'"),
                *SystemName, *ParameterType, *ParameterName);
            ErrCode = ErrorCodes::ERR_PARAMETER_NOT_FOUND;
        }
        Ctx.SendError(ErrCode, ErrMsg);
    }
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

    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
    bool bFound = false;
    UNiagaraComponent* FoundComp = nullptr;
    for (AActor* Actor : AllActors)
    {
        if (!Actor)
            continue;
        if (!Actor->GetActorLabel().Equals(SystemName, ESearchCase::IgnoreCase))
            continue;

        UNiagaraComponent* NiComp = Actor->FindComponentByClass<UNiagaraComponent>();
        if (!NiComp)
            continue;

        NiComp->Deactivate();
        bFound = true;
        FoundComp = NiComp;
        break;
    }
    if (bFound)
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
    else
    {
        Ctx.SendError(ErrorCodes::ERR_SYSTEM_NOT_FOUND, TEXT("Niagara system not found."));
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
// effect.step_and_capture
// ===========================================================================
REGISTER_RPC_HANDLER("effect.step_and_capture", "effect",
    "Reset and activate a placed Niagara system, settle viewport exposure, freeze editor-world "
    "time, advance the system to one requested instant, capture that exact frame, and restore time.",
    RPC_PARAMS(
        RPC_PARAM_REQ_ALIAS("systemName", "string", "Actor label of the Niagara system (also accepts actorName)", "actorName"),
        RPC_PARAM_OPT("reset", "boolean", "Reset the Niagara system before sampling (default true)."),
        RPC_PARAM_OPT("seconds", "number", "Requested simulation age in seconds, up to 60. Mutually exclusive with frames."),
        RPC_PARAM_OPT("frames", "integer", "Whole simulation frames to advance, from 1 through 10000. Mutually exclusive with seconds; default 1."),
        RPC_PARAM_OPT("deltaTime", "number", "Seconds per simulation step. Default 1/60; the actual per-step delta must be at least 0.0001 seconds and frames * deltaTime must not exceed 60 seconds. With seconds, the uniform step is reduced when needed."),
        RPC_PARAM_OPT("filename", "filepath", "Output filename inside Saved/Screenshots/OpenLevel. The .png extension is appended if missing."),
        RPC_PARAM_OPT("width", "number", "Output width in pixels. Default 768."),
        RPC_PARAM_OPT("height", "number", "Output height in pixels. Default 768."),
        RPC_PARAM_OPT("location", "object", "Level viewport camera location {x, y, z}."),
        RPC_PARAM_OPT("rotation", "object", "Level viewport camera rotation {pitch, yaw, roll}."),
        RPC_PARAM_OPT("projectionMode", "string", "'perspective' (default) or 'orthographic'."),
        RPC_PARAM_OPT("fov", "number", "Perspective field of view in degrees. Default 50."),
        RPC_PARAM_OPT("allowBlank", "boolean", "Accept an intentionally near-uniform black frame. Default false."),
        FParamSpec{TEXT("orthoWidth"), TEXT("number"),
            TEXT("Orthographic frame width in world centimetres. Default 2000. Alias: orthoWorldWidth."),
            false, TEXT("2000"), TArray<FString>({TEXT("orthoWorldWidth")})},
        RPC_PARAM_OPT("viewDistanceScale", "number", "Scoped r.ViewDistanceScale override; omit to use the capture default."),
        RPC_PARAM_OPT("exposure", "object|number", PINWRIGHT_EXPOSURE_PARAM_DESC),
        RPC_PARAM_OPT("hideEditorSprites", "boolean", PINWRIGHT_HIDE_EDITOR_SPRITES_PARAM_DESC),
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC),
        RPC_PARAM_OPT("subject", "object", "Optional world or actor framing subject, matching render.capture_open_level.")
    ))
{
    if (!PinWrightRendering::RequireRenderer(Ctx))
    {
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString SystemName = Ctx.GetString(TEXT("systemName"));
    if (SystemName.IsEmpty())
    {
        Payload->TryGetStringField(TEXT("actorName"), SystemName);
    }

    const bool bHasSeconds = Payload->HasField(TEXT("seconds"));
    const bool bHasFrames = Payload->HasField(TEXT("frames"));
    if (bHasSeconds && bHasFrames)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("seconds and frames are mutually exclusive; provide at most one time form"));
        return true;
    }

    const auto SendInvalidTimeParams = [&Ctx](const TCHAR* Reason)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("%s %s"), Reason,
                *PinWrightEffectStepAndCaptureLimits::Describe()));
    };

    double RequestedSeconds = 0.0;
    double RequestedDeltaTime = 1.0 / 60.0;
    Payload->TryGetNumberField(TEXT("deltaTime"), RequestedDeltaTime);
    if (!FMath::IsFinite(RequestedDeltaTime) ||
        RequestedDeltaTime < PinWrightEffectStepAndCaptureLimits::MinActualStepDeltaSeconds)
    {
        SendInvalidTimeParams(TEXT("deltaTime is non-finite or below the supported minimum."));
        return true;
    }

    double RequestedSteps = 1.0;
    double StepDeltaSeconds = RequestedDeltaTime;
    FString TimeMode = TEXT("frames");
    if (bHasSeconds)
    {
        Payload->TryGetNumberField(TEXT("seconds"), RequestedSeconds);
        if (!(RequestedSeconds > 0.0) || !FMath::IsFinite(RequestedSeconds))
        {
            SendInvalidTimeParams(TEXT("seconds must be a finite number greater than zero."));
            return true;
        }
        if (RequestedSeconds > PinWrightEffectStepAndCaptureLimits::MaxSimulatedSeconds)
        {
            SendInvalidTimeParams(TEXT("seconds exceeds the supported total simulated duration."));
            return true;
        }

        const double StepRatio = RequestedSeconds / RequestedDeltaTime;
        const double RoundedStepRatio = FMath::RoundToDouble(StepRatio);
        const double RequiredSteps = FMath::IsNearlyEqual(StepRatio, RoundedStepRatio, 1e-9)
            ? RoundedStepRatio
            : FMath::CeilToDouble(StepRatio);
        RequestedSteps = FMath::Max(1.0, RequiredSteps);
        StepDeltaSeconds = RequestedSeconds / RequestedSteps;
        TimeMode = TEXT("seconds");
    }
    else
    {
        double RequestedFrames = 1.0;
        Payload->TryGetNumberField(TEXT("frames"), RequestedFrames);
        RequestedSteps = RequestedFrames;
        RequestedSeconds = StepDeltaSeconds * RequestedSteps;
    }

    PinWrightEffectStepAndCaptureLimits::FPreparedSimulation PreparedSimulation;
    FString ValidationError;
    if (!PinWrightEffectStepAndCaptureLimits::Prepare(
            RequestedSteps, StepDeltaSeconds, PreparedSimulation, ValidationError))
    {
        SendInvalidTimeParams(*ValidationError);
        return true;
    }
    const int32 Steps = PreparedSimulation.Steps;
    const float SimulationDeltaTime = PreparedSimulation.SimulationDeltaTime;

    PinWrightEffectRuntime::FNiagaraTarget Target;
    FString ErrorCode;
    FString ErrorMessage;
    if (!PinWrightEffectRuntime::ResolveTarget(
            SystemName, Target, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    UWorld* World = Target.Actor.IsValid() ? Target.Actor->GetWorld() : nullptr;
    FScopedWorldTimeDilation TimeGuard(World);
    if (!TimeGuard.IsAvailable())
    {
        Ctx.SendError(ErrorCodes::ERR_NO_WORLD_SETTINGS,
            TEXT("The Niagara actor's world has no WorldSettings"));
        return true;
    }

    const bool bReset = Ctx.GetBool(TEXT("reset"), true);
    PinWrightEffectRuntime::FAdvanceResult AdvanceResult;
    PinWrightRenderCapture::FViewportCaptureHooks Hooks;
    Hooks.BeforeWarmup = [&Target, bReset](FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (!PinWrightEffectRuntime::Activate(
                Target, bReset, OutErrorCode, OutErrorMessage))
        {
            return false;
        }
        UNiagaraComponent* Component = Target.Component.Get();
        if (!Component || !Component->IsActive())
        {
            OutErrorCode = ErrorCodes::ERR_ACTION_FAILED;
            OutErrorMessage = TEXT("Niagara system did not become active after reset/activation");
            return false;
        }
        return true;
    };
    Hooks.BeforeFinalFrame =
        [&Target, &TimeGuard, &AdvanceResult, Steps, SimulationDeltaTime](
            const PinWrightRenderCapture::FViewportCaptureOutput& SettledCapture,
            FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (!SettledCapture.bWarmupSettled)
        {
            OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
            OutErrorMessage = TEXT("Viewport exposure did not settle before the world-time freeze");
            return false;
        }
        if (!TimeGuard.Freeze())
        {
            OutErrorCode = ErrorCodes::ERR_NO_WORLD_SETTINGS;
            OutErrorMessage = TEXT("Failed to freeze the Niagara actor's world time");
            return false;
        }
        return PinWrightEffectRuntime::Advance(
            Target, Steps, SimulationDeltaTime, AdvanceResult,
            OutErrorCode, OutErrorMessage);
    };
    Hooks.AfterFinalFrame = [&TimeGuard]()
    {
        TimeGuard.Restore();
    };

    const PinWrightOpenLevelCapture::FSuccessDecorator DecorateSuccess =
        [&Target, &TimeGuard, SystemName, bReset, TimeMode, Steps, SimulationDeltaTime,
            RequestedSeconds, &AdvanceResult](
            const PinWrightRenderCapture::FViewportCaptureOutput& Capture,
            const TSharedPtr<FJsonObject>& Result)
    {
        TSharedPtr<FJsonObject> Effect = MakeShared<FJsonObject>();
        Effect->SetStringField(TEXT("actorName"), SystemName);
        Effect->SetBoolField(TEXT("reset"), bReset);
        Effect->SetStringField(TEXT("timeMode"), TimeMode);
        Effect->SetNumberField(TEXT("steps"), Steps);
        Effect->SetNumberField(TEXT("deltaTime"), static_cast<double>(SimulationDeltaTime));
        Effect->SetNumberField(TEXT("sampledSeconds"), RequestedSeconds);
        Effect->SetNumberField(TEXT("requestedSeconds"), RequestedSeconds);
        Effect->SetNumberField(TEXT("simulatedSeconds"), AdvanceResult.SimulatedSeconds);
        if (AdvanceResult.bStoppedEarly)
        {
            Effect->SetBoolField(TEXT("stoppedEarly"), true);
            Effect->SetStringField(TEXT("stopReason"), AdvanceResult.StopReason);
        }
        Effect->SetBoolField(TEXT("exposureSettledBeforeFreeze"), Capture.bWarmupSettled);
        if (UNiagaraComponent* Component = Target.Component.Get())
        {
            Effect->SetBoolField(TEXT("active"), Component->IsActive());
        }

        TSharedPtr<FJsonObject> WorldTime = MakeShared<FJsonObject>();
        WorldTime->SetNumberField(TEXT("before"), TimeGuard.GetOriginalTimeDilation());
        WorldTime->SetNumberField(TEXT("frozen"), TimeGuard.GetFrozenTimeDilation());
        WorldTime->SetBoolField(TEXT("restored"), TimeGuard.WasRestored());
        Effect->SetObjectField(TEXT("worldTimeDilation"), WorldTime);
        Result->SetObjectField(TEXT("effect"), Effect);
    };

    return PinWrightOpenLevelCapture::Handle(Ctx, Hooks, DecorateSuccess);
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

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (!ActorSS)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, TEXT("EditorActorSubsystem not available"));
        return true;
    }
    TArray<AActor*> Actors = ActorSS->GetAllLevelActors();
    TArray<FString> Removed;
    for (AActor* A : Actors)
    {
        if (!A)
            continue;
        FString Label = A->GetActorLabel();
        if (Label.IsEmpty())
            continue;
        if (!Label.StartsWith(Filter, ESearchCase::IgnoreCase))
            continue;
        bool bDel = ActorSS->DestroyActor(A);
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

// ===========================================================================
// effect.spawn_niagara
// ===========================================================================
REGISTER_RPC_MUTATING_HANDLER("effect.spawn_niagara", "effect",
    "Spawn and activate a compiled Niagara system, then report the measured component state",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "string", "Asset path to the Niagara system"),
        RPC_PARAM_OPT("location", "array|object", "Location [x,y,z] or {x,y,z}"),
        RPC_PARAM_OPT("rotation", "array", "Rotation [pitch,yaw,roll]"),
        RPC_PARAM_OPT("scale", "array|number", "Scale [x,y,z] or uniform"),
        RPC_PARAM_OPT("autoDestroy", "boolean", "Auto destroy after effect (default false)"),
        RPC_PARAM_OPT("attachToActor", "string", "Actor label to attach to"),
        RPC_PARAM_OPT_ALIAS("name", "string", "Actor label (also accepts actorName)", "actorName")
    ))
{
    const TSharedPtr<FJsonObject>& LocalPayload = Ctx.GetRawPayload();

    FString SystemPath = Ctx.GetString(TEXT("systemPath"));
    if (SystemPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("systemPath required"));
        return true;
    }

    FVector Loc = ParseLocationFromPayload(LocalPayload, TEXT("location"));

    // Rotation
    TArray<double> RotArr = {0, 0, 0};
    const TArray<TSharedPtr<FJsonValue>>* RA = nullptr;
    if (LocalPayload->TryGetArrayField(TEXT("rotation"), RA) && RA && RA->Num() >= 3)
    {
        RotArr[0] = (*RA)[0]->AsNumber();
        RotArr[1] = (*RA)[1]->AsNumber();
        RotArr[2] = (*RA)[2]->AsNumber();
    }

    // Scale
    FVector Scale = ParseScale(LocalPayload);

    const bool bAutoDestroy = LocalPayload->HasField(TEXT("autoDestroy"))
        ? GetJsonBoolField(LocalPayload, TEXT("autoDestroy"))
        : false;
    FString AttachToActor;
    LocalPayload->TryGetStringField(TEXT("attachToActor"), AttachToActor);

    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    FString ResolvedWorldMode;
    UWorld* TargetWorld = McpActorUtils::ResolveQueryWorld(TEXT("auto"), ResolvedWorldMode);
    if (!TargetWorld)
    {
        Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND, TEXT("No valid editor or PIE world available for spawn"));
        return true;
    }

    const FResolvedAsset ResolvedSystem = ResolveAsset(SystemPath, /*bLoadObject=*/true);
    if (!ResolvedSystem.Object)
    {
        Ctx.SendError(ErrorCodes::ERR_SYSTEM_NOT_FOUND,
            FString::Printf(TEXT("Niagara system asset not found: %s"), *SystemPath));
        return true;
    }
    UNiagaraSystem* System = Cast<UNiagaraSystem>(ResolvedSystem.Object);
    if (!System)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE,
            FString::Printf(TEXT("'%s' is a %s, but effect.spawn_niagara requires a NiagaraSystem"),
                *SystemPath, *ResolvedSystem.Object->GetClass()->GetName()));
        return true;
    }

    AActor* Parent = nullptr;
    if (!AttachToActor.IsEmpty())
    {
        Parent = McpActorUtils::FindActorByName(TargetWorld, AttachToActor);
        if (!Parent)
        {
            AActor* OtherWorldParent = McpActorUtils::FindActorByName(nullptr, AttachToActor);
            UWorld* OtherWorld = OtherWorldParent ? OtherWorldParent->GetWorld() : nullptr;
            if (OtherWorld && OtherWorld != TargetWorld)
            {
                TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
                State->SetStringField(TEXT("requestedAttachmentParent"), AttachToActor);
                State->SetStringField(TEXT("targetWorld"), TargetWorld->GetPathName());
                State->SetStringField(TEXT("parentWorld"), OtherWorld->GetPathName());
                Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED,
                    TEXT("Requested attachment parent belongs to a different world"), State);
                return true;
            }
            Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                FString::Printf(TEXT("Requested attachment parent not found: %s"), *AttachToActor));
            return true;
        }
        if (Parent->GetWorld() != TargetWorld)
        {
            Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED,
                TEXT("Requested attachment parent belongs to a different world"));
            return true;
        }
    }

    // Spawning is the demand that on-demand compilation defers to, so this wait may drain a
    // request that is only queued - UNiagaraComponent::Activate flushes the same one itself. A
    // system whose compile was deferred at load reports the request outstanding forever while
    // nothing is actively compiling, and refusing on it made the asset permanently unspawnable.
    const PinWrightNiagara::FCompileWaitOutcome CompileWait =
        PinWrightNiagara::WaitForSystemCompile(*System, /*bMayFlushRequestCompile=*/true);
    const TSharedPtr<FJsonObject> Compile = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
    const PinWrightNiagara::FCompileVerdict CompileVerdict =
        PinWrightNiagara::ReadCompileVerdict(Compile);
    // Measured with the predicate the refusal below is decided on. The diagnostics block reports
    // the GPU-inclusive queue, which is a wider question than whether this system can be run.
    const bool bCompilePending = PinWrightNiagara::HasPendingCompileWork(*System);
    const bool bCompileReady = PinWrightNiagara::IsSystemReadyAfterCompileWait(
        *System,
        CompileWait,
        CompileVerdict.Check == PinWrightNiagara::EScriptCompileCheck::Failed);
    FString CompileStatus = PinWrightNiagara::ScriptCompileCheckToString(CompileVerdict.Check);
    if (bCompileReady)
    {
        // On-demand compilation may leave duplicated script status fields uninitialized even
        // though Niagara's executable state is ready. Engine readiness is authoritative there.
        CompileStatus = TEXT("passed");
    }
    else if (CompileWait.bTimedOut)
    {
        CompileStatus = TEXT("timedOut");
    }
    else if (bCompilePending)
    {
        CompileStatus = TEXT("outstanding");
    }

    TSharedPtr<FJsonObject> CompileState = MakeShared<FJsonObject>();
    CompileState->SetStringField(TEXT("systemPath"), System->GetPathName());
    CompileState->SetStringField(TEXT("compileStatus"), CompileStatus);
    CompileState->SetBoolField(TEXT("compileWaited"), CompileWait.bWaited);
    CompileState->SetNumberField(TEXT("compileWaitedMs"), CompileWait.WaitedSeconds * 1000.0);
    CompileState->SetBoolField(TEXT("compileTimedOut"), CompileWait.bTimedOut);
    CompileState->SetBoolField(TEXT("outstandingCompilationRequests"), bCompilePending);
    CompileState->SetObjectField(TEXT("compile"), Compile);
    if (!bCompileReady)
    {
        FString Message = FString::Printf(
            TEXT("Niagara system '%s' is not compiled and ready to spawn (compileStatus=%s)"),
            *SystemPath, *CompileStatus);
        if (CompileVerdict.FailedScripts.Num() > 0)
        {
            Message += TEXT(": ") +
                PinWrightNiagara::DescribeScriptCompileFailure(CompileVerdict.FailedScripts[0]);
        }
        Ctx.SendError(ErrorCodes::ERR_SYSTEM_NOT_COMPILED, Message, CompileState);
        return true;
    }

    const FRotator SpawnRot(static_cast<float>(RotArr[0]), static_cast<float>(RotArr[1]), static_cast<float>(RotArr[2]));
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    AActor* Spawned = TargetWorld->SpawnActor<AActor>(
        ANiagaraActor::StaticClass(), Loc, SpawnRot, SpawnParams);
    if (!Spawned)
    {
        Ctx.SendError(ErrorCodes::ERR_SPAWN_FAILED, TEXT("Failed to spawn NiagaraActor"));
        return true;
    }

    UNiagaraComponent* NiComp = Spawned->FindComponentByClass<UNiagaraComponent>();
    if (!NiComp)
    {
        const bool bDestroyedAfterFailure = TargetWorld->DestroyActor(Spawned);
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("systemPath"), System->GetPathName());
        State->SetBoolField(TEXT("componentFound"), false);
        State->SetBoolField(TEXT("active"), false);
        State->SetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure);
        Ctx.SendError(ErrorCodes::ERR_EFFECT_NOT_ACTIVE,
            TEXT("Spawned NiagaraActor has no Niagara component to activate"), State);
        return true;
    }

    // ANiagaraActor registers its component while spawning. SetAsset on that registered component
    // auto-activates it, so prepare it while unregistered and explicitly activate exactly once only
    // after every requested property and attachment has been applied.
    NiComp->UnregisterComponent();
    NiComp->SetAutoActivate(false);
    NiComp->SetAsset(System);
    NiComp->SetWorldScale3D(Scale);
    NiComp->SetAutoDestroy(bAutoDestroy);

    if (Parent && !Spawned->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))
    {
        const bool bDestroyedAfterFailure = TargetWorld->DestroyActor(Spawned);
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("requestedAttachmentParent"), AttachToActor);
        State->SetBoolField(TEXT("attached"), false);
        State->SetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure);
        Ctx.SendError(ErrorCodes::ERR_ATTACH_FAILED,
            FString::Printf(TEXT("Failed to attach Niagara actor to '%s'"), *AttachToActor), State);
        return true;
    }

    // Set actor label
    FString Name;
    LocalPayload->TryGetStringField(TEXT("name"), Name);
    if (Name.IsEmpty())
        LocalPayload->TryGetStringField(TEXT("actorName"), Name);

    if (!Name.IsEmpty())
    {
        Spawned->SetActorLabel(Name);
    }
    else
    {
        Spawned->SetActorLabel(FString::Printf(TEXT("Niagara_%lld"), FDateTime::Now().ToUnixTimestamp()));
    }

    NiComp->RegisterComponent();
    const FString ComponentPath = NiComp->GetPathName();
    NiComp->Activate(true);
#if WITH_DEV_AUTOMATION_TESTS
    if (PinWrightEffectSpawnTestHooks::ForceInactiveAfterActivation())
    {
        NiComp->DeactivateImmediate();
    }
#endif

    const bool bActorAlive = IsValid(Spawned);
    const bool bComponentAlive = IsValid(NiComp);
    const bool bAssignedSystem = bComponentAlive && NiComp->GetAsset() == System;
    const bool bActive = bComponentAlive && NiComp->IsActive();
    const FBoolProperty* AutoDestroyProperty =
        FindFProperty<FBoolProperty>(UNiagaraComponent::StaticClass(), TEXT("bAutoDestroy"));
    const bool bAutoDestroyMeasured = bComponentAlive && AutoDestroyProperty != nullptr;
    const bool bMeasuredAutoDestroy = bAutoDestroyMeasured &&
        AutoDestroyProperty->GetPropertyValue_InContainer(NiComp);
    const bool bAttached = bActorAlive && Parent && Spawned->GetAttachParentActor() == Parent;
    if (!bActorAlive || !bComponentAlive || !bAssignedSystem || !bActive || !bAutoDestroyMeasured ||
        bMeasuredAutoDestroy != bAutoDestroy ||
        (Parent && !bAttached))
    {
        TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("systemPath"), System->GetPathName());
        State->SetStringField(TEXT("componentPath"), ComponentPath);
        State->SetBoolField(TEXT("componentAlive"), bComponentAlive);
        State->SetBoolField(TEXT("systemAssigned"), bAssignedSystem);
        State->SetBoolField(TEXT("active"), bActive);
        State->SetBoolField(TEXT("autoDestroy"), bMeasuredAutoDestroy);
        State->SetBoolField(TEXT("autoDestroyMeasured"), bAutoDestroyMeasured);
        State->SetBoolField(TEXT("attached"), bAttached);
        State->SetStringField(TEXT("compileStatus"), CompileStatus);
        const bool bDestroyedAfterFailure = !bActorAlive || TargetWorld->DestroyActor(Spawned);
        State->SetBoolField(TEXT("destroyedAfterFailure"), bDestroyedAfterFailure);
        Ctx.SendError(ErrorCodes::ERR_EFFECT_NOT_ACTIVE,
            TEXT("Spawned Niagara component did not reach the requested active state"), State);
        return true;
    }

    UE_LOG(LogPinWrightSubsystem, Display,
           TEXT("spawn_niagara: Spawned actor '%s' (ID: %u)"),
           *Spawned->GetActorLabel(), Spawned->GetUniqueID());

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    AddActorVerification(Resp, Spawned);
    Resp->SetStringField(TEXT("systemPath"), System->GetPathName());
    Resp->SetStringField(TEXT("componentPath"), ComponentPath);
    Resp->SetBoolField(TEXT("systemAssigned"), bAssignedSystem);
    Resp->SetBoolField(TEXT("active"), bActive);
    Resp->SetBoolField(TEXT("autoDestroy"), bMeasuredAutoDestroy);
    Resp->SetBoolField(TEXT("autoDestroyMeasured"), bAutoDestroyMeasured);
    Resp->SetBoolField(TEXT("attached"), bAttached);
    if (Parent)
    {
        Resp->SetStringField(TEXT("attachmentParent"), Parent->GetPathName());
    }
    Resp->SetStringField(TEXT("compileStatus"), CompileStatus);
    Resp->SetBoolField(TEXT("compileWaited"), CompileWait.bWaited);
    Resp->SetNumberField(TEXT("compileWaitedMs"), CompileWait.WaitedSeconds * 1000.0);
    Resp->SetBoolField(TEXT("compileTimedOut"), CompileWait.bTimedOut);
    Resp->SetBoolField(TEXT("outstandingCompilationRequests"), bCompilePending);
    Ctx.SendSuccess(Resp);
    return true;
}
