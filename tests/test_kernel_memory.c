// Kernel memory map (heap / regions / QueryMemory), compiled with kernel.c so the statics are visible.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"

#include "../runtime/kernel.c"

// Data the kernel references but this path never reads.
Module g_modules[8];
int g_nmodules;
int rt_romfs_files = -1;
const NxGame nx_game = {0};
void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { checks++; long long a_=(long long)(a), b_=(long long)(b); if (a_ != b_) { failures++; fprintf(stderr, "FAIL %s:%d: %s=%lld != %s=%lld\n", __FILE__, __LINE__, #a, a_, #b, b_); } } while (0)

int main(void) {
    nregions = 0;
    heap_size = 0;
    uint64_t out[5];

    region_add(0x1000, 0x1000, 3, 7);
    region_add(0x4000, 0x1000, 4, 3);
    memset(out, 0, sizeof out);
    query_memory(0x1000, (uint64_t)(uintptr_t)out);
    CHECK_EQ(out[0], 0x1000);
    CHECK_EQ(out[1], 0x1000);
    CHECK_EQ(out[2], 3);
    CHECK_EQ(out[3], 7);

    // A gap before the first region, and one between the two.
    query_memory(0x800, (uint64_t)(uintptr_t)out);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0x1000);
    query_memory(0x2000, (uint64_t)(uintptr_t)out);
    CHECK_EQ(out[0], 0x2000);
    CHECK_EQ(out[1], 0x2000);

    // The heap is one region at HEAP_BASE, capped at HEAP_MAX.
    kernel_set_heap(0x100000);
    CHECK_EQ(kernel_heap_size(), 0x100000);
    query_memory(HEAP_BASE, (uint64_t)(uintptr_t)out);
    CHECK_EQ(out[0], HEAP_BASE);
    CHECK_EQ(out[1], 0x100000);
    CHECK_EQ(out[2], 5);
    kernel_set_heap(HEAP_MAX + 0x1000);
    CHECK_EQ(kernel_heap_size(), HEAP_MAX);
    kernel_set_heap(0);  // remove
    CHECK_EQ(kernel_heap_size(), 0);

    CHECK(checks > 0);
    printf("test_kernel_memory: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
