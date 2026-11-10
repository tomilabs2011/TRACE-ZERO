#include "fz_pbf.h"
#include "fz_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

/* ----------------------------------------------------- protobuf decode */

typedef struct { const uint8_t *p, *end; } pb;

static uint64_t pb_varint(pb *b)
{
    uint64_t v = 0;
    int shift = 0;
    while (b->p < b->end) {
        uint8_t c = *b->p++;
        v |= (uint64_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) return v;
        shift += 7;
        if (shift > 63) break;
    }
    fz_die("corrupt PBF: bad varint");
    return 0;
}

static int64_t pb_zz(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

/* Next field; returns 0 at end of message. */
static int pb_next(pb *b, uint32_t *field, uint32_t *wt)
{
    if (b->p >= b->end) return 0;
    uint64_t k = pb_varint(b);
    *field = (uint32_t)(k >> 3);
    *wt = (uint32_t)(k & 7);
    return 1;
}

static pb pb_sub(pb *b)
{
    uint64_t n = pb_varint(b);
    if (n > (uint64_t)(b->end - b->p)) fz_die("corrupt PBF: length out of range");
    pb s = { b->p, b->p + n };
    b->p += n;
    return s;
}

static void pb_skip(pb *b, uint32_t wt)
{
    switch (wt) {
    case 0: pb_varint(b); break;
    case 1: if (b->end - b->p < 8) fz_die("corrupt PBF"); b->p += 8; break;
    case 2: pb_sub(b); break;
    case 5: if (b->end - b->p < 4) fz_die("corrupt PBF"); b->p += 4; break;
    default: fz_die("corrupt PBF: wire type %u", wt);
    }
}

/* Repeated numeric field, packed (wt 2) or not (wt 0), appended raw. */
typedef FZ_VEC(uint64_t) u64vec;

static void pb_repeated(pb *b, uint32_t wt, u64vec *out)
{
    if (wt == 0) { fz_vec_push(*out, pb_varint(b)); return; }
    if (wt != 2) fz_die("corrupt PBF: unexpected wire type %u", wt);
    pb s = pb_sub(b);
    while (s.p < s.end) fz_vec_push(*out, pb_varint(&s));
}

/* --------------------------------------------------------- block state */

typedef struct {
    fz_pbf_handler *h;
    char          **strs;      /* string table */
    size_t          nstr;
    char           *strbuf;
    size_t          strbuf_cap;
    size_t          strs_cap;
    int64_t         gran, lat_off, lon_off;
    FZ_VEC(fz_tag)    tags;
    FZ_VEC(int64_t)   refs;
    FZ_VEC(fz_member) mem;
    u64vec          v1, v2, v3, v4, v5;
} block;

static const char *bstr(block *bl, uint64_t i)
{
    if (i >= bl->nstr) fz_die("corrupt PBF: string index %llu out of range", (unsigned long long)i);
    return bl->strs[i];
}

static void parse_stringtable(block *bl, pb st)
{
    /* first pass: count and total size */
    size_t count = 0, total = 0;
    pb s = st;
    uint32_t f, wt;
    while (pb_next(&s, &f, &wt)) {
        if (f == 1 && wt == 2) { pb x = pb_sub(&s); count++; total += (size_t)(x.end - x.p) + 1; }
        else pb_skip(&s, wt);
    }
    if (count > bl->strs_cap) { bl->strs = (char **)fz_realloc(bl->strs, count * sizeof(char *)); bl->strs_cap = count; }
    if (total > bl->strbuf_cap) { bl->strbuf = (char *)fz_realloc(bl->strbuf, total); bl->strbuf_cap = total; }
    char *w = bl->strbuf;
    size_t i = 0;
    s = st;
    while (pb_next(&s, &f, &wt)) {
        if (f == 1 && wt == 2) {
            pb x = pb_sub(&s);
            size_t n = (size_t)(x.end - x.p);
            memcpy(w, x.p, n);
            w[n] = 0;
            bl->strs[i++] = w;
            w += n + 1;
        } else pb_skip(&s, wt);
    }
    bl->nstr = count;
}

static void build_tags(block *bl, const u64vec *keys, const u64vec *vals)
{
    bl->tags.n = 0;
    if (keys->n != vals->n) fz_die("corrupt PBF: tag key/value count mismatch");
    for (size_t i = 0; i < keys->n; i++) {
        fz_tag t = { bstr(bl, keys->a[i]), bstr(bl, vals->a[i]) };
        fz_vec_push(bl->tags, t);
    }
}

static void parse_dense(block *bl, pb d)
{
    fz_pbf_handler *h = bl->h;
    u64vec *ids = &bl->v1, *lats = &bl->v2, *lons = &bl->v3, *kv = &bl->v4;
    ids->n = lats->n = lons->n = kv->n = 0;
    uint32_t f, wt;
    while (pb_next(&d, &f, &wt)) {
        switch (f) {
        case 1: pb_repeated(&d, wt, ids); break;
        case 8: pb_repeated(&d, wt, lats); break;
        case 9: pb_repeated(&d, wt, lons); break;
        case 10: pb_repeated(&d, wt, kv); break;
        default: pb_skip(&d, wt);
        }
    }
    if (lats->n != ids->n || lons->n != ids->n) fz_die("corrupt PBF: dense node arrays differ in length");
    int64_t id = 0, lat = 0, lon = 0;
    size_t k = 0;
    for (size_t i = 0; i < ids->n; i++) {
        id += pb_zz(ids->a[i]);
        lat += pb_zz(lats->a[i]);
        lon += pb_zz(lons->a[i]);
        bl->tags.n = 0;
        if (kv->n) {
            while (k < kv->n && kv->a[k] != 0) {
                if (k + 1 >= kv->n) fz_die("corrupt PBF: dangling dense tag");
                fz_tag t = { bstr(bl, kv->a[k]), bstr(bl, kv->a[k + 1]) };
                fz_vec_push(bl->tags, t);
                k += 2;
            }
            k++; /* the 0 delimiter */
        }
        if (h->want_nodes == 1 && bl->tags.n == 0) continue;
        h->node(h->ctx, id, bl->lat_off + bl->gran * lat, bl->lon_off + bl->gran * lon,
                bl->tags.a, (int)bl->tags.n);
    }
}

static void parse_node(block *bl, pb n)
{
    fz_pbf_handler *h = bl->h;
    int64_t id = 0, lat = 0, lon = 0;
    u64vec *keys = &bl->v1, *vals = &bl->v2;
    keys->n = vals->n = 0;
    uint32_t f, wt;
    while (pb_next(&n, &f, &wt)) {
        switch (f) {
        case 1: id = pb_zz(pb_varint(&n)); break;
        case 2: pb_repeated(&n, wt, keys); break;
        case 3: pb_repeated(&n, wt, vals); break;
        case 8: lat = pb_zz(pb_varint(&n)); break;
        case 9: lon = pb_zz(pb_varint(&n)); break;
        default: pb_skip(&n, wt);
        }
    }
    build_tags(bl, keys, vals);
    if (h->want_nodes == 1 && bl->tags.n == 0) return;
    h->node(h->ctx, id, bl->lat_off + bl->gran * lat, bl->lon_off + bl->gran * lon,
            bl->tags.a, (int)bl->tags.n);
}

static void parse_way(block *bl, pb w)
{
    fz_pbf_handler *h = bl->h;
    int64_t id = 0;
    u64vec *keys = &bl->v1, *vals = &bl->v2, *refs = &bl->v3;
    keys->n = vals->n = refs->n = 0;
    uint32_t f, wt;
    while (pb_next(&w, &f, &wt)) {
        switch (f) {
        case 1: id = (int64_t)pb_varint(&w); break;
        case 2: pb_repeated(&w, wt, keys); break;
        case 3: pb_repeated(&w, wt, vals); break;
        case 8: pb_repeated(&w, wt, refs); break;
        default: pb_skip(&w, wt);
        }
    }
    build_tags(bl, keys, vals);
    bl->refs.n = 0;
    fz_vec_grow(bl->refs, refs->n);
    int64_t r = 0;
    for (size_t i = 0; i < refs->n; i++) { r += pb_zz(refs->a[i]); bl->refs.a[i] = r; }
    bl->refs.n = refs->n;
    h->way(h->ctx, id, bl->refs.a, (int)bl->refs.n, bl->tags.a, (int)bl->tags.n);
}

static void parse_relation(block *bl, pb r)
{
    fz_pbf_handler *h = bl->h;
    int64_t id = 0;
    u64vec *keys = &bl->v1, *vals = &bl->v2, *roles = &bl->v3, *mids = &bl->v4, *types = &bl->v5;
    keys->n = vals->n = roles->n = mids->n = types->n = 0;
    uint32_t f, wt;
    while (pb_next(&r, &f, &wt)) {
        switch (f) {
        case 1: id = (int64_t)pb_varint(&r); break;
        case 2: pb_repeated(&r, wt, keys); break;
        case 3: pb_repeated(&r, wt, vals); break;
        case 8: pb_repeated(&r, wt, roles); break;
        case 9: pb_repeated(&r, wt, mids); break;
        case 10: pb_repeated(&r, wt, types); break;
        default: pb_skip(&r, wt);
        }
    }
    if (roles->n != mids->n || types->n != mids->n) fz_die("corrupt PBF: relation member arrays differ");
    build_tags(bl, keys, vals);
    bl->mem.n = 0;
    int64_t m = 0;
    for (size_t i = 0; i < mids->n; i++) {
        m += pb_zz(mids->a[i]);
        fz_member mm = { m, (int)types->a[i], bstr(bl, (int64_t)(int32_t)roles->a[i] < 0 ? 0 : roles->a[i]) };
        fz_vec_push(bl->mem, mm);
    }
    h->relation(h->ctx, id, bl->mem.a, (int)bl->mem.n, bl->tags.a, (int)bl->tags.n);
}

static void parse_primitive_block(block *bl, pb b)
{
    fz_pbf_handler *h = bl->h;
    bl->gran = 100;
    bl->lat_off = bl->lon_off = 0;
    bl->nstr = 0;
    uint32_t f, wt;
    pb s = b;
    int have_st = 0;
    while (pb_next(&s, &f, &wt)) {
        if (f == 1 && wt == 2) { parse_stringtable(bl, pb_sub(&s)); have_st = 1; }
        else if (f == 17 && wt == 0) bl->gran = (int64_t)pb_varint(&s);
        else if (f == 19 && wt == 0) bl->lat_off = (int64_t)pb_varint(&s);
        else if (f == 20 && wt == 0) bl->lon_off = (int64_t)pb_varint(&s);
        else pb_skip(&s, wt);
    }
    if (!have_st) fz_die("corrupt PBF: block without string table");
    s = b;
    while (pb_next(&s, &f, &wt)) {
        if (f != 2 || wt != 2) { pb_skip(&s, wt); continue; }
        pb g = pb_sub(&s);
        uint32_t gf, gwt;
        while (pb_next(&g, &gf, &gwt)) {
            if (gwt != 2) { pb_skip(&g, gwt); continue; }
            pb e = pb_sub(&g);
            switch (gf) {
            case 1: if (h->want_nodes) parse_node(bl, e); break;
            case 2: if (h->want_nodes) parse_dense(bl, e); break;
            case 3: if (h->want_ways) parse_way(bl, e); break;
            case 4: if (h->want_relations) parse_relation(bl, e); break;
            default: break;
            }
        }
    }
}

static void parse_header(pb b, fz_pbf_info *info)
{
    uint32_t f, wt;
    while (pb_next(&b, &f, &wt)) {
        if (f == 1 && wt == 2) {
            pb bb = pb_sub(&b);
            int64_t v[5] = {0};
            uint32_t bf, bwt;
            while (pb_next(&bb, &bf, &bwt)) {
                if (bf >= 1 && bf <= 4 && bwt == 0) v[bf] = pb_zz(pb_varint(&bb));
                else pb_skip(&bb, bwt);
            }
            info->has_bbox = 1;
            info->minlon = v[1] / 1e9; info->maxlon = v[2] / 1e9;
            info->maxlat = v[3] / 1e9; info->minlat = v[4] / 1e9;
        } else if (f == 4 && wt == 2) {
            pb x = pb_sub(&b);
            size_t n = (size_t)(x.end - x.p);
            char feat[128];
            if (n >= sizeof feat) n = sizeof feat - 1;
            memcpy(feat, x.p, n);
            feat[n] = 0;
            if (strcmp(feat, "OsmSchema-V0.6") && strcmp(feat, "DenseNodes") &&
                strcmp(feat, "HistoricalInformation"))
                fz_die("PBF needs unsupported feature \"%s\"", feat);
        } else if (f == 17 && wt == 2) {
            pb x = pb_sub(&b);
            size_t n = (size_t)(x.end - x.p);
            if (n >= sizeof info->source) n = sizeof info->source - 1;
            memcpy(info->source, x.p, n);
            info->source[n] = 0;
        } else if (f == 32 && wt == 0) {
            info->replication_timestamp = (int64_t)pb_varint(&b);
        } else {
            pb_skip(&b, wt);
        }
    }
}

/* ---------------------------------------------------------------- file */

static int read_exact(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n;
}

void fz_pbf_read(const char *path, fz_pbf_handler *h, fz_pbf_info *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) fz_die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    double fsize = (fstat(fileno(f), &st) == 0 && st.st_size > 0) ? (double)st.st_size : 1.0;
    fz_pbf_info dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof *info);

    block bl;
    memset(&bl, 0, sizeof bl);
    bl.h = h;
    uint8_t *hdrbuf = (uint8_t *)fz_malloc(65536);
    uint8_t *blobbuf = NULL, *rawbuf = NULL;
    size_t blobcap = 0, rawcap = 0;
    double done = 0;
    int saw_header = 0;

    for (;;) {
        uint8_t lenb[4];
        size_t got = fread(lenb, 1, 4, f);
        if (got == 0) break;
        if (got != 4) fz_die("truncated PBF file");
        uint32_t hlen = ((uint32_t)lenb[0] << 24) | ((uint32_t)lenb[1] << 16) | ((uint32_t)lenb[2] << 8) | lenb[3];
        if (hlen == 0 || hlen > 65536) fz_die("corrupt PBF: blob header size %u", hlen);
        if (!read_exact(f, hdrbuf, hlen)) fz_die("truncated PBF file");
        pb bh = { hdrbuf, hdrbuf + hlen };
        char type[32] = {0};
        int64_t datasize = -1;
        uint32_t fld, wt;
        while (pb_next(&bh, &fld, &wt)) {
            if (fld == 1 && wt == 2) {
                pb x = pb_sub(&bh);
                size_t n = (size_t)(x.end - x.p);
                if (n >= sizeof type) n = sizeof type - 1;
                memcpy(type, x.p, n);
                type[n] = 0;
            } else if (fld == 3 && wt == 0) datasize = (int64_t)pb_varint(&bh);
            else pb_skip(&bh, wt);
        }
        if (datasize < 0 || datasize > 64 * 1024 * 1024) fz_die("corrupt PBF: blob size %lld", (long long)datasize);
        if ((size_t)datasize > blobcap) { blobcap = (size_t)datasize; blobbuf = (uint8_t *)fz_realloc(blobbuf, blobcap); }
        if (!read_exact(f, blobbuf, (size_t)datasize)) fz_die("truncated PBF file");
        done += 4 + hlen + (double)datasize;

        int is_header = strcmp(type, "OSMHeader") == 0;
        int is_data = strcmp(type, "OSMData") == 0;
        if (!is_header && !is_data) continue; /* unknown blob types must be skipped */

        pb blob = { blobbuf, blobbuf + datasize };
        const uint8_t *raw = NULL, *zdata = NULL;
        size_t rawlen = 0, zlen = 0;
        int64_t raw_size = -1;
        while (pb_next(&blob, &fld, &wt)) {
            if (fld == 1 && wt == 2) { pb x = pb_sub(&blob); raw = x.p; rawlen = (size_t)(x.end - x.p); }
            else if (fld == 2 && wt == 0) raw_size = (int64_t)pb_varint(&blob);
            else if (fld == 3 && wt == 2) { pb x = pb_sub(&blob); zdata = x.p; zlen = (size_t)(x.end - x.p); }
            else if (fld >= 4 && fld <= 7) fz_die("PBF uses an unsupported compression (field %u); re-export with zlib", fld);
            else pb_skip(&blob, wt);
        }
        const uint8_t *data;
        size_t dlen;
        if (raw) { data = raw; dlen = rawlen; }
        else if (zdata) {
            if (raw_size < 0 || raw_size > 64 * 1024 * 1024) fz_die("corrupt PBF: raw size");
            if ((size_t)raw_size > rawcap) { rawcap = (size_t)raw_size; rawbuf = (uint8_t *)fz_realloc(rawbuf, rawcap ? rawcap : 1); }
            uLongf out = (uLongf)raw_size;
            int zr = uncompress(rawbuf, &out, zdata, (uLong)zlen);
            if (zr != Z_OK || out != (uLongf)raw_size) fz_die("corrupt PBF: zlib error %d", zr);
            data = rawbuf;
            dlen = (size_t)out;
        } else {
            fz_die("corrupt PBF: empty blob");
            return;
        }
        pb msg = { data, data + dlen };
        if (is_header) { parse_header(msg, info); saw_header = 1; }
        else {
            if (!saw_header) fz_die("corrupt PBF: data before header");
            parse_primitive_block(&bl, msg);
        }
        fz_progress(done / fsize);
    }
    if (!saw_header) fz_die("%s is not an OSM PBF file", path);
    fclose(f);
    free(hdrbuf);
    free(blobbuf);
    free(rawbuf);
    free(bl.strs);
    free(bl.strbuf);
    fz_vec_free(bl.tags);
    fz_vec_free(bl.refs);
    fz_vec_free(bl.mem);
    fz_vec_free(bl.v1);
    fz_vec_free(bl.v2);
    fz_vec_free(bl.v3);
    fz_vec_free(bl.v4);
    fz_vec_free(bl.v5);
}
