/*
 * Route queries on a contraction hierarchy (CH).
 *
 * A CH answers shortest-path queries by searching "upwards" from both ends
 * through increasingly important nodes; a query on a whole country settles a
 * few hundred to a few thousand nodes instead of millions, which is why a
 * route across Budapest takes milliseconds on an iPhone 5s. See
 * docs/ROUTING.md for the full description.
 */
#include "tz_private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define SEED_MARK 0xFFFFFFF0u   /* nodeent.edge for search roots: SEED_MARK | dir */
#define DEFAULT_RADIUS 1500.0

/* ---------------------------------------------------------------- open */

tz_graph *tz_graph_open(const tz_roads *roads, const char *path, int *status)
{
    int st_dummy;
    if (!status) status = &st_dummy;
    if (!roads) { *status = TZ_ERR_ARGUMENT; return NULL; }
    tz_graph *g = (tz_graph *)calloc(1, sizeof *g);
    if (!g) { *status = TZ_ERR_NOMEM; return NULL; }
    int st = tz_map_file(path, TZ_MAGIC_GRAPH, &g->m);
    if (st != TZ_OK) { free(g); *status = st; return NULL; }

    const tz_file_header *h = g->m.hdr;
    g->roads = roads;
    g->profile = (int)h->count[TZG_C_PROFILE];
    g->n_nodes = h->count[TZG_C_NODES];
    g->n_road_nodes = h->count[TZG_C_ROAD_NODES];
    g->n_segs = h->count[TZG_C_SEGS];
    g->n_edges = h->count[TZG_C_EDGES];
    g->n_arrive = h->count[TZG_C_ARRIVE];
    g->n_depart = h->count[TZG_C_DEPART];

    if (h->count[TZG_C_ROADS_CRC] != roads->checksum ||
        g->n_road_nodes != roads->n_nodes || g->n_segs != roads->n_segs) {
        tz_unmap_file(&g->m);
        free(g);
        *status = TZ_ERR_MISMATCH;
        return NULL;
    }

    g->cost = (const tz_segcost *)tz_section_ptr(&g->m, TZG_S_SEGCOST);
    g->first = (const uint32_t *)tz_section_ptr(&g->m, TZG_S_FIRST);
    g->edges = (const tz_chedge *)tz_section_ptr(&g->m, TZG_S_EDGES);
    g->node_road = (const uint32_t *)tz_section_ptr(&g->m, TZG_S_NODE_ROAD);
    g->road_ch = (const uint32_t *)tz_section_ptr(&g->m, TZG_S_ROAD_CH);
    g->arrive = (const tz_keynode *)tz_section_ptr(&g->m, TZG_S_ARRIVE);
    g->depart = (const tz_keynode *)tz_section_ptr(&g->m, TZG_S_DEPART);
    g->deg = (const uint8_t *)tz_section_ptr(&g->m, TZG_S_NODE_DEG);
    g->sflags = (const uint8_t *)tz_section_ptr(&g->m, TZG_S_SEGFLAGS);

    int ok = g->profile >= 0 && g->profile < TZ_PROFILE_COUNT;
    ok &= g->n_nodes >= g->n_road_nodes;
    ok &= tz_section_size(&g->m, TZG_S_SEGCOST) == (uint64_t)g->n_segs * sizeof(tz_segcost);
    ok &= tz_section_size(&g->m, TZG_S_FIRST) == ((uint64_t)g->n_nodes + 1) * 4;
    ok &= tz_section_size(&g->m, TZG_S_EDGES) == (uint64_t)g->n_edges * sizeof(tz_chedge);
    ok &= tz_section_size(&g->m, TZG_S_NODE_ROAD) == (uint64_t)g->n_nodes * 4;
    ok &= tz_section_size(&g->m, TZG_S_ROAD_CH) == (uint64_t)g->n_road_nodes * 4;
    ok &= tz_section_size(&g->m, TZG_S_ARRIVE) == (uint64_t)g->n_arrive * sizeof(tz_keynode);
    ok &= tz_section_size(&g->m, TZG_S_DEPART) == (uint64_t)g->n_depart * sizeof(tz_keynode);
    ok &= tz_section_size(&g->m, TZG_S_NODE_DEG) == (uint64_t)g->n_road_nodes;
    ok &= tz_section_size(&g->m, TZG_S_SEGFLAGS) == (uint64_t)g->n_segs;
    ok &= g->n_segs < 0x7FFFFFFFu;
    if (ok && g->n_nodes) ok &= g->first[0] == 0 && g->first[g->n_nodes] == g->n_edges;
    if (!ok) {
        tz_unmap_file(&g->m);
        free(g);
        *status = TZ_ERR_FORMAT;
        return NULL;
    }
    /* The search jumps around the file; read-ahead would only waste RAM.
     * The advice is optional, so a failure is ignored. */
    (void)posix_madvise((void *)g->m.base, g->m.size, POSIX_MADV_RANDOM);
    *status = TZ_OK;
    return g;
}

void tz_graph_close(tz_graph *g)
{
    if (!g) return;
    tz_unmap_file(&g->m);
    free(g);
}

int tz_graph_profile(const tz_graph *g) { return g->profile; }

/* ------------------------------------------------------------- helpers */

static int keynode_find(const tz_keynode *a, uint32_t n, uint32_t key, uint32_t *lo_out)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a[mid].key < key) lo = mid + 1; else hi = mid;
    }
    *lo_out = lo;
    return lo < n && a[lo].key == key;
}

/* CH node reached after travelling segment `seg` in direction `dir`. */
static uint32_t arrive_node(const tz_graph *g, uint32_t seg, int dir)
{
    uint32_t i;
    if (g->n_arrive && keynode_find(g->arrive, g->n_arrive, seg * 2u + (uint32_t)dir, &i))
        return g->arrive[i].node;
    const tz_seg *s = &g->roads->segs[seg];
    uint32_t end = dir == 0 ? s->b : s->a;
    return end < g->n_road_nodes ? g->road_ch[end] : UINT32_MAX;
}

static uint32_t seg_weight(const tz_graph *g, uint32_t seg, int dir)
{
    return dir == 0 ? g->cost[seg].w_fwd : g->cost[seg].w_bwd;
}

static uint32_t seg_duration(const tz_graph *g, uint32_t seg, int dir)
{
    return dir == 0 ? g->cost[seg].d_fwd : g->cost[seg].d_bwd;
}

typedef struct { const tz_graph *g; int allow_islands; } snap_filter_ctx;

static int graph_filter(const void *vctx, uint32_t seg, int *fwd_ok, int *bwd_ok)
{
    const snap_filter_ctx *c = (const snap_filter_ctx *)vctx;
    const tz_graph *g = c->g;
    *fwd_ok = g->cost[seg].w_fwd != TZ_NO_ACCESS;
    *bwd_ok = g->cost[seg].w_bwd != TZ_NO_ACCESS;
    if (!*fwd_ok && !*bwd_ok) return 0;
    if (!c->allow_islands && (g->sflags[seg] & TZ_SF_ISLAND)) return 0;
    return 1;
}

static int snap_heading(const tz_graph *g, double lat, double lon, double radius,
                        double heading, tz_snap *out)
{
    snap_filter_ctx c = { g, 0 };
    if (tz_roads_nearest(g->roads, lat, lon, radius, heading, graph_filter, &c, out)) return 1;
    c.allow_islands = 1;
    return tz_roads_nearest(g->roads, lat, lon, radius, heading, graph_filter, &c, out);
}

int tz_snap_point(const tz_graph *g, double lat, double lon, double radius_m, tz_snap *out)
{
    if (!g || !out) return 0;
    return snap_heading(g, lat, lon, radius_m > 0 ? radius_m : DEFAULT_RADIUS, -1.0, out);
}

/* ---------------------------------------------------------------- search */

typedef struct {
    tz_nodemap map;
    tz_heap    heap;
} side;

static int seed(side *s, uint32_t node, double cost_ds, int dir)
{
    if (node == UINT32_MAX) return 1;
    double c = cost_ds < 0 ? 0 : cost_ds;
    if (c > (double)TZ_E_WMASK) c = (double)TZ_E_WMASK;
    uint32_t d = (uint32_t)(c + 0.5);
    tz_nodeent *e = tz_nodemap_put(&s->map, node);
    if (!e) return 0;
    if (d < e->dist) {
        e->dist = d;
        e->parent = UINT32_MAX;
        e->edge = SEED_MARK | (uint32_t)dir;
        if (!tz_heap_push(&s->heap, d, node)) return 0;
    }
    return 1;
}

/* Returns 1 if a meeting node was found, 0 if not, -1 on allocation failure. */
static int ch_search(const tz_graph *g, side *F, side *B, uint32_t *meet_out,
                     uint64_t *best_out, uint32_t *settled_out)
{
    uint64_t best = UINT64_MAX;
    uint32_t meet = UINT32_MAX, settled = 0;
    for (;;) {
        uint64_t fmin = tz_heap_min(&F->heap), bmin = tz_heap_min(&B->heap);
        int fdone = fmin >= best, bdone = bmin >= best;
        if (fdone && bdone) break;
        int dir = (!fdone && (bdone || fmin <= bmin)) ? 0 : 1;
        side *S = dir == 0 ? F : B;
        side *O = dir == 0 ? B : F;
        uint64_t key;
        uint32_t u;
        tz_heap_pop(&S->heap, &key, &u);
        tz_nodeent *eu = tz_nodemap_get(&S->map, u);
        if (!eu || eu->settled || key > eu->dist) continue;
        eu->settled = 1;
        settled++;
        uint32_t du = eu->dist;
        tz_nodeent *eo = tz_nodemap_get(&O->map, u);
        if (eo && eo->dist != UINT32_MAX) {
            uint64_t tot = (uint64_t)du + eo->dist;
            if (tot < best) { best = tot; meet = u; }
        }
        if (u >= g->n_nodes) continue;
        uint32_t flag = dir == 0 ? TZ_E_FWD : TZ_E_BWD;
        uint32_t e0 = g->first[u], e1 = g->first[u + 1];
        if (e1 > g->n_edges || e0 > e1) continue;
        for (uint32_t e = e0; e < e1; e++) {
            const tz_chedge *ed = &g->edges[e];
            if (!(ed->wf & flag)) continue;
            uint32_t v = ed->target;
            if (v >= g->n_nodes) continue;
            uint64_t nd = (uint64_t)du + (ed->wf & TZ_E_WMASK);
            if (nd >= best || nd >= UINT32_MAX) continue;
            tz_nodeent *ev = tz_nodemap_put(&S->map, v);
            if (!ev) return -1;
            if (nd < ev->dist) {
                ev->dist = (uint32_t)nd;
                ev->parent = u;
                ev->edge = e;
                ev->settled = 0;
                if (!tz_heap_push(&S->heap, nd, v)) return -1;
            }
        }
    }
    *meet_out = meet;
    *best_out = best;
    *settled_out = settled;
    return meet != UINT32_MAX;
}

int tz__ch_node_distance(const tz_graph *g, uint32_t src_road, uint32_t dst_road, uint64_t *dist)
{
    if (src_road >= g->n_road_nodes || dst_road >= g->n_road_nodes) return 0;
    side F, B;
    memset(&F, 0, sizeof F);
    memset(&B, 0, sizeof B);
    int ok = tz_nodemap_init(&F.map, 1024) && tz_nodemap_init(&B.map, 1024);
    if (ok && g->deg[src_road] > 0) ok = seed(&F, g->road_ch[src_road], 0, 0);
    if (ok) ok = seed(&B, g->road_ch[dst_road], 0, 0);
    /* copies of the destination: every arrival there counts */
    for (uint32_t i = 0; ok && i < g->n_arrive; i++)
        if (g->node_road[g->arrive[i].node] == dst_road) ok = seed(&B, g->arrive[i].node, 0, 0);
    uint32_t meet = UINT32_MAX, settled = 0;
    uint64_t best = UINT64_MAX;
    int found = ok ? ch_search(g, &F, &B, &meet, &best, &settled) : -1;
    tz_nodemap_free(&F.map);
    tz_nodemap_free(&B.map);
    tz_heap_free(&F.heap);
    tz_heap_free(&B.heap);
    if (found > 0) *dist = best;
    return found > 0;
}

/* -------------------------------------------------------------- unpack */

typedef struct { uint32_t seg; uint32_t dir; } arc;
typedef struct { uint32_t e, at, fwd; } unpack_item;

/* Expand CH edge e stored at node `at` into original segments. fwd=1 means
 * travel at -> target, fwd=0 means target -> at. */
static int unpack_edge(const tz_graph *g, uint32_t e, uint32_t at, int fwd,
                       tz_vec *arcs, tz_vec *stack)
{
    stack->n = 0;
    unpack_item *it = (unpack_item *)tz_vec_push(stack);
    if (!it) return TZ_ERR_NOMEM;
    it->e = e; it->at = at; it->fwd = (uint32_t)fwd;
    uint64_t guard = 0;
    while (stack->n) {
        if (++guard > 100000000ull) return TZ_ERR_FORMAT;
        unpack_item cur = ((unpack_item *)stack->p)[--stack->n];
        if (cur.e >= g->n_edges) return TZ_ERR_FORMAT;
        const tz_chedge *ed = &g->edges[cur.e];
        if (!(ed->wf & TZ_E_SHORTCUT)) {
            uint32_t seg = ed->payload >> 1, dir = ed->payload & 1;
            if (!cur.fwd) dir ^= 1;
            if (seg >= g->n_segs) return TZ_ERR_FORMAT;
            arc *a = (arc *)tz_vec_push(arcs);
            if (!a) return TZ_ERR_NOMEM;
            a->seg = seg;
            a->dir = dir;
            continue;
        }
        uint32_t m = ed->payload, x = cur.at, y = ed->target;
        uint32_t w = ed->wf & TZ_E_WMASK;
        if (m >= g->n_nodes) return TZ_ERR_FORMAT;
        /* Travel order: first p -> m, then m -> q. */
        uint32_t p = cur.fwd ? x : y, q = cur.fwd ? y : x;
        uint32_t m0 = g->first[m], m1 = g->first[m + 1];
        if (m1 > g->n_edges || m0 > m1) return TZ_ERR_FORMAT;
        uint32_t found1 = UINT32_MAX, found2 = UINT32_MAX;
        for (uint32_t i = m0; i < m1 && found1 == UINT32_MAX; i++) {
            const tz_chedge *a = &g->edges[i];
            if (a->target != p || !(a->wf & TZ_E_BWD)) continue;   /* p -> m */
            uint32_t wa = a->wf & TZ_E_WMASK;
            if (wa > w) continue;
            for (uint32_t j = m0; j < m1; j++) {
                const tz_chedge *b = &g->edges[j];
                if (b->target != q || !(b->wf & TZ_E_FWD)) continue; /* m -> q */
                if (wa + (b->wf & TZ_E_WMASK) == w) { found1 = i; found2 = j; break; }
            }
        }
        if (found1 == UINT32_MAX) return TZ_ERR_FORMAT;
        if (!tz_vec_reserve(stack, stack->n + 2)) return TZ_ERR_NOMEM;
        unpack_item *s = (unpack_item *)stack->p;
        /* LIFO: push the second half first. */
        s[stack->n].e = found2; s[stack->n].at = m; s[stack->n].fwd = 1; stack->n++;
        s[stack->n].e = found1; s[stack->n].at = m; s[stack->n].fwd = 0; stack->n++;
    }
    return TZ_OK;
}

/* ------------------------------------------------------------ geometry */

typedef struct {
    uint32_t seg;
    int      dir;          /* 0 = a->b, 1 = b->a */
    double   from, to;     /* fractions along a->b */
    uint32_t i0, i1;       /* polyline index range */
    int      ends_at_node; /* 1 if the piece ends at a junction node */
} piece;

typedef struct {
    tz_vec lat, lon, cd, ct;   /* double */
} polyline;

static int pl_push(polyline *pl, double lat, double lon)
{
    if (pl->lat.n) {
        double plat = ((double *)pl->lat.p)[pl->lat.n - 1];
        double plon = ((double *)pl->lon.p)[pl->lon.n - 1];
        if (fabs(plat - lat) < 1e-9 && fabs(plon - lon) < 1e-9) return 1;
    }
    double *a = (double *)tz_vec_push(&pl->lat);
    double *b = (double *)tz_vec_push(&pl->lon);
    if (!a || !b) return 0;
    *a = lat;
    *b = lon;
    return 1;
}

/* Point at distance s (metres) along the polyline p with cumulative lengths L. */
static void interp_point(const tz_point *p, const double *L, int n, double s,
                         double *lat, double *lon)
{
    int k = 0;
    while (k < n - 2 && L[k + 1] < s) k++;
    double len = L[k + 1] - L[k];
    double t = len > 0 ? (s - L[k]) / len : 0.0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *lat = (p[k].lat + t * (p[k + 1].lat - p[k].lat)) / 1e6;
    *lon = (p[k].lon + t * (p[k + 1].lon - p[k].lon)) / 1e6;
}

/* Append the part of `seg` from fraction `from` to `to` (in a->b length
 * space). Returns 0 on allocation failure, -1 on format error. */
static int append_piece(const tz_roads *r, polyline *pl, tz_vec *pts, tz_vec *cum,
                        piece *pc)
{
    int n = tz_roads_seg_points(r, pc->seg, pts);
    if (n < 2) return -1;
    const tz_point *p = (const tz_point *)pts->p;
    if (!tz_vec_reserve(cum, (size_t)n)) return 0;
    double *L = (double *)cum->p;
    L[0] = 0.0;
    for (int i = 1; i < n; i++)
        L[i] = L[i - 1] + tz_distance_m(p[i - 1].lat / 1e6, p[i - 1].lon / 1e6, p[i].lat / 1e6, p[i].lon / 1e6);
    double total = L[n - 1];
    double s0 = pc->from * total, s1 = pc->to * total;

    double la, lo;
    interp_point(p, L, n, s0, &la, &lo);
    if (!pl_push(pl, la, lo)) return 0;
    pc->i0 = (uint32_t)pl->lat.n - 1; /* the first point, or the shared joint */
    if (s1 >= s0) {
        for (int i = 0; i < n; i++)
            if (L[i] > s0 && L[i] < s1 && !pl_push(pl, p[i].lat / 1e6, p[i].lon / 1e6)) return 0;
    } else {
        for (int i = n - 1; i >= 0; i--)
            if (L[i] < s0 && L[i] > s1 && !pl_push(pl, p[i].lat / 1e6, p[i].lon / 1e6)) return 0;
    }
    interp_point(p, L, n, s1, &la, &lo);
    if (!pl_push(pl, la, lo)) return 0;
    pc->i1 = (uint32_t)pl->lat.n - 1;
    return 1;
}

/* ----------------------------------------------------------- maneuvers */

static double bearing_back(const polyline *pl, uint32_t idx, double want)
{
    const double *la = (const double *)pl->lat.p, *lo = (const double *)pl->lon.p;
    const double *cd = (const double *)pl->cd.p;
    uint32_t j = idx;
    while (j > 0 && cd[idx] - cd[j] < want) j--;
    if (j == idx) return 0.0;
    return tz_bearing_deg(la[j], lo[j], la[idx], lo[idx]);
}

static double bearing_fwd(const polyline *pl, uint32_t idx, double want)
{
    const double *la = (const double *)pl->lat.p, *lo = (const double *)pl->lon.p;
    const double *cd = (const double *)pl->cd.p;
    uint32_t n = (uint32_t)pl->lat.n, j = idx;
    while (j + 1 < n && cd[j] - cd[idx] < want) j++;
    if (j == idx) return 0.0;
    return tz_bearing_deg(la[idx], lo[idx], la[j], lo[j]);
}

static int classify_turn(double a)
{
    double x = fabs(a);
    int s = a < 0 ? -1 : 1;
    if (x <= 25.0) return 0;
    if (x <= 60.0) return s;
    if (x <= 140.0) return 2 * s;
    if (x <= 170.0) return 3 * s;
    return 4;
}

static int is_major(uint8_t cls)
{
    return cls == TZ_CLS_MOTORWAY || cls == TZ_CLS_TRUNK;
}

static tz_maneuver *man_push(tz_vec *mv, const polyline *pl, uint8_t type, int mod,
                             uint32_t idx, const tz_roads *r, const tz_seg *next)
{
    tz_maneuver *m = (tz_maneuver *)tz_vec_push(mv);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);
    m->type = type;
    m->modifier = (int8_t)mod;
    m->point = idx;
    m->dist_m = ((const double *)pl->cd.p)[idx];
    m->time_s = ((const double *)pl->ct.p)[idx];
    m->name = next ? tz_roads_str(r, next->name) : "";
    m->ref = next ? tz_roads_str(r, next->ref) : "";
    return m;
}

static int build_maneuvers(const tz_graph *g, const polyline *pl, const piece *pcs,
                           uint32_t npc, tz_vec *mv)
{
    const tz_roads *r = g->roads;
    uint32_t last = (uint32_t)pl->lat.n - 1;
    const double WANT = 20.0;

    tz_maneuver *m = man_push(mv, pl, TZ_MAN_DEPART, 0, 0, r, &r->segs[pcs[0].seg]);
    if (!m) return 0;
    m->bearing_after = bearing_fwd(pl, 0, WANT);

    int in_rb = (r->segs[pcs[0].seg].flags & TZ_SEG_ROUNDABOUT) != 0;
    size_t rb_index = 0;
    int rb_exits = 0;
    if (in_rb) {
        m = man_push(mv, pl, TZ_MAN_ROUNDABOUT, 0, 0, r, NULL);
        if (!m) return 0;
        rb_index = mv->n - 1;
    }

    for (uint32_t i = 0; i + 1 < npc; i++) {
        const piece *a = &pcs[i], *b = &pcs[i + 1];
        const tz_seg *sa = &r->segs[a->seg], *sb = &r->segs[b->seg];
        uint32_t idx = a->i1;
        if (idx == 0 || idx >= last) continue;
        uint32_t node = a->dir == 0 ? sa->b : sa->a;
        double bin = bearing_back(pl, idx, WANT), bout = bearing_fwd(pl, idx, WANT);
        double turn = tz_norm_angle(bout - bin);
        int mod = classify_turn(turn);
        int rb_a = (sa->flags & TZ_SEG_ROUNDABOUT) != 0, rb_b = (sb->flags & TZ_SEG_ROUNDABOUT) != 0;
        int deg = node < g->n_road_nodes ? g->deg[node] : 0;
        int back = seg_weight(g, a->seg, a->dir ^ 1) != TZ_NO_ACCESS;
        int choice = deg - back >= 2;

        if (rb_a && rb_b) { if (deg >= 2) rb_exits++; continue; }
        if (!rb_a && rb_b) {
            m = man_push(mv, pl, TZ_MAN_ROUNDABOUT, 0, idx, r, NULL);
            if (!m) return 0;
            m->bearing_before = bin;
            rb_index = mv->n - 1;
            rb_exits = 0;
            in_rb = 1;
            continue;
        }
        if (rb_a && !rb_b) {
            tz_maneuver *rm = &((tz_maneuver *)mv->p)[rb_index];
            rm->exit = (uint8_t)(rb_exits + 1 > 255 ? 255 : rb_exits + 1);
            rm->name = tz_roads_str(r, sb->name);
            rm->ref = tz_roads_str(r, sb->ref);
            rm->bearing_after = bout;
            in_rb = 0;
            continue;
        }

        int name_changed = sa->name != sb->name || sa->ref != sb->ref;
        uint8_t type = 0xFF;
        if ((sb->flags & TZ_SEG_FERRY) && !(sa->flags & TZ_SEG_FERRY)) {
            type = TZ_MAN_FERRY;
        } else if ((sb->flags & TZ_SEG_LINK) && !(sa->flags & TZ_SEG_LINK) && choice) {
            type = TZ_MAN_RAMP;
            if (mod == 0) mod = turn < 0 ? -1 : 1;
        } else if (mod == 4) {
            type = TZ_MAN_TURN;
        } else if (mod <= -2 || mod >= 2) {
            if (choice || name_changed) type = TZ_MAN_TURN;
        } else if (mod == 1 || mod == -1) {
            if (choice && name_changed)
                type = (is_major(sa->cls) || (sa->flags & TZ_SEG_LINK)) ? TZ_MAN_FORK : TZ_MAN_TURN;
        } else if (choice && name_changed) {
            type = TZ_MAN_CONTINUE;
        }
        if (type == 0xFF) continue;
        m = man_push(mv, pl, type, mod, idx, r, sb);
        if (!m) return 0;
        m->bearing_before = bin;
        m->bearing_after = bout;
    }
    if (in_rb) {
        /* Destination inside the roundabout: exit 0 means "enter the roundabout". */
        tz_maneuver *rm = &((tz_maneuver *)mv->p)[rb_index];
        rm->exit = 0;
    }

    m = man_push(mv, pl, TZ_MAN_ARRIVE, 0, last, r, NULL);
    if (!m) return 0;
    m->bearing_before = bearing_back(pl, last, WANT);

    /* Merge turn pairs closer than 25 m (crossing a divided road, short
     * connector pieces) into one instruction. */
    tz_maneuver *ms = (tz_maneuver *)mv->p;
    size_t n = mv->n, w = 0;
    for (size_t i = 0; i < n; i++) {
        tz_maneuver cur = ms[i];
        if (w > 0) {
            tz_maneuver *prev = &ms[w - 1];
            int pt = prev->type, ct = cur.type;
            if ((pt == TZ_MAN_TURN || pt == TZ_MAN_CONTINUE) &&
                (ct == TZ_MAN_TURN || ct == TZ_MAN_CONTINUE) &&
                cur.dist_m - prev->dist_m < 25.0) {
                double comb = tz_norm_angle(cur.bearing_after - prev->bearing_before);
                int cm = classify_turn(comb);
                prev->modifier = (int8_t)cm;
                prev->type = cm == 0 ? TZ_MAN_CONTINUE : TZ_MAN_TURN;
                prev->bearing_after = cur.bearing_after;
                prev->name = cur.name;
                prev->ref = cur.ref;
                if (cm == 0 && w >= 2 && strcmp(ms[w - 2].name, cur.name) == 0 &&
                    strcmp(ms[w - 2].ref, cur.ref) == 0) {
                    w--; /* net effect: we kept going on the same road */
                }
                continue;
            }
        }
        ms[w++] = cur;
    }
    mv->n = w;
    return 1;
}

/* ---------------------------------------------------------------- route */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

void tz_route_free(tz_route *rt)
{
    if (!rt) return;
    free(rt->lat);
    free(rt->lon);
    free(rt->cum_dist);
    free(rt->cum_time);
    free(rt->man);
    free(rt);
}

/* Route between two already snapped points. */
static int route_snapped(const tz_graph *g, const tz_waypoint *from, tz_snap s, tz_snap t,
                         double t0, tz_route **out)
{
    const tz_roads *r = g->roads;

    int status = TZ_OK;
    side F, B;
    memset(&F, 0, sizeof F);
    memset(&B, 0, sizeof B);
    tz_vec arcs, stack, pts, cum, pcs, mv;
    tz_vec_init(&arcs, sizeof(arc));
    tz_vec_init(&stack, sizeof(unpack_item));
    tz_vec_init(&pts, sizeof(tz_point));
    tz_vec_init(&cum, sizeof(double));
    tz_vec_init(&pcs, sizeof(piece));
    tz_vec_init(&mv, sizeof(tz_maneuver));
    polyline pl;
    tz_vec_init(&pl.lat, sizeof(double));
    tz_vec_init(&pl.lon, sizeof(double));
    tz_vec_init(&pl.cd, sizeof(double));
    tz_vec_init(&pl.ct, sizeof(double));
    tz_route *rt = NULL;

    if (!tz_nodemap_init(&F.map, 1024) || !tz_nodemap_init(&B.map, 1024)) { status = TZ_ERR_NOMEM; goto done; }

    /* Directions allowed at the start. With a known heading, only leave in
     * the direction we are already travelling (if that is legal). */
    int sdir_ok[2], tdir_ok[2];
    sdir_ok[0] = seg_weight(g, s.seg, 0) != TZ_NO_ACCESS;
    sdir_ok[1] = seg_weight(g, s.seg, 1) != TZ_NO_ACCESS;
    if (from->heading >= 0.0 && sdir_ok[0] && sdir_ok[1]) {
        double d0 = fabs(tz_norm_angle(s.bearing - from->heading));
        double d1 = fabs(tz_norm_angle(s.bearing + 180.0 - from->heading));
        if (d0 <= 80.0 && d1 > 100.0) sdir_ok[1] = 0;
        else if (d1 <= 80.0 && d0 > 100.0) sdir_ok[0] = 0;
    }
    tdir_ok[0] = seg_weight(g, t.seg, 0) != TZ_NO_ACCESS;
    tdir_ok[1] = seg_weight(g, t.seg, 1) != TZ_NO_ACCESS;

    /* forward seeds */
    int ok = 1;
    if (sdir_ok[0]) ok &= seed(&F, arrive_node(g, s.seg, 0), (1.0 - s.frac) * seg_weight(g, s.seg, 0), 0);
    if (sdir_ok[1]) ok &= seed(&F, arrive_node(g, s.seg, 1), s.frac * seg_weight(g, s.seg, 1), 1);
    /* backward seeds: every CH node from which we may leave along t */
    for (int d = 0; d < 2 && ok; d++) {
        if (!tdir_ok[d]) continue;
        const tz_seg *ts = &r->segs[t.seg];
        uint32_t from_node = d == 0 ? ts->a : ts->b;
        double c = (d == 0 ? t.frac : 1.0 - t.frac) * seg_weight(g, t.seg, d);
        if (from_node < g->n_road_nodes && g->deg[from_node] > 0)
            ok &= seed(&B, g->road_ch[from_node], c, d);
        uint32_t key = t.seg * 2u + (uint32_t)d, i = 0;
        if (g->n_depart) {
            (void)keynode_find(g->depart, g->n_depart, key, &i); /* i = first entry >= key */
            for (; i < g->n_depart && g->depart[i].key == key; i++)
                ok &= seed(&B, g->depart[i].node, c, d);
        }
    }
    if (!ok) { status = TZ_ERR_NOMEM; goto done; }

    uint32_t meet = UINT32_MAX, settled = 0;
    uint64_t best = UINT64_MAX;
    int found = ch_search(g, &F, &B, &meet, &best, &settled);
    if (found < 0) { status = TZ_ERR_NOMEM; goto done; }

    /* Start and end on the same segment: driving straight there may win. */
    int direct = 0;
    double direct_cost = 1e300;
    if (s.seg == t.seg) {
        if (t.frac >= s.frac && sdir_ok[0]) direct_cost = (t.frac - s.frac) * seg_weight(g, s.seg, 0);
        else if (t.frac < s.frac && sdir_ok[1]) direct_cost = (s.frac - t.frac) * seg_weight(g, s.seg, 1);
        if (direct_cost < 1e300 && (!found || direct_cost <= (double)best)) direct = 1;
    }
    if (!found && !direct) { status = TZ_ERR_NO_ROUTE; goto done; }

    /* Build the list of pieces travelled. */
    if (direct) {
        piece *pc = (piece *)tz_vec_push(&pcs);
        if (!pc) { status = TZ_ERR_NOMEM; goto done; }
        memset(pc, 0, sizeof *pc);
        pc->seg = s.seg;
        pc->dir = t.frac >= s.frac ? 0 : 1;
        pc->from = s.frac;
        pc->to = t.frac;
    } else {
        /* forward half: walk parents back to the root, then reverse */
        tz_vec chain;
        tz_vec_init(&chain, sizeof(unpack_item));
        uint32_t u = meet;
        int sroot_dir = 0, troot_dir = 0;
        for (uint32_t guard = 0;; guard++) {
            tz_nodeent *e = tz_nodemap_get(&F.map, u);
            if (!e || guard > g->n_nodes + 2) { status = TZ_ERR_FORMAT; break; }
            if (e->parent == UINT32_MAX) { sroot_dir = (int)(e->edge & 1); break; }
            unpack_item *ci = (unpack_item *)tz_vec_push(&chain);
            if (!ci) { status = TZ_ERR_NOMEM; break; }
            ci->e = e->edge; ci->at = e->parent; ci->fwd = 1;
            u = e->parent;
        }
        if (status != TZ_OK) { tz_vec_free(&chain); goto done; }
        size_t nf = chain.n;
        /* backward half: from meet towards the target root */
        u = meet;
        for (uint32_t guard = 0;; guard++) {
            tz_nodeent *e = tz_nodemap_get(&B.map, u);
            if (!e || guard > g->n_nodes + 2) { status = TZ_ERR_FORMAT; break; }
            if (e->parent == UINT32_MAX) { troot_dir = (int)(e->edge & 1); break; }
            unpack_item *ci = (unpack_item *)tz_vec_push(&chain);
            if (!ci) { status = TZ_ERR_NOMEM; break; }
            ci->e = e->edge; ci->at = e->parent; ci->fwd = 0;
            u = e->parent;
        }
        if (status != TZ_OK) { tz_vec_free(&chain); goto done; }

        unpack_item *ch = (unpack_item *)chain.p;
        for (size_t i = 0; i < nf / 2; i++) { unpack_item x = ch[i]; ch[i] = ch[nf - 1 - i]; ch[nf - 1 - i] = x; }
        for (size_t i = 0; i < chain.n && status == TZ_OK; i++)
            status = unpack_edge(g, ch[i].e, ch[i].at, (int)ch[i].fwd, &arcs, &stack);
        tz_vec_free(&chain);
        if (status != TZ_OK) goto done;

        piece *pc = (piece *)tz_vec_push(&pcs);
        if (!pc) { status = TZ_ERR_NOMEM; goto done; }
        memset(pc, 0, sizeof *pc);
        pc->seg = s.seg; pc->dir = sroot_dir; pc->from = s.frac; pc->to = sroot_dir == 0 ? 1.0 : 0.0;
        pc->ends_at_node = 1;
        const arc *as = (const arc *)arcs.p;
        for (size_t i = 0; i < arcs.n; i++) {
            pc = (piece *)tz_vec_push(&pcs);
            if (!pc) { status = TZ_ERR_NOMEM; goto done; }
            memset(pc, 0, sizeof *pc);
            pc->seg = as[i].seg; pc->dir = (int)as[i].dir;
            pc->from = as[i].dir == 0 ? 0.0 : 1.0; pc->to = 1.0 - pc->from;
            pc->ends_at_node = 1;
        }
        pc = (piece *)tz_vec_push(&pcs);
        if (!pc) { status = TZ_ERR_NOMEM; goto done; }
        memset(pc, 0, sizeof *pc);
        pc->seg = t.seg; pc->dir = troot_dir; pc->from = troot_dir == 0 ? 0.0 : 1.0; pc->to = t.frac;
    }

    /* Geometry, cumulative distance and time. */
    piece *P = (piece *)pcs.p;
    for (size_t i = 0; i < pcs.n; i++) {
        int rc = append_piece(r, &pl, &pts, &cum, &P[i]);
        if (rc == 0) { status = TZ_ERR_NOMEM; goto done; }
        if (rc < 0) { status = TZ_ERR_FORMAT; goto done; }
    }
    if (pl.lat.n == 1 && !pl_push(&pl, ((double *)pl.lat.p)[0] + 1e-7, ((double *)pl.lon.p)[0])) {
        status = TZ_ERR_NOMEM; goto done;
    }
    size_t np = pl.lat.n;
    if (!tz_vec_reserve(&pl.cd, np) || !tz_vec_reserve(&pl.ct, np)) { status = TZ_ERR_NOMEM; goto done; }
    pl.cd.n = pl.ct.n = np;
    {
        double *la = (double *)pl.lat.p, *lo = (double *)pl.lon.p, *cd = (double *)pl.cd.p, *ct = (double *)pl.ct.p;
        memset(ct, 0, np * sizeof(double));
        cd[0] = 0.0;
        for (size_t i = 1; i < np; i++) cd[i] = cd[i - 1] + tz_distance_m(la[i - 1], lo[i - 1], la[i], lo[i]);
        for (size_t k = 0; k < pcs.n; k++) {
            uint32_t i0 = P[k].i0, i1 = P[k].i1;
            uint32_t dur = seg_duration(g, P[k].seg, P[k].dir);
            double secs = dur == TZ_NO_ACCESS ? 0.0 : fabs(P[k].to - P[k].from) * dur / 10.0;
            double len = cd[i1] - cd[i0];
            for (uint32_t i = i0 + 1; i <= i1; i++)
                ct[i] = ct[i0] + (len > 0 ? secs * (cd[i] - cd[i0]) / len : 0.0);
        }
        /* Points not covered by any piece (the padding point of a zero-length
         * route) inherit the time of the point before. */
        for (size_t i = 1; i < np; i++) if (ct[i] < ct[i - 1]) ct[i] = ct[i - 1];
    }

    if (!build_maneuvers(g, &pl, P, (uint32_t)pcs.n, &mv)) { status = TZ_ERR_NOMEM; goto done; }

    rt = (tz_route *)calloc(1, sizeof *rt);
    if (!rt) { status = TZ_ERR_NOMEM; goto done; }
    rt->profile = g->profile;
    rt->npts = (uint32_t)np;
    rt->lat = (double *)pl.lat.p; pl.lat.p = NULL;
    rt->lon = (double *)pl.lon.p; pl.lon.p = NULL;
    rt->cum_dist = (double *)pl.cd.p; pl.cd.p = NULL;
    rt->cum_time = (double *)pl.ct.p; pl.ct.p = NULL;
    rt->distance_m = rt->cum_dist[np - 1];
    rt->duration_s = rt->cum_time[np - 1];
    rt->nman = (uint32_t)mv.n;
    rt->man = (tz_maneuver *)mv.p; mv.p = NULL;
    rt->snap_start_m = s.dist_m;
    rt->snap_end_m = t.dist_m;
    rt->settled = settled;
    rt->compute_ms = now_ms() - t0;
    *out = rt;
    status = TZ_OK;

done:
    tz_nodemap_free(&F.map);
    tz_nodemap_free(&B.map);
    tz_heap_free(&F.heap);
    tz_heap_free(&B.heap);
    tz_vec_free(&arcs);
    tz_vec_free(&stack);
    tz_vec_free(&pts);
    tz_vec_free(&cum);
    tz_vec_free(&pcs);
    tz_vec_free(&mv);
    tz_vec_free(&pl.lat);
    tz_vec_free(&pl.lon);
    tz_vec_free(&pl.cd);
    tz_vec_free(&pl.ct);
    return status;
}

/* Candidate snaps for a waypoint: islands only when nothing else is near. */
static int snap_candidates(const tz_graph *g, double lat, double lon, double radius,
                           double heading, tz_snap *out, int k)
{
    snap_filter_ctx c = { g, 0 };
    int n = tz_roads_nearest_k(g->roads, lat, lon, radius, heading, graph_filter, &c, out, k);
    if (n) return n;
    c.allow_islands = 1;
    return tz_roads_nearest_k(g->roads, lat, lon, radius, heading, graph_filter, &c, out, k);
}

#define SNAP_ALTERNATIVES 4

int tz_route_compute(const tz_graph *g, const tz_waypoint *from,
                     const tz_waypoint *to, tz_route **out)
{
    if (!g || !from || !to || !out) return TZ_ERR_ARGUMENT;
    *out = NULL;
    double t0 = now_ms();
    double rs = from->radius_m > 0 ? from->radius_m : DEFAULT_RADIUS;
    double rz = to->radius_m > 0 ? to->radius_m : DEFAULT_RADIUS;

    tz_snap S[SNAP_ALTERNATIVES], T[SNAP_ALTERNATIVES];
    int ns = snap_candidates(g, from->lat, from->lon, rs, from->heading, S, 1);
    if (!ns) return TZ_ERR_NO_START;
    int nt = snap_candidates(g, to->lat, to->lon, rz, -1.0, T, 1);
    if (!nt) return TZ_ERR_NO_END;
    int st = route_snapped(g, from, S[0], T[0], t0, out);
    if (st != TZ_ERR_NO_ROUTE) return st;

    /* The nearest roads are not connected (a one-way trap, a private stub,
     * the edge of the map). Try the next-nearest roads, closest pairs first. */
    ns = snap_candidates(g, from->lat, from->lon, rs, from->heading, S, SNAP_ALTERNATIVES);
    nt = snap_candidates(g, to->lat, to->lon, rz, -1.0, T, SNAP_ALTERNATIVES);
    int tried[SNAP_ALTERNATIVES][SNAP_ALTERNATIVES];
    memset(tried, 0, sizeof tried);
    tried[0][0] = 1; /* S[0], T[0] are the pair already tried */
    for (int attempt = 0; attempt < ns * nt; attempt++) {
        int bi = -1, bj = -1;
        double best = 1e300;
        for (int i = 0; i < ns; i++)
            for (int j = 0; j < nt; j++)
                if (!tried[i][j] && S[i].dist_m + T[j].dist_m < best) { best = S[i].dist_m + T[j].dist_m; bi = i; bj = j; }
        if (bi < 0) break;
        tried[bi][bj] = 1;
        st = route_snapped(g, from, S[bi], T[bj], t0, out);
        if (st != TZ_ERR_NO_ROUTE) return st;
    }
    return TZ_ERR_NO_ROUTE;
}
