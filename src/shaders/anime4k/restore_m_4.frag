#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Anime4K-v4.0-Restore-CNN-(M)-Conv-4x3x3x8 (EmuSuNi's Vulkan form of Anime4K_Restore_CNN_M.glsl, pass 4, made by scripts/make-anime4k.py)
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
layout(set = 0, binding = 0) uniform sampler2D tex_conv2d_3_tf;
#define conv2d_3_tf_pos vTexCoord
#define conv2d_3_tf_size vec2(textureSize(tex_conv2d_3_tf, 0))
#define conv2d_3_tf_pt (pc.SourceSize.zw)
#define conv2d_3_tf_tex(p) texture(tex_conv2d_3_tf, p)
#define conv2d_3_tf_texOff(off) texture(tex_conv2d_3_tf, vTexCoord + conv2d_3_tf_pt * (off))
#define go_0(x_off, y_off) (max((conv2d_3_tf_texOff(vec2(x_off, y_off))), 0.0))
#define go_1(x_off, y_off) (max(-(conv2d_3_tf_texOff(vec2(x_off, y_off))), 0.0))
vec4 hook() {
    vec4 t, p;
    vec4 result = vec4(-0.048888464, -0.0561434, 0.030690912, -0.030496685);
    t = conv2d_3_tf_texOff(vec2(-1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(-0.03263897, -0.12611481, 0.21624452, -0.00137238) * p.x;
    result += vec4(-0.14438105, 0.0742568, -0.0947031189, 0.291133433) * p.y;
    result += vec4(-0.002595613, -0.06202692, -0.085200448, -0.063433073) * p.z;
    result += vec4(-0.153374124, 0.170384826, 0.08352289, 0.243052534) * p.w;
    result += vec4(-0.25749087, -0.0053866603, 0.09391162, 0.06129529) * t.x;
    result += vec4(0.094091184, 0.07419633, -0.0013858611, -0.012000353) * t.y;
    result += vec4(0.062903, 0.0204224, 0.12113313, -0.017942557) * t.z;
    result += vec4(0.073379934, -0.052201986, -0.35864577, -0.023564404) * t.w;
    t = conv2d_3_tf_texOff(vec2(-1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.229276274, -0.02308603, -0.10616439, 0.216704751) * p.x;
    result += vec4(-0.071237547, 0.143970693, -0.131490324, -0.286232524) * p.y;
    result += vec4(-0.1684844, -0.07781198, 0.068522139, -0.018952135) * p.z;
    result += vec4(-0.00956309, -0.09451887, -0.51876965, -0.157598) * p.w;
    result += vec4(-0.100115694, -0.19451359, -0.23252094, -0.19506809) * t.x;
    result += vec4(0.12470779, -0.0027281935, 0.17488572, 0.018721964) * t.y;
    result += vec4(0.15159339, -0.18457152, -0.057712987, 0.08191495) * t.z;
    result += vec4(-0.19735703, -0.07326743, 0.28563106, -0.01642815) * t.w;
    t = conv2d_3_tf_texOff(vec2(-1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.013978606, 0.04684582, 0.0673352406, 0.29576276) * p.x;
    result += vec4(0.230557326, -0.1575395, -0.04988742, -0.054426991) * p.y;
    result += vec4(-0.02538723, -0.10155102, -0.206538096, -0.005075421) * p.z;
    result += vec4(0.241814036, 0.23023109, 0.128082791, -0.11834336) * p.w;
    result += vec4(-0.068062514, -0.28356665, -0.07377898, -0.42776972) * t.x;
    result += vec4(-0.28725025, 0.13045293, 0.17525704, 0.05885591) * t.y;
    result += vec4(0.16676305, 0.2555945, 0.10078422, 0.053032875) * t.z;
    result += vec4(-0.084470876, -0.06460686, -0.13824362, 0.05231353) * t.w;
    t = conv2d_3_tf_texOff(vec2(0.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.252032287, -0.137747004, -0.11576368, 0.0550964) * p.x;
    result += vec4(-0.322034877, 0.172781219, -0.54854033, 0.01826219) * p.y;
    result += vec4(0.0842882, 0.117371352, -0.029209, 0.05225702) * p.z;
    result += vec4(-0.13358083, -0.07285817, -0.56997079, -0.049061444) * p.w;
    result += vec4(-0.22637829, 0.028969254, -0.1968254, 0.13331996) * t.x;
    result += vec4(-0.038017053, 0.008854481, 0.2031639, -0.09237089) * t.y;
    result += vec4(0.3821112, -0.1108527, 0.11029933, 0.24542028) * t.z;
    result += vec4(-0.22416145, 0.031492114, 0.19144306, 0.0996271) * t.w;
    t = conv2d_3_tf_texOff(vec2(0.0, 0.0));
    p = max(t, 0.0);
    result += vec4(0.15169057, 0.27680126, 0.00235116, -0.19392471) * p.x;
    result += vec4(-0.23154755, -0.003966, -0.132512736, 0.05507207) * p.y;
    result += vec4(0.00967186, 0.01500815, 0.240099445, -0.054095134) * p.z;
    result += vec4(-0.05161907, -0.38798934, -0.086627822, 0.164102031) * p.w;
    result += vec4(-0.10776744, -0.16363445, -0.14656505, 0.3737814) * t.x;
    result += vec4(0.06642015, -0.5616549, 0.008412252, 0.37266847) * t.y;
    result += vec4(-0.12506576, 0.15329036, -0.037538245, 0.10810259) * t.z;
    result += vec4(-0.01706349, -0.1813702, -0.035651788, 0.012786579) * t.w;
    t = conv2d_3_tf_texOff(vec2(0.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.0619238, 0.2891386, -0.03050925, 0.18625805) * p.x;
    result += vec4(-0.01219308, -0.008644022, -0.001007394, -0.255734168) * p.y;
    result += vec4(-0.13868372, -0.195854936, -0.319799612, 0.24566626) * p.z;
    result += vec4(0.411076587, -0.10191392, 0.179320712, 0.251199385) * p.w;
    result += vec4(0.4023338, 0.2098614, 0.18285121, 0.02727653) * t.x;
    result += vec4(-0.26107362, -0.041306913, 0.036515504, 0.045217298) * t.y;
    result += vec4(0.39958602, 0.21229339, 0.021053292, 0.13427502) * t.z;
    result += vec4(-0.36178818, -0.20934913, -0.1500852, -0.2634554) * t.w;
    t = conv2d_3_tf_texOff(vec2(1.0, -1.0));
    p = max(t, 0.0);
    result += vec4(0.0732607896, -0.10546213, -0.108914952, 0.145532595) * p.x;
    result += vec4(0.012843914, 0.57494405, 0.19181665, 0.10826464) * p.y;
    result += vec4(-0.147104874, -0.284066314, -0.23621407, 0.191752947) * p.z;
    result += vec4(0.17703088, 0.18349756, -0.099155565, 0.045444076) * p.w;
    result += vec4(-0.07794611, 0.25937587, 0.06822529, 0.056336135) * t.x;
    result += vec4(-0.094220124, -0.21588847, 0.0455218, 0.10968329) * t.y;
    result += vec4(0.08068449, 0.31366697, -0.07799637, -0.24252681) * t.z;
    result += vec4(-0.23963861, -0.13715535, -0.010329345, -0.09094301) * t.w;
    t = conv2d_3_tf_texOff(vec2(1.0, 0.0));
    p = max(t, 0.0);
    result += vec4(-0.29271018, -0.151338444, 0.04525271, -0.145090183) * p.x;
    result += vec4(0.20353332, 0.40293597, 0.165831503, 0.24804662) * p.y;
    result += vec4(-0.06622588, -0.12303484, 0.10285783, 0.17957405) * p.z;
    result += vec4(0.826771, -0.06539394, 0.29788266, 0.00280875) * p.w;
    result += vec4(0.20975718, 0.12550138, -0.14453574, 0.0020878632) * t.x;
    result += vec4(0.07153068, -0.3249998, 0.056577377, -0.18166828) * t.y;
    result += vec4(-0.37204072, -0.17018336, -0.3752895, -0.32178587) * t.z;
    result += vec4(-0.2571982, 0.27258632, 0.25971004, 0.40536007) * t.w;
    t = conv2d_3_tf_texOff(vec2(1.0, 1.0));
    p = max(t, 0.0);
    result += vec4(-0.50949999, -0.21352788, 0.15878776, -0.12744727) * p.x;
    result += vec4(0.16545561, 0.264638134, 0.235038422, 0.05122958) * p.y;
    result += vec4(-0.16022417, -0.01963512, -0.167516939, -0.0920131) * p.z;
    result += vec4(0.35351445, -0.099202149, 0.062614333, 0.05215333) * p.w;
    result += vec4(0.3243907, 0.06300621, 0.09398436, 0.19549188) * t.x;
    result += vec4(-0.14906861, -0.061537784, 0.055284478, -0.11281728) * t.y;
    result += vec4(-0.12964857, -0.09979093, 0.1810159, 0.4104283) * t.z;
    result += vec4(-0.05807971, 0.056371246, -0.08072554, -0.18479007) * t.w;
    return result;
}
void main()
{
  FragColor = hook();
}
