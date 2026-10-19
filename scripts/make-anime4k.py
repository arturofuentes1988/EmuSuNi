#!/usr/bin/env python3
# EmuSuNi - GPL-2.0-or-later (see LICENSE).
# Turns an Anime4K shader for mpv (bloc97's //!HOOK passes, MIT) into one Vulkan fragment shader
# per pass for GpuFilter.cpp: the pass's textures at bindings 0.., the position from quad.vert.
#   make-anime4k.py <Anime4K_X.glsl> <out_dir> <name> [--partial]
# Writes <out_dir>/<name>_<n>.frag and prints, per pass, what it reads and saves (GpuFilter.cpp's
# table must match). --partial also writes <name>_<n>_partial.frag for the last pass (the one that
# adds the network's correction to its input) with the correction scaled by pc.Origin.z (Natural
# AI's strength, set by GpuFilter.cpp): the network's picture mixed with its input, for free.
#
# Each pass is then rewritten into an equivalent form that is cheaper on the PS4's GPU (GCN GFX7,
# RADV/ACO), same network and weights, same result up to float rounding (rewrite() below):
# CReLU as (A+B)*max(t,0) - B*t (max(-t,0) = max(t,0) - t: half the max instructions), the
# matrix products accumulated column by column into the result (one v_madmk_f32 per weight), and
# the texel size from the push constants (pc.SourceSize.zw, set by GpuFilter.cpp's SetConstants
# for the pass's first input; every input of these passes has that size).
import os
import re
import sys

MAT = re.compile(r"^\s*(?:vec4 result =|result \+=) mat4\(([^)]*)\) \* (go|g)_(\d+)(\(([^)]*)\))?;\s*$")
BIAS = re.compile(r"^\s*result \+= vec4\(([^)]*)\);\s*$")


def fmt(v):
    return repr(float("%.9g" % v)) if v != 0 else "0.0"


def rewrite(text):
    lines = text.splitlines()
    body_start = next(i for i, line in enumerate(lines) if line.startswith("vec4 hook() {"))
    body_end = next(i for i in range(body_start + 1, len(lines)) if lines[i].strip() == "}")
    head, body, tail = lines[:body_start], lines[body_start + 1:body_end], lines[body_end + 1:]
    new_head = []
    for line in head:
        m = re.match(r"#define (\w+)_pt \(vec2\(1\.0\) / \w+_size\)", line)
        if m:
            line = "#define %s_pt (pc.SourceSize.zw)" % m.group(1)
        new_head.append(line)
        if line.startswith("#version"):
            new_head += ["#extension GL_GOOGLE_include_directive : require", '#include "common.glsl"']
    macro = {}
    for line in head:
        m = re.match(r"#define (go|g)_(\d+)(\(x_off, y_off\))? \((.*)\)\s*$", line)
        if not m:
            continue
        expr = m.group(4)
        inner = re.search(r"(\w+_(?:texOff|tex))\(([^()]*(?:\([^()]*\))?[^()]*)\)", expr)
        macro[(m.group(1), int(m.group(2)))] = (expr.startswith("max(-("), expr.startswith("max(("),
                                                inner.group(1), inner.group(2))
    taps, order, bias, ret = {}, [], None, None
    for line in body:
        m = MAT.match(line)
        if m:
            coeffs = [float(c) for c in m.group(1).split(",")]
            neg, pos, func, arg = macro[(m.group(2), int(m.group(3)))]
            if m.group(5) is not None:
                x, y = [a.strip() for a in m.group(5).split(",")]
                arg = "vec2(%s, %s)" % (x, y)
            key = (func, arg)
            if key not in taps:
                taps[key] = {}
                order.append(key)
            part = "neg" if neg else "pos" if pos else "raw"
            assert part not in taps[key]
            taps[key][part] = coeffs
        elif BIAS.match(line):
            bias = [float(c) for c in BIAS.match(line).group(1).split(",")]
        elif line.strip().startswith("return"):
            ret = line.strip()
        elif line.strip():
            raise RuntimeError("unexpected line %r" % line)
    products = []
    for key in order:
        parts = taps[key]
        if "raw" in parts:
            products.append((key, [("t", parts["raw"])]))
        else:
            a, b = parts["pos"], parts["neg"]
            products.append((key, [("p", [x + y for x, y in zip(a, b)]), ("t", [-y for y in b])]))
    rows = 3 if all(m[4 * j + 3] == 0 for _, prods in products for _, m in prods for j in range(4)) and \
        bias[3] == 0 else 4
    acc = "result" if rows == 4 else "result.rgb"
    vec = "vec4" if rows == 4 else "vec3"
    new_body = ["vec4 hook() {", "    vec4 t, p;", "    vec4 result = vec4(%s);" % ", ".join(fmt(c) for c in bias)]
    for (func, arg), prods in products:
        new_body.append("    t = %s(%s);" % (func, arg))
        if any(v == "p" for v, _ in prods):
            new_body.append("    p = max(t, 0.0);")
        for v, m in prods:
            for j in range(4):
                col = m[4 * j:4 * j + rows]
                if all(c == 0 for c in col):
                    continue
                new_body.append("    %s += %s(%s) * %s.%s;" % (acc, vec, ", ".join(fmt(c) for c in col), v, "xyzw"[j]))
    new_body.append("    " + ret)
    new_body.append("}")
    note = ["// Rewritten for GCN (same network and weights): CReLU as (A+B)*max(t,0) - B*t, products",
            "// accumulated column by column, the texel size from the push constants."]
    return "\n".join(new_head[:4] + note + new_head[4:] + new_body + tail) + "\n"


def main():
    source, out_dir, name = sys.argv[1], sys.argv[2], sys.argv[3]
    partial = '--partial' in sys.argv[4:]
    with open(source, encoding='utf-8') as f:
        text = f.read()
    license_end = text.index('//!DESC')
    license_text = text[:license_end].strip()
    blocks = re.split(r'(?m)^(?=//!DESC)', text[license_end:])
    os.makedirs(out_dir, exist_ok=True)
    n = 0
    for block in blocks:
        if not block.startswith('//!DESC'):
            continue
        desc = block.splitlines()[0][len('//!DESC'):].strip()
        binds = re.findall(r'(?m)^//!BIND\s+(\S+)', block)
        save = (re.findall(r'(?m)^//!SAVE\s+(\S+)', block) or ['MAIN'])[0]
        width = (re.findall(r'(?m)^//!WIDTH\s+(.+)$', block) or [''])[0].strip()
        code = '\n'.join(line for line in block.splitlines() if not line.startswith('//!')).strip()
        out = ['#version 450',
               '// %s (EmuSuNi\'s Vulkan form of %s, pass %d, made by scripts/make-anime4k.py)'
               % (desc, os.path.basename(source), n)]
        out += ['// ' + line.lstrip('/ ').rstrip() if line.strip() else '//' for line in license_text.splitlines()]
        out += ['', 'layout(location = 0) in vec2 vTexCoord;', 'layout(location = 0) out vec4 FragColor;']
        for i, bind in enumerate(binds):
            t = 'tex_' + bind
            out += ['layout(set = 0, binding = %d) uniform sampler2D %s;' % (i, t),
                    '#define %s_pos vTexCoord' % bind,
                    '#define %s_size vec2(textureSize(%s, 0))' % (bind, t),
                    '#define %s_pt (vec2(1.0) / %s_size)' % (bind, bind),
                    '#define %s_tex(p) texture(%s, p)' % (bind, t),
                    '#define %s_texOff(off) texture(%s, vTexCoord + %s_pt * (off))' % (bind, t, bind)]
        if 'MAIN' in binds:
            for suffix in ('pos', 'size', 'pt', 'tex', 'texOff'):
                out.append('#define HOOKED_%s MAIN_%s' % (suffix, suffix))
        out += [code, 'void main()', '{', '  FragColor = hook();', '}', '']
        path = os.path.join(out_dir, '%s_%d.frag' % (name, n))
        shader = rewrite('\n'.join(out))
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(shader)
        last = '    return result + MAIN_tex(MAIN_pos);\n'
        if partial and last in shader:
            variant = shader.replace(last, '    return result * pc.Origin.z + MAIN_tex(MAIN_pos);\n').replace(
                'made by scripts/make-anime4k.py)', 'made by scripts/make-anime4k.py --partial)', 1)
            with open(os.path.join(out_dir, '%s_%d_partial.frag' % (name, n)), 'w', encoding='utf-8',
                      newline='\n') as f:
                f.write(variant)
            print('%s_%d_partial: the same, the correction scaled by pc.Origin.z' % (name, n))
        print('%s_%d: reads %s, saves %s, width %s' % (name, n, ','.join(binds), save, width or 'same'))
        n += 1


main()
