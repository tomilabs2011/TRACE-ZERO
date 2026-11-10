/* Reading OSM data, splitting ways into segments and writing roads.tzr. */
#include "fz_build.h"
#include "../../Core/tz_core.h"
#include "../../Core/tz_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ pass 1 */

static uint32_t spool_add_opt(fz_build *b, const char *s) { return s ? fz_pool_add(&b->spool, s) : 0; }

/* Alternative names joined by spaces (normalised later). */
static uint32_t collect_extra(fz_build *b, const fz_tag *t, int n, const char *label)
{
    static const char *const keys[] = { "alt_name", "old_name", "official_name", "short_name",
                                        "int_name", "name:en", "brand", "operator", "loc_name", NULL };
    char buf[1024];
    size_t len = 0;
    buf[0] = 0;
    if (label) {
        size_t l = strlen(label);
        if (l < sizeof buf - 1) { memcpy(buf, label, l); len = l; buf[len] = 0; }
    }
    for (const char *const *k = keys; *k; k++) {
        const char *v = fz_tag_get(t, n, *k);
        if (!v) continue;
        size_t l = strlen(v);
        if (len + l + 2 >= sizeof buf) break;
        if (len) buf[len++] = ' ';
        memcpy(buf + len, v, l);
        len += l;
        buf[len] = 0;
    }
    if (!len) return 0;
    char norm[1024];
    tz_normalize(buf, norm, sizeof norm);
    return fz_pool_add(&b->spool, norm);
}

static double parse_population(const char *v)
{
    if (!v) return 0;
    double p = 0;
    for (const char *c = v; *c; c++) {
        if (*c >= '0' && *c <= '9') p = p * 10 + (*c - '0');
        else if (*c == ',' || *c == '.' || *c == ' ' || *c == '\'') continue;
        else break;
    }
    return p;
}

static uint16_t place_rank(int type, double pop)
{
    static const uint16_t base[] = { 60000, 52000, 44000, 36000, 42000, 34000, 30000, 38000 };
    double r = base[type];
    if (pop > 0) { double bonus = 800.0 * log10(pop + 1); r += bonus > 5000 ? 5000 : bonus; }
    return (uint16_t)(r > 65535 ? 65535 : r);
}

/* Shared by nodes and ways: places, addresses and POIs. */
static void add_search_items(fz_build *b, const fz_tag *t, int n, int32_t lat, int32_t lon,
                             uint64_t ref0, uint32_t nref, int is_node)
{
    const char *name = fz_tag_get(t, n, "name");
    const char *place = fz_tag_get(t, n, "place");
    int ptype = fz_place_type(place);
    if (ptype >= 0 && name && is_node) {
        fz_place pl;
        pl.lat = lat; pl.lon = lon;
        pl.pop = parse_population(fz_tag_get(t, n, "population"));
        pl.name = fz_pool_add(&b->spool, name);
        pl.type = (int8_t)ptype;
        fz_vec_push(b->places, pl);
        fz_sitem s;
        memset(&s, 0, sizeof s);
        s.lat = lat; s.lon = lon;
        s.kind = TZ_KIND_PLACE;
        s.sub = (uint8_t)ptype;
        s.rank = place_rank(ptype, pl.pop);
        s.name = pl.name;
        s.extra = collect_extra(b, t, n, NULL);
        fz_vec_push(b->sitems, s);
        return; /* a place node is not also an address or POI */
    }
    const char *hn = fz_tag_get(t, n, "addr:housenumber");
    const char *street = fz_tag_get(t, n, "addr:street");
    const char *aplace = fz_tag_get(t, n, "addr:place");
    fz_poi_info poi;
    int is_poi = name && fz_poi_parse(t, n, &poi);
    int is_addr = hn && (street || aplace);
    if (is_addr) {
        fz_sitem s;
        memset(&s, 0, sizeof s);
        s.lat = lat; s.lon = lon; s.ref0 = ref0; s.nref = nref;
        s.kind = TZ_KIND_ADDRESS;
        s.rank = 16000;
        s.street = fz_pool_add(&b->spool, street ? street : aplace);
        s.hn = fz_pool_add(&b->spool, hn);
        s.postcode = spool_add_opt(b, fz_tag_get(t, n, "addr:postcode"));
        s.city = spool_add_opt(b, fz_tag_get(t, n, "addr:city"));
        fz_vec_push(b->sitems, s);
    }
    if (is_poi) {
        fz_sitem s;
        memset(&s, 0, sizeof s);
        s.lat = lat; s.lon = lon; s.ref0 = ref0; s.nref = nref;
        s.kind = TZ_KIND_POI;
        s.sub = (uint8_t)poi.group;
        uint32_t r = 20000u + poi.rank_bonus;
        s.rank = (uint16_t)(r > 65535 ? 65535 : r);
        s.name = fz_pool_add(&b->spool, name);
        s.label = fz_pool_add(&b->spool, poi.label);
        if (is_addr) {
            s.street = fz_pool_add(&b->spool, street ? street : aplace);
            s.hn = fz_pool_add(&b->spool, hn);
        }
        s.city = spool_add_opt(b, fz_tag_get(t, n, "addr:city"));
        s.extra = collect_extra(b, t, n, poi.label);
        fz_vec_push(b->sitems, s);
    }
}

static int32_t nd_to_e6(int64_t nd)
{
    /* nanodegrees -> microdegrees, rounded half away from zero */
    return (int32_t)(nd >= 0 ? (nd + 500) / 1000 : (nd - 500) / 1000);
}

static void p1_node(void *ctx, int64_t id, int64_t lat, int64_t lon, const fz_tag *t, int n)
{
    fz_build *b = (fz_build *)ctx;
    uint8_t bm = fz_barrier_mask(t, n);
    if (bm) { fz_vec_push(b->barrier_ids, id); fz_vec_push(b->barrier_masks, bm); }
    if (fz_is_traffic_signal(t, n)) fz_vec_push(b->signal_ids, id);
    if (b->with_search) add_search_items(b, t, n, nd_to_e6(lat), nd_to_e6(lon), 0, 0, 1);
}

static void p1_way(void *ctx, int64_t id, const int64_t *refs, int nref, const fz_tag *t, int n)
{
    fz_build *b = (fz_build *)ctx;
    if (nref < 2) return;
    fz_way_attr attr;
    int routable = fz_way_attr_parse(t, n, &attr);
    int searchable = 0;
    if (b->with_search && !routable) {
        const char *name = fz_tag_get(t, n, "name");
        fz_poi_info poi;
        searchable = (fz_tag_get(t, n, "addr:housenumber") &&
                      (fz_tag_get(t, n, "addr:street") || fz_tag_get(t, n, "addr:place"))) ||
                     (name && fz_poi_parse(t, n, &poi));
    }
    if (!routable && !searchable) return;
    uint64_t ref0 = b->refs.n;
    fz_vec_grow(b->refs, b->refs.n + (size_t)nref);
    memcpy(b->refs.a + b->refs.n, refs, (size_t)nref * sizeof(int64_t));
    b->refs.n += (size_t)nref;
    if (routable) {
        fz_rway w;
        memset(&w, 0, sizeof w);
        w.id = id;
        w.ref0 = ref0;
        w.nref = (uint32_t)nref;
        w.attr = attr;
        const char *name = fz_tag_get(t, n, "name");
        const char *ref = fz_tag_get(t, n, "ref");
        w.name = name ? fz_pool_add(&b->rpool, name) : 0;
        w.ref = ref ? fz_pool_add(&b->rpool, ref) : 0;
        fz_vec_push(b->rways, w);
    } else {
        add_search_items(b, t, n, FZ_MISSING, FZ_MISSING, ref0, (uint32_t)nref, 0);
    }
}

static void p1_relation(void *ctx, int64_t id, const fz_member *m, int nm, const fz_tag *t, int n)
{
    (void)id;
    fz_build *b = (fz_build *)ctx;
    fz_rkind kind;
    fz_rturn turn;
    uint8_t mask = fz_restriction_parse(t, n, &kind, &turn);
    mask &= (uint8_t)b->profiles;
    if (!mask) return;
    int64_t from = 0, to = 0, via = 0;
    int nf = 0, nt = 0, nv = 0;
    for (int i = 0; i < nm; i++) {
        if (strcmp(m[i].role, "from") == 0) { if (m[i].type != 1) return; from = m[i].ref; nf++; }
        else if (strcmp(m[i].role, "to") == 0) { if (m[i].type != 1) return; to = m[i].ref; nt++; }
        else if (strcmp(m[i].role, "via") == 0) { if (m[i].type != 0) return; via = m[i].ref; nv++; }
    }
    if (nf != 1 || nt != 1 || nv != 1) return; /* via-way and multi-member restrictions are skipped */
    fz_restr r = { from, to, via, mask, (uint8_t)kind, (uint8_t)turn };
    fz_vec_push(b->restr, r);
}

/* ============================================================ pass 2 */

static void p2_node(void *ctx, int64_t id, int64_t lat, int64_t lon, const fz_tag *t, int n)
{
    (void)t; (void)n;
    fz_build *b = (fz_build *)ctx;
    /* Nodes usually arrive sorted by id: advance a cursor, fall back to
     * binary search if the file is not sorted. */
    size_t i = b->pass2_cursor;
    if (i < b->n_need && b->need[i] < id) {
        size_t step = 1;
        while (i + step < b->n_need && b->need[i + step] < id) { i += step; step *= 2; }
        while (i < b->n_need && b->need[i] < id) i++;
    } else if (i < b->n_need && b->need[i] > id) {
        size_t k = fz_bsearch_i64(b->need, b->n_need, id);
        if (k == SIZE_MAX) return;
        i = k;
    }
    b->pass2_cursor = i;
    if (i >= b->n_need || b->need[i] != id) return;
    b->nlat[i] = nd_to_e6(lat);
    b->nlon[i] = nd_to_e6(lon);
}

void fz_build_read(fz_build *b)
{
    fz_pool_init(&b->rpool);
    fz_pool_init(&b->spool);

    fz_stage(1, 8, "Reading ways, places and restrictions");
    fz_pbf_handler h;
    memset(&h, 0, sizeof h);
    h.ctx = b;
    h.want_nodes = 1;
    h.want_ways = 1;
    h.want_relations = 1;
    h.node = p1_node;
    h.way = p1_way;
    h.relation = p1_relation;
    fz_pbf_read(b->input, &h, &b->info);
    fz_log("  %zu routable ways, %zu restrictions, %zu search items, %zu places",
           b->rways.n, b->restr.n, b->sitems.n, b->places.n);
    if (!b->rways.n) fz_die("no roads found in %s", b->input);

    fz_stage(2, 8, "Reading node coordinates");
    size_t nrefs = b->refs.n;
    int64_t *need = (int64_t *)fz_malloc((nrefs ? nrefs : 1) * sizeof(int64_t));
    memcpy(need, b->refs.a, nrefs * sizeof(int64_t));
    b->n_need = fz_sort_unique_i64(need, nrefs);
    b->need = (int64_t *)fz_realloc(need, (b->n_need ? b->n_need : 1) * sizeof(int64_t));
    b->nlat = (int32_t *)fz_malloc((b->n_need ? b->n_need : 1) * sizeof(int32_t));
    b->nlon = (int32_t *)fz_malloc((b->n_need ? b->n_need : 1) * sizeof(int32_t));
    for (size_t i = 0; i < b->n_need; i++) b->nlat[i] = b->nlon[i] = FZ_MISSING;
    memset(&h, 0, sizeof h);
    h.ctx = b;
    h.want_nodes = 2;
    h.node = p2_node;
    b->pass2_cursor = 0;
    fz_pbf_read(b->input, &h, NULL);

    /* refs -> need indices */
    if (b->n_need > 0xFFFFFFF0u) fz_die("too many nodes for this format");
    b->refidx = (uint32_t *)fz_malloc((nrefs ? nrefs : 1) * sizeof(uint32_t));
    size_t missing = 0;
    for (size_t i = 0; i < nrefs; i++) {
        size_t k = fz_bsearch_i64(b->need, b->n_need, b->refs.a[i]);
        b->refidx[i] = (uint32_t)k;
        if (b->nlat[k] == FZ_MISSING) { b->refidx[i] = UINT32_MAX; missing++; }
    }
    if (missing) fz_log("  %zu way nodes are missing from the extract (ignored)", missing);
    fz_vec_free(b->refs);

    /* node flags: signals and barriers */
    b->nflags = (uint8_t *)fz_calloc(b->n_need ? b->n_need : 1, 1);
    for (size_t i = 0; i < b->signal_ids.n; i++) {
        size_t k = fz_bsearch_i64(b->need, b->n_need, b->signal_ids.a[i]);
        if (k != SIZE_MAX) b->nflags[k] |= 1;
    }
    for (size_t i = 0; i < b->barrier_ids.n; i++) {
        size_t k = fz_bsearch_i64(b->need, b->n_need, b->barrier_ids.a[i]);
        if (k != SIZE_MAX) b->nflags[k] |= (uint8_t)(b->barrier_masks.a[i] << 1);
    }
    fz_vec_free(b->signal_ids);
    fz_vec_free(b->barrier_ids);
    fz_vec_free(b->barrier_masks);

    fz_map64_init(&b->way_index, b->rways.n);
    for (size_t i = 0; i < b->rways.n; i++) fz_map64_put(&b->way_index, b->rways.a[i].id, (uint32_t)i);
}

/* ======================================================== segmentation */

static double ref_dist(const fz_build *b, uint32_t i, uint32_t j)
{
    return tz_distance_m(b->nlat[i] / 1e6, b->nlon[i] / 1e6, b->nlat[j] / 1e6, b->nlon[j] / 1e6);
}

static void emit_segment(fz_build *b, uint32_t w, uint32_t r0, uint32_t r1)
{
    const fz_rway *rw = &b->rways.a[w];
    const uint32_t *ri = b->refidx + rw->ref0;
    fz_bseg s;
    memset(&s, 0, sizeof s);
    s.way = w;
    s.a = b->roadid[ri[r0]];
    s.b = b->roadid[ri[r1]];
    s.r0 = r0;
    s.r1 = r1;
    double len = 0;
    uint32_t prev = ri[r0];
    uint32_t interior = 0;
    for (uint32_t k = r0 + 1; k <= r1; k++) {
        uint32_t cur = ri[k];
        if (cur == prev) continue;
        len += ref_dist(b, prev, cur);
        if (k < r1) { interior++; if (b->nflags[cur] & 1) s.sig_mid = 1; }
        prev = cur;
    }
    if (interior > 65535) fz_die("way %lld has a segment with more than 65535 points", (long long)rw->id);
    s.len_m = len;
    b->rways.a[w].len_m += len;
    fz_vec_push(b->segs, s);
}

void fz_build_segments(fz_build *b)
{
    fz_stage(3, 8, "Splitting roads into segments");
    size_t nn = b->n_need;
    uint8_t *use = (uint8_t *)fz_calloc(nn ? nn : 1, 1);
    uint8_t *junction = (uint8_t *)fz_calloc(nn ? nn : 1, 1);
    for (size_t w = 0; w < b->rways.n; w++) {
        const fz_rway *rw = &b->rways.a[w];
        const uint32_t *ri = b->refidx + rw->ref0;
        uint32_t prev = UINT32_MAX;
        for (uint32_t k = 0; k < rw->nref; k++) {
            uint32_t i = ri[k];
            if (i == UINT32_MAX) {
                /* the node before a gap must end a segment */
                if (prev != UINT32_MAX) junction[prev] = 1;
                prev = UINT32_MAX;
                continue;
            }
            if (i == prev) continue;
            if (use[i] < 255) use[i]++;
            if (prev == UINT32_MAX) junction[i] = 1; /* first node, or first after a gap */
            prev = i;
        }
        if (prev != UINT32_MAX) junction[prev] = 1;
    }
    uint32_t nroad = 0;
    for (size_t i = 0; i < nn; i++) {
        if (use[i] >= 2 || (b->nflags[i] >> 1)) junction[i] |= use[i] ? 1 : 0;
        if (junction[i] && use[i]) nroad++;
        else junction[i] = 0;
    }
    free(use);

    /* Road node ids in Z-order for memory locality on the phone. */
    uint32_t *jl = (uint32_t *)fz_malloc((nroad ? nroad : 1) * sizeof(uint32_t));
    uint64_t *key = (uint64_t *)fz_malloc((nn ? nn : 1) * sizeof(uint64_t));
    uint32_t k = 0;
    for (size_t i = 0; i < nn; i++) {
        if (!junction[i]) continue;
        jl[k++] = (uint32_t)i;
        key[i] = fz_morton(b->nlat[i], b->nlon[i]);
    }
    fz_sort_idx_by_u64(jl, nroad, key);
    b->roadid = (uint32_t *)fz_malloc((nn ? nn : 1) * sizeof(uint32_t));
    for (size_t i = 0; i < nn; i++) b->roadid[i] = UINT32_MAX;
    b->n_road = nroad;
    b->road_lat = (int32_t *)fz_malloc((nroad ? nroad : 1) * sizeof(int32_t));
    b->road_lon = (int32_t *)fz_malloc((nroad ? nroad : 1) * sizeof(int32_t));
    b->road_need = (uint32_t *)fz_malloc((nroad ? nroad : 1) * sizeof(uint32_t));
    for (uint32_t r = 0; r < nroad; r++) {
        b->roadid[jl[r]] = r;
        b->road_lat[r] = b->nlat[jl[r]];
        b->road_lon[r] = b->nlon[jl[r]];
        b->road_need[r] = jl[r];
    }
    free(jl);
    free(junction);

    /* Cut every way at its junctions. */
    for (size_t w = 0; w < b->rways.n; w++) {
        const fz_rway *rw = &b->rways.a[w];
        const uint32_t *ri = b->refidx + rw->ref0;
        int64_t start = -1;
        for (uint32_t r = 0; r < rw->nref; r++) {
            uint32_t i = ri[r];
            if (i == UINT32_MAX) { start = -1; continue; }
            if (b->roadid[i] == UINT32_MAX) continue;
            if (start >= 0 && !(ri[start] == i && r == (uint32_t)start + 1))
                emit_segment(b, (uint32_t)w, (uint32_t)start, r);
            start = r;
        }
        if ((w & 0xFFFF) == 0) fz_progress(0.5 * w / (double)b->rways.n);
    }
    if (b->segs.n >= 0x7FFFFFFFu) fz_die("too many road segments for this format");

    /* Sort segments in Z-order of their midpoint. */
    size_t ns = b->segs.n;
    uint32_t *perm = (uint32_t *)fz_malloc((ns ? ns : 1) * sizeof(uint32_t));
    uint64_t *skey = (uint64_t *)fz_malloc((ns ? ns : 1) * sizeof(uint64_t));
    for (size_t i = 0; i < ns; i++) {
        const fz_bseg *s = &b->segs.a[i];
        int32_t mlat = (int32_t)(((int64_t)b->road_lat[s->a] + b->road_lat[s->b]) / 2);
        int32_t mlon = (int32_t)(((int64_t)b->road_lon[s->a] + b->road_lon[s->b]) / 2);
        perm[i] = (uint32_t)i;
        skey[i] = fz_morton(mlat, mlon);
    }
    fz_sort_idx_by_u64(perm, ns, skey);
    fz_bseg *sorted = (fz_bseg *)fz_malloc((ns ? ns : 1) * sizeof(fz_bseg));
    for (size_t i = 0; i < ns; i++) sorted[i] = b->segs.a[perm[i]];
    free(b->segs.a);
    b->segs.a = sorted;
    b->segs.cap = ns;
    free(perm);
    free(skey);
    free(key);

    /* CSR: way -> segments, road node -> segments */
    size_t nw = b->rways.n;
    b->way_seg_first = (uint32_t *)fz_calloc(nw + 1, sizeof(uint32_t));
    b->way_segs = (uint32_t *)fz_malloc((ns ? ns : 1) * sizeof(uint32_t));
    for (size_t i = 0; i < ns; i++) b->way_seg_first[b->segs.a[i].way + 1]++;
    for (size_t w = 0; w < nw; w++) b->way_seg_first[w + 1] += b->way_seg_first[w];
    uint32_t *fill = (uint32_t *)fz_malloc((nw + 1) * sizeof(uint32_t));
    memcpy(fill, b->way_seg_first, (nw + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < ns; i++) b->way_segs[fill[b->segs.a[i].way]++] = (uint32_t)i;
    free(fill);

    b->node_seg_first = (uint32_t *)fz_calloc((size_t)nroad + 1, sizeof(uint32_t));
    b->node_segs = (uint32_t *)fz_malloc((ns ? ns * 2 : 1) * sizeof(uint32_t));
    for (size_t i = 0; i < ns; i++) {
        b->node_seg_first[b->segs.a[i].a + 1]++;
        if (b->segs.a[i].b != b->segs.a[i].a) b->node_seg_first[b->segs.a[i].b + 1]++;
    }
    for (uint32_t r = 0; r < nroad; r++) b->node_seg_first[r + 1] += b->node_seg_first[r];
    fill = (uint32_t *)fz_malloc(((size_t)nroad + 1) * sizeof(uint32_t));
    memcpy(fill, b->node_seg_first, ((size_t)nroad + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < ns; i++) {
        b->node_segs[fill[b->segs.a[i].a]++] = (uint32_t)i;
        if (b->segs.a[i].b != b->segs.a[i].a) b->node_segs[fill[b->segs.a[i].b]++] = (uint32_t)i;
    }
    free(fill);

    /* bounding box of the road network */
    b->bbox[0] = b->bbox[1] = INT32_MAX;
    b->bbox[2] = b->bbox[3] = INT32_MIN;
    for (uint32_t r = 0; r < nroad; r++) {
        if (b->road_lat[r] < b->bbox[0]) b->bbox[0] = b->road_lat[r];
        if (b->road_lon[r] < b->bbox[1]) b->bbox[1] = b->road_lon[r];
        if (b->road_lat[r] > b->bbox[2]) b->bbox[2] = b->road_lat[r];
        if (b->road_lon[r] > b->bbox[3]) b->bbox[3] = b->road_lon[r];
    }
    fz_log("  %u junctions, %zu segments", nroad, ns);
    fz_progress(1.0);
}

/* =========================================================== roads.tzr */

/* Iterate the distinct points of a segment (including both ends). */
typedef struct { const fz_build *b; const uint32_t *ri; uint32_t k, r1, prev; } segit;

static void segit_init(segit *it, const fz_build *b, const fz_bseg *s)
{
    it->b = b;
    it->ri = b->refidx + b->rways.a[s->way].ref0;
    it->k = s->r0;
    it->r1 = s->r1;
    it->prev = UINT32_MAX;
}

static int segit_next(segit *it, uint32_t *need_idx)
{
    while (it->k <= it->r1) {
        uint32_t i = it->ri[it->k++];
        if (i == it->prev) continue;
        it->prev = i;
        *need_idx = i;
        return 1;
    }
    return 0;
}

void fz_build_write_roads(fz_build *b, const char *path)
{
    fz_stage(4, 8, "Writing road geometry");
    size_t ns = b->segs.n;
    fz_writer w;
    fz_writer_open(&w, path, TZ_MAGIC_ROADS);

    /* nodes */
    fz_writer_begin(&w, TZR_S_NODES);
    for (uint32_t r = 0; r < b->n_road; r++) {
        tz_point p = { b->road_lat[r], b->road_lon[r] };
        fz_writer_write(&w, &p, sizeof p);
    }
    fz_writer_end(&w);

    /* geometry first (to know offsets), kept in memory */
    fz_bytes geom;
    memset(&geom, 0, sizeof geom);
    tz_seg *out = (tz_seg *)fz_malloc((ns ? ns : 1) * sizeof(tz_seg));
    for (size_t i = 0; i < ns; i++) {
        const fz_bseg *s = &b->segs.a[i];
        const fz_rway *rw = &b->rways.a[s->way];
        tz_seg *o = &out[i];
        memset(o, 0, sizeof *o);
        o->a = s->a;
        o->b = s->b;
        o->name = rw->name;
        o->ref = rw->ref;
        double dm = s->len_m * 10.0 + 0.5;
        o->len_dm = dm > 4e9 ? 4000000000u : (uint32_t)dm;
        o->cls = rw->attr.cls;
        o->flags = rw->attr.flags;
        if (geom.n > 0xFFFFFF00u) fz_die("road geometry exceeds 4 GB");
        o->geom = (uint32_t)geom.n;
        /* Count the distinct points; everything but the two ends is interior. */
        segit it;
        segit_init(&it, b, s);
        uint32_t idx = 0, total = 0;
        while (segit_next(&it, &idx)) total++;
        uint32_t interior = total >= 2 ? total - 2 : 0;
        segit_init(&it, b, s);
        segit_next(&it, &idx); /* node a */
        int32_t plat = b->nlat[idx], plon = b->nlon[idx];
        for (uint32_t j = 0; j < interior; j++) {
            segit_next(&it, &idx);
            fz_put_varint(&geom, tz_zigzag_enc(b->nlat[idx] - plat));
            fz_put_varint(&geom, tz_zigzag_enc(b->nlon[idx] - plon));
            plat = b->nlat[idx];
            plon = b->nlon[idx];
        }
        o->npts = (uint16_t)interior;
        if (!interior) o->geom = 0;
    }
    fz_writer_begin(&w, TZR_S_SEGS);
    fz_writer_write(&w, out, ns * sizeof(tz_seg));
    fz_writer_end(&w);
    fz_writer_begin(&w, TZR_S_GEOM);
    fz_writer_write(&w, geom.a, geom.n);
    fz_writer_end(&w);
    fz_writer_begin(&w, TZR_S_STRINGS);
    fz_writer_write(&w, b->rpool.buf, b->rpool.len);
    fz_writer_end(&w);
    fz_progress(0.5);

    /* spatial grid */
    double lat_span = (b->bbox[2] - b->bbox[0]) + 1.0, lon_span = (b->bbox[3] - b->bbox[1]) + 1.0;
    uint32_t cell = 5000; /* 0.005 degrees, ~550 m */
    while ((lat_span / cell + 1) * (lon_span / cell + 1) > 4e6) cell *= 2;
    int32_t lat0 = b->bbox[0] - (int32_t)cell, lon0 = b->bbox[1] - (int32_t)cell;
    uint32_t gw = (uint32_t)((lon_span + 2.0 * cell) / cell) + 1;
    uint32_t gh = (uint32_t)((lat_span + 2.0 * cell) / cell) + 1;
    uint64_t cells = (uint64_t)gw * gh;
    uint32_t *start = (uint32_t *)fz_calloc((size_t)cells + 1, sizeof(uint32_t));
    FZ_VEC(uint32_t) gseg;
    memset(&gseg, 0, sizeof gseg);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t *fill = NULL;
        if (pass == 1) {
            for (uint64_t c = 0; c < cells; c++) start[c + 1] += start[c];
            fz_vec_grow(gseg, start[cells]);
            gseg.n = start[cells];
            fill = (uint32_t *)fz_malloc(((size_t)cells + 1) * sizeof(uint32_t));
            memcpy(fill, start, ((size_t)cells + 1) * sizeof(uint32_t));
        }
        for (size_t i = 0; i < ns; i++) {
            segit it;
            segit_init(&it, b, &b->segs.a[i]);
            uint32_t idx;
            int32_t mnla = INT32_MAX, mnlo = INT32_MAX, mxla = INT32_MIN, mxlo = INT32_MIN;
            while (segit_next(&it, &idx)) {
                if (b->nlat[idx] < mnla) mnla = b->nlat[idx];
                if (b->nlat[idx] > mxla) mxla = b->nlat[idx];
                if (b->nlon[idx] < mnlo) mnlo = b->nlon[idx];
                if (b->nlon[idx] > mxlo) mxlo = b->nlon[idx];
            }
            uint32_t r0 = (uint32_t)((mnla - lat0) / (int64_t)cell), r1 = (uint32_t)((mxla - lat0) / (int64_t)cell);
            uint32_t c0 = (uint32_t)((mnlo - lon0) / (int64_t)cell), c1 = (uint32_t)((mxlo - lon0) / (int64_t)cell);
            for (uint32_t r = r0; r <= r1 && r < gh; r++)
                for (uint32_t c = c0; c <= c1 && c < gw; c++) {
                    uint64_t ci = (uint64_t)r * gw + c;
                    if (pass == 0) start[ci + 1]++;
                    else gseg.a[fill[ci]++] = (uint32_t)i;
                }
        }
        free(fill);
    }
    fz_writer_begin(&w, TZR_S_GRID_START);
    fz_writer_write(&w, start, ((size_t)cells + 1) * sizeof(uint32_t));
    fz_writer_end(&w);
    fz_writer_begin(&w, TZR_S_GRID_SEGS);
    fz_writer_write(&w, gseg.a, gseg.n * sizeof(uint32_t));
    fz_writer_end(&w);

    w.h.count[TZR_C_NODES] = b->n_road;
    w.h.count[TZR_C_SEGS] = (uint32_t)ns;
    w.h.count[TZR_C_GRID_W] = gw;
    w.h.count[TZR_C_GRID_H] = gh;
    w.h.count[TZR_C_GRID_CELL] = cell;
    w.h.ival[TZR_I_GRID_LAT0] = lat0;
    w.h.ival[TZR_I_GRID_LON0] = lon0;
    w.h.ival[TZR_I_MINLAT] = b->bbox[0];
    w.h.ival[TZR_I_MINLON] = b->bbox[1];
    w.h.ival[TZR_I_MAXLAT] = b->bbox[2];
    w.h.ival[TZR_I_MAXLON] = b->bbox[3];
    tz_file_header hdr;
    fz_writer_close(&w, &hdr);
    b->roads_crc = tz_header_checksum(&hdr);

    free(out);
    fz_vec_free(geom);
    free(start);
    fz_vec_free(gseg);
    fz_progress(1.0);
}
