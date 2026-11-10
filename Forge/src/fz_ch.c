/*
 * Contraction hierarchy construction (Geisberger et al. 2008).
 *
 * Nodes are removed ("contracted") one at a time, least important first.
 * When node v is removed, every path u -> v -> x that might be a shortest
 * path gets a shortcut edge u -> x, unless a "witness" path u ~> x that
 * avoids v is at least as short. The order in which nodes are contracted is
 * their rank. A query then only ever has to move to higher ranks.
 *
 * Importance (priority, lower = contract earlier), as used by OSRM:
 *   2 * added/removed edges + added/removed original edges ("hops") + depth
 * evaluated lazily: a popped node is re-evaluated and pushed back if it is
 * no longer the cheapest.
 */
#include "fz_ch.h"
#include "fz_util.h"

#include <stdlib.h>
#include <string.h>

#define F_OUT      1u
#define F_SHORTCUT 2u

#define SIM_SETTLE_LIMIT  100
#define REAL_SETTLE_LIMIT 1000

typedef struct {
    uint32_t other, w, payload, flags;
    uint32_t hops;   /* number of original arcs this edge stands for */
} adj;

typedef struct {
    adj      *arena;
    size_t    arena_n, arena_cap;
    size_t   *off;
    uint32_t *len, *cap;
} dyngraph;

static void dg_add(dyngraph *g, uint32_t node, adj e)
{
    if (g->len[node] == g->cap[node]) {
        uint32_t nc = g->cap[node] ? g->cap[node] * 2 : 4;
        if (g->arena_n + nc > g->arena_cap) {
            size_t c = g->arena_cap ? g->arena_cap : 1024;
            while (c < g->arena_n + nc) c += c / 2;
            g->arena = (adj *)fz_realloc(g->arena, c * sizeof(adj));
            g->arena_cap = c;
        }
        memcpy(g->arena + g->arena_n, g->arena + g->off[node], g->len[node] * sizeof(adj));
        g->off[node] = g->arena_n;
        g->arena_n += nc;
        g->cap[node] = nc;
    }
    g->arena[g->off[node] + g->len[node]++] = e;
}

/* Remove the entry pointing to `other` with direction flag `out`. */
static void dg_remove(dyngraph *g, uint32_t node, uint32_t other, uint32_t out)
{
    adj *a = g->arena + g->off[node];
    uint32_t n = g->len[node];
    for (uint32_t i = 0; i < n; i++) {
        if (a[i].other == other && (a[i].flags & F_OUT) == out) {
            a[i] = a[--n];
            i--;
        }
    }
    g->len[node] = n;
}

static adj *dg_find(dyngraph *g, uint32_t node, uint32_t other, uint32_t out)
{
    adj *a = g->arena + g->off[node];
    for (uint32_t i = 0; i < g->len[node]; i++)
        if (a[i].other == other && (a[i].flags & F_OUT) == out) return &a[i];
    return NULL;
}

/* ------------------------------------------------------------ heaps */

typedef struct { int64_t key; uint32_t node; } hitem;
typedef struct { hitem *a; size_t n, cap; } iheap;

static void ih_push(iheap *h, int64_t key, uint32_t node)
{
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 1024;
        h->a = (hitem *)fz_realloc(h->a, h->cap * sizeof(hitem));
    }
    size_t i = h->n++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (h->a[p].key < key || (h->a[p].key == key && h->a[p].node <= node)) break;
        h->a[i] = h->a[p];
        i = p;
    }
    h->a[i].key = key;
    h->a[i].node = node;
}

static hitem ih_pop(iheap *h)
{
    hitem top = h->a[0];
    hitem last = h->a[--h->n];
    size_t i = 0, n = h->n;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && (h->a[c + 1].key < h->a[c].key ||
                          (h->a[c + 1].key == h->a[c].key && h->a[c + 1].node < h->a[c].node))) c++;
        if (last.key < h->a[c].key || (last.key == h->a[c].key && last.node <= h->a[c].node)) break;
        h->a[i] = h->a[c];
        i = c;
    }
    if (n) h->a[i] = last;
    return top;
}

/* ---------------------------------------------------------- witness */

typedef struct {
    dyngraph  g;
    uint32_t  n;
    uint32_t *dist, *stamp, cur;
    uint32_t *tstamp;        /* == cur: node is a witness target not yet settled */
    iheap     wh;
    uint32_t *del_nb, *level;
    uint8_t  *contracted;
    FZ_VEC(adj) tmp_in;
    FZ_VEC(adj) tmp_out;
} chstate;

static inline uint32_t wdist(chstate *s, uint32_t x)
{
    return s->stamp[x] == s->cur ? s->dist[x] : UINT32_MAX;
}

/* Dijkstra from src, never entering `skip`, stopping once every target is
 * settled, at maxd, or after `limit` settled nodes. Targets are the out
 * neighbours of `skip` (except src). Results are read with wdist(). */
static void witness(chstate *s, uint32_t src, uint32_t skip, uint32_t maxd, int limit)
{
    s->cur++;
    if (s->cur == 0) {
        memset(s->stamp, 0, s->n * sizeof(uint32_t));
        memset(s->tstamp, 0, s->n * sizeof(uint32_t));
        s->cur = 1;
    }
    uint32_t targets = 0;
    for (size_t j = 0; j < s->tmp_out.n; j++) {
        uint32_t x = s->tmp_out.a[j].other;
        if (x != src && s->tstamp[x] != s->cur) { s->tstamp[x] = s->cur; targets++; }
    }
    s->wh.n = 0;
    s->stamp[src] = s->cur;
    s->dist[src] = 0;
    ih_push(&s->wh, 0, src);
    int settled = 0;
    while (s->wh.n) {
        hitem it = ih_pop(&s->wh);
        uint32_t u = it.node;
        if ((uint32_t)it.key != s->dist[u]) continue;
        if ((uint32_t)it.key > maxd) break;
        if (s->tstamp[u] == s->cur) {
            s->tstamp[u] = 0;
            if (--targets == 0) break;  /* every target has its final distance */
        }
        if (++settled > limit) break;
        adj *a = s->g.arena + s->g.off[u];
        uint32_t len = s->g.len[u];
        for (uint32_t i = 0; i < len; i++) {
            if (!(a[i].flags & F_OUT)) continue;
            uint32_t v = a[i].other;
            if (v == skip) continue;
            uint64_t nd = (uint64_t)it.key + a[i].w;
            if (nd > maxd) continue;
            if (nd < wdist(s, v)) {
                s->stamp[v] = s->cur;
                s->dist[v] = (uint32_t)nd;
                ih_push(&s->wh, (int64_t)nd, v);
            }
        }
    }
}

static void add_or_update_shortcut(chstate *s, uint32_t u, uint32_t x, uint32_t w, uint32_t mid, uint32_t hops)
{
    adj *o = dg_find(&s->g, u, x, F_OUT);
    if (o) {
        if (o->w <= w) return;
        o->w = w; o->payload = mid; o->flags = F_OUT | F_SHORTCUT; o->hops = hops;
        adj *i = dg_find(&s->g, x, u, 0);
        if (!i) fz_die("internal: CH adjacency out of sync");
        i->w = w; i->payload = mid; i->flags = F_SHORTCUT; i->hops = hops;
        return;
    }
    adj ao = { x, w, mid, F_OUT | F_SHORTCUT, hops };
    adj ai = { u, w, mid, F_SHORTCUT, hops };
    dg_add(&s->g, u, ao);
    dg_add(&s->g, x, ai);
}

/* Count (simulate) or add the shortcuts needed to contract v. Reports the
 * edges removed and the original arcs ("hops") added and removed. */
static int contract_node(chstate *s, uint32_t v, int simulate, int *removed,
                         uint32_t *hops_added, uint32_t *hops_removed)
{
    s->tmp_in.n = s->tmp_out.n = 0;
    adj *a = s->g.arena + s->g.off[v];
    for (uint32_t i = 0; i < s->g.len[v]; i++) {
        if (a[i].flags & F_OUT) fz_vec_push(s->tmp_out, a[i]);
        else fz_vec_push(s->tmp_in, a[i]);
    }
    *removed = (int)(s->tmp_in.n + s->tmp_out.n);
    uint32_t hr = 0, ha = 0;
    for (size_t i = 0; i < s->tmp_in.n; i++) hr += s->tmp_in.a[i].hops;
    for (size_t i = 0; i < s->tmp_out.n; i++) hr += s->tmp_out.a[i].hops;
    int added = 0;
    int limit = simulate ? SIM_SETTLE_LIMIT : REAL_SETTLE_LIMIT;
    for (size_t i = 0; i < s->tmp_in.n; i++) {
        uint32_t u = s->tmp_in.a[i].other, w1 = s->tmp_in.a[i].w;
        uint32_t maxw2 = 0;
        int any = 0;
        for (size_t j = 0; j < s->tmp_out.n; j++)
            if (s->tmp_out.a[j].other != u) { any = 1; if (s->tmp_out.a[j].w > maxw2) maxw2 = s->tmp_out.a[j].w; }
        if (!any) continue;
        uint64_t maxd = (uint64_t)w1 + maxw2;
        if (maxd > TZ_E_WMASK) maxd = TZ_E_WMASK;
        witness(s, u, v, (uint32_t)maxd, limit);
        for (size_t j = 0; j < s->tmp_out.n; j++) {
            uint32_t x = s->tmp_out.a[j].other;
            if (x == u) continue;
            uint64_t need = (uint64_t)w1 + s->tmp_out.a[j].w;
            if (need > TZ_E_WMASK) need = TZ_E_WMASK;
            if (wdist(s, x) <= need) continue; /* witness found */
            added++;
            uint32_t hops = s->tmp_in.a[i].hops + s->tmp_out.a[j].hops;
            ha += hops;
            if (!simulate) add_or_update_shortcut(s, u, x, (uint32_t)need, v, hops);
        }
    }
    *hops_added = ha;
    *hops_removed = hr;
    return added;
}

static int64_t priority(chstate *s, uint32_t v)
{
    int removed;
    uint32_t ha, hr;
    int added = contract_node(s, v, 1, &removed, &ha, &hr);
    double eq = removed ? (double)added / removed : 0.0;
    double hq = hr ? (double)ha / hr : 0.0;
    /* fixed point, 1/1000 units */
    return (int64_t)(1000.0 * (2.0 * eq + hq) + 1000.0 * s->level[v]);
}

/* ------------------------------------------------------------ build */

static int cmp_arc(const void *pa, const void *pb)
{
    const fz_arc *a = (const fz_arc *)pa, *b = (const fz_arc *)pb;
    if (a->u != b->u) return a->u < b->u ? -1 : 1;
    if (a->v != b->v) return a->v < b->v ? -1 : 1;
    if (a->w != b->w) return a->w < b->w ? -1 : 1;
    return (a->payload > b->payload) - (a->payload < b->payload);
}

typedef struct { uint32_t at, target, wf, payload; } outedge;

static int cmp_outedge(const void *pa, const void *pb)
{
    const outedge *a = (const outedge *)pa, *b = (const outedge *)pb;
    if (a->target != b->target) return a->target < b->target ? -1 : 1;
    if (a->payload != b->payload) return a->payload < b->payload ? -1 : 1;
    uint32_t wa = a->wf & (TZ_E_WMASK | TZ_E_SHORTCUT), wb = b->wf & (TZ_E_WMASK | TZ_E_SHORTCUT);
    return (wa > wb) - (wa < wb);
}

void fz_ch_build(uint32_t n, fz_arc *arcs, size_t m, fz_ch *out)
{
    memset(out, 0, sizeof *out);
    out->n_nodes = n;

    /* Drop self loops and parallel arcs (keep the cheapest). */
    qsort(arcs, m, sizeof(fz_arc), cmp_arc);
    size_t w = 0;
    for (size_t i = 0; i < m; i++) {
        if (arcs[i].u == arcs[i].v) continue;
        if (w && arcs[w - 1].u == arcs[i].u && arcs[w - 1].v == arcs[i].v) continue;
        if (arcs[i].w == 0) arcs[i].w = 1;
        if (arcs[i].w > TZ_E_WMASK) arcs[i].w = TZ_E_WMASK;
        arcs[w++] = arcs[i];
    }
    m = w;

    chstate s;
    memset(&s, 0, sizeof s);
    s.n = n;
    s.g.off = (size_t *)fz_calloc(n ? n : 1, sizeof(size_t));
    s.g.len = (uint32_t *)fz_calloc(n ? n : 1, sizeof(uint32_t));
    s.g.cap = (uint32_t *)fz_calloc(n ? n : 1, sizeof(uint32_t));
    s.g.arena_cap = m * 2 + 1024;
    s.g.arena = (adj *)fz_malloc(s.g.arena_cap * sizeof(adj));
    /* Pre-size each node exactly to avoid early reallocation. */
    for (size_t i = 0; i < m; i++) { s.g.cap[arcs[i].u]++; s.g.cap[arcs[i].v]++; }
    size_t pos = 0;
    for (uint32_t v = 0; v < n; v++) {
        uint32_t c = s.g.cap[v] + 2; /* room for a couple of shortcuts */
        s.g.off[v] = pos;
        s.g.cap[v] = c;
        pos += c;
    }
    if (pos > s.g.arena_cap) { s.g.arena_cap = pos + 1024; s.g.arena = (adj *)fz_realloc(s.g.arena, s.g.arena_cap * sizeof(adj)); }
    s.g.arena_n = pos;
    for (size_t i = 0; i < m; i++) {
        adj ao = { arcs[i].v, arcs[i].w, arcs[i].payload, F_OUT, 1 };
        adj ai = { arcs[i].u, arcs[i].w, arcs[i].payload, 0, 1 };
        dg_add(&s.g, arcs[i].u, ao);
        dg_add(&s.g, arcs[i].v, ai);
    }

    s.dist = (uint32_t *)fz_malloc((n ? n : 1) * sizeof(uint32_t));
    s.stamp = (uint32_t *)fz_calloc(n ? n : 1, sizeof(uint32_t));
    s.tstamp = (uint32_t *)fz_calloc(n ? n : 1, sizeof(uint32_t));
    s.del_nb = (uint32_t *)fz_calloc(n ? n : 1, sizeof(uint32_t));
    s.level = (uint32_t *)fz_calloc(n ? n : 1, sizeof(uint32_t));
    s.contracted = (uint8_t *)fz_calloc(n ? n : 1, 1);

    iheap pq;
    memset(&pq, 0, sizeof pq);
    for (uint32_t v = 0; v < n; v++) {
        ih_push(&pq, priority(&s, v), v);
        if ((v & 0xFFF) == 0) fz_progress(0.15 * v / (double)n);
    }

    out->rank = (uint32_t *)fz_malloc((n ? n : 1) * sizeof(uint32_t));
    out->first = (uint32_t *)fz_malloc(((size_t)n + 1) * sizeof(uint32_t));
    FZ_VEC(outedge) oe;
    memset(&oe, 0, sizeof oe);

    uint32_t next_rank = 0;
    uint64_t shortcuts = 0;
    while (pq.n) {
        hitem it = ih_pop(&pq);
        uint32_t v = it.node;
        if (s.contracted[v]) continue;
        int64_t p = priority(&s, v);
        if (pq.n && p > pq.a[0].key) { ih_push(&pq, p, v); continue; }

        /* Record v's remaining edges: they all lead to higher ranks. */
        out->first[next_rank] = (uint32_t)oe.n;
        adj *a = s.g.arena + s.g.off[v];
        for (uint32_t i = 0; i < s.g.len[v]; i++) {
            outedge e;
            e.at = v;
            e.target = a[i].other;
            uint32_t sc = (a[i].flags & F_SHORTCUT) ? TZ_E_SHORTCUT : 0;
            if (a[i].flags & F_OUT) {
                e.wf = a[i].w | TZ_E_FWD | sc;
                e.payload = a[i].payload;
            } else {
                e.wf = a[i].w | TZ_E_BWD | sc;
                /* original arc u -> v seen from v: geometric direction flips */
                e.payload = sc ? a[i].payload : (a[i].payload ^ 1u);
            }
            fz_vec_push(oe, e);
        }
        out->rank[v] = next_rank++;

        int removed;
        uint32_t ha, hr;
        shortcuts += (uint64_t)contract_node(&s, v, 0, &removed, &ha, &hr);
        s.contracted[v] = 1;

        /* Detach v from its neighbours and update their bookkeeping. */
        a = s.g.arena + s.g.off[v];
        for (uint32_t i = 0; i < s.g.len[v]; i++) {
            uint32_t x = a[i].other;
            dg_remove(&s.g, x, v, (a[i].flags & F_OUT) ? 0 : F_OUT);
            s.del_nb[x]++;
            if (s.level[x] < s.level[v] + 1) s.level[x] = s.level[v] + 1;
        }
        s.g.len[v] = 0;
        if ((next_rank & 0x3FF) == 0) fz_progress(0.15 + 0.85 * next_rank / (double)n);
    }
    out->first[n] = (uint32_t)oe.n;
    if (oe.n > 0xFFFFFFF0u) fz_die("graph too large for this format");

    /* Map node ids to ranks, then sort and merge each node's edges. */
    for (size_t i = 0; i < oe.n; i++) {
        oe.a[i].target = out->rank[oe.a[i].target];
        if (oe.a[i].wf & TZ_E_SHORTCUT) oe.a[i].payload = out->rank[oe.a[i].payload];
    }
    out->edges = (tz_chedge *)fz_malloc((oe.n ? oe.n : 1) * sizeof(tz_chedge));
    uint32_t ne = 0;
    for (uint32_t r = 0; r < n; r++) {
        uint32_t b = out->first[r], e = out->first[r + 1];
        out->first[r] = ne;
        qsort(oe.a + b, e - b, sizeof(outedge), cmp_outedge);
        for (uint32_t i = b; i < e; i++) {
            outedge *x = &oe.a[i];
            if (x->target <= r) fz_die("internal: CH edge does not go upwards");
            if (ne > out->first[r]) {
                tz_chedge *pv = &out->edges[ne - 1];
                uint32_t pk = pv->wf & (TZ_E_WMASK | TZ_E_SHORTCUT), xk = x->wf & (TZ_E_WMASK | TZ_E_SHORTCUT);
                if (pv->target == x->target && pv->payload == x->payload && pk == xk) {
                    pv->wf |= x->wf & (TZ_E_FWD | TZ_E_BWD);
                    continue;
                }
            }
            out->edges[ne].target = x->target;
            out->edges[ne].wf = x->wf;
            out->edges[ne].payload = x->payload;
            ne++;
        }
    }
    out->first[n] = ne;
    out->n_edges = ne;
    out->shortcuts = shortcuts;

    fz_vec_free(oe);
    free(pq.a);
    free(s.g.arena);
    free(s.g.off);
    free(s.g.len);
    free(s.g.cap);
    free(s.dist);
    free(s.stamp);
    free(s.tstamp);
    free(s.del_nb);
    free(s.level);
    free(s.contracted);
    free(s.wh.a);
    fz_vec_free(s.tmp_in);
    fz_vec_free(s.tmp_out);
    fz_progress(1.0);
}

void fz_ch_free(fz_ch *ch)
{
    free(ch->rank);
    free(ch->first);
    free(ch->edges);
    memset(ch, 0, sizeof *ch);
}
