// NVN format/state to Metal conversion (split from renderer.m).
#import <Foundation/Foundation.h>
#include <stdint.h>
#include "renderer_formats.h"
#include "rt.h"

int renderer_attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

MTLVertexFormat renderer_vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return MTLVertexFormatUCharNormalized;
    case 0x02: return MTLVertexFormatCharNormalized;
    case 0x03: return MTLVertexFormatUChar;
    case 0x04: return MTLVertexFormatChar;
    case 0x05: return MTLVertexFormatHalf;
    case 0x06: return MTLVertexFormatUShortNormalized;
    case 0x07: return MTLVertexFormatShortNormalized;
    case 0x08: return MTLVertexFormatUShort;
    case 0x09: return MTLVertexFormatShort;
    case 0x0A: return MTLVertexFormatFloat;
    case 0x0B: return MTLVertexFormatUInt;
    case 0x0C: return MTLVertexFormatInt;
    case 0x0D: return MTLVertexFormatUChar2Normalized;
    case 0x0E: return MTLVertexFormatChar2Normalized;
    case 0x0F: return MTLVertexFormatUChar2;
    case 0x10: return MTLVertexFormatChar2;
    case 0x11: return MTLVertexFormatHalf2;
    case 0x12: return MTLVertexFormatUShort2Normalized;
    case 0x13: return MTLVertexFormatShort2Normalized;
    case 0x14: return MTLVertexFormatUShort2;
    case 0x15: return MTLVertexFormatShort2;
    case 0x16: return MTLVertexFormatFloat2;
    case 0x17: return MTLVertexFormatUInt2;
    case 0x18: return MTLVertexFormatInt2;
    case 0x19: return MTLVertexFormatUChar3Normalized;
    case 0x1A: return MTLVertexFormatChar3Normalized;
    case 0x1B: return MTLVertexFormatUChar3;
    case 0x1C: return MTLVertexFormatChar3;
    case 0x1D: return MTLVertexFormatHalf3;
    case 0x1E: return MTLVertexFormatUShort3Normalized;
    case 0x1F: return MTLVertexFormatShort3Normalized;
    case 0x20: return MTLVertexFormatUShort3;
    case 0x21: return MTLVertexFormatShort3;
    case 0x22: return MTLVertexFormatFloat3;
    case 0x23: return MTLVertexFormatUInt3;
    case 0x24: return MTLVertexFormatInt3;
    case 0x25: return MTLVertexFormatUChar4Normalized;
    case 0x26: return MTLVertexFormatChar4Normalized;
    case 0x27: return MTLVertexFormatUChar4;
    case 0x28: return MTLVertexFormatChar4;
    case 0x29: return MTLVertexFormatHalf4;
    case 0x2A: return MTLVertexFormatUShort4Normalized;
    case 0x2B: return MTLVertexFormatShort4Normalized;
    case 0x2C: return MTLVertexFormatUShort4;
    case 0x2D: return MTLVertexFormatShort4;
    case 0x2E: return MTLVertexFormatFloat4;
    case 0x2F: return MTLVertexFormatUInt4;
    case 0x30: return MTLVertexFormatInt4;
    case 0x3D: return MTLVertexFormatUInt1010102Normalized;
    case 0x69: return MTLVertexFormatInt1010102Normalized;
    case 0x3F: return MTLVertexFormatFloatRG11B10;
    case 0x40: return MTLVertexFormatFloatRGB9E5;
    // integer-to-float ("I2F") formats: converted to float by the vertex fetch
    case 0x51: return MTLVertexFormatUChar;   // these read as raw integers converted in shader
    default:
        return MTLVertexFormatFloat4;
    }
}

bool renderer_is_int_format(uint32_t f) { return renderer_attr_kind(f) != 0; }

MTLBlendFactor renderer_blend_factor(uint8_t f) {
    switch (f) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 0x10: return MTLBlendFactorSource1Color;
    case 0x11: return MTLBlendFactorOneMinusSource1Color;
    case 0x12: return MTLBlendFactorSource1Alpha;
    case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
    case 0x61: return MTLBlendFactorBlendColor;
    case 0x62: return MTLBlendFactorOneMinusBlendColor;
    case 0x63: return MTLBlendFactorBlendAlpha;
    case 0x64: return MTLBlendFactorOneMinusBlendAlpha;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[metal] unknown blend factor %#x\n", f);
        return MTLBlendFactorOne;
    }
    }
}

MTLBlendOperation renderer_blend_op(uint8_t e) {
    switch (e) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction renderer_compare_fn(uint8_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation renderer_stencil_op(uint8_t o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

int renderer_rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}
