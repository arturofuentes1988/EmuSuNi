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
