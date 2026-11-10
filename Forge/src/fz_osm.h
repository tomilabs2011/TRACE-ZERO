/* Interpretation of OpenStreetMap tags for routing and search.
 * All routing rules live here so they can be read and tuned in one place;
 * docs/ROUTING.md explains each choice. */
#ifndef FZ_OSM_H
#define FZ_OSM_H

#include <stdint.h>
#include "fz_pbf.h"

const char *fz_tag_get(const fz_tag *t, int n, const char *key);

#define FZ_ACC_FWD 1
#define FZ_ACC_BWD 2

typedef struct {
    uint8_t cls;            /* TZ_CLS_* */
    uint8_t flags;          /* TZ_SEG_* */
    uint8_t acc[3];         /* per profile: FZ_ACC_FWD | FZ_ACC_BWD */
    float   speed[3];       /* km/h, per profile */
    float   factor[3];      /* weight multiplier (>= 0.5): route preference */
    int32_t ferry_s;        /* ferry crossing time from the duration tag, -1 unknown */
} fz_way_attr;

/* Returns 1 if the way can be used by at least one profile. */
int fz_way_attr_parse(const fz_tag *t, int n, fz_way_attr *a);

/* Bit (1 << profile) set if the node blocks that profile. */
uint8_t fz_barrier_mask(const fz_tag *t, int n);
int     fz_is_traffic_signal(const fz_tag *t, int n);

/* Turn restriction relation. */
typedef enum { FZ_R_NO = 0, FZ_R_ONLY = 1 } fz_rkind;
typedef enum { FZ_TURN_ANY = 0, FZ_TURN_LEFT, FZ_TURN_RIGHT, FZ_TURN_STRAIGHT, FZ_TURN_U } fz_rturn;
/* Parses a restriction relation's tags; returns profile mask (0 = ignore). */
uint8_t fz_restriction_parse(const fz_tag *t, int n, fz_rkind *kind, fz_rturn *turn);

/* ------------------------------------------------------------- search */

/* place=* -> TZ_PLACE_*, or -1 */
int fz_place_type(const char *v);

typedef struct {
    int         group;      /* TZ_POI_* */
    char        label[48];  /* human category, e.g. "Fast food" */
    uint16_t    rank_bonus;
} fz_poi_info;

/* Returns 1 if the element is a searchable point of interest. */
int fz_poi_parse(const fz_tag *t, int n, fz_poi_info *out);

#endif
