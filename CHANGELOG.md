# Changelog

All notable changes to EmuSuNi. Versions follow the PS4 package version (`XX.YY`).

## 00.20 (public release, 2026-10-18)
The first public release since 00.19: the test builds 00.33 to 00.54 below.
- **PlayStation** (SwanStation), **Nintendo DS** (melonDS DS), **PSP** (PPSSPP 1.20.4) and
  **Dreamcast** (Flycast 2.7): ten consoles in one app, with their tabs, folders (`Roms/ps1`, `nds`,
  `psp`, `dc` on the USB drive by default), box art and controls. HD 3D for the PlayStation (up to 4x,
  PGXP, True Colour) and the PSP (up to 4x, texture upscaling), the PlayStation's bezels, the DS's
  screens side by side, one above the other or the top one big, with the touch screen on the
  touchpad, and the Dreamcast's analog triggers on L2 / R2 and its VMU. No BIOS needed (the real ones
  are used when they're there). The Dreamcast plays at Low quality only for now.
- **Games on several discs** (PlayStation, Dreamcast) are one game in the library, grouped by their
  names or their folders; the in-game menu's **Disc** changes the disc, and the last one goes back in
  next time.
- **Find Cheats Online**: libretro's cheat collection, searched for the game (every console but the
  Dreamcast).
- **Smooth Edges, Natural HD and Natural AI**: three new GPU filters, one per Quality level.
- **Console Order** (Settings): the tabs in the order you choose, or By Company, By Power, By Year or
  Alphabetical; the tab bar goes round.
- The emulation settings and Controls are set from a game (Triangle); Settings lists the app's own.
- Delete Game deletes all of a game's files (its tracks and discs).
- Even frame pacing and no sound stutter with V-Sync off; the app's heap can't freeze it any more.

## 00.54 (test build)
- Flycast (scripts/build-dc-core.sh): what a Dreamcast game forced - its region, the real BIOS,
  framebuffer emulation... (Flycast's per-game settings) - is undone when the next game loads. The core
  stays in the app from game to game, and libretro's Settings::reset() does nothing (RetroArch loads the
  core afresh): the PS4's log showed Crazy Taxi after Marvel vs. Capcom 2 starting with Europe's region
  ("Europe region not supported. Using USA instead"); after a game that forces the HLE BIOS the next
  ones booted it too, even with the real BIOS there, and after one that forces the real BIOS they said
  "This game requires a real BIOS". Tested on the PC: the same two games in one process, before and after.
- Quality's level isn't made Custom by the Dreamcast's hidden settings any more; a setting of every
  console but the Dreamcast has no console tag (only those).
- PS4 check (screenshot tour, 00.53): Marvel vs. Capcom 2 (Europe) shows its Display Mode Select screen at
  60 FPS, 100%, and goes on by itself to the Virgin Interactive logo.

## 00.53 (test build)
- **The Dreamcast plays at Low quality only** (the user's choice): on the PS4 its higher levels slowed
  the games to a crawl (00.52: Crazy Taxi at 3x, 0.6 pictures a second) or drew wrong colours (per-pixel
  sorting, 00.49). Whatever the Quality, Flycast draws at 640x480 with 4x texture sharpness and per
  triangle sorting, and no GPU filter goes over it; Quality, GPU Upscaler and the Dreamcast's own picture
  settings aren't shown for its games ("Dreamcast: Low quality only"), and the Quality levels' texts
  don't name it any more.

## 00.52 (test build)
- A third review of the discs, verified, fixed:
  - A folder with a game.m3u and its discs ("Legend of Dragoon/game.m3u", CD1-CD4), or one game in two
    kinds of file ("disc.gdi", "disc.chd"), is named after its folder again (00.51 named them "game",
    one key for all of them). Different games in one folder are "Folder - disc", "Folder - game".
  - PBP games named EBOOT.PBP are named after their folders (they all shared one key: "EBOOT").
  - Dreamcast: a disc change that finds the emulation stopped says so (the console starts again); a
    "Reboot to BIOS" of before a loaded state or a Reset doesn't restart over it; a state that fails
    half way (a file cut short) leaves the game as it was and running (before: what was read of it ran);
    a state of another disc that fails puts the machine back as it was (no disc change seen by the game);
    when Continue's state can't be saved at the end, the older one goes (its older VMU).
- A fourth review, verified, fixed: a Dreamcast state is measured again when it grew between its size and
  itself (Flycast's emulation runs in between: "Serializer buffer overflow"), up to 3 times - with the
  line above, a Continue could have been lost to it; a state is read at its size (a 36 MB Dreamcast state
  took 64 MB as text) and freed before the machine is put back; Reset after an emulation error found by a
  disc's insert goes on (the error ended the game after it); "Folder_(USA)" names give each file's game a
  title of its own. Saved data of earlier test builds stays under the old names (PBP games: "EBOOT"; a
  game.m3u folder in 00.51: "game").

## 00.51 (test build)
- A second and a third review of the discs, verified before release, fixed:
  - The in-game menu opened while the Dreamcast waits to start again (a game gone back to the BIOS):
    it starts again first, so Save, Load, Reset and Quit act on a running console. Flycast gives no
    state of an emulation that ended in an error, and goes on when a state can't be loaded (it stopped
    the game before). Quitting while it waits: the older Continue state goes (it would put back an
    older VMU).
  - A "Reboot to BIOS" left in Flycast's queue by the game before doesn't restart the next one.
  - The 3-restart limit counts only the restarts the player didn't ask for (a disc change resets it);
    an emulation that stopped restarts at once (no 3 s frozen).
  - A Dreamcast state of another disc: the disc changes in one go with the lid closed; when the state
    can't be loaded, the disc before goes back in.
  - Files named only after their disc in the game's folder ("FF7/CD1.chd", "FF7/CD2.chd") and disc
    folders whose files carry the disc ("Shenmue/Disc 1/Shenmue (Disc 1).gdi") are one game; two
    plain names in one folder ("disc.gdi", "game.cdi") keep their own names (the folder can't name both).
  - Reset All Settings keeps the one-time settings migrations done (they ran again at the next start
    and undid what was chosen after the reset).

## 00.50 (test build)
- **Games on several discs, by themselves** (PlayStation, Dreamcast): files side by side whose names
  differ only by the disc - "(Disc 1)", "(Disc 2 of 3)", "(CD1)", "[Disc 2]" - or a folder per disc
  ("Shenmue (Disc 1)/disc.gdi") are one game, named without the disc. It starts with an .m3u of its
  discs (data_dir/cache/m3u) and with the disc it was played with last (saves/<game>.disc: a state
  saved on disc 2 is disc 2's). In the in-game menu, **Disc** (only for these games): Left / Right
  choose one, Cross puts it in - the lid opens for 1.5 s of the game, as on the console, then it
  closes with the disc chosen. A PlayStation memory card is the game's for all its discs (a game
  played disc by disc before keeps the newest disc's card); the Dreamcast's VMU already was (Flycast
  names it after the game's product number). An .m3u of the user's own works for the Dreamcast too.
- Flycast (scripts/build-dc-core.sh): a reset boots the disc in the drive, as the console's. When a
  game goes back to the BIOS (its lid opened when it didn't ask for another disc, or its own Exit),
  Flycast's HLE BIOS has no menu and stopped there; the console starts again with the disc in it.
  Tested on the PC: Crazy Taxi as disc 1, Marvel vs. Capcom 2 as disc 2 - the lid opened, Crazy Taxi
  went back to the BIOS, Marvel vs. Capcom 2 started; with SwanStation, a disc changed while a game
  ran and a game started on its disc 2.
- **Delete Game deletes all the game's files**: a .cue's or .gdi's tracks (only the .cue was deleted
  before, its .bin tracks stayed) and every disc - in the game's own folder, and not a track another
  game there names too. **Information** shows the size of all of them and the discs.
- "disc.gdi" (and other plain names) in a folder of the game's name: the game is named after the folder;
  folders named only "Disc 1", "Disc 2"... inside the game's folder are its discs ("Shenmue/Disc 1/disc.gdi").
- Found by a review and verified before release, fixed:
  - Flycast's disc changer is set afresh at each game's start (scripts/build-dc-core.sh): a game left on
    disc 2 made every later Dreamcast .m3u game fail to start ("Unknown disk format") until the app
    started again.
  - "Reboot to BIOS" with the lid open came with RETRO_ENVIRONMENT_SHUTDOWN, which ended the game with
    that error: for the Dreamcast it is the console starting again now. After 3 restarts in a row the
    game ends ("This disc goes back to the Dreamcast's BIOS each time it starts").
  - Flycast's emulation thread is stopped while a disc goes out or in (as for a save state).
  - A Dreamcast state of a game on several discs keeps its disc (<state>.disc) and puts it in when loaded.
  - A disc that can't be put in: the one before goes back in; a disc played last that doesn't start any
    more: the game starts with disc 1 next time (said in the error).
  - The PlayStation memory card taken for a game played disc by disc before is the one with the most
    saves (a disc started once has a newer, empty card), once.
  - The disc's toast is shown after the drawing beside the emulation (a data race with the DrawWorker).
  - A PS1, DS or PSP game named "game", "disc"... in a folder of its own is named after the folder too
    (two such games shared their saves before); saved data of earlier test builds stays under the old name.
- **Dreamcast Transparency per pixel left out of Ultra** (00.49 on the PS4: Crazy Taxi's picture came
  out in wrong colours once Ultra was chosen in the game; the same change on the PC drew right, so it is
  the PS4's GPU with Flycast's per-pixel renderer). Ultra is per triangle; where per pixel was set it is
  per triangle again (once); the choice stays, marked experimental, and is taken only when a game
  starts (it was given to Flycast at once, which made its renderer again in the middle of the game).
- Marvel vs. Capcom 2 (Europe) starts with its own 50 Hz / 60 Hz screen (Display Mode Select, every
  PAL game on a European console): Down to "60 Hz display", Cross, then Options to create its file.

## 00.49 (test build)
- **Dreamcast games start again** (00.48 crashed at every game's start on the PS4: Crazy Taxi,
  Marvel vs. Capcom 2): the app gave Flycast "Dump Unique Textures", an option Flycast 2.7 reads but
  never declares, and Flycast stops (verify, DEBUGBREAK) at such an option. Reproduced on the PC with
  the app's own 90 Dreamcast options; without that one both games run.
- The options each core declares (SET_VARIABLES) are kept: the Dreamcast is given only its own, and
  any other core's undeclared option is logged once ("isn't one the core declared").

## 00.48 (test build)
- **Dreamcast** with Flycast 2.7 (`scripts/build-dc-core.sh`): its tab, folder (`Roms/dc` on the USB
  drive), box art and controls (A/B/X/Y where the DualShock 4 has them, its analog L / R triggers on
  L2 / R2 with their pressure, its stick the left stick). The games (.gdi with its tracks, .chd, .cdi,
  .cue) are read from their path; a .chd or .cue in a folder shared with the PlayStation or the PSP
  goes to the console it is for (a GD-ROM CHD's "CHGD" tracks, a Redump .cue's "HIGH-DENSITY AREA").
  Drawn by Flycast's Vulkan renderer on the app's device (threaded: the SH4, ARM7 and AICA on its
  own thread, core 3), its SH4 and ARM7 recompilers, its HLE BIOS unless system/dc/dc_boot.bin is
  there, each game's VMU in saves/dc. **Dreamcast Resolution** (1x-4x, in the Quality levels),
  **Dreamcast Texture Sharpness**, **Dreamcast Transparency** (per triangle or per pixel: Ultra),
  Frame Skip (Flycast's own). No GPU readbacks. Flycast takes no cheat codes: Cheats is left out for
  its games. Sega's consoles come after Sony's in the standard order.
- Found by a review before release and fixed in the core (reproduced on the PC: SIGSEGV with the old
  core, none with the new one): with per-pixel sorting (Ultra) every Dreamcast game crashed at its
  start, Flycast making its renderer again from the options the pads read before the game, with no
  graphics context yet.
- Settings' migrations: the 00.40 GPU Upscaler step runs before the consoles' values are added (a
  Quality level from 00.39 is recognised again), and a console's value set by hand is never replaced
  (tested on the user's own settings.ini).

## 00.47 (test build)
- **No MSAA for the PSP in the Quality levels** (Ultra: 4x without it): with MSAA x2 every new Vulkan
  pipeline took ~86 ms to compile on the PS4 (5-19 ms without) and God of War stuttered at each new
  effect.
- **PSP Frame Rate (God of War)**, Performance: Auto (60 FPS while the PS4 keeps up; 30 for the rest
  of the game after ~2 s in a row too slow, not counting loading frames nor the first 15 s), 60, 30.
  PPSSPP's FramerateHack30, only for games with the GoWFramerateHack60 compat flag: half the work,
  the game's speed unchanged.
- PPSSPP (scripts/build-psp-core.sh): the driver's VkPipelineCache kept with its shader cache
  (EMUSUNI_PSP_PIPELINE_CACHE=1: God of War recompiled 115 pipelines at each start, 60-140 ms each),
  compat.ini's DisableFirstFrameReadback for Ghost of Sparta (its first-frame downloads drained the
  render thread and the GPU: 29-97 ms each), and timing lines in the log every ~10 s (where each
  frame's time goes, the render thread, pipeline compiles, readbacks, the GPU's own time).

## 00.46 (test build)
- **Even pacing with V-Sync off**: the frames owed each pass are rounded to the nearest, not down.
  Rounded down, passes of 16.6-16.8 ms against the TV's 16.68 ran 0, 2, 0, 2 frames once the owed
  fraction sat near 0 (God of War at 100% speed with a third of its pictures shown twice, already in
  00.38): every console without V-Sync stuttered like that at times.
- **Ultra draws the PSP at 3x** (with 2x MSAA), not 4x: God of War's intro and fights at 4x + MSAA +
  the AI ran at 85-90%, the drawing waiting 6-7 ms a frame for the GPU (GPU busy 55-63%).
- A PSP picture at 4x gets no AI from the GPU filters (only Natural HD's sharpening): it is the
  screen's size already.

## 00.45 (test build)
- **The app's heap takes a blocking lock** (port/ps4_dlmalloc.c: a pthread mutex, not dlmalloc's
  test-and-set spin lock with a sched_yield() every 64 spins). The PS4 runs threads by fixed
  priorities and sched_yield() only lets equal or higher ones run: threads spinning on the heap kept
  a preempted lower-priority holder from ever running again and the whole app froze. That was God of
  War: Ghost of Sparta's freeze at its start in 00.40/00.41 (PPSSPP compiling 86 cached pipelines on
  threads of three priorities), reproduced on the PC with the PS4's cores and priorities; 00.42 could
  freeze too. It protects the N64's and the DS's threads as well.
- PPSSPP's compile and pool threads at one priority (700) on cores 1, 3, 4, 5.
- The PSP's speed-ups back on: the render thread, fast memory (its fault handler now spells out
  FreeBSD's signal values) and hardware skinning.

## 00.44 (test build)
- **Console Order's ready-made orders**: By Company (the default: Nintendo, then Sony, the most basic
  first), By Power (whoever made it), By Year (Japan's release dates) and Alphabetical; the one in use
  says so. The consoles can still be moved by hand above them.

## 00.43 (test build)
- **Settings lists only the app's own options**: the emulation settings (Video, Graphics,
  Performance, Emulation, System) and Controls are set from a game (Triangle -> Game Settings,
  Controls: for that game, its console or all games).
- **Console Order stays open** while you pick a console up and put it down (it slid in again every
  time).
- **The standard console order**: Nintendo's consoles, then Sony's, each from the most basic to the
  most powerful: Game Boy, NES, Game Boy Color, Super Nintendo, Game Boy Advance, Nintendo 64,
  Nintendo DS, PlayStation, PSP. An order saved before is dropped once (xmb.ini
  console_order_version=2).
- **About** says who made EmuSuNi and its version, nothing more.
- **The intro shows the version**, small, at the bottom right.

## 00.42 (test build)
- **God of War: Ghost of Sparta starts again (for real)**: 00.41 still hung at its start. The log
  stops in PPSSPP's StopThreads at the GPU's start (the Vulkan submission thread joined, never the
  compiler thread) after 86 pipelines from its shader cache; FIFA 14 (55) went on. PPSSPP's render
  thread is off again (EMUSUNI_PSP_RENDER_THREAD=0) and skinning back on the CPU (hardware skinning
  was faster only with the render thread): the PSP as 00.39 ran God of War, with the drawing beside
  the emulation while its core is heavy.

## 00.41 (test build)
- **God of War: Ghost of Sparta starts again**: 00.40 froze on a black screen at its start (even
  L3+R3 did nothing). PPSSPP's fast memory installs a fault handler that registered with the
  OpenOrbis headers' Linux values (SA_SIGINFO 4, SIGBUS 7) where the PS4's FreeBSD kernel wants
  0x40 and 10 (port/ps4_crashlog.cpp has its own constants for this): the game's first stray access
  hung it. Fast memory is off again (the render thread and hardware skinning stay).
- The log says how far the left stick was pushed every 10 s (PSP, PS1, N64): the PSP's stick was
  reported dead on the PS4, though it moves Kratos in God of War on the PC test frontend.

## 00.40 (test build)
- **Three new GPU Upscaler filters, one per Quality level** (the old ones stay): **Smooth Edges**
  (Medium: MMPX 2x and SMAA for pixel art, SMAA then Catmull-Rom for 3D — only the jagged edges
  change), **Natural HD** (High: Smooth Edges, then AMD FSR's EASU and RCAS) and **Natural AI**
  (Ultra: Smooth Edges, Anime4K Restore M with its correction at 70%, Catmull-Rom — no waxy look).
  Before, Low, Medium and High drew no GPU filter and Ultra the full AI. The full AI stays as
  "AI (Anime4K)". Chosen on real frames of every console (scratchpad filters-look); about 0.5-1.9 ms
  of PS4 GPU each (estimated). SMAA (MIT) and MMPX (MIT) in THIRD_PARTY_NOTICES.md.
- A Quality level chosen before keeps its name: its new GPU Upscaler is set once ([global]
  presets=2).
- **The PSP faster** (scripts/build-psp-core.sh, measured on the PC in God of War: Ghost of Sparta's
  first fight): PPSSPP's render thread on (it records and submits the Vulkan work beside the
  emulation, on core 4; the core's emulated swapchain now never calls get/wait_sync_index and hands
  the newest image to set_image at the end of retro_run), fast memory on (its fault handler skips
  stray accesses and passes the rest to the crash log), hardware skinning. Estimated ~17.4 ->
  ~13-14 ms a frame on the PS4. While its core is heavy (above 85% of a refresh) the PSP's drawing
  runs beside the emulation too, as the DS's. The picture is one frame later (two while heavy).
- **The DS faster** (scripts/build-nds-core.sh, measured on the PC, every frame identical to
  before): the 3D rasterizer on two threads (even and odd lines, cores 4 and 5), the sub screen's 2D
  on a helper thread (core 3) a line at a time, the halted CPUs' idle loop turns skipped, a main RAM
  fast path, frames the app won't show not drawn (Frame Skip, now offered for the DS), PGO
  (scripts/nds-core.profdata.xz). Mario Kart DS races 2.63 -> 1.84 ms a frame on the PC (estimated
  ~26 -> ~17.5 ms on the PS4), New Super Mario Bros. -35%. One port hook for its threads,
  emusuni_nds_thread(name); a timing line from the core every ~10 s.
- **The DS's Big Top Screen is made on the GPU**: the core always gives the screens side by side
  (or one above the other) and the app places them (melonDS DS scaling them on the CPU cost ~3.6 ms
  a frame); the top screen goes through the GPU filters. R3 swaps the app's view.

## 00.39 (test build)
- **A core too slow for a refresh isn't chased any more**: God of War: Ghost of Sparta's fights took
  17-18 ms a frame (of 16.7) and the clock's schedule ran 4 frames a pass to catch up: 72 ms passes,
  16 pictures a second at 91% speed. When the core's average passes 92% of a refresh (until it is
  back under 85%) one frame runs per pass, every one shown; the game runs a little slower and its
  sound is played as much slower (no gaps). Logged as "pacing: the core takes ... one frame a pass".
- **PSP Resolution and Anti-Aliasing apply when the game starts**: PPSSPP's Vulkan picture keeps the
  size it started with, so a change while it ran (Quality from the in-game menu) zoomed into a corner.
- The DS's speed line is in the log.

## 00.38 (test build)
- **The PSP's picture shows**: 00.36/00.37 ran the games (FIFA 14 and God of War: Ghost of Sparta at
  98-99% speed, with sound) on a black screen. PPSSPP's picture keeps the PSP's alpha (its stencil,
  often 0) and was drawn blended, so invisible; it is read through a view whose alpha is 1 (a new
  one each frame, Display::OpaqueView).
- **The DS draws beside its emulation**: Mario Kart DS's core took 15-17 ms of the 16.7 and the
  drawing 2.7 ms more after it, so two frames were run per refresh (98% speed, 22-28 pictures a
  second). The drawing of each pass now runs on a thread of its own (cores 1, 4, 5) while the next
  frames are emulated; the picture shown is one frame older.

## 00.37 (test build)
- **Find Cheats Online** (Cheats, in the in-game menu and on a game's Triangle menu): libretro's
  cheat collection (RetroArch's: about 28,000 lists, 35 MB, plain HTTP from buildbot.libretro.com,
  kept in /data/EmuSuNi/cache/cheats.zip for a month) is searched for the game. Its lists are shown,
  the same game first (a PSP disc by its ID, read from the ISO), the game's region next, demos and
  fan versions last. You open one, choose the codes you want and add them to the game's list (off).
  Nothing is downloaded until you ask.
- Cheats: up to 20,000 codes per game (was 1,000); long codes go to the cores that take them
  whole (the PSP's, the DS's...); PPSSPP's cheat engine on.

## 00.36 (test build)
- **PSP** with PPSSPP 1.20.4 (`scripts/build-psp-core.sh`): its tab, folder (`Roms/psp` on the USB
  drive), box art and controls (the DualShock 4's buttons where the PSP has them, its stick the left
  stick, SELECT on the touchpad, fast forward on R2). The games (.iso, .cso, .chd, .pbp, .elf) are
  read from their path. Drawn by its Vulkan renderer on the app's device, run by its x86-64 JIT (the
  IR interpreter if the PS4 won't run it); FFmpeg for the videos. **PSP Resolution** (1x to 4x, in
  the Quality levels: Ultra is Full HD with 2x MSAA), **PSP Texture Sharpness** (anisotropic),
  **PSP Texture Upscaling** (2xBRZ, 4xBRZ, MMPX on the GPU), **PSP Anti-Aliasing**, Frame Skip (the
  core's own). The games' saves are the memory stick's, in /data/EmuSuNi/saves/psp; PPSSPP's fonts
  and system dialogs come in the package. No BIOS needed.
- A .iso, .chd or .pbp in a folder the PlayStation and the PSP share goes to the console it is for
  (by what is inside it); in a console's own folder, the folder says.
- The Vulkan device is also made with logic ops, depth clamp, clip and cull distances and, when the
  GPU has them, render passes 2 with depth resolve and stencil export; the cores are told every
  feature it has.
- A Quality level chosen before keeps its name: its PSP values are added.
- States are written without a second copy in memory (a PSP state is 40-48 MiB).

## 00.35 (test build)
- **Nintendo DS** with melonDS DS (`scripts/build-nds-core.sh`, its ARM JIT in a static buffer the
  app makes executable): its tab, folder (`Roms/nds` on the USB drive), box art and controls (B/A/Y/X
  on Cross/Circle/Square/Triangle, Select on L2, fast forward on R2, L3 blows into the microphone,
  R3 swaps the screen layout). The DualShock 4's touchpad is the touch screen. The games (.nds,
  .ids) are read from their path, so a 128 MB game fits in memory. **DS Screens**: Side by Side,
  Big Top Screen or One Above the Other. The built-in BIOS and firmware start most games; the real
  ones (bios7.bin, bios9.bin, firmware.bin in /data/EmuSuNi/system) are used when they're there.
  The firmware's language follows the app's. The 3D is drawn on its own core.
- The cores' XRGB8888 pictures are turned into RGB565, and the libretro pointer is answered.

## 00.34 (test build)
- **The sound no longer stutters with V-Sync off** (every console): each pass slept the rest of
  its own refresh and the sleeps ran long, so the games ran at 98% speed (58.8 passes a second) and
  the sound ran dry about ten times every ten seconds. Now the passes keep to a deadline that moves
  on one refresh at a time, and the frames due are counted by the clock.
- **The PlayStation's Quality levels** (chosen on real Tekken 3 and Gran Turismo 2 frames, side by
  side): Medium 2x with xBR textures and True Colour; High 4x, xBR, True Colour and PGXP; Ultra the
  same with 4x MSAA and the AI on top. Ultra used to look like plain 4x: the textures stayed blocky
  and dithered.
- **The GPU's AI for the PlayStation**: its HD 3D (3x and 4x) is made a little smaller with
  Catmull-Rom (supersampled) and cleaned up by the Anime4K network. At 1x and 2x the AI and Pixel
  Art HD looked painted, so Smooth HD is used there. New **PS1 Anti-Aliasing** setting (MSAA 2x-8x).
- The AI's images are capped at 960x720 pixels whatever the picture's shape (a 480-line picture no
  longer runs out of room), and their memory block is 64 MiB.

## 00.33 (test build)
- **PlayStation** with SwanStation (`scripts/build-ps1-core.sh`): its tab, folder (`Roms/ps1` on the
  USB drive), box art, controls (the DualShock 4's own layout, both sticks analog) and bezels
  (PlayStation, PSone, Midnight). The games are CD images read from their path (.cue/.bin, .chd,
  .pbp, .m3u for several discs, .iso, .img, .ecm, .mds; the discs an .m3u lists aren't listed
  again). The memory card is the battery save. Drawn by its Vulkan renderer on the app's device:
  **PS1 Resolution** up to 4x (real HD 3D; in the Quality levels), PGXP, texture filtering, True
  Colour, a widescreen hack, faster disc reading, the boot logo, the controller type. The BIOS
  (scph5501.bin...) goes in /data/EmuSuNi/system; without it a built-in one starts the games.
- **The tab bar goes round**: an endless carousel, the selected tab in the middle, the others
  fading out towards the ends; L1 / R1 go on from the last tab to the first.
- **Settings -> Console Order**: the consoles' tabs (and XMB columns, and the Console sort) in the
  order you choose; Default Order puts them back.
- The Vulkan device is made with dualSrcBlend and sampleRateShading when the GPU has them.

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
