# Project status

_Last updated: 2026-08-19 — Slice 4 + polish passes 1–3. **Feature-complete; frozen — fixes only.**_

> **Now building on Windows.** Slices 1–4 (plus polish passes 1–3) compile with UE 5.5 + Visual
> Studio 2022 (MSVC 14.44) via Unreal Build Tool, and all **30 automation tests pass in-engine**
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

## What is implemented — Slice 4: replanning, escape, dynamic zones, demo scenarios

```
Source/NavalNav/
  Navigation/ReplanPolicy.{h,cpp}          FReplanPolicy, FReplanPolicyParams, EReplanReason
  Navigation/NavalNavigatorComponent.{h,cpp}  + replanning, Escaping state, event subscriptions
  Grid/SeaGrid.{h,cpp}                      + OnThreatChanged, GetObserverCellCost, FindEscapeTarget
  Grid/SeaGridPathfinder.{h,cpp}            + FindNearestCellBelowCost (engine-free ring search)
  Threat/DangerZone.{h,cpp}                 + Static/Patrol/Orbit movement, change events, disc visual
  Demo/*                                    + self-sufficient environment, five scenarios, controls
  Tests/ReplanPolicyTest.cpp                8 automation tests
```

**Demo presentation (the play-test fix).** Before Slice 4 proper, the demo was a black void: no
lighting, no water, invisible ships, and the grid overlay drew nothing because it depended on a
placed debug actor. The GameMode now spawns a sun + `SkyAtmosphere` + `SkyLight` and a large blue
sea plane, ships carry a lit per-ship colour (flagship gold, escorts grey-blue), danger zones show
a coloured disc (amber → red by power), and the GameMode draws the grid overlay itself. So **New
Empty Level → Play** is watchable with no imported assets.

**Replanning.** `FReplanPolicy` is a plain, engine-free struct that decides *when* a route is
stale — the pathfinder still owns *how* to route. Five triggers, each individually toggleable and
tunable:

- **Off-route** — cross-track distance beyond `OffRouteDistance` for longer than `OffRouteGraceSeconds`;
- **Path blocked** — a cell on the remaining path is now blocking *for this ship's power*, scanned
  at `PathCheckInterval` through an injected cost sampler (never per-frame A\*);
- **Threat changed** — a zone moved or changed power within `RelevanceRadius` of the ship or its
  remaining route (event-driven, from the grid's `OnThreatChanged`);
- **Power changed** — this ship's own `UShipPowerComponent` fired `OnPowerChanged`;
- **Periodic** — a cheap backstop once the path is older than `MaxPathAge`.

`MinReplanInterval` is the storm-guard: many triggers in a window cost one A\* (a 100-event jitter
storm collapses to ~10 replans in the tests). The navigator replans **without stopping** — the new
route is planned from the ship's current position and spliced in, so the helmsman's fresh waypoint
1 is already ahead of the bow (no reset-to-start jerk) — and a still-valid route is only replaced
for one cheaper than `old × ImprovementCostRatio`.

**Escape.** When the observer cost at the ship's own cell rises past `EscapeCostThreshold` (a zone
drifted onto it, or its power dropped), the navigator goes `Following → Escaping`: a bounded
outward ring search (`FSeaGridPathfinder::FindNearestCellBelowCost`, rated for this ship's power)
finds the nearest open-water cell within `EscapeSearchRadius`, or — if the ship is fully boxed in —
the **least-bad passable cell**, i.e. the exit through the weakest part of the enclosure. It heads
there and resumes its original goal the moment it is back in open water. An `OnStateChanged`
delegate reports every transition.

**Dynamic zones.** `ADangerZone` gained `Static` / `Patrol` / `Orbit` movement and now routes its
move/power changes through `USeaGridSubsystem::NotifyZoneChanged`, which both marks the threat
layer dirty (re-stamped at most once per frame, as before) and broadcasts `OnThreatChanged` so
navigators can react. A per-observer `GetThreatCostAt` lets a ship rate a zone for its own power
without disturbing the shared stamped layer.

**Demo scenarios.** Number keys **5–9** each reset the world deterministically (seeded):

| Key | Scenario | What to watch |
| --- | --- | --- |
| `5` | Baseline | static zones, ships wander between random goals |
| `6` | Moving zone | a patrolling zone slides across a route → a mid-voyage replan |
| `7` | Power contrast | weak (crimson) and strong (gold) ship, same start/goal → different routes |
| `8` | Enclosure | a ship ringed by zones with one weak gap → escapes through it |
| `9` | Power drop | a strong ship crossing a zone; press `P` to weaken it → it re-solves around |

Controls: left-click to move the selected ship (a player order — it stops wandering and its hull
brightens), `Tab` to cycle ships, `1`/`2`/`3` to toggle the navigator / ship / grid overlays,
**arrow keys** to steer the wind (`Left`/`Right` direction, `Up`/`Down` strength), **`O`/`P`** to
strengthen / weaken the selected ship (its route re-solves as it crosses the hostility bands),
mouse-wheel to zoom. The HUD shows the scenario, the live wind (with a compass arrow), the selected
ship's power and the key map.

**What is tunable** (`FReplanPolicyParams` on the navigator): the five trigger toggles;
`OffRouteDistance`, `OffRouteGraceSeconds`; `BlockedCostThreshold`, `PathBlockSampleSpacing`,
`PathCheckInterval`; `RelevanceRadius`; `MinReplanInterval`, `MaxPathAge`, `ImprovementCostRatio`.
On the navigator: `EscapeCostThreshold`, `EscapeSearchRadius`. On a zone: movement pattern,
amplitude/period/orbit radius.

**Known simplifications (deliberate).** Replanning is A\* from scratch (with buffer reuse), not an
incremental repair like D\*-Lite — correct and simple, and cheap enough at the demo's replan rate.
Escape picks a single target cell and plans to it; it does not continuously re-evaluate the exit
mid-run beyond the ordinary Following triggers once it is clear. Escape re-entry is guarded only by
`EscapeCostThreshold` (no explicit cooldown), which is fine because A\* routes the resumed leg
around the zone; a pathologically moving zone could in principle re-trigger it.

## Polish pass 1 (play-test fixes)

A play-test after Slice 4 turned up presentation and behaviour bugs; none changed the feature set,
they made it work as intended. 27 automation tests pass (Slice 4's 26 plus a new no-orbit test).

**Presentation.**
- **Grey ships → per-ship colour.** The hull tint silently no-op'd: the dynamic material was built
  from the cone's *default* material, which has no `Color` parameter. Ships are now explicitly based
  on `BasicShapeMaterial` (verified to expose `Color`), and the player-selected ship is brightened.
- **Zone discs → banded glow + label.** Discs use an additive, two-sided emissive material (a
  see-through glow, not an opaque coin), coloured by a four-band power ramp (green/yellow/orange/red),
  and every zone carries an always-on rim ring and a `P n` power number.
- **Sea and camera.** The sea plane now covers ~10× the field so its edge is never in shot, and
  zoom-out is clamped to keep it that way.
- **Grid overlay flicker.** Two causes fixed: the overlay is stamped for a fixed `OverlayObserverPower`
  each frame (so the drawn threat no longer flips as ships of different power replan), and it is drawn
  with a lifetime slightly longer than a frame (so a single-frame gap in the debug-line batch is
  covered by the previous frame).
- **HUD.** Scenario title, live wind (with a compass arrow) and the key map are drawn to a Canvas HUD
  in a large font on a translucent box, replacing the tiny on-screen debug lines.

**Behaviour.**
- **Ships orbiting the goal.** Arrival slowdown eased the sheets so far the ship lost the steerage
  way it needs to turn (zero yaw at rest), so it circled the goal. Fix: a `MinSteerageTrim` floor
  while following, plus recognising that a goal *inside* the ship's turning circle is reached once
  within `ArrivalTurnRadiusFactor` × turn radius (or once it slips behind). Covered by a new test.
- **Player orders hijacked by wander.** A left-click is now a *player order*: the ship stops
  wandering, and a click while Escaping sets the goal to resume once clear rather than steering back
  into danger.
- **Wind had no visible effect.** The console cvars did work, but nothing showed it; arrow keys now
  steer the wind live (clearing any stale cvar override), the HUD shows it, and changes are logged.
- **Replan counter climbed on static routes.** It counted every policy firing, including periodic
  re-validations that change nothing. Split into an honest `validations N / replans M` — a replan is
  only counted when a new path is actually adopted — and `MaxPathAge` raised to 30 s.

**New tunables.** Helmsman: `MinSteerageTrim`, `ArrivalBehindFactor`, and orbit handling (see the
polish 2 note — the instant turning-circle arrival that first shipped here was replaced by
`OrbitGiveUpTurnDeg` when it turned out to eat fresh move orders near the ship).
Demo GameMode: `OverlayObserverPower`, `SeaColor`. Pawn: `HullColor`. Wind: `AddWindYaw` /
`AddWindStrength`. **New keys:** arrow keys steer the wind.

## Polish pass 2 (play-test fixes)

A second play-test. 30 automation tests pass (27 plus new in-irons recovery, a random-goal/wind
soak, and a fresh-order-near-the-ship manoeuvre test).

- **Left-click stopped issuing move orders** (regression from polish 1's orbit fix). The instant
  "arrive once inside the turning circle" rule also fired on a *fresh* order, so clicking near or
  beside a fast ship was read as an immediate arrival and nothing happened. Replaced with orbit
  *detection*: the ship gives up on a goal only after it has demonstrably circled it
  (`OrbitGiveUpTurnDeg`), so a fresh order always manoeuvres first.
- **Ships stalled head-to-wind ("in irons").** At zero speed the steering authority was zero, so a
  bow pointed into the no-go cone could never turn out and stayed stalled forever. The model gains a
  small at-rest yaw floor (`MinYawAuthorityAtRest`) — turning only, never drive — and the helmsman
  recovers by bearing away past the no-go to a point of sail that makes drive (`IronsBearAwayDeg` /
  `IronsSpeedRatio`) until the ship has way on, then pointing back up. It never commands a heading
  inside the cone. (Wind-aware *planning* — routing around upwind legs — stays deliberate future
  work; the planner is physics-agnostic.)
- **Overlays 1 and 2 overlapped and differed in size.** The navigator overlay is offset to the
  ship's starboard and the ship overlay to port (screen-right / screen-left under the yaw-inheriting
  chase cam), both at the same font scale.
- **Ship power was invisible and `P` seemed to do nothing.** The navigator overlay and the HUD now
  show power (and "hostile: yes/no"); `O`/`P` step it up/down (clamped, logged). The real cause of
  "nothing re-routed" was that a replan compared the fresh plan against the current path's *stale*
  cost; it now re-costs the current path for the ship's current power, so a route gone hostile reads
  as expensive and the safe reroute wins.
- **Wind indicator** is now a proper arrow (big head at the downwind end, "W" label) instead of a
  line, in the ship overlay and the HUD compass.

**New tunables.** Model: `MinYawAuthorityAtRest`. Helmsman: `IronsSpeedRatio`, `IronsBearAwayDeg`,
`OrbitGiveUpTurnDeg`. **New keys:** `O` / `P` strengthen / weaken the selected ship.

## Polish pass 3 (presentation)

Purely visual, from a play-test; no gameplay or test changes (still 30 green). I could not judge
the look headless — these were verified to compile, run and not crash, but the *appearance* is the
user's call.

- **Bigger, saturated hulls.** The hull is scaled to ~400 uu (about two grid cells) so it reads at
  default zoom, on a matte material (roughness 1) so the strong sun does not wash the colour to
  white. Fleet colours are high-contrast against the blue sea: flagship gold-orange, escorts
  crimson. The selected ship keeps its colour and gets a bright cyan ring on the water.
- **The navigable area is obvious.** A persistent bright, thin cyan frame is drawn around the grid;
  the navigable water is a matte mid blue, the sea beyond it a huge, darker, desaturated navy plane
  (so the horizon is never a black edge and the outside clearly reads as "not playable"); and a
  click outside the grid is rejected with a centred "Outside navigable area" message instead of
  sending a ship into blank sea.
- **Motion reference.** A sparse dark-blue lattice (every ~5 grid cells) over the navigable water —
  a nautical-chart look, no grey grid material — plus a ring of dark islets just outside the grid
  and a short fading wake behind every ship.
- **Overlay text moved to the HUD edges.** Overlay 1 (navigator) is pinned to the right screen edge
  and overlay 2 (ship/wind) to the left, on translucent panels, for the *selected ship only*, shown
  by keys 1 / 2. The per-ship world-space text is gone; the world markers (route, look-ahead,
  turn-in, wind/heading arrows) stay. The top-right wind compass (which did not rotate with the
  camera) was removed — wind reads from the HUD text and the world wind arrow.
- **Less clutter.** The ship overlay and the navigator's look-ahead/turn-in markers draw for the
  selected ship only; routes still draw for every ship. `naval.Nav.DebugAllShips 1` restores the
  markers for all ships.

**New cvar:** `naval.Nav.DebugAllShips` (0 = markers on the selected ship only, default; 1 = all).

## Did it compile?

Slices 2–4 **compile and their tests pass** on Windows (UE 5.5, VS 2022 / MSVC 14.44); the table
below is the Slice 1 verification, done before an engine was available. What *was* verified then:

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

Slices 2–4 added the sailing, helmsman and replanning/escape scenarios to that same harness
(`SailingModel.cpp`, `PredictiveHelmsman.cpp` and `ReplanPolicy.cpp` are now in its compile list,
and the shim's `FMath` grew `Atan2`/`Cos`/`Tan`/degree conversions to feed them). They were **not**
re-run standalone on the Windows box — it has no POSIX C++ compiler — but the identical cases pass
as UE automation tests, so the standalone count above is the Slice 1 figure, not the current one.

One real bug was caught by the tests while writing them: a zone configured with no lethal core
made the single cell at its exact centre impassable (`0 <= 0`), quietly punching a hole through
an otherwise avoidable zone.

## What needs attention

1. **Run the demo.** With the demo GameMode set as `GlobalDefaultGameMode`: open the editor, make
   a **New Empty Level** (or any level without its own GameMode override) and press **Play**. Use
   keys **5–9** to pick a scenario (see the table above), left-click to move the selected ship,
   `Tab` to cycle ships, `1`/`2`/`3` for the overlays, arrow keys for the wind, `P` for the
   power-drop scenario. Headless smoke tests confirm the GameMode spawns, plans and replans without
   warnings, but the *feel* is best judged in a viewport.
2. **No `.umap`.** The whole demo is spawned from code, deliberately — nothing to author, nothing to
   break on clone. A hand-authored map with nicer meshes would be pure polish.
3. **The tacker does not beat to windward** (Slice 3), and **replanning is A\* from scratch, not
   incremental** (Slice 4). Both are deliberate; see the per-slice simplification notes.
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
- **Slice 4 — replanning, escape, dynamic zones, demo scenarios. _Done._** An engine-free
  `FReplanPolicy` (off-route / path-blocked / threat-changed / power-changed / periodic, with
  hysteresis), replan-without-stopping, a `Following → Escaping` state that breaks a boxed-in ship
  out through the weakest gap, moving zones, and five seeded demo scenarios. **This is the last
  feature slice: the code is now frozen and further work is polish only.**
- **Later, if worth it (out of scope for this sample):** incremental replanning (D\*-Lite) instead
  of A\*-from-scratch, cost-field flow fields for squadrons, hierarchical planning for larger seas,
  moving the search off the game thread, and full VMG beating to windward.
