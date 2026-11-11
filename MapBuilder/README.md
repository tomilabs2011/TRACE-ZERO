# TRACE ZERO MAP FORGE

Builds map packs (`.vmap` folders) for TRACE//ZERO on a Mac. Two ways to
use it, both doing the same thing.

## The Mac app

1. Open `MapBuilder/MapForge.xcodeproj` in Xcode and press Run (no Apple ID
   needed: it is signed to run locally). The build compiles `tzforge` and
   puts it inside the app together with `forge.sh`. Afterwards you can keep
   `TRACE ZERO MAP FORGE.app` from Xcode's Products folder anywhere.
2. Search for a region (country, state, city), or choose a local
   `.osm.pbf` file.
3. Choose the routing profiles and where to save, then press **FORGE**.
4. When it says ready, press **Show in Finder** and drag the `.vmap`
   folder to the phone (Finder → iPhone → Files → TRACE ZERO).

## Terminal

```bash
Forge/forge.sh --list hungary
```

```bash
Forge/forge.sh hungary --out ~/Desktop
```

```bash
Forge/forge.sh --file my-area.osm.pbf --id my-area --name "My area"
```

`Forge/forge.sh --help` lists every option (profiles, no map, no search,
fresh download).

## What happens

1. **Download** the OpenStreetMap extract from Geofabrik (checked against
   its MD5, cached for a week).
2. **Map tiles** with Planetiler 0.10.2 (OpenMapTiles schema, PMTiles).
   The first run downloads Planetiler, a Java 21 runtime if you do not
   have one, and about 1.5 GB of ocean, lake and Natural Earth data, all
   reused for later regions.
3. **Routing and search** with `tzforge` (this repository's C builder).
4. **Check**: every routing graph is compared with plain Dijkstra on random
   pairs before the pack is called ready.

Everything downloaded lives in `~/Library/Caches/TraceZeroForge`; delete
that folder to reclaim the space.

## Requirements

- macOS 12 or newer, Xcode (or at least the command line tools:
  `xcode-select --install`).
- Free disk: a few times the size of the extract plus the 1.5 GB shared
  data. RAM: 8 GB is plenty for most countries; the largest ones (Germany,
  France) want 16 GB.

## Time

A city takes a minute or two. A mid-sized country like Hungary takes
roughly 10–20 minutes on a recent Mac, most of it drawing the map tiles.
These are estimates; the first run also includes the one-time downloads.

## The tzforge tool

```bash
make -C Forge
```

builds `Forge/tzforge`. Besides `build` it can query a pack directly,
which is handy for checking a pack without the phone:

```bash
Forge/tzforge route hungary.vmap car "47.4979,19.0402" "47.5316,21.6273"
```

```bash
Forge/tzforge search hungary.vmap "andrassy ut 60" --near 47.50,19.06
```

```bash
Forge/tzforge verify hungary.vmap
```
