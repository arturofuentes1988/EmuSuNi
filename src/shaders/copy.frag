#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// The SNES/NES picture out of the game texture's corner into an image of its own size (RGBA8), so
// the filters read clean edges (the texture holds older, bigger pictures around it).

layout(set = 0, binding = 0) uniform sampler2D Source;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
  FragColor = vec4(texelFetch(Source, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
}
