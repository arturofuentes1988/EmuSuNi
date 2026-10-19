// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Emulator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <pthread.h>

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
// The disc changer with the first disc and the discs' paths (the same newer libretro.h; SwanStation and
// Flycast give it when the frontend says version 1).
#ifndef RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE
#define RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION 57
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE 58
typedef bool(RETRO_CALLCONV* retro_set_initial_image_t)(unsigned index, const char* path);
typedef bool(RETRO_CALLCONV* retro_get_image_path_t)(unsigned index, char* s, size_t len);
typedef bool(RETRO_CALLCONV* retro_get_image_label_t)(unsigned index, char* s, size_t len);
struct retro_disk_control_ext_callback
{
  retro_set_eject_state_t set_eject_state;
  retro_get_eject_state_t get_eject_state;
  retro_get_image_index_t get_image_index;
  retro_set_image_index_t set_image_index;
  retro_get_num_images_t get_num_images;
  retro_replace_image_index_t replace_image_index;
  retro_add_image_index_t add_image_index;
  retro_set_initial_image_t set_initial_image;
  retro_get_image_path_t get_image_path;
  retro_get_image_label_t get_image_label;
};
#endif

// ---- State the libretro callbacks use (one game at a time). ----
const CoreApi* s_core = &Cores::SNES9X;  // the running game's core
SystemId s_system = SystemId::SNES;
std::string s_game_id;  // the game's key
std::string s_system_dir, s_save_dir;
std::map<std::string, std::string> s_variables;  // the core's option values for GET_VARIABLE
bool s_variables_changed = false;
// The options the core declared (SET_VARIABLES): Flycast stops (verify, DEBUGBREAK) at an option it is
// given but never declared, so the Dreamcast is given only its own; the others' are logged once.
std::set<std::string> s_core_keys, s_undeclared_logged;
// The core's disc changer (SET_DISK_CONTROL_(EXT_)INTERFACE: SwanStation's, Flycast's): the in-game
// menu's Disc. The PlayStation's and the Dreamcast's only (FCEUmm keeps the Famicom Disk System's sides).
retro_disk_control_ext_callback s_disk = {};
bool s_disk_set = false;

bool TakesDiscs()
{
  return s_system == SystemId::PS1 || s_system == SystemId::DC;
}

// The discs the core can change between, and the one in it (from 0).
int DiscCount()
{
  return s_disk_set && s_disk.get_num_images ? static_cast<int>(s_disk.get_num_images()) : 0;
}

int DiscIn()
{
  return s_disk_set && s_disk.get_image_index ? static_cast<int>(s_disk.get_image_index()) : 0;
}
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
// The DS's screens as shown (DS Screens; R3 swaps to the other view): the core gives them side by
// side (or one above the other); the Big Top Screen is placed here, on the GPU (DrawPicture).
std::string s_nds_chosen;
bool s_nds_alt = false, s_nds_r3 = false;
// The left stick's furthest push since the last speed line (the log: the PSP's stick, 00.40).
int s_stick_max = 0;
// The PSP's resolution and multisampling as the game started: PPSSPP's Vulkan picture keeps the size
// it had then (its swapchain), so a change while it runs only zoomed in.
int s_psp_scale = 0;
std::string s_psp_msaa;
// PSP Frame Rate Auto: God of War at 30 FPS (PPSSPP's hack) once its core couldn't keep up, until the
// game ends.
bool s_psp_30fps_auto = false;
bool s_shutdown = false;  // the core asked to stop (RETRO_ENVIRONMENT_SHUTDOWN)
// Flycast's HLE BIOS said "Reboot to BIOS" (a game went back to the BIOS, which it can't show): Run()
// starts the console again; its RETRO_ENVIRONMENT_SHUTDOWN then isn't the game's end, but says the
// emulation stopped (s_dc_stopped: no waiting before the restart then).
bool s_dc_reboot = false, s_dc_stopped = false;
retro_hw_render_interface_vulkan s_vulkan = {};
retro_vulkan_image s_hw_image = {};
bool s_hw_image_set = false;
bool s_frame_hw = false;  // the newest frame is the core's image
// The analog sticks per player (RETRO_DEVICE_ANALOG): left / right, x / y.
std::array<std::array<std::array<int16_t, 2>, 2>, MAX_PLAYERS> s_analog{};
// The Dreamcast's analog triggers (L, R: 0..0x7FFF), from the DualShock 4's L2 / R2 pressure.
std::array<std::array<int16_t, 2>, MAX_PLAYERS> s_triggers{};
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

// A touch screen (RETRO_DEVICE_POINTER): where player 1's finger is on the touchpad, in libretro's
// -0x7FFF..0x7FFF over the picture, and whether it is down.
struct Pointer
{
  int16_t x = 0, y = 0;
  bool pressed = false;
};
Pointer s_pointer;
// The core's pixels are XRGB8888 (SET_PIXEL_FORMAT): each frame is made RGB565 in s_converted.
bool s_xrgb8888 = false;
std::vector<uint16_t> s_converted;
// The newest picture (valid until the next retro_run).
const void* s_frame = nullptr;
unsigned s_frame_w = 0, s_frame_h = 0;
size_t s_frame_pitch = 0;
bool s_frame_new = false;
// While the drawing runs beside the emulation (DrawWorker: the DS), the picture the core hands over
// waits here: the drawing reads the one before. Taken over when the drawing is done.
bool s_defer_frame = false;
struct PendingFrame
{
  bool hw = false, fresh = false;
  const void* frame = nullptr;
  unsigned w = 0, h = 0;
  size_t pitch = 0;
  // The core's image (set_image) while the drawing runs beside the emulation (the PSP).
  bool image_set = false;
  retro_vulkan_image image = {};
  std::vector<VkSemaphore> semaphores;
};
PendingFrame s_pending;

void TakePendingFrame()
{
  if (s_pending.image_set)
  {
    s_hw_image = s_pending.image;
    s_hw_image_set = true;
    s_pending.image_set = false;
    if (!s_pending.semaphores.empty() && s_display)
      s_display->WaitForSemaphores(static_cast<uint32_t>(s_pending.semaphores.size()), s_pending.semaphores.data());
    s_pending.semaphores.clear();
  }
  if (!s_pending.fresh)
    return;
  s_frame_hw = s_pending.hw;
  s_frame = s_pending.frame;
  s_frame_w = s_pending.w;
  s_frame_h = s_pending.h;
  s_frame_pitch = s_pending.pitch;
  s_frame_new = true;
  s_pending.fresh = false;
}
// Resolution: the picture made bigger, and the part of the game texture the picture fills.
std::vector<uint16_t> s_scaled;
uint32_t s_texture_w = 0, s_texture_h = 0;
bool s_texture_scaled = false;
double s_scale_ms = 0.0;  // time spent making pictures bigger (for the log)
int s_scale_frames = 0;
std::string s_scale_name;
GameAudio* s_audio = nullptr;
double s_sample_rate = 32040.0;
// The game's speed while its core can't keep up (one frame a pass, below 100%): the sound is played
// that much slower, not with gaps.
double s_audio_speed = 1.0;

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

// SwanStation's options (the PlayStation): drawn by its Vulkan renderer on the app's device, the
// recompiler, the memory card as the battery save (RETRO_MEMORY_SAVE_RAM), the disc read ahead by
// a thread of its own, its shaders compiled as they are needed.
void BuildPs1Variables(const std::string& g)
{
  const std::string p = "swanstation_";
  s_variables[p + "GPU_Renderer"] = "Vulkan";
  s_variables[p + "CPU_ExecutionMode"] = "Recompiler";
  s_variables[p + "MemoryCards_Card1Type"] = "Libretro";
  s_variables[p + "MemoryCards_Card2Type"] = "None";
  s_variables[p + "CDROM_ReadThread"] = "true";
  s_variables[p + "CDROM_ReadaheadSectors"] = "16";
  s_variables[p + "GPU_ShaderPrecompile"] = "Lazy";
  s_variables[p + "Display_ShowOSDMessages"] = "false";
  s_variables[p + "Logging_LogLevel"] = "Info";
  // Any 512 KiB BIOS in the system folder is found by its contents; these are the usual names.
  s_variables[p + "BIOS_PathNTSCU"] = "scph5501.bin";
  s_variables[p + "BIOS_PathNTSCJ"] = "scph5500.bin";
  s_variables[p + "BIOS_PathPAL"] = "scph5502.bin";
  s_variables[p + "BIOS_PatchFastBoot"] = Settings::Get("ps1_boot", g) == "skip" ? "true" : "false";
  s_variables[p + "GPU_ResolutionScale"] = std::to_string(std::clamp(Settings::GetInt("ps1_resolution", g), 1, 4));
  s_variables[p + "GPU_PGXPEnable"] = Settings::Get("ps1_pgxp", g) == "on" ? "true" : "false";
  s_variables[p + "GPU_TextureFilter"] = Settings::Get("ps1_texture_filter", g);
  const bool true_color = Settings::Get("ps1_colors", g) == "true";
  s_variables[p + "GPU_TrueColor"] = true_color ? "true" : "false";
  s_variables[p + "GPU_ScaledDithering"] = "true";
  s_variables[p + "GPU_MSAA"] = Settings::Get("ps1_antialiasing", g);
  const bool wide = Settings::Get("ps1_widescreen", g) == "on";
  s_variables[p + "GPU_WidescreenHack"] = wide ? "true" : "false";
  s_variables[p + "Display_AspectRatio"] = wide ? "16:9" : "4:3";
  s_variables[p + "Display_CropMode"] = "Borders";
  const std::string region = Settings::Get("region", g);
  s_variables[p + "Console_Region"] = region == "ntsc" ? "NTSC-U" : region == "pal" ? "PAL" : "Auto";
  const std::string speed = Settings::Get("ps1_cd_speed", g);
  s_variables[p + "CDROM_ReadSpeedup"] = speed;
  s_variables[p + "CDROM_SeekSpeedup"] = speed;
  s_variables[p + "Controller1_ForceAnalog"] = Settings::Get("ps1_controller", g) == "analog" ? "true" : "false";
  s_variables[p + "Controller1_AnalogDPadInDigitalMode"] = "true";
}

// "side", "big" or "stack": DS Screens, or the other view after R3.
std::string NdsView()
{
  if (!s_nds_alt)
    return s_nds_chosen;
  return s_nds_chosen == "side" ? "big" : "side";
}

// The Big Top Screen is drawn now (from the core's side-by-side picture).
bool NdsBig()
{
  return s_system == SystemId::NDS && s_frame_w == 512 && s_frame_h == 192 && NdsView() == "big";
}

// melonDS DS's options: every one of its 78 is answered (an unanswered one doesn't get the default
// it declares). The JIT, the threaded 3D (its thread on core 3, port/ps4_n64.c), the built-in BIOS
// and firmware unless all three of the DS's own are in the system folder, the touchpad as the touch
// screen, two screen layouts R3 swaps between, no Wi-Fi.
void BuildNdsVariables(const std::string& g)
{
  const std::string p = "melonds_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("console_mode", "ds");
  set("boot_mode", "direct");
  // The DS's BIOS and firmware (bios7.bin, bios9.bin, firmware.bin) in system/ or system/melonDS DS/:
  // encrypted ROMs need them; most dumps don't.
  std::string firmware;
  for (const std::string dir : {std::string(), std::string("melonDS DS/")})
  {
    if (firmware.empty() && FileExists(s_system_dir + "/" + dir + "bios7.bin") &&
        FileExists(s_system_dir + "/" + dir + "bios9.bin") && FileExists(s_system_dir + "/" + dir + "firmware.bin"))
      firmware = dir + "firmware.bin";
  }
  set("sysfile_mode", firmware.empty() ? "builtin" : "native");
  set("firmware_nds_path", firmware.empty() ? "/notfound" : firmware);
  set("firmware_dsi_path", "/notfound");
  set("dsi_nand_path", "/notfound");
  set("jit_enable", "enabled");
  set("jit_block_size", "32");
  set("jit_branch_optimisations", "enabled");
  set("jit_literal_optimisations", "enabled");
  set("jit_fast_memory", "disabled");
  set("threaded_renderer", "enabled");
  // DS Screens: the core gives the screens side by side, or one above the other; the app shows them
  // (the Big Top Screen made on the GPU from the side-by-side picture: melonDS DS scaling it on the
  // CPU cost ~3.6 ms a frame on the PS4) and R3 swaps to the other view (NdsView).
  const std::string chosen = Settings::Get("nds_layout", g);
  if (chosen != s_nds_chosen)
  {
    s_nds_chosen = chosen;
    s_nds_alt = false;
  }
  set("number_of_screen_layouts", "1");
  set("screen_layout1", NdsView() == "stack" ? "top-bottom" : "left-right");
  set("screen_layout2", "left-right");
  const char* rest[] = {"largescreen-top", "largescreen-bottom", "hybrid-top", "hybrid-bottom", "top", "bottom"};
  for (int i = 3; i <= 8; i++)
    set(("screen_layout" + std::to_string(i)).c_str(), rest[i - 3]);
  set("hybrid_ratio", "3");  // Big Top Screen: 1024x576, 16:9
  set("hybrid_small_screen", "one");
  set("opengl_filtering", "linear");
  set("screen_gap", "0");
  set("secondary_screen_scale", "100");
  set("secondary_screen_filtering", "linear");
  // The touch screen: the touchpad only (the joystick cursor's R2 is fast forward here).
  set("touch_mode", "touch");
  set("show_cursor", "timeout");
  set("cursor_timeout", "3");
  set("joystick_cursor_deadzone", "15");
  set("joystick_cursor_maxspeed", "3");
  set("joystick_cursor_response", "200");
  set("joystick_cursor_speedup", "200");
  // No microphone: L3 blows into it.
  set("mic_input", "blow");
  set("mic_input_active", "hold");
  set("audio_bitdepth", "auto");
  set("audio_interpolation", "disabled");
  // The firmware: in the menus' language when the DS has it.
  const std::string language = Settings::Get("language");
  set("firmware_language",
      language == "es" || language == "fr" || language == "de" || language == "it" || language == "ja" ? language : "en");
  set("firmware_username", "EmuSuNi");
  set("firmware_wfc_dns", "default");
  set("firmware_favorite_color", "default");
  set("firmware_birth_month", "default");
  set("firmware_birth_day", "default");
  set("firmware_enable_alarm", "default");
  set("firmware_alarm_hour", "default");
  set("firmware_alarm_minute", "default");
  set("mac_address_mode", "firmware");
  set("slot2_device", "auto");
  set("rumble_intensity", "65535");
  set("rumble_type", "both");
  set("solar_sensor_host_sensor", "disabled");
  set("dsi_sdcard", "disabled");
  set("dsi_sdcard_readonly", "disabled");
  set("dsi_sdcard_sync_sdcard_to_host", "disabled");
  set("homebrew_sdcard", "enabled");
  set("homebrew_readonly", "disabled");
  set("homebrew_sync_sdcard_to_host", "disabled");
  set("battery_update_interval", "15");
  set("ds_battery_ok_threshold", "20");
  // The clock: the PS4's (its time zone is TZ, main.cpp).
  set("start_time_mode", "real");
  set("start_time_relative_year_offset", "0");
  set("start_time_relative_day_offset", "0");
  set("start_time_relative_hour_offset", "0");
  set("start_time_relative_minute_offset", "0");
  set("start_time_absolute_year", "2004");
  set("start_time_absolute_month", "1");
  set("start_time_absolute_day", "1");
  set("start_time_absolute_hour", "0");
  set("start_time_absolute_minute", "0");
  set("start_time_absolute_second", "0");
  // Its on-screen messages need a message interface the app doesn't have.
  for (const char* key : {"show_unsupported_features", "show_bios_warnings", "show_current_layout", "show_mic_state",
                          "show_camera_state", "show_lid_state", "show_sensor_reading"})
    set(key, "disabled");
}

// PPSSPP's options (the PSP): Vulkan on the app's device, the JIT (the IR interpreter if the PS4 won't
// make its code executable), no fast memory (scripts/build-psp-core.sh), one picture per TV refresh,
// a PSP-2000 with its memory stick, no network. Every option the core reads is answered.
void BuildPspVariables(const std::string& g)
{
  const std::string p = "ppsspp_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("backend", "vulkan");
  set("cpu_core", "JIT");
  // Fast memory: its fault handler (build-psp-core.sh: FreeBSD's signal values written out) skips the
  // JIT's stray accesses and hands the rest to the crash log.
  set("fast_memory", "enabled");
  set("cache_iso", "disabled");
  set("frame_duplication", "enabled");
  set("detect_vsync_swap_interval", "disabled");
  set("psp_model", "psp_2000_3000");
  set("ignore_bad_memory_access", "enabled");
  set("io_timing_method", "Fast");
  set("memstick_inserted", "enabled");
  set("inflight_frames", "Up to 2");
  set("cropto16x9", "enabled");
  set("software_rendering", "disabled");
  set("gpu_hardware_transform", "enabled");
  set("software_skinning", "disabled");  // hardware skinning: faster with the render thread
  set("hardware_tesselation", "disabled");
  set("spline_quality", "High");
  set("skip_buffer_effects", "disabled");
  set("skip_gpu_readbacks", "disabled");
  set("lazy_texture_caching", "disabled");
  set("disable_range_culling", "disabled");
  set("lower_resolution_for_effects", "disabled");
  // CPU texture scaling is too slow for the PS4's CPU: the GPU's texture shaders instead.
  set("texture_scaling_level", "disabled");
  set("texture_scaling_type", "xbrz");
  set("texture_deposterize", "disabled");
  set("texture_replacement", "disabled");
  set("texture_filtering", "Auto");
  set("smart_2d_texture_filtering", "disabled");
  // PSP Resolution, Texture Sharpness, Texture Upscaling, Anti-Aliasing (the size and the
  // multisampling as the game started: s_psp_scale).
  if (s_psp_scale == 0)
  {
    s_psp_scale = std::clamp(Settings::GetInt("psp_resolution", g), 1, 4);
    const std::string level = Settings::Get("psp_antialiasing", g);
    s_psp_msaa = level == "2" || level == "4" || level == "8" ? "x" + level : "Disabled";  // (its spelling)
  }
  const int scale = s_psp_scale;
  set("internal_resolution", std::to_string(480 * scale) + "x" + std::to_string(272 * scale));
  const std::string aniso = Settings::Get("psp_anisotropy", g);
  set("texture_anisotropic_filtering", aniso == "4" || aniso == "8" || aniso == "16" ? aniso + "x" : "disabled");
  const std::string shader = Settings::Get("psp_texture_shader", g);
  set("texture_shader", shader == "2xBRZ" || shader == "4xBRZ" || shader == "MMPX" ? shader : "disabled");
  set("mulitsample_level", s_psp_msaa);
  // PSP Frame Rate: PPSSPP's 30 FPS hack, for the God of War games only (their compat flag; build-psp-
  // core.sh): half the emulation and GPU work, the game's speed unchanged.
  const std::string rate = Settings::Get("psp_frame_rate", g);
  set("emusuni_30fps", rate == "30" || (rate == "auto" && s_psp_30fps_auto) ? "enabled" : "disabled");
  // Frame Skip: the core's own (it leaves out the GPU's work, not the game's).
  const std::string skip = Settings::Get("frameskip", g);
  set("frameskip", skip == "1" || skip == "2" ? skip : skip == "auto" ? "1" : "disabled");
  set("frameskiptype", "Number of frames");
  set("auto_frameskip", skip == "auto" ? "enabled" : "disabled");
  set("locked_cpu_speed", "disabled");
  set("force_lag_sync", "disabled");
  set("cheats", "enabled");  // its CWCheat engine: the codes come through retro_cheat_set
  // The games' language: the menus' one.
  const std::string language = Settings::Get("language");
  static const std::pair<const char*, const char*> LANGUAGES[] = {
      {"es", "Spanish"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"},
      {"ja", "Japanese"}, {"ru", "Russian"}, {"ko", "Korean"}, {"nl", "Dutch"}};
  std::string psp_language = "English";
  for (const auto& [code, name] : LANGUAGES)
  {
    if (language == code)
      psp_language = name;
  }
  set("language", psp_language);
  set("button_preference", "Cross");
  set("analog_deadzone", "0.15");
  set("analog_sensitivity", "1.00");
  set("analog_is_circular", "disabled");
  // No network: Wi-Fi off, a fixed MAC address (else the core asks to make one up).
  set("enable_wlan", "disabled");
  set("enable_upnp", "disabled");
  set("wlan_channel", "Auto");
  for (int i = 1; i <= 12; i++)
  {
    char key[32];
    snprintf(key, sizeof(key), "change_mac_address%02d", i);
    set(key, "0");
  }
}

// Flycast's options (the Dreamcast): its recompilers, threaded rendering (SH4, ARM7 and AICA on its
// "Flycast-emu" thread, the renderer on retro_run's), Vulkan on the app's device, no GPU readbacks,
// no network. Every option it reads is answered.
void BuildDcVariables(const std::string& g)
{
  const std::string p = "reicast_";
  auto set = [&](const std::string& key, const std::string& value) { s_variables[p + key] = value; };
  set("region", "Default");     // the disc's (USA, else Europe, else Japan)
  set("broadcast", "NTSC");     // 60 Hz; Flycast picks PAL or NTSC itself for the few that need it
  set("cable_type", "VGA");     // 480p; Flycast falls back to composite for the games without VGA
  set("hle_bios", "disabled");  // the real BIOS if system/dc/dc_boot.bin is there, else its HLE BIOS
  const std::string l = Settings::Get("language");
  set("language", l == "ja" ? "Japanese" : l == "de" ? "German" : l == "fr" ? "French" : l == "es" ? "Spanish" :
                  l == "it" ? "Italian" : "English");
  set("dc_32mb_mod", "disabled");
  set("sh4clock", "200");
  set("gdrom_fast_loading", "disabled");
  set("enable_dsp", "enabled");
  set("threaded_rendering", "enabled");
  set("delay_frame_swapping", "enabled");
  set("detect_vsync_swap_interval", "disabled");
  // The Dreamcast plays at Low whatever the Quality - 640x480, 4x texture sharpness, transparency per
  // triangle, no GPU filter (GpuMode) - its settings hidden (Settings.cpp): on the PS4 the higher levels
  // slowed the games to a crawl (Crazy Taxi at 3x: 0.6 pictures a second, 00.52) and per-pixel sorting
  // drew wrong colours (00.49).
  set("internal_resolution", "640x480");
  set("anisotropic_filtering", "4");
  set("alpha_sorting", "per-triangle (normal)");
  set("oit_abuffer_size", "512MB");
  set("oit_layers", "32");
  set("texture_filtering", "0");
  set("mipmapping", "enabled");
  set("fog", "enabled");
  set("volume_modifier_enable", "enabled");
  set("native_depth_interpolation", "enabled");  // AMD GPUs (Flycast's own default for the Xbox)
  set("fix_upscale_bleeding_edge", "enabled");
  set("pvr2_filtering", "disabled");
  set("texupscale", "1");
  set("texupscale_max_filtered_texture_size", "256");
  // No GPU->CPU readbacks (each one drains the GPU: the PSP's stutters).
  set("enable_rttb", "disabled");
  set("emulate_framebuffer", "disabled");
  // Frame Skip: Flycast's own (it never asks GET_AUDIO_VIDEO_ENABLE).
  const std::string skip = Settings::Get("frameskip", g);
  set("frame_skipping", skip == "1" || skip == "2" ? skip : "disabled");
  set("auto_skip_frame", skip == "auto" ? "some" : "disabled");
  set("widescreen_cheats", "disabled");
  set("widescreen_hack", "disabled");
  set("screen_rotation", "horizontal");
  // (Not dump_unique_textures: Flycast 2.7 reads it but never declares it, and stops at an option it
  // is given without its declaration - both games crashed at their start in 00.48.)
  for (const char* key : {"custom_textures", "preload_custom_textures", "dump_textures", "dump_replaced_textures"})
    set(key, "disabled");
  set("analog_stick_deadzone", "15%");
  set("trigger_deadzone", "0%");
  set("digital_triggers", "disabled");
  for (int i = 1; i <= 4; i++)
  {
    const std::string n = std::to_string(i);
    set("device_port" + n + "_slot1", "VMU");
    set("device_port" + n + "_slot2", "Purupuru");  // the Jump Pack: rumble
    set("vmu" + n + "_screen_display", "disabled");
    set("vmu" + n + "_screen_position", "Upper Left");
    set("vmu" + n + "_screen_size_mult", "1x");
    set("vmu" + n + "_pixel_on_color", "DEFAULT_ON 00");
    set("vmu" + n + "_pixel_off_color", "DEFAULT_OFF 01");
    set("vmu" + n + "_screen_opacity", "100%");
    set("lightgun" + n + "_crosshair", "disabled");
  }
  set("per_content_vmus", "VMU A1");  // each game's VMU A1 in saves/dc; B1-D1 shared in system/dc
  set("vmu_sound", "disabled");
  set("linked_vmu_storage", "disabled");
  set("lightgun_crosshair_size_scaling", "100%");
  set("show_vmu_screen_settings", "disabled");
  set("show_lightgun_settings", "disabled");
  set("emulate_bba", "disabled");
  set("upnp", "disabled");
  set("dcnet", "disabled");
  set("network_output", "disabled");
  set("allow_service_buttons", "disabled");  // (NAOMI only)
  set("force_freeplay", "enabled");
  set("coin_limit", "0");
}

// The DS's touch screen in the frame melonDS DS composed (its layouts, by the frame's size): x, y,
// width, height in the frame's pixels.
std::array<float, 4> NdsTouchScreen(unsigned w, unsigned h)
{
  if (w == 512 && h == 192)
    return {256, 0, 256, 192};  // side by side: on the right
  if (w == 256 && h >= 384)
    return {0, static_cast<float>(h - 192), 256, 192};  // one above the other: at the bottom
  if (w == 1024 && h == 576)
    return {768, 192, 256, 192};  // big top screen (3x): the small one at the right, in the middle
  if (w == 768 && h == 384)
    return {512, 96, 256, 192};  // big top screen (2x)
  return {0, 0, static_cast<float>(w), static_cast<float>(h)};
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
  else if (s_system == SystemId::PS1)
    BuildPs1Variables(s_game_id);
  else if (s_system == SystemId::NDS)
    BuildNdsVariables(s_game_id);
  else if (s_system == SystemId::PSP)
    BuildPspVariables(s_game_id);
  else if (s_system == SystemId::DC)
    BuildDcVariables(s_game_id);
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
  if (s_defer_frame)
  {
    // The drawing beside the emulation still shows the image before: taken over with the frame.
    s_pending.image = *image;
    s_pending.image_set = true;
    s_pending.semaphores.assign(semaphores, semaphores + semaphore_count);
    return;
  }
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
    const bool undeclared = it != s_variables.end() && !s_core_keys.empty() && s_core_keys.count(variable->key) == 0;
    if (undeclared && s_undeclared_logged.insert(variable->key).second)
    {
      LogF("%s: option %s isn't one the core declared: %s", s_core->name, variable->key,
           s_system == SystemId::DC ? "not given" : "given anyway");
    }
    if (it == s_variables.end() || (undeclared && s_system == SystemId::DC))
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
  {
    // RGB565 as the game texture, the upscalers and the GPU filters take it; XRGB8888 (melonDS) is
    // made RGB565 as each frame arrives.
    const retro_pixel_format format = *static_cast<const retro_pixel_format*>(data);
    if (format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_XRGB8888)
      return false;
    s_xrgb8888 = format == RETRO_PIXEL_FORMAT_XRGB8888;
    return true;
  }
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
    // (Logged once while it repeats: Flycast's "Reboot to BIOS" comes every frame until the restart.)
    static std::string logged;
    if (logged != message->msg)
      LogF("%s: message: %s", s_core->name, message->msg);
    logged = message->msg;
    if (s_system == SystemId::DC && std::strcmp(message->msg, "Reboot to BIOS") == 0)
    {
      s_dc_reboot = true;  // (not shown: Run() starts the console again)
      return true;
    }
    s_message = message->msg;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    static_cast<retro_rumble_interface*>(data)->set_rumble_state = SetRumble;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    // mGBA, SwanStation, melonDS DS and PPSSPP read all the buttons at once (InputState's mask); the
    // others as before.
    return IsHandheld(s_system) || s_system == SystemId::PS1 || s_system == SystemId::NDS ||
           s_system == SystemId::PSP || s_system == SystemId::DC;
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
  case RETRO_ENVIRONMENT_SHUTDOWN:
    // Flycast's HLE BIOS with the lid open throws "Reboot to BIOS" (its message just before) and asks
    // this every frame: Run() starts the console again once the lid closes - not the game's end.
    if (s_system == SystemId::DC && s_dc_reboot)
    {
      s_dc_stopped = true;
      return true;
    }
    // The core can't go on (PPSSPP: the game didn't start): back to the menu.
    s_shutdown = true;
    LogF("%s: shutdown asked", s_core->name);
    return true;
  case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
    if (!data || !TakesDiscs())
      return false;
    *static_cast<unsigned*>(data) = 1;  // the interface with set_initial_image
    return true;
  case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    if (!data || !TakesDiscs())
      return false;
    s_disk = *static_cast<const retro_disk_control_ext_callback*>(data);
    s_disk_set = true;
    return true;
  case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
  {
    if (!data || !TakesDiscs())
      return false;
    const auto* disk = static_cast<const retro_disk_control_callback*>(data);
    s_disk = {};
    s_disk.set_eject_state = disk->set_eject_state;
    s_disk.get_eject_state = disk->get_eject_state;
    s_disk.get_image_index = disk->get_image_index;
    s_disk.set_image_index = disk->set_image_index;
    s_disk.get_num_images = disk->get_num_images;
    s_disk.replace_image_index = disk->replace_image_index;
    s_disk.add_image_index = disk->add_image_index;
    s_disk_set = true;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  {
    // The core's options (all of them each time: the list before is replaced).
    s_core_keys.clear();
    for (const auto* variable = static_cast<const retro_variable*>(data); variable && variable->key; variable++)
      s_core_keys.insert(variable->key);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
  case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
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
    if (s_defer_frame)
    {
      s_pending = {true, true, nullptr, width, height, 0};
      return;
    }
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
  if (s_xrgb8888)
  {
    // 32-bit pixels: 5-6-5 bits of each (the DS draws 6 bits per colour; the picture stays the same).
    s_converted.resize(static_cast<size_t>(width) * height);
    for (unsigned y = 0; y < height; y++)
    {
      const uint32_t* in = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pixels) + y * pitch);
      uint16_t* out = s_converted.data() + static_cast<size_t>(y) * width;
      for (unsigned x = 0; x < width; x++)
      {
        const uint32_t c = in[x];
        out[x] = static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
      }
    }
    pixels = s_converted.data();
    pitch = static_cast<size_t>(width) * 2;
  }
  if (s_defer_frame)
  {
    s_pending = {false, true, pixels, width, height, pitch};
    return;
  }
  s_frame = pixels;
  s_frame_w = width;
  s_frame_h = height;
  s_frame_pitch = pitch;
  s_frame_new = true;
}

size_t AudioBatch(const int16_t* samples, size_t frames)
{
  if (s_audio)
    s_audio->Push(samples, frames, s_sample_rate * s_audio_speed);
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
  if ((device & 0xFF) == RETRO_DEVICE_POINTER)
  {
    // Player 1's touchpad as a touch screen (the DS's stylus): -0x7FFF..0x7FFF over the picture.
    if (port != 0 || index != 0)
      return 0;
    return id == RETRO_DEVICE_ID_POINTER_X       ? s_pointer.x :
           id == RETRO_DEVICE_ID_POINTER_Y       ? s_pointer.y :
           id == RETRO_DEVICE_ID_POINTER_PRESSED ? (s_pointer.pressed ? 1 : 0) :
           id == RETRO_DEVICE_ID_POINTER_COUNT   ? (s_pointer.pressed ? 1 : 0) :
                                                   0;
  }
  if (port < MAX_PLAYERS && (device & 0xFF) == RETRO_DEVICE_ANALOG)
  {
    if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)  // the Dreamcast's analog triggers (Flycast)
      return id == RETRO_DEVICE_ID_JOYPAD_L2 ? s_triggers[port][0] :
             id == RETRO_DEVICE_ID_JOYPAD_R2 ? s_triggers[port][1] :
                                               0;
    return index < 2 && id < 2 ? s_analog[port][index][id] : 0;
  }
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
  if (s_system == SystemId::DC)
    return GpuFilter::Mode::Off;  // (the Dreamcast plays at Low: BuildDcVariables)
  const std::string name = Settings::Get("gpu_filter", game);
  GpuFilter::Mode mode = name == "pixel"      ? GpuFilter::Mode::PixelArt :
                         name == "ai"         ? GpuFilter::Mode::AI :
                         name == "fsr"        ? GpuFilter::Mode::Smooth :
                         name == "sharpen"    ? GpuFilter::Mode::Sharpen :
                         name == "edges"      ? GpuFilter::Mode::Edges :
                         name == "natural"    ? GpuFilter::Mode::NaturalHD :
                         name == "natural_ai" ? GpuFilter::Mode::NaturalAI :
                                                GpuFilter::Mode::Off;
  // Smooth Edges, Natural HD and Natural AI (the Quality levels' steps) suit every console and size
  // as they are: GpuFilter redraws pixel art and only anti-aliases drawn (3D) pictures (the source's
  // kind, DrawPicture), and leaves out what a picture as big as the screen doesn't need.
  const bool steps = mode == GpuFilter::Mode::Edges || mode == GpuFilter::Mode::NaturalHD ||
                     mode == GpuFilter::Mode::NaturalAI;
  if (s_system == SystemId::NDS && s_frame_w > 768 &&
      (mode == GpuFilter::Mode::AI || mode == GpuFilter::Mode::PixelArt))
  {
    // The DS's Big Top Screen: already made 3x bigger by the core; Smooth HD on it.
    mode = GpuFilter::Mode::Smooth;
  }
  else if (s_frame_hw && s_system == SystemId::PSP)
  {
    // The PSP: as the PlayStation, the AI on its HD 3D from 3x (1440 wide), else Smooth HD. At 4x the
    // picture is the screen's size already: the AIs' 1.5-3 ms of GPU went short where the 4x picture
    // itself needed it (God of War at Ultra: the drawing waited 6-7 ms a frame for the GPU), so it is
    // only sharpened (Natural HD's pass for a picture that big).
    if ((mode == GpuFilter::Mode::AI && s_frame_w < 1440) || mode == GpuFilter::Mode::PixelArt)
      mode = GpuFilter::Mode::Smooth;
    if (s_frame_w >= 1900 && (mode == GpuFilter::Mode::AI || mode == GpuFilter::Mode::NaturalAI))
      mode = GpuFilter::Mode::NaturalHD;
  }
  else if (s_frame_hw && s_system == SystemId::PS1)
  {
    // The PlayStation: the AI on its HD 3D from 3x (made a little smaller, supersampled, then the
    // network). On its 1x and 2x pictures the AI and Pixel Art HD looked painted (real Tekken 3 and
    // Gran Turismo 2 frames): Smooth HD there.
    if ((mode == GpuFilter::Mode::AI && Settings::GetInt("ps1_resolution", game) < 3) ||
        mode == GpuFilter::Mode::PixelArt)
      mode = GpuFilter::Mode::Smooth;
  }
  else if (s_frame_hw)
  {
    // The N64's 3D has no pixel art to redraw: Smooth HD, which at 4x (as wide as the screen
    // already) only sharpens.
    if (mode == GpuFilter::Mode::PixelArt || mode == GpuFilter::Mode::AI)
      mode = GpuFilter::Mode::Smooth;
    if (mode == GpuFilter::Mode::Smooth && s_frame_w >= 2560)
      mode = GpuFilter::Mode::Sharpen;
  }
  else if ((mode == GpuFilter::Mode::PixelArt || mode == GpuFilter::Mode::AI || steps) && NtscFilterOn(game))
  {
    mode = GpuFilter::Mode::Off;  // the NTSC filter's picture is a TV's look of its own
  }
  return mode;
}

// The mode redraws the console's own picture (the CPU's Resolution is left out).
bool GpuRedraws(GpuFilter::Mode mode)
{
  return mode == GpuFilter::Mode::PixelArt || mode == GpuFilter::Mode::AI || mode == GpuFilter::Mode::Smooth ||
         mode == GpuFilter::Mode::Edges || mode == GpuFilter::Mode::NaturalHD || mode == GpuFilter::Mode::NaturalAI;
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
  const bool nds_big = NdsBig();
  const float frame_w = nds_big ? 1024.0f : static_cast<float>(s_frame_w);  // (the core's own placement)
  float frame_h = nds_big ? 576.0f : static_cast<float>(s_frame_h);
  if (s_system == SystemId::PS1)
    frame_h /= static_cast<float>(std::clamp(Settings::GetInt("ps1_resolution", game), 1, 4));  // SwanStation's scale
  else if (s_system == SystemId::PSP)
    frame_h /= std::max(1.0f, std::round(static_cast<float>(s_frame_w) / 480.0f));  // PPSSPP's (480x270 at 1x)
  else if (s_system == SystemId::DC)
    frame_h /= std::max(1.0f, std::floor(frame_h / 480.0f));  // Flycast's internal resolution (480 lines at 1x)
  else if (s_frame_hw && s_frame_w >= 1280)
    frame_h /= static_cast<float>(s_frame_w / 640);  // ParaLLEl-RDP's upscaled scanout
  const float lines = frame_h > 300.0f && s_system != SystemId::NDS ? frame_h * 0.5f : frame_h;  // (the DS: two screens)
  picture.rows = lines;
  const std::string aspect = Settings::Get("aspect", game);
  float ratio = 4.0f / 3.0f;
  // A handheld's screen keeps its shape (10:9, the Game Boy Advance's 3:2) at Auto and Pixel
  // Perfect: its dots are square. The Super Game Boy's 256x224 picture was a TV's.
  const bool screen = (IsHandheld(s_system) && s_frame_w != 256) || s_system == SystemId::NDS;
  if (screen && !nds_big)
    picture.columns = static_cast<float>(s_frame_w);
  if (screen && (aspect == "auto" || aspect == "pixel"))
    ratio = frame_w / std::max(1.0f, frame_h);
  else if (s_system == SystemId::PS1 && Settings::Get("ps1_widescreen", game) == "on" && aspect != "stretch")
    ratio = 16.0f / 9.0f;  // the widescreen hack's picture
  else if (s_system == SystemId::PSP && (aspect == "auto" || aspect == "pixel"))
    ratio = static_cast<float>(s_frame_w) / std::max(1.0f, static_cast<float>(s_frame_h));  // its 16:9 screen
  else if (aspect == "pixel" && s_system != SystemId::N64 && s_system != SystemId::PS1 && s_system != SystemId::PSP &&
           s_system != SystemId::DC)
    ratio = 256.0f / std::max(1.0f, lines);  // (the N64's and the PlayStation's pictures are 4:3 already)
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
    // The PSP's picture keeps the PSP's alpha (often 0): read as opaque, else it is blended away.
    VkImageView view = s_hw_image.image_view;
    if (s_system == SystemId::PSP)
    {
      const VkImageView opaque = display.OpaqueView(s_hw_image.create_info);
      if (opaque != VK_NULL_HANDLE)
        view = opaque;
    }
    if (gpu != GpuFilter::Mode::Off)
    {
      GpuFilter::Source source;
      source.view = view;
      source.layout = s_hw_image.image_layout;
      source.width = s_frame_w;
      source.height = s_frame_h;
      source.kind = GpuFilter::Source::Kind::Drawn;  // the cores' 3D
      if (GpuFilter::Draw(dl, gpu, source, picture.min, picture.max))
      {
        DrawScanlines(dl, picture, game);
        return;
      }
    }
    // Above Native (2x, 4x) always smooth, as Texture Filtering promises: sharp (nearest) drops
    // whole columns of the 2560-wide picture and doubles some rows on the way to the screen.
    const bool upscaled = s_system == SystemId::PS1   ? Settings::GetInt("ps1_resolution", game) > 1 :
                          s_system == SystemId::PSP   ? s_frame_w > 480 :
                          s_system == SystemId::DC    ? s_frame_h > 480 :
                                                        s_frame_w >= 1280;
    const bool smooth = upscaled || Settings::Get("filter", game) == "smooth";
    const ImTextureID image = display.ExternalTexture(view, s_hw_image.image_layout, smooth);
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
  if (NdsBig())
  {
    // The DS's Big Top Screen, from the side-by-side picture: the top screen 3x over three quarters
    // of a 16:9 picture (through the GPU's filters: it is the picture's left half, in the texture's
    // top-left corner), the touch screen 1x at the right, in the middle.
    const float s = (picture.max.x - picture.min.x) / 1024.0f;
    const ImVec2 top0 = picture.min, top1(picture.min.x + 768.0f * s, picture.min.y + 576.0f * s);
    const ImVec2 touch0(picture.min.x + 768.0f * s, picture.min.y + 192.0f * s);
    const ImVec2 touch1(picture.min.x + 1024.0f * s, picture.min.y + 384.0f * s);
    const float half = static_cast<float>(s_texture_w) * 0.5f;
    bool drawn = false;
    if (gpu != GpuFilter::Mode::Off && (!s_texture_scaled || !GpuRedraws(gpu)))
    {
      GpuFilter::Source source;
      source.view = display.GameView();
      source.width = s_texture_w / 2;
      source.height = s_texture_h;
      source.exact = false;
      source.kind = GpuFilter::Source::Kind::PixelArt;
      drawn = GpuFilter::Draw(dl, gpu, source, top0, top1);
    }
    const bool smooth = s_texture_scaled || Settings::Get("filter", game) == "smooth";
    const float inset = smooth ? 0.5f : 0.0f;
    const float tw = Display::GAME_TEXTURE_W, th = Display::GAME_TEXTURE_H;
    if (!drawn)
    {
      dl->AddImage(ImTextureRef(display.GameTexture(smooth)), top0, top1, ImVec2(inset / tw, inset / th),
                   ImVec2((half - inset) / tw, (s_texture_h - inset) / th));
    }
    dl->AddImage(ImTextureRef(display.GameTexture(true)), touch0, touch1, ImVec2((half + 0.5f) / tw, 0.5f / th),
                 ImVec2((s_texture_w - 0.5f) / tw, (s_texture_h - 0.5f) / th));
    return;
  }
  if (gpu != GpuFilter::Mode::Off && (!s_texture_scaled || !GpuRedraws(gpu)))
  {
    GpuFilter::Source source;
    source.view = display.GameView();
    source.width = s_texture_w;
    source.height = s_texture_h;
    source.exact = false;
    // Pixel art, but for the DS's Big Top Screen: made 3x bigger by the core already (its 3D drawn
    // bigger), anti-aliased only.
    source.kind = s_system == SystemId::NDS && s_frame_w > 768 ? GpuFilter::Source::Kind::Drawn :
                                                                  GpuFilter::Source::Kind::PixelArt;
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
  if ((s_frame_w > 300 || s_frame_h > 300) && s_system != SystemId::NDS)
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

// The saves on a PlayStation memory card: its directory, frames 1-15 of block 0 (0x51: a save's first
// block in use).
int CardSaves(const std::string& card)
{
  if (card.size() < 16 * 0x80 || card[0] != 'M' || card[1] != 'C')
    return 0;
  int saves = 0;
  for (size_t frame = 1; frame < 16; frame++)
    saves += static_cast<uint8_t>(card[frame * 0x80]) == 0x51 ? 1 : 0;
  return saves;
}

// A game on several discs was each disc's own game before 00.50: the PlayStation's memory card (its
// battery save) is the discs' card with the most saves then (a disc started once has a new, empty card:
// the newest one isn't it), the newest of those. Once: before the game's first session as one game (its
// .disc is written at the end of each), so a card deleted in Saved Data stays deleted. (Flycast names
// the Dreamcast's VMU after the game's product number, the same on each disc.)
void AdoptDiscsCard(const Library::Game& game)
{
  if (game.system != SystemId::PS1 || game.discs.size() < 2)
    return;
  const std::string card = Saves::SramPath(game.key);
  if (FileExists(card) || FileExists(Saves::DiscPath(game.key)))
    return;
  std::string best, best_path;
  int best_saves = 0;
  int64_t best_time = 0;
  for (const std::string& disc : game.discs)
  {
    const std::string old = Saves::SramPath(GetSystem(game.system).prefix + std::filesystem::path(disc).stem().string());
    const std::string data = ReadTextFile(old);
    const int saves = CardSaves(data);
    const int64_t time = Saves::Modified(old);
    if (saves > best_saves || (saves > 0 && saves == best_saves && time > best_time))
    {
      best = data;
      best_path = old;
      best_saves = saves;
      best_time = time;
    }
  }
  if (best_saves > 0 && WriteTextFile(card, best))
    LogF("save: the memory card of %s (%d saves) for all the game's discs", best_path.c_str(), best_saves);
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

// The core's state. Flycast's emulation thread runs between its size and the state itself, which can
// then be bigger (a display list or a maple transfer begun meanwhile: "Serializer buffer overflow"): it
// is measured again, up to 3 times. (No size: the emulation ended in an error.)
bool SerializeCore(std::vector<uint8_t>* out)
{
  for (int attempt = 0; attempt < 3; attempt++)
  {
    const size_t size = s_core->serialize_size();
    if (size == 0)
      break;
    out->assign(size, 0);
    if (s_core->serialize(out->data(), size))
      return true;
    if (s_system != SystemId::DC)
      break;
  }
  std::vector<uint8_t>().swap(*out);
  return false;
}

bool SaveState(int slot)
{
  std::vector<uint8_t> state;
  if (!SerializeCore(&state))
    return false;
  const size_t size = state.size();
  const std::string path = Saves::StatePath(s_game_id, slot);
  if (!WriteDataFile(path, state.data(), size))
    return false;
  // A Dreamcast game on several discs: the disc in it with the state (Flycast's states don't hold it;
  // LoadState puts it in first).
  const std::string disc = path + ".disc";
  if (s_system == SystemId::DC && DiscCount() > 1)
    WriteTextFile(disc, std::to_string(DiscIn()));
  else
    std::remove(disc.c_str());
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
  // (Read at its size: a Dreamcast state is ~36 MB, and a string grown on the way took 64 MB.)
  const std::string path = Saves::StatePath(s_game_id, slot);
  std::vector<uint8_t> data(Saves::Size(path));
  FILE* file = data.empty() ? nullptr : fopen(path.c_str(), "rb");
  const bool read = file && fread(data.data(), 1, data.size(), file) == data.size();
  if (file)
    fclose(file);
  if (!read)
    return false;
  // The state's disc first (a Dreamcast game on several discs: a state of disc 1 run over disc 2 goes
  // on with the wrong disc; SwanStation's states change the disc themselves). Flycast changes it with
  // the lid closed in one call, its emulation stopped meanwhile (scripts/build-dc-core.sh); the disc
  // before goes back in when the state can't be loaded.
  int disc_before = -1;
  std::vector<uint8_t> machine;  // (as it was: put back when the state fails, the disc changes unseen)
  if (s_system == SystemId::DC && DiscCount() > 1 && s_disk.set_image_index)
  {
    const std::string saved = ReadTextFile(Saves::StatePath(s_game_id, slot) + ".disc");
    const int disc = saved.empty() ? -1 : std::atoi(saved.c_str());
    if (disc >= 0 && disc < DiscCount() && disc != DiscIn())
    {
      // (Without it the disc isn't changed: a state that fails would leave two disc changes seen.)
      if (!SerializeCore(&machine))
      {
        LogF("state: slot %d is disc %d's: the machine couldn't be kept first, not loaded", slot, disc + 1);
        return false;
      }
      disc_before = DiscIn();
      const bool in = s_disk.set_image_index(static_cast<unsigned>(disc));
      LogF("state: slot %d is disc %d's: %s", slot, disc + 1, in ? "put in" : "it couldn't be put in");
      if (!in)
      {
        s_disk.set_image_index(static_cast<unsigned>(disc_before));
        std::vector<uint8_t>().swap(data);  // (memory for the restore)
        s_core->unserialize(machine.data(), machine.size());
        return false;
      }
    }
  }
  const bool ok = s_core->unserialize(data.data(), data.size());
  if (!ok && disc_before >= 0)
  {
    s_disk.set_image_index(static_cast<unsigned>(disc_before));
    std::vector<uint8_t>().swap(data);
    s_core->unserialize(machine.data(), machine.size());
  }
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

// One pass's drawing on a thread of its own while the emulation thread runs the next frames: the
// DS's core takes most of a refresh (Mario Kart DS: 15-17 ms of 16.7), the drawing 2-3 ms more,
// one after the other two frames had to be run per refresh (25 pictures a second). The picture shown
// is one frame older. Display and ImGui are used by one thread at a time: the emulation thread waits
// (Wait) before it touches them again.
class DrawWorker
{
public:
  ~DrawWorker() { Stop(); }
  bool Start()
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);  // Mesa's submits and ImGui
    const bool ok = pthread_create(&m_thread, &attr, &DrawWorker::Main, this) == 0;
    pthread_attr_destroy(&attr);
    m_started = ok;
    return ok;
  }
  void Post(std::function<void()> job)
  {
    std::lock_guard lock(m_mutex);
    m_job = std::move(job);
    m_busy = true;
    m_wake.notify_all();
  }
  void Wait()
  {
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [this] { return !m_busy; });
  }
  void Stop()
  {
    if (!m_started)
      return;
    {
      std::lock_guard lock(m_mutex);
      m_quit = true;
    }
    m_wake.notify_all();
    pthread_join(m_thread, nullptr);
    m_started = false;
  }

private:
  static void* Main(void* self)
  {
    // Beside the emulation thread (core 2) and the DS's 3D thread (core 3), at their priority; off
    // core 4 for the PSP (PPSSPP's render thread, port/ps4_n64.c).
    // (the DS: off cores 3-5, its 2D helper's and 3D threads', port/ps4_n64.c)
    const uint64_t cores = s_system == SystemId::PSP ? (1ull << 1) | (1ull << 5) :
                           s_system == SystemId::NDS ? (1ull << 1) :
                                                       (1ull << 1) | (1ull << 4) | (1ull << 5);
    const int affinity = emusuni_set_affinity(scePthreadSelf(), cores);
    const int priority = scePthreadSetprio(scePthreadSelf(), 480);
    LogF("game: drawing beside the emulation: cores %#llx (%#x), priority 480 (%#x)",
         static_cast<unsigned long long>(cores), static_cast<unsigned>(affinity), static_cast<unsigned>(priority));
    auto* worker = static_cast<DrawWorker*>(self);
    std::unique_lock lock(worker->m_mutex);
    for (;;)
    {
      worker->m_wake.wait(lock, [worker] { return worker->m_busy || worker->m_quit; });
      if (worker->m_quit)
        return nullptr;
      std::function<void()> job = std::move(worker->m_job);
      lock.unlock();
      job();
      lock.lock();
      worker->m_busy = false;
      worker->m_done.notify_all();
    }
  }
  pthread_t m_thread{};
  bool m_started = false, m_busy = false, m_quit = false;
  std::mutex m_mutex;
  std::condition_variable m_wake, m_done;
  std::function<void()> m_job;
};

Result Run(Display& display, const Library::Game& game, ImFont* regular, ImFont* bold,
           std::string* error)
{
  LogF("game: %s (%s)", game.title.c_str(), game.path.c_str());
  std::vector<uint8_t> rom;
  // A CD image is read by the core itself from its path (a missing one would start the BIOS alone).
  const bool by_path = GetSystem(game.system).by_path;
  if (by_path && !FileExists(game.path))
  {
    *error = "The game's file can't be opened";
    LogF("game: can't open %s", game.path.c_str());
    return Result::Failed;
  }
  for (size_t i = 0; i < game.discs.size(); i++)
  {
    if (!FileExists(game.discs[i]))
    {
      *error = "Disc " + std::to_string(i + 1) + " of this game can't be opened";
      LogF("game: can't open disc %zu, %s", i + 1, game.discs[i].c_str());
      return Result::Failed;
    }
  }
  if (!by_path && !Library::ReadRom(game.path, &rom, error))
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
  s_xrgb8888 = false;  // RGB565 unless the core asks for XRGB8888
  s_pointer = {};
  for (auto& player : s_analog)
    player = {};
  s_triggers = {};
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
  if (s_system == SystemId::PSP)
  {
    // PPSSPP's own files (fonts, system dialogs, compat.ini: /app0/ppsspp/PPSSPP, scripts/package.sh),
    // read only; the memory stick (the games' saves, PSP/SAVEDATA) in saves/psp.
    s_system_dir = std::string(App::ASSETS_DIR) + "/ppsspp";
    s_save_dir += "/psp";
    MakeDirs(s_save_dir);
  }
  if (s_system == SystemId::DC)
  {
    // Each game's VMU A1 in saves/dc; system/dc (made by the core) holds the BIOS, the flash, the
    // shared VMUs and its caches.
    s_save_dir += "/dc";
    MakeDirs(s_save_dir);
  }
  s_shutdown = false;
  s_dc_reboot = s_dc_stopped = false;
  s_psp_scale = 0;
  s_psp_30fps_auto = false;
  s_nds_chosen.clear();
  s_nds_alt = false;
  s_nds_r3 = false;
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

  s_core_keys.clear();  // declared again by set_environment (SET_VARIABLES)
  s_undeclared_logged.clear();
  s_disk = {};  // set again by the core's init (SET_DISK_CONTROL_EXT_INTERFACE)
  s_disk_set = false;
  s_core->set_environment(Environment);
  s_core->set_video_refresh(VideoRefresh);
  s_core->set_audio_sample(AudioSample);
  s_core->set_audio_sample_batch(AudioBatch);
  s_core->set_input_poll(InputPoll);
  s_core->set_input_state(InputState);
  s_core->init();
  // The Dreamcast's pads before the game (set after it, Flycast unplugs them for 0.1 s at its first
  // frame, and a game continued from its state then sees no pad).
  if (s_system == SystemId::DC)
  {
    for (unsigned port = 0; port < 4; port++)
      s_core->set_controller_port_device(port, RETRO_DEVICE_JOYPAD);
  }
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
  // The PSP: PPSSPP's render thread records and submits its Vulkan work beside the emulation
  // (scripts/build-psp-core.sh: the core then calls nothing of the frontend's from it but the queue's
  // lock; the picture is one frame later). God of War: Ghost of Sparta's fights ~17.4 -> ~13-14 ms
  // a frame on the PS4 (estimated from the PC) with fast memory and hardware skinning.
  // (God of War: Ghost of Sparta froze at its start in 00.40/00.41: not the render thread but the
  // heap's spin lock under the PS4's fixed priorities, port/ps4_dlmalloc.c.)
  if (s_system == SystemId::PSP)
    setenv("EMUSUNI_PSP_RENDER_THREAD", "1", 1);
  // PPSSPP's pipelines with the driver's own cache data kept (build-psp-core.sh): those of earlier
  // sessions aren't compiled again at each start (God of War recompiled 115, 60-140 ms each).
  if (s_system == SystemId::PSP)
    setenv("EMUSUNI_PSP_PIPELINE_CACHE", "1", 1);
  // A game on several discs: an .m3u of them (data_dir/cache/m3u/<console>), the core's disc changer
  // swaps them (the in-game menu's Disc). It starts with the disc it was played with last: a state
  // saved on disc 2 is disc 2's.
  std::string content = game.path;
  if (game.discs.size() > 1)
  {
    const std::string dir = std::string(App::DATA_DIR) + "/cache/m3u/" + GetSystem(game.system).key;
    MakeDirs(dir);
    std::string name = game.id;
    for (char& c : name)
    {
      if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        c = '_';
    }
    const std::string m3u = dir + "/" + name + ".m3u";
    std::string list;
    for (const std::string& disc : game.discs)
      list += disc + "\n";
    if (WriteTextFile(m3u, list))
      content = m3u;
    LogF("game: %zu discs, %s", game.discs.size(), content == m3u ? m3u.c_str() : "the .m3u couldn't be written");
  }
  // (Only for an .m3u: SwanStation's start fails with a disc number for one disc's file - the .m3u
  // couldn't be written.)
  const std::vector<std::string> discs = Library::Discs(game);
  std::string extension = content.size() > 4 ? content.substr(content.size() - 4) : std::string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool playlist = extension == ".m3u";
  int last_disc = 0;
  if (s_disk_set && s_disk.set_initial_image && discs.size() > 1 && playlist)
  {
    last_disc = std::atoi(ReadTextFile(Saves::DiscPath(game.key)).c_str());
    if (last_disc > 0 && last_disc < static_cast<int>(discs.size()))
    {
      const bool taken = s_disk.set_initial_image(static_cast<unsigned>(last_disc), discs[last_disc].c_str());
      LogF("game: starts with disc %d, played last (%s)", last_disc + 1, taken ? "taken" : "refused");
    }
    else
    {
      last_disc = 0;
    }
  }
  retro_game_info info = {};
  info.path = content.c_str();
  info.data = rom.empty() ? nullptr : rom.data();  // a CD image: its path only
  info.size = rom.size();
  // The same for GET_GAME_INFO_EXT; a .zip's ROM is named after the .zip (the core reads the
  // region from the name).
  const bool zipped = content.size() > 4 &&
                      (content.compare(content.size() - 4, 4, ".zip") == 0 ||
                       content.compare(content.size() - 4, 4, ".ZIP") == 0);
  const size_t slash = content.rfind('/');
  const size_t dot = content.rfind('.');
  s_info_path = content;
  s_info_dir = slash == std::string::npos ? "." : content.substr(0, slash);
  s_info_name = game.id;
  s_info_extension = zipped ? GetSystem(game.system).extensions.front().substr(1) :
                              (dot == std::string::npos ? "" : content.substr(dot + 1));
  s_info_ext = {};
  s_info_ext.full_path = zipped ? nullptr : s_info_path.c_str();
  s_info_ext.archive_path = zipped ? s_info_path.c_str() : nullptr;
  s_info_ext.dir = s_info_dir.c_str();
  s_info_ext.name = s_info_name.c_str();
  s_info_ext.ext = s_info_extension.c_str();
  s_info_ext.data = rom.empty() ? nullptr : rom.data();
  s_info_ext.size = rom.size();
  s_info_ext.file_in_archive = zipped;
  // N64 CPU Cores: before the game, which starts ParaLLEl-RDP's threads.
  const bool fixed_cores = s_system == SystemId::N64 && Settings::Get("n64_cores", game.key) != "system";
#ifdef EMUSUNI_N64
  emusuni_n64_fixed_cores = fixed_cores ? 1 : 0;
#endif
  const bool loaded = s_core->load_game(&info);
  s_info_ext = {};
  if (s_system == SystemId::N64 || IsHandheld(s_system) || s_system == SystemId::NDS)
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
    // The disc played last doesn't start (gone, or damaged): the game starts with disc 1 next time.
    if (last_disc > 0)
    {
      std::remove(Saves::DiscPath(game.key).c_str());
      *error += "\nIt starts with disc 1 next time.";
      LogF("game: disc %d didn't start: disc 1 next time", last_disc + 1);
    }
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
  // The PlayStation's pads are DualShocks (SwanStation's analog controller); the Super Multitap is
  // the Super Nintendo's (FCEUmm plugs in the Four Score by itself).
  constexpr unsigned DEVICE_DUALSHOCK = (1 << 8) | RETRO_DEVICE_ANALOG;
  const bool ps1 = s_system == SystemId::PS1;
  s_core->set_controller_port_device(0, ps1 ? DEVICE_DUALSHOCK : RETRO_DEVICE_JOYPAD);
  const bool snes = s_system == SystemId::SNES;
  bool multitap = snes && Settings::GetOn("multitap", game.key);
  s_core->set_controller_port_device(1, ps1 ? DEVICE_DUALSHOCK : multitap ? DEVICE_MULTITAP : RETRO_DEVICE_JOYPAD);
  AdoptDiscsCard(game);
  LoadSram();
  LoadBezel(display, game);
  if (DiscCount() > 1)
    LogF("game: %d discs, disc %d in", DiscCount(), DiscIn() + 1);
  // (Flycast takes the disc played last only when its path is the .m3u's as it reads it: else disc 1
  // is in, and the state Continue loads puts its own disc in - LoadState.)
  if (last_disc > 0 && DiscIn() != last_disc)
    LogF("game: disc %d asked for, disc %d in", last_disc + 1, DiscIn() + 1);

  IngameMenu menu;
  menu.Init(&display, regular, bold);
  menu.SetGame(App::DATA_DIR, game.key, game.title, game.system, game.path);
  menu.CheatList().ApplyToCore(*s_core, s_system);
  if (!menu.CheatList().Entries().empty())
    LogF("cheats: %zu codes", menu.CheatList().Entries().size());
  // Changing the disc (the in-game menu's Disc): the lid stays open for 1.5 s of the game - a game sees
  // it open, as on the console - then the disc chosen goes in.
  int disc_lid_open = 0;
  int disc_before = 0;  // (the disc that was in: it goes back in when the one chosen doesn't)
  auto insert_disc = [&]() {
    disc_lid_open = 0;
    const int disc = DiscIn();
    if (s_disk.set_eject_state && s_disk.set_eject_state(false))
    {
      menu.Toast("Disc " + std::to_string(disc + 1) + " is in");
      LogF("game: disc %d in, the lid closed", disc + 1);
      return;
    }
    // (A disc that can't be read: the one before goes back in, the menu and the next start say so.)
    const bool back = disc != disc_before && s_disk.set_image_index &&
                      s_disk.set_image_index(static_cast<unsigned>(disc_before)) && s_disk.set_eject_state(false);
    menu.Toast("The disc couldn't be put in");
    LogF("game: disc %d couldn't be put in; disc %d %s", disc + 1, disc_before + 1, back ? "is in again" : "neither");
  };
  // Flycast's "Reboot to BIOS" (s_dc_reboot): the console starts again once the lid is closed; a disc
  // that goes back to the BIOS each time it starts ends the game after 3 restarts in a row.
  bool dc_reboot_pending = false;
  int dc_restart_wait = 0;  // (frames after a restart: the messages already sent aren't another one)
  int dc_restarts = 0, dc_since_restart = 1 << 30;
  auto restart_dc = [&]() {
    dc_reboot_pending = false;
    s_dc_stopped = false;
    s_core->reset();  // (Flycast boots the disc in the drive: scripts/build-dc-core.sh)
    dc_restart_wait = 180;
    menu.Toast(DiscCount() > 1 ? "The console starts again with disc " + std::to_string(DiscIn() + 1) :
                                 std::string("The console starts again"),
               3.0f);
    LogF("game: the game went back to the Dreamcast's BIOS: the console starts again (disc %d)", DiscIn() + 1);
  };

  // Continue: the PSP's state after its first frame (PPSSPP's GPU is made by then).
  bool resume_pending = false;
  if (Settings::GetOn("resume", game.key) && Saves::Modified(Saves::StatePath(game.key, 0)))
  {
    if (s_system == SystemId::PSP)
      resume_pending = true;
    else if (LoadState(0))
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
  // The DS: its 3D thread on core 3; the PSP and the Dreamcast: their other threads off core 2
  // (port/ps4_n64.c).
  if (fixed_cores || s_system == SystemId::NDS || s_system == SystemId::PSP || s_system == SystemId::DC)
  {
    const int got = emusuni_get_affinity(scePthreadSelf(), &old_mask);
    const int set = got == 0 ? emusuni_set_affinity(scePthreadSelf(), 1ull << 2) : -1;
    pinned = got == 0 && set == 0;
    LogF("game: emulation thread on core 2: %s (cores before %#llx, %#x %#x); running on CPU %d",
         pinned ? "yes" : "no", static_cast<unsigned long long>(old_mask), static_cast<unsigned>(got),
         static_cast<unsigned>(set), sceKernelGetCurrentCpu());
  }
  Controls::Layout layout = Controls::LayoutFor(game.key, game.system);
  // The DS: its drawing beside its emulation (DrawWorker).
  DrawWorker drawer;
  // The PSP: only while its core is heavy (one frame more latency then: psp_overlap).
  const bool overlap_drawing =
      (s_system == SystemId::NDS || s_system == SystemId::PSP || s_system == SystemId::DC) && drawer.Start();
  bool psp_overlap = false;
  s_defer_frame = false;
  s_pending = {};

  Stats stats;
  stats.pass_ms.reserve(4096);
  stats.run_ms.reserve(4096);
  ResetWindow(stats, display.GetFlipState());
  FramePacer pacer;
  VblankCheck vblank_check;
  const auto game_start = Clock::now();
  display.TakeWaitMs();  // the menu's waits aren't the game's
  auto last = Clock::now();
  auto next_pass = last;  // V-Sync off: when the next pass is due
  double schedule = 0.0;  // frames owed
  // The core's time per frame (a moving average), and whether it is too slow to be caught up.
  double core_avg = 0.0;
  bool heavy = false;
  int psp_heavy_passes = 0;  // (PSP Frame Rate Auto: passes in a row the core couldn't keep up)
  s_audio_speed = 1.0;
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
        // A disc being changed goes in first (the menu's states and Quit Game see it in).
        if (disc_lid_open > 0)
          insert_disc();
        // A Dreamcast waiting to start again starts now: the menu's Save, Load, Reset and Quit act on a
        // running console. (The disc's insert can be what found its emulation stopped.)
        if (s_dc_reboot)
        {
          s_dc_reboot = false;
          if (s_dc_stopped)
            dc_reboot_pending = true;
        }
        if (dc_reboot_pending)
          restart_dc();
        menu.SetDiscs(DiscCount(), DiscIn());
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
          dc_restart_wait = s_system == SystemId::DC ? 180 : 0;  // ("Reboot to BIOS" of before the state)
          menu.Toast("Loaded slot " + std::to_string(menu.Slot()));
          menu.Close();
        }
        else
        {
          menu.Toast("Slot " + std::to_string(menu.Slot()) + " can't be loaded");
        }
        break;
      case IngameMenu::Action::Reset:
        // (An emulation that ended in an error - found by a disc's insert as the menu opened - is
        // replaced by the reset: not the game's end. Before it: an error of the reset itself counts.)
        s_shutdown = false;
        s_message.clear();
        s_core->reset();
        dc_reboot_pending = s_dc_stopped = false;  // (the Dreamcast's restart, if it waited for one)
        dc_restart_wait = s_system == SystemId::DC ? 180 : 0;
        menu.Close();
        menu.Toast("Game reset");
        break;
      case IngameMenu::Action::ChangeDisc:
      {
        const unsigned disc = static_cast<unsigned>(menu.Disc());
        disc_before = DiscIn();
        const bool opened = s_disk.set_eject_state && s_disk.set_image_index && s_disk.set_eject_state(true);
        if (opened && s_disk.set_image_index(disc))
        {
          disc_lid_open = 90;
          dc_restarts = 0;  // (a restart this lid brings is the user's, not a disc going back by itself)
          LogF("game: the lid opened for disc %u", disc + 1);
          menu.Close();
          menu.Toast("Changing to disc " + std::to_string(disc + 1) + "...", 1.6f);
        }
        else
        {
          if (opened)
            s_disk.set_eject_state(false);  // the disc before goes back in
          menu.Toast("The disc couldn't be changed");
          LogF("game: disc %u couldn't be chosen", disc + 1);
        }
        break;
      }
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
        menu.CheatList().ApplyToCore(*s_core, s_system);
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

    // ---- The drawing (below; the DS's beside the emulation) ----
    bool drawn = false;
    double pass_draw_ms = 0.0, pass_draw_wait = 0.0;
    const bool vsync = Settings::Get("vsync", game.key) != "off";
    auto draw_pass = [&](uint64_t total) {
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
      stats.flip_frame[static_cast<size_t>(display.NextFlip() % 8)] = total;
      display.EndFrame();
      pass_draw_wait = display.TakeWaitMs();
      pass_draw_ms = std::chrono::duration<double, std::milli>(Clock::now() - draw_start).count();
      if (!tour.shot.empty())
        Tour::SaveShot(display, tour.shot);
    };

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
        if (s_system == SystemId::N64 || s_system == SystemId::PS1 || s_system == SystemId::PSP ||
            s_system == SystemId::DC)
        {
          // The analog sticks: the N64's control stick and C buttons, the DualShock's two sticks, the
          // PSP's and the Dreamcast's stick (the left one).
          auto axis = [](uint8_t value) {
            return static_cast<int16_t>(std::clamp((static_cast<int>(value) - 128) * 258, -32768, 32767));
          };
          const bool on = pad.ok && !touring;
          s_analog[i][0] = {on ? axis(pad.data.leftStick.x) : int16_t(0), on ? axis(pad.data.leftStick.y) : int16_t(0)};
          if (i == 0)
            s_stick_max = std::max({s_stick_max, std::abs(static_cast<int>(s_analog[0][0][0])),
                                    std::abs(static_cast<int>(s_analog[0][0][1]))});
          s_analog[i][1] = {on ? axis(pad.data.rightStick.x) : int16_t(0), on ? axis(pad.data.rightStick.y) : int16_t(0)};
        }
        if (s_system == SystemId::DC)
        {
          // The Dreamcast's analog triggers: the DualShock 4's L2 / R2 pressure, where its L and R
          // triggers are on L2 / R2 (else Flycast reads them pressed or not from the buttons).
          const bool on = pad.ok && !touring;
          auto trigger = [&](unsigned id) -> int16_t {
            const uint32_t mask = layout.button_mask[id];
            const int value = mask == ORBIS_PAD_BUTTON_L2 ? pad.data.analogButtons.l2 :
                              mask == ORBIS_PAD_BUTTON_R2 ? pad.data.analogButtons.r2 :
                                                            0;
            return static_cast<int16_t>(on ? value * 32767 / 255 : 0);
          };
          s_triggers[i] = {trigger(RETRO_DEVICE_ID_JOYPAD_L2), trigger(RETRO_DEVICE_ID_JOYPAD_R2)};
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
        if (i == 0 && s_system == SystemId::NDS)
        {
          // R3 (Swap Screens): the app's view of the screens, not the core's.
          const bool r3 = (s_buttons[0] >> RETRO_DEVICE_ID_JOYPAD_R3) & 1;
          s_buttons[0] &= ~(1u << RETRO_DEVICE_ID_JOYPAD_R3);
          if (r3 && !s_nds_r3)
          {
            const std::string before = NdsView();
            s_nds_alt = !s_nds_alt;
            if ((before == "stack") != (NdsView() == "stack"))
              BuildVariables();  // the core's layout changes too
          }
          s_nds_r3 = r3;
          // The touchpad (1920x942) is the DS's touch screen: a finger on it is the stylus down, at
          // the same place on the touch screen's part of the frame.
          s_pointer = {};
          const auto& touch = pad.data.touch;
          if (pad.ok && !touring && touch.fingers > 0 && s_frame_w > 0 && s_frame_h > 0)
          {
            const auto screen = NdsTouchScreen(s_frame_w, s_frame_h);
            const float u = std::clamp(touch.touch[0].x / 1919.0f, 0.0f, 1.0f);
            const float v = std::clamp(touch.touch[0].y / 941.0f, 0.0f, 1.0f);
            const float fx = screen[0] + u * (screen[2] - 1.0f), fy = screen[1] + v * (screen[3] - 1.0f);
            s_pointer.x = static_cast<int16_t>(std::clamp(fx * 65534.0f / s_frame_w - 32767.0f, -32767.0f, 32767.0f));
            s_pointer.y = static_cast<int16_t>(std::clamp(fy * 65534.0f / s_frame_h - 32767.0f, -32767.0f, 32767.0f));
            s_pointer.pressed = true;
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
        else if (Settings::Get("vsync", game.key) == "off")
          schedule += ratio * std::clamp(static_cast<double>(dt) * hz, 0.0, 4.0);  // by the clock: the passes
                                                                                   // don't come with the TV's refreshes
        else
          schedule += ratio;
        // The frames owed, rounded to the nearest: the backlog stays within half a frame either way.
        // (Rounded down, passes of 16.6-16.8 ms against the TV's 16.68 ran 0, 2, 0, 2 frames once the
        // owed fraction sat near 0: God of War at 100% speed with a third of its pictures shown twice.)
        frames = static_cast<int>(std::floor(schedule + 0.5));
        schedule -= frames;
        frames = std::clamp(frames, 0, 10);
        // A core that takes about a refresh per frame or more (God of War: Ghost of Sparta's fights,
        // 17-18 ms; Mario Kart DS's races) can't be caught up: each pass ran more frames and took
        // longer still (4 frames a pass, 72 ms passes: 16 pictures a second at 91% speed). One frame
        // a pass then, every one of them shown; the game runs that much slower, its sound too.
        const double refresh_ms = 1000.0 / std::max(hz, 1.0);
        const bool was_heavy = heavy;
        if (!heavy && core_avg > refresh_ms * 0.92)
          heavy = true;
        else if (heavy && core_avg < refresh_ms * 0.85)
          heavy = false;
        if (heavy != was_heavy)
        {
          LogF("pacing: the core takes %.1f ms a frame: %s", core_avg,
               heavy ? "one frame a pass, the game a little slower" : "caught up again");
        }
        // PSP Frame Rate Auto: about 2 s in a row too slow for 60 (not a loading screen's long frames,
        // nor the first seconds' pipeline compiles): God of War at 30 FPS for the rest of the game.
        if (s_system == SystemId::PSP && !s_psp_30fps_auto && Settings::Get("psp_frame_rate", game.key) == "auto")
        {
          const bool slow = heavy && core_avg < refresh_ms * 2.0 &&
                            std::chrono::duration<double>(Clock::now() - game_start).count() > 15.0;
          psp_heavy_passes = slow ? psp_heavy_passes + 1 : 0;
          if (psp_heavy_passes >= 120)
          {
            s_psp_30fps_auto = true;
            BuildVariables();
            LogF("pacing: the PSP's core takes %.1f ms a frame: God of War at 30 FPS from now on (PSP Frame "
                 "Rate: Auto)", core_avg);
          }
        }
        if (heavy && !forward)
        {
          frames = std::min(frames, 1);
          schedule = std::min(schedule, 0.5);
          // The speed one frame a pass gives (the pass's time against the game's frame).
          const double speed = std::clamp(1000.0 / std::max(fps, 1.0) / std::max(dt * 1000.0, 1.0), 0.5, 1.0);
          s_audio_speed += (speed - s_audio_speed) * 0.05;
        }
        else
        {
          s_audio_speed += (1.0 - s_audio_speed) * 0.05;
        }
      }
      stats.per_pass[static_cast<size_t>(std::min(frames, 3))]++;
      audio.SetMuted(fast && ff != "off");
      // (The PSP's and the Dreamcast's Frame Skip are their cores' own: BuildPspVariables, BuildDcVariables.)
      const std::string skip = s_system == SystemId::PSP || s_system == SystemId::DC ?
                                   std::string("off") :
                                   Settings::Get("frameskip", game.key);
      // The DS: the last picture drawn while these frames run; the PSP and the Dreamcast only while
      // their core is heavy.
      if (s_system == SystemId::PSP || s_system == SystemId::DC)
      {
        const double refresh_ms = 1000.0 / std::max(display.RefreshHz(), 1.0);
        if (!psp_overlap && core_avg > refresh_ms * 0.85)
          psp_overlap = true;
        else if (psp_overlap && core_avg < refresh_ms * 0.75)
          psp_overlap = false;
      }
      const bool overlap = overlap_drawing && frames > 0 && !touring &&
                           (s_system == SystemId::NDS || psp_overlap);
      if (overlap)
      {
        s_defer_frame = true;
        const uint64_t total = stats.total;
        drawer.Post([&draw_pass, total] { draw_pass(total); });
      }
      bool disc_due = false;
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
        core_avg = core_avg <= 0.0 ? run_ms : core_avg * 0.9 + run_ms * 0.1;
        stats.emulated++;
        stats.emulated10++;
        stats.total++;
        stats.run_ms.push_back(run_ms);
        // (The disc goes in after the drawing beside these frames: its toast is the menu's, which
        // that drawing reads.)
        if (disc_lid_open > 0 && --disc_lid_open == 0)
          disc_due = true;
        if (dc_restart_wait > 0)
          dc_restart_wait--;
        dc_since_restart++;
        if (s_shutdown)
          break;
      }
      if (overlap)
      {
        drawer.Wait();
        s_defer_frame = false;
        TakePendingFrame();
        drawn = true;
      }
      if (disc_due)
        insert_disc();
      if (resume_pending && !s_shutdown)
      {
        resume_pending = false;
        if (LoadState(0))
          menu.Toast("Continuing where you left off");
      }
      if (s_shutdown)
      {
        // The game couldn't start (PPSSPP): the menu says why.
        *error = s_message.empty() ? "The emulator couldn't start this game" : s_message;
        s_message.clear();
        result = Result::Failed;
        quit = true;
        break;
      }
      // Flycast's HLE BIOS has no menu for a game to go back to (a game whose lid opened when it didn't
      // ask for another disc - Crazy Taxi; a game's own Exit) and says so every frame: the console starts
      // again then, as after the real BIOS's menu, with the disc in it (scripts/build-dc-core.sh), once
      // the lid is closed. A disc that goes back each time it starts ends the game.
      // (The messages sent before a restart aren't another one - dc_restart_wait -, but an emulation that
      // stopped is: it restarts at once.)
      if (s_dc_reboot)
      {
        s_dc_reboot = false;
        if (dc_restart_wait == 0 || s_dc_stopped)
          dc_reboot_pending = true;
      }
      if (dc_reboot_pending && disc_lid_open == 0 && (dc_restart_wait == 0 || s_dc_stopped))
      {
        dc_restarts = dc_since_restart < 600 ? dc_restarts + 1 : 1;
        dc_since_restart = 0;
        if (dc_restarts > 3)
        {
          *error = "This disc goes back to the Dreamcast's BIOS each time it starts";
          LogF("game: the game went back to the Dreamcast's BIOS %d times in a row: ended", dc_restarts);
          result = Result::Failed;
          quit = true;
          break;
        }
        restart_dc();
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
      if ((s_system == SystemId::N64 || s_system == SystemId::PS1 || s_system == SystemId::PSP || s_system == SystemId::DC ||
           s_system == SystemId::NDS || GpuMode(game.key) != GpuFilter::Mode::Off) &&
          !menu.IsOpen() &&
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
        if (s_system == SystemId::PSP || s_system == SystemId::PS1 || s_system == SystemId::N64 ||
            s_system == SystemId::DC)
        {
          LogF("input: left stick pushed up to %d%% in these 10 s", s_stick_max * 100 / 32767);
          s_stick_max = 0;
        }
        // The GPU filter's own GPU time over the same 10 seconds (timestamps around its passes).
        char filter_time[768];
        if (GpuFilter::TakeGpuTime(filter_time, sizeof(filter_time)))
          LogF("%s", filter_time);
        LogPacing(stats, flip);
        ResetWindow(stats, flip);
      }
    }

    // ---- Drawing ----
    if (!drawn)
      draw_pass(stats.total);
    if (!menu.IsOpen())
    {
      stats.draw_wait_ms += pass_draw_wait;
      stats.draw_ms += pass_draw_ms;
      stats.draws++;
    }
    // V-Sync off: the flips don't wait for the TV; a pass per refresh by the clock, to a deadline
    // that moves on one refresh each pass (a sleep that runs long is made up by the next one; it
    // ran 2% slow, 58.8 passes a second, when each pass slept the rest of its own refresh).
    if (!vsync)
    {
      const double hz = display.RefreshHz() > 1.0 ? display.RefreshHz() : 59.94;
      const auto refresh = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
      next_pass += refresh;
      const auto after = Clock::now();
      if (next_pass + refresh < after)
        next_pass = after;  // far behind (a heavy frame): from now on
      else if (next_pass > after)
        sceKernelUsleep(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(next_pass - after).count()));
    }
    else
    {
      next_pass = Clock::now();
    }
  }

  // ---- Back to the menu ----
  drawer.Stop();
  s_defer_frame = false;
  if (priority_set)
    scePthreadSetprio(scePthreadSelf(), old_priority);
  if (pinned)
    emusuni_set_affinity(scePthreadSelf(), old_mask);
  StopRumble();
  if (motion_on && pads.pads[0].handle >= 0)
    emusuni_pad_set_motion(pads.pads[0].handle, false);
  display.SetVsync(true);
  if (disc_lid_open > 0)
    insert_disc();
  // The disc in now: the game starts with it next time.
  if (DiscCount() > 1 && result != Result::Failed)
    WriteTextFile(Saves::DiscPath(game.key), std::to_string(DiscIn()));
  // (Not while the Dreamcast waits to start again: its machine is stopped inside the BIOS's call. When
  // the Dreamcast's state isn't saved, the one of before goes: Continue would put back its older VMU.)
  const bool resume = Settings::GetOn("resume", game.key);
  const bool saved = resume && result != Result::Failed && !dc_reboot_pending && SaveState(0);
  if (resume && !saved && s_system == SystemId::DC)
    Saves::DeleteState(game.key, 0);
  SaveSram(true);
  if (audio_log >= 1.0f)
    audio.LogHealth(audio_log);
  s_audio = nullptr;
  audio.Stop();
  Upscaler::Stop();
  std::vector<uint16_t>().swap(s_scaled);
  std::vector<uint16_t>().swap(s_converted);
  pads.Close();
  menu.ForgetPictures();
  FreeBezel(display);
  // As RetroArch: the core's Vulkan objects go first (its RDRAM is still there), then the game and
  // the core. The frames that show the core's images are finished before.
  if (s_hw_vulkan && s_hw_render.context_destroy)
  {
    display.WaitIdle();
    display.ForgetOpaqueViews();
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
