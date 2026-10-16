// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The display, from DolphinPS4's XMB display (DolphinNoGUI/PS4XMBDisplay.cpp): a minimal Vulkan
// renderer on Mesa's RADV (linked in) that draws Dear ImGui into three 1920x1080 linear images
// registered with VideoOut and flipped behind the GPU work. The menus and the game share it: the
// game's picture is one more texture (RGB565, as Snes9x renders it), refreshed every frame - or,
// for a core that draws with Vulkan itself (the N64 core's ParaLLEl-RDP, in the N64 build), the
// core's own image, on the same device (libretro's Vulkan interface: see Emulator.cpp).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <imgui.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

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
  // The game texture itself (the picture in its top-left corner), for the GPU filters.
  VkImageView GameView() const;
  // V-Sync off: the flips happen at the next line instead of the next TV refresh.
  void SetVsync(bool on);
  // The time spent waiting for the TV and the GPU since the last call (ms).
  double TakeWaitMs();
  // The screen EndFrame just drew (WIDTH x HEIGHT, RGBA8), once the GPU has drawn it.
  bool Capture(std::vector<uint8_t>* rgba);

  // ---- For a core that draws with Vulkan ----
  // The device was made by ParaLLEl-RDP (with what it needs), so a core can draw on it.
  bool SharesVulkan() const;
  VkInstance Instance() const;
  VkPhysicalDevice Gpu() const;
  VkDevice Device() const;
  VkQueue Queue() const;
  uint32_t QueueFamily() const;
  // Every use of the queue holds this lock (the core submits from its own threads).
  void LockQueue();
  void UnlockQueue();
  // The frame EndFrame draws next (0..2) and all of them, as bits; WaitSyncIndex() waits until
  // the GPU is done with that frame's last use (its resources can be used again).
  uint32_t SyncIndex() const;
  uint32_t SyncIndexMask() const;
  void WaitSyncIndex();
  // The next frame waits for these semaphores (the core's image is ready) and signals that one.
  void WaitForSemaphores(uint32_t count, const VkSemaphore* semaphores);
  void SignalSemaphore(VkSemaphore semaphore);
  // The core's image (a view in that layout), for ImDrawList::AddImage in the next frame.
  ImTextureID ExternalTexture(VkImageView view, VkImageLayout layout, bool smooth);
  // Until the GPU has drawn every frame (before a core frees images they show).
  void WaitIdle();
};
