// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// PNG files (libpng): covers, wallpapers and save state pictures.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ImageRGBA
{
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> pixels;  // RGBA8, rows top to bottom
  bool empty() const { return pixels.empty(); }
};

// Loads a PNG (any colour type) as RGBA8; scaled down to fit max_w x max_h (0: no limit).
bool LoadPng(const std::string& path, ImageRGBA* out, uint32_t max_w = 0, uint32_t max_h = 0);
bool SavePng(const std::string& path, const ImageRGBA& image);
// An RGB565 picture (Snes9x's) as RGBA8.
ImageRGBA FromRgb565(const uint16_t* pixels, uint32_t width, uint32_t height, size_t pitch_bytes);
// Box-filtered downscale (or the same image when it already fits).
ImageRGBA Shrink(const ImageRGBA& image, uint32_t max_w, uint32_t max_h);
