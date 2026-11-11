#!/usr/bin/env python3
"""Generate the TRACE//ZERO map styles.

    python3 Tools/make_styles.py

writes App/Resources/Styles/{outline,hatch,solid,minimal,walker,walker-inverted}.json.

All styles read OpenMapTiles-schema vector tiles (what Planetiler produces)
and share the same navigation layers. The first four use two colours on
black: CYAN for the map, RED only for the destination. The two WALKER
styles are the "Alan Walker" monitor look (see walker_style()).

Placeholders filled in by the app at runtime (see StyleComposer.swift):
  {TILES}   file URL of a pack's map.pmtiles (the "omt" source is
            duplicated once per installed pack)
  {GLYPHS}  file URL of the bundled glyph directory
Images added at runtime (MapImages.swift): "hatch", "hatch-water",
"chevron", and for the WALKER styles "hatch-navy", "hatch-cyan",
"chevron-navy" and "chevron-cyan".

How the route stands out from other roads (the most important rule of the
design):
  * it is wider than any road at every zoom level;
  * it is drawn as a sandwich: soft cyan glow, black gap, bright cyan core;
  * black chevrons on the core show the direction of travel - no other line
    on the map has them;
  * while navigating the whole base map is dimmed by a black veil
    ("nav-dim") that sits below the route but above the roads;
  * the part already driven is reduced to a thin faint line.
"""
import json
import os

CYAN = "#00CCFF"
BLACK = "#000000"
RED = "#FF2B3A"
FONT = ["Noto Sans Regular"]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "App", "Resources", "Styles")

MODES = ["outline", "hatch", "solid", "minimal", "walker", "walker-inverted"]

# WALKER: the look of the phone map in Alan Walker's "Alone" video - a bright
# cyan screen, navy line work, royal-indigo water and a pixel font.
# WALKER INVERTED swaps screen and ink: navy screen, cyan lines, bright cyan
# water, like a dot-matrix display. In both, coral is used only for the
# destination ring and the route is the only white thing on the map.
WALKER_CYAN = "#3ADCE8"
WALKER_NAVY = "#0B1650"
WALKER_INDIGO = "#2233B8"
WALKER_INDIGO_EDGE = "#15218C"
WALKER_WHITE = "#FFFFFF"
WALKER_CORAL = "#FF6B7E"
WALKER_FONT = ["Walker Mono"]   # VT323 merged over Noto Sans, see Tools/make_glyphs.py


def zoom(base, *stops):
    """interpolate exponential over zoom: zoom(1.5, 10, 1, 18, 8)"""
    return ["interpolate", ["exponential", base], ["zoom"], *stops]


def lin(*stops):
    return ["interpolate", ["linear"], ["zoom"], *stops]


def cls(*values):
    return ["match", ["get", "class"], list(values), True, False]


def zoom_by_class(base, classes, *stops):
    """Zoom interpolation whose output depends on the feature class:
    stops are (zoom, width if class in classes, width otherwise). The zoom
    interpolation must be the outermost expression in MapLibre styles."""
    out = []
    for z, a, b in zip(stops[0::3], stops[1::3], stops[2::3]):
        out += [z, ["match", ["get", "class"], classes, a, b]]
    return ["interpolate", ["exponential", base], ["zoom"], *out]


# ---------------------------------------------------------------- base map

def background():
    return [{"id": "background", "type": "background", "paint": {"background-color": BLACK}}]


def water(mode):
    layers = []
    filt = ["all", ["!=", ["get", "brunnel"], "tunnel"]]
    if mode == "solid":
        layers.append({"id": "water-fill", "type": "fill", "source": "omt", "source-layer": "water",
                       "filter": filt, "paint": {"fill-color": CYAN, "fill-opacity": 0.22, "fill-antialias": False}})
    elif mode == "hatch":
        layers.append({"id": "water-fill", "type": "fill", "source": "omt", "source-layer": "water",
                       "filter": filt, "paint": {"fill-pattern": "hatch-water", "fill-antialias": False}})
    layers.append({"id": "water-edge", "type": "line", "source": "omt", "source-layer": "water", "filter": filt,
                   "paint": {"line-color": CYAN, "line-width": zoom(1.4, 6, 0.5, 14, 1.0, 18, 1.6),
                             "line-opacity": 0.45 if mode == "minimal" else 0.85}})
    return layers


def waterways(mode):
    return [{"id": "waterway", "type": "line", "source": "omt", "source-layer": "waterway", "minzoom": 9,
             "filter": ["!=", ["get", "brunnel"], "tunnel"],
             "layout": {"line-cap": "round"},
             "paint": {"line-color": CYAN,
                       "line-opacity": 0.35 if mode == "minimal" else 0.6,
                       "line-width": zoom_by_class(1.4, ["river", "canal"],
                                                   9, 0.6, 0.1, 12, 1.0, 0.3, 14, 1.6, 0.6, 18, 4.0, 1.5)}}]


GREEN_LANDCOVER = ["wood", "forest", "grass", "wetland", "scrub"]
GREEN_LANDUSE = ["cemetery", "grass", "park", "recreation_ground", "pitch", "playground"]


def greens(mode):
    if mode == "minimal":
        return []
    out = []
    srcs = [("landcover", cls(*GREEN_LANDCOVER)), ("park", True), ("landuse", cls(*GREEN_LANDUSE))]
    for layer, filt in srcs:
        base = {"source": "omt", "source-layer": layer}
        if filt is not True:
            base["filter"] = filt
        if mode == "solid":
            out.append({**base, "id": f"{layer}-fill", "type": "fill",
                        "paint": {"fill-color": CYAN, "fill-opacity": 0.08, "fill-antialias": False}})
        elif mode == "hatch":
            out.append({**base, "id": f"{layer}-fill", "type": "fill", "minzoom": 10,
                        "paint": {"fill-pattern": "hatch", "fill-antialias": False, "fill-opacity": 0.8}})
        out.append({**base, "id": f"{layer}-edge", "type": "line", "minzoom": 11,
                    "paint": {"line-color": CYAN, "line-width": 0.6,
                              "line-opacity": lin(11, 0.1, 15, 0.3),
                              "line-dasharray": [2, 2]}})
    return out


def aeroway(mode):
    if mode == "minimal":
        return []
    return [{"id": "aeroway", "type": "line", "source": "omt", "source-layer": "aeroway", "minzoom": 11,
             "filter": ["==", ["geometry-type"], "LineString"],
             "paint": {"line-color": CYAN, "line-opacity": 0.35,
                       "line-width": zoom_by_class(1.5, ["runway"], 11, 1.0, 0.5, 17, 12.0, 5.0)}}]


def buildings(mode):
    if mode == "minimal":
        return []
    out = []
    if mode == "solid":
        out.append({"id": "building-fill", "type": "fill", "source": "omt", "source-layer": "building", "minzoom": 14,
                    "paint": {"fill-color": CYAN, "fill-opacity": lin(14, 0.0, 16, 0.14), "fill-antialias": False}})
    out.append({"id": "building-edge", "type": "line", "source": "omt", "source-layer": "building", "minzoom": 14,
                "paint": {"line-color": CYAN, "line-width": 0.6, "line-opacity": lin(14, 0.0, 16, 0.4)}})
    return out


def boundaries(mode):
    return [{"id": "boundary-country", "type": "line", "source": "omt", "source-layer": "boundary",
             "filter": ["all", ["==", ["get", "admin_level"], 2], ["!=", ["get", "maritime"], 1]],
             "paint": {"line-color": CYAN, "line-opacity": 0.6, "line-width": zoom(1.4, 3, 0.6, 12, 1.6),
                       "line-dasharray": [4, 2, 1, 2]}},
            {"id": "boundary-state", "type": "line", "source": "omt", "source-layer": "boundary", "minzoom": 5,
             "filter": ["all", ["==", ["get", "admin_level"], 4], ["!=", ["get", "maritime"], 1]],
             "paint": {"line-color": CYAN, "line-opacity": 0.25, "line-width": 0.8, "line-dasharray": [3, 3]}}]


# Road classes from major to minor with (min zoom, width stops, opacity).
ROADS = [
    ("motorway",  ["motorway"],  5,  [5, 0.8, 10, 2.2, 14, 4.5, 18, 14], 1.0),
    ("trunk",     ["trunk"],     5,  [5, 0.6, 10, 1.8, 14, 4.0, 18, 13], 1.0),
    ("primary",   ["primary"],   7,  [7, 0.5, 10, 1.4, 14, 3.4, 18, 12], 1.0),
    ("secondary", ["secondary"], 9,  [9, 0.4, 11, 1.0, 14, 2.8, 18, 10], 0.95),
    ("tertiary",  ["tertiary"],  10, [10, 0.4, 12, 0.9, 14, 2.2, 18, 9], 0.9),
    ("minor",     ["minor", "busway", "raceway"], 12, [12, 0.4, 14, 1.4, 18, 7], 0.75),
    ("service",   ["service", "track"], 13, [13, 0.3, 15, 0.8, 18, 4], 0.5),
]


def inner_stops(stops):
    """Width of the black inside of an outlined road: the full width minus
    two cyan edges (about 0.5 px each when thin, 1 px each when wide)."""
    out = []
    for i in range(0, len(stops), 2):
        z, w = stops[i], stops[i + 1]
        edge = 0.0 if w < 1.2 else (1.0 if w < 4 else 2.0)
        out += [z, max(0.0, w - edge)]
    return out


def road_layers(mode, brunnel):
    """Roads for one brunnel group: 'tunnel', 'road' (ground level) or 'bridge'."""
    out = []
    if brunnel == "tunnel":
        bfilt = ["==", ["get", "brunnel"], "tunnel"]
    elif brunnel == "bridge":
        bfilt = ["==", ["get", "brunnel"], "bridge"]
    else:
        bfilt = ["!", ["match", ["get", "brunnel"], ["tunnel", "bridge"], True, False]]
    tunnel = brunnel == "tunnel"

    # paths and footways: dotted, never cased
    if mode != "minimal" or brunnel == "road":
        out.append({"id": f"{brunnel}-path", "type": "line", "source": "omt", "source-layer": "transportation",
                    "minzoom": 14, "filter": ["all", bfilt, cls("path")],
                    "layout": {"line-cap": "round"},
                    "paint": {"line-color": CYAN, "line-opacity": 0.45 if not tunnel else 0.2,
                              "line-width": zoom(1.5, 14, 0.5, 18, 1.6), "line-dasharray": [1, 2]}})

    for name, classes, minz, stops, opacity in reversed(ROADS):
        filt = ["all", bfilt, cls(*classes)]
        width = zoom(1.5, *stops)
        op = opacity * (0.35 if tunnel else 1.0)
        if mode == "outline" and name in ("motorway", "trunk", "primary", "secondary", "tertiary"):
            # wireframe look: two thin parallel lines (cyan edge, black inside)
            out.append({"id": f"{brunnel}-{name}-edge", "type": "line", "source": "omt",
                        "source-layer": "transportation", "minzoom": minz, "filter": filt,
                        "layout": {"line-cap": "butt", "line-join": "round"},
                        "paint": {"line-color": CYAN, "line-opacity": op, "line-width": width}})
            inner = zoom(1.5, *inner_stops(stops))
            out.append({"id": f"{brunnel}-{name}-core", "type": "line", "source": "omt",
                        "source-layer": "transportation", "minzoom": max(minz, 11), "filter": filt,
                        "layout": {"line-cap": "butt", "line-join": "round"},
                        "paint": {"line-color": BLACK, "line-width": inner}})
        else:
            if mode == "minimal":
                width = zoom(1.5, *[v if i % 2 == 0 else v * 0.6 for i, v in enumerate(stops)])
            paint = {"line-color": CYAN, "line-opacity": op, "line-width": width}
            if tunnel:
                paint["line-dasharray"] = [2, 1]
            out.append({"id": f"{brunnel}-{name}", "type": "line", "source": "omt", "source-layer": "transportation",
                        "minzoom": minz, "filter": filt,
                        "layout": {"line-cap": "round" if not tunnel else "butt", "line-join": "round"},
                        "paint": paint})
    if brunnel == "road":
        out.append({"id": "rail", "type": "line", "source": "omt", "source-layer": "transportation", "minzoom": 11,
                    "filter": ["all", bfilt, cls("rail", "transit")],
                    "paint": {"line-color": CYAN, "line-opacity": 0.4, "line-width": zoom(1.4, 11, 0.5, 18, 1.6),
                              "line-dasharray": [3, 3]}})
        out.append({"id": "ferry", "type": "line", "source": "omt", "source-layer": "transportation", "minzoom": 8,
                    "filter": cls("ferry"),
                    "paint": {"line-color": CYAN, "line-opacity": 0.5, "line-width": 1, "line-dasharray": [6, 4]}})
    return out


def roads(mode):
    return road_layers(mode, "tunnel") + road_layers(mode, "road") + road_layers(mode, "bridge")


def label(id_, layer, filt, size, minzoom, extra_layout=None, extra_paint=None):
    lay = {"text-field": ["coalesce", ["get", "name"], ["get", "name_en"]], "text-font": FONT, "text-size": size,
           "text-max-width": 8, "text-padding": 2}
    if extra_layout:
        lay.update(extra_layout)
    paint = {"text-color": CYAN, "text-halo-color": BLACK, "text-halo-width": 1.6}
    if extra_paint:
        paint.update(extra_paint)
    d = {"id": id_, "type": "symbol", "source": "omt", "source-layer": layer, "minzoom": minzoom,
         "layout": lay, "paint": paint}
    if filt is not None:
        d["filter"] = filt
    return d


def road_labels(mode):
    return [
        label("road-ref", "transportation_name",
              ["all", ["has", "ref"], cls("motorway", "trunk", "primary")], 10, 8,
              {"text-field": ["get", "ref"], "symbol-placement": "line", "symbol-spacing": 400,
               "text-rotation-alignment": "viewport", "text-letter-spacing": 0.05},
              {"text-halo-width": 2.2}),
        label("road-name", "transportation_name",
              ["!", cls("motorway", "trunk", "rail", "transit", "ferry")],
              zoom(1.2, 13, 9.5, 18, 13), 13,
              {"symbol-placement": "line", "symbol-spacing": 300, "text-max-angle": 30,
               "text-rotation-alignment": "map", "text-pitch-alignment": "viewport"},
              {"text-opacity": 0.9}),
    ]


def poi_labels(mode):
    if mode == "minimal":
        return []
    keep = ["railway", "bus", "aerialway", "hospital", "fuel", "parking", "college", "town_hall", "museum",
            "theatre", "cinema", "shop", "grocery", "restaurant", "cafe", "fast_food", "bar", "pharmacy",
            "lodging", "attraction", "place_of_worship", "police", "post"]
    return [
        {"id": "poi-dot", "type": "circle", "source": "omt", "source-layer": "poi", "minzoom": 16,
         "filter": ["all", cls(*keep), ["<=", ["get", "rank"], 25]],
         "paint": {"circle-radius": 2.2, "circle-color": BLACK, "circle-stroke-color": CYAN,
                   "circle-stroke-width": 1, "circle-opacity": 0.9}},
        label("poi-name", "poi", ["all", cls(*keep), ["<=", ["get", "rank"], 25]], 10, 16,
              {"text-anchor": "top", "text-offset": [0, 0.5], "text-max-width": 7, "text-optional": True},
              {"text-opacity": 0.75}),
        label("housenumber", "housenumber", None, 9, 17,
              {"text-field": ["get", "housenumber"], "text-padding": 4},
              {"text-opacity": 0.55, "text-halo-width": 1}),
    ]


def water_labels(mode):
    return [label("water-name", "water_name", ["==", ["geometry-type"], "Point"], 11, 10,
                  {"text-letter-spacing": 0.15, "text-transform": "uppercase"}, {"text-opacity": 0.7})]


def place_labels(mode):
    return [
        label("place-minor", "place", cls("neighbourhood", "quarter", "suburb", "hamlet", "isolated_dwelling"),
              zoom(1.2, 12, 10, 16, 12), 12,
              {"text-transform": "uppercase", "text-letter-spacing": 0.12}, {"text-opacity": 0.75}),
        label("place-village", "place", cls("village"), zoom(1.2, 10, 10, 15, 13), 10, None, None),
        label("place-town", "place", cls("town"), zoom(1.2, 8, 11, 14, 15), 8,
              {"text-transform": "uppercase", "text-letter-spacing": 0.08}, None),
        label("place-city", "place", cls("city"), zoom(1.2, 5, 11, 12, 19), 4,
              {"text-transform": "uppercase", "text-letter-spacing": 0.12}, {"text-halo-width": 2}),
        label("place-country", "place", cls("country", "state"), zoom(1.2, 3, 10, 7, 15), 2,
              {"text-transform": "uppercase", "text-letter-spacing": 0.25}, {"text-opacity": 0.8}),
    ]


# -------------------------------------------------------- navigation layers

def nav_layers_below_labels():
    route_w = zoom(1.5, 8, 3.0, 12, 5.0, 14, 7.5, 16, 11, 18, 18)
    gap_w = zoom(1.5, 8, 6.0, 12, 9.0, 14, 13, 16, 18, 18, 28)
    glow_w = zoom(1.5, 8, 12, 12, 18, 14, 24, 16, 32, 18, 46)
    return [
        # Black veil that pushes the base map back while navigating.
        {"id": "nav-dim", "type": "fill", "source": "nav-dim",
         "paint": {"fill-color": BLACK, "fill-opacity": 0.0, "fill-antialias": False}},
        # Already travelled part of the route: thin and faint.
        {"id": "route-passed", "type": "line", "source": "route-passed",
         "layout": {"line-cap": "round", "line-join": "round"},
         "paint": {"line-color": CYAN, "line-opacity": 0.35, "line-width": zoom(1.5, 10, 1.0, 18, 3.0),
                   "line-dasharray": [1, 1.5]}},
        {"id": "route-glow", "type": "line", "source": "route",
         "layout": {"line-cap": "round", "line-join": "round"},
         "paint": {"line-color": CYAN, "line-opacity": 0.28, "line-width": glow_w, "line-blur": 6}},
        {"id": "route-gap", "type": "line", "source": "route",
         "layout": {"line-cap": "round", "line-join": "round"},
         "paint": {"line-color": BLACK, "line-width": gap_w}},
        {"id": "route-core", "type": "line", "source": "route",
         "layout": {"line-cap": "round", "line-join": "round"},
         "paint": {"line-color": CYAN, "line-width": route_w}},
        {"id": "route-arrows", "type": "symbol", "source": "route", "minzoom": 12,
         "layout": {"symbol-placement": "line", "symbol-spacing": zoom(1.0, 12, 50, 18, 90),
                    "icon-image": "chevron", "icon-size": zoom(1.5, 12, 0.45, 14, 0.6, 16, 0.85, 18, 1.4),
                    "icon-allow-overlap": True, "icon-ignore-placement": True,
                    "icon-rotation-alignment": "map", "icon-pitch-alignment": "map"},
         "paint": {"icon-opacity": 1.0}},
        # Upcoming maneuver: a ring on the route.
        {"id": "maneuver-ring", "type": "circle", "source": "maneuver",
         "paint": {"circle-radius": zoom(1.5, 12, 6, 18, 16), "circle-color": BLACK,
                   "circle-stroke-color": CYAN, "circle-stroke-width": 3,
                   "circle-pitch-alignment": "map"}},
    ]


def nav_layers_top():
    return [
        # Dropped pin / selected search result (cyan) before a route exists.
        {"id": "pin-halo", "type": "circle", "source": "pin",
         "paint": {"circle-radius": 14, "circle-color": CYAN, "circle-opacity": 0.18}},
        {"id": "pin-dot", "type": "circle", "source": "pin",
         "paint": {"circle-radius": 6, "circle-color": BLACK, "circle-stroke-color": CYAN, "circle-stroke-width": 2.5}},
        # Destination: the only red on the map.
        {"id": "dest-halo", "type": "circle", "source": "dest",
         "paint": {"circle-radius": zoom(1.3, 10, 12, 18, 22), "circle-color": RED, "circle-opacity": 0.25}},
        {"id": "dest-dot", "type": "circle", "source": "dest",
         "paint": {"circle-radius": zoom(1.3, 10, 6, 18, 10), "circle-color": RED,
                   "circle-stroke-color": BLACK, "circle-stroke-width": 2}},
        {"id": "dest-center", "type": "circle", "source": "dest",
         "paint": {"circle-radius": zoom(1.3, 10, 1.6, 18, 3), "circle-color": BLACK}},
    ]


EMPTY = {"type": "FeatureCollection", "features": []}
WORLD = {"type": "FeatureCollection", "features": [{"type": "Feature", "properties": {}, "geometry": {
    "type": "Polygon", "coordinates": [[[-180, -85], [180, -85], [180, 85], [-180, 85], [-180, -85]]]}}]}


def recolor(obj, mapping):
    """Copy of a style fragment with colour strings swapped."""
    if isinstance(obj, dict):
        return {k: recolor(v, mapping) for k, v in obj.items()}
    if isinstance(obj, list):
        return [recolor(v, mapping) for v in obj]
    if isinstance(obj, str) and obj.upper() in mapping:
        return mapping[obj.upper()]
    return obj


def walker_style(inverted):
    """WALKER (inverted=False) and WALKER INVERTED: the SOLID layout (same
    layers, widths and zoom rules) recoloured, with the pixel font.

    The route keeps every rule from the top of this file; its colour is
    white, which nothing else on these maps uses:
      WALKER           navy glow, white band, navy core, cyan chevrons
      WALKER INVERTED  white glow, navy band, white core, navy chevrons
    """
    screen, ink = (WALKER_NAVY, WALKER_CYAN) if inverted else (WALKER_CYAN, WALKER_NAVY)
    style = recolor(build("solid"), {CYAN.upper(): ink, BLACK.upper(): screen})
    for layer in style["layers"]:
        lid, paint, layout = layer["id"], layer.get("paint", {}), layer.get("layout", {})
        if "text-font" in layout:
            layout["text-font"] = WALKER_FONT
        if lid == "water-fill":
            paint.update({"fill-color": WALKER_CYAN if inverted else WALKER_INDIGO, "fill-opacity": 1.0})
        elif lid == "water-edge":
            paint.update({"line-color": WALKER_CYAN if inverted else WALKER_INDIGO_EDGE, "line-opacity": 1.0})
        elif lid == "waterway":
            paint.update({"line-color": WALKER_CYAN if inverted else WALKER_INDIGO, "line-opacity": 1.0})
        elif lid == "water-name" and inverted:
            # sits on bright water: navy letters, cyan halo
            paint.update({"text-color": WALKER_NAVY, "text-halo-color": WALKER_CYAN})
        elif lid in ("landcover-fill", "park-fill", "landuse-fill"):
            paint.pop("fill-color", None)
            paint.pop("fill-opacity", None)
            paint["fill-pattern"] = "hatch-cyan" if inverted else "hatch-navy"
            layer["minzoom"] = 10
        elif lid == "building-fill":
            # line work, not blocks: outlines with a faint tint
            paint["fill-opacity"] = lin(14, 0.0, 16, 0.06)
        elif lid == "route-glow":
            paint.update({"line-color": WALKER_WHITE if inverted else WALKER_NAVY,
                          "line-opacity": 0.35 if inverted else 0.3})
        elif lid == "route-gap":
            paint["line-color"] = WALKER_NAVY if inverted else WALKER_WHITE
        elif lid == "route-core":
            paint["line-color"] = WALKER_WHITE if inverted else WALKER_NAVY
        elif lid == "route-arrows":
            layout["icon-image"] = "chevron-navy" if inverted else "chevron-cyan"
        elif lid == "dest-halo":
            # the soft coral ring from the video
            paint.update({"circle-color": WALKER_CORAL, "circle-opacity": 0.3, "circle-blur": 0.6,
                          "circle-radius": zoom(1.3, 10, 18, 18, 30)})
        elif lid == "dest-dot":
            # a donut: screen-coloured centre, so it reads as a ring even
            # where the route ends inside it
            paint.update({"circle-color": screen, "circle-opacity": 1.0,
                          "circle-stroke-color": WALKER_CORAL, "circle-stroke-width": 6,
                          "circle-radius": zoom(1.3, 10, 9, 18, 14)})
        elif lid == "dest-center":
            paint["circle-color"] = ink
    name = "walker-inverted" if inverted else "walker"
    style["name"] = f"TRACE//ZERO {name}"
    style["metadata"]["tracezero:mode"] = name
    # MapController lays the screen texture over the map
    style["metadata"]["tracezero:overlay"] = "dot-matrix" if inverted else "scanlines"
    return style


def build(mode):
    if mode in ("walker", "walker-inverted"):
        return walker_style(mode == "walker-inverted")
    layers = []
    layers += background()
    layers += greens(mode)
    layers += water(mode)
    layers += waterways(mode)
    layers += aeroway(mode)
    layers += buildings(mode)
    layers += boundaries(mode)
    layers += roads(mode)
    layers += road_labels(mode)
    layers += poi_labels(mode)
    layers += water_labels(mode)
    layers += nav_layers_below_labels()
    layers += place_labels(mode)
    layers += nav_layers_top()
    ids = [l["id"] for l in layers]
    assert len(ids) == len(set(ids)), "duplicate layer ids"
    return {
        "version": 8,
        "name": f"TRACE//ZERO {mode}",
        "metadata": {"tracezero:mode": mode, "tracezero:generator": "Tools/make_styles.py"},
        "glyphs": "{GLYPHS}/{fontstack}/{range}.pbf",
        "sources": {
            "omt": {"type": "vector", "url": "pmtiles://{TILES}",
                    "attribution": "© OpenStreetMap contributors"},
            "nav-dim": {"type": "geojson", "data": WORLD},
            "route": {"type": "geojson", "data": EMPTY},
            "route-passed": {"type": "geojson", "data": EMPTY},
            "maneuver": {"type": "geojson", "data": EMPTY},
            "pin": {"type": "geojson", "data": EMPTY},
            "dest": {"type": "geojson", "data": EMPTY},
        },
        "layers": layers,
    }


def main():
    os.makedirs(OUT, exist_ok=True)
    for mode in MODES:
        style = build(mode)
        path = os.path.join(OUT, f"{mode}.json")
        with open(path, "w") as f:
            json.dump(style, f, indent=1, ensure_ascii=False)
            f.write("\n")
        print(f"{path}: {len(style['layers'])} layers")


if __name__ == "__main__":
    main()
