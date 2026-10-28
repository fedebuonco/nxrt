// Metal 4 base + specialisation, then a real draw read back: the pipeline must produce the triangle's red pixel.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static MTL4LibraryFunctionDescriptor *fn(NSString *name, id<MTLLibrary> lib) {
    MTL4LibraryFunctionDescriptor *f = [MTL4LibraryFunctionDescriptor new];
    f.name = name;
    f.library = lib;
    return f;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTL4Compiler> c = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        MTL4LibraryDescriptor *ld = [MTL4LibraryDescriptor new];
        ld.source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                     "struct VOut { float4 pos [[position]]; };\n"
                     "vertex VOut main0(uint v [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VOut o; o.pos = float4(p[v], 0.0, 1.0); return o; }\n"
                     "fragment float4 main0_f() { return float4(1.0,0.0,0.0,1.0); }\n";
        id<MTLLibrary> lib = [c newLibraryWithDescriptor:ld error:&e];
        if (!lib) { fprintf(stderr, "lib: %s\n", e.localizedDescription.UTF8String); return 3; }

        MTL4RenderPipelineDescriptor *pd = [MTL4RenderPipelineDescriptor new];
        pd.vertexFunctionDescriptor = fn(@"main0", lib);
        pd.fragmentFunctionDescriptor = fn(@"main0_f", lib);
        pd.rasterSampleCount = 1;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;  // the format the base is built for
        pd.colorAttachments[0].blendingState = MTL4BlendStateUnspecialized;
        id<MTLRenderPipelineState> base = [c newRenderPipelineStateWithDescriptor:pd dynamicLinkingDescriptor:nil
                                                             compilerTaskOptions:nil error:&e];
        if (!base) { fprintf(stderr, "base: %s\n", e.localizedDescription.UTF8String); return 4; }

        MTL4RenderPipelineDescriptor *sd = [MTL4RenderPipelineDescriptor new];
        sd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        sd.colorAttachments[0].blendingState = MTL4BlendStateDisabled;
        id<MTLRenderPipelineState> spec = [c newRenderPipelineStateBySpecializationWithDescriptor:sd pipeline:base error:&e];
        if (!spec) { fprintf(stderr, "spec: %s\n", e.localizedDescription.UTF8String); return 5; }

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:spec];
        [enc setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        uint8_t px[4] = {0};
        [tex getBytes:px bytesPerRow:4 fromRegion:MTLRegionMake2D(32, 32, 1, 1) mipmapLevel:0];
        fprintf(stderr, "draw: status %ld pixel %u,%u,%u,%u\n", (long)cb.status, px[0], px[1], px[2], px[3]);
        if (cb.status != MTLCommandBufferStatusCompleted || px[2] < 200) return 6;  // red in BGRA
        return 0;
    }
}
