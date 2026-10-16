#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// The AI picture to the screen: Catmull-Rom (sharp bicubic) resampling with 5 bilinear taps: the
// 4x4 texels' weights folded into 3x3 linear fetches, without the four corner fetches (their
// weights are at most 0.0039 each), renormalized. Same look as the 9-tap form at about half the
// texture work (the pass to the screen is texture-bound on the PS4's GPU).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;  // linear

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  const vec2 sample_pos = vTexCoord * pc.SourceSize.xy;
  const vec2 tex_pos1 = floor(sample_pos - 0.5) + 0.5;
  const vec2 f = sample_pos - tex_pos1;
  const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  const vec2 w3 = f * f * (-0.5 + 0.5 * f);
  const vec2 w12 = w1 + w2;
  const vec2 offset12 = w2 / w12;
  const vec2 t0 = (tex_pos1 - 1.0) * pc.SourceSize.zw;
  const vec2 t3 = (tex_pos1 + 2.0) * pc.SourceSize.zw;
  const vec2 t12 = (tex_pos1 + offset12) * pc.SourceSize.zw;
  const float wt = w12.x * w0.y, wl = w0.x * w12.y, wc = w12.x * w12.y, wr = w3.x * w12.y, wb = w12.x * w3.y;
  vec3 c = texture(Source, vec2(t12.x, t0.y)).rgb * wt;
  c += texture(Source, vec2(t0.x, t12.y)).rgb * wl;
  c += texture(Source, vec2(t12.x, t12.y)).rgb * wc;
  c += texture(Source, vec2(t3.x, t12.y)).rgb * wr;
  c += texture(Source, vec2(t12.x, t3.y)).rgb * wb;
  FragColor = vec4(clamp(c / (wt + wl + wc + wr + wb), 0.0, 1.0), 1.0);
}
