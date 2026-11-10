/* roads.tzr reader: geometry, names and the spatial grid. */
#include "tz_private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

tz_roads *tz_roads_open(const char *path, int *status)
{
    int st_dummy;
    if (!status) status = &st_dummy;
    tz_roads *r = (tz_roads *)calloc(1, sizeof *r);
    if (!r) { *status = TZ_ERR_NOMEM; return NULL; }
    int st = tz_map_file(path, TZ_MAGIC_ROADS, &r->m);
    if (st != TZ_OK) { free(r); *status = st; return NULL; }

    const tz_file_header *h = r->m.hdr;
    r->n_nodes = h->count[TZR_C_NODES];
    r->n_segs = h->count[TZR_C_SEGS];
    r->gw = h->count[TZR_C_GRID_W];
    r->gh = h->count[TZR_C_GRID_H];
    r->cell = h->count[TZR_C_GRID_CELL];
    r->lat0 = h->ival[TZR_I_GRID_LAT0];
    r->lon0 = h->ival[TZR_I_GRID_LON0];
    r->nodes = (const tz_point *)tz_section_ptr(&r->m, TZR_S_NODES);
    r->segs = (const tz_seg *)tz_section_ptr(&r->m, TZR_S_SEGS);
    r->geom = (const uint8_t *)tz_section_ptr(&r->m, TZR_S_GEOM);
    r->geom_size = tz_section_size(&r->m, TZR_S_GEOM);
    r->str = (const char *)tz_section_ptr(&r->m, TZR_S_STRINGS);
    r->str_size = tz_section_size(&r->m, TZR_S_STRINGS);
    r->grid_start = (const uint32_t *)tz_section_ptr(&r->m, TZR_S_GRID_START);
    r->grid_segs = (const uint32_t *)tz_section_ptr(&r->m, TZR_S_GRID_SEGS);
    r->n_grid_segs = tz_section_size(&r->m, TZR_S_GRID_SEGS) / 4;
    r->checksum = tz_header_checksum(h);

    uint64_t cells = (uint64_t)r->gw * r->gh;
    int ok = 1;
    ok &= tz_section_size(&r->m, TZR_S_NODES) == (uint64_t)r->n_nodes * sizeof(tz_point);
    ok &= tz_section_size(&r->m, TZR_S_SEGS) == (uint64_t)r->n_segs * sizeof(tz_seg);
    ok &= r->str_size >= 1 && r->str && r->str[0] == 0 && r->str[r->str_size - 1] == 0;
    ok &= r->cell > 0 && r->gw > 0 && r->gh > 0 && cells < (1u << 28);
    ok &= tz_section_size(&r->m, TZR_S_GRID_START) == (cells + 1) * 4;
    ok &= (tz_section_size(&r->m, TZR_S_GRID_SEGS) % 4) == 0;
    if (!ok) {
        tz_unmap_file(&r->m);
        free(r);
        *status = TZ_ERR_FORMAT;
        return NULL;
    }
    *status = TZ_OK;
    return r;
}

void tz_roads_close(tz_roads *r)
{
    if (!r) return;
    tz_unmap_file(&r->m);
    free(r);
}

void tz_roads_bbox(const tz_roads *r, double out[4])
{
    const tz_file_header *h = r->m.hdr;
    out[0] = h->ival[TZR_I_MINLAT] / 1e6;
    out[1] = h->ival[TZR_I_MINLON] / 1e6;
    out[2] = h->ival[TZR_I_MAXLAT] / 1e6;
    out[3] = h->ival[TZR_I_MAXLON] / 1e6;
}

uint32_t tz_roads_segment_count(const tz_roads *r) { return r->n_segs; }

const char *tz_roads_str(const tz_roads *r, uint32_t off)
{
    if (off >= r->str_size) return "";
    return r->str + off;
}

int tz_roads_seg_points(const tz_roads *r, uint32_t seg, tz_vec *pts)
{
    pts->n = 0;
    if (seg >= r->n_segs) return -1;
    const tz_seg *s = &r->segs[seg];
    if (s->a >= r->n_nodes || s->b >= r->n_nodes) return -1;
    uint32_t total = (uint32_t)s->npts + 2;
    if (!tz_vec_reserve(pts, total)) return -1;
    tz_point *out = (tz_point *)pts->p;
    out[0] = r->nodes[s->a];
    if (s->npts) {
        if (!r->geom || s->geom >= r->geom_size) return -1;
        const uint8_t *p = r->geom + s->geom, *end = r->geom + r->geom_size;
        int32_t lat = out[0].lat, lon = out[0].lon;
        for (uint32_t i = 0; i < s->npts; i++) {
            uint32_t a, b;
            if (p >= end) return -1;
            p = tz_varint_get(p, end, &a);
            if (p >= end) return -1;
            p = tz_varint_get(p, end, &b);
            lat += tz_zigzag_dec(a);
            lon += tz_zigzag_dec(b);
            out[i + 1].lat = lat;
            out[i + 1].lon = lon;
        }
    }
    out[total - 1] = r->nodes[s->b];
    pts->n = total;
    return (int)total;
}

void tz_roads_visit_box(const tz_roads *r, double minlat, double minlon,
                        double maxlat, double maxlon, tz_seg_visitor fn, void *ctx)
{
    double c = (double)r->cell;
    int64_t r0 = (int64_t)floor((minlat * 1e6 - r->lat0) / c);
    int64_t r1 = (int64_t)floor((maxlat * 1e6 - r->lat0) / c);
    int64_t c0 = (int64_t)floor((minlon * 1e6 - r->lon0) / c);
    int64_t c1 = (int64_t)floor((maxlon * 1e6 - r->lon0) / c);
    if (r1 < 0 || c1 < 0 || r0 >= (int64_t)r->gh || c0 >= (int64_t)r->gw) return;
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 >= (int64_t)r->gh) r1 = r->gh - 1;
    if (c1 >= (int64_t)r->gw) c1 = r->gw - 1;
    for (int64_t row = r0; row <= r1; row++) {
        for (int64_t col = c0; col <= c1; col++) {
            uint64_t cell = (uint64_t)row * r->gw + (uint64_t)col;
            uint32_t s = r->grid_start[cell], e = r->grid_start[cell + 1];
            if (e > r->n_grid_segs || s > e) continue;
            for (uint32_t i = s; i < e; i++) {
                uint32_t seg = r->grid_segs[i];
                if (seg >= r->n_segs) continue;
                if (!fn(ctx, seg)) return;
            }
        }
    }
}

/* ------------------------------------------------------ nearest segment */

#define NEAREST_MAX_K 8

typedef struct {
    const tz_roads *r;
    tz_proj         proj;
    double          heading;
    tz_seg_filter   accept;
    const void     *actx;
    tz_vec          pts;
    int             k, n;              /* wanted, found */
    double          score[NEAREST_MAX_K];
    tz_snap         best[NEAREST_MAX_K];
} nearest_ctx;

static int nearest_visit(void *vctx, uint32_t seg)
{
    nearest_ctx *c = (nearest_ctx *)vctx;
    /* A segment is listed in every grid cell it touches: evaluate it once. */
    for (int i = 0; i < c->n; i++) if (c->best[i].seg == seg) return 1;
    int fwd_ok = 1, bwd_ok = 1;
    if (c->accept && !c->accept(c->actx, seg, &fwd_ok, &bwd_ok)) return 1;
    int n = tz_roads_seg_points(c->r, seg, &c->pts);
    if (n < 2) return 1;
    const tz_point *p = (const tz_point *)c->pts.p;

    /* projected coordinates, query point at the origin */
    double total = 0.0, best_d = 1e300, best_along = 0.0, best_bearing = 0.0;
    double best_x = 0, best_y = 0;
    double ax, ay;
    tz_proj_xy(&c->proj, p[0].lat / 1e6, p[0].lon / 1e6, &ax, &ay);
    for (int i = 1; i < n; i++) {
        double bx, by, t;
        tz_proj_xy(&c->proj, p[i].lat / 1e6, p[i].lon / 1e6, &bx, &by);
        double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        double d = tz_point_seg_dist(0.0, 0.0, ax, ay, bx, by, &t);
        if (d < best_d) {
            best_d = d;
            best_along = total + t * len;
            best_x = ax + t * (bx - ax);
            best_y = ay + t * (by - ay);
            best_bearing = (len > 0.0) ? fmod(atan2(bx - ax, by - ay) / TZ_DEG2RAD + 360.0, 360.0) : 0.0;
        }
        total += len;
        ax = bx;
        ay = by;
    }
    double score = best_d;
    if (c->heading >= 0.0) {
        double diff = 360.0;
        if (fwd_ok) diff = fabs(tz_norm_angle(best_bearing - c->heading));
        if (bwd_ok) {
            double d2 = fabs(tz_norm_angle(best_bearing + 180.0 - c->heading));
            if (d2 < diff) diff = d2;
        }
        if (diff > 90.0) score += 30.0;
        else if (diff > 45.0) score += 10.0;
    }
    if (c->n == c->k && score >= c->score[c->n - 1]) return 1;
    int i = c->n < c->k ? c->n++ : c->k - 1;
    while (i > 0 && c->score[i - 1] > score) { c->score[i] = c->score[i - 1]; c->best[i] = c->best[i - 1]; i--; }
    c->score[i] = score;
    tz_snap *b = &c->best[i];
    b->seg = seg;
    b->dist_m = best_d;
    b->bearing = best_bearing;
    b->frac = (total > 0.0) ? best_along / total : 0.0;
    b->lat = c->proj.lat0 + best_y / c->proj.ky;
    b->lon = c->proj.lon0 + best_x / c->proj.kx;
    return 1;
}

int tz_roads_nearest_k(const tz_roads *r, double lat, double lon, double radius_m,
                       double heading, tz_seg_filter accept, const void *actx,
                       tz_snap *out, int k)
{
    if (!(radius_m > 0.0) || !isfinite(lat) || !isfinite(lon) || k < 1) return 0;
    if (k > NEAREST_MAX_K) k = NEAREST_MAX_K;
    nearest_ctx c;
    memset(&c, 0, sizeof c);
    c.r = r;
    c.heading = heading;
    c.accept = accept;
    c.actx = actx;
    c.k = k;
    tz_proj_init(&c.proj, lat, lon);
    tz_vec_init(&c.pts, sizeof(tz_point));

    /* Search a small box first and grow it; stop as soon as the k best
     * candidates are certainly inside the searched radius. */
    const double steps[3] = { 100.0, 400.0, radius_m };
    double coslat = cos(lat * TZ_DEG2RAD);
    if (coslat < 0.01) coslat = 0.01;
    for (int i = 0; i < 3; i++) {
        double rad = steps[i] < radius_m ? steps[i] : radius_m;
        double dlat = rad / 111195.0, dlon = rad / (111195.0 * coslat);
        c.n = 0;
        tz_roads_visit_box(r, lat - dlat, lon - dlon, lat + dlat, lon + dlon, nearest_visit, &c);
        if ((c.n == k && c.score[k - 1] <= rad) || rad >= radius_m) break;
    }
    int found = 0;
    for (int i = 0; i < c.n; i++) if (c.best[i].dist_m <= radius_m) out[found++] = c.best[i];
    tz_vec_free(&c.pts);
    return found;
}

int tz_roads_nearest(const tz_roads *r, double lat, double lon, double radius_m,
                     double heading, tz_seg_filter accept, const void *actx,
                     tz_snap *out)
{
    return tz_roads_nearest_k(r, lat, lon, radius_m, heading, accept, actx, out, 1);
}

static int named_filter(const void *ctx, uint32_t seg, int *fwd_ok, int *bwd_ok)
{
    const tz_roads *r = (const tz_roads *)ctx;
    *fwd_ok = *bwd_ok = 1;
    const tz_seg *s = &r->segs[seg];
    return s->name != 0 || s->ref != 0;
}

const char *tz_roads_nearest_name(const tz_roads *r, double lat, double lon,
                                  double radius_m, double *dist_m)
{
    tz_snap s;
    if (!tz_roads_nearest(r, lat, lon, radius_m, -1.0, named_filter, r, &s)) return NULL;
    if (dist_m) *dist_m = s.dist_m;
    const tz_seg *seg = &r->segs[s.seg];
    return seg->name ? tz_roads_str(r, seg->name) : tz_roads_str(r, seg->ref);
}
