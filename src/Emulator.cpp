// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Emulator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
#include "Image.h"
#include "IngameMenu.h"
#include "Log.h"
#include "PS4Lang.h"
#include "Saves.h"
#include "Settings.h"
#include "Tour.h"
#include "Upscaler.h"

namespace Emulator
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr int MAX_PLAYERS = 5;
constexpr unsigned DEVICE_MULTITAP = (1 << 8) | RETRO_DEVICE_JOYPAD;  // Snes9x's Multitap

// ---- State the libretro callbacks use (one game at a time). ----
std::string s_game_id;
std::string s_system_dir, s_save_dir;
std::map<std::string, std::string> s_variables;  // snes9x_* option values for GET_VARIABLE
bool s_variables_changed = false;
bool s_render_video = true;
std::array<uint16_t, MAX_PLAYERS> s_buttons{};  // SNES buttons per input port
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
  LogF("snes9x: %s", line);
}

// The menus' settings as the core's options.
void BuildVariables()
{
  const std::string& g = s_game_id;
  s_variables.clear();
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
  s_variables_changed = true;
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
  if (!pixels)
    return;  // a duplicated frame
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
};

Picture PlacePicture(const std::string& game)
{
  Picture picture;
  const float W = static_cast<float>(Display::WIDTH), H = static_cast<float>(Display::HEIGHT);
  // The picture in the console's units: 256 wide (512 hi-res, 602 with the NTSC filter), 224 or
  // 239 lines (448 / 478 interlaced).
  const float lines = s_frame_h > 300 ? s_frame_h * 0.5f : static_cast<float>(s_frame_h);
  picture.rows = lines;
  const std::string aspect = Settings::Get("aspect", game);
  float ratio = 4.0f / 3.0f;
  if (aspect == "pixel")
    ratio = 256.0f / std::max(1.0f, lines);
  else if (aspect == "wide")
    ratio = 16.0f / 9.0f;
  else if (aspect == "stretch")
    ratio = W / H;
  const std::string scaling = Settings::Get("scaling", game);
  float height = H;
  if (scaling == "integer")
    height = std::floor(H / lines) * lines;
  else if (scaling == "2" || scaling == "3" || scaling == "4")
    height = lines * static_cast<float>(std::atoi(scaling.c_str()));
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
  picture.min = ImVec2((W - width) * 0.5f, (H - height) * 0.5f);
  picture.max = ImVec2(picture.min.x + width, picture.min.y + height);
  return picture;
}

void DrawPicture(ImDrawList* dl, Display& display, const std::string& game)
{
  if (s_frame_w == 0 || s_frame_h == 0)
    return;
  if (s_texture_w == 0 || s_texture_h == 0)
    return;
  const Picture picture = PlacePicture(game);
  // A picture made bigger is always drawn smooth (it is rarely an exact multiple of the TV's
  // lines). Smooth stays half a texel inside: around the picture are older, bigger ones.
  const bool smooth = s_texture_scaled || Settings::Get("filter", game) == "smooth";
  const float inset = smooth ? 0.5f : 0.0f;
  const ImVec2 uv0(inset / Display::GAME_TEXTURE_W, inset / Display::GAME_TEXTURE_H);
  const ImVec2 uv1((s_texture_w - inset) / Display::GAME_TEXTURE_W,
                   (s_texture_h - inset) / Display::GAME_TEXTURE_H);
  dl->AddImage(ImTextureRef(display.GameTexture(smooth)), picture.min, picture.max, uv0, uv1);
  const std::string scanlines = Settings::Get("scanlines", game);
  if (scanlines != "off")
  {
    const int alpha = scanlines == "light" ? 60 : scanlines == "medium" ? 110 : 160;
    const float row = (picture.max.y - picture.min.y) / picture.rows;
    const ImU32 dark = IM_COL32(0, 0, 0, alpha);
    for (float y = picture.min.y; y < picture.max.y; y += row)
      dl->AddRectFilled(ImVec2(picture.min.x, y + row * 0.55f), ImVec2(picture.max.x, y + row), dark);
  }
}

// The new picture into the game texture: as it is, or made bigger first (Resolution).
void UploadPicture(Display& display, const std::string& game)
{
  int factor = std::atoi(Settings::Get("resolution", game).c_str());
  if (s_frame_w > 300 || s_frame_h > 300)
    factor /= 2;  // hi-res and interlaced pictures are twice as big already
  if (Settings::Get("ntsc_filter", game) != "disabled")
    factor = 1;  // the NTSC filter's picture is a TV's look of its own
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
std::vector<uint8_t> s_sram_saved;  // the battery save as last written

void LoadSram()
{
  uint8_t* sram = static_cast<uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
  const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  s_sram_saved.clear();
  if (!sram || size == 0)
    return;
  FILE* file = fopen(Saves::SramPath(s_game_id).c_str(), "rb");
  if (file)
  {
    const size_t read = fread(sram, 1, size, file);
    fclose(file);
    LogF("save: %zu of %zu bytes of battery save loaded", read, size);
  }
  s_sram_saved.assign(sram, sram + size);
}

// Writes the battery save when the game changed it.
void SaveSram(bool force)
{
  const uint8_t* sram = static_cast<const uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
  const size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
  if (!sram || size == 0)
    return;
  if (!force && s_sram_saved.size() == size && std::memcmp(s_sram_saved.data(), sram, size) == 0)
    return;
  const std::string path = Saves::SramPath(s_game_id);
  const std::string text(reinterpret_cast<const char*>(sram), size);
  if (WriteTextFile(path, text))
  {
    s_sram_saved.assign(sram, sram + size);
    LogF("save: battery save written (%zu bytes)", size);
  }
}

bool SaveState(int slot)
{
  const size_t size = retro_serialize_size();
  if (size == 0)
    return false;
  std::vector<uint8_t> state(size);
  if (!retro_serialize(state.data(), size))
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
  const bool ok = retro_unserialize(data.data(), data.size());
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
};

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
  s_game_id = game.id;
  s_system_dir = std::string(App::DATA_DIR) + "/system";
  s_save_dir = std::string(App::DATA_DIR) + "/saves";
  MakeDirs(s_system_dir);
  Saves::Init(App::DATA_DIR);
  BuildVariables();
  s_frame = nullptr;
  s_frame_w = s_frame_h = 0;
  s_texture_w = s_texture_h = 0;
  s_texture_scaled = false;
  s_buttons.fill(0);

  retro_set_environment(Environment);
  retro_set_video_refresh(VideoRefresh);
  retro_set_audio_sample(AudioSample);
  retro_set_audio_sample_batch(AudioBatch);
  retro_set_input_poll(InputPoll);
  retro_set_input_state(InputState);
  retro_init();
  retro_game_info info = {};
  info.path = game.path.c_str();
  info.data = rom.data();
  info.size = rom.size();
  if (!retro_load_game(&info))
  {
    *error = "Snes9x couldn't load this ROM";
    retro_deinit();
    return Result::Failed;
  }
  retro_system_av_info av = {};
  retro_get_system_av_info(&av);
  s_sample_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 32040.0;
  const double game_fps = av.timing.fps > 0 ? av.timing.fps : 60.0988;
  LogF("game: %.4f FPS, %.0f Hz sound, %s", game_fps, s_sample_rate,
       retro_get_region() == RETRO_REGION_PAL ? "PAL" : "NTSC");
  LogMemory("game loaded");
  retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  bool multitap = Settings::GetOn("multitap", game.id);
  retro_set_controller_port_device(1, multitap ? DEVICE_MULTITAP : RETRO_DEVICE_JOYPAD);
  LoadSram();

  IngameMenu menu;
  menu.Init(&display, regular, bold);
  menu.SetGame(App::DATA_DIR, game.id, game.title);
  menu.CheatList().ApplyToCore();
  if (!menu.CheatList().Entries().empty())
    LogF("cheats: %zu codes", menu.CheatList().Entries().size());

  if (Settings::GetOn("resume", game.id) && Saves::Modified(Saves::StatePath(game.id, 0)))
  {
    if (LoadState(0))
      menu.Toast("Continuing where you left off");
  }

  GameAudio audio;
  audio.Start(Settings::GetInt("audio_buffer", game.id));
  audio.SetVolume(Settings::GetInt("volume", game.id));
  s_audio = &audio;

  Pads pads;
  pads.Open();
  Controls::Layout layout = Controls::LayoutFor(game.id);

  Stats stats;
  auto last = Clock::now();
  double schedule = 0.0;  // frames owed
  int skip_counter = 0;
  float sram_timer = 0.0f;
  bool slow_last = false;
  Result result = Result::Menu;
  bool quit = false;
  uint32_t menu_pad_held = 0;
  float memory_log = 0.0f;
  while (!quit)
  {
    const auto now = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.25f);
    last = now;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

    pads.Read(dt);
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
        menu.Open();
        audio.SetMuted(true);
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
        retro_reset();
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
        audio.SetVolume(Settings::GetInt("volume", game.id));
        const bool want_multitap = Settings::GetOn("multitap", game.id);
        if (want_multitap != multitap)
        {
          multitap = want_multitap;
          retro_set_controller_port_device(1, multitap ? DEVICE_MULTITAP : RETRO_DEVICE_JOYPAD);
        }
      }
      if (menu.TakeCheatsChanged())
        menu.CheatList().ApplyToCore();
      if (!menu.IsOpen())
      {
        audio.Clear();
        audio.SetMuted(false);
        layout = Controls::LayoutFor(game.id);
        schedule = 0.0;
      }
    }
    if (quit)
      break;

    // ---- Emulation ----
    const auto emu_start = Clock::now();
    if (!menu.IsOpen())
    {
      // Buttons of every player (the in-game menu combo isn't passed on).
      bool fast = false;
      for (int i = 0; i < MAX_PLAYERS; i++)
      {
        const auto& pad = pads.pads[i];
        s_buttons[i] = pad.ok && !touring ? Controls::SnesButtons(layout, pad.data) : 0;
        if (i == 0 && pad.ok && !touring && layout.fast_forward && (pad.data.buttons & layout.fast_forward))
          fast = true;
      }
      // Frames per TV refresh: the game's own rate (60.1 FPS is run at the TV's 60, the sound
      // follows), times the speed setting or fast forward.
      const std::string limit = Settings::Get("fps_limit", game.id);
      double fps = game_fps;
      if (limit == "60")
        fps = 60.0;
      else if (limit == "50")
        fps = 50.0;
      double ratio = std::abs(fps / 59.94 - 1.0) < 0.012 ? 1.0 : fps / 59.94;
      ratio *= Settings::GetInt("speed", game.id) / 100.0;
      const std::string ff = Settings::Get("fast_forward", game.id);
      if (fast && ff != "off")
        ratio = std::atoi(ff.c_str());
      if (limit == "0")
        ratio = std::max(ratio, 4.0);
      schedule += ratio;
      int frames = static_cast<int>(schedule);
      schedule -= frames;
      frames = std::min(frames, 10);
      audio.SetMuted(fast && ff != "off");
      const std::string skip = Settings::Get("frameskip", game.id);
      for (int f = 0; f < frames; f++)
      {
        // Only the last frame of this refresh is drawn; frame skip leaves out more.
        bool render = f == frames - 1;
        if (render && skip == "auto")
          render = !slow_last || (skip_counter++ % 2) == 0;
        else if (render && (skip == "1" || skip == "2"))
          render = (skip_counter++ % (std::atoi(skip.c_str()) + 1)) == 0;
        s_render_video = render;
        retro_run();
        stats.emulated++;
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
      UploadPicture(display, game.id);
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
    stats.frames++;
    stats.fps_time += dt;
    stats.frame_ms[stats.at] = dt * 1000.0f;
    stats.at = (stats.at + 1) % stats.frame_ms.size();
    if (stats.fps_time >= 1.0f)
    {
      stats.fps = stats.frames / stats.fps_time;
      stats.speed = static_cast<float>(stats.emulated / stats.fps_time / game_fps * 100.0);
      stats.frames = stats.emulated = 0;
      stats.fps_time = 0.0f;
    }

    // ---- Drawing ----
    const bool vsync = Settings::Get("vsync", game.id) != "off";
    display.SetVsync(vsync);
    display.BeginFrame();
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    DrawPicture(dl, display, game.id);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    DrawStats(fg, bold, stats, Settings::GetOn("show_fps", game.id),
              Settings::GetOn("perf_overlay", game.id), audio.Fill());
    menu.Draw(fg, dt);
    ImGui::Render();
    display.EndFrame();
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
  display.SetVsync(true);
  if (Settings::GetOn("resume", game.id))
    SaveState(0);
  SaveSram(true);
  s_audio = nullptr;
  audio.Stop();
  Upscaler::Stop();
  std::vector<uint16_t>().swap(s_scaled);
  pads.Close();
  menu.ForgetPictures();
  retro_unload_game();
  retro_deinit();
  LogLine("game: ended");
  return result;
}
}  // namespace Emulator
