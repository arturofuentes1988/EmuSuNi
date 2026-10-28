// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Pictures: PNG files (libpng) - covers, wallpapers and save state pictures -, and JPEG files (libjpeg-turbo, 00.81:
// covers the player puts there by hand).

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
// Loads a picture, a PNG or a JPEG (00.81), as RGBA8 scaled down to fit max_w x max_h (0: no limit). Known by its
// first bytes, whatever its name says (a JPEG named .png too). A big one is made smaller while it is read (a JPEG by
// libjpeg's own 1/2 to 1/8 scaling first): never the whole picture in memory, except a progressive JPEG's or an
// interlaced PNG's, which are refused past 48 MiB. false: *why says why in plain words (English, one of a few
// sentences the language files translate; for the log and the menu), *detail libpng's or libjpeg's own words, if any.
bool LoadPicture(const std::string& path, ImageRGBA* out, uint32_t max_w = 0, uint32_t max_h = 0,
                 std::string* why = nullptr, std::string* detail = nullptr);
bool SavePng(const std::string& path, const ImageRGBA& image);
// An RGB565 picture (Snes9x's) as RGBA8.
ImageRGBA FromRgb565(const uint16_t* pixels, uint32_t width, uint32_t height, size_t pitch_bytes);
// Box-filtered downscale (or the same image when it already fits).
ImageRGBA Shrink(const ImageRGBA& image, uint32_t max_w, uint32_t max_h);
