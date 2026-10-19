#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Smooth Edges / Natural HD / Natural AI, anti-aliasing step 3 of 3: SMAA's neighbourhood blending
// (Jimenez et al., SMAA.hlsl, MIT, see THIRD_PARTY_NOTICES.md): each pixel on an edge mixed with
// its neighbour as step 2 decided; the rest of the picture is left as it is.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "smaa_config.glsl"

layout(set = 0, binding = 0) uniform sampler2D ColorTex;  // the picture (linear)
layout(set = 0, binding = 1) uniform sampler2D BlendTex;  // step 2's weights (linear)

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  vec4 offset;
  SMAANeighborhoodBlendingVS(vTexCoord, offset);
  FragColor = vec4(SMAANeighborhoodBlendingPS(vTexCoord, offset, ColorTex, BlendTex).rgb, 1.0);
}
