#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Smooth Edges / Natural HD / Natural AI, anti-aliasing step 2 of 3: SMAA's blending weights
// (Jimenez et al., SMAA.hlsl and its area and search textures, MIT, see THIRD_PARTY_NOTICES.md):
// the shape of the edges around each pixel (L, Z, U and diagonal patterns) and how much of the
// neighbours to blend in.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "smaa_config.glsl"

layout(set = 0, binding = 0) uniform sampler2D EdgesTex;   // step 1's edges (linear)
layout(set = 0, binding = 1) uniform sampler2D AreaTex;    // SMAA's area texture (linear)
layout(set = 0, binding = 2) uniform sampler2D SearchTex;  // SMAA's search texture (nearest)

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  vec2 pixcoord;
  vec4 offset[3];
  SMAABlendingWeightCalculationVS(vTexCoord, pixcoord, offset);
  FragColor = SMAABlendingWeightCalculationPS(vTexCoord, pixcoord, offset, EdgesTex, AreaTex, SearchTex, vec4(0.0));
}
