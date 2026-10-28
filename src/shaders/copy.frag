#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// The SNES/NES picture out of the game texture's corner into an image of its own size (RGBA8), so
// the filters read clean edges (the texture holds older, bigger pictures around it).
// With parts (00.80: the DS's and the 3DS's screens placed by the app, GpuFilter.h Source::parts): up to two
// rectangles of the image, each copied to its own place in the picture, black elsewhere (the gap between the screens).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  const ivec2 p = ivec2(gl_FragCoord.xy);
  const uint parts = pc.Const3.z;
  if (parts == 0u)
  {
    FragColor = vec4(texelFetch(Source, p, 0).rgb, 1.0);
    return;
  }
  // A part: its place (x, y, width, height: Const0, Const2) and where it comes from (x, y: Const1, Const3).
  vec3 colour = vec3(0.0);
  ivec4 place = ivec4(pc.Const0);
  if (all(greaterThanEqual(p, place.xy)) && all(lessThan(p, place.xy + place.zw)))
    colour = texelFetch(Source, p - place.xy + ivec2(pc.Const1.xy), 0).rgb;
  if (parts > 1u)
  {
    place = ivec4(pc.Const2);
    if (all(greaterThanEqual(p, place.xy)) && all(lessThan(p, place.xy + place.zw)))
      colour = texelFetch(Source, p - place.xy + ivec2(pc.Const3.xy), 0).rgb;
  }
  FragColor = vec4(colour, 1.0);
}
