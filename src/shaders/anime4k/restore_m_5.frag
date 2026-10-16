#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x8 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 5, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 0) uniform sampler2D tex_conv2d_4_tf;
#define conv2d_4_tf_pos vTexCoord
#define conv2d_4_tf_size vec2(textureSize(tex_conv2d_4_tf, 0))
#define conv2d_4_tf_pt (pc.SourceSize.zw)
#define conv2d_4_tf_tex(p) texture(tex_conv2d_4_tf, p)
#define conv2d_4_tf_texOff(off) texture(tex_conv2d_4_tf, vTexCoord + conv2d_4_tf_pt * (off))
#define go_0(x_off, y_off) (max((conv2d_4_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_4_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(0.011169491, 0.032399546, 0.138099, 0.023857072);
    t = conv2d_4_tf_texOff(vec2(-1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.21104635, -0.136767042, 0.01400839, -0.36443153) * p.x;
    result += vec4(0.145812394, -0.36079876, -0.243730044, -0.097845081) * p.y;
    result += vec4(0.518631416, -0.59054609, -0.049029529, 0.0490032784) * p.z;
    result += vec4(0.225683602, 0.340212715, -0.16030471, -0.221869613) * p.w;
    result += vec4(-0.05772507, 0.1640253, 0.13499664, 0.20460358) * t.x;
    result += vec4(0.024399966, -0.14966168, 0.090857334, 0.039677754) * t.y;
    result += vec4(-0.00036956606, 0.24236615, 0.053542696, 0.0049544116) * t.z;
    result += vec4(-0.026651502, -0.39019194, 0.2742246, 0.061242323) * t.w;
    t = conv2d_4_tf_texOff(vec2(-1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.017359631, -0.016451022, 0.229283479, 0.28533418) * p.x;
    result += vec4(0.258409948, -0.50818743, 0.165331771, 0.19634456) * p.y;
    result += vec4(0.0178068, -0.42408382, -0.15657875, 0.445233407) * p.z;
    result += vec4(0.22727176, 0.40145854, 0.25596581, 0.00924083) * p.w;
    result += vec4(0.016323274, 0.036179908, -0.029965919, -0.11151491) * t.x;
    result += vec4(0.00016685206, 0.29573023, -0.17996423, 0.20145437) * t.y;
    result += vec4(-0.1324275, 0.18442132, 0.24618152, -0.061780427) * t.z;
    result += vec4(0.02770517, -0.28452995, -0.39804098, 0.1174389) * t.w;
    t = conv2d_4_tf_texOff(vec2(-1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.174181397, 0.035778673, -0.10916967, 0.30781023) * p.x;
    result += vec4(0.14337834, 0.021605723, -0.43860978, -0.07212246) * p.y;
    result += vec4(-0.135335276, -0.13525303, -0.0405794, 0.214513746) * p.z;
    result += vec4(-0.005448867, 0.074434517, 0.122972266, -0.243847965) * p.w;
    result += vec4(0.025068847, 0.053328387, 0.27053785, -0.26866457) * t.x;
    result += vec4(0.09866204, -0.057677213, -0.01850112, 0.18014707) * t.y;
    result += vec4(0.13319959, 0.14411181, 0.26355243, 0.022209354) * t.z;
    result += vec4(0.05062645, 0.036771543, -0.13294417, 0.18458557) * t.w;
    t = conv2d_4_tf_texOff(vec2(0.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.199465027, -0.187907962, 0.38380162, -0.047750128) * p.x;
    result += vec4(0.00108071, -0.126021765, -0.5222444, -0.54293563) * p.y;
    result += vec4(-0.1735352, -0.41999098, -0.33325281, -0.039197503) * p.z;
    result += vec4(0.50353974, -0.163491917, 0.032405071, 0.51170818) * p.w;
    result += vec4(0.046194963, -0.038230438, 0.08993043, 0.07236354) * t.x;
    result += vec4(-0.11031123, 0.16504908, 0.09517036, 0.16459833) * t.y;
    result += vec4(0.5279925, -0.12686682, 0.05726125, -0.055361677) * t.z;
    result += vec4(-0.31593755, -0.027328093, -0.001839602, -0.30581662) * t.w;
    t = conv2d_4_tf_texOff(vec2(0.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.28340658, -0.00265426, 0.067673862, -0.215760062) * p.x;
    result += vec4(0.0550612, -0.01065008, 0.058768665, -0.03404653) * p.y;
    result += vec4(-0.02379271, -0.168250745, -0.32685791, -0.32933077) * p.z;
    result += vec4(-0.28286394, 0.498392639, 0.263076277, 0.74357122) * p.w;
    result += vec4(-0.08608678, -0.03168437, -0.007713377, 0.26140293) * t.x;
    result += vec4(0.1268983, -0.13395861, 0.069848835, 0.24080403) * t.y;
    result += vec4(-0.018839337, 0.049821075, 0.21461345, 0.14168301) * t.z;
    result += vec4(0.0872339, -0.47096667, -0.022512507, -0.14860632) * t.w;
    t = conv2d_4_tf_texOff(vec2(0.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.117964412, 0.38794564, -0.215363815, 0.147133093) * p.x;
    result += vec4(0.64117296, -0.25922641, 0.30198281, 0.43271005) * p.y;
    result += vec4(-0.013400402, 0.184331585, -0.107736137, -0.2815909) * p.z;
    result += vec4(-0.434925835, 0.38326996, -0.03147385, 0.036469725) * p.w;
    result += vec4(-0.06293673, -0.22462969, -0.045494985, -0.021673543) * t.x;
    result += vec4(-0.18227446, 0.2956555, -0.08010543, 0.01919729) * t.y;
    result += vec4(0.012190269, -0.241983, 0.046537094, 0.40094566) * t.z;
    result += vec4(0.3853647, -0.1081711, 0.16926058, -0.16138376) * t.w;
    t = conv2d_4_tf_texOff(vec2(1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.17112048, 0.10079302, -0.064325474, -0.04366933) * p.x;
    result += vec4(0.159795705, -0.14341447, 0.61175566, 0.104374136) * p.y;
    result += vec4(-0.047196753, 0.05830563, -0.0981228986, 0.0467985) * p.z;
    result += vec4(0.16042945, -0.37886972, -0.220738574, -0.41357041) * p.w;
    result += vec4(0.14854589, 0.17625804, 0.10849075, -0.221543) * t.x;
    result += vec4(-0.099971965, -0.13901573, -0.29464146, -0.020068526) * t.y;
    result += vec4(-0.054358527, 0.10351705, 0.0062914286, -0.24127026) * t.z;
    result += vec4(0.16914125, -0.12729423, 0.18377453, 0.6452375) * t.w;
    t = conv2d_4_tf_texOff(vec2(1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.10279973, -0.03686294, 0.05102951, 0.03242342) * p.x;
    result += vec4(-0.244498526, 0.05675719, 0.01062299, -0.25797294) * p.y;
    result += vec4(-0.07411342, 0.247315076, 0.1792359, -0.116652092) * p.z;
    result += vec4(-0.346523151, 0.32410023, -0.64867762, -0.10525818) * p.w;
    result += vec4(-0.12603393, 0.10986093, -0.2314103, -0.16915044) * t.x;
    result += vec4(0.13619255, 0.09349073, -0.20594226, 0.34507084) * t.y;
    result += vec4(-0.19077192, -0.052500796, -0.07185645, -0.029082738) * t.z;
    result += vec4(0.015576321, -0.08254907, 0.5501743, 0.38495848) * t.w;
    t = conv2d_4_tf_texOff(vec2(1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.14789057, 0.115847644, 0.94816695, -0.145669363) * p.x;
    result += vec4(0.42534079, -0.28609939, 0.281577294, 0.141719998) * p.y;
    result += vec4(0.01901177, 0.231189047, 0.08201319, -0.134592077) * p.z;
    result += vec4(-0.0751374, 0.4156354, -0.20792925, -0.0600114146) * p.w;
    result += vec4(-0.09300796, 0.079218306, -0.46825135, 0.08735625) * t.x;
    result += vec4(-0.06321122, -0.16234867, -0.042932414, 0.013057422) * t.y;
    result += vec4(-0.09697148, -0.23457524, -0.19417483, 0.16804664) * t.z;
    result += vec4(-0.18379296, -0.17770062, 0.050235, 0.059676602) * t.w;
    return result;
}
void main()
{
  FragColor = hook();
}
