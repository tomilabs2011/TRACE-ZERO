/* Private structure definitions shared by tz_roads.c, tz_route.c, tz_search.c */
#ifndef TZ_PRIVATE_H
#define TZ_PRIVATE_H

#include "tz_internal.h"

struct tz_roads {
    tz_mapped        m;
    const tz_point  *nodes;
    const tz_seg    *segs;
    const uint8_t   *geom;
    const char      *str;
    const uint32_t  *grid_start;
    const uint32_t  *grid_segs;
    uint64_t         geom_size, str_size, n_grid_segs;
    uint32_t         n_nodes, n_segs;
    uint32_t         gw, gh, cell;
    int32_t          lat0, lon0;
    uint32_t         checksum;
};

struct tz_graph {
    tz_mapped          m;
    const tz_roads    *roads;
    int                profile;
    uint32_t           n_nodes, n_road_nodes, n_segs, n_edges, n_arrive, n_depart;
    const tz_segcost  *cost;
    const uint32_t    *first;
    const tz_chedge   *edges;
    const uint32_t    *node_road;
    const uint32_t    *road_ch;
    const tz_keynode  *arrive;
    const tz_keynode  *depart;
    const uint8_t     *deg;
    const uint8_t     *sflags;
};

/* String at offset, "" if out of range. */
const char *tz_roads_str(const tz_roads *r, uint32_t off);

/* Decode the full geometry of a segment (a, interior..., b) into pts.
 * Returns the number of points or -1 if the segment is invalid. */
int tz_roads_seg_points(const tz_roads *r, uint32_t seg, tz_vec *pts /* tz_point */);

/* Visit every segment id listed in grid cells overlapping the box.
 * A segment may be visited more than once. Returns 0 to stop early. */
typedef int (*tz_seg_visitor)(void *ctx, uint32_t seg);
void tz_roads_visit_box(const tz_roads *r, double minlat, double minlon,
                        double maxlat, double maxlon, tz_seg_visitor fn, void *ctx);

/* Generic nearest-segment search used by snapping and reverse geocoding.
 * accept(ctx, seg) filters segments; heading < 0 disables the heading bias. */
typedef int (*tz_seg_filter)(const void *ctx, uint32_t seg, int *fwd_ok, int *bwd_ok);
int tz_roads_nearest(const tz_roads *r, double lat, double lon, double radius_m,
                     double heading, tz_seg_filter accept, const void *ctx,
                     tz_snap *out);
/* Up to k nearest distinct segments, best first. Returns how many. */
int tz_roads_nearest_k(const tz_roads *r, double lat, double lon, double radius_m,
                       double heading, tz_seg_filter accept, const void *ctx,
                       tz_snap *out, int k);

/* Diagnostics for the builder's `verify` command: shortest weight from road
 * node src to road node dst (arriving in any turn-restriction state).
 * Returns 1 if connected. */
int tz__ch_node_distance(const tz_graph *g, uint32_t src_road, uint32_t dst_road, uint64_t *dist);

#endif
