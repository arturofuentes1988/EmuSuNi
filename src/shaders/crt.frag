#version 450
// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// Tube TV (CRT): the picture as an old tube TV showed it, in one pass to the screen. Each of the console's lines is
// a beam of light (a Gaussian, wider where it is brighter, so bright lines fill the gaps between them), swept across
// with a soft step between the console's pixels; the glass is curved (a barrel warp: the edges bend out) with
// rounded corners and darker edges; the phosphors' mask (an aperture grille, a slot mask or dots) is drawn at the
// screen's own pixels, and the glass glows a little around the bright parts (a blurred copy, at about the console's
// size, added on top). A picture with two screens (the DS's, the 3DS's) is two tubes, each with its own lines.
// The light is added up linearly (gamma 2: squared in, square root out).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D Source;  // linear: the picture, at its own size
layout(set = 0, binding = 1) uniform sampler2D Glow;    // linear: the picture at about the console's size (or Source)

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// GpuFilter.cpp SetConstants (CrtConstants):
//   Const0, Const1: screen 0 and 1: its place in the picture drawn (x, y | width, height: unorm16 pairs in .x, .y)
//     and in the source (.z, .w).
//   Const2.x, .y: their lines (float bits); .z: the screens (1, 2) | the mask << 8 (0 none, 1 aperture grille,
//     2 slot mask, 3 dots) | the source's quarter turns << 16 | the glow copy's block (1-8: the source's texels per
//     its texel) << 20 | 1 << 24 for one screen that is the whole picture; .w: the curvature across and down (halves).
//   Const3 (halves): .x: the beam's hardness (negative: the darker the lines) and its widening with brightness;
//     .y: the mask's strength and the glow; .z: the vignette and the corners' radius (of the screen's height);
//     .w: the brightness and the sharpness across.
//   Origin.z, .w: how a beam's peak drops as it widens with the level (0..1): sqrt(1 - widening * level), as
//     1 + level * (z + level * w).

vec4 Rect(uint xy, uint wh)
{
  return vec4(unpackUnorm2x16(xy), unpackUnorm2x16(wh));
}

// The source at a point, in texels (gamma: as stored).
vec3 Fetch(vec2 texel)
{
  return texture(Source, texel * pc.SourceSize.zw).rgb;
}

// A line's colour where the beam is (gamma): the middle of its band of texels; two taps over a tall band (a picture
// drawn bigger than the console's lines).
vec3 LineColour(float u, float top, float bottom, float line, float per_line)
{
  const float middle = clamp(top + (line + 0.5) * per_line, top + 0.5, bottom - 0.5);
  if (per_line > 2.5)
  {
    const float half_band = 0.25 * per_line;
    return 0.5 * (Fetch(vec2(u, max(middle - half_band, top + 0.5))) + Fetch(vec2(u, min(middle + half_band, bottom - 0.5))));
  }
  return Fetch(vec2(u, middle));
}

vec3 Mask(uint kind, float strength, vec2 pixel)
{
  const float dark = 1.0 - strength;
  if (kind == 1u)
  {
    // Aperture grille: magenta and green stripes, a pixel each (fine enough for a 1080p screen).
    return fract(pixel.x * 0.5) < 0.5 ? vec3(1.0, dark, 1.0) : vec3(dark, 1.0, dark);
  }
  // Red, green and blue stripes, a pixel each.
  const float triad = floor(pixel.x * (1.0 / 3.0));
  float column = pixel.x - 0.5 - triad * 3.0;  // 0, 1, 2
  if (kind == 3u)
  {
    // Dots (a shadow mask): every other pair of rows a pixel along, the triads staggered.
    column = mod(column + (fract(floor(pixel.y * 0.5) * 0.5) > 0.25 ? 1.0 : 0.0), 3.0);
  }
  vec3 mask = vec3(column < 0.5 ? 1.0 : dark, column > 0.5 && column < 1.5 ? 1.0 : dark, column > 1.5 ? 1.0 : dark);
  if (kind == 2u)
  {
    // Slot mask: the stripes broken every third row, every other triad's breaks a row lower.
    const float row = fract((pixel.y + (fract(triad * 0.5) > 0.25 ? 1.0 : 0.0)) * (1.0 / 3.0));
    mask *= row < 0.3 ? dark + 0.5 * strength : 1.0;
  }
  return mask;
}

void main()
{
  const uint mask_kind = (pc.Const2.z >> 8) & 0xFFu;
  float lines = uintBitsToFloat(pc.Const2.x);
  // Across this screen (0..1), its size on the TV in pixels, and where it is in the source, in texels: one screen
  // that is the whole picture (bit 24: the usual case, no more to work out), else the screen the pixel is on.
  vec2 local = vTexCoord;
  vec2 size = pc.OutputSize.xy;
  vec2 texels = pc.SourceSize.xy;
  vec2 top_left = vec2(0.0);
  if (((pc.Const2.z >> 24) & 1u) == 0u)
  {
    vec4 place = Rect(pc.Const0.x, pc.Const0.y);
    vec4 from = Rect(pc.Const0.z, pc.Const0.w);
    if ((pc.Const2.z & 0xFFu) > 1u)
    {
      const vec4 place1 = Rect(pc.Const1.x, pc.Const1.y);
      const vec2 in1 = (vTexCoord - place1.xy) / place1.zw;
      if (all(greaterThanEqual(in1, vec2(0.0))) && all(lessThan(in1, vec2(1.0))))
      {
        place = place1;
        from = Rect(pc.Const1.z, pc.Const1.w);
        lines = uintBitsToFloat(pc.Const2.y);
      }
    }
    local = (vTexCoord - place.xy) / place.zw;
    size = place.zw * pc.OutputSize.xy;
    texels = from.zw * pc.SourceSize.xy;
    top_left = from.xy * pc.SourceSize.xy;
  }
  const float per_lines = 1.0 / lines;

  // The curved glass: the middle as it is, the edges bent outwards.
  const vec2 curve = unpackHalf2x16(pc.Const2.w);
  vec2 q = local * 2.0 - 1.0;
  q *= vec2(1.0 + q.y * q.y * curve.x, 1.0 + q.x * q.x * curve.y);
  local = q * 0.5 + 0.5;
  // The rounded corners (a rounded rectangle's distance, in pixels), the edge smoothed over a pixel.
  const vec2 vignette_corner = unpackHalf2x16(pc.Const3.z);
  const float radius = vignette_corner.y * size.y;
  const vec2 d = abs(local - 0.5) * size - (0.5 * size - radius);
  const float edge = clamp(0.5 - (length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius), 0.0, 1.0);
  if (edge <= 0.0)
  {
    FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  // The source's own way round: a turned picture (a vertical arcade game) is a monitor on its side, its lines down
  // the TV (Turn: libretro's quarter turns anticlockwise, as DrawPicture draws it).
  const uint turns = (pc.Const2.z >> 16) & 3u;
  const vec2 at_source = turns == 1u ? vec2(1.0 - local.y, local.x) :
                         turns == 2u ? 1.0 - local :
                         turns == 3u ? vec2(local.y, 1.0 - local.x) :
                                       local;
  const vec2 size_source = (turns & 1u) != 0u ? size.yx : size;
  // The console's lines over the screen's texels.
  const float per_line = texels.y * per_lines;
  const vec4 brightness_sharpness = vec4(unpackHalf2x16(pc.Const3.w), 0.0, 0.0);
  // Across: each texel crisp, with a soft step to the next (its width: the sharpness; a picture as big as the
  // screen's or bigger: linear).
  const float x = at_source.x * texels.x;
  const float left = floor(x - 0.5);
  const float per_texel = size_source.x / texels.x;
  const float step = clamp((x - 0.5 - left - 0.5) * max(per_texel * brightness_sharpness.y, 1.0) + 0.5, 0.0, 1.0);
  const float u = clamp(top_left.x + left + 0.5 + step, top_left.x + 0.5, top_left.x + texels.x - 0.5);

  // Down: the two nearest lines' beams.
  const float y = at_source.y * lines;
  const float line = floor(y);
  const float f = y - line - 0.5;  // from this line's middle (-0.5..0.5)
  const float other = clamp(line + (f < 0.0 ? -1.0 : 1.0), 0.0, lines - 1.0);
  const float other_there = abs(other - line) > 0.5 ? 1.0 : 0.0;  // (none above the first line, below the last)
  const float bottom = top_left.y + texels.y;
  const vec3 s0 = LineColour(u, top_left.y, bottom, line, per_line);
  const vec3 s1 = LineColour(u, top_left.y, bottom, other, per_line);
  const vec2 scan = unpackHalf2x16(pc.Const3.x);  // hardness, widening
  // Where the lines are too close for them (under about 3 pixels a line), they fade away (no moire).
  const float pixels_per_line = size_source.y * per_lines;
  const float fade = smoothstep(1.5, 3.0, pixels_per_line);
  const vec3 h0 = scan.x * (1.0 - scan.y * s0);
  const vec3 h1 = scan.x * (1.0 - scan.y * s1);
  // Each beam's light kept whatever its width: 1 / its Gaussian's area, sqrt(-h ln 2 / pi), as the darkest's times
  // sqrt(1 - widening * level) (pc.Origin.zw: that as a polynomial of the level).
  const float area = sqrt(-scan.x * 0.2206356);
  const vec3 n0 = area * (1.0 + s0 * (pc.Origin.z + s0 * pc.Origin.w));
  const vec3 n1 = area * (1.0 + s1 * (pc.Origin.z + s1 * pc.Origin.w));
  const float d0 = f * f, d1 = (1.0 - abs(f)) * (1.0 - abs(f));
  const vec3 c0 = s0 * s0, c1 = s1 * s1;
  const vec3 beams = c0 * exp2(h0 * d0) * n0 + c1 * exp2(h1 * d1) * n1 * other_there;
  vec3 colour = mix(c0, beams, fade);

  // The phosphors' mask, then the glass's glow on top.
  const vec2 mask_glow = unpackHalf2x16(pc.Const3.y);
  if (mask_kind != 0u)
    colour *= Mask(mask_kind, mask_glow.x, gl_FragCoord.xy);
  if (mask_glow.y > 0.0)
  {
    // (A glow texel: the source's, times the block of them each of the smaller copy's stands for: bits 20-23.)
    const vec2 g = float((pc.Const2.z >> 20) & 15u) * pc.SourceSize.zw;
    const vec2 at = (top_left + at_source * texels) * pc.SourceSize.zw;
    vec3 glow = texture(Glow, at + vec2(-1.25, -0.6) * g).rgb;
    vec3 sum = glow * glow;
    glow = texture(Glow, at + vec2(1.25, -0.6) * g).rgb;
    sum += glow * glow;
    glow = texture(Glow, at + vec2(-1.25, 0.6) * g).rgb;
    sum += glow * glow;
    glow = texture(Glow, at + vec2(1.25, 0.6) * g).rgb;
    sum += glow * glow;
    colour += sum * (0.25 * mask_glow.y);
  }
  // Darker towards the edges; the brightness the mask took back.
  colour *= brightness_sharpness.x * (1.0 - vignette_corner.x * 0.5 * dot(q, q));
  FragColor = vec4(sqrt(clamp(colour, 0.0, 1.0)) * edge, 1.0);
}
