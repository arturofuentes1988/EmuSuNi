/* EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
 *
 * What the PS Vita core (Vita3K, scripts/build-vita-core.sh) asks the system for, on the PS4's kernel
 * calls: the guest's 4 GiB, its page faults and the memory its recompiler writes code in
 * (port/ps4_vita_sys.c). EmuSuNi's own. On the PC the same code runs on stand-ins for the PS4's
 * calls (16 KiB pages, direct memory that comes dirty), so the tests there walk the console's paths.
 */
#ifndef EMUSUNI_PS4_VITA_SYS_H
#define EMUSUNI_PS4_VITA_SYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The host's page, on the PS4 and on the PC alike: every address and size below is a multiple. */
#define EMUSUNI_VITA_PAGE 16384u

/* ---- The guest's memory: 4 GiB of addresses, memory under the parts in use only. ----
 * One at a time. The base, or NULL when the system refuses (the log says why). */
void *emusuni_vita_mem_reserve(void);
/* Everything back to the system. */
void emusuni_vita_mem_release(void);
/* Pages in use from now on, readable and writable. What they hold is whatever was there: the caller
 * clears them. Pages already in use stay as they are, except that they become writable. 1: done. */
int emusuni_vita_mem_commit(uint64_t address, uint64_t size);
/* Pages no longer in use. Their memory goes back once a whole 2 MiB piece is out of use; until
 * then they may still be readable. */
void emusuni_vita_mem_decommit(uint64_t address, uint64_t size);
/* Pages in use: 0 no access (they keep what they hold), 1 read only, 3 read and write. 1: done. */
int emusuni_vita_mem_protect(uint64_t address, uint64_t size, int access);
/* For the log and the tests: MiB of memory under the guest's addresses now, and the most so far. */
void emusuni_vita_mem_usage(unsigned *now_mib, unsigned *most_mib);

/* ---- Page faults (SIGSEGV and SIGBUS). ----
 * handler(address, write): 1 when the access may be tried again. Anything else goes to the handlers
 * there were before (the app's crash log). Not for faults of running code at a bad address. */
typedef int (*emusuni_vita_fault_fn)(void *address, int write);
void emusuni_vita_fault_install(emusuni_vita_fault_fn handler);
void emusuni_vita_fault_uninstall(void);
/* (17-11) For the recompiler's own fault handler, which the system calls before this file's: a fault of
 * recompiled code (`info`, `context`: the signal's) on the guest's memory is given to `handler` first when the
 * "keepfastmem" switch is on. 1: done, the instruction may run again; 0: the recompiler's to deal with. */
int emusuni_vita_fault_first(void *info, void *context);
/* 1 when this file's handler is the one the system calls first: the recompiler's own, which belongs over it and
 * goes with it at emusuni_vita_fault_uninstall, then puts itself in place again. */
int emusuni_vita_fault_on_top(void);
/* For the log: faults `handler` took (protected pages), and those of them given to it first by the call above. */
void emusuni_vita_fault_counts(unsigned long long *handled, unsigned long long *first);
/* For the log: the 2 MiB pieces under the guest's addresses now, how many of them are whole (every page readable
 * and writable: nothing in the way of one large page), and how many the kernel says are one 2 MiB page (-1 where
 * it doesn't tell: the PC). */
void emusuni_vita_mem_large(unsigned *pieces, unsigned *whole, int *large);

/* ---- The recompiler's code memory: readable, writable and executable. ----
 * A block per recompiler (Vita3K makes one for each of the guest's threads): `size` bytes of addresses
 * (128 MiB at most), with memory under them only where something was written. The first write to each
 * 256 KiB piece faults and the fault handler maps it (system flexible memory), so the block must be
 * written before it is read or run, as a recompiler does, and new memory holds zeros. NULL when there
 * is none (the log says why). */
void *emusuni_vita_jit_alloc(size_t size);
/* The block's memory back to the system. */
void emusuni_vita_jit_free(void *block);
/* Before a game starts: 1 when a block can be had and code runs in it. Vita3K's recompiler throws a C++
 * exception when it gets no block, and on the PS4 an exception ends the app: asked first, the game's
 * load fails with a message instead. */
int emusuni_vita_jit_ready(void);
/* The size of each recompiler's block: the number in the file /data/EmuSuNi/vita-jit-mib (the PC:
 * EMUSUNI_VITA_JIT_MIB) when there is one; else 64 MiB with the "bigjit" switch (the first 40 blocks; 16 MiB
 * the ones after, so that the reservation holds them all) and 16 MiB without it. When a block is full its
 * recompiler starts over. */
size_t emusuni_vita_jit_cache_size(void);
/* (01.34) With blocks of 64 MiB the code memory of all of them is kept under 112 MiB: 1 when the recompiler
 * that is about to write at `at` (the place of its next code) should start over instead - it is past its
 * first 16 MiB, the memory there isn't mapped yet and the whole is at its limit. */
int emusuni_vita_jit_tight(const void *at);

/* ---- (01.34) The speed switches. ----
 * What another PS4 port of Vita3K does for speed and this one didn't, each behind a switch. A switch is ON or
 * OFF by its default below unless a file says otherwise: /data/EmuSuNi/vita-<name>-on or
 * /data/EmuSuNi/vita-<name>-off (empty files; off wins). On the PC: EMUSUNI_VITA_<NAME>=1 or =0. They are
 * looked at once a game, by its first recompiler, and one line of the log says how each one is. */
enum
{
  /* "fastfp", ON: the recompiler's floating point with the PS4's own instructions - no fused multiply-add
   * (the PS4's CPU has none: the exact way does it in software), the host's estimates for reciprocals, no
   * fixing of NaN answers, no change of the rounding state around each vector operation. */
  EMUSUNI_VITA_FASTFP,
  /* "bigjit", ON: 64 MiB of code memory a guest thread instead of 16 (see emusuni_vita_jit_cache_size). */
  EMUSUNI_VITA_BIGJIT,
  /* "framequeue", ON since 17-11 (after 01.63): a game that keeps one image waiting hands an image over and goes
   * on once the image before is shown, as the console does; OFF is Vita3K's way, waiting until that very image
   * is shown. 01.45's console log of Mortal Kombat, the one game tried that keeps such a queue: its fights at
   * 19.5 pictures a second - three refreshes of the screen a picture exactly - with 40-42 ms of work in each. */
  EMUSUNI_VITA_FRAMEQUEUE,
  /* "fastatomics", ON since the set 0174 (OFF before): the game's exclusive loads and stores (its locks and
   * counters) as one instruction of the PS4's on the guest's memory, without the monitor all the threads
   * share. Not exact where another thread writes a value and writes it back in between - Uncharted stopped for
   * good with it (the bench, set 0173) -, which is what "monitor64" below is for. */
  EMUSUNI_VITA_FASTATOMICS,
  /* "hints", ON: the ARM's hints the emulator does nothing for (a preload, a yield, an event) are no code at
   * all; OFF, each one ends its block with a call out, as upstream. */
  EMUSUNI_VITA_HINTS,
  /* "filewalk", ON: a file's name in another case is looked for folder by folder; OFF, the game's whole
   * folder is listed (once in 10 s), as before. */
  EMUSUNI_VITA_FILEWALK,
  /* "copybarrier", ON: the copy of a painted surface for the draws that sample it waits for what was drawn
   * on it and its two steps for each other (the PS4's GPU reads parts not written yet otherwise). */
  EMUSUNI_VITA_COPYBARRIER,
  /* "mxcsr": the guest's floating point state (its rounding, its flush-to-zero) is put back after every call
   * out of the recompiled code. Upstream does that only when it counts cycles, which Vita3K doesn't: the
   * state the game had set was replaced by the host's at each thread's first system call. */
  EMUSUNI_VITA_MXCSR,
  /* (01.40) Three for the pictures, all OFF: tests for a game whose menus come out wrong.
   * "texsync": every texture a draw samples is looked at again (and sent to the GPU again when its bytes
   * changed) at each draw, not only after the game binds it or starts a scene. Slow: a hash of each texture
   * at each draw. For a game that writes into a texture it keeps bound (a text's glyphs). */
  EMUSUNI_VITA_TEXSYNC,
  /* "accuracy": Vita3K's own "high accuracy" option of the renderer. */
  EMUSUNI_VITA_ACCURACY,
  /* "syncshaders": a pipeline is compiled before the draw that needs it (Vita3K's option, here the other way
   * round: by default they are compiled aside and the draw is left out until its pipeline is there). */
  EMUSUNI_VITA_SYNCSHADERS,
  /* "gpubarrier": before each scene's commands the GPU is told that everything written so far - colour,
   * depth, transfers - must be done and readable. On the PC it changes nothing; on the PS4's GPU it has the
   * driver empty its colour and depth caches, which is the test: a picture that is right with it was reading
   * what the GPU hadn't written yet. */
  EMUSUNI_VITA_GPUBARRIER,
  /* "centertouch" (after 01.47): R3, a button the PS Vita doesn't have, is a finger on the middle of the front
   * screen (of the rear panel with L2 held) while the frontend sends no touch of its own - the app sends none
   * for this console yet. Enough for a title that waits for a touch anywhere (Uncharted: Golden Abyss's
   * "Touch to Start"); a game that asks for a touch at a place needs the app's. */
  EMUSUNI_VITA_CENTERTOUCH,
  /* "keepfastmem" (17-11): a page fault of recompiled code on a page the emulator itself protects (a texture it
   * has in its cache is read only, so that a write says the texture changed) goes to the emulator's own handler
   * first, which lifts the protection and has the instruction run again. Without it the recompiler takes the
   * fault for a bad address: it does that access through a call (where it faults a second time) and makes the
   * instruction's block again with a call in its place for good - every later use of that instruction, a
   * copy routine's store say, then costs a call. */
  EMUSUNI_VITA_KEEPFASTMEM,
  /* "nativelib" and "fastcalls" (17-11): the firmware library's long routines (memcpy, memmove, memset, strlen)
   * done by the PS4 itself, and the system's simplest calls answered without leaving the recompiler -
   * port/ps4_vita_native.cpp says how, and how one of them is left out by its name. */
  EMUSUNI_VITA_NATIVELIB,
  EMUSUNI_VITA_FASTCALLS,
  /* "cpuopt" (17-11), ON: the recompiler's optimizations and its fast memory, as Vita3K's CPU Optimisation option
   * (on by default) says. Until 01.63 this core never had them: its loader started the kernel with a field of
   * the kernel's that nothing sets, where the standalone passes the option. Every guest thread's recompiler was
   * made with no optimizations at all - each block left through the dispatcher's lookup - and without fast
   * memory - each memory access of the game's a call of the emulator's. OFF is that again, should a game need
   * it. (The other speed switches that are about the recompiler - fastfp, fastatomics - did nothing without it.) */
  EMUSUNI_VITA_CPUOPT,
  /* "ngsdefer" (after 01.66), ON: a voice keyed off (sceNgsVoiceKeyOff) ends, and its "finished" callback runs, at
   * the next sceNgsSystemUpdate and on that call's thread - where a PS Vita runs it. Vita3K ran the callback
   * inside sceNgsVoiceKeyOff itself, on the thread that keyed the voice off: MotorStorm RC makes that call holding
   * its sound system's lock, its callback takes the same lock, and the game stopped there for good at its first
   * race (its main thread and its two sound threads all waiting for that lock). OFF is Vita3K's way again. */
  EMUSUNI_VITA_NGSDEFER,
  /* "offline" (after 01.68), OFF: a test - the console says what is true here, that it has no network. Vita3K
   * answers a game that the network is connected (sceNetCtlInetGetState: an address obtained) and that the
   * system's network check dialog went well (sceNetCheckDialog, PlayStation Network modes); a game then goes on
   * to its servers, which nothing here reaches (port/ps4_vita_nonet.c), and waits: LittleBigPlanet stays at
   * "Connecting to LittleBigPlanet PS Vita Online". With the switch the state is "disconnected" and the dialog
   * ends as when the player closes it without connecting. (Ad hoc play is another state: as it was.)
   * OFF until a game is known to go further with it: LittleBigPlanet stays at that screen with it too, and it
   * changes what every game is told. */
  EMUSUNI_VITA_OFFLINE,
  /* "mic" (after 01.67), ON: the microphone hears silence at its real pace. In this core sceAudioInInput gave its
   * block of silence at once (a PS Vita's waits until the block is recorded: 16 ms), and sceAudioInGetStatus said
   * "muted" while no port was open: LittleBigPlanet's "Mic Thread" asked 5 million times a second, a whole core.
   * With the switch the microphone isn't muted and each block takes its time. OFF is as it was. */
  EMUSUNI_VITA_MIC,
  /* "drawcopy" (after 01.68), OFF: a test - what a draw reads from the game's memory (its uniform buffers, its
   * vertices, its indices) is copied when the game asks for the draw. Vita3K hands the renderer's thread their
   * addresses, and that thread reads them when it gets to the draw (the "memory mapping" option off, as it is
   * here): a moment later on a PC, later still on the PS4, where that thread is the slow one. A game that writes
   * its next pictures' data in the same places before then would get draws with the vertices of one picture
   * and the uniforms or the indices of another. Not seen in any game on the PC bench, even with the renderer's
   * thread held back; kept to try on the console. (Not for a deferred context's draws: the game keeps those
   * lists itself.) */
  EMUSUNI_VITA_DRAWCOPY,
  /* "freshshaders" (after 01.68), OFF: a test - nothing that an earlier start of the game saved for its pictures is
   * read: neither the list of the shaders made for it (so they are all made again) nor the GPU driver's pipeline
   * cache (so the driver compiles every pipeline again). The console reads both from a game's second start on,
   * and what is on its disk may be of an older version of the app. Slower starts and the first seconds' stutter
   * of a first start, every time. To see whether a picture that comes out wrong comes out right then. */
  EMUSUNI_VITA_FRESHSHADERS,
  /* "vertexw" (after 01.68), ON: a vertex shader for each layout of its vertex data, on a GPU that doesn't take
   * vertex data of 3 components of 8 or 16 bits (AMD's of the PS4's generation). Vita3K reads 4 components there
   * and writes into the vertex shader which attributes must have their 4th one set to 1 - as the first layout
   * the shader is made for has them - and then uses that shader for every layout of the same program: with
   * another layout the 4th component is garbage (a position's: the vertex goes anywhere; a colour's: another
   * colour) or a 1 that was not meant. The PC bench's GPU takes those formats, so the bench never drew that
   * way. OFF is Vita3K's one shader a program again. (Does nothing on a GPU that takes those formats.) */
  EMUSUNI_VITA_VERTEXW,
  /* "vertexalign" (after 01.68), ON: a vertex stream whose vertices are not a whole number of 4 bytes apart is
   * given to the GPU with each vertex padded to the next multiple of 4. The PS4's GPU is said to read numbers of
   * 2 and 4 bytes wrong when their vertices are an odd number of bytes apart (HENkaku-PS4's notes; the PC bench's
   * GPU reads any). Street Fighter X Tekken's menus draw their text and some shapes with vertices 17 and 21 bytes
   * apart with floats in them: on the console, shapes pulled across the screen and the options' text gone. OFF
   * is the streams as the game has them. */
  EMUSUNI_VITA_VERTEXALIGN,
  /* "condrelease" (after 01.70), ON: a thread that waits for a condition gives up the condition's mutex whole -
   * every lock it has on it, when it holds it more than once - and takes them all back after the wait. Vita3K
   * unlocks it once: a thread that held it twice goes on holding it while it waits, and the thread that must
   * take the mutex to signal the condition waits for ever. A guess at why God of War Collection's launcher
   * stops on the console before its first picture (its main thread in sceKernelWaitLwCond, a file thread in
   * sceKernelLockLwMutex): the PC bench never stops and never has a thread wait that way, so it is not seen at
   * work yet - the log's "vita: kernel:" line says so the first time a thread does. OFF is Vita3K's one
   * unlock. */
  EMUSUNI_VITA_CONDRELEASE,
  /* "zipmarks" (after 01.70), ON: a compressed file of a game's ZIP is read at any place without inflating it
   * again from its first byte - several streams a file, by where they are, and marks left every 4 MiB to start
   * one from (some 45 KiB each, 320 at most: 14 MiB). Vita3K keeps one stream a file and, for a part before the
   * stream's place, starts again from the file's start and throws away everything up to that part: Street Fighter
   * X Tekken reads its 1 GiB archive (compressed in the owner's ZIP) here and there, and on the console each read
   * took 50 to 190 ms - minutes at the "VS" screen before a fight. OFF is Vita3K's one stream a file. */
  EMUSUNI_VITA_ZIPMARKS,
  /* "uniformlog" (after 01.70), a look and not a fix: the numbers a game gives each of its shader programs (a
   * uniform buffer's floats) go into the log the first times a program is given different ones - "vita:
   * uniforms:", up to 24 sets a fragment program and one a vertex program, 2400 lines a game at most and only
   * in its first 20 minutes. For a picture that is wrong on the console and right on the PC bench (Street
   * Fighter X Tekken's fighters with black skin, a colour their shaders take from the game's numbers): the same
   * lines from both say whether the game gave other numbers or the GPU made another picture of the same ones.
   * ON in the sets 0172 to 0176 (01.72 to 01.76), so that the console's logs had them without a file to make;
   * OFF since the set 0177: the pictures they were for (Uncharted dark, black skin) were the app's, which drew
   * the Vita's picture with its alpha over black. */
  EMUSUNI_VITA_UNIFORMLOG,
  /* "vertexrelay" (after 01.70), ON: a vertex stream that holds numbers of 2 or more bytes at a place inside the
   * vertex that is not a multiple of their size is given to the GPU laid out anew - each attribute at the next
   * multiple of 4 bytes, in the order they have in the vertex. The other half of "vertexalign": HENkaku-PS4's
   * notes say the PS4's GPU reads such numbers wrong at odd distances between vertices - seen on the console,
   * 01.70 - and at odd places inside them - not seen yet: God of War in game has them (vertices of 15 and 19
   * bytes with 16-bit numbers at odd places) and it doesn't get past its launcher there. OFF is the attributes
   * where the game has them (the vertices still padded by "vertexalign"). */
  EMUSUNI_VITA_VERTEXRELAY,
  /* "coreturns" (after 01.71), ON for God of War Collection (PCSA00126) and OFF for every other game: the threads
   * a game ties to one of the Vita's three cores run the game's code one at a time, as on the Vita - a thread
   * takes its core's turn to run and gives it up when it waits; another takes it when nobody has it, when its
   * holder has been inside a system call for 0.3 ms or has had it for 10 ms, or at once with a higher priority.
   * Since the set 0174 a thread whose wait is over takes the turn back before it goes on, and a lightweight mutex
   * let go of is not handed to a waiting thread of these: it takes it itself once it has the turn.
   * Sony's file library counts on that (two of its threads take two mutexes in opposite orders): with a core
   * of the PS4's for each thread they cross, and God of War Collection's launcher stopped at every start on
   * the console. A file turns it ON or OFF for whatever game is started. */
  EMUSUNI_VITA_CORETURNS,
  /* "monitor64" (after 01.73), ON: the 64-bit exclusive loads and stores (LDREXD/STREXD) go through the monitor
   * all the threads share even when "fastatomics" is ON - a store then fails when another thread stored there
   * since the load, whatever the value. A queue without a lock keeps its head in such a pair of words (Sony's
   * own threading library, in Uncharted): compared by value alone, a head taken out and put back in between
   * passes for untouched, a waiting thread is lost and the game's loading never ends. OFF, they are one
   * instruction of the PS4's like the 8, 16 and 32-bit ones. Nothing without "fastatomics". */
  EMUSUNI_VITA_MONITOR64,
  /* "lwfast" (after 01.73, the performance specialist's; port/ps4_vita_lwfast.cpp), OFF: a lightweight mutex's
   * lock, try and unlock are answered inside the recompiler's call for them when nothing has to wait - the
   * same calls on the emulator's mutex as the ordinary way makes, without the stop and the start of the
   * recompiled code around them (Street Fighter X Tekken's job threads make 1.1 million of them every 10 s in
   * a fight). A lock that would wait goes the ordinary way. OFF until the console has tried it. */
  EMUSUNI_VITA_LWFAST,
  /* "queuelimits" (after 01.74), ON (OFF for MotorStorm RC, PCSC00014): the queue of images waiting to be shown
   * is as long as the game asks for (16 at most), and a hand-over waits for room in it before it marks its
   * buffers - Vita3K's own later fix (its pull request 4354) for games at 24 or 48 pictures a second where a
   * Vita shows 30 or 60. OFF is the queue one
   * shorter, 2 at most, as before: Street Fighter X Tekken asks for 3 and had 2, Uncharted and MotorStorm RC ask
   * for 2 and had 1. Nothing for a game that asks for 1 (Mortal Kombat, God of War, LittleBigPlanet).
   * (After 01.80) With a renderer's thread that runs behind, the longer queue lets a game get a picture further
   * ahead and write over the vertices and uniforms of draws the renderer has not read yet: MotorStorm RC's
   * random shapes on the console (01.80), on the bench a white box and a broken panel in 6-18 pictures of 99
   * while driving, none with the switch OFF (and no CPU more). OFF for that game only (Street Fighter X Tekken
   * reached 60 pictures a second on the console with it ON); a file still changes it for every game. */
  EMUSUNI_VITA_QUEUELIMITS,
  /* The three below (after 01.74) are about how the Vita's shader programs are translated. A game's shaders are
   * saved once translated: to see one of these switches the other way the saved ones must not be read, which the
   * "freshshaders" switch does (the file vita-freshshaders-on beside the switch's own).
   * "negp2", ON: a vector instruction that runs on "not p2" keeps its condition. OFF, it runs always, as upstream
   * (Mortal Kombat has such shaders). */
  EMUSUNI_VITA_NEGP2,
  /* "loopcap", ON: a translated loop ends after 4096 turns at most - a loop that never ends is a GPU that never
   * finishes, and the PS4 closes the app for that. OFF, no limit, as upstream. */
  EMUSUNI_VITA_LOOPCAP,
  /* "intclamp", ON: a number made an integer of 8 or 16 bits is first held inside what the type can hold; out of
   * it the conversion is whatever the GPU does. OFF, as upstream. */
  EMUSUNI_VITA_INTCLAMP,
  /* "fastgxm" (after 01.76), OFF: the graphics library's setters (27 of them: a number for a shader, a state of
   * the next draw, a texture's filter - port/ps4_vita_native.cpp has the list) are answered inside the
   * recompiler's call for them, as the "fastcalls" are, without the stop and the start of the recompiled code
   * around each (1.1 microseconds on the PS4). For God of War's "Render" thread: 3.1 million calls every 10 s
   * on the console, two in three of them these on the bench. OFF until the console has tried it (the file
   * vita-fastgxm-on). */
  EMUSUNI_VITA_FASTGXM,
  /* "ngsresample" (after 01.76, the performance specialist's; port/ps4_vita_ngsresample.cpp), OFF: the voices of
   * a game's sound that are not at 48 kHz go through a resampler of the port's own - 16 products a sample, its
   * tables made once, a voice going on from where it was when its pitch changes - instead of FFmpeg's, which
   * Vita3K makes anew (a table of 32 thousand coefficients) at every change of a voice's pitch: an engine's
   * sound, every update. MotorStorm RC's "NGSynth Update Thread" was 79-88% of a core on the console for that.
   * The sound is not the same to the last bit (a little less of the highest notes): OFF until it was heard on
   * the console (the file vita-ngsresample-on). */
  EMUSUNI_VITA_NGSRESAMPLE,
  /* "res2x" (after 01.76): the picture is made at twice the PS Vita's size each way (1920x1088) whatever the
   * frontend's option for it says - Vita3K's own "internal resolution multiplier". More work for the GPU only:
   * the game runs the same code. OFF is the size the frontend asks for (the Vita's 960x544 when it says nothing). */
  EMUSUNI_VITA_RES2X,
  /* "netbind" (after 01.76), ON: a game that gives a socket a port (bind) is told "done", and nothing is bound -
   * as on a PS Vita whose network is down, where the socket is made and bound and simply nothing arrives. OFF, the
   * bind fails ("network down"), as every call that would reach the network does (port/ps4_vita_nonet.c):
   * LittleBigPlanet then closes its socket and waits for ever at "Connecting to LittleBigPlanet PS Vita Online".
   * With it that game goes on to its introduction (the bench); nothing leaves or reaches the console either way. */
  EMUSUNI_VITA_NETBIND,
  /* "fastids" (after 01.76, the performance specialist's; port/ps4_vita_fastids.cpp), OFF: the two calls a
   * thread makes to know itself - its id, the address of its own block of data (TLS) - are answered at once
   * from what the recompiler has of the thread, where "fastcalls" still went through Vita3K's own answer (a
   * lock and a look in a map each time). On a PS Vita they are no calls of the system at all. Uncharted's main
   * thread makes 2.4 million of them every 10 s on the console (01.75): about a tenth of that thread. OFF until
   * the console has tried it (the file vita-fastids-on). */
  EMUSUNI_VITA_FASTIDS,
  /* "msgauto" (01.78), OFF: the system's message dialog answers itself. A PS Vita draws the message over the
   * game and waits for a button; this core draws no dialog, so a game that opens one waits for ever. OFF until
   * a game is seen to open one (the file vita-msgauto-on): none of the bench's six does - LittleBigPlanet only
   * asks for the dialog's state, it never opens it -, so the answering has not been seen at work. Either way the
   * log says when a game opens one and what it says ("vita: dialog:"). With the switch ON a message with buttons is
   * answered 1.5 s after it opens with the button that changes nothing: its only button when it has one, "No"
   * or "Cancel" when it has two or three. A lone "Cancel" (a wait the game may still end by itself) is given
   * 20 s first. A "please wait" and a progress bar are never answered: they are the game's to close, as on a
   * PS Vita. The log says each message's text and what was answered ("vita: dialog:"). */
  EMUSUNI_VITA_MSGAUTO,
  /* "surfacepick" (after 01.78), ON: a texture that lies on a surface the game painted is looked for among all
   * the painted surfaces that hold its address, not only the nearest one below it. LittleBigPlanet paints its
   * effects into parts of one large surface (768x1024) and samples those parts as textures by their address
   * inside it; a smaller surface it also paints (its shadows, 16 bits a pixel) starts inside that same memory,
   * nearer the textures. Vita3K looked only at that nearer one, found it laid out otherwise and read the
   * textures from the game's memory - where nothing painted ever is, here -: every 3D picture of that game
   * came out a flat grey, on the console too (01.78). With the switch, when the nearest surface is laid out
   * otherwise (tiling, bytes a line), another one that holds the texture whole with the texture's layout is
   * taken; and when the nearest one ends before the texture (that game's third such picture lies past the
   * shadows' end), one further below that holds its address. The log's second "vita: gfx:" line counts the
   * textures of each case. */
  EMUSUNI_VITA_SURFACEPICK,
  /* "gxmfast" (after 01.78, the performance specialist's; port/ps4_vita_gxmfast.cpp), OFF: the GPU library's
   * simplest calls answered inside the recompiler's call for them - a program parameter's type, component
   * count, array size, resource index and the like read from Vita3K's own structures; sceGxmSetUniformDataF
   * and the searches for a parameter, their export called from there. On a PS Vita they are functions of a
   * library, not calls of the system. MotorStorm RC's main thread makes 1.96 million of the four getters
   * every 10 s on the console (01.78): about 2.7 s of every 10 of that thread. OFF until the console has tried
   * it (the file vita-gxmfast-on). */
  EMUSUNI_VITA_GXMFAST,
  /* "rawhalves" (after 01.78), ON: a fragment program whose output is 64 bits of raw content (two 32-bit
   * words) drawn on a surface of four half floats writes those 64 bits as they are: four halves with the
   * words' bits, from where the program says its output starts. Vita3K wrote the output register's four
   * 32-bit words as four float numbers instead, from its start whatever the program said. Resistance -
   * Burning Skies paints two colours of 8 bits a channel on each pixel of such a surface that way and reads
   * them back as a texture twice as wide: its 3D pictures came out red and blue, no green, in bands - on the
   * console (01.78) and on the bench. The saved shaders' version changes with it (1502). */
  EMUSUNI_VITA_RAWHALVES,
  /* "movpclr" (after 01.78, the performance specialist's), OFF: the recompiler ends the block of the ARM
   * instruction MOV PC, LR - the last one of every import stub Vita3K writes - as a return (like BX LR, with the
   * return stack buffer's guess) where upstream dynarmic goes back to its dispatcher and looks the next block
   * up in C++: that made every system call of a game, answered inside the recompiler or not, come back through
   * a look-up (the log's "blocks looked up by the dispatcher" equalled the system calls: Uncharted 4.3 million
   * and MotorStorm RC 3.7 million every 10 s on the console, 01.78). No word of the game's memory changes.
   * Read once, when the first block is translated. OFF until the console has tried it (the file
   * vita-movpclr-on). */
  EMUSUNI_VITA_MOVPCLR,
  /* The five below (after 01.80) are the performance specialist's delivery for 01.82 (its note
   * vita-0182-rendimiento.md), all OFF until the console has tried each; the first three are read once a start
   * of the app. "nzcvword": the recompiler writes the guest's N, Z (and C) flags as the whole flags word, not one
   * byte of it, so that the condition read right after (which reads the word) is not held up by the store
   * (Uncharted's hot blocks: the flags from 25-29% of their samples to 11-12% on the bench). */
  EMUSUNI_VITA_NZCVWORD,
  /* "vfp64": a double register's VLDR/VSTR/VLDM/VSTM as one 64-bit access, not two 32-bit ones packed
   * (little-endian games only, as the Vita's run; MotorStorm RC's vector maths). */
  EMUSUNI_VITA_VFP64,
  /* "gamefn" (port/ps4_vita_gamefn.cpp): one of a game's hot functions done natively - found by its first bytes and
   * a hash of all of them, no game code in the source -: today only Uncharted's tree search (its main thread -5%). */
  EMUSUNI_VITA_GAMEFN,
  /* "gxmring" (port/ps4_vita_gxmring.cpp): an immediate context's commands to the renderer from a ring of the
   * emulator's own (32768 commands) before the game's and before new, and arenas of the app's heap of their own
   * for the threads that make and free them (on the bench sceGxmDrawPrecomputed -15% in the main thread). */
  EMUSUNI_VITA_GXMRING,
  /* "gxmringcount": the commands as ever, counted - the log's "vita: gxmring:" line says how many came with new
   * (the game's ring used up): to try on the console before "gxmring". */
  EMUSUNI_VITA_GXMRINGCOUNT,
  /* "gxmpacket" (port/ps4_vita_gxmpacket.cpp; after 01.82, the Vita specialist's): a draw's commands to the renderer
   * (uniform buffers, programs, textures, vertex streams, the draw) as the same records, in the same order, in one
   * packet of a ring of the emulator's own - one command a draw, not 8-15; the draw's highest index found in the
   * renderer, not by a pass over all its indices in the game's thread; and the commands' frees given back every 64
   * and at the end of each list, not one by one on a line the game's thread reads at every command. */
  EMUSUNI_VITA_GXMPACKET,
  /* "stubcall" (19-11, for 01.83, the call-bridge specialist's; port/ps4_vita_stubcall.cpp), OFF: an import stub
   * ("svc #0; mov pc, lr; <number>") made by the recompiler, when it translates it, into one block with a direct
   * call of the function found then - for the calls the switches nativelib, fastcalls, fastgxm, gxmfast, lwfast
   * and fastids answer inside the recompiler: no reading of the number, no four tables, no call_import, no
   * clear_exclusive, no second block for "mov pc, lr"; sceKernelGetThreadId/GetTLSAddr with the thread's own
   * numbers written into the block; every other call straight to the thread's loop. The games' busiest threads
   * make 1.6-9 million such calls every 10 s (on the bench -5% of Uncharted's and MotorStorm RC's main thread). */
  EMUSUNI_VITA_STUBCALL,
  /* "callstats" (19-11, for 01.83), OFF: with "stubcall", the time of those calls measured on one call in 16
   * (the time stamp counter costs about as much as a short call); their count stays exact. */
  EMUSUNI_VITA_CALLSTATS,
  /* "iolock" (ON; 01.82's LittleBigPlanet): the open files', terminals', folders' and found paths' tables of the
   * core (io/state.h) behind a lock each while they are looked into or changed, the file's own work outside it;
   * the numbers of new files given atomically. Upstream has none: the game's threads opening files at once broke the
   * tree of open files (LittleBigPlanet fell in std::map's insert in open_file, its fonts loading on many threads). */
  EMUSUNI_VITA_IOLOCK,
  /* "uniformcopy" (OFF, 01.83; to try with MotorStorm RC's shapes): a draw's default uniform buffer (block 0, the one
   * GXM's ring gives out) copied as the draw is asked for - into its packet with "gxmpacket", else a block of its own -,
   * not read by the renderer from the game's memory when it gets to the draw. Vita3K's ring takes the buffer back after
   * a lap without waiting for the renderer: when the renderer is a lap late, the draw gets the next picture's numbers.
   * On the bench it took MotorStorm's shapes from 2-6 in 59 pictures to 1 with gxmpacket, not to 0: not all of it. */
  EMUSUNI_VITA_UNIFORMCOPY,
  /* "stubgxm" (19-11, for 01.83, the call bridge's), OFF: with "stubcall", 97 more of the GPU library's simplest
   * functions - setters and getters of a texture's, a precomputed state's and the context's words,
   * port/ps4_vita_stubcall.cpp's kMoreGxm - answered inside the recompiler: in Street Fighter X Tekken's fight
   * ~100 thousand calls every 10 s on the bench went to the thread's loop for three of them. */
  EMUSUNI_VITA_STUBGXM,
  /* "sixcores" (20-11, for 01.84), OFF: every thread of the core's - the game's, the renderer's, the pipeline
   * compilers' - on the PS4's cores 0-5 (0x3f), never the seventh, which the system shares; each sets it itself as it
   * starts (port/ps4_vita_place.cpp; "vitaplace" keeps to those six too). HENkaku-PS4 starts all its threads so; ours
   * floated on all seven (the log's "cores 0x7f"), Uncharted's main thread and renderer each near a whole core. */
  EMUSUNI_VITA_SIXCORES,
  /* "texbatch" (20-11, for 01.84), OFF: a texture the cache checks by hashing its bytes is hashed once a command list,
   * not again at each later bind of it in the same list (the list's draws read the textures as they are when the
   * renderer gets to it). HENkaku-PS4 hashes so ("once a batch": God of War II hashed 11,800 times a second). Not
   * with "texsync" ON, which asks for a look at every draw. The line "vita: renderer:" says the textures hashed. */
  EMUSUNI_VITA_TEXBATCH,
  /* "vertexrange" (20-11, for 01.84), OFF: a draw's per-vertex streams copied for the GPU from the draw's lowest
   * index's vertex, not from vertex 0, and drawn with the GPU's vertex offset at minus that index (the indices as
   * they are). HENkaku-PS4 copies "only the vertex range the draw uses". Instanced streams, the ordered
   * (primitive by primitive) path and memory mapping as before. The line "vita: renderer:" says the vertex bytes
   * copied and those below the lowest vertex (near 0 with the switch). */
  EMUSUNI_VITA_VERTEXRANGE,
  /* "texviewport" (20-11, for 01.84), OFF: the app makes the Vulkan device itself (Granite, which enables every
   * feature the GPU offers), so the core took none of the optional ones - with no "standard layout" of uniform buffers,
   * no texture viewports, and every part of a painted surface a texture reads was copied for it. With the switch the
   * standard layout is taken when the GPU offers it, and texture viewports with it: such a part is sampled in place.
   * Uncharted's first chapter on the bench: 12 surface copies a picture, 70% of them parts of the surface being drawn
   * on; HENkaku-PS4, which makes its own device, 2. Read when the renderer starts (the app's start of the core). */
  EMUSUNI_VITA_TEXVIEWPORT,
  /* "submitthread" (20-11, for 01.85), OFF: the core's submissions to the GPU made on a thread of their own, in their
   * order; the renderer only queues them. The PS4's GPU driver (DolphinPS4's RADV winsys) makes two kernel calls for
   * each on the thread that submits, sceGnmSubmitCommandBuffers (~0.4 ms) and sceGnmSubmitDone, here under the
   * frontend's queue lock: 01.84's Uncharted waited 0.58 ms a submission, 14 a picture. HENkaku-PS4 (Crema's winsys
   * since its 1.08) makes them on a thread: 45 us. What waits for the GPU first waits for the submissions it needs.
   * Read when the renderer starts. */
  EMUSUNI_VITA_SUBMITTHREAD,
  /* "presentnowait" (20-11, for 01.85), OFF: each picture's copy for the frontend no longer waited for on the CPU
   * right after it is submitted - the renderer waited there for the GPU to finish the whole picture -, but at the next
   * copy; a barrier keeps the game's later drawing into the surface after the copy's read of it. HENkaku-PS4 hands its
   * picture to a flip the GPU does. The line "vita: renderer:" says the renderer's wait for the copies. */
  EMUSUNI_VITA_PRESENTNOWAIT,
  /* "surfacewatch" (20-11, for 01.86), ON: a fix. Each colour surface made watches its memory for the CPU's writes
   * (add_protect). Made again in the same place - Uncharted remakes ~2 a picture, the same address in another size or
   * format -, it watched the range once more, and its earlier watch there stayed: a callback setting the old surface's
   * flags, which nothing reads any more, never cleared (the GPU's drawing doesn't trip it). The range's watches piled
   * up and each new one merged them all: on the PS4 a scene's commands went from ~200 to ~500 us in minutes (01.84,
   * 01.85), and back down after a loading screen. With it the slot's earlier watch on the range is dropped as the new
   * one comes - nothing that would act is lost. The line "vita: memory watch:" says the watches. */
  EMUSUNI_VITA_SURFACEWATCH,
  /* (The Vita's motion, 01.88, is no switch: the console's setting PS Vita Motion Sensor, vita_motion, which the app
   * reads - src/Emulator.cpp MotionVita - offering the core the DualShock 4's sensors with it. 0188a's "motion", 63,
   * read a file: gone before any version had it.) */
  EMUSUNI_VITA_SWITCHES
};
int emusuni_vita_switch(int which);
/* The game that is being loaded, by its title's code (the core says it before the game's first thread): for the
 * switches that are ON for one game only. The switches are looked at again after it. */
void emusuni_vita_game(const char *title);
/* (17-11) A test on the console without a computer at hand: with the empty file /data/EmuSuNi/vita-test-cycle (the
 * PC: EMUSUNI_VITA_TEST_CYCLE=1) four of the graphics' test switches - gpubarrier, texsync, syncshaders and, after
 * 01.68, drawcopy - go ON one at a time, 20 seconds each, with 20 seconds of none of them first and between rounds
 * (a round: 100 seconds). Called once a refresh
 * of the screen: the words to show on it when a step starts (the player says which words were up when the
 * picture was right), else NULL. The log has a line for each step. */
const char *emusuni_vita_test_tick(void);

/* ---- (after 01.71) port/ps4_vita_stats.cpp, for the app's FPS counter ---- */
/* The pictures the game itself showed (its sceDisplaySetFrameBuf calls, what the log's "vita: speed:" line counts) -
 * not the screen's refreshes, which are 60 a second whatever the game does. The first: how many since the core was
 * loaded. The second: those of the last second, as a rate; the app calls it once a frame while the game runs. Only
 * there when the app has the PS Vita core linked in. */
unsigned long long emusuni_vita_pictures_total(void);
unsigned emusuni_vita_pictures_per_second(void);

/* ---- (17-11) port/ps4_vita_native.cpp ---- */
void emusuni_vita_native_start(const char *(*name_of)(unsigned));
int emusuni_vita_native_stub(unsigned nid);
int emusuni_vita_svc_kind(unsigned nid);
void emusuni_vita_native_run(int kind, unsigned *r, unsigned char *base);
/* For the log and the tests: KiB of code memory mapped now and at most, and the blocks in use. */
void emusuni_vita_jit_usage(unsigned *now_kib, unsigned *most_kib, unsigned *blocks_now);

/* A line in the app's log (on the PC: stderr). */
void emusuni_vita_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* (01.29) A step of the start, "vita: step: ...", written to the disk at once (the last one in the log says
 * where the app ended); and the log so far written to the disk. */
void emusuni_vita_step(const char *format, ...) __attribute__((format(printf, 1, 2)));
void emusuni_vita_sync(void);

#ifdef __cplusplus
}
#endif

#endif
