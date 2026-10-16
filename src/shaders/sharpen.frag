#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// HD Sharpen: the picture drawn smooth (bilinear), sharpened in the same pass with the lobe of AMD
// FidelityFX FSR 1's RCAS (MIT, see THIRD_PARTY_NOTICES.md): five smooth taps one screen pixel
// apart; edges get crisper without ringing.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;  // linear

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  const vec2 uv = vTexCoord;
  const vec2 d = pc.OutputSize.zw;  // one screen pixel, across the picture
  const vec3 e = texture(Source, uv).rgb;
  const vec3 b = texture(Source, uv - vec2(0.0, d.y)).rgb;
  const vec3 h = texture(Source, uv + vec2(0.0, d.y)).rgb;
  const vec3 l = texture(Source, uv - vec2(d.x, 0.0)).rgb;
  const vec3 r = texture(Source, uv + vec2(d.x, 0.0)).rgb;
  const vec3 mn4 = min(min(b, l), min(r, h));
  const vec3 mx4 = max(max(b, l), max(r, h));
  // RCAS: the most negative lobe that keeps every channel inside 0..1 (no clipping, no halos).
  const vec3 hit_min = min(mn4, e) / (4.0 * mx4 + 1.0e-5);
  const vec3 hit_max = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1.0e-5);
  const vec3 lobe_rgb = max(-hit_min, hit_max);
  const float limit = 0.25 - 1.0 / 16.0;
  const float lobe = max(-limit, min(max(lobe_rgb.r, max(lobe_rgb.g, lobe_rgb.b)), 0.0)) * pc.Origin.z;
  FragColor = vec4((lobe * (b + l + h + r) + e) / (4.0 * lobe + 1.0), 1.0);
}
