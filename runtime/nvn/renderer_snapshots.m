// Snapshot validity (split from renderer.m).
#import <Foundation/Foundation.h>
#include <string.h>

#include "renderer_snapshots.h"

static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";

static void invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++)
        if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

void renderer_snapshots_invalidate(const char *why) { snap_reason = why; invalidate(NULL); }
void renderer_snapshots_invalidate_tex(NvnTexture *t, const char *why) { snap_reason = why; invalidate(t); }
NvnTexture *renderer_snapshot_texture(int i) { return snap_tex[i]; }
bool renderer_snapshot_ok(int i) { return snap_ok[i]; }
const char *renderer_snapshot_why(int i) { return snap_why[i]; }
void renderer_snapshot_take(int i, NvnTexture *t) { snap_tex[i] = t; snap_ok[i] = true; }
void renderer_snapshots_reset(void) { memset(snap_tex, 0, sizeof snap_tex); memset(snap_ok, 0, sizeof snap_ok); }
