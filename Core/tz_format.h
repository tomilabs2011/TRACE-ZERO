/*
 * TRACE//ZERO binary file formats.
 *
 * This header is the single source of truth for the on-disk layout of the
 * routing and search files inside a `.vmap` package. It is shared by the
 * Mac-side builder (Forge/) and the reader in the app (Core/), so the two can
 * never disagree about a field.
 *
 * Conventions (see docs/FILE-FORMATS.md for the prose version):
 *   - little-endian, every section starts on an 8-byte boundary
 *   - coordinates are int32 micro-degrees ("E6", ~11 cm resolution)
 *   - durations and weights are uint32 deciseconds
 *   - strings live in a blob of NUL-terminated UTF-8; offset 0 is ""
 *   - files are memory-mapped read-only on the phone; nothing is parsed or
 *     copied at open time, so opening a country costs microseconds and the
 *     kernel can drop pages under memory pressure.
 */
#ifndef TZ_FORMAT_H
#define TZ_FORMAT_H

#include <stdint.h>

#define TZ_FORMAT_VERSION 1u

/* Magic strings (8 bytes, NUL padded). */
#define TZ_MAGIC_ROADS  "TZROADS"
#define TZ_MAGIC_GRAPH  "TZGRAPH"
#define TZ_MAGIC_SEARCH "TZSRCH"

#define TZ_MAX_SECTIONS 16

typedef struct {
    uint64_t off;   /* byte offset from start of file, multiple of 8 */
    uint64_t size;  /* byte size */
} tz_section;

/* Every file starts with this 512-byte header. */
typedef struct {
    char       magic[8];
    uint32_t   version;          /* TZ_FORMAT_VERSION */
    uint32_t   header_size;      /* sizeof(tz_file_header) */
    uint64_t   file_size;        /* total size, used to detect truncated copies */
    uint32_t   count[16];        /* file-specific counters, see below */
    int32_t    ival[16];         /* file-specific signed values, see below */
    tz_section sec[TZ_MAX_SECTIONS];
    uint8_t    reserved[512 - 8 - 4 - 4 - 8 - 64 - 64 - 16 * 16];
} tz_file_header;

/* ------------------------------------------------------------------------ */
/* roads.tzr : shared road geometry, names and spatial index                */
/* ------------------------------------------------------------------------ */

enum {
    TZR_C_NODES = 0,     /* number of junction nodes */
    TZR_C_SEGS,          /* number of segments */
    TZR_C_GRID_W,        /* spatial grid columns */
    TZR_C_GRID_H,        /* spatial grid rows */
    TZR_C_GRID_CELL      /* cell size in E6 units (same for lat and lon) */
};
enum {
    TZR_I_GRID_LAT0 = 0, /* south edge of grid, E6 */
    TZR_I_GRID_LON0,     /* west edge of grid, E6 */
    TZR_I_MINLAT, TZR_I_MINLON, TZR_I_MAXLAT, TZR_I_MAXLON /* data bbox, E6 */
};
enum {
    TZR_S_NODES = 0,     /* tz_point[n_nodes] */
    TZR_S_SEGS,          /* tz_seg[n_segs] */
    TZR_S_GEOM,          /* interior points: zigzag-varint deltas */
    TZR_S_STRINGS,       /* NUL-terminated UTF-8 blob */
    TZR_S_GRID_START,    /* uint32[w*h+1] index into GRID_SEGS */
    TZR_S_GRID_SEGS      /* uint32 segment ids */
};

typedef struct {
    int32_t lat, lon;    /* E6 */
} tz_point;

/* Road classes, ordered roughly by importance. */
enum {
    TZ_CLS_MOTORWAY = 0, TZ_CLS_TRUNK, TZ_CLS_PRIMARY, TZ_CLS_SECONDARY,
    TZ_CLS_TERTIARY, TZ_CLS_UNCLASSIFIED, TZ_CLS_RESIDENTIAL,
    TZ_CLS_LIVING_STREET, TZ_CLS_SERVICE, TZ_CLS_TRACK, TZ_CLS_CYCLEWAY,
    TZ_CLS_PATH, TZ_CLS_FOOTWAY, TZ_CLS_PEDESTRIAN, TZ_CLS_STEPS,
    TZ_CLS_BRIDLEWAY, TZ_CLS_ROAD, TZ_CLS_FERRY,
    TZ_CLS_COUNT
};

/* Segment flags. */
#define TZ_SEG_ROUNDABOUT 0x01
#define TZ_SEG_LINK       0x02   /* *_link ramp */
#define TZ_SEG_TUNNEL     0x04
#define TZ_SEG_BRIDGE     0x08
#define TZ_SEG_TOLL       0x10
#define TZ_SEG_FERRY      0x20
#define TZ_SEG_UNPAVED    0x40

/*
 * A segment is a piece of one OSM way between two junction nodes. Its
 * geometry is node a, then `npts` interior points, then node b. The interior
 * points are stored at byte offset `geom` of the GEOM section as zigzag
 * varints (dlat, dlon) relative to the previous point, starting at node a.
 */
typedef struct {
    uint32_t a, b;       /* junction node ids */
    uint32_t geom;       /* byte offset into GEOM */
    uint32_t name;       /* string offset, 0 = unnamed */
    uint32_t ref;        /* string offset of route ref (e.g. "M1"), 0 = none */
    uint32_t len_dm;     /* length in decimetres */
    uint16_t npts;       /* interior point count */
    uint8_t  cls;        /* TZ_CLS_* */
    uint8_t  flags;      /* TZ_SEG_* */
} tz_seg;                /* 28 bytes */

/* ------------------------------------------------------------------------ */
/* car.tzg / bike.tzg / foot.tzg : one contraction hierarchy per profile    */
/* ------------------------------------------------------------------------ */

enum { TZ_PROFILE_CAR = 0, TZ_PROFILE_BIKE = 1, TZ_PROFILE_FOOT = 2, TZ_PROFILE_COUNT };

enum {
    TZG_C_PROFILE = 0,   /* TZ_PROFILE_* */
    TZG_C_NODES,         /* CH nodes = road nodes + restriction copies */
    TZG_C_ROAD_NODES,    /* must equal roads file node count */
    TZG_C_SEGS,          /* must equal roads file segment count */
    TZG_C_EDGES,         /* CH edges */
    TZG_C_ARRIVE,        /* entries in ARRIVE */
    TZG_C_DEPART,        /* entries in DEPART */
    TZG_C_ROADS_CRC      /* checksum of roads header, ties the files together */
};
enum {
    TZG_S_SEGCOST = 0,   /* tz_segcost[n_segs] */
    TZG_S_FIRST,         /* uint32[n_nodes+1] CSR offsets by CH id */
    TZG_S_EDGES,         /* tz_chedge[n_edges] */
    TZG_S_NODE_ROAD,     /* uint32[n_nodes]  CH id -> road node id */
    TZG_S_ROAD_CH,       /* uint32[n_road_nodes] road node id -> CH id */
    TZG_S_ARRIVE,        /* tz_keynode[], sorted by key */
    TZG_S_DEPART,        /* tz_keynode[], sorted by key */
    TZG_S_NODE_DEG,      /* uint8[n_road_nodes] usable departures (maneuvers) */
    TZG_S_SEGFLAGS       /* uint8[n_segs] TZ_SF_* */
};

#define TZ_NO_ACCESS 0xFFFFFFFFu

/* Per-segment cost in one profile. TZ_NO_ACCESS = direction not allowed. */
typedef struct {
    uint32_t w_fwd, w_bwd;   /* routing weight a->b, b->a (deciseconds, with preferences) */
    uint32_t d_fwd, d_bwd;   /* real travel duration (deciseconds) */
} tz_segcost;

#define TZ_SF_ISLAND 0x01    /* segment belongs to a tiny disconnected component */

/*
 * Contraction hierarchy edge. CH node ids are ordered by rank (id 0 is the
 * least important), so every edge is stored at its lower-ranked endpoint and
 * `target` always has a higher id. One stored edge can serve the forward
 * search (travel this -> target) and/or the backward search
 * (travel target -> this).
 *
 *   wf bits 0..28  weight (deciseconds)
 *   wf bit 29      TZ_E_FWD      travel this -> target allowed
 *   wf bit 30      TZ_E_BWD      travel target -> this allowed
 *   wf bit 31      TZ_E_SHORTCUT payload is the middle CH node
 *   payload        shortcut: middle CH node id
 *                  original: (segment id << 1) | dir, where dir is the
 *                  geometric direction of travelling this -> target
 *                  (0 = segment a->b, 1 = b->a)
 */
typedef struct {
    uint32_t target;
    uint32_t wf;
    uint32_t payload;
} tz_chedge;

#define TZ_E_WMASK    0x1FFFFFFFu
#define TZ_E_FWD      0x20000000u
#define TZ_E_BWD      0x40000000u
#define TZ_E_SHORTCUT 0x80000000u

/*
 * Turn restrictions are modelled by node splitting. Travelling segment s in
 * direction d (key = s*2+d) normally arrives at the road node at its end.
 * If that arrival is restricted, ARRIVE maps the key to a copy node whose
 * outgoing edges are only the allowed ones. DEPART lists, for each key, the
 * copy nodes that may leave along it (used to seed the backward search).
 */
typedef struct {
    uint32_t key;
    uint32_t node;   /* CH id */
} tz_keynode;

/* ------------------------------------------------------------------------ */
/* search.tzs : offline geocoder                                            */
/* ------------------------------------------------------------------------ */

enum {
    TZS_C_ENTRIES = 0,
    TZS_C_TOKENS,
    TZS_C_GRID_W, TZS_C_GRID_H, TZS_C_GRID_CELL
};
enum { TZS_I_GRID_LAT0 = 0, TZS_I_GRID_LON0 };
enum {
    TZS_S_ENTRIES = 0,   /* tz_sentry[], ordered by importance (most important first) */
    TZS_S_STRINGS,       /* NUL-terminated UTF-8 blob */
    TZS_S_TOKENS,        /* tz_stoken[], sorted by token bytes */
    TZS_S_TOKSTR,        /* NUL-terminated normalised tokens */
    TZS_S_POSTINGS,      /* varint delta-coded ascending entry ids */
    TZS_S_GRID_START,    /* uint32[w*h+1] */
    TZS_S_GRID_IDS       /* uint32 entry ids */
};

enum {
    TZ_KIND_PLACE = 0,   /* city, town, village, suburb ... */
    TZ_KIND_STREET,
    TZ_KIND_ADDRESS,
    TZ_KIND_POI,
    TZ_KIND_COORD        /* only produced by the coordinate parser */
};

typedef struct {
    int32_t  lat, lon;   /* E6 */
    uint32_t name;       /* display name */
    uint32_t ctx;        /* display context ("Cafe · Budapest") */
    uint32_t extra;      /* extra normalised search words, may be 0 */
    uint16_t rank;       /* static importance 0..65535 */
    uint8_t  kind;       /* TZ_KIND_* */
    uint8_t  sub;        /* place type / POI group, see tz_core.h */
} tz_sentry;             /* 24 bytes */

typedef struct {
    uint32_t str;        /* offset into TOKSTR */
    uint32_t post;       /* byte offset into POSTINGS */
    uint32_t count;      /* number of entry ids */
} tz_stoken;

#endif /* TZ_FORMAT_H */
