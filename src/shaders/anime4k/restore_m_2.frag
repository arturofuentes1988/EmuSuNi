#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x8 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 2, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 0) uniform sampler2D tex_conv2d_1_tf;
#define conv2d_1_tf_pos vTexCoord
#define conv2d_1_tf_size vec2(textureSize(tex_conv2d_1_tf, 0))
#define conv2d_1_tf_pt (pc.SourceSize.zw)
#define conv2d_1_tf_tex(p) texture(tex_conv2d_1_tf, p)
#define conv2d_1_tf_texOff(off) texture(tex_conv2d_1_tf, vTexCoord + conv2d_1_tf_pt * (off))
#define go_0(x_off, y_off) (max((conv2d_1_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_1_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(-0.008952847, -0.0058945753, -0.08097229, 0.020968592);
    t = conv2d_1_tf_texOff(vec2(-1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.321394099, -0.02965565, 0.25423206, -0.565750795) * p.x;
    result += vec4(0.101453354, 0.02083664, 0.00102503, -0.07527143) * p.y;
    result += vec4(-0.086472865, 0.01929232, -0.034862276, 0.086043596) * p.z;
    result += vec4(0.23611806, 0.05263493, -0.081036461, -0.23252258) * p.w;
    result += vec4(-0.0059623295, 0.26060802, -0.32115817, -0.021025505) * t.x;
    result += vec4(-0.09783085, 0.15865178, -0.1473021, 0.24977303) * t.y;
    result += vec4(0.033508282, -0.17480391, 0.091310136, -0.09870876) * t.z;
    result += vec4(-0.10504043, 0.06105686, -0.013493489, 0.11278855) * t.w;
    t = conv2d_1_tf_texOff(vec2(-1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.12071362, -0.263976275, 0.50109742, -0.241789146) * p.x;
    result += vec4(-0.153825965, -0.25059027, -0.12186432, -0.08636178) * p.y;
    result += vec4(-0.0600102, 0.28873093, 0.06387171, 0.20093727) * p.z;
    result += vec4(-0.072722211, -0.071431702, -0.199202146, 0.305404328) * p.w;
    result += vec4(-0.14875248, 0.14859414, -0.19377062, 0.17456068) * t.x;
    result += vec4(-0.101288855, 0.1113682, 0.48944646, -0.1018565) * t.y;
    result += vec4(0.037392337, -0.08539691, -0.1751306, 0.15428723) * t.z;
    result += vec4(0.059375558, -0.027663672, -0.051804014, 0.049813222) * t.w;
    t = conv2d_1_tf_texOff(vec2(-1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.129858974, -0.33544956, 0.219031742, -0.2639448) * p.x;
    result += vec4(-0.34847202, -0.2580615, 0.483256015, 0.141680729) * p.y;
    result += vec4(-0.216605718, 0.248260008, 0.23133325, 0.215294429) * p.z;
    result += vec4(-0.133312034, 0.16929705, -0.0162934172, 0.151281314) * p.w;
    result += vec4(-0.118846565, 0.19869871, 0.037388258, -0.08456728) * t.x;
    result += vec4(0.11662527, 0.43818352, 0.093285345, -0.038507205) * t.y;
    result += vec4(0.051991668, -0.21008292, -0.10792365, -0.2020924) * t.z;
    result += vec4(-0.057021596, -0.09460527, -0.0016551288, 0.0015957063) * t.w;
    t = conv2d_1_tf_texOff(vec2(0.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.05721641, -0.02594838, 0.143218454, -0.37506661) * p.x;
    result += vec4(-0.172356952, 0.07626065, 0.10209525, 0.140264309) * p.y;
    result += vec4(-0.086207778, 0.175850064, 0.10167832, 0.09191063) * p.z;
    result += vec4(0.056906285, -0.064752626, 0.20601326, 0.02084201) * p.w;
    result += vec4(-0.11062174, 0.2639232, 0.060295466, 0.3217331) * t.x;
    result += vec4(0.050545212, -0.30989558, -0.30906132, -0.030323273) * t.y;
    result += vec4(-0.028986752, -0.037429404, -0.20855664, 0.19848943) * t.z;
    result += vec4(-0.034687653, 0.09599135, 0.06250494, 0.13215867) * t.w;
    t = conv2d_1_tf_texOff(vec2(0.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.219462034, -0.23380175, 0.21574843, 0.29597866) * p.x;
    result += vec4(-0.10930972, 0.23157774, 0.588245865, 0.42551698) * p.y;
    result += vec4(-0.21788191, -0.154004837, -0.376204325, 0.215621576) * p.z;
    result += vec4(-0.160971665, 0.050930715, -0.19712605, 0.459081517) * p.w;
    result += vec4(0.010391146, -0.07657845, -0.44491258, -0.0435906) * t.x;
    result += vec4(-0.0075931503, -0.42632654, -0.47022533, -0.34737435) * t.y;
    result += vec4(0.15452717, 0.14613411, 0.45231065, -0.12094409) * t.z;
    result += vec4(-0.0067911847, -0.057501152, -0.09876979, -0.044946447) * t.w;
    t = conv2d_1_tf_texOff(vec2(0.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.28000527, -0.13525853, -0.218980185, -0.153512798) * p.x;
    result += vec4(-0.244015425, 0.39472799, 0.11773198, 0.14038846) * p.y;
    result += vec4(0.067761847, -0.025394905, -0.2487917, -0.238427647) * p.z;
    result += vec4(-0.11189345, 0.16895403, 0.00165953, 0.05286306) * p.w;
    result += vec4(0.15607435, -0.2293058, 0.09520331, -0.012836732) * t.x;
    result += vec4(0.15282455, -0.26437718, 0.1685477, 0.13211122) * t.y;
    result += vec4(0.055801593, 0.016778728, 0.34478986, 0.23228309) * t.z;
    result += vec4(-0.12300962, 0.13235827, 0.13987203, 0.16550972) * t.w;
    t = conv2d_1_tf_texOff(vec2(1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.191981935, 0.28820878, 0.005706939, -0.46492124) * p.x;
    result += vec4(0.061578276, 0.14159724, -0.236995546, -0.130207641) * p.y;
    result += vec4(0.127596431, 0.07461445, 0.03243393, 0.01392927) * p.z;
    result += vec4(-0.19162059, 0.25063061, -0.03139639, 0.38766888) * p.w;
    result += vec4(-0.13161735, 0.09039346, 0.033475474, 0.23686698) * t.x;
    result += vec4(-0.1514885, -0.20977421, -0.031431954, 0.0049226107) * t.y;
    result += vec4(-0.090661936, -0.15288061, 0.03316583, -0.09646573) * t.z;
    result += vec4(0.32651708, -0.18825398, 0.15777239, -0.17572704) * t.w;
    t = conv2d_1_tf_texOff(vec2(1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.013190944, 0.12512311, -0.04364863, -0.2026566) * p.x;
    result += vec4(-0.153825407, 0.261718377, 0.034565734, -0.009707468) * p.y;
    result += vec4(-0.08448629, -0.0897827, -0.2502459, 0.095613265) * p.z;
    result += vec4(-0.10987478, -0.08639482, 0.03381129, 0.199112382) * p.w;
    result += vec4(-0.112157226, 0.08712878, -0.23453182, -0.1043877) * t.x;
    result += vec4(0.14686783, -0.28682423, 0.086443506, -0.059457052) * t.y;
    result += vec4(0.31530112, 0.2700583, 0.06028952, 0.070416875) * t.z;
    result += vec4(-0.18053482, -0.16653341, -0.25215197, -0.061915852) * t.w;
    t = conv2d_1_tf_texOff(vec2(1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.184012765, 0.183884515, 0.11529466, -0.214522046) * p.x;
    result += vec4(-0.24968971, 0.1577436, 0.021965896, 0.152832) * p.y;
    result += vec4(0.31429962, -0.42855952, -0.192997694, -0.4306517) * p.z;
    result += vec4(-0.05353567, 0.138619405, -0.057858829, 0.18471762) * p.w;
    result += vec4(0.20122242, -0.076313145, 0.0988483, -0.094337784) * t.x;
    result += vec4(0.35436687, -0.3762327, 0.07809558, -0.3055848) * t.y;
    result += vec4(-0.10425242, 0.17087407, -0.030301496, 0.13911743) * t.z;
    result += vec4(-0.01630275, -0.24247427, 0.006474477, -0.03842641) * t.w;
    return result;
}
void main()
{
  FragColor = hook();
}
