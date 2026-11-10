/*
 * Offline geocoder.
 *
 * Index layout (see tz_format.h): entries sorted by importance, a sorted
 * token dictionary, and per token a delta-coded list of entry ids. A query:
 *   1. normalise and split the query into words (last word may be partial);
 *   2. find each word's dictionary range by prefix;
 *   3. take the word with the fewest postings as the driver and collect up
 *      to CAND_MAX candidate ids from its range, most important first;
 *   4. check every candidate against all words using its own text, scoring
 *      exact/prefix matches in the name higher than in the context;
 *   5. add importance and closeness to the reference point; keep the best.
 */
#include "tz_private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QTOK   8
#define QTOK_LEN   64
#define CAND_MAX   30000
#define TEXT_MAX   512
#define ETOK_MAX   48

struct tz_search {
    tz_mapped        m;
    const tz_sentry *ent;
    const char      *str;
    const tz_stoken *tok;
    const char      *tokstr;
    const uint8_t   *post;
    const uint32_t  *grid_start;
    const uint32_t  *grid_ids;
    uint64_t         str_size, tokstr_size, post_size, n_grid_ids;
    uint32_t         n_ent, n_tok, gw, gh, cell;
    int32_t          lat0, lon0;
};

static int ends_with_nul(const char *p, uint64_t size) { return size >= 1 && p && p[0] == 0 && p[size - 1] == 0; }

tz_search *tz_search_open(const char *path, int *status)
{
    int st_dummy;
    if (!status) status = &st_dummy;
    tz_search *s = (tz_search *)calloc(1, sizeof *s);
    if (!s) { *status = TZ_ERR_NOMEM; return NULL; }
    int st = tz_map_file(path, TZ_MAGIC_SEARCH, &s->m);
    if (st != TZ_OK) { free(s); *status = st; return NULL; }
    const tz_file_header *h = s->m.hdr;
    s->n_ent = h->count[TZS_C_ENTRIES];
    s->n_tok = h->count[TZS_C_TOKENS];
    s->gw = h->count[TZS_C_GRID_W];
    s->gh = h->count[TZS_C_GRID_H];
    s->cell = h->count[TZS_C_GRID_CELL];
    s->lat0 = h->ival[TZS_I_GRID_LAT0];
    s->lon0 = h->ival[TZS_I_GRID_LON0];
    s->ent = (const tz_sentry *)tz_section_ptr(&s->m, TZS_S_ENTRIES);
    s->str = (const char *)tz_section_ptr(&s->m, TZS_S_STRINGS);
    s->str_size = tz_section_size(&s->m, TZS_S_STRINGS);
    s->tok = (const tz_stoken *)tz_section_ptr(&s->m, TZS_S_TOKENS);
    s->tokstr = (const char *)tz_section_ptr(&s->m, TZS_S_TOKSTR);
    s->tokstr_size = tz_section_size(&s->m, TZS_S_TOKSTR);
    s->post = (const uint8_t *)tz_section_ptr(&s->m, TZS_S_POSTINGS);
    s->post_size = tz_section_size(&s->m, TZS_S_POSTINGS);
    s->grid_start = (const uint32_t *)tz_section_ptr(&s->m, TZS_S_GRID_START);
    s->grid_ids = (const uint32_t *)tz_section_ptr(&s->m, TZS_S_GRID_IDS);
    s->n_grid_ids = tz_section_size(&s->m, TZS_S_GRID_IDS) / 4;

    uint64_t cells = (uint64_t)s->gw * s->gh;
    int ok = tz_section_size(&s->m, TZS_S_ENTRIES) == (uint64_t)s->n_ent * sizeof(tz_sentry);
    ok &= tz_section_size(&s->m, TZS_S_TOKENS) == (uint64_t)s->n_tok * sizeof(tz_stoken);
    ok &= ends_with_nul(s->str, s->str_size);
    ok &= ends_with_nul(s->tokstr, s->tokstr_size);
    ok &= s->cell > 0 && s->gw > 0 && s->gh > 0 && cells < (1u << 28);
    ok &= tz_section_size(&s->m, TZS_S_GRID_START) == (cells + 1) * 4;
    if (!ok) {
        tz_unmap_file(&s->m);
        free(s);
        *status = TZ_ERR_FORMAT;
        return NULL;
    }
    *status = TZ_OK;
    return s;
}

void tz_search_close(tz_search *s)
{
    if (!s) return;
    tz_unmap_file(&s->m);
    free(s);
}

uint32_t tz_search_entry_count(const tz_search *s) { return s->n_ent; }

static const char *sstr(const tz_search *s, uint32_t off) { return off < s->str_size ? s->str + off : ""; }
static const char *tstr(const tz_search *s, uint32_t i)
{
    uint32_t off = s->tok[i].str;
    return off < s->tokstr_size ? s->tokstr + off : "";
}

/* [lo, hi) = tokens starting with q */
static void prefix_range(const tz_search *s, const char *q, uint32_t *lo_out, uint32_t *hi_out)
{
    size_t ql = strlen(q);
    uint32_t lo = 0, hi = s->n_tok;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (strcmp(tstr(s, mid), q) < 0) lo = mid + 1; else hi = mid;
    }
    uint32_t a = lo;
    hi = s->n_tok;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (strncmp(tstr(s, mid), q, ql) <= 0) lo = mid + 1; else hi = mid;
    }
    *lo_out = a;
    *hi_out = lo;
}

/* ------------------------------------------------- posting list cursors */

typedef struct { const uint8_t *p, *end; uint32_t left, cur; } cursor;

static int cursor_init(const tz_search *s, uint32_t tok, cursor *c)
{
    const tz_stoken *t = &s->tok[tok];
    if (t->post >= s->post_size || t->count == 0) return 0;
    c->p = s->post + t->post;
    c->end = s->post + s->post_size;
    c->left = t->count;
    c->cur = 0;
    uint32_t d;
    c->p = tz_varint_get(c->p, c->end, &d);
    c->cur = d;
    c->left--;
    return 1;
}

static int cursor_next(cursor *c)
{
    if (!c->left || c->p >= c->end) return 0;
    uint32_t d;
    c->p = tz_varint_get(c->p, c->end, &d);
    c->cur += d;
    c->left--;
    return 1;
}

typedef struct { cursor *a; uint32_t n; } cheap; /* min-heap of cursors by cur */

static void cheap_down(cheap *h, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, m = i;
        if (l < h->n && h->a[l].cur < h->a[m].cur) m = l;
        if (l + 1 < h->n && h->a[l + 1].cur < h->a[m].cur) m = l + 1;
        if (m == i) return;
        cursor t = h->a[i]; h->a[i] = h->a[m]; h->a[m] = t;
        i = m;
    }
}

/* Collect up to cap distinct ids (ascending = most important first). */
static uint32_t collect(const tz_search *s, uint32_t lo, uint32_t hi, uint32_t *out, uint32_t cap)
{
    uint32_t n = 0;
    if (hi - lo == 1) {
        cursor c;
        if (!cursor_init(s, lo, &c)) return 0;
        do { if (c.cur < s->n_ent) out[n++] = c.cur; } while (n < cap && cursor_next(&c));
        return n;
    }
    cheap h;
    h.a = (cursor *)malloc((size_t)(hi - lo) * sizeof(cursor));
    if (!h.a) return 0;
    h.n = 0;
    for (uint32_t t = lo; t < hi; t++)
        if (cursor_init(s, t, &h.a[h.n])) h.n++;
    for (int64_t i = (int64_t)h.n / 2 - 1; i >= 0; i--) cheap_down(&h, (uint32_t)i);
    uint32_t last = UINT32_MAX;
    while (h.n && n < cap) {
        uint32_t id = h.a[0].cur;
        if (id != last && id < s->n_ent) { out[n++] = id; last = id; }
        if (!cursor_next(&h.a[0])) h.a[0] = h.a[--h.n];
        cheap_down(&h, 0);
    }
    free(h.a);
    return n;
}

/* ---------------------------------------------------------- scoring */

typedef struct { const char *w[ETOK_MAX]; int n; } wordlist;

/* Split a normalised string in place into words. */
static void split_words(char *buf, wordlist *wl)
{
    char *p = buf;
    while (*p && wl->n < ETOK_MAX) {
        while (*p == ' ') p++;
        if (!*p) break;
        wl->w[wl->n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
}

/* 2 = exact word, 1 = prefix of a word, 0 = none */
static int best_match(const wordlist *wl, const char *q, size_t ql)
{
    int best = 0;
    for (int i = 0; i < wl->n; i++) {
        if (strncmp(wl->w[i], q, ql) == 0) {
            if (wl->w[i][ql] == 0) return 2;
            best = 1;
        }
    }
    return best;
}

static void insert_hit(tz_hit *hits, int *nh, int max, const tz_hit *h)
{
    int n = *nh;
    if (n == max && h->score <= hits[n - 1].score) return;
    int i = n < max ? n : max - 1;
    while (i > 0 && hits[i - 1].score < h->score) { hits[i] = hits[i - 1]; i--; }
    hits[i] = *h;
    if (n < max) (*nh)++;
}

int tz_search_query(const tz_search *s, const char *query, double ref_lat, double ref_lon,
                    int has_ref, tz_hit *hits, int max_hits)
{
    if (!s || !query || !hits || max_hits <= 0 || !s->n_tok) return 0;

    char qbuf[MAX_QTOK * QTOK_LEN];
    tz_normalize(query, qbuf, sizeof qbuf);
    char qt[MAX_QTOK][QTOK_LEN];
    size_t ql[MAX_QTOK];
    int nq = 0;
    {
        wordlist wl;
        wl.n = 0;
        split_words(qbuf, &wl);
        for (int i = 0; i < wl.n && nq < MAX_QTOK; i++) {
            size_t l = strlen(wl.w[i]);
            if (l >= QTOK_LEN) l = QTOK_LEN - 1;
            int dup = 0;
            for (int j = 0; j < nq; j++) if (ql[j] == l && memcmp(qt[j], wl.w[i], l) == 0) dup = 1;
            if (dup) continue;
            memcpy(qt[nq], wl.w[i], l);
            qt[nq][l] = 0;
            ql[nq] = l;
            nq++;
        }
    }
    if (nq == 0) return 0;

    /* Dictionary ranges and driver selection. */
    uint32_t lo[MAX_QTOK], hi[MAX_QTOK];
    int driver = -1;
    uint64_t driver_total = UINT64_MAX;
    for (int i = 0; i < nq; i++) {
        prefix_range(s, qt[i], &lo[i], &hi[i]);
        if (lo[i] >= hi[i]) return 0; /* a word that matches nothing */
        uint64_t total = 0;
        for (uint32_t k = lo[i]; k < hi[i] && total < 10000000ull; k++) total += s->tok[k].count;
        if (total < driver_total) { driver_total = total; driver = i; }
    }

    uint32_t *cand = (uint32_t *)malloc(CAND_MAX * sizeof(uint32_t));
    if (!cand) return 0;
    uint32_t nc = collect(s, lo[driver], hi[driver], cand, CAND_MAX);

    int nh = 0;
    char nbuf[TEXT_MAX], cbuf[TEXT_MAX];
    for (uint32_t ci = 0; ci < nc; ci++) {
        const tz_sentry *e = &s->ent[cand[ci]];
        wordlist wn, wc;
        wn.n = wc.n = 0;
        tz_normalize(sstr(s, e->name), nbuf, sizeof nbuf);
        split_words(nbuf, &wn);
        /* context + extra words in one list */
        size_t cl = tz_normalize(sstr(s, e->ctx), cbuf, sizeof cbuf);
        if (e->extra && cl + 2 < sizeof cbuf) {
            cbuf[cl] = ' ';
            strncpy(cbuf + cl + 1, sstr(s, e->extra), sizeof cbuf - cl - 2);
            cbuf[sizeof cbuf - 1] = 0;
        }
        split_words(cbuf, &wc);

        double match = 0.0;
        int in_name = 0, ok = 1;
        for (int i = 0; i < nq; i++) {
            int mn = best_match(&wn, qt[i], ql[i]);
            if (mn) { match += mn == 2 ? 30.0 : 20.0; in_name++; continue; }
            int mc = best_match(&wc, qt[i], ql[i]);
            if (mc) { match += mc == 2 ? 12.0 : 8.0; continue; }
            ok = 0;
            break;
        }
        if (!ok) continue;
        if (in_name == nq) {
            match += 25.0;
            if (wn.n == nq) match += 15.0; /* the query is the whole name */
        }
        double lat = e->lat / 1e6, lon = e->lon / 1e6;
        double score = match + 100.0 * e->rank / 65535.0;
        double dist = -1.0;
        if (has_ref) {
            dist = tz_distance_m(ref_lat, ref_lon, lat, lon);
            score -= 25.0 * log10(1.0 + dist / 1000.0);
        }
        tz_hit h;
        memset(&h, 0, sizeof h);
        h.id = cand[ci];
        h.kind = e->kind;
        h.sub = e->sub;
        h.lat = lat;
        h.lon = lon;
        h.dist_m = dist;
        h.score = score;
        h.name = sstr(s, e->name);
        h.context = sstr(s, e->ctx);

        /* Drop near-duplicates (same name and context within 300 m). */
        int dup = -1;
        for (int k = 0; k < nh; k++) {
            if (hits[k].kind == h.kind && strcmp(hits[k].name, h.name) == 0 &&
                strcmp(hits[k].context, h.context) == 0 &&
                tz_distance_m(hits[k].lat, hits[k].lon, h.lat, h.lon) < 300.0) { dup = k; break; }
        }
        if (dup >= 0) {
            if (hits[dup].score >= h.score) continue;
            for (int k = dup; k + 1 < nh; k++) hits[k] = hits[k + 1];
            nh--;
        }
        insert_hit(hits, &nh, max_hits, &h);
    }
    free(cand);
    return nh;
}

int tz_search_nearest(const tz_search *s, double lat, double lon, double radius_m, tz_hit *hit)
{
    if (!s || !hit || !(radius_m > 0)) return 0;
    double coslat = cos(lat * TZ_DEG2RAD);
    if (coslat < 0.01) coslat = 0.01;
    double dlat = radius_m / 111195.0, dlon = radius_m / (111195.0 * coslat);
    double c = (double)s->cell;
    int64_t r0 = (int64_t)floor(((lat - dlat) * 1e6 - s->lat0) / c);
    int64_t r1 = (int64_t)floor(((lat + dlat) * 1e6 - s->lat0) / c);
    int64_t c0 = (int64_t)floor(((lon - dlon) * 1e6 - s->lon0) / c);
    int64_t c1 = (int64_t)floor(((lon + dlon) * 1e6 - s->lon0) / c);
    if (r1 < 0 || c1 < 0 || r0 >= (int64_t)s->gh || c0 >= (int64_t)s->gw) return 0;
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 >= (int64_t)s->gh) r1 = s->gh - 1;
    if (c1 >= (int64_t)s->gw) c1 = s->gw - 1;
    double best = radius_m;
    uint32_t best_id = UINT32_MAX;
    for (int64_t row = r0; row <= r1; row++) {
        for (int64_t col = c0; col <= c1; col++) {
            uint64_t cell = (uint64_t)row * s->gw + (uint64_t)col;
            uint32_t a = s->grid_start[cell], b = s->grid_start[cell + 1];
            if (b > s->n_grid_ids || a > b) continue;
            for (uint32_t i = a; i < b; i++) {
                uint32_t id = s->grid_ids[i];
                if (id >= s->n_ent) continue;
                const tz_sentry *e = &s->ent[id];
                double d = tz_distance_m(lat, lon, e->lat / 1e6, e->lon / 1e6);
                /* Addresses and POIs describe a spot better than a street
                 * or a whole town, so give them a small advantage. */
                if (e->kind == TZ_KIND_STREET) d += 15.0;
                if (e->kind == TZ_KIND_PLACE) d += 50.0;
                if (d < best) { best = d; best_id = id; }
            }
        }
    }
    if (best_id == UINT32_MAX) return 0;
    const tz_sentry *e = &s->ent[best_id];
    memset(hit, 0, sizeof *hit);
    hit->id = best_id;
    hit->kind = e->kind;
    hit->sub = e->sub;
    hit->lat = e->lat / 1e6;
    hit->lon = e->lon / 1e6;
    hit->dist_m = tz_distance_m(lat, lon, hit->lat, hit->lon);
    hit->score = 0;
    hit->name = sstr(s, e->name);
    hit->context = sstr(s, e->ctx);
    return 1;
}
