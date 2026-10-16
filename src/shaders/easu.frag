#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Smooth HD, step 1: AMD FidelityFX Super Resolution 1's EASU (edge-adaptive upscaling; AMD's
// ffx_fsr1.h, MIT, see THIRD_PARTY_NOTICES.md) into an image of the picture's size on the screen.
// FP32 path: the PS4's GPU has no 16-bit float arithmetic.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D InputTexture;  // the picture (linear)

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(InputTexture, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(InputTexture, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(InputTexture, p, 2); }
#include "ffx_fsr1.h"

void main()
{
  AF3 color;
  FsrEasuF(color, AU2(gl_FragCoord.xy), pc.Const0, pc.Const1, pc.Const2, pc.Const3);
  FragColor = AF4(color, 1.0);
}
