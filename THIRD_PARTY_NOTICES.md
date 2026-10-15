# Third-party software

EmuSuNi is built from these projects. Their licenses apply to their parts.

| Component | Used for | License |
|---|---|---|
| [Snes9x](https://github.com/snes9xgit/snes9x) 1.63 (libretro build) | The emulation, linked into the app | [Snes9x License](https://github.com/snes9xgit/snes9x/blob/master/LICENSE): free for non-commercial use |
| xBRZ by Zenju (from Snes9x's `filter/`) | Resolution: the xBRZ filter | GPL-3.0 |
| HQx (Snes9x's `filter/hq2x.cpp`, from FFmpeg) | Resolution: the HQx filter | LGPL-2.1-or-later |
| snes_ntsc by Shay Green (blargg) | NTSC Filter | LGPL-2.1-or-later |
| [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) (based on the [Dolphin Emulator](https://dolphin-emu.org)) | The launcher's design, the PS4 runtime (`port/`), the build setup (`toolchain/`), the display code, language files, the Mesa patch | GPL-2.0-or-later |
| [Mesa 3D](https://mesa3d.org) (RADV) with DolphinPS4's patch | Vulkan on the PS4's GPU | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.2b | Drawing the menus | MIT |
| [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (PacBrew build) | Compiler setup, system libraries, PKG tools | GPL-3.0 (tools); the libraries have their own |
| dlmalloc by Doug Lea | The app's memory heap | CC0 / public domain |
| libpng, zlib, minizip | PNG pictures, ZIP games | libpng license, zlib license |
| curl, Mbed TLS | Downloading covers | curl license, Apache-2.0 |
| Nunito, Noto Sans JP / KR / SC, Questrial, Michroma | Fonts | SIL Open Font License 1.1 |
| Mozilla CA certificate bundle (`cacert.pem`) | HTTPS | MPL-2.0 |
| [libretro-thumbnails](https://github.com/libretro-thumbnails) | Box art and title screens, downloaded by the app at run time (not included) | See the project |

EmuSuNi's own code is GPL-2.0-or-later (see [LICENSE](LICENSE)). The Snes9x license is not
GPL-compatible and doesn't allow commercial use: the EmuSuNi packages are free, non-commercial
builds.
