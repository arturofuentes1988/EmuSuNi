// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Emulator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <libretro.h>

#include "Cheats.h"
#include "Controls.h"
#include "Display.h"
#include "GameAudio.h"
#include "GpuFilter.h"
#include "Image.h"
#include "IngameMenu.h"
#include "LibretroVulkan.h"
#include "Log.h"
#include "Pacing.h"
#include "PS4Lang.h"
#include "Saves.h"
#include "Settings.h"
#include "Systems.h"
#include "Tour.h"
#include "Upscaler.h"
#include "VulkanFunctions.h"

#ifdef EMUSUNI_N64
// port/ps4_n64.c: may code made at run time run in the core's data (the N64 dynarec)? The way
// that works, or -1. port/ps4_runtime.cpp: the way executable memory is mapped anywhere (the RSP's
// recompiler), -1 when none (ps4_exec_probe() at start, main.cpp).
extern "C" int emusuni_jit_check(void);
extern "C" int ps4_exec_method;
// port/ps4_n64.c: ParaLLEl-RDP's threads on cores of their own (N64 CPU Cores: Fixed).
extern "C" int emusuni_n64_fixed_cores;
#endif

// Mesa's PS4 layer (ac_ps4_drm.c): the GPU's busy time and the span it covers (GPU clock ticks,
// since the start), and its submissions and buffers.
extern "C" void ac_ps4_get_gpu_busy(uint64_t* busy_ticks, uint64_t* span_ticks);
// libkernel's affinity calls as they really are (OpenOrbis declares them without parameters):
// bit n of the mask is core n.
extern "C" int emusuni_get_affinity(OrbisPthread thread, uint64_t* mask) __asm__("scePthreadGetaffinity");
extern "C" int emusuni_set_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");
extern "C" void ac_ps4_get_stats(uint64_t* submitted, uint64_t* completed, uint64_t* bo_bytes, uint64_t* bo_count);
// libScePad's motion sensors on or off (OpenOrbis declares it without parameters).
extern "C" int emusuni_pad_set_motion(int handle, bool enable) __asm__("scePadSetMotionSensorState");

namespace Emulator
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr int MAX_PLAYERS = 5;
constexpr unsigned DEVICE_MULTITAP = (1 << 8) | RETRO_DEVICE_JOYPAD;  // Snes9x's Multitap

// Newer libretro.h (FCEUmm's) than Snes9x's: the ROM from memory, so a .zip's works too.
#ifndef RETRO_ENVIRONMENT_GET_GAME_INFO_EXT
#define RETRO_ENVIRONMENT_GET_GAME_INFO_EXT 66
struct retro_game_info_ext
{
  const char* full_path;
  const char* archive_path;
  const char* archive_file;
  const char* dir;
  const char* name;
  const char* ext;
  const char* meta;
  const void* data;
  size_t size;
  bool file_in_archive;
  bool persistent_data;
};
#endif

// ---- State the libretro callbacks use (one game at a time). ----
const CoreApi* s_core = &Cores::SNES9X;  // the running game's core
SystemId s_system = SystemId::SNES;
std::string s_game_id;  // the game's key
std::string s_system_dir, s_save_dir;
std::map<std::string, std::string> s_variables;  // the core's option values for GET_VARIABLE
bool s_variables_changed = false;
bool s_render_video = true;
std::array<uint16_t, MAX_PLAYERS> s_buttons{};  // the console's buttons per input port
// The ROM for GET_GAME_INFO_EXT while the game loads.
retro_game_info_ext s_info_ext = {};
std::string s_info_path, s_info_dir, s_info_name, s_info_extension;
std::string s_message;  // the core's newest message (SET_MESSAGE), shown by the in-game menu
double s_game_fps = 60.0988;
// A core that draws with Vulkan itself (ParaLLEl-RDP for the N64): libretro's Vulkan interface
// over the display's device; the image it hands over for the frame (set_image).
Display* s_display = nullptr;
retro_hw_render_callback s_hw_render = {};
bool s_hw_vulkan = false;
retro_hw_render_interface_vulkan s_vulkan = {};
retro_vulkan_image s_hw_image = {};
bool s_hw_image_set = false;
bool s_frame_hw = false;  // the newest frame is the core's image
// The analog sticks per player (RETRO_DEVICE_ANALOG): left / right, x / y.
std::array<std::array<std::array<int16_t, 2>, 2>, MAX_PLAYERS> s_analog{};
// The N64 core may use its recompilers (port/ps4_n64.c, ps4_exec_probe).
bool s_dynarec_ok = false, s_rsp_jit_ok = false;
// Rumble (the N64's Rumble Pak): each player's pad and its two motors (strong, weak).
std::array<int, MAX_PLAYERS> s_pad_handles{};
std::array<std::array<uint8_t, 2>, MAX_PLAYERS> s_rumble{};

bool SetRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
  if (port >= MAX_PLAYERS || s_pad_handles[port] < 0)
    return false;
  uint8_t& motor = s_rumble[port][effect == RETRO_RUMBLE_STRONG ? 0 : 1];
  const uint8_t value = static_cast<uint8_t>(strength >> 8);
  if (motor == value)
    return true;  // mGBA sets both motors every frame, mostly to what they are
  motor = value;
  OrbisPadVibeParam motors = {s_rumble[port][0], s_rumble[port][1]};
  scePadSetVibration(s_pad_handles[port], &motors);
  return true;
}

void StopRumble()
{
  for (int i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_pad_handles[i] >= 0 && (s_rumble[i][0] || s_rumble[i][1]))
    {
      OrbisPadVibeParam off = {0, 0};
      scePadSetVibration(s_pad_handles[i], &off);
    }
    s_rumble[i] = {};
  }
}
// ---- The Game Boys ----
// The Game Boy game's cartridge header: made for the Game Boy Color (0x143), with Super Game Boy
// features (0x146).
bool s_gb_color_game = false, s_gb_sgb_game = false;
// The N64 cartridge's name in its header ("GOLDENEYE"): N64 CPU Timing's Auto.
std::string s_n64_name;

// The name in an N64 ROM's header (0x20, 20 characters), whatever the file's byte order (.z64 as the
// cartridge, .v64 with its bytes swapped in pairs, .n64 in fours).
std::string N64Name(const std::vector<uint8_t>& rom)
{
  if (rom.size() < 0x40)
    return {};
  uint8_t header[0x40];
  std::memcpy(header, rom.data(), sizeof(header));
  if (header[0] == 0x37 && header[1] == 0x80)
  {
    for (size_t i = 0; i < sizeof(header); i += 2)
      std::swap(header[i], header[i + 1]);
  }
  else if (header[0] == 0x40 && header[1] == 0x12)
  {
    for (size_t i = 0; i < sizeof(header); i += 4)
    {
      std::swap(header[i], header[i + 3]);
      std::swap(header[i + 1], header[i + 2]);
    }
  }
  std::string name(reinterpret_cast<const char*>(header + 0x20), 20);
  while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
    name.pop_back();
  return name;
}

// The tilt and turn sensors of some cartridges (WarioWare: Twisted!, Yoshi Topsy-Turvy, Kirby Tilt
// 'n' Tumble), through libretro's sensor interface in Android's units and axes, as mGBA reads it
// (m/s^2 along the handheld's x to the right and y up its screen; rad/s around z, out of the screen,
// anticlockwise): player 1's right stick, and with Tilt Sensor: DualShock 4 Motion the controller's
// own sensors, from how it was held when the game started (or the in-game menu closed). The
// DualShock 4's axes: x right, y up out of its face, z towards the player; held like the handheld
// lying flat, the handheld's y is the DualShock's -z, and turning it like a wheel is around its z.
struct Tilt
{
  float stick_x = 0.0f, stick_y = 0.0f;  // -1..1, y down
  bool motion = false;                   // the DualShock 4's sensors too
  bool calibrate = true;                 // the next reading is the controller at rest
  vec_float3 accel{}, rest{};            // G
  float turn = 0.0f;                     // rad/s around the DualShock 4's z
};
Tilt s_tilt;
// libretro's sensor actions and inputs (Snes9x's libretro.h has the first two of each only).
constexpr int SENSOR_ACCELEROMETER_ENABLE = 0, SENSOR_GYROSCOPE_ENABLE = 2;
constexpr unsigned SENSOR_ACCELEROMETER_X = 0, SENSOR_ACCELEROMETER_Y = 1, SENSOR_GYROSCOPE_Z = 5;
constexpr float MAX_TILT = 7.0f;  // m/s^2 (about 45 degrees): mGBA's scale keeps it inside 32 bits
constexpr float MAX_TURN = 3.8f;  // rad/s

bool SensorState(unsigned port, enum retro_sensor_action action, unsigned)
{
  const int what = static_cast<int>(action);
  // Enabling the accelerometer or the gyroscope (not the light sensor: Sunlight is a setting).
  return port == 0 && (what == SENSOR_ACCELEROMETER_ENABLE || what == SENSOR_GYROSCOPE_ENABLE ||
                       what == SENSOR_ACCELEROMETER_ENABLE + 1 || what == SENSOR_GYROSCOPE_ENABLE + 1);
}

float SensorInput(unsigned port, unsigned id)
{
  if (port != 0)
    return 0.0f;
  const Tilt& t = s_tilt;
  const float g = 9.81f;
  switch (id)
  {
  case SENSOR_ACCELEROMETER_X:  // tilted right: the right side down reads negative
    return std::clamp(-t.stick_x * MAX_TILT + (t.motion ? (t.accel.x - t.rest.x) * g : 0.0f), -MAX_TILT, MAX_TILT);
  case SENSOR_ACCELEROMETER_Y:  // the top tilted away (the stick up): negative
    return std::clamp(t.stick_y * MAX_TILT - (t.motion ? (t.accel.z - t.rest.z) * g : 0.0f), -MAX_TILT, MAX_TILT);
  case SENSOR_GYROSCOPE_Z:  // turned clockwise (the stick right): negative
    return std::clamp(-t.stick_x * MAX_TURN * 0.9f + (t.motion ? t.turn : 0.0f), -MAX_TURN, MAX_TURN);
  default:
    return 0.0f;
  }
}

// The newest picture (valid until the next retro_run).
const void* s_frame = nullptr;
unsigned s_frame_w = 0, s_frame_h = 0;
size_t s_frame_pitch = 0;
bool s_frame_new = false;
// Resolution: the picture made bigger, and the part of the game texture the picture fills.
std::vector<uint16_t> s_scaled;
uint32_t s_texture_w = 0, s_texture_h = 0;
bool s_texture_scaled = false;
double s_scale_ms = 0.0;  // time spent making pictures bigger (for the log)
int s_scale_frames = 0;
std::string s_scale_name;
GameAudio* s_audio = nullptr;
double s_sample_rate = 32040.0;

void CoreLog(enum retro_log_level level, const char* format, ...)
{
  if (level < RETRO_LOG_INFO)
    return;
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  size_t length = strlen(line);
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
    line[--length] = 0;
  LogF("%s: %s", s_core->name, line);
}

// The menus' settings as Snes9x's options.
void BuildSnesVariables(const std::string& g)
{
  s_variables["snes9x_overclock_superfx"] = Settings::Get("superfx_clock", g) + "%";
  s_variables["snes9x_overclock_cycles"] = Settings::Get("cpu_clock", g);
  s_variables["snes9x_reduce_sprite_flicker"] = Settings::Get("sprite_flicker", g);
  s_variables["snes9x_overscan"] = Settings::Get("overscan", g);
  s_variables["snes9x_aspect"] = "4:3";
  s_variables["snes9x_region"] = Settings::Get("region", g);
  s_variables["snes9x_block_invalid_vram_access"] = Settings::Get("block_vram", g);
  s_variables["snes9x_echo_buffer_hack"] = Settings::Get("echo_hack", g);
  s_variables["snes9x_blargg"] = Settings::Get("ntsc_filter", g);
  s_variables["snes9x_hires_blend"] = Settings::Get("hires_blend", g);
  s_variables["snes9x_audio_interpolation"] = Settings::Get("audio_interpolation", g);
  s_variables["snes9x_up_down_allowed"] = Settings::Get("up_down", g);
  s_variables["snes9x_randomize_memory"] = Settings::Get("randomize_memory", g);
  s_variables["snes9x_gfx_transp"] = Settings::Get("transparency", g);
  s_variables["snes9x_gfx_clip"] = "enabled";
  s_variables["snes9x_show_lightgun_settings"] = "disabled";
  s_variables["snes9x_show_advanced_av_settings"] = "disabled";
}

// The menus' settings as FCEUmm's options (the settings both consoles share keep Snes9x's
// values: some have other names here).
void BuildNesVariables(const std::string& g)
{
  const std::string region = Settings::Get("region", g);
  s_variables["fceumm_region"] = region == "ntsc" ? "NTSC" : region == "pal" ? "PAL" : "Auto";
  // Crop Overscan: the top and bottom 8 lines (224 of the 240), as most TVs hid them.
  const char* crop = Settings::Get("overscan", g) == "disabled" ? "0" : "8";
  s_variables["fceumm_overscan_v_top"] = crop;
  s_variables["fceumm_overscan_v_bottom"] = crop;
  s_variables["fceumm_overscan_h_left"] = "0";
  s_variables["fceumm_overscan_h_right"] = "0";
  s_variables["fceumm_aspect"] = "8:7 PAR";  // EmuSuNi shapes the picture itself
  s_variables["fceumm_palette"] = Settings::Get("nes_palette", g);
  const std::string ntsc = Settings::Get("ntsc_filter", g);
  s_variables["fceumm_ntsc_filter"] = ntsc == "s-video" ? "svideo" : ntsc == "rf" ? "composite" : ntsc;
  s_variables["fceumm_hdpacks"] = "disabled";
  s_variables["fceumm_sndrate_hint"] = "48KHz";  // the PS4's own rate
  s_variables["fceumm_sndquality"] = "High";
  s_variables["fceumm_sndvolume"] = "7";
  for (const char* channel : {"fceumm_apu_1", "fceumm_apu_2", "fceumm_apu_3", "fceumm_apu_4", "fceumm_apu_5"})
    s_variables[channel] = "enabled";
  s_variables["fceumm_turbo_enable"] = Settings::GetOn("nes_turbo", g) ? "Both" : "None";
  s_variables["fceumm_turbo_delay"] = "3";
  s_variables["fceumm_up_down_allowed"] = Settings::Get("up_down", g);
  s_variables["fceumm_nospritelimit"] = Settings::Get("sprite_flicker", g);
  // Light and normal overclock add lines after the picture (the most compatible); Max adds them
  // in the vertical blank.
  const std::string clock = Settings::Get("cpu_clock", g);
  s_variables["fceumm_overclocking"] = clock == "disabled" ? "disabled" : clock == "max" ? "2x-VBlank" : "2x-Postrender";
  s_variables["fceumm_ramstate"] = Settings::Get("randomize_memory", g) == "enabled" ? "random" : "fill $ff";
  s_variables["fceumm_game_genie"] = "disabled";
}

// Mupen64Plus-Next's options: drawn by ParaLLEl-RDP (Vulkan), the recompilers when the PS4 lets
// their code run, else the interpreters.
void BuildN64Variables(const std::string& g)
{
  const std::string prefix = "mupen64plus-";
  s_variables[prefix + "rdp-plugin"] = "parallel";
  s_variables[prefix + "cpucore"] =
      s_dynarec_ok && Settings::Get("n64_cpu", g) == "dynarec" ? "dynamic_recompiler" : "cached_interpreter";
  // Fast: the core's "hle" with ParaLLEl-RDP (patched: HLE sound, ParaLLEl-RSP for the rest).
  const std::string rsp = Settings::Get("n64_rsp", g);
  s_variables[prefix + "rsp-plugin"] = !s_rsp_jit_ok ? "cxd4" : rsp == "hle" ? "hle" : rsp == "parallel" ? "parallel" : "cxd4";
  const std::string resolution = Settings::Get("n64_resolution", g);
  s_variables[prefix + "parallel-rdp-upscaling"] = resolution == "4" ? "4x" : resolution == "2" ? "2x" : "1x";
  // Asynchronous: the GPU draws while the N64 runs on (the CPU doesn't wait for each picture).
  s_variables[prefix + "parallel-rdp-synchronous"] = Settings::Get("n64_sync", g) == "accurate" ? "True" : "False";
  // The picture (ParaLLEl-RDP): the video chip's filters and the upscaling quirks are read again every
  // frame; Screen Effects at the next start.
  const bool aa = Settings::Get("n64_aa", g) != "off";
  s_variables[prefix + "parallel-rdp-vi-aa"] = aa ? "True" : "False";
  s_variables[prefix + "parallel-rdp-divot-filter"] = aa ? "True" : "False";  // cleans up the AA's edges
  s_variables[prefix + "parallel-rdp-dither-filter"] = Settings::Get("n64_dither_filter", g) != "off" ? "True" : "False";
  s_variables[prefix + "parallel-rdp-native-tex-rect"] = Settings::Get("n64_2d", g) == "hd" ? "False" : "True";
  s_variables[prefix + "parallel-rdp-native-texture-lod"] = Settings::Get("n64_texture_lod", g) == "native" ? "True" : "False";
  s_variables[prefix + "parallel-rdp-super-sampled-read-back"] =
      Settings::Get("n64_fb_effects", g) == "smooth" ? "True" : "False";
  s_variables[prefix + "aspect"] = "4:3";
  s_variables[prefix + "Framerate"] = "Original";
  // N64 CPU Timing: Light is 3 counts per instruction (the core's game list uses it for 25 games),
  // Normal the list's own (2 for the others). GoldenEye then draws about 23 frames a second instead
  // of 33 (the console: about 20), with about a third less RSP and RDP work per second.
  const std::string timing = Settings::Get("n64_cpu_timing", g);
  const bool light = timing == "light" || (timing == "auto" && s_n64_name == "GOLDENEYE");
  s_variables[prefix + "CountPerOp"] = light ? "3" : "0";
  s_variables[prefix + "virefresh"] = "Auto";
  s_variables[prefix + "pak1"] = Settings::Get("n64_pak", g);
  for (const char* pak : {"pak2", "pak3", "pak4"})
    s_variables[prefix + pak] = "memory";
  s_variables[prefix + "astick-deadzone"] = "15";
  s_variables[prefix + "astick-sensitivity"] = "100";
  // Independent C-button Controls: every N64 button on a RetroPad button of its own, as
  // src/Controls.cpp sends them (without it R2 is a C-button mode that takes A and B away, R and L
  // are R1 and L1, and the C buttons on the face buttons only work with R2 held).
  s_variables[prefix + "alt-map"] = "True";
}

// A setting of the running game's console, else `other` (the global value of a setting for other
// consoles isn't this one's).
std::string ConsoleSetting(const char* key, const std::string& g, const char* other)
{
  const Settings::Setting* setting = Settings::Find(key);
  return setting && Settings::AppliesTo(*setting, s_system) ? Settings::Get(key, g) : std::string(other);
}

// mGBA's options (the three Game Boys). A game for the first Game Boy runs on it with the colours
// chosen (Game Boy Colours), or on the Super Game Boy: mGBA's Autodetect would make the ones with
// Super Game Boy features (Pokémon) a Super Game Boy's 256x224 picture at 61 FPS. A Game Boy Color
// game runs on the Game Boy Color, a Game Boy Advance game on its own.
void BuildGbVariables(const std::string& g)
{
  const bool dmg = s_system != SystemId::GBA && !s_gb_color_game;
  const std::string palette = Settings::Get("gb_palette", g);
  const bool sgb = dmg && palette == "sgb";
  s_variables["mgba_gb_model"] = !dmg ? "Autodetect" : sgb ? "Super Game Boy" : "Game Boy";
  s_variables["mgba_sgb_borders"] = sgb ? "ON" : "OFF";
  // The palette, and where the game's own colours come from: the Game Boy Color's list of
  // Nintendo's games (the others get its green and blue, as on it), the Super Game Boy's.
  s_variables["mgba_gb_colors_preset"] = palette == "gbc" ? "1" : sgb ? "2" : "0";
  s_variables["mgba_gb_colors"] = palette == "dmg"    ? "DMG Green" :
                                  palette == "pocket" ? "GB Pocket" :
                                  palette == "light"  ? "GB Light" :
                                  palette == "gray"   ? "Grayscale" :
                                  sgb                 ? "SGB 1-A" :
                                                        "GBC Dark Green \xE2\x86\x92" "A";  // "→A"
  const std::string bios = ConsoleSetting("gb_bios", g, "on");
  s_variables["mgba_use_bios"] = bios == "off" ? "OFF" : "ON";
  s_variables["mgba_skip_bios"] = bios == "skip" ? "ON" : "OFF";
  s_variables["mgba_color_correction"] = ConsoleSetting("lcd_colors", g, "vivid") == "lcd" ? "Auto" : "OFF";
  const std::string ghosting = ConsoleSetting("lcd_ghosting", g, "off");
  s_variables["mgba_interframe_blending"] = ghosting == "smart" ? "mix_smart" :
                                            ghosting == "mix"   ? "mix" :
                                            ghosting == "lcd"   ? "lcd_ghosting" :
                                                                  "OFF";
  const bool soft = ConsoleSetting("gb_audio_filter", g, "off") == "soft";
  s_variables["mgba_audio_low_pass_filter"] = soft ? "enabled" : "disabled";
  s_variables["mgba_audio_low_pass_range"] = "60";
  s_variables["mgba_allow_opposing_directions"] = Settings::Get("up_down", g) == "enabled" ? "yes" : "no";
  s_variables["mgba_solar_sensor_level"] = ConsoleSetting("gba_solar", g, "6");
  s_variables["mgba_force_gbp"] = "OFF";
  s_variables["mgba_idle_optimization"] = "Remove Known";
  s_variables["mgba_frameskip"] = "disabled";
  s_tilt.motion = ConsoleSetting("gb_tilt", g, "stick") == "motion";
}

void BuildVariables()
{
  s_variables.clear();
  if (s_system == SystemId::NES)
    BuildNesVariables(s_game_id);
  else if (s_system == SystemId::N64)
    BuildN64Variables(s_game_id);
  else if (IsHandheld(s_system))
    BuildGbVariables(s_game_id);
  else
    BuildSnesVariables(s_game_id);
  s_variables_changed = true;
}

// ---- libretro's Vulkan interface (the core's side of the display) ----
void VulkanSetImage(void*, const retro_vulkan_image* image, uint32_t semaphore_count,
                    const VkSemaphore* semaphores, uint32_t)
{
  if (!image)
    return;
  s_hw_image = *image;
  s_hw_image_set = true;
  if (semaphore_count > 0)
    s_display->WaitForSemaphores(semaphore_count, semaphores);
}

uint32_t VulkanGetSyncIndex(void*)
{
  return s_display->SyncIndex();
}

uint32_t VulkanGetSyncIndexMask(void*)
{
  return s_display->SyncIndexMask();
}

void VulkanSetCommandBuffers(void*, uint32_t count, const VkCommandBuffer*)
{
  // ParaLLEl-RDP submits its work itself; a core that hands over command buffers isn't supported.
  static bool logged = false;
  if (count > 0 && !logged)
  {
    logged = true;
    LogLine("vulkan: the core's command buffers aren't submitted (unsupported)");
  }
}

void VulkanWaitSyncIndex(void*)
{
  s_display->WaitSyncIndex();
}

void VulkanLockQueue(void*)
{
  s_display->LockQueue();
}

void VulkanUnlockQueue(void*)
{
  s_display->UnlockQueue();
}

void VulkanSetSignalSemaphore(void*, VkSemaphore semaphore)
{
  s_display->SignalSemaphore(semaphore);
}

bool Environment(unsigned command, void* data)
{
  switch (command)
  {
  case RETRO_ENVIRONMENT_GET_VARIABLE:
  {
    auto* variable = static_cast<retro_variable*>(data);
    if (!variable || !variable->key)
      return false;
    const auto it = s_variables.find(variable->key);
    if (it == s_variables.end())
    {
      variable->value = nullptr;
      return false;
    }
    variable->value = it->second.c_str();
    return true;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *static_cast<bool*>(data) = s_variables_changed;
    s_variables_changed = false;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    return *static_cast<const retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_RGB565;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *static_cast<const char**>(data) = s_system_dir.c_str();
    return true;
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *static_cast<const char**>(data) = s_save_dir.c_str();
    return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    static_cast<retro_log_callback*>(data)->log = CoreLog;
    return true;
  case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
    // 1: draw the picture, 2: make the sound.
    *static_cast<int*>(data) = (s_render_video ? 1 : 0) | 2;
    return true;
  case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
    if (!s_info_ext.data)
      return false;
    *static_cast<const retro_game_info_ext**>(data) = &s_info_ext;
    return true;
  case RETRO_ENVIRONMENT_SET_MESSAGE:
  {
    const auto* message = static_cast<const retro_message*>(data);
    if (!message || !message->msg)
      return false;
    s_message = message->msg;
    LogF("%s: message: %s", s_core->name, message->msg);
    return true;
  }
  case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    static_cast<retro_rumble_interface*>(data)->set_rumble_state = SetRumble;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    // mGBA reads all the buttons at once (InputState's mask); the other cores as before.
    return IsHandheld(s_system);
  case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
  {
    if (!IsHandheld(s_system) || !data)
      return false;
    auto* sensors = static_cast<retro_sensor_interface*>(data);
    sensors->set_sensor_state = SensorState;
    sensors->get_sensor_input = SensorInput;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_HW_RENDER:
  {
    // Vulkan only, on the display's device (made by ParaLLEl-RDP in the N64 build).
    auto* render = static_cast<retro_hw_render_callback*>(data);
    if (!render || render->context_type != RETRO_HW_CONTEXT_VULKAN || !s_display || !s_display->SharesVulkan())
      return false;
    s_hw_render = *render;
    s_hw_vulkan = true;
    LogLine("vulkan: the core draws with Vulkan");
    return true;
  }
  case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    // The device was made through ParaLLEl-RDP's own negotiation when the app started.
    return s_display && s_display->SharesVulkan();
  case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    if (!s_hw_vulkan)
      return false;
    *static_cast<const retro_hw_render_interface**>(data) =
        reinterpret_cast<const retro_hw_render_interface*>(&s_vulkan);
    return true;
  case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
  {
    // The region or the NTSC filter changed while the game runs.
    const auto* av = static_cast<const retro_system_av_info*>(data);
    if (av->timing.sample_rate > 0)
      s_sample_rate = av->timing.sample_rate;
    if (av->timing.fps > 0)
      s_game_fps = av->timing.fps;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
  case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    return true;
  default:
    return false;
  }
}

void VideoRefresh(const void* pixels, unsigned width, unsigned height, size_t pitch)
{
  if (pixels == RETRO_HW_FRAME_BUFFER_VALID)
  {
    // The core's own image (set_image): nothing to upload.
    s_frame_hw = true;
    s_frame = nullptr;
    s_frame_w = width;
    s_frame_h = height;
    s_frame_new = true;
    return;
  }
  if (!pixels)
    return;  // a duplicated frame
  s_frame_hw = false;
  s_frame = pixels;
  s_frame_w = width;
  s_frame_h = height;
  s_frame_pitch = pitch;
  s_frame_new = true;
}

size_t AudioBatch(const int16_t* samples, size_t frames)
{
  if (s_audio)
    s_audio->Push(samples, frames, s_sample_rate);
  return frames;
}

void AudioSample(int16_t left, int16_t right)
{
  const int16_t frame[2] = {left, right};
  AudioBatch(frame, 1);
}

void InputPoll() {}

int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
  if (port < MAX_PLAYERS && (device & 0xFF) == RETRO_DEVICE_ANALOG)
    return index < 2 && id < 2 ? s_analog[port][index][id] : 0;
  if (port >= MAX_PLAYERS || (device & 0xFF) != RETRO_DEVICE_JOYPAD)
    return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
    return static_cast<int16_t>(s_buttons[port]);
  return id < 16 ? static_cast<int16_t>((s_buttons[port] >> id) & 1) : 0;
}

// ---- The DualShock 4s: player 1 is the user who started the app, then the others signed in. ----
struct Pads
{
  struct Pad
  {
    int32_t user = -1;
    int handle = -1;
    uint32_t held = 0;
    OrbisPadData data = {};
    bool ok = false;
  };
  std::array<Pad, MAX_PLAYERS> pads;
  int32_t first_user = -1;
  float recheck = 0.0f;

  void Open()
  {
    sceUserServiceGetInitialUser(&first_user);
    Refresh(true);
  }
  void Close()
  {
    for (Pad& pad : pads)
    {
      if (pad.handle >= 0)
        scePadClose(pad.handle);
      pad = {};
    }
  }
  // Signed-in users in player order; a new one becomes the next player.
  void Refresh(bool log)
  {
    OrbisUserServiceLoginUserIdList users;
    std::memset(&users, 0xFF, sizeof(users));
    sceUserServiceGetLoginUserIdList(&users);
    std::vector<int32_t> order;
    if (first_user >= 0)
      order.push_back(first_user);
    for (int32_t user : users.userId)
    {
      if (user >= 0 && user != first_user && order.size() < MAX_PLAYERS)
        order.push_back(user);
    }
    for (size_t i = 0; i < pads.size(); i++)
    {
      const int32_t user = i < order.size() ? order[i] : -1;
      Pad& pad = pads[i];
      if (pad.user == user && (user < 0 || pad.handle >= 0))
        continue;
      if (pad.handle >= 0)
        scePadClose(pad.handle);
      pad = {};
      pad.user = user;
      if (user >= 0)
      {
        pad.handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
        if (log || pad.handle >= 0)
          LogF("pads: player %zu = user %d (pad %#x)", i + 1, user, static_cast<unsigned>(pad.handle));
      }
    }
  }
  void Read(float dt)
  {
    recheck -= dt;
    if (recheck <= 0.0f)
    {
      recheck = 1.0f;
      Refresh(false);
    }
    for (Pad& pad : pads)
    {
      pad.ok = false;
      if (pad.handle < 0)
        continue;
      OrbisPadData data = {};
      if (scePadReadState(pad.handle, &data) == 0 && data.connected)
      {
        pad.data = data;
        pad.ok = true;
      }
    }
  }
};

// ---- Drawing the picture ----
struct Picture
{
  ImVec2 min, max;
  float rows = 224.0f;  // the game's lines (for scanlines)
  float columns = 0.0f; // a handheld's screen: its dots across (for the grid between them)
};

// The bezel (Video: Bezel): a frame over the TV's sides, see-through where the game goes. The
// picture is fitted in that window, found along the middle row and column.
struct Bezel
{
  std::string path;  // the file shown ("": none)
  ImTextureID texture{};
  ImVec2 min, max;  // the window on the screen
};
Bezel s_bezel;

// The bezel file the game's console is set to ("": none). Your Own: bezels/<console>/ in the data
// folder with the ROM's name (as The Bezel Project names its bezels) or the game's title, else
// bezels/<console>.png; without one, the console's first built-in bezel.
std::string BezelPath(const Library::Game& game)
{
  const System& system = GetSystem(game.system);
  const std::string choice = Settings::Get(std::string(system.key) + "_bezel", game.key);
  if (choice.empty() || choice == "off")
    return {};
  const std::string builtin = std::string(App::ASSETS_DIR) + "/xmb/bezels/";
  if (choice == "tv")
    return builtin + "tv.png";
  if (choice != "custom")
    return builtin + system.key + "-" + choice + ".png";
  const std::string own = std::string(App::DATA_DIR) + "/bezels/" + system.key;
  for (const std::string& path : {own + "/" + game.id + ".png", own + "/" + game.title + ".png", own + ".png"})
  {
    if (FileExists(path))
      return path;
  }
  LogF("bezel: none of your own in %s/bezels/%s", App::DATA_DIR, system.key);
  return builtin + system.key + "-console.png";
}

void FreeBezel(Display& display)
{
  if (s_bezel.texture)
    display.DestroyTexture(s_bezel.texture);
  s_bezel = {};
}

// Shows the bezel the settings choose, when it changed.
void LoadBezel(Display& display, const Library::Game& game)
{
  const std::string path = BezelPath(game);
  if (path == s_bezel.path)
    return;
  FreeBezel(display);
  s_bezel.path = path;
  if (path.empty())
    return;
  ImageRGBA image;
  if (!LoadPng(path, &image, Display::WIDTH, Display::HEIGHT))
  {
    LogF("bezel: %s can't be read", path.c_str());
    return;
  }
  // The window: see-through from the middle out along the middle row, then along its own middle
  // column (an old TV's screen is off to one side).
  const auto clear = [&image](uint32_t x, uint32_t y) {
    return image.pixels[(static_cast<size_t>(y) * image.width + x) * 4 + 3] < 128;
  };
  const uint32_t middle_y = image.height / 2;
  uint32_t x0 = image.width / 2, x1 = x0;
  if (!clear(x0, middle_y))
  {
    LogF("bezel: %s isn't see-through in the middle", path.c_str());
    return;
  }
  while (x0 > 0 && clear(x0 - 1, middle_y))
    x0--;
  while (x1 + 1 < image.width && clear(x1 + 1, middle_y))
    x1++;
  const uint32_t middle_x = (x0 + x1) / 2;
  uint32_t y0 = middle_y, y1 = middle_y;
  while (y0 > 0 && clear(middle_x, y0 - 1))
    y0--;
  while (y1 + 1 < image.height && clear(middle_x, y1 + 1))
    y1++;
  if (x1 - x0 < image.width / 4 || y1 - y0 < image.height / 4)
  {
    LogF("bezel: %s's window is too small", path.c_str());
    return;
  }
  s_bezel.texture = display.CreateTexture(image.pixels.data(), image.width, image.height);
  if (!s_bezel.texture)
    return;
  // The PNG fills the screen (a smaller one is stretched).
  const float sx = static_cast<float>(Display::WIDTH) / image.width;
  const float sy = static_cast<float>(Display::HEIGHT) / image.height;
  s_bezel.min = ImVec2(x0 * sx, y0 * sy);
  s_bezel.max = ImVec2((x1 + 1) * sx, (y1 + 1) * sy);
  LogF("bezel: %s, window %.0fx%.0f at %.0f,%.0f", path.c_str(), s_bezel.max.x - s_bezel.min.x,
       s_bezel.max.y - s_bezel.min.y, s_bezel.min.x, s_bezel.min.y);
}

void DrawBezel(ImDrawList* dl)
{
  if (s_bezel.texture)
  {
    dl->AddImage(ImTextureRef(s_bezel.texture), ImVec2(0.0f, 0.0f),
                 ImVec2(static_cast<float>(Display::WIDTH), static_cast<float>(Display::HEIGHT)));
  }
}

Picture PlacePicture(const std::string& game);

void DrawScanlines(ImDrawList* dl, const Picture& picture, const std::string& game)
{
  const std::string scanlines = Settings::Get("scanlines", game);
  if (scanlines == "off")
    return;
  const int alpha = scanlines == "light" ? 60 : scanlines == "medium" ? 110 : 160;
  const float row = (picture.max.y - picture.min.y) / picture.rows;
  if (picture.columns > 0.0f)
  {
    // A handheld's LCD: thin dark lines between its dots, both ways.
    const ImU32 grid = IM_COL32(0, 0, 0, alpha * 3 / 4);
    const float column = (picture.max.x - picture.min.x) / picture.columns;
    const float line = std::max(1.0f, std::min(row, column) * 0.18f);
    for (float y = picture.min.y + row; y < picture.max.y - 0.5f; y += row)
      dl->AddRectFilled(ImVec2(picture.min.x, y - line * 0.5f), ImVec2(picture.max.x, y + line * 0.5f), grid);
    for (float x = picture.min.x + column; x < picture.max.x - 0.5f; x += column)
      dl->AddRectFilled(ImVec2(x - line * 0.5f, picture.min.y), ImVec2(x + line * 0.5f, picture.max.y), grid);
    return;
  }
  const ImU32 dark = IM_COL32(0, 0, 0, alpha);
  for (float y = picture.min.y; y < picture.max.y; y += row)
    dl->AddRectFilled(ImVec2(picture.min.x, y + row * 0.55f), ImVec2(picture.max.x, y + row), dark);
}

// The NTSC filter makes the picture (the Super Nintendo's and the NES's).
bool NtscFilterOn(const std::string& game)
{
  return (s_system == SystemId::SNES || s_system == SystemId::NES) && Settings::Get("ntsc_filter", game) != "disabled";
}

// The GPU filter for the picture (Graphics > GPU Upscaler), as this console and picture can use it.
GpuFilter::Mode GpuMode(const std::string& game)
{
  const std::string name = Settings::Get("gpu_filter", game);
  GpuFilter::Mode mode = name == "pixel"   ? GpuFilter::Mode::PixelArt :
                         name == "ai"      ? GpuFilter::Mode::AI :
                         name == "fsr"     ? GpuFilter::Mode::Smooth :
                         name == "sharpen" ? GpuFilter::Mode::Sharpen :
                                             GpuFilter::Mode::Off;
  if (s_frame_hw)
  {
    // The N64's 3D has no pixel art to redraw: Smooth HD, which at 4x (as wide as the screen
    // already) only sharpens.
    if (mode == GpuFilter::Mode::PixelArt || mode == GpuFilter::Mode::AI)
      mode = GpuFilter::Mode::Smooth;
    if (mode == GpuFilter::Mode::Smooth && s_frame_w >= 2560)
      mode = GpuFilter::Mode::Sharpen;
  }
  else if ((mode == GpuFilter::Mode::PixelArt || mode == GpuFilter::Mode::AI) && NtscFilterOn(game))
  {
    mode = GpuFilter::Mode::Off;  // the NTSC filter's picture is a TV's look of its own
  }
  return mode;
}

// The mode redraws the console's own picture (the CPU's Resolution is left out).
bool GpuRedraws(GpuFilter::Mode mode)
{
  return mode == GpuFilter::Mode::PixelArt || mode == GpuFilter::Mode::AI || mode == GpuFilter::Mode::Smooth;
}

Picture PlacePicture(const std::string& game)
{
  Picture picture;
  // The screen, or the bezel's window.
  ImVec2 area(0.0f, 0.0f);
  float W = static_cast<float>(Display::WIDTH), H = static_cast<float>(Display::HEIGHT);
  if (s_bezel.texture)
  {
    area = s_bezel.min;
    W = s_bezel.max.x - s_bezel.min.x;
    H = s_bezel.max.y - s_bezel.min.y;
  }
  // The picture in the console's units: 256 wide (512 hi-res, 602 with the NTSC filter), 224 or
  // 239 lines (448 / 478 interlaced).
  float frame_h = static_cast<float>(s_frame_h);
  if (s_frame_hw && s_frame_w >= 1280)
    frame_h /= static_cast<float>(s_frame_w / 640);  // ParaLLEl-RDP's upscaled scanout
  const float lines = frame_h > 300.0f ? frame_h * 0.5f : frame_h;
  picture.rows = lines;
  const std::string aspect = Settings::Get("aspect", game);
  float ratio = 4.0f / 3.0f;
  // A handheld's screen keeps its shape (10:9, the Game Boy Advance's 3:2) at Auto and Pixel
  // Perfect: its dots are square. The Super Game Boy's 256x224 picture was a TV's.
  const bool screen = IsHandheld(s_system) && s_frame_w != 256;
  if (screen)
    picture.columns = static_cast<float>(s_frame_w);
  if (screen && (aspect == "auto" || aspect == "pixel"))
    ratio = static_cast<float>(s_frame_w) / std::max(1.0f, frame_h);
  else if (aspect == "pixel" && s_system != SystemId::N64)  // the N64's picture is 4:3 already
    ratio = 256.0f / std::max(1.0f, lines);
  else if (aspect == "wide")
    ratio = 16.0f / 9.0f;
  else if (aspect == "stretch")
    ratio = W / H;
  const std::string scaling = Settings::Get("scaling", game);
  float height = H;
  if (scaling == "integer")
    height = std::max(1.0f, std::floor(H / lines)) * lines;
  else if (scaling == "2" || scaling == "3" || scaling == "4")
    height = lines * static_cast<float>(std::atoi(scaling.c_str()));
  if (height > H)
    height = std::max(1.0f, std::floor(H / lines)) * lines;  // a bezel's window: the biggest that fits
  float width = height * ratio;
  if (aspect == "stretch" && scaling == "fit")
    width = W;
  if (width > W)
  {
    height *= W / width;
    width = W;
  }
  const std::string zoom = Settings::Get("zoom", game);
  if (zoom != "off")
  {
    const float factor = std::atoi(zoom.c_str()) / 100.0f;
    width *= factor;
    height *= factor;
  }
  picture.min = ImVec2(area.x + (W - width) * 0.5f, area.y + (H - height) * 0.5f);
  picture.max = ImVec2(picture.min.x + width, picture.min.y + height);
  return picture;
}

void DrawPicture(ImDrawList* dl, Display& display, const std::string& game)
{
  if (s_frame_w == 0 || s_frame_h == 0)
    return;
  if (s_frame_hw)
  {
    // The core's image, whole.
    if (!s_hw_image_set)
      return;
    const Picture picture = PlacePicture(game);
    const GpuFilter::Mode gpu = GpuMode(game);
    if (gpu != GpuFilter::Mode::Off)
    {
      GpuFilter::Source source;
      source.view = s_hw_image.image_view;
      source.layout = s_hw_image.image_layout;
      source.width = s_frame_w;
      source.height = s_frame_h;
      if (GpuFilter::Draw(dl, gpu, source, picture.min, picture.max))
      {
        DrawScanlines(dl, picture, game);
        return;
      }
    }
    // Above Native (2x, 4x) always smooth, as Texture Filtering promises: sharp (nearest) drops
    // whole columns of the 2560-wide picture and doubles some rows on the way to the screen.
    const bool smooth = s_frame_w >= 1280 || Settings::Get("filter", game) == "smooth";
    const ImTextureID image = display.ExternalTexture(s_hw_image.image_view, s_hw_image.image_layout, smooth);
    dl->AddImage(ImTextureRef(image), picture.min, picture.max);
    DrawScanlines(dl, picture, game);
    return;
  }
  if (s_texture_w == 0 || s_texture_h == 0)
    return;
  const Picture picture = PlacePicture(game);
  // The GPU's filters: from the console's own picture (a picture the CPU made bigger only for HD
  // Sharpen; it is one frame old when the filter was just chosen).
  const GpuFilter::Mode gpu = GpuMode(game);
  if (gpu != GpuFilter::Mode::Off && (!s_texture_scaled || !GpuRedraws(gpu)))
  {
    GpuFilter::Source source;
    source.view = display.GameView();
    source.width = s_texture_w;
    source.height = s_texture_h;
    source.exact = false;
    if (GpuFilter::Draw(dl, gpu, source, picture.min, picture.max))
    {
      DrawScanlines(dl, picture, game);
      return;
    }
  }
  // A picture made bigger is always drawn smooth (it is rarely an exact multiple of the TV's
  // lines). Smooth stays half a texel inside: around the picture are older, bigger ones.
  const bool smooth = s_texture_scaled || Settings::Get("filter", game) == "smooth";
  const float inset = smooth ? 0.5f : 0.0f;
  const ImVec2 uv0(inset / Display::GAME_TEXTURE_W, inset / Display::GAME_TEXTURE_H);
  const ImVec2 uv1((s_texture_w - inset) / Display::GAME_TEXTURE_W,
                   (s_texture_h - inset) / Display::GAME_TEXTURE_H);
  dl->AddImage(ImTextureRef(display.GameTexture(smooth)), picture.min, picture.max, uv0, uv1);
  DrawScanlines(dl, picture, game);
}

// The new picture into the game texture: as it is, or made bigger first (Resolution).
void UploadPicture(Display& display, const std::string& game)
{
  int factor = std::atoi(Settings::Get("resolution", game).c_str());
  if (s_frame_w > 300 || s_frame_h > 300)
    factor /= 2;  // hi-res and interlaced pictures are twice as big already
  if (NtscFilterOn(game))
    factor = 1;  // the NTSC filter's picture is a TV's look of its own
  const GpuFilter::Mode gpu = GpuMode(game);
  if (GpuRedraws(gpu) && GpuFilter::Ready(gpu))
    factor = 1;  // the GPU redraws the console's own picture (Graphics > GPU Upscaler)
  while (factor >= 2 && (s_frame_w * factor > Display::GAME_TEXTURE_W ||
                         s_frame_h * factor > Display::GAME_TEXTURE_H))
    factor--;
  if (factor >= 2)
  {
    const std::string name = Settings::Get("upscaler", game);
    const Upscaler::Filter filter = name == "hqx"    ? Upscaler::Filter::HQx :
                                    name == "pixels" ? Upscaler::Filter::Pixels :
                                                       Upscaler::Filter::XBRZ;
    const auto start = Clock::now();
    const int used = Upscaler::Scale(filter, factor, static_cast<const uint16_t*>(s_frame), s_frame_pitch,
                                     static_cast<int>(s_frame_w), static_cast<int>(s_frame_h), &s_scaled);
    if (used >= 2)
    {
      s_scale_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      s_scale_frames++;
      s_scale_name = name + " " + std::to_string(used) + "x";
      s_texture_w = s_frame_w * used;
      s_texture_h = s_frame_h * used;
      s_texture_scaled = true;
      display.UploadGameFrame(s_scaled.data(), s_texture_w, s_texture_h, static_cast<size_t>(s_texture_w) * 2);
      return;
    }
  }
  s_texture_w = s_frame_w;
  s_texture_h = s_frame_h;
  s_texture_scaled = false;
  display.UploadGameFrame(s_frame, s_frame_w, s_frame_h, s_frame_pitch);
}

// ---- Saves ----
// The battery save (RETRO_MEMORY_SAVE_RAM, <key>.srm) and a Game Boy cartridge's clock
// (RETRO_MEMORY_RTC, <key>.rtc), each as last written.
std::vector<uint8_t> s_sram_saved, s_rtc_saved;

void LoadMemory(unsigned id, const std::string& path, std::vector<uint8_t>* saved, const char* what)
{
  uint8_t* data = static_cast<uint8_t*>(s_core->get_memory_data(id));
  const size_t size = s_core->get_memory_size(id);
  saved->clear();
  if (!data || size == 0)
    return;
  FILE* file = fopen(path.c_str(), "rb");
  if (file)
  {
    const size_t read = fread(data, 1, size, file);
    fclose(file);
    LogF("save: %zu of %zu bytes of %s loaded", read, size, what);
  }
  saved->assign(data, data + size);
}

// Writes it when the game changed it (or always, with force). A save the game never touched
// (all 0xFF: a Game Boy Advance game's 128 KiB before it first saves) isn't written.
void SaveMemory(unsigned id, const std::string& path, std::vector<uint8_t>* saved, const char* what, bool force)
{
  const uint8_t* data = static_cast<const uint8_t*>(s_core->get_memory_data(id));
  const size_t size = s_core->get_memory_size(id);
  if (!data || size == 0)
    return;
  if (!force && saved->size() == size && std::memcmp(saved->data(), data, size) == 0)
    return;
  if (std::all_of(data, data + size, [](uint8_t b) { return b == 0xFF; }) && !FileExists(path))
  {
    saved->assign(data, data + size);
    return;
  }
  if (WriteTextFile(path, std::string(reinterpret_cast<const char*>(data), size)))
  {
    saved->assign(data, data + size);
    LogF("save: %s written (%zu bytes)", what, size);
  }
}

void LoadSram()
{
  LoadMemory(RETRO_MEMORY_SAVE_RAM, Saves::SramPath(s_game_id), &s_sram_saved, "battery save");
  LoadMemory(RETRO_MEMORY_RTC, Saves::RtcPath(s_game_id), &s_rtc_saved, "cartridge clock");
}

void SaveSram(bool force)
{
  SaveMemory(RETRO_MEMORY_SAVE_RAM, Saves::SramPath(s_game_id), &s_sram_saved, "battery save", force);
  SaveMemory(RETRO_MEMORY_RTC, Saves::RtcPath(s_game_id), &s_rtc_saved, "cartridge clock", force);
}

bool SaveState(int slot)
{
  const size_t size = s_core->serialize_size();
  if (size == 0)
    return false;
  std::vector<uint8_t> state(size);
  if (!s_core->serialize(state.data(), size))
    return false;
  const std::string path = Saves::StatePath(s_game_id, slot);
  if (!WriteTextFile(path, std::string(reinterpret_cast<const char*>(state.data()), size)))
    return false;
  if (s_frame && s_frame_w && s_frame_h)
  {
    ImageRGBA picture = FromRgb565(static_cast<const uint16_t*>(s_frame), s_frame_w, s_frame_h,
                                   s_frame_pitch);
    SavePng(Saves::StatePicturePath(s_game_id, slot), Shrink(picture, 512, 448));
  }
  LogF("state: saved slot %d (%zu bytes)", slot, size);
  return true;
}

bool LoadState(int slot)
{
  const std::string data = ReadTextFile(Saves::StatePath(s_game_id, slot));
  if (data.empty())
    return false;
  const bool ok = s_core->unserialize(data.data(), data.size());
  LogF("state: slot %d %s", slot, ok ? "loaded" : "refused");
  return ok;
}

// ---- Overlays ----
struct Stats
{
  float fps_time = 0.0f;
  int frames = 0, emulated = 0;
  float fps = 0.0f, speed = 0.0f;
  std::array<float, 120> frame_ms{};
  size_t at = 0;
  // The core's time per emulated frame since the last N64 speed line (the log).
  double core_ms = 0.0;
  int core_runs = 0;
  float core_max = 0.0f;
  // The frontend's drawing and flip per refresh, over the same time (and how much of it waited for
  // the TV or the GPU), and the frames shown in it.
  double draw_ms = 0.0, draw_wait_ms = 0.0;
  int draws = 0;
  int frames10 = 0;
  float time10 = 0.0f;
  // ---- The pacing line, over the same 10 s of play (the in-game menu left out) ----
  int emulated10 = 0;
  int64_t total = 0;                   // frames emulated since the game started
  std::vector<float> pass_ms, run_ms;  // each pass of the loop, each emulated frame
  std::array<int, 4> per_pass{};       // passes that ran 0, 1, 2, 3+ frames
  int64_t let_go = 0;                  // frames skipped far behind (FramePacer)
  int64_t vblanks0 = -1, shown0 = 0;   // VideoOut at the window's start
  // Each flip's frame (`total` when it was drawn), by flip number % 8; the newest flip shown that
  // was counted, its frame; and the flips shown by their step from the flip before: 0 = the same
  // frame again (PAL's cadence on 60 Hz), 1 = the next frame, 2 and 3+ = frames jumped over.
  std::array<int64_t, 8> flip_frame{};
  int64_t counted = -1, counted_frame = -1;
  std::array<int, 4> steps{};
};

// The p-th percentile (0-100) of the values (reorders them).
float Percentile(std::vector<float>& values, double p)
{
  if (values.empty())
    return 0.0f;
  const size_t k = std::min(values.size() - 1, static_cast<size_t>(p / 100.0 * (values.size() - 1) + 0.5));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
  return values[k];
}

// The flips VideoOut showed since the last call (shown: the newest one's number), by their steps.
void CountShownFlips(Stats& stats, int64_t shown)
{
  if (stats.counted < 0 || shown < stats.counted || shown - stats.counted > 6)
  {
    stats.counted = shown;  // the start, after the menu, or more flips than the 8 kept
    stats.counted_frame = -1;
    return;
  }
  for (int64_t flip = stats.counted + 1; flip <= shown; flip++)
  {
    const int64_t frame = stats.flip_frame[static_cast<size_t>(flip % 8)];
    if (stats.counted_frame >= 0)
      stats.steps[static_cast<size_t>(std::clamp<int64_t>(frame - stats.counted_frame, 0, 3))]++;
    stats.counted_frame = frame;
  }
  stats.counted = shown;
}

// A new 10 s window for the speed and pacing lines.
void ResetWindow(Stats& stats, const Display::FlipState& flip)
{
  stats.draw_ms = stats.draw_wait_ms = 0.0;
  stats.draws = 0;
  stats.frames10 = 0;
  stats.time10 = 0.0f;
  stats.core_ms = 0.0;
  stats.core_runs = 0;
  stats.core_max = 0.0f;
  stats.emulated10 = 0;
  stats.pass_ms.clear();
  stats.run_ms.clear();
  stats.per_pass = {};
  stats.let_go = 0;
  stats.steps = {};
  stats.vblanks0 = flip.vblanks;
  stats.shown0 = flip.shown;
}

// How evenly the frames reached the TV in the window (after the speed line). Refreshes: VideoOut's
// own count; late: refreshes no new flip was ready for (the picture before stayed on); the same
// frame again: flips of the frame already shown (PAL on 60 Hz: one refresh in six by design);
// jumped: flips 2+ frames after the one before; let go: frames skipped far behind.
void LogPacing(Stats& stats, const Display::FlipState& flip)
{
  const long long refreshes =
      stats.vblanks0 >= 0 && flip.vblanks >= stats.vblanks0 ? static_cast<long long>(flip.vblanks - stats.vblanks0) : -1;
  const long long shown = static_cast<long long>(flip.shown - stats.shown0);
  const float pass_max = stats.pass_ms.empty() ? 0.0f : *std::max_element(stats.pass_ms.begin(), stats.pass_ms.end());
  const float run_max = stats.run_ms.empty() ? 0.0f : *std::max_element(stats.run_ms.begin(), stats.run_ms.end());
  const float pass50 = Percentile(stats.pass_ms, 50), pass95 = Percentile(stats.pass_ms, 95);
  const float pass99 = Percentile(stats.pass_ms, 99);
  const float run50 = Percentile(stats.run_ms, 50), run95 = Percentile(stats.run_ms, 95);
  const float run99 = Percentile(stats.run_ms, 99);
  LogF("pacing: %lld refreshes, %lld late; %lld flips shown: %d the same frame again, %d the next, %d jumped 2+ "
       "frames; %d frames (%lld let go), per pass 0:%d 1:%d 2:%d 3+:%d; passes p50/p95/p99/max %.1f/%.1f/%.1f/%.1f ms;"
       " frames p50/p95/p99/max %.1f/%.1f/%.1f/%.1f ms",
       refreshes, refreshes >= 0 ? refreshes - shown : -1LL, shown, stats.steps[0], stats.steps[1],
       stats.steps[2] + stats.steps[3], stats.emulated10, static_cast<long long>(stats.let_go), stats.per_pass[0],
       stats.per_pass[1], stats.per_pass[2], stats.per_pass[3], pass50, pass95, pass99, pass_max, run50, run95, run99,
       run_max);
}

void DrawStats(ImDrawList* dl, ImFont* bold, const Stats& stats, bool fps, bool graphs, float fill)
{
  if (fps)
  {
    char text[64];
    snprintf(text, sizeof(text), "%.0f FPS  %.0f%%", stats.fps, stats.speed);
    dl->AddRectFilled(ImVec2(30, 26), ImVec2(30 + bold->CalcTextSizeA(30, FLT_MAX, 0, text).x + 30, 76),
                      IM_COL32(0, 0, 0, 150), 10);
    dl->AddText(bold, 30, ImVec2(45, 34), IM_COL32(255, 255, 255, 235), text);
  }
  if (graphs)
  {
    const ImVec2 p0(30, 90), p1(450, 230);
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 150), 10);
    // 16.7 ms (60 FPS) line.
    const float scale = (p1.y - p0.y - 20) / 40.0f;  // 40 ms tall
    const float y60 = p1.y - 10 - 16.7f * scale;
    dl->AddLine(ImVec2(p0.x + 10, y60), ImVec2(p1.x - 10, y60), IM_COL32(120, 255, 140, 120), 1.5f);
    std::vector<ImVec2> points;
    for (size_t i = 0; i < stats.frame_ms.size(); i++)
    {
      const float v = stats.frame_ms[(stats.at + i) % stats.frame_ms.size()];
      points.push_back(ImVec2(p0.x + 10 + i * (p1.x - p0.x - 20) / stats.frame_ms.size(),
                              p1.y - 10 - std::min(v, 40.0f) * scale));
    }
    dl->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(255, 220, 120, 230),
                    ImDrawFlags_None, 2.0f);
    char text[64];
    snprintf(text, sizeof(text), "frame  audio %.0f%%", fill * 100.0f);
    dl->AddText(bold, 22, ImVec2(p0.x + 12, p0.y + 6), IM_COL32(255, 255, 255, 200), text);
  }
}
}  // namespace

Result Run(Display& display, const Library::Game& game, ImFont* regular, ImFont* bold,
           std::string* error)
{
  LogF("game: %s (%s)", game.title.c_str(), game.path.c_str());
  std::vector<uint8_t> rom;
  if (!Library::ReadRom(game.path, &rom, error))
  {
    LogF("game: can't read the ROM: %s", error->c_str());
    return Result::Failed;
  }
  s_system = game.system;
  s_core = GetSystem(game.system).core;
  s_game_id = game.key;
  s_message.clear();
  s_display = &display;
  s_pad_handles.fill(-1);
  s_rumble = {};
  s_hw_render = {};
  s_hw_vulkan = false;
  s_hw_image_set = false;
  s_frame_hw = false;
  for (auto& player : s_analog)
    player = {};
  s_vulkan = {};
  s_vulkan.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
  s_vulkan.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
  s_vulkan.handle = &display;
  s_vulkan.instance = display.Instance();
  s_vulkan.gpu = display.Gpu();
  s_vulkan.device = display.Device();
  s_vulkan.get_device_proc_addr = VulkanFunctions::GetDeviceProcAddr;
  s_vulkan.get_instance_proc_addr = VulkanFunctions::GetInstanceProcAddr;
  s_vulkan.queue = display.Queue();
  s_vulkan.queue_index = display.QueueFamily();
  s_vulkan.set_image = VulkanSetImage;
  s_vulkan.get_sync_index = VulkanGetSyncIndex;
  s_vulkan.get_sync_index_mask = VulkanGetSyncIndexMask;
  s_vulkan.set_command_buffers = VulkanSetCommandBuffers;
  s_vulkan.wait_sync_index = VulkanWaitSyncIndex;
  s_vulkan.lock_queue = VulkanLockQueue;
  s_vulkan.unlock_queue = VulkanUnlockQueue;
  s_vulkan.set_signal_semaphore = VulkanSetSignalSemaphore;
#ifdef EMUSUNI_N64
  if (s_system == SystemId::N64)
  {
    s_dynarec_ok = emusuni_jit_check() >= 0;
    s_rsp_jit_ok = ps4_exec_method >= 0;
    LogF("n64: dynarec %s, RSP recompiler %s", s_dynarec_ok ? "possible" : "not possible (interpreter)",
         s_rsp_jit_ok ? "possible" : "not possible (CXD4)");
  }
#endif
  s_system_dir = std::string(App::DATA_DIR) + "/system";
  s_save_dir = std::string(App::DATA_DIR) + "/saves";
  MakeDirs(s_system_dir);
  Saves::Init(App::DATA_DIR);
  // A Game Boy cartridge's header: for the Game Boy Color (0x80: also the Game Boy, 0xC0: only it),
  // with Super Game Boy features (0x03).
  s_gb_color_game = rom.size() > 0x146 && (rom[0x143] & 0x80) != 0;
  s_gb_sgb_game = rom.size() > 0x146 && rom[0x146] == 0x03;
  s_tilt = {};
  s_n64_name = s_system == SystemId::N64 ? N64Name(rom) : std::string();
  if (s_system == SystemId::N64)
  {
    LogF("n64: cartridge \"%s\", N64 CPU Timing %s", s_n64_name.c_str(),
         Settings::Get("n64_cpu_timing", game.key).c_str());
  }
  if (s_system == SystemId::GB || s_system == SystemId::GBC)
  {
    LogF("gb: a game for the %s%s", s_gb_color_game ? "Game Boy Color" : "Game Boy",
         s_gb_sgb_game ? ", with Super Game Boy features" : "");
  }
  BuildVariables();
  s_frame = nullptr;
  s_frame_w = s_frame_h = 0;
  s_texture_w = s_texture_h = 0;
  s_texture_scaled = false;
  s_buttons.fill(0);

  s_core->set_environment(Environment);
  s_core->set_video_refresh(VideoRefresh);
  s_core->set_audio_sample(AudioSample);
  s_core->set_audio_sample_batch(AudioBatch);
  s_core->set_input_poll(InputPoll);
  s_core->set_input_state(InputState);
  s_core->init();
  if (s_system == SystemId::N64)
  {
    // ParaLLEl-RDP keeps the pipelines it compiled (up to 0.7 s each on the PS4) in one file of
    // data_dir/cache for every version: it checks the data is for this GPU and driver, else it
    // starts afresh. 00.21-00.24 kept a file per version: the biggest one becomes it.
    const std::string dir = std::string(App::DATA_DIR) + "/cache";
    const std::string path = dir + "/parallel-rdp.bin";
    MakeDirs(dir);
    std::error_code ec;
    std::filesystem::path biggest;
    uintmax_t biggest_size = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
      if (entry.path().filename().string().rfind("parallel-rdp-", 0) != 0)
        continue;
      std::error_code size_error;
      const uintmax_t size = entry.file_size(size_error);
      if (!size_error && size > biggest_size)
      {
        biggest = entry.path();
        biggest_size = size;
      }
    }
    if (!biggest.empty() && !FileExists(path))
      std::filesystem::rename(biggest, path, ec);
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
      if (entry.path().filename().string().rfind("parallel-rdp-", 0) == 0)
        std::filesystem::remove(entry.path(), ec);  // an older version's
    }
    setenv("EMUSUNI_RDP_PIPELINE_CACHE", path.c_str(), 1);
    // N64 Picture Timing: ParaLLEl-RDP's command thread makes the picture, shown one frame later
    // (scripts/build-n64-core.sh: read by RDP::init() at the game's start).
    setenv("EMUSUNI_ASYNC_SCANOUT", Settings::Get("n64_picture", game.key) == "now" ? "0" : "1", 1);
  }
  retro_game_info info = {};
  info.path = game.path.c_str();
  info.data = rom.data();
  info.size = rom.size();
  // The same for GET_GAME_INFO_EXT; a .zip's ROM is named after the .zip (the core reads the
  // region from the name).
  const bool zipped = game.path.size() > 4 &&
                      (game.path.compare(game.path.size() - 4, 4, ".zip") == 0 ||
                       game.path.compare(game.path.size() - 4, 4, ".ZIP") == 0);
  const size_t slash = game.path.rfind('/');
  const size_t dot = game.path.rfind('.');
  s_info_path = game.path;
  s_info_dir = slash == std::string::npos ? "." : game.path.substr(0, slash);
  s_info_name = game.id;
  s_info_extension = zipped ? GetSystem(game.system).extensions.front().substr(1) :
                              (dot == std::string::npos ? "" : game.path.substr(dot + 1));
  s_info_ext = {};
  s_info_ext.full_path = zipped ? nullptr : s_info_path.c_str();
  s_info_ext.archive_path = zipped ? s_info_path.c_str() : nullptr;
  s_info_ext.dir = s_info_dir.c_str();
  s_info_ext.name = s_info_name.c_str();
  s_info_ext.ext = s_info_extension.c_str();
  s_info_ext.data = rom.data();
  s_info_ext.size = rom.size();
  s_info_ext.file_in_archive = zipped;
  // N64 CPU Cores: before the game, which starts ParaLLEl-RDP's threads.
  const bool fixed_cores = s_system == SystemId::N64 && Settings::Get("n64_cores", game.key) != "system";
#ifdef EMUSUNI_N64
  emusuni_n64_fixed_cores = fixed_cores ? 1 : 0;
#endif
  const bool loaded = s_core->load_game(&info);
  s_info_ext = {};
  if (s_system == SystemId::N64 || IsHandheld(s_system))
    std::vector<uint8_t>().swap(rom);  // Mupen64Plus and mGBA copy the ROM: up to 64 MiB back to the heap
  if (loaded && s_hw_vulkan && s_hw_render.context_reset)
  {
    // The context is ready (the display's device): the core sets up its renderer.
    s_hw_render.context_reset();
  }
  if (!loaded)
  {
    // The core's own reason when it gave one (a Famicom Disk System game without its BIOS).
    *error = s_message.empty() ? "The emulator couldn't load this ROM" : s_message;
    if (error->find("disksys.rom") != std::string::npos)
      *error += "\nCopy the BIOS disksys.rom to\n" + s_system_dir;
    s_message.clear();
    s_core->deinit();
    return Result::Failed;
  }
  retro_system_av_info av = {};
  s_core->get_system_av_info(&av);
  s_sample_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32040.0;
  s_game_fps = av.timing.fps > 0 ? av.timing.fps : 60.0988;
  LogF("game: %s, %.4f FPS, %.0f Hz sound, %s", s_core->name, s_game_fps, s_sample_rate,
       s_core->get_region() == RETRO_REGION_PAL ? "PAL" : "NTSC");
  LogMemory("game loaded");
  s_core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  // The Super Multitap is the Super Nintendo's (FCEUmm plugs in the Four Score by itself).
  const bool snes = s_system == SystemId::SNES;
  bool multitap = snes && Settings::GetOn("multitap", game.key);
  s_core->set_controller_port_device(1, multitap ? DEVICE_MULTITAP : RETRO_DEVICE_JOYPAD);
  LoadSram();
  LoadBezel(display, game);

  IngameMenu menu;
  menu.Init(&display, regular, bold);
  menu.SetGame(App::DATA_DIR, game.key, game.title, game.system);
  menu.CheatList().ApplyToCore(*s_core);
  if (!menu.CheatList().Entries().empty())
    LogF("cheats: %zu codes", menu.CheatList().Entries().size());

  if (Settings::GetOn("resume", game.key) && Saves::Modified(Saves::StatePath(game.key, 0)))
  {
    if (LoadState(0))
      menu.Toast("Continuing where you left off");
  }

  GameAudio audio;
  audio.Start(Settings::GetInt("audio_buffer", game.key));
  audio.SetVolume(Settings::GetInt("volume", game.key));
  s_audio = &audio;

  Pads pads;
  pads.Open();
  // Tilt Sensor: DualShock 4 Motion - player 1's motion sensors on.
  bool motion_on = false;
  auto update_motion = [&]() {
    const int handle = pads.pads[0].handle;
    if (!IsHandheld(s_system) || handle < 0 || s_tilt.motion == motion_on)
      return;
    const int result = emusuni_pad_set_motion(handle, s_tilt.motion);
    motion_on = s_tilt.motion;
    s_tilt.calibrate = true;
    LogF("gb: DualShock 4 motion sensors %s (%#x)", motion_on ? "on" : "off", static_cast<unsigned>(result));
  };
  update_motion();
  // The emulation thread first on its core while a game runs: a background thread on the same core
  // (ParaLLEl-RDP's shader compiles, the covers) waits instead of stopping the game. The PS4's
  // priorities: lower is higher, 700 the default. The thread never spins on another one (it blocks).
  int old_priority = 0;
  const bool priority_set = scePthreadGetprio(scePthreadSelf(), &old_priority) == 0 &&
                            scePthreadSetprio(scePthreadSelf(), 480) == 0;
  LogF("game: emulation thread priority %d -> %s", old_priority, priority_set ? "480" : "unchanged");
  // N64 (CPU Cores: Fixed): the emulation thread on core 2 and ParaLLEl-RDP's threads on cores of
  // their own (port/ps4_n64.c): the threads that wait for each other every frame don't share one.
  uint64_t old_mask = 0;
  bool pinned = false;
  if (fixed_cores)
  {
    const int got = emusuni_get_affinity(scePthreadSelf(), &old_mask);
    const int set = got == 0 ? emusuni_set_affinity(scePthreadSelf(), 1ull << 2) : -1;
    pinned = got == 0 && set == 0;
    LogF("game: emulation thread on core 2: %s (cores before %#llx, %#x %#x); running on CPU %d",
         pinned ? "yes" : "no", static_cast<unsigned long long>(old_mask), static_cast<unsigned>(got),
         static_cast<unsigned>(set), sceKernelGetCurrentCpu());
  }
  Controls::Layout layout = Controls::LayoutFor(game.key, game.system);

  Stats stats;
  stats.pass_ms.reserve(4096);
  stats.run_ms.reserve(4096);
  ResetWindow(stats, display.GetFlipState());
  FramePacer pacer;
  VblankCheck vblank_check;
  const auto game_start = Clock::now();
  display.TakeWaitMs();  // the menu's waits aren't the game's
  auto last = Clock::now();
  double schedule = 0.0;  // frames owed
  int skip_counter = 0;
  float sram_timer = 0.0f;
  bool slow_last = false;
  Result result = Result::Menu;
  bool quit = false;
  uint32_t menu_pad_held = 0;
  float memory_log = 0.0f;
  float audio_log = 0.0f;  // play time since the sound's last health line
  while (!quit)
  {
    const auto now = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.25f);
    last = now;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

    pads.Read(dt);
    for (int i = 0; i < MAX_PLAYERS; i++)
      s_pad_handles[i] = pads.pads[i].handle;
    // Player 1's pad drives the menu (the screenshot tour presses its buttons too).
    const Tour::Step tour = Tour::Next(dt);
    if (tour.command == "quit")
      quit = true;
    const auto& p1 = pads.pads[0];
    // While the screenshot tour plays, the controllers are left out.
    const bool touring = Tour::Active();
    const uint32_t pad_held = p1.ok && !touring ? p1.data.buttons : 0;
    const uint32_t p1_pressed = (pad_held & ~menu_pad_held) | tour.buttons;
    const uint32_t p1_held = pad_held | tour.buttons;
    menu_pad_held = pad_held;
    constexpr uint32_t COMBO = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;

    if (!menu.IsOpen())
    {
      if ((p1_held & COMBO) == COMBO && (p1_pressed & COMBO))
      {
        StopRumble();
        menu.Open();
        audio.SetMuted(true);
        if (audio_log >= 1.0f)
          audio.LogHealth(audio_log);  // the play before the menu
        audio_log = 0.0f;
        SaveSram(false);
      }
    }
    else
    {
      const IngameMenu::Action action = menu.Poll(p1_pressed, p1_held, dt);
      switch (action)
      {
      case IngameMenu::Action::Save:
        if (SaveState(menu.Slot()))
          menu.Toast("Saved to slot " + std::to_string(menu.Slot()));
        else
          menu.Toast("Couldn't save the state");
        menu.ForgetPictures();
        break;
      case IngameMenu::Action::Load:
        if (LoadState(menu.Slot()))
        {
          menu.Toast("Loaded slot " + std::to_string(menu.Slot()));
          menu.Close();
        }
        else
        {
          menu.Toast("Slot " + std::to_string(menu.Slot()) + " can't be loaded");
        }
        break;
      case IngameMenu::Action::Reset:
        s_core->reset();
        menu.Close();
        menu.Toast("Game reset");
        break;
      case IngameMenu::Action::Quit:
        quit = true;
        break;
      default:
        break;
      }
      if (menu.TakeSettingsChanged())
      {
        BuildVariables();
        update_motion();
        LoadBezel(display, game);
        audio.SetVolume(Settings::GetInt("volume", game.key));
        const bool want_multitap = snes && Settings::GetOn("multitap", game.key);
        if (want_multitap != multitap)
        {
          multitap = want_multitap;
          s_core->set_controller_port_device(1, multitap ? DEVICE_MULTITAP : RETRO_DEVICE_JOYPAD);
        }
      }
      if (menu.TakeCheatsChanged())
        menu.CheatList().ApplyToCore(*s_core);
      if (!menu.IsOpen())
      {
        audio.Clear();
        audio.SetMuted(false);
        audio.LogHealth(0.0f);  // a new window: the time in the menu isn't the game's
        audio_log = 0.0f;
        layout = Controls::LayoutFor(game.key, game.system);
        s_tilt.calibrate = true;  // how the DualShock 4 is held now is "level"
        schedule = 0.0;
        pacer.Restart();  // the time the menu was open isn't caught up
        ResetWindow(stats, display.GetFlipState());
        stats.counted = -1;
      }
    }
    if (quit)
      break;

    // ---- Emulation ----
    const auto emu_start = Clock::now();
    // VideoOut now: the N64's pacing below, and the pacing line's count of the flips it showed.
    const Display::FlipState flip = display.GetFlipState();
    if (!menu.IsOpen())
      CountShownFlips(stats, flip.shown);
    if (!menu.IsOpen())
    {
      // Buttons of every player (the in-game menu combo isn't passed on).
      bool fast = false;
      for (int i = 0; i < MAX_PLAYERS; i++)
      {
        const auto& pad = pads.pads[i];
        s_buttons[i] = pad.ok && !touring ? Controls::PadButtons(layout, pad.data) : 0;
        if (s_system == SystemId::N64)
        {
          auto axis = [](uint8_t value) {
            return static_cast<int16_t>(std::clamp((static_cast<int>(value) - 128) * 258, -32768, 32767));
          };
          const bool on = pad.ok && !touring;
          s_analog[i][0] = {on ? axis(pad.data.leftStick.x) : int16_t(0), on ? axis(pad.data.leftStick.y) : int16_t(0)};
          s_analog[i][1] = {on ? axis(pad.data.rightStick.x) : int16_t(0), on ? axis(pad.data.rightStick.y) : int16_t(0)};
        }
        if (i == 0 && pad.ok && !touring && layout.fast_forward && (pad.data.buttons & layout.fast_forward))
          fast = true;
        if (i == 0 && IsHandheld(s_system))
        {
          // The cartridge's tilt sensor: the right stick (a dead zone around the centre), the
          // controller's own sensors.
          const bool on = pad.ok && !touring;
          auto axis = [](uint8_t value) {
            const float v = (static_cast<int>(value) - 128) / 127.0f;
            return std::abs(v) < 0.12f ? 0.0f : std::clamp((v - std::copysign(0.12f, v)) / 0.88f, -1.0f, 1.0f);
          };
          s_tilt.stick_x = on ? axis(pad.data.rightStick.x) : 0.0f;
          s_tilt.stick_y = on ? axis(pad.data.rightStick.y) : 0.0f;
          if (on && s_tilt.motion)
          {
            s_tilt.accel = pad.data.acell;
            s_tilt.turn = pad.data.vel.z;
            if (s_tilt.calibrate)
            {
              s_tilt.rest = pad.data.acell;
              s_tilt.calibrate = false;
            }
          }
          else
          {
            s_tilt.accel = s_tilt.rest;
            s_tilt.turn = 0.0f;
          }
        }
      }
      // Frames per TV refresh: the game's own rate (60.1 FPS is run at the TV's 60, the sound
      // follows), times the speed setting or fast forward.
      const std::string limit = Settings::Get("fps_limit", game.key);
      double fps = s_game_fps;
      if (limit == "60")
        fps = 60.0;
      else if (limit == "50")
        fps = 50.0;
      const double hz = display.RefreshHz();
      double ratio = std::abs(fps / hz - 1.0) < 0.012 ? 1.0 : fps / hz;
      ratio *= Settings::GetInt("speed", game.key) / 100.0;
      const std::string ff = Settings::Get("fast_forward", game.key);
      const bool forward = fast && ff != "off";
      if (forward)
        ratio = std::atoi(ff.c_str());
      if (limit == "0")
        ratio = std::max(ratio, 4.0);
      // N64 with V-Sync (N64 Frame Pacing: Even): the frames due by the vblank this pass's flip
      // will be shown at - VideoOut's count + the flips queued + 1 (src/Pacing.h). By the clock
      // (By the Clock, V-Sync off, fast forward, a vblank count that can't be trusted): as 00.27,
      // the time the last pass took (0.5 to 2 refreshes: the 0.5 credits a short pass with time it
      // didn't take - 104-108% in Mario Kart 64 - but taking it out alone makes the hitches worse in
      // the model, so the clock's way stays as it was, for comparison).
      int frames = 0;
      bool paced = s_system == SystemId::N64 && !forward && ratio <= 1.25 && flip.vblanks >= 0 &&
                   Settings::Get("vsync", game.key) != "off" && Settings::Get("frame_pacing", game.key) != "clock";
      if (paced)
      {
        // (emu_start: the clock read next to VideoOut's count - not the pass's start, which can be
        // a bezel's PNG earlier.)
        bool failed = false;
        paced = vblank_check.Ok(flip.vblanks, std::chrono::duration<double>(emu_start - game_start).count(), hz,
                                &failed);
        if (failed)
        {
          LogF("pacing: VideoOut counted %.0f vblanks where the clock says %.0f: paced by the clock",
               vblank_check.Counted(), vblank_check.Expected());
        }
      }
      if (paced)
      {
        frames = pacer.Frames(flip.vblanks + flip.pending + 1, ratio, &stats.let_go);
        schedule = 0.0;
      }
      else
      {
        pacer.Restart();
        if (s_system == SystemId::N64)
          schedule += ratio * std::clamp(static_cast<double>(dt) * hz, 0.5, 2.0);
        else
          schedule += ratio;
        frames = static_cast<int>(schedule);
        schedule -= frames;
        frames = std::min(frames, 10);
      }
      stats.per_pass[static_cast<size_t>(std::min(frames, 3))]++;
      audio.SetMuted(fast && ff != "off");
      const std::string skip = Settings::Get("frameskip", game.key);
      for (int f = 0; f < frames; f++)
      {
        // Only the last frame of this refresh is drawn; frame skip leaves out more.
        bool render = f == frames - 1;
        if (render && skip == "auto")
          render = !slow_last || (skip_counter++ % 2) == 0;
        else if (render && (skip == "1" || skip == "2"))
          render = (skip_counter++ % (std::atoi(skip.c_str()) + 1)) == 0;
        s_render_video = render;
        const auto run_start = Clock::now();
        s_core->run();
        const float run_ms = std::chrono::duration<float, std::milli>(Clock::now() - run_start).count();
        stats.core_ms += run_ms;
        stats.core_runs++;
        stats.core_max = std::max(stats.core_max, run_ms);
        stats.emulated++;
        stats.emulated10++;
        stats.total++;
        stats.run_ms.push_back(run_ms);
      }
      // The core's messages (the Famicom Disk System's disk side, a missing BIOS).
      if (!s_message.empty())
      {
        menu.Toast(s_message, 3.0f);
        s_message.clear();
      }
      // Battery saves: written a few seconds after the game changes them.
      sram_timer += dt;
      if (sram_timer > 3.0f)
      {
        sram_timer = 0.0f;
        SaveSram(false);
      }
    }
    const float emu_ms = std::chrono::duration<float, std::milli>(Clock::now() - emu_start).count();
    slow_last = emu_ms > 14.0f;
    if (s_frame_new && s_frame)
    {
      UploadPicture(display, game.key);
      s_frame_new = false;
    }

    // ---- Stats ----
    memory_log -= dt;
    if (memory_log <= 0.0f)
    {
      memory_log = 60.0f;
      LogMemory("game");
      if (s_scale_frames > 0)
      {
        LogF("upscale: %s, %.1f ms per picture", s_scale_name.c_str(), s_scale_ms / s_scale_frames);
        s_scale_ms = 0.0;
        s_scale_frames = 0;
      }
    }
    // The sound's health every 10 s of play (what the core sent and what was played, the buffer
    // running dry, AudioOut's errors): a game without sound says why.
    if (!menu.IsOpen())
    {
      audio_log += dt;
      if (audio_log >= 10.0f)
      {
        audio.LogHealth(audio_log);
        audio_log = 0.0f;
      }
    }
    stats.frames++;
    stats.fps_time += dt;
    stats.frame_ms[stats.at] = dt * 1000.0f;
    if (!menu.IsOpen())
    {
      stats.frames10++;
      stats.time10 += dt;
      stats.pass_ms.push_back(dt * 1000.0f);
    }
    stats.at = (stats.at + 1) % stats.frame_ms.size();
    if (stats.fps_time >= 1.0f)
    {
      // The counter: the pictures the TV got per second (smoothness) and the game's speed.
      stats.fps = stats.frames / stats.fps_time;
      stats.speed = static_cast<float>(stats.emulated / stats.fps_time / s_game_fps * 100.0);
      stats.frames = stats.emulated = 0;
      stats.fps_time = 0.0f;
      // The N64's speed in the log every 10 seconds of play (how well the PS4 keeps up).
      // (Also for SNES/NES games drawn through a GPU filter: how busy the filter keeps the GPU.)
      if ((s_system == SystemId::N64 || GpuMode(game.key) != GpuFilter::Mode::Off) && !menu.IsOpen() &&
          stats.time10 >= 10.0f)
      {
        // The core's time per frame against the game's frame (20 ms for PAL): above it, the PS4's
        // CPU (or, with Accurate sync, the GPU) can't keep up.
        // The GPU: busy since the last line, and the memory its buffers take.
        static uint64_t last_busy = 0, last_span = 0;
        uint64_t busy = 0, span = 0, submitted = 0, completed = 0, bo_bytes = 0, bo_count = 0;
        ac_ps4_get_gpu_busy(&busy, &span);
        ac_ps4_get_stats(&submitted, &completed, &bo_bytes, &bo_count);
        const double gpu_busy = span > last_span ? 100.0 * static_cast<double>(busy - last_busy) /
                                                       static_cast<double>(span - last_span) : 0.0;
        last_busy = busy;
        last_span = span;
        LogF("speed: %.1f FPS shown, %.0f%% of the game's speed; the core %.1f ms per frame (most %.1f) of %.1f; "
             "drawing %.1f ms (%.1f of it waiting); CPU %d; GPU busy %.0f%%, %llu MiB",
             stats.time10 > 0.0f ? stats.frames10 / stats.time10 : 0.0f,
             stats.time10 > 0.0f ? stats.emulated10 / stats.time10 / s_game_fps * 100.0 : 0.0,
             stats.core_runs ? stats.core_ms / stats.core_runs : 0.0, stats.core_max, 1000.0 / s_game_fps,
             stats.draws ? stats.draw_ms / stats.draws : 0.0, stats.draws ? stats.draw_wait_ms / stats.draws : 0.0,
             sceKernelGetCurrentCpu(), gpu_busy, static_cast<unsigned long long>(bo_bytes >> 20));
        // The GPU filter's own GPU time over the same 10 seconds (timestamps around its passes).
        char filter_time[768];
        if (GpuFilter::TakeGpuTime(filter_time, sizeof(filter_time)))
          LogF("%s", filter_time);
        LogPacing(stats, flip);
        ResetWindow(stats, flip);
      }
    }

    // ---- Drawing ----
    const bool vsync = Settings::Get("vsync", game.key) != "off";
    display.SetVsync(vsync);
    const auto draw_start = Clock::now();
    display.BeginFrame();
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    DrawPicture(dl, display, game.key);
    DrawBezel(dl);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    DrawStats(fg, bold, stats, Settings::GetOn("show_fps", game.key),
              Settings::GetOn("perf_overlay", game.key), audio.Fill());
    menu.Draw(fg, dt);
    ImGui::Render();
    stats.flip_frame[static_cast<size_t>(display.NextFlip() % 8)] = stats.total;
    display.EndFrame();
    const double draw_wait = display.TakeWaitMs();
    if (!menu.IsOpen())
    {
      stats.draw_wait_ms += draw_wait;
      stats.draw_ms += std::chrono::duration<double, std::milli>(Clock::now() - draw_start).count();
      stats.draws++;
    }
    if (!tour.shot.empty())
      Tour::SaveShot(display, tour.shot);
    // V-Sync off: the flips don't wait for the TV; keep the game's pace by the clock.
    if (!vsync)
    {
      const float spent = std::chrono::duration<float>(Clock::now() - now).count();
      const float frame = 1.0f / 59.94f;
      if (spent < frame)
        sceKernelUsleep(static_cast<uint32_t>((frame - spent) * 1e6f));
    }
  }

  // ---- Back to the menu ----
  if (priority_set)
    scePthreadSetprio(scePthreadSelf(), old_priority);
  if (pinned)
    emusuni_set_affinity(scePthreadSelf(), old_mask);
  StopRumble();
  if (motion_on && pads.pads[0].handle >= 0)
    emusuni_pad_set_motion(pads.pads[0].handle, false);
  display.SetVsync(true);
  if (Settings::GetOn("resume", game.key))
    SaveState(0);
  SaveSram(true);
  if (audio_log >= 1.0f)
    audio.LogHealth(audio_log);
  s_audio = nullptr;
  audio.Stop();
  Upscaler::Stop();
  std::vector<uint16_t>().swap(s_scaled);
  pads.Close();
  menu.ForgetPictures();
  FreeBezel(display);
  // As RetroArch: the core's Vulkan objects go first (its RDRAM is still there), then the game and
  // the core. The frames that show the core's images are finished before.
  if (s_hw_vulkan && s_hw_render.context_destroy)
  {
    display.WaitIdle();
    s_hw_render.context_destroy();
  }
  s_core->unload_game();
  s_core->deinit();
  s_hw_vulkan = false;
  s_hw_image_set = false;
  LogLine("game: ended");
  return result;
}
}  // namespace Emulator
