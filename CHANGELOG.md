# Changelog

All notable changes to EmuSuNi. Versions follow the PS4 package version (`XX.YY`).

## 00.19 (public release, 2026-10-16)
The first public release since 00.18: the test builds 00.28 to 00.32 below.
- **Game Boy, Game Boy Color and Game Boy Advance** (mGBA): six consoles in one app, with their
  tabs, folders (`Roms/gb`, `gbc`, `gba` on the USB drive by default), box art, controls, bezels
  shaped like the handhelds, AI upscaling, Game Boy Color / Super Game Boy colours, LCD effects,
  the solar and tilt sensors, rumble and the cartridges' clocks.
- **Nintendo 64**: much less CPU per frame (a lock-free command ring, faster RSP, the picture made
  on the graphics thread), even frame pacing, GoldenEye 007 smoother (bit-exact core speed-ups and
  N64 CPU Timing), and the **controls fixed** (R, L, C Up and C Down didn't reach the core) with a
  comfortable layout: Z on both triggers.
- The app uses the PS4's time zone (the menu's clock, the Pokémon clocks); sound above 57 kHz is
  low-passed before resampling.

## 00.32 (test build)
- The screenshot tour's settings (`set` in tour.txt) are its own: above every game's, console's and
  global value, never written to settings.ini.

## 00.31 (test build)
- **The N64's buttons fixed.** The app sent them in Mupen64Plus-Next's "Independent C-button
  Controls" layout but left that option off, so the core read them its other way: R (R1) switched
  its C-button mode (A and B stopped working while it was held), C Up and C Down on Triangle and
  Circle and the L button did nothing. The option is on now (`alt-map`).
- **A more comfortable N64 layout by default:** left stick the control stick, right stick the C
  buttons, Cross A, Square B, **Z on both L2 and R2** (fire in GoldenEye and Perfect Dark, target
  in Zelda with either trigger), R1 R, L1 L, Triangle / Circle C Up / C Down, Options START, the
  D-pad the D-pad, and fast forward on the touchpad (held). The N64 layout saved before is dropped
  once, as it was set around the fault (a game's own layout stays). A control can now press a
  button together with another one (the two Z rows).
- A Game Boy game in the Game Boy Color's own folder is listed under GBC, and the other way round
  (Pokémon Yellow is a .gb file made for both): the folder decides, unless the two consoles share
  one.

## 00.30 (test build)
- **GoldenEye 007 smoother.** Its microcode hands the RDP one command at a time (about 2900 per
  frame) and puts a whole frame's graphics into one VI out of two or three (30-45 ms on the PS4:
  the stutter and the sound running dry). Four bit-exact core changes (`scripts/build-n64-core.sh`):
  the RDP command ring's producer copies a command at once; ParaLLEl-RSP jumps between compiled
  blocks and reads/writes the plain COP0 registers without calls into C; the dynarec's JR lookup
  has a cache in front. On the PC: about 12% less RSP time and 8% less per VI in GoldenEye, the
  same pictures, sound and RDRAM (also checked with Perfect Dark and Donkey Kong 64).
- **N64 CPU Timing** (Settings -> Performance, Auto by default): Light (3 counts per instruction,
  as the core's own list has for 25 games) makes a game that drew few frames on the console draw
  about as few; the game keeps its speed. Auto uses it for GoldenEye 007 (about 23 frames a second
  instead of 33, a third less graphics work for the PS4; its loading takes longer), Normal for the
  others. The log names the cartridge and the timing.

## 00.29 (test build)
- **Game Boy, Game Boy Color and Game Boy Advance**, with mGBA (libretro's, MPL-2.0) built into the
  app (`scripts/build-gba-core.sh`). Each console has its tab, its game folder (`/usb0/Roms/gb`,
  `gbc`, `gba` until another is chosen in Settings; .zip files work), its covers (libretro's
  thumbnails) and its controls (B and A on Cross and Circle, turbo B and A on Square and Triangle,
  the GBA's L and R on L1 and R1).
- The picture keeps the screen's shape (10:9, the GBA's 3:2) and gets everything the SNES and NES
  have: Resolution with xBRZ/HQx, the GPU Upscaler (Pixel Art HD, AI with Anime4K, Smooth HD), the
  Quality levels (Ultra: the AI), Picture Size and Integer Scale. Scanlines draw the grid between
  the LCD's dots.
- New settings: **Game Boy Colours** (as a Game Boy Color showed them by default, the first Game
  Boy's green, Pocket, Light, grey, or the Super Game Boy with its colours and border), **Screen
  Colours** (as the GBC/GBA screen showed them), **LCD Ghosting** (Smart blending for the games
  that flicker for see-through effects), **Boot Logo (BIOS)** (optional BIOS files in
  /data/EmuSuNi/system), **Sunlight** for Boktai, **Tilt Sensor** (the right stick, or the DualShock
  4's own motion sensors) for WarioWare: Twisted!, Yoshi Topsy-Turvy and Kirby Tilt 'n' Tumble,
  and **Sound Filter**.
- Bezels shaped like the handhelds, three each: Game Boy, Pocket, Dot Matrix; Berry, Teal, Atomic
  Purple; Indigo, Glacier, Black.
- Battery saves as before, plus the cartridge clock (`<game>.rtc`: Pokémon Gold, Silver,
  Crystal). The app now uses the PS4's time zone (the menu's clock, and the clock of Pokémon Ruby,
  Sapphire and Emerald). A save the game never touched isn't written.
- Sound made above 57 kHz (the Game Boys' 131 kHz) is low-passed before it is resampled to 48 kHz.
- Rumble cartridges shake the DualShock 4; the motors are only set when they change.

## 00.28 (test build)
- **N64 speed: the hand-off of graphics commands without locks.** ParaLLEl-RDP's command ring took
  a lock and woke its thread for every RDP command (about 2400 times per frame in Mortal Kombat 4)
  and held only 16 KiB, so the emulation also waited on a full ring. Now the emulation thread
  publishes commands with atomics, the command thread takes everything at once, each wakes the
  other only when it sleeps, and the ring holds 256 KiB. On the PC with the real core: RSP graphics
  time about half, the same RDP commands and pictures frame for frame.
- **N64 speed: ParaLLEl-RSP's vector loads/stores and DMAs** move whole halfwords and lines instead
  of single bytes (bit-exact on every address): about a quarter less RSP time.
- **N64 Picture Timing** (Settings -> Performance, One Frame Later by default): ParaLLEl-RDP's
  command thread makes each picture while the N64 runs on (1-3 ms less per frame at 4x); the
  picture is shown one frame later. At Once is the old way.
- **N64 Frame Pacing** (Settings -> Performance, Even by default): with V-Sync, each N64 frame is
  timed by the TV refresh its picture lands on. A heavy frame no longer makes the next refresh
  jump two frames and the one after repeat (the stutter), PAL games get an even cadence, and the
  game no longer runs 4-8% too fast in heavy scenes (that overflowed the sound). By the Clock is
  the old way. A `pacing:` line in the log every 10 s.
- Measured on the PC with the user's games, the emulation thread's CPU per frame: Mortal Kombat 4
  3.37 -> 2.32 ms, Mario Kart 64 2.52 -> 1.78 ms, Super Mario 64 3.86 -> 2.43 ms, and about a fifth
  less again with One Frame Later.
- Frame Skip is for SNES and NES only (the N64 core never asks whether to draw).

## 00.18 (public release, 2026-10-16)
The first public release since 00.17. It is the test build 00.27 below (with everything from the
test builds 00.18 to 00.26), plus:
- **The sound no longer disappears after a few games.** The menu and every game opened an AudioOut
  port of their own, and the PS4 never gave the closed ones back: after about four games the 9th
  open failed and the menu and every game (N64 included) were silent. Now the app opens one port
  and keeps it; output errors are checked, the port is made again after half a second of errors,
  the sound threads run above the game's threads, and the log gets a sound health line every 10 s.
- The AI filter (SNES/NES) costs about a quarter less GPU time for the same picture: the Anime4K
  layers rewritten into an equivalent, cheaper form, and a 5-tap Catmull-Rom. The log shows each
  GPU filter pass's time.
- Log lines from several threads no longer come out mixed.

## 00.27
- **Quality: Ultra (maximum)**, a fourth level: SNES and NES through the AI filter below, the N64
  at 4x with HD 2D and sharpened. Every level now also sets the GPU Upscaler and N64 2D Graphics.
- **GPU Upscaler** (Settings -> Graphics, also per game and in the in-game menu): the picture is
  drawn by the GPU's own filters, at the size it has on the TV:
  - **Pixel Art HD (xBRZ)**: SNES and NES redrawn with xBRZ at the TV's full resolution (the
    same look as Resolution 6x with xBRZ, a little sharper, and the CPU is left free for the game).
  - **AI (xBRZ + Anime4K)**: xBRZ at 768 pixels wide, then Anime4K's neural network (Restore CNN)
    cleans every edge and curve, and a sharp bicubic scale brings it to the TV. On real SNES/NES
    pictures it gave the smoothest, cleanest edges; the network alone on the console's pixels
    looked blurry, so it isn't offered that way.
  - **Smooth HD (AMD FSR 1)**: AMD FidelityFX Super Resolution (EASU and RCAS), made for 3D: the
    N64 at Native and 2x.
  - **HD Sharpen (AMD RCAS)**: the picture as it is, sharpened: the N64 at 4x.
  - The N64 gets Smooth HD for Pixel Art HD and AI (it has no pixel art), and at 4x Smooth HD only
    sharpens. With the NTSC Filter, Pixel Art HD and AI leave its TV look alone.
  - The filters compile the first time one is chosen (on a thread of their own; the picture is
    drawn as before meanwhile) and are kept in `cache/gpu-filters.bin` for the next start. Their
    images take one block of 48 MiB of GPU memory, made once.
- **N64 threads on cores of their own** (Settings -> Performance -> N64 CPU Cores: Fixed, the
  default, or PS4's Choice as before): the emulation thread on core 2, ParaLLEl-RDP's command thread
  on core 3 (the emulation waits for it every frame), its fence worker on core 1 and its shader
  compiler on core 5 at a low priority. The log says where each one went.
- **N64: no more stall when the shader cache is saved**: ParaLLEl-RDP's cache file (written every
  10 s while a game still compiles new shaders) is written by a thread of its own; two 0.12 s
  stalls in the 00.24 log came with those saves.
- The speed lines in the log: FPS shown averaged over the 10 s, the drawing time split into work
  and waiting (for the TV or the GPU), and they also come for SNES/NES games drawn through a GPU
  filter (with the GPU's busy time).

## 00.26
- **The N64's picture above Native is always drawn smooth**, as Texture Filtering promised: with
  Sharp, the 2560-wide 4x picture lost whole columns and doubled rows on the way to the screen.
- **New N64 picture settings** (Graphics): N64 Anti-Aliasing and N64 Dither Filter (the N64's own
  video filters), N64 2D Graphics (Exact, or Smooth (HD) text and HUD), N64 Distant Textures (Sharp
  (HD) or As the N64) and N64 Screen Effects (Exact, or Smooth (SSAA)).
- Quality: Medium draws the N64 at 2x and High at 4x (4x ran Super Mario 64 at full speed).
- ParaLLEl-RDP's command thread runs at the emulation thread's priority: the emulation waits for
  it at the end of every frame.
- The N64 speed lines in the log also show the GPU's busy time and memory.

## 00.25
- ParaLLEl-RDP's compiled shaders are kept in one file for every version (`cache/parallel-rdp.bin`;
  it is checked against the GPU and driver): an update no longer starts the cache afresh, so the
  shaders a game compiled once (up to 0.7 s each) stay compiled. The biggest older file becomes it.

## 00.24
- The emulation thread runs at a higher priority while a game plays (480 instead of the PS4's
  default 700): a background thread on its core (ParaLLEl-RDP's shader compiles, the covers) waits
  instead of stopping the game for a moment.
- N64 timing in the log, to see where each frame's time goes on the PS4: the core's own split
  (graphics and sound tasks, the recompiler) every 2 s, ParaLLEl-RDP's VI scanout and its wait for
  the GPU every 600 frames, and the frontend's drawing time and CPU core with each speed line.

## 00.23
- **N64 speed: the sound the quick way.** The PS4's CPU took 22-23 ms for each frame of Super
  Mario 64 (a European game has 20). N64 RSP is now **Fast (HLE sound)** by default: the N64's
  sound is made by HLE, while its graphics (and any task HLE doesn't know) still run exactly on
  ParaLLEl-RSP, as mupen64plus's RSP fallback does (the core's own hook was left unfinished).
  ParaLLEl (recompiler) stays for a game whose sound is wrong with Fast.
- The HLE's cache of known microcodes can't overflow anymore.

## 00.22
- **N64 freezes**: ParaLLEl-RDP compiles a shader the first time a game needs it (up to 0.7 s on the
  PS4, the game stops meanwhile). Its pipeline cache now really works (Granite made none without its
  own filesystem): it is read at each start and kept in `data_dir/cache` every 10 s when new
  shaders were compiled, and at the game's end, so each shader stops a game only once. The log says
  how much was read and kept.
- **N64 speed**: its frames are owed by the clock, so a heavy scene keeps more of the game's speed.
- **Apply To: This Console** (a game's settings, and the in-game menu): All Games, This Console or
  This Game. This Console's settings are for every game of that console that has none of its own;
  Reset This Console goes back to the global ones.
- **Clean Up Games**: Keep Only USA Games is per console, each with its own count and confirmation
  (it could delete a whole European N64 collection at once).
- Covers: one writer at a time, written whole and then renamed: a cover chosen in Find Cover always
  wins over the download, and a cut write never leaves a broken cover.
- "Use for All Games" copies only the settings of the game's own console. Folders on a USB drive
  are no longer made at start. Audio Interpolation's "None" is "No Interpolation".

## 00.21
- **N64 speed**: N64 Graphics Sync (Settings -> Performance), Fast by default: the GPU draws while the
  N64 runs instead of the N64 waiting for each picture (ParaLLEl-RDP's asynchronous mode; Accurate
  for the few games whose effects read the screen back). ParaLLEl-RDP's compiled pipelines are kept
  in `data_dir/cache`, so a game starts without the ~2 s of compiling after the first time. The
  log's N64 speed lines also show the core's time per frame.
- **Quality** (Settings -> Video, also per game and in the in-game menu): Low, Medium or High sets
  the resolution, the upscaling filter, the picture size (fit to the screen) and the N64's size and
  sync at once; changing one of them by hand shows Custom.
- N64 Resolution's help says what 2x and 4x cost on the PS4.
- Fixes: the N64 picture's line count at 2x/4x (integer scale, scanlines); a fixed picture size
  stays inside a bezel's window; Games Without Cover keeps the cursor on the next game and clears
  the download's progress; Find Cover ignores a late result of another search and doesn't jump
  onto a picture once you moved; "Download the Closest" takes only nearly identical names (a shared
  word was often another game); a cover chosen in Find Cover is never written over by a download;
  the cover lists are freed while a game runs; About fits above the button hints; Spanish fixes.

## 00.20
- **Nintendo 64 in EmuSuNi** (Mupen64Plus-Next with ParaLLEl-RDP and ParaLLEl-RSP): an N64 tab, its
  settings (N64 Resolution, N64 CPU, N64 RSP, Controller Pak), its controls and covers. Its games are
  looked for on the USB drive in `Roms/n64` (`.z64`, `.n64`, `.v64`) until another folder is chosen.
  The separate N64 test package is gone.
- N64 fixes since the test package: games closed the app as they started (part of ParaLLEl-RDP's
  Vulkan code had been compiled against other Vulkan headers and read the GPU's limits as its memory
  types); the N64's memory is the compact layout, aligned as ParaLLEl-RDP needs, instead of a 512 MiB
  request that always failed; RDRAM is copied to the GPU (the PS4 can't import host memory); the
  core's Vulkan objects are freed before the game, as RetroArch does; the ROM's copy in the app is
  freed once the core has its own.
- **Without Cover** (Options in a game list): only the games without a cover picture, A-Z. A game
  leaves the list as soon as its cover is saved, and the next one is selected.
- **Games Without Cover** (Settings, after Download Covers): every game without a cover. Cross opens
  Find Cover for one, and saving its cover (or Circle) comes back to the list on the next game.
  **Download the Closest for All** gets each one's picture, or else the one most like its name.
- The game folder browser lists the USB drives as the app sees them (`/usb0`, ...).

## 00.19
- **Bezels** (Settings → Video → Bezel, one setting per console, also per game and in the in-game
  menu): three built-in frames for each console (Super Nintendo, Super Famicom and Purple Stars; NES,
  Famicom and Black and Red; Nintendo 64, Four Colours and Atomic Purple) and a Retro TV for all of
  them, drawn by `scripts/make-bezels.py`. **Your Own** takes a 1920x1080 PNG from
  `data_dir/bezels/<console>/` named as the ROM (The Bezel Project's names) or as the game, or
  `bezels/<console>.png` for every game. The picture is fitted into the bezel's see-through window.
- **Rename** a game (Triangle → Rename): the PS4's keyboard, the name is kept in `names.ini` and is
  used for searching and covers too. An empty name brings back the file's own.
- **Find Cover** (Triangle → Find Cover): libretro's box art or title screens most like the game's
  name (or another name typed in), with a preview; Cross makes the picture the game's cover.
- **Sort by console** (Options in a game list): each console's games together, by title.

## 00.18
- **NES games** (FCEUmm): `.nes`, `.fds` (Famicom Disk System, needs `disksys.rom` in
  `system/`), `.unf` / `.unif`, or one of them in a `.zip`. Both cores are in the same app; each
  core's libretro API has its own prefix, and FCEUmm's other symbols are kept private.
- **Tabs per console**: All, SNES, NES, Favorites and Settings (XMB style: a column per console).
  **Saved Data** and **Themes** are now pages inside Settings (Cross opens them, Circle goes back).
- **A game folder per console** (Settings → Game Folders): `data_dir/games` for the Super Nintendo
  and `data_dir/nes` for the NES until others are chosen.
- **The console's badge on every cover**: SNES / SUPER FAMICOM, NES / FAMICOM (Japanese games).
- **Controls per console** (Settings → Controls): the NES pad with Turbo B and Turbo A on Square and
  Triangle, and the Famicom Disk System's disk side and insert / eject on L1 and R1.
- NES settings: **NES Palette** (10 palettes) and **Turbo Buttons**. A game's settings and the
  in-game menu show only its console's settings; the global lists mark the settings of one console
  (SuperFX Clock (SNES)).
- Covers for NES games from libretro's NES thumbnails, in `covers/nes`. The picture chosen is the
  plain box (no special editions, arcade versions or revisions; TOSEC names understood).
- A NES game's settings, saves, states, cheats and cover use its key `nes/<file name>`, so its files
  go in subfolders (`saves/nes`, `states/nes`, `cheats/nes`, `covers/nes`). Super Nintendo files
  stay where they were.
- About: "Created by Arturo Fuentes" and both cores.

## 00.17
- The side panels are nearly opaque: the lists behind them no longer show through.
- The game counter hides behind a side panel; a shade under the XMB style's button hints.
- The screenshot tour (`tour.txt`, for the documentation) leaves the controllers out while it plays.

## 00.16
- **Delete a game** from its options (Triangle), with a confirmation. Its saves stay.
- **Clean Up Games** (Settings): deletes duplicate files of a game and keeps one: the USA version,
  the best dump, the latest revision. A second option keeps only USA games. Every file is listed
  before anything is deleted.
- **More covers**: smarter matching between GoodSNES and No-Intro names (Roman numerals, subtitles,
  word order, "&" / "and", close spellings). The title screen is used when a game has no box art.
- **A short description of every setting and every option**, under the settings lists and in the
  in-game menu (small, under the list).
- Option descriptions in the side panels (game options, confirmations).

## 00.15
- **Resolution**: the game redrawn at 2x to 6x (5x for 1080p) before it is fitted to the TV.
- **Upscaling Filter**: xBRZ (smooth HD look), HQx, or Sharp Pixels. Multithreaded: 3–6 ms per
  frame at 6x, measured on a PS4.
- "Internal Resolution" renamed **Picture Size** (it only changes the size on screen).
- Fixed: changing a value in a game's settings with *Apply To: All Games* turned the game to
  *This Game*. **Use for All Games** copies a game's settings to every game.
- Search keyboard: L2 clears everything, L1 / R1 browse the games found, the special keys show
  their buttons.
- The PS4 start-up picture and the intro animation are centred.

## 00.14
- **Favorites** tab (L2 on a game, or Triangle → Add to Favorites), with a star on the covers.
- **Search** (R2): an on-screen keyboard; the list shows matching games as you type.
- Covers download from `thumbnails.libretro.com` over HTTP (the PS4's TLS fails on GitHub's
  servers).
- Fixed a crash when a game starts (error CE-34878-0): C++ allocations used the PS4's small
  internal memory pool (~12 MiB) instead of the app's heap.

## 00.13
- Box art downloads by itself in the background, paused while a game runs.
- GPU memory reused (a texture arena and persistent upload buffers).

## 00.10 – 00.12
- First versions: Snes9x 1.63, the Library and XMB launcher, themes, sounds, 13 languages,
  settings for all games or one game, the in-game menu with save states, cheats and controls.
- The app runs under the homebrew identity Mesa's Vulkan driver needs.
