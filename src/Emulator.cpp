// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Emulator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <pthread.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <libretro.h>

#include "Achievements.h"
#include "AchievementsSnes.h"
#include "AchievementsUi.h"
#include "Arcade.h"
#include "ArcadeCheats.h"  // ((01.26) FinalBurn Neo's cheats: its options, the player's choices per game)
#include "AssetPack.h"  // (00.90: the built-in bezels are in xmb/assets.zip in the package)
#include "BezelProject.h"
#include "Cheats.h"
#include "Controls.h"
#include "Display.h"
#include "DualScreens.h"
#include "GameAudio.h"
#include "GameDb.h"
#include "GbLink.h"
#include "GpuFilter.h"
#include "Image.h"
#include "IngameMenu.h"
#include "LibretroVulkan.h"
#include "Log.h"
#include "MemoryGuard.h"
#include "Multiplayer.h"
#include "Naomi.h"
#include "Netplay.h"
#include "NetplayLibrary.h"
#include "NetplayUi.h"
#include "MyStats.h"
#include "UsageStats.h"
#include "Pacing.h"
#include "PS4Lang.h"
#include "Portable.h"
#ifdef EMUSUNI_SATURN
#include "SaturnTest.h"
#endif
#include "Saves.h"
#include "Settings.h"
#include "StartGuard.h"
#include "Systems.h"
#ifdef EMUSUNI_3DS
#include "ThreeDsCia.h"  // ((01.09) a game's Mods: On / Off)
#include "ThreeDsTextures.h"  // ((01.42) a game's Texture Pack: On / Off)
#endif
#include "Touchpad.h"
#include "Tour.h"
#include "Upscaler.h"
#ifdef EMUSUNI_VITA
#include "Vita.h"  // (with-vita: the PS Vita's firmware)
#endif
#ifdef EMUSUNI_MUGEN
#include "MugenGo.h"  // (with-mugen: Go's runtime before the first MUGEN game)
#endif
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
#ifdef EMUSUNI_DC
// port/ps4_n64.c: Flycast's threads left where the PS4 puts them, at the priority they start with (the file
// /data/EmuSuNi/dc-threads-free), else placed by emusuni_dc_thread.
extern "C" int emusuni_dc_threads_free;
// scripts/build-dc-core.sh: the console's frame count of the last picture Flycast presented.
extern "C" unsigned long long flycast_emusuni_vblanks(void);
// scripts/build-dc-core.sh (00.69): the console's frames a second the app runs it at (0: no limit), which the
// emulation keeps to while no picture holds it back (a loading screen, a board's BIOS).
extern "C" void flycast_emusuni_pace(double fps);
#endif
#if defined(EMUSUNI_N64) || defined(EMUSUNI_PS1) || defined(EMUSUNI_NDS) || defined(EMUSUNI_PSP) || \
    defined(EMUSUNI_DC) || defined(EMUSUNI_3DS)
// port/ps4_n64.c (linked with every JIT core): the one 32 MiB JIT arena the N64's, PS1's, PSP's, DS's and
// Dreamcast's recompilers share (one core runs at a time), each taking its part as it starts.
extern "C" void emusuni_jit_arena_reset(void);
#endif
// After a core's deinit: the arena's parts back, filled with HLT (a jump into a gone core's code then faults into
// crash.log instead of running it), for the next core.
static void JitArenaReset()
{
#if defined(EMUSUNI_N64) || defined(EMUSUNI_PS1) || defined(EMUSUNI_NDS) || defined(EMUSUNI_PSP) || \
    defined(EMUSUNI_DC) || defined(EMUSUNI_3DS)
  emusuni_jit_arena_reset();
#endif
}

// (01.06) port/ps4_runtime.cpp: an arena of the heap for the calling thread (1: it has one), given back.
extern "C" int ps4_heap_thread_arena(const char* name);
extern "C" void ps4_heap_drop_arena(void);
#ifdef EMUSUNI_3DS
// (01.06) scripts/build-3ds-core.sh: the 3DS's frame handover. The serial of the picture set_image hands over, and the
// wait for its submission (0: made; -1: timeout_ms ran out first), which the drawing does before its own reads it.
// And the core's Vulkan worker's time from that picture's frame end to its submission (ms; -1: not known), which the
// core's frame waited for before the handover: the pacing counts it as the core's.
extern "C" uint64_t emusuni_3ds_image_serial(void);
extern "C" int emusuni_3ds_wait_image(uint64_t serial, uint32_t timeout_ms);
extern "C" double emusuni_3ds_image_worker_ms(uint64_t serial);
// (01.10) port/ps4_n64.c: a 3DS game loads (its GPU commands' and DSP's threads are logged once a game).
extern "C" void emusuni_3ds_game_starts(void);
#endif
#ifdef EMUSUNI_VITA
// (01.72) port/ps4_vita_stats.cpp (the PS Vita core, set 0172): the pictures the game itself showed since the core was
// loaded (its sceDisplaySetFrameBuf calls, not the screen's refreshes, which are 60 whatever the game does). ((01.77,
// the owner: «a ratos se vuelve loco y varía de 30 a 0… como que superpone algunos números») The counter takes it
// once a second, as the other consoles' pictures: 01.72 to 01.75 drew the core's 1-second rate anew every frame, its
// digits changing every refresh.) Global in the core's object from scripts/build-vita-core.sh's APP_GLOBALS on; this
// weak one stands in without it - "not known": the counter says the core's pictures, as for the other consoles.
constexpr unsigned long long VITA_PICTURES_UNKNOWN = ~0ull;
extern "C" __attribute__((weak)) unsigned long long emusuni_vita_pictures_total(void)
{
  return VITA_PICTURES_UNKNOWN;
}
#endif
// (01.78) port/ps4_n64.c: the N64 game's own pictures since the app started (its VI origin's changes; never reset) -
// ParaLLEl-RDP gives one image a VI, 60 a second whatever the game draws: Ocarina of Time shows 20. The counter takes it
// once a second. A weak one stands in without it (a build without that file): "not known", the core's pictures.
constexpr unsigned long long N64_PICTURES_UNKNOWN = ~0ull;
extern "C" __attribute__((weak)) unsigned long long emusuni_n64_pictures_total(void)
{
  return N64_PICTURES_UNKNOWN;
}
#ifdef EMUSUNI_VITA
// ((01.77, the owner: a game at 30 shown 1-2-1-3 refreshes a picture?) How long each of the game's pictures stays on
// the screen: a core run shows the newest picture the game has made (its threads run apart from the runs), so a
// picture first shows at the run its count grew and stays until the next one grows it - 1, 2, 3 or 4+ runs (one a
// refresh) -, and a run in which it grew by 2 or more left pictures never shown. For the pacing line every 10 s.)
struct VitaCadence
{
  unsigned long long seen = VITA_PICTURES_UNKNOWN;
  int runs = 0;  // the runs the newest picture has been shown so far
  std::array<int, 4> held{};
  int skipped = 0;

  void Run()
  {
    const unsigned long long total = emusuni_vita_pictures_total();
    if (total == VITA_PICTURES_UNKNOWN)
      return;
    if (seen != VITA_PICTURES_UNKNOWN && total > seen)
    {
      if (runs > 0)
        held[std::min(runs, 4) - 1]++;
      skipped += static_cast<int>(std::min<unsigned long long>(total - seen - 1, 1000));
      runs = 0;
    }
    seen = total;
    runs++;
  }
  // The window's line, and a new window.
  void Log()
  {
    if (seen == VITA_PICTURES_UNKNOWN)
      return;
    LogF("pacing: the PS Vita game's pictures shown 1, 2, 3, 4+ refreshes each: %d, %d, %d, %d; %d never shown",
         held[0], held[1], held[2], held[3], skipped);
    held = {};
    skipped = 0;
  }
};
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
// A core setting one of its own options (FinalBurn Neo resets its cheats so at every start): the same
// newer libretro.h.
#ifndef RETRO_ENVIRONMENT_SET_VARIABLE
#define RETRO_ENVIRONMENT_SET_VARIABLE 70
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

// A core that stops at an option it is given but never declared (Flycast: verify, DEBUGBREAK; Azahar is kept to
// its own 32 as well, so that the PC test of Build3dsVariables says all there is): it is given only its own.
bool StrictOptions()
{
  return IsFlycast(s_system) || s_system == SystemId::THREEDS;
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
// (01.66, the owner: "la pantalla de carga inicial ... hazla más bonita") The PS Vita's start, drawn by the app: while
// Vita3K prepares a game it hands its pictures over by the CPU (a black one with a grey bar: not shown), the game's by
// the GPU - the first of those ends it. Its steps come as messages ("Installing the title...", in English, when they
// change): the screen's line, in the player's language, not a toast.
bool s_vita_loading = false;
std::string s_vita_step;
#ifdef EMUSUNI_VITA
// (01.67, the owner: "el touch aún no funciona bien (¿no puedes traducir el movimiento a la palanca?)") The PS Vita's
// pointer: L3's short press turns it on or off (held, L3 is the PS TV's as before; with R3, the in-game menu). On, the
// left stick moves a cursor over the Vita's screen and Cross touches there - held, a drag -: neither reaches the game.
// The cursor is drawn where it is, and where a finger is on the touchpad too. (x, y: 0..1 over the Vita's screen.)
struct VitaPointer
{
  bool on = false, down = false, shown = false, hinted = false;
  bool touching = false;  // ((01.69) touching the screen this frame: the touchpad's or the pointer's)
  int recent = 0;         // ((01.69) frames the cursor still shows after the touchpad or a touch)
  float x = 0.5f, y = 0.5f;
};
VitaPointer s_vita_pointer;
#endif
// (01.66) The game's cover for its start (the menu's index has it: SetStartCover), taken once by that game's Run.
std::string s_start_cover_game, s_start_cover_path;
// (00.81) What SwanStation's log says as a PlayStation game loads (CoreLog): the disc image couldn't be opened (its
// reason; the core then starts the BIOS without a disc: a black screen), a LibCrypt-protected disc without its .sbi
// (started anyway: scripts/build-ps1-core.sh).
std::string s_ps1_disc_error;
bool s_ps1_libcrypt = false;
bool s_ps1_openbios = false;  // (00.90) no BIOS file found: the core's built-in OpenBIOS starts the game
bool s_ps1_cooked = false;    // (00.90) a disc image of 2048-byte sectors: its films' and voices' sectors are lost
// (00.23) Local multiplayer (src/Multiplayer.h): what decides the game's adapter for 3 or more players (read as it
// starts; a PlayStation disc's code from the core's log as it loads, CoreLog), the adapter in its ports now, and
// whether FCEUmm's expansion port got the Famicom's 4 player adapter in this session (the core keeps it from one game
// to the next).
Multiplayer::Game s_mp_game;
Multiplayer::Adapter s_adapter = Multiplayer::Adapter::None;
bool s_nes_expansion_set = false;
double s_game_fps = 60.0988;
// A core that draws with Vulkan itself (ParaLLEl-RDP for the N64): libretro's Vulkan interface
// over the display's device; the image it hands over for the frame (set_image).
Display* s_display = nullptr;
retro_hw_render_callback s_hw_render = {};
bool s_hw_vulkan = false;
// The DS's and the 3DS's two screens (00.80, src/DualScreens.h): each core gives them side by side, always the same
// picture, and the app places them as the screens' settings say (DrawScreens). R3 swaps them for as long as the game
// runs (s_r3_swap: Swap Screens the other way; read by the drawing, which is the DS's own thread: atomic), until those
// settings change in the in-game menu (s_screens_seen). Until 00.79 the DS's halves were swapped as the picture came
// in and the 3DS's core swapped its own.
std::atomic<bool> s_r3_swap{false};
bool s_nds_r3 = false;
std::string s_screens_seen;
// The 3DS (Azahar): its resolution (1x) and New 3DS Mode count as the game started (Azahar reads them once; 0 and -1
// until Build3dsVariables sets them).
bool s_3ds_r3 = false;
// How long R3 has been held, and was at the pass before (the DS's and the 3DS's swap when the in-game menu opens with
// R3 held: the swap is a shorter press, made as R3 is let go; 00.68).
float s_r3_held = 0.0f, s_r3_was = 0.0f;
// (01.31, the owner: "en 3ds y ds se use el l3 para cambiar entre las disposiciones de pantallas") L3 on the DS and the
// 3DS: a short press shows the next Screen Layout (TakeL3Press, NextScreenLayout); held from L3_HOLD on, it is what
// Controls puts on L3 (the DS's microphone, the 3DS's SELECT) for as long as it is held. With R3 (the in-game menu's
// L3 + R3) no layout changes.
constexpr float L3_HOLD = 0.35f;
struct L3Press
{
  bool down = false;     // held at the pass before
  float held = 0.0f;     // for how long
  bool through = false;  // held long enough: the game's now
  bool menu = false;     // R3 too: the in-game menu's, not a layout
};
L3Press s_l3;
int s_3ds_scale = 0, s_3ds_new = -1;
// The left stick's furthest push since the last speed line (the log: the PSP's stick, 00.40).
int s_stick_max = 0;
// The DS's or the 3DS's touchpad in the last 10 s (the log): frames with a finger on it, frames with the stylus down,
// taps (Touchpad.h).
int s_touch_frames = 0, s_tap_frames = 0, s_touch_taps = 0;
// (01.24) With 3DS Right Stick: D-Pad, the frames in the last 10 s its directions pressed the 3DS's D-pad (the log).
int s_stick_dpad_frames = 0;
// The PSP's resolution and multisampling: as the settings say, also while the game runs (since 00.62 the
// core makes its swapchain and framebuffers again: scripts/build-psp-core.sh); with the file
// data_dir/psp-resolution-at-start, as the game started (00.39-00.61: a change while it ran zoomed in).
int s_psp_scale = 0;
std::string s_psp_msaa;
bool s_psp_at_start = false;
// Region as the game started, for Genesis Plus GX (Region: next game start): the core changes a
// running game's timing, and the Game Gear's without telling the new frame rate.
std::string s_gpgx_region;
// The Dreamcast's SH4 clock (MHz) as the game started (Dreamcast CPU Clock: next game start, BuildDcVariables).
std::string s_dc_clock;
// Its picture's size and texture sharpness as the game started (Quality, Dreamcast Resolution and Texture Sharpness,
// 00.68: next game start too, BuildDcVariables).
std::string s_dc_resolution, s_dc_anisotropy;
// A Dreamcast game's threads left to the PS4 - any core, the priority they start with - while the file
// /data/EmuSuNi/dc-threads-free exists (an A/B for the PS4: Resonance places none): retro_run's (the
// renderer), the drawing beside it and Flycast's own (port/ps4_n64.c). Else the app places them.
bool s_dc_threads_free = false;
// The Arcade (FinalBurn Neo): the picture's rotation (SET_ROTATION: a vertical game's is turned by the
// app as it comes in, Arcade::Rotate, into s_rotated) and the shape the core reports for it (its
// geometry: a board's 4:3, a vertical one's 3:4), for Aspect Ratio's Auto.
unsigned s_rotation = 0;
std::vector<uint16_t> s_rotated;
float s_core_aspect = 0.0f;
// The NAOMI and the Atomiswave (Flycast's arcade boards; a vertical game's picture is the core's image, turned by the
// app as it draws it: DrawPicture). The game's set (src/Naomi.h: its analog controls, its light gun), its TEST or
// SERVICE button held until `s_board_press_until` (the in-game menu's Arcade Button: RetroPad L3 / R3, which the
// core reads as those buttons) and each player's Gun Reload (RETRO_DEVICE_LIGHTGUN's reload).
const Naomi::Set* s_board_set = nullptr;
std::chrono::steady_clock::time_point s_board_press_until{};
unsigned s_board_press_id = 0;
std::array<bool, MAX_PLAYERS> s_gun_reload{};

// The geometry the core reports (get_system_av_info once the game is loaded; SET_GEOMETRY and
// SET_SYSTEM_AV_INFO when a board changes its picture's size): an arcade board's shape. The other
// consoles' pictures are shaped by the app.
void NoteGeometry(const retro_game_geometry& geometry)
{
  if (s_system == SystemId::ARCADE && geometry.aspect_ratio > 0.0f)
    s_core_aspect = geometry.aspect_ratio;
}
// PSP Frame Rate Auto: God of War at 30 FPS (PPSSPP's hack) once its core couldn't keep up, until the
// game ends.
bool s_psp_30fps_auto = false;
// (01.05) The game is one of the God of War games, the only ones the hack acts on (compat.ini's GoWFramerateHack60, as
// the core says when the game boots: CoreLog).
bool s_psp_gow = false;
// (01.07, the master's idea) PPSSPP stopped the game itself ("Bad memory access detected! ... Stopping emulation.",
// Core/MemFault.cpp: a fault it doesn't ignore - the core's log, on its threads): the player is told to leave with the
// menu instead of a frozen picture with no word (said until the menu opens, again after it closes).
std::atomic<bool> s_psp_stopped{false};
// The PSP core's count of vblanks, given at the end of each run by its private environment call (00.81,
// scripts/build-psp-core.sh): the pacing pays for those a run covered (PspFrames).
constexpr unsigned EMUSUNI_ENVIRONMENT_PSP_VBLANKS = 0x20000 | 0x5053;
unsigned long long s_psp_vblanks = 0;
bool s_psp_vblanks_known = false;
bool s_shutdown = false;  // the core asked to stop (RETRO_ENVIRONMENT_SHUTDOWN)
// Flycast's HLE BIOS said "Reboot to BIOS" (a game went back to the BIOS, which it can't show): Run()
// starts the console again; its RETRO_ENVIRONMENT_SHUTDOWN then isn't the game's end, but says the
// emulation stopped (s_dc_stopped: no waiting before the restart then).
bool s_dc_reboot = false, s_dc_stopped = false;
retro_hw_render_interface_vulkan s_vulkan = {};
retro_vulkan_image s_hw_image = {};
bool s_hw_image_set = false;
// (01.06) The 3DS's frame handover on (EMUSUNI_3DS_ASYNC_HANDOFF given to the core), and the serial of the picture in
// s_hw_image: the drawing waits for its submission before its own (emusuni_3ds_wait_image).
bool s_3ds_handover = false;
uint64_t s_hw_image_serial = 0;
bool s_frame_hw = false;  // the newest frame is the core's image
// (01.72, the master: «investiga eso de los fps para todas las consolas») The pictures the core gave (VideoRefresh with
// one: not a duplicated frame's NULL - the PSP's, MUGEN's while it loads -, nor a rollback's frames run again): the
// counter's FPS, the game's own pictures a second (Flycast's 30 in Jet Set Radio, a slow game's), which the TV's 60
// refreshes hid.
uint64_t s_pictures = 0;
// (00.85) The game's picture as the screen showed it when the in-game menu opened, for a core that draws its own image
// (no pixels of it here: s_frame is NULL): its states' thumbnails (SaveState). Empty: none (a core with pixels).
ImageRGBA s_menu_picture;
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

// (00.23) The game's adapter for 3 or more players (Multiplayer Adapter, src/Multiplayer.h) into its ports, after the
// app's own devices for the first two, and in the log. `before`: the adapter the core had (none at the game's start).
void PlugAdapter(const Multiplayer::Decision& decision, Multiplayer::Adapter before, const char* when)
{
  s_adapter = decision.adapter;
  for (const auto& [port, device] : Multiplayer::PortDevices(s_mp_game, decision.adapter, before, s_nes_expansion_set))
    s_core->set_controller_port_device(port, device);
  if (s_system == SystemId::NES)
    s_nes_expansion_set = decision.adapter == Multiplayer::Adapter::Famicom4P;
  LogF("players: %s: %s, up to %d players (%s)", when, Multiplayer::Name(decision.adapter),
       Multiplayer::MaxPlayers(decision.adapter), decision.reason.c_str());
}
// ---- The Game Boys ----
// The Game Boy game's cartridge header: made for the Game Boy Color (0x143), with Super Game Boy
// features (0x146).
bool s_gb_color_game = false, s_gb_sgb_game = false;
// (00.95) The Game Boy link cable online (src/GbLink.h): this game is a link cable match's - SameBoy's two Game Boys,
// GbLink::Get()'s games -; the second game's ROM (Game Boy 2's) until the core has it; this console's Game Boy's
// battery save (its RAM, then its clock's 48 bytes) as last written; a guest: whether its own save came in the host's
// state (it writes nothing until then), and whether it didn't (the match ends: s_gblink_failed).
#ifdef EMUSUNI_GBLINK
extern "C" size_t sameboy_emusuni_battery_size(unsigned gb);
extern "C" size_t sameboy_emusuni_ram_size(unsigned gb);
extern "C" bool sameboy_emusuni_save_battery(unsigned gb, uint8_t* out, size_t size);
extern "C" void sameboy_emusuni_load_battery(unsigned gb, const uint8_t* data, size_t size);
#endif
#ifdef EMUSUNI_GBALINK
extern "C" size_t gbalink_emusuni_battery_size(unsigned gba);
extern "C" size_t gbalink_emusuni_ram_size(unsigned gba);
extern "C" bool gbalink_emusuni_save_battery(unsigned gba, uint8_t* out, size_t size);
extern "C" void gbalink_emusuni_load_battery(unsigned gba, const uint8_t* data, size_t size);
#endif
// (01.04) The link match's core's battery saves: SameBoy's (two Game Boys) or mGBA's link core's (two GBAs).
struct LinkSaves
{
  size_t (*battery_size)(unsigned);
  size_t (*ram_size)(unsigned);
  bool (*save)(unsigned, uint8_t*, size_t);
  void (*load)(unsigned, const uint8_t*, size_t);
};
#ifdef EMUSUNI_GBLINK
const LinkSaves SAMEBOY_SAVES = {sameboy_emusuni_battery_size, sameboy_emusuni_ram_size, sameboy_emusuni_save_battery,
                                 sameboy_emusuni_load_battery};
#endif
#ifdef EMUSUNI_GBALINK
const LinkSaves GBALINK_SAVES = {gbalink_emusuni_battery_size, gbalink_emusuni_ram_size, gbalink_emusuni_save_battery,
                                 gbalink_emusuni_load_battery};
#endif
const LinkSaves* s_link_saves = nullptr;
bool s_gblink = false;
std::vector<uint8_t> s_gblink_rom2;
std::vector<uint8_t> s_gblink_saved;
bool s_gblink_checked = false, s_gblink_failed = false;
// The N64 cartridge's name in its header ("GOLDENEYE"): N64 CPU Timing's Auto. Its game code (two letters, the same
// for every region and revision: "CU" is Cruis'n USA): the games that need N64 Graphics Sync Accurate.
std::string s_n64_name, s_n64_code;
// (00.81) Its header's two CRCs (0x10, 0x14): the core's game list (mupen64plus.ini) knows it by them too, and the
// app's corrections to that list (GameDb::FindN64Correction).
uint32_t s_n64_crc1 = 0, s_n64_crc2 = 0;
// (01.58) N64 Widescreen on for this game (N64Widescreen): the picture shown 16:9 (DrawPicture's ratio).
bool s_n64_widescreen = false;

// An N64 ROM's header (0x40 bytes) in the cartridge's byte order, whatever the file's (.z64 as the cartridge, .v64
// with its bytes swapped in pairs, .n64 in fours); zeros for a file too short.
std::array<uint8_t, 0x40> N64Header(const std::vector<uint8_t>& rom)
{
  std::array<uint8_t, 0x40> header{};
  if (rom.size() < header.size())
    return header;
  std::memcpy(header.data(), rom.data(), header.size());
  if (header[0] == 0x37 && header[1] == 0x80)
  {
    for (size_t i = 0; i < header.size(); i += 2)
      std::swap(header[i], header[i + 1]);
  }
  else if (header[0] == 0x40 && header[1] == 0x12)
  {
    for (size_t i = 0; i < header.size(); i += 4)
    {
      std::swap(header[i], header[i + 3]);
      std::swap(header[i + 1], header[i + 2]);
    }
  }
  return header;
}

// The header's CRC at `offset` (0x10 or 0x14, big-endian).
uint32_t N64Crc(const std::vector<uint8_t>& rom, size_t offset)
{
  const std::array<uint8_t, 0x40> header = N64Header(rom);
  return (uint32_t(header[offset]) << 24) | (uint32_t(header[offset + 1]) << 16) | (uint32_t(header[offset + 2]) << 8) |
         uint32_t(header[offset + 3]);
}

// The game code in an N64 ROM's header (0x3C, two letters or digits: "CU"), else "".
std::string N64Code(const std::vector<uint8_t>& rom)
{
  const std::array<uint8_t, 0x40> header = N64Header(rom);
  std::string code(reinterpret_cast<const char*>(header.data() + 0x3C), 2);
  for (const char c : code)
  {
    if (!std::isalnum(static_cast<unsigned char>(c)))
      return {};
  }
  return code;
}

// N64 Graphics Sync Fast lets ParaLLEl-RDP draw beside the N64: the RDP's "done" interrupt comes at once, before the
// GPU has drawn, and what it drew reaches the N64's memory a frame later (the PS4's RDRAM is a copy on the GPU: no
// VK_EXT_external_memory_host). A game whose own code reads its pictures back or writes where the RDP drew then waits
// for ever or loses what it wrote: Cruis'n USA stays black after its Controller Pak notice (its transitions shatter
// the picture its CPU reads back), Resident Evil 2 after its language screen (GitHub issue #1; both on the PC with the
// app's options: black with Fast, playing with Accurate). These games play Accurate whatever the setting says.
// 00.72 (a tester's report on v00.21): Resident Evil 2 black after the Expansion Pak and Nintendo 64 screens - the
// American cartridge is NREE, the European NREP ("RE" both: 00.68 covers them), the Japanese Biohazard 2 NB5J (added);
// Ogre Battle 64 (NOBE, NOBJ) with its 2D backgrounds broken, misplaced or swapped "at random": pictures its RSP decodes
// into the N64's memory (its JPEG task) where the RDP drew a frame before, which the GPU's late copy of that frame then
// overwrites - a race that Accurate closes (no cartridge of it on the PC to try).
struct N64AccurateTitle
{
  const char *code, *name, *why;
};
const N64AccurateTitle* N64AccurateGame(const std::string& code)
{
  static const N64AccurateTitle GAMES[] = {
      {"CU", "Cruis'n USA", "with Fast it stays on a black screen: its effects read the picture back"},
      {"RE", "Resident Evil 2", "with Fast it stays on a black screen: its effects read the picture back"},
      {"B5", "Biohazard 2", "with Fast it stays on a black screen: its effects read the picture back"},
      {"OB", "Ogre Battle 64",
       "with Fast its 2D backgrounds break: the GPU's late copy of a frame overwrites the pictures decoded there"},
  };
  for (const N64AccurateTitle& game : GAMES)
  {
    if (code == game.code)
      return &game;
  }
  return nullptr;
}

// (01.58, the owner) N64 Widescreen, "16:9 (experimental)": the games whose 3D is made 16:9 by a few words of their own
// code (the aspect they give their perspective, 4:3, made 16:9), written by the core at every VI where that code is
// (scripts/build-n64-core.sh: the option mupen64plus-emusuni-ram-patches). Worked out here from each game's own code
// (no one else's cheat codes), each checked on the PC with ParaLLEl-RDP (the edges of the screen, the HUD). The picture
// is then shown 16:9 (DrawPicture's ratio); the 2D (the HUD, the menus) stays as drawn and looks wider. Off, the same
// words are given the other way round: a state saved with it on is put back. A game by its header's CRCs (one version).
struct N64WidescreenTitle
{
  uint32_t crc1, crc2;
  const char* name;
  const char* patches;  // "ADDRESS:EXPECTED:VALUE ..." (hex, 32-bit words of the N64's memory)
};
const N64WidescreenTitle* N64Widescreen(uint32_t crc1, uint32_t crc2)
{
  // The Legend of Zelda: Ocarina of Time (USA, Rev 2: "NTSC 1.2"): its guPerspective (0x800D0DB0, libultra's, which
  // every 3D view calls - the playing field, the cutscenes, the pause menu's pages) passes on its aspect argument with
  // mfc1 a3,f14 after a mfc1 a2,f12 that changes nothing (a2 still holds the field of view): those two words become
  // lui a3,0x3FE3 / ori a3,a3,0x8E39, the aspect 16:9 (1.7778) whatever the caller gave (4:3: 320x240 views, 4/3
  // constants). The actors' culling follows (it projects with the same matrix). PC, 01.58: the Hyrule Field intro, the
  // Kokiri Forest flight, Link's house with the HUD and the pause menu - more of the world at the sides, an actor
  // there drawn, the HUD and the pause menu's buttons wider (ovals).
  // The owner's European cartridges: the aspect each caller gives multiplied by 4/3 (1.3359, a lui alone), so a split
  // screen's views (Mario Kart 64's two players: 8:3 each) widen too. In the same libultra guPerspective the two moves
  // to f12 and back (a2) and the move back of the aspect become lui at,0x3FAB / mtc1 at,f12 / mul.s f12,f14,f12, and
  // guPerspectiveF (called from there only) keeps f12 instead of a3 where it reads the aspect back (sw a3 -> swc1 f12).
  // Super Mario 64 also culls its objects by a fixed angle (obj_is_in_view: fov * 0.5 + 1 degree, no aspect): 0.5
  // becomes 0.664, about 4/3 more width at its usual 45 degrees, else the far ones vanish in the new sides. PC, 01.58:
  // SM64's title and its demo (with the HUD; objects at the sides drawn only with the culling word); Mario Kart 64's
  // title, a one-player demo race and the two-player split screen (both views widened, karts in shape; no piece of the
  // track seen missing); Majora's Mask's Clock Town intro (a character at the side drawn); Banjo-Kazooie's intro and
  // the title's Spiral Mountain flight (Rare's own guPerspectiveF: the aspect at 68(sp), read once). Super Smash Bros.
  // reaches its perspective another way (not done); GoldenEye 007 has its own 16:9 (Options: Ratio 16:9 and the app's
  // Aspect Ratio 16:9).
  static const N64WidescreenTitle GAMES[] = {
      {0x693BA2AE, 0xB7F14E9F, "The Legend of Zelda: Ocarina of Time (USA, Rev 2)",
       "000D0DD0:44066000:3C073FE3 000D0DD4:44077000:34E78E39"},
      {0xA03CF036, 0xBCC1C5D2, "Super Mario 64 (Europe)",
       "002F1D84:44866000:3C013FAB 002F1DA0:44066000:44816000 002F1DA4:44077000:460C7302 002F1B5C:AFA7003C:E7AC003C "
       "0026D440:3C013F00:3C013F2A"},
      {0xC3B6DE9D, 0x65D2DE76, "Mario Kart 64 (Europe)",
       "000CF3C4:44866000:3C013FAB 000CF3E0:44066000:44816000 000CF3E4:44077000:460C7302 000CF19C:AFA7003C:E7AC003C"},
      {0xE97955C6, 0xBC338D38, "The Legend of Zelda: Majora's Mask (Europe)",
       "0008ACF4:44866000:3C013FAB 0008AD10:44066000:44816000 0008AD14:44077000:460C7302 0008AACC:AFA7003C:E7AC003C"},
      {0x733FCCB1, 0x444892F9, "Banjo-Kazooie (Europe)",
       "00246E08:44866000:3C013FAB 00246E24:44066000:44816000 00246E28:44077000:460C7302 00246B0C:AFA70044:E7AC0044"},
  };
  for (const N64WidescreenTitle& game : GAMES)
  {
    if (game.name && crc1 == game.crc1 && crc2 == game.crc2)
      return &game;
  }
  return nullptr;
}

// The core's option for a listed game: its words to write (on), or to put back (off).
std::string N64WidescreenPatches(const N64WidescreenTitle& game, bool on)
{
  if (on)
    return game.patches;
  std::string back;
  unsigned address = 0, expected = 0, value = 0;
  int used = 0;
  for (const char* text = game.patches; std::sscanf(text, " %x:%x:%x%n", &address, &expected, &value, &used) == 3;
       text += used)
  {
    char entry[40];
    std::snprintf(entry, sizeof(entry), "%s%08X:%08X:%08X", back.empty() ? "" : " ", address, value, expected);
    back += entry;
  }
  return back;
}

// The name in an N64 ROM's header (0x20, 20 characters).
std::string N64Name(const std::vector<uint8_t>& rom)
{
  const std::array<uint8_t, 0x40> header = N64Header(rom);
  std::string name(reinterpret_cast<const char*>(header.data() + 0x20), 20);
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

// (01.27) The 3DS's motion sensors with 3DS Motion Sensor: DualShock 4 (src/Settings.cpp 3ds_motion): the controller's
// accelerometer and gyroscope are the 3DS's, held as a 3DS is held (the controller's face is the lower screen's).
// Azahar's libretro core reads them through the sensor interface only with its citra_enable_motion on
// (Build3dsVariables), in the 3DS's own axes: the acceleration in G, the turn in rad/s (citra_libretro/input/
// input_factory.cpp LibRetroMotion). The 3DS's axes are the controller's with y the other way, as Azahar's own SDL
// input turns a controller's (input_common/sdl/sdl_impl.cpp): x, -y, z for the acceleration, -x, y, -z for the turn.
// The game loop writes them, the core's emulation thread reads them: each value on its own.
struct Motion3ds
{
  bool on = false;    // for this game, as it started
  bool said = false;  // its first reading logged
  std::atomic<float> axes[6] = {0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f};  // libretro's 6 ids; lying flat at first
};
Motion3ds s_motion3ds;
#ifdef EMUSUNI_VITA
// (01.88) The PS Vita's motion sensors with PS Vita Motion Sensor: DualShock 4 (src/Settings.cpp vita_motion; the
// owner: "prefiero que vaya en ajustes de la consola"): the DualShock 4's accelerometer and gyroscope are the Vita's,
// the controller's face as the Vita's screen (Rayman's tilt). Vita3K's libretro core (libretro_input.cpp) takes RetroArch's readings - m/s^2 and
// rad/s, turned into G (over -g) and turns a second (over 2 pi) - and, unlike its SDL input, turns no axes: they are
// given here in the axes it gives a controller (motion.cpp: SDL's x, -z, y). ScePadData's acceleration and angular
// velocity are in SDL's axes (see Motion3ds). The 6 values: libretro's ids.
Motion3ds s_motion_vita;
#endif

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
#ifdef EMUSUNI_VITA
  // (01.88) The PS Vita's motion sensors (vita_motion): ((01.91) lying still while off - BuildVitaVariables' -, so
  // the setting changes while the game plays.)
  if (s_system == SystemId::VITA)
    return id < 6 ? s_motion_vita.axes[id].load(std::memory_order_relaxed) : 0.0f;
#endif
  if (s_system == SystemId::THREEDS)  // (01.27) 3DS Motion Sensor
    return s_motion3ds.on && id < 6 ? s_motion3ds.axes[id].load(std::memory_order_relaxed) : 0.0f;
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
// The DS's stylus as the app moves it (src/Touchpad.h: the touchpad as a trackpad, a tap touches; or Direct): where it
// is on the touch screen (its 256x192 pixels; it stays where the finger or the right stick left it) and whether it is
// down.
Touchpad::Stylus s_stylus;
// (00.90) The Jaguar's number pad on player 1's touchpad: the key under the finger (0-11: 1 2 3 / 4 5 6 / 7 8 9 /
// * 0 #, as on the Jaguar's pad), -1 none; what Virtual Jaguar reads as the keyboard's keys (InputState).
int s_jaguar_key = -1;
constexpr unsigned JAGUAR_KEYS[12] = {RETROK_1, RETROK_2, RETROK_3, RETROK_4, RETROK_5,     RETROK_6,
                                      RETROK_7, RETROK_8, RETROK_9, RETROK_MINUS, RETROK_0, RETROK_EQUALS};
// The 3DS's stylus is the same on the touch screen's 320x240 pixels; the cursor the app draws at it (frames it still
// shows after the finger left, and where in the core's picture, in its pixels).
int s_3ds_cursor = 0;
std::array<float, 2> s_3ds_at{};

// (01.15) The touches on player 1's touchpad (up to two), for Touchpad::Frame: it follows the finger by its number and
// leaves a touch resting on the pad alone (until 01.14 only the pad's first touch was read: with one resting there,
// the stylus never moved). Returns how many.
int PadTouches(const OrbisPadTouchData& touch, bool live, Touchpad::Touch (&out)[2])
{
  const int count = live ? std::min<int>(touch.fingers, 2) : 0;
  for (int i = 0; i < count; i++)
    out[i] = {static_cast<float>(touch.touch[i].x), static_cast<float>(touch.touch[i].y), touch.touch[i].finger};
  return count;
}

// The right stick moves the stylus (the DS; the 3DS with 3DS Right Stick: Stylus): up to a 64th of the touch screen's
// width a frame past the stick's dead zone (the DS's 4 pixels, as until 01.14). True: it moved.
bool StickMovesStylus(const OrbisPadData& data, float screen_w, float screen_h)
{
  const float sx = (static_cast<int>(data.rightStick.x) - 128) / 128.0f;
  const float sy = (static_cast<int>(data.rightStick.y) - 128) / 128.0f;
  const float len = std::sqrt(sx * sx + sy * sy);
  if (len <= 0.2f)
    return false;
  const float speed = screen_w / 64.0f * std::min(1.0f, (len - 0.2f) / 0.8f) / len;
  s_stylus.x = std::clamp(s_stylus.x + sx * speed, 0.0f, screen_w - 1.0f);
  s_stylus.y = std::clamp(s_stylus.y + sy * speed, 0.0f, screen_h - 1.0f);
  return true;
}

// (01.24) 3DS Right Stick: D-Pad - a stick's directions as the D-pad's buttons (libretro's bits, as s_buttons has
// them): each axis past the same dead zone as the D-pad's Left Stick (Controls::PadButtons: 50 of 128), both of them
// a diagonal. Pushed all the way, the eight directions get about the same share of the stick's circle.
uint16_t StickDpad(uint8_t stick_x, uint8_t stick_y)
{
  constexpr int DEAD = 50;
  const int x = static_cast<int>(stick_x) - 128, y = static_cast<int>(stick_y) - 128;
  uint16_t out = 0;
  if (y < -DEAD)
    out |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
  if (y > DEAD)
    out |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (x < -DEAD)
    out |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (x > DEAD)
    out |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  return out;
}

// The log's note of a touch left alone as resting in these 10 s (Touchpad::Pick), its count started again; "" none.
std::string RestingTouchNote()
{
  if (s_stylus.rested == 0)
    return "";
  char note[128];
  std::snprintf(note, sizeof(note), "; a touch resting on the pad left alone %d frames (at %.0f,%.0f of 1920x942)",
                s_stylus.rested, s_stylus.rest_x, s_stylus.rest_y);
  s_stylus.rested = 0;
  return note;
}
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
  uint64_t image_serial = 0;  // (01.06: the 3DS's handover)
};
PendingFrame s_pending;

void TakePendingFrame()
{
  if (s_pending.image_set)
  {
    s_hw_image = s_pending.image;
    s_hw_image_set = true;
    s_hw_image_serial = s_pending.image_serial;
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

// (01.09, the 3DS's specialist: the app closed by itself at DKC's second reset in 01.08) The 3DS makes its renderer
// again in a reset (Azahar's retro_reset: System::Shutdown and a new load) and in a state's load (its serialize:
// Shutdown(true) and Init): the images it handed over with set_image - their views, images and memory - are gone. The
// app drew the one before until the first new one (~200 ms: ExternalTexture over a destroyed view), and once the GPU
// read freed memory: its fault, the app killed with no crash.log. Now, around those calls (ThreedsRemaking):
//  - before: the GPU done with the frames in flight, which may still read those images (Display::WaitIdle);
//  - after: the images forgotten - none drawn, no handover waited for, until the core hands over a new one;
//  - the passes after it (THREEDS_REMADE_PASSES) say in the log - and boot-trace.log, synced, which a kill leaves -
//    the picture each draws, and whether its view is one of the last ones of before (s_3ds_views).
constexpr int THREEDS_REMADE_PASSES = 20;
VkImageView s_3ds_views[3] = {};         // the last views the core handed over (one each)
VkImageView s_3ds_views_before[3] = {};  // those when its renderer was made again
int s_3ds_views_next = 0;
int s_3ds_remade_passes = 0;             // the passes still to say
const char* s_3ds_remade_by = "";

void ThreedsRememberView(VkImageView view)
{
  for (VkImageView seen : s_3ds_views)
  {
    if (seen == view)
      return;
  }
  s_3ds_views[s_3ds_views_next] = view;
  s_3ds_views_next = (s_3ds_views_next + 1) % 3;
}

// `remake`: the core's call that makes its renderer again (a reset, a state's load); `what`: said in the log.
template <typename Call>
auto ThreedsRemaking(const char* what, Call&& remake)
{
  if (s_display)
    s_display->WaitIdle();
  const uint64_t serial = s_hw_image_serial;
  auto result = remake();
  std::copy(std::begin(s_3ds_views), std::end(s_3ds_views), std::begin(s_3ds_views_before));
  s_hw_image_set = false;
  s_hw_image_serial = 0;
  s_pending.image_set = false;
  s_pending.semaphores.clear();
  s_pending.image_serial = 0;
  s_3ds_remade_passes = THREEDS_REMADE_PASSES;
  s_3ds_remade_by = what;
  LogF("3ds: %s made its renderer again: its pictures (the last %llu) not drawn until a new one", what,
       static_cast<unsigned long long>(serial));
  LogSync();
  return result;
}

// Each drawing of a pass after it (DrawPicture): what it draws, said and synced.
void ThreedsRemadeTrace()
{
  if (s_3ds_remade_passes <= 0)
    return;
  const int pass = THREEDS_REMADE_PASSES - --s_3ds_remade_passes;
  if (!s_hw_image_set)
  {
    LogF("3ds: after the %s, drawing %d: no picture yet", s_3ds_remade_by, pass);
  }
  else
  {
    bool before = false;
    for (VkImageView view : s_3ds_views_before)
      before = before || (view != VK_NULL_HANDLE && view == s_hw_image.image_view);
    LogF("3ds: after the %s, drawing %d: picture %llu (view %p)%s", s_3ds_remade_by, pass,
         static_cast<unsigned long long>(s_hw_image_serial), reinterpret_cast<const void*>(s_hw_image.image_view),
         before ? ": a view of before (the same handle - made again, or the old one)" : "");
  }
  LogSync();
}
// Resolution: the picture made bigger, and the part of the game texture the picture fills.
std::vector<uint16_t> s_scaled;
uint32_t s_texture_w = 0, s_texture_h = 0;
bool s_texture_scaled = false;
double s_scale_ms = 0.0;  // time spent making pictures bigger (for the log)
int s_scale_frames = 0;
std::string s_scale_name;
GameAudio* s_audio = nullptr;
// The core's sample rate: the sound is always resampled from it (its pitch). A game below full speed (one frame a
// pass) sends less sound than is played: GameAudio stretches it at the same pitch. (00.39 to 00.65 resampled it from
// the rate times the game's speed: a game at 75% sounded 5 semitones deep.)
double s_sample_rate = 32040.0;

void CoreLog(enum retro_log_level level, const char* format, ...)
{
  if (level < RETRO_LOG_INFO)
    return;
  // (01.69) 2048, not 512: the 3DS core's 600-frame lines are longer than that and their last counts ("held pages")
  // never reached the log (01.68's console test). Log.cpp's LogF takes the whole line.
  char line[2048];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  size_t length = strlen(line);
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
    line[--length] = 0;
  LogF("%s: %s", s_core->name, line);
  UsageStats::CoreLine(line);  // (01.04: the community online modes' signs - DCNet, the DS's Wi-Fi, the PSP's)
  // (00.81) SwanStation's words for a disc it couldn't open and for a LibCrypt disc without its .sbi (Run tells the
  // player). Its line: "[System] Failed to load CD image '<path>': <reason>. Booting BIOS with no disc."
  if (s_system == SystemId::PS1)
  {
    if (const char* failed = std::strstr(line, "Failed to load CD image '"))
    {
      const char* reason = std::strstr(failed, "': ");
      std::string text = reason ? reason + 3 : "";
      const size_t tail = text.rfind(". Booting BIOS with no disc.");
      if (tail != std::string::npos)
        text.erase(tail);
      s_ps1_disc_error = text.empty() ? "?" : text;
    }
    if (std::strstr(line, "EmuSuNi: LibCrypt-protected disc"))
      s_ps1_libcrypt = true;
    if (std::strstr(line, "EmuSuNi: disc ") && std::strstr(line, "BIOS OpenBIOS"))
      s_ps1_openbios = true;
    if (std::strstr(line, "has 2048-byte sectors (an ISO's"))
      s_ps1_cooked = true;
    // (00.23) The disc's code, from scripts/build-ps1-core.sh's line as the game loads ("EmuSuNi: disc in, code
    // 'SCUS-94426', ..."): Multiplayer Adapter's Auto looks it up.
    if (const char* code = std::strstr(line, "EmuSuNi: disc in, code '"))
    {
      code += std::strlen("EmuSuNi: disc in, code '");
      const char* end = std::strchr(code, '\'');
      s_mp_game.disc_code = end ? std::string(code, end) : std::string();
      UsageStats::SetDiscCode(s_mp_game.disc_code);  // (00.23: the anonymous statistics' game)
    }
  }
  // (01.05) PPSSPP's 30 FPS hack's games, from scripts/build-psp-core.sh's line for each compat.ini flag a game gets
  // ("gamedb: psp UCUS98737: GoWFramerateHack60 = true (compat.ini)"): PSP Frame Rate Auto's 30 FPS only for them.
  if (s_system == SystemId::PSP && std::strstr(line, ": GoWFramerateHack60 = true (compat.ini)"))
    s_psp_gow = true;
  if (s_system == SystemId::PSP && std::strstr(line, "Bad memory access detected!") &&
      std::strstr(line, "Stopping emulation"))
    s_psp_stopped = true;
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

// N64 CPU Timing as the core's CountPerOp option, and where it comes from (empty: the core's game list). Light is 3
// counts per instruction (the core's list, mupen64plus.ini, uses it for 25 games); Auto is 3 for GoldenEye 007 (it
// then draws about 23 frames a second instead of 33 - the console: about 20 - with about a third less RSP and RDP
// work per second), else the list's own; Normal the list's own (2 for most games: "0" lets the core take it). Since
// 00.81 "the list's own" includes the app's corrections from the list's newer upstream copy (GameDb).
std::string N64CountPerOp(const std::string& g, std::string* source)
{
  const std::string timing = Settings::Get("n64_cpu_timing", g);
  if (timing == "light" || (timing == "auto" && s_n64_name == "GOLDENEYE"))
  {
    // (00.90: the file data_dir/n64-goldeneye-lighter makes GoldenEye's Auto 4 - about 17 frames a second, a quarter
    // less RSP work again, its whole frame on the PS4 one VI out of three or four: an A/B on the PS4 for the players
    // who find it slow on a PS4 "Fat", against Light's speed line and pictures a second.)
    if (timing == "auto" && FileExists(std::string(App::DATA_DIR) + "/n64-goldeneye-lighter"))
    {
      *source = "EmuSuNi: N64 CPU Timing Auto, GoldenEye 007, lighter (/data/EmuSuNi/n64-goldeneye-lighter)";
      return "4";
    }
    *source = timing == "light" ? "EmuSuNi: N64 CPU Timing Light" : "EmuSuNi: N64 CPU Timing Auto, GoldenEye 007";
    return "3";
  }
  const GameDb::N64Correction* correction = GameDb::FindN64Correction(s_n64_crc1, s_n64_crc2);
  if (correction && std::strcmp(correction->option, "CountPerOp") == 0)
  {
    *source = correction->source;
    return correction->value;
  }
  source->clear();
  return "0";
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
  // Asynchronous: the GPU draws while the N64 runs on (the CPU doesn't wait for each picture); the games that need
  // Accurate play with it whatever the setting (N64AccurateGame).
  const bool accurate = Settings::Get("n64_sync", g) == "accurate" || N64AccurateGame(s_n64_code) != nullptr;
  s_variables[prefix + "parallel-rdp-synchronous"] = accurate ? "True" : "False";
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
  // (01.58) N64 Widescreen: a listed game's words written (on) or put back (off); none for the others.
  const N64WidescreenTitle* wide = N64Widescreen(s_n64_crc1, s_n64_crc2);
  s_n64_widescreen = wide && Settings::Get("n64_widescreen", g) == "on";
  s_variables[prefix + "emusuni-ram-patches"] = wide ? N64WidescreenPatches(*wide, s_n64_widescreen) : std::string();
  if (wide)
    LogF("n64: N64 Widescreen %s for %s", s_n64_widescreen ? "on (16:9, experimental)" : "off (4:3)", wide->name);
  s_variables[prefix + "Framerate"] = "Original";
  // N64 CPU Timing (N64CountPerOp).
  std::string count_source;
  s_variables[prefix + "CountPerOp"] = N64CountPerOp(g, &count_source);
  s_variables[prefix + "virefresh"] = "Auto";
  s_variables[prefix + "pak1"] = Settings::Get("n64_pak", g);
  // (01.58) Players 2-4: Controller Pak (Players 2-4), a Memory Pak unless chosen (a Rumble Pak: their pads shake too).
  const std::string pak_others = Settings::Get("n64_pak_others", g);
  for (const char* pak : {"pak2", "pak3", "pak4"})
    s_variables[prefix + pak] = pak_others;
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

// (00.95) SameBoy's options for a link cable match (src/GbLink.h): the cable on, the two screens side by side (each
// PS4 shows its own: VideoRefresh), each Game Boy's model and colours from its game's Game Boy Colours: a Game Boy
// game with the Game Boy Color's runs on the Game Boy Color, which colours Nintendo's games as it did (the Super
// Game Boy's too: there is none in a link); the others on the Game Boy with that screen's palette. A Game Boy Color
// game on its own. The sound: this console's Game Boy's (GbLinkLocalVariables). (The host's options are the match's.)
void BuildGbLinkVariables()
{
  const GbLink::Match& match = GbLink::Get();
  if (GbLink::Gba())
  {
    // (01.04) mGBA's link core (two GBAs): the cartridges' clock from the host's time now (the host's options are the
    // match's: the same on both), and the GBA this console shows and hears (GbLinkLocalVariables: its own).
    s_variables["gbalink_rtc"] = std::to_string(static_cast<long long>(std::time(nullptr)));
    s_variables["gbalink_player"] = "1";
    return;
  }
  s_variables["sameboy_link"] = "enabled";
  s_variables["sameboy_screen_layout"] = "left-right";
  for (int i = 0; i < 2; i++)
  {
    const std::string n = std::to_string(i + 1);
    const std::string palette = Settings::Get("gb_palette", match.games[i].key);
    const bool color_game = match.games[i].system == SystemId::GBC;
    s_variables["sameboy_model_" + n] =
        color_game ? "Auto" : (palette == "gbc" || palette == "sgb") ? "Game Boy Color" : "Game Boy";
    s_variables["sameboy_mono_palette_" + n] = palette == "dmg"    ? "lime" :
                                               palette == "pocket" ? "olive" :
                                               palette == "light"  ? "teal" :
                                                                     "greyscale";
    const bool lcd = Settings::Get("lcd_colors", match.games[i].key) == "lcd";
    s_variables["sameboy_color_correction_mode_" + n] = lcd ? "emulate hardware" : "off";
    s_variables["sameboy_high_pass_filter_mode_" + n] = "accurate";
    s_variables["sameboy_audio_interference_" + n] = "0";
    s_variables["sameboy_rumble_" + n] = "never";
  }
}
// The sound of this console's own Game Boy (only its own: not the host's options, and it changes nothing in the
// emulation's state).
void GbLinkLocalVariables()
{
  // (01.04: two GBAs - this console's GBA's picture and sound, the other one is never drawn.)
  const char* key = GbLink::Gba() ? "gbalink_player" : "sameboy_audio_output";
  const char* own = GbLink::Gba() ? (GbLink::Get().local == 1 ? "2" : "1") :
                                    (GbLink::Get().local == 1 ? "Game Boy #2" : "Game Boy #1");
  if (s_variables[key] != own)
  {
    s_variables[key] = own;
    s_variables_changed = true;
  }
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

// The game's picture is two screens: the DS's or the 3DS's.
bool TwoScreens()
{
  return s_system == SystemId::NDS || s_system == SystemId::THREEDS;
}

// The screens' settings for the game (the same on both consoles, each its own values: Settings.cpp
// EMUSUNI_SCREEN_SETTINGS), with R3's swap.
DualScreens::Options ScreenOptions(const std::string& game)
{
  const std::string p = s_system == SystemId::THREEDS ? "3ds_" : "nds_";
  DualScreens::Options options;
  options.layout = Settings::Get(p + "layout", game);
  options.swapped = (Settings::Get(p + "swap", game) == "on") != s_r3_swap.load(std::memory_order_relaxed);
  options.small_size = std::clamp(Settings::GetInt(p + "small_size", game), 2, 4);
  options.small_position = Settings::Get(p + "small_position", game);
  options.gap = Settings::Get(p + "gap", game);
  return options;
}

// The two screens as they are shown now: their places in the picture (in the console's pixels: the DS's 256x192 each,
// the 3DS's 400x240 and 320x240), and where each one is in the core's picture, in its pixels (0: the top screen, 1:
// the touch screen). False: not a picture of two screens (the start's, another size): it is drawn as it is.
struct Screens
{
  DualScreens::Layout layout;
  DualScreens::Rect source[2];
};

bool CurrentScreens(const std::string& game, Screens* screens)
{
  const bool threeds = s_system == SystemId::THREEDS;
  if (!TwoScreens() || !DualScreens::InCorePicture(threeds, s_frame_w, s_frame_h, s_3ds_scale, screens->source))
    return false;
  screens->layout = DualScreens::Arrange(ScreenOptions(game), threeds ? 400.0f : 256.0f, threeds ? 240.0f : 192.0f,
                                         threeds ? 320.0f : 256.0f, threeds ? 240.0f : 192.0f);
  return true;
}

// The screens' settings as one text: a change of them in the in-game menu undoes R3's swap (the screens as the menu
// shows them).
std::string ScreensSeen(const std::string& game)
{
  const std::string p = s_system == SystemId::THREEDS ? "3ds_" : "nds_";
  return Settings::Get(p + "layout", game) + "/" + Settings::Get(p + "swap", game);
}

// The screens as they are placed, in the log: at a game's start, and when their settings change in the in-game menu.
void LogScreens(const std::string& game, const char* when)
{
  if (!TwoScreens())
    return;
  const bool threeds = s_system == SystemId::THREEDS;
  const DualScreens::Options options = ScreenOptions(game);
  const DualScreens::Layout layout = DualScreens::Arrange(options, threeds ? 400.0f : 256.0f, threeds ? 240.0f : 192.0f,
                                                          threeds ? 320.0f : 256.0f, threeds ? 240.0f : 192.0f);
  LogF("screens (%s): %s, %s first%s, small screen 1/%d %s, gap %s: a picture of %.0fx%.0f of the console's pixels "
       "(the core gives both screens side by side, the touch screen always at the same place)",
       when, options.layout.c_str(), options.swapped ? "the touch screen" : "the top screen",
       s_r3_swap.load() ? " (R3)" : "", options.small_size, options.small_position.c_str(), options.gap.c_str(),
       layout.width, layout.height);
}

// (01.31) Player 1's pad on a DS or a 3DS (L3Press): L3 kept from the game until it has been held for L3_HOLD; true as a
// shorter press, without R3, is let go.
bool TakeL3Press(OrbisPadData* data, bool live, float dt)
{
  const bool l3 = live && (data->buttons & ORBIS_PAD_BUTTON_L3) != 0;
  bool short_press = false;
  if (l3)
  {
    if (!s_l3.down)
      s_l3 = {};
    s_l3.held += dt;
    s_l3.menu = s_l3.menu || (data->buttons & ORBIS_PAD_BUTTON_R3) != 0;
    s_l3.through = s_l3.through || s_l3.held >= L3_HOLD;
    if (!s_l3.through)
      data->buttons &= ~static_cast<uint32_t>(ORBIS_PAD_BUTTON_L3);
  }
  else if (s_l3.down)
    short_press = !s_l3.through && !s_l3.menu;
  s_l3.down = l3;
  return short_press;
}

// (01.31) L3's short press: the screens' next Screen Layout - the in-game menu's list, in its order, round - kept as
// the menu keeps it (where the game's settings come from: This Console unless chosen otherwise); R3's swap stays. Its
// name on the screen.
void NextScreenLayout(const std::string& game, IngameMenu& menu)
{
  const std::string key = s_system == SystemId::THREEDS ? "3ds_layout" : "nds_layout";
  const Settings::Setting* setting = Settings::Find(key);
  if (!setting)
    return;
  Settings::UseConsoleByDefault(game);
  const std::string next = Settings::Step(*setting, Settings::Get(key, game), 1);
  Settings::Set(key, next, game);
  s_screens_seen = ScreensSeen(game);
  LogScreens(game, "L3: the next layout");
  menu.Toast("Screen Layout: " + Settings::LabelOf(*setting, next));
}

// (00.95) The DNS the DS's firmware gets for the games' Nintendo Wi-Fi Connection (DS Wi-Fi, nds_wifi): a server that
// took over from Nintendo's, or "" (Off: no network). Checked on 04-11-2026 (nas.nintendowifi.net and the GameSpy
// names asked of each): Wiimmfi through Kaeru WFC's gateway, 178.62.43.212 (melonDS DS's own "Wiimmfi", 95.217.77.181,
// is Wiimmfi's for the Wii: it doesn't know the DS games' names), and WiiLink WFC, 5.161.56.11. Another address, if the
// servers move before a new version: /data/EmuSuNi/nds-dns.txt with the IPv4 address (Off still turns it off).
std::string NdsWifiDns(const std::string& choice)
{
  if (choice == "off")
    return "";
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  const std::string own = ReadTextFile(std::string(App::DATA_DIR) + "/nds-dns.txt");
  if (std::sscanf(own.c_str(), " %u.%u.%u.%u%c", &a, &b, &c, &d, &tail) >= 4 && a < 256 && b < 256 && c < 256 &&
      d < 256 && (tail == 0 || std::isspace(static_cast<unsigned char>(tail))))
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
  return choice == "wiilink" ? "5.161.56.11" : "178.62.43.212";
}

// melonDS DS's options: every one of its 79 is answered (an unanswered one doesn't get the default
// it declares). The JIT, the threaded 3D (its thread on core 3, port/ps4_n64.c), the built-in BIOS
// and firmware unless all three of the DS's own are in the system folder, the touchpad as the touch
// screen, the screens side by side (the app places them), the Wi-Fi as DS Wi-Fi says (00.95).
void BuildNdsVariables(const std::string& g)
{
  const std::string p = "melonds_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("console_mode", "ds");
  set("boot_mode", "direct");
  // The DS's BIOS and firmware (bios7.bin, bios9.bin, firmware.bin) in system/ or system/melonDS DS/:
  // encrypted ROMs need them; most dumps don't.
  std::string firmware;
  for (const std::string& dir : {std::string(), std::string("melonDS DS/")})
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
  // The screens: side by side, always (512x192, the touch screen on the right): the app places them as the screens'
  // settings say, and R3 swaps them there (DrawScreens, src/DualScreens.h), so the touchpad's place on the touch
  // screen never changes. (Until 00.79 the core gave them one above the other for that view; its own big screen,
  // scaled on the CPU, cost ~3.6 ms a frame on the PS4: the app's are the GPU's.) The other layouts and the gap are
  // the core's options, answered.
  set("number_of_screen_layouts", "1");
  set("screen_layout1", "left-right");
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
  // (00.95) The Wi-Fi: melonDS's indirect mode (libslirp, scripts/build-nds-core.sh: only to the internet, never the
  // home network) and the server's DNS in the firmware's connection (melonAP's, the one the built-in firmware has
  // ready), or no network at all. Nothing is sent until a game connects. The log says the server, never an address
  // of the player's.
  const std::string wifi = Settings::Get("nds_wifi", g);
  const std::string dns = NdsWifiDns(wifi);
  set("network_mode", dns.empty() ? "disabled" : "indirect");
  set("firmware_wfc_dns", dns.empty() ? "default" : dns);
  if (dns.empty())
    LogF("nds: Wi-Fi off (DS Wi-Fi)");
  else
    LogF("nds: Wi-Fi: %s, DNS %s (DS Wi-Fi%s); the DS reaches only the internet", wifi == "wiilink" ? "WiiLink WFC" : "Wiimmfi",
         dns.c_str(), FileExists(std::string(App::DATA_DIR) + "/nds-dns.txt") ? ", /data/EmuSuNi/nds-dns.txt" : "");
  UsageStats::SetDsWifi(dns == "5.161.56.11" ? "wiilink" : dns == "178.62.43.212" ? "wiimmfi" : "");  // (01.04)
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
// a PSP-2000 with its memory stick, the network as PSP Online says. Every option the core reads is answered.
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
  // PSP Resolution, Texture Sharpness, Texture Upscaling, Anti-Aliasing: each change applied while the game
  // runs (with psp-resolution-at-start the size and the multisampling stay as the game started: s_psp_scale).
  if (s_psp_scale == 0 || !s_psp_at_start)
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
  // Frame Skip: the core's own (it leaves out the GPU's work, not the game's). With it on, a run lasts until the PSP's
  // next picture (upstream's frame skip turns its frame duplication off); the core counts the vblanks and the pacing
  // pays for them (00.81: PspFrames - until then a game at 30 FPS ran at twice its speed with Frame Skip).
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
  // The left stick's dead zone and scale (00.68, GitHub issue #8: PSP Stick Dead Zone and Sensitivity, per game; the
  // core's own values, 0.15 and 1.00 until 00.68).
  auto one_of = [](const std::string& value, std::initializer_list<const char*> known, const char* fallback) {
    for (const char* each : known)
    {
      if (value == each)
        return value;
    }
    return std::string(fallback);
  };
  set("analog_deadzone", one_of(Settings::Get("psp_deadzone", g), {"0.0", "0.05", "0.10", "0.15", "0.20", "0.25", "0.30"},
                                "0.15"));
  set("analog_sensitivity",
      one_of(Settings::Get("psp_sensitivity", g), {"1.00", "1.10", "1.20", "1.30", "1.40", "1.50"}, "1.00"));
  set("analog_is_circular", "disabled");
  // PSP Online (01.02; scripts/build-psp-core.sh, "Online play"): the PSP's Wi-Fi, for the games' own servers (the
  // revival servers' DNS) and ad hoc play through PSP Ad Hoc Server's relay; Off, no network. No built-in server, no
  // UPnP. A MAC address of this PS4's own, made at random once (psp-mac.txt: a game's ad hoc friends know a player by
  // it; the same all session). psp-net-log: PPSSPP's every network call in the log (never an IPv4 address: the core
  // writes each "<IP>"). The PSP's nickname: below.
  set("enable_wlan", Settings::Get("psp_online", g) == "on" ? "enabled" : "disabled");
  set("enable_builtin_pro_ad_hoc_server", "disabled");
  set("change_pro_ad_hoc_server_address",
      one_of(Settings::Get("psp_adhoc_server", g),
             {"socom.cc", "psp.mgn.pub", "psi-hate.com", "jpa36a7.glddns.com", "brazil-01.sprawl-relay.link"},
             "socom.cc"));
  set("enable_upnp", "disabled");
  set("upnp_use_original_port", "disabled");
  set("port_offset", "10000");  // (PPSSPP's own defaults)
  set("minimum_timeout", "0");
  set("forced_first_connect", "disabled");
  set("wlan_channel", "Auto");
  static std::string s_psp_mac;  // 12 hex digits
  if (s_psp_mac.empty())
  {
    const std::string path = std::string(App::DATA_DIR) + "/psp-mac.txt";
    for (const char c : ReadTextFile(path))
    {
      if (std::isxdigit(static_cast<unsigned char>(c)))
        s_psp_mac += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s_psp_mac.size() != 12)
    {
      // A locally administered unicast address, as PPSSPP's own random ones.
      uint64_t seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                      static_cast<uint64_t>(std::time(nullptr)) * 0x9E3779B97F4A7C15ull;
      s_psp_mac.clear();
      for (int i = 0; i < 12; i++)
      {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        s_psp_mac += "0123456789abcdef"[seed & 15];
      }
      s_psp_mac[1] = "26ae"[seed >> 62];
      std::string text;
      for (int i = 0; i < 12; i += 2)
        text += (i ? ":" : "") + s_psp_mac.substr(i, 2);
      const bool saved = WriteTextFile(path, text + "\n");
      LogF("psp: a MAC address of its own made for online play (psp-mac.txt%s)", saved ? "" : ": NOT saved");
    }
  }
  for (int i = 1; i <= 12; i++)
  {
    char key[32];
    snprintf(key, sizeof(key), "change_mac_address%02d", i);
    set(key, std::string(1, s_psp_mac[i - 1]));
    snprintf(key, sizeof(key), "pro_ad_hoc_server_address%02d", i);  // (only for an "IP address" server: never)
    set(key, "0");
  }
  setenv("EMUSUNI_PSP_NET_LOG", FileExists(std::string(App::DATA_DIR) + "/psp-net-log") ? "1" : "0", 1);
  // The PSP's nickname (its games show it, online too; it goes to the servers): the online nickname the player chose
  // (netplay.ini's, filtered as online play does), else "EmuSuNi-" and the last 4 digits of the random MAC address (no
  // two PS4s alike). Never the PS4 user's name (Netplay::GetNickname's fallback): it can be a real name.
  std::string nickname;
  {
    auto trim = [](const std::string& text) {
      const size_t first = text.find_first_not_of(" \t\r"), last = text.find_last_not_of(" \t\r");
      return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    const std::string ini = ReadTextFile(std::string(App::DATA_DIR) + "/netplay.ini");
    size_t at = 0;
    while (at < ini.size())
    {
      const size_t end = std::min(ini.find('\n', at), ini.size());
      const std::string line = ini.substr(at, end - at);
      at = end + 1;
      const size_t eq = line.find('=');
      if (eq == std::string::npos || trim(line.substr(0, eq)) != "nickname")
        continue;
      const std::string chosen = trim(line.substr(eq + 1));
      const std::string clean = chosen.empty() ? std::string() : NetProto::CleanNick(chosen);
      nickname = clean == "Player" && chosen != "Player" ? std::string() : clean;  // (CleanNick's "none left")
    }
  }
  if (nickname.empty())
    nickname = "EmuSuNi-" + s_psp_mac.substr(8, 4);
  setenv("EMUSUNI_PSP_NICKNAME", nickname.c_str(), 1);
}

// Flycast's options (the Dreamcast, and since 00.63 its arcade boards, the NAOMI and the Atomiswave: the same
// options, the board's own where it differs): its recompilers, threaded rendering (SH4, ARM7 and AICA on its
// "Flycast-emu" thread, the renderer on retro_run's), Vulkan on the app's device, no GPU readbacks,
// no network. Every option it reads is answered.
void BuildDcVariables(const std::string& g)
{
  const std::string p = "reicast_";
  auto set = [&](const std::string& key, const std::string& value) { s_variables[p + key] = value; };
  const bool board = IsFlycastBoard(s_system);
  // The console's language (games with several follow it): Dreamcast Language (00.89), Auto being the menus'.
  const std::string dc_language = s_system == SystemId::DC ? Settings::Get("dc_language", g) : std::string("auto");
  const std::string l = dc_language == "auto" ? Settings::Get("language") : dc_language;
  // The Dreamcast: the disc's region (USA, else Europe, else Japan); with a European language chosen (00.89) Europe,
  // with Japanese Japan, where the disc allows it (Flycast takes the disc's other region if not): a European game
  // made for several regions shows its languages. A NAOMI: the BIOS region NAOMI Region chooses (its Europe is the
  // Export board; its Default would be Korea's BIOS), the core taking one the cartridge was made for when it wasn't;
  // an Atomiswave: Japan, its BIOS's only region (another is looked for in vain first).
  const bool european = dc_language == "es" || dc_language == "fr" || dc_language == "de" || dc_language == "it";
  const std::string naomi_region = Settings::Get("naomi_region", g);
  set("region", s_system == SystemId::NAOMI      ? (naomi_region == "japan" ? "Japan" : naomi_region == "export" ? "Europe" : "USA") :
                s_system == SystemId::ATOMISWAVE ? "Japan" :
                european                         ? "Europe" :
                dc_language == "ja"              ? "Japan" :
                                                   "Default");
  set("broadcast", "NTSC");     // 60 Hz; Flycast picks PAL or NTSC itself for the few that need it
  set("cable_type", "VGA");     // 480p; Flycast falls back to composite for the games without VGA
  set("hle_bios", "disabled");  // the real BIOS if system/dc/dc_boot.bin is there, else its HLE BIOS
  set("language", l == "ja" ? "Japanese" : l == "de" ? "German" : l == "fr" ? "French" : l == "es" ? "Spanish" :
                  l == "it" ? "Italian" : "English");
  set("dc_32mb_mod", "disabled");
  // Dreamcast CPU Clock: the SH4's 200 MHz, or 180, 160, 140 - the game's code then runs that much less of
  // itself a frame (less work for the PS4; a game that needs it all slows down). As the game started
  // (s_dc_clock): the core counts the clock into each block of code as it compiles it, so a change in the game
  // would only reach the code compiled after it.
  if (s_dc_clock.empty())
  {
    const int percent = Settings::GetInt("dc_clock", g);
    s_dc_clock = percent == 90 ? "180" : percent == 80 ? "160" : percent == 70 ? "140" : "200";
  }
  set("sh4clock", s_dc_clock);
  set("gdrom_fast_loading", "disabled");
  // Dreamcast Sound DSP: off unless chosen, as in Flycast's own default (libretro's is on): the echo and
  // reverb some games' sound has, for the emulation thread's time (on the PC 9% of it in Crazy Taxi).
  set("enable_dsp", Settings::GetOn("dc_dsp", g) ? "enabled" : "disabled");
  set("threaded_rendering", "enabled");
  set("delay_frame_swapping", "enabled");
  // A game at 30 FPS (Jet Set Radio, all of it) or Frame Skip: each run of the core is one picture, 2 to 4 of the
  // console's frames. The pacing counts them itself (Run: DcFrames, the core's count of the frames between its
  // pictures), so Flycast's own detection stays off: in 00.67 it paced such a game, but Flycast turns it off with
  // Frame Skip Auto, and Jet Set Radio ran at up to twice its speed again on the owner's PS4.
  set("detect_vsync_swap_interval", "disabled");
  // The picture: Quality's levels (00.68; from 00.53 to 00.67 the Dreamcast played at Low only) - 1x, 2x, 3x (Dreamcast
  // Resolution) and Texture Sharpness 4x, 8x, 8x, 16x; transparency per triangle at every level (per pixel drew wrong
  // colours on the PS4, 00.49), the GPU Upscaler's steps since 00.69 (GpuMode). As the game started: Flycast makes its
  // renderer again for a new size (Crazy Taxi changed to 3x in the game: 0.6 pictures a second for 10 s, 00.52). Low is
  // 00.67's picture.
  if (s_dc_resolution.empty())
  {
    const int scale = std::clamp(Settings::GetInt("dc_resolution", g), 1, 3);
    s_dc_resolution = scale == 3 ? "1920x1440" : scale == 2 ? "1280x960" : "640x480";
    const std::string sharpness = Settings::Get("dc_anisotropy", g);
    s_dc_anisotropy = sharpness == "off" || sharpness == "8" || sharpness == "16" ? sharpness : "4";
  }
  set("internal_resolution", s_dc_resolution);
  set("anisotropic_filtering", s_dc_anisotropy);
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
    // An arcade board's light gun game aims with the left stick: its sight shown (players 1 to 4 white, red, green,
    // blue; the core draws it in gun games only).
    static const char* const SIGHTS[] = {"White", "Red", "Green", "Blue"};
    set("lightgun" + n + "_crosshair", board ? SIGHTS[i - 1] : "disabled");
  }
  set("per_content_vmus", "VMU A1");  // each game's VMU A1 in saves/dc; B1-D1 shared in system/dc
  set("vmu_sound", "disabled");
  set("linked_vmu_storage", "disabled");
  set("lightgun_crosshair_size_scaling", "100%");
  set("show_vmu_screen_settings", "disabled");
  set("show_lightgun_settings", "disabled");
  // Dreamcast Online (01.02): DCNet, Flycast's free servers, through the modem or the broadband adapter (which then
  // replaces the modem); Off, and on the boards: a game that dials gets no answer (scripts/build-dc-core.sh).
  const std::string online = s_system == SystemId::DC ? Settings::Get("dc_online", g) : std::string("off");
  set("emulate_bba", online == "bba" ? "enabled" : "disabled");
  set("upnp", "disabled");
  set("dcnet", online == "modem" || online == "bba" ? "enabled" : "disabled");
  set("network_output", "disabled");
  // The boards' TEST and SERVICE buttons are RetroPad L3 and R3 (the in-game menu's Arcade Button presses them; no
  // DualShock 4 button does until the player puts them on one: Controls). The NAOMI's games on Free Play (Flycast's
  // default: Start starts them; the Atomiswave's coins are each game's own setting, its operator menu's), coins
  // without a limit.
  set("allow_service_buttons", board ? "enabled" : "disabled");
  set("force_freeplay", "enabled");
  set("coin_limit", "0");
}

// Genesis Plus GX's options (the SG-1000, Master System, Game Gear and Mega Drive): the console by
// the file's extension, the region by the game's header unless Region says; the app skips the frames
// (GET_AUDIO_VIDEO_ENABLE) and shapes the picture itself (no borders, no NTSC filter: 256 or 320
// pixels wide, the Game Gear's 160); no BIOS, RAM cartridge or lock-on cartridge. Every option it
// reads is answered.
void BuildGpgxVariables(const std::string& g)
{
  const std::string p = "genesis_plus_gx_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("system_hw", "auto");
  // Region: PAL makes it a European console; NTSC only sets 60 Hz: each game keeps its own region (a
  // Japanese Master System game's FM music, a Japanese Mega Drive game's region check), a European
  // one runs at 60 Hz.
  if (s_gpgx_region.empty())
    s_gpgx_region = Settings::Get("region", g);
  set("region_detect", s_gpgx_region == "pal" ? "pal" : "auto");
  set("vdp_mode", s_gpgx_region == "ntsc" ? "60hz" : "auto");
  set("bios", "disabled");
  set("system_bram", "per bios");
  set("cart_bram", "per cart");
  set("cart_size", "disabled");
  set("add_on", "auto");
  set("lock_on", "disabled");
  set("aspect_ratio", "auto");
  set("overscan", "disabled");
  set("left_border", "disabled");
  set("gg_extra", "disabled");
  set("blargg_ntsc_filter", "disabled");
  set("render", "single field");  // an interlaced game's fields one at a time: 224 lines, not 448
  set("frameskip", "disabled");
  set("frameskip_threshold", "33");
  // The Game Gear's LCD Ghosting: the core has only LCD Ghosting's trail (Smart and Blend Frames -
  // chosen for all games for the Game Boy Advance, say - leave it off).
  set("lcd_filter", ConsoleSetting("lcd_ghosting", g, "off") == "lcd" ? "enabled" : "disabled");
  // The sound: MAME's YM2612 (Nuked's costs more CPU), the Master System's FM chip for the Japanese
  // games that use it, the Game Gear's Sound Filter.
  set("ym2612", "mame (ym2612)");
  set("ym2413", "auto");
  set("ym2413_core", "mame");
  set("sound_output", "stereo");
  set("audio_filter", ConsoleSetting("gb_audio_filter", g, "off") == "soft" ? "low-pass" : "disabled");
  set("lowpass_range", "60");
  set("psg_preamp", "150");
  set("fm_preamp", "100");
  set("cdda_volume", "100");
  set("pcm_volume", "100");
  for (const char* band : {"audio_eq_low", "audio_eq_mid", "audio_eq_high"})
    set(band, "100");
  set("no_sprite_limit", Settings::Get("sprite_flicker", g));
  // Emulated CPU Clock: the 68000's (or the Z80's) speed.
  const std::string clock = Settings::Get("cpu_clock", g);
  set("overclock", clock == "light" ? "125" : clock == "compatible" ? "150" : clock == "max" ? "200" : "100");
  set("force_dtack", "enabled");
  set("addr_error", "enabled");
  set("cd_latency", "enabled");
  set("cd_precache", "disabled");
  set("enhanced_vscroll", "disabled");
  set("enhanced_vscroll_limit", "8");
  // No light gun or mouse (the pads are plugged in after the game: Emulator::Run).
  set("gun_input", "lightgun");
  set("gun_cursor", "disabled");
  set("invert_mouse", "disabled");
}

// FinalBurn Neo's options (the Arcade): RGB565 pictures (its 32-bit ones the app would only convert
// back), the picture as the board draws it (a vertical game's is turned by the app: SET_ROTATION), each
// board's own refresh rate unless Arcade 60 Hz, the vector games drawn at 640x480, Frame Skip's 1 and 2
// as its fixed skips (Auto is the app's), the CPUs at their speed, the service menu on START held, the
// high-score table (hiscore.dat in system/fbneo), the patched romsets of system/fbneo/patched (01.05: a hack
// FinalBurn Neo has no driver of its own for - its zip under the set's name, CRCs unchecked, names and sizes
// still; not online, where the other console has the set as the DAT knows it), 48 kHz sound with 4-point
// interpolation and no low-pass filter, the sticks at their speed, opposite directions as the last
// pressed (SOCD), the Neo Geo as chosen, a memory card per game. The DIP switches and cheats it declares
// per game keep their defaults (unanswered), as do its light-gun and debug options.
void BuildArcadeVariables(const std::string& g)
{
  const std::string p = "fbneo-";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("allow-depth-32", "disabled");
  set("vertical-mode", "disabled");
  set("force-60hz", Settings::GetOn("arcade_60hz", g) ? "enabled" : "disabled");
  set("resolution", "640x480");
  const std::string skip = Settings::Get("frameskip", g);
  set("fixed-frameskip", skip == "1" || skip == "2" ? skip : "0");
  set("cpu-speed-adjust", "100%");
  set("diagnostic-input", "Hold Start");
  // (00.95) Online: off - each console's .hi in RAM made the states differ (and the guest's .hi was written over);
  // from the game's load (FinalBurn Neo reads its options again in the game: a netplay flag alone - the "fast
  // savestates" of GET_AUDIO_VIDEO_ENABLE - was undone by that). The guests take the host's options.
  set("hiscores", Netplay::Active() ? "disabled" : "enabled");
  set("allow-patched-romsets", Netplay::Active() ? "disabled" : "enabled");
  set("samplerate", "48000");
  set("sample-interpolation", "4-point 3rd order");
  set("fm-interpolation", "4-point 3rd order");
  set("lowpass-filter", "disabled");
  set("analog-speed", "100%");
  set("socd", "3");
  // Neo Geo: the setting's value is the core's, in capitals (DIPSWITCH, MVS_EUR, AES_JAP...).
  std::string neogeo = Settings::Get("arcade_neogeo", g);
  std::transform(neogeo.begin(), neogeo.end(), neogeo.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  set("neogeo-mode", neogeo);
  set("memcard-mode", "per-game");
}

#ifdef EMUSUNI_ARCADE
// scripts/build-arcade-core.sh: why the last load failed (FinalBurn Neo's error screen's text), which
// Arcade::LoadError puts in the app's words.
extern "C" const char* fbneo_emusuni_error(void);
#endif

// The DS's touch screen in melonDS DS's picture (side by side, always, since 00.80: 512x192, the touch screen on the
// right; DualScreens::InCorePicture): x, y, width, height in the picture's pixels. Another size: the whole picture.
std::array<float, 4> NdsTouchScreen(unsigned w, unsigned h)
{
  DualScreens::Rect screens[2];
  if (DualScreens::InCorePicture(false, w, h, 1, screens))
    return {screens[1].x, screens[1].y, screens[1].w, screens[1].h};
  return {0, 0, static_cast<float>(w), static_cast<float>(h)};
}

// The 3DS's two screens as Azahar lays them out: side by side, always, since 00.80 (its swap option off: the app
// places the screens, DualScreens), the picture it gives at a resolution of `factor` times the 3DS's own
// (ComputeLayoutGeometry in citra_libretro/emu_window: 720x240 times it; framebuffer_layout.cpp: LargeFrameLayout,
// the touch screen to the right in the middle), and where the touch screen is in it, in the picture's pixels. Until
// 00.79 Azahar laid out every 3DS Screens view itself (its big top screen's picture 1920x960, the touch screen left
// out with Top Screen Only), and R3 had it swap them.
struct ThreedsPicture
{
  unsigned w = 0, h = 0;
  std::array<float, 4> touch{};  // x, y, width, height
};

ThreedsPicture ThreedsLayout(int factor)
{
  const int f = std::clamp(factor, 1, 4);  // (3DS Resolution: 1x to 4x since 00.71)
  ThreedsPicture picture;
  picture.w = 720u * static_cast<unsigned>(f);
  picture.h = 240u * static_cast<unsigned>(f);
  DualScreens::Rect screens[2];
  DualScreens::InCorePicture(true, picture.w, picture.h, f, screens);
  picture.touch = {screens[1].x, screens[1].y, screens[1].w, screens[1].h};
  return picture;
}

// A stick with a dead zone around its centre: a DualShock 4's rest a few units off it, and Azahar's Circle Pad
// and C-stick have none of their own. Inside `zone` (of the way out) nothing, beyond it the rest of the range.
void ApplyDeadZone(std::array<int16_t, 2>* stick, float zone)
{
  const float x = static_cast<float>((*stick)[0]) / 32768.0f, y = static_cast<float>((*stick)[1]) / 32768.0f;
  const float length = std::sqrt(x * x + y * y);
  if (length <= zone)
  {
    *stick = {0, 0};
    return;
  }
  const float scale = std::min(1.0f, (length - zone) / (1.0f - zone)) / length;
  (*stick)[0] = static_cast<int16_t>(std::clamp(x * scale * 32767.0f, -32767.0f, 32767.0f));
  (*stick)[1] = static_cast<int16_t>(std::clamp(y * scale * 32767.0f, -32767.0f, 32767.0f));
}

bool ThreedsRememberNewOnly();  // (01.36, below)

// Why a 3DS game didn't start, in the app's words where Azahar's are English sentences (and one a number).
std::string ThreedsFailure(const std::string& message)
{
  const auto has = [&message](const char* part) { return message.find(part) != std::string::npos; };
  if (has("Unknown error: 13"))  // ResultStatus::ErrorN3DSApplication (core.h): a New 3DS-only game on the original 3DS
  {
    // ((01.36) New 3DS Mode out of the menu - its SMDH said nothing: the game remembered, a New 3DS from its next start.)
    if (ThreedsRememberNewOnly())
      return "This game is made for the New 3DS only\nStart it again: it starts as a New 3DS";
    return "This game is made for the New 3DS only";
  }
  if (has("decrypted"))
    return "This game is encrypted\nOnly decrypted dumps run (.3ds, .cci, .cxi)";
  if (has("format is not supported") || has("Failed to obtain loader"))
    return "The emulator doesn't know this file\nIt takes .3ds, .cci, .cxi and .3dsx, decrypted";
  if (has("GBA Virtual Console"))
    return "GBA Virtual Console games don't run here";
  return message;
}

// The 3DS game's title ID (its program ID, as Azahar's hack lists know it), read from its file's headers when the game
// starts (Run): a .3ds/.cci's first partition, a .cxi's own (both are plain even in an encrypted dump); 0 for a .3dsx
// or a file it can't read. And whether Accurate Shader Multiplication and the right eye are on for it this time - 3DS
// Accurate Shader Multiplication and 3DS Right Eye, Auto by the lists below -, kept for the game as its size is.
uint64_t s_3ds_title = 0;
bool s_3ds_accurate_mul = false, s_3ds_right_eye = false;
// (01.36) Its SMDH says New 3DS only (ThreedsNew3dsOnly), and why New 3DS Mode is what it is (the log's title ID line).
bool s_3ds_new_only = false;
const char* s_3ds_new_why = "";
// (00.85) Azahar's GPU timing emulation for the game (ThreedsGpuTimingsGame), kept for it as the two above.
bool s_3ds_gpu_timings = false;
// (01.42) Its texture pack (src/ThreeDsTextures.h) for this start: Texture Pack On, a picture in its folder and the GPU
// memory for it (EnoughMemory); and the GPU memory free when the pack was left out for want of it (-1: it wasn't).
bool s_3ds_textures = false;
int s_3ds_textures_short = -1;

uint64_t ThreedsTitleId(const std::string& path)
{
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return 0;
  uint8_t header[0x200] = {};
  auto read_at = [&](uint64_t offset) {
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(header, 1, sizeof(header), file) ==
                                                                              sizeof(header);
  };
  auto u32_at = [&](size_t at) {
    return static_cast<uint32_t>(header[at]) | static_cast<uint32_t>(header[at + 1]) << 8 |
           static_cast<uint32_t>(header[at + 2]) << 16 | static_cast<uint32_t>(header[at + 3]) << 24;
  };
  auto magic = [&](const char* word) { return std::memcmp(header + 0x100, word, 4) == 0; };
  uint64_t title = 0;
  if (read_at(0))
  {
    // NCSD (a cartridge's image): the partition table at 0x120, offsets and sizes in media units of 0x200 bytes.
    if (magic("NCSD") && read_at(static_cast<uint64_t>(u32_at(0x120)) * 0x200) && magic("NCCH"))
      title = u32_at(0x118) | static_cast<uint64_t>(u32_at(0x11C)) << 32;
    else if (magic("NCCH"))  // an executable's own (.cxi)
      title = u32_at(0x118) | static_cast<uint64_t>(u32_at(0x11C)) << 32;
  }
  std::fclose(file);
  return title;
}

// The games that draw wrong without Accurate Shader Multiplication (00.81; every region's title ID): Pokémon X and Y
// (the players: graphics errors; upstream, azahar #2583 and #1445: Froakie drawn white until it is on) and the games
// on the same engine after them, Majora's Mask 3D (the players; upstream forces it for Ocarina of Time 3D, the same
// engine: its hack list), Metroid: Samus Returns and Castlevania: Mirror of Fate (the players). Upstream's own list
// (Ocarina of Time 3D, the Mario & Luigi games) is forced by the core whatever the setting.
bool ThreedsAccurateMulGame(uint64_t title)
{
  static const uint64_t GAMES[] = {
      0x0004000000055D00, 0x0004000000055E00,  // Pokémon X, Y (every region)
      0x000400000011C400, 0x000400000011C500,  // Pokémon Omega Ruby, Alpha Sapphire
      0x0004000000164800, 0x0004000000175E00,  // Pokémon Sun, Moon
      0x00040000001B5000, 0x00040000001B5100,  // Pokémon Ultra Sun, Ultra Moon
      0x0004000000125500, 0x0004000000125600, 0x00040000000D6E00,  // Majora's Mask 3D: USA, EUR, JPN
      0x00040000001BB200, 0x00040000001BFB00, 0x00040000001BFC00,  // Metroid: Samus Returns: USA, EUR, JPN
      0x0004000000096600, 0x000400000009E500, 0x00040000000BAC00,  // Castlevania: Mirror of Fate: USA, EUR, JPN
  };
  return std::find(std::begin(GAMES), std::end(GAMES), title) != std::end(GAMES);
}

// (01.36, the owner: the three options out of the menu) The games that break without the right eye drawn - 00.81 to
// 01.17's list, from the git history: with Azahar's Disable Right Eye Render Majora's Mask 3D's blur effects freeze the
// screen (azahar #791), Kingdom Hearts 3D's and Pokémon Super Mystery Dungeon's bottom screen stays black (#1005), Fire
// Emblem Warriors' too, Minecraft: New Nintendo 3DS Edition (upstream's hack list), Metroid: Samus Returns (00.89: the
// elevators' background dark). Every other game leaves it out (faster): the black starts of 01.17 were fixed by
// 3ds-black-uniforms in 01.22 (the 3DS specialist's 3ds-0135 test: DKC, Castlevania, Kirby, Super Mario 3D Land and
// Luigi's Mansion 2 open without it). (The core's own list - Luigi's Mansion 1 and 2, Rayman Origins - draws it anyway.)
// (01.37) Castlevania: Mirror of Fate too: without it Azahar's guess takes one in three of its left-eye pictures for
// the right eye and leaves it out, so the screen shows one from 2 frames before (the mountains come and go; the 3DS
// specialist's 3ds-0136 test, point 2). Its real right eye is drawn elsewhere, so leaving it out saved nothing there.
bool ThreedsLeftEyeSkipsGame(uint64_t title)
{
  static const uint64_t GAMES[] = {
      0x0004000000096600, 0x000400000009E500, 0x00040000000BAC00,  // Castlevania: Mirror of Fate: USA, EUR, JPN
  };
  return std::find(std::begin(GAMES), std::end(GAMES), title) != std::end(GAMES);
}

bool ThreedsRightEyeGame(uint64_t title)
{
  if (ThreedsLeftEyeSkipsGame(title))
    return true;
  static const uint64_t GAMES[] = {
      0x0004000000125500, 0x0004000000125600, 0x00040000000D6E00,  // Majora's Mask 3D: USA, EUR, JPN
      0x00040000001BB200, 0x00040000001BFB00, 0x00040000001BFC00,  // Metroid: Samus Returns: USA, EUR, JPN
      0x000400000008D300, 0x0004000000095500, 0x000400000004EE00,  // Kingdom Hearts 3D: USA, EUR, JPN
      0x0004000000174600, 0x0004000000174400,                      // Pokémon Super Mystery Dungeon: USA, EUR
      0x000400000F70CC00, 0x000400000F70CD00,                      // Fire Emblem Warriors: USA, EUR
      0x00040000001B8700, 0x000400000017CA00, 0x000400000017FD00,  // Minecraft: New 3DS Edition: USA, EUR, JPN
  };
  return std::find(std::begin(GAMES), std::end(GAMES), title) != std::end(GAMES);
}

// (01.36) A game made only for the New 3DS, as Azahar tells it (AppLoader_NCCH::IsN3DSExclusive, which refuses such a
// game on the original 3DS: "Unknown error: 13"): its SMDH - the ExeFS's "icon" - with flag bit 12 (0x1000, the flags
// at 0x2028). From a .3ds/.cci's first partition or a .cxi's own NCCH, unencrypted (the core runs no encrypted one);
// false when it can't be read (a .3dsx: Azahar's own answer is false there too).
bool ThreedsNew3dsOnly(const std::string& path)
{
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;
  auto read_at = [file](uint64_t offset, void* out, size_t size) {
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, file) == size;
  };
  auto u32_at = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
  };
  bool only = false;
  uint8_t header[0x200] = {};
  uint64_t ncch = 0;
  bool there = read_at(0, header, sizeof(header));
  if (there && std::memcmp(header + 0x100, "NCSD", 4) == 0)
  {
    ncch = static_cast<uint64_t>(u32_at(header + 0x120)) * 0x200;
    there = read_at(ncch, header, sizeof(header));
  }
  // "NCCH", its flags' NoCrypto bit (0x04 at 0x18F), the ExeFS's offset and size (media units of 0x200) at 0x1A0.
  if (there && std::memcmp(header + 0x100, "NCCH", 4) == 0 && (header[0x18F] & 0x04) != 0 &&
      u32_at(header + 0x1A4) != 0)
  {
    const uint64_t exefs = ncch + static_cast<uint64_t>(u32_at(header + 0x1A0)) * 0x200;
    uint8_t files[0x200] = {};
    if (read_at(exefs, files, sizeof(files)))
    {
      // Ten files of 16 bytes: the name (8), its offset after this header, its size.
      for (int i = 0; i < 10; i++)
      {
        const uint8_t* entry = files + i * 16;
        if (std::memcmp(entry, "icon\0\0\0\0", 8) != 0 || u32_at(entry + 12) < 0x36C0)
          continue;
        const uint64_t smdh = exefs + 0x200 + u32_at(entry + 8);
        uint8_t magic[4] = {}, flags[4] = {};
        if (read_at(smdh, magic, 4) && std::memcmp(magic, "SMDH", 4) == 0 && read_at(smdh + 0x2028, flags, 4))
          only = (u32_at(flags) & 0x1000) != 0;
      }
    }
  }
  std::fclose(file);
  return only;
}

// (01.36) The games Azahar refused as New 3DS-only although their SMDH didn't say it (ThreedsNew3dsOnly couldn't read
// it): their title IDs, one per line, in <data>/3ds-new-only.txt - New 3DS from their next start (ThreedsFailure).
std::string ThreedsNewOnlyFile()
{
  return Portable::UserDir() + "/3ds-new-only.txt";
}
bool ThreedsRememberedNewOnly(uint64_t title)
{
  if (title == 0)
    return false;
  char id[20];
  std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title));
  std::ifstream file(ThreedsNewOnlyFile());
  for (std::string line; std::getline(file, line);)
  {
    if (line.rfind(id, 0) == 0)
      return true;
  }
  return false;
}
bool ThreedsRememberNewOnly()
{
  if (s_3ds_title == 0 || s_3ds_new == 1)
    return false;  // (a .3dsx, or a New 3DS already: nothing to do the next time)
  if (ThreedsRememberedNewOnly(s_3ds_title))
    return true;
  char line[20];
  std::snprintf(line, sizeof(line), "%016llx\n", static_cast<unsigned long long>(s_3ds_title));
  std::ifstream before(ThreedsNewOnlyFile());
  const std::string text((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
  const bool ok = WriteTextFile(ThreedsNewOnlyFile(), text + line);
  LogF("3ds: title ID %016llx refused on the original 3DS (error 13): a New 3DS from its next start%s",
       static_cast<unsigned long long>(s_3ds_title), ok ? "" : " (couldn't be written)");
  return ok;
}

// (01.36) A test file of /data/EmuSuNi for a 3DS option out of the menu: -1 none, 1 there (on in every game), 0 with
// the word off in it (off in every game) - as the core's own 3ds-right-eye.
int ThreedsTestFile(const char* name)
{
  std::ifstream file(std::string("/data/EmuSuNi/") + name);
  if (!file)
    return -1;
  std::string word;
  file >> word;
  return word == "off" ? 0 : 1;
}

// (01.29) The games whose Miis are the game - Miitopia (and its Casting Call) and Tomodachi Life -, every region's
// title ID in Azahar's own compatibility list (azahar-emu/compatibility-list, compatibility_list.json, read
// 2026-11-13): without a real 3DS's Mii data their Miis have no faces (#69; ThreedsMiiDataThere).
bool ThreedsMiiGame(uint64_t title)
{
  static const uint64_t GAMES[] = {
      0x00040000001B4E00, 0x00040000001B4F00, 0x0004000000178800,  // Miitopia
      0x00040000001B8C00,                                          // Miitopia: Casting Call
      0x000400000008C300, 0x000400000008C400, 0x000400000008C500, 0x0004000000120C00,  // Tomodachi Life
  };
  return std::find(std::begin(GAMES), std::end(GAMES), title) != std::end(GAMES);
}

// (01.29) Is a real 3DS's Mii data (the system archive 0004009B00010202) in its folder as the core reads it: a
// decrypted NCCH of that title (the core: "Encrypted NCCH are not supported") named 00000000.app, or any .app of it
// with a .tmd beside it (the core then takes the content's name from the .tmd). Without it the core loads its
// open-source stand-in, whose eyes, eyebrows and mouths are empty: Miis without faces.
bool ThreedsMiiDataThere(const std::string& folder)
{
  auto mii_ncch = [](const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
      return false;
    uint8_t header[0x200] = {};
    const bool read = std::fread(header, 1, sizeof(header), file) == sizeof(header);
    std::fclose(file);
    uint64_t program = 0;
    for (int i = 7; i >= 0; i--)
      program = program << 8 | header[0x118 + i];
    // "NCCH" at 0x100, the program ID at 0x118, the flags' NoCrypto bit (0x04) at 0x18F.
    return read && std::memcmp(header + 0x100, "NCCH", 4) == 0 && program == 0x0004009B00010202 &&
           (header[0x18F] & 0x04) != 0;
  };
  if (mii_ncch(folder + "/00000000.app"))
    return true;
  bool tmd = false, app = false;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::string ext = it->path().extension().string();
    tmd = tmd || ext == ".tmd";
    app = app || (ext == ".app" && mii_ncch(it->path()));
  }
  return tmd && app;
}

// The games that need Azahar's GPU timing emulation (00.85): Luigi's Mansion 2, whose render thread expects the 3DS's
// GPU to take its time (azahar #2095: with it, the game "no longer needs the delay render thread option to be
// playable"; without, it stutters and crashes as it goes into the mansion - azahar #1878, on the libretro core; a
// player's v00.22 froze, then closed). Off upstream by default, as for every other game here (the file
// /data/EmuSuNi/3ds-gpu-timings turns it on for every game: scripts/build-3ds-core.sh).
bool ThreedsGpuTimingsGame(uint64_t title)
{
  static const uint64_t GAMES[] = {
      0x0004000000055F00, 0x0004000000076500, 0x0004000000076400,  // Luigi's Mansion 2 (Dark Moon): USA, EUR, JPN
      0x00040000000D0000,                                          // (and the fourth of upstream's hack list)
  };
  return std::find(std::begin(GAMES), std::end(GAMES), title) != std::end(GAMES);
}

// Azahar's options (the Nintendo 3DS): the original 3DS unless New 3DS Mode, its recompilers (the ARM11's and the
// PICA shaders'), the vertex shaders on the GPU (Vulkan, on the app's device), accurate multiplication and the right
// eye per game (00.81), 3DS Resolution (1x to 4x: the Quality levels' since 00.71; 1x only from 00.64 to 00.70), no
// texture filters, HLE sound, the screens side by side (the app places them since 00.80: DualScreens), the touchpad as
// the touch screen (the right stick is the C-stick), motion only with 3DS Motion Sensor (01.27), no 3D. Every
// option the core declares is answered (core_settings.cpp declares 34, EmuSuNi's right eye with them since 00.81 and
// its GPU timings since 00.85: its build script): one it isn't given gets the default
// of its code, not the one it declares. (Checked against the core's source by the PC tests.)
void Build3dsVariables(const std::string& g)
{
  const std::string p = "citra_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  // New 3DS Mode and 3DS Resolution count as the game started: Azahar reads the first once, and the size is the
  // picture's for the whole game (its buffers, the touch screen's place). A change under a running game is its next
  // start's.
  if (s_3ds_scale == 0)
  {
    s_3ds_scale = std::clamp(Settings::GetInt("3ds_resolution", g), 1, 4);
    // ((01.36, the owner: New 3DS Mode, Accurate Shader Multiplication and 3DS Right Eye out of the menu - hidden,
    // Settings.cpp) Each one by the game alone; the values chosen before, every game's or one's, aren't read (an old
    // choice can't leave a game broken). A test file of /data/EmuSuNi still sets one for every game (ThreedsTestFile):
    // 3ds-new, 3ds-accurate-mul; the right eye's is the core's own, 3ds-right-eye.)
    // New 3DS: the games made only for it (their SMDH, or refused once with error 13), the original 3DS for the rest -
    // the lightest, and nearly every game runs on it.
    const int new_file = ThreedsTestFile("3ds-new");
    const bool remembered = !s_3ds_new_only && ThreedsRememberedNewOnly(s_3ds_title);
    s_3ds_new = new_file >= 0 ? new_file : s_3ds_new_only || remembered ? 1 : 0;
    s_3ds_new_why = new_file >= 0       ? "/data/EmuSuNi/3ds-new" :
                    s_3ds_new_only      ? "its SMDH: New 3DS only" :
                    remembered          ? "refused on the original 3DS before (3ds-new-only.txt)" :
                                          "not a New 3DS-only game";
    // (00.81: the core applies both when the game takes the GPU, at its start: kept for the game as well.) Accurate
    // multiplication: in the games known to need it (ThreedsAccurateMulGame; off in all of them, Froakie white again).
    const int mul_file = ThreedsTestFile("3ds-accurate-mul");
    s_3ds_accurate_mul = mul_file >= 0 ? mul_file == 1 : ThreedsAccurateMulGame(s_3ds_title);
    // The right eye: left out (faster) but in the games that break without it (ThreedsRightEyeGame, 00.81 to 01.17's
    // list; from 01.18 to 01.35 drawn in every game).
    s_3ds_right_eye = ThreedsRightEyeGame(s_3ds_title);
    s_3ds_gpu_timings = ThreedsGpuTimingsGame(s_3ds_title);
    // (01.27) 3DS Motion Sensor (Motion3ds): the core reads it as its input devices are made, at the game's start.
    s_motion3ds.on = Settings::Get("3ds_motion", g) == "ds4";
    s_motion3ds.said = false;
    const float flat[6] = {0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 6; i++)
      s_motion3ds.axes[i].store(flat[i], std::memory_order_relaxed);
  }
  set("is_new_3ds", s_3ds_new == 1 ? "New 3DS" : "Old 3DS");
  set("use_cpu_jit", "enabled");
  const int clock = Settings::GetInt("3ds_clock", g);
  set("cpu_clock_percentage", clock == 50 || clock == 75 ? std::to_string(clock) : std::string("100"));
  set("region_value", "Auto");  // the game's
  // The games' language: the menus' one.
  static const std::pair<const char*, const char*> LANGUAGES[] = {
      {"es", "Spanish"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"},
      {"ja", "Japanese"}, {"ru", "Russian"}, {"ko", "Korean"}, {"nl", "Dutch"}, {"zh", "Simplified Chinese"}};
  const std::string language = Settings::Get("language");
  std::string name = "English";
  for (const auto& [code, full] : LANGUAGES)
  {
    if (language == code)
      name = full;
  }
  set("language_value", name);
  set("audio_emulation", "hle");
  set("input_type", "none");  // (no microphone)
  set("graphics_api", "Vulkan");
  // (00.85: the file /data/EmuSuNi/3ds-cpu-shaders runs the vertex shaders on the CPU, as Azahar's Enable Hardware
  // Shader off: much slower on the PS4, but it tells whether a game's broken models come from the GPU's shaders.)
  set("use_hw_shader", FileExists("/data/EmuSuNi/3ds-cpu-shaders") ? "disabled" : "enabled");
  set("use_shader_jit", "enabled");
  // Accurate multiplication in the GPU's shaders: Off in most games (the same pictures, a little less GPU work), On in
  // those that draw wrong without it (00.81: 3DS Accurate Shader Multiplication; Off for every game until 00.80).
  set("shaders_accurate_mul", s_3ds_accurate_mul ? "enabled" : "disabled");
  // The right eye's picture: (01.36) left out but in the games that need it (ThreedsRightEyeGame; 01.18 to 01.35 drawn
  // unless 3DS Right Eye said Not Drawn). The core's file /data/EmuSuNi/3ds-right-eye draws it in every game, or with
  // the word off leaves it out in every game.
  set("emusuni_right_eye", s_3ds_right_eye ? "enabled" : "disabled");
  // Azahar's GPU timing emulation: for the games that need it (00.85: ThreedsGpuTimingsGame, Luigi's Mansion 2).
  set("emusuni_gpu_timings", s_3ds_gpu_timings ? "enabled" : "disabled");
#ifdef EMUSUNI_3DS
  // (01.09) The game's LayeredFS mods (saves/3ds/Azahar/load/mods/<title ID>): its Updates, DLC and Mods -> Mods (on
  // unless turned off for it; a core before 01.09 loads them whenever their folder is there).
  set("emusuni_mods", s_3ds_title == 0 || ThreeDsCia::ModsOn(s_3ds_title) ? "enabled" : "disabled");
  // (01.46, the owner) Each of them one by one: its update and its DLC on or off, and the mods in folders of their own
  // that are on (load/mods/<title ID>/<a mod>/, split by '|'; the main one is emusuni_mods). A core that doesn't ask
  // for them (ThreeDsCia::ExtrasCaps 0) loads its update and DLC, and only the main mod.
  set("emusuni_update", s_3ds_title == 0 || ThreeDsCia::UpdateOn(s_3ds_title) ? "enabled" : "disabled");
  set("emusuni_dlc", s_3ds_title == 0 || ThreeDsCia::DlcOn(s_3ds_title) ? "enabled" : "disabled");
  set("emusuni_mod_folders", s_3ds_title == 0 ? std::string() : ThreeDsCia::ModFolders(s_save_dir, s_3ds_title));
#endif
  set("use_disk_shader_cache", "enabled");  // (saves/3ds/Azahar/shaders)
  set("resolution_factor", std::to_string(s_3ds_scale));
  // (01.18) Azahar's texture filter never (01.17's ScaleForce option is gone: on the PS4 a GPU hang in Luigi's
  // Mansion 2 and textures missing in Castlevania); an old settings.ini's 3ds_texture_filter does nothing.
  set("texture_filter", "none");
  set("texture_sampling", "GameControlled");
  // (01.42) The game's texture pack (saves/3ds/Azahar/load/textures/<title ID>): its Updates, DLC and Mods -> Texture
  // Pack, decided at its start (EnoughMemory). The core's own file /data/EmuSuNi/3ds-custom-textures still turns it on
  // for every game, as a test.
  set("custom_textures", s_3ds_textures ? "enabled" : "disabled");
  set("dump_textures", "disabled");
  // The screens: side by side, always, never swapped by the core (720x240 times the resolution, the touch screen on
  // the right: ThreedsLayout): the app places them as the screens' settings say, and R3 swaps them there
  // (DrawScreens, src/DualScreens.h), so the touchpad's place on the touch screen never changes and the core's picture
  // never changes size during a game. (Until 00.79 these were 3DS Screens' layouts, R3 the swap option: Azahar's
  // libretro window has no gap and no place for the small screen, and its big top screen was a 1920x960 picture even
  // at 1x.)
  set("layout_option", "side_by_side");
  set("swap_screen", "Top");
  set("swap_screen_mode", "Toggle");
  set("large_screen_proportion", "4.00");  // (its Large Screen layout's, not used)
  set("render_3d", "off");
  set("factor_3d", "0");
  set("use_virtual_sd", "enabled");
  set("use_libretro_save_path", "LibRetro Default");  // saves/3ds/Azahar: its sdmc, nand and caches
  // The right stick is the C-stick (not a pointer for the touch screen); the touchpad touches (pointer
  // input), no mouse; the app draws the stylus itself.
  set("analog_function", "c_stick");
  set("analog_deadzone", "15");
  set("enable_mouse_touchscreen", "disabled");
  set("enable_touch_touchscreen", "enabled");
  set("enable_touch_pointer_timeout", "enabled");
  set("enable_motion", s_motion3ds.on ? "enabled" : "disabled");  // (01.27: the sensor interface, Motion3ds)
  set("motion_sensitivity", "1.0");
}

#ifdef EMUSUNI_SATURN
// YabaSanshiro's options (the Saturn): the real BIOS (the app finds it: EMUSUNI_SATURN_BIOS, Run), the core's own
// frame skip off (on, its emulation thread waits to 60 frames a second by spinning), the 4 MiB RAM cartridge, the
// console's picture at its own size (its software renderer: scripts/build-saturn-core.sh; the OpenGL renderer's
// options answered as the core's defaults), no multitap (00.23: Multiplayer Adapter On plugs the 6 Player Adaptors in,
// BuildVariables). Every option it reads is answered.
void BuildSaturnVariables(const std::string&)
{
  const std::string p = "yabasanshiro_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("force_hle_bios", "disabled");
  set("frameskip", "disabled");
  set("addon_cart", "4M_extended_ram");
  set("system_language", "english");
  set("multitap_port1", "disabled");
  set("multitap_port2", "disabled");
  set("polygon_mode", "perspective_correction");
  set("resolution_mode", "original");
  set("rbg_resolution_mode", "original");
  set("rbg_use_compute_shader", "disabled");
}
#endif

// Beetle PCE Fast's options (the PC Engine, 00.82): RGB colours, the whole picture (352 wide, lines 3 to 242), the pad
// chosen (PC Engine Pad), no turbo or soft reset switches of the core's (the app's buttons), Reduce Sprite Flicker; the
// app skips the frames (GET_AUDIO_VIDEO_ENABLE: scripts/build-beetle-cores.sh). (The CD's options - its System Card,
// image cache, drive speed and volumes - are left at the core's defaults since 00.83: no CD game is given to it.)
#ifdef EMUSUNI_OPENBOR
// (with-openbor) OpenBOR's options: the engine version picked by the core (by the .pak's contents, or a tag in its
// name, "[ob6391]"), its own analog stick option off (the app moves the D-pad with the left stick: Controls.cpp), rumble
// on (the games that have it), no gamma of its own (the app's picture settings do that). Every option it declares is
// answered.
void BuildOpenborVariables(const std::string&)
{
  s_variables["openbor_engine"] = "auto";
  s_variables["openbor_analog"] = "disabled";
  s_variables["openbor_rumble"] = "enabled";
  s_variables["openbor_gamma"] = "0";
}

#endif
#ifdef EMUSUNI_VITA
// (with-vita) Vita3K's options, the few that aren't its defaults: no mapping of the guest's memory into the GPU's
// (the app's device has no buffer device addresses; the default of the core's build for EmuSuNi too), no network of
// the Vita's (the core's build has none: scripts/build-vita-core.sh), no camera of the PS4's (a plain colour). The
// rest as the core has them: Vulkan, the Vita's own resolution, the recompiler, Cross to confirm.
void BuildVitaVariables(const std::string& g)
{
  s_variables["vita3k_memory_mapping"] = "disabled";
  s_variables["vita3k_http_enable"] = "disabled";
  s_variables["vita3k_psn_signed_in"] = "disabled";
  s_variables["vita3k_front_camera"] = "color";
  s_variables["vita3k_back_camera"] = "color";
  // (01.88) PS Vita Motion Sensor (vita_motion, MotionVita): read here, as the 3DS's 3ds_motion, before the core asks
  // for the sensor interface at its init. Lying flat until the controller answers.
  s_motion_vita.on = Settings::Get("vita_motion", g) == "ds4";
  s_motion_vita.said = false;
  const float flat[6] = {0.0f, 0.0f, 9.80665f, 0.0f, 0.0f, 0.0f};
  for (int k = 0; k < 6; k++)
    s_motion_vita.axes[k].store(flat[k], std::memory_order_relaxed);
  LogF("vita: motion: %s", s_motion_vita.on ? "the DualShock 4's accelerometer and gyroscope (PS Vita Motion Sensor)"
                                            : "off (PS Vita Motion Sensor: DualShock 4 turns it on)");
  // (01.67, the owner: "quiero que los juegos de vita se me muestren en español") The games' language: the menus'
  // one, as the 3DS's - the Vita's system language (vita3k_sys_lang: SceSystemParamLang's numbers), which a game
  // follows when it has that language and the system dialogs show. English (United States) for the rest. The
  // app's Portuguese is Brazil's and its Chinese the simplified one. Read by the core when the game starts.
  static const std::pair<const char*, const char*> LANGUAGES[] = {
      {"es", "3"}, {"fr", "2"}, {"de", "4"}, {"it", "5"}, {"pt", "17"}, {"ja", "0"},
      {"ru", "8"}, {"ko", "9"}, {"nl", "6"}, {"zh", "11"}, {"pl", "16"}, {"tr", "19"}};
  const std::string language = Settings::Get("language");
  std::string number = "1";
  for (const auto& [code, value] : LANGUAGES)
  {
    if (language == code)
      number = value;
  }
  // ((01.72, the owner: MotorStorm RC (Japan) [PCSC00014] "me lo detectó en coreano") A Japanese or Asian game - its
  // title ID in its name: PCSC, PCSG (Japan), PCSD, PCSH (Asia) -, which may not have the menus' language and read
  // Spanish's number as another: English, unless the menus are in Japanese, Korean or Chinese. PS Vita Language
  // (vita_language) chosen for the game: that one. Said in the log, with why.
  std::string why = "the menus' language (" + language + ")";
  std::string id;
  for (size_t i = 0; i + 9 <= g.size() && id.empty(); i++)
  {
    bool match = (i == 0 || !std::isalnum(static_cast<unsigned char>(g[i - 1]))) &&
                 (i + 9 == g.size() || !std::isalnum(static_cast<unsigned char>(g[i + 9])));
    for (size_t j = 0; j < 9 && match; j++)
      match = j < 4 ? std::isupper(static_cast<unsigned char>(g[i + j])) != 0 :
                      std::isdigit(static_cast<unsigned char>(g[i + j])) != 0;
    if (match)
      id = g.substr(i, 9);
  }
  const bool asian = id.size() == 9 && id.compare(0, 3, "PCS") == 0 && std::strchr("CGDH", id[3]) != nullptr;
  if (asian && language != "ja" && language != "ko" && language != "zh")
  {
    number = "1";
    why = "a Japanese or Asian game (" + id + "): English, the menus being " + language;
  }
  const std::string chosen = Settings::Get("vita_language", g);
  if (!chosen.empty() && chosen != "auto")
  {
    number = chosen;
    why = "PS Vita Language chosen for the game";
  }
  s_variables["vita3k_sys_lang"] = number;
  LogF("vita: system language %s: %s", number.c_str(), why.c_str());
}

#endif
void BuildPceVariables(const std::string& g)
{
  const std::string p = "pce_fast_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("palette", "RGB");
  set("nospritelimit", Settings::Get("sprite_flicker", g) == "enabled" ? "enabled" : "disabled");
  set("ocmultiplier", "1");
  set("hoverscan", "352");
  set("initial_scanline", "3");
  set("last_scanline", "242");
  set("frameskip", "disabled");
  set("frameskip_threshold", "33");
  const std::string pad = Settings::Get("pce_pad", g) == "6" ? "6 Buttons" : "2 Buttons";
  for (int port = 1; port <= 5; port++)
    s_variables[p + "default_joypad_type_p" + std::to_string(port)] = pad;
  set("turbo_toggling", "disabled");
  set("turbo_toggle_hotkey", "disabled");
  set("turbo_delay", "3");
  set("disable_softreset", "disabled");
  for (int channel = 0; channel < 6; channel++)
    s_variables[p + "sound_channel_" + std::to_string(channel) + "_volume"] = "100";
  set("mouse_sensitivity", "1.25");
}

// Stella's options (the Atari 2600, 00.90): the console's region and colours as the cartridge's own (its game list),
// the whole picture without Stella's TV effects (the app's NTSC Filter, Tube TV and Scanlines do that), its own shape
// (par), 2600 Phosphor, the paddles on the left stick (and the D-pad), no reload button, no messages of its own. Every
// option it declares is answered.
void BuildA2600Variables(const std::string& g)
{
  const std::string p = "stella_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("console", "auto");
  set("palette", "standard");
  set("filter", "disabled");
  set("crop_hoverscan", "disabled");
  set("crop_voverscan", "0");
  set("ntsc_aspect", "par");
  set("pal_aspect", "par");
  set("stereo", "auto");
  const std::string phosphor = Settings::Get("a2600_phosphor", g);
  set("phosphor", phosphor == "on" || phosphor == "off" ? phosphor : "auto");
  set("phosphor_blend", "60");
  set("paddle_mouse_sensitivity", "10");
  set("paddle_joypad_sensitivity", "3");
  set("paddle_analog_sensitivity", "20");
  set("paddle_analog_deadzone", "15");
  set("paddle_analog_absolute", "disabled");
  set("lightgun_crosshair", "disabled");
  set("reload", "off");
  set("detect_pal60", "disabled");
  set("detect_ntsc50", "disabled");
  for (const char* knob : {"pal_contrast", "pal_brightness", "pal_hue", "pal_saturation", "pal_gamma"})
    set(knob, "0");
  set("dpc_pitch", "20000");
  set("messages", "disabled");
}

// ProSystem's options (the Atari 7800, 00.90): 16-bit colours (the app's game texture is RGB565), no low-pass filter of
// its own (the app's Sound Filter), no dual-stick hack (the second player's joystick on the right stick).
void BuildA7800Variables(const std::string&)
{
  const std::string p = "prosystem_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  set("color_depth", "16bit");
  set("low_pass_filter", "disabled");
  set("low_pass_range", "60");
  set("gamepad_dual_stick_hack", "disabled");
}

#ifdef EMUSUNI_JAGUAR
// Virtual Jaguar's options (the Jaguar, 00.90): its HLE BIOS, or the player's own boot ROM (Jaguar BIOS: On; the core
// looks for jagboot.rom, "[BIOS] Atari Jaguar (World).j64"... in /data/EmuSuNi/system, else boots through its HLE BIOS:
// scripts/build-atari-cores.sh); the accurate blitter (Doom's the fast one) at the console's own clocks and resolution,
// its idle loops fast-forwarded; its per-title presets for speed (Enhancement Profile: performance - never their 2x
// picture, too slow for the PS4 - and still the ones a game needs); Jaguar GPU/DSP Speed's half clock for a game that
// plays better so;
// the frames it would skip are the app's Frame Skip's; no CD, link cable, debugger, texture dumps or remapping (the
// pad as the app maps it; the number pad from the touchpad: the keyboard's 0-9, - and =).
// The games known to play better with something, by the start of their name (lower case; "doom": Doom and its
// romhacks), each checked on the PC against the defaults - its picture and its sound frame for frame - with what it
// gains. Not for every game: Virtual Jaguar's fast blitter draws some wrong (Iron Soldier's briefing tank, I-War's title,
// Syndicate's text: its docs/cart-issue-triage.md), and the half clock changes a game's timing.
struct JaguarTune
{
  const char* name;
  bool fast_blitter;  // (00.92) Virtual Jaguar's fast blitter, always
  bool half_risc;     // Jaguar GPU/DSP Speed's Auto: half
};
// (00.95) Checked too, and left out: Wolfenstein 3D (the fast blitter stripes its floor and ceiling), Iron Soldier and
// Rayman (the same pictures and sound with it, 0-2% lighter only); the half clock for none of them (Tempest 2000's
// music is the DSP's; the others gain 0-10%).
constexpr JaguarTune JAGUAR_TUNES[] = {
    {"doom", true, true},           // the fast blitter 24-33% lighter, the half clock 18% more (3D at half the rate)
    {"tempest 2000", true, false},  // (00.95) the fast blitter 35-45% lighter: its attract mode and a game
    // (00.96) the fast blitter half as heavy in a game: the final game's and the v0.93 beta's attract modes and a game,
    // the same pictures and sound since its window with BKGWREN is the accurate one's and its blits under 8 bits a
    // pixel are the accurate one's (scripts/build-atari-cores.sh: the beta's HUD bar and the final's "GAME OVER"
    // were a pixel short a line). The name: both ("Alien vs Predator (World)", "... (v0.93) (Beta) ...").
    {"alien vs predator", true, false},
    // (00.98) the fast blitter 12-13% lighter (its 3D demo, a game, its attract mode and briefing: the same pictures and
    // sound). "iron soldier (": not Iron Soldier 2, not tried.
    {"iron soldier (", true, false},
    // (00.98) the fast blitter: its screen clear (cmd 01800000, SRCDATA at the destination's pixel size since 00.98's
    // scripts/build-atari-cores.sh) was its only difference; the same pictures and sound in its attract mode and a game.
    {"wolfenstein 3d", true, false},
};

void BuildJaguarVariables(const std::string& g)
{
  const std::string p = "virtualjaguar_";
  auto set = [&](const char* key, const std::string& value) { s_variables[p + key] = value; };
  const bool bios = Settings::Get("jaguar_bios", g) == "on";
  set("bios", bios ? "enabled" : "disabled");
  set("bios_type", "custom");
  std::string name = g.substr(g.find('/') + 1);
  for (char& c : name)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  JaguarTune tune = {"", false, false};
  for (const JaguarTune& known : JAGUAR_TUNES)
  {
    if (name.compare(0, std::strlen(known.name), known.name) == 0)
      tune = known;
  }
  set("usefastblitter", tune.fast_blitter ? "enabled" : "disabled");
  if (tune.fast_blitter)
    LogF("jaguar: the fast blitter for this game");
  set("internal_resolution", "1x");
  set("true_color", "disabled");
  set("widescreen", "disabled");
  set("pertitle_defaults", "enabled");
  set("enhancement_profile", "performance");
  set("enhancement_hooks", "disabled");
  set("risc_idle_skip", "enabled");
  // (Jaguar GPU/DSP Speed's Auto: half for the games JAGUAR_TUNES names - Doom's 3D drawn at half the rate, the game at
  // its speed, the picture right -, normal for the others: Rayman's core 5% lighter only.)
  std::string risc = Settings::Get("jaguar_risc", g);
  if (risc == "auto")
  {
    risc = tune.half_risc ? "0.5x" : "1x";
    if (tune.half_risc)
      LogF("jaguar: GPU/DSP Speed Auto: half for this game");
  }
  set("risc_clock_scale", risc == "0.5x" ? "0.5x" : "1x");
  // (00.96) Jaguar Recompiler: the GPU's and the DSP's (scripts/vjaguar-jit/emusuni_gpu_jit.h, emusuni_dsp_jit.h),
  // or their interpreters.
  const char* const jaguar_jit = Settings::Get("jaguar_jit", g) == "off" ? "disabled" : "enabled";
  set("emusuni_jit", jaguar_jit);
  set("emusuni_jit_dsp", jaguar_jit);
  set("m68k_clock_scale", "1x");
  set("blit_memo", "disabled");
  set("blitter_timing", "disabled");
  set("dram_timing", "disabled");
  set("gpu_pipeline_timing", "disabled");
  set("crash_detect", "enabled");
  set("frameskip", "disabled");
  set("frameskip_max", "3");
  set("pal", "disabled");
  set("jgd", "auto");
  set("jgd_sd", "disabled");
  set("memory_track", "disabled");
  set("texture_dump", "disabled");
  set("texture_replace", "disabled");
  set("alt_inputs", "disabled");
  set("p1_numpad_to_kb", "disabled");
  set("p2_numpad_to_kb", "disabled");
}
#endif

void BuildVariables()
{
  s_variables.clear();
  if (s_gblink)
    BuildGbLinkVariables();  // (00.95: a link cable match's two Game Boys, SameBoy's)
  else if (s_system == SystemId::NES)
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
  else if (IsFlycast(s_system))
    BuildDcVariables(s_game_id);
  else if (IsGpgx(s_system))
    BuildGpgxVariables(s_game_id);
  else if (s_system == SystemId::ARCADE)
    BuildArcadeVariables(s_game_id);
  else if (s_system == SystemId::THREEDS)
    Build3dsVariables(s_game_id);
#ifdef EMUSUNI_SATURN
  else if (s_system == SystemId::SATURN)
    BuildSaturnVariables(s_game_id);
#endif
  else if (s_system == SystemId::PCE)
    BuildPceVariables(s_game_id);
  else if (s_system == SystemId::A2600)
    BuildA2600Variables(s_game_id);
  else if (s_system == SystemId::A7800)
    BuildA7800Variables(s_game_id);
#ifdef EMUSUNI_JAGUAR
  else if (s_system == SystemId::JAGUAR)
    BuildJaguarVariables(s_game_id);
#endif
#ifdef EMUSUNI_OPENBOR
  else if (s_system == SystemId::OPENBOR)
    BuildOpenborVariables(s_game_id);
#endif
#ifdef EMUSUNI_VITA
  else if (s_system == SystemId::VITA)
    BuildVitaVariables(s_game_id);
#endif
  else
    BuildSnesVariables(s_game_id);
  // (00.23) The PlayStation's Multitap, the Saturn's 6 Player Adaptors: options of their cores.
  Multiplayer::AddVariables(s_system, s_adapter, &s_variables);
  s_variables_changed = true;
  // (00.23) Online: the match's options stay - the host's, as the match started (src/Netplay.h) - whatever is changed
  // on either console while it lasts.
  Netplay::KeepVariables(&s_variables, &s_variables_changed);
  if (s_gblink)
    GbLinkLocalVariables();  // (00.95: the sound of this console's Game Boy, not the host's)
}

// ---- libretro's Vulkan interface (the core's side of the display) ----
void VulkanSetImage(void*, const retro_vulkan_image* image, uint32_t semaphore_count,
                    const VkSemaphore* semaphores, uint32_t)
{
  if (!image)
    return;
  if (s_system == SystemId::THREEDS)
    ThreedsRememberView(image->image_view);  // ((01.09) its renderer made again: which views were before)
  // (01.06) The 3DS: this picture's serial, for the drawing's wait for its submission.
  uint64_t serial = 0;
#ifdef EMUSUNI_3DS
  if (s_3ds_handover)
    serial = emusuni_3ds_image_serial();
#endif
  if (s_defer_frame)
  {
    // The drawing beside the emulation still shows the image before: taken over with the frame.
    s_pending.image = *image;
    s_pending.image_set = true;
    s_pending.image_serial = serial;
    s_pending.semaphores.assign(semaphores, semaphores + semaphore_count);
    return;
  }
  s_hw_image = *image;
  s_hw_image_set = true;
  s_hw_image_serial = serial;
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
  // ((01.87) The "gpu:" line's longest hold by a core: the core's code that took the lock, where this returns to.)
  s_display->LockQueue(__builtin_return_address(0));
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
           StrictOptions() ? "not given" : "given anyway");
    }
    if (it == s_variables.end() || (undeclared && StrictOptions()))
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
  case EMUSUNI_ENVIRONMENT_PSP_VBLANKS:
    // (The PSP core's vblanks since it started, at the end of each run: PspFrames.)
    if (!data || s_system != SystemId::PSP)
      return false;
    s_psp_vblanks = *static_cast<const unsigned long long*>(data);
    s_psp_vblanks_known = true;
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
    // (00.23: online, a rollback's frames run again undrawn when the console's rules allow it: Netplay::SkipVideo.)
    // (00.95) 4, "fast savestates", in an Arcade match: FinalBurn Neo's netplay - a fixed clock and random seed on every
    // console, its high scores off (the option is off too: BuildArcadeVariables).
    // (01.03) And in a 7800 match: ProSystem's whole state (its sound chips, the RIOT's timer: 1942 (Extended) went on
    // otherwise from a state without them) and no BIOS (scripts/build-atari-cores.sh). Not the 2600's: Stella's state
    // would grow to 1 MiB.
    *static_cast<int*>(data) =
        (s_render_video && !Netplay::SkipVideo() ? 1 : 0) | 2 |
        (Netplay::Active() && (s_system == SystemId::ARCADE || s_system == SystemId::A7800) ? 4 : 0);
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
    // ((01.66) The PS Vita's steps - they end in "..." -: its loading screen's line. Its failures as before.)
    const size_t length = std::strlen(message->msg);
    if (IsVita(s_system) && length > 3 && std::strcmp(message->msg + length - 3, "...") == 0)
    {
      s_vita_step = message->msg;
      return true;
    }
    s_message = message->msg;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
    static_cast<retro_rumble_interface*>(data)->set_rumble_state = SetRumble;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    // mGBA, SwanStation, melonDS DS, PPSSPP, Flycast, Genesis Plus GX, FinalBurn Neo, YabaSanshiro and Beetle PCE
    // Fast (00.82) read all the buttons at once (InputState's mask); the others as before.
    return IsHandheld(s_system) || s_system == SystemId::PS1 || s_system == SystemId::NDS ||
           s_system == SystemId::PSP || IsFlycast(s_system) || IsGpgx(s_system) || s_system == SystemId::ARCADE ||
           s_system == SystemId::SATURN || s_system == SystemId::PCE || IsAtari(s_system);
  case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
  {
    // (01.27) The 3DS's only with 3DS Motion Sensor: DualShock 4 (Motion3ds).
    bool vita_motion = false;
#ifdef EMUSUNI_VITA
    // (01.88) PS Vita Motion Sensor (MotionVita): ((01.91) the core has it always - a Vita lying still while it is off
    // -, so turning it on or off in the in-game menu works at once.)
    vita_motion = s_system == SystemId::VITA;
#endif
    if (!(IsHandheld(s_system) || (s_system == SystemId::THREEDS && s_motion3ds.on) || vita_motion) || !data)
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
    // The region or the NTSC filter changed while the game runs (an arcade board's picture grew: its
    // shape too).
    const auto* av = static_cast<const retro_system_av_info*>(data);
    if (av->timing.sample_rate > 0)
      s_sample_rate = av->timing.sample_rate;
    if (av->timing.fps > 0)
    {
      // (Flycast at 30 FPS: 29.97, a picture every 2 of its frames - BuildDcVariables. Changes the pacing would
      // round away aren't logged: Flycast's 59.83 at a game's end.)
      if (std::abs(av->timing.fps / s_game_fps - 1.0) >= 0.012)
        LogF("game: the core runs at %.3f FPS now (was %.3f)", av->timing.fps, s_game_fps);
      s_game_fps = av->timing.fps;
    }
    NoteGeometry(av->geometry);
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
    // ((01.26) The Arcade's cheats, for the in-game menu: src/ArcadeCheats.h.)
    if (s_system == SystemId::ARCADE)
      ArcadeCheats::Declare(static_cast<const retro_variable*>(data));
    return true;
  }
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
    // The picture's shape, for Aspect Ratio's Auto (FinalBurn Neo sends it when a board's picture changes
    // its size; its first shape is get_system_av_info's, read as the game starts).
    if (data)
      NoteGeometry(*static_cast<const retro_game_geometry*>(data));
    return true;
  case RETRO_ENVIRONMENT_SET_ROTATION:
  {
    // A vertical arcade game: its picture turned by the app as it comes in (VideoRefresh), so the
    // upscalers, the GPU's filters and the bezels see it upright. The Arcade's; and the NAOMI's and the
    // Atomiswave's (Flycast draws the board's picture as the monitor got it, the core's image turned as it is
    // drawn: DrawPicture).
    if (!IsRomsetSystem(s_system) || !data)
      return false;
    s_rotation = *static_cast<const unsigned*>(data) & 3;
    LogF("%s: the picture turned %u degrees", s_core->name, s_rotation * 90);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_VARIABLE:
  {
    // The core sets one of its own options (FinalBurn Neo resets its cheat options so at every start). The
    // Arcade's only: PPSSPP makes up a MAC address this way (and FCEUmm asks with no option whether it may),
    // answered "no" as before for them.
    const auto* variable = static_cast<const retro_variable*>(data);
    if (s_system != SystemId::ARCADE || !variable || !variable->key)
      return false;
    // ((01.26) A cheat the player chose: that value instead of the default - off line; src/ArcadeCheats.h.)
    s_variables[variable->key] = ArcadeCheats::Starting(variable->key, variable->value ? variable->value : "");
    s_variables_changed = true;
    return true;
  }
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    // The core's map of the console's memory (00.73): RetroAchievements reads the game's memory through it, as it
    // numbers it (src/Achievements.h; copied there: cores pass arrays of their stack).
    Achievements::SetMemoryMap(static_cast<const retro_memory_map*>(data));
    return true;
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
  if (pixels)
    StartGuard::Picture();  // (the start watchdog: a picture came)
  // ((01.66) The PS Vita: a picture by the CPU while it prepares the game, by the GPU once the game runs.)
  if (pixels && IsVita(s_system))
    s_vita_loading = pixels != RETRO_HW_FRAME_BUFFER_VALID;
  if (Netplay::Replaying())
    return;  // (00.23: online, a rollback's frames run again aren't shown - the frame after them is)
  if (pixels)
    s_pictures++;  // ((01.72) the counter's FPS)
  if (pixels == RETRO_HW_FRAME_BUFFER_VALID)
  {
    // The core's own image (set_image): nothing to upload.
    if (s_defer_frame)
    {
      // Field by field: the image set_image has just put in s_pending stays (the whole struct assigned
      // dropped it, so the drawing kept an older image - one the PSP core may since have freed).
      s_pending.hw = true;
      s_pending.fresh = true;
      s_pending.frame = nullptr;
      s_pending.w = width;
      s_pending.h = height;
      s_pending.pitch = 0;
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
  if (s_gblink)
  {
    // (00.95) A link cable match: the two Game Boys' screens side by side (SameBoy's), this console's alone shown.
    unsigned first = 0, columns = width;
    GbLink::VisibleHalf(width, &first, &columns);
    pixels = static_cast<const uint8_t*>(pixels) + static_cast<size_t>(first) * (s_xrgb8888 ? 4 : 2);
    width = columns;
  }
  if (s_xrgb8888)
  {
    // 32-bit pixels: 5-6-5 bits of each (the DS draws 6 bits per colour; the picture stays the same). (Until 00.79
    // the DS's halves traded places here for R3's swap: the app places the screens now, DrawScreens.)
    s_converted.resize(static_cast<size_t>(width) * height);
    for (unsigned y = 0; y < height; y++)
    {
      const uint32_t* in =
          reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * pitch);
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
  if (s_rotation != 0)
  {
    // A vertical arcade game (SET_ROTATION): turned here, its width and height swapped, so everything
    // after sees an upright picture.
    unsigned w = 0, h = 0;
    Arcade::Rotate(static_cast<const uint16_t*>(pixels), pitch, width, height, s_rotation, &s_rotated, &w, &h);
    pixels = s_rotated.data();
    width = w;
    height = h;
    pitch = static_cast<size_t>(width) * 2;
  }
  if (s_defer_frame)
  {
    // (Field by field, as above: a pending set_image stays.)
    s_pending.hw = false;
    s_pending.fresh = true;
    s_pending.frame = pixels;
    s_pending.w = width;
    s_pending.h = height;
    s_pending.pitch = pitch;
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
  // (00.23: online, a rollback's frames run again aren't heard - their sound was played as first predicted.)
  if (s_audio && !Netplay::Replaying())
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
  // (00.90) The Jaguar's number pad from the touchpad: Virtual Jaguar reads player 1's keys as the keyboard's.
  if ((device & 0xFF) == RETRO_DEVICE_KEYBOARD)
    return s_system == SystemId::JAGUAR && s_jaguar_key >= 0 && id == JAGUAR_KEYS[s_jaguar_key] ? 1 : 0;
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
  // A NAOMI or Atomiswave light gun game played with the pad (the left stick aims): its reload, the gun shot off
  // the screen (Gun Reload in Controls).
  if (port < MAX_PLAYERS && (device & 0xFF) == RETRO_DEVICE_LIGHTGUN)
    return id == RETRO_DEVICE_ID_LIGHTGUN_RELOAD && s_gun_reload[port] ? 1 : 0;
  if (port >= MAX_PLAYERS || (device & 0xFF) != RETRO_DEVICE_JOYPAD)
    return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
    return static_cast<int16_t>(s_buttons[port]);
  return id < 16 ? static_cast<int16_t>((s_buttons[port] >> id) & 1) : 0;
}

// ---- The DualShock 4s: player 1 is the user who started the app, then the others signed in. ----
// (00.23) Each other user keeps their player while signed in (a new one takes the first free player: the players
// after them don't move when one signs out), each player's light bar shows its colour as the PS4 gives its users'
// (Multiplayer::PlayerColour: blue, red, green, pink) until the game ends, and a controller that comes or goes after
// the game's first seconds is told (changes: the in-game menu's toast).
struct Pads
{
  struct Pad
  {
    int32_t user = -1;
    int handle = -1;
    uint32_t held = 0;
    OrbisPadData data = {};
    bool ok = false;
    bool lit = false;  // its light bar has the player's colour (set again when the controller comes back)
  };
  std::array<Pad, MAX_PLAYERS> pads;
  int32_t first_user = -1;
  float recheck = 0.0f;
  // The players whose controller is there as last told, how long another state has lasted, the time since Open (the
  // first 2 s told nothing: the controllers of the game's start), the changes not taken yet (player, connected).
  std::array<bool, MAX_PLAYERS> shown{};
  std::array<float, MAX_PLAYERS> flip{};
  float age = 0.0f;
  std::vector<std::pair<int, bool>> changes;
  // (01.27, the PS1's specialist: v00.24's "player 2 not detected") Each player's failed open as last logged (its user,
  // the error: it is tried again every second, logged once), the players whose controller couldn't be opened not told
  // yet (the in-game menu's toast), the signed-in users as last logged.
  std::array<std::pair<int32_t, int>, MAX_PLAYERS> open_failed{};
  std::vector<int> failed;
  std::string users_logged = "-";

  // scePadOpen's errors (the PS4's libScePad: SCE_PAD_ERROR_*), for the log.
  static const char* OpenError(int code)
  {
    switch (static_cast<uint32_t>(code))
    {
    case 0x80920001u:
      return "invalid argument";
    case 0x80920002u:
      return "invalid port type";
    case 0x80920004u:
      return "already opened";
    case 0x80920005u:
      return "the pad library not initialized";
    case 0x80920007u:
      return "no controller connected";
    case 0x80920008u:
      return "no handle left";
    case 0x80920101u:
      return "not permitted";
    case 0x809200FFu:
      return "fatal error";
    }
    return "unknown error";
  }

  void Open()
  {
    sceUserServiceGetInitialUser(&first_user);
    Refresh(true);
  }
  // The pad's light bar back to the PS4's colour for its user, and the pad closed.
  static void Release(Pad& pad)
  {
    if (pad.handle >= 0)
    {
      if (pad.lit)
        scePadResetLightBar(pad.handle);
      scePadClose(pad.handle);
    }
    pad = {};
  }
  void Close()
  {
    for (Pad& pad : pads)
      Release(pad);
  }
  // Signed-in users in player order: player 1 the user who started the app, each other one the player they were while
  // signed in, a new one the first free player.
  void Refresh(bool log)
  {
    OrbisUserServiceLoginUserIdList users;
    std::memset(&users, 0xFF, sizeof(users));
    sceUserServiceGetLoginUserIdList(&users);
    // (01.27) The signed-in users in the log at the game's start and whenever they change (a controller's user signing
    // in: the PS4 gives the game a controller only once someone chose a user on it).
    std::string list;
    for (const int32_t user : users.userId)
    {
      if (user >= 0)
        list += (list.empty() ? "" : ", ") + std::to_string(user);
    }
    if (list != users_logged)
    {
      users_logged = list;
      LogF("pads: signed-in users: %s", list.empty() ? "none" : list.c_str());
    }
    auto signed_in = [&users](int32_t user) {
      return user >= 0 && std::find(std::begin(users.userId), std::end(users.userId), user) != std::end(users.userId);
    };
    std::array<int32_t, MAX_PLAYERS> order;
    order.fill(-1);
    const size_t others = first_user >= 0 ? 1 : 0;  // (without the first user, the others from player 1)
    order[0] = first_user;
    for (size_t i = others; i < pads.size(); i++)
    {
      if (pads[i].user != first_user && signed_in(pads[i].user))
        order[i] = pads[i].user;
    }
    for (int32_t user : users.userId)
    {
      if (user < 0 || user == first_user || std::find(order.begin(), order.end(), user) != order.end())
        continue;
      const auto free = std::find(order.begin() + others, order.end(), -1);
      if (free != order.end())
        *free = user;
    }
    for (size_t i = 0; i < pads.size(); i++)
    {
      const int32_t user = order[i];
      Pad& pad = pads[i];
      if (pad.user == user && (user < 0 || pad.handle >= 0))
        continue;
      if (pad.user >= 0 && pad.user != user)
        LogF("pads: player %zu's user %d signed out: its controller closed", i + 1, pad.user);
      Release(pad);
      pad.user = user;
      if (user >= 0)
      {
        pad.handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
        if (pad.handle >= 0)
        {
          LogF("pads: player %zu = user %d (pad %#x)", i + 1, user, static_cast<unsigned>(pad.handle));
          open_failed[i] = {};
        }
        else if (log || open_failed[i] != std::make_pair(user, pad.handle))
        {
          // (01.27) Why, once per user and error (tried again every second), and the in-game menu's toast.
          open_failed[i] = {user, pad.handle};
          LogF("pads: player %zu = user %d: its controller can't be opened (scePadOpen %#x: %s); tried again every "
               "second", i + 1, user, static_cast<unsigned>(pad.handle), OpenError(pad.handle));
          failed.push_back(static_cast<int>(i));
        }
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
    age += dt;
    for (size_t i = 0; i < pads.size(); i++)
    {
      Pad& pad = pads[i];
      // The player's colour on a controller that is there (again: it came back with its user's colour).
      if (pad.ok && !pad.lit)
      {
        const Multiplayer::Colour colour = Multiplayer::PlayerColour(static_cast<int>(i));
        OrbisPadColor bar = {colour.r, colour.g, colour.b, 0xFF};
        const int result = scePadSetLightBar(pad.handle, &bar);
        pad.lit = true;  // (tried once per connection)
        if (result != 0)
          LogF("pads: player %zu's light bar not set (%#x)", i + 1, static_cast<unsigned>(result));
      }
      else if (!pad.ok)
      {
        pad.lit = false;
      }
      // A controller there or gone for half a second (not a missed read): told once the game's first 2 s are over.
      if (age < 2.0f || pad.ok == shown[i])
      {
        if (age < 2.0f)
          shown[i] = pad.ok;
        flip[i] = 0.0f;
        continue;
      }
      flip[i] += dt;
      if (flip[i] >= 0.5f)
      {
        shown[i] = pad.ok;
        flip[i] = 0.0f;
        changes.emplace_back(static_cast<int>(i), pad.ok);
        LogF("pads: player %zu's controller %s (user %d)", i + 1, pad.ok ? "connected" : "disconnected", pad.user);
      }
    }
  }
};

// ---- Online play (00.23, src/Netplay.h) ----
// This pass's local players for the match: each player's input as the pads gave it (s_buttons and the others, by
// player: a player without a controller is neutral), up to the last player with one - the match keeps each player on
// its port. Neutral while the in-game menu is open: the match goes on, this console's players stand still.
void NetplayLocalInputs(const Pads& pads, bool menu_open)
{
  NetProto::PortInput inputs[NetProto::MAX_PORTS];
  int count = 1;
  for (int i = 0; i < NetProto::MAX_PORTS && i < MAX_PLAYERS; i++)
  {
    if (pads.pads[static_cast<size_t>(i)].ok)
      count = i + 1;
    if (menu_open)
      continue;
    NetProto::PortInput& in = inputs[i];
    in.buttons = s_buttons[static_cast<size_t>(i)];
    in.flags = s_gun_reload[static_cast<size_t>(i)] ? 1 : 0;
    in.axes[0] = s_analog[static_cast<size_t>(i)][0][0];
    in.axes[1] = s_analog[static_cast<size_t>(i)][0][1];
    in.axes[2] = s_analog[static_cast<size_t>(i)][1][0];
    in.axes[3] = s_analog[static_cast<size_t>(i)][1][1];
    in.triggers[0] = s_triggers[static_cast<size_t>(i)][0];
    in.triggers[1] = s_triggers[static_cast<size_t>(i)][1];
  }
  Netplay::SetLocalInputs(inputs, count);
}

// A frame's inputs of every port of the match on the console's ports (Netplay, before each frame it runs - those a
// rollback runs again too).
void NetplayApply(const NetProto::PortInput* ports, int count)
{
  for (int p = 0; p < MAX_PLAYERS; p++)
  {
    const NetProto::PortInput in = p < count && p < NetProto::MAX_PORTS ? ports[p] : NetProto::PortInput();
    const size_t i = static_cast<size_t>(p);
    s_buttons[i] = in.buttons;
    s_gun_reload[i] = (in.flags & 1) != 0;
    s_analog[i][0] = {in.axes[0], in.axes[1]};
    s_analog[i][1] = {in.axes[2], in.axes[3]};
    s_triggers[i] = {in.triggers[0], in.triggers[1]};
  }
}

// A guest: the host's core options before its core starts (Netplay::AgreeOptions: they reach it in a moment, the
// host sends them as its own game starts), the screen saying so meanwhile. False: the match ended (*error: why).
bool NetplayAgree(Display& display, ImFont* regular, ImFont* bold, std::string* error)
{
  StartGuard::Step("online: the host's settings");
  for (;;)
  {
    StartGuard::Beat();
    const Netplay::Wait wait = Netplay::AgreeOptions(&s_variables);
    if (wait == Netplay::Wait::Done)
      return true;
    if (wait == Netplay::Wait::Failed)
    {
      // ((01.23) No reason: the match went back to the room - Back to the Room, a player who left - while our game
      // loaded: no error, the room shows again.)
      std::string why;
      Netplay::Ended(&why);
      *error = why;
      return false;
    }
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    display.BeginFrame();
    ImGui::NewFrame();
    NetplayUi::DrawWaiting(ImGui::GetForegroundDrawList(), regular, bold, Netplay::GetStatus().line);
    ImGui::Render();
    display.EndFrame();
  }
}

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
  std::string want;  // what it was chosen for: the setting's value and the picture's shape (LoadBezel)
  ImTextureID texture{};
  ImVec2 min, max;  // the window on the screen
};
Bezel s_bezel;

Picture PlacePicture(const std::string& game, bool framed = true);

// The frames every console has (00.63: assets/xmb/bezels/frame-<name>.png), besides the Retro TV (tv.png) and the
// console's own (<console>-<name>.png).
bool SharedFrame(const std::string& choice)
{
  return choice == "cabinet" || choice == "minimal" || choice == "neon" || choice == "wood" || choice == "carbon" ||
         choice == "stars";
}

// The picture's shape on a screen without a bezel (width / height; 0 before the game's first picture).
float PictureRatio(const std::string& game)
{
  if (s_frame_w == 0 || s_frame_h == 0)
    return 0.0f;
  const Picture picture = PlacePicture(game, false);
  return (picture.max.x - picture.min.x) / std::max(1.0f, picture.max.y - picture.min.y);
}

// Which window of a built-in frame suits a picture of that shape (PictureRatio; BezelPath): "" a 4:3 one, "-tall" a
// picture taller than wide (the DS's and the 3DS's screens one above the other, a vertical arcade game), "-169" a
// widescreen one (the PSP, a big top screen), "-wide" two screens side by side.
std::string BezelShape(float ratio)
{
  if (ratio <= 0.0f)
    return {};
  return ratio < 0.95f ? "-tall" : ratio <= 1.55f ? "" : ratio <= 2.2f ? "-169" : "-wide";
}

// A built-in frame (assets/xmb/bezels/<name>.png, a 4:3 window), or its window for the picture's shape when it has
// one (<name>-tall.png, -169.png, -wide.png; side by side screens take a 16:9 one after their own).
std::string BuiltinBezel(const std::string& name, const std::string& shape)
{
  const std::string builtin = std::string(App::ASSETS_DIR) + "/xmb/bezels/" + name;
  for (const std::string& variant : {shape, std::string(shape == "-wide" ? "-169" : "")})
  {
    if (!variant.empty() && AssetPack::Exists(builtin + variant + ".png"))
      return builtin + variant + ".png";
  }
  return builtin + ".png";
}

// The console's own bezel - its Bezel setting's default: <console>-console.png, the Arcade's Arcade Cabinet - for a
// game Per Game has no bezel for (00.80); the Retro TV for a console without a drawing of its own.
std::string OwnBezel(const System& system, const std::string& shape)
{
  const Settings::Setting* setting = Settings::Find(std::string(system.key) + "_bezel");
  const std::string own = setting && setting->fallback ? setting->fallback : "console";
  if (own == "off" || own == "game" || own == "custom")
    return {};
  if (own == "tv")
    return BuiltinBezel("tv", shape);
  if (SharedFrame(own))
    return BuiltinBezel("frame-" + own, shape);
  const std::string first = std::string(App::ASSETS_DIR) + "/xmb/bezels/" + system.key + "-" + own + ".png";
  return AssetPack::Exists(first) ? BuiltinBezel(std::string(system.key) + "-" + own, shape) :
                                    BuiltinBezel("tv", shape);
}

// A bezel of the data folder (Your Own, Per Game): data_dir/bezels/...
bool DataBezel(const std::string& path)
{
  const std::string folder = Portable::UserDir() + "/bezels/";  // (00.87: the player's data, src/Portable.h)
  return path.compare(0, folder.size(), folder) == 0;
}

// The bezel file the game's console is set to ("": none), for a picture of that shape (BezelShape). Your Own:
// bezels/<console>/ in the data folder with the ROM's name (as The Bezel Project names its bezels) or the game's
// title, else bezels/<console>.png; without one, the console's own built-in bezel (else the Retro TV). Per Game
// (00.80): the game's own from The Bezel Project, which Settings -> Game Bezels downloads into bezels/<console>/ with
// the ROM's name (src/BezelProject.h), else the console's own.
std::string BezelPath(const Library::Game& game, const std::string& shape)
{
  const System& system = GetSystem(game.system);
  const std::string choice = Settings::Get(std::string(system.key) + "_bezel", game.key);
  if (choice.empty() || choice == "off")
    return {};
  const std::string builtin = std::string(App::ASSETS_DIR) + "/xmb/bezels/";
  if (choice == "tv")
    return BuiltinBezel("tv", shape);
  if (SharedFrame(choice))
    return BuiltinBezel("frame-" + choice, shape);
  const std::string own = Portable::UserDir() + "/bezels/" + system.key;  // (00.87: src/Portable.h)
  if (choice == "game")
  {
    for (const std::string& path : {own + "/" + game.id + ".png", own + "/" + game.title + ".png"})
    {
      if (FileExists(path))
        return path;
    }
    LogF("bezel: no bezel of The Bezel Project's for %s in %s (Settings -> Game Bezels): the console's own",
         game.id.c_str(), own.c_str());
    return OwnBezel(system, shape);
  }
  if (choice != "custom")
    return BuiltinBezel(std::string(system.key) + "-" + choice, shape);
  for (const std::string& path : {own + "/" + game.id + ".png", own + "/" + game.title + ".png", own + ".png"})
  {
    if (FileExists(path))
      return path;
  }
  LogF("bezel: none of your own in %s", own.c_str());
  const std::string first = builtin + system.key + "-console.png";
  return AssetPack::Exists(first) ? BuiltinBezel(std::string(system.key) + "-console", shape) :
                                    BuiltinBezel("tv", shape);
}

void FreeBezel(Display& display)
{
  if (s_bezel.texture)
    display.DestroyTexture(s_bezel.texture);
  s_bezel = {};
}

// A bezel's PNG read and its window found (BezelProject::FindWindow: see-through from the middle out, fully
// see-through there); fitted (00.80: a bezel of the data folder, once the picture's shape is known): the window where a
// picture of that shape fits with no pixel of the bezel over it (The Bezel Project's TV screens have rounded, bulging
// edges). fit: how big a picture of that shape is in it against the whole screen (1: as big).
struct BezelImage
{
  ImageRGBA image;
  BezelProject::Window window;
  float fit = 0.0f;
  bool fitted = false;
};

bool ReadBezel(const std::string& path, float ratio, bool fitted, BezelImage* out)
{
  if (!LoadPng(path, &out->image, Display::WIDTH, Display::HEIGHT))
  {
    LogF("bezel: %s can't be read", path.c_str());
    return false;
  }
  const float W = static_cast<float>(Display::WIDTH), H = static_cast<float>(Display::HEIGHT);
  out->fitted = fitted && ratio > 0.0f;
  out->window = BezelProject::FindWindow(&out->image, out->fitted ? ratio : 0.0f, W, H);
  if (!out->window.found)
  {
    LogF("bezel: %s%s", path.c_str(), out->window.why.c_str());
    return false;
  }
  out->fit = BezelProject::FitScale(out->window, ratio, out->image.width, out->image.height, W, H);
  return true;
}

// The bezel read as the texture drawn, its window where the picture goes.
void ShowBezel(Display& display, const std::string& path, const BezelImage& bezel)
{
  s_bezel.texture = display.CreateTexture(bezel.image.pixels.data(), bezel.image.width, bezel.image.height);
  if (!s_bezel.texture)
    return;
  // The PNG fills the screen (a smaller one is stretched).
  const float sx = static_cast<float>(Display::WIDTH) / static_cast<float>(bezel.image.width);
  const float sy = static_cast<float>(Display::HEIGHT) / static_cast<float>(bezel.image.height);
  s_bezel.min = ImVec2(bezel.window.left * sx, bezel.window.top * sy);
  s_bezel.max = ImVec2(bezel.window.right * sx, bezel.window.bottom * sy);
  LogF("bezel: %s, window %.0fx%.0f at %.0f,%.0f%s", path.c_str(), s_bezel.max.x - s_bezel.min.x,
       s_bezel.max.y - s_bezel.min.y, s_bezel.min.x, s_bezel.min.y, bezel.fitted ? " (fitted to the picture)" : "");
}

// Shows the bezel the settings choose for the picture's shape, when that changed: it is called at every pass of the
// game's loop (a choice made in the in-game menu, R3's other DS view, a picture of a new shape), so the file is looked
// for only when the choice or the shape changes (a bezel of the data folder: the picture's shape to the hundredth,
// its window being fitted to it).
void LoadBezel(Display& display, const Library::Game& game)
{
  const System& system = GetSystem(game.system);
  const std::string choice = Settings::Get(std::string(system.key) + "_bezel", game.key);
  const float ratio = choice == "off" ? 0.0f : PictureRatio(game.key);
  const std::string shape = BezelShape(ratio);
  char fitted_to[24] = "";
  if (choice == "custom" || choice == "game")
    std::snprintf(fitted_to, sizeof(fitted_to), "|%.2f", ratio);
  const std::string want = choice + shape + fitted_to;
  if (want == s_bezel.want)
    return;
  const std::string path = BezelPath(game, shape);
  if (path == s_bezel.path && !DataBezel(path))
  {
    s_bezel.want = want;
    return;
  }
  FreeBezel(display);
  s_bezel.want = want;
  // (A bezel of the data folder waits for the game's first picture: its window is fitted to the picture's shape.)
  if (DataBezel(path) && ratio <= 0.0f)
    return;
  s_bezel.path = path;
  if (path.empty())
    return;
  BezelImage bezel;
  const bool read = ReadBezel(path, ratio, DataBezel(path), &bezel);
  const bool per_game = choice == "game" && DataBezel(path);
  if (!per_game || (read && (ratio <= 0.0f || bezel.fit >= BezelProject::MIN_FIT)))
  {
    if (read)
      ShowBezel(display, path, bezel);
    return;
  }
  // Per Game (00.80): the game's bezel can't be shown, or it is made for another shape of picture (the DS's screens
  // side by side in a bezel made for them one above the other, a 16:9 picture in a TV made for 4:3): the console's own,
  // when it shows the picture clearly bigger (a frame of its own for that shape: the DS's, the 3DS's, the frames').
  const std::string fallback = OwnBezel(system, shape);
  BezelImage own;
  const bool own_read = !fallback.empty() && ReadBezel(fallback, ratio, false, &own);
  if (read && (!own_read || own.fit < bezel.fit + 0.05f))
  {
    LogF("bezel: %s is made for another shape of picture (this one is %.0f%% of its size in it), but the console's "
         "own isn't bigger: kept",
         path.c_str(), bezel.fit * 100.0f);
    ShowBezel(display, path, bezel);
    return;
  }
  if (read)
  {
    LogF("bezel: %s is made for another shape of picture (this one would be %.0f%% of its size in it, %.0f%% in the "
         "console's own): the console's own",
         path.c_str(), bezel.fit * 100.0f, own.fit * 100.0f);
  }
  s_bezel.path = fallback;
  if (own_read)
    ShowBezel(display, fallback, own);
}

void DrawBezel(ImDrawList* dl)
{
  if (s_bezel.texture)
  {
    dl->AddImage(ImTextureRef(s_bezel.texture), ImVec2(0.0f, 0.0f),
                 ImVec2(static_cast<float>(Display::WIDTH), static_cast<float>(Display::HEIGHT)));
  }
}

// Tube TV (Graphics: an old tube TV's look, GpuFilter's Mode::Crt): its look, 1-4 (0: off).
int CrtLook(const std::string& game)
{
  const std::string look = Settings::Get("crt", game);
  return look == "tv" ? 1 : look == "arcade" ? 2 : look == "studio" ? 3 : look == "strong" ? 4 : 0;
}

void DrawScanlines(ImDrawList* dl, const Picture& picture, const std::string& game)
{
  const std::string scanlines = Settings::Get("scanlines", game);
  if (scanlines == "off" || CrtLook(game) != 0)
    return;  // (Tube TV draws lines of its own)
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
  // Tube TV, on every console: it takes the console's own picture, as a TV did (the GPU Upscaler and the CPU's
  // Resolution are left out; the NTSC filter's picture goes through it).
  if (CrtLook(game) != 0)
    return GpuFilter::Mode::Crt;
  // ((01.76, the owner: «el escalador por gpu… ocultarlo del menú y dejar nativamente que funcione»; «uno para que no se
  // vea como diente de sierra») The PS Vita: always Smooth Edges - SMAA on its 960x544, Catmull-Rom to the screen -
  // whatever its Quality or GPU Upscaler were before (both are off its pages, Settings.cpp FOR_ALL_BUT_VITA). Its
  // loading pictures, by the CPU, as they are (Smooth Edges would take them for pixel art: MMPX).
  if (IsVita(s_system))
    return s_frame_hw ? GpuFilter::Mode::Edges : GpuFilter::Mode::Off;
  // (The 3DS's picture, drawn by the GPU, takes the GPU Upscaler as the other 3D consoles' since 00.71, below: until
  // then Azahar's own only.)
  // A vertical NAOMI or Atomiswave game: its picture is turned as it is drawn (DrawPicture), which of the filters only
  // Tube TV does (above); the others would draw it on its side. (Flycast's consoles had no GPU filter from 00.53 to
  // 00.68; since 00.69 the Quality levels' steps, below.)
  if (IsFlycastBoard(s_system) && (s_rotation & 3) != 0)
    return GpuFilter::Mode::Off;
  const std::string name = Settings::Get("gpu_filter", game);
  GpuFilter::Mode mode = name == "pixel"      ? GpuFilter::Mode::PixelArt :
                         name == "ai"         ? GpuFilter::Mode::AI :
                         name == "fsr"        ? GpuFilter::Mode::Smooth :
                         name == "sharpen"    ? GpuFilter::Mode::Sharpen :
                         name == "edges"      ? GpuFilter::Mode::Edges :
                         name == "natural"    ? GpuFilter::Mode::NaturalHD :
                         name == "natural_ai" ? GpuFilter::Mode::NaturalAI :
                                                GpuFilter::Mode::Off;
  // Smooth Edges, Natural HD and Natural AI suit every console and size as they are: GpuFilter redraws pixel art and
  // only anti-aliases drawn (3D) pictures (the source's kind, DrawPicture), and leaves out what a picture as big as the
  // screen doesn't need.
  const bool steps = mode == GpuFilter::Mode::Edges || mode == GpuFilter::Mode::NaturalHD ||
                     mode == GpuFilter::Mode::NaturalAI;
  if (s_frame_hw)
  {
    // A 3D picture drawn by the GPU (the N64, the PlayStation, the PSP, Flycast's consoles, the 3DS) has no pixel art:
    // Pixel Art HD and AI - the Quality levels' High and Ultra since 00.69 - are Smooth Edges (anti-aliased at the size
    // drawn) and Natural HD (sharpened) for it. Anime4K's network works at about 768x576 to 960x720: on an HD picture
    // it only made it smaller and softer (00.69, PC, the filter harness: Tekken 3 at 4x 6.4 of fine detail with Natural
    // AI against Natural HD's 14.4, GoldenEye at 2x 2.3 against 4.4), so Natural AI on a picture drawn bigger than that
    // is Natural HD too (the PSP's at 4x since 00.46). Until 00.68 the PlayStation from 3x and the PSP at 3x took the
    // AI, the rest Smooth HD.
    // (The 3DS: the picture the filter gets, its screens placed by the app - DrawScreens: the screens at one size, or
    // Large Screen + Small's big one.)
    double pixels = double(s_frame_w) * s_frame_h;
    Screens screens;
    if (CurrentScreens(game, &screens))
    {
      const double f = std::clamp(s_3ds_scale, 1, 4);
      const DualScreens::Layout& placed = screens.layout;
      const DualScreens::Rect& big = placed.shown[std::clamp(placed.big, 0, 1)].at;
      pixels = (placed.same_scale ? double(placed.width) * placed.height : double(big.w) * big.h) * f * f;
    }
    if (mode == GpuFilter::Mode::PixelArt)
      mode = GpuFilter::Mode::Edges;
    else if (mode == GpuFilter::Mode::AI || (mode == GpuFilter::Mode::NaturalAI && pixels > 960.0 * 720.0))
      mode = GpuFilter::Mode::NaturalHD;
    // Smooth HD (AMD FSR) makes a picture bigger: on one as big as the screen's already (the N64 at 4x, Flycast at 3x,
    // the 3DS from 3x) it only sharpens.
    if (mode == GpuFilter::Mode::Smooth &&
        ((s_system == SystemId::N64 && s_frame_w >= 2560) || (IsFlycast(s_system) && s_frame_h > 960) ||
         (s_system == SystemId::THREEDS && s_3ds_scale >= 3)))
      mode = GpuFilter::Mode::Sharpen;
  }
  // (The DS's big screen made 3x bigger by the core - Smooth HD on it - is gone since 00.80: its picture is always the
  // two screens at their own size, DualScreens.)
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
         mode == GpuFilter::Mode::Edges || mode == GpuFilter::Mode::NaturalHD || mode == GpuFilter::Mode::NaturalAI ||
         mode == GpuFilter::Mode::Crt;
}

// ((01.72, the master: the owner found Uncharted on the PS Vita too dark) Graphics > Brightness and Saturation,
// percents in settings.ini: drawn by the GPU filter's last pass, GpuFilter.h Source; Tube TV keeps its own look.
// ((01.76, the owner: «el de gamma se lo podrías sacar») Gamma is off the menus and 1.0 always: a picture_gamma saved
// before (1.20 to 1.50 on Uncharted and SFxT) isn't read.)
void SetColours(GpuFilter::Source* source, const std::string& game)
{
  auto percent = [&game](const char* key) {
    const std::string value = Settings::Get(key, game);
    char* end = nullptr;
    const long number = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() && number >= 0 && number <= 400 ? static_cast<float>(number) / 100.0f : 1.0f;
  };
  source->brightness = percent("picture_brightness");
  source->gamma = 1.0f;
  source->saturation = percent("picture_saturation");
}

// The mode a picture is drawn with: the GPU Upscaler's (GpuMode), or - it being Off and the colours not the picture's
// own - HD Sharpen's pass without sharpening (plain: read smooth or sharp as Texture Filtering would draw it), which
// draws them. Not for a vertical NAOMI or Atomiswave game (drawn turned by ImGui's quad; the filters can't).
GpuFilter::Mode WithColours(GpuFilter::Mode gpu, GpuFilter::Source* source, const std::string& game, bool smooth)
{
  SetColours(source, game);
  if (gpu != GpuFilter::Mode::Off || !source->Graded() || (IsFlycastBoard(s_system) && (s_rotation & 3) != 0))
    return gpu;
  source->plain = true;
  source->plain_smooth = smooth;
  return GpuFilter::Mode::Sharpen;
}

Picture PlacePicture(const std::string& game, bool framed)
{
  Picture picture;
  // The screen, or the bezel's window (framed: as it is drawn; else as if there were none, PictureRatio).
  ImVec2 area(0.0f, 0.0f);
  float W = static_cast<float>(Display::WIDTH), H = static_cast<float>(Display::HEIGHT);
  if (s_bezel.texture && framed)
  {
    area = s_bezel.min;
    W = s_bezel.max.x - s_bezel.min.x;
    H = s_bezel.max.y - s_bezel.min.y;
  }
  // The picture in the console's units: 256 wide (512 hi-res, 602 with the NTSC filter), 224 or
  // 239 lines (448 / 478 interlaced).
  // (The DS and the 3DS: their two screens as the app places them, in the console's pixels - DualScreens: One Above the
  // Other 256x384 or 400x480, Side by Side 512x192 or 720x240, with the gap; Large Screen + Small the big screen's
  // pixels, the small one beside it; one screen 256x192, 400x240 or 320x240.)
  Screens screens;
  const bool two = CurrentScreens(game, &screens);
  const float frame_w = two ? screens.layout.width : static_cast<float>(s_frame_w);
  float frame_h = two ? screens.layout.height : static_cast<float>(s_frame_h);
  if (two)
  {
    // (in the console's pixels already)
  }
  else if (s_system == SystemId::PS1)
    frame_h /= static_cast<float>(std::clamp(Settings::GetInt("ps1_resolution", game), 1, 4));  // SwanStation's scale
  else if (s_system == SystemId::PSP)
    frame_h /= std::max(1.0f, std::round(static_cast<float>(s_frame_w) / 480.0f));  // PPSSPP's (480x270 at 1x)
  else if (IsFlycast(s_system))
    frame_h /= std::max(1.0f, std::floor(frame_h / 480.0f));  // Flycast's internal resolution (480 lines at 1x)
  else if (s_system == SystemId::THREEDS)
    frame_h /= static_cast<float>(std::clamp(s_3ds_scale, 1, 4));  // (Azahar's picture: 3DS Resolution's size)
  else if (s_frame_hw && s_frame_w >= 1280)
    frame_h /= static_cast<float>(s_frame_w / 640);  // ParaLLEl-RDP's upscaled scanout
  // A NAOMI's or Atomiswave's vertical game: the core's picture turned as it is drawn (DrawPicture), so its width
  // is the picture's height on the TV (at Flycast's 1x).
  const bool turned = IsFlycastBoard(s_system) && (s_rotation & 1) != 0;
  if (turned)
    frame_h = static_cast<float>(s_frame_w) / std::max(1.0f, std::floor(static_cast<float>(s_frame_h) / 480.0f));
  // (The DS: two screens; an arcade board's lines are its own, 224 to 480; the 3DS's are counted above.)
  const bool own_lines = s_system == SystemId::NDS || s_system == SystemId::ARCADE || s_system == SystemId::THREEDS;
  const float lines = frame_h > 300.0f && !own_lines ? frame_h * 0.5f : frame_h;
  picture.rows = lines;
  const std::string aspect = Settings::Get("aspect", game);
  float ratio = 4.0f / 3.0f;
  // A handheld's screen keeps its shape (10:9, the Game Boy Advance's 3:2) at Auto and Pixel
  // Perfect: its dots are square. The Super Game Boy's 256x224 picture was a TV's. The Game Gear's
  // LCD has the grid between its dots too, its shape 4:3 at Auto (Genesis Plus GX's 6:5 pixels).
  const bool screen = (IsHandheld(s_system) && s_frame_w != 256) || s_system == SystemId::NDS;
  // (The DS's grid is drawn on each screen, DrawScreens: the whole picture's for the screens at one size.)
  if ((screen || (HasLcd(s_system) && s_frame_w != 256)) && (!two || screens.layout.same_scale))
    picture.columns = frame_w;
  if (screen && (aspect == "auto" || aspect == "pixel"))
    ratio = frame_w / std::max(1.0f, frame_h);
  else if (s_system == SystemId::PS1 && Settings::Get("ps1_widescreen", game) == "on" && aspect != "stretch")
    ratio = 16.0f / 9.0f;  // the widescreen hack's picture
  else if (s_system == SystemId::N64 && s_n64_widescreen && aspect != "stretch")
    ratio = 16.0f / 9.0f;  // (01.58) N64 Widescreen: the game's own 16:9 3D, not stretched (Aspect Ratio aside)
  else if (s_system == SystemId::PSP && (aspect == "auto" || aspect == "pixel"))
    ratio = static_cast<float>(s_frame_w) / std::max(1.0f, static_cast<float>(s_frame_h));  // its 16:9 screen
  else if (s_system == SystemId::THREEDS && (aspect == "auto" || aspect == "pixel"))
    ratio = two ? frame_w / std::max(1.0f, frame_h) :  // its screens' pixels are square
                  static_cast<float>(s_frame_w) / std::max(1.0f, static_cast<float>(s_frame_h));
  else if (s_system == SystemId::ARCADE && aspect == "auto" && s_core_aspect > 0.0f)
    ratio = s_core_aspect;  // the board's own shape (a vertical game's 3:4), as FinalBurn Neo reports it
  else if (s_system == SystemId::ARCADE && (aspect == "auto" || aspect == "pixel"))
    ratio = frame_w / std::max(1.0f, frame_h);  // its pixels square (the picture is turned already)
  else if (turned && (aspect == "auto" || aspect == "pixel"))
    ratio = 3.0f / 4.0f;  // a vertical NAOMI or Atomiswave game: its monitor's 4:3 standing upright
  else if (aspect == "pixel" && IsAtari(s_system) && s_core_aspect > 0.0f)
    ratio = s_core_aspect;  // (00.90) the console's own pixels' shape, as its core reports it (the 2600's are wide)
  else if (aspect == "pixel" && (IsGpgx(s_system) || s_system == SystemId::PCE))
    ratio = frame_w / std::max(1.0f, lines);  // the Mega Drive's 320 or 256 pixels, the GG's 160; the PC Engine's 256 to
                                              // 352 (00.82)
  else if (aspect == "pixel" && s_system != SystemId::N64 && s_system != SystemId::PS1 && s_system != SystemId::PSP &&
           !IsFlycast(s_system))
    ratio = 256.0f / std::max(1.0f, lines);  // (the N64's, the PlayStation's and Flycast's pictures are 4:3 already)
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

// The 3DS's stylus on the touch screen, drawn where the touch screen is shown (`touch`, on the TV: DrawScreens) while
// the finger is on the touchpad or the tap button is down, and a moment after (the core's own cursor is off:
// Build3dsVariables). s_3ds_at is where it is in the core's picture, in its pixels; `from` is the touch screen's place
// there.
void Draw3dsStylus(ImDrawList* dl, const DualScreens::Rect& touch, const DualScreens::Rect& from)
{
  if (s_3ds_cursor <= 0 || from.w <= 0.0f || from.h <= 0.0f)
    return;
  s_3ds_cursor--;
  const float x = touch.x + (s_3ds_at[0] - from.x) / from.w * touch.w;
  const float y = touch.y + (s_3ds_at[1] - from.y) / from.h * touch.h;
  const float arm = touch.w * 0.0165f + 6.0f;  // (as big as on 00.79's One Above the Other, for its touch screen)
  const ImU32 dark = IM_COL32(0, 0, 0, 220), light = IM_COL32(255, 255, 255, 240);
  dl->AddLine(ImVec2(x - arm, y), ImVec2(x + arm, y), dark, 5.0f);
  dl->AddLine(ImVec2(x, y - arm), ImVec2(x, y + arm), dark, 5.0f);
  dl->AddLine(ImVec2(x - arm, y), ImVec2(x + arm, y), light, 2.0f);
  dl->AddLine(ImVec2(x, y - arm), ImVec2(x, y + arm), light, 2.0f);
  if (s_stylus.down)
    dl->AddCircle(ImVec2(x, y), arm * 0.7f, light, 16, 2.0f);
}

// Tube TV's look and screen for the picture DrawPicture hands GpuFilter: one tube, with the console's lines (the DS's
// and the 3DS's two screens are a tube each: DrawScreens).
void CrtScreens(GpuFilter::Source* source, const Picture& picture, const std::string& game)
{
  source->crt = std::max(1, CrtLook(game));
  source->screen_count = 1;
  source->screens[0] = GpuFilter::Source::Screen{};
  source->screens[0].lines = picture.rows;
  // A vertical NAOMI or Atomiswave game: its image turned as DrawPicture turns it (a turned monitor's lines).
  source->rotation = IsFlycastBoard(s_system) ? static_cast<int>(s_rotation & 3) : 0;
}

// The DS's and the 3DS's two screens drawn where the screens' settings place them (00.80, src/DualScreens.h), from the
// core's picture of both side by side: the DS's in the game texture's corner (s_texture_w x s_texture_h: the CPU's
// Resolution can have made it bigger), the 3DS's the core's own image. With the GPU's filters, screens at one size
// (One Above the Other, Side by Side, one screen) are one picture for them - the parts copied to their places, the gap
// black (GpuFilter.h Source::parts) -, so each filter works as on any other console's picture; Large Screen + Small: the
// big screen through the filter, the small one drawn plain and smooth (as the DS's Big Screen since 00.40). Tube TV:
// each screen a tube of its own. Without a filter: each screen from the image. Scanlines on each screen, with its own
// lines (the DS's handheld grid); the 3DS's stylus on its touch screen wherever it is shown.
struct ScreensSource
{
  VkImageView view = VK_NULL_HANDLE;  // for the filters
  VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  bool exact = false;                 // the image is the picture (the 3DS's), else it is in its top-left corner
  GpuFilter::Source::Kind kind = GpuFilter::Source::Kind::PixelArt;
  uint32_t width = 0, height = 0;     // the core's picture in the image, in the image's pixels
  float scale = 1.0f;                 // the image's pixels per pixel of the core's picture (the CPU's Resolution)
  float console = 1.0f;               // the core's picture's pixels per pixel of the console's screens (3DS Resolution)
  float uv_w = 1.0f, uv_h = 1.0f;     // the image's size (ImGui's texture coordinates)
  std::function<ImTextureID(bool)> texture;  // the image for ImGui, with linear filtering (true) or nearest
  bool upscaled = false;              // the picture made bigger than the console's own: always drawn smooth
};

void DrawScreens(ImDrawList* dl, const ScreensSource& src, const Screens& screens, const Picture& picture,
                 const std::string& game, GpuFilter::Mode gpu)
{
  const DualScreens::Layout& layout = screens.layout;
  const int count = std::clamp(layout.count, 1, 2);
  const bool threeds = s_system == SystemId::THREEDS;
  const float lines = threeds ? 240.0f : 192.0f;
  // Each screen shown: its place on the TV, and where it is in the image (its pixels).
  DualScreens::Rect tv[2], from[2];
  for (int i = 0; i < count; i++)
  {
    tv[i] = DualScreens::OnScreen(layout.shown[i].at, layout, picture.min.x, picture.min.y, picture.max.x,
                                  picture.max.y);
    const DualScreens::Rect& r = screens.source[layout.shown[i].screen];
    from[i] = {r.x * src.scale, r.y * src.scale, r.w * src.scale, r.h * src.scale};
  }
  // ((01.72) The colours - Brightness, Gamma, Saturation - when the screens are one picture at one size: Large Screen +
  // Small's small screen is drawn without the filter, so neither of the two takes them there.)
  const bool smooth_all = src.upscaled || Settings::Get("filter", game) == "smooth";
  GpuFilter::Source graded;
  const GpuFilter::Mode mode = layout.same_scale ? WithColours(gpu, &graded, game, smooth_all) : gpu;
  auto base = [&src, &graded]() {
    GpuFilter::Source source = graded;
    source.view = src.view;
    source.layout = src.layout;
    source.exact = src.exact;
    source.kind = src.kind;
    source.width = src.width;
    source.height = src.height;
    return source;
  };
  auto rounded = [](float v) { return static_cast<uint32_t>(std::lround(std::max(0.0f, v))); };
  bool filtered[2] = {false, false};
  if (gpu == GpuFilter::Mode::Crt)
  {
    // Both screens in one pass over the whole picture, each a tube of its own: its place in the picture drawn, its
    // place in the source (the core's whole picture), its own lines.
    GpuFilter::Source source = base();
    source.crt = std::max(1, CrtLook(game));
    source.screen_count = count;
    const float sw = static_cast<float>(std::max(1u, src.width)), sh = static_cast<float>(std::max(1u, src.height));
    for (int i = 0; i < count; i++)
    {
      const DualScreens::Rect& at = layout.shown[i].at;
      GpuFilter::Source::Screen& screen = source.screens[i];
      const float place[4] = {at.x / layout.width, at.y / layout.height, at.w / layout.width, at.h / layout.height};
      const float origin[4] = {from[i].x / sw, from[i].y / sh, from[i].w / sw, from[i].h / sh};
      std::copy(std::begin(place), std::end(place), screen.place);
      std::copy(std::begin(origin), std::end(origin), screen.from);
      screen.lines = lines;
    }
    filtered[0] = filtered[1] = GpuFilter::Draw(dl, gpu, source, picture.min, picture.max);
  }
  else if (mode != GpuFilter::Mode::Off && (!s_texture_scaled || threeds || !GpuRedraws(mode)))
  {
    GpuFilter::Source source = base();
    if (layout.same_scale)
    {
      // One picture of the screens at their places, the gap black: the image's pixels per console pixel.
      const float px = src.scale * src.console;
      source.width = rounded(layout.width * px);
      source.height = rounded(layout.height * px);
      source.part_count = count;
      for (int i = 0; i < count; i++)
      {
        const DualScreens::Rect& at = layout.shown[i].at;
        source.parts[i] = {rounded(from[i].x), rounded(from[i].y), rounded(from[i].w), rounded(from[i].h),
                           rounded(at.x * px), rounded(at.y * px)};
      }
      filtered[0] = filtered[1] = GpuFilter::Draw(dl, mode, source, picture.min, picture.max);
    }
    else
    {
      // The big screen alone through the filter.
      const int big = std::clamp(layout.big, 0, count - 1);
      source.width = rounded(from[big].w);
      source.height = rounded(from[big].h);
      source.part_count = 1;
      source.parts[0] = {rounded(from[big].x), rounded(from[big].y), source.width, source.height, 0, 0};
      filtered[big] = GpuFilter::Draw(dl, gpu, source, ImVec2(tv[big].x, tv[big].y),
                                      ImVec2(tv[big].x + tv[big].w, tv[big].y + tv[big].h));
    }
  }
  // What no filter drew: each screen from the image, half a texel inside when smooth (the other screen is beside it
  // in the image); the small screen of Large Screen + Small always smooth (smooth_all: above).
  for (int i = 0; i < count; i++)
  {
    if (filtered[i])
      continue;
    const bool smooth = smooth_all || (!layout.same_scale && i != layout.big);
    const float inset = smooth ? 0.5f : 0.0f;
    const ImVec2 uv0((from[i].x + inset) / src.uv_w, (from[i].y + inset) / src.uv_h);
    const ImVec2 uv1((from[i].x + from[i].w - inset) / src.uv_w, (from[i].y + from[i].h - inset) / src.uv_h);
    dl->AddImage(ImTextureRef(src.texture(smooth)), ImVec2(tv[i].x, tv[i].y),
                 ImVec2(tv[i].x + tv[i].w, tv[i].y + tv[i].h), uv0, uv1);
  }
  // Scanlines, each screen with its own (Tube TV draws its own: DrawScanlines leaves them out).
  for (int i = 0; i < count; i++)
  {
    Picture each;
    each.min = ImVec2(tv[i].x, tv[i].y);
    each.max = ImVec2(tv[i].x + tv[i].w, tv[i].y + tv[i].h);
    each.rows = lines;
    each.columns = threeds ? 0.0f : 256.0f;  // (the DS: the handheld's grid, as on the Game Boys)
    DrawScanlines(dl, each, game);
  }
  if (threeds)
  {
    bool shown = false;
    for (int i = 0; i < count; i++)
    {
      if (layout.shown[i].screen == 1)
      {
        Draw3dsStylus(dl, tv[i], screens.source[1]);
        shown = true;
      }
    }
    if (!shown)
      s_3ds_cursor = 0;  // (the touch screen isn't shown)
  }
}

void DrawPicture(ImDrawList* dl, Display& display, const std::string& game)
{
  if (s_system == SystemId::THREEDS)
    ThreedsRemadeTrace();  // ((01.09) the drawings after its renderer was made again)
  if (s_frame_w == 0 || s_frame_h == 0)
    return;
  if (s_frame_hw)
  {
    // The core's image, whole.
    if (!s_hw_image_set)
      return;
    const Picture picture = PlacePicture(game);
    const GpuFilter::Mode gpu = GpuMode(game);
    // The core's image read as opaque (alpha 1) where ImGui's quad draws it, which blends it over black by its alpha.
    // The PSP's keeps the PSP's alpha (often 0); ((01.76) the PS Vita's too: with no GPU scaler, God of War was black
    // and Uncharted dark; and every core's drawn by the GPU, as their alpha isn't a picture's either: the PlayStation's
    // the VRAM's mask bit, the N64's the RDP's coverage.) The GPU filters read only the colours and write alpha 1: they
    // take the image as it is. OpaqueView waits for the frame EndFrame records next - as ExternalTexture does right
    // after it, so no wait more - and is asked once a frame (the 3DS's two screens share it: a second call would
    // destroy the view the first screen's descriptor holds).
    const VkImageView view = s_hw_image.image_view;
    VkImageView opaque = VK_NULL_HANDLE;
    bool opaque_asked = false;
    auto quad_view = [&display, &opaque, &opaque_asked, view]() {
      if (!opaque_asked)
      {
        opaque_asked = true;
        opaque = display.OpaqueView(s_hw_image.create_info);
      }
      return opaque != VK_NULL_HANDLE ? opaque : view;
    };
    // The 3DS's two screens: placed by the app (00.80), from Azahar's image of both side by side.
    Screens screens;
    if (CurrentScreens(game, &screens))
    {
      ScreensSource src;
      src.view = view;
      src.layout = s_hw_image.image_layout;
      src.exact = true;
      src.kind = GpuFilter::Source::Kind::Drawn;  // the cores' 3D
      src.width = s_frame_w;
      src.height = s_frame_h;
      src.console = static_cast<float>(std::clamp(s_3ds_scale, 1, 4));
      src.uv_w = static_cast<float>(s_frame_w);
      src.uv_h = static_cast<float>(s_frame_h);
      src.upscaled = s_3ds_scale > 1;  // (above Native always smooth, as Texture Filtering promises)
      const VkImageLayout layout = s_hw_image.image_layout;
      src.texture = [&display, &quad_view, layout](bool smooth) {
        return display.ExternalTexture(quad_view(), layout, smooth);
      };
      DrawScreens(dl, src, screens, picture, game, gpu);
      return;
    }
    // Above Native (2x, 4x) always smooth, as Texture Filtering promises: sharp (nearest) drops
    // whole columns of the 2560-wide picture and doubles some rows on the way to the screen.
    const bool upscaled = s_system == SystemId::PS1     ? Settings::GetInt("ps1_resolution", game) > 1 :
                          s_system == SystemId::PSP     ? s_frame_w > 480 :
                          IsFlycast(s_system)           ? s_frame_h > 480 :
                          s_system == SystemId::THREEDS ? s_3ds_scale > 1 :
                                                          s_frame_w >= 1280;
    const bool smooth = upscaled || Settings::Get("filter", game) == "smooth";
    // ((01.72) The colours too: WithColours.)
    GpuFilter::Source source;
    const GpuFilter::Mode mode = WithColours(gpu, &source, game, smooth);
    if (mode != GpuFilter::Mode::Off)
    {
      source.view = view;
      source.layout = s_hw_image.image_layout;
      source.width = s_frame_w;
      source.height = s_frame_h;
      source.kind = GpuFilter::Source::Kind::Drawn;  // the cores' 3D
      if (mode == GpuFilter::Mode::Crt)
        CrtScreens(&source, picture, game);
      if (GpuFilter::Draw(dl, mode, source, picture.min, picture.max))
      {
        DrawScanlines(dl, picture, game);
        return;
      }
    }
    const ImTextureID image = display.ExternalTexture(quad_view(), s_hw_image.image_layout, smooth);
    if (IsFlycastBoard(s_system) && (s_rotation & 3) != 0)
    {
      // A vertical NAOMI or Atomiswave game (SET_ROTATION: libretro's quarter turns anticlockwise): the image's
      // corners drawn turned - at 1, its top right corner at the picture's top left.
      const ImVec2 uv[4] = {ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f)};
      const unsigned r = s_rotation & 3;
      dl->AddImageQuad(ImTextureRef(image), picture.min, ImVec2(picture.max.x, picture.min.y), picture.max,
                       ImVec2(picture.min.x, picture.max.y), uv[r], uv[(r + 1) & 3], uv[(r + 2) & 3], uv[(r + 3) & 3]);
    }
    else
    {
      dl->AddImage(ImTextureRef(image), picture.min, picture.max);
    }
    DrawScanlines(dl, picture, game);
    return;
  }
  if (s_texture_w == 0 || s_texture_h == 0)
    return;
  const Picture picture = PlacePicture(game);
  // The GPU's filters: from the console's own picture (a picture the CPU made bigger only for HD
  // Sharpen; it is one frame old when the filter was just chosen).
  const GpuFilter::Mode gpu = GpuMode(game);
  // The DS's two screens: placed by the app (00.80), from melonDS DS's picture of both side by side in the game
  // texture (One Screen and Big Screen were drawn this way since 00.55 and 00.40; the others were the core's).
  Screens screens;
  if (CurrentScreens(game, &screens))
  {
    ScreensSource src;
    src.view = display.GameView();
    src.width = s_texture_w;
    src.height = s_texture_h;
    src.scale = static_cast<float>(s_texture_w) / static_cast<float>(std::max(1u, s_frame_w));
    src.uv_w = static_cast<float>(Display::GAME_TEXTURE_W);
    src.uv_h = static_cast<float>(Display::GAME_TEXTURE_H);
    src.upscaled = s_texture_scaled;  // (a picture made bigger is always drawn smooth)
    src.texture = [&display](bool smooth) { return display.GameTexture(smooth); };
    DrawScreens(dl, src, screens, picture, game, gpu);
    return;
  }
  // A picture made bigger is always drawn smooth (it is rarely an exact multiple of the TV's
  // lines). Smooth stays half a texel inside: around the picture are older, bigger ones.
  const bool smooth = s_texture_scaled || Settings::Get("filter", game) == "smooth";
  // ((01.72) The colours too: WithColours.)
  GpuFilter::Source source;
  const GpuFilter::Mode mode = WithColours(gpu, &source, game, smooth);
  if (mode != GpuFilter::Mode::Off && (!s_texture_scaled || !GpuRedraws(mode)))
  {
    source.view = display.GameView();
    source.width = s_texture_w;
    source.height = s_texture_h;
    source.exact = false;
    source.kind = GpuFilter::Source::Kind::PixelArt;
    if (mode == GpuFilter::Mode::Crt)
      CrtScreens(&source, picture, game);
    if (GpuFilter::Draw(dl, mode, source, picture.min, picture.max))
    {
      DrawScanlines(dl, picture, game);
      return;
    }
  }
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
  // Hi-res and interlaced pictures are twice as big already (a Mega Drive's 320 pixels aren't hi-res:
  // only its interlaced lines are; an arcade board's picture is whatever size the board draws).
  // (The PC Engine's pictures are 256 to 352 pixels wide, its hi-res mode 512: 00.82.)
  const bool doubled = s_system == SystemId::ARCADE                         ? false :
                       IsGpgx(s_system)                                  ? s_frame_h > 300 :
                       s_system == SystemId::PCE                         ? s_frame_w > 400 :
                       IsAtari(s_system)                                 ? s_frame_h > 300 :  // (00.90: the 7800's 320)
                                                                           s_frame_w > 300 || s_frame_h > 300;
  if (doubled && s_system != SystemId::NDS)
    factor /= 2;
  if (NtscFilterOn(game))
    factor = 1;  // the NTSC filter's picture is a TV's look of its own
  const GpuFilter::Mode gpu = GpuMode(game);
  if (GpuRedraws(gpu) && GpuFilter::Ready(gpu))
    factor = 1;  // the GPU redraws the console's own picture (Graphics > GPU Upscaler)
  // (01.06) Nor while that filter is still being compiled (the game's first second or so): xBRZ on the CPU for those
  // frames made its 64 MiB table (96 MiB while it was made), kept until the app ends - v00.23's U6V4SA: 62 MiB of heap
  // in use before a GBA game with the AI filter, 126 after it, and the 3DS game after that ran out of heap. (A GPU
  // filter that fails still has the CPU's upscaler, as before.)
  if (GpuRedraws(gpu) && GpuFilter::Pending(gpu))
    factor = 1;
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

// The battery save written beside the emulation (00.72). SaveMemory wrote the file - through a temporary file, with
// an fsync - on the emulation thread, 3 s after each change the game made: an N64 game's 296960 bytes (its EEPROM, the
// four Controller Paks, SRAM and FlashRAM) at every save it makes, the game's time spent waiting for the PS4's storage
// (00.28's log: the pipeline cache's 2-4 MB took 14-24 ms, now and then seconds). During play the data is copied
// there (a quarter of a millisecond) and this thread writes it; a newer copy of a file replaces one not written yet.
// The game's end (and a forced save) waits until everything is written, then writes the last copy itself.
class SaveWriter
{
public:
  ~SaveWriter() { Stop(); }
  // False when there is no thread to write it (it couldn't be started): the caller writes it then.
  bool Post(const std::string& path, std::vector<uint8_t> data, const char* what)
  {
    std::lock_guard lock(m_mutex);
    if (!m_started && !m_failed_start)
    {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 256 * 1024);  // (the start watchdog's size: libthr keeps one for the next)
      m_started = pthread_create(&m_thread, &attr, &SaveWriter::Main, this) == 0;
      pthread_attr_destroy(&attr);
      m_failed_start = !m_started;
      if (m_failed_start)
        LogF("save: no thread to write the saves beside the game: written on the emulation thread");
    }
    if (!m_started)
      return false;
    for (Job& job : m_jobs)
    {
      if (job.path == path)
      {
        job.data = std::move(data);  // not written yet: the newer copy instead
        job.posted = std::chrono::steady_clock::now();
        return true;
      }
    }
    m_jobs.push_back({path, std::move(data), what, std::chrono::steady_clock::now()});
    m_wake.notify_all();
    return true;
  }
  // Until everything posted is written.
  void Flush()
  {
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [this] { return m_jobs.empty() && !m_writing; });
  }
  void Stop()
  {
    {
      std::lock_guard lock(m_mutex);
      if (!m_started)
        return;
      m_quit = true;
    }
    m_wake.notify_all();
    pthread_join(m_thread, nullptr);
    m_started = false;
    m_quit = false;
  }

private:
  struct Job
  {
    std::string path;
    std::vector<uint8_t> data;
    const char* what;
    std::chrono::steady_clock::time_point posted;
  };
  static void* Main(void* self)
  {
    // Off the emulation's core 2 and the N64's RDP command thread's core 3, at the PS4's usual priority: it mostly
    // waits for the storage.
    emusuni_set_affinity(scePthreadSelf(), (1ull << 0) | (1ull << 1) | (1ull << 4) | (1ull << 5));
    scePthreadSetprio(scePthreadSelf(), 700);
    static_cast<SaveWriter*>(self)->Loop();
    return nullptr;
  }
  void Loop()
  {
    std::unique_lock lock(m_mutex);
    for (;;)
    {
      m_wake.wait(lock, [this] { return m_quit || !m_jobs.empty(); });
      if (m_jobs.empty())
        break;  // stopped, and everything written
      Job job = std::move(m_jobs.front());
      m_jobs.pop_front();
      m_writing = true;
      lock.unlock();
      const auto start = std::chrono::steady_clock::now();
      const bool written = WriteDataFile(job.path, job.data.data(), job.data.size());
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      const double waited =
          std::chrono::duration<double, std::milli>(start - job.posted).count();
      if (written)
        LogF("save: %s written beside the game (%zu bytes, %.0f ms; it waited %.0f ms)", job.what, job.data.size(), ms,
             waited);
      else
        LogF("save: %s couldn't be written (%zu bytes, %.0f ms): written again at the game's end", job.what,
             job.data.size(), ms);
      lock.lock();
      m_writing = false;
      m_done.notify_all();
    }
    m_writing = false;
    m_done.notify_all();
  }
  std::mutex m_mutex;
  std::condition_variable m_wake, m_done;
  std::deque<Job> m_jobs;
  pthread_t m_thread{};
  bool m_started = false, m_failed_start = false, m_quit = false, m_writing = false;
};
SaveWriter* s_save_writer = nullptr;  // while a game runs (Run)

// Writes it when the game changed it (or always, with force). A save the game never touched
// (all 0xFF: a Game Boy Advance game's 128 KiB before it first saves) isn't written. During play (not forced) by
// s_save_writer's thread; forced (the game's end, a state) here, after what is waiting there.
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
  if (!force && s_save_writer && s_save_writer->Post(path, std::vector<uint8_t>(data, data + size), what))
  {
    saved->assign(data, data + size);  // what the file will hold (a failed write is done again at the game's end)
    return;
  }
  if (s_save_writer)
    s_save_writer->Flush();  // (an older copy mustn't land after this one)
  const auto start = std::chrono::steady_clock::now();
  if (WriteDataFile(path, data, size))
  {
    saved->assign(data, data + size);
    LogF("save: %s written (%zu bytes, %.0f ms)", what, size,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
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

// A PlayStation disc's tracks without their .cue (00.81: "Name (Track 1).bin", "Name (Track 2).bin"... as Redump
// names them, the .cue lost - Library::LoneTracks): the core is given a .cue made for them in data_dir/cache/cue/ps1,
// as Redump's own: the first track data (MODE2/2352), the others CD audio with their 2-second pregap in their files
// (INDEX 00 at 00:00:00, INDEX 01 at 00:02:00); the disc's .sbi (a LibCrypt disc's), "Name.sbi" beside it, copied beside
// that .cue, where the core looks for it. Any other disc as it is (a lone .bin of one track: the core reads it itself).
std::string Ps1DiscContent(const std::string& disc)
{
  const std::vector<std::string> tracks = Library::LoneTracks(disc);
  if (tracks.empty())
    return disc;
  std::string name = std::filesystem::path(disc).stem().string();
  const size_t track = name.rfind(" (Track ");
  if (track != std::string::npos && track > 0)
    name.erase(track);  // "Name (Track 1)" -> "Name": the .sbi's name too
  std::string file = name;
  for (char& c : file)
  {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  }
  const std::string dir = std::string(App::DATA_DIR) + "/cache/cue/ps1";
  MakeDirs(dir);
  const std::string cue = dir + "/" + file + ".cue";
  std::string text;
  for (size_t i = 0; i < tracks.size(); i++)
  {
    char line[64];
    std::snprintf(line, sizeof(line), "  TRACK %02zu %s\r\n", i + 1, i == 0 ? "MODE2/2352" : "AUDIO");
    text += "FILE \"" + tracks[i] + "\" BINARY\r\n" + line +
            (i == 0 ? "    INDEX 01 00:00:00\r\n" : "    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n");
  }
  if (!WriteTextFile(cue, text))
  {
    LogF("ps1: %s: %zu tracks without a .cue, and the .cue for them couldn't be written (%s)", disc.c_str(),
         tracks.size(), cue.c_str());
    return disc;
  }
  const std::string sbi = (std::filesystem::path(disc).parent_path() / (name + ".sbi")).string();
  const std::string sbi_copy = dir + "/" + file + ".sbi";
  const bool has_sbi = FileExists(sbi) && WriteTextFile(sbi_copy, ReadTextFile(sbi));
  if (!has_sbi)
    std::remove(sbi_copy.c_str());
  LogF("ps1: %s: %zu tracks without a .cue: started with %s%s", disc.c_str(), tracks.size(), cue.c_str(),
       has_sbi ? " (and the disc's .sbi)" : "");
  return cue;
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

// (00.95) A link cable match's battery saves (src/GbLink.h). The host: its own game's save in Game Boy 1, the guest's -
// as it sent it - in Game Boy 2, the clocks brought to now; a guest: none (both come in the host's state). Each keeps a
// copy of its own game's save from before the match (BackupSave) and, as last written, its RAM (s_gblink_saved).
void LoadGbLinkSaves()
{
  if (!s_link_saves)
    return;
  const LinkSaves& L = *s_link_saves;
  const GbLink::Match& match = GbLink::Get();
  const GbLink::Game& own = match.games[match.local];
  const char* machine = GbLink::Gba() ? "GBA" : "Game Boy";
  GbLink::BackupSave(own.key);
  const std::vector<uint8_t> mine = GbLink::ReadSave(own.key, static_cast<uint64_t>(std::time(nullptr)));
  const size_t ram = std::min(mine.size(), L.ram_size(static_cast<unsigned>(match.local)));
  s_gblink_saved.assign(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(ram));
  if (match.local == 0)
  {
    L.load(0, mine.data(), mine.size());
    L.load(1, match.guest_save.data(), match.guest_save.size());
    LogF("gblink: battery saves loaded: ours %zu bytes in %s 1 (it keeps %zu), the guest's %zu in %s 2 (it keeps %zu)",
         mine.size(), machine, L.battery_size(0), match.guest_save.size(), machine, L.battery_size(1));
  }
  else
    LogF("gblink: our save (%zu bytes) comes in the host's state, in %s 2", mine.size(), machine);
}

// A guest, once the host's state is in (before its first frame): Game Boy 2's battery RAM is its own save's as it is
// on this console - false: it isn't (the host sent another), and the match ends without writing it.
bool CheckGbLinkSave()
{
  if (!s_link_saves)
    return false;
  const LinkSaves& L = *s_link_saves;
  const size_t size = L.battery_size(1), ram = L.ram_size(1);
  std::vector<uint8_t> data(size);
  if (s_gblink_saved.empty())
    return true;  // (no save of ours yet: a new game)
  if (size == 0 || ram < s_gblink_saved.size() || !L.save(1, data.data(), size))
    return false;
  return std::equal(s_gblink_saved.begin(), s_gblink_saved.end(), data.begin());
}

// The battery RAM `data` (`ram` bytes) is the save last written (s_gblink_saved): the same bytes, the longer one's
// rest 0xFF (01.04: a GBA's save is 128 KiB until mGBA knows its kind - a smaller one, its .srm, in its start).
bool LinkSaveSame(const std::vector<uint8_t>& data, size_t ram)
{
  const size_t common = std::min(ram, s_gblink_saved.size());
  if (!std::equal(s_gblink_saved.begin(), s_gblink_saved.begin() + static_cast<std::ptrdiff_t>(common), data.begin()))
    return false;
  const auto blank = [](uint8_t b) { return b == 0xFF; };
  return std::all_of(data.begin() + static_cast<std::ptrdiff_t>(common), data.begin() + static_cast<std::ptrdiff_t>(ram),
                     blank) &&
         std::all_of(s_gblink_saved.begin() + static_cast<std::ptrdiff_t>(common), s_gblink_saved.end(), blank);
}

// This console's Game Boy's battery save written into its own game's (its RAM into <key>.srm, its clock into
// <key>.rtc) when its RAM changed since last written, or `force`d (the game's end: the clock too - a save without a
// clock, as a GBA's, isn't written again unchanged). Beside the emulation (the SaveWriter) during play. A guest writes
// nothing before its own save came in the host's state.
void SaveGbLinkSave(bool force)
{
  if (!s_link_saves)
    return;
  const LinkSaves& L = *s_link_saves;
  const GbLink::Match& match = GbLink::Get();
  const unsigned gb = static_cast<unsigned>(match.local);
  if (s_gblink_failed || (gb == 1 && !s_gblink_checked))
    return;
  const size_t size = L.battery_size(gb), ram = L.ram_size(gb);
  if (size == 0 || ram > size)
    return;
  std::vector<uint8_t> data(size);
  if (!L.save(gb, data.data(), size))
    return;
  const bool same = LinkSaveSame(data, ram);
  if (same && (!force || size == ram))
    return;
  const std::string& key = match.games[gb].key;
  if (std::all_of(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(ram), [](uint8_t b) { return b == 0xFF; }) &&
      !FileExists(Saves::SramPath(key)))
    return;  // (nothing saved yet: no file made)
  const bool clock = size > ram;
  if (!force && s_save_writer &&
      s_save_writer->Post(Saves::SramPath(key), std::vector<uint8_t>(data.begin(), data.begin() + ram), "battery save"))
  {
    if (clock)
      s_save_writer->Post(Saves::RtcPath(key), std::vector<uint8_t>(data.begin() + ram, data.end()), "cartridge clock");
    s_gblink_saved.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(ram));
    return;
  }
  if (s_save_writer)
    s_save_writer->Flush();  // (an older copy mustn't land after this one)
  if (GbLink::WriteSave(key, data.data(), size, ram, clock))
  {
    s_gblink_saved.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(ram));
    LogF("gblink: %s %u's battery save written into %s (%zu bytes%s)", GbLink::Gba() ? "GBA" : "Game Boy", gb + 1,
         key.c_str(), ram, clock ? ", and its clock" : "");
  }
}

void LoadSram()
{
  if (s_gblink)
  {
    LoadGbLinkSaves();
    return;
  }
  LoadMemory(RETRO_MEMORY_SAVE_RAM, Saves::SramPath(s_game_id), &s_sram_saved, "battery save");
  LoadMemory(RETRO_MEMORY_RTC, Saves::RtcPath(s_game_id), &s_rtc_saved, "cartridge clock");
}

void SaveSram(bool force)
{
  // (00.95) A link cable match: each console writes its own Game Boy's save into its own game's.
  if (s_gblink)
  {
    SaveGbLinkSave(force);
    return;
  }
  // (00.23) Online, a guest plays the host's game (its battery save came with the host's state): its own save file
  // isn't written over.
  if (Netplay::KeepOwnSaves())
    return;
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
    if (!IsFlycast(s_system))
      break;
  }
  std::vector<uint8_t>().swap(*out);
  return false;
}

// (00.85) The game's picture on the screen now - the last drawing, the in-game menu not yet over it -, kept for the
// states saved while the menu is open when the core draws its own image (SaveState: no pixels of it here). Where
// DrawPicture put it (PlacePicture; the 3DS's screens are inside it), at most 512 x 448 as the other thumbnails.
void KeepMenuPicture(Display& display, const std::string& game)
{
  s_menu_picture = ImageRGBA();
  if (!s_frame_hw || !s_hw_image_set || s_frame_w == 0 || s_frame_h == 0)
    return;
  const Picture picture = PlacePicture(game);
  const int x = static_cast<int>(std::floor(picture.min.x)), y = static_cast<int>(std::floor(picture.min.y));
  const int w = static_cast<int>(std::ceil(picture.max.x)) - x, h = static_cast<int>(std::ceil(picture.max.y)) - y;
  const auto start = Clock::now();
  ImageRGBA image;
  if (!display.CaptureArea(x, y, w, h, 512, 448, &image.pixels, &image.width, &image.height))
  {
    LogF("menu: the game's picture (%d,%d %dx%d) couldn't be read from the screen: its states get no thumbnail", x, y,
         w, h);
    return;
  }
  s_menu_picture = std::move(image);
  LogF("menu: the game's picture kept for a state's thumbnail (%ux%u, from %dx%d of the screen, %.1f ms)",
       s_menu_picture.width, s_menu_picture.height, w, h,
       std::chrono::duration<double, std::milli>(Clock::now() - start).count());
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
  // A Dreamcast game on several discs: the disc in it with the state (Flycast's states don't hold it; LoadState puts
  // it in first).
  const std::string disc = path + ".disc";
  if (s_system == SystemId::DC && DiscCount() > 1)
    WriteTextFile(disc, std::to_string(DiscIn()));
  else
    std::remove(disc.c_str());
  // The state's picture (its slot's thumbnail): the core's own pixels; a core that draws its own image (the N64, the
  // PlayStation, the PSP, Flycast's machines, the 3DS: s_frame NULL) the screen's picture as the in-game menu opened
  // (s_menu_picture). Until 00.84 those had none - "Empty slot" beside a state saved, GitHub #33. No picture: the slot's
  // older one goes, not shown for this state.
  const std::string picture_path = Saves::StatePicturePath(s_game_id, slot);
  const char* pictured = nullptr;
  if (s_frame && s_frame_w && s_frame_h && !s_frame_hw)
  {
    ImageRGBA picture = FromRgb565(static_cast<const uint16_t*>(s_frame), s_frame_w, s_frame_h,
                                   s_frame_pitch);
    if (SavePng(picture_path, Shrink(picture, 512, 448)))
      pictured = "its picture";
  }
  else if (!s_menu_picture.empty() && SavePng(picture_path, s_menu_picture))
  {
    pictured = "the screen's picture";
  }
  if (!pictured)
    std::remove(picture_path.c_str());
  LogF("state: saved slot %d (%zu bytes, %s)", slot, size, pictured ? pictured : "no picture");
  Achievements::StateSaved(path);  // (the achievements' progress beside it: <state>.ra)
  return true;
}

// (01.57, the owner) The N64 Resolution changed in the in-game menu: ParaLLEl-RDP's size is made as the game starts
// (changing it in the running core, our 4x's buffers with it, is a risk on the PS4's GPU memory), so the game's state is
// kept apart - "<slot 0's file>-restart": no slot of the player's, nor Continue's, and Saved Data doesn't list it -,
// the game starts again (Result::Restart) and loads it as its first thing. Taken once, within a minute, by that game.
std::string s_restart_game;
std::chrono::steady_clock::time_point s_restart_time;

std::string RestartStatePath(const std::string& game_id)
{
  return Saves::StatePath(game_id, 0) + "-restart";
}

bool SaveRestartState()
{
  std::vector<uint8_t> state;
  const std::string path = RestartStatePath(s_game_id);
  if (!SerializeCore(&state) || !WriteDataFile(path, state.data(), state.size()))
  {
    LogLine("state: the N64 Resolution's restart state couldn't be kept: the game goes on as it is");
    std::remove(path.c_str());
    return false;
  }
  Achievements::StateSaved(path);
  s_restart_game = s_game_id;
  s_restart_time = std::chrono::steady_clock::now();
  LogF("state: kept for the start again with the new N64 Resolution (%zu bytes)", state.size());
  return true;
}

// The start again's state, loaded (true), or none for this game (*asked false) / one that didn't load (*asked true).
bool LoadRestartState(const std::string& game_id, bool* asked)
{
  const std::string path = RestartStatePath(game_id);
  *asked = s_restart_game == game_id && std::chrono::steady_clock::now() - s_restart_time < std::chrono::minutes(1);
  s_restart_game.clear();
  if (!*asked)
  {
    std::remove(path.c_str());  // (one left by a start that didn't come)
    std::remove((path + ".ra").c_str());
    return false;
  }
  std::vector<uint8_t> data(Saves::Size(path));
  FILE* file = data.empty() ? nullptr : fopen(path.c_str(), "rb");
  const bool read = file && fread(data.data(), 1, data.size(), file) == data.size();
  if (file)
    fclose(file);
  const bool ok = read && s_core->unserialize(data.data(), data.size());
  if (ok)
    Achievements::StateLoaded(path);
  std::remove(path.c_str());
  std::remove((path + ".ra").c_str());
  LogF("state: the start again with the new N64 Resolution: its state %s", ok ? "loaded" : read ? "refused" : "unread");
  return ok;
}

// (01.27) Triangle -> Load State's slot for the game's next start (LoadStateAtStart): -1 none, or another game's, or
// asked more than a minute ago (a start that didn't come). Taken once.
std::mutex s_start_slot_mutex;
std::string s_start_slot_game;
int s_start_slot = -1;
std::chrono::steady_clock::time_point s_start_slot_time;

int TakeStartSlot(const std::string& game_key)
{
  std::lock_guard lock(s_start_slot_mutex);
  const int slot = s_start_slot;
  const bool fresh = std::chrono::steady_clock::now() - s_start_slot_time < std::chrono::minutes(1);
  s_start_slot = -1;
  if (slot < 0)
    return -1;
  if (game_key != s_start_slot_game || !fresh)
  {
    LogF("state: Load State's slot %d for %s not used (%s)", slot, s_start_slot_game.c_str(),
         fresh ? "another game started" : "asked more than a minute ago");
    return -1;
  }
  LogF("state: started from Load State: slot %d", slot);
  return slot;
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
  // ((01.09) The 3DS makes its renderer again in a state's load: its images forgotten - ThreedsRemaking.)
  const bool ok = s_system == SystemId::THREEDS ?
                      ThreedsRemaking("state's load",
                                      [&data] { return s_core->unserialize(data.data(), data.size()); }) :
                      s_core->unserialize(data.data(), data.size());
  if (!ok && disc_before >= 0)
  {
    s_disk.set_image_index(static_cast<unsigned>(disc_before));
    std::vector<uint8_t>().swap(data);
    s_core->unserialize(machine.data(), machine.size());
  }
  LogF("state: slot %d %s", slot, ok ? "loaded" : "refused");
  if (ok)
    Achievements::StateLoaded(path);  // (the achievements' progress of then, or afresh)
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
  // (01.72) The PS Vita: the pictures the game itself showed in the last second (its core's count), which the counter
  // shows instead of the TV's; -1 for the other consoles.
  float game_fps = -1.0f;
  unsigned long long vita_at = ~0ull;  // ((01.77) the core's count at the counter's last second; ~0: not known)
  unsigned long long n64_at = ~0ull;   // ((01.78) the same for the N64's own pictures)
  // (01.72) The core's pictures a second (s_pictures), shown by the counter (-1: not a second's worth yet); s_pictures
  // at the counter's last second and at the log's window's start.
  float pictures_fps = -1.0f;
  uint64_t pictures_at = 0, pictures10_at = 0;
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

// (01.39, the 3DS specialist's 3ds-0137 test: Luigi's Mansion 2's passes of 160-250 ms and more on a room's change, its
// core's frames 20-60 ms; the pacing's own time is cut at 250 ms) What a pass spent its time on.
struct PassParts
{
  double core = 0.0, core_most = 0.0;  // the core's runs (s_core->run, Netplay::RunFrame)
  int runs = 0;
  double handover = 0.0;  // the drawing's wait for the 3DS's picture from the core's Vulkan worker
  double gpu = 0.0;       // its waits for the GPU (Display: its frames' fences)
  double flip = 0.0;      // its wait for the TV (Display: its image off screen)
  double work = 0.0;      // the drawing's own work
  bool beside = false;    // the drawing beside the core (another thread): beside_wait, the wait for it after the core
  double beside_wait = 0.0;
  bool quiet = true;  // the menu open in it, or the game's first pass: not said
};

// A pass over 100 ms in the log, as it was; 8 lines at most each 10 s, the others counted.
struct LongPasses
{
  Clock::time_point window{};
  int said = 0, left = 0;
};

void LogLongPass(LongPasses& passes, const PassParts& parts, double pass_ms, Clock::time_point now)
{
  if (parts.quiet || pass_ms <= 100.0)
    return;
  if (passes.said == 0 || now - passes.window >= std::chrono::seconds(10))
  {
    if (passes.left > 0)
      LogF("pacing: %d more passes over 100 ms in the 10 s before, not said one by one", passes.left);
    passes.window = now;
    passes.said = passes.left = 0;
  }
  if (passes.said >= 8)
  {
    passes.left++;
    return;
  }
  passes.said++;
  const double drawing = parts.handover + parts.gpu + parts.flip + parts.work;
  const double rest = pass_ms - parts.core - (parts.beside ? parts.beside_wait : drawing);
  char beside[64] = "";
  if (parts.beside)
    std::snprintf(beside, sizeof(beside), " beside it (waited for %.1f after the core)", parts.beside_wait);
  LogF("pacing: a pass of %.0f ms: the core %.1f ms (%d runs, the longest %.1f); the drawing%s %.1f ms: the "
       "picture's handover %.1f, waiting for the GPU %.1f, for the flip %.1f, its own work %.1f; the rest %.1f",
       pass_ms, parts.core, parts.runs, parts.core_most, beside, drawing, parts.handover, parts.gpu, parts.flip,
       parts.work, std::max(0.0, rest));
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
  stats.pictures10_at = s_pictures;  // ((01.72) the core's pictures in the window: from here)
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

// (01.66, the owner: "la pantalla de carga inicial ... hazla más bonita") The PS Vita's loading screen, the app's own:
// a night-blue gradient with the PS Vita's blue glowing behind the game's cover (or a card with the console's name),
// its name, the core's step in the player's language, a bar whose light runs along it and how long it has taken. The
// core prepares the game inside its run, some seconds in one: the screen then stays as it was drawn, which reads well
// still (the bar a bar, its light where it stopped).
void DrawVitaLoading(ImDrawList* dl, ImFont* regular, ImFont* bold, const std::string& title, ImTextureID cover,
                     float cover_aspect, float seconds)
{
  const float W = static_cast<float>(Display::WIDTH), H = static_cast<float>(Display::HEIGHT);
  const auto rgba = [](uint32_t rgb, float alpha) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255));
  };
  constexpr uint32_t BLUE = 0x2F6BFF, LIGHT = 0x8FB4FF;
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), rgba(0x101A3E, 1), rgba(0x0B1230, 1), rgba(0x04060F, 1),
                              rgba(0x070A1C, 1));
  // The cover's place: left of the middle, 480 tall, its own shape (a card when there is none).
  const float ch = 480.0f, cw = ch * std::clamp(cover ? cover_aspect : 0.62f, 0.55f, 1.45f);
  const ImVec2 cc(W * 0.30f, H * 0.47f);
  for (int i = 8; i >= 1; i--)  // (the glow: rings of the PS Vita's blue, faint outwards)
    dl->AddCircleFilled(cc, 140.0f + 46.0f * static_cast<float>(i), rgba(BLUE, 0.022f), 64);
  const ImVec2 c0(cc.x - cw * 0.5f, cc.y - ch * 0.5f), c1(cc.x + cw * 0.5f, cc.y + ch * 0.5f);
  dl->AddRectFilled(ImVec2(c0.x + 10, c0.y + 16), ImVec2(c1.x + 10, c1.y + 16), rgba(0x000000, 0.45f), 18.0f);
  if (cover)
  {
    dl->AddImageRounded(ImTextureRef(cover), c0, c1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 18.0f);
  }
  else
  {
    // (Rounded as a cover: its colour, lighter at the top - ImGui's gradients have square corners.)
    dl->AddRectFilled(c0, c1, rgba(0x14337C, 1), 18.0f);
    for (int band = 1; band <= 8; band++)  // (eight faint layers from the top: a gradient without a line)
      dl->AddRectFilled(c0, ImVec2(c1.x, c0.y + ch * static_cast<float>(band) / 8.0f), rgba(0x4C78E0, 0.045f), 18.0f,
                        ImDrawFlags_RoundCornersTop);
    const char* name = "PS Vita";
    const ImVec2 size = bold->CalcTextSizeA(64, FLT_MAX, 0.0f, name);
    dl->AddText(bold, 64, ImVec2(cc.x - size.x * 0.5f, cc.y - size.y * 0.5f), rgba(0xFFFFFF, 0.92f), name);
  }
  dl->AddRect(c0, c1, rgba(LIGHT, 0.35f), 18.0f, 0, 2.0f);
  // The words: the console, the game's name (two lines at most), the step, its note when it is a long one.
  const float x = W * 0.30f + 480.0f * 0.5f + 150.0f, width = W - 140.0f - x;
  float y = cc.y - 170.0f;
  dl->AddText(bold, 30, ImVec2(x, y), rgba(LIGHT, 1), "PS VITA");
  y += 52.0f;
  const ImVec2 title_size = bold->CalcTextSizeA(56, FLT_MAX, width, title.c_str());
  dl->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + 56.0f * 2.4f), true);
  dl->AddText(bold, 56, ImVec2(x, y), rgba(0xFFFFFF, 1), title.c_str(), nullptr, width);
  dl->PopClipRect();
  y += std::min(title_size.y, 56.0f * 2.4f) + 34.0f;
  const std::string step = PS4Lang::T(s_vita_step.empty() ? "Preparing the title..." : s_vita_step);
  dl->AddText(regular, 34, ImVec2(x, y), rgba(0xFFFFFF, 0.88f), step.c_str(), nullptr, width);
  y += 52.0f;
  const bool long_step = s_vita_step.rfind("Installing", 0) == 0 || s_vita_step == "Preparing the firmware...";
  if (long_step)
  {
    const std::string note = PS4Lang::T("Only the first time: it can take a few minutes");
    dl->AddText(regular, 26, ImVec2(x, y), rgba(LIGHT, 0.75f), note.c_str(), nullptr, width);
  }
  y += 64.0f;
  // The bar: its track, and a light running along it every 1.8 s (quiet when the core holds the screen).
  const float bar_w = std::min(width, 760.0f), bar_h = 10.0f;
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_w, y + bar_h), rgba(0xFFFFFF, 0.12f), bar_h * 0.5f);
  const float run = std::fmod(seconds, 1.8f) / 1.8f, light = 240.0f;
  const float l0 = x - light + (bar_w + light) * run;
  const float a0 = std::max(x, l0), a1 = std::min(x + bar_w, l0 + light);
  if (a1 > a0)
  {
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + bar_w, y + bar_h), true);
    const float mid = l0 + light * 0.5f;
    dl->AddRectFilledMultiColor(ImVec2(l0, y), ImVec2(mid, y + bar_h), rgba(BLUE, 0.0f), rgba(LIGHT, 1),
                                rgba(LIGHT, 1), rgba(BLUE, 0.0f));
    dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(l0 + light, y + bar_h), rgba(LIGHT, 1), rgba(BLUE, 0.0f),
                                rgba(BLUE, 0.0f), rgba(LIGHT, 1));
    dl->PopClipRect();
  }
  char elapsed[16];
  const int whole = static_cast<int>(seconds);
  std::snprintf(elapsed, sizeof(elapsed), "%d:%02d", whole / 60, whole % 60);
  dl->AddText(regular, 26, ImVec2(x + bar_w + 22.0f, y - 10.0f), rgba(0xFFFFFF, 0.55f), elapsed);
}

#ifdef EMUSUNI_VITA
// (01.67) The PS Vita's cursor over its picture: the pointer's (L3) or the touchpad's (01.69: a trackpad) - a white
// ring with a dark edge and a dot, filled while it touches; the PS Vita's blue while the pointer is on.
void DrawVitaCursor(ImDrawList* dl, const std::string& game)
{
  if (!s_vita_pointer.shown || s_vita_loading || !s_frame_hw)
    return;
  const Picture picture = PlacePicture(game);
  const ImVec2 at(picture.min.x + s_vita_pointer.x * (picture.max.x - picture.min.x),
                  picture.min.y + s_vita_pointer.y * (picture.max.y - picture.min.y));
  const bool touching = s_vita_pointer.touching;
  const ImU32 ring = s_vita_pointer.on ? IM_COL32(143, 180, 255, 255) : IM_COL32(255, 255, 255, 235);
  if (touching)
    dl->AddCircleFilled(at, 22.0f, s_vita_pointer.on ? IM_COL32(47, 107, 255, 110) : IM_COL32(255, 255, 255, 80), 32);
  dl->AddCircle(at, 18.0f, IM_COL32(0, 0, 0, 150), 32, 6.0f);
  dl->AddCircle(at, 18.0f, ring, 32, 3.0f);
  dl->AddCircleFilled(at, 4.5f, IM_COL32(0, 0, 0, 150), 16);
  dl->AddCircleFilled(at, 3.0f, ring, 16);
}
#endif

void DrawStats(ImDrawList* dl, ImFont* bold, const Stats& stats, bool fps, bool graphs, float fill)
{
  if (fps)
  {
    char text[64];
    // ((01.72) The PS Vita: the game's own pictures a second, without a percent - the game's own rate, 30 or 60,
    // isn't known, and its core's speed is always the TV's.)
    // ((01.72) The others: the core's pictures a second - not the TV's refreshes, always 60 -, and the game's speed.)
    if (stats.game_fps >= 0.0f)
      snprintf(text, sizeof(text), "%.0f FPS", stats.game_fps);
    else
      snprintf(text, sizeof(text), "%.0f FPS  %.0f%%", stats.pictures_fps >= 0.0f ? stats.pictures_fps : stats.fps,
               stats.speed);
    // ((01.77) The box as wide as three digits' text at least: it doesn't jump with the number.)
    const char* widest = stats.game_fps >= 0.0f ? "888 FPS" : "888 FPS  888%";
    const float width =
        std::max(bold->CalcTextSizeA(30, FLT_MAX, 0, text).x, bold->CalcTextSizeA(30, FLT_MAX, 0, widest).x);
    dl->AddRectFilled(ImVec2(30, 26), ImVec2(30 + width + 30, 76), IM_COL32(0, 0, 0, 150), 10);
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

bool N64WidescreenListed(const std::string& rom_path)
{
  std::vector<uint8_t> header(0x40);
  FILE* file = std::fopen(rom_path.c_str(), "rb");
  if (!file)
    return false;
  const bool read = std::fread(header.data(), 1, header.size(), file) == header.size();
  std::fclose(file);
  return read && N64Widescreen(N64Crc(header, 0x10), N64Crc(header, 0x14)) != nullptr;
}

void LoadStateAtStart(const std::string& game_key, int slot)
{
  std::lock_guard lock(s_start_slot_mutex);
  s_start_slot_game = game_key;
  s_start_slot = slot;
  s_start_slot_time = std::chrono::steady_clock::now();
}

void SetStartCover(const std::string& game_key, const std::string& path)
{
  s_start_cover_game = game_key;
  s_start_cover_path = path;
}

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
    // (the DS: off cores 3-5, its 2D helper's and 3D threads', port/ps4_n64.c; the Saturn: off cores 1 and 3-5,
    // its renderer's and sound's threads, src/SaturnBench.cpp)
    const uint64_t cores = s_system == SystemId::PSP    ? (1ull << 1) | (1ull << 5) :
                           s_system == SystemId::NDS    ? (1ull << 1) :
                           s_system == SystemId::SATURN ? (1ull << 0) | (1ull << 6) :
                                                          (1ull << 1) | (1ull << 4) | (1ull << 5);
    if (s_dc_threads_free)
    {
      // (A Dreamcast game's threads left to the PS4: this one too.)
      int priority = 0;
      scePthreadGetprio(scePthreadSelf(), &priority);
      LogF("game: drawing beside the emulation: the PS4's cores, priority %d as it started (dc-threads-free)", priority);
    }
    else
    {
      const int affinity = emusuni_set_affinity(scePthreadSelf(), cores);
      const int priority = scePthreadSetprio(scePthreadSelf(), 480);
      LogF("game: drawing beside the emulation: cores %#llx (%#x), priority 480 (%#x)",
           static_cast<unsigned long long>(cores), static_cast<unsigned>(affinity), static_cast<unsigned>(priority));
    }
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

// The memory check before a game starts (src/MemoryGuard.h): a start that needs more of the PS4's regular flexible
// memory than is left would have the PS4 close the app in the middle of it (00.58's God of War at 41 MiB free: the
// blue error screen, no crash report); the menu asks the player to close EmuSuNi and open it again instead.
// data_dir/memory-guard holds a number added to every need (999: every game refused, to see the message).
static bool EnoughMemory(const Library::Game& game, std::string* error)
{
  static bool s_nds_started = false;  // (the DS's 1 MiB stacks are taken at its first start)
  const System& system = GetSystem(game.system);
  // The CPU's Resolution (src/Upscaler.cpp): its threads, the first time it makes a picture bigger (a console whose
  // picture the CPU hands over, at 2x or more, also while a GPU Upscaler compiles; not the NTSC filter's picture).
  const Settings::Setting* resolution = Settings::Find("resolution");
  const bool ntsc = (game.system == SystemId::SNES || game.system == SystemId::NES) &&
                    Settings::Get("ntsc_filter", game.key) != "disabled";
  const bool upscaler = s_scale_name.empty() && resolution && Settings::AppliesTo(*resolution, game.system) && !ntsc &&
                        Settings::GetInt("resolution", game.key) >= 2;
  const int cold = MemoryGuard::ColdMiB(system.key);
  const int own = s_nds_started ? 0 : MemoryGuard::OwnStacksMiB(system.key);
  const int extra = MemoryGuard::ExtraFromText(ReadTextFile(std::string(App::DATA_DIR) + "/memory-guard"));
  // (01.42) A 3DS game's texture pack (src/ThreeDsTextures.h): its loading thread's stack in this memory, and the
  // core's cap for its pictures in the GPU's (direct) memory - with less than that free, and some to spare, the game
  // starts without it this time (said: Run's toast).
  int textures_mib = 0;
  s_3ds_textures = false;
  s_3ds_textures_short = -1;
#ifdef EMUSUNI_3DS
  if (game.system == SystemId::THREEDS)
  {
    const uint64_t title = ThreedsTitleId(game.path);
    if (title != 0 && ThreeDsTextures::On(title) &&
        ThreeDsTextures::HasPng(ThreeDsTextures::InstalledFolder(Portable::UserDir() + "/saves/3ds", title)))
    {
      const int gpu_free = ThreeDsTextures::GpuFreeMiB();
      const int gpu_need = ThreeDsTextures::GPU_MIB + ThreeDsTextures::GPU_MARGIN_MIB;
      s_3ds_textures = gpu_free < 0 || gpu_free >= gpu_need;
      s_3ds_textures_short = s_3ds_textures ? -1 : gpu_free;
      textures_mib = s_3ds_textures ? ThreeDsTextures::THREAD_MIB : 0;
      LogF("memory (before game): its texture pack: GPU memory %s, %d MiB needed (the core's cap %d, %d to spare): %s",
           gpu_free < 0 ? "free not known" : (std::to_string(gpu_free) + " MiB free").c_str(), gpu_need,
           ThreeDsTextures::GPU_MIB, ThreeDsTextures::GPU_MARGIN_MIB,
           s_3ds_textures ? "on" : "left out this time");
    }
  }
#endif
  const MemoryGuard::Decision decision =
      MemoryGuard::Decide(FlexibleFreeMiB(), FlexibleMostMiB(),
                          cold + (upscaler ? MemoryGuard::UPSCALER_MIB : 0) + textures_mib, own, extra);
  std::string parts = "its threads " + std::to_string(cold);
  if (upscaler)
    parts += ", the CPU upscaler's " + std::to_string(MemoryGuard::UPSCALER_MIB);
  if (textures_mib > 0)
    parts += ", its texture pack's thread " + std::to_string(textures_mib);
  if (own > 0)
    parts += ", its 1 MiB stacks " + std::to_string(own);
  if (extra != 0)
    parts += ", memory-guard " + std::to_string(extra);
  LogF("memory (before game): flexible %d MiB free, %d at most so far (%d MiB of thread stacks to reuse); %s needs "
       "%d (%s): %s",
       decision.free_mib, decision.most_mib, decision.reusable_mib, system.core ? system.core->name : system.key,
       decision.need_mib, parts.c_str(),
       decision.start ? "starting" : "not started, the player is asked to reopen EmuSuNi");
  if (!decision.start)
  {
    *error = MemoryGuard::Message();
    return false;
  }
  if (game.system == SystemId::NDS)
    s_nds_started = true;
  MemorySessionStart();
  return true;
}

#ifdef EMUSUNI_DC
// Flycast's pictures and the console's frames between them (00.68). A run of the core is one picture: one frame of
// the console in a 60 FPS game, 2 in one at 30 FPS (Jet Set Radio), 2 to 4 with Frame Skip or once Auto frame skip
// skips. The core gives the frame count of each picture it presents (flycast_emusuni_vblanks: the emulation's count
// when it sent the picture, whatever the threads' timing), and the pacing pays for the frames a picture covered.
struct DcFrames
{
  unsigned long long last = 0;
  bool known = false;
  int candidate = 1, same = 0, logged = 1;

  // The console's frames the run just made covered: 1 to 4 (a run without a new picture, 1).
  int Take()
  {
    const unsigned long long now = flycast_emusuni_vblanks();
    int covered = 1;
    const bool picture = known && now > last;
    if (picture)
      covered = static_cast<int>(std::min<unsigned long long>(now - last, 4));
    if (!known || now != last)
    {
      last = now;
      known = true;
    }
    // A new spacing once 30 pictures in a row agree: logged.
    if (picture)
    {
      same = covered == candidate ? same + 1 : 1;
      candidate = covered;
      if (same >= 30 && candidate != logged)
      {
        LogF("pacing: Flycast's pictures are %d of the console's frames apart now (was %d)%s", candidate, logged,
             candidate == 2 ? ": a game at 30 FPS, or Frame Skip" : "");
        logged = candidate;
      }
    }
    return covered;
  }
  // The console's frames a picture covered beyond the runs its pass had planned: owed back, a run left out of each
  // pass that follows until they are paid (the pacing's frames owed can't go below half a frame).
  int ahead = 0;
  void Owe(int frames) { ahead = std::min(ahead + frames, 8); }
  int Pay(int frames)
  {
    const int paid = std::min(frames, ahead);
    ahead -= paid;
    return frames - paid;
  }
  // The frames counted from the next picture on (the in-game menu closed).
  void Restart()
  {
    known = false;
    ahead = 0;
  }
};
#endif

// The PSP's vblanks per run of the core (00.81, GitHub issue #19: God of War suddenly much faster). With Frame Skip on
// (PPSSPP's own) a run lasts until the PSP's next picture: 2 vblanks in a game at 30 FPS - God of War once PSP Frame Rate
// Auto chose 30 -, one more for each picture skipped, up to 10 in a loading screen; until 00.81 the app ran it once per
// refresh, the game at 2x and more. The core gives its count of vblanks at the end of each run
// (EMUSUNI_ENVIRONMENT_PSP_VBLANKS: s_psp_vblanks), and the pacing pays for those a run covered as for Flycast's pictures
// (DcFrames): the runs its pass still planned first, then a run left out of each pass that follows. With Frame Skip off
// every run is one vblank (the core's frame duplication): nothing changes.
struct PspFrames
{
  unsigned long long last = 0;
  bool known = false;
  int candidate = 1, same = 0, logged = 1;
  int ahead = 0;

  // The vblanks the run just made covered: 1 to 10 (a run without the core's count, or one that ran none, 1).
  int Take()
  {
    int covered = 1;
    if (!s_psp_vblanks_known)
      return covered;
    const bool ran = known && s_psp_vblanks > last;
    if (ran)
      covered = static_cast<int>(std::min<unsigned long long>(s_psp_vblanks - last, 10));
    last = s_psp_vblanks;
    known = true;
    // A new spacing once 30 runs in a row agree: logged.
    if (ran)
    {
      same = covered == candidate ? same + 1 : 1;
      candidate = covered;
      if (same >= 30 && candidate != logged)
      {
        LogF("pacing: the PSP's pictures are %d vblanks apart now (was %d)%s", candidate, logged,
             candidate > 1 ? ": Frame Skip, or a game at 30 FPS with it" : "");
        logged = candidate;
      }
    }
    return covered;
  }
  // The vblanks a run covered beyond the runs its pass had planned: owed back, a run left out of each pass that follows
  // until they are paid.
  void Owe(int frames) { ahead = std::min(ahead + frames, 20); }
  int Pay(int frames)
  {
    const int paid = std::min(frames, ahead);
    ahead -= paid;
    return frames - paid;
  }
  // The vblanks counted from the next run on (the in-game menu closed).
  void Restart()
  {
    known = false;
    ahead = 0;
  }
};

// The start watchdog's times by console (src/StartGuard.h), in seconds: the game running without a picture, one call
// into the core, the core's load of the game. The cartridge consoles show a picture at their first frame; the
// Dreamcast's first start compiles its shaders (up to a minute, mostly between frames), the PSP's its pipelines; a
// NAOMI's cartridge (up to 384 MiB) is read from the USB drive in its load.
static StartGuard::Limits StartLimits(SystemId system)
{
  if (IsFlycast(system))
    return {120.0f, 120.0f, 180.0f};
  switch (system)
  {
  case SystemId::PSP:
    return {60.0f, 90.0f, 120.0f};
  case SystemId::N64:
  case SystemId::PS1:
  case SystemId::NDS:
  case SystemId::THREEDS:
  case SystemId::SATURN:
    return {30.0f, 60.0f, 90.0f};
#ifdef EMUSUNI_VITA
  // (with-vita) The PS Vita's first start installs the firmware and the game (minutes, the core's own progress on the
  // screen meanwhile): the longest times there are.
  case SystemId::VITA:
    return {120.0f, 120.0f, 180.0f};
#endif
#ifdef EMUSUNI_MUGEN
  // (01.27, with-mugen) A MUGEN game's first picture comes after Go's start (its test, once per version) and Ikemen's
  // own load of its scripts, screenpack and fonts: not measured on the PS4 yet.
  case SystemId::MUGEN:
    return {60.0f, 90.0f, 120.0f};
#endif
  case SystemId::ARCADE:
    return {30.0f, 45.0f, 90.0f};
  case SystemId::PCE:  // (00.82: cartridges, a picture at their first frame)
  default:
    return {20.0f, 45.0f, 90.0f};
  }
}

Result Run(Display& display, const Library::Game& game, ImFont* regular, ImFont* bold,
           std::string* error)
{
  LogF("game: %s (%s)", game.title.c_str(), game.path.c_str());
  // (00.95) A link cable match (src/GbLink.h) is this game's alone: over at every return.
  struct GbLinkEnd
  {
    ~GbLinkEnd()
    {
      GbLink::Clear();
      s_gblink = false;
      s_link_saves = nullptr;
      std::vector<uint8_t>().swap(s_gblink_rom2);
      std::vector<uint8_t>().swap(s_gblink_saved);
    }
  } gblink_end;
  // (00.87) The player's data on a USB drive (src/Portable.h) that was taken out: no game until EmuSuNi starts again -
  // its saves would begin empty in the fallback folder, and later be taken for newer than the drive's.
  if (!Portable::CheckDrive() || Portable::Lost())
  {
    *error = "The USB drive with your data was removed.\nRestart EmuSuNi to play:\nwith this PS4's data, or with the "
             "drive's\nonce it is connected again.";
    LogLine("game: not started: the USB drive with the player's data is gone (portable data)");
    return Result::Failed;
  }
  // The start watched until a while after its first picture (src/StartGuard.h); every return ends the watch.
  const StartGuard::Scope start_guard(game.path, game.title, GetSystem(game.system).key, StartLimits(game.system));
  if (!EnoughMemory(game, error))
    return Result::Failed;
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
  if (!by_path)
    StartGuard::Step("the game's file is read", true);
  if (!by_path && !Library::ReadRom(game.path, game.system, &rom, error))
  {
    LogF("game: can't read the ROM: %s", error->c_str());
    return Result::Failed;
  }
  s_system = game.system;
  s_core = GetSystem(game.system).core;
  // (00.95) A link cable match (src/GbLink.h): SameBoy runs both Game Boys, this game (Game Boy 1: the host's) and the
  // other (Game Boy 2: the guest's), whose ROM is read now.
  s_gblink = GbLink::Active();
  s_gblink_checked = false;
  s_gblink_failed = false;
  if (!s_gblink && Netplay::Active() && Netplay::LinkRoom())
  {
    // (A link cable room's match without its two games set - the menu's SetGbLinkMatch: not one Game Boy alone.)
    *error = "The Game Boy link cable's games aren't ready";
    LogLine("gblink: a link cable match without its games set (GbLink::Set): not started");
    return Result::Failed;
  }
  s_link_saves = nullptr;
  if (s_gblink)
  {
    // (01.04) Two Game Boy Advances: mGBA's link core (in mGBA's object) - else two Game Boys: SameBoy.
    const bool gba = GbLink::Gba();
#ifdef EMUSUNI_GBALINK
    if (gba)
    {
      s_core = &Cores::GBALINK;
      s_link_saves = &GBALINK_SAVES;
    }
#endif
#ifdef EMUSUNI_GBLINK
    if (!gba)
    {
      s_core = &Cores::SAMEBOY;
      s_link_saves = &SAMEBOY_SAVES;
    }
#endif
    if (!s_link_saves)
    {
      *error = "This version of EmuSuNi has no Game Boy link cable";
      LogF("gblink: no %s in this build: the match can't start",
           gba ? "GBA link core (scripts/build-gba-core.sh)" : "SameBoy (scripts/build.sh with-gblink)");
      return Result::Failed;
    }
    const GbLink::Game& other = GbLink::Get().games[1];
    if (!Library::ReadRom(other.path, other.system, &s_gblink_rom2, error))
    {
      LogF("gblink: can't read %s 2's ROM (%s): %s", gba ? "GBA" : "Game Boy", other.path.c_str(), error->c_str());
      return Result::Failed;
    }
  }
  // (00.23) Online (src/Netplay.h): the core's controller devices kept by the match (the host's are the guest's); the
  // room left, the others told, at every end of the game (NetplayEnd).
  const bool online = Netplay::Active();
  if (online)
    s_core = Netplay::WrapCore(s_core);
  UsageStats::SetPlaying(GetSystem(game.system).key, online);  // (00.23: the anonymous statistics' "playing")
  MyStats::GameStarted(game.path, GetSystem(game.system).key, online);  // (00.23: Settings -> My Stats, on the PS4)
  struct NetplayEnd
  {
    bool on;
    ~NetplayEnd()
    {
      UsageStats::SetPlaying(nullptr, false);
      MyStats::GameEnded();
      if (on)
        Netplay::GameEnded();
    }
  } netplay_end{online};
  s_game_id = game.key;
  s_message.clear();
  s_vita_step.clear();  // ((01.66) its loading screen: from its first step; drawn from the start)
  s_vita_loading = IsVita(game.system);
#ifdef EMUSUNI_VITA
  s_vita_pointer = {};  // ((01.67) off at each start, in the middle)
#endif
  s_ps1_disc_error.clear();
  s_ps1_libcrypt = false;
  s_ps1_openbios = false;
  s_ps1_cooked = false;
  s_display = &display;
  s_pad_handles.fill(-1);
  s_rumble = {};
  s_hw_render = {};
  s_hw_vulkan = false;
  s_hw_image_set = false;
  s_frame_hw = false;
  s_xrgb8888 = false;  // RGB565 unless the core asks for XRGB8888
  s_pointer = {};
  s_stylus = {};
  s_jaguar_key = -1;
  s_r3_swap = false;
  s_3ds_r3 = false;
  s_l3 = {};  // (01.31)
  s_screens_seen.clear();
  s_3ds_scale = 0;
  s_3ds_new = -1;
  s_3ds_title = 0;
  s_3ds_new_only = false;  // (01.36)
  s_3ds_cursor = 0;
  s_3ds_at = {};
  // The stylus starts in the middle of the touch screen: the 3DS's 320x240, the DS's 256x192.
  if (s_system == SystemId::THREEDS)
  {
    s_stylus.x = 160.0f;
    s_stylus.y = 120.0f;
  }
  else if (s_system == SystemId::NDS)
  {
    s_stylus.x = 128.0f;
    s_stylus.y = 96.0f;
  }
  s_touch_frames = s_tap_frames = s_touch_taps = 0;
  s_stick_dpad_frames = 0;
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
  // (00.87) The player's saves where the player's data is (src/Portable.h: a USB drive's portable data, else
  // /data/EmuSuNi): every core's own folders in them too (PPSSPP's memory stick, Flycast's VMUs, the Saturn's backup
  // memory, Azahar's sdmc and nand). The BIOS from the drive's EmuSuNi-Portable/system when the player put the
  // console's there, else from /data/EmuSuNi/system.
  s_system_dir = Portable::SystemDir(GetSystem(s_system).key);
  s_save_dir = Portable::UserDir() + "/saves";
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
#ifdef EMUSUNI_SATURN
  if (s_system == SystemId::SATURN)
  {
    // The Saturn: its BIOS (512 KiB: the owner's keeps its own name) found here in system/saturn, system or the
    // game's folder - (01.91, the owner) or a folder above it, up to Roms/saturn: a game in its own folder ("Panzer
    // Dragoon (Europe)/...cue") finds one left in Roms/saturn -, and given to the core by its path
    // (EMUSUNI_SATURN_BIOS: scripts/build-saturn-core.sh); the backup memory (the games' saves, one file for all) in
    // saves/saturn/yabasanshiro.
    s_system_dir += "/saturn";
    s_save_dir += "/saturn";
    MakeDirs(s_system_dir);
    MakeDirs(s_save_dir);
    uint32_t crc = 0;
    // (00.87: the portable data's system/saturn and system first, then this PS4's - src/Portable.h.)
    std::vector<std::string> bios_folders = Portable::BiosFolders("saturn");
    const std::string game_folder = std::filesystem::path(game.path).parent_path().string();
    bios_folders.push_back(game_folder);
    for (const std::string& above :
         Library::FoldersAbove(game_folder, Library::ConsoleRoot(game_folder, SystemId::SATURN, App::DATA_DIR)))
      bios_folders.push_back(above);  // ((01.91) nearest first, up to Roms/saturn)
    const std::string bios = SaturnTest::FindSaturnBios(bios_folders, &crc);
    if (bios.empty())
    {
      LogF("saturn: no BIOS (a .bin of 512 KiB) in %s, system, the game's folder or the folders above it up to "
           "Roms/saturn", s_system_dir.c_str());
      // ((01.92) EmuSuNi's own folder written out in full: the portable data's when it is in use (BiosFolders' first).)
      *error = "No Saturn BIOS found.\nBIOS: the USA one, 512 KB (any name with \"bios\" in it),\n"
               "in the game's folder, a folder above it,\nor in EmuSuNi's BIOS folder:\n" + bios_folders.front();
      return Result::Failed;
    }
    LogF("saturn: BIOS %s (CRC32 %08x: %s)", bios.c_str(), static_cast<unsigned>(crc),
         SaturnTest::SaturnBiosName(crc).c_str());
    setenv("EMUSUNI_SATURN_BIOS", bios.c_str(), 1);
    // (00.69) The SH-2s' waits counted, not run (scripts/build-saturn-core.sh); the file
    // /data/EmuSuNi/saturn-sh2-skip-off (any content) runs every turn of them as 00.68 did.
    const bool skip_off = FileExists(std::string(App::DATA_DIR) + "/saturn-sh2-skip-off");
    setenv("EMUSUNI_SATURN_SH2_SKIP", skip_off ? "0" : "1", 1);
    LogF("saturn: the SH-2s' waits %s", skip_off ? "run turn by turn (the file saturn-sh2-skip-off)" :
                                                   "counted, not run (the file /data/EmuSuNi/saturn-sh2-skip-off: "
                                                   "run them)");
  }
#endif
  // The PC Engine (00.82): system/pce, the core's system folder (no file is needed for its HuCards).
  if (s_system == SystemId::PCE)
  {
    s_system_dir += "/pce";
    MakeDirs(s_system_dir);
  }
  // Atari's consoles (00.90): their own saves' folder (the 2600's Stella keeps the SaveKey's and AtariVox's EEPROM and
  // a cartridge's flash in saves/atari2600/stella/nvram; the Jaguar's EEPROM saves are the app's, saves/atarijaguar). The
  // system folder as RetroArch's: the 7800's optional BIOS ("7800 BIOS (U).rom") and the Jaguar's
  // ("[BIOS] Atari Jaguar (World).j64", jagboot.rom) right in /data/EmuSuNi/system.
  if (s_system == SystemId::A2600)
  {
    s_save_dir += "/atari2600";
    MakeDirs(s_save_dir);
  }
#ifdef EMUSUNI_OPENBOR
  // (with-openbor) OpenBOR: what its games write - settings (<mod>.cfg), high scores, saved games, the engine's log -
  // in saves/openbor/OpenBOR/<engine build>/{Saves,Logs,ScreenShots}, made by the core (a folder per engine version:
  // those files are raw structures that differ between versions).
  if (s_system == SystemId::OPENBOR)
  {
    s_save_dir += "/openbor";
    MakeDirs(s_save_dir);
  }
#endif
#ifdef EMUSUNI_VITA
  // (with-vita) The PS Vita: Vita3K's own folder is system/vita3k (the firmware it installs from the .PUP files there,
  // the games it installs, its caches); the games' saves in saves/psvita/vita3k. Without the firmware a game shows
  // nothing: said here (the menu then says which files to bring and copies them, src/Vita.h) instead of starting it.
  if (s_system == SystemId::VITA)
  {
    s_save_dir += "/psvita";
    MakeDirs(s_save_dir);
    MakeDirs(s_system_dir + "/vita3k");
    const Vita::Firmware firmware = Vita::FindFirmware(s_system_dir);
    LogF("vita: firmware in %s/vita3k: %s", s_system_dir.c_str(), Vita::Describe(firmware).c_str());
    if (!firmware.Ready())
    {
      *error = Vita::MissingMessage(firmware, s_system_dir);
      return Result::Failed;
    }
#ifdef EMUSUNI_VITA_NOKEYS
    // (with-vita-nokeys) This build's core has none of Sony's keys inside: they are a file of the player's
    // (src/Vita.h). Without it the firmware can't be installed and no game starts (the system's modules, which
    // every game loads, are encrypted on the disk): said here, the core isn't asked.
    {
      const Vita::Keys keys = Vita::FindKeys(s_system_dir);
      LogF("vita: keys: %s", Vita::DescribeKeys(keys, s_system_dir).c_str());
      if (keys != Vita::Keys::Ready)
      {
        LogF("vita: keys: needed for %s: not started",
             firmware.Installed() ? "the system's modules" : "the firmware's install");
        *error = Vita::KeysMessage(keys, s_system_dir, !firmware.Installed());
        return Result::Failed;
      }
    }
#endif
    if (!firmware.Installed())
      LogLine("vita: the core installs the firmware at this start (a minute or more, its progress on the screen)");
    Vita::RemoveCopiedFiles(s_system_dir);  // (installed at an earlier start: the app's copies of its .PUP files go)
  }
#endif
#ifdef EMUSUNI_MUGEN
  // (01.27, with-mugen) MUGEN: Go's runtime starts before its first game of the session, after its checks and once per
  // version its test (src/MugenGo.h, the log's "mugen: go:" lines); a PS4 where it can't run, or where it closed the
  // app the last time, is said here instead of starting the game. The game itself writes in its own folder (its
  // save/config.ini, its logs), as Ikemen GO does beside its executable.
  if (s_system == SystemId::MUGEN)
  {
    std::string why;
    if (!MugenGo::Prepare(App::DATA_DIR, &why))
    {
      *error = why;
      return Result::Failed;
    }
  }
#endif
  // A NAOMI or Atomiswave game: its board's EEPROM and flash (the operator menu's settings, the high scores) in
  // saves/naomi/reicast and saves/atomiswave/reicast, made by the core and named after the zip; system/dc (Flycast's
  // own folder) can hold the BIOS zips, which it also finds beside the game. What the game needs and isn't there is
  // said here, before the core is started (below), or if the core refuses it.
  std::vector<std::string> board_missing;
  s_board_set = nullptr;
  unsetenv("EMUSUNI_FOLDERS_ABOVE");  // ((01.91) a board game's only: below)
  if (IsFlycastBoard(s_system))
  {
    s_save_dir += s_system == SystemId::NAOMI ? "/naomi" : "/atomiswave";
    MakeDirs(s_save_dir);
    // ((01.91, the owner) A game in a folder below Roms/naomi or Roms/atomiswave: its BIOS zip, a clone's parent zip
    // and its parent's GD-ROM image looked for in the folders above it too, up to the console's - by the core as well
    // (EMUSUNI_FOLDERS_ABOVE: scripts/build-dc-core.sh), so both find the same files.)
    const std::string board_folder = std::filesystem::path(game.path).parent_path().string();
    const std::vector<std::string> board_above =
        Library::FoldersAbove(board_folder, Library::ConsoleRoot(board_folder, s_system, App::DATA_DIR));
    std::string above_list;
    for (const std::string& above : board_above)
    {
      above_list += (above_list.empty() ? "" : "\n") + above;
      LogF("naomi: its BIOS and parent also looked for in %s", above.c_str());
    }
    if (!above_list.empty())
      setenv("EMUSUNI_FOLDERS_ABOVE", above_list.c_str(), 1);
    // A cartridge image (.dat, .bin): no set of Flycast's list (its own board, inputs and picture), naomi.zip.
    const bool image = Naomi::IsImageName(game.path);
    s_board_set = image ? nullptr : Naomi::Find(game.id);
    if (image)
    {
      std::error_code size_error;
      const auto bytes = std::filesystem::file_size(game.path, size_error);
      board_missing = Naomi::ImageMissing(board_folder, board_above, s_system_dir);
      LogF("naomi: %s, a NAOMI cartridge image of %u MiB (Flycast plays it as it is); BIOS naomi.zip", game.id.c_str(),
           size_error ? 0u : static_cast<unsigned>(bytes >> 20));
    }
    if (s_board_set)
    {
      board_missing = Naomi::Missing(*s_board_set, board_folder, board_above, s_system_dir);
      LogF("naomi: %s, %s, \"%s\"; BIOS %s, cartridge %u MiB%s%s%s%s", game.id.c_str(),
           Naomi::BoardName(*s_board_set).c_str(), s_board_set->title.c_str(), Naomi::BiosZip(*s_board_set).c_str(),
           static_cast<unsigned>(s_board_set->size >> 20), s_board_set->vertical ? ", vertical" : "",
           s_board_set->analog ? ", analog controls" : "", s_board_set->gun ? ", light gun" : "",
           s_board_set->linked ? ", linked boards" : "");
    }
    for (const std::string& file : board_missing)
      LogF("naomi: %s isn't where the core looks for it (beside the game or above it, or %s/dc for a BIOS)",
           file.c_str(), s_system_dir.c_str());
    // What makes Flycast refuse the game is said here instead (00.64): its refusal is a C++ exception, and on the PS4
    // one doesn't come back to its catch - the app ends without a word. The board's BIOS not there, a GD-ROM game's
    // disc image not there, a zip whose name isn't its set's in small letters where the drive tells case apart (the
    // PS4's storage: Flycast finds its set by the name as it is); and (00.65) what else it would refuse a set for,
    // asked of the core itself as it would look (Naomi::CheckSet): a file of the set in neither its zip nor its
    // parent's (another MAME version's set), a BIOS zip without the files the board boots from.
    std::string refusal;
    const std::string bios = s_board_set ? Naomi::BiosZip(*s_board_set) : std::string("naomi.zip");
    const auto missing = [&board_missing](const std::string& file) {
      return std::find(board_missing.begin(), board_missing.end(), file) != board_missing.end();
    };
    if ((image || s_board_set) && missing(bios))
      refusal = "Error: cannot load BIOS " + bios;
    else if (s_board_set && !s_board_set->gdrom.empty() && missing(Naomi::GdromFile(*s_board_set)))
      refusal = "Naomi GDROM: no " + Naomi::GdromFile(*s_board_set);
    else if (!image)
    {
      // (Run passes the zip's name in small letters when the drive finds the file so, else as it is: below.)
      const std::filesystem::path path(game.path);
      std::string lower = path.filename().string();
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const bool lower_there = FileExists((path.parent_path() / lower).string());
      const std::string passed = lower_there ? std::filesystem::path(lower).stem().string() : path.stem().string();
      // (00.90: a zip named after its game, or in capitals, is its set too - Naomi::CheckSet tells the core which.)
      if (!s_board_set || Naomi::Find(passed) != s_board_set)
        refusal = "Unknown game";
      else
        refusal = Naomi::CheckSet(lower_there ? (path.parent_path() / lower).string() : game.path, s_system_dir);
    }
    if (!refusal.empty())
    {
      LogF("naomi: %s not started: %s (Flycast would refuse it)", game.id.c_str(), refusal.c_str());
      *error = Naomi::LoadError(refusal, s_board_set, board_missing, s_system_dir);
      return Result::Failed;
    }
  }
  s_board_press_until = {};
  s_gun_reload = {};
  if (s_system == SystemId::THREEDS)
  {
    // Azahar's files in saves/3ds/Azahar (made by the core): its sdmc and nand (the games' saves), the shader
    // caches, its keys (sysdata/aes_keys.txt, optional).
    s_save_dir += "/3ds";
    MakeDirs(s_save_dir);
  }
  // The zips an arcade game may need and hasn't got beside it (its BIOS set, its parent), said if the core
  // refuses the game. Not a reason to refuse it here: the core takes the ROMs it finds, by their CRCs, from
  // any zip it knows of, so a non-merged set (a clone's zip with its parent's files, a full one's with the
  // BIOS's too) starts without them.
  std::vector<std::string> arcade_not_found;
  std::vector<std::string> arcade_above;  // ((01.91) the folders above the game's, up to Roms/arcade: Arcade::FoldersAbove)
  if (s_system == SystemId::ARCADE)
  {
    const Arcade::Driver* driver = Arcade::Find(game.id);
    const std::string folder = std::filesystem::path(game.path).parent_path().string();
    arcade_above = Arcade::FoldersAbove(folder, Library::ConsoleRoot(folder, SystemId::ARCADE, App::DATA_DIR));
    Arcade::SetCoreFolders(arcade_above);  // (the core looks in them too, after the game's folder)
    for (const std::string& above : arcade_above)
      LogF("arcade: its parent and BIOS sets also looked for in %s", above.c_str());
    if (driver)
      arcade_not_found = Arcade::Missing(*driver, folder, arcade_above, s_system_dir);
    for (const std::string& zip : arcade_not_found)
      LogF("arcade: %s isn't beside %s (the core goes on without it)", zip.c_str(), game.id.c_str());
    // FinalBurn Neo's folders (saves/fbneo: the games' NVRAM; system/fbneo: hiscore.dat, samples,
    // cheats; system/fbneo/patched: the patched romsets), and its high-score table from the package the
    // first time (scripts/package.sh).
    MakeDirs(s_system_dir + "/fbneo/patched");
    MakeDirs(s_save_dir + "/fbneo");
    // ((01.26) Its cheats: the player's choices for this game - none online -, a file downloaded for it put in place.)
    ArcadeCheats::Begin(Portable::UserDir() + "/cheats/" + game.key + ".fbneo", online, s_system_dir + "/fbneo/cheats",
                        game.id);
    // (01.05) A patched romset is the core's first choice (BuildArcadeVariables): the log says when one is there.
    for (const char* ext : {".zip", ".7z"})
    {
      const std::string patched = s_system_dir + "/fbneo/patched/" + game.id + ext;
      if (FileExists(patched))
        LogF("arcade: patched romset %s (%s; its CRCs aren't checked)", patched.c_str(),
             Netplay::Active() ? "not used online" : "used before the game's own zip");
    }
    const std::string hiscore = s_system_dir + "/fbneo/hiscore.dat";
    if (!FileExists(hiscore))
    {
      const std::string table = ReadTextFile(std::string(App::ASSETS_DIR) + "/fbneo/hiscore.dat");
      if (!table.empty() && WriteTextFile(hiscore, table))
        LogF("arcade: hiscore.dat copied to %s (%zu bytes)", hiscore.c_str(), table.size());
    }
  }
  s_shutdown = false;
  s_dc_reboot = s_dc_stopped = false;
  s_psp_scale = 0;
  s_psp_at_start = s_system == SystemId::PSP && FileExists(std::string(App::DATA_DIR) + "/psp-resolution-at-start");
  if (s_psp_at_start)
    LogLine("psp: resolution and anti-aliasing as the game starts (the file psp-resolution-at-start)");
  s_gpgx_region.clear();
  s_dc_clock.clear();
  s_dc_resolution.clear();
  s_dc_anisotropy.clear();
  // (Before the core starts any of its threads.)
  s_dc_threads_free = IsFlycast(s_system) && FileExists(std::string(App::DATA_DIR) + "/dc-threads-free");
#ifdef EMUSUNI_DC
  emusuni_dc_threads_free = s_dc_threads_free ? 1 : 0;
#endif
  s_rotation = 0;
  s_core_aspect = 0.0f;
  s_psp_30fps_auto = false;
  s_psp_gow = false;
  s_psp_stopped = false;  // ((01.07))
  s_psp_vblanks_known = false;  // (the PSP core gives its count again from its first run: PspFrames)
  s_nds_r3 = false;
  Saves::Init(Portable::UserDir());  // (00.87: src/Portable.h)
  // A Game Boy cartridge's header: for the Game Boy Color (0x80: also the Game Boy, 0xC0: only it),
  // with Super Game Boy features (0x03).
  s_gb_color_game = rom.size() > 0x146 && (rom[0x143] & 0x80) != 0;
  s_gb_sgb_game = rom.size() > 0x146 && rom[0x146] == 0x03;
  s_tilt = {};
  s_n64_name = s_system == SystemId::N64 ? N64Name(rom) : std::string();
  s_n64_code = s_system == SystemId::N64 ? N64Code(rom) : std::string();
  s_n64_crc1 = s_system == SystemId::N64 ? N64Crc(rom, 0x10) : 0;
  s_n64_crc2 = s_system == SystemId::N64 ? N64Crc(rom, 0x14) : 0;
  s_3ds_title = s_system == SystemId::THREEDS ? ThreedsTitleId(game.path) : 0;  // (00.81: before Build3dsVariables)
  // ((01.36) New 3DS Mode out of the menu: what its SMDH says, read before Build3dsVariables as the title ID.)
  s_3ds_new_only = s_system == SystemId::THREEDS && ThreedsNew3dsOnly(game.path);
  if (s_system == SystemId::PSP)
  {
    // (00.81, GitHub issue #19: what a PSP game's speed and stick depend on, in a player's log. Frame Skip is PPSSPP's
    // own: until 00.81 it made a 30 FPS game - God of War once PSP Frame Rate Auto chose 30 - run at twice its speed.)
    const Settings::Scope scope = Settings::ScopeOf(game.key);
    LogF("psp: settings from %s: Frame Skip %s, PSP Frame Rate %s, Emulation Speed %s%%, stick dead zone %s, "
         "sensitivity %s",
         scope == Settings::Scope::Game ? "This Game" : scope == Settings::Scope::Console ? "This Console" : "All Games",
         Settings::Get("frameskip", game.key).c_str(), Settings::Get("psp_frame_rate", game.key).c_str(),
         Settings::Get("speed", game.key).c_str(), Settings::Get("psp_deadzone", game.key).c_str(),
         Settings::Get("psp_sensitivity", game.key).c_str());
  }
  if (s_system == SystemId::N64)
  {
    LogF("n64: cartridge \"%s\" (game code %s), N64 CPU Timing %s, N64 Graphics Sync %s", s_n64_name.c_str(),
         s_n64_code.empty() ? "none" : s_n64_code.c_str(), Settings::Get("n64_cpu_timing", game.key).c_str(),
         Settings::Get("n64_sync", game.key).c_str());
    if (const N64AccurateTitle* accurate = N64AccurateGame(s_n64_code))
    {
      LogF("n64: %s plays with N64 Graphics Sync Accurate whatever the setting (%s)", accurate->name, accurate->why);
      GameDb::Log("n64", accurate->name, "N64 Graphics Sync", "Accurate",
                  std::string("EmuSuNi's list: ") + accurate->why);
    }
    // (00.81) The app's CPU timing for it, over the core's game list (whose settings for the game are logged once the
    // core has started: GameDb::LogN64).
    std::string count_source;
    const std::string count = N64CountPerOp(game.key, &count_source);
    if (!count_source.empty())
      GameDb::Log("n64", s_n64_name, "CountPerOp", count, count_source);
    // Where its picture's settings come from: a console with settings of its own (This Console) doesn't follow the
    // main Settings' Quality - "Banjo-Kazooie stays blurry however I set the graphics" (src/Settings.cpp,
    // UseConsoleByDefault).
    const Settings::Scope scope = Settings::ScopeOf(game.key);
    LogF("n64: picture settings from %s: Quality %s (All Games: %s), N64 Resolution %sx, GPU Upscaler %s, N64 2D %s",
         scope == Settings::Scope::Game ? "This Game" : scope == Settings::Scope::Console ? "This Console" : "All Games",
         Settings::Get("quality", game.key).c_str(), Settings::Get("quality").c_str(),
         Settings::Get("n64_resolution", game.key).c_str(), Settings::Get("gpu_filter", game.key).c_str(),
         Settings::Get("n64_2d", game.key).c_str());
  }
  if (s_system == SystemId::GB || s_system == SystemId::GBC)
  {
    LogF("gb: a game for the %s%s", s_gb_color_game ? "Game Boy Color" : "Game Boy",
         s_gb_sgb_game ? ", with Super Game Boy features" : "");
  }
#ifdef EMUSUNI_3DS
  display.PublishDeviceLists(s_system == SystemId::THREEDS);  // (the 3DS core is told more of the device)
#endif
  // (00.23) Multiplayer Adapter: what the game is (its cartridge's bytes are read now: some cores free them once it
  // loaded), the adapter for it (a PlayStation disc's Auto again once the core read its code: below).
  s_mp_game = Multiplayer::ReadGame(game.system, game.path, rom);
  UsageStats::SetGame(GetSystem(game.system).key, game.path, rom);  // (00.23: a checksum or set, never the name)
  s_adapter = Multiplayer::Adapter::None;
  if (Multiplayer::HasAdapter(s_system))
    s_adapter = Multiplayer::Decide(Settings::Get("multiplayer_adapter", game.key), s_mp_game).adapter;
  BuildVariables();
  // (00.23) Online: the host's core options are the match's - sent from here; a guest waits for them before its core
  // starts (the emulation the same on both from the first frame).
  if (online && !NetplayAgree(display, regular, bold, error))
    return error->empty() ? Result::Menu : Result::Failed;
  if (s_gblink)
    GbLinkLocalVariables();  // (00.95: a guest has the host's options now, its sound too)
  if (IsFlycast(s_system))
  {
    if (IsFlycastBoard(s_system))
      LogF("naomi: region %s, TEST and SERVICE buttons on, free play %s", s_variables["reicast_region"].c_str(),
           s_variables["reicast_force_freeplay"].c_str());
    LogF("dc: CPU clock %s MHz (Dreamcast CPU Clock), sound DSP %s; picture %s, texture sharpness %s (Quality); "
         "language %s, region %s (Dreamcast Language %s); online %s (Dreamcast Online); threads %s",
         s_dc_clock.c_str(), s_variables["reicast_enable_dsp"] == "enabled" ? "on" : "off", s_dc_resolution.c_str(),
         s_dc_anisotropy == "off" ? "off" : (s_dc_anisotropy + "x").c_str(), s_variables["reicast_language"].c_str(),
         s_variables["reicast_region"].c_str(), Settings::Get("dc_language", game.key).c_str(),
         s_variables["reicast_dcnet"] != "enabled"          ? "off" :
         s_variables["reicast_emulate_bba"] == "enabled"    ? "DCNet, broadband adapter" :
                                                              "DCNet, modem",
         s_dc_threads_free ? "left to the PS4: any core, the priority they start with (/data/EmuSuNi/dc-threads-free)" :
                             "placed: the renderer (retro_run) on core 2, Flycast-emu on core 3, both at priority 480");
  }
  if (s_system == SystemId::THREEDS)
  {
    // Its level and size (the Quality levels since 00.71: 3DS Resolution 1x to 4x, the GPU Upscaler), and where they
    // come from (This Console doesn't follow All Games' Quality: the N64's line says the same).
    const Settings::Scope scope = Settings::ScopeOf(game.key);
    const std::string screens = Settings::Get("3ds_layout", game.key);
    const ThreedsPicture where = ThreedsLayout(s_3ds_scale);
    LogF("3ds: picture settings from %s: Quality %s (All Games: %s), 3DS Resolution %dx (the picture %ux%u, screens "
         "%s; the size until the game's next start), GPU Upscaler %s",
         scope == Settings::Scope::Game ? "This Game" : scope == Settings::Scope::Console ? "This Console" : "All Games",
         Settings::Get("quality", game.key).c_str(), Settings::Get("quality").c_str(), s_3ds_scale, where.w, where.h,
         screens.c_str(), Settings::Get("gpu_filter", game.key).c_str());
    // (00.81) The title ID, and what was made of the three for it - (01.36) by the game alone, the menu has them no more
    // (the players' graphics errors).
    const int eye_file = ThreedsTestFile("3ds-right-eye");
    LogF("3ds: title ID %016llx; %s 3DS (%s); Accurate Shader Multiplication %s (%s); right eye %s (%s)",
         static_cast<unsigned long long>(s_3ds_title), s_3ds_new == 1 ? "New" : "Old", s_3ds_new_why,
         s_3ds_accurate_mul ? "on" : "off",
         ThreedsTestFile("3ds-accurate-mul") >= 0 ? "/data/EmuSuNi/3ds-accurate-mul" :
         ThreedsAccurateMulGame(s_3ds_title)       ? "a game known to need it" :
                                                     "not one of the games that need it",
         eye_file == 1 ? "drawn" : eye_file == 0 ? "not drawn" : s_3ds_right_eye ? "drawn" : "not drawn",
         eye_file >= 0                        ? "/data/EmuSuNi/3ds-right-eye, the core's" :
         ThreedsLeftEyeSkipsGame(s_3ds_title) ? "without it the left eye skips pictures in this game" :
         ThreedsRightEyeGame(s_3ds_title)     ? "a game that breaks without it" :
                                                "left out: faster");
    // (00.85) The GPU timing emulation (Luigi's Mansion 2), and the vertex shaders on the CPU (the diagnosis file).
    LogF("3ds: GPU timings %s%s; vertex shaders on the %s",
         s_3ds_gpu_timings ? "emulated (the game's list)" : "not emulated",
         FileExists("/data/EmuSuNi/3ds-gpu-timings") ? " (/data/EmuSuNi/3ds-gpu-timings: emulated in every game)" : "",
         FileExists("/data/EmuSuNi/3ds-cpu-shaders") ? "CPU (/data/EmuSuNi/3ds-cpu-shaders)" : "GPU");
#ifdef EMUSUNI_3DS
    // (01.09) Its mods: on or off (Updates, DLC and Mods), and whether its folder has any.
    if (s_3ds_title != 0)
      LogF("3ds: mods %s (%s)", ThreeDsCia::ModsOn(s_3ds_title) ? "on" : "off (the game's Updates, DLC and Mods)",
           ThreeDsCia::HasMods(s_save_dir, s_3ds_title) ? "its folder has some" : "none in its folder");
    // (01.46) Each of them: the update's and DLC's switches, the mods in folders of their own (what the core does of
    // them: its caps).
    if (s_3ds_title != 0)
    {
      const std::vector<ThreeDsCia::Mod> mods = ThreeDsCia::ListMods(s_save_dir, s_3ds_title);
      std::string own;
      for (const ThreeDsCia::Mod& mod : mods)
      {
        if (!mod.main)
          own += (own.empty() ? "" : ", ") + mod.name + (ThreeDsCia::ModOn(s_3ds_title, mod) ? " on" : " off");
      }
      LogF("3ds: update %s, DLC %s; mods in folders of their own: %s (the core's extras caps %d)",
           ThreeDsCia::UpdateOn(s_3ds_title) ? "on" : "off", ThreeDsCia::DlcOn(s_3ds_title) ? "on" : "off",
           own.empty() ? "none" : own.c_str(), ThreeDsCia::ExtrasCaps());
    }
    // (01.42) Its texture pack: on, off, or on but left out this time (the GPU memory: EnoughMemory's line).
    if (s_3ds_title != 0 && (s_3ds_textures || s_3ds_textures_short >= 0 || ThreeDsTextures::On(s_3ds_title)))
      LogF("3ds: texture pack %s (%s)",
           s_3ds_textures                ? "on" :
           s_3ds_textures_short >= 0     ? "left out this time: too little GPU memory free" :
                                           "on, but no picture in its folder",
           ThreeDsTextures::InstalledFolder(s_save_dir, s_3ds_title).c_str());
#endif
  }
  LogScreens(game.key, "the game's start");
  if (TwoScreens())
    s_screens_seen = ScreensSeen(game.key);
  s_frame = nullptr;
  s_frame_w = s_frame_h = 0;
  s_texture_w = s_texture_h = 0;
  s_texture_scaled = false;
  s_buttons.fill(0);

  s_core_keys.clear();  // declared again by set_environment (SET_VARIABLES)
  s_undeclared_logged.clear();
  s_disk = {};  // set again by the core's init (SET_DISK_CONTROL_EXT_INTERFACE)
  s_disk_set = false;
  StartGuard::Step("the core starts");
  s_core->set_environment(Environment);
  s_core->set_video_refresh(VideoRefresh);
  s_core->set_audio_sample(AudioSample);
  s_core->set_audio_sample_batch(AudioBatch);
  s_core->set_input_poll(InputPoll);
  s_core->set_input_state(InputState);
  s_core->init();
  // The Dreamcast's pads before the game (set after it, Flycast unplugs them for 0.1 s at its first
  // frame, and a game continued from its state then sees no pad). The arcade boards' too: a pad on each of
  // the 4 ports (their JVS board reads the 4 players from them; a light gun game is aimed with a stick).
  if (IsFlycast(s_system))
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
  // (01.06) The 3DS: its frame handed over without the core waiting for its Vulkan worker at each frame's end (Donkey
  // Kong Country Returns 3D: 2.7-4 ms a frame on the PS4); the drawing waits for each picture's submission itself,
  // beside the next frame's emulation in the heavy mode (scripts/build-3ds-core.sh). The file 3ds-handoff-sync: as
  // before.
  s_3ds_handover = false;
#ifdef EMUSUNI_3DS
  if (s_system == SystemId::THREEDS)
  {
    s_3ds_handover = !FileExists(std::string(App::DATA_DIR) + "/3ds-handoff-sync");
    if (s_3ds_handover)
      setenv("EMUSUNI_3DS_ASYNC_HANDOFF", "1", 1);
    else
      unsetenv("EMUSUNI_3DS_ASYNC_HANDOFF");
    LogF("3ds: the frame handed over %s", s_3ds_handover ?
                                            "without the core waiting for its Vulkan worker (the drawing waits for "
                                            "each picture's submission)" :
                                            "once the core's Vulkan worker sent it, as before (the file "
                                            "3ds-handoff-sync)");
  }
#endif
  // PPSSPP's pipelines with the driver's own cache data kept (build-psp-core.sh): those of earlier
  // sessions aren't compiled again at each start (God of War recompiled 115, 60-140 ms each).
  if (s_system == SystemId::PSP)
    setenv("EMUSUNI_PSP_PIPELINE_CACHE", "1", 1);
  // PPSSPP's CPU depth rasterizer (the lens flares of Midnight Club: LA Remix, Syphon Filter, Wipeout...: compat.ini's
  // SoftwareRasterDepth), on a thread of its own (00.89, build-psp-core.sh): at low quality, as upstream on phones,
  // unless the file psp-depth-raster-full or psp-depth-raster-off says otherwise (an A/B on the PS4).
  if (s_system == SystemId::PSP)
  {
    const char* depth = FileExists(std::string(App::DATA_DIR) + "/psp-depth-raster-off")  ? "off" :
                        FileExists(std::string(App::DATA_DIR) + "/psp-depth-raster-full") ? "full" :
                                                                                            "low";
    setenv("EMUSUNI_PSP_DEPTH_RASTER", depth, 1);
    if (strcmp(depth, "low") != 0)
      LogF("psp: depth raster %s (the file psp-depth-raster-%s)", depth, depth);
  }
  // A game on several discs: an .m3u of them (data_dir/cache/m3u/<console>), the core's disc changer
  // swaps them (the in-game menu's Disc). It starts with the disc it was played with last: a state
  // saved on disc 2 is disc 2's.
  std::string content = game.path;
  if (IsRomsetSystem(s_system))
  {
    // The zip's name is the romset's, in lower case (the core compares it with its drivers' names; the
    // USB drive finds MSLUG.ZIP for mslug.zip). (A NAOMI's or Atomiswave's: Flycast compares it with its
    // sets' names the same way; where the drive tells case apart and has no such file, the name as it is.)
    const std::filesystem::path path(game.path);
    std::string file = path.filename().string();
    std::transform(file.begin(), file.end(), file.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    content = (path.parent_path() / file).string();
    if (IsFlycastBoard(s_system) && content != game.path && !FileExists(content))
      content = game.path;
  }
  // (00.81) A PlayStation disc's tracks without their .cue: a .cue made for them (Ps1DiscContent), each disc's.
  if (s_system == SystemId::PS1 && game.discs.size() <= 1)
    content = Ps1DiscContent(content);
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
      list += (s_system == SystemId::PS1 ? Ps1DiscContent(disc) : disc) + "\n";
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
  // RetroAchievements (00.73): the game identified as rcheevos does it (a cartridge from the ROM's bytes, now: the
  // core may free its copy; a disc - the one it starts with - or an arcade set from its file, on the module's
  // thread) and its achievements asked for. Nothing when no one is logged in; never a reason not to start the game.
  if (!s_gblink)  // (00.95: a link cable match's two Game Boys: no achievements, as online)
  {
    retro_system_info core_info = {};
    s_core->get_system_info(&core_info);
    Achievements::Game ra;
    ra.system = s_system;
    ra.path = discs.size() > 1 && last_disc >= 0 && last_disc < static_cast<int>(discs.size()) ? discs[last_disc] :
              content;
    ra.data = rom.empty() ? nullptr : rom.data();
    ra.size = rom.size();
    ra.title = game.title;
    ra.library_name = core_info.library_name ? core_info.library_name : "";
    ra.library_version = core_info.library_version ? core_info.library_version : "";
    Achievements::SetCoreMemory(s_core->get_memory_data, s_core->get_memory_size);
    Achievements::GameStarting(ra);
  }
  StartGuard::Step("the core loads the game", true);
#ifdef EMUSUNI_3DS
  if (s_system == SystemId::THREEDS)
    emusuni_3ds_game_starts();
#endif
  bool loaded = false;
  if (s_gblink)
  {
    // (00.95) The link cable's two Game Boys: Game Boy 1 this game, Game Boy 2 the other (the core copies both ROMs).
    // (01.04: or two GBAs, mGBA's link core.)
    retro_game_info games[2] = {info, {}};
    games[1].path = GbLink::Get().games[1].path.c_str();
    games[1].data = s_gblink_rom2.data();
    games[1].size = s_gblink_rom2.size();
    loaded = s_core->load_game_special(GbLink::GAME_TYPE_LINK_2P, games, 2);
    std::vector<uint8_t>().swap(s_gblink_rom2);
    const char* machine = GbLink::Gba() ? "GBA" : "Game Boy";
    LogF("gblink: %s %s %s 1 (%s) and %s 2 (%s), joined by the cable; ours %s %d", s_core->name,
         loaded ? "loaded" : "couldn't load", machine, GbLink::Get().games[0].title.c_str(), machine,
         GbLink::Get().games[1].title.c_str(), machine, GbLink::Get().local + 1);
  }
  else
    loaded = s_core->load_game(&info);
  s_info_ext = {};
  // Mupen64Plus, mGBA and Genesis Plus GX copy the ROM: up to 64 MiB back to the heap.
  // (00.82: Beetle PCE Fast too - a HuCard in its ROM space; 00.90: Stella too. ProSystem plays the 7800's from the
  // app's copy: kept.)
  if (s_system == SystemId::N64 || IsHandheld(s_system) || s_system == SystemId::NDS || IsGpgx(s_system) ||
      s_system == SystemId::PCE || s_system == SystemId::A2600)
    std::vector<uint8_t>().swap(rom);
  if (loaded && s_hw_vulkan && s_hw_render.context_reset)
  {
    // The context is ready (the display's device): the core sets up its renderer.
    StartGuard::Step("the core sets up its renderer");
    s_hw_render.context_reset();
  }
  // (00.81) A PlayStation disc image SwanStation couldn't open (a .cue whose tracks aren't there, a damaged file): it
  // started the console's BIOS without a disc - a black screen, nothing to play. The game ends here instead, as the
  // game's end does (its Vulkan objects, the game), and the menu says why.
  const bool ps1_no_disc = loaded && s_system == SystemId::PS1 && !s_ps1_disc_error.empty();
  if (ps1_no_disc)
  {
    LogF("ps1: the disc image couldn't be opened (%s): the game doesn't start", s_ps1_disc_error.c_str());
    if (s_hw_vulkan && s_hw_render.context_destroy)
    {
      display.WaitIdle();
      display.ForgetOpaqueViews();
      s_hw_render.context_destroy();
    }
    s_core->unload_game();
    loaded = false;
  }
  if (!loaded)
  {
    // The core's own reason when it gave one (a Famicom Disk System game without its BIOS).
    *error = s_message.empty() ? "The emulator couldn't load this ROM" : s_message;
    if (ps1_no_disc)
    {
      // A track the .cue names that isn't there (SwanStation: "Failed to open track filename '<path>' (from '<name>'
      // and '<cue>'): errno 2"): its name; another reason as the core says it, cut to the panel's width.
      const std::string missing = "Failed to open track filename '";
      const size_t from = s_ps1_disc_error.find("' (from '");
      if (s_ps1_disc_error.rfind(missing, 0) == 0 && from != std::string::npos &&
          s_ps1_disc_error.find("errno 2") != std::string::npos)
      {
        const std::string path = s_ps1_disc_error.substr(missing.size(), from - missing.size());
        *error = "The disc image couldn't be opened:\nThis file its .cue names isn't beside it:\n" +
                 std::filesystem::path(path).filename().string();
      }
      else
      {
        size_t cut = s_ps1_disc_error.size() > 90 ? 87 : s_ps1_disc_error.size();
        while (cut > 0 && cut < s_ps1_disc_error.size() && (static_cast<unsigned char>(s_ps1_disc_error[cut]) & 0xC0) == 0x80)
          cut--;  // (not inside a UTF-8 letter)
        *error = "The disc image couldn't be opened:\n" + s_ps1_disc_error.substr(0, cut) +
                 (cut < s_ps1_disc_error.size() ? "..." : "");
      }
    }
    if (s_system == SystemId::THREEDS)
      *error = ThreedsFailure(*error);
    // (01.29) A DS game whose ROM found no memory (melonDS DS: "Not enough memory for the ROM (512 MiB)",
    // scripts/build-nds-core.sh): in the app's words, back to the menu (until 01.28 new's failure ended the app).
    if (s_system == SystemId::NDS && error->find("Not enough memory for the ROM") != std::string::npos)
    {
      LogF("nds: %s", error->c_str());
      *error = "Not enough memory for this game";
    }
    if (error->find("disksys.rom") != std::string::npos)
      *error += "\nCopy the BIOS disksys.rom to\n" + s_system_dir;
#ifdef EMUSUNI_ARCADE
    if (s_system == SystemId::ARCADE)
    {
      std::string reason = fbneo_emusuni_error();
      *error = Arcade::LoadError(reason, arcade_not_found, s_system_dir, game.path, arcade_above);  // (00.90)
      std::replace(reason.begin(), reason.end(), '\n', '|');
      LogF("arcade: %s refused: %s", game.id.c_str(), reason.c_str());
      s_core->unload_game();  // what the load set up before it failed: FinalBurn Neo frees it here
    }
#endif
    // A NAOMI or Atomiswave game: Flycast's reason (its message: no BIOS, a file of the set not in its zip, no
    // disc image...) in the app's words, with where the files go. (What it loaded - the cartridge's memory - is
    // freed by the deinit below: Flycast's unloadGame of a game in its Error state.)
    if (IsFlycastBoard(s_system))
    {
      LogF("naomi: %s refused: %s", game.id.c_str(), s_message.empty() ? "(no message)" : s_message.c_str());
      *error = Naomi::LoadError(s_message, s_board_set, board_missing, s_system_dir);
    }
    // (00.89) A Dreamcast disc image Flycast can't read (its error, which closed the app before 00.89: the rescue of
    // scripts/build-dc-core.sh): said as such, with how a good one is made.
    if (s_system == SystemId::DC && !s_message.empty())
    {
      LogF("dc: %s refused: %s", game.id.c_str(), s_message.c_str());
      auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
      if (has(s_message, "CHD") || has(s_message, "chd:"))
        *error = "The disc image couldn't be opened:\n" + s_message +
                 "\nA Dreamcast CHD is made with chdman createcd\nfrom the game's .gdi or .cue";
      else if (has(s_message, "CDI") || has(s_message, "CUE") || has(s_message, "Unknown disk format") ||
               has(s_message, "This media cannot be loaded"))
        *error = "The disc image couldn't be opened:\n" + s_message +
                 "\nThe file is damaged or isn't a whole disc:\na .gdi with its tracks, a .chd, .cdi or .cue";
    }
    // The disc played last doesn't start (gone, or damaged): the game starts with disc 1 next time.
    if (last_disc > 0)
    {
      std::remove(Saves::DiscPath(game.key).c_str());
      *error += "\nIt starts with disc 1 next time.";
      LogF("game: disc %d didn't start: disc 1 next time", last_disc + 1);
    }
    s_message.clear();
    Achievements::GameEnded();
    s_core->deinit();
#ifdef EMUSUNI_SATURN
    unsetenv("EMUSUNI_SATURN_BIOS");
    unsetenv("EMUSUNI_SATURN_SH2_SKIP");
#endif
    JitArenaReset();
    return Result::Failed;
  }
  StartGuard::Step("the game loaded");
  // RetroAchievements (00.73): Snes9x gives no map of the console's memory - the app makes it (an SA-1 game's I-RAM,
  // which Snes9x shows no other way, included: src/AchievementsSnes.h).
  if (s_system == SystemId::SNES)
  {
    std::vector<retro_memory_descriptor> snes_map;
    if (AchievementsSnesMap(&snes_map))
    {
      const retro_memory_map map = {snes_map.data(), static_cast<unsigned>(snes_map.size())};
      Achievements::SetMemoryMap(&map);
    }
  }
  // A NAOMI cartridge image: Flycast's warning for every .dat and .bin ("Please upgrade to MAME romsets or expect
  // issues") isn't shown - the image plays (logged above). Flycast turns no image's picture (only its sets', from its
  // list: every vertical NAOMI game there is ROT270, a quarter turn): a vertical one's header says so, and the app
  // turns it the same way.
  if (IsFlycastBoard(s_system) && s_message == "Please upgrade to MAME romsets or expect issues")
    s_message.clear();
  Naomi::Image board_image;
  if (s_system == SystemId::NAOMI && Naomi::IsImageName(game.path) && Naomi::ReadImage(game.path, &board_image) &&
      board_image.vertical)
  {
    s_rotation = 1;
    LogF("naomi: a vertical game's image: its picture turned 90 degrees");
  }
  retro_system_av_info av = {};
  s_core->get_system_av_info(&av);
  NoteGeometry(av.geometry);  // (FinalBurn Neo sends no SET_GEOMETRY as a game starts: its shape is here)
  s_sample_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32040.0;
  s_game_fps = av.timing.fps > 0 ? av.timing.fps : 60.0988;
  // (00.90) The Jaguar's pictures change size with its games' video modes, and its drawing can run beside its emulation
  // (overlap_drawing): their RGB565 copy must never move while one is drawn - room for the largest from the start.
  if (s_system == SystemId::JAGUAR)
    s_converted.reserve(static_cast<size_t>(av.geometry.max_width) * av.geometry.max_height);
  LogF("game: %s, %.4f FPS, %.0f Hz sound, %s", s_core->name, s_game_fps, s_sample_rate,
       s_core->get_region() == RETRO_REGION_PAL ? "PAL" : "NTSC");
  // (00.81) The core's game list's settings for this cartridge (mupen64plus.ini: the core wrote its copy when it
  // started, and applies the entry itself), in the log. The other cores log their own lists' (PPSSPP's compat.ini
  // "gamedb: psp ...", SwanStation's "Game settings found for ...", Flycast's "Forcing ...").
  if (s_system == SystemId::N64)
    GameDb::LogN64(s_system_dir + "/Mupen64plus/mupen64plus.ini", s_n64_crc1, s_n64_crc2, s_n64_name);
  LogMemory("game loaded");
  // The PlayStation's pads are DualShocks (SwanStation's analog controller); the adapters for 3 or more players
  // (the Super Multitap, the Four Score...) are plugged in below (00.23: Multiplayer Adapter).
  constexpr unsigned DEVICE_DUALSHOCK = (1 << 8) | RETRO_DEVICE_ANALOG;
  // The Arcade's pads are FinalBurn Neo's "Modern" RetroPad (RETRO_DEVICE_SUBCLASS(ANALOG, 1): a Neo Geo
  // game's A, B, C, D on Square, Cross, Triangle, Circle, as on its own pad), on every port.
  constexpr unsigned DEVICE_MODERN = (2 << 8) | RETRO_DEVICE_ANALOG;
  const bool ps1 = s_system == SystemId::PS1, arcade = s_system == SystemId::ARCADE;
  s_core->set_controller_port_device(0, ps1 ? DEVICE_DUALSHOCK : arcade ? DEVICE_MODERN : RETRO_DEVICE_JOYPAD);
  s_core->set_controller_port_device(1, ps1 ? DEVICE_DUALSHOCK : arcade ? DEVICE_MODERN : RETRO_DEVICE_JOYPAD);
  for (unsigned port = 2; arcade && port < MAX_PLAYERS; port++)
    s_core->set_controller_port_device(port, DEVICE_MODERN);
  // (00.23) The game's adapter for 3 or more players: a PlayStation disc's Auto again, now that the core read its code
  // (its Multitap is an option of the core's: given to it with the others when it changes).
  if (Multiplayer::HasAdapter(s_system))
  {
    const Multiplayer::Decision adapter =
        Multiplayer::Decide(Settings::Get("multiplayer_adapter", game.key), s_mp_game);
    const bool options_changed = adapter.adapter != s_adapter;
    PlugAdapter(adapter, Multiplayer::Adapter::None, "the game's start");
    if (options_changed)
      BuildVariables();
  }
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
  menu.SetGame(Portable::UserDir(), game.key, game.title, game.system, game.path);  // (00.87: its cheats there)
  // The cheats (RetroAchievements is Casual only, 00.78: they always apply). (00.23: none online - one console's would
  // be the match's.)
  auto apply_cheats = [&]() {
    if (!online)
      menu.CheatList().ApplyToCore(*s_core, s_system);
    // ((01.26) The Arcade's: FinalBurn Neo's options, read by the core on its next frame - src/ArcadeCheats.h; online
    // none.)
    if (arcade && ArcadeCheats::TakeChanges(&s_variables))
      s_variables_changed = true;
  };
  // (01.36, report 5VT63M: Streets of Rage's red screen, its codes on) Genesis Plus GX writes a Mega Drive code for the
  // cartridge (a Game Genie's) into the game's ROM itself, and a game that adds its ROM up as it starts (Sega's red
  // screen when the sum is wrong) stops there. Its codes go in after CHEATS_AFTER_START frames of the game, at the start
  // and after a Reset (the ROM as it was first), as the player did by hand; a change in the in-game menu meanwhile waits
  // too (cheats_wait).
  constexpr int CHEATS_AFTER_START = 300;  // 5 s of the game
  int cheats_wait = 0;
  const auto codes_after_start = [&]() {
    const auto& entries = menu.CheatList().Entries();
    return s_system == SystemId::MD && !online &&
           std::any_of(entries.begin(), entries.end(), [](const Cheats::Entry& entry) { return entry.enabled; });
  };
  if (codes_after_start())
  {
    cheats_wait = CHEATS_AFTER_START;
    LogF("cheats: the Mega Drive's in after %d frames of the game (a ROM's sum checked as it starts)",
         CHEATS_AFTER_START);
  }
  else
    apply_cheats();
  if (!menu.CheatList().Entries().empty())
    LogF("cheats: %zu codes", menu.CheatList().Entries().size());
  if (arcade && !ArcadeCheats::List().empty())
    LogF("cheats: the arcade game has %zu, %d as the player chose them%s", ArcadeCheats::List().size(),
         ArcadeCheats::Given(), online ? " (online: none, the defaults)" : "");
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
      // (RetroAchievements: the disc identified.)
      if (disc >= 0 && disc < static_cast<int>(discs.size()))
        Achievements::DiscChanged(discs[disc]);
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
    Achievements::Reset();
    dc_restart_wait = 180;
    menu.Toast(DiscCount() > 1 ? "The console starts again with disc " + std::to_string(DiscIn() + 1) :
                                 std::string("The console starts again"),
               3.0f);
    LogF("game: the game went back to the Dreamcast's BIOS: the console starts again (disc %d)", DiscIn() + 1);
  };

  // Continue: the PSP's state after its first frame (PPSSPP's GPU is made by then); the Saturn's too (YabaSanshiro
  // takes a state only once it has run a frame: before, it says it took it).
  bool resume_pending = false;
  // (Not after the start watchdog restarted EmuSuNi because the start stopped while that state loaded: SkipContinue.)
  // (00.95: nor in a link cable match: its game's Continue is mGBA's, of the game alone.)
  // ((01.27) Triangle -> Load State: that slot instead, Continue on or off - LoadStateAtStart.)
  const int start_slot = TakeStartSlot(game.key);
  const int resume_slot = start_slot >= 0 ? start_slot : 0;
  // ((01.57) Started again for the N64 Resolution: its state kept apart instead of Continue's or a slot's.)
  bool restart_asked = false;
  const bool restarted = s_system == SystemId::N64 && LoadRestartState(game.key, &restart_asked);
  if (restart_asked)
    menu.Toast(restarted ? "N64 Resolution changed: the game goes on where it was" :
                           "N64 Resolution changed: the game started again (its state couldn't be loaded)",
               4.0f);
  const bool resume_state = !restart_asked && (start_slot >= 0 || Settings::GetOn("resume", game.key)) &&
                            Saves::Modified(Saves::StatePath(game.key, resume_slot)) && !StartGuard::SkipContinue() &&
                            !s_gblink;
  const std::string resume_toast =
      resume_slot > 0 ? "Loaded slot " + std::to_string(resume_slot) : std::string("Continuing where you left off");
  if (resume_state)
  {
    if (s_system == SystemId::PSP || s_system == SystemId::SATURN)
      resume_pending = true;
    else
    {
      StartGuard::Step(StartGuard::CONTINUE_STEP);
      if (LoadState(resume_slot))
        menu.Toast(resume_toast);
    }
  }
  if (s_system == SystemId::DC)
  {
    // Its first start compiles the shaders (black for up to a minute), and a PAL disc then asks 50 or
    // 60 Hz on a silent screen that waits for A: players took both for a game that doesn't start.
    menu.Toast("Dreamcast: the first start can take a minute (shaders). Cross: A, Options: Start.", 8.0f);
  }
  if (s_system == SystemId::PS1 && s_ps1_libcrypt)
  {
    // (00.81) A LibCrypt-protected disc (European games: Parasite Eve II, MediEvil, Spyro 3...) without its .sbi: it
    // started anyway (scripts/build-ps1-core.sh; it couldn't before, "the emulator couldn't load this ROM"), but the
    // protection's checks may stop it later. Redump's .sbi, named as the disc's file, goes beside it.
    LogLine("ps1: a LibCrypt-protected disc without its .sbi file: started anyway, the player told");
    menu.Toast("Protected disc (LibCrypt): put its .sbi file beside it, or the game may stop later.", 10.0f);
  }
  else if (s_system == SystemId::PS1 && s_ps1_cooked)
  {
    // (00.90) An ISO of 2048-byte sectors (a converter's "BIN to ISO"): the core makes its data sectors whole, but the
    // sound and film sectors (XA voices and music, STR videos) were cut by the converter: videos without voices
    // (Tombi), games that freeze at a video (Tekken 3's endings, Metal Gear Solid, Driver 2: GitHub #43).
    LogLine("ps1: a disc image of 2048-byte sectors (no sound or film sectors): the player told");
    menu.Toast("This ISO lacks the sound and video sectors: it may freeze. Use its .cue/.bin or .chd", 10.0f);
  }
  else if (s_system == SystemId::PS1 && s_ps1_openbios)
  {
    // (00.90) No PlayStation BIOS file: the core's built-in OpenBIOS starts the game, and fewer games start with it
    // (players' "PS1 games don't open": Alone in the Dark, "5 games and only 1"). Said once a session; the log every time.
    static bool told = false;
    LogF("ps1: no BIOS file in %s: the built-in OpenBIOS starts the game%s", s_system_dir.c_str(),
         told ? "" : ", the player told");
    // ((01.92, Lei on a PS5) The folder written out in full: the portable data's when it is in use - its BIOS are
    // read from there (Portable::SystemDir) -, else this PS4's.)
    const std::string bios_dir = Portable::Active() ? Portable::DataFolder() + "/system" : s_system_dir;
    if (!told)
      menu.Toast("No PS1 BIOS: fewer games start. Put one (scph5501.bin...) in " + bios_dir, 10.0f);
    told = true;
  }
  if (s_system == SystemId::THREEDS && ThreedsMiiGame(s_3ds_title))
  {
    // (01.29) Miitopia, Tomodachi Life without a real 3DS's Mii data: Miis without faces (#69 - the core's open-source
    // stand-in has no eyes, eyebrows or mouths). The file is Nintendo's, never in the package or on a server: the player
    // takes it from their own 3DS. Said once a game in a session, as the PS1 BIOS is; the log every time. (Its own
    // lines: the folder's path is too long for one; the texts translated here, the whole toast then untranslated.)
    const std::string nand = s_save_dir + "/Azahar/nand/00000000000000000000000000000000/";
    const std::string file = "title/0004009b/00010202/content/00000000.app";
    if (!ThreedsMiiDataThere(nand + "title/0004009b/00010202/content"))
    {
      static std::set<uint64_t> told;
      const bool tell = told.insert(s_3ds_title).second;
      LogF("3ds: no Mii data (a decrypted 0004009B00010202 from a 3DS) as %s%s: the Miis without faces%s", nand.c_str(),
           file.c_str(), tell ? ", the player told" : "");
      if (tell)
        menu.Toast(std::string(PS4Lang::T("Miis will have no faces: a 3DS system file (the Mii data) is missing.")) +
                       "\n" + PS4Lang::T("Take it from your own 3DS, decrypted, and save it as:") + "\n" + nand + "\n" +
                       file,
                   15.0f);
    }
  }
  if (IsFlycastBoard(s_system))
  {
    // An arcade board: coins and START, and where its TEST and SERVICE buttons are (the operator menu).
    // (No " = " in it: the language file's separator.)
    menu.Toast("Touchpad: Coin. Options: Start. TEST and SERVICE: Arcade Button in the L3+R3 menu.", 8.0f);
  }
  // A start again (the start watchdog, src/StartGuard.h): why, over the console's own toast.
  if (!StartGuard::RetryToast().empty())
    menu.Toast(StartGuard::RetryToast(), 8.0f);
  // (01.42) A 3DS game's texture pack left out for want of GPU memory (EnoughMemory).
  if (s_system == SystemId::THREEDS && s_3ds_textures_short >= 0)
    menu.Toast("Too little memory for its texture pack: the game started without it.", 8.0f);
  // (00.23) Online: the game loaded (Continue's state too): the host sends its state, a guest waits for it; the frames
  // run once both have it (Netplay::FramesThisPass).
  if (online)
    Netplay::GameLoaded(GetSystem(game.system).key, s_game_fps, NetplayApply);

  GameAudio audio;
  audio.Start(Settings::GetInt("audio_buffer", game.key));
  audio.SetVolume(Settings::GetInt("volume", game.key));
  s_audio = &audio;

  Pads pads;
  pads.Open();
  // (01.27) The players the game takes at once (NetplayLibrary: the cartridge consoles' and the Arcade's lists; 0: not
  // known - a disc's), or its adapter's for 3 or more (a PlayStation's Multitap disc, Auto or On): a game for 2 or more
  // that starts with one controller gets a hint (below), once a game while the app runs.
  int game_players = 0;
  {
    // (Only the consoles the list has, a Game Gear's link games left out: the list stays in memory, ~2 MB - never
    // loaded for a disc console's game.)
    static const char* const LISTED[] = {"nes", "snes", "md", "sms", "sg1000", "pce", "atari2600", "arcade"};
    const std::string console = GetSystem(game.system).key;
    uint32_t crc = 0;
    uint64_t size = 0;
    if (std::find(std::begin(LISTED), std::end(LISTED), console) != std::end(LISTED))
    {
      NetplayLibrary::Lookup(game.path, &crc, &size);
      game_players = NetplayLibrary::Players(console, game.id, crc);
    }
  }
  bool pads_said = false;
  static std::set<std::string> s_pads_hinted;
  // Tilt Sensor: DualShock 4 Motion - player 1's motion sensors on. (01.27) And 3DS Motion Sensor: DualShock 4.
  bool motion_on = false;
  auto update_motion = [&]() {
    const int handle = pads.pads[0].handle;
    const bool threeds = s_system == SystemId::THREEDS;
    bool vita = false, vita_want = false;
#ifdef EMUSUNI_VITA
    vita = s_system == SystemId::VITA;  // (01.88) the PS Vita's motion sensors (vita_motion)
    vita_want = s_motion_vita.on;       // ((01.91) here: s_motion_vita is with-vita's only - a build without it)
#endif
    const bool want = threeds ? s_motion3ds.on : vita ? vita_want : s_tilt.motion;
    if ((!IsHandheld(s_system) && !threeds && !vita) || handle < 0 || want == motion_on)
      return;
    const int result = emusuni_pad_set_motion(handle, want);
    motion_on = want;
    s_tilt.calibrate = true;
    LogF("%s: DualShock 4 motion sensors %s (%#x)", threeds ? "3ds" : vita ? "vita" : "gb", motion_on ? "on" : "off",
         static_cast<unsigned>(result));
  };
  update_motion();
  // The emulation thread first on its core while a game runs: a background thread on the same core
  // (ParaLLEl-RDP's shader compiles, the covers) waits instead of stopping the game. The PS4's
  // priorities: lower is higher, 700 the default. The thread never spins on another one (it blocks).
  int old_priority = 0;
  // (A Dreamcast game's with /data/EmuSuNi/dc-threads-free: left as it is, and on any core.)
  const bool priority_set = scePthreadGetprio(scePthreadSelf(), &old_priority) == 0 && !s_dc_threads_free &&
                            scePthreadSetprio(scePthreadSelf(), 480) == 0;
  LogF("game: emulation thread priority %d -> %s", old_priority, priority_set ? "480" : "unchanged");
  // N64 (CPU Cores: Fixed): the emulation thread on core 2 and ParaLLEl-RDP's threads on cores of
  // their own (port/ps4_n64.c): the threads that wait for each other every frame don't share one.
  uint64_t old_mask = 0;
  bool pinned = false;
  // The DS: its 3D thread on core 3; the PSP, the Dreamcast (and its arcade boards) and the 3DS: their other
  // threads off core 2 (port/ps4_n64.c). (00.89) The Arcade too: its drawing beside a heavy board's emulation (Killer
  // Instinct's MIPS at 100 MHz) is on cores 1, 4 and 5 (DrawWorker), the sound's thread off core 2 (GameAudio).
  // (00.99) The Jaguar the same way (00.98's log: its emulation on CPU 0, 2 and 3 in turn, 0 with the sound's thread).
  if ((fixed_cores || s_system == SystemId::NDS || s_system == SystemId::PSP || IsFlycast(s_system) ||
       s_system == SystemId::THREEDS || s_system == SystemId::SATURN || s_system == SystemId::ARCADE ||
       s_system == SystemId::JAGUAR) &&
      !s_dc_threads_free)
  {
    const int got = emusuni_get_affinity(scePthreadSelf(), &old_mask);
    const int set = got == 0 ? emusuni_set_affinity(scePthreadSelf(), 1ull << 2) : -1;
    pinned = got == 0 && set == 0;
    LogF("game: emulation thread on core 2: %s (cores before %#llx, %#x %#x); running on CPU %d",
         pinned ? "yes" : "no", static_cast<unsigned long long>(old_mask), static_cast<unsigned>(got),
         static_cast<unsigned>(set), sceKernelGetCurrentCpu());
  }
  // (01.06) The 3DS: the emulation thread's allocations from an arena of the heap of its own while the game runs (its
  // load's long-lived ones before, in the heap), given back when it ends - not under the one lock the core's pipeline,
  // shader and Vulkan threads (their arenas: port/ps4_n64.c) took with it. /data/EmuSuNi/3ds-heap-arenas-off: no.
  bool emulation_arena = false;
  if (s_system == SystemId::THREEDS)
  {
    if (FileExists(Portable::UserDir() + "/3ds-heap-arenas-off") || FileExists("/data/EmuSuNi/3ds-heap-arenas-off"))
      LogLine("game: the emulation thread with the one heap (3ds-heap-arenas-off)");
    else
    {
      emulation_arena = ps4_heap_thread_arena("3ds emulation") != 0;
      LogF("game: the emulation thread: %s",
           emulation_arena ? "an arena of the heap of its own" : "no arena left: the heap");
    }
  }
  Controls::Layout layout = Controls::LayoutFor(game.key, game.system);
  // A NAOMI or Atomiswave game with analog controls (a wheel, a flight stick, pedals) or a light gun: the left stick
  // is analog only (the wheel, the sight), not the D-pad too - Crazy Taxi's gears are Up and Down - unless the
  // player chose Left Stick for the D-Pad in Controls.
  const bool board_stick_analog =
      IsFlycastBoard(s_system) && s_board_set && (s_board_set->analog || s_board_set->gun);
  auto board_layout = [&]() {
    if (board_stick_analog && layout.dpad && layout.left_stick)
      layout.left_stick = false;
  };
  board_layout();
  if (board_stick_analog)
    LogF("naomi: the left stick is the game's analog control (%s), not the D-pad", s_board_set->gun ? "the gun's sight" :
                                                                                                     "a wheel, a stick or pedals");
  // The battery save written beside the emulation during play (SaveWriter, 00.72): its thread starts at the first
  // save the game makes; the game's end writes the last copy after it (SaveSram(true)), and nothing points at it after.
  SaveWriter save_writer;
  s_save_writer = &save_writer;
  struct ForgetSaveWriter
  {
    ~ForgetSaveWriter() { s_save_writer = nullptr; }
  } forget_save_writer;
  // The DS: its drawing beside its emulation (DrawWorker).
  DrawWorker drawer;
  // The PSP: only while its core is heavy (one frame more latency then: psp_overlap). (00.89) The Arcade the same way
  // (#26, #35: Killer Instinct at 59-65% speed, the drawing's 2-3 ms after each of its frames). (00.90) The Jaguar too
  // (Rayman: its core 13-17 ms a frame, the drawing 3.8 ms after each: 80-97% speed).
  const bool overlap_drawing = (s_system == SystemId::NDS || s_system == SystemId::PSP || IsFlycast(s_system) ||
                                s_system == SystemId::THREEDS || s_system == SystemId::SATURN ||
                                s_system == SystemId::ARCADE || s_system == SystemId::JAGUAR) &&
                               drawer.Start();
  if (s_system == SystemId::JAGUAR && overlap_drawing)
    LogF("pacing: the Jaguar's drawing always beside its emulation (a frame later)");
  bool psp_overlap = false;
  int arcade_overlap_said = 0;  // (the Arcade's changes between the two, in the log: the first ten; the PSP's too)
  double psp_long_share = 0.0;  // (01.05) the PSP's recent long frames (over 0.85 of a refresh): their share, ~20 frames'
  s_defer_frame = false;
  s_pending = {};

  Stats stats;
  stats.pass_ms.reserve(4096);
  stats.run_ms.reserve(4096);
  stats.pictures_at = s_pictures;  // (and pictures10_at: ResetWindow)
  ResetWindow(stats, display.GetFlipState());
  FramePacer pacer;
  VblankCheck vblank_check;
  const auto game_start = Clock::now();
  display.TakeWaitMs();  // the menu's waits aren't the game's
  auto last = Clock::now();
  auto next_pass = last;  // V-Sync off: when the next pass is due
  double schedule = 0.0;  // frames owed
  double clock_drift = 0.0;  // V-Sync off: the time the passes of about one refresh were off by (in refreshes)
  // The core's time per frame (a moving average), and whether it is too slow to be caught up.
  double core_avg = 0.0;
  // (01.06) The drawing's time per pass (a moving average): the 3DS's heavy mode counts it too.
  double draw_avg = 0.0;
  // (01.06) The 3DS with the frame handover: its Vulkan worker's time after the frame's end (a moving average), and the
  // last picture it counted (a pass that draws it again, with no frame of the core, counts nothing).
  double worker_avg = 0.0;
  uint64_t worker_serial = 0;
  bool heavy = false;
  bool threeds_pairs_said = false;  // (the 3DS: a light frame and the next in one pass, said once in the log)
  // (01.08, the 3DS's specialist) That pair with V-Sync on too (the pacing, below: from 00.95 on it never ran). The file
  // 3ds-pair-off: with V-Sync on, one frame a pass in the heavy mode, as 00.95 to 01.07.
  bool threeds_vsync_pairs = false;
  if (s_system == SystemId::THREEDS)
  {
    threeds_vsync_pairs =
        !FileExists(Portable::UserDir() + "/3ds-pair-off") && !FileExists("/data/EmuSuNi/3ds-pair-off");
    LogLine(threeds_vsync_pairs ? "pacing: the 3DS's heavy mode runs a light frame and the next in one pass with "
                                  "V-Sync on too" :
                                  "pacing: the 3DS's heavy mode runs one frame a pass with V-Sync on (3ds-pair-off)");
  }
#ifdef EMUSUNI_DC
  DcFrames dc_frames;  // (Flycast: the console's frames per picture)
#endif
  PspFrames psp_frames;  // (the PSP: its vblanks per run, Frame Skip on)
#ifdef EMUSUNI_VITA
  VitaCadence vita_cadence;  // ((01.77) the PS Vita: how long each of the game's pictures stays on the screen)
#endif
  int psp_heavy_passes = 0;  // (PSP Frame Rate Auto: passes in a row the core couldn't keep up)
  int skip_counter = 0;
  float sram_timer = 0.0f;
  bool slow_last = false;
  Result result = Result::Menu;
  bool quit = false;
  // ((01.57) The N64 Resolution the game started with: another one chosen in the in-game menu starts it again.)
  std::string n64_resolution = Settings::Get("n64_resolution", game.key);
  // ((01.66) The PS Vita's loading screen: the game's cover (the menu's, 640 pixels a side at most), its time since it
  // began - again after a load in the game, "Closing the title...".)
  ImTextureID vita_cover{};
  float vita_cover_aspect = 0.75f;
  auto vita_since = Clock::now();
  bool vita_was_loading = s_vita_loading;
  if (IsVita(s_system) && s_start_cover_game == game.key && !s_start_cover_path.empty())
  {
    ImageRGBA picture;
    if (LoadPicture(s_start_cover_path, &picture, 640, 640) && picture.width > 0 && picture.height > 0)
    {
      vita_cover = display.CreateTexture(picture.pixels.data(), picture.width, picture.height);
      vita_cover_aspect = static_cast<float>(picture.width) / static_cast<float>(picture.height);
    }
    LogF("vita: the loading screen's cover %s", vita_cover ? s_start_cover_path.c_str() : "couldn't be read");
  }
  s_start_cover_game.clear();
  s_start_cover_path.clear();
  uint32_t menu_pad_held = 0;
  // Back from the in-game menu, player 1's pad reaches the game once every button is up (the press that closed
  // the menu isn't the game's).
  Controls::MenuRelease menu_release;
  Controls::MenuOpener menu_opener;  // the in-game menu's buttons, a hold's time (00.68)
  float memory_log = 0.0f;
  float audio_log = 0.0f;  // play time since the sound's last health line
  uint32_t join_noticed = 0;  // (00.86: the last request to join our room said in the game)
  PassParts pass_parts;      // ((01.39) the pass before's, for a long one's line: LogLongPass)
  LongPasses long_passes;
  StartGuard::Step("the game runs");
  while (!quit)
  {
    StartGuard::Beat();  // (the start watchdog: the emulation thread goes on)
    const auto now = Clock::now();
    const float pass_s = std::chrono::duration<float>(now - last).count();  // ((01.39) the real time, uncut)
    const float dt = std::clamp(pass_s, 0.0f, 0.25f);
    last = now;
    if (!menu.IsOpen())
      LogLongPass(long_passes, pass_parts, pass_s * 1000.0, now);
    pass_parts = {};
    pass_parts.quiet = menu.IsOpen();
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

    pads.Read(dt);
    for (int i = 0; i < MAX_PLAYERS; i++)
      s_pad_handles[i] = pads.pads[i].handle;
    // (00.23) A controller that came or went while the game runs: whose it is (Pads).
    for (const auto& [player, connected] : pads.changes)
    {
      const std::string n = std::to_string(player + 1);
      menu.Toast("Controller " + n + (connected ? " connected: Player " : " disconnected: Player ") + n, 3.0f);
    }
    pads.changes.clear();
    // (01.27) A signed-in user's controller the PS4 didn't give the app (Pads: why, in the log).
    for (const int player : pads.failed)
      menu.Toast("Player " + std::to_string(player + 1) + "'s controller couldn't be opened", 4.0f);
    pads.failed.clear();
    // (01.27, v00.24's "player 2 not detected") 9 s in (after the start's own toasts, 8 s at most): the controllers there
    // in the log; a game for 2 or more players with player 1's alone - not online, not a Game Boy's - says how to add one
    // (the PS4 gives the app a controller once a user is chosen on it).
    if (!pads_said && pads.age >= 9.0f)
    {
      pads_said = true;
      std::string there;
      for (size_t i = 0; i < pads.pads.size(); i++)
      {
        if (pads.pads[i].ok)
          there += (there.empty() ? "" : ", ") + std::to_string(i + 1);
      }
      const int adapter_players =
          s_adapter != Multiplayer::Adapter::None ? Multiplayer::MaxPlayers(s_adapter) : 0;
      const int wanted = std::max(game_players, adapter_players);
      const bool alone = there == "1" || there.empty();
      const bool hint = wanted >= 2 && alone && !online && !IsHandheld(s_system) && !Tour::Active() &&
                        s_pads_hinted.insert(game.key).second;
      const std::string takes = wanted > 0 ? std::to_string(wanted) : "?";
      LogF("pads: 9 s in: the controllers of players %s; the game takes %s players%s",
           there.empty() ? "none" : there.c_str(), takes.c_str(), hint ? ": the hint for another controller shown" : "");
      if (hint)
        menu.Toast("One controller only: for Player 2, turn on another one and choose a PS4 user on it", 8.0f);
    }
    // (01.07) PPSSPP stopped the game (CoreLog: s_psp_stopped): said over it, again and again while the menu is
    // closed.
    if (s_psp_stopped && !menu.IsOpen())
    {
      static bool said = false;
      if (!said)
        LogLine("game: PPSSPP stopped the emulation (a bad memory access): the player is told to leave with the menu");
      said = true;
      menu.Toast("The game stopped because of an emulation error: leave it with the menu", 1.5f);
    }
    // (00.86) Our online room stays open while this game is played alone: its network every pass (the server drops a
    // console silent for 30 s), and a player asking to join is said - the in-game menu answers (Accept Player ends the
    // game and opens the room; the request waits Netplay::ASK_SECONDS).
    if (!online && Netplay::Hosting())
    {
      Netplay::Update();
      const Netplay::JoinRequest ask = Netplay::GetJoinRequest();
      if (ask.member >= 0 && ask.serial != join_noticed)
      {
        join_noticed = ask.serial;
        LogF("online: %s asks to join our room while we play alone", ask.nick.c_str());
        menu.Toast(ask.nick + " wants to join your room: open the menu to answer", 6.0f);
      }
    }
    // (01.23, the owner) Our online match under way, entry with permission: a player asking to join it is said - the
    // in-game menu's online page answers (Accept Player: it joins the match).
    if (online && Netplay::IsHost())
    {
      const Netplay::JoinRequest ask = Netplay::GetJoinRequest();
      if (ask.member >= 0 && ask.serial != join_noticed)
      {
        join_noticed = ask.serial;
        LogF("online: %s asks to join our match", ask.nick.c_str());
        menu.Toast(ask.nick + " wants to join the match: open the menu to answer", 6.0f);
      }
    }
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
    // The in-game menu's buttons (00.68: Controls -> In-Game Menu, for every game; L3 + R3 always open it too).
    const Controls::MenuButtons menu_buttons = Controls::MenuButtonsFor(Settings::Get("menu_buttons"), game.system);

    if (!menu.IsOpen())
    {
      if (menu_opener.Update(menu_buttons, p1_held, p1_pressed, dt))
      {
        // (R3's press made no swap of the DS's or the 3DS's screens: forgotten, so letting it go swaps none.)
        s_nds_r3 = s_3ds_r3 = false;
        s_l3 = {};  // ((01.31) nor a layout from L3 let go after it)
        StopRumble();
        // (00.85) The game's picture as the screen shows it, before the menu covers it: a state's thumbnail when the
        // core draws its own image.
        KeepMenuPicture(display, game.key);
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
        Touchpad::Forget(s_stylus);  // (no tap made of a finger lifted while the menu is open)
        s_jaguar_key = -1;           // (nor a Jaguar key held through it)
        audio.SetMuted(true);
        if (audio_log >= 1.0f)
          audio.LogHealth(audio_log);  // the play before the menu
        audio_log = 0.0f;
        SaveSram(false);
      }
    }
    else
    {
      // (00.90) With the sticks' directions (Controls::StickBits): Controls can put a console button on one.
      const IngameMenu::Action action =
          menu.Poll(p1_pressed, p1_held | (p1.ok && !touring ? Controls::StickBits(p1.data) : 0u), dt);
      Achievements::Paused();  // (rc_client's waiting work while the game is paused: retries, pings)
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
        // ((01.09) The 3DS makes its renderer again: its images forgotten - ThreedsRemaking.)
        if (s_system == SystemId::THREEDS)
          ThreedsRemaking("reset", [] {
            s_core->reset();
            return true;
          });
        else if (codes_after_start())
        {
          // ((01.36) The Mega Drive: its ROM as it was for the start's sum, the codes after it - cheats_wait.)
          s_core->cheat_reset();
          s_core->reset();
          cheats_wait = CHEATS_AFTER_START;
          LogF("cheats: reset - the Mega Drive's in again after %d frames", CHEATS_AFTER_START);
        }
        else
          s_core->reset();
        Achievements::Reset();  // (the achievements' progress from the start too)
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
      case IngameMenu::Action::ArcadeButton:
        // The board's TEST (RetroPad L3: its operator menu) or SERVICE (R3) for half a second of the game (the menu
        // closed itself): long enough for the board to see it, one press.
        s_board_press_id = menu.ArcadeTest() ? RETRO_DEVICE_ID_JOYPAD_L3 : RETRO_DEVICE_ID_JOYPAD_R3;
        s_board_press_until = Clock::now() + std::chrono::milliseconds(500);
        LogF("naomi: %s pressed (the in-game menu's Arcade Button)", menu.ArcadeTest() ? "TEST" : "SERVICE");
        break;
      default:
        break;
      }
      if (menu.TakeSettingsChanged())
      {
        // (00.23) Multiplayer Adapter: the adapter again, before the core's options (the PlayStation's Multitap and the
        // Saturn's adaptors are options; the others' are ports: PlugAdapter below).
        const Multiplayer::Adapter adapter_before = s_adapter;
        const Multiplayer::Decision adapter =
            Multiplayer::Decide(Settings::Get("multiplayer_adapter", game.key), s_mp_game);
        if (Multiplayer::HasAdapter(s_system))
          s_adapter = adapter.adapter;
        BuildVariables();
        update_motion();
        // The DS's and the 3DS's screens: their settings changed, the screens as the menu shows them (R3's swap off).
        if (TwoScreens() && ScreensSeen(game.key) != s_screens_seen)
        {
          s_screens_seen = ScreensSeen(game.key);
          s_r3_swap = false;
          LogScreens(game.key, "changed in the in-game menu");
        }
        LoadBezel(display, game);
        audio.SetVolume(Settings::GetInt("volume", game.key));
        if (Multiplayer::HasAdapter(s_system) && adapter.adapter != adapter_before)
          PlugAdapter(adapter, adapter_before, "Multiplayer Adapter changed in the in-game menu");
      }
      if (menu.TakeCheatsChanged() && cheats_wait == 0)  // ((01.36) waiting: they go in with the wait's end)
        apply_cheats();
      if (!menu.IsOpen())
      {
        audio.Clear();
        audio.SetMuted(false);
        audio.LogHealth(0.0f);  // a new window: the time in the menu isn't the game's
        audio_log = 0.0f;
        layout = Controls::LayoutFor(game.key, game.system);
        board_layout();
        s_tilt.calibrate = true;  // how the DualShock 4 is held now is "level"
        schedule = 0.0;
        clock_drift = 0.0;
        pacer.Restart();  // the time the menu was open isn't caught up
#ifdef EMUSUNI_DC
        dc_frames.Restart();
#endif
        psp_frames.Restart();
        ResetWindow(stats, display.GetFlipState());
        stats.counted = -1;
        menu_release.Start();  // (the game sees player 1's pad again once every button is up)
        menu_opener.Closed();  // (and a hold of the menu's buttons counts again once they were let go)
        // ((01.57, the owner) The N64 Resolution changed as the menu closes: the game starts again with it and goes on
        // from its state (SaveRestartState; main.cpp runs it again). Online, or the state not kept: the next start's.)
        const std::string resolution = Settings::Get("n64_resolution", game.key);
        if (s_system == SystemId::N64 && resolution != n64_resolution && !quit)
        {
          if (!online && SaveRestartState())
          {
            LogF("game: N64 Resolution %s -> %s: the game starts again from its state", n64_resolution.c_str(),
                 resolution.c_str());
            result = Result::Restart;
            quit = true;
          }
          else
          {
            menu.Toast(online ? "Online, the N64 Resolution changes at the next game start" :
                                "The N64 Resolution couldn't change now: next time a game starts",
                       3.0f);
            n64_resolution = resolution;  // (not asked again at every closing)
          }
        }
      }
    }
    if (quit)
      break;

    // ---- The drawing (below; the DS's beside the emulation) ----
    bool drawn = false;
    double pass_draw_ms = 0.0, pass_draw_wait = 0.0;
    double pass_worker_ms = -1.0;  // (01.06: the 3DS's handover - the worker's time for the picture drawn; -1: none)
    const bool vsync = Settings::Get("vsync", game.key) != "off";
    LoadBezel(display, game);  // (here, before the drawing: the bezel's window for the picture's shape now)
    auto draw_pass = [&](uint64_t total) {
      display.SetVsync(vsync);
      const auto draw_start = Clock::now();
      display.BeginFrame();
      ImGui::NewFrame();
      ImDrawList* dl = ImGui::GetBackgroundDrawList();
      if (s_vita_loading && IsVita(s_system))
      {
        // ((01.66) The PS Vita preparing the game: the app's screen instead of the core's picture.)
        if (!vita_was_loading)
          vita_since = Clock::now();
        DrawVitaLoading(dl, regular, bold, game.title, vita_cover, vita_cover_aspect,
                        std::chrono::duration<float>(Clock::now() - vita_since).count());
      }
      else
      {
        DrawPicture(dl, display, game.key);
        DrawBezel(dl);
      }
      vita_was_loading = s_vita_loading;
      ImDrawList* fg = ImGui::GetForegroundDrawList();
#ifdef EMUSUNI_VITA
      if (IsVita(s_system))
        DrawVitaCursor(fg, game.key);  // ((01.67) the pointer's or a finger's, over the picture, under the menu)
#endif
      DrawStats(fg, bold, stats, Settings::GetOn("show_fps", game.key),
                Settings::GetOn("perf_overlay", game.key), audio.Fill());
      if (online)
        NetplayUi::DrawOverlay(fg, regular, bold);  // (00.23: the ping, who speaks, what the match waits for)
      menu.Draw(fg, dt);
      AchievementsUi::Draw(fg, display, regular, bold, dt);  // (RetroAchievements' popups, 00.73)
      ImGui::Render();
      stats.flip_frame[static_cast<size_t>(display.NextFlip() % 8)] = total;
      // (01.06) The 3DS's picture sent by the core's Vulkan worker before this submission reads it (the handover):
      // counted as the drawing's waiting. A picture not sent in 2 s is drawn anyway, said once. The worker's time for
      // it after the frame's end goes to the pacing (below).
      double handover_ms = 0.0;
#ifdef EMUSUNI_3DS
      if (s_3ds_handover && s_frame_hw && s_hw_image_set && s_hw_image_serial != 0)
      {
        const auto handover_start = Clock::now();
        static bool handover_said = false;
        if (emusuni_3ds_wait_image(s_hw_image_serial, 2000) == 0)
        {
          if (s_hw_image_serial != worker_serial)
          {
            worker_serial = s_hw_image_serial;
            pass_worker_ms = emusuni_3ds_image_worker_ms(s_hw_image_serial);
          }
        }
        else if (!handover_said)
        {
          handover_said = true;
          LogF("3ds: picture %llu not sent by the core in 2 s: drawn anyway",
               static_cast<unsigned long long>(s_hw_image_serial));
        }
        handover_ms = std::chrono::duration<double, std::milli>(Clock::now() - handover_start).count();
      }
#endif
      display.EndFrame();
      double flip_wait = 0.0;
      const double display_wait = display.TakeWaitMs(&flip_wait);
      pass_draw_wait = display_wait + handover_ms;
      pass_draw_ms = std::chrono::duration<double, std::milli>(Clock::now() - draw_start).count();
      pass_parts.handover += handover_ms;  // ((01.39) LogLongPass)
      pass_parts.gpu += display_wait - flip_wait;
      pass_parts.flip += flip_wait;
      pass_parts.work += std::max(0.0, pass_draw_ms - pass_draw_wait);
      if (!tour.shot.empty())
        Tour::SaveShot(display, tour.shot);
    };

    // ---- Emulation ----
    const auto emu_start = Clock::now();
    // VideoOut now: the N64's pacing below, and the pacing line's count of the flips it showed.
    const Display::FlipState flip = display.GetFlipState();
    if (!menu.IsOpen())
      CountShownFlips(stats, flip.shown);
    // (00.23: online the match goes on while the in-game menu is open - this console's players neutral meanwhile.)
    if (!menu.IsOpen() || online)
    {
      // Buttons of every player (the in-game menu combo isn't passed on). Player 1's untouched while the
      // buttons held as the in-game menu closed are down (Controls::MenuRelease).
      Pads::Pad player1 = pads.pads[0];
      int hidden_passes = 0;
      player1.data = menu_release.Pad(player1.data, player1.ok && !touring, &hidden_passes);
      if (hidden_passes > 0)
        LogF("input: after the menu, player 1's pad kept from the game for %d passes, until every button was up",
             hidden_passes);
      bool fast = false;
      for (int i = 0; i < MAX_PLAYERS; i++)
      {
        // ((01.27) Its sticks as Controls' sensitivity and dead zone make them, for all that reads them below: the
        // core's analog values, the D-pad's Left Stick, the sticks' directions as buttons, the stylus, the tilt.)
        Pads::Pad pad = i == 0 ? player1 : pads.pads[i];
        Controls::TuneSticks(layout, &pad.data);
        // ((01.31) L3 on the DS and the 3DS: a short press the next Screen Layout, held the game's - L3Press. (01.67)
        // On the PS Vita: the pointer on or off.)
        if (i == 0 && (TwoScreens() || IsVita(s_system)) && !menu.IsOpen() &&
            TakeL3Press(&pad.data, pad.ok && !touring, dt))
        {
#ifdef EMUSUNI_VITA
          if (IsVita(s_system))
          {
            s_vita_pointer.on = !s_vita_pointer.on;
            s_vita_pointer.down = false;
            LogF("vita: the pointer %s (L3)", s_vita_pointer.on ? "on" : "off");
            menu.Toast(s_vita_pointer.on ? "Pointer on: the left stick moves it, Cross touches (L3: off)" :
                                           "Pointer off",
                       s_vita_pointer.on ? 4.0f : 2.0f);
          }
          else
#endif
            NextScreenLayout(game.key, menu);
        }
#ifdef EMUSUNI_VITA
        // ((01.67) The pointer on: the left stick moves the cursor - faster the further it is pushed, as fast up and
        // down as across the Vita's 960 x 544 -, Cross touches; both kept from the game.)
        if (i == 0 && IsVita(s_system))
        {
          const bool live = s_vita_pointer.on && pad.ok && !touring && !menu.IsOpen();
          s_vita_pointer.down = live && (pad.data.buttons & ORBIS_PAD_BUTTON_CROSS) != 0;
          if (live)
          {
            constexpr float DEAD = 0.12f, SPEED = 0.85f;  // (the whole screen's width in 1.2 s, pushed all the way)
            const auto speed = [](uint8_t value) {
              const float n = std::clamp((static_cast<float>(value) - 128.0f) / 127.0f, -1.0f, 1.0f);
              const float m = std::max(0.0f, (std::abs(n) - DEAD) / (1.0f - DEAD));
              return (n < 0 ? -1.0f : 1.0f) * m * m * SPEED;
            };
            s_vita_pointer.x = std::clamp(s_vita_pointer.x + speed(pad.data.leftStick.x) * dt, 0.0f, 1.0f);
            s_vita_pointer.y =
                std::clamp(s_vita_pointer.y + speed(pad.data.leftStick.y) * dt * (960.0f / 544.0f), 0.0f, 1.0f);
            pad.data.buttons &= ~static_cast<uint32_t>(ORBIS_PAD_BUTTON_CROSS);
            pad.data.leftStick.x = pad.data.leftStick.y = 128;
          }
          // (Once the game shows its first picture: how to get the pointer, said once.)
          if (!s_vita_loading && !s_vita_pointer.hinted && s_frame_hw)
          {
            s_vita_pointer.hinted = true;
            menu.Toast("Touchpad: slide to move the cursor, click to touch. L3: the left stick and Cross", 6.0f);
          }
        }
#endif
        s_buttons[i] = pad.ok && !touring ? Controls::PadButtons(layout, pad.data) : 0;
        if (s_system == SystemId::N64 || s_system == SystemId::PS1 || s_system == SystemId::PSP ||
            IsFlycast(s_system) || s_system == SystemId::ARCADE || s_system == SystemId::THREEDS ||
            IsVita(s_system))  // (with-vita: the PS Vita's two sticks)
        {
          // The analog sticks: the N64's control stick and C buttons, the DualShock's two sticks, the
          // PSP's and the Dreamcast's stick (the left one), an arcade game's wheel, paddle or dial
          // (FinalBurn Neo reads the sticks for them; a NAOMI's or Atomiswave's wheel, flight stick or gun
          // sight, Flycast), the 3DS's Circle Pad and C-stick.
          auto axis = [](uint8_t value) {
            return static_cast<int16_t>(std::clamp((static_cast<int>(value) - 128) * 258, -32768, 32767));
          };
          const bool on = pad.ok && !touring;
          s_analog[i][0] = {on ? axis(pad.data.leftStick.x) : int16_t(0), on ? axis(pad.data.leftStick.y) : int16_t(0)};
          if (i == 0)
            s_stick_max = std::max({s_stick_max, std::abs(static_cast<int>(s_analog[0][0][0])),
                                    std::abs(static_cast<int>(s_analog[0][0][1]))});
          s_analog[i][1] = {on ? axis(pad.data.rightStick.x) : int16_t(0), on ? axis(pad.data.rightStick.y) : int16_t(0)};
          if (s_system == SystemId::THREEDS)
          {
            for (auto& stick : s_analog[i])
              ApplyDeadZone(&stick, 0.12f);
          }
        }
        if (IsFlycast(s_system))
        {
          // The Dreamcast's analog triggers: the DualShock 4's L2 / R2 pressure, where its L and R
          // triggers are on L2 / R2 (else Flycast reads them pressed or not from the buttons). A NAOMI's
          // driving game's accelerator and brake the same (buttons 7 and 8); the Atomiswave has no button
          // there, its pedals are the triggers' pressure as they are.
          const bool on = pad.ok && !touring;
          auto trigger = [&](unsigned id) -> int16_t {
            uint32_t mask = layout.button_mask[id];
            if (mask == 0 && IsFlycastBoard(s_system))
              mask = id == RETRO_DEVICE_ID_JOYPAD_L2 ? ORBIS_PAD_BUTTON_L2 : ORBIS_PAD_BUTTON_R2;
            const int value = mask == ORBIS_PAD_BUTTON_L2 ? pad.data.analogButtons.l2 :
                              mask == ORBIS_PAD_BUTTON_R2 ? pad.data.analogButtons.r2 :
                                                            0;
            return static_cast<int16_t>(on ? value * 32767 / 255 : 0);
          };
          s_triggers[i] = {trigger(RETRO_DEVICE_ID_JOYPAD_L2), trigger(RETRO_DEVICE_ID_JOYPAD_R2)};
        }
        if (IsFlycastBoard(s_system))
        {
          // Gun Reload (a light gun game's; the core asks only in those), and the in-game menu's TEST or SERVICE
          // for player 1 while it lasts.
          s_gun_reload[i] = pad.ok && !touring && layout.reload && (pad.data.buttons & layout.reload);
          if (i == 0 && s_board_press_until > Clock::now())
            s_buttons[0] |= static_cast<uint16_t>(1u << s_board_press_id);
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
        if (i == 0 && s_system == SystemId::THREEDS && s_motion3ds.on)
        {
          // (01.27) 3DS Motion Sensor: ScePadData's acceleration (G) and angular velocity (rad/s) after its orientation
          // - OpenOrbis names them vel and acell - in the 3DS's axes (Motion3ds). None yet (all 0 until the sensors
          // answer) or no controller: the 3DS as it was, not turning.
          const vec_float3& a = pad.data.vel;
          const vec_float3& w = pad.data.acell;
          const bool fresh = pad.ok && !touring && (a.x != 0.0f || a.y != 0.0f || a.z != 0.0f);
          const float now[6] = {a.x, -a.y, a.z, -w.x, w.y, -w.z};
          for (int k = fresh ? 0 : 3; k < 6; k++)
            s_motion3ds.axes[k].store(fresh ? now[k] : 0.0f, std::memory_order_relaxed);
          if (fresh && !s_motion3ds.said)
          {
            s_motion3ds.said = true;
            LogF("3ds: motion: the DualShock 4 held now: %.2f %.2f %.2f G, %.2f %.2f %.2f rad/s", a.x, a.y, a.z, w.x,
                 w.y, w.z);
          }
        }
#ifdef EMUSUNI_VITA
        if (i == 0 && s_system == SystemId::VITA && s_motion_vita.on)
        {
          // (01.88) The PS Vita's motion sensors (MotionVita): ScePadData's acceleration (G) and angular velocity
          // (rad/s) - OpenOrbis names them vel and acell - as RetroArch gives them (m/s^2, rad/s), in the axes Vita3K
          // gives a controller (x, -z, y). None yet or no controller: the Vita as it was, not turning.
          const vec_float3& a = pad.data.vel;
          const vec_float3& w = pad.data.acell;
          const bool fresh = pad.ok && !touring && (a.x != 0.0f || a.y != 0.0f || a.z != 0.0f);
          constexpr float G = 9.80665f;
          const float now[6] = {a.x * G, -a.z * G, a.y * G, w.x, -w.z, w.y};
          for (int k = fresh ? 0 : 3; k < 6; k++)
            s_motion_vita.axes[k].store(fresh ? now[k] : 0.0f, std::memory_order_relaxed);
          if (fresh && !s_motion_vita.said)
          {
            s_motion_vita.said = true;
            LogF("vita: motion: the DualShock 4 held now: %.2f %.2f %.2f G, %.2f %.2f %.2f rad/s", a.x, a.y, a.z, w.x,
                 w.y, w.z);
          }
        }
#endif
        if (i == 0 && s_system == SystemId::JAGUAR)
        {
          // (00.90) The touchpad (1920x942) is the Jaguar's number pad, its twelve keys laid out as on the pad: three
          // across, four down (1 2 3 / 4 5 6 / 7 8 9 / * 0 #); the finger on one holds it down.
          const auto& touch = pad.data.touch;
          const bool finger = pad.ok && !touring && touch.fingers > 0;
          s_jaguar_key = -1;
          if (finger)
          {
            const int column = std::clamp(static_cast<int>(touch.touch[0].x * 3.0f / Touchpad::PAD_W), 0, 2);
            const int row = std::clamp(static_cast<int>(touch.touch[0].y * 4.0f / Touchpad::PAD_H), 0, 3);
            s_jaguar_key = row * 3 + column;
          }
        }
        if (i == 0 && s_system == SystemId::NDS)
        {
          // R3 (Swap Screens): the two screens trade places where the app places them (DrawScreens), not in the
          // core's picture - the same as on the 3DS (00.80).
          const bool r3 = (s_buttons[0] >> RETRO_DEVICE_ID_JOYPAD_R3) & 1;
          s_buttons[0] &= ~(1u << RETRO_DEVICE_ID_JOYPAD_R3);
          // (With the in-game menu on R3 held, 00.68: the swap is R3 let go before the menu's second.)
          const bool on_release = menu_buttons.mask == ORBIS_PAD_BUTTON_R3;
          s_r3_held = r3 && s_nds_r3 ? s_r3_held + dt : 0.0f;
          if (on_release ? !r3 && s_nds_r3 && s_r3_was < menu_buttons.hold : r3 && !s_nds_r3)
            s_r3_swap = !s_r3_swap.load();
          s_r3_was = s_r3_held;
          s_nds_r3 = r3;
          // The touchpad (1920x942) is the DS's touch screen (src/Touchpad.h): a trackpad - the finger moves the
          // stylus from where it is, a tap touches there, the Tap button (Controls) holds it down - or, with DS
          // Touch Screen: Direct, the screen itself. The right stick moves the stylus too (the DS has no use for
          // that stick); it stays where it was left. (01.15: the finger followed among the pad's touches.)
          const bool live = pad.ok && !touring;
          Touchpad::Touch touches[2];
          const int count = PadTouches(pad.data.touch, live, touches);
          const bool tap = live && layout.tap && (pad.data.buttons & layout.tap);
          const int taps_before = s_stylus.taps;
          const bool finger = Touchpad::Frame(s_stylus, touches, count, tap, 256.0f, 192.0f,
                                              Settings::Get("nds_touch", game.key) == "touch");
          if (s_stylus.taps != taps_before)
            s_touch_taps++;
          if (!finger && live)
            StickMovesStylus(pad.data, 256.0f, 192.0f);
          if (finger)
            s_touch_frames++;
          if (s_stylus.down)
            s_tap_frames++;
          s_pointer = {};
          if (s_frame_w > 0 && s_frame_h > 0)
          {
            const auto screen = NdsTouchScreen(s_frame_w, s_frame_h);
            const float fx = screen[0] + s_stylus.x / 255.0f * (screen[2] - 1.0f);
            const float fy = screen[1] + s_stylus.y / 191.0f * (screen[3] - 1.0f);
            s_pointer.x = static_cast<int16_t>(std::clamp(fx * 65534.0f / s_frame_w - 32767.0f, -32767.0f, 32767.0f));
            s_pointer.y = static_cast<int16_t>(std::clamp(fy * 65534.0f / s_frame_h - 32767.0f, -32767.0f, 32767.0f));
            s_pointer.pressed = s_stylus.down;
          }
        }
        if (i == 0 && s_system == SystemId::THREEDS)
        {
          // R3 (Swap Screens): the two screens trade places where the app places them (DrawScreens), as on the DS
          // (00.80; until 00.79 the core's swap option, its picture laid out again); the button isn't the game's.
          const bool r3 = (s_buttons[0] >> RETRO_DEVICE_ID_JOYPAD_R3) & 1;
          s_buttons[0] &= ~(1u << RETRO_DEVICE_ID_JOYPAD_R3);
          // (With the in-game menu on R3 held, 00.68: the swap is R3 let go before the menu's second.)
          const bool on_release = menu_buttons.mask == ORBIS_PAD_BUTTON_R3;
          s_r3_held = r3 && s_3ds_r3 ? s_r3_held + dt : 0.0f;
          if (on_release ? !r3 && s_3ds_r3 && s_r3_was < menu_buttons.hold : r3 && !s_3ds_r3)
            s_r3_swap = !s_r3_swap.load();
          s_r3_was = s_r3_held;
          s_3ds_r3 = r3;
          // The touchpad (1920x942) is the 3DS's touch screen (320x240), as the DS's (src/Touchpad.h): a trackpad -
          // the finger moves the stylus from where it is, a tap touches there, the Tap button (Controls) holds it
          // down - or, with 3DS Touch Screen: Direct, the screen itself; it stays where it was left. The core wants
          // it where the touch screen is in its picture (ThreedsLayout: always the same place since 00.80, whatever
          // the screens' settings and R3 show), over the whole picture; also with Top Screen Only, unseen. (01.15:
          // the finger followed among the pad's touches.)
          const bool live = pad.ok && !touring;
          Touchpad::Touch touches[2];
          const int count = PadTouches(pad.data.touch, live, touches);
          const bool tap = live && layout.tap && (pad.data.buttons & layout.tap);
          const int taps_before = s_stylus.taps;
          const bool finger = Touchpad::Frame(s_stylus, touches, count, tap, 320.0f, 240.0f,
                                              Settings::Get("3ds_touch", game.key) == "touch");
          if (s_stylus.taps != taps_before)
            s_touch_taps++;
          // (01.15) 3DS Right Stick: Stylus - the right stick moves the stylus as on the DS (the Tap button touches)
          // and the game gets no C-stick; C-Stick (the default) as until 01.14. (01.24, GitHub #74, #67, #57) D-Pad:
          // its directions press the 3DS's D-pad with the D-pad's own (StickDpad), no C-stick either; here, before
          // the match takes this pass's buttons online (NetplayLocalInputs), so the other consoles get the same.
          const std::string right_stick = Settings::Get("3ds_right_stick", game.key);
          bool stick = false;
          if (right_stick == "stylus")
          {
            s_analog[0][1] = {};
            stick = !finger && live && StickMovesStylus(pad.data, 320.0f, 240.0f);
          }
          else if (right_stick == "dpad")
          {
            s_analog[0][1] = {};
            const uint16_t dpad = live ? StickDpad(pad.data.rightStick.x, pad.data.rightStick.y) : 0;
            s_buttons[0] |= dpad;
            if (dpad != 0)
              s_stick_dpad_frames++;
          }
          if (finger)
            s_touch_frames++;
          if (s_stylus.down)
            s_tap_frames++;
          if (finger || stick || s_stylus.down)
            s_3ds_cursor = 90;
          s_pointer = {};
          const ThreedsPicture where = ThreedsLayout(s_3ds_scale);
          if (s_frame_w == where.w && s_frame_h == where.h)
          {
            // The middle of the stylus's pixel.
            const float fx = where.touch[0] + (s_stylus.x + 0.5f) / 320.0f * where.touch[2];
            const float fy = where.touch[1] + (s_stylus.y + 0.5f) / 240.0f * where.touch[3];
            s_3ds_at = {fx, fy};
            s_pointer.x = static_cast<int16_t>(std::clamp(fx * 65534.0f / s_frame_w - 32767.0f, -32767.0f, 32767.0f));
            s_pointer.y = static_cast<int16_t>(std::clamp(fy * 65534.0f / s_frame_h - 32767.0f, -32767.0f, 32767.0f));
            s_pointer.pressed = s_stylus.down;
          }
        }
#ifdef EMUSUNI_VITA
        // (After 01.47, the PS Vita's specialist) The touchpad is the PS Vita's touch screen, direct: where the finger
        // is on the pad is where it is on the screen, and it touches while it rests there. Uncharted: Golden Abyss's
        // title ("Touch to Start") and its menus take nothing else. With L2 held the core sends the touch to the
        // rear panel. The core's log says in how many refreshes there was a finger ("vita: gfx:").
        // ((01.69, the owner: «el pad de Vita debe ser trackpad») The touchpad as a laptop's trackpad over the Vita's
        // 960 x 544, the DS's and the 3DS's way (src/Touchpad.h): the finger moves the cursor from where it is without
        // touching; the touchpad's click - Controls' Tap the Screen - touches where it is, held while the finger moves a
        // drag; a short, still tap touches too. One cursor with L3's pointer (the left stick moves it, Cross touches).
        // Shown while the pointer is on, and 3 s after the touchpad or a touch.)
        if (i == 0 && IsVita(s_system))
        {
          const bool live = pad.ok && !touring;
          Touchpad::Touch touches[2];
          const int count = PadTouches(pad.data.touch, live, touches);
          const bool click = live && layout.tap && (pad.data.buttons & layout.tap);
          s_stylus.x = s_vita_pointer.x * 959.0f;  // (where L3's pointer left the cursor)
          s_stylus.y = s_vita_pointer.y * 543.0f;
          const bool finger = Touchpad::Frame(s_stylus, touches, count, click, 960.0f, 544.0f, false);
          s_vita_pointer.x = s_stylus.x / 959.0f;
          s_vita_pointer.y = s_stylus.y / 543.0f;
          s_vita_pointer.touching = s_stylus.down || (s_vita_pointer.on && s_vita_pointer.down);
          s_pointer.x = static_cast<int16_t>(s_vita_pointer.x * 65534.0f - 32767.0f);
          s_pointer.y = static_cast<int16_t>(s_vita_pointer.y * 65534.0f - 32767.0f);
          s_pointer.pressed = s_vita_pointer.touching;
          if (finger || s_vita_pointer.touching)
            s_vita_pointer.recent = 180;
          else if (s_vita_pointer.recent > 0)
            s_vita_pointer.recent--;
          s_vita_pointer.shown = s_vita_pointer.on || s_vita_pointer.recent > 0;
        }
#endif
      }
      // (00.23) Online: this pass's local players to the match (src/Netplay.h); no fast forward; RetroAchievements
      // paused (a rollback's frames run again, a prediction's would count).
      if (online)
      {
        fast = false;
        NetplayLocalInputs(pads, menu.IsOpen());
        Achievements::Paused();
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
      // (00.23) Online: the game's own speed on both consoles (no Emulation Speed, no FPS limit of one's own).
      if (online)
        ratio = std::abs(s_game_fps / hz - 1.0) < 0.012 ? 1.0 : s_game_fps / hz;
#ifdef EMUSUNI_DC
      // Flycast keeps the console to this pace while no picture holds it back (00.69: a loading screen or a board's
      // BIOS ran the emulation as fast as the PS4 could, its sound too fast for the app): the frames a second these
      // passes ask for, Speed and Fast Forward included; no limit with the FPS limit off.
      if (IsFlycast(s_system))
        flycast_emusuni_pace(limit == "0" ? 0.0 : ratio * hz);
#endif
      // N64 with V-Sync (N64 Frame Pacing: Even): the frames due by the vblank this pass's flip
      // will be shown at - VideoOut's count + the flips queued + 1 (src/Pacing.h). By the clock
      // (By the Clock, V-Sync off, fast forward, a vblank count that can't be trusted): as 00.27,
      // the time the last pass took (0.5 to 2 refreshes: the 0.5 credits a short pass with time it
      // didn't take - 104-108% in Mario Kart 64 - but taking it out alone makes the hitches worse in
      // the model, so the clock's way stays as it was, for comparison).
      int frames = 0;
      int threeds_owed = 0;  // (the 3DS's heavy mode: the frames owed before its one frame a pass)
      bool vsync_paced = false;  // ((01.08) a frame a pass: the V-Sync on way below)
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
        {
          // By the clock: the passes don't come with the TV's refreshes. A pass of about one refresh counts as
          // exactly one, and what it was off by is kept and given back a whole frame at a time: the wake-up of
          // each pass's sleep moves by a fraction of a millisecond, and with the frames owed near a half (where
          // the heavy mode leaves them) that alone ran 0, 2, 0, 2 frames a pass - the 3DS at 100% speed with
          // "190 the same frame again, 190 jumped 2+ frames" in 600 refreshes (00.62).
          double refreshes = std::clamp(static_cast<double>(dt) * hz, 0.0, 4.0);
          if (std::abs(refreshes - 1.0) < 0.25)
          {
            clock_drift += refreshes - 1.0;
            refreshes = 1.0;
            if (std::abs(clock_drift) >= 1.0)
            {
              const double whole = clock_drift > 0.0 ? 1.0 : -1.0;
              refreshes += whole;
              clock_drift -= whole;
            }
          }
          schedule += ratio * refreshes;
        }
        else
        {
          schedule += ratio;
          vsync_paced = true;
        }
        // The frames owed, rounded to the nearest: the backlog stays within half a frame either way.
        // (Rounded down, passes of 16.6-16.8 ms against the TV's 16.68 ran 0, 2, 0, 2 frames once the
        // owed fraction sat near 0: God of War at 100% speed with a third of its pictures shown twice.)
        frames = static_cast<int>(std::floor(schedule + 0.5));
        schedule -= frames;
        frames = std::clamp(frames, 0, 10);
        // A core that takes about a refresh per frame or more (God of War: Ghost of Sparta's fights,
        // 17-18 ms; Mario Kart DS's races) can't be caught up: each pass ran more frames and took
        // longer still (4 frames a pass, 72 ms passes: 16 pictures a second at 91% speed). One frame
        // a pass then, every one of them shown; the game runs that much slower, its sound at its pitch.
        const double refresh_ms = 1000.0 / std::max(hz, 1.0);
        const bool was_heavy = heavy;
        // (01.06) The 3DS: the core and the drawing after it, which the heavy mode puts beside it (its only overlap).
        // Castlevania: Mirror of Fate's calm parts at 4x took 11.8 ms of core and 8.6 of drawing, a pass of 20.4 ms
        // after the emulation: 81% speed with the core under the 0.92 of a refresh that alone turned it on.
        // With the frame handover the core's frame no longer waits for its Vulkan worker at the end: that time
        // (worker_avg) counts as the core's, as when it was in the frame - else a pass that no longer fits a refresh
        // (the drawing waits for the worker instead) would stay out of the heavy mode, and the heavy mode, where the
        // drawing beside seldom waits, would end too soon.
        const double threeds_core = core_avg + worker_avg;
        const double load = s_system == SystemId::THREEDS ? threeds_core + draw_avg : core_avg;
        if (!heavy && load > refresh_ms * 0.92)
          heavy = true;
        else if (heavy && load < refresh_ms * 0.85)
          heavy = false;
        if (heavy != was_heavy)
        {
          if (s_system == SystemId::THREEDS)
          {
            LogF("pacing: the core takes %.1f ms a frame and the drawing %.1f: %s", threeds_core, draw_avg,
                 heavy ? "one frame a pass, the drawing beside the emulation" : "caught up again");
          }
          else
          {
            LogF("pacing: the core takes %.1f ms a frame: %s", core_avg,
                 s_system == SystemId::SATURN ? (heavy ? "longer than a refresh, the game a little slower (one frame a "
                                                         "pass, as always for the Saturn)" :
                                                         "shorter than a refresh again") :
                 heavy                        ? "one frame a pass, the game a little slower" :
                                                "caught up again");
          }
        }
        // PSP Frame Rate Auto: about 2 s in a row too slow for 60 (not a loading screen's long frames,
        // nor the first seconds' pipeline compiles): God of War at 30 FPS for the rest of the game. (01.05: only a God of
        // War game - the hack acts on no other; 00.23's log said it for Midnight Club: L.A. Remix.)
        if (s_system == SystemId::PSP && s_psp_gow && !s_psp_30fps_auto &&
            Settings::Get("psp_frame_rate", game.key) == "auto")
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
          threeds_owed = s_system == SystemId::THREEDS ? frames : 0;
          // (01.08, the 3DS's specialist) With V-Sync on each pass owed exactly one frame (schedule += ratio above), so
          // 00.70's light frame and the next in one pass (below) never ran from 00.95 on: Captain Toad's frames of ~3
          // and ~37 ms took 54 ms for two (a light one waiting a whole refresh by itself), 40 together. The 3DS's
          // frames owed then by the refreshes the last pass took (1 to 2), as the clock's way counts them. Only the
          // pair's choice: the schedule stays as it was (a pass with the heavy mode just over runs no catch-up frame),
          // and a heavy first frame is one frame a pass as before.
          if (s_system == SystemId::THREEDS && vsync_paced && threeds_vsync_pairs)
          {
            const double refreshes = std::clamp(static_cast<double>(dt) * hz, 1.0, 2.0);
            threeds_owed = std::max(threeds_owed, static_cast<int>(std::floor(ratio * refreshes + 0.5)));
          }
          frames = std::min(frames, 1);
          schedule = std::min(schedule, 0.5);
          // (The sound keeps its pitch: GameAudio stretches what the slower game sends.)
        }
        // The Saturn (00.69): one frame a pass, always. Its core takes most of a refresh on the PS4 (00.68's log: Sonic
        // R's race 15-21 ms of 16.7), so the heavy mode came and went every few seconds ("one frame a pass" / "caught
        // up again" in turn), and each catch-up ran 2 or 3 frames in one pass: 35-56 ms without a new picture, its
        // stutter. Now a frame longer than a refresh makes the game that much slower (the sound keeps its pitch) and
        // is never made up for in a burst. (Fast Forward and Frame Rate Limit off as before.)
        if (s_system == SystemId::SATURN && !forward && limit != "0")
        {
          frames = std::min(frames, 1);
          schedule = std::min(schedule, 0.5);
        }
      }
#ifdef EMUSUNI_DC
      // (Flycast: the frames its pictures covered ahead of the pacing are paid here: DcFrames.)
      if (IsFlycast(s_system) && !paced)
        frames = dc_frames.Pay(frames);
#endif
      // (The PSP with Frame Skip: the vblanks its runs covered ahead of the pacing: PspFrames.)
      if (s_system == SystemId::PSP && !paced)
        frames = psp_frames.Pay(frames);
      // (00.23) Online: the frames the other console's inputs allow (none while waiting for it; the network first).
      if (online)
        frames = Netplay::FramesThisPass(frames);
      // (00.95) A link cable match's guest, before its first frame: its own save must be Game Boy 2's in the host's
      // state (else the match ends, nothing written: s_gblink_failed). The host loaded both itself.
      if (s_gblink && frames > 0 && !s_gblink_checked && !s_gblink_failed)
      {
        if (GbLink::Get().local == 0 || CheckGbLinkSave())
        {
          s_gblink_checked = true;
        }
        else
        {
          s_gblink_failed = true;
          frames = 0;
          LogLine("gblink: our save in the host's state isn't the one on this PS4: the match ends, nothing written");
        }
      }
      stats.per_pass[static_cast<size_t>(std::min(frames, 3))]++;
      audio.SetMuted(fast && ff != "off");
      // (The PSP's and the Dreamcast's Frame Skip are their cores' own: BuildPspVariables, BuildDcVariables -
      // the NAOMI's and the Atomiswave's too; the Arcade's 1 and 2 too, BuildArcadeVariables, its Auto the app's.)
      // (The Saturn: none - YabaSanshiro draws every frame on its renderer's threads.)
      std::string skip = s_system == SystemId::PSP || IsFlycast(s_system) || s_system == SystemId::SATURN ?
                             std::string("off") :
                             Settings::Get("frameskip", game.key);
      if (s_system == SystemId::ARCADE && skip != "auto")
        skip = "off";
      // The DS: the last picture drawn while these frames run; the PSP, Flycast's machines, the Saturn, the Arcade and
      // the Jaguar only while their core is heavy.
      if (s_system == SystemId::PSP || IsFlycast(s_system) || s_system == SystemId::SATURN ||
          s_system == SystemId::ARCADE || s_system == SystemId::JAGUAR)
      {
        const double refresh_ms = 1000.0 / std::max(display.RefreshHz(), 1.0);
        const bool was_overlap = psp_overlap;
        // (01.05) The PSP also when more than 30% of its recent frames are long - over 0.85 of a refresh, as the
        // average's mark: with the drawing after it (~1.9 ms) a 14.8 ms frame's pass already takes two -, though their
        // average isn't: a game at 30 FPS alternates its frame and a light one (Midnight Club: L.A. Remix with
        // compat.ini's EmuSuNiMax30FPS, scripts/build-psp-core.sh: ~25 and ~1 ms, an average of 12-14 under the 14.2
        // above), and after the emulation the long one's pass took two refreshes and the light one's a third: 67% speed.
        // Beside the drawing both fit in two. Off only with the average low and under 10% of them long. (A lone long
        // frame - a loading's, a compile's - is 5% of the share: MotorStorm's frames are long only now and then, as
        // before. God of War at 30 FPS the same way as Midnight Club once its frames are long.)
        const bool psp = s_system == SystemId::PSP;
        if (!psp_overlap && (core_avg > refresh_ms * 0.85 || (psp && psp_long_share > 0.3)))
          psp_overlap = true;
        else if (psp_overlap && core_avg < refresh_ms * 0.75 && (!psp || psp_long_share < 0.1))
          psp_overlap = false;
        if ((s_system == SystemId::ARCADE || psp) && psp_overlap != was_overlap && arcade_overlap_said < 10)
        {
          arcade_overlap_said++;
          LogF("pacing: the %s takes %.1f ms a frame%s: the drawing %s",
               s_system == SystemId::ARCADE ? "board" : "core", core_avg,
               psp ? (" (" + std::to_string(static_cast<int>(psp_long_share * 100.0 + 0.5)) +
                      "% of its recent frames long)").c_str() : "",
               psp_overlap ? "beside the emulation (a frame later)" : "after the emulation again");
        }
        // (The Saturn, 00.69: always - each change between the two showed a picture twice or jumped one, and its core
        // is near a refresh most of the time. (00.99) The Jaguar too: its core's average at those thresholds, Alien vs
        // Predator's and Iron Soldier's 12-16 ms, changed between the two every few seconds in 00.98's log.)
        if (s_system == SystemId::SATURN || s_system == SystemId::JAGUAR)
          psp_overlap = true;
      }
      // (The 3DS: only in the heavy mode, one frame a pass: the core renders into a ring of 3 images and must
      // never reach the one being drawn.)
      // (The Saturn: one frame a pass only - YabaSanshiro's renderer keeps three pictures, the one drawn here among
      // them, and makes one each frame: more runs than one beside the drawing would reach it.)
      // (Online - the Arcade's -: never; the match's rollbacks run the core again and its overlay is drawn from
      // what they change.)
      const bool overlap = overlap_drawing && frames > 0 && !touring && !online &&
                           (s_system == SystemId::NDS || (s_system == SystemId::THREEDS ? heavy && !forward : psp_overlap)) &&
                           (s_system != SystemId::SATURN || frames == 1);
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
        // (00.23: online, the frame with every port's input of the match, after any rollback - Netplay::RunFrame.)
        if (online)
          Netplay::RunFrame();
        else
          s_core->run();
        const float run_ms = std::chrono::duration<float, std::milli>(Clock::now() - run_start).count();
        // RetroAchievements (00.73): every frame the core ran (fast forward's and skipped pictures' too), the game's
        // achievements checked against its memory - nothing when the game has none. (00.23: not online.)
        if (!online)
          Achievements::DoFrame();
        // ((01.36) The Mega Drive's codes after its start: cheats_wait.)
        if (cheats_wait > 0 && --cheats_wait == 0)
        {
          apply_cheats();
          LogF("cheats: the Mega Drive's in now");
        }
        // The 3DS (00.70): a game drawing at 30 FPS alternates a heavy frame (its picture) and a light one, and once
        // the heavy ones pass a refresh the heavy mode ran one frame a pass, a light frame waiting a whole refresh by
        // itself (00.69's log, Hey! Pikmin's busy scenes: frames of 34 and 5 ms, 51 ms for two, 62% speed). A light
        // first frame (under half a refresh) lets the pass run the next one too when the pacing owes it: two images
        // of the core's ring of three, never the one drawn beside.
        if (threeds_owed >= 2 && f == 0 && frames == 1 && run_ms < 500.0f / std::max(static_cast<float>(hz), 1.0f))
        {
          frames = 2;
          stats.per_pass[1]--;
          stats.per_pass[2]++;
          if (!threeds_pairs_said)
          {
            threeds_pairs_said = true;
            LogF("pacing: a light 3DS frame (%.1f ms) and the next in one pass while the core is heavy (a game "
                 "drawing at 30 FPS alternates heavy and light frames)", run_ms);
          }
        }
        // The console's frames this run covered: 1, but Flycast's picture can be 2 to 4 (DcFrames). The pacing pays
        // for them - the runs still planned for this pass first, then runs left out of the passes that follow - and
        // the speed and the core's time per frame count them (Jet Set Radio at 30 FPS: one run every other refresh).
        int covered = 1;
#ifdef EMUSUNI_DC
        if (IsFlycast(s_system))
          covered = dc_frames.Take();
#endif
        // (The PSP: 1, but with Frame Skip on a run lasts until its next picture, 2 to 10 vblanks: PspFrames.)
        if (s_system == SystemId::PSP)
          covered = psp_frames.Take();
#ifdef EMUSUNI_VITA
        if (IsVita(s_system))
          vita_cadence.Run();
#endif
        stats.core_ms += run_ms;
        stats.core_runs += covered;
        stats.core_max = std::max(stats.core_max, run_ms);
        pass_parts.core += run_ms;  // ((01.39) LogLongPass)
        pass_parts.core_most = std::max(pass_parts.core_most, static_cast<double>(run_ms));
        pass_parts.runs++;
        const double frame_ms = run_ms / covered;
        core_avg = core_avg <= 0.0 ? frame_ms : core_avg * 0.9 + frame_ms * 0.1;
        if (s_system == SystemId::PSP)
          psp_long_share = psp_long_share * 0.95 + (frame_ms > 850.0 / std::max(hz, 1.0) ? 0.05 : 0.0);
        stats.emulated += covered;
        stats.emulated10 += covered;
        stats.total++;
        stats.run_ms.push_back(run_ms);
        if (covered > 1)
        {
          const int planned = std::min(covered - 1, frames - 1 - f);
          f += planned;
          if (s_system == SystemId::PSP)
            psp_frames.Owe(covered - 1 - planned);
#ifdef EMUSUNI_DC
          else
            dc_frames.Owe(covered - 1 - planned);
#endif
        }
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
        const auto beside_start = Clock::now();
        drawer.Wait();
        pass_parts.beside = true;  // ((01.39) LogLongPass)
        pass_parts.beside_wait = std::chrono::duration<double, std::milli>(Clock::now() - beside_start).count();
        s_defer_frame = false;
        TakePendingFrame();
        drawn = true;
      }
      if (disc_due)
        insert_disc();
      if (resume_pending && !s_shutdown)
      {
        resume_pending = false;
        StartGuard::Step(StartGuard::CONTINUE_STEP);
        if (LoadState(resume_slot))  // ((01.27) Triangle -> Load State's slot, else Continue's)
          menu.Toast(resume_toast);
        StartGuard::Step("the game runs");
      }
      if (s_shutdown)
      {
        // The game couldn't start (PPSSPP): the menu says why.
        *error = s_message.empty() ? "The emulator couldn't start this game" : s_message;
        if (s_system == SystemId::THREEDS)
          *error = ThreedsFailure(*error);
#ifdef EMUSUNI_VITA
        // (01.30) The PS Vita's firmware, there as .PUP files, couldn't be installed (the core's one sentence, from
        // scripts/build-vita-core.sh): the menu's two lines, in its languages.
        if (s_system == SystemId::VITA && error->rfind("The PS Vita's firmware couldn't be installed", 0) == 0)
          *error = "The PS Vita's firmware couldn't be installed.\nCheck that the PS4 has 1 GB free and try again.";
        // (The core's last word was a step of its progress - "Preparing the title..." -, no reason: the menu's own.)
        else if (s_system == SystemId::VITA && error->size() > 3 && error->compare(error->size() - 3, 3, "...") == 0)
          *error = "The emulator couldn't start this game";
#endif
        s_message.clear();
        result = Result::Failed;
        quit = true;
        break;
      }
      // (00.23) Online: the match ended - the other left, the connection was lost -: the game ends, the menu says why.
      std::string netplay_ended;
      if (s_gblink_failed)
      {
        // (00.95: a guest's own Game Boy save didn't come as it is here - CheckGbLinkSave.)
        *error = "Your Game Boy save didn't arrive as it is on this PS4.\nThe match ended without saving.";
        if (online)
          Netplay::Leave();
        result = Result::Failed;
        quit = true;
        break;
      }
      if (online && Netplay::Ended(&netplay_ended))
      {
        // (A Choose Together match the other ended: no error - back to the room.)
        *error = netplay_ended;
        result = netplay_ended.empty() ? Result::Menu : Result::Failed;
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
      // (01.20) A 3DS game whose picture stays black at its start is no longer reset by itself (01.16 to 01.19: at
      // 10 s, twice at most a load; the owner's decision). The core's 3ds-black-watch still writes the log's lines.
      // The core's messages (the Famicom Disk System's disk side, a missing BIOS). ((01.78) Two lines - the 3DS
      // game's save folder not made, and what to do - 6 s, to be read.)
      if (!s_message.empty())
      {
        menu.Toast(s_message, s_message.find('\n') != std::string::npos ? 6.0f : 3.0f);
        s_message.clear();
      }
      // (00.87) The player's data on a USB drive (src/Portable.h): taken out, its next save went to this PS4 - said.
      if (const std::string warning = Portable::TakeWarning(); !warning.empty())
      {
        menu.Toast(warning, 6.0f);
        Saves::Init(Portable::UserDir());  // (the fallback folder's)
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
      // ((01.72) And the core's pictures: what the counter says as FPS.)
      stats.pictures_fps = static_cast<float>(static_cast<double>(s_pictures - stats.pictures_at) / stats.fps_time);
      stats.pictures_at = s_pictures;
      // ((01.78) The N64: the game's own pictures (its VI origin's changes), not ParaLLEl-RDP's image a VI.)
      if (s_system == SystemId::N64)
      {
        const unsigned long long total = emusuni_n64_pictures_total();
        if (total != N64_PICTURES_UNKNOWN && stats.n64_at != N64_PICTURES_UNKNOWN && total >= stats.n64_at)
          stats.pictures_fps = static_cast<float>(static_cast<double>(total - stats.n64_at) / stats.fps_time);
        stats.n64_at = total;
      }
#ifdef EMUSUNI_VITA
      // ((01.72, the owner: «siempre dice 60 y el juego anda a tirones») The PS Vita's core runs apart from the TV's
      // refreshes, 60 a second whatever the game shows: the counter says the game's own pictures over this second, from
      // its core's count (01.77: once a second, as above).
      if (IsVita(s_system))
      {
        const unsigned long long total = emusuni_vita_pictures_total();
        if (total != VITA_PICTURES_UNKNOWN && stats.vita_at != VITA_PICTURES_UNKNOWN && total >= stats.vita_at)
          stats.game_fps = static_cast<float>(static_cast<double>(total - stats.vita_at) / stats.fps_time);
        stats.vita_at = total;
      }
#endif
      stats.frames = stats.emulated = 0;
      stats.fps_time = 0.0f;
      // The N64's speed in the log every 10 seconds of play (how well the PS4 keeps up).
      // (Also for SNES/NES games drawn through a GPU filter: how busy the filter keeps the GPU.)
      // (The Saturn too, 00.69: 00.68's log had its speed only while a GPU filter was on.)
      if ((s_system == SystemId::N64 || s_system == SystemId::PS1 || s_system == SystemId::PSP || IsFlycast(s_system) ||
           s_system == SystemId::NDS || s_system == SystemId::ARCADE || s_system == SystemId::THREEDS ||
           s_system == SystemId::SATURN || IsVita(s_system) || GpuMode(game.key) != GpuFilter::Mode::Off) &&
          !menu.IsOpen() &&
          stats.time10 >= 10.0f)
      {
        // The core's time per frame against the game's frame (20 ms for PAL): above it, the PS4's
        // CPU (or, with Accurate sync, the GPU) can't keep up.
        // The GPU: busy since the last line, and the memory its buffers take.
        static uint64_t last_busy = 0, last_span = 0, last_completed = 0;
        uint64_t busy = 0, span = 0, submitted = 0, completed = 0, bo_bytes = 0, bo_count = 0;
        ac_ps4_get_gpu_busy(&busy, &span);
        ac_ps4_get_stats(&submitted, &completed, &bo_bytes, &bo_count);
        // (01.05) The driver's count (DolphinPS4's port/mesa/ac_ps4_drm.c, ac_ps4_get_gpu_busy: the union of each
        // submission's GPU clock from its start to its end) can stand still while the GPU still finishes work: Midnight
        // Club: L.A. Remix's "GPU busy 0%" from its second line on (00.23), PPSSPP's own timestamps 14-18 ms of each 30
        // ms frame. Then "?" and the count's numbers, not a 0.
        const bool counted = span > last_span && busy >= last_busy;
        char gpu_busy[96];
        if (counted)
          snprintf(gpu_busy, sizeof(gpu_busy), "%.0f%%",
                   100.0 * static_cast<double>(busy - last_busy) / static_cast<double>(span - last_span));
        else if (completed > last_completed)
          snprintf(gpu_busy, sizeof(gpu_busy), "? (the driver's count stood still: busy %llu, span %llu ticks)",
                   static_cast<unsigned long long>(busy), static_cast<unsigned long long>(span));
        else
          snprintf(gpu_busy, sizeof(gpu_busy), "0%%");
        last_busy = busy;
        last_span = span;
        last_completed = completed;
        // (And the regular flexible memory free: the trend up to any close of the app.)
        // ((01.72) With the core's pictures a second: the counter's FPS.)
        LogF("speed: %.1f FPS shown (the core's pictures %.1f), %.0f%% of the game's speed; the core %.1f ms per frame "
             "(most %.1f) of %.1f; drawing %.1f ms (%.1f of it waiting); CPU %d; GPU busy %s, %llu MiB, flexible %d MiB "
             "free",
             stats.time10 > 0.0f ? stats.frames10 / stats.time10 : 0.0f,
             stats.time10 > 0.0f ? static_cast<double>(s_pictures - stats.pictures10_at) / stats.time10 : 0.0,
             stats.time10 > 0.0f ? stats.emulated10 / stats.time10 / s_game_fps * 100.0 : 0.0,
             stats.core_runs ? stats.core_ms / stats.core_runs : 0.0, stats.core_max, 1000.0 / s_game_fps,
             stats.draws ? stats.draw_ms / stats.draws : 0.0, stats.draws ? stats.draw_wait_ms / stats.draws : 0.0,
             sceKernelGetCurrentCpu(), gpu_busy, static_cast<unsigned long long>(bo_bytes >> 20), FlexibleFreeMiB());
        // ((01.86) The GPU's "shared queue") The submissions to the GPU and the queue lock over these 10 s (the
        // switch gpu-submitthread-on: Mesa's PS4 winsys makes its kernel calls on a thread of its own).
        display.LogGpuSubmissions();
        if (s_system == SystemId::PSP || s_system == SystemId::PS1 || s_system == SystemId::N64 ||
            IsFlycast(s_system) || s_system == SystemId::THREEDS)
        {
          LogF("input: left stick pushed up to %d%% in these 10 s", s_stick_max * 100 / 32767);
          s_stick_max = 0;
        }
        if (s_system == SystemId::NDS)
        {
          // Whether the touchpad reaches the game at all (a player couldn't start a touch-screen game).
          LogF("input: touchpad (%s): %d frames with a finger, %d with the stylus down, %d taps in these 10 s; stylus "
               "at %.0f,%.0f%s",
               Settings::Get("nds_touch", game.key) == "touch" ? "direct" : "trackpad", s_touch_frames, s_tap_frames,
               s_touch_taps, s_stylus.x, s_stylus.y, RestingTouchNote().c_str());
          s_touch_frames = s_tap_frames = s_touch_taps = 0;
        }
        if (s_system == SystemId::THREEDS)
        {
          // The picture the core gives against the one the touch screen's place was worked out for, and whether
          // the touchpad reaches the game at all.
          const std::string screens = Settings::Get("3ds_layout", game.key);
          const ThreedsPicture where = ThreedsLayout(s_3ds_scale);
          // (01.24) 3DS Right Stick: D-Pad, and how many frames its directions pressed the D-pad.
          const std::string right_stick = Settings::Get("3ds_right_stick", game.key);
          char dpad_note[80] = "";
          if (right_stick == "dpad")
            std::snprintf(dpad_note, sizeof(dpad_note), "; the right stick pressed the D-pad in %d frames",
                          s_stick_dpad_frames);
          LogF("3ds: picture %ux%u (expected %ux%u), screens %s%s, touch screen at %.0f,%.0f %.0fx%.0f, %s 3DS at %dx; "
               "touchpad (%s%s): %d frames with a finger, %d with the stylus down, %d taps in these 10 s; stylus at "
               "%.0f,%.0f%s%s",
               s_frame_w, s_frame_h, where.w, where.h, screens.c_str(), s_r3_swap.load() ? " (R3: swapped)" : "",
               where.touch[0], where.touch[1], where.touch[2], where.touch[3], s_3ds_new == 1 ? "New" : "Old",
               s_3ds_scale, Settings::Get("3ds_touch", game.key) == "touch" ? "direct" : "trackpad",
               right_stick == "stylus" ? ", the right stick the stylus" :
               right_stick == "dpad"   ? ", the right stick the D-pad" :
                                         "",
               s_touch_frames, s_tap_frames, s_touch_taps, s_stylus.x, s_stylus.y, RestingTouchNote().c_str(),
               dpad_note);
          s_touch_frames = s_tap_frames = s_touch_taps = 0;
          s_stick_dpad_frames = 0;
        }
        // The GPU filter's own GPU time over the same 10 seconds (timestamps around its passes).
        char filter_time[768];
        if (GpuFilter::TakeGpuTime(filter_time, sizeof(filter_time)))
          LogF("%s", filter_time);
        LogPacing(stats, flip);
#ifdef EMUSUNI_VITA
        if (IsVita(s_system))
          vita_cadence.Log();  // ((01.77) how long the game's pictures stayed: VitaCadence)
#endif
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
      // (01.06) Its work only: not its wait for the flip (V-Sync's, the GPU's), which a game at full speed spends there
      // - a light 3DS game would have counted a refresh of drawing and gone heavy for nothing, a frame later.
      const double draw_work = std::max(0.0, pass_draw_ms - pass_draw_wait);
      draw_avg = draw_avg <= 0.0 ? draw_work : draw_avg * 0.9 + draw_work * 0.1;
      // (01.06) The 3DS's worker after the frame's end (the handover): known in both modes, so the heavy mode comes
      // and goes as before the handover. From 0, not from the first one: a first picture waiting for a pipeline
      // (Luigi's Mansion: Dark Moon's on the PC, up to 0.9 s) isn't the game's pace.
      if (pass_worker_ms >= 0.0)
        worker_avg = worker_avg * 0.9 + pass_worker_ms * 0.1;
    }
    // ---- The start watchdog (src/StartGuard.h): the first picture, or none in the console's time ----
    // (00.23: online, the time waiting for the other console - the host's state, its inputs - isn't the game's.)
    const StartGuard::Verdict start_verdict = StartGuard::Pass(stats.total, menu.IsOpen() || Netplay::Waiting());
    if (start_verdict != StartGuard::Verdict::Go)
    {
      // Stopped as Quit Game stops it: started once more (Retry), or the menu says why (the second time).
      result = start_verdict == StartGuard::Verdict::Retry ? Result::Retry : Result::Failed;
      if (start_verdict == StartGuard::Verdict::Fail)
        *error = StartGuard::FailMessage();
      // (00.23: online a start again would be a game of its own: the match ends instead.)
      if (online && result == Result::Retry)
      {
        result = Result::Failed;
        *error = "The game showed no picture: the online match ended";
      }
      quit = true;
      break;
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
  StartGuard::Ending();  // (the stop watched too: a core that doesn't stop restarts EmuSuNi into the menu)
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
  if (DiscCount() > 1 && result == Result::Menu)
    WriteTextFile(Saves::DiscPath(game.key), std::to_string(DiscIn()));
  // (Not while the Dreamcast waits to start again: its machine is stopped inside the BIOS's call. When
  // the Dreamcast's state isn't saved, the one of before goes: Continue would put back its older VMU - an
  // arcade board's older EEPROM.)
  // (A start the start watchdog stopped - Retry - keeps the state it continues from as it was: neither saved over
  // nor deleted.)
  // (00.23: online, a guest's Continue state isn't written over with the host's game.)
  // (00.95: nor a link cable match's: its two Game Boys' state isn't the game's alone, which Continue is.)
  const bool resume = Settings::GetOn("resume", game.key) && !Netplay::KeepOwnSaves() && !s_gblink;
  const bool saved = resume && result == Result::Menu && !dc_reboot_pending && SaveState(0);
  if (resume && !saved && IsFlycast(s_system) && result != Result::Retry)
    Saves::DeleteState(game.key, 0);
  SaveSram(true);
  if (audio_log >= 1.0f)
    audio.LogHealth(audio_log);
  s_audio = nullptr;
  audio.Stop();
  Upscaler::Stop();
  if (emulation_arena)
    ps4_heap_drop_arena();  // ((01.06) for the next 3DS game's; what it holds stays where it is until freed)
  std::vector<uint16_t>().swap(s_scaled);
  std::vector<uint16_t>().swap(s_converted);
  std::vector<uint16_t>().swap(s_rotated);
  pads.Close();
  menu.ForgetPictures();
  s_menu_picture = ImageRGBA();  // (the thumbnail kept as the in-game menu last opened)
  FreeBezel(display);
  if (vita_cover)
    display.DestroyTexture(vita_cover);  // ((01.66) the PS Vita's loading screen's cover)
  s_vita_loading = false;
  // RetroAchievements (00.73): the game's achievements unloaded (an unlock still on its way goes on being sent), their
  // pictures' textures freed.
  Achievements::GameEnded();
  AchievementsUi::Forget(display);
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
#ifdef EMUSUNI_SATURN
  unsetenv("EMUSUNI_SATURN_BIOS");
  unsetenv("EMUSUNI_SATURN_SH2_SKIP");
#endif
  JitArenaReset();
  s_hw_vulkan = false;
  s_hw_image_set = false;
  LogLine("game: ended");
  // What the game left of the memory (its threads' stacks stay: src/MemoryGuard.h) and what it took.
  LogMemory("game ended");
  LogMemorySession();
  return result;
}
}  // namespace Emulator
