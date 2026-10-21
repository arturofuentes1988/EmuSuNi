#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Tube TV (CRT), first step for a picture drawn bigger than the console's (HD 3D, a DS or PlayStation picture
// at 2x-4x): the picture made smaller (OutputSize) for the glass's glow, each texel the average of its block
// of the source's: four linear taps a quarter of the block from its middle (a 2x2 or 4x4 block exactly).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;  // linear

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  const vec2 block = pc.SourceSize.xy * pc.OutputSize.zw;  // the source's texels per texel here
  const vec2 middle = (floor(gl_FragCoord.xy) + 0.5) * block;
  const vec2 o = 0.25 * block;
  vec3 c = texture(Source, (middle + vec2(-o.x, -o.y)) * pc.SourceSize.zw).rgb;
  c += texture(Source, (middle + vec2(o.x, -o.y)) * pc.SourceSize.zw).rgb;
  c += texture(Source, (middle + vec2(-o.x, o.y)) * pc.SourceSize.zw).rgb;
  c += texture(Source, (middle + vec2(o.x, o.y)) * pc.SourceSize.zw).rgb;
  FragColor = vec4(c * 0.25, 1.0);
}
