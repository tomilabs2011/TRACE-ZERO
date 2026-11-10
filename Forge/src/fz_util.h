/* Builder utilities: logging, growable arrays, hash maps, string pools. */
#ifndef FZ_UTIL_H
#define FZ_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ----------------------------------------------------------- logging */

extern int fz_progress_mode;   /* 1: machine-readable PROGRESS lines on stdout */

void fz_log(const char *fmt, ...);
void fz_stage(int index, int total, const char *name);
void fz_progress(double fraction);         /* rate-limited */
void fz_die(const char *fmt, ...);         /* print and exit(1) */
double fz_now(void);                       /* seconds, monotonic */

void *fz_malloc(size_t n);                 /* never returns NULL */
void *fz_calloc(size_t n, size_t sz);
void *fz_realloc(void *p, size_t n);
char *fz_strdup(const char *s);

/* --------------------------------------------------- growable arrays */

#define FZ_VEC(T) struct { T *a; size_t n, cap; }

#define fz_vec_grow(v, need) do { \
        if ((need) > (v).cap) { \
            size_t nc_ = (v).cap ? (v).cap : 16; \
            while (nc_ < (need)) nc_ += nc_ / 2 + 16; \
            (v).a = fz_realloc((v).a, nc_ * sizeof *(v).a); \
            (v).cap = nc_; \
        } \
    } while (0)

#define fz_vec_push(v, x) do { fz_vec_grow(v, (v).n + 1); (v).a[(v).n++] = (x); } while (0)
#define fz_vec_free(v) do { free((v).a); (v).a = NULL; (v).n = (v).cap = 0; } while (0)

/* ------------------------------------------- int64 -> uint32 hash map */

typedef struct {
    int64_t  *keys;
    uint32_t *vals;
    size_t    cap, n;
} fz_map64;

void     fz_map64_init(fz_map64 *m, size_t expected);
void     fz_map64_put(fz_map64 *m, int64_t key, uint32_t val);   /* overwrite */
uint32_t fz_map64_get(const fz_map64 *m, int64_t key);           /* UINT32_MAX if absent */
void     fz_map64_free(fz_map64 *m);

/* ------------------------------------------------ deduplicating pool */

/* A blob of NUL-terminated strings; equal strings share one offset.
 * Offset 0 is always the empty string. */
typedef struct {
    char     *buf;
    size_t    len, cap;
    uint32_t *slots;     /* hash table of offsets, 0 = empty */
    size_t    nslots, used;
} fz_pool;

void     fz_pool_init(fz_pool *p);
uint32_t fz_pool_add(fz_pool *p, const char *s);   /* returns offset */
const char *fz_pool_get(const fz_pool *p, uint32_t off);
void     fz_pool_free(fz_pool *p);

/* ------------------------------------------------------------ sorting */

/* Sort uint32 indices by a 64-bit key array (stable radix sort). */
void fz_sort_idx_by_u64(uint32_t *idx, size_t n, const uint64_t *key);
/* Sort and deduplicate int64 values in place; returns new length. */
size_t fz_sort_unique_i64(int64_t *a, size_t n);
/* Index of x in sorted array a, or SIZE_MAX. */
size_t fz_bsearch_i64(const int64_t *a, size_t n, int64_t x);

uint64_t fz_morton(int32_t lat_e6, int32_t lon_e6);

/* ------------------------------------------------------------- output */

#include "../../Core/tz_format.h"

typedef struct {
    FILE          *f;
    char          *path, *tmp;
    tz_file_header h;
    uint64_t       pos;
    int            cur;      /* section being written, -1 = none */
} fz_writer;

void fz_writer_open(fz_writer *w, const char *path, const char *magic);
void fz_writer_begin(fz_writer *w, int section);    /* aligns to 8 */
void fz_writer_write(fz_writer *w, const void *data, size_t n);
void fz_writer_end(fz_writer *w);
/* Finish: write header, flush, rename into place. Returns header copy. */
void fz_writer_close(fz_writer *w, tz_file_header *out_header);

/* zigzag varint append to a byte vector */
typedef FZ_VEC(uint8_t) fz_bytes;
void fz_put_varint(fz_bytes *b, uint32_t v);

#endif
