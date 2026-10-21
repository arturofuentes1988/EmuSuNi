// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GpuFilter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

#include <imgui_impl_vulkan.h>

#ifdef __PS4__
#include <orbis/libkernel.h>
// libkernel's affinity call as it really is (bit n of the mask is core n).
extern "C" int emusuni_filter_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");
#endif

#include "Log.h"
#include "VulkanFunctions.h"

// The shaders as SPIR-V: CMakeLists.txt compiles src/shaders with glslangValidator.
#include "catmull.frag.h"
#include "copy.frag.h"
#include "crt.frag.h"
#include "crt_glow.frag.h"
#include "easu.frag.h"
#include "mmpx.frag.h"
#include "quad.vert.h"
#include "rcas.frag.h"
#include "restore_m_0.frag.h"
#include "restore_m_1.frag.h"
#include "restore_m_2.frag.h"
#include "restore_m_3.frag.h"
#include "restore_m_4.frag.h"
#include "restore_m_5.frag.h"
#include "restore_m_6.frag.h"
#include "restore_m_7.frag.h"
#include "restore_m_7_partial.frag.h"
#include "sharp_bilinear.frag.h"
#include "sharpen.frag.h"
#include "smaa_blend.frag.h"
#include "smaa_edges.frag.h"
#include "smaa_weights.frag.h"
#include "xbrz0.frag.h"
#include "xbrz1.frag.h"
// SMAA's two lookup textures (MIT, see THIRD_PARTY_NOTICES.md), uploaded the first time a mode
// needs them.
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"

namespace GpuFilter
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr uint32_t FRAMES = 3;      // Display's frames in flight
constexpr uint32_t MAX_INPUTS = 8;  // the textures one pass reads (Anime4K's last pass: 8)
constexpr uint32_t MAX_PASSES = 16;
// The passes' images, in one block of GPU memory kept for the app's life (each allocation maps
// memory on the PS4, see Display.cpp). The AI's network takes the most: 7 RGBA16F images of at most
// 960x720 pixels and a few RGBA8 ones, about 46 MiB (the rest: the driver's padding; Natural AI's
// biggest chain, the DS's screens one above the other, about 51 MiB).
constexpr VkDeviceSize BLOCK_SIZE = 64ull << 20;
// The AI redraws the picture this wide with xBRZ (3x for the SNES and the NES) before the network
// cleans it up: wider costs much more GPU memory and time for little.
constexpr uint32_t AI_WIDTH = 768;
// A picture already drawn wider than that (the PlayStation's 3D at 2x-4x) is made smaller with
// Catmull-Rom instead, to at most this many pixels (its edges come out supersampled), and the
// network cleans that up: about the AI's usual size, so the same GPU memory and time.
constexpr double AI_DRAWN_PIXELS = 960.0 * 720.0;
// Smooth HD's image is as big as the picture on the screen (Letterbox Zoom makes it bigger).
constexpr uint32_t MAX_OUTPUT_W = 2560, MAX_OUTPUT_H = 1600;
// RCAS's sharpening, in stops (0 = the most): Smooth HD after EASU, HD Sharpen.
constexpr float FSR_SHARPNESS_STOPS = 0.25f;
constexpr float SHARPEN_STOPS = 0.25f;
// Natural AI: how much of the network's correction is kept, the rest being its input (Smooth
// Edges' picture). 1 is the AI's full look, which flattens textures and dithering into a waxy,
// painted picture; 0.7 keeps them and still cleans the edges (real SNES, NES and PlayStation
// frames).
constexpr float NATURAL_AI_STRENGTH = 0.7f;
// Natural AI's network size: about 768x576 (the AI's), more for a picture whose 2x redraw is wider
// (the DS's two screens side by side) up to this; a drawn picture bigger than the screen's (no SMAA
// images then) up to AI_DRAWN_PIXELS. The GPU memory block holds the biggest chains (PSP at 3x).
constexpr double SMALL_AI_PIXELS = 768.0 * 576.0;
// A drawn (3D) picture is anti-aliased (SMAA) at its own size only when the screen shows it bigger
// than it is (else it is supersampled already) and it has at most this many pixels: the N64, the
// PlayStation and the PSP up to 2x. The PSP at 3x (1440x810) is nearly the screen's size already,
// and SMAA there would take Natural HD past 2 ms of the PS4's GPU.
constexpr double SMAA_MAX_PIXELS = 1.0e6;
// Tube TV's looks (Source::crt 1..4, src/shaders/crt.frag): the glass's curvature across and down, the corners' radius
// (of the screen's height); the beam's hardness (the lines' darkness: exp2(hardness * d * d) d lines from a line's
// middle) and how much brighter parts widen it; the phosphors' mask (1 aperture grille, 2 slot mask, 3 dots) and its
// strength; the glass's glow; the vignette; the sharpness across (1: one screen pixel's step between texels, less is
// softer, as a TV's signal).
struct CrtLook
{
  float curve_x, curve_y, corner;
  float hardness, widening;
  uint32_t mask;
  float mask_strength, glow, vignette, sharpness;
};
const CrtLook CRT_LOOKS[4] = {
    {0.031f, 0.041f, 0.040f, -9.0f, 0.50f, 3, 0.22f, 0.10f, 0.22f, 0.30f},   // TV
    {0.022f, 0.030f, 0.028f, -12.0f, 0.45f, 2, 0.28f, 0.07f, 0.16f, 0.60f},  // Arcade Monitor
    {0.0f, 0.0f, 0.010f, -11.0f, 0.40f, 1, 0.18f, 0.04f, 0.06f, 1.00f},      // Studio Monitor: flat, crisp
    {0.045f, 0.060f, 0.055f, -16.0f, 0.45f, 2, 0.40f, 0.14f, 0.32f, 0.40f},  // Intense
};

enum class Target
{
  Screen,  // the screen's render pass (Display's)
  Rgba8,
  Rgba16f,
};
constexpr int TARGET_COUNT = 3;

enum Shader
{
  COPY,
  SHARPEN,
  XBRZ0,
  XBRZ1,
  EASU,
  RCAS,
  CATMULL,
  RESTORE0,  // Anime4K Restore CNN (M): 7 layers, then the last pass (RESTORE0 + 7)
  MMPX = RESTORE0 + 8,
  SMAA_EDGES,
  SMAA_WEIGHTS,
  SMAA_BLEND,
  SHARP_BILINEAR,
  RESTORE_PARTIAL,  // RESTORE0 + 7 with the network's correction scaled (Natural AI)
  CRT_GLOW,         // Tube TV: a picture drawn bigger made smaller for the glow
  CRT,              // Tube TV
  SHADER_COUNT,
};

struct ShaderCode
{
  const char* name;
  const uint32_t* code;
  size_t size;
};
#define EMUSUNI_SPV(name, array) {name, array, sizeof(array)}
const ShaderCode SHADERS[SHADER_COUNT] = {
    EMUSUNI_SPV("copy", emusuni_spv_copy_frag),
    EMUSUNI_SPV("sharpen", emusuni_spv_sharpen_frag),
    EMUSUNI_SPV("xbrz0", emusuni_spv_xbrz0_frag),
    EMUSUNI_SPV("xbrz1", emusuni_spv_xbrz1_frag),
    EMUSUNI_SPV("easu", emusuni_spv_easu_frag),
    EMUSUNI_SPV("rcas", emusuni_spv_rcas_frag),
    EMUSUNI_SPV("catmull", emusuni_spv_catmull_frag),
    EMUSUNI_SPV("restore0", emusuni_spv_restore_m_0_frag),
    EMUSUNI_SPV("restore1", emusuni_spv_restore_m_1_frag),
    EMUSUNI_SPV("restore2", emusuni_spv_restore_m_2_frag),
    EMUSUNI_SPV("restore3", emusuni_spv_restore_m_3_frag),
    EMUSUNI_SPV("restore4", emusuni_spv_restore_m_4_frag),
    EMUSUNI_SPV("restore5", emusuni_spv_restore_m_5_frag),
    EMUSUNI_SPV("restore6", emusuni_spv_restore_m_6_frag),
    EMUSUNI_SPV("restore7", emusuni_spv_restore_m_7_frag),
    EMUSUNI_SPV("mmpx", emusuni_spv_mmpx_frag),
    EMUSUNI_SPV("smaa-edges", emusuni_spv_smaa_edges_frag),
    EMUSUNI_SPV("smaa-weights", emusuni_spv_smaa_weights_frag),
    EMUSUNI_SPV("smaa-blend", emusuni_spv_smaa_blend_frag),
    EMUSUNI_SPV("sharp-bilinear", emusuni_spv_sharp_bilinear_frag),
    EMUSUNI_SPV("restore7-partial", emusuni_spv_restore_m_7_partial_frag),
    EMUSUNI_SPV("crt-glow", emusuni_spv_crt_glow_frag),
    EMUSUNI_SPV("crt", emusuni_spv_crt_frag),
};
#undef EMUSUNI_SPV

// src/shaders/common.glsl's push constants.
struct Push
{
  float clip[4];
  float source[4];
  float output[4];
  float origin[4];
  uint32_t con[16];
};
static_assert(sizeof(Push) == 128, "common.glsl's Push");

struct ImageSpec
{
  uint32_t width = 0, height = 0;
  Target target = Target::Rgba8;
  bool operator==(const ImageSpec& other) const
  {
    return width == other.width && height == other.height && target == other.target;
  }
};

struct Image
{
  ImageSpec spec;
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

// The inputs that aren't the chain's images: the picture, SMAA's lookup textures.
constexpr int INPUT_PICTURE = -1;
constexpr int INPUT_SMAA_AREA = -2;
constexpr int INPUT_SMAA_SEARCH = -3;

// One pass: reads its inputs (-1: the picture, -2/-3: SMAA's textures, else an image) and draws an
// image, or the screen (output -1, the last pass).
struct Pass
{
  Shader shader = COPY;
  Target target = Target::Rgba8;
  int output = -1;
  int inputs[MAX_INPUTS] = {};
  uint32_t input_count = 0;
  bool linear = false;
  uint32_t nearest_inputs = 0;  // inputs (bits) read with the nearest sampler although linear is set
  Push push = {};
};

// What Draw asked for this frame.
struct Job
{
  bool pending = false;   // Draw added the callback
  bool recorded = false;  // Record wrote the frame's descriptors and passes
  uint32_t frame = 0;
  Mode mode = Mode::Off;
  Source source;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the picture on the screen (whole pixels)
};

Context s_context;
bool s_ok = false;
bool s_rgba16f = false;  // RGBA16F images (the AI's network) can be drawn into and read
VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkPipelineLayout s_layout = VK_NULL_HANDLE;
VkDescriptorPool s_pool = VK_NULL_HANDLE;
VkDescriptorSet s_sets[FRAMES][MAX_PASSES] = {};
VkRenderPass s_render_passes[TARGET_COUNT] = {};  // [Screen] is Display's
VkSampler s_linear = VK_NULL_HANDLE, s_nearest = VK_NULL_HANDLE;
VkShaderModule s_vertex = VK_NULL_HANDLE;
VkShaderModule s_modules[SHADER_COUNT] = {};
VkPipelineCache s_cache = VK_NULL_HANDLE;
size_t s_cache_size = 0;  // what the cache file holds
// Written by the compiler thread, read once the mode that needs them is ready (s_ready).
VkPipeline s_pipelines[SHADER_COUNT][TARGET_COUNT] = {};
std::atomic<bool> s_ready[MODE_COUNT];
std::atomic<bool> s_failed[MODE_COUNT];
std::atomic<bool> s_compiling{false};
std::thread s_compiler;

VkDeviceMemory s_block = VK_NULL_HANDLE;
uint32_t s_block_type = 0;
std::vector<Image> s_images;
std::vector<ImageSpec> s_failed_specs;  // too big for the block: not tried again every frame
bool s_have_failed = false;
std::vector<Pass> s_chain;
Job s_job;
std::string s_logged;  // the last chain logged

// SMAA's lookup textures (area: 160x560 RG8, search: 64x16 R8), made the first time a chain needs
// them, uploaded by the next Record (a buffer of the bytes is kept until Shutdown: 180 KiB).
struct Lut
{
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};
Lut s_area, s_search;
VkDeviceMemory s_lut_memory = VK_NULL_HANDLE;
VkBuffer s_lut_buffer = VK_NULL_HANDLE;
VkDeviceMemory s_lut_buffer_memory = VK_NULL_HANDLE;
constexpr VkDeviceSize LUT_SEARCH_OFFSET = (AREATEX_SIZE + 255) / 256 * 256;  // in the buffer
bool s_luts_made = false;
bool s_luts_failed = false;
bool s_luts_uploaded = false;

// ---- GPU time: Vulkan timestamps (bottom of pipe) per frame slot ----
// [0] before the first pass, [1 + p] after pass p into its image, [MAX_PASSES + 1] and
// [MAX_PASSES + 2] around the pass to the screen. A slot is read back when Display records it
// again: EndFrame has waited for its fence, so no wait here. One query pool for the app's life
// (on the PS4 each allocation maps memory).
constexpr uint32_t QUERIES_PER_FRAME = MAX_PASSES + 3;
VkQueryPool s_queries = VK_NULL_HANDLE;
uint64_t s_tick_mask = ~0ull;  // the queue's timestampValidBits
double s_vulkan_mhz = 0.0;     // 1000 / timestampPeriod: what the driver says the clock is
struct Timed
{
  uint32_t images = 0;  // passes into images timed (0 with nothing recorded)
  bool screen = false;  // the pass to the screen timed
  Clock::time_point at;  // when the slot was recorded (the clock's rate, below)
};
Timed s_timed[FRAMES];
// The sums since TakeGpuTime, for one chain (a new chain starts them again).
std::string s_timed_chain;
double s_pass_ticks[MAX_PASSES] = {};  // passes into images, then [count - 1]: to the screen
uint32_t s_timed_count = 0;            // passes in that chain
uint32_t s_timed_frames = 0, s_screen_frames = 0;
// The GPU clock's rate against the CPU's: the first and last timed frames' start (GPU ticks) and
// when they were recorded (CPU). Over seconds the GPU's lag behind the recording averages out.
uint64_t s_first_ticks = 0, s_last_ticks = 0;
Clock::time_point s_first_at, s_last_at;
bool s_have_first = false;
// The main thread's time in Draw, Record and DrawFinal (building the chain, the descriptor
// writes, recording the passes), since TakeGpuTime.
double s_cpu_ms = 0.0;
uint32_t s_cpu_frames = 0;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool Check(VkResult result, const char* what)
{
  if (result == VK_SUCCESS)
    return true;
  LogF("gpu filter: %s failed (%d)", what, static_cast<int>(result));
  return false;
}

VkRenderPass CreateImagePass(VkFormat format)
{
  VkAttachmentDescription color = {};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is drawn
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &ref;
  // Before: the last frame's passes are done reading the image. After: the next passes (and the
  // screen's) read it.
  VkSubpassDependency dependencies[2] = {};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = 0;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 2;
  info.pDependencies = dependencies;
  VkRenderPass pass = VK_NULL_HANDLE;
  Check(vkCreateRenderPass(s_context.device, &info, nullptr, &pass), "vkCreateRenderPass");
  return pass;
}

VkFormat FormatOf(Target target)
{
  return target == Target::Rgba16f ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
}

VkSampler CreateSampler(VkFilter filter)
{
  VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = filter;
  info.minFilter = filter;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.maxLod = 0.0f;
  VkSampler sampler = VK_NULL_HANDLE;
  Check(vkCreateSampler(s_context.device, &info, nullptr, &sampler), "vkCreateSampler");
  return sampler;
}

VkShaderModule CreateModule(const uint32_t* code, size_t size)
{
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = size;
  info.pCode = code;
  VkShaderModule module = VK_NULL_HANDLE;
  Check(vkCreateShaderModule(s_context.device, &info, nullptr, &module), "vkCreateShaderModule");
  return module;
}

// ---- The pipelines (compiled on the compiler thread) ----
struct Need
{
  Shader shader;
  Target target;
};

std::vector<Need> Needs(Mode mode)
{
  std::vector<Need> needs = {{COPY, Target::Rgba8}};
  switch (mode)
  {
  case Mode::Sharpen:
    needs.push_back({SHARPEN, Target::Screen});
    break;
  case Mode::PixelArt:
    needs.push_back({XBRZ0, Target::Rgba8});
    needs.push_back({XBRZ1, Target::Screen});
    break;
  case Mode::Smooth:
    needs.push_back({EASU, Target::Rgba8});
    needs.push_back({RCAS, Target::Screen});
    break;
  case Mode::AI:
    needs.push_back({XBRZ0, Target::Rgba8});
    needs.push_back({XBRZ1, Target::Rgba8});
    needs.push_back({CATMULL, Target::Rgba8});  // a picture drawn bigger (AI_DRAWN_PIXELS)
    for (int layer = 0; layer < 7; layer++)
      needs.push_back({static_cast<Shader>(RESTORE0 + layer), Target::Rgba16f});
    needs.push_back({static_cast<Shader>(RESTORE0 + 7), Target::Rgba8});
    needs.push_back({CATMULL, Target::Screen});
    break;
  case Mode::Edges:
  case Mode::NaturalHD:
  case Mode::NaturalAI:
    // Pixel art redrawn at 2x or 4x, then SMAA (drawn pictures only SMAA); what follows depends on
    // the picture: every pipeline the mode can use.
    needs.push_back({MMPX, Target::Rgba8});
    needs.push_back({SMAA_EDGES, Target::Rgba8});
    needs.push_back({SMAA_WEIGHTS, Target::Rgba8});
    needs.push_back({SMAA_BLEND, Target::Rgba8});
    if (mode == Mode::Edges)
    {
      needs.push_back({SHARP_BILINEAR, Target::Screen});
      needs.push_back({CATMULL, Target::Screen});
    }
    else if (mode == Mode::NaturalHD)
    {
      needs.push_back({EASU, Target::Rgba8});
      needs.push_back({RCAS, Target::Screen});
      needs.push_back({SHARPEN, Target::Screen});
    }
    else
    {
      needs.push_back({CATMULL, Target::Rgba8});
      for (int layer = 0; layer < 7; layer++)
        needs.push_back({static_cast<Shader>(RESTORE0 + layer), Target::Rgba16f});
      needs.push_back({RESTORE_PARTIAL, Target::Rgba8});
      needs.push_back({CATMULL, Target::Screen});
    }
    break;
  case Mode::Crt:
    needs.push_back({CRT_GLOW, Target::Rgba8});
    needs.push_back({CRT, Target::Screen});
    break;
  case Mode::Off:
    needs.clear();
    break;
  }
  return needs;
}

VkPipeline CreatePipeline(Shader shader, Target target)
{
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = s_vertex;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = s_modules[shader];
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = states;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = s_layout;
  info.renderPass = s_render_passes[static_cast<int>(target)];
  info.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (!Check(vkCreateGraphicsPipelines(s_context.device, s_cache, 1, &info, nullptr, &pipeline),
             SHADERS[shader].name))
    return VK_NULL_HANDLE;
  return pipeline;
}

void SaveCache()
{
  size_t size = 0;
  if (vkGetPipelineCacheData(s_context.device, s_cache, &size, nullptr) != VK_SUCCESS || size == 0 ||
      size == s_cache_size)
    return;
  std::vector<uint8_t> data(size);
  if (vkGetPipelineCacheData(s_context.device, s_cache, &size, data.data()) != VK_SUCCESS)
    return;
  const std::string& path = s_context.cache_path;
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos)
    MakeDirs(path.substr(0, slash));
  const std::string temp = path + ".part";
  FILE* file = fopen(temp.c_str(), "wb");
  if (!file)
    return;
  const bool written = fwrite(data.data(), 1, size, file) == size;
  fclose(file);
  if (written && std::rename(temp.c_str(), path.c_str()) == 0)
  {
    s_cache_size = size;
    LogF("gpu filter: pipeline cache: %zu bytes kept", size);
  }
}

void Compile(Mode mode)
{
#ifdef __PS4__
  // Made by the emulation thread during a game: below every game thread, and off the emulation
  // thread's core (2, an N64 game's) whatever this thread inherited.
  scePthreadSetprio(scePthreadSelf(), 760);
  emusuni_filter_affinity(scePthreadSelf(), 0x3Bull);  // cores 0, 1, 3, 4 and 5
#endif
  const auto start = Clock::now();
  bool ok = true;
  int made = 0;
  for (const Need& need : Needs(mode))
  {
    VkPipeline& pipeline = s_pipelines[need.shader][static_cast<int>(need.target)];
    if (pipeline != VK_NULL_HANDLE)
      continue;
    pipeline = CreatePipeline(need.shader, need.target);
    if (pipeline == VK_NULL_HANDLE)
    {
      ok = false;
      break;
    }
    made++;
  }
  if (made > 0)
    SaveCache();
  LogF("gpu filter: %s %s (%d pipelines compiled in %.0f ms)", Name(mode), ok ? "ready" : "failed", made,
       std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  (ok ? s_ready : s_failed)[static_cast<int>(mode)].store(true, std::memory_order_release);
  s_compiling.store(false, std::memory_order_release);
}

// ---- The images ----
void DestroyImages()
{
  for (Image& image : s_images)
  {
    vkDestroyFramebuffer(s_context.device, image.framebuffer, nullptr);
    vkDestroyImageView(s_context.device, image.view, nullptr);
    vkDestroyImage(s_context.device, image.image, nullptr);
  }
  s_images.clear();
}

// The block of GPU memory the images go in, allocated the first time (of a type they can use).
bool EnsureBlock(uint32_t type_bits)
{
  if (s_block != VK_NULL_HANDLE)
    return (type_bits & (1u << s_block_type)) != 0;
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(s_context.gpu, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; i++)
  {
    if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
      VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc.allocationSize = BLOCK_SIZE;
      alloc.memoryTypeIndex = i;
      if (!Check(vkAllocateMemory(s_context.device, &alloc, nullptr, &s_block), "vkAllocateMemory"))
        return false;
      s_block_type = i;
      LogF("gpu filter: %llu MiB of GPU memory for the filters' images",
           static_cast<unsigned long long>(BLOCK_SIZE >> 20));
      return true;
    }
  }
  LogLine("gpu filter: no GPU memory type for the images");
  return false;
}

bool CreateImage(const ImageSpec& spec, VkDeviceSize* offset, Image* image)
{
  image->spec = spec;
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = FormatOf(spec.target);
  info.extent = {spec.width, spec.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!Check(vkCreateImage(s_context.device, &info, nullptr, &image->image), "vkCreateImage"))
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(s_context.device, image->image, &req);
  const VkDeviceSize at = (*offset + req.alignment - 1) / req.alignment * req.alignment;
  if (!EnsureBlock(req.memoryTypeBits) || at + req.size > BLOCK_SIZE)
    return false;
  if (!Check(vkBindImageMemory(s_context.device, image->image, s_block, at), "vkBindImageMemory"))
    return false;
  *offset = at + req.size;
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image->image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = info.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (!Check(vkCreateImageView(s_context.device, &view, nullptr, &image->view), "vkCreateImageView"))
    return false;
  VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = s_render_passes[static_cast<int>(spec.target)];
  fb.attachmentCount = 1;
  fb.pAttachments = &image->view;
  fb.width = spec.width;
  fb.height = spec.height;
  fb.layers = 1;
  return Check(vkCreateFramebuffer(s_context.device, &fb, nullptr, &image->framebuffer), "vkCreateFramebuffer");
}

// The images the chain needs: those of last time when they match, else new ones (after the GPU
// is done with the old ones: a new game or picture size, rare).
bool MakeImages(const std::vector<ImageSpec>& specs)
{
  bool same = specs.size() == s_images.size();
  for (size_t i = 0; same && i < specs.size(); i++)
    same = specs[i] == s_images[i].spec;
  if (same)
    return true;
  if (s_have_failed && specs.size() == s_failed_specs.size() &&
      std::equal(specs.begin(), specs.end(), s_failed_specs.begin()))
    return false;
  if (s_context.wait_idle)
    s_context.wait_idle();
  DestroyImages();
  VkDeviceSize offset = 0;
  for (const ImageSpec& spec : specs)
  {
    s_images.emplace_back();
    if (!CreateImage(spec, &offset, &s_images.back()))
    {
      LogF("gpu filter: no room for the images (%zu of them, %llu MiB needed so far)", specs.size(),
           static_cast<unsigned long long>(offset >> 20));
      DestroyImages();
      s_failed_specs = specs;
      s_have_failed = true;
      return false;
    }
  }
  s_have_failed = false;
  LogF("gpu filter: %zu images, %.1f MiB", specs.size(), static_cast<double>(offset) / (1 << 20));
  return true;
}

// ---- SMAA's lookup textures ----
bool FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags, uint32_t* type)
{
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(s_context.gpu, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; i++)
  {
    if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
    {
      *type = i;
      return true;
    }
  }
  return false;
}

void DestroyLuts()
{
  for (Lut* lut : {&s_area, &s_search})
  {
    if (lut->view != VK_NULL_HANDLE)
      vkDestroyImageView(s_context.device, lut->view, nullptr);
    if (lut->image != VK_NULL_HANDLE)
      vkDestroyImage(s_context.device, lut->image, nullptr);
    *lut = {};
  }
  if (s_lut_buffer != VK_NULL_HANDLE)
    vkDestroyBuffer(s_context.device, s_lut_buffer, nullptr);
  if (s_lut_buffer_memory != VK_NULL_HANDLE)
    vkFreeMemory(s_context.device, s_lut_buffer_memory, nullptr);
  if (s_lut_memory != VK_NULL_HANDLE)
    vkFreeMemory(s_context.device, s_lut_memory, nullptr);
  s_lut_buffer = VK_NULL_HANDLE;
  s_lut_buffer_memory = VK_NULL_HANDLE;
  s_lut_memory = VK_NULL_HANDLE;
  s_luts_made = s_luts_uploaded = false;
}

// The two images, their memory and a buffer with their bytes (once; false after a failure).
bool MakeLuts()
{
  if (s_luts_made || s_luts_failed)
    return s_luts_made;
  VkDevice device = s_context.device;
  bool ok = true;
  struct Spec
  {
    Lut* lut;
    VkFormat format;
    uint32_t width, height;
  };
  const Spec specs[2] = {{&s_area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT},
                         {&s_search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}};
  VkMemoryRequirements reqs[2] = {};
  for (int i = 0; i < 2 && ok; i++)
  {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = specs[i].format;
    info.extent = {specs[i].width, specs[i].height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ok = Check(vkCreateImage(device, &info, nullptr, &specs[i].lut->image), "vkCreateImage (SMAA)");
    if (ok)
      vkGetImageMemoryRequirements(device, specs[i].lut->image, &reqs[i]);
  }
  // Both images in one allocation (each allocation maps memory on the PS4).
  const VkDeviceSize search_at = ok ? (reqs[0].size + reqs[1].alignment - 1) / reqs[1].alignment * reqs[1].alignment : 0;
  uint32_t type = 0;
  if (ok)
    ok = FindMemoryType(reqs[0].memoryTypeBits & reqs[1].memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type);
  if (ok)
  {
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = search_at + reqs[1].size;
    alloc.memoryTypeIndex = type;
    ok = Check(vkAllocateMemory(device, &alloc, nullptr, &s_lut_memory), "vkAllocateMemory (SMAA)") &&
         Check(vkBindImageMemory(device, s_area.image, s_lut_memory, 0), "vkBindImageMemory (SMAA)") &&
         Check(vkBindImageMemory(device, s_search.image, s_lut_memory, search_at), "vkBindImageMemory (SMAA)");
  }
  for (int i = 0; i < 2 && ok; i++)
  {
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = specs[i].lut->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = specs[i].format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ok = Check(vkCreateImageView(device, &view, nullptr, &specs[i].lut->view), "vkCreateImageView (SMAA)");
  }
  // The bytes, for the copy in Record.
  if (ok)
  {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = LUT_SEARCH_OFFSET + SEARCHTEX_SIZE;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ok = Check(vkCreateBuffer(device, &info, nullptr, &s_lut_buffer), "vkCreateBuffer (SMAA)");
  }
  if (ok)
  {
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, s_lut_buffer, &req);
    ok = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &type);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    void* data = nullptr;
    ok = ok && Check(vkAllocateMemory(device, &alloc, nullptr, &s_lut_buffer_memory), "vkAllocateMemory (SMAA)") &&
         Check(vkBindBufferMemory(device, s_lut_buffer, s_lut_buffer_memory, 0), "vkBindBufferMemory (SMAA)") &&
         Check(vkMapMemory(device, s_lut_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data), "vkMapMemory (SMAA)");
    if (ok)
    {
      std::memcpy(data, areaTexBytes, AREATEX_SIZE);
      std::memcpy(static_cast<uint8_t*>(data) + LUT_SEARCH_OFFSET, searchTexBytes, SEARCHTEX_SIZE);
      vkUnmapMemory(device, s_lut_buffer_memory);
    }
  }
  if (!ok)
  {
    LogLine("gpu filter: no SMAA textures, so no Smooth Edges, Natural HD or Natural AI");
    DestroyLuts();
    s_luts_failed = true;
    return false;
  }
  s_luts_made = true;
  return true;
}

void UploadLuts(VkCommandBuffer commands)
{
  if (!s_luts_made || s_luts_uploaded)
    return;
  VkImageMemoryBarrier barriers[2] = {};
  const Lut* luts[2] = {&s_area, &s_search};
  for (int i = 0; i < 2; i++)
  {
    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[i].srcAccessMask = 0;
    barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = luts[i]->image;
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 2, barriers);
  VkBufferImageCopy area = {};
  area.bufferOffset = 0;
  area.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  area.imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1};
  vkCmdCopyBufferToImage(commands, s_lut_buffer, s_area.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &area);
  VkBufferImageCopy search = {};
  search.bufferOffset = LUT_SEARCH_OFFSET;
  search.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  search.imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};
  vkCmdCopyBufferToImage(commands, s_lut_buffer, s_search.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &search);
  for (VkImageMemoryBarrier& barrier : barriers)
  {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 2, barriers);
  s_luts_uploaded = true;
}

// ---- The chain of passes ----
void SetSize(float* v, double width, double height)
{
  v[0] = static_cast<float>(width);
  v[1] = static_cast<float>(height);
  v[2] = static_cast<float>(1.0 / width);
  v[3] = static_cast<float>(1.0 / height);
}

uint32_t FloatBits(float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// AMD's FsrEasuCon (ffx_fsr1.h): the picture (in_w x in_h, the whole image) to out_w x out_h.
void EasuConstants(uint32_t* con, double in_w, double in_h, double out_w, double out_h)
{
  const float values[16] = {
      static_cast<float>(in_w / out_w), static_cast<float>(in_h / out_h),
      static_cast<float>(0.5 * in_w / out_w - 0.5), static_cast<float>(0.5 * in_h / out_h - 0.5),
      static_cast<float>(1.0 / in_w), static_cast<float>(1.0 / in_h),
      static_cast<float>(1.0 / in_w), static_cast<float>(-1.0 / in_h),
      static_cast<float>(-1.0 / in_w), static_cast<float>(2.0 / in_h),
      static_cast<float>(1.0 / in_w), static_cast<float>(2.0 / in_h),
      0.0f, static_cast<float>(4.0 / in_h), 0.0f, 0.0f};
  for (int i = 0; i < 16; i++)
    con[i] = FloatBits(values[i]);
}

// Tube TV's glow reads a picture of about the console's size: one drawn bigger (its lines two texels apart or more:
// HD 3D, an interlaced picture) is made smaller first, a block of its texels for each of the console's pixels (else
// the glow's taps would fall between its details). The block's side, 1 (the picture itself) to 8.
uint32_t CrtGlowBlock(const Source& source)
{
  double per_line = 1.0;
  for (int i = 0; i < std::clamp(source.screen_count, 1, 2); i++)
  {
    const Source::Screen& screen = source.screens[i];
    per_line = std::max(per_line, double(screen.from[3]) * source.height / std::max(1.0, double(screen.lines)));
  }
  return per_line >= 1.75 ? std::min<uint32_t>(8, static_cast<uint32_t>(std::lround(per_line))) : 1;
}

// The passes for the mode and the images they draw into; false when the images can't be made.
bool BuildChain(Mode mode, const Source& source, uint32_t picture_w, uint32_t picture_h)
{
  std::vector<ImageSpec> specs;
  std::vector<Pass> passes;
  auto image = [&](uint32_t width, uint32_t height, Target target) {
    specs.push_back({width, height, target});
    return static_cast<int>(specs.size()) - 1;
  };
  auto pass = [&](Shader shader, Target target, int output, std::initializer_list<int> inputs, bool linear) {
    Pass p;
    p.shader = shader;
    p.target = target;
    p.output = output;
    for (int input : inputs)
      p.inputs[p.input_count++] = input;
    p.linear = linear;
    passes.push_back(p);
  };
  const uint32_t sw = source.width, sh = source.height;
  int picture = -1;
  if (!source.exact)
  {
    picture = image(sw, sh, Target::Rgba8);
    pass(COPY, Target::Rgba8, picture, {-1}, false);
  }
  switch (mode)
  {
  case Mode::Sharpen:
    pass(SHARPEN, Target::Screen, -1, {picture}, true);
    break;
  case Mode::PixelArt:
  {
    const int info = image(sw, sh, Target::Rgba8);
    pass(XBRZ0, Target::Rgba8, info, {picture}, false);
    pass(XBRZ1, Target::Screen, -1, {info, picture}, false);
    break;
  }
  case Mode::Smooth:
  {
    if (picture_w > MAX_OUTPUT_W || picture_h > MAX_OUTPUT_H)
      return false;
    const int easu = image(picture_w, picture_h, Target::Rgba8);
    pass(EASU, Target::Rgba8, easu, {picture}, true);
    pass(RCAS, Target::Screen, -1, {easu}, false);
    break;
  }
  case Mode::AI:
  {
    uint32_t aw = AI_WIDTH;
    uint32_t ah = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(double(sh) * aw / sw)), 16, 1024);
    if (double(aw) * ah > AI_DRAWN_PIXELS)
    {
      // A tall picture (the PlayStation's 480 lines): narrower, so the network's images still fit.
      const double scale = std::sqrt(AI_DRAWN_PIXELS / (double(aw) * ah));
      aw = static_cast<uint32_t>(std::lround(aw * scale));
      ah = static_cast<uint32_t>(std::lround(ah * scale));
    }
    int redrawn = -1;
    if (sw > AI_WIDTH)
    {
      // Drawn bigger already (the PlayStation's HD 3D): smaller, its shape kept, with Catmull-Rom.
      const double scale = std::min(1.0, std::sqrt(AI_DRAWN_PIXELS / (double(sw) * sh)));
      aw = std::max<uint32_t>(16, static_cast<uint32_t>(std::lround(sw * scale)));
      ah = std::max<uint32_t>(16, static_cast<uint32_t>(std::lround(sh * scale)));
      redrawn = image(aw, ah, Target::Rgba8);
      pass(CATMULL, Target::Rgba8, redrawn, {picture}, true);
    }
    else
    {
      // Pixel art (the SNES, the NES, the Game Boys, the PlayStation at 1x): redrawn with xBRZ.
      const int info = image(sw, sh, Target::Rgba8);
      redrawn = image(aw, ah, Target::Rgba8);
      pass(XBRZ0, Target::Rgba8, info, {picture}, false);
      pass(XBRZ1, Target::Rgba8, redrawn, {info, picture}, false);
    }
    int layers[7];
    for (int k = 0; k < 7; k++)
    {
      layers[k] = image(aw, ah, Target::Rgba16f);
      pass(static_cast<Shader>(RESTORE0 + k), Target::Rgba16f, layers[k], {k == 0 ? redrawn : layers[k - 1]}, false);
    }
    const int restored = image(aw, ah, Target::Rgba8);
    pass(static_cast<Shader>(RESTORE0 + 7), Target::Rgba8, restored,
         {redrawn, layers[0], layers[1], layers[2], layers[3], layers[4], layers[5], layers[6]}, false);
    pass(CATMULL, Target::Screen, -1, {restored}, true);
    break;
  }
  case Mode::Edges:
  case Mode::NaturalHD:
  case Mode::NaturalAI:
  {
    const bool drawn = source.kind == Source::Kind::Drawn || (source.kind == Source::Kind::Auto && source.exact);
    const double source_pixels = double(sw) * sh, picture_pixels = double(picture_w) * picture_h;
    // SMAA (3 passes) on an image of the given size: the anti-aliased image (its lookup textures
    // made the first time).
    bool luts = true;
    auto smaa = [&](int color, uint32_t w, uint32_t h) {
      luts = MakeLuts();
      const int edges = image(w, h, Target::Rgba8);
      pass(SMAA_EDGES, Target::Rgba8, edges, {color}, false);
      const int weights = image(w, h, Target::Rgba8);
      pass(SMAA_WEIGHTS, Target::Rgba8, weights, {edges, INPUT_SMAA_AREA, INPUT_SMAA_SEARCH}, true);
      passes.back().nearest_inputs = 1u << 2;  // the search texture
      const int blended = image(w, h, Target::Rgba8);
      pass(SMAA_BLEND, Target::Rgba8, blended, {color, weights}, true);
      return blended;
    };
    // Smooth Edges' picture: pixel art redrawn at 2x by MMPX (its own colours, slopes and curves
    // filled in), then SMAA; a drawn picture anti-aliased at its own size (when the screen shows it
    // bigger; one as big as the screen's picture is supersampled already).
    int smooth = picture;
    uint32_t smooth_w = sw, smooth_h = sh;
    const bool supersampled = drawn && source_pixels >= picture_pixels;
    if (!drawn)
    {
      // MMPX again (4x) when the screen still shows its 2x picture 3 times bigger or more (the Game
      // Boys): SMAA's soft steps then stay one or two screen pixels wide instead of a blur.
      int doubled = picture;
      for (int k = 0; k < 2; k++)
      {
        if (k == 1 && (double(picture_w) < 3.0 * smooth_w || double(picture_h) < 3.0 * smooth_h))
          break;
        const int next = image(smooth_w * 2, smooth_h * 2, Target::Rgba8);
        pass(MMPX, Target::Rgba8, next, {doubled}, false);
        doubled = next;
        smooth_w *= 2;
        smooth_h *= 2;
      }
      smooth = smaa(doubled, smooth_w, smooth_h);
    }
    else if (!supersampled && source_pixels <= SMAA_MAX_PIXELS)
    {
      smooth = smaa(picture, sw, sh);
    }
    if (!luts)
      return false;
    if (mode == Mode::Edges)
    {
      // Pixel art as crisp pixels (every one as wide as the next); a drawn picture with Catmull-Rom.
      pass(drawn ? CATMULL : SHARP_BILINEAR, Target::Screen, -1, {smooth}, true);
    }
    else if (mode == Mode::NaturalHD)
    {
      if (supersampled)
      {
        // As big as the screen's picture already: only sharpened on the way (HD Sharpen's pass).
        pass(SHARPEN, Target::Screen, -1, {smooth}, true);
      }
      else
      {
        if (picture_w > MAX_OUTPUT_W || picture_h > MAX_OUTPUT_H)
          return false;
        const int easu = image(picture_w, picture_h, Target::Rgba8);
        pass(EASU, Target::Rgba8, easu, {smooth}, true);
        pass(RCAS, Target::Screen, -1, {easu}, false);
      }
    }
    else
    {
      // Natural AI: Smooth Edges' picture resampled to the network's size, at the shape it has on
      // the screen: about 768x576, more of a supersampled picture's pixels, at most AI_DRAWN_PIXELS.
      const double aspect = double(picture_w) / double(picture_h);
      double pixels = double(AI_WIDTH) * (double(AI_WIDTH) / aspect);
      const double cap = supersampled ? AI_DRAWN_PIXELS : SMALL_AI_PIXELS;
      pixels = std::max(pixels, std::min(double(smooth_w) * smooth_h, cap));
      pixels = std::min(pixels, AI_DRAWN_PIXELS);
      const uint32_t aw = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(std::sqrt(pixels * aspect))), 16, 2048);
      const uint32_t ah = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(std::sqrt(pixels / aspect))), 16, 2048);
      const int redrawn = image(aw, ah, Target::Rgba8);
      pass(CATMULL, Target::Rgba8, redrawn, {smooth}, true);
      int layers[7];
      for (int k = 0; k < 7; k++)
      {
        layers[k] = image(aw, ah, Target::Rgba16f);
        pass(static_cast<Shader>(RESTORE0 + k), Target::Rgba16f, layers[k], {k == 0 ? redrawn : layers[k - 1]}, false);
      }
      // The network's correction at NATURAL_AI_STRENGTH: its picture mixed with its input.
      const int restored = image(aw, ah, Target::Rgba8);
      pass(RESTORE_PARTIAL, Target::Rgba8, restored,
           {redrawn, layers[0], layers[1], layers[2], layers[3], layers[4], layers[5], layers[6]}, false);
      pass(CATMULL, Target::Screen, -1, {restored}, true);
    }
    break;
  }
  case Mode::Crt:
  {
    const uint32_t block = CrtGlowBlock(source);
    int glow = picture;
    if (block >= 2)
    {
      glow = image(std::max<uint32_t>(4, sw / block), std::max<uint32_t>(4, sh / block), Target::Rgba8);
      pass(CRT_GLOW, Target::Rgba8, glow, {picture}, true);
    }
    pass(CRT, Target::Screen, -1, {picture, glow}, true);
    break;
  }
  case Mode::Off:
    return false;
  }
  if (passes.size() > MAX_PASSES || !MakeImages(specs))
    return false;
  s_chain = std::move(passes);
  return true;
}

void InputSize(int input, double* width, double* height)
{
  if (input < 0)
  {
    *width = s_job.source.width;
    *height = s_job.source.height;
  }
  else
  {
    *width = s_images[input].spec.width;
    *height = s_images[input].spec.height;
  }
}

// Two values of 0..1 as 16-bit fractions in one word (GLSL's unpackUnorm2x16).
uint32_t PackUnorm2(float a, float b)
{
  const auto unorm = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
  return unorm(a) | (unorm(b) << 16);
}

// A float as a 16-bit one (normal numbers; GLSL's unpackHalf2x16 reads two from a word).
uint32_t Half(float value)
{
  const uint32_t bits = FloatBits(value);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
  if (exponent <= 0)
    return sign;  // too small: 0
  if (exponent >= 31)
    return sign | 0x7C00u;
  return sign | (static_cast<uint32_t>(exponent) << 10) | ((bits & 0x7FFFFFu) >> 13);
}

uint32_t PackHalf2(float a, float b)
{
  return Half(a) | (Half(b) << 16);
}

// Tube TV's constants (src/shaders/crt.frag's list): the screens, the look.
void CrtConstants(Push& push)
{
  const Source& source = s_job.source;
  const CrtLook& look = CRT_LOOKS[std::clamp(source.crt, 1, 4) - 1];
  const int screens = std::clamp(source.screen_count, 1, 2);
  for (int i = 0; i < 2; i++)
  {
    const Source::Screen& screen = source.screens[i < screens ? i : 0];
    push.con[i * 4 + 0] = PackUnorm2(screen.place[0], screen.place[1]);
    push.con[i * 4 + 1] = PackUnorm2(screen.place[2], screen.place[3]);
    push.con[i * 4 + 2] = PackUnorm2(screen.from[0], screen.from[1]);
    push.con[i * 4 + 3] = PackUnorm2(screen.from[2], screen.from[3]);
    push.con[8 + i] = FloatBits(std::max(1.0f, screen.lines));
  }
  // One screen that is the whole picture: the shader has nothing to work out about where it is.
  const Source::Screen& first = source.screens[0];
  const bool whole = screens == 1 && first.place[0] == 0.0f && first.place[1] == 0.0f && first.place[2] == 1.0f &&
                     first.place[3] == 1.0f && first.from[0] == 0.0f && first.from[1] == 0.0f && first.from[2] == 1.0f &&
                     first.from[3] == 1.0f;
  push.con[10] = static_cast<uint32_t>(screens) | (look.mask << 8) | (static_cast<uint32_t>(source.rotation & 3) << 16) |
                 (CrtGlowBlock(source) << 20) | (whole ? 1u << 24 : 0u);
  push.con[11] = PackHalf2(look.curve_x, look.curve_y);
  push.con[12] = PackHalf2(look.hardness, look.widening);
  push.con[13] = PackHalf2(look.mask_strength, look.glow);
  push.con[14] = PackHalf2(look.vignette, look.corner);
  // The light the mask takes away, given back (the average of its lit and dark stripes).
  const float s = look.mask_strength;
  const float mask_average = look.mask == 1 ? 1.0f - s / 2.0f :
                             look.mask == 2 ? (1.0f - 2.0f * s / 3.0f) * (1.0f - s / 6.0f) :
                             look.mask == 3 ? 1.0f - 2.0f * s / 3.0f :
                                              1.0f;
  push.con[15] = PackHalf2(1.0f / mask_average, look.sharpness);
  // sqrt(1 - widening * level) as 1 + level * (b + level * c), exact at the levels 0, 1/2 and 1.
  const float at_half = std::sqrt(1.0f - 0.5f * look.widening) - 1.0f, at_one = std::sqrt(1.0f - look.widening) - 1.0f;
  const float c = 2.0f * at_one - 4.0f * at_half;
  push.origin[2] = at_one - c;
  push.origin[3] = c;
}

// Every pass's push constants for this frame (the picture's place can change any frame).
void SetConstants()
{
  const double pw = s_job.x1 - s_job.x0, ph = s_job.y1 - s_job.y0;
  for (Pass& pass : s_chain)
  {
    Push& push = pass.push;
    push = {};
    double in_w, in_h;
    InputSize(pass.inputs[0], &in_w, &in_h);
    SetSize(push.source, in_w, in_h);
    if (pass.output >= 0)
    {
      const ImageSpec& out = s_images[pass.output].spec;
      push.clip[0] = push.clip[1] = -1.0f;
      push.clip[2] = push.clip[3] = 1.0f;
      SetSize(push.output, out.width, out.height);
    }
    else
    {
      // The quad over the picture, in clip space (it can stick out of the screen).
      const double w = s_context.width, h = s_context.height;
      push.clip[0] = static_cast<float>(2.0 * s_job.x0 / w - 1.0);
      push.clip[1] = static_cast<float>(2.0 * s_job.y0 / h - 1.0);
      push.clip[2] = static_cast<float>(2.0 * s_job.x1 / w - 1.0);
      push.clip[3] = static_cast<float>(2.0 * s_job.y1 / h - 1.0);
      SetSize(push.output, pw, ph);
      push.origin[0] = static_cast<float>(s_job.x0);
      push.origin[1] = static_cast<float>(s_job.y0);
    }
    switch (pass.shader)
    {
    case SHARPEN:
      push.origin[2] = std::exp2(-SHARPEN_STOPS);
      break;
    case EASU:
      EasuConstants(push.con, in_w, in_h, push.output[0], push.output[1]);
      break;
    case RCAS:
      push.con[0] = FloatBits(std::exp2(-FSR_SHARPNESS_STOPS));
      break;
    case RESTORE_PARTIAL:
      push.origin[2] = NATURAL_AI_STRENGTH;
      break;
    case CRT:
      CrtConstants(push);
      break;
    default:
      break;
    }
  }
}

void WriteSet(VkDescriptorSet set, const Pass& pass)
{
  VkDescriptorImageInfo images[MAX_INPUTS] = {};
  VkWriteDescriptorSet writes[MAX_INPUTS] = {};
  for (uint32_t i = 0; i < pass.input_count; i++)
  {
    const int input = pass.inputs[i];
    images[i].sampler = pass.linear && !(pass.nearest_inputs & (1u << i)) ? s_linear : s_nearest;
    images[i].imageView = input == INPUT_SMAA_AREA   ? s_area.view :
                          input == INPUT_SMAA_SEARCH ? s_search.view :
                          input < 0                  ? s_job.source.view :
                                                       s_images[input].view;
    images[i].imageLayout = input == INPUT_PICTURE ? s_job.source.layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(s_context.device, pass.input_count, writes, 0, nullptr);
}

void Bind(VkCommandBuffer commands, const Pass& pass, VkDescriptorSet set)
{
  vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipelines[pass.shader][static_cast<int>(pass.target)]);
  vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(commands, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(Push), &pass.push);
}

// What the frame slot measured the last time it was recorded (its fence has been waited for).
void ReadTimes(uint32_t slot)
{
  Timed& timed = s_timed[slot];
  const uint32_t images = timed.images;
  const bool screen = timed.screen;
  timed.images = 0;
  timed.screen = false;
  if (s_queries == VK_NULL_HANDLE || (images == 0 && !screen))
    return;
  const uint32_t base = slot * QUERIES_PER_FRAME;
  uint64_t t[QUERIES_PER_FRAME] = {};
  // The passes into images and the pass to the screen apart: the second pair isn't written when
  // the picture was clipped away. Without VK_QUERY_RESULT_WAIT_BIT: they are there (fence).
  if (vkGetQueryPoolResults(s_context.device, s_queries, base, images + 1, sizeof(uint64_t) * (images + 1), t,
                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  uint64_t final_ticks = 0;
  if (screen && vkGetQueryPoolResults(s_context.device, s_queries, base + MAX_PASSES + 1, 2, sizeof(uint64_t) * 2,
                                      &t[MAX_PASSES + 1], sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
    final_ticks = (t[MAX_PASSES + 2] - t[MAX_PASSES + 1]) & s_tick_mask;
  // A different chain (mode, picture size): its own sums.
  const std::string chain = s_logged;
  if (chain != s_timed_chain || images + 1 != s_timed_count)
  {
    s_timed_chain = chain;
    s_timed_count = images + 1;
    std::fill(std::begin(s_pass_ticks), std::end(s_pass_ticks), 0.0);
    s_timed_frames = s_screen_frames = 0;
  }
  for (uint32_t p = 0; p < images; p++)
    s_pass_ticks[p] += static_cast<double>((t[p + 1] - t[p]) & s_tick_mask);
  if (final_ticks)
  {
    s_pass_ticks[images] += static_cast<double>(final_ticks);
    s_screen_frames++;
  }
  s_timed_frames++;
  if (!s_have_first)
  {
    s_have_first = true;
    s_first_ticks = t[0];
    s_first_at = timed.at;
  }
  s_last_ticks = t[0];
  s_last_at = timed.at;
}

// The last pass, inside ImGui's render pass where the picture is in the draw list.
void DrawFinal(const ImDrawList*, const ImDrawCmd* cmd)
{
  if (!s_job.recorded || s_chain.empty())
    return;
  const auto cpu_start = Clock::now();
  s_job.pending = s_job.recorded = false;
  auto* state = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
  if (!state)
    return;
  // The part of the picture on the screen, inside ImGui's clipping.
  const int x0 = std::max({s_job.x0, 0, static_cast<int>(std::floor(cmd->ClipRect.x))});
  const int y0 = std::max({s_job.y0, 0, static_cast<int>(std::floor(cmd->ClipRect.y))});
  const int x1 = std::min({s_job.x1, static_cast<int>(s_context.width), static_cast<int>(std::ceil(cmd->ClipRect.z))});
  const int y1 = std::min({s_job.y1, static_cast<int>(s_context.height), static_cast<int>(std::ceil(cmd->ClipRect.w))});
  if (x1 <= x0 || y1 <= y0)
    return;
  VkCommandBuffer commands = state->CommandBuffer;
  const uint32_t queries = s_job.frame * QUERIES_PER_FRAME + MAX_PASSES + 1;
  if (s_queries != VK_NULL_HANDLE)
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_queries, queries);
  const Pass& pass = s_chain.back();
  Bind(commands, pass, s_sets[s_job.frame][s_chain.size() - 1]);
  const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(s_context.width),
                               static_cast<float>(s_context.height), 0.0f, 1.0f};
  vkCmdSetViewport(commands, 0, 1, &viewport);
  const VkRect2D scissor = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
  vkCmdSetScissor(commands, 0, 1, &scissor);
  vkCmdDraw(commands, 4, 1, 0, 0);
  if (s_queries != VK_NULL_HANDLE)
  {
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_queries, queries + 1);
    s_timed[s_job.frame].screen = true;
  }
  s_cpu_ms += MsSince(cpu_start);
}
}  // namespace

const char* Name(Mode mode)
{
  switch (mode)
  {
  case Mode::Sharpen:
    return "HD Sharpen";
  case Mode::PixelArt:
    return "Pixel Art HD (xBRZ)";
  case Mode::Smooth:
    return "Smooth HD (FSR)";
  case Mode::AI:
    return "AI (xBRZ + Anime4K)";
  case Mode::Edges:
    return "Smooth Edges";
  case Mode::NaturalHD:
    return "Natural HD";
  case Mode::NaturalAI:
    return "Natural AI";
  case Mode::Crt:
    return "Tube TV (CRT)";
  case Mode::Off:
    break;
  }
  return "off";
}

bool Init(const Context& context)
{
  s_context = context;
  VkDevice device = context.device;
  for (int m = 0; m < MODE_COUNT; m++)
  {
    s_ready[m] = false;
    s_failed[m] = false;
  }
  VkFormatProperties props = {};
  vkGetPhysicalDeviceFormatProperties(context.gpu, VK_FORMAT_R16G16B16A16_SFLOAT, &props);
  const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  s_rgba16f = (props.optimalTilingFeatures & needed) == needed;

  VkDescriptorSetLayoutBinding bindings[MAX_INPUTS] = {};
  for (uint32_t i = 0; i < MAX_INPUTS; i++)
  {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = MAX_INPUTS;
  set_info.pBindings = bindings;
  if (!Check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &s_set_layout), "vkCreateDescriptorSetLayout"))
    return false;
  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &s_set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (!Check(vkCreatePipelineLayout(device, &layout_info, nullptr, &s_layout), "vkCreatePipelineLayout"))
    return false;
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES * MAX_PASSES * MAX_INPUTS};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = FRAMES * MAX_PASSES;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (!Check(vkCreateDescriptorPool(device, &pool_info, nullptr, &s_pool), "vkCreateDescriptorPool"))
    return false;
  std::vector<VkDescriptorSetLayout> layouts(FRAMES * MAX_PASSES, s_set_layout);
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = s_pool;
  alloc.descriptorSetCount = FRAMES * MAX_PASSES;
  alloc.pSetLayouts = layouts.data();
  if (!Check(vkAllocateDescriptorSets(device, &alloc, &s_sets[0][0]), "vkAllocateDescriptorSets"))
    return false;

  s_render_passes[static_cast<int>(Target::Screen)] = context.screen;
  s_render_passes[static_cast<int>(Target::Rgba8)] = CreateImagePass(VK_FORMAT_R8G8B8A8_UNORM);
  if (s_rgba16f)
    s_render_passes[static_cast<int>(Target::Rgba16f)] = CreateImagePass(VK_FORMAT_R16G16B16A16_SFLOAT);
  s_linear = CreateSampler(VK_FILTER_LINEAR);
  s_nearest = CreateSampler(VK_FILTER_NEAREST);
  s_vertex = CreateModule(emusuni_spv_quad_vert, sizeof(emusuni_spv_quad_vert));
  bool ok = s_render_passes[static_cast<int>(Target::Rgba8)] != VK_NULL_HANDLE && s_linear != VK_NULL_HANDLE &&
            s_nearest != VK_NULL_HANDLE && s_vertex != VK_NULL_HANDLE;
  for (int i = 0; i < SHADER_COUNT && ok; i++)
  {
    s_modules[i] = CreateModule(SHADERS[i].code, SHADERS[i].size);
    ok = s_modules[i] != VK_NULL_HANDLE;
  }
  if (!ok)
    return false;

  // The pipelines compiled on earlier starts (the driver ignores a cache from another version).
  std::vector<uint8_t> data;
  if (FILE* file = fopen(context.cache_path.c_str(), "rb"))
  {
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size > 0 && size < (64 << 20))
    {
      data.resize(static_cast<size_t>(size));
      if (fread(data.data(), 1, data.size(), file) != data.size())
        data.clear();
    }
    fclose(file);
  }
  VkPipelineCacheCreateInfo cache_info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  cache_info.initialDataSize = data.size();
  cache_info.pInitialData = data.empty() ? nullptr : data.data();
  if (vkCreatePipelineCache(device, &cache_info, nullptr, &s_cache) != VK_SUCCESS)
  {
    cache_info.initialDataSize = 0;
    cache_info.pInitialData = nullptr;
    if (!Check(vkCreatePipelineCache(device, &cache_info, nullptr, &s_cache), "vkCreatePipelineCache"))
      return false;
    data.clear();
  }
  s_cache_size = data.size();

  // The GPU time of the passes (TakeGpuTime): timestamps, if the queue has them.
  VkPhysicalDeviceProperties gpu_props;
  vkGetPhysicalDeviceProperties(context.gpu, &gpu_props);
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(context.gpu, &families, nullptr);
  std::vector<VkQueueFamilyProperties> family_props(families);
  vkGetPhysicalDeviceQueueFamilyProperties(context.gpu, &families, family_props.data());
  const uint32_t valid_bits = context.queue_family < families ? family_props[context.queue_family].timestampValidBits : 0;
  if (valid_bits > 0 && gpu_props.limits.timestampPeriod > 0.0f)
  {
    s_tick_mask = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
    s_vulkan_mhz = 1000.0 / gpu_props.limits.timestampPeriod;
    VkQueryPoolCreateInfo query_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = FRAMES * QUERIES_PER_FRAME;
    if (vkCreateQueryPool(device, &query_info, nullptr, &s_queries) != VK_SUCCESS)
      s_queries = VK_NULL_HANDLE;
  }
  s_ok = true;
  LogF("gpu filter: ready (%zu bytes of compiled pipelines%s; GPU timestamps %s, %u bits, %.1f MHz)", data.size(),
       s_rgba16f ? "" : "; no 16-bit float images, so no AI", s_queries != VK_NULL_HANDLE ? "on" : "off", valid_bits,
       s_vulkan_mhz);
  return true;
}

void Shutdown()
{
  if (s_compiler.joinable())
    s_compiler.join();
  VkDevice device = s_context.device;
  if (device == VK_NULL_HANDLE)
    return;
  DestroyImages();
  if (s_block != VK_NULL_HANDLE)
    vkFreeMemory(device, s_block, nullptr);
  s_block = VK_NULL_HANDLE;
  DestroyLuts();
  s_luts_failed = false;
  for (auto& row : s_pipelines)
  {
    for (VkPipeline& pipeline : row)
    {
      if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
  }
  for (VkShaderModule& module : s_modules)
  {
    if (module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, module, nullptr);
    module = VK_NULL_HANDLE;
  }
  if (s_vertex != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, s_vertex, nullptr);
  if (s_cache != VK_NULL_HANDLE)
    vkDestroyPipelineCache(device, s_cache, nullptr);
  if (s_queries != VK_NULL_HANDLE)
    vkDestroyQueryPool(device, s_queries, nullptr);
  s_queries = VK_NULL_HANDLE;
  for (Timed& timed : s_timed)
    timed = {};
  if (s_pool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device, s_pool, nullptr);
  if (s_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device, s_layout, nullptr);
  if (s_set_layout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(device, s_set_layout, nullptr);
  for (int t = 1; t < TARGET_COUNT; t++)  // [Screen] is Display's
  {
    if (s_render_passes[t] != VK_NULL_HANDLE)
      vkDestroyRenderPass(device, s_render_passes[t], nullptr);
    s_render_passes[t] = VK_NULL_HANDLE;
  }
  if (s_linear != VK_NULL_HANDLE)
    vkDestroySampler(device, s_linear, nullptr);
  if (s_nearest != VK_NULL_HANDLE)
    vkDestroySampler(device, s_nearest, nullptr);
  s_vertex = VK_NULL_HANDLE;
  s_cache = VK_NULL_HANDLE;
  s_pool = VK_NULL_HANDLE;
  s_layout = VK_NULL_HANDLE;
  s_set_layout = VK_NULL_HANDLE;
  s_linear = s_nearest = VK_NULL_HANDLE;
  s_chain.clear();
  s_job = {};
  s_ok = false;
  s_context = {};
}

bool Ready(Mode mode)
{
  if (!s_ok || mode == Mode::Off)
    return false;
  const int m = static_cast<int>(mode);
  if (s_ready[m].load(std::memory_order_acquire))
    return true;
  if (s_failed[m].load(std::memory_order_acquire))
    return false;
  if ((mode == Mode::AI || mode == Mode::NaturalAI) && !s_rgba16f)
  {
    s_failed[m] = true;
    return false;
  }
  if (!s_compiling.load(std::memory_order_acquire))
  {
    if (s_compiler.joinable())
      s_compiler.join();
    s_compiling.store(true, std::memory_order_release);
    LogF("gpu filter: compiling %s", Name(mode));
    s_compiler = std::thread(Compile, mode);
  }
  return false;
}

bool Draw(ImDrawList* dl, Mode mode, const Source& source, ImVec2 min, ImVec2 max)
{
  const auto cpu_start = Clock::now();
  if (source.view == VK_NULL_HANDLE || source.width < 4 || source.height < 4 || !Ready(mode))
    return false;
  // The picture in whole screen pixels.
  const int x0 = static_cast<int>(std::lround(min.x)), y0 = static_cast<int>(std::lround(min.y));
  const int x1 = static_cast<int>(std::lround(max.x)), y1 = static_cast<int>(std::lround(max.y));
  if (x1 - x0 < 16 || y1 - y0 < 16 || x1 <= 0 || y1 <= 0 || x0 >= static_cast<int>(s_context.width) ||
      y0 >= static_cast<int>(s_context.height))
    return false;
  if (!BuildChain(mode, source, static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)))
    return false;
  s_job.pending = true;
  s_job.recorded = false;
  s_job.mode = mode;
  s_job.source = source;
  s_job.x0 = x0;
  s_job.y0 = y0;
  s_job.x1 = x1;
  s_job.y1 = y1;
  char what[160];
  const bool steps = mode == Mode::Edges || mode == Mode::NaturalHD || mode == Mode::NaturalAI;
  const bool drawn = source.kind == Source::Kind::Drawn || (source.kind == Source::Kind::Auto && source.exact);
  static const char* const CRT_NAMES[4] = {" (TV", " (Arcade Monitor", " (Studio Monitor", " (Intense"};
  char crt[64] = "";
  if (mode == Mode::Crt)
    std::snprintf(crt, sizeof(crt), "%s, %d screen%s)", CRT_NAMES[std::clamp(source.crt, 1, 4) - 1],
                  std::clamp(source.screen_count, 1, 2), source.screen_count > 1 ? "s" : "");
  std::snprintf(what, sizeof(what), "%s%s, %ux%u%s to %dx%d, %zu passes", Name(mode), crt, source.width, source.height,
                !steps ? "" : drawn ? " (drawn)" : " (pixel art)", x1 - x0, y1 - y0, s_chain.size());
  if (s_logged != what)
  {
    s_logged = what;
    LogF("gpu filter: %s", what);
  }
  dl->AddCallback(DrawFinal, nullptr);
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  s_cpu_ms += MsSince(cpu_start);
  return true;
}

void Record(VkCommandBuffer commands, uint32_t frame)
{
  if (!s_job.pending || s_chain.empty())
    return;
  const auto cpu_start = Clock::now();
  s_job.frame = frame % FRAMES;
  // SMAA's textures, the first time a chain needs them (not timed).
  UploadLuts(commands);
  // The slot's timestamps from its last use (its fence was waited for), then this frame's.
  ReadTimes(s_job.frame);
  const uint32_t queries = s_job.frame * QUERIES_PER_FRAME;
  if (s_queries != VK_NULL_HANDLE)
  {
    vkCmdResetQueryPool(commands, s_queries, queries, QUERIES_PER_FRAME);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_queries, queries);
  }
  SetConstants();
  // This frame's sets: the GPU is done with them (EndFrame waited for the frame's fence).
  for (size_t p = 0; p < s_chain.size(); p++)
    WriteSet(s_sets[s_job.frame][p], s_chain[p]);
  for (size_t p = 0; p + 1 < s_chain.size(); p++)
  {
    const Pass& pass = s_chain[p];
    const Image& out = s_images[pass.output];
    VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = s_render_passes[static_cast<int>(pass.target)];
    begin.framebuffer = out.framebuffer;
    begin.renderArea.extent = {out.spec.width, out.spec.height};
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    Bind(commands, pass, s_sets[s_job.frame][p]);
    const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(out.spec.width),
                                 static_cast<float>(out.spec.height), 0.0f, 1.0f};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor = {{0, 0}, {out.spec.width, out.spec.height}};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdDraw(commands, 4, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    if (s_queries != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_queries,
                          queries + 1 + static_cast<uint32_t>(p));
  }
  if (s_queries != VK_NULL_HANDLE)
  {
    s_timed[s_job.frame].images = static_cast<uint32_t>(s_chain.size() - 1);
    s_timed[s_job.frame].at = Clock::now();
  }
  s_job.recorded = true;
  s_cpu_ms += MsSince(cpu_start);
  s_cpu_frames++;
}

bool TakeGpuTime(char* text, size_t size)
{
  if (s_timed_frames == 0 || s_timed_count == 0 || s_chain.size() != s_timed_count)
    return false;
  // The clock's rate: GPU ticks between the first and last timed frames against the CPU's time
  // between their recording (needs a few seconds); else what the driver says.
  const double seconds = std::chrono::duration<double>(s_last_at - s_first_at).count();
  double mhz = s_vulkan_mhz;
  if (seconds >= 2.0 && s_last_ticks > s_first_ticks)
    mhz = static_cast<double>((s_last_ticks - s_first_ticks) & s_tick_mask) / seconds / 1e6;
  const double ms_per_tick = mhz > 0.0 ? 1e-3 / mhz : 0.0;
  double total = 0.0;
  char parts[512] = "";
  int at = 0;
  for (uint32_t p = 0; p < s_timed_count && at >= 0 && at < static_cast<int>(sizeof(parts)); p++)
  {
    const bool last = p + 1 == s_timed_count;
    const uint32_t frames = last ? s_screen_frames : s_timed_frames;
    const double ms = frames ? s_pass_ticks[p] * ms_per_tick / frames : 0.0;
    total += ms;
    at += std::snprintf(parts + at, sizeof(parts) - at, "%s%s %.2f", p ? ", " : "", SHADERS[s_chain[p].shader].name, ms);
  }
  std::snprintf(text, size,
                "gpu filter: %s, %.2f ms of GPU per frame (%u frames: %s); main thread %.3f ms per frame; GPU clock "
                "%.1f MHz (the driver says %.1f)",
                Name(s_job.mode), total, s_timed_frames, parts, s_cpu_frames ? s_cpu_ms / s_cpu_frames : 0.0, mhz,
                s_vulkan_mhz);
  // The next window.
  std::fill(std::begin(s_pass_ticks), std::end(s_pass_ticks), 0.0);
  s_timed_frames = s_screen_frames = 0;
  s_cpu_ms = 0.0;
  s_cpu_frames = 0;
  s_first_ticks = s_last_ticks;
  s_first_at = s_last_at;
  return true;
}
}  // namespace GpuFilter
