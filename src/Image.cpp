// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Image.h"

#include <algorithm>
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <png.h>
// (libjpeg-turbo's: PacBrew's sysroot has it; jpeglib.h wants FILE and size_t first.)
#include <jpeglib.h>

namespace
{
// The biggest picture read, either side: a bigger one would take long to read even made smaller as it is read.
constexpr uint32_t MAX_SIDE = 16384;
// The biggest one kept at its own size (no size asked for: the app's own pictures), as before 00.81.
constexpr uint32_t MAX_WHOLE_SIDE = 8192;
// The most memory a picture read whole may take: an interlaced PNG (all its pixels, before they are made smaller) or
// a JPEG of several scans - a progressive one - (libjpeg keeps all its coefficients, 128 bytes per 8x8 block, whatever
// size it is scaled to). Bigger ones are refused: the PS4's memory is short, and a C++ allocation that fails closes
// the app.
constexpr size_t MAX_WHOLE_BYTES = size_t{48} << 20;  // (a 4000 x 4000 progressive JPEG: 46 MiB)

// Why a picture isn't read, in plain words (assets/xmb/lang translates each sentence).
constexpr const char* WHY_OPEN = "The file can't be opened.";
constexpr const char* WHY_EMPTY = "The file is empty.";
constexpr const char* WHY_WEBP = "It is a WebP picture: only PNG and JPEG pictures are read.";
constexpr const char* WHY_GIF = "It is a GIF picture: only PNG and JPEG pictures are read.";
constexpr const char* WHY_BMP = "It is a BMP picture: only PNG and JPEG pictures are read.";
constexpr const char* WHY_HEIC = "It is a HEIC or AVIF picture: only PNG and JPEG pictures are read.";
constexpr const char* WHY_TIFF = "It is a TIFF picture: only PNG and JPEG pictures are read.";
constexpr const char* WHY_PAGE = "It is a web page, not a picture.";
constexpr const char* WHY_OTHER = "It isn't a PNG or JPEG picture.";
constexpr const char* WHY_BIG = "It is too big: over 16384 pixels wide or high.";
constexpr const char* WHY_WHOLE =
    "It is too big for its kind (a progressive JPEG or an interlaced PNG): save it smaller or as a normal one.";
constexpr const char* WHY_DAMAGED = "The picture is damaged or cut short.";
constexpr const char* WHY_MEMORY = "There wasn't enough memory to read it.";
constexpr const char* WHY_NOT_PNG = "It isn't a PNG picture.";  // (LoadPng: not said in the menu)

// What the first bytes say the file is: nullptr for a PNG or a JPEG (*jpeg), else why it isn't read.
const char* Sniff(const unsigned char* head, size_t size, bool* jpeg)
{
  auto at = [&](size_t offset, const char* magic, size_t length) {
    return size >= offset + length && std::memcmp(head + offset, magic, length) == 0;
  };
  *jpeg = false;
  if (size == 0)
    return WHY_EMPTY;
  if (at(0, "\x89PNG\r\n\x1a\n", 8))
    return nullptr;
  if (at(0, "\xff\xd8\xff", 3))
  {
    *jpeg = true;
    return nullptr;
  }
  if (at(0, "RIFF", 4) && at(8, "WEBP", 4))
    return WHY_WEBP;
  if (at(0, "GIF8", 4))
    return WHY_GIF;
  if (at(0, "BM", 2))
    return WHY_BMP;
  if (at(4, "ftyp", 4))
    return WHY_HEIC;
  if (at(0, "II*\0", 4) || at(0, "MM\0*", 4))
    return WHY_TIFF;
  size_t first = at(0, "\xef\xbb\xbf", 3) ? 3 : 0;  // (a UTF-8 mark)
  while (first < size && (head[first] == ' ' || head[first] == '\t' || head[first] == '\r' || head[first] == '\n'))
    first++;
  if (first < size && head[first] == '<')
    return WHY_PAGE;
  return WHY_OTHER;
}

// Shrink()'s size for a w x h picture (the picture's own when it fits; 0: no limit).
void ShrunkSize(uint32_t w, uint32_t h, uint32_t max_w, uint32_t max_h, uint32_t* out_w, uint32_t* out_h)
{
  max_w = max_w ? max_w : w;
  max_h = max_h ? max_h : h;
  if (w <= max_w && h <= max_h)
  {
    *out_w = w;
    *out_h = h;
    return;
  }
  const float scale = std::min(static_cast<float>(max_w) / w, static_cast<float>(max_h) / h);
  *out_w = std::max(1u, static_cast<uint32_t>(w * scale));
  *out_h = std::max(1u, static_cast<uint32_t>(h * scale));
}

// Shrink()'s box filter fed the picture a row at a time, top to bottom: one row of sums kept, never the whole
// picture, and the same pixels as Shrink() of the whole picture (each box: the picture's columns x * w / out_w up to
// (x + 1) * w / out_w, its rows the same way).
class RowShrink
{
public:
  // A w x h picture made out_w x out_h (at most its own size) in *out.
  void Start(uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h, ImageRGBA* out)
  {
    m_w = w;
    m_h = h;
    m_out = out;
    m_y = 0;
    m_out_y = 0;
    out->width = out_w;
    out->height = out_h;
    out->pixels.assign(static_cast<size_t>(out_w) * out_h * 4, 0);
    m_same = out_w == w && out_h == h;
    if (m_same)
      return;
    m_cols.resize(out_w + 1);
    for (uint32_t x = 0; x <= out_w; x++)
      m_cols[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / out_w);
    m_sums.assign(static_cast<size_t>(out_w) * 4, 0);
    m_row_end = RowStart(1);
  }
  // The picture's next row (w RGBA8 pixels).
  void Row(const uint8_t* pixels)
  {
    if (m_y >= m_h)
      return;
    ImageRGBA& out = *m_out;
    if (m_same)
    {
      std::memcpy(out.pixels.data() + static_cast<size_t>(m_y) * m_w * 4, pixels, static_cast<size_t>(m_w) * 4);
      m_y++;
      return;
    }
    uint64_t* sum = m_sums.data();
    for (uint32_t x = 0; x < out.width; x++, sum += 4)
    {
      const uint8_t* p = pixels + static_cast<size_t>(m_cols[x]) * 4;
      for (uint32_t sx = m_cols[x]; sx < m_cols[x + 1]; sx++, p += 4)
      {
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
        sum[3] += p[3];
      }
    }
    m_y++;
    if (m_y < m_row_end)
      return;
    // The result's row is whole: each pixel its box's average.
    const uint64_t rows = m_row_end - RowStart(m_out_y);
    uint8_t* px = out.pixels.data() + static_cast<size_t>(m_out_y) * out.width * 4;
    sum = m_sums.data();
    for (uint32_t x = 0; x < out.width; x++, px += 4, sum += 4)
    {
      const uint64_t count = (m_cols[x + 1] - m_cols[x]) * rows;
      for (int c = 0; c < 4; c++)
        px[c] = static_cast<uint8_t>(sum[c] / count);
    }
    std::fill(m_sums.begin(), m_sums.end(), 0);
    m_out_y++;
    m_row_end = m_out_y < out.height ? RowStart(m_out_y + 1) : m_h;
  }

private:
  uint32_t RowStart(uint32_t out_y) const
  {
    return static_cast<uint32_t>(static_cast<uint64_t>(out_y) * m_h / m_out->height);
  }

  uint32_t m_w = 0, m_h = 0, m_y = 0, m_out_y = 0, m_row_end = 0;
  bool m_same = false;
  ImageRGBA* m_out = nullptr;
  std::vector<uint32_t> m_cols;  // the result's columns' first picture column (out_w + 1: the end too)
  std::vector<uint64_t> m_sums;  // the result's row being made: RGBA sums
};

// ---- PNG (libpng) ----

// The reader's state that libpng's longjmp leaves as it is (the caller's, not ReadPng's locals).
struct PngState
{
  char message[160] = {};
  uint8_t* whole = nullptr;  // an interlaced picture's pixels (malloc: a failure is no C++ exception)
  std::vector<uint8_t> row;
};

void PngError(png_structp png, png_const_charp message)
{
  PngState* state = static_cast<PngState*>(png_get_error_ptr(png));
  std::snprintf(state->message, sizeof(state->message), "%s", message ? message : "");
  png_longjmp(png, 1);
}

void PngWarning(png_structp, png_const_charp)
{
  // (A known-incorrect sRGB profile and the like: the picture is fine. Not one log line per cover.)
}

// nullptr: read; else why not. (No C++ objects made here: libpng leaves by longjmp.)
const char* ReadPng(FILE* file, uint32_t max_w, uint32_t max_h, PngState* state, RowShrink* shrink,
                    ImageRGBA* image)
{
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, state, PngError, PngWarning);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info)
  {
    png_destroy_read_struct(&png, &info, nullptr);
    return WHY_MEMORY;
  }
  if (setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, nullptr);
    return std::strstr(state->message, "memory") ? WHY_MEMORY : WHY_DAMAGED;
  }
  png_init_io(png, file);
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
  const int passes = png_set_interlace_handling(png);
  png_read_update_info(png, info);
  const uint32_t w = png_get_image_width(png, info), h = png_get_image_height(png, info);
  const char* refused = nullptr;
  if (w == 0 || h == 0 || png_get_rowbytes(png, info) != static_cast<size_t>(w) * 4)
    refused = WHY_DAMAGED;
  else if (w > MAX_SIDE || h > MAX_SIDE || (!max_w && !max_h && (w > MAX_WHOLE_SIDE || h > MAX_WHOLE_SIDE)))
    refused = WHY_BIG;
  else if (passes > 1 && static_cast<size_t>(w) * h * 4 > MAX_WHOLE_BYTES)
    refused = WHY_WHOLE;
  if (refused)
  {
    png_destroy_read_struct(&png, &info, nullptr);
    return refused;
  }
  uint32_t out_w, out_h;
  ShrunkSize(w, h, max_w, max_h, &out_w, &out_h);
  shrink->Start(w, h, out_w, out_h, image);
  const size_t stride = static_cast<size_t>(w) * 4;
  if (passes > 1)
  {
    // Interlaced: every pass fills in some of each row's pixels, so the whole picture first.
    state->whole = static_cast<uint8_t*>(std::malloc(stride * h));
    if (!state->whole)
    {
      png_destroy_read_struct(&png, &info, nullptr);
      return WHY_MEMORY;
    }
    for (int pass = 0; pass < passes; pass++)
    {
      for (uint32_t y = 0; y < h; y++)
        png_read_row(png, state->whole + y * stride, nullptr);
    }
    for (uint32_t y = 0; y < h; y++)
      shrink->Row(state->whole + y * stride);
  }
  else
  {
    state->row.resize(stride);
    for (uint32_t y = 0; y < h; y++)
    {
      png_read_row(png, state->row.data(), nullptr);
      shrink->Row(state->row.data());
    }
  }
  // (Not png_read_end: what follows the pixels - text, a damaged chunk after them - isn't needed.)
  png_destroy_read_struct(&png, &info, nullptr);
  return nullptr;
}

// ---- JPEG (libjpeg-turbo) ----

struct JpegState
{
  jpeg_error_mgr manager;  // first: libjpeg's err points at it, and JpegExit finds the rest from it
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void JpegExit(j_common_ptr info)
{
  JpegState* state = reinterpret_cast<JpegState*>(info->err);
  state->message[0] = '\0';
  (*info->err->format_message)(info, state->message);
  std::longjmp(state->jump, 1);
}

void JpegMessage(j_common_ptr, int)
{
  // (Warnings - a picture cut short is shown as far as it goes - and traces: not in the log.)
}

// A CMYK picture's row as RGBA (an Adobe one's inks are stored inverted, as Photoshop writes them).
void CmykToRgba(uint8_t* p, uint32_t count, bool inverted)
{
  for (uint32_t i = 0; i < count; i++, p += 4)
  {
    unsigned c = p[0], m = p[1], y = p[2], k = p[3];
    if (!inverted)
    {
      c = 255 - c;
      m = 255 - m;
      y = 255 - y;
      k = 255 - k;
    }
    p[0] = static_cast<uint8_t>(c * k / 255);
    p[1] = static_cast<uint8_t>(m * k / 255);
    p[2] = static_cast<uint8_t>(y * k / 255);
    p[3] = 255;
  }
}

// nullptr: read; else why not. (No C++ objects made here: libjpeg leaves by longjmp.)
const char* ReadJpeg(FILE* file, uint32_t max_w, uint32_t max_h, JpegState* state, RowShrink* shrink,
                     ImageRGBA* image)
{
  jpeg_decompress_struct info{};  // (mem null: jpeg_destroy_decompress is safe whatever fails first)
  info.err = jpeg_std_error(&state->manager);
  state->manager.error_exit = JpegExit;
  state->manager.emit_message = JpegMessage;
  state->message[0] = '\0';
  if (setjmp(state->jump))
  {
    jpeg_destroy_decompress(&info);
    return std::strstr(state->message, "memory") ? WHY_MEMORY : WHY_DAMAGED;
  }
  jpeg_create_decompress(&info);
  jpeg_stdio_src(&info, file);
  const char* refused = nullptr;
  uint32_t w = 0, h = 0;
  if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK)
  {
    refused = WHY_DAMAGED;
  }
  else
  {
    w = info.image_width;
    h = info.image_height;
    if (w == 0 || h == 0)
      refused = WHY_DAMAGED;
    else if (w > MAX_SIDE || h > MAX_SIDE || (!max_w && !max_h && (w > MAX_WHOLE_SIDE || h > MAX_WHOLE_SIDE)))
      refused = WHY_BIG;
  }
  if (!refused && jpeg_has_multiple_scans(&info))
  {
    // Several scans (a progressive JPEG): libjpeg keeps every 8x8 block's coefficients until the last one.
    size_t bytes = 0;
    for (int c = 0; c < info.num_components; c++)
    {
      const jpeg_component_info& component = info.comp_info[c];
      const size_t across = (component.width_in_blocks + component.h_samp_factor - 1) / component.h_samp_factor *
                            static_cast<size_t>(component.h_samp_factor);
      const size_t down = (component.height_in_blocks + component.v_samp_factor - 1) / component.v_samp_factor *
                          static_cast<size_t>(component.v_samp_factor);
      bytes += across * down * sizeof(JBLOCK);
    }
    if (bytes > MAX_WHOLE_BYTES)
      refused = WHY_WHOLE;
  }
  if (refused)
  {
    jpeg_destroy_decompress(&info);
    return refused;
  }
  // libjpeg's own scaling (1/2, 1/4, 1/8: fewer coefficients turned into pixels, the cheapest way to make a big
  // picture small): the smallest picture still twice the size it ends at, which the box filter then makes that size
  // (only just bigger, libjpeg's scaled colours show at sharp edges).
  uint32_t final_w, final_h;
  ShrunkSize(w, h, max_w, max_h, &final_w, &final_h);
  unsigned denominator = 1;
  for (const unsigned d : {8u, 4u, 2u})
  {
    if ((w + d - 1) / d >= 2 * final_w && (h + d - 1) / d >= 2 * final_h)
    {
      denominator = d;
      break;
    }
  }
  info.scale_num = 1;
  info.scale_denom = denominator;
  const bool cmyk = info.jpeg_color_space == JCS_CMYK || info.jpeg_color_space == JCS_YCCK;
  info.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_RGBA;
  jpeg_start_decompress(&info);
  const uint32_t scaled_w = info.output_width, scaled_h = info.output_height;
  if (info.output_components != 4 || scaled_w == 0 || scaled_h == 0)
  {
    jpeg_destroy_decompress(&info);
    return WHY_DAMAGED;
  }
  uint32_t out_w, out_h;
  ShrunkSize(scaled_w, scaled_h, max_w, max_h, &out_w, &out_h);
  shrink->Start(scaled_w, scaled_h, out_w, out_h, image);
  JSAMPARRAY row = (*info.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&info), JPOOL_IMAGE, scaled_w * 4, 1);
  const bool inverted = info.saw_Adobe_marker;
  while (info.output_scanline < scaled_h)
  {
    if (jpeg_read_scanlines(&info, row, 1) != 1)
    {
      jpeg_destroy_decompress(&info);
      return WHY_DAMAGED;
    }
    if (cmyk)
      CmykToRgba(row[0], scaled_w, inverted);
    shrink->Row(row[0]);
  }
  // (Not jpeg_finish_decompress: the rest of the file isn't needed.)
  jpeg_destroy_decompress(&info);
  return nullptr;
}

bool Load(const std::string& path, ImageRGBA* out, uint32_t max_w, uint32_t max_h, bool png_only, std::string* why,
          std::string* detail)
{
  auto fail = [&](const char* reason, const char* words) {
    if (why)
      *why = reason;
    if (detail)
      *detail = words ? words : "";
    return false;
  };
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return fail(WHY_OPEN, std::strerror(errno));
  unsigned char head[16] = {};
  const size_t size = std::fread(head, 1, sizeof(head), file);
  bool jpeg = false;
  const char* reason = Sniff(head, size, &jpeg);
  if (!reason && jpeg && png_only)
    reason = WHY_NOT_PNG;
  if (reason || std::fseek(file, 0, SEEK_SET) != 0)
  {
    std::fclose(file);
    return fail(reason ? reason : WHY_OPEN, nullptr);
  }
  ImageRGBA image;
  RowShrink shrink;
  std::string words;
  if (jpeg)
  {
    JpegState state;
    reason = ReadJpeg(file, max_w, max_h, &state, &shrink, &image);
    words = state.message;
  }
  else
  {
    PngState state;
    reason = ReadPng(file, max_w, max_h, &state, &shrink, &image);
    std::free(state.whole);
    words = state.message;
  }
  std::fclose(file);
  if (reason)
    return fail(reason, words.c_str());
  *out = std::move(image);
  return true;
}
}  // namespace

bool LoadPng(const std::string& path, ImageRGBA* out, uint32_t max_w, uint32_t max_h)
{
  return Load(path, out, max_w, max_h, true, nullptr, nullptr);
}

bool LoadPicture(const std::string& path, ImageRGBA* out, uint32_t max_w, uint32_t max_h, std::string* why,
                 std::string* detail)
{
  return Load(path, out, max_w, max_h, false, why, detail);
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
