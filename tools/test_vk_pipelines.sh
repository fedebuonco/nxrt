#!/bin/sh
# GPU test of the Vulkan shader path (tools/test_vk_pipelines.c): fuzzed Maxwell programs -> GLSL -> the
# backend's compile/reflection/layouts -> real pipelines on the device, under the validation layer if present.
# usage: tools/test_vk_pipelines.sh [count] [seed]
set -e
cd "$(dirname "$0")/.."
OUT=build/test_vk_pipelines
mkdir -p "$OUT"
CC=${CC:-clang}
INC="-Iruntime -Iruntime/nvn -Iplatform -Isettings -Isave -Iinput -Iui -Ithird_party/smaa/Textures"
# (test_vk_stubs.c: the runtime symbols shaders.c needs that the test doesn't link; smaa_hlsl_src is a stub there)
$CC -O2 -w $INC -D_FILE_OFFSET_BITS=64 tools/test_vk_pipelines.c tools/test_vk_stubs.c \
    runtime/nvn/maxwell/translate.c runtime/nvn/vulkan/shaders.c \
    -o "$OUT/test_vk_pipelines" -lvulkan -lshaderc_shared -lm
"$OUT/test_vk_pipelines" runtime/nvn/maxwell/translate.c "${1:-2000}" "${2:-1}"
