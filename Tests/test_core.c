/* Unit tests for the portable core: text normalisation, coordinate parsing,
 * geometry helpers. Run with `make -C Tests`. */
#include "../Core/tz_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void norm(const char *in, const char *want)
{
    char out[256];
    tz_normalize(in, out, sizeof out);
    CHECK(strcmp(out, want) == 0, "normalize(\"%s\") = \"%s\", want \"%s\"", in, out, want);
}

static void coord_ok(const char *in, double lat, double lon)
{
    double a = 999, b = 999;
    int ok = tz_parse_coordinate(in, &a, &b);
    CHECK(ok && fabs(a - lat) < 1e-6 && fabs(b - lon) < 1e-6,
          "parse(\"%s\") = %d %.7f %.7f, want %.7f %.7f", in, ok, a, b, lat, lon);
}

static void coord_bad(const char *in)
{
    double a, b;
    CHECK(!tz_parse_coordinate(in, &a, &b), "parse(\"%s\") should fail, got %.6f %.6f", in, a, b);
}

int main(void)
{
    /* --- normalisation --- */
    norm("Váci út 12", "vaci ut 12");
    norm("  Andrássy   út  ", "andrassy ut");
    norm("Straße", "strasse");
    norm("Øresundsbron", "oresundsbron");
    norm("Łódź", "lodz");
    norm("Hőgyes Endre u.", "hogyes endre u");
    norm("Bürgermeister-Smidt-Straße", "burgermeister smidt strasse");
    norm("12a", "12 a");
    norm("12/A", "12 a");
    norm("A1", "a 1");
    norm("Şişli", "sisli");
    norm("Țară", "tara");
    norm("Đà Nẵng", "da nang");
    norm("Москва", "москва");
    norm("Ёлка", "елка");
    norm("Αθήνα", "αθηνα");
    norm("ΣΟΦΟΚΛΕΟΥΣ", "σοφοκλεουσ");
    norm("O'Brien’s Pub", "o brien s pub");
    norm("Cafe\xcc\x81", "cafe");            /* decomposed accent */
    norm("ＡＢＣ１２３", "abc 123");          /* fullwidth */
    norm("東京", "東京");                     /* CJK kept as is */
    norm("", "");
    norm("---", "");
    {
        /* truncation never splits a character and always terminates */
        char small[6];
        size_t n = tz_normalize("ÁÁÁÁÁÁ", small, sizeof small);
        CHECK(n == 5 && strcmp(small, "aaaaa") == 0, "truncation: \"%s\" (%zu)", small, n);
        char tiny[3];
        tz_normalize("Москва", tiny, sizeof tiny);
        CHECK(strcmp(tiny, "м") == 0, "utf8 truncation: \"%s\"", tiny);
    }
    {
        /* every Latin Extended-A letter folds to ASCII */
        for (unsigned cp = 0x100; cp <= 0x17F; cp++) {
            char in[4] = { (char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F)), 0, 0 };
            char out[8];
            tz_normalize(in, out, sizeof out);
            int ascii = out[0] != 0;
            for (char *p = out; *p; p++) if ((unsigned char)*p >= 0x80 || *p < 'a' || *p > 'z') ascii = 0;
            CHECK(ascii, "U+%04X folds to \"%s\"", cp, out);
        }
        for (unsigned cp = 0x1E00; cp <= 0x1E95; cp++) {
            char in[4] = { (char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F)), 0 };
            char out[8];
            tz_normalize(in, out, sizeof out);
            CHECK(strlen(out) == 1 && out[0] >= 'a' && out[0] <= 'z', "U+%04X folds to \"%s\"", cp, out);
        }
        /* spot checks of the alphabetical layout */
        norm("\xe1\xb8\x80", "a");   /* U+1E00 */
        norm("\xe1\xb8\x8c", "d");   /* U+1E0C D with dot below */
        norm("\xe1\xb9\xa3", "s");   /* U+1E63 s with dot below */
        norm("\xe1\xba\x95", "z");   /* U+1E95 */
        norm("\xc5\x91", "o");       /* U+0151 o double acute */
        norm("\xc5\xb1", "u");       /* U+0171 u double acute */
        norm("\xc5\x93", "oe");      /* U+0153 */
        norm("\xc4\xb3", "ij");      /* U+0133 */
    }

    /* --- coordinates --- */
    coord_ok("47.4979, 19.0402", 47.4979, 19.0402);
    coord_ok("47.4979 19.0402", 47.4979, 19.0402);
    coord_ok("47.4979,19.0402", 47.4979, 19.0402);
    coord_ok("-33.8688, 151.2093", -33.8688, 151.2093);
    coord_ok("N47.4979 E19.0402", 47.4979, 19.0402);
    coord_ok("47.4979N 19.0402E", 47.4979, 19.0402);
    coord_ok("47.4979 N, 19.0402 E", 47.4979, 19.0402);
    coord_ok("S33.8688 E151.2093", -33.8688, 151.2093);
    coord_ok("40.7128 N 74.0060 W", 40.7128, -74.0060);
    coord_ok("E19.0402 N47.4979", 47.4979, 19.0402);
    coord_ok("47°29'52.4\"N 19°02'24.7\"E", 47 + 29 / 60.0 + 52.4 / 3600.0, 19 + 2 / 60.0 + 24.7 / 3600.0);
    coord_ok("47° 29′ 52″ N, 19° 2′ 24″ E", 47 + 29 / 60.0 + 52 / 3600.0, 19 + 2 / 60.0 + 24 / 3600.0);
    coord_ok("47 29.873 N 19 2.412 E", 47 + 29.873 / 60.0, 19 + 2.412 / 60.0);
    coord_ok("47 29.873 19 2.412", 47 + 29.873 / 60.0, 19 + 2.412 / 60.0);
    coord_ok("47 29 52 19 2 24", 47 + 29 / 60.0 + 52 / 3600.0, 19 + 2 / 60.0 + 24 / 3600.0);
    coord_ok("geo:47.4979,19.0402", 47.4979, 19.0402);
    coord_ok("47.5, 19", 47.5, 19);
    coord_ok("47 N 19 E", 47, 19);
    coord_bad("12 34");                 /* two whole numbers: probably a house number */
    coord_bad("Váci út 12");
    coord_bad("Nagy utca");
    coord_bad("Route 66");
    coord_bad("95.0, 19.0");            /* latitude out of range */
    coord_bad("47.0, 190.0");
    coord_bad("47 61 00 N 19 0 0 E");    /* minutes >= 60 */
    coord_bad("N47.5 N19.0");            /* two latitudes */
    coord_bad("-47.5 S 19 E");           /* contradictory sign */
    coord_bad("");
    coord_bad("47.5");

    /* --- geometry --- */
    double d = tz_distance_m(47.4979, 19.0402, 48.2082, 16.3738); /* Budapest - Vienna */
    CHECK(fabs(d - 214000) < 2000, "distance Budapest-Vienna %.0f", d);
    double b = tz_bearing_deg(0, 0, 1, 0);
    CHECK(fabs(b) < 1e-9, "bearing north %.6f", b);
    b = tz_bearing_deg(0, 0, 0, 1);
    CHECK(fabs(b - 90) < 1e-9, "bearing east %.6f", b);
    CHECK(strcmp(tz_status_message(TZ_ERR_NO_ROUTE), "No route between these points") == 0, "status message");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
