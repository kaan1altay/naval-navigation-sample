# Project status

_Last updated: 2026-08-17 — end of Slice 3._

> **Now building on Windows.** Slices 1–3 compile with UE 5.5 + Visual Studio 2022
> (MSVC 14.44) via Unreal Build Tool, and all **18 automation tests pass in-engine**
> (`Automation RunTests NavalNav`, headless, `-nullrhi`). The Linux notes below are the
> history of how Slice 1 was first authored; they no longer describe the only place the code
> has run.

## Environment

Slice 1 was first authored in a **Linux container** (Ubuntu 24.04, x86-64), not on the Windows
workstation the task assumed, so at the end of Slice 1 the project had not yet been compiled by
Unreal Build Tool — everything was written against the UE 5.5 API and verified only by the
engine-free harness. Slice 2 was built on the Windows box, where the whole module now compiles
and the tests run in the editor.

| Tool | Found | Notes |
| --- | --- | --- |
| Unreal Engine 5.x | **no** | No `/opt`, `/usr/share` or Epic launcher install; the machine is Linux, so `C:\Program Files\Epic Games\UE_5.*` and the Windows registry do not apply. |
| Visual Studio 2022 (C++ game dev) | **no** | Windows-only; `vswhere` not applicable on this platform. |
| .NET SDK (needed by UnrealBuildTool) | **no** | `dotnet` not on PATH. |
| clang++ / g++ | **yes** | clang++ and g++ present, C++20 capable. Used for the standalone test harness. |
| git | **yes** | 2.43.0. |
| `gh` CLI | **no** | Not installed. Not needed: the GitHub remote was already configured. |

The project targets **UE 5.5** (`EngineAssociation` in `NavalNavSample.uproject`). Nothing large
was installed.

## What is implemented — Slice 1: sea grid, A\*, debug draw

```
Source/NavalNav/
  Grid/SeaGridTypes.{h,cpp}       FSeaGridConfig, FSeaGridData, FNavalPath
  Grid/SeaGrid.{h,cpp}            USeaGridSubsystem (UWorldSubsystem)
  Grid/SeaGridPathfinder.{h,cpp}  FSeaGridPathfinder, FSeaGridPathQuery
  Grid/SeaGridDebugDraw.{h,cpp}   FSeaGridDebugDraw, FSeaGridDebugDrawSettings
  Threat/DangerZone.{h,cpp}       ADangerZone
  Threat/ThreatEvaluator.{h,cpp}  FThreatEvaluator
  Debug/NavalNavDebugActor.{h,cpp} ANavalNavDebugActor
  Tests/SeaGridPathfinderTest.cpp 8 automation tests
Tools/AlgoSelfTest/               Engine-free test harness (development aid)
```

**Sea grid.** `FSeaGridData` is a plain `USTRUCT` holding the grid geometry plus two cost
layers: a static base layer (open water, land, shallows) and a mutable threat layer. Defaults
cover 40 km × 40 km at 200 uu per cell (200 × 200 = 40,000 cells), and the cell count is
clamped so a mistyped `CellSize` grows the cells rather than allocating gigabytes.

Keeping the data out of the subsystem is deliberate: it is what allows the search to run in a
unit test with no `UWorld`, and it lets the threat layer be re-stamped every frame without
touching the static data.

**`USeaGridSubsystem`** is the one grid per world. Danger zones register with it, route
consumers query it. It owns a persistent `FSeaGridPathfinder` so repeated replans reuse the
same scratch buffers. The threat layer is rebuilt lazily: zones raise a dirty flag, and only
once they have drifted at least half a cell, so ten moving zones cost one rebuild per frame
rather than ten.

**A\*** is 8-neighbour with an octile heuristic. Notable properties:

- cell cost comes from an **injected functor**, not from the grid directly, so the same search
  can run against ground truth or against what a particular ship believes and can rate threats
  per-ship. Slice 4 needs exactly this hook.
- open/closed state lives in reusable buffers marked with a **per-query generation stamp**, so
  a replan neither allocates nor clears 40k entries.
- diagonal steps refuse to cut the corner between two touching obstacles.
- `HeuristicWeight` above 1 gives weighted A\* (fewer expansions, near-optimal cost);
  `MaxSearchedCells` caps a hopeless query so it cannot stall a frame.
- the route is simplified by collinear-waypoint removal plus a **cost-aware string pull**: a
  shortcut is only taken when the straight line is not meaningfully more expensive than the
  grid route it replaces, so smoothing can never cut back through the danger the search just
  spent cost avoiding.

**Threat.** `ADangerZone` contributes only a footprint (position, radius, power) and knows how
to stamp it; the subsystem decides when. Cost falls off smoothly from the centre, with an
optional lethal core that is flatly impassable. Threat is *relative*: `FThreatEvaluator`
decides hostility by comparing zone power against the planning ship's own power, so a strong
ship ignores zones it outguns and sails straight through.

**Debug.** `ANavalNavDebugActor` is dropped into a level, owns two draggable endpoints, replans
on an interval and draws grid, zones and route, with an on-screen readout of cost, length,
expanded cells and plan time. It ticks in the editor viewport too, so a zone can be dragged
around and the route seen bending without entering PIE. Console variables: `naval.DrawGrid`,
`naval.DrawCosts`, `naval.DrawPath`.

## What is implemented — Slice 2: sailing ship + wind

```
Source/NavalNav/
  Ship/SailingModel.{h,cpp}             FSailingModel, FSailingModelParams, FSailingState
  Ship/WindSubsystem.{h,cpp}            UWindSubsystem (UTickableWorldSubsystem)
  Ship/SailingShipPawn.{h,cpp}          ASailingShipPawn (kinematic)
  Ship/ShipPowerComponent.{h,cpp}       UShipPowerComponent + FOnPowerChanged
  Ship/SailingShipConfig.h              USailingShipConfig (UDataAsset, optional rig sharing)
  Ship/SailingShipPlayerController.*    ASailingShipPlayerController (optional manual control)
  Tests/SailingShipTest.cpp             5 automation tests
```

**Sailing model.** `FSailingModel` is a plain, engine-free struct — the same discipline as the
Slice 1 pathfinder core — so the physics is unit-tested without a `UWorld`. The heart of it is
the **polar curve**: forward drive as a function of the angle between the bow and the wind. It is
flatly zero inside a **no-go zone** (±`NoGoAngleDegrees`, default 40°) dead upwind, rises smoothly
to a peak of 1 on a **beam reach**, and eases to `DownwindFactor` (0.65) dead downwind. Speed then
integrates against a **quadratic drag** toward a bounded terminal, and — this is the tuning trick —
the drag coefficient is derived from `MaxThrustAccel` and `MaxSpeed` so thrust and drag balance
*exactly* at `MaxSpeed`. Terminal speed is therefore `MaxSpeed · √drive`, bounded by construction
rather than by luck. The drag step is semi-implicit, so it stays stable and non-negative under any
frame time. Yaw comes from the rudder scaled by **steering authority**, which is zero at rest and
reaches full bite at `SteeringResponseSpeedRatio` of top speed: a ship cannot turn without way on,
which is the constraint that makes the Slice 3 follower non-trivial. `PredictTurnRadius` and
`EstimateTimeToTurn` are provided for that follower's look-ahead.

**Wind.** `UWindSubsystem` is one wind per world, read by anything that sails. It carries a
direction (yaw the wind blows *toward*) and a 0..1 strength, can drift the direction slowly for a
demo (`bDriftDirection`), and can be overridden live from the console — `naval.Wind.Yaw`,
`naval.Wind.Strength` — without touching a placed actor. `GetWindFromYaw()` returns the opposite
bearing, which is the angle the polar is measured against.

**Ship.** `ASailingShipPawn` is **kinematic** — no PhysX, no Chaos, no buoyancy. It is a thin
shell around `FSailingModel`: it reads the wind, advances the model, and writes the result straight
onto the actor transform. Inputs are two numbers, `SetRudderInput(-1..1)` and `SetSailTrim(0..1)`.
Optional lateral **leeway** adds a little cosmetic downwind drift. `naval.Ship.Debug` draws a
heading arrow, a wind arrow, the rudder kick and a text readout (heading, speed, rudder, trim,
angle off wind, and a NO-GO flag), and the pawn ticks in an editor viewport so it can be tuned
without PIE. `UShipPowerComponent` holds the ship's combat power and fires `FOnPowerChanged` for
Slice 4 to react to. `ASailingShipPlayerController` (optional) lets a human sail with A/D + W/S for
recording clips, building its Enhanced Input actions in code so there is no asset to ship.

**What is tunable** (all `EditAnywhere`, grouped, or via a `USailingShipConfig` data asset):
`MaxSpeed`, `MaxThrustAccel`, `MaxTurnRateDegPerSec`, `MaxRudderAngleDegrees`, `RudderRateDegPerSec`,
`SteeringResponseSpeedRatio`, `NoGoAngleDegrees`, `BeamReachAngleDegrees`, `DownwindFactor`,
`bEnableLeeway`/`LeewayFactor`; and on the wind, direction, strength and drift.

**Known simplifications (deliberate).** No sideways hull momentum or inertia in the turn — heading
changes are driven straight by the rudder, not by an accumulated angular velocity. No apparent-wind
feedback (the polar uses true wind, not the wind altered by the ship's own motion). Trim maps
linearly onto drive rather than modelling sail stall past the ideal sheeting angle. Tacking through
the eye of the wind loses speed only through the no-go zero, not through an explicit momentum cost.
None of these are needed for a readable, predictable ship the navigation follower can drive; the
model is intentionally the *simplest* thing that still forces tacking and reproduces turn radius.

## What is implemented — Slice 3: predictive helmsman + navigator

```
Source/NavalNav/
  Navigation/PredictiveHelmsman.{h,cpp}    FPredictiveHelmsman, FHelmsmanParams, FHelmsmanOutput
  Navigation/NavalNavigatorComponent.{h,cpp}  UNavalNavigatorComponent (state machine, OnArrived)
  Demo/NavalNavDemoGameMode.{h,cpp}        Spawns fleet + zones + wind from an empty map
  Demo/NavalNavDemoPlayerController.{h,cpp}  Click-to-move, Tab cycle, 1/2/3 debug toggles
  Tests/PredictiveHelmsmanTest.cpp         5 automation tests
```

**The design principle, kept honest.** The planner is physics-agnostic and the helmsman owns the
physics. A\* returns an `FNavalPath` that knows nothing about wind, turning circles or momentum;
the helmsman is what reconciles that geometric *intent* with a hull that cannot point upwind and
cannot corner on a dime. The path is a suggestion, not a rail.

**Predictive helmsman.** `FPredictiveHelmsman` is a plain, engine-free struct (same discipline as
the sailing model and the pathfinder). Each tick it:

- steers at a **look-ahead point** walked along the path, at a distance that scales with speed, so
  a fast ship anticipates further — this is pure-pursuit tracking, not corner-chasing;
- starts each turn **before** the corner. It reads the heading change at the next waypoint and,
  from `FSailingModel::PredictTurnRadius`, computes a **turn-in distance** `R·tan(θ/2)`; once the
  ship is within it, the desired heading is blended toward the next leg so the ship carves the
  corner instead of overshooting. Turn-in grows with speed and with corner sharpness;
- commands rudder as a **PD** on bearing error (with a deadband) damped by the measured yaw rate,
  and eases sheets for arrival (`SlowdownRadius` → `ArrivalRadius`) and in hard turns;
- **advances the active waypoint** as soon as it is within `WaypointAcceptRadius` *or* has fallen
  behind the ship, so a ship that misses a waypoint never circles back for it;
- **tacks** when the course it wants is inside the no-go cone: it holds the nearer close-hauled
  edge (with hysteresis) until the bearing clears the cone. Deliberately simple — see the
  simplification note below.

Outputs are the rudder and trim orders plus telemetry (look-ahead point, turn-in point, bearing
error, tacking flag) for the debug draw and the tests.

**Navigator.** `UNavalNavigatorComponent` sits on the ship and ties the two halves together.
`RequestMoveTo(Goal)` plans through `USeaGridSubsystem` **with this ship's own power** (so the
relative-threat model applies per ship — a flagship sails through what a weak escort routes
around), then a per-tick helmsman drives the `ASailingShipPawn`. State machine
`Idle → Planning → Following → Arrived`, an `OnArrived` delegate (Slice 4's replanning will hang
off it), and a `naval.Nav.Debug` overlay. With no sea grid in the world it falls back to a
straight line, so it still works in a bare test level.

**Demo.** `ANavalNavDemoGameMode` builds the whole thing from an empty level — grid, wind, a
scatter of danger zones and a small fleet of navigator-driven ships, one given high power to show
relative threat — and, left to wander, keeps handing them fresh goals. It is set as the project's
`GlobalDefaultGameMode`, so **New Empty Level → Play** is the entire demo, no `.umap` to author.
`ANavalNavDemoPlayerController` drives it with the cursor: left-click on the water orders the
selected ship there, `Tab` cycles which ship you command and watch, `1`/`2`/`3` toggle the
navigator / ship / grid overlays. Ships now also carry a **visible default hull** (a scaled engine
cone), so they show up with no imported assets.

**What is tunable** (`FHelmsmanParams`, on the navigator or a test): `LookAheadBase`,
`LookAheadPerSpeed`, `LookAheadMin`/`Max`; `SteerP`, `SteerD`, `BearingDeadbandDeg`;
`TurnInLeadScale`; `WaypointAcceptRadius`, `ArrivalRadius`, `SlowdownRadius`;
`bEaseTrimInTurns`/`TurnTrimEaseDeg`/`MinTurnTrim`; `TackMarginDeg`, `TackHysteresisDeg`.

**Known simplification (deliberate).** The tacker is a single-decision affair: when the wanted
course is upwind it holds one close-hauled edge and bears away once the bearing clears the no-go.
It does **not** beat to windward with laylines and alternating boards, so a goal placed *directly*
upwind is sailed as one long tack plus a bear-away rather than a proper zig-zag. That is enough to
make the wind matter and keep the helmsman readable; full VMG beating is out of scope for the
sample. Everything else (look-ahead, turn-in, waypoint advance, arrival) is exercised by the
closed-loop test that sails a zigzag to its goal over 10k ticks.

## Did it compile?

Slices 2 and 3 **compile and their tests pass** on Windows (UE 5.5, VS 2022 / MSVC 14.44); the
table below is the Slice 1 verification, done before an engine was available. What *was* verified
then:

| Check | Result |
| --- | --- |
| Engine-free core compiled with clang++ (C++20, `-Wall -Wextra`) | **pass**, no warnings |
| Standalone test suite (`Tools/AlgoSelfTest/run_tests.sh`) | **pass — 643 checks, 0 failures** |
| Same suite under AddressSanitizer + UndefinedBehaviorSanitizer | **pass**, no diagnostics |
| UHT convention lint (`.generated.h` placement, `GENERATED_BODY`, `NAVALNAV_API`, include resolution) | **pass** |
| Unreal Build Tool (`NavalNavSampleEditor Win64 Development`) | **not run** |

`Tools/AlgoSelfTest` compiles `SeaGridTypes.cpp`, `SeaGridPathfinder.cpp` and
`ThreatEvaluator.cpp` against a minimal shim of Unreal's types (`TArray`, `FVector`, `FMath`,
`FIntPoint`, the reflection macros as no-ops). It is a development aid, never part of the
`NavalNav` module — but it means the algorithm itself is verified rather than merely written,
and it runs in CI on a box with no engine installed.

Covered scenarios (identical in the automation tests and the standalone harness):

1. grid geometry round-trips world ↔ cell, and the cell-count clamp holds
2. straight route across open water collapses to two waypoints and costs exactly 15 for
   15 steps; diagonals are charged √2, not 2
3. route bends around a threat blob, never enters the lethal core, and costs more than the
   straight route — while a ship that outguns the zone still goes straight through
4. no route when the sea is walled off; one gate is enough; endpoints inside land fail rather
   than silently snapping somewhere reachable
5. diagonal steps do not squeeze between two touching obstacles
6. string pulling removes waypoints without cutting through danger
7. threat falls off strictly with distance, stacks when zones overlap, and hostility respects
   the power threshold
8. 100 repeated queries on reused buffers return bit-identical results, and a differently
   sized grid does not trip over the previous buffers

The standalone harness additionally runs a **reference Dijkstra** over the same cost model and
asserts A\* returns exactly the optimal cost on 12 randomised cost fields with threat blobs.

Slices 2 and 3 added the five sailing and five helmsman scenarios to that same harness
(`Ship/SailingModel.cpp` and `Navigation/PredictiveHelmsman.cpp` are now in its compile list, and
the shim's `FMath` grew `Atan2`/`Cos`/`Tan`/degree conversions to feed them). They were **not**
re-run standalone on the Windows box — it has no POSIX C++ compiler — but the identical cases pass
as UE automation tests, so the standalone count above is the Slice 1 figure, not the current one.

One real bug was caught by the tests while writing them: a zone configured with no lethal core
made the single cell at its exact centre impassable (`0 <= 0`), quietly punching a hole through
an otherwise avoidable zone.

## What needs attention

1. **Run the demo.** With the demo GameMode set as `GlobalDefaultGameMode`: open the editor, make
   a **New Empty Level** (or use any level without its own GameMode override) and press **Play**.
   The fleet, wind and danger zones spawn from code. Left-click the water to send the selected ship
   there, `Tab` to cycle ships, `1`/`2`/`3` to toggle the navigator / ship / grid overlays. Headless
   smoke test confirmed the GameMode spawns and plans without warnings, but the *feel* is best
   judged in a viewport.
2. **No `.umap` yet.** The demo is spawned from code rather than placed in a level. A hand-authored
   map with terrain/water and pretty meshes is a Slice 4 nicety; it is not needed to see the system
   work.
3. **The tacker does not beat to windward.** A goal placed directly upwind is sailed as one tack
   plus a bear-away, not a laylined zig-zag (see the Slice 3 simplification note). Fine for the
   sample; worth knowing before pointing a ship straight into the wind and expecting a beat.
4. **`EngineAssociation` is `"5.5"`.** Change it if the target machine has a different version.

## Roadmap

- **Slice 2 — sailing ship pawn + wind subsystem. _Done._** A kinematic hull driven by a polar
  curve, a `UWindSubsystem` with direction/strength/drift, two-number helm-and-trim input. The
  interesting constraint is delivered: a sailing ship cannot make ground dead upwind and cannot
  turn without way on, which is what makes the follower in Slice 3 non-trivial.
- **Slice 3 — predictive helmsman follower. _Done._** Consumes `FNavalPath` and drives the pawn:
  speed-scaled look-ahead, turn-in prediction from the sailing model's turning circle, no-go-zone
  awareness with a simple tacker, and a navigator component wiring planner to helmsman. A code-only
  demo spawns the whole thing from an empty map.
- **Slice 4 — replanning triggers + demo map.** Replan on threat-layer change, on drifting off
  the plan, and on a wind shift that makes the current legs unsailable; hysteresis so a route
  does not flip every frame. An Escaping state on the navigator for when a ship is boxed in, and
  a hand-authored demo level tying the slices together. `OnArrived` and the per-ship cost hook are
  already in place for it.
- **Later, if worth it:** cost-field flow fields for squadrons, hierarchical planning for
  larger seas, and moving the search off the game thread.
