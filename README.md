<p align="center">
  <img src="docs/banner.png" alt="EmuSuNi" width="860">
</p>

<p align="center">
  <b>Ten classic consoles in one app for jailbroken PS4 consoles.</b><br>
  Super Nintendo · NES · Nintendo 64 · Game Boy · Game Boy Color · Game Boy Advance ·
  PlayStation · Nintendo DS · PSP · Dreamcast
</p>

<p align="center">
  A launcher that feels like a console's own: your games with their box art, an AI-enhanced HD
  picture, cheats from the internet, save states, bezels and settings for every game.
</p>

<p align="center">
  <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest"><img alt="Download" src="https://img.shields.io/github/v/release/arturofuentes1988/EmuSuNi?label=download&style=for-the-badge&color=7b4dff"></a>
  <img alt="PS4 with GoldHEN" src="https://img.shields.io/badge/PS4-GoldHEN-003791?style=for-the-badge&logo=playstation&logoColor=white">
  <img alt="10 consoles" src="https://img.shields.io/badge/consoles-10-e5484d?style=for-the-badge">
  <a href="LICENSE"><img alt="License GPL-2.0+" src="https://img.shields.io/badge/license-GPL--2.0%2B-3db46d?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.es.md">Leer en español</a> · <a href="CHANGELOG.md">Changelog</a> · <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest">Download</a></p>

---

## The ten consoles

| Console | Emulator | Highlights |
|---|---|---|
| **Super Nintendo** | Snes9x 1.63 | Every chip (SuperFX, SA-1, DSP...), hi-res modes, up to 5 players |
| **NES** and Famicom Disk System | FCEUmm | Palettes, turbo buttons, NTSC filter |
| **Nintendo 64** | Mupen64Plus-Next | Drawn by the PS4's GPU up to 4x (ParaLLEl-RDP), Controller Pak and Rumble Pak |
| **Game Boy** and **Game Boy Color** | mGBA | Game Boy Color or Super Game Boy colours, bezels shaped like each handheld |
| **Game Boy Advance** | mGBA | Tilt and sunlight sensors, rumble, the cartridges' clock |
| **PlayStation** — *new* | SwanStation | HD 3D up to 4x, steady polygons (PGXP), True Colour, both analog sticks, games on several discs |
| **Nintendo DS** — *new* | melonDS DS 1.4.0 | Three screen layouts in DS Screens (R3 swaps between yours and Side by Side, or Big Top Screen), the touchpad is the touch screen, L3 blows into the mic |
| **PSP** — *new* | PPSSPP 1.20.4 | Up to 4x (Full HD) with texture upscaling, no BIOS needed |
| **Dreamcast** — *new* | Flycast 2.7 | Analog triggers on L2 / R2, a VMU for each game, rumble, games on several discs |

No BIOS files are needed to start, except `disksys.rom` for Famicom Disk System games; the real ones
are used when you add them ([BIOS](#bios-files)).

## What's new in 00.20

- **PlayStation, Nintendo DS, PSP and Dreamcast** join the six consoles of 00.19.
- **Games on several discs** (PlayStation, Dreamcast) show up as one game; the in-game menu changes the disc.
- **Find Cheats Online**: libretro's cheat collection, searched for your game.
- **Smooth Edges, Natural HD and Natural AI**: three new GPU filters, for the Medium, High and Ultra
  Quality levels (coming from 00.19, choose your level again in **Triangle → Game Settings → Quality**).
- **Console Order**: the consoles in your order, or By Company, By Power, By Year or Alphabetical.
- A game's settings and controls are set from the game itself (**Triangle**).
- For now the Dreamcast always plays at Low quality: the higher levels are too heavy for the PS4.

All the details: the [release notes](https://github.com/arturofuentes1988/EmuSuNi/releases/latest) and the [changelog](CHANGELOG.md).

## Screenshots

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
- **Quality** in one setting: Low, Medium, High or **Ultra** (the most each console can do).
  Medium, High and Ultra each have their GPU filter: Smooth Edges, Natural HD and Natural AI (Low
  has none).
- **GPU Upscaler**: Smooth Edges (MMPX and SMAA), Natural HD (AMD FSR), Natural AI (the Anime4K
  neural network, without the waxy look), Pixel Art HD (xBRZ), AI (xBRZ + Anime4K), Smooth HD and
  HD Sharpen, at the TV's resolution.
- **HD 3D**: the Nintendo 64 and the PlayStation up to 4x, the PSP up to 4x with texture upscaling.
- **Resolution** 2x–6x with xBRZ, HQx or Sharp Pixels; aspect ratio, integer scaling, zoom,
  scanlines (on the Game Boys, the grid of their screen), NTSC filter, V-Sync.
- **Bezels** (every console but the DS, the PSP and the Dreamcast): three per console plus a retro
  TV (the Game Boys shaped like each handheld; the PlayStation, the PSone and Midnight), or your own.

**Playing**
- Battery saves written automatically, **8 save-state slots** with screenshots, *resume where you
  left off*, reset.
- **Cheats**: RetroArch `.cht` files, or **Find Cheats Online** (every console but the Dreamcast).
- **Games on several discs** grouped by themselves, with **Disc** in the in-game menu.
- **The in-game menu** (L3 + R3): save and load states, change the disc, settings, cheats, reset,
  quit.
- Up to 5 DualShock 4 controllers, rumble, fast forward.

**A launcher like a console's**
- Two menu styles: **Library** (box art in a row, a tab per console on L1 / R1, going round) and
  **XMB** (the PSP-style columns), 12 colour themes or your own wallpapers, 8-bit sounds.
- **Box art, automatically** from libretro's thumbnail server.
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
  | PlayStation | `.cue` (with its `.bin` tracks) `.chd` `.pbp` `.m3u` `.iso` `.img` `.ecm` `.mds` |
  | Nintendo DS | `.nds` `.ids` |
  | PSP | `.iso` `.cso` `.chd` `.pbp` `.elf` |
  | Dreamcast | `.gdi` (with its tracks) `.chd` `.cdi` `.cue` `.m3u` |

  The cartridge consoles (Super Nintendo to Game Boy Advance) take `.zip` files too; the disc
  consoles and the DS read their games as they are.
- Internet on the PS4 for the box art and the online cheats (optional).

## Installation

1. Download **`EmuSuNi-vXX.YY.pkg`** from the [latest release](https://github.com/arturofuentes1988/EmuSuNi/releases/latest).
2. Copy it to a USB drive (FAT32 or exFAT), for example in a `PKG` folder.
3. On the PS4, install it with GoldHEN's **Package Installer** (or *Settings → Debug Settings →
   Game → Package Installer*).
4. Start **EmuSuNi** from the home screen.

To update, install the new package over the old one: your settings, saves and covers stay in
`/data/EmuSuNi`.

## Where the games go

The easiest is a **USB drive** with a folder per console:

```
USB drive
└── Roms
    ├── snes    (Super Nintendo)
    ├── nes
    ├── n64
    ├── gb
    ├── gbc
    ├── gba
    ├── ps1     (PlayStation)
    ├── nds     (Nintendo DS)
    ├── psp
    └── dc      (Dreamcast)
```

- **Every console but the Super Nintendo and the NES** looks in its `Roms` folder on the USB drive
  without any setup.
- **Super Nintendo and NES** start in `/data/EmuSuNi/games` and `/data/EmuSuNi/nes` (on the PS4,
  by FTP).
- **Any folder works**: in **Settings → Game Folders** you choose each console's, on a USB drive or
  on the PS4. Subfolders are searched too.
- After copying games, press **Square** in the Library to look for them again.

**Games on several discs** (PlayStation, Dreamcast) become one game by themselves when:
- their files sit side by side with the disc in their names: `Game (Disc 1).chd`, `Game (Disc 2).chd`
  (also `(CD2)`, `(Disk 2)`, `[Disc 2]`);
- each disc has a folder: `Game (Disc 1)/disc.gdi`, or `Game/Disc 1/disc.gdi`;
- or an `.m3u` file lists them.

In the game, **L3 + R3 → Disc** changes the disc (Left / Right to choose, Cross to put it in). The
next time, the game starts with the disc you played last.

## BIOS files

None are needed to play, except `disksys.rom` for the Famicom Disk System. Copy the real ones by FTP
if you have them:

| Console | Files | Folder |
|---|---|---|
| PlayStation (recommended: more games work) | `scph5501.bin`, `scph5500.bin`, `scph5502.bin` or another | `/data/EmuSuNi/system` |
| Nintendo DS (all three) | `bios7.bin`, `bios9.bin`, `firmware.bin` | `/data/EmuSuNi/system` |
| Dreamcast | `dc_boot.bin` | `/data/EmuSuNi/system/dc` |
| Famicom Disk System (required) | `disksys.rom` | `/data/EmuSuNi/system` |
| Game Boy, Color, Advance | `gb_bios.bin`, `gbc_bios.bin`, `sgb_bios.bin`, `gba_bios.bin` | `/data/EmuSuNi/system` |

The PSP needs no BIOS.

## Box art

- **Downloaded automatically** (the PS4 needs internet) from libretro's thumbnail server; the
  progress shows under *Your games*. *Settings → Download Covers* looks again.
- **Your own work too**: a PNG named like the game's file or its title in `/data/EmuSuNi/covers`
  (Super Nintendo) or in its console's subfolder: `covers/nes`, `n64`, `gb`, `gbc`, `gba`, `ps1`,
  `nds`, `psp`, `dc`.
- When one is missing, **Triangle → Find Cover** shows the closest ones to pick from.

## Controls

**L3 + R3** (both sticks pressed) opens the in-game menu on every console. Every button can be
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

| Menu (Library style) | |
|---|---|
| D-Pad Left / Right | Previous / next game (Up / Down: 10 at a time) |
| L1 / R1 | Tabs: All, each console, Favorites, Settings |
| Cross | Play |
| Triangle | Game options: favorite, game settings, cheats, controls, rename, find cover, information, default game, delete |
| L2 / R2 | Favorite / search |
| Square | Look for the games again |
| Options | Sort |
| Circle | Back |

Hold **L1** while EmuSuNi starts to open the menu instead of the default game.

## Settings

**Settings** in the menu holds the app's own options: Game Folders, Console Order, Clean Up Games,
Download Covers, Themes, Language, Menu Style, Sound Effects, Clock, Reset All Settings...

A game's settings are set from the game: **Triangle → Game Settings** in the menu, or **Settings**
in the in-game menu. Each one can apply to **all games**, to **a console** or to **one game**
(*Apply To* at the top), and a short text under the list explains it and its current option.

| Category | Settings |
|---|---|
| **Video** | **Quality** (Low, Medium, High, Ultra) · Resolution · N64, PS1 and PSP Resolution · Aspect Ratio · DS Screens · PS1 Widescreen · Picture Size · Letterbox Zoom · **Bezel** (not for the DS, PSP and Dreamcast) · Crop Overscan · Frame Rate Limit · V-Sync · FPS Counter |
| **Graphics** | Upscaling Filter · **GPU Upscaler** · Texture Filtering · the N64's anti-aliasing, dither filter, 2D, distant textures and screen effects · the PlayStation's geometry (PGXP), texture filtering, colours and anti-aliasing · the PSP's texture sharpness, texture upscaling and anti-aliasing · NTSC Filter · Scanlines · Hi-Res Blending · Reduce Sprite Flicker · Transparency Effects · **Game Boy Colours** · Screen Colours · LCD Ghosting |
| **Performance** | PSP Frame Rate (God of War) · Emulated CPU Clock · SuperFX Clock · the N64's graphics sync, CPU cores, picture timing, CPU timing and frame pacing · PS1 Disc Speed · Emulation Speed · Fast Forward · Frame Skip · Performance Graphs |
| **Emulation** | Region · NES Palette · Turbo Buttons · the N64's CPU, RSP and Controller Pak · Boot Logo (BIOS) · Sunlight (Boktai) · Tilt Sensor · PS1 Controller · PS1 Boot Logo · Allow Opposing Directions · Randomize Memory and more |
| **System** | Game Volume · Audio Interpolation · Sound Filter · Audio Buffer · Multitap (3-5 Players) · Resume Where You Left Off |

The Dreamcast's games show only the settings that apply to them (they always play at Low quality).

## Files on the PS4

Everything lives in `/data/EmuSuNi` (reachable by FTP with GoldHEN):

| Path | Contents |
|---|---|
| `games/`, `nes/` | The Super Nintendo's and the NES's default game folders |
| `covers/` | Box art (a subfolder per console: `nes`, `n64`, `gb`, `gbc`, `gba`, `ps1`, `nds`, `psp`, `dc`) |
| `saves/` | Battery saves and the cartridges' clocks (`.rtc`), the PlayStation's memory cards, the PSP's memory stick (`saves/psp`), the Dreamcast's VMUs (`saves/dc`) |
| `states/` | Save states |
| `cheats/` | Cheats: `<game file name>.cht` (RetroArch format), in its console's subfolder (`cheats/nes`, `cheats/ps1`...; the Super Nintendo's right in `cheats/`). If a game has no cheats yet, **Triangle → Cheats** shows the exact folder and name |
| `system/` | BIOS files (see [BIOS](#bios-files)) |
| `cache/` | The online cheat collection (kept a month) and the lists of games on several discs |
| `bezels/` | Your bezels: a 1920×1080 PNG, see-through where the game goes, in `bezels/<console>/` |
| `themes/` | Wallpapers (PNG, 1920×1080) and sounds that replace the built-in ones |
| `settings.ini`, `controls.ini`, `xmb.ini`, `favorites.ini` | Your settings |
| `emusuni.log` | The log, for bug reports |

## Building from source

The build runs on **Ubuntu 24.04** (WSL2 works) with clang 21, PacBrew's OpenOrbis toolchain and
DolphinPS4's PS4 runtime (libc++ 21 and Mesa's RADV Vulkan driver).

```sh
# once: packages, toolchain, libc++, Mesa, the ten consoles' cores and Dear ImGui (as root)
sudo -i bash /path/to/EmuSuNi/scripts/setup.sh

# then, as root too (setup.sh puts the sources and the PS4 runtime in root's home)
sudo -i
cd /path/to/EmuSuNi
EMUSUNI_VERSION=00.20 bash scripts/build.sh   # builds ~/emusuni-build/emusuni_eboot/eboot.bin
bash scripts/package.sh 00.20                 # makes the .pkg in ~/emusuni-build/out
```

`scripts/build.sh` builds each core first (`scripts/build-*-core.sh`, again only what changed);
`no-n64`, `no-ps1`, `no-nds`, `no-psp` or `no-dc` leave a console out. `scripts/make-assets.py`
regenerates the icon, the start-up pictures, the logo and the 8-bit sounds (Python + Pillow);
`scripts/make-bezels.py` the bezels.

<details>
<summary>Under the hood</summary>

- **Display**: Vulkan through Mesa's RADV driver running on the PS4's GNM interface (from
  DolphinPS4), three 1920×1080 VideoOut buffers. The menus are drawn with Dear ImGui.
- **Cores**: every emulator is a libretro core linked into the app, its API renamed
  (`snes9x_retro_*`, `flycast_retro_*`...), so one process plays all ten consoles.
- **3D on the PS4's GPU**: the Nintendo 64 (ParaLLEl-RDP), the PlayStation, the PSP and the
  Dreamcast draw with their own Vulkan renderers on the app's Vulkan device.
- **Threads**: the N64's RDP, the DS's 3D (on two threads) and the PSP's and DS's drawing run beside
  the emulation, on cores of their own.
- **GPU filters**: xBRZ, MMPX, SMAA, Anime4K and AMD FSR/RCAS run as Vulkan fragment-shader passes
  before the menu is drawn; their pipelines are compiled in the background and cached.
- **Memory**: the PS4's system C library serves allocations from a ~12 MiB pool shared with the
  kernel's own objects. All of the app's allocations — C `malloc` and C++ `new` — go to a
  dlmalloc heap in system flexible memory instead (`port/`), behind a blocking lock (the PS4 runs
  threads by fixed priorities).
- **Covers**: libretro's thumbnail server over plain HTTP (the PS4's TLS fails on GitHub's
  servers), with GitHub as a fallback; pictures are shrunk to the size the menu shows.

</details>

## Credits

- The emulators: **[Snes9x](https://github.com/snes9xgit/snes9x)** (Super Nintendo),
  **[FCEUmm](https://github.com/libretro/libretro-fceumm)** (NES),
  **[Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx)** with ParaLLEl-RDP and
  ParaLLEl-RSP (Nintendo 64), **[mGBA](https://mgba.io)** (Game Boy, Color and Advance),
  **[SwanStation](https://github.com/libretro/swanstation)**, based on
  [DuckStation](https://github.com/stenzek/duckstation) (PlayStation),
  **[melonDS DS](https://github.com/JesseTG/melonds-ds)**, based on
  [melonDS](https://melonds.kuribo64.net) (Nintendo DS), **[PPSSPP](https://www.ppsspp.org)** (PSP)
  and **[Flycast](https://github.com/flyinghead/flycast)** (Dreamcast).
- **[Anime4K](https://github.com/bloc97/Anime4K)**, **AMD FidelityFX Super Resolution**,
  **[SMAA](https://github.com/iryoku/smaa)** and **MMPX** — the AI-enhanced HD picture, the smooth
  upscaling and the clean edges.
- **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — the launcher's design, the PS4
  runtime, the Mesa port and the build setup that EmuSuNi grew from.
- **[Dear ImGui](https://github.com/ocornut/imgui)**, **[Mesa](https://mesa3d.org)**,
  **[OpenOrbis](https://github.com/OpenOrbis) / [PacBrew](https://github.com/PacBrew)**,
  **[libretro-thumbnails](https://github.com/libretro-thumbnails)**, libretro's cheat collection,
  xBRZ, HQx, blargg's NTSC filter, and the fonts' authors. See
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

EmuSuNi by [@arturofuentes1988](https://github.com/arturofuentes1988).

## License and legal

EmuSuNi's own code is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)). Snes9x is
under its own license, which allows free, **non-commercial** use only; the EmuSuNi packages are
free builds. The other emulators and components keep their licenses
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

**No games or BIOS files are included.** Use games you own. EmuSuNi is not affiliated with or
endorsed by Nintendo, Sony or Sega. *Super Nintendo*, *Super Famicom*, *SNES*, *NES*, *Nintendo 64*,
*Game Boy*, *Game Boy Color*, *Game Boy Advance* and *Nintendo DS* are trademarks of Nintendo;
*PlayStation*, *PSP* and *PS4* are trademarks of Sony Interactive Entertainment; *Dreamcast* is a
trademark of Sega.
