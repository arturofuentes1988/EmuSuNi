<p align="center">
  <img src="docs/banner.png" alt="EmuSuNi" width="860">
</p>

<p align="center">
  <b>Super Nintendo, NES, Nintendo 64, Game Boy, Game Boy Color and Game Boy Advance for jailbroken PS4 consoles.</b><br>
  Six consoles in a console-style launcher: automatic box art, AI-enhanced HD picture, quality
  levels, bezels, favorites, search, and settings for every game.
</p>

<p align="center">
  <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest"><img alt="Download" src="https://img.shields.io/github/v/release/arturofuentes1988/EmuSuNi?label=download&style=for-the-badge&color=7b4dff"></a>
  <img alt="PS4 with GoldHEN" src="https://img.shields.io/badge/PS4-GoldHEN-003791?style=for-the-badge&logo=playstation&logoColor=white">
  <img alt="Snes9x, FCEUmm, Mupen64Plus, mGBA" src="https://img.shields.io/badge/cores-Snes9x%20%C2%B7%20FCEUmm%20%C2%B7%20Mupen64Plus%20%C2%B7%20mGBA-e5484d?style=for-the-badge">
  <a href="LICENSE"><img alt="License GPL-2.0+" src="https://img.shields.io/badge/license-GPL--2.0%2B-3db46d?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.es.md">Leer en español</a> · <a href="CHANGELOG.md">Changelog</a> · <a href="https://github.com/arturofuentes1988/EmuSuNi/releases/latest">Download</a></p>

---

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

**Six consoles**
- **Super Nintendo** with **Snes9x 1.63**: SuperFX, SA-1, DSP and the other chips, hi-res modes,
  **up to 5 players** with the Super Multitap.
- **NES** and the Famicom Disk System with **FCEUmm**: palettes, turbo, NTSC filter.
- **Nintendo 64** with **Mupen64Plus-Next**, ParaLLEl-RDP (the PS4's GPU draws at 1x, 2x or 4x) and
  ParaLLEl-RSP, the dynamic recompiler, Controller Pak and Rumble Pak.
- **Game Boy, Game Boy Color and Game Boy Advance** with **mGBA**: Game Boy Color or Super Game Boy
  colours with their borders, screen colour correction, LCD ghosting, the solar (Boktai) and tilt
  sensors (the right stick, or moving the DualShock 4), rumble, and the cartridges' clock
  (Pokémon).
- Battery saves written automatically, **8 save-state slots** with screenshots, *resume where you
  left off*, RetroArch `.cht` **cheats**, reset.

**Picture**
- **Quality** in one setting: Low, Medium, High or **Ultra** (the most each console can do).
- **GPU Upscaler**: Pixel Art HD (xBRZ), **AI (xBRZ + the Anime4K neural network)**, Smooth HD
  (AMD FSR) and HD Sharpen (AMD RCAS), at the TV's resolution.
- **Resolution** 2x–6x with xBRZ, HQx or Sharp Pixels; aspect ratio, integer scaling, zoom,
  scanlines (on the Game Boys, the grid of their screen), NTSC filter, V-Sync.
- **Bezels**: three per console plus a retro TV (the Game Boys shaped like each handheld), or your
  own.

**A launcher like a console's**
- Two menu styles: **Library** (box art in a row, a tab per console on L1 / R1) and **XMB** (the
  PSP-style columns), 12 colour themes or your own wallpapers, 8-bit sounds.
- **Box art, automatically** from libretro's thumbnail server (GoodSNES and No-Intro names).
- Favorites, search, sorting (A–Z, recently played, region, console), a default game, a folder
  browser, *Clean Up Games* and **13 languages**.
- **Every setting for all games, for a console or for one game**, with a short explanation of each
  option, and **button remapping**.

## Requirements

- A PS4 with **[GoldHEN](https://github.com/GoldHEN/GoldHEN)** (any model).
- Your own games (`.zip` works too):

  | Console | Files |
  |---|---|
  | Super Nintendo | `.sfc` `.smc` `.swc` `.fig` `.bs` |
  | NES | `.nes` `.fds` `.unf` |
  | Nintendo 64 | `.z64` `.n64` `.v64` |
  | Game Boy | `.gb` `.sgb` |
  | Game Boy Color | `.gbc` (and the `.gb` files you put in its folder, like Pokémon Yellow) |
  | Game Boy Advance | `.gba` |
- Internet on the PS4 for the box art (optional).

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
    └── gba
```

- **N64, GB, GBC and GBA** look in `Roms/n64`, `Roms/gb`, `Roms/gbc` and `Roms/gba` on the USB
  drive without any setup.
- **Super Nintendo and NES** start in `/data/EmuSuNi/games` and `/data/EmuSuNi/nes` (on the PS4,
  by FTP).
- **Any folder works**: in **Settings → Game Folders** you choose each console's, on a USB drive or
  on the PS4. Subfolders are searched too.
- After copying games, press **Square** in the Library to look for them again.

## Box art

- **Downloaded automatically** (the PS4 needs internet) from libretro's thumbnail server; the
  progress shows under *Your games*. *Settings → Download Covers* looks again.
- **Your own work too**: a PNG named like the game's file or its title in `/data/EmuSuNi/covers`
  (Super Nintendo) or in its console's subfolder: `covers/nes`, `covers/n64`, `covers/gb`,
  `covers/gbc`, `covers/gba`.
- When one is missing, **Triangle → Find Cover** shows the closest ones to pick from.

## Controls

**L3 + R3** (both sticks pressed) opens the in-game menu on every console. Every button can be
changed in **Settings → Controls** (for a console or for one game).

| DualShock 4 | Super Nintendo | NES | Nintendo 64 | Game Boy / Color | Game Boy Advance |
|---|---|---|---|---|---|
| Cross | B | B | A | B | B |
| Circle | A | A | C Down | A | A |
| Square | Y | Turbo B | B | Turbo B | Turbo B |
| Triangle | X | Turbo A | C Up | Turbo A | Turbo A |
| L1 / R1 | L / R | Disk side / insert (FDS) | L / R | — | L / R |
| L2 | — | — | Z | — | — |
| R2 | Fast forward | Fast forward | Z | Fast forward | Fast forward |
| Options | Start | Start | Start | Start | Start |
| Touchpad | Select | Select | Fast forward | Select | Select |
| Left stick | D-Pad | D-Pad | Stick | D-Pad | D-Pad |
| Right stick | — | — | C buttons | Tilt sensor | Tilt sensor |

| Menu (Library style) | |
|---|---|
| D-Pad Left / Right | Previous / next game (Up / Down: 10 at a time) |
| L1 / R1 | Tabs: All, each console, Favorites, Settings |
| Cross | Play |
| Triangle | Game options: favorite, settings, cheats, controls, find cover, rename, information, default game, delete |
| L2 / R2 | Favorite / search |
| Square | Look for the games again |
| Options | Sort |
| Circle | Back |

Hold **L1** while EmuSuNi starts to open the menu instead of the default game.

## Settings

Every setting can apply to **all games**, to **a console** or to **one game** (*Apply To* at the
top of a game's settings). Under each list, a short text explains the setting and its current
option.

| Category | Settings |
|---|---|
| **Video** | **Quality** (Low, Medium, High, Ultra) · Resolution · N64 Resolution · Aspect Ratio · Picture Size · Letterbox Zoom · each console's **Bezel** · Crop Overscan · Frame Rate Limit · V-Sync · FPS Counter |
| **Graphics** | Upscaling Filter · **GPU Upscaler** (AI, Pixel Art HD, Smooth HD, HD Sharpen) · Texture Filtering · Scanlines · NTSC Filter · the N64's anti-aliasing, dither filter, 2D, textures and effects · **Game Boy Colours** · Screen Colours · LCD Ghosting · Hi-Res Blending · Sprite Flicker · Transparency |
| **Performance** | CPU Clock · SuperFX Clock · the N64's sync, CPU cores, picture timing, frame pacing and CPU timing · Emulation Speed · Fast Forward · Frame Skip · Performance Graphs |
| **Emulation** | Region · the N64's CPU, RSP and Controller Pak · NES Palette · Turbo Buttons · Boot Logo (BIOS) · Sunlight · Tilt Sensor · Opposing Directions · Randomize Memory and more |
| **System** | Language · Volume · Audio Interpolation · Sound Filter · Audio Buffer · Multitap · Resume Where You Left Off |

## Files on the PS4

Everything lives in `/data/EmuSuNi` (reachable by FTP with GoldHEN):

| Path | Contents |
|---|---|
| `games/`, `nes/` | The Super Nintendo's and the NES's default game folders |
| `covers/` | Box art (subfolders `nes`, `n64`, `gb`, `gbc`, `gba`) |
| `saves/`, `states/` | Battery saves (and the cartridges' clocks, `.rtc`) and save states |
| `cheats/` | Cheats: `<game file name>.cht` (RetroArch format) |
| `system/` | Optional BIOS files: `disksys.rom` (Famicom Disk System), `gb_bios.bin`, `gbc_bios.bin`, `sgb_bios.bin`, `gba_bios.bin` |
| `bezels/` | Your bezels: a 1920×1080 PNG, see-through where the game goes, in `bezels/<console>/` |
| `themes/` | Wallpapers (PNG, 1920×1080) and sounds that replace the built-in ones |
| `settings.ini`, `controls.ini`, `xmb.ini`, `favorites.ini` | Your settings |
| `emusuni.log` | The log, for bug reports |

## Building from source

The build runs on **Ubuntu 24.04** (WSL2 works) with clang 21, PacBrew's OpenOrbis toolchain and
DolphinPS4's PS4 runtime (libc++ 21 and Mesa's RADV Vulkan driver).

```sh
# once: packages, toolchain, libc++, Mesa, the cores and Dear ImGui (as root, ~45 minutes)
sudo -i bash /path/to/EmuSuNi/scripts/setup.sh

# then
EMUSUNI_VERSION=00.19 bash scripts/build.sh   # builds ~/emusuni-build/emusuni_eboot/eboot.bin
bash scripts/package.sh 00.19                 # makes the .pkg in ~/emusuni-build/out
```

`scripts/make-assets.py` regenerates the icon, the start-up pictures, the logo and the 8-bit
sounds (Python + Pillow); `scripts/make-bezels.py` the bezels; `scripts/make-lang.py` the language
files.

<details>
<summary>Under the hood</summary>

- **Display**: Vulkan through Mesa's RADV driver running on the PS4's GNM interface (from
  DolphinPS4), three 1920×1080 VideoOut buffers. The menus are drawn with Dear ImGui.
- **Cores**: every emulator is a libretro core linked into the app, its API renamed
  (`snes9x_retro_*`, `mgba_retro_*`...), so one process plays all six consoles.
- **Nintendo 64**: ParaLLEl-RDP draws on the app's own Vulkan device; its command ring is lock-free
  and the VI scanout can run on its thread; the emulation and the RDP threads have cores of their
  own.
- **GPU filters**: xBRZ, Anime4K and AMD FSR/RCAS run as Vulkan fragment-shader passes before the
  menu is drawn; their pipelines are compiled in the background and cached.
- **Memory**: the PS4's system C library serves allocations from a ~12 MiB pool shared with the
  kernel's own objects. All of the app's allocations — C `malloc` and C++ `new` — go to a
  dlmalloc heap in system flexible memory instead (`port/`).
- **Covers**: libretro's thumbnail server over plain HTTP (the PS4's TLS fails on GitHub's
  servers), with GitHub as a fallback; pictures are shrunk to the size the menu shows.

</details>

## Credits

- **[Snes9x](https://github.com/snes9xgit/snes9x)** (Super Nintendo),
  **[FCEUmm](https://github.com/libretro/libretro-fceumm)** (NES),
  **[Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx)** with ParaLLEl-RDP and
  ParaLLEl-RSP (Nintendo 64) and **[mGBA](https://mgba.io)** (Game Boy, Color and Advance) — the
  emulators.
- **[Anime4K](https://github.com/bloc97/Anime4K)** and **AMD FidelityFX Super Resolution** — the
  AI-enhanced HD picture and the smooth upscaling.
- **[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)** — the launcher's design, the PS4
  runtime, the Mesa port and the build setup that EmuSuNi grew from.
- **[Dear ImGui](https://github.com/ocornut/imgui)**, **[Mesa](https://mesa3d.org)**,
  **[OpenOrbis](https://github.com/OpenOrbis) / [PacBrew](https://github.com/PacBrew)**,
  **[libretro-thumbnails](https://github.com/libretro-thumbnails)**, xBRZ, HQx, blargg's NTSC
  filter, and the fonts' authors. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

EmuSuNi by [@arturofuentes1988](https://github.com/arturofuentes1988).

## License and legal

EmuSuNi's own code is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)). Snes9x is
under its own license, which allows free, **non-commercial** use only; the EmuSuNi packages are
free builds. Other components keep their licenses ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

**No games are included.** Use games you own. EmuSuNi is not affiliated with or endorsed by
Nintendo or Sony. *Super Nintendo*, *Super Famicom*, *SNES*, *NES*, *Nintendo 64*, *Game Boy*,
*Game Boy Color* and *Game Boy Advance* are trademarks of Nintendo; *PlayStation* and *PS4* are
trademarks of Sony Interactive Entertainment.
