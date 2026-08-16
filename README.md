# NavalNavSample

Threat-aware naval navigation for physically-driven sailing ships — a clean-room UE5 C++ sample.

A world-space sea grid carries a mutable *threat cost layer* fed by danger zones, and an A\*
pathfinder plans routes that trade distance against danger instead of only against length. A
ship that outguns a zone sails straight through it; a weaker one goes around.

- **Engine:** Unreal Engine 5.5 · single `NavalNav` C++ runtime module · **License:** MIT
- **Status:** Slice 1 (sea grid, A\*, threat layer, debug draw) — see **[docs/STATUS.md](docs/STATUS.md)**

> Written from scratch as a portfolio piece. It has been verified by tests but **not yet
> compiled by Unreal Build Tool** — it was written on a machine without an engine install.
> `docs/STATUS.md` is explicit about what that does and does not mean.

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

## Trying it

Open the project in UE 5.5, place an `ANavalNavDebugActor` and a few `ADangerZone`s in a level,
and drag them around: the actor ticks in the editor viewport, so the route bends without
entering PIE. Console: `naval.DrawGrid`, `naval.DrawCosts`, `naval.DrawPath`.

Without an engine installed, the algorithm still runs:

```bash
./Tools/AlgoSelfTest/run_tests.sh     # 643 checks, incl. an A*-vs-Dijkstra optimality proof
```

## Roadmap

Slice 2 — sailing ship pawn + wind subsystem · Slice 3 — predictive helmsman that tacks upwind
· Slice 4 — replanning triggers + demo map. Details in [docs/STATUS.md](docs/STATUS.md).
