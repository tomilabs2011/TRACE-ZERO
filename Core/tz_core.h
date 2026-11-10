/*
 * TRACE//ZERO core: offline routing, snapping and search.
 *
 * Plain C99 with no dependencies beyond libc and POSIX mmap, so the same
 * code runs on an iPhone 5s (iOS 12, arm64), a current iPhone, a Mac, or a
 * Linux test box. The iOS app reaches it through a Swift bridging header;
 * everything UI- or platform-specific lives outside this directory.
 *
 * Ownership rules:
 *   - Handles (tz_roads, tz_graph, tz_search) are read-only after opening and
 *     may be shared between threads. Each query allocates its own scratch
 *     memory, so concurrent queries are safe.
 *   - `const char *` strings returned by the library point into the mapped
 *     files and stay valid until the owning handle is closed.
 *   - A tz_graph borrows its tz_roads: close graphs before their roads.
 */
#ifndef TZ_CORE_H
#define TZ_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- status */

typedef enum {
    TZ_OK = 0,
    TZ_ERR_IO,            /* file missing or unreadable */
    TZ_ERR_FORMAT,        /* not a TRACE//ZERO file, or corrupt/truncated */
    TZ_ERR_VERSION,       /* built by an incompatible builder version */
    TZ_ERR_MISMATCH,      /* routing graph does not belong to this roads file */
    TZ_ERR_NOMEM,
    TZ_ERR_NO_START,      /* no usable road near the start */
    TZ_ERR_NO_END,        /* no usable road near the destination */
    TZ_ERR_NO_ROUTE,      /* both snapped, but not connected */
    TZ_ERR_ARGUMENT
} tz_status;

const char *tz_status_message(int status);

/* ------------------------------------------------------------------ geo */

/* Great-circle distance in metres. */
double tz_distance_m(double lat1, double lon1, double lat2, double lon2);
/* Initial bearing from point 1 to point 2, degrees 0..360 (0 = north). */
double tz_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/*
 * Parse a coordinate typed by a person. Accepts, among others:
 *   "47.4979, 19.0402"   "47.4979 19.0402"   "N47.4979 E19.0402"
 *   "47.4979N 19.0402E"  "47°29'52.4\"N 19°02'24.7\"E"  "47 29.873 N 19 2.412 E"
 *   "geo:47.4979,19.0402"
 * Returns 1 and fills lat/lon on success, 0 otherwise.
 */
int tz_parse_coordinate(const char *text, double *lat, double *lon);

/*
 * Search normalisation: lower-case, strip accents ("Váci" -> "vaci",
 * "Straße" -> "strasse"), split letters from digits ("12a" -> "12 a") and
 * collapse punctuation into single spaces. Writes a NUL-terminated string
 * and returns its length (truncated to outcap-1).
 */
size_t tz_normalize(const char *in, char *out, size_t outcap);

/* ---------------------------------------------------------------- roads */

typedef struct tz_roads tz_roads;

tz_roads *tz_roads_open(const char *path, int *status);
void      tz_roads_close(tz_roads *roads);
/* Data bounding box: min_lat, min_lon, max_lat, max_lon. */
void      tz_roads_bbox(const tz_roads *roads, double out[4]);
uint32_t  tz_roads_segment_count(const tz_roads *roads);

/* Name of the nearest named road within radius_m, or NULL. */
const char *tz_roads_nearest_name(const tz_roads *roads, double lat, double lon,
                                  double radius_m, double *dist_m);

/* ---------------------------------------------------------------- graph */

typedef enum { TZ_CAR = 0, TZ_BIKE = 1, TZ_FOOT = 2 } tz_profile;

typedef struct tz_graph tz_graph;

tz_graph *tz_graph_open(const tz_roads *roads, const char *path, int *status);
void      tz_graph_close(tz_graph *graph);
int       tz_graph_profile(const tz_graph *graph);

typedef struct {
    double   lat, lon;      /* projected point on the road */
    double   dist_m;        /* distance from the query point */
    double   bearing;       /* direction of the segment (a->b) at that point */
    double   frac;          /* position along the segment, 0..1 by length */
    uint32_t seg;           /* segment id */
} tz_snap;

/* Nearest road usable by the profile within radius_m. 1 = found. */
int tz_snap_point(const tz_graph *graph, double lat, double lon, double radius_m,
                  tz_snap *out);

typedef struct {
    double lat, lon;
    double heading;         /* direction of travel in degrees, or < 0 if unknown */
    double radius_m;        /* snapping radius; <= 0 uses the default (1500 m) */
} tz_waypoint;

typedef enum {
    TZ_MAN_DEPART = 0,
    TZ_MAN_TURN,            /* see modifier */
    TZ_MAN_CONTINUE,        /* road changes name, no real turn */
    TZ_MAN_FORK,            /* keep left / right at a split */
    TZ_MAN_RAMP,            /* take a motorway ramp */
    TZ_MAN_ROUNDABOUT,      /* enter roundabout, take `exit` */
    TZ_MAN_FERRY,           /* board a ferry */
    TZ_MAN_ARRIVE
} tz_maneuver_type;

/* modifier: -3 sharp left, -2 left, -1 slight left, 0 straight,
 *           +1 slight right, +2 right, +3 sharp right, 4 U-turn */
typedef struct {
    uint8_t     type;           /* tz_maneuver_type */
    int8_t      modifier;
    uint8_t     exit;           /* roundabout exit number, 1-based */
    uint8_t     _pad;
    uint32_t    point;          /* index into the route polyline */
    double      dist_m;         /* distance from route start */
    double      time_s;         /* travel time from route start */
    double      bearing_before; /* degrees */
    double      bearing_after;  /* degrees */
    const char *name;           /* road taken after the maneuver, may be "" */
    const char *ref;            /* its route number, may be "" */
} tz_maneuver;

typedef struct {
    int          profile;
    uint32_t     npts;
    double      *lat, *lon;     /* polyline, npts each */
    double      *cum_dist;      /* metres from start at each point */
    double      *cum_time;      /* seconds from start at each point */
    double       distance_m;
    double       duration_s;
    uint32_t     nman;
    tz_maneuver *man;
    double       snap_start_m;  /* how far the start was from the road */
    double       snap_end_m;
    double       compute_ms;    /* time spent in the query */
    uint32_t     settled;       /* nodes settled by the search (diagnostics) */
} tz_route;

/* Compute a route. On success returns TZ_OK and *out must be freed with
 * tz_route_free. */
int  tz_route_compute(const tz_graph *graph, const tz_waypoint *from,
                      const tz_waypoint *to, tz_route **out);
void tz_route_free(tz_route *route);

/* --------------------------------------------------------------- search */

typedef struct tz_search tz_search;

tz_search *tz_search_open(const char *path, int *status);
void       tz_search_close(tz_search *search);
uint32_t   tz_search_entry_count(const tz_search *search);

/* Place types (tz_hit.sub when kind == TZ_HIT_PLACE). */
enum {
    TZ_PLACE_CITY = 0, TZ_PLACE_TOWN, TZ_PLACE_VILLAGE, TZ_PLACE_HAMLET,
    TZ_PLACE_SUBURB, TZ_PLACE_NEIGHBOURHOOD, TZ_PLACE_LOCALITY, TZ_PLACE_OTHER
};
/* POI groups (tz_hit.sub when kind == TZ_HIT_POI). */
enum {
    TZ_POI_OTHER = 0, TZ_POI_FOOD, TZ_POI_SHOP, TZ_POI_TRANSPORT, TZ_POI_LODGING,
    TZ_POI_HEALTH, TZ_POI_CULTURE, TZ_POI_FUEL, TZ_POI_PARKING, TZ_POI_NATURE,
    TZ_POI_EDUCATION, TZ_POI_SERVICE
};

/* Same values as TZ_KIND_* in tz_format.h. */
enum { TZ_HIT_PLACE = 0, TZ_HIT_STREET, TZ_HIT_ADDRESS, TZ_HIT_POI, TZ_HIT_COORD };

typedef struct {
    uint32_t    id;
    uint8_t     kind;           /* TZ_HIT_* */
    uint8_t     sub;
    uint16_t    _pad;
    double      lat, lon;
    double      dist_m;         /* from the reference point, or -1 */
    double      score;
    const char *name;
    const char *context;
} tz_hit;

/*
 * Search the index. Words may be typed in any order and the last word may be
 * incomplete. If has_ref is non-zero, results near (ref_lat, ref_lon) rank
 * higher. Returns the number of hits written (<= max_hits).
 */
int tz_search_query(const tz_search *search, const char *query,
                    double ref_lat, double ref_lon, int has_ref,
                    tz_hit *hits, int max_hits);

/* Nearest address, POI or place to a point (reverse geocoding). 1 = found. */
int tz_search_nearest(const tz_search *search, double lat, double lon,
                      double radius_m, tz_hit *hit);

#ifdef __cplusplus
}
#endif

#endif /* TZ_CORE_H */
