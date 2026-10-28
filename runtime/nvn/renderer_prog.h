// A translated program: its Metal functions/libraries and translated ShaderInfo (renderer_prog.m).
#pragma once
#import <Metal/Metal.h>
#include <stdatomic.h>
#include "maxwell/shader.h"
#include "shader_prep.h"

@interface Prog : NSObject {
@public
    id<MTLFunction> vs, fs;
    id<MTLLibrary> vlib, flib;
    ShaderInfo *vi, *fi;
    bool failed;
    _Atomic bool ready;  // functions compiled (async compile: draws are skipped until then)
    ShaderFp fp[2];      // (stale-program detector, shader_prep.h; render thread only)
}
@end
