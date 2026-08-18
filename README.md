# NavalNavSample

Threat-aware naval navigation for physically-driven sailing ships — a clean-room UE5 C++ sample.

A world-space sea grid carries a mutable *threat cost layer* fed by danger zones, and an A\*
pathfinder plans routes that trade distance against danger instead of only against length. A
ship that outguns a zone sails straight through it; a weaker one goes around.

- **Engine:** Unreal Engine 5.5 · single `NavalNav` C++ runtime module · **License:** MIT
- **Status:** Slice 4 — feature-complete (replanning, escape, dynamic zones, demo scenarios) — see **[docs/STATUS.md](docs/STATUS.md)**

> Written from scratch as a portfolio piece. It builds with UE 5.5 + Visual Studio 2022 and all
> 30 automation tests pass in-engine; the navigation, sailing, helmsman and replanning cores are
> also verified by an engine-free harness that runs with a plain compiler. `docs/STATUS.md` has the
> details.

## The idea

Ordinary grid pathfinding asks "what is the shortest way there?". A warship asks "what is the
way there that I survive?" — and the answer depends on who is asking. Threat here is a
continuous cost layer rather than a set of blocking volumes, so routes *slide* around danger
as it moves instead of flipping between "straight through" and "all the way around".

```
Source/NavalNav/
  Grid/SeaGridTypes.*        Grid geometry + base and threat cost layers (no UObject: unit-testable)
  Grid/SeaGrid.*             USeaGridSubsystem — one grid per world, lazy threat rebuild
  Grid/SeaGridPathfinder.*   8-neighbour A*, octile heuristic, injectable cost, cost-aware string pull
  Grid/SeaGridDebugDraw.*    Throttled immediate-mode overlay for the cost field and routes
  Threat/DangerZone.*        ADangerZone — a footprint (position, radius, power) and nothing more
  Threat/ThreatEvaluator.*   Pure tuning rules: relative hostility, distance falloff
  Ship/SailingModel.*        Wind polar + drag + rudder, engine-free and unit-tested
  Ship/WindSubsystem.*       UWindSubsystem — one wind per world, drift and console overrides
  Ship/SailingShipPawn.*     ASailingShipPawn — kinematic hull wrapping the sailing model
  Ship/ShipPowerComponent.*  Combat power + FOnPowerChanged, for relative-threat replanning
  Navigation/PredictiveHelmsman.*  Look-ahead + turn-in follower, engine-free and unit-tested
  Navigation/ReplanPolicy.*        When a route is stale (5 triggers + hysteresis), engine-free
  Navigation/NavalNavigatorComponent.*  Planner + helmsman + replanning + escape state machine
  Demo/NavalNavDemoGameMode.*  Spawns the whole demo (sea, sky, fleet, zones) and five scenarios
  Demo/NavalNavDemoPlayerController.*  Click-to-move, Tab cycle, 1/2/3 overlays, 5-9 scenarios, P
  Debug/NavalNavDebugActor.* Drop-in-a-level harness with two draggable endpoints
  Tests/                     Automation tests
Tools/AlgoSelfTest/          The same tests without an engine (development aid)
```

A few decisions worth pointing at:

- **Cost comes from an injected functor**, not from the grid, so a search can run against
  ground truth or against what one particular ship believes it knows.
- **String pulling is cost-aware.** A naive line-of-sight smoothing pass cuts straight back
  through the danger the search just spent cost avoiding; this one only shortcuts when the
  straight line is not meaningfully more expensive than the route it replaces.
- **The search reuses its buffers** and marks visited cells with a per-query generation stamp,
  so replanning every few seconds costs no heap traffic and no 40k-entry clear.
- **The threat layer is cleared and re-stamped, not incrementally edited.** Zones overlap and
  stack, so subtracting a moving zone's old footprint would need per-zone bookkeeping to stay
  exact. Re-stamping is inherently correct and happens at most once per frame, lazily.

## Sailing model

The ship is a sailing ship, not a motorboat, and the difference is one curve. Forward drive is a
**polar function of the angle between the bow and the wind**: flatly zero in a no-go zone dead
upwind (so the ship must tack, not point, to make ground to windward), rising to a peak on a beam
reach, and easing off again running downwind. Speed then chases a quadratic-drag terminal that is
pinned to `MaxSpeed` on the best point of sail, and the rudder only bites once the ship has way on
— together, exactly the constraints that make the Slice 3 follower have to *think* about the wind
rather than drive straight lines. The math lives in a plain `FSailingModel` struct so it is
unit-tested without a world, the same way the pathfinder is. (A stationary ship keeps a small
at-rest yaw authority so it can turn out of the no-go cone instead of getting stuck head-to-wind;
the helmsman bears away to a driving point of sail to recover.)

Wind-aware *planning* — routing around legs that would be dead upwind — is deliberate future work.
The planner stays physics-agnostic: A\* knows nothing about the wind, and the helmsman reconciles
its geometric route with what the hull can actually sail.

## Predictive helmsman

The design rule the whole repo is built around: **the planner is physics-agnostic and the helmsman
owns the physics.** A\* hands back a route that knows nothing about wind or turning circles — it is
*intent*, not a rail — and the helmsman reconciles it with a hull that cannot point upwind or corner
instantly. It steers at a **look-ahead point** that slides further along the path the faster the
ship goes, and it starts each turn *before* the corner: from the sailing model's turning circle it
computes a **turn-in distance** (`R·tan(θ/2)`, so it grows with speed and corner sharpness) and
blends onto the next leg early, carving the corner instead of overshooting. When the course it
wants is dead upwind it tacks to the nearest sailable edge. Like everything else here, the helmsman
is a plain struct — a closed loop of it plus `FSailingModel` sails a zigzag to its goal in a unit
test with no world.

## Replanning triggers

A route is *intent*, and intent goes stale. `FReplanPolicy` — another plain, engine-free struct —
decides *when* to replan (the pathfinder still decides *how*). Five triggers, each toggleable: the
ship drifts **off-route** past a grace time; a cell on the remaining path becomes **blocking for
this ship's power** (a zone moved onto it); a **relevant zone changed** near the route (event-driven,
not polled); this ship's **own power changed**; or a cheap **periodic** backstop. A minimum replan
interval is the storm-guard — a hundred zone jitters in a window collapse to about ten replans, not
a hundred — and the navigator replans **without stopping**: the new route is planned from the ship's
current position and spliced in, so there is no snap back to a start waypoint, and a still-valid
route is only replaced for a meaningfully cheaper one.

## Escape behavior

When a ship finds itself in hostile water — a zone drifted over it, or its power dropped — it breaks
off (`Following → Escaping`). A bounded outward ring search, rated for that ship's own power, finds
the **nearest open-water cell**; if the ship is fully boxed in, it takes the **least-bad exit** —
the cell through the weakest part of the enclosure — then resumes its original goal the moment it is
clear. The search is a shared grid utility (`FSeaGridPathfinder::FindNearestCellBelowCost`), so it
is unit-tested on a synthetic cost field with no world.

## Trying it

**The demo.** Open the project in UE 5.5, make a **New Empty Level**, and press **Play** — the demo
GameMode spawns the sea, sky, a fleet, the wind and danger zones from code. Pick a scenario with the
number keys:

| Key | Scenario |
| --- | --- |
| `5` | Baseline — static zones, ships wander between goals |
| `6` | Moving zone — a patrol slides across a route → mid-voyage replan |
| `7` | Power contrast — weak (blue) vs strong (gold), same start/goal → different routes |
| `8` | Enclosure — a ship ringed by zones with one weak gap → escapes through it |
| `9` | Power drop — a strong ship crossing a zone; press `O`/`P` to change its power → it re-solves |

Left-click the water to move the selected ship (a player order — it stops wandering and its hull
brightens), `Tab` cycles ships, `1`/`2`/`3` toggle the navigator / ship / grid overlays, the
**arrow keys** steer the wind (`Left`/`Right` direction, `Up`/`Down` strength), **`O`/`P`**
strengthen / weaken the selected ship, and the mouse-wheel zooms. A HUD shows the scenario, the live
wind, the selected ship's power and the key map.

**The planner on its own.** Place an `ANavalNavDebugActor` and a few `ADangerZone`s in a level and
drag them around: the actor ticks in the editor viewport, so the route bends without entering PIE.

Without an engine installed, the navigation, sailing, helmsman and replanning cores still run:

```bash
./Tools/AlgoSelfTest/run_tests.sh     # A*-vs-Dijkstra optimality, the sailing model, a
                                      # closed-loop helmsman, and the replan/escape logic
```

## Scope / non-goals

Deliberate boundaries, so the sample stays a readable illustration of one idea rather than a
half-finished game:

- **Kinematic hull, not a buoyancy sim.** The ship is `FSailingModel` integrated on the XY plane and
  written to the transform — no PhysX/Chaos, no waves, no heel. Readable and deterministic on purpose.
- **No full VMG beating to windward.** The tacker holds one close-hauled edge and bears away; it does
  not lay a proper zig-zag with laylines.
- **Replanning is A\* from scratch** (with buffer reuse), not incremental D\*-Lite. Correct and simple
  at the demo's replan rate.
- **Single-threaded, single-player, no networking.** The search runs on the game thread; there is no
  replication.

Each of these is called out with its reasoning in [docs/STATUS.md](docs/STATUS.md); none is a
missing feature so much as a chosen edge.
