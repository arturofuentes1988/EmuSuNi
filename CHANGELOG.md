# Changelog

All notable changes to EmuSuNi. Versions follow the PS4 package version (`XX.YY`).

**Public release v00.21 (2026-10-20)** is the 00.64 test build below, packaged as version 00.21 (the public versions count on from v00.20; the test builds 00.55 to 00.64 led to it). The 00.65 entry is work in progress, not in v00.21.

## 00.65 (test build)
- **Sega Saturn: a speed test for the owner's PS4, only in builds made with `scripts/build.sh with-saturn`** (the
  first Saturn step: is a Saturn worth building?). The normal build has none of it. With it, and only with the file
  /data/EmuSuNi/saturn-test, Settings has a row "Saturn (speed test)": it runs each Saturn game in Roms/saturn (a .cue
  with its .bin tracks, or a .chd) with the owner's BIOS, from the BIOS's logo to the end of the game's own demo race
  (Daytona USA 4200 frames, Sonic R 6600), as fast as the PS4 goes, with no picture and no sound (a "measuring"
  screen; Circle stops it), and logs what each frame costs the PS4's CPU: the emulation thread's CPU time (mean,
  p95), the sound (SCSP) thread's, the share of full speed, the PS4's factor to the PC on the same frames, and the
  verdict for Daytona USA (20 ms a frame or less: the full Saturn; 20-30: light games only; more: no Saturn), with
  the OpenGL renderer's work on the emulation thread added as the PC measured it (Daytona x1.32, Sonic R x1.68). The
  core: YabaSanshiro 3.4.2 (scripts/build-saturn-core.sh) without a renderer (the PS4 has no OpenGL), the SH-2
  interpreter, its sound thread sleeping instead of spinning, frame skip off, the work RAMs' fast paths (-18% / -10%
  on the PC); its tables of 64 KiB or more on the heap: 1.1 MiB more MemSiz than without it. Two fixes the
  renderer-less core needed (found on the PC, checked with the GL build's pictures): the VDP1's command list walked
  as the renderers walk it (else no game leaves its title screen), and the VDP1's registers zeroed at start (an
  upstream bug left them as heap garbage, and Daytona USA then never drew its 3D).

## 00.64 (test build)
- **3DS: its own resolution only, as on the console** (asked for by the owner). The 3DS has no Quality levels any
  more (00.63 gave it Low, Medium, High and Ultra: 1x, 2x, 3x and 3x): as the Dreamcast's pages say "Standard
  quality only", its pages say "3DS: the console's own resolution only", and every game plays at 1x (400x240 and
  320x240) whatever a settings.ini of 00.63 says (3DS Resolution is hidden and no level sets it). Quality's help no
  longer speaks of the 3DS. PC: Build3dsVariables over the 195 combinations of the settings sends 1x every time,
  with a 3DS Resolution of 2x or 3x in the file too.
- **The touchpad as a trackpad, on the DS and the 3DS** (asked for by the owner: the stylus jumped to where the
  finger landed). Putting the finger down never moves the stylus; moving it moves the stylus from where it was (the
  touchpad's height about the screen's); a short tap - at most a quarter of a second, the finger moved less than
  about 2 mm - touches the screen where the stylus is. To drag: hold the touchpad's click and move, or tap and touch
  again at once (within a quarter of a second) and move; a double tap is two touches. The right stick still moves
  the DS's stylus, and the 3DS's is drawn as before. DS / 3DS Touch Screen: Trackpad (the default; "Click to Tap"
  before) or Direct ("Touch to Tap" before: the touchpad is the screen, the finger presses where it lands). One
  piece of code for both (src/Touchpad.h). The log says which and counts the taps: `input: touchpad (trackpad): N
  frames with a finger, N with the stylus down, N taps in these 10 s; stylus at x,y` (the 3DS's in its `3ds:` line).
  PC: 50 checks on both screens (the finger down moves nothing, the same distance either way, taps and not-taps, the
  click's drag, tap then drag, a double tap, no tap from a finger lifted while the in-game menu is open, the edges,
  Direct); the settings' texts and their Spanish.
- **3DS Screens: One Above the Other is the default** - the 3DS's own look, the top screen over the touch screen (R3
  still swaps them); first in the list, in Settings > Video and in the in-game menu's settings. Side by Side and the
  others are still there, and a choice made before stays.
- **NAOMI and Atomiswave games start on the PS4.** 00.63 closed the app as one started (the owner's Dolphin Blue and
  KOF Neowave, fast memory on or off), without a crash report: the log ended right after the board's memory was set
  up. A board's normal start threw three C++ exceptions inside Flycast and caught them again - looking for its zip by
  its bare name (naomi_cart_LoadBios), opening flash defaults the core doesn't have (resource::load) - and on the PS4
  an exception the core throws doesn't come back to its catch (the app's own __cxa_throw, Sony's unwinder and
  personality routine): the app ends at once. Flycast asks first now (scripts/build-dc-core.sh); a Dreamcast disc's
  start threw none before either. A game Flycast would refuse - that refusal is an exception too - is now refused by
  the app with the reason, before the core starts: its board's BIOS zip not there, a GD-ROM game's disc image not
  there, a zip whose name isn't its set's in small letters on the PS4's storage. (A file missing from a set's zip
  still ends the app.) The cartridge's memory (16 to 384 MiB) is off the app's heap, in direct memory as the
  Dreamcast's RAM, and the log follows a board's start ("EmuSuNi: start: ..."). PC (the PS4's two memory paths, the
  harness counting every exception): the owner's Dolphin Blue and KOF Neowave with his awbios.zip play, 0 exceptions
  in their start; the Dreamcast's cold boots give the same pictures as 00.62's core.
- **NAOMI cartridge images.** A NAOMI game can also be the whole cartridge dumped as one decrypted file, a .dat or a
  .bin (the owner's Marvel vs. Capcom 2 and Virtua Tennis 2), loose in Roms/naomi: Flycast plays it as it is, with
  naomi.zip (not from inside a .zip or .7z: an archive it reads as a MAME set, by its name, so those stay unlisted).
  The Library reads the image's header (its board must be the NAOMI: a NAOMI 2's or an Atomiswave's image doesn't
  play this way) and finds its set by the file's name or the header's ("MARVEL VS CAPCOM 2 USA"): the title "Marvel
  vs. Capcom 2 New Age of Heroes", covers by the file's name (libretro's names for these dumps) and the set's;
  Information says "Board: NAOMI (cartridge image)" and "Needs: naomi.zip" while it isn't there. Flycast's "Please
  upgrade to MAME romsets" toast isn't shown, and a vertical game's image is turned as its header says. PC: both of
  the owner's images boot and play with his naomi.zip.
- **The console box says where the BIOS goes.** The Arcade's, NAOMI's and Atomiswave's BIOS rows: "neogeo.zip and the
  like in Roms/arcade or /data/EmuSuNi/system/fbneo", "naomi.zip, naomi2.zip in Roms/naomi or
  /data/EmuSuNi/system/dc", "awbios.zip in Roms/atomiswave or /data/EmuSuNi/system/dc" (no other console's box has
  one). When the box is full only "Plays" gives way: the BIOS, the files and the folders always stay (with three
  drives or more the box grows rather than lose a folder). The Files rows fit one line ("FinalBurn Neo 1.0.0.03 sets,
  one .zip per game (mslug.zip)", "MAME sets (mvsc2.zip) or .dat / .bin images, unzipped"). Checked with the real
  fonts in English and Spanish, one and two drives, a folder of the player's (layout-check.py).

## 00.63 (test build)
- **The app makes every console's folder itself**: `Roms/<console>` for each console of the build (the newest,
  NAOMI and Atomiswave, included: the list is the console table) in the PS4's storage (`/data/EmuSuNi/Roms`) and
  on the USB drives the player keeps games on: each drive that has a Roms folder (in its own spelling, `ROMS` or
  `roms` too), or `/usb0` when no drive has one; never a drive that looks like the PS4's own storage (games under
  `user/app`). At start and whenever the games are read again (a drive plugged in, Square); only what's missing is
  made, a folder already there in another letters' case counts. A read-only or full drive stops at its first
  failure and is said once in the log (one line per folder: `folders: /usb0/Roms: 3 made, 15 there`), the menu
  goes on. PC tests: which drives, the letters' case kept, a file in the way, a read-only drive and a full one
  (tmpfs), the new consoles.
- **The console box writes the folders out in full**, one line per place, as the app makes them and looks in them
  (players misread "Roms/arcade on a USB drive or in /data/EmuSuNi"): `USB: Roms/arcade` (the path as a PC shows
  the stick; with the drive, `(usb1)`, when several have one), `PS4: /data/EmuSuNi/Roms/arcade` (as FTP shows it),
  then `Also: <folder>` for the player's own folder of Game Folders. The box is 620 px wide (was 600); when a
  console's lines don't fit (several drives, a folder of the player's, long notes), its "Plays" and then "BIOS"
  rows give way, never the files and the folders. The empty pages say "Copy your games to Roms/psp on the USB
  drive or to /data/EmuSuNi/Roms/psp"; the Arcade's line reads "Neo Geo, Capcom CPS1/CPS2/CPS3, Sega, Konami,
  Taito..." (one line now); a console of zipped sets without a note of its own says "One .zip or .7z per game, not
  unpacked". Checked with the real fonts in English and Spanish (layout-check.py).
- **Controls from everywhere**: Settings has a **Controls** row of its own ("Each console's buttons on the DualShock
  4": the consoles, then one console's buttons), Triangle on a game -> Controls opens that game's console (Apply To:
  All Games, the console's normal layout, or This Game, its own), and the in-game menu (L3+R3) has **Controls** after
  Settings: the running game's console, the same rows (Cross, then the new button; Left: none; the D-pad's choices
  with Left / Right; Reset to Default; Apply To), applied when the game goes on (its layout is read again as the menu
  closes, and the pad reaches the game only once every button is up: the press that closed the menu isn't the
  game's). While a button is waited for (5 s), every press is the capture's: L3+R3 and Circle don't close the menu.
- **Fast Forward names its button**: its help on a game's page and in the in-game menu says the button that does it
  in that game, from its own controls or its console's, the player's own when changed ("Speed while R2 is held
  (change the button in Controls)"; on the N64, the touchpad), or "No button does it on this console yet: choose one
  in Controls" where it starts at None (the PlayStation, the Arcade, the Dreamcast, the NAOMI, the Atomiswave, the
  3DS: every button is the console's). Every console has the Fast Forward (hold) row in Controls; its help says a
  button the console uses too does both. The old text spoke of R2 and the N64's touchpad on every console. PC tests:
  each console's default, the player's own (L3 on the PlayStation), a game's own, None, a D-pad direction, read back.
- **Quality for every console**: Low, Medium, High and Ultra on every console, the 3DS too (3DS Resolution 1x, 2x,
  3x and 3x: its most, no GPU filters for its picture yet); the Dreamcast, the NAOMI and the Atomiswave play at one
  fixed level, Standard (their pages say so). **Low is each console's own resolution**, with no upscaler and no
  filter: the N64 at its native size, the PlayStation, the PSP (480x272), the DS and the 3DS at 1x, the 2D consoles'
  pictures as they made them; the PSP's Low has no anisotropic filtering now (4x until 00.62: a Low chosen before
  loses it, once) and every level leaves the PSP's textures as they are (Texture Upscaling Off). Medium, High and
  Ultra go up from there: Smooth Edges, Natural HD, Natural AI; the N64 at 2x, 4x, 4x, the PlayStation 2x, 4x, 4x,
  the PSP 2x, 3x, 4x; Ultra no further than the PS4's logs show at full speed (the N64 at 4x 96-100% in Mario 64,
  Banjo-Tooie, Donkey Kong 64, Perfect Dark; the PSP's God of War and Tekken 6 at 4x 94-100%). The levels no longer
  set Picture Size, Letterbox Zoom or the NTSC Filter (nor the bezels, Tube TV or Scanlines): the player's own look
  stays. On a game's page and in the in-game menu each level's help says what it is on that console, in plain words
  ("Low: The console's own resolution (480x272), with no filters."; the N64's and the 3DS's add "The size changes at
  the next start"); the level chosen is the one shown, also where two set the same (the 3DS's High and Ultra). PC
  tests: every console's four levels (each a step, Low at 1x without filters), the player's look untouched, This
  Console and This Game, the files of before (00.62's Low, a 4x set by hand at High, a file from before the PSP).
- **NAOMI and Atomiswave: two new tabs**, after the Dreamcast and the Arcade. Sega's and Sammy's arcade boards (the
  Dreamcast's hardware in a cabinet) play on the Dreamcast's core, Flycast 2.7. A game is a MAME set, one .zip (or
  .7z) named after it and not unpacked (`mvsc2.zip`, `kofxi.zip`), in `Roms/naomi` or `Roms/atomiswave`. The tab
  lists the zips named after one of Flycast's own sets (its list, read from the core: `naomi: 349 sets in Flycast's
  list: NAOMI 243, NAOMI 2 44, Atomiswave 34 (GD-ROM 105), System SP 28`; the NAOMI 2 games in the NAOMI tab, System
  SP's in none) by the set's full title ("Marvel vs. Capcom 2 New Age of Heroes (Export, Korea)") and its region,
  with covers from libretro's "Sega - Naomi" and "Atomiswave" thumbnails (by that title, else the parent's). A BIOS
  zip is no game. Each board has its badge, icon, saves (`saves/naomi`, `saves/atomiswave`) and place in Console
  Order (by year: NAOMI 1998, Atomiswave 2003); Triangle's Information shows the romset, its parent, the board
  ("NAOMI GD-ROM", "NAOMI 2") and what's missing.
- **The BIOS is the player's own**: each game boots from its board's, MAME's zip (`naomi.zip`, `naomi2.zip`,
  `awbios.zip`; four games `hod2bios.zip`, two `airlbios.zip`), beside the games or in `/data/EmuSuNi/system/dc`
  (where Flycast looks first). The console box says it; Information says `Needs: naomi.zip (not found)` (also a
  GD-ROM game's disc image, `<set>/<disc>.chd` beside the zip; a clone's parent zip "May need"); a game that doesn't
  start says why: no BIOS (and where to copy it), a BIOS zip without this game's files, a set Flycast doesn't know
  (named in small letters), a set whose files don't match Flycast's list (the missing file, the parent not found),
  the GD-ROM image, not enough memory. The log: `naomi: mvsc2, NAOMI, "Marvel vs. Capcom 2 ..."; BIOS naomi.zip,
  cartridge 136 MiB`, then each file not found and the core's own message.
- **Their controls** (Controls -> NAOMI / Atomiswave; Flycast reads the RetroPad): buttons 1 to 4 on Cross, Circle,
  Square, Triangle, 5 and 6 on R1 and L1, 7 and 8 on R2 and L2 (the Atomiswave has five), START on Options, the
  coin slot on the touchpad; the NAOMI's games on Free Play. TEST and SERVICE (the operator menu: coins, difficulty,
  the game's settings) are on no button: the in-game menu's new **Arcade Button** presses one for half a second
  back in the game (Left / Right: TEST or SERVICE; the menu opens on it again, for SERVICE presses one after
  another in the operator menu), or Controls puts them on buttons. A game with a wheel, a flight
  stick or pedals (79 sets) has them on the left stick (the D-pad stays digital: Crazy Taxi's gears) and on the
  triggers' pressure; a light gun game (20) aims with the left stick, with a sight for each player (white, red,
  green, blue), and reloads with Gun Reload (R1). Four players. A vertical game (22, Ikaruga) stands upright, 3:4.
- **Their settings**: the Dreamcast's that apply to a board (CPU Clock, Sound DSP, Frame Skip; Low quality, as the
  Dreamcast, which Quality's help now says), each board keeping its own with This Console. New **NAOMI Region**: USA
  by default, Export or Japan, the region of the BIOS the games run on (Flycast's default would be Korea's; a game
  made only for another region runs on that one); the Atomiswave's is Japan, its BIOS's only one. No cheats.
- Flycast's **printer fonts are back in the core** (444 KB): three NAOMI games print (Touch de Uno! 1 and 2,
  Nittere Shiki! Mirai Yosou Studio) and crashed at their first print-out without them (the print-outs go to
  `system/dc`); its F355 print template (F355 Challenge needs linked boards, not in this build) and its
  translations stay out. The game list is the core's own (`flycast_emusuni_arcade_list`, added by
  scripts/build-dc-core.sh), so the tab and the core never disagree.
- **Tube TV (CRT)** (Graphics, every console, changes at once): an old tube TV's look, drawn by the GPU on the
  console's own picture. Each of its lines is a beam of light (wider where it is brighter, so bright parts fill the
  gaps), a soft step between its pixels, the glass curved with rounded corners and darker edges, the phosphors' mask
  at the screen's own pixels and a little glow. Four looks: **TV** (soft, a dot mask), **Arcade Monitor** (sharper,
  darker lines, a slot mask, less curved), **Studio Monitor** (flat, crisp, an aperture grille), **Intense**. The DS's
  and the 3DS's two screens are a tube each, in every view of DS Screens and 3DS Screens; a vertical NAOMI or
  Atomiswave game is a monitor on its side. It takes the place of the GPU Upscaler and of the CPU's Resolution (one
  pass from the console's own picture) and of Scanlines (its own lines, never twice); the NTSC Filter's picture goes
  through it; the curved glass sits inside a bezel's window (with the Retro TV: a tube in the TV). One shader
  (`src/shaders/crt.frag`; for a picture drawn bigger than the console's, HD 3D, a smaller copy first for the glow):
  about 0.8 ms of the PS4's GPU a frame at 4:3 and 1 ms at 16:9 (estimated from its GCN code against the filters
  measured on the PS4); the log's `gpu filter: Tube TV (CRT) ... ms of GPU per frame` line measures it. PC test:
  lavapipe with Vulkan's validation layer (0 errors), every console's pictures, in bezels too.
- **A bezel for every console**: the SG-1000, Master System, Game Gear, Genesis, PSP, Nintendo DS, Nintendo 3DS,
  Dreamcast, NAOMI and Atomiswave get one of their own (original drawings of their shapes and colours, no logos or
  names), and every console can now choose six more: **Arcade Cabinet**, **Dark Minimal**, **Neon**, **Wood Panel**,
  **Carbon** and **Starfield** (the Arcade's own is the cabinet). Each of those has a window for each shape of
  picture (4:3, 16:9, two screens side by side, tall: one above the other or a vertical arcade game), chosen as the
  picture's shape changes (R3's other DS view, another picture); the DS's and the 3DS's own have one for each of
  their views; the PSP's is 16:9 and the Retro TV has a widescreen tube for wide pictures (a frame makes a picture as
  wide as the TV smaller: the PSP's, the DS's and the 3DS's Bezel help says so). Made by `scripts/make-bezels.py`, as
  palette PNGs (the 22 older ones turned into palette PNGs too, the same look): the 63 bezels take 2.8 MB (the 22
  took 1.9). A bezel's file is looked for only when the choice or the picture's shape changes; one texture at a
  time, freed before the next one is made (the textures' arena reuses its place: no memory growth).
- **A memory check before every game** (`src/MemoryGuard.h`, in `Emulator::Run` right after the `game:` line, before
  the ROM is read and the core starts): when the PS4's regular flexible memory runs out it closes the app without a
  word (00.58's God of War, started with 41 MiB free: the blue error screen, no crash report). After the start only
  thread stacks take that memory, and they are never given back but reused by later threads of the same size, so a
  start needs `16 + max(0, cold - reusable)` MiB: `cold` is the core's own cost from a fresh app, measured in the
  PS4 logs (PSP 63, 3DS 18 - no 3DS floor: its closes weren't memory -, N64 12, Dreamcast/NAOMI/Atomiswave 9,
  PlayStation 8, DS and Game Boys 6, the others 3; 12 more the first time the CPU upscaler runs), `reusable` the
  most free since the menu's first reading less what is free now less 4; the DS's first start adds 3 (its 1 MiB
  stacks). When less is free the game doesn't start and the menu's "The game couldn't start" panel says: "EmuSuNi
  is running low on memory. / Close EmuSuNi and open it again, / then start this game again. / To close it: PS
  button, OPTIONS, / Close Application." (Spanish too). Every start logs `memory (before game): flexible F MiB free,
  M at most so far (R MiB of thread stacks to reuse); PPSSPP needs N (its threads 63): starting` (or `not started,
  the player is asked to reopen EmuSuNi`). A number in `/data/EmuSuNi/memory-guard` is added to every need (999:
  every game refused, to see the panel; a negative one lets more through). PC test: the plan's replay of 64
  logged starts through the real header, line for line the same: the 49 that ran start, the God of War that died is
  refused; the panel's lines measured with the menu's fonts (335 px at most of 550).
- **Memory in the log**, to measure this version's memory work on the console: every `memory (...)` line adds the
  free memory in KiB, the heap's use (`heap U MiB in use of 512 (most M)`, counted by the malloc wrappers) and the
  threads alive by stack size with the most at once of each (`threads by stack (alive/most): 2064 KiB 14/30, ... =
  N MiB of stacks at most, M made`); `threads: default stack N KiB, guard N KiB` at start; the 10 s `speed:` line
  ends with `flexible N MiB free`; after `game: ended`, `memory (game ended)` and `memory: the game took N MiB of
  flexible memory (B MiB free before it, L at the lowest, E now); T threads started`. Every thread started with
  pthread_create (std::thread too; `--wrap=pthread_create`) is counted, and gets a 64 KiB stack for signals: a thread
  whose own stack overflows now reaches crash.log (`thread stack ...: it OVERFLOWED`), where the app used to end
  without a word; crash.log's stack scan stays inside the thread's stack. The app gives the cores' shared JIT arena
  back after every core's end (both `deinit` calls) and runs the start-up probe of the program's surroundings
  (`memory probe:` lines).
- **PSP: no new thread for every shader, and 30 MiB less of the app's image** (scripts/build-psp-core.sh). The PS4
  keeps every thread's stack (2 MiB) for the next thread and never gives it back, so the most threads ever alive at
  once is memory gone until EmuSuNi closes. PPSSPP made a new thread for every shader it compiled (God of War: 18 at
  once; its first session took ~30 MiB more after its start); now 4 threads of their own compile them all in turn,
  on the same cores (`ShaderGLSL 0` to `3`; at the game's end `EmuSuNi: N GLSL compiles on the 4 ShaderGLSL threads
  (at most M waiting at once)`), with 2 threads for file reads instead of 4 and no UPnP thread (UPnP isn't built
  in): a game's start makes 16 threads (15 before) and its play none (`ThreadManager::Init(compute threads: 4, all:
  6)`). Out of the image, which the PS4 charges whole at load: the PSP's 22 MiB of JIT memory, now its part of the
  cores' shared JIT arena, taken at each game's start (`EmuSuNi: 22 MiB of executable JIT memory at ...`; at the end
  `EmuSuNi: every JIT block freed at the core's end`), glslang's 6 MiB SPIR-V table (made only if its disassembler
  ever runs: never) and the ad hoc relay's 2 MiB of session tables (on the heap while a game uses the relay): the
  core's .bss 32.7 -> 2.8 MiB. PC tests (the PSP harness with the app's options and render thread): 69 pspautotests
  (CPU and GPU) give the same output as 00.62's core, their 141 shader compiles on the 4 threads instead of 141
  threads; at most 15 of the core's threads alive at once (20 before); two games in one process, the same pictures
  as before, and the PS4's stack policy replayed on the threads: 18 stacks (36 MiB) after both games (00.62: 21, then
  23 after the second); another core filling the whole arena between two games changes nothing; without the arena
  the IR interpreter runs; saves and the picture's live changes as in 00.62.
- **One shared JIT arena for the recompilers, and the N64's 38 MiB out of the image** (port/ps4_n64.c,
  scripts/build-n64-core.sh). The code the recompilers write goes into one 32 MiB array of the app's data
  (`emusuni_jit_arena`), not one array per core: the N64's, PlayStation's, PSP's, DS's and Dreamcast's were 111 MiB
  of the program's .bss together, and the PS4 charges the whole .bss to the games' memory when it loads the app. In
  the program's data the arena stays within 2 GiB of every core's code, as their x86-64 code needs. Only one core runs
  at a time: each takes its part as it starts, the same memory again if it asks twice in one game (the N64 after a
  reset), and the app gives it back after every core's end. It is then filled with HLT, so a jump into a gone
  core's code faults into crash.log instead of running old code. It is made executable the way it was before (mprotect
  on the owner's console) and again at every take. The log: `jit arena: 32.0 MiB at ... (+N MiB from the code's
  start), read/write/execute by way 0 (mprotect)` once, then `jit arena: 32.0 MiB at ... for n64 (+0.0 MiB of the
  arena, way 0)` at each N64 start and `jit arena: given back by n64 (5.6 MiB of 32.0 MiB written)` at its end. The
  N64's own 34 MiB array (its 32 MiB cache and 2 MiB of alignment) left its `g_dev` (60.5 -> 26.5 MiB). GLideN64's
  4 MiB command buffer (that renderer never runs here) is on the heap. The dynarec no longer takes the execute right
  back at a game's end. The N64's part alone is 6 MiB less at load (38 out, the arena's 32 in). With the other
  cores' moves of this version so far (PlayStation, DS and PSP onto the arena, and the Genesis', Arcade's and PSP's
  tables to the heap), the image is 257.0 MiB at load (346.0 in this version's earlier builds), about 181 MiB free at
  display ready. The Dreamcast's 15 MiB move is still to come.
  PC tests:
  - the arena's rules, with the real port/ps4_n64.c (the PS4's calls mocked) and with the PC stub the other cores'
    test frontends use;
  - Mario Kart 64, Mortal Kombat 4 and Mario Kart 64 again in one process on the dynarec, ended and started as the
    app does with the arena given back in between: the same address each time, every picture the same as with
    00.62's core, and the first game bit for bit (RDRAM too).
- **The PlayStation, DS, Genesis and Arcade cores: 53 MiB less of the image** (the memory plan's first two steps for
  them; scripts/build-ps1-core.sh, build-nds-core.sh, build-gpgx-core.sh, build-arcade-core.sh). SwanStation's
  recompiler (24 MiB: 16 of code, 8 of slow paths) and melonDS's JIT (16 MiB) write their code into their part of
  the shared JIT arena instead of arrays of their own: `jit arena: 24.0 MiB at ... for ps1`, `jit arena: 16.0 MiB at
  ... for nds`, and melonDS's `JIT: 16 MiB of executable code memory at ... (the app's JIT arena)`. Each takes its
  part when its recompiler starts (the DS for every console it makes, a reset's too), the same memory again within a
  game, and keeps nothing of it after the game. If the arena can't be had, the PlayStation runs its cached
  interpreter and the DS its interpreter, as when the PS4 refused executable memory before. Genesis Plus GX's
  cartridge memory (the 10 MiB ROM buffer with the cartridge's RAM and registers, or the Mega-CD's) is on the heap
  while the core runs (upstream's USE_DYNAMIC_ALLOC: made at the core's start, given back at its end; the Master
  System's, Game Gear's and Mega-CD's BIOSes it held are loaded again the next time). FinalBurn Neo's Philips XA and
  TLCS-900 page tables (3 MiB; no game of the list uses either CPU, but the program kept them) are allocated when
  such a CPU starts, like the TMS34010's and E1-32XS's. The four cores' builds now stop if a static array of 1 MiB
  or more comes back. The DS's profile-guided optimisation still applies (only two start-up functions of its JIT
  memory changed). PC tests: prueba-sega (284 checks) and prueba-arcade (232; all 8,441 drivers started and closed)
  pass; Tekken 3 and Gran Turismo 2 on the PlayStation, Mario Kart DS and New Super Mario Bros. on the DS give the
  same pictures in two games in one process, with the arena given back and filled with HLT in between; with the
  arena refused the interpreters run, without a crash.
- DS: **Reset no longer crashes**. The core keeps one copy of the ROM and moves the cart to the new console at a
  reset; melonDS had encrypted the cart's secure area for the first boot, so without the native BIOS the new console
  refused the ROM as encrypted ("The loaded ROM is encrypted and needs native NDS BIOS files"), the core asked to
  stop, and the next frame ran with no console (on the PC: a crash in `NDS::SetKeyMask`). A reset's cart now skips
  that check: it passed it when the game loaded, and the same BIOS decrypts it again. PC test: New Super Mario Bros.
  and Mario Kart DS reset at frame 700, in two games in one process: both play on, the same pictures each time.
- **Two more memory readings in the log**:
  - every emulator thread's line now ends with the flexible memory free (`psp: thread "PoolW 0": ..., flexible N KiB
    free`; N64, DS, Dreamcast and 3DS too). A new thread takes 2 MiB + 16 KiB, and the next thread of the same size
    reuses that stack.
  - the start-up probe (`memory probe: ...`, two lines) looks at the addresses just past the program, where 00.64 may
    map the arena from the system's memory instead (32 MiB more free at start). It names the program's data and the
    next mapping, reserves 64 MiB past the program, maps 16 KiB of system flexible memory read/write/execute there and
    runs code in it. It also tries Resonance's way (a plain read/write/execute mmap hinted 64 MiB past the code), then
    gives everything back. It frees nothing and costs nothing.

- **3DS: no more closing at the 3D title screens** (Super Mario 3D Land after the StreetPass dialog, Hey! Pikmin at
  "Touch to begin": the screen went black, the log showed "Run out of pools" and the app closed). The first draw
  with a geometry shader stopped the PS4's GPU (the GPU is GFX7: without fragment-shader barycentrics Azahar gave
  every lit draw one). Azahar now draws without geometry shaders on the PS4, as on Android
  (scripts/build-3ds-core.sh), and the app's Vulkan device no longer asks for them (src/Display.cpp; no other core
  used them). A descriptor-set allocation that fails for another reason now ends the game with a message instead
  of spinning.
- **3DS faster**: the right eye is no longer drawn (the PS4 never shows 3D; Super Mario 3D Land 1-1: 18% less CPU
  and 41% less GPU on the PC; the file /data/EmuSuNi/3ds-right-eye brings it back for a game that flickers),
  the core is built at -O2 (2-7% faster than -O3 in the tests), and while the 3DS is heavy its picture is drawn
  beside the emulation, as the PSP's and the Dreamcast's are.
- **Smoother pacing with V-Sync off** (every console but the N64): a pass of about one refresh counts as exactly
  one and the difference is given back a whole frame at a time, so a heavy game no longer flips between showing
  a frame twice and skipping one (the 3DS's "190 the same frame again, 190 jumped 2+ frames").

## 00.62 (test build)
- PSP: **games save again** ("it doesn't save my games, as if the memory card weren't created": FIFA 14 left an
  empty `saves/psp/SAVEDATA/ULES01590001` folder). The memory stick is `saves/psp`, a folder named PSP, which PPSSPP
  takes for the stick's own PSP folder (`ms0:/PSP/SAVEDATA/<save>` is `saves/psp/SAVEDATA/<save>`), but on the PS4's
  storage, whose names are case-sensitive, it first looked up every name of a path for its case, `PSP` too, which
  isn't a folder there: every file a save writes was refused ("file not found", 00.61's log) while its folder was
  made. Now only the part on the disk is looked up (scripts/build-psp-core.sh); the folders stay as they are (no
  `PSP` level). The PC tests never saw it: their stick's folder was named `saves`. In the log, every save:
  `EmuSuNi: save written: ms0:/PSP/SAVEDATA/<save> (PARAM.SFO n bytes, DATA.BIN n bytes, its pictures) in ...` or
  `EmuSuNi: save NOT written: <save>: <why>`, and the free space told to a game that asks (`EmuSuNi: free space
  told to the game: ...`, with 00.57's `disk: free space at ...`). PC: the savedata demo writes its four files
  with the folder named `psp` (none before); pspautotests' savedata tests give the same output as with a folder
  named `saves`; a save refused (SAVEDATA a file) says why.
- PSP: **Quality and the PSP's picture settings change while playing**. PSP Resolution and PSP Anti-Aliasing
  applied only at the next start since 00.39 (a resolution change while playing zoomed into a corner: the core's
  Vulkan picture kept its size). Now the core makes the PSP's framebuffers (the size it draws the 3D at) and its
  picture again at the new size, as PPSSPP's own frontends do when a window changes size (its old images kept
  until the app can't be showing them, each picture handed over with its own size; log `EmuSuNi: the PSP's
  framebuffers are drawn at 1920x1088 (x4, ...)`; on the PC a game switched to 4x while running gives the same
  pictures as one started at 4x); PSP Texture Sharpness's 4x / 8x / 16x showed only the first level used (its samplers
  are made again now); PSP Texture Upscaling already applied at once. Anti-aliasing changed while playing makes
  the 3D's shaders again: its help says it can stutter a few seconds. Quality's help says what still waits for
  the next start: the N64's size and sync. Log: `EmuSuNi: the picture's size changed while the game runs: 480x270
  -> 1920x1080 (the swapchain made again)`, `EmuSuNi: anisotropic filtering 4x -> 16x ...`, `EmuSuNi:
  anti-aliasing (MSAA) x1 -> x4 ...`. The file `/data/EmuSuNi/psp-resolution-at-start` brings the old way back
  (resolution and anti-aliasing as the game starts). PC (two demos, PPSSPP's render thread on and off, Vulkan
  validation on): 1x, 4x, 2x, 1x, 3x and 1x again, MSAA x4 and off, anisotropy 4x and 16x, 2xBRZ, a resolution
  change every 2 frames for 60 frames, two games in one process: no validation error, no picture bigger than its
  image, no image used after it was destroyed, the whole picture at every size. The timing lines after Reset
  Game no longer show huge numbers (the GPU's counters restart from zero then).
- PSP and Dreamcast: **heavy scenes show the newest picture**. While the core is heavy the drawing runs beside the
  emulation (since 00.40) and the core's picture waits until the drawing is done; the wait dropped the image the
  core had just handed over (`VideoRefresh` replaced the whole pending frame), so the drawing kept an old image the
  core went on drawing into (a stale or torn picture) - and, with 00.62's live resolution, one the PSP core frees
  30 frames after a change: a crash. The pending frame is now filled field by field (src/Emulator.cpp); the DS,
  which hands its pictures as pixels, is unchanged, and the N64, PlayStation and 3DS never draw that way. PC (the
  app's way emulated in the test frontend): 0 errors; the old way: an image used after it was freed.
- PSP: **the VRAM mirrors are the same memory**, as in PPSSPP's PC builds (until now each of the three had 2 MiB of
  memory of its own, as PPSSPP's NO_MMAP builds: a write through a mirror didn't reach VRAM). The PS4 maps the
  same direct memory twice only after `sceKernelEnableDmemAliasing()` (its second form if the first is refused),
  which the app now asks for once at its start, before any direct memory is mapped (port/ps4_runtime.cpp,
  src/main.cpp; the Dreamcast core asked for it only when a Dreamcast game started), with a self-test: one page
  mapped at two addresses, a write through each read through the other (`dmem: aliasing on (0, 0); self-test ok:
  one page mapped twice, the same memory`, or what failed). The PSP core checks each mirror the same way before
  using it and falls back on memory of its own (as before) when the mapping is refused or isn't the same memory:
  `VRAM mirrors aliased (the same memory mapped again 3 times, self-test ok)` or `VRAM mirrors NOT aliased (...):
  <why>`. The shader cache stays off (00.61).
- **An information box on each console's page** (Library style, bottom left, under the covers): the systems it
  plays, the BIOS it needs as the app and its core handle them (none; optional ones and their folder; the Famicom
  Disk System's `disksys.rom`; built-in BIOSes for the PlayStation, DS and Dreamcast and where the real ones go;
  the Arcade's `neogeo.zip` and the like beside the games), its files and its folders. 25 px text (a size the
  font atlas already has), Spanish too; a long game title moves right of it instead of under it. The empty Arcade and 3DS pages keep one line of their
  own (the rest is in the box).
- **Games are found without choosing folders**: `Roms/<console>` on every USB drive (`/usb0` to `/usb7`, any
  letters' case: `roms/SNES` too) and in the PS4's storage (`/data/EmuSuNi/Roms/<console>`, made at start), as
  well as the console's folder in Game Folders (or its old default one, `games/` and `nes/`). Only the drives'
  top folders and their Roms folders are read to find them; a drive plugged in later is found when a tab is
  entered (looked at every 1.5 s at most) or back from a game, and Square reads the games again as before. A game
  reached through two folders is one game.
- **Settings → Consoles Shown**: All Consoles, Only Those With Games, or Only the Ones I Choose (each console on
  or off on the same page; turning one on or off chooses it). The Library's tabs and the XMB's columns follow;
  when that would leave none, all are shown. Their games stay in All and Favorites.
- **Settings → Covers Saved On** (was Covers): Internal Storage (the default, as before) or USB Drive
  (`EmuSuNi/covers` on the first drive with an EmuSuNi folder, else the one with the games); without the drive,
  the internal storage. Covers in both places are shown; Download Covers leaves out the games that have one in
  either place; Find Cover replaces a cover where it is.
- These three are global values of `settings.ini` (a tour's `set consoles_shown games` works) that Reset All
  Settings keeps. New tour commands: `tab <console key | all | favorites | settings>`, `open consoles | covers |
  folders`. PC tests of the folders, the library over them, the consoles shown and the covers' USB folder:
  105 checks with ASan and UBSan.
- **Dreamcast: the sound DSP is off unless chosen** (Settings → Performance → **Dreamcast Sound DSP**, Off / On; This
  Game, This Console or All Games, as every setting): Flycast's own default outside libretro (libretro's was on), and
  Resonance plays without it. Some games' echo and reverb need it (On; it changes while the game runs). On the PC,
  from save states (3 runs each), the emulation thread needs about 6% less time a frame without it: Daytona USA
  3.37 -> 3.16 ms, Crazy Taxi 2.81 -> 2.65 ms (its reverb: the sound 11% quieter); Marvel vs. Capcom 2 doesn't use
  it (the same time, the same sound).
- **Dreamcast CPU Clock** (Settings → Performance): 100% (the default, the SH4's 200 MHz), 90%, 80%, 70%: Flycast's
  under-clock (`reicast_sh4clock` 180, 160, 140 MHz). The game's code gets that much less time a frame: lighter for
  the PS4, but a game that needs the whole processor then slows down. Read as the game starts (the core counts the
  clock into each block of code as it compiles it, so a change in the game waits for the next start). On the PC
  (DSP off, 3 runs each from save states) the emulation thread's time a frame at 90 / 80 / 70%: Daytona USA's race
  -7 / -10 / -13% (3.16 ms at 100%), Crazy Taxi -3 / -7 / -12%, Marvel vs. Capcom 2 -5 / -10 / -14%; the pictures
  and the games' own clocks (Daytona's lap time, Crazy Taxi's game time, the fight's timer) the same at every step:
  these scenes don't need the whole 200 MHz (Daytona's race is the same even at 100 MHz, -15%), the clock only takes
  the time the game waits. With the DSP off too, Daytona's race at 70% needs 19% less than with 00.61's options. For
  Daytona USA, whose races ran at 73-87% on the PS4 (its emulation thread busy 16-21 ms a frame for a 16.7 ms frame).
- **Dreamcast threads, an A/B for the PS4**: with the file `/data/EmuSuNi/dc-threads-free` (empty, put there by
  FTP) a Dreamcast game's threads are left on the cores the PS4 gives them, at the priority they start with, as
  Resonance runs Flycast: the renderer (retro_run's thread), Flycast-emu and the drawing beside them; without it,
  as before: the renderer on core 2, Flycast-emu on core 3, both at priority 480. The game's start says which in the
  log (`dc: CPU clock 200 MHz (Dreamcast CPU Clock), sound DSP off; threads placed: ...` or `... threads left to
  the PS4 ...`), and each thread its cores and priority.
- **Nintendo 3DS in every build, marked experimental**: `scripts/build.sh` builds and links Azahar by default now
  (`no-3ds` leaves it out; `with-3ds` is still taken). Its memory: 127.5 MiB of file, 345.5 MiB in memory (MemSiz), about
  93 MiB of flexible memory free at display ready, 119 without the 3DS (see the next point). A 3DS game's settings pages say "Nintendo 3DS: experimental".
- **The 3DS no longer costs every game 64 MiB**: the PS4 charges a program's whole .bss to the games' flexible
  memory at load, not only its code and data (the owner's logs: the free memory at "display ready" fell by code,
  data and .bss each time cores came in: 00.32 -> 00.33, 00.33 -> 00.36, 00.46 -> 00.48, 00.55 -> 00.58), and 64 of
  the 3DS core's 64.4 MiB of .bss were its recompilers' code memory: as built, the 3DS would have left about 28 MiB
  free at display ready. That memory is now mapped when a 3DS game starts, from the system's own flexible memory,
  readable, writable and executable, on a range reserved above the app's image and heap (as DolphinPS4's JIT
  buffers; checked by running code there), and given back when the game ends (log: `3ds: JIT memory (3ds jit): 64.0
  MiB of system flexible memory at 0x..., read, write and execute at once`, then `... given back`). If the PS4 refuses
  that, or code faults there, the same range is mapped read/write and made executable the way the other recompilers
  use on the console (log: `... made executable by way 0 (mprotect)`). If nothing works, the 3DS game doesn't start, saying "The PS4 gave no memory for the 3DS's processor" (the log says why), and nothing
  else changes. Both recompilers reach the emulator only through far calls, so the mapping can be anywhere (on the
  PC it lands terabytes from the code, and the tests run there). The program in memory: 345.5 MiB (MemSiz, .bss
  included; 409.5 before), about 93 MiB free at display ready (119 without the 3DS). `scripts/build.sh`'s image line
  counts MemSiz now: `image: 127.5 MiB of file, 345.5 MiB of memory at load (MemSiz): about 93 MiB of flexible
  memory free at display ready` (about 438 - MemSiz: 00.55 151, 152 measured; 00.59 54; 00.60 119). PC (the harness,
  the core built for this PC): Super Haxagon with the app's options and a save state, two games in one process (the
  memory given back and mapped again; the same picture), the memory capped at 20 MiB (runs) and at 1 MiB (refused
  with a message), executable memory refused (the game doesn't start, with the message), Craftus Reloaded in every
  screen layout.
- **In-game menu: the press that closes it no longer reaches the game.** Back from the menu (Resume Game, Circle,
  L3+R3, a state loaded, Reset, a disc changed) the game sees player 1's pad untouched - no button, the sticks in
  the middle, the triggers up, no finger on the touchpad - until every button is up: Cross or Circle, or L3+R3 and
  the sticks pushed with them, acted in the game before (it read them at its next frame). The log says how long:
  `input: after the menu, player 1's pad kept from the game for N passes, until every button was up`. Inside the
  menu (Settings, Cheats, the states) the pages act only on new presses already. PC test with ASan and UBSan: the
  two Dreamcast settings in their scopes on the owner's settings.ini, and the menu's wait on scripted pads.

## 00.61 (test build)
- PSP: **God of War: Ghost of Sparta starts again** (it closed the app with the blue error screen at its
  second start since 00.58). The crash came right after the core opened the game's shader cache
  (`saves/psp/SYSTEM/CACHE/<game>.vkshadercache`), which makes every pipeline of the session before again
  at once; with the file moved away the game started. The shader cache is now off on the PS4 (scripts/
  build-psp-core.sh; `EMUSUNI_PSP_SHADER_CACHE=1` turns it on for the PC tests): each pipeline is made
  when the game first draws with it, so a game can stutter briefly the first time it shows something new.
  The memory diet of 00.60 was not the fix: 00.60 started God of War with 80 MiB free and still closed.

## 00.60 (test build)
- **Nintendo 3DS** (Azahar 2126.2, the Citra fork, through its libretro core, linked in as the other cores:
  scripts/build-3ds-core.sh, about 11 minutes to compile on 4 cores; scripts/setup.sh downloads and checks the
  source), **only in builds made with `scripts/build.sh with-3ds`**: the core adds 27.2 MB of code and data to the
  app's image (the loader's "image" line of the build: 101.4 MiB without it, 127.3 MiB with it) and
  67.6 MB of zeroed memory, and the PS4 maps that image out of the same flexible memory the games use (about
  118 MiB free at start become about 92), so the normal build stays as it was, without the
  tab (checked: none of the core's symbols or strings are in it). Built with a section for each function and variable
  (`CTR_CORE_SECTIONS=1 scripts/build-3ds-core.sh <dir>`; not the default yet, the gc has not been run on the core on
  the PC harness) the app's link, which leaves out what nothing uses (`--gc-sections`, in the toolchain file since the
  image was shrunk), drops 2.9 MB of it: 24.3 MB, and the eboot is 3.2 MB smaller.
  One tab for the 3DS: `Roms/3ds` on the USB drive, decrypted dumps only (`.3ds` / `.cci`, `.cxi`, a homebrew's
  `.3dsx`; no `.cia`, no `.zip`), covers from libretro's "Nintendo - Nintendo 3DS". The PS4's GPU draws the 3DS's
  two screens (Azahar's Vulkan renderer on the app's own device, the vertex shaders on the GPU), the CPU is Azahar's
  recompiler (dynarmic) in the executable memory the app hands out, the sound is HLE. All 32 options the core
  declares are answered (it falls back to its own defaults for any it isn't told). Controls: Circle Pad on the left
  stick, C-stick on the right stick, B / A / Y / X on Cross / Circle / Square / Triangle, L / R on L1 / R1, ZL / ZR
  on L2 / R2 (the New 3DS's), START on Options, SELECT on L3, **R3 swaps the two screens**, and the touchpad is the
  touch screen as on the DS (the finger moves the stylus, the click taps, held it drags; the stylus is drawn by the
  app over the touch screen wherever it is shown); no HOME button yet. Settings: 3DS Screens (side by side, one above
  the other, the top screen big with the touch screen small, the top screen only, the touch screen only), 3DS
  Resolution (the 3DS's own size, 2x or 3x: the next start), 3DS CPU Clock (100%, 75%, 50%: how fast the game sees its
  processor, lighter for the PS4), 3DS Touch Screen (Click to Tap, Touch to Tap), New 3DS Mode (off by default; the
  next start; the few games made only for the New 3DS ask for it and the error page says so). Quality, the GPU filters
  and Frame Skip aren't for the 3DS (it plays at its own size). Saves, states and the core's own files (virtual SD
  card, NAND) are in `saves/3ds/` and `states/3ds/`; no cheats; translated to Spanish. Most 3D games are heavier than
  the PS4 can run at full speed: the light 2D ones play well.
- The Vulkan device for the 3DS: Azahar asks for features ParaLLEl-RDP's loader doesn't (extended dynamic state,
  custom border colours, 8-bit indices, fragment shader interlock and barycentric coordinates, and the geometry
  shader); in a build with the 3DS the app adds them to the device it creates (a hook on `vkCreateDevice`; the other
  cores are told the lists they always were), and makes it again without them if the driver refuses, telling the core
  only what the device really has (`EMUSUNI_VK_DEVICE_FEATURES`). The hook's own text, compiled for the PC and run
  against a Vulkan implementation (lavapipe), makes the device with the extras and Granite's create info untouched in
  the chain, makes it again without them when the driver refuses, and gives the core the lists it should.
- Two changes to the core's sources (`EMUSUNI_PORT` only): Crypto++ looked at the CPU's features at start-up through a
  SIGILL handler, which replaced the app's own crash handler (found by following every static initialiser's calls
  down to the imports: it is a plain CPUID now); the core's log filter at Info, not Debug. What the initialisers reach,
  by that analysis (8,032 functions): the heap, the C++ runtime, string and memory functions, locks, stderr; nothing
  that maps memory, starts a thread, reads a file or the clock, or installs a signal handler; on the PC they run in
  no time and 10 MB.
- Checks on the PC (no 3DS console and no game of Nintendo's): the real `Build3dsVariables` against the options the
  core declares (`core_settings.cpp`: 32), over 195 combinations of the settings and the 13 languages: every declared
  option answered with a value it knows, none it doesn't, numbers that parse, the picture's size and the touch
  screen's place as the core lays them out; Super Haxagon and Craftus Reloaded (homebrew) through the PC harness with
  the options the app sends: in 17 runs (every screen layout, swapped or not, at 1x, 2x and 3x) the touch screen's
  rectangle, cut out of the core's own picture, is the same bottom screen with the taps on the same buttons, and
  the picture has the size the app expects; the save state; Reset in the middle of a game (the game starts again
  exactly as the first time); the library (the four extensions, no `.cia`, no `.zip`), the settings (an old
  `settings.ini` unchanged) and the controls (L3 is SELECT and never reaches the core) with and without the 3DS in
  the build.
- **Dreamcast speed, from Resonance** (https://github.com/iHaiDeeZ/Resonance, a Flycast-based Dreamcast
  emulator for the PS4 by the author of DolphinPS4; GPL-2.0): fast memory (the recompiled code reads and
  writes the console's RAM, VRAM and sound RAM directly instead of through the memory handlers: one block
  of direct memory mapped as often as the console mirrors it; tried when the core starts and again at the
  game's start, and if the PS4 refuses any step the log says "vmem: fast memory off: <why>" and the old
  memory path runs; a start that died inside it is not tried again until `/data/EmuSuNi/dc-fastmem-crashed`
  is deleted), the SH4 recompiler jumping from block to block and reading its block table inline, and
  VRAM pages that fault too often (a fault costs the PS4 about a millisecond) checked by hash instead of
  write-protected. Each can be switched off without a new package by a file in `/data/EmuSuNi`:
  `dc-fastmem-off`, `dc-dispatch-off`, `dc-hotpages-off`. The log line every 10 s now counts the faults
  by kind. On the PC test (Crazy Taxi and Marvel vs. Capcom 2 from a save state, 3 runs each) the pictures
  are the same and the emulation thread needs 15% and 20% less time a frame; the PS4's slower cores
  should gain more (not measured there yet).

## 00.59 (test build)
(The Arcade tab of 00.58 corrected after a review, with what 00.58 lacked: see below.)
- The Arcade corrections: Aspect Ratio Auto takes the board's own shape; a missing parent or BIOS zip
  is only a hint (the core takes ROMs by CRC from any zip it finds), the error page says it in short
  lines that fit and are translated; the Information lines fit the screen for every game; the spanish
  "Arcade" is no longer swallowed by a line with its own separator; SET_VARIABLE is answered for the
  Arcade only; Clean Up Games leaves the Arcade alone.
- **Arcade** (FinalBurn Neo v1.0.0.03, linked in as the other cores: scripts/build-arcade-core.sh, about
  2 minutes to compile): one tab for every arcade board it emulates (Neo Geo, CPS-1/2/3, Cave, PGM,
  Toaplan, Konami, Sega System 16, Midway...). A game is its romset, a `.zip` (or `.7z`) named after the
  core's driver (`mslug.zip`) in `Roms/arcade` on the USB drive, made for the core's DAT v1.0.0.03
  (FBNeo's `dats/`); the Neo Geo games need `neogeo.zip` beside them (or in
  `/data/EmuSuNi/system/fbneo`), a clone its parent's zip. Titles from the core's own driver list (a
  clone with its parent's), covers from libretro's "FBNeo - Arcade Games" by the romset's full name (the
  parent's as a fallback); BIOS and console sets are never listed; Information says the romset, its
  board, maker and year and the zips it may need that aren't beside it (neogeo.zip, a clone's parent: a
  split set needs them, a non-merged one carries their files, so the core is left to try and the game is
  never refused for them). When the core refuses a game, the error page says why in short lines that fit
  it (and are translated): romset unknown; the zips not found and where they go (a Neo Geo game without
  neogeo.zip); files that don't match its DAT, with the first missing ROMs. Clean Up Games leaves the
  Arcade out (a clone is no duplicate of its parent, whose zip it may need). Vertical games are shown
  upright (the app turns the picture as it comes in; Aspect Ratio Auto is the board's own shape, 4:3 or
  a vertical game's 3:4, Pixel Perfect its pixels),
  and Frame Skip, Resolution, the GPU filters and the bezels (Retro TV, your own) work as for the other
  2D consoles. Controls: the DualShock 4's buttons as FinalBurn Neo's "Modern" pad (buttons 1-4 on
  Cross, Circle, Square, Triangle, 5-8 on R2, R1, L2, L1; a Neo Geo game's A, B, C, D on Square, Cross,
  Triangle, Circle, and a six-button fighter's punches and kicks, said on the Controls page), COIN on the
  touchpad, START on Options, no fast forward. Settings: Arcade 60 Hz (every game at 60; off, the
  default: each board's own rate) and Neo Geo (the European MVS the BIOS DIP switch gives, the MVS of
  another region, or the AES home console). The high-score table (hiscore.dat) ships in the package
  and is copied to `system/fbneo` at the first arcade game. No cheats for the arcade yet (libretro's
  don't fit the core; FinalBurn Neo's own come later); NAOMI/Atomiswave through Flycast in a later
  version.
- FinalBurn Neo's licence (non-commercial, no donations) ships verbatim in the package (`licenses/`),
  and THIRD_PARTY_NOTICES says so.
- The app's static memory: FinalBurn Neo's two big tables (the TMS34010's 64 MB, the E1-32XS's 16 MB)
  are allocated only while such a game runs; its 4.5 MB Chinese error font is left out (the app shows
  the errors itself).
- FinalBurn Neo's driver list can be started and closed again (its BurnLibExit freed the drivers' name
  copies without putting the originals back, and the next BurnLibInit freed them again: RetroArch never
  saw it, it unloads the core between games; here the second arcade game would have crashed).
- A game the core refuses before its driver starts (files missing) is no longer closed as one that ran: the
  Exit of some boards (the Mega Drive ones') crashed when their Init never ran, so any incomplete romset of
  such a board would have crashed the app. Found by giving each of the 8,441 arcade drivers a zip that
  isn't its own: all are refused with their reason and closed, none crashes (about 40 s).
- PC test (local/herramientas/prueba-arcade), 10 of MAME's 12 freely distributable romsets the core
  drives (Gridlee, Robby Roto, Teeter Torture, Circus, FAX, Hard Hat, Rip Cord, Robot Bowl, Spectar,
  Targ; the free Alien Arena and Side Trak zips lack a sound ROM this version wants and are refused
  naming it, as the app would): 599 pictures in 600 frames each (the core draws nothing for its first
  frame), 48 kHz sound as expected in every one and heard in 7 once a coin is in, every option it
  declares answered with a value it knows, states back to the same picture, the Modern pads, Frame Skip
  through GET_AUDIO_VIDEO_ENABLE, reset; 0.1 to 0.6 ms per frame on the PC. The driver list: 29,338
  drivers, 8,441 arcade games, 14 BIOS sets and 5,831 clones, the DAT's 8,456 sets exactly; 2,521
  vertical games. The turned picture: the core's vertical mode makes Gridlee ask rotation 3 with a 3:4
  aspect and the app turns its 256x240 into 240x256 (the rotation itself on a numbered picture, all four
  ways); a romset the core doesn't know and a zip with another version's files are refused with the
  reason the app shows. The Neo Geo isn't tested here: its BIOS isn't free.
- **A game's settings apply to its console by default** (asked for by the user: everything changed for all
  the consoles at once): the first time the Game Settings page of a game is opened (the menu's or the
  in-game one), Apply To is This Console - its values start as the ones the game had, so nothing
  changes at first. A game set to All Games on purpose stays so.
- **The empty Arcade tab explains itself**: what is inside (Neo Geo, Capcom CPS1/2/3, Sega, Konami,
  Taito...), that each game is one .zip left as it is with its short name, that only FinalBurn Neo
  v1.0.0.03 sets work and that the Neo Geo games need neogeo.zip beside them.
## 00.58 (test build)
- The first version of the **Arcade** tab (FinalBurn Neo), the Sega consoles' names and "This Console" as
  the default Apply To of a game's settings; see 00.59 for the Arcade as it ended up.

## 00.57 (test build)
- **PSP games save again**: the PS4 libc's statvfs fills nothing (it calls statfs64 into a buffer of
  its own and returns only its sign), so PPSSPP read zeros for the memory stick's free space and the
  games refused to save ("not enough space"; a YouTube report, and God of War's "corrupt data"). The app
  answers now (port/ps4_diskfree.c: the kernel's statfs, checked by its version, else 2 GiB) and logs
  what it found ("disk: free space at ...").
- **Genesis**: the Mega Drive is named Genesis (tab GEN, badge GENESIS); Japanese games keep "Mega
  Drive", as the Super Famicom. Folders and saves unchanged (`Roms/md`).
- **The DS's touchpad in the log** every 10 s (frames with a finger, with the stylus down, where the
  stylus is): a player can't start Professor Layton (touch-screen games), so whether the touchpad
  reaches the game on the PS4 is checked first.
- **A notice when a Dreamcast game starts**: its first start compiles the shaders (black for up to a
  minute) and a PAL disc then asks 50/60 Hz on a silent screen that waits for A; players took both for
  a game that doesn't start. Read again, the 00.53 tour's "frozen" Marvel vs. Capcom 2 (Europe) was that
  screen: with the app's options the PC core shows it from frame ~300 to ~3000, then the logo and the
  memory-card screen (Options); choosing 50 Hz with the VGA cable changes nothing.

## 00.56 (test build)
- **SG-1000, Master System, Game Gear and Mega Drive** (Genesis Plus GX, one core for the four, as mGBA
  for the Game Boys; scripts/build-gpgx-core.sh): their tabs (SG-1000, SMS, GG, MD), folders
  (`Roms/sg1000`, `sms`, `gg`, `md` on the USB drive by default), box art, controls, cheats, battery
  saves and states; `.zip` games too. No BIOS needed. The Mega Drive's six-button pad: A, B, C on Square,
  Cross, Circle, X, Y, Z on L1, Triangle, R1, MODE on the touchpad; buttons 1 and 2 on Cross and Circle
  for the others, PAUSE on Options (the Game Gear's START). Region, Reduce Sprite Flicker, Emulated
  CPU Clock and Frame Skip work for them (Region NTSC: 60 Hz, each game on its own region's console, a
  Japanese Master System game with its FM music); the Game Gear has the LCD grid (Scanlines), LCD
  Ghosting (the core's only kind) and the Sound Filter; the bezels are Off, Retro TV or your own for
  now. Mega-CD and 32X come later.
- A Mega Drive's 320-pixel picture isn't taken for a hi-res one (Resolution redraws it at its size);
  Pixel Perfect is each picture's own width (the Mega Drive's 320 or 256, the Game Gear's 10:9), and
  says so (no more "8:7" in its name).
- Console Order: the Sega consoles in their places By Company, By Power and By Year (a ready-made order
  chosen before keeps them in its places). A `.bin` that a `.cue` or `.gdi` beside it names (a CD's
  track, in any letters' case) isn't listed as a Mega Drive game; in a `.zip`, a `.md` or `.bin` (a
  readme's too) is the game only when no other ROM is there. The empty All Games page shows the folders
  in rows that fit the screen.
- PC test (local/herramientas/prueba-sega): test ROMs made for the four consoles and sverx's SMS Test
  Suite, 284 checks (pictures, sound, both pads, battery saves as the app loads and writes them, states,
  cheats up to the 150 codes the core has room for and ending with their game - a patch of
  scripts/build-gpgx-core.sh -, `.zip` games, PAL, NTSC with each game's own region).
- **The DS's touch screen** (asked for by the user): the stylus stays where the finger left it (it went
  back to the frame's middle), the right stick moves it too, and a **Tap button** presses it: the
  touchpad's click by default (Controls: Tap the Screen), held to drag. **DS Touch Screen** (Emulation):
  Click to Tap (the default), or Touch to Tap as before (the finger presses where it lands).
- **A second DS game no longer crashes the app** (New Super Mario Bros. after quitting Mario Kart DS
  mid-race, 00.55 on the PS4): melonDS reads its VRAM capture flags in the GPU's constructor before
  Reset() zeroes them, and the next console gets the last one's memory; zeroed in their declaration
  (scripts/build-nds-core.sh). Reproduced and verified on the PC with glibc's MALLOC_PERTURB_.

## 00.55 (test build)
- **The DS's One Screen** (DS Screens; asked for by players): only one screen, as big as the TV allows
  (4:3), through the GPU's filters. **R3 swaps the two screens** in every layout: Side by Side, Big
  Screen (was Big Top Screen: now the touch screen can be the big one), One Above the Other and One
  Screen (the touch screen alone). Before, R3 went between the chosen layout and Side by Side or Big
  Top Screen. The halves of the core's picture trade places as it comes in (two runs a row, no
  division per pixel), so the touchpad still touches the touch screen.
- **The Library's pages show their icon beside their name**: a console's full name with its picture
  (the tabs show short names), Favorites' star, Settings' gear.
- **The version under EMUSUNI** (Library) and under the emblem (XMB).
- A Quality level chosen in the public 00.19 is recognised (it showed as Custom with its old GPU
  Upscaler): the PlayStation is one of the consoles that came after the levels (Settings.cpp NEWER).
  Tested on the PC with a 00.19 settings.ini (Ultra) and with the user's own (unchanged).

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
- **Smooth Edges, Natural HD and Natural AI**: three new GPU filters, for Medium, High and Ultra (Low
  has none). A level chosen in 00.19 shows as Custom, with its old GPU Upscaler, until it is chosen
  again.
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
