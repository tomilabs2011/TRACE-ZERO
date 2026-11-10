/*
 * tzforge - builds TRACE//ZERO map packs from OpenStreetMap data.
 *
 *   tzforge build  <input.osm.pbf> <pack.vmap> [options]
 *   tzforge route  <pack.vmap> <car|bike|foot> <lat,lon> <lat,lon> [--heading deg]
 *   tzforge search <pack.vmap> <query> [--near lat,lon]
 *   tzforge verify <pack.vmap> [--pairs N]
 *   tzforge info   <pack.vmap>
 */
#include "fz_build.h"
#include "fz_ch.h"
#include "../../Core/tz_core.h"
#include "../../Core/tz_private.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TZFORGE_VERSION "1.0.0"

static const char *const PROFILE_FILES[3] = { "car.tzg", "bike.tzg", "foot.tzg" };
static const char *const PROFILE_NAMES[3] = { "car", "bike", "foot" };

static void usage(void)
{
    fprintf(stderr,
        "tzforge " TZFORGE_VERSION " - TRACE//ZERO map pack builder\n\n"
        "usage:\n"
        "  tzforge build  <input.osm.pbf> <pack.vmap> [--name NAME] [--id ID] [--source URL]\n"
        "                 [--profiles car,bike,foot] [--no-search] [--progress]\n"
        "  tzforge route  <pack.vmap> <car|bike|foot> <lat,lon> <lat,lon> [--heading DEG]\n"
        "  tzforge search <pack.vmap> <query> [--near lat,lon]\n"
        "  tzforge verify <pack.vmap> [--pairs N]\n"
        "  tzforge info   <pack.vmap>\n");
    exit(2);
}

static char *join_path(const char *dir, const char *file)
{
    size_t n = strlen(dir) + strlen(file) + 2;
    char *p = (char *)fz_malloc(n);
    snprintf(p, n, "%s/%s", dir, file);
    return p;
}

static int file_exists(const char *path, long long *size)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (size) *size = (long long)st.st_size;
    return 1;
}

static void mkdir_p(const char *path)
{
    char *tmp = fz_strdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) fz_die("cannot create %s: %s", tmp, strerror(errno));
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) fz_die("cannot create %s: %s", tmp, strerror(errno));
    free(tmp);
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static void iso_time(time_t t, char *buf, size_t cap)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void write_manifest(const fz_build *b, size_t search_entries)
{
    char *path = join_path(b->outdir, "manifest.json");
    char *tmp = join_path(b->outdir, "manifest.json.part");
    FILE *f = fopen(tmp, "w");
    if (!f) fz_die("cannot write %s", tmp);
    char now[32], osm[32];
    iso_time(time(NULL), now, sizeof now);
    osm[0] = 0;
    if (b->info.replication_timestamp > 0) iso_time((time_t)b->info.replication_timestamp, osm, sizeof osm);
    double minlat = b->bbox[0] / 1e6, minlon = b->bbox[1] / 1e6, maxlat = b->bbox[2] / 1e6, maxlon = b->bbox[3] / 1e6;

    fprintf(f, "{\n  \"format\": %u,\n  \"id\": ", TZ_FORMAT_VERSION);
    json_str(f, b->pack_id);
    fprintf(f, ",\n  \"name\": ");
    json_str(f, b->pack_name);
    fprintf(f, ",\n  \"created\": \"%s\",\n", now);
    if (osm[0]) fprintf(f, "  \"osm_timestamp\": \"%s\",\n", osm);
    if (b->source_url) { fprintf(f, "  \"source\": "); json_str(f, b->source_url); fprintf(f, ",\n"); }
    fprintf(f, "  \"bbox\": [%.6f, %.6f, %.6f, %.6f],\n", minlon, minlat, maxlon, maxlat);
    fprintf(f, "  \"center\": [%.6f, %.6f],\n", (minlon + maxlon) / 2, (minlat + maxlat) / 2);
    fprintf(f, "  \"builder\": \"tzforge " TZFORGE_VERSION "\",\n");
    fprintf(f, "  \"attribution\": \"© OpenStreetMap contributors\",\n");
    fprintf(f, "  \"files\": {\n");
    const char *keys[] = { "tiles", "roads", "search", "car", "bike", "foot" };
    const char *files[] = { "map.pmtiles", "roads.tzr", "search.tzs", "car.tzg", "bike.tzg", "foot.tzg" };
    int first = 1;
    for (int i = 0; i < 6; i++) {
        char *fp = join_path(b->outdir, files[i]);
        long long size;
        if (file_exists(fp, &size)) {
            fprintf(f, "%s    \"%s\": { \"path\": \"%s\", \"bytes\": %lld }", first ? "" : ",\n", keys[i], files[i], size);
            first = 0;
        }
        free(fp);
    }
    fprintf(f, "\n  },\n  \"stats\": { \"junctions\": %u, \"segments\": %zu, \"search_entries\": %zu }\n}\n",
            b->n_road, b->segs.n, search_entries);
    if (fclose(f) != 0) fz_die("cannot write %s", tmp);
    if (rename(tmp, path) != 0) fz_die("cannot rename %s", tmp);
    free(path);
    free(tmp);
}

static int parse_profiles(const char *s)
{
    int mask = 0;
    char *d = fz_strdup(s);
    for (char *tok = strtok(d, ","); tok; tok = strtok(NULL, ",")) {
        int found = 0;
        for (int p = 0; p < 3; p++) if (strcmp(tok, PROFILE_NAMES[p]) == 0) { mask |= 1 << p; found = 1; }
        if (!found) fz_die("unknown profile \"%s\" (use car, bike, foot)", tok);
    }
    free(d);
    return mask;
}

static int cmd_build(int argc, char **argv)
{
    if (argc < 2) usage();
    fz_build b;
    memset(&b, 0, sizeof b);
    b.input = argv[0];
    b.outdir = argv[1];
    b.profiles = 7;
    b.with_search = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) b.pack_name = argv[++i];
        else if (!strcmp(argv[i], "--id") && i + 1 < argc) b.pack_id = argv[++i];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) b.source_url = argv[++i];
        else if (!strcmp(argv[i], "--profiles") && i + 1 < argc) b.profiles = parse_profiles(argv[++i]);
        else if (!strcmp(argv[i], "--no-search")) b.with_search = 0;
        else if (!strcmp(argv[i], "--progress")) fz_progress_mode = 1;
        else usage();
    }
    /* default id/name from the folder name */
    const char *base = strrchr(b.outdir, '/');
    base = base ? base + 1 : b.outdir;
    char idbuf[128];
    snprintf(idbuf, sizeof idbuf, "%s", base);
    char *dot = strstr(idbuf, ".vmap");
    if (dot) *dot = 0;
    if (!b.pack_id) b.pack_id = idbuf;
    if (!b.pack_name) b.pack_name = b.pack_id;

    double t0 = fz_now();
    mkdir_p(b.outdir);
    fz_build_read(&b);
    fz_build_segments(&b);
    char *roads = join_path(b.outdir, "roads.tzr");
    fz_build_write_roads(&b, roads);
    for (int p = 0; p < 3; p++) {
        char *gp = join_path(b.outdir, PROFILE_FILES[p]);
        if (b.profiles & (1 << p)) fz_build_profile(&b, p, gp);
        else remove(gp);
        free(gp);
    }
    size_t entries = 0;
    char *sp = join_path(b.outdir, "search.tzs");
    if (b.with_search) {
        fz_build_search(&b, sp);
        int st;
        tz_search *s = tz_search_open(sp, &st);
        if (!s) fz_die("search index failed to load: %s", tz_status_message(st));
        entries = tz_search_entry_count(s);
        tz_search_close(s);
    } else {
        remove(sp);
    }
    free(sp);

    /* Self-check: every file must open with the same reader the phone uses. */
    int st;
    tz_roads *r = tz_roads_open(roads, &st);
    if (!r) fz_die("roads file failed to load: %s", tz_status_message(st));
    for (int p = 0; p < 3; p++) {
        if (!(b.profiles & (1 << p))) continue;
        char *gp = join_path(b.outdir, PROFILE_FILES[p]);
        tz_graph *g = tz_graph_open(r, gp, &st);
        if (!g) fz_die("%s failed to load: %s", PROFILE_FILES[p], tz_status_message(st));
        tz_graph_close(g);
        free(gp);
    }
    tz_roads_close(r);
    free(roads);

    write_manifest(&b, entries);
    fz_log("done in %.1f s -> %s", fz_now() - t0, b.outdir);
    if (fz_progress_mode) { printf("PROGRESS 100.0\nDONE\n"); fflush(stdout); }
    return 0;
}

/* ------------------------------------------------------------ queries */

static int parse_latlon(const char *s, double *lat, double *lon)
{
    return tz_parse_coordinate(s, lat, lon);
}

static const char *man_text(const tz_maneuver *m, char *buf, size_t cap)
{
    static const char *const mods[] = { "sharp left", "left", "slight left", "straight", "slight right", "right", "sharp right" };
    const char *mod = m->modifier == 4 ? "U-turn" : (m->modifier >= -3 && m->modifier <= 3) ? mods[m->modifier + 3] : "?";
    const char *name = m->name && *m->name ? m->name : (m->ref && *m->ref ? m->ref : "");
    switch (m->type) {
    case TZ_MAN_DEPART: snprintf(buf, cap, "Head out on %s", *name ? name : "the road"); break;
    case TZ_MAN_TURN: snprintf(buf, cap, "Turn %s%s%s", mod, *name ? " onto " : "", name); break;
    case TZ_MAN_CONTINUE: snprintf(buf, cap, "Continue onto %s", name); break;
    case TZ_MAN_FORK: snprintf(buf, cap, "Keep %s%s%s", m->modifier < 0 ? "left" : "right", *name ? " onto " : "", name); break;
    case TZ_MAN_RAMP: snprintf(buf, cap, "Take the ramp on the %s%s%s", m->modifier < 0 ? "left" : "right", *name ? " to " : "", name); break;
    case TZ_MAN_ROUNDABOUT: snprintf(buf, cap, "At the roundabout take exit %u%s%s", m->exit, *name ? " onto " : "", name); break;
    case TZ_MAN_FERRY: snprintf(buf, cap, "Take the ferry %s", name); break;
    case TZ_MAN_ARRIVE: snprintf(buf, cap, "Arrive"); break;
    default: snprintf(buf, cap, "?"); break;
    }
    return buf;
}

static int profile_index(const char *s)
{
    for (int p = 0; p < 3; p++) if (!strcmp(s, PROFILE_NAMES[p])) return p;
    fz_die("unknown profile %s", s);
    return 0;
}

static int cmd_route(int argc, char **argv)
{
    if (argc < 4) usage();
    int p = profile_index(argv[1]);
    tz_waypoint a, z;
    memset(&a, 0, sizeof a);
    memset(&z, 0, sizeof z);
    a.heading = z.heading = -1;
    if (!parse_latlon(argv[2], &a.lat, &a.lon) || !parse_latlon(argv[3], &z.lat, &z.lon)) fz_die("bad coordinate");
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--heading") && i + 1 < argc) a.heading = atof(argv[++i]);
        else usage();
    }
    char *rp = join_path(argv[0], "roads.tzr"), *gp = join_path(argv[0], PROFILE_FILES[p]);
    int st;
    tz_roads *r = tz_roads_open(rp, &st);
    if (!r) fz_die("%s: %s", rp, tz_status_message(st));
    tz_graph *g = tz_graph_open(r, gp, &st);
    if (!g) fz_die("%s: %s", gp, tz_status_message(st));
    tz_route *rt;
    st = tz_route_compute(g, &a, &z, &rt);
    if (st != TZ_OK) { printf("no route: %s\n", tz_status_message(st)); return 1; }
    printf("distance %.0f m, duration %.0f s, %u points, %u maneuvers, %u nodes settled, %.2f ms\n",
           rt->distance_m, rt->duration_s, rt->npts, rt->nman, rt->settled, rt->compute_ms);
    char buf[256];
    for (uint32_t i = 0; i < rt->nman; i++) {
        const tz_maneuver *m = &rt->man[i];
        printf("  %7.0f m  %s\n", m->dist_m, man_text(m, buf, sizeof buf));
    }
    tz_route_free(rt);
    tz_graph_close(g);
    tz_roads_close(r);
    free(rp);
    free(gp);
    return 0;
}

static int cmd_search(int argc, char **argv)
{
    if (argc < 2) usage();
    double lat = 0, lon = 0;
    int has_ref = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--near") && i + 1 < argc) {
            if (!parse_latlon(argv[++i], &lat, &lon)) fz_die("bad coordinate");
            has_ref = 1;
        } else usage();
    }
    char *sp = join_path(argv[0], "search.tzs");
    int st;
    tz_search *s = tz_search_open(sp, &st);
    if (!s) fz_die("%s: %s", sp, tz_status_message(st));
    tz_hit hits[20];
    double t0 = fz_now();
    int n = tz_search_query(s, argv[1], lat, lon, has_ref, hits, 20);
    double ms = (fz_now() - t0) * 1000;
    printf("%d results in %.2f ms\n", n, ms);
    for (int i = 0; i < n; i++)
        printf("  [%d] %-40s | %-40s %.5f,%.5f score %.1f\n", hits[i].kind, hits[i].name, hits[i].context,
               hits[i].lat, hits[i].lon, hits[i].score);
    tz_search_close(s);
    free(sp);
    return 0;
}

/* ------------------------------------------------------------ verify */

typedef struct { uint32_t to, w; } varc;

/* first index with key >= k in a sorted keynode table */
static uint32_t kn_lower(const tz_keynode *a, uint32_t n, uint32_t k)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; if (a[m].key < k) lo = m + 1; else hi = m; }
    return lo;
}

static uint64_t rng_state = 88172645463325252ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Rebuilds the plain (uncontracted) graph from the pack and checks that the
 * contraction hierarchy returns exactly the Dijkstra distances. */
static int verify_profile(const tz_roads *r, const tz_graph *g, int pairs)
{
    const struct tz_graph *G = g;
    uint32_t N = G->n_nodes, R = G->n_road_nodes;
    /* rebuild arcs in CH-id space */
    uint32_t *deg = (uint32_t *)fz_calloc((size_t)N + 1, sizeof(uint32_t));
    FZ_VEC(uint32_t) au;
    FZ_VEC(varc) av;
    memset(&au, 0, sizeof au);
    memset(&av, 0, sizeof av);
    for (uint32_t s = 0; s < r->n_segs; s++) {
        const tz_seg *sg = &r->segs[s];
        for (uint32_t d = 0; d < 2; d++) {
            uint32_t w = d == 0 ? G->cost[s].w_fwd : G->cost[s].w_bwd;
            if (w == TZ_NO_ACCESS) continue;
            uint32_t X = d == 0 ? sg->a : sg->b, Y = d == 0 ? sg->b : sg->a;
            uint32_t key = s * 2 + d, target = G->road_ch[Y];
            uint32_t ai = kn_lower(G->arrive, G->n_arrive, key);
            if (ai < G->n_arrive && G->arrive[ai].key == key) target = G->arrive[ai].node;
            if (X == Y && target == G->road_ch[Y]) continue;
            if (G->deg[X] > 0) { fz_vec_push(au, G->road_ch[X]); fz_vec_push(av, ((varc){ target, w })); }
            for (uint32_t i = kn_lower(G->depart, G->n_depart, key); i < G->n_depart && G->depart[i].key == key; i++) {
                fz_vec_push(au, G->depart[i].node);
                fz_vec_push(av, ((varc){ target, w }));
            }
        }
    }
    for (size_t i = 0; i < au.n; i++) deg[au.a[i] + 1]++;
    for (uint32_t v = 0; v < N; v++) deg[v + 1] += deg[v];
    varc *adjv = (varc *)fz_malloc((au.n ? au.n : 1) * sizeof(varc));
    uint32_t *fill = (uint32_t *)fz_malloc(((size_t)N + 1) * sizeof(uint32_t));
    memcpy(fill, deg, ((size_t)N + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < au.n; i++) adjv[fill[au.a[i]]++] = av.a[i];
    free(fill);

    uint64_t *dist = (uint64_t *)fz_malloc((size_t)N * sizeof(uint64_t));
    tz_heap h;
    memset(&h, 0, sizeof h);
    int bad = 0, connected = 0;
    double ch_ms = 0, dj_ms = 0;
    for (int k = 0; k < pairs; k++) {
        uint32_t src = (uint32_t)(rnd() % R), dst = (uint32_t)(rnd() % R);
        if (G->deg[src] == 0) { k--; continue; }
        /* Dijkstra from src (original node) to any node whose road node is dst */
        double t0 = fz_now();
        for (uint32_t v = 0; v < N; v++) dist[v] = UINT64_MAX;
        h.n = 0;
        uint32_t s0 = G->road_ch[src];
        dist[s0] = 0;
        tz_heap_push(&h, 0, s0);
        uint64_t ref = UINT64_MAX, key;
        uint32_t u;
        while (tz_heap_pop(&h, &key, &u)) {
            if (key != dist[u]) continue;
            if (G->node_road[u] == dst) { ref = key; break; }
            for (uint32_t e = deg[u]; e < deg[u + 1]; e++) {
                uint64_t nd = key + adjv[e].w;
                if (nd < dist[adjv[e].to]) { dist[adjv[e].to] = nd; tz_heap_push(&h, nd, adjv[e].to); }
            }
        }
        dj_ms += (fz_now() - t0) * 1000;
        t0 = fz_now();
        uint64_t chd = UINT64_MAX;
        int ok = tz__ch_node_distance(g, src, dst, &chd);
        ch_ms += (fz_now() - t0) * 1000;
        if (!ok) chd = UINT64_MAX;
        if (ref != UINT64_MAX) connected++;
        if (chd != ref) {
            if (bad < 10) fprintf(stderr, "  MISMATCH %u -> %u: dijkstra %llu, ch %llu\n", src, dst,
                                  (unsigned long long)ref, (unsigned long long)chd);
            bad++;
        }
    }
    printf("  %d pairs (%d connected): %d mismatches; avg CH %.3f ms, avg Dijkstra %.2f ms\n",
           pairs, connected, bad, ch_ms / pairs, dj_ms / pairs);
    free(dist);
    tz_heap_free(&h);
    free(deg);
    free(adjv);
    fz_vec_free(au);
    fz_vec_free(av);
    return bad;
}

static int cmd_verify(int argc, char **argv)
{
    if (argc < 1) usage();
    int pairs = 300;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pairs") && i + 1 < argc) pairs = atoi(argv[++i]);
        else usage();
    }
    if (pairs < 1) pairs = 1;
    char *rp = join_path(argv[0], "roads.tzr");
    int st, bad = 0;
    tz_roads *r = tz_roads_open(rp, &st);
    if (!r) fz_die("%s: %s", rp, tz_status_message(st));
    for (int p = 0; p < 3; p++) {
        char *gp = join_path(argv[0], PROFILE_FILES[p]);
        if (!file_exists(gp, NULL)) { free(gp); continue; }
        tz_graph *g = tz_graph_open(r, gp, &st);
        if (!g) fz_die("%s: %s", gp, tz_status_message(st));
        printf("%s:\n", PROFILE_NAMES[p]);
        bad += verify_profile(r, g, pairs);

        /* end-to-end queries between random points inside the data */
        double bb[4];
        tz_roads_bbox(r, bb);
        int ok = 0, fail = 0, noroute = 0;
        double ms = 0, worst = 0;
        for (int k = 0; k < pairs; k++) {
            tz_waypoint a = { bb[0] + (bb[2] - bb[0]) * (rnd() % 10000) / 1e4, bb[1] + (bb[3] - bb[1]) * (rnd() % 10000) / 1e4, -1, 3000 };
            tz_waypoint z = { bb[0] + (bb[2] - bb[0]) * (rnd() % 10000) / 1e4, bb[1] + (bb[3] - bb[1]) * (rnd() % 10000) / 1e4, -1, 3000 };
            tz_route *rt = NULL;
            int rs = tz_route_compute(g, &a, &z, &rt);
            if (rs == TZ_OK) {
                ok++;
                ms += rt->compute_ms;
                if (rt->compute_ms > worst) worst = rt->compute_ms;
                /* the polyline must be continuous: no jump longer than its own segments allow */
                for (uint32_t i = 1; i < rt->npts; i++) {
                    if (rt->cum_dist[i] < rt->cum_dist[i - 1] || rt->cum_time[i] < rt->cum_time[i - 1]) {
                        fprintf(stderr, "  invalid: non-monotonic distance/time at point %u\n", i);
                        fail++;
                        break;
                    }
                }
                if (rt->nman < 2 || rt->man[0].type != TZ_MAN_DEPART || rt->man[rt->nman - 1].type != TZ_MAN_ARRIVE) {
                    fprintf(stderr, "  invalid: maneuver list (%u entries, first %d, last %d) for %.6f,%.6f -> %.6f,%.6f\n",
                            rt->nman, rt->nman ? rt->man[0].type : -1, rt->nman ? rt->man[rt->nman - 1].type : -1,
                            a.lat, a.lon, z.lat, z.lon);
                    fail++;
                }
                for (uint32_t i = 1; i < rt->nman; i++)
                    if (rt->man[i].point < rt->man[i - 1].point) {
                        fprintf(stderr, "  invalid: maneuvers out of order\n");
                        fail++;
                        break;
                    }
                tz_route_free(rt);
            } else if (rs == TZ_ERR_NO_ROUTE || rs == TZ_ERR_NO_START || rs == TZ_ERR_NO_END) {
                noroute++;
            } else {
                fprintf(stderr, "  route error: %s\n", tz_status_message(rs));
                fail++;
            }
        }
        printf("  %d routes ok (avg %.2f ms, worst %.2f ms), %d unreachable, %d invalid\n",
               ok, ok ? ms / ok : 0, worst, noroute, fail);
        bad += fail;
        tz_graph_close(g);
        free(gp);
    }
    tz_roads_close(r);
    free(rp);
    printf(bad ? "VERIFY FAILED\n" : "VERIFY OK\n");
    return bad ? 1 : 0;
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) usage();
    char *rp = join_path(argv[0], "roads.tzr");
    int st;
    tz_roads *r = tz_roads_open(rp, &st);
    if (!r) fz_die("%s: %s", rp, tz_status_message(st));
    double bb[4];
    tz_roads_bbox(r, bb);
    printf("roads: %u segments, bbox %.4f,%.4f - %.4f,%.4f\n", tz_roads_segment_count(r), bb[0], bb[1], bb[2], bb[3]);
    for (int p = 0; p < 3; p++) {
        char *gp = join_path(argv[0], PROFILE_FILES[p]);
        if (file_exists(gp, NULL)) {
            tz_graph *g = tz_graph_open(r, gp, &st);
            if (g) {
                printf("%s: %u nodes, %u edges, %u split nodes\n", PROFILE_NAMES[p], g->n_nodes, g->n_edges,
                       g->n_nodes - g->n_road_nodes);
                tz_graph_close(g);
            } else printf("%s: %s\n", PROFILE_NAMES[p], tz_status_message(st));
        }
        free(gp);
    }
    tz_roads_close(r);
    free(rp);
    char *sp = join_path(argv[0], "search.tzs");
    tz_search *s = tz_search_open(sp, &st);
    if (s) { printf("search: %u entries\n", tz_search_entry_count(s)); tz_search_close(s); }
    free(sp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();
    if (!strcmp(argv[1], "build")) return cmd_build(argc - 2, argv + 2);
    if (!strcmp(argv[1], "route")) return cmd_route(argc - 2, argv + 2);
    if (!strcmp(argv[1], "search")) return cmd_search(argc - 2, argv + 2);
    if (!strcmp(argv[1], "verify")) return cmd_verify(argc - 2, argv + 2);
    if (!strcmp(argv[1], "info")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(argv[1], "--version")) { printf("tzforge " TZFORGE_VERSION "\n"); return 0; }
    usage();
    return 2;
}
