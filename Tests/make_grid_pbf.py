#!/usr/bin/env python3
"""Write a synthetic street-grid city as an OSM PBF file.

Used to test the builder and router at "big city" scale without
downloading data:

    python3 make_grid_pbf.py out.osm.pbf --size 1000

creates a size x size grid of junctions (~100 m apart) with:
  * residential streets, every 10th street a primary road (50 km/h)
  * alternating one-way streets
  * a no_left_turn restriction at every 7th primary/primary crossing
  * one interior shape point on every block
Only the standard library is used.
"""
import argparse
import struct
import zlib


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def zz(v):
    return (v << 1) ^ (v >> 63)


def key(field, wt):
    return varint((field << 3) | wt)


def f_bytes(field, data):
    return key(field, 2) + varint(len(data)) + data


def f_varint(field, v):
    return key(field, 0) + varint(v)


def packed(field, values, signed=False):
    body = b"".join(varint(zz(v) if signed else v) for v in values)
    return f_bytes(field, body)


class StringTable:
    def __init__(self):
        self.index = {"": 0}
        self.items = [b""]

    def get(self, s):
        if s not in self.index:
            self.index[s] = len(self.items)
            self.items.append(s.encode())
        return self.index[s]

    def encode(self):
        return b"".join(f_bytes(1, s) for s in self.items)


def write_blob(f, kind, payload):
    blob = f_varint(2, len(payload)) + f_bytes(3, zlib.compress(payload, 6))
    header = f_bytes(1, kind.encode()) + f_varint(3, len(blob))
    f.write(struct.pack(">I", len(header)))
    f.write(header)
    f.write(blob)


def node_block(nodes):
    """nodes: list of (id, lat, lon) with lat/lon in degrees"""
    st = StringTable()
    ids, lats, lons = [], [], []
    pid = plat = plon = 0
    for nid, lat, lon in nodes:
        la = int(round(lat * 1e7))  # granularity 100 nanodegrees
        lo = int(round(lon * 1e7))
        ids.append(nid - pid)
        lats.append(la - plat)
        lons.append(lo - plon)
        pid, plat, plon = nid, la, lo
    dense = packed(1, ids, True) + packed(8, lats, True) + packed(9, lons, True)
    group = f_bytes(2, dense)
    return f_bytes(1, st.encode()) + f_bytes(2, group)


def way_block(ways):
    """ways: list of (id, refs, tags dict)"""
    st = StringTable()
    body = b""
    for wid, refs, tags in ways:
        keys = [st.get(k) for k in tags]
        vals = [st.get(v) for v in tags.values()]
        deltas, prev = [], 0
        for r in refs:
            deltas.append(r - prev)
            prev = r
        w = f_varint(1, wid) + packed(2, keys) + packed(3, vals) + packed(8, deltas, True)
        body += f_bytes(3, w)
    return f_bytes(1, st.encode()) + f_bytes(2, body)


def rel_block(rels):
    """rels: list of (id, members [(type, ref, role)], tags)"""
    st = StringTable()
    body = b""
    for rid, members, tags in rels:
        keys = [st.get(k) for k in tags]
        vals = [st.get(v) for v in tags.values()]
        roles = [st.get(m[2]) for m in members]
        mids, prev = [], 0
        for m in members:
            mids.append(m[1] - prev)
            prev = m[1]
        types = [m[0] for m in members]
        r = (f_varint(1, rid) + packed(2, keys) + packed(3, vals) + packed(8, roles)
             + packed(9, mids, True) + packed(10, types))
        body += f_bytes(4, r)
    return f_bytes(1, st.encode()) + f_bytes(2, body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=300)
    args = ap.parse_args()
    n = args.size
    lat0, lon0, dlat, dlon = 47.0, 19.0, 0.0009, 0.0013

    def jid(i, j):  # junction node id
        return 1 + i * n + j

    shape_base = 1 + n * n

    def hid(i, j):  # shape point east of junction (i, j)
        return shape_base + i * n + j

    def vid(i, j):  # shape point north of junction (i, j)
        return shape_base + n * n + i * n + j

    with open(args.out, "wb") as f:
        header = f_bytes(4, b"OsmSchema-V0.6") + f_bytes(4, b"DenseNodes") + f_bytes(16, b"make_grid_pbf")
        write_blob(f, "OSMHeader", header)

        nodes = []

        def flush_nodes():
            if nodes:
                write_blob(f, "OSMData", node_block(nodes))
                nodes.clear()

        # node ids must be ascending: junctions, then east shape points, then north
        for i in range(n):
            for j in range(n):
                nodes.append((jid(i, j), lat0 + i * dlat, lon0 + j * dlon))
                if len(nodes) >= 8000:
                    flush_nodes()
        for i in range(n):
            for j in range(n):
                nodes.append((hid(i, j), lat0 + i * dlat + dlat * 0.05, lon0 + (j + 0.5) * dlon))
                if len(nodes) >= 8000:
                    flush_nodes()
        for i in range(n):
            for j in range(n):
                nodes.append((vid(i, j), lat0 + (i + 0.5) * dlat, lon0 + j * dlon + dlon * 0.05))
                if len(nodes) >= 8000:
                    flush_nodes()
        flush_nodes()

        ways = []

        def flush_ways():
            if ways:
                write_blob(f, "OSMData", way_block(ways))
                ways.clear()

        def street_tags(k, axis):
            tags = {}
            if k % 10 == 0:
                tags["highway"] = "primary"
                tags["maxspeed"] = "50"
                tags["name"] = f"{axis} Avenue {k}"
            else:
                tags["highway"] = "residential"
                tags["name"] = f"{axis} Street {k}"
                if k % 3 == 1:
                    tags["oneway"] = "yes"
                elif k % 3 == 2:
                    tags["oneway"] = "-1"
            return tags

        way_id = 1
        for i in range(n):  # east-west streets
            refs = []
            for j in range(n):
                refs.append(jid(i, j))
                if j + 1 < n:
                    refs.append(hid(i, j))
            ways.append((way_id, refs, street_tags(i, "East")))
            way_id += 1
            if len(ways) >= 4000:
                flush_ways()
        for j in range(n):  # north-south streets
            refs = []
            for i in range(n):
                refs.append(jid(i, j))
                if i + 1 < n:
                    refs.append(vid(i, j))
            ways.append((way_id, refs, street_tags(j, "North")))
            way_id += 1
            if len(ways) >= 4000:
                flush_ways()
        flush_ways()

        rels = []
        rid = 1
        for i in range(0, n, 10):
            for j in range(0, n, 10):
                if (i // 10 + j // 10) % 7 == 0 and 0 < i < n - 1 and 0 < j < n - 1:
                    rels.append((rid, [(1, 1 + i, "from"), (0, jid(i, j), "via"), (1, 1 + n + j, "to")],
                                 {"type": "restriction", "restriction": "no_left_turn"}))
                    rid += 1
        if rels:
            write_blob(f, "OSMData", rel_block(rels))


if __name__ == "__main__":
    main()
