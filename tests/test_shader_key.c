// The compiled-library cache name (runtime/nvn/shader_source_key.c), vectors shared with tools/shader_libs.py.
#include "harness.h"
#include "shader_source_key.h"

int main(void) {
    CHECK_EQ(shader_source_key(""), 0xcbf29ce484222325ull);
    CHECK_EQ(shader_source_key("abc"), 0xe71fa2190541574bull);
    CHECK_EQ(shader_source_key("hello"), 0xa430d84680aabd0bull);
    TEST_END();
}
