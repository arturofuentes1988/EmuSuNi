// EmuSuNi - GPL-2.0-or-later (see LICENSE).
// What every GPU filter pass shares (GpuFilter.cpp's Push, 128 bytes): the quad and the sizes.
layout(push_constant) uniform Push
{
  vec4 Clip;        // the quad in clip space: x0, y0, x1, y1 (quad.vert)
  vec4 SourceSize;  // the pass's first input: width, height, 1 / width, 1 / height
  vec4 OutputSize;  // the picture the pass draws, whole: width, height, 1 / width, 1 / height
  vec4 Origin;      // xy: the picture's top-left corner in the target (pixels); z: sharpness
  uvec4 Const0;     // AMD FSR's constants (EASU: all four, RCAS: the first)
  uvec4 Const1;
  uvec4 Const2;
  uvec4 Const3;
} pc;

// (01.72) The picture's colours (Graphics: Brightness, Gamma, Saturation), drawn by the pass to the screen (GpuFilter.cpp
// SetConstants; every mode's last pass but Tube TV's, which has a look of its own): Const3.x 1 / gamma (above 1 the
// dark parts lighter, the white staying white), .y the brightness, .z the saturation (floats' bits), .w 1 to draw them.
vec3 Grade(vec3 c)
{
  if (pc.Const3.w == 0u)
    return c;
  c = pow(clamp(c, 0.0, 1.0), vec3(uintBitsToFloat(pc.Const3.x))) * uintBitsToFloat(pc.Const3.y);
  const float grey = dot(c, vec3(0.2126, 0.7152, 0.0722));
  return clamp(mix(vec3(grey), c, uintBitsToFloat(pc.Const3.z)), 0.0, 1.0);
}
