#import <Foundation/Foundation.h>

#include "renderer_cache.h"
#include "renderer_context.h"

id renderer_cache_get(NSString *key) { return renderer_context()->cache[key]; }

void renderer_cache_set(NSString *key, id value) {
    if (value) renderer_context()->cache[key] = value;
    else [renderer_context()->cache removeObjectForKey:key];
}

void renderer_cache_clear(void) { [renderer_context()->cache removeAllObjects]; }
