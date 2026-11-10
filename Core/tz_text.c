/*
 * Search-text normalisation.
 *
 * The builder normalises every indexed name with this function and the app
 * normalises what the user types with the very same function, so matching is
 * a plain byte comparison afterwards. The rules are deliberately simple and
 * table-driven (no ICU, no locale): lower-case, strip diacritics for Latin,
 * Greek and Cyrillic, turn punctuation into spaces and split digits from
 * letters so "12/a", "12a" and "12 a" all become "12 a".
 */
#include "tz_internal.h"

#include <string.h>

/* Latin Extended-A, U+0100..U+017F: base letter per code point.
 * '1' = "ij", '2' = "oe". */
static const char LATIN_EXT_A[129] =
    "aaaaaa"          /* 0100-0105 */
    "cccccccc"        /* 0106-010D */
    "dddd"            /* 010E-0111 */
    "eeeeeeeeee"      /* 0112-011B */
    "gggggggg"        /* 011C-0123 */
    "hhhh"            /* 0124-0127 */
    "iiiiiiiiii"      /* 0128-0131 */
    "11"              /* 0132-0133 */
    "jj"              /* 0134-0135 */
    "kkk"             /* 0136-0138 */
    "llllllllll"      /* 0139-0142 */
    "nnnnnnnnn"       /* 0143-014B */
    "oooooo"          /* 014C-0151 */
    "22"              /* 0152-0153 */
    "rrrrrr"          /* 0154-0159 */
    "ssssssss"        /* 015A-0161 */
    "tttttt"          /* 0162-0167 */
    "uuuuuuuuuuuu"    /* 0168-0173 */
    "ww"              /* 0174-0175 */
    "yyy"             /* 0176-0178 */
    "zzzzzz"          /* 0179-017E */
    "s";              /* 017F */

/* Latin Extended Additional, U+1E00..U+1E95 (alphabetical pairs). */
static const char LATIN_EXT_ADD[151] =
    "aa" "bbbbbb" "cc" "dddddddddd" "eeeeeeeeee" "ff" "gg" "hhhhhhhhhh"
    "iiii" "kkkkkk" "llllllll" "mmmmmm" "nnnnnnnn" "oooooooo" "pppp"
    "rrrrrrrr" "ssssssssss" "tttttttt" "uuuuuuuuuu" "vvvv" "wwwwwwwwww"
    "xxxx" "yy" "zzzzzz";

/* Latin-1 Supplement U+00C0..U+00FF. '1'="ae", '2'="th", '3'="ss", ' '=separator */
static const char LATIN1[65] =
    "aaaaaa1ceeeeiiii"   /* C0-CF */
    "dnooooo ouuuuy23"   /* D0-DF */
    "aaaaaa1ceeeeiiii"   /* E0-EF */
    "dnooooo ouuuuy2y";  /* F0-FF */

/* Result of folding one code point. */
typedef enum { F_SEP, F_DROP, F_TEXT } fold_kind;

static fold_kind fold_cp(uint32_t c, uint32_t *out, int *nout)
{
    *nout = 0;
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') { out[(*nout)++] = c + 32; return F_TEXT; }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[(*nout)++] = c; return F_TEXT; }
        return F_SEP;
    }
    if (c < 0xC0) return F_SEP; /* NBSP, Latin-1 punctuation, symbols */
    if (c <= 0xFF) {
        char b = LATIN1[c - 0xC0];
        switch (b) {
        case ' ': return F_SEP;
        case '1': out[0] = 'a'; out[1] = 'e'; *nout = 2; return F_TEXT;
        case '2': out[0] = 't'; out[1] = 'h'; *nout = 2; return F_TEXT;
        case '3': out[0] = 's'; out[1] = 's'; *nout = 2; return F_TEXT;
        default: out[0] = (uint32_t)b; *nout = 1; return F_TEXT;
        }
    }
    if (c >= 0x100 && c <= 0x17F) {
        char b = LATIN_EXT_A[c - 0x100];
        if (b == '1') { out[0] = 'i'; out[1] = 'j'; *nout = 2; return F_TEXT; }
        if (b == '2') { out[0] = 'o'; out[1] = 'e'; *nout = 2; return F_TEXT; }
        out[0] = (uint32_t)b; *nout = 1; return F_TEXT;
    }
    /* Latin Extended-B: the letters that matter for European/Vietnamese names. */
    if (c >= 0x180 && c <= 0x24F) {
        uint32_t m = 0;
        if (c == 0x192) m = 'f';
        else if (c == 0x1A0 || c == 0x1A1) m = 'o';
        else if (c == 0x1AF || c == 0x1B0) m = 'u';
        else if (c == 0x1CD || c == 0x1CE) m = 'a';
        else if (c == 0x1CF || c == 0x1D0) m = 'i';
        else if (c == 0x1D1 || c == 0x1D2) m = 'o';
        else if (c >= 0x1D3 && c <= 0x1DC) m = 'u';
        else if (c >= 0x218 && c <= 0x219) m = 's';   /* Romanian s-comma */
        else if (c >= 0x21A && c <= 0x21B) m = 't';   /* Romanian t-comma */
        else if (c == 0x189) m = 'd';
        else if (c == 0x197) m = 'i';
        if (m) { out[0] = m; *nout = 1; return F_TEXT; }
        out[0] = c; *nout = 1; return F_TEXT;
    }
    if (c >= 0x300 && c <= 0x36F) return F_DROP; /* combining diacritics */
    /* Greek */
    if (c >= 0x370 && c <= 0x3FF) {
        uint32_t m = c;
        if (c >= 0x391 && c <= 0x3A9) m = c + 0x20;
        else switch (c) {
            case 0x386: case 0x3AC: m = 0x3B1; break;
            case 0x388: case 0x3AD: m = 0x3B5; break;
            case 0x389: case 0x3AE: m = 0x3B7; break;
            case 0x38A: case 0x3AF: case 0x390: case 0x3AA: case 0x3CA: m = 0x3B9; break;
            case 0x38C: case 0x3CC: m = 0x3BF; break;
            case 0x38E: case 0x3CD: case 0x3B0: case 0x3AB: case 0x3CB: m = 0x3C5; break;
            case 0x38F: case 0x3CE: m = 0x3C9; break;
            case 0x3C2: m = 0x3C3; break;
            case 0x37E: case 0x387: return F_SEP; /* Greek question mark, ano teleia */
            default: break;
        }
        out[0] = m; *nout = 1; return F_TEXT;
    }
    /* Cyrillic */
    if (c >= 0x400 && c <= 0x4FF) {
        uint32_t m = c;
        if (c >= 0x410 && c <= 0x42F) m = c + 0x20;
        else if (c >= 0x400 && c <= 0x40F) m = c + 0x50;
        else if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x4FF)) m = c | 1;
        else if (c >= 0x4C1 && c <= 0x4CE) m = (c & 1) ? c + 1 : c;
        else if (c == 0x4C0) m = 0x4CF;
        if (m == 0x451) m = 0x435; /* ё -> е */
        out[0] = m; *nout = 1; return F_TEXT;
    }
    if (c == 0x1E9E) { out[0] = 's'; out[1] = 's'; *nout = 2; return F_TEXT; }
    if (c >= 0x1E00 && c <= 0x1E95) { out[0] = (uint32_t)LATIN_EXT_ADD[c - 0x1E00]; *nout = 1; return F_TEXT; }
    if (c >= 0x1EA0 && c <= 0x1EF9) { /* Vietnamese */
        uint32_t m;
        if (c <= 0x1EB7) m = 'a';
        else if (c <= 0x1EC7) m = 'e';
        else if (c <= 0x1ECB) m = 'i';
        else if (c <= 0x1EE3) m = 'o';
        else if (c <= 0x1EF1) m = 'u';
        else m = 'y';
        out[0] = m; *nout = 1; return F_TEXT;
    }
    /* Fullwidth ASCII */
    if (c >= 0xFF10 && c <= 0xFF19) { out[0] = c - 0xFF10 + '0'; *nout = 1; return F_TEXT; }
    if (c >= 0xFF21 && c <= 0xFF3A) { out[0] = c - 0xFF21 + 'a'; *nout = 1; return F_TEXT; }
    if (c >= 0xFF41 && c <= 0xFF5A) { out[0] = c - 0xFF41 + 'a'; *nout = 1; return F_TEXT; }
    if (c >= 0xFF00 && c <= 0xFF65) return F_SEP;  /* other fullwidth punctuation */
    if (c >= 0xFE00 && c <= 0xFE0F) return F_DROP; /* variation selectors */
    if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF || c == 0xAD) return F_DROP;
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || c == 0xFFFD) return F_SEP;
    out[0] = c; *nout = 1; return F_TEXT; /* any other script: keep as is */
}

static int is_digit_cp(uint32_t c) { return c >= '0' && c <= '9'; }

static size_t put_utf8(char *out, size_t pos, size_t cap, uint32_t c)
{
    unsigned char b[4];
    size_t n;
    if (c < 0x80) { b[0] = (unsigned char)c; n = 1; }
    else if (c < 0x800) { b[0] = (unsigned char)(0xC0 | (c >> 6)); b[1] = (unsigned char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { b[0] = (unsigned char)(0xE0 | (c >> 12)); b[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (c & 0x3F)); n = 3; }
    else { b[0] = (unsigned char)(0xF0 | (c >> 18)); b[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (c & 0x3F)); n = 4; }
    if (pos + n >= cap) return pos; /* no room: drop (never split a character) */
    memcpy(out + pos, b, n);
    return pos + n;
}

size_t tz_normalize(const char *in, char *out, size_t outcap)
{
    if (!outcap) return 0;
    const unsigned char *s = (const unsigned char *)(in ? in : "");
    const unsigned char *end = s + strlen((const char *)s);
    size_t pos = 0;
    int pending_space = 0; /* a separator was seen since the last character */
    int last_digit = -1;   /* -1 = start of token */
    while (s < end) {
        uint32_t cp = tz_utf8_next(&s, end);
        uint32_t f[2];
        int nf;
        fold_kind k = fold_cp(cp, f, &nf);
        if (k == F_DROP) continue;
        if (k == F_SEP) { pending_space = 1; continue; }
        for (int i = 0; i < nf; i++) {
            int d = is_digit_cp(f[i]);
            if (pos > 0 && (pending_space || (last_digit >= 0 && d != last_digit))) {
                if (pos + 1 >= outcap) goto done;
                out[pos++] = ' ';
            }
            pending_space = 0;
            size_t np = put_utf8(out, pos, outcap, f[i]);
            if (np == pos) goto done;
            pos = np;
            last_digit = d;
        }
    }
done:
    out[pos] = 0;
    return pos;
}
