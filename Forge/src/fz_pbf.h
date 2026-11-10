/* Minimal OpenStreetMap PBF reader (https://wiki.openstreetmap.org/wiki/PBF_Format).
 * Only zlib-compressed and raw blobs are supported, which covers every
 * extract published by Geofabrik, BBBike and planet.openstreetmap.org. */
#ifndef FZ_PBF_H
#define FZ_PBF_H

#include <stdint.h>

typedef struct { const char *k, *v; } fz_tag;

typedef struct {
    int64_t     ref;
    int         type;      /* 0 node, 1 way, 2 relation */
    const char *role;
} fz_member;

/* Coordinates are given in nanodegrees. */
typedef struct {
    void *ctx;
    int   want_nodes;          /* 0: skip, 1: tagged only, 2: all nodes */
    int   want_ways;
    int   want_relations;
    void (*node)(void *ctx, int64_t id, int64_t lat_nd, int64_t lon_nd, const fz_tag *t, int nt);
    void (*way)(void *ctx, int64_t id, const int64_t *refs, int nref, const fz_tag *t, int nt);
    void (*relation)(void *ctx, int64_t id, const fz_member *m, int nm, const fz_tag *t, int nt);
} fz_pbf_handler;

typedef struct {
    int64_t replication_timestamp;   /* seconds since epoch, 0 if unknown */
    int     has_bbox;
    double  minlat, minlon, maxlat, maxlon;
    char    source[256];
} fz_pbf_info;

/* Read the whole file, calling the handler for each element in file order.
 * Exits with an error message on malformed input. */
void fz_pbf_read(const char *path, fz_pbf_handler *h, fz_pbf_info *info);

#endif
