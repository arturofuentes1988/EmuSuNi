// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The display, from DolphinPS4's XMB display (DolphinNoGUI/PS4XMBDisplay.cpp): a minimal Vulkan
// renderer on Mesa's RADV (linked in) that draws Dear ImGui into three 1920x1080 linear images
// registered with VideoOut and flipped behind the GPU work. The menus and the game share it: the
// game's picture is one more texture (RGB565, as Snes9x renders it), refreshed every frame.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <imgui.h>

// Frames submitted so far (main.cpp's start-up watchdog).
extern std::atomic<unsigned> g_display_frames;

class Display
{
public:
  static constexpr uint32_t WIDTH = 1920;
  static constexpr uint32_t HEIGHT = 1080;
  // The game texture: big enough for every Snes9x picture (512x478 hi-res interlaced, 602 wide
  // with the NTSC filter) and for those Resolution makes bigger (6x: 1536x1434).
  static constexpr uint32_t GAME_TEXTURE_W = 1536;
  static constexpr uint32_t GAME_TEXTURE_H = 1440;

  // Creates the Vulkan device, the VideoOut buffers and ImGui's renderer; ImGui's context must
  // exist. Returns false (and logs) on failure.
  bool Initialize();
  void Shutdown();

  void BeginFrame();
  // Renders ImGui's draw data and queues the flip (vsync: waits while two flips are pending).
  void EndFrame();

  // An RGBA8 texture for ImDrawList::AddImage (0 on failure).
  ImTextureID CreateTexture(const void* rgba, uint32_t width, uint32_t height);
  void DestroyTexture(ImTextureID texture);

  // The next frame's game picture (RGB565, pitch in bytes); drawn with GameTexture().
  void UploadGameFrame(const void* pixels, uint32_t width, uint32_t height, size_t pitch);
  // The game picture with smooth (linear) or sharp (nearest) filtering.
  ImTextureID GameTexture(bool smooth) const;
  // V-Sync off: the flips happen at the next line instead of the next TV refresh.
  void SetVsync(bool on);
  // The screen EndFrame just drew (WIDTH x HEIGHT, RGBA8), once the GPU has drawn it.
  bool Capture(std::vector<uint8_t>* rgba);
};
