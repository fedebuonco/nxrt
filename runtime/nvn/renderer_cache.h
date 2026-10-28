// The renderer's shared object cache (renderer_cache.m): translated programs, pipeline states, depth-stencil states.
// renderer.m owns the locks/synchronized sections; this module owns the dictionary and its reset.
#pragma once
#import <Foundation/Foundation.h>

id renderer_cache_get(NSString *key);
void renderer_cache_set(NSString *key, id value);
void renderer_cache_clear(void);
