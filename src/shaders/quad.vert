#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Every pass draws one quad (4 vertices as a triangle strip, no vertex buffer) over the picture's
// rectangle (Clip, in clip space), and gives the fragment shader the position across it (0..1).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec2 vTexCoord;

void main()
{
  vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  gl_Position = vec4(mix(pc.Clip.xy, pc.Clip.zw, corner), 0.0, 1.0);
  vTexCoord = corner;
}
