/* Contraction hierarchy construction. */
#ifndef FZ_CH_H
#define FZ_CH_H

#include <stddef.h>
#include <stdint.h>
#include "../../Core/tz_format.h"

typedef struct {
    uint32_t u, v;       /* directed arc u -> v */
    uint32_t w;          /* weight, deciseconds, > 0 */
    uint32_t payload;    /* (segment << 1) | travel direction */
} fz_arc;

typedef struct {
    uint32_t   n_nodes;
    uint32_t  *rank;     /* node -> CH id (0 = least important) */
    uint32_t  *first;    /* n_nodes + 1, indexed by CH id */
    tz_chedge *edges;    /* targets and middles are CH ids */
    uint32_t   n_edges;
    uint64_t   shortcuts;
} fz_ch;

/* Builds the hierarchy. `arcs` is consumed (sorted and deduplicated). */
void fz_ch_build(uint32_t n_nodes, fz_arc *arcs, size_t n_arcs, fz_ch *out);
void fz_ch_free(fz_ch *ch);

#endif
