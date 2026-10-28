// The Metal binary archive (renderer_archive.m): load/save/tag and its counters.
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>

void renderer_archive_init(id<MTLDevice> dev);
void renderer_archive_open(void);
id<MTLBinaryArchive> renderer_archive_get(void);
void renderer_archive_added(void);
void renderer_archive_hit(void);
void renderer_archive_save(bool at_exit);
void renderer_archive_exit(void);
void renderer_archive_pipeline_us(uint64_t us);
void renderer_archive_library_us(uint64_t us);
uint64_t renderer_archive_open_us(void);
uint64_t renderer_archive_pipeline_us_total(void);
uint64_t renderer_archive_library_us_total(void);
int renderer_archive_hits(void);
int renderer_archive_adds(void);
bool renderer_archive_try_lock(void);  // read side; false when a save holds the write side
void renderer_archive_unlock(void);
