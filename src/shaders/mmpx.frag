#version 450
// Smooth Edges / Natural HD / Natural AI for pixel art, step 1: MMPX, the picture redrawn at 2x with
// its own colours only: 1:1 and 2:1 slopes filled in, corners rounded where the art has curves,
// square corners, thin lines, dithering and text kept as drawn.
//
// MMPX by Morgan McGuire and Mara Gagiu (2021, "MMPX Style-Preserving Pixel Art Magnification",
// JCGT). EmuSuNi's port of hunterk's GLSL adaptation in libretro's slang-shaders
// (edge-smoothing/scalenx/shaders/mmpx.slang): the same rules, with exact texel fetches.
//
// Copyright 2020 Morgan McGuire & Mara Gagiu. Available under the MIT license:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
// OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;  // the picture, its own size

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

ivec2 g_center;
ivec2 g_last;

vec3 src(int x, int y)
{
  return texelFetch(Source, clamp(g_center + ivec2(x, y), ivec2(0), g_last), 0).rgb;
}

float luma(vec3 col)
{
  return dot(col, vec3(0.2126, 0.7152, 0.0722));
}

bool same(vec3 B, vec3 A0)
{
  return all(equal(B, A0));
}

bool notsame(vec3 B, vec3 A0)
{
  return any(notEqual(B, A0));
}

bool all_eq2(vec3 B, vec3 A0, vec3 A1)
{
  return same(B, A0) && same(B, A1);
}

bool all_eq3(vec3 B, vec3 A0, vec3 A1, vec3 A2)
{
  return same(B, A0) && same(B, A1) && same(B, A2);
}

bool all_eq4(vec3 B, vec3 A0, vec3 A1, vec3 A2, vec3 A3)
{
  return same(B, A0) && same(B, A1) && same(B, A2) && same(B, A3);
}

bool any_eq3(vec3 B, vec3 A0, vec3 A1, vec3 A2)
{
  return same(B, A0) || same(B, A1) || same(B, A2);
}

bool none_eq2(vec3 B, vec3 A0, vec3 A1)
{
  return notsame(B, A0) && notsame(B, A1);
}

bool none_eq4(vec3 B, vec3 A0, vec3 A1, vec3 A2, vec3 A3)
{
  return notsame(B, A0) && notsame(B, A1) && notsame(B, A2) && notsame(B, A3);
}

void main()
{
  // Each source pixel becomes the 2x2 output pixels J K / L M.
  const ivec2 out_pixel = ivec2(gl_FragCoord.xy);
  g_center = out_pixel >> 1;
  g_last = ivec2(pc.SourceSize.xy) - 1;
  const ivec2 quadrant = out_pixel & 1;

  // The 3x3 grid A-I around E.
  const vec3 E = src(0, 0);
  const vec3 A = src(-1, -1);
  const vec3 B = src(0, -1);
  const vec3 C = src(1, -1);
  const vec3 D = src(-1, 0);
  const vec3 F = src(1, 0);
  const vec3 G = src(-1, 1);
  const vec3 H = src(0, 1);
  const vec3 I = src(1, 1);
  FragColor = vec4(E, 1.0);
  // A flat 3x3 area stays as it is (most of a picture).
  if (same(E, A) && same(E, B) && same(E, C) && same(E, D) && same(E, F) && same(E, G) && same(E, H) && same(E, I))
    return;

  vec3 J = E, K = E, L = E, M = E;
  // The tips of the diamond.
  const vec3 P = src(0, -2);
  const vec3 Q = src(-2, 0);
  const vec3 R = src(2, 0);
  const vec3 S = src(0, 2);
  const float Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

  // Round some corners and fill in 1:1 slopes, but keep sharp right angles (MMPX's rules: EPX's
  // test, no double fill on thick 1:1 lines, square corners kept, 1-pixel bumps on lines kept).
  if ((same(D, B) && notsame(D, H) && notsame(D, F)) && ((El >= Dl) || same(E, A)) && any_eq3(E, A, C, G) &&
      ((El < Dl) || notsame(A, D) || notsame(E, P) || notsame(E, Q)))
    J = D;
  if ((same(B, F) && notsame(B, D) && notsame(B, H)) && ((El >= Bl) || same(E, C)) && any_eq3(E, A, C, I) &&
      ((El < Bl) || notsame(C, B) || notsame(E, P) || notsame(E, R)))
    K = B;
  if ((same(H, D) && notsame(H, F) && notsame(H, B)) && ((El >= Hl) || same(E, G)) && any_eq3(E, A, G, I) &&
      ((El < Hl) || notsame(G, H) || notsame(E, S) || notsame(E, Q)))
    L = H;
  if ((same(F, H) && notsame(F, B) && notsame(F, D)) && ((El >= Fl) || same(E, I)) && any_eq3(E, C, G, I) &&
      ((El < Fl) || notsame(I, H) || notsame(E, R) || notsame(E, S)))
    M = F;

  // Clean up disconnected line intersections (not at the notch of a checkerboard dither).
  if ((notsame(E, F) && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && notsame(F, src(3, 0)))
    K = M = F;
  if ((notsame(E, D) && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && notsame(D, src(-3, 0)))
    J = L = D;
  if ((notsame(E, H) && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && notsame(H, src(0, 3)))
    L = M = H;
  if ((notsame(E, B) && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && notsame(B, src(0, -3)))
    J = K = B;

  // Remove the tips of bright triangles on dark backgrounds.
  if ((Bl < El) && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F))
    J = K = B;
  if ((Hl < El) && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F))
    L = M = H;
  if ((Fl < El) && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H))
    K = M = F;
  if ((Dl < El) && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H))
    J = L = D;

  // 2:1 and 1:2 slopes of one colour (extending what the rules above assigned).
  if (notsame(H, B))
  {
    if (notsame(H, A) && notsame(H, E) && notsame(H, C))
    {
      if (all_eq3(H, G, F, R) && none_eq2(H, D, src(2, -1)))
        L = M;
      if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(-2, -1)))
        M = L;
    }
    if (notsame(B, I) && notsame(B, G) && notsame(B, E))
    {
      if (all_eq3(B, A, F, R) && none_eq2(B, D, src(2, 1)))
        J = K;
      if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(-2, 1)))
        K = J;
    }
  }
  if (notsame(F, D))
  {
    if (notsame(D, I) && notsame(D, E) && notsame(D, C))
    {
      if (all_eq3(D, A, H, S) && none_eq2(D, B, src(1, 2)))
        J = L;
      if (all_eq3(D, G, B, P) && none_eq2(D, H, src(1, -2)))
        L = J;
    }
    if (notsame(F, E) && notsame(F, A) && notsame(F, G))
    {
      if (all_eq3(F, C, H, S) && none_eq2(F, B, src(-1, 2)))
        K = M;
      if (all_eq3(F, I, B, P) && none_eq2(F, H, src(-1, -2)))
        M = K;
    }
  }

  FragColor.rgb = quadrant.x == 0 ? (quadrant.y == 0 ? J : L) : (quadrant.y == 0 ? K : M);
}
