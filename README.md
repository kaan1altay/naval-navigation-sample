# NavalNavSample

Threat-aware naval navigation for physically-driven sailing ships — a clean-room UE5 C++ sample.

A world-space sea grid carries a mutable *threat cost layer* fed by danger zones, and an A\*
pathfinder plans routes that trade distance against danger instead of only against length.

> Work in progress. See [`docs/STATUS.md`](docs/STATUS.md) for what is implemented, what
> compiles, and what comes next.

- **Engine:** Unreal Engine 5.5 (C++, single `NavalNav` runtime module)
- **License:** MIT
