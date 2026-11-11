# Style preview

Renders the app's map styles in a desktop browser (MapLibre GL JS), with
real OpenStreetMap data and a real route from `tzforge`, so style changes
can be checked without building the app. The screenshots in
`docs/images` were made with it.

It is a development aid only; the phone uses MapLibre Native with the
same style JSON.

```bash
cd Tools/style-preview
python3 -m venv .venv && .venv/bin/pip install osmium        # OSM reader
npm install maplibre-gl@4.7.1 geojson-vt@4.0.2 vt-pbf@3.1.3 playwright
.venv/bin/python osm2omt.py ../../Tests/data/helsinki.osm.pbf omt.json
../../Forge/tzforge build ../../Tests/data/helsinki.osm.pbf /tmp/hel.vmap
../../Forge/tzforge route /tmp/hel.vmap car "60.1712,24.9396" "60.1665,24.9505" --geojson > route.geojson
node server.cjs "$PWD/node_modules" "$PWD" "$PWD/../../App/Resources" 8765 &
open "http://127.0.0.1:8765/index.html?mode=outline&nav=1"   # mode: outline|hatch|solid|minimal|screen
OUT=. node shoot.cjs outline:0 outline:1 solid:1             # optional screenshots
```

`osm2omt.py` is a rough converter into the OpenMapTiles layer schema (only
the layers and classes the styles use); real packs get their tiles from
Planetiler. The pattern and chevron images in `index.html` mirror
`App/Sources/Map/MapImages.swift`; keep them in sync.
