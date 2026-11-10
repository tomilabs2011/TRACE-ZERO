/* Shared state of one `tzforge build` run. */
#ifndef FZ_BUILD_H
#define FZ_BUILD_H

#include "fz_util.h"
#include "fz_osm.h"
#include "fz_pbf.h"

#define FZ_MISSING INT32_MIN

typedef struct {
    int64_t     id;
    uint64_t    ref0;      /* first index into refs */
    uint32_t    nref;
    uint32_t    name, ref; /* offsets in the roads string pool */
    double      len_m;     /* total length of its segments */
    fz_way_attr attr;
} fz_rway;

typedef struct {
    uint32_t way;          /* index into rways */
    uint32_t a, b;         /* road node ids */
    uint32_t r0, r1;       /* inclusive ref range inside the way */
    double   len_m;
    uint8_t  sig_mid;      /* traffic signals in the interior */
} fz_bseg;

typedef struct {
    int64_t from_way, to_way, via;
    uint8_t mask, kind, turn;
} fz_restr;

typedef struct {
    int32_t  lat, lon;     /* E6; FZ_MISSING until resolved */
    double   pop;
    uint32_t name;         /* search pool */
    int8_t   type;         /* TZ_PLACE_* */
} fz_place;

typedef struct {
    int32_t  lat, lon;     /* E6, FZ_MISSING for ways until pass 2 */
    uint64_t ref0;         /* for ways: centroid from these refs */
    uint32_t nref;
    uint32_t name, street, hn, postcode, city, label, extra; /* search pool */
    uint16_t rank;
    uint8_t  kind, sub;
} fz_sitem;

typedef struct {
    /* options */
    const char *input, *outdir, *pack_name, *pack_id, *source_url;
    int         profiles;          /* bit mask of TZ_PROFILE_* */
    int         with_search;

    fz_pbf_info info;

    /* pass 1 */
    FZ_VEC(int64_t)  refs;         /* OSM node ids of all kept ways */
    FZ_VEC(fz_rway)  rways;
    FZ_VEC(fz_restr) restr;
    FZ_VEC(int64_t)  signal_ids;
    FZ_VEC(int64_t)  barrier_ids;
    FZ_VEC(uint8_t)  barrier_masks;
    FZ_VEC(fz_place) places;
    FZ_VEC(fz_sitem) sitems;
    fz_pool          rpool;        /* roads strings */
    fz_pool          spool;        /* search strings */

    /* pass 2 */
    int64_t  *need;                /* sorted unique node ids */
    size_t    n_need;
    int32_t  *nlat, *nlon;         /* E6 per need index */
    uint32_t *refidx;              /* refs converted to need indices */
    size_t    pass2_cursor;

    /* segmentation */
    uint32_t *roadid;              /* need index -> road node id or UINT32_MAX */
    uint8_t  *nflags;              /* need index: bit0 signal, bits 1..3 barrier mask */
    uint32_t  n_road;
    int32_t  *road_lat, *road_lon;
    uint32_t *road_need;           /* road node id -> need index */
    FZ_VEC(fz_bseg) segs;
    uint32_t *way_seg_first;       /* CSR rway -> segments */
    uint32_t *way_segs;
    uint32_t *node_seg_first;      /* CSR road node -> incident segments */
    uint32_t *node_segs;
    int32_t   bbox[4];             /* minlat, minlon, maxlat, maxlon E6 */
    uint32_t  roads_crc;
    fz_map64  way_index;           /* OSM way id -> rway index */
} fz_build;

void fz_build_read(fz_build *b);           /* passes 1 and 2 */
void fz_build_segments(fz_build *b);
void fz_build_write_roads(fz_build *b, const char *path);
void fz_build_profile(fz_build *b, int profile, const char *path);
void fz_build_search(fz_build *b, const char *path);

/* helpers shared between stages */
static inline uint8_t fz_node_barrier(const fz_build *b, uint32_t need_idx, int profile)
{
    return (uint8_t)((b->nflags[need_idx] >> 1) & (1u << profile));
}

#endif
