#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x3 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 0, made by scripts/make-anime4k.py)
// Rewritten for GCN (same network and weights): CReLU as (A+B)*max(t,0) - B*t, products
// accumulated column by column, the texel size from the push constants.
// MIT License
//
// Copyright (c) 2019-2021 bloc97
// All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D tex_MAIN;
#define MAIN_pos vTexCoord
#define MAIN_size vec2(textureSize(tex_MAIN, 0))
#define MAIN_pt (pc.SourceSize.zw)
#define MAIN_tex(p) texture(tex_MAIN, p)
#define MAIN_texOff(off) texture(tex_MAIN, vTexCoord + MAIN_pt * (off))
#define HOOKED_pos MAIN_pos
#define HOOKED_size MAIN_size
#define HOOKED_pt MAIN_pt
#define HOOKED_tex MAIN_tex
#define HOOKED_texOff MAIN_texOff
#define go_0(x_off, y_off) (MAIN_texOff(vec2(x_off, y_off)))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(-0.061233472, 0.39222646, 0.029704979, 0.02586828);
    t = MAIN_texOff(vec2(-1.0, -1.0));
    result += vec4(-0.09991986, 0.13782342, -0.031251684, -0.06356843) * t.x;
    result += vec4(-0.3437488, 0.05450952, 0.34347802, 0.46335372) * t.y;
    result += vec4(0.08607224, 0.044988394, 0.137179, 0.17976908) * t.z;
    t = MAIN_texOff(vec2(-1.0, 0.0));
    result += vec4(-0.024212424, -0.09278509, -0.00040907756, 0.34552294) * t.x;
    result += vec4(-0.13254678, 0.113105185, 0.005667946, -0.00036919137) * t.y;
    result += vec4(-0.06375679, 0.009184115, 0.115518734, -0.115506776) * t.z;
    t = MAIN_texOff(vec2(-1.0, 1.0));
    result += vec4(-0.14101827, 0.023523493, 0.044094566, -0.019271746) * t.x;
    result += vec4(-0.44348842, -0.08818877, -0.4026149, -0.21995795) * t.y;
    result += vec4(-0.15880394, -0.013732858, -0.020751135, 0.012719151) * t.z;
    t = MAIN_texOff(vec2(0.0, -1.0));
    result += vec4(0.013001821, -0.34503505, 0.39219138, 0.18792126) * t.x;
    result += vec4(0.24760444, -0.016173402, 0.10154511, 0.15453082) * t.y;
    result += vec4(-0.058132876, 0.016784398, -0.05808539, -0.11039915) * t.z;
    t = MAIN_texOff(vec2(0.0, 0.0));
    result += vec4(0.37024534, 0.041440863, -0.3374568, -0.44994286) * t.x;
    result += vec4(0.19555596, 0.20855539, -0.27974075, -0.5372628) * t.y;
    result += vec4(0.21228147, -0.0295346, -0.56700057, 0.030042822) * t.z;
    t = MAIN_texOff(vec2(0.0, 1.0));
    result += vec4(-0.12940632, 0.057526, 0.090682045, -0.06985033) * t.x;
    result += vec4(-0.13704006, -0.047685407, 0.44615674, -0.48056605) * t.y;
    result += vec4(-0.06166251, -0.01883519, 0.2032237, -0.113287605) * t.z;
    t = MAIN_texOff(vec2(1.0, -1.0));
    result += vec4(0.010856669, -0.35820737, 0.16757219, 0.082619876) * t.x;
    result += vec4(-0.03967303, 0.038705572, 0.32652855, -0.012030017) * t.y;
    result += vec4(0.015120559, -0.15314877, 0.23442009, 0.09767922) * t.z;
    t = MAIN_texOff(vec2(1.0, 0.0));
    result += vec4(-0.046272673, -0.17752305, 0.082018286, -0.2512824) * t.x;
    result += vec4(0.58619463, -0.060903464, -0.022793597, 0.077803515) * t.y;
    result += vec4(-0.17025311, 0.05136993, 0.029383298, -0.15475409) * t.z;
    t = MAIN_texOff(vec2(1.0, 1.0));
    result += vec4(-0.11212024, 0.13378005, -0.2027488, 0.08056421) * t.x;
    result += vec4(-0.11176219, -0.048429377, -0.08396386, 0.10507829) * t.y;
    result += vec4(0.13326839, 0.0430627, 0.051362377, 0.06482755) * t.z;
    return result;
}
void main()
{
  FragColor = hook();
}
