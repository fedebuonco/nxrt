// The strict gate's counters (runtime/nvn/shader_strict.c).
#include "harness.h"
#include "shader_strict.h"

int main(void) {
    shader_strict_reset();
    CHECK(!shader_strict_failed());
    shader_strict_first_visit();
    shader_strict_skip();
    shader_strict_skip();
    CHECK_EQ(shader_strict_first_visits(), 1);
    CHECK_EQ(shader_strict_skips(), 2);
    CHECK(shader_strict_failed());
    shader_strict_reset();
    CHECK(!shader_strict_failed());
    TEST_END();
}
