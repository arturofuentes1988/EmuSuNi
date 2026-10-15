# Changelog

All notable changes to EmuSuNi. Versions follow the PS4 package version (`XX.YY`).

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
