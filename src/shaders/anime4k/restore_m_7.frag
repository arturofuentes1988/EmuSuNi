#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-3x1x1x56 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 7, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 1) uniform sampler2D tex_conv2d_tf;
#define conv2d_tf_pos vTexCoord
#define conv2d_tf_size vec2(textureSize(tex_conv2d_tf, 0))
#define conv2d_tf_pt (pc.SourceSize.zw)
#define conv2d_tf_tex(p) texture(tex_conv2d_tf, p)
#define conv2d_tf_texOff(off) texture(tex_conv2d_tf, vTexCoord + conv2d_tf_pt * (off))
layout(set = 0, binding = 2) uniform sampler2D tex_conv2d_1_tf;
#define conv2d_1_tf_pos vTexCoord
#define conv2d_1_tf_size vec2(textureSize(tex_conv2d_1_tf, 0))
#define conv2d_1_tf_pt (pc.SourceSize.zw)
#define conv2d_1_tf_tex(p) texture(tex_conv2d_1_tf, p)
#define conv2d_1_tf_texOff(off) texture(tex_conv2d_1_tf, vTexCoord + conv2d_1_tf_pt * (off))
layout(set = 0, binding = 3) uniform sampler2D tex_conv2d_2_tf;
#define conv2d_2_tf_pos vTexCoord
#define conv2d_2_tf_size vec2(textureSize(tex_conv2d_2_tf, 0))
#define conv2d_2_tf_pt (pc.SourceSize.zw)
#define conv2d_2_tf_tex(p) texture(tex_conv2d_2_tf, p)
#define conv2d_2_tf_texOff(off) texture(tex_conv2d_2_tf, vTexCoord + conv2d_2_tf_pt * (off))
layout(set = 0, binding = 4) uniform sampler2D tex_conv2d_3_tf;
#define conv2d_3_tf_pos vTexCoord
#define conv2d_3_tf_size vec2(textureSize(tex_conv2d_3_tf, 0))
#define conv2d_3_tf_pt (pc.SourceSize.zw)
#define conv2d_3_tf_tex(p) texture(tex_conv2d_3_tf, p)
#define conv2d_3_tf_texOff(off) texture(tex_conv2d_3_tf, vTexCoord + conv2d_3_tf_pt * (off))
layout(set = 0, binding = 5) uniform sampler2D tex_conv2d_4_tf;
#define conv2d_4_tf_pos vTexCoord
#define conv2d_4_tf_size vec2(textureSize(tex_conv2d_4_tf, 0))
#define conv2d_4_tf_pt (pc.SourceSize.zw)
#define conv2d_4_tf_tex(p) texture(tex_conv2d_4_tf, p)
#define conv2d_4_tf_texOff(off) texture(tex_conv2d_4_tf, vTexCoord + conv2d_4_tf_pt * (off))
layout(set = 0, binding = 6) uniform sampler2D tex_conv2d_5_tf;
#define conv2d_5_tf_pos vTexCoord
#define conv2d_5_tf_size vec2(textureSize(tex_conv2d_5_tf, 0))
#define conv2d_5_tf_pt (pc.SourceSize.zw)
#define conv2d_5_tf_tex(p) texture(tex_conv2d_5_tf, p)
#define conv2d_5_tf_texOff(off) texture(tex_conv2d_5_tf, vTexCoord + conv2d_5_tf_pt * (off))
layout(set = 0, binding = 7) uniform sampler2D tex_conv2d_6_tf;
#define conv2d_6_tf_pos vTexCoord
#define conv2d_6_tf_size vec2(textureSize(tex_conv2d_6_tf, 0))
#define conv2d_6_tf_pt (pc.SourceSize.zw)
#define conv2d_6_tf_tex(p) texture(tex_conv2d_6_tf, p)
#define conv2d_6_tf_texOff(off) texture(tex_conv2d_6_tf, vTexCoord + conv2d_6_tf_pt * (off))
#define HOOKED_pos MAIN_pos
#define HOOKED_size MAIN_size
#define HOOKED_pt MAIN_pt
#define HOOKED_tex MAIN_tex
#define HOOKED_texOff MAIN_texOff
#define g_0 (max((conv2d_tf_tex(conv2d_tf_pos)), 0.0))
#define g_1 (max(-(conv2d_tf_tex(conv2d_tf_pos)), 0.0))
#define g_2 (max((conv2d_1_tf_tex(conv2d_1_tf_pos)), 0.0))
#define g_3 (max(-(conv2d_1_tf_tex(conv2d_1_tf_pos)), 0.0))
#define g_4 (max((conv2d_2_tf_tex(conv2d_2_tf_pos)), 0.0))
#define g_5 (max(-(conv2d_2_tf_tex(conv2d_2_tf_pos)), 0.0))
#define g_6 (max((conv2d_3_tf_tex(conv2d_3_tf_pos)), 0.0))
#define g_7 (max(-(conv2d_3_tf_tex(conv2d_3_tf_pos)), 0.0))
#define g_8 (max((conv2d_4_tf_tex(conv2d_4_tf_pos)), 0.0))
#define g_9 (max(-(conv2d_4_tf_tex(conv2d_4_tf_pos)), 0.0))
#define g_10 (max((conv2d_5_tf_tex(conv2d_5_tf_pos)), 0.0))
#define g_11 (max(-(conv2d_5_tf_tex(conv2d_5_tf_pos)), 0.0))
#define g_12 (max((conv2d_6_tf_tex(conv2d_6_tf_pos)), 0.0))
#define g_13 (max(-(conv2d_6_tf_tex(conv2d_6_tf_pos)), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(-0.010478934, -0.008364784, -0.010246552, 0.0);
    t = conv2d_tf_tex(conv2d_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(-0.026375166, -0.008920076, 0.033379757) * p.x;
    result.rgb += vec3(0.016392392, 0.0092047293, -0.013118202) * p.y;
    result.rgb += vec3(0.069770233, 0.17301392, 0.195704072) * p.z;
    result.rgb += vec3(-0.102124593, -0.049952139, -0.063939545) * p.w;
    result.rgb += vec3(-0.061996464, -0.05631466, -0.06808407) * t.x;
    result.rgb += vec3(0.005013109, 0.0044589997, 0.032367796) * t.y;
    result.rgb += vec3(-0.016481603, -0.13721058, -0.14924648) * t.z;
    result.rgb += vec3(-0.020035887, 0.07250003, 0.08034037) * t.w;
    t = conv2d_1_tf_tex(conv2d_1_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(0.11076139, 0.094095282, 0.071242694) * p.x;
    result.rgb += vec3(0.168321036, 0.150968924, 0.117170153) * p.y;
    result.rgb += vec3(-0.01266186, 0.08417419, 0.061868059) * p.z;
    result.rgb += vec3(0.02146094, 0.00589634, 0.00971052) * p.w;
    result.rgb += vec3(0.13002375, -0.012733757, -0.017821986) * t.x;
    result.rgb += vec3(-0.17767483, -0.20204604, -0.1751779) * t.y;
    result.rgb += vec3(-0.12804912, -0.07381453, -0.05655911) * t.z;
    result.rgb += vec3(-0.17044514, -0.07301451, -0.06523978) * t.w;
    t = conv2d_2_tf_tex(conv2d_2_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(0.00384664, 0.04388038, 0.045932276) * p.x;
    result.rgb += vec3(-0.104240688, -0.068298548, -0.063341325) * p.y;
    result.rgb += vec3(0.031365925, 0.052951932, 0.044420119) * p.z;
    result.rgb += vec3(0.03640034, 0.008291118, 0.00229913) * p.w;
    result.rgb += vec3(-0.12094524, -0.09518409, -0.07387219) * t.x;
    result.rgb += vec3(-0.062216382, -0.053228356, -0.031372335) * t.y;
    result.rgb += vec3(-0.072797105, -0.026258165, -0.009804673) * t.z;
    result.rgb += vec3(-0.120719045, -0.073281154, -0.056623302) * t.w;
    t = conv2d_3_tf_tex(conv2d_3_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(-0.053324755, -0.04027522, -0.044264335) * p.x;
    result.rgb += vec3(-0.020401013, -0.02607949, -0.026701555) * p.y;
    result.rgb += vec3(0.016182185, 0.006567069, 0.000702196) * p.z;
    result.rgb += vec3(-0.036519819, -0.0562541373, -0.0472272204) * p.w;
    result.rgb += vec3(-0.058090195, -0.07538767, -0.059722915) * t.x;
    result.rgb += vec3(-0.044788487, -0.04212742, -0.027502589) * t.y;
    result.rgb += vec3(-0.04892866, -0.015416752, -0.008312418) * t.z;
    result.rgb += vec3(0.011864114, 0.0074752793, 0.0060824654) * t.w;
    t = conv2d_4_tf_tex(conv2d_4_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(0.039643008, 0.0628619186, 0.0569850069) * p.x;
    result.rgb += vec3(0.038279745, 0.061093995, 0.052868325) * p.y;
    result.rgb += vec3(-0.057998268, -0.05380127, -0.049544127) * p.z;
    result.rgb += vec3(0.01074113, 0.016923446, 0.024009737) * p.w;
    result.rgb += vec3(0.003803544, -0.0008906116, 0.00059585314) * t.x;
    result.rgb += vec3(-0.102071285, -0.11485224, -0.10007254) * t.y;
    result.rgb += vec3(0.074306004, 0.08803551, 0.07972321) * t.z;
    result.rgb += vec3(0.030704215, 0.021514274, 0.009049376) * t.w;
    t = conv2d_5_tf_tex(conv2d_5_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(0.0331652886, 0.0430461038, 0.0402372706) * p.x;
    result.rgb += vec3(0.009306695, 0.006859321, 0.010263355) * p.y;
    result.rgb += vec3(0.08939367, 0.096228662, 0.091365253) * p.z;
    result.rgb += vec3(0.111165201, 0.127053417, 0.11620872) * p.w;
    result.rgb += vec3(-0.02655948, -0.041905303, -0.03861737) * t.x;
    result.rgb += vec3(-0.048471425, -0.049788587, -0.050447535) * t.y;
    result.rgb += vec3(-0.12092813, -0.13564217, -0.12613249) * t.z;
    result.rgb += vec3(0.0023508538, -0.0012828974, -0.0028730957) * t.w;
    t = conv2d_6_tf_tex(conv2d_6_tf_pos);
    p = max(t, 0.0);
    result.rgb += vec3(0.115238838, 0.127209597, 0.114390824) * p.x;
    result.rgb += vec3(-0.114926123, -0.13099212, -0.124647733) * p.y;
    result.rgb += vec3(-0.06257204, -0.084035847, -0.079285036) * p.z;
    result.rgb += vec3(-0.11914884, -0.122259005, -0.104514744) * p.w;
    result.rgb += vec3(-0.10676299, -0.118409514, -0.10618478) * t.x;
    result.rgb += vec3(0.05880252, 0.06488367, 0.06432695) * t.y;
    result.rgb += vec3(-0.019221924, -0.017602798, -0.017413978) * t.z;
    result.rgb += vec3(0.07512528, 0.080483615, 0.066218294) * t.w;
    return result + MAIN_tex(MAIN_pos);
}
void main()
{
  FragColor = hook();
}
