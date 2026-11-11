# Map pack format (format 1)

A pack is a folder, normally `<id>.vmap`:

| File | Made by | Read by |
|---|---|---|
| `manifest.json` | tzforge | `MapPack` (Swift) |
| `map.pmtiles` | Planetiler | MapLibre Native |
| `roads.tzr` | tzforge | Core (C) |
| `car.tzg`, `bike.tzg`, `foot.tzg` | tzforge | Core (C) |
| `search.tzs` | tzforge | Core (C) |

Every file except `manifest.json` is optional; the app uses what is
there (for example a pack without `map.pmtiles` still routes and searches).

The binary layouts are defined once, in `Core/tz_format.h`, which both
the builder and the app include. That header is the reference; this page
is the overview.

## manifest.json

```json
{
  "format": 1,
  "id": "hungary",
  "name": "Hungary",
  "created": "2026-11-10T21:38:40Z",
  "osm_timestamp": "2026-11-09T20:21:02Z",
  "source": "https://download.geofabrik.de/europe/hungary-latest.osm.pbf",
  "bbox": [16.11, 45.74, 22.90, 48.59],
  "center": [19.5, 47.16],
  "builder": "tzforge 1.0.0",
  "attribution": "© OpenStreetMap contributors",
  "files": { "roads": { "path": "roads.tzr", "bytes": 123 }, "...": {} },
  "stats": { "junctions": 0, "segments": 0, "search_entries": 0 }
}
```

`bbox` is `[min lon, min lat, max lon, max lat]`. The app refuses packs
whose `format` it does not know and says why.

## Binary files: common rules

- Little-endian, 512-byte header, then sections; every section starts on
  an 8-byte boundary. The header holds a magic string, the format version,
  the total file size (so an interrupted copy is detected), counters and up
  to 16 section offsets/sizes.
- Coordinates are int32 micro-degrees (about 11 cm).
- Times and routing weights are uint32 tenths of a second.
- Strings are a blob of NUL-terminated UTF-8; offset 0 is the empty string.
- Files are memory-mapped read-only and every offset is bounds-checked, so
  a damaged file gives an error, not a crash.

## roads.tzr

The road network shared by all profiles: junction coordinates, segments
(a piece of one OSM way between two junctions, with name, route number,
road class, flags such as roundabout/tunnel/ferry, length and the shape
points as zig-zag varint deltas), the string table and a spatial grid
(about 550 m cells) used to find the road nearest to a point.

## car.tzg / bike.tzg / foot.tzg

One contraction hierarchy per travel profile:

- per segment: weight and duration in each direction (or "no access"),
  plus an island flag;
- the hierarchy: nodes numbered by importance, edges stored at their less
  important end (CSR), each with target, weight, direction flags and either
  the segment it stands for or the middle node of the shortcut;
- maps between road junctions and hierarchy nodes;
- turn-restriction tables (which arrivals lead to a split copy of a
  junction, and which copies may leave along which segment);
- per junction, the number of usable exits (for instructions).

A graph stores a checksum of the roads header, so a graph from a different
build is rejected instead of giving wrong routes.

## search.tzs

Entries (position, name, context such as "1052 Budapest" or
"Cafe · Váci utca 12, Budapest", extra search words, importance, kind),
sorted by importance; a sorted dictionary of normalised words; for each
word a delta-coded list of entry ids; a spatial grid for "what is here".
Normalisation (`Core/tz_text.c`) lower-cases, removes accents for Latin,
Greek and Cyrillic, and splits letters from digits.

## Changing the format

Bump `TZ_FORMAT_VERSION` in `Core/tz_format.h` for any incompatible change,
and keep `tzFormatVersion` in `App/Sources/Core/MapPack.swift` equal to it.
Old packs are then rejected with "Rebuild the pack" instead of misread.
