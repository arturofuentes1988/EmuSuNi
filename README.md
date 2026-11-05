<p align="center">
  <img src="docs/banner.png" alt="EmuSuNi" width="860">
</p>

<p align="center">
  <b>Twenty-three classic consoles and arcade boards, and OpenBOR's games, in one app for jailbroken PS4 consoles.</b><br>
  Super Nintendo · NES · Nintendo 64 · Game Boy · Game Boy Color · Game Boy Advance · PlayStation ·
  Nintendo DS · Nintendo 3DS · PSP · Dreamcast · NAOMI · Atomiswave · SG-1000 · Master System ·
  Game Gear · Genesis · Saturn · PC Engine · Atari 2600 · Atari 7800 · Atari Jaguar · Arcade · OpenBOR
</p>

<p align="center">
  A launcher that feels like a console's own: your games with their box art, an AI-enhanced HD
  picture, online play with your friends, cheats from the internet, RetroAchievements, save states,
  bezels and settings for every game.
</p>

<p align="center">
  <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest"><img alt="Download" src="https://img.shields.io/github/v/release/arturofuentes1988/EmuSuNi?label=download&style=for-the-badge&color=7b4dff"></a>
  <img alt="PS4 with GoldHEN" src="https://img.shields.io/badge/PS4-GoldHEN-003791?style=for-the-badge&logo=playstation&logoColor=white">
  <img alt="23 consoles" src="https://img.shields.io/badge/consoles-23-e5484d?style=for-the-badge">
  <a href="LICENSE"><img alt="License GPL-2.0+" src="https://img.shields.io/badge/license-GPL--2.0%2B-3db46d?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.es.md">Leer en español</a> · <a href="CHANGELOG.md">Changelog</a> · <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest">Download</a> · <a href="https://t.me/+A53DraC3it5mZTcx">Telegram</a></p>

---

## Quick start

1. **Install**: download `EmuSuNi-vXX.YY.pkg` from the [latest release](https://github.com/arturofuentes1988/EmuSuNi/releases/latest),
   copy it to a USB drive and install it with GoldHEN's **Package Installer** ([Installation](#installation)).
   **To update**: from v00.22 on, the app tells you when a new version is out and **Settings → Check for
   Updates** installs it; or install the new package over the old one (your games, saves and settings
   stay). From v00.21, install v00.22 by hand this once.
2. **Add your games**: copy them to `Roms/<console>` on the USB drive or to `/data/EmuSuNi/Roms/<console>`
   on the PS4 (by FTP). The app makes the folders itself: `snes`, `nes`, `n64`, `gb`, `gbc`, `gba`, `ps1`,
   `nds`, `3ds`, `psp`, `dc`, `naomi`, `atomiswave`, `sg1000`, `sms`, `gg`, `md` (Genesis), `saturn`,
   `pce` (PC Engine), `atari2600`, `atari7800`, `atarijaguar`, `arcade` and `openbor` (OpenBOR's `.pak` games).
   Arcade, NAOMI and Atomiswave games stay in their `.zip` ([formats](#the-consoles)).
3. **BIOS**: only the Arcade's Neo Geo games (`neogeo.zip`), NAOMI (`naomi.zip`, `naomi2.zip`),
   Atomiswave (`awbios.zip`) and the Saturn (a 512 KB `.bin`, such as `saturn_bios.bin`) need yours, plus
   `disksys.rom` for Famicom Disk System games ([BIOS files](#bios-files)). No games or BIOS files are
   included.
4. **Play**: your games show up by themselves, with their box art when the PS4 is online. In a game,
   **L3 + R3** opens the menu: save states, settings, controls, quit.
5. **Help and bug reports**: the [Telegram group](https://t.me/+A53DraC3it5mZTcx), or an
   [issue](https://github.com/arturofuentes1988/EmuSuNi/issues) with your `/data/EmuSuNi/emusuni.log`.
   **Settings → Send Log** sends the log from the app and gives you a code to write in your report.

## The consoles

| Console | Emulator | Formats | BIOS |
|---|---|---|---|
| **Super Nintendo** | Snes9x 1.63 | `.sfc` `.smc` `.swc` `.fig` `.bs` | Not needed |
| **NES** and Famicom Disk System | FCEUmm | `.nes` `.fds` `.unf` `.unif` | Not needed |
| **Nintendo 64** | Mupen64Plus-Next | `.z64` `.n64` `.v64` | Not needed |
| **Game Boy** and **Game Boy Color** | mGBA | `.gb` `.sgb` `.dmg` `.gbc` `.cgb` | Not needed |
| **Game Boy Advance** | mGBA | `.gba` `.agb` | Not needed |
| **PlayStation** | SwanStation | `.cue` `.bin` `.chd` `.pbp` `.m3u` `.iso` `.img` `.ecm` `.mds` | Not needed |
| **Nintendo DS** | melonDS DS 1.4.0 | `.nds` `.ids` | Not needed |
| **Nintendo 3DS** — *experimental* | Azahar 2126.2 | Decrypted `.3ds` `.cci` `.cxi` `.3dsx` | Not needed |
| **PSP** | PPSSPP 1.20.4 | `.iso` `.cso` `.chd` `.pbp` `.elf` | Not needed |
| **Dreamcast** | Flycast 2.7 | `.gdi` `.cdi` `.chd` `.cue` `.m3u` | Not needed |
| **NAOMI** and NAOMI 2 | Flycast 2.7 | MAME sets `.zip` `.7z`, images `.dat` `.bin` | **Yours**: `naomi.zip`, `naomi2.zip` |
| **Atomiswave** | Flycast 2.7 | MAME sets `.zip` `.7z` | **Yours**: `awbios.zip` |
| **SG-1000** and SC-3000 | Genesis Plus GX | `.sg` `.sc` | Not needed |
| **Master System** | Genesis Plus GX | `.sms` `.bms` | Not needed |
| **Game Gear** | Genesis Plus GX | `.gg` | Not needed |
| **Genesis** / Mega Drive | Genesis Plus GX | `.md` `.gen` `.smd` `.bin` `.mdx` `.68k` `.sgd` | Not needed |
| **Saturn** — *experimental* | YabaSanshiro 3.4.2 | `.cue` `.chd` `.iso` `.ccd` `.mds` | **Yours**: a 512 KB `.bin` ([BIOS](#bios-files)) |
| **PC Engine** / TurboGrafx-16 | Beetle PCE Fast | `.pce` (HuCards) | Not needed |
| **Atari 2600** — *new* | Stella | `.a26` `.bin` | Not needed |
| **Atari 7800** — *new* | ProSystem | `.a78` | Not needed |
| **Atari Jaguar** — *experimental, new* | Virtual Jaguar 3.11 | `.j64` `.jag` `.abs` `.cof` | Not needed |
| **Arcade**: Neo Geo, Capcom CPS1/CPS2/CPS3, Sega, Konami, Taito... | FinalBurn Neo 1.0.0.03 | One `.zip` / `.7z` per game, not unpacked | Neo Geo: **yours**, `neogeo.zip` |
| **OpenBOR**: the beat 'em ups fans make — *new* | OpenBOR v3.0 (Build 6391) and v4.0 (Build 8025) | `.pak` | Not needed |

No BIOS files or games are included. Only the Arcade's Neo Geo games, NAOMI, Atomiswave and the Saturn
need a BIOS of yours ([BIOS](#bios-files)).

The **Saturn** is experimental: it draws in software, on threads of its own, and its games can run slower
than on the console. The **Nintendo 3DS** is still experimental: decrypted games only; if a game doesn't
load the first time, **L3 + R3 → Reset Game** until it does. The PC Engine plays HuCards (no CD games).
The **Atari Jaguar** is experimental too: most of the games tried play well, a few (Iron Soldier's 3D) still run
slower.
**OpenBOR** plays the free beat 'em ups its fans make, each a `.pak` file: none come with EmuSuNi, put yours in
`Roms/openbor` ([Where the games go](#where-the-games-go)). They have no save states, cheats or covers.

## What's new in v00.23

- **Online play with your friends**: play one game with a friend anywhere in the world, as if both controllers
  were on one console. Open the **Online** tab, make a room (a 6-letter code for a friend, or a public room, for
  2 to 4 players) or join one. It works on the NES, Super Nintendo, Genesis, Master System, Game Gear, SG-1000,
  PC Engine, Arcade, Atari 2600 and Atari 7800, and the Game Boy and Game Boy Color play over their **link
  cable** (**Triangle → Link Cable Online**). You can also **Choose Together**: the app shows the games you both
  have. See [Online play](#online-play).
- **Voice chat** while you play online, with a mute for each player (**Settings → Voice Chat**), and a latency bar
  with everyone's ping.
- **PSP, Dreamcast and DS online** through the community's free servers: the PSP games whose servers fans revived
  and ad hoc play, DCNet on the Dreamcast, Wiimmfi on the DS ([how](#online-with-the-communitys-servers-psp-dreamcast-ds)).
- **New consoles**: the **Atari 2600**, the **Atari 7800** and the **Atari Jaguar** (experimental), and
  **OpenBOR**'s games (your own `.pak` files in `Roms/openbor`).
- **Super Smash Bros. on the 3DS** looks right: its fighters no longer break up in a fight. The 3DS also stutters
  much less the first time a game is played.
- **3 and 4 players on one PS4**: every extra DualShock 4 signed in to a PS4 user is the next player, and each
  console's multiplayer adapter is plugged in for you (**Multiplayer Adapter**).
- **Portable mode**: keep your saves, states, covers and settings on a USB drive and play with them on
  another PS4 with EmuSuNi ([Portable mode](#portable-mode)).
- **Ultra from the start**: every console starts at Quality Ultra (the 3DS at Medium) and the DS with its screens
  side by side. Updating moves the consoles whose quality you never chose to the new levels, once.
- **Anonymous statistics**, **on by default**: a small heartbeat tells us how many people use EmuSuNi, from
  which countries and with which versions, never who. **Settings → Anonymous Statistics → Off** sends
  nothing at all ([details](#anonymous-statistics)). There is also a [web page](https://emusuni.com)
  with the live numbers.
- **Settings → Send Log**: the log goes to EmuSuNi's server and you get a code for your report on GitHub or
  Telegram. Your games and saves are never sent; the server keeps only the country, never your address, and
  deletes the log after 90 days.
- **Big game libraries**: the app opens at once whatever the number of games (it did not open with more than
  10 000).
- **Controls**: macros (one button that presses several), assignable sticks, and **Settings → Controls →
  In-Game Menu** also has R3, L1 + R1 and L2 + R2 pressed (no need to hold).
- **Menu Size** (100 %, 90 %, 80 %, 70 %): more games in view.
- **Save states have their picture** on every console (the N64, PlayStation, PSP, Dreamcast, NAOMI,
  Atomiswave and 3DS had none).
- **My Stats**: your time played by game, kept on your PS4 only.
- **Fixes**: the Jaguar much faster with its new recompiler; the PlayStation with fewer pauses while loading,
  and the European Metal Gear Solid, Tekken 3 and Driver 2 play again; a damaged Dreamcast disc no longer
  closes the app and Sega Rally 2 starts; PSP saves on a PS5; saving a state on the Saturn no longer closes the
  app; a 3DS game that goes wrong ends the game, not EmuSuNi.

All the details: the [release notes](https://github.com/arturofuentes1988/EmuSuNi/releases/latest) and the [changelog](CHANGELOG.md).

## What was new in 00.22

- **Two new consoles**: the **Sega Saturn** (experimental) and the **PC Engine / TurboGrafx-16**.
- **RetroAchievements** (Casual) on every console but the 3DS: log in at **Settings → RetroAchievements**;
  **Triangle → Achievements** shows a game's achievements; an "RA" mark on the covers of the games that
  have them, and the **touchpad** shows only those; **My Achievements**; their texts translated into the
  app's language.
- **Updates from the app**: **Settings → Check for Updates** says when a new version is out, downloads it
  and hands it to the PS4's installer. Coming from v00.21, install v00.22 by hand this once.
- **Star ratings** under the covers (the players of the LaunchBox Games Database), and **a bezel for each
  game** from The Bezel Project (**Settings → Game Bezels** downloads the ones of your games).
- **DS and 3DS screens with the same settings**: Screen Layout (one above the other, side by side, large
  screen + small, one screen), Swap Screens, the small screen's size and position, the gap between them.
- **Quality levels** on the Dreamcast, NAOMI and Atomiswave (up to 3x) and on the 3DS (up to 4x); on every
  console each level is now clearly sharper than the one below.
- **Faster**: the Dreamcast, NAOMI and Atomiswave, the Saturn, the 3DS (fewer stops to compile its shaders
  and code) and the N64. The sound keeps its pitch when a game runs below full speed.
- **Fixes**: the PSP no longer runs too fast with Frame Skip (God of War); PlayStation `.bin` files without
  their `.cue`, 2048-byte-sector ISOs and LibCrypt discs start; the N64's Cruis'n USA, Resident Evil 2 and
  World Driver Championship no longer stay black, and Ultra draws no more lines over its 2D; automatic
  settings for the 3DS graphics errors of Pokémon X and Y, Majora's Mask 3D, Metroid: Samus Returns and
  Castlevania: Mirror of Fate.
- **Covers**: the Super Nintendo's in `covers/snes`, JPEG and any size; numbered file names (`01 Comix
  Zone`) get theirs; **Games Without Cover** says why a picture isn't used.
- **The emulators' own lists of special settings for some games** are applied (the log says which).
- **A game that doesn't open the first time is started again by itself.**
- **One keyboard for every text** (rename, search, login) with the menu's buttons; every menu goes round;
  **L1 / R1 held** go through the consoles; the in-game menu can also open with R3 or the touchpad held,
  Options + touchpad, or L1 + R1 or L2 + R2 held (**Settings → Controls → In-Game Menu**); PSP stick dead
  zone and sensitivity.
- Every console starts at Quality Low, Aspect Ratio Auto and its own bezel (since 00.23: Ultra, the 3DS Medium).

All the details: the [release notes](https://github.com/arturofuentes1988/EmuSuNi/releases/latest) and the [changelog](CHANGELOG.md).

## Screenshots

New in 00.22 (the app in Spanish):

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/v00.22/1-saturn-library.png" alt="Sega Saturn page"></td>
    <td width="50%"><img src="docs/screenshots/v00.22/2-saturn-daytona-usa.png" alt="Daytona USA on the Saturn"></td>
  </tr>
  <tr>
    <td align="center"><b>Sega Saturn</b> (experimental) — covers, star ratings and the RA mark</td>
    <td align="center"><b>Daytona USA</b> — with its own bezel from The Bezel Project</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/v00.22/3-pc-engine-library.png" alt="PC Engine page"></td>
    <td><img src="docs/screenshots/v00.22/4-pc-engine-new-adventure-island.png" alt="New Adventure Island on the PC Engine"></td>
  </tr>
  <tr>
    <td align="center"><b>PC Engine / TurboGrafx-16</b> — HuCards</td>
    <td align="center"><b>New Adventure Island</b> — with its own bezel</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/v00.22/5-achievements-in-game.png" alt="Achievements in the in-game menu"></td>
    <td><img src="docs/screenshots/v00.22/6-my-achievements.png" alt="My Achievements"></td>
  </tr>
  <tr>
    <td align="center"><b>RetroAchievements</b> in the in-game menu (L3 + R3) — translated to Spanish</td>
    <td align="center"><b>My Achievements</b> — your latest unlocks and games</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/v00.22/7-check-for-updates.png" alt="Check for Updates"></td>
    <td></td>
  </tr>
  <tr>
    <td align="center"><b>Check for Updates</b> — the app tells you about each new version</td>
    <td></td>
  </tr>
</table>

Earlier versions:

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/game-snes.png" alt="Super Nintendo"></td>
    <td width="50%"><img src="docs/screenshots/game-n64.png" alt="Nintendo 64"></td>
  </tr>
  <tr>
    <td align="center"><b>Super Nintendo</b> — redrawn with AI (xBRZ + Anime4K)</td>
    <td align="center"><b>Nintendo 64</b> — at 4x, about Full HD</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/game-gb.png" alt="Game Boy"></td>
    <td><img src="docs/screenshots/game-gba.png" alt="Game Boy Advance"></td>
  </tr>
  <tr>
    <td align="center"><b>Game Boy</b> — in the console's own bezel</td>
    <td align="center"><b>Game Boy Advance</b> — in its Indigo bezel</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/game-gbc.png" alt="Game Boy Color"></td>
    <td><img src="docs/screenshots/game-nes.png" alt="NES"></td>
  </tr>
  <tr>
    <td align="center"><b>Game Boy Color</b> — in its Berry bezel</td>
    <td align="center"><b>NES</b> — in the retro TV bezel</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/library.png" alt="The Library"></td>
    <td><img src="docs/screenshots/xmb.png" alt="XMB style"></td>
  </tr>
  <tr>
    <td align="center"><b>Library</b> — a tab per console, box art downloaded automatically</td>
    <td align="center"><b>XMB style</b> — the PSP-style columns, 12 colour themes</td>
  </tr>
  <tr>
    <td><img src="docs/screenshots/settings-quality.png" alt="Quality"></td>
    <td><img src="docs/screenshots/11-ingame-settings.png" alt="In-game menu"></td>
  </tr>
  <tr>
    <td align="center"><b>Quality</b> — Low, Medium, High or Ultra at once</td>
    <td align="center"><b>In-game menu</b> (L3 + R3) — save states, settings, cheats</td>
  </tr>
</table>

<details>
<summary>More screenshots</summary>

| | |
|---|---|
| ![Super Game Boy](docs/screenshots/game-gb-sgb.png) | ![Mario Kart 64](docs/screenshots/game-n64-mariokart.png) |
| Game Boy as the Super Game Boy: its colours and border | Mario Kart 64 at 4x |
| ![SNES](docs/screenshots/tab-snes.png) | ![NES](docs/screenshots/tab-nes.png) |
| Super Nintendo tab | NES tab |
| ![N64](docs/screenshots/tab-n64.png) | ![GB](docs/screenshots/tab-gb.png) |
| Nintendo 64 tab | Game Boy tab |
| ![GBC](docs/screenshots/tab-gbc.png) | ![GBA](docs/screenshots/tab-gba.png) |
| Game Boy Color tab | Game Boy Advance tab |
| ![Favorites](docs/screenshots/03-favorites.png) | ![Search](docs/screenshots/04-search.png) |
| Favorites (L2 on a game) | Search with an on-screen keyboard |
| ![Game options](docs/screenshots/05-game-options.png) | ![Clean Up Games](docs/screenshots/08-clean-up.png) |
| A game's options (Triangle) | Clean Up Games: duplicates and other regions |

</details>

## Features

**Picture**
- **Quality** in one setting: Low, Medium, High or **Ultra** (the most each console can do), each
  level clearly sharper than the one below. Low is the console's own picture; on the 2D consoles
  Medium, High and Ultra are Smooth Edges, Pixel Art HD (xBRZ) and AI (xBRZ + Anime4K), on the 3D
  ones a bigger picture with smooth edges, sharpened at Ultra.
- **GPU Upscaler**: Smooth Edges (MMPX and SMAA), Natural HD (AMD FSR), Natural AI (the Anime4K
  neural network, without the waxy look), Pixel Art HD (xBRZ), AI (xBRZ + Anime4K), Smooth HD and
  HD Sharpen, at the TV's resolution.
- **HD 3D**: the Nintendo 64, the PlayStation and the PSP up to 4x (the PSP with texture upscaling),
  the Dreamcast, NAOMI and Atomiswave up to 3x, the 3DS up to 4x.
- **Resolution** 2x–6x with xBRZ, HQx or Sharp Pixels; aspect ratio, integer scaling, zoom,
  scanlines (on the Game Boys, the grid of their screen), NTSC filter, V-Sync.
- **Bezels for every console**: its own console's frame, a retro TV and six shared frames (arcade
  cabinet, dark minimal, neon, wood, carbon, starfield), your own, or **a bezel for each game** from
  [The Bezel Project](https://github.com/thebezelproject) (**Settings → Game Bezels** downloads the
  ones of your games). **Tube TV (CRT)**: curved glass, scanlines and a phosphor mask, in four looks.
- **DS and 3DS screens**: one above the other, side by side, a large screen and a small one, or one
  screen; swapped, with a gap, the small one where you want it.

**Playing**
- Battery saves written automatically, **8 save-state slots** with screenshots, *resume where you
  left off*, reset.
- **[RetroAchievements](https://retroachievements.org)** (Casual) on every console but the 3DS: log in
  at **Settings → RetroAchievements**, unlock achievements as you play, see a game's with
  **Triangle → Achievements** and yours in **My Achievements**; an "RA" mark on the covers of the
  games that have them; the achievements' texts translated into the app's language.
- **Cheats**: RetroArch `.cht` files, or **Find Cheats Online** (not on the Dreamcast, NAOMI,
  Atomiswave, Arcade or 3DS, whose emulators take no cheat codes here).
- **Games on several discs** grouped by themselves, with **Disc** in the in-game menu.
- **The in-game menu** (L3 + R3, or other buttons of your choice): save and load states, change the
  disc, settings, cheats, achievements, reset, quit.
- Up to 5 DualShock 4 controllers, rumble, fast forward.
- A game that doesn't open the first time is started again by itself.

**Together**
- **[Online play](#online-play)** over the internet, with rollback (the way fighting games do it) and voice
  chat, for the NES, Super Nintendo, Genesis, Master System, Game Gear, SG-1000, PC Engine, Arcade, Atari 2600
  and Atari 7800, and the Game Boy's link cable; the PSP, Dreamcast and DS online through the community's servers.
- **[Up to 4 players](#playing-together-up-to-4-players)** on one PS4, with the right multitap for each console.
- **[Portable mode](#portable-mode)**: your data on a USB drive, to play on a friend's PS4.

**A launcher like a console's**
- Two menu styles: **Library** (box art in a row, a tab per console on L1 / R1, going round) and
  **XMB** (the PSP-style columns), 12 colour themes or your own wallpapers, 8-bit sounds.
- **Box art, automatically** from libretro's thumbnail server, and **star ratings** under them (the
  players of the [LaunchBox Games Database](https://gamesdb.launchbox-app.com)).
- **Updates from the app**: **Settings → Check for Updates** says when a new version is out,
  downloads it and hands it to the PS4's installer.
- **Console Order**: the consoles in the order you like, or By Company, By Power, By Year or
  Alphabetical.
- Favorites, search, sorting (A–Z, recently played, region, console), a default game, a folder
  browser, *Clean Up Games* and **13 languages** (complete in English and Spanish, partial in the
  other 11).
- **Every setting for all games, for a console or for one game**, with a short explanation of each
  option, and **button remapping**.

## Requirements

- A PS4 with **[GoldHEN](https://github.com/GoldHEN/GoldHEN)** (any model).
- Your own games:

  | Console | Files |
  |---|---|
  | Super Nintendo | `.sfc` `.smc` `.swc` `.fig` `.bs` |
  | NES | `.nes` `.fds` `.unf` `.unif` |
  | Nintendo 64 | `.z64` `.n64` `.v64` |
  | Game Boy | `.gb` `.sgb` `.dmg` |
  | Game Boy Color | `.gbc` `.cgb` (and the `.gb` files you put in its folder, like Pokémon Yellow) |
  | Game Boy Advance | `.gba` `.agb` |
  | PlayStation | `.cue` (with its `.bin` tracks) or a `.bin` alone, `.chd` `.pbp` `.m3u` `.iso` `.img` `.ecm` `.mds` |
  | Nintendo DS | `.nds` `.ids` |
  | Nintendo 3DS (experimental) | Decrypted `.3ds` `.cci` `.cxi` `.3dsx` (no `.cia`) |
  | PSP | `.iso` `.cso` `.chd` `.pbp` `.elf` |
  | Dreamcast | `.gdi` (with its tracks) `.chd` `.cdi` `.cue` `.m3u` |
  | NAOMI / NAOMI 2 | MAME sets `.zip` `.7z`, not unpacked, or `.dat` `.bin` images |
  | Atomiswave | MAME sets `.zip` `.7z`, not unpacked |
  | SG-1000 / SC-3000 | `.sg` `.sc` |
  | Master System | `.sms` `.bms` |
  | Game Gear | `.gg` |
  | Genesis / Mega Drive | `.md` `.gen` `.smd` `.bin` `.mdx` `.68k` `.sgd` |
  | Saturn (experimental) | `.cue` (with its `.bin` tracks) `.chd` `.iso` `.ccd` `.mds` |
  | PC Engine / TurboGrafx-16 | `.pce` (HuCards) |
  | Atari 2600 | `.a26`, or a `.bin` of 64 KB or less |
  | Atari 7800 | `.a78` (with its header) |
  | Atari Jaguar (experimental) | `.j64` `.jag` `.abs` `.cof` |
  | Arcade | FinalBurn Neo 1.0.0.03 sets: one `.zip` / `.7z` per game, not unpacked (`mslug.zip`) |
  | OpenBOR | `.pak`, as its maker gives it (not zipped) |

  The cartridge consoles (Super Nintendo to Game Boy Advance, SG-1000 to Genesis, PC Engine, Atari's) take
  `.zip` files too; the disc consoles, the DS, the 3DS and OpenBOR read their games as they are. An Arcade, NAOMI
  or Atomiswave game's `.zip` is the game itself: keep it zipped, with its set's name.
- Internet on the PS4 for the box art, the online cheats and online play (optional).

## Installation

1. Download **`EmuSuNi-vXX.YY.pkg`** from the [latest release](https://github.com/arturofuentes1988/EmuSuNi/releases/latest).
2. Copy it to a USB drive (FAT32 or exFAT), for example in a `PKG` folder.
3. On the PS4, install it with GoldHEN's **Package Installer** (or *Settings → Debug Settings →
   Game → Package Installer*).
4. Start **EmuSuNi** from the home screen.

**To update**: from v00.22 on, the app checks once a day and tells you when a new version is out;
**Settings → Check for Updates** checks now, and **Update Now** downloads the package (to the USB
drive's `PKG` folder, or the PS4) and hands it to the PS4's installer (the installation shows in
*Notifications → Downloads*; close EmuSuNi so the PS4 can finish it). From v00.21, install v00.22 by
hand this once.

By hand, install the new package over the old one: your settings, saves, states and covers stay
in `/data/EmuSuNi`. If the Package Installer won't install it over the old one, delete EmuSuNi from
the home screen (**OPTIONS → Delete**) and install the new package: `/data/EmuSuNi` isn't touched, so
nothing is lost.

## Where the games go

The app makes a folder for every console by itself, on the PS4 and on the USB drive you use for games:

| Where | Path | Over FTP |
|---|---|---|
| USB drive | `Roms/<console>` at the drive's root | `/mnt/usb0/Roms/<console>` |
| The PS4 | `/data/EmuSuNi/Roms/<console>` | `/data/EmuSuNi/Roms/<console>` |

The folders: `snes`, `nes`, `n64`, `gb`, `gbc`, `gba`, `ps1`, `nds`, `3ds`, `psp`, `dc`, `naomi`,
`atomiswave`, `sg1000`, `sms`, `gg`, `md` (Genesis), `saturn`, `pce` (PC Engine), `atari2600`, `atari7800`,
`atarijaguar`, `arcade` and `openbor`.

**OpenBOR's games** go in `openbor`: `Roms/openbor` on the USB drive (`/mnt/usb0/Roms/openbor` over FTP) or
`/data/EmuSuNi/Roms/openbor` on the PS4. Copy each game's `.pak` there as it is (not zipped); the app picks the
OpenBOR version it was made for. EmuSuNi includes none: they are free games made by OpenBOR's fans.

- Copy your games there (by FTP or with the USB drive on a PC): they show up by themselves. Press
  **Square** in the Library to look again right away.
- **Any folder works** too: **Settings → Game Folders** sets another one for a console.
- On the PS4's own storage letters' case matters: an arcade, NAOMI or Atomiswave zip must keep its
  set's name in small letters (`mslug.zip`, `mvsc2.zip`).
- Deleting or updating the app doesn't delete `/data/EmuSuNi`: your games, saves, covers and settings stay.

**Games on several discs** (PlayStation, Dreamcast) become one game by themselves when:
- their files sit side by side with the disc in their names: `Game (Disc 1).chd`, `Game (Disc 2).chd`
  (also `(CD2)`, `(Disk 2)`, `[Disc 2]`);
- each disc has a folder: `Game (Disc 1)/disc.gdi`, or `Game/Disc 1/disc.gdi`;
- or an `.m3u` file lists them.

In the game, **L3 + R3 → Disc** changes the disc (Left / Right to choose, Cross to put it in). The
next time, the game starts with the disc you played last.

## BIOS files

None are included. Only these need yours:

| Console | Files | Folder |
|---|---|---|
| Arcade: Neo Geo games | `neogeo.zip` | Beside the games (`Roms/arcade`) |
| NAOMI / NAOMI 2 | `naomi.zip`, `naomi2.zip` | Beside the games (`Roms/naomi`) or `/data/EmuSuNi/system/dc` |
| Atomiswave | `awbios.zip` | Beside the games (`Roms/atomiswave`) or `/data/EmuSuNi/system/dc` |
| Saturn | A 512 KB `.bin`: `saturn_bios.bin` or `mpr-17933.bin` (USA / Europe), `sega_101.bin` (Japan), or any `.bin` with "bios" in its name (a USA or Europe one is preferred) | `/data/EmuSuNi/system/saturn`, `/data/EmuSuNi/system` or beside the game (`Roms/saturn`) |
| Famicom Disk System | `disksys.rom` | `/data/EmuSuNi/system` |

The other consoles play without one, the PC Engine and the Atari consoles included. If you have the real ones, they
are used: PlayStation (`scph5501.bin` or another, more games work), Nintendo DS (`bios7.bin`, `bios9.bin`,
`firmware.bin`), Atari 7800 (`7800 BIOS (U).rom`) and Atari Jaguar (`jagboot.rom`, its boot animation) in
`/data/EmuSuNi/system`, Dreamcast (`dc_boot.bin`) in `/data/EmuSuNi/system/dc`.

## Box art

- **Downloaded automatically** (the PS4 needs internet) from libretro's thumbnail server; the
  progress shows under *Your games*. *Settings → Download Covers* looks again.
- **Your own work too**: a PNG or JPEG named like the game's file or its title in
  `/data/EmuSuNi/covers/<console>`, the same folder names as the games' (`covers/snes`, `covers/nes`,
  `covers/md`...), or in `EmuSuNi/covers/<console>` on the USB drive. Any size: a big picture is made
  smaller as it is read. **Settings → Games Without Cover** says why a picture of yours isn't used.
- When one is missing, **Triangle → Find Cover** shows the closest ones to pick from.

## Controls

**L3 + R3** (both sticks pressed) opens the in-game menu on every console (**Settings → Controls →
In-Game Menu** adds R3, L1 + R1 or L2 + R2 pressed once, or the touchpad held, Options + touchpad, L1 + R1 or L2 + R2 held). Every button can be
changed from a game: **Triangle → Controls** (*Apply To*: This Game, or All Games, which is every
game of that console).

| DualShock 4 | Super Nintendo | NES | Nintendo 64 | Game Boy / Color | Game Boy Advance |
|---|---|---|---|---|---|
| Cross | B | B | A | B | B |
| Circle | A | A | C Down | A | A |
| Square | Y | Turbo B | B | Turbo B | Turbo B |
| Triangle | X | Turbo A | C Up | Turbo A | Turbo A |
| L1 / R1 | L / R | Disk side / insert or eject (FDS) | L / R | — | L / R |
| L2 | — | — | Z | — | — |
| R2 | Fast forward | Fast forward | Z | Fast forward | Fast forward |
| Options | Start | Start | Start | Start | Start |
| Touchpad | Select | Select | Fast forward | Select | Select |
| Left stick | D-Pad | D-Pad | Stick | D-Pad | D-Pad |
| Right stick | — | — | C buttons | Tilt sensor | Tilt sensor |

| DualShock 4 | PlayStation | Nintendo DS | PSP | Dreamcast |
|---|---|---|---|---|
| Cross | Cross | B | Cross | A |
| Circle | Circle | A | Circle | B |
| Square | Square | Y | Square | X |
| Triangle | Triangle | X | Triangle | Y |
| L1 / R1 | L1 / R1 | L / R | L / R | — |
| L2 | L2 | Select | — | L trigger (analog) |
| R2 | R2 | Fast forward | Fast forward | R trigger (analog) |
| L3 / R3 | L3 / R3 | Microphone (blow) / swap screens | — | — |
| Options | Start | Start | Start | Start |
| Touchpad | Select | Touch screen | Select | — |
| Left stick | Left stick | D-Pad | Stick | Stick |
| Right stick | Right stick | — | — | — |

| DualShock 4 | Saturn | PC Engine |
|---|---|---|
| Cross | B | II |
| Circle | C | I |
| Square | A | III (6-button pad) |
| Triangle | Y | IV (6-button pad) |
| L1 / R1 | X / Z | V / VI (6-button pad) |
| L2 | L | — |
| R2 | R | Fast forward |
| Options | Start | Run |
| Touchpad | Fast forward | Select |
| Left stick | D-Pad | D-Pad |

| DualShock 4 | Atari 2600 | Atari 7800 | Atari Jaguar | OpenBOR |
|---|---|---|---|---|
| Cross | Fire | Button 1 | B | Jump |
| Circle | Booster Grip's trigger | Button 2 | A | Special |
| Square | TV type: Color | — | C | Attack |
| Triangle | TV type: B/W | RESET | OPTION | Attack 2 |
| L1 / R1 | Left / right difficulty A | Left / right difficulty B (held) | Number pad 1 / 2 | Attack 3 / 4 |
| L2 / R2 | Left / right difficulty B | — / Fast forward | Number pad 3 / 4 | — / Fast forward |
| Options | RESET | PAUSE | PAUSE | START |
| Touchpad | SELECT | SELECT | The number pad's 12 keys | SELECT (back) |
| Left stick | Joystick | Joystick | D-Pad | D-Pad |

| Menu (Library style) | |
|---|---|
| D-Pad Left / Right | Previous / next game (Up / Down: 10 at a time) |
| L1 / R1 | Tabs: All, each console, Favorites, Settings (held: through them) |
| Cross | Play |
| Triangle | Game options: game settings, play online, cheats, controls, rename, find cover, achievements, information, default game, delete |
| L2 / R2 | Favorite / search |
| Square | Look for the games again |
| Touchpad | RetroAchievements filter: every game, the games with achievements, the ones in progress |
| Options | Sort |
| Circle | Back |

Hold **L1** while EmuSuNi starts to open the menu instead of the default game.

### Playing together (up to 4 players)

Every extra player needs a DualShock 4 **signed in to a PS4 user**: press the PS button on the new controller and
pick who is using it (a new local user is fine; it needs no PSN account). Player 1 is the user who started
EmuSuNi; the others are players 2, 3 and 4 in the order they sign in, each one keeping their number while signed
in. The PS4 lets 4 users in at once, so 4 players at most. Each controller's light bar shows its player (blue,
red, green, pink), and a controller that comes or goes during a game is announced on screen. Every player has the
same buttons (Controls); the in-game menu, fast forward and the touch screens are player 1's.

| Console | Players | How |
|---|---|---|
| Super Nintendo | 3-5 | Super Multitap in port 2 |
| NES | 3-4 | Four Score; for Famicom games, the Famicom's 4 player adapter |
| Genesis | 3-5 | Team Player in port 1 (and a pad in port 2), EA's 4 Way Play for EA's games; J-Cart games (Micro Machines 2, Pete Sampras...) have the extra ports on the cartridge |
| PlayStation | 3-5 | Multitap in port 2 |
| Saturn | 3-6 | 6 Player Adaptor (**On** only) |
| PC Engine | 3-5 | TurboTap, always connected |
| N64, Dreamcast, NAOMI, Atomiswave, Arcade | 3-4 | the console's own four ports |
| OpenBOR | 3-4 | the game's own players (up to 4) |

**Multiplayer Adapter** (a game's settings, *System*) chooses the adapter: **Auto** (the default) plugs it in
only for the games known to use it - the emulator's own list (the NES, the J-Cart), the game's header (the
Genesis), lists of the adapter's games (Super Bomberman, Secret of Mana, NHL '94, Crash Team Racing...) - so 1-2
player games play exactly as before; **Off** never; **On** always, for a game Auto doesn't know (a game made
without the adapter may then misread the pads).

## Online play

Two PS4s anywhere in the world play one game as if both controllers were on one console. EmuSuNi uses its own
online system (not RetroArch's) with **rollback**, the technique fighting games use: the game keeps running
without waiting, and fixes itself if the other player's button arrives late.

- **Consoles**: NES, Super Nintendo, Genesis, Master System, Game Gear, SG-1000, PC Engine, Arcade, Atari 2600
  and Atari 7800 (the lighter ones). They show a small globe in the menu.
- **The Online tab** lists every room with its game, console, host, players, type and ping. **Cross** joins,
  **Square** creates a room, **Triangle** joins with a code, **L2 / R2** filter.
- **Rooms**: a game's **Triangle → Play Online** makes a **private room** (a 6-letter code to tell a
  friend) or a **public room** (in everyone's list), for **2, 3 or 4 players** (**Players**, on the consoles that
  have them). **Choose Together** makes a room without a game: when your friend comes in, you both see, console by
  console, the games you both have.
- **The Game Boy's link cable**: on a Game Boy or Game Boy Color game, **Triangle → Link Cable Online** makes a
  room for two; your friend picks their own game (**Choose Your Game**), and you trade or battle as with a real
  cable. Each PS4 shows its own Game Boy and keeps its own save.
- **Both PS4s need the same EmuSuNi version and the same game file** (the app compares them and tells you if you
  have another version).
- **Your room stays open** while you use the menus (**Circle** goes back to the menu; **Close Room** closes it).
  When someone asks to join, a notice lets you **accept** or **reject** them.
- **Voice chat** (**Settings → Voice Chat**): the microphone of the user playing (a headset on the
  DualShock 4, a USB microphone or the PlayStation Camera). Open microphone or push to talk, volumes and a
  **Test Microphone**. It starts **off in public rooms** (each player turns theirs on), and you can mute any player.
- **The connection** is direct between the two PS4s when it can be, and goes through a relay server when it
  can't. **Hide My Address** always uses the relay, so the other player never learns your address.
- **Nickname and profile**: your nickname is the PS4 user's unless you type one (Online tab → Online
  Settings). The **Online Profile** is optional and off by default ([details](#my-stats-and-the-online-profile)).
- **Latency Bar**: each player's ping on the screen (green, amber, red); it can be turned off in the Online
  settings or the in-game menu.
- While you play online there is no saving or loading states, rewind, cheats or pause: the match goes on. The
  Arcade's high-score tables are off online (your own stay as they were).

### Online with the community's servers: PSP, Dreamcast, DS

These consoles play online the way they did on the real hardware, through free servers that fans keep running
(not EmuSuNi's). Each is a **game setting** (**Triangle → Game Settings → Emulation**, or the in-game menu's
Settings), and nothing is sent until the game itself connects. As with the real console, those servers see your
public IP address.

| Console | Setting | What it plays |
|---|---|---|
| **PSP** | **PSP Online** (Off by default) and **PSP Ad Hoc Server** | The games whose servers fans revived (MotorStorm Arctic Edge, WipEout Pulse, FIFA, SOCOM...), from the game's own network menu, and ad hoc play (Monster Hunter, Tekken 6, Dissidia...) through the chosen ad hoc server's relay: players who play together choose the same server. Your name in the games is your online nickname |
| **Dreamcast** | **Dreamcast Online**: Off (the default), DCNet (modem) or DCNet (broadband adapter) | DCNet, Flycast's free servers: ChuChu Rocket! and the other modem games; the broadband adapter for the games made for it. Some games ask for a name the first time and keep it on the VMU |
| **Nintendo DS** | **DS Wi-Fi**: Wiimmfi (the default), WiiLink WFC or Off | The games' Nintendo Wi-Fi Connection (Mario Kart DS, Pokémon...). Friends registered on one server aren't on the other |

## Portable mode

Take your games' saves, states, cheats, covers, bezels, themes, settings, controls and game list on a USB
drive, and play with them on another PS4 that has EmuSuNi (a friend's, for example).

- **Settings → Keep My Data on the USB Drive → Activate** copies your data to an `EmuSuNi-Portable` folder in the
  drive's root and restarts the app. Nothing on your PS4 is moved or deleted.
- **On another PS4** with EmuSuNi, plugging the drive in asks once "Use the data on this drive?". Its own data
  is not touched and comes back when the drive is removed.
- **Not copied**: the RetroAchievements login, the online profile and nickname, the statistics' number, My
  Stats, logs and the BIOS files (put a BIOS in the drive's `EmuSuNi-Portable/system` yourself if you want it there).
- If you pull the drive out while playing, the app says so and keeps what you save in
  `/data/EmuSuNi/portable-fallback` (never over the PS4's own saves); the next time the drive is in, it is copied back.
- **Turn Off** offers to copy the drive's data back to the PS4.

## Frequently asked questions

**Do I lose my games and saves when I update?** No. Everything lives in `/data/EmuSuNi` (or on your drive in
portable mode) and a new version leaves it alone. Even deleting and reinstalling the app does not touch it.

**Why do some games have no box art?** The app downloads covers by the game's name from libretro's
thumbnails: the PS4 needs internet, and the file should be named as the game is known (No-Intro style:
`Super Mario World (USA).sfc`). Otherwise put your own PNG or JPEG named like the game in
`covers/<console>`, or use **Triangle → Find Cover**. **Settings → Games Without Cover** says why a picture
isn't used. See [Box art](#box-art).

**Which 3DS games work?** Only **decrypted** ones (`.3ds`, `.cci`, `.cxi`, `.3dsx`): encrypted games and
`.cia` files don't load. The 3DS is experimental and the heaviest console: a PS4 Pro runs it better.

**My Arcade game doesn't start.** EmuSuNi uses **FinalBurn Neo 1.0.0.03** sets: one `.zip` per game, not
unpacked, with the set's name in small letters (`mslug.zip`). A set from another MAME or FinalBurn version may
have different files and won't start. Neo Geo games also need your `neogeo.zip` next to them. NAOMI and
Atomiswave need `naomi.zip` / `awbios.zip`. See [BIOS files](#bios-files).

**How do I play the Saturn?** Put your Saturn BIOS (a 512 KB `.bin`) in `/data/EmuSuNi/system/saturn` and the
games (`.cue` with its `.bin` tracks, or `.chd`) in `Roms/saturn`. It is experimental: it draws in software and
some games run slower than on the real console.

**How do I play OpenBOR games?** OpenBOR's games are free beat 'em ups made by its fans, each a `.pak` file.
EmuSuNi includes none: copy the `.pak` of a game you have, as it is, to `Roms/openbor` on the USB drive or
`/data/EmuSuNi/Roms/openbor`, and it shows up in the **OpenBOR** tab. EmuSuNi plays the games made for OpenBOR
3.0 (Build 6391, most of them) and 4.0 (Build 8025).

**Does it work on a PS5?** EmuSuNi is made and tested for the jailbroken PS4. Some players run it on a PS5
through backward compatibility, but that is not tested or supported.

**How do I install it?** Copy the `.pkg` to a USB drive (FAT32 or exFAT) and install it with GoldHEN's
**Package Installer** ([Installation](#installation)). Then put your games in `Roms/<console>` on the drive.

**Does it send anything about me?** Only the [anonymous statistics](#anonymous-statistics), which you can turn off
in **Settings → Anonymous Statistics**, and your log when you choose **Settings → Send Log**. Nothing is ever sent
about your files, names or PS4.

## Settings

**Settings** in the menu holds the app's own options: Themes, Language, Controls, RetroAchievements,
Game Folders, Console Order, Consoles Shown, Clean Up Games, Download Covers, Games Without Cover, Game
Bezels, Star Ratings, Sound Effects, Menu Style, **Menu Size**, Clock, **Voice Chat**, **Online Play**, **My Stats**,
**Anonymous Statistics**, **Keep My Data on the USB Drive**, Reset All Settings, Check for Updates, **Send Log**,
**How to Help**...

A game's settings are set from the game: **Triangle → Game Settings** in the menu, or **Settings**
in the in-game menu. Each one can apply to **all games**, to **a console** or to **one game**
(*Apply To* at the top), and a short text under the list explains it and its current option.

| Category | Settings |
|---|---|
| **Video** | **Quality** (Low, Medium, High, Ultra) · Resolution · N64, PS1, PSP, Dreamcast and 3DS Resolution · Aspect Ratio · the DS's and the 3DS's Screen Layout, Swap Screens, Small Screen Size, Small Screen Position and Gap Between Screens · PS1 Widescreen · Picture Size · Letterbox Zoom · **Bezel** · Crop Overscan · Frame Rate Limit · V-Sync · FPS Counter |
| **Graphics** | Upscaling Filter · **GPU Upscaler** · Texture Filtering · the N64's anti-aliasing, dither filter, 2D, distant textures and screen effects · the PlayStation's geometry (PGXP), texture filtering, colours and anti-aliasing · the PSP's texture sharpness, texture upscaling and anti-aliasing · the Dreamcast's texture sharpness and transparency · the 2600's Phosphor · NTSC Filter · Scanlines · **Tube TV (CRT)** · Hi-Res Blending · Reduce Sprite Flicker · Transparency Effects · **Game Boy Colours** · Screen Colours · LCD Ghosting |
| **Performance** | PSP Frame Rate (God of War) · 3DS CPU Clock · Dreamcast CPU Clock · Dreamcast Sound DSP · **Jaguar Recompiler** and Jaguar GPU/DSP Speed · Emulated CPU Clock · SuperFX Clock · the N64's graphics sync, CPU cores, picture timing, CPU timing and frame pacing · PS1 Disc Speed · Emulation Speed · Fast Forward · Frame Skip · Performance Graphs |
| **Emulation** | Region · NES Palette · Turbo Buttons · the N64's CPU, RSP and Controller Pak · Boot Logo (BIOS) · Sunlight (Boktai) · Tilt Sensor · PS1 Controller · PS1 Boot Logo · PSP Stick Dead Zone and Sensitivity · DS and 3DS Touch Screen · New 3DS Mode · the 3DS's Accurate Shader Multiplication and Right Eye · PC Engine Pad · Dreamcast Language · Jaguar BIOS · **PSP Online**, PSP Ad Hoc Server, **Dreamcast Online** and **DS Wi-Fi** ([online](#online-with-the-communitys-servers-psp-dreamcast-ds)) · Allow Opposing Directions · Randomize Memory and more |
| **System** | Game Volume · Audio Interpolation · Sound Filter · Audio Buffer · **Multiplayer Adapter** · Resume Where You Left Off |

The Dreamcast's, NAOMI's and Atomiswave's games show only the settings that apply to them, Quality's
four levels among them (up to 3x).

### Anonymous statistics

So we know how many people use EmuSuNi, where, and with which version, the app sends a small
anonymous heartbeat to EmuSuNi's online server. It goes when the app starts, every 5 minutes while it
is open, and after an update check. It also gives Online Play its count of players online now.
It is **on by default**: **Settings → Anonymous Statistics → Off** turns it off, and then nothing at all is sent.

- **What is sent**: a random number this app makes once (`stats.ini` — not the PS4's, a user's or
  PSN's id), the app's version, the PS4's language, what the app is doing (the menus, a game of which
  console, online play), **the game being played** — only as a checksum of its cartridge, an arcade
  set's short name or a disc's code, never its file's name —, how many update checks since the last
  heartbeat, and, once after an update, the version before it.
- **What is not sent**: no names, no file names or game list, no files or saves, no PS4, user or PSN id.
- **What the server keeps**: counts by country, version, language and console. The country comes from
  the connection's address, looked up when the heartbeat arrives (DB-IP's free database); the address
  itself is never stored or logged. A day's heartbeats are counted once per install through a hash of its
  number whose salt is deleted when the day ends; the weeks' and months' totals come from a
  statistical summary (HyperLogLog) from which no install's number can be read back.
- **What is public**: [EmuSuNi's page](https://emusuni.com) shows totals only
  (players online now, countries, versions, consoles, the most played games, the public rooms without
  nicknames or codes). A game's title comes from public game lists (No-Intro, Redump, FinalBurn Neo),
  never from the app, and is shown only once 3 or more installs have played it.

### My Stats and the online profile

- **My Stats** (Settings → My Stats, also in the Online tab): your time played by game and console,
  sessions, last played, online matches and RetroAchievements numbers. Kept on your PS4 only
  (`mystats.ini`), never sent.
- **Online Profile** (the **Online** tab; **off** unless you turn it on): EmuSuNi's server keeps this
  PS4's history — minutes, sessions and online matches by game and console, its country — under a
  hashed form of its random number, and gives you a **profile code** that opens it on the page's
  **Statistics → My profile**. Without the code nobody can find or read it. **Public Profile** (off
  unless you turn it on) also puts your nickname, country and hours in the page's rankings. **Delete
  My Profile** deletes it all on the server (even with the statistics off); a profile unused for 400
  days is deleted by itself. Without the online profile, nothing is kept per PS4.

## Files on the PS4

Everything lives in `/data/EmuSuNi` (reachable by FTP with GoldHEN):

| Path | Contents |
|---|---|
| `games/`, `nes/` | The Super Nintendo's and the NES's default game folders |
| `covers/` | Box art: a folder per console, named as the games' folders (`covers/snes`, `covers/nes`, `covers/md`...); PNG or JPEG |
| `saves/` | Battery saves and the cartridges' clocks (`.rtc`), the PlayStation's memory cards, the PSP's memory stick (`saves/psp`), the Dreamcast's VMUs (`saves/dc`), the Saturn's backup memory (`saves/saturn`), OpenBOR's games' saves and high scores (`saves/openbor`) |
| `states/` | Save states |
| `cheats/` | Cheats: `<game file name>.cht` (RetroArch format), in its console's subfolder (`cheats/nes`, `cheats/ps1`...; the Super Nintendo's right in `cheats/`). If a game has no cheats yet, **Triangle → Cheats** shows the exact folder and name |
| `system/` | BIOS files (see [BIOS](#bios-files)) |
| `cache/` | The online cheat collection (kept a month), the lists of games on several discs, RetroAchievements' lists, pictures and translations, the star ratings, The Bezel Project's lists |
| `bezels/` | Your bezels: a 1920×1080 PNG, see-through where the game goes, in `bezels/<console>/`; the ones **Settings → Game Bezels** downloads go there too |
| `themes/` | Wallpapers (PNG, 1920×1080) and sounds that replace the built-in ones |
| `settings.ini`, `controls.ini`, `xmb.ini`, `favorites.ini` | Your settings |
| `retroachievements.ini` | Your RetroAchievements login (its token, never the password) |
| `stats.ini` | The [anonymous statistics](#anonymous-statistics): on or off, this app's random number, the online profile (on or off, public or not, its code) |
| `mystats.ini` | [My Stats](#my-stats-and-the-online-profile): your time played, sessions and online matches by game (never sent) |
| `netplay.ini` | [Online play](#online-play): your nickname, the latency bar and, if you want, another server |
| `psp-mac.txt` | The PSP's own network address for [PSP Online](#online-with-the-communitys-servers-psp-dreamcast-ds), made at random once |
| `portable-drives.ini`, `portable-fallback/` | [Portable mode](#portable-mode): the drives this PS4 answered about; what was saved while the drive was away |
| `emusuni.log` | The log, for bug reports (`emusuni-1.log` to `emusuni-4.log`: the sessions before) |

## Building from source

The build runs on **Ubuntu 24.04** (WSL2 works) with clang 21, PacBrew's OpenOrbis toolchain and
DolphinPS4's PS4 runtime (libc++ 21 and Mesa's RADV Vulkan driver).

```sh
# once: packages, toolchain, libc++, Mesa, every console's core and Dear ImGui (as root)
sudo -i bash /path/to/EmuSuNi/scripts/setup.sh

# then, as root too (setup.sh puts the sources and the PS4 runtime in root's home)
sudo -i
cd /path/to/EmuSuNi
EMUSUNI_VERSION=00.23 bash scripts/build.sh with-saturn with-gblink with-openbor   # builds ~/emusuni-build/emusuni_eboot/eboot.bin
bash scripts/package.sh 00.23                                                     # makes the .pkg in ~/emusuni-build/out
```

`scripts/build.sh` builds each core first (`scripts/build-*-core.sh`, again only what changed);
`no-n64`, `no-ps1`, `no-nds`, `no-psp`, `no-dc`, `no-sega`, `no-arcade`, `no-3ds`, `no-pce`, `no-atari` or `no-jaguar`
leave a console out, and `with-saturn`, `with-gblink` (the Game Boy's link cable online) and `with-openbor` put
the Saturn, the link cable and OpenBOR in (the releases are built with them). `scripts/make-assets.py`
regenerates the icon, the start-up pictures, the logo and the 8-bit sounds (Python + Pillow);
`scripts/make-bezels.py` the bezels.

<details>
<summary>Under the hood</summary>

- **Display**: Vulkan through Mesa's RADV driver running on the PS4's GNM interface (from
  DolphinPS4), three 1920×1080 VideoOut buffers. The menus are drawn with Dear ImGui.
- **Cores**: every emulator is a libretro core linked into the app, its API renamed
  (`snes9x_retro_*`, `flycast_retro_*`...), so one process plays every console.
- **3D on the PS4's GPU**: the Nintendo 64 (ParaLLEl-RDP), the PlayStation, the PSP, the
  Dreamcast (with NAOMI and Atomiswave) and the 3DS draw with their own Vulkan renderers on the app's
  Vulkan device. The Saturn draws in software.
- **Threads**: the N64's RDP, the DS's 3D (on two threads), the PSP's and DS's drawing and the
  Saturn's VDP1 and VDP2 run beside the emulation, on cores of their own.
- **GPU filters**: xBRZ, MMPX, SMAA, Anime4K and AMD FSR/RCAS run as Vulkan fragment-shader passes
  before the menu is drawn; their pipelines are compiled in the background and cached.
- **Memory**: the PS4's system C library serves allocations from a ~12 MiB pool shared with the
  kernel's own objects. All of the app's allocations — C `malloc` and C++ `new` — go to a
  dlmalloc heap in system flexible memory instead (`port/`), behind a blocking lock (the PS4 runs
  threads by fixed priorities).
- **Covers**: libretro's thumbnail server over plain HTTP, with GitHub as a fallback; pictures are
  shrunk to the size the menu shows.

</details>

## Credits

- The emulators: **[Snes9x](https://github.com/snes9xgit/snes9x)** (Super Nintendo),
  **[FCEUmm](https://github.com/libretro/libretro-fceumm)** (NES),
  **[Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx)** with ParaLLEl-RDP and
  ParaLLEl-RSP (Nintendo 64), **[mGBA](https://mgba.io)** (Game Boy, Color and Advance),
  **[SwanStation](https://github.com/libretro/swanstation)**, based on
  [DuckStation](https://github.com/stenzek/duckstation) (PlayStation),
  **[melonDS DS](https://github.com/JesseTG/melonds-ds)**, based on
  [melonDS](https://melonds.kuribo64.net) (Nintendo DS), **[Azahar](https://azahar-emu.org)**
  (Nintendo 3DS), **[PPSSPP](https://www.ppsspp.org)** (PSP),
  **[Flycast](https://github.com/flyinghead/flycast)** (Dreamcast, NAOMI and Atomiswave; with parts of
  [Resonance](https://github.com/iHaiDeeZ/Resonance)'s fast memory),
  **[Genesis Plus GX](https://github.com/libretro/Genesis-Plus-GX)** (SG-1000, Master System, Game Gear
  and Genesis), **[YabaSanshiro](https://github.com/libretro/yabause/tree/yabasanshiro)** (Saturn),
  **[Beetle PCE Fast](https://github.com/libretro/beetle-pce-fast-libretro)**, based on
  [Mednafen](https://mednafen.github.io) (PC Engine), **[Stella](https://stella-emu.github.io)** (Atari 2600),
  **[ProSystem](https://github.com/libretro/prosystem-libretro)** (Atari 7800),
  **[Virtual Jaguar](https://github.com/libretro/virtualjaguar-libretro)** (Atari Jaguar),
  **[FinalBurn Neo](https://github.com/finalburnneo/FBNeo)** (Arcade),
  **[SameBoy](https://sameboy.github.io)** (the Game Boy's link cable online) and
  **[OpenBOR](https://github.com/DCurrent/openbor)**, in [zanac's libretro core](https://github.com/zanac/openbor-6391-libretro).
- **[RetroAchievements](https://retroachievements.org)** and its rcheevos library, the
  **[LaunchBox Games Database](https://gamesdb.launchbox-app.com)**'s ratings and
  **[The Bezel Project](https://github.com/thebezelproject)**'s bezels, downloaded by the app.
- **[Anime4K](https://github.com/bloc97/Anime4K)**, **AMD FidelityFX Super Resolution**,
  **[SMAA](https://github.com/iryoku/smaa)** and **MMPX** — the AI-enhanced HD picture, the smooth
  upscaling and the clean edges.
- **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — the launcher's design, the PS4
  runtime, the Mesa port and the build setup that EmuSuNi grew from.
- **[Opus](https://opus-codec.org)** for the voice chat, and the community servers that keep the PSP's, the
  Dreamcast's (DCNet) and the DS's (Wiimmfi, WiiLink WFC) online games alive.
- **[Dear ImGui](https://github.com/ocornut/imgui)**, **[Mesa](https://mesa3d.org)**,
  **[OpenOrbis](https://github.com/OpenOrbis) / [PacBrew](https://github.com/PacBrew)**,
  **[libretro-thumbnails](https://github.com/libretro-thumbnails)**, libretro's cheat collection,
  xBRZ, HQx, blargg's NTSC filter, and the fonts' authors. See
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

EmuSuNi by [@arturofuentes1988](https://github.com/arturofuentes1988).

## License and legal

EmuSuNi's own code is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)). Snes9x,
Genesis Plus GX and FinalBurn Neo are under their own licenses, which allow free, **non-commercial**
use only; the EmuSuNi packages are free builds, and EmuSuNi asks for no donations. The other emulators
and components keep their licenses ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

**No games, BIOS files or OpenBOR games are included.** Use games you own. EmuSuNi is not affiliated with or
endorsed by Nintendo, Sony, Sega, Atari or any other maker of these consoles and games, nor by the services its
online play connects to. *Super Nintendo*, *Super Famicom*, *SNES*, *NES*, *Nintendo 64*, *Game Boy*, *Game Boy
Color*, *Game Boy Advance*, *Nintendo DS* and *Nintendo 3DS* are trademarks of Nintendo; *PlayStation*, *PSP* and
*PS4* are trademarks of Sony Interactive Entertainment; *Dreamcast*, *Saturn*, *Genesis*, *Mega Drive*, *Master
System*, *Game Gear* and *NAOMI* are trademarks of Sega; *Atari 2600*, *Atari 7800* and *Jaguar* are trademarks of
Atari; the other names are trademarks of their owners.
