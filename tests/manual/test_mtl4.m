#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static MTL4LibraryFunctionDescriptor *fn(NSString *name, id<MTLLibrary> lib) {
    MTL4LibraryFunctionDescriptor *f = [MTL4LibraryFunctionDescriptor new];
    f.name = name; f.library = lib; return f;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no device\n"); return 2; }
        NSError *e = nil;
        id<MTL4Compiler> c = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        if (!c) { fprintf(stderr, "compiler: %s\n", e.localizedDescription.UTF8String); return 3; }
        MTL4LibraryDescriptor *ld = [MTL4LibraryDescriptor new];
        ld.source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                     "vertex float4 main0(uint v [[vertex_id]]) { return float4(1.0); }\n"
                     "fragment float4 main0_f() { return float4(1.0); }\n";
        id<MTLLibrary> lib = [c newLibraryWithDescriptor:ld error:&e];
        if (!lib) { fprintf(stderr, "lib: %s\n", e.localizedDescription.UTF8String); return 4; }
        MTL4RenderPipelineDescriptor *pd = [MTL4RenderPipelineDescriptor new];
        pd.vertexFunctionDescriptor = fn(@"main0", lib);
        pd.fragmentFunctionDescriptor = fn(@"main0_f", lib);
        pd.rasterSampleCount = 1;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
        pd.colorAttachments[0].blendingState = MTL4BlendStateUnspecialized;
        id<MTLRenderPipelineState> base = [c newRenderPipelineStateWithDescriptor:pd dynamicLinkingDescriptor:nil compilerTaskOptions:nil error:&e];
        fprintf(stderr, "base: %s %s\n", base ? "yes" : "no", e ? e.localizedDescription.UTF8String : "-");
        if (!base) return 5;
        MTL4RenderPipelineDescriptor *sd = [MTL4RenderPipelineDescriptor new];
        sd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        sd.colorAttachments[0].blendingState = MTL4BlendStateDisabled;
        id<MTLRenderPipelineState> spec = [c newRenderPipelineStateBySpecializationWithDescriptor:sd pipeline:base error:&e];
        fprintf(stderr, "specialized: %s %s\n", spec ? "yes" : "no", e ? e.localizedDescription.UTF8String : "-");
        return spec ? 0 : 6;
    }
}
