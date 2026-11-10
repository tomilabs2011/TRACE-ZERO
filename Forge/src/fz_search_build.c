/* Builds search.tzs: places, streets, addresses and points of interest. */
#include "fz_build.h"
#include "../../Core/tz_core.h"
#include "../../Core/tz_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------- settlement lookup */

typedef struct {
    int32_t   lat0, lon0, cell;
    uint32_t  w, h;
    uint32_t *start, *ids;
} placegrid;

static void pg_build(placegrid *g, const fz_build *b)
{
    memset(g, 0, sizeof *g);
    int32_t mnla = INT32_MAX, mnlo = INT32_MAX, mxla = INT32_MIN, mxlo = INT32_MIN;
    for (size_t i = 0; i < b->places.n; i++) {
        const fz_place *p = &b->places.a[i];
        if (p->lat < mnla) mnla = p->lat;
        if (p->lat > mxla) mxla = p->lat;
        if (p->lon < mnlo) mnlo = p->lon;
        if (p->lon > mxlo) mxlo = p->lon;
    }
    g->cell = 50000; /* 0.05 degrees */
    if (!b->places.n) { mnla = mxla = mnlo = mxlo = 0; }
    while (((int64_t)(mxla - mnla) / g->cell + 1) * ((int64_t)(mxlo - mnlo) / g->cell + 1) > 4000000) g->cell *= 2;
    g->lat0 = mnla;
    g->lon0 = mnlo;
    g->w = (uint32_t)((int64_t)(mxlo - mnlo) / g->cell + 1);
    g->h = (uint32_t)((int64_t)(mxla - mnla) / g->cell + 1);
    size_t cells = (size_t)g->w * g->h;
    g->start = (uint32_t *)fz_calloc(cells + 1, sizeof(uint32_t));
    g->ids = (uint32_t *)fz_malloc((b->places.n ? b->places.n : 1) * sizeof(uint32_t));
    for (size_t i = 0; i < b->places.n; i++) {
        const fz_place *p = &b->places.a[i];
        size_t c = (size_t)((p->lat - g->lat0) / g->cell) * g->w + (size_t)((p->lon - g->lon0) / g->cell);
        g->start[c + 1]++;
    }
    for (size_t c = 0; c < cells; c++) g->start[c + 1] += g->start[c];
    uint32_t *fill = (uint32_t *)fz_malloc((cells + 1) * sizeof(uint32_t));
    memcpy(fill, g->start, (cells + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < b->places.n; i++) {
        const fz_place *p = &b->places.a[i];
        size_t c = (size_t)((p->lat - g->lat0) / g->cell) * g->w + (size_t)((p->lon - g->lon0) / g->cell);
        g->ids[fill[c]++] = (uint32_t)i;
    }
    free(fill);
}

static void pg_free(placegrid *g) { free(g->start); free(g->ids); }

static double place_radius(int type)
{
    switch (type) {
    case TZ_PLACE_CITY: return 25000;
    case TZ_PLACE_TOWN: return 10000;
    case TZ_PLACE_VILLAGE: return 4000;
    case TZ_PLACE_HAMLET: return 2000;
    case TZ_PLACE_SUBURB: return 3000;
    case TZ_PLACE_NEIGHBOURHOOD: return 1500;
    default: return 0;
    }
}

/* Nearest place whose type is in `mask` (bit per TZ_PLACE_*); -1 if none. */
static int64_t pg_nearest(const placegrid *g, const fz_build *b, int32_t lat, int32_t lon,
                          unsigned mask, int64_t exclude)
{
    if (!b->places.n || lat == FZ_MISSING) return -1;
    double la = lat / 1e6;
    double coslat = cos(la * TZ_DEG2RAD);
    if (coslat < 0.05) coslat = 0.05;
    double rmax = 25000;
    int64_t dr = (int64_t)(rmax / 111195.0 * 1e6 / g->cell) + 1;
    int64_t dc = (int64_t)(rmax / (111195.0 * coslat) * 1e6 / g->cell) + 1;
    int64_t r0 = (lat - g->lat0) / g->cell, c0 = (lon - g->lon0) / g->cell;
    if (lat < g->lat0) r0 = -1;
    if (lon < g->lon0) c0 = -1;
    double best = 1.0;
    int64_t best_i = -1;
    for (int64_t r = r0 - dr; r <= r0 + dr; r++) {
        if (r < 0 || r >= (int64_t)g->h) continue;
        for (int64_t c = c0 - dc; c <= c0 + dc; c++) {
            if (c < 0 || c >= (int64_t)g->w) continue;
            size_t cell = (size_t)r * g->w + (size_t)c;
            for (uint32_t k = g->start[cell]; k < g->start[cell + 1]; k++) {
                uint32_t i = g->ids[k];
                if ((int64_t)i == exclude) continue;
                const fz_place *p = &b->places.a[i];
                if (!(mask & (1u << p->type))) continue;
                double rad = place_radius(p->type);
                if (rad <= 0) continue;
                double d = tz_distance_m(la, lon / 1e6, p->lat / 1e6, p->lon / 1e6);
                double score = d / rad;
                if (score < best) { best = score; best_i = i; }
            }
        }
    }
    return best_i;
}

#define SETTLEMENTS ((1u << TZ_PLACE_CITY) | (1u << TZ_PLACE_TOWN) | (1u << TZ_PLACE_VILLAGE) | (1u << TZ_PLACE_HAMLET))
#define DISTRICTS   ((1u << TZ_PLACE_SUBURB) | (1u << TZ_PLACE_NEIGHBOURHOOD))

/* ------------------------------------------------------------ entries */

typedef struct {
    int32_t  lat, lon;
    uint32_t name, ctx, extra;   /* offsets in the output pool */
    uint16_t rank;
    uint8_t  kind, sub;
    uint32_t order;              /* tie breaker */
} entry;

static int cmp_entry(const void *pa, const void *pb)
{
    const entry *a = (const entry *)pa, *b = (const entry *)pb;
    if (a->rank != b->rank) return a->rank > b->rank ? -1 : 1;
    return (a->order > b->order) - (a->order < b->order);
}

typedef FZ_VEC(entry) entry_vec;

static void join2(char *out, size_t cap, const char *a, const char *sep, const char *b)
{
    if (a && *a && b && *b) snprintf(out, cap, "%s%s%s", a, sep, b);
    else if (a && *a) snprintf(out, cap, "%s", a);
    else if (b && *b) snprintf(out, cap, "%s", b);
    else out[0] = 0;
}

static const char *place_label(int type)
{
    static const char *const L[] = { "City", "Town", "Village", "Hamlet", "District", "Neighbourhood", "Locality", "Place" };
    return (type >= 0 && type <= 7) ? L[type] : "Place";
}

/* "District, Settlement" around a point. */
static void area_context(const placegrid *g, const fz_build *b, int32_t lat, int32_t lon, char *out, size_t cap,
                         int64_t exclude)
{
    int64_t s = pg_nearest(g, b, lat, lon, SETTLEMENTS, exclude);
    int64_t d = pg_nearest(g, b, lat, lon, DISTRICTS, exclude);
    const char *sn = s >= 0 ? fz_pool_get(&b->spool, b->places.a[s].name) : NULL;
    const char *dn = d >= 0 ? fz_pool_get(&b->spool, b->places.a[d].name) : NULL;
    if (sn && dn && strcmp(sn, dn) == 0) dn = NULL;
    join2(out, cap, dn, ", ", sn);
}

static void centroid(const fz_build *b, uint64_t ref0, uint32_t nref, int32_t *lat, int32_t *lon)
{
    double sla = 0, slo = 0;
    uint32_t n = 0;
    /* closed rings repeat the first node; skip the last ref then */
    const uint32_t *ri = b->refidx + ref0;
    uint32_t m = (nref > 1 && ri[0] == ri[nref - 1]) ? nref - 1 : nref;
    for (uint32_t k = 0; k < m; k++) {
        uint32_t i = ri[k];
        if (i == UINT32_MAX) continue;
        sla += b->nlat[i];
        slo += b->nlon[i];
        n++;
    }
    if (!n) { *lat = *lon = FZ_MISSING; return; }
    *lat = (int32_t)llround(sla / n);
    *lon = (int32_t)llround(slo / n);
}

/* --------------------------------------------------------- streets */

typedef struct { uint32_t name, node, way; } sref;

static int cmp_sref(const void *pa, const void *pb)
{
    const sref *a = (const sref *)pa, *b = (const sref *)pb;
    if (a->name != b->name) return a->name < b->name ? -1 : 1;
    if (a->node != b->node) return a->node < b->node ? -1 : 1;
    return (a->way > b->way) - (a->way < b->way);
}

static uint32_t uf_find(uint32_t *p, uint32_t x)
{
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}

static void add_streets(fz_build *b, const placegrid *g, fz_pool *out, entry_vec *ev)
{
    size_t nw = b->rways.n;
    FZ_VEC(sref) refs;
    memset(&refs, 0, sizeof refs);
    for (size_t w = 0; w < nw; w++) {
        const fz_rway *rw = &b->rways.a[w];
        if (!rw->name || (rw->attr.flags & TZ_SEG_FERRY)) continue;
        const uint32_t *ri = b->refidx + rw->ref0;
        for (uint32_t k = 0; k < rw->nref; k++) {
            uint32_t i = ri[k];
            if (i == UINT32_MAX || b->roadid[i] == UINT32_MAX) continue;
            fz_vec_push(refs, ((sref){ rw->name, i, (uint32_t)w }));
        }
    }
    qsort(refs.a, refs.n, sizeof(sref), cmp_sref);
    uint32_t *par = (uint32_t *)fz_malloc((nw ? nw : 1) * sizeof(uint32_t));
    for (size_t w = 0; w < nw; w++) par[w] = (uint32_t)w;
    for (size_t i = 1; i < refs.n; i++) {
        if (refs.a[i].name == refs.a[i - 1].name && refs.a[i].node == refs.a[i - 1].node) {
            uint32_t x = uf_find(par, refs.a[i].way), y = uf_find(par, refs.a[i - 1].way);
            if (x != y) par[x] = y;
        }
    }
    fz_vec_free(refs);

    /* per cluster root: longest member and best class */
    uint32_t *best = (uint32_t *)fz_malloc((nw ? nw : 1) * sizeof(uint32_t));
    uint8_t *bestcls = (uint8_t *)fz_malloc(nw ? nw : 1);
    for (size_t w = 0; w < nw; w++) { best[w] = UINT32_MAX; bestcls[w] = 255; }
    for (size_t w = 0; w < nw; w++) {
        const fz_rway *rw = &b->rways.a[w];
        if (!rw->name || (rw->attr.flags & TZ_SEG_FERRY)) continue;
        uint32_t r = uf_find(par, (uint32_t)w);
        if (best[r] == UINT32_MAX || rw->len_m > b->rways.a[best[r]].len_m) best[r] = (uint32_t)w;
        if (rw->attr.cls < bestcls[r]) bestcls[r] = rw->attr.cls;
    }
    size_t count = 0;
    for (size_t r = 0; r < nw; r++) {
        if (best[r] == UINT32_MAX || uf_find(par, (uint32_t)r) != r) continue;
        const fz_rway *rw = &b->rways.a[best[r]];
        if (rw->len_m <= 0) continue;
        /* midpoint along the representative way */
        const uint32_t *ri = b->refidx + rw->ref0;
        double half = rw->len_m / 2, acc = 0;
        uint32_t prev = UINT32_MAX, pick = UINT32_MAX;
        for (uint32_t k = 0; k < rw->nref; k++) {
            uint32_t i = ri[k];
            if (i == UINT32_MAX) continue;
            if (prev != UINT32_MAX) acc += tz_distance_m(b->nlat[prev] / 1e6, b->nlon[prev] / 1e6, b->nlat[i] / 1e6, b->nlon[i] / 1e6);
            prev = i;
            pick = i;
            if (acc >= half) break;
        }
        if (pick == UINT32_MAX) continue;
        entry e;
        memset(&e, 0, sizeof e);
        e.lat = b->nlat[pick];
        e.lon = b->nlon[pick];
        e.kind = TZ_KIND_STREET;
        e.sub = bestcls[r];
        uint16_t bonus = bestcls[r] <= TZ_CLS_PRIMARY ? 4000 : bestcls[r] == TZ_CLS_SECONDARY ? 3000 :
                         bestcls[r] == TZ_CLS_TERTIARY ? 2000 : bestcls[r] <= TZ_CLS_RESIDENTIAL ? 1000 : 0;
        e.rank = (uint16_t)(24000 + bonus);
        e.name = fz_pool_add(out, fz_pool_get(&b->rpool, rw->name));
        char ctx[512];
        area_context(g, b, e.lat, e.lon, ctx, sizeof ctx, -1);
        e.ctx = fz_pool_add(out, ctx);
        if (rw->ref) {
            char norm[256];
            tz_normalize(fz_pool_get(&b->rpool, rw->ref), norm, sizeof norm);
            e.extra = fz_pool_add(out, norm);
        }
        e.order = (uint32_t)ev->n;
        fz_vec_push(*ev, e);
        count++;
    }
    fz_log("  %zu streets", count);
    free(par);
    free(best);
    free(bestcls);
}

/* ------------------------------------------------------------ tokens */

typedef struct {
    uint32_t str;      /* offset in token pool */
    uint32_t last, count;
    fz_bytes post;
} tokacc;

static const fz_pool *g_tokpool;
static const tokacc *g_toks;

static int cmp_tokid(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return strcmp(fz_pool_get(g_tokpool, g_toks[a].str), fz_pool_get(g_tokpool, g_toks[b].str));
}

void fz_build_search(fz_build *b, const char *path)
{
    fz_stage(8, 8, "Building search index");
    placegrid g;
    pg_build(&g, b);
    fz_pool out;
    fz_pool_init(&out);
    entry_vec ev;
    memset(&ev, 0, sizeof ev);

    /* places, addresses and POIs collected in pass 1 */
    size_t nplace = 0, naddr = 0, npoi = 0, place_idx = 0;
    for (size_t i = 0; i < b->sitems.n; i++) {
        fz_sitem *s = &b->sitems.a[i];
        /* PLACE items were added in the same order as b->places */
        int64_t self = s->kind == TZ_KIND_PLACE ? (int64_t)place_idx++ : -1;
        int32_t lat = s->lat, lon = s->lon;
        if (s->nref) centroid(b, s->ref0, s->nref, &lat, &lon);
        if (lat == FZ_MISSING) continue;
        entry e;
        memset(&e, 0, sizeof e);
        e.lat = lat;
        e.lon = lon;
        e.kind = s->kind;
        e.sub = s->sub;
        e.rank = s->rank;
        char name[512], ctx[512], area[512], tmp[512];
        const char *city = s->city ? fz_pool_get(&b->spool, s->city) : NULL;
        if (s->kind == TZ_KIND_PLACE) {
            snprintf(name, sizeof name, "%s", fz_pool_get(&b->spool, s->name));
            if (s->sub == TZ_PLACE_SUBURB || s->sub == TZ_PLACE_NEIGHBOURHOOD || s->sub == TZ_PLACE_LOCALITY ||
                s->sub == TZ_PLACE_OTHER) {
                int64_t st = pg_nearest(&g, b, lat, lon, SETTLEMENTS, self);
                join2(ctx, sizeof ctx, place_label(s->sub), " · ",
                      st >= 0 ? fz_pool_get(&b->spool, b->places.a[st].name) : NULL);
            } else {
                snprintf(ctx, sizeof ctx, "%s", place_label(s->sub));
            }
            nplace++;
        } else if (s->kind == TZ_KIND_ADDRESS) {
            join2(name, sizeof name, fz_pool_get(&b->spool, s->street), " ", fz_pool_get(&b->spool, s->hn));
            if (city) snprintf(area, sizeof area, "%s", city);
            else area_context(&g, b, lat, lon, area, sizeof area, -1);
            join2(ctx, sizeof ctx, s->postcode ? fz_pool_get(&b->spool, s->postcode) : NULL, " ", area);
            naddr++;
        } else {
            snprintf(name, sizeof name, "%s", fz_pool_get(&b->spool, s->name));
            if (city) snprintf(area, sizeof area, "%s", city);
            else area_context(&g, b, lat, lon, area, sizeof area, -1);
            if (s->street) {
                join2(tmp, sizeof tmp, fz_pool_get(&b->spool, s->street), " ", fz_pool_get(&b->spool, s->hn));
                char where[512];
                join2(where, sizeof where, tmp, ", ", area);
                join2(ctx, sizeof ctx, fz_pool_get(&b->spool, s->label), " · ", where);
            } else {
                join2(ctx, sizeof ctx, fz_pool_get(&b->spool, s->label), " · ", area);
            }
            npoi++;
        }
        e.name = fz_pool_add(&out, name);
        e.ctx = fz_pool_add(&out, ctx);
        e.extra = s->extra ? fz_pool_add(&out, fz_pool_get(&b->spool, s->extra)) : 0;
        e.order = (uint32_t)ev.n;
        fz_vec_push(ev, e);
        if ((i & 0xFFFF) == 0) fz_progress(0.4 * i / (double)b->sitems.n);
    }
    fz_log("  %zu places, %zu addresses, %zu points of interest", nplace, naddr, npoi);
    add_streets(b, &g, &out, &ev);
    pg_free(&g);

    qsort(ev.a, ev.n, sizeof(entry), cmp_entry);
    if (ev.n > 0x7FFFFFFFu) fz_die("too many search entries");

    /* tokens and postings, entries in final id order */
    fz_pool tokpool;
    fz_pool_init(&tokpool);
    fz_map64 tokmap;
    fz_map64_init(&tokmap, 1 << 16);
    FZ_VEC(tokacc) toks;
    memset(&toks, 0, sizeof toks);
    char text[2048], norm[2048];
    for (size_t id = 0; id < ev.n; id++) {
        const entry *e = &ev.a[id];
        size_t len = tz_normalize(fz_pool_get(&out, e->name), norm, sizeof norm);
        memcpy(text, norm, len + 1);
        size_t l2 = tz_normalize(fz_pool_get(&out, e->ctx), norm, sizeof norm);
        if (len + l2 + 2 < sizeof text) { text[len] = ' '; memcpy(text + len + 1, norm, l2 + 1); len += l2 + 1; }
        if (e->extra) {
            const char *x = fz_pool_get(&out, e->extra);
            size_t l3 = strlen(x);
            if (len + l3 + 2 < sizeof text) { text[len] = ' '; memcpy(text + len + 1, x, l3 + 1); len += l3 + 1; }
        }
        /* split and deduplicate */
        const char *words[128];
        int nw = 0;
        char *p = text;
        while (*p && nw < 128) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *wstart = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
            int dup = 0;
            for (int k = 0; k < nw; k++) if (strcmp(words[k], wstart) == 0) { dup = 1; break; }
            if (!dup) words[nw++] = wstart;
        }
        for (int k = 0; k < nw; k++) {
            uint32_t off = fz_pool_add(&tokpool, words[k]);
            uint32_t t = fz_map64_get(&tokmap, off);
            if (t == UINT32_MAX) {
                t = (uint32_t)toks.n;
                tokacc ta;
                memset(&ta, 0, sizeof ta);
                ta.str = off;
                fz_vec_push(toks, ta);
                fz_map64_put(&tokmap, off, t);
            }
            tokacc *ta = &toks.a[t];
            fz_put_varint(&ta->post, (uint32_t)id - ta->last);
            ta->last = (uint32_t)id;
            ta->count++;
        }
        if ((id & 0xFFFF) == 0) fz_progress(0.4 + 0.5 * id / (double)ev.n);
    }
    fz_map64_free(&tokmap);

    uint32_t ntok = (uint32_t)toks.n;
    uint32_t *order = (uint32_t *)fz_malloc((ntok ? ntok : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < ntok; i++) order[i] = i;
    g_tokpool = &tokpool;
    g_toks = toks.a;
    qsort(order, ntok, sizeof(uint32_t), cmp_tokid);

    fz_writer w;
    fz_writer_open(&w, path, TZ_MAGIC_SEARCH);
    fz_writer_begin(&w, TZS_S_ENTRIES);
    for (size_t i = 0; i < ev.n; i++) {
        tz_sentry se;
        memset(&se, 0, sizeof se);
        se.lat = ev.a[i].lat;
        se.lon = ev.a[i].lon;
        se.name = ev.a[i].name;
        se.ctx = ev.a[i].ctx;
        se.extra = ev.a[i].extra;
        se.rank = ev.a[i].rank;
        se.kind = ev.a[i].kind;
        se.sub = ev.a[i].sub;
        fz_writer_write(&w, &se, sizeof se);
    }
    fz_writer_end(&w);
    fz_writer_begin(&w, TZS_S_STRINGS);
    fz_writer_write(&w, out.buf, out.len);
    fz_writer_end(&w);

    /* token table, token strings and postings in dictionary order */
    tz_stoken *tt = (tz_stoken *)fz_malloc((ntok ? ntok : 1) * sizeof(tz_stoken));
    fz_bytes tokstr, post;
    memset(&tokstr, 0, sizeof tokstr);
    memset(&post, 0, sizeof post);
    fz_vec_push(tokstr, 0);
    for (uint32_t k = 0; k < ntok; k++) {
        tokacc *ta = &toks.a[order[k]];
        const char *s = fz_pool_get(&tokpool, ta->str);
        size_t l = strlen(s) + 1;
        if (tokstr.n + l > 0xFFFFFF00u || post.n + ta->post.n > 0xFFFFFF00u) fz_die("search index exceeds 4 GB");
        tt[k].str = (uint32_t)tokstr.n;
        fz_vec_grow(tokstr, tokstr.n + l);
        memcpy(tokstr.a + tokstr.n, s, l);
        tokstr.n += l;
        tt[k].post = (uint32_t)post.n;
        tt[k].count = ta->count;
        fz_vec_grow(post, post.n + ta->post.n);
        memcpy(post.a + post.n, ta->post.a, ta->post.n);
        post.n += ta->post.n;
        fz_vec_free(ta->post);
    }
    fz_writer_begin(&w, TZS_S_TOKENS);
    fz_writer_write(&w, tt, (size_t)ntok * sizeof(tz_stoken));
    fz_writer_end(&w);
    fz_writer_begin(&w, TZS_S_TOKSTR);
    fz_writer_write(&w, tokstr.a, tokstr.n);
    fz_writer_end(&w);
    fz_writer_begin(&w, TZS_S_POSTINGS);
    fz_writer_write(&w, post.a, post.n);
    fz_writer_end(&w);

    /* spatial grid for reverse geocoding */
    int32_t mnla = INT32_MAX, mnlo = INT32_MAX, mxla = INT32_MIN, mxlo = INT32_MIN;
    for (size_t i = 0; i < ev.n; i++) {
        if (ev.a[i].lat < mnla) mnla = ev.a[i].lat;
        if (ev.a[i].lat > mxla) mxla = ev.a[i].lat;
        if (ev.a[i].lon < mnlo) mnlo = ev.a[i].lon;
        if (ev.a[i].lon > mxlo) mxlo = ev.a[i].lon;
    }
    if (!ev.n) { mnla = mxla = mnlo = mxlo = 0; }
    uint32_t cell = 10000;
    while (((double)(mxla - mnla) / cell + 1) * ((double)(mxlo - mnlo) / cell + 1) > 4e6) cell *= 2;
    uint32_t gw = (uint32_t)((int64_t)(mxlo - mnlo) / cell + 1), gh = (uint32_t)((int64_t)(mxla - mnla) / cell + 1);
    size_t cells = (size_t)gw * gh;
    uint32_t *start = (uint32_t *)fz_calloc(cells + 1, sizeof(uint32_t));
    uint32_t *gids = (uint32_t *)fz_malloc((ev.n ? ev.n : 1) * sizeof(uint32_t));
    for (size_t i = 0; i < ev.n; i++) {
        size_t c = (size_t)((ev.a[i].lat - mnla) / (int64_t)cell) * gw + (size_t)((ev.a[i].lon - mnlo) / (int64_t)cell);
        start[c + 1]++;
    }
    for (size_t c = 0; c < cells; c++) start[c + 1] += start[c];
    uint32_t *fill = (uint32_t *)fz_malloc((cells + 1) * sizeof(uint32_t));
    memcpy(fill, start, (cells + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < ev.n; i++) {
        size_t c = (size_t)((ev.a[i].lat - mnla) / (int64_t)cell) * gw + (size_t)((ev.a[i].lon - mnlo) / (int64_t)cell);
        gids[fill[c]++] = (uint32_t)i;
    }
    free(fill);
    fz_writer_begin(&w, TZS_S_GRID_START);
    fz_writer_write(&w, start, (cells + 1) * sizeof(uint32_t));
    fz_writer_end(&w);
    fz_writer_begin(&w, TZS_S_GRID_IDS);
    fz_writer_write(&w, gids, ev.n * sizeof(uint32_t));
    fz_writer_end(&w);

    w.h.count[TZS_C_ENTRIES] = (uint32_t)ev.n;
    w.h.count[TZS_C_TOKENS] = ntok;
    w.h.count[TZS_C_GRID_W] = gw;
    w.h.count[TZS_C_GRID_H] = gh;
    w.h.count[TZS_C_GRID_CELL] = cell;
    w.h.ival[TZS_I_GRID_LAT0] = mnla;
    w.h.ival[TZS_I_GRID_LON0] = mnlo;
    fz_writer_close(&w, NULL);
    fz_log("  %zu entries, %u distinct words", ev.n, ntok);

    free(start);
    free(gids);
    free(tt);
    free(order);
    fz_vec_free(tokstr);
    fz_vec_free(post);
    fz_vec_free(toks);
    fz_pool_free(&tokpool);
    fz_pool_free(&out);
    fz_vec_free(ev);
    fz_progress(1.0);
}
