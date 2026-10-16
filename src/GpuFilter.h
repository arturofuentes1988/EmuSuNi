// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game picture drawn through the GPU's filters (shaders) instead of ImGui's plain textured
// quad: xBRZ at the TV's resolution for pixel art, AMD's FSR 1 and RCAS, and Anime4K's neural
// network. The passes before the last draw into images of their own (Display::EndFrame records
// them before the screen's render pass); the last one draws straight into the screen, inside
// ImGui's render pass, through a draw-list callback where the picture goes.

#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace GpuFilter
{
enum class Mode
{
  Off,
  Sharpen,   // HD Sharpen: smooth (bilinear) and sharpened by AMD's RCAS, in one pass
  PixelArt,  // Pixel Art HD: xBRZ (freescale), drawn at the size the picture has on the screen
  Smooth,    // Smooth HD: AMD FidelityFX Super Resolution 1 (EASU, then RCAS)
  AI,        // AI: xBRZ at 768 pixels wide, Anime4K's Restore network, Catmull-Rom to the screen
};
constexpr int MODE_COUNT = 5;

const char* Name(Mode mode);

// What it draws with: Display's device and the screen's render pass (RGBA8, subpass 0).
struct Context
{
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkRenderPass screen = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;  // the screen
  void (*wait_idle)() = nullptr;   // until the GPU is done with every frame Display has submitted
  std::string cache_path;          // the compiled pipelines, kept for the next start
  uint32_t queue_family = 0;       // Display's queue (its timestamps' valid bits)
};

// Display::Initialize, once its render pass exists. False: no filters (logged); pictures are
// drawn as before.
bool Init(const Context& context);
void Shutdown();

// The mode's pipelines are compiled. They are compiled on a thread of their own the first time a
// mode is asked for (quickly from the cache file after the first time); until then the picture
// is drawn as before.
bool Ready(Mode mode);

// The picture: an image fragment shaders can read, in that layout.
struct Source
{
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  uint32_t width = 0, height = 0;  // the picture, in the image's top-left corner
  bool exact = true;               // the image is the picture's size (else it is copied out first)
};

// DrawPicture: this frame's picture through the mode's passes, the last one into the screen at
// min..max (a callback in the draw list). False: not drawn (not compiled yet, too big, no
// memory): draw it as before.
bool Draw(ImDrawList* dl, Mode mode, const Source& source, ImVec2 min, ImVec2 max);

// Display::EndFrame, after the frame's fence and the game texture's upload, before the screen's
// render pass: the passes into images, for this frame (0..2).
void Record(VkCommandBuffer commands, uint32_t frame);

// The GPU's time for the filter since the last call (Vulkan timestamps around every pass, read
// back once the frame's fence has been waited for), as a log line: the average per frame, each
// pass, and the GPU clock's rate measured against the CPU's. False: nothing was measured.
bool TakeGpuTime(char* text, size_t size);
}  // namespace GpuFilter
