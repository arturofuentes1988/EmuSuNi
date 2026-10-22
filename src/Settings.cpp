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
// The categories and their order follow Dolphin for PS4 (Video, Graphics, Performance, System);
// Emulation holds the consoles' own options. A setting for one console only says which (systems).
const std::vector<Setting> SETTINGS = {
    // Video
    // Four levels for every console but Flycast's (the Dreamcast, the NAOMI, the Atomiswave: one fixed level, Standard,
    // Emulator.cpp BuildDcVariables) and the 3DS (its own resolution only, 1x: Build3dsVariables); their pages say so.
    // The values: PRESETS; what a level is on one console's or game's page: QualityHelp (these choices' texts are for
    // a list of every console's settings).
    {"quality", "Quality", "Video",
     {{"custom", "Custom Quality", "Your own choices (they match none of the four levels)."},
      {"low", "Low (fastest)",
       "Each console's own resolution (the N64, the PlayStation, the PSP and the DS at 1x), no upscaler and no "
       "filters."},
      {"medium", "Medium (balanced)", "Smooth Edges; the N64, the PlayStation and the PSP at 2x."},
      {"high", "High (sharpest)", "Natural HD; the N64 and the PlayStation at 4x, the PSP at 3x."},
      {"ultra", "Ultra (maximum)", "Natural AI; the N64, the PlayStation and the PSP at 4x."}},
     "custom", true,
     "The picture in one choice: Low is the console's own; each level above is sharper and more work for the PS4. "
     "Changing one of its settings makes it Custom.",
     FOR_ALL & ~FOR_FLYCAST & ~FOR_3DS},
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
    {"nds_layout", "DS Screens", "Video",
     {{"side", "Side by Side",
       "Both screens the same size, next to each other: the touch screen on the right (R3 swaps them)."},
      {"big", "Big Screen",
       "One screen fills the TV (16:9), the other one small beside it: the top screen big (R3: the touch "
       "screen)."},
      {"stack", "One Above the Other", "As on the DS: the top screen over the touch screen (R3 swaps them)."},
      {"single", "One Screen",
       "Only one screen, as big as the TV allows: the top screen (R3: the touch screen). The touchpad still "
       "touches the touch screen."}},
     "side", true,
     "How the two screens are shown. R3 swaps them. The DualShock 4's touchpad is always the touch screen.",
     FOR_NDS},
    // (One Above the Other first and the default since 00.64: the 3DS as it looks, the top screen over the touch
    // screen.)
    {"3ds_layout", "3DS Screens", "Video",
     {{"stack", "One Above the Other", "As on the 3DS: the top screen over the touch screen (R3 swaps them)."},
      {"side", "Side by Side",
       "The top screen and the touch screen next to each other, as tall as the TV allows (R3 swaps them)."},
      {"large", "Big Top Screen",
       "The top screen big, the touch screen small at the bottom right (R3: the touch screen big)."},
      {"top", "Top Screen Only",
       "Only the top screen, as big as the TV allows. The touch screen isn't shown, so it can't be touched (R3: "
       "the touch screen alone)."},
      {"bottom", "Touch Screen Only",
       "Only the touch screen, as big as the TV allows (R3: the top screen alone)."}},
     "stack", true,
     "How the two screens are shown. R3 swaps them. The DualShock 4's touchpad is the touch screen, wherever it "
     "is shown.",
     FOR_3DS},
    {"ps1_widescreen", "PS1 Widescreen", "Video",
     {{"off", "Off (4:3)", "As the PlayStation drew it."},
      {"on", "On (16:9)", "The 3D drawn wider to fill a widescreen TV (the 2D and some games stay 4:3)."}},
     "off", true, "Shows more of the 3D world on a 16:9 TV (a widescreen hack: some games glitch).", FOR_PS1},
    {"psp_resolution", "PSP Resolution", "Video",
     {{"1", "Native (272p)", "The PSP's own 480x272 picture."},
      {"2", "2x (544p)", "Twice the size: sharper 3D."},
      {"3", "3x (816p)", "Three times: for 720p TVs and up."},
      {"4", "4x (1088p)", "Four times: Full HD, the sharpest 3D."}},
     "2", true, "The size the GPU draws PSP games at: real HD 3D, not an enlarged picture.", FOR_PSP},
    {"dc_resolution", "Dreamcast Resolution", "Video",
     {{"1", "Native (480p)", "The Dreamcast's own 640x480 picture."},
      {"2", "2x (960p)", "Twice the size: sharper 3D."},
      {"3", "3x (1440p)", "Three times: the sharpest on a 1080p TV (made smaller for it)."},
      {"4", "4x (1920p)", "Four times: for 4K TVs (the most GPU time)."}},
     "2", true, "The size the GPU draws Dreamcast games at: real HD 3D, not an enlarged picture.",
     0 /* hidden: the Dreamcast plays at Low (Emulator.cpp BuildDcVariables) */},
    {"3ds_resolution", "3DS Resolution", "Video",
     {{"1", "Native (240p)", "The 3DS's own screens, 400x240 and 320x240: the lightest for the PS4."},
      {"2", "2x (480p)", "Twice the size: sharper 3D, much more work for the GPU."},
      {"3", "3x (720p)", "Three times the size: for 720p TVs, the most work for the GPU."}},
     "1", false,
     "The size the GPU draws 3DS games at: real HD 3D, not an enlarged picture. Most 3D games already run slower "
     "than the 3DS on the PS4: 2x and 3x are for the light ones.",
     0 /* hidden since 00.64: the 3DS plays at its own resolution, 1x (Emulator.cpp Build3dsVariables) */},
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
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/snes/ with the "
       "ROM's name (The Bezel Project's names), or bezels/snes.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SNES},
    {"nes_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "NES", "The NES's light grey and black, with POWER and RESET."},
      {"famicom", "Famicom", "The Famicom's cream, dark red and gold, with the controller's D-pad and buttons."},
      {"black", "Black and Red", "Black with red lines and the controller's buttons."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/nes/ with the "
       "ROM's name (The Bezel Project's names), or bezels/nes.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_NES},
    {"n64_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo 64", "Charcoal grey with the four colours and the stick."},
      {"colors", "Four Colours", "The four colours of the Nintendo 64's logo, as bands."},
      {"atomic", "Atomic Purple", "See-through purple, like the Atomic Purple console."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/n64/ with the "
       "ROM's name (The Bezel Project's names), or bezels/n64.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_N64},
    {"gb_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Game Boy", "The first Game Boy's grey, its screen frame, the D-pad and the A and B buttons."},
      {"pocket", "Game Boy Pocket", "The Pocket's silver, slimmer frame and black buttons."},
      {"green", "Dot Matrix", "The green of the Game Boy's screen, with its dots."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gb/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gb.png for every game."}},
     "off", true, "The handheld around the picture, see-through where the game goes.", FOR_GB},
    {"gbc_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Berry", "The Game Boy Color in berry, with COLOR in its five colours."},
      {"teal", "Teal", "The Game Boy Color in teal."},
      {"atomic", "Atomic Purple", "See-through purple, the insides showing."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gbc/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gbc.png for every game."}},
     "off", true, "The handheld around the picture, see-through where the game goes.", FOR_GBC},
    {"gba_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Indigo", "The Game Boy Advance in indigo, with L and R."},
      {"glacier", "Glacier", "See-through ice blue, the insides showing."},
      {"black", "Black", "A black Game Boy Advance with silver lines."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gba/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gba.png for every game."}},
     "off", true, "The handheld around the picture, see-through where the game goes.", FOR_GBA},
    {"ps1_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "PlayStation", "The PlayStation's grey, with the four symbols and its disc lid."},
      {"psone", "PSone", "The PSone's white and silver, round and slim."},
      {"dark", "Midnight", "Black and deep blue, with the four symbols."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/ps1/ with the "
       "ROM's name (The Bezel Project's names), or bezels/ps1.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_PS1},
    // The Sega consoles (00.63: a design of their own each).
    {"sg1000_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "SG-1000", "Black with red and white lines, the joystick and buttons 1 and 2."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/sg1000/ with the "
       "ROM's name (The Bezel Project's names), or bezels/sg1000.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SG1000},
    {"sms_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Master System", "Black with red lines and the ribs on top, the pad and buttons 1 and 2."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/sms/ with the "
       "ROM's name (The Bezel Project's names), or bezels/sms.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SMS},
    {"gg_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Game Gear", "The handheld in black around its screen: the D-pad, START and buttons 1 and 2."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gg/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gg.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_GG},
    {"md_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Genesis", "Black with the round grooves, the D-pad and the A, B and C buttons."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/md/ with the "
       "ROM's name (The Bezel Project's names), or bezels/md.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_MD},
    // The Arcade: the shared frames (its own: the Arcade Cabinet), or the user's own (The Bezel Project names its
    // arcade bezels after the romsets, as the games' files are: mslug.png).
    {"arcade_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/arcade/ with the "
       "romset's name (The Bezel Project's names: mslug.png), or bezels/arcade.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_ARCADE},
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
     "off", true,
     "A frame around the picture, see-through where the game goes. The PSP's picture is as wide as the TV: in a frame "
     "it is made smaller (about 83%).",
     FOR_PSP},
    {"nds_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo DS", "The DS in white around its screens, made for each way DS Screens shows them."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/nds/ with the "
       "game's name, or bezels/nds.png for every game."}},
     "off", true,
     "A frame around the screens, see-through where they go. Side by side or with the big screen, the picture is made "
     "smaller to fit in it.",
     FOR_NDS},
    {"3ds_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo 3DS", "The 3DS in aqua blue around its screens, made for each way 3DS Screens shows them."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/3ds/ with the "
       "game's name, or bezels/3ds.png for every game."}},
     "off", true,
     "A frame around the screens, see-through where they go. Side by side, with the big top screen or the top "
     "screen only, the picture is made smaller to fit in it.",
     FOR_3DS},
    {"dc_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Dreamcast", "The console's white, its round lid and the controller's four coloured buttons."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/dc/ with the "
       "game's name, or bezels/dc.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_DC},
    {"naomi_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "NAOMI Cabinet", "A white arcade cabinet with gold trim, its stick and six buttons."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/naomi/ with the "
       "romset's name (mvsc2.png), or bezels/naomi.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_NAOMI},
    {"atomiswave_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Atomiswave Cabinet", "A black arcade cabinet with green lights, its stick, buttons and cartridge."},
      EMUSUNI_SHARED_FRAMES,
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/atomiswave/ with the "
       "romset's name (kofxi.png), or bezels/atomiswave.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_ATOMISWAVE},
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
       "still natural, never painted. Quality High uses it."},
      {"natural_ai", "Natural AI",
       "Natural HD's clean edges with an AI network (Anime4K) mixed into the picture, so textures and "
       "dithering stay: the smoothest look without the waxy one. Quality Ultra uses it."},
      {"pixel", "Pixel Art HD (xBRZ)",
       "SNES and NES redrawn by the GPU at the TV's full resolution: rounded, clean edges without blur; "
       "frees the CPU. The N64 gets Smooth HD."},
      {"ai", "AI (Anime4K)",
       "xBRZ, then a neural network (Anime4K) at full strength cleans every edge and curve: the "
       "smoothest, most painted HD look for SNES, NES and the Game Boys. The N64 gets Smooth HD."},
      {"fsr", "Smooth HD (AMD FSR)",
       "AMD FidelityFX Super Resolution: smooth upscaling with sharp edges, made for 3D (the N64 at "
       "Native and 2x; at 4x it sharpens)."},
      {"sharpen", "HD Sharpen (AMD RCAS)",
       "The picture as it is, made crisper by AMD's sharpening (RCAS): for the N64 at 4x and pictures "
       "already made bigger."}},
     "off", true,
     "Draws the picture with the GPU's filters at the TV's resolution. Every filter but HD Sharpen "
     "starts from the console's own picture: Resolution and the Upscaling Filter are left out.",
     FOR_ALL & ~FOR_FLYCAST & ~FOR_3DS},
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
       "Text, icons and 2D backgrounds at the N64 Resolution: smoother; some games show thin lines "
       "between their pieces."}},
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
     "16", true, "How sharp the textures of far, slanted surfaces stay (anisotropic filtering, on the GPU).",
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
     "8", true, "How sharp the textures of far, slanted surfaces stay (anisotropic filtering, on the GPU).",
     0 /* hidden: the Dreamcast plays at Low */},
    {"dc_sorting", "Dreamcast Transparency", "Graphics",
     {{"triangle", "Normal (per triangle)",
       "Transparent surfaces sorted per triangle: fast; a few games show some in the wrong order."},
      {"pixel", "Accurate (per pixel)",
       "Sorted per pixel, as the Dreamcast did: heavier and about 0.5 GB of GPU memory. Experimental: on "
       "the PS4 the picture can come out in wrong colours (Crazy Taxi did)."}},
     "triangle", false, "How overlapping transparent surfaces (smoke, glass, water) are drawn.",
     0 /* hidden: the Dreamcast plays at Low */},
    {"psp_frame_rate", "PSP Frame Rate (God of War)", "Performance",
     {{"auto", "Auto", "60 FPS while the PS4 keeps up; 30 for the rest of the game once it can't (no slowdown)."},
      {"60", "60 FPS", "The smoothest; heavy fights slow the game down."},
      {"30", "30 FPS", "Half the work (PPSSPP's hack for weak hardware): a steady full speed."}},
     "auto", true, "God of War's frame rate on the PS4 (the God of War games only; others keep theirs).", FOR_PSP},
    {"3ds_clock", "3DS CPU Clock", "Performance",
     {{"100", "100% (as the 3DS)", "The 3DS's own speed."},
      {"75", "75%", "The game sees a slower 3DS: a quarter less work for the PS4 where the game keeps up."},
      {"50", "50%", "The game sees a 3DS at half speed: half the work for the PS4, the games that need the speed "
                    "slow down or stutter."}},
     "100", true,
     "How fast the emulated 3DS's processor runs. Lower is lighter for the PS4, which most 3D games need; a game may "
     "freeze or slow down if it needs the speed.",
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
     "disabled", true, "Draws more sprites per line than the console could (less flicker).", FOR_RETRO | FOR_SEGA},
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
     {{"fast", "Fast (asynchronous)", "The GPU draws while the N64 runs: much smoother on the PS4."},
      {"accurate", "Accurate (synchronous)",
       "The N64 waits for each picture: slower. For the few games whose effects read the screen back "
       "(camera photos, some pause screens)."}},
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
     FOR_RETRO | FOR_PSP | FOR_NDS | FOR_FLYCAST | FOR_SEGA | FOR_ARCADE},
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
    // The PSP's stick (issue #8): PPSSPP's dead zone and sensitivity (Emulator.cpp BuildPspVariables), 15% and 100%
    // as before by default.
    {"psp_stick_deadzone", "PSP Stick Dead Zone", "Emulation",
     {{"0", "0%", "No dead zone: the slightest touch moves. A stick that rests a little off centre drifts."},
      {"5", "5%", "Very small."},
      {"10", "10%", "Small."},
      {"15", "15% (default)", "PPSSPP's own."},
      {"20", "20%", "Larger: for a stick that drifts at rest."},
      {"25", "25%", "Larger still."},
      {"30", "30%", "The largest."}},
     "15", false, "How far the left stick goes before the PSP sees it move.", FOR_PSP},
    {"psp_stick_sensitivity", "PSP Stick Sensitivity", "Emulation",
     {{"100", "100% (default)", "The stick all the way out is the PSP's all the way out."},
      {"110", "110%", "The PSP's full push with the stick 92% of the way out (with a 15% dead zone)."},
      {"125", "125%", "The PSP's full push with the stick 83% of the way out (with a 15% dead zone)."},
      {"150", "150%", "The PSP's full push with the stick 72% of the way out (with a 15% dead zone): for games "
                      "that run only with the stick all the way out."},
      {"175", "175%", "The PSP's full push with the stick 64% of the way out (with a 15% dead zone)."},
      {"200", "200%", "The PSP's full push with the stick 58% of the way out (with a 15% dead zone)."}},
     "100", false, "How far the PSP sees the left stick pushed, for how far it is pushed.", FOR_PSP},
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
// its textures not upscaled). Above it the GPU Upscaler goes one step per level: Medium Smooth Edges,
// High Natural HD, Ultra Natural AI (each the one before plus more, GpuFilter.h), for every console (Resolution 3x/6x
// stays the CPU's picture while the GPU's filters compile); the 3D consoles' resolution goes up with it: what the PS4
// carries at full speed (its logs: the N64 at 4x 96-100% in Mario 64, Banjo-Tooie, Donkey Kong 64, Perfect Dark - the
// heavy games are as slow at 1x, by the CPU -; the PSP's God of War and Tekken 6 at 4x 94-100% since 00.47; the
// PlayStation's Gran Turismo 2 and Tekken 3 used a quarter of the GPU at most in 00.33). The 3DS has no levels since
// 00.64 (its own resolution only, as the Dreamcast has one: most of its 3D games are slow at 1x already, by the CPU;
// 00.63 set it 1x, 2x, 3x, 3x). The levels leave the player's own look as it is: the picture's size, zoom and shape, the
// bezels, Tube TV, Scanlines and the NTSC Filter (00.63: the size, zoom and NTSC Filter were set by every level until
// 00.62, the same at each).
const std::vector<Preset> PRESETS = {
    {"low", {{"resolution", "1"}, {"upscaler", "xbrz"}, {"n64_resolution", "1"}, {"n64_sync", "fast"},
             {"n64_2d", "native"}, {"gpu_filter", "off"}, {"ps1_resolution", "1"}, {"ps1_texture_filter", "Nearest"},
             {"ps1_colors", "dither"}, {"ps1_pgxp", "off"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "1"},
             {"psp_anisotropy", "0"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
             {"dc_resolution", "1"}, {"dc_anisotropy", "4"}, {"dc_sorting", "triangle"}}},
    {"medium", {{"resolution", "3"}, {"upscaler", "xbrz"}, {"n64_resolution", "2"}, {"n64_sync", "fast"},
                {"n64_2d", "native"}, {"gpu_filter", "edges"}, {"ps1_resolution", "2"}, {"ps1_texture_filter", "xBR"},
                {"ps1_colors", "true"}, {"ps1_pgxp", "off"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "2"},
                {"psp_anisotropy", "8"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
                {"dc_resolution", "2"}, {"dc_anisotropy", "8"}, {"dc_sorting", "triangle"}}},
    {"high", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"n64_resolution", "4"}, {"n64_sync", "fast"},
              {"n64_2d", "native"}, {"gpu_filter", "natural"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
              {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "3"},
              {"psp_anisotropy", "16"}, {"psp_texture_shader", "off"}, {"psp_antialiasing", "1"},
              {"dc_resolution", "3"}, {"dc_anisotropy", "16"}, {"dc_sorting", "triangle"}}},
    // Ultra for the PlayStation: 4x, xBR textures, True Colour, PGXP, 4x MSAA, then Natural AI (the AI
    // chosen on real Tekken 3 and Gran Turismo 2 frames, mixed with its input: no waxy look). The PSP
    // at 4x without MSAA: with it every new pipeline took ~86 ms to compile on the PS4 (5-19 ms
    // without) and God of War stuttered; a 4x picture gets Natural HD's sharpening, not the AI.
    {"ultra", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"n64_resolution", "4"}, {"n64_sync", "fast"},
               {"n64_2d", "hd"}, {"gpu_filter", "natural_ai"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
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
      // (Not the settings no console shows: the Dreamcast's, which it doesn't follow - it plays at Standard.)
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
  // What the levels set since 00.63 (the PSP's textures not upscaled; the 3DS's resolution, 00.63 only): in no file
  // of before, so the comparisons with the levels below leave it out (a 3DS's value is never added: its games stay as
  // they were).
  auto since_0063 = [](const std::string& key) { return key == "psp_texture_shader" || key == "3ds_resolution"; };
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
  const std::string section = SectionLocked(game_id);
  return section == "global" ? Scope::Global : section.rfind("console:", 0) == 0 ? Scope::Console : Scope::Game;
}

void UseConsoleByDefault(const std::string& game_id)
{
  bool never_chosen;
  {
    std::lock_guard lock(s_mutex);
    // Never chosen: no "custom" value of its own, and the console has no settings of its own yet.
    never_chosen = !game_id.empty() && ModeLocked(game_id).empty() && SectionLocked(game_id) == "global";
  }
  if (never_chosen)
    SetScope(game_id, Scope::Console);
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
    const std::string console = ConsoleSection(game_id);
    auto& section = s_sections[console];
    if (ModeLocked(console) != "on")
    {
      if (section.size() <= 1)  // the console's first settings: the ones its game uses now
      {
        const SystemId system = SystemOfKey(game_id).id;
        for (const Setting& setting : SETTINGS)
        {
          if (now.count(setting.key) && AppliesTo(setting, system))
            section[setting.key] = now[setting.key];
        }
      }
      section["custom"] = "on";
    }
    s_sections[game_id]["custom"] = "console";
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
  // NAOMI, the Atomiswave) and the 3DS (the picture's: they play at their own size, which Quality's and the 3DS's
  // header say).
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
  // (No levels for Flycast's consoles, Standard, nor the 3DS, its own resolution: their pages say so.)
  if (at < 0 || IsFlycast(system) || system == SystemId::THREEDS)
    return {};
  // The consoles whose picture is pixels (the CPU's): the GPU Upscaler's steps alone (PRESETS).
  static const char* const PIXELS[] = {
      "The console's own resolution: its picture as it made it, made bigger for the TV, with no filters.",
      "Smooth Edges: the GPU smooths the jagged edges; the pixels stay as they are.",
      "Natural HD: smooth edges, then a sharp HD picture at the TV's resolution (AMD FSR).",
      "Natural AI: Natural HD with an AI network (Anime4K) that cleans every edge; the textures stay."};
  static const char* const NDS[] = {
      "The console's own resolution: both screens as the DS made them (256x192), with no filters.",
      "Smooth Edges on both screens: the GPU smooths the jagged edges.",
      "Natural HD on both screens: smooth edges, then a sharp HD picture (AMD FSR).",
      "Natural AI on both screens: Natural HD with an AI network (Anime4K) that cleans every edge."};
  static const char* const N64[] = {
      "The console's own resolution, with no filters: the lightest. The size changes at the next start.",
      "2x (480p) with Smooth Edges. The size changes at the next start.",
      "4x (960p) with Natural HD. The size changes at the next start.",
      "4x (960p) with Natural AI, and smooth HD text and icons. The size changes at the next start."};
  static const char* const PS1[] = {
      "The console's own resolution (1x), its own textures and colours, with no filters.",
      "2x (480p), smooth textures (xBR) and true colour, with Smooth Edges.",
      "4x (960p), smooth textures, true colour and steady 3D (PGXP), with Natural HD.",
      "4x (960p), smooth textures, steady 3D, 4x anti-aliasing (MSAA) and Natural AI."};
  static const char* const PSP[] = {
      "The console's own resolution (480x272), with no filters.",
      "2x (960x544), sharper textures far away, with Smooth Edges.",
      "3x (1440x816), the sharpest textures far away, with Natural HD.",
      "4x (1920x1088, Full HD), the sharpest textures far away, made crisper (Natural HD)."};
  switch (system)
  {
  case SystemId::N64:
    return N64[at];
  case SystemId::PS1:
    return PS1[at];
  case SystemId::PSP:
    return PSP[at];
  case SystemId::NDS:
    return NDS[at];
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
