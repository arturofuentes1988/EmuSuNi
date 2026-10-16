#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Smooth HD, step 2: AMD FidelityFX Super Resolution 1's RCAS (sharpening; AMD's ffx_fsr1.h, MIT,
// see THIRD_PARTY_NOTICES.md), reading EASU's image pixel for pixel, straight into the screen.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D InputTexture;  // EASU's image, as big as the picture

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p)
{
  // The picture's edges repeat (the screen's pixels around it aren't the picture).
  return texelFetch(InputTexture, clamp(p, ASU2(0, 0), textureSize(InputTexture, 0) - ASU2(1, 1)), 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main()
{
  AF3 color;
  FsrRcasF(color.r, color.g, color.b, AU2(gl_FragCoord.xy - pc.Origin.xy), pc.Const0);
  FragColor = AF4(color, 1.0);
}
