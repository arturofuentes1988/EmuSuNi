# Third-party software

EmuSuNi is built from these projects. Their licenses apply to their parts.

| Component | Used for | License |
|---|---|---|
| [Snes9x](https://github.com/snes9xgit/snes9x) 1.63 (libretro build) | The emulation, linked into the app | [Snes9x License](https://github.com/snes9xgit/snes9x/blob/master/LICENSE): free for non-commercial use |
| xBRZ by Zenju (from Snes9x's `filter/`) | Resolution: the xBRZ filter | GPL-3.0 |
| HQx (Snes9x's `filter/hq2x.cpp`, from FFmpeg) | Resolution: the HQx filter | LGPL-2.1-or-later |
| xBRZ freescale shaders (RetroArch's [slang-shaders](https://github.com/libretro/slang-shaders), `src/shaders/xbrz*.frag`): Hyllian's xBR code and texel mapping, with xBRZ by Zenju | GPU Upscaler: Pixel Art HD and AI | MIT (Hyllian) and GPL-3.0 (xBRZ) |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (`src/third_party/ffx`) | GPU Upscaler: Smooth HD (EASU) and the sharpening (RCAS) | MIT |
| [Anime4K](https://github.com/bloc97/Anime4K) by bloc97: the Restore CNN (M) network (`src/third_party/anime4k`, made into `src/shaders/anime4k` by `scripts/make-anime4k.py`) | GPU Upscaler: AI | MIT |
| snes_ntsc by Shay Green (blargg) | NTSC Filter | LGPL-2.1-or-later |
| [FCEUmm](https://github.com/libretro/libretro-fceumm) (libretro-fceumm, with nes_ntsc by blargg and libretro-common) | The NES emulation, linked into the app | GPL-2.0-or-later (libretro-common: MIT) |
| [mGBA](https://mgba.io) by Vicki Pfau and contributors ([libretro's fork](https://github.com/libretro/mgba), its libretro core; with inih by Ben Hoyt) | The Game Boy, Game Boy Color and Game Boy Advance emulation, linked into the app | [MPL-2.0](https://github.com/libretro/mgba/blob/master/LICENSE) (inih: BSD-3-Clause) |
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
