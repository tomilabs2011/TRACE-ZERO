# Architecture

## The big picture

```
 Mac                                         iPhone
 ───                                         ──────
 Geofabrik .osm.pbf                          Documents/hungary.vmap/
   │                                           map.pmtiles ──► MapLibre Native (Metal)
   ├─► Planetiler ──► map.pmtiles              roads.tzr ─┐
   │                                           car.tzg   ─┼─► Core (C): routing
   └─► tzforge ────► roads.tzr                 bike.tzg  ─┤
                     car/bike/foot.tzg         foot.tzg  ─┘
                     search.tzs                search.tzs ──► Core (C): search
                     manifest.json             manifest.json ► PackStore (Swift)
```

All heavy work (parsing OpenStreetMap, building routing graphs and search
indexes) happens once on the Mac. The phone only memory-maps the results
and answers queries, which is why it is fast on a 2013 phone.

## Layers in the app

Each layer only depends on the ones below it. The lower layers know nothing
about UIKit, MapLibre or CarPlay, so they survive any change of UI
framework, map renderer or iOS version.

| Layer | Folder | Depends on | Job |
|---|---|---|---|
| C core | `Core/` | libc, POSIX `mmap` | File formats, routing queries, snapping, instructions, search, coordinate parsing |
| Core (Swift) | `App/Sources/Core` | C core, Foundation | `MapPack`, `PackStore`, `Router`, `SearchEngine`: Swift types around the C handles |
| Navigation | `App/Sources/Navigation` | Core, CoreLocation, AVFoundation | `LocationService` (the only GPS user), `NavigationSession` (progress along a route, pure logic), `Navigator` (the active trip, reroute, arrival), `VoiceGuide`, `Instructions` (all wording) |
| Map | `App/Sources/Map` | MapLibre | `MapController` wraps `MLNMapView`; `StyleComposer` turns a style template into a style for the installed packs; `MapImages`; `MapLocationAdapter` |
| UI | `App/Sources/UI` | everything above, UIKit | Screens and panels |
| CarPlay | `App/Sources/CarPlay` | Navigation, Map, CarPlay | Car screen templates |

Rules that keep it portable:

- **Only `MapController` touches MapLibre.** Changing or upgrading the
  renderer is one file.
- **Only `Instructions` builds user-facing sentences.** Translating the
  app is one file.
- **Only `LocationService` owns a `CLLocationManager`.** The map's user
  arrow is fed from it through `MapLocationAdapter`, so GPS runs once.
- **Navigation state lives in `Navigator`**, not in a view controller.
  The phone screen and CarPlay both just observe it, which is how a trip
  started on one shows on the other.

## Threads

- Main thread: UI, map, `Navigator`, `NavigationSession` (it is cheap).
- `tracezero.routing` queue: route queries (`Router`).
- `tracezero.search` queue: search queries (`SearchEngine`); a newer query
  always replaces an older one.
- The C handles are read-only after opening and every query allocates its
  own scratch memory, so queries can run concurrently.

## Memory and battery on the iPhone 5s (1 GB RAM)

- Pack files are memory-mapped, not loaded. Opening a country costs
  microseconds; pages are read on demand and the system can drop them
  under memory pressure without killing the app. Graph files are marked
  `POSIX_MADV_RANDOM` so iOS does not read ahead.
- A route query uses small hash maps for the nodes it touches (a few
  thousand), never arrays the size of the country.
- Rendering runs at 30 fps by default (Settings → Smooth for 60 fps).
- GPS accuracy follows the task: ten-metre accuracy with a 5 m filter
  while browsing, best accuracy while navigating, "best for navigation"
  only when driving and charging. GPS stops when the app goes to the
  background unless navigation is allowed to continue.
- While navigating the user arrow is snapped to the route, so the map
  does not jitter with GPS noise.

## Map packs and the app

`PackStore` scans `Documents/` for `*.vmap` folders and watches the
folder for changes, so packs copied with Finder appear without a restart.
`MapPack` reads `manifest.json` and opens the other files lazily on first
use. The map shows at most three packs at once (those overlapping the
screen); `StyleComposer` writes a style with one tile source per visible
pack. A route needs one pack that contains both ends; with overlapping
packs the smallest one wins (a city pack over its country pack).
