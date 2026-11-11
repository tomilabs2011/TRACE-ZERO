# Decisions, verification and known limits

A log of the choices made in the rewrite, why, and how far each part has
been tested. Newest information first within each section.

## Decisions

**Start over instead of patching.** The old app routed with SQLite queries,
which cannot scale to big cities. The routing data format had to change,
which touches the builder, the app and the pack format, so everything was
rebuilt around a new core.

**Routing and search in C, not Swift.** One C library (`Core/`) is used by
the Mac builder and the phone, so they can never disagree about a file
format, and the same code is tested on Linux, macOS and iOS. C over
memory-mapped files is also the most frugal option on a 1 GB phone.

**Contraction hierarchies for routing.** The standard technique of
OSRM and GraphHopper: milliseconds per query on any distance, at the cost of
preprocessing on the Mac. Turn restrictions are handled by splitting
junctions (exact, no slowdown) rather than an edge-based graph (about 3×
the size).

**Own search index instead of SQLite FTS.** Which FTS versions and
tokenizers iOS 12's SQLite has is not documented. A small prefix index with
accent folding written in C behaves identically everywhere and was fully
testable here.

**Own OSM PBF reader instead of libosmium.** The builder needs only zlib
(always on macOS), so `make` works on any Mac with Xcode, with no Homebrew
packages.

**MapLibre Native 6.10.0, pinned.** The old app used this version on the
5s. Every MapLibre iOS release since 6.0 renders with Metal, and the A7 is
older than the oldest GPU MapLibre mentions testing (iPad Air 2, A8X);
the Metal backend also calls an API (`supportsFamily:`) documented as iOS
13+. Because 6.10.0 already worked on this phone, it stays pinned rather
than risking a newer release. On newer iPhones upgrade freely
([PORTING.md](PORTING.md)).

**PMTiles from Planetiler.** Planetiler is the fastest OpenMapTiles
generator and writes PMTiles directly; MapLibre reads PMTiles from a local
file without a tile server.

**UIKit with frame-based layout, iOS 12 APIs only.** SwiftUI and SF
Symbols need iOS 13. Icons are drawn with Core Graphics. Everything
MapLibre-specific is in one class, everything user-visible in English is in
one file, so a later move to SwiftUI or another renderer stays contained.

**One GPS owner.** MapLibre normally runs its own location manager. Here
the map is fed from the app's `LocationService` through a small adapter,
so the GPS radio runs once, and while navigating the arrow is snapped to
the route.

**Navigation pauses in the background by default**, as before: GPS stops
when the app or screen sleeps. The screen is kept on while navigating
(Settings), and background navigation is an option. CarPlay always keeps
navigation running.

**Automatic reroute by default.** The old app made rerouting manual,
presumably because routing was slow. Now a reroute is instant, so it
happens automatically; the button is still there.

**Jailbreak install route.** On a jailbroken phone a fake-signed IPA
avoids the 7-day expiry of free signing and is the only way to include the
CarPlay entitlement without Apple's approval.

**Generated Xcode projects.** `Tools/gen_xcodeproj.py` writes both
projects, so the project settings are readable code and survive merges.

**WALKER: a style that brings its own interface.** The two WALKER maps are
style JSON generated from SOLID, so the four standard styles stay
untouched and the route rules still apply. The interface follows through
`Theme.look`: colours and fonts are read from Theme when a view is built,
and AppDelegate rebuilds the main screen when the look changes. This is
simpler and safer than re-colouring every live view, and nothing is lost
because navigation state lives in `Navigator`. The map's pixel font is
shipped as MapLibre glyph files ("Walker Mono", VT323 merged over Noto)
instead of a font fallback stack, because a local glyph URL cannot combine
two font directories. The screen textures are a UIView pattern, not a
style layer, so they cost nothing per frame.

## What has been verified, and how

Done in a Linux container, without a Mac:

| Part | Verification |
|---|---|
| C core and builder | Built with GCC and Clang with strict warnings; run under AddressSanitizer and UndefinedBehaviorSanitizer. |
| Routing correctness | `tzforge verify`: contraction hierarchy vs plain Dijkstra on random pairs. 0 mismatches on real Helsinki data (car, bike, foot, with real turn restrictions) and on synthetic cities of 40,000 / 90,000 / 360,000 junctions with one-way streets and restrictions on through-ways. |
| End-to-end routes | Thousands of random routes per profile: continuous geometry, monotonic distance and time, valid instructions. |
| Routing speed | See the table in [ROUTING.md](ROUTING.md) (desktop CPU; the phone will be slower). |
| Search | Real queries on Helsinki data (addresses, POIs, streets by district, accents), 344 unit checks of normalisation and coordinate parsing. |
| Pack pipeline | `forge.sh` run end to end on a local extract, without the map step. Region lookup tested with a sample of the Geofabrik index. |
| Map styles | Validated with MapLibre's style-spec validator; rendered in MapLibre GL JS with real Helsinki data and a real route (`docs/images`). |
| Xcode projects | Generated and parsed with an independent `.pbxproj` parser. |
| Swift code | Every file parsed by the Swift 6 compiler. The non-UI layer (packs, router, search, navigation logic, formatting) type-checked against the real C header. |

**Not verified here** (needs a Mac, the phone or the network):

- Compiling the UIKit, MapLibre, CarPlay and SwiftUI code with Xcode. The
  API calls were checked against the MapLibre 6.10.0 headers and Apple's
  documented iOS 12 APIs, but the first build in Xcode may still show
  small errors. Please send them if it does.
- Running on the iPhone 5s, rendering performance on the A7, and GPS
  behaviour on the road.
- The Planetiler step and Geofabrik downloads (both blocked from the
  container). The commands follow Planetiler 0.10.2's documented usage.
- CarPlay ([CARPLAY.md](CARPLAY.md)).
- Pack sizes in [INSTALL.md](INSTALL.md) are estimates, not measurements.

## Known limits

- Routing works inside one pack; a route cannot cross from one country
  pack into another. A pack of several countries (for example a Geofabrik
  region like `dach`) avoids this.
- Via-way turn restrictions and conditional restrictions (time-dependent)
  are ignored. No live traffic (the app is offline).
- Instructions and voice are English only (one file to translate:
  `App/Sources/Navigation/Instructions.swift`).
- Address display order is "street number", which suits most of Europe.
- Building the routing graph for a very large country (Germany, France)
  needs several GB of RAM on the Mac and some minutes per profile.
