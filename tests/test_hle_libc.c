// The SDK C routines implemented on the host (runtime/hle/libc.c). gen.py routes the SDK's mem*/str* calls here, so
// their exact return values (the byte difference, not just its sign), overlapping copies and NUL handling matter.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "hle.h"

void hle_memcpy(Cpu *c);
void hle_memmove(Cpu *c);
void hle_memset(Cpu *c);
void hle_memcmp(Cpu *c);
void hle_bcmp(Cpu *c);
void hle_strlen(Cpu *c);
void hle_strcmp(Cpu *c);
void hle_strncmp(Cpu *c);
void hle_memchr(Cpu *c);

static int64_t ret(Cpu *c) {
    return (int64_t)c->x[0];
}

static void test_mem(void) {
    Cpu c = {0};
    char a[16] = "abc", b[16] = "abd", z[16] = "abc";
    c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)b, c.x[2] = 3;
    hle_memcmp(&c);
    CHECK_EQ(ret(&c), 'c' - 'd');  // -1, the difference of the first unequal bytes
    c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)z;
    hle_memcmp(&c);
    CHECK_EQ(ret(&c), 0);
    c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)b, c.x[2] = 0;  // zero bytes: equal
    hle_memcmp(&c);
    CHECK_EQ(ret(&c), 0);
    char big[16] = "abz", small[16] = "abc";
    c.x[0] = (uint64_t)(uintptr_t)big, c.x[1] = (uint64_t)(uintptr_t)small, c.x[2] = 3;
    hle_memcmp(&c);
    CHECK_EQ(ret(&c), 23);
    c.x[0] = (uint64_t)(uintptr_t)big, c.x[1] = (uint64_t)(uintptr_t)small;
    hle_bcmp(&c);
    CHECK_EQ(ret(&c), 1);
    c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)z;
    hle_bcmp(&c);
    CHECK_EQ(ret(&c), 0);

    char buf[16] = "abcdef";
    c.x[0] = (uint64_t)(uintptr_t)(buf + 2), c.x[1] = (uint64_t)(uintptr_t)buf, c.x[2] = 4;
    hle_memcpy(&c);
    CHECK_STR(buf, "ababcd");
    char move[16] = "abcdef";
    c.x[0] = (uint64_t)(uintptr_t)(move + 2), c.x[1] = (uint64_t)(uintptr_t)move, c.x[2] = 4;
    hle_memmove(&c);
    CHECK_STR(move, "ababcd");
    c.x[0] = (uint64_t)(uintptr_t)move, c.x[1] = '7', c.x[2] = 4;
    hle_memset(&c);
    CHECK_MEM(move, "7777", 4);
    CHECK_EQ(move[4], 'c');  // one byte past the count is untouched (from the move above)
}

static void test_str(void) {
    Cpu c = {0};
    char a[16] = "abc", b[16] = "abd";
    c.x[0] = (uint64_t)(uintptr_t)a;
    hle_strlen(&c);
    CHECK_EQ(ret(&c), 3);
    c.x[0] = (uint64_t)(uintptr_t)"";
    hle_strlen(&c);
    CHECK_EQ(ret(&c), 0);

    c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)b;
    hle_strcmp(&c);
    CHECK_EQ(ret(&c), 'c' - 'd');
    c.x[0] = (uint64_t)(uintptr_t)b, c.x[1] = (uint64_t)(uintptr_t)a;
    hle_strcmp(&c);
    CHECK_EQ(ret(&c), 'd' - 'c');
    c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)a;
    hle_strcmp(&c);
    CHECK_EQ(ret(&c), 0);
    char ab[16] = "ab";
    c.x[0] = (uint64_t)(uintptr_t)ab, c.x[1] = (uint64_t)(uintptr_t)a;
    hle_strcmp(&c);
    CHECK_EQ(ret(&c), 0 - 'c');  // the shorter string's NUL is smaller

    // strncmp: at most n bytes, and a NUL ends the comparison.
    for (int n = 0; n <= 3; n++) {
        c.x[0] = (uint64_t)(uintptr_t)a, c.x[1] = (uint64_t)(uintptr_t)b, c.x[2] = (uint64_t)n;
        hle_strncmp(&c);
        CHECK_EQ(ret(&c), n < 3 ? 0 : 'c' - 'd');
    }
    c.x[0] = (uint64_t)(uintptr_t)ab, c.x[1] = (uint64_t)(uintptr_t)a, c.x[2] = 5;
    hle_strncmp(&c);
    CHECK_EQ(ret(&c), 0 - 'c');
    c.x[0] = (uint64_t)(uintptr_t)ab, c.x[1] = (uint64_t)(uintptr_t)a, c.x[2] = 2;
    hle_strncmp(&c);
    CHECK_EQ(ret(&c), 0);
}

static void test_search(void) {
    Cpu c = {0};
    char buf[16] = "hello world";
    c.x[0] = (uint64_t)(uintptr_t)buf, c.x[1] = 'o', c.x[2] = 11;
    hle_memchr(&c);
    CHECK_EQ(ret(&c), (uintptr_t)(buf + 4));
    c.x[0] = (uint64_t)(uintptr_t)buf, c.x[1] = 'z';
    hle_memchr(&c);
    CHECK_EQ(ret(&c), 0);
    c.x[0] = (uint64_t)(uintptr_t)buf, c.x[1] = 'd', c.x[2] = 10;  // 'd' is the 11th byte: not in the first 10
    hle_memchr(&c);
    CHECK_EQ(ret(&c), 0);
    c.x[0] = (uint64_t)(uintptr_t)buf, c.x[2] = 11;
    hle_memchr(&c);
    CHECK_EQ(ret(&c), (uintptr_t)(buf + 10));
}

int main(void) {
    test_mem();
    test_str();
    test_search();
    TEST_END();
}
