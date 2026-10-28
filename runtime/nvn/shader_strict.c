#include "shader_strict.h"

#include <stdatomic.h>

static _Atomic int first_visits, skips;

void shader_strict_first_visit(void) { atomic_fetch_add(&first_visits, 1); }
void shader_strict_skip(void) { atomic_fetch_add(&skips, 1); }
int shader_strict_first_visits(void) { return atomic_load(&first_visits); }
int shader_strict_skips(void) { return atomic_load(&skips); }
bool shader_strict_failed(void) { return atomic_load(&first_visits) || atomic_load(&skips); }
void shader_strict_reset(void) {
    atomic_store(&first_visits, 0);
    atomic_store(&skips, 0);
}
