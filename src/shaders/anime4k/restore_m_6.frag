#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x8 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 6, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 0) uniform sampler2D tex_conv2d_5_tf;
#define conv2d_5_tf_pos vTexCoord
#define conv2d_5_tf_size vec2(textureSize(tex_conv2d_5_tf, 0))
#define conv2d_5_tf_pt (pc.SourceSize.zw)
#define conv2d_5_tf_tex(p) texture(tex_conv2d_5_tf, p)
#define conv2d_5_tf_texOff(off) texture(tex_conv2d_5_tf, vTexCoord + conv2d_5_tf_pt * (off))
#define go_0(x_off, y_off) (max((conv2d_5_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_5_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(0.013687534, -0.08185164, -0.04755438, 0.290178);
    t = conv2d_5_tf_texOff(vec2(-1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.43783639, -0.18190875, 0.344889208, -0.12784807) * p.x;
    result += vec4(-0.05793172, 0.347731403, -0.46383757, -0.24878107) * p.y;
    result += vec4(0.210062895, 0.10629296, 0.32607427, 0.170389595) * p.z;
    result += vec4(0.07189784, -0.122952175, 0.06542087, 0.00419761) * p.w;
    result += vec4(0.21030277, 0.09578802, -0.013482288, 0.21484336) * t.x;
    result += vec4(-0.12995781, -0.40431052, 0.3347856, 0.18183486) * t.y;
    result += vec4(-0.15550353, 0.04402301, -0.4603779, -0.14874357) * t.z;
    result += vec4(0.07694621, 0.053523075, 0.19607326, 0.10850742) * t.w;
    t = conv2d_5_tf_texOff(vec2(-1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.05595348, 0.17336182, 0.04937383, 0.02122943) * p.x;
    result += vec4(0.30403325, 0.213829164, -0.34126992, -0.17454328) * p.y;
    result += vec4(0.05403237, 0.40445223, 0.131413258, -0.090285845) * p.z;
    result += vec4(-0.145183351, 0.03028448, 0.27864851, 0.10983086) * p.w;
    result += vec4(0.2347211, -0.2697403, 0.0634794, 0.17925987) * t.x;
    result += vec4(-0.17231455, -0.24999185, 0.5208536, 0.10491828) * t.y;
    result += vec4(0.233575, -0.52950364, -0.0038063182, 0.1380038) * t.z;
    result += vec4(-0.022935199, -0.19369157, -0.14586553, -0.1938704) * t.w;
    t = conv2d_5_tf_texOff(vec2(-1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.32162746, 0.421213774, -0.02780378, 0.08058651) * p.x;
    result += vec4(0.58979199, 0.099737946, -0.471266156, 0.0658617) * p.y;
    result += vec4(0.156626818, -0.06301719, -0.05961605, -0.1187315) * p.z;
    result += vec4(-0.278838065, -0.34913864, 0.36030969, -0.11165284) * p.w;
    result += vec4(0.10245223, -0.34150192, -0.25862157, 0.20165509) * t.x;
    result += vec4(-0.5597771, -0.114510864, 0.122526556, 0.04010975) * t.y;
    result += vec4(-0.1704679, 0.23335956, 0.16771887, 0.03783455) * t.z;
    result += vec4(0.056995615, -0.24153493, 0.08082429, 0.24210933) * t.w;
    t = conv2d_5_tf_texOff(vec2(0.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.402535846, 0.166707229, -0.10214924, -0.199222576) * p.x;
    result += vec4(-0.036030915, 0.23866251, 0.0453203, -0.220543666) * p.y;
    result += vec4(0.01383404, 0.18528028, 0.204621915, -0.088204723) * p.z;
    result += vec4(-0.30295435, -0.24142469, 0.01497084, 0.02271647) * p.w;
    result += vec4(0.103466526, -0.15278348, 0.30526164, 0.080755696) * t.x;
    result += vec4(-0.103505425, -0.15862796, -0.14696524, 0.008358076) * t.y;
    result += vec4(0.09180311, 0.12505089, -0.28052542, 0.13551563) * t.z;
    result += vec4(-0.07528779, 0.09636086, 0.10369617, -0.23656134) * t.w;
    t = conv2d_5_tf_texOff(vec2(0.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.39291326, 0.292025815, -0.243254896, -0.169301075) * p.x;
    result += vec4(0.295958836, 0.39759428, -0.2408077, 0.12468755) * p.y;
    result += vec4(0.273158545, 0.09155132, 0.34892547, -0.2632077) * p.z;
    result += vec4(0.123230357, 0.320978486, 0.31738426, -0.2475147) * p.w;
    result += vec4(0.25752836, -0.099439755, 0.30716348, -0.035077725) * t.x;
    result += vec4(-0.023509016, -0.23106368, -0.05277125, -0.34910464) * t.y;
    result += vec4(-0.088015385, -0.26995596, -0.1390645, 0.40671825) * t.z;
    result += vec4(-0.18096298, 0.100688554, -0.5492049, -0.2482101) * t.w;
    t = conv2d_5_tf_texOff(vec2(0.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.06581649, 0.001866199, -0.42099345, 0.089408672) * p.x;
    result += vec4(0.14846829, 0.1060527, -0.132713856, -0.00484293) * p.y;
    result += vec4(0.19881141, -0.24757856, -0.499952892, -0.09147096) * p.z;
    result += vec4(-0.06289887, -0.361641, -0.039657477, 0.11497477) * p.w;
    result += vec4(-0.41411775, 0.107200556, 0.13813478, -0.13768874) * t.x;
    result += vec4(-0.27137747, -0.06313619, 0.08522967, -0.03218302) * t.y;
    result += vec4(0.03166121, 0.3415683, 0.52242, 0.1741813) * t.z;
    result += vec4(0.36956537, -0.179129, 0.09742935, 0.11696616) * t.w;
    t = conv2d_5_tf_texOff(vec2(1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.25707452, -0.13794432, 0.51649813, 0.25461551) * p.x;
    result += vec4(-0.02123609, 0.27639425, -0.13081694, -0.26494858) * p.y;
    result += vec4(0.19785286, -0.15330616, -0.147710398, -0.13418456) * p.z;
    result += vec4(0.35512498, -0.422825, 0.27885512, 0.06670546) * p.w;
    result += vec4(0.07975504, -0.17964838, -0.37122533, -0.16064765) * t.x;
    result += vec4(-0.14309953, -0.29473078, -0.0926391, 0.22333665) * t.y;
    result += vec4(-0.34612748, 0.3387473, -0.0077308523, 0.07239449) * t.z;
    result += vec4(-0.18522519, 0.21297298, -0.11493978, -0.16117814) * t.w;
    t = conv2d_5_tf_texOff(vec2(1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.227906652, -0.11011472, 0.140953584, 0.229693884) * p.x;
    result += vec4(0.50360517, 0.100016427, -0.178301388, -0.118713498) * p.y;
    result += vec4(-0.41549082, 0.05646791, -0.16507771, 0.09328684) * p.z;
    result += vec4(-0.01623578, -0.18248655, -0.13422258, 0.02380251) * p.w;
    result += vec4(0.17402779, -0.10023144, -0.11712206, -0.031971734) * t.x;
    result += vec4(-0.18713303, -0.08736295, -0.013007052, 0.06943139) * t.y;
    result += vec4(0.20102951, 0.010721135, 0.2562522, -0.34877458) * t.z;
    result += vec4(0.13732676, 0.40258047, -0.25824392, -0.15720639) * t.w;
    t = conv2d_5_tf_texOff(vec2(1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.116472535, 0.14465675, 0.105862165, 0.0951901933) * p.x;
    result += vec4(0.38655747, 0.355855543, -0.196697345, -0.135324247) * p.y;
    result += vec4(-0.519394595, 0.32062281, 0.28690485, -0.27725086) * p.z;
    result += vec4(-0.06935653, -0.24981562, 0.647753376, 0.00044889) * p.w;
    result += vec4(-0.044494305, -0.3296108, -0.0017603852, -0.09362289) * t.x;
    result += vec4(-0.38839245, -0.40015858, 0.13395199, 0.044521853) * t.y;
    result += vec4(0.56266373, -0.251378, -0.5005789, 0.13106057) * t.z;
    result += vec4(0.18491416, 0.046887, -0.067797676, 0.14694957) * t.w;
    return result;
}
void main()
{
  FragColor = hook();
}
