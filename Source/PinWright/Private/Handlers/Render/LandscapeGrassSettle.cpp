// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/LandscapeGrassSettle.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Compat/EngineVersionCompat.h"
// GUndo -- ALandscapeProxy::UpdateGrass refuses to run while a transaction is open, so the
// grass block has to be able to say that is why nothing was built.
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
// FCachedLandscapeFoliage / AsyncFoliageTasks -- the proxy's own transient record of which grass
// components exist and which of them are still being built. Read directly rather than inferred
// from FoliageComponents, because the Pending flag is the only thing that separates "built" from
// "queued" and it lives on the cache entry.
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"

namespace PinWrightCaptureGrass
{
namespace
{
    // Named for this file, not for the folder. Unity merges these translation units, so a helper
    // called anything as generic as `CountGrass` here would collide with a same-named one in a
    // sibling Render/*.cpp (docs/lessons.md).
    void PinWrightCaptureGrassMeasureProxies(const TArray<ALandscapeProxy*>& Proxies,
        int32& OutComponents, int64& OutInstances, int32& OutUnreadableComponents,
        int32& OutPendingComponents, int32& OutPendingTasks)
    {
        OutComponents = 0;
        OutInstances = 0;
        OutUnreadableComponents = 0;
        OutPendingComponents = 0;
        OutPendingTasks = 0;
        for (const ALandscapeProxy* Proxy : Proxies)
        {
            if (!IsValid(Proxy))
            {
                continue;
            }
            OutPendingTasks += Proxy->AsyncFoliageTasks.Num();
            for (const FCachedLandscapeFoliage::FGrassComp& GrassComp : Proxy->FoliageCache.CachedGrassComps)
            {
                ++OutComponents;
                if (GrassComp.Pending)
                {
                    ++OutPendingComponents;
                }
                if (const UHierarchicalInstancedStaticMeshComponent* Foliage = GrassComp.Foliage.Get())
                {
                    OutInstances += static_cast<int64>(GrassComponentInstanceCount(*Foliage));
                }
                else
                {
                    // The entry outlived its component. Adding a zero here would fold "this one
                    // cannot be read" into the same number as "this one holds no grass", which is
                    // the collapse this whole block exists to prevent.
                    ++OutUnreadableComponents;
                }
            }
        }
    }
}

int32 GrassComponentInstanceCount(const UHierarchicalInstancedStaticMeshComponent& Foliage)
{
    // Attribution for all branches is on the declaration in LandscapeGrassSettle.h.
    const int32 RenderInstances = Foliage.GetNumRenderInstances();
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // On 5.3 UGrassInstancedStaticMeshComponent does not override GetNumRenderInstances -- the
    // override arrives in 5.4 -- so the virtual resolves to the base spelling over
    // PerInstanceSMData, the array AcceptPrebuiltTree asserts is empty for grass. The built count
    // is still written to the public NumBuiltRenderInstances member on 5.3
    // (HierarchicalInstancedStaticMesh.cpp:2889), and that member is exactly what the 5.4+
    // override returns, so reading it here makes 5.3 report the same number every later engine
    // reports rather than the structural zero the ticket is about.
    if (RenderInstances <= 0 && Foliage.NumBuiltRenderInstances > 0)
    {
        return Foliage.NumBuiltRenderInstances;
    }
#endif
    return RenderInstances > 0 ? RenderInstances : Foliage.GetInstanceCount();
}

FGrassBuildReport SettleGrassForCapturePose(UWorld* World, const FVector& CameraLocation)
{
    FGrassBuildReport Report;
    Report.CameraLocation = CameraLocation;
    if (!World)
    {
        return Report;
    }
    ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>();
    if (!Subsystem)
    {
        return Report;
    }
    Report.bMeasured = true;

    TArray<ALandscapeProxy*> Proxies;
    for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
    {
        if (ALandscapeProxy* Proxy = *It)
        {
            Proxies.Add(Proxy);
        }
    }
    Report.LandscapeProxies = Proxies.Num();
    if (Proxies.Num() == 0)
    {
        // Nothing to build and nothing outstanding. Settled is a fact here, not a shortcut: an
        // asset-preview scene and a level with no terrain both land on this branch, and neither
        // can have grass missing from its frame.
        Report.bSettled = true;
        return Report;
    }

    int64 InstancesBefore = 0;
    int32 UnreadableComponentsBefore = 0;
    int32 PendingComponentsBefore = 0;
    int32 PendingTasksBefore = 0;
    PinWrightCaptureGrassMeasureProxies(Proxies, Report.ComponentsBefore, InstancesBefore,
        UnreadableComponentsBefore, PendingComponentsBefore, PendingTasksBefore);

    if (GUndo != nullptr)
    {
        // UpdateGrass refuses to run inside a transaction (LandscapeGrass.cpp:2854-2857), so
        // calling RegenerateGrass here would report a build that never happened. Publish what is
        // already there and say the pose did not drive it.
        Report.bBlockedByTransaction = true;
        Report.ComponentsAfter = Report.ComponentsBefore;
        Report.InstancesAfter = InstancesBefore;
        Report.UnreadableComponents = UnreadableComponentsBefore;
        Report.bInstancesMeasured = (UnreadableComponentsBefore == 0);
        Report.PendingComponents = PendingComponentsBefore;
        Report.PendingTasks = PendingTasksBefore;
        return Report;
    }

    const double BuildStartSeconds = FPlatformTime::Seconds();
    // One camera: this capture's own eye. bInFlushGrass false so grass already built elsewhere is
    // reused rather than thrown away and rebuilt on the next editor tick; bInForceSync true so the
    // instances exist when this function returns instead of a few frames later.
    TArray<FVector> CaptureCameras;
    CaptureCameras.Add(CameraLocation);
    Subsystem->RegenerateGrass(/*bInFlushGrass=*/false, /*bInForceSync=*/true,
        TArrayView<FVector>(CaptureCameras));
    Report.BuildMs = (FPlatformTime::Seconds() - BuildStartSeconds) * 1000.0;
    Report.bBuiltForPose = true;

    PinWrightCaptureGrassMeasureProxies(Proxies, Report.ComponentsAfter, Report.InstancesAfter,
        Report.UnreadableComponents, Report.PendingComponents, Report.PendingTasks);
    Report.bInstancesMeasured = (Report.UnreadableComponents == 0);
    Report.bSettled = (Report.PendingComponents == 0 && Report.PendingTasks == 0);
    return Report;
}

void MeasureGrassFrameReach(UWorld* World, const FVector& CullingOrigin,
    bool bInCullingOriginMeasured, float ViewDistanceScale, FGrassBuildReport& InOutReport)
{
    if (!World || !InOutReport.bMeasured || InOutReport.ComponentsAfter <= 0)
    {
        // Nothing built means nothing to be out of reach, and the build report already says so.
        // Leaving bReachMeasured false here is the point: a zero would read as "none in range".
        return;
    }

    // The renderer multiplies the component's own end-cull distance by the cached
    // r.ViewDistanceScale (HierarchicalInstancedStaticMesh.cpp:1672). A non-positive scale is not
    // a legal renderer state to model, so fall back to the identity rather than inventing a cull.
    const double Scale = ViewDistanceScale > 0.0f ? static_cast<double>(ViewDistanceScale) : 1.0;

    double NearestApproachSeen = TNumericLimits<double>::Max();
    double SmallestCull = TNumericLimits<double>::Max();
    bool bAnyComponentSeen = false;
    bool bAnyCullDistance = false;
    int32 ComponentsInReach = 0;
    int64 InstancesInReach = 0;

    for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
    {
        const ALandscapeProxy* Proxy = *It;
        if (!IsValid(Proxy))
        {
            continue;
        }
        for (const FCachedLandscapeFoliage::FGrassComp& GrassComp : Proxy->FoliageCache.CachedGrassComps)
        {
            const UHierarchicalInstancedStaticMeshComponent* Foliage = GrassComp.Foliage.Get();
            // GrassComponentInstanceCount, not GetInstanceCount: with the old accessor EVERY grass
            // component read zero here, so this loop skipped all of them, `bAnyComponentSeen`
            // stayed false and the whole reach sub-block was silently omitted from every capture
            // of real landscape grass. The reach measurement was dead on exactly the content it
            // was written for (B-ortho-capture-renders-no-landscape-grass).
            const int32 ComponentInstances = Foliage ? GrassComponentInstanceCount(*Foliage) : 0;
            if (ComponentInstances <= 0)
            {
                continue;
            }
            bAnyComponentSeen = true;

            // Nearest point of the component's bounds, not its centre: a grass component covers a
            // landscape subsection, and culling the whole of it because its centre is far would
            // report the frame's edge grass as absent.
            const FBoxSphereBounds& ComponentBounds = Foliage->Bounds;
            const double NearestApproach = FMath::Max(0.0,
                FVector::Dist(ComponentBounds.Origin, CullingOrigin)
                    - static_cast<double>(ComponentBounds.SphereRadius));
            NearestApproachSeen = FMath::Min(NearestApproachSeen, NearestApproach);

            int32 StartCullDistance = 0;
            int32 EndCullDistance = 0;
            Foliage->GetCullDistances(StartCullDistance, EndCullDistance);
            const double EffectiveCull = EndCullDistance > 0
                ? static_cast<double>(EndCullDistance) * Scale : 0.0;
            if (EffectiveCull > 0.0)
            {
                bAnyCullDistance = true;
                SmallestCull = FMath::Min(SmallestCull, EffectiveCull);
            }
            // End-cull 0 is UpdateGrass' "built with no cameras, so do not cull" state
            // (LandscapeGrass.cpp:3211-3216); such a component is in reach at any distance.
            if (EffectiveCull <= 0.0 || NearestApproach <= EffectiveCull)
            {
                ++ComponentsInReach;
                InstancesInReach += static_cast<int64>(ComponentInstances);
            }
        }
    }

    if (!bAnyComponentSeen)
    {
        // Components exist in the cache but hold no instances yet. That is a build state, already
        // carried by pendingComponents/pendingTasks; reporting a reach of zero here would blame
        // culling for it.
        return;
    }

    InOutReport.bReachMeasured = true;
    InOutReport.bCullingOriginMeasured = bInCullingOriginMeasured;
    InOutReport.CullingOrigin = CullingOrigin;
    InOutReport.NearestComponentDistance = NearestApproachSeen;
    InOutReport.GrassCullDistance = bAnyCullDistance ? SmallestCull : 0.0;
    InOutReport.ComponentsInReach = ComponentsInReach;
    InOutReport.InstancesInReach = InstancesInReach;
}

TSharedPtr<FJsonObject> MakeGrassBuildInfoObject(const FGrassBuildReport& Report)
{
    TSharedPtr<FJsonObject> Grass = MakeShared<FJsonObject>();
    Grass->SetBoolField(TEXT("measured"), Report.bMeasured);
    if (!Report.bMeasured)
    {
        // No world, or no landscape subsystem on it. Everything below would be a zero standing in
        // for a measurement that was never taken (docs/rpc-design.md section 4), so it is omitted.
        return Grass;
    }

    Grass->SetNumberField(TEXT("landscapes"), Report.LandscapeProxies);
    Grass->SetBoolField(TEXT("builtForPose"), Report.bBuiltForPose);
    Grass->SetBoolField(TEXT("settled"), Report.bSettled);
    if (Report.LandscapeProxies == 0)
    {
        // A level with no terrain. `landscapes: 0` is the whole answer; component and instance
        // counts below would be zeros about nothing.
        return Grass;
    }

    TSharedPtr<FJsonObject> CameraObject = MakeShared<FJsonObject>();
    CameraObject->SetNumberField(TEXT("x"), Report.CameraLocation.X);
    CameraObject->SetNumberField(TEXT("y"), Report.CameraLocation.Y);
    CameraObject->SetNumberField(TEXT("z"), Report.CameraLocation.Z);
    Grass->SetObjectField(TEXT("cameraLocation"), CameraObject);

    Grass->SetNumberField(TEXT("components"), Report.ComponentsAfter);
    Grass->SetNumberField(TEXT("componentsBefore"), Report.ComponentsBefore);
    // Omitted rather than published as a floor when a cache entry could not be read at all. The
    // whole value of this field is that `instances: 0` beside `settled: true` means bare ground;
    // an incomplete total emitted as a number takes that meaning away from every honest zero.
    if (Report.bInstancesMeasured)
    {
        Grass->SetNumberField(TEXT("instances"), static_cast<double>(Report.InstancesAfter));
    }
    if (Report.UnreadableComponents > 0)
    {
        Grass->SetNumberField(TEXT("unreadableComponents"), Report.UnreadableComponents);
    }
    Grass->SetNumberField(TEXT("pendingComponents"), Report.PendingComponents);
    Grass->SetNumberField(TEXT("pendingTasks"), Report.PendingTasks);
    if (Report.bBuiltForPose)
    {
        Grass->SetNumberField(TEXT("buildMs"), Report.BuildMs);
    }

    // The reach half. Omitted rather than zeroed when nobody measured it, because
    // `instancesInReach: 0` is exactly the reading it would be mistaken for.
    if (Report.bReachMeasured)
    {
        TSharedPtr<FJsonObject> Reach = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> OriginObject = MakeShared<FJsonObject>();
        OriginObject->SetNumberField(TEXT("x"), Report.CullingOrigin.X);
        OriginObject->SetNumberField(TEXT("y"), Report.CullingOrigin.Y);
        OriginObject->SetNumberField(TEXT("z"), Report.CullingOrigin.Z);
        Reach->SetObjectField(TEXT("cullingOrigin"), OriginObject);
        // The renderer culls from this point, and for a lit editor orthographic view it is not
        // the camera. Published beside the distances so the numbers below can be read at all.
        Reach->SetBoolField(TEXT("cullingOriginMeasured"), Report.bCullingOriginMeasured);
        Reach->SetNumberField(TEXT("cullingOriginPushback"),
            FVector::Dist(Report.CullingOrigin, Report.CameraLocation));
        Reach->SetNumberField(TEXT("nearestComponentDistance"), Report.NearestComponentDistance);
        if (Report.GrassCullDistance > 0.0)
        {
            Reach->SetNumberField(TEXT("grassCullDistance"), Report.GrassCullDistance);
        }
        Reach->SetNumberField(TEXT("componentsInReach"), Report.ComponentsInReach);
        Reach->SetNumberField(TEXT("instancesInReach"), static_cast<double>(Report.InstancesInReach));
        Grass->SetObjectField(TEXT("reach"), Reach);
    }

    if (!Report.bBuiltForPose)
    {
        // The pose never became a grass camera. This is the pre-fix state of every capture, and
        // the reason the ticket exists: the frame is a picture of grass built around some other
        // point. Names the transaction when that is what stopped it, because the remedy differs.
        Grass->SetStringField(TEXT("grassWarning"), Report.bBlockedByTransaction
            ? FString::Printf(
                  TEXT("This capture's pose was NOT used to build the landscape grass: an editor "
                       "transaction was open, and ALandscapeProxy::UpdateGrass returns without "
                       "doing anything while GUndo is set. The %d grass component(s) in this frame "
                       "were built around whatever camera the editor last ticked with, which is "
                       "not necessarily anywhere near this shot. Re-take the capture outside the "
                       "transaction, or move the camera there with editor.set_camera first and let "
                       "the editor tick."),
                  Report.ComponentsAfter)
            : TEXT("This capture's pose was NOT used to build the landscape grass, so the grass in "
                   "this frame was built around a different camera and may be absent from ground "
                   "that really is covered. Move the camera with editor.set_camera before "
                   "capturing and read this block off the new response."));
    }
    else if (!Report.bSettled)
    {
        // The build ran for this pose and did not finish. This is the case a caller must not read
        // as bare ground, and the only field that could ever have told them apart.
        Grass->SetStringField(TEXT("grassWarning"), FString::Printf(
            TEXT("The landscape grass build for this pose had not finished when the frame was "
                 "read: %d grass component(s) still pending and %d async task(s) outstanding, "
                 "after a synchronous build of %.1f ms. Ground that looks bare in these pixels may "
                 "simply not have been built yet -- do NOT read this frame as missing vegetation. "
                 "Re-shoot at the same pose; the components built by this call are cached, so the "
                 "second shot starts from further along."),
            Report.PendingComponents, Report.PendingTasks, Report.BuildMs));
    }
    else if (Report.bReachMeasured && Report.InstancesAfter > 0 && Report.InstancesInReach == 0)
    {
        // Built, finished, and none of it can reach the frame. Without this line the response is
        // `builtForPose: true`, `settled: true`, a large `instances`, success, and a picture of
        // bare ground -- the exact silent-wrong-data failure
        // B-ortho-capture-renders-no-landscape-grass was filed for. The remedy differs by whether
        // the culling origin is the camera, so the two cases say different things.
        const double Pushback = FVector::Dist(Report.CullingOrigin, Report.CameraLocation);
        // Named because the two cases need different remedies: an origin sitting on the camera is
        // a genuine cull-distance shortfall, one sitting far off it is the orthographic near-plane
        // correction moving the point every distance is measured from.
        const FString PushbackNote = Pushback > 1.0
            ? FString::Printf(
                  TEXT("That point is %.0f cm from the capture camera, not on it, which is what an "
                       "orthographic editor view's near-plane correction does. "),
                  Pushback)
            : FString();
        Grass->SetStringField(TEXT("grassWarning"), FString::Printf(
            TEXT("The landscape grass for this pose is BUILT and SETTLED -- %d component(s), "
                 "%lld instance(s) -- and NONE of it can appear in this frame: the nearest grass "
                 "component sits %.0f cm from the point the renderer culls from, past its %.0f cm "
                 "effective end-cull distance. %sThese pixels show bare ground because the grass "
                 "is culled, NOT because the ground has none on it -- do not read this frame as "
                 "missing vegetation. Raise grass.CullDistanceScale, or narrow the frame so the "
                 "culling origin sits closer to the ground."),
            Report.ComponentsAfter,
            static_cast<long long>(Report.InstancesAfter),
            Report.NearestComponentDistance,
            Report.GrassCullDistance,
            *PushbackNote));
    }
    else if (!Report.bInstancesMeasured)
    {
        // Last in the chain on purpose: every branch above describes something that went wrong
        // with the BUILD, which is more actionable than "the total is short". This one fires only
        // when the build itself is clean and the only thing missing is the count -- and it must
        // fire, because `instances` is absent above and an absent field with nothing said about it
        // reads as an older server rather than as a measurement that could not be taken.
        //
        // Cannot fire on a fully-read report: bInstancesMeasured is set from
        // UnreadableComponents == 0, so a settled build with zero instances and every component
        // readable stays warning-free -- that is the honest "this ground is bare" answer and
        // warning about it would train callers to discount it.
        Grass->SetStringField(TEXT("grassWarning"), FString::Printf(
            TEXT("The landscape grass instance count is NOT published for this capture: %d of the "
                 "%d grass component(s) in the foliage cache had already been destroyed when the "
                 "count ran, so any total would be a floor rather than a measurement. Do NOT read "
                 "the absent `instances` as zero -- this frame may show a full carpet. `components` "
                 "is measured and is the field to read here; re-take the capture for a total."),
            Report.UnreadableComponents, Report.ComponentsAfter));
    }

    return Grass;
}
}
