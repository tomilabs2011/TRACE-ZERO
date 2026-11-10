/*
 * One routing graph per profile: costs, turn restrictions, barriers,
 * island detection, contraction and output (car.tzg, bike.tzg, foot.tzg).
 */
#include "fz_build.h"
#include "fz_ch.h"
#include "../../Core/tz_core.h"
#include "../../Core/tz_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIGNAL_DS_CAR  80   /* 8 s average wait at a traffic signal */
#define SIGNAL_DS_BIKE 50
#define FERRY_BOARD_S  300  /* waiting/boarding time added per ferry crossing */
#define ISLAND_MAX     1000 /* smaller disconnected components are islands */

static const char *const PROFILE_NAME[3] = { "car", "bike", "foot" };

typedef struct { uint32_t key, node; } keynode;

typedef FZ_VEC(keynode) keynode_vec;
typedef FZ_VEC(uint32_t) u32_vec;

static int cmp_keynode(const void *pa, const void *pb)
{
    const keynode *a = (const keynode *)pa, *b = (const keynode *)pb;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return (a->node > b->node) - (a->node < b->node);
}

static int find_key(const keynode *a, size_t n, uint32_t key, size_t *first)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid].key < key) lo = mid + 1; else hi = mid;
    }
    *first = lo;
    return lo < n && a[lo].key == key;
}

/* ------------------------------------------------------------- costs */

static void compute_costs(const fz_build *b, int p, tz_segcost *cost)
{
    for (size_t i = 0; i < b->segs.n; i++) {
        const fz_bseg *s = &b->segs.a[i];
        const fz_rway *w = &b->rways.a[s->way];
        tz_segcost *c = &cost[i];
        c->w_fwd = c->w_bwd = c->d_fwd = c->d_bwd = TZ_NO_ACCESS;
        uint8_t acc = w->attr.acc[p];
        if (!acc) continue;
        double secs;
        if ((w->attr.flags & TZ_SEG_FERRY) && w->attr.ferry_s > 0 && w->len_m > 0)
            secs = w->attr.ferry_s * (s->len_m / w->len_m);
        else
            secs = s->len_m / (w->attr.speed[p] / 3.6);
        if (w->attr.flags & TZ_SEG_FERRY)
            secs += w->len_m > 0 ? FERRY_BOARD_S * (s->len_m / w->len_m) : FERRY_BOARD_S;
        double ds = secs * 10.0;
        double sig = p == TZ_PROFILE_CAR ? SIGNAL_DS_CAR : p == TZ_PROFILE_BIKE ? SIGNAL_DS_BIKE : 0;
        double mid = s->sig_mid ? sig : 0;
        /* A signal at a junction is waited for when arriving at it. */
        double end_b = (b->nflags[b->road_need[s->b]] & 1) ? sig : 0;
        double end_a = (b->nflags[b->road_need[s->a]] & 1) ? sig : 0;
        double f = w->attr.factor[p];
        if (acc & FZ_ACC_FWD) {
            double d = ds + mid + end_b;
            c->d_fwd = (uint32_t)fmin(fmax(d, 1.0), 4e8);
            c->w_fwd = (uint32_t)fmin(fmax(d * f, 1.0), 4e8);
        }
        if (acc & FZ_ACC_BWD) {
            double d = ds + mid + end_a;
            c->d_bwd = (uint32_t)fmin(fmax(d, 1.0), 4e8);
            c->w_bwd = (uint32_t)fmin(fmax(d * f, 1.0), 4e8);
        }
    }
}

/* ------------------------------------------------------ restrictions */

typedef struct { uint32_t seg, dir; } sdir;

/* Bearing of segment s at its end touching node `node`, pointing away
 * from the node (into the segment). */
static double bearing_from_node(const fz_build *b, uint32_t s, int at_a)
{
    const fz_bseg *sg = &b->segs.a[s];
    const uint32_t *ri = b->refidx + b->rways.a[sg->way].ref0;
    uint32_t n0, n1 = UINT32_MAX;
    if (at_a) {
        n0 = ri[sg->r0];
        for (uint32_t k = sg->r0 + 1; k <= sg->r1; k++) if (ri[k] != n0) { n1 = ri[k]; break; }
    } else {
        n0 = ri[sg->r1];
        for (int64_t k = (int64_t)sg->r1 - 1; k >= (int64_t)sg->r0; k--) if (ri[k] != n0) { n1 = ri[k]; break; }
    }
    if (n1 == UINT32_MAX) return 0;
    return tz_bearing_deg(b->nlat[n0] / 1e6, b->nlon[n0] / 1e6, b->nlat[n1] / 1e6, b->nlon[n1] / 1e6);
}

/* Turn angle (-180..180, positive = right) from arriving arc to leaving arc at a node. */
static double turn_angle(const fz_build *b, sdir in, sdir out)
{
    /* arriving along `in`: direction of travel at the node is the reverse of
     * the bearing measured from the node into the segment */
    double bin = bearing_from_node(b, in.seg, in.dir == 1) + 180.0;
    double bout = bearing_from_node(b, out.seg, out.dir == 0);
    return tz_norm_angle(bout - bin);
}

static int turn_matches(double a, int turn)
{
    double x = fabs(a);
    switch (turn) {
    case FZ_TURN_LEFT: return a < -30 && a > -160;
    case FZ_TURN_RIGHT: return a > 30 && a < 160;
    case FZ_TURN_STRAIGHT: return x <= 45;
    case FZ_TURN_U: return x >= 135;
    default: return 1;
    }
}

static double turn_ideal(int turn)
{
    switch (turn) {
    case FZ_TURN_LEFT: return -90;
    case FZ_TURN_RIGHT: return 90;
    case FZ_TURN_U: return 180;
    default: return 0;
    }
}

typedef struct {
    uint32_t key;          /* arriving arc */
    uint32_t via;          /* road node */
    uint8_t  kind;
    uint32_t to_first, to_n; /* range in the `tos` array */
} rule;

static int cmp_rule(const void *pa, const void *pb)
{
    const rule *a = (const rule *)pa, *b = (const rule *)pb;
    return (a->key > b->key) - (a->key < b->key);
}

static int allowed(const tz_segcost *c, uint32_t seg, uint32_t dir)
{
    return (dir == 0 ? c[seg].w_fwd : c[seg].w_bwd) != TZ_NO_ACCESS;
}

/* Builds the copy nodes. Returns number of copies; fills arrive/depart and copy_orig. */
static uint32_t build_restrictions(const fz_build *b, int p, const tz_segcost *cost, const uint8_t *is_barrier,
                                   keynode_vec *arrive, keynode_vec *depart, u32_vec *copy_orig)
{
    FZ_VEC(rule) rules;
    FZ_VEC(sdir) tos;
    memset(&rules, 0, sizeof rules);
    memset(&tos, 0, sizeof tos);
    size_t used = 0, skipped = 0;

    for (size_t ri = 0; ri < b->restr.n; ri++) {
        const fz_restr *r = &b->restr.a[ri];
        if (!(r->mask & (1u << p))) continue;
        uint32_t fw = fz_map64_get(&b->way_index, r->from_way);
        uint32_t tw = fz_map64_get(&b->way_index, r->to_way);
        size_t vi = fz_bsearch_i64(b->need, b->n_need, r->via);
        if (fw == UINT32_MAX || tw == UINT32_MAX || vi == SIZE_MAX || b->roadid[vi] == UINT32_MAX) { skipped++; continue; }
        uint32_t V = b->roadid[vi];

        sdir from[8], to[8];
        int nf = 0, nt = 0;
        for (uint32_t k = b->way_seg_first[fw]; k < b->way_seg_first[fw + 1]; k++) {
            uint32_t s = b->way_segs[k];
            const fz_bseg *sg = &b->segs.a[s];
            if (sg->a == sg->b) continue;
            if (sg->b == V && allowed(cost, s, 0) && nf < 8) from[nf++] = (sdir){ s, 0 };
            if (sg->a == V && allowed(cost, s, 1) && nf < 8) from[nf++] = (sdir){ s, 1 };
        }
        for (uint32_t k = b->way_seg_first[tw]; k < b->way_seg_first[tw + 1]; k++) {
            uint32_t s = b->way_segs[k];
            const fz_bseg *sg = &b->segs.a[s];
            if (sg->a == sg->b) continue;
            if (sg->a == V && allowed(cost, s, 0) && nt < 8) to[nt++] = (sdir){ s, 0 };
            if (sg->b == V && allowed(cost, s, 1) && nt < 8) to[nt++] = (sdir){ s, 1 };
        }
        if (!nf || !nt) { skipped++; continue; }

        for (int i = 0; i < nf; i++) {
            sdir chosen[8];
            int nc = 0;
            if (fw == tw && r->turn == FZ_TURN_U) {
                /* U-turn on the same way: only the reverse of the arriving arc */
                if (allowed(cost, from[i].seg, from[i].dir ^ 1)) chosen[nc++] = (sdir){ from[i].seg, from[i].dir ^ 1 };
            } else {
                for (int j = 0; j < nt; j++)
                    if (!(to[j].seg == from[i].seg && to[j].dir == (from[i].dir ^ 1)) || r->turn == FZ_TURN_U)
                        chosen[nc++] = to[j];
                if (nc > 1 && r->turn != FZ_TURN_ANY) {
                    /* The to-way passes through the via node: pick the branch
                     * that matches the restriction's direction. */
                    sdir keep[8];
                    int nk = 0;
                    for (int j = 0; j < nc; j++)
                        if (turn_matches(turn_angle(b, from[i], chosen[j]), r->turn)) keep[nk++] = chosen[j];
                    if (nk == 0) {
                        double best = 1e9;
                        for (int j = 0; j < nc; j++) {
                            double d = fabs(tz_norm_angle(turn_angle(b, from[i], chosen[j]) - turn_ideal(r->turn)));
                            if (d < best) { best = d; keep[0] = chosen[j]; nk = 1; }
                        }
                    }
                    memcpy(chosen, keep, (size_t)nk * sizeof(sdir));
                    nc = nk;
                }
            }
            if (!nc) continue;
            rule ru;
            ru.key = from[i].seg * 2u + from[i].dir;
            ru.via = V;
            ru.kind = r->kind;
            ru.to_first = (uint32_t)tos.n;
            ru.to_n = (uint32_t)nc;
            for (int j = 0; j < nc; j++) fz_vec_push(tos, chosen[j]);
            fz_vec_push(rules, ru);
        }
        used++;
    }

    qsort(rules.a, rules.n, sizeof(rule), cmp_rule);
    uint32_t copies = 0;
    FZ_VEC(sdir) dep;
    memset(&dep, 0, sizeof dep);
    FZ_VEC(uint8_t) keep;
    memset(&keep, 0, sizeof keep);
    for (size_t i = 0; i < rules.n;) {
        size_t j = i;
        while (j < rules.n && rules.a[j].key == rules.a[i].key) j++;
        uint32_t V = rules.a[i].via;
        /* all departures from V */
        dep.n = 0;
        if (!is_barrier[V]) {
            for (uint32_t k = b->node_seg_first[V]; k < b->node_seg_first[V + 1]; k++) {
                uint32_t s = b->node_segs[k];
                const fz_bseg *sg = &b->segs.a[s];
                if (sg->a == V && allowed(cost, s, 0)) fz_vec_push(dep, ((sdir){ s, 0 }));
                if (sg->b == V && allowed(cost, s, 1)) fz_vec_push(dep, ((sdir){ s, 1 }));
            }
        }
        fz_vec_grow(keep, dep.n);
        keep.n = dep.n;
        for (size_t d = 0; d < dep.n; d++) keep.a[d] = 1;
        for (size_t k = i; k < j; k++) {
            const rule *ru = &rules.a[k];
            for (size_t d = 0; d < dep.n; d++) {
                int listed = 0;
                for (uint32_t t = 0; t < ru->to_n; t++) {
                    const sdir *x = &tos.a[ru->to_first + t];
                    if (x->seg == dep.a[d].seg && x->dir == dep.a[d].dir) listed = 1;
                }
                if (ru->kind == FZ_R_NO && listed) keep.a[d] = 0;
                if (ru->kind == FZ_R_ONLY && !listed) keep.a[d] = 0;
            }
        }
        int changed = 0;
        for (size_t d = 0; d < dep.n; d++) if (!keep.a[d]) changed = 1;
        if (changed) {
            uint32_t c = b->n_road + copies++;
            fz_vec_push(*copy_orig, V);
            fz_vec_push(*arrive, ((keynode){ rules.a[i].key, c }));
            for (size_t d = 0; d < dep.n; d++)
                if (keep.a[d]) fz_vec_push(*depart, ((keynode){ dep.a[d].seg * 2u + dep.a[d].dir, c }));
        }
        i = j;
    }
    qsort(arrive->a, arrive->n, sizeof(keynode), cmp_keynode);
    qsort(depart->a, depart->n, sizeof(keynode), cmp_keynode);
    fz_log("  %s: %zu turn restrictions applied (%zu not applicable), %u split nodes",
           PROFILE_NAME[p], used, skipped, copies);
    fz_vec_free(rules);
    fz_vec_free(tos);
    fz_vec_free(dep);
    fz_vec_free(keep);
    return copies;
}


/* ------------------------------------------------------------- build */

void fz_build_profile(fz_build *b, int p, const char *path)
{
    char title[96];
    snprintf(title, sizeof title, "Building %s routing graph", PROFILE_NAME[p]);
    fz_stage(5 + p, 8, title);
    double t0 = fz_now();
    size_t ns = b->segs.n;
    uint32_t R = b->n_road;

    tz_segcost *cost = (tz_segcost *)fz_malloc((ns ? ns : 1) * sizeof(tz_segcost));
    compute_costs(b, p, cost);

    uint8_t *is_barrier = (uint8_t *)fz_calloc(R ? R : 1, 1);
    for (uint32_t r = 0; r < R; r++) is_barrier[r] = fz_node_barrier(b, b->road_need[r], p) ? 1 : 0;

    keynode_vec arrive;
    keynode_vec depart;
    u32_vec copy_orig;
    memset(&arrive, 0, sizeof arrive);
    memset(&depart, 0, sizeof depart);
    memset(&copy_orig, 0, sizeof copy_orig);
    uint32_t C = 0;
    if (p != TZ_PROFILE_FOOT) C = build_restrictions(b, p, cost, is_barrier, &arrive, &depart, &copy_orig);
    uint32_t N = R + C;

    /* node degrees for maneuver generation */
    uint8_t *deg = (uint8_t *)fz_calloc(R ? R : 1, 1);
    for (size_t i = 0; i < ns; i++) {
        const fz_bseg *s = &b->segs.a[i];
        if (cost[i].w_fwd != TZ_NO_ACCESS && !is_barrier[s->a] && deg[s->a] < 255) deg[s->a]++;
        if (cost[i].w_bwd != TZ_NO_ACCESS && !is_barrier[s->b] && deg[s->b] < 255) deg[s->b]++;
    }

    /* arcs */
    FZ_VEC(fz_arc) arcs;
    memset(&arcs, 0, sizeof arcs);
    for (size_t i = 0; i < ns; i++) {
        const fz_bseg *s = &b->segs.a[i];
        for (uint32_t d = 0; d < 2; d++) {
            uint32_t w = d == 0 ? cost[i].w_fwd : cost[i].w_bwd;
            if (w == TZ_NO_ACCESS) continue;
            uint32_t X = d == 0 ? s->a : s->b, Y = d == 0 ? s->b : s->a;
            uint32_t key = (uint32_t)i * 2u + d;
            size_t k;
            uint32_t target = find_key(arrive.a, arrive.n, key, &k) ? arrive.a[k].node : Y;
            if (!is_barrier[X]) fz_vec_push(arcs, ((fz_arc){ X, target, w, key }));
            if (find_key(depart.a, depart.n, key, &k))
                for (; k < depart.n && depart.a[k].key == key; k++)
                    fz_vec_push(arcs, ((fz_arc){ depart.a[k].node, target, w, key }));
        }
    }

    /* Islands: segments outside the largest strongly connected network
     * (one-way traps, private stubs, cut-off pieces at the edge of the
     * extract). Snapping avoids them when a better road is nearby. */
    uint8_t *sflags = (uint8_t *)fz_calloc(ns ? ns : 1, 1);
    {
        uint32_t *of = (uint32_t *)fz_calloc((size_t)R + 1, sizeof(uint32_t));
        uint32_t *inf = (uint32_t *)fz_calloc((size_t)R + 1, sizeof(uint32_t));
        for (size_t i = 0; i < ns; i++) {
            const fz_bseg *sg = &b->segs.a[i];
            if (cost[i].w_fwd != TZ_NO_ACCESS && !is_barrier[sg->a]) { of[sg->a + 1]++; inf[sg->b + 1]++; }
            if (cost[i].w_bwd != TZ_NO_ACCESS && !is_barrier[sg->b]) { of[sg->b + 1]++; inf[sg->a + 1]++; }
        }
        for (uint32_t r = 0; r < R; r++) { of[r + 1] += of[r]; inf[r + 1] += inf[r]; }
        uint32_t *oa = (uint32_t *)fz_malloc(((size_t)of[R] + 1) * sizeof(uint32_t));
        uint32_t *ia = (uint32_t *)fz_malloc(((size_t)inf[R] + 1) * sizeof(uint32_t));
        uint32_t *fo = (uint32_t *)fz_malloc(((size_t)R + 1) * sizeof(uint32_t));
        uint32_t *fi = (uint32_t *)fz_malloc(((size_t)R + 1) * sizeof(uint32_t));
        memcpy(fo, of, ((size_t)R + 1) * sizeof(uint32_t));
        memcpy(fi, inf, ((size_t)R + 1) * sizeof(uint32_t));
        for (size_t i = 0; i < ns; i++) {
            const fz_bseg *sg = &b->segs.a[i];
            if (cost[i].w_fwd != TZ_NO_ACCESS && !is_barrier[sg->a]) { oa[fo[sg->a]++] = sg->b; ia[fi[sg->b]++] = sg->a; }
            if (cost[i].w_bwd != TZ_NO_ACCESS && !is_barrier[sg->b]) { oa[fo[sg->b]++] = sg->a; ia[fi[sg->a]++] = sg->b; }
        }
        free(fo);
        free(fi);
        /* Kosaraju pass 1: finishing order with an explicit stack */
        uint32_t *order = (uint32_t *)fz_malloc(((size_t)R + 1) * sizeof(uint32_t));
        uint32_t *stk = (uint32_t *)fz_malloc(((size_t)R + 1) * sizeof(uint32_t));
        uint32_t *pos = (uint32_t *)fz_malloc(((size_t)R + 1) * sizeof(uint32_t));
        uint8_t *seen = (uint8_t *)fz_calloc(R ? R : 1, 1);
        uint32_t no = 0;
        for (uint32_t v = 0; v < R; v++) {
            if (seen[v]) continue;
            uint32_t sp = 0;
            stk[sp] = v; pos[sp] = of[v]; sp++;
            seen[v] = 1;
            while (sp) {
                uint32_t u = stk[sp - 1];
                if (pos[sp - 1] < of[u + 1]) {
                    uint32_t x = oa[pos[sp - 1]++];
                    if (!seen[x]) { seen[x] = 1; stk[sp] = x; pos[sp] = of[x]; sp++; }
                } else {
                    order[no++] = u;
                    sp--;
                }
            }
        }
        /* pass 2: components on the reversed graph */
        uint32_t *comp = (uint32_t *)fz_malloc(((size_t)R + 1) * sizeof(uint32_t));
        for (uint32_t v = 0; v < R; v++) comp[v] = UINT32_MAX;
        FZ_VEC(uint32_t) csize;
        memset(&csize, 0, sizeof csize);
        for (uint32_t k = R; k-- > 0;) {
            uint32_t v = order[k];
            if (comp[v] != UINT32_MAX) continue;
            uint32_t c = (uint32_t)csize.n, cnt = 0, sp = 0;
            stk[sp++] = v;
            comp[v] = c;
            while (sp) {
                uint32_t u = stk[--sp];
                cnt++;
                for (uint32_t e = inf[u]; e < inf[u + 1]; e++) {
                    uint32_t x = ia[e];
                    if (comp[x] == UINT32_MAX) { comp[x] = c; stk[sp++] = x; }
                }
            }
            fz_vec_push(csize, cnt);
        }
        uint32_t biggest = 0, bc = 0;
        for (size_t c = 0; c < csize.n; c++) if (csize.a[c] > biggest) { biggest = csize.a[c]; bc = (uint32_t)c; }
        size_t islands = 0;
        for (size_t i = 0; i < ns; i++) {
            if (cost[i].w_fwd == TZ_NO_ACCESS && cost[i].w_bwd == TZ_NO_ACCESS) continue;
            uint32_t ca = comp[b->segs.a[i].a], cb = comp[b->segs.a[i].b];
            int small_a = ca != bc && csize.a[ca] < ISLAND_MAX;
            int small_b = cb != bc && csize.a[cb] < ISLAND_MAX;
            if (small_a || small_b) { sflags[i] |= TZ_SF_ISLAND; islands++; }
        }
        fz_log("  %s: largest network has %u junctions, %zu segments marked as islands",
               PROFILE_NAME[p], biggest, islands);
        free(of); free(inf); free(oa); free(ia); free(order); free(stk); free(pos); free(seen); free(comp);
        fz_vec_free(csize);
    }

    fz_log("  %s: contracting %u nodes, %zu arcs", PROFILE_NAME[p], N, arcs.n);
    fz_ch ch;
    fz_ch_build(N, arcs.a, arcs.n, &ch);
    fz_vec_free(arcs);
    fz_log("  %s: %u CH edges (%llu shortcuts) in %.1f s", PROFILE_NAME[p], ch.n_edges,
           (unsigned long long)ch.shortcuts, fz_now() - t0);

    /* id maps */
    uint32_t *node_road = (uint32_t *)fz_malloc((N ? N : 1) * sizeof(uint32_t));
    uint32_t *road_ch = (uint32_t *)fz_malloc((R ? R : 1) * sizeof(uint32_t));
    for (uint32_t v = 0; v < N; v++) node_road[ch.rank[v]] = v < R ? v : copy_orig.a[v - R];
    for (uint32_t r = 0; r < R; r++) road_ch[r] = ch.rank[r];
    tz_keynode *arr_out = (tz_keynode *)fz_malloc((arrive.n ? arrive.n : 1) * sizeof(tz_keynode));
    tz_keynode *dep_out = (tz_keynode *)fz_malloc((depart.n ? depart.n : 1) * sizeof(tz_keynode));
    for (size_t i = 0; i < arrive.n; i++) { arr_out[i].key = arrive.a[i].key; arr_out[i].node = ch.rank[arrive.a[i].node]; }
    for (size_t i = 0; i < depart.n; i++) { dep_out[i].key = depart.a[i].key; dep_out[i].node = ch.rank[depart.a[i].node]; }

    fz_writer w;
    fz_writer_open(&w, path, TZ_MAGIC_GRAPH);
    fz_writer_begin(&w, TZG_S_SEGCOST);   fz_writer_write(&w, cost, ns * sizeof(tz_segcost)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_FIRST);     fz_writer_write(&w, ch.first, ((size_t)N + 1) * sizeof(uint32_t)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_EDGES);     fz_writer_write(&w, ch.edges, (size_t)ch.n_edges * sizeof(tz_chedge)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_NODE_ROAD); fz_writer_write(&w, node_road, (size_t)N * sizeof(uint32_t)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_ROAD_CH);   fz_writer_write(&w, road_ch, (size_t)R * sizeof(uint32_t)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_ARRIVE);    fz_writer_write(&w, arr_out, arrive.n * sizeof(tz_keynode)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_DEPART);    fz_writer_write(&w, dep_out, depart.n * sizeof(tz_keynode)); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_NODE_DEG);  fz_writer_write(&w, deg, R); fz_writer_end(&w);
    fz_writer_begin(&w, TZG_S_SEGFLAGS);  fz_writer_write(&w, sflags, ns); fz_writer_end(&w);
    w.h.count[TZG_C_PROFILE] = (uint32_t)p;
    w.h.count[TZG_C_NODES] = N;
    w.h.count[TZG_C_ROAD_NODES] = R;
    w.h.count[TZG_C_SEGS] = (uint32_t)ns;
    w.h.count[TZG_C_EDGES] = ch.n_edges;
    w.h.count[TZG_C_ARRIVE] = (uint32_t)arrive.n;
    w.h.count[TZG_C_DEPART] = (uint32_t)depart.n;
    w.h.count[TZG_C_ROADS_CRC] = b->roads_crc;
    fz_writer_close(&w, NULL);

    fz_ch_free(&ch);
    free(cost);
    free(is_barrier);
    free(deg);
    free(sflags);
    free(node_road);
    free(road_ch);
    free(arr_out);
    free(dep_out);
    fz_vec_free(arrive);
    fz_vec_free(depart);
    fz_vec_free(copy_orig);
}
