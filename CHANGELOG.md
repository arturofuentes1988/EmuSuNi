# Changelog

All notable changes to EmuSuNi. Versions follow the PS4 package version (`XX.YY`).

**Public release v00.21 (2026-10-20)** is the 00.64 test build below, packaged as version 00.21 (the public versions count on from v00.20; the test builds 00.55 to 00.64 led to it). The 00.65 to 00.83 entries came after it: they are not in v00.21.

**Public release v00.22 (2026-10-28)** is the 00.83 test build below, packaged as version 00.22.

## 00.83 (test build)
- **Seven consoles out: the Sega CD (Mega-CD) and the 32X (added in 00.80), the PC Engine CD (TurboGrafx-CD), the Neo
  Geo Pocket, the Neo Geo Pocket Color, the WonderSwan and the WonderSwan Color (added in 00.82).** The owner chose to
  keep only the PC Engine of the consoles added last. 20 consoles (SYSTEM_COUNT 27 -> 20; the PC Engine's SystemId 21 ->
  19, the last one): every console of 00.79, the Saturn and the PC Engine.
  - Gone with them: their tabs, icons and names, their entries in Systems.cpp, the folders the app makes (Roms/megacd,
    32x, pcecd, ngp, ngpc, wswan, wswanc on the USB drive and in /data/EmuSuNi/Roms; the saves' and states' folders),
    the console boxes, the covers' and The Bezel Project's folders (SegaCD, Sega32X, PCE-CD, TG-CD, NGP, NGPC,
    WonderSwan, WonderSwanColor), the star ratings' LaunchBox platforms and their indexes (assets/xmb/ratings), the
    app's own bezels (scripts/make-bezels.py and the nine images: megacd-console, 32x-console, pcecd-console,
    ngp-console, ngpc-console, wswan-console(-tall), wswanc-console(-tall)), RetroAchievements (their hashes - the Sega
    CD's and the PC Engine CD's discs -, memory, marks, the history's consoles), cheats (and PicoDrive's code-length
    limit, the Neo Geo Pocket's "no cheat codes" row), the memory guard's and the start watchdog's entries, their
    controls (the WonderSwan's turned pad and its help), Settings -> WonderSwan Screen and the five consoles' Bezel rows,
    the Sega CD's BIOS lookup (bios_CD_U/E/J.bin, EMUSUNI_MCD_BIOS_*, its "This USA disc needs the BIOS ..." message)
    and the PC Engine CD's System Card lookup (syscard*.pce, EMUSUNI_PCE_BIOS, its message), the discs told apart by
    their first sector in a shared folder (SEGADISCSYSTEM, "PC Engine CD-ROM SYSTEM": the PlayStation's, the PSP's, the
    Dreamcast's and the Saturn's as in 00.79), the 32X's .bin by its header, the Sega CD's multi-disc and per-game
    backup RAM, the Neo Geo Pocket's flash saves in Saved Data, their Spanish lines (es.ini, es-emusuni.txt) and the
    README's rows, folders and BIOS (the Sega CD's and the PC Engine CD's).
  - **The PC Engine stays as in 00.82** (HuCards, .pce or a .zip of one, key pce, on Beetle PCE Fast): its tab, box,
    covers, ratings, bezels (its own and The Bezel Project's PCEngine and TG16), RetroAchievements, cheats, PC Engine
    Pad. Only the CD's part went: its core options (System Card, image cache, drive speed, CD volumes) are left at the
    core's defaults, as no CD game is given to it. The PC Engine Pad's 6 Buttons help no longer names CD games. A
    System Card file (syscard3.pce...) in Roms/pce is still no HuCard game (00.82 told players to put it there).
- **Cores dropped**: PicoDrive (the 32X), Beetle NeoPop (the Neo Geo Pockets) and Beetle Cygne (the WonderSwans) are
  no longer built or linked (scripts/build-pico-core.sh and assets/licenses/picodrive-license.txt gone; setup.sh pins
  only Beetle PCE Fast; scripts/build-beetle-cores.sh builds PCE Fast alone, without 00.82's System Card patch;
  CMakeLists.txt and build.sh: no PICO/NGP/WSWAN core objects, no no-32x/no-ngp/no-wswan; THIRD_PARTY_NOTICES.md
  without them). The sources stay in /root/src. Genesis Plus GX stays for the SG-1000, Master System, Game Gear and
  Genesis: scripts/build-gpgx-core.sh is v00.21's again, without 00.80's Mega-CD patches (an .m3u's absolute paths,
  the disc to start with, the BIOS's paths and message, the backup RAM as the battery save).
- **Players coming from 00.80 to 00.82**: the app starts as usual and leaves the removed consoles' things alone - a
  saved Console Order or Consoles Shown drops the keys it doesn't know, their settings sections, controls, covers,
  bezels, saves, states and Roms folders stay where they are (nothing deleted, nothing shown), a download of The Bezel
  Project's bezels left pending for them is skipped. A ratings copy of 00.80 to 00.82 (made for more consoles) is kept,
  not downloaded again: only a copy without one of this version's platforms is made again, and the removed consoles'
  games in it aren't read.
- The image: 127.7 MiB of file, 214.6 MiB of MemSiz, about 223 MiB free at display ready (00.82: 129.8 MiB, 217.5 MiB,
  about 221).
- PC tests (scratchpad remove-0083; ASan + UBSan): test0069 with the official build's 20 consoles (1337 checks), with
  the 3DS (839) and without (799), ui-0068's unit test (351, with and without the 3DS); the Library (76 checks: the 20
  consoles and the removed keys no console's; the folders made - none of the removed consoles', a player's old ones
  left -; the PC Engine's HuCards without the System Card; a folder shared by the disc consoles: the Sega CD's and the
  PC Engine CD's discs are the PlayStation's again, as before 00.80, a PC Engine .ccd the Saturn's, as before 00.82;
  the .ws, .wsc, .ngc, .32x files no console's, a 32X .bin the Genesis's; Consoles Shown with removed keys in its
  list); an upgrade test (27 checks: the owner's settings.ini of 00.81 with the removed consoles' settings, sections
  and games added - loaded, read, changed, saved: their sections kept, every console's values valid, the PC Engine's
  game keeps its own); the PC Engine's info box in English and Spanish; the PC Engine's options against its core;
  verify-0083-remove.sh and every older verify script that still applies on the built app and the tree.

## 00.82 (test build)
- **Six new consoles, the light ones the owner chose: the PC Engine (TurboGrafx-16) and the PC Engine CD
  (TurboGrafx-CD), the Neo Geo Pocket and the Neo Geo Pocket Color, the WonderSwan and the WonderSwan Color.** Each has
  its tab, icon and name (PCE, PCE CD, NGP, NGPC, WS, WSC; in the standard order after Sega's consoles - NEC's, SNK's,
  Bandai's - and by power and by year where they belong; a saved Console Order gets them at its end), its folders
  (Roms/pce, pcecd, ngp, ngpc, wswan, wswanc on the USB drive and in /data/EmuSuNi/Roms, made by the app), the
  console's box on its tab (what it plays, the files, the System Card for the CD), covers (libretro-thumbnails' "NEC - PC
  Engine - TurboGrafx 16", "NEC - PC Engine CD - TurboGrafx-CD", "SNK - Neo Geo Pocket (Color)", "Bandai - WonderSwan
  (Color)"), star ratings (LaunchBox's "NEC TurboGrafx-16", "NEC TurboGrafx-CD", "SNK Neo Geo Pocket (Color)",
  "WonderSwan (Color)": a mono game in the colour model's folder matched with both, as the Game Boys'), The Bezel
  Project's game bezels (PC Engine and TG16, PCE-CD and TG-CD, NGP and NGPC, WonderSwan and WonderSwan Color),
  RetroAchievements (PC Engine, PC Engine CD - its discs hashed as rcheevos does -, Neo Geo Pocket, WonderSwan: the
  hashes, the memory, the marks, the history), cheats (not the Neo Geo Pocket's: its core takes none), Clean Up, saves
  and states folders of their own, the memory guard and the start's watchdog. As the Game Boy and Color, a mono game in
  the colour model's own folder is listed there.
- Defaults as the other 2D consoles: Quality Low (the Quality levels as for the pixel consoles: Smooth Edges, Pixel
  Art HD, AI), Aspect Ratio Auto - 4:3 for the PC Engine, the handheld's own shape for the others (160x152, 224x144) -,
  the console's own bezel: eight new ones in scripts/make-bezels.py's style, no logos or names (the PC Engine's white
  with its HuCard and orange line, the CD unit's grey with its disc lid, the Neo Geo Pocket in carbon black and the
  Color in platinum blue around a 6x window, the WonderSwan in pearl white and the Color in wine red around a 6x window,
  each also standing upright around a 4x window for the vertical games). The handhelds' screens get the LCD grid
  (Scanlines) as the Game Boys'.
- **The PC Engine and PC Engine CD** run on Beetle PCE Fast (libretro's Mednafen pce_fast, 4eee99c). HuCards: .pce (a
  .zip of one too). CD games: .cue (with its .bin tracks), .chd, .ccd (with its .img), .m3u - not zipped; in a folder
  shared with the PlayStation, the Sega CD and the Saturn a disc is the PC Engine CD's by its data track's boot sector
  ("PC Engine CD-ROM SYSTEM"). **CD games need the player's System Card** (none comes with the app): syscard3.pce (also
  syscard3u, syscard2(u), syscard1, or a "[BIOS] ...card....pce" in system/pce), looked for in
  /data/EmuSuNi/system/pce, /data/EmuSuNi/system, beside the game, in the folder above it and in the HuCards' folder
  beside it (Roms/pce) (any letters' case);
  without it the game doesn't start and the panel says "PC Engine CD games need the System Card BIOS: syscard3.pce
  (the Super System Card 3.0). Put it beside the game, or in /data/EmuSuNi/system/pce". The pad: II and I on Cross and
  Circle, SELECT on the touchpad, RUN on Options; Settings -> PC Engine Pad: 6 Buttons (Avenue Pad 6) puts III-VI on
  Square, Triangle, L1, R1. Reduce Sprite Flicker and Frame Skip apply. No SuperGrafx: this core has none of its
  hardware (Beetle SuperGrafx would be another core).
- **The Neo Geo Pocket and Color** run on Beetle NeoPop (a50d5ac), its BIOS emulated (no file). .ngp, .ngc. A and B on
  Cross and Circle, OPTION on Options, the stick on the D-pad and left stick. Its saves are the cartridge's flash,
  which the core writes as saves/ngp(c)/<game>.flash when the game ends (Quit Game), listed in Saved Data.
- **The WonderSwan and Color** run on Beetle Cygne (4b01295). .ws (.pc2), .wsc. **Vertical games**: the cartridge's
  header says a game is played upright (as Mednafen reads it) and the core turns the picture (the app refuses
  SET_ROTATION, so the picture comes upright, 144x224, with its own bezel) and the pad with it: the D-pad works the Y
  buttons, Square, Triangle, Circle and Cross the X buttons, L1 and R1 A and B. Settings -> WonderSwan Screen: Auto,
  Horizontal, Upright. Held normally: the X buttons on the D-pad, A and B on Circle and Cross, the Y buttons on R2 (up),
  R1 (right), L2 (down), L1 (left), START on Options, Fast Forward on the touchpad. 60 frames a second for the TV (the
  console's 75 shown as 60); Sound Filter's low-pass.
- The three cores: scripts/build-beetle-cores.sh (built for the PS4 as the others, their APIs renamed
  beetle_pce_retro_*, beetle_ngp_retro_*, beetle_wswan_retro_*; scripts/setup.sh pins them; GPL-2.0, in
  THIRD_PARTY_NOTICES.md). No threads, no C++ exceptions, no static array of 1 MiB or more (PCE Fast's 1 MiB memory
  space and Cygne's 1 MiB tile table on the heap). Changes: the System Card by the path the app gives
  (EMUSUNI_PCE_BIOS); frames the app skips not drawn (all three); NeoPop's RAM cleared at each game's start (upstream
  kept the previous game's) and a state's sound at its own level; no option translations.
- The image: 129.8 MiB of file, 217.5 MiB of MemSiz, about 221 MiB free at display ready (00.81: 222). Spanish in es.ini
  and es-emusuni.txt. README / README.es: the console table, folders and BIOS (with the Sega CD and the 32X, missing
  there since 00.80).
- PC tests (homebrew of our own: ca65 HuCards and a stand-in System Card, a nasm WonderSwan program, a hand-assembled
  Neo Geo Pocket Color one; a CD of our own as .cue/.bin, .ccd, .chd, .m3u; local/herramientas/prueba-beetle, with
  ASan): pictures, sound, the pads, states, cheats, frames left out, a second game in the same process, the
  WonderSwan upright and horizontal, the CD path with and without the System Card - all OK; on this PC 0.12 to 0.25 ms
  a frame (PS4 estimate about 1-2 ms of 16.7). The app's core options checked against the cores' declarations; the
  Library (the PC Engine CD's discs in its own and in shared folders, the handhelds' families) and the unit tests with
  27 consoles (test0069 1757 checks, unit 351) and the info box's layout in English and Spanish all OK.

## 00.81 (test build)
- **Super Nintendo covers in covers/snes work** (GitHub #25 and a YouTube comment: "no detecta mis covers ... la ruta
  es /data/EmuSuNi/covers/snes"). The Super Nintendo's game keys have no prefix, so its covers were read only right in
  covers/ and covers/snes/ never (every other console has covers/<key>/); the app downloaded them again into covers/.
  Now covers/snes/ is the Super Nintendo's folder like any console's (new covers go there); covers/ itself is still
  read, its picture losing to covers/snes/'s of the same name (so a player's own in covers/snes/ wins over the copy
  00.80 downloaded beside it). The Super Nintendo's covers right in covers/ are moved into covers/snes/ once, on the
  PS4 and in a USB drive's EmuSuNi/covers: each picture renamed (a stop half-way leaves each in one of the two folders,
  both read, and the next start goes on), then covers/snes/.moved-from-covers; a name covers/snes/ has already keeps
  its pictures in covers/ (they lose to it), of two of one name only the shown one moves. A console's folder in another
  case (covers/SNES on the PS4's case-sensitive storage) is found too, and new covers go into it. Every other console
  checked: its covers folder is its key (nes, n64, gb, gbc, gba, ps1, nds, 3ds, psp, dc, naomi, atomiswave, sg1000,
  sms, gg, md, megacd, 32x, saturn, arcade). src/CoverFiles.h (the menu and Download Covers look the same way now).
- **Your own covers as JPEG, and any size** (a player's Ben 10 cover "bigger than the official one" didn't show: the
  real cause was that only true PNG files were read - a JPG, even named .png, wasn't). A cover is a PNG or a JPEG
  (.png, .jpg, .jpeg, any case), known by its first bytes, whatever its name says; JPEGs by PacBrew's libjpeg-turbo
  2.1.2 (baseline, progressive, grayscale, CMYK). A big picture is made smaller while it is read: a row at a time into
  the box filter (the same pixels as before, PC-checked), a big JPEG first by libjpeg's own 1/2 to 1/8 scaling, so a
  4000 x 4000 PNG takes ~1 MB instead of 64 MB, and up to 16384 pixels a side are read (8192 before); a progressive
  JPEG or an interlaced PNG, read whole, is refused past 48 MiB (a 4000 x 4000 progressive JPEG fits). Two pictures of
  one name in one folder: the newest. Find Cover over a JPEG of the player's own saves a PNG beside it (newer: it wins).
- **Why a picture of yours isn't used**: a cover file that can't be read is logged ("covers: <file> isn't used:
  <why> (<libpng's or libjpeg's words>)"), stops counting as the game's cover (the game shows in Games Without Cover,
  its other names' pictures are looked for) and Games Without Cover says it under the list: "Your picture isn't used:
  <file>" and why - a WebP, GIF, BMP, TIFF, HEIC/AVIF picture or a web page saved as .png, an empty, damaged or cut
  file, too big. Download Covers never writes over it. Settings -> Covers Saved On's text, README (all the consoles'
  folders) and the Spanish lines updated.
- PC tests (scratchpad covers0081): every PNG kind at 5 sizes pixel-identical to 00.80's loader; JPEGs identical to
  PIL's decoding with the same box filter; peak memory 4.6 MB for 4000 and 9000 pixel PNGs and JPEGs; 105 checks of
  the folders (the bug, the old place, the move cut half-way, two places, other cases, all 21 consoles) with ASan.
- **The emulators' own lists of games and their special settings** (the owner: "revisa en los proyectos originales de
  cada emulador si existe una base de datos o un registro de configuraciones especiales para algunos juegos ... y si es
  posible aplícalos"). Every core's list was checked against its project (report: scratchpad gamedb-0081\informe.md):
  they were already at work - Mupen64Plus-Next's mupen64plus.ini (built in, written to system/Mupen64plus/ and read at
  each N64 game's start: its save type, CPU timing, the paks it takes, fix patches; all 97 of the owner's N64 cartridges
  are in it, and on the PC Paper Mario plays with the list's CountPerOp 1 exactly as with it forced), PPSSPP's
  compat.ini (in the package), SwanStation's 391 PlayStation games, Flycast's per-game settings (the RTT copy, the
  cable, the BIOS, the broadcast, depth scales), Azahar's hack list, Snes9x's, FCEUmm's, mGBA's, Genesis Plus GX's,
  PicoDrive's, melonDS's and FinalBurn Neo's (its hiscore.dat in the package). None was switched off by the app's
  options: each core's per-game setting wins over them (but N64 CPU Timing Light, when chosen, and its Auto for
  GoldenEye). YabaSanshiro (the Saturn) has none. What's new:
  - PSP: upstream PPSSPP's newer compat.ini entries whose fix is a flag this PPSSPP already reads, without a code change
    (16 games: Metal Gear Solid: Portable Ops and Echochrome's other regions draw their lines centred, Tales of
    Phantasia FVE's framebuffers, Megpoid the Music's UMD delays, the PopCap games and a GTA romhack); the ones that
    came with code changes or the software renderer are left out (scripts/build-psp-core.sh says which and why).
  - N64: Wipeout 64 (USA) at the core's default CPU timing, as mupen64plus-core's newer list has it (with CountPerOp 1
    each press counted more than once; the European version keeps 1). The core's copy of the list is otherwise up to
    date; its two deliberate differences from upstream (Quake II and Goldfinger without CountPerOp 1, for speed) stay.
  - The log says what each list gives the game starting: "gamedb: n64 Paper Mario (E) (M4) [!]: CountPerOp = 1
    (mupen64plus.ini)", "gamedb: psp ULUS10202: CenteredLines = true (compat.ini)", and the app's own per-game choices
    the same way ("gamedb: n64 Cruis'n USA: N64 Graphics Sync = Accurate (EmuSuNi's list: ...)", GoldenEye's CPU
    timing). SwanStation ("Game settings found for ...") and Flycast ("Forcing ...") already log theirs.
  - PC tests (scratchpad gamedb-0081\test): the N64 list read by the header's CRCs finds, for each of the owner's 97
    cartridges, the same entry as the core's MD5 lookup; the log lines; RefMD5; no correction for any of the owner's
    cartridges (their settings unchanged).
- **PlayStation: a .bin without its .cue shows and plays** (two players on YouTube: "el emulador de ps1 no acepta los
  juegos formato .BIN", "los pasé a formato ISO y no funcionan"). The PlayStation's list took .cue, .chd, .pbp, .iso,
  .img... but never a .bin, so a game whose .cue was lost didn't show. Now a .bin is a PlayStation game when no .cue
  beside it names it and it holds a CD's data track (an ISO 9660 volume in its sector 16); in a folder another console
  shares, only a PlayStation disc's ("PLAYSTATION" its volume's system), so a Genesis cartridge stays the Genesis's
  and a BIOS file (scph5501.bin) or a CD's audio track is no game. The core reads it as one data track. A disc's
  tracks named as Redump names them - "Name (Track 1).bin", "Name (Track 2).bin"... - without their .cue are one game,
  "Name" (the same key, saves and cover as with the .cue), started with a .cue the app makes for them in
  /data/EmuSuNi/cache/cue/ps1 (the first track data, the others CD audio as Redump's; the disc's .sbi copied beside
  it); Delete Game deletes every track. Lone discs side by side ("(Disc 1).bin", "(Disc 2).bin") are one game, as
  before for .cue files. A game with its .cue as before (its tracks are no games of their own). The Genesis and the
  32X no longer take a CD's data .bin in a folder they share with the PlayStation. The PlayStation's info box: ".cue
  (with its .bin tracks) or a .bin alone, .chd, .pbp, .iso, .m3u; not in a .zip".
- **PlayStation ISOs of 2048-byte sectors play** (a converter's "BIN to ISO"; a .bin renamed .iso, 2352-byte sectors,
  already worked). SwanStation read every .bin, .img and .iso as 2352-byte sectors, so such a game never started. Now
  the core finds the sector size in the file (a raw image begins with a sector's sync, a cooked one has its volume
  descriptor at 32768) and makes each 2048-byte sector a whole Mode 2 Form 1 one as it reads it (also a .cue's
  MODE1/2048 or MODE2/2048 track; scripts/build-ps1-core.sh). Such an ISO has lost the disc's sound and film sectors
  (XA, STR): its game plays without them - its .bin with the .cue, or a .chd, is the whole disc.
- **Jackie Chan Stuntmaster at full speed** (two players: "va muy lento"; GitHub #22). SwanStation's game database
  forced its interpreter for the game (DuckStation's of 2020: "must use interpreter to avoid crash"), which needs
  2.2-2.4 times the recompiler's CPU time (PC, Tekken 3 and Gran Turismo 2: 3.2-3.4 ms a frame at the 95th percentile
  against 1.4-1.7; the PS4 takes about 7 times as long: some 23 ms of a 16.7 ms frame). The game raises a software
  interrupt by writing the status register's mask with the interrupt already pending and needs it taken at once; the
  recompiler took it only at its block's end. DuckStation's fix (commit 312790c9, 2023, which dropped the interpreter
  for the game too) is in the core now: the block ends right after that write, and the interpreter's uncached blocks
  too; the database's line for the game is gone. PC test ROM: the interrupt taken before the next instruction on the
  interpreter and now on the recompiler (6 instructions later before); Tekken 3's and Gran Turismo 2's pictures
  identical to 00.80's. Not tried with the game itself (not on the owner's drive).
- **PlayStation LibCrypt discs without their .sbi start** (European discs: Parasite Eve II, MediEvil, Spyro 3, Dino
  Crisis...; a player's "no agarra, dice que no puede reproducir" with Parasite Eve 2). SwanStation refused them - its
  "are you sure?" needs a dialog libretro hasn't - so the app said only that the emulator couldn't load the game. Now
  they start, with a toast: "Protected disc (LibCrypt): put its .sbi file beside it, or the game may stop later."
  (Redump's .sbi, named as the disc's file; its protection's checks can stop the game without it).
- **A PlayStation disc image that can't be opened says so** (a .cue whose .bin was renamed, a damaged file): SwanStation
  then started the console without a disc - a black screen. Now the game doesn't start and the menu says "The disc
  image couldn't be opened:" and why: "This file its .cue names isn't beside it:" with the file's name, or the core's
  reason.
- **Toy Story 2's black screen (GitHub #22): not reproduced** - the game isn't on the owner's drive. DuckStation's
  database has nothing for it ("No Issues" with its default settings), nor SwanStation's, nor the LibCrypt list; what
  differs here: without a BIOS file the built-in OpenBIOS starts the games (it starts fewer than Sony's: scph5501.bin
  in /data/EmuSuNi/system), and a disc image that couldn't be opened (above). The log now says at each PlayStation
  start "EmuSuNi: disc in (or NONE), code 'SLUS-00893', BIOS OpenBIOS (built in: no BIOS file found) or from a BIOS
  file, CPU Recompiler": the next player's log tells which.
- PC tests of the PlayStation's (scratchpad ps1-0081): the core before and after these changes (ps1test, SwanStation
  for the PC): a test ROM for the interrupt (above); homebrew discs (Bench3D) as a lone .bin, a 2048-byte ISO (didn't
  start before; now the same pictures as the .bin), a raw .bin renamed .iso, a LibCrypt game code without .sbi (refused
  before, starts now), Jackie Chan's code (interpreter before, recompiler now), a cue-less track set through the app's
  .cue, a .cue naming a missing .bin (the core's "Failed to load CD image", then a clean end, twice in one process);
  Tekken 3 and Gran Turismo 2 identical. The Library (libtest, ASan): a lone .bin, cooked and raw .iso, a .cue's tracks
  once, a cue-less track set as one game named as the disc, a BIOS file left out, lone discs grouped, a .cue naming its
  .BIN in other letters, LoneTracks, Delete Game's files, the folder browser's count; in a folder shared with the
  Genesis and the 32X each console's own; the Genesis's own folder as before.
- **NAOMI and Atomiswave: the log says what closes the app** (players on v00.21, the 00.64 test build: The King of
  Fighters XI closes EmuSuNi when its first fight starts, Metal Slug 6 runs too fast, and the NAOMI's Crazy Taxi and
  House of the Dead 2 are slow; none of these four games was available to try).
  - **Every C++ exception the Flycast core throws is now logged first.** On the PS4 such an exception never reaches
    its catch, so the app ends at once with no crash report and nothing in the log. The line gives the exception's
    type, its message, where the SH4 was in the game's code, and the game, for example:
    `EmuSuNi: C++ exception 1 thrown in the core: 18NaomiCartException: Cannot find ax2206m01.ic16 (SH4 pc a0000000,
    game ...); on the PS4 its catch is never reached: the app ends here`. (scripts/build-dc-core.sh routes the core's
    throws through a logger, oslib/emusuni_throw.cpp, with ld.lld -r --wrap.) On the PC, KOF NeoWave with a file
    taken out of its zip, started without the app's check, logs that line and stops as before.
  - **One such exception is gone.** A display list with a parameter the graphics chip doesn't know (types 3 and 6)
    made Flycast throw, and its own catch then left out the rest of the list. The rest of the list is now left out
    without the exception, so the picture is the same. The first one is logged (`a display list with a parameter the
    TA doesn't know (pcw ...)`) and the count goes in the 10 s line.
  - **The app's heap running out ended the app the same way** (an exception thrown by a C++ allocation, on every
    console). It now logs `memory: the heap has no N bytes for a C++ allocation (operator new): the app ends here`
    first (port/ps4_new.cpp).
  - **Two more lines:** the board's video timing whenever it changes, which sets the frames a second the app runs it
    at (`EmuSuNi: video timing: 525 lines of 858 pixel clocks at 27 MHz: 59.945 frames a second`), and where a big
    block of memory went (`EmuSuNi: 320 MiB of direct memory at ...`; KOF XI's cartridge is 320 MiB).
  - **Metal Slug 6:** in v00.21, a game that draws a picture every 2 of the console's frames (30 FPS), or any game
    with Frame Skip, ran at up to twice its speed, because each run of Flycast was one picture. 00.68 paces Flycast
    by the console's frames (the log's `pacing: Flycast's pictures are 2 of the console's frames apart`), and 00.69
    holds the emulation to the app's pace while nothing is drawn. 00.69's wait-loop skip can't speed a game up: it
    only lets the time of a loop that would come round unchanged go by to the next event, and the console's frames
    still come at their times.
  - **Crazy Taxi and House of the Dead 2:** the speed work of 00.66 to 00.69 isn't in v00.21 yet (00.69 alone:
    Crazy Taxi's Dreamcast race -33% of the emulation thread on the PC).
  - **No per-game settings added:** Flycast has none for these four sets (its master branch checked), and nothing
    here points to one.
  - **PC checks** (the owner's Dolphin Blue and KOF NeoWave from his stick, the renderer on the emulation's thread):
    pictures identical to 00.80's core (28 of 28 each) and the whole sound identical. With the app's VGA output both
    run at 59.945 frames a second, nearly every picture 1 console frame (Dolphin Blue 2394 of 2400, KOF NeoWave 2361),
    with no exception and no unknown TA parameter.
- **3DS: the graphics errors of Pokémon X and Y, Majora's Mask 3D, Metroid: Samus Returns and Castlevania: Mirror of
  Fate** (five players: GitHub #23, #29, #31 and the 3DS video's comments). Two causes in upstream's reports, each a
  setting per game now (Settings -> Emulation, the 3DS's pages and its in-game menu; both at the next start):
  - **Accurate Shader Multiplication** ("Sombreado exacto"): Azahar's accurate multiplication in the GPU's shaders, Off
    in every game until 00.80 (upstream's default is On; its Android build has it Off, and there Pokémon X draws
    Froakie white until it is turned on: azahar #2583, #1445). Auto, the default: On for Pokémon X and Y and the Pokémon
    games on their engine after them (Omega Ruby, Alpha Sapphire, Sun, Moon, Ultra Sun, Ultra Moon), Majora's Mask 3D
    (upstream forces it for Ocarina of Time 3D, the same engine), Metroid: Samus Returns and Castlevania: Mirror of
    Fate, by every region's title ID; Off in the others. On and Off for any game. PC, Super Mario 3D Land's World 1-1
    from a state with it On: the same pictures, the emulation thread 2.20 -> 2.54 ms a frame (most of it waiting for
    the PC's software GPU; the game's own code the same), the pipelines it built 59% longer to compile (54 -> 86 ms on
    the emulation thread, 163 -> 255 ms on the workers): on the PS4 the cost is the GPU's (its shaders), 8-15% busy at
    1x.
  - **3DS Right Eye**: the right eye's picture, left out in every game since 00.63 (the PS4 shows no 3D), breaks a
    few games: Majora's Mask 3D's intro stays black and its blur effects freeze the screen (azahar #791), Samus
    Returns' bottom screen is drawn out of place (#740), Kingdom Hearts 3D's and Pokémon Super Mystery Dungeon's bottom
    screen stays black (#1005). Auto, the default: drawn in Majora's Mask 3D, Kingdom Hearts 3D, Pokémon Super Mystery
    Dungeon, Fire Emblem Warriors and Minecraft: New Nintendo 3DS Edition; left out in the others. Not Drawn and Drawn
    for any game; the file /data/EmuSuNi/3ds-right-eye still draws it in every game. PC, the same World 1-1: the right
    eye drawn, 133,726 draws in 600 frames against 70,613, the emulation thread 3.0 against 2.2 ms a frame.
  - **Upstream's fix of the right eye's guess** (azahar PR #2467, merged after Azahar 2126.2; scripts/build-3ds-core.sh):
    a command list that draws nothing is no longer taken for a repeat of the top screen and dropped (Majora's Mask 3D's
    black intro, Samus Returns' bottom screen, the Ace Attorney games' bottom screen). As merged it kept Super Mario 3D
    Land from leaving its right eye out at all (133,726 draws, every 00.63 saving gone): EmuSuNi takes a list its scan
    cut off (256 commands) as drawing, as before - 70,613 draws again, the same pictures as 00.80's core.
  - The game's title ID is read from its file (.3ds/.cci, .cxi) at its start, and the log says it and what Auto made of
    both: `3ds: title ID 0004000000125600; Accurate Shader Multiplication on (auto), right eye drawn (3DS Right Eye
    auto)`, then the core's `EmuSuNi: accurate multiplication on for 0004000000125600 (the setting on)` and
    `EmuSuNi: right eye: drawn ...` (Ocarina of Time 3D and the Mario & Luigi games: "forced on by the hack list").
  - Not changed: the hardware shaders (Off sends every vertex to the CPU: the "it was fixed by turning the hardware
    shader off" a player remembers from other emulators is what accurate multiplication fixes without that cost),
    OpenGL (#29: the PS4 has Vulkan only), async shaders (a draw skipped while its pipeline compiles shows a few frames
    late, once, not a lasting error). The four games themselves weren't tried: none of them is among the owner's
    decrypted dumps.
  - PC tests (scratchpad 3ds-0081/test0081.sh): the two settings and their Spanish lines, the real Build3dsVariables over
    3 x 3 choices x title IDs (301 configurations: Auto right for every one; every option the core declares, 33 now,
    answered), the title ID read from made-up .3ds, .cxi, .3dsx, broken and missing files; 00.69's, 00.62's, 00.68's and
    00.71's tests still passing.
- **PSP: God of War no longer turns twice as fast, and no game does with Frame Skip on** (GitHub #19: "suddenly the
  speed of the game changed it got really faster", then "most of games runs with high speed"). Frame Skip on a PSP game
  (1, 2 or Auto) is PPSSPP's own, and PPSSPP turns its frame duplication off while it is on: a run of the core then
  lasts until the PSP's next picture, while the app ran the core once per TV refresh. So with any Frame Skip:
  - a game drawing at 30 FPS (most 3D PSP games) ran 2 of the PSP's vblanks a run: twice its speed wherever the PS4
    kept up, its sound partly left out;
  - God of War draws at 60 until PSP Frame Rate Auto turns PPSSPP's 30 FPS hack on (about 2 s too slow for 60, after its
    first 15 s): from that moment it ran at twice its speed - the jump in the report;
  - Frame Skip 1 and 2 kept the speed only because PPSSPP slept inside each run until the skipped picture's time: runs
    of 33 to 100 ms, which the app took for a heavy core (one frame a pass, 10 to 30 pictures a second).
  Now the core counts its vblanks and gives the count at the end of each run (scripts/build-psp-core.sh: a private
  environment call), and the pacing pays for the vblanks a run covered, as it does for Flycast's pictures
  (src/Emulator.cpp: PspFrames - a 30 FPS game with Frame Skip runs every other refresh, each picture shown twice);
  PPSSPP no longer sleeps inside a run. Every picture is still one the PSP drew whole (a run ends at a picture, as
  upstream's). With Frame Skip off nothing changes: every run was and is one vblank (the owner's God of War log at its
  switch to 30 FPS: sound at 44.1 kHz, 99% speed). The log notes a new spacing: `pacing: the PSP's pictures are 2
  vblanks apart now (was 1): Frame Skip, or a game at 30 FPS with it`.
  - Nothing stays set from one game to the next: PSP Frame Rate Auto's 30 FPS is the app's for the game it found it
    in (reset before the next game's options are built), and the core reads its options again at each game's load. The
    likely "after God of War most games run fast" is Frame Skip itself: a game's Settings page sets it for This Console
    by default (00.58), so turning it on in one PSP game turns it on for all of them.
  - A PSP game's start logs what its speed and stick depend on: `psp: settings from This Console: Frame Skip auto, PSP
    Frame Rate auto, Emulation Speed 100%, stick dead zone 0.15, sensitivity 1.00`.
  - PC (scratchpad psp-0081: the core for the PC with the app's options, paced as the app at 60 Hz, each run's PSP time
    measured by its sound and by the core's count): a program drawing at 30 FPS (pspautotests' NeHe 10) with Frame Skip
    Auto 200% -> 100% of real time; Frame Skip 1 and 2 100% both, but PPSSPP's time inside a run 66.6 / 100.0 ms ->
    0.18 / 0.22 ms; a 60 FPS one (cube) 100% in every case, its pictures with Frame Skip 1 each a new one, 2 vblanks
    apart. All eight cases at 100% (+-1%) with the new core, its count equal to the sound's vblanks.
- **PSP stick checked (Crash: Mind over Mutant spins when the stick turns a little, GitHub #19)**: no fault found,
  nothing changed. The DualShock 4's byte goes to the PSP's byte as PPSSPP's own controls do (the same formula as its
  ControlMapper: the dead zone and the sensitivity on the stick's longest axis, the direction kept): the centre stays
  128, a full push 0 / 255, up is up. PPSSPP's dead zone is square, so on the DualShock 4's round stick it is 15% on the
  axes and 21% on the diagonals, and right at its edge the PSP's byte steps can bend a tiny push's direction toward an
  axis (pushes the PSP sees as under 5%). Crash: Mind over Mutant starts its spin by rotating the left stick (the game's
  own control), so a turn swept along the stick's edge is that gesture; a game running below full speed sees the same
  thumb movement as a quicker rotation, and with Frame Skip on a 30 FPS game ran at twice its speed (fixed above).
  Turning by moving the stick through its centre avoids it. PSP Stick Dead Zone and Sensitivity (00.68) stay as they
  were (scratchpad psp-0081\stickpath.cpp: every byte of the path checked).

## 00.80 (test build)
- **RetroAchievements' filter on the games** (the owner: "agregar un filtro en el carrusel de juegos, que filtre por
  los que tienen RA"). The touchpad, on a game list (a console's tab, All Games, Favorites; Library or XMB), goes
  round three steps: every game; "With achievements" (the games whose file RetroAchievements has achievements for -
  the "RA" mark); "Achievements in progress" (begun, not all unlocked). Each list keeps its selected game when it's in
  the new list. The title's line says it, with the count: "With achievements: 438 of 793  -  A-Z" (the XMB: a toast);
  while the background check goes on, the files not checked yet stay out and the line says how far it is ("checking
  1200 of 3585..."), the list filling as they're found (made again at most once a second, never under an open panel or
  the keyboard). A list it empties says so ("No games with achievements here", "Touchpad shows every game again"). The
  choice is kept in xmb.ini (ra_filter). It needs Settings -> RetroAchievements -> Marks on the Games (on by default;
  off, the touchpad says so and every game shows). The hints line shows "Touchpad: RA Filter" (the Library leaves it
  out while a search shows its Clear Search: no room); What is RetroAchievements? has a subject about it.
- **Triangle -> Achievements on every game of a console RetroAchievements has** (the owner asked why some games had
  the entry and others not: it showed only once the background check had reached the file, and never with the marks
  off or for a game RetroAchievements knows without achievements). Now every game of those consoles has it, the marks
  on or off; the 3DS (none yet) has no entry. A file not checked yet is checked when the page opens ("Checking this
  file: ..."): its hash made then (kept for the marks too), the console's list read from the PS4 or asked for, its
  state - with achievements: the game's page; RetroAchievements knows the game but has none yet: said so ("...knows
  this game, "Name", but it has no achievements for it yet"); not known or another version: the files it knows
  (00.79); the console's list not there (no connection): try again later.
- PC tests: 25 scenarios, 147 checks pass - new: a file checked on demand with the marks off (hashed now, the list
  asked for once, known / known without achievements / not known).
- **One keyboard for every text** (the owner: "normalizar la escritura de teclado en la app, ya que hay un teclado en
  que elijo la letra con X y otro teclado en que elijo la letra con círculo"). Rename, Find Cover's Look For and
  RetroAchievements' login were typed on the PS4's own keyboard (libSceImeDialog), which chooses a key with the
  console's Enter button - Circle on some consoles - and can't be told otherwise (its settings have no such field: it
  follows the system's). Now every text is typed on the app's keyboard, the search's grown (src/TextKeyboard.h), with
  the menu's buttons everywhere: Cross types the selected key, Circle goes back (Cancel; the search: Close), Square
  deletes, Triangle types a space, R2 / Options end it (OK; the search: Done).
  - A text asked for: its title and length ("8 / 128"), the field, a line under it saying what it's for; under the
    characters Shift (L2: the next letter, every letter, off - a name gets a capital at each word's start by itself, a
    user name or a password none), the page key (R1 / L1: the letters with the ñ, the signs - all of ASCII's, for
    passwords -, the accents, each with its capitals), Space, Delete, then Clear All, Cancel and OK. A password shows
    as dots, with Show / Hide. At most 100 characters for a name, 64 for the user name, 128 for the password (an accent
    is one); OK needs something typed for the login (a name may be left empty: Rename's own name again). What's behind
    is dimmed; the hints line shows the buttons. The search keeps its keys and buttons (L1 / R1 the games, L2 Clear
    All). What was typed is wiped from the keyboard once OK or Cancel takes it.
  - Gone: src/SystemKeyboard.cpp / .h, libSceImeDialog and libSceCommonDialog, the "The PS4's keyboard didn't open"
    toast. RetroAchievements' help and What is RetroAchievements? say "the keyboard". The log says the console's Enter
    button at the start ("enter button: Circle"). Lost with the PS4's keyboard: other alphabets (Japanese, Chinese,
    Korean, Russian) in Rename - the app's keyboard has Latin letters with accents.
  - PC tests: ui-0068's unit test has the keyboard's 225 checks (each mode's keys and buttons, every ASCII sign typed
    in a password, accents, Shift and capitals, lengths, OK / Cancel, the moves through the keys).
- **The Bezel Project's game bezels** (the owner: "agregar esto" - github.com/thebezelproject/BezelProject). The Bezel
  Project draws a bezel for each game (its box art around a TV's or a handheld's screen) for RetroArch: one GitHub
  project per console (bezelproject-SNES, -NES, -PSX, -MAME...), the PNGs named after the games' No-Intro names (the
  arcade's after the MAME sets). It publishes no licence, so no bezel of theirs is in the app, its packages or this
  repository: the app downloads, when the player asks, the bezels of the player's own games from the project's GitHub,
  as its own RetroPie script and Windows utility do (src/BezelProject.cpp).
  - **Settings -> Game Bezels** (after Covers Saved On): Download for All Consoles (Stop the Download while it runs),
    each console with games (Cross: its bezels only; the row says how many it has, "Looking...", "12 / 612", "None for
    these games" or "Failed" and why), Delete Downloaded Bezels. The Settings row says how it goes ("Downloading
    bezels: 31 / 151 MB", "612 bezels downloaded").
  - Each project's list: one request to GitHub's API (git/trees/master:<its folder>, gzip: the whole folder - the
    recursive listing is cut short for the PlayStation's and the arcade's), kept in data_dir/cache/bezels/<project>.txt
    and asked for again a day later with its ETag (304: nothing downloaded again). GitHub allows 60 requests an hour
    without an account; every console's lists are 23 requests; at its limit the page says until when ("again after
    18:40"). A console's games are matched with all its projects' lists or none (the Super Nintendo's SNES and SFC, the
    NES's NES, Famicom and FDS, the Game Boys' GB and GBC).
  - Matched as the covers are (CoverDownload::Match, the new function with Download Covers' own passes: the names'
    words, their parts, close spellings, a list's numbering in front left out, the game's region preferred - never a
    closest guess); an Arcade, NAOMI or Atomiswave game by its set's name, then its parent's.
  - The files from raw.githubusercontent.com, one at a time, through <file>.part: a cut download goes on from its byte;
    the free space is checked first (the bezels plus 64 MB, "Not enough free space: N MB needed, M MB free"); nothing
    is asked for while a game runs (the file coming stops, its part kept, and goes on after the game); a download the
    app's closing or the connection cut goes on in the background at the next start (data_dir/cache/bezels/
    pending.txt; not after the player's Stop); GitHub asking to slow down (429) is waited out (its Retry-After, else a
    minute). Each file is checked against the list's Git hash (logged when it
    differs) and recorded in data_dir/bezels/<console>/bezelproject.txt. They go to data_dir/bezels/<console>/<the game's
    file name>.png, where Your Own looks too; a bezel of the player's own there is never written over, and Delete
    Downloaded Bezels deletes only the recorded files, as they were written (and the parts).
  - One small thread (256 KiB, made per download; libthr reuses one of that size), the lists' copies a few hundred KB
    each; +0.2 MiB of image.
- **Video -> Bezel: Per Game (The Bezel Project)** on every console The Bezel Project has bezels for (all but the PSP:
  it has only one for the console): the game's own once downloaded, else the console's own (the Arcade's: the Arcade
  Cabinet). The default stays each console's own bezel (00.69, the owner's choice).
  - **Never a pixel of it over the picture**: The Bezel Project's TV screens have rounded, bulging edges (the 4:3 window
    1432x1073, its corners 23 000 pixels of frame), made for RetroArch drawing them over the picture's corners. A bezel
    of the data folder (Per Game, Your Own) now places the picture in the biggest rectangle of its shape that is all
    see-through (src/BezelProject.cpp FindWindow, Emulator.cpp LoadBezel; found again when the picture's shape
    changes): a TV's 4:3 picture 1373x1030 (95% of 1440x1080), the Game Boys' 10:9 and the Game Boy Advance's 3:2 their
    whole height, a vertical arcade game 97%. The built-in bezels as before.
  - A game's bezel made for another shape of picture gives way to the console's own when that shows the picture clearly
    bigger: the DS's and the 3DS's bezels are made for their screens one above the other (the default: the DS's 100%
    of its size, the 3DS's 80%), so their other views show the console's own frame for that view; a 16:9 picture keeps
    the game's bezel when the console's own has no 16:9 window either (the Super Nintendo's: 74% against 75%).
  - Mega-CD and 32X: their projects are there (Sega CD: 468 bezels, Sega 32X: 215) for the consoles' keys "megacd"
    and "32x".
- **Credits**: About ("Game bezels by The Bezel Project (github.com/thebezelproject)") and THIRD_PARTY_NOTICES.md.
  Spanish in es.ini and es-emusuni.txt.
- PC tests (scratchpad bezels-0080/pc: the app's BezelProject.cpp, Net.cpp with libcurl, CoverDownload.cpp,
  Library.cpp's folder scan, Arcade.cpp with FinalBurn Neo's DAT, Image.cpp; ASan + UBSan):
  - The lists from GitHub's API: 20 consoles, 23 projects in 23 requests (5.0 MB of gzip; MAME 11 205 bezels, PSX
    8 550, NDS 7 492, none cut short), then from the copies (no request), then two copies 2 days old: 304.
  - The owner's games (scratchpad usbinv: the USB stick's lists and that day's uploads, read as the app reads them)
    with a bezel: Super Nintendo 776 of 783, NES 821 of 846, N64 243 of 245, Game Boy 4 of 5, Game Boy Color 483 of
    491, Game Boy Advance 190 of 212, Mega Drive 654 of 666, DS 315 of 326, Arcade 612 of 612 - 4 098 of 4 166 (98%),
    about 990 MB. Without one: homebrew, hacks and Spanish names of other titles ("Pokemon Rojo", "Pokémon Blanco").
  - Downloads from raw.githubusercontent.com (9 scenarios, all pass): 30 SNES bezels with a game started midway (no
    byte while it ran), the app closed after 8 of 28 NES bezels and the next start going on (the 8 kept), a bezel cut
    at byte 100 000 going on from it (GitHub's Git hash after), 10 MB free (refused before the first file), a bezel of
    the player's own (left as it was), the player's Stop (not resumed), 15 arcade sets, all 87 files' Git hashes as
    GitHub's lists have them (and as git's own), Delete Downloaded Bezels (only the player's own file left).
  - LoadBezel's window and Per Game's choice on real bezels (67 samples of all 23 projects and 72 of the owner's own,
    each in every shape of picture its console gives - 4:3, 16:9, 10:9, the Super Game Boy's 4:3, 3:2, the Mega Drive's
    320 wide, the DS's four views, the 3DS's, vertical arcade games): 0 pixels of the bezel over the picture.
- **The DS's and the 3DS's screens: the same settings on both consoles** (the owner: "agrégale a la Nintendo DS poder
  configurar la pantalla arriba y abajo, igual que la 3DS; dejar para ambas consolas las mismas opciones de
  configuración del tamaño y lugar de las pantallas"). DS Screens and 3DS Screens are gone: both consoles' pages (a
  game's Triangle -> Settings, the in-game menu's Settings) have the same five settings, with the same names, choices
  and order, in Video after Aspect Ratio; each console keeps values of its own.
  - **Screen Layout**: One Above the Other (the top screen over the touch screen; the 3DS's narrower touch screen
    centred under it), Side by Side, Large Screen + Small (the top screen big, the touch screen small beside it or in a
    corner), Top Screen Only, Touch Screen Only. One Above the Other is the default on both (the 3DS's since 00.64; the
    DS's was Side by Side).
  - **Swap Screens**: Off / On - the touch screen first: above, on the left, big, or the one shown alone. R3 swaps them
    in the game the same way on both consoles, for as long as the game runs; Screen Layout or Swap Screens changed in the
    in-game menu shows the screens as the menu says (R3's swap undone).
  - **Small Screen Size** (Large Screen + Small): Half, A Third (the default), A Quarter of the big screen.
  - **Small Screen Position** (Large Screen + Small): Right (the default, in the middle of the side), Bottom Right, Top
    Right, Left, Bottom Left, Top Left, Below, Above.
  - **Gap Between Screens**: None (the default), Thin (1/16 of a screen's height), Small (1/8), Medium (1/4), Large
    (1/2: about the DS's own hinge).
  - Every change shows at once, also in a game. Spanish in es.ini and es-emusuni.txt.
- **How**: each core gives its two screens side by side, always the same picture - melonDS DS 512x192, Azahar 720x240
  times 3DS Resolution - and the app places them for both consoles with one piece of code (src/DualScreens.h; Emulator.cpp
  DrawScreens). Until 00.79 the DS's One Above the Other was melonDS DS's own picture and R3 swapped the picture's halves
  as it came in; the 3DS's views were Azahar's own (its libretro window has no gap and no place for the small screen, and
  its big top screen was a 1920x960 picture even at 1x) and R3 changed its options, its picture laid out again.
  - The touchpad's place on the touch screen never changes with the layout, the gap or R3: the cores' touch screen is
    always at the same place of their picture. The 3DS's Top Screen Only now touches the touch screen unseen, as the
    DS's One Screen did; the 3DS's stylus is drawn on its touch screen wherever that is shown.
  - The GPU's filters (the Quality levels): screens at their own size (One Above the Other, Side by Side, one screen)
    are one picture for them - the filters' copy pass puts each screen at its place, the gap black (GpuFilter.h
    Source::parts, src/shaders/copy.frag) -, so each filter works as on any other console's picture; Large Screen +
    Small: the big screen through the filter, the small one drawn smooth (as the DS's Big Screen since 00.40); Tube TV:
    each screen a tube of its own. Scanlines on each screen with its own lines (the DS's handheld grid).
  - Bezels: every layout takes the window of its shape (4:3, -169, -wide, -tall) of the console's own bezel, the
    frames and the Retro TV, as before; the variants there are fit every layout, so none had to be made.
- **What a player kept stays** (00.70's rule), once ([global] screens=00.80; the log says what changed: `settings: the
  DS's and the 3DS's screens (00.80), once: ...`): the DS's Big Screen is Large Screen + Small (a third, on the right,
  as it was), its One Screen Top Screen Only; where the 3DS's Big Top Screen was chosen its look stays (a quarter, at the
  bottom right); a Side by Side kept stays. The DS's new default reaches only what was never set. The owner's file (his
  newest copy, 00.69's): his DS's own settings (This Console) keep Large Screen + Small, the same look as before, the new
  settings beside it; his 3DS stays One Above the Other; All Games' DS is One Above the Other.
- The log: `screens (the game's start): stack, the top screen first, small screen 1/3 right, gap none: a picture of
  256x384 of the console's pixels (...)`, again when they change in the in-game menu; the GPU filter's line says
  "2 parts".
- PC tests (scratchpad screens-0080; ASan + UBSan where C++):
  - src/DualScreens.h: 560 layouts of both consoles, 12 480 checks (places, gaps, sizes, no overlap, whole pixels; a
    touch by coordinates through the core's pointer and onto the TV).
  - Settings: 663 and 623 checks with and without the 3DS (the same names, choices, order, texts and defaults on both
    consoles' pages; the one-time change, only once; Reset All; the owner's file).
  - The GPU filters on lavapipe with Vulkan's validation layer: 198 pairs - every filter mode; Mario Kart DS, New Super
    Mario Bros. and the 3DS at 1x and 2x; each layout at one size, with and without a gap, swapped - drawn from the
    core's side-by-side picture with parts and from the same layout made on the CPU: the same pixels; no validation
    error.
  - Azahar with the options the app sends now for every 3DS Screens layout, swap and 3DS Resolution 1x to 3x: the
    picture side by side at the size expected; Craftus Reloaded's buttons tapped through the app's rectangle reach the
    same game state as 00.62's One Above the Other did (the touch screen identical; 28 of 255 apart without the taps).
  - melonDS DS: touches sent as the app sends them put its cursor at the touch's point of the touch screen (4 points,
    within a pixel); then every layout on a 1080p TV with every bezel (off, the console's own, the Retro TV, the six
    frames; Fit and Integer Scale), from the cores' real pictures: 40 308 checks - nothing cut, no bezel over a screen
    (but the Retro TV's rounded tube corners, its look on every console), the screens never over each other, each its
    own shape, and the DS's cursor shown on the TV at the touch's point in every layout.
  - Azahar's options against the core's 32 (255 configurations), test0069, test0071, ui-0068's unit test and the
    touchpad's pass; p2-pc's old-file test reads "large" for the DS's "big" now.
- **Two new consoles: the Sega CD (Mega-CD) and the 32X** (the owner: "quiero agregar Sega Mega CD y Sega 32X"). Each
  has its tab, icon and name (after the Genesis in the standard order; by power and by year where they belong), its
  folders (Roms/megacd and Roms/32x on the USB drive and in /data/EmuSuNi/Roms), the console's box on its tab, covers
  (libretro-thumbnails' "Sega - Mega-CD - Sega CD" and "Sega - 32X"), star ratings (LaunchBox's "Sega CD" and "Sega
  32X"), RetroAchievements (the hashes, the memory, the RA marks), cheats, Clean Up, saves and states folders of its
  own, the memory guard and the start's watchdog, and the Genesis's six-button pad. Defaults as the other 2D consoles:
  Quality Low, Aspect Ratio Auto (4:3), the console's own bezel - two new ones in scripts/make-bezels.py's style, no
  logos or names: the Sega CD's black with the drive's round lid, the 32X's black with its red and gold stripes, each
  with the pad's D-pad and six buttons. The Bezel Project has both (Sega CD, Sega 32X: see above).
  - **The Sega CD** runs on Genesis Plus GX (the Genesis's core). It needs the player's BIOS - none comes with the app:
    bios_CD_U.bin (USA), bios_CD_E.bin (Europe) or bios_CD_J.bin (Japan), by the disc's region (its header), looked for
    in /data/EmuSuNi/system/megacd, /data/EmuSuNi/system, beside the game and in the folder above it (any case of the
    name). Missing, the "game couldn't start" panel says which and where ("This USA disc needs the BIOS bios_CD_U.bin.
    Put it beside the game, or in /data/EmuSuNi/system/megacd"); the log says which BIOS was used.
  - Its discs: .cue (with its .bin tracks), .chd, .iso, .m3u - not in a .zip. In a folder shared with the PlayStation
    and the Saturn a disc is the Sega CD's by its first sector ("SEGADISCSYSTEM"). A game on several discs is one game
    as the PlayStation's (an .m3u, or "(Disc 1)", "(Disc 2)" in the names): the disc changed in the in-game menu, a
    state remembers its disc, the game starts at the disc it was on. Its backup RAM (the saves) is per game; a PAL disc
    runs at 50 Hz.
  - Genesis Plus GX's changes (scripts/build-gpgx-core.sh): an .m3u's absolute paths; the newer disc control (the first
    disc chosen, the discs' paths and names); the BIOS's paths from the app and a missing one said; the backup RAM given
    to the app; fixed: "disabled" cartridge RAM crashing (its size 0xff), the disc and the .m3u left open at the game's
    end.
  - **The 32X** runs on PicoDrive (libretro, 2.05-1890c29; scripts/build-pico-core.sh, scripts/setup.sh), built for the
    PS4 as the other cores (its API renamed picodrive_retro_*). Files: .32x, a .bin in its own folder (in a folder
    shared with the Genesis, a .bin whose header says "MARS CHECK MODE"), a .zip of one. No BIOS needed (PicoDrive's
    own start). The 32X CD games aren't there.
  - Its two SH-2 CPUs run on PicoDrive's recompiler, its code (4 MiB) in the app's shared JIT arena: on this PC 0.25 ms
    a frame against 2.6 ms with the interpreters (both SH-2s busy all the time) - about 1.8 ms of a frame's 16.7 on the
    PS4 against 13 to 19. Where the arena can't run code the interpreters run by themselves; the file
    /data/EmuSuNi/32x-drc-off turns the recompiler off (if a game misbehaves). Its big tables are on the heap only while
    a game runs.
  - PicoDrive's changes: at the game's end its idle-loop patches put back before the 32X's memory is freed (a crash
    with the next game) and the cheats ended (they went on into the next game); frames the app leaves out not drawn.
    The app keeps its cheats within what PicoDrive reads (each part of a code at most 11 characters).
  - PicoDrive's licence is non-commercial (it may not be sold); EmuSuNi is free. THIRD_PARTY_NOTICES.md and
    assets/licenses/picodrive-license.txt.
- **Ratings**: a copy of LaunchBox's list made before 00.80 has neither console, so the app downloads LaunchBox's
  Metadata again once (the copy's new "# platforms:" line says which consoles it has).
- The image: 128.3 MiB of file, 215.6 MiB of MemSiz, about 222 MiB free at display ready (00.79: 225). Spanish in es.ini
  and es-emusuni.txt.
- PC tests (homebrew of our own, no Sega code; local/herramientas/prueba-32x and prueba-megacd, scratchpad sega-0080;
  ASan where C++):
  - The 32X: a test ROM whose two SH-2s draw the Mandelbrot set and play a PWM tone - pictures, sound, states, a cheat,
    frames left out, the arena given back, the interpreters' picture the same as the recompiler's, a second game in the
    same process.
  - The Sega CD: a test BIOS and discs of our own - each region's BIOS asked for and found, the message without it,
    pictures and sound, the backup RAM per game, an .m3u of two discs (the disc changed, a state of disc 2, the game
    started at disc 2), a Genesis cartridge after it.
  - The Library (19 checks: the Sega CD's own folder, a folder shared with the PlayStation, the Saturn and the Genesis,
    the 32X's .bin by its header); the boxes' and messages' layout in English and Spanish; the unit tests with 21
    consoles.

## 00.79 (test build)
- **A tip on the 3DS tab** (the owner: "pon una cajita de información al lado derecho abajo, en el carrusel de juegos,
  informando que si no les carga un juego a la primera, pueden apretar L3 y R3 y reiniciar el juego hasta que cargue").
  A small box at the carousel's bottom right, drawn like the console box: "A game doesn't load the first time? Open
  the menu with L3 + R3 and choose Reset Game until it loads." The buttons are the player's In-Game Menu choice
  (Settings -> Controls; a Touchpad choice is L3 + R3 on the 3DS, as in the game). Only on the 3DS tab, under the
  covers, their stars and "RA" marks; the game's title and its line under it keep out of the box (a long line ends in
  "..."), and the box fades out under the search keyboard and a side panel. Spanish: "¿Un juego no carga a la primera? Abre el
  menú con L3 + R3 y elige Reiniciar juego hasta que cargue."
- **RetroAchievements' texts in Spanish at once, with Google's translator** (the owner: "las descripciones de los logros
  aún salen en inglés"; his 00.76 and 00.78 logs: Super Mario World's 174 achievements, 193 titles translated, then
  MyMemory's 4500 characters a day used - "the rest tomorrow (296 texts)" - with the descriptions waiting behind the
  titles). The main translator is now the one Chrome's page translation uses (translate.googleapis.com, client "gtx":
  no key, unofficial - the owner accepts it may stop some day): a whole game's titles and descriptions in one POST (a
  "q" each, its answer a list in order; up to about 24 KB a request). On the PC through the app's own Net.cpp: Super
  Mario World's 89 achievements and Super Mario Bros. 3's 76 (329 texts, 16 KB) in one request, 0.85 s.
  - MyMemory stays as the fallback when Google doesn't answer (Google again after 1, 5, 15, then 60 minutes), refuses
    (429, its "unusual traffic" page: left for the day, kept in translate-usage.txt) or answers something else; and for
    a text whose names Google lost. Its day counts the characters as sent (the names taken out), up to 5000; an
    answer it can't use is kept as English for good (it used to be asked again each session and still cost characters).
  - The order: each achievement's title then its description; the achievements on screen first (the selected one, then
    those around it: the in-game page and a game's page), then the game asked for last (the one played, the one
    opened), then the others. The names (the game's, quoted, capitalised) stay protected; the full stop a translator
    adds to a text without one is taken off; each text is asked for once (kept in cache/ra/translations-es.txt).
  - Said on the pages instead of English in silence: "Translating this game's texts: 92 left", or "92 of this game's
    texts stay in English for now: the translators can't today, the rest in about 5 h". The log says which service
    translated which game, how long it took, and each error.
  - MyMemory's e-mail (its "de" parameter: 50 000 characters a day, counted per e-mail address, not per PS4) isn't used:
    a project's address would be one pool shared by every player, written in the open source.
- **Settings -> RetroAchievements -> What is RetroAchievements?** (first row): what it is, the free account at
  retroachievements.org (its e-mail confirmed), the login here with the PS4's keyboard, playing (the start card, the
  popups, the in-game page, My Achievements, Triangle -> Achievements), Casual and the points, the consoles with
  achievements (the app's list; not the 3DS yet), which files it knows (the "RA" mark), and playing offline.
- **A game's Triangle -> Achievements** (the owner: the game page should open its achievements): on a game whose file
  RetroAchievements knows, the page My Achievements has - every achievement, locked or not, the last session's
  progress, in Spanish (its texts asked for first); Circle goes back to the game's options. Not logged in, and never
  played here: a line says the login shows them.
- **"Hay muchos juegos que no reconoce": the files it does know.** The same Triangle -> Achievements on a file it
  doesn't know (or another version of a game) shows its game - by the file's hash for another version, else by the
  name (brackets, "the" and punctuation left out; never a subset's entry) - with its achievements and the files it
  supports, named after No-Intro's list in libretro's database (GitHub, metadat/no-intro: the MD5 there is
  RetroAchievements' hash for cartridges - a NES file's without its header), downloaded once a month per console, only
  the names of the files RetroAchievements knows kept (cache/ra/names-<console>.txt); the player's own files it
  supports for that game; else "no achievements for a game of that name". RetroAchievements gives its files' names
  only to its web API's key or its site's pages (behind Cloudflare), so the rest are counted, with where its site lists
  them. Against RetroAchievements' lists of the owner's consoles, at least one supported file is named for 84% of the
  Mega Drive's games with achievements, 80% of the NES's, 78% of the Game Boy Color's, 75% of the GBA's, 57% of the
  Super Nintendo's and 38% of the N64's; discs (RetroAchievements' own hashes) and the arcade (sets' names) get no names.
  The consoles' lists from RetroAchievements now keep the games' names: those of 00.77-00.78 are asked for again once.
- PC tests: 24 scenarios, 140 checks pass - new: Google's stand-in (a game in one request, the names never sent, not
  asked again), Google without an answer (MyMemory meanwhile), Google losing a name (MyMemory for that one), the
  achievement on screen first, the versions page (by name, by hash, No-Intro's names, the player's files, the list
  asked for once); MyMemory's own scenarios run with Google refusing.

## 00.78 (test build)
- **RetroAchievements in Casual only** (the owner: "quiero que dejes solo el modo Casual en RetroAchievements").
  RetroAchievements hasn't approved EmuSuNi, and its server keeps an unapproved emulator's Hardcore unlocks as Casual
  (00.77), so Hardcore is gone until it does: rc_client always starts in Casual, and none of Hardcore's rules apply -
  states load (Continue included), cheats apply, Emulation Speed under 100% and a 60 FPS game at 50 work, the in-game
  menu opens at any time, the core options are as chosen.
  - Gone from the screens: Settings -> RetroAchievements' Hardcore Mode; the in-game Achievements page's Turn Hardcore
    Off, its mode line and its leaderboards (Hardcore's only: rc_client runs none in Casual - a game with leaderboards
    only now counts as one without achievements, and theirs aren't sent to MyMemory); "Off (Hardcore)" on Load
    State and Cheats, the Hardcore notes on the Cheats and Settings pages and in the help; the toasts ("Hardcore: states
    can't be loaded", "the game can pause again in N s"...); the start card's "RetroAchievements - Casual/Hardcore"
    (now "RetroAchievements"); "Game Mastered" (a game with every achievement: "Game Completed"); the 00.77 "Hardcore
    isn't available yet" notice. The server's warnings in a game's set ("Warning: Unknown Emulator" and the like, all
    about Hardcore) are logged, never shown.
  - The account's points as one number (Settings -> RetroAchievements: Points, the status line "Connected as ...
    (N points)"): the Hardcore points made elsewhere and the Casual ones together. My Achievements and its games no
    longer say "Hardcore"/"Casual" or "N in Hardcore"; a game's unlocks come from one request (h=0: all of them) instead
    of two. The covers' "RA" mark is gold when every achievement is unlocked (Information: "(completed)").
  - A retroachievements.ini of 00.73-00.77 with "hardcore=1" (or 00.77's "hardcore_refused") is read as Casual; those
    lines go at the next save. The history's files keep their columns (the mode written 0), so either version reads them.
  - The Hardcore code (the setting, its rules for states, cheats, pausing, speed and core options, the reset rc_client
    asks for) is removed, not switched off: 00.77's sources are kept in local/checkpoints/00.77-ra-hardcore-source.tar.gz
    for when RetroAchievements approves EmuSuNi. The Spanish lines of the removed texts are gone (32 lines and 00.77's
    "Hardcore not available yet" block); two new ones (the points' help, the marks' help).
  - PC tests: the module's scenarios against the stand-in server, Hardcore's dropped (its rules, the 00.77 refusal) and
    a new one (a 00.77 ini with Hardcore on and refused: logged in, the ini saved without it, the server's warning in
    the set shows nothing, every call to the server with h=0); 19 scenarios, 118 checks pass.

## 00.77 (test build)
- **RetroAchievements' mark on the games** (the owner: "que cada juego en las pestañas ... tenga alguna marquita o
  palabra para identificar cuáles son compatibles o soportados por RetroAchievements"). A small "RA" box in the cover's
  bottom right corner (30 pixels tall for the selected cover, as big as the cover for the others) and after the stars
  in the XMB's rows, for each game whose file RetroAchievements has achievements for - gold when the player mastered it
  (every achievement in Hardcore), gold-edged when completed in Casual, a gold line inside for the share unlocked;
  nothing while not checked, or for a file it doesn't know. Information (Triangle): "RetroAchievements: 89 achievements
  (12 unlocked)", "(mastered)", or why there is no mark ("this file isn't known", "another version of this game has
  achievements", "no achievements for this game yet", "not checked yet"). Settings -> RetroAchievements -> Marks on the
  Games: On (the default) / Off (none, and no file read for it). No account needed; with one, the account's progress
  (its games, asked for after the login when 10 minutes old - My Achievements' own) and the games played in EmuSuNi.
  - RetroAchievements' games of each console in the library come from its "systemgames" (no account: every game of the
    console with its achievements' count and its files' hashes, supported or not), one request a console, gzip (a
    quarter of the size: SNES 233 KB instead of 915), kept in data_dir/cache/ra/known-<console>.txt and asked for again
    after 7 days (or My Achievements' Update Now). A NES library asks for the Famicom Disk System's too.
  - The player's files are hashed as rc_client hashes a game (rc_hash: a cartridge's file - a zipped one's ROM read as
    the game's start reads it, 48 MiB of zip at most -, a disc's few sectors through the app's CHD/CSO reader, an arcade
    set by its name), quietly, on a thread of the module's own (256 KiB, low priority, cores 0, 1, 4, 5, made once),
    one file after another, stopping between files while a game runs (as the star ratings' work); each hash kept by
    the file's path, size and date in data_dir/cache/ra/library-hashes.txt: once per file.
  - The owner's games (his archives in D:\Descarga telegram unpacked on the PC, his arcade sets by name) against
    RetroAchievements itself: 3585 files, 6.45 GB read, 59 s on the PC (the next start: 0.5 s, nothing read or asked
    for); about 4 to 6 minutes on the PS4 from a USB drive, once. With achievements: SNES 438 of 793, Arcade 336 of
    642, GBC 149 of 484, GBA 100 of 221, N64 10 of 97 (73 are European versions RetroAchievements doesn't support),
    NES 17 of 899 and Mega Drive 17 of 449 (895 and 447 of them are Spanish translations, "(ESP)", which it doesn't
    know): 1067 in all. The menu's cost: the marks' table asked for once a second at most while the hashing goes on
    (4.5 ms for 3585 games on the PC's instrumented build), a few rectangles and two letters per cover drawn.
- **RetroAchievements' Hardcore while it hasn't approved EmuSuNi** (the owner, with Hardcore on: "¿los logros no quedan
  guardados?" - Super Mario World's "Unleash The Dragon", unlocked in 00.75, didn't show in 00.76). The server keeps an
  unapproved emulator's Hardcore unlocks as Casual (its "Warning: Unknown Emulator", id 101000001, "new score: 0"; the
  next login said "0 points, 2 casual"), and a Hardcore session shows only Hardcore unlocks. Now: the warning in a game's
  set puts the game in Casual right away (after the frame its achievements loaded), says it once ("Hardcore isn't
  available yet: RetroAchievements hasn't approved EmuSuNi: the achievements count in Casual"), shows the account's
  Casual unlocks as unlocked (the in-game page, My Achievements and its games), and is remembered for this version
  (retroachievements.ini's hardcore_refused): the next games start in Casual - Continue loads its state again - and
  Settings -> RetroAchievements -> Hardcore Mode says "Not available yet" and why. Each new version tries Hardcore once
  again (a Hardcore session without the warning clears it). The history's unlocks kept as Hardcore before are Casual
  too (no EmuSuNi has been approved). The warning itself is no achievement of the game's: out of the in-game list.
- **RetroAchievements on the Saturn** (Daytona USA's log: "the core shows none of the console's memory yet", then
  "The requested game is no longer active"). YabaSanshiro showed no memory (its retro_get_memory_data: NULL);
  scripts/build-saturn-core.sh now has it give its work RAM as Beetle Saturn gives RetroArch: a memory map with Work
  RAM Low at the SH-2's 0x00200000 and Work RAM High at 0x06000000, 1 MiB each - rcheevos' two Saturn regions
  ($000000-$0FFFFF, $100000-$1FFFFF) find them by those addresses -, both in 16-bit words of the host's order as
  Beetle's (memory.h's T2ReadByte: address ^ 1). PC (the core built with SATURN_CORE_HOST=1, Daytona USA from its .cue):
  "the core's memory map, 2 pieces", 2097152 of 2097152 bytes shown, 4/4 addresses valid.
- **A core whose memory comes late keeps its session**: after 10 s without memory the game was unloaded, and the
  session's start still on its way failed ("-31: The requested game is no longer active"). Now the note is said once
  and the memory asked for again each second; when it comes, the achievements go on loading (in Casual if the game had
  run without Hardcore's rules meanwhile).
- Spanish in es.ini. PC (scratchpad ra-0073/pc, ASan/UBSan, the stand-in with systemgames - gzip -, startsession's
  unlocks and the server's warning): library and library_noaccount (a file it has, another version, an unknown one, an
  arcade set by name; one list per console; the progress with an account; kept on the PS4; paused while a game runs),
  unknown_emulator (Hardcore while loading, Casual at the warning, the Casual unlock shown, said once, remembered, the
  next game Casual from its start); the 17 scenarios of 00.73-00.76 as before; run-libscan.sh (the owner's games,
  RetroAchievements itself); run-saturn.sh (the Saturn core's memory).

## 00.76 (test build)
- **Every menu goes round** (the owner: "que todos los menús se puedan pasar del primero al último apretando flecha
  para arriba"): Up on the first item is the last one, Down on the last the first - the Settings and their pages, the
  in-game menu (L3 + R3) and its pages, a game's options, the panels with choices, My Achievements, the XMB's lists
  and columns, the Library's games (Left / Right; Up / Down's ten at a time reach the end, then the next press goes
  round), the search keyboard's keys and its games (L1 / R1). Holding the arrow never goes round by itself: its
  repeat stops at the end, once; the next press goes on round (src/MenuNav.h, used by Menu.cpp and IngameMenu.cpp). A
  list that goes round shows its other end at once instead of sliding through every row, so the covers and the
  achievements' pictures in between aren't loaded on the way.
- **L1 / R1 held go through the console tabs** (the owner: "en el menú de pestañas, que se pueda mantener L o R
  apretado para avanzar varias consolas"): held, they repeat after 0.4 s, then a tab every 0.18 s (about five a second,
  each name readable as it passes), stopping at the last tab (the first with L1) - the next press goes round, as one
  press always did. The same repeat for the XMB's L1 / R1 (five items) and the search keyboard's games. While the tabs
  move, no covers are asked for and the drives aren't looked at: the tab they stop on loads its own 0.22 s after the
  last change (a single press: at once after that). L2 and R2 stay one press each, as before: Favorite and Search in
  the games' lists (Clear All and Done in the search keyboard) - they act, they don't move, so they don't repeat.
- PC (the menus' rules as the menus run them): scratchpad wsl/ui-0069/menunav-checks.h in ui-0068's unit.sh (126
  checks) and ui-0069's test0069.sh (1259, 839 and 799): round on a fresh press, a repeat stopping at the end, the
  headers of a settings page skipped, a list grown shorter, the pages of ten, the arrows' repeat (0.35 s, then every
  0.075 s, as before), Down and Up held through a list, R1 held through 19 tabs (18 changes, about 5.5 a second, never
  settled on the way, then round on a fresh press).
- **RetroAchievements: a game's achievements in My Achievements** (the owner tested 00.75: a game showed only what was
  unlocked - the page listed the unlocks made in EmuSuNi, and its games couldn't be opened). Cross on a game now shows
  every achievement of it, as the in-game page does: Locked first, then Unlocked, each with its picture (a locked one's
  grey one), name and points (Hardcore marked); below, its description, the progress of a measured one (the last
  session in EmuSuNi), when it was unlocked and in which mode, and how many players have it; at the top, unlocked of
  all and points. From the PS4's copy at once (data_dir/cache/ra/game-<id>.txt: written when a game's achievements
  load and when it ends, and after each look - shown offline too), then from retroachievements.org with the token
  (the game's sets, "achievementsets", and the account's unlocks, "unlocks" with h=0 and h=1). Circle goes back to
  the game's row.
- **RetroAchievements in Spanish** (the owner: "que los logros aparezcan en español"). Every label of the app's was
  already in es.ini; now also the server's warnings ("Warning: Unknown Emulator", "Hardcore unlocks cannot be earned
  using this emulator."...), and the achievements' dates as numbers outside English ("23/10/2026  14:05": the app's
  months are English abbreviations). The achievements' own names and descriptions (and leaderboards'), which
  RetroAchievements has in English only, are machine-translated:
  - The service: **MyMemory** (Translated srl; api.mymemory.translated.net/get) - free, no key (nothing secret in the
    package), its neural machine translation. Its limits: 500 bytes a request, 5000 characters a day for each address
    without an e-mail; its terms allow automated use, one segment a request ("translating more than one paragraph at
    once" is forbidden), and keep what is sent (public texts here). The others: LibreTranslate's own server wants a
    paid key now (its answer: "Visit https://portal.libretranslate.com to get an API key"); Lingva's public instances
    answered 403 and 500 (a front end of Google's site, against its terms); Apertium's free server answers without a
    key but translates word by word ("Batió el primer jefe sin tomar daño"); Google, DeepL and Microsoft need keys.
  - Once per game: when its achievements load (in a game, or opening it in My Achievements), its titles first, then
    the descriptions, one request each on the module's thread (after the server's own calls and between the pictures),
    within 4500 characters a day (counted in data_dir/cache/ra/translate-usage.txt); the rest goes on the next day, or
    a minute later without an answer. Kept in data_dir/cache/ra/translations-<language>.txt: offline afterwards, never
    asked again. English while a translation isn't there, or when it failed.
  - Names kept: the game's name, quoted names and, in a description, capitalised words inside the sentence and the level
    after them ("Defeat Bowser in World 8-4" goes as "Defeat {2} in {1}": "Derrota Bowser en World 8-4"); a
    translation that loses one of them isn't used. MyMemory's own first answer can be a crowd's segment of no quality
    ("Finish the game" came as "de la pistolera."): its machine translation is taken from its matches instead, else an
    exact match of good quality.
  - Settings -> RetroAchievements -> Translate Achievements: On / Off, on by default in Spanish (the row shows in any
    language but English; the texts go to the app's language). Where it shows: the in-game Achievements page and its
    popups (an unlock, a leaderboard), My Achievements and its games. The rich presence ("Level 7, 3 lives") stays as
    the game makes it, each second.
  - PC (scratchpad ra-0073/pc, the stand-in with MyMemory's /get, its quota and its matches): translate (6 texts, the
    names never sent, the popup, kept, Off), translate_quota (the day over after 2: no more asked), translate_break (a
    lost placeholder: that text stays English), game_detail (played then opened: the PS4's copy at once with its 4/10,
    then the server's set and unlocks; offline: the copy); with MyMemory itself (run-real-mt.sh): "Primero", "Derrota
    Bowser en World 8-4 de \"Sample Game\"", "Recoge 10 monedas en Sample Game", "Terminar el juego". The 13 scenarios
    of 00.73-00.75 pass as before. THIRD_PARTY_NOTICES.md: MyMemory.

## 00.75 (test build)
- **RetroAchievements: the session's state, live** (the owner: "colócale un estado de la sesión, si está conectado o
  desconectado"). Settings -> RetroAchievements starts with one line: "Connected as tester (1234 points, 56 Casual)",
  "Connecting to RetroAchievements...", "No connection to RetroAchievements: trying again in 25 s", "Not logged in" or
  "The login failed: <why>"; the Settings row says it short ("Connected as tester", "No connection (trying again)"...).
  It follows the connection as it goes: every server call's answer, or the lack of one, counts (a 502/503/504/52x from
  the site's front as none); without one the module tries again by itself - a login with the saved token, after 15 s,
  30 s, 60 s, then every 2 minutes - and while the panel shows, the connection is checked once a minute (a login with
  the token, which brings the account's points too). The in-game menu's Achievements page shows the same line (and,
  offline, that the unlocks wait and go when the connection comes back). A game started without a connection says so
  ("No connection to RetroAchievements: it tries again while you play") and its achievements are asked for again while
  it plays (the game's hash is kept: the app hashes it itself now, rc_hash as rc_client did, and loads by it); when they
  come, "Connected to RetroAchievements: this game's achievements are on" - in Casual if Hardcore was on, as
  RetroAchievements' rules want for a game that didn't start in Hardcore (no reset of the game).
- **RetroAchievements: My Achievements** (the owner: "¿dónde podrás ver el historial de logros?"). Settings ->
  RetroAchievements -> My Achievements: the achievements unlocked in EmuSuNi, the newest first (picture, name, points,
  Hardcore or Casual; below, the game, the date and its description), then the games with their progress (picture,
  name, unlocked / total; below, the console, how many in Hardcore, when played in EmuSuNi): those played in EmuSuNi
  first, the last one first, then every other game of the account with something unlocked (RetroArch's too).
  - Kept on the PS4, shown offline too: data_dir/retroachievements-history.txt (each unlock and each game's last
    session in EmuSuNi, by user; the last 1000 unlocks and 400 games), written on the module's thread.
  - The account's games come from retroachievements.org with the token alone (rc_client's own calls): "allprogress"
    for each of the app's 17 RetroAchievements consoles (every game of the console with achievements, the player's
    counts where there are some), then the games' names and icons ("gameinfolist", 90 at a time); when the page opens
    and they are older than 10 minutes, or Update Now; kept in data_dir/cache/ra/account-games.txt for offline. A game
    played in EmuSuNi after that update shows its own newer numbers.
  - What the server allows without the password (its dorequest.php, RAWeb's Connect API, with the user name and the
    token): the account's progress per console, the games' names and icons, a game's achievements and which of them
    the account has (their dates only from "startsession", which marks the game as being played), the account's
    points (the login). Not there: the account's recent unlocks across games with their dates (only the site's Web API has them, API_GetUserRecentAchievements.php, with a personal Web
    API key from the site's settings - not the token): so the dated list is EmuSuNi's own record.
  - The pictures are downloaded once into data_dir/cache/ra and made into textures only for the rows shown (the main
    menu's frames now count for AchievementsUi's three new pictures a frame).
- Spanish in es.ini. PC (scratchpad ra-0073/pc, ASan/UBSan, the stand-in server with allprogress, gameinfolist and a
  dropped connection): the 9 scenarios of 00.73 and 4 new ones - status (connected, a dropped connection found by a
  call, back by itself, the check once a minute at most), history (an unlock kept on the PS4, the account's games from
  17 consoles' answers and one names' call, the order, an empty answer of a console), history_offline (the record and
  the last update read at the start without a connection, another user's left out), retry_load (a game started without
  a connection: the note, no Hardcore, its achievements once the server is back, in Casual, an unlock sent as Casual).
- **The Saturn speed test's Settings row is gone** (the lead's change): Menu.cpp's "Saturn (speed test)" row, its
  case and its flag, and main.cpp's run of it, are taken out. src/SaturnTest.cpp stays for the Saturn's BIOS finder
  (FindSaturnBios, SaturnBiosName), which the Saturn console uses.

## 00.74 (test build)
- **The error reports of 00.73 are taken out** (the owner: "mejor elimina esta función de los fallos, me
  arrepentí"): no offer after a crash or a start that failed, no Settings -> Send a Report, no Square on "The game
  couldn't start", nothing sent anywhere (00.73 had no server set: nothing ever left a PS4). What 00.73 kept on the
  PS4 - data_dir/reports (a report waiting to be offered or sent) and report-server.ini if one was made - is deleted
  at the first start ("reports: 00.73's error reports are gone in this version: ... deleted (N files and folders)").
  The start watchdog of 00.70 (start.txt, start-again.txt, the restart, the logs kept) is as it was.

## 00.73 (test build)
- **HTTPS works on the PS4.** PacBrew's Mbed TLS 2.16.6 (curl's TLS) was built with clang 13 by a bignum assembly
  without its memory constraints (fixed upstream in 2.16.12 / 2.28.0), so every RSA operation failed ("RSA - The
  PKCS#1 verification failed", -0x4380) and every server with an RSA certificate or key exchange refused the PS4:
  raw.githubusercontent.com, GitHub's release files, thumbnails.libretro.com (only ECDSA servers, api.github.com and
  RetroAchievements, answered). scripts/build-mbedtls.sh (run by scripts/build.sh once; setup.sh fetches PacBrew's
  tarball, SHA-256 checked) compiles bignum.c again as PacBrew's package does, with upstream's 3-line fix, into a copy
  of PacBrew's libmbedcrypto.a; libmbedx509.a and libmbedtls.a are PacBrew's. It checks that every other object is
  PacBrew's byte for byte and that the symbols defined and needed are the same (curl's libcurl.a links them as
  before); CMake's MBEDTLS_LIB_DIR. The server's certificate is always checked (the app's cacert.pem, the host name,
  the dates), TLS 1.2 at least, a redirect from https to http refused, Authorization and Cookie headers never sent on
  to another host. PC (the PS4's own objects in a Linux curl 7.80.0, scratchpad update-0073/pc): Mbed TLS's self-test
  of the PS4 library now passes (MPI exp_mod, PKCS#1 decryption failed before); api.github.com, the release's .pkg
  through GitHub's redirect to release-assets.githubusercontent.com (186843136 bytes, stopped and resumed, GitHub's
  SHA-256), raw.githubusercontent.com, thumbnails.libretro.com over https and RetroAchievements' POST answer; expired,
  wrong-host, self-signed and untrusted certificates refused (badssl.com and a local server); the covers' folder list
  and the cheat collection over plain HTTP as before.
- **src/Net.h, the app's network for every feature** (the updater, RetroAchievements, the star ratings): Net::Fetch
  (the body in memory), Net::Download (into a file, through <file>.part, which a later call can go on with: Range) and
  Net::Stream (a sink), for GET, POST (a body and its content type), HEAD or any method, more headers, a user agent
  (default "EmuSuNi/<version> (PS4)"), a whole-transfer timeout and a stall limit, redirects followed (303, and 301/302
  after a POST, as a GET), a 4xx/5xx answer's body when asked, a progress callback (which can stop it), a cancel flag;
  the answer: its status, body, headers (Header()), final address, sizes, curl's error for the log and plain words for
  the screen ("the server's certificate isn't valid", "the secure connection failed: check the PS4's date and time",
  "the storage is full or can't be written"...). Nothing throws. Net::Get / GetText (covers, cheats) are the same
  calls underneath. PC: 50 checks against a local HTTP/HTTPS server with ASan/UBSan (POST, 302/303/307/308, loops,
  error bodies, HEAD, ETag/304, timeouts, a stall, cancel from another thread, resume, a server that ignores Range, a
  cut transfer, a full drive, two threads with a session each).
- **Updates from inside the app** (the owner: "buscar actualizaciones ... que se actualicen solas ... o por lo menos
  que les salga un aviso"). src/Update.cpp asks GitHub (api.github.com/repos/arturofuentes1988/EmuSuNi/releases/latest,
  no token) at most once a day by itself (when the menu first opens) and whenever the player asks; a release is newer
  when its tag vNN.NN is above this version (v00.21 < 00.73: no false alarm). What it found is kept in
  /data/EmuSuNi/update.ini (with GitHub's ETag: an unchanged answer is a 304). A newer version's notice ("Update
  Available": its number, the release notes' first lines without Markdown or emoji, Update Now / Later) comes at every
  start until it is installed; Later closes it until the next start; a start whose check fails (offline) shows none.
  "[obligatoria]" in the release's text makes it an "Important Update". Settings -> Check for Updates: this version, the
  newest one, Update Now, Check Again, Include Test Versions (GitHub's pre-releases too: /releases; off by default).
  Update Now downloads the release's .pkg to the USB drive's PKG folder (any letters' case; never a drive that looks
  like the PS4's own storage), else /data/EmuSuNi/update, after checking the free space (the .pkg + 16 MiB on a drive,
  twice it + 64 MiB inside), with its progress (Circle closes the page, the download goes on; Stop the Download; a
  stopped or cut one goes on from where it was), then checks it: a PS4 package (its magic) of EmuSuNi (its content id),
  GitHub's SHA-256 ("digest"), else a .sha256 file of the release, else its size; a damaged one is deleted. Then it hands
  it to the PS4's installer as the homebrew stores do (src/UpdateInstall.cpp: App Inst Util and BGFT loaded,
  RegisterTaskByStorageEx with the path as the system sees it, /mnt/usb0/... or /user/data/..., StartTask; the
  installation shows in Notifications > Downloads), follows the PS4's copy of it and then offers Close EmuSuNi (the PS4
  finishes the installation with the app closed). When the PS4 answers 0x80990088 (the same application installed:
  EmuSuNi's old task), that old task is released and the new one registered at once (pkg-mutant-shop's way, measured on
  a 13.52 PS4); the file /data/EmuSuNi/update-keep-tasks turns that off. A .pkg on the internal storage is deleted
  once the PS4 has copied it (else when the new version first starts, or a day later). If the installer can't be asked,
  the .pkg stays (one inside moved to /data/pkg) and the page says where and to install it with GoldHEN's Package
  Installer. Every step is in the log ("update: ..."; the installer's every answer with its code). A file
  /data/EmuSuNi/update-url.txt (its first line: another address of GitHub's answer, http or https) points the check
  somewhere else, for tests. PC (scratchpad update-0073/pc/run-update.sh, a fake GitHub, ASan/UBSan): 50 checks of the
  rules (versions, GitHub's real answer and a list with drafts and pre-releases, the notes' lines, the marker,
  update.ini, once a day, the notice, the space) and 47 of the app's starts one after another (the notice after the
  first check, the same day without a check, offline, after the update, the old public version, the important update,
  USB and internal downloads, deletion, a wrong SHA-256, a web page, another title's package, a .sha256 file, the size
  alone, no room, the installer refusing, GitHub's hourly limit, ETag, test versions, stop and resume, https); and the
  hand-over against stand-ins of the PS4's calls (run-install.sh, 15 checks: the system's paths, the task's parameters,
  the old task's release and update-keep-tasks, a queued download left alone, both progress layouts, errors). Only the
  PS4 can show the installer's part: BGFT from this app without a jailbreak, the old task's release, the install while
  EmuSuNi runs.
- **Star ratings under the covers** (the owner: "que cada juego tenga abajo de la carátula una especie de estrellas de
  valoración del juego"). src/Ratings.cpp:
  - The ratings: the LaunchBox Games Database's community ratings (the average of its players' own, 0 to 5 stars, with
    how many voted), in half stars. Chosen among ScreenScraper (one member's note out of 20, no votes, developer keys),
    IGDB, MobyGames and RAWG (secret or paid keys; no redistributing), TheGamesDB (no scores), Wikidata (CC0, but
    critics' scores of mixed scales for 50-360 games a console) and libretro-database's magazine scores (64 entries):
    LaunchBox rates nearly every game of the app's 19 consoles (SNES 2478, NES 3002, PlayStation 4303, DS 3789, Arcade
    6841...) with no key. It has
    no licence that lets an app hand its data on, and its developer offers the whole database as a download ("This is
    exactly how LaunchBox itself ties into the data"): so **each PS4 downloads it from LaunchBox**, as LaunchBox's own
    app and others do - https://gamesdb.launchbox-app.com/Metadata.zip, about 110 MB, in the background when the menu
    opens without ratings (or with ones 60 days old), paused while a game runs, a cut download going on where it
    stopped - and keeps only the rated games of its consoles (data_dir/cache/ratings/launchbox.tsv: id, platform, half
    stars, votes, names; 2.5 MB, 34190 games); the zip is deleted. Metadata.xml is read as it is unpacked, a line at a
    time, up to its pictures' part (1.6 s on the PC). After that the stars need no network.
  - The package has the app's own index (assets/xmb/ratings/<console>.tsv, 1 MB, scripts/make-ratings.py): each
    console's No-Intro / Redump names (libretro-database's, CC BY-SA 4.0: the names the covers are matched with) with
    the LaunchBox game they are - by their words as the covers compare names, LaunchBox's other names (Japanese and
    European titles), and the cartridges' game codes (Pokemon - Edicion Esmeralda (Spain), BPES, is Pokemon - Emerald
    Version, BPEE) -, and every FinalBurn Neo, NAOMI and Atomiswave set by its name (MAME's list in LaunchBox, then the
    set's full name, then its parent's).
  - A game is matched once, on a thread of its own (256 KiB) as the menu reads the games: the covers' matching
    (CoverDownload::Closest's sure matches: the name given with Rename, else the file's; GoodTools names, a list's
    numbering in front, region tags) against its console's index and LaunchBox's own names of the console's games (those
    rated by 3 players at least: a hack's name rated by one would be some other game's closest name); the Game Boy and
    the Game Boy Color together; an arcade, NAOMI or Atomiswave game by its set's name, else its parent's. Kept by game
    key; a game renamed is matched again; a new download matches every game again.
  - The Library: five stars under each cover (27 pixels each for the selected one, smaller for the others), half stars,
    on the reflection's top, under the part of the cover in view (the nearer covers hide the inner side of the wide
    ones) and along its tilted edge; none for a game without a rating; gone while the search keyboard is up. Drawn as a
    picture of a star (made once, soft edges) tinted gold or grey, a quad each: on the PC a Library frame's draw lists
    take 9.5 us instead of 7.3 (as polygons 32 us). The XMB's rows: small stars after the badge. Information (Triangle):
    "Rating: 4.5 of 5 from 1367 votes" with its stars, "By the players of the LaunchBox Games Database" (or why there
    is none). Settings -> Star Ratings: On (the default) / Off (no stars, no download) / Download Now or Again, what
    was downloaded and when, or the download's progress or failure; the Library's line under the console's name says
    "Downloading ratings 45 / 104 MB". About credits LaunchBox. Spanish in es.ini.
  - PC (scratchpad ratings-0073: pc/run.sh, ASan/UBSan and -O2): the app's Extract of the real Metadata.zip is
    byte-for-byte the script's own reading of it (Python's XML parser); each console's joined names are the script's;
    the owner's games (the FTP listings of his stick and the archives uploaded to it) rated: 3409 of 3434 (SNES 778/783,
    NES 830/840 with the Spanish translations, N64 239/239, DS 319/326, Mega Drive 654/655, Arcade 586/588, the Game Boys
    3/3) in 0.4 s for all on the PC; the Downloader with a cut and a resume, a pause, a failing server, a web page
    instead of the zip. pc/online.sh: the app's own Net.cpp (curl over the PS4's Mbed TLS) downloading LaunchBox's live
    Metadata.zip over HTTPS (with a pause), then the copy (34190 games). The layout (ui/mock-stars.cpp with Menu.cpp's
    drawing, real covers, dark and light themes, wide and upright covers) and the texts' widths in English and Spanish
    (ui/lang-check.py): all fit.
- **RetroAchievements** (the owner: "logros ... como en RetroArch"). src/Achievements.cpp, with RetroAchievements' own
  library, rcheevos 12.5.0 (rc_client, rc_libretro's memory maps and Hardcore rules, rhash; MIT), and libchdr for .chd
  discs, both pinned in scripts/setup.sh and built by CMakeLists.txt (no exceptions; about 0.6 MiB of code with the
  app's part):
  - The account: Settings -> RetroAchievements -> Log In: the PS4's keyboard asks for the user name, then the password
    (the keyboard's password mode, no learning, no capitals), which is sent once to log in and never kept: only the
    token RetroAchievements gives back is saved, in /data/EmuSuNi/retroachievements.ini (with the user name and
    Hardcore's setting). The app logs in with it at every start, in the background; an expired token is forgotten (the
    panel says to log in again); Log Out forgets it; the panel shows the account's points (Hardcore and Casual). Its
    network is src/Net.h's HTTPS, on a thread of its own (256 KiB, cores 0, 1, 4, 5; started only once there is
    something to send), with RetroAchievements' required
    user agent: "EmuSuNi/00.73 (PS4 <system version>) <core>_libretro/<version> rcheevos/12.5".
  - Each game is identified as RetroArch does: a cartridge from its bytes as the game starts (a 32 MiB N64 cartridge
    in about 80 ms on the PC), a disc on the module's thread - .cue/.bin, .iso, .chd (CD, GD-ROM and DVD), .cso,
    .gdi, .ccd/.img, .mds/.mdf, the disc an .m3u starts with (and each disc put in later) -, an arcade set by its name,
    a Famicom Disk System image as one. Then its achievements, leaderboards and rich presence load, and after every
    frame the core runs rc_client checks them against the game's memory, as each core shows it (its memory map, else its
    memory; Snes9x's map made by the app, src/AchievementsSnes.cpp, an SA-1 game's I-RAM included).
  - On screen: the game's card at its start (its picture, "12 of 50 achievements - 105 of 400 points", Hardcore or
    Casual), each unlock's popup with its picture (downloaded once into /data/EmuSuNi/cache/ra) and points, Game
    Mastered, leaderboards started, failed and submitted, their trackers, challenge indicators and progress. The in-game
    menu's Achievements: the game's achievements in rc_client's groups (Recently Unlocked, Almost There, Locked,
    Unlocked...) with their pictures and points (each one's description, progress, unlock date and rarity below), the
    game's rich presence and its leaderboards. Unlocks that can't be sent are retried while the app runs (No connection /
    Connected again popups). Each save state keeps the achievements' progress beside it (<state>.ra), loaded with it.
  - Hardcore (Settings -> RetroAchievements -> Hardcore Mode; Off by default; from the next game), RetroAchievements'
    rules: no state is loaded (Load State, and Continue: the game starts from its beginning, and says so), no cheats
    (the .cht codes and the cores' own), no slow motion (Speed under 100%, a 60 FPS game at 50), the core options
    rc_libretro disallows put back (a PAL region forced, Snes9x's hidden layers, FCEUmm's Game Genie, a slower Dreamcast or
    3DS clock...), the in-game menu opens only as often as rc_client allows (a toast says when), and the in-game menu's
    Turn Hardcore Off goes back to Casual at once. Fast Forward stays. Until RetroAchievements approves EmuSuNi its
    server counts Hardcore unlocks as Casual (the setting's help says so).
  - Never in a game's way: nobody logged in, no connection, a game RetroAchievements doesn't know (or a version of it it
    hasn't tested, or that doesn't work: said as such), a 3DS game (RetroAchievements has no 3DS achievements yet), a core
    that shows no memory: the game plays as before, with a short note. Every step is in the log ("ra: ...").
  - Spanish in es.ini. THIRD_PARTY_NOTICES.md: rcheevos and libchdr.
  - PC (scratchpad ra-0073/pc, the module with the app's Net over libcurl, ASan/UBSan): 9 scenarios against a stand-in
    server, 61 checks (the login - the password only in the login's request, the token in the file -, the token's login,
    an expired token, a game with unlocks, progress, leaderboards, rich presence, pictures and a state's .ra, an unlock
    retried until the server answers, an unknown game, an untested version, offline, every Hardcore rule); the hashes
    against RetroAchievements' real server (no account: its game lookups): the same hash from .cue/.bin, .iso, .chd and
    .cso, Daytona USA (Saturn), Mario Kart DS, GoldenEye, arcade sets, the owner's N64 cartridges (16 European dumps: 3
    with achievements, the others versions RetroAchievements hasn't tested, Ocarina of Time's one that doesn't work);
    the memory of the app's cores built for Linux - Snes9x with the app's map (SA-1 I-RAM too), FCEUmm, mGBA (GBA, GBC),
    Genesis Plus GX (Mega Drive, Master System, Game Gear) and the app's own FinalBurn Neo object: every region
    RetroAchievements uses that the console has, as in RetroArch. scratchpad ra-0073/aprobacion.md: what Hardcore's
    approval takes (six months public: 15 April 2027 at the earliest; a privacy policy; questions for RAdmin).

## 00.72 (test build)
- **N64: World Driver Championship and Stunt Racer 64 start** (a tester's report on v00.21: "never start, black screen
  right away"). Boss Game Studios' microcode waits, in the middle of a graphics task, for a signal the N64's CPU gives it
  (it reads the RSP's status over and over); ParaLLEl-RSP halts such a task after a number of reads so that the CPU can
  run, and the task went on when the CPU answered - until upstream's parallel-rsp be05ef7 (2024, in mupen64plus-next
  since 2025: "Fixed RSP hang when setting HALT but not BREAK"), after which any halt stayed: the task never ran on and
  the game waited for ever after its first graphics task. Now only a halt the microcode sets itself stays; the waiting
  one goes on as before (as CXD4 does). scripts/build-n64-core.sh. PC (the owner's World Driver Championship (Europe),
  the app's options): before, one graphics task and then nothing (with ParaLLEl-RSP, through HLE sound or alone; with
  CXD4 it played); now 6 graphics tasks a frame as with CXD4, the legal screen, the language choice, the Midway and Boss
  logos in 3D, the title, the menus and the car choice with its car turning (scratchpad n64-0072/shots/wdc-in2-sheet.png).
  Stunt Racer 64 (USA only) runs on the same engine; no cartridge of it on the PC. None of the other games tried takes
  that path (their sound and memory are the same, below). The `n64 core per frame` line says how often a task waited
  (`the RSP waited for the CPU N times`).
- **N64: Ogre Battle 64 plays with N64 Graphics Sync Accurate**, whatever the setting, as Cruis'n USA and Resident Evil
  2 since 00.68 (the tester: "in some cutscenes and places the 2D backgrounds are broken, misplaced or swapped, seemingly
  at random"). Its backgrounds are pictures its RSP decodes into the N64's memory (a JPEG task) and the RDP then draws;
  with Fast the RDP's "done" comes before the GPU has read or drawn anything, and the PS4's GPU works on a copy of that
  memory, so the game decodes the next picture into the same place while the GPU still reads the last one, or the
  GPU's late copy of a frame lands over a picture decoded since. Accurate waits for the GPU at each finished picture.
  By the cartridge's game code (OB: the American and Japanese cartridges); no cartridge of it on the PC (the owner has
  none), so only the PS4 can show it. Also **Resident Evil 2**: the tester's black after the Expansion Pak and Nintendo
  64 screens is v00.21's (00.64's) Fast; 00.68 plays it Accurate by its game code RE, the same on the American (NREE) and
  European (NREP) cartridges; the Japanese one, Biohazard 2, is NB5J: added. The log says why each plays Accurate.
- **The battery save is written beside the game** (every console whose save the app writes: the N64's EEPROM,
  Controller Paks, SRAM and FlashRAM - 296960 bytes -, the cartridge consoles' saves...). The file was written, with an
  fsync, on the emulation thread 3 s after each change the game made: the game's time spent on the PS4's storage, a
  hitch where a game saves (00.28's GoldenEye log: written every few seconds at times). The data is copied there (a
  fraction of a millisecond) and a thread of its own (256 KiB of stack, started at the game's first save, off the
  emulation's core) writes it; a newer copy replaces one not written yet; the game's end waits for it and then writes
  the last copy itself, as before. The log: `save: battery save written beside the game (N bytes, N ms; it waited N
  ms)`, and the end's `save: ... written (N bytes, N ms)`. src/Emulator.cpp (SaveWriter). PC (scratchpad
  n64-0072/save-test: the class cut from the source, the PS4's calls stubbed, ASan, UBSan and TSan): a write beside the
  caller, eight copies posted during a slow write written as the newest one, the game's end with two files waiting
  (both written first), a failed write logged and the next one written, 2000 posts with flushes between them.
- **N64: the RSP's work is lighter** (the tester: Battle for Naboo, Conker's Bad Fur Day, Banjo-Tooie, Pokemon Snap,
  Rogue Squadron, Jet Force Gemini, Beetle Adventure Racing and Perfect Dark at 85-96% of their speed in heavy
  scenes). In those games the RSP's graphics microcode is 75-92% of the emulation thread's time (the PC, the owner's
  cartridges' attract modes: Conker 4.5 of 5.2 ms a frame, Snap 3.1 of 3.4, Rogue Squadron 2.3 of 2.7); the rest is the
  N64's processor and the sound. scripts/build-n64-core.sh:
  - ParaLLEl-RSP's commonest vector loads and stores of whole aligned blocks (LQV/SQV a quadword, LDV/SDV a
    doubleword, LPV/LUV eight bytes made elements, SPV/SUV elements made bytes) in one SSE load or store and a shuffle
    (the DMEM holds the N64's big-endian words as the PS4's words): they were 11% of Conker's emulation thread. Every
    one of the 24 vector loads and stores checked against upstream's on every address, element and register (9.4
    million cases): the same registers and memory.
  - PGO: scripts/n64-core.profdata.xz, a profile of the PC build of the same sources playing 20 of the owner's
    cartridges (as the Dreamcast's, the 3DS's and the Saturn's; N64_CORE_PGO=off builds without it). The PS4 build
    matches all but 30 of its 1252 functions in the files with any change; its call-graph section is left out of the
    core's object (GNU ld -r leaves it without an entry size, which the app's link refuses).
  - PC (the emulation thread's CPU time a frame in the attract modes, the median of 3 interleaved rounds against the
    same build without them, the null GPU): Conker -7.6% (its RSP -8.4%), Super Mario 64 -8.3%, Ocarina of Time -7.2%,
    Majora's Mask -6.8%, Jet Force Gemini -6.7%, Pokemon Snap -6.1%, Paper Mario -5.6%, Perfect Dark -5.4%, Rogue
    Squadron -3.3%, GoldenEye -3.3%, Banjo-Tooie -1.0% (its RSP -7.4%), Battle for Naboo -0.5% (its RSP -2.7%: the
    N64's processor is half its time); the same sound, memory and RDP commands in every one. And 19 cartridges' first
    3000 frames against 00.71's core: the same sound and memory in each (World Driver Championship apart). Conker stays
    slow: its microcode alone takes about 8 ms per picture on the PC (some 55 on the PS4).
- **N64: the hitches measured, and the log says what each was** (the tester: GoldenEye 007 and Perfect Dark "a strong
  hitch on every weapon change", Paper Mario "random strong hitches, down to 82% and 76%", Mario Party "one or two").
  On the PC the strongest frames are the N64 dynarec compiling code the game runs for the first time in the session:
  GoldenEye's first weapon change 827 and 318 blocks in two frames (13.4 and 6.4 ms on the PC, the PS4 several times
  that), the next changes of those weapons nothing; Paper Mario's attract mode 11-14 ms frames at its scene changes.
  ParaLLEl-RSP's own compiles are cheap (some 100 in a session, 5-7 ms in all) and no code was thrown away and compiled
  again (no invalidation in GoldenEye's run). The core never timed its compiling (the line's "recompiler 0.00 ms"):
  now it does (scripts/build-n64-core.sh, around new_recompile_block), and the `n64 core per frame` line every 10 s
  adds `the game showed N pictures a second` (the video chip's picture address changing: the game's own frame rate),
  `recompiled N blocks in N ms` and `the heaviest frame: RSP N ms (N tasks), recompiler N ms (N blocks), sound N ms`
  (port/ps4_n64.c): with the speed line's `most N ms`, a hitch's cause. (N64 CPU Timing Light, GoldenEye's since 00.30,
  is no help for Perfect Dark: on the PC its intro stops with it.)
- **N64: checked and left as they are**: Battle for Naboo's and Rogue Squadron's attract modes draw the same pictures
  with N64 Graphics Sync Fast and Accurate (lavapipe, every 250th frame of 6000): Naboo's breaking up is its speed, not
  that race. Space Station Silicon Valley (the tester's note cut off) plays its logos, its 3D intro and its newsflash
  cutscene on the PC. The tester's fine games (Super Mario 64, Ocarina of Time, Majora's Mask, F-Zero X, Diddy Kong
  Racing, Banjo-Kazooie among the owner's cartridges) emulate exactly as in 00.71 (the same sound and memory), only
  faster.

## 00.71 (test build)
- **3DS: the Quality levels again - Low, Medium, High and Ultra** (the owner: "darle calidades a la 3DS: media, alta y
  ultra"; from 00.64 to 00.70 it played at its own resolution only). The 3DS's pages (and its in-game menu) have
  Quality, 3DS Resolution (Native, 2x, 3x and now 4x) and the GPU Upscaler, as the other 3D consoles:
  - Low: its own screens, 400x240 and 320x240, with no filter - still the default (a new install, Reset All);
  - Medium: 2x (800x480 a screen) with Smooth Edges (SMAA at the size drawn: 800x960 for the screens one above the
    other);
  - High: 3x (1200x720) with Smooth Edges (one above the other, a picture bigger than the TV's, made smaller: its
    edges supersampled);
  - Ultra: 4x (1600x960) made crisper by AMD's sharpening (Natural HD).
  The size is the game's from its start (changed in the in-game menu, it is the next start's, as the N64's and the
  Dreamcast's); the GPU Upscaler changes at once. Each level's help on a 3DS page says its size and, above Low, that a
  game that reads its picture back can slow down. src/Settings.cpp (the levels, 3DS Resolution shown), src/Emulator.cpp
  (Build3dsVariables reads 3DS Resolution, 1x to 4x; GpuMode gives the 3DS's picture the GPU Upscaler as the other 3D
  consoles' - Pixel Art HD Smooth Edges, AI Natural HD -, Smooth HD only sharpening from 3x); the "3DS: the console's
  own resolution only" row is gone; Spanish in es.ini.
  - PC (lavapipe), Hey! Pikmin's gameplay and Super Mario 3D Land's World 1-1 from saved states, 900 frames at each
    size: the emulation thread's CPU time a frame the same at 1x, 2x, 3x and 4x (2.04, 1.92, 1.95, 1.92 ms and 1.81,
    1.84, 1.86, 1.91 ms), its waits for the GPU 105-117 and 134-151 ms in 600 frames, no surface read back at any
    size; GPU memory 32, 32, 96 and 96 MiB of blocks (11-12, 28-30, 50-57 and 81-94 MiB used).
  - The filter harness on both games' pictures as a 1080p TV shows them: each level a clear step from the one below
    (the TV's pixels changed by 3.0 and 2.8 of 255 to Medium, 1.3 and 1.3 to High, 1.9 and 1.1 to Ultra), with more
    fine detail each time; no Vulkan validation error; and on every 3DS Screens layout at 1x to 4x (75 pictures, the
    modes the 3DS's picture gets) each filter's chain made, no validation error.
  - Not used: Azahar's texture filters (ScaleForce, xBRZ and Anime4K only subtle on the 3D, MMPX blocky, Bicubic
    softer; their textures kept at the full size took 224 MiB of GPU memory at 3x and 4x against 96), and its Linear
    texture sampling (the games filter their textures already: Pikmin's picture the same, Mario's 0.2 of 255 changed).
  - Only the PS4 can show the GPU's time at 2x to 4x (00.69's log: Super Mario 3D Land at 1x 8% busy, 1018 MiB of GPU
    buffers; the Dreamcast at 3x, the 3DS 4x's 2.8 million pixels, 11-31%): the speed lines' "GPU busy" and the
    "gpu filter" lines say it.
- **3DS: a level chosen before is the 3DS's too**, once ([global] 3ds_levels=00.71): where a 3DS game's settings come
  from - All Games, This Console, the game's own - a level chosen there (or All Games' level by its values, in a file
  of before 00.63) gives the 3DS its size, and in the 3DS's own settings its GPU Upscaler too: All Games' page shows
  the level it showed. Where none was chosen the 3DS plays as before: a 3DS Resolution of 00.63 kept in the file is
  1x, a GPU Upscaler copied into the 3DS's own settings Off (All Games' GPU Upscaler is every console's now, the 3DS's
  too). No other console's values change. The log says it: `settings: the 3DS's Quality levels (00.71), once: ...`
  (each place and value changed, or "nothing to change"). PC tests (scratchpad 3ds-0071/test0071.sh, ASan + UBSan,
  with and without the 3DS): the four levels on the 3DS's pages, This Console, a size changed by hand (Custom), Reset
  All, files of 00.63 to 00.70, every other console's games reading the same with and without the change, the owner's
  settings.ini of 00.69 (his 3DS at Low as it was); 00.69's, 00.62's and 00.68's tests updated and passing.
- **3DS: the log says the level and the size**: a game's start, `3ds: picture settings from This Console: Quality ultra
  (All Games: low), 3DS Resolution 4x (the picture 1600x1920, screens stack; the size until the game's next start), GPU
  Upscaler ai`; and the stall log's GPU commands count the surfaces read back from the GPU into the 3DS's memory, the
  cost a bigger size can bring to a game that does it: `...; surfaces made N, N ms; read back N (N KiB, N ms)), ...`
  (scripts/build-3ds-core.sh).

## 00.70 (test build)
- **A game that doesn't open the first time repairs itself: the start watchdog** (the owner: "hay algunos juegos que
  no abre a la primera, se va a la pantalla azul o a negro o a blanco y debo reiniciar el juego. Por favor usa alguna
  técnica para que se repare"). src/StartGuard.cpp watches each game's start, from the core's set-up to 15 s after its
  first picture:
  - **No picture in the console's time while the game runs** (the core comes back from each frame, but no picture
    comes): the game is stopped as Quit Game stops it and started once more at once, with the toast "The game showed
    no picture: it was started again" (its Continue state kept as it was, neither saved over nor deleted); the second
    time the menu says "No picture came in N seconds, twice in a row." The game's running time, the in-game menu's
    left out: 20 s on the cartridge consoles, 30 s on the Arcade, the N64, the PlayStation, the DS, the 3DS and the
    Saturn, 60 s on the PSP, 120 s on the Dreamcast, the NAOMI and the Atomiswave (a first start compiles shaders).
  - **The emulation thread stuck inside the core, or a crash, while a game starts**: one call that doesn't come back
    (the game's file read, the core's load of the game, its renderer's set-up, Continue's state, a frame) for 45 s (60
    s on the N64, PlayStation, DS, 3DS and Saturn, 90 s on the PSP, 120 s on Flycast's consoles; the load 90 to 180
    s). The watchdog's own thread then restarts EmuSuNi (sceSystemServiceLoadExec of /app0/eboot.bin; the app's
    start-up watchdog already closes it with that call) after writing data_dir/start-again.txt, and the new process
    starts the same game at once (L1 held: the menu), with a toast: "The game stopped while starting: EmuSuNi
    restarted and started it again" or "The game crashed while starting: ...". A start that stopped while Continue's
    state loaded starts again without it ("...: EmuSuNi restarted and started it from the beginning"). A crash reaches
    the watchdog through the crash handler (port/ps4_crashlog.cpp, after crash.log), which waits up to 6 s for the
    restart and else ends the app as before: the 3DS's and the Dreamcast's fast memory, whose "trying" markers turn it
    off at the start after one that died, now get that start at once. The second attempt stopping or crashing too
    restarts into the menu, which says "The emulator stopped while it started, twice in a row". A game's end that
    doesn't come back (the core's stop, 45 s or the console's time) restarts EmuSuNi into the menu. If the restart call
    fails EmuSuNi closes after 5 s instead of staying frozen, and its next start starts the game again.
  - **Where a start stopped stays in the log.** data_dir/start.txt is written as a start begins (the game, its console,
    the attempt, when, the version) and removed after the first picture or the start's end; one left at the next app
    start is logged (`start: the last start didn't finish: <title> (<console>, <path>), attempt N, begun <time> with
    EmuSuNi <version> - the app was closed or stopped during it (its log: emusuni-1.log)`), and a second attempt's
    makes the menu say "EmuSuNi closed while it started, twice in a row". The steps with their times since the start:
    `start: the game's file is read (0.00 s)`, `the core starts`, `the core loads the game`, `the core sets up its
    renderer`, `the game loaded`, `Continue: the saved state loads`, `the game runs`, `the core's first frame ran
    (0.31 s)`, `the first picture shown (0.47 s; the game ran 0.2 s for it, 2 frames of the core)`, `no picture yet:
    ...` every 10 s, `start watchdog: ...` for what it did.
  - **The four sessions before are kept**: at each app start emusuni.log becomes emusuni-1.log ... emusuni-4.log, and
    xmb-trace.log (every line with its time) xmb-trace-1.log ... xmb-trace-4.log. Until the first picture, and while a
    game ends, the watchdog's thread writes both through to the disk every 2 s (fsync): a hang, a forced close or the
    power going off keeps them (the trace at a crash too).
  - Off the game's timing: a pass of the main loop costs an atomic store, a picture an atomic add; the watchdog's
    thread (256 KiB of stack, priority 350, cores 0, 1, 4 and 5) wakes every 250 ms and does nothing while no game
    starts or ends.
  - The screens: while a game loads the app shows its own black (the menu fades to black, the load runs on the
    emulation thread and nothing is drawn until the core's first frame), so black is a stop in the load or a core that
    shows nothing; blue is the PS4's home screen after the app closed (a crash; a C++ exception inside a core, as
    00.63's Atomiswave; the PS4 closing it); white is a game's own picture that stays (its core stopped after it, or a
    game waiting: the DS's health and safety screen waits for a touch). What differs at a first start: the Dreamcast's
    shaders (up to a minute), the PSP's pipelines, the 3DS's JIT warm-up and fast memory self-test, the 3DS's and the
    Dreamcast's fast memory "trying" markers (a start that dies trying it makes the next one go without it: "fails
    once, then works" by design), files read from the USB drive for the first time. The caches can't break a start:
    each is written to a temporary file and renamed, and checked when read (the Dreamcast's SPIR-V and pipeline
    caches, the N64's, the GPU filters', the 3DS's JIT warm-up). 00.68's log has one start of the "alive, no pictures"
    kind: Dolphin Blue after the in-game Reset at 0.2 pictures a second (its frames timing out at 100 ms) until the
    owner quit and started it again.
  - PC (scratchpad start-0070/test/run.sh: StartGuard.cpp and Log.cpp, the restart call a stub, each case in a process
    of its own): 70 checks - a first start (start.txt while it starts, gone after the picture, the steps' lines), no
    picture (Retry on time, the in-game menu's time left out, then the menu's message), a call stuck (the restart,
    start-again.txt, the new process's game at once with its toast, the leftover start.txt logged), the second attempt
    stuck (the menu's message), a real SIGSEGV during a start (the restart for the game), none after the start, a stop
    in the seconds after the picture, a stuck end (the menu), the app killed in a first and a second attempt,
    Continue's state stuck (started again without it), the restart call failing (closed after 5 s, start-again.txt
    kept), the log synced every 2 s only while a game starts, the logs' rotation. Only the PS4 can show the restart
    itself (LoadExec of the app's own eboot under GoldHEN).
- **3DS: a game that draws at 30 FPS no longer loses a third of its speed when it gets heavy** (the owner: "la 3DS
  sigue estando lenta, mira el reporte de la PS4"; 00.69's log: Hey! Pikmin's busy scenes at 61-63%). Hey! Pikmin
  draws a picture every other frame: a heavy frame (its picture: 34 ms on the PS4 there) and a light one (5 ms). Once
  the core took more than a refresh on average, the pacing's heavy mode ran one frame a pass, and the light frame
  waited a whole refresh by itself: 51 ms for two frames. Now, in that mode, a light first frame (under half a refresh)
  lets the pass run the next frame too when the pacing owes it - two images of the core's ring of three, never the one
  the drawing beside the emulation shows. src/Emulator.cpp; the 3DS only; a game whose frames are all heavy runs as
  before. The log says it once a game (`pacing: a light 3DS frame (N ms) and the next in one pass while the core is
  heavy (a game drawing at 30 FPS alternates heavy and light frames)`), and the pacing line's `per pass ... 2:` counts
  those passes.
  - A model of the passes as Emulator.cpp runs them (V-Sync off, as the owner's 3DS), with the frame times of 00.69's
    log (scratchpad 3ds-0070/model/pacing_model4.py): Pikmin's busiest scenes 65% -> 84%, its scenes at 69-86% in the
    log 88% -> 100%, the longest wait for a new game picture (p99) 60 -> 43 ms; a 60 FPS game too heavy everywhere,
    the light scenes and Super Mario 3D Land unchanged.
- **3DS: less work for each draw** (00.69's log: in Pikmin's busy scenes the GPU's commands took as much of the
  emulation thread as the game's own code, 6-9 ms a frame; on the PC the same gameplay makes 230 draws a frame, about
  3 us each with their command lists - some 20 us on the PS4). scripts/build-3ds-core.sh; the same pictures and the
  same Vulkan commands, fewer made:
  - a draw's texture and utility descriptor sets: upstream took a new set from the pool and wrote its images for every
    draw; a draw with the same images as the one before it, in the same submission, binds that set again (Pikmin's
    descriptor writes 930 -> 386 a frame);
  - the pipeline: upstream hashed the whole pipeline state twice and looked both hashes up for every draw; a state
    equal byte for byte to the last draw's gives its pipeline at once;
  - two short loops of every draw that clang had vectorized with masked loads and stores (vmaskmovps: microcoded on
    the PS4's Jaguar, cheap on the PC, so it doesn't show there): the command lists' runs of registers and the fixed
    vertex attributes (none left in those functions; 10 in 00.69's).
  - PC, from saved states, 3 interleaved rounds against 00.69's core (each with the same PGO profile): Pikmin's
    gameplay (walking, whistling, throwing) the GPU's commands -4.7% (390 -> 372 ms in 600 frames), the emulation
    thread -2.0% (1.78 -> 1.74 ms a frame); Super Mario 3D Land's World 1-1 -1.8% and -0.5%. The same pictures, 10 of 10
    in each game, byte for byte.
- **3DS CPU Clock is applied after a saved state too, and says what it does**: a state brought the core timing's
  clocks back at 100% (upstream doesn't keep the setting in the state and never applied it again), so a game continued
  from a state (Continue, a state loaded from the menu) ran at 100% whatever the setting said. And a lower clock
  doesn't lighten the games tried: with the fix, on the PC, Hey! Pikmin's gameplay at 100/90/75/50% took the same 1.85
  ms a frame and drew the same 30 pictures a second (its processor waits for the next picture 86% of the time; a
  slower clock only stretches the waits), Super Mario 3D Land's World 1-1 at 100/75/50% the same 1.8 ms and 60
  pictures a second. Its texts no longer promise "a quarter less work": less work only in a game that keeps the
  processor busy without a break; a game that needs the speed slows down or freezes (Spanish in es.ini).
- **3DS: the log divides the GPU's commands**: each slow frame and each 600-frame line say how many draws (and
  vertices) they made and the time the draws took of them (the rest: the command lists' registers), the command
  lists' size, the draws whose vertices the CPU had to shade (none in the games tried), and how many pictures the game
  drew (its top screen's swaps: 300 in 600 frames is a 30 FPS game): `GPU commands N ms (N draws of N vertices, N ms
  of it the draws; N KiB of command lists; ...)` and `...; the game drew N pictures`.

## 00.69 (test build)
- **Every console starts at Quality Low, Aspect Ratio Auto and its own bezel** (the owner: "como configuración general
  quiero que todas las consolas partan con calidad baja, resolución automática 4:3 y con marco de la consola"): what a
  setting never changed reads, so a new install and Reset All Settings.
  - Quality Low: the PSP's own 480x272 and no anisotropic filtering (2x and 16x until now: the only values never
    changed that weren't Low's, so a PSP game read Custom).
  - Aspect Ratio Auto, as before: 4:3 on the TV consoles (the NAOMI's and the Atomiswave's vertical games 3:4), the
    Game Boys' screens their own shape, the PSP's 16:9, the DS's and the 3DS's screens theirs, an arcade board its own.
  - Each console's own bezel (Off until now); the Arcade's own is the Arcade Cabinet.
  - **The Saturn's bezel**, new (`saturn-console.png`, `scripts/make-bezels.py saturn-console`, as the other Sega
    consoles': a 4:3 window of 1440x1080): charcoal black, the round lid, 32-BIT, the round D-pad, START, X Y Z and A B C,
    L and R. Saturn: Bezel in Video, with the frames every console has and Your Own (`bezels/saturn/`).
  - A value kept in settings.ini stays: one chosen can't be told from a copy of an old default (a game's page copied
    every value from 00.58 to 00.67), so only what was never set starts this way; Reset All Settings starts every
    console so. The owner's file: the Master System and the Saturn get their bezels; his levels, 16:9 and bezels turned
    off stay.
  - **A bezel never covers the picture**: the built-in bezels' edges were soft (drawn at twice the size, then made
    smaller), so up to a tenth of their colour (alpha 14 to 27 of 255) lay over the picture's outer 1-2 pixels on every
    side. The window is now see-through all the way to its edge where it counts as see-through (src/Emulator.cpp
    LoadBezel: alpha under 128; the Retro TV's rounded corners stay). PC (scratchpad wsl/ui-0069/bezel-check.py, the
    window found and the picture placed as the app does it): every console's own bezel, each shape of its picture at
    Auto (28 pictures), 0 pixels of the bezel over the picture.
- **3DS: one screen above the other, the touch screen below** (the owner: "la 3DS que parta con una pantalla arriba y
  otra abajo, la táctil abajo"). One Above the Other has been the default since 00.64, but a view kept in settings.ini
  stayed (00.62's and 00.63's Side by Side copied into a console's settings, or a view chosen in the in-game menu): every
  3DS Screens value kept - All Games, This Console, This Game - becomes One Above the Other, once ([global]
  3ds_stack=00.69); a view chosen after it stays. The log says where: `settings: 3DS Screens set to One Above the Other,
  the touch screen below, once (00.69): console:3ds (side), ...` (the owner's file: his 3DS's Side by Side, and a copy in
  Resident Evil Code: Veronica's settings).
- **Quality: each level clearly sharper than the one below, on every console but the 3DS** (the owner: "quiero que de
  verdad se sienta la subida de calidad: baja, media, alta y ultra"):

  | Console | Low | Medium | High | Ultra |
  |---|---|---|---|---|
  | 2D consoles, Arcade, DS, Saturn | its own pixels | Smooth Edges | Pixel Art HD (xBRZ at the TV's resolution) | AI (xBRZ, then Anime4K) |
  | N64 | 1x | 2x, Smooth Edges | 4x, Smooth Edges | 4x, sharpened (Natural HD) |
  | PlayStation | 1x | 2x, xBR textures, true colour, Smooth Edges | 3x, also steady 3D (PGXP) | 4x, also 4x MSAA, sharpened |
  | PSP | 1x | 2x, textures 8x sharp, Smooth Edges | 3x, 16x, Smooth Edges | 4x, 16x, sharpened |
  | Dreamcast, NAOMI, Atomiswave | 1x, textures 4x sharp | 2x, 8x, Smooth Edges | 3x, 8x, Smooth Edges | 3x, 16x, sharpened |

  - From 00.40 to 00.68 Medium, High and Ultra were Smooth Edges, Natural HD and Natural AI, three looks hard to tell
    apart (PC, the filter harness, the mean change per pixel of 255: Medium to High 0.8-2.1 and High to Ultra 0.7-2.3 on
    the 2D consoles' pictures; now 1.8-3.7 and 1.6-4.6), and on the 3D consoles' HD pictures Natural AI was softer than
    High's Natural HD: its network works at 768x576 to 960x720, so it made a 4x picture smaller (Tekken 3 at 4x: 6.4
    of fine detail with it against 14.4 sharpened; GoldenEye at 2x 2.3 against 4.4). Ultra is now always the sharpest.
  - For a 3D picture drawn by the GPU (the N64, the PlayStation, the PSP, Flycast's consoles) the GPU Upscaler's Pixel
    Art HD and AI - the levels' High and Ultra - are Smooth Edges and Natural HD; Natural AI on a picture drawn bigger
    than 960x720 is Natural HD too (the PSP's at 4x since 00.46); Smooth HD on the N64 at 4x and on Flycast at 3x only
    sharpens (src/Emulator.cpp GpuMode). The PlayStation's High is 3x (4x is Ultra's now), its Ultra sharpened (Natural
    AI until now).
  - Flycast's consoles have the GPU Upscaler too (none from 00.53 to 00.68): Medium and High a Catmull-Rom picture, Ultra
    sharpened; a vertical NAOMI or Atomiswave game none (the filters don't turn a picture; Tube TV does).
  - The PS4's GPU carries them: in 00.68's log it was 5-31% busy in every game (the Dreamcast at 3x 12-31% at full
    speed, Natural AI 2.2-2.4 ms a frame on the Saturn and the NES); Pixel Art HD is two passes, the AI's chain as long
    as Natural AI's, the sharpening one pass. The PS4's `gpu filter: ... ms of GPU per frame` lines will show it.
  - A level chosen before is still that level, with its new look, once ([global] levels=00.69): for each console whose
    games read a section, the values of that console's part of a level of 00.68 it holds (its size among them) become the
    level's new ones; a Dreamcast's, NAOMI's or Atomiswave's section gets its level's GPU Upscaler where it had none; a
    value changed by hand (that console no level) stays. The owner's file: his consoles at Ultra (Super Nintendo, NES,
    Game Boy Color, PlayStation, DS, PSP, Dreamcast, NAOMI, Atomiswave, SG-1000, Game Gear, Genesis, Arcade) get the new
    Ultra, the N64 stays at Medium, the Game Boy and the Saturn at Low, the Game Boy Advance's AI chosen by hand now
    reads Ultra.
  - The levels' texts say what each one is on every console (High is "High (HD)" now); the Spanish in es.ini.
  - PC: scratchpad wsl/ui-0069/test0069.sh (Settings.cpp with the Saturn and the 3DS, with the 3DS, without: 1184, 769
    and 757 checks): the defaults of a new install and after Reset All, every console's four levels (each a step up,
    the N64's 2D Exact, Flycast no 4x and per triangle, the PSP's textures as they are), the files of 00.68, v00.21 and
    v00.19 and a PSP changed by hand, the 3DS's layout and its log line, the owner's file, every text's Spanish; the
    ui-0062 tests (641 and 627 checks); the filter harness on lavapipe (0 validation errors) for every console's four
    levels.
- **In-Game Menu: L1 + R1 or L2 + R2 held** (the owner: "agrégale mantener L2 y R2 y L1 y R1"; the idea of pull
  request #14 by danmoralesa33-prog): two more choices in Settings -> Controls -> In-Game Menu and in the in-game menu's
  Controls, held for a second as R3's and the touchpad's, on every console (the DS's and the 3DS's L and R too). As 00.68's
  choices: L3 + R3 always open the menu too, the chosen buttons close it, the start's hint shows them ("L1 + R1 (hold)"),
  a hold counts again only once its buttons were let go, and back in the game the pad is the game's once every button
  is up. The game sees the buttons for that second: their help says a game that holds both (a racing game's L2 and R2)
  or R2's Fast Forward can clash with them. PC: scratchpad wsl/ui-0068/unit.sh, 103 checks (each console, the second's
  timing, one button alone, one let go in between, a hold after the menu closed, L3 + R3).
- **Saturn: faster, and the stutter gone** (the owner, 00.68: "en la Saturn anda mal el rendimiento, a veces va a
  tirones"). His log of Sonic R and Daytona USA: the core took 15-21 ms a frame of 16.7 (the game at 79-98% of its speed,
  its sound stretched up to 31%); the app's heavy mode came and went every few seconds, and each "caught up again" ran 2
  or 3 frames in one pass (passes of 36-56 ms: the stutter); 558 of 600 pictures came a frame late and the others not,
  and each change between the two showed a picture twice and never the next one; Daytona USA's VDP1 thread took
  13.5-17.8 ms a frame (its 3D: a drawing every third frame, about 50 ms each), so the emulation waited up to 6.8 ms a
  frame for it; the VDP2's threads took 15 and 13 ms; the `speed:` line was there only while a GPU filter was on.
  - **The SH-2s' waits counted, not run** (scripts/build-saturn-core.sh: sh2int.c, memory.c). Most of their instructions
    are waits: Daytona USA's master SH-2 spends 57% of its own in a loop of 21 NOPs around a flag, its slave 77% reading
    its FRT's flag until the master starts it (the SGL's slave loop); Sonic R's master and slave 67% and 68% in loops of
    4 instructions (the PC: every instruction's address counted over 600 frames of their races). Upstream's idle
    detection (14 instructions at most, ending in a branch back) missed all of them. At each slice's start the
    interpreter watches up to 64 instructions: back where the slice started with every register as it was, having
    written nothing and read only work RAM or the FRT's registers (memory.c counts every other access), each further
    turn of that loop is the same turn - nothing it reads can change within a slice: the other SH-2, the DMA, the
    interrupts and the timers run between slices - so all but the last are counted (their cycles added), not run; the
    last one and the slice's end run as before. In the races: half of the master's cycles and 64% of the slave's in
    Sonic R, 58% of the slave's in Daytona USA. The file /data/EmuSuNi/saturn-sh2-skip-off (any content) runs every turn
    as 00.68 did; the log says which (`saturn: the SH-2s' waits counted, not run`) and how much, every 600 frames
    (`saturn: SH-2 waits: 245k cycles a frame of the master's and 305k of the slave's counted, not run`).
  - **The VDP1's drawing in half the work, on two threads**: upstream's line drawing (drawQuad, DrawLine, getpixel,
    putpixel: a function call, a division and the command's decoding at every pixel) as one inlined path - the command
    decoded once, untextured polygons without a texture read, unshaded ones without the gouraud sums, the lines' lengths
    without walking them, lines outside the system clipping left out - and two threads ("vdp1" on core 4, "vdp1b" on core
    0), each walking the whole command list and drawing its own band of the frame buffer's rows (the boundary follows the
    work): the same pixels, in the same order in each row.
  - **The VDP2's picture**: the layers erased after their composition, band by band and only those drawn into (titan
    erased all six whole, 3.8 MiB a frame at 352x224, on one thread while the other waited); the back screen's lines
    filled only when their colour changes; the window tests answered at once when the windows are off (most games:
    upstream worked out both windows and read the sprite window's mask at every pixel of every layer); the scroll screens'
    pixel stores without two function calls.
  - **The renderer's copies**: only the 4 KiB pages of the VDP1's and the VDP2's memory written since the last copy (512
    KiB each time before: 0.3-0.4 ms a frame of the emulation thread on the PS4).
  - **PGO** (as the Dreamcast's and the 3DS's): scripts/saturn-core.profdata.xz, a profile of the PC build running both
    games from the BIOS through their demo races (`SATURN_CORE_PGO=off` builds without it).
  - **The renderer's floating point as written** (vidsoft.c and titan.c without -ffast-math): its gouraud and texture sums
    are no longer reordered, so its pictures don't depend on how the compiler lays the code out (two builds of the same
    drawing differed by a colour step in 51 of 1,500 Sonic R pictures); some shading differs from 00.68's by a step.
  - **Pictures shown evenly**: the frame's own picture when its drawing is done in time, else the one before it - and once
    one was late, the one before at every frame until the frame's own would have been in time for 10 s in a row (20 s,
    40 s, at most a minute after each later switch): a picture is shown twice, or one jumped over, only at a switch. The
    drawing beside the emulation (a frame of latency) is always on for the Saturn: it came and went with the core's time,
    each change a picture twice or one jumped.
  - **One frame a pass, always** (src/Emulator.cpp): a frame longer than a refresh makes the game that much slower (its
    sound keeps its pitch: GameAudio stretches it) and is never made up for in a burst of 2 or 3 frames in one pass.
    Fast Forward and Frame Rate Limit off as before.
  - **The game's end**: the renderer's threads finish their drawing before the core frees, loads, saves or resets what
    they read (the game's end freed the VDP2's memory while its thread could still draw the last picture: the PC's slow
    instrumented build crashed there; a picture takes 13 ms on the PS4).
  - **The log**: the `speed:` line every 10 s for the Saturn too; `saturn: renderer:` has the VDP1's two threads, the
    pictures shown a frame later and the switches; `saturn: SH-2 waits:` every 600 frames; the memory check counts the
    fifth thread (11 MiB).
  - PC (WSL on 4 cores with SMT; each thread's CPU time a frame in the demo races, two interleaved runs, 00.68 / 00.69):
    Daytona USA's emulation 2.20 / 1.74 ms (-21%), its VDP1 1.88 / 0.49 + 0.49, its VDP2 1.67 + 1.48 / 1.53 + 1.48, the
    emulation's waits for the VDP1 0.07 / 0; Sonic R's emulation 2.57 / 1.52 (-41%), its VDP1 1.25 / 0.46 + 0.45, its
    VDP2 2.10 + 1.89 / 1.67 + 1.63, the emulation's waits for the VDP2 0.32 / 0.09.
  - On the PS4, at 00.65's 7.2 times the PC for the emulation and 00.68's 8.5 for the renderer's threads (estimates):
    Daytona USA's emulation about 12.6 ms a frame, its VDP1 4 ms on each thread, its VDP2 13.0 and 12.6; Sonic R's 11.0,
    4 and 14.2 and 13.8 - each under the 16.7 of a frame. The `speed:`, `saturn: renderer:`, `saturn: SH-2 waits:` and
    `pacing:` lines will say how it really goes.
  - Checked on the PC: every picture of both games' runs (Daytona USA 4,200 frames, Sonic R 6,600) hashed and compared
    with 00.68's renderer (built without -ffast-math) in a test mode where the emulation waits for the 68000's thread
    at every tenth of a line: identical. (The core is otherwise timing-dependent - the 68000 runs on its own thread: 00.68
    itself differed in 212 of 2,000 Daytona USA pictures with that thread on the emulation's CPU; in the normal mode
    Sonic R's pictures are identical, Daytona USA's within that variation.)
- **Dreamcast, NAOMI and Atomiswave: the SH4's waits let go by** (the owner: "Dreamcast sigue mejorándolo por favor, al
  igual que NAOMI y Atomiswave"; players: Daytona USA still slow, House of the Dead 2 lags with its sound slowed, Soul
  Reaver and Crazy Taxi 2 lag a little). A PC profile with each piece of recompiled code named by its SH4 address
  showed where the emulation thread's time went: the games' libraries waiting for the next frame. Sega's (Crazy Taxi,
  Daytona USA, Jet Set Radio) calls an empty function and reads four words of memory until the vblank's interrupt
  changes them - 25-32% of the emulation thread in Crazy Taxi's and Daytona USA's races, the SH4 itself waiting 54-64%
  of its time there; another library's (Marvel vs. Capcom 2, Virtua Tennis 2) counts a word down as it waits - 30-52%;
  the Atomiswave's BIOS polls a register while it boots.
  - A loop that comes round to the same registers with nothing changed in between (no block of it writes memory, no
    event, interrupt or read of a register that changes by itself) would go on the same way until the next event: that
    time now goes by without running it, slice by slice as the scheduler counts it, and the loop looks again after the
    event. The counted wait is recognised by its code, word for word; the rounds that fit before the next event go by
    and its count is counted down by them (never to 0: it ends by itself).
  - PC, the emulation thread a frame, 00.68 -> 00.69 (with the change below): Crazy Taxi's race 2.46 -> 1.65 ms (-33%),
    Daytona USA's 2.86 -> 2.19 (-23%), Marvel vs. Capcom 2's fight 2.41 -> 1.49 (-38%), the NAOMI's Marvel vs. Capcom 2
    2.14 -> 0.99 (-54%), Virtua Tennis 2 2.45 -> 1.04 (-58%), Dolphin Blue 2.46 -> 1.78 (-28%), KOF NeoWave's start
    2.69 -> 2.37 (-12%), Jet Set Radio's start 3.60 -> 1.48 (-59%, still 2 of the console's frames a picture). The
    pictures and the sound the same to the bit (cold boots with the renderer on the emulation's thread: Crazy Taxi 36
    pictures, MvC2 30, Daytona USA 60, Dolphin Blue 28, Virtua Tennis 2 34, the NAOMI's MvC2 34, and their whole sound).
    The PS4 should see the same shares: Daytona USA's race took 16.7 ms a frame there in 00.68 (97% speed), Virtua
    Tennis 2 18.5 (90%).
  - The log's 10 s line `dc: the SH4 waited in a loop N% of the console's time` says how much went by this way. Off
    with the file /data/EmuSuNi/dc-idle-off.
- **Dreamcast, NAOMI and Atomiswave: the SH4's memory accesses in its compiled code**: a read or a write whose address
  isn't known when the code is compiled called a 4-instruction function of the fast memory and came back (the call and
  the return 5-15% of the emulation thread: Virtua Tennis 2 and MvC2 15%); those 4 instructions are in the code itself
  now. An access to the hardware's addresses still faults once and becomes the call of its handler, as before. PC:
  3-8% less of the emulation thread. Off with the file /data/EmuSuNi/dc-inlinemem-off.
- **Dreamcast, NAOMI and Atomiswave: the renderer's descriptor sets**: every textured polygon got a Vulkan descriptor
  set of its own each frame (allocated, written, two heap allocations), though a frame's polygons share far fewer
  textures; polygons with the same texture and sampler share one now, and one already bound isn't bound again (the
  same picture). PC, the renderer thread: Daytona USA's race 1,690 draws a frame, its drawing 0.71 -> 0.53 ms, the
  thread -19%; Virtua Tennis 2 -20%, Crazy Taxi -11%.
- **Dreamcast, NAOMI and Atomiswave: the console kept to the game's speed when it draws nothing**: with Flycast's
  threaded rendering only the renderer held the emulation back, so a loading screen, a menu between two pictures or a
  board's BIOS ran as fast as the PS4 could, and its sound too fast for the app: Daytona USA's menus sent 107-113% of
  the sound's rate in 00.68's log (the app's audio cut it down), Virtua Tennis 2's demo 111-124%; on the PC a NAOMI's or
  an Atomiswave's boot ran 4-8 times faster than real and dropped up to 37% of its sound. The app gives Flycast the
  frames a second it runs the console at (Speed and Fast Forward included), and the emulation sleeps at a frame that is
  more than 3 frames ahead of it: Dolphin Blue's boot now 4.6 s with all its sound, as on the board.
- **Dreamcast, NAOMI and Atomiswave: no more full recompiles when the code caches fill**: the SH4's 20 MiB and the
  ARM7's 8 MiB, twice Flycast's (00.68's log: "dc sh4 (10.0 MiB of 11.0 MiB written), dc arm7 (4.0 MiB of 4.0 MiB
  written)" after Dolphin Blue, Jet Set Radio, Daytona USA and Virtua Tennis 2: a full cache drops all its code and
  compiles it again as it runs). The same 32 MiB JIT arena as before: no memory more.
- **Atomiswave: the cartridge's decryption by tables** (its two bit permutations per 16-bit word were loops: 9% of the
  emulation thread while an Atomiswave game boots); the same words to the bit.
- **Dreamcast log: the renderer's parts**: a 10 s line `dc renderer per frame`: its draws' commands and how many, the
  presenting, the queue's submits and the waits for the app's lock of the queue. Daytona USA's renderer took 14.9 ms a
  frame at Ultra (3x) in 00.68's log, only 2.1 of it the TA lists and textures, its GPU waits 0.01: the rest will show.
- **3DS: the code a game ran before is compiled ahead at its next start (the JIT warm-up)** (the owner: the 3DS is
  still slow and stutters; 00.68's log: its stops in Super Mario 3D Land's loads and in Driver Renegade 3D's and
  Zelda's new areas are dynarmic compiling ARM11 code met for the first time - Mario's first 600 frames compiled
  61,324 blocks in 5.1 s, 83 µs a block, frames of 100-663 ms - and every start compiles the same code again). The
  blocks of the game's own code that dynarmic compiled, and its memory accesses there that faulted with the fast memory
  (8,965 faults in Mario's session, each a recompile), are kept by location (no code) in
  saves/3ds/Azahar/shaders/emusuni-jit/<program id>.bin (Mario's: 628 KiB), written every minute while they grow (on a
  thread of its own) and when the game ends. At the next start the accesses are marked first and the blocks compiled
  again in the order the game met them, ahead of it: 0.4 s in the first frame, 12 ms a frame in the next 120 (the
  boot's light frames), then 6 ms a frame until the list is done. Only the game's code segment (its address, size and
  hash are in the file: a file made for other code - another version, an update - starts again) and core 0 (Mario
  compiles nothing on core 1, Zelda 8%); 8 MiB of the code cache (24 MiB a core, 12 in New 3DS Mode) stay free for
  what the game meets next. A game's first session is as before; from its second one the boot, the menus and every
  area played before don't stop to compile. The log: `EmuSuNi: JIT warm-up: N blocks of this game's code from earlier
  sessions to compile, N of their memory accesses outside the fast memory`, `... N blocks compiled in N ms over N
  frames`, `... N blocks and N accesses kept for the next session`. The file /data/EmuSuNi/3ds-jit-warmup-off (any
  content) turns it off.
  - PC (00.69's core, a cold start with the app's options, 3,000 frames, then the same again in the same save folder):
    Super Mario 3D Land's second start compiled its 63,991 blocks in 430 ms over its first 4 frames, then 19 ms of
    compiling in the other 2,996 (the first start: 430 ms spread over its loads; 00.68's: 733 ms); its slowest frame
    after the start 88 -> 19 ms, the fast memory's faults 8,213 -> 58. Hey! Pikmin: 40,978 blocks in 259 ms in the
    first frame, then 5 ms; its slowest frame 184 -> 13 ms; faults 1,425 -> 342.
- **3DS: dynarmic compiles 40% faster, the same code.** scripts/build-3ds-core.sh; each change leaves the x86 code
  dynarmic writes as it was (on the PC a cold start's first 600 frames of Super Mario 3D Land and Hey! Pikmin compile
  the same blocks into the same 9,312,940 and 6,193,567 bytes as 00.68's, the first 5,000 blocks to the same sizes
  in the same order):
  - the ARM decoder searched the VFP table (62 patterns) and the ASIMD one (147: NEON, which the ARM11 hasn't) one
    pattern after another for every instruction: now only where their patterns can match (checked against them);
  - the register allocator released its 102 places after every IR instruction (now only the ones used), scanned them
    all for every value it defined (an assertion: dynarmic's are always on), copied its candidate registers into a new
    vector for every register it chose and made a one-element vector for every fixed one;
  - the heap: 48 allocations a block -> 13 (the allocator's places, value lists and register orders; the IR pool's
    room kept for the next block; the identity pass's list; room for the deferred code; the blocks' ranges for
    invalidation in 4 KiB page buckets instead of an interval map of sets; Xbyak's label tables' nodes from free lists
    of the thread; the perf map's name of each block only when the perf map is on); on the PS4 every malloc and free
    takes the app's heap mutex;
  - the ARM decoder's 4096 buckets point to its matchers instead of copying them: 1.07 million allocations less at
    the first block of the first 3DS game.
  - The PGO profile (scripts/3ds-core.profdata.xz) made again on 00.69's code: the starts of Super Mario 3D Land, Hey!
    Pikmin, The Legend of Zelda: Ocarina of Time 3D and Driver Renegade 3D, 3,000 frames each, twice (the second with
    the warm-up's list).
  - PC (each version's core with its own profile; the stall log's `JIT compiling`): a cold start's first 600 frames,
    the same blocks, Super Mario 3D Land 519 -> 296 ms, Hey! Pikmin 337 -> 203 ms; their 3,000 frames 733 -> 430 and
    449 -> 269 ms. The emulation thread's time a frame from a saved state (3 interleaved rounds, the warm-up off):
    Mario's World 1-1 1.685 -> 1.640 ms, Pikmin's garden 2.303 -> 2.196 ms (each round faster).
- **3DS: the games' file reads on two threads made once.** Azahar made a thread for every file request its cache
  didn't answer: 1,449 in a Driver Renegade 3D session of 00.68's log (758 in 3,000 frames on the PC, now 2). The
  game's end (a reset, a state loaded) waits for the requests under way first, as it waited for those threads.
- **3DS: textures decoded faster.** ETC1 textures (00.68's log: Driver Renegade 3D decoded 4.4 MB in one 270 ms
  frame, mostly ETC1) a 4x4 subtile at a time: its base colours and tables worked out once, not for each pixel (the
  same pixels: 64 million checked against the old decoder; PC: a fifth faster). And every texture is decoded into
  ordinary memory, then copied in one pass into the staging buffer, which is write-combined memory on the PS4 (the
  tiled decoders wrote it 32 bytes of a row at a time). The same bytes.

## 00.68 (test build)
- **Sega Saturn, experimental: a tab of its own, the games with their picture** (only in builds made with
  `scripts/build.sh with-saturn`; the owner: "a Saturn tab where Daytona USA and Sonic R show a picture and play, even
  if it's slower"). YabaSanshiro 3.4.2, 00.65's speed test's core, now with its software renderer (vidsoft): its OpenGL
  one needs OpenGL, which the PS4 has only through Mesa's Zink and a port of the renderer (8-13 days by the study).
  - The fastest way, measured on the PC (the PS4 is 7.2-7.3 times slower, 00.65's test): on the emulation thread the
    renderer's work is 2.2 times the emulation's own (Daytona USA's demo race: the emulation 2.1 ms a frame, the
    VDP1's drawing 1.9, the VDP2's screens and composition 3.0; Sonic R's: 2.5, 1.2 and 3.9), about 52 ms a frame on
    the PS4: a third of full speed. So it draws on three threads of its own beside the emulation thread: the VDP1's
    drawing (core 4) from a copy of the VDP1's memory taken where the core draws, while the emulation thread walks the
    same command list without drawing (the VDP1's registers and status as when it draws); the VDP2's screens, sprite
    layer, composition and picture (cores 5 and 1, the lines in two bands: each thread half of every layer and of the
    composition) from a copy of the VDP2's state at the frame's VBlank OUT. The emulation waits for the VDP1 only where
    it sees its drawing (at the frame buffers' change the picture waits instead, drawing the scroll screens meanwhile:
    Sonic R starts each drawing just before the change) and for the VDP2 only when a picture takes longer than a frame
    (at VBlank IN it shows the frame's picture if done, else the one before). The pictures are the same, pixel for
    pixel, as the renderer's alone on the emulation thread (19 frames of both games compared). PC, the demo races:
    Daytona USA's emulation thread 2.24 ms a frame (2.10 without a picture), 2.45 ms with its waits, its renderer threads
    1.7-2.2 (the VDP1's), 1.7 and 1.5 (the VDP2's); Sonic R's 2.55 (2.54), 3.0 with its waits (the VDP2's thread waits
    for its drawings), 1.1-1.4, 2.1 and 1.9. On the PS4, at 00.65's 7.2 times the PC: Daytona USA about 17-18 ms a
    frame (full speed is 16.7: about 95%), Sonic R about 21-22 (about 78%) - to be measured on the PS4 (the log's
    "saturn: renderer" lines every 600 frames: each thread's time and the waits).
  - Its pictures are the OpenGL build's on the same frames (Daytona USA's demo race with its cars, the rankings over
    it; Sonic R's Resort Island) after two fixes of the renderer for YabaSanshiro: the VDP1's frame buffers erased and
    changed at VBlank OUT as YabaSanshiro does it for its OpenGL renderer (the software one changed them itself at
    VBlank IN, which a manual change never reached: Daytona USA showed no 3D at all), and the colour RAM read from the
    copy it draws from (it read the OpenGL renderer's, the colour RAM's address passed as a transparency). Its own
    twelve threads (they waited for a wake that never comes) are never started; what it makes it frees at the game's
    end (its compositor's 11 MiB were kept, and used freed by the next game).
  - The console: the tab "Saturn" (Roms/saturn on the USB drive); .cue (with its .bin tracks), .chd, .iso, .ccd, .mds,
    one disc each (a .cue, .iso or .mds in a folder shared with the PlayStation is the Saturn's when its first sector
    says SEGA SEGASATURN); the BIOS in /data/EmuSuNi/system/saturn (or beside the games: the owner's "Sega Saturn BIOS
    (USA).bin" keeps its name, found as the speed test finds it; none: the game says where it goes); the pad as the
    Mega Drive's (A B C on Square, Cross, Circle; X Y Z on L1, Triangle, R1), its L and R on L2 and R2, START on
    Options, Fast Forward on the touchpad; the games' saves in saves/saturn (the console's backup memory), states
    (Continue after the first frame: before, the core says it took a state it didn't); the drawing beside the
    emulation (cores 0 and 6), the emulation thread on core 2; the memory check counts its four threads (9 MiB). Clean
    Up Games no longer takes a game's discs for copies of each other.
- **Covers for numbered file names** (GitHub issue #7: "my ROMs were numbered, 01 Comix zone, 02 sonic 2, and the
  downloader gave them no covers"): a name with a list's numbering in front (`01 `, `01 - `, `1. `, `[01] `, `(01) `,
  `#01 `, `001_`) is looked for as it is by its same name only, then without the numbering, then as it is in every way.
  Before, the number was taken for the title's own (a sequel's), so no match was ever sure: of the owner's 783 Super
  Nintendo files (his USB stick's names) numbered `01 ...`, 103 got a cover against 774 as named; now 774 with every
  numbering, each the same picture as its name without it (the N64's 239: 38 -> 237, the NES's 840: 41 -> 822). A number
  of the title ("3 Ninjas Kick Back", "2020 Super Baseball", "1942", "10-Yard Fight", "7th Saga") is found as before:
  the owner's 1,862 names as named keep every picture they had. The issue's names: `01 Comix zone` -> Comix Zone (USA),
  `02 sonic 2` -> Sonic The Hedgehog 2 (USA) (`03 sonic 7`: no such game). Find Cover's first, sure suggestion the same
  way. The server's names of the project's links (`... - SymbolicLink`, the same picture) come after the picture's own.
- **Why a game has no cover, on screen**: Games Without Cover says it under its list for the selected game: no picture
  in libretro's list has a name like it; the closest picture isn't sure (and which one); libretro's list or the picture
  couldn't be downloaded, and why in plain words (no internet connection, the server didn't answer or took too long,
  the connection was cut, the server refused it or had a problem); or it couldn't be saved. Download Covers' result
  gives the first failure's reason the same way (curl's own words stay in the log).
- **The cheat collection's download** (two users: "it gives an error"): reproduced on the PC with the app's own network
  code (Net.cpp and CheatDownload.cpp with curl): libretro's server answers well over plain HTTP (cheats.zip, 36,872,908
  bytes, through Cloudflare: 2.8 s here, about 30 s on the owner's PS4 in 00.38's log), and its folders are the
  consoles' own names; the failure is on the player's side, and every cause read the same ("couldn't be downloaded",
  "Check the PS4's internet connection"). Now there is no limit for the whole download (600 s stopped any connection
  under 62 KB/s: the collection is 35 MB), only for a minute without a byte; a cut, a server's error or no answer is
  tried twice more; what came must be the zip, all of it (a network's sign-in page used to be kept as the collection,
  then "can't be read"); a write that failed (the storage full) used to read as a cancel. The page says why (no internet
  connection, the server didn't answer, took too long, refused it or had a problem, the connection was cut, what came
  isn't the collection - does the network ask to sign in? -, the PS4's storage may be full, about 40 MB) and the
  progress ("12 of 35 MB"); the log the address, the file, the HTTP status, the bytes and curl's error. PC test:
  libretro's server, then a stand-in for each failure (a slow line, a sign-in page, 403, 503, a cut, a minute's stall,
  no name lookup, a full storage, a cut with a collection of before kept): 16 checks.
- **The in-game menu's buttons** (GitHub issue #5): Settings -> Controls -> In-Game Menu (and the in-game menu's own
  Controls page), for every game: L3 + R3 (as before), R3 held a second, the touchpad held a second, or Options +
  Touchpad (START + SELECT on most consoles). The PS button is the PS4's own. L3 + R3 always open the menu too (a choice
  can't leave a player without it), and the menu closes with its buttons as with Circle; the start's hint shows the
  ones chosen. On the DS and the 3DS the touchpad is the touch screen: a touchpad's choice is L3 + R3 there, and with R3
  held their Swap Screens is R3 let go before the second (the hold opens the menu without a swap). A hold counts again
  only once its buttons were let go; back in the game player 1's pad is the game's once every button is up, as before.
  PC test: each choice's timing, a hold after the menu closed, the DS's and the 3DS's.
- **PSP stick: dead zone and sensitivity** (GitHub issue #8: "pushed all the way, the character only walks"): PSP Stick
  Dead Zone (0% to 30%; 15% as before) and PSP Stick Sensitivity (100% to 150%: the PSP's edge sooner, for a worn stick
  or a game that wants it all the way) in Emulation, per game too, changed at once: PPSSPP's own dead zone and scale,
  fixed at 0.15 and 1.00 until now. PC test: every value is one of the core's own.
- **N64: Cruis'n USA and Resident Evil 2 no longer go black** (GitHub issue #1: "both games enter but after some
  screens black screen, even at 240p"). Cruis'n USA stayed black after START at its Controller Pak notice, Resident
  Evil 2 after its language screen: the game went on sending sound tasks but never another picture, waiting for
  something the RDP drew (Cruis'n USA's CPU reads its own picture back - its transitions shatter it). N64 Graphics Sync
  Fast (the default) lets ParaLLEl-RDP draw beside the N64 and hands the game the RDP's "done" interrupt at once, and
  on the PS4 the memory the GPU draws into is a copy of the N64's (no VK_EXT_external_memory_host) that reaches the
  CPU's a frame later. Both games now play with Accurate whatever the setting says (src/Emulator.cpp N64AccurateGame,
  by the cartridge header's game code: every region and revision), and the log says so (`n64: Resident Evil 2 plays
  with N64 Graphics Sync Accurate whatever the setting ...`); the setting's help names them. PC (the core with the
  app's options and the owner's Europe cartridges, lavapipe, 1x; scratchpad n64-0068): with Fast, Cruis'n USA black
  from START (frame 405) to the end of the run (5000) and Resident Evil 2 from its START (1505) to 5000; the same with
  ParaLLEl-RSP doing every task (so not the HLE sound or video); with the null GPU (nothing drawn, nothing copied back)
  Cruis'n USA plays on. With Accurate, Cruis'n USA plays its Nintendo and Williams logos, its intro and its title,
  Resident Evil 2 its warning, logos, title menu and saved games screen. Cruis'n World plays with Fast (to its car
  select) and isn't listed. With Accurate the N64 waits for the GPU at every finished picture (Resident Evil 2 makes
  4 a frame, one per graphics task; Cruis'n USA one every 2 or 3 frames): the PS4's `speed:` line will show what that
  costs.
- **N64 log: a black screen shows in it.** The `n64 core per frame` line also counts the graphics and sound tasks a
  frame and gives the longest graphics task (`graphics tasks (RSP) N ms (N tasks, the longest N ms), sound tasks N ms
  (N)`: a game drawing at 30 FPS sends a graphics task every second frame; the longest is the heaviest frame, a
  stutter), and 10 s of sound tasks without a graphics task log `n64: no graphics task in these 10 s while the game's
  sound runs: the picture isn't changing (black or frozen?). If it stays so, try N64 Graphics Sync: Accurate.`: the
  next game that needs Accurate shows in its log (port/ps4_n64.c). A game's start logs its header's game code and its
  N64 Graphics Sync.
- **N64 Ultra: no more lines and squares over 2D backgrounds** (issue #1: Zelda: Ocarina of Time's houses, ISS 64's
  intro, "if I put Ultra it happens", with the AI filter off too). Ultra drew the N64's 2D as Smooth (HD), ParaLLEl-
  RDP's texture rectangles at 4x, which the core's own option text warns against ("should generally be rendered at
  native resolution to avoid seams"): backgrounds made of tiles showed their joins (PC, the core with the app's
  options, lavapipe, 4x: ISS 64's Konami logo crossed by a grid of lines with Smooth (HD), clean with Exact). Ultra's
  2D is Exact now, as the other levels'; Smooth (HD) stays in N64 2D Graphics, its help naming those games. An Ultra
  chosen before (4x, Smooth 2D and Natural AI, or 00.19's AI filter) stays Ultra with Exact 2D, once ([global]
  n64_2d_ultra=00.68); a Smooth 2D chosen by hand with other values stays. PC (scratchpad
  n64-0068\settings-test\run.sh, v00.21's Settings.cpp against this one): 00.67's and 00.19's Ultra files come out
  Ultra with Exact 2D for the N64, the PlayStation and the PSP; High unchanged; a Smooth 2D at Medium stays.
- **"Banjo-Kazooie is blurry however I set the graphics" (YouTube, since v00.21): opening a game's page no longer
  freezes its console's settings.** Since 00.58 a game's page (Triangle, or the in-game menu) starts at This Console,
  and opening it copied All Games' values into the console's own settings at once: from then on the main Settings'
  Quality no longer reached that console's games - its N64 games kept the native resolution of the Low they had when
  the page was opened, while Quality said Ultra. Now the page still shows This Console, but the console gets settings
  of its own only when one is changed there (src/Settings.cpp: Set, StartConsoleLocked); until then its games follow
  All Games. A console's settings that are still that copy - every value All Games' - follow All Games again, once
  ([global] console_copies=00.68); a console whose settings were changed keeps them. PC (scratchpad
  n64-0068\settings-test\scope2.sh): v00.21's code, a page opened and then Quality Ultra for All Games: the N64 game
  stays Low at 1x; now it gets Ultra at 4x; a change on the page still makes the console's own settings (High there,
  then Ultra for All Games: the N64 games keep High); This Console chosen in Apply To still copies at once; the
  owner's settings.ini keeps every console section. A copy that All Games has moved away from since stays (it can't
  be told from a console's own choice): a game's page (Triangle, Settings) changes it, or Apply To: All Games. An N64
  game's start logs where its picture comes from: `n64: picture settings from This Console: Quality low (All Games:
  ultra), N64 Resolution 1x, GPU Upscaler off, N64 2D native`.
- **Dreamcast: Jet Set Radio at its own speed, Frame Skip Auto included** (the owner, 00.67: it still sounds
  accelerated). 00.67 paced a 30 FPS game by Flycast's own detection (detect_vsync_swap_interval), and Flycast turns
  that off while its Auto frame skip is on - and the owner's Dreamcast settings (This Console, which Jet Set Radio and
  Daytona USA follow) have Frame Skip Auto. It never fired: the log has no 29.97 FPS line for Jet Set Radio (Daytona
  USA's two at its start, `29.970` then `59.945`, were its video mode being set), the core sent its sound at 63-86 kHz
  (144-196% of 44.1 kHz), and the speed line said 61-97%, because it counted Flycast's runs, each 2 of the console's
  frames. Now the app counts the frames itself:
  - The core tags every picture with the console's frame count of when the emulation sent it
    (scripts/build-dc-core.sh: flycast_emusuni_vblanks, carried on the renderer queue's messages). The count per
    picture is exact, whatever the threads' timing.
  - The pacing (src/Emulator.cpp: DcFrames) pays for the frames each picture covered: first the runs its pass still had
    planned, then a run left out of each pass that follows.
  - So a 30 FPS game runs every other refresh from its first picture on, with every Frame Skip (1, 2, Auto), Fast
    Forward (at the speed it says), Speed and Frame Rate Limit. Flycast's detection is off again.
  - The speed line counts the console's frames (the game's real speed), and the core's time per frame is per console
    frame (the heavy mode decides on it).
  - The log notes each new spacing: `pacing: Flycast's pictures are 2 of the console's frames apart now (was 1): a game
    at 30 FPS, or Frame Skip`.
  - PC, the core with the app's options, the console's frames of every picture (flycast_emusuni_vblanks): Crazy Taxi,
    Marvel vs. Capcom 2 and Daytona USA 1 (Daytona USA 1098 of 1100, one run without a new picture and one of 2 after
    it); Crazy Taxi with Frame Skip 1 2, with 2 3 (all 1100); Jet Set Radio from its disc to its tutorial 2 in 4394 of
    4400 (4 of 1 and 2 of 4 at the loads), with Auto frame skip the same (4393), with Frame Skip 1 4 in all 1900. The
    core's change draws the same pictures: the cold boots of Crazy Taxi and Marvel vs. Capcom 2 (threaded rendering off)
    identical to 00.67's (36 and 30 pictures).
  - The pacing with the app's real sound path (00.67's harness, scratchpad\dc0068\pacing, with this crediting; 30 s,
    V-Sync on, Flycast's threaded rendering modelled, a 12 ms renderer), 00.66 / 00.67 / 00.68:
    - a game at 30 FPS, 8 ms per console frame: 200% / 102% (100% after a 0.45 s switch) / 100% from the start;
    - at 15.5 ms per frame (Jet Set Radio's streets on the PS4, if it is 6.8 times slower than the PC as Daytona USA's
      race was): 107.5% / 100.2% / 100.0%;
    - 4 of the console's frames a picture (Jet Set Radio with Frame Skip 1): 00.66 400%, 00.68 100% (one run every
      4th refresh);
    - Fast Forward x4 on a 30 FPS game: 800% before, 400% now;
    - a 60 FPS game: unchanged.
    - The sound arrives at 44.1 kHz with nothing stretched or cut: no gaps, no clicks, the pitch within the rate
      control's 9 cents.
- **Dreamcast, NAOMI and Atomiswave: the Quality levels again** (the owner asked for Low, Medium, High and Ultra; since
  00.53 the menu said "Standard quality only"):

  | Level | Picture | Texture Sharpness |
  |---|---|---|
  | Low | 640x480 (00.67's picture: transparency per triangle) | 4x |
  | Medium | 2x, 1280x960 | 8x |
  | High | 3x, 1920x1440, made smaller for the 1080p TV | 8x |
  | Ultra | 3x | 16x |

  - No level puts a GPU filter over Flycast's picture (as before). There is no 4x: the PS4 shows 1080p, and 2560x1920
    made that small only shimmers more than 3x.
  - The size and the sharpness change at the next game start: Flycast makes its renderer again for a new size (in
    00.52, Crazy Taxi changed to 3x in the middle of a game made 0.6 pictures a second for 10 s).
  - Why the PS4 can carry them: its GPU was 8-13% busy at 1x in 00.67's logs (Daytona USA's race, Jet Set Radio). In
    00.52, Crazy Taxi at 3x with Smooth Edges kept it 4-12% busy, its speed limited by the CPU; 00.53's "slowed to a
    crawl" was the first 10 s after that change.
  - Dreamcast Resolution and Texture Sharpness are shown again, for This Console or This Game. Transparency stays per
    triangle (per pixel drew wrong colours on the PS4).
  - A level chosen before is the Dreamcast's too now: the levels had written its values since 00.48, unused. So All
    Games or This Console at Ultra plays Dreamcast games at 3x from now on, and a level shows as that level (not
    Custom), on the main Settings and on every console's page. High's texture sharpness is 8x now (16x is Ultra's): a
    section at High from before gets 8x once ([global] dc_levels=00.68), so it still shows High. The owner's settings
    (Low for All Games and for the Dreamcast) keep 00.67's picture.
  - The log notes what a game got: `dc: CPU clock 200 MHz (Dreamcast CPU Clock), sound DSP off; picture 1280x960,
    texture sharpness 8x (Quality); ...`.
  - The Spanish texts are in es.ini.
  - PC, the core with the app's options: Crazy Taxi, Marvel vs. Capcom 2 and Daytona USA at each level: the picture
    640x480, 1280x960 or 1920x1440 as chosen, no error, the 2D of Marvel vs. Capcom 2 without seams; the emulation
    thread's time per frame the same at every level (Crazy Taxi 2.53-2.57 ms, Marvel vs. Capcom 2 2.37-2.42, Daytona USA
    2.93-3.00): the levels are the GPU's work (which the PC's software GPU can't measure for the PS4).
- **3DS: no more stops for a new shader during play: asynchronous shaders by default** (the owner: the 3DS still
  looks slow and stuttery; 00.67's log of Super Mario 3D Land: once loaded it ran at full speed, the core 8-10 ms a
  frame, but stopped for 0.26-0.38 s whenever a new effect needed a Vulkan pipeline the driver's cache didn't have
  yet). Now a draw whose pipeline isn't built yet is skipped until the workers have built it - that object shows up a
  few frames late - instead of the whole game waiting, as Azahar does on Android; draws of 6 vertices or fewer (the
  2D parts) still wait. With nothing cached at all (the first time a scene is seen) some objects can be missing for
  a few seconds while their pipelines compile (PC, Super Mario 3D Land's intro: its castle and tree); the driver's
  cache, saved every 60 s, has them the next time. The file /data/EmuSuNi/3ds-sync-shaders (any content) waits for
  every pipeline as before. The log says which (`EmuSuNi: shaders: ...`). PC, a first start with nothing cached and
  pipelines as slow to compile as the PS4's (300 ms each, a test switch of the PC build): Super Mario 3D Land's
  stops 34.4 s -> 18.5 s, Hey! Pikmin's 10.2 s -> 5.5 s.
- **3DS: fast memory on by default** (00.66-00.67: only with the file /data/EmuSuNi/3ds-fastmem, which is no longer
  read). The file /data/EmuSuNi/3ds-fastmem-off (any content) turns it off. The self-test, the fallbacks and the
  marker files as before; the start marker /data/EmuSuNi/3ds-fastmem-trying now goes after 120 frames with 64 of the
  JIT's faults handled (or 1800 frames), or when the game ends normally: closing a game early no longer counts as a
  crash. The log: `EmuSuNi: fastmem: on for this game (by default)` with the 4 GiB range's line, or `EmuSuNi:
  fastmem: off (the file /data/EmuSuNi/3ds-fastmem-off): the page table`, or `EmuSuNi: fastmem: off: <why> (the page
  table instead)`; at a game's end what it did.
- **3DS: the stall log divides the emulation's own time.** 00.67's log had 35 frames of 100-683 ms in the game's loads
  with no pipeline, shader, texture, file or GPU wait behind them. Each slow frame and each 600-frame summary now starts
  with `guest code N ms, JIT compiling N ms (N blocks, N cache flushes), HLE N ms (N calls), GPU commands N ms
  (surfaces made N, N ms), audio N ms` (the CPU's time stamp counter, cheap enough for every HLE call: no measurable
  cost on the PC), and counts `draws skipped N` (async shaders): the next log says what the loads spend their time on.
- **3DS: a little less work per frame**: the audio's resampling over a contiguous copy instead of indexing a deque
  three times a sample (the same output bit for bit: 300,000 random cases against upstream's code, and the same hash of
  every sample over a scene), and dynarmic without its IR verification pass (assertions only) when it compiles a block.
  PC, the emulation thread's time per frame (3 rounds, medians, after the start's precompile): these alone -3.0% in
  World 1-1 and -3.6% in Pikmin's garden; 00.68 against 00.67 as each comes (with the fast memory): -6.8% and -15.6%.

## 00.67 (test build)
- **Dreamcast: a game at 30 FPS runs at its own speed, its pictures evenly spaced** (the owner: Jet Set Radio stutters,
  its sound stutters, and sometimes it plays faster). Jet Set Radio draws a picture every 2 of the console's frames,
  all of it (its start, menus, streets), and a run of Flycast is one picture: 2 console frames, with their sound (1471
  frames instead of 736). The app ran Flycast once per TV refresh. So wherever the PS4 could make a picture in less
  than a refresh, the game ran at up to twice its speed and sent its sound at up to twice its rate (00.65's ring
  overflowed; 00.66's stretcher left the extra sound out). Between that point and the PS4's limit, its pictures came
  1 or 2 refreshes apart. Frame Skip 1 and 2 did the same to every game Flycast runs (Dreamcast, NAOMI, Atomiswave):
  2 and 3 console frames a run, so the game got faster, not lighter. Now Flycast reports what a run is
  (src/Emulator.cpp: its option detect_vsync_swap_interval is on). It measures its runs by their sound and reports
  29.97 FPS through SET_SYSTEM_AV_INFO once 6 runs in a row agree, and 59.94 again the same way. The pacing then runs
  it every other refresh (s_game_fps). The sound is still sent at the core's 44.1 kHz, so its pitch is unchanged.
  The log notes each change: `game: the core runs at 29.973 FPS now (was 59.945)`. Frame Skip Auto is the exception:
  Flycast doesn't measure while its auto skip is on (the frames a run vary), so there a 30 FPS game is paced as
  before.
  - PC, the core with the app's options: Jet Set Radio reports 29.97 FPS at its 26th run from a save state and at its
    44th from the disc, and nothing else in the 4500 frames to its tutorial. Crazy Taxi, Marvel vs. Capcom 2 and
    Daytona USA (their starts from the disc, a race, a fight) report nothing. Frame Skip 1 reports 29.97, Frame Skip
    2 19.98, Jet Set Radio with Frame Skip 1 14.99; Auto reports nothing.
  - The pacing with the app's real sound path (scratchpad\dc0066\pacing: 00.66's audio harness with Flycast's
    threaded rendering and its detector added; a 30 FPS game, 30 s, V-Sync on, a 12 ms renderer), 00.66 -> 00.67:
    at 8 ms per console frame, speed 200% -> 100% and the sound sent at 88 -> 44.1 kHz (half of it left out
    -> none), from the detection's half second on;
    at 15.5 ms (Jet Set Radio's streets on the PS4, if it is 6.8 times slower than the PC as Daytona USA's race was
    in 00.61), 107.5% -> 100.2%; at 17.5 ms, 95.3% both. No gaps or clicks; the pitch stays within the rate
    control's +-9 cents. A 60 FPS game reports nothing and runs as before.
- **Dreamcast: faster.** On the PS4 the Dreamcast is limited by its emulation thread (Daytona USA's race ran at
  73-87% in 00.60/00.61: that thread busy 16-21 ms of a 16.7 ms frame, the GPU 6-10% busy). PC profiles of that
  thread: the recompiled SH4 code 60-73%, the sound chip 10-22% (Daytona USA's 22%), the SH4's vector maths
  2.7-6.8%. Four changes in the core (scripts/build-dc-core.sh), each keeping the same pictures and the same sound:
  - PGO: the Flycast core compiled with a profile of the PC build playing Crazy Taxi, Marvel vs. Capcom 2, Daytona
    USA, Street Fighter III: 3rd Strike and Jet Set Radio (their starts from the disc, races, a fight, Shibuya's
    streets, the attract demo; function counters only, nothing of the games): scripts/dc-core.profdata.xz, as the DS
    and 3DS cores have (DC_CORE_PGO=off builds without it).
  - FTRV and FIPR (a vector times a 4x4 matrix and a dot product, done for every vertex in 3D games) are now done by
    the SH4 recompiler in x86 inside the block, instead of a call of their C versions. The same double-precision
    products are summed in the same order and rounded the same way, so the results match to the bit (PC: 32
    million results against the C versions, in the four rounding modes with and without flushing denormals, NaNs
    and infinities included).
  - The recompiled code's memory accesses no longer save and restore the four float registers it keeps (xmm8-xmm11)
    around every call of a memory handler. The handlers that call C keep them themselves.
  - The sound chip steps only its playing channels. Before, it stepped all 64 at each of the 44,100 samples a
    second, and a silent one adds exactly nothing.
  - PC, the emulation thread's CPU time per picture (1500 after 300 frames, 3 interleaved runs), 00.66 -> 00.67:
    Daytona USA's race 3.085 -> 2.839 ms (-8.0%), Crazy Taxi's race 2.656 -> 2.428 (-8.6%), Marvel vs. Capcom 2's
    fight 2.402 -> 2.275 (-5.3%), Jet Set Radio's streets 5.180 -> 4.547 (-12.2%). A Jet Set Radio picture is 2
    console frames, so that is 2.3 ms a frame, the lightest of the four. PGO alone gives -1.8 to -2.8%; the
    renderer's thread is -2 to -4% (PGO). The PS4's gain is to be measured from its log; at Daytona USA's 6.8 times,
    Jet Set Radio's streets would go from 35 to 31 ms per picture (of 33.4).
  - Pictures (threaded rendering off; cold boots of Crazy Taxi 900 frames, Marvel vs. Capcom 2 1500, Daytona USA
    3000 to its race, Jet Set Radio 4500 to its tutorial with the console's clock and its system files held):
    identical to 00.66's.
  - Switches for a game that misbehaves, without a new build: /data/EmuSuNi/dc-vector-off and dc-memcalls-off
    (alongside 00.60's dc-fastmem-off, dc-dispatch-off and dc-hotpages-off). The log states each:
    `SH4 recompiler: FTRV and FIPR in x86 on`, `memory calls without saving xmm8-xmm11 on`.
  - For Jet Set Radio (PC, its streets, the emulation thread per picture): Dreamcast CPU Clock 90% saves 3.8%, 80%
    9.8%, 70% 15.2% (still a picture every 2 frames at 70%, so the game keeps its speed there). Dreamcast Sound DSP
    on costs 9% (the sound chip 0.35 -> 0.77 ms). Frame Skip saves the emulation thread almost nothing (1: -7% per
    console frame; Auto: -1.5%).
- **Dreamcast: 42.5 MiB less in the program's image** (the 00.63 memory plan's part for the Dreamcast). The core's
  recompilers' code (SH4 11 MiB, ARM7 4 MiB, DSP 32 KiB) is now three parts of the app's shared JIT arena, taken
  again at each game's start (the code is written again). Its big tables are on the heap only while a game runs (the
  ARM7's 16 MiB table of compiled blocks, the SH4 MMU's 4 MiB, plus 1.5 MiB for a game that turns the MMU on), or
  never made (glslang's 6 MiB SPIR-V table, which Flycast never uses). The image (MemSiz) is 42.5 MiB smaller:
  42.5 MiB more free memory for every other console's games, 22.5 MiB for a Dreamcast game. build.sh's line, 00.66
  -> 00.67: `image: 127.0 -> 126.3 MiB of file, 256.3 -> 213.0 MiB of memory at load (MemSiz): about 182 -> 225 MiB
  of flexible memory free at display ready`. The build now refuses a static array of 1 MiB or more in the core.
  Fast memory is unchanged.
  - PC: two and three games in one process, with the arena given back between them as the app does (Crazy Taxi then
    Marvel vs. Capcom 2: the second game's pictures equal those of Marvel vs. Capcom 2 alone; Jet Set Radio then
    Crazy Taxi twice, with the DSP on). With the arena refused, the three interpreters run.
  - Found on the way: the recompilers register their main loops' unwind records, which Flycast writes into the code
    memory. They are now unregistered at the core's end, before the arena goes to the next core (without that, a
    second game in the same process crashed in __deregister_frame).
  - The log: `jit arena: 11.0 MiB at ... for dc sh4`, `4.0 MiB ... for dc arm7`, `32 KiB ... for dc dsp`; at the
    game's end `jit arena: given back by dc sh4 (... of 11.0 MiB written), dc arm7 (...), dc dsp (...)`.
- **Dreamcast log: where the frame's time goes.** The 10 s `dc per frame` line now says how much of the emulation
  thread is the sound chip (the ARM7, the channels, the DSP); how many disc reads there were, their time per frame
  and the longest (a slow USB stick shows there); how much of the rendering is the TA lists' conversion with the
  textures (the rest is Vulkan's work); and every wait of the emulation for the renderer.

## 00.66 (test build)
- **The sound keeps its pitch when a game runs below full speed** (players heard it "deep, like horror sounds", as if
  slowed down, on the Dreamcast, the PSP, the N64, the 3DS and others). Since 00.39 a game whose core couldn't keep up
  (the pacing's heavy mode, one frame a pass) had its sound resampled as if the core's rate were that much lower:
  pitch times the game's speed - 75% speed played 5 semitones deep, 60% almost an octave. The owner's logs show it as
  "the core sent X Hz (of N)", N the rate the resampler was given: Crazy Taxi x0.53-0.85, Daytona USA x0.60-0.92, God
  of War: Ghost of Sparta x0.71-0.89, Hey! Pikmin x0.75-0.88, Mario Kart DS x0.90. With the Dreamcast's bursts of
  sound on top, the ring overflowed - 20,000 to 127,000 frames dropped every 10 s, sound skipping ahead ("plays
  faster") - and ran dry and restarted (the stutter). Now the sound is always resampled at the core's own rate
  (src/Emulator.cpp: s_audio_speed gone), and what a slower game doesn't send is made up by a stretcher in the sound
  thread (src/GameAudio.cpp): a waveform period of the sound (2.5 to 25 ms: down to a 40 Hz bass) played again - the
  sound just played, kept in the ring for it - or left out when the game runs faster (Speed above 100%, the frames
  owed after a stall), where the sound matches itself best (normalized cross-correlation of both channels, a coarse
  pass and a fine one; 6 ms crossfade). The sound lasts longer or shorter at its own pitch, up to half of it.
  RetroArch's rate control (+-0.5%) stays for the small differences (a 60.1 FPS game at the TV's 59.94 Hz) and is
  held at 1 while the stretcher works. A stall longer than the stretcher bridges is a short silence faded out and in
  (2 ms), not a cut; volume changes and the mute are ramped over 5 ms; after fast forward the delay is the buffer's
  own at once (it stayed at 190 ms for seconds).
- PC (scratchpad\audio0066: the app's real GameAudio and its sound thread against an AudioOut double in virtual time,
  Emulator.cpp's pacing for V-Sync on and off and its heavy mode, sines of 440 Hz and 1 kHz and a chord with a 55 Hz
  bass at each core's rate - Snes9x, FCEUmm, Mupen64Plus, PPSSPP, Flycast, Azahar, melonDS, FinalBurn Neo, mGBA,
  SwanStation -, 30 s each, 64 ms buffer). 00.65: at 90/80/60% speed -290..-320 / -480..-510 / -940..-975 cents;
  with stalls of 40-250 ms 10-16 gaps and 76-124 clicks; the 3DS's pattern (2.5 s at full speed, 2.5 s at 85%,
  stalls) -165 cents, worst -470, 14-24 gaps; Speed 110% +21 cents and 2119 clicks. 00.66: 0.0 cents at every speed
  (worst 0.0), no gap and no click down to 57% speed; with stalls 0-5 gaps, 0 clicks; the 3DS's pattern only its 4
  stalls' silences; full speed within the rate control's -9..+6 cents; the chord's bass when stretched 50 dB above the
  splices' noise (49 dB at full speed: the resampler's own). (The speed factor taken out alone: 296 gaps and 2291
  clicks in 30 s at 80%.) The stretcher costs the sound thread about 4 us a period of 5.3 ms on the PC (vectorized);
  ThreadSanitizer quiet with both threads on the real clock. Buffers of 48, 96 and 128 ms the same.
- **The sound's log line says what it does**, every 10 s: `audio: 10.0 s: the core sent 24424 Hz, 74.6% of its
  32728 Hz; pitch: the core's, rate control +0.00% (+0.00..+0.00%, held at 0 for 100%); stretched +25.5% (102
  splices), shortened 0.0% (0, 0 cuts); 100.0% played, 0.0% silence, 0.0% lost, dry 0 times, 0 frames dropped; 45 ms
  buffered (lowest 29); the PS4 played 47997 Hz: ...` (a 3DS game at 75%) - the core's rate by the PS4's sound clock
  and its share of the core's own (the game's audio speed), the pitch correction (the rate control's mean and range,
  and how long it was held), what the stretcher added and took out, the buffer's lowest level, and the PS4's own
  output rate by the clock. At full speed: `... rate control -0.29% (-0.38..-0.21%, held at 0 for 0%); stretched
  +0.0% (0 splices) ...` (Snes9x's 60.1 FPS at 59.94 Hz).
- **3DS: shorter stops while a game compiles its shaders** (the owner: the 3DS games run very badly; 00.65's log: in
  a game's first minutes it stopped for 0.4-2.6 s every ~10 s, waiting for Vulkan pipelines to be built).
  scripts/build-3ds-core.sh:
  - a pipeline that a draw needs while it still waits in the workers' queue is built at once, not after everything
    queued before it (at a game's start the shader cache queues every pipeline the driver's cache doesn't have:
    hundreds after an update of the app, whose new Mesa build doesn't take the old driver cache);
  - 3 pipeline and 3 shader workers instead of 2, on cores 0, 1, 3, 4 and 5, below every other thread of the game
    (port/ps4_n64.c: core 0 added; in a game it has only the sound's short turns): a burst of new pipelines is built
    3 at a time;
  - the driver's pipeline cache (saves/3ds/Azahar/shaders/vulkan/pipeline/) is also saved every 60 s while it grows,
    on a worker, not only when a game ends cleanly: a session closed from the PS4's menu or by a crash keeps what it
    compiled, and the next start doesn't compile it again (never while a game switches to another title's cache);
  - PC (a first start with nothing cached, 3000 frames, 00.65's options; the frames over 100 ms after the load):
    Super Mario 3D Land 3.7 s of stops -> 2.5 s, Hey! Pikmin 0.72 -> 0.62 s; a second start 0.16 s and none.
- **3DS: asynchronous shader compilation as an option.** With the file /data/EmuSuNi/3ds-async-shaders (any content;
  read at each game's start) a draw whose pipeline isn't ready is skipped (an object missing for a few frames) instead
  of stopping the game. PC, the same first start: Mario's stops 3.7 -> 0.5 s, Pikmin's 0.72 -> 0.18 s.
- **3DS: the log says where a slow frame's time went.** A frame of more than 100 ms logs `EmuSuNi: slow frame N ms:
  waited N ms for the GPU side; pipelines N here (N ms) and N on the workers (N ms); shaders ...; textures N KiB
  (N ms); files read N KiB here (N ms) and N KiB elsewhere (N ms)`, and every 600 frames one line sums them up
  (`EmuSuNi: 600 frames in N ms: ...`), so the next log names each stop's cause.
- **3DS: the core is compiled with a profile (PGO):** scripts/3ds-core.profdata.xz, a profile of the PC build playing
  Super Mario 3D Land (logos, title, menus, World 1-1) and Hey! Pikmin (the start, the intro's 3D scenes), taken again
  on 00.66's code (function counters only, nothing of the games); still -O2; CTR_CORE_PGO=off builds without it.
  PC, 00.66's code with and without it (3 interleaved rounds, medians): the emulation thread's time per frame -4.3%
  in World 1-1, -4.1% in Pikmin's garden.
- **3DS: fast memory, to try: only with the file /data/EmuSuNi/3ds-fastmem** (any content; read at each game's
  start; without it nothing changes). The ARM11's recompiler (dynarmic) then reads and writes the game's memory with
  one instruction instead of a walk of the page table: each page table's 4 GiB of the 3DS's address space is reserved,
  and every 16 KiB group of guest pages that lies in order in the 3DS's memory (FCRAM, VRAM, the New 3DS RAM: direct
  memory) is mapped there a second time (direct memory aliasing, as the Dreamcast's fast memory). Anything else (I/O,
  the pages the GPU cache watches, the shared pages) faults once, and dynarmic turns the fault into a call of that
  access's fallback and recompiles it with the page table (its POSIX fault handler, now built for the PS4, installed
  only while a game uses fast memory; the handlers before are put back at the game's end). With the file, the game's
  code, data and stack are taken from FCRAM at their guest addresses' 16 KiB phase, so that they map whole. A
  self-test before the first use (a page of direct memory mapped twice is the same memory; a load from an unmapped
  page faults with its address, pc and stack, and turned into a call it returns), the page table whenever anything is
  refused, and the marker files /data/EmuSuNi/3ds-fastmem-trying and 3ds-fastmem-crashed: a start that dies while
  trying it, or in the first 600 frames of play with it, turns it off until 3ds-fastmem-crashed is deleted. The log
  says which: `EmuSuNi: fastmem: on for this game (the file /data/EmuSuNi/3ds-fastmem)` or `EmuSuNi: fastmem: off:
  <why> (the page table instead)`, and at a game's end what it did. scripts/build-3ds-core.sh (common/emusuni_fastmem.cpp).
  PC (the PS4's kernel calls by stand-ins with its 16 KiB pages): the emulation thread's time per frame -8.4% in
  World 1-1 and -10.1% in Pikmin's garden; the same pictures with and without it; each refusal falls back.

## 00.65 (test build)
- **NAOMI / Atomiswave: a set with a file missing, or a BIOS zip without its files, now says so instead of closing the app** (a check in the core that reads the zips' directories without loading anything or throwing a C++ exception: flycast_emusuni_check_set, scripts/build-dc-core.sh; src/Naomi.cpp).
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
