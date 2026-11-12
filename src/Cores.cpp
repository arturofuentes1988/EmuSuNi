// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Cores.h"

// One core's renamed libretro API (CMakeLists.txt: core_prefix_header).
#define DECLARE_CORE(core)                                                                     \
  extern "C" {                                                                                 \
  void core##_retro_set_environment(retro_environment_t);                                      \
  void core##_retro_set_video_refresh(retro_video_refresh_t);                                  \
  void core##_retro_set_audio_sample(retro_audio_sample_t);                                    \
  void core##_retro_set_audio_sample_batch(retro_audio_sample_batch_t);                        \
  void core##_retro_set_input_poll(retro_input_poll_t);                                        \
  void core##_retro_set_input_state(retro_input_state_t);                                      \
  void core##_retro_init(void);                                                                \
  void core##_retro_deinit(void);                                                              \
  unsigned core##_retro_api_version(void);                                                     \
  void core##_retro_get_system_info(struct retro_system_info*);                                \
  void core##_retro_get_system_av_info(struct retro_system_av_info*);                          \
  void core##_retro_set_controller_port_device(unsigned, unsigned);                            \
  void core##_retro_reset(void);                                                               \
  void core##_retro_run(void);                                                                 \
  size_t core##_retro_serialize_size(void);                                                    \
  bool core##_retro_serialize(void*, size_t);                                                  \
  bool core##_retro_unserialize(const void*, size_t);                                          \
  void core##_retro_cheat_reset(void);                                                         \
  void core##_retro_cheat_set(unsigned, bool, const char*);                                    \
  bool core##_retro_load_game(const struct retro_game_info*);                                  \
  bool core##_retro_load_game_special(unsigned, const struct retro_game_info*, size_t);        \
  void core##_retro_unload_game(void);                                                         \
  unsigned core##_retro_get_region(void);                                                      \
  void* core##_retro_get_memory_data(unsigned);                                                \
  size_t core##_retro_get_memory_size(unsigned);                                               \
  }

#define CORE_TABLE(core, label)                                                                \
  {                                                                                            \
    label, core##_retro_set_environment, core##_retro_set_video_refresh,                       \
        core##_retro_set_audio_sample, core##_retro_set_audio_sample_batch,                    \
        core##_retro_set_input_poll, core##_retro_set_input_state, core##_retro_init,          \
        core##_retro_deinit, core##_retro_api_version, core##_retro_get_system_info,           \
        core##_retro_get_system_av_info, core##_retro_set_controller_port_device,              \
        core##_retro_reset, core##_retro_run, core##_retro_serialize_size,                     \
        core##_retro_serialize, core##_retro_unserialize, core##_retro_cheat_reset,            \
        core##_retro_cheat_set, core##_retro_load_game, core##_retro_load_game_special,        \
        core##_retro_unload_game, core##_retro_get_region, core##_retro_get_memory_data,       \
        core##_retro_get_memory_size                                                           \
  }

DECLARE_CORE(snes9x)
DECLARE_CORE(fceumm)
#ifdef EMUSUNI_N64
DECLARE_CORE(mupen64plus)
#endif
#ifdef EMUSUNI_GBA
DECLARE_CORE(mgba)
#endif
#ifdef EMUSUNI_PS1
DECLARE_CORE(swanstation)
#endif
#ifdef EMUSUNI_NDS
DECLARE_CORE(melondsds)
#endif
#ifdef EMUSUNI_PSP
DECLARE_CORE(ppsspp)
#endif
#ifdef EMUSUNI_DC
DECLARE_CORE(flycast)
#endif
#ifdef EMUSUNI_SEGA
DECLARE_CORE(gpgx)
#endif
#ifdef EMUSUNI_ARCADE
DECLARE_CORE(fbneo)
#endif
#ifdef EMUSUNI_3DS
DECLARE_CORE(azahar)
#endif
#ifdef EMUSUNI_SATURN
DECLARE_CORE(yabasanshiro)
#endif
#ifdef EMUSUNI_PCE
DECLARE_CORE(beetle_pce)
#endif
#ifdef EMUSUNI_A2600
DECLARE_CORE(stella)
#endif
#ifdef EMUSUNI_A7800
DECLARE_CORE(prosystem)
#endif
#ifdef EMUSUNI_GBLINK
DECLARE_CORE(sameboy)
#endif
#ifdef EMUSUNI_GBALINK
DECLARE_CORE(gbalink)
#endif
#ifdef EMUSUNI_JAGUAR
DECLARE_CORE(vjaguar)
#endif
#ifdef EMUSUNI_OPENBOR
DECLARE_CORE(openbor)
#endif

namespace Cores
{
const CoreApi SNES9X = CORE_TABLE(snes9x, "Snes9x");
const CoreApi FCEUMM = CORE_TABLE(fceumm, "FCEUmm");
#ifdef EMUSUNI_N64
const CoreApi MUPEN64PLUS = CORE_TABLE(mupen64plus, "Mupen64Plus");
#endif
#ifdef EMUSUNI_GBA
const CoreApi MGBA = CORE_TABLE(mgba, "mGBA");
#endif
#ifdef EMUSUNI_PS1
const CoreApi SWANSTATION = CORE_TABLE(swanstation, "SwanStation");
#endif
#ifdef EMUSUNI_NDS
const CoreApi MELONDSDS = CORE_TABLE(melondsds, "melonDS DS");
#endif
#ifdef EMUSUNI_PSP
const CoreApi PPSSPP = CORE_TABLE(ppsspp, "PPSSPP");
#endif
#ifdef EMUSUNI_DC
const CoreApi FLYCAST = CORE_TABLE(flycast, "Flycast");
#endif
#ifdef EMUSUNI_SEGA
const CoreApi GPGX = CORE_TABLE(gpgx, "Genesis Plus GX");
#endif
#ifdef EMUSUNI_ARCADE
const CoreApi FBNEO = CORE_TABLE(fbneo, "FinalBurn Neo");
#endif
#ifdef EMUSUNI_3DS
const CoreApi AZAHAR = CORE_TABLE(azahar, "Azahar");
#endif
#ifdef EMUSUNI_SATURN
const CoreApi YABASANSHIRO = CORE_TABLE(yabasanshiro, "YabaSanshiro");
#endif
#ifdef EMUSUNI_PCE
const CoreApi BEETLE_PCE = CORE_TABLE(beetle_pce, "Beetle PCE Fast");
#endif
#ifdef EMUSUNI_A2600
const CoreApi STELLA = CORE_TABLE(stella, "Stella");
#endif
#ifdef EMUSUNI_A7800
const CoreApi PROSYSTEM = CORE_TABLE(prosystem, "ProSystem");
#endif
#ifdef EMUSUNI_GBLINK
const CoreApi SAMEBOY = CORE_TABLE(sameboy, "SameBoy");
#endif
#ifdef EMUSUNI_GBALINK
const CoreApi GBALINK = CORE_TABLE(gbalink, "mGBA (link)");
#endif
#ifdef EMUSUNI_JAGUAR
const CoreApi VIRTUAL_JAGUAR = CORE_TABLE(vjaguar, "Virtual Jaguar");
#endif
#ifdef EMUSUNI_OPENBOR
const CoreApi OPENBOR = CORE_TABLE(openbor, "OpenBOR");
#endif
}  // namespace Cores
