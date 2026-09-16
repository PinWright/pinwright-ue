# performance-profiling

How to profile a level and act on the result without measuring the wrong thing. The order of
operations decides the answer — reproduce, classify, find the bound thread, attribute, then change
one thing — and each step has a specific way of lying to you; pairs with [`insights`](insights.md)
for capture, [`performance`](performance.md) for scalability and CVar state, and
[`level-review`](level-review.md) for capture comparability.

## A Fixed-Pose Profile Cannot See A Convergence Cost

Read this before designing a measurement rig. It is the failure this page exists to prevent, and the
rig that produced it was careful.

An end user reported roughly 3 FPS flying inside a forest. The profiling pass that answered it chose
poses from an instance-density histogram, shot at fixed pitch and yaw from a lattice of probe points,
measured 11.5–15.4 ms and later 19–20 ms at the densest canopy pose, and ranked the cost: shadow
depths 37.7%, Nanite visibility buffer 20.4%, base pass 11.2%. It attributed the forest premium to
one mesh's world-position-offset material by interleaved A/B — freezing that one mesh's 840 instances
was indistinguishable from freezing every foliage mesh in the level, and shadow depths fell 8.36 →
1.44 ms with its WPO off. It ruled out overdraw, shadow-page thrash, inflated bounds, global
illumination and translucency **by measurement**. It concluded: playable, 50 FPS at the worst pose,
and the user's 3 FPS was the editor window being minimized.

Every one of those measurements was correct. The conclusion was wrong.

The user's own trace showed a single scope — the Lumen surface cache's mesh-card capture — at
**272 ms of a 300 ms frame, 92%** — while every pass the profile had ranked stayed **flat across a
15x frame-time swing** (shadow depths moved 4.45 → 5.99 ms; visibility buffer 3.16 → 4.22).
Subtracting that one scope reproduced the profile's own 19 ms number exactly. **The pass measured
the scene correctly; it measured the wrong thing.**

The cost was invisible to that method by construction. Surface-cache capture is a **convergence
cost**: it is paid only while the camera keeps bringing *unvisited* geometry into the scene. A static
pose warms the cache within a second and then measures a warm cache. Repeating the pose for
stability warms it harder. Even the fly-through that pass ran was a ping-pong over one fixed path,
which warms on the first traversal and then measures ~2,100 warm frames. Sample fixed poses, repeat
for stability, take the low percentile — **that method is structurally blind to any cost that decays
as you dwell**, and it will return a confident, internally consistent, wrong answer.

It resolved to one content decision, found by the subsystem's own logging CVar: ~36,000 instances of
a small non-Nanite ground plant, each its own surface-cache entry, requesting captures faster than
the per-frame budget could retire them. Taking that one mesh out of the global-illumination scene
took the worst route from 38 to 59 scene renders per second, removed every frame over 100 ms, cost
nothing at any settled pose, and changed the picture by less than the wind moves between two
consecutive frames. **The fix was one property on one asset; the whole difficulty was attribution.**

## Reproduce Before You Measure

Measure the user's conditions, not a heuristic's idea of the worst case.

- **Get the user's trace.** It carries the camera path, the render resolution, the frame
  distribution, and which frames were actually slow. Every hour spent constructing an equivalent
  repro is an hour spent guessing at the thing the trace already contains.
- **Fly the way a person flies** — inside the geometry, moving continuously, into ground this
  session has not visited, looking where they look. Not a lattice of static probes at fixed yaw, and
  not a ping-pong along one path.
- **Choosing poses by measurement is still worth doing.** A density histogram finds the worst
  *raster* pose honestly. It cannot find a cost that exists only in motion, so it is step one of two,
  never the whole rig.
- **A profile of a level nobody is flying through is a profile of a screensaver.** State plainly
  which regime you measured; a pose lattice remains valid evidence for raster cost and for nothing
  else.

## Classify The Cost Before You Rank It

Ranking both classes in one table is how the case above went wrong.

**Steady-state costs** are visible from any pose that contains the geometry: rasterisation, shading,
shadow depths, post-processing, translucency. Fixed poses measure these correctly, and a ranked
table of them is meaningful.

**Convergence costs** are paid while state is being built and decay as you dwell: global-illumination
surface-cache capture, virtual-shadow-map page caching, texture and level streaming, shader and PSO
warm-up, acceleration-structure builds after a large scene change.

**The tell: a cost that vanishes when you stop moving, or when you revisit the same ground, is a
convergence cost, and no static-pose method will find it.** Test it directly rather than reasoning
about it — stop the camera and watch whether the scope's per-frame count collapses.

Two properties make a convergence cost worse than a steady-state cost of the same mean. It is
**bursty**, because it saturates a per-frame budget: frames are 300 ms or 15 ms with little between,
so the median describes no frame anyone saw. And it can **fail to converge** — in the trace above the
storm cleared for 1–2 s and immediately rebuilt, five times in 54 s, so it never amortised into
warm-up.

Report convergence scopes on their own line, separately from the raster passes. A merged ranking is
what let a 92% cost hide behind a correct 37.7%.

**When a hypothesis survives measurement, check whether the engine implements the mechanism at all.**
The WPO root cause above was plausible for the 300 ms frames and was wrong for a reason no
measurement would have surfaced: engine source disables world-position-offset in the Lumen card
capture vertex shader outright, and Nanite meshes emit no per-page capture scope at all, so every one
of those captures was non-Nanite geometry and the suspected mesh could not be the offender.

**The subsystem usually has a logging CVar that names the object it is working on.** Reach for it
before guessing: `r.LumenScene.SurfaceCache.LogUpdates 2` logs the mesh name, instance index and
primitive count of every captured page and names the offender in one frame.

**Then rank its output by which objects actually emit the cost, not by how often they appear.** The
log lists every unit of work queued, and only some of those units cost anything: Nanite primitives
went through one batched path and emitted no per-page timing scope, while non-Nanite pages each
became a real ~0.95 ms draw. The most frequent name in the log — 39% of pages on slow frames, 68% on
fast ones — was a Nanite mesh costing nothing measurable. Split the list by the property that decides
the code path, then compare the split across speed classes: the offender was 0.6% of pages on calm
frames and 40.5% on slow ones, and was the only non-Nanite entry of any size.

**Check the engine's default before blaming the thing everyone expects.** The prime suspect was
landscape grass — a document in the same repo predicted it by name, and 185 grass components used
exactly the offending mesh. `r.RayTracing.Geometry.LandscapeGrass` defaults to `0`, so under hardware
ray tracing grass has no ray-tracing representation and is already outside the global-illumination
scene; the grass-variety struct has no field for the flag that would have been the fix, which is the
tell that the engine never intended it to participate.

## Reproducing A Convergence Cost Needs A Route, And A Reset

Two reproduction attempts failed before one worked, and both failures generalise.

**The densest pose can be the wrong pose.** A 40-second continuous flight through the level's densest
canopy — chosen by an instance-density histogram, flown with a cold cache, exactly what the section
above prescribes — reproduced nothing: 60 FPS, worst frame 26 ms. That canopy was built from meshes
on the cheap code path; the cost lived in open ground the histogram ranked near the bottom. A
120-second serpentine over the whole map found it, then reproduced to within two frames across runs.
**A convergence cost is a property of the route, not of the worst frame's contents.**

**Reset the subsystem's cache at the start of every leg — and know the reset can also hide the bug.**
Unvisited ground is a one-shot resource: whichever config flies a stretch first pays the cost and
warms it for the other, so a naive interleaved A/B measures order, not configuration. A per-leg reset
(`r.LumenScene.SurfaceCache.Reset 1`, a one-shot the engine clears after acting on it) makes both
sides fly identical cold geometry, and repeats then hold to ±0.6 FPS. The same reset before a *short*
leg guarantees an under-subscribed cache for its whole duration — the regime where nothing goes
wrong, and the cause of the false negative above.

**Advance the camera by wall-clock time, not frame count, and report scene renders per second.**
Frame-count advance lets a slow config linger and converge, flattering it. Time advance flies both
configs over the same ground in the same seconds, so the metric is how many frames each managed —
also the number the user experiences. The fix here moved mean frame time 26.3 → 16.8 ms and frames
over 200 ms from 74 to 0 while leaving p50 at 16.2. **A convergence fix removes a spike; expect the
median not to move, and do not read that as the change having done nothing.**

## Find The Bound Thread First

One measurement eliminates most hypotheses, and it costs nothing.

In a trace, sum exclusive time per thread. **A game thread dominated by its own wait scope is not the
cost.** In the trace above, `GameThreadWaitForTask` was 45.70 s of 54 s (84.6%), rising to 93.6% in
the worst window, and total game-thread work outside waits was ~8 s, almost all UI. Downstream the
chain is clean: render thread in `WaitForTasks` 89.2% of that window, RHI thread in present and
sync-point waits. GPU-bound, established in one pass.

Live, without a trace, the same finding reads differently: render-thread time tracking frame time to
two decimals while the sum of its own named scopes is a couple of milliseconds means the render
thread is blocked on the GPU.

**A compute-queue scope can be a cross-queue fence wait rather than work.** In the same trace an
async-compute `RayTracingDynamicGeometryUpdate` showed 278 ms and looked like a second bottleneck.
Its spans matched the graphics queue's capture spans to within 0.05–0.2 ms at both ends, and 99.3% of
its wall time was covered by graphics-queue execution: it was waiting. Its real cost is the 0.53 ms
it shows on fast frames. Before ranking any compute-queue scope, check whether its spans bracket a
graphics-queue scope; if they do, do not rank it as a cost.

## Separate Frames That Drew The Scene From Frames That Did Not

An editor session interleaves scene renders with UI-only frames, and mixing them destroys the
distribution.

| | n | p10 | p50 | p90 | p99 | max |
|---|---|---|---|---|---|---|
| frames that drew the scene | 619 | 18.6 | **24.9** | **185.6** | **406.5** | **718.7** |
| frames that drew UI only | 508 | 8.4 | 12.7 | 18.3 | 362.2 | 383.3 |
| all frames | 1127 | 8.6 | 20.0 | 79.8 | 375.3 | 718.7 |

The merged p50 of 20.0 ms is a number no frame in that trace resembled. Split on whether the
scene-render scope appears in the frame, then report **scene renders per second** alongside frame
time — sustained stretches at 2–4 scene renders per second are what a user calls "3 FPS", and they
are invisible in an average.

## Report The Resolution, Always

**A profile that does not state its render resolution is not a result.** With a temporal upscaler the
render resolution is not the viewport size — read it off the profiler's temporal-upscale pass
dimensions, not off a viewport-size query. State scalability level, editor viewport versus PIE, and
the window state alongside it.

An editor window does not come back the same size across restarts: one machine gave 1515x939, then
738x502, then 1024x726 for the same session. The smallest reads about 50% faster and would have
looked like a spectacular optimisation. Pin the window size before baselining and verify it after the
capture, not only before.

## What Will Lie To You

Each of these was measured, and each cost real time.

**A minimized editor renders at exactly 333.33 ms per frame with zero draw calls.** It reads as a
catastrophic regression and it is an idle cap: the editor engine's max tick rate is a hardcoded 3 FPS
whenever it throttles, and it throttles whenever every top-level window is hidden — the
*throttle CPU when not foreground* setting being off does **not** prevent it, because the
hidden-window check runs after that setting and overrides it. The editor also skips the viewport
redraw entirely in that state, so the CSV reports 0 draw calls, frozen thread times and garbage
counters. The tells are the constant to five figures and the zero draw count. `t.MaxFPS 0` is
irrelevant; no CVar disables this path.

  A window verb cannot recover it. `editor.set_window_state {state:"restored"}` returns
  `NO_WINDOWS`, because a minimized window is not a *visible* top-level window and the shared window
  selector enumerates only visible ones. Recovery is a platform restore call against the editor
  process. **The one state the verb exists to fix is the one it cannot select — file it when you meet
  it.**

**A non-interleaved A/B measures drift, not your change.** One machine got **18% faster over 50
minutes with nothing changed**: two identical baseline sweeps of the same six poses, before and after
a tuning pass, differed by −18% to −22% at every pose. Every A/B taken against the first baseline was
wrong by enough to invent conclusions — three changes read as −27%, −16% and −20%, and re-measured
interleaved the largest of those was −3.3%. **Alternate A and B several times inside one run,**
applying the properties and CVars at each switch, so the drift lands on both sides; repeats then hold
to ±0.1 ms. **A low percentile does not protect you** — it defends against stalls *within* a capture,
not against the baseline moving *between* two captures.

**A stray background process invalidates a whole baseline.** A second engine process on the machine
made every GPU pass about 40% more expensive at identical resolution and identical draw calls. Check
the process list before baselining and discard anything captured alongside one.

**A change that helps one pose can hurt another.** Measure the regression poses, not only the pose
you are optimising, and reject on the worst one: a per-species distance table measured −3.5% at one
pose, +2.3% at a second and +0.47 ms at a third, and was reverted.

**Both halves of a comparison must have seen the same scene.** In a shared editor the
primitives-drawn count is the cheapest guard — it jumped 95,648 → 151,568 when another agent's edit
landed mid-comparison. Verify the camera pose after the capture as well as before; another agent's
capture verb can move the viewport between your two halves.

**Compare a low percentile, not the median, on a machine that stalls.** One capture gave p25 15.08,
p50 20.29, p75 41.24 on identical draw calls. A stall can only add time, so p10 tracks the real cost:
it reproduced to 0.07 ms across independent samples where the median wandered by 4 ms. This is
orthogonal to the drift hazard above and does not substitute for it.

**Interacting with the viewport during a backlog costs the whole backlog.** Hit-proxy readback on
mouse hover or click flushes rendering commands, blocking the game thread until the GPU drains — the
worst frame in the trace above, 719 ms, is a click landing inside a 300 ms backlog. Do not touch the
viewport during a capture.

**`unreal.Rotator()` takes `(roll, pitch, yaw)` positionally.** A requested `(-5, 135, 0)` reads back
as pitch 89.99, yaw 0, roll −5; a whole 17-pose sweep was measured at the wrong orientations before
this was caught. Always pass it by keyword.

**A CVar you set from the console stays pinned for the session.** After a probe it reads
`LastSetBy: Console` instead of `Scalability`, so a later scalability change silently will not move
it. Put each probe back and restart the editor before any baseline that depends on scalability being
authoritative.

## Attribute Before You Optimise, And Record What You Rejected

**A change that helps but was not the bottleneck is noise dressed as progress.** Establish the
attribution first — which scope's cost tracks the frame time across speed classes — then change one
thing and re-measure interleaved.

**Prefer the fix that keeps the look, and price the ones that do not.** Wind, contact shadows and
bounce light are load-bearing to a scene; a lever that removes them is a trade to be argued, not an
optimisation to be landed quietly. Quantify the visual cost the same way you quantify the frame cost:
two captures four seconds apart differed by mean |dLuma| 6.76/255 with 27.9% of pixels moving more
than 8/255, concentrated in the canopy — that is what "the trees move" is worth in a number, and it
is what a distance cutoff spends.

**Record the rejections. They are the most reusable part of a profiling report,** because the next
reader's first instinct is usually one of them:

- Cutting cull distances removed **41% of grass instances** and drew **31% more triangles**, making
  the frame *slower*, while stripping the mid-distance ground back to bare material. Instance count
  was never the cost — at one pose 723,880 instances drew 95,648 triangles, 0.13 each, because
  distant instances are GPU-culled for free. The cost is near-field pixels, so the dial that pays is
  density or the material, never `EndCullDistance`.
- A world-position-offset disable distance helped the target pose and hurt two others.
- Bounding a WPO material's maximum displacement changed nothing at three poses, closing a
  hypothesis that inflated bounds were the shadow cost.

**Mean luminance is identical to four decimals across the cull-distance regression.** Only the
picture caught it. A numeric gate cannot sign off a visual change — see
[`level-review`](level-review.md).

## Config Inherited From Elsewhere Is A First-Class Suspect

The enabling condition for the 300 ms frames was a rendering CVar set project-wide in a
`[SystemSettings]` ini section **for a different level**, months earlier:
`r.LumenScene.SurfaceCache.AtlasSize` raised 4096 → 8192. That doubles the per-axis size of the
surface cache's *capture scratch atlas* — 16 → 64 pages per frame. At the default the scratch atlas
throttles captures long before the per-frame count cap is reached; raised, nothing throttles them, so
a frame runs to the count cap of 300 captures at ~0.95 ms each, which is 285 ms, which is the
observed slow frame to within noise.

**A CVar raised to fix one scene removes a throttle in every scene.** Before profiling, dump the
CVars that matter *with their set-by source* and treat anything applied by a project-wide ini section
as suspect until you know which scene it was for. The remedy for that class of setting is usually
per-map rather than a global revert — reverting it globally regresses the level it was added for.

`[SystemSettings]` values also outrank scalability: the engine reported the same CVar as
`Constructor: 4096`, `Scalability: 4096`, `SystemSettingsIni: 8192`, and the ini's own comment about
the default was wrong. Read the value the engine reports, not the comment beside it.

## What To Report

- **The conditions**: level, render resolution, scalability, viewport or PIE, window state, and
  whether the machine was otherwise idle.
- **How the poses or the path were chosen**, and whether the camera was moving through geometry the
  session had not already visited.
- **The frame distribution**, split into frames that drew the scene and frames that did not, with a
  tail percentile — never a single average.
- **Which thread is bound, and the evidence** that made it so.
- **The ranked cost, with convergence scopes on their own lines** and any cross-queue scope
  identified as a wait rather than work.
- **Every change measured, including the rejected ones**, each with what it costs the look.
- **What the method could not see.** A profile that names its own blind spot is the only kind that
  can be safely built on. The pass in the case study above named none, and its verdict — *playable,
  the user's report is a window state* — stood until the user's own trace arrived the next day.

## See also

- [`performance-profiling.headless-insights`](performance-profiling.headless-insights.md) — analysing
  a `.utrace` out of process, the four ways the export command line lies, and what the CSV cannot
  carry.
- [`insights`](insights.md) — starting and stopping a trace, exporting it, and the untraced-CPU-time
  playbook for cost that hides as self-time of a coarse scope.
- [`insights.tracing-reference`](insights.tracing-reference.md) — channels, `Trace.*` commands,
  launch args, and the CSV-profiler capture recipe used for per-frame budgets.
- [`insights.stat-companions`](insights.stat-companions.md) — `stat` commands that find the bound
  thread without recording a trace.
- [`performance`](performance.md) — scalability, CVar tuning and stat snapshots on the live session.
- [`level-review`](level-review.md) — capture comparability, pinned exposure, and why a number never
  carries a visual claim on its own.
- [`vegetation-authoring`](vegetation-authoring.md) — the dense-scatter cost figures this page's
  rejections came from, in their content context.
