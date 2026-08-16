# Project status

_Last updated: 2026-08-16 — end of Slice 1._

## Environment

The work was carried out in a **Linux container** (Ubuntu 24.04, x86-64), not on the Windows
workstation the task originally assumed. That has one consequence worth stating plainly at the
top: **the project has not been compiled by Unreal Build Tool.** Everything below is written
against the UE 5.5 API but has not been through UHT or a C++ compiler with engine headers.

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

## Did it compile?

**Not as an Unreal module — no engine, no toolchain on this machine.** What *was* verified:

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

One real bug was caught by the tests while writing them: a zone configured with no lethal core
made the single cell at its exact centre impassable (`0 <= 0`), quietly punching a hole through
an otherwise avoidable zone.

## What needs attention

1. **Build the project.** On a Windows box with UE 5.5 + VS 2022:
   `Engine\Build\BatchFiles\Build.bat NavalNavSampleEditor Win64 Development -Project="…\NavalNavSample.uproject" -WaitMutex`.
   Expect the usual first-compile friction (a missing include, a UHT specifier) — the code has
   never seen UHT. Header hygiene was linted, but that is not the same as compiling.
2. **Run the automation tests in-engine** to confirm they match the standalone results:
   `UnrealEditor-Cmd.exe NavalNavSample.uproject -ExecCmds="Automation RunTests NavalNav" -unattended -nopause -testexit="Automation Test Queue Empty"`.
3. **No demo map yet.** `GameDefaultMap` points at an engine template map. The demo level with
   placed `ADangerZone`s and an `ANavalNavDebugActor` arrives with Slice 4; a `.umap` cannot be
   authored without the editor.
4. **`EngineAssociation` is `"5.5"`.** Change it if the target machine has a different version.

## Roadmap

- **Slice 2 — sailing ship pawn + wind subsystem.** Physically driven hull (buoyancy,
  drag, rudder), a `UWindSubsystem` with direction/strength and local gusts, sail trim mapped
  through Enhanced Input. The interesting constraint: a sailing ship cannot hold an arbitrary
  heading, which is what makes the follower in Slice 3 non-trivial.
- **Slice 3 — predictive helmsman follower.** Consumes `FNavalPath` and drives the pawn:
  look-ahead proportional to speed and turning radius, no-go-zone awareness against the wind,
  tacking when the next leg is upwind. `FNavalPath::Costs` is already there for it to decide
  where burning speed is worth it.
- **Slice 4 — replanning triggers + demo map.** Replan on threat-layer change, on drifting off
  the plan, and on a wind shift that makes the current legs unsailable; hysteresis so a route
  does not flip every frame. Plus the demo level tying the slices together.
- **Later, if worth it:** cost-field flow fields for squadrons, hierarchical planning for
  larger seas, and moving the search off the game thread.
