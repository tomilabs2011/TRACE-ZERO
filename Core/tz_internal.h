/* Internal helpers shared by the core library files. Not part of the API. */
#ifndef TZ_INTERNAL_H
#define TZ_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "tz_format.h"
#include "tz_core.h"

#define TZ_PI 3.14159265358979323846
#define TZ_EARTH_R 6371008.8
#define TZ_DEG2RAD (TZ_PI / 180.0)

/* ---------------------------------------------------------- mapped file */

typedef struct {
    const uint8_t        *base;
    size_t                size;
    const tz_file_header *hdr;
} tz_mapped;

/* Map a file read-only and validate the header, magic, version, size and
 * section bounds. Returns TZ_OK or an error status. */
int  tz_map_file(const char *path, const char *magic, tz_mapped *out);
void tz_unmap_file(tz_mapped *m);
/* Pointer to a section, or NULL when it is empty. */
const void *tz_section_ptr(const tz_mapped *m, int idx);
uint64_t    tz_section_size(const tz_mapped *m, int idx);
uint32_t    tz_header_checksum(const tz_file_header *h);

/* --------------------------------------------------------------- varint */

static inline uint32_t tz_zigzag_enc(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
static inline int32_t  tz_zigzag_dec(uint32_t v) { return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }

/* Decode an unsigned LEB128 varint (max 5 bytes). Never reads past end. */
static inline const uint8_t *tz_varint_get(const uint8_t *p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    int shift = 0;
    while (p < end && shift <= 28) {
        uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = v; return p; }
        shift += 7;
    }
    *out = v;
    return end; /* malformed: stop decoding */
}

/* ----------------------------------------------------------------- geo */

/* Local equirectangular projection around a reference latitude. Accurate to
 * well under 1% over a few kilometres, which is all snapping needs. */
typedef struct { double kx, ky; double lat0, lon0; } tz_proj;
void tz_proj_init(tz_proj *p, double lat0, double lon0);
static inline void tz_proj_xy(const tz_proj *p, double lat, double lon, double *x, double *y)
{
    double dlon = lon - p->lon0;
    if (dlon > 180.0) dlon -= 360.0; else if (dlon < -180.0) dlon += 360.0;
    *x = dlon * p->kx;
    *y = (lat - p->lat0) * p->ky;
}

/* Distance from point P to segment AB in projected metres; t = position on
 * AB (0..1) of the closest point. */
double tz_point_seg_dist(double px, double py, double ax, double ay,
                         double bx, double by, double *t);

double tz_norm_angle(double deg);   /* -> (-180, 180] */

/* ------------------------------------------------------------- min heap */

typedef struct { uint64_t key; uint32_t val; uint32_t _pad; } tz_heap_item;
typedef struct { tz_heap_item *a; size_t n, cap; } tz_heap;

int  tz_heap_push(tz_heap *h, uint64_t key, uint32_t val);
int  tz_heap_pop(tz_heap *h, uint64_t *key, uint32_t *val);
static inline uint64_t tz_heap_min(const tz_heap *h) { return h->n ? h->a[0].key : UINT64_MAX; }
void tz_heap_free(tz_heap *h);

/* ---------------------------------------- open-addressing node hash map */

typedef struct {
    uint32_t node;      /* key, UINT32_MAX = empty */
    uint32_t dist;      /* tentative distance */
    uint32_t parent;    /* parent node, UINT32_MAX = search root */
    uint32_t edge;      /* index of the CH edge used to reach node */
    uint8_t  settled;
    uint8_t  _pad[3];
} tz_nodeent;

typedef struct { tz_nodeent *t; uint32_t cap, n; } tz_nodemap;

int         tz_nodemap_init(tz_nodemap *m, uint32_t cap_pow2);
tz_nodeent *tz_nodemap_get(const tz_nodemap *m, uint32_t node);
/* Returns the slot (inserting a fresh one with dist = UINT32_MAX if absent),
 * or NULL on allocation failure. Pointers are invalidated by later inserts. */
tz_nodeent *tz_nodemap_put(tz_nodemap *m, uint32_t node);
void        tz_nodemap_free(tz_nodemap *m);

/* ----------------------------------------------------- growable buffers */

typedef struct { void *p; size_t n, cap, elem; } tz_vec;
void  tz_vec_init(tz_vec *v, size_t elem);
void *tz_vec_push(tz_vec *v);            /* returns slot or NULL */
int   tz_vec_reserve(tz_vec *v, size_t n);
void  tz_vec_free(tz_vec *v);

/* --------------------------------------------------------------- utf-8 */

/* Decode one code point; advances *s. Invalid bytes become U+FFFD. */
uint32_t tz_utf8_next(const unsigned char **s, const unsigned char *end);

#endif
