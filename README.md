# NavalNavSample

Threat-aware naval navigation for physically-driven sailing ships — a clean-room UE5 C++ sample.

A world-space sea grid carries a mutable *threat cost layer* fed by danger zones, and an A\*
pathfinder plans routes that trade distance against danger instead of only against length. A
ship that outguns a zone sails straight through it; a weaker one goes around.

- **Engine:** Unreal Engine 5.5 · single `NavalNav` C++ runtime module · **License:** MIT
- **Status:** Slice 2 (adds a sailing ship + wind) — see **[docs/STATUS.md](docs/STATUS.md)**

> Written from scratch as a portfolio piece. It builds with UE 5.5 + Visual Studio 2022 and all
> 13 automation tests pass in-engine; the navigation and sailing cores are also verified by an
> engine-free harness that runs with a plain compiler. `docs/STATUS.md` has the details.

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

## Trying it

Open the project in UE 5.5, place an `ANavalNavDebugActor` and a few `ADangerZone`s in a level,
and drag them around: the actor ticks in the editor viewport, so the route bends without
entering PIE. Console: `naval.DrawGrid`, `naval.DrawCosts`, `naval.DrawPath`.

Drop an `ASailingShipPawn` too, set `naval.Ship.Debug 1`, and sail it — possess it with an
`ASailingShipPlayerController` (A/D rudder, W/S trim) or call `SetRudderInput`/`SetSailTrim`. It
stalls head-to-wind, accelerates onto a reach, and turns only with way on. Point the wind with
`naval.Wind.Yaw` and `naval.Wind.Strength`.

Without an engine installed, the navigation and sailing cores still run:

```bash
./Tools/AlgoSelfTest/run_tests.sh     # incl. an A*-vs-Dijkstra optimality proof + the sailing model
```

## Roadmap

Slice 3 — predictive helmsman that tacks upwind · Slice 4 — replanning triggers + demo map.
Details in [docs/STATUS.md](docs/STATUS.md).
