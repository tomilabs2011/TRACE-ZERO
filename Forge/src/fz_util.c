#include "fz_util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int fz_progress_mode = 0;

static double g_last_progress_t = 0;
static int g_last_pct = -1;
static int g_stage_index = 0, g_stage_total = 1;

double fz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void fz_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void fz_stage(int index, int total, const char *name)
{
    g_stage_index = index;
    g_stage_total = total > 0 ? total : 1;
    g_last_pct = -1;
    fz_log("[%d/%d] %s", index, total, name);
    if (fz_progress_mode) {
        printf("STAGE %d %d %s\n", index, total, name);
        fflush(stdout);
    }
    fz_progress(0.0);
}

void fz_progress(double fraction)
{
    if (!fz_progress_mode) return;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    /* overall percentage across stages */
    double overall = ((g_stage_index - 1) + fraction) / g_stage_total;
    int pct = (int)(overall * 1000.0);
    double t = fz_now();
    if (pct == g_last_pct || (t - g_last_progress_t < 0.25 && fraction < 1.0)) return;
    g_last_pct = pct;
    g_last_progress_t = t;
    printf("PROGRESS %.1f\n", pct / 10.0);
    fflush(stdout);
}

void fz_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (fz_progress_mode) {
        va_start(ap, fmt);
        printf("ERROR ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
        fflush(stdout);
    }
    exit(1);
}

void *fz_malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) fz_die("out of memory (%zu bytes)", n);
    return p;
}

void *fz_calloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) fz_die("out of memory (%zu x %zu bytes)", n, sz);
    return p;
}

void *fz_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) fz_die("out of memory (%zu bytes)", n);
    return q;
}

char *fz_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)fz_malloc(n);
    memcpy(d, s, n);
    return d;
}

/* ------------------------------------------------------------- map64 */

static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

#define MAP_EMPTY INT64_MIN

void fz_map64_init(fz_map64 *m, size_t expected)
{
    size_t cap = 1024;
    while (cap < expected * 2) cap <<= 1;
    m->keys = (int64_t *)fz_malloc(cap * sizeof(int64_t));
    m->vals = (uint32_t *)fz_malloc(cap * sizeof(uint32_t));
    for (size_t i = 0; i < cap; i++) m->keys[i] = MAP_EMPTY;
    m->cap = cap;
    m->n = 0;
}

static void map64_grow(fz_map64 *m)
{
    fz_map64 b;
    fz_map64_init(&b, m->cap);
    for (size_t i = 0; i < m->cap; i++)
        if (m->keys[i] != MAP_EMPTY) fz_map64_put(&b, m->keys[i], m->vals[i]);
    fz_map64_free(m);
    *m = b;
}

void fz_map64_put(fz_map64 *m, int64_t key, uint32_t val)
{
    if ((m->n + 1) * 10 > m->cap * 7) map64_grow(m);
    size_t mask = m->cap - 1, i = (size_t)mix64((uint64_t)key) & mask;
    while (m->keys[i] != MAP_EMPTY && m->keys[i] != key) i = (i + 1) & mask;
    if (m->keys[i] == MAP_EMPTY) { m->keys[i] = key; m->n++; }
    m->vals[i] = val;
}

uint32_t fz_map64_get(const fz_map64 *m, int64_t key)
{
    if (!m->cap) return UINT32_MAX;
    size_t mask = m->cap - 1, i = (size_t)mix64((uint64_t)key) & mask;
    while (m->keys[i] != MAP_EMPTY) {
        if (m->keys[i] == key) return m->vals[i];
        i = (i + 1) & mask;
    }
    return UINT32_MAX;
}

void fz_map64_free(fz_map64 *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* -------------------------------------------------------------- pool */

static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

void fz_pool_init(fz_pool *p)
{
    memset(p, 0, sizeof *p);
    p->cap = 1 << 16;
    p->buf = (char *)fz_malloc(p->cap);
    p->buf[0] = 0;
    p->len = 1;
    p->nslots = 1 << 12;
    p->slots = (uint32_t *)fz_calloc(p->nslots, sizeof(uint32_t));
}

static void pool_rehash(fz_pool *p)
{
    size_t ns = p->nslots * 2;
    uint32_t *sl = (uint32_t *)fz_calloc(ns, sizeof(uint32_t));
    for (size_t i = 0; i < p->nslots; i++) {
        uint32_t off = p->slots[i];
        if (!off) continue;
        size_t j = str_hash(p->buf + off) & (ns - 1);
        while (sl[j]) j = (j + 1) & (ns - 1);
        sl[j] = off;
    }
    free(p->slots);
    p->slots = sl;
    p->nslots = ns;
}

uint32_t fz_pool_add(fz_pool *p, const char *s)
{
    if (!s || !*s) return 0;
    if ((p->used + 1) * 10 > p->nslots * 7) pool_rehash(p);
    size_t mask = p->nslots - 1, i = str_hash(s) & mask;
    while (p->slots[i]) {
        if (strcmp(p->buf + p->slots[i], s) == 0) return p->slots[i];
        i = (i + 1) & mask;
    }
    size_t n = strlen(s) + 1;
    if (p->len + n > 0xFFFFFFF0u) fz_die("string pool exceeds 4 GB");
    while (p->len + n > p->cap) {
        p->cap *= 2;
        p->buf = (char *)fz_realloc(p->buf, p->cap);
    }
    uint32_t off = (uint32_t)p->len;
    memcpy(p->buf + off, s, n);
    p->len += n;
    p->slots[i] = off;
    p->used++;
    return off;
}

const char *fz_pool_get(const fz_pool *p, uint32_t off)
{
    return off < p->len ? p->buf + off : "";
}

void fz_pool_free(fz_pool *p)
{
    free(p->buf);
    free(p->slots);
    memset(p, 0, sizeof *p);
}

/* ------------------------------------------------------------ sorting */

void fz_sort_idx_by_u64(uint32_t *idx, size_t n, const uint64_t *key)
{
    uint32_t *tmp = (uint32_t *)fz_malloc(n * sizeof(uint32_t));
    size_t count[256];
    for (int pass = 0; pass < 8; pass++) {
        int shift = pass * 8;
        /* skip passes where every key has the same byte */
        memset(count, 0, sizeof count);
        for (size_t i = 0; i < n; i++) count[(key[idx[i]] >> shift) & 0xFF]++;
        int trivial = 0;
        for (int b = 0; b < 256; b++) if (count[b] == n) trivial = 1;
        if (trivial) continue;
        size_t sum = 0;
        for (int b = 0; b < 256; b++) { size_t c = count[b]; count[b] = sum; sum += c; }
        for (size_t i = 0; i < n; i++) tmp[count[(key[idx[i]] >> shift) & 0xFF]++] = idx[i];
        memcpy(idx, tmp, n * sizeof(uint32_t));
    }
    free(tmp);
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

size_t fz_sort_unique_i64(int64_t *a, size_t n)
{
    if (!n) return 0;
    qsort(a, n, sizeof(int64_t), cmp_i64);
    size_t w = 1;
    for (size_t i = 1; i < n; i++) if (a[i] != a[w - 1]) a[w++] = a[i];
    return w;
}

size_t fz_bsearch_i64(const int64_t *a, size_t n, int64_t x)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < x) lo = mid + 1; else hi = mid;
    }
    return (lo < n && a[lo] == x) ? lo : SIZE_MAX;
}

uint64_t fz_morton(int32_t lat_e6, int32_t lon_e6)
{
    uint32_t y = (uint32_t)((int64_t)lat_e6 + 90000000) >> 4;
    uint32_t x = (uint32_t)((int64_t)lon_e6 + 180000000) >> 4;
    uint64_t r = 0;
    for (int i = 0; i < 32; i++) {
        r |= (uint64_t)((x >> i) & 1) << (2 * i);
        r |= (uint64_t)((y >> i) & 1) << (2 * i + 1);
    }
    return r;
}

/* ------------------------------------------------------------- writer */

void fz_writer_open(fz_writer *w, const char *path, const char *magic)
{
    memset(w, 0, sizeof *w);
    w->path = fz_strdup(path);
    size_t n = strlen(path) + 8;
    w->tmp = (char *)fz_malloc(n);
    snprintf(w->tmp, n, "%s.part", path);
    w->f = fopen(w->tmp, "wb");
    if (!w->f) fz_die("cannot create %s: %s", w->tmp, strerror(errno));
    memset(&w->h, 0, sizeof w->h);
    strncpy(w->h.magic, magic, sizeof w->h.magic);
    w->h.version = TZ_FORMAT_VERSION;
    w->h.header_size = sizeof(tz_file_header);
    if (fwrite(&w->h, sizeof w->h, 1, w->f) != 1) fz_die("write failed: %s", w->tmp);
    w->pos = sizeof w->h;
    w->cur = -1;
}

void fz_writer_begin(fz_writer *w, int section)
{
    static const uint8_t zeros[8] = {0};
    if (w->cur >= 0) fz_die("internal: nested section");
    size_t pad = (size_t)((8 - (w->pos & 7)) & 7);
    if (pad && fwrite(zeros, 1, pad, w->f) != pad) fz_die("write failed: %s", w->tmp);
    w->pos += pad;
    w->cur = section;
    w->h.sec[section].off = w->pos;
    w->h.sec[section].size = 0;
}

void fz_writer_write(fz_writer *w, const void *data, size_t n)
{
    if (!n) return;
    if (fwrite(data, 1, n, w->f) != n) fz_die("write failed: %s (%s)", w->tmp, strerror(errno));
    w->pos += n;
    if (w->cur >= 0) w->h.sec[w->cur].size += n;
}

void fz_writer_end(fz_writer *w)
{
    if (w->h.sec[w->cur].size == 0) w->h.sec[w->cur].off = 0;
    w->cur = -1;
}

void fz_writer_close(fz_writer *w, tz_file_header *out)
{
    static const uint8_t zeros[8] = {0};
    size_t pad = (size_t)((8 - (w->pos & 7)) & 7);
    if (pad && fwrite(zeros, 1, pad, w->f) != pad) fz_die("write failed: %s", w->tmp);
    w->pos += pad;
    w->h.file_size = w->pos;
    if (fseek(w->f, 0, SEEK_SET) != 0) fz_die("seek failed: %s", w->tmp);
    if (fwrite(&w->h, sizeof w->h, 1, w->f) != 1) fz_die("write failed: %s", w->tmp);
    if (fflush(w->f) != 0) fz_die("flush failed: %s (%s)", w->tmp, strerror(errno));
    fsync(fileno(w->f));
    if (fclose(w->f) != 0) fz_die("close failed: %s", w->tmp);
    if (rename(w->tmp, w->path) != 0) fz_die("cannot rename %s: %s", w->tmp, strerror(errno));
    if (out) *out = w->h;
    free(w->path);
    free(w->tmp);
    memset(w, 0, sizeof *w);
}

void fz_put_varint(fz_bytes *b, uint32_t v)
{
    fz_vec_grow(*b, b->n + 5);
    while (v >= 0x80) { b->a[b->n++] = (uint8_t)(v | 0x80); v >>= 7; }
    b->a[b->n++] = (uint8_t)v;
}
