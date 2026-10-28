// Snapshot validity for render-target feedback (renderer_snapshots.m).
#pragma once
#include "nvn.h"
#include <stdbool.h>

void renderer_snapshots_invalidate(const char *why);              // all
void renderer_snapshots_invalidate_tex(NvnTexture *t, const char *why);
void renderer_snapshots_reset(void);
NvnTexture *renderer_snapshot_texture(int i);
bool renderer_snapshot_ok(int i);
const char *renderer_snapshot_why(int i);
void renderer_snapshot_take(int i, NvnTexture *t);
