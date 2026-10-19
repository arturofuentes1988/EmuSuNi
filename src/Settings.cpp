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
// The categories and their order follow Dolphin for PS4 (Video, Graphics, Performance, System);
// Emulation holds the consoles' own options. A setting for one console only says which (systems).
const std::vector<Setting> SETTINGS = {
    // Video
    {"quality", "Quality", "Video",
     {{"custom", "Custom Quality", "Your own choices (they match none of the three levels)."},
      {"low", "Low (fastest)", "The fastest: the consoles' own pictures, fit to the screen; the N64 at its own size."},
      {"medium", "Medium (balanced)",
       "Smooth Edges: the GPU smooths the jagged edges and leaves the pixels and textures as they are; "
       "the N64, the PlayStation and the PSP at 2x."},
      {"high", "High (sharpest)",
       "Natural HD: smooth edges, then a sharp HD picture at the TV's resolution (AMD FSR) that still "
       "looks natural; the N64 and the PlayStation at 4x, the PSP at 3x. Heavy N64 games "
       "can slow down."},
      {"ultra", "Ultra (maximum)",
       "The best each console can do: Natural AI, an AI network (Anime4K) that cleans every edge and keeps "
       "the textures (no waxy look); the N64 at 4x with HD 2D; the PlayStation at 4x with smooth textures, "
       "steady 3D and anti-aliasing; the PSP at 4x. Heavy "
       "N64 games can slow down."}},
     "custom", true,
     "Sets the picture for a level at once: resolution, upscaling (CPU and GPU), picture size, and the "
     "N64's size, sync and 2D. Changing one of them makes it Custom. The Dreamcast always plays at Low "
     "(the PS4 can't do more yet).",
     FOR_ALL & ~FOR_DC},
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
     {{"auto", "Auto (4:3)", "As on a TV of the time: a little wider than the console's own pixels."},
      {"pixel", "Pixel Perfect (8:7)", "Square pixels, exactly as the console draws them (a little narrower)."},
      {"wide", "16:9", "Stretched to a widescreen TV (everything looks wider)."},
      {"stretch", "Stretch", "Fills the whole screen, whatever its shape."}},
     "auto", true,
     "The picture's shape on the TV. 4:3 is how the consoles looked on a TV; the Game Boys keep their "
     "screen's own shape."},
    {"nds_layout", "DS Screens", "Video",
     {{"side", "Side by Side", "Both screens the same size, next to each other: the touch screen on the right."},
      {"big", "Big Top Screen", "The top screen fills the TV (16:9), the touch screen small beside it."},
      {"stack", "One Above the Other", "As on the DS: the top screen over the touch screen."}},
     "side", true,
     "How the two screens are shown. R3 swaps between this and Side by Side (or Big Top Screen). The "
     "DualShock 4's touchpad is the touch screen.", FOR_NDS},
    {"ps1_widescreen", "PS1 Widescreen", "Video",
     {{"off", "Off (4:3)", "As the PlayStation drew it."},
      {"on", "On (16:9)", "The 3D drawn wider to fill a widescreen TV (the 2D and some games stay 4:3)."}},
     "off", true, "Shows more of the 3D world on a 16:9 TV (a widescreen hack: some games glitch).", FOR_PS1},
    {"psp_resolution", "PSP Resolution", "Video",
     {{"1", "Native (272p)", "The PSP's own 480x272 picture."},
      {"2", "2x (544p)", "Twice the size: sharper 3D."},
      {"3", "3x (816p)", "Three times: for 720p TVs and up."},
      {"4", "4x (1088p)", "Four times: Full HD, the sharpest 3D."}},
     "2", false, "The size the GPU draws PSP games at: real HD 3D, not an enlarged picture.", FOR_PSP},
    {"dc_resolution", "Dreamcast Resolution", "Video",
     {{"1", "Native (480p)", "The Dreamcast's own 640x480 picture."},
      {"2", "2x (960p)", "Twice the size: sharper 3D."},
      {"3", "3x (1440p)", "Three times: the sharpest on a 1080p TV (made smaller for it)."},
      {"4", "4x (1920p)", "Four times: for 4K TVs (the most GPU time)."}},
     "2", true, "The size the GPU draws Dreamcast games at: real HD 3D, not an enlarged picture.",
     0 /* hidden: the Dreamcast plays at Low (Emulator.cpp BuildDcVariables) */},
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
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/snes/ with the "
       "ROM's name (The Bezel Project's names), or bezels/snes.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_SNES},
    {"nes_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "NES", "The NES's light grey and black, with POWER and RESET."},
      {"famicom", "Famicom", "The Famicom's cream, dark red and gold, with the controller's D-pad and buttons."},
      {"black", "Black and Red", "Black with red lines and the controller's buttons."},
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/nes/ with the "
       "ROM's name (The Bezel Project's names), or bezels/nes.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_NES},
    {"n64_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Nintendo 64", "Charcoal grey with the four colours and the stick."},
      {"colors", "Four Colours", "The four colours of the Nintendo 64's logo, as bands."},
      {"atomic", "Atomic Purple", "See-through purple, like the Atomic Purple console."},
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/n64/ with the "
       "ROM's name (The Bezel Project's names), or bezels/n64.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_N64},
    {"gb_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Game Boy", "The first Game Boy's grey, its screen frame, the D-pad and the A and B buttons."},
      {"pocket", "Game Boy Pocket", "The Pocket's silver, slimmer frame and black buttons."},
      {"green", "Dot Matrix", "The green of the Game Boy's screen, with its dots."},
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gb/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gb.png for every game."}},
     "off", true, "The handheld around the picture, see-through where the game goes.", FOR_GB},
    {"gbc_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Berry", "The Game Boy Color in berry, with COLOR in its five colours."},
      {"teal", "Teal", "The Game Boy Color in teal."},
      {"atomic", "Atomic Purple", "See-through purple, the insides showing."},
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gbc/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gbc.png for every game."}},
     "off", true, "The handheld around the picture, see-through where the game goes.", FOR_GBC},
    {"gba_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "Indigo", "The Game Boy Advance in indigo, with L and R."},
      {"glacier", "Glacier", "See-through ice blue, the insides showing."},
      {"black", "Black", "A black Game Boy Advance with silver lines."},
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/gba/ with the "
       "ROM's name (The Bezel Project's names), or bezels/gba.png for every game."}},
     "off", true, "The handheld around the picture, see-through where the game goes.", FOR_GBA},
    {"ps1_bezel", "Bezel", "Video",
     {{"off", "Off", "The picture alone, on black."},
      {"console", "PlayStation", "The PlayStation's grey, with the four symbols and its disc lid."},
      {"psone", "PSone", "The PSone's white and silver, round and slim."},
      {"dark", "Midnight", "Black and deep blue, with the four symbols."},
      {"tv", "Retro TV", "An old wooden tube TV around the picture."},
      {"custom", "Your Own",
       "Your own 1920x1080 PNG, see-through where the game goes: /data/EmuSuNi/bezels/ps1/ with the "
       "ROM's name (The Bezel Project's names), or bezels/ps1.png for every game."}},
     "off", true, "A frame over the sides of the TV, see-through where the game goes.", FOR_PS1},
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
     FOR_ALL & ~FOR_DC},
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
     "1", false, "Smooths the jagged edges of the 3D (multisampling, on the GPU).", FOR_PSP},
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
     "off", true, "Dark lines between the picture's rows, like a CRT TV; on the Game Boys, the grid between their screen's dots."},
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
     "disabled", true, "Draws more sprites per line than the console could (less flicker).", FOR_RETRO},
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
     "off", true, "The handheld's screen kept a little of the frame before; some games count on it.", FOR_HANDHELD},
    // Performance
    {"cpu_clock", "Emulated CPU Clock", "Performance",
     {{"disabled", "100%", "The console's own speed."},
      {"light", "Light Overclock", "A little faster: less slowdown, safe in most games."},
      {"compatible", "Overclock", "Faster: removes most slowdown; some games glitch."},
      {"max", "Max Overclock", "Much faster; more games glitch."}},
     "disabled", true,
     "Overclocking the console's CPU removes slowdown in busy scenes (Gradius III, Super R-Type); "
     "a few games glitch.", FOR_RETRO},
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
     "3", true, "Speed while the Fast Forward button is held (Controls: R2; on the N64, the touchpad)."},
    {"frameskip", "Frame Skip", "Performance",
     {{"off", "Off", "Every frame drawn."},
      {"auto", "Auto", "Skips drawing a frame only when the PS4 falls behind."},
      {"1", "1", "Draws every other frame."},
      {"2", "2", "Draws one frame in three."}},
     "off", true, "Skips drawing some frames when the PS4 can't keep up (the game keeps its speed).",
     FOR_RETRO | FOR_PSP | FOR_NDS | FOR_DC},
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
     "auto", false, "Auto uses the game's own region.", FOR_RETRO | FOR_PS1},
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
    {"ps1_boot", "PS1 Boot Logo", "Emulation",
     {{"on", "On", "The PlayStation's start-up logo and sound (with its BIOS)."},
      {"skip", "Skip It", "The game starts at once."}},
     "on", false,
     "Copy the PlayStation BIOS (for example scph5501.bin) to /data/EmuSuNi/system: without it a "
     "built-in BIOS starts the games, which works with fewer of them.", FOR_PS1},
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
     "off", true, "Softens the handhelds' sharp sound.", FOR_HANDHELD},
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

const std::vector<const char*> CATEGORIES = {"Video", "Graphics", "Performance", "Emulation",
                                             "System"};

// Quality's levels (Video): the values each one sets. Quality isn't stored: it is the level whose
// values are all set, else Custom.
struct Preset
{
  const char* name;
  std::vector<std::pair<const char*, const char*>> values;
};
// The GPU Upscaler goes one step per level: Medium Smooth Edges, High Natural HD, Ultra Natural AI
// (each the one before plus more, GpuFilter.h), for every console (Resolution 3x/6x stays the CPU's
// picture while the GPU's filters compile).
const std::vector<Preset> PRESETS = {
    {"low", {{"resolution", "1"}, {"upscaler", "xbrz"}, {"ntsc_filter", "disabled"}, {"scaling", "fit"},
             {"zoom", "off"}, {"n64_resolution", "1"}, {"n64_sync", "fast"}, {"n64_2d", "native"},
             {"gpu_filter", "off"}, {"ps1_resolution", "1"}, {"ps1_texture_filter", "Nearest"},
             {"ps1_colors", "dither"}, {"ps1_pgxp", "off"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "1"},
             {"psp_anisotropy", "4"}, {"psp_antialiasing", "1"}, {"dc_resolution", "1"}, {"dc_anisotropy", "4"},
             {"dc_sorting", "triangle"}}},
    {"medium", {{"resolution", "3"}, {"upscaler", "xbrz"}, {"ntsc_filter", "disabled"}, {"scaling", "fit"},
                {"zoom", "off"}, {"n64_resolution", "2"}, {"n64_sync", "fast"}, {"n64_2d", "native"},
                {"gpu_filter", "edges"}, {"ps1_resolution", "2"}, {"ps1_texture_filter", "xBR"},
                {"ps1_colors", "true"}, {"ps1_pgxp", "off"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "2"},
                {"psp_anisotropy", "8"}, {"psp_antialiasing", "1"}, {"dc_resolution", "2"},
                {"dc_anisotropy", "8"}, {"dc_sorting", "triangle"}}},
    {"high", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"ntsc_filter", "disabled"}, {"scaling", "fit"},
              {"zoom", "off"}, {"n64_resolution", "4"}, {"n64_sync", "fast"}, {"n64_2d", "native"},
              {"gpu_filter", "natural"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
              {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "1"}, {"psp_resolution", "3"},
              {"psp_anisotropy", "16"}, {"psp_antialiasing", "1"}, {"dc_resolution", "3"},
              {"dc_anisotropy", "16"}, {"dc_sorting", "triangle"}}},
    // Ultra for the PlayStation: 4x, xBR textures, True Colour, PGXP, 4x MSAA, then Natural AI (the AI
    // chosen on real Tekken 3 and Gran Turismo 2 frames, mixed with its input: no waxy look). The PSP
    // at 4x without MSAA: with it every new pipeline took ~86 ms to compile on the PS4 (5-19 ms
    // without) and God of War stuttered; a 4x picture gets Natural HD's sharpening, not the AI.
    {"ultra", {{"resolution", "6"}, {"upscaler", "xbrz"}, {"ntsc_filter", "disabled"}, {"scaling", "fit"},
               {"zoom", "off"}, {"n64_resolution", "4"}, {"n64_sync", "fast"}, {"n64_2d", "hd"},
               {"gpu_filter", "natural_ai"}, {"ps1_resolution", "4"}, {"ps1_texture_filter", "xBR"},
               {"ps1_colors", "true"}, {"ps1_pgxp", "on"}, {"ps1_antialiasing", "4"}, {"psp_resolution", "4"},
               {"psp_anisotropy", "16"}, {"psp_antialiasing", "1"}, {"dc_resolution", "3"},
               {"dc_anisotropy", "16"}, {"dc_sorting", "triangle"}}},
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

// The level whose values the game (or the global settings) uses, else "custom".
std::string QualityLocked(const std::string& game_id)
{
  for (const Preset& preset : PRESETS)
  {
    bool same = true;
    for (const auto& [key, value] : preset.values)
    {
      // (Not the settings no console shows: the Dreamcast's, which it doesn't follow - it plays at Low.)
      const Setting* setting = Find(key);
      if (setting && setting->systems == 0)
        continue;
      same = same && GetLocked(key, game_id) == value;
    }
    if (same)
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
  // The consoles that came after the Quality levels (their values left out where a level chosen
  // before them is recognised).
  static const char* const NEWER[] = {"psp_", "dc_"};
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
          else if (!newer)
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
  // A Quality level chosen before a console came (the PSP: 00.35 and older; the Dreamcast: 00.47 and
  // older) stays that level: the level's values for that console are added to the section that has
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
          if (!newer(key))
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
    // A level: its values (Custom changes nothing).
    for (const Preset& preset : PRESETS)
    {
      if (value == preset.name)
      {
        for (const auto& [preset_key, preset_value] : preset.values)
          section[preset_key] = preset_value;
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
    if (std::string(setting.key) == "language" || std::string(setting.key) == "quality" ||
        !AppliesTo(setting, system))
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
  // No tag for a setting of every console this build plays - nor of every one but the Dreamcast (the
  // picture's: it plays at Low, which Quality's help says).
  if ((setting.systems | FOR_DC) == FOR_ALL)
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
