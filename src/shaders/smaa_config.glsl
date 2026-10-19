// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// SMAA (src/third_party/smaa/SMAA.hlsl, MIT) as the GPU filters use it: GLSL, the pass's size from
// the push constants (all three passes draw an image as big as their first input), colour edge
// detection with SMAA's High preset searches and Ultra's threshold (pixel art's palettes have
// edges of little contrast), no predication. Its vertex shader parts run in the fragment shader
// (quad.vert is every pass's).
#define SMAA_GLSL_4 1
#define SMAA_RT_METRICS vec4(pc.SourceSize.zw, pc.SourceSize.xy)
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
// Edge detection writes every pixel instead of discarding those without edges.
#define discard
#include "../third_party/smaa/SMAA.hlsl"
