// Headless renderer context/cache: create, set/get/clear, release (no window, no game).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>

#include "renderer_context.h"
#include "renderer_cache.h"

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        renderer_context_init(dev);
        RendererContext *c = renderer_context();
        assert(c->dev == dev);
        assert(c->cache != nil);
        renderer_cache_set(@"k", @"v");
        assert([renderer_cache_get(@"k") isEqual:@"v"]);
        renderer_cache_clear();
        assert(renderer_cache_get(@"k") == nil);
        renderer_context_release();
        assert(renderer_context()->dev == nil);
        assert(renderer_context()->cache == nil);
        puts("renderer_context: ok");
        return 0;
    }
}
