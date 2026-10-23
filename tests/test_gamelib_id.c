// The game code library's id (runtime/gamelib.h): GAMELIB_ID-<the first 8 bytes of main's build id>. A library with
// any other id is refused, so the byte order and the NSO check here decide whether a player's game code is usable.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gamelib.h"
#include "harness.h"

static char g_tmp[512];

static void mkdir_p(const char *p) {
    char b[1024];
    snprintf(b, sizeof b, "%s", p);
    for (char *c = b + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(b, 0755);
            *c = '/';
        }
    mkdir(b, 0755);
}

static void write_nso(const char *game, const uint8_t *build_id) {
    char p[900];
    mkdir_p((snprintf(p, sizeof p, "%s/exefs", game), p));
    snprintf(p, sizeof p, "%s/exefs/main", game);
    uint8_t hdr[0x60] = {0};
    memcpy(hdr, "NSO0", 4);
    memcpy(hdr + 0x40, build_id, 8);
    FILE *f = fopen(p, "wb");
    if (!f) {
        perror(p);
        exit(2);
    }
    fwrite(hdr, 1, sizeof hdr, f);
    fclose(f);
}

static bool id_of(const char *game, char *out, size_t n) {
    return gamelib_full_id(game, out, n);
}

int main(void) {
    char dir[] = "/tmp/nxrt-gamelib-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_tmp, sizeof g_tmp, "%s", dir);
    char out[GAMELIB_FULL_ID_SIZE];

    static const uint8_t build_id[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    write_nso(g_tmp, build_id);
    CHECK(id_of(g_tmp, out, sizeof out));
    CHECK_STR(out, GAMELIB_ID "-0123456789ABCDEF");

    // A build id with a zero byte: every byte is printed, so the id stays fixed-width.
    static const uint8_t zero_id[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    write_nso(g_tmp, zero_id);
    CHECK(id_of(g_tmp, out, sizeof out));
    CHECK_STR(out, GAMELIB_ID "-0000000000000000");

    // exefs/main missing, not an NSO, or too short: no id.
    char p[900];
    snprintf(p, sizeof p, "%s/exefs/main", g_tmp);
    remove(p);
    CHECK(!id_of(g_tmp, out, sizeof out));
    write_nso(g_tmp, build_id);
    FILE *f = fopen(p, "r+b");
    CHECK(f != NULL);
    if (f) {
        fwrite("XSO0", 1, 4, f);
        fclose(f);
    }
    CHECK(!id_of(g_tmp, out, sizeof out));
    f = fopen(p, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("NSO0", f);  // too short
        fclose(f);
    }
    CHECK(!id_of(g_tmp, out, sizeof out));

    // A different GAMELIB_ID compile would be compared by the build; here the prefix is the one built in.
    char small[8];
    write_nso(g_tmp, build_id);
    CHECK(id_of(g_tmp, small, sizeof small));
    CHECK(strncmp(small, GAMELIB_ID "-", strlen(GAMELIB_ID) + 1) == 0);

    snprintf(p, sizeof p, "%s/exefs/main", g_tmp);
    remove(p);
    snprintf(p, sizeof p, "%s/exefs", g_tmp);
    rmdir(p);
    rmdir(g_tmp);
    TEST_END();
}
