/*
 * Parse coordinates typed by a person (decimal degrees, degrees+minutes,
 * degrees+minutes+seconds, with or without hemisphere letters).
 */
#include "tz_internal.h"

#include <math.h>
#include <string.h>

typedef enum { T_NUM, T_HEMI, T_SEP } tok_type;

typedef struct {
    tok_type type;
    double   val;       /* T_NUM */
    int      neg;       /* T_NUM had a leading minus */
    int      frac;      /* T_NUM had a decimal point */
    char     hemi;      /* T_HEMI: N S E W */
} ctok;

#define MAX_TOK 16

static int lex(const char *s, ctok *t, int *nt)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp((const char *)p, "geo:", 4) == 0) p += 4;
    while (*p) {
        unsigned char c = *p;
        if (n >= MAX_TOK) return 0;
        if ((c >= '0' && c <= '9') || c == '.' ||
            ((c == '-' || c == '+') && ((p[1] >= '0' && p[1] <= '9') || p[1] == '.'))) {
            int neg = 0;
            if (c == '-' || c == '+') { neg = (c == '-'); p++; }
            double v = 0.0;
            int digits = 0, frac = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; digits++; }
            if (*p == '.') {
                p++;
                frac = 1;
                double scale = 0.1;
                while (*p >= '0' && *p <= '9') { v += (*p - '0') * scale; scale *= 0.1; p++; digits++; }
            }
            if (!digits) return 0;
            t[n].type = T_NUM; t[n].val = v; t[n].neg = neg; t[n].frac = frac; t[n].hemi = 0;
            n++;
            continue;
        }
        char up = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        if (up == 'N' || up == 'S' || up == 'E' || up == 'W') {
            /* A hemisphere letter must stand alone, not be part of a word. */
            unsigned char nx = p[1];
            if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z')) return 0;
            t[n].type = T_HEMI; t[n].hemi = up; n++;
            p++;
            continue;
        }
        if (c == ',' || c == ';' || c == '/') {
            t[n].type = T_SEP; n++;
            p++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\'' || c == '"' || c == ':') { p++; continue; }
        /* UTF-8 degree, ordinal, prime, double prime, curly quotes */
        if (c == 0xC2 && (p[1] == 0xB0 || p[1] == 0xBA)) { p += 2; continue; }
        if (c == 0xE2 && p[1] == 0x80 && (p[2] == 0xB2 || p[2] == 0xB3 || p[2] == 0x99 || p[2] == 0x9D)) { p += 3; continue; }
        return 0; /* any other character: not a coordinate */
    }
    *nt = n;
    return 1;
}

typedef struct { double nums[3]; int nnum; int neg; int frac_mask; char hemi; } group;

static int group_value(const group *g, double *out)
{
    if (g->nnum < 1 || g->nnum > 3) return 0;
    double deg = g->nums[0];
    if (g->nnum >= 2) {
        /* degrees and minutes must be whole when followed by smaller units */
        if (g->frac_mask & 1) return 0;
        if (g->nums[1] >= 60.0) return 0;
        deg += g->nums[1] / 60.0;
    }
    if (g->nnum == 3) {
        if (g->frac_mask & 2) return 0;
        if (g->nums[2] >= 60.0) return 0;
        deg += g->nums[2] / 3600.0;
    }
    if (g->neg) deg = -deg;
    if (g->hemi == 'S' || g->hemi == 'W') {
        if (g->neg) return 0; /* "-47 S" is contradictory */
        deg = -deg;
    }
    *out = deg;
    return 1;
}

int tz_parse_coordinate(const char *text, double *lat, double *lon)
{
    if (!text) return 0;
    ctok t[MAX_TOK];
    int nt = 0;
    if (!lex(text, t, &nt) || nt == 0) return 0;

    int prefix = (t[0].type == T_HEMI);
    group g[4];
    memset(g, 0, sizeof g);
    int ng = 0, open = 0, has_hemi = 0, any_frac = 0, overflow = 0;
    int total_nums = 0;

    for (int i = 0; i < nt; i++) {
        if (t[i].type == T_SEP) {
            if (open) { ng++; open = 0; }
            continue;
        }
        if (t[i].type == T_HEMI) {
            has_hemi = 1;
            if (prefix) {
                if (open) { ng++; open = 0; }
                if (ng >= 4) return 0;
                g[ng].hemi = t[i].hemi;
                open = 1;
            } else {
                if (!open) return 0; /* suffix letter without numbers */
                g[ng].hemi = t[i].hemi;
                ng++;
                open = 0;
            }
            continue;
        }
        /* number */
        if (ng >= 4) return 0;
        group *cur = &g[ng];
        if (cur->nnum >= 3) { overflow = 1; break; }
        if (cur->nnum > 0 && t[i].neg) return 0; /* sign only on degrees */
        if (cur->nnum == 0) cur->neg = t[i].neg;
        if (t[i].frac) { cur->frac_mask |= 1 << cur->nnum; any_frac = 1; }
        cur->nums[cur->nnum++] = t[i].val;
        total_nums++;
        open = 1;
    }
    if (overflow) {
        ng = 0; /* handled by the run-of-numbers fallback below */
        for (int i = 0; i < nt; i++) if (t[i].type == T_NUM && t[i].frac) any_frac = 1;
        total_nums = 0;
        for (int i = 0; i < nt; i++) if (t[i].type == T_NUM) total_nums++;
    } else if (open) {
        ng++;
    }

    /* One group with 2, 4 or 6 numbers and no letters: split it in half. */
    if (ng == 1 && !has_hemi && (g[0].nnum == 2)) {
        group a = g[0];
        memset(g, 0, sizeof g);
        g[0].nums[0] = a.nums[0]; g[0].nnum = 1; g[0].neg = a.neg; g[0].frac_mask = a.frac_mask & 1;
        g[1].nums[0] = a.nums[1]; g[1].nnum = 1; g[1].frac_mask = (a.frac_mask >> 1) & 1;
        ng = 2;
    }
    if (ng != 2) {
        /* "47 29 52 19 2 24": single run of 4 or 6 numbers. Re-lex the raw
         * numbers since a group holds at most three. */
        if (has_hemi) return 0;
        double nums[6];
        int negs[6], fr[6], k = 0;
        for (int i = 0; i < nt; i++) {
            if (t[i].type != T_NUM) continue;
            if (k >= 6) return 0;
            nums[k] = t[i].val; negs[k] = t[i].neg; fr[k] = t[i].frac; k++;
        }
        if (k != 4 && k != 6) return 0;
        int h = k / 2;
        memset(g, 0, sizeof g);
        for (int j = 0; j < 2; j++) {
            for (int m = 0; m < h; m++) {
                int idx = j * h + m;
                if (m > 0 && negs[idx]) return 0;
                g[j].nums[m] = nums[idx];
                if (fr[idx]) g[j].frac_mask |= 1 << m;
            }
            g[j].nnum = h;
            g[j].neg = negs[j * h];
        }
        ng = 2;
    }

    /* Two bare integers ("12 34") are far more likely a house number search. */
    if (!has_hemi && !any_frac && total_nums == 2) return 0;

    double v0, v1;
    if (!group_value(&g[0], &v0) || !group_value(&g[1], &v1)) return 0;

    int lat_first = 1;
    int h0 = g[0].hemi, h1 = g[1].hemi;
    int h0_lon = (h0 == 'E' || h0 == 'W'), h1_lat = (h1 == 'N' || h1 == 'S');
    int h0_lat = (h0 == 'N' || h0 == 'S'), h1_lon = (h1 == 'E' || h1 == 'W');
    if (h0_lon || h1_lat) lat_first = 0;
    if ((h0_lat && h1_lat) || (h0_lon && h1_lon)) return 0;
    if (lat_first && (h0_lon || h1_lat)) return 0;

    double la = lat_first ? v0 : v1, lo = lat_first ? v1 : v0;
    if (!(la >= -90.0 && la <= 90.0 && lo >= -180.0 && lo <= 180.0)) return 0;
    if (isnan(la) || isnan(lo)) return 0;
    *lat = la;
    *lon = lo;
    return 1;
}
