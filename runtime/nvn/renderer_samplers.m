// Sampler state conversion (split from renderer.m).
#import <Foundation/Foundation.h>
#include <string.h>
#include "renderer_context.h"
#include "renderer_formats.h"
#include "renderer_samplers.h"
#include "rt.h"

#define dev (renderer_context()->dev)

id<MTLSamplerState> renderer_sampler_for(NvnSamplerDesc *d) {
    if (d->backend) return (__bridge id<MTLSamplerState>)d->backend;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.magFilter = d->mag_filter == 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    uint32_t mf = d->min_filter;
    sd.minFilter = (mf == 1 || mf == 3 || mf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mf <= 1 ? MTLSamplerMipFilterNotMipmapped : (mf == 2 || mf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    MTLSamplerAddressMode modes[3];
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = MTLSamplerAddressModeClampToEdge; break;
        case 1: modes[i] = MTLSamplerAddressModeRepeat; break;
        case 2: case 3: modes[i] = MTLSamplerAddressModeMirrorClampToEdge; break;
        case 4: case 5: modes[i] = MTLSamplerAddressModeClampToBorderColor; break;
        case 6: modes[i] = MTLSamplerAddressModeMirrorRepeat; break;
        default: modes[i] = MTLSamplerAddressModeRepeat;
        }
    }
    sd.sAddressMode = modes[0]; sd.tAddressMode = modes[1]; sd.rAddressMode = modes[2];
    sd.lodMinClamp = d->lod_min;
    sd.lodMaxClamp = d->lod_max > 1000 ? FLT_MAX : d->lod_max;
    if (d->max_aniso > 1) sd.maxAnisotropy = (NSUInteger)fminf(d->max_aniso, 16);
    if (d->lod_bias != 0 && getenv("NXRT_LOG_BIAS"))
        LOG("[sampler] lod bias %g (min filter %u, aniso %g)\n", d->lod_bias, mf, d->max_aniso);
    static float lodmin = -1;  // NXRT_LOD_MIN=<n>: skip the n sharpest mips everywhere (aliasing experiments)
    if (lodmin < 0) lodmin = getenv("NXRT_LOD_MIN") ? (float)atof(getenv("NXRT_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) sd.lodMinClamp = fmaxf(sd.lodMinClamp, lodmin);
    static int stest = -1;  // NXRT_SAMPLER_TEST: full mip range + 16x aniso on mipmapped samplers (moire test)
    if (stest < 0) stest = getenv("NXRT_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { sd.lodMinClamp = 0; sd.lodMaxClamp = FLT_MAX; sd.maxAnisotropy = 16; }
    if (getenv("NXRT_LOG_SAMPLERS"))
        LOG("[sampler] min %u mag %u lod %g..%g aniso %g wrap %u %u\n", mf, d->mag_filter, d->lod_min, d->lod_max, d->max_aniso,
            d->wrap[0], d->wrap[1]);
    if (d->compare_mode) sd.compareFunction = getenv("NXRT_NO_SHADOW") ? MTLCompareFunctionAlways : renderer_compare_fn((uint8_t)d->compare_func);
    sd.supportArgumentBuffers = NO;
    id<MTLSamplerState> s = [dev newSamplerStateWithDescriptor:sd];
    d->backend = (void *)CFBridgingRetain(s);
    return s;
}

id<MTLSamplerState> renderer_default_sampler(void) {
    static id<MTLSamplerState> s;
    if (!s) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        s = [dev newSamplerStateWithDescriptor:sd];
    }
    return s;
}

int renderer_sampler_slots(int n, __strong id<MTLSamplerState> *smp, NvnSamplerDesc *const *desc,
                         __strong id<MTLSamplerState> *slot, uint32_t *map) {
    const NvnSamplerDesc *sdesc[MSL_MAX_SAMPLERS] = {0};
    int ns = 0;
    for (int i = 0; i < n; i++) {
        int k = 0;
        for (; k < ns; k++)  // (one state: the same object, or descriptors alike apart from the cached object)
            if (slot[k] == smp[i] || (desc[i] && sdesc[k] && !memcmp(desc[i], sdesc[k], offsetof(NvnSamplerDesc, backend))))
                break;
        if (k == ns && ns < MSL_MAX_SAMPLERS) { slot[ns] = smp[i]; sdesc[ns++] = desc[i]; }
        else if (k == ns) {  // more distinct states than slots (not seen in the game): the nearest slot - one with the
            static int warned;  // same compare mode, else the last
            if (!warned++) LOG("[metal] a shader with %d textures uses more than %d sampler states: some share one\n", n, MSL_MAX_SAMPLERS);
            uint32_t cmp = desc[i] ? desc[i]->compare_mode : 0;
            for (k = 0; k < ns - 1; k++)
                if ((sdesc[k] ? sdesc[k]->compare_mode : 0) == cmp) break;
        }
        map[i] = (uint32_t)k;
    }
    return ns;
}
