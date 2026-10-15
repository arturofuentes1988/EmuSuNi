// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Image.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>

#include <png.h>

bool LoadPng(const std::string& path, ImageRGBA* out, uint32_t max_w, uint32_t max_h)
{
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return false;
  png_byte header[8];
  if (fread(header, 1, 8, file) != 8 || png_sig_cmp(header, 0, 8) != 0)
  {
    fclose(file);
    return false;
  }
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info)
  {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(file);
    return false;
  }
  ImageRGBA image;
  std::vector<png_bytep> rows;
  if (setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(file);
    return false;
  }
  png_init_io(png, file);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);
  const png_byte color = png_get_color_type(png, info);
  const png_byte depth = png_get_bit_depth(png, info);
  if (depth == 16)
    png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);
  image.width = png_get_image_width(png, info);
  image.height = png_get_image_height(png, info);
  if (image.width == 0 || image.height == 0 || image.width > 8192 || image.height > 8192)
  {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(file);
    return false;
  }
  image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
  rows.resize(image.height);
  for (uint32_t y = 0; y < image.height; y++)
    rows[y] = image.pixels.data() + static_cast<size_t>(y) * image.width * 4;
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);
  fclose(file);
  *out = (max_w || max_h) ? Shrink(image, max_w ? max_w : image.width, max_h ? max_h : image.height) :
                            std::move(image);
  return true;
}

bool SavePng(const std::string& path, const ImageRGBA& image)
{
  if (image.empty())
    return false;
  FILE* file = fopen(path.c_str(), "wb");
  if (!file)
    return false;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info || setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    fclose(file);
    return false;
  }
  png_init_io(png, file);
  png_set_compression_level(png, 3);
  png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  for (uint32_t y = 0; y < image.height; y++)
  {
    png_write_row(png, const_cast<png_bytep>(image.pixels.data() +
                                             static_cast<size_t>(y) * image.width * 4));
  }
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  fclose(file);
  return true;
}

ImageRGBA FromRgb565(const uint16_t* pixels, uint32_t width, uint32_t height, size_t pitch_bytes)
{
  ImageRGBA image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; y++)
  {
    const uint16_t* row = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(pixels) + y * pitch_bytes);
    uint8_t* out = image.pixels.data() + static_cast<size_t>(y) * width * 4;
    for (uint32_t x = 0; x < width; x++)
    {
      const uint16_t p = row[x];
      const uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
      out[x * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
      out[x * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
      out[x * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
      out[x * 4 + 3] = 255;
    }
  }
  return image;
}

ImageRGBA Shrink(const ImageRGBA& image, uint32_t max_w, uint32_t max_h)
{
  if (image.width <= max_w && image.height <= max_h)
    return image;
  const float scale = std::min(static_cast<float>(max_w) / image.width,
                               static_cast<float>(max_h) / image.height);
  ImageRGBA out;
  out.width = std::max(1u, static_cast<uint32_t>(image.width * scale));
  out.height = std::max(1u, static_cast<uint32_t>(image.height * scale));
  out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (uint32_t y = 0; y < out.height; y++)
  {
    const uint32_t y0 = y * image.height / out.height;
    const uint32_t y1 = std::max(y0 + 1, (y + 1) * image.height / out.height);
    for (uint32_t x = 0; x < out.width; x++)
    {
      const uint32_t x0 = x * image.width / out.width;
      const uint32_t x1 = std::max(x0 + 1, (x + 1) * image.width / out.width);
      uint32_t sum[4] = {};
      uint32_t count = 0;
      for (uint32_t sy = y0; sy < y1; sy++)
      {
        const uint8_t* row = image.pixels.data() + (static_cast<size_t>(sy) * image.width + x0) * 4;
        for (uint32_t sx = x0; sx < x1; sx++, row += 4)
        {
          sum[0] += row[0];
          sum[1] += row[1];
          sum[2] += row[2];
          sum[3] += row[3];
          count++;
        }
      }
      uint8_t* px = out.pixels.data() + (static_cast<size_t>(y) * out.width + x) * 4;
      for (int c = 0; c < 4; c++)
        px[c] = static_cast<uint8_t>(sum[c] / count);
    }
  }
  return out;
}
