/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
 *
 * The Game Boy Advance link cable online (01.04): two Game Boy Advances of mGBA in one libretro core, joined by
 * mGBA's own link cable (its lockstep driver, src/gba/sio/lockstep.c), and EACH PS4 of a match runs both, kept in step
 * by the online play's rollback (src/Netplay.h) - as SameBoy's two Game Boys for the Game Boy's cable
 * (scripts/build-gblink-core.sh, src/GbLink.h). Built into mGBA's own object by scripts/build-gba-core.sh (it copies
 * this file into mGBA's libretro folder, whose files its CMake compiles), so there is one mGBA in the app.
 *
 *  - GBA 1 is the host's game (player 1: the host's controller, port 0), GBA 2 the guest's (player 2: port 1); each
 *    can be another cartridge (Ruby with Sapphire, FireRed with Emerald). Both always start at once, without a BIOS
 *    file (mGBA's own: both PS4s start the same machines), with mGBA's known idle loops removed (as the app's mGBA).
 *  - One thread, no waiting: GBA 1 owns the cable's clock and runs one frame; GBA 2 runs after it until it caught up
 *    (the lockstep driver's sleep and wake are flags here, not threads). A frame is GBA 1's.
 *  - Each PS4 draws and plays only its own GBA (the variable gbalink_player, 1 or 2: not the host's options, set by
 *    each console - src/Emulator.cpp): the other GBA is never drawn (drawing changes nothing in the emulation), and
 *    its sound is thrown away. A 240x160 picture, RGB565, as the app's mGBA; the sound at 65536 Hz, as it.
 *  - The cartridges' clock (Pokemon Ruby, Sapphire, Emerald): mGBA's "fake epoch" - the host's time when the match
 *    started (gbalink_rtc, Unix seconds: the host's options are the match's) plus the frames run. In the state.
 *  - The battery saves: each GBA's 128 KiB buffer, as the app's mGBA keeps it (its .srm): gbalink_emusuni_*.
 *  - The state: both GBAs' (mGBA's own states, their battery saves and clocks in them) and this console's GBA's
 *    picture (GBA 2 is a little behind GBA 1 at a frame's end: its picture in the making). Its picture differs from
 *    the other console's, so the match compares the memory, not the states (src/Netplay.cpp, RulesFor).
 *  - mGBA as patched by scripts/build-gba-core.sh (the link across states: the lockstep's event queue loaded whole,
 *    the transfer's end time, no mode change sent while a state loads - GBASIOLockstepLoading - and SIOMLT_SEND kept).
 */
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/core/version.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "libretro-audio.h"

/* mGBA's link cable driver: its CMake builds src/gba/sio/ into its full library only, not into the libretro core
   (which never links two GBAs). Compiled here, with the libretro core's defines (no threads: its mutex is nothing). */
#include "../../gba/sio/lockstep.c"

#define GBAS 2
#define WIDTH 240
#define HEIGHT 160
#define PICTURE_BYTES (WIDTH * HEIGHT * sizeof(mColor))
#define SAVE_BYTES GBA_SIZE_FLASH1M
#define LINK_GAME_TYPE 0x101 /* src/GbLink.h: GAME_TYPE_LINK_2P */
#define AUDIO_CHUNK 1024
/* One GBA's state at most: mGBA's (GBASerializedState), then its extdata - the battery save (128 KiB at most), the
   clock, the link's driver - and their headers. */
#define ONE_STATE_MAX (0x61000 + SAVE_BYTES + 0x1000)
#define STATE_MAGIC 0x4C424745u /* "EGBL" */
#define STATE_VERSION 1u
#define STATE_HEADER 32

/* mGBA's, patched by scripts/build-gba-core.sh: while it is true, a GBA's mode change (its RCNT written again by a
   state loading) reaches the lockstep driver without events to the other GBA. */
extern bool GBASIOLockstepLoading;

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static struct mCore* cores[GBAS];
static void* roms[GBAS];
static size_t rom_sizes[GBAS];
static uint8_t* saves[GBAS];
static mColor* pictures[GBAS]; /* each GBA's picture in the making (only this console's is drawn) */
static mColor* shown;          /* this console's GBA's last finished picture */
static struct GBASIOLockstepCoordinator coordinator;
static struct GBASIOLockstepDriver drivers[GBAS];
struct LinkUser
{
  struct mLockstepUser d;
  int id;
};
static struct LinkUser users[GBAS];
static bool asleep[GBAS];
static int local; /* this console's GBA: 0 or 1 */
static int64_t rtc_ms;
static bool loaded;
static struct LibretroAudioConverter converter;
static int16_t* audio_in;
static int16_t* audio_out;
static uint8_t* memory_scratch;
static size_t memory_scratch_size;
static bool deadlock_said;

static void say(enum retro_log_level level, const char* fmt, ...)
{
  if (!log_cb)
    return;
  char text[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  log_cb(level, "gbalink: %s\n", text);
}

/* mGBA's own messages: its errors only (its default logger may still be the app's mGBA's, from another game). */
static void quiet_log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args)
{
  (void) logger;
  (void) category;
  if (!(level & (mLOG_FATAL | mLOG_ERROR)) || !log_cb)
    return;
  char text[512];
  vsnprintf(text, sizeof(text), format, args);
  log_cb(RETRO_LOG_WARN, "gbalink: mGBA: %s\n", text);
}
static struct mLogger logger = {.log = quiet_log};

static void link_sleep(struct mLockstepUser* user)
{
  asleep[((struct LinkUser*) user)->id] = true;
}
static void link_wake(struct mLockstepUser* user)
{
  asleep[((struct LinkUser*) user)->id] = false;
}
static int link_requested_id(struct mLockstepUser* user)
{
  return ((struct LinkUser*) user)->id;
}
static void link_id_changed(struct mLockstepUser* user, int id)
{
  (void) user;
  (void) id;
}

static void frame_ended(void* context)
{
  if ((int) (intptr_t) context == local)
    memcpy(shown, pictures[local], PICTURE_BYTES);
}

/* This console's GBA drawn (when the app wants pictures: a frame run again in a rollback needn't be - but GBA 2's frame
   ends a little after GBA 1's, inside the next frame, so it is always drawn), the other never. */
static void set_drawing(bool video)
{
  for (int i = 0; i < GBAS; i++)
  {
    struct GBA* gba = cores[i]->board;
    gba->video.frameskip = 0;
    if (i != local)
      gba->video.frameskipCounter = 1 << 30;
    else
      gba->video.frameskipCounter = (local == 0 && !video) ? 1 : 0;
  }
}

static const char* variable(const char* key)
{
  struct retro_variable var = {key, NULL};
  if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    return var.value;
  return NULL;
}

static void read_player(void)
{
  const char* player = variable("gbalink_player");
  local = player && player[0] == '2' ? 1 : 0;
}

void gbalink_retro_set_environment(retro_environment_t cb)
{
  env_cb = cb;
  struct retro_log_callback log;
  if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    log_cb = log.log;
}
void gbalink_retro_set_video_refresh(retro_video_refresh_t cb)
{
  video_cb = cb;
}
void gbalink_retro_set_audio_sample(retro_audio_sample_t cb)
{
  (void) cb;
}
void gbalink_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  audio_batch_cb = cb;
}
void gbalink_retro_set_input_poll(retro_input_poll_t cb)
{
  input_poll_cb = cb;
}
void gbalink_retro_set_input_state(retro_input_state_t cb)
{
  input_state_cb = cb;
}

void gbalink_retro_init(void)
{
  mLogSetDefaultLogger(&logger);
}

void gbalink_retro_deinit(void) {}

unsigned gbalink_retro_api_version(void)
{
  return RETRO_API_VERSION;
}

void gbalink_retro_get_system_info(struct retro_system_info* info)
{
  memset(info, 0, sizeof(*info));
  info->library_name = "mGBA (link)";
  info->library_version = projectVersion;
  info->valid_extensions = "gba";
  info->need_fullpath = false;
  info->block_extract = false;
}

void gbalink_retro_get_system_av_info(struct retro_system_av_info* info)
{
  memset(info, 0, sizeof(*info));
  info->geometry.base_width = WIDTH;
  info->geometry.base_height = HEIGHT;
  info->geometry.max_width = WIDTH;
  info->geometry.max_height = HEIGHT;
  info->geometry.aspect_ratio = (float) WIDTH / HEIGHT;
  info->timing.fps = 16777216.0 / 280896.0; /* the GBA's clock over a frame's cycles (59.73) */
  info->timing.sample_rate = GBA_OUTPUT_RATE;
}

void gbalink_retro_set_controller_port_device(unsigned port, unsigned device)
{
  (void) port;
  (void) device;
}

static void unload(void)
{
  for (int i = 0; i < GBAS; i++)
  {
    if (cores[i])
    {
      cores[i]->setPeripheral(cores[i], mPERIPH_GBA_LINK_PORT, NULL);
      if (drivers[i].coordinator)
        GBASIOLockstepCoordinatorDetach(&coordinator, &drivers[i]);
      mCoreConfigDeinit(&cores[i]->config);
      cores[i]->deinit(cores[i]);
      cores[i] = NULL;
    }
    if (roms[i])
      mappedMemoryFree(roms[i], rom_sizes[i]);
    if (saves[i])
      mappedMemoryFree(saves[i], SAVE_BYTES);
    free(pictures[i]);
    roms[i] = NULL;
    saves[i] = NULL;
    pictures[i] = NULL;
    memset(&drivers[i], 0, sizeof(drivers[i]));
  }
  if (loaded)
    GBASIOLockstepCoordinatorDeinit(&coordinator);
  free(shown);
  free(audio_in);
  free(audio_out);
  free(memory_scratch);
  shown = NULL;
  audio_in = audio_out = NULL;
  memory_scratch = NULL;
  memory_scratch_size = 0;
  loaded = false;
}

static bool load_one(int i, const struct retro_game_info* game)
{
  struct mCore* core = GBACoreCreate();
  if (!core)
    return false;
  mCoreInitConfig(core, NULL);
  if (!core->init(core))
  {
    mCoreConfigDeinit(&core->config);
    free(core);
    return false;
  }
  cores[i] = core;
  mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "remove");
  mCoreLoadConfig(core);
  pictures[i] = calloc(WIDTH * HEIGHT, sizeof(mColor));
  if (!pictures[i])
    return false;
  core->setVideoBuffer(core, pictures[i], WIDTH);
  roms[i] = anonymousMemoryMap(game->size);
  if (!roms[i])
    return false;
  rom_sizes[i] = game->size;
  memcpy(roms[i], game->data, game->size);
  struct VFile* rom = VFileFromMemory(roms[i], game->size);
  if (!rom || !core->loadROM(core, rom))
    return false;
  saves[i] = anonymousMemoryMap(SAVE_BYTES);
  if (!saves[i])
    return false;
  memset(saves[i], 0xFF, SAVE_BYTES);
  struct VFile* save = VFileFromMemory(saves[i], SAVE_BYTES);
  if (!save || !core->loadSave(core, save))
    return false;
  core->rtc.override = RTC_FAKE_EPOCH;
  core->rtc.value = rtc_ms;
  core->reset(core);
  struct mCoreCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = (void*) (intptr_t) i;
  callbacks.videoFrameEnded = frame_ended;
  core->addCoreCallbacks(core, &callbacks);
  users[i].d.sleep = link_sleep;
  users[i].d.wake = link_wake;
  users[i].d.requestedId = link_requested_id;
  users[i].d.playerIdChanged = link_id_changed;
  users[i].id = i;
  asleep[i] = false;
  GBASIOLockstepDriverCreate(&drivers[i], &users[i].d);
  GBASIOLockstepCoordinatorAttach(&coordinator, &drivers[i]);
  core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &drivers[i].d);
  return true;
}

bool gbalink_retro_load_game_special(unsigned type, const struct retro_game_info* games, size_t count)
{
  if (type != LINK_GAME_TYPE || count != GBAS || !games)
    return false;
  for (size_t i = 0; i < count; i++)
  {
    if (!games[i].data || !games[i].size)
    {
      say(RETRO_LOG_ERROR, "GBA %u's game came without its bytes", (unsigned) i + 1);
      return false;
    }
  }
  unload();
  read_player();
  const char* rtc = variable("gbalink_rtc");
  /* (No time given: 2004-01-01 00:00 UTC, the same on both consoles.) */
  rtc_ms = (rtc ? strtoll(rtc, NULL, 10) : 1072915200LL) * 1000LL;
  memset(&coordinator, 0, sizeof(coordinator));
  GBASIOLockstepCoordinatorInit(&coordinator);
  loaded = true;
  shown = calloc(WIDTH * HEIGHT, sizeof(mColor));
  audio_in = malloc(AUDIO_CHUNK * 2 * sizeof(int16_t));
  audio_out = malloc(AUDIO_CHUNK * 2 * 2 * sizeof(int16_t));
  bool ok = shown && audio_in && audio_out;
  for (int i = 0; ok && i < GBAS; i++)
  {
    ok = load_one(i, &games[i]);
    if (!ok)
      say(RETRO_LOG_ERROR, "GBA %d's game couldn't be loaded", i + 1);
  }
  if (!ok)
  {
    unload();
    return false;
  }
  enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
  env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
  audioConverterReset(&converter, cores[local]->audioSampleRate(cores[local]));
  set_drawing(true);
  deadlock_said = false;
  say(RETRO_LOG_INFO, "two GBAs joined by the cable; ours GBA %d; the cartridges' clock from %lld", local + 1,
      (long long) (rtc_ms / 1000));
  return true;
}

bool gbalink_retro_load_game(const struct retro_game_info* game)
{
  (void) game;
  say(RETRO_LOG_ERROR, "the link cable's core takes two games (retro_load_game_special)");
  return false;
}

void gbalink_retro_unload_game(void)
{
  unload();
}

void gbalink_retro_reset(void)
{
  if (!loaded)
    return;
  for (int i = 0; i < GBAS; i++)
    cores[i]->reset(cores[i]);
  set_drawing(true);
}

/* One frame: GBA 1 runs to its next frame's start; GBA 2 follows until it caught up with the cable's clock (asleep). */
static void run_frame(void)
{
  struct mCore* first = cores[0];
  const uint32_t target = first->frameCounter(first) + 1;
  long steps = 0;
  for (;;)
  {
    const bool first_done = (int32_t) (first->frameCounter(first) - target) >= 0;
    bool ran = false;
    if (!first_done && !asleep[0])
    {
      first->runLoop(first);
      ran = true;
    }
    for (int i = 1; i < GBAS; i++)
    {
      while (!asleep[i] && steps < 50000000)
      {
        cores[i]->runLoop(cores[i]);
        ran = true;
        steps++;
      }
    }
    if (first_done)
      return;
    if (!ran || steps >= 50000000)
    {
      if (!deadlock_said)
        say(RETRO_LOG_ERROR, "the cable's GBAs wait for each other (GBA 1 %s, GBA 2 %s): the frame cut short",
            asleep[0] ? "asleep" : "awake", asleep[1] ? "asleep" : "awake");
      deadlock_said = true;
      return;
    }
  }
}

static const unsigned BUTTONS[10] = {
    RETRO_DEVICE_ID_JOYPAD_A,     RETRO_DEVICE_ID_JOYPAD_B,    RETRO_DEVICE_ID_JOYPAD_SELECT,
    RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
    RETRO_DEVICE_ID_JOYPAD_UP,    RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_R,
    RETRO_DEVICE_ID_JOYPAD_L};

void gbalink_retro_run(void)
{
  if (!loaded)
    return;
  bool updated = false;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
  {
    const int before = local;
    read_player();
    if (local != before)
      audioConverterReset(&converter, cores[local]->audioSampleRate(cores[local]));
  }
  input_poll_cb();
  for (int i = 0; i < GBAS; i++)
  {
    uint32_t keys = 0;
    for (unsigned b = 0; b < 10; b++)
    {
      if (input_state_cb((unsigned) i, RETRO_DEVICE_JOYPAD, 0, BUTTONS[b]))
        keys |= 1u << b;
    }
    cores[i]->setKeys(cores[i], keys);
  }
  int av = 3;
  if (!env_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av))
    av = 3;
  set_drawing((av & 1) != 0);
  run_frame();

  for (int i = 0; i < GBAS; i++)
  {
    struct mAudioBuffer* buffer = cores[i]->getAudioBuffer(cores[i]);
    if (i != local || !(av & 2) || !audio_batch_cb)
    {
      mAudioBufferClear(buffer);
      continue;
    }
    const unsigned rate = cores[i]->audioSampleRate(cores[i]);
    if (rate != converter.inputRate)
      audioConverterReset(&converter, rate);
    for (;;)
    {
      const size_t read = mAudioBufferRead(buffer, audio_in, AUDIO_CHUNK);
      if (!read)
        break;
      const size_t made = audioConverterProcess(&converter, audio_in, read, audio_out);
      if (made)
        audio_batch_cb(audio_out, made);
    }
  }
  if (video_cb)
    video_cb(shown, WIDTH, HEIGHT, WIDTH * sizeof(mColor));
}

/* The state: a header (magic, version, GBAs, whose picture), then each GBA's (its size, mGBA's state with its battery
   save and clock), then the picture of the header's GBA (in the making, then its last finished). Zeros after. */
size_t gbalink_retro_serialize_size(void)
{
  return loaded ? STATE_HEADER + GBAS * (4 + ONE_STATE_MAX) + 2 * PICTURE_BYTES : 0;
}

static void put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16);
  p[3] = (uint8_t) (v >> 24);
}
static uint32_t get32(const uint8_t* p)
{
  return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

bool gbalink_retro_serialize(void* data, size_t size)
{
  if (!loaded || !data || size < gbalink_retro_serialize_size())
    return false;
  uint8_t* out = data;
  memset(out, 0, STATE_HEADER);
  put32(out + 0, STATE_MAGIC);
  put32(out + 4, STATE_VERSION);
  put32(out + 8, GBAS);
  put32(out + 12, (uint32_t) local);
  size_t at = STATE_HEADER;
  for (int i = 0; i < GBAS; i++)
  {
    uint8_t* one = out + at + 4;
    struct VFile* vf = VFileFromMemory(one, ONE_STATE_MAX);
    if (!vf)
      return false;
    const bool ok = mCoreSaveStateNamed(cores[i], vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
    const ssize_t written = vf->seek(vf, 0, SEEK_CUR);
    vf->close(vf);
    if (!ok || written <= 0 || (size_t) written > ONE_STATE_MAX)
    {
      say(RETRO_LOG_ERROR, "GBA %d's state couldn't be saved", i + 1);
      return false;
    }
    put32(out + at, (uint32_t) written);
    memset(one + written, 0, ONE_STATE_MAX - (size_t) written);
    at += 4 + ONE_STATE_MAX;
  }
  memcpy(out + at, pictures[local], PICTURE_BYTES);
  memcpy(out + at + PICTURE_BYTES, shown, PICTURE_BYTES);
  at += 2 * PICTURE_BYTES;
  if (size > at)
    memset(out + at, 0, size - at);
  return true;
}

bool gbalink_retro_unserialize(const void* data, size_t size)
{
  if (!loaded || !data || size < gbalink_retro_serialize_size())
    return false;
  const uint8_t* in = data;
  if (get32(in) != STATE_MAGIC || get32(in + 4) != STATE_VERSION || get32(in + 8) != GBAS)
  {
    say(RETRO_LOG_WARN, "a state that isn't this core's (or of another version): not loaded");
    return false;
  }
  size_t at = STATE_HEADER;
  for (int i = 0; i < GBAS; i++)
  {
    if (get32(in + at) == 0 || get32(in + at) > ONE_STATE_MAX)
      return false;
    at += 4 + ONE_STATE_MAX;
  }
  at = STATE_HEADER;
  bool ok = true;
  GBASIOLockstepLoading = true;
  for (int i = 0; i < GBAS; i++)
  {
    struct VFile* vf = VFileFromConstMemory(in + at + 4, get32(in + at));
    if (!vf || !mCoreLoadStateNamed(cores[i], vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC))
    {
      say(RETRO_LOG_ERROR, "GBA %d's state couldn't be loaded", i + 1);
      ok = false;
    }
    if (vf)
      vf->close(vf);
    at += 4 + ONE_STATE_MAX;
  }
  GBASIOLockstepLoading = false;
  /* This console's GBA's picture (a state of the other console's: its own GBA's, not ours - left as it is). */
  if ((int) get32(in + 12) == local)
  {
    memcpy(pictures[local], in + at, PICTURE_BYTES);
    memcpy(shown, in + at + PICTURE_BYTES, PICTURE_BYTES);
  }
  set_drawing(true);
  return ok;
}

void gbalink_retro_cheat_reset(void) {}
void gbalink_retro_cheat_set(unsigned index, bool enabled, const char* code)
{
  (void) index;
  (void) enabled;
  (void) code;
}

unsigned gbalink_retro_get_region(void)
{
  return RETRO_REGION_NTSC;
}

/* For the match's checks (src/Netplay.cpp's MemoryCrc): both GBAs' work RAM (EWRAM, IWRAM) as the system's RAM, their
   VRAM, palettes and OAM as the video's - copied now, one after the other. No battery save here (the app reads and
   writes them with gbalink_emusuni_*). */
void* gbalink_retro_get_memory_data(unsigned id)
{
  if (!loaded || (id != RETRO_MEMORY_SYSTEM_RAM && id != RETRO_MEMORY_VIDEO_RAM))
    return NULL;
  const size_t one = id == RETRO_MEMORY_SYSTEM_RAM ? GBA_SIZE_EWRAM + GBA_SIZE_IWRAM :
                                                     GBA_SIZE_VRAM + GBA_SIZE_PALETTE_RAM + GBA_SIZE_OAM;
  if (memory_scratch_size < GBAS * one)
  {
    free(memory_scratch);
    memory_scratch = malloc(GBAS * one);
    memory_scratch_size = memory_scratch ? GBAS * one : 0;
    if (!memory_scratch)
      return NULL;
  }
  uint8_t* p = memory_scratch;
  for (int i = 0; i < GBAS; i++)
  {
    struct GBA* gba = cores[i]->board;
    if (id == RETRO_MEMORY_SYSTEM_RAM)
    {
      memcpy(p, gba->memory.wram, GBA_SIZE_EWRAM);
      memcpy(p + GBA_SIZE_EWRAM, gba->memory.iwram, GBA_SIZE_IWRAM);
    }
    else
    {
      memcpy(p, gba->video.vram, GBA_SIZE_VRAM);
      memcpy(p + GBA_SIZE_VRAM, gba->video.palette, GBA_SIZE_PALETTE_RAM);
      memcpy(p + GBA_SIZE_VRAM + GBA_SIZE_PALETTE_RAM, gba->video.oam.raw, GBA_SIZE_OAM);
    }
    p += one;
  }
  return memory_scratch;
}

size_t gbalink_retro_get_memory_size(unsigned id)
{
  if (!loaded)
    return 0;
  if (id == RETRO_MEMORY_SYSTEM_RAM)
    return GBAS * (GBA_SIZE_EWRAM + GBA_SIZE_IWRAM);
  if (id == RETRO_MEMORY_VIDEO_RAM)
    return GBAS * (GBA_SIZE_VRAM + GBA_SIZE_PALETTE_RAM + GBA_SIZE_OAM);
  return 0;
}

/* ---- The battery saves (src/Emulator.cpp; as the app's mGBA keeps them: its 128 KiB buffer, its .srm) ---- */
/* GBA `gba`'s save size (0 or 1): its kind's - or 128 KiB while mGBA hasn't seen which kind it is (as the app's mGBA
   says). 0: no game. */
size_t gbalink_emusuni_battery_size(unsigned gba)
{
  if (!loaded || gba >= GBAS)
    return 0;
  struct GBA* board = cores[gba]->board;
  if (board->memory.savedata.type == GBA_SAVEDATA_AUTODETECT)
    return SAVE_BYTES;
  const size_t size = GBASavedataSize(&board->memory.savedata);
  return size <= SAVE_BYTES ? size : 0;
}

/* The same: a GBA cartridge's save has no clock after it (its clock is the console's time). */
size_t gbalink_emusuni_ram_size(unsigned gba)
{
  return gbalink_emusuni_battery_size(gba);
}

bool gbalink_emusuni_save_battery(unsigned gba, uint8_t* out, size_t size)
{
  const size_t want = gbalink_emusuni_battery_size(gba);
  if (!want || size < want || !out)
    return false;
  memcpy(out, saves[gba], want);
  return true;
}

/* A save into GBA `gba` before its first frame (as the app's mGBA: into its buffer; the rest stays 0xFF). */
void gbalink_emusuni_load_battery(unsigned gba, const uint8_t* data, size_t size)
{
  if (!loaded || gba >= GBAS || !data || !size)
    return;
  memcpy(saves[gba], data, size < SAVE_BYTES ? size : SAVE_BYTES);
}

#ifdef EMUSUNI_GBALINK_DEBUG
/* The PC test's only (GBA_CORE_HOST=1): a byte of GBA `gba`'s memory read or written as its CPU would. */
uint8_t gbalink_emusuni_debug_read(unsigned gba, uint32_t address)
{
  return loaded && gba < GBAS ? (uint8_t) cores[gba]->busRead8(cores[gba], address) : 0xFF;
}
void gbalink_emusuni_debug_write(unsigned gba, uint32_t address, uint8_t value)
{
  if (loaded && gba < GBAS)
    cores[gba]->busWrite8(cores[gba], address, value);
}
#endif
