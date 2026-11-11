# Routing

## Why the old app could not route through big cities

A plain shortest-path search (Dijkstra, or A* over SQLite) explores every
junction closer than the destination. Across a large city that is hundreds
of thousands of junctions, each a database lookup on a 2013 phone.

## What this app does instead: contraction hierarchies

On the Mac, `tzforge` ranks every junction by importance and removes them
one at a time, least important first. When a junction is removed, it adds
a **shortcut** between its neighbours wherever the path through it might be
a shortest path. The result is stored in `car.tzg`, `bike.tzg` and
`foot.tzg`.

On the phone a query searches from both ends at once and only ever moves
towards more important junctions. The two searches meet near the "top" of
the hierarchy after visiting a few hundred to a few thousand junctions,
whatever the distance. The shortcuts are then expanded back into real
road segments.

Measured on this repository's test data (desktop CPU):

| Network | Junctions | Query, average |
|---|---|---|
| Central Helsinki (real OSM) | 3,300 | 0.04 ms |
| Synthetic grid city | 90,000 | 0.07–0.4 ms |
| Synthetic grid city | 360,000 | 0.13–0.8 ms (corner to corner: 1 ms, 535 junctions visited) |

Grids are the worst case for this technique (no main roads to form a
hierarchy); real road networks do better. Building the hierarchy is the
slow part and happens on the Mac (seconds for a city, minutes for a large
country).

Correctness is checked, not assumed: `tzforge verify` rebuilds the plain
road graph from a pack and compares the contraction hierarchy against plain
Dijkstra on random pairs. It must match exactly, and it does on every test
network, including turn restrictions. `forge.sh` runs this check on every
pack it builds.

## Profiles

All rules are in `Forge/src/fz_osm.c`.

**Car** uses roads from motorway down to service roads. Access tags follow
the OSM hierarchy (`access` → `vehicle` → `motor_vehicle` → `motorcar`;
the most specific wins). It respects one-way streets (roundabouts and
motorways are implicitly one-way), `maxspeed` (including `HU:urban`-style
and mph values), and treats tracks as closed unless tagged open. Speeds are
realistic averages rather than limits (for example 90% of the limit on
motorways, 60% on residential streets). Service roads, parking aisles,
living streets and destination-only roads are made more expensive so
routes avoid cutting through them. Ferries are used only when tagged for
cars. Traffic signals add about 8 s.

**Bike** avoids motorways and motorroads, prefers cycleways, and makes
busy roads more expensive unless they have a cycle lane. Speed drops on
cobbles, gravel and bad surfaces. It respects one-way streets unless
cycling against the flow is allowed (`oneway:bicycle=no`,
`cycleway=opposite*`). Footways and pedestrian areas are allowed at
walking pace (pushing). Steps only where a ramp or `bicycle=yes` allows it.

**Foot** (the default in the app) uses everything walkable at 5 km/h
(steps slower), ignores one-way streets, and slightly prefers footways and
paths over big roads.

The routing *weight* (what is minimised) includes these preferences; the
shown *duration* is the real travel time without them.

## Turn restrictions

`no_*` and `only_*` restrictions with a via node are applied to car and
bike (unless `except` excludes them). They are modelled by splitting the
via junction: arriving over the restricted road leads to a copy of the
junction that only has the allowed exits. This is exact and keeps queries
as fast as without restrictions. When a from- or to-way passes through the
junction instead of ending there, the right branch is chosen by turn angle.
Via-way restrictions (rare) are skipped.

## Barriers

Bollards, gates marked locked or private, turnstiles, stiles, fences and
similar nodes block the profiles they apply to. Access tags on the barrier
override the defaults (a bollard with `motor_vehicle=yes` stays open).

## Snapping to the road

The start and destination snap to the nearest road usable by the profile,
within 1.5 km. Segments in small disconnected or one-way-trapped pieces of
network (outside the largest strongly connected network) are avoided when a
better road is close. If the nearest roads still cannot be connected, the
router retries with the next-nearest candidates (up to four on each side).
When the direction of travel is known (you are moving), the route starts in
that direction, which matters for reroutes on one-way and divided roads.

## Instructions

From the expanded route, `Core/tz_route.c` creates maneuvers at junctions:
turn angle classes (slight / turn / sharp / U-turn), name changes,
motorway ramps and forks, roundabouts with exit numbers, ferries, depart
and arrive. A bend in a road with no alternative is not announced. Two
turns less than 25 m apart (crossing a divided road) become one
instruction. The app turns them into banner text and speech in
`App/Sources/Navigation/Instructions.swift`.

## Following the route

`NavigationSession` projects each GPS fix onto the route line (searching
near the last position first, so a route that crosses itself does not
jump), computes the distance and time left, and finds the next maneuver.
You are off route after two fixes more than 30–35 m (more with poor GPS
accuracy) from the line for three seconds. With automatic reroute on, a
new route from your position and direction is computed at once; the old
app's manual reroute button still exists for when auto reroute is off.
Arrival is within 15 m (car: 30 m) of the destination.
