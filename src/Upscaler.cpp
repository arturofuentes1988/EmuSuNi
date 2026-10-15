// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Upscaler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Log.h"

// Snes9x's filters (filter/xbrz.cpp and filter/hq2x.cpp, built with the core).
#include "filter/hq2x.h"
#include "filter/xbrz.h"

namespace Upscaler
{
namespace
{
// Four workers and the calling thread (the emulator's): five slices of rows. The PS4 gives a game
// six cores; the others run the sound and the system.
constexpr int WORKERS = 4;
constexpr int SLICES = WORKERS + 1;

std::vector<std::thread> s_threads;
bool s_started = false;
std::mutex s_mutex;
std::condition_variable s_wake, s_done;
const std::function<void(int)>* s_job = nullptr;
unsigned s_job_id = 0;
int s_pending = 0;
bool s_stop = false;

// xBRZ's colour distance table: 0 not made, 1 being made, 2 ready, 3 failed.
std::atomic<int> s_xbrz_state{0};
std::thread s_xbrz_thread;

std::vector<uint32_t> s_src32, s_trg32;  // xBRZ works on 32-bit pixels
std::vector<uint16_t> s_window[SLICES];   // HQx: each slice's rows and one more above and below

uint32_t To32(uint16_t p)
{
  const uint32_t r = p >> 11, g = (p >> 5) & 0x3F, b = p & 0x1F;
  return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

uint16_t To16(uint32_t p)
{
  return static_cast<uint16_t>(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
}

void Worker(int slice, unsigned seen)
{
  for (;;)
  {
    const std::function<void(int)>* job;
    {
      std::unique_lock lock(s_mutex);
      s_wake.wait(lock, [&] { return s_stop || s_job_id != seen; });
      if (s_stop)
        return;
      seen = s_job_id;
      job = s_job;
    }
    (*job)(slice);
    std::lock_guard lock(s_mutex);
    if (--s_pending == 0)
      s_done.notify_one();
  }
}

void Start()
{
  s_started = true;
  s_stop = false;
  try
  {
    for (int i = 0; i < WORKERS; i++)
      s_threads.emplace_back(Worker, i + 1, s_job_id);
  }
  catch (...)
  {
  }
  LogF("upscaler: %zu worker threads", s_threads.size());
}

// job(0) ... job(SLICES - 1): the workers take slices 1 to their number, the caller the rest.
void RunSlices(const std::function<void(int)>& job)
{
  const int workers = static_cast<int>(s_threads.size());
  if (workers > 0)
  {
    {
      std::lock_guard lock(s_mutex);
      s_job = &job;
      s_pending = workers;
      s_job_id++;
    }
    s_wake.notify_all();
  }
  job(0);
  for (int slice = workers + 1; slice < SLICES; slice++)
    job(slice);
  if (workers > 0)
  {
    std::unique_lock lock(s_mutex);
    s_done.wait(lock, [] { return s_pending == 0; });
    s_job = nullptr;
  }
}

// The source rows of a slice.
void SliceRows(int slice, int height, int* first, int* last)
{
  *first = height * slice / SLICES;
  *last = height * (slice + 1) / SLICES;
}
}  // namespace

void Stop()
{
  {
    std::lock_guard lock(s_mutex);
    s_stop = true;
  }
  s_wake.notify_all();
  for (std::thread& thread : s_threads)
    thread.join();
  s_threads.clear();
  s_started = false;
  s_stop = false;
  if (s_xbrz_thread.joinable())
    s_xbrz_thread.join();
  // The big buffers go back to the heap (the table stays: it is made once).
  std::vector<uint32_t>().swap(s_src32);
  std::vector<uint32_t>().swap(s_trg32);
  for (std::vector<uint16_t>& window : s_window)
    std::vector<uint16_t>().swap(window);
}

bool Ready(Filter filter)
{
  if (filter != Filter::XBRZ)
    return true;
  const int state = s_xbrz_state.load();
  if (state == 2)
    return true;
  if (state != 0)
    return false;
  s_xbrz_state = 1;
  if (s_xbrz_thread.joinable())
    s_xbrz_thread.join();
  try
  {
    s_xbrz_thread = std::thread([] {
      const auto start = std::chrono::steady_clock::now();
      // A tiny picture: the first use makes the table.
      uint32_t src[4 * 4] = {}, trg[8 * 8] = {};
      src[5] = 0xFFFFFF;
      xbrz::scale(2, src, trg, 4, 4, xbrz::ColorFormat::RGB);
      LogF("upscaler: xBRZ ready (%.0f ms)",
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      s_xbrz_state = 2;
    });
  }
  catch (...)
  {
    LogLine("upscaler: xBRZ's thread couldn't be made");
    s_xbrz_state = 3;
  }
  return false;
}

int Scale(Filter filter, int factor, const uint16_t* src, size_t pitch, int width, int height,
          std::vector<uint16_t>* out)
{
  factor = std::clamp(factor, 1, filter == Filter::HQx ? 4 : xbrz::SCALE_FACTOR_MAX);
  if (factor < 2 || !src || width <= 0 || height <= 0 || !Ready(filter))
    return 1;
  if (!s_started)
    Start();
  const int out_w = width * factor;
  out->resize(static_cast<size_t>(out_w) * height * factor);
  uint16_t* dst = out->data();
  auto source_row = [&](int y) {
    return reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src) + static_cast<size_t>(y) * pitch);
  };
  switch (filter)
  {
  case Filter::XBRZ:
  {
    s_src32.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++)
    {
      const uint16_t* row = source_row(y);
      uint32_t* to = &s_src32[static_cast<size_t>(y) * width];
      for (int x = 0; x < width; x++)
        to[x] = To32(row[x]);
    }
    s_trg32.resize(out->size());
    // Disjoint slices of one picture may run at once (xbrz.h).
    RunSlices([&](int slice) {
      int first, last;
      SliceRows(slice, height, &first, &last);
      if (first >= last)
        return;
      xbrz::scale(factor, s_src32.data(), s_trg32.data(), width, height, xbrz::ColorFormat::RGB,
                  xbrz::ScalerCfg(), first, last);
      const size_t begin = static_cast<size_t>(first) * factor * out_w;
      const size_t end = static_cast<size_t>(last) * factor * out_w;
      for (size_t i = begin; i < end; i++)
        dst[i] = To16(s_trg32[i]);
    });
    break;
  }
  case Filter::HQx:
  {
    using Function = void (*)(uint8_t*, int, uint8_t*, int, int, int);
    const Function hq = factor == 2 ? HQ2X_16 : factor == 3 ? HQ3X_16 : HQ4X_16;
    // The filter makes its colour table on first use: once here, before the threads share it.
    static bool table_made = false;
    if (!table_made)
    {
      uint16_t in[2 * 2] = {}, made[4 * 4] = {};
      HQ2X_16(reinterpret_cast<uint8_t*>(in), 4, reinterpret_cast<uint8_t*>(made), 8, 2, 2);
      table_made = true;
    }
    // Each slice with a row more above and below (HQx looks one row around), kept apart.
    RunSlices([&](int slice) {
      int first, last;
      SliceRows(slice, height, &first, &last);
      if (first >= last)
        return;
      const int top = std::max(0, first - 1), bottom = std::min(height, last + 1);
      std::vector<uint16_t>& window = s_window[slice];
      window.resize(static_cast<size_t>(out_w) * (bottom - top) * factor);
      hq(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(source_row(top))), static_cast<int>(pitch),
         reinterpret_cast<uint8_t*>(window.data()), out_w * 2, width, bottom - top);
      std::memcpy(dst + static_cast<size_t>(first) * factor * out_w,
                  window.data() + static_cast<size_t>(first - top) * factor * out_w,
                  static_cast<size_t>(last - first) * factor * out_w * 2);
    });
    break;
  }
  case Filter::Pixels:
    RunSlices([&](int slice) {
      int first, last;
      SliceRows(slice, height, &first, &last);
      for (int y = first; y < last; y++)
      {
        const uint16_t* row = source_row(y);
        uint16_t* to = dst + static_cast<size_t>(y) * factor * out_w;
        for (int x = 0; x < width; x++)
        {
          const uint16_t pixel = row[x];
          for (int k = 0; k < factor; k++)
            to[x * factor + k] = pixel;
        }
        for (int k = 1; k < factor; k++)
          std::memcpy(to + static_cast<size_t>(k) * out_w, to, static_cast<size_t>(out_w) * 2);
      }
    });
    break;
  }
  return factor;
}
}  // namespace Upscaler
