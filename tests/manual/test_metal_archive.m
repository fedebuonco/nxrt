#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
int main(void) {
 @autoreleasepool {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e=nil;
  NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 main0(uint v [[vertex_id]]) { return float4(1.0); }\nfragment float4 main0_f() { return float4(1.0); }\n";
  id<MTLLibrary> lib=[dev newLibraryWithSource:src options:nil error:&e];
  id<MTLBinaryArchive> a=[dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
  MTLFunctionDescriptor *fd=[MTLFunctionDescriptor functionDescriptor]; fd.name=@"main0";
  BOOL ok=[a addFunctionWithDescriptor:fd library:lib error:&e];
  fprintf(stderr,"add fn %d %s\n",ok,e?e.localizedDescription.UTF8String:"-");
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
  pd.vertexFunction=[lib newFunctionWithName:@"main0"]; pd.fragmentFunction=[lib newFunctionWithName:@"main0_f"];
  pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
  ok=[a addRenderPipelineFunctionsWithDescriptor:pd error:&e];
  fprintf(stderr,"add pipe %d %s\n",ok,e?e.localizedDescription.UTF8String:"-");
  unlink("/tmp/nxrt_test2.metalarchive");
  ok=[a serializeToURL:[NSURL fileURLWithPath:@"/tmp/nxrt_test2.metalarchive"] error:&e];
  fprintf(stderr,"serialize %d %s\n",ok,e?e.localizedDescription.UTF8String:"-");
  return ok?0:1;
 }
}
