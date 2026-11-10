#include "fz_osm.h"
#include "../../Core/tz_format.h"
#include "../../Core/tz_core.h"

#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *fz_tag_get(const fz_tag *t, int n, const char *key)
{
    for (int i = 0; i < n; i++) if (strcmp(t[i].k, key) == 0) return t[i].v;
    return NULL;
}

static int eq(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

static int in_list(const char *v, const char *const *list)
{
    if (!v) return 0;
    for (; *list; list++) if (strcmp(v, *list) == 0) return 1;
    return 0;
}

/* First item of a ';'-separated value, copied into buf. */
static const char *first_value(const char *v, char *buf, size_t cap)
{
    if (!v) return NULL;
    size_t i = 0;
    while (v[i] && v[i] != ';' && i + 1 < cap) { buf[i] = v[i]; i++; }
    buf[i] = 0;
    while (i > 0 && buf[i - 1] == ' ') buf[--i] = 0;
    return buf;
}

/* ------------------------------------------------------------- access */

typedef enum { A_UNSET = 0, A_YES, A_NO, A_DEST, A_PUSH } accval;

static accval acc_value(const char *raw)
{
    static const char *const yes[] = { "yes", "permissive", "designated", "official", "public",
                                       "use_sidepath", "discouraged", "unknown", NULL };
    static const char *const no[] = { "no", "private", "agricultural", "forestry", "emergency",
                                      "military", "permit", "delivery", "agricultural;forestry",
                                      "restricted", "residents", "bus", "psv", NULL };
    static const char *const dest[] = { "destination", "customers", NULL };
    char buf[64];
    const char *v = first_value(raw, buf, sizeof buf);
    if (!v || !*v) return A_UNSET;
    if (in_list(v, yes)) return A_YES;
    if (in_list(v, no)) return A_NO;
    if (in_list(v, dest)) return A_DEST;
    if (strcmp(v, "dismount") == 0) return A_PUSH;
    return A_UNSET;
}

static const char *const CAR_KEYS[] = { "access", "vehicle", "motor_vehicle", "motorcar", NULL };
static const char *const BIKE_KEYS[] = { "access", "vehicle", "bicycle", NULL };
static const char *const FOOT_KEYS[] = { "access", "foot", NULL };
static const char *const *const PROFILE_KEYS[3] = { CAR_KEYS, BIKE_KEYS, FOOT_KEYS };

/* Most specific access tag wins. */
static accval profile_access(const fz_tag *t, int n, int profile)
{
    accval r = A_UNSET;
    for (const char *const *k = PROFILE_KEYS[profile]; *k; k++) {
        accval a = acc_value(fz_tag_get(t, n, *k));
        if (a != A_UNSET) r = a;
    }
    return r;
}

/* -------------------------------------------------------------- speed */

/* Parse maxspeed into km/h; 0 = unknown. */
static float parse_maxspeed(const char *v)
{
    if (!v) return 0;
    if (strcmp(v, "none") == 0) return 140;
    if (strcmp(v, "walk") == 0) return 6;
    const char *colon = strchr(v, ':');
    if (colon && !isdigit((unsigned char)v[0])) {
        const char *k = colon + 1;
        if (strcmp(k, "urban") == 0) return 50;
        if (strcmp(k, "rural") == 0) return 90;
        if (strcmp(k, "motorway") == 0) return 130;
        if (strcmp(k, "trunk") == 0) return 110;
        if (strcmp(k, "living_street") == 0) return 20;
        if (strcmp(k, "bicycle_road") == 0) return 30;
        if (strncmp(k, "zone", 4) == 0) {
            const char *d = k + 4;
            while (*d && !isdigit((unsigned char)*d)) d++;
            if (isdigit((unsigned char)*d)) return (float)atof(d);
        }
        return 0;
    }
    if (!isdigit((unsigned char)v[0])) return 0;
    char *end;
    double s = strtod(v, &end);
    while (*end == ' ') end++;
    if (strncmp(end, "mph", 3) == 0) s *= 1.609344;
    else if (strncmp(end, "knots", 5) == 0) s *= 1.852;
    if (s < 3 || s > 200) return 0;
    return (float)s;
}

static int parse_duration_s(const char *v)
{
    if (!v || !*v) return -1;
    int h = 0, m = 0, s = 0;
    if (strchr(v, ':')) {
        int n = sscanf(v, "%d:%d:%d", &h, &m, &s);
        if (n < 2) return -1;
        if (n == 2) s = 0;
    } else {
        /* plain number of minutes (sometimes written as "45" or "1.5" hours?) */
        double mins = atof(v);
        if (mins <= 0) return -1;
        return (int)(mins * 60.0);
    }
    if (h < 0 || m < 0 || s < 0) return -1;
    int total = h * 3600 + m * 60 + s;
    return total > 0 ? total : -1;
}

typedef struct { const char *name; uint8_t cls; uint8_t link; } hwmap;
static const hwmap HW[] = {
    { "motorway", TZ_CLS_MOTORWAY, 0 },      { "motorway_link", TZ_CLS_MOTORWAY, 1 },
    { "trunk", TZ_CLS_TRUNK, 0 },            { "trunk_link", TZ_CLS_TRUNK, 1 },
    { "primary", TZ_CLS_PRIMARY, 0 },        { "primary_link", TZ_CLS_PRIMARY, 1 },
    { "secondary", TZ_CLS_SECONDARY, 0 },    { "secondary_link", TZ_CLS_SECONDARY, 1 },
    { "tertiary", TZ_CLS_TERTIARY, 0 },      { "tertiary_link", TZ_CLS_TERTIARY, 1 },
    { "unclassified", TZ_CLS_UNCLASSIFIED, 0 }, { "residential", TZ_CLS_RESIDENTIAL, 0 },
    { "living_street", TZ_CLS_LIVING_STREET, 0 }, { "service", TZ_CLS_SERVICE, 0 },
    { "track", TZ_CLS_TRACK, 0 },            { "cycleway", TZ_CLS_CYCLEWAY, 0 },
    { "path", TZ_CLS_PATH, 0 },              { "footway", TZ_CLS_FOOTWAY, 0 },
    { "pedestrian", TZ_CLS_PEDESTRIAN, 0 },  { "steps", TZ_CLS_STEPS, 0 },
    { "bridleway", TZ_CLS_BRIDLEWAY, 0 },    { "road", TZ_CLS_ROAD, 0 },
    { NULL, 0, 0 }
};

/* Default access per class: car, bike, foot (A_YES / A_NO / A_PUSH). */
static const accval DEFAULT_ACCESS[TZ_CLS_COUNT][3] = {
    [TZ_CLS_MOTORWAY]      = { A_YES, A_NO,   A_NO  },
    [TZ_CLS_TRUNK]         = { A_YES, A_YES,  A_YES },
    [TZ_CLS_PRIMARY]       = { A_YES, A_YES,  A_YES },
    [TZ_CLS_SECONDARY]     = { A_YES, A_YES,  A_YES },
    [TZ_CLS_TERTIARY]      = { A_YES, A_YES,  A_YES },
    [TZ_CLS_UNCLASSIFIED]  = { A_YES, A_YES,  A_YES },
    [TZ_CLS_RESIDENTIAL]   = { A_YES, A_YES,  A_YES },
    [TZ_CLS_LIVING_STREET] = { A_YES, A_YES,  A_YES },
    [TZ_CLS_SERVICE]       = { A_YES, A_YES,  A_YES },
    [TZ_CLS_TRACK]         = { A_NO,  A_YES,  A_YES },
    [TZ_CLS_CYCLEWAY]      = { A_NO,  A_YES,  A_YES },
    [TZ_CLS_PATH]          = { A_NO,  A_YES,  A_YES },
    [TZ_CLS_FOOTWAY]       = { A_NO,  A_PUSH, A_YES },
    [TZ_CLS_PEDESTRIAN]    = { A_NO,  A_PUSH, A_YES },
    [TZ_CLS_STEPS]         = { A_NO,  A_NO,   A_YES },
    [TZ_CLS_BRIDLEWAY]     = { A_NO,  A_NO,   A_YES },
    [TZ_CLS_ROAD]          = { A_YES, A_YES,  A_YES },
    [TZ_CLS_FERRY]         = { A_NO,  A_YES,  A_YES },
};

typedef struct { float def, link, cap, k; } carspeed;
static const carspeed CAR_SPEED[TZ_CLS_COUNT] = {
    [TZ_CLS_MOTORWAY]      = { 115, 60, 140, 0.9f },
    [TZ_CLS_TRUNK]         = { 85,  50, 120, 0.9f },
    [TZ_CLS_PRIMARY]       = { 65,  40, 100, 0.8f },
    [TZ_CLS_SECONDARY]     = { 55,  35, 90,  0.8f },
    [TZ_CLS_TERTIARY]      = { 45,  30, 80,  0.7f },
    [TZ_CLS_UNCLASSIFIED]  = { 40,  40, 70,  0.7f },
    [TZ_CLS_RESIDENTIAL]   = { 25,  25, 50,  0.6f },
    [TZ_CLS_LIVING_STREET] = { 8,   8,  20,  0.6f },
    [TZ_CLS_SERVICE]       = { 15,  15, 30,  0.6f },
    [TZ_CLS_TRACK]         = { 15,  15, 30,  0.6f },
    [TZ_CLS_ROAD]          = { 25,  25, 50,  0.6f },
};

static const float BIKE_SPEED[TZ_CLS_COUNT] = {
    [TZ_CLS_TRUNK] = 18, [TZ_CLS_PRIMARY] = 18, [TZ_CLS_SECONDARY] = 18, [TZ_CLS_TERTIARY] = 18,
    [TZ_CLS_UNCLASSIFIED] = 17, [TZ_CLS_RESIDENTIAL] = 17, [TZ_CLS_LIVING_STREET] = 12,
    [TZ_CLS_SERVICE] = 15, [TZ_CLS_TRACK] = 13, [TZ_CLS_CYCLEWAY] = 18, [TZ_CLS_PATH] = 13,
    [TZ_CLS_FOOTWAY] = 14, [TZ_CLS_PEDESTRIAN] = 12, [TZ_CLS_STEPS] = 2,
    [TZ_CLS_BRIDLEWAY] = 10, [TZ_CLS_ROAD] = 15, [TZ_CLS_FERRY] = 15,
};

static const float BIKE_FACTOR[TZ_CLS_COUNT] = {
    [TZ_CLS_TRUNK] = 3.0f, [TZ_CLS_PRIMARY] = 1.8f, [TZ_CLS_SECONDARY] = 1.4f, [TZ_CLS_TERTIARY] = 1.15f,
    [TZ_CLS_UNCLASSIFIED] = 1.0f, [TZ_CLS_RESIDENTIAL] = 1.0f, [TZ_CLS_LIVING_STREET] = 1.0f,
    [TZ_CLS_SERVICE] = 1.1f, [TZ_CLS_TRACK] = 1.1f, [TZ_CLS_CYCLEWAY] = 0.85f, [TZ_CLS_PATH] = 1.1f,
    [TZ_CLS_FOOTWAY] = 1.0f, [TZ_CLS_PEDESTRIAN] = 1.0f, [TZ_CLS_STEPS] = 3.0f,
    [TZ_CLS_BRIDLEWAY] = 1.2f, [TZ_CLS_ROAD] = 1.1f, [TZ_CLS_FERRY] = 1.0f,
};

static const float FOOT_FACTOR[TZ_CLS_COUNT] = {
    [TZ_CLS_TRUNK] = 3.0f, [TZ_CLS_PRIMARY] = 1.3f, [TZ_CLS_SECONDARY] = 1.2f, [TZ_CLS_TERTIARY] = 1.05f,
    [TZ_CLS_UNCLASSIFIED] = 1.0f, [TZ_CLS_RESIDENTIAL] = 1.0f, [TZ_CLS_LIVING_STREET] = 0.95f,
    [TZ_CLS_SERVICE] = 1.1f, [TZ_CLS_TRACK] = 1.0f, [TZ_CLS_CYCLEWAY] = 1.4f, [TZ_CLS_PATH] = 0.95f,
    [TZ_CLS_FOOTWAY] = 0.95f, [TZ_CLS_PEDESTRIAN] = 0.95f, [TZ_CLS_STEPS] = 1.0f,
    [TZ_CLS_BRIDLEWAY] = 1.0f, [TZ_CLS_ROAD] = 1.0f, [TZ_CLS_FERRY] = 1.0f,
};

static const char *const PAVED[] = { "paved", "asphalt", "concrete", "concrete:plates", "concrete:lanes",
                                     "paving_stones", "chipseal", "metal", "wood", NULL };
static const char *const ROUGH_PAVED[] = { "sett", "cobblestone", "unhewn_cobblestone", "cobblestone:flattened",
                                           "grass_paver", NULL };
static const char *const FIRM[] = { "compacted", "fine_gravel", NULL };
static const char *const LOOSE[] = { "gravel", "unpaved", "dirt", "ground", "grass", "earth",
                                     "pebblestone", "rock", "stone", NULL };
static const char *const SOFT[] = { "sand", "mud", "woodchips", "snow", "ice", "salt", NULL };

/* Bicycle speed limit imposed by the surface; 0 = no limit. */
static float surface_bike_cap(const fz_tag *t, int n, int *unpaved)
{
    const char *s = fz_tag_get(t, n, "surface");
    const char *tt = fz_tag_get(t, n, "tracktype");
    const char *sm = fz_tag_get(t, n, "smoothness");
    float cap = 0;
    *unpaved = 0;
    if (s) {
        if (in_list(s, ROUGH_PAVED)) cap = 12;
        else if (in_list(s, FIRM)) { cap = 15; *unpaved = 1; }
        else if (in_list(s, LOOSE)) { cap = 10; *unpaved = 1; }
        else if (in_list(s, SOFT)) { cap = 6; *unpaved = 1; }
        else if (!in_list(s, PAVED)) cap = 0;
    }
    if (tt) {
        float c = 0;
        if (eq(tt, "grade2")) c = 13;
        else if (eq(tt, "grade3")) c = 10;
        else if (eq(tt, "grade4") || eq(tt, "grade5")) c = 7;
        if (c > 0) { *unpaved = 1; if (cap == 0 || c < cap) cap = c; }
    }
    if (sm) {
        float c = 0;
        if (eq(sm, "bad") || eq(sm, "very_bad")) c = 10;
        else if (eq(sm, "horrible") || eq(sm, "very_horrible") || eq(sm, "impassable")) c = 5;
        if (c > 0 && (cap == 0 || c < cap)) cap = c;
    }
    return cap;
}

static int has_cycle_lane(const fz_tag *t, int n)
{
    static const char *const keys[] = { "cycleway", "cycleway:right", "cycleway:left", "cycleway:both", NULL };
    static const char *const lanes[] = { "lane", "track", "share_busway", "separate", "exclusive", NULL };
    for (const char *const *k = keys; *k; k++)
        if (in_list(fz_tag_get(t, n, *k), lanes)) return 1;
    return 0;
}

static int bike_contraflow(const fz_tag *t, int n)
{
    static const char *const keys[] = { "cycleway", "cycleway:right", "cycleway:left", "cycleway:both", NULL };
    for (const char *const *k = keys; *k; k++) {
        const char *v = fz_tag_get(t, n, *k);
        if (v && strncmp(v, "opposite", 8) == 0) return 1;
    }
    return 0;
}

static int oneway_value(const char *v, int *reversible)
{
    *reversible = 0;
    if (!v) return 2; /* unset */
    if (eq(v, "yes") || eq(v, "true") || eq(v, "1")) return 1;
    if (eq(v, "-1") || eq(v, "reverse")) return -1;
    if (eq(v, "no") || eq(v, "false") || eq(v, "0")) return 0;
    if (eq(v, "reversible") || eq(v, "alternating")) { *reversible = 1; return 0; }
    return 2;
}

static uint8_t dir_bits(int ow)
{
    return ow == 1 ? FZ_ACC_FWD : ow == -1 ? FZ_ACC_BWD : (FZ_ACC_FWD | FZ_ACC_BWD);
}

int fz_way_attr_parse(const fz_tag *t, int n, fz_way_attr *a)
{
    memset(a, 0, sizeof *a);
    a->ferry_s = -1;
    for (int p = 0; p < 3; p++) a->factor[p] = 1.0f;

    const char *hw = fz_tag_get(t, n, "highway");
    const char *route = fz_tag_get(t, n, "route");
    int cls = -1, link = 0;
    if (hw) {
        for (const hwmap *m = HW; m->name; m++)
            if (strcmp(hw, m->name) == 0) { cls = m->cls; link = m->link; break; }
    }
    if (cls < 0 && eq(route, "ferry")) cls = TZ_CLS_FERRY;
    if (cls < 0) return 0;
    if (cls != TZ_CLS_FERRY && eq(fz_tag_get(t, n, "area"), "yes")) return 0;
    if (fz_tag_get(t, n, "construction") && eq(hw, "construction")) return 0;

    a->cls = (uint8_t)cls;
    if (link) a->flags |= TZ_SEG_LINK;
    const char *junction = fz_tag_get(t, n, "junction");
    int roundabout = eq(junction, "roundabout") || eq(junction, "circular");
    if (roundabout) a->flags |= TZ_SEG_ROUNDABOUT;
    if (fz_tag_get(t, n, "tunnel") && !eq(fz_tag_get(t, n, "tunnel"), "no")) a->flags |= TZ_SEG_TUNNEL;
    if (fz_tag_get(t, n, "bridge") && !eq(fz_tag_get(t, n, "bridge"), "no")) a->flags |= TZ_SEG_BRIDGE;
    if (eq(fz_tag_get(t, n, "toll"), "yes")) a->flags |= TZ_SEG_TOLL;
    if (cls == TZ_CLS_FERRY) a->flags |= TZ_SEG_FERRY;

    int motorroad = eq(fz_tag_get(t, n, "motorroad"), "yes");
    int unpaved = 0;
    float bike_cap = surface_bike_cap(t, n, &unpaved);
    if (unpaved) a->flags |= TZ_SEG_UNPAVED;

    /* ---- access per profile ---- */
    accval acc[3];
    for (int p = 0; p < 3; p++) {
        accval def = DEFAULT_ACCESS[cls][p];
        accval tag = profile_access(t, n, p);
        acc[p] = tag != A_UNSET ? tag : def;
        /* A ferry carries cars only when tagged for them. */
        if (cls == TZ_CLS_FERRY && p == TZ_PROFILE_CAR && tag != A_YES && tag != A_DEST) acc[p] = A_NO;
        /* "Push your bike" on a footway or steps does not open it to cars. */
        if (acc[p] == A_PUSH && p != TZ_PROFILE_BIKE) acc[p] = A_YES;
    }
    if (motorroad) { acc[TZ_PROFILE_BIKE] = A_NO; acc[TZ_PROFILE_FOOT] = A_NO; }
    /* Steps: a bike can be carried/pushed only where explicitly allowed
     * (bicycle=yes/dismount or a bicycle ramp), never ridden. */
    if (cls == TZ_CLS_STEPS) {
        accval tb = profile_access(t, n, TZ_PROFILE_BIKE);
        int ok = tb == A_YES || tb == A_DEST || tb == A_PUSH ||
                 eq(fz_tag_get(t, n, "ramp:bicycle"), "yes");
        acc[TZ_PROFILE_BIKE] = (ok && tb != A_NO) ? A_PUSH : A_NO;
    }

    /* ---- direction ---- */
    int rev = 0;
    int ow = oneway_value(fz_tag_get(t, n, "oneway"), &rev);
    if (rev) acc[TZ_PROFILE_CAR] = A_NO;
    if (ow == 2) ow = (roundabout || (cls == TZ_CLS_MOTORWAY && !link)) ? 1 : 0;

    int rev_b = 0;
    int ow_b = oneway_value(fz_tag_get(t, n, "oneway:bicycle"), &rev_b);
    if (ow_b == 2) ow_b = bike_contraflow(t, n) ? 0 : ow;
    if (acc[TZ_PROFILE_BIKE] == A_PUSH) ow_b = 0; /* walking the bike */

    int rev_f = 0;
    int ow_f = oneway_value(fz_tag_get(t, n, "oneway:foot"), &rev_f);
    if (ow_f == 2) {
        int footish = cls == TZ_CLS_FOOTWAY || cls == TZ_CLS_STEPS || cls == TZ_CLS_PATH || cls == TZ_CLS_PEDESTRIAN;
        ow_f = footish ? ow : 0;
    }

    int ows[3] = { ow, ow_b, ow_f };
    int any = 0;
    for (int p = 0; p < 3; p++) {
        if (acc[p] == A_NO) { a->acc[p] = 0; continue; }
        a->acc[p] = dir_bits(ows[p]);
        any = 1;
    }
    if (!any) return 0;

    /* ---- car ---- */
    if (a->acc[TZ_PROFILE_CAR]) {
        float sp;
        if (cls == TZ_CLS_FERRY) sp = 25;
        else {
            const carspeed *cs = &CAR_SPEED[cls];
            float def = link ? cs->link : cs->def;
            float maxs = parse_maxspeed(fz_tag_get(t, n, "maxspeed"));
            sp = maxs > 0 ? maxs * cs->k : def;
            if (sp > cs->cap) sp = cs->cap;
            if (link && sp > cs->link * 1.3f) sp = cs->link * 1.3f;
        }
        float f = 1.0f;
        const char *service = fz_tag_get(t, n, "service");
        if (cls == TZ_CLS_LIVING_STREET) f *= 1.5f;
        if (cls == TZ_CLS_SERVICE) {
            f *= 1.3f;
            if (eq(service, "parking_aisle") || eq(service, "driveway")) { f *= 1.6f; if (sp > 10) sp = 10; }
            if (eq(service, "drive-through")) f *= 3.0f;
        }
        if (cls == TZ_CLS_TRACK) f *= 2.0f;
        if (unpaved && cls != TZ_CLS_FERRY) { if (sp > 25) sp = 25; f *= 1.5f; }
        if (acc[TZ_PROFILE_CAR] == A_DEST) f *= 3.0f;
        if (sp < 5) sp = 5;
        a->speed[TZ_PROFILE_CAR] = sp;
        a->factor[TZ_PROFILE_CAR] = f;
    }

    /* ---- bicycle ---- */
    if (a->acc[TZ_PROFILE_BIKE]) {
        float sp, f;
        if (acc[TZ_PROFILE_BIKE] == A_PUSH) {
            sp = cls == TZ_CLS_STEPS ? 2.0f : 5.0f;
            f = cls == TZ_CLS_STEPS ? 3.0f : 1.5f;
        } else {
            sp = BIKE_SPEED[cls] > 0 ? BIKE_SPEED[cls] : 15;
            f = BIKE_FACTOR[cls] > 0 ? BIKE_FACTOR[cls] : 1.0f;
            if ((cls == TZ_CLS_PRIMARY || cls == TZ_CLS_SECONDARY) && has_cycle_lane(t, n))
                f = cls == TZ_CLS_PRIMARY ? 1.1f : 1.0f;
            if (cls == TZ_CLS_PATH && eq(fz_tag_get(t, n, "bicycle"), "designated")) f = 0.9f;
            if (bike_cap > 0 && sp > bike_cap) sp = bike_cap;
            if (unpaved) f *= 1.2f;
            if (acc[TZ_PROFILE_BIKE] == A_DEST) f *= 1.5f;
        }
        a->speed[TZ_PROFILE_BIKE] = sp;
        a->factor[TZ_PROFILE_BIKE] = f;
    }

    /* ---- foot ---- */
    if (a->acc[TZ_PROFILE_FOOT]) {
        float sp = cls == TZ_CLS_STEPS ? 2.5f : cls == TZ_CLS_FERRY ? 15.0f : 5.0f;
        float f = FOOT_FACTOR[cls] > 0 ? FOOT_FACTOR[cls] : 1.0f;
        const char *sw = fz_tag_get(t, n, "sidewalk");
        if ((cls == TZ_CLS_PRIMARY || cls == TZ_CLS_SECONDARY || cls == TZ_CLS_TERTIARY) &&
            (eq(sw, "no") || eq(sw, "none")))
            f *= 1.5f;
        a->speed[TZ_PROFILE_FOOT] = sp;
        a->factor[TZ_PROFILE_FOOT] = f;
    }

    if (cls == TZ_CLS_FERRY) a->ferry_s = parse_duration_s(fz_tag_get(t, n, "duration"));
    return 1;
}

/* ---------------------------------------------------------- barriers */

uint8_t fz_barrier_mask(const fz_tag *t, int n)
{
    const char *b = fz_tag_get(t, n, "barrier");
    if (!b) return 0;
    static const char *const all[] = { "fence", "wall", "city_wall", "retaining_wall", "hedge", "debris", NULL };
    static const char *const car[] = { "bollard", "block", "cycle_barrier", "chain", "jersey_barrier", "log",
                                       "motorcycle_barrier", "bus_trap", "sump_buster", "planter", "barrier_board",
                                       "rope", "kerb", NULL };
    static const char *const carbike[] = { "stile", "turnstile", "full-height_turnstile", "kissing_gate",
                                           "horse_stile", NULL };
    static const char *const gates[] = { "gate", "lift_gate", "swing_gate", "sliding_gate", "wicket_gate",
                                         "hampshire_gate", "entrance", "toll_booth", "border_control", NULL };
    uint8_t mask = 0;
    const int C = 1 << TZ_PROFILE_CAR, B = 1 << TZ_PROFILE_BIKE, F = 1 << TZ_PROFILE_FOOT;
    if (in_list(b, all)) mask = (uint8_t)(C | B | F);
    else if (in_list(b, car)) mask = (uint8_t)C;
    else if (in_list(b, carbike)) mask = (uint8_t)(C | B);
    else if (in_list(b, gates)) {
        if (eq(fz_tag_get(t, n, "locked"), "yes")) mask = (uint8_t)(C | B | F);
    }
    /* Explicit access tags on the node override the defaults. */
    for (int p = 0; p < 3; p++) {
        accval a = profile_access(t, n, p);
        if (a == A_NO) mask |= (uint8_t)(1 << p);
        else if (a == A_YES || a == A_DEST || a == A_PUSH) mask &= (uint8_t)~(1 << p);
    }
    return mask;
}

int fz_is_traffic_signal(const fz_tag *t, int n)
{
    return eq(fz_tag_get(t, n, "highway"), "traffic_signals");
}

/* ------------------------------------------------------- restrictions */

static int parse_rvalue(const char *v, fz_rkind *kind, fz_rturn *turn)
{
    if (!v) return 0;
    if (strncmp(v, "no_", 3) == 0) *kind = FZ_R_NO;
    else if (strncmp(v, "only_", 5) == 0) *kind = FZ_R_ONLY;
    else return 0;
    const char *r = v + (*kind == FZ_R_NO ? 3 : 5);
    if (strcmp(r, "left_turn") == 0) *turn = FZ_TURN_LEFT;
    else if (strcmp(r, "right_turn") == 0) *turn = FZ_TURN_RIGHT;
    else if (strcmp(r, "straight_on") == 0) *turn = FZ_TURN_STRAIGHT;
    else if (strcmp(r, "u_turn") == 0) *turn = FZ_TURN_U;
    else return 0; /* no_entry, no_exit and friends need several from/to ways */
    return 1;
}

uint8_t fz_restriction_parse(const fz_tag *t, int n, fz_rkind *kind, fz_rturn *turn)
{
    if (!eq(fz_tag_get(t, n, "type"), "restriction")) return 0;
    const int C = 1 << TZ_PROFILE_CAR, B = 1 << TZ_PROFILE_BIKE;
    uint8_t mask = 0;
    if (parse_rvalue(fz_tag_get(t, n, "restriction"), kind, turn)) {
        mask = (uint8_t)(C | B);
        const char *ex = fz_tag_get(t, n, "except");
        if (ex) {
            if (strstr(ex, "motorcar") || strstr(ex, "motor_vehicle") || strstr(ex, "vehicle")) mask &= (uint8_t)~C;
            if (strstr(ex, "bicycle") || strstr(ex, "vehicle")) mask &= (uint8_t)~B;
        }
    } else if (parse_rvalue(fz_tag_get(t, n, "restriction:motorcar"), kind, turn) ||
               parse_rvalue(fz_tag_get(t, n, "restriction:motor_vehicle"), kind, turn)) {
        mask = (uint8_t)C;
    } else if (parse_rvalue(fz_tag_get(t, n, "restriction:vehicle"), kind, turn)) {
        mask = (uint8_t)(C | B);
    } else if (parse_rvalue(fz_tag_get(t, n, "restriction:bicycle"), kind, turn)) {
        mask = (uint8_t)B;
    }
    return mask;
}

/* ------------------------------------------------------------- search */

int fz_place_type(const char *v)
{
    if (!v) return -1;
    if (eq(v, "city")) return TZ_PLACE_CITY;
    if (eq(v, "town")) return TZ_PLACE_TOWN;
    if (eq(v, "village")) return TZ_PLACE_VILLAGE;
    if (eq(v, "hamlet")) return TZ_PLACE_HAMLET;
    if (eq(v, "suburb") || eq(v, "quarter") || eq(v, "borough")) return TZ_PLACE_SUBURB;
    if (eq(v, "neighbourhood")) return TZ_PLACE_NEIGHBOURHOOD;
    if (eq(v, "locality") || eq(v, "isolated_dwelling")) return TZ_PLACE_LOCALITY;
    if (eq(v, "island") || eq(v, "islet") || eq(v, "square")) return TZ_PLACE_OTHER;
    return -1;
}

static void humanize(const char *v, char *out, size_t cap)
{
    size_t i = 0;
    for (; v[i] && i + 1 < cap; i++) out[i] = (v[i] == '_' || v[i] == ';') ? ' ' : v[i];
    out[i] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

typedef struct { const char *v; int group; uint16_t bonus; } poimap;

static const poimap AMENITY[] = {
    { "restaurant", TZ_POI_FOOD, 0 }, { "cafe", TZ_POI_FOOD, 0 }, { "fast_food", TZ_POI_FOOD, 0 },
    { "bar", TZ_POI_FOOD, 0 }, { "pub", TZ_POI_FOOD, 0 }, { "food_court", TZ_POI_FOOD, 0 },
    { "ice_cream", TZ_POI_FOOD, 0 }, { "biergarten", TZ_POI_FOOD, 0 }, { "nightclub", TZ_POI_FOOD, 0 },
    { "fuel", TZ_POI_FUEL, 500 }, { "charging_station", TZ_POI_FUEL, 0 },
    { "parking", TZ_POI_PARKING, 0 }, { "bicycle_parking", TZ_POI_PARKING, 0 },
    { "motorcycle_parking", TZ_POI_PARKING, 0 }, { "parking_entrance", TZ_POI_PARKING, 0 },
    { "hospital", TZ_POI_HEALTH, 4000 }, { "clinic", TZ_POI_HEALTH, 1000 }, { "pharmacy", TZ_POI_HEALTH, 500 },
    { "doctors", TZ_POI_HEALTH, 0 }, { "dentist", TZ_POI_HEALTH, 0 }, { "veterinary", TZ_POI_HEALTH, 0 },
    { "school", TZ_POI_EDUCATION, 500 }, { "university", TZ_POI_EDUCATION, 3000 },
    { "college", TZ_POI_EDUCATION, 1500 }, { "kindergarten", TZ_POI_EDUCATION, 0 },
    { "library", TZ_POI_CULTURE, 1000 }, { "theatre", TZ_POI_CULTURE, 2000 }, { "cinema", TZ_POI_CULTURE, 1000 },
    { "arts_centre", TZ_POI_CULTURE, 1000 }, { "place_of_worship", TZ_POI_CULTURE, 500 },
    { "community_centre", TZ_POI_CULTURE, 0 },
    { "bus_station", TZ_POI_TRANSPORT, 3000 }, { "ferry_terminal", TZ_POI_TRANSPORT, 3000 },
    { "taxi", TZ_POI_TRANSPORT, 0 }, { "car_rental", TZ_POI_TRANSPORT, 0 },
    { "bicycle_rental", TZ_POI_TRANSPORT, 0 }, { "car_sharing", TZ_POI_TRANSPORT, 0 },
    { "townhall", TZ_POI_SERVICE, 2000 }, { "police", TZ_POI_SERVICE, 1000 },
    { "post_office", TZ_POI_SERVICE, 500 }, { "bank", TZ_POI_SERVICE, 0 }, { "atm", TZ_POI_SERVICE, 0 },
    { "marketplace", TZ_POI_SHOP, 1000 },
    { NULL, 0, 0 }
};

static const char *const AMENITY_SKIP[] = { "bench", "waste_basket", "waste_disposal", "recycling",
                                            "vending_machine", "parking_space", "post_box", "grit_bin",
                                            "hunting_stand", "letter_box", "clock", NULL };

int fz_poi_parse(const fz_tag *t, int n, fz_poi_info *o)
{
    memset(o, 0, sizeof *o);
    const char *v;
    int ok = 0;
    if ((v = fz_tag_get(t, n, "amenity")) && !in_list(v, AMENITY_SKIP)) {
        o->group = TZ_POI_OTHER;
        for (const poimap *m = AMENITY; m->v; m++)
            if (strcmp(v, m->v) == 0) { o->group = m->group; o->rank_bonus = m->bonus; break; }
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "shop"))) {
        o->group = TZ_POI_SHOP;
        humanize(eq(v, "yes") ? "shop" : v, o->label, sizeof o->label);
        if (eq(v, "mall") || eq(v, "department_store")) o->rank_bonus = 2000;
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "tourism")) && !eq(v, "information") && !eq(v, "yes")) {
        static const char *const lodging[] = { "hotel", "hostel", "guest_house", "motel", "apartment",
                                               "chalet", "camp_site", "caravan_site", "alpine_hut", NULL };
        o->group = in_list(v, lodging) ? TZ_POI_LODGING : TZ_POI_CULTURE;
        if (eq(v, "museum") || eq(v, "attraction") || eq(v, "zoo") || eq(v, "theme_park")) o->rank_bonus = 3000;
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "railway")) &&
               (eq(v, "station") || eq(v, "halt") || eq(v, "tram_stop"))) {
        o->group = TZ_POI_TRANSPORT;
        o->rank_bonus = eq(v, "station") ? 6000 : eq(v, "halt") ? 3000 : 0;
        const char *st = fz_tag_get(t, n, "station");
        humanize(eq(v, "station") && eq(st, "subway") ? "metro_station" :
                 eq(v, "station") ? "railway_station" : v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "public_transport")) && eq(v, "station")) {
        o->group = TZ_POI_TRANSPORT;
        o->rank_bonus = 5000;
        humanize("station", o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "aeroway")) && (eq(v, "aerodrome") || eq(v, "terminal"))) {
        o->group = TZ_POI_TRANSPORT;
        o->rank_bonus = eq(v, "aerodrome") ? 8000 : 4000;
        humanize(eq(v, "aerodrome") ? "airport" : "airport_terminal", o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "historic")) && !eq(v, "yes")) {
        o->group = TZ_POI_CULTURE;
        o->rank_bonus = (eq(v, "castle") || eq(v, "monument")) ? 2000 : 0;
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "leisure"))) {
        static const char *const nature[] = { "park", "nature_reserve", "garden", "beach_resort", NULL };
        static const char *const skip[] = { "pitch", "picnic_table", "firepit", "slipway", "track",
                                            "outdoor_seating", "swimming_area", NULL };
        if (!in_list(v, skip)) {
            o->group = in_list(v, nature) ? TZ_POI_NATURE : TZ_POI_OTHER;
            if (eq(v, "stadium") || eq(v, "park")) o->rank_bonus = 1500;
            humanize(v, o->label, sizeof o->label);
            ok = 1;
        }
    } else if ((v = fz_tag_get(t, n, "natural")) &&
               (eq(v, "peak") || eq(v, "volcano") || eq(v, "cave_entrance") || eq(v, "spring") ||
                eq(v, "beach") || eq(v, "glacier") || eq(v, "saddle"))) {
        o->group = TZ_POI_NATURE;
        o->rank_bonus = eq(v, "peak") ? 1000 : 0;
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "healthcare")) && !eq(v, "yes")) {
        o->group = TZ_POI_HEALTH;
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "office"))) {
        o->group = TZ_POI_SERVICE;
        humanize(eq(v, "yes") ? "office" : v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "craft"))) {
        o->group = TZ_POI_SERVICE;
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "highway")) && eq(v, "bus_stop")) {
        o->group = TZ_POI_TRANSPORT;
        humanize("bus_stop", o->label, sizeof o->label);
        ok = 1;
    } else if ((v = fz_tag_get(t, n, "man_made")) && (eq(v, "lighthouse") || eq(v, "tower") || eq(v, "bridge"))) {
        o->group = TZ_POI_CULTURE;
        humanize(v, o->label, sizeof o->label);
        ok = 1;
    }
    if (ok && (fz_tag_get(t, n, "wikidata") || fz_tag_get(t, n, "wikipedia"))) o->rank_bonus += 2000;
    return ok;
}
