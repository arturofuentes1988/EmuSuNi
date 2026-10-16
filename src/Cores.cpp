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

namespace Cores
{
const CoreApi SNES9X = CORE_TABLE(snes9x, "Snes9x");
const CoreApi FCEUMM = CORE_TABLE(fceumm, "FCEUmm");
#ifdef EMUSUNI_N64
const CoreApi MUPEN64PLUS = CORE_TABLE(mupen64plus, "Mupen64Plus");
#endif
}  // namespace Cores
