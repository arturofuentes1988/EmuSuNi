#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Azahar (azahar-emu/azahar 2126.2, ~/src/azahar: the unified source tarball with every
# dependency in it, committed once by setup.sh so the patches below start from pristine files;
# GPL-2.0-or-later: the Nintendo 3DS emulator) for the PS4 into one object that scripts/build.sh
# links into EmuSuNi: the 3DS. Its own CMake (ENABLE_LIBRETRO=ON) makes the libretro core with the
# PS4 toolchain (toolchain/ps4-love-modern.cmake): dynarmic (the ARM11's x86-64 recompiler, one per
# emulated core), the PICA200 vertex-shader JIT, the Vulkan renderer (GLSL compiled at run time by
# glslang, SPIR-V written by sirit), the software renderer as a fallback (never a display path on the
# PS4: a second a frame), HLE audio (teakra's DSP LLE built in), decrypted .3ds/.cci/.cxi/.3dsx read
# by the core itself (stdio). Its libretro API becomes azahar_retro_* and everything else in it
# private, as for FCEUmm (CMakeLists.txt). The PS4 changes to its sources (applied each time to the
# files as git has them, checked; all under EMUSUNI_PORT, which the CMake option of that name
# defines):
#   - The JITs' code memory: one 64 MiB mapping (common/emusuni_jit.cpp), readable, writable and
#     executable, that EmuSuNi makes when the first 3DS game starts (port/ps4_n64.c:
#     emusuni_map_executable: system flexible memory on a range reserved above the app's image and
#     heap, as DolphinPS4's JIT buffers, checked by running code there) and that goes back to the
#     system when the core shuts down with nothing of it in use (retro_deinit). Never an array in the
#     core's data: the PS4 charges an executable's whole .bss to the games' flexible memory at load,
#     3DS game or not (this pool was 64 of the core's 64.4 MiB of it). Shared out first-fit to
#     dynarmic's blocks of code (24 MiB per emulated core in Old 3DS mode: 2 cores; 12 MiB each on a
#     New 3DS: 4 cores; upstream reserved 128 MiB each) and to the PICA shader JIT's 256 KiB per
#     shader (16 MiB left for them). If the PS4 refuses the mapping, the game doesn't start: a message
#     says so and the log why (the interpreters, DynCom and the shader interpreter, would be far too
#     slow there). If it is full: dynarmic's block of code is tried again at half the
#     size, down to 6 MiB (its cache flushes more often: slower, not dead), a shader that gets no
#     block runs in the shader interpreter, and the JITs of page tables no process has any more (the
#     one made before any process existed, a process that ended, the processes a save state's load
#     replaced) are given back (upstream kept them all and threw Xbyak's ERR_CANT_ALLOC out of
#     retro_run or the process switch, which ended the whole app); as a last try, a mapping of its
#     own made the same way (never reached in the tests). A game whose first JIT can't be made fails
#     to load with a message, and an exception out of the emulation ends the game
#     (RETRO_ENVIRONMENT_SHUTDOWN), not the app.
#     dynarmic's spin lock (the exclusive monitor's, taken by every LDREX/STREX) was x86 code emitted
#     at static initialisation through Xbyak's own mmap and mprotect(RWX): before main, for every
#     EmuSuNi user, whichever console they play; a console that refuses RWX would have ended the app
#     at launch. Plain C++ with the same protocol instead (common/spin_lock_x64.cpp).
#     The mapping can be anywhere: both JITs reach the core's code and data only through far calls
#     (dynarmic's CallFunction, the shader JIT's CallFarFunction: mov reg, imm64; call reg when out of
#     a 32-bit offset's reach) and absolute addresses in registers; their rip-relative operands and
#     direct jumps stay in their own buffers (dynarmic's constant pool and blocks, the shader JIT's
#     constants and labels). On this PC the mapping lands far from the code too: the PC tests run so.
#   - Fast memory for dynarmic, on by default since 00.68 (the file /data/EmuSuNi/3ds-fastmem-off
#     turns it off; common/emusuni_fastmem.cpp; the PC tests: EMUSUNI_3DS_FASTMEM=0 off): a page
#     table's 4 GiB reserved,
#     the 16 KiB groups of guest pages that lie in order in the 3DS's memory blocks mapped there a
#     second time (direct memory aliasing), so a recompiled load or store is one instruction; anything
#     else faults once and is recompiled with the page table (dynarmic's POSIX fault handler, on the PS4
#     too, installed only then and taken out at the game's end). A self-test before the first use, the
#     page table whenever anything is refused, the marker files 3ds-fastmem-trying and -crashed for a
#     start that dies while trying it; the loader's FCRAM at the guest addresses' 16 KiB phase. Turned
#     off: the page table and no fault handler, as before. Nothing is mprotect-ed (checked at the
#     end: the only objects that still name mprotect are Xbyak's inline code in the generators given
#     EmuSuNi's allocator, which never runs it).
#   - Vulkan: the renderer adopts the frontend's device. EmuSuNi makes its only VkDevice at start
#     (src/Display.cpp) and never calls the context negotiation's create_device: the core takes the
#     instance, GPU, device and queue of GET_HW_RENDER_INTERFACE (upstream does too) but believed the
#     device had every extension and feature the GPU offers; now it has only what
#     EMUSUNI_VK_DEVICE_EXTENSIONS and EMUSUNI_VK_DEVICE_FEATURES say (comma-separated; the
#     VkPhysicalDeviceFeatures names plus the extension features' names: timelineSemaphore,
#     extendedDynamicState, customBorderColors, customBorderColorWithoutFormat, indexTypeUint8,
#     fragmentShaderPixelInterlock, pipelineCreationCacheControl, fragmentShaderBarycentric; none
#     when unset: the core copes, as it does on GPUs without them). Every vkDeviceWaitIdle of the
#     core under the frontend's lock_queue / unlock_queue (its submits already were). The frames in a
#     ring of 3 images of the core's own, one step per frame presented, not the frontend's sync
#     index; their render pass waits for the frontend's sampling of earlier images and makes its
#     writes visible to it.
#   - Threads: every thread Azahar names ("VulkanWorker", "Pipeline workers", "Shader workers",
#     "citra:Log", "SwRenderer workers", "PlayTimeReport"...) first calls emusuni_3ds_thread(name),
#     which the app defines (port/): its core, priority and name; the ones upstream left unnamed get
#     one ("VulkanFence": the fence wait thread; "HleAsync": the HLE's asynchronous file work;
#     "Detached"; "Teakra": the DSP LLE's own thread; "DlpServer" and "DlpClient": Download Play;
#     "AmScan": the title scans; "CamCapture"; "HttpRequest"). The AM scans' std::async takes
#     std::launch::async: the default policy may run the task on the thread that waits for it, the
#     emulation thread, which the hook must never re-place. The core's own priority calls do nothing
#     (the PS4 runs threads by fixed priorities, which the hook sets). 3 pipeline and 3 shader
#     compile workers (upstream: half the CPUs each).
#   - No networking or SSL: LibreSSL isn't built, cpp-httplib without it (the 3DS's HTTPS requests
#     fail at once; plain HTTP, the NUS downloads and the rooms compile against the PS4's sockets
#     but nothing in the app uses them). Every random byte from common/emusuni_random.cpp (getentropy,
#     else /dev/urandom, else a generator seeded from the clock; said once in the log): the ssl:C
#     service's and the AM ticket keys' (LibreSSL's RAND_bytes), Crypto++'s OS generator
#     (arc4random_buf on a FreeBSD target, which the PS4's libc hasn't) and every std::random_device
#     (libc++'s opens /dev/urandom, which the PS4 may not have: the core timing's jitter, the rooms,
#     cpp-httplib's multipart boundary; checked at the end: none left in the object).
#   - No built-in keys (ENABLE_BUILTIN_KEYBLOB off): upstream compiles Nintendo's AES keys into the
#     binary (encrypted in its source) for players without a key dump. As the other consoles' BIOS
#     files, they are the player's: sysdata/aes_keys.txt in the core's directory (GodMode9 dumps it
#     with the games). Decrypted games run without them; the few that encrypt through the PS or APT
#     services (and amiibo) need them, and the core logs which key it misses.
#   - The 3DS's memory off the app's heap: FCRAM (256 MiB, always the New 3DS size), VRAM and the
#     New 3DS extra RAM in blocks of their own (common/emusuni_mem.cpp): direct memory on the PS4
#     (sceKernelAllocateDirectMemory, sceKernelMapDirectMemory), an mmap elsewhere; freed with the
#     emulator. DynCom's 125 MiB translation cache the same way, and only when the interpreter runs
#     (upstream's array stayed in the app's memory for nothing). A save state is serialised into one
#     vector with room for the emulated RAM reserved and zstd-compressed from it by frames, and read
#     back in place (upstream's string stream, its copy and the one-shot compression peaked at 5 x
#     the RAM on the heap: 1.2 GiB measured on a New 3DS; the load copied the state twice more).
#   - The Vulkan pipeline cache makes its shader cache object on first use when no disk resources
#     were loaded (the disk cache option off, or the renderer made again by a save state's load):
#     upstream read through a null pointer and crashed at the first draw.
#   - Shader stalls: a pipeline that a draw needs while it still waits in the workers' queue is built
#     at once; the driver's pipeline cache is saved every 60 s while it grows, not only when a game
#     ends cleanly; asynchronous shader compilation by default since 00.68 (a draw whose pipeline
#     isn't ready is skipped until it is; /data/EmuSuNi/3ds-sync-shaders waits instead). A frame of
#     more than 100 ms says in the log where its time went (the guest's code, the JIT compiling it,
#     the HLE calls, the GPU's commands, the audio; pipelines, shaders, textures, file reads, the wait
#     for the GPU side), and every 600 frames a line sums it up. dynarmic without its IR verification
#     pass (asserts only); the audio's resampling over a contiguous copy (the same output).
#   - The JIT's compiles (00.69): dynarmic writes the same x86 code in about 40% less time and with a
#     quarter of the heap allocations (the VFP and ASIMD decoders searched only where their patterns can
#     match; the register allocator without its scans, copies and temporary vectors; the IR pool's slab
#     kept for the next block; the blocks' ranges in 4 KiB page buckets; Xbyak's label tables' nodes from
#     free lists of the thread); the JIT warm-up (citra_libretro/emusuni_jitwarm.h: the blocks of the
#     game's code segment and its accesses outside the fast memory kept per game in
#     shaders/emusuni-jit/<program id>.bin, compiled ahead from the next start's first frame;
#     /data/EmuSuNi/3ds-jit-warmup-off turns it off). The FS service's file work on two threads made
#     once (RunAsync made one per request; System::Shutdown waits for them first); every texture decoded
#     into ordinary memory, then copied once into the staging buffer (write-combined on the PS4).
#   - The draws (00.70): a draw whose texture and utility descriptor writes are the last draw's, in the same
#     submission, binds that descriptor set again (upstream wrote a new set for every draw); a pipeline state
#     equal byte for byte to the last draw's gives its pipeline without the two hashes and lookups; two loops of
#     every draw without masked moves (vmaskmovps, microcoded on the Jaguar). A save state's load applies 3DS CPU
#     Clock again (the timers came back at 100%). The stall log counts the draws, their vertices and time, the
#     command lists' size and the game's own pictures.
#   - C++ exceptions (00.85): on the PS4 one never reaches its catch and the app ended at once, with no crash report.
#     Every throw is logged first, straight into the log (common/emusuni_throw.cpp, linked over __cxa_throw with
#     ld.lld -r --wrap): its type, message and place; on the PS4 one thrown under retro_run, the game's load, the
#     renderer's set-up or a save state goes back to that call (setjmp/longjmp): the game ends with a message and
#     EmuSuNi stays. The throws Azahar meant to catch are made without one: a vertex shader the decompiler can't
#     take (shaded on the CPU), the shader JIT's and dynarmic's memory asked first, the stream buffers, the driver's
#     pipeline cache and a shader module by their results. A frame or the game's load that runs for seconds says
#     so while it goes on and what it is in ("EmuSuNi: stall: ...", common/emusuni_stall.cpp). What the 3DS can't
#     go on from ends the game with a message (its code crashed, ERR:F, it closed itself, the system stopped).
#     Azahar's GPU timing emulation per game (citra_emusuni_gpu_timings: the app's, Luigi's Mansion 2).
#   - Graphics (00.89): upstream's fixes after 2126.2 for the SPIR-V fragment shaders' shadows (#2590) and Vulkan's
#     procedural texture tables (#2627); the file /data/EmuSuNi/3ds-glsl-shaders makes the fragment shaders through
#     GLSL, as on OpenGL (a cache of their own), to test a game's broken pictures.
#     Textures (00.89): upstream's faster texture codec (#2564: ETC1 5x, the 4-bit and converted formats 6-15x on
#     this PC; scripts/3ds-azahar/, the same bytes out).
#   - A game's first play (00.90): a draw that waits for a new pipeline puts it at the workers' front and the Vulkan
#     worker builds it if no worker has (no more seconds behind the queue); 4 compile workers of each kind; core 0's
#     code cache 40 MiB (8 for the others: a big game no longer throws its compiled code away); fast memory's crash
#     marker per build of the core.
#   - Pipelines by parts (00.94, VK_EXT_graphics_pipeline_library, which src/Display.cpp asks the device for): each
#     shader compiled once per render pass as a library, and a pipeline linked from its parts at once (RADV: under
#     0.1 ms) instead of compiled whole (its vertex shader again each time); the whole, optimised one made later by one
#     more worker ("Pipeline optimizer") and bound from then on. Whole pipelines as before without the extension, with
#     the file /data/EmuSuNi/3ds-gpl-off, after a part the driver refuses, or after a start that ended while making
#     them (/data/EmuSuNi/3ds-gpl-crashed). The log says which at each game's start.
#   - Vertices (00.95): a loader with a load of 2 or more bytes at an odd address (Super Smash Bros.' fighters: 3-byte
#     positions in strides of 3, 5, 7 or 9 bytes) is copied with each attribute at an offset its loads' alignment
#     divides and a stride a multiple of 4, the same values (the PS4's GPU reads such a load wrong: the fighters were
#     drawn spiky; the file /data/EmuSuNi/3ds-vertex-as-is keeps the game's layout). A draw whose vertices an earlier
#     draw of the same command list copied binds that copy again (Driver Renegade 3D, DKC Returns: half their vertex
#     data; the file /data/EmuSuNi/3ds-vertex-reuse-off copies them for every draw). Every 600 frames the log says
#     where the draws' time went. The pipeline optimizer below the compile workers (port/ps4_n64.c).
#   - Vertex compaction (00.96): an indexed draw whose indices use at most half of their range's vertices copies only
#     those, renumbered (Driver Renegade 3D copied ~20 MB of vertex data a frame on the PS4 for ~170 indices a draw;
#     the file /data/EmuSuNi/3ds-vertex-compact-off copies the whole range again).
#   - Indices from vertex 0 (00.98): an indexed draw that starts above index 0 gets its indices minus its first one and
#     no negative vertex offset (Super Smash Bros.' fighters, drawn high in a shared vertex buffer with indices up to
#     ~5500, broken into shards and spikes on the PS4 only; the file /data/EmuSuNi/3ds-index-as-is: as before).
#   - Vertex shaders' jumps (01.00): a subroutine whose jumps all go forward is written without the decompiler's jump
#     loop (Super Smash Bros.' fighters: their bone lookups were the only ones inside it, broken on the PS4 only; the
#     file /data/EmuSuNi/3ds-vs-jump-loop keeps the loop).
#   - CPU JIT warm-up (01.01): the list's first 60,000 blocks compiled ahead only, half of core 0's 40 MiB code cache
#     left to the game (Smash filled it and flushed it in a fight), the list kept in the order of last use.
#   - GPU sync diagnosis (01.02): the files /data/EmuSuNi/3ds-gpu-sync-draw, 3ds-gpu-sync-frame, 3ds-uniforms-fresh
#     and 3ds-vertex-reuse-off, read every 2 s (Super Smash Bros.' fighters right when paused, in shards in a fight,
#     on the PS4 only). Never for playing.
#   - Vertex loads at offsets their size divides (01.03): every non-float attribute load moved so, in a stride 4
#     divides, not only the odd ones (Super Smash Bros.' fighters: 3 shorts in strides of 18; the file
#     /data/EmuSuNi/3ds-vertex-align-odd: as before); the diagnosis file 3ds-vertex-float (non-float attributes as
#     floats); 3ds-vs-bone0 now makes the shader caches again (it never ran while they held the game's shaders).
#   - Directories made without the trailing '/', a passing error tried again, a lasting EINVAL by a rename, and a
#     failure said in full (01.04): a PS4 on system 13.09 stopped at its first 3DS game, mkdir of
#     .../nand/data/<id>/sysdata/ refused with EINVAL (v00.23's reports H4F3TQ and FA7NXC).
#   - A save state of the other 3DS (New 3DS Mode changed since) loaded as that 3DS, a shadow texture on a surface
#     that isn't RGBA8 drawn without it, and a game's memory mode that doesn't fit this 3DS made its default (01.04):
#     they asserted (v00.23's UHA4KY, ZWE4NA, BQ5RWS and HBDCG5).
#   - 3DS online (01.04): the local wireless play (UDS) through an Azahar room on a server (scripts/build-3ds-room.sh)
#     when the app's 3DS Online gives one (citra_emusuni_room, citra_emusuni_room_nickname; off otherwise):
#     citra_libretro/emusuni_room.h; ENet's socket flags made the PS4 kernel's; no address in the log.
#   - A texture blit's source surface taken after the surface it creates, a blit with a surface without an image
#     left out, 64 MiB reserved for the rest of a save state and its growth 32 MiB at a time, and each step of the
#     system's end said in the log (01.05): v00.23's T73ZVE and UWP3HS (A Link Between Worlds, in the driver),
#     5VZWA7 (Super Smash Bros., out of heap saving a state) and 7D3S7C (Castlevania's end stopped for 60 s).
#   - Surfaces queued for deletion kept 3 frames for recycling, moved vertex layouts rebuilt with byte shuffles, a
#     test switch for dynarmic's unsafe floating point, and the frame handed over without waiting for the Vulkan
#     worker when the app waits for each picture itself (01.06, EMUSUNI_3DS_ASYNC_HANDOFF; the object's three other
#     globals, emusuni_3ds_image_serial, emusuni_3ds_wait_image and emusuni_3ds_image_worker_ms): the owner's slow
#     games at 4x (Luigi's Mansion: Dark Moon, Castlevania: Mirror of Fate, Donkey Kong Country Returns 3D).
#   - The GPU's commands and the audio's mixing on threads of their own (01.10, common/emusuni_async.h; by
#     default since 01.11: the word "off" in /data/EmuSuNi/3ds-async-pica or 3ds-async-dsp leaves that work
#     on the emulation thread, as the GPU's stays there while its thread doesn't pay).
#   - 01.11's other speed switches, on by default (the word "off" in the file turns one off): the CPU's reads
#     of the GPU's memory wait only where the GPU wrote (3ds-async-cpu-reads), the game's idle loops are
#     skipped (3ds-cpu-idle-loops), the 3DS CPU at 50 % for Castlevania: Mirror of Fate (3ds-cpu-clock-games;
#     since 01.12 only with its file: at 50 % the game walked in jumps on the PS4);
#     a line at each game's start says them all.
#   - 01.12's, on by default too: the CPU's writes to the GPU's memory queue the cache's invalidation after the
#     GPU commands' thread's commands instead of waiting (3ds-async-cpu-writes), and the JIT's warm-up compiles
#     the list's first 120,000 blocks ahead, not 60,000 (3ds-jit-warmup-wide).
#   - 01.13: the 3DS GPU's time per game, only with the file /data/EmuSuNi/3ds-gpu-timing-games (the word "off"
#     in it: off): Azahar's GPU timing emulation at 100 % of its estimate for Castlevania: Mirror of Fate, at 200
#     (the 3DS's own by it) for Monster Hunter 4 Ultimate and Luigi's Mansion: Dark Moon. Without it they drew 60
#     pictures a second or more where a 3DS draws 30: 1.6 to 3 times the work a frame. A number in the file
#     3ds-gpu-timings sets every other game's. The speed switches' line is two lines (the app's log cuts at ~520).
#     svcGetThreadId on the current thread answered at once (3ds-hle-fast, on: Monster Hunter's ~550 a frame),
#     Castlevania's own clock at 75 % (3ds-cpu-clock-games, still only with its file), and a new PGO profile made
#     with 01.13's code (Castlevania, Monster Hunter, Dark Moon, DKC and 3D Land; below).
#   - 01.14: 3ds-gpu-timing-games on by default (the word "off" in its file: off); the owner tried 01.13 without
#     its file.
#   - 01.15: a game's file that says it's decrypted but isn't (its ExeFS has no .code, or its .code doesn't
#     decompress) fails as encrypted, with a log line saying why, not "Failed to load ROM" (below). Castlevania:
#     Mirror of Fate's GPU commands on the emulation thread (3ds-pica-inline-games; the word "off" in its file:
#     off), its EUR and JPN games at 100 % of the GPU estimate as the USA one.
#   - 01.16: a command list's end at its time (3ds-pica-on-time; the word "off" in its file: off): each GPU
#     command carries the emulated time it was sent, its interrupt comes at that time plus its estimate, not later
#     by the commands' thread's lag (Castlevania's intro); 3ds-pica-inline-games only with its file. Castlevania's
#     core 0 with 56 MiB of code cache, the JIT's memory 80 MiB (3ds-jit-big-games; off: as any game). The picture
#     black at a game's start watched (3ds-black-watch; off: not watched), emusuni_3ds_black_ms for the app.
#   - 01.17: the JIT warm-up again after a reset and after a save state's load (until 01.16 only as the game
#     loaded: the black picture's reset threw it away); Castlevania's may fill all but a quarter of core 0's cache.
#     Castlevania's draws wait for their pipelines (3ds-sync-shaders-games) and its GPU commands are waited for at
#     their time, exactly (3ds-pica-strict-games: it draws less when its GPU looks slow). 3ds-black-watch reads the
#     first minute. Azahar's texture filter (the app's 3DS Texture Filter): the scale of the textures it redraws
#     capped by the number in 3ds-texture-filter-scale.
#   - 01.18: Azahar's texture filter never, whatever the app sends (01.17's ScaleForce out, the owner's decision: a
#     GPU hang in Luigi's Mansion: Dark Moon and textures missing in Castlevania on the PS4). 3ds-black-watch also
#     says, with its lines, the draws with no buffer to draw into, the command lists and top-screen copies Azahar's
#     right-eye guesser left out and the screen copies done on the CPU (counting only). The right eye drawn, as
#     upstream: Azahar's disabler opened Castlevania and DKC black on the PS4 (the app's 3DS Right Eye Not Drawn,
#     or the word off in 3ds-right-eye: left out). Castlevania's draws wait for their pipelines only while they are
#     made by parts (with whole ones, 3ds-gpl-crashed, a new place froze it 5 to 8 s a frame).
#   - 01.20: a renderer that closes leaves the pipelines still queued for its workers unbuilt (with whole pipelines,
#     3ds-gpl-off, Castlevania's first reset by itself waited for them all and never went on). 3ds-black-watch also
#     says what each screen shows: its two buffers, the one shown, the screen copies into each and the swaps.
#   - 01.21: 3ds-black-watch also says through which surfaces of the rasterizer cache the picture comes: the one each
#     screen shows, the last screen copy's source and destination, the last draws' colour buffer, and how each was
#     written (draws, copies, loads from memory, copies from other surfaces). Counting only.
#   - 01.22: 3ds-black-uniforms: a black picture with the game drawing in 3ds-black-watch's first minute has every
#     draw upload its three uniform blocks fresh until it shows (the PS4's black starts of 01.21 came back so at
#     once and stayed); before a refresh for a new black (at most 4 a renderer), a diagnosis says whether the
#     blocks' bytes where the draws found them were the CPU's, in memory and as the GPU copies them (off: the word
#     off in the file). Upstream's |= for the VS block's pending upload (its 1edf59e).
#   - $HOME unset (the PS4): /data/EmuSuNi instead of a failed assertion (the libretro save
#     directory is where the core's files really go).
#   - Built as a static library, at -O2 with the profile scripts/3ds-core.profdata.xz (PGO, below),
#     without LTO, precompiled headers on, no -Werror, without the stack
#     protector and _FORTIFY_SOURCE (musl has neither), with its own Boost 1.89 (serialization and
#     iostreams compiled), Crypto++, fmt, zstd, glslang, SPIRV-Tools, sirit, VMA, teakra, faad2,
#     SoundTouch, enet, xxHash, lodepng, inih (nothing of the system's), as upstream's libretro
#     builds. The PS4's CMake system is "Generic": glslang's POSIX parts taken anyway (as the PSP
#     port), Boost.Asio without kqueue (no sys/event.h), the submodule check (no .git in the
#     submodules) skipped.
# The object is linked from Azahar's own libraries whole (the libretro glue, Boost.Serialization's
# class registrations live there) and from its dependencies only what they reference: linked whole,
# they carried 4.6 MB of code and 6.4 MB of static memory nothing used (glslang's disassembler
# tables, teakra's test generator, Crypto++'s other ciphers).
# The object is checked at the end: only azahar_retro_* defined globally, no weak references left,
# the C++ operator new/delete it uses all EmuSuNi's own (port/ps4_new.cpp, on the app's heap) and
# none defined in it, no std::random_device, mprotect named only by the objects listed above, and
# every other symbol it needs found in what the app links (OpenOrbis' libc, libkernel,
# libSceLibcInternal, clang's builtins, libc++/libc++abi/libunwind, port/ps4_n64.c: emusuni_3ds_thread
# and the JIT memory's emusuni_map_executable / emusuni_unmap_executable).
# The sources must be the 2126.2 tarball setup.sh commits (its GIT-COMMIT file is checked first).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-3ds-core.sh <out dir> [clean]
#   CTR_CORE_HOST=1 build-3ds-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests: no PS4 checks; the test frontend
#                                                 (scratchpad: 3dsfront.cpp) provides
#                                                 emusuni_map_executable, emusuni_unmap_executable
#                                                 and emusuni_3ds_thread.
#                                                 (env 3DS_CORE_HOST=1 is read too; bash can't name
#                                                 a variable that starts with a digit.)
#   CTR_CORE_SECTIONS=1 build-3ds-core.sh <another out dir>
#                                                 a section for each function and variable (-ffunction-
#                                                 sections -fdata-sections), for a final link with
#                                                 --gc-sections to drop what nothing reaches.
#   CTR_CORE_PGO=off build-3ds-core.sh <out dir>  without the PGO profile;
#   CTR_CORE_HOST=1 CTR_CORE_PGO=generate ...     instrumented, to make a new one (PGO, below).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${AZAHAR_DIR:-$HOME/src/azahar}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/3ds-core.log"
HOST="${CTR_CORE_HOST:-$(printenv 3DS_CORE_HOST 2>/dev/null || echo 0)}"
JOBS="${CTR_CORE_JOBS:-4}"   # at most 4 compilers at once (the PC's load)

# The tree setup.sh commits: the 2126.2 unified tarball, whose GIT-COMMIT file names the commit it
# was made from. Another tree would fail the patches' checks below one at a time; said once here.
AZAHAR_GIT_COMMIT=219d743a1e698101fe1ab8bef43e6563126c3c27
TREE_COMMIT="$(tr -d '[:space:]' < "$SRC/GIT-COMMIT" 2>/dev/null || true)"
if [ "$TREE_COMMIT" != "$AZAHAR_GIT_COMMIT" ]; then
  echo "Azahar: $SRC isn't the 2126.2 unified source (GIT-COMMIT ${TREE_COMMIT:-missing}, wanted $AZAHAR_GIT_COMMIT): run scripts/setup.sh"
  exit 1
fi
if ! git -C "$SRC" rev-parse -q --verify HEAD >/dev/null 2>&1; then
  echo "Azahar: $SRC has no commit to patch from (setup.sh commits the tarball once, git add -A -f)"
  exit 1
fi

python3 -I - "$SRC" "$HERE" <<'PATCH'
import hashlib
import os
import subprocess
import sys
root = sys.argv[1]
here = sys.argv[2]
patched = {}
def git_file(repo, path):
    run = subprocess.run(['git', '-C', repo, 'show', 'HEAD:' + path], capture_output=True)
    if run.returncode != 0:
        # (setup.sh commits the whole tarball, git add -A -f: its own .gitignore would hide files.)
        sys.exit("Azahar: %s isn't in the commit of %s: run scripts/setup.sh again" % (path, repo))
    return run.stdout.decode('utf-8').replace('\r\n', '\n')
def patch(path, old, new, repo=None):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    # (The unified tarball is one repository: the submodules' files are reached by their paths.)
    key = (repo or root, path)
    if key not in patched:
        patched[key] = git_file(key[0], path)
    text = patched[key]
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[key] = text.replace(old, new)
def addfile(path, text):
    # A file of EmuSuNi's own in the tree (git doesn't know it).
    patched[(root, path)] = text
def upstream(path, base_sha256=None):
    # A file of a later Azahar's (scripts/3ds-azahar/<its name>) in place of 2126.2's, which must be the one it was
    # made from (its sha-256; none: a file 2126.2 hasn't). Nothing else may patch it.
    key = (root, path)
    assert key not in patched, path
    if base_sha256 is not None:
        assert hashlib.sha256(git_file(root, path).encode('utf-8')).hexdigest() == base_sha256, path
    with open(os.path.join(here, '3ds-azahar', os.path.basename(path)), encoding='utf-8') as f:
        patched[key] = f.read()

# --- The build: the EMUSUNI_PORT option, a static libretro core, no submodule check, no stack
# protector or fortify, no LibreSSL.
patch('CMakeLists.txt',
      'option(ENABLE_LIBRETRO "Build as a LibRetro core" OFF)\n',
      'option(ENABLE_LIBRETRO "Build as a LibRetro core" OFF)\n'
      '# EmuSuNi (scripts/build-3ds-core.sh): the libretro core as one object of the PS4 app (a static\n'
      '# library), with the changes under EMUSUNI_PORT.\n'
      'option(EMUSUNI_PORT "Build the libretro core for EmuSuNi (PS4)" OFF)\n'
      'if(EMUSUNI_PORT)\n'
      '    add_compile_definitions(EMUSUNI_PORT=1)\n'
      '    # No zstd tracing hooks (weak references nothing defines).\n'
      '    add_compile_definitions(ZSTD_TRACE=0)\n'
      '    if(NOT UNIX)\n'
      '        # The PS4 (CMake\'s "Generic" system, FreeBSD headers without sys/event.h): Boost.Asio\'s\n'
      '        # select reactor, no kqueue.\n'
      '        add_compile_definitions(BOOST_ASIO_DISABLE_KQUEUE)\n'
      '    endif()\n'
      'endif()\n')
patch('CMakeLists.txt',
      'if (EXISTS "${PROJECT_SOURCE_DIR}/.git/objects")\n    # only check submodules when source is obtained via Git\n',
      'if (EXISTS "${PROJECT_SOURCE_DIR}/.git/objects" AND NOT EMUSUNI_PORT)  # EmuSuNi: the unified tarball, one repository\n'
      '    # only check submodules when source is obtained via Git\n')
patch('src/CMakeLists.txt',
      '    add_compile_options(\n        -Wall\n'
      '        # In case a flag isn\'t supported on e.g. a certain architecture, don\'t error.\n'
      '        -Wno-unused-command-line-argument\n'
      '        # Build fortification options\n'
      '        -fstack-protector-strong\n    )\n',
      '    add_compile_options(\n        -Wall\n'
      '        # In case a flag isn\'t supported on e.g. a certain architecture, don\'t error.\n'
      '        -Wno-unused-command-line-argument\n    )\n'
      '    if (NOT EMUSUNI_PORT)  # EmuSuNi: no stack protector on the PS4 (speed; musl has no fortify)\n'
      '        # Build fortification options\n'
      '        add_compile_options(-fstack-protector-strong)\n'
      '    endif()\n')
patch('src/CMakeLists.txt',
      '    if (NOT CMAKE_BUILD_TYPE STREQUAL Debug AND NOT FORTIFY_SOURCE_DEFINED)\n',
      '    if (NOT CMAKE_BUILD_TYPE STREQUAL Debug AND NOT FORTIFY_SOURCE_DEFINED AND NOT EMUSUNI_PORT)\n')
patch('src/citra_libretro/CMakeLists.txt',
      'add_library(citra_libretro SHARED\n        citra_libretro.cpp\n',
      'if(EMUSUNI_PORT)\n'
      '  # EmuSuNi: linked into the app (scripts/build-3ds-core.sh makes one object of it).\n'
      '  set(EMUSUNI_LIBRETRO_KIND STATIC)\n'
      'else()\n'
      '  set(EMUSUNI_LIBRETRO_KIND SHARED)\n'
      'endif()\n'
      'add_library(citra_libretro ${EMUSUNI_LIBRETRO_KIND}\n        citra_libretro.cpp\n')
patch('externals/CMakeLists.txt',
      '# OpenSSL\nif (USE_SYSTEM_OPENSSL)\n',
      '# OpenSSL\n'
      'if (EMUSUNI_PORT)\n'
      '    # EmuSuNi: no LibreSSL (no HTTPS: the app has no network use; the OpenSSL licence question with it).\n'
      '    set(OPENSSL_LIBRARIES "")\n'
      'elseif (USE_SYSTEM_OPENSSL)\n')
patch('externals/CMakeLists.txt',
      'target_compile_options(httplib INTERFACE -DCPPHTTPLIB_OPENSSL_SUPPORT)\n'
      'target_link_libraries(httplib INTERFACE ${OPENSSL_LIBRARIES})\n',
      'if (NOT EMUSUNI_PORT)  # EmuSuNi: cpp-httplib without SSL\n'
      '    target_compile_options(httplib INTERFACE -DCPPHTTPLIB_OPENSSL_SUPPORT)\n'
      '    target_link_libraries(httplib INTERFACE ${OPENSSL_LIBRARIES})\n'
      'endif()\n')
# The PS4's CMake system is "Generic" (OpenOrbis' toolchain): glslang's POSIX OS layer (upstream
# picks it only for UNIX/Fuchsia/Android), as the PSP port does.
patch('externals/glslang/CMakeLists.txt',
      'elseif(UNIX OR ANDROID)\n    add_compile_definitions(GLSLANG_OSINCLUDE_UNIX)\n',
      'elseif(UNIX OR ANDROID OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n    add_compile_definitions(GLSLANG_OSINCLUDE_UNIX)\n')
patch('externals/glslang/glslang/CMakeLists.txt',
      'elseif(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID)\n    add_subdirectory(OSDependent/Unix)\n',
      'elseif(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n'
      '    add_subdirectory(OSDependent/Unix)\n')
patch('externals/glslang/glslang/CMakeLists.txt',
      'if(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID)\n    set(THREADS_PREFER_PTHREAD_FLAG ON)\n',
      'if(UNIX OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Fuchsia" OR ANDROID OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too\n'
      '    set(THREADS_PREFER_PTHREAD_FLAG ON)\n')
patch('src/common/CMakeLists.txt',
      'add_library(citra_common STATIC EXCLUDE_FROM_ALL\n    aarch64/cpu_detect.cpp\n',
      'add_library(citra_common STATIC EXCLUDE_FROM_ALL\n'
      '    emusuni_dmem.h\n    emusuni_fastmem.cpp\n    emusuni_fastmem.h\n    emusuni_fault.h\n'
      '    emusuni_jit.cpp\n    emusuni_jit.h\n    emusuni_mem.cpp\n    emusuni_mem.h\n    emusuni_random.cpp\n'
      '    aarch64/cpu_detect.cpp\n')

# --- The JITs' code memory: one mapping the app makes executable when the first game starts, shared out
# to dynarmic's blocks of code and the PICA shader JIT, given back when the core shuts down.
addfile('src/common/emusuni_jit.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the JITs' code memory. One mapping the app makes readable,
// writable and executable when the first game starts (port/ps4_n64.c: emusuni_map_executable), shared
// out to dynarmic's blocks of code (core/arm/dynarmic) and the PICA shader JIT's (video_core/shader),
// and given back when the core shuts down (retro_deinit). Without it the game doesn't start
// (core/core.cpp): never part of the executable, whose whole .bss the PS4 charges at load.
#pragma once
#ifdef EMUSUNI_PORT
#include <cstddef>

namespace Xbyak {
struct Allocator;
}

namespace EmuSuNi {
// Whether the JIT memory is there: mapped on the first call after the core starts (a refusal is tried
// again by the next game).
bool JitAvailable();
// A block of it (16 KiB granularity), or of a mapping of its own when it is full; nullptr when neither
// is possible.
void* JitAlloc(std::size_t size);
void JitFree(void* p);
// For Xbyak::CodeGenerator (the shader JIT): the JIT memory, no mprotect.
Xbyak::Allocator* XbyakAllocator();
// The core shuts down (retro_deinit): the mapping back to the system when nothing of it is in use.
void JitRelease();
} // namespace EmuSuNi
#endif
''')
addfile('src/common/emusuni_jit.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): see emusuni_jit.h.
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <xbyak/xbyak.h>
#include "common/common_types.h"
#include "common/emusuni_jit.h"
#include "common/logging/log.h"

// The app's (port/ps4_n64.c; on this PC the test frontend's): `size` bytes to write code in and run it,
// readable, writable and executable (on the PS4: system flexible memory on a range reserved above the
// app's image and heap, checked by running code there), or nullptr when the system refuses (the app's
// log says why); emusuni_unmap_executable gives them back.
extern "C" void* emusuni_map_executable(size_t size, const char* name);
extern "C" void emusuni_unmap_executable(void* address, size_t size);

namespace EmuSuNi {
namespace {
// 64 MiB: dynarmic's blocks (24 MiB per emulated core on an Old 3DS, 12 MiB each on a New 3DS: 48 MiB
// either way) and 16 MiB for the PICA shader JIT (256 KiB a shader). Wherever the system puts it: both
// JITs reach the core's code and data through far calls and absolute addresses, never 32-bit offsets.
constexpr size_t POOL_SIZE = 64u << 20;
constexpr size_t GRANULE = 16384; // the PS4's page
u8* g_pool = nullptr;
std::mutex g_lock;
int g_state = 0; // 0: not mapped (not tried yet, or given back), 1: mapped, -1: refused
std::map<size_t, size_t> g_free;  // offset -> size of each free block of the pool
std::map<void*, size_t> g_used;   // block -> size, of the pool's blocks
std::map<void*, size_t> g_mapped; // blocks mapped on their own when the pool was full -> their size
size_t g_test_cap = 0;            // the PC tests' cap on the pool (EMUSUNI_3DS_JIT_TEST_KIB), 0 otherwise

bool Available() {
    if (g_state == 0) {
        g_pool = static_cast<u8*>(emusuni_map_executable(POOL_SIZE, "3ds jit"));
        g_state = g_pool != nullptr ? 1 : -1;
        if (g_pool != nullptr) {
            g_free.clear();
            g_free[0] = POOL_SIZE;
            g_test_cap = 0;
            // (The PC tests: EMUSUNI_3DS_JIT_TEST_KIB caps the pool and refuses the mappings of their own
            // below, as a console short of both would; nothing sets it on the PS4.)
            if (const char* cap = std::getenv("EMUSUNI_3DS_JIT_TEST_KIB")) {
                g_test_cap = (static_cast<size_t>(std::strtoull(cap, nullptr, 10)) * 1024) & ~(GRANULE - 1);
                if (g_test_cap > 0 && g_test_cap < POOL_SIZE) {
                    g_free[0] = g_test_cap;
                    LOG_WARNING(Common, "EmuSuNi: the JIT memory capped at {} KiB for a test", g_test_cap / 1024);
                }
            }
            LOG_INFO(Common, "EmuSuNi: {} MiB of executable JIT memory at {}", POOL_SIZE >> 20,
                     static_cast<void*>(g_pool));
        } else {
            LOG_CRITICAL(Common, "EmuSuNi: no executable JIT memory ({} MiB): the game can't start",
                         POOL_SIZE >> 20);
        }
    }
    return g_state > 0;
}

class PoolAllocator final : public Xbyak::Allocator {
public:
    uint8_t* alloc(size_t size) override {
        void* p = JitAlloc(size);
        if (p == nullptr) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        return static_cast<uint8_t*>(p);
    }
    void free(uint8_t* p) override {
        JitFree(p);
    }
    bool useProtect() const override {
        return false;
    }
};
PoolAllocator g_xbyak_allocator;
} // namespace

bool JitAvailable() {
    std::lock_guard guard{g_lock};
    return Available();
}

void* JitAlloc(size_t size) {
    std::lock_guard guard{g_lock};
    if (!Available() || size == 0) {
        return nullptr;
    }
    const size_t want = (size + GRANULE - 1) & ~(GRANULE - 1);
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        if (it->second < want) {
            continue;
        }
        const size_t offset = it->first, left = it->second - want;
        g_free.erase(it);
        if (left) {
            g_free[offset + want] = left;
        }
        g_used[g_pool + offset] = want;
        return g_pool + offset;
    }
    // Full (the callers cope: a smaller block of code, the shader interpreter). A last try: a mapping
    // of its own, made the same way.
    size_t left = 0;
    for (const auto& block : g_free) {
        left += block.second;
    }
    LOG_WARNING(Common, "EmuSuNi: the JIT memory is full: {} KiB wanted, {} KiB free in {} pieces", want / 1024,
                left / 1024, g_free.size());
    if (g_test_cap != 0) {
        return nullptr; // (the test: no mapping either)
    }
    void* p = emusuni_map_executable(want, "3ds jit (more)");
    if (p == nullptr) {
        LOG_ERROR(Common, "EmuSuNi: the JIT memory is full and no mapping for {} KiB more", want / 1024);
        return nullptr;
    }
    LOG_WARNING(Common, "EmuSuNi: the JIT memory is full: {} KiB mapped at {}", want / 1024, p);
    g_mapped[p] = want;
    return p;
}

void JitFree(void* p) {
    if (p == nullptr) {
        return;
    }
    std::lock_guard guard{g_lock};
    if (auto mapped = g_mapped.find(p); mapped != g_mapped.end()) {
        emusuni_unmap_executable(p, mapped->second);
        g_mapped.erase(mapped);
        return;
    }
    auto used = g_used.find(p);
    if (used == g_used.end()) {
        LOG_ERROR(Common, "EmuSuNi: freeing {}, not a block of the JIT memory", p);
        return;
    }
    size_t offset = static_cast<u8*>(p) - g_pool;
    size_t length = used->second;
    g_used.erase(used);
    // Back to the free blocks, merged with its neighbours.
    auto next = g_free.lower_bound(offset);
    if (next != g_free.end() && offset + length == next->first) {
        length += next->second;
        next = g_free.erase(next);
    }
    if (next != g_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += length;
            return;
        }
    }
    g_free[offset] = length;
}

Xbyak::Allocator* XbyakAllocator() {
    return &g_xbyak_allocator;
}

void JitRelease() {
    std::lock_guard guard{g_lock};
    if (g_state < 0) {
        g_state = 0; // (refused: the next game asks again)
        return;
    }
    if (g_state == 0) {
        return;
    }
    if (!g_used.empty() || !g_mapped.empty()) {
        // (Code of the emulator still there: the mapping stays, as it is, for the next game.)
        LOG_WARNING(Common, "EmuSuNi: the JIT memory kept: {} blocks of it still in use",
                    g_used.size() + g_mapped.size());
        return;
    }
    emusuni_unmap_executable(g_pool, POOL_SIZE);
    LOG_INFO(Common, "EmuSuNi: the {} MiB of JIT memory given back", POOL_SIZE >> 20);
    g_pool = nullptr;
    g_free.clear();
    g_state = 0;
}
} // namespace EmuSuNi
#endif
''')
patch('externals/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp',
      '#else\n    static constexpr size_t DYNARMIC_PAGE_SIZE = 4096;\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '    // EmuSuNi (scripts/build-3ds-core.sh): the blocks of code from EmuSuNi\'s pool of executable\n'
      '    // memory (src/common/emusuni_jit.cpp): never mmap and mprotect, which the PS4 refuses.\n'
      '    uint8_t* alloc(size_t size) override {\n'
      '        void* p = EmuSuNi::JitAlloc(size);\n'
      '        if (p == nullptr) {\n'
      '            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);\n'
      '        }\n'
      '        return static_cast<uint8_t*>(p);\n'
      '    }\n\n'
      '    void free(uint8_t* p) override {\n'
      '        EmuSuNi::JitFree(p);\n'
      '    }\n\n'
      '    bool useProtect() const override { return false; }\n'
      '#else\n    static constexpr size_t DYNARMIC_PAGE_SIZE = 4096;\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp',
      'namespace Dynarmic::Backend::X64 {\n\n#ifdef _WIN32\nconst Xbyak::Reg64 BlockOfCode::ABI_RETURN',
      '#ifdef EMUSUNI_PORT\n'
      'namespace EmuSuNi {  // src/common/emusuni_jit.cpp\n'
      'void* JitAlloc(size_t size);\n'
      'void JitFree(void* p);\n'
      '}  // namespace EmuSuNi\n'
      '#endif\n\n'
      'namespace Dynarmic::Backend::X64 {\n\n#ifdef _WIN32\nconst Xbyak::Reg64 BlockOfCode::ABI_RETURN')
# dynarmic's spin lock (the exclusive monitor's: every LDREX/STREX takes it) was two routines emitted
# with Xbyak by a global CodeGenerator at static initialisation: Xbyak's own mmap and mprotect(RWX)
# before main, for every EmuSuNi user whatever console they play (some PS4 firmwares refuse RWX: the
# app would have ended at launch). The end check on mprotect can't see such a generator (Xbyak's
# inline code names mprotect in every one), so none may exist. The same protocol in C++: the JIT's
# blocks take and release the lock with it (backend/x64/emit_x64_memory.h: lock xchg, test, pause;
# xchg 0, mfence).
patch('externals/dynarmic/src/dynarmic/common/spin_lock_x64.cpp',
      r'''namespace {

struct SpinLockImpl {
    void Initialize();

    Xbyak::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;
SpinLockImpl impl;

void SpinLockImpl::Initialize() {
    const Xbyak::Reg64 ABI_PARAM1 = Backend::X64::HostLocToReg64(Backend::X64::ABI_PARAM1);

    code.align();
    lock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, ABI_PARAM1, code.eax);
    code.ret();

    code.align();
    unlock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, ABI_PARAM1, code.eax);
    code.ret();
}

}  // namespace

void SpinLock::Lock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}
''',
      r'''#ifdef EMUSUNI_PORT
// EmuSuNi (scripts/build-3ds-core.sh): upstream emitted these two routines with Xbyak at static
// initialisation (a global CodeGenerator: its own mmap and mprotect(RWX) before main, whichever
// console runs; some PS4 firmwares refuse RWX and the app would end at launch). The same protocol
// in C++: the JIT's blocks take and release the exclusive monitor's lock with it
// (backend/x64/emit_x64_memory.h: lock xchg, test, pause; xchg 0, mfence).
void SpinLock::Lock() {
    while (__atomic_exchange_n(&storage, 1, __ATOMIC_ACQUIRE) != 0) {
        __builtin_ia32_pause();
    }
}

void SpinLock::Unlock() {
    __atomic_exchange_n(&storage, 0, __ATOMIC_SEQ_CST);
    __builtin_ia32_mfence();
}
#else
namespace {

struct SpinLockImpl {
    void Initialize();

    Xbyak::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;
SpinLockImpl impl;

void SpinLockImpl::Initialize() {
    const Xbyak::Reg64 ABI_PARAM1 = Backend::X64::HostLocToReg64(Backend::X64::ABI_PARAM1);

    code.align();
    lock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, ABI_PARAM1, code.eax);
    code.ret();

    code.align();
    unlock = code.getCurr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, ABI_PARAM1, code.eax);
    code.ret();
}

}  // namespace

void SpinLock::Lock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() {
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}
#endif
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/settings.h"\n'
      '#include "core/hle/kernel/kernel.h"\n#include "core/hle/kernel/process.h"\n#endif\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.define_unpredictable_behaviour = true;\n\n    // Multi-process state\n',
      '    config.define_unpredictable_behaviour = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a share of the 64 MiB pool (common/emusuni_jit.cpp) for each emulated core (upstream\n'
      '    // reserved 128 MiB each): 24 MiB on an Old 3DS (2 cores), 12 MiB on a New 3DS (4 cores).\n'
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n'
      '#endif\n\n'
      '    // Multi-process state\n')
# The pool may not have a whole share left (the shader JIT, a process more): half the size again,
# down to 6 MiB (24, 12, 6; 12, 6 on a New 3DS); nothing at all is a clear error for the loader
# (do_load_game catches it: the game fails with a message). Upstream let Xbyak's exception out of
# the process switch end the app.
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.global_monitor = &exclusive_monitor.monitor;\n\n'
      '    return std::make_unique<Dynarmic::A32::Jit>(config);\n}\n',
      '    config.global_monitor = &exclusive_monitor.monitor;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the block of code comes from the 64 MiB pool (common/emusuni_jit.cpp). When it hasn\'t\n'
      '    // that much left, half the size again, down to 6 MiB (the cache flushes more often: slower, not\n'
      '    // dead); nothing at all is a clear error for the loader (upstream let Xbyak\'s end the app).\n'
      '    for (;;) {\n'
      '        try {\n'
      '            return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '        } catch (const std::exception& error) {\n'
      '            if (config.code_cache_size < 8u << 20) {\n'
      '                throw std::runtime_error(fmt::format("no executable memory for the CPU JIT ({} MiB: {})",\n'
      '                                                     config.code_cache_size >> 20, error.what()));\n'
      '            }\n'
      '            LOG_WARNING(Core_ARM11, "EmuSuNi: no {} MiB of JIT memory for core {} ({}): {} MiB tried",\n'
      '                        config.code_cache_size >> 20, GetID(), error.what(), config.code_cache_size >> 21);\n'
      '            config.code_cache_size /= 2;\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '#endif\n'
      '}\n')
# Each core's JIT is made before any process exists (a null page table) and again for the process:
# upstream kept the first (a map by page table, 128 MiB each reserved, never touched), and the JITs of
# processes that ended and of the processes a save state's load replaced (new page tables). With the
# pool shared out they are given back here, before the new JIT takes its share: a page table is dead
# when no process of the kernel's list has it (the cores' maps alone hold it).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    auto new_jit = MakeJit();\n    jit = new_jit.get();\n    LoadContext(ctx);\n'
      '    jits.emplace(current_page_table, std::move(new_jit));\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the JITs of page tables no process has any more (the one made before any process\n'
      '    // existed, a process that ended, the processes a save state\'s load replaced): their shares of\n'
      '    // the pool back (common/emusuni_jit.cpp) before the new JIT takes its own. Upstream kept them\n'
      '    // all, 128 MiB reserved each. (The context was saved above; jit is set again below.)\n'
      '    jit = nullptr;\n'
      '    for (auto it = jits.begin(); it != jits.end();) {\n'
      '        bool live = false;\n'
      '        if (it->first != nullptr) {\n'
      '            for (const auto& process : system.Kernel().GetProcessList()) {\n'
      '                live |= process->vm_manager.page_table == it->first;\n'
      '            }\n'
      '        }\n'
      '        if (live) {\n'
      '            ++it;\n'
      '        } else {\n'
      '            LOG_INFO(Core_ARM11, "EmuSuNi: core {}: the JIT of a page table no process has given back", GetID());\n'
      '            it = jits.erase(it);\n'
      '        }\n'
      '    }\n'
      '#endif\n'
      '    auto new_jit = MakeJit();\n    jit = new_jit.get();\n    LoadContext(ctx);\n'
      '    jits.emplace(current_page_table, std::move(new_jit));\n'
      '}\n')
patch('src/core/core.cpp',
      '#include "common/scope_exit.h"\n#include "common/settings.h"\n',
      '#include "common/scope_exit.h"\n#include "common/settings.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <stdexcept>\n'
      '#include "common/emusuni_jit.h"\n'
      '// EmuSuNi: dynarmic in the JIT memory the app maps when the game starts (common/emusuni_jit.cpp).\n'
      '// Without it the game doesn\'t start (do_load_game shows the message, in the player\'s words): DynCom,\n'
      '// the interpreter, is far too slow on the PS4.\n'
      'static bool EmuSuNiCpuJit() {\n'
      '    if (!Settings::values.use_cpu_jit.GetValue()) {\n'
      '        return false;\n'
      '    }\n'
      '    if (!EmuSuNi::JitAvailable()) {\n'
      '        throw std::runtime_error("The PS4 gave no memory for the 3DS\'s processor\\n"\n'
      '                                 "(64 MiB to run its code: /data/EmuSuNi/emusuni.log says why)");\n'
      '    }\n'
      '    return true;\n'
      '}\n'
      '#define EMUSUNI_3DS_CPU_JIT EmuSuNiCpuJit()\n'
      '#else\n'
      '#define EMUSUNI_3DS_CPU_JIT Settings::values.use_cpu_jit\n'
      '#endif\n')
patch('src/core/core.cpp',
      '    if (Settings::values.use_cpu_jit) {\n#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)\n',
      '    if (EMUSUNI_3DS_CPU_JIT) {\n#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)\n')
patch('src/video_core/shader/shader.cpp',
      '#include "common/arch.h"\n#include "video_core/shader/shader_interpreter.h"\n',
      '#include "common/arch.h"\n#include "video_core/shader/shader_interpreter.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "common/emusuni_jit.h"\n'
      '// EmuSuNi: the shader JIT only when its code memory is executable (else the interpreter).\n'
      '#define EMUSUNI_3DS_SHADER_JIT && EmuSuNi::JitAvailable()\n'
      '#else\n'
      '#define EMUSUNI_3DS_SHADER_JIT\n'
      '#endif\n')
patch('src/video_core/shader/shader.cpp',
      '    if (use_jit) {\n        return std::make_unique<Shader::JitEngine>();\n',
      '    if (use_jit EMUSUNI_3DS_SHADER_JIT) {\n        return std::make_unique<Shader::JitEngine>();\n')
patch('src/video_core/shader/shader_jit_x64_compiler.cpp',
      '#include "common/x64/xbyak_util.h"\n#include "video_core/pica/shader_unit.h"\n',
      '#include "common/x64/xbyak_util.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_jit.h"\n#endif\n'
      '#include "video_core/pica/shader_unit.h"\n')
patch('src/video_core/shader/shader_jit_x64_compiler.cpp',
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE) {}\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the code from the pool of executable memory (common/emusuni_jit.cpp), never Xbyak\'s mmap\n'
      '// and mprotect.\n'
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE, nullptr, EmuSuNi::XbyakAllocator()) {}\n'
      '#else\n'
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE) {}\n'
      '#endif\n')
# A shader program that gets no block of the pool (256 KiB each, never freed by upstream's cache; the
# pool full) runs in the shader interpreter instead: upstream threw Xbyak's error out of retro_run.
patch('src/video_core/shader/shader_jit.h',
      '#include <memory>\n#include <unordered_map>\n#include "common/common_types.h"\n'
      '#include "video_core/shader/shader.h"\n',
      '#include <memory>\n#include <unordered_map>\n'
      '#ifdef EMUSUNI_PORT\n#include <unordered_set>\n#endif\n'
      '#include "common/common_types.h"\n'
      '#include "video_core/shader/shader.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "video_core/shader/shader_interpreter.h"\n#endif\n')
patch('src/video_core/shader/shader_jit.h',
      'private:\n    std::unordered_map<u64, std::unique_ptr<JitShader>> cache;\n};\n',
      'private:\n    std::unordered_map<u64, std::unique_ptr<JitShader>> cache;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the programs that got no block of the JITs\' memory run here instead.\n'
      '    InterpreterEngine emusuni_interpreter;\n'
      '    std::unordered_set<u64> emusuni_interpreted;\n'
      '#endif\n'
      '};\n')
patch('src/video_core/shader/shader_jit.cpp',
      '#include "common/microprofile.h"\n#include "video_core/shader/shader.h"\n',
      '#include "common/microprofile.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/logging/log.h"\n#endif\n'
      '#include "video_core/shader/shader.h"\n')
patch('src/video_core/shader/shader_jit.cpp',
      '    auto iter = cache.find(cache_key);\n'
      '    if (iter != cache.end()) {\n'
      '        setup.cached_shader = iter->second.get();\n'
      '    } else {\n'
      '        auto shader = std::make_unique<JitShader>();\n'
      '        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());\n'
      '        setup.cached_shader = shader.get();\n'
      '        cache.emplace_hint(iter, cache_key, std::move(shader));\n'
      '    }\n'
      '}\n',
      '    auto iter = cache.find(cache_key);\n'
      '    if (iter != cache.end()) {\n'
      '        setup.cached_shader = iter->second.get();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    } else if (emusuni_interpreted.contains(cache_key)) {\n'
      '        setup.cached_shader = nullptr; // EmuSuNi: interpreted (Run)\n'
      '    } else {\n'
      '        // EmuSuNi: a program that gets no block of the JITs\' memory (common/emusuni_jit.cpp: the pool\n'
      '        // full, no mapping) runs in the interpreter; upstream threw Xbyak\'s error out of retro_run.\n'
      '        std::unique_ptr<JitShader> shader;\n'
      '        try {\n'
      '            shader = std::make_unique<JitShader>();\n'
      '        } catch (const Xbyak::Error& error) {\n'
      '            LOG_ERROR(HW_GPU, "EmuSuNi: no JIT memory for shader program {:016X} ({}): interpreted", cache_key,\n'
      '                      error.what());\n'
      '            emusuni_interpreted.insert(cache_key);\n'
      '            setup.cached_shader = nullptr;\n'
      '            return;\n'
      '        }\n'
      '        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());\n'
      '        setup.cached_shader = shader.get();\n'
      '        cache.emplace_hint(iter, cache_key, std::move(shader));\n'
      '    }\n'
      '#else\n'
      '    } else {\n'
      '        auto shader = std::make_unique<JitShader>();\n'
      '        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());\n'
      '        setup.cached_shader = shader.get();\n'
      '        cache.emplace_hint(iter, cache_key, std::move(shader));\n'
      '    }\n'
      '#endif\n'
      '}\n')
patch('src/video_core/shader/shader_jit.cpp',
      'void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {\n'
      '    ASSERT(setup.cached_shader != nullptr);\n',
      'void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (setup.cached_shader == nullptr) { // EmuSuNi: no block of JIT memory for it (SetupBatch)\n'
      '        emusuni_interpreter.Run(setup, state);\n'
      '        return;\n'
      '    }\n'
      '#else\n'
      '    ASSERT(setup.cached_shader != nullptr);\n'
      '#endif\n')

# --- The libretro glue: what the emulation throws (the JITs\' memory, above) ends the game, never the
# app. Upstream caught nothing: the exception left retro_run or context_reset and terminated.
patch('src/citra_libretro/citra_libretro.cpp',
      '#include <list>\n#include <numeric>\n#include <vector>\n',
      '#include <list>\n#include <numeric>\n#include <vector>\n'
      '#ifdef EMUSUNI_PORT\n#include <exception>\n#include <string>\n#include "common/emusuni_jit.h"\n#endif\n')
# The JITs' memory (common/emusuni_jit.cpp) back to the system when the core shuts down: the emulator is
# gone, so nothing of it is in use (it says so and keeps the mapping if something is).
patch('src/citra_libretro/citra_libretro.cpp',
      '    delete emu_instance;\n\n    Common::Log::Stop();\n}\n',
      '    delete emu_instance;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::JitRelease(); // EmuSuNi: the JIT memory back to the system (the next game maps it again)\n'
      '#endif\n\n'
      '    Common::Log::Stop();\n}\n')
patch('src/citra_libretro/citra_libretro.cpp',
      'static bool do_load_game() {\n'
      '    const Core::System::ResultStatus load_result{\n'
      '        Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path)};\n\n'
      '    switch (load_result) {\n',
      'static bool do_load_game() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: what the load throws (no executable memory for the CPU JIT: core/arm/dynarmic) fails\n'
      '    // the game with a message; upstream let it end the app.\n'
      '    Core::System::ResultStatus load_result{};\n'
      '    try {\n'
      '        load_result =\n'
      '            Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path);\n'
      '    } catch (const std::exception& error) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: loading the game threw: {}", error.what());\n'
      '        // (A message of the port\'s own, in lines, is the player\'s as it is: no JIT memory, core/core.cpp.)\n'
      '        const std::string what = error.what();\n'
      '        LibRetro::DisplayMessage(what.find(\'\\n\') != std::string::npos\n'
      '                                     ? what.c_str()\n'
      '                                     : ("Azahar could not start the game: " + what).c_str());\n'
      '        return false;\n'
      '    }\n'
      '#else\n'
      '    const Core::System::ResultStatus load_result{\n'
      '        Core::System::GetInstance().Load(*emu_instance->emu_window, LibRetro::settings.file_path)};\n'
      '#endif\n\n'
      '    switch (load_result) {\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    if (!emu_instance->game_loaded) {\n'
      '        emu_instance->game_loaded = do_load_game();\n'
      '    } else {\n',
      '    if (!emu_instance->game_loaded) {\n'
      '        emu_instance->game_loaded = do_load_game();\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (!emu_instance->game_loaded) {\n'
      '            LibRetro::Shutdown(); // EmuSuNi: back to the menu with the message, not a black screen\n'
      '        }\n'
      '#endif\n'
      '    } else {\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    while (!emu_instance->emu_window->HasSubmittedFrame()) {\n'
      '        auto result = Core::System::GetInstance().RunLoop();\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '#endif\n'
      '    while (!emu_instance->emu_window->HasSubmittedFrame()) {\n'
      '        auto result = Core::System::GetInstance().RunLoop();\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '            LibRetro::DisplayMessage(msg.c_str());\n'
      '        }\n'
      '    }\n'
      '}\n\n'
      'static void setup_memory_maps() {\n',
      '            LibRetro::DisplayMessage(msg.c_str());\n'
      '        }\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    } catch (const std::exception& error) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: the emulation threw: {}", error.what());\n'
      '        LibRetro::DisplayMessage((std::string("Azahar stopped: ") + error.what()).c_str());\n'
      '        emu_instance->game_loaded = false; // (empty frames until the frontend stops the game)\n'
      '        LibRetro::Shutdown();\n'
      '    }\n'
      '#endif\n'
      '}\n\n'
      'static void setup_memory_maps() {\n')

# The Vulkan pipeline cache reads through its shader cache object, which only exists once the disk
# resources were loaded: never with the disk cache option off, nor after a save state's load made
# the renderer again (upstream's own note at SwitchDiskCache). It is made on first use instead (the
# disk cache object honours the option itself).
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    /// Switches the disk cache at runtime to use a different title ID\n'
      '    void SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,\n'
      '                         const VideoCore::DiskResourceLoadCallback& callback);\n',
      '    /// Switches the disk cache at runtime to use a different title ID\n'
      '    void SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,\n'
      '                         const VideoCore::DiskResourceLoadCallback& callback);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: the shader cache object, made on first use when none was loaded\n'
      '    const std::shared_ptr<ShaderDiskCache>& EmuSuNiDiskCache();\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'bool PipelineCache::UseProgrammableVertexShader(const Pica::RegsInternal& regs,\n',
      '#ifdef EMUSUNI_PORT\n'
      'const std::shared_ptr<ShaderDiskCache>& PipelineCache::EmuSuNiDiskCache() {\n'
      '    if (!curr_disk_cache) {\n'
      '        // EmuSuNi: no disk resources were loaded (the option off, or the renderer made again by a save\n'
      '        // state\'s load): the cache object for the program anyway (it reads no file when the option is\n'
      '        // off). Upstream read through the null pointer.\n'
      '        LOG_WARNING(Render_Vulkan, "EmuSuNi: no shader cache loaded for title {:016X}: made now", GetProgramID());\n'
      '        LoadDiskCache();\n'
      '    }\n'
      '    return curr_disk_cache;\n'
      '}\n'
      '#endif\n\n'
      'bool PipelineCache::UseProgrammableVertexShader(const Pica::RegsInternal& regs,\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    GraphicsPipeline* const pipeline = curr_disk_cache->GetPipeline(info);\n',
      '    GraphicsPipeline* const pipeline = EMUSUNI_DISK_CACHE->GetPipeline(info);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    auto res = curr_disk_cache->UseProgrammableVertexShader(regs, setup, layout);\n',
      '    auto res = EMUSUNI_DISK_CACHE->UseProgrammableVertexShader(regs, setup, layout);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    auto res = curr_disk_cache->UseFixedGeometryShader(regs);\n',
      '    auto res = EMUSUNI_DISK_CACHE->UseFixedGeometryShader(regs);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    auto res = curr_disk_cache->UseFragmentShader(regs, user);\n',
      '    auto res = EMUSUNI_DISK_CACHE->UseFragmentShader(regs, user);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '#include "video_core/renderer_vulkan/vk_pipeline_cache.h"\n',
      '#include "video_core/renderer_vulkan/vk_pipeline_cache.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#define EMUSUNI_DISK_CACHE EmuSuNiDiskCache()\n'
      '#else\n'
      '#define EMUSUNI_DISK_CACHE curr_disk_cache\n'
      '#endif\n')

# --- Save states: serialised into one vector and compressed from it by frames; read back in place.
# Upstream's string stream, its copy and the one-shot compression peaked at ~5 x the emulated RAM on
# the heap (1.2 GiB measured on a New 3DS), which the PS4's heap hasn't; the load copied the state
# into a string and a string stream (3 x).
patch('src/common/zstd_compression.h',
      '[[nodiscard]] std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed);\n',
      '[[nodiscard]] std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed);\n'
      '\n#ifdef EMUSUNI_PORT\n'
      '/**\n'
      ' * EmuSuNi: compresses source by frames of 1 MiB, appending to out (no buffer the size of the\n'
      ' * input); the frame carries the content size, as CompressDataZSTD\'s does.\n'
      ' */\n'
      'bool CompressDataZSTDStreaming(std::span<const u8> source, std::vector<u8>& out);\n'
      '#endif\n')
patch('src/common/zstd_compression.cpp',
      'std::vector<u8> CompressDataZSTDDefault(std::span<const u8> source) {\n'
      '    return CompressDataZSTD(source, ZSTD_CLEVEL_DEFAULT);\n}\n',
      'std::vector<u8> CompressDataZSTDDefault(std::span<const u8> source) {\n'
      '    return CompressDataZSTD(source, ZSTD_CLEVEL_DEFAULT);\n}\n'
      '\n#ifdef EMUSUNI_PORT\n'
      'bool CompressDataZSTDStreaming(std::span<const u8> source, std::vector<u8>& out) {\n'
      '    ZSTD_CStream* stream = ZSTD_createCStream();\n'
      '    if (stream == nullptr) {\n'
      '        return false;\n'
      '    }\n'
      '    ZSTD_CCtx_setParameter(stream, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);\n'
      '    ZSTD_CCtx_setPledgedSrcSize(stream, source.size()); // the content size in the frame header\n'
      '    std::vector<u8> buffer(ZSTD_CStreamOutSize());\n'
      '    std::size_t pos = 0;\n'
      '    bool ok = true;\n'
      '    while (ok) {\n'
      '        const std::size_t chunk = std::min<std::size_t>(source.size() - pos, 1u << 20);\n'
      '        ZSTD_inBuffer in{source.data() + pos, chunk, 0};\n'
      '        const ZSTD_EndDirective mode = pos + chunk == source.size() ? ZSTD_e_end : ZSTD_e_continue;\n'
      '        std::size_t remaining = 0;\n'
      '        do {\n'
      '            ZSTD_outBuffer o{buffer.data(), buffer.size(), 0};\n'
      '            remaining = ZSTD_compressStream2(stream, &o, &in, mode);\n'
      '            if (ZSTD_isError(remaining)) {\n'
      '                LOG_ERROR(Common, "Error compressing ZSTD data: {}", ZSTD_getErrorName(remaining));\n'
      '                ok = false;\n'
      '                break;\n'
      '            }\n'
      '            out.insert(out.end(), buffer.data(), buffer.data() + o.pos);\n'
      '        } while (mode == ZSTD_e_end ? remaining != 0 : in.pos < in.size);\n'
      '        pos += chunk;\n'
      '        if (mode == ZSTD_e_end) {\n'
      '            break;\n'
      '        }\n'
      '    }\n'
      '    ZSTD_freeCStream(stream);\n'
      '    return ok;\n'
      '}\n'
      '#endif\n')
patch('src/core/savestate.cpp',
      '#include "network/network.h"\n',
      '#include "network/network.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <span>\n#include <streambuf>\n'
      '#include "common/settings.h"\n'
      '#include "core/memory.h"\n\n'
      'namespace EmuSuNi {\n'
      '// The archive written straight into a vector (room reserved first).\n'
      'class VectorStreamBuf : public std::streambuf {\n'
      'public:\n'
      '    void Reserve(std::size_t size) {\n'
      '        data.reserve(size);\n'
      '    }\n'
      '    std::span<const u8> Data() const {\n'
      '        return data;\n'
      '    }\n\n'
      'protected:\n'
      '    std::streamsize xsputn(const char* s, std::streamsize n) override {\n'
      '        data.insert(data.end(), s, s + n);\n'
      '        return n;\n'
      '    }\n'
      '    int overflow(int c) override {\n'
      '        if (c != traits_type::eof()) {\n'
      '            data.push_back(static_cast<u8>(c));\n'
      '        }\n'
      '        return c;\n'
      '    }\n\n'
      'private:\n'
      '    std::vector<u8> data;\n'
      '};\n\n'
      '// The archive read in place.\n'
      'class SpanStreamBuf : public std::streambuf {\n'
      'public:\n'
      '    explicit SpanStreamBuf(std::vector<u8>& v) {\n'
      '        char* begin = reinterpret_cast<char*>(v.data());\n'
      '        setg(begin, begin, begin + v.size());\n'
      '    }\n'
      '};\n'
      '} // namespace EmuSuNi\n'
      '#endif\n')
patch('src/core/savestate.cpp',
      'std::vector<u8> System::SaveStateBuffer() const {\n'
      '    std::ostringstream sstream{std::ios_base::binary};\n'
      '    // Serialize\n'
      '    oarchive oa{sstream};\n'
      '    oa&* this;\n\n'
      '    const std::string& str{sstream.str()};\n'
      '    const auto data = std::span<const u8>{reinterpret_cast<const u8*>(str.data()), str.size()};\n'
      '    auto buffer = Common::Compression::CompressDataZSTDDefault(data);\n\n'
      '    CSTHeader header{};\n',
      'std::vector<u8> System::SaveStateBuffer() const {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: into one vector, with room for the emulated RAM reserved (no string copy), then\n'
      '    // compressed from it by frames behind the header.\n'
      '    EmuSuNi::VectorStreamBuf buf;\n'
      '    buf.Reserve((Settings::values.is_new_3ds.GetValue() ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE) +\n'
      '                Memory::VRAM_SIZE + Memory::N3DS_EXTRA_RAM_SIZE + (32u << 20));\n'
      '    {\n'
      '        std::ostream stream{&buf};\n'
      '        oarchive oa{stream};\n'
      '        oa&* this;\n'
      '    }\n'
      '    const auto data = buf.Data();\n'
      '    std::vector<u8> buffer; // (the compressed state goes behind the header: see below)\n'
      '#else\n'
      '    std::ostringstream sstream{std::ios_base::binary};\n'
      '    // Serialize\n'
      '    oarchive oa{sstream};\n'
      '    oa&* this;\n\n'
      '    const std::string& str{sstream.str()};\n'
      '    const auto data = std::span<const u8>{reinterpret_cast<const u8*>(str.data()), str.size()};\n'
      '    auto buffer = Common::Compression::CompressDataZSTDDefault(data);\n'
      '#endif\n\n'
      '    CSTHeader header{};\n')
patch('src/core/savestate.cpp',
      '    std::vector<u8> result((u8*)&header, (u8*)&header + sizeof(header));\n'
      '    std::copy(buffer.begin(), buffer.end(), std::back_inserter(result));\n\n'
      '    return result;\n',
      '    std::vector<u8> result((u8*)&header, (u8*)&header + sizeof(header));\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (!Common::Compression::CompressDataZSTDStreaming(data, result)) {\n'
      '        throw std::runtime_error("Could not compress the save state");\n'
      '    }\n'
      '#else\n'
      '    std::copy(buffer.begin(), buffer.end(), std::back_inserter(result));\n'
      '#endif\n\n'
      '    return result;\n')
patch('src/core/savestate.cpp',
      '    std::vector<u8> state(buffer.begin() + sizeof(CSTHeader), buffer.end());\n'
      '    auto decompressed = Common::Compression::DecompressDataZSTD(state);\n\n'
      '    std::istringstream sstream{\n'
      '        std::string{reinterpret_cast<char*>(decompressed.data()), decompressed.size()},\n'
      '        std::ios_base::binary};\n'
      '    decompressed.clear();\n\n'
      '    // Deserialize\n'
      '    iarchive ia{sstream};\n'
      '    ia&* this;\n\n'
      '    return true;\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: decompressed once from the buffer itself and read in place (upstream copied the\n'
      '    // compressed state, then the decompressed one into a string and a string stream).\n'
      '    auto decompressed = Common::Compression::DecompressDataZSTD(\n'
      '        std::span<const u8>(buffer.data() + sizeof(CSTHeader), buffer.size() - sizeof(CSTHeader)));\n'
      '    if (decompressed.empty()) {\n'
      '        LOG_ERROR(Core, "Save state could not be decompressed");\n'
      '        return false;\n'
      '    }\n'
      '    buffer.clear();\n'
      '    buffer.shrink_to_fit();\n'
      '    EmuSuNi::SpanStreamBuf buf{decompressed};\n'
      '    std::istream stream{&buf};\n\n'
      '    // Deserialize\n'
      '    iarchive ia{stream};\n'
      '    ia&* this;\n\n'
      '    return true;\n'
      '}\n'
      '#else\n'
      '    std::vector<u8> state(buffer.begin() + sizeof(CSTHeader), buffer.end());\n'
      '    auto decompressed = Common::Compression::DecompressDataZSTD(state);\n\n'
      '    std::istringstream sstream{\n'
      '        std::string{reinterpret_cast<char*>(decompressed.data()), decompressed.size()},\n'
      '        std::ios_base::binary};\n'
      '    decompressed.clear();\n\n'
      '    // Deserialize\n'
      '    iarchive ia{sstream};\n'
      '    ia&* this;\n\n'
      '    return true;\n'
      '}\n'
      '#endif\n')

# --- Threads: the app names and places them (emusuni_3ds_thread, port/); the unnamed ones get names.
patch('src/common/thread.cpp',
      '#ifdef __FreeBSD__\n#define cpu_set_t cpuset_t\n#endif\n\nnamespace Common {\n',
      '#ifdef __FreeBSD__\n#define cpu_set_t cpuset_t\n#endif\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (the app, port/): called first thing in every thread Azahar names ("VulkanWorker",\n'
      '// "Pipeline workers", "Shader workers", "citra:Log", "VulkanFence"...): its core, priority and name.\n'
      'extern "C" void emusuni_3ds_thread(const char* name);\n'
      '#endif\n\n'
      'namespace Common {\n')
patch('src/common/thread.cpp',
      'void SetCurrentThreadPriority(ThreadPriority new_priority) {\n    pthread_t this_thread = pthread_self();\n',
      'void SetCurrentThreadPriority(ThreadPriority new_priority) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    (void)new_priority;\n'
      '    return; // EmuSuNi: the hook sets the priorities (the PS4 runs threads by fixed priorities)\n'
      '#endif\n'
      '    pthread_t this_thread = pthread_self();\n')
patch('src/common/thread.cpp',
      '#if !defined(_WIN32) || defined(_MSC_VER)\nvoid SetCurrentThreadName(const char* name) {\n#ifdef __APPLE__\n',
      '#if !defined(_WIN32) || defined(_MSC_VER)\nvoid SetCurrentThreadName(const char* name) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_3ds_thread(name);\n'
      '    return;\n'
      '#endif\n'
      '#ifdef __APPLE__\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: 3 pipeline and 3 shader compile workers (upstream: half the CPUs each). On the PS4 they run\n'
      '// below every other thread of the game on cores 0, 1, 3, 4 and 5 (port/ps4_n64.c), all but the\n'
      '// emulation\'s: a burst of new pipelines (0.25-0.5 s each there) is built 3 at a time while those cores\n'
      '// idle (2 until 00.65: the third\'s core was free; the GPU filter\'s compiles use the same five).\n'
      'static u32 EmuSuNiWorkerThreads() {\n    return 3;\n}\n'
      '#else\n'
      'static u32 EmuSuNiWorkerThreads() {\n    return std::max(std::thread::hardware_concurrency(), 2U) / 2;\n}\n'
      '#endif\n\n'
      'PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '      num_worker_threads{std::max(std::thread::hardware_concurrency(), 2U) / 2},\n',
      '      num_worker_threads{EmuSuNiWorkerThreads()},\n')
patch('src/core/hle/kernel/hle_ipc.h',
      '#include "common/thread_worker.h"\n',
      '#include "common/thread_worker.h"\n#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/kernel/hle_ipc.h',
      '                    std::move(std::async(std::launch::async, [this, async_section] {\n'
      '                        s64 sleep_for = async_section(*this);\n',
      '                    std::move(std::async(std::launch::async, [this, async_section] {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                        Common::SetCurrentThreadName("HleAsync"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '                        s64 sleep_for = async_section(*this);\n')
patch('src/common/detached_tasks.cpp',
      '#include "common/detached_tasks.h"\n',
      '#include "common/detached_tasks.h"\n#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/common/detached_tasks.cpp',
      '    std::thread([task{std::move(task)}]() {\n        task();\n',
      '    std::thread([task{std::move(task)}]() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        Common::SetCurrentThreadName("Detached"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '        task();\n')
# The threads upstream starts without a name (none of them spins, but on the PS4 an unplaced thread
# runs at the default priority on any core): the DSP LLE's own thread (the "lle_multithread" audio
# option: the emulation thread waits for it every slice), Download Play's connection managers, the
# AM title scans, the camera capture and the HTTP requests.
patch('src/audio_core/lle/lle.cpp',
      '    void TeakraThread() {\n        while (true) {\n',
      '    void TeakraThread() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        Common::SetCurrentThreadName("Teakra"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '        while (true) {\n')
patch('src/core/hle/service/dlp/dlp_srvr.cpp',
      '#include "common/archives.h"\n#include "common/common_types.h"\n',
      '#include "common/archives.h"\n#include "common/common_types.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/dlp/dlp_srvr.cpp',
      'void DLP_SRVR::ServerConnectionManager() {\n    auto uds = GetUDS();\n',
      'void DLP_SRVR::ServerConnectionManager() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("DlpServer"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '    auto uds = GetUDS();\n')
patch('src/core/hle/service/dlp/dlp_clt_base.cpp',
      '#include "common/alignment.h"\n#include "common/string_util.h"\n',
      '#include "common/alignment.h"\n#include "common/string_util.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/dlp/dlp_clt_base.cpp',
      'void DLP_Clt_Base::ClientConnectionManager() {\n    auto uds = GetUDS();\n',
      'void DLP_Clt_Base::ClientConnectionManager() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("DlpClient"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '    auto uds = GetUDS();\n')
patch('src/core/hle/service/am/am.cpp',
      '#include "common/alignment.h"\n#include "common/archives.h"\n',
      '#include "common/alignment.h"\n#include "common/archives.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
# (std::launch::async: with the default policy the task may run deferred, on the thread that waits for
# it, the emulation thread, which the hook must never re-place.)
for future, capture in (('scan_tickets_future', '[this]'), ('scan_titles_future', '[this, media_type]'),
                        ('scan_all_future', '[this]')):
    patch('src/core/hle/service/am/am.cpp',
          '        %s = std::async(%s() {\n            std::scoped_lock lock(am_lists_mutex);\n' % (future, capture),
          '#ifdef EMUSUNI_PORT\n'
          '        // EmuSuNi: a thread of its own (never deferred onto the emulation thread), placed by the app.\n'
          '        %s = std::async(std::launch::async, %s() {\n' % (future, capture) +
          '            Common::SetCurrentThreadName("AmScan");\n'
          '#else\n'
          '        %s = std::async(%s() {\n' % (future, capture) +
          '#endif\n'
          '            std::scoped_lock lock(am_lists_mutex);\n')
patch('src/core/hle/service/cam/cam.cpp',
      '#include "common/archives.h"\n#include "common/bit_set.h"\n',
      '#include "common/archives.h"\n#include "common/bit_set.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/cam/cam.cpp',
      '    port.capture_result = std::async(std::launch::async, [&camera, &port, this] {\n',
      '    port.capture_result = std::async(std::launch::async, [&camera, &port, this] {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        Common::SetCurrentThreadName("CamCapture"); // EmuSuNi: the app places it\n'
      '#endif\n')
patch('src/core/hle/service/http/http_c.cpp',
      '#include "common/archives.h"\n#include "common/assert.h"\n',
      '#include "common/archives.h"\n#include "common/assert.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/core/hle/service/http/http_c.cpp',
      'void Context::MakeRequest() {\n    ASSERT(state == RequestState::NotStarted);\n',
      'void Context::MakeRequest() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("HttpRequest"); // EmuSuNi: the app places it (always a std::async task)\n'
      '#endif\n'
      '    ASSERT(state == RequestState::NotStarted);\n')

# --- Vulkan: the frontend's device adopted as it was made (EMUSUNI_VK_DEVICE_EXTENSIONS and
# EMUSUNI_VK_DEVICE_FEATURES), the idle waits under the frontend's lock, the frame images a ring of
# the core's own.
patch('src/citra_libretro/libretro_vk.cpp',
      '#include "common/settings.h"\n',
      '#include "common/settings.h"\n'
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#include "common/thread.h"\n#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '} // namespace LibRetro\n\nnamespace Vulkan {\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the queue is the frontend\'s (its menus draw on it too): every wait for it under its lock.\n'
      'void LockQueue() {\n'
      '    if (vulkan_intf && vulkan_intf->lock_queue) {\n'
      '        vulkan_intf->lock_queue(vulkan_intf->handle);\n'
      '    }\n'
      '}\n\n'
      'void UnlockQueue() {\n'
      '    if (vulkan_intf && vulkan_intf->unlock_queue) {\n'
      '        vulkan_intf->unlock_queue(vulkan_intf->handle);\n'
      '    }\n'
      '}\n\n'
      '// EmuSuNi: what the frontend made its device with (src/Display.cpp): EMUSUNI_VK_DEVICE_EXTENSIONS,\n'
      '// comma-separated; none when unset.\n'
      'std::vector<std::string> EnabledDeviceExtensions() {\n'
      '    std::vector<std::string> list;\n'
      '    const char* env = std::getenv("EMUSUNI_VK_DEVICE_EXTENSIONS");\n'
      '    if (env == nullptr) {\n'
      '        return list;\n'
      '    }\n'
      '    for (const char* at = env; *at != 0;) {\n'
      '        const char* end = std::strchr(at, \',\');\n'
      '        const std::string name = end ? std::string(at, end) : std::string(at);\n'
      '        if (!name.empty()) {\n'
      '            list.push_back(name);\n'
      '        }\n'
      '        at = end ? end + 1 : at + name.size();\n'
      '    }\n'
      '    return list;\n'
      '}\n'
      '#endif\n\n'
      '} // namespace LibRetro\n\nnamespace Vulkan {\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    const std::vector extensions = physical_device.enumerateDeviceExtensionProperties();\n'
      '    available_extensions.reserve(extensions.size());\n'
      '    for (const auto& extension : extensions) {\n'
      '        available_extensions.emplace_back(extension.extensionName.data());\n'
      '    }\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the device was made by the frontend with these (not everything the GPU has).\n'
      '    available_extensions = LibRetro::EnabledDeviceExtensions();\n'
      '    std::string emusuni_listed;\n'
      '    for (const auto& name : available_extensions) {\n'
      '        emusuni_listed += emusuni_listed.empty() ? name : " " + name;\n'
      '    }\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the frontend\'s device has {} extensions enabled: {}",\n'
      '             available_extensions.size(), emusuni_listed);\n'
      '    available_extensions.emplace_back("EmuSuNi"); // (never empty: CreateDevice takes that as no device)\n'
      '#else\n'
      '    const std::vector extensions = physical_device.enumerateDeviceExtensionProperties();\n'
      '    available_extensions.reserve(extensions.size());\n'
      '    for (const auto& extension : extensions) {\n'
      '        available_extensions.emplace_back(extension.extensionName.data());\n'
      '    }\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    // Wait for any pending operations\n    WaitPresent();\n    device.waitIdle();\n',
      '    // Wait for any pending operations\n    WaitPresent();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    LibRetro::LockQueue(); // EmuSuNi: the menus\' queue too\n'
      '    device.waitIdle();\n'
      '    LibRetro::UnlockQueue();\n'
      '#else\n'
      '    device.waitIdle();\n'
      '#endif\n')
# The render pass that writes the frame images: it waits for the fragment shader reads of every
# earlier submission (the frontend may still sample an older image of the ring when the core writes
# it again) and makes its writes visible to the frontend's sampling (upstream's implicit end
# dependency doesn't).
patch('src/citra_libretro/libretro_vk.cpp',
      '    const vk::RenderPassCreateInfo renderpass_info = {\n'
      '        .attachmentCount = 1,\n'
      '        .pAttachments = &color_attachment,\n'
      '        .subpassCount = 1,\n'
      '        .pSubpasses = &subpass,\n'
      '        .dependencyCount = 1,\n'
      '        .pDependencies = &dependency,\n'
      '    };\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: wait for the frontend\'s sampling of the image (an earlier frame of the ring), and\n'
      '    // make the writes visible to its next sampling.\n'
      '    const std::array<vk::SubpassDependency, 2> emusuni_dependencies = {\n'
      '        vk::SubpassDependency{\n'
      '            .srcSubpass = VK_SUBPASS_EXTERNAL,\n'
      '            .dstSubpass = 0,\n'
      '            .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader |\n'
      '                            vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '            .srcAccessMask = {},\n'
      '            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,\n'
      '        },\n'
      '        vk::SubpassDependency{\n'
      '            .srcSubpass = 0,\n'
      '            .dstSubpass = VK_SUBPASS_EXTERNAL,\n'
      '            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,\n'
      '            .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,\n'
      '            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,\n'
      '            .dstAccessMask = vk::AccessFlagBits::eShaderRead,\n'
      '        },\n'
      '    };\n'
      '    const vk::RenderPassCreateInfo renderpass_info = {\n'
      '        .attachmentCount = 1,\n'
      '        .pAttachments = &color_attachment,\n'
      '        .subpassCount = 1,\n'
      '        .pSubpasses = &subpass,\n'
      '        .dependencyCount = static_cast<u32>(emusuni_dependencies.size()),\n'
      '        .pDependencies = emusuni_dependencies.data(),\n'
      '    };\n'
      '#else\n'
      '    const vk::RenderPassCreateInfo renderpass_info = {\n'
      '        .attachmentCount = 1,\n'
      '        .pAttachments = &color_attachment,\n'
      '        .subpassCount = 1,\n'
      '        .pSubpasses = &subpass,\n'
      '        .dependencyCount = 1,\n'
      '        .pDependencies = &dependency,\n'
      '    };\n'
      '#endif\n')
# The frame images: a ring of 3 of the core's own (upstream: 2 slots sharing one image), one step per
# frame presented, never the frontend's sync index (the frontend may call retro_run several times
# between two of its frames, or draw on another thread meanwhile).
patch('src/citra_libretro/libretro_vk.cpp',
      '    const u32 frame_count = 2; // Double buffering for LibRetro\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const u32 frame_count = EMUSUNI_IMAGES; // EmuSuNi: a ring of images of the core\'s own\n'
      '#else\n'
      '    const u32 frame_count = 2; // Double buffering for LibRetro\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '        frame.image = output_image; // All frames use the same output texture\n'
      '        frame.image_view = output_image_view;\n'
      '        frame.allocation = {}; // VMA allocation handled separately\n'
      '        frame.cmdbuf = command_buffers[i];\n',
      '#ifdef EMUSUNI_PORT\n'
      '        if (i == 0) {\n'
      '            frame.image = output_image;\n'
      '            frame.image_view = output_image_view;\n'
      '            frame.allocation = {}; // (the output texture\'s, destroyed with it)\n'
      '        } else {\n'
      '            // EmuSuNi: an image of this frame\'s own, like the output texture.\n'
      '            vk::ImageCreateInfo image_info = {\n'
      '                .imageType = vk::ImageType::e2D,\n'
      '                .format = output_format,\n'
      '                .extent = {output_width, output_height, 1},\n'
      '                .mipLevels = 1,\n'
      '                .arrayLayers = 1,\n'
      '                .samples = vk::SampleCountFlagBits::e1,\n'
      '                .tiling = vk::ImageTiling::eOptimal,\n'
      '                .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc |\n'
      '                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,\n'
      '                .sharingMode = vk::SharingMode::eExclusive,\n'
      '                .initialLayout = vk::ImageLayout::eUndefined,\n'
      '            };\n'
      '            VmaAllocationCreateInfo alloc_create = {};\n'
      '            alloc_create.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;\n'
      '            VkImage vk_image;\n'
      '            const VkResult result = vmaCreateImage(instance.GetAllocator(),\n'
      '                                                   reinterpret_cast<const VkImageCreateInfo*>(&image_info),\n'
      '                                                   &alloc_create, &vk_image, &frame.allocation, nullptr);\n'
      '            if (result != VK_SUCCESS) {\n'
      '                LOG_CRITICAL(Render_Vulkan, "EmuSuNi: failed to create frame image {}: {}", i, static_cast<int>(result));\n'
      '                throw std::runtime_error("Failed to create LibRetro frame image");\n'
      '            }\n'
      '            frame.image = vk::Image{vk_image};\n'
      '            vk::ImageViewCreateInfo view_info = output_view_create_info;\n'
      '            view_info.image = frame.image;\n'
      '            frame.image_view = device.createImageView(view_info);\n'
      '        }\n'
      '#else\n'
      '        frame.image = output_image; // All frames use the same output texture\n'
      '        frame.image_view = output_image_view;\n'
      '        frame.allocation = {}; // VMA allocation handled separately\n'
      '#endif\n'
      '        frame.cmdbuf = command_buffers[i];\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '            .attachmentCount = 1,\n            .pAttachments = &output_image_view,\n            .width = output_width,\n',
      '            .attachmentCount = 1,\n'
      '#ifdef EMUSUNI_PORT\n'
      '            .pAttachments = &frame.image_view, // EmuSuNi: this frame\'s image\n'
      '#else\n'
      '            .pAttachments = &output_image_view,\n'
      '#endif\n'
      '            .width = output_width,\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    for (auto& frame : frame_pool) {\n        if (frame.framebuffer) {\n            device.destroyFramebuffer(frame.framebuffer);\n        }\n',
      '    for (auto& frame : frame_pool) {\n        if (frame.framebuffer) {\n            device.destroyFramebuffer(frame.framebuffer);\n        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (frame.allocation) { // EmuSuNi: an image of this frame\'s own (not the output texture)\n'
      '            device.destroyImageView(frame.image_view);\n'
      '            vmaDestroyImage(instance.GetAllocator(), static_cast<VkImage>(frame.image), frame.allocation);\n'
      '            frame.allocation = {};\n'
      '        }\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    // LibRetro synchronization: Use LibRetro\'s wait mechanism instead of fences\n'
      '    if (vulkan_intf && vulkan_intf->wait_sync_index && vulkan_intf->handle) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the next image of the ring (never the one the frontend shows now, set_image\'s).\n'
      '    current_frame_index = (current_frame_index + 1) % frame_pool.size();\n'
      '    return &frame_pool[current_frame_index];\n'
      '#endif\n'
      '    // LibRetro synchronization: Use LibRetro\'s wait mechanism instead of fences\n'
      '    if (vulkan_intf && vulkan_intf->wait_sync_index && vulkan_intf->handle) {\n')
patch('src/citra_libretro/libretro_vk.cpp',
      '    persistent_libretro_image.image_view = static_cast<VkImageView>(frame->image_view);\n'
      '    persistent_libretro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;\n'
      '    persistent_libretro_image.create_info =\n'
      '        static_cast<VkImageViewCreateInfo>(output_view_create_info);\n\n'
      '    vulkan_intf->set_image(vulkan_intf->handle, &persistent_libretro_image, 0, nullptr,\n'
      '                           instance.GetGraphicsQueueFamilyIndex());\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: one descriptor per image of the ring (the frontend keeps the pointer).\n'
      '    retro_vulkan_image& libretro_image = emusuni_images[frame - frame_pool.data()];\n'
      '    libretro_image.image_view = static_cast<VkImageView>(frame->image_view);\n'
      '    libretro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;\n'
      '    libretro_image.create_info = static_cast<VkImageViewCreateInfo>(output_view_create_info);\n'
      '    libretro_image.create_info.image = static_cast<VkImage>(frame->image);\n\n'
      '    vulkan_intf->set_image(vulkan_intf->handle, &libretro_image, 0, nullptr,\n'
      '                           instance.GetGraphicsQueueFamilyIndex());\n'
      '#else\n'
      '    persistent_libretro_image.image_view = static_cast<VkImageView>(frame->image_view);\n'
      '    persistent_libretro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;\n'
      '    persistent_libretro_image.create_info =\n'
      '        static_cast<VkImageViewCreateInfo>(output_view_create_info);\n\n'
      '    vulkan_intf->set_image(vulkan_intf->handle, &persistent_libretro_image, 0, nullptr,\n'
      '                           instance.GetGraphicsQueueFamilyIndex());\n'
      '#endif\n')
patch('src/citra_libretro/libretro_vk.h',
      '    retro_vulkan_image persistent_libretro_image{};\n};\n',
      '    retro_vulkan_image persistent_libretro_image{};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the ring of frame images (scripts/build-3ds-core.sh), a descriptor each.\n'
      '    static constexpr u32 EMUSUNI_IMAGES = 3;\n'
      '    std::array<retro_vulkan_image, EMUSUNI_IMAGES> emusuni_images{};\n'
      '#endif\n'
      '};\n')
patch('src/citra_libretro/libretro_vk.h',
      '#include <memory>\n#include <variant>\n',
      '#include <array>\n#include <memory>\n#include <variant>\n')
# The fence wait thread, named for the app's hook.
patch('src/citra_libretro/libretro_vk.cpp',
      'void MasterSemaphoreLibRetro::WaitThread(std::stop_token token) {\n    const vk::Device device{instance.GetDevice()};\n',
      'void MasterSemaphoreLibRetro::WaitThread(std::stop_token token) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("VulkanFence"); // EmuSuNi: the app places it\n'
      '#endif\n'
      '    const vk::Device device{instance.GetDevice()};\n')
# What the device was made with: the features of EMUSUNI_VK_DEVICE_FEATURES only (upstream took the
# physical device's).
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      '#include <vk_mem_alloc.h>\n',
      '#include <vk_mem_alloc.h>\n'
      r'''
#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
// EmuSuNi (scripts/build-3ds-core.sh): the frontend made the device (src/Display.cpp) with the features
// of EMUSUNI_VK_DEVICE_FEATURES (comma-separated VkPhysicalDeviceFeatures names, and the extension
// features' names); none when unset.
bool EmuSuNiFeatureListed(const char* name) {
    const char* list = std::getenv("EMUSUNI_VK_DEVICE_FEATURES");
    if (list == nullptr) {
        return false;
    }
    const size_t length = std::strlen(name);
    for (const char* at = std::strstr(list, name); at != nullptr; at = std::strstr(at + 1, name)) {
        if ((at == list || at[-1] == ',') && (at[length] == '\0' || at[length] == ',')) {
            return true;
        }
    }
    return false;
}

// VkPhysicalDeviceFeatures as Vulkan lays it out: 55 VkBool32 in this order.
constexpr std::array<const char*, 55> EMUSUNI_FEATURE_NAMES = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
    "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
    "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds",
    "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy",
    "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise",
    "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics",
    "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended", "shaderStorageImageExtendedFormats",
    "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
    "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
    "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
    "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
    "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries",
};
static_assert(sizeof(VkPhysicalDeviceFeatures) == EMUSUNI_FEATURE_NAMES.size() * sizeof(VkBool32));

// Of what the GPU has, only what the device was made with.
void EmuSuNiMaskFeatures(vk::PhysicalDeviceFeatures& features) {
    VkBool32* flags = reinterpret_cast<VkBool32*>(&features);
    std::string enabled;
    for (size_t i = 0; i < EMUSUNI_FEATURE_NAMES.size(); i++) {
        if (flags[i] && !EmuSuNiFeatureListed(EMUSUNI_FEATURE_NAMES[i])) {
            flags[i] = VK_FALSE;
        } else if (flags[i]) {
            enabled += enabled.empty() ? EMUSUNI_FEATURE_NAMES[i] : std::string(" ") + EMUSUNI_FEATURE_NAMES[i];
        }
    }
    LOG_INFO(Render_Vulkan, "EmuSuNi: the frontend's device has these features enabled: {}", enabled);
}
} // namespace
#define EMUSUNI_3DS_FEATURE_LISTED(feature) && EmuSuNiFeatureListed(#feature)
#else
#define EMUSUNI_3DS_FEATURE_LISTED(feature)
#endif
''')
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      '    features = feature_chain.get().features;\n    if (available_extensions.empty()) {\n',
      '    features = feature_chain.get().features;\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      '    EmuSuNiMaskFeatures(features); // EmuSuNi: only what the frontend\'s device was made with\n'
      '#endif\n'
      '    if (available_extensions.empty()) {\n')
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      'if (feature_chain.get<structName>().feature) {',
      'if (feature_chain.get<structName>().feature EMUSUNI_3DS_FEATURE_LISTED(feature)) {')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '#include "video_core/renderer_vulkan/vk_shader_util.h"\n',
      '#include "video_core/renderer_vulkan/vk_shader_util.h"\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      'namespace LibRetro { // citra_libretro/libretro_vk.cpp: the frontend\'s queue lock\n'
      'void LockQueue();\n'
      'void UnlockQueue();\n'
      '} // namespace LibRetro\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '    main_present_window.WaitPresent();\n    device.waitIdle();\n\n    device.destroyShaderModule(present_vertex_shader);\n',
      '    main_present_window.WaitPresent();\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      '    LibRetro::LockQueue(); // EmuSuNi: the menus\' queue too\n'
      '    device.waitIdle();\n'
      '    LibRetro::UnlockQueue();\n'
      '#else\n'
      '    device.waitIdle();\n'
      '#endif\n\n'
      '    device.destroyShaderModule(present_vertex_shader);\n')

# --- No SSL: the ssl:C service's random bytes and the AM ticket keys from the PS4's entropy; HTTPS
# requests refused at once.
addfile('src/common/emusuni_random.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): every random byte of the core. LibreSSL isn't built and
// Crypto++'s OS generator wanted arc4random_buf on a FreeBSD target (the PS4's libc hasn't it);
// libc++'s std::random_device opens /dev/urandom, which the PS4 may not have. Here: getentropy, else
// /dev/urandom, else a generator seeded from the clock (said once in the log).
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
#include "common/logging/log.h"

namespace {
std::mutex g_lock;
int g_source = 0; // 0: not tried yet, 1: getentropy, 2: /dev/urandom, 3: the clock
int g_fd = -1;
uint64_t g_state = 0;

uint64_t SplitMix() {
    uint64_t z = (g_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Choose() {
    unsigned char probe[16];
    if (getentropy(probe, sizeof(probe)) == 0) {
        g_source = 1;
        return;
    }
    g_fd = open("/dev/urandom", O_RDONLY);
    if (g_fd >= 0 && read(g_fd, probe, sizeof(probe)) == static_cast<ssize_t>(sizeof(probe))) {
        g_source = 2;
        return;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    g_state = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec) ^
              reinterpret_cast<uintptr_t>(&probe);
    g_source = 3;
    LOG_WARNING(Common, "EmuSuNi: no getentropy and no /dev/urandom: random bytes from a generator seeded by the clock");
}
} // namespace

extern "C" void emusuni_3ds_random_bytes(void* out, size_t size) {
    std::lock_guard guard{g_lock};
    if (g_source == 0) {
        Choose();
    }
    unsigned char* at = static_cast<unsigned char*>(out);
    while (size > 0) {
        size_t chunk = size < 256 ? size : 256;
        bool ok = false;
        if (g_source == 1) {
            ok = getentropy(at, chunk) == 0;
        } else if (g_source == 2) {
            const ssize_t got = read(g_fd, at, chunk);
            ok = got > 0;
            chunk = ok ? static_cast<size_t>(got) : 0;
        }
        if (!ok) {
            for (size_t i = 0; i < chunk; i += 8) {
                const uint64_t word = SplitMix();
                std::memcpy(at + i, &word, chunk - i < 8 ? chunk - i : 8);
            }
        }
        at += chunk;
        size -= chunk;
    }
}

extern "C" unsigned emusuni_3ds_random_seed(void) {
    unsigned seed = 0;
    emusuni_3ds_random_bytes(&seed, sizeof(seed));
    return seed;
}
#endif
''')
patch('src/core/hle/service/ssl/ssl_c.cpp',
      '#include <openssl/rand.h>\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" void emusuni_3ds_random_bytes(void* out, size_t size); // common/emusuni_random.cpp\n'
      '#else\n#include <openssl/rand.h>\n#endif\n')
patch('src/core/hle/service/ssl/ssl_c.cpp',
      '    RAND_bytes(out.data(), static_cast<int>(out.size()));\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_3ds_random_bytes(out.data(), out.size()); // EmuSuNi: no LibreSSL\n'
      '#else\n'
      '    RAND_bytes(out.data(), static_cast<int>(out.size()));\n'
      '#endif\n')
patch('src/core/hle/service/am/am.cpp',
      '#include <openssl/rand.h>\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" void emusuni_3ds_random_bytes(void* out, size_t size); // common/emusuni_random.cpp\n'
      '#else\n#include <openssl/rand.h>\n#endif\n')
patch('src/core/hle/service/am/am.cpp',
      '    RAND_bytes(key.data(), static_cast<int>(key.size()));\n'
      '    RAND_bytes(iv.data(), static_cast<int>(iv.size()));\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_3ds_random_bytes(key.data(), key.size()); // EmuSuNi: no LibreSSL\n'
      '    emusuni_3ds_random_bytes(iv.data(), iv.size());\n'
      '#else\n'
      '    RAND_bytes(key.data(), static_cast<int>(key.size()));\n'
      '    RAND_bytes(iv.data(), static_cast<int>(iv.size()));\n'
      '#endif\n')
patch('src/core/hle/service/http/http_c.cpp',
      'void Context::MakeRequestSSL(httplib::Request& request, const Common::URLInfo& url_info,\n'
      '                             std::vector<Context::RequestHeader>& pending_headers) {\n'
      '    httplib::Error error{-1};\n'
      '    X509* cert = nullptr;\n',
      'void Context::MakeRequestSSL(httplib::Request& request, const Common::URLInfo& url_info,\n'
      '                             std::vector<Context::RequestHeader>& pending_headers) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: built without LibreSSL: no HTTPS (the request fails as if the server were unreachable).\n'
      '    LOG_ERROR(Service_HTTP, "EmuSuNi: HTTPS request to {} refused: no SSL in this build", url_info.host);\n'
      '    state = RequestState::Completed;\n'
      '    FinishTransfer();\n'
      '}\n'
      '#else\n'
      '    httplib::Error error{-1};\n'
      '    X509* cert = nullptr;\n')
patch('src/core/hle/service/http/http_c.cpp',
      '    client->enable_server_certificate_verification(false);\n\n'
      '    client->set_header_writer(\n'
      '        [this, &pending_headers](httplib::Stream& strm, httplib::Headers& httplib_headers) {\n'
      '            return HandleHeaderWrite(pending_headers, strm, httplib_headers);\n'
      '        });\n\n'
      '    if (!client->send(request, response, error)) {\n'
      '        LOG_ERROR(Service_HTTP, "Request failed: {}: {}", error, httplib::to_string(error));\n'
      '        state = RequestState::Completed;\n'
      '    } else {\n'
      '        LOG_DEBUG(Service_HTTP, "Request successful");\n'
      '        state = RequestState::ReceivingBody;\n'
      '    }\n\n'
      '    FinishTransfer();\n'
      '}\n',
      '    client->enable_server_certificate_verification(false);\n\n'
      '    client->set_header_writer(\n'
      '        [this, &pending_headers](httplib::Stream& strm, httplib::Headers& httplib_headers) {\n'
      '            return HandleHeaderWrite(pending_headers, strm, httplib_headers);\n'
      '        });\n\n'
      '    if (!client->send(request, response, error)) {\n'
      '        LOG_ERROR(Service_HTTP, "Request failed: {}: {}", error, httplib::to_string(error));\n'
      '        state = RequestState::Completed;\n'
      '    } else {\n'
      '        LOG_DEBUG(Service_HTTP, "Request successful");\n'
      '        state = RequestState::ReceivingBody;\n'
      '    }\n\n'
      '    FinishTransfer();\n'
      '}\n'
      '#endif // !EMUSUNI_PORT\n')
# std::random_device: libc++'s opens /dev/urandom, which the PS4 may not have (it would throw); the
# three users (the core timing's jitter, the rooms, cpp-httplib's multipart boundary) seed from the
# chain above instead, and the end check wants none left.
patch('src/core/core_timing.cpp',
      '#include "core/core_timing.h"\n',
      '#include "core/core_timing.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" unsigned emusuni_3ds_random_seed(void); // common/emusuni_random.cpp\n'
      '#endif\n')
patch('src/core/core_timing.cpp',
      '    std::mt19937 random_gen(std::random_device{}());\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::mt19937 random_gen(emusuni_3ds_random_seed()); // EmuSuNi: the PS4\'s entropy (no /dev/urandom?)\n'
      '#else\n'
      '    std::mt19937 random_gen(std::random_device{}());\n'
      '#endif\n')
patch('externals/httplib/httplib.h',
      'inline std::string make_multipart_data_boundary() {\n',
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" unsigned emusuni_3ds_random_seed(void); // EmuSuNi: src/common/emusuni_random.cpp\n'
      '#endif\n'
      'inline std::string make_multipart_data_boundary() {\n')
patch('externals/httplib/httplib.h',
      '  // std::random_device might actually be deterministic on some\n'
      '  // platforms, but due to lack of support in the c++ standard library,\n'
      '  // doing better requires either some ugly hacks or breaking portability.\n'
      '  std::random_device seed_gen;\n\n'
      '  // Request 128 bits of entropy for initialization\n'
      '  std::seed_seq seed_sequence{seed_gen(), seed_gen(), seed_gen(), seed_gen()};\n',
      '#ifdef EMUSUNI_PORT\n'
      '  // EmuSuNi (scripts/build-3ds-core.sh): the PS4\'s entropy, not std::random_device (it would open\n'
      '  // /dev/urandom, which the PS4 may not have).\n'
      '  std::seed_seq seed_sequence{emusuni_3ds_random_seed(), emusuni_3ds_random_seed(),\n'
      '                              emusuni_3ds_random_seed(), emusuni_3ds_random_seed()};\n'
      '#else\n'
      '  // std::random_device might actually be deterministic on some\n'
      '  // platforms, but due to lack of support in the c++ standard library,\n'
      '  // doing better requires either some ugly hacks or breaking portability.\n'
      '  std::random_device seed_gen;\n\n'
      '  // Request 128 bits of entropy for initialization\n'
      '  std::seed_seq seed_sequence{seed_gen(), seed_gen(), seed_gen(), seed_gen()};\n'
      '#endif\n')
patch('src/network/room.cpp',
      '#include "network/verify_user.h"\n',
      '#include "network/verify_user.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'extern "C" unsigned emusuni_3ds_random_seed(void); // common/emusuni_random.cpp\n'
      '#define EMUSUNI_3DS_RANDOM_SEED emusuni_3ds_random_seed()\n'
      '#else\n'
      '#define EMUSUNI_3DS_RANDOM_SEED std::random_device()()\n'
      '#endif\n')
patch('src/network/room.cpp',
      '        : NintendoOUI{0x00, 0x1F, 0x32, 0x00, 0x00, 0x00}, random_gen(std::random_device()()) {}\n',
      '        : NintendoOUI{0x00, 0x1F, 0x32, 0x00, 0x00, 0x00}, random_gen(EMUSUNI_3DS_RANDOM_SEED) {}\n')
# Crypto++'s CPU detection includes FreeBSD's ELF auxiliary vector headers (only ARM needs them; the
# PS4 toolchain hasn't sys/elf_common.h and its getauxval clashes with Crypto++'s own stub).
patch('externals/cryptopp/src/core/cpu.cpp',
      '#if defined(__FreeBSD__)\n# include <sys/auxv.h>\n# include <sys/elf_common.h>\n#endif\n',
      '#if defined(__FreeBSD__) && !defined(EMUSUNI_PORT)  // EmuSuNi: not on the PS4 (x86-64 uses CPUID)\n'
      '# include <sys/auxv.h>\n# include <sys/elf_common.h>\n#endif\n')
# Crypto++ probes the CPU when the program starts (a namespace-scope object: DetectX86Features), before any
# game, for every console the app plays - and its CpuId does it under a temporary SIGILL handler (signal,
# sigprocmask, setjmp), which then puts back the handler it found as a plain one: the app's crash log
# installs its SIGILL handler with SA_SIGINFO, and would be called without its siginfo and context after
# that. x86-64 always has CPUID: the instruction alone.
patch('externals/cryptopp/src/core/cpu.cpp',
      '\tvolatile bool result = true;\n\n\tvolatile SigHandler oldHandler = signal(SIGILL, SigIllHandler);\n',
      '#if defined(EMUSUNI_PORT)\n'
      '\t// EmuSuNi (scripts/build-3ds-core.sh): CPUID itself, no SIGILL handler (it would replace the app\'s).\n'
      '\tasm volatile\n\t(\n'
      '\t\t"pushq %%rbx; cpuid; mov %%ebx, %%edi; popq %%rbx"\n'
      '\t\t: "=a" (output[0]), "=D" (output[1]), "=c" (output[2]), "=d" (output[3])\n'
      '\t\t: "a" (func), "c" (subfunc)\n\t\t: "cc"\n\t);\n'
      '\treturn true;\n'
      '#endif\n'
      '\tvolatile bool result = true;\n\n\tvolatile SigHandler oldHandler = signal(SIGILL, SigIllHandler);\n')
# The libretro log: the core formats every message it passes (HLE services log a lot at Debug level, on the
# emulation thread) and the app drops everything below Info: the filter at Info.
patch('src/citra_libretro/citra_libretro.cpp',
      '    CitraLibRetro() : log_filter(Common::Log::Level::Debug) {}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    CitraLibRetro() : log_filter(Common::Log::Level::Info) {} // EmuSuNi: the app drops what is below Info\n'
      '#else\n'
      '    CitraLibRetro() : log_filter(Common::Log::Level::Debug) {}\n'
      '#endif\n')
# Crypto++'s OS generator: arc4random_buf on FreeBSD (the PS4 hasn't it), /dev/urandom elsewhere.
patch('externals/cryptopp/src/random/osrng.cpp',
      '#ifdef __FreeBSD__\n# define DONT_USE_O_NOFOLLOW 1\n# define USE_FREEBSD_ARC4RANDOM 1\n',
      '#if defined(EMUSUNI_PORT)\n'
      '// EmuSuNi (scripts/build-3ds-core.sh): no arc4random on the PS4: src/common/emusuni_random.cpp.\n'
      'extern "C" void emusuni_3ds_random_bytes(void* out, size_t size);\n'
      '#elif defined(__FreeBSD__)\n# define DONT_USE_O_NOFOLLOW 1\n# define USE_FREEBSD_ARC4RANDOM 1\n')
patch('externals/cryptopp/src/random/osrng.cpp',
      '\tm_fd = open("/dev/urandom", flags);\n\tif (m_fd == -1)\n\t\tthrow OS_RNG_Err("open /dev/urandom");\n',
      '#if defined(EMUSUNI_PORT)\n'
      '\tm_fd = -1;  // EmuSuNi: emusuni_3ds_random_bytes\n'
      '#else\n'
      '\tm_fd = open("/dev/urandom", flags);\n\tif (m_fd == -1)\n\t\tthrow OS_RNG_Err("open /dev/urandom");\n'
      '#endif\n')
patch('externals/cryptopp/src/random/osrng.cpp',
      '#if !defined(CRYPTOPP_WIN32_AVAILABLE) && !defined(USE_FREEBSD_ARC4RANDOM)\n\tclose(m_fd);\n#endif\n',
      '#if !defined(CRYPTOPP_WIN32_AVAILABLE) && !defined(USE_FREEBSD_ARC4RANDOM)\n'
      '\tif (m_fd != -1)  // EmuSuNi: none opened\n'
      '\t\tclose(m_fd);\n#endif\n')
patch('externals/cryptopp/src/random/osrng.cpp',
      '# if defined(USE_FREEBSD_ARC4RANDOM)\n\t// Cryptographic quality prng based on ChaCha20,\n',
      '# if defined(EMUSUNI_PORT)\n'
      '\temusuni_3ds_random_bytes(output, size);  // EmuSuNi: the PS4\'s entropy\n'
      '# elif defined(USE_FREEBSD_ARC4RANDOM)\n\t// Cryptographic quality prng based on ChaCha20,\n')

# --- The 3DS's memory off the app's heap (direct memory on the PS4).
addfile('src/common/emusuni_mem.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the 3DS's memory blocks (FCRAM, VRAM, the New 3DS extra RAM)
// in pages of their own, off the app's heap: on the PS4 direct memory (the 4.5 GiB pool), elsewhere
// an anonymous mmap; the app's heap (new[]) if neither can.
#pragma once
#ifdef EMUSUNI_PORT
#include <cstddef>
#include <cstdint>
#include <memory>

namespace EmuSuNi {
// Zeroed, 16 KiB aligned; nullptr when nothing can be had.
void* PagesAlloc(std::size_t size);
void PagesFree(void* p);

struct PagesDeleter {
    void operator()(unsigned char* p) const {
        PagesFree(p);
    }
};
using Pages = std::unique_ptr<unsigned char[], PagesDeleter>;
inline Pages MakePages(std::size_t size) {
    return Pages(static_cast<unsigned char*>(PagesAlloc(size)));
}

// The blocks of direct memory (all of them on the PS4; on the PC tests with the fast memory, its stand-ins'):
// where each is mapped, its size and its offset in direct memory, for the fast memory to map them again
// (common/emusuni_fastmem.cpp). Returns how many it wrote.
struct DirectBlock {
    void* at;
    std::size_t size;
    std::int64_t phys;
};
int PagesDirectBlocks(DirectBlock* out, int max);
} // namespace EmuSuNi
#endif
''')
addfile('src/common/emusuni_mem.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): see emusuni_mem.h. After Flycast's port (scripts/build-dc-core.sh).
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <sys/types.h>
#include <cstdlib>
#include "common/emusuni_dmem.h"
#include "common/emusuni_fastmem.h"
#include "common/emusuni_mem.h"
#include "common/logging/log.h"

namespace EmuSuNi {
namespace {
constexpr size_t PAGE = 16384;
enum class Kind { Direct, Mapped, Heap };
struct Block {
    void* at;
    size_t size;
    off_t phys;
    Kind kind;
};
std::mutex g_lock;
Block g_blocks[16];
int g_count = 0;
} // namespace

void* PagesAlloc(size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (size == 0) {
        return nullptr;
    }
    void* at = nullptr;
    off_t phys = 0;
    Kind kind = Kind::Mapped;
#if defined(__ORBIS__)
    const bool direct = true;
#else
    // (The PC tests: the stand-ins' direct memory (common/emusuni_dmem.h) when the fast memory is on, for it to
    // map these blocks a second time, or with EMUSUNI_3DS_DMEM_SHIM=1 to compare on the same memory; else an mmap.)
    const bool direct = Fastmem::On() || std::getenv("EMUSUNI_3DS_DMEM_SHIM") != nullptr;
#endif
    constexpr int32_t PROT_CPU_RW = 0x03; // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
    constexpr int32_t WB_ONION = 0;       // SCE_KERNEL_WB_ONION: CPU write-back memory
    if (direct && sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), size,
                                                64 * 1024, WB_ONION, &phys) == 0) {
        // A hint well above 0x200000000, where the kernel library puts its own objects.
        void* base = reinterpret_cast<void*>(0x2600000000ull);
        const int32_t ret = sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, PAGE);
        if (ret == 0 && base != nullptr) {
            at = base;
            kind = Kind::Direct;
            std::memset(at, 0, size); // direct memory comes as it was left
        } else {
            LOG_WARNING(Common, "EmuSuNi: mapping {} KiB of direct memory failed ({:#x})", size / 1024,
                        static_cast<unsigned>(ret));
            sceKernelReleaseDirectMemory(phys, size);
        }
    }
    if (at == nullptr) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            at = p; // (mmap gives whole pages, zeroed)
        } else {
            LOG_WARNING(Common, "EmuSuNi: no mapping for {} KiB: the heap", size / 1024);
            at = new (std::nothrow) unsigned char[size]();
            kind = Kind::Heap;
            if (at == nullptr) {
                LOG_ERROR(Common, "EmuSuNi: no memory for {} KiB", size / 1024);
                return nullptr;
            }
        }
    }
    {
        std::lock_guard guard{g_lock};
        if (g_count < static_cast<int>(sizeof(g_blocks) / sizeof(g_blocks[0]))) {
            g_blocks[g_count++] = {at, size, phys, kind};
        } else {
            LOG_ERROR(Common, "EmuSuNi: too many memory blocks ({} kept)", at);
        }
    }
    LOG_INFO(Common, "EmuSuNi: {} KiB of {} at {}", size / 1024,
             kind == Kind::Direct ? "direct memory" : kind == Kind::Mapped ? "mapped memory" : "the heap", at);
    return at;
}

void PagesFree(void* p) {
    if (p == nullptr) {
        return;
    }
    Block block{};
    {
        std::lock_guard guard{g_lock};
        for (int i = 0; i < g_count; i++) {
            if (g_blocks[i].at == p) {
                block = g_blocks[i];
                g_blocks[i] = g_blocks[--g_count];
                break;
            }
        }
    }
    if (block.at == nullptr) {
        LOG_ERROR(Common, "EmuSuNi: freeing {}, not a block of PagesAlloc", p);
        return;
    }
    switch (block.kind) {
    case Kind::Direct:
        sceKernelMunmap(block.at, block.size);
        sceKernelReleaseDirectMemory(block.phys, block.size);
        break;
    case Kind::Mapped:
        munmap(block.at, block.size);
        break;
    case Kind::Heap:
        delete[] static_cast<unsigned char*>(block.at);
        break;
    }
}

int PagesDirectBlocks(DirectBlock* out, int max) {
    std::lock_guard guard{g_lock};
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        if (g_blocks[i].kind == Kind::Direct) {
            out[n++] = {g_blocks[i].at, g_blocks[i].size, static_cast<std::int64_t>(g_blocks[i].phys)};
        }
    }
    return n;
}
} // namespace EmuSuNi
#endif
''')
patch('src/core/memory.cpp',
      '#include "common/settings.h"\n#include "common/swap.h"\n',
      '#include "common/settings.h"\n#include "common/swap.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mem.h"\n#endif\n')
patch('src/core/memory.cpp',
      '    std::unique_ptr<u8[]> fcram = std::make_unique<u8[]>(Memory::FCRAM_N3DS_SIZE);\n'
      '    std::unique_ptr<u8[]> vram = std::make_unique<u8[]>(Memory::VRAM_SIZE);\n'
      '    std::unique_ptr<u8[]> n3ds_extra_ram = std::make_unique<u8[]>(Memory::N3DS_EXTRA_RAM_SIZE);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: pages of their own, off the app\'s heap (direct memory on the PS4; common/emusuni_mem.h).\n'
      '    EmuSuNi::Pages fcram = EmuSuNi::MakePages(Memory::FCRAM_N3DS_SIZE);\n'
      '    EmuSuNi::Pages vram = EmuSuNi::MakePages(Memory::VRAM_SIZE);\n'
      '    EmuSuNi::Pages n3ds_extra_ram = EmuSuNi::MakePages(Memory::N3DS_EXTRA_RAM_SIZE);\n'
      '#else\n'
      '    std::unique_ptr<u8[]> fcram = std::make_unique<u8[]>(Memory::FCRAM_N3DS_SIZE);\n'
      '    std::unique_ptr<u8[]> vram = std::make_unique<u8[]>(Memory::VRAM_SIZE);\n'
      '    std::unique_ptr<u8[]> n3ds_extra_ram = std::make_unique<u8[]>(Memory::N3DS_EXTRA_RAM_SIZE);\n'
      '#endif\n')

# DynCom's translation cache (the interpreter's, 125 MiB): an array in the core's data upstream, which
# stays reserved in the app whether the interpreter runs or not; a block of its own when first used.
patch('src/core/arm/dyncom/arm_dyncom_trans.h',
      '#define TRANS_CACHE_SIZE (64 * 1024 * 2000)\nextern char trans_cache_buf[TRANS_CACHE_SIZE];\n',
      '#define TRANS_CACHE_SIZE (64 * 1024 * 2000)\n'
      '#ifdef EMUSUNI_PORT\n'
      'extern char* trans_cache_buf; // EmuSuNi: taken when the interpreter first translates (arm_dyncom_trans.cpp)\n'
      '#else\n'
      'extern char trans_cache_buf[TRANS_CACHE_SIZE];\n'
      '#endif\n')
patch('src/core/arm/dyncom/arm_dyncom_trans.cpp',
      '#include "core/arm/skyeye_common/vfp/vfp.h"\n\n'
      'char trans_cache_buf[TRANS_CACHE_SIZE];\n'
      'size_t trans_cache_buf_top = 0;\n\n'
      'static void* AllocBuffer(std::size_t size) {\n'
      '    std::size_t start = trans_cache_buf_top;\n',
      '#include "core/arm/skyeye_common/vfp/vfp.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mem.h"\n#endif\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: pages of its own, only when the interpreter runs (the array stayed in the app\'s memory).\n'
      'char* trans_cache_buf = nullptr;\n'
      '#else\n'
      'char trans_cache_buf[TRANS_CACHE_SIZE];\n'
      '#endif\n'
      'size_t trans_cache_buf_top = 0;\n\n'
      'static void* AllocBuffer(std::size_t size) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (trans_cache_buf == nullptr) {\n'
      '        trans_cache_buf = static_cast<char*>(EmuSuNi::PagesAlloc(TRANS_CACHE_SIZE));\n'
      '        ASSERT_MSG(trans_cache_buf != nullptr, "No memory for the interpreter\'s translation cache");\n'
      '    }\n'
      '#endif\n'
      '    std::size_t start = trans_cache_buf_top;\n')

# --- $HOME: unset on the PS4 (the core's files go to the libretro save directory anyway).
patch('src/common/file_util.cpp',
      '            auto pw = getpwuid(getuid());\n'
      '            ASSERT_MSG(pw,\n'
      '                       "$HOME isn’t defined, and the current user can’t be found in /etc/passwd.");\n'
      '            home_path = pw->pw_dir;\n',
      '            auto pw = getpwuid(getuid());\n'
      '#ifdef EMUSUNI_PORT\n'
      '            if (pw == nullptr) {\n'
      '                home_path = "/data/EmuSuNi"; // EmuSuNi: the PS4 has no $HOME and no passwd\n'
      '            } else\n'
      '#else\n'
      '            ASSERT_MSG(pw,\n'
      '                       "$HOME isn’t defined, and the current user can’t be found in /etc/passwd.");\n'
      '#endif\n'
      '            home_path = pw->pw_dir;\n')

# --- No geometry shaders on the PS4 (as Azahar on Android: Instance::UseGeometryShaders() false). Without fragment
# shader barycentrics (RADV has them from GFX10.3 on, the PS4's GPU is GFX7) Azahar gives every lit draw a geometry
# shader, and the first one hung the PS4's GPU: 00.62 closed at the 3D title screens of Super Mario 3D Land and Hey!
# Pikmin (the screen went black; "Run out of pools" in the log: descriptor sets that the stopped GPU never gave back).
# On the PS4 the rings' sizes (VGT_ESGS_RING_SIZE, VGT_GSVS_RING_SIZE) belong to the system's driver
# (libSceGnmDriver's sceGnmSetGsRingSizes, 4-8 MiB, 1 MiB aligned); RADV writes them from its command buffer. On a
# GCN GPU of that age a geometry shader also costs memory traffic (ES -> ring -> GS -> ring -> copy shader). Without
# it the vertex shader passes the normal quaternions straight on, as on Android (some lit edges may shade a little
# differently).
patch('src/video_core/renderer_vulkan/vk_instance.h',
      '    bool UseGeometryShaders() const {\n#ifdef __ANDROID__\n',
      '    bool UseGeometryShaders() const {\n'
      '#if defined(__ANDROID__) || defined(EMUSUNI_PORT) // EmuSuNi: the PS4 too (scripts/build-3ds-core.sh)\n')
# --- Descriptor pools: an allocation that fails for any other reason than a full pool looped forever (the emulation
# thread spinning, the picture frozen); now it throws, and the libretro glue ends the game with the message. The log
# line says which heap grew and to how many pools.
patch('src/video_core/renderer_vulkan/vk_resource_pool.cpp',
      '            current_pool++;\n'
      '            if (current_pool == pools.size()) {\n'
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one!");\n'
      '                AppendDescriptorPool();\n'
      '            }\n'
      '            alloc_info.descriptorPool = *pools[current_pool];\n'
      '        }\n',
      '            current_pool++;\n'
      '            if (current_pool == pools.size()) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one! ({} sets each, {} pools now)",\n'
      '                         descriptor_heap_count, pools.size() + 1);\n'
      '#else\n'
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one!");\n'
      '#endif\n'
      '                AppendDescriptorPool();\n'
      '            }\n'
      '            alloc_info.descriptorPool = *pools[current_pool];\n'
      '        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        else {\n'
      '            throw std::runtime_error(\n'
      '                fmt::format("vkAllocateDescriptorSets failed: {}", vk::to_string(result)));\n'
      '        }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_resource_pool.cpp',
      '#include <cstddef>\n#include <optional>\n',
      '#include <cstddef>\n#include <optional>\n#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include <fmt/format.h>\n#endif\n')

# --- The right eye not drawn (Azahar's "Disable Right Eye Render", off by default upstream because it "can cause major
# graphical issues" in some games, azahar issue #594; "up to 50%" faster in others, release 2120). EmuSuNi never shows
# 3D, yet some games draw both eyes all the time: on the PC, Super Mario 3D Land 1-1 took 15% less of the emulation
# thread's time and 30% less of the Vulkan worker's (Hey! Pikmin: 4% less). The titles Azahar knows it breaks are kept
# drawing by its hack list (Luigi's Mansion 1 and 2, Rayman Origins: HackType::RIGHT_EYE_DISABLE, read per process in
# GSP_GPU::AcquireGpuRight). A game that flickers draws it again with the file /data/EmuSuNi/3ds-right-eye (any
# content; read at each game start), or EMUSUNI_3DS_RIGHT_EYE=1 on the PC.
patch('src/citra_libretro/core_settings.cpp',
      '    Settings::values.use_disk_shader_cache =\n'
      '        LibRetro::FetchVariable(config::graphics::use_disk_shader_cache, config::enabled) ==\n'
      '        config::enabled;\n',
      '    Settings::values.use_disk_shader_cache =\n'
      '        LibRetro::FetchVariable(config::graphics::use_disk_shader_cache, config::enabled) ==\n'
      '        config::enabled;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the right eye is never shown on the PS4 (no 3D): not drawn (scripts/build-3ds-core.sh).\n'
      '    Settings::values.disable_right_eye_render = std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr &&\n'
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n'
      '#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '#include "citra_libretro/core_settings.h"\n',
      '#include "citra_libretro/core_settings.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <cstdlib>\n'
      '#include <unistd.h>\n'
      '#endif\n')


# --- Where a slow frame's time goes (for the log): the 3DS's stalls on the PS4 are 0.4-2.6 s frames every ~10 s in
# the first minutes (00.65 log), and the app's lines can't say why. The core counts, on every thread, the pipelines
# it builds, the shaders it compiles, the textures it decodes and uploads, the bytes it reads from files and the time
# the emulation thread waits for the GPU side; a retro_run of more than 100 ms logs where its time went
# ("EmuSuNi: slow frame ..."), and every 600 frames one line sums them up ("EmuSuNi: 600 frames: ..."). A few
# clock reads per event: nothing measurable on the PC (the A/B in the 3ds-speed v2 research).
addfile('src/common/emusuni_stats.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): what the core spends time on, for the log's slow-frame lines
// (citra_libretro.cpp). Counted on every thread; "emu" is the thread that runs retro_run.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <chrono>
#include <cstdint>

namespace EmuSuNi::Stats {
enum Counter {
    PipelinesEmu,   // pipelines built on the emulation thread (the driver's cache asked first)
    PipelineUsEmu,
    PipelinesOther, // on the pipeline workers
    PipelineUsOther,
    ShadersEmu,     // GLSL or SPIR-V compiled into a shader module
    ShaderUsEmu,
    ShadersOther,
    ShaderUsOther,
    UploadBytes,    // guest textures decoded and uploaded (emulation thread)
    UploadUs,
    ReadBytesEmu,   // bytes read from files (the game, saves, caches)
    ReadUsEmu,
    ReadBytesOther,
    ReadUsOther,
    WaitUs,         // the emulation thread waiting for the GPU worker or a GPU fence
    Count
};
inline std::atomic<std::uint64_t> counters[Count];
inline thread_local bool on_emulation_thread = false;

inline std::uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void Add(Counter c, std::uint64_t v) {
    counters[c].fetch_add(v, std::memory_order_relaxed);
}
// Counts one event and its time into the emulation thread's pair (emu) or the other threads' (other).
struct Event {
    Counter count, us;
    std::uint64_t t0 = NowUs();
    Event(Counter emu_count, Counter other_count)
        : count(on_emulation_thread ? emu_count : other_count),
          us(static_cast<Counter>((on_emulation_thread ? emu_count : other_count) + 1)) {}
    ~Event() {
        Add(count, 1);
        Add(us, NowUs() - t0);
    }
};
struct Timer {
    Counter us;
    std::uint64_t t0 = NowUs();
    explicit Timer(Counter us_) : us(us_) {}
    ~Timer() {
        Add(us, NowUs() - t0);
    }
};
} // namespace EmuSuNi::Stats
#endif
''')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n    MICROPROFILE_SCOPE(Vulkan_Pipeline);\n',
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n    MICROPROFILE_SCOPE(Vulkan_Pipeline);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      'vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {\n',
      'vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::ShadersEmu, EmuSuNi::Stats::ShadersOther};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      'std::vector<u32> CompileGLSL(std::string_view code, vk::ShaderStageFlagBits stage,\n'
      '                             std::string_view premable) {\n',
      'std::vector<u32> CompileGLSL(std::string_view code, vk::ShaderStageFlagBits stage,\n'
      '                             std::string_view premable) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (its time with the module\'s: one shader, counted by CompileSPV)\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::on_emulation_thread ? EmuSuNi::Stats::ShaderUsEmu\n'
      '                                                                                   : EmuSuNi::Stats::ShaderUsOther};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      'void RasterizerCache<T>::UploadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_UploadSurface);\n',
      'void RasterizerCache<T>::UploadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_UploadSurface);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::UploadUs};\n'
      '    EmuSuNi::Stats::Add(EmuSuNi::Stats::UploadBytes, boost::icl::length(interval));\n'
      '#endif\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/common/file_util.cpp',
      '    std::size_t read = FREAD(data, elem_size, length, m_file);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const std::uint64_t emusuni_t0 = EmuSuNi::Stats::NowUs();\n'
      '#endif\n'
      '    std::size_t read = FREAD(data, elem_size, length, m_file);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        const bool emu = EmuSuNi::Stats::on_emulation_thread;\n'
      '        EmuSuNi::Stats::Add(emu ? EmuSuNi::Stats::ReadBytesEmu : EmuSuNi::Stats::ReadBytesOther, read * elem_size);\n'
      '        EmuSuNi::Stats::Add(emu ? EmuSuNi::Stats::ReadUsEmu : EmuSuNi::Stats::ReadUsOther,\n'
      '                            EmuSuNi::Stats::NowUs() - emusuni_t0);\n'
      '    }\n'
      '#endif\n')
patch('src/common/file_util.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);\n    DispatchWork();\n',
      '    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '#endif\n'
      '    DispatchWork();\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      'void Scheduler::Wait(u64 tick) {\n',
      'void Scheduler::Wait(u64 tick) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (Most calls find the tick done already: the stream buffers ask at every draw. Only a real wait is timed:\n'
      '    // two clock reads at every draw were 0.5% of the emulation thread on the PC, where reading the clock costs\n'
      '    // no system call.)\n'
      '    if (master_semaphore->IsFree(tick)) {\n'
      '        return;\n'
      '    }\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '#include "common/microprofile.h"\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '#endif\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: where a slow frame\'s time went (common/emusuni_stats.h), and a summary every 600 frames.\n'
      '    struct FrameStats {\n'
      '        std::uint64_t t0 = EmuSuNi::Stats::NowUs();\n'
      '        std::uint64_t at[EmuSuNi::Stats::Count];\n'
      '        FrameStats() {\n'
      '            EmuSuNi::Stats::on_emulation_thread = true;\n'
      '            for (int i = 0; i < EmuSuNi::Stats::Count; i++)\n'
      '                at[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);\n'
      '        }\n'
      '        static std::string Describe(const std::uint64_t* d, std::uint64_t ms) {\n'
      '            using namespace EmuSuNi::Stats;\n'
      '            return fmt::format("{} ms: waited {} ms for the GPU side; pipelines {} here ({} ms) and {} on the "\n'
      '                               "workers ({} ms); shaders {} here ({} ms) and {} on the workers ({} ms); "\n'
      '                               "textures {} KiB ({} ms); files read {} KiB here ({} ms) and {} KiB elsewhere ({} ms)",\n'
      '                               ms, d[WaitUs] / 1000, d[PipelinesEmu], d[PipelineUsEmu] / 1000, d[PipelinesOther],\n'
      '                               d[PipelineUsOther] / 1000, d[ShadersEmu], d[ShaderUsEmu] / 1000, d[ShadersOther],\n'
      '                               d[ShaderUsOther] / 1000, d[UploadBytes] / 1024, d[UploadUs] / 1000,\n'
      '                               d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024,\n'
      '                               d[ReadUsOther] / 1000);\n'
      '        }\n'
      '        ~FrameStats() {\n'
      '            static std::uint64_t period_at[EmuSuNi::Stats::Count];\n'
      '            static std::uint64_t period_t0 = t0;\n'
      '            static int frames = 0;\n'
      '            const std::uint64_t ms = (EmuSuNi::Stats::NowUs() - t0) / 1000;\n'
      '            std::uint64_t d[EmuSuNi::Stats::Count];\n'
      '            // (100 ms, or EMUSUNI_3DS_SLOW_MS on the PC tests)\n'
      '            static const std::uint64_t slow_ms =\n'
      '                std::getenv("EMUSUNI_3DS_SLOW_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_SLOW_MS"), nullptr, 10) : 100;\n'
      '            if (ms >= slow_ms) {\n'
      '                for (int i = 0; i < EmuSuNi::Stats::Count; i++)\n'
      '                    d[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed) - at[i];\n'
      '                LOG_WARNING(Frontend, "EmuSuNi: slow frame {}", Describe(d, ms));\n'
      '            }\n'
      '            if (++frames == 600) {\n'
      '                for (int i = 0; i < EmuSuNi::Stats::Count; i++) {\n'
      '                    const std::uint64_t now = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);\n'
      '                    d[i] = now - period_at[i];\n'
      '                    period_at[i] = now;\n'
      '                }\n'
      '                LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, (EmuSuNi::Stats::NowUs() - period_t0) / 1000));\n'
      '                period_t0 = EmuSuNi::Stats::NowUs();\n'
      '                frames = 0;\n'
      '            }\n'
      '        }\n'
      '    } emusuni_frame_stats;\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "citra_libretro/citra_libretro.h"\n',
      '#include "citra_libretro/citra_libretro.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include "common/emusuni_stats.h"\n#endif\n')


# --- Shorter shader stalls (3ds-speed v2). On the PS4 the 3DS stops for 0.4-2.6 s every ~10 s in a game's first
# minutes (00.65 log): a draw whose Vulkan pipeline isn't built yet is recorded anyway, the Vulkan worker waits for
# the pipeline (GraphicsPipeline::Build on the pipeline workers), and the emulation thread waits for the worker at the
# end of every frame (RasterizerVulkan::TickFrame -> Scheduler::WaitWorker). Three changes:
#  1. A pipeline still waiting in the workers' queue when a draw needs it is built at once on the emulation thread,
#     instead of after everything queued before it (the start-up precompile of the disk cache queues every pipeline
#     the driver's cache doesn't have: hundreds after an update of the app, whose new Mesa build invalidates it).
#  2. The driver's pipeline cache (saves/3ds/Azahar/shaders/vulkan/pipeline/*.bin) is also saved every 60 s while new
#     pipelines appear, on the pipeline workers, not only when the game ends cleanly: a session closed from the PS4's
#     menu or by a crash kept nothing of what it compiled. (Never during a switch to another title's cache.)
#  3. Azahar's asynchronous shader compilation (by default since 00.68; the file /data/EmuSuNi/3ds-sync-shaders
#     turns it off): a draw whose pipeline isn't ready is skipped (an object missing for a few frames) instead of
#     stopping the game.
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    std::array<Shader*, 3> stages;\n    bool is_pending{};\n};\n',
      '    std::array<Shader*, 3> stages;\n    bool is_pending{};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::atomic<int> emusuni_claimed{0}; // EmuSuNi: 1 once a thread has started building it\n'
      '#endif\n'
      '};\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    if (is_pending) {\n        return wait_built;\n    }\n',
      '    if (is_pending) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: still in the workers\' queue (no worker has started it): built now, here, rather than after\n'
      '        // everything queued before it.\n'
      '        int expected = 0;\n'
      '        if (wait_built && emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '#endif\n'
      '        return wait_built;\n'
      '    }\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    // Fallback to (a)synchronous compilation\n    worker->QueueWork([this] { Build(); });\n',
      '    // Fallback to (a)synchronous compilation\n'
      '#ifdef EMUSUNI_PORT\n'
      '    worker->QueueWork([this] {\n'
      '        int expected = 0;\n'
      '        if (emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '    });\n'
      '#else\n'
      '    worker->QueueWork([this] { Build(); });\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    void SaveDriverPipelineDiskCache();\n',
      '    void SaveDriverPipelineDiskCache();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: EmuSuNiSaveNowAndThen\'s state.\n'
      '    u64 emusuni_last_save_ms = 0;\n'
      '    size_t emusuni_saved_size = 0; // the data size at the last save (a new game starts a new PipelineCache)\n'
      '    // A save on a worker never runs while SwitchCache replaces the driver\'s cache, and one queued before\n'
      '    // the switch is dropped (the generation, changed only by the emulation thread under the mutex).\n'
      '    std::mutex emusuni_save_mutex;\n'
      '    u64 emusuni_save_generation = 0;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '#include <bitset>\n',
      '#include <bitset>\n#ifdef EMUSUNI_PORT\n#include <mutex>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    // Save current driver cache, update program ID and load the new driver cache\n'
      '    SaveDriverPipelineDiskCache();\n'
      '    SetProgramID(title_id);\n'
      '    LoadDriverPipelineDiskCache(stop_loading, nullptr);\n',
      '    // Save current driver cache, update program ID and load the new driver cache\n'
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        // EmuSuNi: not during a save of EmuSuNiSaveNowAndThen\'s, and none queued before it runs after.\n'
      '        std::scoped_lock emusuni_lock{emusuni_save_mutex};\n'
      '        emusuni_save_generation++;\n'
      '        emusuni_saved_size = 0;\n'
      '        SaveDriverPipelineDiskCache();\n'
      '        SetProgramID(title_id);\n'
      '        LoadDriverPipelineDiskCache(stop_loading, nullptr);\n'
      '    }\n'
      '#else\n'
      '    SaveDriverPipelineDiskCache();\n'
      '    SetProgramID(title_id);\n'
      '    LoadDriverPipelineDiskCache(stop_loading, nullptr);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'PipelineCache::~PipelineCache() {\n',
      '#ifdef EMUSUNI_PORT\n'
      'void PipelineCache::EmuSuNiSaveNowAndThen() {\n'
      '    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(\n'
      '                                         std::chrono::steady_clock::now().time_since_epoch())\n'
      '                                         .count());\n'
      '    if (emusuni_last_save_ms == 0) {\n'
      '        emusuni_last_save_ms = now;\n'
      '        return;\n'
      '    }\n'
      '    if (now - emusuni_last_save_ms < 60000) {\n'
      '        return;\n'
      '    }\n'
      '    emusuni_last_save_ms = now;\n'
      '    // Only when the driver compiled something since the last save (its data grew).\n'
      '    if (!driver_pipeline_cache) {\n'
      '        return;\n'
      '    }\n'
      '    size_t size = 0;\n'
      '    if (instance.GetDevice().getPipelineCacheData(*driver_pipeline_cache, &size, nullptr) != vk::Result::eSuccess ||\n'
      '        size <= emusuni_saved_size) {\n'
      '        return;\n'
      '    }\n'
      '    emusuni_saved_size = size;\n'
      '    const u64 generation = emusuni_save_generation;\n'
      '    pipeline_workers.QueueWork([this, generation] {\n'
      '        std::scoped_lock lock{emusuni_save_mutex};\n'
      '        if (generation == emusuni_save_generation) {\n'
      '            SaveDriverPipelineDiskCache();\n'
      '        }\n'
      '    });\n'
      '}\n'
      '#endif\n'
      '\n'
      'PipelineCache::~PipelineCache() {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::TickFrame() {\n    scheduler.WaitWorker();\n    res_cache.TickFrame();\n}\n',
      'void RasterizerVulkan::TickFrame() {\n    scheduler.WaitWorker();\n    res_cache.TickFrame();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n'
      '#endif\n'
      '}\n')
patch('src/citra_libretro/core_settings.cpp',
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n#endif\n',
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for, with the file\n'
      '    // /data/EmuSuNi/3ds-async-shaders (or EMUSUNI_3DS_ASYNC_SHADERS=1 on the PC).\n'
      '    Settings::values.async_shader_compilation = std::getenv("EMUSUNI_3DS_ASYNC_SHADERS") != nullptr ||\n'
      '                                                ::access("/data/EmuSuNi/3ds-async-shaders", F_OK) == 0;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '#include <boost/container/static_vector.hpp>\n',
      '#include <boost/container/static_vector.hpp>\n#ifdef EMUSUNI_PORT\n#include <chrono>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    bool BindPipeline(PipelineInfo& info, bool wait_built = false);\n',
      '    bool BindPipeline(PipelineInfo& info, bool wait_built = false);\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: the driver\'s cache saved again (on the pipeline workers) at most every 60 s while it grows;\n'
      '    /// called once a frame (RasterizerVulkan::TickFrame).\n'
      '    void EmuSuNiSaveNowAndThen();\n'
      '#endif\n')

# --- Fast memory for dynarmic, the ARM11's recompiler (common/emusuni_fastmem.h): a page table's guest 4 GiB as one
# reserved range in which the 16 KiB groups of guest pages backed by the 3DS's memory blocks (direct memory) are mapped
# a second time, so that a recompiled load or store is one instruction; anything else faults once and dynarmic
# recompiles that access with the page table. As the Dreamcast's (scripts/build-dc-core.sh: core/linux/emusuni_vmem.cpp):
# a self-test before the first use (aliasing, a fault with its address, pc and stack, the call it is turned into),
# the page table whenever anything is refused, the marker files for a start that dies while trying it. On by default
# since 00.68 (00.66-00.67: only with the file /data/EmuSuNi/3ds-fastmem); the file /data/EmuSuNi/3ds-fastmem-off
# turns it off (the PC tests: EMUSUNI_3DS_FASTMEM=0, on the stand-ins of common/emusuni_dmem.h).
addfile('src/common/emusuni_dmem.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the PS4's direct memory and address space calls, declared by hand as the
// kernel has them (several OpenOrbis prototypes are wrong). On the PC tests (not __ORBIS__) the same names are
// stand-ins with the PS4's rules (common/emusuni_fastmem.cpp): direct memory is a sparse memfd, mapping it an
// mmap(MAP_SHARED) of it, a reservation an mmap(PROT_NONE); everything in 16 KiB pages, a reservation hint that
// isn't free fails instead of moving. So the fast memory's very code runs there (on unless EMUSUNI_3DS_FASTMEM=0).
#pragma once
#ifdef EMUSUNI_PORT
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace EmuSuNi::Dmem {
constexpr int32_t kProtRead = 0x01;  // SCE_KERNEL_PROT_CPU_READ
constexpr int32_t kProtRW = 0x03;    // SCE_KERNEL_PROT_CPU_READ | SCE_KERNEL_PROT_CPU_WRITE
constexpr int32_t kWbOnion = 0;      // SCE_KERNEL_WB_ONION: CPU write-back memory
constexpr int32_t kMapFixed = 0x10;  // SCE_KERNEL_MAP_FIXED
constexpr std::size_t kPage = 0x4000;
} // namespace EmuSuNi::Dmem

#if defined(__ORBIS__)
extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment,
                                      int32_t memoryType, off_t* physicalOut);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t length);
int32_t sceKernelMapDirectMemory(void** address, size_t length, int32_t protection, int32_t flags,
                                 off_t directMemoryStart, size_t alignment);
int32_t sceKernelReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* address, size_t length);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelInternalMemoryGetAvailableSize(size_t* size); // (as src/Log.cpp's LogMemory)
}
#else
namespace EmuSuNi::Dmem {
int32_t AllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment, int32_t memoryType,
                             off_t* physicalOut);
int32_t ReleaseDirectMemory(off_t start, size_t length);
int32_t MapDirectMemory(void** address, size_t length, int32_t protection, int32_t flags, off_t directMemoryStart,
                        size_t alignment);
int32_t ReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment);
int32_t Munmap(void* address, size_t length);
size_t GetDirectMemorySize();
int InternalMemoryGetAvailableSize(size_t* size);
// EMUSUNI_3DS_SHIM_FAIL=<list> (comma-separated) makes one thing fail, to test the fallbacks: alloc, reserve,
// alias (a second mapping of the same direct memory refused), map=<n> (the n-th mapping), nofault (the self-test's
// unmapped page is mapped), noredirect (the self-test's handler leaves the context as it is), crash (the process
// is killed while it tries: the marker files).
bool Injected(const char* what, long* value = nullptr);
} // namespace EmuSuNi::Dmem
#define sceKernelAllocateDirectMemory EmuSuNi::Dmem::AllocateDirectMemory
#define sceKernelReleaseDirectMemory EmuSuNi::Dmem::ReleaseDirectMemory
#define sceKernelMapDirectMemory EmuSuNi::Dmem::MapDirectMemory
#define sceKernelReserveVirtualRange EmuSuNi::Dmem::ReserveVirtualRange
#define sceKernelMunmap EmuSuNi::Dmem::Munmap
#define sceKernelGetDirectMemorySize EmuSuNi::Dmem::GetDirectMemorySize
#define sceKernelInternalMemoryGetAvailableSize EmuSuNi::Dmem::InternalMemoryGetAvailableSize
#endif
#endif
''')
addfile('src/common/emusuni_fault.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): a fault as a signal handler sees it, for dynarmic's fault handler
// (externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp) and the fast memory's self-test
// (common/emusuni_fastmem.cpp): the same numbers and the same register slots in both.
#pragma once
#ifdef EMUSUNI_PORT
#include <csignal>
#include <cstddef>
#include <cstdint>
#if !defined(__ORBIS__)
#include <ucontext.h>
#endif

namespace EmuSuNi::Fault {
#if defined(__ORBIS__)
// The PS4's FreeBSD kernel, whatever a header says (as the Dreamcast's and the PSP's handlers have them, and
// port/ps4_crashlog.cpp: measured on the console): SIGSEGV 11, SIGBUS 10, SA_SIGINFO 0x40, SA_ONSTACK 1, si_addr 24
// bytes into the siginfo, the registers 0x40 bytes into the context in mcontext order (rip the 21st 8-byte slot, rsp
// the 24th); the struct sigaction handed to the kernel is FreeBSD's (the PSChrome libc overlay's headers come first).
constexpr int kSigSegv = 11, kSigBus = 10, kSaSiginfo = 0x40, kSaOnstack = 0x0001;
constexpr std::size_t kSiAddr = 24;
static_assert(offsetof(struct sigaction, sa_flags) == 8 && sizeof(sigset_t) == 16,
              "struct sigaction isn't FreeBSD's: the PSChrome libc overlay's headers must come first");
inline std::uint64_t& Slot(void* context, int slot) {
    return reinterpret_cast<std::uint64_t*>(static_cast<char*>(context) + 0x40)[slot];
}
inline std::uint64_t& Rip(void* context) {
    return Slot(context, 20);
}
inline std::uint64_t& Rsp(void* context) {
    return Slot(context, 23);
}
#else
constexpr int kSigSegv = SIGSEGV, kSigBus = SIGBUS, kSaSiginfo = SA_SIGINFO, kSaOnstack = SA_ONSTACK;
constexpr std::size_t kSiAddr = offsetof(siginfo_t, si_addr);
inline std::uint64_t& Rip(void* context) {
    return reinterpret_cast<std::uint64_t&>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
}
inline std::uint64_t& Rsp(void* context) {
    return reinterpret_cast<std::uint64_t&>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
}
#endif
inline void* Address(const siginfo_t* info) {
    return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(info) + kSiAddr);
}
} // namespace EmuSuNi::Fault

// dynarmic's fault handler (exception_handler_posix.cpp), for the fast memory (common/emusuni_fastmem.cpp): the
// JITs made while it is wanted install it (the handlers before kept), uninstall puts those back.
extern "C" void emusuni_dynarmic_fault_handler_wanted(bool wanted);
extern "C" bool emusuni_dynarmic_fault_handler_installed(void);
extern "C" void emusuni_dynarmic_fault_handler_uninstall(void);
// common/emusuni_fastmem.cpp: one fault of the JIT's turned into a call of its memory fallback (a count).
extern "C" void emusuni_3ds_fastmem_fault(void);
#endif
''')
addfile('src/common/emusuni_fastmem.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): fast memory for dynarmic, the ARM11's recompiler. A page table's guest 4 GiB
// as one reserved range of the address space, in which every 16 KiB group of guest pages that lies in order in one of
// the 3DS's memory blocks (common/emusuni_mem.cpp: FCRAM, VRAM, the New 3DS RAM, direct memory on the PS4) is mapped
// a second time (sceKernelMapDirectMemory into the range: direct memory aliasing, which the app turns on at its
// start). A recompiled load or store is then one instruction at the range's base plus the guest address, instead of a
// walk of the page table. Anything else (I/O, the pages the GPU cache watches, a group whose pages aren't in order)
// isn't mapped: its first access faults, and dynarmic's fault handler turns the fault into a call of that access's
// fallback and recompiles it with the page table (common/emusuni_fault.h). After the Dreamcast's fast memory
// (scripts/build-dc-core.sh: core/linux/emusuni_vmem.cpp): the same kernel calls, a self-test before the first use
// and the page table whenever anything is refused.
// On by default since 00.68; the file /data/EmuSuNi/3ds-fastmem-off (read at each game's start; the PC tests:
// EMUSUNI_3DS_FASTMEM=0) turns it off. A start that dies while trying it leaves /data/EmuSuNi/3ds-fastmem-trying
// (from the self-test until the first game has run a few seconds with it, or ended): the next start makes
// 3ds-fastmem-crashed of it and doesn't try again until that file is deleted (the log says so).
#pragma once
#ifdef EMUSUNI_PORT
#include <cstdint>

namespace EmuSuNi::Fastmem {
struct Arena;

// A game's: made first and destroyed last with its memory (Memory::MemorySystem). At the start: whether this game
// uses fast memory (the switch, the marker files, the self-test the first time); at the end: dynarmic's fault handler
// taken out again (the handlers before put back) and what the fast memory did, in the log.
struct Game {
    Game();
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
};

// This game uses fast memory.
bool On();

// A page table's range, made with its first JIT: nullptr when the fast memory is off or the system refuses (logged).
Arena* Create();
void Destroy(Arena* arena);
std::uintptr_t Base(const Arena* arena);
// The range follows the page table over pages [first_page, first_page + count) (raw: the table's host pointers).
void Sync(Arena* arena, std::uint8_t* const* raw, std::uint32_t first_page, std::uint32_t count);

// Once a frame (retro_run): the "trying" marker goes once the first game has run a while with the fast memory.
void FrameDone();
} // namespace EmuSuNi::Fastmem
#endif
''')
addfile('src/common/emusuni_fastmem.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh): see emusuni_fastmem.h. After the Dreamcast's fast memory
// (scripts/build-dc-core.sh: core/linux/emusuni_vmem.cpp, itself Resonance's shell/ps4/ps4_vmem.cpp) and the
// 3ds-speed research's prototype (its Sync).
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "common/emusuni_dmem.h"
#include "common/emusuni_fastmem.h"
#include "common/emusuni_fault.h"
#include "common/emusuni_mem.h"
#include "common/logging/log.h"
#if !defined(__ORBIS__)
#include <fcntl.h>
#include <mutex>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

// ---- The PC tests' stand-ins for the PS4's calls (emusuni_dmem.h).
#if !defined(__ORBIS__)
namespace EmuSuNi::Dmem {
namespace {
constexpr int32_t kInval = static_cast<int32_t>(0x80020016), kNoMem = static_cast<int32_t>(0x8002000c),
                  kBusy = static_cast<int32_t>(0x80020010);
constexpr off_t kPool = off_t(16) << 30; // a sparse memfd
std::mutex g_shim;
int g_fd = -1;
off_t g_top = 0;
struct Range {
    off_t start;
    size_t length;
};
std::vector<Range> g_free; // given back: taken again first
struct Mapped {
    std::uintptr_t address;
    size_t length;
    off_t phys;
};
std::vector<Mapped> g_mapped;
long g_map_calls = 0;

bool Aligned(std::uintptr_t v, size_t a) {
    return (v & (a - 1)) == 0;
}

// Drops [address, address + length) from the records of what is mapped.
void Forget(std::uintptr_t address, size_t length) {
    std::vector<Mapped> kept;
    for (const Mapped& m : g_mapped) {
        if (m.address + m.length <= address || m.address >= address + length) {
            kept.push_back(m);
            continue;
        }
        if (m.address < address) {
            kept.push_back({m.address, address - m.address, m.phys});
        }
        if (m.address + m.length > address + length) {
            const size_t cut = address + length - m.address;
            kept.push_back({address + length, m.address + m.length - (address + length), m.phys + off_t(cut)});
        }
    }
    g_mapped.swap(kept);
}

bool AlreadyMapped(off_t phys, size_t length) {
    for (const Mapped& m : g_mapped) {
        if (m.phys < phys + off_t(length) && phys < m.phys + off_t(m.length)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool Injected(const char* what, long* value) {
    const char* list = std::getenv("EMUSUNI_3DS_SHIM_FAIL");
    if (list == nullptr) {
        return false;
    }
    const size_t length = std::strlen(what);
    for (const char* p = std::strstr(list, what); p != nullptr; p = std::strstr(p + 1, what)) {
        if ((p == list || p[-1] == ',') && (p[length] == '\0' || p[length] == ',' || p[length] == '=')) {
            if (value != nullptr) {
                *value = p[length] == '=' ? std::atol(p + length + 1) : 0;
            }
            return true;
        }
    }
    return false;
}

int32_t AllocateDirectMemory(off_t, off_t, size_t length, size_t alignment, int32_t, off_t* physicalOut) {
    std::lock_guard lock{g_shim};
    if (Injected("alloc")) {
        return kNoMem;
    }
    if (length == 0 || !Aligned(length, kPage) || alignment < kPage || (alignment & (alignment - 1)) != 0) {
        return kInval;
    }
    if (g_fd < 0) {
        g_fd = memfd_create("emusuni-3ds-dmem", 0);
        if (g_fd < 0 || ftruncate(g_fd, kPool) != 0) {
            return kNoMem;
        }
    }
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        const off_t start = (it->start + off_t(alignment) - 1) & ~off_t(alignment - 1);
        const off_t end = it->start + off_t(it->length);
        if (start + off_t(length) <= end) {
            const Range before{it->start, size_t(start - it->start)};
            const Range after{start + off_t(length), size_t(end - start - off_t(length))};
            g_free.erase(it);
            if (before.length != 0) {
                g_free.push_back(before);
            }
            if (after.length != 0) {
                g_free.push_back(after);
            }
            *physicalOut = start;
            return 0;
        }
    }
    const off_t start = (g_top + off_t(alignment) - 1) & ~off_t(alignment - 1);
    if (start + off_t(length) > kPool) {
        return kNoMem;
    }
    g_top = start + off_t(length);
    *physicalOut = start;
    return 0;
}

int32_t ReleaseDirectMemory(off_t start, size_t length) {
    std::lock_guard lock{g_shim};
    if (g_fd >= 0) {
        fallocate(g_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, start, off_t(length)); // the pages back
    }
    g_free.push_back({start, length});
    return 0;
}

int32_t MapDirectMemory(void** address, size_t length, int32_t protection, int32_t flags, off_t directMemoryStart,
                        size_t alignment) {
    std::lock_guard lock{g_shim};
    const long call = ++g_map_calls;
    long n = 0;
    if (Injected("map", &n) && n == call) {
        return kNoMem;
    }
    const bool fixed = (flags & kMapFixed) != 0;
    if (g_fd < 0 || length == 0 || !Aligned(length, kPage) || !Aligned(std::uintptr_t(directMemoryStart), kPage) ||
        alignment < kPage || (fixed && !Aligned(reinterpret_cast<std::uintptr_t>(*address), kPage))) {
        return kInval;
    }
    if (alignment > kPage && (!Aligned(std::uintptr_t(directMemoryStart), alignment) || !Aligned(length, alignment) ||
                              (fixed && !Aligned(reinterpret_cast<std::uintptr_t>(*address), alignment)))) {
        return kInval;
    }
    if (Injected("alias") && AlreadyMapped(directMemoryStart, length)) {
        return kBusy; // as the PS4 before aliasing is enabled
    }
    const int prot = (protection & 2) != 0 ? PROT_READ | PROT_WRITE : PROT_READ;
    void* p = mmap(*address, length, prot, MAP_SHARED | (fixed ? MAP_FIXED : 0), g_fd, directMemoryStart);
    if (p == MAP_FAILED) {
        return kNoMem;
    }
    Forget(reinterpret_cast<std::uintptr_t>(p), length);
    g_mapped.push_back({reinterpret_cast<std::uintptr_t>(p), length, directMemoryStart});
    *address = p;
    return 0;
}

int32_t ReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment) {
    std::lock_guard lock{g_shim};
    if (Injected("reserve")) {
        return kNoMem;
    }
    if (length == 0 || !Aligned(length, kPage) || alignment < kPage || (alignment & (alignment - 1)) != 0) {
        return kInval;
    }
    constexpr int kAnon = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void* want = *address;
    if ((flags & kMapFixed) != 0) {
        void* p = mmap(want, length, PROT_NONE, kAnon | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) {
            return kNoMem;
        }
        Forget(reinterpret_cast<std::uintptr_t>(p), length);
        return 0;
    }
    if (want != nullptr) {
        // A hint that isn't free fails, as on the PS4 (the kernel doesn't look on).
        if (!Aligned(reinterpret_cast<std::uintptr_t>(want), alignment)) {
            return kInval;
        }
        void* p = mmap(want, length, PROT_NONE, kAnon | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            return kNoMem;
        }
        if (p != want) {
            munmap(p, length);
            return kNoMem;
        }
        return 0;
    }
    // Anywhere, aligned: more than asked for, the ends given back.
    void* p = mmap(nullptr, length + alignment, PROT_NONE, kAnon, -1, 0);
    if (p == MAP_FAILED) {
        return kNoMem;
    }
    const std::uintptr_t low = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t start = (low + alignment - 1) & ~std::uintptr_t(alignment - 1);
    if (start > low) {
        munmap(p, start - low);
    }
    if (low + length + alignment > start + length) {
        munmap(reinterpret_cast<void*>(start + length), low + length + alignment - (start + length));
    }
    *address = reinterpret_cast<void*>(start);
    return 0;
}

int32_t Munmap(void* address, size_t length) {
    std::lock_guard lock{g_shim};
    Forget(reinterpret_cast<std::uintptr_t>(address), length);
    return munmap(address, length) == 0 ? 0 : kInval;
}

size_t GetDirectMemorySize() {
    return size_t(kPool);
}

int InternalMemoryGetAvailableSize(size_t* size) {
    *size = 0;
    return 0;
}
} // namespace EmuSuNi::Dmem
#endif

// ---- The self-test's probe: a load of the byte at rdi, then ret; and the call a fault in that load is turned into
// (as dynarmic turns a fast-memory fault into a call of the access's fallback): eax = 0x5a, ret to the probe's ret.
extern "C" unsigned emusuni_fastmem_probe(const volatile void* address);
extern "C" const char emusuni_fastmem_probe_resume[];
extern "C" const char emusuni_fastmem_probe_call[];
asm(".pushsection .text\n"
    ".p2align 4\n"
    ".globl emusuni_fastmem_probe\n"
    ".hidden emusuni_fastmem_probe\n"
    ".globl emusuni_fastmem_probe_resume\n"
    ".hidden emusuni_fastmem_probe_resume\n"
    ".globl emusuni_fastmem_probe_call\n"
    ".hidden emusuni_fastmem_probe_call\n"
    "emusuni_fastmem_probe:\n"
    "    movzbl (%rdi), %eax\n"
    "emusuni_fastmem_probe_resume:\n"
    "    ret\n"
    ".p2align 4\n"
    "emusuni_fastmem_probe_call:\n"
    "    movl $0x5a, %eax\n"
    "    ret\n"
    ".popsection\n");

namespace {
std::atomic<std::uint64_t> g_faults{0}; // the JIT's faults turned into calls, since the app started
} // namespace

extern "C" void emusuni_3ds_fastmem_fault(void) {
    g_faults.fetch_add(1, std::memory_order_relaxed); // (in the signal handler: a lock-free atomic)
}

namespace EmuSuNi::Fastmem {
namespace {
constexpr std::size_t kGuestPage = 4096;
constexpr std::size_t kGroup = Dmem::kPage;              // 16 KiB: the PS4's page
constexpr std::size_t kGroupPages = kGroup / kGuestPage; // 4 guest pages
constexpr std::size_t kSpace = std::size_t(1) << 32;     // the guest's address space
constexpr std::size_t kGuard = 64 * 1024;                // an access across its end faults there
constexpr std::size_t kGroups = kSpace / kGroup;
constexpr std::size_t kLarge = 2 * 1024 * 1024;
constexpr int kMaxArenas = 4;
// The kernel library puts its own objects at 0x200000000 and the app's heap is below 0x1800000000
// (port/ps4_runtime.cpp), the 3DS's memory blocks near 0x2600000000, the Dreamcast's range at 0x2800000000 to
// 0x3800000000 (not while a 3DS game runs): above all of them first, 16 GiB apart (a hint that isn't free fails and
// the next is tried), then between them, else anywhere above 0x400000000.
constexpr std::uintptr_t kHints[] = {0x4000000000ull, 0x4400000000ull, 0x4800000000ull, 0x4C00000000ull,
                                     0x5000000000ull, 0x2000000000ull, 0x1C00000000ull, 0x3000000000ull,
                                     0x3400000000ull, 0};
constexpr std::uintptr_t kNotBelow = 0x400000000ull;

std::mutex g_lock;  // Decide, End, Create, Destroy
std::mutex g_sync;  // Sync (the emulation thread's, and any other's)
bool g_on = false;  // this game
int g_tested = -1;  // the self-test: -1 not yet, 0 failed (g_why), 1 passed (for the app's life)
char g_why[240] = "";
bool g_said_off = false;
bool g_trying = false; // the marker file is there
std::uint64_t g_frames = 0;
std::uint64_t g_faults_at_start = 0;
int g_live = 0;
// This game's counts.
std::uint64_t g_syncs = 0, g_maps = 0, g_unmaps = 0, g_refused = 0;
bool g_said_cap = false;

struct ProbeState {
    volatile int armed = 0;
    volatile int faults = 0;
    volatile int signal = 0;
    volatile std::uintptr_t address = 0, rip = 0, rsp = 0;
    sigjmp_buf jump;
};
ProbeState g_probe;
} // namespace

struct Arena {
    std::uint8_t* base = nullptr;
    // Per 16 KiB group of the guest's: 0 nothing mapped there (reserved), else 1 + the direct memory offset mapped
    // there in 16 KiB units (consecutive groups of one run have consecutive values).
    std::vector<std::uint32_t> mapped;
    std::size_t groups_mapped = 0;
};

namespace {
std::size_t InternalFreeKiB() {
    size_t size = 0;
    sceKernelInternalMemoryGetAvailableSize(&size);
    return size / 1024;
}

bool MarkerPath(const char* name, char* path, std::size_t size) {
#if defined(__ORBIS__)
    std::snprintf(path, size, "/data/EmuSuNi/%s", name);
    return true;
#else
    const char* dir = std::getenv("EMUSUNI_3DS_MARKER_DIR");
    if (dir == nullptr) {
        return false;
    }
    std::snprintf(path, size, "%s/%s", dir, name);
    return true;
#endif
}
bool MarkerExists(const char* name) {
    char path[200];
    return MarkerPath(name, path, sizeof(path)) && ::access(path, F_OK) == 0;
}
void MarkerMake(const char* name) {
    char path[200];
    if (MarkerPath(name, path, sizeof(path))) {
        if (std::FILE* file = std::fopen(path, "wb")) {
            std::fclose(file);
        }
    }
}
void MarkerRemove(const char* name) {
    char path[200];
    if (MarkerPath(name, path, sizeof(path))) {
        std::remove(path);
    }
}

bool SwitchOn(char* how, std::size_t size) {
#if defined(__ORBIS__)
    const bool on = ::access("/data/EmuSuNi/3ds-fastmem", F_OK) == 0;
    std::snprintf(how, size, on ? "the file /data/EmuSuNi/3ds-fastmem" : "no file /data/EmuSuNi/3ds-fastmem");
    return on;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM");
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
    std::snprintf(how, size, on ? "EMUSUNI_3DS_FASTMEM=1" : "EMUSUNI_3DS_FASTMEM isn't 1");
    return on;
#endif
}

// One page table's 4 GiB and the guard, reserved (2 MiB aligned) at the first hint that is free.
int32_t ReserveSpace(void** out) {
    int32_t r = -1;
    for (const std::uintptr_t hint : kHints) {
        void* base = reinterpret_cast<void*>(hint);
        r = sceKernelReserveVirtualRange(&base, kSpace + kGuard, 0, kLarge);
        if (r == 0 && reinterpret_cast<std::uintptr_t>(base) >= kNotBelow) {
            *out = base;
            return 0;
        }
        if (r == 0) {
            sceKernelMunmap(base, kSpace + kGuard);
            r = -2;
        }
    }
    return r;
}

// Unmaps a range of a reservation and reserves it again, so nothing else lands there.
bool UnmapToReserved(void* address, std::size_t size) {
    sceKernelMunmap(address, size);
    void* at = address;
    const int32_t r = sceKernelReserveVirtualRange(&at, size, Dmem::kMapFixed, Dmem::kPage);
    if (r != 0 || at != address) {
        LOG_ERROR(Common, "EmuSuNi: fastmem: reserving {} again failed ({:#x})", address, static_cast<unsigned>(r));
        return false;
    }
    return true;
}

void ProbeHandler(int sig, siginfo_t* info, void* context) {
    if (g_probe.armed) {
        const int faults = g_probe.faults + 1;
        g_probe.faults = faults;
        if (faults == 1) {
            g_probe.signal = sig;
            g_probe.address = reinterpret_cast<std::uintptr_t>(Fault::Address(info));
            g_probe.rip = Fault::Rip(context);
            g_probe.rsp = Fault::Rsp(context);
#if !defined(__ORBIS__)
            if (Dmem::Injected("noredirect")) {
                return;
            }
#endif
            // What dynarmic's handler does with a fast-memory fault: a call of the access's fallback, which
            // returns after the load.
            Fault::Rsp(context) -= sizeof(std::uint64_t);
            *reinterpret_cast<std::uint64_t*>(Fault::Rsp(context)) =
                reinterpret_cast<std::uint64_t>(emusuni_fastmem_probe_resume);
            Fault::Rip(context) = reinterpret_cast<std::uint64_t>(emusuni_fastmem_probe_call);
            return;
        }
        siglongjmp(g_probe.jump, 1); // the load ran again: the changes to the context didn't take
    }
    // Not the test's (another thread's fault meanwhile): the default action, as without a handler.
    struct sigaction dfl {};
    sigaction(sig, &dfl, nullptr);
}

__attribute__((noinline)) unsigned RunProbe(const volatile void* address, bool* again, std::uintptr_t* frame) {
    *frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    g_probe.faults = 0;
    g_probe.armed = 1;
    volatile unsigned value = 0;
    if (sigsetjmp(g_probe.jump, 1) == 0) {
        value = emusuni_fastmem_probe(address);
    } else {
        *again = true;
    }
    g_probe.armed = 0;
    return value;
}

const char* SignalName(int sig) {
    return sig == Fault::kSigSegv ? "SIGSEGV" : sig == Fault::kSigBus ? "SIGBUS" : "?";
}

// What the fast memory needs of the system, tried once before its first use: the address space of a page table;
// direct memory mapped a second time at a fixed address of a reserved range (aliasing) is the same memory; a load
// from a reserved page that isn't mapped faults, the handler sees the address, the load's pc and the stack, and the
// fault turned into a call (the stack and pc changed in the context) returns where the load was. "" when all is
// well, else what isn't.
const char* SelfTest(char* detail, std::size_t detail_size) {
    static char why[240];
    void* space = nullptr;
    int32_t r = ReserveSpace(&space);
    if (r != 0) {
        std::snprintf(why, sizeof(why), "no 4 GiB of address space (%x)", static_cast<unsigned>(r));
        return why;
    }
    sceKernelMunmap(space, kSpace + kGuard);

    off_t phys = 0;
    r = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), Dmem::kPage, Dmem::kPage,
                                      Dmem::kWbOnion, &phys);
    if (r != 0) {
        std::snprintf(why, sizeof(why), "no page of direct memory for the self-test (%x)", static_cast<unsigned>(r));
        return why;
    }
    void* range = nullptr;
    r = ReserveSpace(&range); // (the probe's range: a page table's, as the JIT's will be)
    if (r != 0) {
        sceKernelReleaseDirectMemory(phys, Dmem::kPage);
        std::snprintf(why, sizeof(why), "no address space for the self-test (%x)", static_cast<unsigned>(r));
        return why;
    }
    auto* page = static_cast<std::uint8_t*>(range) + 0x10000000; // (a group well inside it)
    void* plain = reinterpret_cast<void*>(0x1f00200000ull);         // a hint: anywhere will do
    const int32_t r_plain = sceKernelMapDirectMemory(&plain, Dmem::kPage, Dmem::kProtRW, 0, phys, Dmem::kPage);
    void* fixed = page;
    const int32_t r_fixed =
        r_plain == 0 ? sceKernelMapDirectMemory(&fixed, Dmem::kPage, Dmem::kProtRW, Dmem::kMapFixed, phys, Dmem::kPage)
                     : -1;
    const char* result = "";
    if (r_plain != 0) {
        std::snprintf(why, sizeof(why), "the self-test's page of direct memory can't be mapped (%x)",
                      static_cast<unsigned>(r_plain));
        result = why;
    } else if (r_fixed != 0 || fixed != page) {
        std::snprintf(why, sizeof(why), "the kernel won't map direct memory a second time in a reserved range (%x)",
                      static_cast<unsigned>(r_fixed));
        result = why;
    } else {
        auto* a = static_cast<volatile std::uint8_t*>(plain);
        auto* b = static_cast<volatile std::uint8_t*>(fixed);
        a[0x123] = 0x3c;
        b[0x456] = 0xc3;
        if (b[0x123] != 0x3c || a[0x456] != 0xc3) {
            result = "two mappings of the same direct memory are not the same memory";
        }
    }
    if (r_fixed == 0 && fixed == page) {
        UnmapToReserved(page, Dmem::kPage);
    }
    if (result[0] == '\0') {
#if !defined(__ORBIS__)
        if (Dmem::Injected("nofault")) {
            void* again = page;
            sceKernelMapDirectMemory(&again, Dmem::kPage, Dmem::kProtRW, Dmem::kMapFixed, phys, Dmem::kPage);
        }
#endif
        struct sigaction act {}, old_segv {}, old_bus {}; // (an empty mask)
        act.sa_sigaction = ProbeHandler;
        act.sa_flags = Fault::kSaSiginfo;
        sigaction(Fault::kSigSegv, &act, &old_segv);
        sigaction(Fault::kSigBus, &act, &old_bus);
        const auto* target = page + 0x123;
        bool again = false;
        std::uintptr_t frame = 0;
        const unsigned value = RunProbe(target, &again, &frame);
        sigaction(Fault::kSigSegv, &old_segv, nullptr);
        sigaction(Fault::kSigBus, &old_bus, nullptr);
        const auto load = reinterpret_cast<std::uintptr_t>(&emusuni_fastmem_probe);
        if (again) {
            result = "the fault's context was changed and the load ran again anyway";
        } else if (g_probe.faults == 0) {
            result = "a load from a reserved page that isn't mapped didn't fault";
        } else if (g_probe.address != reinterpret_cast<std::uintptr_t>(target)) {
            std::snprintf(why, sizeof(why), "fault address %p, the load was from %p",
                          reinterpret_cast<void*>(g_probe.address), static_cast<const void*>(target));
            result = why;
        } else if (g_probe.rip != load) {
            std::snprintf(why, sizeof(why), "fault pc %p, the load is at %p", reinterpret_cast<void*>(g_probe.rip),
                          reinterpret_cast<void*>(load));
            result = why;
        } else if (g_probe.rsp + 4096 < frame || g_probe.rsp > frame + 4096) {
            std::snprintf(why, sizeof(why), "fault rsp %p, the stack is at %p", reinterpret_cast<void*>(g_probe.rsp),
                          reinterpret_cast<void*>(frame));
            result = why;
        } else if (value != 0x5a) {
            std::snprintf(why, sizeof(why), "the call the fault was turned into gave %#x, not 0x5a", value);
            result = why;
        } else {
            std::snprintf(detail, detail_size,
                          "aliasing; a load from an unmapped page faults (%s) with its address, pc and stack, and "
                          "turned into a call it returns",
                          SignalName(g_probe.signal));
        }
    }
    sceKernelMunmap(range, kSpace + kGuard);
    if (r_plain == 0) {
        sceKernelMunmap(plain, Dmem::kPage);
    }
    sceKernelReleaseDirectMemory(phys, Dmem::kPage);
    return result;
}

// This game's start: on or off, and why (the log's one line).
void Decide() {
    std::lock_guard lock{g_lock};
    g_on = false;
    g_frames = 0;
    g_faults_at_start = g_faults.load(std::memory_order_relaxed);
    g_syncs = g_maps = g_unmaps = g_refused = 0;
    g_said_cap = false;
    char how[96];
    if (!SwitchOn(how, sizeof(how))) {
        if (!g_said_off) {
            LOG_INFO(Common, "EmuSuNi: fastmem: off ({}): the page table", how);
            g_said_off = true;
        }
        emusuni_dynarmic_fault_handler_wanted(false);
        return;
    }
    if (g_tested < 0) {
        char path[200] = "";
        if (MarkerExists("3ds-fastmem-crashed")) {
            MarkerPath("3ds-fastmem-crashed", path, sizeof(path));
            std::snprintf(g_why, sizeof(g_why), "an earlier start died while trying it: delete the file %s to try again",
                          path);
            g_tested = 0;
        } else if (MarkerExists("3ds-fastmem-trying")) {
            MarkerMake("3ds-fastmem-crashed");
            MarkerRemove("3ds-fastmem-trying");
            MarkerPath("3ds-fastmem-crashed", path, sizeof(path));
            std::snprintf(g_why, sizeof(g_why),
                          "the start before died while trying it (noted in the file %s: delete it to try again)", path);
            g_tested = 0;
        } else {
            MarkerMake("3ds-fastmem-trying");
            g_trying = true;
#if !defined(__ORBIS__)
            if (Dmem::Injected("crash")) {
                std::raise(SIGKILL); // (the marker files' test)
            }
#endif
            const std::size_t internal = InternalFreeKiB();
            LOG_INFO(Common, "EmuSuNi: fastmem: trying it ({}; kernel memory {} KiB free)", how, internal);
            char detail[200] = "";
            const char* why = SelfTest(detail, sizeof(detail));
            if (why[0] != '\0') {
                std::snprintf(g_why, sizeof(g_why), "%s", why);
                MarkerRemove("3ds-fastmem-trying");
                g_trying = false;
                g_tested = 0;
            } else {
                g_tested = 1;
                LOG_INFO(Common, "EmuSuNi: fastmem: self-test ok: {}; kernel memory {} KiB free (was {})", detail,
                         InternalFreeKiB(), internal);
            }
        }
    }
    if (g_tested == 0) {
        LOG_WARNING(Common, "EmuSuNi: fastmem: off: {} (the page table instead)", g_why);
        emusuni_dynarmic_fault_handler_wanted(false);
        return;
    }
    g_on = true;
    emusuni_dynarmic_fault_handler_wanted(true);
    LOG_INFO(Common, "EmuSuNi: fastmem: on for this game ({})", how);
}

// This game's end (its JITs are gone): dynarmic's handler out again, and the counts.
void End() {
    std::lock_guard lock{g_lock};
    emusuni_dynarmic_fault_handler_uninstall();
    if (g_on) {
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: this game: {} syncs, {} mappings and {} unmappings ({} refused), {} faults "
                 "turned into calls (each recompiled with the page table), {} frames; kernel memory {} KiB free",
                 g_syncs, g_maps, g_unmaps, g_refused, g_faults.load(std::memory_order_relaxed) - g_faults_at_start,
                 g_frames, InternalFreeKiB());
    }
    g_on = false;
}
} // namespace

Game::Game() {
    Decide();
}

Game::~Game() {
    End();
}

bool On() {
    return g_on;
}

Arena* Create() {
    std::lock_guard lock{g_lock};
    if (!g_on) {
        return nullptr;
    }
    if (g_live >= kMaxArenas) {
        if (!g_said_cap) {
            LOG_WARNING(Common, "EmuSuNi: fastmem: {} page tables have a range already: this one's JIT takes the "
                                "page table", g_live);
            g_said_cap = true;
        }
        return nullptr;
    }
    DirectBlock blocks[16];
    if (PagesDirectBlocks(blocks, 16) == 0) {
        if (!g_said_cap) {
            LOG_WARNING(Common, "EmuSuNi: fastmem: the 3DS's memory isn't in direct memory: the page table");
            g_said_cap = true;
        }
        return nullptr;
    }
    void* base = nullptr;
    const int32_t r = ReserveSpace(&base);
    if (r != 0) {
        LOG_WARNING(Common, "EmuSuNi: fastmem: no 4 GiB of address space for a page table ({:#x}): its JIT takes the "
                            "page table", static_cast<unsigned>(r));
        return nullptr;
    }
    auto* arena = new (std::nothrow) Arena;
    if (arena == nullptr) {
        sceKernelMunmap(base, kSpace + kGuard);
        return nullptr;
    }
    arena->base = static_cast<std::uint8_t*>(base);
    arena->mapped.assign(kGroups, 0);
    g_live++;
    LOG_INFO(Common, "EmuSuNi: fastmem: a page table's 4 GiB at {} (kernel memory {} KiB free)", base,
             InternalFreeKiB());
    return arena;
}

void Destroy(Arena* arena) {
    if (arena == nullptr) {
        return;
    }
    std::lock_guard lock{g_lock};
    LOG_INFO(Common, "EmuSuNi: fastmem: a page table's range given back ({} KiB were mapped)",
             arena->groups_mapped * kGroup / 1024);
    sceKernelMunmap(arena->base, kSpace + kGuard);
    delete arena;
    g_live--;
}

std::uintptr_t Base(const Arena* arena) {
    return reinterpret_cast<std::uintptr_t>(arena->base);
}

void Sync(Arena* arena, std::uint8_t* const* raw, std::uint32_t first_page, std::uint32_t count) {
    if (arena == nullptr || count == 0) {
        return;
    }
    std::lock_guard lock{g_sync};
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    g_syncs++;
    const std::size_t g0 = first_page / kGroupPages;
    const std::size_t g1 = std::min<std::size_t>((std::size_t(first_page) + count + kGroupPages - 1) / kGroupPages,
                                                 kGroups);
    // Runs of groups that change the same way: [run_g, run_g + run_n) mapped to direct memory from run_first
    // (consecutive), or made a reservation again (run_first 0).
    std::size_t run_g = 0, run_n = 0;
    std::uint32_t run_first = 0;
    bool run_was_mapped = false;
    auto flush = [&]() {
        if (run_n == 0) {
            return;
        }
        std::uint8_t* address = arena->base + run_g * kGroup;
        const std::size_t length = run_n * kGroup;
        if (run_was_mapped) {
            UnmapToReserved(address, length);
            g_unmaps++;
        }
        bool mapped = false;
        if (run_first != 0) {
            void* at = address;
            const off_t phys = off_t(run_first - 1) * off_t(kGroup);
            const int32_t r = sceKernelMapDirectMemory(&at, length, Dmem::kProtRW, Dmem::kMapFixed, phys, Dmem::kPage);
            g_maps++;
            mapped = r == 0 && at == address;
            if (!mapped) {
                if (r == 0) {
                    UnmapToReserved(at, length);
                }
                if (g_refused++ < 8) {
                    LOG_WARNING(Common, "EmuSuNi: fastmem: mapping {} KiB at guest {:#x} refused ({:#x}): that range "
                                        "takes the page table", length / 1024, run_g * kGroup,
                                static_cast<unsigned>(r));
                }
            }
        }
        for (std::size_t i = 0; i < run_n; i++) {
            std::uint32_t& slot = arena->mapped[run_g + i];
            arena->groups_mapped -= slot != 0 ? 1 : 0;
            slot = mapped ? run_first + std::uint32_t(i) : 0;
            arena->groups_mapped += slot != 0 ? 1 : 0;
        }
        run_n = 0;
    };
    for (std::size_t group = g0; group < g1; group++) {
        // What this group should be: the direct memory behind its four pages when they lie in order in one block,
        // from a whole group's offset in it; else nothing.
        const std::size_t page = group * kGroupPages;
        const std::uint8_t* p = raw[page];
        std::uint32_t want = 0;
        if (p != nullptr) {
            for (int i = 0; i < block_count; i++) {
                const auto* at = static_cast<const std::uint8_t*>(blocks[i].at);
                if (p < at || p >= at + blocks[i].size) {
                    continue;
                }
                const std::size_t offset = std::size_t(p - at);
                if (offset % kGroup == 0 && offset + kGroup <= blocks[i].size && raw[page + 1] == p + kGuestPage &&
                    raw[page + 2] == p + 2 * kGuestPage && raw[page + 3] == p + 3 * kGuestPage) {
                    want = std::uint32_t((std::uint64_t(blocks[i].phys) + offset) / kGroup) + 1;
                }
                break;
            }
        }
        const std::uint32_t have = arena->mapped[group];
        if (want == have) {
            flush();
            continue;
        }
        const bool extends = run_n > 0 && ((want == 0 && run_first == 0) ||
                                           (want != 0 && run_first != 0 && want == run_first + run_n));
        if (!extends) {
            flush();
            run_g = group;
            run_first = want;
            run_was_mapped = false;
        }
        run_was_mapped |= have != 0;
        run_n++;
    }
    flush();
}

void FrameDone() {
    if (!g_on) {
        return;
    }
    g_frames++;
    if (!g_trying) {
        return;
    }
    const std::uint64_t faults = g_faults.load(std::memory_order_relaxed) - g_faults_at_start;
    if ((g_frames >= 600 && faults >= 64) || g_frames >= 3600) {
        MarkerRemove("3ds-fastmem-trying");
        g_trying = false;
        LOG_INFO(Common, "EmuSuNi: fastmem: {} frames and {} faults turned into calls: it works here (the start "
                         "marker is gone)", g_frames, faults);
    }
}
} // namespace EmuSuNi::Fastmem
#endif
''')
# dynarmic's POSIX fault handler on the PS4 too (upstream builds the generic one, which has no fast memory, for
# CMake's "Generic" system), changed for EmuSuNi: installed only while a game uses fast memory, taken out again at its
# end, no alternate signal stack, SIGBUS too, the PS4's numbers and register slots (common/emusuni_fault.h). Without
# fast memory nothing is installed, as before.
patch('externals/dynarmic/src/dynarmic/CMakeLists.txt',
      'elseif (UNIX)\n    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")\n',
      'elseif (UNIX OR EMUSUNI_PORT)  # EmuSuNi: the PS4 too (the fast memory, common/emusuni_fastmem.h)\n'
      '    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      '#include "dynarmic/backend/exception_handler.h"\n',
      '#include "dynarmic/backend/exception_handler.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#    include <algorithm>\n'
      '#    include "../../../../../src/common/emusuni_fault.h"\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      '#    ifndef __OpenBSD__\n#        include <ucontext.h>\n#    endif\n',
      '#    if !defined(__OpenBSD__) && !(defined(EMUSUNI_PORT) && defined(__ORBIS__))\n'
      '#        include <ucontext.h>\n'
      '#    endif\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      'namespace Dynarmic::Backend {\n\nnamespace {\n',
      r'''namespace Dynarmic::Backend {

namespace {

#if defined(EMUSUNI_PORT) && defined(MCL_ARCHITECTURE_X86_64)
// EmuSuNi (scripts/build-3ds-core.sh): the fault handler of the fast memory (common/emusuni_fastmem.h). Installed
// when a JIT registers while the core wants it (emusuni_dynarmic_fault_handler_wanted: a game with fast memory), taken
// out again at the game's end (emusuni_dynarmic_fault_handler_uninstall), the handlers before put back, as the other
// cores' handlers do (the Dreamcast's, the PSP's): the chain is the running core's handler, then the app's crash log.
// On the thread's stack for signals (SA_ONSTACK, 01.09: the app gives every thread one, port/ps4_runtime.cpp; with
// none, the 01.08 handler left a stack's overflow without crash.log), SIGBUS as well as SIGSEGV, the PS4's numbers
// and register slots (common/emusuni_fault.h). Without fast memory nothing is installed (as the generic handler
// before).
struct CodeBlockInfo {
    u64 code_begin, code_end;
    std::function<FakeCall(u64)> cb;
};

class SigHandler {
public:
    void AddCodeBlock(CodeBlockInfo info);
    void RemoveCodeBlock(u64 host_pc);
    bool SupportsFastmem() const { return installed; }
    void Install();
    void Uninstall();

    auto FindCodeBlockInfo(u64 host_pc) {
        return std::find_if(code_block_infos.begin(), code_block_infos.end(), [&](const auto& x) { return x.code_begin <= host_pc && x.code_end > host_pc; });
    }

    bool installed = false;
    std::vector<CodeBlockInfo> code_block_infos;
    std::mutex code_block_infos_mutex;
    struct sigaction old_sa_segv {};
    struct sigaction old_sa_bus {};

    static void SigAction(int sig, siginfo_t* info, void* raw_context);
};

std::mutex handler_lock;
std::optional<SigHandler> sig_handler;
bool handler_wanted = false;

void RegisterHandler() {
    std::lock_guard<std::mutex> guard(handler_lock);
    if (!sig_handler) {
        sig_handler.emplace();
    }
    if (handler_wanted) {
        sig_handler->Install();
    }
}

void SigHandler::Install() {
    if (installed) {
        return;
    }
    struct sigaction sa {};  // (an empty mask)
    sa.sa_sigaction = &SigHandler::SigAction;
    sa.sa_flags = ::EmuSuNi::Fault::kSaSiginfo | ::EmuSuNi::Fault::kSaOnstack; // (01.09: the signal stack)
    if (sigaction(::EmuSuNi::Fault::kSigSegv, &sa, &old_sa_segv) != 0) {
        fmt::print(stderr, "dynarmic: EmuSuNi: could not set the SIGSEGV handler\n");
        return;
    }
    if (sigaction(::EmuSuNi::Fault::kSigBus, &sa, &old_sa_bus) != 0) {
        sigaction(::EmuSuNi::Fault::kSigSegv, &old_sa_segv, nullptr);
        fmt::print(stderr, "dynarmic: EmuSuNi: could not set the SIGBUS handler\n");
        return;
    }
    installed = true;
}

void SigHandler::Uninstall() {
    if (!installed) {
        return;
    }
    sigaction(::EmuSuNi::Fault::kSigSegv, &old_sa_segv, nullptr);
    sigaction(::EmuSuNi::Fault::kSigBus, &old_sa_bus, nullptr);
    installed = false;
}

void SigHandler::AddCodeBlock(CodeBlockInfo cbi) {
    std::lock_guard<std::mutex> guard(code_block_infos_mutex);
    if (auto iter = FindCodeBlockInfo(cbi.code_begin); iter != code_block_infos.end()) {
        code_block_infos.erase(iter);
    }
    code_block_infos.push_back(cbi);
}

void SigHandler::RemoveCodeBlock(u64 host_pc) {
    std::lock_guard<std::mutex> guard(code_block_infos_mutex);
    const auto iter = FindCodeBlockInfo(host_pc);
    if (iter == code_block_infos.end()) {
        return;
    }
    code_block_infos.erase(iter);
}

void SigHandler::SigAction(int sig, siginfo_t* info, void* raw_context) {
    {
        std::lock_guard<std::mutex> guard(sig_handler->code_block_infos_mutex);
        const u64 rip = ::EmuSuNi::Fault::Rip(raw_context);
        const auto iter = sig_handler->FindCodeBlockInfo(rip);
        if (iter != sig_handler->code_block_infos.end()) {
            // A fast-memory access that faulted: a call of its fallback (through the page table), which returns
            // after the access; dynarmic recompiles that access with the page table.
            const FakeCall fc = iter->cb(rip);
            u64& rsp = ::EmuSuNi::Fault::Rsp(raw_context);
            rsp -= sizeof(u64);
            *mcl::bit_cast<u64*>(rsp) = fc.ret_rip;
            ::EmuSuNi::Fault::Rip(raw_context) = fc.call_rip;
            emusuni_3ds_fastmem_fault();
            return;
        }
    }
    // Not the JIT's: the handler before (the app's crash log), as the kernel would have called it.
    struct sigaction* retry_sa = sig == ::EmuSuNi::Fault::kSigSegv ? &sig_handler->old_sa_segv : &sig_handler->old_sa_bus;
    if (retry_sa->sa_flags & ::EmuSuNi::Fault::kSaSiginfo) {
        retry_sa->sa_sigaction(sig, info, raw_context);
        return;
    }
    if (retry_sa->sa_handler == SIG_IGN) {
        return;
    }
    if (retry_sa->sa_handler == SIG_DFL) {
        struct sigaction dfl {};  // the default action: the fault again, without a handler
        sigaction(sig, &dfl, nullptr);
        return;
    }
    retry_sa->sa_handler(sig);
}
#else
''')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      '}  // anonymous namespace\n',
      '#endif  // EMUSUNI_PORT\n}  // anonymous namespace\n')
patch('externals/dynarmic/src/dynarmic/backend/exception_handler_posix.cpp',
      'void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {\n    impl->SetCallback(cb);\n}\n',
      r'''void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    impl->SetCallback(cb);
}

#if defined(EMUSUNI_PORT) && defined(MCL_ARCHITECTURE_X86_64)
// EmuSuNi: the fast memory's switches of the handler (common/emusuni_fault.h).
extern "C" void emusuni_dynarmic_fault_handler_wanted(bool wanted) {
    std::lock_guard<std::mutex> guard(handler_lock);
    handler_wanted = wanted;
}

extern "C" bool emusuni_dynarmic_fault_handler_installed(void) {
    std::lock_guard<std::mutex> guard(handler_lock);
    return sig_handler && sig_handler->installed;
}

extern "C" void emusuni_dynarmic_fault_handler_uninstall(void) {
    std::lock_guard<std::mutex> guard(handler_lock);
    handler_wanted = false;
    if (sig_handler) {
        sig_handler->Uninstall();
    }
}
#endif
''')

# The page table keeps its range in step: every change of its pointers (the mappings, the pages the GPU cache marks
# and unmarks, the watchpoints, a save state's load, Clear) syncs the pages it changed, in runs.
patch('src/core/memory.h',
      '#include "common/swap.h"\n',
      '#include "common/swap.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <utility>\n'
      '#include <vector>\n'
      'namespace EmuSuNi::Fastmem {\n'
      'struct Arena;\n'
      '}\n'
      '#endif\n')
patch('src/core/memory.h',
      '    std::unordered_map<VAddr, WatchpointPageInfo> watchpoint_pages_map{};\n\n    void Clear();\n',
      '    std::unordered_map<VAddr, WatchpointPageInfo> watchpoint_pages_map{};\n\n    void Clear();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the fast memory\'s range of this address space (common/emusuni_fastmem.h), made with its first\n'
      '    // JIT, kept in step with the pointers.\n'
      '    EmuSuNi::Fastmem::Arena* fastmem = nullptr;\n'
      '    void FastmemSync(u32 first_page, u32 count);\n'
      '    // Pages of several tables, synced in runs (sorts `pages`).\n'
      '    static void FastmemSyncPages(std::vector<std::pair<PageTable*, u32>>& pages);\n'
      '    PageTable() = default;\n'
      '    PageTable(const PageTable&) = delete;\n'
      '    PageTable& operator=(const PageTable&) = delete;\n'
      '    ~PageTable();\n'
      '#endif\n')
patch('src/core/memory.h',
      '        for (std::size_t i = 0; i < PAGE_TABLE_NUM_ENTRIES; i++) {\n'
      '            pointers.raw[i] = pointers.refs[i].GetPtr();\n'
      '        }\n    }\n',
      '        for (std::size_t i = 0; i < PAGE_TABLE_NUM_ENTRIES; i++) {\n'
      '            pointers.raw[i] = pointers.refs[i].GetPtr();\n'
      '        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        FastmemSync(0, PAGE_TABLE_NUM_ENTRIES); // (a save state\'s load)\n'
      '#endif\n'
      '    }\n')
patch('src/core/memory.cpp',
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mem.h"\n#endif\n',
      '#ifdef EMUSUNI_PORT\n#include <algorithm>\n#include <functional>\n#include "common/emusuni_fastmem.h"\n'
      '#include "common/emusuni_mem.h"\n#endif\n')
patch('src/core/memory.cpp',
      'void PageTable::Clear() {\n    pointers.raw.fill(nullptr);\n    pointers.refs.fill(MemoryRef());\n'
      '    attributes.fill(PageType::Unmapped);\n}\n',
      r'''void PageTable::Clear() {
    pointers.raw.fill(nullptr);
    pointers.refs.fill(MemoryRef());
    attributes.fill(PageType::Unmapped);
#ifdef EMUSUNI_PORT
    FastmemSync(0, PAGE_TABLE_NUM_ENTRIES);
#endif
}
#ifdef EMUSUNI_PORT
void PageTable::FastmemSync(u32 first_page, u32 count) {
    if (fastmem != nullptr) {
        EmuSuNi::Fastmem::Sync(fastmem, pointers.raw.data(), first_page, count);
    }
}

void PageTable::FastmemSyncPages(std::vector<std::pair<PageTable*, u32>>& pages) {
    std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? std::less<PageTable*>{}(a.first, b.first) : a.second < b.second;
    });
    for (std::size_t i = 0; i < pages.size();) {
        PageTable* const table = pages[i].first;
        const u32 first = pages[i].second;
        u32 last = first;
        std::size_t j = i + 1;
        while (j < pages.size() && pages[j].first == table && pages[j].second <= last + 1) {
            last = std::max(last, pages[j].second);
            j++;
        }
        table->FastmemSync(first, last - first + 1);
        i = j;
    }
}

PageTable::~PageTable() {
    EmuSuNi::Fastmem::Destroy(fastmem);
}
#endif
''')
patch('src/core/memory.cpp',
      '    // EmuSuNi: pages of their own, off the app\'s heap (direct memory on the PS4; common/emusuni_mem.h).\n'
      '    EmuSuNi::Pages fcram = EmuSuNi::MakePages(Memory::FCRAM_N3DS_SIZE);\n',
      '    // EmuSuNi: made first and destroyed last: whether this game uses fast memory, decided before its memory is\n'
      '    // allocated (common/emusuni_fastmem.h).\n'
      '    EmuSuNi::Fastmem::Game emusuni_fastmem_game;\n'
      '    // EmuSuNi: pages of their own, off the app\'s heap (direct memory on the PS4; common/emusuni_mem.h).\n'
      '    EmuSuNi::Pages fcram = EmuSuNi::MakePages(Memory::FCRAM_N3DS_SIZE);\n')
patch('src/core/memory.cpp',
      '    u32 end = base + size;\n    while (base != end) {\n',
      '    u32 end = base + size;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const u32 emusuni_first = base;\n'
      '#endif\n'
      '    while (base != end) {\n')
patch('src/core/memory.cpp',
      '        base += 1;\n        if (memory != nullptr && memory.GetSize() > CITRA_PAGE_SIZE)\n'
      '            memory += CITRA_PAGE_SIZE;\n    }\n}\n',
      '        base += 1;\n        if (memory != nullptr && memory.GetSize() > CITRA_PAGE_SIZE)\n'
      '            memory += CITRA_PAGE_SIZE;\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    page_table.FastmemSync(emusuni_first, size);\n'
      '#endif\n'
      '}\n')
patch('src/core/memory.cpp',
      'void MemorySystem::RasterizerMarkRegionCached(PAddr start, u32 size, bool cached) {\n'
      '    if (start == 0) {\n        return;\n    }\n',
      'void MemorySystem::RasterizerMarkRegionCached(PAddr start, u32 size, bool cached) {\n'
      '    if (start == 0) {\n        return;\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the pages whose pointers this changes, for the fast memory to follow in runs at the end.\n'
      '    std::vector<std::pair<PageTable*, u32>> emusuni_changed;\n'
      '#endif\n')
patch('src/core/memory.cpp',
      '                        page_table->pointers[vaddr >> CITRA_PAGE_BITS] = nullptr;\n                        break;\n',
      '                        page_table->pointers[vaddr >> CITRA_PAGE_BITS] = nullptr;\n'
      '#ifdef EMUSUNI_PORT\n'
      '                        if (page_table->fastmem != nullptr) {\n'
      '                            emusuni_changed.emplace_back(page_table.get(), vaddr >> CITRA_PAGE_BITS);\n'
      '                        }\n'
      '#endif\n'
      '                        break;\n')
patch('src/core/memory.cpp',
      '                            page_table->pointers[vaddr >> CITRA_PAGE_BITS] =\n'
      '                                GetPointerForRasterizerCache(vaddr & ~CITRA_PAGE_MASK);\n',
      '                            page_table->pointers[vaddr >> CITRA_PAGE_BITS] =\n'
      '                                GetPointerForRasterizerCache(vaddr & ~CITRA_PAGE_MASK);\n'
      '#ifdef EMUSUNI_PORT\n'
      '                            if (page_table->fastmem != nullptr) {\n'
      '                                emusuni_changed.emplace_back(page_table.get(), vaddr >> CITRA_PAGE_BITS);\n'
      '                            }\n'
      '#endif\n')
patch('src/core/memory.cpp',
      '    }\n}\n\nu8 MemorySystem::Read8(const VAddr addr) {\n',
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    PageTable::FastmemSyncPages(emusuni_changed);\n'
      '#endif\n'
      '}\n\nu8 MemorySystem::Read8(const VAddr addr) {\n')
patch('src/core/memory.cpp',
      '                type = PageType::MemoryWatchpoint;\n                page_table.pointers[page_index] = nullptr;\n'
      '                break;\n',
      '                type = PageType::MemoryWatchpoint;\n                page_table.pointers[page_index] = nullptr;\n'
      '#ifdef EMUSUNI_PORT\n'
      '                page_table.FastmemSync(page_index, 1);\n'
      '#endif\n'
      '                break;\n')
patch('src/core/memory.cpp',
      '                    type = PageType::Memory;\n                    page_table.pointers[page_index] = it->second.memory;\n'
      '                    break;\n',
      '                    type = PageType::Memory;\n                    page_table.pointers[page_index] = it->second.memory;\n'
      '#ifdef EMUSUNI_PORT\n'
      '                    page_table.FastmemSync(page_index, 1);\n'
      '#endif\n'
      '                    break;\n')
# Each JIT of a page table: the range's base as dynarmic's fast memory pointer (its loads and stores at base + the
# guest address; the page table stays for the accesses that fault).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/settings.h"\n',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_fastmem.h"\n#include "common/settings.h"\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n#endif\n',
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n'
      '    // EmuSuNi: fast memory (common/emusuni_fastmem.h): the page table\'s range, made with its first JIT; the\n'
      '    // JIT installs dynarmic\'s fault handler as it registers.\n'
      '    if (current_page_table && EmuSuNi::Fastmem::On()) {\n'
      '        if (current_page_table->fastmem == nullptr) {\n'
      '            current_page_table->fastmem = EmuSuNi::Fastmem::Create();\n'
      '            current_page_table->FastmemSync(0, Memory::PAGE_TABLE_NUM_ENTRIES);\n'
      '        }\n'
      '        if (current_page_table->fastmem != nullptr) {\n'
      '            config.fastmem_pointer = EmuSuNi::Fastmem::Base(current_page_table->fastmem);\n'
      '        }\n'
      '    }\n'
      '#endif\n')
# Once a frame: the fast memory's start marker goes once the first game has run a while with it.
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "citra_libretro/citra_libretro.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include "common/emusuni_stats.h"\n#endif\n',
      '#include "citra_libretro/citra_libretro.h"\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include "common/emusuni_fastmem.h"\n'
      '#include "common/emusuni_stats.h"\n#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    // EmuSuNi: where a slow frame\'s time went (common/emusuni_stats.h), and a summary every 600 frames.\n',
      '    EmuSuNi::Fastmem::FrameDone(); // EmuSuNi: (common/emusuni_fastmem.h)\n'
      '    // EmuSuNi: where a slow frame\'s time went (common/emusuni_stats.h), and a summary every 600 frames.\n')

# --- With the fast memory, a process's code, data and stack at the guest address's 16 KiB phase in FCRAM: the
# FCRAM behind them (what the loader maps when the process starts) is taken at the same 16 KiB phase as the guest
# address it is mapped at, so that those ranges are mapped by whole 16 KiB pages. Upstream takes the top of the
# highest free block whatever its phase: then a 16 KiB group of guest pages straddles two 16 KiB pages of FCRAM and
# the fast memory can't map it (Super Mario 3D Land: its code, data and stack). The space the phase leaves above a
# block (less than 16 KiB) is charged to the process, used and freed with it: a game that asks for every free byte of
# its region (Super Mario 3D Land's linear heap) must not find the region fragmented, nor its commit limit off. Heap
# commits (svcControlMemory) keep upstream's allocator: they can be freed and committed again, and a charged gap would
# leak each time. Without the fast memory, upstream's allocation as before.
patch('src/core/hle/kernel/memory.h',
      '    IntervalSet HeapAllocate(u32 size);\n',
      '    IntervalSet HeapAllocate(u32 size);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: one interval whose start is `phase` modulo `align` (a power of two), the space left above it\n'
      '    /// in its free block (less than `align`) taken too and returned in `gap`; else as HeapAllocate.\n'
      '    IntervalSet HeapAllocateAligned(u32 size, u32 phase, u32 align, IntervalSet& gap);\n'
      '#endif\n')
patch('src/core/hle/kernel/memory.cpp',
      'bool MemoryRegionInfo::LinearAllocate(u32 offset, u32 size) {\n',
      r'''#ifdef EMUSUNI_PORT
MemoryRegionInfo::IntervalSet MemoryRegionInfo::HeapAllocateAligned(u32 size, u32 phase, u32 align,
                                                                      IntervalSet& gap) {
    ASSERT(!is_locked);
    // From the higher addresses, as HeapAllocate: the highest start in a free block with the right phase.
    for (auto iter = free_blocks.rbegin(); iter != free_blocks.rend(); ++iter) {
        const u32 lower = iter->lower();
        const u32 upper = iter->upper();
        if (upper - lower < size) {
            continue;
        }
        u32 start = ((upper - size) & ~(align - 1)) + phase;
        if (start > upper - size) {
            if (start < align) {
                continue;
            }
            start -= align;
        }
        if (start < lower) {
            continue;
        }
        IntervalSet result;
        result += Interval(start, start + size);
        free_blocks -= Interval(start, upper);
        used += upper - start;
        if (start + size < upper) {
            gap += Interval(start + size, upper);
        }
        return result;
    }
    return HeapAllocate(size);
}
#endif

bool MemoryRegionInfo::LinearAllocate(u32 offset, u32 size) {
''')
patch('src/core/hle/kernel/process.h',
      '    MemoryRegionInfo::IntervalSet holding_memory;\n',
      '    MemoryRegionInfo::IntervalSet holding_memory;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: while the loader maps the code, data and stack (Run), with the fast memory: FCRAM at the guest\n'
      '    /// address\'s 16 KiB phase.\n'
      '    bool emusuni_phase_aligned = false;\n'
      '#endif\n')
patch('src/core/hle/kernel/process.cpp',
      '#include "common/serialization/boost_vector.hpp"\n',
      '#include "common/serialization/boost_vector.hpp"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_fastmem.h"\n#endif\n')
patch('src/core/hle/kernel/process.cpp',
      '    auto allocated_fcram = memory_region->HeapAllocate(size);\n',
      r'''#ifdef EMUSUNI_PORT
    // EmuSuNi: the loader's mappings at the guest address's 16 KiB phase (the fast memory maps 16 KiB pages on the
    // PS4); the space that leaves above them is the process's, freed with it.
    MemoryRegionInfo::IntervalSet emusuni_gap;
    auto allocated_fcram = emusuni_phase_aligned
                               ? memory_region->HeapAllocateAligned(size, target & 0x3FFF, 0x4000, emusuni_gap)
                               : memory_region->HeapAllocate(size);
    // (The gap counts as the process's memory everywhere the kernel counts it: its intervals, its use and its commit
    // limit, all given back by the process's cleanup, which frees holding_memory interval by interval.)
    holding_memory += emusuni_gap;
    for (const auto& gap : emusuni_gap) {
        memory_used += gap.upper() - gap.lower();
        resource_limit->Reserve(ResourceLimitType::Commit, gap.upper() - gap.lower());
    }
#else
    auto allocated_fcram = memory_region->HeapAllocate(size);
#endif
''')
patch('src/core/hle/kernel/process.cpp',
      '    // Map CodeSet segments\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_phase_aligned = EmuSuNi::Fastmem::On();\n'
      '#endif\n'
      '    // Map CodeSet segments\n')
patch('src/core/hle/kernel/process.cpp',
      '    HeapAllocate(std::addressof(out_addr), Memory::HEAP_VADDR_END - stack_size, stack_size,\n'
      '                 VMAPermission::ReadWrite, MemoryState::Locked, true);\n',
      '    HeapAllocate(std::addressof(out_addr), Memory::HEAP_VADDR_END - stack_size, stack_size,\n'
      '                 VMAPermission::ReadWrite, MemoryState::Locked, true);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_phase_aligned = false;\n'
      '#endif\n')

# --- 00.68: where the emulation thread's own time goes, in the stall log. 00.67's log (Super Mario 3D Land on the
# PS4) had 35 frames of 100-683 ms in the game's loads with no pipeline, shader, texture, file or GPU wait behind them:
# the emulation thread's own work, which the log didn't divide. Now each frame counts, with the CPU's time stamp
# counter (cheap enough for every HLE call; its rate measured against the clock over the frame itself): the guest's
# code (dynarmic's run), the JIT compiling it (blocks, and the flushes of its whole cache), the HLE kernel and
# services (SVCs), the GPU's commands (GSP's, PICA's; with the surfaces made for them) and the audio DSP's frames, and
# the draws skipped while their pipeline compiles (async shaders). common/emusuni_frame_stats.h writes the lines.
addfile('src/common/emusuni_frame_stats.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the log's account of the emulation thread's time (citra_libretro.cpp's
// retro_run makes one of these for every frame): a frame of more than 100 ms logs "EmuSuNi: slow frame ...", and every
// 600 frames one line sums them up ("EmuSuNi: 600 frames in ..."), from common/emusuni_stats.h's counters. Of the
// emulation thread's own work: the guest's code (the ARM11's, recompiled), dynarmic compiling it (blocks, and the
// flushes of its whole cache), the HLE kernel and services, the GPU's commands (PICA, with the surfaces made for them)
// and the audio DSP; then what it waited for and what other threads did.
#pragma once
#ifdef EMUSUNI_PORT
#include <cstdint>
#include <cstdlib>
#include <string>
#include <fmt/format.h>
#include "common/emusuni_stats.h"
#include "common/logging/log.h"

namespace EmuSuNi::Stats {
struct FrameStats {
    std::uint64_t t0 = NowUs();
    std::uint64_t k0 = Ticks();
    std::uint64_t at[Count];
    std::uint64_t ticks_at[TickCount];
    std::uint64_t counts_at[CountCount];

    FrameStats() {
        on_emulation_thread = true;
        for (int i = 0; i < Count; i++) {
            at[i] = counters[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < TickCount; i++) {
            ticks_at[i] = ticks[i];
        }
        for (int i = 0; i < CountCount; i++) {
            counts_at[i] = counts[i];
        }
    }

    // d: the counters' differences, dt: the ticks', dc: the counts'; ms of wall time; tpu: ticks per microsecond.
    static std::string Describe(const std::uint64_t* d, const std::uint64_t* dt, const std::uint64_t* dc,
                                std::uint64_t ms, double tpu) {
        const auto t_ms = [tpu](std::uint64_t t) -> std::uint64_t {
            return tpu > 0 ? static_cast<std::uint64_t>(static_cast<double>(t) / tpu / 1000.0) : 0;
        };
        // (Inside the guest's run: its code, the JIT's compiling and the HLE calls; the GPU's commands mostly come
        // through an HLE call, GSP's, and are taken out of it.)
        const std::uint64_t jit = dt[JitTicks], svc = dt[SvcTicks], gpu_in_svc = dt[GpuInSvcTicks];
        const std::uint64_t guest = dt[GuestTicks] > jit + svc ? dt[GuestTicks] - jit - svc : 0;
        const std::uint64_t hle = svc > gpu_in_svc ? svc - gpu_in_svc : 0;
        return fmt::format("{} ms: guest code {} ms, JIT compiling {} ms ({} blocks, {} cache flushes), HLE {} ms ({} "
                           "calls), GPU commands {} ms (surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "
                           "side; pipelines {} here ({} ms) and {} on the workers ({} ms); shaders {} here ({} ms) and {} "
                           "on the workers ({} ms); draws skipped {}; textures {} KiB ({} ms); files read {} KiB here "
                           "({} ms) and {} KiB elsewhere ({} ms)",
                           ms, t_ms(guest), t_ms(jit), dc[JitBlocks], dc[JitFlushes], t_ms(hle), dc[SvcCalls],
                           t_ms(dt[GpuTicks]), dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),
                           d[WaitUs] / 1000, d[PipelinesEmu], d[PipelineUsEmu] / 1000, d[PipelinesOther],
                           d[PipelineUsOther] / 1000, d[ShadersEmu], d[ShaderUsEmu] / 1000, d[ShadersOther],
                           d[ShaderUsOther] / 1000, dc[DrawsSkipped], d[UploadBytes] / 1024, d[UploadUs] / 1000,
                           d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024, d[ReadUsOther] / 1000);
    }

    ~FrameStats() {
        static std::uint64_t period_at[Count], period_ticks[TickCount], period_counts[CountCount];
        static std::uint64_t period_t0 = t0, period_k0 = k0;
        static int frames = 0;
        static bool first = true;
        if (first) {
            // (The first period starts with the first frame, not with what the game's load counted.)
            for (int i = 0; i < Count; i++) {
                period_at[i] = at[i];
            }
            for (int i = 0; i < TickCount; i++) {
                period_ticks[i] = ticks_at[i];
            }
            for (int i = 0; i < CountCount; i++) {
                period_counts[i] = counts_at[i];
            }
            first = false;
        }
        const std::uint64_t now_us = NowUs(), now_k = Ticks();
        const std::uint64_t ms = (now_us - t0) / 1000;
        std::uint64_t d[Count], dt[TickCount], dc[CountCount];
        // (100 ms, or EMUSUNI_3DS_SLOW_MS on the PC tests)
        static const std::uint64_t slow_ms =
            std::getenv("EMUSUNI_3DS_SLOW_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_SLOW_MS"), nullptr, 10) : 100;
        if (ms >= slow_ms) {
            for (int i = 0; i < Count; i++) {
                d[i] = counters[i].load(std::memory_order_relaxed) - at[i];
            }
            for (int i = 0; i < TickCount; i++) {
                dt[i] = ticks[i] - ticks_at[i];
            }
            for (int i = 0; i < CountCount; i++) {
                dc[i] = counts[i] - counts_at[i];
            }
            const double tpu = now_us > t0 ? static_cast<double>(now_k - k0) / static_cast<double>(now_us - t0) : 0;
            LOG_WARNING(Frontend, "EmuSuNi: slow frame {}", Describe(d, dt, dc, ms, tpu));
        }
        if (++frames == 600) {
            for (int i = 0; i < Count; i++) {
                const std::uint64_t now = counters[i].load(std::memory_order_relaxed);
                d[i] = now - period_at[i];
                period_at[i] = now;
            }
            for (int i = 0; i < TickCount; i++) {
                dt[i] = ticks[i] - period_ticks[i];
                period_ticks[i] = ticks[i];
            }
            for (int i = 0; i < CountCount; i++) {
                dc[i] = counts[i] - period_counts[i];
                period_counts[i] = counts[i];
            }
            const double tpu = now_us > period_t0 ? static_cast<double>(now_k - period_k0) /
                                                        static_cast<double>(now_us - period_t0)
                                                  : 0;
            LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, dt, dc, (now_us - period_t0) / 1000, tpu));
            period_t0 = now_us;
            period_k0 = now_k;
            frames = 0;
        }
    }
};
} // namespace EmuSuNi::Stats
#endif
''')
patch('src/common/emusuni_stats.h',
      '} // namespace EmuSuNi::Stats\n#endif\n',
      r'''
// The emulation thread's own work, in ticks of the CPU's time stamp counter (read at every HLE call, so cheaper than
// the clock: a system call on the PS4). Written and read on that thread only (retro_run's), so plain counters;
// common/emusuni_frame_stats.h turns ticks into ms with the counter's rate measured over the frame.
enum TickCounter { GuestTicks, JitTicks, SvcTicks, GpuTicks, GpuInSvcTicks, DspTicks, SurfaceTicks, TickCount };
enum CountCounter { JitBlocks, JitFlushes, SvcCalls, Surfaces, DrawsSkipped, CountCount };
inline std::uint64_t ticks[TickCount];
inline std::uint64_t counts[CountCount];
inline thread_local bool in_svc = false;
#if defined(__ORBIS__)
extern "C" std::uint64_t sceKernelReadTsc(void);
inline std::uint64_t Ticks() {
    return sceKernelReadTsc();
}
#else
inline std::uint64_t Ticks() {
    return __builtin_ia32_rdtsc();
}
#endif
struct TickTimer {
    TickCounter counter;
    std::uint64_t k0 = Ticks();
    explicit TickTimer(TickCounter counter_) : counter(counter_) {}
    ~TickTimer() {
        ticks[counter] += Ticks() - k0;
    }
};
// An HLE call (SVC): its time, and "inside an HLE call" for the GPU's commands that come through one.
struct SvcTimer {
    std::uint64_t k0 = Ticks();
    SvcTimer() {
        counts[SvcCalls]++;
        in_svc = true;
    }
    ~SvcTimer() {
        in_svc = false;
        ticks[SvcTicks] += Ticks() - k0;
    }
};
struct GpuTimer {
    std::uint64_t k0 = Ticks();
    bool svc = in_svc;
    ~GpuTimer() {
        const std::uint64_t d = Ticks() - k0;
        ticks[GpuTicks] += d;
        if (svc) {
            ticks[GpuInSvcTicks] += d;
        }
    }
};
} // namespace EmuSuNi::Stats
#endif
''')
patch('src/citra_libretro/citra_libretro.cpp',
      r'''    struct FrameStats {
        std::uint64_t t0 = EmuSuNi::Stats::NowUs();
        std::uint64_t at[EmuSuNi::Stats::Count];
        FrameStats() {
            EmuSuNi::Stats::on_emulation_thread = true;
            for (int i = 0; i < EmuSuNi::Stats::Count; i++)
                at[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);
        }
        static std::string Describe(const std::uint64_t* d, std::uint64_t ms) {
            using namespace EmuSuNi::Stats;
            return fmt::format("{} ms: waited {} ms for the GPU side; pipelines {} here ({} ms) and {} on the "
                               "workers ({} ms); shaders {} here ({} ms) and {} on the workers ({} ms); "
                               "textures {} KiB ({} ms); files read {} KiB here ({} ms) and {} KiB elsewhere ({} ms)",
                               ms, d[WaitUs] / 1000, d[PipelinesEmu], d[PipelineUsEmu] / 1000, d[PipelinesOther],
                               d[PipelineUsOther] / 1000, d[ShadersEmu], d[ShaderUsEmu] / 1000, d[ShadersOther],
                               d[ShaderUsOther] / 1000, d[UploadBytes] / 1024, d[UploadUs] / 1000,
                               d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024,
                               d[ReadUsOther] / 1000);
        }
        ~FrameStats() {
            static std::uint64_t period_at[EmuSuNi::Stats::Count];
            static std::uint64_t period_t0 = t0;
            static int frames = 0;
            const std::uint64_t ms = (EmuSuNi::Stats::NowUs() - t0) / 1000;
            std::uint64_t d[EmuSuNi::Stats::Count];
            // (100 ms, or EMUSUNI_3DS_SLOW_MS on the PC tests)
            static const std::uint64_t slow_ms =
                std::getenv("EMUSUNI_3DS_SLOW_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_SLOW_MS"), nullptr, 10) : 100;
            if (ms >= slow_ms) {
                for (int i = 0; i < EmuSuNi::Stats::Count; i++)
                    d[i] = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed) - at[i];
                LOG_WARNING(Frontend, "EmuSuNi: slow frame {}", Describe(d, ms));
            }
            if (++frames == 600) {
                for (int i = 0; i < EmuSuNi::Stats::Count; i++) {
                    const std::uint64_t now = EmuSuNi::Stats::counters[i].load(std::memory_order_relaxed);
                    d[i] = now - period_at[i];
                    period_at[i] = now;
                }
                LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, (EmuSuNi::Stats::NowUs() - period_t0) / 1000));
                period_t0 = EmuSuNi::Stats::NowUs();
                frames = 0;
            }
        }
    } emusuni_frame_stats;
''',
      '    EmuSuNi::Stats::FrameStats emusuni_frame_stats; // (common/emusuni_frame_stats.h)\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "common/emusuni_fastmem.h"\n#include "common/emusuni_stats.h"\n#endif\n',
      '#include "common/emusuni_fastmem.h"\n#include "common/emusuni_frame_stats.h"\n#include "common/emusuni_stats.h"\n'
      '#endif\n')
# The guest's run (its code, the JIT's compiling and the HLE calls made from it).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_fastmem.h"\n#include "common/settings.h"\n',
      '#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_fastmem.h"\n#include "common/emusuni_stats.h"\n'
      '#include "common/settings.h"\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    if (break_flag) [[unlikely]] {\n        return;\n    }\n\n    jit->Run();\n}\n',
      '    if (break_flag) [[unlikely]] {\n        return;\n    }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::GuestTicks};\n'
      '#endif\n'
      '    jit->Run();\n}\n')
# The HLE calls.
patch('src/core/hle/kernel/svc.cpp',
      '#include "common/scm_rev.h"\n',
      '#include "common/scm_rev.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/core/hle/kernel/svc.cpp',
      'void SVC::CallSVC(u32 immediate) {\n    MICROPROFILE_SCOPE(Kernel_SVC);\n',
      'void SVC::CallSVC(u32 immediate) {\n    MICROPROFILE_SCOPE(Kernel_SVC);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::SvcTimer emusuni_timer;\n'
      '#endif\n')
# The GPU's commands (GSP's: PICA command lists, transfers, fills).
patch('src/video_core/gpu.cpp',
      '#include "common/hacks/hack_manager.h"\n',
      '#include "common/hacks/hack_manager.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/gpu.cpp',
      'void GPU::Execute(const Service::GSP::Command& command) {\n',
      'void GPU::Execute(const Service::GSP::Command& command) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::GpuTimer emusuni_timer;\n'
      '#endif\n')
# The audio DSP's frames (every 4.9 ms of the game's time).
patch('src/audio_core/hle/hle.cpp',
      '#include "audio_core/sink.h"\n',
      '#include "audio_core/sink.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/audio_core/hle/hle.cpp',
      'void DspHle::Impl::AudioTickCallback(s64 cycles_late) {\n',
      'void DspHle::Impl::AudioTickCallback(s64 cycles_late) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::DspTicks};\n'
      '#endif\n')
# The surfaces the rasterizer cache makes (a texture or render target the GPU hasn't had yet).
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '        return slot_surfaces.insert(runtime, params, initial_flags);\n    }();\n',
      '#ifdef EMUSUNI_PORT\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::SurfaceTicks};\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::Surfaces]++;\n'
      '#endif\n'
      '        return slot_surfaces.insert(runtime, params, initial_flags);\n    }();\n')
# The draws skipped while their pipeline compiles (async shaders).
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#include "video_core/texture/texture_decode.h"\n',
      '#include "video_core/texture/texture_decode.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {\n        return true;\n    }\n',
      '    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::DrawsSkipped]++;\n'
      '#endif\n'
      '        return true;\n    }\n')
# dynarmic compiling a block (and the flushes of its whole cache: full, or a cache clear). Its IR verification pass,
# which only asserts (a map of every instruction's uses, for every block compiled), is left out: compiling is the
# loads' cost on the PS4.
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '#include "dynarmic/common/atomic.h"\n',
      '#include "dynarmic/common/atomic.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#    include "../../../../../../src/common/emusuni_stats.h"\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '        auto block = emitter.GetBasicBlock(descriptor);\n        if (block)\n            return *block;\n',
      '        auto block = emitter.GetBasicBlock(descriptor);\n        if (block)\n            return *block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const ::EmuSuNi::Stats::TickTimer emusuni_timer{::EmuSuNi::Stats::JitTicks};\n'
      '        ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitBlocks]++;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '        Optimization::VerificationPass(ir_block);\n',
      '#ifndef EMUSUNI_PORT\n'
      '        Optimization::VerificationPass(ir_block);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '            if (invalidate_entire_cache) {\n                block_of_code.ClearCache();\n',
      '            if (invalidate_entire_cache) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitFlushes]++;\n'
      '#endif\n'
      '                block_of_code.ClearCache();\n')

# --- 00.68: asynchronous shaders by default. 00.67's log: Super Mario 3D Land at full speed once loaded, but stopped
# for 0.26-0.38 s whenever a new effect needed a pipeline the driver's cache hadn't (three times in four minutes), the
# whole game waiting for it. Now such a draw is skipped until its pipeline is built on the workers (an object shows a
# few frames late), as Azahar does on Android; draws of 6 vertices or fewer still wait (Azahar's rule: the 2D parts).
# The file /data/EmuSuNi/3ds-sync-shaders (or EMUSUNI_3DS_SYNC_SHADERS=1 on the PC) waits for every pipeline as before.
patch('src/citra_libretro/core_settings.cpp',
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for, with the file\n'
      '    // /data/EmuSuNi/3ds-async-shaders (or EMUSUNI_3DS_ASYNC_SHADERS=1 on the PC).\n'
      '    Settings::values.async_shader_compilation = std::getenv("EMUSUNI_3DS_ASYNC_SHADERS") != nullptr ||\n'
      '                                                ::access("/data/EmuSuNi/3ds-async-shaders", F_OK) == 0;\n',
      r'''    // EmuSuNi: a draw whose pipeline isn't built yet is skipped rather than waited for (the object shows a few
    // frames late instead of the game stopping), by default since 00.68; the file /data/EmuSuNi/3ds-sync-shaders (or
    // EMUSUNI_3DS_SYNC_SHADERS=1 on the PC) waits for every pipeline as before.
    Settings::values.async_shader_compilation = std::getenv("EMUSUNI_3DS_SYNC_SHADERS") == nullptr &&
                                                ::access("/data/EmuSuNi/3ds-sync-shaders", F_OK) != 0;
    {
        static int said = -1; // (said once, and again if it changes)
        const int now = Settings::values.async_shader_compilation.GetValue() ? 1 : 0;
        if (now != said) {
            LOG_INFO(Frontend, "EmuSuNi: shaders: {}",
                     now ? "a draw whose pipeline isn't built yet is skipped until it is (async; the file "
                           "/data/EmuSuNi/3ds-sync-shaders waits instead)"
                         : "every draw waits for its pipeline (the file /data/EmuSuNi/3ds-sync-shaders)");
            said = now;
        }
    }
''')
# The PC tests only: pipelines that take as long to compile as the PS4's.
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '#include "common/hash.h"\n',
      '#include "common/hash.h"\n'
      '#if defined(EMUSUNI_PORT) && !defined(__ORBIS__)\n#include <chrono>\n#include <cstdlib>\n#include <thread>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '#endif\n',
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '#endif\n'
      '#if defined(EMUSUNI_PORT) && !defined(__ORBIS__)\n'
      '    // EmuSuNi, the PC tests only: EMUSUNI_3DS_TEST_PIPELINE_MS=<n> makes each compile n ms longer, as long as the\n'
      '    // PS4\'s (0.25-0.5 s), to see what a game does meanwhile (async shaders\' pop-in, the stalls).\n'
      '    if (!fail_on_compile_required) {\n'
      '        static const int emusuni_test_ms = std::getenv("EMUSUNI_3DS_TEST_PIPELINE_MS")\n'
      '                                               ? std::atoi(std::getenv("EMUSUNI_3DS_TEST_PIPELINE_MS"))\n'
      '                                               : 0;\n'
      '        if (emusuni_test_ms > 0) {\n'
      '            std::this_thread::sleep_for(std::chrono::milliseconds(emusuni_test_ms));\n'
      '        }\n'
      '    }\n'
      '#endif\n')

# --- 00.68: the audio's resampling without the deque's indexing. AudioInterp::Linear (2.7% of the emulation thread in
# Super Mario 3D Land on the PC, the first of the audio's) put the two samples kept from the last call in front of the
# deque and indexed it three times a sample; now the same steps run over a contiguous copy of the samples the call can
# reach. The output is the same bit for bit (the PC test: the hash of every sample over a scene).
patch('src/audio_core/interpolate.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include <vector>\n#endif\n')
patch('src/audio_core/interpolate.cpp',
      '    if (input.empty())\n        return;\n\n    input.insert(input.begin(), {state.xn2, state.xn1});\n',
      r'''    if (input.empty())
        return;

#ifdef EMUSUNI_PORT
    {
        // EmuSuNi: upstream's steps (below) over a contiguous copy: [xn2, xn1, the samples this call can reach].
        const u64 step_size = static_cast<u64>(rate * scale_factor);
        u64 fposition = state.fposition;
        const std::size_t total = input.size() + 2; // the samples upstream's loop sees
        const std::size_t steps = output.size() - outputi;
        const u64 last = fposition + step_size * (steps > 0 ? steps - 1 : 0);
        const std::size_t reach = std::min<std::size_t>(total, static_cast<std::size_t>(last / scale_factor) + 3);
        thread_local std::vector<std::array<s16, 2>> buffer;
        buffer.resize(reach);
        buffer[0] = state.xn2;
        buffer[1] = state.xn1;
        std::copy_n(input.begin(), reach - 2, buffer.begin() + 2);
        std::size_t inputi = 0;
        while (outputi < output.size()) {
            inputi = static_cast<std::size_t>(fposition / scale_factor);
            if (inputi + 2 >= total) {
                inputi = total - 2;
                break;
            }
            const u64 fraction = fposition & scale_mask;
            output[outputi++] = fn(fraction, buffer[inputi], buffer[inputi + 1], buffer[inputi + 2]);
            fposition += step_size;
        }
        state.xn2 = buffer[inputi];
        state.xn1 = buffer[inputi + 1];
        state.fposition = fposition - inputi * scale_factor;
        input.erase(input.begin(), std::next(input.begin(), inputi));
        return;
    }
#endif
    input.insert(input.begin(), {state.xn2, state.xn1});
''')

# --- 00.68: the fast memory on by default (00.66-00.67: only with the file /data/EmuSuNi/3ds-fastmem); the file
# /data/EmuSuNi/3ds-fastmem-off (or EMUSUNI_3DS_FASTMEM=0 on the PC) turns it off. The self-test, the fallbacks and the
# marker files as before; the "trying" marker now goes once the first game has run 120 frames with 64 of the JIT's
# faults turned into calls (or 1800 frames), or when that game ends normally: a session closed early is no crash.
patch('src/common/emusuni_fastmem.cpp',
      r'''bool SwitchOn(char* how, std::size_t size) {
#if defined(__ORBIS__)
    const bool on = ::access("/data/EmuSuNi/3ds-fastmem", F_OK) == 0;
    std::snprintf(how, size, on ? "the file /data/EmuSuNi/3ds-fastmem" : "no file /data/EmuSuNi/3ds-fastmem");
    return on;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM");
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
    std::snprintf(how, size, on ? "EMUSUNI_3DS_FASTMEM=1" : "EMUSUNI_3DS_FASTMEM isn't 1");
    return on;
#endif
}
''',
      r'''bool SwitchOn(char* how, std::size_t size) {
#if defined(__ORBIS__)
    const bool off = ::access("/data/EmuSuNi/3ds-fastmem-off", F_OK) == 0;
    std::snprintf(how, size, off ? "the file /data/EmuSuNi/3ds-fastmem-off" : "by default");
    return !off;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM");
    const bool off = value != nullptr && std::strcmp(value, "0") == 0;
    std::snprintf(how, size, off ? "EMUSUNI_3DS_FASTMEM=0" : "by default");
    return !off;
#endif
}
''')
patch('src/common/emusuni_fastmem.cpp',
      '    if ((g_frames >= 600 && faults >= 64) || g_frames >= 3600) {\n',
      '    if ((g_frames >= 120 && faults >= 64) || g_frames >= 1800) {\n')
patch('src/common/emusuni_fastmem.cpp',
      '    std::lock_guard lock{g_lock};\n    emusuni_dynarmic_fault_handler_uninstall();\n',
      '    std::lock_guard lock{g_lock};\n    emusuni_dynarmic_fault_handler_uninstall();\n'
      '    if (g_on && g_trying) {\n'
      '        // (The game ended normally before the frames above: no crash while trying it.)\n'
      '        MarkerRemove("3ds-fastmem-trying");\n'
      '        g_trying = false;\n'
      '    }\n')


# --- 00.69: dynarmic compiles a block faster, the same code. The 00.68 log (the PS4): the slow frames of the games'
# loads and new areas are dynarmic compiling the ARM11's code for the first time - Super Mario 3D Land's first 600
# frames 61,324 blocks in 5.1 s (83 us a block; this PC: 11 us). Each change leaves the x86 code it writes as it was:
#  - The ARM decoder searched the VFP table (62 patterns) and the ASIMD table (147, NEON: the 3DS's ARM11 has none)
#    one pattern after another for every instruction before the fast table of the ARM ones. Every VFP pattern has
#    bits 27-26 = 11 and 11-9 = 101 and every ASIMD pattern the top nibble 1111 (checked against vfp.inc and asimd.inc
#    of this tree): a table is searched only when its patterns can match.
#  - The register allocator released all 102 of its places (16 registers, 16 XMM, 6 flags, 64 spill slots) after
#    every IR instruction; now only the ones used since the last release (releasing is idempotent: the rest are as
#    the release left them).
#  - The IR's memory pool took room for 4096 instructions (about 400 KiB from the app's heap) for every block; now 256
#    at a time, more when a block needs them.
#  - The perf map's name of every block (a formatted string) only when the perf map is on (PERF_BUILDID_DIR, Linux).
patch('externals/dynarmic/src/dynarmic/frontend/A32/translate/translate_arm.cpp',
      '            if (const auto vfp_decoder = DecodeVFP<TranslatorVisitor>(*arm_instruction)) {\n'
      '                should_continue = vfp_decoder->get().call(visitor, *arm_instruction);\n'
      '            } else if (const auto asimd_decoder = DecodeASIMD<TranslatorVisitor>(*arm_instruction)) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi: the VFP and ASIMD tables only where their patterns can match (scripts/build-3ds-core.sh).\n'
      '            if (const auto vfp_decoder = (*arm_instruction & 0x0C000E00) == 0x0C000A00\n'
      '                                             ? DecodeVFP<TranslatorVisitor>(*arm_instruction)\n'
      '                                             : std::nullopt) {\n'
      '                should_continue = vfp_decoder->get().call(visitor, *arm_instruction);\n'
      '            } else if (const auto asimd_decoder = (*arm_instruction & 0xF0000000) == 0xF0000000\n'
      '                                                      ? DecodeASIMD<TranslatorVisitor>(*arm_instruction)\n'
      '                                                      : std::nullopt) {\n'
      '#else\n'
      '            if (const auto vfp_decoder = DecodeVFP<TranslatorVisitor>(*arm_instruction)) {\n'
      '                should_continue = vfp_decoder->get().call(visitor, *arm_instruction);\n'
      '            } else if (const auto asimd_decoder = DecodeASIMD<TranslatorVisitor>(*arm_instruction)) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    std::vector<HostLocInfo> hostloc_info;\n    HostLocInfo& LocInfo(HostLoc loc);\n',
      '    std::vector<HostLocInfo> hostloc_info;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the places LocInfo handed out for change since the last EndOfAllocScope (bit = HostLoc).\n'
      '    std::uint64_t emusuni_used[2] = {0, 0};\n'
      '#endif\n'
      '    HostLocInfo& LocInfo(HostLoc loc);\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::EndOfAllocScope() {\n    for (auto& iter : hostloc_info) {\n        iter.ReleaseAll();\n    }\n}\n',
      'void RegAlloc::EndOfAllocScope() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: only the places used since the last release (ReleaseAll is idempotent: the others are as it left\n'
      '    // them; every change goes through the non-const LocInfo).\n'
      '    static_assert(NonSpillHostLocCount + SpillCount <= 128);\n'
      '    for (size_t word = 0; word < 2; word++) {\n'
      '        std::uint64_t bits = emusuni_used[word];\n'
      '        emusuni_used[word] = 0;\n'
      '        while (bits != 0) {\n'
      '            hostloc_info[word * 64 + static_cast<size_t>(__builtin_ctzll(bits))].ReleaseAll();\n'
      '            bits &= bits - 1;\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    for (auto& iter : hostloc_info) {\n        iter.ReleaseAll();\n    }\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLocInfo& RegAlloc::LocInfo(HostLoc loc) {\n'
      '    ASSERT(loc != HostLoc::RSP && loc != HostLoc::R15);\n'
      '    return hostloc_info[static_cast<size_t>(loc)];\n}\n',
      'HostLocInfo& RegAlloc::LocInfo(HostLoc loc) {\n'
      '    ASSERT(loc != HostLoc::RSP && loc != HostLoc::R15);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    const size_t index = static_cast<size_t>(loc);\n'
      '    emusuni_used[index >> 6] |= std::uint64_t{1} << (index & 63);\n'
      '    return hostloc_info[index];\n'
      '#else\n'
      '    return hostloc_info[static_cast<size_t>(loc)];\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/ir/basic_block.cpp',
      '        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 4096)} {}\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi: room for 256 instructions at a time (4096 took ~400 KiB of the heap for every block).\n'
      '        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 256)} {}\n'
      '#else\n'
      '        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 4096)} {}\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp',
      '#include <iterator>\n',
      '#include <iterator>\n#ifdef EMUSUNI_PORT\n#include <stdlib.h>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/emit_x64.cpp',
      '    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
      '    Patch(descriptor, entrypoint);\n',
      '#ifdef EMUSUNI_PORT\n'
      '#ifdef __linux__\n'
      '    // EmuSuNi: the block\'s name only when the perf map is on (its file is written only then; elsewhere\n'
      '    // PerfMapRegister does nothing).\n'
      '    static const bool emusuni_perf_map = ::getenv("PERF_BUILDID_DIR") != nullptr;\n'
      '    if (emusuni_perf_map) {\n'
      '        PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
      '    }\n'
      '#endif\n'
      '#else\n'
      '    PerfMapRegister(entrypoint, code.getCurr(), LocationDescriptorToFriendlyName(descriptor));\n'
      '#endif\n'
      '    Patch(descriptor, entrypoint);\n')

# --- 00.69: dynarmic's register allocator without its assertion scans. dynarmic's ASSERTs are always on (mcl), and
# three of them scanned all 102 places for every IR instruction: DefineValue's "not defined yet" (ValueLocation of a
# value that is nowhere: the whole scan, with a search of every place's list) and AssertNoMoreUses at every block's
# end; GetArgumentInfo and DefineValue looked the same value up twice (once in the assertion). The checks are of
# dynarmic's own invariants (the same code is written); the lookups that remain are kept.
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '            ASSERT_MSG(ValueLocation(arg.GetInst()), "argument must already been defined");\n'
      '            LocInfo(*ValueLocation(arg.GetInst())).AddArgReference();\n',
      '#ifdef EMUSUNI_PORT\n'
      '            const auto emusuni_location = ValueLocation(arg.GetInst());  // EmuSuNi: looked up once\n'
      '            ASSERT_MSG(emusuni_location, "argument must already been defined");\n'
      '            LocInfo(*emusuni_location).AddArgReference();\n'
      '#else\n'
      '            ASSERT_MSG(ValueLocation(arg.GetInst()), "argument must already been defined");\n'
      '            LocInfo(*ValueLocation(arg.GetInst())).AddArgReference();\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, HostLoc host_loc) {\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, HostLoc host_loc) {\n'
      '#ifndef EMUSUNI_PORT  // EmuSuNi: a scan of every place for every value defined\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, const IR::Value& use_inst) {\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n',
      'void RegAlloc::DefineValueImpl(IR::Inst* def_inst, const IR::Value& use_inst) {\n'
      '#ifndef EMUSUNI_PORT  // EmuSuNi: a scan of every place for every value defined\n'
      '    ASSERT_MSG(!ValueLocation(def_inst), "def_inst has already been defined");\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    ASSERT_MSG(ValueLocation(use_inst.GetInst()), "use_inst must already be defined");\n'
      '    const HostLoc location = *ValueLocation(use_inst.GetInst());\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const auto emusuni_location = ValueLocation(use_inst.GetInst());  // EmuSuNi: looked up once\n'
      '    ASSERT_MSG(emusuni_location, "use_inst must already be defined");\n'
      '    const HostLoc location = *emusuni_location;\n'
      '#else\n'
      '    ASSERT_MSG(ValueLocation(use_inst.GetInst()), "use_inst must already be defined");\n'
      '    const HostLoc location = *ValueLocation(use_inst.GetInst());\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'void RegAlloc::AssertNoMoreUses() {\n'
      '    ASSERT(std::all_of(hostloc_info.begin(), hostloc_info.end(), [](const auto& i) { return i.IsEmpty(); }));\n'
      '}\n',
      'void RegAlloc::AssertNoMoreUses() {\n'
      '#ifndef EMUSUNI_PORT  // EmuSuNi: a scan of every place at every block\'s end\n'
      '    ASSERT(std::all_of(hostloc_info.begin(), hostloc_info.end(), [](const auto& i) { return i.IsEmpty(); }));\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    ASSERT(IsValueLive(inst) || !inst->HasUses());\n',
      '    ASSERT(!inst->HasUses() || IsValueLive(inst));  // (EmuSuNi: the cheap test first; the same check)\n')
# The memory accesses' "no fast memory here" markers (each instruction that faulted once): a hash set instead of a
# tree (looked up for every load and store compiled; thousands of entries in a game).
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '    std::set<DoNotFastmemMarker> do_not_fastmem;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    struct EmuSuNiMarkerHash {\n'
      '        size_t operator()(const DoNotFastmemMarker& m) const noexcept {\n'
      '            return std::hash<u64>{}(std::get<0>(m).Value() * 0x9E3779B97F4A7C15ull ^ std::get<1>(m));\n'
      '        }\n'
      '    };\n'
      '    tsl::robin_set<DoNotFastmemMarker, EmuSuNiMarkerHash> do_not_fastmem;  // EmuSuNi: a hash set\n'
      '#else\n'
      '    std::set<DoNotFastmemMarker> do_not_fastmem;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '#include <tsl/robin_map.h>\n',
      '#include <tsl/robin_map.h>\n#ifdef EMUSUNI_PORT\n#include <tsl/robin_set.h>\n#endif\n')

# --- 00.69: dynarmic compiles a block with fewer allocations, the same code. This PC counted 55 allocations of the
# heap for every block compiled (68 in all, the decoder's tables at the first block included: 650,000 once); on the
# PS4 each malloc/free pair takes the app's heap mutex. The ones that went:
#  - the register allocator copied its candidate registers into a new vector for every register it chose (6 a block)
#    and made a one-element vector for every fixed register (a host call: one per caller-saved register): a span of
#    them, the candidates copied into a stack array (the same partitions, so the same registers);
#  - its 102 places in an array instead of a heap vector, its register orders as spans of the emitters' (any_xmm is
#    a namespace-scope initializer_list: static storage; the A32 order a member), and the
#    A32 emitter's register order made once, not for every block;
#  - each place's list of values with room for 4 inside (a small vector);
#  - the identity removal pass's list of removed instructions, the same;
#  - room for 8 deferred emits from the start (the vector grew 2-3 times a block);
#  - the IR pool's slab kept for the next block on the same thread (one slab, its usual size only);
#  - the ARM decoder's 4096 buckets hold pointers to the matchers instead of copies of them (each copy a std::function
#    allocation: 650,000 at the first block of the first game);
#  - the blocks' ranges (for invalidation) in buckets of 4 KiB of the guest's addresses instead of an interval map of
#    sets (whose every insertion allocated 3 times and split intervals): the same locations found for a range.
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '#include <optional>\n#include <utility>\n#include <vector>\n',
      '#include <optional>\n#include <utility>\n#include <vector>\n'
      '#ifdef EMUSUNI_PORT\n#include <span>\n#include <boost/container/small_vector.hpp>\n'
      '#include "dynarmic/backend/x64/stack_layout.h"\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    // Value state\n    std::vector<IR::Inst*> values;\n',
      '    // Value state\n'
      '#ifdef EMUSUNI_PORT\n'
      '    boost::container::small_vector<IR::Inst*, 4> values;  // EmuSuNi: room for 4 inside\n'
      '#else\n'
      '    std::vector<IR::Inst*> values;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    explicit RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the orders as spans (the emitters\' vectors and any_xmm, a namespace-scope initializer_list,\n'
      '    // outlive every RegAlloc they make).\n'
      '    explicit RegAlloc(BlockOfCode& code, std::span<const HostLoc> gpr_order, std::span<const HostLoc> xmm_order);\n'
      '#else\n'
      '    explicit RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    std::vector<HostLoc> gpr_order;\n    std::vector<HostLoc> xmm_order;\n\n'
      '    HostLoc SelectARegister(const std::vector<HostLoc>& desired_locations) const;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::span<const HostLoc> gpr_order;\n    std::span<const HostLoc> xmm_order;\n\n'
      '    HostLoc SelectARegister(std::span<const HostLoc> desired_locations) const;\n'
      '#else\n'
      '    std::vector<HostLoc> gpr_order;\n    std::vector<HostLoc> xmm_order;\n\n'
      '    HostLoc SelectARegister(const std::vector<HostLoc>& desired_locations) const;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    HostLoc UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc ScratchImpl(const std::vector<HostLoc>& desired_locations);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    HostLoc UseImpl(IR::Value use_value, std::span<const HostLoc> desired_locations);\n'
      '    HostLoc UseScratchImpl(IR::Value use_value, std::span<const HostLoc> desired_locations);\n'
      '    HostLoc ScratchImpl(std::span<const HostLoc> desired_locations);\n'
      '#else\n'
      '    HostLoc UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations);\n'
      '    HostLoc ScratchImpl(const std::vector<HostLoc>& desired_locations);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.h',
      '    std::vector<HostLocInfo> hostloc_info;\n#ifdef EMUSUNI_PORT\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::array<HostLocInfo, NonSpillHostLocCount + SpillCount> hostloc_info;  // EmuSuNi: no heap vector\n'
      '#else\n'
      '    std::vector<HostLocInfo> hostloc_info;\n'
      '#endif\n'
      '#ifdef EMUSUNI_PORT\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'RegAlloc::RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order)\n'
      '        : gpr_order(gpr_order)\n'
      '        , xmm_order(xmm_order)\n'
      '        , hostloc_info(NonSpillHostLocCount + SpillCount)\n'
      '        , code(code) {}\n',
      '#ifdef EMUSUNI_PORT\n'
      'RegAlloc::RegAlloc(BlockOfCode& code, std::span<const HostLoc> gpr_order, std::span<const HostLoc> xmm_order)\n'
      '        : gpr_order(gpr_order)\n'
      '        , xmm_order(xmm_order)\n'
      '        , code(code) {}\n'
      '#else\n'
      'RegAlloc::RegAlloc(BlockOfCode& code, std::vector<HostLoc> gpr_order, std::vector<HostLoc> xmm_order)\n'
      '        : gpr_order(gpr_order)\n'
      '        , xmm_order(xmm_order)\n'
      '        , hostloc_info(NonSpillHostLocCount + SpillCount)\n'
      '        , code(code) {}\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    arg.allocated = true;\n    UseImpl(arg.value, {host_loc});\n',
      '    arg.allocated = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    UseImpl(arg.value, std::span<const HostLoc>{&host_loc, 1});\n'
      '#else\n'
      '    UseImpl(arg.value, {host_loc});\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    arg.allocated = true;\n    UseScratchImpl(arg.value, {host_loc});\n',
      '    arg.allocated = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    UseScratchImpl(arg.value, std::span<const HostLoc>{&host_loc, 1});\n'
      '#else\n'
      '    UseScratchImpl(arg.value, {host_loc});\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'Xbyak::Reg64 RegAlloc::ScratchGpr(HostLoc desired_location) {\n'
      '    return HostLocToReg64(ScratchImpl({desired_location}));\n}\n',
      'Xbyak::Reg64 RegAlloc::ScratchGpr(HostLoc desired_location) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    return HostLocToReg64(ScratchImpl(std::span<const HostLoc>{&desired_location, 1}));\n'
      '#else\n'
      '    return HostLocToReg64(ScratchImpl({desired_location}));\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'Xbyak::Xmm RegAlloc::ScratchXmm(HostLoc desired_location) {\n'
      '    return HostLocToXmm(ScratchImpl({desired_location}));\n}\n',
      'Xbyak::Xmm RegAlloc::ScratchXmm(HostLoc desired_location) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    return HostLocToXmm(ScratchImpl(std::span<const HostLoc>{&desired_location, 1}));\n'
      '#else\n'
      '    return HostLocToXmm(ScratchImpl({desired_location}));\n'
      '#endif\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'HostLoc RegAlloc::UseImpl(IR::Value use_value, std::span<const HostLoc> desired_locations) {\n'
      '#else\n'
      'HostLoc RegAlloc::UseImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'HostLoc RegAlloc::UseScratchImpl(IR::Value use_value, std::span<const HostLoc> desired_locations) {\n'
      '#else\n'
      'HostLoc RegAlloc::UseScratchImpl(IR::Value use_value, const std::vector<HostLoc>& desired_locations) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::ScratchImpl(const std::vector<HostLoc>& desired_locations) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'HostLoc RegAlloc::ScratchImpl(std::span<const HostLoc> desired_locations) {\n'
      '#else\n'
      'HostLoc RegAlloc::ScratchImpl(const std::vector<HostLoc>& desired_locations) {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      '    for (HostLoc caller_saved : other_caller_save) {\n        ScratchImpl({caller_saved});\n    }\n',
      '    for (HostLoc caller_saved : other_caller_save) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        ScratchImpl(std::span<const HostLoc>{&caller_saved, 1});\n'
      '#else\n'
      '        ScratchImpl({caller_saved});\n'
      '#endif\n'
      '    }\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/reg_alloc.cpp',
      'HostLoc RegAlloc::SelectARegister(const std::vector<HostLoc>& desired_locations) const {\n'
      '    std::vector<HostLoc> candidates = desired_locations;\n\n'
      '    // Find all locations that have not been allocated..\n'
      '    const auto allocated_locs = std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return !this->LocInfo(loc).IsLocked();\n'
      '    });\n'
      '    candidates.erase(allocated_locs, candidates.end());\n'
      '    ASSERT_MSG(!candidates.empty(), "All candidate registers have already been allocated");\n\n'
      '    // Selects the best location out of the available locations.\n'
      '    // TODO: Actually do LRU or something. Currently we just try to pick something without a value if possible.\n\n'
      '    std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return this->LocInfo(loc).IsEmpty();\n'
      '    });\n\n'
      '    return candidates.front();\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: the same two partitions on a copy in a stack array (random-access iterators both: the same algorithm,\n'
      '// so the same register), not a new vector each time.\n'
      'HostLoc RegAlloc::SelectARegister(std::span<const HostLoc> desired_locations) const {\n'
      '    std::array<HostLoc, NonSpillHostLocCount> candidates;\n'
      '    ASSERT(desired_locations.size() <= candidates.size());\n'
      '    HostLoc* const first = candidates.data();\n'
      '    HostLoc* last = std::copy(desired_locations.begin(), desired_locations.end(), first);\n\n'
      '    // Find all locations that have not been allocated..\n'
      '    last = std::partition(first, last, [this](auto loc) {\n'
      '        return !this->LocInfo(loc).IsLocked();\n'
      '    });\n'
      '    ASSERT_MSG(first != last, "All candidate registers have already been allocated");\n\n'
      '    std::partition(first, last, [this](auto loc) {\n'
      '        return this->LocInfo(loc).IsEmpty();\n'
      '    });\n\n'
      '    return *first;\n'
      '}\n'
      '#else\n'
      'HostLoc RegAlloc::SelectARegister(const std::vector<HostLoc>& desired_locations) const {\n'
      '    std::vector<HostLoc> candidates = desired_locations;\n\n'
      '    // Find all locations that have not been allocated..\n'
      '    const auto allocated_locs = std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return !this->LocInfo(loc).IsLocked();\n'
      '    });\n'
      '    candidates.erase(allocated_locs, candidates.end());\n'
      '    ASSERT_MSG(!candidates.empty(), "All candidate registers have already been allocated");\n\n'
      '    // Selects the best location out of the available locations.\n'
      '    // TODO: Actually do LRU or something. Currently we just try to pick something without a value if possible.\n\n'
      '    std::partition(candidates.begin(), candidates.end(), [this](auto loc) {\n'
      '        return this->LocInfo(loc).IsEmpty();\n'
      '    });\n\n'
      '    return candidates.front();\n'
      '}\n'
      '#endif\n')
# The A32 emitter: its register order made once; room for 8 deferred emits.
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.cpp',
      '    const std::vector<HostLoc> gpr_order = [this] {\n'
      '        std::vector<HostLoc> gprs{any_gpr};\n'
      '        if (conf.page_table) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));\n'
      '        }\n'
      '        if (conf.fastmem_pointer) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));\n'
      '        }\n'
      '        return gprs;\n'
      '    }();\n\n'
      '    RegAlloc reg_alloc{code, gpr_order, any_xmm};\n'
      '    A32EmitContext ctx{conf, reg_alloc, block};\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the order made once (conf doesn\'t change), not for every block.\n'
      '    if (emusuni_gpr_order.empty()) {\n'
      '        emusuni_gpr_order = [this] {\n'
      '            std::vector<HostLoc> gprs{any_gpr};\n'
      '            if (conf.page_table) {\n'
      '                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));\n'
      '            }\n'
      '            if (conf.fastmem_pointer) {\n'
      '                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));\n'
      '            }\n'
      '            return gprs;\n'
      '        }();\n'
      '    }\n'
      '    const std::vector<HostLoc>& gpr_order = emusuni_gpr_order;\n'
      '#else\n'
      '    const std::vector<HostLoc> gpr_order = [this] {\n'
      '        std::vector<HostLoc> gprs{any_gpr};\n'
      '        if (conf.page_table) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));\n'
      '        }\n'
      '        if (conf.fastmem_pointer) {\n'
      '            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));\n'
      '        }\n'
      '        return gprs;\n'
      '    }();\n'
      '#endif\n\n'
      '    RegAlloc reg_alloc{code, gpr_order, any_xmm};\n'
      '    A32EmitContext ctx{conf, reg_alloc, block};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    ctx.deferred_emits.reserve(8);  // EmuSuNi: grown 2-3 times a block otherwise\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '    BlockDescriptor Emit(IR::Block& block);\n\n    void ClearCache() override;\n',
      '    BlockDescriptor Emit(IR::Block& block);\n\n    void ClearCache() override;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::vector<HostLoc> emusuni_gpr_order;  // EmuSuNi: Emit\'s register order, made once\n'
      '#endif\n')
# The identity removal pass's list.
patch('externals/dynarmic/src/dynarmic/ir/opt/identity_removal_pass.cpp',
      '#include <vector>\n',
      '#include <vector>\n#ifdef EMUSUNI_PORT\n#include <boost/container/small_vector.hpp>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/ir/opt/identity_removal_pass.cpp',
      'void IdentityRemovalPass(IR::Block& block) {\n    std::vector<IR::Inst*> to_invalidate;\n',
      'void IdentityRemovalPass(IR::Block& block) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    boost::container::small_vector<IR::Inst*, 32> to_invalidate;  // EmuSuNi: room for 32 inside\n'
      '#else\n'
      '    std::vector<IR::Inst*> to_invalidate;\n'
      '#endif\n')
# The IR pool's slab kept for the next block (one, of the usual size, per thread).
patch('externals/dynarmic/src/dynarmic/common/memory_pool.cpp',
      'Pool::~Pool() {\n    std::free(current_slab);\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi: one slab of the usual size kept per thread for the next block\'s pool (a block\'s pool lives on one\n'
      '// thread; a slab kept at a thread\'s end is the only memory left behind).\n'
      'namespace {\n'
      'thread_local char* emusuni_kept_slab = nullptr;\n'
      'thread_local size_t emusuni_kept_bytes = 0;\n'
      '}  // namespace\n'
      '#endif\n\n'
      'Pool::~Pool() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_kept_slab == nullptr && current_slab != nullptr) {\n'
      '        emusuni_kept_slab = current_slab;\n'
      '        emusuni_kept_bytes = object_size * slab_size;\n'
      '        current_slab = nullptr;\n'
      '    }\n'
      '#endif\n'
      '    std::free(current_slab);\n')
patch('externals/dynarmic/src/dynarmic/common/memory_pool.cpp',
      'void Pool::AllocateNewSlab() {\n    current_slab = static_cast<char*>(std::malloc(object_size * slab_size));\n',
      'void Pool::AllocateNewSlab() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_kept_slab != nullptr && emusuni_kept_bytes == object_size * slab_size) {\n'
      '        current_slab = emusuni_kept_slab;\n'
      '        emusuni_kept_slab = nullptr;\n'
      '    } else {\n'
      '        current_slab = static_cast<char*>(std::malloc(object_size * slab_size));\n'
      '    }\n'
      '#else\n'
      '    current_slab = static_cast<char*>(std::malloc(object_size * slab_size));\n'
      '#endif\n')
# The ARM decoder's buckets: pointers to the matchers of one list (made once, never changed), not copies.
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      'template<typename Visitor>\nusing ArmDecodeTable = std::array<std::vector<ArmMatcher<Visitor>>, 0x1000>;\n',
      'template<typename Visitor>\n'
      '#ifdef EMUSUNI_PORT\n'
      'using ArmDecodeTable = std::array<std::vector<const ArmMatcher<Visitor>*>, 0x1000>;  // EmuSuNi: pointers\n'
      '#else\n'
      'using ArmDecodeTable = std::array<std::vector<ArmMatcher<Visitor>>, 0x1000>;\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      'template<typename V>\nArmDecodeTable<V> GetArmDecodeTable() {\n    std::vector<ArmMatcher<V>> list = {\n',
      'template<typename V>\nArmDecodeTable<V> GetArmDecodeTable() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    static std::vector<ArmMatcher<V>> list = {  // EmuSuNi: kept (the buckets point into it)\n'
      '#else\n'
      '    std::vector<ArmMatcher<V>> list = {\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      '    ArmDecodeTable<V> table{};\n    for (size_t i = 0; i < table.size(); ++i) {\n        for (auto matcher : list) {\n'
      '            const auto expect = detail::ToFastLookupIndexArm(matcher.GetExpected());\n'
      '            const auto mask = detail::ToFastLookupIndexArm(matcher.GetMask());\n'
      '            if ((i & mask) == expect) {\n'
      '                table[i].push_back(matcher);\n',
      '    ArmDecodeTable<V> table{};\n    for (size_t i = 0; i < table.size(); ++i) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        for (const auto& matcher : list) {\n'
      '#else\n'
      '        for (auto matcher : list) {\n'
      '#endif\n'
      '            const auto expect = detail::ToFastLookupIndexArm(matcher.GetExpected());\n'
      '            const auto mask = detail::ToFastLookupIndexArm(matcher.GetMask());\n'
      '            if ((i & mask) == expect) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                table[i].push_back(&matcher);\n'
      '#else\n'
      '                table[i].push_back(matcher);\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/frontend/A32/decoder/arm.h',
      '    const auto matches_instruction = [instruction](const auto& matcher) { return matcher.Matches(instruction); };\n\n'
      '    const auto& subtable = table[detail::ToFastLookupIndexArm(instruction)];\n'
      '    auto iter = std::find_if(subtable.begin(), subtable.end(), matches_instruction);\n'
      '    return iter != subtable.end() ? std::optional<std::reference_wrapper<const ArmMatcher<V>>>(*iter) : std::nullopt;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const auto matches_instruction = [instruction](const auto* matcher) { return matcher->Matches(instruction); };\n\n'
      '    const auto& subtable = table[detail::ToFastLookupIndexArm(instruction)];\n'
      '    auto iter = std::find_if(subtable.begin(), subtable.end(), matches_instruction);\n'
      '    return iter != subtable.end() ? std::optional<std::reference_wrapper<const ArmMatcher<V>>>(**iter) : std::nullopt;\n'
      '#else\n'
      '    const auto matches_instruction = [instruction](const auto& matcher) { return matcher.Matches(instruction); };\n\n'
      '    const auto& subtable = table[detail::ToFastLookupIndexArm(instruction)];\n'
      '    auto iter = std::find_if(subtable.begin(), subtable.end(), matches_instruction);\n'
      '    return iter != subtable.end() ? std::optional<std::reference_wrapper<const ArmMatcher<V>>>(*iter) : std::nullopt;\n'
      '#endif\n')
# The blocks' ranges: buckets of 4 KiB pages (each block in every page it covers), the same locations returned.
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.h',
      'private:\n    boost::icl::interval_map<ProgramCounterType, std::set<IR::LocationDescriptor>> block_ranges;\n};\n',
      'private:\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: every block\'s range in the bucket of each 4 KiB page it covers (an interval map of sets\n'
      '    // allocated 3 times and split intervals for every block added).\n'
      '    struct EmuSuNiRange {\n'
      '        ProgramCounterType first, last;\n'
      '        IR::LocationDescriptor location;\n'
      '    };\n'
      '    tsl::robin_map<ProgramCounterType, std::vector<EmuSuNiRange>> emusuni_pages;\n'
      '#else\n'
      '    boost::icl::interval_map<ProgramCounterType, std::set<IR::LocationDescriptor>> block_ranges;\n'
      '#endif\n'
      '};\n')
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.h',
      '#include <tsl/robin_set.h>\n',
      '#include <tsl/robin_set.h>\n#ifdef EMUSUNI_PORT\n#include <vector>\n#include <tsl/robin_map.h>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.cpp',
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {\n'
      '    block_ranges.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::ClearCache() {\n'
      '    block_ranges.clear();\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {\n'
      '    if (boost::icl::is_empty(range)) {\n'
      '        return;  // (as the interval map: nothing added)\n'
      '    }\n'
      '    const ProgramCounterType first = boost::icl::first(range), last = boost::icl::last(range);\n'
      '    for (ProgramCounterType page = first >> 12;; page++) {\n'
      '        emusuni_pages[page].push_back(EmuSuNiRange{first, last, location});\n'
      '        if (page == (last >> 12)) {\n'
      '            break;\n'
      '        }\n'
      '    }\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::ClearCache() {\n'
      '    emusuni_pages.clear();\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {\n'
      '    tsl::robin_set<IR::LocationDescriptor> erase_locations;\n'
      '    for (auto invalidate_interval : ranges) {\n'
      '        if (boost::icl::is_empty(invalidate_interval)) {\n'
      '            continue;\n'
      '        }\n'
      '        const ProgramCounterType first = boost::icl::first(invalidate_interval), last = boost::icl::last(invalidate_interval);\n'
      '        // (Pages of the range that hold blocks: each page of the range, or each page held when that is fewer.)\n'
      '        const auto check = [&](const std::vector<EmuSuNiRange>& entries) {\n'
      '            for (const EmuSuNiRange& entry : entries) {\n'
      '                if (entry.first <= last && entry.last >= first) {\n'
      '                    erase_locations.insert(entry.location);\n'
      '                }\n'
      '            }\n'
      '        };\n'
      '        const ProgramCounterType first_page = first >> 12, last_page = last >> 12;\n'
      '        if (static_cast<u64>(last_page - first_page) >= emusuni_pages.size()) {\n'
      '            for (const auto& [page, entries] : emusuni_pages) {\n'
      '                if (page >= first_page && page <= last_page) {\n'
      '                    check(entries);\n'
      '                }\n'
      '            }\n'
      '        } else {\n'
      '            for (ProgramCounterType page = first_page;; page++) {\n'
      '                if (const auto it = emusuni_pages.find(page); it != emusuni_pages.end()) {\n'
      '                    check(it->second);\n'
      '                }\n'
      '                if (page == last_page) {\n'
      '                    break;\n'
      '                }\n'
      '            }\n'
      '        }\n'
      '    }\n'
      '    return erase_locations;\n'
      '}\n'
      '#else\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {\n'
      '    block_ranges.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));\n'
      '}\n\n'
      'template<typename ProgramCounterType>\n'
      'void BlockRangeInformation<ProgramCounterType>::ClearCache() {\n'
      '    block_ranges.clear();\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/backend/block_range_information.cpp',
      '    // TODO: EFFICIENCY: Remove ranges that are to be erased.\n    return erase_locations;\n}\n',
      '    // TODO: EFFICIENCY: Remove ranges that are to be erased.\n    return erase_locations;\n}\n'
      '#endif\n')

# --- 00.69: the JIT warm-up. The 00.68 log: the games' loads and new areas stop for 0.1-0.66 s while dynarmic compiles
# the ARM11's code met for the first time (Super Mario 3D Land: 61,324 blocks in its first 600 frames, 5.1 s; the same
# code at every start), and each of the 2,000-9,000 memory accesses a session finds outside the fast memory costs a
# fault and a recompile. Now both are kept per game, by location only (no code: the game's own code segment is
# compiled again from memory), in saves/3ds/Azahar/shaders/emusuni-jit/<program id>.bin, written every minute while
# they grow (on a worker thread) and when the game ends. At the next start the accesses are marked first and the
# blocks compiled again in the order they were first met, from the first frame on: 0.4 s in the first frame, 12 ms a
# frame in the next 120 (the boot's light frames), then 6 ms a frame, ahead of the game (its boot runs the same code in
# the same order). Only blocks inside the game's code segment (the code that can't change while the game runs: the
# file also keeps the segment's address, size and
# hash, and a file made for other code - an update - is started again), on core 0 (the application core: Super Mario
# 3D Land compiles nothing on core 1, Zelda 8%), and leaving 8 MiB of the 24 MiB code cache free (no flush). The file
# /data/EmuSuNi/3ds-jit-warmup-off (EMUSUNI_3DS_JIT_WARMUP=0 on the PC) turns it off. dynarmic gets four calls: the
# locations it compiled, compile a location now, its "no fast memory here" marks, and add one.
patch('externals/dynarmic/src/dynarmic/interface/A32/a32.h',
      '    /// Debugging: Dump a disassembly all compiled code to the console.\n',
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (citra_libretro/emusuni_jitwarm.h): the location (UniqueHash) of every block compiled since the\n'
      '    /// JIT was made, in order (a recompiled block again).\n'
      '    const std::vector<std::uint64_t>& EmuSuNiCompiledLocations() const;\n'
      '    /// EmuSuNi: the block at this location compiled now, as when it is first run: 1 compiled, 0 there already,\n'
      '    /// -1 not compiled (the code cache keeps 8 MiB free for the game). Not from a callback.\n'
      '    int EmuSuNiPrecompile(std::uint64_t location);\n'
      '    /// EmuSuNi: the memory accesses compiled with the page table because they faulted with the fast memory\n'
      '    /// (block location, IR instruction name), and one added (before its block is compiled).\n'
      '    std::vector<std::pair<std::uint64_t, std::uint32_t>> EmuSuNiFastmemMarkers() const;\n'
      '    void EmuSuNiAddFastmemMarker(std::uint64_t location, std::uint32_t name);\n'
      '#endif\n\n'
      '    /// Debugging: Dump a disassembly all compiled code to the console.\n')
patch('externals/dynarmic/src/dynarmic/interface/A32/a32.h',
      '#include <string>\n#include <vector>\n',
      '#include <string>\n#include <vector>\n#ifdef EMUSUNI_PORT\n#include <utility>\n#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '    A32EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {\n'
      '        auto block = emitter.GetBasicBlock(descriptor);\n'
      '        if (block)\n'
      '            return *block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const ::EmuSuNi::Stats::TickTimer emusuni_timer{::EmuSuNi::Stats::JitTicks};\n'
      '        ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitBlocks]++;\n'
      '#endif\n',
      '    A32EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {\n'
      '        auto block = emitter.GetBasicBlock(descriptor);\n'
      '        if (block)\n'
      '            return *block;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const ::EmuSuNi::Stats::TickTimer emusuni_timer{::EmuSuNi::Stats::JitTicks};\n'
      '        ::EmuSuNi::Stats::counts[::EmuSuNi::Stats::JitBlocks]++;\n'
      '        if (emusuni_compiled.size() < 600000) {\n'
      '            emusuni_compiled.push_back(descriptor.Value());  // EmuSuNi: (the JIT warm-up)\n'
      '        }\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '    A32JitState jit_state;\n    BlockOfCode block_of_code;\n    A32EmitX64 emitter;\n',
      '#ifdef EMUSUNI_PORT\n'
      'public:\n'
      '    // EmuSuNi: the JIT warm-up (citra_libretro/emusuni_jitwarm.h).\n'
      '    std::vector<u64> emusuni_compiled;\n'
      '    int EmuSuNiPrecompile(u64 location) {\n'
      '        const IR::LocationDescriptor descriptor{location};\n'
      '        if (emitter.GetBasicBlock(descriptor)) {\n'
      '            return 0;\n'
      '        }\n'
      '        if (block_of_code.SpaceRemaining() < 8 * 1024 * 1024) {\n'
      '            return -1;\n'
      '        }\n'
      '        GetBasicBlock(descriptor);\n'
      '        return 1;\n'
      '    }\n'
      '    std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const {\n'
      '        return emitter.EmuSuNiFastmemMarkers();\n'
      '    }\n'
      '    void EmuSuNiAddFastmemMarker(u64 location, u32 name) {\n'
      '        emitter.EmuSuNiAddFastmemMarker(location, name);\n'
      '    }\n\n'
      'private:\n'
      '#endif\n'
      '    A32JitState jit_state;\n    BlockOfCode block_of_code;\n    A32EmitX64 emitter;\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      'void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {\n'
      '    impl->InvalidateCacheRange(start_address, length);\n'
      '}\n',
      'void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {\n'
      '    impl->InvalidateCacheRange(start_address, length);\n'
      '}\n'
      '#ifdef EMUSUNI_PORT\n\n'
      'const std::vector<std::uint64_t>& Jit::EmuSuNiCompiledLocations() const {\n'
      '    return impl->emusuni_compiled;\n'
      '}\n\n'
      'int Jit::EmuSuNiPrecompile(std::uint64_t location) {\n'
      '    ASSERT(!is_executing);\n'
      '    return impl->EmuSuNiPrecompile(location);\n'
      '}\n\n'
      'std::vector<std::pair<std::uint64_t, std::uint32_t>> Jit::EmuSuNiFastmemMarkers() const {\n'
      '    return impl->EmuSuNiFastmemMarkers();\n'
      '}\n\n'
      'void Jit::EmuSuNiAddFastmemMarker(std::uint64_t location, std::uint32_t name) {\n'
      '    impl->EmuSuNiAddFastmemMarker(location, name);\n'
      '}\n'
      '#endif\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.h',
      '    std::vector<HostLoc> emusuni_gpr_order;  // EmuSuNi: Emit\'s register order, made once\n#endif\n',
      '    std::vector<HostLoc> emusuni_gpr_order;  // EmuSuNi: Emit\'s register order, made once\n'
      '    // EmuSuNi: the JIT warm-up\'s (citra_libretro/emusuni_jitwarm.h) "no fast memory here" marks.\n'
      '    std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const {\n'
      '        std::vector<std::pair<u64, u32>> markers;\n'
      '        markers.reserve(do_not_fastmem.size());\n'
      '        for (const auto& marker : do_not_fastmem) {\n'
      '            markers.emplace_back(std::get<0>(marker).Value(), static_cast<u32>(std::get<1>(marker)));\n'
      '        }\n'
      '        return markers;\n'
      '    }\n'
      '    void EmuSuNiAddFastmemMarker(u64 location, u32 name) {\n'
      '        do_not_fastmem.insert(DoNotFastmemMarker{IR::LocationDescriptor{location}, name});\n'
      '    }\n'
      '#endif\n')
# Azahar: a core's JIT of its current page table, for the warm-up.
patch('src/core/arm/arm_interface.h',
      '    /// Notify CPU emulation that page tables have changed\n'
      '    virtual void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) = 0;\n',
      '    /// Notify CPU emulation that page tables have changed\n'
      '    virtual void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) = 0;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (citra_libretro/emusuni_jitwarm.h): the JIT warm-up\'s calls on this core\'s JIT of its current\n'
      '    /// page table (dynarmic\'s; none for an interpreter): that page table, the locations it compiled, compile one\n'
      '    /// now (1 compiled, 0 there already, -1 not), its "no fast memory here" marks and add one.\n'
      '    virtual std::shared_ptr<Memory::PageTable> EmuSuNiJitPageTable() const {\n'
      '        return nullptr;\n'
      '    }\n'
      '    virtual const std::vector<u64>* EmuSuNiCompiledLocations() const {\n'
      '        return nullptr;\n'
      '    }\n'
      '    virtual int EmuSuNiPrecompile(u64 location) {\n'
      '        return -1;\n'
      '    }\n'
      '    virtual std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const {\n'
      '        return {};\n'
      '    }\n'
      '    virtual void EmuSuNiAddFastmemMarker(u64 location, u32 name) {}\n'
      '#endif\n')
patch('src/core/arm/arm_interface.h',
      '#include <cstddef>\n#include <memory>\n',
      '#include <cstddef>\n#include <memory>\n#ifdef EMUSUNI_PORT\n#include <utility>\n#include <vector>\n#endif\n')
patch('src/core/arm/dynarmic/arm_dynarmic.h',
      '    bool HasSingleInstructionBreakAccuracy() override {\n        return false;\n    }\n',
      '    bool HasSingleInstructionBreakAccuracy() override {\n        return false;\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::shared_ptr<Memory::PageTable> EmuSuNiJitPageTable() const override {\n'
      '        return jit != nullptr ? current_page_table : nullptr;\n'
      '    }\n'
      '    const std::vector<u64>* EmuSuNiCompiledLocations() const override {\n'
      '        return jit != nullptr ? &jit->EmuSuNiCompiledLocations() : nullptr;\n'
      '    }\n'
      '    int EmuSuNiPrecompile(u64 location) override {\n'
      '        return jit != nullptr ? jit->EmuSuNiPrecompile(location) : -1;\n'
      '    }\n'
      '    std::vector<std::pair<u64, u32>> EmuSuNiFastmemMarkers() const override {\n'
      '        return jit != nullptr ? jit->EmuSuNiFastmemMarkers() : std::vector<std::pair<u64, u32>>{};\n'
      '    }\n'
      '    void EmuSuNiAddFastmemMarker(u64 location, u32 name) override {\n'
      '        if (jit != nullptr) {\n'
      '            jit->EmuSuNiAddFastmemMarker(location, name);\n'
      '        }\n'
      '    }\n'
      '#endif\n')
addfile('src/citra_libretro/emusuni_jitwarm.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the JIT warm-up. The blocks of the game's code segment that dynarmic compiled on
// core 0, and its memory accesses there that faulted with the fast memory, are kept by location in
// <user>/shaders/emusuni-jit/<program id>.bin. At the next start the accesses are marked first (compiled with the page
// table at once: no fault, no recompile) and the blocks compiled again in the order they were first met, from the
// first frame on (0.4 s in the first frame, 12 ms a frame in the next 120, then 6 ms), so that the game's boot and the
// areas met before don't stop to compile them. Written every minute while it grows (on a worker thread) and at the
// game's end.
// A file made for other code (another version, an update) is started again; /data/EmuSuNi/3ds-jit-warmup-off
// (EMUSUNI_3DS_JIT_WARMUP=0 on the PC) turns it all off. Called on the emulation thread only (retro_run,
// retro_unload_game).
#pragma once
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/thread_worker.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace EmuSuNi::JitWarm {

// The file: this header, `count` block locations (u64), then `markers` accesses (u64 block location, u32 IR name,
// u32 0).
struct FileHeader {
    char magic[4];
    std::uint32_t version;
    std::uint64_t program_id;
    std::uint32_t code_addr, code_size;
    std::uint64_t code_hash;
    std::uint32_t count, markers;
};
static_assert(sizeof(FileHeader) == 40);
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxLocations = 200000, kMaxMarkers = 100000;
constexpr std::uint64_t kSingleStepping = std::uint64_t{1} << 34;  // (A32 LocationDescriptor::UniqueHash)

struct State {
    bool started = false, off = false;
    std::uint64_t program_id = 0, code_hash = 0;
    std::uint32_t code_addr = 0, code_size = 0;
    std::string path;
    std::vector<std::uint64_t> loaded;  // the file's, in order
    std::size_t next = 0, compiled = 0, already = 0, saved = 0, saved_markers = 0;
    std::uint64_t spent_us = 0, last_save_ms = 0;
    int frames = 0, tries = 0;
};
inline State g;

inline std::uint64_t NowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

inline bool SwitchedOff() {
#if defined(__ORBIS__)
    return ::access("/data/EmuSuNi/3ds-jit-warmup-off", F_OK) == 0;
#else
    const char* value = std::getenv("EMUSUNI_3DS_JIT_WARMUP");
    return value != nullptr && std::strcmp(value, "0") == 0;
#endif
}

inline bool InCode(std::uint64_t location) {
    const std::uint32_t pc = static_cast<std::uint32_t>(location);
    return location != 0 && (location & kSingleStepping) == 0 && pc >= g.code_addr && pc - g.code_addr < g.code_size;
}

// Core 0, when its JIT is the one of the game process's page table; the process too.
inline Core::ARM_Interface* GameJit(Core::System& system, std::shared_ptr<Kernel::Process>& process) {
    if (system.GetNumCores() == 0) {
        return nullptr;
    }
    process = system.Kernel().GetCurrentProcess();
    if (!process || !process->codeset || process->codeset->CodeSegment().size == 0) {
        return nullptr;
    }
    Core::ARM_Interface& core = system.GetCore(0);
    const std::shared_ptr<Memory::PageTable> table = core.EmuSuNiJitPageTable();
    if (!table || table != process->vm_manager.page_table || core.EmuSuNiCompiledLocations() == nullptr) {
        return nullptr;
    }
    return &core;
}

inline void WriteFile(const std::string& path, const std::string& tmp, const std::vector<std::uint8_t>& data) {
    FileUtil::CreateFullPath(path);
    {
        FileUtil::IOFile file(tmp, "wb");
        if (!file.IsOpen() || file.WriteBytes(data.data(), data.size()) != data.size()) {
            LOG_WARNING(Frontend, "EmuSuNi: JIT warm-up: {} not written", tmp);
            return;
        }
    }
    if (!FileUtil::Rename(tmp, path)) {
        LOG_WARNING(Frontend, "EmuSuNi: JIT warm-up: {} not renamed to {}", tmp, path);
    }
}

inline void Start(Core::System& system) {
    std::shared_ptr<Kernel::Process> process;
    Core::ARM_Interface* core = GameJit(system, process);
    if (core == nullptr) {
        // (Not yet: the game's page table isn't core 0's. Given up after 600 frames.)
        if (++g.tries >= 600) {
            g.started = g.off = true;
        }
        return;
    }
    g.started = true;
    if (SwitchedOff()) {
        g.off = true;
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: off (the file /data/EmuSuNi/3ds-jit-warmup-off)");
        return;
    }
    const auto& code = process->codeset->CodeSegment();
    g.program_id = process->codeset->program_id;
    g.code_addr = code.addr;
    g.code_size = code.size;
    std::vector<std::uint8_t> bytes(code.size);
    system.Memory().ReadBlock(*process, code.addr, bytes.data(), bytes.size());
    g.code_hash = Common::ComputeHash64(bytes.data(), bytes.size());
    g.path = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "emusuni-jit/" +
             fmt::format("{:016X}.bin", g.program_id);
    FileUtil::IOFile file(g.path, "rb");
    if (!file.IsOpen()) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: nothing kept yet for this game ({})", g.path);
        return;
    }
    FileHeader header{};
    const std::uint64_t size = file.GetSize();
    if (size < sizeof(header) || file.ReadBytes(&header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header.magic, "EJW1", 4) != 0 || header.version != kVersion || header.count > kMaxLocations ||
        header.markers > kMaxMarkers ||
        size != sizeof(header) + std::uint64_t{header.count} * 8 + std::uint64_t{header.markers} * 16) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} isn't a list of this version: started again", g.path);
        return;
    }
    if (header.program_id != g.program_id || header.code_addr != g.code_addr || header.code_size != g.code_size ||
        header.code_hash != g.code_hash) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} was made for other code (another version or an update): "
                 "started again", g.path);
        return;
    }
    std::vector<std::uint64_t> locations(header.count);
    std::vector<std::uint64_t> markers(std::size_t{header.markers} * 2);
    if (file.ReadBytes(locations.data(), locations.size() * 8) != locations.size() * 8 ||
        file.ReadBytes(markers.data(), markers.size() * 8) != markers.size() * 8) {
        LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} not read: started again", g.path);
        return;
    }
    // (The marks first: the blocks compiled from now on take the page table for those accesses.)
    std::size_t marked = 0;
    for (std::size_t i = 0; i < header.markers; i++) {
        if (InCode(markers[i * 2])) {
            core->EmuSuNiAddFastmemMarker(markers[i * 2], static_cast<std::uint32_t>(markers[i * 2 + 1]));
            marked++;
        }
    }
    g.loaded = std::move(locations);
    g.saved = g.loaded.size();
    g.saved_markers = header.markers;
    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks of this game's code from earlier sessions to compile, {} of "
             "their memory accesses outside the fast memory ({})", g.loaded.size(), marked, g.path);
}

// The file's locations then this session's, each once, those in the code segment (a small open-addressed set: one
// allocation, not a node per location).
inline std::vector<std::uint64_t> Collect(Core::ARM_Interface& core) {
    const std::vector<std::uint64_t>& compiled = *core.EmuSuNiCompiledLocations();
    std::size_t capacity = 1024;
    while (capacity < 2 * (g.loaded.size() + compiled.size())) {
        capacity *= 2;
    }
    std::vector<std::uint64_t> set(capacity, 0);
    std::vector<std::uint64_t> out;
    out.reserve(std::min(kMaxLocations, g.loaded.size() + compiled.size()));
    const auto add = [&](std::uint64_t location) {
        if (!InCode(location) || out.size() >= kMaxLocations) {
            return;
        }
        std::size_t slot = static_cast<std::size_t>((location * 0x9E3779B97F4A7C15ull) >> 20) & (capacity - 1);
        while (set[slot] != 0) {
            if (set[slot] == location) {
                return;
            }
            slot = (slot + 1) & (capacity - 1);
        }
        set[slot] = location;
        out.push_back(location);
    };
    for (const std::uint64_t location : g.loaded) {
        add(location);
    }
    for (const std::uint64_t location : compiled) {
        add(location);
    }
    return out;
}

inline Common::ThreadWorker& Writer() {
    // (Made at the first save, never destroyed: the app may end with a write queued.)
    static Common::ThreadWorker* worker = new Common::ThreadWorker(1, "EmuSuNi JIT warm-up");
    return *worker;
}

inline void Save(Core::System& system, bool at_end) {
    if (g.off || !g.started || g.path.empty()) {
        return;
    }
    std::shared_ptr<Kernel::Process> process;
    Core::ARM_Interface* core = GameJit(system, process);
    if (core == nullptr || process->codeset->program_id != g.program_id) {
        return;
    }
    std::vector<std::uint64_t> locations = Collect(*core);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> markers = core->EmuSuNiFastmemMarkers();
    markers.erase(std::remove_if(markers.begin(), markers.end(), [](const auto& m) { return !InCode(m.first); }),
                  markers.end());
    if (markers.size() > kMaxMarkers) {
        markers.resize(kMaxMarkers);
    }
    // (Every minute only when the blocks grew by 1000 or more; at the end when anything grew.)
    const bool grew = locations.size() > g.saved || markers.size() > g.saved_markers;
    if (!grew || (!at_end && locations.size() < g.saved + 1000)) {
        return;
    }
    FileHeader header{};
    std::memcpy(header.magic, "EJW1", 4);
    header.version = kVersion;
    header.program_id = g.program_id;
    header.code_addr = g.code_addr;
    header.code_size = g.code_size;
    header.code_hash = g.code_hash;
    header.count = static_cast<std::uint32_t>(locations.size());
    header.markers = static_cast<std::uint32_t>(markers.size());
    auto data = std::make_shared<std::vector<std::uint8_t>>(sizeof(header) + locations.size() * 8 +
                                                            markers.size() * 16);
    std::uint8_t* out = data->data();
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    std::memcpy(out, locations.data(), locations.size() * 8);
    out += locations.size() * 8;
    for (const auto& [location, name] : markers) {
        const std::uint64_t record[2] = {location, name};
        std::memcpy(out, record, 16);
        out += 16;
    }
    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks and {} accesses kept for the next session (+{} blocks, {} KiB)",
             locations.size(), markers.size(), locations.size() - std::min(locations.size(), g.saved),
             data->size() / 1024);
    g.saved = locations.size();
    g.saved_markers = markers.size();
    if (at_end) {
        WriteFile(g.path, g.path + ".tmp", *data);
    } else {
        const std::string path = g.path;
        Writer().QueueWork([path, data] { WriteFile(path, path + ".w.tmp", *data); });
    }
}

// retro_run, before the frame's emulation.
inline void BeforeFrame(Core::System& system) {
    if (!g.started) {
        Start(system);
        if (!g.started) {
            return;
        }
    }
    if (g.off) {
        return;
    }
    if (g.next < g.loaded.size()) {
        std::shared_ptr<Kernel::Process> process;
        Core::ARM_Interface* core = GameJit(system, process);
        if (core != nullptr) {
            const std::uint64_t budget_us = g.frames == 0 ? 400000 : g.frames < 120 ? 12000 : 6000;
            const std::uint64_t t0 = NowUs();
            while (g.next < g.loaded.size()) {
                const std::uint64_t location = g.loaded[g.next++];
                if (!InCode(location)) {
                    continue;
                }
                const int result = core->EmuSuNiPrecompile(location);
                if (result < 0) {
                    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: the code cache has no more room to spare: {} blocks "
                             "left for the game to compile when it meets them", g.loaded.size() - g.next + 1);
                    g.next = g.loaded.size();
                    break;
                }
                (result > 0 ? g.compiled : g.already)++;
                if ((g.next & 15) == 0 && NowUs() - t0 >= budget_us) {
                    break;
                }
            }
            g.spent_us += NowUs() - t0;
            g.frames++;
        }
        if (g.next >= g.loaded.size()) {
            LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks compiled in {} ms over {} frames ({} there already)",
                     g.compiled, g.spent_us / 1000, g.frames, g.already);
        }
    }
    const std::uint64_t now_ms = NowUs() / 1000;
    if (g.last_save_ms == 0) {
        g.last_save_ms = now_ms;
    } else if (now_ms - g.last_save_ms >= 60000) {
        g.last_save_ms = now_ms;
        Save(system, false);
    }
}

// retro_unload_game, before the system shuts down: the list written now; the state made new for the next game.
inline void End(Core::System& system) {
    Save(system, true);
    g = State{};
}

} // namespace EmuSuNi::JitWarm
#endif
''')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "common/emusuni_frame_stats.h"\n#include "common/emusuni_stats.h"\n',
      '#include "common/emusuni_frame_stats.h"\n#include "common/emusuni_stats.h"\n#include "citra_libretro/emusuni_jitwarm.h"\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n    try {\n#endif\n',
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n    try {\n'
      '    // EmuSuNi: the JIT warm-up (citra_libretro/emusuni_jitwarm.h): earlier sessions\' blocks compiled ahead.\n'
      '    EmuSuNi::JitWarm::BeforeFrame(Core::System::GetInstance());\n'
      '#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      'void retro_unload_game() {\n    LOG_DEBUG(Frontend, "Unloading game...");\n    Core::System::GetInstance().Shutdown();\n}\n',
      'void retro_unload_game() {\n    LOG_DEBUG(Frontend, "Unloading game...");\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::JitWarm::End(Core::System::GetInstance());  // EmuSuNi: (citra_libretro/emusuni_jitwarm.h)\n'
      '#endif\n'
      '    Core::System::GetInstance().Shutdown();\n}\n')

# --- 00.69: the games' file reads and writes on two threads made once. Azahar's RunAsync made a new thread
# (std::async) for every file request the FS cache doesn't answer: 1,449 threads in a Driver Renegade 3D session of
# the 00.68 log (758 in 3,000 frames on this PC), each a thread creation on the emulation thread and the hook's
# placing. Now the FS service's files and directories (file.cpp, directory.cpp) use RunOnThreadWorker, Azahar's own
# way for the same thing (FS_USER's worker), with one worker of two threads ("HleAsync"); the other services' requests
# (sockets, which may block for long, HTTP, AM) keep their threads. std::async's future waited for its request when
# destroyed (with the system); the worker's don't: System::Shutdown waits for the worker's requests first.
addfile('src/core/hle/service/fs/emusuni_fs_worker.h', r'''// EmuSuNi (scripts/build-3ds-core.sh): the FS service's files' and directories' asynchronous work (reads, writes,
// flushes, lists) on two threads made at the first request and kept (never destroyed: a request may be queued when the
// app ends), instead of a thread per request (RunAsync's std::async).
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include "common/thread_worker.h"

namespace Service::FS {
inline std::atomic<Common::ThreadWorker*> emusuni_file_worker{nullptr};

inline Common::ThreadWorker& EmuSuNiFileWorker() {
    static Common::ThreadWorker* const worker = [] {
        Common::ThreadWorker* made = new Common::ThreadWorker(2, "HleAsync");
        emusuni_file_worker.store(made);
        return made;
    }();
    return *worker;
}

/// The worker's requests finished (System::Shutdown, first: a request's context and client thread go with the
/// system; RunAsync's std::async future waited for its request when destroyed).
inline void EmuSuNiFileWorkerWait() {
    if (Common::ThreadWorker* worker = emusuni_file_worker.load()) {
        worker->WaitForRequests();
    }
}
} // namespace Service::FS

// The two files' ctx.RunAsync(...) calls become ctx.RunOnThreadWorker(that worker, ...): the same request, the same
// result callback on the emulation thread, the client thread asleep meanwhile. (Not where only the wait is wanted.)
#ifndef EMUSUNI_FS_WORKER_WAIT_ONLY
#define RunAsync(...) RunOnThreadWorker(::Service::FS::EmuSuNiFileWorker(), __VA_ARGS__)
#endif
#endif
''')
patch('src/core/hle/service/fs/file.cpp',
      '#include "core/hle/service/fs/file.h"\n',
      '#include "core/hle/service/fs/file.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "core/hle/service/fs/emusuni_fs_worker.h" // (last: it renames RunAsync)\n#endif\n')
patch('src/core/hle/service/fs/directory.cpp',
      '#include "core/hle/service/fs/directory.h"\n',
      '#include "core/hle/service/fs/directory.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "core/hle/service/fs/emusuni_fs_worker.h" // (last: it renames RunAsync)\n#endif\n')
patch('src/core/core.cpp',
      '#include "core/hle/service/sm/sm.h"\n',
      '#include "core/hle/service/sm/sm.h"\n'
      '#ifdef EMUSUNI_PORT\n#define EMUSUNI_FS_WORKER_WAIT_ONLY\n#include "core/hle/service/fs/emusuni_fs_worker.h"\n#endif\n')
patch('src/core/core.cpp',
      'void System::Shutdown(bool is_deserializing) {\n',
      'void System::Shutdown(bool is_deserializing) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the FS service\'s file requests on their worker finished first (service/fs/emusuni_fs_worker.h).\n'
      '    Service::FS::EmuSuNiFileWorkerWait();\n'
      '#endif\n')

# --- 00.69: textures decoded into ordinary memory, then copied to the staging buffer in one pass. The staging buffer
# is write-combined memory on the PS4 (garlic: the radv winsys maps every HOST_VISIBLE|HOST_COHERENT buffer that
# isn't HOST_CACHED so), and the tiled decoders write it 32 bytes of a row at a time, eight rows apart (an 8x8 tile),
# which write-combining memory takes badly (each partial line goes out on its own); Driver Renegade 3D's loads decoded
# 58 MB of textures in 364 ms, once 4.4 MB in 270 ms. The decode now writes a scratch buffer (cached; the emulation
# thread's own, grown to the largest texture and kept) and one memcpy streams it to the staging buffer. The same bytes.
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '#include <type_traits>\n#include <boost/container/small_vector.hpp>\n',
      '#include <type_traits>\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#endif\n'
      '#include <boost/container/small_vector.hpp>\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '    DecodeTexture(load_info, load_info.addr, load_info.end, upload_data, staging.mapped,\n'
      '                  runtime.NeedsConversion(surface));\n',
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        // EmuSuNi: decoded into the thread\'s scratch buffer (cached memory), then one sequential copy into the\n'
      '        // staging buffer (write-combined on the PS4: scripts/build-3ds-core.sh). Kept for the thread\'s life (the\n'
      '        // emulation thread: the app\'s main thread).\n'
      '        static thread_local u8* emusuni_scratch = nullptr;\n'
      '        static thread_local std::size_t emusuni_scratch_size = 0;\n'
      '        std::span<u8> emusuni_decode_to = staging.mapped;\n'
      '        if (staging.mapped.size() <= std::size_t{64} << 20) {\n'
      '            if (emusuni_scratch_size < staging.mapped.size()) {\n'
      '                u8* grown = static_cast<u8*>(std::realloc(emusuni_scratch, staging.mapped.size()));\n'
      '                if (grown != nullptr) {\n'
      '                    emusuni_scratch = grown;\n'
      '                    emusuni_scratch_size = staging.mapped.size();\n'
      '                }\n'
      '            }\n'
      '            if (emusuni_scratch_size >= staging.mapped.size()) {\n'
      '                emusuni_decode_to = std::span<u8>{emusuni_scratch, staging.mapped.size()};\n'
      '            }\n'
      '        }\n'
      '        DecodeTexture(load_info, load_info.addr, load_info.end, upload_data, emusuni_decode_to,\n'
      '                      runtime.NeedsConversion(surface));\n'
      '        if (emusuni_decode_to.data() != staging.mapped.data()) {\n'
      '            std::memcpy(staging.mapped.data(), emusuni_decode_to.data(), emusuni_decode_to.size());\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    DecodeTexture(load_info, load_info.addr, load_info.end, upload_data, staging.mapped,\n'
      '                  runtime.NeedsConversion(surface));\n'
      '#endif\n')

# --- 00.69: Xbyak's labels without a heap allocation each. A block's code takes ~5 labels (its condition, its
# terminal's links, each memory access's slow path); each was 2-3 heap allocations (nodes of the label manager's hash
# tables: defined labels, the labels' list, jumps waiting for one) - 10 of the 23 a block still made. The three tables'
# nodes now come from free lists of the thread (16-byte size classes up to 128; the tables' bucket arrays as before).
# The same labels, the same code. In Xbyak itself (externals/xbyak, used by dynarmic and the PICA shader JIT alike).
patch('externals/xbyak/xbyak/xbyak.h',
      'class LabelManager;\n\nclass Label {\n',
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (scripts/build-3ds-core.sh): the label tables\' nodes from free lists of the thread (a code generator is\n'
      '// used by one thread at a time; a node freed on another thread goes to that one\'s list). Nodes stay in the lists\n'
      '// (the most a thread had at once); bigger or array allocations as before.\n'
      'namespace emusuni {\n'
      'struct NodeLists {\n'
      '\tvoid *head[8];\n'
      '};\n'
      'inline NodeLists& Lists()\n'
      '{\n'
      '\tstatic thread_local NodeLists lists = {};\n'
      '\treturn lists;\n'
      '}\n'
      'template<class T>\n'
      'struct NodeAlloc {\n'
      '\ttypedef T value_type;\n'
      '\tNodeAlloc() noexcept {}\n'
      '\ttemplate<class U> NodeAlloc(const NodeAlloc<U>&) noexcept {}\n'
      '\tstatic constexpr bool pooled = sizeof(T) <= 128 && alignof(T) <= 16;\n'
      '\tstatic constexpr size_t size_class = (sizeof(T) + 15) / 16 - 1;\n'
      '\tT *allocate(size_t n)\n'
      '\t{\n'
      '\t\tif (pooled && n == 1) {\n'
      '\t\t\tvoid *&head = Lists().head[size_class];\n'
      '\t\t\tif (head) {\n'
      '\t\t\t\tvoid *p = head;\n'
      '\t\t\t\thead = *static_cast<void **>(p);\n'
      '\t\t\t\treturn static_cast<T *>(p);\n'
      '\t\t\t}\n'
      '\t\t\treturn static_cast<T *>(::operator new((size_class + 1) * 16));\n'
      '\t\t}\n'
      '\t\treturn static_cast<T *>(::operator new(n * sizeof(T)));\n'
      '\t}\n'
      '\tvoid deallocate(T *p, size_t n) noexcept\n'
      '\t{\n'
      '\t\tif (pooled && n == 1) {\n'
      '\t\t\tvoid *&head = Lists().head[size_class];\n'
      '\t\t\t*reinterpret_cast<void **>(p) = head;\n'
      '\t\t\thead = p;\n'
      '\t\t\treturn;\n'
      '\t\t}\n'
      '\t\t::operator delete(p);\n'
      '\t}\n'
      '\ttemplate<class U> bool operator==(const NodeAlloc<U>&) const noexcept { return true; }\n'
      '\ttemplate<class U> bool operator!=(const NodeAlloc<U>&) const noexcept { return false; }\n'
      '};\n'
      '} // emusuni\n'
      '#endif\n\n'
      'class LabelManager;\n\nclass Label {\n')
patch('externals/xbyak/xbyak/xbyak.h',
      '\ttypedef XBYAK_STD_UNORDERED_MAP<int, ClabelVal> ClabelDefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_MULTIMAP<int, const JmpLabel> ClabelUndefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_SET<Label*> LabelPtrList;\n',
      '#ifdef EMUSUNI_PORT\n'
      '\ttypedef std::unordered_map<int, ClabelVal, std::hash<int>, std::equal_to<int>, emusuni::NodeAlloc<std::pair<const int, ClabelVal> > > ClabelDefList;\n'
      '\ttypedef std::unordered_multimap<int, const JmpLabel, std::hash<int>, std::equal_to<int>, emusuni::NodeAlloc<std::pair<const int, const JmpLabel> > > ClabelUndefList;\n'
      '\ttypedef std::unordered_set<Label*, std::hash<Label*>, std::equal_to<Label*>, emusuni::NodeAlloc<Label*> > LabelPtrList;\n'
      '#else\n'
      '\ttypedef XBYAK_STD_UNORDERED_MAP<int, ClabelVal> ClabelDefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_MULTIMAP<int, const JmpLabel> ClabelUndefList;\n'
      '\ttypedef XBYAK_STD_UNORDERED_SET<Label*> LabelPtrList;\n'
      '#endif\n')

# --- 00.69: ETC1 textures decoded a 4x4 subtile at a time (driver Renegade 3D: 00.68's log decoded 4.4 MB of textures
# in one 270 ms frame, mostly ETC1). 00.89: upstream's faster texture codec in its place (azahar PR #2564, merged
# 2026-09-20, after 2126.2): ETC1's colours worked out once a block, 8 pixels at a time in SSSE3 (CITRA_HAS_SSE42, the
# PS4's Jaguar has it), the Morton order in SIMD, the tile bounds checked once a tile. This PC, a 256x256 texture,
# 00.69's codec against it: ETC1 and ETC1A4 0.28 -> 0.055 ms, I4/A4/IA4 0.05-0.06 -> 0.006-0.010 ms, the converted
# RGBA4/RGB5A1/RGB565 0.12-0.15 -> 0.009-0.018 ms, every format's encoders as fast or faster; the same bytes out of
# every decoder and encoder (random textures, whole and in part). Its files as upstream has them
# (scripts/3ds-azahar/texture_codec.h and unroll.h, from that commit); MakeInt is LoadFromBytes there.
upstream('src/video_core/rasterizer_cache/texture_codec.h',
         '572e590c806bd228127c092596c7a852273dd47eb4f44550f0a7b3003b38c7d1')
upstream('src/common/unroll.h')
patch('src/video_core/renderer_vulkan/vk_texture_runtime.cpp',
      '            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);\n            const u32 d24 = d24s8 >> 8;\n',
      '            const u32 d24s8 = VideoCore::LoadFromBytes<u32>(ptr);\n            const u32 d24 = d24s8 >> 8;\n')
patch('src/video_core/renderer_vulkan/vk_texture_runtime.cpp',
      '            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);\n'
      '            const float d32 = (d24s8 >> 8) / 16777215.f;\n',
      '            const u32 d24s8 = VideoCore::LoadFromBytes<u32>(ptr);\n'
      '            const float d32 = (d24s8 >> 8) / 16777215.f;\n')

# --- 00.70: the 3DS CPU Clock after a save state's load. A state brings the core timing's timers back made anew, their
# clock scale at 100% (upstream doesn't keep it in the state and never applied the setting again): a game continued
# from a state (the app's Continue, a state loaded from the menu) ran at 100% whatever 3DS CPU Clock said.
patch('src/core/savestate.cpp',
      '    // Deserialize\n    iarchive ia{stream};\n    ia&* this;\n',
      '    // Deserialize\n    iarchive ia{stream};\n    ia&* this;\n'
      '    // EmuSuNi: the CPU clock again (the timers come back made anew, at 100%).\n'
      '    if (timing) {\n'
      '        timing->UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());\n'
      '    }\n')
patch('src/core/savestate.cpp',
      '#include "core/core.h"\n',
      '#include "core/core.h"\n#include "core/core_timing.h"\n')
# --- 00.70: less work per draw on the emulation thread (the GPU's commands were 40-45% of it in Hey! Pikmin's busy
# scenes on the PS4: 230-450 draws a frame, ~20 us each). The same Vulkan commands and descriptors, fewer made:
#   - a draw's texture and utility descriptor sets: upstream took a new set from the pool and wrote its 3-4 images for
#     every draw (930 descriptor writes a frame in Pikmin); now a draw whose writes are the same as the last draw's, in
#     the same submission (its set can't have gone back to the pool before that submission ran), binds that set again;
#   - the pipeline: upstream hashed the draw's whole pipeline state twice and looked both hashes up for every draw;
#     now a state equal to the last draw's (byte for byte) gives its pipeline at once;
#   - two short loops that clang (-march=btver2, with the PGO profile's small trip counts) vectorized with masked
#     loads and stores (vmaskmovps: microcoded on the PS4's Jaguar): the command lists' runs of registers, the fixed
#     vertex attributes' layout.
patch('src/video_core/renderer_vulkan/vk_rasterizer.h',
      '    /// Binds a texture cube to texture unit 0\n'
      '    void BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture,\n'
      '                         vk::DescriptorSet texture_set);\n',
      '    /// Binds a texture cube to texture unit 0\n'
      '    void BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture,\n'
      '                         vk::DescriptorSet texture_set);\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a descriptor set\'s image writes, gathered for a draw (SyncTextureUnits, SyncUtilityTextures),\n'
      '    // and the last set made from them: the same writes in the same submission bind it again (EmuSuNiCommitSet).\n'
      '    struct EmuSuNiImageWrite {\n'
      '        vk::ImageView view{};\n'
      '        vk::Sampler sampler{};\n'
      '        u8 binding = 0;\n'
      '        u8 array_index = 0;\n'
      '        bool storage = false;\n'
      '        bool operator==(const EmuSuNiImageWrite&) const = default;\n'
      '    };\n'
      '    struct EmuSuNiSetCache {\n'
      '        std::array<EmuSuNiImageWrite, 12> writes{};\n'
      '        u32 count = 0;\n'
      '        u64 tick = ~0ULL;\n'
      '        vk::DescriptorSet set{};\n'
      '    };\n'
      '    void EmuSuNiAddImage(u8 binding, u8 array_index, vk::ImageView view, vk::Sampler sampler, bool storage);\n'
      '    void EmuSuNiCommitSet(EmuSuNiSetCache& cache, DescriptorHeapType type);\n'
      '    std::array<EmuSuNiImageWrite, 12> emusuni_writes{};\n'
      '    u32 emusuni_write_count = 0;\n'
      '    EmuSuNiSetCache emusuni_texture_set;\n'
      '    EmuSuNiSetCache emusuni_utility_set;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    /// Sets the dynamic offset for the uniform buffer at binding\n',
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi: binds a descriptor set taken earlier in this submission again (RasterizerVulkan::EmuSuNiCommitSet).\n'
      '    void EmuSuNiRebind(DescriptorHeapType type, vk::DescriptorSet descriptor_set) {\n'
      '        bound_descriptor_sets[static_cast<u32>(type)] = descriptor_set;\n'
      '    }\n'
      '#endif\n'
      '\n'
      '    /// Sets the dynamic offset for the uniform buffer at binding\n')
# The texture units and the utility textures: the same surfaces, samplers and copies as upstream in the same order;
# only their descriptor writes are gathered, then committed (EmuSuNiCommitSet). (Upstream's two functions kept for
# the other builds, after #else.)
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {\n',
      r'''#ifdef EMUSUNI_PORT
void RasterizerVulkan::EmuSuNiAddImage(u8 binding, u8 array_index, vk::ImageView view, vk::Sampler sampler,
                                       bool storage) {
    if (emusuni_write_count < emusuni_writes.size()) {
        emusuni_writes[emusuni_write_count++] = {view, sampler, binding, array_index, storage};
    }
}

void RasterizerVulkan::EmuSuNiCommitSet(EmuSuNiSetCache& cache, DescriptorHeapType type) {
    // The set made for an earlier draw of this submission (taken from the pool at this tick, it goes back only once
    // the GPU has run the submission) with the same writes: bound again, nothing written.
    const u64 tick = scheduler.CurrentTick();
    if (cache.set && cache.tick == tick && cache.count == emusuni_write_count &&
        std::equal(emusuni_writes.begin(), emusuni_writes.begin() + emusuni_write_count, cache.writes.begin())) {
        pipeline_cache.EmuSuNiRebind(type, cache.set);
        return;
    }
    const vk::DescriptorSet set = pipeline_cache.Acquire(type);
    for (u32 i = 0; i < emusuni_write_count; i++) {
        const EmuSuNiImageWrite& write = emusuni_writes[i];
        if (write.storage) {
            update_queue.AddStorageImage(set, write.binding, write.view);
        } else {
            update_queue.AddImageSampler(set, write.binding, write.array_index, write.view, write.sampler);
        }
    }
    std::copy(emusuni_writes.begin(), emusuni_writes.begin() + emusuni_write_count, cache.writes.begin());
    cache.count = emusuni_write_count;
    cache.tick = tick;
    cache.set = set;
}

void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
    using CubeFace = Pica::TexturingRegs::CubeFace;

    const auto pica_textures = regs.texturing.GetTextures();
    emusuni_write_count = 0;

    for (u32 texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        // If the texture unit is disabled bind a null surface to it
        if (!texture.enabled) {
            switch (texture.config.type.Value()) {
            case TextureType::TextureCube:
            case TextureType::ShadowCube: {
                Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_CUBE_ID);
                const Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SURFACE_CUBE_ID);
                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, null_surface.ImageView(),
                                null_sampler.Handle(), false);
                break;
            }
            default: {
                Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
                const Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SURFACE_ID);
                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, null_surface.ImageView(),
                                null_sampler.Handle(), false);
                break;
            }
            }
            continue;
        }

        // Handle special tex0 configurations
        if (texture_index == 0) {
            switch (texture.config.type.Value()) {
            case TextureType::Shadow2D: {
                Surface& surface = res_cache.GetTextureSurface(texture);
                Sampler& sampler = res_cache.GetSampler(texture.config);
                surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, surface.StorageView(), sampler.Handle(), false);
                continue;
            }
            case TextureType::ShadowCube: {
                // (BindShadowCube's work, its writes gathered)
                auto info = Pica::Texture::TextureInfo::FromPicaRegister(texture.config, texture.format);
                constexpr std::array faces = {
                    CubeFace::PositiveX, CubeFace::NegativeX, CubeFace::PositiveY,
                    CubeFace::NegativeY, CubeFace::PositiveZ, CubeFace::NegativeZ,
                };
                Sampler& sampler = res_cache.GetSampler(texture.config);
                for (CubeFace face : faces) {
                    const u32 binding = static_cast<u32>(face);
                    info.physical_address = regs.texturing.GetCubePhysicalAddress(face);
                    const VideoCore::SurfaceId surface_id = res_cache.GetTextureSurface(info);
                    Surface& surface = res_cache.GetSurface(surface_id);
                    surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
                    EmuSuNiAddImage(0, static_cast<u8>(binding), surface.StorageView(), sampler.Handle(), false);
                }
                continue;
            }
            case TextureType::TextureCube: {
                // (BindTextureCube's work, its write gathered)
                const VideoCore::TextureCubeConfig config = {
                    .px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX),
                    .nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX),
                    .py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY),
                    .ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY),
                    .pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ),
                    .nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ),
                    .width = texture.config.width,
                    .levels = texture.config.lod.max_level + 1,
                    .format = texture.format,
                };
                Surface& surface = res_cache.GetTextureCube(config);
                Sampler& sampler = res_cache.GetSampler(texture.config);
                EmuSuNiAddImage(0, 0, surface.ImageView(), sampler.Handle(), false);
                continue;
            }
            default:
                break;
            }
        }

        // Bind the texture provided by the rasterizer cache
        Surface& surface = res_cache.GetTextureSurface(texture);
        Sampler& sampler = res_cache.GetSampler(texture.config);
        const vk::ImageView color_view = framebuffer->ImageView(SurfaceType::Color);
        const bool is_feedback_loop = color_view == surface.FramebufferView();
        const vk::ImageView texture_view =
            is_feedback_loop ? surface.CopyImageView() : surface.ImageView();
        EmuSuNiAddImage(static_cast<u8>(texture_index), 0, texture_view, sampler.Handle(), false);
    }
    EmuSuNiCommitSet(emusuni_texture_set, DescriptorHeapType::Texture);
}

void RasterizerVulkan::SyncUtilityTextures(const Framebuffer* framebuffer) {
    const bool shadow_writing = regs.framebuffer.IsShadowRendering();
    bool shadow_reading = regs.lighting.config0.enable_shadow;
    // Ensure the shadow-texture slot is actually enabled
    if (shadow_reading) {
        const u32 shadow_texture_unit = regs.lighting.config0.shadow_selector.Value();
        const auto shadow_texture = regs.texturing.GetTextures()[shadow_texture_unit];
        shadow_reading &= shadow_texture.enabled;
    }

    // Reading and writing are mutually exclusive
    assert(!(shadow_writing && shadow_reading));

    emusuni_write_count = 0;
    if (shadow_writing) {
        EmuSuNiAddImage(0, 0, framebuffer->ImageView(SurfaceType::Color), {}, true);
    } else if (shadow_reading) {
        const u32 shadow_texture_unit = regs.lighting.config0.shadow_selector.Value();
        const auto shadow_texture = regs.texturing.GetTextures()[shadow_texture_unit];
        Surface& shadow_surface = res_cache.GetTextureSurface(shadow_texture);
        EmuSuNiAddImage(0, 0, shadow_surface.StorageView(), {}, true);
    } else {
        Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
        EmuSuNiAddImage(0, 0, null_surface.StorageView(), {}, true);
    }
    EmuSuNiCommitSet(emusuni_utility_set, DescriptorHeapType::Utility);
}
#else
void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        update_queue.AddStorageImage(utility_set, 0, null_surface.StorageView());\n'
      '    }\n'
      '}\n'
      '\n'
      'void RasterizerVulkan::BindShadowCube(',
      '        update_queue.AddStorageImage(utility_set, 0, null_surface.StorageView());\n'
      '    }\n'
      '}\n'
      '#endif\n'
      '\n'
      'void RasterizerVulkan::BindShadowCube(')
# The pipeline of the last draw's state.
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      'GraphicsPipeline* ShaderDiskCache::GetPipeline(const PipelineInfo& info) {\n',
      'GraphicsPipeline* ShaderDiskCache::GetPipeline(const PipelineInfo& info) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the same state as the last draw\'s (byte for byte: its two hashes are the same): its pipeline.\n'
      '    if (emusuni_last_pipeline != nullptr &&\n'
      '        std::memcmp(&emusuni_last_state, &info.state, sizeof(info.state)) == 0) {\n'
      '        return emusuni_last_pipeline;\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      '        AppendPLConfig(pl_cache, entry, hash);\n'
      '    }\n'
      '\n'
      '    return it.value().get();\n'
      '}\n',
      '        AppendPLConfig(pl_cache, entry, hash);\n'
      '    }\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::memcpy(&emusuni_last_state, &info.state, sizeof(info.state));\n'
      '    emusuni_last_pipeline = it.value().get();\n'
      '#endif\n'
      '    return it.value().get();\n'
      '}\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.h',
      '    std::unordered_set<u64> known_graphic_pipelines;\n',
      '    std::unordered_set<u64> known_graphic_pipelines;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the last state GetPipeline was asked for, and its pipeline (owned by graphics_pipelines).\n'
      '    StaticPipelineInfo emusuni_last_state{};\n'
      '    GraphicsPipeline* emusuni_last_pipeline = nullptr;\n'
      '#endif\n')
# No masked moves (vmaskmovps) in two loops of every draw on the Jaguar.
patch('src/video_core/pica/pica_core.cpp',
      '            u32* __restrict batch_dst = dst + offset;\n'
      '            const u32* __restrict batch_src = values + offset;\n'
      '            for (u32 i = 0; i < batch_count; ++i) {\n',
      '            u32* __restrict batch_dst = dst + offset;\n'
      '            const u32* __restrict batch_src = values + offset;\n'
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi: a few registers at a time; clang folded the loop\'s end into masked moves (vmaskmovps,\n'
      '            // microcoded on the PS4\'s Jaguar).\n'
      '#pragma clang loop vectorize(disable)\n'
      '#endif\n'
      '            for (u32 i = 0; i < batch_count; ++i) {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    // Loop one more time to find unused attributes and assign them to the default one\n'
      '    // If the attribute is just disabled, shove the default attribute to avoid\n'
      '    // errors if the shader ever decides to use it.\n'
      '    for (u32 i = 0; i < 16; i++) {\n',
      '    // Loop one more time to find unused attributes and assign them to the default one\n'
      '    // If the attribute is just disabled, shove the default attribute to avoid\n'
      '    // errors if the shader ever decides to use it.\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (EmuSuNi: not vectorized - its conditional stores were masked moves, microcoded on the Jaguar.)\n'
      '#pragma clang loop vectorize(disable)\n'
      '#endif\n'
      '    for (u32 i = 0; i < 16; i++) {\n')
# --- 00.70: the stall log divides the GPU's commands, and counts the game's own pictures. 00.69's log (Hey! Pikmin's
# busy scenes on the PS4): "GPU commands" 3.6-5.6 s of the emulation thread's 600 frames, as much as the guest's code;
# on the PC the same scenes make 230 draws a frame, a few microseconds each, and the game draws 30 pictures a second
# (every other frame). Now each slow frame and each 600-frame line say how many draws (and vertices) the GPU's
# commands made and the time the draws took of them (the rest: the command lists' registers), the command lists'
# size, the draws whose vertices the CPU had to shade (none in the games tried), and how many pictures the game drew
# (its top screen's buffer swaps: 300 in 600 frames is a 30 FPS game).
patch('src/common/emusuni_stats.h',
      'enum TickCounter { GuestTicks, JitTicks, SvcTicks, GpuTicks, GpuInSvcTicks, DspTicks, SurfaceTicks, TickCount };\n'
      'enum CountCounter { JitBlocks, JitFlushes, SvcCalls, Surfaces, DrawsSkipped, CountCount };\n',
      'enum TickCounter {\n'
      '    GuestTicks, JitTicks, SvcTicks, GpuTicks, GpuInSvcTicks, DspTicks, SurfaceTicks,\n'
      '    DrawTicks, // (00.70) the draws (PicaCore::DrawArrays: the vertices set up, the draw recorded)\n'
      '    TickCount\n'
      '};\n'
      'enum CountCounter {\n'
      '    JitBlocks, JitFlushes, SvcCalls, Surfaces, DrawsSkipped,\n'
      '    Draws, DrawVertices, SoftwareDraws, ListBytes, GamePictures, // (00.70)\n'
      '    CountCount\n'
      '};\n')
patch('src/common/emusuni_frame_stats.h',
      '        const std::uint64_t hle = svc > gpu_in_svc ? svc - gpu_in_svc : 0;\n',
      '        const std::uint64_t hle = svc > gpu_in_svc ? svc - gpu_in_svc : 0;\n'
      '        const std::string on_cpu =\n'
      '            dc[SoftwareDraws] != 0 ? fmt::format(", {} of them shaded on the CPU", dc[SoftwareDraws]) : std::string();\n')
patch('src/common/emusuni_frame_stats.h',
      '                           "calls), GPU commands {} ms (surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "\n',
      '                           "calls), GPU commands {} ms ({} draws of {} vertices{}, {} ms of it the draws; {} KiB of "\n'
      '                           "command lists; surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "\n')
patch('src/common/emusuni_frame_stats.h',
      '                           "({} ms) and {} KiB elsewhere ({} ms)",\n',
      '                           "({} ms) and {} KiB elsewhere ({} ms); the game drew {} pictures",\n')
patch('src/common/emusuni_frame_stats.h',
      '                           t_ms(dt[GpuTicks]), dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),\n',
      '                           t_ms(dt[GpuTicks]), dc[Draws], dc[DrawVertices], on_cpu, t_ms(dt[DrawTicks]),\n'
      '                           dc[ListBytes] / 1024, dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),\n')
patch('src/common/emusuni_frame_stats.h',
      '                           d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024, d[ReadUsOther] / 1000);\n',
      '                           d[ReadBytesEmu] / 1024, d[ReadUsEmu] / 1000, d[ReadBytesOther] / 1024, d[ReadUsOther] / 1000,\n'
      '                           dc[GamePictures]);\n')
# The draws (the PICA's draw registers): accelerated (the GPU shades the vertices) or not (the CPU shades them).
patch('src/video_core/pica/pica_core.cpp',
      '#include "video_core/pica/pica_core.h"\n',
      '#include "video_core/pica/pica_core.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/pica/pica_core.cpp',
      '    // Attempt to use hardware vertex shaders if possible.\n'
      '    if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {\n'
      '        return;\n'
      '    }\n'
      '\n'
      '    // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.\n'
      '    LoadVertices(is_indexed);\n'
      '\n'
      '    // Draw emitted triangles.\n'
      '    rasterizer->DrawTriangles();\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the draws and their time, for the log (common/emusuni_frame_stats.h).\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::Draws]++;\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::DrawVertices] += regs.internal.pipeline.num_vertices;\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::DrawTicks};\n'
      '        // Attempt to use hardware vertex shaders if possible.\n'
      '        if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {\n'
      '            return;\n'
      '        }\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::SoftwareDraws]++;\n'
      '\n'
      '        // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.\n'
      '        LoadVertices(is_indexed);\n'
      '\n'
      '        // Draw emitted triangles.\n'
      '        rasterizer->DrawTriangles();\n'
      '    }\n'
      '#else\n'
      '    // Attempt to use hardware vertex shaders if possible.\n'
      '    if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {\n'
      '        return;\n'
      '    }\n'
      '\n'
      '    // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.\n'
      '    LoadVertices(is_indexed);\n'
      '\n'
      '    // Draw emitted triangles.\n'
      '    rasterizer->DrawTriangles();\n'
      '#endif\n')
# The command lists' size; the game's pictures (its top screen's buffer swaps).
patch('src/video_core/gpu.cpp',
      '    case CommandId::SubmitCmdList: {\n'
      '        auto& params = command.submit_gpu_cmdlist;\n',
      '    case CommandId::SubmitCmdList: {\n'
      '        auto& params = command.submit_gpu_cmdlist;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::ListBytes] += params.size;\n'
      '#endif\n')
patch('src/video_core/gpu.cpp',
      '    if (screen_id == 0) {\n        MicroProfileFlip();\n',
      '    if (screen_id == 0) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::counts[EmuSuNi::Stats::GamePictures]++;\n'
      '#endif\n'
      '        MicroProfileFlip();\n')
# --- 00.71: the stall log counts the surfaces read back from the GPU. 3DS Resolution goes up to 4x with the Quality
# levels: on the PC a bigger size cost the emulation thread nothing in the games tried (Hey! Pikmin, Super Mario 3D
# Land: the same CPU time a frame at 1x to 4x, no surface read back), but a game that reads a picture the GPU drew back
# into the 3DS's memory (RasterizerCache::DownloadSurface: a CPU read of it, a copy to ordinary memory) waits for the
# GPU's work - more of it at a bigger size, which also makes the surface smaller first. Each slow frame and each
# 600-frame line say how many surfaces were read back, their size and the emulation thread's time in it.
patch('src/common/emusuni_stats.h',
      '    DrawTicks, // (00.70) the draws (PicaCore::DrawArrays: the vertices set up, the draw recorded)\n',
      '    DrawTicks, // (00.70) the draws (PicaCore::DrawArrays: the vertices set up, the draw recorded)\n'
      '    ReadbackTicks, // (00.71) the surfaces read back from the GPU (RasterizerCache::DownloadSurface)\n')
patch('src/common/emusuni_stats.h',
      '    Draws, DrawVertices, SoftwareDraws, ListBytes, GamePictures, // (00.70)\n',
      '    Draws, DrawVertices, SoftwareDraws, ListBytes, GamePictures, // (00.70)\n'
      '    Readbacks, ReadbackBytes, // (00.71)\n')
patch('src/common/emusuni_frame_stats.h',
      '                           "command lists; surfaces made {}, {} ms), audio {} ms; waited {} ms for the GPU "\n',
      '                           "command lists; surfaces made {}, {} ms; read back {} ({} KiB, {} ms)), audio {} ms; "\n'
      '                           "waited {} ms for the GPU "\n')
patch('src/common/emusuni_frame_stats.h',
      '                           dc[ListBytes] / 1024, dc[Surfaces], t_ms(dt[SurfaceTicks]), t_ms(dt[DspTicks]),\n',
      '                           dc[ListBytes] / 1024, dc[Surfaces], t_ms(dt[SurfaceTicks]), dc[Readbacks],\n'
      '                           dc[ReadbackBytes] / 1024, t_ms(dt[ReadbackTicks]), t_ms(dt[DspTicks]),\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      'void RasterizerCache<T>::DownloadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_DownloadSurface);\n',
      'void RasterizerCache<T>::DownloadSurface(Surface& surface, SurfaceInterval interval) {\n'
      '    MICROPROFILE_SCOPE(RasterizerCache_DownloadSurface);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a surface read back into the 3DS\'s memory, for the log (common/emusuni_frame_stats.h).\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::ReadbackTicks};\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::Readbacks]++;\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::ReadbackBytes] += boost::icl::length(interval);\n'
      '#endif\n')
# --- 00.81: the players' graphics errors (Pokémon X and Y, Majora's Mask 3D, Metroid: Samus Returns, Castlevania:
# Mirror of Fate). Three changes:
# - The right eye per game: the app's 3DS Right Eye (Auto: drawn for the games that break without it, Emulator.cpp
#   ThreedsRightEyeGame) comes as an option of the core's own, citra_emusuni_right_eye (declared, so that the app,
#   which gives Azahar only the options it declares - StrictOptions -, can send it); the file
#   /data/EmuSuNi/3ds-right-eye still draws it in every game.
# - Upstream's fix of the right eye's guess (azahar PR #2467, merged 2026-10-23, after 2126.2): a command list that
#   draws nothing is never taken for a repeat of the top screen and dropped. With the right eye left out, Majora's Mask
#   3D's intro stayed black, Metroid: Samus Returns' bottom screen was drawn out of place (azahar #740), the Ace
#   Attorney games' bottom screen black; upstream measured Ocarina of Time 3D still leaving out 4141 lists a run (4441
#   before). Its known cost: Fire Emblem Warriors' bottom screen black (the app's Auto draws its right eye).
# - Accurate multiplication: what the core used (the setting, or its hack list's forced titles) said in the log once
#   for each game (3DS Accurate Shader Multiplication: the app's, Auto by title ID).
patch('src/citra_libretro/core_settings.cpp',
      'static constexpr const char* shaders_accurate_mul = citra_setting(BaseKeys::shaders_accurate_mul);\n',
      'static constexpr const char* shaders_accurate_mul = citra_setting(BaseKeys::shaders_accurate_mul);\n'
      '#ifdef EMUSUNI_PORT\n'
      'static constexpr const char* emusuni_right_eye = "citra_emusuni_right_eye"; // (EmuSuNi\'s: the right eye drawn)\n'
      '#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '        "Use accurate multiplication in shaders. "\n'
      '        "More accurate but can reduce performance. Only works with hardware shaders.",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::enabled, "Enabled" },\n'
      '            { config::disabled, "Disabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::enabled\n'
      '    },\n',
      '        "Use accurate multiplication in shaders. "\n'
      '        "More accurate but can reduce performance. Only works with hardware shaders.",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::enabled, "Enabled" },\n'
      '            { config::disabled, "Disabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::enabled\n'
      '    },\n'
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        config::graphics::emusuni_right_eye,\n'
      '        "Right Eye (EmuSuNi)",\n'
      '        "Right Eye",\n'
      '        "Draw the right eye\'s picture of the 3D (never shown on the PS4; a few games need it drawn).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n'
      '#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '    Settings::values.disable_right_eye_render = std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr &&\n'
      '                                                ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0;\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for (the object shows a few\n',
      '    // (00.81: and the app\'s 3DS Right Eye, per game.)\n'
      '    Settings::values.disable_right_eye_render =\n'
      '        std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr && ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0 &&\n'
      '        LibRetro::FetchVariable(config::graphics::emusuni_right_eye, config::disabled) != config::enabled;\n'
      '    {\n'
      '        static int said = -1; // (said once, and again if it changes)\n'
      '        const int now = Settings::values.disable_right_eye_render.GetValue() ? 1 : 0;\n'
      '        if (now != said) {\n'
      '            LOG_INFO(Frontend, "EmuSuNi: right eye: {}",\n'
      '                     now ? "not drawn (the PS4 shows no 3D)"\n'
      '                         : "drawn (3DS Right Eye, or the file /data/EmuSuNi/3ds-right-eye)");\n'
      '            said = now;\n'
      '        }\n'
      '    }\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for (the object shows a few\n')
# Upstream's PR #2467 (src/video_core/pica/pica_core.cpp, pica_core.h, right_eye_disabler.cpp), as merged.
patch('src/video_core/pica/pica_core.h',
      '        bool vp_heigh_found = false;\n        bool paddr_found = false;\n',
      '        bool vp_heigh_found = false;\n        bool paddr_found = false;\n'
      '        // Whether the scanned list issues any geometry.\n        bool has_draw = false;\n')
patch('src/video_core/pica/pica_core.cpp',
      'PicaCore::RenderPropertiesGuess PicaCore::GuessCmdRenderProperties(PAddr list, u32 size) {\n'
      '    // Initialize command list tracking.\n'
      '    const u8* head = memory.GetPhysicalPointer(list);\n'
      '    cmd_list.Reset(list, head, size);\n'
      '\n'
      '    constexpr size_t max_iterations = 0x100;\n'
      '\n'
      '    RenderPropertiesGuess find_info{};\n'
      '\n'
      '    find_info.vp_height = regs.internal.rasterizer.viewport_size_y.Value();\n'
      '    find_info.paddr = regs.internal.framebuffer.framebuffer.color_buffer_address.Value() * 8;\n'
      '\n',
      'PicaCore::RenderPropertiesGuess PicaCore::GuessCmdRenderProperties(PAddr list, u32 size) {\n'
      '    constexpr size_t max_iterations = 0x100;\n'
      '\n'
      '    RenderPropertiesGuess find_info{};\n'
      '\n'
      '    find_info.vp_height = regs.internal.rasterizer.viewport_size_y.Value();\n'
      '    find_info.paddr = regs.internal.framebuffer.framebuffer.color_buffer_address.Value() * 8;\n'
      '\n'
      '    // Initialize command list tracking.\n'
      '    const u8* head = memory.GetPhysicalPointer(list);\n'
      '    if (head == nullptr) {\n'
      '        return find_info;\n'
      '    }\n'
      '    cmd_list.Reset(list, head, size);\n'
      '\n')
patch('src/video_core/pica/pica_core.cpp',
      '            find_info.paddr = value * 8;\n'
      '            find_info.paddr_found = true;\n'
      '            break;\n'
      '        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):\n',
      '            find_info.paddr = value * 8;\n'
      '            find_info.paddr_found = true;\n'
      '            break;\n'
      '        case PICA_REG_INDEX(pipeline.trigger_draw):\n'
      '        case PICA_REG_INDEX(pipeline.trigger_draw_indexed):\n'
      '            find_info.has_draw = true;\n'
      '            break;\n'
      '        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):\n')
patch('src/video_core/pica/pica_core.cpp',
      '            const u8* head = memory.GetPhysicalPointer(addr);\n'
      '            cmd_list.Reset(addr, head, size);\n'
      '            break;\n'
      '        }\n'
      '        default:\n'
      '            break;\n'
      '        }\n'
      '        return find_info.vp_heigh_found && find_info.paddr_found;\n',
      '            const u8* head = memory.GetPhysicalPointer(addr);\n'
      '            if (head != nullptr) {\n'
      '                cmd_list.Reset(addr, head, size);\n'
      '            }\n'
      '            break;\n'
      '        }\n'
      '        default:\n'
      '            break;\n'
      '        }\n'
      '        return find_info.vp_heigh_found && find_info.paddr_found && find_info.has_draw;\n')
patch('src/video_core/right_eye_disabler.cpp',
      '    if (guess.vp_height == top_screen_size && !top_screen_blocked) {\n',
      '    if (guess.has_draw && guess.vp_height == top_screen_size && !top_screen_blocked) {\n')
# EmuSuNi's limit on it: only a list scanned to its end without a draw counts as drawing nothing. A list cut off by the
# scan's 256 commands may draw further on, and is taken as before #2467: as merged, Super Mario 3D Land's right eye
# wasn't left out at all (PC, World 1-1: 133,726 draws in 600 frames against 70,613, the emulation thread 2.9 against
# 2.1 ms a frame - all 00.63's saving gone); with this limit 70,613 again and the same pictures.
patch('src/video_core/pica/pica_core.cpp',
      '        iterations++;\n    }\n\n    return find_info;\n}\n',
      '        iterations++;\n    }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: a list scanned only in part may still draw (scripts/build-3ds-core.sh, 00.81).\n'
      '    if (!find_info.has_draw && iterations >= max_iterations) {\n'
      '        find_info.has_draw = true;\n'
      '    }\n'
      '#endif\n'
      '    return find_info;\n}\n')
# Accurate multiplication as the game got it, in the log (once a game; again if it changes).
patch('src/video_core/gpu.cpp',
      '#include "common/hacks/hack_manager.h"\n',
      '#include "common/hacks/hack_manager.h"\n#include "common/logging/log.h"\n')
patch('src/video_core/gpu.cpp',
      '    impl->rasterizer->SetAccurateMul(use_accurate_mul);\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        static u64 said_for = 0;\n'
      '        static int said = -1;\n'
      '        if (said_for != program_ID || said != (use_accurate_mul ? 1 : 0)) {\n'
      '            LOG_INFO(Frontend, "EmuSuNi: accurate multiplication {} for {:016X} (the setting {}{})",\n'
      '                     use_accurate_mul ? "on" : "off", program_ID,\n'
      '                     Settings::values.shaders_accurate_mul.GetValue() ? "on" : "off",\n'
      '                     hack && hack->mode == Common::Hacks::HackAllowMode::FORCE ? "; forced on by the hack list"\n'
      '                     : hack && hack->mode == Common::Hacks::HackAllowMode::DISALLOW ? "; off by the hack list"\n'
      '                                                                                   : "");\n'
      '            said_for = program_ID;\n'
      '            said = use_accurate_mul ? 1 : 0;\n'
      '        }\n'
      '    }\n'
      '#endif\n'
      '    impl->rasterizer->SetAccurateMul(use_accurate_mul);\n}\n')

# --- 00.85: a 3DS game that goes wrong ends the game, not EmuSuNi, and the log says what went wrong. The players' v00.22:
# the app closed right after a 3DS game opened; Luigi's Mansion 2 froze, then the PS4 closed the app with an error
# (the first 3DS game on that console); Majora's Mask 3D went dark with its sound playing, the app not answering for
# seconds at a time. On the PS4 a C++ exception never reaches its catch (the app's own __cxa_throw with Sony's unwinder,
# 00.64): every throw anywhere in the core ended the app at once, with no crash report - and Azahar throws and catches
# as a matter of course in a few places (a vertex shader its decompiler can't take, the PICA shader JIT's memory full,
# dynarmic's block of code tried smaller, the Vulkan stream buffers and pipeline cache made again smaller or empty, a
# save state that doesn't read). Five changes:
#  1. Every C++ exception the core throws is logged first, at once and straight into the log file (common/emusuni_
#     throw.cpp, linked over __cxa_throw with ld.lld -r --wrap, below): its type, its message, where it was thrown from
#     (and where retro_run is, to find the function in the build's map). On the PS4, one thrown under retro_run, the
#     game's load, a save state's load or save, goes back to that call (setjmp/longjmp) instead of ending the app: the
#     game ends with a message and EmuSuNi stays (what the exception skipped is lost - the game's end may then take the
#     start watchdog's restart; never the app closing on its own). On this PC the catches work: as upstream, unless
#     EMUSUNI_3DS_RESCUE=1 (the tests; EMUSUNI_3DS_TEST_THROW=<frame> throws in retro_run at that frame).
#  2. The throws Azahar meant to catch, made without an exception: the decompiler's failure (the shader then runs on
#     the CPU, as upstream meant), the shader JIT's memory taken before its code generator is made (none: the
#     interpreter), dynarmic's block of code sized by asking the pool first, the stream buffers and the driver's
#     pipeline cache by their results, a shader module by its result; no JIT memory at all said by the load.
#  3. A frame (or the game's load) that has run for 2 s says so while it goes on ("EmuSuNi: stall: ..."), then every
#     2 s, 10 s after the first 10 (a thread of the core's own, straight into the log): what the emulation thread is in
#     (waiting for the GPU, for the Vulkan worker, building a pipeline, compiling a shader, reading a file, a texture),
#     for how long, and what the frame did so far; and when it ends. A GPU that stopped shows as the GPU waited for (with
#     the RADV winsys' own "GPU hang?" line), a storm of pipelines as pipelines built.
#  4. What the 3DS can't go on from ends the game with a message instead of running on: the game's code crashed (its
#     exception, which upstream logged and left the game frozen on), a fatal error the game threw (ERR:F), the game
#     closing itself, the system stopped (upstream's loop then spun in retro_run for ever, the app's main loop with it).
#  5. Azahar's GPU timing emulation (azahar #2095: "Luigi's Mansion 2 no longer needs the delay render thread option to
#     be playable"; off by default upstream, not in the libretro core's options) as citra_emusuni_gpu_timings, which the
#     app turns on for Luigi's Mansion 2 (Emulator.cpp; the file /data/EmuSuNi/3ds-gpu-timings: every game).
addfile('src/common/emusuni_throw.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 00.85): every C++ exception the core throws, logged before it's thrown; on the
// PS4 (where its catch is never reached: the app would end) one thrown under a rescue point goes back to it instead.
#pragma once
#ifdef EMUSUNI_PORT
#include <csetjmp>

namespace EmuSuNi {
// A line straight into the log (stderr: the app's log file; written and synced at once, no lock, no allocation).
void RawLine(const char* text);
}

namespace EmuSuNi::Throw {
// A point the thread goes back to (longjmp) when a C++ exception is thrown under it, on the PS4. Used as:
//   EmuSuNi::Throw::Rescue rescue{"loading the game"};
//   if (setjmp(rescue.env) != 0) { ... the exception's message: RescuedMessage(); undo what can be; return; }
//   EmuSuNi::Throw::Arm(rescue);
//   ... the work ...
//   EmuSuNi::Throw::Disarm(rescue);
struct Rescue {
    explicit Rescue(const char* where_) : where(where_) {}
    std::jmp_buf env;
    const char* where;
    Rescue* outer = nullptr;
    unsigned mxcsr = 0; // (dynarmic's guest rounding and flush-to-zero may be in MXCSR when the throw comes)
    bool armed = false;
};
// Whether a throw goes back to its rescue point: always on the PS4; on this PC with EMUSUNI_3DS_RESCUE=1 (the tests).
bool RescueOn();
void Arm(Rescue& rescue);
void Disarm(Rescue& rescue);
// The exception the last rescue came back from: its type and message.
const char* RescuedMessage();
// The C++ exceptions thrown so far (the log's count).
unsigned Thrown();
// The PC tests: EMUSUNI_3DS_TEST_THROW=<n> throws a std::runtime_error at the n-th call (retro_run's frames).
void TestThrow();
} // namespace EmuSuNi::Throw
#endif
''')
addfile('src/common/emusuni_throw.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh, 00.85): see emusuni_throw.h. The final link renames the core's calls of
// __cxa_throw to __wrap___cxa_throw (ld.lld -r --wrap); __real___cxa_throw is the runtime's.
#ifdef EMUSUNI_PORT
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <cxxabi.h>
#include <unistd.h>
#include <xmmintrin.h>
#include "common/emusuni_stats.h"
#include "common/emusuni_throw.h"

extern "C" void retro_run(void); // (azahar_retro_run: the prefix header renames it; its address goes in the log)

namespace EmuSuNi {
void RawLine(const char* text) {
    char line[1024];
    const int n = std::snprintf(line, sizeof(line), "Azahar: %s\n", text);
    if (n > 0) {
        const size_t length = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
        if (write(STDERR_FILENO, line, length) < 0) {
        }
#if defined(__ORBIS__)
        fsync(STDERR_FILENO); // (the app may end right after: the line on the disk first)
#endif
    }
}
} // namespace EmuSuNi

namespace EmuSuNi::Throw {
namespace {
std::atomic<unsigned> g_thrown{0};
thread_local Rescue* t_rescue = nullptr;
char g_rescued[256];

// The Itanium C++ ABI's type_info layouts (the same for every runtime): a class without bases, one with a single
// public base at offset 0, one with several (each with its offset; a virtual base's offset is in the vtable).
struct ClassInfo {
    const void* vptr;
    const char* name;
};
struct SingleInfo {
    const void* vptr;
    const char* name;
    const ClassInfo* base;
};
struct BaseInfo {
    const ClassInfo* base;
    long offset_flags; // offset << 8; 1: virtual, 2: public
};
struct MultiInfo {
    const void* vptr;
    const char* name;
    unsigned flags;
    unsigned count;
    BaseInfo bases[1];
};
// The vtables of the kinds, from classes of this file (the runtime that gives them is the core's own classes').
struct KindA {
    virtual ~KindA();
};
KindA::~KindA() = default;
struct KindB : KindA {};
struct KindC {
    virtual ~KindC();
};
KindC::~KindC() = default;
struct KindD : KindA, KindC {};
const void* VptrOf(const std::type_info& type) {
    return *reinterpret_cast<const void* const*>(&type);
}
const char* NameOf(const ClassInfo* type) {
    const char* name = type->name;
    return name[0] == '*' ? name + 1 : name;
}
// The std::exception in the thrown object, if its class has one. std's own exceptions (St..., NSt3__1...) all derive
// from it alone, at offset 0 (their type_info may come from another runtime, Sony's: their kind not told by vtable).
const std::exception* FindException(char* object, const ClassInfo* type, int depth) {
    if (type == nullptr || object == nullptr || depth > 12) {
        return nullptr;
    }
    const char* name = NameOf(type);
    if (std::strncmp(name, "St", 2) == 0 || std::strncmp(name, "NSt3__1", 7) == 0) {
        return std::strcmp(name, "St13bad_exception") == 0 || std::strcmp(name, "NSt3__117nested_exceptionE") == 0
                   ? nullptr
                   : reinterpret_cast<const std::exception*>(object);
    }
    if (type->vptr == VptrOf(typeid(KindB))) {
        return FindException(object, reinterpret_cast<const SingleInfo*>(type)->base, depth + 1);
    }
    if (type->vptr == VptrOf(typeid(KindD))) {
        const auto* multi = reinterpret_cast<const MultiInfo*>(type);
        for (unsigned i = 0; i < multi->count && i < 16; i++) {
            const long flags = multi->bases[i].offset_flags;
            if ((flags & 2) == 0) {
                continue;
            }
            long offset = flags >> 8;
            if ((flags & 1) != 0) {
                const char* vtable = *reinterpret_cast<char* const*>(object);
                offset = *reinterpret_cast<const long*>(vtable + offset);
            }
            if (const std::exception* found = FindException(object + offset, multi->bases[i].base, depth + 1)) {
                return found;
            }
        }
    }
    return nullptr;
}
} // namespace

bool RescueOn() {
#if defined(__ORBIS__)
    return true;
#else
    static const bool on = std::getenv("EMUSUNI_3DS_RESCUE") != nullptr;
    return on;
#endif
}

void Arm(Rescue& rescue) {
    if (!RescueOn()) {
        return;
    }
    rescue.mxcsr = _mm_getcsr();
    rescue.outer = t_rescue;
    rescue.armed = true;
    t_rescue = &rescue;
}

void Disarm(Rescue& rescue) {
    if (rescue.armed && t_rescue == &rescue) {
        t_rescue = rescue.outer;
    }
    rescue.armed = false;
}

const char* RescuedMessage() {
    return g_rescued;
}

unsigned Thrown() {
    return g_thrown.load();
}

void TestThrow() {
#if !defined(__ORBIS__)
    static const long at = std::getenv("EMUSUNI_3DS_TEST_THROW") ? std::atol(std::getenv("EMUSUNI_3DS_TEST_THROW")) : 0;
    static long calls = 0;
    if (at > 0 && ++calls == at) {
        throw std::runtime_error("EMUSUNI_3DS_TEST_THROW: a test exception");
    }
#endif
}
} // namespace EmuSuNi::Throw

extern "C" [[noreturn]] void __real___cxa_throw(void* object, std::type_info* type, void (*destructor)(void*));

extern "C" [[noreturn]] void __wrap___cxa_throw(void* object, std::type_info* type, void (*destructor)(void*)) {
    using namespace EmuSuNi::Throw;
    const unsigned n = ++g_thrown;
    void* const from = __builtin_return_address(0);
    Rescue* const rescue = t_rescue;
    char what[256] = "";
    {
        const char* mangled = type != nullptr ? type->name() : "?";
        int status = -1;
        char* demangled = type != nullptr ? abi::__cxa_demangle(mangled, nullptr, nullptr, &status) : nullptr;
        const std::exception* exception =
            type != nullptr ? FindException(static_cast<char*>(object), reinterpret_cast<const ClassInfo*>(type), 0)
                            : nullptr;
        const char* message = exception != nullptr ? exception->what() : nullptr;
        std::snprintf(what, sizeof(what), "%s%s%s", demangled != nullptr && status == 0 ? demangled : mangled,
                      message != nullptr ? ": " : "", message != nullptr ? message : "");
        std::free(demangled);
    }
    // (The first 20, then one in 100: on this PC a caught one may come again and again.)
    if (n <= 20 || n % 100 == 0 || rescue != nullptr) {
        char line[768];
        std::snprintf(line, sizeof(line),
                      "EmuSuNi: C++ exception %u thrown in the core, on %s: %s (thrown from %p; retro_run at %p)%s", n,
                      EmuSuNi::Stats::on_emulation_thread ? "the emulation thread" : "another thread", what, from,
                      reinterpret_cast<void*>(&retro_run),
                      rescue != nullptr ? "" :
#if defined(__ORBIS__)
                                        "; on the PS4 its catch is never reached: EmuSuNi ends here"
#else
                                        ""
#endif
        );
        EmuSuNi::RawLine(line);
    }
    if (rescue != nullptr) {
        // Back to the rescue point (on the PS4, or the PC tests): the exception object is left (a few bytes).
        std::snprintf(g_rescued, sizeof(g_rescued), "%s", what);
        char line[512];
        std::snprintf(line, sizeof(line), "EmuSuNi: the exception goes back to \"%s\" (EmuSuNi stays; the game ends)",
                      rescue->where);
        EmuSuNi::RawLine(line);
        t_rescue = rescue->outer;
        rescue->armed = false;
        if (EmuSuNi::Stats::on_emulation_thread) {
            EmuSuNi::Stats::doing.store(nullptr); // (the Doing scopes the jump skips)
        }
        _mm_setcsr(rescue->mxcsr);
        std::longjmp(rescue->env, 1);
    }
    __real___cxa_throw(object, type, destructor);
}
#endif
''')
addfile('src/common/emusuni_stall.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 00.85): a frame (or the game's load) that runs for seconds, said while it goes
// on: what the emulation thread is in (EmuSuNi::Stats::Doing) and what the frame did so far, from a thread of its own.
#pragma once
#ifdef EMUSUNI_PORT
namespace EmuSuNi::Stall {
// The emulation thread starts / ends something watched ("a frame", "the game's load"...): a text that lives on.
void Begin(const char* what);
void End();
// The core shuts down: the watching thread stops.
void Stop();
} // namespace EmuSuNi::Stall
#endif
''')
addfile('src/common/emusuni_stall.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh, 00.85): see emusuni_stall.h.
#ifdef EMUSUNI_PORT
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include "common/emusuni_frame_stats.h"
#include "common/emusuni_stall.h"
#include "common/emusuni_stats.h"
#include "common/emusuni_throw.h"
#include "common/thread.h"

namespace EmuSuNi::Stall {
namespace {
using namespace EmuSuNi::Stats;
// Written by the emulation thread at Begin, read by the watch: the counters as the watched thing began.
std::uint64_t g_at[Count], g_ticks_at[TickCount], g_counts_at[CountCount];
std::atomic<std::uint64_t> g_t0_us{0}; // 0: nothing watched now
std::atomic<std::uint64_t> g_k0{0};
std::atomic<const char*> g_what{""};
std::atomic<std::uint64_t> g_seq{0};      // one more at each Begin
std::atomic<std::uint64_t> g_said_seq{0}; // the watch said a stall of this one
std::atomic<bool> g_run{false};
std::mutex g_mutex; // (the thread's start and stop)
std::thread g_thread;
// (2000 ms, or EMUSUNI_3DS_STALL_MS on the PC tests)
std::uint64_t FirstMs() {
    static const std::uint64_t ms =
        std::getenv("EMUSUNI_3DS_STALL_MS") ? std::strtoull(std::getenv("EMUSUNI_3DS_STALL_MS"), nullptr, 10) : 2000;
    return ms > 0 ? ms : 2000;
}

void Say(std::uint64_t seq, std::uint64_t now_us, std::uint64_t now_k) {
    const std::uint64_t t0 = g_t0_us.load(std::memory_order_acquire);
    if (t0 == 0 || g_seq.load() != seq) {
        return;
    }
    const std::uint64_t k0 = g_k0.load();
    const double tpu = now_us > t0 ? static_cast<double>(now_k - k0) / static_cast<double>(now_us - t0) : 0;
    std::uint64_t d[Count], dt[TickCount], dc[CountCount];
    for (int i = 0; i < Count; i++) {
        d[i] = counters[i].load(std::memory_order_relaxed) - g_at[i];
    }
    for (int i = 0; i < TickCount; i++) {
        dt[i] = reinterpret_cast<const volatile std::uint64_t*>(ticks)[i] - g_ticks_at[i];
    }
    for (int i = 0; i < CountCount; i++) {
        dc[i] = reinterpret_cast<const volatile std::uint64_t*>(counts)[i] - g_counts_at[i];
    }
    const char* in = doing.load(std::memory_order_relaxed);
    const std::uint64_t in_k = doing_since.load(std::memory_order_relaxed);
    char now_in[160];
    if (in != nullptr && tpu > 0 && now_k > in_k) {
        std::snprintf(now_in, sizeof(now_in), "%s for %.1f s", in, static_cast<double>(now_k - in_k) / tpu / 1e6);
    } else {
        std::snprintf(now_in, sizeof(now_in), "running the 3DS's code, its HLE or its GPU's commands");
    }
    const std::string so_far = FrameStats::Describe(d, dt, dc, (now_us - t0) / 1000, tpu);
    char line[1024];
    std::snprintf(line, sizeof(line), "EmuSuNi: stall: %s has run for %.1f s and goes on: now %s; so far %s",
                  g_what.load(), static_cast<double>(now_us - t0) / 1e6, now_in, so_far.c_str());
    EmuSuNi::RawLine(line);
    g_said_seq.store(seq);
}

void Main() {
    Common::SetCurrentThreadName("EmuSuNi stall watch");
    std::uint64_t watched = 0, next_ms = 0;
    while (g_run.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const std::uint64_t t0 = g_t0_us.load(std::memory_order_acquire);
        if (t0 == 0) {
            continue;
        }
        const std::uint64_t seq = g_seq.load();
        if (seq != watched) {
            watched = seq;
            next_ms = FirstMs();
        }
        const std::uint64_t now_us = NowUs(), now_k = Ticks();
        const std::uint64_t ms = now_us > t0 ? (now_us - t0) / 1000 : 0;
        if (ms >= next_ms) {
            Say(seq, now_us, now_k);
            next_ms = ms + (ms < 10000 ? 2000 : 10000);
        }
    }
}
} // namespace

void Begin(const char* what) {
    if (!g_run.load()) {
        std::scoped_lock lock{g_mutex};
        if (!g_run.load()) {
            g_run = true;
            g_thread = std::thread(Main);
        }
    }
    g_t0_us.store(0, std::memory_order_release);
    for (int i = 0; i < Count; i++) {
        g_at[i] = counters[i].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < TickCount; i++) {
        g_ticks_at[i] = ticks[i];
    }
    for (int i = 0; i < CountCount; i++) {
        g_counts_at[i] = counts[i];
    }
    g_what.store(what);
    g_seq.fetch_add(1);
    g_k0.store(Ticks());
    g_t0_us.store(NowUs(), std::memory_order_release);
}

void End() {
    const std::uint64_t t0 = g_t0_us.exchange(0);
    const std::uint64_t seq = g_seq.load();
    if (t0 != 0 && g_said_seq.load() == seq) {
        char line[256];
        std::snprintf(line, sizeof(line), "EmuSuNi: stall over: %s took %.1f s", g_what.load(),
                      static_cast<double>(NowUs() - t0) / 1e6);
        EmuSuNi::RawLine(line);
    }
}

void Stop() {
    std::scoped_lock lock{g_mutex};
    if (g_run.exchange(false) && g_thread.joinable()) {
        g_thread.join();
    }
}
} // namespace EmuSuNi::Stall
#endif
''')
patch('src/common/CMakeLists.txt',
      '    emusuni_random.cpp\n    aarch64/cpu_detect.cpp\n',
      '    emusuni_random.cpp\n'
      '    emusuni_stall.cpp\n    emusuni_stall.h\n    emusuni_throw.cpp\n    emusuni_throw.h\n'
      '    aarch64/cpu_detect.cpp\n')
# What the emulation thread is in now, for the stall lines (EmuSuNi::Stats::Doing around the waits and compiles; the
# time in CPU ticks: no clock read, a system call on the PS4).
patch('src/common/emusuni_stats.h',
      '        if (svc) {\n            ticks[GpuInSvcTicks] += d;\n        }\n    }\n};\n} // namespace EmuSuNi::Stats\n',
      '        if (svc) {\n            ticks[GpuInSvcTicks] += d;\n        }\n    }\n};\n'
      '// (00.85) What the emulation thread is in now, for the stall lines (common/emusuni_stall.cpp): set around its\n'
      '// waits and compiles, nothing on the other threads.\n'
      'inline std::atomic<const char*> doing{nullptr};\n'
      'inline std::atomic<std::uint64_t> doing_since{0};\n'
      'struct Doing {\n'
      '    const char* before = nullptr;\n'
      '    std::uint64_t since_before = 0;\n'
      '    bool on;\n'
      '    explicit Doing(const char* what) : on(on_emulation_thread) {\n'
      '        if (on) {\n'
      '            before = doing.load(std::memory_order_relaxed);\n'
      '            since_before = doing_since.load(std::memory_order_relaxed);\n'
      '            doing_since.store(Ticks(), std::memory_order_relaxed);\n'
      '            doing.store(what, std::memory_order_relaxed);\n'
      '        }\n'
      '    }\n'
      '    ~Doing() {\n'
      '        if (on) {\n'
      '            doing.store(before, std::memory_order_relaxed);\n'
      '            doing_since.store(since_before, std::memory_order_relaxed);\n'
      '        }\n'
      '    }\n'
      '    Doing(const Doing&) = delete;\n'
      '    Doing& operator=(const Doing&) = delete;\n'
      '};\n'
      '} // namespace EmuSuNi::Stats\n')
# The frame watched from its start to its end (common/emusuni_frame_stats.h's FrameStats is retro_run's).
patch('src/common/emusuni_frame_stats.h',
      '    FrameStats() {\n        on_emulation_thread = true;\n',
      '    FrameStats() {\n        on_emulation_thread = true;\n'
      '        EmuSuNi::Stall::Begin("a frame"); // (00.85: common/emusuni_stall.h)\n')
patch('src/common/emusuni_frame_stats.h',
      '    ~FrameStats() {\n',
      '    ~FrameStats() {\n'
      '        EmuSuNi::Stall::End();\n')
patch('src/common/emusuni_frame_stats.h',
      '#include "common/emusuni_stats.h"\n',
      '#include "common/emusuni_stall.h"\n#include "common/emusuni_stats.h"\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);\n#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n',
      '    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);\n#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"waiting for the Vulkan worker (the GPU\'s commands recorded and "\n'
      '                                              "sent; it may wait for a pipeline)"};\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '        return;\n    }\n    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n#endif\n',
      '        return;\n    }\n    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"waiting for the GPU to finish its work"};\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n',
      '    const EmuSuNi::Stats::Event emusuni_event{EmuSuNi::Stats::PipelinesEmu, EmuSuNi::Stats::PipelinesOther};\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"building a Vulkan pipeline"};\n')
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      '                                                                                   : EmuSuNi::Stats::ShaderUsOther};\n'
      '#endif\n',
      '                                                                                   : EmuSuNi::Stats::ShaderUsOther};\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"compiling a shader (GLSL to SPIR-V)"};\n'
      '#endif\n')
patch('src/common/file_util.cpp',
      '    const std::uint64_t emusuni_t0 = EmuSuNi::Stats::NowUs();\n',
      '    const std::uint64_t emusuni_t0 = EmuSuNi::Stats::NowUs();\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"reading a file"};\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::UploadUs};\n',
      '    const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::UploadUs};\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"decoding and uploading a texture"};\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::ReadbackTicks};\n',
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::ReadbackTicks};\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"reading a picture back from the GPU"};\n')
# The libretro glue: the rescue points (retro_run, the game's load, the context's set-up, a save state's load and
# save), the stall watch around the load, the statuses that end the game.
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "citra_libretro/emusuni_jitwarm.h"\n#endif\n',
      '#include "citra_libretro/emusuni_jitwarm.h"\n#include <csetjmp>\n#include "common/emusuni_stall.h"\n'
      '#include "common/emusuni_throw.h"\n#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    delete emu_instance;\n#ifdef EMUSUNI_PORT\n',
      '    delete emu_instance;\n#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Stall::Stop(); // (00.85: the stall watch\'s thread)\n')
patch('src/citra_libretro/citra_libretro.cpp',
      'CitraLibRetro* emu_instance;\n',
      'CitraLibRetro* emu_instance;\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (00.85): the player\'s message when a C++ exception came back to a rescue point (common/emusuni_throw.h:\n'
      '// on the PS4 its catch is never reached; EmuSuNi stays, the game ends). The app shows it in its own words.\n'
      'static constexpr const char* EMUSUNI_RESCUED =\n'
      '    "The 3DS emulator stopped on an error\\nThe game was closed: EmuSuNi\'s log says where";\n'
      '// The statuses the 3DS can\'t go on from: the game ends with its message (retro_run).\n'
      'static const char* EmuSuNiEnds(Core::System::ResultStatus result) {\n'
      '    switch (result) {\n'
      '    case Core::System::ResultStatus::ErrorCoreExceptionRaised:\n'
      '        return "The 3DS game crashed (an exception in its code)";\n'
      '    case Core::System::ResultStatus::ErrorUnknown:\n'
      '        return "The 3DS game stopped on a fatal error";\n'
      '    case Core::System::ResultStatus::ShutdownRequested:\n'
      '        return "The 3DS game closed itself";\n'
      '    case Core::System::ResultStatus::ErrorNotInitialized:\n'
      '        return "The 3DS emulator stopped";\n'
      '    default:\n'
      '        return nullptr;\n'
      '    }\n'
      '}\n'
      '#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '    // EmuSuNi: the JIT warm-up',
      '    // EmuSuNi (00.85): on the PS4 a C++ exception thrown under this frame comes back here (common/emusuni_throw.h:\n'
      '    // its catch below is never reached there, the app would end): the game ends with a message, EmuSuNi stays.\n'
      '    EmuSuNi::Throw::Rescue emusuni_rescue{"running the game (retro_run)"};\n'
      '    if (setjmp(emusuni_rescue.env) != 0) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: the emulation stopped on a C++ exception ({}): the game ends",\n'
      '                     EmuSuNi::Throw::RescuedMessage());\n'
      '        LibRetro::DisplayMessage(EMUSUNI_RESCUED);\n'
      '        emu_instance->game_loaded = false; // (empty frames until the frontend stops the game)\n'
      '        LibRetro::Shutdown();\n'
      '        return;\n'
      '    }\n'
      '    EmuSuNi::Throw::Arm(emusuni_rescue);\n'
      '    // EmuSuNi: an exception out of the emulation (the JITs\' memory, a failed allocation...) ends the\n'
      '    // game, not the app: the message, then RETRO_ENVIRONMENT_SHUTDOWN.\n'
      '    try {\n'
      '    EmuSuNi::Throw::TestThrow(); // (the PC tests: EMUSUNI_3DS_TEST_THROW)\n'
      '    // EmuSuNi: the JIT warm-up')
patch('src/citra_libretro/citra_libretro.cpp',
      '            LibRetro::DisplayMessage(msg.c_str());\n'
      '        }\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    } catch (const std::exception& error) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: the emulation threw: {}", error.what());\n'
      '        LibRetro::DisplayMessage((std::string("Azahar stopped: ") + error.what()).c_str());\n'
      '        emu_instance->game_loaded = false; // (empty frames until the frontend stops the game)\n'
      '        LibRetro::Shutdown();\n'
      '    }\n'
      '#endif\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi (00.85): a status the 3DS can\'t go on from ends the game with its message (upstream showed\n'
      '            // it and ran on: a crashed game stayed frozen, and a system that stopped made this loop spin for\n'
      '            // ever, the app\'s main loop with it).\n'
      '            if (const char* ends = EmuSuNiEnds(result)) {\n'
      '                LOG_CRITICAL(Frontend, "EmuSuNi: the game ends: {} (status {}: {})", ends, static_cast<int>(result),\n'
      '                             errorContent);\n'
      '                LibRetro::DisplayMessage(ends);\n'
      '                emu_instance->game_loaded = false;\n'
      '                LibRetro::Shutdown();\n'
      '                break;\n'
      '            }\n'
      '#endif\n'
      '            LibRetro::DisplayMessage(msg.c_str());\n'
      '        }\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    } catch (const std::exception& error) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: the emulation threw: {}", error.what());\n'
      '        LibRetro::DisplayMessage((std::string("Azahar stopped: ") + error.what()).c_str());\n'
      '        emu_instance->game_loaded = false; // (empty frames until the frontend stops the game)\n'
      '        LibRetro::Shutdown();\n'
      '    }\n'
      '    EmuSuNi::Throw::Disarm(emusuni_rescue);\n'
      '#endif\n'
      '}\n')
# The game's load: under a rescue point and the stall watch; no JIT memory said here (core/core.cpp threw it).
patch('src/citra_libretro/citra_libretro.cpp',
      'static bool do_load_game() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: what the load throws',
      '#ifdef EMUSUNI_PORT\n'
      'static bool emusuni_load_game();\n'
      '\n'
      '// EmuSuNi (00.85): the game\'s load under a rescue point (common/emusuni_throw.h: on the PS4 a C++ exception\'s\n'
      '// catch is never reached) and the stall watch; the CPU JIT\'s memory asked first (core/core.cpp threw for it).\n'
      'static bool do_load_game() {\n'
      '    if (Settings::values.use_cpu_jit.GetValue() && !EmuSuNi::JitAvailable()) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: no executable memory for the 3DS\'s processor: the game doesn\'t start");\n'
      '        LibRetro::DisplayMessage("The PS4 gave no memory for the 3DS\'s processor\\n"\n'
      '                                 "(64 MiB to run its code: /data/EmuSuNi/emusuni.log says why)");\n'
      '        return false;\n'
      '    }\n'
      '    EmuSuNi::Throw::Rescue rescue{"loading the game"};\n'
      '    if (setjmp(rescue.env) != 0) {\n'
      '        EmuSuNi::Stall::End();\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: loading the game stopped on a C++ exception ({})",\n'
      '                     EmuSuNi::Throw::RescuedMessage());\n'
      '        LibRetro::DisplayMessage(EMUSUNI_RESCUED);\n'
      '        return false;\n'
      '    }\n'
      '    EmuSuNi::Throw::Arm(rescue);\n'
      '    EmuSuNi::Stats::on_emulation_thread = true;\n'
      '    EmuSuNi::Stall::Begin("the game\'s load");\n'
      '    const bool loaded = emusuni_load_game();\n'
      '    EmuSuNi::Stall::End();\n'
      '    EmuSuNi::Throw::Disarm(rescue);\n'
      '    return loaded;\n'
      '}\n'
      '\n'
      'static bool emusuni_load_game() {\n'
      '#else\n'
      'static bool do_load_game() {\n'
      '#endif\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: what the load throws')
patch('src/citra_libretro/citra_libretro.cpp',
      '    if (Settings::values.use_disk_shader_cache) {\n'
      '        Core::System::GetInstance().GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(\n'
      '            false, nullptr);\n'
      '    }\n',
      '    if (Settings::values.use_disk_shader_cache) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const EmuSuNi::Stats::Doing emusuni_doing{"loading the shader cache"};\n'
      '#endif\n'
      '        Core::System::GetInstance().GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(\n'
      '            false, nullptr);\n'
      '    }\n')
# The 3DS's language from the game's start (01.08): upstream's libretro port puts the core option's language in CFG
# only when something first asks CFG for the region (UpdatePreferredRegionCode). A game that reads its language before
# that, or never asks, got the config save's English: Donkey Kong Country Returns 3D, Super Mario 3D Land and Driver
# Renegade 3D never asked in the owner's logs (no "Preferred region code set" line), and players' games came out in
# English with the app in Spanish. Asked once here, after the load and before the game's first instruction: the region
# and the language as upstream chooses them (the app's language when the game's region has it, else the region's
# first). A save state brings back its own (CFG's blocks are in it).
patch('src/citra_libretro/citra_libretro.cpp',
      '    Core::System::GetInstance().GPU().ApplyPerProgramSettings(program_id);\n',
      '    Core::System::GetInstance().GPU().ApplyPerProgramSettings(program_id);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.08): the language (the app\'s: citra_language_value) and the region in CFG now, before the\n'
      '    // game runs: upstream set them when something first asked CFG for the region, and a game that read its\n'
      '    // language before that, or never asked (Donkey Kong Country Returns 3D), got the config save\'s English.\n'
      '    if (Settings::values.region_value.GetValue() == Settings::REGION_VALUE_AUTO_SELECT) {\n'
      '        static const char* const emusuni_languages[] = {\n'
      '            "Japanese", "English", "French",  "German",     "Italian", "Spanish",\n'
      '            "Chinese",  "Korean",  "Dutch",   "Portuguese", "Russian", "Chinese (Taiwan)"};\n'
      '        const unsigned emusuni_asked = static_cast<unsigned>(LibRetro::settings.language_value);\n'
      '        const unsigned emusuni_language = static_cast<unsigned>(\n'
      '            Service::CFG::GetModule(Core::System::GetInstance())->GetSystemLanguage());\n'
      '        LOG_INFO(Frontend, "EmuSuNi: the 3DS\'s language from the start: {} (the app\'s: {})",\n'
      '                 emusuni_language < 12 ? emusuni_languages[emusuni_language] : "?",\n'
      '                 emusuni_asked < 12 ? emusuni_languages[emusuni_asked] : "?");\n'
      '    }\n'
      '#endif\n')
# The context's set-up (the renderer made: the Vulkan instance's checks throw, as vulkan.hpp does when the device runs
# out of memory) under a rescue point too.
patch('src/citra_libretro/citra_libretro.cpp',
      'static void context_reset() {\n    LOG_DEBUG(Frontend, "context_reset");\n',
      '#ifdef EMUSUNI_PORT\n'
      'static void emusuni_context_reset();\n'
      '\n'
      '// EmuSuNi (00.85): under a rescue point (common/emusuni_throw.h).\n'
      'static void context_reset() {\n'
      '    EmuSuNi::Throw::Rescue rescue{"setting up the renderer (context_reset)"};\n'
      '    if (setjmp(rescue.env) != 0) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: setting up the renderer stopped on a C++ exception ({})",\n'
      '                     EmuSuNi::Throw::RescuedMessage());\n'
      '        LibRetro::DisplayMessage(EMUSUNI_RESCUED);\n'
      '        emu_instance->game_loaded = false;\n'
      '        LibRetro::Shutdown();\n'
      '        return;\n'
      '    }\n'
      '    EmuSuNi::Throw::Arm(rescue);\n'
      '    emusuni_context_reset();\n'
      '    EmuSuNi::Throw::Disarm(rescue);\n'
      '}\n'
      '\n'
      'static void emusuni_context_reset() {\n'
      '#else\n'
      'static void context_reset() {\n'
      '#endif\n'
      '    LOG_DEBUG(Frontend, "context_reset");\n')
# A save state: its load under a rescue point (a state that doesn't read throws in Boost's archive: on the PS4 the game
# ends, half loaded, rather than the app); its making too (nothing of the game changed: it goes on, without the state).
patch('src/citra_libretro/citra_libretro.cpp',
      'size_t retro_serialize_size() {\n'
      '    auto& system = Core::System::GetInstance();\n',
      'size_t retro_serialize_size() {\n'
      '    auto& system = Core::System::GetInstance();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Throw::Rescue emusuni_rescue{"saving a state"};\n'
      '    if (setjmp(emusuni_rescue.env) != 0) {\n'
      '        LOG_ERROR(Frontend, "EmuSuNi: saving a state stopped on a C++ exception ({}): no state",\n'
      '                  EmuSuNi::Throw::RescuedMessage());\n'
      '        savestate.reset();\n'
      '        return 0;\n'
      '    }\n'
      '    EmuSuNi::Throw::Arm(emusuni_rescue);\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"saving a state"};\n'
      '    struct EmuSuNiDisarm {\n'
      '        EmuSuNi::Throw::Rescue& rescue;\n'
      '        ~EmuSuNiDisarm() {\n'
      '            EmuSuNi::Throw::Disarm(rescue);\n'
      '        }\n'
      '    } emusuni_disarm{emusuni_rescue};\n'
      '#endif\n')
patch('src/citra_libretro/citra_libretro.cpp',
      'bool retro_unserialize(const void* data, size_t size) {\n'
      '    auto& system = Core::System::GetInstance();\n',
      'bool retro_unserialize(const void* data, size_t size) {\n'
      '    auto& system = Core::System::GetInstance();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Throw::Rescue emusuni_rescue{"loading a save state"};\n'
      '    if (setjmp(emusuni_rescue.env) != 0) {\n'
      '        LOG_CRITICAL(Frontend, "EmuSuNi: loading a save state stopped on a C++ exception ({}): the game ends",\n'
      '                     EmuSuNi::Throw::RescuedMessage());\n'
      '        LibRetro::DisplayMessage(EMUSUNI_RESCUED);\n'
      '        emu_instance->game_loaded = false;\n'
      '        LibRetro::Shutdown();\n'
      '        return false;\n'
      '    }\n'
      '    EmuSuNi::Throw::Arm(emusuni_rescue);\n'
      '    const EmuSuNi::Stats::Doing emusuni_doing{"loading a save state"};\n'
      '    struct EmuSuNiDisarm {\n'
      '        EmuSuNi::Throw::Rescue& rescue;\n'
      '        ~EmuSuNiDisarm() {\n'
      '            EmuSuNi::Throw::Disarm(rescue);\n'
      '        }\n'
      '    } emusuni_disarm{emusuni_rescue};\n'
      '#endif\n')
# The decompiler's failures without an exception (DecompileFail): the analysis and the generator note the first and
# stop, DecompileProgram returns "" as upstream's catch did (the program's draws then shaded on the CPU).
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      'class DecompileFail : public std::runtime_error {\npublic:\n    using std::runtime_error::runtime_error;\n};\n',
      'class DecompileFail : public std::runtime_error {\npublic:\n    using std::runtime_error::runtime_error;\n};\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (00.85): why this thread\'s decompilation failed (nullptr: it didn\'t), instead of DecompileFail\'s\n'
      '// exception (on the PS4 its catch is never reached: the app ended).\n'
      'thread_local const char* emusuni_fail = nullptr;\n'
      'static void EmuSuNiFail(const char* why) {\n'
      '    if (emusuni_fail == nullptr) {\n'
      '        emusuni_fail = why;\n'
      '    }\n'
      '}\n'
      '#define EMUSUNI_DECOMPILE_FAIL(why, ...) { EmuSuNiFail(why); return __VA_ARGS__; }\n'
      '#else\n'
      '#define EMUSUNI_DECOMPILE_FAIL(why, ...) throw DecompileFail(why);\n'
      '#endif\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '        if (program_main.exit_method != ExitMethod::AlwaysEnd)\n'
      '            throw DecompileFail("Program does not always end");\n',
      '        if (program_main.exit_method != ExitMethod::AlwaysEnd)\n'
      '            EMUSUNI_DECOMPILE_FAIL("Program does not always end")\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '        if (subroutine.exit_method == ExitMethod::Undetermined)\n'
      '            throw DecompileFail("Recursive function detected");\n',
      '        if (subroutine.exit_method == ExitMethod::Undetermined) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '            EmuSuNiFail("Recursive function detected"); // (kept, as upstream would have: the analysis ends)\n'
      '#else\n'
      '            throw DecompileFail("Recursive function detected");\n'
      '#endif\n'
      '        }\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '                          instr.opcode.Value().GetInfo().name, instr.hex);\n'
      '                throw DecompileFail("Unhandled instruction");\n'
      '                break;\n'
      '            }\n'
      '            }\n'
      '\n'
      '            break;\n'
      '        }\n'
      '\n'
      '        case OpCode::Type::MultiplyAdd: {\n',
      '                          instr.opcode.Value().GetInfo().name, instr.hex);\n'
      '                EMUSUNI_DECOMPILE_FAIL("Unhandled instruction", PROGRAM_END)\n'
      '                break;\n'
      '            }\n'
      '            }\n'
      '\n'
      '            break;\n'
      '        }\n'
      '\n'
      '        case OpCode::Type::MultiplyAdd: {\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '                LOG_ERROR(HW_GPU, "Unhandled multiply-add instruction: 0x{:02x} ({}): 0x{:08x}",\n'
      '                          (int)instr.opcode.Value().EffectiveOpCode(),\n'
      '                          instr.opcode.Value().GetInfo().name, instr.hex);\n'
      '                throw DecompileFail("Unhandled instruction");\n',
      '                LOG_ERROR(HW_GPU, "Unhandled multiply-add instruction: 0x{:02x} ({}): 0x{:08x}",\n'
      '                          (int)instr.opcode.Value().EffectiveOpCode(),\n'
      '                          instr.opcode.Value().GetInfo().name, instr.hex);\n'
      '                EMUSUNI_DECOMPILE_FAIL("Unhandled instruction", PROGRAM_END)\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '                LOG_ERROR(HW_GPU, "Unhandled instruction: 0x{:02x} ({}): 0x{:08x}",\n'
      '                          (int)instr.opcode.Value().EffectiveOpCode(),\n'
      '                          instr.opcode.Value().GetInfo().name, instr.hex);\n'
      '                throw DecompileFail("Unhandled instruction");\n',
      '                LOG_ERROR(HW_GPU, "Unhandled instruction: 0x{:02x} ({}): 0x{:08x}",\n'
      '                          (int)instr.opcode.Value().EffectiveOpCode(),\n'
      '                          instr.opcode.Value().GetInfo().name, instr.hex);\n'
      '                EMUSUNI_DECOMPILE_FAIL("Unhandled instruction", PROGRAM_END)\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '    try {\n'
      '        auto subroutines = ControlFlowAnalyzer(program_code, main_offset).MoveSubroutines();\n'
      '        GLSLGenerator generator(subroutines, program_code, swizzle_data, main_offset,\n'
      '                                inputreg_getter, outputreg_getter, sanitize_mul);\n'
      '        return generator.MoveShaderCode();\n'
      '    } catch (const DecompileFail& exception) {\n'
      '        LOG_INFO(HW_GPU, "Shader decompilation failed: {}", exception.what());\n'
      '        return "";\n'
      '    }\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.85): the failure noted, not thrown (emusuni_fail); the same outcome as upstream\'s catch.\n'
      '    emusuni_fail = nullptr;\n'
      '#if !defined(__ORBIS__)\n'
      '    // (The PC tests: EMUSUNI_3DS_TEST_DECOMPILE_FAIL=1 fails every program, as one the decompiler can\'t take.)\n'
      '    static const bool emusuni_test_fail = std::getenv("EMUSUNI_3DS_TEST_DECOMPILE_FAIL") != nullptr;\n'
      '    if (emusuni_test_fail) {\n'
      '        EmuSuNiFail("EMUSUNI_3DS_TEST_DECOMPILE_FAIL");\n'
      '    }\n'
      '#endif\n'
      '    auto subroutines = ControlFlowAnalyzer(program_code, main_offset).MoveSubroutines();\n'
      '    if (emusuni_fail == nullptr) {\n'
      '        GLSLGenerator generator(subroutines, program_code, swizzle_data, main_offset, inputreg_getter,\n'
      '                                outputreg_getter, sanitize_mul);\n'
      '        if (emusuni_fail == nullptr) {\n'
      '            return generator.MoveShaderCode();\n'
      '        }\n'
      '    }\n'
      '    LOG_INFO(HW_GPU, "Shader decompilation failed: {} (EmuSuNi: shaded on the CPU instead)", emusuni_fail);\n'
      '    emusuni_fail = nullptr;\n'
      '    return "";\n'
      '#else\n'
      '    try {\n'
      '        auto subroutines = ControlFlowAnalyzer(program_code, main_offset).MoveSubroutines();\n'
      '        GLSLGenerator generator(subroutines, program_code, swizzle_data, main_offset,\n'
      '                                inputreg_getter, outputreg_getter, sanitize_mul);\n'
      '        return generator.MoveShaderCode();\n'
      '    } catch (const DecompileFail& exception) {\n'
      '        LOG_INFO(HW_GPU, "Shader decompilation failed: {}", exception.what());\n'
      '        return "";\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '#include <map>\n#include <set>\n',
      '#include <map>\n#include <set>\n#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#endif\n')
# (The analysis stops at the first failure: every scan after it ends at once.)
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '        ExitMethod& exit_method = iter->second;\n'
      '        if (!inserted)\n'
      '            return exit_method;\n',
      '        ExitMethod& exit_method = iter->second;\n'
      '        if (!inserted)\n'
      '            return exit_method;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (emusuni_fail != nullptr) {\n'
      '            return exit_method = ExitMethod::AlwaysEnd; // (the result is thrown away)\n'
      '        }\n'
      '#endif\n')
# The shader JIT: its code memory taken from the pool first; none, the interpreter - no Xbyak exception to catch.
patch('src/video_core/shader/shader_jit_x64_compiler.h',
      'class JitShader : public Xbyak::CodeGenerator {\npublic:\n    JitShader();\n',
      'class JitShader : public Xbyak::CodeGenerator {\npublic:\n    JitShader();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (00.85): the code written in `code`, MAX_SHADER_SIZE bytes of the JITs\' memory, given back\n'
      '    /// (EmuSuNi::JitFree) by the destructor.\n'
      '    explicit JitShader(void* code);\n'
      '    ~JitShader() override;\n'
      '    void* emusuni_code = nullptr;\n'
      '#endif\n')
patch('src/video_core/shader/shader_jit_x64_compiler.cpp',
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE, nullptr, EmuSuNi::XbyakAllocator()) {}\n',
      'JitShader::JitShader() : Xbyak::CodeGenerator(MAX_SHADER_SIZE, nullptr, EmuSuNi::XbyakAllocator()) {}\n'
      '// (00.85: a buffer of the user\'s, for Xbyak: nothing allocated, nothing thrown.)\n'
      'JitShader::JitShader(void* code) : Xbyak::CodeGenerator(MAX_SHADER_SIZE, code), emusuni_code(code) {}\n'
      'JitShader::~JitShader() {\n'
      '    if (emusuni_code != nullptr) {\n'
      '        EmuSuNi::JitFree(emusuni_code);\n'
      '    }\n'
      '}\n')
patch('src/video_core/shader/shader_jit.cpp',
      '        std::unique_ptr<JitShader> shader;\n'
      '        try {\n'
      '            shader = std::make_unique<JitShader>();\n'
      '        } catch (const Xbyak::Error& error) {\n'
      '            LOG_ERROR(HW_GPU, "EmuSuNi: no JIT memory for shader program {:016X} ({}): interpreted", cache_key,\n'
      '                      error.what());\n'
      '            emusuni_interpreted.insert(cache_key);\n'
      '            setup.cached_shader = nullptr;\n'
      '            return;\n'
      '        }\n',
      '        // (00.85: the memory taken first; Xbyak\'s exception\'s catch is never reached on the PS4.)\n'
      '        void* const emusuni_code = EmuSuNi::JitAlloc(MAX_SHADER_SIZE);\n'
      '        if (emusuni_code == nullptr) {\n'
      '            LOG_ERROR(HW_GPU, "EmuSuNi: no JIT memory for shader program {:016X}: interpreted", cache_key);\n'
      '            emusuni_interpreted.insert(cache_key);\n'
      '            setup.cached_shader = nullptr;\n'
      '            return;\n'
      '        }\n'
      '        auto shader = std::make_unique<JitShader>(emusuni_code);\n')
patch('src/video_core/shader/shader_jit.cpp',
      '#include "common/microprofile.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/logging/log.h"\n#endif\n',
      '#include "common/microprofile.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_jit.h"\n#include "common/logging/log.h"\n#endif\n')
# dynarmic's block of code: the size asked of the pool first (a block taken and given back), never an exception to
# catch; nothing at all is still an exception (the rescue point: the game ends).
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    for (;;) {\n'
      '        try {\n'
      '            return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '        } catch (const std::exception& error) {\n'
      '            if (config.code_cache_size < 8u << 20) {\n'
      '                throw std::runtime_error(fmt::format("no executable memory for the CPU JIT ({} MiB: {})",\n'
      '                                                     config.code_cache_size >> 20, error.what()));\n'
      '            }\n'
      '            LOG_WARNING(Core_ARM11, "EmuSuNi: no {} MiB of JIT memory for core {} ({}): {} MiB tried",\n'
      '                        config.code_cache_size >> 20, GetID(), error.what(), config.code_cache_size >> 21);\n'
      '            config.code_cache_size /= 2;\n'
      '        }\n'
      '    }\n',
      '    // (00.85: the size asked of the pool first, a block taken and given back: on the PS4 an exception\'s catch is\n'
      '    // never reached.)\n'
      '    for (;;) {\n'
      '        void* const emusuni_probe = EmuSuNi::JitAlloc(config.code_cache_size);\n'
      '        if (emusuni_probe != nullptr) {\n'
      '            EmuSuNi::JitFree(emusuni_probe);\n'
      '            return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '        }\n'
      '        if (config.code_cache_size < 8u << 20) {\n'
      '            throw std::runtime_error(\n'
      '                fmt::format("no executable memory for the CPU JIT ({} MiB)", config.code_cache_size >> 20));\n'
      '        }\n'
      '        LOG_WARNING(Core_ARM11, "EmuSuNi: no {} MiB of JIT memory for core {}: {} MiB tried",\n'
      '                    config.code_cache_size >> 20, GetID(), config.code_cache_size >> 21);\n'
      '        config.code_cache_size /= 2;\n'
      '    }\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include <stdexcept>\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include <stdexcept>\n#include "common/emusuni_jit.h"\n')
# A shader module, the driver's pipeline cache and the stream buffers by their results, not vulkan.hpp's exceptions.
patch('src/video_core/renderer_vulkan/vk_shader_util.cpp',
      '    try {\n'
      '        return device.createShaderModule(shader_info);\n'
      '    } catch (vk::SystemError& err) {\n'
      '        LOG_ERROR(Render_Vulkan, "{}", err.what());\n'
      '    }\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.85): by its result (on the PS4 an exception\'s catch is never reached).\n'
      '    vk::ShaderModule emusuni_module{};\n'
      '    if (const vk::Result result = device.createShaderModule(&shader_info, nullptr, &emusuni_module);\n'
      '        result != vk::Result::eSuccess) {\n'
      '        LOG_ERROR(Render_Vulkan, "vkCreateShaderModule: {}", vk::to_string(result));\n'
      '        return {};\n'
      '    }\n'
      '    return emusuni_module;\n'
      '#else\n'
      '    try {\n'
      '        return device.createShaderModule(shader_info);\n'
      '    } catch (vk::SystemError& err) {\n'
      '        LOG_ERROR(Render_Vulkan, "{}", err.what());\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '        const vk::Device device = instance.GetDevice();\n'
      '        try {\n'
      '            driver_pipeline_cache = device.createPipelineCacheUnique(cache_info);\n'
      '        } catch (const vk::SystemError& err) {\n'
      '            LOG_ERROR(Render_Vulkan, "Failed to create pipeline cache: {}", err.what());\n'
      '            if (allow_fallback) {\n'
      '                // Fall back to empty cache\n'
      '                cache_info.initialDataSize = 0;\n'
      '                cache_info.pInitialData = nullptr;\n'
      '                try {\n'
      '                    driver_pipeline_cache = device.createPipelineCacheUnique(cache_info);\n'
      '                } catch (const vk::SystemError& err) {\n'
      '                    LOG_ERROR(Render_Vulkan, "Failed to create fallback pipeline cache: {}",\n'
      '                              err.what());\n'
      '                }\n'
      '            }\n'
      '        }\n',
      '        const vk::Device device = instance.GetDevice();\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (00.85): by its result (on the PS4 an exception\'s catch is never reached), the same fallback.\n'
      '        const auto emusuni_create = [&]() {\n'
      '            vk::PipelineCache cache{};\n'
      '            const vk::Result result = device.createPipelineCache(&cache_info, nullptr, &cache);\n'
      '            if (result == vk::Result::eSuccess) {\n'
      '                driver_pipeline_cache = vk::UniquePipelineCache{\n'
      '                    cache, vk::detail::ObjectDestroy<vk::Device, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>{device}};\n'
      '            }\n'
      '            return result;\n'
      '        };\n'
      '        if (const vk::Result result = emusuni_create(); result != vk::Result::eSuccess) {\n'
      '            LOG_ERROR(Render_Vulkan, "Failed to create pipeline cache: {}", vk::to_string(result));\n'
      '            if (allow_fallback) {\n'
      '                // Fall back to empty cache\n'
      '                cache_info.initialDataSize = 0;\n'
      '                cache_info.pInitialData = nullptr;\n'
      '                if (const vk::Result again = emusuni_create(); again != vk::Result::eSuccess) {\n'
      '                    LOG_ERROR(Render_Vulkan, "Failed to create fallback pipeline cache: {}",\n'
      '                              vk::to_string(again));\n'
      '                }\n'
      '            }\n'
      '        }\n'
      '#else\n'
      '        try {\n'
      '            driver_pipeline_cache = device.createPipelineCacheUnique(cache_info);\n'
      '        } catch (const vk::SystemError& err) {\n'
      '            LOG_ERROR(Render_Vulkan, "Failed to create pipeline cache: {}", err.what());\n'
      '            if (allow_fallback) {\n'
      '                // Fall back to empty cache\n'
      '                cache_info.initialDataSize = 0;\n'
      '                cache_info.pInitialData = nullptr;\n'
      '                try {\n'
      '                    driver_pipeline_cache = device.createPipelineCacheUnique(cache_info);\n'
      '                } catch (const vk::SystemError& err) {\n'
      '                    LOG_ERROR(Render_Vulkan, "Failed to create fallback pipeline cache: {}",\n'
      '                              err.what());\n'
      '                }\n'
      '            }\n'
      '        }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_stream_buffer.cpp',
      '    while (attempt_size >= min_buffer_size) {\n        try {\n',
      '    while (attempt_size >= min_buffer_size) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (00.85): the same tries by their results (on the PS4 an exception\'s catch is never reached).\n'
      '        {\n'
      '            const vk::BufferCreateInfo emusuni_buffer_info = {\n'
      '                .size = attempt_size,\n'
      '                .usage = usage,\n'
      '            };\n'
      '            vk::Buffer new_buffer{};\n'
      '            if (device.createBuffer(&emusuni_buffer_info, nullptr, &new_buffer) != vk::Result::eSuccess) {\n'
      '                attempt_size /= 2;\n'
      '                continue;\n'
      '            }\n'
      '            buffer = new_buffer;\n'
      '            const auto requirements_chain = device.getBufferMemoryRequirements2<\n'
      '                vk::MemoryRequirements2, vk::MemoryDedicatedRequirements>({.buffer = buffer});\n'
      '            const auto& requirements = requirements_chain.get<vk::MemoryRequirements2>();\n'
      '            const auto& dedicated_requirements =\n'
      '                requirements_chain.get<vk::MemoryDedicatedRequirements>();\n'
      '            const vk::MemoryDedicatedAllocateInfo dedicated_info = {.buffer = buffer};\n'
      '            const vk::MemoryAllocateInfo alloc_info = {\n'
      '                .pNext = dedicated_requirements.prefersDedicatedAllocation ? &dedicated_info : nullptr,\n'
      '                .allocationSize = requirements.memoryRequirements.size,\n'
      '                .memoryTypeIndex = preferred_type,\n'
      '            };\n'
      '            vk::DeviceMemory new_memory{};\n'
      '            void* new_mapped = nullptr;\n'
      '            if (device.allocateMemory(&alloc_info, nullptr, &new_memory) != vk::Result::eSuccess) {\n'
      '                device.destroyBuffer(buffer);\n'
      '                buffer = VK_NULL_HANDLE;\n'
      '                attempt_size /= 2;\n'
      '                continue;\n'
      '            }\n'
      '            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBindBufferMemory(static_cast<VkDevice>(device),\n'
      '                                                                 static_cast<VkBuffer>(buffer),\n'
      '                                                                 static_cast<VkDeviceMemory>(new_memory),\n'
      '                                                                 0) != VK_SUCCESS ||\n'
      '                device.mapMemory(new_memory, 0, requirements.memoryRequirements.size, {}, &new_mapped) !=\n'
      '                    vk::Result::eSuccess) {\n'
      '                device.freeMemory(new_memory);\n'
      '                device.destroyBuffer(buffer);\n'
      '                buffer = VK_NULL_HANDLE;\n'
      '                attempt_size /= 2;\n'
      '                continue;\n'
      '            }\n'
      '            memory = new_memory;\n'
      '            mapped = reinterpret_cast<u8*>(new_mapped);\n'
      '            stream_buffer_size = static_cast<u64>(attempt_size);\n'
      '            LOG_INFO(Render_Vulkan, "Created {} buffer with size {} KiB (flags {})", BufferTypeName(type),\n'
      '                     stream_buffer_size / 1024, vk::to_string(mem_type.propertyFlags));\n'
      '            return;\n'
      '        }\n'
      '#endif\n'
      '        try {\n')
# The GPU timing emulation per game (Luigi's Mansion 2: the app's list), as an option of the core's own.
patch('src/citra_libretro/core_settings.cpp',
      'static constexpr const char* emusuni_right_eye = "citra_emusuni_right_eye"; // (EmuSuNi\'s: the right eye drawn)\n',
      'static constexpr const char* emusuni_right_eye = "citra_emusuni_right_eye"; // (EmuSuNi\'s: the right eye drawn)\n'
      '// (00.85, EmuSuNi\'s: Azahar\'s GPU timing emulation, simulate_3ds_gpu_timings)\n'
      'static constexpr const char* emusuni_gpu_timings = "citra_emusuni_gpu_timings";\n')
patch('src/citra_libretro/core_settings.cpp',
      '        "Draw the right eye\'s picture of the 3D (never shown on the PS4; a few games need it drawn).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n',
      '        "Draw the right eye\'s picture of the 3D (never shown on the PS4; a few games need it drawn).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n'
      '    {\n'
      '        config::graphics::emusuni_gpu_timings,\n'
      '        "GPU Timings (EmuSuNi)",\n'
      '        "GPU Timings",\n'
      '        "Emulate the time the 3DS\'s GPU takes (Luigi\'s Mansion 2 needs it).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n')
patch('src/citra_libretro/core_settings.cpp',
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for (the object shows a few\n',
      '    // (00.85) Azahar\'s GPU timing emulation (azahar #2095): the app\'s option for the games that need it, or the\n'
      '    // file /data/EmuSuNi/3ds-gpu-timings (EMUSUNI_3DS_GPU_TIMINGS=1 on the PC) for every game.\n'
      '    Settings::values.simulate_3ds_gpu_timings =\n'
      '        LibRetro::FetchVariable(config::graphics::emusuni_gpu_timings, config::disabled) == config::enabled ||\n'
      '        std::getenv("EMUSUNI_3DS_GPU_TIMINGS") != nullptr || ::access("/data/EmuSuNi/3ds-gpu-timings", F_OK) == 0;\n'
      '    {\n'
      '        static int said = -1; // (said once, and again if it changes)\n'
      '        const int now = Settings::values.simulate_3ds_gpu_timings.GetValue() ? 1 : 0;\n'
      '        if (now != said) {\n'
      '            LOG_INFO(Frontend, "EmuSuNi: GPU timings: {}",\n'
      '                     now ? "emulated (the game\'s list, or the file /data/EmuSuNi/3ds-gpu-timings)"\n'
      '                         : "not emulated (upstream\'s default)");\n'
      '            said = now;\n'
      '        }\n'
      '    }\n'
      '    // EmuSuNi: a draw whose pipeline isn\'t built yet is skipped rather than waited for (the object shows a few\n')
# The descriptor pools' growth in the log with the sets the heap has made.
patch('src/video_core/renderer_vulkan/vk_resource_pool.cpp',
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one! ({} sets each, {} pools now)",\n'
      '                         descriptor_heap_count, pools.size() + 1);\n',
      '                LOG_INFO(Render_Vulkan, "Run out of pools, creating new one! ({} sets each, {} pools now; {} sets "\n'
      '                                        "in the heap\'s ring)",\n'
      '                         descriptor_heap_count, pools.size() + 1, end);\n')

# --- 00.89: the players' graphics errors, from upstream's fixes after 2126.2 and a test switch. The players of v00.22:
# Pokémon X, Y and Omega Ruby "almost all black", Castlevania: Mirror of Fate dark and without textures in the
# pictures (#23), Luigi's Mansion 2's menu in loud green (#39); a player who ran Azahar under Linux on a PS4 Pro saw
# glitches with Vulkan that OpenGL didn't have (#29). On Vulkan Azahar writes the fragment shaders' SPIR-V itself (its
# "SPIR-V shader generation", on by default; the libretro core has no option for it), on OpenGL through GLSL. Three
# changes:
# - Upstream's PR #2590 as merged (2026-09-24, after 2126.2; azahar #1233, Captain Toad's shadows): in the SPIR-V
#   generator a light's shadow darkened its ambient too (the GLSL generator's: only the diffuse), so a lit object in a
#   game's shadow went black instead of ambient-lit; and a shadow texture is read through texture unit 0 (sampled, as
#   the rasterizer binds it there) instead of a storage image.
# - Upstream's PR #2627 as merged (2026-10-12; azahar #2471): Vulkan uploaded the procedural textures' tables only when
#   their colour table changed; a new noise table, colour map or alpha map alone never reached the GPU.
# - The file /data/EmuSuNi/3ds-glsl-shaders (EMUSUNI_3DS_GLSL_SHADERS=1 on the PC): the fragment shaders through the
#   GLSL generator and glslang, as on OpenGL (Azahar's SPIR-V shader generation off), to tell whether a game's broken
#   pictures come from the SPIR-V generator on the PS4's GPU. Slower to compile (more pop-in with async shaders). Its
#   shaders are kept in a disk cache of their own (shaders/vulkan/transferable-glsl): with upstream's single cache a
#   change of generator recompiled every saved shader of the game at its start, one at a time (a long load).
patch('src/video_core/shader/generator/spv_fs_shader_gen.cpp',
      '        const Id diffuse_mul_dot{OpVectorTimesScalar(vec_ids.Get(3), light_diffuse, dot_product)};\n'
      '\n'
      '        // Compute primary fragment color (diffuse lighting) function\n'
      '        Id diffuse_sum_rgb{OpFAdd(vec_ids.Get(3), diffuse_mul_dot, light_ambient)};\n'
      '        diffuse_sum_rgb = OpVectorTimesScalar(vec_ids.Get(3), diffuse_sum_rgb, dist_atten);\n'
      '        diffuse_sum_rgb = OpVectorTimesScalar(vec_ids.Get(3), diffuse_sum_rgb, spot_atten);\n'
      '        if (shadow_primary_enable) {\n'
      '            diffuse_sum_rgb = OpFMul(vec_ids.Get(3), diffuse_sum_rgb, shadow_rgb);\n'
      '        }\n',
      '        Id diffuse_mul_dot{OpVectorTimesScalar(vec_ids.Get(3), light_diffuse, dot_product)};\n'
      '\n'
      '        // Shadow only attenuates diffuse, not the light\'s ambient\n'
      '        if (shadow_primary_enable) {\n'
      '            diffuse_mul_dot = OpFMul(vec_ids.Get(3), diffuse_mul_dot, shadow_rgb);\n'
      '        }\n'
      '\n'
      '        // Compute primary fragment color (diffuse lighting) function\n'
      '        Id diffuse_sum_rgb{OpFAdd(vec_ids.Get(3), diffuse_mul_dot, light_ambient)};\n'
      '        diffuse_sum_rgb = OpVectorTimesScalar(vec_ids.Get(3), diffuse_sum_rgb, dist_atten);\n'
      '        diffuse_sum_rgb = OpVectorTimesScalar(vec_ids.Get(3), diffuse_sum_rgb, spot_atten);\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.cpp',
      '    const Id z{OpBitcast(u32_id, z_i32)};\n'
      '    const Id shadow_texture_px{OpLoad(image_r32_id, shadow_texture_px_id)};\n'
      '    const Id px_size{OpImageQuerySize(ivec_ids.Get(2), shadow_texture_px)};\n',
      '    const Id z{OpBitcast(u32_id, z_i32)};\n'
      '\n'
      '    const Id sampled_image{OpLoad(TypeSampledImage(image2d_u32_id), tex0_id)};\n'
      '    const Id shadow_texture{OpImage(image2d_u32_id, sampled_image)};\n'
      '    const Id px_size{OpImageQuerySizeLod(ivec_ids.Get(2), shadow_texture, ConstS32(0))};\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.cpp',
      '        const Id px_texel{OpImageRead(uvec_ids.Get(4), shadow_texture_px, uv)};\n',
      '        const Id px_texel{OpImageFetch(uvec_ids.Get(4), shadow_texture, uv,\n'
      '                                       spv::ImageOperandsMask::Lod, ConstS32(0))};\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.cpp',
      '    image_r32_id = TypeImage(u32_id, spv::Dim::Dim2D, 0, 0, 0, 2, spv::ImageFormat::R32ui);\n',
      '    image2d_u32_id = TypeImage(u32_id, spv::Dim::Dim2D, 0, 0, 0, 1, spv::ImageFormat::Unknown);\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.cpp',
      '    const auto tex0_type = texture_type == TextureType::TextureCube ? image_cube_id : image2d_id;\n'
      '    tex0_id = DefineUniformConst(TypeSampledImage(tex0_type), 1, 0);\n',
      '    Id tex0_type{image2d_id};\n'
      '    if (texture_type == TextureType::TextureCube) {\n'
      '        tex0_type = image_cube_id;\n'
      '    } else if (texture_type == TextureType::Shadow2D) {\n'
      '        tex0_type = image2d_u32_id;\n'
      '    }\n'
      '    tex0_id = DefineUniformConst(TypeSampledImage(tex0_type), 1, 0);\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.cpp',
      '    tex2_id = DefineUniformConst(TypeSampledImage(image2d_id), 1, 2);\n'
      '\n'
      '    // Define shadow textures\n'
      '    shadow_texture_px_id = DefineUniformConst(image_r32_id, 2, 0, true);\n',
      '    tex2_id = DefineUniformConst(TypeSampledImage(image2d_id), 1, 2);\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.h',
      '    Id image_r32_id{};\n',
      '    Id image2d_u32_id{};\n')
patch('src/video_core/shader/generator/spv_fs_shader_gen.h',
      '    Id texture_buffer_lut_rgba_id{};\n    Id shadow_texture_px_id{};\n',
      '    Id texture_buffer_lut_rgba_id{};\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    if (!pica.proctex.lut_dirty) {\n        return;\n    }\n',
      '    if (!pica.proctex.table_dirty) {\n        return;\n    }\n')
patch('src/citra_libretro/core_settings.cpp',
      '    // (00.85) Azahar\'s GPU timing emulation (azahar #2095): the app\'s option for the games that need it, or the\n',
      r'''    // (00.89) The fragment shaders through the GLSL generator and glslang, as on OpenGL, with the file
    // /data/EmuSuNi/3ds-glsl-shaders (EMUSUNI_3DS_GLSL_SHADERS=1 on the PC); Azahar's own SPIR-V generator otherwise.
    // Read when the renderer is made (the game's start).
    Settings::values.spirv_shader_gen = std::getenv("EMUSUNI_3DS_GLSL_SHADERS") == nullptr &&
                                        ::access("/data/EmuSuNi/3ds-glsl-shaders", F_OK) != 0;
    {
        static int said = -1; // (said once, and again if it changes)
        const int now = Settings::values.spirv_shader_gen.GetValue() ? 1 : 0;
        if (now != said) {
            LOG_INFO(Frontend, "EmuSuNi: fragment shaders: {}",
                     now ? "Azahar's SPIR-V generator (the file /data/EmuSuNi/3ds-glsl-shaders: GLSL instead)"
                         : "GLSL through glslang, as on OpenGL (the file /data/EmuSuNi/3ds-glsl-shaders); "
                           "their own disk cache (transferable-glsl)");
            said = now;
        }
    }
    // (00.85) Azahar's GPU timing emulation (azahar #2095): the app's option for the games that need it, or the
''')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'std::string PipelineCache::GetTransferableDir() const {\n'
      '    return GetVulkanDir() + DIR_SEP + "transferable";\n'
      '}\n',
      'std::string PipelineCache::GetTransferableDir() const {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the GLSL generator\'s shaders (the file /data/EmuSuNi/3ds-glsl-shaders) in a cache of their own, so\n'
      '    // that a change of generator doesn\'t recompile the game\'s whole cache at its start (scripts/build-3ds-core.sh).\n'
      '    if (!profile.vk_use_spirv_generator) {\n'
      '        return GetVulkanDir() + DIR_SEP + "transferable-glsl";\n'
      '    }\n'
      '#endif\n'
      '    return GetVulkanDir() + DIR_SEP + "transferable";\n'
      '}\n')

# --- 00.89: less work on the emulation thread between the ARM11's cores. The core loop (core.cpp RunLoop) runs the two
# emulated cores one after the other on the same thread and calls KernelSystem::SetRunningCPU four times a slice, many
# times a frame; each call copied the running process and its page table (shared_ptr copies: atomic adds) into the
# kernel, the memory system and the CPU, and the CPU saved its whole register context out of its JIT, looked the page
# table's JIT up in a map and loaded the same context back - for the same process and page table nearly every time
# (both cores run the game's process). This PC, Super Mario 3D Land: SetRunningCPU and ARM_Dynarmic::SetPageTable 3.1% of
# the emulation thread. Now what is already in place is left as it is (the same state after each call). And the
# exclusive monitor's lock (every LDREX/STREX through dynarmic's callbacks, and the JIT's own exclusive accesses) is
# let go with a release store instead of xchg and mfence, two full barriers (SpinLock::Unlock 1.6% of the thread):
# on x86 a plain store keeps the holder's earlier stores before it, all a lock needs (both cores run on the one
# emulation thread anyway).
patch('src/core/hle/kernel/kernel.cpp',
      'void KernelSystem::SetRunningCPU(Core::ARM_Interface* cpu) {\n'
      '    if (current_process) {\n'
      '        stored_processes[current_cpu->GetID()] = current_process;\n'
      '    }\n'
      '    current_cpu = cpu;\n'
      '    timing.SetCurrentTimer(cpu->GetID());\n'
      '    if (stored_processes[current_cpu->GetID()]) {\n'
      '        SetCurrentProcess(stored_processes[current_cpu->GetID()]);\n'
      '    }\n'
      '}\n',
      'void KernelSystem::SetRunningCPU(Core::ARM_Interface* cpu) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: upstream\'s steps, those that change nothing left out (scripts/build-3ds-core.sh, 00.89).\n'
      '    if (current_process && stored_processes[current_cpu->GetID()] != current_process) {\n'
      '        stored_processes[current_cpu->GetID()] = current_process;\n'
      '    }\n'
      '    current_cpu = cpu;\n'
      '    timing.SetCurrentTimer(cpu->GetID());\n'
      '    const std::shared_ptr<Process>& next = stored_processes[current_cpu->GetID()];\n'
      '    if (next) {\n'
      '        if (current_process != next) {\n'
      '            current_process = next;\n'
      '        }\n'
      '        memory.SetCurrentPageTable(next->vm_manager.page_table);\n'
      '        current_cpu->SetPageTable(next->vm_manager.page_table);\n'
      '    }\n'
      '#else\n'
      '    if (current_process) {\n'
      '        stored_processes[current_cpu->GetID()] = current_process;\n'
      '    }\n'
      '    current_cpu = cpu;\n'
      '    timing.SetCurrentTimer(cpu->GetID());\n'
      '    if (stored_processes[current_cpu->GetID()]) {\n'
      '        SetCurrentProcess(stored_processes[current_cpu->GetID()]);\n'
      '    }\n'
      '#endif\n'
      '}\n')
patch('src/core/memory.h',
      '    void SetCurrentPageTable(std::shared_ptr<PageTable> page_table);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    void SetCurrentPageTable(const std::shared_ptr<PageTable>& page_table);\n'
      '#else\n'
      '    void SetCurrentPageTable(std::shared_ptr<PageTable> page_table);\n'
      '#endif\n')
patch('src/core/memory.cpp',
      'void MemorySystem::SetCurrentPageTable(std::shared_ptr<PageTable> page_table) {\n'
      '    impl->current_page_table = page_table;\n'
      '}\n',
      '#ifdef EMUSUNI_PORT\n'
      'void MemorySystem::SetCurrentPageTable(const std::shared_ptr<PageTable>& page_table) {\n'
      '    // (EmuSuNi: the same table left as it is, no copy.)\n'
      '    if (impl->current_page_table != page_table) {\n'
      '        impl->current_page_table = page_table;\n'
      '    }\n'
      '}\n'
      '#else\n'
      'void MemorySystem::SetCurrentPageTable(std::shared_ptr<PageTable> page_table) {\n'
      '    impl->current_page_table = page_table;\n'
      '}\n'
      '#endif\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      'void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {\n'
      '    current_page_table = page_table;\n',
      'void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the page table whose JIT already runs (jit is always current_page_table\'s): nothing to do;\n'
      '    // upstream saved the context and loaded it back into the same JIT.\n'
      '    if (jit != nullptr && page_table == current_page_table) {\n'
      '        return;\n'
      '    }\n'
      '#endif\n'
      '    current_page_table = page_table;\n')
patch('externals/dynarmic/src/dynarmic/common/spin_lock_x64.cpp',
      'void SpinLock::Unlock() {\n'
      '    __atomic_exchange_n(&storage, 0, __ATOMIC_SEQ_CST);\n'
      '    __builtin_ia32_mfence();\n'
      '}\n',
      'void SpinLock::Unlock() {\n'
      '    // (00.89: a release store, a plain mov on x86; it was xchg and mfence.)\n'
      '    __atomic_store_n(&storage, 0, __ATOMIC_RELEASE);\n'
      '}\n')
patch('externals/dynarmic/src/dynarmic/common/spin_lock_x64.cpp',
      'void EmitSpinLockUnlock(Xbyak::CodeGenerator& code, Xbyak::Reg64 ptr, Xbyak::Reg32 tmp) {\n'
      '    code.xor_(tmp, tmp);\n'
      '    code.xchg(code.dword[ptr], tmp);\n'
      '    code.mfence();\n'
      '}\n',
      'void EmitSpinLockUnlock(Xbyak::CodeGenerator& code, Xbyak::Reg64 ptr, Xbyak::Reg32 tmp) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.89): a release store, as SpinLock::Unlock (x86 keeps the holder\'s stores before it).\n'
      '    (void)tmp;\n'
      '    code.mov(code.dword[ptr], 0);\n'
      '#else\n'
      '    code.xor_(tmp, tmp);\n'
      '    code.xchg(code.dword[ptr], tmp);\n'
      '    code.mfence();\n'
      '#endif\n'
      '}\n')

# --- 00.90: the first play of a game on the PS4 (the owner's log, 02-11: Kirby: Planet Robobot, Donkey Kong Country Returns
# 3D, Pokémon Ultra Sun). Its pipelines are built for the first time there, 50-250 ms each on the Jaguar (Kirby's first
# minute: 94 on the workers in 24 s, 52 on the emulation thread in 2.7 s), while the draws that need them wait or are
# skipped (objects missing, black where the scenery should be, until they are built). Five changes:
# - A draw that waits for its pipeline (6 vertices or fewer: the 2D parts and full-screen quads, Azahar's async rule;
#   every draw with the file 3ds-sync-shaders) puts it at the FRONT of the workers' queue, and the Vulkan worker, when
#   it gets to that draw, builds it itself if no worker has started it yet. Upstream (and EmuSuNi until 00.89) queued it
#   at the back and the Vulkan worker waited for it (WaitDone) behind every pipeline queued before it - Kirby stopped
#   2.2-5.2 s at a time ("waiting for the Vulkan worker ... it may wait for a pipeline") - or built it on the emulation
#   thread, one after another (one of Kirby's frames built 21 of them there: 1.3 s). Now several are built at once, by
#   the free workers and the Vulkan worker, while the emulation goes on. (Skipping those draws instead, as the bigger
#   ones are, was tried: the backgrounds are such quads, and a frame showed the bottom screen over the top one.) A
#   bigger draw, skipped while its pipeline waits, puts it at the front too the next time it asks: what is on screen
#   now is built before what earlier scenes queued (Pokémon's models showed black parts while theirs waited).
# - 4 pipeline and 4 shader workers (3 before): they run below every other thread of the game, on the five cores the
#   emulation doesn't use, so a burst of new pipelines is built a quarter faster where those cores idle.
# - The ARM11's code caches: the game's core (core 0) gets 40 MiB of the JIT pool and the others 8 MiB each (24 + 8 on
#   a New 3DS), the same 48 MiB as the 24 MiB each before. A big game filled core 0's 24 MiB and dynarmic threw ALL of
#   its compiled code away to start again (Super Smash Bros. on this PC, ~3600 frames in: "1 cache flushes", every
#   block compiled anew - on the PS4 seconds of stutter); the other core never filled 8 MiB here. With 40 MiB none of
#   Smash, Kirby, Donkey Kong Country Returns, Pokémon X and Ultra Sun filled it in 6000 frames.
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    // EmuSuNi: a share of the 64 MiB pool (common/emusuni_jit.cpp) for each emulated core (upstream\n'
      '    // reserved 128 MiB each): 24 MiB on an Old 3DS (2 cores), 12 MiB on a New 3DS (4 cores).\n'
      '    config.code_cache_size = Settings::values.is_new_3ds.GetValue() ? 12u << 20 : 24u << 20;\n',
      '    // EmuSuNi: a share of the 64 MiB pool (common/emusuni_jit.cpp) for each emulated core (upstream\n'
      '    // reserved 128 MiB each): the game\'s core (0) 40 MiB on an Old 3DS and 24 on a New 3DS, the others 8 MiB\n'
      '    // each (00.90; 24 and 12 MiB each before: a big game filled core 0\'s and lost all its compiled code).\n'
      '    config.code_cache_size =\n'
      '        GetID() == 0 ? (Settings::values.is_new_3ds.GetValue() ? 24u << 20 : 40u << 20) : 8u << 20;\n')
# - Fast memory's crash marker: one start that died while trying it (the app closed with the PS button in a 3DS game's
#   first 30 s was enough) turned it off for good; the owner's 00.90 ran every 3DS game with the page table. Now the
#   markers say which build of the core wrote them (its compile time): one another build left is removed and fast
#   memory tried again; the "trying" marker goes after 10 s of a game (600 frames; it was 1800).
patch('src/common/thread_worker.h',
      '    void QueueWork(Task work) {\n'
      '        {\n'
      '            std::unique_lock lock{queue_mutex};\n'
      '            requests.emplace(std::move(work));\n'
      '            ++work_scheduled;\n'
      '        }\n'
      '        condition.notify_one();\n'
      '    }\n',
      '    void QueueWork(Task work) {\n'
      '        {\n'
      '            std::unique_lock lock{queue_mutex};\n'
      '#ifdef EMUSUNI_PORT\n'
      '            requests.emplace_back(std::move(work));\n'
      '#else\n'
      '            requests.emplace(std::move(work));\n'
      '#endif\n'
      '            ++work_scheduled;\n'
      '        }\n'
      '        condition.notify_one();\n'
      '    }\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (00.90): a task the next free worker takes before every one queued (scripts/build-3ds-core.sh).\n'
      '    void QueueWorkFront(Task work) {\n'
      '        {\n'
      '            std::unique_lock lock{queue_mutex};\n'
      '            requests.emplace_front(std::move(work));\n'
      '            ++work_scheduled;\n'
      '        }\n'
      '        condition.notify_one();\n'
      '    }\n'
      '#endif\n')
patch('src/common/thread_worker.h',
      '                        task = std::move(requests.front());\n'
      '                        requests.pop();\n',
      '                        task = std::move(requests.front());\n'
      '#ifdef EMUSUNI_PORT\n'
      '                        requests.pop_front();\n'
      '#else\n'
      '                        requests.pop();\n'
      '#endif\n')
patch('src/common/thread_worker.h',
      '    std::queue<Task> requests;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    std::deque<Task> requests; // (EmuSuNi: QueueWorkFront)\n'
      '#else\n'
      '    std::queue<Task> requests;\n'
      '#endif\n')
patch('src/common/thread_worker.h',
      '#include <vector>\n#include <queue>\n',
      '#include <vector>\n#include <queue>\n#ifdef EMUSUNI_PORT\n#include <deque>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    bool Build(bool fail_on_compile_required = false);\n',
      '    bool Build(bool fail_on_compile_required = false);\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (00.90): built here if no thread has started it yet, else waited for (the Vulkan worker\'s).\n'
      '    void EmuSuNiBuildOrWait();\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    std::atomic<int> emusuni_claimed{0}; // EmuSuNi: 1 once a thread has started building it\n',
      '    std::atomic<int> emusuni_claimed{0}; // EmuSuNi: 1 once a thread has started building it\n'
      '    bool emusuni_fronted = false;        // (00.90) queued at the workers\' front (the emulation thread\'s)\n'
      '    void EmuSuNiQueueFront();\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      'bool GraphicsPipeline::TryBuild(bool wait_built) {\n',
      '#ifdef EMUSUNI_PORT\n'
      'void GraphicsPipeline::EmuSuNiQueueFront() {\n'
      '    // (00.90) Ahead of every queued pipeline; whichever thread starts it first builds it (the claim).\n'
      '    emusuni_fronted = true;\n'
      '    worker->QueueWorkFront([this] {\n'
      '        int none = 0;\n'
      '        if (emusuni_claimed.compare_exchange_strong(none, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '    });\n'
      '}\n'
      '\n'
      'void GraphicsPipeline::EmuSuNiBuildOrWait() {\n'
      '    int none = 0;\n'
      '    if (emusuni_claimed.compare_exchange_strong(none, 1)) {\n'
      '        Build();\n'
      '    } else {\n'
      '        WaitDone();\n'
      '    }\n'
      '}\n'
      '#endif\n'
      '\n'
      'bool GraphicsPipeline::TryBuild(bool wait_built) {\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '        // EmuSuNi: still in the workers\' queue (no worker has started it): built now, here, rather than after\n'
      '        // everything queued before it.\n'
      '        int expected = 0;\n'
      '        if (wait_built && emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '#endif\n',
      '        // EmuSuNi: still in the workers\' queue (no worker has started it) and a draw wants it again: put at the\n'
      '        // queue\'s front once, ahead of what earlier scenes queued (00.90; a draw that waits for it: the Vulkan\n'
      '        // worker builds it itself if it gets there first; 00.65-00.89 built those here, one after another).\n'
      '        if (!emusuni_fronted && emusuni_claimed.load() == 0) {\n'
      '            EmuSuNiQueueFront();\n'
      '        }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    // Fallback to (a)synchronous compilation\n'
      '#ifdef EMUSUNI_PORT\n'
      '    worker->QueueWork([this] {\n'
      '        int expected = 0;\n'
      '        if (emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '            Build();\n'
      '        }\n'
      '    });\n',
      '    // Fallback to (a)synchronous compilation\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (wait_built) {\n'
      '        // (00.90) a draw waits for it: ahead of the queued ones (and the Vulkan worker builds it if it gets there\n'
      '        // first).\n'
      '        EmuSuNiQueueFront();\n'
      '    } else {\n'
      '        worker->QueueWork([this] {\n'
      '            int expected = 0;\n'
      '            if (emusuni_claimed.compare_exchange_strong(expected, 1)) {\n'
      '                Build();\n'
      '            }\n'
      '        });\n'
      '    }\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '        if (pipeline_dirty) {\n'
      '            if (!pipeline->IsDone()) {\n'
      '                pipeline->WaitDone();\n'
      '            }\n',
      '        if (pipeline_dirty) {\n'
      '            if (!pipeline->IsDone()) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '                pipeline->EmuSuNiBuildOrWait(); // (00.90: built here if no worker has started it)\n'
      '#else\n'
      '                pipeline->WaitDone();\n'
      '#endif\n'
      '            }\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'static u32 EmuSuNiWorkerThreads() {\n    return 3;\n}\n',
      'static u32 EmuSuNiWorkerThreads() {\n    return 4; // (00.90; 3 until then)\n}\n')
patch('src/common/emusuni_fastmem.cpp',
      'void MarkerMake(const char* name) {\n'
      '    char path[200];\n'
      '    if (MarkerPath(name, path, sizeof(path))) {\n'
      '        if (std::FILE* file = std::fopen(path, "wb")) {\n'
      '            std::fclose(file);\n'
      '        }\n'
      '    }\n'
      '}\n',
      '// (00.90) What the markers hold: the build of the core that wrote them.\n'
      'constexpr const char kMarkerStamp[] = "EmuSuNi 3DS core " __DATE__ " " __TIME__;\n'
      'void MarkerMake(const char* name) {\n'
      '    char path[200];\n'
      '    if (MarkerPath(name, path, sizeof(path))) {\n'
      '        if (std::FILE* file = std::fopen(path, "wb")) {\n'
      '            std::fputs(kMarkerStamp, file);\n'
      '            std::fclose(file);\n'
      '        }\n'
      '    }\n'
      '}\n'
      'bool MarkerFromThisCore(const char* name) {\n'
      '    char path[200];\n'
      '    if (!MarkerPath(name, path, sizeof(path))) {\n'
      '        return false;\n'
      '    }\n'
      '    std::FILE* file = std::fopen(path, "rb");\n'
      '    if (file == nullptr) {\n'
      '        return false;\n'
      '    }\n'
      '    char text[sizeof(kMarkerStamp) + 8] = "";\n'
      '    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);\n'
      '    std::fclose(file);\n'
      '    text[length] = \'\\0\';\n'
      '    return std::strcmp(text, kMarkerStamp) == 0;\n'
      '}\n')
patch('src/common/emusuni_fastmem.cpp',
      '    if (g_tested < 0) {\n'
      '        char path[200] = "";\n'
      '        if (MarkerExists("3ds-fastmem-crashed")) {\n',
      '    if (g_tested < 0) {\n'
      '        char path[200] = "";\n'
      '        // (00.90) A marker another build of the core left (an older EmuSuNi\'s, or an empty one from before 00.90):\n'
      '        // removed, fast memory tried again with this one.\n'
      '        for (const char* name : {"3ds-fastmem-crashed", "3ds-fastmem-trying"}) {\n'
      '            if (MarkerExists(name) && !MarkerFromThisCore(name)) {\n'
      '                MarkerRemove(name);\n'
      '                LOG_INFO(Common, "EmuSuNi: fastmem: {} came from another version of the core: trying it again", name);\n'
      '            }\n'
      '        }\n'
      '        if (MarkerExists("3ds-fastmem-crashed")) {\n')
patch('src/common/emusuni_fastmem.cpp',
      '    if ((g_frames >= 120 && faults >= 64) || g_frames >= 1800) {\n',
      '    if ((g_frames >= 120 && faults >= 64) || g_frames >= 600) {\n')

# --- 00.94: pipelines by parts (VK_EXT_graphics_pipeline_library). The 03-11 report (RADV's compiler for the PS4's GPU,
# GFX7 Bonaire, on its null device on this PC): Kirby: Planet Robobot took 8.6 s to compile the 774 pipelines of its
# first 100 s (the PS4: 2.5-3 times that), because every pipeline compiled its vertex shader again whole (18 vertex
# shaders of ~900 instructions in 425 compiles). By parts, as DXVK 2.0: each shader's part (a library: the vertex shader
# with the pre-rasterisation state, the fragment shader with its own) made once per render pass, the vertex input's and
# the colour output's (state only, no shader) once per state, and a pipeline linked from its four parts (under 0.1 ms
# on this PC): Kirby 0.67 s, 763 of the 774 pipelines ready in under 5 ms.
#  - A pipeline whose shaders' parts are made is linked at once on the emulation thread: the draw goes on (before: queued
#    and the draw skipped until a worker had compiled it). Only the fragment shader's part (or a state part) missing:
#    made at the workers' front and the draw waits for it (a few ms; the Vulkan worker makes it if no worker has started
#    it). A new vertex shader's part: as before (queued, the draw skipped until it is made; 3ds-sync-shaders: waited).
#  - Each linked pipeline is made again whole (optimised, as before) by one more worker ("Pipeline optimizer") after
#    everything else; the draws bind it from then on, and the driver's cache keeps it for the game's next start (where
#    the driver's cache is asked first, as before). What a game's end or a change of title leaves queued is dropped.
#  - Only with the device's VK_EXT_graphics_pipeline_library, VK_KHR_pipeline_library, the graphicsPipelineLibrary
#    feature, fast linking and extended dynamic state (the parts leave the cull mode, depth and stencil to the draws),
#    which src/Display.cpp asks for. Without them, or with the file /data/EmuSuNi/3ds-gpl-off (EMUSUNI_3DS_GPL=0 on the
#    PC), whole pipelines as before. A part or a link the driver refuses: whole pipelines from then on. A start that
#    ends while making them (its first 600 frames: the PS4's RADV had never made them) leaves /data/EmuSuNi/3ds-gpl-
#    trying; the next start makes 3ds-gpl-crashed of it, and whole pipelines until that file is deleted (or the core is
#    another build). The log says which and why at each renderer's start, and every 60 s what the parts did.
patch('src/video_core/renderer_vulkan/vk_instance.h',
      '    /// Returns true when VK_EXT_pipeline_creation_cache_control is supported\n'
      '    bool IsPipelineCreationCacheControlSupported() const {\n'
      '        return pipeline_creation_cache_control;\n'
      '    }\n',
      '    /// Returns true when VK_EXT_pipeline_creation_cache_control is supported\n'
      '    bool IsPipelineCreationCacheControlSupported() const {\n'
      '        return pipeline_creation_cache_control;\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (00.94): whether pipelines can be made by parts (VK_EXT_graphics_pipeline_library); if not, why.\n'
      '    bool EmuSuNiCanLinkPipelines(std::string& why) const;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_instance.cpp',
      'void Instance::CreateAllocator() {\n',
      r'''#ifdef EMUSUNI_PORT
bool Instance::EmuSuNiCanLinkPipelines(std::string& why) const {
    // EmuSuNi (00.94): of what the device was made with (the frontend's lists, src/Display.cpp).
    const auto has = [this](std::string_view name) {
        return std::find(available_extensions.begin(), available_extensions.end(), name) !=
               available_extensions.end();
    };
    if (!has(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) || !has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        why = "the device has no VK_EXT_graphics_pipeline_library";
        return false;
    }
#ifdef HAVE_LIBRETRO
    if (!EmuSuNiFeatureListed("graphicsPipelineLibrary")) {
        why = "the device was made without its graphicsPipelineLibrary feature";
        return false;
    }
#endif
    if (!extended_dynamic_state) {
        why = "no extended dynamic state (the parts leave the cull mode, depth and stencil to the draws)";
        return false;
    }
    const auto chain = physical_device.getProperties2<vk::PhysicalDeviceProperties2,
                                                      vk::PhysicalDeviceGraphicsPipelineLibraryPropertiesEXT>();
    if (!chain.get<vk::PhysicalDeviceGraphicsPipelineLibraryPropertiesEXT>().graphicsPipelineLibraryFastLinking) {
        why = "the driver doesn't link the parts fast (no graphicsPipelineLibraryFastLinking)";
        return false;
    }
    return true;
}
#endif

void Instance::CreateAllocator() {
''')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '#include "common/thread_worker.h"\n',
      '#include "common/thread_worker.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <functional>\n#include <string>\n#include <unordered_map>\n#include <utility>\n#include <vector>\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    vk::ShaderModule module;\n    vk::Device device;\n    std::string program;\n};\n',
      '    vk::ShaderModule module;\n    vk::Device device;\n    std::string program;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (00.94): this shader\'s part of the pipelines (a library of its stage) for a render pass, made\n'
      '    /// once by `make` (whoever asks first; the others wait for it). Null if the driver refused it.\n'
      '    vk::Pipeline EmuSuNiPart(vk::RenderPass renderpass, const std::function<vk::Pipeline()>& make);\n'
      '    /// The part if it is made already, never waiting (false: not made yet, or being made).\n'
      '    bool EmuSuNiPartMade(vk::RenderPass renderpass, vk::Pipeline& part);\n'
      '    std::mutex emusuni_parts_mutex;\n'
      '    std::vector<std::pair<vk::RenderPass, vk::Pipeline>> emusuni_parts;\n'
      '#endif\n'
      '};\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      'class GraphicsPipeline : public Common::AsyncHandle {\npublic:\n',
      r'''#ifdef EMUSUNI_PORT
/// EmuSuNi (00.94): pipelines by parts (VK_EXT_graphics_pipeline_library, scripts/build-3ds-core.sh), the pipeline
/// cache's: whether they are made, the parts that have no shader (the vertex input's and the colour output's, by their
/// state), the worker that makes the linked pipelines whole later, and what all that did (for the log).
struct EmuSuNiParts {
    std::atomic<bool> on{false};
    std::atomic<bool> closing{false}; // the whole pipelines still queued are dropped (the cache goes away)
    Common::ThreadWorker* optimizer{};
    vk::Device device;

    std::mutex mutex; // the two maps
    std::unordered_map<std::string, vk::Pipeline> vertex_input;
    std::unordered_map<std::string, vk::Pipeline> color_output;

    std::atomic<u32> linked_here{0}, linked_by_workers{0}, vertex_parts{0}, fragment_parts{0}, whole_later{0};
    std::atomic<u64> link_us{0}, vertex_us{0}, vertex_max_us{0}, fragment_us{0}, fragment_max_us{0},
        whole_later_us{0};

    ~EmuSuNiParts();
    /// The driver refused a part or a link: whole pipelines from now on (the log says so once).
    void Refused(const char* what, vk::Result result);
    static u64 NowUs();
    static void Max(std::atomic<u64>& max, u64 value);
};
#endif

class GraphicsPipeline : public Common::AsyncHandle {
public:
''')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '                              vk::PipelineLayout layout, std::array<Shader*, 3> stages,\n'
      '                              Common::ThreadWorker* worker);\n',
      '                              vk::PipelineLayout layout, std::array<Shader*, 3> stages,\n'
      '                              Common::ThreadWorker* worker\n'
      '#ifdef EMUSUNI_PORT\n'
      '                              , EmuSuNiParts* emusuni_parts = nullptr // (00.94) by parts when they are on\n'
      '#endif\n'
      '    );\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    [[nodiscard]] vk::Pipeline Handle() const noexcept {\n        return *pipeline;\n    }\n',
      '    [[nodiscard]] vk::Pipeline Handle() const noexcept {\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (00.94): the one the draws bind (the linked one until it is made whole).\n'
      '        return vk::Pipeline{emusuni_handle.load(std::memory_order_acquire)};\n'
      '#else\n'
      '        return *pipeline;\n'
      '#endif\n'
      '    }\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h',
      '    bool emusuni_fronted = false;        // (00.90) queued at the workers\' front (the emulation thread\'s)\n'
      '    void EmuSuNiQueueFront();\n'
      '#endif\n',
      '    bool emusuni_fronted = false;        // (00.90) queued at the workers\' front (the emulation thread\'s)\n'
      '    void EmuSuNiQueueFront();\n'
      '    // (00.94) Pipelines by parts.\n'
      '    enum class EmuSuNiHow {\n'
      '        Whole,       // one pipeline, as upstream\n'
      '        CachedOnly,  // whole, only if the driver\'s cache has it\n'
      '        Parts,       // linked from its parts, made here when missing (a worker)\n'
      '        PartsIfMade, // linked only if both shaders\' parts are made (the emulation thread: never waits)\n'
      '        Optimized,   // whole, after it was linked (the optimizer): the draws bind it from then on\n'
      '    };\n'
      '    bool EmuSuNiCreate(EmuSuNiHow how);\n'
      '    bool EmuSuNiLink(const vk::GraphicsPipelineCreateInfo& whole, bool may_make);\n'
      '    void EmuSuNiWholeLater(const vk::GraphicsPipelineCreateInfo& whole);\n'
      '    bool EmuSuNiUsesParts() const;\n'
      '    int EmuSuNiShaderPartsMade(); // 1: the vertex shader\'s part is made, 2: the fragment shader\'s\n'
      '    bool emusuni_draws_wait = false;                      // only a cheap part was missing: every draw waits\n'
      '    EmuSuNiParts* emusuni_parts{};                        // the pipeline cache\'s (null: never by parts)\n'
      '    std::atomic<VkPipeline> emusuni_handle{VK_NULL_HANDLE}; // what Handle gives\n'
      '    vk::Pipeline emusuni_linked;                          // made of the parts (kept: draws were recorded with it)\n'
      '    vk::Pipeline emusuni_whole;                           // made whole later\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include <chrono>\n#include "common/emusuni_stats.h"\n'
      '#include "common/logging/log.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      'Shader::~Shader() {\n    if (device && module) {\n        device.destroyShaderModule(module);\n    }\n}\n',
      'Shader::~Shader() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    for (const auto& [renderpass, part] : emusuni_parts) {\n'
      '        if (part) {\n'
      '            device.destroyPipeline(part); // EmuSuNi (00.94): its parts\n'
      '        }\n'
      '    }\n'
      '#endif\n'
      '    if (device && module) {\n        device.destroyShaderModule(module);\n    }\n}\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '                                   Common::ThreadWorker* worker_)\n'
      '    : instance{instance_}, renderpass_cache{renderpass_cache_}, worker{worker_},\n'
      '      pipeline_layout{layout_}, pipeline_cache{pipeline_cache_}, info{info_}, stages{stages_} {}\n\n'
      'GraphicsPipeline::~GraphicsPipeline() = default;\n',
      '                                   Common::ThreadWorker* worker_\n'
      '#ifdef EMUSUNI_PORT\n'
      '                                   , EmuSuNiParts* emusuni_parts_\n'
      '#endif\n'
      '                                   )\n'
      '    : instance{instance_}, renderpass_cache{renderpass_cache_}, worker{worker_},\n'
      '      pipeline_layout{layout_}, pipeline_cache{pipeline_cache_}, info{info_}, stages{stages_}\n'
      '#ifdef EMUSUNI_PORT\n'
      '      , emusuni_parts{emusuni_parts_}\n'
      '#endif\n'
      '{\n}\n\n'
      '#ifdef EMUSUNI_PORT\n'
      'GraphicsPipeline::~GraphicsPipeline() {\n'
      '    // EmuSuNi (00.94): the linked one and the one made whole later (`pipeline` holds neither).\n'
      '    for (const vk::Pipeline made : {emusuni_linked, emusuni_whole}) {\n'
      '        if (made) {\n'
      '            instance.GetDevice().destroyPipeline(made);\n'
      '        }\n'
      '    }\n'
      '}\n'
      '#else\n'
      'GraphicsPipeline::~GraphicsPipeline() = default;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '        return wait_built;\n    }\n\n    // If the shaders haven\'t been compiled yet, we cannot proceed.\n',
      '#ifdef EMUSUNI_PORT\n'
      '        return wait_built || emusuni_draws_wait; // (00.94: only a cheap part missing: every draw waits)\n'
      '#else\n'
      '        return wait_built;\n'
      '#endif\n'
      '    }\n\n    // If the shaders haven\'t been compiled yet, we cannot proceed.\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    // Fallback to (a)synchronous compilation\n#ifdef EMUSUNI_PORT\n    if (wait_built) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.94): by parts. Both shaders\' parts made: linked here at once (the draw goes on). Only the\n'
      '    // fragment shader\'s part (or a state part) missing: made at the workers\' front, and the draws wait for it (a\n'
      '    // few ms; the Vulkan worker makes it if no worker has started it). The vertex shader\'s part missing: as before.\n'
      '    if (!shaders_pending && EmuSuNiUsesParts()) {\n'
      '        const int made = EmuSuNiShaderPartsMade();\n'
      '        if (made == 3 && EmuSuNiCreate(EmuSuNiHow::PartsIfMade)) {\n'
      '            return true;\n'
      '        }\n'
      '        if ((made & 1) != 0 && EmuSuNiUsesParts()) {\n'
      '            EmuSuNiQueueFront();\n'
      '            is_pending = true;\n'
      '            emusuni_draws_wait = true;\n'
      '            return true;\n'
      '        }\n'
      '    }\n'
      '#endif\n\n'
      '    // Fallback to (a)synchronous compilation\n#ifdef EMUSUNI_PORT\n    if (wait_built) {\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n    MICROPROFILE_SCOPE(Vulkan_Pipeline);\n',
      '#ifdef EMUSUNI_PORT\n'
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n'
      '    if (fail_on_compile_required) {\n'
      '        return EmuSuNiCreate(EmuSuNiHow::CachedOnly);\n'
      '    }\n'
      '    return EmuSuNiCreate(EmuSuNiUsesParts() ? EmuSuNiHow::Parts : EmuSuNiHow::Whole);\n'
      '}\n\n'
      '// EmuSuNi (00.94): upstream\'s Build, the pipeline\'s state made the same way whichever way it is made.\n'
      'bool GraphicsPipeline::EmuSuNiCreate(EmuSuNiHow how) {\n'
      '    const bool fail_on_compile_required = how == EmuSuNiHow::CachedOnly;\n'
      '#else\n'
      'bool GraphicsPipeline::Build(bool fail_on_compile_required) {\n'
      '#endif\n'
      '    MICROPROFILE_SCOPE(Vulkan_Pipeline);\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    if (!fail_on_compile_required) {\n        static const int emusuni_test_ms',
      '    if (how == EmuSuNiHow::Whole || how == EmuSuNiHow::Optimized) { // (00.94: not the parts)\n'
      '        static const int emusuni_test_ms')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    if (fail_on_compile_required) {\n'
      '        pipeline_info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT;\n'
      '    }\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.94): by parts, or whole after the parts.\n'
      '    if (how == EmuSuNiHow::PartsIfMade) {\n'
      '        return EmuSuNiLink(pipeline_info, false);\n'
      '    }\n'
      '    if (how == EmuSuNiHow::Parts && EmuSuNiLink(pipeline_info, true)) {\n'
      '        return true;\n'
      '    } // (a part refused: whole, as before)\n'
      '    if (how == EmuSuNiHow::Optimized) {\n'
      '        EmuSuNiWholeLater(pipeline_info);\n'
      '        return true;\n'
      '    }\n'
      '#endif\n'
      '    if (fail_on_compile_required) {\n'
      '        pipeline_info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT;\n'
      '    }\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '    if (result.result == vk::Result::eSuccess) {\n        pipeline = std::move(result.value);\n',
      '    if (result.result == vk::Result::eSuccess) {\n        pipeline = std::move(result.value);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        emusuni_handle.store(static_cast<VkPipeline>(*pipeline), std::memory_order_release);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp',
      '\n} // namespace Vulkan\n',
      r'''
#ifdef EMUSUNI_PORT
// EmuSuNi (00.94): pipelines by parts (scripts/build-3ds-core.sh).
u64 EmuSuNiParts::NowUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void EmuSuNiParts::Max(std::atomic<u64>& max, u64 value) {
    u64 seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

void EmuSuNiParts::Refused(const char* what, vk::Result result) {
    if (on.exchange(false)) {
        LOG_ERROR(Render_Vulkan,
                  "EmuSuNi: pipelines by parts: the driver refused {} ({}): OFF from now on, whole pipelines "
                  "as before",
                  what, vk::to_string(result));
    }
}

EmuSuNiParts::~EmuSuNiParts() {
    for (const auto* map : {&vertex_input, &color_output}) {
        for (const auto& [key, part] : *map) {
            if (part) {
                device.destroyPipeline(part);
            }
        }
    }
}

vk::Pipeline Shader::EmuSuNiPart(vk::RenderPass renderpass, const std::function<vk::Pipeline()>& make) {
    std::scoped_lock lock{emusuni_parts_mutex};
    for (const auto& [pass, part] : emusuni_parts) {
        if (pass == renderpass) {
            return part;
        }
    }
    const vk::Pipeline part = make();
    emusuni_parts.emplace_back(renderpass, part);
    return part;
}

bool Shader::EmuSuNiPartMade(vk::RenderPass renderpass, vk::Pipeline& part) {
    std::unique_lock lock{emusuni_parts_mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return false; // (being made)
    }
    for (const auto& [pass, made] : emusuni_parts) {
        if (pass == renderpass) {
            part = made;
            return true;
        }
    }
    return false;
}

namespace {
// The part of the pipeline `whole` that `which` names, as a library (null if the driver refuses it). Every part takes
// the whole's dynamic states: each uses those of its own state.
vk::Pipeline EmuSuNiMakePart(EmuSuNiParts& parts, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& whole,
                             vk::GraphicsPipelineLibraryFlagBitsEXT which,
                             const vk::PipelineShaderStageCreateInfo* stage, const char* what) {
    const vk::GraphicsPipelineLibraryCreateInfoEXT library_info = {.flags = which};
    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &library_info;
    info.flags = vk::PipelineCreateFlagBits::eLibraryKHR;
    info.pDynamicState = whole.pDynamicState;
    info.basePipelineIndex = -1;
    switch (which) {
    case vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface:
        info.pVertexInputState = whole.pVertexInputState;
        info.pInputAssemblyState = whole.pInputAssemblyState;
        break;
    case vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders:
        info.stageCount = 1;
        info.pStages = stage;
        info.pViewportState = whole.pViewportState;
        info.pRasterizationState = whole.pRasterizationState;
        info.layout = whole.layout;
        info.renderPass = whole.renderPass;
        break;
    case vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader:
        info.stageCount = 1;
        info.pStages = stage;
        info.pDepthStencilState = whole.pDepthStencilState;
        info.pMultisampleState = whole.pMultisampleState;
        info.layout = whole.layout;
        info.renderPass = whole.renderPass;
        break;
    case vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface:
        info.pColorBlendState = whole.pColorBlendState;
        info.pMultisampleState = whole.pMultisampleState;
        info.renderPass = whole.renderPass;
        break;
    }
    vk::Pipeline made{};
    const vk::Result result = parts.device.createGraphicsPipelines(cache, 1, &info, nullptr, &made);
    if (result != vk::Result::eSuccess) {
        parts.Refused(what, result);
        return {};
    }
    return made;
}

void EmuSuNiPut(std::string& key, const void* data, std::size_t size) {
    key.append(static_cast<const char*>(data), size);
}
} // namespace

bool GraphicsPipeline::EmuSuNiUsesParts() const {
    return emusuni_parts != nullptr && emusuni_parts->on.load(std::memory_order_relaxed) && stages[0] != nullptr &&
           stages[1] != nullptr && stages[2] == nullptr;
}

int GraphicsPipeline::EmuSuNiShaderPartsMade() {
    const vk::RenderPass renderpass =
        renderpass_cache.GetRenderpass(info.state.attachments.color, info.state.attachments.depth, false);
    vk::Pipeline vertex{};
    vk::Pipeline fragment{};
    return (stages[0]->EmuSuNiPartMade(renderpass, vertex) && vertex ? 1 : 0) |
           (stages[1]->EmuSuNiPartMade(renderpass, fragment) && fragment ? 2 : 0);
}

bool GraphicsPipeline::EmuSuNiLink(const vk::GraphicsPipelineCreateInfo& whole, bool may_make) {
    EmuSuNiParts& parts = *emusuni_parts;
    const vk::PipelineShaderStageCreateInfo* vertex_stage = nullptr;
    const vk::PipelineShaderStageCreateInfo* fragment_stage = nullptr;
    for (u32 i = 0; i < whole.stageCount; i++) {
        if (whole.pStages[i].stage == vk::ShaderStageFlagBits::eVertex) {
            vertex_stage = &whole.pStages[i];
        } else if (whole.pStages[i].stage == vk::ShaderStageFlagBits::eFragment) {
            fragment_stage = &whole.pStages[i];
        }
    }
    if (vertex_stage == nullptr || fragment_stage == nullptr || whole.stageCount != 2) {
        return false;
    }

    // The shaders' parts, one per shader and render pass.
    const vk::RenderPass renderpass = whole.renderPass;
    vk::Pipeline vertex{};
    vk::Pipeline fragment{};
    if (may_make) {
        vertex = stages[0]->EmuSuNiPart(renderpass, [&] {
            const u64 start = EmuSuNiParts::NowUs();
            const vk::Pipeline part = EmuSuNiMakePart(parts, pipeline_cache, whole,
                                                      vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders,
                                                      vertex_stage, "a vertex shader's part");
            const u64 us = EmuSuNiParts::NowUs() - start;
            parts.vertex_parts++;
            parts.vertex_us += us;
            EmuSuNiParts::Max(parts.vertex_max_us, us);
            return part;
        });
        fragment = stages[1]->EmuSuNiPart(renderpass, [&] {
            const u64 start = EmuSuNiParts::NowUs();
            const vk::Pipeline part = EmuSuNiMakePart(parts, pipeline_cache, whole,
                                                      vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader,
                                                      fragment_stage, "a fragment shader's part");
            const u64 us = EmuSuNiParts::NowUs() - start;
            parts.fragment_parts++;
            parts.fragment_us += us;
            EmuSuNiParts::Max(parts.fragment_max_us, us);
            return part;
        });
    } else if (!stages[0]->EmuSuNiPartMade(renderpass, vertex) || !stages[1]->EmuSuNiPartMade(renderpass, fragment)) {
        return false; // (the workers make it)
    }

    // The parts without shaders, by their state (cheap: made by whoever needs them first).
    std::string input_key;
    const vk::PipelineVertexInputStateCreateInfo& input = *whole.pVertexInputState;
    EmuSuNiPut(input_key, input.pVertexBindingDescriptions,
               input.vertexBindingDescriptionCount * sizeof(vk::VertexInputBindingDescription));
    input_key.push_back('|');
    EmuSuNiPut(input_key, input.pVertexAttributeDescriptions,
               input.vertexAttributeDescriptionCount * sizeof(vk::VertexInputAttributeDescription));
    EmuSuNiPut(input_key, &whole.pInputAssemblyState->topology, sizeof(vk::PrimitiveTopology));
    std::string output_key;
    const vk::PipelineColorBlendStateCreateInfo& output = *whole.pColorBlendState;
    EmuSuNiPut(output_key, output.pAttachments, sizeof(vk::PipelineColorBlendAttachmentState));
    EmuSuNiPut(output_key, &output.logicOpEnable, sizeof(output.logicOpEnable));
    EmuSuNiPut(output_key, &output.logicOp, sizeof(output.logicOp));
    const VkRenderPass pass = static_cast<VkRenderPass>(renderpass);
    EmuSuNiPut(output_key, &pass, sizeof(pass));
    const auto state_part = [&](std::unordered_map<std::string, vk::Pipeline>& map, const std::string& key,
                                vk::GraphicsPipelineLibraryFlagBitsEXT which, const char* what) {
        std::scoped_lock lock{parts.mutex};
        const auto [it, added] = map.try_emplace(key);
        if (added) {
            it->second = EmuSuNiMakePart(parts, pipeline_cache, whole, which, nullptr, what);
        }
        return it->second;
    };
    const vk::Pipeline vertex_input = state_part(
        parts.vertex_input, input_key, vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface,
        "the vertex input's part");
    const vk::Pipeline color_output = state_part(
        parts.color_output, output_key, vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface,
        "the colour output's part");
    if (!vertex || !fragment || !vertex_input || !color_output || !parts.on) {
        return false; // (a part refused: whole, as before)
    }

    // Linked (fast: no link-time optimisation).
    const std::array<vk::Pipeline, 4> libraries = {vertex_input, vertex, fragment, color_output};
    const vk::PipelineLibraryCreateInfoKHR link_info = {
        .libraryCount = static_cast<u32>(libraries.size()),
        .pLibraries = libraries.data(),
    };
    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &link_info;
    info.layout = whole.layout;
    info.basePipelineIndex = -1;
    const u64 start = EmuSuNiParts::NowUs();
    vk::Pipeline linked{};
    const vk::Result result = parts.device.createGraphicsPipelines(vk::PipelineCache{}, 1, &info, nullptr, &linked);
    if (result != vk::Result::eSuccess) {
        parts.Refused("a link of parts", result);
        return false;
    }
    parts.link_us += EmuSuNiParts::NowUs() - start;
    (may_make ? parts.linked_by_workers : parts.linked_here)++;
    emusuni_linked = linked;
    emusuni_handle.store(static_cast<VkPipeline>(linked), std::memory_order_release);
    MarkDone();

    // Made whole later, after everything else (the draws bind it from then on; the driver's cache keeps it).
    EmuSuNiParts* const parts_of_cache = &parts;
    if (parts.optimizer != nullptr) {
        parts.optimizer->QueueWork([this, parts_of_cache] {
            if (!parts_of_cache->closing.load()) {
                EmuSuNiCreate(EmuSuNiHow::Optimized);
            }
        });
    }
    return true;
}

void GraphicsPipeline::EmuSuNiWholeLater(const vk::GraphicsPipelineCreateInfo& whole) {
    EmuSuNiParts& parts = *emusuni_parts;
    const u64 start = EmuSuNiParts::NowUs();
    vk::Pipeline made{};
    if (parts.device.createGraphicsPipelines(pipeline_cache, 1, &whole, nullptr, &made) != vk::Result::eSuccess) {
        return; // (the linked one stays)
    }
    emusuni_whole = made;
    emusuni_handle.store(static_cast<VkPipeline>(made), std::memory_order_release);
    parts.whole_later++;
    parts.whole_later_us += EmuSuNiParts::NowUs() - start;
}
#endif

} // namespace Vulkan
''')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      '            *parent.pipeline_layout, parent.current_shaders, &parent.pipeline_workers);\n',
      '            *parent.pipeline_layout, parent.current_shaders, &parent.pipeline_workers\n'
      '#ifdef EMUSUNI_PORT\n'
      '            , &parent.emusuni_parts // (00.94) by parts\n'
      '#endif\n'
      '        );\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      '                *parent.pipeline_layout, shaders, &parent.pipeline_workers);\n',
      '                *parent.pipeline_layout, shaders, &parent.pipeline_workers\n'
      '#ifdef EMUSUNI_PORT\n'
      '                , &parent.emusuni_parts // (00.94) by parts\n'
      '#endif\n'
      '            );\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    u64 current_program_id{0};\n    std::vector<std::shared_ptr<ShaderDiskCache>> disk_caches;\n',
      '    u64 current_program_id{0};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.94): pipelines by parts, and the worker that makes them whole later (only when they are on).\n'
      '    EmuSuNiParts emusuni_parts;\n'
      '    std::unique_ptr<Common::ThreadWorker> emusuni_optimizer;\n'
      '    bool emusuni_parts_trying = false; // the marker 3ds-gpl-trying is ours (its first 600 frames)\n'
      '    u32 emusuni_parts_frames = 0;\n'
      '    u32 emusuni_parts_logged = 0;      // the links at the last log line\n'
      '#endif\n'
      '    std::vector<std::shared_ptr<ShaderDiskCache>> disk_caches;\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '    /// EmuSuNi: the shader cache object, made on first use when none was loaded\n'
      '    const std::shared_ptr<ShaderDiskCache>& EmuSuNiDiskCache();\n'
      '#endif\n',
      '    /// EmuSuNi: the shader cache object, made on first use when none was loaded\n'
      '    const std::shared_ptr<ShaderDiskCache>& EmuSuNiDiskCache();\n'
      '    /// EmuSuNi (00.94): whether pipelines are made by parts (the log says which and why), and what they did\n'
      '    void EmuSuNiPartsStart();\n'
      '    void EmuSuNiPartsLog();\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '#include <boost/container/static_vector.hpp>\n#ifdef EMUSUNI_PORT\n#include <chrono>\n#endif\n',
      '#include <boost/container/static_vector.hpp>\n#ifdef EMUSUNI_PORT\n#include <chrono>\n#include <cstdio>\n'
      '#include <cstdlib>\n#include <cstring>\n#include <unistd.h>\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'static u32 EmuSuNiWorkerThreads() {\n    return 4; // (00.90; 3 until then)\n}\n',
      'static u32 EmuSuNiWorkerThreads() {\n    return 4; // (00.90; 3 until then)\n}\n'
      r'''
// EmuSuNi (00.94): the marker files of pipelines by parts (as fast memory's, common/emusuni_fastmem.cpp): in
// /data/EmuSuNi on the PS4, in EMUSUNI_3DS_MARKER_DIR on the PC (none when unset); they hold the core's build.
constexpr const char kEmuSuNiPartsStamp[] = "EmuSuNi 3DS core " __DATE__ " " __TIME__;
static bool EmuSuNiMarkerPath(const char* name, char* path, std::size_t size) {
#if defined(__ORBIS__)
    std::snprintf(path, size, "/data/EmuSuNi/%s", name);
    return true;
#else
    const char* dir = std::getenv("EMUSUNI_3DS_MARKER_DIR");
    if (dir == nullptr) {
        return false;
    }
    std::snprintf(path, size, "%s/%s", dir, name);
    return true;
#endif
}
static bool EmuSuNiMarkerExists(const char* name) {
    char path[200];
    return EmuSuNiMarkerPath(name, path, sizeof(path)) && ::access(path, F_OK) == 0;
}
static void EmuSuNiMarkerMake(const char* name) {
    char path[200];
    if (EmuSuNiMarkerPath(name, path, sizeof(path))) {
        if (std::FILE* file = std::fopen(path, "wb")) {
            std::fputs(kEmuSuNiPartsStamp, file);
            std::fclose(file);
        }
    }
}
static void EmuSuNiMarkerRemove(const char* name) {
    char path[200];
    if (EmuSuNiMarkerPath(name, path, sizeof(path))) {
        std::remove(path);
    }
}
static bool EmuSuNiMarkerFromThisCore(const char* name) {
    char path[200];
    if (!EmuSuNiMarkerPath(name, path, sizeof(path))) {
        return false;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    char text[sizeof(kEmuSuNiPartsStamp) + 8] = "";
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
    return std::strcmp(text, kEmuSuNiPartsStamp) == 0;
}
''')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    BuildLayout();\n}\n\nvoid PipelineCache::BuildLayout() {\n',
      '    BuildLayout();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNiPartsStart(); // (00.94)\n'
      '#endif\n'
      '}\n\nvoid PipelineCache::BuildLayout() {\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'void PipelineCache::EmuSuNiSaveNowAndThen() {\n',
      r'''void PipelineCache::EmuSuNiPartsStart() {
    // (00.94) Pipelines by parts, unless the switch, the device or a start that ended while making them says no.
    std::string why;
    bool on = false;
    const char* env = std::getenv("EMUSUNI_3DS_GPL");
    if ((env != nullptr && std::strcmp(env, "0") == 0) || ::access("/data/EmuSuNi/3ds-gpl-off", F_OK) == 0) {
        why = "the file /data/EmuSuNi/3ds-gpl-off";
    } else if (instance.EmuSuNiCanLinkPipelines(why)) {
        for (const char* name : {"3ds-gpl-crashed", "3ds-gpl-trying"}) {
            if (EmuSuNiMarkerExists(name) && !EmuSuNiMarkerFromThisCore(name)) {
                EmuSuNiMarkerRemove(name);
                LOG_INFO(Render_Vulkan,
                         "EmuSuNi: pipelines by parts: {} came from another version of the core: trying them again",
                         name);
            }
        }
        if (EmuSuNiMarkerExists("3ds-gpl-trying")) {
            // The last start ended (crashed, hung) while it made them: not again with this core.
            EmuSuNiMarkerMake("3ds-gpl-crashed");
            EmuSuNiMarkerRemove("3ds-gpl-trying");
        }
        if (EmuSuNiMarkerExists("3ds-gpl-crashed")) {
            why = "a start of this core ended while making them (delete /data/EmuSuNi/3ds-gpl-crashed to try them "
                  "again)";
        } else {
            on = true;
        }
    }
    if (!on) {
        LOG_INFO(Render_Vulkan,
                 "EmuSuNi: pipelines by parts (VK_EXT_graphics_pipeline_library): OFF, {}: whole pipelines as before",
                 why);
        return;
    }
    emusuni_parts.device = instance.GetDevice();
    emusuni_optimizer = std::make_unique<Common::ThreadWorker>(1, "Pipeline optimizer");
    emusuni_parts.optimizer = emusuni_optimizer.get();
    emusuni_parts.on = true;
    EmuSuNiMarkerMake("3ds-gpl-trying");
    emusuni_parts_trying = true;
    LOG_INFO(Render_Vulkan, "EmuSuNi: pipelines by parts (VK_EXT_graphics_pipeline_library): ON: each shader compiled "
                            "once, a pipeline linked from its parts, made whole later (the file "
                            "/data/EmuSuNi/3ds-gpl-off: whole pipelines as before)");
}

void PipelineCache::EmuSuNiPartsLog() {
    EmuSuNiParts& parts = emusuni_parts;
    const u32 here = parts.linked_here.load();
    const u32 by_workers = parts.linked_by_workers.load();
    const u32 linked = here + by_workers;
    if (linked == emusuni_parts_logged) {
        return; // (nothing new; never on: nothing at all)
    }
    emusuni_parts_logged = linked;
    LOG_INFO(Render_Vulkan,
             "EmuSuNi: pipelines by parts: {} linked ({} at once on the emulation thread, {} by the workers; {} us "
             "each), {} vertex shader parts ({} ms, longest {} ms), {} fragment shader parts ({} ms, longest {} ms), "
             "{} made whole later ({} ms){}",
             linked, here, by_workers, parts.link_us.load() / std::max<u32>(linked, 1), parts.vertex_parts.load(),
             parts.vertex_us.load() / 1000, parts.vertex_max_us.load() / 1000, parts.fragment_parts.load(),
             parts.fragment_us.load() / 1000, parts.fragment_max_us.load() / 1000, parts.whole_later.load(),
             parts.whole_later_us.load() / 1000, parts.on.load() ? "" : " (OFF now: the driver refused one)");
}

void PipelineCache::EmuSuNiSaveNowAndThen() {
    // (00.94) The marker of pipelines by parts goes after their first 600 frames.
    if (emusuni_parts_trying && ++emusuni_parts_frames >= 600) {
        emusuni_parts_trying = false;
        EmuSuNiMarkerRemove("3ds-gpl-trying");
    }
''')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    emusuni_last_save_ms = now;\n'
      '    // Only when the driver compiled something since the last save (its data grew).\n',
      '    emusuni_last_save_ms = now;\n'
      '    EmuSuNiPartsLog(); // (00.94) every 60 s\n'
      '    // Only when the driver compiled something since the last save (its data grew).\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      'PipelineCache::~PipelineCache() {\n'
      '    pipeline_workers.WaitForRequests();\n'
      '    shader_workers.WaitForRequests();\n'
      '    SaveDriverPipelineDiskCache();\n'
      '}\n',
      'PipelineCache::~PipelineCache() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_parts.closing = true; // (00.94) the whole pipelines still queued: dropped\n'
      '#endif\n'
      '    pipeline_workers.WaitForRequests();\n'
      '    shader_workers.WaitForRequests();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_optimizer) {\n'
      '        emusuni_optimizer->WaitForRequests();\n'
      '    }\n'
      '#endif\n'
      '    SaveDriverPipelineDiskCache();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNiPartsLog();\n'
      '    if (emusuni_parts_trying) {\n'
      '        EmuSuNiMarkerRemove("3ds-gpl-trying"); // (the game ended)\n'
      '    }\n'
      '#endif\n'
      '}\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    // Check if the current cache is for the specified TID.\n'
      '    if (curr_disk_cache && curr_disk_cache->GetProgramID() == title_id) {\n'
      '        return;\n'
      '    }\n',
      '    // Check if the current cache is for the specified TID.\n'
      '    if (curr_disk_cache && curr_disk_cache->GetProgramID() == title_id) {\n'
      '        return;\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.94): the whole pipelines queued for these caches dropped first (one may be erased below).\n'
      '    if (emusuni_optimizer) {\n'
      '        emusuni_parts.closing = true;\n'
      '        emusuni_optimizer->WaitForRequests();\n'
      '        emusuni_parts.closing = false;\n'
      '    }\n'
      '#endif\n')

# --- 00.95: vertex attributes at aligned offsets. The owner's 00.94 test (04-11): Super Smash Bros.' fighters drawn
# deformed and spiky on the PS4, never on this PC (lavapipe; with and without parts, with and without the accurate
# multiplication, and a NaN position turned into a spike drew none). Its fighters' vertices are packed: positions as 3
# signed bytes (R8G8B8_SSCALED) in strides of 3, 5, 7 or 9 bytes, 2- and 4-byte attributes at odd offsets. RADV loads
# a 3-byte attribute with an 8_8 load and an 8 one, a 4-byte one with one dword-sized load, and lets GFX7 do them at
# any address (radeonsi too: only GFX6 and GFX10+ get split loads), because Linux's amdgpu sets the shaders' memory
# alignment mode to "unaligned" on GFX7-9; on the PS4 Sony's system sets up the GPU, and a load at an odd address
# there reads the wrong bytes: every other vertex of those meshes off. (Loads of 4 bytes at even addresses that 4
# doesn't divide are not it: Kirby: Planet Robobot draws right on the PS4 with nearly all its vertices so, strides of
# 14 bytes.) So a loader with a load of 2 or more bytes at an odd offset or in an odd stride is copied into the stream
# buffer with each attribute at an offset its loads' alignment divides (2 bytes for a 2- or 3-component 8-bit
# attribute and a single 16-bit one, 4 for the rest but a single byte) and a stride a multiple of 4: the same values,
# the GPU reads what the 3DS would. On this PC: ~7000 vertices a frame in Smash (39% of its vertices), ~2300 in
# Pokemon X (43%), ~1500 in Kirby (9%), none in DKC Returns, Pokemon Ultra Sun or Driver Renegade; the others are
# copied as before. The file /data/EmuSuNi/3ds-vertex-as-is (EMUSUNI_3DS_VERTEX_ALIGN=0 on the PC) keeps the game's
# layout; the log says which, and every 600 frames how many vertices moved.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n\nnamespace Vulkan {\n',
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#include <optional>\n#include <unistd.h>\n'
      '#include "common/emusuni_stats.h"\n#endif\n\nnamespace Vulkan {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'using namespace Pica::Shader::Generator;\n\nconstexpr u64 STREAM_BUFFER_SIZE = 64_MiB;\n',
      r'''using namespace Pica::Shader::Generator;

#ifdef EMUSUNI_PORT
// EmuSuNi (00.95): the byte alignment a vertex attribute's loads need on the PS4's GPU (RADV, GFX7: a 3-component 8-
// or 16-bit attribute is loaded as a 2-component load and a 1-component one, the rest with one load of its size).
u32 EmuSuNiAttribAlign(u32 component_bytes, u32 components) {
    if (component_bytes >= 4) {
        return 4;
    }
    if (component_bytes == 2) {
        return components == 1 ? 2 : 4;
    }
    return components == 1 ? 1 : components == 4 ? 4 : 2;
}

// Whether a loader's misaligned attributes are moved to aligned offsets (scripts/build-3ds-core.sh, 00.95): not with
// the file /data/EmuSuNi/3ds-vertex-as-is (EMUSUNI_3DS_VERTEX_ALIGN=0 on the PC).
bool EmuSuNiAlignVertices() {
    static const bool on = [] {
        const char* env = std::getenv("EMUSUNI_3DS_VERTEX_ALIGN");
        const bool as_is = (env != nullptr && std::strcmp(env, "0") == 0) ||
                           ::access("/data/EmuSuNi/3ds-vertex-as-is", F_OK) == 0;
        LOG_INFO(Render_Vulkan, "EmuSuNi: vertex attributes {}",
                 as_is ? "as the game lays them out (the file /data/EmuSuNi/3ds-vertex-as-is)"
                       : "moved to aligned offsets where the game's are not (the file "
                         "/data/EmuSuNi/3ds-vertex-as-is: as the game lays them out)");
        return !as_is;
    }();
    return on;
}

// One attribute's bytes (1-4 bytes, 2-8 in shorts, 4-16 in floats): copies of a known size, inlined.
inline void EmuSuNiCopyAttribute(u8* dst, const u8* src, u32 bytes) {
    switch (bytes) {
    case 1:
        dst[0] = src[0];
        return;
    case 2:
        std::memcpy(dst, src, 2);
        return;
    case 3:
        std::memcpy(dst, src, 3);
        return;
    case 4:
        std::memcpy(dst, src, 4);
        return;
    case 6:
        std::memcpy(dst, src, 6);
        return;
    case 8:
        std::memcpy(dst, src, 8);
        return;
    case 12:
        std::memcpy(dst, src, 12);
        return;
    case 16:
        std::memcpy(dst, src, 16);
        return;
    default:
        std::memcpy(dst, src, bytes);
        return;
    }
}

// What the alignment moved, for the log every 600 frames (the emulation thread's).
u64 emusuni_vertices_seen = 0;
u64 emusuni_vertices_moved = 0;
u32 emusuni_loaders_moved = 0;
u32 emusuni_align_frames = 0;

// (00.95) A loader's vertices copied into the stream buffer, to bind them again for a later draw of the same command
// list instead of copying them again: the guest's CPU can't change them meanwhile (the list runs inside one HLE call),
// nor the GPU (a surface flushed back to the guest's memory would count a readback), and the copy stays where it is
// (the same submission - the stream buffer keeps a region until the submission that wrote it is done - and the
// stream buffer hasn't wrapped). Not with the file /data/EmuSuNi/3ds-vertex-reuse-off (EMUSUNI_3DS_VERTEX_REUSE=0).
struct EmuSuNiUpload {
    u64 list = 0;
    u64 readbacks = 0;
    u64 wraps = 0;
    u64 tick = 0;
    u64 shape = 0; // what the copy's layout depends on (the moved attributes' offsets)
    u32 base = 0;  // the loader's data in the guest's memory
    u32 byte_count = 0;
    u32 stride = 0;
    u32 min = 0;
    u32 max = 0;
    u32 offset = 0; // of vertex `min` in the stream buffer
};
std::array<EmuSuNiUpload, 16> emusuni_uploads{};
u32 emusuni_upload_next = 0;
u64 emusuni_bytes_copied = 0;
u64 emusuni_bytes_reused = 0;
u32 emusuni_loaders_reused = 0;

bool EmuSuNiReuseVertices() {
    static const bool on = [] {
        const char* env = std::getenv("EMUSUNI_3DS_VERTEX_REUSE");
        const bool off = (env != nullptr && std::strcmp(env, "0") == 0) ||
                         ::access("/data/EmuSuNi/3ds-vertex-reuse-off", F_OK) == 0;
        LOG_INFO(Render_Vulkan, "EmuSuNi: vertex data {}",
                 off ? "copied for every draw (the file /data/EmuSuNi/3ds-vertex-reuse-off)"
                     : "copied once per command list where draws share it (the file "
                       "/data/EmuSuNi/3ds-vertex-reuse-off: for every draw)");
        return !off;
    }();
    return on;
}

bool EmuSuNiFindUpload(const EmuSuNiUpload& want, u32& offset) {
    if (!EmuSuNiReuseVertices() || want.list == 0) {
        return false;
    }
    for (const EmuSuNiUpload& made : emusuni_uploads) {
        if (made.list == want.list && made.readbacks == want.readbacks && made.wraps == want.wraps &&
            made.tick == want.tick && made.base == want.base && made.byte_count == want.byte_count &&
            made.stride == want.stride && made.shape == want.shape && made.min <= want.min && want.max <= made.max) {
            offset = made.offset + (want.min - made.min) * made.stride;
            return true;
        }
    }
    return false;
}
#endif

constexpr u64 STREAM_BUFFER_SIZE = 64_MiB;
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    auto [array_ptr, array_offset, invalidate] = stream_buffer.Map(vs_input_size, 16);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.95): room for loaders whose attributes move to aligned offsets (each grows by at most 3 bytes,\n'
      '    // and its stride to a multiple of 4).\n'
      '    u32 emusuni_room = 0;\n'
      '    if (EmuSuNiAlignVertices()) {\n'
      '        const u32 vertices = vs_input_index_max - vs_input_index_min + 1;\n'
      '        for (const auto& loader : regs.pipeline.vertex_attributes.attribute_loaders) {\n'
      '            if (loader.component_count != 0 && loader.byte_count != 0) {\n'
      '                emusuni_room += (3 * static_cast<u32>(loader.component_count) + 3) * vertices + 4;\n'
      '            }\n'
      '        }\n'
      '    }\n'
      '    auto [array_ptr, array_offset, invalidate] = stream_buffer.Map(vs_input_size + emusuni_room, 16);\n'
      '#else\n'
      '    auto [array_ptr, array_offset, invalidate] = stream_buffer.Map(vs_input_size, 16);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        // Analyze the attribute loader by checking which attributes it provides\n        u32 offset = 0;\n',
      '        // Analyze the attribute loader by checking which attributes it provides\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (00.95): each attribute\'s input register, offset, size and the alignment its loads need.\n'
      '        std::array<std::array<u32, 4>, 12> emusuni_attrs;\n'
      '        u32 emusuni_count = 0;\n'
      '        bool emusuni_misaligned = false;\n'
      '#endif\n'
      '        u32 offset = 0;\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '            enable_attributes[input_reg] = true;\n            offset += vertex_attributes.GetStride(attribute_index);\n',
      '            enable_attributes[input_reg] = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '            const u32 emusuni_align =\n'
      '                EmuSuNiAttribAlign(vertex_attributes.GetElementSizeInBytes(attribute_index), size);\n'
      '            // (only an odd address for a load of 2 or more bytes: Kirby\'s 4-byte loads at even addresses that 4\n'
      '            // doesn\'t divide, nearly all its vertices, draw right on the PS4)\n'
      '            emusuni_misaligned |=\n'
      '                emusuni_align >= 2 &&\n'
      '                ((offset | Common::AlignUp(static_cast<u32>(loader.byte_count), stride_alignment)) & 1) != 0;\n'
      '            emusuni_attrs[emusuni_count++] = {input_reg, offset, vertex_attributes.GetStride(attribute_index),\n'
      '                                              emusuni_align};\n'
      '#endif\n'
      '            offset += vertex_attributes.GetStride(attribute_index);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        // Align stride up if required by Vulkan implementation.\n'
      '        const u32 aligned_stride =\n'
      '            Common::AlignUp(static_cast<u32>(loader.byte_count), stride_alignment);\n'
      '        if (aligned_stride == loader.byte_count) {\n'
      '            std::memcpy(dst_ptr, src_ptr, data_size);\n'
      '        } else {\n'
      '            for (std::size_t vertex = 0; vertex < vertex_num; vertex++) {\n'
      '                std::memcpy(dst_ptr + vertex * aligned_stride, src_ptr + vertex * loader.byte_count,\n'
      '                            loader.byte_count);\n'
      '            }\n'
      '        }\n',
      r'''#ifdef EMUSUNI_PORT
        // Align stride up if required by Vulkan implementation.
        u32 aligned_stride = Common::AlignUp(static_cast<u32>(loader.byte_count), stride_alignment);
        u32 emusuni_byte_count = loader.byte_count;
        emusuni_vertices_seen += vertex_num;
        // EmuSuNi (00.95): the attributes moved to offsets their loads' alignment divides, the stride a multiple of 4
        // (on the PS4 a load at an address it doesn't divide reads the wrong bytes).
        const bool emusuni_move = emusuni_misaligned && EmuSuNiAlignVertices();
        std::array<u32, 12> new_offsets{};
        u64 emusuni_shape = emusuni_move ? 1 : 0;
        if (emusuni_move) {
            u32 cursor = 0;
            for (u32 i = 0; i < emusuni_count; i++) {
                cursor = Common::AlignUp(cursor, emusuni_attrs[i][3]);
                new_offsets[i] = cursor;
                layout.attributes[emusuni_attrs[i][0]].offset.Assign(cursor);
                cursor += emusuni_attrs[i][2];
                emusuni_shape = emusuni_shape * 0x100000001B3ull ^
                                (emusuni_attrs[i][1] | emusuni_attrs[i][2] << 12 | new_offsets[i] << 18);
            }
            aligned_stride = Common::AlignUp(cursor, std::max<u32>(4, stride_alignment));
            emusuni_byte_count = aligned_stride;
        }
        // (00.95) The same vertices copied earlier in this command list and submission: bound there again.
        EmuSuNiUpload emusuni_this;
        emusuni_this.list = EmuSuNi::Stats::counts[EmuSuNi::Stats::CommandLists];
        emusuni_this.readbacks = EmuSuNi::Stats::counts[EmuSuNi::Stats::Readbacks];
        emusuni_this.wraps = stream_buffer.emusuni_wraps;
        emusuni_this.tick = scheduler.CurrentTick();
        emusuni_this.shape = emusuni_shape;
        emusuni_this.base = base_address + loader.data_offset;
        emusuni_this.byte_count = loader.byte_count;
        emusuni_this.stride = aligned_stride;
        emusuni_this.min = vs_input_index_min;
        emusuni_this.max = vs_input_index_max;
        u32 emusuni_reused_offset = 0;
        const bool emusuni_reuse = EmuSuNiFindUpload(emusuni_this, emusuni_reused_offset);
        if (emusuni_reuse) {
            emusuni_loaders_reused++;
            emusuni_bytes_reused += static_cast<u64>(aligned_stride) * vertex_num;
        } else if (emusuni_move) {
            const u32 new_stride = aligned_stride;
            // A batch of vertices made in ordinary memory, then copied in one piece (the stream buffer is write-
            // combined on the PS4: small scattered writes there are slow).
            alignas(16) u8 batch[4096];
            const std::size_t per_batch = std::max<std::size_t>(1, sizeof(batch) / new_stride);
            for (std::size_t first = 0; first < vertex_num; first += per_batch) {
                const std::size_t count = std::min<std::size_t>(per_batch, vertex_num - first);
                if (count * new_stride > sizeof(batch)) {
                    // (a stride over 4 KiB: one vertex at a time, straight into the buffer)
                    std::memset(dst_ptr + first * new_stride, 0, new_stride);
                    for (u32 i = 0; i < emusuni_count; i++) {
                        EmuSuNiCopyAttribute(dst_ptr + first * new_stride + new_offsets[i],
                                             src_ptr + first * loader.byte_count + emusuni_attrs[i][1],
                                             emusuni_attrs[i][2]);
                    }
                    continue;
                }
                std::memset(batch, 0, count * new_stride);
                for (std::size_t k = 0; k < count; k++) {
                    const u8* src = src_ptr + (first + k) * loader.byte_count;
                    u8* dst = batch + k * new_stride;
                    for (u32 i = 0; i < emusuni_count; i++) {
                        EmuSuNiCopyAttribute(dst + new_offsets[i], src + emusuni_attrs[i][1], emusuni_attrs[i][2]);
                    }
                }
                std::memcpy(dst_ptr + first * new_stride, batch, count * new_stride);
            }
            emusuni_vertices_moved += vertex_num;
            emusuni_loaders_moved++;
            static bool emusuni_logged = false;
            if (!emusuni_logged) {
                emusuni_logged = true;
                LOG_INFO(Render_Vulkan,
                         "EmuSuNi: vertex attributes moved to aligned offsets (the first: {} attributes in a stride "
                         "of {} bytes, now {})",
                         emusuni_count, static_cast<u32>(loader.byte_count), new_stride);
            }
        } else if (aligned_stride == loader.byte_count) {
            std::memcpy(dst_ptr, src_ptr, data_size);
        } else {
            for (std::size_t vertex = 0; vertex < vertex_num; vertex++) {
                std::memcpy(dst_ptr + vertex * aligned_stride, src_ptr + vertex * loader.byte_count,
                            loader.byte_count);
            }
        }
        if (!emusuni_reuse) {
            emusuni_this.offset = static_cast<u32>(array_offset + buffer_offset);
            emusuni_uploads[emusuni_upload_next++ % emusuni_uploads.size()] = emusuni_this;
            emusuni_bytes_copied += static_cast<u64>(aligned_stride) * vertex_num;
        }
#else
        // Align stride up if required by Vulkan implementation.
        const u32 aligned_stride =
            Common::AlignUp(static_cast<u32>(loader.byte_count), stride_alignment);
        if (aligned_stride == loader.byte_count) {
            std::memcpy(dst_ptr, src_ptr, data_size);
        } else {
            for (std::size_t vertex = 0; vertex < vertex_num; vertex++) {
                std::memcpy(dst_ptr + vertex * aligned_stride, src_ptr + vertex * loader.byte_count,
                            loader.byte_count);
            }
        }
#endif
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        // Will be adjusted on pipeline build, to keep the info transferable.\n'
      '        binding.byte_count.Assign(loader.byte_count);\n',
      '        // Will be adjusted on pipeline build, to keep the info transferable.\n'
      '#ifdef EMUSUNI_PORT\n'
      '        binding.byte_count.Assign(emusuni_byte_count); // (00.95: the new stride if the attributes moved)\n'
      '#else\n'
      '        binding.byte_count.Assign(loader.byte_count);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#ifdef EMUSUNI_PORT\n    pipeline_cache.EmuSuNiSaveNowAndThen();\n#endif\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n'
      '    // (00.95) What the vertex alignment moved, every 600 frames (nothing when it moved nothing).\n'
      '    if (++emusuni_align_frames >= 600) {\n'
      '        if (emusuni_vertices_moved != 0) {\n'
      '            LOG_INFO(Render_Vulkan,\n'
      '                     "EmuSuNi: vertex attributes moved to aligned offsets: {} of {} vertices ({} loaders) in "\n'
      '                     "the last 600 frames",\n'
      '                     emusuni_vertices_moved, emusuni_vertices_seen, emusuni_loaders_moved);\n'
      '        }\n'
      '        if (emusuni_bytes_copied + emusuni_bytes_reused != 0) {\n'
      '            LOG_INFO(Render_Vulkan,\n'
      '                     "EmuSuNi: vertex data in the last 600 frames: {} KiB copied, {} KiB bound again from "\n'
      '                     "earlier in their command list ({} loaders)",\n'
      '                     emusuni_bytes_copied / 1024, emusuni_bytes_reused / 1024, emusuni_loaders_reused);\n'
      '        }\n'
      '        emusuni_align_frames = 0;\n'
      '        emusuni_vertices_seen = 0;\n'
      '        emusuni_vertices_moved = 0;\n'
      '        emusuni_loaders_moved = 0;\n'
      '        emusuni_bytes_copied = 0;\n'
      '        emusuni_bytes_reused = 0;\n'
      '        emusuni_loaders_reused = 0;\n'
      '    }\n'
      '#endif\n'
      '}\n')
# The draws' time by part, for the log (00.95). The owner's 00.94 log: Driver Renegade 3D's draws took ~105 us each
# on the PS4 (69 a frame: 7-11 ms of a 22 ms frame), ~9.5 us on this PC, which does them 4-5 times faster otherwise;
# DKC Returns' ~13 us (500 a frame). Where that time goes on the PS4, each part timed with the CPU's time stamp counter
# (two reads a part: well under a microsecond a draw): the vertices (their analysis, the copy into the stream buffer,
# the indices), the shaders (the vertex, geometry and fragment shaders' lookups), the state and the framebuffers (the
# draw state, the surfaces drawn to, the render pass), the textures, the uniforms and lighting tables, binding the
# pipeline, recording the draw; a line of its own after each "600 frames" one (the PS4's log cuts long lines).
patch('src/common/emusuni_stats.h',
      '    ReadbackTicks, // (00.71) the surfaces read back from the GPU (RasterizerCache::DownloadSurface)\n    TickCount\n',
      '    ReadbackTicks, // (00.71) the surfaces read back from the GPU (RasterizerCache::DownloadSurface)\n'
      '    // (00.95) the draws\' parts (renderer_vulkan/vk_rasterizer.cpp)\n'
      '    DrawVertexTicks, DrawShaderTicks, DrawStateTicks, DrawTextureTicks, DrawUniformTicks, DrawPipelineTicks,\n'
      '    DrawRecordTicks,\n'
      '    TickCount\n')
patch('src/common/emusuni_frame_stats.h',
      '            LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, dt, dc, (now_us - period_t0) / 1000, tpu));\n',
      '            LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, dt, dc, (now_us - period_t0) / 1000, tpu));\n'
      '            // (00.95) The draws\' time by part (a line of its own: the PS4\'s log cuts long lines).\n'
      '            const auto part_ms = [tpu](std::uint64_t t) -> std::uint64_t {\n'
      '                return tpu > 0 ? static_cast<std::uint64_t>(static_cast<double>(t) / tpu / 1000.0) : 0;\n'
      '            };\n'
      '            const std::uint64_t parts = dt[DrawVertexTicks] + dt[DrawShaderTicks] + dt[DrawStateTicks] +\n'
      '                                        dt[DrawTextureTicks] + dt[DrawUniformTicks] + dt[DrawPipelineTicks] +\n'
      '                                        dt[DrawRecordTicks];\n'
      '            LOG_INFO(Frontend,\n'
      '                     "EmuSuNi: the draws of those 600 frames, {} ms: vertices {} ms, shaders {} ms, state and "\n'
      '                     "framebuffers {} ms, textures {} ms, uniforms and lighting tables {} ms, pipelines {} ms, "\n'
      '                     "recording {} ms, the rest {} ms",\n'
      '                     part_ms(dt[DrawTicks]), part_ms(dt[DrawVertexTicks]), part_ms(dt[DrawShaderTicks]),\n'
      '                     part_ms(dt[DrawStateTicks]), part_ms(dt[DrawTextureTicks]), part_ms(dt[DrawUniformTicks]),\n'
      '                     part_ms(dt[DrawPipelineTicks]), part_ms(dt[DrawRecordTicks]),\n'
      '                     part_ms(dt[DrawTicks] > parts ? dt[DrawTicks] - parts : 0));\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    SetupVertexArray();\n\n    if (!SetupVertexShader()) {\n        return false;\n    }\n'
      '    if (!SetupGeometryShader()) {\n        return false;\n    }\n',
      '#ifdef EMUSUNI_PORT\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawVertexTicks}; // (00.95)\n'
      '        SetupVertexArray();\n'
      '    }\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawShaderTicks};\n'
      '        if (!SetupVertexShader()) {\n'
      '            return false;\n'
      '        }\n'
      '        if (!SetupGeometryShader()) {\n'
      '            return false;\n'
      '        }\n'
      '    }\n'
      '#else\n'
      '    SetupVertexArray();\n\n    if (!SetupVertexShader()) {\n        return false;\n    }\n'
      '    if (!SetupGeometryShader()) {\n        return false;\n    }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    MICROPROFILE_SCOPE(Vulkan_Drawing);\n    SyncDrawState();\n',
      '    MICROPROFILE_SCOPE(Vulkan_Drawing);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    std::optional<EmuSuNi::Stats::TickTimer> emusuni_state{EmuSuNi::Stats::DrawStateTicks}; // (00.95)\n'
      '#endif\n'
      '    SyncDrawState();\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    // Sync and bind the texture surfaces\n    SyncTextureUnits(framebuffer);\n    SyncUtilityTextures(framebuffer);\n\n'
      '    // Sync and bind the shader\n    pipeline_cache.UseFragmentShader(regs, user_config);\n\n'
      '    // Sync the LUTs within the texture buffer\n    SyncAndUploadLUTs();\n    SyncAndUploadLUTsLF();\n'
      '    UploadUniforms(accelerate);\n\n'
      '    // Begin rendering\n    const auto draw_rect = fb_helper.DrawRect();\n'
      '    renderpass_cache.BeginRendering(framebuffer, draw_rect);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_state.reset(); // (00.95: the draw state and the framebuffers so far)\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawTextureTicks};\n'
      '        SyncTextureUnits(framebuffer);\n'
      '        SyncUtilityTextures(framebuffer);\n'
      '    }\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawShaderTicks};\n'
      '        pipeline_cache.UseFragmentShader(regs, user_config);\n'
      '    }\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawUniformTicks};\n'
      '        SyncAndUploadLUTs();\n'
      '        SyncAndUploadLUTsLF();\n'
      '        UploadUniforms(accelerate);\n'
      '    }\n'
      '    emusuni_state.emplace(EmuSuNi::Stats::DrawStateTicks);\n'
      '    const auto draw_rect = fb_helper.DrawRect();\n'
      '    renderpass_cache.BeginRendering(framebuffer, draw_rect);\n'
      '    emusuni_state.reset();\n'
      '#else\n'
      '    // Sync and bind the texture surfaces\n    SyncTextureUnits(framebuffer);\n    SyncUtilityTextures(framebuffer);\n\n'
      '    // Sync and bind the shader\n    pipeline_cache.UseFragmentShader(regs, user_config);\n\n'
      '    // Sync the LUTs within the texture buffer\n    SyncAndUploadLUTs();\n    SyncAndUploadLUTsLF();\n'
      '    UploadUniforms(accelerate);\n\n'
      '    // Begin rendering\n    const auto draw_rect = fb_helper.DrawRect();\n'
      '    renderpass_cache.BeginRendering(framebuffer, draw_rect);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    if (is_indexed) {\n        SetupIndexArray();\n    }\n\n'
      '    const bool wait_built = !async_shaders || regs.pipeline.num_vertices <= 6;\n'
      '    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '    if (is_indexed) {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawVertexTicks}; // (00.95)\n'
      '        SetupIndexArray();\n'
      '    }\n\n'
      '    const bool wait_built = !async_shaders || regs.pipeline.num_vertices <= 6;\n'
      '    bool emusuni_bound;\n'
      '    {\n'
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawPipelineTicks};\n'
      '        emusuni_bound = pipeline_cache.BindPipeline(pipeline_info, wait_built);\n'
      '    }\n'
      '    if (!emusuni_bound) {\n'
      '#else\n'
      '    if (is_indexed) {\n        SetupIndexArray();\n    }\n\n'
      '    const bool wait_built = !async_shaders || regs.pipeline.num_vertices <= 6;\n'
      '    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    scheduler.Record([this, params](vk::CommandBuffer cmdbuf) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_record{EmuSuNi::Stats::DrawRecordTicks}; // (00.95)\n'
      '#endif\n'
      '    scheduler.Record([this, params](vk::CommandBuffer cmdbuf) {\n')
# Vertex data copied once per command list (00.95). The same profile: Driver Renegade 3D's vertex setup was ~80% of its
# draws' time on this PC (memcpy: each draw copied its indices' whole vertex range, 10-25 MB a frame for 67 draws that
# share their meshes' buffers); on the PS4 into the write-combined stream buffer. A loader's copy is bound again for a
# later draw of the same command list and submission whose range it holds (EmuSuNiFindUpload above). On this PC about
# half the vertex data is bound again in Driver and DKC Returns: Driver's vertex time 314 -> 170 ms per 600 frames
# (the draws' 405 -> 246), DKC's 139 -> 86; Driver's frames the same with it and without.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        binding_offsets[layout.binding_count++] = static_cast<u32>(array_offset + buffer_offset);\n'
      '        buffer_offset += Common::AlignUp(aligned_stride * vertex_num, 4);\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // (00.95) Copied now, or earlier in this command list (where vertex `min` of this draw lies there).\n'
      '        binding_offsets[layout.binding_count++] =\n'
      '            emusuni_reuse ? emusuni_reused_offset : static_cast<u32>(array_offset + buffer_offset);\n'
      '        if (!emusuni_reuse) {\n'
      '            buffer_offset += Common::AlignUp(aligned_stride * vertex_num, 4);\n'
      '        }\n'
      '#else\n'
      '        binding_offsets[layout.binding_count++] = static_cast<u32>(array_offset + buffer_offset);\n'
      '        buffer_offset += Common::AlignUp(aligned_stride * vertex_num, 4);\n'
      '#endif\n')
patch('src/common/emusuni_stats.h',
      '    Readbacks, ReadbackBytes, // (00.71)\n',
      '    Readbacks, ReadbackBytes, // (00.71)\n'
      '    CommandLists, // (00.95) the PICA command lists run (vertex data is copied once per list)\n')
patch('src/video_core/pica/pica_core.cpp',
      'void PicaCore::ProcessCmdList(PAddr list, u32 size, bool ignore_list) [[hot]] {\n',
      'void PicaCore::ProcessCmdList(PAddr list, u32 size, bool ignore_list) [[hot]] {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::CommandLists]++; // (00.95: renderer_vulkan/vk_rasterizer.cpp)\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_stream_buffer.h',
      '    vk::Buffer Handle() const noexcept {\n        return buffer;\n    }\n\nprivate:\n',
      '    vk::Buffer Handle() const noexcept {\n        return buffer;\n    }\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    u64 emusuni_wraps = 0; // (00.95) times the buffer started over from its beginning\n'
      '#endif\n\n'
      'private:\n')
patch('src/video_core/renderer_vulkan/vk_stream_buffer.cpp',
      '        // The buffer would overflow, save the amount of used watches and reset the state.\n'
      '        invalidate = true;\n',
      '        // The buffer would overflow, save the amount of used watches and reset the state.\n'
      '        invalidate = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        emusuni_wraps++; // (00.95: copies made before can\'t be bound again)\n'
      '#endif\n')

# --- 00.96: vertex compaction. The owner's 00.95 log (04-11): Driver Renegade 3D's draws still 4.9 s of every 600
# frames of its city (~8 ms a frame of a 20 ms one), the vertices 82% of it (6.7 ms a frame): ~20 MB of vertex data
# copied a frame into the write-combined stream buffer, and as much bound again (00.95), for ~170 indices a draw. Each
# draw copies its indices' whole range of vertices (the meshes share big buffers: ~2300 vertices for ~150 used). Now an
# indexed draw whose indices use at most half of their range (and the range is 64 to 32768 vertices) copies only the
# vertices they use, in the order they first use them, and is drawn with the indices renumbered (16-bit) from vertex 0;
# the vertices are the same, and so is what the GPU draws. A table of the range's vertices finds each one's new number
# (4 bytes a vertex of the biggest range met: at most 128 KiB, ~10 KiB for Driver), stamped per draw so it is never
# cleared but when the stamp wraps. Such a draw binds nothing again (00.95's reuse: the others do). The file
# /data/EmuSuNi/3ds-vertex-compact-off (EMUSUNI_3DS_VERTEX_COMPACT=0 on the PC) copies the whole range as before; every
# 600 frames the log says how many draws were compacted and how many vertices they copied of their ranges'.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#include <optional>\n#include <unistd.h>\n',
      '#include <optional>\n#include <unistd.h>\n#include <vector>\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    return false;\n}\n#endif\n\nconstexpr u64 STREAM_BUFFER_SIZE = 64_MiB;\n',
      r'''    return false;
}

// (00.96) Vertex compaction (scripts/build-3ds-core.sh): not with the file /data/EmuSuNi/3ds-vertex-compact-off
// (EMUSUNI_3DS_VERTEX_COMPACT=0 on the PC).
bool EmuSuNiCompactVertices() {
    static const bool on = [] {
        const char* env = std::getenv("EMUSUNI_3DS_VERTEX_COMPACT");
        const bool off = (env != nullptr && std::strcmp(env, "0") == 0) ||
                         ::access("/data/EmuSuNi/3ds-vertex-compact-off", F_OK) == 0;
        LOG_INFO(Render_Vulkan, "EmuSuNi: vertex data {}",
                 off ? "copied for each draw's whole index range (the file /data/EmuSuNi/3ds-vertex-compact-off)"
                     : "copied only where a draw's indices use it when they use at most half of their range (the "
                       "file /data/EmuSuNi/3ds-vertex-compact-off: the whole range)");
        return !off;
    }();
    return on;
}

bool emusuni_draw_indexed = false;  // the draw being set up is indexed (AccelerateDrawBatch)
bool emusuni_compact = false;       // its vertices compacted (SetupVertexArray; SetupIndexArray, the draw's offset)
std::vector<u32> emusuni_remap;     // each vertex of the range: (the draw's stamp << 16) | its new number
std::vector<u16> emusuni_order;     // the new numbers' vertices (from the range's first)
std::vector<u16> emusuni_indices;   // the draw's indices renumbered
u32 emusuni_stamp = 0;
u64 emusuni_compact_draws = 0;
u64 emusuni_compact_vertices = 0;
u64 emusuni_compact_range = 0;

// The draw's indices renumbered in the order they first use each vertex of [min, min + range).
bool EmuSuNiCompact(const u8* index_data, bool u16_indices, u32 count, u32 min, u32 range) {
    if (emusuni_remap.size() < range) {
        emusuni_remap.resize(range, 0);
    }
    if (++emusuni_stamp > 0xFFFF) {
        std::fill(emusuni_remap.begin(), emusuni_remap.end(), 0u);
        emusuni_stamp = 1;
    }
    const u32 tag = emusuni_stamp << 16;
    emusuni_order.clear();
    emusuni_indices.resize(count);
    for (u32 i = 0; i < count; i++) {
        u32 index;
        if (u16_indices) {
            u16 value;
            std::memcpy(&value, index_data + 2 * i, 2);
            index = value;
        } else {
            index = index_data[i];
        }
        const u32 vertex = index - min;
        if (vertex >= range) {
            return false; // (not one of the range's: the whole range is copied)
        }
        u32& entry = emusuni_remap[vertex];
        if ((entry & 0xFFFF0000u) != tag) {
            entry = tag | static_cast<u32>(emusuni_order.size());
            emusuni_order.push_back(static_cast<u16>(vertex));
        }
        emusuni_indices[i] = static_cast<u16>(entry & 0xFFFF);
    }
    return true;
}

// The compacted vertices of one loader, in their new order: a batch made in ordinary memory, then copied in one piece
// (the stream buffer is write-combined on the PS4). With `moved` attributes (00.95's alignment) each goes to its new
// offset; else the vertex's bytes as they are.
void EmuSuNiCopyCompact(u8* dst, const u8* src, u32 src_stride, u32 dst_stride, u32 moved,
                        const std::array<std::array<u32, 4>, 12>& attrs, const std::array<u32, 12>& new_offsets) {
    const auto make = [&](u8* to, std::size_t k) {
        const u8* from = src + std::size_t{emusuni_order[k]} * src_stride;
        if (moved == 0) {
            std::memcpy(to, from, src_stride);
            return;
        }
        for (u32 i = 0; i < moved; i++) {
            EmuSuNiCopyAttribute(to + new_offsets[i], from + attrs[i][1], attrs[i][2]);
        }
    };
    alignas(16) u8 batch[4096];
    const std::size_t used = emusuni_order.size();
    const std::size_t per_batch = std::max<std::size_t>(1, sizeof(batch) / dst_stride);
    for (std::size_t first = 0; first < used; first += per_batch) {
        const std::size_t count = std::min<std::size_t>(per_batch, used - first);
        if (count * dst_stride > sizeof(batch)) {
            // (a stride over 4 KiB: one vertex at a time, straight into the buffer)
            std::memset(dst + first * dst_stride, 0, dst_stride);
            make(dst + first * dst_stride, first);
            continue;
        }
        if (moved != 0 || dst_stride != src_stride) {
            std::memset(batch, 0, count * dst_stride);
        }
        for (std::size_t k = 0; k < count; k++) {
            make(batch + k * dst_stride, first + k);
        }
        std::memcpy(dst + first * dst_stride, batch, count * dst_stride);
    }
}
#endif

constexpr u64 STREAM_BUFFER_SIZE = 64_MiB;
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    const PAddr base_address = vertex_attributes.GetPhysicalBaseAddress(); // GPUREG_ATTR_BUF_BASE\n'
      '    const u32 stride_alignment = instance.GetMinVertexStrideAlignment();\n',
      '    const PAddr base_address = vertex_attributes.GetPhysicalBaseAddress(); // GPUREG_ATTR_BUF_BASE\n'
      '    const u32 stride_alignment = instance.GetMinVertexStrideAlignment();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (00.96): an indexed draw that uses at most half of its index range\'s vertices copies only those\n'
      '    // (EmuSuNiCompact): the indices renumbered in SetupIndexArray, the draw from vertex 0.\n'
      '    emusuni_compact = false;\n'
      '    if (emusuni_draw_indexed && EmuSuNiCompactVertices()) {\n'
      '        const u32 count = regs.pipeline.num_vertices;\n'
      '        const u32 range = vs_input_index_max - vs_input_index_min + 1;\n'
      '        if (range >= 64 && range <= 32768 && count <= range / 2) {\n'
      '            const u8* index_data =\n'
      '                memory.GetPhysicalPointer(base_address + regs.pipeline.index_array.offset);\n'
      '            emusuni_compact = index_data != nullptr &&\n'
      '                              EmuSuNiCompact(index_data, regs.pipeline.index_array.format != 0, count,\n'
      '                                             vs_input_index_min, range);\n'
      '            if (emusuni_compact) {\n'
      '                emusuni_compact_draws++;\n'
      '                emusuni_compact_vertices += emusuni_order.size();\n'
      '                emusuni_compact_range += range;\n'
      '            }\n'
      '        }\n'
      '    }\n'
      '    const u32 emusuni_used = emusuni_compact ? static_cast<u32>(emusuni_order.size()) : 0;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        const bool emusuni_reuse = EmuSuNiFindUpload(emusuni_this, emusuni_reused_offset);\n'
      '        if (emusuni_reuse) {\n'
      '            emusuni_loaders_reused++;\n'
      '            emusuni_bytes_reused += static_cast<u64>(aligned_stride) * vertex_num;\n'
      '        } else if (emusuni_move) {\n',
      '        const bool emusuni_reuse = !emusuni_compact && EmuSuNiFindUpload(emusuni_this, emusuni_reused_offset);\n'
      '        if (emusuni_reuse) {\n'
      '            emusuni_loaders_reused++;\n'
      '            emusuni_bytes_reused += static_cast<u64>(aligned_stride) * vertex_num;\n'
      '        } else if (emusuni_compact) {\n'
      '            // (00.96) Only the vertices the indices use, in their new order.\n'
      '            EmuSuNiCopyCompact(dst_ptr, src_ptr, loader.byte_count, aligned_stride,\n'
      '                               emusuni_move ? emusuni_count : 0, emusuni_attrs, new_offsets);\n'
      '            if (emusuni_move) {\n'
      '                emusuni_vertices_moved += emusuni_used;\n'
      '                emusuni_loaders_moved++;\n'
      '            }\n'
      '        } else if (emusuni_move) {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        if (!emusuni_reuse) {\n'
      '            emusuni_this.offset = static_cast<u32>(array_offset + buffer_offset);\n',
      '        if (emusuni_compact) {\n'
      '            emusuni_bytes_copied += static_cast<u64>(aligned_stride) * emusuni_used; // (00.96: not bound again)\n'
      '        } else if (!emusuni_reuse) {\n'
      '            emusuni_this.offset = static_cast<u32>(array_offset + buffer_offset);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        if (!emusuni_reuse) {\n'
      '            buffer_offset += Common::AlignUp(aligned_stride * vertex_num, 4);\n'
      '        }\n',
      '        if (!emusuni_reuse) {\n'
      '            // (00.96: only the vertices used when compacted)\n'
      '            buffer_offset += Common::AlignUp(aligned_stride * (emusuni_compact ? emusuni_used : vertex_num), 4);\n'
      '        }\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawVertexTicks}; // (00.95)\n'
      '        SetupVertexArray();\n',
      '        const EmuSuNi::Stats::TickTimer emusuni_t{EmuSuNi::Stats::DrawVertexTicks}; // (00.95)\n'
      '        emusuni_draw_indexed = is_indexed; // (00.96: SetupVertexArray\'s compaction)\n'
      '        SetupVertexArray();\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        .vertex_offset = -static_cast<s32>(vertex_info.vs_input_index_min),\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // (00.96: compacted, the renumbered indices count from vertex 0)\n'
      '        .vertex_offset = emusuni_compact ? 0 : -static_cast<s32>(vertex_info.vs_input_index_min),\n'
      '#else\n'
      '        .vertex_offset = -static_cast<s32>(vertex_info.vs_input_index_min),\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::SetupIndexArray() {\n',
      r'''void RasterizerVulkan::SetupIndexArray() {
#ifdef EMUSUNI_PORT
    if (emusuni_compact) {
        // EmuSuNi (00.96): the indices renumbered for the compacted vertices (SetupVertexArray), 16-bit.
        const u32 size = static_cast<u32>(emusuni_indices.size()) * 2;
        auto [index_ptr, index_offset, _] = stream_buffer.Map(size, 2);
        std::memcpy(index_ptr, emusuni_indices.data(), size);
        stream_buffer.Commit(size);
        scheduler.Record([this, index_offset = index_offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindIndexBuffer(stream_buffer.Handle(), index_offset, vk::IndexType::eUint16);
        });
        return;
    }
#endif
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        emusuni_align_frames = 0;\n',
      '        if (emusuni_compact_draws != 0) {\n'
      '            LOG_INFO(Render_Vulkan,\n'
      '                     "EmuSuNi: vertex compaction in the last 600 frames: {} draws, {} vertices copied of the {} "\n'
      '                     "in their index ranges",\n'
      '                     emusuni_compact_draws, emusuni_compact_vertices, emusuni_compact_range);\n'
      '        }\n'
      '        emusuni_compact_draws = 0; // (00.96)\n'
      '        emusuni_compact_vertices = 0;\n'
      '        emusuni_compact_range = 0;\n'
      '        emusuni_align_frames = 0;\n')

# --- 00.98: indices from vertex 0. The owner's 00.97 test (04-11): Super Smash Bros.' fighters still broken on the PS4
# (photos: Zelda's dress in long flat shards to one side, Kirby full of spikes; the stage right), drawn right with the
# vertex shaders on the CPU (3ds-cpu-shaders) and never wrong on this PC (lavapipe), whatever was tried; without
# pipeline parts (3ds-gpl-off), with the GLSL fragment shaders (3ds-glsl-shaders) or the accurate multiplication: as
# broken. On this PC the fighters' bodies are one vertex program's draws (skipped, only their shadows are left), and 88%
# of them start above index 0 (up to 5466): Azahar draws such a draw from its first index's vertex with a negative
# vertex offset (-that index). The stage is another program's draws doing the same skinning (two bones read through the
# address registers, the same jump tables), always from index 0, and it is right on the PS4. So are Kirby: Planet
# Robobot and Pokemon Ultra Sun, which never start a draw above index 0 (Kirby's indices reach 5236 from 0; its models'
# vertex formats, strides and shader constructs are those of Smash's fighters). What breaks on the PS4 is the negative
# offset (Pokemon X starts thousands of draws above index 0 too). So an indexed draw that starts above index 0 is now
# given its indices minus its first one (16-bit) and drawn from vertex 0: the vertices bound already start at its first
# index (SetupVertexArray copies from there), so the GPU draws the same triangles without a negative offset. One
# subtraction an index. The file /data/EmuSuNi/3ds-index-as-is (EMUSUNI_3DS_INDEX_REBASE=0 on the PC) gives the game's
# indices and the negative offset as before; every 600 frames the log says how many draws started from vertex 0 that
# way.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'u64 emusuni_compact_range = 0;\n',
      r'''u64 emusuni_compact_range = 0;

// (00.98) A draw that starts above index 0 drawn from vertex 0, its indices minus its first one (SetupIndexArray): not
// with the file /data/EmuSuNi/3ds-index-as-is (EMUSUNI_3DS_INDEX_REBASE=0 on the PC).
bool EmuSuNiRebaseIndices() {
    static const bool on = [] {
        const char* env = std::getenv("EMUSUNI_3DS_INDEX_REBASE");
        const bool off = (env != nullptr && std::strcmp(env, "0") == 0) ||
                         ::access("/data/EmuSuNi/3ds-index-as-is", F_OK) == 0;
        LOG_INFO(Render_Vulkan, "EmuSuNi: indexed draws {}",
                 off ? "with the game's indices and a negative vertex offset (the file /data/EmuSuNi/3ds-index-as-is)"
                     : "from vertex 0, their indices minus their first one (the file /data/EmuSuNi/3ds-index-as-is: "
                       "the game's indices)");
        return !off;
    }();
    return on;
}

bool emusuni_rebased = false; // the indexed draw being set up drawn from vertex 0 (SetupIndexArray, its offset)
u64 emusuni_rebased_draws = 0;
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        emusuni_draw_indexed = is_indexed; // (00.96: SetupVertexArray\'s compaction)\n',
      '        emusuni_draw_indexed = is_indexed; // (00.96: SetupVertexArray\'s compaction)\n'
      '        emusuni_rebased = false;           // (00.98: SetupIndexArray says)\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        // (00.96: compacted, the renumbered indices count from vertex 0)\n'
      '        .vertex_offset = emusuni_compact ? 0 : -static_cast<s32>(vertex_info.vs_input_index_min),\n',
      '        // (00.96: compacted, the renumbered indices count from vertex 0; 00.98: or rebased)\n'
      '        .vertex_offset =\n'
      '            emusuni_compact || emusuni_rebased ? 0 : -static_cast<s32>(vertex_info.vs_input_index_min),\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '''        scheduler.Record([this, index_offset = index_offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindIndexBuffer(stream_buffer.Handle(), index_offset, vk::IndexType::eUint16);
        });
        return;
    }
#endif
''',
      r'''        scheduler.Record([this, index_offset = index_offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindIndexBuffer(stream_buffer.Handle(), index_offset, vk::IndexType::eUint16);
        });
        return;
    }
    const u32 emusuni_first = vertex_info.vs_input_index_min;
    if (emusuni_first != 0 && EmuSuNiRebaseIndices()) {
        // EmuSuNi (00.98): the indices minus the draw's first one, 16-bit, drawn from vertex 0 (the vertices bound
        // start there). Made in ordinary memory and copied in pieces: the stream buffer is write-combined on the PS4.
        const u8* data = memory.GetPhysicalPointer(regs.pipeline.vertex_attributes.GetPhysicalBaseAddress() +
                                                   regs.pipeline.index_array.offset);
        if (data != nullptr) {
            const bool u16_indices = regs.pipeline.index_array.format != 0;
            const u32 count = regs.pipeline.num_vertices;
            auto [index_ptr, index_offset, _] = stream_buffer.Map(count * 2, 2);
            alignas(16) std::array<u16, 1024> piece;
            for (u32 first = 0; first < count; first += static_cast<u32>(piece.size())) {
                const u32 n = std::min<u32>(static_cast<u32>(piece.size()), count - first);
                for (u32 i = 0; i < n; i++) {
                    u32 index;
                    if (u16_indices) {
                        u16 value;
                        std::memcpy(&value, data + 2 * (first + i), 2);
                        index = value;
                    } else {
                        index = data[first + i];
                    }
                    piece[i] = static_cast<u16>(index - emusuni_first);
                }
                std::memcpy(index_ptr + 2 * first, piece.data(), 2 * n);
            }
            stream_buffer.Commit(count * 2);
            scheduler.Record([this, index_offset = index_offset](vk::CommandBuffer cmdbuf) {
                cmdbuf.bindIndexBuffer(stream_buffer.Handle(), index_offset, vk::IndexType::eUint16);
            });
            emusuni_rebased = true;
            emusuni_rebased_draws++;
            return;
        }
    }
#endif
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        emusuni_compact_draws = 0; // (00.96)\n',
      '        if (emusuni_rebased_draws != 0) {\n'
      '            LOG_INFO(Render_Vulkan, "EmuSuNi: indexed draws from vertex 0 in the last 600 frames: {} (00.98)",\n'
      '                     emusuni_rebased_draws);\n'
      '        }\n'
      '        emusuni_rebased_draws = 0; // (00.98)\n'
      '        emusuni_compact_draws = 0; // (00.96)\n')

# --- 01.00: vertex shaders' jumps without the jump loop. The owner's 00.98 test (04-11): Smash's fighters as broken
# with 00.98's indices from vertex 0 (the draws starting above index 0 were not it). On this PC, in the GLSL the
# decompiler writes, one thing sets the fighters' vertex shader apart from the stage's (the same skinning by two bones,
# the same vertex layouts, right on the PS4) and from every one of Kirby: Planet Robobot's (right too): its bone lookups
# (the MOVA and the twelve reads through the address registers) sit in the block that opens a subroutine with a jump,
# and the decompiler writes such a subroutine inside its jump loop (while (true) { switch (jmp_to) { case ... } });
# everyone else's are in straight code. RADV 25.2 on this PC makes those reads per vertex, as it should, and lavapipe
# and the CPU's shaders draw the fighters right; the PS4's RADV is Mesa 26.0.8, likely the one compiling them wrong
# there. Now a subroutine whose jumps all go forward to its own blocks is written without the loop: its blocks in
# order, each run if (jmp_to <= its label), a jump setting jmp_to and skipping the rest of its block (an else). The
# same program, with the bone lookups in straight code as in the stage's shader. A jump back, or into the middle of an
# IF or LOOP block, keeps the jump loop. The file /data/EmuSuNi/3ds-vs-jump-loop (EMUSUNI_3DS_VS_JUMP_LOOP=1 on the
# PC) keeps it everywhere; the log says which at the first vertex shader made.
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#endif\n',
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <unistd.h>\n#endif\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '#define EMUSUNI_DECOMPILE_FAIL(why, ...) { EmuSuNiFail(why); return __VA_ARGS__; }\n',
      r'''#define EMUSUNI_DECOMPILE_FAIL(why, ...) { EmuSuNiFail(why); return __VA_ARGS__; }
// (01.00) A subroutine with jumps written without the jump loop where it can be (GLSLGenerator::EmuSuNiStraight): not
// with the file /data/EmuSuNi/3ds-vs-jump-loop (EMUSUNI_3DS_VS_JUMP_LOOP=1 on the PC).
static bool EmuSuNiStraightJumps() {
    static const bool on = [] {
        const bool loop = std::getenv("EMUSUNI_3DS_VS_JUMP_LOOP") != nullptr ||
                          ::access("/data/EmuSuNi/3ds-vs-jump-loop", F_OK) == 0;
        LOG_INFO(HW_GPU, "EmuSuNi: vertex shaders' jumps {}",
                 loop ? "in the jump loop everywhere (the file /data/EmuSuNi/3ds-vs-jump-loop)"
                      : "without the jump loop where they all go forward (the file /data/EmuSuNi/3ds-vs-jump-loop: "
                        "the jump loop everywhere)");
        return !loop;
    }();
    return on;
}
''')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '    std::string MoveResult() {\n        return std::move(shader_source);\n    }\n',
      '    std::string MoveResult() {\n        return std::move(shader_source);\n    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (01.00) What is written so far, and back to it (a subroutine tried without its jump loop).\n'
      '    std::size_t EmuSuNiSize() const {\n'
      '        return shader_source.size();\n'
      '    }\n'
      '    void EmuSuNiTruncate(std::size_t size) {\n'
      '        shader_source.resize(size);\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '                shader.AddLine("if ({}) {{", condition);\n'
      '                ++shader.scope;\n'
      '                shader.AddLine("{{ jmp_to = {}u; break; }}",\n'
      '                               instr.flow_control.dest_offset.Value());\n',
      r'''#ifdef EMUSUNI_PORT
                if (emusuni_straight) {
                    // EmuSuNi (01.00): without the jump loop (EmuSuNiStraight), the jump skips the rest of its block
                    // (an else, closed at the block's end) and the blocks before its target (if (jmp_to <= label)).
                    if (instr.flow_control.dest_offset.Value() <= offset) {
                        emusuni_straight_failed = true; // (a jump back: the jump loop after all)
                    }
                    shader.AddLine("if ({}) {{", condition);
                    ++shader.scope;
                    shader.AddLine("jmp_to = {}u;", instr.flow_control.dest_offset.Value());
                    --shader.scope;
                    shader.AddLine("}} else {{");
                    ++shader.scope;
                    ++emusuni_open_elses;
                    break;
                }
#endif
                shader.AddLine("if ({}) {{", condition);
                ++shader.scope;
                shader.AddLine("{{ jmp_to = {}u; break; }}",
                               instr.flow_control.dest_offset.Value());
''')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '    void Generate() {\n        if (sanitize_mul) {\n',
      r'''#ifdef EMUSUNI_PORT
    /// EmuSuNi (01.00): a subroutine with jumps written without upstream's jump loop when every jump in it goes forward
    /// to one of its blocks: the blocks in order, each run if (jmp_to <= its label), a jump setting jmp_to and skipping
    /// the rest of its block (CompileInstr's JMPC/JMPU: an else). The same program. Nothing written, and false, when a
    /// jump goes back or into the middle of an IF or LOOP block (the jump loop then), or with the file
    /// /data/EmuSuNi/3ds-vs-jump-loop.
    bool EmuSuNiStraight(const Subroutine& subroutine) {
        if (!EmuSuNiStraightJumps()) {
            return false;
        }
        std::set<u32> labels = subroutine.labels;
        labels.insert(subroutine.begin);
        if (*labels.begin() < subroutine.begin) {
            return false; // (a jump back before the subroutine's start)
        }
        const std::size_t mark = shader.EmuSuNiSize();
        const int scope = shader.scope;
        emusuni_straight = true;
        emusuni_straight_failed = false;
        shader.AddLine("uint jmp_to = {}u;", subroutine.begin);
        for (auto it = labels.begin(); it != labels.end() && !emusuni_straight_failed; ++it) {
            const auto next = std::next(it);
            const u32 next_label = next == labels.end() ? subroutine.end : *next;
            shader.AddLine("if (jmp_to <= {}u) {{", *it);
            ++shader.scope;
            emusuni_open_elses = 0;
            const u32 compile_end = CompileRange(*it, next_label);
            if (compile_end > next_label && compile_end != PROGRAM_END) {
                emusuni_straight_failed = true; // (a label inside an IF or LOOP block)
            }
            for (; emusuni_open_elses > 0; --emusuni_open_elses) {
                --shader.scope;
                shader.AddLine("}}");
            }
            --shader.scope;
            shader.AddLine("}}");
        }
        emusuni_straight = false;
        if (emusuni_straight_failed) {
            shader.EmuSuNiTruncate(mark);
            shader.scope = scope;
            return false;
        }
        shader.AddLine("return false;");
        return true;
    }
#endif

    void Generate() {
        if (sanitize_mul) {
''')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '            if (labels.empty()) {\n'
      '                if (CompileRange(subroutine.begin, subroutine.end) != PROGRAM_END) {\n'
      '                    shader.AddLine("return false;");\n'
      '                }\n'
      '            } else {\n',
      '            if (labels.empty()) {\n'
      '                if (CompileRange(subroutine.begin, subroutine.end) != PROGRAM_END) {\n'
      '                    shader.AddLine("return false;");\n'
      '                }\n'
      '#ifdef EMUSUNI_PORT\n'
      '            } else if (EmuSuNiStraight(subroutine)) {\n'
      '                // (01.00) written without the jump loop (EmuSuNiStraight)\n'
      '#endif\n'
      '            } else {\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '    ShaderWriter shader;\n};\n',
      '#ifdef EMUSUNI_PORT\n'
      '    bool emusuni_straight = false;        // (01.00) a subroutine being written without its jump loop\n'
      '    bool emusuni_straight_failed = false; // a jump back met: the jump loop after all\n'
      '    int emusuni_open_elses = 0;           // the elses its block\'s jumps opened (closed at the block\'s end)\n'
      '#endif\n'
      '    ShaderWriter shader;\n};\n')

# --- 01.01: the CPU JIT's warm-up. The owner's 01.00 log (04-11): Super Smash Bros.' slow frames are nearly all
# dynarmic compiling. The warm-up (00.69) compiled 197,747 blocks kept from earlier sessions (10.2 s over the game's
# first 1386 frames; the list grows each session, 200,000 at most) into core 0's 40 MiB code cache (00.90), leaving the
# game only 8 MiB; the fights' modules (CROs: fighters, stages, code the warm-up never keeps) brought 61,000-68,000 more
# blocks a 600 frames, and at 47 s the cache was full: a whole flush, every block compiled again. Now:
# - The warm-up compiles ahead only the list's first 60,000 blocks, leaving at least half the code cache to the game
#   (8 MiB before); the rest the game compiles when it meets them. And the list is kept in the order of last use as far
#   as it can be told: a block compiled ahead can't tell whether the game ran it, but one past the first 60,000 can
#   (the game compiles it). So the list written at the end is the first 20,000 (the boot, compiled first), then the
#   blocks the game compiled itself this session (met now: ahead next time), then the rest of those compiled ahead,
#   then those past them the game didn't meet; past 200,000 the last of these go. Blocks met no more sink and leave.
# - Short of room for a core's code cache (another process's JITs), its size halves down to 4 MiB (8 before) rather
#   than fail: core 1's is 8 MiB since 00.90. The log says each core's size.
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '        if (config.code_cache_size < 8u << 20) {\n'
      '            throw std::runtime_error(\n'
      '                fmt::format("no executable memory for the CPU JIT ({} MiB)", config.code_cache_size >> 20));\n'
      '        }\n',
      '        if (config.code_cache_size < 4u << 20) { // (01.01: down to 4 MiB, 8 before)\n'
      '            throw std::runtime_error(\n'
      '                fmt::format("no executable memory for the CPU JIT ({} MiB)", config.code_cache_size >> 20));\n'
      '        }\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '        if (emusuni_probe != nullptr) {\n'
      '            EmuSuNi::JitFree(emusuni_probe);\n'
      '            return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '        }\n',
      '        if (emusuni_probe != nullptr) {\n'
      '            EmuSuNi::JitFree(emusuni_probe);\n'
      '            LOG_INFO(Core_ARM11, "EmuSuNi: core {}\'s JIT: {} MiB of code cache", GetID(),\n'
      '                     config.code_cache_size >> 20); // (01.01)\n'
      '            return std::make_unique<Dynarmic::A32::Jit>(config);\n'
      '        }\n')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp',
      '        if (block_of_code.SpaceRemaining() < 8 * 1024 * 1024) {\n'
      '            return -1;\n'
      '        }\n',
      '        // (01.01: at least half of the code cache left to the game, 8 MiB at the least)\n'
      '        const std::size_t emusuni_keep = std::max<std::size_t>(8 * 1024 * 1024, conf.code_cache_size / 2);\n'
      '        if (block_of_code.SpaceRemaining() < emusuni_keep) {\n'
      '            return -1;\n'
      '        }\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      'constexpr std::size_t kMaxLocations = 200000, kMaxMarkers = 100000;\n',
      'constexpr std::size_t kMaxLocations = 200000, kMaxMarkers = 100000;\n'
      '// (01.01) Compiled ahead: the list\'s first 60,000 blocks at most (Super Smash Bros. kept 197,747: 10 s of\n'
      '// compiling over its first 23 s); the first 20,000, the boot, stay first when the list is written again.\n'
      'constexpr std::size_t kAheadBlocks = 60000, kBootBlocks = 20000;\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '// The file\'s locations then this session\'s, each once, those in the code segment (a small open-addressed set: one\n'
      '// allocation, not a node per location).\n'
      'inline std::vector<std::uint64_t> Collect(Core::ARM_Interface& core) {\n'
      '    const std::vector<std::uint64_t>& compiled = *core.EmuSuNiCompiledLocations();\n',
      r'''// (01.01) The locations the warm-up compiled ahead this session (the file's first g.next), as a set.
inline std::vector<std::uint64_t> AheadSet(std::size_t& capacity) {
    const std::size_t ahead = std::min(g.next, g.loaded.size());
    capacity = 1024;
    while (capacity < 2 * ahead) {
        capacity *= 2;
    }
    std::vector<std::uint64_t> set(capacity, 0);
    for (std::size_t i = 0; i < ahead; i++) {
        const std::uint64_t location = g.loaded[i];
        if (location == 0) {
            continue;
        }
        std::size_t slot = static_cast<std::size_t>((location * 0x9E3779B97F4A7C15ull) >> 20) & (capacity - 1);
        while (set[slot] != 0 && set[slot] != location) {
            slot = (slot + 1) & (capacity - 1);
        }
        set[slot] = location;
    }
    return set;
}

inline bool InSet(const std::vector<std::uint64_t>& set, std::size_t capacity, std::uint64_t location) {
    std::size_t slot = static_cast<std::size_t>((location * 0x9E3779B97F4A7C15ull) >> 20) & (capacity - 1);
    while (set[slot] != 0) {
        if (set[slot] == location) {
            return true;
        }
        slot = (slot + 1) & (capacity - 1);
    }
    return false;
}

// The list to keep, each location once, those in the code segment (a small open-addressed set: one allocation, not a
// node per location), in the order of last use as far as it can be told (01.01): the file's first 20,000 (the boot),
// the blocks the game compiled itself this session, the rest of those the warm-up compiled ahead (which can't tell
// whether the game ran them), then the file's others, which the game didn't meet this session (past 200,000: left out).
inline std::vector<std::uint64_t> Collect(Core::ARM_Interface& core) {
    const std::vector<std::uint64_t>& compiled = *core.EmuSuNiCompiledLocations();
    std::size_t ahead_capacity = 0;
    const std::vector<std::uint64_t> ahead_set = AheadSet(ahead_capacity);
    const std::size_t ahead = std::min(g.next, g.loaded.size());
    const std::size_t boot = std::min(ahead, kBootBlocks);
''')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '    for (const std::uint64_t location : g.loaded) {\n'
      '        add(location);\n'
      '    }\n'
      '    for (const std::uint64_t location : compiled) {\n'
      '        add(location);\n'
      '    }\n'
      '    return out;\n'
      '}\n',
      '    for (std::size_t i = 0; i < boot; i++) {\n'
      '        add(g.loaded[i]);\n'
      '    }\n'
      '    for (const std::uint64_t location : compiled) {\n'
      '        if (!InSet(ahead_set, ahead_capacity, location)) {\n'
      '            add(location); // (the game compiled it: met this session)\n'
      '        }\n'
      '    }\n'
      '    for (std::size_t i = boot; i < g.loaded.size(); i++) {\n'
      '        add(g.loaded[i]);\n'
      '    }\n'
      '    return out;\n'
      '}\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '    if (g.next < g.loaded.size()) {\n'
      '        std::shared_ptr<Kernel::Process> process;\n'
      '        Core::ARM_Interface* core = GameJit(system, process);\n',
      '    // (01.01: the list\'s first kAheadBlocks only; the rest the game compiles when it meets them)\n'
      '    const std::size_t ahead_end = std::min(g.loaded.size(), kAheadBlocks);\n'
      '    if (g.next < ahead_end && !g.no_room) {\n'
      '        std::shared_ptr<Kernel::Process> process;\n'
      '        Core::ARM_Interface* core = GameJit(system, process);\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '            while (g.next < g.loaded.size()) {\n'
      '                const std::uint64_t location = g.loaded[g.next++];\n',
      '            while (g.next < ahead_end) {\n'
      '                const std::uint64_t location = g.loaded[g.next++];\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '                    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: the code cache has no more room to spare: {} blocks "\n'
      '                             "left for the game to compile when it meets them", g.loaded.size() - g.next + 1);\n'
      '                    g.next = g.loaded.size();\n'
      '                    break;\n',
      '                    LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: the code cache has no more room to spare: {} blocks "\n'
      '                             "left for the game to compile when it meets them", g.loaded.size() - g.next + 1);\n'
      '                    g.next--; // (01.01: not compiled; g.next marks those compiled ahead)\n'
      '                    g.no_room = true;\n'
      '                    break;\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '        if (g.next >= g.loaded.size()) {\n'
      '            LOG_INFO(Frontend, "EmuSuNi: JIT warm-up: {} blocks compiled in {} ms over {} frames ({} there already)",\n'
      '                     g.compiled, g.spent_us / 1000, g.frames, g.already);\n'
      '        }\n',
      '        if (g.next >= ahead_end || g.no_room) {\n'
      '            LOG_INFO(Frontend,\n'
      '                     "EmuSuNi: JIT warm-up: {} blocks compiled in {} ms over {} frames ({} there already); "\n'
      '                     "{} more of the list left for the game to compile when it meets them (01.01: the "\n'
      '                     "first {} ahead at most)", g.compiled, g.spent_us / 1000, g.frames, g.already,\n'
      '                     g.loaded.size() - g.next, kAheadBlocks);\n'
      '            g.no_room = true; // (said once)\n'
      '        }\n')
patch('src/citra_libretro/emusuni_jitwarm.h',
      '    bool started = false, off = false;\n',
      '    bool started = false, off = false;\n'
      '    bool no_room = false; // (01.01) the warm-up done: no room to spare, or the list\'s ahead part compiled\n')
# - A diagnosis file for Smash's fighters, /data/EmuSuNi/3ds-vs-bone0 (EMUSUNI_3DS_VS_BONE0=1 on the PC): the vertex
#   shaders read every uniform through the address registers as if they were 0 (each fighter skinned by its first bone
#   only: drawn whole and stiff where the vertices reach the GPU right, still in shards where they don't). Read when a
#   vertex shader is made (no restart; the game's saved shaders are made again). Never for playing.
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '        shader.AddLine("int fixed_offset = offset >= -128 && offset <= 127 ? offset : 0;");\n',
      '#ifdef EMUSUNI_PORT\n'
      '        // (01.01) The diagnosis file /data/EmuSuNi/3ds-vs-bone0: every such read as at offset 0.\n'
      '        if (std::getenv("EMUSUNI_3DS_VS_BONE0") != nullptr ||\n'
      '            ::access("/data/EmuSuNi/3ds-vs-bone0", F_OK) == 0) {\n'
      '            static bool said = false;\n'
      '            if (!said) {\n'
      '                said = true;\n'
      '                LOG_WARNING(HW_GPU, "EmuSuNi: the file /data/EmuSuNi/3ds-vs-bone0: the vertex shaders\' "\n'
      '                                    "reads through the address registers all at offset 0 (a diagnosis: "\n'
      '                                    "models stiff)");\n'
      '            }\n'
      '            shader.AddLine("int fixed_offset = 0; // EmuSuNi: 3ds-vs-bone0");\n'
      '        } else {\n'
      '            shader.AddLine("int fixed_offset = offset >= -128 && offset <= 127 ? offset : 0;");\n'
      '        }\n'
      '#else\n'
      '        shader.AddLine("int fixed_offset = offset >= -128 && offset <= 127 ? offset : 0;");\n'
      '#endif\n')

# --- 01.02: a GPU sync diagnosis for Smash's fighters. The owner's 01.01 test (04-11): the fighters are right while
# the game is paused, and in its intro and results, and in shards in a fight. What a fight changes is the load (the
# CPU writing the next draws' data while the GPU draws the earlier ones) and the bones, new every frame. The uniforms'
# path looks right: each draw's blocks go to new room of the 8 MiB stream buffer (16-byte aligned; room comes back only
# once the submission that read it is done, and a fight goes round the buffer every ~45 frames), the dynamic offsets
# are taken by value when the draw is recorded, and the memory is host-coherent (no flush needed). So three diagnosis
# files test the sync on the PS4 in one run, each read at every game start and every 2 s (made or removed in the
# middle of a fight, it takes effect then; the log says each change, and every 600 frames what they did). Never for
# playing:
# - /data/EmuSuNi/3ds-gpu-sync-draw: the GPU finishes each draw before the next is set up (submitted and waited for,
#   then the queue idle: two waits of different kinds). Slow: a fight in Smash has ~60 draws a frame.
# - /data/EmuSuNi/3ds-gpu-sync-frame: the same at the end of each frame only.
# - /data/EmuSuNi/3ds-uniforms-fresh: every draw uploads its three uniform blocks again into new room, each at its own
#   256-byte boundary (no cache line shared with another block), and binds them there.
# - /data/EmuSuNi/3ds-vertex-reuse-off (00.95's file, read once per app run before): now also when made later. The
#   other thing written per draw that the vertex shaders read is the vertex data, which 00.95 binds again for later
#   draws of the same submission; 3ds-gpu-sync-draw stops that too (a submission a draw), so this one tells them apart.
# On the PC: EMUSUNI_3DS_GPU_DIAG naming sync-draw, sync-frame, uniforms-fresh and/or vertex-reuse-off.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#include "common/emusuni_stats.h"\n#endif\n\nnamespace Vulkan {\n',
      '#include "common/emusuni_stats.h"\n#endif\n'
      '#if defined(EMUSUNI_PORT) && defined(HAVE_LIBRETRO)\n'
      '// (01.02) citra_libretro/libretro_vk.cpp: the frontend\'s queue, taken (the GPU sync diagnosis waits for it\n'
      '// idle).\n'
      'namespace LibRetro {\n'
      'void LockQueue();\n'
      'void UnlockQueue();\n'
      '} // namespace LibRetro\n'
      '#endif\n'
      '\nnamespace Vulkan {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '\nconstexpr u64 STREAM_BUFFER_SIZE = 64_MiB;\n',
      r'''
#ifdef EMUSUNI_PORT
// (01.02) The GPU sync diagnosis (scripts/build-3ds-core.sh): which of its files are there (read at every game start
// and every 2 s), and what each did in the last 600 frames.
constexpr u32 EMUSUNI_SYNC_DRAW = 1;      // /data/EmuSuNi/3ds-gpu-sync-draw
constexpr u32 EMUSUNI_SYNC_FRAME = 2;     // /data/EmuSuNi/3ds-gpu-sync-frame
constexpr u32 EMUSUNI_UNIFORMS_FRESH = 4; // /data/EmuSuNi/3ds-uniforms-fresh
constexpr u32 EMUSUNI_REUSE_OFF = 8;      // /data/EmuSuNi/3ds-vertex-reuse-off (00.95's, made after the start too)
constexpr u32 EMUSUNI_ALIGN_ODD = 16;     // /data/EmuSuNi/3ds-vertex-align-odd (01.03: 00.95's rule only)
constexpr u32 EMUSUNI_FLOAT = 32;         // /data/EmuSuNi/3ds-vertex-float (01.03)
u32 emusuni_gpu_diag = 0;
u32 emusuni_gpu_diag_frames = 0;
u64 emusuni_synced_draws = 0;
u64 emusuni_synced_frames = 0;
u64 emusuni_fresh_uploads = 0;

void EmuSuNiReadGpuDiag(bool game_start) {
    struct File {
        u32 bit;
        const char* name; // in EMUSUNI_3DS_GPU_DIAG on the PC
        const char* path;
    };
    static constexpr std::array<File, 6> files{{
        {EMUSUNI_SYNC_DRAW, "sync-draw", "/data/EmuSuNi/3ds-gpu-sync-draw"},
        {EMUSUNI_SYNC_FRAME, "sync-frame", "/data/EmuSuNi/3ds-gpu-sync-frame"},
        {EMUSUNI_UNIFORMS_FRESH, "uniforms-fresh", "/data/EmuSuNi/3ds-uniforms-fresh"},
        {EMUSUNI_REUSE_OFF, "vertex-reuse-off", "/data/EmuSuNi/3ds-vertex-reuse-off"},
        {EMUSUNI_ALIGN_ODD, "vertex-align-odd", "/data/EmuSuNi/3ds-vertex-align-odd"},
        {EMUSUNI_FLOAT, "vertex-float", "/data/EmuSuNi/3ds-vertex-float"},
    }};
    const char* env = std::getenv("EMUSUNI_3DS_GPU_DIAG");
    u32 now = 0;
    for (const File& file : files) {
        if ((env != nullptr && std::strstr(env, file.name) != nullptr) || ::access(file.path, F_OK) == 0) {
            now |= file.bit;
        }
    }
    if (now != emusuni_gpu_diag || (game_start && now != 0)) {
        std::string said;
        for (const File& file : files) {
            said += fmt::format("{}{} {}", said.empty() ? "" : ", ", file.name, (now & file.bit) != 0 ? "ON" : "off");
        }
        LOG_WARNING(Render_Vulkan,
                    "EmuSuNi: GPU sync diagnosis {} (01.02): {} (the files /data/EmuSuNi/3ds-gpu-sync-draw, "
                    "3ds-gpu-sync-frame, 3ds-uniforms-fresh, 3ds-vertex-reuse-off, 3ds-vertex-align-odd and "
                    "3ds-vertex-float; never for playing)",
                    game_start ? "at the game's start" : "changed", said);
    }
    emusuni_gpu_diag = now;
}

// (01.02) The GPU done with all the work so far: submitted and waited for (the scheduler's fences), then the queue
// idle (the frontend's, under its lock): two waits of different kinds, should one of them end early.
void EmuSuNiSyncGpu(Scheduler& scheduler, const Instance& instance) {
    scheduler.Finish();
#ifdef HAVE_LIBRETRO
    LibRetro::LockQueue();
#endif
    instance.GetDevice().waitIdle();
#ifdef HAVE_LIBRETRO
    LibRetro::UnlockQueue();
#endif
}

// (01.03) One attribute of a loader moved to its new offset: its bytes copied, or with kind (3ds-vertex-float: its
// PICA format | its components << 4) its values as 32-bit floats, as the 3DS reads them (no normalization).
inline void EmuSuNiPutAttribute(u8* dst, const u8* src, u32 bytes, u32 kind) {
    if (kind == 0) {
        EmuSuNiCopyAttribute(dst, src, bytes);
        return;
    }
    const u32 components = kind >> 4;
    float values[4];
    for (u32 c = 0; c < components; c++) {
        switch (kind & 0xF) {
        case 0: // BYTE
            values[c] = static_cast<float>(static_cast<s8>(src[c]));
            break;
        case 1: // UBYTE
            values[c] = static_cast<float>(src[c]);
            break;
        case 2: { // SHORT
            s16 value;
            std::memcpy(&value, src + 2 * c, 2);
            values[c] = static_cast<float>(value);
            break;
        }
        default: // FLOAT
            std::memcpy(&values[c], src + 4 * c, 4);
            break;
        }
    }
    std::memcpy(dst, values, 4 * components);
}
#endif

constexpr u64 STREAM_BUFFER_SIZE = 64_MiB;
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    scheduler.RegisterOnSubmit([&renderpass_cache] { renderpass_cache.EndRendering(); });\n',
      '    scheduler.RegisterOnSubmit([&renderpass_cache] { renderpass_cache.EndRendering(); });\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game\'s start\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n',
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n'
      '    // (01.02) The GPU sync diagnosis files read again every 2 s; 3ds-gpu-sync-frame: the GPU idle at the end\n'
      '    // of each frame.\n'
      '    if (++emusuni_gpu_diag_frames >= 120) {\n'
      '        emusuni_gpu_diag_frames = 0;\n'
      '        EmuSuNiReadGpuDiag(false);\n'
      '    }\n'
      '    if ((emusuni_gpu_diag & EMUSUNI_SYNC_FRAME) != 0) {\n'
      '        EmuSuNiSyncGpu(scheduler, instance);\n'
      '        emusuni_synced_frames++;\n'
      '    }\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        emusuni_rebased_draws = 0; // (00.98)\n',
      '        if (emusuni_synced_draws + emusuni_synced_frames + emusuni_fresh_uploads != 0) {\n'
      '            LOG_INFO(Render_Vulkan,\n'
      '                     "EmuSuNi: GPU sync diagnosis in the last 600 frames: the GPU finished {} draws and {} "\n'
      '                     "frames one by one, {} draws\' uniforms uploaded fresh (01.02)",\n'
      '                     emusuni_synced_draws, emusuni_synced_frames, emusuni_fresh_uploads);\n'
      '        }\n'
      '        emusuni_synced_draws = 0; // (01.02)\n'
      '        emusuni_synced_frames = 0;\n'
      '        emusuni_fresh_uploads = 0;\n'
      '        emusuni_rebased_draws = 0; // (00.98)\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    vertex_batch.clear();\n    return succeeded;\n}\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // (01.02) 3ds-gpu-sync-draw: the GPU done with this draw before the next is set up.\n'
      '    if ((emusuni_gpu_diag & EMUSUNI_SYNC_DRAW) != 0) {\n'
      '        EmuSuNiSyncGpu(scheduler, instance);\n'
      '        emusuni_synced_draws++;\n'
      '    }\n'
      '#endif\n'
      '    vertex_batch.clear();\n    return succeeded;\n}\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::UploadUniforms(bool accelerate_draw) {\n'
      '    const bool sync_vs_pica = accelerate_draw && pica.vs_setup.uniforms_dirty;\n',
      r'''void RasterizerVulkan::UploadUniforms(bool accelerate_draw) {
#ifdef EMUSUNI_PORT
    if ((emusuni_gpu_diag & EMUSUNI_UNIFORMS_FRESH) != 0) {
        // (01.02) 3ds-uniforms-fresh: the three blocks again into new room, each at its own 256-byte boundary.
        constexpr u32 align = 256;
        const u32 vs_size = Common::AlignUp(static_cast<u32>(sizeof(vs_data)), align);
        const u32 fs_size = Common::AlignUp(static_cast<u32>(sizeof(fs_data)), align);
        const u32 pica_size = Common::AlignUp(static_cast<u32>(sizeof(VSPicaUniformData)), align);
        const auto [fresh, fresh_offset, fresh_wrapped] = uniform_buffer.Map(vs_size + fs_size + pica_size, align);
        std::memcpy(fresh, &vs_data, sizeof(vs_data));
        pipeline_cache.UpdateRange(1, fresh_offset);
        std::memcpy(fresh + vs_size, &fs_data, sizeof(fs_data));
        pipeline_cache.UpdateRange(2, fresh_offset + vs_size);
        VSPicaUniformData vs_uniforms;
        vs_uniforms.SetFromRegs(pica.vs_setup);
        std::memcpy(fresh + vs_size + fs_size, &vs_uniforms, sizeof(vs_uniforms));
        pipeline_cache.UpdateRange(0, fresh_offset + vs_size + fs_size);
        uniform_buffer.Commit(vs_size + fs_size + pica_size);
        vs_data_dirty = false;
        fs_data_dirty = false;
        pica.vs_setup.uniforms_dirty = false;
        emusuni_fresh_uploads++;
        (void)accelerate_draw;
        (void)fresh_wrapped;
        return;
    }
#endif
    const bool sync_vs_pica = accelerate_draw && pica.vs_setup.uniforms_dirty;
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        const bool emusuni_reuse = !emusuni_compact && EmuSuNiFindUpload(emusuni_this, emusuni_reused_offset);\n',
      '        // (01.02: not while the file /data/EmuSuNi/3ds-vertex-reuse-off is there, made after the start too)\n'
      '        const bool emusuni_reuse = !emusuni_compact && (emusuni_gpu_diag & EMUSUNI_REUSE_OFF) == 0 &&\n'
      '                                   EmuSuNiFindUpload(emusuni_this, emusuni_reused_offset);\n')

# --- 01.03: the vertex fetch diagnosis for Smash's fighters, and 3ds-vs-bone0 made to work. The owner's 01.02 test
# (05-11): nothing fixed the fighters (3ds-gpu-sync-frame, -sync-draw, 3ds-uniforms-fresh), so it's not sync. And
# 3ds-vs-bone0 never ran: the vertex shaders came from the game's disk shader cache, which links each saved shader to
# its saved SPIR-V without translating it again (only a new version of the shader generator's sources, a build that
# changes them, makes the cache again: 01.00 and 01.01 did), and the log has neither its warning nor a shader made. The
# fighters' vertices differ from the items' and nearly all the stage's (right): positions as 3 signed shorts in a
# stride of 18 bytes (R16G16B16_SSCALED at 0, then 3 signed bytes at 6, 2 shorts at 10, 2 bytes at 14 and 16), so
# every other vertex's position is at an address 4 doesn't divide; the items' are floats in strides 4 divides. RADV
# loads a 3-component 16-bit attribute as a 16_16 typed load and a 16 one, and on GFX7 never splits a typed load for
# its alignment (only on GFX6 and GFX10+: it counts on Linux's "unaligned" memory mode); on the PS4, Sony's system sets
# the GPU up. 00.95 moves only attributes at odd addresses. So (the master's call: one test session, the fix shown by
# a plain run), and the files read at every game start and every 2 s like 01.02's:
# - Every non-float attribute load now moved to an offset its alignment divides (4 for a 16_16 or 8_8_8_8 load, 2 for
#   8_8, 1 for 8), in a stride 4 divides: the fighters' stride 18 becomes 20; same formats, the same copy as 00.95's
#   (playing since then), only more loaders. Not the floats: untyped loads, right at any address on the PS4 (Kirby:
#   Planet Robobot's at addresses 4 doesn't divide). The file /data/EmuSuNi/3ds-vertex-align-odd: only the odd ones
#   moved, as before.
# - /data/EmuSuNi/3ds-vertex-float: every loader with a non-float attribute copied with all its attributes as 32-bit
#   floats (the values the 3DS reads, no normalization), 4-byte aligned: no compact format nor odd address left for the
#   GPU to load. Costs CPU (and 00.96's compaction is off with it).
# - /data/EmuSuNi/3ds-vs-bone0 (01.01) now makes the game's shader caches again when it comes and again when it goes
#   (a version of their own while it's there), read as the game starts: make it or remove it outside the game.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '} // namespace LibRetro\n#endif\n\nnamespace Vulkan {\n',
      '} // namespace LibRetro\n#endif\n'
      '#ifdef EMUSUNI_PORT\n'
      'namespace EmuSuNi {\n'
      '// (01.03) The diagnosis file /data/EmuSuNi/3ds-vs-bone0 as this game started (RasterizerVulkan): the shader\n'
      '// caches\' version (vk_shader_disk_cache.cpp) and the vertex shaders (glsl_shader_decompiler.cpp) follow it.\n'
      'bool vs_bone0 = false;\n'
      '} // namespace EmuSuNi\n'
      '#endif\n'
      '\nnamespace Vulkan {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game\'s start\n',
      '    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game\'s start\n'
      '    // (01.03) 3ds-vs-bone0 as the game starts (before its shader caches open).\n'
      '    ::EmuSuNi::vs_bone0 = std::getenv("EMUSUNI_3DS_VS_BONE0") != nullptr ||\n'
      '                          ::access("/data/EmuSuNi/3ds-vs-bone0", F_OK) == 0;\n'
      '    if (::EmuSuNi::vs_bone0) {\n'
      '        LOG_WARNING(Render_Vulkan,\n'
      '                    "EmuSuNi: the file /data/EmuSuNi/3ds-vs-bone0 (01.03): every address register read at "\n'
      '                    "offset 0 (a diagnosis: models stiff); the shader caches made again for it, and again "\n'
      '                    "once it is gone");\n'
      '    }\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: vertex attributes (01.03): {}",\n'
      '             (emusuni_gpu_diag & EMUSUNI_ALIGN_ODD) != 0\n'
      '                 ? "only the loads at odd offsets moved, as 00.95 (the file "\n'
      '                   "/data/EmuSuNi/3ds-vertex-align-odd)"\n'
      '                 : "every non-float load moved to an offset its size divides (the file "\n'
      '                   "/data/EmuSuNi/3ds-vertex-align-odd: only the odd ones, as 00.95)");\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      '#include "video_core/shader/generator/glsl_shader_gen.h"\n',
      '#include "video_core/shader/generator/glsl_shader_gen.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'namespace EmuSuNi {\n'
      'extern bool vs_bone0; // (01.03) renderer_vulkan/vk_rasterizer.cpp\n'
      '} // namespace EmuSuNi\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_shader_disk_cache.cpp',
      '    std::memcpy(hash.data(), Common::g_shader_cache_version, length);\n    return hash;\n',
      '    std::memcpy(hash.data(), Common::g_shader_cache_version, length);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (01.03) The diagnosis 3ds-vs-bone0 changes the vertex shaders: while its file is there the caches\n'
      '    // have a version of their own, so they are made again when it comes and when it goes (never a mix).\n'
      '    if (::EmuSuNi::vs_bone0) {\n'
      '        hash[hash.size() - 1] ^= 0xB0;\n'
      '        hash[hash.size() - 2] ^= 0x4E;\n'
      '    }\n'
      '#endif\n'
      '    return hash;\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '#include "video_core/shader/generator/glsl_shader_decompiler.h"\n',
      '#include "video_core/shader/generator/glsl_shader_decompiler.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      'namespace EmuSuNi {\n'
      'extern bool vs_bone0; // (01.03) renderer_vulkan/vk_rasterizer.cpp\n'
      '} // namespace EmuSuNi\n'
      '#endif\n')
patch('src/video_core/shader/generator/glsl_shader_decompiler.cpp',
      '        if (std::getenv("EMUSUNI_3DS_VS_BONE0") != nullptr ||\n'
      '            ::access("/data/EmuSuNi/3ds-vs-bone0", F_OK) == 0) {\n',
      '        if (::EmuSuNi::vs_bone0) { // (01.03: as the game started, with the shader caches\' version)\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        std::array<std::array<u32, 4>, 12> emusuni_attrs;\n',
      '        std::array<std::array<u32, 4>, 12> emusuni_attrs;\n'
      '        std::array<u32, 12> emusuni_kind{}; // (01.03) each one\'s PICA format | its components << 4\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '            emusuni_attrs[emusuni_count++] = {input_reg, offset, vertex_attributes.GetStride(attribute_index),\n'
      '                                              emusuni_align};\n',
      '            emusuni_attrs[emusuni_count++] = {input_reg, offset, vertex_attributes.GetStride(attribute_index),\n'
      '                                              emusuni_align};\n'
      '            // (01.03) Every non-float load at an offset (and in a stride) its alignment divides, not only the\n'
      '            // odd ones (the file 3ds-vertex-align-odd: only those, as 00.95); 3ds-vertex-float: every loader\n'
      '            // with a non-float attribute, as floats.\n'
      '            const auto emusuni_format = vertex_attributes.GetFormat(attribute_index);\n'
      '            emusuni_kind[emusuni_count - 1] = static_cast<u32>(emusuni_format) | static_cast<u32>(size) << 4;\n'
      '            const u32 emusuni_stride = Common::AlignUp(static_cast<u32>(loader.byte_count), stride_alignment);\n'
      '            if ((emusuni_gpu_diag & EMUSUNI_ALIGN_ODD) == 0 &&\n'
      '                emusuni_format != Pica::PipelineRegs::VertexAttributeFormat::FLOAT && emusuni_align >= 2 &&\n'
      '                ((offset | emusuni_stride) % emusuni_align) != 0) {\n'
      '                emusuni_misaligned = true;\n'
      '            }\n'
      '            if ((emusuni_gpu_diag & EMUSUNI_FLOAT) != 0 &&\n'
      '                emusuni_format != Pica::PipelineRegs::VertexAttributeFormat::FLOAT) {\n'
      '                emusuni_misaligned = true;\n'
      '            }\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        const bool emusuni_move = emusuni_misaligned && EmuSuNiAlignVertices();\n',
      '        const bool emusuni_move = emusuni_misaligned && EmuSuNiAlignVertices();\n'
      '        // (01.03) 3ds-vertex-float: the moved attributes as floats.\n'
      '        const bool emusuni_float = emusuni_move && (emusuni_gpu_diag & EMUSUNI_FLOAT) != 0;\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        u64 emusuni_shape = emusuni_move ? 1 : 0;\n',
      '        u64 emusuni_shape = emusuni_move ? (emusuni_float ? 3 : 1) : 0; // (01.03: 3 as floats)\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '            for (u32 i = 0; i < emusuni_count; i++) {\n'
      '                cursor = Common::AlignUp(cursor, emusuni_attrs[i][3]);\n'
      '                new_offsets[i] = cursor;\n'
      '                layout.attributes[emusuni_attrs[i][0]].offset.Assign(cursor);\n'
      '                cursor += emusuni_attrs[i][2];\n',
      '            for (u32 i = 0; i < emusuni_count; i++) {\n'
      '                // (01.03: as floats, 4 bytes a component at an offset 4 divides)\n'
      '                cursor = Common::AlignUp(cursor, emusuni_float ? 4u : emusuni_attrs[i][3]);\n'
      '                new_offsets[i] = cursor;\n'
      '                layout.attributes[emusuni_attrs[i][0]].offset.Assign(cursor);\n'
      '                if (emusuni_float) {\n'
      '                    layout.attributes[emusuni_attrs[i][0]].type.Assign(\n'
      '                        Pica::PipelineRegs::VertexAttributeFormat::FLOAT);\n'
      '                }\n'
      '                cursor += emusuni_float ? 4 * (emusuni_kind[i] >> 4) : emusuni_attrs[i][2];\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                        EmuSuNiCopyAttribute(dst_ptr + first * new_stride + new_offsets[i],\n'
      '                                             src_ptr + first * loader.byte_count + emusuni_attrs[i][1],\n'
      '                                             emusuni_attrs[i][2]);\n',
      '                        EmuSuNiPutAttribute(dst_ptr + first * new_stride + new_offsets[i],\n'
      '                                            src_ptr + first * loader.byte_count + emusuni_attrs[i][1],\n'
      '                                            emusuni_attrs[i][2], emusuni_float ? emusuni_kind[i] : 0);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                        EmuSuNiCopyAttribute(dst + new_offsets[i], src + emusuni_attrs[i][1], emusuni_attrs[i][2]);\n',
      '                        EmuSuNiPutAttribute(dst + new_offsets[i], src + emusuni_attrs[i][1],\n'
      '                                            emusuni_attrs[i][2], emusuni_float ? emusuni_kind[i] : 0);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    if (emusuni_draw_indexed && EmuSuNiCompactVertices()) {\n',
      '    if (emusuni_draw_indexed && EmuSuNiCompactVertices() &&\n'
      '        (emusuni_gpu_diag & EMUSUNI_FLOAT) == 0) { // (01.03: not with 3ds-vertex-float)\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                emusuni_room += (3 * static_cast<u32>(loader.component_count) + 3) * vertices + 4;\n',
      '                // (01.03: 3ds-vertex-float makes an attribute up to 12 bytes bigger, and 3 of alignment)\n'
      '                emusuni_room += ((emusuni_gpu_diag & EMUSUNI_FLOAT) != 0 ? 15 : 3) *\n'
      '                                    static_cast<u32>(loader.component_count) * vertices +\n'
      '                                3 * vertices + 4;\n')

# --- 01.04: a directory made without the trailing '/', tried again, and said in full when it fails. v00.23's reports
# H4F3TQ and FA7NXC (one PS4, system 13.09, its first 3DS game): mkdir of ".../nand/data/<id>/sysdata/" failed with
# EINVAL, and the CECD service stopped the game there (an assert on its system save data, SIGILL), at every start. The
# same run had made every directory above it (Azahar, sdmc, nand/rw, nand/ro, nand/data/<id>), each with the '/' that
# CreateFullPath gives: the '/' alone doesn't explain it (the first directory nine deep did fail). Now on the PS4:
# - the path without its trailing '/' (IsDirectory and Exists never stat with it either);
# - a passing error (EINVAL, EAGAIN, EINTR, EIO) tried again 3 times, 20 ms apart, and a directory that is there
#   afterwards taken as made (another thread's, or the kernel's late answer); a retry that helped is logged;
# - a refusal that stays EINVAL: one more way, made under another name and renamed to its own;
# - a failure logged with errno, the path's length and its parent's state, for the next report.
patch('src/common/file_util.cpp',
      '#include <cerrno>\n',
      '#include <cerrno>\n'
      '#ifdef EMUSUNI_PORT\n#include <algorithm>\n#include <sys/stat.h>\n#include <time.h>\n#endif\n')
patch('src/common/file_util.cpp',
      '#else\n'
      '    if (mkdir(path.c_str(), 0755) == 0)\n'
      '        return true;\n'
      '\n'
      '    int err = errno;\n'
      '\n'
      '    if (err == EEXIST) {\n'
      '        LOG_DEBUG(Common_Filesystem, "mkdir failed on {}: already exists", path);\n'
      '        return true;\n'
      '    }\n'
      '\n'
      '    LOG_ERROR(Common_Filesystem, "mkdir failed on {}: {}", path, strerror(err));\n'
      '    return false;\n'
      '#endif\n',
      '#elif defined(EMUSUNI_PORT)\n'
      '    // EmuSuNi (01.04): without the trailing \'/\', tried again on a passing error, and said in full (a PS4 on\n'
      '    // system 13.09 refused ".../nand/data/<id>/sysdata/" with EINVAL: v00.23\'s reports H4F3TQ and FA7NXC).\n'
      '    std::string emusuni_dir = path;\n'
      '    while (emusuni_dir.size() > 1 && emusuni_dir.back() == \'/\')\n'
      '        emusuni_dir.pop_back();\n'
      '    int err = 0;\n'
      '    for (int attempt = 0; attempt < 4; attempt++) {\n'
      '        if (mkdir(emusuni_dir.c_str(), 0755) == 0) {\n'
      '            if (attempt > 0)\n'
      '                LOG_WARNING(Common_Filesystem, "EmuSuNi: mkdir made {} at try {} (errno {} before)", path,\n'
      '                            attempt + 1, err);\n'
      '            return true;\n'
      '        }\n'
      '        err = errno;\n'
      '        if (err == EEXIST || IsDirectory(emusuni_dir))\n'
      '            return true;\n'
      '        if (err != EINVAL && err != EAGAIN && err != EINTR && err != EIO)\n'
      '            break;\n'
      '        struct timespec emusuni_wait{0, 20 * 1000 * 1000};\n'
      '        nanosleep(&emusuni_wait, nullptr);\n'
      '    }\n'
      '    if (err == EINVAL) {\n'
      '        // (A last way: made under another name, then renamed to its own; the other one taken away again.)\n'
      '        const std::string emusuni_other = emusuni_dir + ".emusuni-new";\n'
      '        if (mkdir(emusuni_other.c_str(), 0755) == 0 || errno == EEXIST) {\n'
      '            if (rename(emusuni_other.c_str(), emusuni_dir.c_str()) == 0) {\n'
      '                LOG_WARNING(Common_Filesystem, "EmuSuNi: mkdir made {} by a rename (EINVAL to mkdir)", path);\n'
      '                return true;\n'
      '            }\n'
      '            rmdir(emusuni_other.c_str());\n'
      '        }\n'
      '    }\n'
      '    const std::string emusuni_parent{GetParentPath(emusuni_dir)};\n'
      '    struct stat emusuni_stat{};\n'
      '    const int emusuni_parent_stat = stat(emusuni_parent.c_str(), &emusuni_stat);\n'
      '    LOG_ERROR(Common_Filesystem,\n'
      '              "mkdir failed on {}: {} (errno {}; {} characters, {} parts; its parent: {})", path,\n'
      '              strerror(err), err, emusuni_dir.size(),\n'
      '              std::count(emusuni_dir.begin(), emusuni_dir.end(), \'/\'),\n'
      '              emusuni_parent_stat != 0       ? "missing"\n'
      '              : S_ISDIR(emusuni_stat.st_mode) ? "a directory"\n'
      '                                              : "not a directory");\n'
      '    return false;\n'
      '#else\n'
      '    if (mkdir(path.c_str(), 0755) == 0)\n'
      '        return true;\n'
      '\n'
      '    int err = errno;\n'
      '\n'
      '    if (err == EEXIST) {\n'
      '        LOG_DEBUG(Common_Filesystem, "mkdir failed on {}: already exists", path);\n'
      '        return true;\n'
      '    }\n'
      '\n'
      '    LOG_ERROR(Common_Filesystem, "mkdir failed on {}: {}", path, strerror(err));\n'
      '    return false;\n'
      '#endif\n')

# --- 01.04: three more of v00.23's 3DS crashes (an assert, SIGILL):
# - A save state of the other 3DS (reports UHA4KY, Pokémon Moon modified, and ZWE4NA, Project X Zone 2: Continue's
#   state, then one loaded from the menu). Its load builds the system again with the state's memory mode and cores,
#   but MemoryInit lays them out on an FCRAM of New 3DS Mode's size and asserted when the two differed - New 3DS Mode
#   changed since the state was made (a change applies at the game's next start: Continue's state is then the other
#   3DS's). Now the state is loaded in its own 3DS (a warning in the log); the next game start takes the setting again.
# - A shadow texture whose surface isn't RGBA8 (report BQ5RWS, Captain Toad: Treasure Tracker, ~30 s in). Its view
#   for the shaders' image reads is RGBA8's (R32Uint), and upstream's ImageView asserted on any other format (on the
#   PS4 a D32FS8 surface stands in for every D24S8 one, and some reinterpretations to and from it are missing). Now
#   that draw gets the null surface's view (no shadow on it) and the log says so, three times at most.
# - A memory mode that doesn't fill this 3DS's FCRAM at a game's start (report HBDCG5, Donkey Kong Country Returns 3D
#   "-decrypted", title 00040000000ccf00, both tries; the owner's USA copy, cce00, has mode 0): only an exheader's Old
#   3DS mode of 1 (unused), 8 to 15 or 6-7 on an Old 3DS gets there, and MemoryInit asserted. Now such a mode gets this
#   3DS's default one (Prod, NewProd) and the log names the mode the game asked for. On a New 3DS (NewProd) the game's
#   Old 3DS mode only sets the memory it's told of: 1 told it none and 8 to 15 one read past the table (the game broke
#   at its start); now they tell it Prod's.
patch('src/core/hle/kernel/memory.cpp',
      'void KernelSystem::MemoryInit(MemoryMode memory_mode, u64 override_init_time) {\n'
      '    const bool is_new_3ds = Settings::values.is_new_3ds.GetValue();\n',
      'void KernelSystem::MemoryInit(MemoryMode memory_mode, u64 override_init_time) {\n'
      '    const bool is_new_3ds = Settings::values.is_new_3ds.GetValue();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.04): a mode whose regions don\'t fill this 3DS\'s FCRAM (the unused 1, 8 to 15, a New\n'
      '    // 3DS one on an Old 3DS) gets this 3DS\'s default one, in the log: the assert below ended the app\n'
      '    // (v00.23\'s report HBDCG5, at a game\'s start).\n'
      '    {\n'
      '        const u32 emusuni_index = static_cast<u32>(memory_mode);\n'
      '        u32 emusuni_total = 0;\n'
      '        if (emusuni_index < 8) {\n'
      '            for (int i = 0; i < 3; ++i)\n'
      '                emusuni_total += memory_region_sizes[emusuni_index][i];\n'
      '        }\n'
      '        const u32 emusuni_fcram = is_new_3ds ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE;\n'
      '        if (emusuni_total != emusuni_fcram) {\n'
      '            const MemoryMode emusuni_default = is_new_3ds ? MemoryMode::NewProd : MemoryMode::Prod;\n'
      '            LOG_ERROR(Kernel,\n'
      '                      "EmuSuNi: memory mode {} doesn\'t fit an {} 3DS ({} of {} KiB of FCRAM): "\n'
      '                      "mode {} instead",\n'
      '                      emusuni_index, is_new_3ds ? "New" : "Old", emusuni_total / 1024, emusuni_fcram / 1024,\n'
      '                      static_cast<u32>(emusuni_default));\n'
      '            memory_mode = emusuni_default;\n'
      '            this->memory_mode = emusuni_default;\n'
      '        }\n'
      '    }\n'
      '#endif\n')
patch('src/core/hle/kernel/memory.cpp',
      '        const u32 mem_type_index = static_cast<u32>(memory_mode);\n'
      '        auto& config_mem = config_mem_handler->GetConfigMem();\n',
      '        const u32 mem_type_index = static_cast<u32>(memory_mode);\n'
      '        auto& config_mem = config_mem_handler->GetConfigMem();\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (01.04): an Old 3DS mode with no layout of its own (the unused 1, 8 to 15: an odd\n'
      '        // exheader, v00.23\'s report HBDCG5) is told Prod\'s memory: it was told none (1) or a size read\n'
      '        // past the table (8 to 15), and the game broke at its start with New 3DS Mode on.\n'
      '        if (mem_type_index == 1 || mem_type_index >= 8) {\n'
      '            LOG_ERROR(Kernel,\n'
      '                      "EmuSuNi: memory mode {} has no Old 3DS layout: the game is told mode 0\'s memory",\n'
      '                      mem_type_index);\n'
      '            config_mem.app_mem_alloc = memory_region_sizes[0][0];\n'
      '            return;\n'
      '        }\n'
      '#endif\n')
patch('src/core/core.cpp',
      '    ar & mem_mode;\n'
      '\n'
      '    if (Archive::is_loading::value) {\n'
      '        // When loading, we want to make sure any lingering state gets cleared out before we begin.\n',
      '    ar & mem_mode;\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.04): a state of the other 3DS (New 3DS Mode changed since) loaded as that 3DS:\n'
      '    // MemoryInit lays the state\'s regions out on an FCRAM of the setting\'s size, and asserted\n'
      '    // (v00.23\'s reports UHA4KY and ZWE4NA).\n'
      '    if (Archive::is_loading::value) {\n'
      '        const bool emusuni_state_new =\n'
      '            static_cast<u32>(mem_mode) >= static_cast<u32>(Kernel::MemoryMode::NewProd);\n'
      '        if (emusuni_state_new != Settings::values.is_new_3ds.GetValue()) {\n'
      '            LOG_WARNING(Core,\n'
      '                        "EmuSuNi: the save state is {}\'s (New 3DS Mode is {}): loaded as one ({} cores)",\n'
      '                        emusuni_state_new ? "a New 3DS" : "an Old 3DS",\n'
      '                        Settings::values.is_new_3ds.GetValue() ? "on" : "off", num_cores);\n'
      '            Settings::values.is_new_3ds = emusuni_state_new;\n'
      '        }\n'
      '    }\n'
      '#endif\n'
      '\n'
      '    if (Archive::is_loading::value) {\n'
      '        // When loading, we want to make sure any lingering state gets cleared out before we begin.\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {\n'
      '    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;\n'
      '    using CubeFace = Pica::TexturingRegs::CubeFace;\n'
      '\n'
      '    const auto pica_textures = regs.texturing.GetTextures();\n'
      '    emusuni_write_count = 0;\n',
      '// EmuSuNi (01.04): a shadow texture\'s view for the shaders\' image reads (RGBA8\'s, as R32Uint); the\n'
      '// null surface\'s when its surface is of another format (upstream\'s ImageView asserted: Captain\n'
      '// Toad, v00.23\'s report BQ5RWS).\n'
      'static vk::ImageView EmuSuNiShadowView(Surface& surface, Surface& null_surface) {\n'
      '    if (surface.traits.native == vk::Format::eR8G8B8A8Unorm) {\n'
      '        return surface.StorageView();\n'
      '    }\n'
      '    static u32 told = 0;\n'
      '    if (told < 3) {\n'
      '        told++;\n'
      '        LOG_WARNING(Render_Vulkan, "EmuSuNi: a shadow texture on a {} surface (not RGBA8): drawn without it",\n'
      '                    vk::to_string(surface.traits.native));\n'
      '    }\n'
      '    return null_surface.StorageView();\n'
      '}\n'
      '\n'
      'void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {\n'
      '    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;\n'
      '    using CubeFace = Pica::TexturingRegs::CubeFace;\n'
      '\n'
      '    const auto pica_textures = regs.texturing.GetTextures();\n'
      '    emusuni_write_count = 0;\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, surface.StorageView(), sampler.Handle(), false);\n',
      '                Surface& emusuni_null = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);\n'
      '                EmuSuNiAddImage(static_cast<u8>(texture_index), 0, EmuSuNiShadowView(surface, emusuni_null),\n'
      '                                sampler.Handle(), false);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                    EmuSuNiAddImage(0, static_cast<u8>(binding), surface.StorageView(), sampler.Handle(), false);\n',
      '                    Surface& emusuni_null = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);\n'
      '                    EmuSuNiAddImage(0, static_cast<u8>(binding), EmuSuNiShadowView(surface, emusuni_null),\n'
      '                                    sampler.Handle(), false);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        EmuSuNiAddImage(0, 0, shadow_surface.StorageView(), {}, true);\n',
      '        Surface& emusuni_null = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);\n'
      '        EmuSuNiAddImage(0, 0, EmuSuNiShadowView(shadow_surface, emusuni_null), {}, true);\n')

# --- 01.04: 3DS online. The owner's 01.03 test (05-11) fixed Smash; then the 3DS's local wireless play over the
# internet, as the owner approved it (free, off by default, no address in any log): Azahar's rooms (src/network),
# which the core compiled but never started ("NWM_UDS: Network isn't initalized" at every game's start). The app's 3DS
# Online gives a room's server as the core option citra_emusuni_room ("<IPv4 address>[:port]"; "disabled") and a
# nickname as citra_emusuni_room_nickname ("auto", or the app's); citra_libretro/emusuni_room.h makes the room member
# before the game's load (the NWM service binds to it when the load makes the service), joins on a thread of its own
# after the load, joins again when the connection drops, shows the changes on the screen and logs counters, never an
# address. The room's server: scripts/build-3ds-room.sh. Also:
# - A test without the app's option: the file /data/EmuSuNi/3ds-room.txt, "<IPv4 address>[:port]" on its first line
#   (EMUSUNI_3DS_ROOM on the PC): an Azahar for PC's own room on the PS4's network (Multiplayer, Host Room), say.
# - ENet on the PS4 (OpenOrbis's headers give Linux's values; the kernel is FreeBSD's): MSG_NOSIGNAL is 0x4000, its
#   MSG_NBIO, on every send and receive (UDP: no SIGPIPE to keep away), and MSG_TRUNC 0x20, its MSG_CTRUNC: none and
#   0x10 now (checked by preprocessing unix.c with the PS4's flags).
# - The room member: its thread named "Room" (the app places it), the room's address used as it is (musl's lookup
#   asks no DNS server on the PS4), no assert when its socket can't be made or when the connection drops under a
#   packet (an assert ends the app), at most 1 s for the server's goodbye (5 s held the game's end), and the local
#   wireless packets it sends counted for the log.
# - The NWM service: what another console's packets made it assert on (an association refused, a join to a full
#   network, a management frame, a beacon's odd tags or sizes) is logged and dropped: in a room, another player's
#   packet must never end the app.
addfile('src/citra_libretro/emusuni_room.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.04): 3DS online - the
// 3DS's local wireless play (UDS: the games' "Local Play"; Download Play needs the player's keys)
// through an Azahar room on a server (scripts/build-3ds-room.sh), as
// Azahar's own rooms (src/network): every console in the room gets the others' beacons and data, whatever network
// it is on, and nobody gets anybody's address. The app's 3DS Online (off by default) gives the room as the core
// option citra_emusuni_room ("disabled", or "<IPv4 address>[:port]"; a name too on the PC) and the nickname as
// citra_emusuni_room_nickname ("auto": "3DS-" and four characters of the console ID's hash; or the app's). For a test
// without the option: the file /data/EmuSuNi/3ds-room.txt with the room on its first line (EMUSUNI_3DS_ROOM on the PC).
//   Prepare(), before the game's load: Network::Init (the NWM service binds to the room member when the load makes
//     the service: a member made later, and the game would never hear the room).
//   Start(), after it: a thread of its own, "Room join", joins the room (Join waits up to 5 s for the server; then
//     the room answers), and joins again 30 s after the connection drops (the room forgets a console that went
//     silent within 30 s); one that refuses for good (another version, a password, banned) is left alone.
//   Tick(), every frame on the emulation thread (one test when it's off): the screen's messages and, every 10 s if
//     anything changed, a log line: the local wireless packets each way and the players in the room.
//   Stop(), after the game's shutdown: the room left (1 s at most for the server's goodbye), the network shut.
// Nothing here logs an address: not the server's, not the players' (the room never gives them).
#pragma once
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <fmt/format.h>
#include "citra_libretro/environment.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/hle/service/cfg/cfg.h"
#include "network/network.h"
#include "network/room_member.h"

namespace EmuSuNi::Room {

using Clock = std::chrono::steady_clock;
using Member = Network::RoomMember;

struct Session {
    // The emulation thread's (Prepare, Start, Stop).
    bool on = false;      // a room given at this game's start
    bool network = false; // Network::Init done
    std::string host;     // the room's server: never logged
    u16 port = Network::DefaultRoomPort;
    std::string nickname_option;
    std::shared_ptr<Member> member;
    Member::CallbackHandle<Member::State> on_state;
    Member::CallbackHandle<Member::Error> on_error;
    Member::CallbackHandle<Network::WifiPacket> on_packet;
    Member::CallbackHandle<Network::RoomInformation> on_info;
    std::thread joiner;
    std::mutex lock; // stop, and the joiner's waits
    std::condition_variable wake;
    bool stop = false;
    // The room member's thread (its callbacks) and the joiner's.
    std::atomic<int> error{-1};
    std::atomic<u32> players{0}, packets_in{0}, beacons_in{0};
    std::atomic<u64> bytes_in{0};
    std::atomic<s64> in_room_ms{0}; // the connections that ended
    std::mutex message_lock;
    std::string message; // the next message for the screen (Tick shows it)
    // The emulation thread's (Tick).
    Clock::time_point last_log;
    u32 frames = 0, logged_out = 0, logged_in = 0, logged_beacons = 0, logged_players = 0, base_out = 0, base_in = 0;
    u64 logged_bytes_out = 0, logged_bytes_in = 0;
};

// (Never destroyed: its thread may still run when the app ends.)
inline Session& S() {
    static Session* const session = new Session();
    return *session;
}

inline s64 MsSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

inline void Say(const std::string& message) {
    std::lock_guard guard(S().message_lock);
    S().message = message;
}

inline void Wake() {
    { std::lock_guard guard(S().lock); }
    S().wake.notify_all();
}

inline const char* ErrorText(int error) {
    switch (static_cast<Member::Error>(error)) {
    case Member::Error::LostConnection:
        return "the connection was lost";
    case Member::Error::HostKicked:
        return "the room put this console out";
    case Member::Error::UnknownError:
        return "no network (the socket wasn't made)";
    case Member::Error::NameCollision:
        return "the nickname is in use there";
    case Member::Error::MacCollision:
        return "the console's MAC address is in use there";
    case Member::Error::ConsoleIdCollision:
        return "a console with the same console ID is there";
    case Member::Error::WrongVersion:
        return "the room is another version's";
    case Member::Error::WrongPassword:
        return "the room wants a password";
    case Member::Error::CouldNotConnect:
        return "the server didn't answer in 5 s";
    case Member::Error::RoomIsFull:
        return "the room is full";
    case Member::Error::HostBanned:
        return "the room banned this console";
    default:
        return "the room didn't answer";
    }
}

// The room's rule: 4 to 20 letters, digits, spaces, dots, hyphens and underscores.
inline std::string Nickname(const std::string& option, const std::string& console_id_hash, std::size_t variant) {
    std::string name;
    if (option != "auto") {
        for (const char c : option) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '.' || c == '_' || c == '-')
                name += c;
            if (name.size() == 20)
                break;
        }
    }
    if (name.size() < 4)
        name = "3DS";
    // (Taken: the next four characters of the hash.)
    const std::size_t from = variant * 4;
    if (name == "3DS" || variant > 0) {
        name = name.substr(0, 15) + "-";
        for (std::size_t i = from; i < from + 4 && i < console_id_hash.size(); i++)
            name += static_cast<char>(std::toupper(static_cast<unsigned char>(console_id_hash[i])));
    }
    return name;
}

inline void JoinLoop(std::string console_id_hash, Network::MacAddress mac) {
    Common::SetCurrentThreadName("Room join");
    Session& s = S();
    const std::shared_ptr<Member> member = s.member;
    Network::MacAddress preferred = mac;
    std::size_t variant = 0;
    int failures = 0, delay_s = 0;
    bool said = false;
    for (;;) {
        {
            std::unique_lock guard(s.lock);
            if (s.wake.wait_for(guard, std::chrono::seconds(delay_s), [&] { return s.stop; }))
                return;
        }
        const std::string name = Nickname(s.nickname_option, console_id_hash, variant);
        s.error = -1;
        const auto tried = Clock::now();
        member->Join(name, console_id_hash, s.host.c_str(), s.port, 0, preferred);
        {
            // The room's answer (Join returns once the server took the connection, or didn't in 5 s).
            std::unique_lock guard(s.lock);
            s.wake.wait_for(guard, std::chrono::seconds(5),
                            [&] { return s.stop || member->GetState() != Member::State::Joining; });
            if (s.stop)
                return;
        }
        const Member::State state = member->GetState();
        if (state == Member::State::Joined || state == Member::State::Moderator) {
            const auto joined = Clock::now();
            LOG_INFO(Frontend, "EmuSuNi: 3DS online: in the room after {} ms as \"{}\" ({} players there){}",
                     MsSince(tried), name, s.players.load(),
                     preferred == mac ? "" : " (with a MAC address of the room's)");
            Say("Connected to the 3DS room");
            failures = 0;
            said = false;
            bool stopped;
            {
                std::unique_lock guard(s.lock);
                while (!s.stop && member->IsConnected())
                    s.wake.wait_for(guard, std::chrono::seconds(1));
                stopped = s.stop;
            }
            s.in_room_ms += MsSince(joined);
            s.players = 0;
            if (stopped)
                return;
            LOG_WARNING(Frontend, "EmuSuNi: 3DS online: out of the room after {} s: {}; joining again in 30 s",
                        MsSince(joined) / 1000, ErrorText(s.error));
            Say("3DS room disconnected");
            delay_s = 30;
            continue;
        }
        // Not in (the room's answer, or none: the next Join leaves first).
        const int error = state == Member::State::Joining ? -1 : s.error.load();
        failures++;
        switch (static_cast<Member::Error>(error)) {
        case Member::Error::NameCollision:
            if (variant < 3) {
                variant++;
                LOG_INFO(Frontend, "EmuSuNi: 3DS online: the nickname \"{}\" is in use there: another one", name);
                delay_s = 0;
                continue;
            }
            break;
        case Member::Error::MacCollision:
            if (preferred != Network::NoPreferredMac) {
                preferred = Network::NoPreferredMac;
                LOG_INFO(Frontend, "EmuSuNi: 3DS online: the console's MAC address is in use there: the room's");
                delay_s = 0;
                continue;
            }
            break;
        case Member::Error::WrongVersion:
        case Member::Error::WrongPassword:
        case Member::Error::HostBanned:
            LOG_WARNING(Frontend, "EmuSuNi: 3DS online: can't join the room: {} (no more tries in this game)",
                        ErrorText(error));
            Say("The 3DS room doesn't accept this console");
            return;
        default:
            break;
        }
        // (A console ID in use may be this console's own last connection, which the room forgets within 30 s.)
        // (10 s at least: a retry at once after another nickname or MAC address left delay_s at 0.)
        delay_s = static_cast<Member::Error>(error) == Member::Error::RoomIsFull ? 60
                  : failures == 1                                               ? 10
                                                                                : std::clamp(delay_s * 2, 10, 120);
        LOG_WARNING(Frontend, "EmuSuNi: 3DS online: can't join the room: {} (try {}; again in {} s)",
                    ErrorText(error), failures, delay_s);
        if (!said) {
            Say("Can't connect to the 3DS room");
            said = true;
        }
    }
}

// Before the game's load.
inline void Prepare() {
    Session& s = S();
    if (s.network)
        return; // (a reset: the session goes on)
    s.on = false;
    std::string room = LibRetro::FetchVariable("citra_emusuni_room", "disabled");
    // A test without the app's option: the file /data/EmuSuNi/3ds-room.txt (EMUSUNI_3DS_ROOM on the PC), its first
    // line "<IPv4 address>[:port]" - an Azahar for PC's own room on the PS4's network, say (Multiplayer, Host Room).
    const char* source = "the app's 3DS Online";
    if (room.empty() || room == "disabled") {
        std::string test;
        if (const char* env = std::getenv("EMUSUNI_3DS_ROOM")) {
            test = env;
        } else if (FILE* file = std::fopen("/data/EmuSuNi/3ds-room.txt", "r")) {
            char line[128] = {};
            if (std::fgets(line, sizeof(line), file))
                test = line;
            std::fclose(file);
        }
        test.erase(std::remove_if(test.begin(), test.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                   test.end());
        if (!test.empty()) {
            room = test;
            source = "the file /data/EmuSuNi/3ds-room.txt, a test";
        }
    }
    if (room.empty() || room == "disabled") {
        LOG_INFO(Frontend, "EmuSuNi: 3DS online: off (the app's 3DS Online)");
        return;
    }
    std::string host = room;
    u16 port = Network::DefaultRoomPort;
    if (const auto colon = room.rfind(':'); colon != std::string::npos) {
        char* end = nullptr;
        const long value = std::strtol(room.c_str() + colon + 1, &end, 10);
        if (value < 1 || value > 65535 || *end != '\0') {
            LOG_WARNING(Frontend, "EmuSuNi: 3DS online: the room's port isn't a number from 1 to 65535: off");
            return;
        }
        host = room.substr(0, colon);
        port = static_cast<u16>(value);
    }
#ifdef __ORBIS__
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) != 1) {
        LOG_WARNING(Frontend, "EmuSuNi: 3DS online: the room's server isn't an IPv4 address (no lookups here): off");
        return;
    }
#endif
    if (!Network::Init()) {
        LOG_ERROR(Frontend, "EmuSuNi: 3DS online: the network didn't start: off");
        return;
    }
    s.network = true;
    s.member = Network::GetRoomMember().lock();
    Member* const member = s.member.get();
    s.on_state = s.member->BindOnStateChanged([](const Member::State&) { Wake(); });
    s.on_error = s.member->BindOnError([](const Member::Error& error) {
        S().error = static_cast<int>(error);
        Wake();
    });
    s.on_packet = s.member->BindOnWifiPacketReceived([](const Network::WifiPacket& packet) {
        Session& session = S();
        session.packets_in.fetch_add(1, std::memory_order_relaxed);
        session.bytes_in.fetch_add(packet.data.size(), std::memory_order_relaxed);
        if (packet.type == Network::WifiPacket::PacketType::Beacon)
            session.beacons_in.fetch_add(1, std::memory_order_relaxed);
    });
    // (On the member's thread, right after it read the list: the list is that thread's.)
    s.on_info = s.member->BindOnRoomInformationChanged([member](const Network::RoomInformation&) {
        S().players = static_cast<u32>(member->GetMemberInformation().size());
    });
    s.host = host;
    s.port = port;
    s.nickname_option = LibRetro::FetchVariable("citra_emusuni_room_nickname", "auto");
    s.error = -1;
    s.players = 0;
    s.in_room_ms = 0;
    s.frames = 0;
    s.last_log = Clock::now();
    s.base_out = s.logged_out = Network::emusuni_wifi_packets_out.load();
    s.logged_bytes_out = Network::emusuni_wifi_bytes_out.load();
    s.base_in = s.logged_in = s.packets_in.load();
    s.logged_beacons = s.beacons_in.load();
    s.logged_bytes_in = s.bytes_in.load();
    s.logged_players = 0;
    s.on = true;
    LOG_INFO(Frontend, "EmuSuNi: 3DS online: on ({}): the room on port {} of its server", source, port);
}

// After a successful load.
inline void Start(Core::System& system) {
    Session& s = S();
    if (!s.on || s.joiner.joinable())
        return; // (off; or a reset: the thread goes on)
    const std::string hash = Service::CFG::GetConsoleIdHash(system);
    const Network::MacAddress mac = Service::CFG::GetConsoleMacAddress(system);
    {
        std::lock_guard guard(s.lock);
        s.stop = false;
    }
    s.joiner = std::thread(JoinLoop, hash, mac);
}

// Every frame, on the emulation thread.
inline void Tick() {
    Session& s = S();
    if (!s.on || ++s.frames % 30 != 0)
        return;
    std::string message;
    {
        std::lock_guard guard(s.message_lock);
        message.swap(s.message);
    }
    if (!message.empty())
        LibRetro::DisplayMessage(message.c_str());
    const s64 interval_ms = MsSince(s.last_log); // (10 s, or more if the app paused the game)
    if (interval_ms < 10000)
        return;
    s.last_log = Clock::now();
    const u32 out = Network::emusuni_wifi_packets_out.load(std::memory_order_relaxed);
    const u64 bytes_out = Network::emusuni_wifi_bytes_out.load(std::memory_order_relaxed);
    const u32 in = s.packets_in.load(std::memory_order_relaxed);
    const u32 beacons = s.beacons_in.load(std::memory_order_relaxed);
    const u64 bytes_in = s.bytes_in.load(std::memory_order_relaxed);
    const u32 players = s.players.load();
    if (out == s.logged_out && in == s.logged_in && players == s.logged_players)
        return; // (nothing new)
    LOG_INFO(Frontend,
             "EmuSuNi: 3DS online: in the last {} s: {} local wireless packets out ({} KiB), {} in ({} beacons, {} "
             "KiB); {} players in the room",
             interval_ms / 1000, out - s.logged_out, (bytes_out - s.logged_bytes_out) / 1024, in - s.logged_in,
             beacons - s.logged_beacons, (bytes_in - s.logged_bytes_in) / 1024, players);
    s.logged_out = out;
    s.logged_bytes_out = bytes_out;
    s.logged_in = in;
    s.logged_beacons = beacons;
    s.logged_bytes_in = bytes_in;
    s.logged_players = players;
}

// After the game's shutdown (and in retro_deinit).
inline void Stop() {
    Session& s = S();
    if (!s.network)
        return;
    {
        std::lock_guard guard(s.lock);
        s.stop = true;
    }
    s.wake.notify_all();
    if (s.joiner.joinable())
        s.joiner.join(); // (a Join under way: up to its 5 s)
    if (s.member->IsConnected())
        s.member->Leave();
    s.member->Unbind(s.on_state);
    s.member->Unbind(s.on_error);
    s.member->Unbind(s.on_packet);
    s.member->Unbind(s.on_info);
    s.member.reset();
    Network::Shutdown();
    s.network = false;
    s.on = false;
    LOG_INFO(Frontend, "EmuSuNi: 3DS online: the room left: {} s in it; {} local wireless packets out, {} in",
             s.in_room_ms.load() / 1000, Network::emusuni_wifi_packets_out.load() - s.base_out,
             s.packets_in.load() - s.base_in);
}

} // namespace EmuSuNi::Room
#endif
''')
patch('src/citra_libretro/citra_libretro.cpp',
      '#include "citra_libretro/emusuni_jitwarm.h"\n',
      '#include "citra_libretro/emusuni_jitwarm.h"\n#include "citra_libretro/emusuni_room.h"\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    Core::System::ResultStatus load_result{};\n    try {\n',
      '    EmuSuNi::Room::Prepare(); // (01.04) 3DS online: the room member made before the NWM service binds to it\n'
      '    Core::System::ResultStatus load_result{};\n    try {\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    setup_memory_maps();\n\n    return true;\n}\n',
      '    setup_memory_maps();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Room::Start(Core::System::GetInstance()); // (01.04) 3DS online: the join, on its own thread\n'
      '#endif\n\n    return true;\n}\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    EmuSuNi::JitWarm::End(Core::System::GetInstance());  // EmuSuNi: (citra_libretro/emusuni_jitwarm.h)\n'
      '#endif\n    Core::System::GetInstance().Shutdown();\n}\n',
      '    EmuSuNi::JitWarm::End(Core::System::GetInstance());  // EmuSuNi: (citra_libretro/emusuni_jitwarm.h)\n'
      '#endif\n    Core::System::GetInstance().Shutdown();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Room::Stop(); // (01.04) 3DS online: the room left, the network shut\n'
      '#endif\n}\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    EmuSuNi::Stall::Stop(); // (00.85: the stall watch\'s thread)\n',
      '    EmuSuNi::Room::Stop(); // (01.04: the room, if the game\'s unload didn\'t leave it)\n'
      '    EmuSuNi::Stall::Stop(); // (00.85: the stall watch\'s thread)\n')
patch('src/citra_libretro/citra_libretro.cpp',
      '    // EmuSuNi: the JIT warm-up (citra_libretro/emusuni_jitwarm.h): earlier sessions\' blocks compiled ahead.\n'
      '    EmuSuNi::JitWarm::BeforeFrame(Core::System::GetInstance());\n',
      '    EmuSuNi::Room::Tick(); // (01.04) 3DS online: the screen\'s messages and the log (one test when it\'s off)\n'
      '    // EmuSuNi: the JIT warm-up (citra_libretro/emusuni_jitwarm.h): earlier sessions\' blocks compiled ahead.\n'
      '    EmuSuNi::JitWarm::BeforeFrame(Core::System::GetInstance());\n')
# The two options, declared (the app gives Azahar only the options it declares: StrictOptions). Their values are the
# app's: a room's address, a nickname.
patch('src/citra_libretro/core_settings.cpp',
      '        "Emulate the time the 3DS\'s GPU takes (Luigi\'s Mansion 2 needs it).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n',
      '        "Emulate the time the 3DS\'s GPU takes (Luigi\'s Mansion 2 needs it).",\n'
      '        nullptr,\n'
      '        config::category::graphics,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { config::enabled, "Enabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n'
      '    {\n'
      '        "citra_emusuni_room",\n'
      '        "3DS Online (EmuSuNi)",\n'
      '        "3DS Online",\n'
      '        "The Azahar room for the 3DS\'s local wireless play over the internet: its IPv4 address and port "\n'
      '        "(the app\'s 3DS Online), or disabled.",\n'
      '        nullptr,\n'
      '        config::category::system,\n'
      '        {\n'
      '            { config::disabled, "Disabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::disabled\n'
      '    },\n'
      '    {\n'
      '        "citra_emusuni_room_nickname",\n'
      '        "3DS Online Nickname (EmuSuNi)",\n'
      '        "3DS Online Nickname",\n'
      '        "The nickname in the room (auto: 3DS- and four characters of the console ID).",\n'
      '        nullptr,\n'
      '        config::category::system,\n'
      '        {\n'
      '            { "auto", "Auto" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        "auto"\n'
      '    },\n')
# The room member.
patch('src/network/room_member.h',
      '#include <functional>\n',
      '#include <atomic>\n#include <functional>\n')
patch('src/network/room_member.h',
      'namespace Network {\n',
      'namespace Network {\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (01.04): the local wireless packets this console sent to the room and their bytes (the log\'s).\n'
      'extern std::atomic<u32> emusuni_wifi_packets_out;\n'
      'extern std::atomic<u64> emusuni_wifi_bytes_out;\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '#include "common/assert.h"\n',
      '#include "common/assert.h"\n#ifdef EMUSUNI_PORT\n#include "common/thread.h"\n#endif\n')
patch('src/network/room_member.cpp',
      'constexpr u32 ConnectionTimeoutMs = 5000;\n',
      'constexpr u32 ConnectionTimeoutMs = 5000;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      'std::atomic<u32> emusuni_wifi_packets_out{0};\n'
      'std::atomic<u64> emusuni_wifi_bytes_out{0};\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      'void RoomMember::RoomMemberImpl::MemberLoop() {\n',
      'void RoomMember::RoomMemberImpl::MemberLoop() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    Common::SetCurrentThreadName("Room"); // EmuSuNi: the app places it (port/)\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '                    ASSERT_MSG(member_information.size() > 0,\n'
      '                               "We have not yet received member information.");\n',
      '#ifndef EMUSUNI_PORT // EmuSuNi: a room\'s odd answer doesn\'t end the app\n'
      '                    ASSERT_MSG(member_information.size() > 0,\n'
      '                               "We have not yet received member information.");\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '    while (enet_host_service(client, &event, ConnectionTimeoutMs) > 0) {\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: 1 s at most for the server\'s goodbye (5 s held the game\'s end when the server was gone).\n'
      '    while (enet_host_service(client, &event, 1000) > 0) {\n'
      '#else\n'
      '    while (enet_host_service(client, &event, ConnectionTimeoutMs) > 0) {\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '    ASSERT_MSG(!IsConnected(), "RoomMember is being destroyed while connected");\n',
      '#ifndef EMUSUNI_PORT // EmuSuNi: left below\n'
      '    ASSERT_MSG(!IsConnected(), "RoomMember is being destroyed while connected");\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '    ASSERT_MSG(IsConnected(), "Tried to get MAC address while not connected");\n',
      '#ifndef EMUSUNI_PORT // EmuSuNi: the connection may drop between the NWM service\'s test and this\n'
      '    ASSERT_MSG(IsConnected(), "Tried to get MAC address while not connected");\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '        room_member_impl->client = enet_host_create(nullptr, 1, NumChannels, 0, 0);\n'
      '        ASSERT_MSG(room_member_impl->client != nullptr, "Could not create client");\n',
      '        room_member_impl->client = enet_host_create(nullptr, 1, NumChannels, 0, 0);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        if (room_member_impl->client == nullptr) {\n'
      '            // EmuSuNi: no socket (no network): an error, not the app\'s end.\n'
      '            room_member_impl->SetState(State::Idle);\n'
      '            room_member_impl->SetError(Error::UnknownError);\n'
      '            return;\n'
      '        }\n'
      '#else\n'
      '        ASSERT_MSG(room_member_impl->client != nullptr, "Could not create client");\n'
      '#endif\n')
patch('src/network/room_member.cpp',
      '    ENetAddress address{};\n    enet_address_set_host(&address, server_addr);\n',
      '    ENetAddress address{};\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: an address as it is, no lookup (musl\'s asks no DNS server on the PS4); a name on the PC.\n'
      '    if (enet_address_set_host_ip(&address, server_addr) != 0)\n'
      '#endif\n'
      '        enet_address_set_host(&address, server_addr);\n')
patch('src/network/room_member.cpp',
      '    packet << wifi_packet.data;\n    room_member_impl->Send(std::move(packet));\n',
      '    packet << wifi_packet.data;\n    room_member_impl->Send(std::move(packet));\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_wifi_packets_out.fetch_add(1, std::memory_order_relaxed);\n'
      '    emusuni_wifi_bytes_out.fetch_add(wifi_packet.data.size(), std::memory_order_relaxed);\n'
      '#endif\n')
# ENet's socket flags for the PS4's kernel.
patch('externals/enet/unix.c',
      '#ifndef MSG_NOSIGNAL\n#define MSG_NOSIGNAL 0\n#endif\n',
      '#if defined(EMUSUNI_PORT) && defined(__ORBIS__)\n'
      '/* EmuSuNi (scripts/build-3ds-core.sh, 01.04): OpenOrbis gives Linux\'s values, the PS4 kernel is\n'
      '   FreeBSD\'s: MSG_NOSIGNAL 0x4000 is its MSG_NBIO (UDP has no SIGPIPE to keep away), MSG_TRUNC 0x20 its\n'
      '   MSG_CTRUNC. */\n'
      '#undef MSG_NOSIGNAL\n#define MSG_NOSIGNAL 0\n'
      '#undef MSG_TRUNC\n#define MSG_TRUNC 0x10\n'
      '#endif\n'
      '#ifndef MSG_NOSIGNAL\n#define MSG_NOSIGNAL 0\n#endif\n')
# The NWM service: another console's packets never end the app.
patch('src/core/hle/service/nwm/nwm_uds.cpp',
      '    ASSERT_MSG(std::get<AssocStatus>(assoc_result) == AssocStatus::Successful,\n'
      '               "Could not join network");\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.04): a host\'s refusal ends this attempt (the game times out), not the app.\n'
      '    if (std::get<AssocStatus>(assoc_result) != AssocStatus::Successful) {\n'
      '        LOG_WARNING(Service_NWM, "EmuSuNi: a host refused this console\'s association ({})",\n'
      '                    static_cast<u32>(std::get<AssocStatus>(assoc_result)));\n'
      '        return;\n'
      '    }\n'
      '#else\n'
      '    ASSERT_MSG(std::get<AssocStatus>(assoc_result) == AssocStatus::Successful,\n'
      '               "Could not join network");\n'
      '#endif\n')
patch('src/core/hle/service/nwm/nwm_uds.cpp',
      '        ASSERT(connection_status.max_nodes != connection_status.total_nodes);\n',
      '#ifdef EMUSUNI_PORT\n'
      '        if (connection_status.max_nodes == connection_status.total_nodes) {\n'
      '            // EmuSuNi (01.04): a console asking to join a full network is ignored, not an assert.\n'
      '            LOG_WARNING(Service_NWM, "EmuSuNi: a console asked to join this network, which is full");\n'
      '            return;\n'
      '        }\n'
      '#else\n'
      '        ASSERT(connection_status.max_nodes != connection_status.total_nodes);\n'
      '#endif\n')
patch('src/core/hle/service/nwm/nwm_uds.cpp',
      '    ASSERT(!secure_data.is_management);\n',
      '#ifdef EMUSUNI_PORT\n'
      '    if (secure_data.is_management) {\n'
      '        LOG_DEBUG(Service_NWM, "EmuSuNi: a management data frame: dropped"); // (01.04: was an assert)\n'
      '        return;\n'
      '    }\n'
      '#else\n'
      '    ASSERT(!secure_data.is_management);\n'
      '#endif\n')
patch('src/core/hle/service/nwm/nwm_uds.cpp',
      '    // Read the encrypted data.\n'
      '    // The first 4 bytes should be the OUI and the OUI Type of the tags.\n'
      '    std::array<u8, 3> oui;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.04): a beacon\'s odd tags or sizes (another console\'s, in a room) get an error\n'
      '    // (upstream\'s "invalid" result, 0xE1211005), not an assert or a read past the buffers.\n'
      '    if (encrypted_data0_buffer.size() < 4 || encrypted_data1_buffer.size() < 4 ||\n'
      '        std::memcmp(encrypted_data0_buffer.data(), NintendoOUI.data(), NintendoOUI.size()) != 0 ||\n'
      '        encrypted_data0_buffer[3] != static_cast<u8>(NintendoTagId::EncryptedData0) ||\n'
      '        encrypted_data0_buffer.size() + encrypted_data1_buffer.size() - 8 <\n'
      '            sizeof(BeaconData) + std::size_t{net_info.max_nodes} * sizeof(BeaconNodeInfo)) {\n'
      '        LOG_WARNING(Service_NWM, "EmuSuNi: a beacon with odd data: not decrypted");\n'
      '        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);\n'
      '        rb.Push(Result(0xE1211005));\n'
      '        return;\n'
      '    }\n'
      '#endif\n'
      '    // Read the encrypted data.\n'
      '    // The first 4 bytes should be the OUI and the OUI Type of the tags.\n'
      '    std::array<u8, 3> oui;\n')

# --- 01.05: three of v00.23's 3DS reports of 06-11 (the others are 01.04's four fixes, or not the 3DS's):
# - A Link Between Worlds ended in the driver ~10 minutes in (T73ZVE and UWP3HS: SIGSEGV at 0xa0 in RADV's
#   radv_image_queue_family_mask, a blit's barrier on the scheduler's worker). A texture whose smallest level isn't a
#   multiple of 8 is blitted from its source into a surface made for it, and the source was taken before
#   CreateSurface, which can move every surface (the slot vector grows into a new array): the blit read the moved-out
#   one, without an image. Now the source is taken after it (as upstream's 28c6f99 did, #2688, after 2126.2), and a
#   blit with a surface without an image (an invalid format) is left out and said in the log, three times at most.
# - Super Smash Bros. in New 3DS mode ran out of heap saving a state (5VZWA7: "the heap has no 624951296 bytes"): its
#   archive is 301.5 MiB (this PC), past the 298 reserved (the emulated RAM and 32 MiB), and the vector doubled. Now
#   64 MiB are reserved for the rest (35.5 MiB in Smash, in both modes: 0.5 MiB to spare in Old 3DS mode), and past
#   the room the vector grows 32 MiB at a time.
# - Castlevania: Mirror of Fate's end stopped 60 s before the pipeline cache's line (7D3S7C; the start watchdog
#   restarted the app, nothing more in the log). Now each step of the system's end, and each wait of the renderer's,
#   is said before it starts.
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '        const auto [src_surface_id, rect] =\n'
      '            GetSurfaceSubRect(params, ScaleMatch::Ignore, true, initial_flags);\n'
      '        Surface& src_surface = slot_surfaces[src_surface_id];\n'
      '\n'
      '        params.res_scale = src_surface.res_scale;\n'
      '        SurfaceId tmp_surface_id = CreateSurface(params, initial_flags);\n',
      '        const auto [src_surface_id, rect] =\n'
      '            GetSurfaceSubRect(params, ScaleMatch::Ignore, true, initial_flags);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (01.05): the source taken after CreateSurface, which can move every surface (the slot\n'
      '        // vector grows): the blit read the moved-out one, without an image, and the driver ended the app\n'
      '        // (A Link Between Worlds, v00.23\'s reports T73ZVE and UWP3HS). Upstream\'s 28c6f99 does the same.\n'
      '        params.res_scale = slot_surfaces[src_surface_id].res_scale;\n'
      '        SurfaceId tmp_surface_id = CreateSurface(params, initial_flags);\n'
      '        Surface& src_surface = slot_surfaces[src_surface_id];\n'
      '#else\n'
      '        Surface& src_surface = slot_surfaces[src_surface_id];\n'
      '\n'
      '        params.res_scale = src_surface.res_scale;\n'
      '        SurfaceId tmp_surface_id = CreateSurface(params, initial_flags);\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_texture_runtime.cpp',
      'bool TextureRuntime::BlitTextures(Surface& source, Surface& dest,\n'
      '                                  const VideoCore::TextureBlit& blit) {\n',
      'bool TextureRuntime::BlitTextures(Surface& source, Surface& dest,\n'
      '                                  const VideoCore::TextureBlit& blit) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.05): a surface without an image (an invalid format) left out of a blit: a barrier with\n'
      '    // a null image ended the app in the driver (v00.23\'s T73ZVE and UWP3HS: a surface that had moved).\n'
      '    if (!source.Image() || !dest.Image()) {\n'
      '        static u32 emusuni_said = 0;\n'
      '        if (emusuni_said < 3) {\n'
      '            ++emusuni_said;\n'
      '            LOG_ERROR(Render_Vulkan,\n'
      '                      "EmuSuNi: a blit with a surface without an image (from {} {}x{} at {:#x} to {} "\n'
      '                      "{}x{} at {:#x}): left out",\n'
      '                      VideoCore::PixelFormatAsString(source.pixel_format), source.width, source.height,\n'
      '                      source.addr, VideoCore::PixelFormatAsString(dest.pixel_format), dest.width,\n'
      '                      dest.height, dest.addr);\n'
      '        }\n'
      '        return false;\n'
      '    }\n'
      '#endif\n')
patch('src/core/savestate.cpp',
      'protected:\n'
      '    std::streamsize xsputn(const char* s, std::streamsize n) override {\n'
      '        data.insert(data.end(), s, s + n);\n'
      '        return n;\n'
      '    }\n'
      '    int overflow(int c) override {\n'
      '        if (c != traits_type::eof()) {\n'
      '            data.push_back(static_cast<u8>(c));\n',
      'protected:\n'
      '    // (01.05) Past the room reserved, 32 MiB more at a time: the vector\'s own growth doubled it (Super\n'
      '    // Smash Bros. in New 3DS mode asked the heap for 596 MiB and the app ended, v00.23\'s 5VZWA7).\n'
      '    void Room(std::size_t n) {\n'
      '        if (data.capacity() - data.size() < n) {\n'
      '            const std::size_t more = n > (32u << 20) ? n : std::size_t{32u << 20};\n'
      '            LOG_INFO(Core, "EmuSuNi: the save state passed its room ({} MiB): {} MiB more",\n'
      '                     data.capacity() >> 20, more >> 20);\n'
      '            data.reserve(data.size() + more);\n'
      '        }\n'
      '    }\n'
      '    std::streamsize xsputn(const char* s, std::streamsize n) override {\n'
      '        Room(static_cast<std::size_t>(n));\n'
      '        data.insert(data.end(), s, s + n);\n'
      '        return n;\n'
      '    }\n'
      '    int overflow(int c) override {\n'
      '        if (c != traits_type::eof()) {\n'
      '            Room(1);\n'
      '            data.push_back(static_cast<u8>(c));\n')
patch('src/core/savestate.cpp',
      '                Memory::VRAM_SIZE + Memory::N3DS_EXTRA_RAM_SIZE + (32u << 20));\n',
      '                Memory::VRAM_SIZE + Memory::N3DS_EXTRA_RAM_SIZE +\n'
      '                (64u << 20)); // (01.05: 32 MiB; Super Smash Bros.\' rest is 35.5)\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '#include "common/assert.h"\n',
      '#include <chrono>\n'
      '#include "common/assert.h"\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      'RendererVulkan::~RendererVulkan() {\n'
      '    vk::Device device = instance.GetDevice();\n'
      '    scheduler.Finish();\n'
      '    main_present_window.WaitPresent();\n',
      'RendererVulkan::~RendererVulkan() {\n'
      '    vk::Device device = instance.GetDevice();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.05): each wait of the renderer\'s end said before it starts (v00.23\'s report 7D3S7C:\n'
      '    // Castlevania: Mirror of Fate\'s end stopped for 60 s before the pipeline cache\'s line).\n'
      '    const auto emusuni_t0 = std::chrono::steady_clock::now();\n'
      '    const auto emusuni_ms = [&emusuni_t0] {\n'
      '        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(\n'
      '                                          std::chrono::steady_clock::now() - emusuni_t0)\n'
      '                                          .count());\n'
      '    };\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: waiting for the GPU\'s work");\n'
      '#endif\n'
      '    scheduler.Finish();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: waiting for the last picture ({} ms)",\n'
      '             emusuni_ms());\n'
      '#endif\n'
      '    main_present_window.WaitPresent();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: waiting for the GPU to idle ({} ms)",\n'
      '             emusuni_ms());\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '    LibRetro::UnlockQueue();\n'
      '#else\n'
      '    device.waitIdle();\n'
      '#endif\n'
      '\n'
      '    device.destroyShaderModule(present_vertex_shader);\n',
      '    LibRetro::UnlockQueue();\n'
      '#else\n'
      '    device.waitIdle();\n'
      '#endif\n'
      '#ifdef EMUSUNI_PORT\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: the GPU idle ({} ms)", emusuni_ms());\n'
      '#endif\n'
      '\n'
      '    device.destroyShaderModule(present_vertex_shader);\n')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
      '    emusuni_parts.closing = true; // (00.94) the whole pipelines still queued: dropped\n'
      '#endif\n'
      '    pipeline_workers.WaitForRequests();\n'
      '    shader_workers.WaitForRequests();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_optimizer) {\n'
      '        emusuni_optimizer->WaitForRequests();\n'
      '    }\n'
      '#endif\n'
      '    SaveDriverPipelineDiskCache();\n',
      '    emusuni_parts.closing = true; // (00.94) the whole pipelines still queued: dropped\n'
      '    // (01.05) Each wait said before it starts (v00.23\'s 7D3S7C: Castlevania\'s end stopped).\n'
      '    const auto emusuni_t0 = std::chrono::steady_clock::now();\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: waiting for the shader and pipeline workers");\n'
      '#endif\n'
      '    pipeline_workers.WaitForRequests();\n'
      '    shader_workers.WaitForRequests();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (emusuni_optimizer) {\n'
      '        emusuni_optimizer->WaitForRequests();\n'
      '    }\n'
      '    LOG_INFO(Render_Vulkan,\n'
      '             "EmuSuNi: the renderer closes: saving the driver\'s pipeline cache (the workers: {} ms)",\n'
      '             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(\n'
      '                                        std::chrono::steady_clock::now() - emusuni_t0)\n'
      '                                        .count()));\n'
      '#endif\n'
      '    SaveDriverPipelineDiskCache();\n')
patch('src/core/core.cpp',
      'void System::Shutdown(bool is_deserializing) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi: the FS service\'s file requests on their worker finished first '
      '(service/fs/emusuni_fs_worker.h).\n',
      'void System::Shutdown(bool is_deserializing) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.05): each step of the end said before it starts (v00.23\'s 7D3S7C: Castlevania\'s end\n'
      '    // stopped 60 s after the JIT list was kept, before the process\'s cleanup, and nothing more was said).\n'
      '    const auto emusuni_t0 = std::chrono::steady_clock::now();\n'
      '    const auto emusuni_step = [&emusuni_t0](const char* what) {\n'
      '        LOG_INFO(Core, "EmuSuNi: the system closes: {} ({} ms)", what,\n'
      '                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(\n'
      '                                            std::chrono::steady_clock::now() - emusuni_t0)\n'
      '                                            .count()));\n'
      '    };\n'
      '    emusuni_step("waiting for the file requests on their worker");\n'
      '    // EmuSuNi: the FS service\'s file requests on their worker finished first '
      '(service/fs/emusuni_fs_worker.h).\n')
patch('src/core/core.cpp',
      '    is_powered_on = false;\n'
      '\n'
      '    gpu.reset();\n'
      '    if (!is_deserializing) {\n'
      '        lle_modules.clear();\n',
      '    is_powered_on = false;\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_step("the GPU");\n'
      '#endif\n'
      '    gpu.reset();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_step("the game\'s file, the services and the DSP");\n'
      '#endif\n'
      '    if (!is_deserializing) {\n'
      '        lle_modules.clear();\n')
patch('src/core/core.cpp',
      '    dsp_core.reset();\n'
      '    kernel.reset();\n'
      '    cpu_cores.clear();\n',
      '    dsp_core.reset();\n'
      '#ifdef EMUSUNI_PORT\n'
      '    emusuni_step("the kernel");\n'
      '#endif\n'
      '    kernel.reset();\n'
      '    cpu_cores.clear();\n')

# --- 01.06: the owner's slow 3DS games at 4x (his PS4's logs of 07-11: Luigi's Mansion: Dark Moon at 63%, Castlevania:
# Mirror of Fate 34%, Donkey Kong Country Returns 3D 75%, Captain Toad 93%):
# - Surfaces recycled. A surface that the GPU's own writes leave wholly invalid goes to the deletion queue, and the
#   garbage collector erased it as soon as the GPU was done with it: before the next frame asked for the same one, so
#   it was made again, and its framebuffers. On this PC Dark Moon made 26 a frame at 8-48 addresses (the same
#   framebuffers, transfers and copies every frame); the owner's PS4 10 a frame in Dark Moon and 22 in Castlevania, 68
#   us each there. Now the collector keeps a queued surface 3 frames after it first sees it, so the next frames'
#   requests recycle it - still only once the GPU is done with it (upstream's d216f96: a recycled surface can be
#   scaled up, its image made again). Dark Moon on this PC: 13-103 surfaces made in 600 frames instead of 15,500, its
#   "state and framebuffers" a fifth to a third less. The file /data/EmuSuNi/3ds-surface-recycle-off
#   (EMUSUNI_3DS_GC_HOLD=<frames> on the PC): erased once the GPU is done, as before (read once per app run).
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#endif\n',
      '#ifdef EMUSUNI_PORT\n#include <cstdlib>\n#include <cstring>\n#include <unistd.h>\n#include <vector>\n#endif\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      'namespace VideoCore {\n\nMICROPROFILE_DECLARE(RasterizerCache_CopySurface);\n',
      r'''namespace VideoCore {

#ifdef EMUSUNI_PORT
// EmuSuNi (01.06): the garbage collector keeps a surface of the deletion queue some frames after it first sees it
// there, for the next frames' requests to recycle (scripts/build-3ds-core.sh).
inline u32 emusuni_gc_frame = 0;
inline std::vector<u32> emusuni_gc_seen; // by surface slot: the frame the collector first saw it queued, plus 1

inline u32 EmuSuNiGcHold() {
    static const u32 hold = [] {
        const char* env = std::getenv("EMUSUNI_3DS_GC_HOLD");
        u32 frames = 3;
        if (env != nullptr) {
            frames = static_cast<u32>(std::atoi(env));
        } else if (::access("/data/EmuSuNi/3ds-surface-recycle-off", F_OK) == 0) {
            frames = 0;
        }
        LOG_INFO(Render, "EmuSuNi: surfaces of the deletion queue kept {} frames to be recycled (the file "
                         "/data/EmuSuNi/3ds-surface-recycle-off: 0, erased once the GPU is done with them)",
                 frames);
        return frames;
    }();
    return hold;
}

// Whether a queued surface is younger than the hold (its first sight by the collector recorded).
inline bool EmuSuNiGcYoung(SurfaceId id) {
    if (id.index >= emusuni_gc_seen.size()) {
        emusuni_gc_seen.resize(id.index + 64, 0);
    }
    u32& seen = emusuni_gc_seen[id.index];
    if (seen == 0) {
        seen = emusuni_gc_frame + 1;
    }
    return emusuni_gc_frame + 1 - seen < EmuSuNiGcHold();
}

inline void EmuSuNiGcForget(SurfaceId id) {
    if (id.index < emusuni_gc_seen.size()) {
        emusuni_gc_seen[id.index] = 0;
    }
}
#endif

MICROPROFILE_DECLARE(RasterizerCache_CopySurface);
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      'void RasterizerCache<T>::TickFrame() {\n    custom_tex_manager.TickFrame();\n',
      'void RasterizerCache<T>::TickFrame() {\n#ifdef EMUSUNI_PORT\n    emusuni_gc_frame++; // (01.06)\n#endif\n'
      '    custom_tex_manager.TickFrame();\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '        const auto [surface_id, resource_last_used_tick] = *it;\n',
      '        const auto [surface_id, resource_last_used_tick] = *it;\n'
      '#ifdef EMUSUNI_PORT\n'
      '        const bool emusuni_young = EmuSuNiGcYoung(surface_id); // (01.06)\n'
      '#endif\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '            // Resource is still in-use, skip deletion\n            it++;\n            continue;\n        }\n'
      '        RemoveFramebuffers(surface_id);\n',
      '            // Resource is still in-use, skip deletion\n            it++;\n            continue;\n        }\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (01.06): kept some frames, to be recycled.\n'
      '        if (emusuni_young) {\n            it++;\n            continue;\n        }\n'
      '        EmuSuNiGcForget(surface_id);\n'
      '#endif\n'
      '        RemoveFramebuffers(surface_id);\n')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h',
      '            const SurfaceId surface_id = it->first;\n            sentenced.erase(it);\n'
      '            return surface_id;\n',
      '            const SurfaceId surface_id = it->first;\n            sentenced.erase(it);\n'
      '#ifdef EMUSUNI_PORT\n            EmuSuNiGcForget(surface_id); // (01.06)\n#endif\n'
      '            return surface_id;\n')
# - Moved vertex layouts (00.95, 01.03) rebuilt with byte shuffles, strides up to 128 bytes: SSSE3's pshufb (the
#   PS4's Jaguar has it), for each 16 bytes of the new stride two 16-byte loads, two shuffles and a store, for the
#   vertices whose loads lie inside the loader's data; the last few, longer strides and the floats of
#   3ds-vertex-float one attribute at a time as before, and the same bytes (on this PC every batch of Pokemon X and
#   Dark Moon compared with the old way: 600,000 batches, 54 million vertices, none different). Dark Moon's vertex part
#   a ninth less on this PC; Castlevania moves half its vertices (79,000 a frame), and the draws' vertex part took 6.8
#   ms a frame on the owner's PS4.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#include <unistd.h>\n#include <vector>\n',
      '#include <unistd.h>\n#include <vector>\n#if defined(__x86_64__)\n#include <immintrin.h> // (01.06)\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      "// What the alignment moved, for the log every 600 frames (the emulation thread's).\n",
      r'''// (01.06) A moved loader's vertices rebuilt with byte shuffles, the new stride in pieces of 16 bytes: for
// each piece, the first source byte it takes, and for each of its bytes the one it takes of the 32 from there (two
// loads, two shuffles), or a zero (0x80) where no attribute lies, as the copies one attribute at a time leave them.
// Strides up to 128 bytes, each piece's bytes within 32 of the source (attributes only move a few bytes on).
struct EmuSuNiShuffle {
    u32 pieces = 0;
    u32 window = 0; // how far into a vertex's source its loads reach
#if defined(__x86_64__)
    std::array<u32, 8> base{};
    std::array<__m128i, 16> masks{}; // each piece's masks for its two loads
#endif
};

bool EmuSuNiShufflePlan(EmuSuNiShuffle& plan, u32 src_stride, u32 new_stride,
                        const std::array<std::array<u32, 4>, 12>& attrs, const std::array<u32, 12>& offsets,
                        u32 count) {
#if defined(__x86_64__)
    constexpr u32 NONE = 0xFFFF;
    if (src_stride == 0 || new_stride == 0 || new_stride > 128) {
        return false;
    }
    std::array<u32, 128> from;
    from.fill(NONE);
    for (u32 i = 0; i < count; i++) {
        for (u32 b = 0; b < attrs[i][2]; b++) {
            if (offsets[i] + b >= new_stride || attrs[i][1] + b >= src_stride) {
                return false; // (outside the strides: one attribute at a time)
            }
            from[offsets[i] + b] = attrs[i][1] + b;
        }
    }
    plan.pieces = (new_stride + 15) / 16;
    plan.window = 0;
    alignas(16) u8 masks[16][16];
    for (u32 j = 0; j < plan.pieces; j++) {
        u32 first = NONE;
        for (u32 x = 0; x < 16; x++) {
            first = std::min(first, from[16 * j + x]);
        }
        if (first == NONE) {
            first = 0; // (a piece of padding only: zeros)
        }
        plan.base[j] = first;
        for (u32 x = 0; x < 16; x++) {
            const u32 f = from[16 * j + x];
            masks[2 * j][x] = 0x80;
            masks[2 * j + 1][x] = 0x80;
            if (f == NONE) {
                continue;
            }
            if (f - first >= 32) {
                return false;
            }
            masks[2 * j + (f - first) / 16][x] = static_cast<u8>((f - first) % 16);
        }
        plan.window = std::max(plan.window, first + 32);
        plan.masks[2 * j] = _mm_load_si128(reinterpret_cast<const __m128i*>(masks[2 * j]));
        plan.masks[2 * j + 1] = _mm_load_si128(reinterpret_cast<const __m128i*>(masks[2 * j + 1]));
    }
    return true;
#else
    return false;
#endif
}

// `count` vertices from `src` (each one's plan.window bytes of source inside the loader's data) into `dst` (with
// room for 16 bytes past the last one): how many it rebuilt.
#if defined(__x86_64__)
__attribute__((target("ssse3")))
#endif
std::size_t EmuSuNiShuffleVertices(u8* dst, const u8* src, std::size_t count, u32 src_stride, u32 new_stride,
                                   const EmuSuNiShuffle& plan) {
#if defined(__x86_64__)
    for (std::size_t k = 0; k < count; k++) {
        const u8* s = src + k * src_stride;
        u8* d = dst + k * new_stride;
        for (u32 j = 0; j < plan.pieces; j++) {
            const __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + plan.base[j]));
            const __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + plan.base[j] + 16));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 16 * j),
                             _mm_or_si128(_mm_shuffle_epi8(lo, plan.masks[2 * j]),
                                          _mm_shuffle_epi8(hi, plan.masks[2 * j + 1])));
        }
    }
    return count;
#else
    return 0;
#endif
}

// What the alignment moved, for the log every 600 frames (the emulation thread's).
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '            const u32 new_stride = aligned_stride;\n'
      '            // A batch of vertices made in ordinary memory, then copied in one piece (the stream buffer is '
      'write-\n'
      '            // combined on the PS4: small scattered writes there are slow).\n'
      '            alignas(16) u8 batch[4096];\n'
      '            const std::size_t per_batch = std::max<std::size_t>(1, sizeof(batch) / new_stride);\n',
      '            const u32 new_stride = aligned_stride;\n'
      '            // (01.06) Byte shuffles for the vertices whose loads lie inside the loader\'s data (not with\n'
      '            // 3ds-vertex-float\'s floats).\n'
      '            EmuSuNiShuffle emusuni_plan;\n'
      '            const bool emusuni_simd =\n'
      '                !emusuni_float && EmuSuNiShufflePlan(emusuni_plan, loader.byte_count, new_stride,\n'
      '                                                     emusuni_attrs, new_offsets, emusuni_count);\n'
      '            const std::size_t emusuni_window = emusuni_plan.window;\n'
      '            const std::size_t emusuni_src_bytes = static_cast<std::size_t>(vertex_num) * loader.byte_count;\n'
      '            const std::size_t emusuni_simd_end =\n'
      '                emusuni_simd && emusuni_src_bytes >= emusuni_window\n'
      '                    ? (emusuni_src_bytes - emusuni_window) / loader.byte_count + 1\n'
      '                    : 0;\n'
      '            // A batch of vertices made in ordinary memory, then copied in one piece (the stream buffer is\n'
      '            // write-combined on the PS4: small scattered writes there are slow). (01.06: 32 bytes more, for\n'
      '            // the last vertex\'s shuffled stores.)\n'
      '            alignas(16) u8 batch[4096 + 32];\n'
      '            const std::size_t per_batch = std::max<std::size_t>(1, 4096 / new_stride);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                if (count * new_stride > sizeof(batch)) {\n',
      '                if (count * new_stride > 4096) {\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '                std::memset(batch, 0, count * new_stride);\n'
      '                for (std::size_t k = 0; k < count; k++) {\n'
      '                    const u8* src = src_ptr + (first + k) * loader.byte_count;\n',
      '                // (01.06) The shuffles first, then the rest one attribute at a time.\n'
      '                std::size_t k = 0;\n'
      '                if (emusuni_simd_end > first) {\n'
      '                    k = EmuSuNiShuffleVertices(batch, src_ptr + first * loader.byte_count,\n'
      '                                               std::min<std::size_t>(count, emusuni_simd_end - first),\n'
      '                                               loader.byte_count, new_stride, emusuni_plan);\n'
      '                }\n'
      '                std::memset(batch + k * new_stride, 0, (count - k) * new_stride);\n'
      '                for (; k < count; k++) {\n'
      '                    const u8* src = src_ptr + (first + k) * loader.byte_count;\n')
# - A test switch for the games whose own code is the slow part (Castlevania's guest code: 28 ms a frame on the owner's
#   PS4): the file /data/EmuSuNi/3ds-cpu-unsafe-fp (EMUSUNI_3DS_CPU_UNSAFE_FP=1 on the PC) gives dynarmic its unsafe
#   floating-point shortcuts (NaN results not made exact, no FMA unfused, reduced-error estimates, the standard FPSCR
#   value ignored), not the global monitor's (the cores' exclusive accesses stay exact). Read at each JIT's making (the
#   game's start); without the file the ARM11's code runs as before.
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '#include <stdexcept>\n#include "common/emusuni_jit.h"\n',
      '#include <stdexcept>\n#include <cstdlib>\n#include <cstring>\n#include <unistd.h>\n'
      '#include "common/emusuni_jit.h"\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    config.define_unpredictable_behaviour = true;\n#ifdef EMUSUNI_PORT\n',
      '    config.define_unpredictable_behaviour = true;\n#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.06): the test switch 3ds-cpu-unsafe-fp, dynarmic\'s unsafe floating-point shortcuts.\n'
      '    {\n'
      '        const char* emusuni_env = std::getenv("EMUSUNI_3DS_CPU_UNSAFE_FP");\n'
      '        if (emusuni_env != nullptr ? std::strcmp(emusuni_env, "0") != 0\n'
      '                                   : ::access("/data/EmuSuNi/3ds-cpu-unsafe-fp", F_OK) == 0) {\n'
      '            config.unsafe_optimizations = true;\n'
      '            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |\n'
      '                                    Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA |\n'
      '                                    Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |\n'
      '                                    Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;\n'
      '            LOG_INFO(Core_ARM11,\n'
      '                     "EmuSuNi: core {}\'s JIT: dynarmic\'s unsafe floating-point shortcuts (the file "\n'
      '                     "/data/EmuSuNi/3ds-cpu-unsafe-fp)",\n'
      '                     GetID());\n'
      '        }\n'
      '    }\n')
# - The frame handed over without waiting for the Vulkan worker, when the app asks (EMUSUNI_3DS_ASYNC_HANDOFF=1, set
#   before the game loads). Upstream's TickFrame waits at the end of every frame until the worker has recorded and
#   sent all of the frame's work (WaitWorker), so the picture's submission is in the queue before retro_run returns
#   and the app's drawing reads it; the emulation thread stood still meanwhile (Donkey Kong Country Returns 3D:
#   2.7-4 ms a frame on the owner's PS4). Now each picture has a serial (set_image's caller asks
#   emusuni_3ds_image_serial), the worker says when its submission is made, and the app waits for that itself before
#   its own submission reads the picture (emusuni_3ds_wait_image), on its drawing thread - beside the next frame's
#   emulation in the heavy mode. The frame's end waits only for the picture before (the worker one frame behind at
#   most). Nothing else needs the worker idle there: the garbage collector, the descriptor sets, the command buffers
#   and the stream buffers go by the GPU's finished ticks, not by the worker. A renderer that ends counts its pictures
#   as sent (no wait outlives it). The worker's time after each frame's end, which upstream's waited for, is kept per
#   picture (emusuni_3ds_image_worker_ms): the app's pacing counts it as the core's, as before. Without the variable,
#   as upstream.
patch('src/video_core/renderer_vulkan/vk_scheduler.h',
      'class Instance;\n\n/// The scheduler abstracts',
      'class Instance;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '// EmuSuNi (01.06, vk_scheduler.cpp): each picture handed to the app has a serial; whether the app waits\n'
      '// for each one\'s submission itself (EMUSUNI_3DS_ASYNC_HANDOFF=1: the frame\'s end then waits only for the\n'
      '// one before).\n'
      'u64 EmuSuNiNextImageSerial();\n'
      'bool EmuSuNiAsyncHandoff();\n'
      '// Picture `serial`\'s frame ends now (TickFrame, with the handover): the worker\'s time from here to its\n'
      '// submission is what upstream waited for (emusuni_3ds_image_worker_ms).\n'
      'void EmuSuNiImageFrameEnded(u64 serial);\n'
      '#endif\n\n'
      '/// The scheduler abstracts')
patch('src/video_core/renderer_vulkan/vk_scheduler.h',
      '    std::mutex submit_mutex;\n\nprivate:\n',
      '    std::mutex submit_mutex;\n\n'
      '#ifdef EMUSUNI_PORT\n'
      '    /// EmuSuNi (01.06): the next submission draws this picture; its serial goes to the app once it is\n'
      '    /// made.\n'
      '    void EmuSuNiMarkNextSubmit(u64 serial) {\n'
      '        emusuni_next_mark = serial;\n'
      '    }\n'
      '#endif\n\n'
      'private:\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.h',
      '    bool use_worker_thread;\n',
      '    bool use_worker_thread;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    u64 emusuni_next_mark = 0; // (01.06) the picture the next submission draws, 0: none\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n',
      '#include "common/microprofile.h"\n#ifdef EMUSUNI_PORT\n'
      '#include <algorithm>\n#include <array>\n#include <chrono>\n#include <condition_variable>\n#include <cstdint>\n'
      '#include <cstdlib>\n#include <cstring>\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      'MICROPROFILE_DEFINE(Vulkan_Submit, "Vulkan", "Submit Exectution", MP_RGB(255, 192, 255));\n',
      r'''MICROPROFILE_DEFINE(Vulkan_Submit, "Vulkan", "Submit Exectution", MP_RGB(255, 192, 255));

#ifdef EMUSUNI_PORT
// EmuSuNi (01.06, scripts/build-3ds-core.sh): the pictures handed to the app. The emulation thread gives each a serial
// (RendererVulkan::RenderToWindow, before its submission and set_image); the Vulkan worker marks it once that
// submission is made; the app waits for its picture's mark before its own submission reads it. The three functions at
// the end are the object's only globals besides the libretro API.
namespace {
std::mutex emusuni_image_mutex;
std::condition_variable emusuni_image_cv;
u64 emusuni_image_serial = 0;    // the last picture's serial (handed out on the emulation thread)
u64 emusuni_image_submitted = 0; // the last one whose submission is made
// For the last 8 pictures: when the frame's end reached the point where upstream's waited for the worker (TickFrame,
// with the handover) and when the picture's submission was made, each the first time it is said. The worker's time
// between the two is what upstream's frame end waited for (emusuni_3ds_image_worker_ms).
struct EmuSuNiImageTimes {
    u64 serial = 0;
    bool ended = false;
    bool sent = false;
    std::chrono::steady_clock::time_point ended_at{};
    std::chrono::steady_clock::time_point sent_at{};
};
std::array<EmuSuNiImageTimes, 8> emusuni_image_times{};

// (Under emusuni_image_mutex.) This picture's entry, emptied when it held an older picture; none when it holds a newer
// one.
EmuSuNiImageTimes* EmuSuNiTimesOf(u64 serial) {
    EmuSuNiImageTimes& times = emusuni_image_times[serial % emusuni_image_times.size()];
    if (serial == 0 || times.serial > serial) {
        return nullptr;
    }
    if (times.serial < serial) {
        times = EmuSuNiImageTimes{};
        times.serial = serial;
    }
    return &times;
}

// made: the worker made the picture's submission (its time kept); false for a renderer that ends (every picture it
// handed over counted as sent, with no time).
void EmuSuNiImageSubmitted(u64 serial, bool made) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{emusuni_image_mutex};
        emusuni_image_submitted = std::max(emusuni_image_submitted, serial);
        EmuSuNiImageTimes* times = made ? EmuSuNiTimesOf(serial) : nullptr;
        if (times != nullptr && !times->sent) {
            times->sent = true;
            times->sent_at = now;
        }
    }
    emusuni_image_cv.notify_all();
}
} // namespace

namespace Vulkan {
u64 EmuSuNiNextImageSerial() {
    std::scoped_lock lock{emusuni_image_mutex};
    return ++emusuni_image_serial;
}

void EmuSuNiImageFrameEnded(u64 serial) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{emusuni_image_mutex};
    EmuSuNiImageTimes* times = EmuSuNiTimesOf(serial);
    if (times != nullptr && !times->ended) {
        times->ended = true;
        times->ended_at = now;
    }
}

bool EmuSuNiAsyncHandoff() {
    const char* env = std::getenv("EMUSUNI_3DS_ASYNC_HANDOFF");
    return env != nullptr && std::strcmp(env, "1") == 0;
}
} // namespace Vulkan

// The serial of the picture last handed over (set_image's caller: read it there). 0: none yet.
extern "C" std::uint64_t emusuni_3ds_image_serial(void) {
    std::scoped_lock lock{emusuni_image_mutex};
    return emusuni_image_serial;
}

// Waits until the submission that draws picture `serial` is made (at once for 0, or once its renderer ended), at most
// timeout_ms: 0 when it is, -1 when the time ran out first.
extern "C" int emusuni_3ds_wait_image(std::uint64_t serial, std::uint32_t timeout_ms) {
    std::unique_lock lock{emusuni_image_mutex};
    const bool made = emusuni_image_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                                [serial] { return emusuni_image_submitted >= serial; });
    return made ? 0 : -1;
}

// The Vulkan worker's time from picture `serial`'s frame end to its submission, in ms: what upstream's frame end waited
// for, now beside the app's work or the next frame (the app's pacing counts it as the core's, as before). 0 when the
// submission was made first, -1 when either isn't known (yet, any more, or without the handover).
extern "C" double emusuni_3ds_image_worker_ms(std::uint64_t serial) {
    std::scoped_lock lock{emusuni_image_mutex};
    const EmuSuNiImageTimes& times = emusuni_image_times[serial % emusuni_image_times.size()];
    if (serial == 0 || times.serial != serial || !times.ended || !times.sent) {
        return -1.0;
    }
    if (times.sent_at <= times.ended_at) {
        return 0.0;
    }
    return std::chrono::duration<double, std::milli>(times.sent_at - times.ended_at).count();
}
#endif
''')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      'Scheduler::~Scheduler() = default;\n',
      '#ifdef EMUSUNI_PORT\n'
      'Scheduler::~Scheduler() {\n'
      '    // EmuSuNi (01.06): every picture this renderer handed over counts as sent: no wait of the app\'s\n'
      '    // outlives it.\n'
      '    EmuSuNiImageSubmitted(emusuni_3ds_image_serial(), false);\n'
      '}\n'
      '#else\n'
      'Scheduler::~Scheduler() = default;\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '    Record([signal_semaphore, wait_semaphore, signal_value, this](vk::CommandBuffer cmdbuf) {\n'
      '        MICROPROFILE_SCOPE(Vulkan_Submit);\n'
      '        std::scoped_lock lock{submit_mutex};\n'
      '        master_semaphore->SubmitWork(cmdbuf, wait_semaphore, signal_semaphore, signal_value);\n'
      '    });\n',
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.06): the picture this submission draws, if any, marked once it is made.\n'
      '    const u64 emusuni_mark = std::exchange(emusuni_next_mark, 0);\n'
      '    Record([signal_semaphore, wait_semaphore, signal_value, emusuni_mark, this](vk::CommandBuffer cmdbuf) {\n'
      '        MICROPROFILE_SCOPE(Vulkan_Submit);\n'
      '        {\n'
      '            std::scoped_lock lock{submit_mutex};\n'
      '            master_semaphore->SubmitWork(cmdbuf, wait_semaphore, signal_semaphore, signal_value);\n'
      '        }\n'
      '        if (emusuni_mark != 0) {\n'
      '            EmuSuNiImageSubmitted(emusuni_mark, true);\n'
      '        }\n'
      '    });\n'
      '#else\n'
      '    Record([signal_semaphore, wait_semaphore, signal_value, this](vk::CommandBuffer cmdbuf) {\n'
      '        MICROPROFILE_SCOPE(Vulkan_Submit);\n'
      '        std::scoped_lock lock{submit_mutex};\n'
      '        master_semaphore->SubmitWork(cmdbuf, wait_semaphore, signal_semaphore, signal_value);\n'
      '    });\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '        DrawScreens(frame, layout, flipped);\n        scheduler.Flush(frame->render_ready);\n',
      '        DrawScreens(frame, layout, flipped);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        // EmuSuNi (01.06): this picture\'s serial, for the app (emusuni_3ds_image_serial in set_image, and\n'
      '        // emusuni_3ds_wait_image until the worker has made this submission).\n'
      '        scheduler.EmuSuNiMarkNextSubmit(EmuSuNiNextImageSerial());\n'
      '#endif\n'
      '        scheduler.Flush(frame->render_ready);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      "// What the alignment moved, for the log every 600 frames (the emulation thread's).\n",
      '// (01.06) Whether the app waits for each picture\'s submission itself (EMUSUNI_3DS_ASYNC_HANDOFF=1), read\n'
      '// when the game starts.\n'
      'bool emusuni_async_handoff = false;\n'
      'extern "C" std::uint64_t emusuni_3ds_image_serial(void);                  // vk_scheduler.cpp\n'
      'extern "C" int emusuni_3ds_wait_image(std::uint64_t serial, std::uint32_t timeout_ms);\n\n'
      "// What the alignment moved, for the log every 600 frames (the emulation thread's).\n")
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      "    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game's start\n",
      "    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game's start\n"
      '    // (01.06) The frame handed over without waiting for the Vulkan worker, when the app waits itself.\n'
      '    emusuni_async_handoff = EmuSuNiAsyncHandoff();\n'
      '    LOG_INFO(Render_Vulkan, "EmuSuNi: each frame\'s end {}",\n'
      '             emusuni_async_handoff ? "waits only for the picture before (the app waits for each picture\'s "\n'
      '                                     "submission itself: EMUSUNI_3DS_ASYNC_HANDOFF)"\n'
      '                                   : "waits for the Vulkan worker to send all of the frame\'s work (as "\n'
      '                                     "upstream)");\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::TickFrame() {\n    scheduler.WaitWorker();\n',
      'void RasterizerVulkan::TickFrame() {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.06): with the app waiting for each picture\'s submission (EMUSUNI_3DS_ASYNC_HANDOFF),\n'
      '    // only the picture before must be sent here: the Vulkan worker records this frame\'s last work beside\n'
      '    // the next one.\n'
      '    if (emusuni_async_handoff) {\n'
      '        const u64 emusuni_serial = emusuni_3ds_image_serial();\n'
      '        if (emusuni_serial > 1 && emusuni_3ds_wait_image(emusuni_serial - 1, 0) != 0) {\n'
      '            const EmuSuNi::Stats::Timer emusuni_timer{EmuSuNi::Stats::WaitUs};\n'
      '            const EmuSuNi::Stats::Doing emusuni_doing{"waiting for the Vulkan worker (the picture before)"};\n'
      '            while (emusuni_3ds_wait_image(emusuni_serial - 1, 60000) != 0) {\n'
      '            }\n'
      '        }\n'
      '        // This picture\'s frame ends here: the worker\'s time from now to its submission is what upstream\n'
      '        // waited for just here (emusuni_3ds_image_worker_ms; the app\'s pacing counts it as the core\'s).\n'
      '        EmuSuNiImageFrameEnded(emusuni_serial);\n'
      '    } else {\n'
      '        scheduler.WaitWorker();\n'
      '    }\n'
      '#else\n'
      '    scheduler.WaitWorker();\n'
      '#endif\n')

# --- 01.09: the app's installer of a game's update and DLC from CIA files, and its Mods option per game.
# - The installer: five C functions the app's menus call (citra_libretro/emusuni_cia.h; src/ThreeDsCia.cpp declares
#   them, weakly defined there; the final link keeps them global: CIA_GLOBALS). Upstream's own installer
#   (Service::AM::CIAFile, as its InstallCIA feeds it) into the emulated SD card (<save dir>/Azahar/sdmc/), where a
#   game's load finds its update (ncch.cpp) and the game its DLC; its own loop, so that the app's Cancel stops it.
#   Before it, EmuSuNi's checks: the header's parts against each other and the file's size (upstream's parser
#   allocates and reads what they say), no encrypted content (as upstream checks it, for the contents the CIA has: a
#   DLC's pack may hold a part of its TMD's; the core has no keys and needs none), updates and DLC only, the free
#   space (the app's port/ps4_diskfree.c), no game running. An installed update is moved aside, a DLC's pieces copied
#   aside (its packs add up), and put back when the install is cancelled, fails or is rescued (each call under a
#   rescue point).
# - Mods: the option citra_emusuni_mods (the app's, per game; enabled by default, as upstream). Disabled, the game
#   takes none of its mods: load/mods/<title ID>/ (LayeredFS, ExeFS files, code patches, exheader.bin), Luma's
#   code.ips, the files beside the ROM.
# - The game's load says what it takes from the SD card and its mods folder (its update and version, its DLC, the
#   mods folder used or left alone).
addfile('src/citra_libretro/emusuni_cia.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.09).
// The 3DS core's installer of a game's update and DLC from CIA files into the emulated SD card, for the app's menus
// (src/ThreeDsCia.cpp declares the same, weakly defined there until this object brings them). Plain C. save_dir: the
// 3DS core's libretro save directory (<UserDir>/saves/3ds); the core's own files are in <save_dir>/Azahar/.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// This interface's version: emusuni_3ds_cia_version() returns it.
#define EMUSUNI_3DS_CIA_VERSION 1

// The results: 0, or one of these (negative; the app's own start at -100).
#define EMUSUNI_3DS_CIA_OK 0
#define EMUSUNI_3DS_CIA_ENCRYPTED (-1)     // its contents are encrypted: refused (the core has no keys, needs none)
#define EMUSUNI_3DS_CIA_INVALID (-2)       // not a CIA, or damaged: its parts don't fit in the file
#define EMUSUNI_3DS_CIA_IO (-3)            // it couldn't be read or written, or there's no room for it
#define EMUSUNI_3DS_CIA_CANCELLED (-4)     // stopped by *cancel: what was there before is back
#define EMUSUNI_3DS_CIA_CRASHED (-5)       // the core failed on it (its rescue point): what was there before is back
#define EMUSUNI_3DS_CIA_NOT_INSTALLED (-6) // a kind this installer leaves alone: only updates and DLC are installed
#define EMUSUNI_3DS_CIA_GAME_RUNNING (-7)  // a 3DS game is running: nothing installed or removed under it
#define EMUSUNI_3DS_CIA_BUSY (-8)          // another install or removal is running

struct emusuni_3ds_cia_info {
    uint64_t title_id;      // 00040000... a game, 0004000E... its update, 0004008C... its DLC
    uint16_t version;       // as its TMD has it: major v>>10, minor (v>>4)&63, micro v&15
    uint8_t encrypted;      // 1: its contents are encrypted (the install refuses it)
    uint8_t reserved[5];
    uint64_t file_bytes;    // the CIA's size (a compressed one's: its contents')
    uint64_t install_bytes; // what the install writes into the emulated SD card
    uint32_t contents;      // its contents (a DLC's: one per piece)
    char name[128];         // its icon's short English title, UTF-8 ("" without one: an update or DLC often has none)
};

struct emusuni_3ds_installed {
    uint16_t update_version;
    uint8_t has_update, reserved[5];
    uint64_t update_bytes;
    uint32_t dlc_titles, dlc_contents; // (the DLC's pieces installed)
    uint64_t dlc_bytes;
};

// Called while installing, on the installing thread, every 64 KiB: the CIA's bytes done and its size.
typedef void (*emusuni_3ds_cia_progress)(uint64_t done, uint64_t total, void* user);

// EMUSUNI_3DS_CIA_VERSION.
int emusuni_3ds_cia_version(void);

// What a CIA is: its header, ticket, TMD and icon, checked as an install checks them (an encrypted one is read:
// encrypted 1). EMUSUNI_3DS_CIA_IO / _INVALID / _CRASHED when it can't be. Any thread, also during an install.
int emusuni_3ds_cia_info(const char* path, struct emusuni_3ds_cia_info* info);

// Installs an update or DLC into the emulated SD card (<save_dir>/Azahar/sdmc/), with no 3DS game running. Blocking:
// from a thread of its own. Over an installed version it replaces it; cancelled (*cancel set nonzero, read before each
// 64 KiB) or failed, the version before is put back. progress and cancel may be null.
int emusuni_3ds_cia_install(const char* save_dir, const char* path, emusuni_3ds_cia_progress progress, void* user,
                            volatile int* cancel);

// What is installed for a game (its title ID, 00040000...): its update and its DLC. Read only; any thread.
int emusuni_3ds_installed(const char* save_dir, uint64_t game_title_id, struct emusuni_3ds_installed* out);

// Removes an installed update (0004000E...) or DLC (0004008C...): its contents; the game's save data stays. Nothing
// installed: EMUSUNI_3DS_CIA_OK. With no 3DS game running.
int emusuni_3ds_uninstall(const char* save_dir, uint64_t title_id);

#ifdef __cplusplus
}
#endif
''')
addfile('src/citra_libretro/emusuni_cia.cpp', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.09).
// See emusuni_cia.h. Upstream's installer (Service::AM::CIAFile, as its InstallCIA feeds it), after EmuSuNi's checks:
// the header's parts against each other and the file's size (upstream's parser allocates and reads what they say), no
// encrypted content (upstream's own check: the TMD's flags, each content's NCCH header), only updates and DLC, the
// free space, no game running. Its own loop, so that *cancel stops it. An installed version is moved aside first
// (content.emusuni-before/) and put back when the install is cancelled, fails or is rescued; so the content folder is
// always a new one, whose TMD names are upstream's own (its std::stoul on another name throws from inside the C++
// library, where no rescue point reaches). Each call under a rescue point (common/emusuni_throw.h), its outcome in the
// app's log (stderr: the core's own log runs only in a game).
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <fmt/format.h>
#if !defined(__ORBIS__)
#include <sys/statvfs.h>
#endif
#include "citra_libretro/emusuni_cia.h"
#include "common/common_types.h"
#include "common/emusuni_throw.h"
#include "common/file_derived.h"
#include "common/file_util.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

#if defined(__ORBIS__)
// The app's (port/ps4_diskfree.c): 1 and *out when the free space of the partition that has path could be read.
extern "C" int emusuni_disk_space_known(const char* path, int64_t* out);
#endif

// The structs as the app's src/ThreeDsCia.cpp has them (it checks the same sizes).
static_assert(sizeof(struct emusuni_3ds_cia_info) == 168 && offsetof(struct emusuni_3ds_cia_info, contents) == 32 &&
              offsetof(struct emusuni_3ds_cia_info, name) == 36);
static_assert(sizeof(struct emusuni_3ds_installed) == 32 && offsetof(struct emusuni_3ds_installed, dlc_bytes) == 24);

namespace {

using Service::FS::MediaType;

// The emulated SD card's titles, as upstream names their folders (am.cpp, GetMediaTitlePath: its fixed IDs; the
// install checks it against upstream's own path).
constexpr const char* SD_TITLES = "Azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/"
                                  "00000000000000000000000000000000/title/";
constexpr const char* KEPT = "content.emusuni-before/";

enum Kind { GAME, UPDATE, DLC, OTHER };

std::atomic<bool> g_busy{false};

// The install being done, for its rescue (a global keeps its values across the jump back).
struct Job {
    u64 title_id = 0;
    std::string content, kept;
    bool had_old = false;
};
Job g_job;

void Say(const char* format, ...) {
    char line[960];
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    EmuSuNi::RawLine(line);
}

// One install or removal at a time, and none under a running game.
struct Gate {
    int refused = EMUSUNI_3DS_CIA_OK;
    bool taken = false;
    Gate() {
        if (g_busy.exchange(true)) {
            refused = EMUSUNI_3DS_CIA_BUSY;
            return;
        }
        taken = true;
        if (Core::System::GetInstance().IsPoweredOn())
            refused = EMUSUNI_3DS_CIA_GAME_RUNNING;
    }
    ~Gate() {
        if (taken)
            g_busy.store(false);
    }
};

Kind KindOf(u64 title_id) {
    switch (static_cast<u32>(title_id >> 32)) {
    case 0x00040000:
        return GAME;
    case 0x0004000E:
        return UPDATE;
    case 0x0004008C:
        return DLC;
    default:
        return OTHER;
    }
}

const char* KindName(Kind kind) {
    return kind == UPDATE ? "update" : kind == DLC ? "DLC" : kind == GAME ? "game" : "other";
}

std::string SaveDir(const char* save_dir) {
    std::string dir = save_dir;
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir;
}

std::string TitleDir(const std::string& save, u64 title_id) {
    return save + SD_TITLES + fmt::format("{:08x}/{:08x}/", title_id >> 32, title_id & 0xFFFFFFFFULL);
}

std::string Unslash(std::string path) {
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

std::string Version(u16 v) {
    return fmt::format("v{} ({}.{}.{})", v, v >> 10, (v >> 4) & 0x3F, v & 0xF);
}

constexpr u64 Align64(u64 value) {
    return (value + 63) & ~u64{63};
}

// The file as upstream reads a CIA (a compressed one, Azahar's Z3DS, through its decompressor).
std::unique_ptr<FileUtil::IOFileBase> Open(const std::string& path) {
    auto plain = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (!plain->IsOpen())
        return nullptr;
    std::unique_ptr<FileUtil::IOFileBase> file = std::move(plain);
    if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt)
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    return file;
}

// The header's parts against each other and the file's size, before upstream's parser allocates and reads what
// they say (a ticket or TMD size of 2 GiB, a content past the end).
int CheckHeader(FileUtil::IOFileBase& file, u64 size) {
    FileSys::CIAHeader header{};
    if (size < FileSys::CIA_HEADER_SIZE || file.ReadAtBytes(&header, sizeof(header), 0) != sizeof(header))
        return EMUSUNI_3DS_CIA_INVALID;
    const u64 cert = header.cert_size, tik = header.tik_size, tmd = header.tmd_size, meta = header.meta_size;
    const u64 content = header.content_size;
    if (header.header_size != FileSys::CIA_HEADER_SIZE || cert > (64u << 10) || tik == 0 || tik > (64u << 10) ||
        tmd == 0 || tmd > (4u << 20) || meta > (64u << 10) || content == 0 || content > size) {
        return EMUSUNI_3DS_CIA_INVALID;
    }
    const u64 tmd_at = Align64(Align64(Align64(FileSys::CIA_HEADER_SIZE) + cert) + tik);
    const u64 content_at = Align64(tmd_at + tmd);
    const u64 end = meta != 0 ? Align64(content_at + content) + meta : content_at + content;
    return end <= size ? EMUSUNI_3DS_CIA_OK : EMUSUNI_3DS_CIA_INVALID;
}

struct Facts {
    u64 title_id = 0;
    u64 file_bytes = 0;
    u64 install_bytes = 0;
    u16 version = 0;
    u32 contents = 0;
    bool encrypted = false;
    std::string name;
};

// What the CIA is (an encrypted one too: encrypted).
int Inspect(const std::string& path, Facts& facts) {
    const auto file = Open(path);
    if (!file)
        return EMUSUNI_3DS_CIA_IO;
    facts.file_bytes = file->GetSize();
    if (const int bad = CheckHeader(*file, facts.file_bytes); bad != EMUSUNI_3DS_CIA_OK)
        return bad;
    FileSys::CIAContainer container;
    if (container.Load(file.get()) != Loader::ResultStatus::Success)
        return EMUSUNI_3DS_CIA_INVALID;
    const FileSys::TitleMetadata& tmd = container.GetTitleMetadata();
    facts.title_id = tmd.GetTitleID();
    facts.version = tmd.GetTitleVersion();
    facts.install_bytes = tmd.GetCombinedContentSize(container.GetHeader());
    facts.contents = static_cast<u32>(tmd.GetContentCount());
    if (const auto& smdh = container.GetSMDH()) {
        for (const int language : {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) { // (English first)
            facts.name = Common::UTF16BufferToUTF8(
                smdh->GetShortTitle(static_cast<Loader::SMDH::TitleLanguage>(language)));
            if (!facts.name.empty())
                break;
        }
    }
    // Encrypted contents, as upstream's own check (CheckCIAToInstall) finds them - the TMD's flags, then each
    // content's NCCH header - but only the contents the CIA has: a DLC's pack may hold a part of its TMD's, and
    // upstream's reads every one's header (past the file's end: "damaged").
    if (tmd.HasEncryptedContent(container.GetHeader())) {
        facts.encrypted = true;
        return EMUSUNI_3DS_CIA_OK;
    }
    for (std::size_t i = 0; i < tmd.GetContentCount(); i++) {
        if (!container.GetHeader()->IsContentPresent(i))
            continue;
        NCCH_Header ncch{};
        if (file->ReadAtBytes(&ncch, sizeof(ncch), container.GetContentOffset(i)) != sizeof(ncch) ||
            ncch.magic != FileUtil::MakeMagic('N', 'C', 'C', 'H')) {
            return EMUSUNI_3DS_CIA_INVALID;
        }
        if (!ncch.no_crypto) {
            facts.encrypted = true;
            break;
        }
    }
    return EMUSUNI_3DS_CIA_OK;
}

// A title's TMD: the smallest of its content folder's, as upstream reads it, the names checked first.
bool ReadTmd(const std::string& content, FileSys::TitleMetadata& tmd) {
    FileUtil::FSTEntry entries;
    FileUtil::ScanDirectoryTree(content, entries);
    std::string first;
    for (const auto& entry : entries.children) {
        const std::string& name = entry.virtualName;
        if (entry.isDirectory || name.size() != 12 || name.compare(8, 4, ".tmd") != 0 ||
            name.find_first_not_of("0123456789abcdefABCDEF") < 8) {
            continue;
        }
        if (first.empty() || std::strtoul(name.c_str(), nullptr, 16) < std::strtoul(first.c_str(), nullptr, 16))
            first = name;
    }
    return !first.empty() && tmd.Load(content + first) == Loader::ResultStatus::Success;
}

void Tally(const FileUtil::FSTEntry& entry, u64& bytes, u32& apps) {
    for (const auto& child : entry.children) {
        if (child.isDirectory) {
            Tally(child, bytes, apps);
        } else {
            bytes += child.size;
            const std::string& name = child.virtualName;
            apps += name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0 ? 1 : 0;
        }
    }
}

// A folder's files copied into another (made), its subfolders too; false when one couldn't be.
bool CopyTree(const FileUtil::FSTEntry& entry, const std::string& from, const std::string& to) {
    if (!FileUtil::CreateFullPath(to))
        return false;
    for (const auto& child : entry.children) {
        const std::string source = from + child.virtualName, target = to + child.virtualName;
        if (child.isDirectory ? !CopyTree(child, source + "/", target + "/") : !FileUtil::Copy(source, target))
            return false;
    }
    return true;
}

// Every TMD name in a content folder upstream's own (8 hex digits): its std::stoul throws on another.
bool TmdNamesOk(const std::string& content) {
    FileUtil::FSTEntry entries;
    FileUtil::ScanDirectoryTree(content, entries);
    for (const auto& entry : entries.children) {
        const std::string& name = entry.virtualName;
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmd") == 0 &&
            (name.size() != 12 || name.find_first_not_of("0123456789abcdefABCDEF") < 8)) {
            return false;
        }
    }
    return true;
}

// The free space where the core's folder is, or -1 when it can't be read.
s64 FreeSpace(const std::string& dir) {
#if defined(__ORBIS__)
    int64_t bytes = 0;
    return emusuni_disk_space_known(dir.c_str(), &bytes) ? static_cast<s64>(bytes) : -1;
#else
    struct statvfs fs{};
    if (statvfs(dir.c_str(), &fs) != 0)
        return -1;
    return static_cast<s64>(fs.f_bavail) * static_cast<s64>(fs.f_frsize);
#endif
}

// A version an interrupted install moved aside (the app closed in the middle), put back first.
void Repair(const std::string& content, const std::string& kept) {
    if (!FileUtil::Exists(kept))
        return;
    if (FileUtil::Exists(content))
        FileUtil::DeleteDirRecursively(content);
    const bool back = FileUtil::Rename(Unslash(kept), Unslash(content));
    Say("EmuSuNi: CIA: %s: the version an interrupted install moved aside %s", Unslash(content).c_str(),
        back ? "put back" : "couldn't be put back");
}

// After a cancel, a failure or a rescue: what was written removed, the version before put back.
void PutBack() {
    if (g_job.title_id == 0)
        return;
    EmuSuNi::Throw::Rescue rescue{"putting a title's version back"};
    if (setjmp(rescue.env) != 0) {
        Say("EmuSuNi: CIA: %016llX: putting its version back stopped too (%s)",
            static_cast<unsigned long long>(g_job.title_id), EmuSuNi::Throw::RescuedMessage());
        g_job = Job{};
        return;
    }
    EmuSuNi::Throw::Arm(rescue);
    if (FileUtil::Exists(g_job.content))
        FileUtil::DeleteDirRecursively(g_job.content);
    if (g_job.had_old)
        FileUtil::Rename(Unslash(g_job.kept), Unslash(g_job.content));
    EmuSuNi::Throw::Disarm(rescue);
    g_job = Job{};
}

int InfoWork(const char* path, struct emusuni_3ds_cia_info* info) {
    Facts facts;
    const int result = Inspect(path, facts);
    info->title_id = facts.title_id;
    info->version = facts.version;
    info->encrypted = facts.encrypted ? 1 : 0;
    info->file_bytes = facts.file_bytes;
    info->install_bytes = facts.install_bytes;
    info->contents = facts.contents;
    std::size_t n = std::min(facts.name.size(), sizeof(info->name) - 1);
    while (n > 0 && n < facts.name.size() && (static_cast<unsigned char>(facts.name[n]) & 0xC0) == 0x80)
        n--; // (not inside a UTF-8 character)
    std::memcpy(info->name, facts.name.data(), n);
    info->name[n] = '\0';
    return result;
}

int InstallWork(const std::string& save, const char* path, emusuni_3ds_cia_progress progress, void* user,
                volatile int* cancel) {
    Facts facts;
    int result = Inspect(path, facts);
    const Kind kind = KindOf(facts.title_id);
    if (result == EMUSUNI_3DS_CIA_OK && facts.encrypted)
        result = EMUSUNI_3DS_CIA_ENCRYPTED;
    else if (result == EMUSUNI_3DS_CIA_OK && kind != UPDATE && kind != DLC)
        result = EMUSUNI_3DS_CIA_NOT_INSTALLED;
    if (result != EMUSUNI_3DS_CIA_OK) {
        Say("EmuSuNi: CIA: %s not installed (%d): %016llX, %s", path, result,
            static_cast<unsigned long long>(facts.title_id), KindName(kind));
        return result;
    }
    const std::string title = TitleDir(save, facts.title_id);
    if (Service::AM::GetTitlePath(MediaType::SDMC, facts.title_id) != title) {
        Say("EmuSuNi: CIA: %s not installed: upstream's title folder isn't %s", path, title.c_str());
        return EMUSUNI_3DS_CIA_IO;
    }
    const std::string content = title + "content/", kept = title + KEPT;
    Repair(content, kept);
    // An update replaces the version there: moved aside. A DLC's pieces add up (a CIA per pack, the same title ID):
    // upstream writes the new ones beside those there, which are copied aside instead.
    const bool had_old = FileUtil::Exists(content);
    FileUtil::FSTEntry old_tree;
    u64 old_bytes = 0;
    u32 old_apps = 0;
    if (had_old && kind == DLC) {
        if (!TmdNamesOk(content)) {
            Say("EmuSuNi: CIA: %s not installed: a TMD of %s isn't named as upstream names them", path,
                Unslash(content).c_str());
            return EMUSUNI_3DS_CIA_IO;
        }
        FileUtil::ScanDirectoryTree(content, old_tree, 8);
        Tally(old_tree, old_bytes, old_apps);
    }
    const s64 free_bytes = FreeSpace(save + "Azahar/");
    const u64 need = facts.install_bytes + old_bytes + (32ULL << 20);
    if (free_bytes >= 0 && static_cast<u64>(free_bytes) < need) {
        Say("EmuSuNi: CIA: %s not installed: %llu MiB needed, %lld MiB free", path,
            static_cast<unsigned long long>(need >> 20), static_cast<long long>(free_bytes >> 20));
        return EMUSUNI_3DS_CIA_IO;
    }
    Say("EmuSuNi: CIA: installing %s: %016llX \"%s\" (%s, %s, %u contents), %llu MiB into the emulated SD card "
        "(%lld MiB free)%s",
        path, static_cast<unsigned long long>(facts.title_id), facts.name.c_str(), KindName(kind),
        Version(facts.version).c_str(), facts.contents, static_cast<unsigned long long>(facts.install_bytes >> 20),
        static_cast<long long>(free_bytes >= 0 ? free_bytes >> 20 : -1),
        !had_old       ? ""
        : kind == DLC ? "; the DLC there (its pieces) copied aside until it's done"
                      : "; the version there moved aside until it's done");
    if (had_old && !(kind == DLC ? CopyTree(old_tree, content, kept)
                                 : FileUtil::Rename(Unslash(content), Unslash(kept)))) {
        if (kind == DLC && FileUtil::Exists(kept))
            FileUtil::DeleteDirRecursively(kept);
        Say("EmuSuNi: CIA: %s not installed: what is there couldn't be %s aside", path,
            kind == DLC ? "copied" : "moved");
        return EMUSUNI_3DS_CIA_IO;
    }
    g_job.title_id = facts.title_id;
    g_job.content = content;
    g_job.kept = kept;
    g_job.had_old = had_old;
    const auto start = std::chrono::steady_clock::now();
    const auto file = Open(path);
    FileSys::CIAContainer container;
    if (!file || container.Load(file.get()) != Loader::ResultStatus::Success) {
        PutBack();
        return EMUSUNI_3DS_CIA_IO;
    }
    file->Seek(0, SEEK_SET);
    {
        Service::AM::CIAFile install(Core::System::GetInstance(),
                                     Service::AM::GetTitleMediaType(facts.title_id));
        std::vector<u8> buffer(0x10000);
        const u64 size = file->GetSize();
        u64 done = 0;
        while (done < size) {
            if (cancel != nullptr && *cancel != 0) {
                result = EMUSUNI_3DS_CIA_CANCELLED;
                break;
            }
            EmuSuNi::Throw::TestThrow(); // (the PC tests' rescue: EMUSUNI_3DS_TEST_THROW=<n>, at the n-th block)
            const std::size_t n = file->ReadBytes(buffer.data(), buffer.size());
            if (n == 0 || install.Write(done, n, true, false, buffer.data()).Failed()) {
                result = EMUSUNI_3DS_CIA_IO;
                break;
            }
            done += n;
            if (progress != nullptr)
                progress(done, size, user);
        }
        install.Close(); // (cut short: upstream's own clean-up of the new folder)
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (result != EMUSUNI_3DS_CIA_OK) {
        PutBack();
        Say("EmuSuNi: CIA: %016llX not installed (%s) after %.1f s%s", static_cast<unsigned long long>(facts.title_id),
            result == EMUSUNI_3DS_CIA_CANCELLED ? "cancelled" : "a read or write failed", seconds,
            had_old ? ": the version before put back" : "");
        return result;
    }
    if (had_old)
        FileUtil::DeleteDirRecursively(kept);
    g_job = Job{};
    Say("EmuSuNi: CIA: %016llX installed in %.1f s", static_cast<unsigned long long>(facts.title_id), seconds);
    return EMUSUNI_3DS_CIA_OK;
}

int InstalledWork(const std::string& save, u64 game, struct emusuni_3ds_installed* out) {
    const u64 low = game & 0xFFFFFFFFULL;
    FileSys::TitleMetadata tmd;
    const std::string update = TitleDir(save, 0x0004000E00000000ULL | low) + "content/";
    if (ReadTmd(update, tmd)) {
        FileUtil::FSTEntry tree;
        FileUtil::ScanDirectoryTree(update, tree, 8);
        u32 apps = 0;
        out->has_update = 1;
        out->update_version = tmd.GetTitleVersion();
        Tally(tree, out->update_bytes, apps);
    }
    const std::string dlc = TitleDir(save, 0x0004008C00000000ULL | low) + "content/";
    if (ReadTmd(dlc, tmd)) {
        FileUtil::FSTEntry tree;
        FileUtil::ScanDirectoryTree(dlc, tree, 8);
        out->dlc_titles = 1;
        Tally(tree, out->dlc_bytes, out->dlc_contents);
    }
    return EMUSUNI_3DS_CIA_OK;
}

int UninstallWork(const std::string& save, u64 title_id) {
    const std::string title = TitleDir(save, title_id);
    const std::string content = title + "content/", kept = title + KEPT;
    if (FileUtil::Exists(kept))
        FileUtil::DeleteDirRecursively(kept);
    if (!FileUtil::Exists(content))
        return EMUSUNI_3DS_CIA_OK;
    const bool removed = FileUtil::DeleteDirRecursively(content);
    Say("EmuSuNi: CIA: %016llX (%s) %s", static_cast<unsigned long long>(title_id), KindName(KindOf(title_id)),
        removed ? "removed from the emulated SD card (the game's save data kept)" : "couldn't be removed");
    return removed ? EMUSUNI_3DS_CIA_OK : EMUSUNI_3DS_CIA_IO;
}

} // namespace

extern "C" int emusuni_3ds_cia_version(void) {
    return EMUSUNI_3DS_CIA_VERSION;
}

extern "C" int emusuni_3ds_cia_info(const char* path, struct emusuni_3ds_cia_info* info) {
    if (info == nullptr)
        return EMUSUNI_3DS_CIA_IO;
    std::memset(info, 0, sizeof(*info));
    if (path == nullptr || path[0] == '\0')
        return EMUSUNI_3DS_CIA_IO;
    EmuSuNi::Throw::Rescue rescue{"reading a CIA"};
    if (setjmp(rescue.env) != 0) {
        Say("EmuSuNi: CIA: reading %s stopped on a C++ exception (%s)", path, EmuSuNi::Throw::RescuedMessage());
        return EMUSUNI_3DS_CIA_CRASHED;
    }
    EmuSuNi::Throw::Arm(rescue);
    const int result = InfoWork(path, info);
    EmuSuNi::Throw::Disarm(rescue);
    return result;
}

extern "C" int emusuni_3ds_cia_install(const char* save_dir, const char* path, emusuni_3ds_cia_progress progress,
                                       void* user, volatile int* cancel) {
    if (save_dir == nullptr || save_dir[0] == '\0' || path == nullptr || path[0] == '\0')
        return EMUSUNI_3DS_CIA_IO;
    const Gate gate;
    if (gate.refused != EMUSUNI_3DS_CIA_OK)
        return gate.refused;
    const std::string save = SaveDir(save_dir);
    // (Upstream's installer writes where FileUtil's user path says: the core's own folder, as a game's load sets it
    // - core_settings.cpp, ParseStorageOptions.)
    if (!FileUtil::CreateFullPath(save + "Azahar/"))
        return EMUSUNI_3DS_CIA_IO;
    FileUtil::SetUserPath(save + "Azahar/");
    EmuSuNi::Throw::Rescue rescue{"installing a CIA"};
    if (setjmp(rescue.env) != 0) {
        Say("EmuSuNi: CIA: installing %s stopped on a C++ exception (%s): the version before put back", path,
            EmuSuNi::Throw::RescuedMessage());
        PutBack();
        return EMUSUNI_3DS_CIA_CRASHED;
    }
    EmuSuNi::Throw::Arm(rescue);
    const int result = InstallWork(save, path, progress, user, cancel);
    EmuSuNi::Throw::Disarm(rescue);
    return result;
}

extern "C" int emusuni_3ds_installed(const char* save_dir, uint64_t game_title_id,
                                     struct emusuni_3ds_installed* out) {
    if (out == nullptr)
        return EMUSUNI_3DS_CIA_IO;
    std::memset(out, 0, sizeof(*out));
    if (save_dir == nullptr || save_dir[0] == '\0')
        return EMUSUNI_3DS_CIA_IO;
    EmuSuNi::Throw::Rescue rescue{"reading what is installed"};
    if (setjmp(rescue.env) != 0) {
        Say("EmuSuNi: CIA: reading what is installed for %016llX stopped on a C++ exception (%s)",
            static_cast<unsigned long long>(game_title_id), EmuSuNi::Throw::RescuedMessage());
        return EMUSUNI_3DS_CIA_CRASHED;
    }
    EmuSuNi::Throw::Arm(rescue);
    const int result = InstalledWork(SaveDir(save_dir), game_title_id, out);
    EmuSuNi::Throw::Disarm(rescue);
    return result;
}

extern "C" int emusuni_3ds_uninstall(const char* save_dir, uint64_t title_id) {
    const Kind kind = KindOf(title_id);
    if (kind != UPDATE && kind != DLC)
        return EMUSUNI_3DS_CIA_NOT_INSTALLED;
    if (save_dir == nullptr || save_dir[0] == '\0')
        return EMUSUNI_3DS_CIA_IO;
    const Gate gate;
    if (gate.refused != EMUSUNI_3DS_CIA_OK)
        return gate.refused;
    EmuSuNi::Throw::Rescue rescue{"removing a title"};
    if (setjmp(rescue.env) != 0) {
        Say("EmuSuNi: CIA: removing %016llX stopped on a C++ exception (%s)",
            static_cast<unsigned long long>(title_id), EmuSuNi::Throw::RescuedMessage());
        return EMUSUNI_3DS_CIA_CRASHED;
    }
    EmuSuNi::Throw::Arm(rescue);
    const int result = UninstallWork(SaveDir(save_dir), title_id);
    EmuSuNi::Throw::Disarm(rescue);
    return result;
}
#endif
''')
addfile('src/common/emusuni_mods.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.09).
// The app's Mods option for the game (citra_emusuni_mods, read with the core's options at the game's load). Off:
// NCCHContainer takes none of the game's mods - its folder load/mods/<title ID>/ (LayeredFS's romfs/ and romfs_ext/,
// exefs/ and code patches, exheader.bin), Luma's code.ips in the emulated SD card, the files beside the ROM
// (.exheader, .exefsdir/, .romfs). On (upstream's way): whatever of those is there.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>

namespace EmuSuNi {
inline std::atomic<bool> g_mods_off{false};
inline bool ModsOff() {
    return g_mods_off.load(std::memory_order_relaxed);
}
} // namespace EmuSuNi
#endif
''')
patch('src/citra_libretro/CMakeLists.txt',
      '        core_settings.cpp\n'
      '        core_settings.h)\n',
      '        core_settings.cpp\n'
      '        core_settings.h\n'
      '        emusuni_cia.cpp\n'
      '        emusuni_cia.h)\n')
patch('src/citra_libretro/core_settings.cpp',
      '#include "common/settings.h"\n',
      '#include "common/settings.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mods.h"\n#endif\n')
patch('src/citra_libretro/core_settings.cpp',
      '    citra_setting(BaseKeys::use_libretro_save_path);\n} // namespace storage\n',
      '    citra_setting(BaseKeys::use_libretro_save_path);\n'
      '// (01.09, EmuSuNi\'s: the game\'s mods, taken by default as upstream)\n'
      'static constexpr const char* emusuni_mods = "citra_emusuni_mods";\n'
      '} // namespace storage\n')
patch('src/citra_libretro/core_settings.cpp',
      '        "LibRetro Default"\n'
      '    },\n'
      '\n'
      '    // Input Category\n',
      '        "LibRetro Default"\n'
      '    },\n'
      '    {\n'
      '        config::storage::emusuni_mods,\n'
      '        "Mods (EmuSuNi)",\n'
      '        "Mods",\n'
      '        "Load the game\'s mods: load/mods/<title ID>/ (LayeredFS, code patches), the files beside the ROM.",\n'
      '        nullptr,\n'
      '        config::category::storage,\n'
      '        {\n'
      '            { config::enabled, "Enabled" },\n'
      '            { config::disabled, "Disabled" },\n'
      '            { nullptr, nullptr }\n'
      '        },\n'
      '        config::enabled\n'
      '    },\n'
      '\n'
      '    // Input Category\n')
patch('src/citra_libretro/core_settings.cpp',
      'static void ParseStorageOptions(void) {\n',
      'static void ParseStorageOptions(void) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.09): the app\'s Mods option for this game (common/emusuni_mods.h).\n'
      '    EmuSuNi::g_mods_off.store(LibRetro::FetchVariable(config::storage::emusuni_mods, config::enabled) ==\n'
      '                              config::disabled);\n'
      '#endif\n')
patch('src/core/file_sys/ncch_container.cpp',
      '#include "core/file_sys/layered_fs.h"\n',
      '#include "core/file_sys/layered_fs.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mods.h"\n#endif\n')
patch('src/core/file_sys/ncch_container.cpp',
      '            bool has_exheader_override = false;\n'
      '            for (const auto& path : exheader_override_paths) {\n',
      '            bool has_exheader_override = false;\n'
      '#ifdef EMUSUNI_PORT\n'
      '            // EmuSuNi (01.09): none with the app\'s Mods option off (common/emusuni_mods.h).\n'
      '            if (!EmuSuNi::ModsOff())\n'
      '#endif\n'
      '            for (const auto& path : exheader_override_paths) {\n')
patch('src/core/file_sys/ncch_container.cpp',
      'Loader::ResultStatus NCCHContainer::ApplyCodePatch(std::vector<u8>& code) const {\n',
      'Loader::ResultStatus NCCHContainer::ApplyCodePatch(std::vector<u8>& code) const {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (EmuSuNi::ModsOff())\n'
      '        return Loader::ResultStatus::ErrorNotUsed; // (EmuSuNi, 01.09: the app\'s Mods option off)\n'
      '#endif\n')
patch('src/core/file_sys/ncch_container.cpp',
      'Loader::ResultStatus NCCHContainer::LoadOverrideExeFSSection(const char* name,\n'
      '                                                             std::vector<u8>& buffer) {\n',
      'Loader::ResultStatus NCCHContainer::LoadOverrideExeFSSection(const char* name,\n'
      '                                                             std::vector<u8>& buffer) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (EmuSuNi::ModsOff())\n'
      '        return Loader::ResultStatus::ErrorNotUsed; // (EmuSuNi, 01.09: the app\'s Mods option off)\n'
      '#endif\n')
patch('src/core/file_sys/ncch_container.cpp',
      '    int block_size = is_proto ? 1 : kBlockSize;\n'
      '\n'
      '    if (ReadOverrideRomFS(romfs_file) == Loader::ResultStatus::Success)\n'
      '        return Loader::ResultStatus::Success;\n',
      '    int block_size = is_proto ? 1 : kBlockSize;\n'
      '\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.09): with the app\'s Mods option off, neither the .romfs beside the ROM nor LayeredFS.\n'
      '    if (EmuSuNi::ModsOff())\n'
      '        use_layered_fs = false;\n'
      '    else if (ReadOverrideRomFS(romfs_file) == Loader::ResultStatus::Success)\n'
      '        return Loader::ResultStatus::Success;\n'
      '#else\n'
      '    if (ReadOverrideRomFS(romfs_file) == Loader::ResultStatus::Success)\n'
      '        return Loader::ResultStatus::Success;\n'
      '#endif\n')
patch('src/core/loader/ncch.cpp',
      '#include "core/loader/smdh.h"\n',
      '#include "core/loader/smdh.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_mods.h"\n#endif\n')
patch('src/core/loader/ncch.cpp',
      '        if (result == ResultStatus::Success) {\n'
      '            overlay_ncch = &update_ncch;\n'
      '        }\n'
      '    }\n',
      '        if (result == ResultStatus::Success) {\n'
      '            overlay_ncch = &update_ncch;\n'
      '        }\n'
      '    }\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // EmuSuNi (01.09): what the game takes from the emulated SD card and its mods folder, said at its load.\n'
      '    {\n'
      '        const auto emusuni_installed = [](u64 title_id) -> std::string {\n'
      '            FileSys::TitleMetadata tmd;\n'
      '            const std::string path =\n'
      '                Service::AM::GetTitleMetadataPath(Service::FS::MediaType::SDMC, title_id, false);\n'
      '            if (path.empty() || !FileUtil::Exists(path) || tmd.Load(path) != ResultStatus::Success)\n'
      '                return "none";\n'
      '            return fmt::format("{:016X} v{}", title_id, tmd.GetTitleVersion());\n'
      '        };\n'
      '        const u64 emusuni_low = ncch_program_id & 0xFFFFFFFFULL;\n'
      '        const std::string emusuni_mods = fmt::format(\n'
      '            "{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir), ncch_program_id);\n'
      '        LOG_INFO(Loader, "EmuSuNi: the game\'s update: {}; its DLC: {}; its mods ({}): {}",\n'
      '                 overlay_ncch == &update_ncch ? emusuni_installed(emusuni_low | 0x0004000E00000000ULL)\n'
      '                                              : std::string("none"),\n'
      '                 emusuni_installed(emusuni_low | 0x0004008C00000000ULL), emusuni_mods,\n'
      '                 !FileUtil::Exists(emusuni_mods) ? "none"\n'
      '                 : EmuSuNi::ModsOff()            ? "there, left alone (the app\'s Mods option off)"\n'
      '                                                 : "used");\n'
      '    }\n'
      '#endif\n')

# --- 01.09: the threads' study's measures (the 3DS specialist's 3ds-hilos-estudio.md: what more threads would gain).
# Two lines after the draws' one, every 600 frames: each ARM11 core's runs (does core 1 do anything worth a thread of
# its own?), the Vulkan worker's busy time (has core 3 room?), the GSP's GPU commands by kind (lists, fills,
# transfers, DMA; the lists' immediate-mode words), and the gaps an asynchronous GPU could fill, each counted up to
# its list's own time: from a command list's end to the next list's start (what a GPU thread would take off this one
# if the game let each list end before sending the next), and to the game's next wait on GSP's interrupt event (if
# the game waited there for each list; a game's own thread for GSP's interrupts may wait there at once). All the
# lists' time is the ceiling. Ticks of the time stamp counter, as the 600-frame line's; none of the shader cache's
# hashed files.
patch('src/common/emusuni_stats.h',
      '    DrawRecordTicks,\n    TickCount\n};\n',
      '    DrawRecordTicks,\n'
      '    // (01.09) The threads\' study: the ARM11 cores\' runs, the GSP\'s GPU commands by kind, the gaps an\n'
      '    // asynchronous GPU could fill (common/emusuni_frame_stats.h says them every 600 frames).\n'
      '    Core0Ticks, CoreOtherTicks, GxListTicks, GxFillTicks, GxTransferTicks, GxDmaTicks, GxOtherTicks,\n'
      '    ListToListTicks, AsyncUpTicks, ListToWaitTicks, AsyncLowTicks,\n'
      '    TickCount\n};\n')
patch('src/common/emusuni_stats.h',
      '    CommandLists, // (00.95) the PICA command lists run (vertex data is copied once per list)\n'
      '    CountCount\n};\n',
      '    CommandLists, // (00.95) the PICA command lists run (vertex data is copied once per list)\n'
      '    ImmediateWords, ListGaps, GspWaits, // (01.09) the threads\' study\n'
      '    CountCount\n};\n')
patch('src/common/emusuni_stats.h',
      '} // namespace EmuSuNi::Stats\n#endif\n',
      '// (01.09) The threads\' study: GPU::SubmitCmdList\'s lists, their gaps, and the game\'s waits on GSP\'s\n'
      '// interrupt event (all on the emulation thread: plain values). Each gap counts at most its list\'s own time:\n'
      '// what a GPU thread could take off this one.\n'
      'inline const void* gsp_event = nullptr; // (GSP\'s interrupt event, as a kernel WaitObject)\n'
      'inline std::uint64_t list_end = 0, list_ticks = 0;\n'
      'inline bool wait_pending = false;\n'
      'inline void ListStarted(std::uint64_t now) {\n'
      '    if (list_end != 0) {\n'
      '        const std::uint64_t gap = now - list_end;\n'
      '        ticks[ListToListTicks] += gap;\n'
      '        ticks[AsyncUpTicks] += gap < list_ticks ? gap : list_ticks;\n'
      '        counts[ListGaps]++;\n'
      '    }\n'
      '}\n'
      'inline void ListEnded(std::uint64_t start, std::uint64_t now) {\n'
      '    list_ticks = now - start;\n'
      '    list_end = now;\n'
      '    wait_pending = true;\n'
      '}\n'
      'inline void GspWait(const void* object) {\n'
      '    if (!wait_pending || object == nullptr || object != gsp_event)\n'
      '        return;\n'
      '    const std::uint64_t gap = Ticks() - list_end;\n'
      '    ticks[ListToWaitTicks] += gap;\n'
      '    ticks[AsyncLowTicks] += gap < list_ticks ? gap : list_ticks;\n'
      '    counts[GspWaits]++;\n'
      '    wait_pending = false;\n'
      '}\n'
      '// The Vulkan worker\'s busy time (its own thread: an atomic).\n'
      'inline std::atomic<std::uint64_t> worker_ticks{0};\n'
      '} // namespace EmuSuNi::Stats\n#endif\n')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp',
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::GuestTicks};\n#endif\n    jit->Run();\n',
      '    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::GuestTicks};\n'
      '    // (01.09) The threads\' study: each ARM11 core\'s runs (their JIT\'s, HLE calls\' and GPU commands\'\n'
      '    // time in them too).\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_core{GetID() == 0 ? EmuSuNi::Stats::Core0Ticks\n'
      '                                                              : EmuSuNi::Stats::CoreOtherTicks};\n'
      '#endif\n    jit->Run();\n')
patch('src/video_core/gpu.cpp',
      '    const EmuSuNi::Stats::GpuTimer emusuni_timer;\n#endif\n    using Service::GSP::CommandId;\n',
      '    const EmuSuNi::Stats::GpuTimer emusuni_timer;\n#endif\n    using Service::GSP::CommandId;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (01.09) The threads\' study: the GSP\'s GPU commands by kind.\n'
      '    const EmuSuNi::Stats::TickTimer emusuni_kind{\n'
      '        command.id == CommandId::SubmitCmdList ? EmuSuNi::Stats::GxListTicks\n'
      '        : command.id == CommandId::MemoryFill  ? EmuSuNi::Stats::GxFillTicks\n'
      '        : command.id == CommandId::DisplayTransfer || command.id == CommandId::TextureCopy\n'
      '            ? EmuSuNi::Stats::GxTransferTicks\n'
      '        : command.id == CommandId::RequestDma ? EmuSuNi::Stats::GxDmaTicks\n'
      '                                              : EmuSuNi::Stats::GxOtherTicks};\n'
      '#endif\n')
patch('src/video_core/gpu.cpp',
      '    impl->pica.ProcessCmdList(addr, size,\n'
      '                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));\n'
      '    config.trigger[index] = 0;\n',
      '#ifdef EMUSUNI_PORT\n'
      '    const std::uint64_t emusuni_start = EmuSuNi::Stats::Ticks(); // (01.09: the threads\' study)\n'
      '    EmuSuNi::Stats::ListStarted(emusuni_start);\n'
      '#endif\n'
      '    impl->pica.ProcessCmdList(addr, size,\n'
      '                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Stats::ListEnded(emusuni_start, EmuSuNi::Stats::Ticks());\n'
      '#endif\n'
      '    config.trigger[index] = 0;\n')
patch('src/core/hle/service/gsp/gsp_gpu.cpp',
      '#include "core/hle/service/gsp/gsp_gpu.h"\n',
      '#include "core/hle/service/gsp/gsp_gpu.h"\n'
      '#ifdef EMUSUNI_PORT\n#include "common/emusuni_stats.h"\n#endif\n')
patch('src/core/hle/service/gsp/gsp_gpu.cpp',
      '    session_data->interrupt_event = std::move(interrupt_event);\n'
      '    session_data->registered = true;\n',
      '    session_data->interrupt_event = std::move(interrupt_event);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    // (01.09) The threads\' study: the game\'s waits on this event (svc.cpp); its first list\'s gap\n'
      '    // not the game before\'s.\n'
      '    EmuSuNi::Stats::gsp_event = static_cast<const Kernel::WaitObject*>(session_data->interrupt_event.get());\n'
      '    EmuSuNi::Stats::list_end = 0;\n'
      '    EmuSuNi::Stats::wait_pending = false;\n'
      '#endif\n'
      '    session_data->registered = true;\n')
patch('src/core/hle/service/gsp/gsp_gpu.cpp',
      '    SessionData* session_data = GetSessionData(ctx.Session());\n'
      '    session_data->interrupt_event = nullptr;\n'
      '    session_data->registered = false;\n',
      '    SessionData* session_data = GetSessionData(ctx.Session());\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if (EmuSuNi::Stats::gsp_event ==\n'
      '        static_cast<const Kernel::WaitObject*>(session_data->interrupt_event.get())) {\n'
      '        EmuSuNi::Stats::gsp_event = nullptr;\n'
      '    }\n'
      '#endif\n'
      '    session_data->interrupt_event = nullptr;\n'
      '    session_data->registered = false;\n')
patch('src/core/hle/kernel/svc.cpp',
      '    LOG_TRACE(Kernel_SVC, "called handle=0x{:08X}({}:{}), nanoseconds={}", handle,\n'
      '              object->GetTypeName(), object->GetName(), nano_seconds);\n',
      '    LOG_TRACE(Kernel_SVC, "called handle=0x{:08X}({}:{}), nanoseconds={}", handle,\n'
      '              object->GetTypeName(), object->GetName(), nano_seconds);\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Stats::GspWait(object.get()); // (01.09: the threads\' study)\n'
      '#endif\n')
patch('src/core/hle/kernel/svc.cpp',
      '        auto object = kernel.GetCurrentProcess()->handle_table.Get<WaitObject>(handle);\n'
      '        R_UNLESS(object, ResultInvalidHandle);\n'
      '        objects[i] = object;\n',
      '        auto object = kernel.GetCurrentProcess()->handle_table.Get<WaitObject>(handle);\n'
      '        R_UNLESS(object, ResultInvalidHandle);\n'
      '#ifdef EMUSUNI_PORT\n'
      '        EmuSuNi::Stats::GspWait(object.get()); // (01.09: the threads\' study)\n'
      '#endif\n'
      '        objects[i] = object;\n')
patch('src/video_core/renderer_vulkan/vk_scheduler.cpp',
      '            const bool has_submit = work->HasSubmit();\n'
      '            work->ExecuteAll(current_cmdbuf);\n',
      '            const bool has_submit = work->HasSubmit();\n'
      '#ifdef EMUSUNI_PORT\n'
      '            const std::uint64_t emusuni_k0 = EmuSuNi::Stats::Ticks(); // (01.09: the threads\' study)\n'
      '#endif\n'
      '            work->ExecuteAll(current_cmdbuf);\n'
      '#ifdef EMUSUNI_PORT\n'
      '            EmuSuNi::Stats::worker_ticks.fetch_add(EmuSuNi::Stats::Ticks() - emusuni_k0,\n'
      '                                                   std::memory_order_relaxed);\n'
      '#endif\n')
patch('src/video_core/pica/pica_core.cpp',
      'void PicaCore::SubmitImmediate(u32 value) {\n',
      'void PicaCore::SubmitImmediate(u32 value) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    EmuSuNi::Stats::counts[EmuSuNi::Stats::ImmediateWords]++; // (01.09: the threads\' study)\n'
      '#endif\n')
patch('src/common/emusuni_frame_stats.h',
      '                     part_ms(dt[DrawTicks] > parts ? dt[DrawTicks] - parts : 0));\n'
      '            period_t0 = now_us;\n',
      '                     part_ms(dt[DrawTicks] > parts ? dt[DrawTicks] - parts : 0));\n'
      '            // (01.09) The threads\' study (3ds-hilos-estudio.md), in two lines (the log cuts long ones).\n'
      '            const std::uint64_t gx = dt[GxListTicks] + dt[GxFillTicks] + dt[GxTransferTicks] +\n'
      '                                     dt[GxDmaTicks] + dt[GxOtherTicks];\n'
      '            static std::uint64_t worker_at = 0;\n'
      '            const std::uint64_t worker = worker_ticks.load(std::memory_order_relaxed);\n'
      '            LOG_INFO(Frontend,\n'
      '                     "EmuSuNi: the threads of those 600 frames: ARM11 core 0\'s runs {} ms, the other "\n'
      '                     "cores\' {} ms; the Vulkan worker busy {} ms; GSP\'s GPU commands: lists {} ms ({}, "\n'
      '                     "{} KiB, {} words of immediate mode), fills {} ms, transfers {} ms, DMA {} ms, "\n'
      '                     "others {} ms; the rest {} ms",\n'
      '                     part_ms(dt[Core0Ticks]), part_ms(dt[CoreOtherTicks]), part_ms(worker - worker_at),\n'
      '                     part_ms(dt[GxListTicks]), dc[CommandLists], dc[ListBytes] / 1024, dc[ImmediateWords],\n'
      '                     part_ms(dt[GxFillTicks]), part_ms(dt[GxTransferTicks]), part_ms(dt[GxDmaTicks]),\n'
      '                     part_ms(dt[GxOtherTicks]), part_ms(dt[GpuTicks] > gx ? dt[GpuTicks] - gx : 0));\n'
      '            worker_at = worker;\n'
      '            LOG_INFO(Frontend,\n'
      '                     "EmuSuNi: an asynchronous GPU beside those 600 frames: from a command list\'s end to "\n'
      '                     "the next list {} ms ({} gaps; each list ended before the next: {} ms off this "\n'
      '                     "thread), to the game\'s next wait on GSP\'s event {} ms ({} waits; the game waiting "\n'
      '                     "there: {} ms off)",\n'
      '                     part_ms(dt[ListToListTicks]), dc[ListGaps], part_ms(dt[AsyncUpTicks]),\n'
      '                     part_ms(dt[ListToWaitTicks]), dc[GspWaits], part_ms(dt[AsyncLowTicks]));\n'
      '            period_t0 = now_us;\n')

# --- 01.10: the GPU's commands and the audio's mixing on threads of their own, each behind a test file read at each
# game's start (off without it: the work stays on the emulation thread, as before): /data/EmuSuNi/3ds-async-pica
# (EMUSUNI_3DS_ASYNC_PICA=1 on the PC) and /data/EmuSuNi/3ds-async-dsp (EMUSUNI_3DS_ASYNC_DSP=1). The 3DS specialist's
# 3ds-hilos-estudio.md (options A and C) and 3ds-hilos-gpu-audio.md.
# - common/emusuni_async.h: what the workers finish comes back to the emulation thread, which acts on it at each slice's
#   start (System::RunLoop), drains a worker (waits for all its work, then takes it) before it touches what the worker
#   uses, and waits for a worker when no ARM11 core has a thread to run while it works (the game waits for it: the
#   emulated time doesn't run on without its work, as it doesn't with the work on the emulation thread).
# - video_core/emusuni_async_pica.h: the GSP's command lists, fills, display transfers and texture copies, in order, on
#   their thread ("PICA <core>": the app places it, port/ps4_n64.c; the file's number, 0 by default). The pages its
#   cache marks and its interrupts are the emulation thread's to change and signal: marks first, then interrupts. A DMA
#   (the CPU's copy) after the queue; the emulation thread drains it at the VBlank, the GPU's registers, a buffer swap,
#   the CPU on a page the GPU's cache holds, the GPU's right changing hands, a save state, a setting or the renderer
#   changing. Automatic unless the file says "always": every 60 frames its busy time against the emulation thread's
#   waits for it; less than a tenth not waited for, 600 frames inline (on the emulation thread, as before), then 60
#   there again (the PC: Luigi's Mansion: Dark Moon 15-20% faster with it; Donkey Kong Country Returns, Captain Toad
#   and Castlevania wait for each command at once and lost 4-17% with it always on: inline, as fast as before).
# - The DSP's mixing ("DSP HLE <core>", 1 by default): started at each audio frame's tick, its frame to the sink and its
#   interrupt when the emulation thread takes it; drained at the next tick, a pipe's write, a save state; mixed by the
#   emulation thread when its own thread hasn't started it yet and nothing else can run. Automatic too: inline while a
#   frame's mixing takes less than 25 us (the hand-over's cost; the PC: about 7 us).
# - The physical memory regions' lookup (memory.cpp) without upstream's cache of the last one, which every thread wrote
#   and read: the new threads look regions up at once with the emulation thread (Dark Moon crashed every other run).
# - Save states: the same format (both drained first: nothing of theirs is saved); a C++ exception on either thread
#   ends the game as on the emulation thread. Every 600 frames a line says their time and the emulation's waits.
addfile('src/common/emusuni_async.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.10).
// Two of the emulation thread's jobs on threads of their own, each behind a test file read at each game's start: the
// GSP's GPU commands (the PICA's: video_core/emusuni_async_pica.h, the file /data/EmuSuNi/3ds-async-pica) and the
// audio DSP's mixing (audio_core/hle/hle.cpp, /data/EmuSuNi/3ds-async-dsp).
// What they finish comes back to the emulation thread (the GPU's interrupts and the pages its cache marks; the DSP's
// frame of sound and its interrupt), which acts on it at the start of each of its slices (System::RunLoop), and waits
// for a worker:
//   - before it touches what that worker uses (a drain: the GPU's state, a page the GPU's cache holds, a save state,
//     a setting);
//   - when no ARM11 core has a thread to run while a worker works: the game waits for it, and the emulated time
//     doesn't run on without its work, as it doesn't when the work is on the emulation thread.
// Without the files nothing here runs: the work stays on the emulation thread, as before.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <immintrin.h>
#include "common/emusuni_stats.h"

namespace EmuSuNi::Async {
// A wait spins a little before it sleeps: the other thread's answer often comes within tens of microseconds, and a
// sleep and its wake-up cost as much (the PS4's TSC runs at 1.6 GHz: 100,000 ticks are 62 us; EMUSUNI_3DS_ASYNC_SPIN
// on the PC: the ticks, 0 none).
inline std::uint64_t SpinTicks() {
#if defined(__ORBIS__)
    return 100000;
#else
    static const std::uint64_t ticks = std::getenv("EMUSUNI_3DS_ASYNC_SPIN") != nullptr
                                           ? std::strtoull(std::getenv("EMUSUNI_3DS_ASYNC_SPIN"), nullptr, 10)
                                           : 100000;
    return ticks;
#endif
}
template <typename Ready>
inline bool Spin(Ready ready) {
    const std::uint64_t ticks = SpinTicks();
    if (ticks == 0) {
        return ready();
    }
    const std::uint64_t until = Stats::Ticks() + ticks;
    do {
        if (ready()) {
            return true;
        }
        _mm_pause();
    } while (Stats::Ticks() < until);
    return ready();
}
// Why the emulation thread drained a worker (the 600-frame line says each one's waits).
enum DrainWhy { DrainVBlank, DrainMemory, DrainGsp, DrainDma, DrainOther, DrainWhyCount };

// A worker, as the emulation thread sees it (all its calls on that thread).
class Worker {
public:
    virtual ~Worker() = default;
    // Work given to it and not acted on yet: queued, running, or its results not taken.
    virtual bool Busy() const = 0;
    // What it finished, taken and acted on.
    virtual void Deliver() = 0;
    // All its work finished (waited for), then Deliver.
    virtual void Drain(DrainWhy why) = 0;
    // Work its thread hasn't started run here instead (the emulation thread has nothing else to do: a sleeping thread
    // would take longer to wake than to do it), and acted on; true when there was some.
    virtual bool Steal() {
        return false;
    }
    // Its thread stopped on a C++ exception: thrown here (System::RunLoop's slice: the game ends as it would have
    // with the work on this thread).
    virtual void Check() {}
};
enum Slot { PicaSlot, DspSlot, SlotCount };
inline Worker* workers[SlotCount] = {};

// Pieces of work finished and not taken yet: a worker adds one for each (under the lock, then the notice).
inline std::atomic<unsigned> results{0};
inline std::mutex results_mutex;
inline std::condition_variable results_cv;

inline void Published() {
    {
        std::lock_guard lock{results_mutex};
        results.fetch_add(1, std::memory_order_release);
    }
    results_cv.notify_all();
}

inline bool AnyBusy() {
    for (const Worker* worker : workers) {
        if (worker != nullptr && worker->Busy()) {
            return true;
        }
    }
    return false;
}

// System::RunLoop, at each slice's start: what the workers finished is acted on (a load when nothing is).
inline void Poll() {
    if (results.load(std::memory_order_acquire) == 0) {
        return;
    }
    results.exchange(0, std::memory_order_acq_rel);
    for (Worker* worker : workers) {
        if (worker != nullptr) {
            worker->Deliver();
            worker->Check();
        }
    }
}

// No ARM11 core has a thread to run: while a worker works, what it finishes next is waited for and acted on (true);
// false when none works (the cores idle on to the next event, as before).
inline bool IdleWait() {
    if (!AnyBusy()) {
        return false;
    }
    for (Worker* worker : workers) {
        if (worker != nullptr && worker->Steal()) {
            Poll();
            return true;
        }
    }
    if (results.load(std::memory_order_acquire) == 0) {
        const Stats::Doing doing{"waiting for the GPU's or the DSP's thread (no thread of the game can run)"};
        const std::uint64_t k0 = Stats::Ticks();
        if (!Spin([] { return results.load(std::memory_order_acquire) != 0; })) {
            std::unique_lock lock{results_mutex};
            results_cv.wait(lock, [] { return results.load(std::memory_order_acquire) != 0 || !AnyBusy(); });
        }
        Stats::ticks[Stats::IdleWaitTicks] += Stats::Ticks() - k0;
        Stats::counts[Stats::IdleWaits]++;
    }
    Poll();
    return true;
}

// Every worker drained: a save state, the settings changing.
inline void DrainAll() {
    for (Worker* worker : workers) {
        if (worker != nullptr) {
            worker->Drain(DrainOther);
        }
    }
}
inline void Drain(Slot slot, DrainWhy why) {
    if (Worker* worker = workers[slot]) {
        worker->Drain(why);
    }
}

// What the GPU commands' thread leaves for the emulation thread while a command runs: the interrupts to signal and
// the pages whose tables (and fast memory mappings) the rasterizer's cache asked to change, in order.
struct Deferred {
    struct Interrupt {
        std::uint32_t id;
        std::uint64_t delay_ns;
    };
    struct Mark {
        std::uint32_t start, size;
        bool cached;
    };
    std::vector<Interrupt> interrupts;
    std::vector<Mark> marks;
};
// Set on the GPU commands' thread only.
inline thread_local Deferred* deferred = nullptr;
inline bool OnPicaThread() {
    return deferred != nullptr;
}
// GPU::SetInterruptHandler's: on that thread, an interrupt is the emulation thread's to signal.
inline bool DeferInterrupt(std::uint32_t id, std::uint64_t delay_ns) {
    Deferred* const d = deferred;
    if (d == nullptr) {
        return false;
    }
    d->interrupts.push_back({id, delay_ns});
    return true;
}
// Memory::RasterizerMarkRegionCached's: the pages' tables are the emulation thread's (its CPU reads them).
inline bool DeferMark(std::uint32_t start, std::uint32_t size, bool cached) {
    Deferred* const d = deferred;
    if (d == nullptr) {
        return false;
    }
    d->marks.push_back({start, size, cached});
    return true;
}

// A test file: /data/EmuSuNi/<name> on the PS4 (the PS4 core in it, its first digit 0-6, or `default_core`; the word
// "always" in it: `*always` true); on the PC the environment variable (any value but 0; "always" as on the PS4). -1:
// off.
inline int TestFile(const char* name, const char* env, int default_core, bool* always = nullptr) {
    char text[64] = {};
#if defined(__ORBIS__)
    (void)env;
    char path[96];
    std::snprintf(path, sizeof(path), "/data/EmuSuNi/%s", name);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return -1;
    }
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
#else
    (void)name;
    const char* value = std::getenv(env);
    if (value == nullptr || value[0] == '\0' || value[0] == '0') {
        return -1;
    }
    std::snprintf(text, sizeof(text), "%s", value);
#endif
    if (always != nullptr) {
        *always = std::strstr(text, "always") != nullptr;
    }
    for (const char* c = text; *c != '\0'; c++) {
        if (*c >= '0' && *c <= '6' && (c == text || c[-1] < '0' || c[-1] > '9')) {
#if defined(__ORBIS__)
            return *c - '0';
#else
            break; // (on the PC "1" only turns it on)
#endif
        }
    }
    return default_core;
}
} // namespace EmuSuNi::Async
#endif
''')
addfile('src/video_core/emusuni_async_pica.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.10).
// The GSP's GPU commands on a thread of their own (common/emusuni_async.h; the file /data/EmuSuNi/3ds-async-pica, read
// at each game's start; EMUSUNI_3DS_ASYNC_PICA=1 on the PC). Command lists, memory fills, display transfers and texture
// copies run there one after another, in the order the game sent them, while the emulation thread runs the game on.
// What they would have done to the emulation's own state is left for the emulation thread, which does it when it takes
// the thread's results (at its next slice, or when it drains it): the pages the rasterizer's cache marks (the pages'
// tables and the fast memory's mappings, which its CPU reads), then the GPU's interrupts (P3D, PSC0/1, PPF), so that
// the game learns a command finished only after its pages are marked. (A drain for the CPU on the GPU's memory, in the
// middle of the guest's code or of an HLE call, marks the pages only: the interrupts wait for the next slice.)
// A DMA stays on the emulation thread (the CPU's copy through the game's memory map), after the queue's commands. The
// emulation thread drains this thread (waits for every command, then takes their results) before it touches the GPU's
// state: the VBlank's picture, the GPU's registers, a buffer swap, the CPU on a page the GPU's cache holds, the GPU's
// right changing hands, a save state, the settings or the renderer changing. A C++ exception on this thread (rescued
// there on the PS4) is thrown again on the emulation thread at its next slice: the game ends as it would have.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "common/emusuni_async.h"
#include "common/emusuni_stats.h"
#include "common/emusuni_throw.h"
#include "common/thread.h"
#include "core/hle/service/gsp/gsp_command.h"

namespace EmuSuNi::Async {
class PicaThread final : public Worker {
public:
    using Execute = std::function<void(const Service::GSP::Command&)>;
    using Interrupt = std::function<void(std::uint32_t id, std::uint64_t delay_ns)>;
    using Mark = std::function<void(std::uint32_t start, std::uint32_t size, bool cached)>;

    // core: the PS4 core the app is asked to place it on (its name says it: port/ps4_n64.c); always: never inline
    // (below).
    PicaThread(int core, bool always_, Execute execute_, Interrupt interrupt_, Mark mark_)
        : execute{std::move(execute_)}, interrupt{std::move(interrupt_)}, mark{std::move(mark_)}, always{always_} {
        std::snprintf(name, sizeof(name), "PICA %d", core);
#if !defined(__ORBIS__)
        const char* test = std::getenv("EMUSUNI_3DS_TEST_PICA_THROW");
        throw_at = test != nullptr ? std::strtol(test, nullptr, 10) : 0;
#endif
        Totals(busy_at, waited_at); // (the counters at the first window's start)
        thread = std::thread([this] { Run(); });
        workers[PicaSlot] = this;
    }
    ~PicaThread() override {
        if (workers[PicaSlot] == this) {
            workers[PicaSlot] = nullptr;
        }
        {
            std::lock_guard lock{m};
            stop = true;
            stopping.store(true, std::memory_order_release);
        }
        work_cv.notify_one();
        thread.join();
        // (What it finished and the emulation thread didn't take is left: the system it was for ends.)
    }
    PicaThread(const PicaThread&) = delete;
    PicaThread& operator=(const PicaThread&) = delete;

    // The commands it runs: all of GPU::Execute's but a DMA.
    static bool Takes(Service::GSP::CommandId id) {
        using Service::GSP::CommandId;
        return id == CommandId::SubmitCmdList || id == CommandId::MemoryFill || id == CommandId::DisplayTransfer ||
               id == CommandId::TextureCopy || id == CommandId::CacheFlush;
    }

    void Submit(const Service::GSP::Command& command) {
        outstanding.fetch_add(1, std::memory_order_acq_rel);
        unfinished.fetch_add(1, std::memory_order_acq_rel);
        bool wake;
        {
            std::lock_guard lock{m};
            queue.push_back(command);
            queued.fetch_add(1, std::memory_order_release);
            wake = sleeping;
        }
        if (wake) {
            work_cv.notify_one();
        }
        Stats::counts[Stats::PicaCommands]++;
    }

    bool Busy() const override {
        return outstanding.load(std::memory_order_acquire) != 0;
    }

    // Inline: the commands run on the emulation thread as before (GPU::Execute, once this thread has nothing left).
    // A game that waits for each command as soon as it sends it (Donkey Kong Country Returns, Captain Toad,
    // Castlevania on the PC) gains nothing from this thread and pays the hand-overs; one that runs on while they run
    // (Luigi's Mansion: Dark Moon) gains what it doesn't wait for. So, unless the file says "always": every 60 frames
    // here, the emulation thread's waits for this thread against its busy time; with less than a tenth of it not
    // waited for, 600 frames inline, then 60 here again to measure (GPU::VBlankCallback, after its drain).
    bool Inline() const {
        return inline_mode;
    }
    void FrameEnd() {
        if (inline_mode) {
            Stats::counts[Stats::PicaInlineFrames]++;
        }
        if (always || ++frames < (inline_mode ? 600 : 60)) {
            return;
        }
        frames = 0;
        std::uint64_t busy, waited;
        Totals(busy, waited);
        const std::uint64_t busy_now = busy - busy_at, waited_now = waited - waited_at;
        busy_at = busy;
        waited_at = waited;
        if (inline_mode) {
            inline_mode = false; // (60 frames here again: measured)
            return;
        }
        if (busy_now != 0 && waited_now * 10 >= busy_now * 9) {
            inline_mode = true;
            Stats::counts[Stats::PicaInlineSwitches]++;
        }
    }

    void Deliver() override {
        Deliver(true);
    }
    // interrupts_too false: only the pages marked (a drain for the CPU on the GPU's memory: in the middle of the
    // guest's code or an HLE call, where the kernel's events aren't signalled); the interrupts at the next slice.
    void Deliver(bool interrupts_too) {
        unsigned taken = 0;
        Deferred taking;
        {
            std::lock_guard lock{m};
            if (done_commands == 0 && done.marks.empty()) {
                return;
            }
            taking.marks.swap(done.marks);
            if (interrupts_too) {
                taken = done_commands;
                done_commands = 0;
                taking.interrupts.swap(done.interrupts);
            }
        }
        for (const Deferred::Mark& page : taking.marks) {
            mark(page.start, page.size, page.cached);
        }
        for (const Deferred::Interrupt& irq : taking.interrupts) {
            interrupt(irq.id, irq.delay_ns);
        }
        if (taken != 0) {
            outstanding.fetch_sub(taken, std::memory_order_acq_rel);
        }
    }

    void Drain(DrainWhy why) override {
        if (outstanding.load(std::memory_order_acquire) == 0 || OnPicaThread()) {
            return; // (nothing given to it; or on it: what it runs now comes first)
        }
        {
            if (unfinished.load(std::memory_order_acquire) != 0) {
                const Stats::Doing doing{"waiting for the GPU commands' thread (3ds-async-pica)"};
                const std::uint64_t k0 = Stats::Ticks();
                if (!Spin([this] { return unfinished.load(std::memory_order_acquire) == 0; })) {
                    std::unique_lock lock{m};
                    idle_cv.wait(lock, [this] { return queue.empty() && !running; });
                }
                const std::uint64_t waited = Stats::Ticks() - k0;
                Stats::ticks[Stats::PicaWaitVBlankTicks + why] += waited;
                Stats::counts[Stats::PicaWaitVBlank + why]++;
                // (The 600-frame line's parts: in an HLE call the wait is the GPU's, not the call's; the CPU's on a
                // page isn't the guest's code; a DMA's is GPU::Execute's own time already.)
                if (Stats::in_svc && why != DrainDma) {
                    Stats::ticks[Stats::GpuInSvcTicks] += waited;
                } else if (!Stats::in_svc && why == DrainMemory) {
                    Stats::ticks[Stats::GuestWaitTicks] += waited;
                }
            }
        }
        Deliver(why != DrainMemory);
    }

    void Check() override {
        std::string why;
        {
            std::lock_guard lock{m};
            if (failure.empty() || failure_thrown) {
                return;
            }
            failure_thrown = true;
            why = failure;
        }
        throw std::runtime_error("the GPU commands' thread (3ds-async-pica) stopped on a C++ exception: " + why);
    }

private:
    // This thread's busy time and the emulation thread's waits so far (common/emusuni_stats.h's ticks).
    static void Totals(std::uint64_t& busy, std::uint64_t& waited) {
        busy = Stats::ticks[Stats::PicaBusyTicks];
        waited = Stats::ticks[Stats::PicaWaitVBlankTicks] + Stats::ticks[Stats::PicaWaitMemoryTicks] +
                 Stats::ticks[Stats::PicaWaitGspTicks] + Stats::ticks[Stats::PicaWaitDmaTicks] +
                 Stats::ticks[Stats::PicaWaitOtherTicks] + Stats::ticks[Stats::IdleWaitTicks];
    }

    void Run() {
        Common::SetCurrentThreadName(name);
        Deferred local;
        deferred = &local;
        std::unique_lock lock{m};
        for (;;) {
            if (queue.empty() && !stop) {
                // (The game's next command often comes within microseconds: a spin first, then a sleep.)
                lock.unlock();
                Spin([this] {
                    return queued.load(std::memory_order_acquire) != 0 || stopping.load(std::memory_order_acquire);
                });
                lock.lock();
                while (queue.empty() && !stop) {
                    sleeping = true;
                    work_cv.wait(lock);
                    sleeping = false;
                }
            }
            if (stop) {
                break;
            }
            const Service::GSP::Command command = queue.front();
            queue.pop_front();
            queued.fetch_sub(1, std::memory_order_relaxed);
            running = true;
            const bool skip = !failure.empty();
            lock.unlock();
            const std::uint64_t k0 = Stats::Ticks();
            if (!skip) {
                RunOne(command);
            }
            Stats::ticks[Stats::PicaBusyTicks] += Stats::Ticks() - k0;
            lock.lock();
            running = false;
            done.interrupts.insert(done.interrupts.end(), local.interrupts.begin(), local.interrupts.end());
            done.marks.insert(done.marks.end(), local.marks.begin(), local.marks.end());
            local.interrupts.clear();
            local.marks.clear();
            done_commands++;
            unfinished.fetch_sub(1, std::memory_order_release);
            const bool idle = queue.empty();
            lock.unlock();
            Published();
            if (idle) {
                idle_cv.notify_all();
            }
            lock.lock();
        }
        deferred = nullptr;
    }

    // One command, under a rescue point (the PS4's exceptions come back to it; the PC's are caught).
    void RunOne(const Service::GSP::Command& command) {
        EmuSuNi::Throw::Rescue rescue{"the GPU's commands on their thread (3ds-async-pica)"};
        if (setjmp(rescue.env) != 0) {
            Fail(EmuSuNi::Throw::RescuedMessage());
            return;
        }
        EmuSuNi::Throw::Arm(rescue);
        try {
#if !defined(__ORBIS__)
            if (throw_at > 0 && ++commands_run == throw_at) {
                throw std::runtime_error("EMUSUNI_3DS_TEST_PICA_THROW: a test exception");
            }
#endif
            execute(command);
        } catch (const std::exception& error) {
            Fail(error.what());
        }
        EmuSuNi::Throw::Disarm(rescue);
    }

    void Fail(const char* what) {
        std::lock_guard lock{m};
        if (failure.empty()) {
            failure = what != nullptr && what[0] != '\0' ? what : "?";
        }
    }

    Execute execute;
    Interrupt interrupt;
    Mark mark;
    char name[16] = "PICA";
    std::mutex m;
    std::condition_variable work_cv, idle_cv;
    std::deque<Service::GSP::Command> queue;
    bool running = false, sleeping = false, stop = false;
    Deferred done;                  // finished commands' results (under m)
    unsigned done_commands = 0;     // (under m)
    std::atomic<unsigned> outstanding{0}; // given and not taken by the emulation thread
    std::atomic<unsigned> unfinished{0};  // given and not finished (queued or running)
    std::atomic<unsigned> queued{0};      // in the queue (the spins' view of it)
    std::atomic<bool> stopping{false};
    std::string failure;            // (under m) the exception that stopped it
    bool failure_thrown = false;
    // (The emulation thread's: inline or not, the frames of this window, the counters at its start.)
    const bool always;
    bool inline_mode = false;
    unsigned frames = 0;
    std::uint64_t busy_at = 0, waited_at = 0;
#if !defined(__ORBIS__)
    long throw_at = 0, commands_run = 0;
#endif
    std::thread thread;
};
} // namespace EmuSuNi::Async
#endif
''')
patch('src/common/emusuni_stats.h', r'''
    ListToListTicks, AsyncUpTicks, ListToWaitTicks, AsyncLowTicks,
    TickCount
''', r'''
    ListToListTicks, AsyncUpTicks, ListToWaitTicks, AsyncLowTicks,
    // (01.10) The GPU commands' and the DSP's threads (common/emusuni_async.h): their busy time, the emulation
    // thread's waits for the GPU's by why (in DrainWhy's order), with no thread of the game to run, the CPU's on a
    // page the GPU's cache holds (out of the guest's code's), and its waits for the DSP's mixing.
    PicaBusyTicks, PicaWaitVBlankTicks, PicaWaitMemoryTicks, PicaWaitGspTicks, PicaWaitDmaTicks, PicaWaitOtherTicks,
    IdleWaitTicks, GuestWaitTicks, DspBusyTicks, DspWaitTicks, DspStolenTicks,
    TickCount
''')
patch('src/common/emusuni_stats.h', r'''
    ImmediateWords, ListGaps, GspWaits, // (01.09) the threads' study
    CountCount
''', r'''
    ImmediateWords, ListGaps, GspWaits, // (01.09) the threads' study
    // (01.10) The commands the GPU's thread was given, the waits (as the ticks'), its frames inline and switches to
    // them; the frames the DSP's mixed, those the emulation thread mixed itself (its thread not started yet)
    PicaCommands, PicaWaitVBlank, PicaWaitMemory, PicaWaitGsp, PicaWaitDma, PicaWaitOther, IdleWaits,
    PicaInlineFrames, PicaInlineSwitches, DspFrames, DspWaits, DspStolen, DspInlineFrames, DspSwitches,
    CountCount
''')
patch('src/common/emusuni_frame_stats.h', r'''
        const std::uint64_t jit = dt[JitTicks], svc = dt[SvcTicks], gpu_in_svc = dt[GpuInSvcTicks];
        const std::uint64_t guest = dt[GuestTicks] > jit + svc ? dt[GuestTicks] - jit - svc : 0;
''', r'''
        const std::uint64_t jit = dt[JitTicks], svc = dt[SvcTicks], gpu_in_svc = dt[GpuInSvcTicks];
        // ((01.10) The CPU's waits for the GPU's thread on a page its cache holds aren't the guest's code.)
        const std::uint64_t guest_waits = dt[GuestWaitTicks];
        const std::uint64_t guest =
            dt[GuestTicks] > jit + svc + guest_waits ? dt[GuestTicks] - jit - svc - guest_waits : 0;
''')
patch('src/common/emusuni_frame_stats.h', r'''
                     part_ms(dt[ListToWaitTicks]), dc[GspWaits], part_ms(dt[AsyncLowTicks]));
            period_t0 = now_us;
''', r'''
                     part_ms(dt[ListToWaitTicks]), dc[GspWaits], part_ms(dt[AsyncLowTicks]));
            // (01.10) The GPU commands' and the DSP's threads, when on (common/emusuni_async.h): a line each.
            if (dc[PicaCommands] != 0 || dc[PicaInlineFrames] != 0) {
                const std::uint64_t waited = dt[PicaWaitVBlankTicks] + dt[PicaWaitMemoryTicks] +
                                             dt[PicaWaitGspTicks] + dt[PicaWaitDmaTicks] + dt[PicaWaitOtherTicks];
                LOG_INFO(Frontend,
                         "EmuSuNi: the GPU commands' thread (3ds-async-pica) in those 600 frames: busy {} ms ({} "
                         "commands; inline {} frames, {} switches to it); the emulation waited {} ms for it: the "
                         "VBlank {} ms ({}), the CPU on the GPU's memory {} ms ({}), GSP and registers {} ms ({}), "
                         "DMA {} ms ({}), the rest {} ms ({}); with no thread of the game to run {} ms ({})",
                         part_ms(dt[PicaBusyTicks]), dc[PicaCommands], dc[PicaInlineFrames], dc[PicaInlineSwitches],
                         part_ms(waited), part_ms(dt[PicaWaitVBlankTicks]), dc[PicaWaitVBlank],
                         part_ms(dt[PicaWaitMemoryTicks]), dc[PicaWaitMemory], part_ms(dt[PicaWaitGspTicks]),
                         dc[PicaWaitGsp], part_ms(dt[PicaWaitDmaTicks]), dc[PicaWaitDma],
                         part_ms(dt[PicaWaitOtherTicks]), dc[PicaWaitOther], part_ms(dt[IdleWaitTicks]),
                         dc[IdleWaits]);
            }
            if (dc[DspFrames] != 0 || dc[DspStolen] != 0 || dc[DspInlineFrames] != 0) {
                LOG_INFO(Frontend,
                         "EmuSuNi: the DSP's mixing thread (3ds-async-dsp) in those 600 frames: {} ms ({} frames; "
                         "{} mixed by the emulation thread, {} ms, before their thread woke; {} inline, {} switches); "
                         "the emulation waited {} ms for it ({} times){}",
                         part_ms(dt[DspBusyTicks]), dc[DspFrames], dc[DspStolen], part_ms(dt[DspStolenTicks]),
                         dc[DspInlineFrames], dc[DspSwitches], part_ms(dt[DspWaitTicks]), dc[DspWaits],
                         dc[PicaCommands] != 0 ? std::string()
                                               : fmt::format("; with no thread of the game to run {} ms ({})",
                                                             part_ms(dt[IdleWaitTicks]), dc[IdleWaits]));
            }
            period_t0 = now_us;
''')
patch('src/video_core/gpu.h', r'''
namespace Frontend {
class EmuWindow;
}
''', r'''
namespace Frontend {
class EmuWindow;
}

#ifdef EMUSUNI_PORT
namespace EmuSuNi::Async {
class PicaThread;
}
#endif
''')
patch('src/video_core/gpu.h', r'''
    struct Impl;
    std::unique_ptr<Impl> impl;
''', r'''
    struct Impl;
    std::unique_ptr<Impl> impl;
#ifdef EMUSUNI_PORT
    // (01.10) The GSP's GPU commands on a thread of their own (video_core/emusuni_async_pica.h; 3ds-async-pica), and
    // its drain (waits for its commands, then takes their results) before the GPU's state is touched here.
    std::unique_ptr<EmuSuNi::Async::PicaThread> emusuni_pica;
    void EmuSuNiDrain(int why);
#endif
''')
patch('src/video_core/gpu.cpp', r'''
#ifdef EMUSUNI_PORT
#include "common/emusuni_stats.h"
#endif
#include "common/microprofile.h"
''', r'''
#ifdef EMUSUNI_PORT
#include "common/emusuni_stats.h"
#include "video_core/emusuni_async_pica.h"
#endif
#include "common/microprofile.h"
''')
patch('src/video_core/gpu.cpp', r'''
    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);
}

GPU::~GPU() = default;
''', r'''
    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);
#ifdef EMUSUNI_PORT
    // (01.10) The GSP's GPU commands on a thread of their own with the file /data/EmuSuNi/3ds-async-pica
    // (video_core/emusuni_async_pica.h); the PS4 core asked of the app: the file's number, 0 by default; "always" in
    // it: never inline.
    bool emusuni_always = false;
    const int emusuni_core = EmuSuNi::Async::TestFile("3ds-async-pica", "EMUSUNI_3DS_ASYNC_PICA", 0, &emusuni_always);
    if (emusuni_core >= 0) {
        emusuni_pica = std::make_unique<EmuSuNi::Async::PicaThread>(
            emusuni_core, emusuni_always, [this](const Service::GSP::Command& command) { Execute(command); },
            [this](u32 id, u64 delay_ns) {
                impl->signal_interrupt(static_cast<Service::GSP::InterruptId>(id), delay_ns);
            },
            [this](u32 start, u32 size, bool cached) {
                impl->memory.RasterizerMarkRegionCached(start, size, cached);
            });
        LOG_INFO(HW_GPU, "EmuSuNi: the GPU's commands on a thread of their own (3ds-async-pica; the PS4's core {}{})",
                 emusuni_core, emusuni_always ? "; always, never inline" : "");
    }
#endif
}

GPU::~GPU() {
#ifdef EMUSUNI_PORT
    emusuni_pica.reset(); // (01.10) its thread stopped first: the renderer and the PICA it uses go after
#endif
}

#ifdef EMUSUNI_PORT
void GPU::EmuSuNiDrain(int why) {
    if (emusuni_pica) {
        emusuni_pica->Drain(static_cast<EmuSuNi::Async::DrainWhy>(why));
    }
}
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
''', r'''
void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
#ifdef EMUSUNI_PORT
    // (01.10) On the GPU commands' thread an interrupt is left for the emulation thread (common/emusuni_async.h).
    handler = [inner = std::move(handler)](Service::GSP::InterruptId id, u64 delay_ns) {
        if (!EmuSuNi::Async::DeferInterrupt(static_cast<u32>(id), delay_ns)) {
            inner(id, delay_ns);
        }
    };
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::FlushRegion(PAddr addr, u32 size) {
''', r'''
void GPU::FlushRegion(PAddr addr, u32 size) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainMemory); // (01.10: its cache, after the GPU commands' thread)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::InvalidateRegion(PAddr addr, u32 size) {
''', r'''
void GPU::InvalidateRegion(PAddr addr, u32 size) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainMemory); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::ClearAll(bool flush) {
''', r'''
void GPU::ClearAll(bool flush) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainOther); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::Execute(const Service::GSP::Command& command) {
#ifdef EMUSUNI_PORT
    const EmuSuNi::Stats::GpuTimer emusuni_timer;
''', r'''
void GPU::Execute(const Service::GSP::Command& command) {
#ifdef EMUSUNI_PORT
    // (01.10) With the GPU commands' thread (3ds-async-pica): its commands queued there, in order (inline, once it has
    // nothing left: here, as before); a DMA after them.
    if (emusuni_pica && !EmuSuNi::Async::OnPicaThread()) {
        const bool emusuni_takes = EmuSuNi::Async::PicaThread::Takes(command.id);
        if (emusuni_takes && (!emusuni_pica->Inline() || emusuni_pica->Busy())) {
            emusuni_pica->Submit(command);
            return;
        }
        emusuni_pica->Drain(emusuni_takes ? EmuSuNi::Async::DrainOther : EmuSuNi::Async::DrainDma);
    }
    const EmuSuNi::Stats::GpuTimer emusuni_timer;
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
''', r'''
void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainGsp); // (01.10: the GPU's registers, after its commands' thread)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::SetColorFill(const Pica::ColorFill& fill) {
''', r'''
void GPU::SetColorFill(const Pica::ColorFill& fill) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainGsp); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
u32 GPU::ReadReg(VAddr addr) {
''', r'''
u32 GPU::ReadReg(VAddr addr) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainGsp); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::WriteReg(VAddr addr, u32 data) {
''', r'''
void GPU::WriteReg(VAddr addr, u32 data) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainGsp); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::ApplyPerProgramSettings(u64 program_ID) {
''', r'''
void GPU::ApplyPerProgramSettings(u64 program_ID) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainGsp); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
#ifdef EMUSUNI_PORT
    const std::uint64_t emusuni_start = EmuSuNi::Stats::Ticks(); // (01.09: the threads' study)
    EmuSuNi::Stats::ListStarted(emusuni_start);
#endif
    impl->pica.ProcessCmdList(addr, size,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));
#ifdef EMUSUNI_PORT
    EmuSuNi::Stats::ListEnded(emusuni_start, EmuSuNi::Stats::Ticks());
#endif
''', r'''
#ifdef EMUSUNI_PORT
    // (01.09: the threads' study; 01.10: not on the GPU commands' thread, whose lists aren't the emulation's time)
    const bool emusuni_study = !EmuSuNi::Async::OnPicaThread();
    const std::uint64_t emusuni_start = EmuSuNi::Stats::Ticks();
    if (emusuni_study) {
        EmuSuNi::Stats::ListStarted(emusuni_start);
    }
#endif
    impl->pica.ProcessCmdList(addr, size,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));
#ifdef EMUSUNI_PORT
    if (emusuni_study) {
        EmuSuNi::Stats::ListEnded(emusuni_start, EmuSuNi::Stats::Ticks());
    }
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
''', r'''
void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
#ifdef EMUSUNI_PORT
    // (01.10) The picture of every command sent before the VBlank, as with the commands on this thread; every 60
    // frames, the GPU commands' thread or inline (video_core/emusuni_async_pica.h).
    EmuSuNiDrain(EmuSuNi::Async::DrainVBlank);
    if (emusuni_pica) {
        emusuni_pica->FrameEnd();
    }
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
''', r'''
void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainOther); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::ReleaseRenderer() {
''', r'''
void GPU::ReleaseRenderer() {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainOther); // (01.10)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::serialize(Archive& ar, const u32 file_version) {
''', r'''
void GPU::serialize(Archive& ar, const u32 file_version) {
#ifdef EMUSUNI_PORT
    EmuSuNiDrain(EmuSuNi::Async::DrainOther); // (01.10)
#endif
''')
patch('src/core/memory.cpp', r'''
#include "common/emusuni_fastmem.h"
#include "common/emusuni_mem.h"
#endif
''', r'''
#include "common/emusuni_async.h"
#include "common/emusuni_fastmem.h"
#include "common/emusuni_mem.h"
#endif
''')
patch('src/core/memory.cpp', r'''
            if (start >= region_end || end <= region_start) {
                // No overlap with region
                return;
            }
''', r'''
            if (start >= region_end || end <= region_start) {
                // No overlap with region
                return;
            }
#ifdef EMUSUNI_PORT
            // (01.10) The GPU's cache is its commands' thread's while it works (3ds-async-pica): after its commands.
            EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainMemory);
#endif
''')
patch('src/core/memory.cpp', r'''
    if (start == 0) {
        return;
    }
#ifdef EMUSUNI_PORT
    // EmuSuNi: the pages whose pointers this changes, for the fast memory to follow in runs at the end.
''', r'''
    if (start == 0) {
        return;
    }
#ifdef EMUSUNI_PORT
    // (01.10) On the GPU commands' thread (3ds-async-pica) the pages' tables and the fast memory's mappings are the
    // emulation thread's to change (its CPU reads them): it does when it takes that thread's results.
    if (EmuSuNi::Async::DeferMark(start, size, cached)) {
        return;
    }
    // EmuSuNi: the pages whose pointers this changes, for the fast memory to follow in runs at the end.
''')
patch('src/core/memory.cpp', r'''
MemorySystem::PhysMemRegionInfo MemorySystem::GetPhysMemRegionInfo(PAddr address) {
    if (address >= phys_mem_region_info_cache.region_start &&
''', r'''
MemorySystem::PhysMemRegionInfo MemorySystem::GetPhysMemRegionInfo(PAddr address) {
#ifdef EMUSUNI_PORT
    // (01.10) Without the cache of the last region, written and read by every thread that looks one up: the GPU
    // commands' and the DSP's threads do too, at once with the emulation thread, and a cache half written by one gave
    // another one region's start with another region's memory (Luigi's Mansion: Dark Moon's indices read off the map,
    // in VRAM taken for FCRAM). The four regions each time, in upstream's order and with its inclusive ends.
    if (address >= VRAM_PADDR && address <= VRAM_PADDR + VRAM_SIZE) {
        return {&impl->vram_mem, VRAM_PADDR, VRAM_SIZE};
    }
    if (address >= DSP_RAM_PADDR && address <= DSP_RAM_PADDR + DSP_RAM_SIZE) {
        return {&impl->dsp_mem, DSP_RAM_PADDR, DSP_RAM_SIZE};
    }
    if (address >= FCRAM_PADDR && address <= FCRAM_PADDR + FCRAM_N3DS_SIZE) {
        return {&impl->fcram_mem, FCRAM_PADDR, FCRAM_N3DS_SIZE};
    }
    if (address >= N3DS_EXTRA_RAM_PADDR && address <= N3DS_EXTRA_RAM_PADDR + N3DS_EXTRA_RAM_SIZE) {
        return {&impl->n3ds_extra_ram_mem, N3DS_EXTRA_RAM_PADDR, N3DS_EXTRA_RAM_SIZE};
    }
    LOG_ERROR(HW_Memory, "Unknown GetPhysMemRegionInfo @ {:#08X} at PC {:#08X}", address, impl->GetPC());
    return PhysMemRegionInfo();
#else
    if (address >= phys_mem_region_info_cache.region_start &&
''')
patch('src/core/memory.cpp', r'''
    return phys_mem_region_info_cache;
}

u8* MemorySystem::GetPhysicalPointer(PAddr address) {
''', r'''
    return phys_mem_region_info_cache;
#endif
}

u8* MemorySystem::GetPhysicalPointer(PAddr address) {
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
#ifdef EMUSUNI_PORT
#include "common/emusuni_stats.h"
#endif
''', r'''
#ifdef EMUSUNI_PORT
#include "common/emusuni_async.h"
#include "common/emusuni_stats.h"
#endif
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
void GSP_GPU::SaveVramSysArea(Kernel::HLERequestContext& ctx) {
''', r'''
void GSP_GPU::SaveVramSysArea(Kernel::HLERequestContext& ctx) {
#ifdef EMUSUNI_PORT
    // (01.10) The screens' memory, after the GPU commands' thread (3ds-async-pica).
    EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainGsp);
#endif
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
void GSP_GPU::RestoreVramSysArea(Kernel::HLERequestContext& ctx) {
''', r'''
void GSP_GPU::RestoreVramSysArea(Kernel::HLERequestContext& ctx) {
#ifdef EMUSUNI_PORT
    EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainGsp); // (01.10)
#endif
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
                                bool blocking) {
    const auto session_data = GetSessionData(ctx.Session());
''', r'''
                                bool blocking) {
#ifdef EMUSUNI_PORT
    // (01.10) The GPU's right changes hands (its settings, the shader cache): after the GPU commands' thread.
    EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainGsp);
#endif
    const auto session_data = GetSessionData(ctx.Session());
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
void GSP_GPU::ReleaseRight(const SessionData* session_data) {
''', r'''
void GSP_GPU::ReleaseRight(const SessionData* session_data) {
#ifdef EMUSUNI_PORT
    EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainGsp); // (01.10)
#endif
''')
patch('src/core/core.cpp', r'''
#ifdef EMUSUNI_PORT
#include <stdexcept>
#include "common/emusuni_jit.h"
''', r'''
#ifdef EMUSUNI_PORT
#include <stdexcept>
#include "common/emusuni_async.h"
#include "common/emusuni_jit.h"
''')
patch('src/core/core.cpp', r'''
    ProcessPendingWork();

    Signal signal{Signal::None};
''', r'''
    ProcessPendingWork();
#ifdef EMUSUNI_PORT
    // (01.10) What the GPU commands' and the DSP's threads finished, acted on (common/emusuni_async.h).
    EmuSuNi::Async::Poll();
#endif

    Signal signal{Signal::None};
''')
patch('src/core/core.cpp', r'''
            max_slice = std::min(max_slice, cpu_core->GetTimer().GetMaxSliceLength());
        }
        for (auto& cpu_core : cpu_cores) {
''', r'''
            max_slice = std::min(max_slice, cpu_core->GetTimer().GetMaxSliceLength());
        }
#ifdef EMUSUNI_PORT
        // (01.10) No core has a thread to run while the GPU commands' or the DSP's thread works (3ds-async-pica,
        // 3ds-async-dsp): the game waits for it. What it finishes is waited for here, not idled ahead of (the
        // emulated time would run on without its work, which takes none with the work on this thread); the next run
        // takes the threads it woke.
        if (EmuSuNi::Async::AnyBusy()) {
            bool emusuni_none = true;
            for (const auto& cpu_core : cpu_cores) {
                if (kernel->GetThreadManager(cpu_core->GetID()).GetCurrentThread() != nullptr) {
                    emusuni_none = false;
                    break;
                }
            }
            if (emusuni_none && EmuSuNi::Async::IdleWait()) {
                return status;
            }
        }
#endif
        for (auto& cpu_core : cpu_cores) {
''')
patch('src/core/core.cpp', r'''
void System::ApplySettings() {
''', r'''
void System::ApplySettings() {
#ifdef EMUSUNI_PORT
    EmuSuNi::Async::DrainAll(); // (01.10) the settings the GPU commands' and the DSP's threads read change
#endif
''')
patch('src/core/core.cpp', r'''
void System::serialize(Archive& ar, const unsigned int file_version) {
''', r'''
void System::serialize(Archive& ar, const unsigned int file_version) {
#ifdef EMUSUNI_PORT
    // (01.10) The GPU commands' and the DSP's threads drained first: what they did is in the state, nothing of theirs.
    EmuSuNi::Async::DrainAll();
#endif
''')
patch('src/audio_core/hle/hle.cpp', r'''
#ifdef EMUSUNI_PORT
#include "common/emusuni_stats.h"
#endif
''', r'''
#ifdef EMUSUNI_PORT
#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include "common/emusuni_async.h"
#include "common/emusuni_stats.h"
#include "common/emusuni_throw.h"
#include "common/thread.h"
#endif
''')
patch('src/audio_core/hle/hle.cpp', r'''
    std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> interrupt_handler{};
''', r'''
    std::function<void(Service::DSP::InterruptType type, DspPipe pipe)> interrupt_handler{};
#ifdef EMUSUNI_PORT
    // (01.10) The audio frames' mixing on a thread of its own (the file /data/EmuSuNi/3ds-async-dsp; below).
    class EmuSuNiMixer;
    std::unique_ptr<EmuSuNiMixer> emusuni_mixer;
#endif
''')
patch('src/audio_core/hle/hle.cpp', r'''
    friend class boost::serialization::access;
};

DspHle::Impl::Impl(DspHle& parent_, Memory::MemorySystem& memory, Core::Timing& timing)
''', r'''
    friend class boost::serialization::access;
};

#ifdef EMUSUNI_PORT
// EmuSuNi (01.10): the audio frames' mixing on a thread of its own (common/emusuni_async.h; the file
// /data/EmuSuNi/3ds-async-dsp, read at each game's start; EMUSUNI_3DS_ASYNC_DSP=1 on the PC). At each audio frame's
// tick the emulation thread starts it there ("DSP HLE <core>": the sources and the mixers of that frame, its samples
// into the DSP's memory); the frame goes to the sink and its interrupt to the game when the emulation thread takes it,
// at its next slice (or when it needs the DSP: the next tick, a pipe's write, a save state). The game's audio thread
// waits for that interrupt before it reads the frame's statuses and writes the next frame's configuration, as before.
class DspHle::Impl::EmuSuNiMixer final : public EmuSuNi::Async::Worker {
public:
    EmuSuNiMixer(Impl& impl_, int core, bool always_) : impl{impl_}, always{always_} {
        std::snprintf(name, sizeof(name), "DSP HLE %d", core);
        rate_ticks = EmuSuNi::Stats::Ticks();
        rate_us = EmuSuNi::Stats::NowUs();
        thread = std::thread([this] { Run(); });
        EmuSuNi::Async::workers[EmuSuNi::Async::DspSlot] = this;
    }
    ~EmuSuNiMixer() override {
        if (EmuSuNi::Async::workers[EmuSuNi::Async::DspSlot] == this) {
            EmuSuNi::Async::workers[EmuSuNi::Async::DspSlot] = nullptr;
        }
        {
            std::lock_guard lock{m};
            stop = true;
        }
        work_cv.notify_all();
        thread.join();
    }
    EmuSuNiMixer(const EmuSuNiMixer&) = delete;
    EmuSuNiMixer& operator=(const EmuSuNiMixer&) = delete;

    // The emulation thread, at an audio frame's tick with the DSP on: that frame's mixing started.
    void Start() {
        pending.store(true, std::memory_order_release);
        {
            std::lock_guard lock{m};
            job = true;
        }
        work_cv.notify_all();
    }

    bool Busy() const override {
        return pending.load(std::memory_order_acquire);
    }

    // Inline: the frame mixed at its tick on the emulation thread, as before. The mixing's time is measured wherever
    // it runs, and every 300 frames its mean is weighed against 25 us (a hand-over's cost, about: the thread's wake-up
    // and the notices): below, inline (this PC's mixing takes about 7 us a frame), above, on its thread. "always" in
    // the file: on its thread.
    bool Inline() const {
        return inline_mode;
    }
    // The emulation thread, for each frame mixed (its ticks; inline or not).
    void Mixed(std::uint64_t ticks, bool here) {
        if (here) {
            EmuSuNi::Stats::counts[EmuSuNi::Stats::DspInlineFrames]++;
        }
        window_ticks += ticks;
        if (++window_frames < 300) {
            return;
        }
        const std::uint64_t now_ticks = EmuSuNi::Stats::Ticks(), now_us = EmuSuNi::Stats::NowUs();
        const double per_us = now_us > rate_us ? static_cast<double>(now_ticks - rate_ticks) /
                                                     static_cast<double>(now_us - rate_us)
                                               : 0.0;
        const double mean_us = per_us > 0 ? static_cast<double>(window_ticks) / window_frames / per_us : 0.0;
        rate_ticks = now_ticks;
        rate_us = now_us;
        window_ticks = 0;
        window_frames = 0;
        const bool want = !always && per_us > 0 && mean_us < 25.0;
        if (want != inline_mode) {
            inline_mode = want;
            EmuSuNi::Stats::counts[EmuSuNi::Stats::DspSwitches]++;
        }
    }

    void Deliver() override {
        if (!pending.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::lock_guard lock{m};
            if (!done) {
                return;
            }
            done = false;
        }
        Finish();
    }

    // The frame's mixing not started on its thread yet (asleep between frames: its wake-up takes longer than the
    // mixing): done here instead.
    bool Steal() override {
        if (!pending.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard lock{m};
            if (!job) {
                return false;
            }
            job = false;
        }
        const std::uint64_t k0 = EmuSuNi::Stats::Ticks();
        mixed = Mix(); // (its thread won't touch it: the job is this thread's now)
        job_ticks = EmuSuNi::Stats::Ticks() - k0;
        EmuSuNi::Stats::ticks[EmuSuNi::Stats::DspStolenTicks] += job_ticks;
        EmuSuNi::Stats::counts[EmuSuNi::Stats::DspStolen]++;
        Finish();
        return true;
    }

    void Drain(EmuSuNi::Async::DrainWhy) override {
        if (!pending.load(std::memory_order_acquire)) {
            return;
        }
        if (Steal()) {
            return;
        }
        {
            std::unique_lock lock{m};
            if (!done) {
                const EmuSuNi::Stats::Doing doing{"waiting for the DSP's mixing thread (3ds-async-dsp)"};
                const std::uint64_t k0 = EmuSuNi::Stats::Ticks();
                done_cv.wait(lock, [this] { return done; });
                EmuSuNi::Stats::ticks[EmuSuNi::Stats::DspWaitTicks] += EmuSuNi::Stats::Ticks() - k0;
                EmuSuNi::Stats::counts[EmuSuNi::Stats::DspWaits]++;
            }
            done = false;
        }
        Finish();
    }

    void Check() override {
        std::string why;
        {
            std::lock_guard lock{m};
            if (failure.empty() || failure_thrown) {
                return;
            }
            failure_thrown = true;
            why = failure;
        }
        throw std::runtime_error("the DSP's mixing thread (3ds-async-dsp) stopped on a C++ exception: " + why);
    }

private:
    // The emulation thread: the frame to the sink, its interrupt to the game (none for a frame whose mixing failed).
    void Finish() {
        pending.store(false, std::memory_order_release);
        Mixed(job_ticks, false);
        if (!mixed) {
            return;
        }
        impl.parent.OutputFrame(std::move(frame));
        impl.interrupt_handler(InterruptType::Pipe, DspPipe::Audio);
    }

    void Run() {
        Common::SetCurrentThreadName(name);
        std::unique_lock lock{m};
        for (;;) {
            work_cv.wait(lock, [this] { return stop || job; });
            if (stop) {
                break;
            }
            job = false;
            lock.unlock();
            const std::uint64_t k0 = EmuSuNi::Stats::Ticks();
            const bool ok = Mix();
            const std::uint64_t took = EmuSuNi::Stats::Ticks() - k0;
            EmuSuNi::Stats::ticks[EmuSuNi::Stats::DspBusyTicks] += took;
            EmuSuNi::Stats::counts[EmuSuNi::Stats::DspFrames]++;
            lock.lock();
            mixed = ok;
            job_ticks = took;
            done = true;
            lock.unlock();
            done_cv.notify_all();
            EmuSuNi::Async::Published();
            lock.lock();
        }
    }

    // One frame, under a rescue point (the PS4's exceptions come back to it; the PC's are caught).
    bool Mix() {
        EmuSuNi::Throw::Rescue rescue{"the audio's mixing on its thread (3ds-async-dsp)"};
        if (setjmp(rescue.env) != 0) {
            Fail(EmuSuNi::Throw::RescuedMessage());
            return false;
        }
        EmuSuNi::Throw::Arm(rescue);
        try {
            frame = impl.GenerateCurrentFrame();
        } catch (const std::exception& error) {
            Fail(error.what());
            EmuSuNi::Throw::Disarm(rescue);
            return false;
        }
        EmuSuNi::Throw::Disarm(rescue);
        return true;
    }

    void Fail(const char* what) {
        std::lock_guard lock{m};
        if (failure.empty()) {
            failure = what != nullptr && what[0] != '\0' ? what : "?";
        }
    }

    Impl& impl;
    const bool always;
    char name[16] = "DSP HLE";
    std::mutex m;
    std::condition_variable work_cv, done_cv;
    bool job = false, done = false, stop = false, mixed = false; // (under m)
    std::uint64_t job_ticks = 0; // (under m, or the emulation thread's for a stolen job) the last frame's mixing
    // (The emulation thread's: inline or not, this window's mixing and frames, the clocks at its start.)
    bool inline_mode = false;
    std::uint64_t window_ticks = 0, rate_ticks = 0, rate_us = 0;
    unsigned window_frames = 0;
    std::atomic<bool> pending{false}; // started and not finished on the emulation thread
    StereoFrame16 frame{};
    std::string failure; // (under m) the exception that stopped a frame's mixing
    bool failure_thrown = false;
    std::thread thread;
};
#endif

DspHle::Impl::Impl(DspHle& parent_, Memory::MemorySystem& memory, Core::Timing& timing)
''')
patch('src/audio_core/hle/hle.cpp', r'''
    core_timing.ScheduleEvent(audio_frame_ticks, tick_event);
}

DspHle::Impl::~Impl() {
    core_timing.UnscheduleEvent(tick_event, 0);
}
''', r'''
    core_timing.ScheduleEvent(audio_frame_ticks, tick_event);
#ifdef EMUSUNI_PORT
    // (01.10) The PS4 core asked of the app: the file's number, 1 by default; "always" in it: never inline.
    bool emusuni_always = false;
    const int emusuni_core = EmuSuNi::Async::TestFile("3ds-async-dsp", "EMUSUNI_3DS_ASYNC_DSP", 1, &emusuni_always);
    if (emusuni_core >= 0) {
        emusuni_mixer = std::make_unique<EmuSuNiMixer>(*this, emusuni_core, emusuni_always);
        LOG_INFO(Audio_DSP,
                 "EmuSuNi: the audio frames' mixing on a thread of its own (3ds-async-dsp; the PS4's core {}{})",
                 emusuni_core, emusuni_always ? "; always, never inline" : "");
    }
#endif
}

DspHle::Impl::~Impl() {
#ifdef EMUSUNI_PORT
    emusuni_mixer.reset(); // (01.10) its thread stopped first
#endif
    core_timing.UnscheduleEvent(tick_event, 0);
}
''')
patch('src/audio_core/hle/hle.cpp', r'''
void DspHle::Impl::PipeWrite(DspPipe pipe_number, std::span<const u8> buffer) {
''', r'''
void DspHle::Impl::PipeWrite(DspPipe pipe_number, std::span<const u8> buffer) {
#ifdef EMUSUNI_PORT
    if (emusuni_mixer) {
        emusuni_mixer->Drain(EmuSuNi::Async::DrainOther); // (01.10) the mixing's state and memory change here
    }
#endif
''')
patch('src/audio_core/hle/hle.cpp', r'''
    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::DspTicks};
#endif
    if (Tick()) {
''', r'''
    const EmuSuNi::Stats::TickTimer emusuni_timer{EmuSuNi::Stats::DspTicks};
    if (emusuni_mixer) {
        // (01.10) On its thread (3ds-async-dsp): the frame before out first (taken already, as a rule), this one's
        // mixing started there, its frame and interrupt when the emulation thread takes it; or mixed here, as before,
        // while its thread doesn't pay (inline).
        emusuni_mixer->Drain(EmuSuNi::Async::DrainOther);
        if (GetDspState() == DspState::On) {
            if (emusuni_mixer->Inline()) {
                const std::uint64_t emusuni_k0 = EmuSuNi::Stats::Ticks();
                StereoFrame16 emusuni_frame = GenerateCurrentFrame();
                emusuni_mixer->Mixed(EmuSuNi::Stats::Ticks() - emusuni_k0, true);
                parent.OutputFrame(std::move(emusuni_frame));
                interrupt_handler(InterruptType::Pipe, DspPipe::Audio);
            } else {
                emusuni_mixer->Start();
            }
        }
    } else
#endif
    if (Tick()) {
''')
patch('src/citra_libretro/citra_libretro.cpp', r'''
#include "common/emusuni_stall.h"
#include "common/emusuni_throw.h"
#endif
''', r'''
#include "common/emusuni_async.h"
#include "common/emusuni_stall.h"
#include "common/emusuni_throw.h"
#endif
''')
patch('src/citra_libretro/citra_libretro.cpp', r'''
    if (LibRetro::HasUpdatedConfig()) {
        LibRetro::ParseCoreOptions();
''', r'''
    if (LibRetro::HasUpdatedConfig()) {
#ifdef EMUSUNI_PORT
        EmuSuNi::Async::DrainAll(); // (01.10) the settings the GPU commands' and the DSP's threads read change
#endif
        LibRetro::ParseCoreOptions();
''')
patch('src/citra_libretro/citra_libretro.cpp', r'''
    if (screen_swap_btn != screen_swap_button_state) {
''', r'''
    if (screen_swap_btn != screen_swap_button_state) {
#ifdef EMUSUNI_PORT
        EmuSuNi::Async::DrainAll(); // (01.10)
#endif
''')

# --- 01.11: the GPU's commands' and the audio's mixing's threads by default (the owner's OK, 08-11: in 01.10's test on
# the PS4 Dark Moon at 4x went from about 63 % to 72 % with them, and no game lost anything): the word "off" in
# /data/EmuSuNi/3ds-async-pica or 3ds-async-dsp leaves that work on the emulation thread, as before 01.10 (on the PC,
# EMUSUNI_3DS_ASYNC_PICA or EMUSUNI_3DS_ASYNC_DSP set to 0 or off); a number and "always" in them as in 01.10.
patch('src/common/emusuni_async.h',
      '// Without the files nothing here runs: the work stays on the emulation thread, as before.\n',
      '// (01.11) By default: the word "off" in its file leaves a job on the emulation thread, as before 01.10.\n')
patch('src/common/emusuni_async.h', r'''
// A test file: /data/EmuSuNi/<name> on the PS4 (the PS4 core in it, its first digit 0-6, or `default_core`; the word
// "always" in it: `*always` true); on the PC the environment variable (any value but 0; "always" as on the PS4). -1:
// off.
inline int TestFile(const char* name, const char* env, int default_core, bool* always = nullptr) {
    char text[64] = {};
#if defined(__ORBIS__)
    (void)env;
    char path[96];
    std::snprintf(path, sizeof(path), "/data/EmuSuNi/%s", name);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return -1;
    }
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
#else
    (void)name;
    const char* value = std::getenv(env);
    if (value == nullptr || value[0] == '\0' || value[0] == '0') {
        return -1;
    }
    std::snprintf(text, sizeof(text), "%s", value);
#endif
    if (always != nullptr) {
        *always = std::strstr(text, "always") != nullptr;
    }
''', r'''
// A test file: /data/EmuSuNi/<name> on the PS4 (the PS4 core in it, its first digit 0-6, or `default_core`; the word
// "always" in it: `*always` true); on the PC the environment variable ("always" as on the PS4). -1: off. (01.11) On
// by default: no file (no variable) as an empty one; the word "off" in it (on the PC, a value starting with 0 too): -1.
inline int TestFile(const char* name, const char* env, int default_core, bool* always = nullptr) {
    char text[64] = {};
    if (always != nullptr) {
        *always = false;
    }
#if defined(__ORBIS__)
    (void)env;
    char path[96];
    std::snprintf(path, sizeof(path), "/data/EmuSuNi/%s", name);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return default_core;
    }
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
#else
    (void)name;
    const char* value = std::getenv(env);
    if (value == nullptr) {
        return default_core;
    }
    if (value[0] == '0') {
        return -1;
    }
    std::snprintf(text, sizeof(text), "%s", value);
#endif
    for (char* c = text; *c != '\0'; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c = static_cast<char>(*c - 'A' + 'a');
        }
    }
    if (std::strstr(text, "off") != nullptr) {
        return -1;
    }
    if (always != nullptr) {
        *always = std::strstr(text, "always") != nullptr;
    }
''')
patch('src/video_core/gpu.cpp', r'''
    // (01.10) The GSP's GPU commands on a thread of their own with the file /data/EmuSuNi/3ds-async-pica
    // (video_core/emusuni_async_pica.h); the PS4 core asked of the app: the file's number, 0 by default; "always" in
    // it: never inline.
''', r'''
    // (01.10) The GSP's GPU commands on a thread of their own (video_core/emusuni_async_pica.h; by default since 01.11,
    // the word "off" in the file /data/EmuSuNi/3ds-async-pica: on the emulation thread); the PS4 core asked of the app:
    // the file's number, 0 by default; "always" in it: never inline.
''')
patch('src/video_core/gpu.cpp', r'''
        LOG_INFO(HW_GPU, "EmuSuNi: the GPU's commands on a thread of their own (3ds-async-pica; the PS4's core {}{})",
                 emusuni_core, emusuni_always ? "; always, never inline" : "");
    }
''', r'''
        LOG_INFO(HW_GPU, "EmuSuNi: the GPU's commands on a thread of their own (3ds-async-pica; the PS4's core {}{})",
                 emusuni_core, emusuni_always ? "; always, never inline" : "");
    } else {
        LOG_INFO(HW_GPU, "EmuSuNi: the GPU's commands on the emulation thread (3ds-async-pica: off)");
    }
''')
patch('src/audio_core/hle/hle.cpp', r'''
    // (01.10) The PS4 core asked of the app: the file's number, 1 by default; "always" in it: never inline.
''', r'''
    // (01.10) The PS4 core asked of the app: the file's number, 1 by default; "always" in it: never inline. By default
    // since 01.11: the word "off" in the file /data/EmuSuNi/3ds-async-dsp leaves the mixing on the emulation thread.
''')
patch('src/audio_core/hle/hle.cpp', r'''
                 "EmuSuNi: the audio frames' mixing on a thread of its own (3ds-async-dsp; the PS4's core {}{})",
                 emusuni_core, emusuni_always ? "; always, never inline" : "");
    }
''', r'''
                 "EmuSuNi: the audio frames' mixing on a thread of its own (3ds-async-dsp; the PS4's core {}{})",
                 emusuni_core, emusuni_always ? "; always, never inline" : "");
    } else {
        LOG_INFO(Audio_DSP, "EmuSuNi: the audio frames' mixing on the emulation thread (3ds-async-dsp: off)");
    }
''')

# --- 01.11 (with the threads by default): less of the emulation thread's time on what isn't the game's code (the 3DS
# specialist's 3ds-acelerar-nucleo2.md; the owner: "only the recipe for the cook"), on by default; the word off in its
# file, read at each game's start, turns each off:
# - /data/EmuSuNi/3ds-async-cpu-reads (EMUSUNI_3DS_ASYNC_CPU_READS=0 or off on the PC): a CPU read of a page the GPU's
#   cache holds (memory.cpp's CheckRegion) waits for the GPU commands' thread (01.10) and flushes only where the GPU
#   wrote something not written back yet: the cache's dirty regions, by page of 4 KiB (common/emusuni_dirty_pages.h).
#   A read of other pages (a texture's, say) had nothing to flush, so it goes on at once. On the PC no command still
#   queued ever used what the CPU read (Dark Moon, Toad, Castlevania: 6,800 such waits, 1.25 s; dbgdrain.py); a game
#   that reads what it gave the GPU without waiting for the GPU's interrupt could read it unfinished, as on a 3DS whose
#   GPU were slower. Every 600 frames a line counts the reads with and without the wait.
# - The host's CPU features (cpuid: those dynarmic can use), once per app run in the log.
patch('src/common/emusuni_async.h', '''    return default_core;
}
} // namespace EmuSuNi::Async
#endif
''', r'''    return default_core;
}

// (01.11) /data/EmuSuNi/3ds-async-cpu-reads, read at each game's start (GPU::GPU), for memory.cpp's CheckRegion.
inline bool cpu_reads = false;
} // namespace EmuSuNi::Async
#endif
''')
addfile('src/common/emusuni_dirty_pages.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.11).
// The physical pages holding something the GPU wrote and the cache hasn't written back to memory yet (the rasterizer
// cache's dirty regions, by page of 4 KiB). A CPU read of a page the GPU's cache holds (memory.cpp's CheckRegion)
// needs the GPU's work only on these; unless /data/EmuSuNi/3ds-async-cpu-reads says off, a read of the others
// neither waits for the GPU commands' thread nor flushes. The cache's own thread changes the table (the GPU commands'
// thread while it works, else the emulation thread), the emulation thread reads it: a byte per page, atomic.
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <cstdint>

namespace EmuSuNi::DirtyPages {
// From VRAM's start (0x18000000) to the end of a New 3DS's FCRAM (0x30000000): 98,304 pages.
constexpr std::uint32_t BASE = 0x18000000, END = 0x30000000, PAGE_BITS = 12;
inline std::atomic<std::uint8_t> pages[(END - BASE) >> PAGE_BITS];

// The table's pages [first, last) of [addr, addr + size); false: none.
inline bool Range(std::uint32_t addr, std::uint32_t size, std::uint32_t& first, std::uint32_t& last) {
    const std::uint64_t lo = addr, hi = std::uint64_t{addr} + size;
    if (size == 0 || hi <= BASE || lo >= END) {
        return false;
    }
    first = static_cast<std::uint32_t>(((lo < BASE ? BASE : lo) - BASE) >> PAGE_BITS);
    last = static_cast<std::uint32_t>(((hi > END ? END : hi) - BASE + (1u << PAGE_BITS) - 1) >> PAGE_BITS);
    return true;
}
// The GPU wrote [addr, addr + size) (a dirty region set).
inline void Mark(std::uint32_t addr, std::uint32_t size) {
    std::uint32_t first = 0, last = 0;
    if (Range(addr, size, first, last)) {
        for (std::uint32_t i = first; i < last; i++) {
            pages[i].store(1, std::memory_order_release);
        }
    }
}
// Dirty regions taken away from [addr, addr + size): its pages no dirty region touches any more (`still_dirty(start,
// end)` false) unmarked.
template <typename StillDirty>
inline void Unmark(std::uint32_t addr, std::uint32_t size, StillDirty still_dirty) {
    std::uint32_t first = 0, last = 0;
    if (Range(addr, size, first, last)) {
        for (std::uint32_t i = first; i < last; i++) {
            const std::uint32_t start = BASE + (i << PAGE_BITS);
            if (pages[i].load(std::memory_order_relaxed) != 0 && !still_dirty(start, start + (1u << PAGE_BITS))) {
                pages[i].store(0, std::memory_order_release);
            }
        }
    }
}
inline void UnmarkAll() {
    for (std::atomic<std::uint8_t>& page : pages) {
        page.store(0, std::memory_order_release);
    }
}
// Any page of [addr, addr + size) the GPU wrote (beyond the table: yes, to be safe).
inline bool Any(std::uint32_t addr, std::uint32_t size) {
    if (addr < BASE || std::uint64_t{addr} + size > END) {
        return true;
    }
    std::uint32_t first = 0, last = 0;
    if (!Range(addr, size, first, last)) {
        return false;
    }
    for (std::uint32_t i = first; i < last; i++) {
        if (pages[i].load(std::memory_order_acquire) != 0) {
            return true;
        }
    }
    return false;
}
} // namespace EmuSuNi::DirtyPages
#endif
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', '''#include "common/emusuni_stats.h"
''', '''#include "common/emusuni_stats.h"
#include "common/emusuni_dirty_pages.h"
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
    // Reset dirty regions
    dirty_regions -= flushed_intervals;
}
''', r'''
    // Reset dirty regions
    dirty_regions -= flushed_intervals;
#ifdef EMUSUNI_PORT
    // (01.11) The pages written back and left without a dirty region, unmarked (common/emusuni_dirty_pages.h).
    for (const auto& emusuni_interval : flushed_intervals) {
        EmuSuNi::DirtyPages::Unmark(emusuni_interval.lower(), emusuni_interval.upper() - emusuni_interval.lower(),
                                    [this](PAddr start, PAddr end) {
                                        return dirty_regions.find(SurfaceInterval(start, end)) != dirty_regions.end();
                                    });
    }
#endif
}
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
    if (region_owner_id) {
        dirty_regions.set({invalid_interval, region_owner_id});
    } else {
        dirty_regions.erase(invalid_interval);
    }
''', r'''
    if (region_owner_id) {
        dirty_regions.set({invalid_interval, region_owner_id});
#ifdef EMUSUNI_PORT
        EmuSuNi::DirtyPages::Mark(addr, size); // (01.11) common/emusuni_dirty_pages.h
#endif
    } else {
        dirty_regions.erase(invalid_interval);
#ifdef EMUSUNI_PORT
        EmuSuNi::DirtyPages::Unmark(addr, size, [this](PAddr start, PAddr end) { // (01.11)
            return dirty_regions.find(SurfaceInterval(start, end)) != dirty_regions.end();
        });
#endif
    }
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
    cached_pages -= flush_interval;
    dirty_regions.clear();
''', r'''
    cached_pages -= flush_interval;
    dirty_regions.clear();
#ifdef EMUSUNI_PORT
    EmuSuNi::DirtyPages::UnmarkAll(); // (01.11)
#endif
''')
patch('src/core/memory.cpp', '''#include "common/emusuni_async.h"
''', '''#include "common/emusuni_async.h"
#include "common/emusuni_dirty_pages.h"
''')
patch('src/core/memory.cpp', r'''
#ifdef EMUSUNI_PORT
            // (01.10) The GPU's cache is its commands' thread's while it works (3ds-async-pica): after its commands.
            EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainMemory);
#endif
''', r'''
#ifdef EMUSUNI_PORT
            // (01.11; 3ds-async-cpu-reads) A read of pages the GPU hasn't written (a texture's, say:
            // common/emusuni_dirty_pages.h) has nothing to flush: neither the flush nor its wait.
            if (mode == FlushMode::Flush && EmuSuNi::Async::cpu_reads) {
                const VAddr emusuni_start = std::max(start, region_start);
                const VAddr emusuni_end = std::min(end, region_end);
                if (!EmuSuNi::DirtyPages::Any(paddr_region_start + (emusuni_start - region_start),
                                              emusuni_end - emusuni_start)) {
                    EmuSuNi::Stats::counts[EmuSuNi::Stats::CpuReadsGo]++;
                    return;
                }
                EmuSuNi::Stats::counts[EmuSuNi::Stats::CpuReadsDirty]++;
            }
            // (01.10) The GPU's cache is its commands' thread's while it works (3ds-async-pica): after its commands.
            EmuSuNi::Async::Drain(EmuSuNi::Async::PicaSlot, EmuSuNi::Async::DrainMemory);
#endif
''')
patch('src/common/emusuni_stats.h', r'''
    PicaInlineFrames, PicaInlineSwitches, DspFrames, DspWaits, DspStolen, DspInlineFrames, DspSwitches,
    CountCount
''', r'''
    PicaInlineFrames, PicaInlineSwitches, DspFrames, DspWaits, DspStolen, DspInlineFrames, DspSwitches,
    CpuReadsGo, CpuReadsDirty, // (01.11) 3ds-async-cpu-reads: the CPU's reads of the GPU's pages without, with a wait
    CountCount
''')
patch('src/common/emusuni_frame_stats.h', r'''
            period_t0 = now_us;
            period_k0 = now_k;
''', r'''
            // (01.11) The CPU's reads of pages the GPU's cache holds, with 3ds-async-cpu-reads (memory.cpp).
            if (dc[CpuReadsGo] != 0 || dc[CpuReadsDirty] != 0) {
                LOG_INFO(Frontend,
                         "EmuSuNi: the CPU's reads of the GPU's memory in those 600 frames (3ds-async-cpu-reads): "
                         "{} at once (the GPU hadn't written those pages), {} where it had (flushed, after its "
                         "commands)",
                         dc[CpuReadsGo], dc[CpuReadsDirty]);
            }
            period_t0 = now_us;
            period_k0 = now_k;
''')
patch('src/video_core/gpu.cpp', '''#include "video_core/emusuni_async_pica.h"
''', '''#include "video_core/emusuni_async_pica.h"
#include "common/emusuni_dirty_pages.h"
''')
patch('src/video_core/gpu.cpp', r'''
        LOG_INFO(HW_GPU, "EmuSuNi: the GPU's commands on the emulation thread (3ds-async-pica: off)");
    }
#endif
}
''', r'''
        LOG_INFO(HW_GPU, "EmuSuNi: the GPU's commands on the emulation thread (3ds-async-pica: off)");
    }
    // (01.11) The CPU's reads of the GPU's memory wait only where the GPU wrote (memory.cpp), unless the file
    // /data/EmuSuNi/3ds-async-cpu-reads says off; a new game: no page written yet.
    EmuSuNi::DirtyPages::UnmarkAll();
    EmuSuNi::Async::cpu_reads =
        EmuSuNi::Async::TestFile("3ds-async-cpu-reads", "EMUSUNI_3DS_ASYNC_CPU_READS", 0) >= 0;
    LOG_INFO(HW_GPU, "EmuSuNi: the CPU's reads of the GPU's memory {} (3ds-async-cpu-reads)",
             EmuSuNi::Async::cpu_reads ? "wait only where the GPU wrote" : "wait for its thread as in 01.10: off");
#endif
}
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', '''#include "common/emusuni_stats.h"
''', '''#include "common/emusuni_stats.h"
#include <cpuid.h>
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    config.define_unpredictable_behaviour = true;
#ifdef EMUSUNI_PORT
    // EmuSuNi (01.06): the test switch 3ds-cpu-unsafe-fp, dynarmic's unsafe floating-point shortcuts.
''', r'''
    config.define_unpredictable_behaviour = true;
#ifdef EMUSUNI_PORT
    // (01.11) The host's CPU features dynarmic can use (it asks cpuid itself), once per app run in the log: on the
    // PS4 the Jaguar's (no AVX2, BMI2 or FMA).
    static const bool emusuni_cpu_said = [] {
        unsigned a = 0, b = 0, c = 0, d = 0;
        unsigned c1 = 0, b7 = 0, cx = 0;
        if (__get_cpuid(1, &a, &b, &c, &d) != 0) {
            c1 = c;
        }
        if (__get_cpuid_count(7, 0, &a, &b, &c, &d) != 0) {
            b7 = b;
        }
        if (__get_cpuid(0x80000001u, &a, &b, &c, &d) != 0) {
            cx = c;
        }
        bool os_avx = false;
        if ((c1 & (1u << 27)) != 0) { // OSXSAVE: the system saves the AVX registers if XCR0 says so
            unsigned lo = 0, hi = 0;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            os_avx = (lo & 6u) == 6u;
        }
        const auto yes = [](unsigned bits, int bit) { return (bits & (1u << bit)) != 0 ? "yes" : "no"; };
        LOG_INFO(Core_ARM11,
                 "EmuSuNi: the host's CPU (cpuid): SSE4.1 {}, SSE4.2 {}, AVX {} (saved by the system: {}), AVX2 {}, "
                 "FMA {}, F16C {}, BMI1 {}, BMI2 {}, LZCNT {}, POPCNT {}, MOVBE {}, AES {}, CLMUL {}",
                 yes(c1, 19), yes(c1, 20), yes(c1, 28), os_avx ? "yes" : "no", yes(b7, 5), yes(c1, 12),
                 yes(c1, 29), yes(b7, 3), yes(b7, 8), yes(cx, 5), yes(c1, 23), yes(c1, 22), yes(c1, 25),
                 yes(c1, 1));
        return true;
    }();
    (void)emusuni_cpu_said;
    // EmuSuNi (01.06): the test switch 3ds-cpu-unsafe-fp, dynarmic's unsafe floating-point shortcuts.
''')

# --- 01.11: the game's idle loops skipped (on by default; the word off in /data/EmuSuNi/3ds-cpu-idle-loops, or
# EMUSUNI_3DS_CPU_IDLE_LOOPS=0 on the PC, turns it off; the 3DS specialist's 3ds-acelerar-nucleo2.md). A short loop of
# the game that only waits
# (Donkey Kong Country Returns 3D at 0x001156CC: a flag set, a counter read until another thread of the game gives it
# work; 16 % of the emulation thread on the PC) can't end within a slice of the emulated core: what it reads changes
# only when the other cores, the events or the kernel run, between slices. So when one turn of it leaves every
# register as the turn before, the rest of the slice is spent at once, as its turns would have spent it: the same
# emulated time, without running them.
# - dynarmic (frontend/A32/translate/translate_arm.cpp): an ARM block ending in a branch back of at most 64 bytes whose
#   loop holds only unconditional data processing, loads with an offset and stores of a register a MOV of a constant
#   set in the loop (and a load at least) raises A32::Exception::EmuSuNiIdleLoop on each turn back.
# - core/arm/dynarmic/arm_dynarmic.cpp: on it, the registers (r0-r14, CPSR) as the turn before at the same place: the
#   slice's remaining ticks added at once (dynarmic reads them again after the call and leaves the slice).
# - Every 600 frames a line: the loops skipped and the share of the 3DS CPU's cycles they took.
patch('externals/dynarmic/src/dynarmic/interface/A32/config.h', r'''
    NoExecuteFault,
};
''', r'''
    NoExecuteFault,
#ifdef EMUSUNI_PORT
    /// EmuSuNi (01.11): a turn of a short loop that only waits (frontend/A32/translate/translate_arm.cpp).
    EmuSuNiIdleLoop,
#endif
};
#ifdef EMUSUNI_PORT
/// EmuSuNi (01.11): the game's idle loops marked as they are translated (/data/EmuSuNi/3ds-cpu-idle-loops: off).
inline bool emusuni_idle_loops = false;
#endif
''')
patch('externals/dynarmic/src/dynarmic/frontend/A32/translate/translate_arm.cpp', r'''
namespace Dynarmic::A32 {

IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
''', r'''
namespace Dynarmic::A32 {

#ifdef EMUSUNI_PORT
// EmuSuNi (01.11): an instruction of an idle loop's body: unconditional data processing (no PC written, no shift by a
// register), a load with an immediate offset (no write-back, not into PC), or a store of a register a MOV/MVN of a
// constant set earlier in the loop (`constants`, by register). False: not such a loop.
static bool EmuSuNiIdleBody(u32 insn, u32& constants, int& loads) {
    if ((insn >> 28) != 0xE) {
        return false;
    }
    if (insn == 0xE320F000) { // NOP
        return true;
    }
    const u32 rd = (insn >> 12) & 15;
    const u32 op = (insn >> 21) & 15;
    const bool s = ((insn >> 20) & 1) != 0;
    const bool dp_imm = (insn & 0x0E000000) == 0x02000000;
    const bool dp_reg = (insn & 0x0E000010) == 0x00000000;
    if ((dp_imm || dp_reg) && !(op >= 8 && op <= 11 && !s)) {
        if (op >= 8 && op <= 11) { // TST TEQ CMP CMN
            return true;
        }
        if (rd == 15 || (op != 0 && op != 1 && op != 2 && op != 4 && op != 12 && op != 13 && op != 14 && op != 15)) {
            return false; // (PC written; RSB, ADC, SBC, RSC)
        }
        if (dp_imm && (op == 13 || op == 15)) { // MOV/MVN of a constant
            constants |= 1u << rd;
        } else {
            constants &= ~(1u << rd);
        }
        return true;
    }
    const bool p = ((insn >> 24) & 1) != 0, w = ((insn >> 21) & 1) != 0, load = ((insn >> 20) & 1) != 0;
    const bool word_byte = (insn & 0x0E000000) == 0x04000000;                               // LDR/STR(B), imm offset
    const bool half = (insn & 0x0E400090) == 0x00400090 && ((insn >> 5) & 3) != 0;          // LDRH/LDRSB/LDRSH/STRH
    if ((word_byte || half) && p && !w && rd != 15) {
        if (load) {
            constants &= ~(1u << rd);
            loads++;
            return true;
        }
        return (!half || ((insn >> 5) & 3) == 1) && ((constants >> rd) & 1) != 0;
    }
    return false;
}

// EmuSuNi (01.11): a block ending in a branch (B) back of at most 64 bytes over such a body: each turn back raises
// EmuSuNiIdleLoop (the block's own code runs only on that turn when the branch is conditional: dynarmic gives a
// conditional instruction a block of its own).
static void EmuSuNiMarkIdleLoop(IR::Block& block, TranslatorVisitor& visitor, TranslateCallbacks* tcb) {
    u32 target = 0;
    const IR::Terminal terminal = block.GetTerminal();
    if (const auto* link = boost::get<IR::Term::LinkBlock>(&terminal)) {
        target = LocationDescriptor{link->next}.PC();
    } else if (const auto* fast = boost::get<IR::Term::LinkBlockFast>(&terminal)) {
        target = LocationDescriptor{fast->next}.PC();
    } else {
        return;
    }
    const u32 branch_pc = visitor.ir.current_location.PC() - 4;
    if (target > branch_pc || branch_pc - target > 64) {
        return;
    }
    const auto branch = tcb->MemoryReadCode(branch_pc);
    if (!branch || (*branch & 0x0F000000) != 0x0A000000) { // B, not BL
        return;
    }
    u32 constants = 0;
    int loads = 0;
    for (u32 pc = target; pc < branch_pc; pc += 4) {
        const auto insn = tcb->MemoryReadCode(pc);
        if (!insn || !EmuSuNiIdleBody(*insn, constants, loads)) {
            return;
        }
    }
    if (loads != 0) {
        visitor.ir.ExceptionRaised(Exception::EmuSuNiIdleLoop);
    }
}
#endif

IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
''')
patch('externals/dynarmic/src/dynarmic/frontend/A32/translate/translate_arm.cpp', r'''
    block.SetEndLocation(visitor.ir.current_location);

    return block;
}

bool TranslateSingleArmInstruction(''', r'''
    block.SetEndLocation(visitor.ir.current_location);
#ifdef EMUSUNI_PORT
    if (emusuni_idle_loops && !single_step) {
        EmuSuNiMarkIdleLoop(block, visitor, tcb); // (01.11)
    }
#endif

    return block;
}

bool TranslateSingleArmInstruction(''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', '''#include <cpuid.h>
''', '''#include <cpuid.h>
#include "common/emusuni_async.h"
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
''', r'''
    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
#ifdef EMUSUNI_PORT
        if (exception == Dynarmic::A32::Exception::EmuSuNiIdleLoop) {
            EmuSuNiIdleLoop(pc); // (01.11)
            return;
        }
#endif
        switch (exception) {
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};
''', r'''
#ifdef EMUSUNI_PORT
    // (01.11) A turn of an idle loop (dynarmic marks them, 3ds-cpu-idle-loops): when it left every register as the
    // turn before at the same place, nothing in this slice can change what the loop reads, so the slice's remaining
    // ticks are spent at once (dynarmic reads them again after this call and leaves the slice).
    void EmuSuNiIdleLoop(VAddr pc) {
        std::array<u32, 16> regs = parent.jit->Regs();
        regs[15] = 0;
        const u32 cpsr = parent.jit->Cpsr();
        if (emusuni_idle_pc == pc && regs == emusuni_idle_regs && cpsr == emusuni_idle_cpsr) {
            const std::uint64_t left = GetTicksRemaining();
            if (left != 0) {
                AddTicks(left);
                EmuSuNi::Stats::counts[EmuSuNi::Stats::IdleLoopSkips]++;
                EmuSuNi::Stats::counts[EmuSuNi::Stats::IdleLoopCycles] += left;
            }
            emusuni_idle_pc = ~VAddr{0};
            return;
        }
        emusuni_idle_pc = pc;
        emusuni_idle_regs = regs;
        emusuni_idle_cpsr = cpsr;
    }
    VAddr emusuni_idle_pc = ~VAddr{0};
    std::array<u32, 16> emusuni_idle_regs{};
    u32 emusuni_idle_cpsr = 0;
#endif

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    (void)emusuni_cpu_said;
''', r'''
    (void)emusuni_cpu_said;
    // (01.11) The game's idle loops skipped unless /data/EmuSuNi/3ds-cpu-idle-loops says off (dynarmic's translator).
    Dynarmic::A32::emusuni_idle_loops =
        EmuSuNi::Async::TestFile("3ds-cpu-idle-loops", "EMUSUNI_3DS_CPU_IDLE_LOOPS", 0) >= 0;
    if (Dynarmic::A32::emusuni_idle_loops) {
        LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the game's idle loops skipped (3ds-cpu-idle-loops)", GetID());
    }
''')
patch('src/common/emusuni_stats.h', r'''
    CpuReadsGo, CpuReadsDirty, // (01.11) 3ds-async-cpu-reads: the CPU's reads of the GPU's pages without, with a wait
''', r'''
    CpuReadsGo, CpuReadsDirty, // (01.11) 3ds-async-cpu-reads: the CPU's reads of the GPU's pages without, with a wait
    IdleLoopSkips, IdleLoopCycles, // (01.11) 3ds-cpu-idle-loops: the slices ended at an idle loop, their cycles
''')
patch('src/common/emusuni_frame_stats.h', r'''
            // (01.11) The CPU's reads of pages the GPU's cache holds, with 3ds-async-cpu-reads (memory.cpp).
''', r'''
            // (01.11) The game's idle loops skipped (3ds-cpu-idle-loops): of the 3DS CPU's 268,111,856 cycles a
            // second, at 59.83 frames a second.
            if (dc[IdleLoopSkips] != 0) {
                LOG_INFO(Frontend,
                         "EmuSuNi: the game's idle loops in those 600 frames (3ds-cpu-idle-loops): {} skipped, {} "
                         "million cycles of the 3DS CPU not run ({} per cent of its time)",
                         dc[IdleLoopSkips], dc[IdleLoopCycles] / 1000000,
                         dc[IdleLoopCycles] * 100 / (268111856ull * 600 * 100 / 5983));
            }
            // (01.11) The CPU's reads of pages the GPU's cache holds, with 3ds-async-cpu-reads (memory.cpp).
''')

# --- 01.11: the 3DS CPU's clock per game while the app's 3DS CPU Clock says 100 % (the owner's idea 4; the 3DS
# specialist's 3ds-acelerar-nucleo2.md): Castlevania: Lords of Shadow - Mirror of Fate (USA) at 50. In its busy parts
# on the PC that is 43 % less work a frame, with the same scenes at the same frames and the same sound: its own code
# keeps the CPU busy and it still draws as often. The games that wait for their next picture gain nothing (Toad,
# Pikmin, 3D Land), DKC little once its idle loops are skipped. The word off in /data/EmuSuNi/3ds-cpu-clock-games
# (EMUSUNI_3DS_CPU_CLOCK_GAMES=0 on the PC) leaves the app's clock. And as each game starts, a line with every speed
# switch of 01.11 as the game found it (core/emusuni_switches.h).
addfile('src/core/emusuni_switches.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.11).
// The 3DS CPU's clock per game, and the line saying 01.11's speed switches as a game starts (core.cpp, savestate.cpp).
#pragma once
#ifdef EMUSUNI_PORT
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/emusuni_async.h"
#include "common/logging/log.h"
#include "common/settings.h"

namespace Core {
// The clock (per cent) EmuSuNi gives a game while the app's 3DS CPU Clock says 100: the games whose own code keeps the
// CPU busy and that still draw as often at a lower clock.
inline u32 EmuSuNiGameClock(u64 title_id) {
    switch (title_id) {
    case 0x0004000000096600: // Castlevania: Lords of Shadow - Mirror of Fate (USA)
        return 50;
    default:
        return 100;
    }
}
// The clock a game runs at: the app's 3DS CPU Clock, or, while that says 100, EmuSuNi's own for the game when the file
// /data/EmuSuNi/3ds-cpu-clock-games is there without the word off in it (EMUSUNI_3DS_CPU_CLOCK_GAMES set on the PC).
// (01.12: on only with the file. At 50 % Castlevania walked in jumps on the owner's PS4 and things came and went about
// every second: the game does less in each frame, which a saved state's scene didn't show.)
inline u32 EmuSuNiClock(u64 title_id, u32 percentage) {
#if defined(__ORBIS__)
    const bool file = ::access("/data/EmuSuNi/3ds-cpu-clock-games", F_OK) == 0;
#else
    const bool file = std::getenv("EMUSUNI_3DS_CPU_CLOCK_GAMES") != nullptr;
#endif
    if (percentage != 100 || !file ||
        EmuSuNi::Async::TestFile("3ds-cpu-clock-games", "EMUSUNI_3DS_CPU_CLOCK_GAMES", 0) < 0) {
        return percentage;
    }
    return EmuSuNiGameClock(title_id);
}
// As a game starts: 01.11's speed switches as it found them (each a file in /data/EmuSuNi; the word off in it turns
// it off; the unsafe floating point is on with its file only).
inline void EmuSuNiLogSwitches(u64 title_id) {
    bool pica_always = false, dsp_always = false;
    const int pica = EmuSuNi::Async::TestFile("3ds-async-pica", "EMUSUNI_3DS_ASYNC_PICA", 0, &pica_always);
    const int dsp = EmuSuNi::Async::TestFile("3ds-async-dsp", "EMUSUNI_3DS_ASYNC_DSP", 1, &dsp_always);
    const auto thread = [](int core, bool always) {
        return core < 0 ? std::string("off") : fmt::format("on (core {}{})", core, always ? ", always" : "");
    };
    const auto on = [](bool yes) { return yes ? "on" : "off"; };
    const u32 app_clock = Settings::values.cpu_clock_percentage.GetValue();
    const u32 clock = EmuSuNiClock(title_id, app_clock);
    const char* fp = std::getenv("EMUSUNI_3DS_CPU_UNSAFE_FP");
    const bool unsafe_fp =
        fp != nullptr ? std::strcmp(fp, "0") != 0 : ::access("/data/EmuSuNi/3ds-cpu-unsafe-fp", F_OK) == 0;
    LOG_INFO(Core,
             "EmuSuNi: the speed switches for {:016X} (01.11; each off with the word off in its file in "
             "/data/EmuSuNi): the GPU commands' thread {} (3ds-async-pica), the DSP's mixing thread {} "
             "(3ds-async-dsp), the CPU's reads of the GPU's memory without needless waits {} (3ds-async-cpu-reads), "
             "the game's idle loops skipped {} (3ds-cpu-idle-loops), the 3DS CPU at {} per cent ({}; "
             "3ds-cpu-clock-games), dynarmic's unsafe floating point {} (3ds-cpu-unsafe-fp, on with the file)",
             title_id, thread(pica, pica_always), thread(dsp, dsp_always),
             on(EmuSuNi::Async::TestFile("3ds-async-cpu-reads", "EMUSUNI_3DS_ASYNC_CPU_READS", 0) >= 0),
             on(EmuSuNi::Async::TestFile("3ds-cpu-idle-loops", "EMUSUNI_3DS_CPU_IDLE_LOOPS", 0) >= 0), clock,
             clock == app_clock ? "the app's 3DS CPU Clock" : "EmuSuNi's own for this game", on(unsafe_fp));
}
} // namespace Core
#endif
''')
patch('src/core/core.cpp', '''#include "common/emusuni_async.h"
''', '''#include "common/emusuni_async.h"
#include "core/emusuni_switches.h"
''')
patch('src/core/core.cpp', r'''
    cheat_engine.LoadCheatFile(title_id);
    cheat_engine.Connect(process->process_id);
''', r'''
#ifdef EMUSUNI_PORT
    // (01.11) This game's 3DS CPU clock (core/emusuni_switches.h) and its speed switches, in the log.
    timing->UpdateClockSpeed(EmuSuNiClock(title_id, Settings::values.cpu_clock_percentage.GetValue()));
    EmuSuNiLogSwitches(title_id);
#endif
    cheat_engine.LoadCheatFile(title_id);
    cheat_engine.Connect(process->process_id);
''')
patch('src/core/core.cpp', r'''
        CoreTiming().UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());
''', r'''
#ifdef EMUSUNI_PORT
        CoreTiming().UpdateClockSpeed(EmuSuNiClock(title_id, Settings::values.cpu_clock_percentage.GetValue()));
#else
        CoreTiming().UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());
#endif
''')
patch('src/core/savestate.cpp', '''#include "common/zstd_compression.h"
''', '''#include "common/zstd_compression.h"
#ifdef EMUSUNI_PORT
#include "core/emusuni_switches.h"
#endif
''')
patch('src/core/savestate.cpp', r'''
        timing->UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());
''', r'''
        timing->UpdateClockSpeed(EmuSuNiClock(title_id, Settings::values.cpu_clock_percentage.GetValue())); // (01.11)
''')

# --- 01.12 (on by default; the word off in its file in /data/EmuSuNi, or its variable at 0 on the PC, turns each off;
# and 01.11's 3ds-cpu-clock-games now on only with its file, core/emusuni_switches.h):
# - 3ds-async-cpu-writes (EMUSUNI_3DS_ASYNC_CPU_WRITES): the CPU's writes to the GPU's memory don't wait for the GPU
#   commands' thread. In 01.11's test Dark Moon still waited 2 to 6.5 ms a frame there: the CPU writing into pages the
#   GPU's cache holds (a texture's, say: memory.cpp's CheckRegion, Invalidate). While that thread has commands to run
#   and the GPU wrote nothing in those pages (common/emusuni_dirty_pages.h), the pages' invalidation goes into its
#   queue after them, in order: the commands sent before the write still see the old texture in the cache, those sent
#   after it the new one in memory. The pages waiting for it are marked, so more writes to them don't queue it again.
# - 3ds-jit-warmup-wide (EMUSUNI_3DS_JIT_WARMUP_WIDE): the JIT's warm-up compiles ahead the list's first 120,000 blocks,
#   not 60,000 (01.01), still leaving half of the code cache to the game. In 01.11's test nearly every frame over 100 ms
#   was the game meeting thousands of blocks the warm-up had left (Dark Moon: 62,539 of 122,539 left; frames of 100 to
#   1,400 ms compiling 1,500 to 21,000 blocks); the list's blocks now come during the first seconds instead.
patch('src/common/emusuni_dirty_pages.h', r'''
} // namespace EmuSuNi::DirtyPages
#endif
''', r'''
// (01.12) The pages whose invalidation waits in the GPU commands' thread's queue (memory.cpp, gpu.cpp:
// 3ds-async-cpu-writes): more writes to them don't queue it again.
inline std::atomic<std::uint8_t> pending[(END - BASE) >> PAGE_BITS];
inline bool AllPending(std::uint32_t addr, std::uint32_t size) {
    std::uint32_t first = 0, last = 0;
    if (!Range(addr, size, first, last)) {
        return false;
    }
    for (std::uint32_t i = first; i < last; i++) {
        if (pending[i].load(std::memory_order_acquire) == 0) {
            return false;
        }
    }
    return true;
}
inline void SetPending(std::uint32_t addr, std::uint32_t size, bool value) {
    std::uint32_t first = 0, last = 0;
    if (Range(addr, size, first, last)) {
        for (std::uint32_t i = first; i < last; i++) {
            pending[i].store(value ? 1 : 0, std::memory_order_release);
        }
    }
}
} // namespace EmuSuNi::DirtyPages
#endif
''')
patch('src/video_core/emusuni_async_pica.h', '''#include "common/emusuni_async.h"
''', '''#include "common/emusuni_async.h"
#include "common/emusuni_dirty_pages.h"
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    using Mark = std::function<void(std::uint32_t start, std::uint32_t size, bool cached)>;
''', r'''
    using Mark = std::function<void(std::uint32_t start, std::uint32_t size, bool cached)>;
    // (01.12) The cache's invalidation of pages the CPU wrote (3ds-async-cpu-writes): queued after the commands.
    using Invalidate = std::function<void(std::uint32_t start, std::uint32_t size)>;
    struct Item {
        Service::GSP::Command command;
        std::uint32_t invalidate_start = 0, invalidate_size = 0; // (01.12) size 0: a command
    };
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    void Submit(const Service::GSP::Command& command) {
        outstanding.fetch_add(1, std::memory_order_acq_rel);
        unfinished.fetch_add(1, std::memory_order_acq_rel);
        bool wake;
        {
            std::lock_guard lock{m};
            queue.push_back(command);
''', r'''
    // (01.12) 3ds-async-cpu-writes: the invalidation (gpu.cpp's), the commands still to run (queued or running).
    void SetInvalidate(Invalidate invalidate_) {
        invalidate = std::move(invalidate_);
    }
    unsigned Unfinished() const {
        return unfinished.load(std::memory_order_acquire);
    }
    void SubmitInvalidate(std::uint32_t start, std::uint32_t size) {
        Item item;
        item.invalidate_start = start;
        item.invalidate_size = size;
        Push(item);
    }

    void Submit(const Service::GSP::Command& command) {
        Item item;
        item.command = command;
        Push(item);
    }
    void Push(const Item& item) {
        outstanding.fetch_add(1, std::memory_order_acq_rel);
        unfinished.fetch_add(1, std::memory_order_acq_rel);
        bool wake;
        {
            std::lock_guard lock{m};
            queue.push_back(item);
''')
patch('src/video_core/emusuni_async_pica.h', r'''
            const Service::GSP::Command command = queue.front();
''', r'''
            const Item item = queue.front();
''')
patch('src/video_core/emusuni_async_pica.h', r'''
                RunOne(command);
''', r'''
                RunOne(item);
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    void RunOne(const Service::GSP::Command& command) {
''', r'''
    void RunOne(const Item& item) {
''')
patch('src/video_core/emusuni_async_pica.h', r'''
            execute(command);
''', r'''
            if (item.invalidate_size != 0) { // (01.12)
                DirtyPages::SetPending(item.invalidate_start, item.invalidate_size, false);
                invalidate(item.invalidate_start, item.invalidate_size);
            } else {
                execute(item.command);
            }
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    std::deque<Service::GSP::Command> queue;
''', r'''
    std::deque<Item> queue;
    Invalidate invalidate; // (01.12)
''')
patch('src/video_core/gpu.h', r'''
    void EmuSuNiDrain(int why);
''', r'''
    void EmuSuNiDrain(int why);

public:
    // (01.12) 3ds-async-cpu-writes: the cache's invalidation of [addr, addr + size) queued after the GPU commands'
    // thread's commands; false: it has none to run (the caller invalidates at once).
    bool EmuSuNiQueueInvalidate(u32 addr, u32 size);

private:
''')
patch('src/video_core/gpu.cpp', r'''
            [this](u32 start, u32 size, bool cached) {
                impl->memory.RasterizerMarkRegionCached(start, size, cached);
            });
''', r'''
            [this](u32 start, u32 size, bool cached) {
                impl->memory.RasterizerMarkRegionCached(start, size, cached);
            });
        emusuni_pica->SetInvalidate([this](u32 start, u32 size) { // (01.12)
            Renderer().Rasterizer()->InvalidateRegion(start, size);
        });
''')
patch('src/video_core/gpu.cpp', r'''
void GPU::EmuSuNiDrain(int why) {
''', r'''
bool GPU::EmuSuNiQueueInvalidate(u32 addr, u32 size) {
    if (!emusuni_pica || EmuSuNi::Async::OnPicaThread() || emusuni_pica->Unfinished() == 0) {
        return false;
    }
    EmuSuNi::DirtyPages::SetPending(addr, size, true);
    emusuni_pica->SubmitInvalidate(addr, size);
    return true;
}

void GPU::EmuSuNiDrain(int why) {
''')
patch('src/video_core/gpu.cpp', r'''
             EmuSuNi::Async::cpu_reads ? "wait only where the GPU wrote" : "wait for its thread as in 01.10: off");
''', r'''
             EmuSuNi::Async::cpu_reads ? "wait only where the GPU wrote" : "wait for its thread as in 01.10: off");
    // (01.12) Its writes too, unless /data/EmuSuNi/3ds-async-cpu-writes says off; no page waiting for one yet.
    EmuSuNi::Async::cpu_writes =
        EmuSuNi::Async::TestFile("3ds-async-cpu-writes", "EMUSUNI_3DS_ASYNC_CPU_WRITES", 0) >= 0;
    for (std::atomic<std::uint8_t>& page : EmuSuNi::DirtyPages::pending) {
        page.store(0, std::memory_order_relaxed);
    }
''')
patch('src/common/emusuni_async.h', r'''
inline bool cpu_reads = false;
''', r'''
inline bool cpu_reads = false;
// (01.12) /data/EmuSuNi/3ds-async-cpu-writes (GPU::GPU): memory.cpp's CheckRegion, its Invalidate.
inline bool cpu_writes = false;
''')
patch('src/core/memory.cpp', r'''
                EmuSuNi::Stats::counts[EmuSuNi::Stats::CpuReadsDirty]++;
            }
''', r'''
                EmuSuNi::Stats::counts[EmuSuNi::Stats::CpuReadsDirty]++;
            }
            // (01.12; 3ds-async-cpu-writes) A write into pages the GPU hasn't written while the GPU commands' thread
            // has commands to run: the pages' invalidation goes into its queue after them (gpu.cpp), once.
            if (mode == FlushMode::Invalidate && EmuSuNi::Async::cpu_writes) {
                const VAddr emusuni_start = std::max(start, region_start);
                const VAddr emusuni_end = std::min(end, region_end);
                const PAddr emusuni_first = (paddr_region_start + (emusuni_start - region_start)) & ~PAddr{0xFFF};
                const PAddr emusuni_last = (paddr_region_start + (emusuni_end - region_start) + 0xFFF) & ~PAddr{0xFFF};
                const u32 emusuni_size = emusuni_last - emusuni_first;
                if (!EmuSuNi::DirtyPages::Any(emusuni_first, emusuni_size)) {
                    if (EmuSuNi::DirtyPages::AllPending(emusuni_first, emusuni_size)) {
                        EmuSuNi::Stats::counts[EmuSuNi::Stats::CpuWritesPending]++;
                        return;
                    }
                    if (system.GPU().EmuSuNiQueueInvalidate(emusuni_first, emusuni_size)) {
                        EmuSuNi::Stats::counts[EmuSuNi::Stats::CpuWritesQueued]++;
                        return;
                    }
                }
            }
''')
patch('src/common/emusuni_stats.h', r'''
    IdleLoopSkips, IdleLoopCycles, // (01.11) 3ds-cpu-idle-loops: the slices ended at an idle loop, their cycles
''', r'''
    IdleLoopSkips, IdleLoopCycles, // (01.11) 3ds-cpu-idle-loops: the slices ended at an idle loop, their cycles
    CpuWritesQueued, CpuWritesPending, // (01.12) 3ds-async-cpu-writes: invalidations queued, writes to queued pages
''')
patch('src/common/emusuni_frame_stats.h', r'''
                         dc[CpuReadsGo], dc[CpuReadsDirty]);
            }
''', r'''
                         dc[CpuReadsGo], dc[CpuReadsDirty]);
            }
            // (01.12) Its writes, with 3ds-async-cpu-writes (memory.cpp, gpu.cpp).
            if (dc[CpuWritesQueued] != 0 || dc[CpuWritesPending] != 0) {
                LOG_INFO(Frontend,
                         "EmuSuNi: the CPU's writes to the GPU's memory in those 600 frames (3ds-async-cpu-writes): "
                         "{} invalidations queued after the GPU commands' thread's commands, {} writes to pages "
                         "already queued",
                         dc[CpuWritesQueued], dc[CpuWritesPending]);
            }
''')
patch('src/citra_libretro/emusuni_jitwarm.h', '''#include "common/hash.h"
''', '''#include "common/emusuni_async.h"
#include "common/hash.h"
''')
patch('src/citra_libretro/emusuni_jitwarm.h', r'''
    const std::size_t ahead_end = std::min(g.loaded.size(), kAheadBlocks);
''', r'''
    // (01.12) 120,000 unless /data/EmuSuNi/3ds-jit-warmup-wide says off (half of the code cache still the game's).
    static const std::size_t emusuni_ahead =
        EmuSuNi::Async::TestFile("3ds-jit-warmup-wide", "EMUSUNI_3DS_JIT_WARMUP_WIDE", 0) >= 0 ? 120000 : kAheadBlocks;
    const std::size_t ahead_end = std::min(g.loaded.size(), emusuni_ahead);
''')
patch('src/citra_libretro/emusuni_jitwarm.h', r'''
                     g.loaded.size() - g.next, kAheadBlocks);
''', r'''
                     g.loaded.size() - g.next, emusuni_ahead);
''')
patch('src/core/emusuni_switches.h', r'''
             "EmuSuNi: the speed switches for {:016X} (01.11; each off with the word off in its file in "
''', r'''
             "EmuSuNi: the speed switches for {:016X} (01.12; each off with the word off in its file in "
''')
patch('src/core/emusuni_switches.h', r'''
             "(3ds-async-dsp), the CPU's reads of the GPU's memory without needless waits {} (3ds-async-cpu-reads), "
''', r'''
             "(3ds-async-dsp), the CPU's reads of the GPU's memory without needless waits {} (3ds-async-cpu-reads), "
             "its writes {} (3ds-async-cpu-writes), the JIT's wide warm-up {} (3ds-jit-warmup-wide), "
''')
patch('src/core/emusuni_switches.h', r'''
             on(EmuSuNi::Async::TestFile("3ds-async-cpu-reads", "EMUSUNI_3DS_ASYNC_CPU_READS", 0) >= 0),
''', r'''
             on(EmuSuNi::Async::TestFile("3ds-async-cpu-reads", "EMUSUNI_3DS_ASYNC_CPU_READS", 0) >= 0),
             on(EmuSuNi::Async::TestFile("3ds-async-cpu-writes", "EMUSUNI_3DS_ASYNC_CPU_WRITES", 0) >= 0),
             on(EmuSuNi::Async::TestFile("3ds-jit-warmup-wide", "EMUSUNI_3DS_JIT_WARMUP_WIDE", 0) >= 0),
''')

# --- 01.13: the 3DS GPU's time per game (only with the file /data/EmuSuNi/3ds-gpu-timing-games without the word off in
# it; EMUSUNI_3DS_GPU_TIMING_GAMES on the PC). Azahar's GPU timing emulation (00.85: simulate_3ds_gpu_timings) signals
# a command list's end after the time its estimate gives (video_core/pica/pica_core.h's DelayGenerator, which takes the
# GPU as twice the 3DS's); without it the end comes at once. A game that draws as often as it can then ends each
# picture within a frame. In the 3DS specialist's PC tests (3ds-acelerar-nucleo2.md), from saved states, on the move:
# - Castlevania: Mirror of Fate drew a new picture every frame and ~4 times the draws a frame it makes with the timing
#   on, where it draws a picture every other frame (30 a second, as a 3DS): a third of the work a frame.
# - Monster Hunter 4 Ultimate and Luigi's Mansion: Dark Moon drew 60 pictures a second even at Azahar's estimate, 30 at
#   twice it (the 3DS's own by it), with 40 to 45 per cent less work a frame; Castlevania at twice it drew 15 to 20.
# The games' own time ran as before (the same scene at the same frame). So, for those games, the timing on at that per
# cent of the estimate: Castlevania 100, Monster Hunter and Dark Moon 200 (common/emusuni_gpu_time.h,
# core/emusuni_switches.h); a number (25 to 400) in the file /data/EmuSuNi/3ds-gpu-timings (00.85's: the timing for
# every game) sets every other game's. The speed switches' line, which the app's log cuts at ~520 characters, is two
# lines now: the second says the 3DS CPU's clock, the GPU's time and the unsafe floating point.
addfile('src/common/emusuni_gpu_time.h', r'''// EmuSuNi (scripts/build-3ds-core.sh, 01.13).
// The 3DS GPU's time (Azahar's GPU timing emulation) as a per cent of Azahar's estimate (video_core/pica/pica_core.h's
// DelayGenerator, which takes the GPU as twice the 3DS's): 100 its own, 200 the 3DS's by it. Set as each game starts
// and as the settings change (core/emusuni_switches.h); read where a command list's end is signalled (on the GPU
// commands' thread, or on the emulation thread).
#pragma once
#ifdef EMUSUNI_PORT
#include <atomic>
#include <cstdint>

namespace EmuSuNi::GpuTime {
inline std::atomic<std::uint32_t> percent{100};
} // namespace EmuSuNi::GpuTime
#endif
''')
patch('src/video_core/pica/pica_core.h', r'''
#include "video_core/pica/dirty_regs.h"
''', r'''
#ifdef EMUSUNI_PORT
#include "common/emusuni_gpu_time.h"
#endif
#include "video_core/pica/dirty_regs.h"
''')
patch('src/video_core/pica/pica_core.h', r'''
        triangle_count = 0;
        command_count = 0;

        return static_cast<u64>(result);
''', r'''
        triangle_count = 0;
        command_count = 0;

#ifdef EMUSUNI_PORT
        // (01.13) The 3DS GPU's time per game: a per cent of this estimate (common/emusuni_gpu_time.h).
        result = result * static_cast<float>(EmuSuNi::GpuTime::percent.load(std::memory_order_relaxed)) / 100.0f;
#endif
        return static_cast<u64>(result);
''')
patch('src/core/emusuni_switches.h', r'''
#include "common/emusuni_async.h"
#include "common/logging/log.h"
''', r'''
#include <cstdio>
#include "common/emusuni_async.h"
#include "common/emusuni_gpu_time.h"
#include "common/logging/log.h"
''')
patch('src/core/emusuni_switches.h', r'''
// As a game starts: 01.11's speed switches as it found them (each a file in /data/EmuSuNi; the word off in it turns
''', r'''
// (01.13) The first number in /data/EmuSuNi/<name> (the variable's value on the PC), 0 without one.
inline u32 EmuSuNiFileNumber(const char* name, const char* env) {
    char text[64] = {};
#if defined(__ORBIS__)
    (void)env;
    char path[96];
    std::snprintf(path, sizeof(path), "/data/EmuSuNi/%s", name);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return 0;
    }
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
#else
    (void)name;
    const char* value = std::getenv(env);
    if (value == nullptr) {
        return 0;
    }
    std::snprintf(text, sizeof(text), "%s", value);
#endif
    for (const char* c = text; *c != '\0'; c++) {
        if (*c >= '0' && *c <= '9') {
            return static_cast<u32>(std::strtoul(c, nullptr, 10));
        }
    }
    return 0;
}
// (01.13) The 3DS GPU's time EmuSuNi gives a game, a per cent of Azahar's estimate (0: none of its own): the games that
// draw as often as they can and drew more pictures than a 3DS's 30 a second without it.
inline u32 EmuSuNiGameGpuTime(u64 title_id) {
    switch (title_id) {
    case 0x0004000000096600: // Castlevania: Lords of Shadow - Mirror of Fate (USA)
        return 100;
    case 0x0004000000126300: // Monster Hunter 4 Ultimate (USA)
    case 0x0004000000055F00: // Luigi's Mansion: Dark Moon (USA; the app's list turns the timing on at 100)
    case 0x0004000000076500: // (EUR)
    case 0x0004000000076400: // (JPN)
        return 200;
    default:
        return 0;
    }
}
// (01.13) A game's 3DS GPU time, as it starts and as the settings change (after core_settings.cpp set the timing from
// the app's list and 3ds-gpu-timings): with the file /data/EmuSuNi/3ds-gpu-timing-games without the word off in it
// (EMUSUNI_3DS_GPU_TIMING_GAMES set on the PC), a game of EmuSuNiGameGpuTime's gets the timing on at its per cent; any
// other the number in 3ds-gpu-timings (25 to 400) or 100. Returns the game's own per cent, 0 for none.
inline u32 EmuSuNiGpuTime(u64 title_id) {
    u32 percent = 100;
    const u32 all = EmuSuNiFileNumber("3ds-gpu-timings", "EMUSUNI_3DS_GPU_TIMINGS");
    if (all >= 25 && all <= 400) {
        percent = all;
    }
#if defined(__ORBIS__)
    const bool file = ::access("/data/EmuSuNi/3ds-gpu-timing-games", F_OK) == 0;
#else
    const bool file = std::getenv("EMUSUNI_3DS_GPU_TIMING_GAMES") != nullptr;
#endif
    const u32 own = file && EmuSuNi::Async::TestFile("3ds-gpu-timing-games", "EMUSUNI_3DS_GPU_TIMING_GAMES", 0) >= 0
                        ? EmuSuNiGameGpuTime(title_id)
                        : 0;
    if (own != 0) {
        Settings::values.simulate_3ds_gpu_timings = true;
        percent = own;
    }
    EmuSuNi::GpuTime::percent.store(percent, std::memory_order_relaxed);
    return own;
}
// As a game starts: 01.11's speed switches as it found them (each a file in /data/EmuSuNi; the word off in it turns
''')
patch('src/core/emusuni_switches.h', r'''
             "EmuSuNi: the speed switches for {:016X} (01.12; each off with the word off in its file in "
''', r'''
             "EmuSuNi: the speed switches for {:016X} (01.13; each off with the word off in its file in "
''')
patch('src/core/emusuni_switches.h', r'''
             "the game's idle loops skipped {} (3ds-cpu-idle-loops), the 3DS CPU at {} per cent ({}; "
             "3ds-cpu-clock-games), dynarmic's unsafe floating point {} (3ds-cpu-unsafe-fp, on with the file)",
''', r'''
             "the game's idle loops skipped {} (3ds-cpu-idle-loops)",
''')
patch('src/core/emusuni_switches.h', r'''
             on(EmuSuNi::Async::TestFile("3ds-cpu-idle-loops", "EMUSUNI_3DS_CPU_IDLE_LOOPS", 0) >= 0), clock,
             clock == app_clock ? "the app's 3DS CPU Clock" : "EmuSuNi's own for this game", on(unsafe_fp));
}
''', r'''
             on(EmuSuNi::Async::TestFile("3ds-cpu-idle-loops", "EMUSUNI_3DS_CPU_IDLE_LOOPS", 0) >= 0));
    // (01.13) A second line (the app's log cuts one at ~520 characters): the clock, the GPU's time (set here as the
    // game starts), the unsafe floating point.
    const u32 gpu_own = EmuSuNiGpuTime(title_id);
    const std::string gpu_time =
        !Settings::values.simulate_3ds_gpu_timings.GetValue()
            ? std::string("not emulated")
            : fmt::format("emulated at {} per cent of Azahar's estimate ({})",
                          EmuSuNi::GpuTime::percent.load(std::memory_order_relaxed),
                          gpu_own != 0 ? "EmuSuNi's own for this game" : "the app's list or 3ds-gpu-timings");
    LOG_INFO(Core,
             "EmuSuNi: the speed switches for {:016X} (2): the 3DS CPU at {} per cent ({}; 3ds-cpu-clock-games, on "
             "with the file), the 3DS GPU's time {} (3ds-gpu-timing-games, on with the file), dynarmic's unsafe "
             "floating point {} (3ds-cpu-unsafe-fp, on with the file)",
             title_id, clock, clock == app_clock ? "the app's 3DS CPU Clock" : "EmuSuNi's own for this game",
             gpu_time, on(unsafe_fp));
}
''')
patch('src/core/core.cpp', r'''
        CoreTiming().UpdateClockSpeed(EmuSuNiClock(title_id, Settings::values.cpu_clock_percentage.GetValue()));
#else
''', r'''
        CoreTiming().UpdateClockSpeed(EmuSuNiClock(title_id, Settings::values.cpu_clock_percentage.GetValue()));
        EmuSuNiGpuTime(title_id); // (01.13) after core_settings.cpp set the GPU timing again
#else
''')

# --- 01.13, also:
# - 3ds-hle-fast (EMUSUNI_3DS_HLE_FAST; on unless its file says off): svcGetThreadId on the current thread's
#   pseudo-handle is answered as the SVC is called (kernel/svc.cpp), with the same result and the same 677 cycles of the
#   3DS CPU, without the HLE's lock, the handle table's shared pointers or Azahar's two clock reads per SVC (its perf
#   stats, which nothing in the libretro core reads; skipped for every SVC). Monster Hunter 4 Ultimate makes ~550 a
#   frame, 93 per cent of its SVCs (the 3DS specialist's PC count); the current thread changes only on the emulation
#   thread, which this runs on.
# - 3ds-cpu-clock-games (01.11's, only with its file): Castlevania: Mirror of Fate at 75 per cent, not 50 (at 50 things
#   came and went on the owner's PS4; on the PC at 75 it walks with the pictures it draws at 100).
patch('src/core/emusuni_switches.h', r'''
namespace Core {
// The clock (per cent) EmuSuNi gives a game while the app's 3DS CPU Clock says 100: the games whose own code keeps the
''', r'''
namespace Core {
// (01.13) 3ds-hle-fast: the HLE's shortcuts (kernel/svc.cpp), set as each game starts (EmuSuNiLogSwitches).
inline bool emusuni_hle_fast = true;
// The clock (per cent) EmuSuNi gives a game while the app's 3DS CPU Clock says 100: the games whose own code keeps the
''')
patch('src/core/emusuni_switches.h', r'''
    case 0x0004000000096600: // Castlevania: Lords of Shadow - Mirror of Fate (USA)
        return 50;
''', r'''
    case 0x0004000000096600: // Castlevania: Lords of Shadow - Mirror of Fate (USA)
        return 75; // (01.13; 50 before: things came and went on the owner's PS4)
''')
patch('src/core/emusuni_switches.h', r'''
    const u32 gpu_own = EmuSuNiGpuTime(title_id);
''', r'''
    emusuni_hle_fast = EmuSuNi::Async::TestFile("3ds-hle-fast", "EMUSUNI_3DS_HLE_FAST", 0) >= 0;
    const u32 gpu_own = EmuSuNiGpuTime(title_id);
''')
patch('src/core/emusuni_switches.h', r'''
             "floating point {} (3ds-cpu-unsafe-fp, on with the file)",
             title_id, clock, clock == app_clock ? "the app's 3DS CPU Clock" : "EmuSuNi's own for this game",
             gpu_time, on(unsafe_fp));
''', r'''
             "floating point {} (3ds-cpu-unsafe-fp, on with the file), the HLE's shortcuts {} (3ds-hle-fast)",
             title_id, clock, clock == app_clock ? "the app's 3DS CPU Clock" : "EmuSuNi's own for this game",
             gpu_time, on(unsafe_fp), on(emusuni_hle_fast));
''')
patch('src/core/hle/kernel/svc.cpp', r'''
#include "core/hle/kernel/wait_object.h"
''', r'''
#include "core/hle/kernel/wait_object.h"
#ifdef EMUSUNI_PORT
#include "core/emusuni_switches.h"
#endif
''')
patch('src/core/hle/kernel/svc.cpp', r'''
#ifdef EMUSUNI_PORT
    const EmuSuNi::Stats::SvcTimer emusuni_timer;
#endif
    system.perf_stats->BeginSVCProcessing();
''', r'''
#ifdef EMUSUNI_PORT
    const EmuSuNi::Stats::SvcTimer emusuni_timer;
    // (01.13) 3ds-hle-fast: svcGetThreadId on the current thread's pseudo-handle (Monster Hunter 4 Ultimate: ~550 a
    // frame) answered here, as GetThreadId would (r0 the result, r1 the id; the same cycles), without the HLE's lock
    // and the handle table's shared pointers; and no perf stats' clock reads (nothing reads them in this core).
    if (Core::emusuni_hle_fast) {
        if (immediate == 0x37 && GetReg(1) == CurrentThread) {
            system.GetRunningCore().GetTimer().AddTicks(GetSVCInfo(immediate)->cycles);
            SetReg(0, ResultSuccess.raw);
            SetReg(1, kernel.GetCurrentThreadManager().GetCurrentThread()->GetThreadId());
            return;
        }
    } else {
        system.perf_stats->BeginSVCProcessing();
    }
#else
    system.perf_stats->BeginSVCProcessing();
#endif
''')
patch('src/core/hle/kernel/svc.cpp', r'''
            LOG_ERROR(Kernel_SVC, "unimplemented SVC function {:02X} {}(..)", info->id, info->name);
        }
    }
    system.perf_stats->EndSVCProcessing();
}
''', r'''
            LOG_ERROR(Kernel_SVC, "unimplemented SVC function {:02X} {}(..)", info->id, info->name);
        }
    }
#ifdef EMUSUNI_PORT
    if (!Core::emusuni_hle_fast) {
        system.perf_stats->EndSVCProcessing();
    }
#else
    system.perf_stats->EndSVCProcessing();
#endif
}
''')

# --- 01.14: 3ds-gpu-timing-games on by default (the word off in the file turns it off; EMUSUNI_3DS_GPU_TIMING_GAMES=off
# on the PC): the owner tried 01.13 without its file. On the PC (the 3DS specialist's), walking: Monster Hunter and Dark
# Moon draw 30 pictures a second at 200 per cent, ~40 at 150 and 20 at 300; Castlevania 30-40 at 100, 27 at 150. DKC,
# Super Mario 3D Land, Super Street Fighter IV and Captain Toad don't draw more than they show (not on the list).
patch('src/core/emusuni_switches.h', r'''
// the app's list and 3ds-gpu-timings): with the file /data/EmuSuNi/3ds-gpu-timing-games without the word off in it
// (EMUSUNI_3DS_GPU_TIMING_GAMES set on the PC), a game of EmuSuNiGameGpuTime's gets the timing on at its per cent; any
// other the number in 3ds-gpu-timings (25 to 400) or 100. Returns the game's own per cent, 0 for none.
''', r'''
// the app's list and 3ds-gpu-timings): unless the file /data/EmuSuNi/3ds-gpu-timing-games says off (01.14; 01.13 only
// with the file; EMUSUNI_3DS_GPU_TIMING_GAMES=off on the PC), a game of EmuSuNiGameGpuTime's gets the timing on at its
// per cent; any other the number in 3ds-gpu-timings (25 to 400) or 100. Returns the game's own per cent, 0 for none.
''')
patch('src/core/emusuni_switches.h', r'''
#if defined(__ORBIS__)
    const bool file = ::access("/data/EmuSuNi/3ds-gpu-timing-games", F_OK) == 0;
#else
    const bool file = std::getenv("EMUSUNI_3DS_GPU_TIMING_GAMES") != nullptr;
#endif
    const u32 own = file && EmuSuNi::Async::TestFile("3ds-gpu-timing-games", "EMUSUNI_3DS_GPU_TIMING_GAMES", 0) >= 0
                        ? EmuSuNiGameGpuTime(title_id)
                        : 0;
''', r'''
    // (01.14) On unless its file says off (01.13: only with the file).
    const u32 own = EmuSuNi::Async::TestFile("3ds-gpu-timing-games", "EMUSUNI_3DS_GPU_TIMING_GAMES", 0) >= 0
                        ? EmuSuNiGameGpuTime(title_id)
                        : 0;
''')
patch('src/core/emusuni_switches.h', r'''
             "with the file), the 3DS GPU's time {} (3ds-gpu-timing-games, on with the file), dynarmic's unsafe "
''', r'''
             "with the file), the 3DS GPU's time {} (3ds-gpu-timing-games, off with the word off), dynarmic's unsafe "
''')
patch('src/core/emusuni_switches.h', r'''
             "EmuSuNi: the speed switches for {:016X} (01.13; each off with the word off in its file in "
''', r'''
             "EmuSuNi: the speed switches for {:016X} (01.14; each off with the word off in its file in "
''')

# --- 01.15: a game's file that says it's decrypted (the NCCH's NoCrypto flag) but isn't (report SWCWEV: Kirby: Planet
# Robobot, Majora's Mask 3D, Yoshi's New Island and New Super Mario Bros. 2, all "-decrypted.3ds"): its extended header
# and ExeFS are noise (decrypted twice, or with the wrong keys), so Azahar found no .code and said only "Failed to load
# ROM" (ResultStatus::Error), after a memory mode read from the noise ("mode 9/14 doesn't fit an Old 3DS"). Now an
# executable whose ExeFS has no .code (and no override gives one), or whose .code doesn't decompress, is ErrorEncrypted
# with a log line saying why, before any of the noise is used: the app shows its "This game is encrypted / Only
# decrypted dumps run" (its words for Azahar's "...decrypted..."). A .code whose size, read from its last bytes, is
# beyond 128 MiB isn't allocated (a throw from operator new would end the app on the PS4). On the PC: a copy of the
# owner's Kirby (Old 3DS mode 0, New 3DS mode 1: no New 3DS needed) with those two parts scrambled.
patch('src/core/file_sys/ncch_container.cpp', r'''
    LoadOverrides();

    // We need at least one of these or overrides, practically
''', r'''
    LoadOverrides();

#ifdef EMUSUNI_PORT
    // EmuSuNi (01.15): an executable whose ExeFS has no .code, and no override gives one: a file that says it's
    // decrypted but isn't (decrypted twice, or with the wrong keys); its extended header is noise too.
    if (has_exheader && has_exefs && !is_tainted && !is_proto && ncch_header.is_executable != 0) {
        bool emusuni_code = false;
        for (const auto& section : exefs_header.section) {
            emusuni_code = emusuni_code || std::strncmp(section.name, ".code", sizeof(section.name)) == 0;
        }
        std::vector<u8> emusuni_override;
        if (!emusuni_code &&
            LoadOverrideExeFSSection(".code", emusuni_override) != Loader::ResultStatus::Success) {
            LOG_ERROR(Service_FS,
                      "EmuSuNi: {:016X}: the file says it is decrypted, but its ExeFS has no .code (its extended "
                      "header: jump ID {:016X}, program ID {:016X}, memory mode {}): still encrypted, or decrypted "
                      "with the wrong keys; decrypt the game again from its dump",
                      ncch_header.program_id, exheader_header.system_info.jump_id,
                      exheader_header.arm11_system_local_caps.program_id,
                      static_cast<int>(exheader_header.arm11_system_local_caps.system_mode));
            return Loader::ResultStatus::ErrorEncrypted;
        }
    }
#endif

    // We need at least one of these or overrides, practically
''')
patch('src/core/file_sys/ncch_container.cpp', r'''
                // Decompress .code section...
                buffer.resize(LZSS_GetDecompressedSize(temp_buffer));
                if (!LZSS_Decompress(temp_buffer, buffer)) {
                    return Loader::ResultStatus::ErrorInvalidFormat;
                }
''', r'''
                // Decompress .code section...
#ifdef EMUSUNI_PORT
                // EmuSuNi (01.15): a .code that doesn't decompress is noise (still encrypted, or decrypted with the
                // wrong keys); its size, read from its last bytes, can be gigabytes: not allocated.
                const std::size_t emusuni_size = LZSS_GetDecompressedSize(temp_buffer);
                if (emusuni_size < temp_buffer.size() || emusuni_size > std::size_t{128} * 1024 * 1024) {
                    LOG_ERROR(Service_FS, "EmuSuNi: the .code would decompress to {} bytes from {}: still encrypted, "
                                          "or decrypted with the wrong keys",
                              emusuni_size, temp_buffer.size());
                    return Loader::ResultStatus::ErrorEncrypted;
                }
                buffer.resize(emusuni_size);
                if (!LZSS_Decompress(temp_buffer, buffer)) {
                    LOG_ERROR(Service_FS, "EmuSuNi: the .code doesn't decompress: still encrypted, or decrypted "
                                          "with the wrong keys");
                    return Loader::ResultStatus::ErrorEncrypted;
                }
#else
                buffer.resize(LZSS_GetDecompressedSize(temp_buffer));
                if (!LZSS_Decompress(temp_buffer, buffer)) {
                    return Loader::ResultStatus::ErrorInvalidFormat;
                }
#endif
''')
patch('src/core/loader/ncch.cpp', r'''
    std::vector<u8> code;
    u64_le program_id;
    if (ResultStatus::Success == ReadCode(code) &&
        ResultStatus::Success == ReadProgramId(program_id)) {
''', r'''
    std::vector<u8> code;
    u64_le program_id;
#ifdef EMUSUNI_PORT
    // EmuSuNi (01.15): a .code that is noise (file_sys/ncch_container.cpp) says so, not "Failed to load ROM".
    const ResultStatus emusuni_code = ReadCode(code);
    if (emusuni_code == ResultStatus::ErrorEncrypted) {
        return emusuni_code;
    }
    if (ResultStatus::Success == emusuni_code &&
#else
    if (ResultStatus::Success == ReadCode(code) &&
#endif
        ResultStatus::Success == ReadProgramId(program_id)) {
''')

# --- 01.15: Castlevania: Mirror of Fate's GPU commands always on the emulation thread (3ds-pica-inline-games; the word
# off in its file: as any game), and its EUR and JPN games at 100 per cent of Azahar's GPU estimate as the USA one. The
# owner's 01.14 (100, the commands on their thread): its intro didn't show, its lightning failed, it "skipped frames".
# In the 3DS specialist's PC tests (3ds-acelerar-nucleo2.md): when a command list's end comes later than the estimate
# gives, its intro drops the creature and the sky (only the lightning on black) or stops - already at 112 per cent. On
# the PS4 the GPU commands' thread is ~7 times slower than the PC's (0.2 ms a command), and a list's end reaches the
# game when that thread finished plus the estimate; on the emulation thread it comes at the list's time plus the
# estimate, as on the PC (its intro whole). The cost: the commands' time on the emulation thread (PC, walking: 3.1 ms a
# frame against 2.4; the PS4's 11.9 by the same ratio ~16). Its skipped frames (pictures of 1 and 2 frames in turn)
# stay: at 50 per cent it walks at 60 pictures a second, smooth, but at 5.7 ms a frame (~28 on the PS4).
# Monster Hunter 4 Ultimate and Luigi's Mansion: Dark Moon as before (200, their thread).
patch('src/common/emusuni_gpu_time.h', r'''
inline std::atomic<std::uint32_t> percent{100};
} // namespace EmuSuNi::GpuTime
''', r'''
inline std::atomic<std::uint32_t> percent{100};
// (01.15) The game's GPU commands always on the emulation thread, never on 3ds-async-pica's
// (video_core/emusuni_async_pica.h): a game of EmuSuNi's list whose list ends must come at their time (Castlevania;
// core/emusuni_switches.h). Set as each game starts.
inline std::atomic<bool> pica_inline{false};
} // namespace EmuSuNi::GpuTime
''')
patch('src/video_core/emusuni_async_pica.h', r'''
#include "common/emusuni_async.h"
''', r'''
#include "common/emusuni_async.h"
#include "common/emusuni_gpu_time.h"
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    bool Inline() const {
        return inline_mode;
    }
    void FrameEnd() {
        if (inline_mode) {
''', r'''
    bool Inline() const {
        // (01.15) Or always, for a game whose list ends must come at their time (common/emusuni_gpu_time.h).
        return inline_mode || EmuSuNi::GpuTime::pica_inline.load(std::memory_order_relaxed);
    }
    void FrameEnd() {
        if (EmuSuNi::GpuTime::pica_inline.load(std::memory_order_relaxed)) {
            Stats::counts[Stats::PicaInlineFrames]++; // (01.15: inline for this game, nothing to measure)
            return;
        }
        if (inline_mode) {
''')
patch('src/core/emusuni_switches.h', r'''
    case 0x0004000000096600: // Castlevania: Lords of Shadow - Mirror of Fate (USA)
        return 100;
    case 0x0004000000126300: // Monster Hunter 4 Ultimate (USA)
''', r'''
    case 0x0004000000096600: // Castlevania: Lords of Shadow - Mirror of Fate (USA)
    case 0x000400000009E500: // (EUR; 01.15)
    case 0x00040000000BAC00: // (JPN; 01.15)
        return 100;
    case 0x0004000000126300: // Monster Hunter 4 Ultimate (USA)
''')
patch('src/core/emusuni_switches.h', r'''
    EmuSuNi::GpuTime::percent.store(percent, std::memory_order_relaxed);
    return own;
}
''', r'''
    EmuSuNi::GpuTime::percent.store(percent, std::memory_order_relaxed);
    // (01.15) Castlevania's GPU commands on the emulation thread (common/emusuni_gpu_time.h), unless the file
    // /data/EmuSuNi/3ds-pica-inline-games says off (EMUSUNI_3DS_PICA_INLINE_GAMES=off on the PC).
    const bool pica_inline =
        (title_id == 0x0004000000096600 || title_id == 0x000400000009E500 || title_id == 0x00040000000BAC00) &&
        EmuSuNi::Async::TestFile("3ds-pica-inline-games", "EMUSUNI_3DS_PICA_INLINE_GAMES", 0) >= 0;
    if (pica_inline != EmuSuNi::GpuTime::pica_inline.exchange(pica_inline, std::memory_order_relaxed) && pica_inline) {
        LOG_INFO(Core, "EmuSuNi: {:016X}: its GPU commands on the emulation thread, not 3ds-async-pica's "
                       "(3ds-pica-inline-games; off with the word off)",
                 title_id);
    }
    return own;
}
''')
patch('src/core/emusuni_switches.h', r'''
             "EmuSuNi: the speed switches for {:016X} (01.14; each off with the word off in its file in "
''', r'''
             "EmuSuNi: the speed switches for {:016X} (01.16; each off with the word off in its file in "
''')

# --- 01.16: a command list's end at its time with the GPU commands on their thread (3ds-pica-on-time; the word off in
# its file: as before; EMUSUNI_3DS_PICA_ON_TIME=off on the PC). With Azahar's GPU timing a list's end (and a fill's, a
# transfer's) is signalled its estimate after the list is sent, as when the commands ran on the emulation thread; on
# their thread (3ds-async-pica) it was signalled its estimate after the emulation thread took the thread's result, so
# that thread's lag, in the emulated time, came on top. On the PS4 that thread is ~7 times slower than the PC's (0.2 ms
# a command), and Castlevania: Mirror of Fate's intro drops the creature and the sky, or stops, when its lists end
# later than the estimate gives (already at 112 per cent, the 3DS specialist's PC tests): 01.15 put its commands on the
# emulation thread (3ds-pica-inline-games), which cost ~4-5 ms a frame on the PS4 (15.9 ms a frame, 83 per cent). Now
# each command carries the emulated time it was sent; its interrupt comes at that time plus its delay (at once when the
# thread finished later still). 3ds-pica-inline-games is now only with its file. Every 3000 lists a line says how many
# ended later than their estimate and by how much.
patch('src/common/emusuni_gpu_time.h', r'''
inline std::atomic<bool> pica_inline{false};
''', r'''
inline std::atomic<bool> pica_inline{false};
// (01.16) A command list's end at its time with the commands on their thread (3ds-pica-on-time; video_core/gpu.cpp).
inline std::atomic<bool> on_time{true};
''')
patch('src/common/emusuni_async.h', r'''
    struct Interrupt {
        std::uint32_t id;
        std::uint64_t delay_ns;
    };
''', r'''
    struct Interrupt {
        std::uint32_t id;
        std::uint64_t delay_ns;
        std::uint64_t submit_ns = 0; // (01.16) the emulated time its command was sent (0: not known)
    };
''')
patch('src/common/emusuni_async.h', r'''
inline thread_local Deferred* deferred = nullptr;
''', r'''
inline thread_local Deferred* deferred = nullptr;
// (01.16) The command the GPU commands' thread runs: the emulated time it was sent (DeferInterrupt's).
inline thread_local std::uint64_t pica_submit_ns = 0;
''')
patch('src/common/emusuni_async.h', r'''
    d->interrupts.push_back({id, delay_ns});
''', r'''
    d->interrupts.push_back({id, delay_ns, pica_submit_ns});
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    using Interrupt = std::function<void(std::uint32_t id, std::uint64_t delay_ns)>;
''', r'''
    using Interrupt = std::function<void(std::uint32_t id, std::uint64_t delay_ns, std::uint64_t submit_ns)>;
''')
patch('src/video_core/emusuni_async_pica.h', r'''
        std::uint32_t invalidate_start = 0, invalidate_size = 0; // (01.12) size 0: a command
    };
''', r'''
        std::uint32_t invalidate_start = 0, invalidate_size = 0; // (01.12) size 0: a command
        std::uint64_t submit_ns = 0; // (01.16) the emulated time the command was sent
    };
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    void Submit(const Service::GSP::Command& command) {
        Item item;
        item.command = command;
        Push(item);
    }
''', r'''
    void Submit(const Service::GSP::Command& command, std::uint64_t submit_ns = 0) {
        Item item;
        item.command = command;
        item.submit_ns = submit_ns;
        Push(item);
    }
''')
patch('src/video_core/emusuni_async_pica.h', r'''
            } else {
                execute(item.command);
            }
''', r'''
            } else {
                pica_submit_ns = item.submit_ns; // (01.16: its interrupts' time, DeferInterrupt)
                execute(item.command);
            }
''')
patch('src/video_core/emusuni_async_pica.h', r'''
            interrupt(irq.id, irq.delay_ns);
''', r'''
            interrupt(irq.id, irq.delay_ns, irq.submit_ns);
''')
patch('src/video_core/gpu.cpp', r'''
            [this](u32 id, u64 delay_ns) {
                impl->signal_interrupt(static_cast<Service::GSP::InterruptId>(id), delay_ns);
            },
''', r'''
            [this](u32 id, u64 delay_ns, u64 submit_ns) {
                // (01.16) At its time (3ds-pica-on-time): the delay counted from the command's sending, not from
                // now - the thread's lag in the emulated time not added (at once when it finished later still).
                if (submit_ns != 0 && EmuSuNi::GpuTime::on_time.load(std::memory_order_relaxed)) {
                    const u64 now_ns = static_cast<u64>(impl->timing.GetGlobalTimeUs().count()) * 1000;
                    const u64 lag_ns = now_ns > submit_ns ? now_ns - submit_ns : 0;
                    static u64 lists = 0, late = 0, late_ns = 0, lag_sum_ns = 0;
                    if (static_cast<Service::GSP::InterruptId>(id) == Service::GSP::InterruptId::P3D) {
                        lists++;
                        lag_sum_ns += lag_ns;
                        if (lag_ns > delay_ns) {
                            late++;
                            late_ns += lag_ns - delay_ns;
                        }
                        if (lists == 3000) {
                            LOG_INFO(HW_GPU,
                                     "EmuSuNi: a list's end at its time (3ds-pica-on-time): of the last "
                                     "3000, {} ended later than their estimate, by {} us on average; the "
                                     "thread's lag {} us on average",
                                     late, late != 0 ? late_ns / late / 1000 : 0, lag_sum_ns / lists / 1000);
                            lists = late = late_ns = lag_sum_ns = 0;
                        }
                    }
                    delay_ns = delay_ns > lag_ns ? delay_ns - lag_ns : 0;
                }
                impl->signal_interrupt(static_cast<Service::GSP::InterruptId>(id), delay_ns);
            },
''')
patch('src/video_core/gpu.cpp', r'''
            emusuni_pica->Submit(command);
''', r'''
            emusuni_pica->Submit(command, static_cast<u64>(impl->timing.GetGlobalTimeUs().count()) * 1000);
''')
patch('src/core/emusuni_switches.h', r'''
    // (01.15) Castlevania's GPU commands on the emulation thread (common/emusuni_gpu_time.h), unless the file
    // /data/EmuSuNi/3ds-pica-inline-games says off (EMUSUNI_3DS_PICA_INLINE_GAMES=off on the PC).
    const bool pica_inline =
        (title_id == 0x0004000000096600 || title_id == 0x000400000009E500 || title_id == 0x00040000000BAC00) &&
        EmuSuNi::Async::TestFile("3ds-pica-inline-games", "EMUSUNI_3DS_PICA_INLINE_GAMES", 0) >= 0;
''', r'''
    // (01.15) Castlevania's GPU commands on the emulation thread (common/emusuni_gpu_time.h); (01.16) only with the
    // file /data/EmuSuNi/3ds-pica-inline-games without the word off in it (EMUSUNI_3DS_PICA_INLINE_GAMES on the PC):
    // a list's end comes at its time on their thread now (3ds-pica-on-time, below).
#if defined(__ORBIS__)
    const bool inline_file = ::access("/data/EmuSuNi/3ds-pica-inline-games", F_OK) == 0;
#else
    const bool inline_file = std::getenv("EMUSUNI_3DS_PICA_INLINE_GAMES") != nullptr;
#endif
    const bool pica_inline =
        inline_file &&
        (title_id == 0x0004000000096600 || title_id == 0x000400000009E500 || title_id == 0x00040000000BAC00) &&
        EmuSuNi::Async::TestFile("3ds-pica-inline-games", "EMUSUNI_3DS_PICA_INLINE_GAMES", 0) >= 0;
    // (01.16) A command list's end at its time with the commands on their thread, unless the file
    // /data/EmuSuNi/3ds-pica-on-time says off (EMUSUNI_3DS_PICA_ON_TIME=off on the PC).
    const bool on_time = EmuSuNi::Async::TestFile("3ds-pica-on-time", "EMUSUNI_3DS_PICA_ON_TIME", 0) >= 0;
    if (on_time != EmuSuNi::GpuTime::on_time.exchange(on_time, std::memory_order_relaxed)) {
        LOG_INFO(Core, "EmuSuNi: a command list's end at its time (3ds-pica-on-time): {}", on_time ? "on" : "off");
    }
''')

# --- 01.16, also: Castlevania: Mirror of Fate's core 0 with 56 MiB of code cache, not 40, and the JIT pool 80 MiB, not
# 64 (3ds-jit-big-games; the word off in its file: as any game; EMUSUNI_3DS_JIT_BIG_GAMES=off on the PC), the owner's
# OK: only for that game, the pool given back whole as the game ends (JitRelease, as before), ~100 MiB of flexible
# memory left during the game (01.15: 154 MiB free with the 64 MiB pool). Its warm-up list is ~102,000 blocks and the
# warm-up leaves half the code cache to the game: with 40 MiB it compiled 35,302 ahead and the game met 56,088 more,
# frames of 100 to 400 ms compiling 2,000 to 4,800 blocks. With 56 MiB the warm-up has 28 MiB, not 20. The game's
# program ID comes from the ROM's headers before the JITs are made (citra_libretro.cpp's early loader).
patch('src/common/emusuni_jit.h', r'''
#include <cstddef>
''', r'''
#include <cstddef>
#include <cstdint>
''')
patch('src/common/emusuni_jit.h', r'''
void JitRelease();
} // namespace EmuSuNi
''', r'''
void JitRelease();
// (01.16) The game about to start (its program ID, from the ROM's headers, before the JITs are made): Castlevania:
// Mirror of Fate's core 0 gets 16 MiB more of code cache, and the pool as much more (3ds-jit-big-games).
void JitSetTitle(std::uint64_t title_id);
// The bytes core 0's code cache (and the pool) get on top for this game: 0 for most.
std::size_t JitCoreExtra();
} // namespace EmuSuNi
''')
patch('src/common/emusuni_jit.cpp', r'''
#include "common/emusuni_jit.h"
''', r'''
#include "common/emusuni_async.h"
#include "common/emusuni_jit.h"
''')
patch('src/common/emusuni_jit.cpp', r'''
constexpr size_t POOL_SIZE = 64u << 20;
''', r'''
constexpr size_t POOL_BASE = 64u << 20;
// (01.16) The pool's size as it was mapped: POOL_BASE, and JitCoreExtra() more for a game that gets it.
size_t POOL_SIZE = POOL_BASE;
size_t g_extra = 0; // (01.16) JitCoreExtra(): set by JitSetTitle before the game's JITs are made
''')
patch('src/common/emusuni_jit.cpp', r'''
        g_pool = static_cast<u8*>(emusuni_map_executable(POOL_SIZE, "3ds jit"));
''', r'''
        POOL_SIZE = POOL_BASE + g_extra; // (01.16)
        g_pool = static_cast<u8*>(emusuni_map_executable(POOL_SIZE, "3ds jit"));
''')
patch('src/common/emusuni_jit.cpp', r'''
void JitRelease() {
''', r'''
void JitSetTitle(std::uint64_t title_id) {
    const bool big =
        (title_id == 0x0004000000096600 || title_id == 0x000400000009E500 || title_id == 0x00040000000BAC00) &&
        EmuSuNi::Async::TestFile("3ds-jit-big-games", "EMUSUNI_3DS_JIT_BIG_GAMES", 0) >= 0;
    std::scoped_lock lock{g_lock};
    g_extra = big ? 16u << 20 : 0;
    if (big) {
        LOG_INFO(Common, "EmuSuNi: {:016X}: core 0's JIT 16 MiB bigger, and its memory (3ds-jit-big-games; off "
                         "with the word off)",
                 title_id);
    }
}

std::size_t JitCoreExtra() {
    std::scoped_lock lock{g_lock};
    return g_extra;
}

void JitRelease() {
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    config.code_cache_size =
        GetID() == 0 ? (Settings::values.is_new_3ds.GetValue() ? 24u << 20 : 40u << 20) : 8u << 20;
''', r'''
    config.code_cache_size =
        GetID() == 0 ? (Settings::values.is_new_3ds.GetValue() ? 24u << 20 : 40u << 20) : 8u << 20;
    if (GetID() == 0) {
        config.code_cache_size += EmuSuNi::JitCoreExtra(); // (01.16) 3ds-jit-big-games: Castlevania, 16 MiB more
    }
''')
patch('src/citra_libretro/citra_libretro.cpp', r'''
        // Stash the loader so System::Load can reuse it instead of re-opening
        Core::System::GetInstance().RegisterAppLoaderEarly(loader);
''', r'''
#ifdef EMUSUNI_PORT
        // (01.16) The game's program ID before its JITs are made: its code cache (common/emusuni_jit.h).
        u64 emusuni_title = 0;
        loader->ReadProgramId(emusuni_title);
        EmuSuNi::JitSetTitle(emusuni_title);
#endif
        // Stash the loader so System::Load can reuse it instead of re-opening
        Core::System::GetInstance().RegisterAppLoaderEarly(loader);
''')

# --- 01.16, also: the picture black at a game's start (3ds-black-watch; the word off in its file: none of it;
# EMUSUNI_3DS_BLACK_WATCH=off on the PC). 01.15 on the PS4: every 3DS game's picture black, its sound on, until one
# to three resets (nine resets in four games; none in 01.14's starts), and nothing in the log said why. Now the
# renderer counts, for each screen since it started, the pictures from the game's own drawing, those the LCD's
# color fill made (SetLcdForceBlack, the fill registers) and those whose surface it didn't find (AccelerateDisplay
# failed: the screen's own texture, black). Every 60 pictures, until one with something in it, 24 points of the
# picture it hands over (4x4 on the top screen, 4x2 on the bottom) are read back from the GPU. Logged: the first
# picture that isn't black (at every start: when), and while it has been black since the start with the game
# drawing, at 4 s and then 8, 16, 32...: those counts, the points and their alpha; the counts again if the renderer
# closes still black. emusuni_3ds_black_ms() tells the app how long (0 once a picture was seen), for a reset by
# itself as the owner made by hand (the app's part: Frontend's).
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
namespace LibRetro { // citra_libretro/libretro_vk.cpp: the frontend's queue lock
void LockQueue();
void UnlockQueue();
} // namespace LibRetro
#endif
''', r'''
namespace LibRetro { // citra_libretro/libretro_vk.cpp: the frontend's queue lock
void LockQueue();
void UnlockQueue();
} // namespace LibRetro
#endif
#ifdef EMUSUNI_PORT
#include <atomic>
#include <cstdlib>
#include "common/emusuni_async.h"
#include "common/emusuni_stats.h"
// (01.16) 3ds-black-watch: the picture black at a game's start (the renderer's constructor, PrepareRendertarget,
// RenderToWindow and its end).
namespace EmuSuNiBlack {
constexpr u32 POINTS = 24; // read back: 4x4 on the top screen, 4x2 on the bottom
constexpr u64 EVERY = 60;  // pictures from one reading to the next
bool on = false;
bool test_black = false; // (the PC: EMUSUNI_3DS_TEST_BLACK) every reading taken as black
bool test_log = false;   // (the PC: EMUSUNI_3DS_TEST_BLACK_LOG) every reading said
std::chrono::steady_clock::time_point start, last;
u64 pictures = 0;
u64 readings = 0;
// Each screen's pictures (0 the top, 1 the bottom): from the game's drawing, the LCD's color fill, no surface found.
u64 drawn[2], filled[2], missing[2];
u32 fill_color[2], missing_at[2], missing_format[2], missing_w[2], missing_h[2];
VkBuffer buffer = VK_NULL_HANDLE;
VmaAllocation allocation = nullptr;
const u8* mapped = nullptr;
bool pending = false;
u64 pending_tick = 0;
u32 pending_points = 0;
u64 draws_at = 0;
bool seen = false;    // a picture with something in it since the start
double black_s = 0.0; // the seconds of black pictures with the game drawing, until one is seen
double next_log = 4.0;
std::atomic<u64> black_ms{0};

std::string Screen(int s) {
    std::string text = fmt::format("{} drawn by the game, {} the LCD's color fill", drawn[s], filled[s]);
    if (filled[s] != 0) {
        text += fmt::format(" (#{:06X})", fill_color[s]);
    }
    text += fmt::format(", {} with no surface found", missing[s]);
    if (missing[s] != 0) {
        text += fmt::format(" (at {:08X}, format {}, {}x{})", missing_at[s], missing_format[s], missing_w[s],
                            missing_h[s]);
    }
    return text;
}

// A line of its own (the app's log cuts its lines at ~520 characters).
void LogScreens(const char* until) {
    LOG_INFO(Render_Vulkan, "EmuSuNi: 3ds-black-watch: its pictures {}: the top screen {}; the bottom {}", until,
             Screen(0), Screen(1));
}
} // namespace EmuSuNiBlack
// (01.16) For the app: how long the picture has been black since the renderer started, the game drawing, in ms; 0
// once a picture with something in it was seen, and with 3ds-black-watch off.
extern "C" std::uint64_t emusuni_3ds_black_ms(void) {
    return EmuSuNiBlack::black_ms.load(std::memory_order_relaxed);
}
#endif
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
    CompileShaders();
    BuildLayouts();
    BuildPipelines();
''', r'''
    CompileShaders();
    BuildLayouts();
    BuildPipelines();
#ifdef EMUSUNI_PORT
    { // (01.16) 3ds-black-watch: its counts from this start, and the buffer of its readings
        namespace B = EmuSuNiBlack;
        B::on = EmuSuNi::Async::TestFile("3ds-black-watch", "EMUSUNI_3DS_BLACK_WATCH", 0) >= 0;
#ifndef __ORBIS__
        B::test_black = std::getenv("EMUSUNI_3DS_TEST_BLACK") != nullptr;
        B::test_log = std::getenv("EMUSUNI_3DS_TEST_BLACK_LOG") != nullptr;
#endif
        B::start = B::last = std::chrono::steady_clock::now();
        B::pictures = B::readings = 0;
        for (int s = 0; s < 2; s++) {
            B::drawn[s] = B::filled[s] = B::missing[s] = 0;
        }
        B::pending = false;
        B::seen = false;
        B::black_s = 0.0;
        B::next_log = 4.0;
        B::black_ms.store(0, std::memory_order_relaxed);
        B::draws_at = EmuSuNi::Stats::counts[EmuSuNi::Stats::Draws];
        if (B::on) {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = B::POINTS * 4;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo alloc{};
            alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            VmaAllocationInfo got{};
            B::buffer = VK_NULL_HANDLE;
            B::allocation = nullptr;
            if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &B::buffer, &B::allocation, &got) !=
                    VK_SUCCESS ||
                got.pMappedData == nullptr) {
                if (B::buffer != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(instance.GetAllocator(), B::buffer, B::allocation);
                }
                B::buffer = VK_NULL_HANDLE;
                B::allocation = nullptr;
                B::on = false;
                LOG_WARNING(Render_Vulkan, "EmuSuNi: 3ds-black-watch off: no buffer for its readings");
            } else {
                B::mapped = static_cast<const u8*>(got.pMappedData);
            }
        }
    }
#endif
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
#ifdef EMUSUNI_PORT
    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: the GPU idle ({} ms)", emusuni_ms());
#endif
''', r'''
#ifdef EMUSUNI_PORT
    LOG_INFO(Render_Vulkan, "EmuSuNi: the renderer closes: the GPU idle ({} ms)", emusuni_ms());
    if (EmuSuNiBlack::on && !EmuSuNiBlack::seen && EmuSuNiBlack::readings != 0) {
        // (01.16) 3ds-black-watch: closed (a reset, a state's load, the game's end) with its picture black throughout
        LOG_WARNING(Render_Vulkan,
                    "EmuSuNi: the renderer closes with its picture black since it started, {:.0f} s of it with the "
                    "game drawing, {} pictures (3ds-black-watch)",
                    EmuSuNiBlack::black_s, EmuSuNiBlack::pictures);
        EmuSuNiBlack::LogScreens("since it started");
    }
    if (EmuSuNiBlack::buffer != VK_NULL_HANDLE) { // (01.16) 3ds-black-watch's readings
        vmaDestroyBuffer(instance.GetAllocator(), EmuSuNiBlack::buffer, EmuSuNiBlack::allocation);
        EmuSuNiBlack::buffer = VK_NULL_HANDLE;
        EmuSuNiBlack::allocation = nullptr;
        EmuSuNiBlack::mapped = nullptr;
    }
    EmuSuNiBlack::on = false;
    EmuSuNiBlack::black_ms.store(0, std::memory_order_relaxed);
#endif
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            FillScreen(color_fill.AsVector(), texture);
            continue;
        }
''', r'''
        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            FillScreen(color_fill.AsVector(), texture);
#ifdef EMUSUNI_PORT
            if (i != 1 && EmuSuNiBlack::on) { // (01.16) 3ds-black-watch: the LCD's color fill
                const int s = i == 0 ? 0 : 1;
                const Common::Vec3<u8> c = color_fill.AsVector();
                EmuSuNiBlack::filled[s]++;
                EmuSuNiBlack::fill_color[s] = (u32{c.x} << 16) | (u32{c.y} << 8) | u32{c.z};
            }
#endif
            continue;
        }
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
    }
}
''', r'''
        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
#ifdef EMUSUNI_PORT
        if (i != 1 && EmuSuNiBlack::on) { // (01.16) 3ds-black-watch: the screen's own texture: no surface found
            const int s = i == 0 ? 0 : 1;
            if (screen_infos[i].image_view == texture.image_view) {
                EmuSuNiBlack::missing[s]++;
                EmuSuNiBlack::missing_at[s] =
                    framebuffer.active_fb == 0 ? framebuffer.address_left1 : framebuffer.address_left2;
                EmuSuNiBlack::missing_format[s] = static_cast<u32>(framebuffer.color_format.Value());
                EmuSuNiBlack::missing_w[s] = framebuffer.width.Value();
                EmuSuNiBlack::missing_h[s] = framebuffer.height.Value();
            } else {
                EmuSuNiBlack::drawn[s]++;
            }
        }
#endif
    }
}
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped) {
''', r'''
#ifdef EMUSUNI_PORT
// (01.16) 3ds-black-watch: the reading before looked at once the GPU is done with it; every EVERY pictures, until
// one with something in it, POINTS points of the picture handed over read back (the image as the app gets it:
// shader-read after the present's render pass, and so again after the copy).
static void EmuSuNiBlackWatch(const Instance& instance, Scheduler& scheduler, Frame* frame,
                              const Layout::FramebufferLayout& layout) {
    namespace B = EmuSuNiBlack;
    B::pictures++;
    if (B::pending && scheduler.IsFree(B::pending_tick)) {
        B::pending = false;
        B::readings++;
        vmaInvalidateAllocation(instance.GetAllocator(), B::allocation, 0, VK_WHOLE_SIZE);
        u32 black = 0, clear = 0;
        for (u32 p = 0; p < B::pending_points; p++) {
            const u8* px = B::mapped + p * 4;
            black += px[0] <= 2 && px[1] <= 2 && px[2] <= 2 ? 1 : 0;
            clear += px[3] == 0 ? 1 : 0;
        }
        const bool is_black = B::test_black || black == B::pending_points;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - B::last).count();
        const double since = std::chrono::duration<double>(now - B::start).count();
        B::last = now;
        const u64 draws = EmuSuNi::Stats::counts[EmuSuNi::Stats::Draws];
        const u64 new_draws = draws - B::draws_at;
        B::draws_at = draws;
        if (B::test_log) {
            // Each point: '.' black, 'a' alpha 0 (not black), 'A' black and alpha 0, 'x' neither.
            std::string points;
            for (u32 p = 0; p < B::pending_points; p++) {
                const u8* px = B::mapped + p * 4;
                const bool dark = px[0] <= 2 && px[1] <= 2 && px[2] <= 2;
                points += dark ? (px[3] == 0 ? 'A' : '.') : (px[3] == 0 ? 'a' : 'x');
                points += p == 15 ? "|" : "";
            }
            LOG_INFO(Render_Vulkan,
                     "EmuSuNi: 3ds-black-watch: picture {} ({:.1f} s): {} of {} points black, {} with alpha 0 "
                     "({}), the first #{:02X}{:02X}{:02X}{:02X}; {} draws",
                     B::pictures, since, black, B::pending_points, clear, points, B::mapped[0], B::mapped[1],
                     B::mapped[2], B::mapped[3], new_draws);
        }
        if (!is_black) {
            B::seen = true;
            B::black_ms.store(0, std::memory_order_relaxed);
            LOG_INFO(Render_Vulkan,
                     "EmuSuNi: the picture's first points not black: {:.1f} s after its renderer started, picture "
                     "{} (3ds-black-watch: {} points read back every {} pictures; {} of them black, {} with alpha "
                     "0)",
                     since, B::pictures, B::pending_points, B::EVERY, black, clear);
            B::LogScreens("until then");
            return;
        }
        if (new_draws != 0) {
            B::black_s += dt;
            B::black_ms.store(static_cast<u64>(B::black_s * 1000.0), std::memory_order_relaxed);
            if (B::black_s >= B::next_log) {
                B::next_log *= 2.0;
                LOG_WARNING(Render_Vulkan,
                            "EmuSuNi: the picture black {:.0f} s since its renderer started {:.0f} s ago, the game "
                            "drawing ({} draws in the last {:.1f} s) (3ds-black-watch; emusuni_3ds_black_ms for "
                            "the app): of {} points, {} black, {} with alpha 0",
                            B::black_s, since, new_draws, dt, B::pending_points, black, clear);
                B::LogScreens("since it started");
            }
        }
    }
    if (B::pending || B::pictures % B::EVERY != 0) {
        return;
    }
    // The points: the middles of a 4x4 grid on the top screen, of a 4x2 one on the bottom.
    std::array<u16, B::POINTS * 2> at{};
    u32 n = 0;
    const auto add = [&](const Common::Rectangle<u32>& r, u32 columns, u32 rows) {
        if (r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        for (u32 y = 0; y < rows; y++) {
            for (u32 x = 0; x < columns; x++) {
                const u32 px = r.left + (r.right - r.left) * (2 * x + 1) / (2 * columns);
                const u32 py = r.top + (r.bottom - r.top) * (2 * y + 1) / (2 * rows);
                if (n < B::POINTS && px < frame->width && py < frame->height) {
                    at[2 * n] = static_cast<u16>(px);
                    at[2 * n + 1] = static_cast<u16>(py);
                    n++;
                }
            }
        }
    };
    add(layout.top_screen, 4, 4);
    add(layout.bottom_screen, 4, 2);
    if (n == 0) {
        return;
    }
    scheduler.Record([image = frame->image, buffer = vk::Buffer{B::buffer}, at, n](vk::CommandBuffer cmdbuf) {
        std::array<vk::BufferImageCopy, B::POINTS> copies{};
        for (u32 p = 0; p < n; p++) {
            copies[p] = vk::BufferImageCopy{
                .bufferOffset = p * 4,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {static_cast<s32>(at[2 * p]), static_cast<s32>(at[2 * p + 1]), 0},
                .imageExtent = {1, 1, 1},
            };
        }
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const vk::ImageMemoryBarrier to_copy = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        const vk::ImageMemoryBarrier back = {
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        const vk::MemoryBarrier to_host = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlags{}, {}, {}, to_copy);
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 vk::ArrayProxy<const vk::BufferImageCopy>(n, copies.data()));
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
                               vk::DependencyFlags{}, to_host, {}, back);
    });
    B::pending = true;
    B::pending_points = n;
    B::pending_tick = scheduler.CurrentTick();
}
#endif

void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped) {
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        DrawScreens(frame, layout, flipped);
#ifdef EMUSUNI_PORT
        // EmuSuNi (01.06): this picture's serial, for the app (emusuni_3ds_image_serial in set_image, and
''', r'''
        DrawScreens(frame, layout, flipped);
#ifdef EMUSUNI_PORT
        if (EmuSuNiBlack::on && !EmuSuNiBlack::seen && &window == &main_present_window) {
            EmuSuNiBlackWatch(instance, scheduler, frame, layout); // (01.16) 3ds-black-watch
        }
        // EmuSuNi (01.06): this picture's serial, for the app (emusuni_3ds_image_serial in set_image, and
''')

# --- 01.17: the JIT warm-up again after a reset and after a save state's load: both make the game's JITs again (a
# new system, or the state's processes). Until 01.16 it ran only as the game loaded. In 01.16 the app's reset for the
# black picture (10 s after Castlevania's start) threw away the 73,134 blocks it had compiled, and the game compiled
# ~105,000 while it played (21 frames of 100 to 512 ms). Now retro_reset and retro_unserialize keep the list, as
# retro_unload_game does (written only if it grew), and start the warm-up again on the new JITs. And the games of
# 3ds-jit-big-games (Castlevania: Mirror of Fate: a list of ~115,000 blocks, ~31 MiB on the PS4, ~760 new ones a
# session) let it fill all but a quarter of core 0's code cache, not half. In 01.16 (New 3DS Mode on: 40 MiB) it
# stopped at 73,134 of 115,480; now it can reach ~110,000.
patch('src/citra_libretro/citra_libretro.cpp', r'''
void retro_reset() {
    LOG_DEBUG(Frontend, "retro_reset");
    Core::System::GetInstance().Shutdown();
''', r'''
void retro_reset() {
    LOG_DEBUG(Frontend, "retro_reset");
#ifdef EMUSUNI_PORT
    // (01.17) The JIT warm-up's list kept and the warm-up made new: it runs again on the new system's JITs.
    EmuSuNi::JitWarm::End(Core::System::GetInstance());
#endif
    Core::System::GetInstance().Shutdown();
''')
patch('src/citra_libretro/citra_libretro.cpp', r'''
    std::vector<u8> buffer(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    try {
        return system.LoadStateBuffer(std::move(buffer));
''', r'''
    std::vector<u8> buffer(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
#ifdef EMUSUNI_PORT
    // (01.17) The state's processes come with new JITs: the warm-up's list kept, the warm-up again on them.
    EmuSuNi::JitWarm::End(system);
#endif
    try {
        return system.LoadStateBuffer(std::move(buffer));
''')
patch('externals/dynarmic/src/dynarmic/interface/A32/config.h', r'''
inline bool emusuni_idle_loops = false;
#endif
''', r'''
inline bool emusuni_idle_loops = false;
/// EmuSuNi (01.17): the part of the code cache the JIT warm-up leaves to the game, in quarters: 2 (half, since
/// 01.01), 1 for the games of 3ds-jit-big-games (src/core/arm/dynarmic/arm_dynarmic.cpp), whose lists hold nearly
/// all of their code. 8 MiB at the least either way.
inline unsigned emusuni_warm_keep_quarters = 2;
#endif
''')
patch('externals/dynarmic/src/dynarmic/backend/x64/a32_interface.cpp', r'''
        const std::size_t emusuni_keep = std::max<std::size_t>(8 * 1024 * 1024, conf.code_cache_size / 2);
''', r'''
        // (01.17: a quarter for the games of 3ds-jit-big-games, A32/config.h)
        const std::size_t emusuni_keep =
            std::max<std::size_t>(8 * 1024 * 1024, conf.code_cache_size / 4 * emusuni_warm_keep_quarters);
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    if (GetID() == 0) {
        config.code_cache_size += EmuSuNi::JitCoreExtra(); // (01.16) 3ds-jit-big-games: Castlevania, 16 MiB more
    }
''', r'''
    if (GetID() == 0) {
        config.code_cache_size += EmuSuNi::JitCoreExtra(); // (01.16) 3ds-jit-big-games: Castlevania, 16 MiB more
        // (01.17) And its warm-up may fill all but a quarter of it (dynarmic's A32/config.h).
        Dynarmic::A32::emusuni_warm_keep_quarters = EmuSuNi::JitCoreExtra() != 0 ? 1 : 2;
    }
''')
patch('src/common/emusuni_jit.cpp', r'''
        LOG_INFO(Common, "EmuSuNi: {:016X}: core 0's JIT 16 MiB bigger, and its memory (3ds-jit-big-games; off "
                         "with the word off)",
''', r'''
        LOG_INFO(Common, "EmuSuNi: {:016X}: core 0's JIT 16 MiB bigger, and its memory; its warm-up may fill all "
                         "but a quarter of it (01.17) (3ds-jit-big-games; off with the word off)",
''')

# --- 01.17, also: Castlevania: Mirror of Fate's draws wait for their pipelines (3ds-sync-shaders-games; the word off
# in its file: skipped until built, as every other game; EMUSUNI_3DS_SYNC_SHADERS_GAMES=off on the PC). In 01.16 the
# owner saw important things appear and then go; a draw whose pipeline isn't built yet is skipped (async), and the
# 600-frame line that counts them is cut by the app's log, so it can't be told. Now it waits (its parts are built
# once and kept, so a wait is a link of microseconds, or a part's compile the first time), and a line of its own says
# the skipped draws of each 600 frames when there are any. And 3ds-black-watch reads on for the first minute after
# the first picture (Xenoblade's start in 01.16: a picture with 8 points not black at 4 s, then nothing the owner
# could see) and says then how many readings were black and how many changed; its black time, for the app's reset,
# still only before a first picture.
patch('src/common/emusuni_jit.h', r'''
std::size_t JitCoreExtra();
''', r'''
std::size_t JitCoreExtra();
// (01.17) The game JitSetTitle was given (its program ID), 0 before one.
std::uint64_t JitTitle();
''')
patch('src/common/emusuni_jit.cpp', r'''
size_t g_extra = 0; // (01.16) JitCoreExtra(): set by JitSetTitle before the game's JITs are made
''', r'''
size_t g_extra = 0; // (01.16) JitCoreExtra(): set by JitSetTitle before the game's JITs are made
std::uint64_t g_title = 0; // (01.17) JitTitle()
''')
patch('src/common/emusuni_jit.cpp', r'''
    std::scoped_lock lock{g_lock};
    g_extra = big ? 16u << 20 : 0;
''', r'''
    std::scoped_lock lock{g_lock};
    g_extra = big ? 16u << 20 : 0;
    g_title = title_id; // (01.17)
''')
patch('src/common/emusuni_jit.cpp', r'''
std::size_t JitCoreExtra() {
''', r'''
std::uint64_t JitTitle() {
    std::scoped_lock lock{g_lock};
    return g_title;
}

std::size_t JitCoreExtra() {
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
#include "common/emusuni_stats.h"
''', r'''
#include "common/emusuni_stats.h"
#include "common/emusuni_async.h" // (01.17: 3ds-sync-shaders-games)
#include "common/emusuni_jit.h"
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
RasterizerVulkan::RasterizerVulkan(Memory::MemorySystem& memory, Pica::PicaCore& pica,
''', r'''
#ifdef EMUSUNI_PORT
// (01.17) 3ds-sync-shaders-games: the games whose draws wait for their pipelines, not skipped until built: Castlevania:
// Mirror of Fate (USA, EUR, JPN).
static bool EmuSuNiSyncShadersGame() {
    const std::uint64_t title = EmuSuNi::JitTitle();
    const bool game = title == 0x0004000000096600 || title == 0x000400000009E500 || title == 0x00040000000BAC00;
    if (!game || !Settings::values.async_shader_compilation.GetValue() ||
        EmuSuNi::Async::TestFile("3ds-sync-shaders-games", "EMUSUNI_3DS_SYNC_SHADERS_GAMES", 0) < 0) {
        return false;
    }
    LOG_INFO(Render_Vulkan, "EmuSuNi: {:016X}: every draw waits for its pipeline, none skipped "
             "(3ds-sync-shaders-games; off with the word off)", title);
    return true;
}
#endif

RasterizerVulkan::RasterizerVulkan(Memory::MemorySystem& memory, Pica::PicaCore& pica,
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
      async_shaders{Settings::values.async_shader_compilation.GetValue()} {
''', r'''
#ifdef EMUSUNI_PORT
      async_shaders{Settings::values.async_shader_compilation.GetValue() && !EmuSuNiSyncShadersGame()} {
#else
      async_shaders{Settings::values.async_shader_compilation.GetValue()} {
#endif
''')
patch('src/common/emusuni_frame_stats.h', r'''
            LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, dt, dc, (now_us - period_t0) / 1000, tpu));
''', r'''
            LOG_INFO(Frontend, "EmuSuNi: 600 frames in {}", Describe(d, dt, dc, (now_us - period_t0) / 1000, tpu));
            if (dc[DrawsSkipped] != 0) { // (01.17: a line of its own; the one above is cut by the app's log)
                LOG_INFO(Frontend, "EmuSuNi: {} draws skipped in those 600 frames: their pipelines not built yet "
                         "(async; 3ds-sync-shaders waits)", dc[DrawsSkipped]);
            }
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
#ifdef EMUSUNI_PORT
#include <atomic>
#include <cstdlib>
''', r'''
#ifdef EMUSUNI_PORT
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
bool seen = false;    // a picture with something in it since the start
''', r'''
bool seen = false;    // a picture with something in it since the start
// (01.17) Read on for the first minute (after a first picture too), then a line: the readings, how many were black
// and how many changed from the one before.
u64 minute_readings = 0, minute_black = 0, minute_changed = 0;
bool minute_said = false;
std::array<u8, POINTS * 4> last_points{};
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        B::seen = false;
        B::black_s = 0.0;
''', r'''
        B::seen = false;
        B::minute_readings = B::minute_black = B::minute_changed = 0; // (01.17)
        B::minute_said = false;
        B::last_points.fill(0);
        B::black_s = 0.0;
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        if (!is_black) {
            B::seen = true;
''', r'''
        if (since < 60.0) { // (01.17) the first minute
            B::minute_readings++;
            B::minute_black += is_black ? 1 : 0;
            B::minute_changed +=
                std::memcmp(B::last_points.data(), B::mapped, std::size_t{B::pending_points} * 4) != 0 ? 1 : 0;
            std::memcpy(B::last_points.data(), B::mapped, std::size_t{B::pending_points} * 4);
        } else if (!B::minute_said) {
            B::minute_said = true;
            LOG_INFO(Render_Vulkan,
                     "EmuSuNi: 3ds-black-watch: its first minute: {} readings, {} of them black, {} changed from the "
                     "one before; now {} of {} points not black",
                     B::minute_readings, B::minute_black, B::minute_changed, B::pending_points - black,
                     B::pending_points);
        }
        if (!B::seen && !is_black) {
            B::seen = true;
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        if (new_draws != 0) {
            B::black_s += dt;
''', r'''
        if (!B::seen && new_draws != 0) { // (01.17: only before a first picture)
            B::black_s += dt;
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        if (EmuSuNiBlack::on && !EmuSuNiBlack::seen && &window == &main_present_window) {
''', r'''
        if (EmuSuNiBlack::on && (!EmuSuNiBlack::seen || !EmuSuNiBlack::minute_said) && // (01.17)
            &window == &main_present_window) {
''')

# --- 01.17, also: Castlevania: Mirror of Fate's GPU commands at their time, exactly (3ds-pica-strict-games; the word
# off in its file: as in 01.16; EMUSUNI_3DS_PICA_STRICT_GAMES=off on the PC). The game draws less when its GPU looks
# slow: on the PC (CPU only, its state, walking), with the commands' thread 200 us slower a command (the PS4's), it
# drew half the draws its geometry shaders make (7,564 of 14,128 in 600 frames) and 9 per cent fewer in all, 3,752
# and 19 per cent without 3ds-pica-on-time; on the owner's PS4 in 01.16 "important things appear and then go". With
# 01.16's interrupt at its time a list still ends late when the thread is behind by more than its estimate. Now the
# emulation thread waits for each command 300 us of emulated time after sending it (every interrupt's delay is its
# estimate and the GSP's 300 us), then takes its results: the delay counted from its sending, the GSP's 300 us
# included, ends it exactly at its time. And 3ds-pica-on-time counts the thread's lag against those 300 us too, for
# every game (gsp_gpu.cpp).
patch('src/common/emusuni_gpu_time.h', r'''
inline std::atomic<bool> on_time{true};
''', r'''
inline std::atomic<bool> on_time{true};
// (01.17) 3ds-pica-strict-games: the emulation thread waits for each GPU command 300 us of emulated time after sending
// it (video_core/gpu.cpp), so its interrupt comes exactly at its time whatever its thread's lag.
inline std::atomic<bool> strict{false};
// (01.17) The thread's lag for the interrupt being signalled (the emulation thread's: GPU's interrupt, then the GSP's
// delay with its own 300 us, core/hle/service/gsp/gsp_gpu.cpp).
inline std::uint64_t gsp_lag_ns = 0;
''')
patch('src/video_core/emusuni_async_pica.h', r'''
        std::uint64_t submit_ns = 0; // (01.16) the emulated time the command was sent
    };
''', r'''
        std::uint64_t submit_ns = 0; // (01.16) the emulated time the command was sent
        std::uint64_t seq = 0;       // (01.17) its number (Submit's), for WaitFor
    };
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    void Submit(const Service::GSP::Command& command, std::uint64_t submit_ns = 0) {
        Item item;
        item.command = command;
        item.submit_ns = submit_ns;
        Push(item);
    }
''', r'''
    std::uint64_t Submit(const Service::GSP::Command& command, std::uint64_t submit_ns = 0) {
        Item item;
        item.command = command;
        item.submit_ns = submit_ns;
        item.seq = ++submitted_seq; // (01.17)
        Push(item);
        return item.seq;
    }
    // (01.17) 3ds-pica-strict-games: until the command `seq` (Submit's) is finished, then the results taken. (Its
    // spin, then the thread's idle: all it has.)
    void WaitFor(std::uint64_t seq) {
        if (!OnPicaThread() && completed_seq.load(std::memory_order_acquire) < seq) {
            const Stats::Doing doing{"waiting for a GPU command at its time (3ds-pica-strict-games)"};
            const std::uint64_t k0 = Stats::Ticks();
            if (!Spin([this, seq] { return completed_seq.load(std::memory_order_acquire) >= seq; })) {
                std::unique_lock lock{m};
                idle_cv.wait(lock, [this, seq] {
                    return completed_seq.load(std::memory_order_acquire) >= seq || (queue.empty() && !running) ||
                           stop;
                });
            }
            Stats::ticks[Stats::PicaWaitVBlankTicks + DrainOther] += Stats::Ticks() - k0;
            Stats::counts[Stats::PicaWaitVBlank + DrainOther]++;
        }
        Deliver(true);
    }
''')
patch('src/video_core/emusuni_async_pica.h', r'''
            done_commands++;
            unfinished.fetch_sub(1, std::memory_order_release);
''', r'''
            done_commands++;
            if (item.seq != 0) {
                completed_seq.store(item.seq, std::memory_order_release); // (01.17)
            }
            unfinished.fetch_sub(1, std::memory_order_release);
''')
patch('src/video_core/emusuni_async_pica.h', r'''
    std::atomic<unsigned> queued{0};      // in the queue (the spins' view of it)
''', r'''
    std::atomic<unsigned> queued{0};      // in the queue (the spins' view of it)
    std::uint64_t submitted_seq = 0;      // (01.17) the last command's number (the emulation thread's)
    std::atomic<std::uint64_t> completed_seq{0}; // (01.17) the last one finished
''')
patch('src/video_core/gpu.cpp', r'''
                        if (lag_ns > delay_ns) {
                            late++;
                            late_ns += lag_ns - delay_ns;
                        }
''', r'''
                        if (lag_ns > delay_ns + 300000) { // (01.17: the GSP's 300 us counted)
                            late++;
                            late_ns += lag_ns - delay_ns - 300000;
                        }
''')
patch('src/video_core/gpu.cpp', r'''
                    delay_ns = delay_ns > lag_ns ? delay_ns - lag_ns : 0;
                }
                impl->signal_interrupt(static_cast<Service::GSP::InterruptId>(id), delay_ns);
''', r'''
                    // (01.17) The lag taken from the whole delay, the GSP's 300 us too (gsp_gpu.cpp).
                    EmuSuNi::GpuTime::gsp_lag_ns = lag_ns;
                }
                impl->signal_interrupt(static_cast<Service::GSP::InterruptId>(id), delay_ns);
                EmuSuNi::GpuTime::gsp_lag_ns = 0;
''')
patch('src/video_core/gpu.cpp', r'''
        emusuni_pica->SetInvalidate([this](u32 start, u32 size) { // (01.12)
''', r'''
        // (01.17) 3ds-pica-strict-games: a command's wait at its time (GPU::Execute).
        emusuni_strict_event = impl->timing.RegisterEvent(
            "EmuSuNi: a GPU command at its time", [this](std::uintptr_t seq, s64) {
                if (emusuni_pica) {
                    emusuni_pica->WaitFor(static_cast<std::uint64_t>(seq));
                }
            });
        emusuni_pica->SetInvalidate([this](u32 start, u32 size) { // (01.12)
''')
patch('src/video_core/gpu.cpp', r'''
            emusuni_pica->Submit(command, static_cast<u64>(impl->timing.GetGlobalTimeUs().count()) * 1000);
            return;
''', r'''
            const u64 emusuni_seq =
                emusuni_pica->Submit(command, static_cast<u64>(impl->timing.GetGlobalTimeUs().count()) * 1000);
            if (emusuni_strict_event != nullptr && EmuSuNi::GpuTime::strict.load(std::memory_order_relaxed)) {
                // (01.17) 3ds-pica-strict-games: waited for 300 us after (every interrupt's delay is at least that).
                impl->timing.ScheduleEvent(nsToCycles(300000), emusuni_strict_event,
                                           static_cast<std::uintptr_t>(emusuni_seq));
            }
            return;
''')
patch('src/video_core/gpu.cpp', r'''
constexpr VAddr VADDR_GPU = 0x1EF00000;
''', r'''
constexpr VAddr VADDR_GPU = 0x1EF00000;
#ifdef EMUSUNI_PORT
// (01.17) 3ds-pica-strict-games: a command's wait at its time (GPU::GPU registers it in the current system's timing).
static Core::TimingEventType* emusuni_strict_event = nullptr;
#endif
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
#include "common/settings.h"
''', r'''
#include "common/settings.h"
#ifdef EMUSUNI_PORT
#include "common/emusuni_gpu_time.h"
#endif
''')
patch('src/core/hle/service/gsp/gsp_gpu.cpp', r'''
#ifdef SHOW_AVERAGE_TIME_PER_FRAME
    track_average(false);
#endif
''', r'''
#ifdef EMUSUNI_PORT
    // (01.17) A GPU command from its own thread (3ds-pica-on-time, video_core/gpu.cpp): its delay counted from its
    // sending, this 300 us included; at once if it finished later still.
    if (EmuSuNi::GpuTime::gsp_lag_ns != 0) {
        const u64 lag = EmuSuNi::GpuTime::gsp_lag_ns;
        wait_delay_ns = wait_delay_ns > lag ? wait_delay_ns - lag : 0;
    }
#endif
#ifdef SHOW_AVERAGE_TIME_PER_FRAME
    track_average(false);
#endif
''')
patch('src/core/emusuni_switches.h', r'''
    if (on_time != EmuSuNi::GpuTime::on_time.exchange(on_time, std::memory_order_relaxed)) {
        LOG_INFO(Core, "EmuSuNi: a command list's end at its time (3ds-pica-on-time): {}", on_time ? "on" : "off");
    }
''', r'''
    if (on_time != EmuSuNi::GpuTime::on_time.exchange(on_time, std::memory_order_relaxed)) {
        LOG_INFO(Core, "EmuSuNi: a command list's end at its time (3ds-pica-on-time): {}", on_time ? "on" : "off");
    }
    // (01.17) Castlevania's commands waited for at their time, exactly (3ds-pica-strict-games; with 3ds-pica-on-time).
    const bool strict =
        on_time &&
        (title_id == 0x0004000000096600 || title_id == 0x000400000009E500 || title_id == 0x00040000000BAC00) &&
        EmuSuNi::Async::TestFile("3ds-pica-strict-games", "EMUSUNI_3DS_PICA_STRICT_GAMES", 0) >= 0;
    EmuSuNi::GpuTime::strict.store(strict, std::memory_order_relaxed);
    if (strict) {
        LOG_INFO(Core, "EmuSuNi: {:016X}: its GPU commands waited for at their time, 300 us after each "
                       "(3ds-pica-strict-games; off with the word off)",
                 title_id);
    }
''')

# --- 01.18: Azahar's texture filter never, whatever the app sends. 01.17's (the app's 3DS Texture Filter, ScaleForce,
# per game) on the PS4: a GPU hang in Luigi's Mansion: Dark Moon and textures missing in Castlevania; the owner's
# decision: out, the app's option too. 01.17's cap (3ds-texture-filter-scale) is gone with it.
patch('src/citra_libretro/core_settings.cpp', r'''
    Settings::values.texture_filter =
        GetTextureFilter(LibRetro::FetchVariable(config::graphics::texture_filter, "none"));
''', r'''
    Settings::values.texture_filter =
        GetTextureFilter(LibRetro::FetchVariable(config::graphics::texture_filter, "none"));
    // EmuSuNi (01.18): Azahar's texture filter never, whatever the app sends (01.17's ScaleForce on the PS4: a GPU hang
    // in Luigi's Mansion: Dark Moon, textures missing in Castlevania).
    Settings::values.texture_filter = Settings::TextureFilter::NoFilter;
''')
# The header 01.17 patched for the cap, written back as Azahar has it: the patch step writes only the files it patches,
# so a tree patched by 01.17 would keep it otherwise.
patch('src/video_core/rasterizer_cache/rasterizer_cache_base.h', r'''
    Settings::TextureFilter filter;
''', r'''
    Settings::TextureFilter filter;
''')

# --- 01.18: where a black picture at a game's start comes from, said by 3ds-black-watch (01.16) with its lines: since
# 01.15 the PS4's 3DS games start black more often than not, with their sound, and the PC never (01.17's logs: the
# black starts make the very draws of the good ones - Castlevania 11,669 draws of 23,994,018 vertices in its first 600
# frames both times -, the top screen stays black, its surface found). A line of its own, after each of the watch's:
# the draws left out for having no buffer to draw into (vk_rasterizer.cpp), the command lists and top-screen copies
# Azahar's right-eye guesser left out (right_eye_disabler.cpp: it skips whole lists; on the PC it leaves out some of
# Castlevania's from frame ~300), the screen copies (display transfers) and texture copies done on the CPU (gpu.cpp).
# Counting only: nothing drawn differently.
addfile('src/common/emusuni_black_counts.h', r'''// EmuSuNi (01.18): what 3ds-black-watch says about a black picture
// (video_core/renderer_vulkan/renderer_vulkan.cpp), counted where it happens; counting only.
#pragma once

#include <atomic>
#include <cstdint>

namespace EmuSuNi::BlackCounts {
enum Counter {
    DrawsNoBuffer,  // draws left out for having no colour or depth buffer (vk_rasterizer.cpp)
    RightEyeLists,  // command lists Azahar's right-eye guesser left out (right_eye_disabler.cpp)
    RightEyeCopies, // top-screen copies it left out
    Transfers,      // screen copies (display transfers) asked for
    TransfersCpu,   // of them, done on the CPU (sw_blitter), not the GPU
    CopiesCpu,      // texture copies done on the CPU
    Count
};
inline std::atomic<std::uint64_t> values[Count];
inline void Add(Counter c) {
    values[c].fetch_add(1, std::memory_order_relaxed);
}
} // namespace EmuSuNi::BlackCounts
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
#include "common/emusuni_jit.h"
''', r'''
#include "common/emusuni_jit.h"
#include "common/emusuni_black_counts.h" // (01.18)
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
    const Framebuffer* framebuffer = fb_helper.Framebuffer();
    if (!framebuffer->Handle()) {
        return true;
    }
''', r'''
    const Framebuffer* framebuffer = fb_helper.Framebuffer();
    if (!framebuffer->Handle()) {
        EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::DrawsNoBuffer); // (01.18, 3ds-black-watch)
        return true;
    }
''')
patch('src/video_core/gpu.cpp', r'''
#include "video_core/right_eye_disabler.h"
''', r'''
#include "video_core/right_eye_disabler.h"
#ifdef EMUSUNI_PORT
#include "common/emusuni_black_counts.h" // (01.18)
#endif
''')
patch('src/video_core/gpu.cpp', r'''
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
            impl->sw_blitter->TextureCopy(config);
        }
''', r'''
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
#ifdef EMUSUNI_PORT
            EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::CopiesCpu); // (01.18, 3ds-black-watch)
#endif
            impl->sw_blitter->TextureCopy(config);
        }
''')
patch('src/video_core/gpu.cpp', r'''
    } else {
        if (right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                           config.input_height)) {
            if (!impl->rasterizer->AccelerateDisplayTransfer(config)) {
                impl->sw_blitter->DisplayTransfer(config);
            }
''', r'''
    } else {
#ifdef EMUSUNI_PORT
        EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::Transfers); // (01.18, 3ds-black-watch)
#endif
        if (right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                           config.input_height)) {
            if (!impl->rasterizer->AccelerateDisplayTransfer(config)) {
#ifdef EMUSUNI_PORT
                EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::TransfersCpu); // (01.18)
#endif
                impl->sw_blitter->DisplayTransfer(config);
            }
''')
patch('src/video_core/right_eye_disabler.cpp', r'''
#include "right_eye_disabler.h"
''', r'''
#include "right_eye_disabler.h"
#ifdef EMUSUNI_PORT
#include "common/emusuni_black_counts.h" // (01.18)
#endif
''')
patch('src/video_core/right_eye_disabler.cpp', r'''
        if (top_screen_transfered) {
            cmd_trigger_blocked = true;
            return false;
        }
''', r'''
        if (top_screen_transfered) {
            cmd_trigger_blocked = true;
#ifdef EMUSUNI_PORT
            EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::RightEyeLists); // (01.18, 3ds-black-watch)
#endif
            return false;
        }
''')
patch('src/video_core/right_eye_disabler.cpp', r'''
        if (src_address == top_screen_buf && cmd_trigger_blocked) {
            top_screen_blocked = true;
            return false;
        }
''', r'''
        if (src_address == top_screen_buf && cmd_trigger_blocked) {
            top_screen_blocked = true;
#ifdef EMUSUNI_PORT
            EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::RightEyeCopies); // (01.18, 3ds-black-watch)
#endif
            return false;
        }
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
namespace EmuSuNiBlack {
constexpr u32 POINTS = 24; // read back: 4x4 on the top screen, 4x2 on the bottom
''', r'''
#include "common/emusuni_black_counts.h" // (01.18)
namespace EmuSuNiBlack {
constexpr u32 POINTS = 24; // read back: 4x4 on the top screen, 4x2 on the bottom
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
void LogScreens(const char* until) {
    LOG_INFO(Render_Vulkan, "EmuSuNi: 3ds-black-watch: its pictures {}: the top screen {}; the bottom {}", until,
             Screen(0), Screen(1));
}
''', r'''
void LogScreens(const char* until) {
    LOG_INFO(Render_Vulkan, "EmuSuNi: 3ds-black-watch: its pictures {}: the top screen {}; the bottom {}", until,
             Screen(0), Screen(1));
}
// (01.18) Where a black picture may come from (common/emusuni_black_counts.h), since the renderer started.
u64 counts_at[EmuSuNi::BlackCounts::Count];
void LogCounts(const char* until) {
    namespace C = EmuSuNi::BlackCounts;
    const auto d = [](C::Counter c) { return C::values[c].load(std::memory_order_relaxed) - counts_at[c]; };
    LOG_INFO(Render_Vulkan,
             "EmuSuNi: 3ds-black-watch: {}: draws with no buffer to draw into {}; the right-eye guesser left out {} "
             "command lists and {} top-screen copies; screen copies {} ({} on the CPU); texture copies on the CPU {}",
             until, d(C::DrawsNoBuffer), d(C::RightEyeLists), d(C::RightEyeCopies), d(C::Transfers),
             d(C::TransfersCpu), d(C::CopiesCpu));
}
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        B::draws_at = EmuSuNi::Stats::counts[EmuSuNi::Stats::Draws];
''', r'''
        B::draws_at = EmuSuNi::Stats::counts[EmuSuNi::Stats::Draws];
        for (int c = 0; c < EmuSuNi::BlackCounts::Count; c++) { // (01.18)
            B::counts_at[c] = EmuSuNi::BlackCounts::values[c].load(std::memory_order_relaxed);
        }
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        EmuSuNiBlack::LogScreens("since it started");
''', r'''
        EmuSuNiBlack::LogScreens("since it started");
        EmuSuNiBlack::LogCounts("since it started"); // (01.18)
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
            B::LogScreens("until then");
''', r'''
            B::LogScreens("until then");
            B::LogCounts("until then"); // (01.18)
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
                B::LogScreens("since it started");
''', r'''
                B::LogScreens("since it started");
                B::LogCounts("since it started"); // (01.18)
''')

# --- 01.18: the right eye drawn, as upstream. Azahar's disabler (on in every game from 00.63 to 01.17: the PS4 shows
# no 3D) guesses the right eye's command lists from the order of the lists and the top screen's copies; on the PS4 it
# took Castlevania's and DKC's first picture for it (the owner's test of 01.17 with the file 3ds-right-eye: both 3 of
# 3 at the first try; without it black starts and resets). The PC never shows that, but there too it leaves out lists
# of games that draw no right eye (Castlevania's play: 2057 lists and 457 copies in 2400 frames, 4.5 per cent of its
# draws; Mario Kart 7, Tri Force Heroes). Drawing it costs the games that draw both eyes (the PC, the first 1800
# frames: A Link Between Worlds 2.1 times the draws, Driver and Sonic Generations 1.4, Super Mario 3D Land 1.3,
# Captain Toad 1.2; the others about the same). Left out when the app's 3DS Right Eye says Not Drawn for the game (its
# Auto draws it since 01.18), or in every game with the word off in the file /data/EmuSuNi/3ds-right-eye (anything
# else there: drawn in every game, as before); EMUSUNI_3DS_RIGHT_EYE on the PC first (0 or off: left out).
patch('src/citra_libretro/core_settings.cpp', r'''
        "Draw the right eye's picture of the 3D (never shown on the PS4; a few games need it drawn).",
        nullptr,
        config::category::graphics,
        {
            { config::disabled, "Disabled" },
            { config::enabled, "Enabled" },
            { nullptr, nullptr }
        },
        config::disabled
    },
''', r'''
        "Draw the right eye's picture of the 3D (never shown on the PS4; left out, some games open black).",
        nullptr,
        config::category::graphics,
        {
            { config::disabled, "Disabled" },
            { config::enabled, "Enabled" },
            { nullptr, nullptr }
        },
        config::enabled
    },
''')
patch('src/citra_libretro/core_settings.cpp', r'''
    // EmuSuNi: the right eye is never shown on the PS4 (no 3D): not drawn (scripts/build-3ds-core.sh).
    // (00.81: and the app's 3DS Right Eye, per game.)
    Settings::values.disable_right_eye_render =
        std::getenv("EMUSUNI_3DS_RIGHT_EYE") == nullptr && ::access("/data/EmuSuNi/3ds-right-eye", F_OK) != 0 &&
        LibRetro::FetchVariable(config::graphics::emusuni_right_eye, config::disabled) != config::enabled;
    {
        static int said = -1; // (said once, and again if it changes)
        const int now = Settings::values.disable_right_eye_render.GetValue() ? 1 : 0;
        if (now != said) {
            LOG_INFO(Frontend, "EmuSuNi: right eye: {}",
                     now ? "not drawn (the PS4 shows no 3D)"
                         : "drawn (3DS Right Eye, or the file /data/EmuSuNi/3ds-right-eye)");
            said = now;
        }
    }
''', r'''
    // EmuSuNi (01.18): the right eye drawn, as upstream (scripts/build-3ds-core.sh: Azahar's disabler, on in every
    // game from 00.63 to 01.17, opened Castlevania and DKC black on the PS4). Left out when the app's 3DS Right Eye
    // says Not Drawn for the game, or in every game with the word off in the file /data/EmuSuNi/3ds-right-eye
    // (anything else there: drawn in every game); EMUSUNI_3DS_RIGHT_EYE on the PC first (0 or off: left out).
    {
        bool left_out =
            LibRetro::FetchVariable(config::graphics::emusuni_right_eye, config::enabled) != config::enabled;
        char text[16] = "";
        bool given = false;
        if (const char* env = std::getenv("EMUSUNI_3DS_RIGHT_EYE")) {
            std::snprintf(text, sizeof(text), "%s", env);
            given = true;
        } else if (std::FILE* file = std::fopen("/data/EmuSuNi/3ds-right-eye", "rb")) {
            const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
            std::fclose(file);
            text[length] = '\0';
            given = true;
        }
        if (given) {
            for (char* c = text; *c != '\0'; c++) {
                if (*c >= 'A' && *c <= 'Z') {
                    *c = static_cast<char>(*c - 'A' + 'a');
                }
            }
            left_out = std::strstr(text, "off") != nullptr || std::strcmp(text, "0") == 0;
        }
        Settings::values.disable_right_eye_render = left_out;
    }
    {
        static int said = -1; // (said once, and again if it changes)
        const int now = Settings::values.disable_right_eye_render.GetValue() ? 1 : 0;
        if (now != said) {
            LOG_INFO(Frontend, "EmuSuNi: right eye: {}",
                     now ? "not drawn: Azahar's disabler (the app's 3DS Right Eye Not Drawn, or off in the file "
                           "/data/EmuSuNi/3ds-right-eye)"
                         : "drawn (01.18; the word off in /data/EmuSuNi/3ds-right-eye: not drawn in every game)");
            said = now;
        }
    }
''')
patch('src/citra_libretro/core_settings.cpp', r'''
#include <cstdlib>
#include <unistd.h>
''', r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
''')
# --- 01.18: Castlevania's draws wait for their pipelines (01.17's 3ds-sync-shaders-games) only while pipelines are
# made by parts (a link of microseconds, or a part's compile the first time). With whole pipelines - 3ds-gpl-crashed
# (01.17's, on the owner's PS4 since a start that ended while making them), 3ds-gpl-off, a part the driver refused -
# each wait was a whole compile, ~1.7 s on the PS4: a new place of Castlevania made 86 in 600 frames, five frames of
# 4.8 to 7.9 s with the Vulkan worker waiting for them, and the owner restarted. Then its draws are skipped until
# built, as every other game's.
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h', r'''
    void SetAccurateMul(bool _accurate_mul) {
        profile.enable_accurate_mul = _accurate_mul;
    }

private:
''', r'''
    void SetAccurateMul(bool _accurate_mul) {
        profile.enable_accurate_mul = _accurate_mul;
    }

#ifdef EMUSUNI_PORT
    /// (01.18) Whether pipelines are made by parts now (off from the start, or since the driver refused one).
    bool EmuSuNiPartsOn() const {
        return emusuni_parts.on.load(std::memory_order_relaxed);
    }
#endif

private:
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.h', r'''
    bool async_shaders{false};
};
''', r'''
    bool async_shaders{false};
#ifdef EMUSUNI_PORT
    bool emusuni_sync_game{false}; // (01.18) 3ds-sync-shaders-games: its draws wait while pipelines are by parts
#endif
};
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
#ifdef EMUSUNI_PORT
      async_shaders{Settings::values.async_shader_compilation.GetValue() && !EmuSuNiSyncShadersGame()} {
#else
''', r'''
#ifdef EMUSUNI_PORT
      async_shaders{Settings::values.async_shader_compilation.GetValue()},
      emusuni_sync_game{EmuSuNiSyncShadersGame()} { // (01.18: its wait at the draw, while pipelines are by parts)
#else
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
    const bool wait_built = !async_shaders || regs.pipeline.num_vertices <= 6;
    bool emusuni_bound;
''', r'''
    // (01.18) Castlevania's draws (3ds-sync-shaders-games) wait only while pipelines are made by parts.
    const bool wait_built = !async_shaders || (emusuni_sync_game && pipeline_cache.EmuSuNiPartsOn()) ||
                            regs.pipeline.num_vertices <= 6;
    bool emusuni_bound;
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
    LOG_INFO(Render_Vulkan, "EmuSuNi: {:016X}: every draw waits for its pipeline, none skipped "
             "(3ds-sync-shaders-games; off with the word off)", title);
''', r'''
    LOG_INFO(Render_Vulkan, "EmuSuNi: {:016X}: every draw waits for its pipeline while pipelines are made by parts "
             "(3ds-sync-shaders-games; off with the word off); with whole ones skipped until built (01.18)", title);
''')

# --- 01.20: a renderer that closes leaves the pipelines still queued for the workers unbuilt. Its pipeline cache waited
# for every queued one (pipeline_workers.WaitForRequests); with whole pipelines (3ds-gpl-off, 3ds-gpl-crashed, a part
# the driver refused) each is a whole compile, ~3.7 s on the PS4, and they pile up: the disk cache's pipelines missing
# from the driver's cache are queued as the game loads, and since 01.18 Castlevania's draws queue theirs instead of
# waiting. The owner's 01.19 with 3ds-gpl-off: Castlevania's first reset by itself stopped there ("waiting for the
# shader and pipeline workers") and never went on. The ones already being built still finish (seconds at most).
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.h', r'''
    std::atomic<bool> closing{false}; // the whole pipelines still queued are dropped (the cache goes away)
''', r'''
    std::atomic<bool> closing{false}; // the whole pipelines still queued are dropped (the cache goes away)
    std::atomic<bool> gone{false};    // (01.20) the cache goes away: no pipeline still queued is built
    std::atomic<u32> dropped{0};      // (01.20) how many weren't (for the log)
''')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp', r'''
    worker->QueueWorkFront([this] {
        int none = 0;
        if (emusuni_claimed.compare_exchange_strong(none, 1)) {
            Build();
        }
    });
''', r'''
    worker->QueueWorkFront([this] {
        if (emusuni_parts != nullptr && emusuni_parts->gone.load(std::memory_order_relaxed)) {
            emusuni_parts->dropped++; // (01.20) the cache goes away
            return;
        }
        int none = 0;
        if (emusuni_claimed.compare_exchange_strong(none, 1)) {
            Build();
        }
    });
''')
patch('src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp', r'''
        worker->QueueWork([this] {
            int expected = 0;
            if (emusuni_claimed.compare_exchange_strong(expected, 1)) {
                Build();
            }
        });
''', r'''
        worker->QueueWork([this] {
            if (emusuni_parts != nullptr && emusuni_parts->gone.load(std::memory_order_relaxed)) {
                emusuni_parts->dropped++; // (01.20) the cache goes away
                return;
            }
            int expected = 0;
            if (emusuni_claimed.compare_exchange_strong(expected, 1)) {
                Build();
            }
        });
''')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp', r'''
    emusuni_parts.closing = true; // (00.94) the whole pipelines still queued: dropped
''', r'''
    emusuni_parts.closing = true; // (00.94) the whole pipelines still queued: dropped
    emusuni_parts.gone = true;    // (01.20) and every pipeline still queued left unbuilt
''')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp', r'''
    LOG_INFO(Render_Vulkan,
             "EmuSuNi: the renderer closes: saving the driver's pipeline cache (the workers: {} ms)",
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - emusuni_t0)
                                        .count()));
''', r'''
    LOG_INFO(Render_Vulkan,
             "EmuSuNi: the renderer closes: saving the driver's pipeline cache (the workers: {} ms; {} pipelines "
             "still queued left unbuilt)",
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - emusuni_t0)
                                        .count()),
             emusuni_parts.dropped.load());
''')
# --- 01.20: 3ds-black-watch says, with its counts, what each screen shows: the LCD's two buffers of the screen (their
# physical addresses, set at each swap: GPU::SetBufferSwap), which one it shows, the screen copies (display transfers)
# that wrote into each, the swaps, and the copies elsewhere. A black start (01.18 and 01.19 on the PS4: DKC, Captain
# Toad, Castlevania) had its copies on the GPU and its surfaces found, the top screen black and the bottom one never
# written (alpha 0): shown a buffer no copy writes into (the swaps), or one written black (the drawing).
patch('src/common/emusuni_black_counts.h', r'''
    CopiesCpu,      // texture copies done on the CPU
    Count
};
''', r'''
    CopiesCpu,      // texture copies done on the CPU
    // (01.20) The screen copies by where they write (GPU::MemoryTransfer), and the swaps (GPU::SetBufferSwap).
    TopInto0,       // into the top screen's buffer 0 (the LCD's address_left1)
    TopInto1,       // into its buffer 1 (address_left2)
    BottomInto0,    // into the bottom screen's buffer 0
    BottomInto1,    // into its buffer 1
    Elsewhere,      // anywhere else (textures, the right eye's buffers)
    TopSwaps,       // the top screen's swaps
    BottomSwaps,    // the bottom screen's
    Count
};
''')
patch('src/common/emusuni_black_counts.h', r'''
inline std::atomic<std::uint64_t> values[Count];
inline void Add(Counter c) {
    values[c].fetch_add(1, std::memory_order_relaxed);
}
''', r'''
inline std::atomic<std::uint64_t> values[Count];
inline void Add(Counter c) {
    values[c].fetch_add(1, std::memory_order_relaxed);
}
// (01.20) Each screen's two buffers (physical addresses) and the one it shows, as of its last swap.
inline std::atomic<std::uint32_t> buffers[2][2];
inline std::atomic<std::uint32_t> shown[2];
// (01.20) A screen copy into `out` (physical), counted by the buffer it writes.
inline void Copied(std::uint32_t out) {
    for (int screen = 0; screen < 2; screen++) {
        for (int buffer = 0; buffer < 2; buffer++) {
            if (out != 0 && buffers[screen][buffer].load(std::memory_order_relaxed) == out) {
                Add(static_cast<Counter>(TopInto0 + screen * 2 + buffer));
                return;
            }
        }
    }
    Add(Elsewhere);
}
''')
patch('src/video_core/gpu.cpp', r'''
    framebuffer.stride = info.stride;
    framebuffer.format = info.format;
    framebuffer.active_fb = info.shown_fb;
''', r'''
    framebuffer.stride = info.stride;
    framebuffer.format = info.format;
    framebuffer.active_fb = info.shown_fb;
#ifdef EMUSUNI_PORT
    if (screen_id < 2) { // (01.20, 3ds-black-watch) the screen's buffers and the one it shows
        namespace B = EmuSuNi::BlackCounts;
        B::buffers[screen_id][0].store(framebuffer.address_left1, std::memory_order_relaxed);
        B::buffers[screen_id][1].store(framebuffer.address_left2, std::memory_order_relaxed);
        B::shown[screen_id].store(framebuffer.active_fb, std::memory_order_relaxed);
        B::Add(screen_id == 0 ? B::TopSwaps : B::BottomSwaps);
    }
#endif
''')
patch('src/video_core/gpu.cpp', r'''
        EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::Transfers); // (01.18, 3ds-black-watch)
''', r'''
        EmuSuNi::BlackCounts::Add(EmuSuNi::BlackCounts::Transfers); // (01.18, 3ds-black-watch)
        EmuSuNi::BlackCounts::Copied(config.GetPhysicalOutputAddress()); // (01.20) where it writes
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
             until, d(C::DrawsNoBuffer), d(C::RightEyeLists), d(C::RightEyeCopies), d(C::Transfers),
             d(C::TransfersCpu), d(C::CopiesCpu));
}
''', r'''
             until, d(C::DrawsNoBuffer), d(C::RightEyeLists), d(C::RightEyeCopies), d(C::Transfers),
             d(C::TransfersCpu), d(C::CopiesCpu));
    // (01.20) What each screen shows: its two buffers, the one shown, the copies into each, its swaps.
    const auto b = [](int screen, int buffer) { return C::buffers[screen][buffer].load(std::memory_order_relaxed); };
    LOG_INFO(Render_Vulkan,
             "EmuSuNi: 3ds-black-watch: {}: the top screen shows buffer {} of {:08X} and {:08X}, screen copies into "
             "them {} and {}, {} swaps; the bottom shows buffer {} of {:08X} and {:08X}, copies {} and {}, {} swaps; "
             "screen copies elsewhere {}",
             until, C::shown[0].load(std::memory_order_relaxed), b(0, 0), b(0, 1), d(C::TopInto0), d(C::TopInto1),
             d(C::TopSwaps), C::shown[1].load(std::memory_order_relaxed), b(1, 0), b(1, 1), d(C::BottomInto0),
             d(C::BottomInto1), d(C::BottomSwaps), d(C::Elsewhere));
}
''')

# --- 01.21: 3ds-black-watch says through which surfaces of the rasterizer cache a black picture comes. 01.20's PS4 log
# of a black start (DKC, Castlevania; the owner: games didn't open at the first try since the port's beginning): both
# screens' buffers got their screen copies and their swaps, as in a good start, so what is copied or shown is black.
# Now, at each of its lines, for each screen: the surface the display showed last (RasterizerVulkan::AccelerateDisplay),
# the last screen copy into the screen's buffers (its source and destination surfaces, AccelerateDisplayTransfer), and
# the last draws' colour buffer (GetFramebufferSurfaces); each with its slot, address, size, scale, pixel format and
# tiling, and how it was written since it was made or recycled: draws into it (d), screen copies into it (c), loads
# from memory (l), copies from other surfaces (o), and its age in top-screen swaps. A copy from a surface no draw went
# into (made from memory), a screen showing a surface the copies don't write, or a chain that is whole (then the draws
# themselves are black) tells where it breaks. Counting only.
addfile('src/common/emusuni_black_surfaces.h', r'''// EmuSuNi (01.21): the surfaces a black picture comes through,
// for 3ds-black-watch (video_core/renderer_vulkan/renderer_vulkan.cpp), noted where the rasterizer cache uses them;
// counting only.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "common/emusuni_black_counts.h"

namespace EmuSuNi::BlackSurfaces {
// A surface as it was when noted: its slot, address, size, scale, pixel format and tiling.
struct Note {
    std::uint32_t id = 0xFFFFFFFFu, addr = 0, width = 0, height = 0, scale = 0, format = 0, tiled = 0;
};
// How a surface was written since it was made or recycled, by its slot (1024 kept: a slot's index modulo).
struct Writes {
    std::atomic<std::uint32_t> draws{0}, copies{0}, loads{0}, others{0}, made_at{0};
};
inline constexpr std::uint32_t kSlots = 1024;
inline Writes writes[kSlots];
inline Writes& Of(std::uint32_t id) {
    return writes[id % kSlots];
}
inline void Made(std::uint32_t id) {
    Writes& w = Of(id);
    w.draws = 0;
    w.copies = 0;
    w.loads = 0;
    w.others = 0;
    w.made_at = static_cast<std::uint32_t>(
        BlackCounts::values[BlackCounts::TopSwaps].load(std::memory_order_relaxed));
}
inline std::mutex mutex;              // the notes below
inline Note shown[2];                 // the surface each screen showed last (0 top, 1 bottom)
inline Note copy_src[2], copy_dst[2]; // the last screen copy into each screen's buffers
inline Note drawn;                    // the last draws' colour buffer
inline std::atomic<std::uint32_t> drawn_id{0xFFFFFFFFu};
template <class S>
Note Make(std::uint32_t id, const S& surface) {
    Note n;
    n.id = id;
    n.addr = surface.addr;
    n.width = surface.width;
    n.height = surface.height;
    n.scale = surface.res_scale;
    n.format = static_cast<std::uint32_t>(surface.pixel_format);
    n.tiled = surface.is_tiled ? 1 : 0;
    return n;
}
// The screen (0 top, 1 bottom) one of whose two buffers is at `addr` (as of its last swap), -1 none.
inline int Screen(std::uint32_t addr) {
    for (int screen = 0; screen < 2; screen++) {
        for (int buffer = 0; buffer < 2; buffer++) {
            if (addr != 0 && BlackCounts::buffers[screen][buffer].load(std::memory_order_relaxed) == addr) {
                return screen;
            }
        }
    }
    return -1;
}
// The capture of what the game drew - the top screen's last copy source - once in a black start: asked by
// 3ds-black-watch (renderer_vulkan.cpp), recorded by the next top-screen copy (vk_rasterizer.cpp) into the buffer the
// renderer made (a VkBuffer: 4 bytes a point, a grid of kDrawnColumns, its rows by the surface's shape), saved there
// once the GPU is done with it.
inline constexpr std::uint32_t kDrawnColumns = 60, kDrawnRowsMax = 200;
inline std::atomic<bool> capture_wanted{false};
inline std::atomic<bool> capture_recorded{false};
inline void* capture_buffer = nullptr; // the renderer's VkBuffer, kDrawnColumns * kDrawnRowsMax * 4 bytes
inline std::uint64_t capture_tick = 0;
inline Note capture_of; // the surface captured (its scaled size)
inline std::uint32_t capture_columns = 0, capture_rows = 0;
} // namespace EmuSuNi::BlackSurfaces
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
#include "common/emusuni_stats.h"
#include "common/emusuni_dirty_pages.h"
#endif
''', r'''
#include "common/emusuni_stats.h"
#include "common/emusuni_dirty_pages.h"
#include "common/emusuni_black_surfaces.h" // (01.21, 3ds-black-watch)
#endif
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
    surface.MarkInvalid(surface.GetInterval());
    return surface_id;
}
''', r'''
    surface.MarkInvalid(surface.GetInterval());
#ifdef EMUSUNI_PORT
    EmuSuNi::BlackSurfaces::Made(surface_id.index); // (01.21, 3ds-black-watch) its writes counted from now
#endif
    return surface_id;
}
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
            CopySurface(copy_surface, surface, copy_interval);
            notify_validated(copy_interval);
            continue;
''', r'''
            CopySurface(copy_surface, surface, copy_interval);
            notify_validated(copy_interval);
#ifdef EMUSUNI_PORT
            EmuSuNi::BlackSurfaces::Of(surface_id.index).others++; // (01.21)
#endif
            continue;
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
        if (ValidateByReinterpretation(surface, params, interval)) {
            notify_validated(interval);
            continue;
''', r'''
        if (ValidateByReinterpretation(surface, params, interval)) {
            notify_validated(interval);
#ifdef EMUSUNI_PORT
            EmuSuNi::BlackSurfaces::Of(surface_id.index).others++; // (01.21)
#endif
            continue;
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
            UploadSurface(surface, interval);
        }
        notify_validated(params.GetInterval());
''', r'''
            UploadSurface(surface, interval);
        }
        notify_validated(params.GetInterval());
#ifdef EMUSUNI_PORT
        EmuSuNi::BlackSurfaces::Of(surface_id.index).loads++; // (01.21) loaded from memory
#endif
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
        color_level = color_surface->LevelOf(color_params.addr);
        color_surface->flags |= SurfaceFlagBits::RenderTarget;
''', r'''
        color_level = color_surface->LevelOf(color_params.addr);
        color_surface->flags |= SurfaceFlagBits::RenderTarget;
#ifdef EMUSUNI_PORT
        { // (01.21, 3ds-black-watch) a draw into it; noted when the colour buffer changes
            namespace BS = EmuSuNi::BlackSurfaces;
            BS::Of(color_id.index).draws++;
            if (BS::drawn_id.exchange(color_id.index, std::memory_order_relaxed) != color_id.index) {
                std::scoped_lock lock{BS::mutex};
                BS::drawn = BS::Make(color_id.index, *color_surface);
            }
        }
#endif
''')
patch('src/video_core/rasterizer_cache/rasterizer_cache.h', r'''
    Surface& src_surface = slot_surfaces[src_surface_id];
    Surface& dst_surface = slot_surfaces[dst_surface_id];

    if (src_surface.is_tiled != dst_surface.is_tiled) {
        std::swap(src_rect.top, src_rect.bottom);
    }
    if (config.flip_vertically) {
''', r'''
    Surface& src_surface = slot_surfaces[src_surface_id];
    Surface& dst_surface = slot_surfaces[dst_surface_id];
#ifdef EMUSUNI_PORT
    { // (01.21, 3ds-black-watch) a screen copy into it; noted when it writes one of a screen's buffers
        namespace BS = EmuSuNi::BlackSurfaces;
        BS::Of(dst_surface_id.index).copies++;
        const int screen = BS::Screen(config.GetPhysicalOutputAddress());
        if (screen >= 0) {
            std::scoped_lock lock{BS::mutex};
            BS::copy_src[screen] = BS::Make(src_surface_id.index, src_surface);
            BS::copy_dst[screen] = BS::Make(dst_surface_id.index, dst_surface);
        }
    }
#endif

    if (src_surface.is_tiled != dst_surface.is_tiled) {
        std::swap(src_rect.top, src_rect.bottom);
    }
    if (config.flip_vertically) {
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
#include "common/emusuni_black_counts.h" // (01.18)
''', r'''
#include "common/emusuni_black_counts.h" // (01.18)
#include "common/emusuni_black_surfaces.h" // (01.21)
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
    Surface& src_surface = res_cache.GetSurface(src_surface_id);
    const u32 scaled_width = src_surface.GetScaledWidth();
''', r'''
    Surface& src_surface = res_cache.GetSurface(src_surface_id);
#ifdef EMUSUNI_PORT
    { // (01.21, 3ds-black-watch) the surface this screen shows (the right eye's buffers are none of the two)
        namespace BS = EmuSuNi::BlackSurfaces;
        const int screen = BS::Screen(framebuffer_addr);
        if (screen >= 0) {
            std::scoped_lock lock{BS::mutex};
            BS::shown[screen] = BS::Make(src_surface_id.index, src_surface);
        }
    }
#endif
    const u32 scaled_width = src_surface.GetScaledWidth();
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
#include "common/emusuni_black_counts.h" // (01.18)
namespace EmuSuNiBlack {
''', r'''
#include "common/emusuni_black_counts.h" // (01.18)
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "common/emusuni_black_surfaces.h" // (01.21)
#include "common/emusuni_jit.h"
namespace EmuSuNiBlack {
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
             d(C::BottomInto1), d(C::BottomSwaps), d(C::Elsewhere));
}
''', r'''
             d(C::BottomInto1), d(C::BottomSwaps), d(C::Elsewhere));
    // (01.21) The surfaces (common/emusuni_black_surfaces.h): slot, address, size, scale (x), pixel format (f), linear
    // or tiled (L/T), its age in top-screen swaps, and draws into it (d), screen copies into it (c), loads from memory
    // (l), copies from other surfaces (o) since it was made.
    namespace S = EmuSuNi::BlackSurfaces;
    S::Note shown[2], src[2], dst[2], drawn;
    {
        std::scoped_lock lock{S::mutex};
        for (int s = 0; s < 2; s++) {
            shown[s] = S::shown[s];
            src[s] = S::copy_src[s];
            dst[s] = S::copy_dst[s];
        }
        drawn = S::drawn;
    }
    const u64 swaps = C::values[C::TopSwaps].load(std::memory_order_relaxed);
    const auto say = [swaps](const S::Note& n) -> std::string {
        if (n.id == 0xFFFFFFFFu) {
            return "none";
        }
        const S::Writes& w = S::Of(n.id);
        return fmt::format("#{} {:08X} {}x{} x{} f{} {} age {} d{} c{} l{} o{}", n.id, n.addr, n.width, n.height,
                           n.scale, n.format, n.tiled ? "T" : "L", swaps - w.made_at.load(), w.draws.load(),
                           w.copies.load(), w.loads.load(), w.others.load());
    };
    for (int s = 0; s < 2; s++) {
        LOG_INFO(Render_Vulkan, "EmuSuNi: 3ds-black-watch: {}: the {} screen shows {}; its last copy from {} into {}",
                 until, s == 0 ? "top" : "bottom", say(shown[s]), say(src[s]), say(dst[s]));
    }
    LOG_INFO(Render_Vulkan, "EmuSuNi: 3ds-black-watch: {}: the last draws into {}", until, say(drawn));
}
''')
# --- 01.21, also: two captures in a black start, once a renderer, 8 s into the black (3ds-black-watch's line then):
# the core's own picture as the app gets it (points of a grid 120 wide) and what the game drew, the top screen's last
# copy source (60 wide, as the 3DS keeps it: turned), saved as small PPM pictures (/data/EmuSuNi/
# 3ds-black-<title>-picture.ppm and -drawn.ppm, 15-36 KiB; EMUSUNI_3DS_CAPTURE_DIR on the PC), each with a line saying
# how many of its points are black. A black picture before the app said by the black lines, now in their words.
# Nothing of it while the picture isn't black.
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp', r'''
bool RasterizerVulkan::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    return res_cache.AccelerateDisplayTransfer(config);
}
''', r'''
bool RasterizerVulkan::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
#ifdef EMUSUNI_PORT
    const bool emusuni_done = res_cache.AccelerateDisplayTransfer(config);
    namespace BS = EmuSuNi::BlackSurfaces;
    if (emusuni_done && BS::capture_wanted.load(std::memory_order_acquire) &&
        BS::Screen(config.GetPhysicalOutputAddress()) == 0) {
        // (01.21, 3ds-black-watch) once in a black start: points of what the game drew, this top-screen copy's
        // source (RGBA8: 4 bytes a point), into the renderer's buffer; read back there once the GPU is done.
        BS::capture_wanted.store(false, std::memory_order_relaxed);
        BS::Note note;
        {
            std::scoped_lock lock{BS::mutex};
            note = BS::copy_src[0];
        }
        Surface& surface = res_cache.GetSurface(VideoCore::SurfaceId{note.id});
        if (BS::capture_buffer != nullptr && surface.pixel_format == VideoCore::PixelFormat::RGBA8) {
            const u32 w = surface.GetScaledWidth();
            const u32 h = surface.GetScaledHeight();
            const u32 columns = std::min<u32>(BS::kDrawnColumns, w);
            const u32 rows = std::clamp<u32>(columns * h / std::max<u32>(w, 1), 1, BS::kDrawnRowsMax);
            const vk::Buffer buffer{static_cast<VkBuffer>(BS::capture_buffer)};
            renderpass_cache.EndRendering();
            scheduler.Record([image = surface.Image(), buffer, w, h, columns, rows](vk::CommandBuffer cmdbuf) {
                std::vector<vk::BufferImageCopy> copies;
                copies.reserve(std::size_t{columns} * rows);
                for (u32 y = 0; y < rows; y++) {
                    for (u32 x = 0; x < columns; x++) {
                        copies.push_back(vk::BufferImageCopy{
                            .bufferOffset = (std::size_t{y} * columns + x) * 4,
                            .bufferRowLength = 0,
                            .bufferImageHeight = 0,
                            .imageSubresource =
                                {
                                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                                    .mipLevel = 0,
                                    .baseArrayLayer = 0,
                                    .layerCount = 1,
                                },
                            .imageOffset = {static_cast<s32>(w * (2 * x + 1) / (2 * columns)),
                                            static_cast<s32>(h * (2 * y + 1) / (2 * rows)), 0},
                            .imageExtent = {1, 1, 1},
                        });
                    }
                }
                const vk::MemoryBarrier to_copy = {
                    .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                };
                const vk::MemoryBarrier to_host = {
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eHostRead,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                                       vk::DependencyFlags{}, to_copy, {}, {});
                cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eGeneral, buffer, copies);
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                       vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
                                       vk::DependencyFlags{}, to_host, {}, {});
            });
            BS::capture_of = note;
            BS::capture_of.width = w;
            BS::capture_of.height = h;
            BS::capture_columns = columns;
            BS::capture_rows = rows;
            BS::capture_tick = scheduler.CurrentTick();
            BS::capture_recorded.store(true, std::memory_order_release);
        } else {
            LOG_INFO(Render_Vulkan,
                     "EmuSuNi: 3ds-black-watch: the top screen's last copy source #{} (format {}) not captured: "
                     "RGBA8 only",
                     note.id, note.format);
        }
    }
    return emusuni_done;
#else
    return res_cache.AccelerateDisplayTransfer(config);
#endif
}
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
std::atomic<u64> black_ms{0};
''', r'''
std::atomic<u64> black_ms{0};
// (01.21) The captures in a black start, once a renderer: the core's picture (points of a grid PICTURE_COLUMNS wide)
// and what the game drew (EmuSuNi::BlackSurfaces), saved as PPM pictures.
constexpr u32 PICTURE_COLUMNS = 120, PICTURE_ROWS_MAX = 80;
VkBuffer picture_buffer = VK_NULL_HANDLE, drawn_buffer = VK_NULL_HANDLE;
VmaAllocation picture_allocation = nullptr, drawn_allocation = nullptr;
const u8* picture_mapped = nullptr;
const u8* drawn_mapped = nullptr;
bool captured = false;
bool picture_pending = false;
u64 picture_tick = 0;
u32 picture_columns = 0, picture_rows = 0, picture_w = 0, picture_h = 0;
// Where a capture goes: /data/EmuSuNi/3ds-black-<title>-<what>.ppm; on the PC EMUSUNI_3DS_CAPTURE_DIR, else nowhere.
std::string CapturePath(const char* what) {
#if defined(__ORBIS__)
    return fmt::format("/data/EmuSuNi/3ds-black-{:016X}-{}.ppm", EmuSuNi::JitTitle(), what);
#else
    const char* dir = std::getenv("EMUSUNI_3DS_CAPTURE_DIR");
    return dir != nullptr ? fmt::format("{}/3ds-black-{:016X}-{}.ppm", dir, EmuSuNi::JitTitle(), what) : "";
#endif
}
// A capture's points (4 bytes each) saved as a PPM picture (P6) at `path` (none: only counted); its black points.
u32 SavePpm(const std::string& path, const u8* points, u32 columns, u32 rows) {
    u32 black = 0;
    std::vector<u8> rgb(std::size_t{columns} * rows * 3);
    for (std::size_t p = 0; p < std::size_t{columns} * rows; p++) {
        const u8* px = points + p * 4;
        black += px[0] <= 2 && px[1] <= 2 && px[2] <= 2 ? 1 : 0;
        rgb[p * 3] = px[0];
        rgb[p * 3 + 1] = px[1];
        rgb[p * 3 + 2] = px[2];
    }
    if (!path.empty()) {
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", columns, rows);
            std::fwrite(rgb.data(), 1, rgb.size(), file);
            std::fclose(file);
        }
    }
    return black;
}
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
            } else {
                B::mapped = static_cast<const u8*>(got.pMappedData);
            }
''', r'''
            } else {
                B::mapped = static_cast<const u8*>(got.pMappedData);
            }
            // (01.21) The captures' buffers: the core's picture and what the game drew.
            const auto make = [&](VkDeviceSize size, VkBuffer& made, VmaAllocation& made_allocation,
                                  const u8*& made_mapped) {
                info.size = size;
                VmaAllocationInfo made_info{};
                made = VK_NULL_HANDLE;
                made_allocation = nullptr;
                made_mapped = nullptr;
                if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &made, &made_allocation, &made_info) ==
                    VK_SUCCESS) {
                    made_mapped = static_cast<const u8*>(made_info.pMappedData);
                }
            };
            B::captured = B::picture_pending = false;
            EmuSuNi::BlackSurfaces::capture_wanted = false;
            EmuSuNi::BlackSurfaces::capture_recorded = false;
            if (B::on) {
                make(VkDeviceSize{B::PICTURE_COLUMNS} * B::PICTURE_ROWS_MAX * 4, B::picture_buffer,
                     B::picture_allocation, B::picture_mapped);
                make(VkDeviceSize{EmuSuNi::BlackSurfaces::kDrawnColumns} * EmuSuNi::BlackSurfaces::kDrawnRowsMax * 4,
                     B::drawn_buffer, B::drawn_allocation, B::drawn_mapped);
            }
            EmuSuNi::BlackSurfaces::capture_buffer =
                B::drawn_mapped != nullptr ? static_cast<void*>(B::drawn_buffer) : nullptr;
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
        EmuSuNiBlack::mapped = nullptr;
    }
    EmuSuNiBlack::on = false;
''', r'''
        EmuSuNiBlack::mapped = nullptr;
    }
    // (01.21) The captures' buffers.
    EmuSuNi::BlackSurfaces::capture_buffer = nullptr;
    EmuSuNi::BlackSurfaces::capture_wanted = false;
    EmuSuNi::BlackSurfaces::capture_recorded = false;
    for (const auto& [made, made_allocation] :
         {std::pair{&EmuSuNiBlack::picture_buffer, &EmuSuNiBlack::picture_allocation},
          std::pair{&EmuSuNiBlack::drawn_buffer, &EmuSuNiBlack::drawn_allocation}}) {
        if (*made != VK_NULL_HANDLE) {
            vmaDestroyBuffer(instance.GetAllocator(), *made, *made_allocation);
            *made = VK_NULL_HANDLE;
            *made_allocation = nullptr;
        }
    }
    EmuSuNiBlack::picture_mapped = EmuSuNiBlack::drawn_mapped = nullptr;
    EmuSuNiBlack::on = false;
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
    namespace B = EmuSuNiBlack;
    B::pictures++;
''', r'''
    namespace B = EmuSuNiBlack;
    B::pictures++;
    // (01.21) The captures, once the GPU is done with them: saved and said.
    if (B::picture_pending && scheduler.IsFree(B::picture_tick)) {
        B::picture_pending = false;
        vmaInvalidateAllocation(instance.GetAllocator(), B::picture_allocation, 0, VK_WHOLE_SIZE);
        const std::string path = B::CapturePath("picture");
        const u32 black = B::SavePpm(path, B::picture_mapped, B::picture_columns, B::picture_rows);
        LOG_INFO(Render_Vulkan,
                 "EmuSuNi: 3ds-black-watch: the core's own picture ({}x{}, as the app gets it) captured as {}x{} "
                 "points, {} of them black{}{}",
                 B::picture_w, B::picture_h, B::picture_columns, B::picture_rows, black, path.empty() ? "" : ": ",
                 path);
    }
    if (EmuSuNi::BlackSurfaces::capture_recorded.load(std::memory_order_acquire) &&
        scheduler.IsFree(EmuSuNi::BlackSurfaces::capture_tick)) {
        namespace BS = EmuSuNi::BlackSurfaces;
        BS::capture_recorded.store(false, std::memory_order_relaxed);
        vmaInvalidateAllocation(instance.GetAllocator(), B::drawn_allocation, 0, VK_WHOLE_SIZE);
        const std::string path = B::CapturePath("drawn");
        const u32 black = B::SavePpm(path, B::drawn_mapped, BS::capture_columns, BS::capture_rows);
        LOG_INFO(Render_Vulkan,
                 "EmuSuNi: 3ds-black-watch: what the game drew, the top screen's last copy source #{} ({:08X}, {}x{} "
                 "scaled, turned as the 3DS keeps it) captured as {}x{} points, {} of them black{}{}",
                 BS::capture_of.id, BS::capture_of.addr, BS::capture_of.width, BS::capture_of.height,
                 BS::capture_columns, BS::capture_rows, black, path.empty() ? "" : ": ", path);
    }
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp', r'''
                            "drawing ({} draws in the last {:.1f} s) (3ds-black-watch; emusuni_3ds_black_ms for "
                            "the app): of {} points, {} black, {} with alpha 0",
                            B::black_s, since, new_draws, dt, B::pending_points, black, clear);
                B::LogScreens("since it started");
                B::LogCounts("since it started"); // (01.18)
            }
''', r'''
                            "drawing ({} draws in the last {:.1f} s) (3ds-black-watch, the core's own picture "
                            "before the app): of {} points, {} black, {} with alpha 0",
                            B::black_s, since, new_draws, dt, B::pending_points, black, clear);
                B::LogScreens("since it started");
                B::LogCounts("since it started"); // (01.18)
                if (!B::captured && B::black_s >= 8.0) { // (01.21) the captures, once a renderer
                    B::captured = true;
                    if (B::picture_mapped != nullptr) {
                        const u32 w = frame->width;
                        const u32 h = frame->height;
                        const u32 columns = std::min<u32>(B::PICTURE_COLUMNS, w);
                        const u32 rows = std::clamp<u32>(columns * h / std::max<u32>(w, 1), 1, B::PICTURE_ROWS_MAX);
                        scheduler.Record([image = frame->image, buffer = vk::Buffer{B::picture_buffer}, w, h,
                                          columns, rows](vk::CommandBuffer cmdbuf) {
                            std::vector<vk::BufferImageCopy> copies;
                            copies.reserve(std::size_t{columns} * rows);
                            for (u32 y = 0; y < rows; y++) {
                                for (u32 x = 0; x < columns; x++) {
                                    copies.push_back(vk::BufferImageCopy{
                                        .bufferOffset = (std::size_t{y} * columns + x) * 4,
                                        .bufferRowLength = 0,
                                        .bufferImageHeight = 0,
                                        .imageSubresource =
                                            {
                                                .aspectMask = vk::ImageAspectFlagBits::eColor,
                                                .mipLevel = 0,
                                                .baseArrayLayer = 0,
                                                .layerCount = 1,
                                            },
                                        .imageOffset = {static_cast<s32>(w * (2 * x + 1) / (2 * columns)),
                                                        static_cast<s32>(h * (2 * y + 1) / (2 * rows)), 0},
                                        .imageExtent = {1, 1, 1},
                                    });
                                }
                            }
                            const vk::ImageSubresourceRange range = {
                                .aspectMask = vk::ImageAspectFlagBits::eColor,
                                .baseMipLevel = 0,
                                .levelCount = 1,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                            };
                            const vk::ImageMemoryBarrier to_copy = {
                                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = image,
                                .subresourceRange = range,
                            };
                            const vk::ImageMemoryBarrier back = {
                                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                                .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eMemoryRead,
                                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = image,
                                .subresourceRange = range,
                            };
                            const vk::MemoryBarrier to_host = {
                                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                .dstAccessMask = vk::AccessFlagBits::eHostRead,
                            };
                            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                                   vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{}, {}, {},
                                                   to_copy);
                            cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, copies);
                            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                                   vk::PipelineStageFlagBits::eAllCommands |
                                                       vk::PipelineStageFlagBits::eHost,
                                                   vk::DependencyFlags{}, to_host, {}, back);
                        });
                        B::picture_columns = columns;
                        B::picture_rows = rows;
                        B::picture_w = w;
                        B::picture_h = h;
                        B::picture_tick = scheduler.CurrentTick();
                        B::picture_pending = true;
                    }
                    if (EmuSuNi::BlackSurfaces::capture_buffer != nullptr) {
                        EmuSuNi::BlackSurfaces::capture_wanted.store(true, std::memory_order_release);
                    }
                }
            }
''')

# --- 01.22: 3ds-black-uniforms. The PS4 tests of 01.21 (DKC, 2026-11-11, twice: on a session's first renderer and on
# one after a reset): a black start with the game drawing came back at once, and stayed after the file was gone, with
# 01.02's 3ds-uniforms-fresh (every draw's three uniform blocks uploaded again into new room); with
# 3ds-vertex-reuse-off it stayed black. That path uploads the CPU's same values: what the draws read was a copy of a
# block that nothing wrote again elsewhere (the VS and FS blocks are uploaded only when they change, and again at the
# buffer's start each time it goes round). So:
# - 3ds-black-watch, when a reading in its first minute is black with the game drawing, asks for that path for the
#   next two readings' pictures (each black reading moves that on), and says when the picture came back. The word off
#   in /data/EmuSuNi/3ds-black-uniforms (EMUSUNI_3DS_BLACK_UNIFORMS=0 on the PC): never.
# - Before a refresh for a new black (none on when it was asked for; at most 4 a renderer: nearly every game's own
#   black at its start asks for one), the rasterizer says where each block was bound, whether an upload of it was
#   pending, and whether its bytes there were the CPU's: in memory (read by the CPU) and as the GPU copies them
#   (vkCmdCopyBuffer, through its caches). A block with no upload pending: memory not the CPU's, something wrote over
#   the buffer; memory right but the GPU's copy not, the GPU's caches (the PS4's driver); both right, the descriptors
#   or their offsets.
# - Upstream's |= in SyncDrawUniforms (its 1edf59e, 2026-10-25; this base is of 10-19): the old = dropped a pending
#   upload of the VS block (the clip plane and the flip), the renderer's first one too, when the flip stayed and the
#   clip registers weren't written; the draws then read 32 bytes of the FS block as the VS block until the buffer went
#   round. The log says the first time a renderer would have dropped one.
# - PC test: EMUSUNI_3DS_TEST_SPOIL_UNIFORMS spoils the VS block in the buffer (clip plane 1 on, at -w: every vertex
#   cut), as in the PS4's black starts: 0, after each upload of it until the first refresh; N, from frame N (where the
#   draws find it, then after each upload) until the next refresh.
addfile('src/common/emusuni_black_uniforms.h', r'''// EmuSuNi (01.22): 3ds-black-uniforms (scripts/build-3ds-core.sh).
// The uniform blocks uploaded fresh at every draw while the picture is black with the game drawing, asked for by
// 3ds-black-watch (renderer_vulkan.cpp) and done by the rasterizer (vk_rasterizer.cpp).
#pragma once

#include <atomic>
#include "common/common_types.h"

namespace EmuSuNi::BlackUniforms {

// Pictures left with the blocks uploaded fresh at every draw: set by 3ds-black-watch, counted down at each frame's
// end (RasterizerVulkan::TickFrame).
inline std::atomic<u32> fresh_left{0};
// The readings that asked for it since the renderer started.
inline std::atomic<u32> asked{0};
// Its diagnosis: wanted at the next fresh upload (a new black: no refresh on when it was asked for), and how many a
// renderer has had (at most PROBES).
inline std::atomic<bool> probe_wanted{false};
inline std::atomic<u32> probes{0};
constexpr u32 PROBES = 4;
// The file /data/EmuSuNi/3ds-black-uniforms (EMUSUNI_3DS_BLACK_UNIFORMS on the PC) without the word off.
inline std::atomic<bool> on{true};

// The renderer's start.
inline void Reset() {
    fresh_left.store(0, std::memory_order_relaxed);
    asked.store(0, std::memory_order_relaxed);
    probe_wanted.store(false, std::memory_order_relaxed);
    probes.store(0, std::memory_order_relaxed);
}

// A reading black with the game drawing: the blocks fresh for the next `pictures`, and the diagnosis wanted if no
// refresh was on. True for the renderer's first.
inline bool Ask(u32 pictures) {
    if (fresh_left.exchange(pictures, std::memory_order_relaxed) == 0 &&
        probes.load(std::memory_order_relaxed) < PROBES) {
        probes.fetch_add(1, std::memory_order_relaxed);
        probe_wanted.store(true, std::memory_order_release);
    }
    return asked.fetch_add(1, std::memory_order_relaxed) == 0;
}

// At each frame's end: one picture less.
inline void Tick() {
    u32 left = fresh_left.load(std::memory_order_relaxed);
    while (left != 0 && !fresh_left.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
    }
}

} // namespace EmuSuNi::BlackUniforms
''')
patch('src/video_core/rasterizer_accelerated.h',
      '    bool vs_data_dirty = true;\n    bool fs_data_dirty = true;\n',
      '    bool vs_data_dirty = true;\n    bool fs_data_dirty = true;\n'
      '#ifdef EMUSUNI_PORT\n'
      '    u64 emusuni_vs_kept = 0;    // (01.22) pending VS-block uploads upstream\'s old line would have dropped\n'
      '    u64 emusuni_vs_uploads = 0; // (01.22) the VS block\'s uploads (RasterizerVulkan::UploadUniforms)\n'
      '#endif\n')
patch('src/video_core/rasterizer_accelerated.cpp',
      '#include "video_core/rasterizer_accelerated.h"\n\nnamespace VideoCore {\n',
      '#include "video_core/rasterizer_accelerated.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include "common/logging/log.h" // (01.22)\n'
      '#endif\n'
      '\nnamespace VideoCore {\n')
patch('src/video_core/rasterizer_accelerated.cpp',
      '    const bool prev_flipped = std::exchange(vs_data.flip_viewport, is_flipped);\n'
      '    vs_data_dirty = is_flipped != prev_flipped;\n',
      r'''    const bool prev_flipped = std::exchange(vs_data.flip_viewport, is_flipped);
#ifdef EMUSUNI_PORT
    // (01.22) Upstream's |= (its 1edf59e): the old = dropped a pending upload of the VS block, the renderer's first
    // one too, when the flip stayed and the clip registers weren't written (the draws then read the VS block where it
    // never was, until the uniform buffer went round). Said the first time a renderer would have dropped one.
    if (vs_data_dirty && is_flipped == prev_flipped && !dirty.CheckClipping() && emusuni_vs_kept++ == 0) {
        LOG_INFO(Render,
                 "EmuSuNi: the VS uniform block's pending upload ({}) kept: upstream's old line dropped it here "
                 "(01.22: its |=)",
                 emusuni_vs_uploads == 0 ? "its first, from the renderer's start" : "after earlier ones");
    }
    vs_data_dirty |= is_flipped != prev_flipped;
#else
    vs_data_dirty = is_flipped != prev_flipped;
#endif
''')
patch('src/video_core/renderer_vulkan/vk_stream_buffer.h',
      '    u64 emusuni_wraps = 0; // (00.95) times the buffer started over from its beginning\n',
      r'''    u64 emusuni_wraps = 0; // (00.95) times the buffer started over from its beginning
    // (01.22) For 3ds-black-uniforms' diagnosis: the mapped memory, its size, and where the next copy goes.
    u8* EmuSuNiMapped() const {
        return mapped;
    }
    u64 EmuSuNiSize() const {
        return stream_buffer_size;
    }
    u32 EmuSuNiOffset() const {
        return offset;
    }
''')
patch('src/video_core/renderer_vulkan/vk_pipeline_cache.h',
      '        bound_descriptor_sets[static_cast<u32>(type)] = descriptor_set;\n    }\n#endif\n',
      '        bound_descriptor_sets[static_cast<u32>(type)] = descriptor_set;\n    }\n'
      '    /// EmuSuNi (01.22): the dynamic offset of the uniform buffer at binding (3ds-black-uniforms\' diagnosis).\n'
      '    u32 EmuSuNiRange(u8 binding) const {\n'
      '        return offsets[binding];\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.h',
      '#include "video_core/renderer_vulkan/vk_texture_runtime.h"\n',
      '#include "video_core/renderer_vulkan/vk_texture_runtime.h"\n'
      '#ifdef EMUSUNI_PORT\n'
      '#include <atomic> // (01.22)\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.h',
      '    EmuSuNiSetCache emusuni_texture_set;\n    EmuSuNiSetCache emusuni_utility_set;\n#endif\n',
      r'''    EmuSuNiSetCache emusuni_texture_set;
    EmuSuNiSetCache emusuni_utility_set;
    // (01.22) 3ds-black-uniforms: its diagnosis, before a refresh for a new black (EmuSuNiProbeUniforms records it,
    // EmuSuNiProbeSay says it at a frame's end once the GPU is done), and the VS block spoiled for its PC test
    // (EMUSUNI_3DS_TEST_SPOIL_UNIFORMS: 0, after each upload until the first refresh; N, from frame N until a
    // refresh).
    struct EmuSuNiProbe {
        static constexpr std::array<u32, 3> AT{0, 2048, 2304}; // each block's place below: PICA, VS, FS
        std::array<u8, 4096> cpu{};                            // the CPU's copies
        std::array<u8, 4096> memory{};                         // the bytes where they were bound, read by the CPU
        std::array<u32, 3> offset{};
        std::array<u32, 3> size{};
        std::array<bool, 3> pending{};
        std::array<bool, 3> inside{};
        u64 wraps = 0;
        u64 buffer_size = 0;
        u32 at = 0;
        u32 number = 0; // the renderer's diagnoses so far
        u64 tick = 0;
        bool copied = false;
        std::atomic<bool> recorded{false};
        VkBuffer buffer = VK_NULL_HANDLE; // the GPU's copies (host-visible)
        void* allocation = nullptr;       // its VmaAllocation
        const u8* mapped = nullptr;
    };
    void EmuSuNiProbeUniforms();
    void EmuSuNiProbeSay();
    void EmuSuNiSpoilVs(u8* where, bool once);
    EmuSuNiProbe emusuni_probe;
    bool emusuni_spoil = false;
    u64 emusuni_spoil_frame = 0;
    u64 emusuni_frames = 0;
    u64 emusuni_spoiled = 0;
#endif
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '#include "common/emusuni_black_surfaces.h" // (01.21)\n#endif\n',
      '#include "common/emusuni_black_surfaces.h" // (01.21)\n'
      '#include "common/emusuni_black_uniforms.h" // (01.22)\n'
      '#include <vk_mem_alloc.h>                  // (01.22: 3ds-black-uniforms\' diagnosis buffer)\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'RasterizerVulkan::~RasterizerVulkan() = default;\n',
      r'''#ifdef EMUSUNI_PORT
RasterizerVulkan::~RasterizerVulkan() {
    // (01.22) 3ds-black-uniforms' diagnosis buffer (the renderer has waited for the GPU to idle).
    if (emusuni_probe.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(instance.GetAllocator(), emusuni_probe.buffer,
                         static_cast<VmaAllocation>(emusuni_probe.allocation));
    }
}
#else
RasterizerVulkan::~RasterizerVulkan() = default;
#endif
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game\'s start\n',
      '    EmuSuNiReadGpuDiag(true); // (01.02) the GPU sync diagnosis files, at the game\'s start\n'
      '#ifndef __ORBIS__\n'
      '    // (01.22) 3ds-black-uniforms\' PC test: the VS block spoiled in the buffer: 0, after each upload of it\n'
      '    // until the first refresh; N, from frame N (where the draws find it, then each upload) until a refresh.\n'
      '    if (const char* emusuni_spoil_env = std::getenv("EMUSUNI_3DS_TEST_SPOIL_UNIFORMS")) {\n'
      '        emusuni_spoil_frame = std::strtoull(emusuni_spoil_env, nullptr, 10);\n'
      '        emusuni_spoil = emusuni_spoil_frame == 0;\n'
      '    }\n'
      '#endif\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n'
      '    // (01.02) The GPU sync diagnosis files read again every 2 s; 3ds-gpu-sync-frame: the GPU idle at the end\n',
      '    pipeline_cache.EmuSuNiSaveNowAndThen();\n'
      '    // (01.22) 3ds-black-uniforms: one picture less of its refresh, and its diagnosis said once the GPU is\n'
      '    // done.\n'
      '    ::EmuSuNi::BlackUniforms::Tick();\n'
      '    EmuSuNiProbeSay();\n'
      '#ifndef __ORBIS__\n'
      '    if (emusuni_spoil_frame != 0 && ++emusuni_frames == emusuni_spoil_frame) { // its PC test\n'
      '        emusuni_spoil = true; // (after each upload of it, from now until the next refresh)\n'
      '        EmuSuNiSpoilVs(uniform_buffer.EmuSuNiMapped() + pipeline_cache.EmuSuNiRange(1), true);\n'
      '    }\n'
      '#endif\n'
      '    // (01.02) The GPU sync diagnosis files read again every 2 s; 3ds-gpu-sync-frame: the GPU idle at the end\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      'void RasterizerVulkan::UploadUniforms(bool accelerate_draw) {\n'
      '#ifdef EMUSUNI_PORT\n'
      '    if ((emusuni_gpu_diag & EMUSUNI_UNIFORMS_FRESH) != 0) {\n',
      r'''#ifdef EMUSUNI_PORT
// (01.22) 3ds-black-uniforms' diagnosis, before a refresh for a new black (at most BlackUniforms::PROBES a renderer):
// each uniform block where the draws find it bound, whether an upload of it is pending (then its bytes there may
// differ), and those bytes against the CPU's copy: in memory (read here) and as the GPU copies them through its
// caches (recorded here, compared by EmuSuNiProbeSay at a frame's end once the GPU is done).
void RasterizerVulkan::EmuSuNiProbeUniforms() {
    auto& p = emusuni_probe;
    if (p.recorded.load(std::memory_order_acquire)) {
        return; // the one before not said yet
    }
    p.number++;
    VSPicaUniformData pica_now;
    pica_now.SetFromRegs(pica.vs_setup);
    std::memcpy(p.cpu.data() + p.AT[0], &pica_now, sizeof(pica_now));
    std::memcpy(p.cpu.data() + p.AT[1], &vs_data, sizeof(vs_data));
    std::memcpy(p.cpu.data() + p.AT[2], &fs_data, sizeof(fs_data));
    p.size = {static_cast<u32>(sizeof(pica_now)), static_cast<u32>(sizeof(vs_data)),
              static_cast<u32>(sizeof(fs_data))};
    p.pending = {pica.vs_setup.uniforms_dirty, vs_data_dirty, fs_data_dirty};
    p.wraps = uniform_buffer.emusuni_wraps;
    p.buffer_size = uniform_buffer.EmuSuNiSize();
    p.at = uniform_buffer.EmuSuNiOffset();
    const u8* base = uniform_buffer.EmuSuNiMapped();
    std::array<vk::BufferCopy, 3> copies{};
    u32 count = 0;
    for (u32 b = 0; b < 3; b++) {
        p.offset[b] = pipeline_cache.EmuSuNiRange(static_cast<u8>(b));
        p.inside[b] = base != nullptr && u64{p.offset[b]} + p.size[b] <= p.buffer_size;
        if (p.inside[b]) {
            std::memcpy(p.memory.data() + p.AT[b], base + p.offset[b], p.size[b]);
            copies[count++] = vk::BufferCopy{.srcOffset = p.offset[b], .dstOffset = p.AT[b], .size = p.size[b]};
        }
    }
    if (p.buffer == VK_NULL_HANDLE) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = p.cpu.size();
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo alloc{};
        alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo got{};
        if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &p.buffer, &allocation, &got) == VK_SUCCESS &&
            got.pMappedData != nullptr) {
            p.allocation = allocation;
            p.mapped = static_cast<const u8*>(got.pMappedData);
        } else if (p.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(instance.GetAllocator(), p.buffer, allocation);
            p.buffer = VK_NULL_HANDLE;
        }
    }
    p.copied = p.mapped != nullptr && count != 0;
    if (p.copied) {
        renderpass_cache.EndRendering();
        scheduler.Record(
            [src = uniform_buffer.Handle(), dst = vk::Buffer{p.buffer}, copies, count](vk::CommandBuffer cmdbuf) {
                cmdbuf.copyBuffer(src, dst, count, copies.data());
                const vk::MemoryBarrier to_host = {
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eHostRead,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                                       vk::DependencyFlags{}, to_host, {}, {});
            });
        p.tick = scheduler.CurrentTick();
    }
    p.recorded.store(true, std::memory_order_release);
}

// (01.22) 3ds-black-uniforms' diagnosis said, at a frame's end once the GPU has made its copy.
void RasterizerVulkan::EmuSuNiProbeSay() {
    auto& p = emusuni_probe;
    if (!p.recorded.load(std::memory_order_acquire) || (p.copied && !scheduler.IsFree(p.tick))) {
        return;
    }
    p.recorded.store(false, std::memory_order_relaxed);
    std::array<u8, 4096> gpu{};
    if (p.copied) {
        vmaInvalidateAllocation(instance.GetAllocator(), static_cast<VmaAllocation>(p.allocation), 0,
                                VK_WHOLE_SIZE);
        std::memcpy(gpu.data(), p.mapped, gpu.size());
    }
    // The padding the shaders never read (the PICA block's bytes 4-15, after its bools; the VS block's 8-15, before
    // its clip plane) holds whatever the uploads' locals held: left out.
    for (u8* bytes : {p.cpu.data(), p.memory.data(), gpu.data()}) {
        std::memset(bytes + p.AT[0] + 4, 0, 12);
        std::memset(bytes + p.AT[1] + 8, 0, 8);
    }
    LOG_WARNING(Render_Vulkan,
                "EmuSuNi: 3ds-black-uniforms: diagnosis {} of this renderer, the uniform blocks as the draws found "
                "them before this refresh (01.22): the buffer at {} of {} KiB, {} times round since its renderer "
                "started",
                p.number, p.at, p.buffer_size / 1024, p.wraps);
    static constexpr std::array<const char*, 3> names{"PICA (the vertex shaders' uniforms)",
                                                      "VS (the clip plane and the flip)", "FS (the fragments')"};
    const auto compare = [&p](u32 b, const u8* bytes) {
        u32 differ = 0;
        u32 first = 0;
        for (u32 i = 0; i < p.size[b]; i++) {
            if (bytes[p.AT[b] + i] != p.cpu[p.AT[b] + i] && differ++ == 0) {
                first = i;
            }
        }
        return differ == 0 ? std::string{"the CPU's"}
                           : fmt::format("{} bytes not the CPU's, the first at byte {} ({:02X}, the CPU's {:02X})",
                                         differ, first, bytes[p.AT[b] + first], p.cpu[p.AT[b] + first]);
    };
    for (u32 b = 0; b < 3; b++) {
        LOG_WARNING(Render_Vulkan,
                    "EmuSuNi: 3ds-black-uniforms: the {} block, {} bytes, bound at {}{}: in memory {}; as the GPU "
                    "copies it {}",
                    names[b], p.size[b], p.offset[b], p.pending[b] ? " (an upload of it pending: it may differ)" : "",
                    p.inside[b] ? compare(b, p.memory.data()) : std::string{"past the buffer's end"},
                    p.inside[b] && p.copied ? compare(b, gpu.data()) : std::string{"not copied"});
    }
}

// (01.22) 3ds-black-uniforms' PC test (EMUSUNI_3DS_TEST_SPOIL_UNIFORMS): the VS block's bytes in the buffer made to
// cut every vertex (clip plane 1 on, at -w), as if the GPU read a copy that isn't the CPU's (the PS4's black starts):
// after each upload of it until a refresh; `once`, where the draws find it (at the frame the test starts).
void RasterizerVulkan::EmuSuNiSpoilVs(u8* where, bool once) {
    auto spoiled = vs_data;
    spoiled.enable_clip1 = 1;
    spoiled.clip_coef = Common::MakeVec(0.0f, 0.0f, 0.0f, -1.0f);
    std::memcpy(where, &spoiled, sizeof(spoiled));
    if (emusuni_spoiled++ == 0) {
        LOG_WARNING(Render_Vulkan,
                    "EmuSuNi: TEST (EMUSUNI_3DS_TEST_SPOIL_UNIFORMS): the VS uniform block spoiled in the buffer {} "
                    "(every vertex cut by its clip plane)",
                    once ? fmt::format("where the draws find it and after each upload of it, from frame {} until "
                                       "3ds-black-uniforms' next refresh",
                                       emusuni_frames)
                         : std::string{"after each upload of it until 3ds-black-uniforms' first refresh"});
    }
}
#endif

void RasterizerVulkan::UploadUniforms(bool accelerate_draw) {
#ifdef EMUSUNI_PORT
    // (01.22) 3ds-black-uniforms: while 3ds-black-watch asks for it, the same as 3ds-uniforms-fresh; before a refresh
    // for a new black, the diagnosis of the blocks as the draws find them.
    const bool emusuni_black_fresh = ::EmuSuNi::BlackUniforms::fresh_left.load(std::memory_order_relaxed) != 0;
    if (emusuni_black_fresh) {
        if (::EmuSuNi::BlackUniforms::probe_wanted.exchange(false, std::memory_order_acquire)) {
            EmuSuNiProbeUniforms();
        }
        emusuni_spoil = false; // (its PC test: spoiled until the first refresh)
    }
    if ((emusuni_gpu_diag & EMUSUNI_UNIFORMS_FRESH) != 0 || emusuni_black_fresh) {
''')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '        std::memcpy(fresh, &vs_data, sizeof(vs_data));\n'
      '        pipeline_cache.UpdateRange(1, fresh_offset);\n',
      '        std::memcpy(fresh, &vs_data, sizeof(vs_data));\n'
      '        emusuni_vs_uploads++; // (01.22)\n'
      '        pipeline_cache.UpdateRange(1, fresh_offset);\n')
patch('src/video_core/renderer_vulkan/vk_rasterizer.cpp',
      '    if (vs_data_dirty || invalidate) {\n'
      '        std::memcpy(uniforms + used_bytes, &vs_data, sizeof(vs_data));\n'
      '        pipeline_cache.UpdateRange(1, offset + used_bytes);\n',
      '    if (vs_data_dirty || invalidate) {\n'
      '        std::memcpy(uniforms + used_bytes, &vs_data, sizeof(vs_data));\n'
      '#ifdef EMUSUNI_PORT\n'
      '        emusuni_vs_uploads++; // (01.22)\n'
      '        if (emusuni_spoil) {\n'
      '            EmuSuNiSpoilVs(uniforms + used_bytes, false); // (01.22) PC test: EMUSUNI_3DS_TEST_SPOIL_UNIFORMS\n'
      '        }\n'
      '#endif\n'
      '        pipeline_cache.UpdateRange(1, offset + used_bytes);\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '#include "common/emusuni_black_surfaces.h" // (01.21)\n',
      '#include "common/emusuni_black_surfaces.h" // (01.21)\n'
      '#include "common/emusuni_black_uniforms.h" // (01.22)\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '\ndouble next_log = 4.0;\n',
      '\ndouble next_log = 4.0;\n'
      'double uniforms_first_s = 0.0; // (01.22) 3ds-black-uniforms\' first refresh, since the renderer started\n')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '        B::on = EmuSuNi::Async::TestFile("3ds-black-watch", "EMUSUNI_3DS_BLACK_WATCH", 0) >= 0;\n',
      r'''        B::on = EmuSuNi::Async::TestFile("3ds-black-watch", "EMUSUNI_3DS_BLACK_WATCH", 0) >= 0;
        // (01.22) 3ds-black-uniforms: on unless the word off is in its file.
        ::EmuSuNi::BlackUniforms::on.store(
            EmuSuNi::Async::TestFile("3ds-black-uniforms", "EMUSUNI_3DS_BLACK_UNIFORMS", 0) >= 0,
            std::memory_order_relaxed);
        ::EmuSuNi::BlackUniforms::Reset();
        B::uniforms_first_s = 0.0;
        if (!::EmuSuNi::BlackUniforms::on.load(std::memory_order_relaxed)) {
            LOG_INFO(Render_Vulkan, "EmuSuNi: 3ds-black-uniforms off (the word off in its file): a black picture only "
                                    "watched");
        }
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '        B::draws_at = draws;\n        if (B::test_log) {\n',
      r'''        B::draws_at = draws;
        // (01.22) 3ds-black-uniforms: a reading black with the game drawing in the first minute: the uniform blocks
        // uploaded fresh at every draw until two readings on (each black reading moves that on).
        if (::EmuSuNi::BlackUniforms::on.load(std::memory_order_relaxed) && is_black && new_draws != 0 &&
            since < 60.0 && ::EmuSuNi::BlackUniforms::Ask(static_cast<u32>(2 * B::EVERY))) {
            B::uniforms_first_s = since;
            LOG_WARNING(Render_Vulkan,
                        "EmuSuNi: 3ds-black-uniforms: the picture black {:.1f} s after its renderer started with the "
                        "game drawing ({} draws in the last {:.1f} s): its three uniform blocks uploaded fresh at "
                        "every draw while it stays so (01.22; the word off in /data/EmuSuNi/3ds-black-uniforms: "
                        "never)",
                        since, new_draws, dt);
        }
        if (B::test_log) {
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '                     since, B::pictures, B::pending_points, B::EVERY, black, clear);\n'
      '            B::LogScreens("until then");\n',
      r'''                     since, B::pictures, B::pending_points, B::EVERY, black, clear);
            if (::EmuSuNi::BlackUniforms::asked.load(std::memory_order_relaxed) != 0) { // (01.22)
                LOG_INFO(Render_Vulkan,
                         "EmuSuNi: 3ds-black-uniforms: the picture shown {:.1f} s after its first refresh of the "
                         "uniform blocks ({} readings asked for it)",
                         since - B::uniforms_first_s, ::EmuSuNi::BlackUniforms::asked.load(std::memory_order_relaxed));
            }
            B::LogScreens("until then");
''')
patch('src/video_core/renderer_vulkan/renderer_vulkan.cpp',
      '                     "one before; now {} of {} points not black",\n'
      '                     B::minute_readings, B::minute_black, B::minute_changed, B::pending_points - black,\n'
      '                     B::pending_points);\n',
      '                     "one before; now {} of {} points not black; 3ds-black-uniforms asked {} times",\n'
      '                     B::minute_readings, B::minute_black, B::minute_changed, B::pending_points - black,\n'
      '                     B::pending_points, ::EmuSuNi::BlackUniforms::asked.load(std::memory_order_relaxed));\n')

written = 0
for (repo, path), text in patched.items():
    full = repo + '/' + path
    try:
        with open(full, encoding='utf-8') as f:
            same = f.read() == text
    except FileNotFoundError:
        same = False
    if not same:
        with open(full, 'w', encoding='utf-8') as f:
            f.write(text)
        written += 1
print("Azahar: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-3ds-core.sh: the Azahar core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f azahar_$f"; done
} > "$OUT/prefix_azahar.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_azahar.h.new" "$OUT/prefix_azahar.h"; then
  rm -f "$OUT/prefix_azahar.h.new"
else
  mv "$OUT/prefix_azahar.h.new" "$OUT/prefix_azahar.h"
fi

if [ "$HOST" != 1 ]; then
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
fi
: > "$LOG"

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): -O2 (2-7% faster than CMake's -O3
# in the 00.63 3DS speed tests), the PS4's CPU and the renamed API.
FLAGS_CORE="-O2 -DNDEBUG -march=btver2 -include $OUT/prefix_azahar.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a"
# CTR_CORE_SECTIONS=1: a section for each function and each variable, so that a link with --gc-sections (the
# app's) can drop what nothing reaches: the 3DS core's code and data are part of the app's image, which the PS4
# takes out of the memory the games use. Off, the flags (and the object) are what they were before this option;
# on, build in another <out dir> (the flags make another stamp).
[ "${CTR_CORE_SECTIONS:-0}" = 1 ] && FLAGS_CORE+=" -ffunction-sections -fdata-sections"
# PGO (profile-guided optimisation): scripts/3ds-core.profdata.xz, a profile of the PC build (CTR_CORE_HOST=1) on
# lavapipe with 01.13's code and the GPU time list on: Castlevania: Mirror of Fate (3000 frames from a saved state,
# its scene then walking), Monster Hunter 4 Ultimate (3000, Val Habar, a talk then walking), Luigi's Mansion: Dark
# Moon (2400, the Front Yard, walking), Donkey Kong Country Returns 3D (3000) and Super Mario 3D Land (2400) from
# their starts (00.69's profile had the starts of 3D Land, Hey! Pikmin, Ocarina of Time 3D and Driver Renegade 3D),
# used by both builds when it's there (CTR_CORE_PGO=off: without it). A profile older than the sources only
# optimises less (the functions that changed get none). A new one: CTR_CORE_HOST=1 CTR_CORE_PGO=generate
# build-3ds-core.sh <dir> (instrumented), the PC test frontend linked with -fprofile-instr-generate and run on the
# games with LLVM_PROFILE_FILE=<dir>/%p.profraw, then llvm-profdata-21 merge -o 3ds-core.profdata <dir>/*.profraw;
# xz -9 it into scripts/. (The profile holds function counters only, nothing of the games.)
PROFILE="$HERE/3ds-core.profdata.xz"
PGO="none"
case "${CTR_CORE_PGO:-use}" in
  generate)
    if [ "$HOST" != 1 ]; then
      echo "Azahar: CTR_CORE_PGO=generate is for the PC build (CTR_CORE_HOST=1)"
      exit 1
    fi
    FLAGS_CORE+=" -fprofile-instr-generate"
    PGO="generate" ;;
  off) ;;
  *)
    if [ -f "$PROFILE" ]; then
      mkdir -p "$OUT"
      xz -dc "$PROFILE" > "$OUT/3ds-core.profdata.new"
      if cmp -s "$OUT/3ds-core.profdata.new" "$OUT/3ds-core.profdata"; then
        rm -f "$OUT/3ds-core.profdata.new"
      else
        mv "$OUT/3ds-core.profdata.new" "$OUT/3ds-core.profdata"
      fi
      FLAGS_CORE+=" -fprofile-instr-use=$OUT/3ds-core.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
      PGO="profile $(sha256sum "$PROFILE" | cut -c1-16)"
    fi ;;
esac
OPTIONS=(
  -DENABLE_LIBRETRO=ON -DEMUSUNI_PORT=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DENABLE_LTO=OFF
  -DENABLE_OPENGL=OFF -DENABLE_VULKAN=ON -DENABLE_SOFTWARE_RENDERER=ON
  -DCITRA_WARNINGS_AS_ERRORS=OFF -DCITRA_USE_PRECOMPILED_HEADERS=ON -DENABLE_TESTS=OFF
  -DENABLE_QT=OFF -DENABLE_QT_TRANSLATION=OFF -DENABLE_SDL2=OFF -DENABLE_WEB_SERVICE=OFF
  -DENABLE_SCRIPTING=OFF -DENABLE_GDBSTUB=OFF -DENABLE_ROOM=OFF -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF
  -DENABLE_LIBUSB=OFF -DENABLE_DISCORD_RPC=OFF -DENABLE_MICROPROFILE=OFF
  # (zstd's dictionary builder wants qsort_r, which the PS4's libc hasn't; Azahar doesn't use it.)
  -DZSTD_BUILD_DICTBUILDER=OFF
  # No Nintendo keys compiled in (upstream's default: an encrypted blob for players without a dump).
  # As the other consoles' BIOS files, the player's own: sysdata/aes_keys.txt in the core's directory
  # (GodMode9 dumps it). Decrypted games run without; PS/APT encryption and amiibo need it (logged).
  -DENABLE_BUILTIN_KEYBLOB=OFF
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CORE"
)
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  }
else
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
# (The tree's GIT-COMMIT, checked above, not the commit setup.sh made locally: the same everywhere. Of
# this script, what makes the build: up to the checks, which a change to needn't compile everything
# again.)
WANT="$SRC | $TREE_COMMIT | host $HOST | pgo $PGO | ${OPTIONS[*]} | $(sed -n '1,/^# The checks\.$/p' "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Azahar: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "Azahar: configuring failed (see $LOG)"
    exit 1
  fi
  # The stamp says what the build directory was configured with, written now: a build cut short (WSL
  # has died under one on this PC) goes on from where it was next time, ninja knowing what is done.
  printf '%s' "$WANT" > "$STAMP"
fi
# (-k 0: every error at once, not only the first.)
if ! ninja -C "$BUILD" -j "$JOBS" -k 0 azahar_libretro >> "$LOG" 2>&1; then
  grep -E "error:" "$LOG" | sort | uniq -c | sort -rn | head -60
  echo "Azahar: build failed (see $LOG)"
  exit 1
fi

# One object: only the libretro API (renamed) stays global. Azahar's own libraries whole (the
# libretro glue, core, video, audio, network, common: Boost.Serialization's class registrations are
# static objects in them that nothing references); its dependencies (Boost's serialization and
# iostreams, Crypto++, dynarmic with mcl and Zydis, fmt, zstd, glslang, SPIRV-Tools, sirit, teakra,
# faad2, SoundTouch, enet, xxHash, lodepng, inih, the open-source system archives) only as far as
# referenced: linked whole they carried 4.6 MB of code and 6.4 MB of static memory nothing used
# (glslang's disassembler tables, teakra's test generator, Crypto++'s other ciphers...).
# Replaced only when it changed (the app isn't linked again for nothing). (01.06) And the three functions of the
# frame's handover the app may call: emusuni_3ds_image_serial, emusuni_3ds_wait_image and emusuni_3ds_image_worker_ms
# (video_core/renderer_vulkan/vk_scheduler.cpp).
HANDOVER_GLOBALS="emusuni_3ds_image_serial emusuni_3ds_wait_image emusuni_3ds_image_worker_ms"
# (01.16) And how long the picture has been black since the game's start (video_core/renderer_vulkan/
# renderer_vulkan.cpp, 3ds-black-watch), for the app's reset by itself.
HANDOVER_GLOBALS="$HANDOVER_GLOBALS emusuni_3ds_black_ms"
# (01.09) And the CIA installer's five, for the app's menus (citra_libretro/emusuni_cia.h; src/ThreeDsCia.cpp
# declares them, weakly defined there until this object brings them).
CIA_GLOBALS="emusuni_3ds_cia_version emusuni_3ds_cia_info emusuni_3ds_cia_install emusuni_3ds_installed
  emusuni_3ds_uninstall"
mapfile -t OWN < <(find "$BUILD/src" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
mapfile -t DEPS < <(find "$BUILD/externals" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
OTHERS=$(find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -not -path "$BUILD/src/*" -not -path "$BUILD/externals/*")
if [ -n "$OTHERS" ]; then
  echo "Azahar: libraries outside src/ and externals/ (whole, or as far as referenced?):"
  echo "$OTHERS"
  exit 1
fi
# (00.85: --wrap - the core's calls of __cxa_throw go to common/emusuni_throw.cpp's __wrap___cxa_throw, which logs
# the exception and, on the PS4, takes it back to the rescue point armed on its thread; else calls the real one.)
ld.lld-21 -r --wrap=__cxa_throw --whole-archive "${OWN[@]}" --no-whole-archive "${DEPS[@]}" -o "$OUT/3ds_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='azahar_retro_*' \
  $(for s in $HANDOVER_GLOBALS $CIA_GLOBALS; do echo "--keep-global-symbol=$s"; done) "$OUT/3ds_core_all.o" \
  "$OUT/3ds_core.o.new"
rm -f "$OUT/3ds_core_all.o"

# The checks.
# (00.85) The throws' logger linked in (local, as everything but the API). (Not grep -q: with pipefail, its early
# exit fails the pipe.)
if ! llvm-nm-21 "$OUT/3ds_core.o.new" | grep " t __wrap___cxa_throw$" > /dev/null; then
  echo "Azahar: the object has no __wrap___cxa_throw (common/emusuni_throw.cpp): its throws wouldn't be logged"
  exit 1
fi
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/3ds_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$( { for f in $API; do echo "azahar_$f"; done; for s in $HANDOVER_GLOBALS $CIA_GLOBALS; do echo "$s";
  done; } | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "Azahar: the object's global symbols aren't exactly the renamed libretro API, the frame's handover and the CIA" \
       "installer:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/3ds_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives (the
# PS4's loader may refuse those).
WEAK=$(llvm-nm-21 -u "$OUT/3ds_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "Azahar: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
# No std::random_device: libc++'s opens /dev/urandom, which the PS4 may not have (it throws then);
# everything random goes through common/emusuni_random.cpp.
if echo "$UNDEFINED" | grep -q '^_ZNSt3__113random_device'; then
  echo "Azahar: the object uses std::random_device (/dev/urandom on the PS4): common/emusuni_random.cpp instead"
  exit 1
fi
# mprotect, named only by Xbyak's inline code in the objects whose generators take EmuSuNi's allocator
# (dead there: useProtect() is false), and by Zydis's memory helper, which nothing references. Another
# object naming it is a generator left with Xbyak's own allocator (mmap, then mprotect(RWX): as
# dynarmic's spin lock was, at static initialisation) or a new mprotect user: it must take the pool.
MPROTECT_OK="a32_interface.cpp.o block_of_code.cpp.o Memory.c.o shader_jit_x64_compiler.cpp.o"
# (The members as "file.o": CMake keeps their directory in the archive, the PS4 toolchain names them
# .obj, this PC's .o.)
MPROTECT_USERS=$(for a in "${OWN[@]}" "${DEPS[@]}"; do llvm-nm-21 -u --print-file-name "$a" 2>/dev/null; done |
  awk '$(NF-1) == "U" && $NF == "mprotect" {print $1}' | sed 's/^.*\.a://; s/:$//; s#^.*/##; s/\.obj$/.o/' | sort -u)
MPROTECT_NEW=$(comm -23 <(echo "$MPROTECT_USERS") <(echo "$MPROTECT_OK" | tr ' ' '\n' | sort -u))
if [ -n "$MPROTECT_NEW" ]; then
  echo "Azahar: objects naming mprotect the port doesn't know (a Xbyak generator without EmuSuNi's allocator?):"
  echo "$MPROTECT_NEW"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/3ds_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "Azahar: the object defines a C++ operator new/delete of its own"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  # What the app gives it: operator new/delete (port/ps4_new.cpp) and the executable memory
  # (port/ps4_n64.c), compiled here as the app compiles them, only to list their symbols.
  PS4C_CHECK="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C_CHECK+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C_CHECK+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C_CHECK -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  clang-21 $PS4C_CHECK -O2 -DEMUSUNI_3DS=1 -c "$PROJECT/port/ps4_n64.c" -o "$OUT/check_ps4_n64.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores. The threads' hook,
  # emusuni_3ds_thread, is the app's to define in port/: a note if it isn't yet. (01.09) The free space,
  # emusuni_disk_space_known, port/ps4_diskfree.c's, for the CIA installer.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o" | awk 'NF >= 3 {print $3}';
                 echo __dso_handle; echo emusuni_3ds_thread; echo emusuni_disk_space_known; } | sort -u)
  rm -f "$OUT/check_ps4_new.o" "$OUT/check_ps4_n64.o"
  if ! grep -qs "emusuni_3ds_thread" "$PROJECT"/port/*.c "$PROJECT"/port/*.cpp; then
    echo "Azahar: note: port/ doesn't define emusuni_3ds_thread yet (the app won't link without it)"
  fi
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "Azahar: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
    echo "$NOT_APPS"
    exit 1
  fi
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a $PS4_BUILD_ROOT/sysroot/lib/libc++.a
    $PS4_BUILD_ROOT/sysroot/lib/libc++abi.a $OPENORBIS/lib/libunwind.a"
  PROVIDED=$( { for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}'; echo "$APP_GIVES"; } | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "Azahar: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "Azahar: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/3ds_core.o.new" "$OUT/3ds_core.o"; then
  rm -f "$OUT/3ds_core.o.new"
else
  mv "$OUT/3ds_core.o.new" "$OUT/3ds_core.o"
fi
ls -la "$OUT/3ds_core.o"
