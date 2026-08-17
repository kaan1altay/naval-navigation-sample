# NavalNavSample

Threat-aware naval navigation for physically-driven sailing ships — a clean-room UE5 C++ sample.

A world-space sea grid carries a mutable *threat cost layer* fed by danger zones, and an A\*
pathfinder plans routes that trade distance against danger instead of only against length. A
ship that outguns a zone sails straight through it; a weaker one goes around.

- **Engine:** Unreal Engine 5.5 · single `NavalNav` C++ runtime module · **License:** MIT
- **Status:** Slice 3 (adds a predictive helmsman + a playable demo) — see **[docs/STATUS.md](docs/STATUS.md)**

> Written from scratch as a portfolio piece. It builds with UE 5.5 + Visual Studio 2022 and all
> 18 automation tests pass in-engine; the navigation, sailing and helmsman cores are also verified
> by an engine-free harness that runs with a plain compiler. `docs/STATUS.md` has the details.

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
  Navigation/NavalNavigatorComponent.*  Ties planner to helmsman; Idle/Planning/Following/Arrived
  Demo/NavalNavDemoGameMode.*  Spawns the whole demo from an empty map (fleet, wind, zones)
  Demo/NavalNavDemoPlayerController.*  Click-to-move, Tab to cycle ships, 1/2/3 debug toggles
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
unit-tested without a world, the same way the pathfinder is.

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

## Trying it

**The demo.** Open the project in UE 5.5, make a **New Empty Level**, and press **Play** — the demo
GameMode spawns a fleet, the wind, and a scatter of danger zones from code (one ship is given high
power, so watch it sail straight through zones the others route around). Left-click the water to
send the selected ship there, `Tab` cycles which ship you command, and `1`/`2`/`3` toggle the
navigator / ship / grid overlays.

**The planner on its own.** Place an `ANavalNavDebugActor` and a few `ADangerZone`s in a level and
drag them around: the actor ticks in the editor viewport, so the route bends without entering PIE.
Console: `naval.DrawGrid`, `naval.DrawCosts`, `naval.DrawPath`, `naval.Nav.Debug`, `naval.Ship.Debug`.

Drop an `ASailingShipPawn` too, set `naval.Ship.Debug 1`, and sail it — possess it with an
`ASailingShipPlayerController` (A/D rudder, W/S trim) or call `SetRudderInput`/`SetSailTrim`. It
stalls head-to-wind, accelerates onto a reach, and turns only with way on. Point the wind with
`naval.Wind.Yaw` and `naval.Wind.Strength`.

Without an engine installed, the navigation, sailing and helmsman cores still run:

```bash
./Tools/AlgoSelfTest/run_tests.sh     # A*-vs-Dijkstra optimality, the sailing model, and a
                                      # closed-loop helmsman that sails a zigzag to its goal
```

## Roadmap

Slice 4 — replanning triggers (threat change, drift, wind shift) with hysteresis, an Escaping state
for boxed-in ships, and a hand-authored demo level. Details in [docs/STATUS.md](docs/STATUS.md).
