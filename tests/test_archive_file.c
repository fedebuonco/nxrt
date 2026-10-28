// The binary archive's file check (runtime/nvn/archive_file.c).
#include <unistd.h>

#include "archive_file.h"
#include "harness.h"

static const char *write_tmp(const char *name, const unsigned char *b, size_t n) {
    static char path[256];
    snprintf(path, sizeof path, "/tmp/nxrt_archive_%s", name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
    return path;
}

int main(void) {
    const unsigned char macho[] = {0xcf, 0xfa, 0xed, 0xfe, 0, 0, 0, 0};
    const unsigned char fat[] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 0};
    const unsigned char metalarc[] = {0xcb, 0xfe, 0xba, 0xbe, 0, 0, 0, 0};
    const unsigned char text[] = {'n', 'o', 'p', 'e'};
    const unsigned char shortf[] = {0xcf, 0xfa};
    CHECK(archive_file_ok(write_tmp("macho", macho, sizeof macho)));
    CHECK(archive_file_ok(write_tmp("fat", fat, sizeof fat)));
    CHECK(archive_file_ok(write_tmp("metalarc", metalarc, sizeof metalarc)));
    CHECK(!archive_file_ok(write_tmp("text", text, sizeof text)));
    CHECK(!archive_file_ok(write_tmp("short", shortf, sizeof shortf)));
    CHECK(!archive_file_ok("/tmp/nxrt_archive_does_not_exist"));
    CHECK(!archive_file_ok(NULL));
    unlink("/tmp/nxrt_archive_macho");
    unlink("/tmp/nxrt_archive_fat");
    unlink("/tmp/nxrt_archive_metalarc");
    unlink("/tmp/nxrt_archive_text");
    unlink("/tmp/nxrt_archive_short");
    TEST_END();
}
