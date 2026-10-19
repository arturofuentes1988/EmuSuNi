#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Smooth Edges for pixel art, last step: the anti-aliased 2x picture to the screen as crisp
// pixels: nearest inside each pixel, a linear ramp one screen pixel wide across its borders (every
// pixel as wide as its neighbours at any size, no blur, no shimmer).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;  // linear

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  const vec2 texel = vTexCoord * pc.SourceSize.xy;
  const vec2 corner = floor(texel - 0.5);
  const vec2 phase = texel - 0.5 - corner;
  const vec2 pixels_per_texel = pc.OutputSize.xy * pc.SourceSize.zw;
  const vec2 ramp = clamp((phase - 0.5) * pixels_per_texel + 0.5, 0.0, 1.0);
  FragColor = vec4(texture(Source, (corner + 0.5 + ramp) * pc.SourceSize.zw).rgb, 1.0);
}
