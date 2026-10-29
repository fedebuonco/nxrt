// Cosmetics recolour hooks (LAS's feature). The generic runtime keeps the seam: a game that supports recolour
// overrides these; the default build is a no-op.
#pragma once

#define RECOLOUR_TINT_FLOATS 16

static inline int recolour_class_of(int id) { (void)id; return 0; }
static inline void recolour_add(int out[2], int cls) { (void)out, (void)cls; }
static inline void recolour_tint_draw(const int classes[2], const float tint[8]) { (void)classes, (void)tint; }
