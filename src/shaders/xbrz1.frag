#version 450
// xBRZ freescale, pass 1 (EmuSuNi's port of RetroArch's slang-shaders
// edge-smoothing/xbrz/shaders/xbrz-freescale-multipass/xbrz-freescale-pass1.slang): the picture
// at any size, each pixel's corners blended as pass 0 decided.
//
// Hyllian's xBR-vertex code and texel mapping, Copyright (C) 2011/2016 Hyllian -
// sergiogdb@gmail.com (MIT license, see THIRD_PARTY_NOTICES.md).
// This shader also uses code and/or concepts from xBRZ as it appears in the Desmume source code:
// part of the HqMAME project, distributed under the GNU General Public License
// (http://www.gnu.org/licenses/gpl-3.0), Copyright (C) Zenju (zenju AT gmx DOT de).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;   // pass 0's blend information (nearest)
layout(set = 0, binding = 1) uniform sampler2D Picture;  // the picture (nearest)

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

#define BLEND_NONE 0
#define LUMINANCE_WEIGHT 1.0

float DistYCbCr(vec3 pixA, vec3 pixB)
{
  const vec3 w = vec3(0.2627, 0.6780, 0.0593);
  const float scaleB = 0.5 / (1.0 - w.b);
  const float scaleR = 0.5 / (1.0 - w.r);
  vec3 diff = pixA - pixB;
  float Y = dot(diff.rgb, w);
  float Cb = scaleB * (diff.b - Y);
  float Cr = scaleR * (diff.r - Y);
  return sqrt(((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr));
}

float get_left_ratio(vec2 center, vec2 origin, vec2 direction, vec2 scale)
{
  vec2 P0 = center - origin;
  vec2 proj = direction * (dot(P0, direction) / dot(direction, direction));
  vec2 distv = P0 - proj;
  vec2 orth = vec2(-direction.y, direction.x);
  float side = sign(dot(P0, orth));
  float v = side * length(distv * scale);
  return smoothstep(-sqrt(2.0) / 2.0, sqrt(2.0) / 2.0, v);
}

#define P(x, y) texture(Picture, coord + pc.SourceSize.zw * vec2(x, y)).rgb

void main()
{
  //---------------------------------------
  // Input Pixel Mapping: -|B|-
  //                      D|E|F
  //                      -|H|-
  vec2 scale = pc.OutputSize.xy * pc.SourceSize.zw;
  vec2 pos = fract(vTexCoord * pc.SourceSize.xy) - vec2(0.5, 0.5);
  vec2 coord = vTexCoord - pos * pc.SourceSize.zw;

  vec3 B = P(0, -1);
  vec3 D = P(-1, 0);
  vec3 E = P(0, 0);
  vec3 F = P(1, 0);
  vec3 H = P(0, 1);

  vec4 info = floor(texture(Source, coord) * 255.0 + 0.5);

  // info Mapping: x|y|
  //               w|z|
  vec4 blendResult = floor(mod(info, 4.0));
  vec4 doLineBlend = floor(mod(info / 4.0, 4.0));
  vec4 haveShallowLine = floor(mod(info / 16.0, 4.0));
  vec4 haveSteepLine = floor(mod(info / 64.0, 4.0));

  vec3 res = E;

  // Pixel Tap Mapping: -|-|-
  //                    -|E|F
  //                    -|H|-
  if (blendResult.z > BLEND_NONE)
  {
    vec2 origin = vec2(0.0, 1.0 / sqrt(2.0));
    vec2 direction = vec2(1.0, -1.0);
    if (doLineBlend.z > 0.0)
    {
      origin = haveShallowLine.z > 0.0 ? vec2(0.0, 0.25) : vec2(0.0, 0.5);
      direction.x += haveShallowLine.z;
      direction.y -= haveSteepLine.z;
    }
    vec3 blendPix = mix(H, F, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|-|-
  //                    D|E|-
  //                    -|H|-
  if (blendResult.w > BLEND_NONE)
  {
    vec2 origin = vec2(-1.0 / sqrt(2.0), 0.0);
    vec2 direction = vec2(1.0, 1.0);
    if (doLineBlend.w > 0.0)
    {
      origin = haveShallowLine.w > 0.0 ? vec2(-0.25, 0.0) : vec2(-0.5, 0.0);
      direction.y += haveShallowLine.w;
      direction.x += haveSteepLine.w;
    }
    vec3 blendPix = mix(H, D, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|B|-
  //                    -|E|F
  //                    -|-|-
  if (blendResult.y > BLEND_NONE)
  {
    vec2 origin = vec2(1.0 / sqrt(2.0), 0.0);
    vec2 direction = vec2(-1.0, -1.0);
    if (doLineBlend.y > 0.0)
    {
      origin = haveShallowLine.y > 0.0 ? vec2(0.25, 0.0) : vec2(0.5, 0.0);
      direction.y -= haveShallowLine.y;
      direction.x -= haveSteepLine.y;
    }
    vec3 blendPix = mix(F, B, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|B|-
  //                    D|E|-
  //                    -|-|-
  if (blendResult.x > BLEND_NONE)
  {
    vec2 origin = vec2(0.0, -1.0 / sqrt(2.0));
    vec2 direction = vec2(-1.0, 1.0);
    if (doLineBlend.x > 0.0)
    {
      origin = haveShallowLine.x > 0.0 ? vec2(0.0, -0.25) : vec2(0.0, -0.5);
      direction.x -= haveShallowLine.x;
      direction.y += haveSteepLine.x;
    }
    vec3 blendPix = mix(D, B, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  FragColor = vec4(res, 1.0);
}
