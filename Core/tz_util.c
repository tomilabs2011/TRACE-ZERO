/* File mapping, geometry helpers and small containers for the core. */
#include "tz_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef char tz_header_is_512_bytes[(sizeof(tz_file_header) == 512) ? 1 : -1];

const char *tz_status_message(int status)
{
    switch (status) {
    case TZ_OK:           return "OK";
    case TZ_ERR_IO:       return "File is missing or unreadable";
    case TZ_ERR_FORMAT:   return "File is damaged or incomplete";
    case TZ_ERR_VERSION:  return "Map pack was built for a different app version";
    case TZ_ERR_MISMATCH: return "Map pack files do not belong together";
    case TZ_ERR_NOMEM:    return "Out of memory";
    case TZ_ERR_NO_START: return "No road near the start";
    case TZ_ERR_NO_END:   return "No road near the destination";
    case TZ_ERR_NO_ROUTE: return "No route between these points";
    case TZ_ERR_ARGUMENT: return "Invalid request";
    default:              return "Unknown error";
    }
}

/* ---------------------------------------------------------- mapped file */

int tz_map_file(const char *path, const char *magic, tz_mapped *out)
{
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return TZ_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return TZ_ERR_IO; }
    if (st.st_size < (off_t)sizeof(tz_file_header)) { close(fd); return TZ_ERR_FORMAT; }
    size_t size = (size_t)st.st_size;
    void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd); /* the mapping keeps the file alive */
    if (p == MAP_FAILED) return (errno == ENOMEM) ? TZ_ERR_NOMEM : TZ_ERR_IO;

    const tz_file_header *h = (const tz_file_header *)p;
    int status = TZ_OK;
    if (strncmp(h->magic, magic, 8) != 0) status = TZ_ERR_FORMAT;
    else if (h->version != TZ_FORMAT_VERSION) status = TZ_ERR_VERSION;
    else if (h->header_size != sizeof(tz_file_header)) status = TZ_ERR_FORMAT;
    else if (h->file_size != (uint64_t)size) status = TZ_ERR_FORMAT; /* truncated copy */
    else {
        for (int i = 0; i < TZ_MAX_SECTIONS; i++) {
            const tz_section *s = &h->sec[i];
            if (s->size == 0) continue;
            if ((s->off & 7) || s->off < sizeof(tz_file_header) ||
                s->off > size || s->size > size - s->off) {
                status = TZ_ERR_FORMAT;
                break;
            }
        }
    }
    if (status != TZ_OK) { munmap(p, size); return status; }

    out->base = (const uint8_t *)p;
    out->size = size;
    out->hdr = h;
    return TZ_OK;
}

void tz_unmap_file(tz_mapped *m)
{
    if (m->base) munmap((void *)m->base, m->size);
    memset(m, 0, sizeof *m);
}

const void *tz_section_ptr(const tz_mapped *m, int idx)
{
    const tz_section *s = &m->hdr->sec[idx];
    return s->size ? (const void *)(m->base + s->off) : NULL;
}

uint64_t tz_section_size(const tz_mapped *m, int idx)
{
    return m->hdr->sec[idx].size;
}

uint32_t tz_header_checksum(const tz_file_header *h)
{
    /* FNV-1a over the whole header. */
    const uint8_t *p = (const uint8_t *)h;
    uint32_t x = 2166136261u;
    for (size_t i = 0; i < sizeof *h; i++) { x ^= p[i]; x *= 16777619u; }
    return x;
}

/* ------------------------------------------------------------------ geo */

double tz_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * TZ_DEG2RAD, p2 = lat2 * TZ_DEG2RAD;
    double dp = p2 - p1, dl = (lon2 - lon1) * TZ_DEG2RAD;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (a > 1.0) a = 1.0;
    return 2.0 * TZ_EARTH_R * asin(sqrt(a));
}

double tz_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * TZ_DEG2RAD, p2 = lat2 * TZ_DEG2RAD;
    double dl = (lon2 - lon1) * TZ_DEG2RAD;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) / TZ_DEG2RAD;
    return fmod(b + 360.0, 360.0);
}

double tz_norm_angle(double deg)
{
    deg = fmod(deg, 360.0);
    if (deg <= -180.0) deg += 360.0;
    else if (deg > 180.0) deg -= 360.0;
    return deg;
}

void tz_proj_init(tz_proj *p, double lat0, double lon0)
{
    p->lat0 = lat0;
    p->lon0 = lon0;
    p->ky = TZ_EARTH_R * TZ_DEG2RAD;
    p->kx = p->ky * cos(lat0 * TZ_DEG2RAD);
}

double tz_point_seg_dist(double px, double py, double ax, double ay,
                         double bx, double by, double *t)
{
    double dx = bx - ax, dy = by - ay;
    double l2 = dx * dx + dy * dy;
    double u = 0.0;
    if (l2 > 0.0) {
        u = ((px - ax) * dx + (py - ay) * dy) / l2;
        if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
    }
    if (t) *t = u;
    double cx = ax + u * dx - px, cy = ay + u * dy - py;
    return sqrt(cx * cx + cy * cy);
}

/* ----------------------------------------------------------------- heap */

int tz_heap_push(tz_heap *h, uint64_t key, uint32_t val)
{
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 256;
        tz_heap_item *na = (tz_heap_item *)realloc(h->a, nc * sizeof *na);
        if (!na) return 0;
        h->a = na;
        h->cap = nc;
    }
    size_t i = h->n++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (h->a[parent].key <= key) break;
        h->a[i] = h->a[parent];
        i = parent;
    }
    h->a[i].key = key;
    h->a[i].val = val;
    return 1;
}

int tz_heap_pop(tz_heap *h, uint64_t *key, uint32_t *val)
{
    if (!h->n) return 0;
    *key = h->a[0].key;
    *val = h->a[0].val;
    tz_heap_item last = h->a[--h->n];
    size_t i = 0, n = h->n;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && h->a[c + 1].key < h->a[c].key) c++;
        if (last.key <= h->a[c].key) break;
        h->a[i] = h->a[c];
        i = c;
    }
    if (n) h->a[i] = last;
    return 1;
}

void tz_heap_free(tz_heap *h)
{
    free(h->a);
    memset(h, 0, sizeof *h);
}

/* -------------------------------------------------------------- nodemap */

static inline uint32_t tz_hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

int tz_nodemap_init(tz_nodemap *m, uint32_t cap_pow2)
{
    uint32_t cap = 64;
    while (cap < cap_pow2) cap <<= 1;
    m->t = (tz_nodeent *)malloc((size_t)cap * sizeof *m->t);
    if (!m->t) { m->cap = m->n = 0; return 0; }
    for (uint32_t i = 0; i < cap; i++) m->t[i].node = UINT32_MAX;
    m->cap = cap;
    m->n = 0;
    return 1;
}

tz_nodeent *tz_nodemap_get(const tz_nodemap *m, uint32_t node)
{
    uint32_t mask = m->cap - 1, i = tz_hash32(node) & mask;
    for (;;) {
        tz_nodeent *e = &m->t[i];
        if (e->node == node) return e;
        if (e->node == UINT32_MAX) return NULL;
        i = (i + 1) & mask;
    }
}

static int tz_nodemap_grow(tz_nodemap *m)
{
    tz_nodemap bigger;
    if (!tz_nodemap_init(&bigger, m->cap * 2)) return 0;
    uint32_t mask = bigger.cap - 1;
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->t[i].node == UINT32_MAX) continue;
        uint32_t j = tz_hash32(m->t[i].node) & mask;
        while (bigger.t[j].node != UINT32_MAX) j = (j + 1) & mask;
        bigger.t[j] = m->t[i];
    }
    bigger.n = m->n;
    free(m->t);
    *m = bigger;
    return 1;
}

tz_nodeent *tz_nodemap_put(tz_nodemap *m, uint32_t node)
{
    if ((m->n + 1) * 10 >= m->cap * 7 && !tz_nodemap_grow(m)) return NULL;
    uint32_t mask = m->cap - 1, i = tz_hash32(node) & mask;
    for (;;) {
        tz_nodeent *e = &m->t[i];
        if (e->node == node) return e;
        if (e->node == UINT32_MAX) {
            e->node = node;
            e->dist = UINT32_MAX;
            e->parent = UINT32_MAX;
            e->edge = UINT32_MAX;
            e->settled = 0;
            m->n++;
            return e;
        }
        i = (i + 1) & mask;
    }
}

void tz_nodemap_free(tz_nodemap *m)
{
    free(m->t);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ vec */

void tz_vec_init(tz_vec *v, size_t elem)
{
    v->p = NULL;
    v->n = v->cap = 0;
    v->elem = elem;
}

int tz_vec_reserve(tz_vec *v, size_t n)
{
    if (n <= v->cap) return 1;
    size_t nc = v->cap ? v->cap : 16;
    while (nc < n) nc *= 2;
    void *np = realloc(v->p, nc * v->elem);
    if (!np) return 0;
    v->p = np;
    v->cap = nc;
    return 1;
}

void *tz_vec_push(tz_vec *v)
{
    if (!tz_vec_reserve(v, v->n + 1)) return NULL;
    return (char *)v->p + (v->n++) * v->elem;
}

void tz_vec_free(tz_vec *v)
{
    free(v->p);
    v->p = NULL;
    v->n = v->cap = 0;
}

/* ---------------------------------------------------------------- utf-8 */

uint32_t tz_utf8_next(const unsigned char **sp, const unsigned char *end)
{
    const unsigned char *s = *sp;
    uint32_t c = *s++;
    if (c < 0x80) { *sp = s; return c; }
    int extra;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) { extra = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; c &= 0x07; min = 0x10000; }
    else { *sp = s; return 0xFFFD; }
    for (int i = 0; i < extra; i++) {
        if (s >= end || (*s & 0xC0) != 0x80) { *sp = s; return 0xFFFD; }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *sp = s;
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0xFFFD;
    return c;
}
