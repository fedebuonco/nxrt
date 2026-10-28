// The binary archive: cache/shaders.metalarchive, the GPU code of every pipeline built here, kept across runs.
// Split out of renderer.m (docs/RENDERER_REFACTOR.md): it owns only the archive state and its tag/counters.
#import <Foundation/Foundation.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include "archive_file.h"
#include "renderer_archive.h"
#include "nx_log.h"
#include "shader_prep.h"

#define ARCHIVE_FILE "cache/shaders.metalarchive"
#define ARCHIVE_TAG ARCHIVE_FILE ".tag"

static id<MTLDevice> g_dev;
static id<MTLBinaryArchive> g_archive;
static _Atomic bool g_archive_ready;  // (loaded in the background: archive_open)
static pthread_rwlock_t g_archive_lock = PTHREAD_RWLOCK_INITIALIZER;  // adds: read side (any number); save: write side
static _Atomic int g_archive_dirty, g_archive_hits, g_archive_adds;
static _Atomic uint64_t g_pipe_us, g_lib_us, g_archive_open_us;  // (preparation's log: summed over threads)
static _Atomic uint64_t g_archive_gen;

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

void renderer_archive_init(id<MTLDevice> dev) { g_dev = dev; }
id<MTLBinaryArchive> renderer_archive_get(void) { return g_archive_ready ? g_archive : nil; }
void renderer_archive_hit(void) { g_archive_hits++; }
void renderer_archive_pipeline_us(uint64_t us) { g_pipe_us += us; }
void renderer_archive_library_us(uint64_t us) { g_lib_us += us; }
uint64_t renderer_archive_open_us(void) { return g_archive_open_us; }
uint64_t renderer_archive_pipeline_us_total(void) { return g_pipe_us; }
uint64_t renderer_archive_library_us_total(void) { return g_lib_us; }
int renderer_archive_hits(void) { return g_archive_hits; }
int renderer_archive_adds(void) { return g_archive_adds; }
bool renderer_archive_try_lock(void) { return pthread_rwlock_tryrdlock(&g_archive_lock) == 0; }
void renderer_archive_unlock(void) { pthread_rwlock_unlock(&g_archive_lock); }

static void archive_tag(char *out, size_t n) {
    char gpu[400];
    snprintf(gpu, sizeof gpu, "%s, %s", g_dev.name.UTF8String, NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    shader_cache_tag(out, n, gpu);
}

static void remove_old_text_cache(void) {  // (cache/shaders/*.metal: up to ~130 MB on players' machines)
    DIR *d = opendir("cache/shaders");
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        if (l <= 6 || strcmp(e->d_name + l - 6, ".metal")) continue;  // (leave compiled .metallib files)
        char path[300];
        snprintf(path, sizeof path, "cache/shaders/%s", e->d_name);
        n += unlink(path) == 0;
    }
    closedir(d);
    rmdir("cache/shaders");  // (fails while compiled libraries are there, which is intended)
    NX_LOGI(NX_LOG_GPU, "[metal] removed the old shader text cache (%d files)\n", n);
}

void renderer_archive_save(bool at_exit) {
    if (!renderer_archive_get() || !g_archive_dirty) return;
    if (at_exit ? pthread_rwlock_trywrlock(&g_archive_lock) : pthread_rwlock_wrlock(&g_archive_lock)) return;
    int adds = g_archive_dirty;
    g_archive_dirty = 0;
    double t0 = now_ms();
    NSError *err = nil;
    mkdir("cache", 0755);
    remove(ARCHIVE_FILE ".tmp");
    bool ok = [g_archive serializeToURL:[NSURL fileURLWithPath:@(ARCHIVE_FILE ".tmp")] error:&err];
    pthread_rwlock_unlock(&g_archive_lock);
    if (ok) ok = rename(ARCHIVE_FILE ".tmp", ARCHIVE_FILE) == 0;
    if (!ok) {
        NX_LOGE(NX_LOG_GPU, "[metal] shader archive not saved: %s\n", err ? err.localizedDescription.UTF8String : "rename");
        return;
    }
    char tag[700];
    archive_tag(tag, sizeof tag);
    if (!shader_cache_tag_ok(ARCHIVE_TAG, tag)) shader_cache_tag_write(ARCHIVE_TAG, tag);
    struct stat sb;
    NX_LOGI(NX_LOG_GPU, "[metal] shader archive saved: %d new pipelines, %.1f MB, %.0f ms\n", adds,
            stat(ARCHIVE_FILE, &sb) == 0 ? sb.st_size / 1048576.0 : 0.0, now_ms() - t0);
}

void renderer_archive_exit(void) { renderer_archive_save(true); }

static void archive_publish(id<MTLBinaryArchive> a, double t0, bool loaded) {
    g_archive = a;
    g_archive_ready = a != nil;
    g_archive_open_us = (uint64_t)((now_ms() - t0) * 1000);
    struct stat sb;
    if (loaded)
        NX_LOGI(NX_LOG_GPU, "[metal] shader archive loaded: %.1f MB in %.0f ms\n",
                stat(ARCHIVE_FILE, &sb) == 0 ? sb.st_size / 1048576.0 : 0.0, g_archive_open_us / 1e3);
}

static id<MTLBinaryArchive> archive_new(void) {
    remove(ARCHIVE_FILE);
    remove(ARCHIVE_TAG);
    NSError *err = nil;
    id<MTLBinaryArchive> a = [g_dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&err];
    if (!a) NX_LOGE(NX_LOG_GPU, "[metal] no shader archive: %s\n", err.localizedDescription.UTF8String);
    return a;
}

void renderer_archive_open(void) {  // (at the first pipeline / preparation)
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        double t0 = now_ms();
        remove_old_text_cache();
        remove(ARCHIVE_FILE ".tmp");  // (a save cut short: what it left - Metal writes <uuid>.metallib files beside it)
        DIR *cd = opendir("cache");
        if (cd) {
            struct dirent *e;
            while ((e = readdir(cd))) {
                size_t l = strlen(e->d_name);
                if (l == 36 + 9 && !strcmp(e->d_name + 36, ".metallib")) {
                    char path[300];
                    snprintf(path, sizeof path, "cache/%s", e->d_name);
                    unlink(path);
                }
            }
            closedir(cd);
        }
        const char *on = getenv("NXRT_METAL_ARCHIVE");
        if (!on || !atoi(on)) {
            if (remove(ARCHIVE_FILE) == 0) NX_LOGI(NX_LOG_GPU, "[metal] removed the shader archive (off)\n");
            remove(ARCHIVE_TAG);
            return;
        }
        atexit(renderer_archive_exit);
        char tag[700];
        archive_tag(tag, sizeof tag);
        bool have = access(ARCHIVE_FILE, F_OK) == 0;
        if (have && !archive_file_ok(ARCHIVE_FILE)) {
            NX_LOGI(NX_LOG_GPU, "[metal] shader archive is not a Metal archive: a new one\n");
            remove(ARCHIVE_FILE);
            remove(ARCHIVE_TAG);
            have = false;
        }
        if (!have || !shader_cache_tag_ok(ARCHIVE_TAG, tag)) {
            if (have) NX_LOGI(NX_LOG_GPU, "[metal] shader archive from another build / GPU / macOS: a new one\n");
            archive_publish(archive_new(), t0, false);
            return;
        }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
            d.url = [NSURL fileURLWithPath:@(ARCHIVE_FILE)];
            NSError *err = nil;
            id<MTLBinaryArchive> a = [g_dev newBinaryArchiveWithDescriptor:d error:&err];
            if (!a) NX_LOGE(NX_LOG_GPU, "[metal] shader archive unreadable (%s): a new one\n", err.localizedDescription.UTF8String);
            archive_publish(a ? a : archive_new(), t0, a != nil);
        });
    });
}

void renderer_archive_added(void) {  // saved 10 s after the last add (first-use builds come in bursts)
    g_archive_dirty++, g_archive_adds++;
    uint64_t my = ++g_archive_gen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (g_archive_gen == my) renderer_archive_save(false);
    });
}
