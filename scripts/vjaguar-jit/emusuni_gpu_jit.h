/* EmuSuNi (scripts/build-atari-cores.sh, 00.96): the Jaguar GPU's recompiler, x86-64 (the PS4's CPU, the PC's).
 * GPL-3.0, as Virtual Jaguar. Included at the end of src/tom/gpu.c, so its handlers and state are in reach.
 *
 * The GPU's code in its 4 KB of local RAM becomes blocks of native code, one per entry point: a straight run of
 * instructions up to a jump (jr, jump, with their delay slot) or GPU_JIT_MAX_INSNS. The machine is in the same state
 * after every instruction as with GPUExec's loop - the same registers, flags and memory, cycles charged (a load's or
 * store's bus stall too, at any RISC clock scale), opcode count, late load write-back (#811), G_FLAGS retire
 * countdown, and at a block's exit the same gpu_pc, gpu_instruction and fields the loop leaves (a save state's) -, so
 * its pictures, sound and save states are the same (the PC: local/herramientas/prueba-atari, the harness's hashes
 * with it on and off, every game there).
 *   - The ALU's common instructions (moves, add/sub/cmp and their quick forms, logic, shifts by a constant, bit test,
 *     16-bit multiplies) are inline code on the GPU's registers (rbx: the live bank, rbp: the other), the cycles in
 *     r13d; the rest call their handler (gpu_opcode_*) with the globals it reads set.
 *   - A jr or jump whose delay slot is such an instruction is inline, its condition read from the flags; any other
 *     calls its handler, which runs its delay slot as the interpreter does.
 *   - After a load or a store, the loop's tail inline when nothing unusual happened (no late load to arm, no bus
 *     stall, no G_FLAGS store, no code written, the GPU still running, the PC where expected), else gpu_jit_tail
 *     and the block stops.
 * A block stops where the loop could go another way: the slice's cycles spent, a jump, anything unusual after a load
 * or store. Code outside local RAM, the pipeline timing model, DRAM timing, blit memoization, the debugger, a nested
 * GPUExec stay with the interpreter. Written code: every 16-byte line of local RAM counts its writes (gpu.c), a block
 * checks its lines' count before it runs and is compiled again when it changed - (00.98) only when a word with compiled
 * code in it was written since its last check (gpu_jit_code_gen), else nothing of it can have.
 * Its code goes in EmuSuNi's shared JIT arena (port/ps4_n64.c: emusuni_jit_arena, "jaguar gpu"), within 2 GiB of
 * this code: rel32 calls, RIP-relative globals. When it is full, everything is compiled again. */
#ifdef EMUSUNI_GPU_JIT

#ifndef __ORBIS__
#include <unistd.h>   /* getpid: perf's map, the PC's only */
#endif

extern void *emusuni_jit_arena(size_t size, const char *name);

/* (00.96) 24 MiB of the arena's 32 (the DSP's 4; one core runs at a time): with 8, Doom, Wolfenstein 3D and Iron Soldier
 * filled it again and again (on the PC, Iron Soldier's 3D demo 202 times in 3700 frames, 651000 blocks compiled; with
 * 24 and a cache of 16384, a few times and some 60000). The arena is in the program's data: no memory more. */
#define GPU_JIT_SIZE        ((size_t)24 << 20)
#define GPU_JIT_MAX_INSNS   48
#define GPU_JIT_BLOCK_ROOM  65536  /* native bytes one block may take: its body (48 instructions: ~16 KB at most),
                                      its exits (4 an instruction at most, ~75 bytes each), its bytes' copy */
#define GPU_JIT_CACHE       16384  /* blocks kept by their bytes (a game's code loaded again finds its own; 896 KiB) */

typedef void (*GpuJitCode)(void);

struct GpuJitBlock
{
   GpuJitCode code;
   uint64_t gen;                     /* gpu_jit_code_gen when it was last checked: the same, nothing to check */
   uint16_t first_line, last_line;   /* the 16-byte lines of local RAM its instructions span */
   uint64_t versions;                /* their gpu_jit_line_version sum when it was last checked */
   const uint8_t *src;               /* a copy of its instructions' bytes (after its code), and how many */
   uint16_t src_off, src_len;
   uint64_t hash;                    /* of those bytes */
};

/* (EMUSUNI_GPU_* on the PC, read once.) */
#ifndef __ORBIS__
static const char *gpu_jit_env(int which)
{
   static const char *values[4];
   static int read;
   static const char *const names[4] = {"EMUSUNI_GPU_DUMP", "EMUSUNI_GPU_DUMP_PC", "EMUSUNI_GPU_PERFMAP",
                                        "EMUSUNI_GPU_JIT_STATS"};
   if (!read)
   {
      int i;
      for (i = 0; i < 4; i++)
         values[i] = getenv(names[i]);
      read = 1;
   }
   return values[which];
}
#endif

static struct GpuJitBlock *gpu_jit_at[0x800];   /* the block at each entry point now: (pc - GPU_WORK_RAM_BASE) / 2 */
static struct GpuJitBlock *gpu_jit_cache;        /* gpu_jit_cache_n of them (the heap), by their entry point and bytes */
static size_t   gpu_jit_size = GPU_JIT_SIZE;      /* (the PC: EMUSUNI_GPU_JIT_MB, EMUSUNI_GPU_JIT_CACHE, to measure) */
static unsigned gpu_jit_cache_n = GPU_JIT_CACHE;  /* a power of 2 */
static uint8_t *gpu_jit_buf;
static size_t   gpu_jit_used;
static int      gpu_jit_tried;   /* the arena asked for (once a reset) */
static uint32_t gpu_jit_full, gpu_jit_compiled;   /* this game's: the arena full, blocks compiled (the log) */
int gpu_jit_wanted;              /* the frontend's virtualjaguar_emusuni_jit (libretro.c) */

/* What a block reads and leaves: the exec loop's locals. */
static int32_t   gpu_jit_cycles;
static uint32_t  gpu_jit_last_pc, gpu_jit_last_index;
static uint32_t *gpu_jit_ldx_armed, gpu_jit_ldx_val, gpu_jit_ldx_pc;
static uint32_t  gpu_jit_jump_target;   /* a jump's target, read before its delay slot runs */
static uint32_t  gpu_jit_idle_skip;     /* the loop's idleSkipActive, for a block looping on itself */

#define GPU_JIT_X(name) gpu_opcode_##name,
static void (*const gpu_jit_handler[64])(void) = { GPU_OPCODE_LIST(GPU_JIT_X) };
#undef GPU_JIT_X

/* ---- x86-64 code ---- */
static uint8_t *gj_p;
static int      gj_fail;   /* a target out of rel32 reach: the block isn't used */

static void gj_b(uint8_t b) { *gj_p++ = b; }
static void gj_bb(uint8_t a, uint8_t b) { gj_b(a); gj_b(b); }
static void gj_d(uint32_t d) { memcpy(gj_p, &d, 4); gj_p += 4; }
/* A rel32 to target, from the end of an instruction with `after` more bytes past it. */
static void gj_rel(const void *target, int after)
{
   intptr_t disp = (intptr_t)target - (intptr_t)(gj_p + 4 + after);
   if (disp != (intptr_t)(int32_t)disp)
      gj_fail = 1;
   gj_d((uint32_t)(int32_t)disp);
}
/* [rip+t] forms */
static void gj_mov_m32(const void *t, uint32_t imm) { gj_bb(0xC7, 0x05); gj_rel(t, 4); gj_d(imm); }
static void gj_mov_m8(const void *t, uint8_t imm) { gj_bb(0xC6, 0x05); gj_rel(t, 1); gj_b(imm); }
static void gj_mov_m64_0(const void *t) { gj_b(0x48); gj_bb(0xC7, 0x05); gj_rel(t, 4); gj_d(0); }
static void gj_add_m32(const void *t, uint32_t imm) { gj_bb(0x81, 0x05); gj_rel(t, 4); gj_d(imm); }
static void gj_cmp_m32_0(const void *t) { gj_bb(0x83, 0x3D); gj_rel(t, 1); gj_b(0); }
static void gj_cmp_m8_0(const void *t) { gj_bb(0x80, 0x3D); gj_rel(t, 1); gj_b(0); }
static void gj_cmp_m32(const void *t, uint32_t imm) { gj_bb(0x81, 0x3D); gj_rel(t, 4); gj_d(imm); }
static void gj_load_eax(const void *t) { gj_bb(0x8B, 0x05); gj_rel(t, 0); }      /* mov eax, [t] */
static void gj_store_eax(const void *t) { gj_bb(0x89, 0x05); gj_rel(t, 0); }     /* mov [t], eax */
static void gj_movzx_eax_m8(const void *t) { gj_b(0x0F); gj_bb(0xB6, 0x05); gj_rel(t, 0); }
static void gj_movzx_ecx_m8(const void *t) { gj_b(0x0F); gj_bb(0xB6, 0x0D); gj_rel(t, 0); }
static void gj_setcc_m8(uint8_t cc, const void *t) { gj_b(0x0F); gj_bb(0x90 | cc, 0x05); gj_rel(t, 0); }
static void gj_store_r13_to(const void *t) { gj_b(0x44); gj_bb(0x89, 0x2D); gj_rel(t, 0); }   /* mov [t], r13d */
static void gj_store_r13(void) { gj_store_r13_to(&gpu_jit_cycles); }
static void gj_load_banks(void)
{
   gj_b(0x48); gj_bb(0x8B, 0x1D); gj_rel(&gpu_reg, 0);             /* mov rbx, [gpu_reg] */
   gj_b(0x48); gj_bb(0x8B, 0x2D); gj_rel(&gpu_alternate_reg, 0);   /* mov rbp, [gpu_alternate_reg] */
}
/* [rbx+8*?] register forms: r = the GPU register, its displacement r*4 */
static void gj_op_eax_reg(uint8_t op, uint32_t r) { gj_bb(op, 0x43); gj_b((uint8_t)(r * 4)); }      /* op eax, [rbx+d] */
static void gj_store_eax_reg(uint32_t r) { gj_bb(0x89, 0x43); gj_b((uint8_t)(r * 4)); }              /* mov [rbx+d], eax */
static void gj_call(const void *fn) { gj_b(0xE8); gj_rel(fn, 0); }
static void gj_sub_r13(uint32_t imm) { gj_b(0x41); gj_bb(0x81, 0xED); gj_d(imm); }                   /* sub r13d, imm */
static uint8_t *gj_jcc(uint8_t cc) { uint8_t *at; gj_bb(0x0F, 0x80 | cc); at = gj_p; gj_d(0); return at; }
static uint8_t *gj_jmp(void) { uint8_t *at; gj_b(0xE9); at = gj_p; gj_d(0); return at; }
static void gj_patch(uint8_t *at, const uint8_t *target)
{
   int32_t d = (int32_t)(target - (at + 4));
   memcpy(at, &d, 4);
}
#define GJ_C   0x2   /* setc / jc */
#define GJ_NC  0x3
#define GJ_Z   0x4
#define GJ_NZ  0x5
#define GJ_S   0x8
#define GJ_LE  0xE

/* ---- Exits: the state the loop would leave after an instruction ---- */
enum { GJ_EXIT_NORMAL, GJ_EXIT_SLOW };
struct GjExit
{
   uint8_t *jump_at;          /* the rel32 to patch to this exit's code */
   uint8_t  kind;
   uint8_t  pc_dynamic;       /* gpu_pc from gpu_jit_jump_target (a taken jump) */
   uint8_t  set_ds;           /* a taken jr/jump: gpu_ds_branch_target and gpu_ds_irq_dispatched */
   uint32_t pc_next, opcode, p1, p2, count, last_index, last_pc;
};
static struct GjExit gj_exits[GPU_JIT_MAX_INSNS * 4 + 8];
static unsigned gj_n_exits;

static struct GjExit *gj_exit_add(uint8_t *jump_at, uint8_t kind)
{
   struct GjExit *e = &gj_exits[gj_n_exits++];
   memset(e, 0, sizeof(*e));
   e->jump_at = jump_at;
   e->kind = kind;
   e->last_index = 0xFF;
   return e;
}

/* ---- What the loop does after a load, a store, a jump: GPUExec's tail for that instruction. Nonzero: the loop
 * could go another way. ---- */
static int gpu_jit_tail(uint32_t index, uint32_t pc_next)
{
   if (gpu_ldx_slot)
   {
      gpu_jit_ldx_armed = GPULoadOverwritePeek(&gpu_jit_ldx_val, &gpu_jit_ldx_pc);
      gpu_ldx_slot = NULL;
   }
   if (riscClockScalePct != 100u && gpu_bus_stall != 0)
   {
      uint32_t stall_scaled;
      gpu_stall_scale_accum += gpu_bus_stall * riscClockScalePct;
      stall_scaled = gpu_stall_scale_accum / 100u;
      gpu_stall_scale_accum %= 100u;
      gpu_jit_cycles -= gpu_opcode_cycles[index] + (int32_t)stall_scaled + (int32_t)gpu_pipe_core_stall;
   }
   else
      gpu_jit_cycles -= gpu_opcode_cycles[index] + (int32_t)gpu_bus_stall + (int32_t)gpu_pipe_core_stall;
   if (gpuFlagsRetireDelay)
   {
      gpuFlagsRetireDelay--;
      if (gpuFlagsRetireDelay == 0)
         GPUFlagsRetire();
   }
   if (gpu_jit_code_hit)
   {
      gpu_jit_code_hit = 0;
      return 1;
   }
   return gpu_jit_ldx_armed != NULL || gpuFlagsRetireDelay != 0 || gpu_jit_cycles <= 0 || !GPU_RUNNING
       || (gpu_control & 0x08) || gpu_pc != pc_next;
}

static uint64_t gpu_jit_flushes;   /* (the PC's counts) */
static void GPUJitFlush(void)
{
   memset(gpu_jit_at, 0, sizeof(gpu_jit_at));
   if (gpu_jit_cache)
      memset(gpu_jit_cache, 0, sizeof(*gpu_jit_cache) * gpu_jit_cache_n);
   memset(gpu_jit_code_word, 0, sizeof(gpu_jit_code_word));
   gpu_jit_flushes++;
   gpu_jit_used = 0;
   gpu_jit_code_hit = 0;
}

static uint64_t GpuJitHash(uint32_t off, uint32_t len)
{
   uint64_t h = 1469598103934665603ull ^ off;
   uint32_t i;
   for (i = 0; i < len; i++)
      h = (h ^ gpu_ram_8[off + i]) * 1099511628211ull;
   return h;
}

/* Where the block compiled at off would end (GpuJitCompile's own rules): after a jump (and its delay slot, when it
 * is inline), the end of local RAM, or GPU_JIT_MAX_INSNS instructions. */
static int gpu_jit_complex(uint32_t index);
static uint32_t GpuJitBlockEnd(uint32_t off)
{
   uint32_t pc = off, n = 0;
   for (;;)
   {
      uint32_t index, len;
      if (pc + 2 > 0x1000)
         break;
      index = gpu_ram_8[pc] >> 2;
      len = index == 38 ? 6 : 2;
      if (pc + len > 0x1000)
         break;
      if (index == 52 || index == 53)
      {
         uint32_t ds = pc + 2, ds_index, ds_len;
         if (ds + 2 <= 0x1000)
         {
            ds_index = gpu_ram_8[ds] >> 2;
            ds_len = ds_index == 38 ? 6 : 2;
            if (ds + ds_len <= 0x1000 && !gpu_jit_complex(ds_index) && ds_index != 51)
               return ds + ds_len;
         }
         return pc + len;
      }
      n++;
      pc += len;
      if (n >= GPU_JIT_MAX_INSNS)
         break;
   }
   return pc;
}

/* The cache's place for a block: its probe window's start, by its entry point and bytes. */
#define GPU_JIT_PROBE 16
static unsigned GpuJitSlot(uint32_t off, uint64_t hash)
{
   return (unsigned)((hash ^ ((uint64_t)off * 0x9E3779B97F4A7C15ull)) >> 32) & (gpu_jit_cache_n - 1);
}

/* A block compiled before with these bytes at off, or NULL. */
static struct GpuJitBlock *GpuJitFind(uint32_t off, uint32_t len, uint64_t hash)
{
   unsigned s = GpuJitSlot(off, hash), i;
   for (i = 0; i < GPU_JIT_PROBE; i++)
   {
      struct GpuJitBlock *c = &gpu_jit_cache[(s + i) & (gpu_jit_cache_n - 1)];
      if (!c->code)
         return NULL;
      if (c->src_off == off && c->src_len == len && c->hash == hash
          && memcmp(gpu_ram_8 + off, c->src, len) == 0)
         return c;
   }
   return NULL;
}

/* Room in the cache for a new block: an empty place in its window, else its first (that block forgotten). */
static struct GpuJitBlock *GpuJitPlace(uint32_t off, uint64_t hash)
{
   unsigned s = GpuJitSlot(off, hash), i;
   struct GpuJitBlock *c;
   for (i = 0; i < GPU_JIT_PROBE; i++)
   {
      c = &gpu_jit_cache[(s + i) & (gpu_jit_cache_n - 1)];
      if (!c->code)
         return c;
   }
   c = &gpu_jit_cache[s];
   if (gpu_jit_at[c->src_off >> 1] == c)
      gpu_jit_at[c->src_off >> 1] = NULL;
   memset(c, 0, sizeof(*c));
   return c;
}

/* At every GPU reset and state load: nothing compiled is kept, and the arena is asked for again (a new game's). */
static void GPUJitReset(void)
{
   GPUJitFlush();
   gpu_jit_buf = NULL;
   gpu_jit_tried = 0;
   gpu_jit_full = gpu_jit_compiled = 0;
}

static uint64_t GpuJitVersions(const struct GpuJitBlock *b)
{
   uint64_t sum = 0;
   unsigned l;
   for (l = b->first_line; l <= b->last_line; l++)
      sum += gpu_jit_line_version[l];
   return sum;
}

/* (00.98) A block checked (compiled, found again, kept): its words marked as code, so the next write to any of them
 * clears its word's mark and adds 1 to gpu_jit_code_gen (GPU_JIT_WROTE, GpuJitCodeWritten) - every block is checked
 * again before it runs then. A word written once since (data where code was) doesn't count again until a block with it
 * is checked: a block whose gen is gpu_jit_code_gen has had none of its words written since its check. */
static void GpuJitMark(const struct GpuJitBlock *b)
{
   unsigned i;
   for (i = b->src_off >> 1; i <= (unsigned)(b->src_off + b->src_len - 1) >> 1; i++)
      gpu_jit_code_word[i] = 1;
}
/* A recompiled store over code (its 4 bytes at o): the words' marks cleared, every block to be checked again. */
static void GpuJitCodeWritten(uint32_t o)
{
   gpu_jit_code_word[o >> 1] = 0;
   gpu_jit_code_word[((o + 1) & 0xFFF) >> 1] = 0;
   gpu_jit_code_word[((o + 3) & 0xFFF) >> 1] = 0;
   gpu_jit_code_gen++;
}

/* Instructions that may load or store outside local RAM, redirect the PC or stop the GPU: the loop's tail after them.
 * The rest touch registers, flags and the GPU's own units only. */
static int gpu_jit_complex(uint32_t index)
{
   return (index >= 39 && index <= 54) || (index >= 58 && index <= 61);
}
static int gpu_jit_is_load(uint32_t index)
{
   return (index >= 39 && index <= 44) || index == 58 || index == 59;
}
/* The loads and stores with a local-RAM fast path (not loadp, storep): what their handler does when the address is
 * in local RAM (GPU_CORRECT_ALIGNMENT: a long form reads and writes the aligned long; loadb and loadw read the whole
 * long; storeb and storew write a long at their own address - GPUWriteLong's SET32 -, as the handlers do). */
static int gpu_jit_local_fast(uint32_t index)
{
   return (index >= 39 && index <= 41) || index == 43 || index == 44 || index == 58 || index == 59
       || (index >= 45 && index <= 47) || index == 49 || index == 50 || index == 60 || index == 61;
}
/* Its address, into eax. */
static void gj_op_eax_reg(uint8_t op, uint32_t r);
static void gj_address(uint32_t index, uint32_t p1)
{
   switch (index)
   {
      case 43: case 49:   /* (r14 + n) */
         gj_op_eax_reg(0x8B, 14); gj_b(0x05); gj_d(gpu_convert_zero[p1] << 2); break;
      case 44: case 50:   /* (r15 + n) */
         gj_op_eax_reg(0x8B, 15); gj_b(0x05); gj_d(gpu_convert_zero[p1] << 2); break;
      case 58: case 60:   /* (r14 + Rm) */
         gj_op_eax_reg(0x8B, 14); gj_op_eax_reg(0x03, p1); break;
      case 59: case 61:   /* (r15 + Rm) */
         gj_op_eax_reg(0x8B, 15); gj_op_eax_reg(0x03, p1); break;
      default:            /* (Rm) */
         gj_op_eax_reg(0x8B, p1); break;
   }
}

/* The ALU instructions written inline here (exactly their handler's result and flags); 0 for the others. A shift by
 * 32 (shlq #0, shrq/sharq #32) calls its handler: C's shift by the width is undefined, its handler's code decides. */
static int gpu_jit_inline(uint32_t index, uint32_t p1)
{
   switch (index)
   {
      case 0: case 2: case 3: case 4: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13:
      case 16: case 17: case 30: case 31: case 34: case 35: case 36: case 37: case 38: case 57:
         return 1;
      case 24: case 25: case 27:   /* shlq, shrq, sharq */
         return p1 != 0;
      default:
         return 0;
   }
}

/* The flags after an ALU instruction, from x86's (Z, N; C when `c` is set). */
static void gj_flags(int c)
{
   if (c)
      gj_setcc_m8(GJ_C, &gpu_flag_c);
   gj_setcc_m8(GJ_Z, &gpu_flag_z);
   gj_setcc_m8(GJ_S, &gpu_flag_n);
}

/* One inline instruction: its handler's work on rbx (live bank) and rbp (the other). imm32 is a movei's value. */
static void gj_inline(uint32_t index, uint32_t p1, uint32_t p2, uint32_t imm32)
{
   uint32_t q = gpu_convert_zero[p1];
   switch (index)
   {
      case 0:   /* add */
         gj_op_eax_reg(0x8B, p2); gj_op_eax_reg(0x03, p1); gj_store_eax_reg(p2); gj_flags(1); break;
      case 2:   /* addq */
         gj_op_eax_reg(0x8B, p2); gj_b(0x05); gj_d(q); gj_store_eax_reg(p2); gj_flags(1); break;
      case 3:   /* addqt */
         gj_bb(0x81, 0x43); gj_b((uint8_t)(p2 * 4)); gj_d(q); break;
      case 4:   /* sub */
         gj_op_eax_reg(0x8B, p2); gj_op_eax_reg(0x2B, p1); gj_store_eax_reg(p2); gj_flags(1); break;
      case 6:   /* subq */
         gj_op_eax_reg(0x8B, p2); gj_b(0x2D); gj_d(q); gj_store_eax_reg(p2); gj_flags(1); break;
      case 7:   /* subqt */
         gj_bb(0x81, 0x6B); gj_b((uint8_t)(p2 * 4)); gj_d(q); break;
      case 8:   /* neg: C = RN != 0, as x86's */
         gj_op_eax_reg(0x8B, p2); gj_bb(0xF7, 0xD8); gj_store_eax_reg(p2); gj_flags(1); break;
      case 9:   /* and */
         gj_op_eax_reg(0x8B, p2); gj_op_eax_reg(0x23, p1); gj_store_eax_reg(p2); gj_flags(0); break;
      case 10:  /* or */
         gj_op_eax_reg(0x8B, p2); gj_op_eax_reg(0x0B, p1); gj_store_eax_reg(p2); gj_flags(0); break;
      case 11:  /* xor */
         gj_op_eax_reg(0x8B, p2); gj_op_eax_reg(0x33, p1); gj_store_eax_reg(p2); gj_flags(0); break;
      case 12:  /* not */
         gj_op_eax_reg(0x8B, p2); gj_bb(0xF7, 0xD0); gj_store_eax_reg(p2); gj_bb(0x85, 0xC0); gj_flags(0); break;
      case 13:  /* btst: Z = the bit is clear */
         gj_b(0x0F); gj_bb(0xBA, 0x63); gj_b((uint8_t)(p2 * 4)); gj_b((uint8_t)p1); gj_setcc_m8(GJ_NC, &gpu_flag_z);
         break;
      case 16:  /* mult: (uint16)RM * (uint16)RN */
         gj_b(0x0F); gj_bb(0xB7, 0x43); gj_b((uint8_t)(p2 * 4));   /* movzx eax, word [rbx+RN] */
         gj_b(0x0F); gj_bb(0xB7, 0x4B); gj_b((uint8_t)(p1 * 4));   /* movzx ecx, word [rbx+RM] */
         gj_b(0x0F); gj_bb(0xAF, 0xC1);                            /* imul eax, ecx */
         gj_store_eax_reg(p2); gj_bb(0x85, 0xC0); gj_flags(0); break;
      case 17:  /* imult: (int16)RN * (int16)RM */
         gj_b(0x0F); gj_bb(0xBF, 0x43); gj_b((uint8_t)(p2 * 4));   /* movsx eax, word [rbx+RN] */
         gj_b(0x0F); gj_bb(0xBF, 0x4B); gj_b((uint8_t)(p1 * 4));   /* movsx ecx, word [rbx+RM] */
         gj_b(0x0F); gj_bb(0xAF, 0xC1);
         gj_store_eax_reg(p2); gj_bb(0x85, 0xC0); gj_flags(0); break;
      case 24:  /* shlq: by 32 - IMM_1, C = RN's top bit */
         gj_op_eax_reg(0x8B, p2);
         gj_bb(0x89, 0xC2); gj_bb(0xC1, 0xEA); gj_b(31);           /* mov edx, eax; shr edx, 31 */
         gj_bb(0x88, 0x15); gj_rel(&gpu_flag_c, 0);                /* mov [c], dl */
         gj_bb(0xC1, 0xE0); gj_b((uint8_t)(32 - p1));              /* shl eax, n */
         gj_store_eax_reg(p2); gj_bb(0x85, 0xC0); gj_flags(0); break;
      case 25:  /* shrq: C = RN's bottom bit */
      case 27:  /* sharq */
         gj_op_eax_reg(0x8B, p2);
         gj_bb(0x89, 0xC2); gj_bb(0x83, 0xE2); gj_b(1);            /* mov edx, eax; and edx, 1 */
         gj_bb(0x88, 0x15); gj_rel(&gpu_flag_c, 0);
         gj_bb(0xC1, index == 25 ? 0xE8 : 0xF8); gj_b((uint8_t)q); /* shr / sar eax, n */
         gj_store_eax_reg(p2); gj_bb(0x85, 0xC0); gj_flags(0); break;
      case 30:  /* cmp */
         gj_op_eax_reg(0x8B, p2); gj_op_eax_reg(0x3B, p1); gj_flags(1); break;
      case 31:  /* cmpq: IMM_1 sign-extended */
         gj_op_eax_reg(0x8B, p2); gj_b(0x3D); gj_d((uint32_t)(int32_t)((p1 & 0x10) ? (int32_t)p1 - 32 : (int32_t)p1));
         gj_flags(1); break;
      case 34:  /* move */
         gj_op_eax_reg(0x8B, p1); gj_store_eax_reg(p2); break;
      case 35:  /* moveq */
         gj_bb(0xC7, 0x43); gj_b((uint8_t)(p2 * 4)); gj_d(p1); break;
      case 36:  /* moveta: the other bank's RN = RM */
         gj_op_eax_reg(0x8B, p1); gj_bb(0x89, 0x45); gj_b((uint8_t)(p2 * 4)); break;
      case 37:  /* movefa: RN = the other bank's RM */
         gj_bb(0x8B, 0x45); gj_b((uint8_t)(p1 * 4)); gj_store_eax_reg(p2); break;
      case 38:  /* movei */
         gj_bb(0xC7, 0x43); gj_b((uint8_t)(p2 * 4)); gj_d(imm32); break;
      default:  /* 57: nop */
         break;
   }
}

/* A movei's value: the two words after it, low word first (gpu_opcode_movei). */
static uint32_t gpu_jit_movei_value(uint32_t off)
{
   return ((uint32_t)gpu_ram_8[off] << 8) | (uint32_t)gpu_ram_8[off + 1]
        | ((uint32_t)gpu_ram_8[off + 2] << 24) | ((uint32_t)gpu_ram_8[off + 3] << 16);
}

/* Could the instruction at off take a late load's place (GPULoadOverwritePeek's own test, on the code here)? */
static int gpu_jit_ldx_possible(uint32_t off, uint32_t load_dest)
{
   uint16_t op;
   uint32_t n, p1, p2;
   uint8_t f;
   if (off + 2 > 0x1000)
      return 1;
   op = ((uint16_t)gpu_ram_8[off] << 8) | (uint16_t)gpu_ram_8[off + 1];
   n = op >> 10; p1 = (op >> 5) & 0x1F; p2 = op & 0x1F;
   f = gpu_pipe_flags[n];
   if ((f & 6) != 4 || ((f & 1) && p1 == p2) || (n >= 39 && n <= 50) || (n >= 58 && n <= 61))
      return 0;
   return p2 == load_dest;
}

/* The bit mask of the eight flag states (Z | C<<1 | N<<2) in which a jr/jump with condition cc branches. */
static uint32_t gpu_jit_branch_mask(uint32_t cc)
{
   uint32_t mask = 0, f;
   for (f = 0; f < 8; f++)
      if (branch_condition_table[cc + (f << 5)])
         mask |= 1u << f;
   return mask;
}

/* Compiles the block at off, which ends at block_end with these bytes (hash), into the cache. */
static struct GpuJitBlock *GpuJitCompile(uint32_t off, uint32_t block_end, uint64_t hash)
{
   struct GpuJitBlock *b;
   uint8_t *start, *epilogue, *body;   /* body: after the entry, where a loop to this block's start comes back */
   unsigned n = 0, i;
   uint32_t pc = off, end = off, pending = 0;
   int cont = -1;    /* the exit with the state after the last instruction that goes on, if the block ends there */
   int ended = 0;    /* ended by a jump or a tail: no exit to add */

   if (gpu_jit_used + GPU_JIT_BLOCK_ROOM + 512 > gpu_jit_size)   /* (+ its bytes' copy) */
   {
      /* Full: everything compiled again from nothing. Said in the log the 1st, 2nd, 4th, 8th... time a game. */
      GPUJitFlush();
      gpu_jit_full++;
      if ((gpu_jit_full & (gpu_jit_full - 1)) == 0)
         LOG_INF("[EmuSuNi] GPU recompiler: its %u MiB full %u times, %u blocks compiled\n",
                 (unsigned)(gpu_jit_size >> 20), gpu_jit_full, gpu_jit_compiled);
   }
#ifndef __ORBIS__
   {
      /* The PC's look at the code (EMUSUNI_GPU_DUMP=<file>, EMUSUNI_GPU_DUMP_PC=<hex>): local RAM as it is when the
       * block there is compiled, and the block's native code after it (<file>.x86). */
      const char *dump = gpu_jit_env(0), *at = gpu_jit_env(1);
      if (dump && at && strtoul(at, NULL, 16) == GPU_WORK_RAM_BASE + off)
      {
         FILE *f = fopen(dump, "wb");
         if (f)
         {
            fwrite(gpu_ram_8, 1, 0x1000, f);
            fclose(f);
         }
      }
   }
#endif
   start = gj_p = gpu_jit_buf + gpu_jit_used;
   gj_fail = 0;
   gj_n_exits = 0;
   gj_b(0x53); gj_b(0x55); gj_bb(0x41, 0x55);         /* push rbx, rbp, r13: the stack 16-byte aligned for calls */
   gj_load_banks();
   gj_b(0x44); gj_bb(0x8B, 0x2D); gj_rel(&gpu_jit_cycles, 0);   /* mov r13d, [cycles] */
   gj_mov_m32(&gpu_bus_stall, 0);
   gj_mov_m32(&gpu_pipe_core_stall, 0);
   body = gj_p;   /* (a loop back here has no stall left: an instruction with one ends the block) */
   for (;;)
   {
      uint16_t opcode;
      uint32_t index, p1, p2, len, pc_next;
      struct GjExit *e;
      if (pc + 2 > 0x1000)
         break;
      opcode = ((uint16_t)gpu_ram_8[pc] << 8) | (uint16_t)gpu_ram_8[pc + 1];
      index = opcode >> 10; p1 = (opcode >> 5) & 0x1F; p2 = opcode & 0x1F;
      len = index == 38 ? 6 : 2;   /* movei: its 32-bit value follows */
      if (pc + len > 0x1000)
         break;
      pc_next = GPU_WORK_RAM_BASE + pc + len;

      if (index == 52 || index == 53)
      {
         /* A jump. Inline when its delay slot is an inline or simple instruction. */
         uint32_t ds_off = pc + 2, ds_op, ds_index, ds_p1, ds_p2, ds_len, mask;
         int ds_ok = 0;
         if (ds_off + 2 <= 0x1000)
         {
            ds_op = ((uint32_t)gpu_ram_8[ds_off] << 8) | gpu_ram_8[ds_off + 1];
            ds_index = ds_op >> 10; ds_p1 = (ds_op >> 5) & 0x1F; ds_p2 = ds_op & 0x1F;
            ds_len = ds_index == 38 ? 6 : 2;
            ds_ok = ds_off + ds_len <= 0x1000 && !gpu_jit_complex(ds_index) && ds_index != 51;
         }
         if (ds_ok)
         {
            uint8_t *not_taken = NULL;
            uint32_t target = (uint32_t)(GPU_WORK_RAM_BASE + pc + 2
                                         + 2 * (int32_t)((p1 & 0x10) ? (p1 | 0xFFFFFFF0u) : p1));
            pending++;   /* the jump; its delay slot isn't counted */
            mask = gpu_jit_branch_mask(p2);
            if (index == 52)
            {
               gj_op_eax_reg(0x8B, p1);            /* the target register, before the delay slot runs */
               gj_store_eax(&gpu_jit_jump_target);
            }
            if (mask != 0xFF && mask != 0)
            {
               gj_movzx_eax_m8(&gpu_flag_z);
               gj_movzx_ecx_m8(&gpu_flag_c);
               gj_b(0x8D); gj_bb(0x04, 0x48);       /* lea eax, [rax+rcx*2] */
               gj_movzx_ecx_m8(&gpu_flag_n);
               gj_b(0x8D); gj_bb(0x04, 0x88);       /* lea eax, [rax+rcx*4] */
               gj_b(0xB9); gj_d(mask);              /* mov ecx, mask */
               gj_b(0x0F); gj_bb(0xA3, 0xC1);       /* bt ecx, eax */
               not_taken = gj_jcc(GJ_NC);
            }
            if (mask != 0)
            {
               /* Taken: the delay slot, then the target. */
               if (gpu_jit_inline(ds_index, ds_p1))
                  gj_inline(ds_index, ds_p1, ds_p2, ds_index == 38 ? gpu_jit_movei_value(ds_off + 2) : 0);
               else
               {
                  gj_mov_m32(&gpu_opcode_first_parameter, ds_p1);
                  gj_mov_m32(&gpu_opcode_second_parameter, ds_p2);
                  gj_call((const void *)gpu_jit_handler[ds_index]);
               }
               gj_sub_r13(gpu_opcode_cycles[index]);
               if (index == 53 && target == GPU_WORK_RAM_BASE + off)
               {
                  /* A jr to this block's own start (a wait loop): around again here while cycles are left, after
                   * what the loop does after it - its count, the delay-slot branch target, and the idle probe when
                   * the loop would call it (taken backward within its reach, idleSkipActive). */
                  uint8_t *no_probe, *out;
                  gj_add_m32(&gpu_exec_opcode_count, pending);
                  gj_mov_m32(&gpu_ds_branch_target, target);
                  gj_mov_m8(&gpu_ds_irq_dispatched, 0);
                  if (pc - off <= GPU_IDLE_MAX_JR_REACH)
                  {
                     gj_cmp_m32_0(&gpu_jit_idle_skip);
                     no_probe = gj_jcc(GJ_Z);
                     gj_b(0x44); gj_bb(0x89, 0xEF);                  /* mov edi, r13d */
                     gj_b(0xBE); gj_d(target);                       /* mov esi, head */
                     gj_b(0xBA); gj_d(GPU_WORK_RAM_BASE + pc);       /* mov edx, the jr's address */
                     gj_call((const void *)GPUIdleLoopProbe);
                     gj_b(0x41); gj_bb(0x89, 0xC5);                  /* mov r13d, eax */
                     gj_patch(no_probe, gj_p);
                  }
                  gj_b(0x45); gj_bb(0x85, 0xED);                     /* test r13d, r13d */
                  out = gj_jcc(GJ_LE);
                  gj_patch(gj_jmp(), body);
                  e = gj_exit_add(out, GJ_EXIT_NORMAL);              /* (counted, probed: last_index none) */
                  e->pc_next = target; e->opcode = opcode; e->p1 = ds_p1; e->p2 = ds_p2; e->count = 0;
               }
               else if (index == 52)
               {
                  /* A jump: to this block's own start, around again here while cycles are left (no probe after a
                   * jump); elsewhere, out. */
                  uint8_t *other, *out;
                  gj_load_eax(&gpu_jit_jump_target);
                  gj_b(0x3D); gj_d(GPU_WORK_RAM_BASE + off);         /* cmp eax, this block's start */
                  other = gj_jcc(GJ_NZ);
                  gj_add_m32(&gpu_exec_opcode_count, pending);
                  gj_mov_m32(&gpu_ds_branch_target, GPU_WORK_RAM_BASE + off);
                  gj_mov_m8(&gpu_ds_irq_dispatched, 0);
                  gj_b(0x45); gj_bb(0x85, 0xED);                     /* test r13d, r13d */
                  out = gj_jcc(GJ_LE);
                  gj_patch(gj_jmp(), body);
                  e = gj_exit_add(out, GJ_EXIT_NORMAL);
                  e->pc_next = GPU_WORK_RAM_BASE + off; e->opcode = opcode; e->p1 = ds_p1; e->p2 = ds_p2;
                  e->count = 0; e->last_index = index; e->last_pc = GPU_WORK_RAM_BASE + pc;
                  e = gj_exit_add(other, GJ_EXIT_NORMAL);
                  e->pc_next = target; e->pc_dynamic = 1; e->set_ds = 1;
                  e->opcode = opcode; e->p1 = ds_p1; e->p2 = ds_p2; e->count = pending;
                  e->last_index = index; e->last_pc = GPU_WORK_RAM_BASE + pc;
               }
               else
               {
                  e = gj_exit_add(gj_jmp(), GJ_EXIT_NORMAL);
                  e->pc_next = target; e->set_ds = 1;
                  e->opcode = opcode; e->p1 = ds_p1; e->p2 = ds_p2; e->count = pending;
                  e->last_index = index; e->last_pc = GPU_WORK_RAM_BASE + pc;
               }
            }
            if (mask != 0xFF)
            {
               if (not_taken)
                  gj_patch(not_taken, gj_p);
               gj_sub_r13(gpu_opcode_cycles[index]);
               e = gj_exit_add(gj_jmp(), GJ_EXIT_NORMAL);
               e->pc_next = GPU_WORK_RAM_BASE + pc + 2; e->opcode = opcode; e->p1 = p1; e->p2 = p2;
               e->count = pending; e->last_index = index; e->last_pc = GPU_WORK_RAM_BASE + pc;
            }
            n++;
            end = ds_off + ds_len;
            ended = 1;
            break;
         }
      }

      if (gpu_jit_inline(index, p1))
      {
         gj_inline(index, p1, p2, index == 38 ? gpu_jit_movei_value(pc + 2) : 0);
         pending++;
         gj_sub_r13(gpu_opcode_cycles[index]);
         cont = (int)gj_n_exits;
         e = gj_exit_add(gj_jcc(GJ_LE), GJ_EXIT_NORMAL);
         e->pc_next = pc_next; e->opcode = opcode; e->p1 = p1; e->p2 = p2; e->count = pending;
      }
      else if (!gpu_jit_complex(index))
      {
         /* A simple instruction through its handler: the fields it reads (move pc: the PC too). */
         gj_mov_m32(&gpu_opcode_first_parameter, p1);
         gj_mov_m32(&gpu_opcode_second_parameter, p2);
         if (index == 51)
            gj_mov_m32(&gpu_pc, GPU_WORK_RAM_BASE + pc + 2);
         gj_call((const void *)gpu_jit_handler[index]);
         pending++;
         gj_sub_r13(gpu_opcode_cycles[index]);
         cont = (int)gj_n_exits;
         e = gj_exit_add(gj_jcc(GJ_LE), GJ_EXIT_NORMAL);
         e->pc_next = pc_next; e->opcode = opcode; e->p1 = p1; e->p2 = p2; e->count = pending;
      }
      else
      {
         /* A load, a store, a jump with an unusual delay slot, mmult: the loop's start of it, its handler, the
          * tail inline when nothing unusual happened, else gpu_jit_tail and the block stops. */
         uint8_t *slow[8], *to_slow_path = NULL, *fast_join = NULL;
         unsigned n_slow = 0, k;
         gj_add_m32(&gpu_exec_opcode_count, pending + 1);
         pending = 0;
         if (gpu_jit_local_fast(index))
         {
            /* Local RAM: the handler's work inline; any other address takes the handler below. */
            uint32_t lim = index == 45 ? 0xFFC : index == 46 ? 0xFFD : 0xFFF;
            uint32_t mask = index == 45 ? 0xFFF : index == 46 ? 0xFFE : 0xFFC;
            gj_address(index, p1);
            gj_bb(0x8D, 0x88); gj_d((uint32_t)-(int32_t)GPU_WORK_RAM_BASE);   /* lea ecx, [rax - base] */
            gj_bb(0x81, 0xF9); gj_d(lim);                                      /* cmp ecx, lim */
            to_slow_path = gj_jcc(0x7);                                        /* ja: not local */
            gj_bb(0x81, 0xE1); gj_d(mask);                                     /* and ecx, mask */
            if (gpu_jit_is_load(index))
            {
               gj_b(0x48); gj_bb(0x8D, 0x15); gj_rel(gpu_ram_8, 0);          /* lea rdx, [gpu_ram_8] */
               gj_b(0x8B); gj_bb(0x04, 0x0A);                                  /* mov eax, [rdx+rcx] */
               gj_bb(0x0F, 0xC8);                                              /* bswap eax */
               gj_store_eax_reg(p2);
               gj_sub_r13(gpu_opcode_cycles[index]);
               e = gj_exit_add(gj_jcc(GJ_LE), GJ_EXIT_NORMAL);
               e->pc_next = pc_next; e->opcode = opcode; e->p1 = p1; e->p2 = p2; e->count = 0;
               fast_join = gj_jmp();
            }
            else
            {
               uint8_t *hit;
               gj_bb(0x8B, 0x53); gj_b((uint8_t)(p2 * 4));                     /* mov edx, [rbx+RN] */
               if (index == 45 || index == 46)
               {
                  gj_bb(0x81, 0xE2); gj_d(index == 45 ? 0xFF : 0xFFFF);        /* and edx, ... */
               }
               gj_bb(0x0F, 0xCA);                                              /* bswap edx */
               gj_b(0x4C); gj_bb(0x8D, 0x05); gj_rel(gpu_ram_8, 0);          /* lea r8, [gpu_ram_8] */
               gj_b(0x41); gj_bb(0x89, 0x14); gj_b(0x08);                      /* mov [r8+rcx], edx */
               /* GPU_JIT_WROTE(off), (off + 1), (off + 3): their lines' counts, and whether compiled code is in
                * their words. */
               gj_bb(0x89, 0xC8); gj_bb(0xC1, 0xE8); gj_b(4);                  /* mov eax, ecx; shr eax, 4 */
               gj_bb(0x8D, 0x51); gj_b(3); gj_bb(0xC1, 0xEA); gj_b(4);          /* lea edx, [rcx+3]; shr edx, 4 */
               gj_b(0x4C); gj_bb(0x8D, 0x05); gj_rel(gpu_jit_line_version, 0);
               gj_b(0x49); gj_bb(0xFF, 0x04); gj_b(0xC0);                      /* inc qword [r8+rax*8] */
               gj_b(0x49); gj_bb(0xFF, 0x04); gj_b(0xD0);                      /* inc qword [r8+rdx*8] */
               gj_b(0x4C); gj_bb(0x8D, 0x05); gj_rel(gpu_jit_code_word, 0);
               gj_bb(0x89, 0xC8); gj_bb(0xD1, 0xE8);                           /* mov eax, ecx; shr eax, 1 */
               gj_bb(0x8D, 0x51); gj_b(1); gj_bb(0xD1, 0xEA);                  /* lea edx, [rcx+1]; shr edx, 1 */
               gj_b(0x41); gj_b(0x0F); gj_bb(0xB6, 0x04); gj_b(0x00);          /* movzx eax, byte [r8+rax] */
               gj_b(0x41); gj_bb(0x0A, 0x04); gj_b(0x10);                      /* or al, [r8+rdx] */
               gj_bb(0x8D, 0x51); gj_b(3); gj_bb(0xD1, 0xEA);                  /* lea edx, [rcx+3]; shr edx, 1 */
               gj_b(0x41); gj_bb(0x0A, 0x04); gj_b(0x10);                      /* or al, [r8+rdx] */
               gj_bb(0x84, 0xC0);                                              /* test al, al */
               hit = gj_jcc(GJ_NZ);
               gj_sub_r13(gpu_opcode_cycles[index]);
               e = gj_exit_add(gj_jcc(GJ_LE), GJ_EXIT_NORMAL);
               e->pc_next = pc_next; e->opcode = opcode; e->p1 = p1; e->p2 = p2; e->count = 0;
               fast_join = gj_jmp();
               /* Written over compiled code: the block stops after it (what follows is compiled again), and
                * every block is checked again before it runs (GpuJitCodeWritten, as GPU_JIT_WROTE). */
               gj_patch(hit, gj_p);
               gj_bb(0x89, 0xCF);                                              /* mov edi, ecx */
               gj_call((const void *)GpuJitCodeWritten);
               gj_sub_r13(gpu_opcode_cycles[index]);
               e = gj_exit_add(gj_jmp(), GJ_EXIT_NORMAL);
               e->pc_next = pc_next; e->opcode = opcode; e->p1 = p1; e->p2 = p2; e->count = 0;
            }
            gj_patch(to_slow_path, gj_p);
         }
         gj_store_r13_to(&gpuExecSliceRemaining);   /* the loop's, for a G_CTRL store's CPU interrupt time */
         gj_mov_m32(&gpu_pc, GPU_WORK_RAM_BASE + pc + 2);
         gj_mov_m32(&gpu_instruction, opcode);
         gj_mov_m32(&gpu_opcode_first_parameter, p1);
         gj_mov_m32(&gpu_opcode_second_parameter, p2);
         gj_call((const void *)gpu_jit_handler[index]);
         gj_load_banks();
         if (index != 52 && index != 53)
         {
            if (gpu_jit_is_load(index) && !gpu_jit_ldx_possible(pc + len, p2))
               gj_mov_m64_0(&gpu_ldx_slot);      /* GPULoadOverwritePeek would find nothing */
            else
            {
               gj_b(0x48); gj_bb(0x83, 0x3D); gj_rel(&gpu_ldx_slot, 1); gj_b(0);   /* cmp qword [slot], 0 */
               slow[n_slow++] = gj_jcc(GJ_NZ);
            }
            if (!gpu_jit_is_load(index))
            {
               gj_cmp_m32_0(&gpuFlagsRetireDelay);  slow[n_slow++] = gj_jcc(GJ_NZ);
               gj_cmp_m8_0(&gpuFlagsIRQPending);    slow[n_slow++] = gj_jcc(GJ_NZ);
               gj_cmp_m8_0(&gpu_jit_code_hit);      slow[n_slow++] = gj_jcc(GJ_NZ);
               gj_load_eax(&gpu_control);
               gj_bb(0x83, 0xE0); gj_b(0x09);       /* and eax, 9 */
               gj_bb(0x83, 0xF8); gj_b(0x01);       /* cmp eax, 1: running, not single-stepped */
               slow[n_slow++] = gj_jcc(GJ_NZ);
               gj_cmp_m32(&gpu_pc, pc_next);        slow[n_slow++] = gj_jcc(GJ_NZ);
            }
            gj_cmp_m32_0(&gpu_bus_stall);           slow[n_slow++] = gj_jcc(GJ_NZ);
            gj_sub_r13(gpu_opcode_cycles[index]);
            cont = (int)gj_n_exits;
            e = gj_exit_add(gj_jcc(GJ_LE), GJ_EXIT_NORMAL);
            e->pc_next = pc_next; e->opcode = opcode; e->p1 = p1; e->p2 = p2; e->count = 0;
            {
               uint8_t *over = gj_jmp();
               for (k = 0; k < n_slow; k++)
                  gj_patch(slow[k], gj_p);
               gj_store_r13();
               gj_b(0xBF); gj_d(index);         /* mov edi, index */
               gj_b(0xBE); gj_d(pc_next);       /* mov esi, pc_next */
               gj_call((const void *)gpu_jit_tail);
               e = gj_exit_add(gj_jmp(), GJ_EXIT_SLOW);
               e->last_index = index; e->last_pc = GPU_WORK_RAM_BASE + pc;
               gj_patch(over, gj_p);
               if (fast_join)
                  gj_patch(fast_join, gj_p);
            }
         }
         else
         {
            gj_store_r13();
            gj_b(0xBF); gj_d(index);
            gj_b(0xBE); gj_d(pc_next);
            gj_call((const void *)gpu_jit_tail);
            e = gj_exit_add(gj_jmp(), GJ_EXIT_SLOW);
            e->last_index = index; e->last_pc = GPU_WORK_RAM_BASE + pc;
            n++;
            end = pc + len;
            ended = 1;
            break;
         }
      }
      n++;
      pc += len;
      end = pc;
      if (n >= GPU_JIT_MAX_INSNS)
         break;
   }
   if (n == 0)
      return NULL;
   if (!ended && cont >= 0)
   {
      /* It ends after an instruction that would go on: that instruction's exit, taken always. */
      struct GjExit *e = gj_exit_add(gj_jmp(), GJ_EXIT_NORMAL);
      uint8_t *at = e->jump_at;
      *e = gj_exits[cont];
      e->jump_at = at;
   }

   /* The exits, then the way out. */
   {
      uint8_t *to_epilogue[GPU_JIT_MAX_INSNS * 4 + 8];
      for (i = 0; i < gj_n_exits; i++)
      {
         struct GjExit *e = &gj_exits[i];
         gj_patch(e->jump_at, gj_p);
         if (e->kind == GJ_EXIT_NORMAL)
         {
            gj_store_r13();
            if (e->pc_dynamic)
            {
               gj_load_eax(&gpu_jit_jump_target);
               gj_store_eax(&gpu_pc);
            }
            else
               gj_mov_m32(&gpu_pc, e->pc_next);
            gj_mov_m32(&gpu_instruction, e->opcode);
            gj_mov_m32(&gpu_opcode_first_parameter, e->p1);
            gj_mov_m32(&gpu_opcode_second_parameter, e->p2);
            if (e->count)
               gj_add_m32(&gpu_exec_opcode_count, e->count);
            if (e->set_ds)
            {
               if (e->pc_dynamic)
                  gj_store_eax(&gpu_ds_branch_target);
               else
                  gj_mov_m32(&gpu_ds_branch_target, e->pc_next);
               gj_mov_m8(&gpu_ds_irq_dispatched, 0);
            }
         }
         gj_mov_m32(&gpu_jit_last_index, e->last_index);
         gj_mov_m32(&gpu_jit_last_pc, e->last_pc);
         to_epilogue[i] = gj_jmp();
      }
      epilogue = gj_p;
      gj_bb(0x41, 0x5D); gj_b(0x5D); gj_b(0x5B);   /* pop r13, rbp, rbx */
      gj_b(0xC3);
      for (i = 0; i < gj_n_exits; i++)
         gj_patch(to_epilogue[i], epilogue);
   }
   if (gj_fail || (size_t)(gj_p - start) > GPU_JIT_BLOCK_ROOM)
      return NULL;
#ifndef __ORBIS__
   {
      const char *dump = gpu_jit_env(0), *at = gpu_jit_env(1);
      if (dump && at && strtoul(at, NULL, 16) == GPU_WORK_RAM_BASE + off)
      {
         char path[512];
         FILE *f;
         snprintf(path, sizeof(path), "%s.x86", dump);
         f = fopen(path, "wb");
         if (f)
         {
            fwrite(start, 1, (size_t)(gj_p - start), f);
            fclose(f);
         }
      }
   }
   {
      /* perf's map of the compiled code (EMUSUNI_GPU_PERFMAP set: /tmp/perf-<pid>.map, added to: the DSP's blocks go
       * there too), a name for each block. */
      static FILE *map;
      static int map_state = -1;
      if (map_state < 0)
      {
         char path[64];
         map_state = 0;
         if (gpu_jit_env(2))
         {
            snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
            map = fopen(path, "a");
            map_state = map != NULL;
         }
      }
      if (map_state > 0)
      {
         fprintf(map, "%llx %x gpu_jit_%06x_n%u\n", (unsigned long long)(uintptr_t)start, (unsigned)(gj_p - start),
                 GPU_WORK_RAM_BASE + off, n);
         fflush(map);
      }
   }
#endif
   if (end != block_end)   /* (GpuJitBlockEnd's rules are this function's: never) */
      return NULL;
   /* A copy of its instructions' bytes after its code: a line written since, with these bytes the same (data next
    * to code, the same code loaded again), keeps the block. */
   b = GpuJitPlace(off, hash);
   b->hash = hash;
   memcpy(gj_p, gpu_ram_8 + off, end - off);
   b->src = gj_p;
   b->src_off = (uint16_t)off;
   b->src_len = (uint16_t)(end - off);
   gj_p += end - off;
   gpu_jit_used += (size_t)(gj_p - start);
   gpu_jit_used = (gpu_jit_used + 15) & ~(size_t)15;
   b->code = (GpuJitCode)(void *)start;
   b->first_line = (uint16_t)(off >> 4);
   b->last_line = (uint16_t)((end - 1) >> 4);
   b->versions = GpuJitVersions(b);
   b->gen = gpu_jit_code_gen;
   GpuJitMark(b);
   return b;
}

#ifndef __ORBIS__
/* The PC's counts (EMUSUNI_GPU_JIT_STATS set): blocks run, compiled, instructions in them and not. */
static uint64_t gpu_jit_stat_runs, gpu_jit_stat_compiles, gpu_jit_stat_insns, gpu_jit_stat_declined;
static uint64_t gpu_jit_stat_new, gpu_jit_stat_changed, gpu_jit_stat_kept, gpu_jit_stat_fast;
static uint32_t gpu_jit_stat_count0;
static uint32_t gpu_jit_stat_at[0x800];   /* compiles by entry point */
static void GpuJitStatsPrint(void)
{
   unsigned i, k;
   fprintf(stderr, "GPU JIT: %llu blocks run, %llu compiled (%llu new, %llu changed bytes), %llu kept (same bytes), "
           "%llu flushes, %llu instructions in them (%.1f a block), %llu times the interpreter's, %u instructions in "
           "all\n",
           (unsigned long long)gpu_jit_stat_runs, (unsigned long long)gpu_jit_stat_compiles,
           (unsigned long long)gpu_jit_stat_new, (unsigned long long)gpu_jit_stat_changed,
           (unsigned long long)gpu_jit_stat_kept, (unsigned long long)gpu_jit_flushes,
           (unsigned long long)gpu_jit_stat_insns,
           gpu_jit_stat_runs ? (double)gpu_jit_stat_insns / gpu_jit_stat_runs : 0.0,
           (unsigned long long)gpu_jit_stat_declined, gpu_exec_opcode_count - gpu_jit_stat_count0);
   fprintf(stderr, "GPU JIT: %llu blocks found with no code written since their check\n",
           (unsigned long long)gpu_jit_stat_fast);
   fprintf(stderr, "GPU JIT: compiled most at:");
   for (k = 0; k < 12; k++)
   {
      unsigned best = 0;
      for (i = 1; i < 0x800; i++)
         if (gpu_jit_stat_at[i] > gpu_jit_stat_at[best])
            best = i;
      if (!gpu_jit_stat_at[best])
         break;
      fprintf(stderr, " %06x x%u", GPU_WORK_RAM_BASE + best * 2, gpu_jit_stat_at[best]);
      gpu_jit_stat_at[best] = 0;
   }
   fprintf(stderr, "\n");
}
static int gpu_jit_stats = -1;
#define GPU_JIT_STAT(x) do { if (gpu_jit_stats > 0) { x; } } while (0)
#else
#define GPU_JIT_STAT(x) do { } while (0)
#endif

/* The block for the code at off now (checked against what was written there since, compiled if it must be), or
 * NULL for the interpreter. */
static struct GpuJitBlock *GpuJitBlockAt(uint32_t off)
{
   struct GpuJitBlock *b = gpu_jit_at[off >> 1];
   if (!b)
      GPU_JIT_STAT(gpu_jit_stat_new++);
   else if (b->gen == gpu_jit_code_gen)
      GPU_JIT_STAT(gpu_jit_stat_fast++);   /* (00.98) no word of it written since it was checked: nothing to check */
   else
   {
      uint64_t now = GpuJitVersions(b);
      if (now != b->versions)
      {
         /* Its lines written since: kept when its own bytes are the same (its words marked again). */
         if (memcmp(gpu_ram_8 + b->src_off, b->src, b->src_len) == 0)
         {
            b->versions = now;
            GpuJitMark(b);
            GPU_JIT_STAT(gpu_jit_stat_kept++);
         }
         else
         {
            b = NULL;
            GPU_JIT_STAT(gpu_jit_stat_changed++);
         }
      }
      if (b)
         b->gen = gpu_jit_code_gen;
   }
   if (!b)
   {
      /* Compiled before with these bytes (code loaded again), else now. */
      uint32_t end = GpuJitBlockEnd(off);
      uint64_t hash;
      if (end == off)
         return NULL;
      hash = GpuJitHash(off, end - off);
      b = GpuJitFind(off, end - off, hash);
      if (b)
      {
         b->versions = GpuJitVersions(b);
         b->gen = gpu_jit_code_gen;
         GpuJitMark(b);
      }
      else
      {
         b = GpuJitCompile(off, end, hash);
         gpu_jit_compiled++;
         GPU_JIT_STAT(gpu_jit_stat_compiles++; gpu_jit_stat_at[off >> 1]++);
         if (!b)
         {
            GPU_JIT_STAT(gpu_jit_stat_declined++);
            return NULL;
         }
      }
      gpu_jit_at[off >> 1] = b;
   }
   return b;
}

/* Blocks from gpu_pc, from GPUExec's loop when nothing it does is pending, one after another while the loop would
 * only go on to the next instruction - after each, what the loop does after its last instruction (a jr: the idle
 * probe, when idle_skip is its idleSkipActive). Nonzero when any ran (the loop's locals updated, a late load armed),
 * zero for the interpreter to run this instruction. */
static int GPUJitRun(int32_t *cycles, int idle_skip, uint32_t **ldx_armed, uint32_t *ldx_val, uint32_t *ldx_pc)
{
   int ran = 0;
#ifndef __ORBIS__
   if (gpu_jit_stats < 0)
   {
      gpu_jit_stats = gpu_jit_env(3) != NULL;
      gpu_jit_stat_count0 = gpu_exec_opcode_count;
      if (gpu_jit_stats)
         atexit(GpuJitStatsPrint);
   }
#endif

   if (gpu_in_exec != 1)
   {
      GPU_JIT_STAT(gpu_jit_stat_declined++);
      return 0;
   }
   if (!gpu_jit_buf)
   {
      if (gpu_jit_tried)
         return 0;
      gpu_jit_tried = 1;
      if (!gpu_jit_cache)
      {
#ifndef __ORBIS__
         const char *mb = getenv("EMUSUNI_GPU_JIT_MB"), *n = getenv("EMUSUNI_GPU_JIT_CACHE");
         if (mb && atoi(mb) > 0)
            gpu_jit_size = (size_t)atoi(mb) << 20;
         if (n && atoi(n) > 0)
            gpu_jit_cache_n = (unsigned)atoi(n);
#endif
         gpu_jit_cache = (struct GpuJitBlock *)calloc(gpu_jit_cache_n, sizeof(*gpu_jit_cache));
      }
      gpu_jit_buf = gpu_jit_cache ? (uint8_t *)emusuni_jit_arena(gpu_jit_size, "jaguar gpu") : NULL;
      GPUJitFlush();
      if (!gpu_jit_buf)
         return 0;
   }
   gpu_jit_cycles = *cycles;
   gpu_jit_idle_skip = idle_skip != 0;
   for (;;)
   {
      uint32_t off = gpu_pc - GPU_WORK_RAM_BASE;
      struct GpuJitBlock *b;
#ifndef __ORBIS__
      uint32_t count_before = gpu_exec_opcode_count;
#endif
      if (off >= 0x1000 || (off & 1))
      {
         GPU_JIT_STAT(gpu_jit_stat_declined++);
         break;
      }
      b = GpuJitBlockAt(off);
      if (!b)
         break;
      gpu_jit_ldx_armed = NULL;
      gpu_jit_code_hit = 0;   /* (writes before this block: its lines' counts, checked above) */
      b->code();
      ran = 1;
      GPU_JIT_STAT(gpu_jit_stat_runs++; gpu_jit_stat_insns += gpu_exec_opcode_count - count_before);
      if (idle_skip && gpu_jit_last_index == 53 && (uint32_t)(gpu_jit_last_pc - gpu_pc) <= GPU_IDLE_MAX_JR_REACH)
         gpu_jit_cycles = GPUIdleLoopProbe(gpu_jit_cycles, gpu_pc, gpu_jit_last_pc);
      if (gpu_jit_ldx_armed)
      {
         *ldx_armed = gpu_jit_ldx_armed;
         *ldx_val = gpu_jit_ldx_val;
         *ldx_pc = gpu_jit_ldx_pc;
         break;
      }
      /* The loop's own tests (single step too), the hook's: the loop's turn. */
      if (gpu_jit_cycles <= 0 || !GPU_RUNNING || (gpu_control & 0x08) || gpuFlagsRetireDelay || gpuFlagsIRQPending
          || blitMemoRecording)
         break;
   }
   *cycles = gpu_jit_cycles;
   return ran;
}

#ifndef __ORBIS__
/* The PC's comparison (EMUSUNI_GPU_TRACE=<file>): at the end of every GPUExec slice, the GPU's state - its count, PC,
 * cycles left, both register banks, flags, control - one line each. Two runs, the recompiler on and off, give the
 * same file line for line when it does what the interpreter does; the first line that differs is where it doesn't. */
static FILE *gpu_jit_trace;
static int gpu_jit_trace_state = -1;
static void GPUJitTraceSlice(int32_t cycles)
{
   uint64_t h = 1469598103934665603ull;
   unsigned i;
   if (gpu_jit_trace_state < 0)
   {
      const char *path = getenv("EMUSUNI_GPU_TRACE");
      gpu_jit_trace = path ? fopen(path, "w") : NULL;
      gpu_jit_trace_state = gpu_jit_trace != NULL;
   }
   if (!gpu_jit_trace_state)
      return;
   for (i = 0; i < 32; i++)
   {
      h = (h ^ gpu_reg_bank_0[i]) * 1099511628211ull;
      h = (h ^ gpu_reg_bank_1[i]) * 1099511628211ull;
   }
   h = (h ^ ((uint32_t)gpu_flag_z | (uint32_t)gpu_flag_n << 8 | (uint32_t)gpu_flag_c << 16)) * 1099511628211ull;
   fprintf(gpu_jit_trace, "%u %06x %d %08x %016llx\n", gpu_exec_opcode_count, gpu_pc, cycles, gpu_control,
           (unsigned long long)h);
}
#endif

#endif /* EMUSUNI_GPU_JIT */
