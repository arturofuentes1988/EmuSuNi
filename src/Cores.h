// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The emulator cores linked into EmuSuNi. Each one is a libretro core whose API was renamed when
// it was built (snes9x_retro_init, fceumm_retro_init: see CMakeLists.txt); a CoreApi holds one
// core's functions, so the emulator calls whichever core the game needs.

#pragma once

#include <cstddef>

#include <libretro.h>

struct CoreApi
{
  const char* name;  // "Snes9x", "FCEUmm" (the log, About)
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*init)(void);
  void (*deinit)(void);
  unsigned (*api_version)(void);
  void (*get_system_info)(struct retro_system_info*);
  void (*get_system_av_info)(struct retro_system_av_info*);
  void (*set_controller_port_device)(unsigned, unsigned);
  void (*reset)(void);
  void (*run)(void);
  size_t (*serialize_size)(void);
  bool (*serialize)(void*, size_t);
  bool (*unserialize)(const void*, size_t);
  void (*cheat_reset)(void);
  void (*cheat_set)(unsigned, bool, const char*);
  bool (*load_game)(const struct retro_game_info*);
  bool (*load_game_special)(unsigned, const struct retro_game_info*, size_t);
  void (*unload_game)(void);
  unsigned (*get_region)(void);
  void* (*get_memory_data)(unsigned);
  size_t (*get_memory_size)(unsigned);
};

namespace Cores
{
extern const CoreApi SNES9X;  // Super Nintendo
extern const CoreApi FCEUMM;  // NES
#ifdef EMUSUNI_N64
extern const CoreApi MUPEN64PLUS;  // Nintendo 64 (Mupen64Plus-Next; builds with EMUSUNI_N64)
#endif
#ifdef EMUSUNI_GBA
extern const CoreApi MGBA;  // Game Boy, Game Boy Color, Game Boy Advance (builds with EMUSUNI_GBA)
#endif
#ifdef EMUSUNI_PS1
extern const CoreApi SWANSTATION;  // PlayStation (builds with EMUSUNI_PS1)
#endif
#ifdef EMUSUNI_NDS
extern const CoreApi MELONDSDS;  // Nintendo DS (melonDS DS; builds with EMUSUNI_NDS)
#endif
#ifdef EMUSUNI_PSP
extern const CoreApi PPSSPP;  // PSP (builds with EMUSUNI_PSP)
#endif
#ifdef EMUSUNI_DC
extern const CoreApi FLYCAST;  // Dreamcast (builds with EMUSUNI_DC)
#endif
#ifdef EMUSUNI_SEGA
extern const CoreApi GPGX;  // SG-1000, Master System, Game Gear, Mega Drive (builds with EMUSUNI_SEGA)
#endif
#ifdef EMUSUNI_ARCADE
extern const CoreApi FBNEO;  // Arcade (FinalBurn Neo; builds with EMUSUNI_ARCADE)
#endif
#ifdef EMUSUNI_3DS
extern const CoreApi AZAHAR;  // Nintendo 3DS (Azahar; builds with EMUSUNI_3DS)
#endif
#ifdef EMUSUNI_SATURN
// Sega Saturn: YabaSanshiro with its software renderer (experimental; its speed test too, src/SaturnTest.h; builds
// with EMUSUNI_SATURN)
extern const CoreApi YABASANSHIRO;
#endif
#ifdef EMUSUNI_PCE
extern const CoreApi BEETLE_PCE;  // PC Engine (Beetle PCE Fast; builds with EMUSUNI_PCE)
#endif
#ifdef EMUSUNI_A2600
extern const CoreApi STELLA;  // Atari 2600 (Stella; builds with EMUSUNI_A2600, 00.90)
#endif
#ifdef EMUSUNI_A7800
extern const CoreApi PROSYSTEM;  // Atari 7800 (ProSystem; builds with EMUSUNI_A7800, 00.90)
#endif
#ifdef EMUSUNI_JAGUAR
extern const CoreApi VIRTUAL_JAGUAR;  // Atari Jaguar (Virtual Jaguar; builds with EMUSUNI_JAGUAR, 00.90)
#endif
#ifdef EMUSUNI_GBLINK
// Two Game Boys joined by the link cable, online (SameBoy; builds with EMUSUNI_GBLINK, 00.95: src/GbLink.h).
extern const CoreApi SAMEBOY;
#endif
#ifdef EMUSUNI_GBALINK
// Two Game Boy Advances joined by the link cable, online (mGBA, in its object: gbalink_retro_*; 01.04, src/GbLink.h).
extern const CoreApi GBALINK;
#endif
#ifdef EMUSUNI_OPENBOR
// OpenBOR: the beat 'em up engine's games, .pak files (zanac's multi-engine core; builds with EMUSUNI_OPENBOR,
// with-openbor).
extern const CoreApi OPENBOR;
#endif
}  // namespace Cores
