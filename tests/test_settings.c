// The settings file (settings/settings.c): one "key = value" ini, sections registered before load, defaults first,
// the first section that claims a key wins, empty values, CRLF, comments and a save/load/save round trip.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "nx_app.h"
#include "nx_settings.h"

NxAppInfo nx_app = {"test", "testexe", "Test App", "test.bundle", "test-data", "test.ini", "1.0"};

static struct {
    int a, c, d, e;
    char b[128];
} st;
static bool loaded_called, loaded_found;
static int second_a_calls;

static void defaults(void) {
    memset(&st, 0, sizeof st);
    st.a = 7;
    st.c = 100;
    st.d = -1;
    st.e = 42;
    strcpy(st.b, "default b");
}

static bool key(const char *k, const char *v) {
    if (!strcmp(k, "a")) return st.a = atoi(v), true;
    if (!strcmp(k, "b")) return snprintf(st.b, sizeof st.b, "%s", v), true;
    if (!strcmp(k, "c")) return st.c = atoi(v), true;
    if (!strcmp(k, "d")) return st.d = atoi(v), true;
    return false;
}

static bool key2(const char *k, const char *v) {  // claims "a" too: the first registered section must get it
    if (!strcmp(k, "a")) second_a_calls++;
    (void)v;
    return false;
}

static void loaded(bool found) {
    loaded_called = true;
    loaded_found = found;
    if (!st.b[0]) strcpy(st.b, "fixed after load");
}

static void save_section(FILE *f) {
    fprintf(f, "a = %d\n", st.a);
    fprintf(f, "b = %s\n", st.b);
    fprintf(f, "c = %d\n", st.c);
    fprintf(f, "d = %d\n", st.d);
    fprintf(f, "e = %d\n", st.e);
}

static const NxSettingsSection section = {"test", defaults, key, loaded, save_section};
static const NxSettingsSection section2 = {"test2", NULL, key2, NULL, NULL};

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = calloc(1, (size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) s[0] = 0;
    fclose(f);
    return s;
}

static void register_sections(void) {
    nx_settings_register(&section);
    nx_settings_register(&section2);
    nx_settings_register(&section);  // (a repeated registration is ignored)
}

static void test_parse(const char *dir, const char *self) {
    char path[1024];
    snprintf(path, sizeof path, "%s/test.ini", dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fputs("# a comment\r\n", f);
    fputs("  # an indented comment = 1\r\n", f);
    fputs("a = 12\r\n", f);
    fputs("b=two words, with = signs\n", f);
    fputs("c = -3\n", f);
    fputs("d =\n", f);  // empty value
    fputs("e = 5\n", f);
    fputs("unknown = x\n", f);
    fputs("malformed line without equals\n", f);
    fclose(f);

    setenv("NXRT_SETTINGS", path, 1);
    CHECK_STR(nx_settings_path(), path);
    register_sections();
    settings_load();
    CHECK(loaded_called);
    CHECK(loaded_found);
    CHECK_EQ(st.a, 12);  // first section that claims it
    CHECK_STR(st.b, "two words, with = signs");
    CHECK_EQ(st.c, -3);
    CHECK_EQ(st.d, 0);   // "d =": the empty value reaches the section
    CHECK_EQ(st.e, 42);  // only the default: not a known key
    CHECK_EQ(second_a_calls, 0);

    // Save: every section writes its lines, the first line is the file's header.
    settings_save();
    char *saved = read_file(path);
    CHECK(saved != NULL);
    if (!saved) return;
    CHECK(strstr(saved, "Test App settings") != NULL);
    CHECK(strstr(saved, "a = 12\n") != NULL);
    CHECK(strstr(saved, "b = two words, with = signs\n") != NULL);
    CHECK(strstr(saved, "c = -3\n") != NULL);
    CHECK(strstr(saved, "d = 0\n") != NULL);
    CHECK(strstr(saved, "e = 42\n") != NULL);
    CHECK(strstr(saved, "unknown") == NULL);
    CHECK(strstr(saved, "malformed") == NULL);

    // Load what was saved again, in a fresh process (the static "loaded" guard makes it a one-shot in this one):
    // the child loads the file and settings_load writes it back; the second save must be byte-identical.
    setenv("NXRT_SETTINGS_ROUNDTRIP", "1", 1);
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "\"%s\" --roundtrip", self);
    CHECK_EQ(system(cmd), 0);
    char *again = read_file(path);
    CHECK(again != NULL);
    if (again) CHECK_STR(again, saved);
    free(again);
    free(saved);
}

// The child re-loads the given file with the same sections and exits from settings_load after saving it.
static void roundtrip_child(void) {
    register_sections();
    settings_load();
    // settings_load exits in NXRT_SETTINGS_ROUNDTRIP mode; if it didn't, fail.
    fprintf(stderr, "roundtrip child: settings_load returned\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--roundtrip")) roundtrip_child();
    char dir[] = "/tmp/nxrt-settings-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    test_parse(dir, argv[0]);
    // Remove the scratch directory's files.
    char p[1024];
    snprintf(p, sizeof p, "%s/test.ini", dir);
    remove(p);
    rmdir(dir);
    TEST_END();
}
