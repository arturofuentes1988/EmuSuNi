#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Smooth Edges / Natural HD / Natural AI, anti-aliasing step 1 of 3: SMAA's colour edge detection
// (Jimenez et al., SMAA.hlsl, MIT, see THIRD_PARTY_NOTICES.md) at the picture's size: which sides
// of each pixel are edges (RG). Every pixel is written (no discard: the image isn't cleared).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "smaa_config.glsl"

layout(set = 0, binding = 0) uniform sampler2D ColorTex;  // the picture

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  vec4 offset[3];
  SMAAEdgeDetectionVS(vTexCoord, offset);
  FragColor = vec4(SMAAColorEdgeDetectionPS(vTexCoord, offset, ColorTex), 0.0, 1.0);
}
