#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x8 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 3, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 0) uniform sampler2D tex_conv2d_2_tf;
#define conv2d_2_tf_pos vTexCoord
#define conv2d_2_tf_size vec2(textureSize(tex_conv2d_2_tf, 0))
#define conv2d_2_tf_pt (pc.SourceSize.zw)
#define conv2d_2_tf_tex(p) texture(tex_conv2d_2_tf, p)
#define conv2d_2_tf_texOff(off) texture(tex_conv2d_2_tf, vTexCoord + conv2d_2_tf_pt * (off))
#define go_0(x_off, y_off) (max((conv2d_2_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_2_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(-0.059377354, -0.02055341, 0.07234869, -0.015452986);
    t = conv2d_2_tf_texOff(vec2(-1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.00886304, -0.123601956, -0.02776337, 0.727273535) * p.x;
    result += vec4(-0.00486936, 0.06193162, -0.00632784, -0.43662) * p.y;
    result += vec4(0.05073435, 0.348640385, -0.18916533, -0.34841238) * p.z;
    result += vec4(0.04167137, -0.23192488, -0.13354743, 0.326327487) * p.w;
    result += vec4(-0.23263514, 0.11719232, -0.2903209, 0.007503795) * t.x;
    result += vec4(0.020222448, 0.17790157, 0.15600762, 0.08741775) * t.y;
    result += vec4(-0.12529704, -0.25548857, 0.04585447, 0.10255033) * t.z;
    result += vec4(-0.18350503, 0.29593533, -0.0868933, -0.027004737) * t.w;
    t = conv2d_2_tf_texOff(vec2(-1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.22927163, -0.341153435, -0.12760352, 0.28329069) * p.x;
    result += vec4(0.23900747, -0.04596563, 0.31403969, 0.133709168) * p.y;
    result += vec4(0.35779872, 0.14332872, -0.48545791, -0.310600253) * p.z;
    result += vec4(-0.151215445, -0.07380491, 0.0977777, -0.25007601) * p.w;
    result += vec4(0.14958654, 0.006238835, 0.2928948, -0.1988557) * t.x;
    result += vec4(0.17057803, -0.12524141, -0.13978264, 0.019280292) * t.y;
    result += vec4(-0.05967142, 0.07790818, 0.5893818, 0.022845713) * t.z;
    result += vec4(0.08596779, -0.07875358, 0.03316667, 0.4369282) * t.w;
    t = conv2d_2_tf_texOff(vec2(-1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.207663617, -0.238434042, 0.00327432, 0.182690546) * p.x;
    result += vec4(-0.067934484, -0.381247077, -0.227474276, -0.000530868) * p.y;
    result += vec4(-0.073492142, -0.291662732, -0.48235034, -0.189290835) * p.z;
    result += vec4(0.35039593, -0.148897864, -0.001798525, -0.0003435414) * p.w;
    result += vec4(-0.19195688, 0.060883682, 0.25897828, -0.07063324) * t.x;
    result += vec4(-0.090833396, -0.003422883, -0.109534174, -0.031180874) * t.y;
    result += vec4(0.05017118, -0.022862168, 0.270113, 0.057831235) * t.z;
    result += vec4(-0.53920543, 0.10252776, 0.091807485, -0.004294343) * t.w;
    t = conv2d_2_tf_texOff(vec2(0.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.49621748, 0.193758424, 0.34253218, 0.11426997) * p.x;
    result += vec4(0.125829225, -0.22747213, -0.224011302, -0.093376566) * p.y;
    result += vec4(0.02727434, 0.3631113, -0.194130116, 0.37105858) * p.z;
    result += vec4(-0.012289452, -0.072631934, -0.291930424, -0.489106189) * p.w;
    result += vec4(0.18494242, 0.119284816, -0.3821897, -0.07777979) * t.x;
    result += vec4(-0.15568028, 0.2854859, 0.22441281, 0.049155876) * t.y;
    result += vec4(0.15292497, -0.21895619, 0.095677756, -0.15210424) * t.z;
    result += vec4(-0.001643022, 0.026176987, -0.048463076, 0.4824009) * t.w;
    t = conv2d_2_tf_texOff(vec2(0.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.341571341, -0.34218497, 0.114181027, -0.190556146) * p.x;
    result += vec4(0.03464188, -0.0643159, -0.242401093, 0.05520809) * p.y;
    result += vec4(0.16155126, 0.19523166, -0.24096939, -0.0218428) * p.z;
    result += vec4(0.14011982, -0.15021299, -0.07635394, 0.0183971315) * p.w;
    result += vec4(-0.007215129, -0.17074333, -0.053930074, 0.027014816) * t.x;
    result += vec4(0.17180431, 0.15163863, 0.0012122132, 0.18934256) * t.y;
    result += vec4(0.08294297, 0.24580221, 0.46552867, 0.27923223) * t.z;
    result += vec4(-0.4092668, -0.06288688, 0.1602188, 0.0030876845) * t.w;
    t = conv2d_2_tf_texOff(vec2(0.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.054415982, -0.37902075, 0.164214526, 0.57506777) * p.x;
    result += vec4(-0.01772723, 0.19023802, 0.062210545, 0.13489432) * p.y;
    result += vec4(0.29820202, -0.072851134, -0.31151276, -0.207414045) * p.z;
    result += vec4(0.29719645, 0.27505637, -0.07242136, 0.16410099) * p.w;
    result += vec4(-0.111870885, -0.03317145, -0.14155298, -0.20328505) * t.x;
    result += vec4(0.05104131, -0.13979794, -0.018966835, 0.07238511) * t.y;
    result += vec4(-0.05493792, 0.14975783, 0.10293237, 0.21985306) * t.z;
    result += vec4(-0.49054706, -0.18288186, 0.26925826, -0.35845932) * t.w;
    t = conv2d_2_tf_texOff(vec2(1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.20517494, 0.149413184, 0.20837736, 0.39614428) * p.x;
    result += vec4(-0.185742905, -0.131583248, -0.08393917, -0.22183397) * p.y;
    result += vec4(-0.197202373, 0.20544746, 0.04511873, -0.253678382) * p.z;
    result += vec4(-0.00770044, 0.0227081, -0.26295233, -0.096115033) * p.w;
    result += vec4(-0.3747799, 0.096748486, 0.17139742, -0.25289854) * t.x;
    result += vec4(0.17421168, 0.018461818, -0.09747162, -0.01660535) * t.y;
    result += vec4(0.20580359, -0.56189656, -0.17151354, 0.26347768) * t.z;
    result += vec4(-0.28350568, 0.21486014, 0.44330928, 0.008981037) * t.w;
    t = conv2d_2_tf_texOff(vec2(1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.00088746, 0.10947476, 0.15195429, 0.49988279) * p.x;
    result += vec4(-0.085690101, 0.080537761, 0.143374961, -0.2021645) * p.y;
    result += vec4(-0.00997653, 0.391411023, -0.412188536, -0.062891539) * p.z;
    result += vec4(0.26851029, 0.26389755, -0.06445898, 0.0518791) * p.w;
    result += vec4(-0.10169985, 0.18244018, -0.04760736, -0.41017643) * t.x;
    result += vec4(0.09468786, 0.024218475, -0.103733875, 0.22540338) * t.y;
    result += vec4(-0.10630112, -0.3677178, 0.104170956, -0.057317447) * t.z;
    result += vec4(-0.21764882, -0.0789158, 0.22041337, -0.15065216) * t.w;
    t = conv2d_2_tf_texOff(vec2(1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.217661625, -0.301192714, 0.24308884, 0.63221785) * p.x;
    result += vec4(0.017673905, 0.086106071, -0.08784917, -0.23661915) * p.y;
    result += vec4(0.15688602, 0.193676475, -0.114945696, 0.156930245) * p.z;
    result += vec4(-0.141637079, 0.063685246, -0.09248089, -0.299780026) * p.w;
    result += vec4(-0.11633995, 0.008195114, 0.14501533, -0.07168025) * t.x;
    result += vec4(-0.058413275, -0.055995367, -0.09362145, 0.13827963) * t.y;
    result += vec4(-0.13760869, -0.040319785, -0.038895044, -0.2675253) * t.z;
    result += vec4(0.087339684, -0.1412073, 0.17166458, 0.2312994) * t.w;
    return result;
}
void main()
{
  FragColor = hook();
}
