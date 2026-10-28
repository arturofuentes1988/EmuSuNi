// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Settings.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "Log.h"

namespace Settings
{
namespace
{
// The frames every console's Bezel offers (00.63), after its own: the Retro TV and six more (Emulator.cpp BezelPath:
// assets/xmb/bezels/tv.png and frame-<value>.png, each with windows for wide and tall pictures).
#define EMUSUNI_SHARED_FRAMES                                                                                        \
  {"tv", "Retro TV", "An old wooden tube TV around the picture."},                                                  \
      {"cabinet", "Arcade Cabinet", "An arcade cabinet's sides: their art and trim, the stick and the buttons."},   \
      {"minimal", "Dark Minimal", "A plain dark frame with a thin edge: nothing but the game."},                    \
      {"neon", "Neon", "Glowing neon tubes and a retro grid on black."},                                             \
      {"wood", "Wood Panel", "Dark wood and black ribs, as on the consoles of the 1970s."},                          \
      {"carbon", "Carbon", "Carbon fibre with a brushed metal edge."},                                              \
      {"stars", "Starfield", "Deep space: stars and a faint nebula."}
// (00.80) The game's own bezel from The Bezel Project (src/BezelProject.h: Settings -> Game Bezels downloads them;
// Emulator.cpp BezelPath, LoadBezel), on every console it has bezels for: all but the PSP.
#define EMUSUNI_GAME_BEZEL                                                                                           \
  {"game", "Per Game (The Bezel Project)",                                                                          \
   "The game's own bezel, made by The Bezel Project, once downloaded (Settings -> Game Bezels). A game without one, " \
   "or a picture of another shape, shows the console's own."}
// The categories and their order follow Dolphin for PS4 (Video, Graphics, Performance, System);
// Emulation holds the consoles' own options. A setting for one console only says which (systems).
const std::vector<Setting> SETTINGS = {
    // Video
    // Four levels for every console. Flycast's consoles (the Dreamcast, the NAOMI, the Atomiswave: one fixed level from
    // 00.53 to 00.67) have them again since 00.68: their resolution and texture sharpness (Emulator.cpp
    // BuildDcVariables), since 00.69 the GPU Upscaler too (Ultra sharpened). The 3DS (its own resolution only, 1x, from
    // 00.64 to 00.70) has them again since 00.71: 3DS Resolution and the GPU Upscaler, as the other 3D consoles. The
    // values: PRESETS; what a level is on one console's or game's page: QualityHelp (these choices' texts are for a list
    // of every console's settings).
    {"quality", "Quality", "Video",
     {{"custom", "Custom Quality", "Your own choices (they match none of the four levels)."},
      {"low", "Low (fastest)",
       "Each console's own resolution (the N64, the PlayStation, the PSP, the DS, the 3DS and the Dreamcast at 1x), "
       "no upscaler and no filters."},
      {"medium", "Medium (balanced)",
       "Smooth Edges; the N64, the PlayStation, the PSP, the 3DS and the Dreamcast at 2x."},
      {"high", "High (HD)",
       "Pixel Art HD (the 3D consoles: Smooth Edges); the N64 at 4x, the PlayStation, the PSP, the 3DS and the "
       "Dreamcast at 3x."},
      {"ultra", "Ultra (maximum)",
       "AI (the 3D consoles: sharpened, Natural HD); the N64, the PlayStation, the PSP and the 3DS at 4x, the "
       "Dreamcast at 3x with the sharpest textures."}},
     "custom", true,
     "The picture in one choice: Low is the console's own; each level above is sharper and more work for the PS4. "
     "Changing one of its settings makes it Custom.",
     FOR_ALL},
    {"n64_resolution", "N64 Resolution", "Video",
     {{"1", "Native (240p)", "The N64's own picture: the smoothest."},
      {"2", "2x (480p)", "Twice the size: sharper 3D; heavy scenes can slow down."},
      {"4", "4x (960p)", "Four times (about Full HD): the sharpest; heavy games can slow down."}},
     "1", false, "The size the GPU draws N64 games at (ParaLLEl-RDP). Native is the smoothest.", FOR_N64},
    {"ps1_resolution", "PS1 Resolution", "Video",
     {{"1", "Native (240p)", "The PlayStation's own picture."},
      {"2", "2x (480p)", "Twice the size: sharper 3D."},
      {"3", "3x (720p)", "Three times: for 720p TVs."},
      {"4", "4x (960p)", "Four times (about Full HD): the sharpest 3D."}},
     "1", true, "The size the GPU draws PlayStation games at: real HD 3D, not an enlarged picture.", FOR_PS1},
    {"aspect", "Aspect Ratio", "Video",
     {{"auto", "Auto (4:3)", "As the console looked on a TV of the time."},
      {"pixel", "Pixel Perfect",
       "Square pixels, exactly as the console draws them: 8:7 on the Super Nintendo and NES (a little "
       "narrower), wider than 4:3 in most Mega Drive games, the same as 4:3 on the Master System."},
      {"wide", "16:9", "Stretched to a widescreen TV (everything looks wider)."},
      {"stretch", "Stretch", "Fills the whole screen, whatever its shape."}},
     "auto", true,
     "The picture's shape on the TV. 4:3 is how the consoles looked on a TV; the Game Boys keep their "
     "screen's own shape; an arcade game has its board's (a vertical game stands upright)."},
// The DS's and the 3DS's two screens (00.80, the owner: "dejar para ambas consolas las mismas opciones de
// configuración del tamaño y lugar de las pantallas"): one set of settings, the same names, choices, order and texts on
// both consoles (this macro), each console with values of its own (nds_*, 3ds_*: a DS game and a 3DS game can be shown
// differently), drawn one way for both (src/DualScreens.h: the cores give their screens side by side, the app places
// them, so the touchpad's place on the touch screen never changes). One Above the Other is the default for both - the
// 3DS's since 00.64, the DS's since 00.80 (Side by Side before). Until 00.79 the DS had Side by Side, Big Screen ("big",
// now Large Screen + Small at a third, on the right), One Above the Other and One Screen ("single", now Top Screen Only),
// the 3DS Azahar's own layouts (its Big Top Screen a quarter, at the bottom right: Load's one-time change keeps that).
// Every one is shown at once: the screens are placed by the app, the cores never change their picture.
#define EMUSUNI_SCREEN_SETTINGS(PREFIX, SYSTEMS)                                                                       \
  {PREFIX "layout", "Screen Layout", "Video",                                                                          \
   {{"stack", "One Above the Other", "The top screen over the touch screen, as on the console."},                      \
    {"side", "Side by Side", "The two screens next to each other, the top screen on the left."},                       \
    {"large", "Large Screen + Small",                                                                                  \
     "The top screen big, the touch screen small beside it or in a corner: Small Screen Size and Small Screen "        \
     "Position say how."},                                                                                             \
    {"top", "Top Screen Only",                                                                                         \
     "Only the top screen, as big as the TV allows. The touchpad still touches the touch screen, unseen."},            \
    {"bottom", "Touch Screen Only", "Only the touch screen, as big as the TV allows."}},                               \
   "stack", true,                                                                                                      \
   "How the two screens are shown. Swap Screens (or R3 in the game) trades their places. The DualShock 4's "           \
   "touchpad is always the touch screen, wherever it is.",                                                             \
   SYSTEMS},                                                                                                           \
  {PREFIX "swap", "Swap Screens", "Video",                                                                             \
   {{"off", "Off", "The top screen first: above, on the left or big."},                                                \
    {"on", "On",                                                                                                       \
     "The touch screen first: above, on the left or big (with one screen only, the other one is shown)."}},            \
   "off", true, "Trades the two screens' places. R3 swaps them in the game too, for as long as it runs.",              \
   SYSTEMS},                                                                                                           \
  {PREFIX "small_size", "Small Screen Size", "Video",                                                                  \
   {{"2", "Half (1/2)", "The small screen half the size of the big one."},                                             \
    {"3", "A Third (1/3)", "The small screen a third of the size of the big one."},                                    \
    {"4", "A Quarter (1/4)", "The small screen a quarter of the size of the big one."}},                               \
   "3", true, "Large Screen + Small: how big the small screen is next to the big one.", SYSTEMS},                      \
  {PREFIX "small_position", "Small Screen Position", "Video",                                                          \
   {{"right", "Right", "Beside the big screen, on its right, in the middle."},                                         \
    {"right_bottom", "Bottom Right", "On the big screen's right, in the bottom corner."},                              \
    {"right_top", "Top Right", "On the big screen's right, in the top corner."},                                       \
    {"left", "Left", "Beside the big screen, on its left, in the middle."},                                            \
    {"left_bottom", "Bottom Left", "On the big screen's left, in the bottom corner."},                                 \
    {"left_top", "Top Left", "On the big screen's left, in the top corner."},                                          \
    {"below", "Below", "Under the big screen, in the middle."},                                                        \
    {"above", "Above", "Over the big screen, in the middle."}},                                                        \
   "right", true,                                                                                                      \
   "Large Screen + Small: where the small screen goes, beside the big one - in the middle of a side, in a "            \
   "corner, below or above it.",                                                                                       \
   SYSTEMS},                                                                                                           \
  {PREFIX "gap", "Gap Between Screens", "Video",                                                                       \
   {{"none", "None", "The screens touch each other."},                                                                 \
    {"thin", "Thin (1/16)", "A thin black line between them: a sixteenth of a screen's height."},                      \
    {"small", "Small (1/8)", "A little black space between them: an eighth of a screen's height."},                    \
    {"medium", "Medium (1/4)", "A quarter of a screen's height between them."},                                        \
    {"large", "Large (1/2)", "Half a screen's height between them: about the DS's own hinge."}},                       \
   "none", true,                                                                                                       \
   "Black space between the two screens, as the hinge left between them on the console (not with one screen "          \
   "only).",                                                                                                           \
   SYSTEMS}
    EMUSUNI_SCREEN_SETTINGS("nds_", FOR_NDS),
    EMUSUNI_SCREEN_SETTINGS("3ds_", FOR_3DS),
#undef EMUSUNI_SCREEN_SETTINGS
    {"ps1_widescreen", "PS1 Widescreen", "Video",
     {{"off", "Off (4:3)", "As the PlayStation drew it."},
      {"on", "On (16:9)", "The 3D drawn wider to fill a widescreen TV (the 2D and some games stay 4:3)."}},
     "off", true, "Shows more of the 3D world on a 16:9 TV (a widescreen hack: some games glitch).", FOR_PS1},
    {"psp_resolution", "PSP Resolution", "Video",
     {{"1", "Native (272p)", "The PSP's own 480x272 picture."},
      {"2", "2x (544p)", "Twice the size: sharper 3D."},
      {"3", "3x (816p)", "Three times: for 720p TVs and up."},
      {"4", "4x (1088p)", "Four times: Full HD, the sharpest 3D."}},
     "1", true, "The size the GPU draws PSP games at: real HD 3D, not an enlarged picture.", FOR_PSP},
    // (Hidden from 00.53 to 00.67, the Dreamcast at 640x480 whatever the Quality; 00.68: the levels' 1x, 2x, 3x. No 4x:
    // the PS4 shows a 1080p picture, and 2560x1920 made that small only shimmers more than 3x does.)
    {"dc_resolution", "Dreamcast Resolution", "Video",
     {{"1", "Native (480p)", "The Dreamcast's own 640x480 picture: the lightest."},
      {"2", "2x (960p)", "Twice the size (1280x960): sharper 3D."},
      {"3", "3x (1440p)",
       "Three times (1920x1440), made smaller for a 1080p TV: the sharpest 3D, the smoothest edges."}},
     "1", false,
     "The size the GPU draws Dreamcast, NAOMI and Atomiswave games at: real HD 3D, not an enlarged picture. The size "
     "changes at the next start.",
     FOR_FLYCAST},
    // (Hidden from 00.64 to 00.70, the 3DS at 1x whatever the Quality; 00.71: the levels' 1x, 2x, 3x, 4x. A bigger size
    // costs the emulation thread nothing - PC, Hey! Pikmin and Super Mario 3D Land at 1x to 4x: the same CPU time a
    // frame - but a game that reads its picture back waits for the GPU's work, which grows with the size.)
    {"3ds_resolution", "3DS Resolution", "Video",
     {{"1", "Native (240p)", "The 3DS's own screens, 400x240 and 320x240: the lightest for the PS4."},
      {"2", "2x (480p)", "Twice the size (800x480): sharper 3D."},
      {"3", "3x (720p)", "Three times (1200x720): sharper 3D still."},
      {"4", "4x (960p)", "Four times (1600x960, about Full HD): the sharpest 3D."}},
     "1", false,
     "The size the GPU draws 3DS games at: real HD 3D, not an enlarged picture. A bigger size is more work for the "
     "GPU only, but a game that reads its picture back (a few do, for effects) waits for it and can slow down.",
     FOR_3DS},
    {"resolution", "Resolution", "Video",
     {{"1", "Native (224p)", "The console's own 256x224 picture, enlarged by the TV (the fastest)."},
      {"2", "2x (448p)", "Redrawn at 512x448 before it is fitted to the TV."},
      {"3", "3x (720p)", "Redrawn at 768x672: for 720p TVs."},
      {"4", "4x (900p)", "Redrawn at 1024x896."},
      {"5", "5x (1080p)", "Redrawn at 1280x1120: the closest to a 1080p (Full HD) TV."},
      {"6", "6x (1440p)", "Redrawn at 1536x1344, then made smaller for the TV (a little smoother)."}},
     "1", true,
     "Redraws the game bigger with the Upscaling Filter (Graphics) before it is fitted to the TV: "
     "3x for 720p, 5x for 1080p. Uses CPU time: lower it if a game slows down.", FOR_PIXELS},
    {"scaling", "Picture Size", "Video",
     {{"fit", "Fit to Screen", "As big as the TV allows."},
      {"integer", "Integer Scale (sharpest)",
       "The biggest whole multiple of the console's lines (4x on a 1080p TV): every pixel the same "
       "size, with a black border."},
      {"2", "2x", "Twice the console's size, small in the middle of the TV."},
      {"3", "3x", "Three times the console's size."},
      {"4", "4x", "Four times the console's size."}},
     "fit", true,
     "How big the picture is drawn. Integer Scale keeps every pixel the same size (no uneven "
     "rows)."},
    {"zoom", "Letterbox Zoom", "Video",
     {{"off", "Off", "The whole picture."},
      {"105", "105%", "A little bigger: the edges are cut."},
      {"110", "110%", "Bigger: more of the edges are cut."},
      {"120", "120%", "Much bigger: fills a widescreen TV more, the edges cut."}},
     "off", true, "Zooms in a little to fill more of the TV (the edges are cut)."},
    {"snes_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Super Nintendo", "Dark grey with the purple line, the D-pad and the four buttons."},
      {"sfc", "Super Famicom", "The Japanese console's light grey and its four colours."},
      {"purple", "Purple Stars", "Deep purple with pixel stars."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/snes/ with the "
       "ROM's name (The Bezel Project's names), or bezels/snes.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SNES},
    {"nes_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "NES", "The NES's light grey and black, with POWER and RESET."},
      {"famicom", "Famicom", "The Famicom's cream, dark red and gold, with the controller's D-pad and buttons."},
      {"black", "Black and Red", "Black with red lines and the controller's buttons."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/nes/ with the "
       "ROM's name (The Bezel Project's names), or bezels/nes.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_NES},
    {"n64_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo 64", "Charcoal grey with the four colours and the stick."},
      {"colors", "Four Colours", "The four colours of the Nintendo 64's logo, as bands."},
      {"atomic", "Atomic Purple", "See-through purple, like the Atomic Purple console."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/n64/ with the "
       "ROM's name (The Bezel Project's names), or bezels/n64.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_N64},
    {"gb_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Game Boy", "The first Game Boy's grey, its screen frame, the D-pad and the A and B buttons."},
      {"pocket", "Game Boy Pocket", "The Pocket's silver, slimmer frame and black buttons."},
      {"green", "Dot Matrix", "The green of the Game Boy's screen, with its dots."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gb/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gb.png for every game."}},
     "console", true, "The handheld around the picture, see-through where the game goes.", FOR_GB},
    {"gbc_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Berry", "The Game Boy Color in berry, with COLOR in its five colours."},
      {"teal", "Teal", "The Game Boy Color in teal."},
      {"atomic", "Atomic Purple", "See-through purple, the insides showing."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gbc/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gbc.png for every game."}},
     "console", true, "The handheld around the picture, see-through where the game goes.", FOR_GBC},
    {"gba_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Indigo", "The Game Boy Advance in indigo, with L and R."},
      {"glacier", "Glacier", "See-through ice blue, the insides showing."},
      {"black", "Black", "A black Game Boy Advance with silver lines."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gba/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gba.png for every game."}},
     "console", true, "The handheld around the picture, see-through where the game goes.", FOR_GBA},
    {"ps1_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "PlayStation", "The PlayStation's grey, with the four symbols and its disc lid."},
      {"psone", "PSone", "The PSone's white and silver, round and slim."},
      {"dark", "Midnight", "Black and deep blue, with the four symbols."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/ps1/ with the "
       "ROM's name (The Bezel Project's names), or bezels/ps1.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_PS1},
    // The Sega consoles (00.63: a design of their own each).
    {"sg1000_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "SG-1000", "Black with red and white lines, the joystick and buttons 1 and 2."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/sg1000/ with the "
       "ROM's name (The Bezel Project's names), or bezels/sg1000.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SG1000},
    {"sms_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Master System", "Black with red lines and the ribs on top, the pad and buttons 1 and 2."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/sms/ with the "
       "ROM's name (The Bezel Project's names), or bezels/sms.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SMS},
    {"gg_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Game Gear", "The handheld in black around its screen: the D-pad, START and buttons 1 and 2."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gg/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gg.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_GG},
    {"md_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Genesis", "Black with the round grooves, the D-pad and the A, B and C buttons."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/md/ with the "
       "ROM's name (The Bezel Project's names), or bezels/md.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_MD},
    // The Arcade: the shared frames (its own: the Arcade Cabinet), or the user's own (The Bezel Project names its
    // arcade bezels after the romsets, as the games' files are: mslug.png).
    {"arcade_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/arcade/ with the "
       "romset's name (The Bezel Project's names: mslug.png), or bezels/arcade.png for every game."}},
     "cabinet", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_ARCADE},
    // 00.63: the PSP's, the DS's and the 3DS's (their pictures are wide: a frame makes them smaller, said in the help;
    // the DS's and the 3DS's have a window for each way their screens are shown), the Dreamcast's, the NAOMI's and the
    // Atomiswave's.
    {"psp_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "PSP", "The handheld in glossy black around its wide screen: the D-pad, the stick, the four symbols."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/psp/ with the "
       "game's name, or bezels/psp.png for every game."}},
     "console", true,
     "A frame around the picture, see-through where the game goes. The PSP's picture is as wide as the TV: in a frame "
     "it is made smaller (about 83%).",
     FOR_PSP},
    {"nds_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo DS", "The DS in white around its screens, made for each way DS Screens shows them."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/nds/ with the "
       "game's name, or bezels/nds.png for every game."}},
     "console", true,
     "A frame around the screens, see-through where they go. Side by side or with the big screen, the picture is made "
     "smaller to fit in it.",
     FOR_NDS},
    {"3ds_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo 3DS", "The 3DS in aqua blue around its screens, made for each way 3DS Screens shows them."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/3ds/ with the "
       "game's name, or bezels/3ds.png for every game."}},
     "console", true,
     "A frame around the screens, see-through where they go. Side by side, with the big top screen or the top "
     "screen only, the picture is made smaller to fit in it.",
     FOR_3DS},
    {"dc_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Dreamcast", "The console's white, its round lid and the controller's four coloured buttons."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/dc/ with the "
       "game's name, or bezels/dc.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_DC},
    {"naomi_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "NAOMI Cabinet", "A white arcade cabinet with gold trim, its stick and six buttons."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/naomi/ with the "
       "romset's name (mvsc2.png), or bezels/naomi.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_NAOMI},
    {"atomiswave_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Atomiswave Cabinet", "A black arcade cabinet with green lights, its stick, buttons and cartridge."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/atomiswave/ with the "
       "romset's name (kofxi.png), or bezels/atomiswave.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_ATOMISWAVE},
    // 00.69: the Saturn's own (scripts/make-bezels.py saturn-console; a build without the Saturn shows it nowhere).
    {"saturn_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Saturn", "Charcoal black with the round lid, the pad's round D-pad and its six buttons."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/saturn/ with the "
       "game's name, or bezels/saturn.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SATURN},
    // 00.82: the PC Engine's own (scripts/make-bezels.py pce-console).
    {"pce_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "PC Engine", "White, the HuCard in its slot and the orange line, the pad's II and I."},
      EMUSUNI_SHARED_FRAMES,
      EMUSUNI_GAME_BEZEL,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/pce/ with the "
       "ROM's name, or bezels/pce.png for every game."}},
     "console", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_PCE},
    {"overscan", "Crop Overscan", "Video",
     {{"enabled", "On", "224 lines: the empty lines at the top and bottom are hidden."},
      {"disabled", "Off", "Every line (some games show a black stripe or glitches at the edges)."},
      {"auto", "Auto", "Hidden when the game doesn't draw on them."}},
     "enabled", true, "Hides the lines at the top and bottom that TVs hid (224 lines are shown).", FOR_RETRO},
    {"fps_limit", "Frame Rate Limit", "Video",
     {{"auto", "Auto (game speed)", "NTSC games at 60 and PAL games at 50 frames per second."},
      {"60", "60 FPS", "Every game at 60 (PAL games run faster than on their console)."},
      {"50", "50 FPS", "Every game at 50 (NTSC games run slower)."},
      {"0", "Unlimited", "As fast as the PS4 can (up to 4x)."}},
     "auto", true,
     "Auto runs NTSC games at 60 and PAL games at 50 frames per second, as on the console."},
    {"vsync", "V-Sync", "Video",
     {{"on", "On", "Smooth movement, no tearing."},
      {"off", "Off", "A little less input delay; the picture may tear."}},
     "on", true, "Waits for the TV's refresh (no tearing). Off can lower the input delay a little."},
    {"show_fps", "FPS Counter", "Video", {{"off", "Off"}, {"on", "On"}}, "off", true,
     "Frames and emulation speed in the corner."},
    // Graphics
    {"upscaler", "Upscaling Filter", "Graphics",
     {{"xbrz", "xBRZ (smooth)", "Rounded edges and clean lines: the most HD look."},
      {"hqx", "HQx", "Softer edges, closer to the original pixels (up to 4x)."},
      {"pixels", "Sharp Pixels", "Bigger square pixels: crisp and even, like the original."}},
     "xbrz", true,
     "How Resolution redraws the pixels: xBRZ and HQx round the edges for an HD look; Sharp "
     "Pixels keeps them square. HQx goes up to 4x.", FOR_PIXELS},
    {"gpu_filter", "GPU Upscaler", "Graphics",
     {{"off", "Off", "The picture as Resolution and the Upscaling Filter make it."},
      {"edges", "Smooth Edges",
       "Only the jagged edges are smoothed (anti-aliasing: MMPX and SMAA); colours, textures and the "
       "pixels' shapes stay as they are. Light on the GPU. Quality Medium uses it."},
      {"natural", "Natural HD",
       "Smooth Edges, then drawn in HD at the TV's resolution and sharpened (AMD FSR): clearly sharper, "
       "still natural, never painted."},
      {"natural_ai", "Natural AI",
       "Natural HD's clean edges with an AI network (Anime4K) mixed into the picture, so textures and "
       "dithering stay: the smoothest look without the waxy one."},
      // (00.69: the levels' High and Ultra, Quality's steps for pixel art; a 3D picture drawn by the GPU gets Smooth
      // Edges and Natural HD from them, Emulator.cpp GpuMode.)
      {"pixel", "Pixel Art HD (xBRZ)",
       "Pixel art redrawn by the GPU at the TV's resolution: rounded, clean edges without blur. Quality High uses it; "
       "a 3D picture drawn by the GPU gets Smooth Edges."},
      {"ai", "AI (Anime4K)",
       "xBRZ, then a neural network (Anime4K) at full strength cleans every edge and curve: the smoothest HD look "
       "for pixel art. Quality Ultra uses it; a 3D picture drawn by the GPU gets Natural HD (the network would only "
       "make it smaller)."},
      {"fsr", "Smooth HD (AMD FSR)",
       "AMD FidelityFX Super Resolution: smooth upscaling with sharp edges, made for 3D (the N64 at "
       "Native and 2x; at 4x it sharpens)."},
      {"sharpen", "HD Sharpen (AMD RCAS)",
       "The picture as it is, made crisper by AMD's sharpening (RCAS): for the N64 at 4x and pictures "
       "already made bigger."}},
     "off", true,
     "Draws the picture with the GPU's filters at the TV's resolution. Every filter but HD Sharpen "
     "starts from the console's own picture: Resolution and the Upscaling Filter are left out.",
     FOR_ALL /* Flycast's consoles too since 00.69 (a vertical NAOMI or Atomiswave game: none), the 3DS since 00.71 */},
    {"filter", "Texture Filtering", "Graphics",
     {{"sharp", "Sharp (nearest)", "Every pixel crisp (at Native some pixels can look wider than others)."},
      {"smooth", "Smooth (linear)", "Soft, blended pixels."}},
     "sharp", true,
     "Sharp keeps the pixels crisp; Smooth softens them. Above Native resolution the picture is "
     "always smooth."},
    {"n64_aa", "N64 Anti-Aliasing", "Graphics",
     {{"on", "On (the N64's)", "Smooth 3D edges, as the N64's video chip made them."},
      {"off", "Off", "Jagged 3D edges, a little sharper: the picture before the N64's filter."}},
     "on", true, "The N64's own edge smoothing (its video chip). Costs almost nothing.", FOR_N64},
    {"n64_dither_filter", "N64 Dither Filter", "Graphics",
     {{"on", "On (the N64's)", "Smooth colours: the N64's filter hides the dots of its 16-bit colours."},
      {"off", "Off", "The dots stay: grainy skies, fog and shadows."}},
     "on", true, "The N64 drew its 16-bit colours with a pattern of dots; its video chip blended them away.",
     FOR_N64},
    {"n64_2d", "N64 2D Graphics", "Graphics",
     {{"native", "Exact", "Text, icons and 2D backgrounds as the N64 drew them: blocky, without seams."},
      {"hd", "Smooth (HD)",
       "Text, icons and 2D backgrounds at the N64 Resolution: smoother, but many games show lines or squares "
       "where their pieces meet (Zelda's pre-drawn rooms, ISS 64's intro)."}},
     "native", true, "How the HUD, text and 2D pictures are drawn above Native resolution.", FOR_N64},
    {"n64_texture_lod", "N64 Distant Textures", "Graphics",
     {{"sharp", "Sharp (HD)", "Far textures as sharp as the N64 Resolution allows."},
      {"native", "As the N64",
       "Far textures as blurry as on the N64: for the few games that swap textures with the distance."}},
     "sharp", true, "The texture detail of far surfaces above Native resolution.", FOR_N64},
    {"n64_fb_effects", "N64 Screen Effects", "Graphics",
     {{"exact", "Exact", "Blurs, pause screens and photos made from the game's own picture, at the N64's size."},
      {"smooth", "Smooth (SSAA)", "Those effects made from the sharp picture: smoother; a few games glitch."}},
     "exact", false,
     "Effects some games make from their own picture (pause backgrounds, motion blur), above Native "
     "resolution.", FOR_N64},
    {"ps1_pgxp", "PS1 Geometry (PGXP)", "Graphics",
     {{"off", "As the PlayStation", "Polygons that shake and textures that bend, as on the console."},
      {"on", "Steady (PGXP)", "Steady polygons and straight textures; costs CPU time (heavy games can slow down)."}},
     "off", true, "Fixes the PlayStation's shaking 3D and warped textures.", FOR_PS1},
    {"ps1_texture_filter", "PS1 Texture Filtering", "Graphics",
     {{"Nearest", "Sharp (as the PlayStation)", "Every texture pixel crisp."},
      {"Bilinear", "Smooth (Bilinear)", "Soft, blended textures."},
      {"JINC2", "JINC2", "Smooth textures that keep sharp edges."},
      {"xBR", "xBR", "Textures redrawn with rounded edges, like xBRZ."}},
     "Nearest", true, "How the 3D's textures are drawn.", FOR_PS1},
    {"ps1_colors", "PS1 Colours", "Graphics",
     {{"dither", "As the PlayStation (dithered)", "Its 16-bit colours with their dot pattern."},
      {"true", "True Colour (smooth)", "Smooth 24-bit colours, without the dots: cleaner skies and shadows."}},
     "dither", true, "The PlayStation drew its colours with a pattern of dots; True Colour drops it.", FOR_PS1},
    {"ps1_antialiasing", "PS1 Anti-Aliasing", "Graphics",
     {{"1", "Off", "The 3D's edges as they are drawn."},
      {"2", "2x MSAA", "Smoother 3D edges."},
      {"4", "4x MSAA", "Much smoother 3D edges (more GPU time)."},
      {"8", "8x MSAA", "The smoothest edges (the most GPU time)."}},
     "1", true, "Smooths the jagged edges of the 3D (multisampling, on the GPU).", FOR_PS1},
    {"psp_anisotropy", "PSP Texture Sharpness", "Graphics",
     {{"0", "Off", "Floors and walls blur into the distance, as on the PSP."},
      {"4", "4x", "Sharper textures far away."},
      {"8", "8x", "Sharper still."},
      {"16", "16x", "The sharpest textures far away."}},
     "0", true, "How sharp the textures of far, slanted surfaces stay (anisotropic filtering, on the GPU).",
     FOR_PSP},
    {"psp_texture_shader", "PSP Texture Upscaling", "Graphics",
     {{"off", "Off", "The games' own textures."},
      {"2xBRZ", "2xBRZ", "Textures redrawn twice as big with rounded edges."},
      {"4xBRZ", "4xBRZ", "Textures redrawn four times as big: smoother, for 2D art."},
      {"MMPX", "MMPX", "Textures redrawn keeping the pixel art's style."}},
     "off", true, "Redraws the games' textures bigger on the GPU (it changes how the art looks).", FOR_PSP},
    {"psp_antialiasing", "PSP Anti-Aliasing", "Graphics",
     {{"1", "Off", "The 3D's edges as they are drawn."},
      {"2", "2x MSAA", "Smoother 3D edges."},
      {"4", "4x MSAA", "Much smoother 3D edges (more GPU time)."},
      {"8", "8x MSAA", "The smoothest edges (the most GPU time)."}},
     "1", true,
     "Smooths the jagged edges of the 3D (multisampling, on the GPU). Changed while a game runs, its 3D is set up "
     "again: it can stutter for a few seconds.",
     FOR_PSP},
    {"dc_anisotropy", "Dreamcast Texture Sharpness", "Graphics",
     {{"off", "Off", "Far textures blur, as on the Dreamcast."},
      {"4", "4x", "Sharper textures far away."},
      {"8", "8x", "Sharper still."},
      {"16", "16x", "The sharpest textures far away."}},
     "4", false,
     "How sharp the textures of far, slanted surfaces stay (anisotropic filtering, on the GPU). It changes at the next "
     "game start.",
     FOR_FLYCAST},
    {"dc_sorting", "Dreamcast Transparency", "Graphics",
     {{"triangle", "Normal (per triangle)",
       "Transparent surfaces sorted per triangle: fast; a few games show some in the wrong order."},
      {"pixel", "Accurate (per pixel)",
       "Sorted per pixel, as the Dreamcast did: heavier and about 0.5 GB of GPU memory. Experimental: on "
       "the PS4 the picture can come out in wrong colours (Crazy Taxi did)."}},
     "triangle", false, "How overlapping transparent surfaces (smoke, glass, water) are drawn.",
     0 /* hidden: per triangle at every Quality level (Emulator.cpp BuildDcVariables) */},
    {"psp_frame_rate", "PSP Frame Rate (God of War)", "Performance",
     {{"auto", "Auto", "60 FPS while the PS4 keeps up; 30 for the rest of the game once it can't (no slowdown)."},
      {"60", "60 FPS", "The smoothest; heavy fights slow the game down."},
      {"30", "30 FPS", "Half the work (PPSSPP's hack for weak hardware): a steady full speed."}},
     "auto", true, "God of War's frame rate on the PS4 (the God of War games only; others keep theirs).", FOR_PSP},
    {"3ds_clock", "3DS CPU Clock", "Performance",
     {{"100", "100% (as the 3DS)", "The 3DS's own speed."},
      // (00.70: a lower clock only stretches the waits of a game that waits for its next picture - Hey! Pikmin and
      // Super Mario 3D Land on the PC: the same work at 100, 75 and 50% - so the texts no longer promise less work.)
      {"75", "75%", "The game sees a slower 3DS. Less work for the PS4 only in a game that keeps the processor busy "
                    "without a break; most wait for their next picture and cost the same."},
      {"50", "50%", "The game sees a 3DS at half speed: a game that needs the speed slows down or stutters."}},
     "100", true,
     "How fast the emulated 3DS's processor runs. Most games wait for their next picture, so a lower clock seldom "
     "saves the PS4 any work (none in the games tried); a game that needs the speed slows down or freezes.",
     FOR_3DS},
    // The Dreamcast's processor (Flycast's SH4 clock, Emulator.cpp BuildDcVariables: read as the game starts, the core
    // counts it into each block of code as it compiles it) and its sound DSP (off, as in Flycast's own default). The
    // NAOMI and the Atomiswave have the same processor and sound chip (00.63: the same two settings, This Console
    // keeps each machine's own).
    {"dc_clock", "Dreamcast CPU Clock", "Performance",
     {{"100", "100% (as the Dreamcast)", "The Dreamcast's own speed (200 MHz)."},
      {"90", "90%", "The game sees a slower Dreamcast (180 MHz): less work for the PS4 where the game keeps up."},
      {"80", "80%", "160 MHz: less work still; more games slow down."},
      {"70", "70%", "140 MHz: the lightest; the games that need the speed slow down."}},
     "100", false,
     "How fast the emulated Dreamcast's processor runs (the NAOMI's and the Atomiswave's is the same one). Lower is "
     "lighter for the PS4, but some games then slow down.",
     FOR_FLYCAST},
    {"dc_dsp", "Dreamcast Sound DSP", "Performance",
     {{"off", "Off", "The sound without the DSP's effects: lighter for the PS4."},
      {"on", "On", "The Dreamcast's sound effects processor, as the games used it: more work for the PS4."}},
     "off", true,
     "The Dreamcast's sound effects processor (DSP; the NAOMI and the Atomiswave have it too). Some games' echo and "
     "reverb need it; Off is lighter for the PS4.",
     FOR_FLYCAST},
    {"ntsc_filter", "NTSC Filter", "Graphics",
     {{"disabled", "Off", "A clean picture."},
      {"composite", "Composite", "The yellow video cable: colours bleed, soft details."},
      {"s-video", "S-Video", "Sharper than composite, a little colour bleeding."},
      {"rgb", "RGB", "The best cable of the time: sharp, clean colours."},
      {"rf", "RF", "The aerial cable: the blurriest, with noise."},
      {"monochrome", "Monochrome", "A black and white TV."}},
     "disabled", true, "The look of an old TV cable (blargg's NTSC filter). Costs CPU time.", FOR_RETRO},
    {"scanlines", "Scanlines", "Graphics",
     {{"off", "Off", "No lines."},
      {"light", "Light", "Faint lines."},
      {"medium", "Medium", "Clear lines."},
      {"strong", "Strong", "Dark lines, like an arcade monitor."}},
     "off", true,
     "Dark lines between the picture's rows, like a CRT TV; on the Game Boys and the Game Gear, the grid between their "
     "screen's dots."},
    // Tube TV (00.63): the picture through an old tube TV's look on the GPU (src/shaders/crt.frag, GpuFilter's Crt mode)
    // for every console; it takes the GPU Upscaler's and Scanlines' place (Emulator.cpp GpuMode, DrawScanlines).
    {"crt", "Tube TV (CRT)", "Graphics",
     {{"off", "Off", "The picture as the other settings make it."},
      {"tv", "TV", "A living-room tube TV: curved glass, soft lines, dots of colour and a little glow."},
      {"arcade", "Arcade Monitor", "An arcade cabinet's monitor: sharper and darker lines, a slot mask, less curved."},
      {"studio", "Studio Monitor", "A flat professional monitor: crisp, thin lines and a fine aperture grille."},
      {"strong", "Intense", "Everything stronger: very curved glass, dark lines, a clear mask and more glow."}},
     "off", true,
     "An old tube TV's look on the console's own picture, drawn by the GPU (about 1 ms a frame). It takes the place "
     "of the GPU Upscaler and of Scanlines; the NTSC Filter's picture goes through it; a bezel frames it."},
    {"hires_blend", "Hi-Res Blending", "Graphics",
     {{"disabled", "Off", "The hi-res mode's columns as they are."},
      {"merge", "Merge", "Pairs of columns averaged: the see-through effects look as on a TV."},
      {"blur", "Blur", "The columns softened."}},
     "disabled", true,
     "Blends the hi-res mode's columns (pseudo-transparency in Kirby's Dream Land 3, Jurassic "
     "Park).", FOR_SNES},
    {"sprite_flicker", "Reduce Sprite Flicker", "Graphics",
     {{"disabled", "Off", "As the console: sprites flicker when too many share a line."},
      {"enabled", "On", "More sprites per line than the console could: less flicker (a few games glitch)."}},
     "disabled", true, "Draws more sprites per line than the console could (less flicker).",
     FOR_RETRO | FOR_SEGA | FOR_PCE},
    {"transparency", "Transparency Effects", "Graphics",
     {{"enabled", "On", "Water, shadows and fades as on the console."},
      {"disabled", "Off", "No colour math (to find graphic glitches)."}},
     "enabled", true, "Colour math: water, shadows, fades. Off only to diagnose glitches.", FOR_SNES},
    {"gb_palette", "Game Boy Colours", "Graphics",
     {{"gbc", "Game Boy Color", "As a Game Boy Color showed them: its own colours for Nintendo's games, green and blue for the others."},
      {"dmg", "Game Boy (green)", "The first Game Boy's green screen."},
      {"pocket", "Game Boy Pocket", "The Pocket's grey and black screen."},
      {"light", "Game Boy Light", "The Light's blue-green backlight."},
      {"gray", "Grey", "Four clean shades of grey."},
      {"sgb", "Super Game Boy",
       "As the Super Nintendo's Super Game Boy: the colours and the border the game made for it (Pokémon, "
       "Donkey Kong, Kirby...); a TV's picture, a little faster."}},
     "gbc", false, "The colours of the Game Boy's games (not those made for the Game Boy Color).", FOR_GB},
    {"lcd_colors", "Screen Colours", "Graphics",
     {{"vivid", "Vivid (as made)", "The colours the game made, bright and strong."},
      {"lcd", "Handheld Screen", "As the handheld's screen showed them: softer, more natural colours."}},
     "vivid", true,
     "The games were made bright for the handheld's dark screen; Handheld Screen shows them as they looked "
     "on it.", FOR_GBC | FOR_GBA},
    {"lcd_ghosting", "LCD Ghosting", "Graphics",
     {{"off", "Off", "Every frame as it is: the sharpest."},
      {"smart", "Smart", "Only what flickers is blended: the see-through effects games made by flickering "
       "(F-Zero, Boktai, Wave Race) look as on the handheld."},
      {"mix", "Blend Frames", "Every frame blended with the one before."},
      {"lcd", "LCD Ghosting", "Moving things leave a short trail, as on the slow screen of the handheld."}},
     "off", true,
     "The handheld's screen kept a little of the frame before; some games count on it. The Game Gear has "
     "LCD Ghosting only.",
     FOR_LCD},
    // Performance
    {"cpu_clock", "Emulated CPU Clock", "Performance",
     {{"disabled", "100%", "The console's own speed."},
      {"light", "Light Overclock", "A little faster: less slowdown, safe in most games."},
      {"compatible", "Overclock", "Faster: removes most slowdown; some games glitch."},
      {"max", "Max Overclock", "Much faster; more games glitch."}},
     "disabled", true,
     "Overclocking the console's CPU removes slowdown in busy scenes (Gradius III, Super R-Type); "
     "a few games glitch.", FOR_RETRO | FOR_SEGA},
    {"superfx_clock", "SuperFX Clock", "Performance",
     {{"100", "100%", "The chip's own speed."},
      {"150", "150%", "A faster chip: smoother 3D."},
      {"200", "200%", "Twice as fast: much smoother 3D."},
      {"300", "300%", "Three times as fast (some scenes may run too fast)."},
      {"400", "400%", "Four times as fast (some scenes may run too fast)."}},
     "100", true, "Faster SuperFX chip: smoother Star Fox, Stunt Race FX, Yoshi's Island.", FOR_SNES},
    {"n64_sync", "N64 Graphics Sync", "Performance",
     {{"fast", "Fast (asynchronous)",
       "The GPU draws while the N64 runs: much smoother on the PS4. Cruis'n USA, Resident Evil 2 and Ogre Battle 64 "
       "play Accurate anyway (with Fast the first two stay on a black screen and Ogre Battle's backgrounds break)."},
      {"accurate", "Accurate (synchronous)",
       "The N64 waits for each picture: slower. For the few games whose effects read the screen back "
       "(camera photos, some pause screens), or a game that goes black with Fast."}},
     "fast", false, "Whether the N64 waits for the GPU's pictures (ParaLLEl-RDP's Synchronous RDP).", FOR_N64},
    {"n64_cores", "N64 CPU Cores", "Performance",
     {{"fixed", "Fixed", "Each N64 thread on a PS4 core of its own: the emulation on core 2, its graphics "
       "commands on core 3."},
      {"system", "PS4's Choice", "The PS4 moves the threads between its cores, as before 00.27."}},
     "fixed", false,
     "Which PS4 cores run an N64 game's threads. Fixed gives the emulation and its graphics thread a core "
     "each, so they run at the same time.", FOR_N64},
    {"n64_picture", "N64 Picture Timing", "Performance",
     {{"later", "One Frame Later",
       "The graphics thread makes each picture while the N64 runs on: at 4x up to 3 ms less per frame; the "
       "picture is shown one frame (17-20 ms) later."},
      {"now", "At Once", "The N64 waits at every frame until its picture is made, as before."}},
     "later", false,
     "When the N64's picture is made: by the graphics thread one frame later (faster), or at once.", FOR_N64},
    {"n64_cpu_timing", "N64 CPU Timing", "Performance",
     {{"auto", "Auto", "Light for GoldenEye 007; Normal for the other games."},
      {"normal", "Normal", "The emulator's own timing: the most frames the game can draw."},
      {"light", "Light (as the N64)",
       "The N64's processor a little slower against its clock: a game that drew few frames on the console "
       "draws about as few, and the PS4 has much less to do (GoldenEye: about a third less). The game keeps "
       "its speed; loading takes longer."}},
     "auto", false,
     "How much the N64's processor does between two pictures. Light keeps heavy games (GoldenEye 007) "
     "smooth on the PS4.", FOR_N64},
    {"ps1_cd_speed", "PS1 Disc Speed", "Performance",
     {{"1", "As the PlayStation", "The console's own loading times."},
      {"2", "2x", "Loads about twice as fast."},
      {"4", "4x", "Loads much faster (a few games mind it)."},
      {"8", "8x", "The fastest (more games may mind it)."}},
     "1", false, "How fast the disc is read: shorter loading.", FOR_PS1},
    {"speed", "Emulation Speed", "Performance",
     {{"100", "100%"}, {"50", "50%"}, {"75", "75%"}, {"150", "150%"}, {"200", "200%"},
      {"300", "300%"}},
     "100", true, "The game's speed. 100% is the console's."},
    {"fast_forward", "Fast Forward", "Performance",
     {{"3", "3x"}, {"2", "2x"}, {"5", "5x"}, {"10", "10x"}, {"off", "Off", "The Fast Forward button does nothing."}},
     // (A console's or a game's page says which button: Controls::FastForwardHelp.)
     "3", true, "Speed while the Fast Forward button is held. Each console has its own button: see it in Controls."},
    {"frameskip", "Frame Skip", "Performance",
     {{"off", "Off", "Every frame drawn."},
      {"auto", "Auto", "Skips drawing a frame only when the PS4 falls behind."},
      {"1", "1", "Draws every other frame."},
      {"2", "2", "Draws one frame in three."}},
     "off", true, "Skips drawing some frames when the PS4 can't keep up (the game keeps its speed).",
     FOR_RETRO | FOR_PSP | FOR_NDS | FOR_FLYCAST | FOR_SEGA | FOR_ARCADE | FOR_PCE},
    {"frame_pacing", "N64 Frame Pacing", "Performance",
     {{"even", "Even (V-Sync)",
       "Each refresh shows the frame the game should be at when the TV shows it: PAL's 50 frames come out "
       "even on the TV's 60, and a heavy frame doesn't make the next refresh jump two frames."},
      {"clock", "By the Clock",
       "By the time each pass of the emulator took, as 00.27: it catches up with two frames in one refresh."}},
     "even", true, "How the N64's frames are spread over the TV's refreshes with V-Sync on.", FOR_N64},
    {"perf_overlay", "Performance Graphs", "Performance", {{"off", "Off"}, {"on", "On"}}, "off",
     true, "Frame time and speed graphs on screen."},
    // Emulation
    {"region", "Region", "Emulation",
     {{"auto", "Auto", "As the game's own cartridge."},
      {"ntsc", "NTSC (60 Hz)", "As an American or Japanese console."},
      {"pal", "PAL (50 Hz)", "As a European console: the game and its music run slower."}},
     "auto", false, "Auto uses the game's own region.", FOR_RETRO | FOR_PS1 | FOR_SEGA},
    // The PC Engine's pad (00.82): its two buttons, or NEC's six-button Avenue Pad 6 for the few games made for it
    // (Beetle PCE Fast's pce_fast_default_joypad_type; its L2 switch between the two is left out of the app's pad).
    {"pce_pad", "PC Engine Pad", "Emulation",
     {{"2", "2 Buttons", "The usual pad: I and II, SELECT and RUN."},
      {"6", "6 Buttons (Avenue Pad 6)",
       "For the few games made for it (Street Fighter II'): III, IV, V, VI on Square, Triangle, L1 and R1."}},
     "2", false, "The pad the PC Engine's games see: two buttons, or six for the games made for it.", FOR_PCE},
    {"block_vram", "Block Invalid VRAM Access", "Emulation",
     {{"enabled", "On", "As the console."}, {"disabled", "Off", "For hacks and translations that need it."}},
     "enabled", true, "As the console does. A few hacks and translations need Off.", FOR_SNES},
    {"echo_hack", "Echo Buffer Hack", "Emulation", {{"disabled", "Off"}, {"enabled", "On"}},
     "disabled", true, "Fixes the sound in a few hacks (Super Mario World ROM hacks).", FOR_SNES},
    {"up_down", "Allow Opposing Directions", "Emulation", {{"disabled", "Off"}, {"enabled", "On"}},
     "disabled", true, "Up+Down or Left+Right at once (some games glitch with it).", FOR_RETRO | FOR_HANDHELD},
    {"randomize_memory", "Randomize Memory", "Emulation", {{"disabled", "Off"}, {"enabled", "On"}},
     "disabled", false, "Starts with random RAM, as a real console. Some games need it.", FOR_RETRO},
    {"nes_palette", "NES Palette", "Emulation",
     {{"default", "Default", "FCEUmm's own colours."},
      {"asqrealc", "Real (AspiringSquire)", "Colours measured from a real console."},
      {"restored-wii-vc", "Wii Virtual Console", "The colours of the Wii's Virtual Console (restored)."},
      {"nes-classic-fbx-fs", "NES Classic", "Like Nintendo's NES Classic mini console."},
      {"composite-direct-fbx", "Composite", "As through a composite cable on a tube TV."},
      {"pvm-style-d93-fbx", "PVM Monitor", "Like a Sony PVM professional monitor: rich and sharp."},
      {"smooth-v2-fbx", "Smooth", "Softer, balanced colours."},
      {"sony-cxa2025as-us", "Sony CXA2025AS", "The colours of a Sony TV's decoder chip (USA)."},
      {"pal", "PAL", "A European console's colours."},
      {"raw", "Raw", "The console's colour numbers as they are (for testing)."}},
     "default", true,
     "The NES makes its colours in the TV signal: each palette is one way to see them.", FOR_NES},
    {"n64_cpu", "N64 CPU", "Emulation",
     {{"dynarec", "Dynamic Recompiler", "The N64's code made into PS4 code as it runs: the fastest."},
      {"cached", "Cached Interpreter", "No recompiling: slower, for a game the recompiler breaks."}},
     "dynarec", false, "How the N64's processor is emulated.", FOR_N64},
    {"n64_rsp", "N64 RSP", "Emulation",
     {{"hle", "Fast (HLE sound)",
       "The N64's sound made the quick way, its graphics exactly (ParaLLEl): much faster on the PS4."},
      {"parallel", "ParaLLEl (recompiler)",
       "Everything exactly, recompiled: slower. For a game whose sound is wrong with Fast."},
      {"cxd4", "CXD4 (interpreter)", "The same, interpreted: much slower."}},
     "hle", false, "How the N64's graphics and sound processor is emulated.", FOR_N64},
    {"n64_pak", "Controller Pak", "Emulation",
     {{"memory", "Memory Pak", "Saves in the controller, as many games need."},
      {"rumble", "Rumble Pak", "The DualShock 4 shakes (games that support it)."},
      {"none", "None", "Nothing plugged into the controller."}},
     "memory", false, "What is plugged into the N64 controller.", FOR_N64},
    {"nes_turbo", "Turbo Buttons", "Emulation",
     {{"on", "On", "Held, Square and Triangle press B and A over and over."},
      {"off", "Off", "No turbo: Square and Triangle do nothing in the game."}},
     "on", true, "The NES turbo buttons (Controls: Turbo B, Turbo A).", FOR_NES},
    {"gb_bios", "Boot Logo (BIOS)", "Emulation",
     {{"on", "On", "The handheld's own start, with its logo, when its BIOS is in the system folder."},
      {"skip", "Skip the Logo", "The BIOS starts the game without its logo."},
      {"off", "Off", "mGBA's own start: no BIOS needed."}},
     "on", false,
     "Optional: copy gb_bios.bin, gbc_bios.bin, sgb_bios.bin and gba_bios.bin to /data/EmuSuNi/system. "
     "Without them mGBA starts the games itself.", FOR_HANDHELD},
    {"gba_solar", "Sunlight (Boktai)", "Emulation",
     {{"0", "0 (night)"}, {"2", "2"}, {"4", "4"}, {"6", "6 (sunny)"}, {"8", "8"}, {"10", "10 (full sun)"}},
     "6", true, "The sunlight the solar sensor of the Boktai games sees.", FOR_GBA},
    {"gb_tilt", "Tilt Sensor", "Emulation",
     {{"stick", "Right Stick", "The right stick tilts and turns the handheld (WarioWare: Twisted!, Yoshi "
       "Topsy-Turvy, Kirby Tilt 'n' Tumble)."},
      {"motion", "DualShock 4 Motion", "Tilt and turn the DualShock 4 itself, starting from how you hold it "
       "when the game starts (the right stick too)."}},
     "stick", true, "For the games with a tilt or turn sensor in the cartridge.", FOR_GBC | FOR_GBA},
    {"ps1_controller", "PS1 Controller", "Emulation",
     {{"auto", "Auto", "The DualShock, as the game asks for it (its left stick moves the D-pad in older games)."},
      {"analog", "DualShock (analog)", "The DualShock with its sticks on from the start."}},
     "auto", false, "The PlayStation pad the game sees.", FOR_PS1},
    // The PSP's stick (00.68, GitHub issue #8: "the dead zone is far too large... pushed all the way, the character
    // only walks"): PPSSPP's own dead zone and scale over the left stick (its values: the core's options, 0.15 and
    // 1.00 until 00.68). A worn stick that doesn't reach the edge runs again with a bigger scale.
    {"psp_deadzone", "PSP Stick Dead Zone", "Emulation",
     {{"0.0", "0%", "Every movement of the stick counts (a worn stick may drift)."},
      {"0.05", "5%", "Almost none."},
      {"0.10", "10%", "Small."},
      {"0.15", "15%", "The default."},
      {"0.20", "20%", "Bigger: a drifting stick stays still."},
      {"0.25", "25%", "Bigger still."},
      {"0.30", "30%", "The biggest."}},
     "0.15", true,
     "How far the left stick moves before the PSP sees it move. The rest of the way is still all of the PSP's stick.",
     FOR_PSP},
    {"psp_sensitivity", "PSP Stick Sensitivity", "Emulation",
     {{"1.00", "100%", "The stick as it is: the PSP's edge at the DualShock 4's."},
      {"1.10", "110%", "The PSP's edge a little sooner."},
      {"1.20", "120%", "Sooner: a game that only walks with the stick pushed all the way runs."},
      {"1.30", "130%", "Sooner still."},
      {"1.40", "140%", "Much sooner."},
      {"1.50", "150%", "The most: the PSP's edge at about three quarters of the way."}},
     "1.00", true,
     "How much of the PSP's stick a push gives. Higher reaches its edge before the DualShock 4's stick does (for a "
     "worn stick, or a game that wants it all the way).",
     FOR_PSP},
    // The touchpad as the DS's and the 3DS's touch screen (src/Touchpad.h): "click", a trackpad (00.64; the finger put
    // the stylus where it landed until then), or "touch", the screen itself.
    {"nds_touch", "DS Touch Screen", "Emulation",
     {{"click", "Trackpad",
       "As a laptop's trackpad: the finger moves the stylus from where it is (putting it down doesn't move it), a "
       "short tap touches the screen there. To drag: hold the touchpad's click and move, or tap and touch again at "
       "once and move."},
      {"touch", "Direct", "The touchpad is the screen: the finger presses the stylus where it lands, as on the DS "
                          "itself."}},
     "click", true,
     "How the DualShock 4's touchpad is the DS's touch screen. The stylus stays where it was left; the right stick "
     "moves it too; the Tap button is set in Controls.", FOR_NDS},
    {"3ds_touch", "3DS Touch Screen", "Emulation",
     {{"click", "Trackpad",
       "As a laptop's trackpad: the finger moves the stylus from where it is (putting it down doesn't move it), a "
       "short tap touches the screen there. To drag: hold the touchpad's click and move, or tap and touch again at "
       "once and move."},
      {"touch", "Direct", "The touchpad is the screen: the finger presses the stylus where it lands, as on the 3DS "
                          "itself."}},
     "click", true,
     "How the DualShock 4's touchpad is the 3DS's touch screen. The stylus stays where it was left; the Tap button "
     "is set in Controls.",
     FOR_3DS},
    {"3ds_new", "New 3DS Mode", "Emulation",
     {{"off", "Off (Old 3DS)",
       "The original 3DS (two ARM11 cores): the lightest, and nearly every game runs on it."},
      {"on", "On (New 3DS)",
       "The New 3DS (four cores, more memory): needed by the few games made only for it, and heavier."}},
     "off", false,
     "Which 3DS is emulated. A New 3DS-only game (Xenoblade Chronicles 3D) says so when it can't start with this Off; "
     "ZL, ZR and the C-stick are the New 3DS's.",
     FOR_3DS},
    // (00.81, the players: graphics errors in Pokémon X and Y, Majora's Mask 3D, Metroid: Samus Returns and Castlevania:
    // Mirror of Fate.) Azahar's shaders_accurate_mul, Off for every game from 00.62 to 00.80 (upstream's default is On;
    // its Android build has it Off, and there Pokémon X draws Froakie white until it is On: azahar issues #2583,
    // #1445). Auto: On for the games of Emulator.cpp ThreedsAccurateMulGame (by title ID, every region).
    {"3ds_accurate_mul", "Accurate Shader Multiplication", "Emulation",
     {{"auto", "Auto",
       "On in the games known to need it (the Pokémon games from X and Y to Ultra Sun and Ultra Moon, Majora's Mask "
       "3D, Metroid: Samus Returns, Castlevania: Mirror of Fate), Off in the others."},
      {"on", "On",
       "Multiplies in the shaders exactly as the 3DS does: models, lights and effects drawn right. A little more work "
       "for the GPU."},
      {"off", "Off", "The faster multiplication: most games look the same with it."}},
     "auto", false,
     "How the GPU's shaders multiply: exactly as the 3DS does, or faster. Without it a few games draw white, too "
     "bright or black models and effects. (Ocarina of Time 3D and the Mario & Luigi games always use it.)",
     FOR_3DS},
    // (00.81) Azahar's "Disable Right Eye Render", on for every game since 00.63 (scripts/build-3ds-core.sh: the PS4
    // shows no 3D), breaks a few games: Majora's Mask 3D's intro stays black and its blur effects freeze the screen
    // (azahar #791), Kingdom Hearts 3D and Pokémon Super Mystery Dungeon lose a screen (#1005). Auto: drawn for the
    // games of Emulator.cpp ThreedsRightEyeGame; the file /data/EmuSuNi/3ds-right-eye still draws it in every game.
    {"3ds_right_eye", "3DS Right Eye", "Emulation",
     {{"auto", "Auto",
       "Not drawn, but drawn in the games known to need it (Majora's Mask 3D, Kingdom Hearts 3D, Pokémon Super "
       "Mystery Dungeon, Fire Emblem Warriors, Minecraft: New Nintendo 3DS Edition)."},
      {"skip", "Not Drawn (faster)",
       "The PS4 shows no 3D, so the right eye's picture is left out: less work in the games that draw both eyes."},
      {"draw", "Drawn",
       "As on the 3DS: for a game whose screen stays black, freezes on an effect or is out of place. Slower in the "
       "games that draw both eyes."}},
     "auto", false,
     "3DS games draw a second picture for the 3D effect, the right eye's, which the PS4 never shows. Leaving it out "
     "is faster (Super Mario 3D Land then draws half as much), but a few games show a black, frozen or misplaced "
     "screen.",
     FOR_3DS},
    {"ps1_boot", "PS1 Boot Logo", "Emulation",
     {{"on", "On", "The PlayStation's start-up logo and sound (with its BIOS)."},
      {"skip", "Skip It", "The game starts at once."}},
     "on", false,
     "Copy the PlayStation BIOS (for example scph5501.bin) to /data/EmuSuNi/system: without it a "
     "built-in BIOS starts the games, which works with fewer of them.", FOR_PS1},
    // The Arcade (FinalBurn Neo): its boards' own refresh rates, and the Neo Geo it runs the Neo Geo
    // games on (the choice is read as the game starts).
    {"arcade_60hz", "Arcade 60 Hz", "Emulation",
     {{"off", "Off", "Each board's own rate (many ran at 54-59 frames per second; the Neo Geo at 59.19)."},
      {"on", "On", "Every game at 60 frames per second: as smooth as the TV, the game and its music a little faster."}},
     "off", false,
     "Runs every arcade game at the TV's 60 frames per second. Off keeps each board's own rate, as in the "
     "arcade.", FOR_ARCADE},
    {"arcade_neogeo", "Neo Geo", "Emulation",
     {{"dipswitch", "As the BIOS DIP Switch", "The European MVS: the app doesn't change the games' DIP switches yet."},
      {"mvs_eur", "MVS Europe", "The arcade cabinet (MVS) of Europe: coins, the arcade's menus, English."},
      {"mvs_usa", "MVS USA", "The arcade cabinet of the USA: English, and its BIOS censors some games."},
      {"mvs_jap", "MVS Japan", "The arcade cabinet of Japan: Japanese text where the game has it."},
      {"aes_eur", "AES Europe", "The home console (AES): no coins, the home menus, English."},
      {"aes_jap", "AES Japan", "The home console of Japan: Japanese text where the game has it."}},
     "dipswitch", false,
     "The Neo Geo the Neo Geo games run on: the arcade cabinet (MVS) of a region, or the home console (AES). "
     "The BIOS files are in neogeo.zip.", FOR_ARCADE},
    // The NAOMI's region (Flycast's region option, read as the game starts: the board boots its BIOS of that region
    // from naomi.zip). Not the Dreamcast's Default: for a NAOMI that is Korea. A game made for other regions only
    // gets one of its own (the core: naomi_flashrom.cpp). The Atomiswave's BIOS has one region.
    {"naomi_region", "NAOMI Region", "Emulation",
     {{"usa", "USA", "The American board: English."},
      {"export", "Export", "The board sold outside Japan and the USA: English."},
      {"japan", "Japan", "The Japanese board: Japanese text where the game has it."}},
     "usa", false,
     "The region of the NAOMI board the games run on (its BIOS, in naomi.zip). A game made only for another "
     "region runs on that one.", FOR_NAOMI},
    // System
    // The in-game menu's buttons (00.68, GitHub issue #5), for every game: in Settings -> Controls and the in-game
    // menu's Controls, not in the settings lists (Controls::MenuButtonsFor; L3 + R3 always open it too).
    {"menu_buttons", "In-Game Menu", "System",
     {{"l3r3", "L3 + R3", "Both sticks pressed at once."},
      {"r3", "R3 (hold)",
       "The right stick pressed for a second. A game that uses R3 sees it meanwhile; on the DS and the 3DS, R3 swaps "
       "the screens when let go sooner."},
      {"touchpad", "Touchpad (hold)",
       "The touchpad pressed for a second (a game sees its SELECT meanwhile). On the DS and the 3DS, where it is the "
       "touch screen: L3 + R3."},
      {"options_touchpad", "Options + Touchpad",
       "Both at once: START and SELECT on most consoles. On the DS and the 3DS: L3 + R3."},
      // (00.69, the idea of pull request #14: both shoulder buttons or both triggers held a second, on every console.)
      {"l1r1", "L1 + R1 (hold)",
       "Both held for a second; the game sees them meanwhile (on the DS and the 3DS, its L and R). It can clash with a "
       "game that holds both together."},
      {"l2r2", "L2 + R2 (hold)",
       "Both held for a second; the game sees them meanwhile. It can clash with a game that holds both together (a "
       "racing game's L2 and R2), or with R2's Fast Forward."}},
     "l3r3", true,
     "The buttons that open the menu in a game; L3 + R3 always open it too. In the menu, Circle goes back to the game. "
     "(The PS button is the PS4's own.)",
     0},
    {"language", "Language", "System",
     {{"en", "English"},
      {"es", "Español"},
      {"fr", "Français"},
      {"de", "Deutsch"},
      {"it", "Italiano"},
      {"pt", "Português"},
      {"nl", "Nederlands"},
      {"pl", "Polski"},
      {"tr", "Türkçe"},
      {"ru", "Русский"},
      {"ja", "日本語"},
      {"zh", "中文 (简体)"},
      {"ko", "한국어"}},
     "en", false, "The menus' language."},
    {"volume", "Game Volume", "System",
     {{"100", "100"}, {"90", "90"}, {"80", "80"}, {"70", "70"}, {"60", "60"}, {"50", "50"},
      {"40", "40"}, {"30", "30"}, {"20", "20"}, {"10", "10"}, {"0", "0"}},
     "100", true, "Volume of the games' sound."},
    {"audio_interpolation", "Audio Interpolation", "System",
     {{"gaussian", "Gaussian (console)", "The console's own sound filter: warm, a little muffled."},
      {"cubic", "Cubic", "Crisper high notes."},
      {"sinc", "Sinc", "The crispest and cleanest (more CPU time)."},
      {"linear", "Linear", "Simple, a little harsh."},
      {"none", "No Interpolation", "The raw samples: sharp and noisy."}},
     "gaussian", true, "Gaussian sounds like the console; Cubic and Sinc are crisper.", FOR_SNES},
    {"gb_audio_filter", "Sound Filter", "System",
     {{"off", "Off", "The sound as the handheld made it."},
      {"soft", "Soft", "A low-pass filter takes the harshness off the high notes."}},
     "off", true, "Softens the handhelds' sharp sound.", FOR_HANDHELD | FOR_GG},
    {"audio_buffer", "Audio Buffer", "System",
     {{"64", "64 ms", "The usual balance."},
      {"48", "48 ms", "Less delay; may crackle when the PS4 is busy."},
      {"96", "96 ms", "Smoother sound in heavy games, a little more delay."},
      {"128", "128 ms", "The smoothest sound, the most delay."}},
     "64", false,
     "Larger is smoother when a game runs below full speed; smaller has less delay."},
    {"multitap", "Multitap (3-5 Players)", "System",
     {{"off", "Off", "Two players."}, {"on", "On", "Up to five players with more DualShock 4s."}},
     "off", true, "The Super Multitap in port 2, for Bomberman and other 3-5 player games.", FOR_SNES},
    {"resume", "Resume Where You Left Off", "System",
     {{"off", "Off", "Each start begins from the game's own start (or its battery save)."},
      {"on", "On", "Quit Game keeps the moment you left; the next start goes on from there."}},
     "off", false, "Quit Game saves the game's state, and the next start continues from there."},
};

#undef EMUSUNI_SHARED_FRAMES
#undef EMUSUNI_GAME_BEZEL

const std::vector<const char*> CATEGORIES = {"Video", "Graphics", "Performance", "Emulation",
                                             "System"};

// Quality's levels (Video): the values each one sets. The level shown is the one whose values are all set (the one
// chosen last first, kept as "quality" where it was chosen: two levels may set the same on a console), else Custom.
struct Preset
{
  const char* name;
  std::vector<std::pair<const char*, const char*>> values;
};
// Low is each console's own resolution with no upscaler and no filter (00.63: the PSP without anisotropic filtering,
// its textures not upscaled; 00.69: Low is also what a setting never changed reads, so a new install and Reset All
// Settings start at Low). Each level above is clearly sharper than the one below (00.69, the owner: "que de verdad se
// sienta la subida de calidad"; 00.40 to 00.68 went Smooth Edges, Natural HD, Natural AI, three looks hard to tell
// apart, and on the 3D consoles' HD pictures Natural AI was softer than Natural HD - PC, the filter harness: Tekken 3 at
// 4x 6.4 against 14.4 of fine detail, GoldenEye at 2x 2.3 against 4.4 - the network only works at about 768x576 to
// 960x720). The GPU Upscaler, one step per level (GpuFilter.h; Resolution 3x/6x stays the CPU's picture while the GPU's
// filters compile):
// - the consoles whose picture is pixels (the CPU's: the 2D consoles, the Arcade, the DS, the Saturn): Medium Smooth
//   Edges (the jagged edges smoothed, the pixels kept), High Pixel Art HD (xBRZ: the pixels redrawn as clean HD shapes
//   at the TV's resolution), Ultra AI (xBRZ, then Anime4K's network: the smoothest);
// - the 3D consoles' pictures, drawn by the GPU (Emulator.cpp GpuMode): Medium and High Smooth Edges (anti-aliased, at
//   the size drawn), Ultra Natural HD (sharpened: AMD's FSR or RCAS), with their resolution going up: what the PS4
//   carries at full speed (its logs: the N64 at 4x 96-100% in Mario 64, Banjo-Tooie, Donkey Kong 64, Perfect Dark - the
//   heavy games are as slow at 1x, by the CPU -; the PSP's God of War and Tekken 6 at 4x 94-100% since 00.47; the
//   PlayStation's Gran Turismo 2 and Tekken 3 used a quarter of the GPU at most at 4x in 00.33; the Dreamcast at 3x
//   12-31% busy in 00.68's log, Jet Set Radio and Daytona USA at full speed, the Atomiswave's Dolphin Blue 93-99%): the
//   N64 1x, 2x, 4x, 4x (its most: 8x would need 512 MiB of GPU memory), the PlayStation and the PSP 1x, 2x, 3x, 4x,
//   Flycast's consoles 1x, 2x, 3x, 3x (no 4x: made that small for the TV it only shimmers more) with Texture Sharpness
//   4x, 8x, 8x, 16x (00.68; 00.53 to 00.67 played them at Low only). The N64's 2D stays Exact at every level (00.68:
//   Smooth HD put seams over pre-drawn backgrounds), Flycast's transparency per triangle.
// The 3DS (00.71; from 00.64 to 00.70 its own resolution only, 00.63 set it 1x, 2x, 3x, 3x): 3DS Resolution 1x, 2x, 3x,
// 4x with the 3D consoles' GPU Upscaler steps (Smooth Edges at 2x - SMAA at 800x960 for its screens one above the other
// -, at 3x the picture bigger than the TV's already, made smaller: supersampled; Ultra sharpened). PC, the filter
// harness on Hey! Pikmin's and Super Mario 3D Land's pictures as a 1080p TV shows them: each step a clear change (the
// TV's pixels 3.0 and 2.8 of 255 from Low to Medium, 1.3 and 1.3 to High, 1.9 and 1.1 to Ultra) with more fine detail
// each time. Not Azahar's texture filters: on the 3D only subtle (ScaleForce, xBRZ, Anime4K; MMPX blocky, Bicubic
// softer), and their textures at the full size took about twice the GPU memory (224 MiB of blocks at 3x and 4x against
// 96). A bigger size costs the emulation thread no time in the games tried (PC: the same CPU time a frame at 1x to 4x;
// GPU memory 32, 32, 96, 96 MiB of blocks); a game that reads its picture back waits for the GPU's work, more at a
// bigger size. The levels leave the player's own look as it is: the picture's size, zoom and shape, the bezels, Tube
// TV, Scanlines and the NTSC Filter (00.63: the size, zoom and NTSC Filter were set by every level until 00.62, the
// same at each).
const std::vector<Preset> PRESETS = {
    {"low", {{"resolution", "1"}, {"upscaler", "xbrz"}, {"n64_resolution", "1"}, {"n64_sync", "fast"},
             {"n64_2d", "native"}, {"gpu_filter", "off"}, {"ps1_resolution", "1"}, {"ps1_texture_filter", "Nearest"},
             {"ps1_colors", "dither"}, {"ps1_pgxp", "off"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "1"},
             {"psp_anisotropy", "0"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
             {"dc_resolution", "1"}, {"dc_anisotropy", "4"}, {"dc_sorting", "triangle"}, {"3ds_resolution", "1"}}},
    {"medium", {{"resolution", "3"}, {"upscaler", "xbrz"}, {"n64_resolution", "2"}, {"n64_sync", "fast"},
                {"n64_2d", "native"}, {"gpu_filter", "edges"}, {"ps1_resolution", "2"}, {"ps1_texture_filter", "xBR"},
                {"ps1_colors", "true"}, {"ps1_pgxp", "off"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "2"},
                {"psp_anisotropy", "8"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
                {"dc_resolution", "2"}, {"dc_anisotropy", "8"}, {"dc_sorting", "triangle"}, {"3ds_resolution", "2"}}},
    // (00.69: the PlayStation 3x, its 4x Ultra's; Pixel Art HD, Smooth Edges for a picture drawn by the GPU.)
    {"high", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"n64_resolution", "4"}, {"n64_sync", "fast"},
              {"n64_2d", "native"}, {"gpu_filter", "pixel"}, {"ps1_resolution", "3"}, {"ps1_texture_filter", "xBR"},
              {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "3"},
              {"psp_anisotropy", "16"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
              {"dc_resolution", "3"}, {"dc_anisotropy", "8"}, {"dc_sorting", "triangle"}, {"3ds_resolution", "3"}}},
    // Ultra for the PlayStation: 4x, xBR textures, True Colour, PGXP, 4x MSAA, sharpened (00.34 to 00.68 then Natural
    // AI: on its 4x picture it was softer than High). The PSP at 4x without MSAA: with it every new pipeline took
    // ~86 ms to compile on the PS4 (5-19 ms without) and God of War stuttered; a 4x picture gets Natural HD's
    // sharpening. (00.69: AI, Natural HD for a picture drawn by the GPU.)
    {"ultra", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"n64_resolution", "4"}, {"n64_sync", "fast"},
               {"n64_2d", "native"}, {"gpu_filter", "ai"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
               {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "4"}, {"psp_resolution", "4"},
               {"psp_anisotropy", "16"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
               {"dc_resolution", "3"}, {"dc_anisotropy", "16"}, {"dc_sorting", "triangle"}, {"3ds_resolution", "4"}}},
};
// The levels as 00.68 set them (Low and Medium as now, but for the 3DS's size, 00.71's): the one-time changes of Load
// find a level chosen before by them (dc_levels, levels=00.69).
const std::vector<Preset> PRESETS_0068_HIGH_ULTRA = {
    {"high", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"n64_resolution", "4"}, {"n64_sync", "fast"},
              {"n64_2d", "native"}, {"gpu_filter", "natural"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
              {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "3"},
              {"psp_anisotropy", "16"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
              {"dc_resolution", "3"}, {"dc_anisotropy", "8"}, {"dc_sorting", "triangle"}}},
    {"ultra", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"n64_resolution", "4"}, {"n64_sync", "fast"},
               {"n64_2d", "native"}, {"gpu_filter", "natural_ai"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
               {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "4"}, {"psp_resolution", "4"},
               {"psp_anisotropy", "16"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
               {"dc_resolution", "3"}, {"dc_anisotropy", "16"}, {"dc_sorting", "triangle"}}},
};

std::mutex s_mutex;
std::string s_path;
std::map<std::string, std::map<std::string, std::string>> s_sections;  // "global", game IDs
std::map<std::string, std::string> s_tour;  // the screenshot tour's values (SetTourValue)

std::string Trim(std::string text)
{
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
    text.pop_back();
  return text;
}

std::string Fallback(const std::string& key)
{
  const Setting* setting = Find(key);
  return setting ? setting->fallback : "";
}

// A section's "custom" value ("" when there is none).
std::string ModeLocked(const std::string& section)
{
  const auto it = s_sections.find(section);
  if (it == s_sections.end())
    return {};
  const auto mode = it->second.find("custom");
  return mode == it->second.end() ? std::string() : mode->second;
}

std::string ConsoleSection(const std::string& game_id)
{
  return std::string("console:") + SystemOfKey(game_id).key;
}

// Where the game's settings come from: its own section, its console's, or "global".
std::string SectionLocked(const std::string& game_id)
{
  if (game_id.empty())
    return "global";
  const std::string mode = ModeLocked(game_id);
  if (mode == "on")
    return game_id;
  if (mode == "global")
    return "global";
  // "console", or never set: the console's when it has settings of its own.
  const std::string console = ConsoleSection(game_id);
  return ModeLocked(console) == "on" ? console : "global";
}

std::string GetLocked(const std::string& key, const std::string& game_id)
{
  // The game's place, then (for a game's own section) its console's, then global, then the default.
  const std::string section = SectionLocked(game_id);
  std::vector<std::string> order = {section};
  if (section != "global" && section.rfind("console:", 0) != 0 && ModeLocked(ConsoleSection(game_id)) == "on")
    order.push_back(ConsoleSection(game_id));
  if (section != "global")
    order.push_back("global");
  for (const std::string& name : order)
  {
    const auto it = s_sections.find(name);
    if (it == s_sections.end())
      continue;
    const auto value = it->second.find(key);
    if (value != it->second.end())
      return value->second;
  }
  return Fallback(key);
}

// The level whose values the game (or the global settings) uses, else "custom". A game's level is read from
// its own console's settings only (a PSP game is at Low whatever another console's settings are).
std::string QualityLocked(const std::string& game_id)
{
  const SystemId system = SystemOfKey(game_id).id;
  auto all_set = [&](const Preset& preset) {
    for (const auto& [key, value] : preset.values)
    {
      // (Not the settings no console shows: the Dreamcast's Transparency, per triangle at every level.)
      const Setting* setting = Find(key);
      if (setting && (setting->systems == 0 || (!game_id.empty() && !AppliesTo(*setting, system))))
        continue;
      if (GetLocked(key, game_id) != value)
        return false;
    }
    return true;
  };
  // The level chosen last where the game's settings come from, while its values are all still set (two levels can
  // set the same on a console), else the first level whose values are.
  const std::string chosen = GetLocked("quality", game_id);
  for (const Preset& preset : PRESETS)
  {
    if (chosen == preset.name && all_set(preset))
      return preset.name;
  }
  for (const Preset& preset : PRESETS)
  {
    if (all_set(preset))
      return preset.name;
  }
  return "custom";
}

void SaveLocked()
{
  if (s_path.empty())
    return;
  std::ostringstream file;
  file << "# EmuSuNi settings, written by the menu and the in-game menu.\n"
          "# [global] applies to every game; a [game] section with custom=on overrides it.\n";
  auto write = [&](const std::string& name, const std::map<std::string, std::string>& values) {
    file << "\n[" << name << "]\n";
    for (const auto& [key, value] : values)
      file << key << "=" << value << "\n";
  };
  if (s_sections.count("global"))
    write("global", s_sections["global"]);
  for (const auto& [name, values] : s_sections)
  {
    if (name != "global")
      write(name, values);
  }
  WriteTextFile(s_path, file.str());
}

// The values a game uses now, its Quality level as the level chosen there (QualityLocked): a new place starts from them.
std::map<std::string, std::string> NowLocked(const std::string& game_id)
{
  std::map<std::string, std::string> now;
  for (const Setting& setting : SETTINGS)
  {
    if (std::string(setting.key) != "quality")
      now[setting.key] = GetLocked(setting.key, game_id);
  }
  if (const std::string level = QualityLocked(game_id); level != "custom")
    now["quality"] = level;
  return now;
}

// This Console for the game: its console gets settings of its own, the ones the game uses now when it has none yet.
void StartConsoleLocked(const std::string& game_id)
{
  const std::string console = ConsoleSection(game_id);
  if (ModeLocked(console) != "on")
  {
    const std::map<std::string, std::string> now = NowLocked(game_id);
    auto& section = s_sections[console];
    if (section.size() <= 1)  // the console's first settings: the ones its game uses now
    {
      const SystemId system = SystemOfKey(game_id).id;
      for (const Setting& setting : SETTINGS)
      {
        const auto value = now.find(setting.key);
        if (value != now.end() && AppliesTo(setting, system))
          section[setting.key] = value->second;
      }
    }
    section["custom"] = "on";
  }
  s_sections[game_id]["custom"] = "console";
}
}  // namespace

const std::vector<Setting>& All()
{
  return SETTINGS;
}

const std::vector<const char*>& Categories()
{
  return CATEGORIES;
}

const Setting* Find(const std::string& key)
{
  for (const Setting& setting : SETTINGS)
  {
    if (key == setting.key)
      return &setting;
  }
  return nullptr;
}

void Load(const std::string& data_dir)
{
  std::lock_guard lock(s_mutex);
  s_path = data_dir + "/settings.ini";
  s_sections.clear();
  std::ifstream file(s_path);
  std::string line, section = "global";
  while (std::getline(file, line))
  {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq != std::string::npos)
      s_sections[section][line.substr(0, eq)] = line.substr(eq + 1);
  }
  // The consoles that came after the Quality levels, in the order they came (their values left out
  // where a level chosen before them is recognised): the PlayStation (00.33; the public 00.19 has
  // none of its values), the PSP (00.36), the Dreamcast (00.48).
  static const char* const NEWER[] = {"ps1_", "psp_", "dc_"};
  // What the levels set since 00.63 (the PSP's textures not upscaled; the 3DS's resolution, in 00.63 and since 00.71):
  // in no file of before, so the comparisons with the levels below leave it out (a 3DS's value is never added here: its
  // own change, 3ds_levels=00.71, is the last below).
  auto since_0063 = [](const std::string& key) { return key == "psp_texture_shader" || key == "3ds_resolution"; };
  // 00.68: Ultra draws the N64's 2D Exact, as the other levels (its Smooth HD 2D put lines and squares over Zelda:
  // Ocarina of Time's pre-drawn rooms and ISS 64's intro: GitHub issue #1). An Ultra chosen before - N64 Resolution 4x,
  // 2D Smooth HD and Natural AI (the AI before 00.40) - stays Ultra with it, once ([global] n64_2d_ultra=00.68). First,
  // so the comparisons with the levels below find Ultra again.
  if (s_sections["global"]["n64_2d_ultra"] != "00.68")
  {
    for (auto& [name, values] : s_sections)
    {
      const auto two_d = values.find("n64_2d"), size = values.find("n64_resolution"), gpu = values.find("gpu_filter");
      if (two_d != values.end() && two_d->second == "hd" && size != values.end() && size->second == "4" &&
          gpu != values.end() && (gpu->second == "natural_ai" || gpu->second == "ai"))
        two_d->second = "native";
    }
    s_sections["global"]["n64_2d_ultra"] = "00.68";
    SaveLocked();
  }
  // 00.40: the levels' GPU Upscaler went from Off, Off, AI (Medium, High, Ultra) to Smooth Edges,
  // Natural HD, Natural AI. A level chosen before stays that level, once ([global] presets=2). First,
  // so the consoles' values below find the level again.
  if (s_sections["global"]["presets"] != "2")
  {
    static const std::pair<const char*, const char*> OLD[] = {{"medium", "off"}, {"high", "off"}, {"ultra", "ai"}};
    for (auto& [name, values] : s_sections)
    {
      for (const auto& [level, old] : OLD)
      {
        const auto preset = std::find_if(PRESETS.begin(), PRESETS.end(),
                                         [level = level](const Preset& p) { return std::string(p.name) == level; });
        if (preset == PRESETS.end() || values.count("gpu_filter") == 0 || values["gpu_filter"] != old)
          continue;
        bool same = true;
        const char* now = nullptr;
        for (const auto& [key, value] : preset->values)
        {
          const std::string k = key;
          const bool newer = std::any_of(std::begin(NEWER), std::end(NEWER),
                                         [&k](const char* prefix) { return k.rfind(prefix, 0) == 0; });
          if (k == "gpu_filter")
            now = value;
          else if (!newer && !since_0063(k))
            same = same && values.count(key) && values[key] == value;
        }
        if (same && now)
        {
          values["gpu_filter"] = now;
          break;
        }
      }
    }
    s_sections["global"]["presets"] = "2";
    SaveLocked();
  }
  // 00.50: the Dreamcast's per-pixel transparency (Ultra's in 00.48 and 00.49) drew the picture in
  // wrong colours on the PS4's GPU (Crazy Taxi, after Ultra was chosen in the game): per triangle again
  // wherever it was set, once ([global] dc_pixel=reset). Ultra is per triangle now.
  if (s_sections["global"]["dc_pixel"] != "reset")
  {
    for (auto& [name, values] : s_sections)
    {
      const auto it = values.find("dc_sorting");
      if (it != values.end() && it->second == "pixel")
        it->second = "triangle";
    }
    s_sections["global"]["dc_pixel"] = "reset";
    SaveLocked();
  }
  // 00.63: Low is each console's own picture with no filter: the PSP's textures without anisotropic filtering (4x until
  // 00.62). A Low chosen before - a section with 00.62's Low for the PSP and the other consoles' values it has - gets
  // it, once ([global] quality_levels=00.63); a Texture Sharpness of 4x with another level stays.
  if (s_sections["global"]["quality_levels"] != "00.63")
  {
    static const std::pair<const char*, const char*> LOW_0062[] = {
        {"resolution", "1"},       {"upscaler", "xbrz"},    {"n64_resolution", "1"},    {"n64_sync", "fast"},
        {"n64_2d", "native"},      {"gpu_filter", "off"},   {"ps1_resolution", "1"},    {"ps1_texture_filter", "Nearest"},
        {"ps1_colors", "dither"},  {"ps1_pgxp", "off"},     {"ps1_antialiasing", "1"},  {"psp_resolution", "1"},
        {"psp_antialiasing", "1"}};
    for (auto& [name, values] : s_sections)
    {
      const auto anisotropy = values.find("psp_anisotropy");
      if (anisotropy == values.end() || anisotropy->second != "4" || !values.count("psp_resolution") ||
          !values.count("gpu_filter"))
        continue;
      bool low = true;
      for (const auto& [key, value] : LOW_0062)
      {
        const auto it = values.find(key);
        low = low && (it == values.end() || it->second == value);
      }
      if (low)
        anisotropy->second = "0";
    }
    s_sections["global"]["quality_levels"] = "00.63";
    SaveLocked();
  }
  // 00.68: Flycast's consoles follow the Quality levels again (from 00.53 to 00.67 they played at Low whatever the
  // level, the levels' values for them written but not used): a level chosen before is theirs too, and still shows as
  // that level. High's texture sharpness for them is 8x now (16x is Ultra's, its one difference from High there): a
  // section at High from before - its other values High's - gets 8x, once ([global] dc_levels=00.68); Low's, Medium's
  // and Ultra's values are their own already.
  if (s_sections["global"]["dc_levels"] != "00.68")
  {
    // (High as 00.68 set it: its GPU Upscaler and PlayStation size changed in 00.69, below.)
    const Preset* high = nullptr;
    for (const Preset& preset : PRESETS_0068_HIGH_ULTRA)
    {
      if (std::string(preset.name) == "high")
        high = &preset;
    }
    for (auto& [name, values] : s_sections)
    {
      const auto anisotropy = values.find("dc_anisotropy");
      if (high == nullptr || anisotropy == values.end() || anisotropy->second != "16")
        continue;
      bool at_high = true, any = false;
      for (const auto& [key, value] : high->values)
      {
        const auto it = values.find(key);
        if (std::string(key).rfind("dc_", 0) == 0 || it == values.end())
          continue;
        any = true;
        at_high = at_high && it->second == value;
      }
      if (any && at_high)
        anisotropy->second = "8";
    }
    s_sections["global"]["dc_levels"] = "00.68";
    SaveLocked();
  }
  // 00.69: High's and Ultra's new looks (PRESETS: High Pixel Art HD and the PlayStation at 3x, Ultra AI). Where a
  // section is a level of 00.68 for a console whose games read it - the values it holds of that console's part of the
  // level, its size among them (Resolution, N64 Resolution...; the Saturn has the GPU Upscaler alone), are all that
  // level's - those values become the level's of 00.69, once ([global] levels=00.69): a level chosen before is still
  // that level, with its new look. A console's own value changed by hand (that console no level) stays as it is. A
  // section only Flycast's games read (the Dreamcast's, the NAOMI's or the Atomiswave's settings, one of their games')
  // gets its level's GPU Upscaler too where it has none: theirs since 00.69, it would come from All Games.
  if (s_sections["global"]["levels"] != "00.69")
  {
    // The levels as 00.68 set them (Low and Medium as now), and as they are.
    std::vector<std::pair<const Preset*, const Preset*>> levels;
    for (const Preset& preset : PRESETS)
    {
      const Preset* before = &preset;
      for (const Preset& old : PRESETS_0068_HIGH_ULTRA)
      {
        if (std::string(old.name) == preset.name)
          before = &old;
      }
      levels.emplace_back(before, &preset);
    }
    static const char* const SIZES[] = {"resolution", "n64_resolution", "ps1_resolution", "psp_resolution",
                                        "dc_resolution"};
    const Setting* quality = Find("quality");
    for (auto& [name, values] : s_sections)
    {
      // The consoles whose games read the section: All Games every console, a console's that console, a game its own.
      std::vector<SystemId> readers;
      if (name == "global")
      {
        for (const System& system : Systems())
          readers.push_back(system.id);
      }
      else if (name.rfind("console:", 0) == 0)
      {
        if (const System* console = SystemByKey(name.substr(8)))
          readers.push_back(console->id);
      }
      else
      {
        readers.push_back(SystemOfKey(name).id);
      }
      const bool flycast_only = readers.size() == 1 && IsFlycast(readers.front());
      // The level chosen last where the section has one (Set): another level's values there are a choice made by hand
      // after it (00.68's Ultra with Natural HD chosen: the N64's part reads as High's, and stays).
      const auto chosen = values.find("quality");
      std::map<std::string, std::string> changes;
      for (const SystemId system : readers)
      {
        // (The 3DS had no levels then: its own change, 3ds_levels=00.71, below.)
        if (!quality || !AppliesTo(*quality, system) || system == SystemId::THREEDS)
          continue;
        for (const auto& [before, now] : levels)
        {
          if (chosen != values.end() && chosen->second != now->name)
            continue;
          // That console's part of the level the section holds: every value 00.68's, its size among them.
          std::vector<std::string> held;
          bool same = true, sized = false, sizes = false;
          for (const auto& [key, value] : before->values)
          {
            const Setting* setting = Find(key);
            if (!setting || setting->systems == 0 || !AppliesTo(*setting, system))
              continue;
            const bool size = std::any_of(std::begin(SIZES), std::end(SIZES),
                                          [key = key](const char* each) { return std::string(key) == each; });
            sizes = sizes || size;
            const auto it = values.find(key);
            if (it == values.end())
              continue;
            same = same && it->second == value;
            sized = sized || size;
            held.push_back(key);
          }
          if (!same || held.empty() || (sizes && !sized))
            continue;
          for (const auto& [key, value] : now->values)
          {
            if (std::find(held.begin(), held.end(), key) != held.end())
              changes[key] = value;
            else if (flycast_only && std::string(key) == "gpu_filter" && !values.count(key))
              changes[key] = value;
          }
          break;  // (the first level whose values these are: Low's and Medium's stay as they are)
        }
      }
      for (const auto& [key, value] : changes)
        values[key] = value;
    }
    s_sections["global"]["levels"] = "00.69";
    SaveLocked();
  }
  // (00.69: the NEWER values below come after the two changes above - 00.68's Dreamcast High and 00.69's levels -,
  // so a file of before them is first at today's levels, and the values of a console that came after its level are
  // today's level's: 00.35's High gets the PSP's High of now.)
  // A Quality level chosen before a console came (the PlayStation: 00.32 and older, the public 00.19
  // too; the PSP: 00.35 and older; the Dreamcast: 00.47 and older) stays that level: the level's values for that console are added to the section that has
  // all its other values (the later consoles' left out of the comparison). Only the missing ones: a
  // value of that console already set (one setting changed by hand) stays.
  for (size_t n = 0; n < std::size(NEWER); n++)
  {
    const std::string prefix = NEWER[n];
    auto newer = [n](const std::string& key) {
      for (size_t m = n; m < std::size(NEWER); m++)
      {
        if (key.rfind(NEWER[m], 0) == 0)
          return true;
      }
      return false;
    };
    for (auto& [name, values] : s_sections)
    {
      if (values.count(prefix + "resolution"))
        continue;
      for (const Preset& preset : PRESETS)
      {
        bool same = true;
        for (const auto& [key, value] : preset.values)
        {
          if (!newer(key) && !since_0063(key))
            same = same && values.count(key) && values[key] == value;
        }
        if (!same)
          continue;
        for (const auto& [key, value] : preset.values)
        {
          if (std::string(key).rfind(prefix, 0) == 0)
            values.emplace(key, value);
        }
        break;
      }
    }
  }
  // 00.68: a console's settings made only by opening one of its games' pages (00.58 to 00.67: UseConsoleByDefault made
  // them at once, a copy of All Games' values) follow All Games again where they still are that copy - every value the
  // same as All Games' -, once ([global] console_copies=00.68). Its games stay at This Console; a console whose
  // settings were changed keeps them. Last: after the migrations above, which change All Games' values and the
  // consoles' alike.
  if (s_sections["global"]["console_copies"] != "00.68")
  {
    for (auto it = s_sections.begin(); it != s_sections.end();)
    {
      bool copy = it->first.rfind("console:", 0) == 0 && ModeLocked(it->first) == "on";
      for (const auto& [key, value] : it->second)
      {
        if (!copy)
          break;
        if (key != "custom" && key != "quality")
          copy = GetLocked(key, "") == value;
      }
      it = copy ? s_sections.erase(it) : std::next(it);
    }
    s_sections["global"]["console_copies"] = "00.68";
    SaveLocked();
  }
  // 00.69: the 3DS as it looks, the top screen above and the touch screen below (the owner: "la 3DS que parta con una
  // pantalla arriba y otra abajo"). One Above the Other is the default since 00.64, but a view kept before stayed: 00.62's
  // and 00.63's default (Side by Side) copied into a console's settings, or one chosen in the in-game menu. Every 3DS
  // Screens value kept - All Games, This Console, This Game - becomes One Above the Other, once ([global]
  // 3ds_stack=00.69, not the setting's own key): a view chosen after this is kept. Last, after the copies above.
  if (s_sections["global"]["3ds_stack"] != "00.69")
  {
    std::string changed;
    for (auto& [name, values] : s_sections)
    {
      const auto layout = values.find("3ds_layout");
      if (layout == values.end() || layout->second == "stack")
        continue;
      changed += (changed.empty() ? "" : ", ") + name + " (" + layout->second + ")";
      layout->second = "stack";
    }
    LogF("settings: 3DS Screens set to One Above the Other, the touch screen below, once (00.69): %s",
         changed.empty() ? "no other view was kept" : changed.c_str());
    s_sections["global"]["3ds_stack"] = "00.69";
    SaveLocked();
  }
  // 00.71: the 3DS has the Quality levels again (00.63 gave it 1x, 2x, 3x, 3x; from 00.64 to 00.70 every game played at
  // 1x whatever the level, without the GPU Upscaler): 3DS Resolution 1x to 4x and the GPU Upscaler, as the other 3D
  // consoles. Where a 3DS game's settings come from - All Games, the 3DS's This Console, a 3DS game's own - a level
  // chosen there (Set's "quality"; for All Games without it, a file of before 00.63, the level whose values it holds) is
  // the 3DS's too: its 3DS Resolution, and in the 3DS's own sections its GPU Upscaler (a copy of another place's there:
  // the 3DS read none). Where none was chosen, the 3DS stays as it played: a 3DS Resolution kept there (00.63's, read by
  // no game since) is 1x, and in its own sections a GPU Upscaler kept there is Off; what a section doesn't hold still
  // comes from All Games, whose GPU Upscaler is every console's, the 3DS's from now on too. So All Games' page shows the
  // level it showed, and a 3DS game the level chosen where its settings come from. Once ([global] 3ds_levels=00.71; the
  // log says what changed). Last, after the changes above.
  if (s_sections["global"]["3ds_levels"] != "00.71")
  {
    // All Games' level where its file doesn't say which was chosen (before 00.63): the one whose values it holds, as
    // its page showed it (the 3DS's size left out: no level set it then).
    const Preset* global_level = nullptr;
    for (const Preset& preset : PRESETS)
    {
      bool all = true;
      for (const auto& [key, value] : preset.values)
      {
        const Setting* setting = Find(key);
        if (std::string(key) != "3ds_resolution" && setting && setting->systems != 0)
          all = all && GetLocked(key, "") == value;
      }
      if (all && global_level == nullptr)
        global_level = &preset;
    }
    std::string changed;
    for (auto& [name, values] : s_sections)
    {
      bool own = false;  // the 3DS's console settings, or a 3DS game's (whatever it follows now: This Game brings it back)
      if (name.rfind("console:", 0) == 0)
      {
        const System* console = SystemByKey(name.substr(8));
        if (!console || console->id != SystemId::THREEDS)
          continue;
        own = true;
      }
      else if (name != "global")
      {
        if (SystemOfKey(name).id != SystemId::THREEDS)
          continue;
        own = true;
      }
      const auto chosen = values.find("quality");
      const Preset* level = own || chosen != values.end() ? nullptr : global_level;
      for (const Preset& preset : PRESETS)
      {
        if (chosen != values.end() && chosen->second == preset.name)
          level = &preset;
      }
      std::string what;
      auto put = [&section = values, &what, own](const std::string& key, const std::string& value) {
        const auto it = section.find(key);
        // (All Games without the value reads the setting's default: the same as writing it.)
        if (it != section.end() ? it->second == value : !own && value == Fallback(key))
          return;
        what += (what.empty() ? "" : ", ") + key + " " + (it == section.end() ? std::string("-") : it->second) + " -> " +
                value;
        section[key] = value;
      };
      if (level != nullptr)
      {
        for (const auto& [key, value] : level->values)
        {
          if (std::string(key) == "3ds_resolution" || (own && std::string(key) == "gpu_filter"))
            put(key, value);
        }
      }
      else
      {
        if (values.count("3ds_resolution"))
          put("3ds_resolution", "1");
        if (own && values.count("gpu_filter"))
          put("gpu_filter", "off");
      }
      if (!what.empty())
        changed += (changed.empty() ? "" : "; ") + name + " (" + what + ")";
    }
    LogF("settings: the 3DS's Quality levels (00.71), once: %s", changed.empty() ? "nothing to change" : changed.c_str());
    s_sections["global"]["3ds_levels"] = "00.71";
    SaveLocked();
  }
  // 00.80: the DS's and the 3DS's screens, one set of settings (EMUSUNI_SCREEN_SETTINGS above). The DS's Big Screen is
  // Large Screen + Small ("big" -> "large": a third, on the right, as it was), its One Screen Top Screen Only ("single"
  // -> "top": R3 still shows the touch screen alone). The 3DS's Big Top Screen (Azahar's own until 00.79: the touch
  // screen a quarter of the top screen, at the bottom right) keeps that look: where 3DS Screens is "large", its Small
  // Screen Size and Small Screen Position are written as it was. Every other value kept stays (a view chosen can't be
  // told from a copy of a default: 00.70's rule); the DS's new default, One Above the Other, is what a value never set
  // reads. Once ([global] screens=00.80; the log says what changed). Last, after the changes above.
  if (s_sections["global"]["screens"] != "00.80")
  {
    std::string changed;
    for (auto& [name, values] : s_sections)
    {
      std::string what;
      const auto nds = values.find("nds_layout");
      if (nds != values.end() && (nds->second == "big" || nds->second == "single"))
      {
        const std::string now = nds->second == "big" ? "large" : "top";
        what = "nds_layout " + nds->second + " -> " + now;
        nds->second = now;
      }
      const auto threeds = values.find("3ds_layout");
      if (threeds != values.end() && threeds->second == "large")
      {
        static const std::pair<const char*, const char*> BIG_TOP[] = {{"3ds_small_size", "4"},
                                                                       {"3ds_small_position", "right_bottom"}};
        for (const auto& [key, value] : BIG_TOP)
        {
          if (values.emplace(key, value).second)
            what += std::string(what.empty() ? "" : ", ") + key + " " + value;
        }
      }
      if (!what.empty())
        changed += (changed.empty() ? "" : "; ") + name + " (" + what + ")";
    }
    LogF("settings: the DS's and the 3DS's screens (00.80), once: %s",
         changed.empty() ? "nothing to change" : changed.c_str());
    s_sections["global"]["screens"] = "00.80";
    SaveLocked();
  }
}

void Save()
{
  std::lock_guard lock(s_mutex);
  SaveLocked();
}

std::string Get(const std::string& key, const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  const auto tour = s_tour.find(key);
  if (tour != s_tour.end())
    return tour->second;
  if (key == "quality")
    return QualityLocked(game_id);
  return GetLocked(key, game_id);
}

int GetInt(const std::string& key, const std::string& game_id)
{
  return std::atoi(Get(key, game_id).c_str());
}

bool GetOn(const std::string& key, const std::string& game_id)
{
  const std::string value = Get(key, game_id);
  return value == "on" || value == "enabled";
}

void Set(const std::string& key, const std::string& value, const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  // A game at This Console whose console has no settings of its own yet (UseConsoleByDefault): the console's start
  // now, from the values its game uses (All Games'), and this change is its first.
  if (!game_id.empty() && ModeLocked(game_id) == "console" && ModeLocked(ConsoleSection(game_id)) != "on")
    StartConsoleLocked(game_id);
  auto& section = s_sections[SectionLocked(game_id)];
  if (key == "quality")
  {
    // A level: its values, and which level it was (QualityLocked: two levels can set the same on a console). Custom
    // changes nothing.
    for (const Preset& preset : PRESETS)
    {
      if (value == preset.name)
      {
        for (const auto& [preset_key, preset_value] : preset.values)
          section[preset_key] = preset_value;
        section["quality"] = value;
      }
    }
  }
  else
  {
    section[key] = value;
  }
  SaveLocked();
}

std::vector<std::pair<std::string, std::string>> GlobalValues()
{
  std::lock_guard lock(s_mutex);
  std::vector<std::pair<std::string, std::string>> values;
  auto global = s_sections.find("global");
  if (global != s_sections.end())
    values.assign(global->second.begin(), global->second.end());
  return values;
}

void SetTourValue(const std::string& key, const std::string& value)
{
  std::lock_guard lock(s_mutex);
  s_tour[key] = value;
}

void ClearTourValues()
{
  std::lock_guard lock(s_mutex);
  s_tour.clear();
}

bool IsCustom(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  return !game_id.empty() && SectionLocked(game_id) == game_id;
}

Scope ScopeOf(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  // (This Console before the console has settings of its own: its games follow All Games until one is changed.)
  if (!game_id.empty() && ModeLocked(game_id) == "console")
    return Scope::Console;
  const std::string section = SectionLocked(game_id);
  return section == "global" ? Scope::Global : section.rfind("console:", 0) == 0 ? Scope::Console : Scope::Game;
}

void UseConsoleByDefault(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  // Never chosen: no "custom" value of its own, and the console has no settings of its own yet. The page then shows This
  // Console, but the console gets settings of its own only when one is changed there (Set): opening a game's page
  // alone (00.58 to 00.67: it made them at once, a copy of All Games' values) left that console's games behind every
  // later change of the main Settings - "however I set the graphics, Banjo-Kazooie stays blurry" (its N64 kept Low's
  // native resolution after Quality was set to Ultra for All Games).
  if (!game_id.empty() && ModeLocked(game_id).empty() && SectionLocked(game_id) == "global")
  {
    s_sections[game_id]["custom"] = "console";
    SaveLocked();
  }
}

void SetScope(const std::string& game_id, Scope scope)
{
  if (game_id.empty())
    return;
  std::lock_guard lock(s_mutex);
  // What the game uses now: a new place starts from it.
  std::map<std::string, std::string> now;
  for (const Setting& setting : SETTINGS)
  {
    if (std::string(setting.key) != "quality")
      now[setting.key] = GetLocked(setting.key, game_id);
  }
  // (Its Quality level too, as the level chosen there: QualityLocked.)
  if (const std::string level = QualityLocked(game_id); level != "custom")
    now["quality"] = level;
  if (scope == Scope::Game)
  {
    auto& section = s_sections[game_id];
    if (section.size() <= 1)  // never its own values yet ("custom" only)
    {
      for (const auto& [key, value] : now)
        section[key] = value;
    }
    section["custom"] = "on";
  }
  else if (scope == Scope::Console)
  {
    StartConsoleLocked(game_id);
  }
  else
  {
    s_sections[game_id]["custom"] = "global";
  }
  SaveLocked();
}

void CopyGameToGlobal(const std::string& game_id)
{
  if (game_id.empty())
    return;
  std::lock_guard lock(s_mutex);
  auto game = s_sections.find(game_id);
  if (game == s_sections.end())
    return;
  const SystemId system = SystemOfKey(game_id).id;
  for (const Setting& setting : SETTINGS)
  {
    // (Quality: the level chosen in the game, kept with its values - QualityLocked shows it where they are all set.)
    if (std::string(setting.key) == "language" || !AppliesTo(setting, system))
      continue;
    const auto value = game->second.find(setting.key);
    if (value != game->second.end())
      s_sections["global"][setting.key] = value->second;
  }
  game->second["custom"] = "global";  // it follows them (not its console's)
  SaveLocked();
}

void ResetGlobal(const std::string& key)
{
  std::lock_guard lock(s_mutex);
  auto global = s_sections.find("global");
  if (global != s_sections.end())
    global->second.erase(key);
  SaveLocked();
}

void ResetGame(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections.erase(game_id);
  SaveLocked();
}

void ResetConsole(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections.erase(ConsoleSection(game_id));
  SaveLocked();
}

void ResetAll()
{
  std::lock_guard lock(s_mutex);
  // The language stays: the menus would switch back to English.
  std::string language;
  if (s_sections.count("global") && s_sections["global"].count("language"))
    language = s_sections["global"]["language"];
  s_sections.clear();
  if (!language.empty())
    s_sections["global"]["language"] = language;
  // The one-time migrations (Load) have nothing left to change: done, so a choice made after the reset
  // isn't undone at the next start.
  s_sections["global"]["presets"] = "2";
  s_sections["global"]["dc_pixel"] = "reset";
  s_sections["global"]["quality_levels"] = "00.63";
  s_sections["global"]["dc_levels"] = "00.68";
  s_sections["global"]["n64_2d_ultra"] = "00.68";
  s_sections["global"]["console_copies"] = "00.68";
  s_sections["global"]["levels"] = "00.69";
  s_sections["global"]["3ds_stack"] = "00.69";
  s_sections["global"]["3ds_levels"] = "00.71";
  s_sections["global"]["screens"] = "00.80";
  SaveLocked();
}

bool AppliesToAny(const Setting& setting)
{
  for (const System& system : Systems())
  {
    if (AppliesTo(setting, system.id))
      return true;
  }
  return false;
}

std::string SystemTag(const Setting& setting)
{
  // No tag for a setting of every console this build plays - nor of every one but Flycast's (the Dreamcast, the
  // NAOMI, the Atomiswave) and the 3DS (the picture's, while they played at their own size: Flycast's to 00.67, the
  // 3DS's to 00.70; Quality and the GPU Upscaler are every console's now).
  if ((setting.systems | FOR_FLYCAST | FOR_3DS) == FOR_ALL)
    return {};
  std::string tags;
  bool every = true;
  for (const System& system : Systems())
  {
    if (AppliesTo(setting, system.id))
      tags += (tags.empty() ? "" : ", ") + std::string(system.tab);
    else
      every = false;
  }
  return every ? std::string() : " (" + tags + ")";
}

std::string LabelOf(const Setting& setting, const std::string& value)
{
  for (const Choice& choice : setting.choices)
  {
    if (value == choice.value)
      return choice.label;
  }
  return value;
}

const Choice* FindChoice(const Setting& setting, const std::string& value)
{
  for (const Choice& choice : setting.choices)
  {
    if (value == choice.value)
      return &choice;
  }
  return nullptr;
}

std::string QualityHelp(const std::string& level, SystemId system)
{
  const int at = level == "low" ? 0 : level == "medium" ? 1 : level == "high" ? 2 : level == "ultra" ? 3 : -1;
  if (at < 0)
    return {};
  // The consoles whose picture is pixels (the CPU's): the GPU Upscaler's steps alone (PRESETS, 00.69).
  static const char* const PIXELS[] = {
      "The console's own resolution: its picture as it made it, made bigger for the TV, with no filters.",
      "Smooth Edges: the GPU smooths the jagged edges; the pixels stay as they are.",
      "Pixel Art HD: the GPU redraws the pixels as clean HD shapes at the TV's resolution (xBRZ).",
      "AI: Pixel Art HD, then an AI network (Anime4K) smooths every edge and curve: the smoothest HD."};
  static const char* const NDS[] = {
      "The console's own resolution: both screens as the DS made them (256x192), with no filters.",
      "Smooth Edges on both screens: the GPU smooths the jagged edges.",
      "Pixel Art HD on both screens: redrawn as clean HD shapes at the TV's resolution (xBRZ).",
      "AI on both screens: Pixel Art HD, then an AI network (Anime4K) smooths every edge: the smoothest HD."};
  // The 3D consoles: their own resolution going up; Smooth Edges at Medium and High, sharpened at Ultra (00.69).
  static const char* const N64[] = {
      "The console's own resolution, with no filters: the lightest. The size changes at the next start.",
      "2x (480p) with Smooth Edges. The size changes at the next start.",
      "4x (960p) with Smooth Edges. The size changes at the next start.",
      "4x (960p), made crisper by AMD's sharpening (Natural HD). The size changes at the next start."};
  static const char* const PS1[] = {
      "The console's own resolution (1x), its own textures and colours, with no filters.",
      "2x (480p), smooth textures (xBR) and true colour, with Smooth Edges.",
      "3x (720p), smooth textures, true colour and steady 3D (PGXP), with Smooth Edges.",
      "4x (960p), smooth textures, steady 3D, 4x anti-aliasing (MSAA), made crisper (Natural HD)."};
  static const char* const PSP[] = {
      "The console's own resolution (480x272), with no filters.",
      "2x (960x544), sharper textures far away, with Smooth Edges.",
      "3x (1440x816), the sharpest textures far away, with Smooth Edges.",
      "4x (1920x1088, Full HD), the sharpest textures far away, made crisper (Natural HD)."};
  // Flycast's consoles: Flycast's own resolution and texture sharpness (00.68), the GPU Upscaler's steps since 00.69.
  static const char* const FLYCAST[] = {
      "The console's own resolution (640x480), textures 4x sharp far away: the lightest. The size changes at the "
      "next start.",
      "2x (1280x960), textures 8x sharp far away, with Smooth Edges. The size changes at the next start.",
      "3x (1920x1440, made smaller for the TV), textures 8x sharp far away, with Smooth Edges. The size changes at "
      "the next start.",
      "3x (1920x1440), the sharpest textures far away (16x), made crisper (Natural HD). The size changes at the "
      "next start."};
  // The 3DS (00.71; from 00.64 to 00.70 its own resolution only): 3DS Resolution 1x to 4x, the 3D consoles' GPU Upscaler
  // steps. A bigger size is the GPU's work, but a game that reads its picture back waits for it (none of the games
  // tried on the PC did).
  static const char* const THREEDS[] = {
      "The console's own resolution (400x240 and 320x240), with no filters: the lightest. The size changes at the "
      "next start.",
      "2x (800x480) with Smooth Edges. The size changes at the next start; a game that reads its picture back can "
      "slow down.",
      "3x (1200x720) with Smooth Edges. The size changes at the next start; a game that reads its picture back can "
      "slow down.",
      "4x (1600x960), made crisper by AMD's sharpening (Natural HD). The size changes at the next start; a game that "
      "reads its picture back can slow down."};
  switch (system)
  {
  case SystemId::THREEDS:
    return THREEDS[at];
  case SystemId::N64:
    return N64[at];
  case SystemId::PS1:
    return PS1[at];
  case SystemId::PSP:
    return PSP[at];
  case SystemId::NDS:
    return NDS[at];
  case SystemId::DC:
  case SystemId::NAOMI:
  case SystemId::ATOMISWAVE:
    return FLYCAST[at];
  default:
    return PIXELS[at];
  }
}

std::string Step(const Setting& setting, const std::string& value, int direction)
{
  if (std::string(setting.key) == "quality")
  {
    // The levels only (Custom is where the values match none of them).
    const int count = static_cast<int>(PRESETS.size());
    int index = -1;
    for (int i = 0; i < count; i++)
    {
      if (value == PRESETS[i].name)
        index = i;
    }
    if (index < 0)
      return direction > 0 ? PRESETS.front().name : PRESETS.back().name;
    return PRESETS[((index + direction) % count + count) % count].name;
  }
  const int count = static_cast<int>(setting.choices.size());
  int index = 0;
  for (int i = 0; i < count; i++)
  {
    if (value == setting.choices[i].value)
      index = i;
  }
  index = (index + direction % count + count) % count;
  return setting.choices[index].value;
}
}  // namespace Settings
