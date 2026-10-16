#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x8 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 1, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 0) uniform sampler2D tex_conv2d_tf;
#define conv2d_tf_pos vTexCoord
#define conv2d_tf_size vec2(textureSize(tex_conv2d_tf, 0))
#define conv2d_tf_pt (pc.SourceSize.zw)
#define conv2d_tf_tex(p) texture(tex_conv2d_tf, p)
#define conv2d_tf_texOff(off) texture(tex_conv2d_tf, vTexCoord + conv2d_tf_pt * (off))
#define go_0(x_off, y_off) (max((conv2d_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(-0.018297346, -0.080951825, -0.062163066, -0.08050014);
    t = conv2d_tf_texOff(vec2(-1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.070307945, -0.23447211, 0.04341229, -0.035666177) * p.x;
    result += vec4(0.223947332, -0.032892494, -0.320911277, -0.33443215) * p.y;
    result += vec4(-0.21578959, 0.299141653, -0.189901075, 0.46929516) * p.z;
    result += vec4(0.02000511, 0.316361946, 0.144026824, -0.1645743) * p.w;
    result += vec4(-0.093798615, -0.17074613, 0.08780678, 0.012520207) * t.x;
    result += vec4(-0.118534856, -0.027508778, 0.2778478, 0.19509242) * t.y;
    result += vec4(0.34137097, -0.32000312, 0.22027159, -0.337515) * t.z;
    result += vec4(-0.16220862, -0.108993016, -0.14070526, -0.12784284) * t.w;
    t = conv2d_tf_texOff(vec2(-1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.04191689, 0.14488455, -0.54285753, 0.133469056) * p.x;
    result += vec4(0.143737842, -0.079585728, -0.155050624, 0.519666763) * p.y;
    result += vec4(0.05983875, 0.490214186, -0.11303948, 0.02920469) * p.z;
    result += vec4(0.2158846, 0.05840341, -0.01782891, 0.177324237) * p.w;
    result += vec4(0.14325632, 0.1467453, 0.27502358, -0.09370837) * t.x;
    result += vec4(-0.11821083, 0.012266484, 0.2100548, -0.4707502) * t.y;
    result += vec4(0.06766648, -0.58165014, 0.2512279, 0.33783755) * t.z;
    result += vec4(-0.1318925, 0.04346277, -0.15454485, -0.044500057) * t.w;
    t = conv2d_tf_texOff(vec2(-1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.22262077, 0.0183271598, -0.280225384, 0.192730935) * p.x;
    result += vec4(-0.63520027, -0.077064538, 0.56371043, 0.61080726) * p.y;
    result += vec4(0.181417085, 0.280993598, -0.4404225, 0.77968778) * p.z;
    result += vec4(0.08504451, 0.007219076, 0.0507803, -0.234986376) * p.w;
    result += vec4(0.05683207, -0.0051946463, 0.108000524, -0.10133204) * t.x;
    result += vec4(0.50763863, -0.007308442, -0.8542404, -0.28387356) * t.y;
    result += vec4(-0.022709515, -0.294523, 0.3822472, -0.66166407) * t.z;
    result += vec4(-0.01404485, -0.031282708, 0.26756814, 0.123147786) * t.w;
    t = conv2d_tf_texOff(vec2(0.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.09581709, 0.27050927, 0.183927542, 0.14293057) * p.x;
    result += vec4(-0.0524708, -0.119344576, -0.1353281, -0.250152912) * p.y;
    result += vec4(0.158744498, 0.17638614, -0.170191273, 0.307411846) * p.z;
    result += vec4(0.20758791, -0.18790504, -0.32659335, -0.221806086) * p.w;
    result += vec4(0.36455178, -0.3470555, 0.045303088, 0.03170764) * t.x;
    result += vec4(0.15802494, 0.0019141496, 0.25939587, 0.23875342) * t.y;
    result += vec4(-0.130428, -0.03954273, 0.17985536, -0.105145946) * t.z;
    result += vec4(-0.15804817, -0.12551713, -0.28371975, 0.085748516) * t.w;
    t = conv2d_tf_texOff(vec2(0.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.0401320963, -0.15380126, 0.594067741, -0.67023461) * p.x;
    result += vec4(-0.125166223, 0.453389558, 0.22757959, 0.18431759) * p.y;
    result += vec4(0.24406098, 0.30854095, -0.045468657, 0.77634407) * p.z;
    result += vec4(-0.45599516, 0.11260221, -0.042333935, -0.18889218) * p.w;
    result += vec4(-0.0060625463, -0.2443924, 0.017692259, 0.20214005) * t.x;
    result += vec4(0.09584515, 0.012805372, 0.13942227, -0.16143198) * t.y;
    result += vec4(-0.12942013, -0.41785547, -0.046071563, -0.7030026) * t.z;
    result += vec4(-0.10499644, 0.20566013, 0.031321276, -0.27830327) * t.w;
    t = conv2d_tf_texOff(vec2(0.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.138130009, 0.12474708, 0.1793083, -0.76848514) * p.x;
    result += vec4(-0.055250171, -0.205664723, 0.13509425, 0.05051227) * p.y;
    result += vec4(-0.12061906, -0.49534932, -0.0488436903, 0.15937497) * p.z;
    result += vec4(0.06607106, -0.00629627, 0.10839222, -0.10776377) * p.w;
    result += vec4(0.081274964, 0.14562319, -0.27200526, 0.20491314) * t.x;
    result += vec4(-0.012910989, -0.024201397, -0.04816258, -0.21297328) * t.y;
    result += vec4(0.22015952, 0.44160756, 0.056035373, -0.33824417) * t.z;
    result += vec4(0.31645304, -0.15469243, -0.053187452, 0.20989445) * t.w;
    t = conv2d_tf_texOff(vec2(1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.018112233, 0.1355442, -0.11718578, 0.33411548) * p.x;
    result += vec4(0.00182863, 0.06723788, -0.10685519, -0.04379402) * p.y;
    result += vec4(-0.20532021, -0.063808905, -0.2865524, -0.053928706) * p.z;
    result += vec4(-0.098072873, -0.154803908, 0.07863768, 0.11733239) * p.w;
    result += vec4(0.046550367, -0.033185404, -0.33337244, -0.12853645) * t.x;
    result += vec4(-0.23520172, 0.05909214, -0.0861368, -0.10706329) * t.y;
    result += vec4(0.07058717, 0.11759937, 0.18594047, -0.080006264) * t.z;
    result += vec4(0.055425353, 0.12506317, -0.15729053, 0.0915004) * t.w;
    t = conv2d_tf_texOff(vec2(1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.052987934, 0.115229417, -0.29624336, 0.139895913) * p.x;
    result += vec4(0.16672301, -0.116599724, -0.067245874, -0.09829318) * p.y;
    result += vec4(0.142399223, 0.173522525, 0.09550374, 0.228106313) * p.z;
    result += vec4(0.09305155, 0.00180755, 0.11252424, 0.00159849) * p.w;
    result += vec4(-0.042516407, -0.14844789, -0.16533111, -0.13502933) * t.x;
    result += vec4(0.0655417, 0.057256397, -0.076713726, 0.23448966) * t.y;
    result += vec4(-0.12855926, -0.014219275, -0.051761385, -0.053433083) * t.z;
    result += vec4(0.2446715, 0.4008074, -0.19603717, 0.1796951) * t.w;
    t = conv2d_tf_texOff(vec2(1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(0.02284369, -0.03226433, -0.031785179, -0.0731389206) * p.x;
    result += vec4(0.154963844, -0.0477362, -0.3035524, -0.432038727) * p.y;
    result += vec4(-0.173581919, -0.212903089, 0.12339446, 0.27174394) * p.z;
    result += vec4(-0.23459933, -0.16537201, -0.30044996, 0.01512331) * p.w;
    result += vec4(-0.14777803, -0.15524907, -0.043158617, 0.06996876) * t.x;
    result += vec4(-0.19210646, 0.2144364, 0.47020787, 0.4207906) * t.y;
    result += vec4(0.18074386, 0.2163903, -0.0030754965, -0.36799973) * t.z;
    result += vec4(0.3837698, 0.0022661497, 0.37276733, 0.28934997) * t.w;
    return result;
}
void main()
{
  FragColor = hook();
}
