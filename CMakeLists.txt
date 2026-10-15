# EmuSuNi: a Super Nintendo emulator for jailbroken PS4 consoles (Snes9x core), with the launcher,
# in-game menu and settings of Dolphin for PS4 (github.com/iHaiDeeZ/DolphinPS4).
#
# Configured by scripts/build.sh with toolchain/ps4-love-modern.cmake (clang 21 + libc++ 21 on the
# PacBrew OpenOrbis C runtime). Mesa's RADV (scripts/build-mesa.sh of DolphinPS4) draws everything.

cmake_minimum_required(VERSION 3.20)
project(emusuni C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(toolchain/ps4-love-style.cmake)

set(SNES9X_DIR $ENV{HOME}/src/snes9x CACHE PATH "Snes9x source tree")
set(IMGUI_DIR $ENV{HOME}/src/imgui CACHE PATH "Dear ImGui source tree (1.92.2b)")
set(MESA_SRC_DIR $ENV{HOME}/dolphinps4-build/src/mesa CACHE PATH "Mesa source (Vulkan headers)")
set(EMUSUNI_VERSION "00.10" CACHE STRING "Version shown in Settings -> About (XX.YY)")

# --- Snes9x (the libretro build of the core: one well-defined API) --------------------------------
set(SNES9X_SOURCES
  apu/apu.cpp apu/bapu/dsp/sdsp.cpp apu/bapu/smp/smp.cpp apu/bapu/smp/smp_state.cpp
  bsx.cpp c4.cpp c4emu.cpp cheats.cpp cheats2.cpp clip.cpp conffile.cpp controls.cpp cpu.cpp
  cpuexec.cpp cpuops.cpp crosshairs.cpp dma.cpp dsp.cpp dsp1.cpp dsp2.cpp dsp3.cpp dsp4.cpp
  fxinst.cpp fxemu.cpp gfx.cpp globals.cpp memmap.cpp obc1.cpp msu1.cpp ppu.cpp stream.cpp sa1.cpp
  sa1cpu.cpp screenshot.cpp sdd1.cpp sdd1emu.cpp seta.cpp seta010.cpp seta011.cpp seta018.cpp
  snapshot.cpp snes9x.cpp spc7110.cpp srtc.cpp tile.cpp tileimpl-n1x1.cpp tileimpl-n2x1.cpp
  tileimpl-h2x1.cpp sha256.cpp bml.cpp movie.cpp fscompat.cpp libretro/libretro.cpp
  filter/snes_ntsc.c filter/xbrz.cpp filter/hq2x.cpp)
list(TRANSFORM SNES9X_SOURCES PREPEND ${SNES9X_DIR}/)
add_library(snes9x STATIC ${SNES9X_SOURCES})
target_include_directories(snes9x PUBLIC
  ${SNES9X_DIR}/libretro ${SNES9X_DIR}/libretro/libretro-common/include ${SNES9X_DIR}
  ${SNES9X_DIR}/apu ${SNES9X_DIR}/apu/bapu)
target_compile_definitions(snes9x PUBLIC RIGHTSHIFT_IS_SAR __LIBRETRO__ ALLOW_CPU_OVERCLOCK
  HAVE_STDINT_H HAVE_STRINGS_H)
target_compile_options(snes9x PRIVATE -O3 -fno-strict-aliasing -w
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# --- Dear ImGui with its Vulkan renderer --------------------------------------------------------
# src/third_party/imgui_impl_vulkan.cpp: ImGui's Vulkan renderer with a persistent upload buffer
# (see the comment there).
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp src/third_party/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party ${IMGUI_DIR}
  ${MESA_SRC_DIR}/include)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_IMPL_VULKAN_NO_PROTOTYPES
  VK_NO_PROTOTYPES)
target_compile_options(imgui PRIVATE -O2 -w)

# --- The app -----------------------------------------------------------------------------------
add_executable(emusuni
  src/main.cpp src/Display.cpp src/VulkanFunctions.cpp src/Menu.cpp src/Settings.cpp
  src/Controls.cpp src/Emulator.cpp src/IngameMenu.cpp src/GameAudio.cpp src/Library.cpp
  src/Cheats.cpp src/Saves.cpp src/Image.cpp src/Log.cpp src/PS4Lang.cpp src/PS4Glyphs.cpp
  src/Net.cpp src/CoverDownload.cpp src/Upscaler.cpp src/Tour.cpp
  port/ps4_jit_stubs.c ${PS4_RUNTIME_SOURCES})
target_include_directories(emusuni PRIVATE src ${OPENORBIS}/usr/include ${OPENORBIS}/usr/include/libpng16)
target_compile_definitions(emusuni PRIVATE EMUSUNI_VERSION="${EMUSUNI_VERSION}")
target_compile_options(emusuni PRIVATE -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(emusuni PRIVATE snes9x imgui
  ${OPENORBIS}/usr/lib/libminizip.a ${OPENORBIS}/usr/lib/libpng16.a ${OPENORBIS}/usr/lib/libcurl.a
  ${OPENORBIS}/usr/lib/libmbedtls.a ${OPENORBIS}/usr/lib/libmbedx509.a
  ${OPENORBIS}/usr/lib/libmbedcrypto.a ${OPENORBIS}/usr/lib/libz.a
  SceSysmodule SceSystemService SceUserService SceVideoOut SceAudioOut ScePad SceNet)
ps4_link_mesa(emusuni)
ps4_add_eboot(emusuni)
