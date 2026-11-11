"""Rough OSM -> OpenMapTiles-schema GeoJSON, only for the style render test."""
import json, sys, osmium

HW = {"motorway": "motorway", "motorway_link": "motorway", "trunk": "trunk", "trunk_link": "trunk",
      "primary": "primary", "primary_link": "primary", "secondary": "secondary", "secondary_link": "secondary",
      "tertiary": "tertiary", "tertiary_link": "tertiary", "residential": "minor", "unclassified": "minor",
      "living_street": "minor", "road": "minor", "service": "service", "track": "track",
      "footway": "path", "path": "path", "cycleway": "path", "steps": "path", "pedestrian": "path", "bridleway": "path"}
layers = {k: [] for k in ["transportation", "transportation_name", "building", "water", "park", "landcover",
                          "landuse", "place", "poi", "housenumber", "waterway", "water_name"]}

def feat(layer, geom, props):
    layers[layer].append({"type": "Feature", "properties": props, "geometry": geom})

class H(osmium.SimpleHandler):
    def node(self, n):
        t = n.tags
        c = [n.location.lon, n.location.lat]
        if "place" in t and "name" in t:
            feat("place", {"type": "Point", "coordinates": c}, {"class": t["place"], "name": t["name"]})
        elif "amenity" in t and "name" in t:
            feat("poi", {"type": "Point", "coordinates": c}, {"class": t["amenity"], "name": t["name"], "rank": 10})
        elif "shop" in t and "name" in t:
            feat("poi", {"type": "Point", "coordinates": c}, {"class": "shop", "name": t["name"], "rank": 12})
        if "addr:housenumber" in t:
            feat("housenumber", {"type": "Point", "coordinates": c}, {"housenumber": t["addr:housenumber"]})

    def way(self, w):
        t = w.tags
        try:
            coords = [[n.lon, n.lat] for n in w.nodes]
        except osmium.InvalidLocationError:
            return
        if len(coords) < 2:
            return
        closed = coords[0] == coords[-1] and len(coords) >= 4
        poly = {"type": "Polygon", "coordinates": [coords]}
        line = {"type": "LineString", "coordinates": coords}
        hw = t.get("highway")
        if hw in HW and t.get("area") != "yes":
            brunnel = "bridge" if t.get("bridge", "no") != "no" else ("tunnel" if t.get("tunnel", "no") != "no" else "")
            p = {"class": HW[hw]}
            if brunnel: p["brunnel"] = brunnel
            feat("transportation", line, p)
            if "name" in t or "ref" in t:
                q = dict(p); q["name"] = t.get("name"); q["ref"] = t.get("ref")
                feat("transportation_name", line, {k: v for k, v in q.items() if v})
        elif t.get("railway") in ("rail", "tram", "subway", "light_rail"):
            feat("transportation", line, {"class": "rail" if t["railway"] == "rail" else "transit"})
        elif closed and "building" in t:
            feat("building", poly, {})
        elif closed and (t.get("natural") == "water" or t.get("landuse") in ("reservoir", "basin")):
            feat("water", poly, {"class": "lake"})
        elif closed and t.get("leisure") in ("park", "garden"):
            feat("park", poly, {"class": "park"})
        elif closed and (t.get("landuse") == "grass" or t.get("natural") in ("wood", "scrub")):
            feat("landcover", poly, {"class": "grass"})
        elif "waterway" in t and not closed:
            feat("waterway", line, {"class": t["waterway"]})

H().apply_file(sys.argv[1], locations=True)
# The sea around central Helsinki (coastline polygons are not assembled here): a rough box south of the centre.
feat("water", {"type": "Polygon", "coordinates": [[[24.935, 60.1600], [24.9600, 60.1600], [24.9600, 60.16420],
     [24.9535, 60.1655], [24.9500, 60.1667], [24.935, 60.1600]]]}, {"class": "ocean"})
json.dump(layers, open(sys.argv[2], "w"))
print({k: len(v) for k, v in layers.items()})
