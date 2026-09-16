// Copyright (c) 2026 Alexander Penkin. MIT License.

// CollisionSummaryUtils.cpp - the shared "what collision does this thing actually have?" read.
// Contract and the engine rules it mirrors are documented in CollisionSummaryUtils.h.

#include "Utils/CollisionSummaryUtils.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodySetup.h" // UBodySetup / FKAggregateGeom
#include "UObject/Class.h"

namespace
{
    // Prefixed names: the plugin builds with Unity on, so file-local helpers must not collide
    // with a sibling TU's anonymous namespace.
    const TCHAR* PinWrightCollisionEnabledName(ECollisionEnabled::Type Value)
    {
        switch (Value)
        {
        case ECollisionEnabled::NoCollision:      return TEXT("NoCollision");
        case ECollisionEnabled::QueryOnly:        return TEXT("QueryOnly");
        case ECollisionEnabled::PhysicsOnly:      return TEXT("PhysicsOnly");
        case ECollisionEnabled::QueryAndPhysics:  return TEXT("QueryAndPhysics");
        default:                                  return TEXT("Unknown");
        }
    }

    const TCHAR* PinWrightTraceKindName(PinWrightCollisionSummary::ETraceKind Kind)
    {
        switch (Kind)
        {
        case PinWrightCollisionSummary::ETraceKind::SimpleAsComplex: return TEXT("UseSimpleAsComplex");
        case PinWrightCollisionSummary::ETraceKind::ComplexAsSimple: return TEXT("UseComplexAsSimple");
        default:                                                     return TEXT("UseSimpleAndComplex");
        }
    }
}

namespace PinWrightCollisionSummary
{
    int32 CountSimpleShapes(UPrimitiveComponent* Component)
    {
        // UPrimitiveComponent::GetBodySetup() is non-const, so this takes a mutable pointer
        // rather than const_cast'ing one; nothing here mutates the component.
        const UBodySetup* BodySetup = Component ? Component->GetBodySetup() : nullptr;
        return BodySetup ? BodySetup->AggGeom.GetElementCount() : -1;
    }

    FComponentCollisionSummary Summarize(UPrimitiveComponent* Component)
    {
        FComponentCollisionSummary Out;
        if (!Component)
        {
            return Out;
        }
        Out.bValid = true;

        const ECollisionEnabled::Type Enabled = Component->GetCollisionEnabled();
        Out.bCollisionEnabled = Component->IsCollisionEnabled();
        Out.CollisionEnabledName = PinWrightCollisionEnabledName(Enabled);

        Out.bRespondsToPawn =
            Component->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Ignore;
        Out.bRespondsToVisibility =
            Component->GetCollisionResponseToChannel(ECC_Visibility) != ECR_Ignore;

        UBodySetup* BodySetup = Component->GetBodySetup();
        if (!BodySetup)
        {
            // No body setup: the simple/complex split does not apply. Leave SimpleShapeCount at
            // -1 so this stays distinguishable from "has a body setup holding zero primitives".
            Out.TraceName = PinWrightTraceKindName(Out.Trace);
            return Out;
        }

        Out.bHasBodySetup = true;
        Out.SimpleShapeCount = BodySetup->AggGeom.GetElementCount();
        Out.SphereCount = BodySetup->AggGeom.SphereElems.Num();
        Out.BoxCount = BodySetup->AggGeom.BoxElems.Num();
        Out.SphylCount = BodySetup->AggGeom.SphylElems.Num();
        Out.ConvexCount = BodySetup->AggGeom.ConvexElems.Num();
        Out.TaperedCapsuleCount = BodySetup->AggGeom.TaperedCapsuleElems.Num();

        // GetCollisionTraceFlag() rather than the raw CollisionTraceFlag field: it resolves
        // CTF_UseDefault against the project's default shape complexity, and every defaulted
        // mesh would otherwise be classified wrong.
        switch (BodySetup->GetCollisionTraceFlag())
        {
        case CTF_UseSimpleAsComplex: Out.Trace = ETraceKind::SimpleAsComplex; break;
        case CTF_UseComplexAsSimple: Out.Trace = ETraceKind::ComplexAsSimple; break;
        default:                     Out.Trace = ETraceKind::SimpleAndComplex; break;
        }
        Out.TraceName = PinWrightTraceKindName(Out.Trace);
        return Out;
    }

    bool RespondsToChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel)
    {
        return (Channel == EDrawChannel::Simple) ? Summary.bRespondsToPawn
                                                 : Summary.bRespondsToVisibility;
    }

    bool DrawsSimpleInChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel)
    {
        const bool bPawn = (Channel == EDrawChannel::Simple);
        const bool bVisibility = !bPawn;
        return (bPawn && Summary.Trace != ETraceKind::ComplexAsSimple)
            || (bVisibility && Summary.Trace == ETraceKind::SimpleAsComplex);
    }

    bool DrawsComplexInChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel)
    {
        const bool bPawn = (Channel == EDrawChannel::Simple);
        const bool bVisibility = !bPawn;
        return (bVisibility && Summary.Trace != ETraceKind::SimpleAsComplex)
            || (bPawn && Summary.Trace == ETraceKind::ComplexAsSimple);
    }

    bool DrawsInChannel(const FComponentCollisionSummary& Summary, EDrawChannel Channel)
    {
        if (!Summary.bValid || !Summary.bCollisionEnabled || !RespondsToChannel(Summary, Channel))
        {
            return false;
        }

        if (!Summary.bHasBodySetup)
        {
            // A non-mesh primitive (landscape heightfield collision, shape components) draws
            // through its own scene proxy and cannot be decomposed into simple/complex here.
            // Claiming it invisible would be a false alarm, which is the one error this whole
            // report exists to avoid.
            return true;
        }

        // Simple geometry only draws if there is any. A body setup holding zero primitives
        // renders nothing - the case that looks identical to a clean scene.
        if (DrawsSimpleInChannel(Summary, Channel) && Summary.SimpleShapeCount > 0)
        {
            return true;
        }

        // Complex geometry presence is not cheaply checkable (see the header caveat), so a
        // component whose trace flag selects complex is assumed to have some.
        return DrawsComplexInChannel(Summary, Channel);
    }

    FActorCollisionReport BuildActorReport(AActor* Actor, EDrawChannel Channel)
    {
        FActorCollisionReport Report;
        if (!Actor)
        {
            return Report;
        }

        Report.Name = Actor->GetActorLabel();
        Report.Class = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();

        TArray<UPrimitiveComponent*> Components;
        Actor->GetComponents<UPrimitiveComponent>(Components);

        for (UPrimitiveComponent* Component : Components)
        {
            const FComponentCollisionSummary Summary = Summarize(Component);
            if (!Summary.bValid)
            {
                continue;
            }
            ++Report.PrimitiveComponents;

            if (Summary.bHasBodySetup)
            {
                Report.bAnyBodySetup = true;
                Report.SimpleShapeTotal += FMath::Max(Summary.SimpleShapeCount, 0);
            }
            Report.bAnyCollisionEnabled |= Summary.bCollisionEnabled;
            Report.bAnyRespondsToChannel |= RespondsToChannel(Summary, Channel);
            Report.bAnyDrawnInThisChannel |= DrawsInChannel(Summary, Channel);
            Report.bComplexAsSimple |= (Summary.Trace == ETraceKind::ComplexAsSimple);
        }

        if (Report.PrimitiveComponents == 0)
        {
            // Nothing renderable/collidable at all - a light, a logic actor. Not a finding.
            return Report;
        }

        if (!Report.bAnyBodySetup)
        {
            // Landscape heightfield collision and friends: there is no simple/complex split to
            // report, so calling it "no collision" would be a false alarm.
            Report.bUnknown = true;
            return Report;
        }

        if (Report.SimpleShapeTotal == 0)
        {
            // A body setup that holds zero simple primitives. Whether that is a problem depends
            // on the trace flag: UseComplexAsSimple means per-triangle geometry answers even
            // simple queries deliberately, anything else means gameplay traces pass through it.
            if (Report.bComplexAsSimple)
            {
                Report.bComplexOnly = true;
            }
            else
            {
                Report.bNoCollision = true;
            }
        }

        return Report;
    }
}
