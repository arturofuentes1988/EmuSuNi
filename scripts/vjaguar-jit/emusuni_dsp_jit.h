/* EmuSuNi (scripts/build-atari-cores.sh, 00.96): the Jaguar DSP's recompiler, x86-64 (the PS4's CPU, the PC's).
 * GPL-3.0, as Virtual Jaguar. Included at the end of src/jerry/dsp.c, so its handlers and state are in reach.
 *
 * The GPU's recompiler (src/tom/emusuni_gpu_jit.h: how it works, read it first) for the DSP's code in its 8 KB of
 * local RAM, with the rules of the DSP's loop (DSPExecUntimed, dsp_exec_untimed.h). The machine is in the same state
 * after every instruction as with that loop - registers, flags, accumulator, memory, cycles charged, opcode count,
 * taken jr count (the idle probe's), late load write-back (#811), D_FLAGS retire countdown, and at a block's exit the
 * same dsp_pc and fields (a save state's) -, so its pictures, sound and save states are the same (the PC:
 * local/herramientas/prueba-atari, the harness's hashes with it on and off, every game there). Where the DSP differs:
 *   - a jr's or jump's delay slot is counted (dsp_exec_opcode_count) but not charged; a taken jr counts in
 *     dsp_jr_taken;
 *   - the untimed loop charges no bus stall;
 *   - an interrupt the DSP latched (IMASKCleared) is taken at the loop's top: a block stops after anything that may
 *     latch one (a load or store through its handler);
 *   - a divide whose result the indexed store right after it would store old (#811/#895, DSPDivStorePeek) is the
 *     loop's: a block ends before it;
 *   - every store to local RAM writes an aligned long (DSPWriteLong), storeb and storew too, and every load reads one.
 * Its code goes in EmuSuNi's shared JIT arena (port/ps4_n64.c: emusuni_jit_arena, "jaguar dsp"). */
#ifdef EMUSUNI_DSP_JIT

#ifndef __ORBIS__
#include <unistd.h>   /* getpid: perf's map, the PC's only */
#endif

extern void *emusuni_jit_arena(size_t size, const char *name);

#define DSP_JIT_RAM         0x2000
#define DSP_JIT_SIZE        ((size_t)4 << 20)
#define DSP_JIT_MAX_INSNS   48
#define DSP_JIT_BLOCK_ROOM  65536  /* native bytes one block may take: its body, its exits, its bytes' copy */
#define DSP_JIT_CACHE       4096   /* blocks kept by their bytes */

typedef void (*DspJitCode)(void);

struct DspJitBlock
{
   DspJitCode code;
   uint64_t gen;                     /* dsp_jit_code_gen when it was last checked: the same, nothing to check */
   uint16_t first_line, last_line;   /* the 16-byte lines of local RAM its instructions span */
   uint64_t versions;                /* their dsp_jit_line_version sum when it was last checked */
   const uint8_t *src;               /* a copy of its instructions' bytes (after its code), and how many */
   uint16_t src_off, src_len;
   uint64_t hash;                    /* of those bytes */
};

/* (EMUSUNI_DSP_* on the PC, read once.) */
#ifndef __ORBIS__
static const char *dsp_jit_env(int which)
{
   static const char *values[4];
   static int read;
   static const char *const names[4] = {"EMUSUNI_DSP_DUMP", "EMUSUNI_DSP_DUMP_PC", "EMUSUNI_DSP_PERFMAP",
                                        "EMUSUNI_DSP_JIT_STATS"};
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

static struct DspJitBlock *dsp_jit_at[DSP_JIT_RAM / 2];   /* the block at each entry point now */
static struct DspJitBlock *dsp_jit_cache;                  /* DSP_JIT_CACHE of them (the heap) */
static uint8_t *dsp_jit_buf;
static size_t   dsp_jit_used;
static int      dsp_jit_tried;   /* the arena asked for (once a reset) */
static uint32_t dsp_jit_full, dsp_jit_compiled;   /* this game's: the arena full, blocks compiled (the log) */
int dsp_jit_wanted;              /* the frontend's virtualjaguar_emusuni_jit (libretro.c) */

/* What a block reads and leaves: the exec loop's locals. */
static int32_t   dsp_jit_cycles;
static uint32_t  dsp_jit_last_pc, dsp_jit_last_index;
static uint32_t *dsp_jit_ldx_armed, dsp_jit_ldx_val, dsp_jit_ldx_pc;
static uint32_t  dsp_jit_jump_target;   /* a jump's target, read before its delay slot runs */
static uint32_t  dsp_jit_idle_skip;     /* the loop's idleSkipActive, for a block looping on itself */

/* ---- x86-64 code ---- */
static uint8_t *dj_p;
static int      dj_fail;   /* a target out of rel32 reach: the block isn't used */

static void dj_b(uint8_t b) { *dj_p++ = b; }
static void dj_bb(uint8_t a, uint8_t b) { dj_b(a); dj_b(b); }
static void dj_d(uint32_t d) { memcpy(dj_p, &d, 4); dj_p += 4; }
/* A rel32 to target, from the end of an instruction with `after` more bytes past it. */
static void dj_rel(const void *target, int after)
{
   intptr_t disp = (intptr_t)target - (intptr_t)(dj_p + 4 + after);
   if (disp != (intptr_t)(int32_t)disp)
      dj_fail = 1;
   dj_d((uint32_t)(int32_t)disp);
}
/* [rip+t] forms */
static void dj_mov_m32(const void *t, uint32_t imm) { dj_bb(0xC7, 0x05); dj_rel(t, 4); dj_d(imm); }
static void dj_mov_m64_0(const void *t) { dj_b(0x48); dj_bb(0xC7, 0x05); dj_rel(t, 4); dj_d(0); }
static void dj_add_m32(const void *t, uint32_t imm) { dj_bb(0x81, 0x05); dj_rel(t, 4); dj_d(imm); }
static void dj_inc_m32(const void *t) { dj_bb(0xFF, 0x05); dj_rel(t, 0); }
static void dj_cmp_m32_0(const void *t) { dj_bb(0x83, 0x3D); dj_rel(t, 1); dj_b(0); }
static void dj_cmp_m8_0(const void *t) { dj_bb(0x80, 0x3D); dj_rel(t, 1); dj_b(0); }
static void dj_cmp_m32(const void *t, uint32_t imm) { dj_bb(0x81, 0x3D); dj_rel(t, 4); dj_d(imm); }
static void dj_test_m8(const void *t, uint8_t imm) { dj_bb(0xF6, 0x05); dj_rel(t, 1); dj_b(imm); }
static void dj_load_eax(const void *t) { dj_bb(0x8B, 0x05); dj_rel(t, 0); }      /* mov eax, [t] */
static void dj_store_eax(const void *t) { dj_bb(0x89, 0x05); dj_rel(t, 0); }     /* mov [t], eax */
static void dj_movzx_eax_m8(const void *t) { dj_b(0x0F); dj_bb(0xB6, 0x05); dj_rel(t, 0); }
static void dj_movzx_ecx_m8(const void *t) { dj_b(0x0F); dj_bb(0xB6, 0x0D); dj_rel(t, 0); }
static void dj_setcc_m8(uint8_t cc, const void *t) { dj_b(0x0F); dj_bb(0x90 | cc, 0x05); dj_rel(t, 0); }
static void dj_store_r13_to(const void *t) { dj_b(0x44); dj_bb(0x89, 0x2D); dj_rel(t, 0); }   /* mov [t], r13d */
static void dj_store_r13(void) { dj_store_r13_to(&dsp_jit_cycles); }
static void dj_load_banks(void)
{
   dj_b(0x48); dj_bb(0x8B, 0x1D); dj_rel(&dsp_reg, 0);             /* mov rbx, [dsp_reg] */
   dj_b(0x48); dj_bb(0x8B, 0x2D); dj_rel(&dsp_alternate_reg, 0);   /* mov rbp, [dsp_alternate_reg] */
}
/* [rbx+d] register forms: r = the DSP register, its displacement r*4 */
static void dj_op_eax_reg(uint8_t op, uint32_t r) { dj_bb(op, 0x43); dj_b((uint8_t)(r * 4)); }      /* op eax, [rbx+d] */
static void dj_store_eax_reg(uint32_t r) { dj_bb(0x89, 0x43); dj_b((uint8_t)(r * 4)); }              /* mov [rbx+d], eax */
static void dj_call(const void *fn) { dj_b(0xE8); dj_rel(fn, 0); }
static void dj_sub_r13(uint32_t imm) { dj_b(0x41); dj_bb(0x81, 0xED); dj_d(imm); }                   /* sub r13d, imm */
static uint8_t *dj_jcc(uint8_t cc) { uint8_t *at; dj_bb(0x0F, 0x80 | cc); at = dj_p; dj_d(0); return at; }
static uint8_t *dj_jmp(void) { uint8_t *at; dj_b(0xE9); at = dj_p; dj_d(0); return at; }
static void dj_patch(uint8_t *at, const uint8_t *target)
{
   int32_t d = (int32_t)(target - (at + 4));
   memcpy(at, &d, 4);
}
#define DJ_C   0x2   /* setc / jc */
#define DJ_NC  0x3
#define DJ_Z   0x4
#define DJ_NZ  0x5
#define DJ_S   0x8
#define DJ_LE  0xE

/* ---- Exits: the state the loop would leave after an instruction ---- */
enum { DJ_EXIT_NORMAL, DJ_EXIT_SLOW };
struct DjExit
{
   uint8_t *jump_at;          /* the rel32 to patch to this exit's code */
   uint8_t  kind;
   uint8_t  pc_dynamic;       /* dsp_pc from dsp_jit_jump_target (a taken jump) */
   uint32_t pc_next, p1, p2, count, last_index, last_pc;
};
static struct DjExit dj_exits[DSP_JIT_MAX_INSNS * 4 + 8];
static unsigned dj_n_exits;

static struct DjExit *dj_exit_add(uint8_t *jump_at, uint8_t kind)
{
   struct DjExit *e = &dj_exits[dj_n_exits++];
   memset(e, 0, sizeof(*e));
   e->jump_at = jump_at;
   e->kind = kind;
   e->last_index = 0xFF;
   return e;
}

/* ---- What the loop does after a load, a store, a jump through its handler: DSPExecUntimed's tail for that
 * instruction (its idle probe after a jr: DSPJitRun's caller). Nonzero: the loop could go another way. ---- */
static int dsp_jit_tail(uint32_t index, uint32_t pc_next)
{
   dsp_jit_cycles -= dsp_opcode_cycles[index];
   if (dsp_ldx_slot)
   {
      dsp_jit_ldx_armed = DSPLoadOverwritePeek(&dsp_jit_ldx_val, &dsp_jit_ldx_pc);
      dsp_ldx_slot = NULL;
   }
   if (dspFlagsRetireDelay)
   {
      dspFlagsRetireDelay--;
      if (dspFlagsRetireDelay == 0)
         DSPUpdateRegisterBanks();
   }
   if (dsp_jit_code_hit)
   {
      dsp_jit_code_hit = 0;
      return 1;
   }
   return dsp_jit_ldx_armed != NULL || dspFlagsRetireDelay != 0 || IMASKCleared || dsp_jit_cycles <= 0
       || !DSP_RUNNING || dsp_pc != pc_next;
}

static uint64_t dsp_jit_flushes;   /* (the PC's counts) */
static void DSPJitFlush(void)
{
   memset(dsp_jit_at, 0, sizeof(dsp_jit_at));
   if (dsp_jit_cache)
      memset(dsp_jit_cache, 0, sizeof(*dsp_jit_cache) * DSP_JIT_CACHE);
   memset(dsp_jit_code_word, 0, sizeof(dsp_jit_code_word));
   dsp_jit_flushes++;
   dsp_jit_used = 0;
   dsp_jit_code_hit = 0;
}

static uint64_t DspJitHash(uint32_t off, uint32_t len)
{
   uint64_t h = 1469598103934665603ull ^ off;
   uint32_t i;
   for (i = 0; i < len; i++)
      h = (h ^ dsp_ram_8[off + i]) * 1099511628211ull;
   return h;
}

/* Instructions that may load or store outside local RAM, redirect the PC, stop the DSP or latch an interrupt: the
 * loop's tail after them. The rest touch registers, flags and the DSP's own units only (mmult reads local RAM). */
static int dsp_jit_complex(uint32_t index)
{
   return (index >= 39 && index <= 41) || (index >= 43 && index <= 47) || index == 49 || index == 50
       || index == 52 || index == 53 || (index >= 58 && index <= 61);
}
static int dsp_jit_is_load(uint32_t index)
{
   return (index >= 39 && index <= 41) || index == 43 || index == 44 || index == 58 || index == 59;
}

/* Is the divide at off one whose result the next instruction would store old (DSPDivStorePeek's test, on the code
 * here)? Then it is the loop's. */
static int dsp_jit_div_held(uint32_t off)
{
   uint16_t op;
   uint32_t n, p1, p2, dp2 = dsp_ram_8[off + 1] & 0x1F;
   if (off + 4 > DSP_JIT_RAM)
      return 0;   /* the next instruction outside local RAM: DSPDivStorePeek finds nothing */
   op = ((uint16_t)dsp_ram_8[off + 2] << 8) | (uint16_t)dsp_ram_8[off + 3];
   n = op >> 10; p1 = (op >> 5) & 0x1F; p2 = op & 0x1F;
   if (n != 49 && n != 50 && n != 60 && n != 61)
      return 0;
   if (p2 != dp2)
      return 0;
   if (((n == 49 || n == 60) && p2 == 14) || ((n == 50 || n == 61) && p2 == 15) || p1 == p2)
      return 0;
   return 1;
}

/* Where the block compiled at off would end (DspJitCompile's own rules): after a jump (and its delay slot, when it
 * is inline), before a held divide, the end of local RAM, or DSP_JIT_MAX_INSNS instructions. */
static uint32_t DspJitBlockEnd(uint32_t off)
{
   uint32_t pc = off, n = 0;
   for (;;)
   {
      uint32_t index, len;
      if (pc + 2 > DSP_JIT_RAM)
         break;
      index = dsp_ram_8[pc] >> 2;
      len = index == 38 ? 6 : 2;
      if (pc + len > DSP_JIT_RAM)
         break;
      if (index == 21 && dsp_jit_div_held(pc))
         break;
      if (index == 52 || index == 53)
      {
         uint32_t ds = pc + 2, ds_index, ds_len;
         if (ds + 2 <= DSP_JIT_RAM)
         {
            ds_index = dsp_ram_8[ds] >> 2;
            ds_len = ds_index == 38 ? 6 : 2;
            if (ds + ds_len <= DSP_JIT_RAM && !dsp_jit_complex(ds_index) && ds_index != 51)
               return ds + ds_len;
         }
         return pc + len;
      }
      n++;
      pc += len;
      if (n >= DSP_JIT_MAX_INSNS)
         break;
   }
   return pc;
}

/* The cache's place for a block: its probe window's start, by its entry point and bytes. */
#define DSP_JIT_PROBE 16
static unsigned DspJitSlot(uint32_t off, uint64_t hash)
{
   return (unsigned)((hash ^ ((uint64_t)off * 0x9E3779B97F4A7C15ull)) >> 32) & (DSP_JIT_CACHE - 1);
}

/* A block compiled before with these bytes at off, or NULL. */
static struct DspJitBlock *DspJitFind(uint32_t off, uint32_t len, uint64_t hash)
{
   unsigned s = DspJitSlot(off, hash), i;
   for (i = 0; i < DSP_JIT_PROBE; i++)
   {
      struct DspJitBlock *c = &dsp_jit_cache[(s + i) & (DSP_JIT_CACHE - 1)];
      if (!c->code)
         return NULL;
      if (c->src_off == off && c->src_len == len && c->hash == hash
          && memcmp(dsp_ram_8 + off, c->src, len) == 0)
         return c;
   }
   return NULL;
}

/* Room in the cache for a new block: an empty place in its window, else its first (that block forgotten). */
static struct DspJitBlock *DspJitPlace(uint32_t off, uint64_t hash)
{
   unsigned s = DspJitSlot(off, hash), i;
   struct DspJitBlock *c;
   for (i = 0; i < DSP_JIT_PROBE; i++)
   {
      c = &dsp_jit_cache[(s + i) & (DSP_JIT_CACHE - 1)];
      if (!c->code)
         return c;
   }
   c = &dsp_jit_cache[s];
   if (dsp_jit_at[c->src_off >> 1] == c)
      dsp_jit_at[c->src_off >> 1] = NULL;
   memset(c, 0, sizeof(*c));
   return c;
}

/* At every DSP reset and state load: nothing compiled is kept, and the arena is asked for again (a new game's). */
static void DSPJitReset(void)
{
   DSPJitFlush();
   dsp_jit_buf = NULL;
   dsp_jit_tried = 0;
   dsp_jit_full = dsp_jit_compiled = 0;
}

static uint64_t DspJitVersions(const struct DspJitBlock *b)
{
   uint64_t sum = 0;
   unsigned l;
   for (l = b->first_line; l <= b->last_line; l++)
      sum += dsp_jit_line_version[l];
   return sum;
}

/* (00.98) A block checked (compiled, found again, kept): its words marked as code, so the next write to any of them
 * clears its word's mark and adds 1 to dsp_jit_code_gen (DSP_JIT_WROTE, DspJitCodeWritten) - every block is checked
 * again before it runs then. A word written once since (data where code was) doesn't count again until a block with it
 * is checked: a block whose gen is dsp_jit_code_gen has had none of its words written since its check. */
static void DspJitMark(const struct DspJitBlock *b)
{
   unsigned i;
   for (i = b->src_off >> 1; i <= (unsigned)(b->src_off + b->src_len - 1) >> 1; i++)
      dsp_jit_code_word[i] = 1;
}
/* A recompiled store over code (the aligned long at o): its two words' marks cleared, every block to be checked
 * again. */
static void DspJitCodeWritten(uint32_t o)
{
   dsp_jit_code_word[o >> 1] = 0;
   dsp_jit_code_word[(o >> 1) + 1] = 0;
   dsp_jit_code_gen++;
}

/* The loads and stores with a local-RAM fast path: all of them. An address in DSP_WORK_RAM_BASE..+0x1FFF reads or
 * writes the aligned long there (DSP_CORRECT_ALIGNMENT and _STORE; loadb and loadw read the whole long, storeb and
 * storew write their byte or word as a long, as their handlers do), with nothing else to it (DSPWriteLong's mailbox
 * log is LOG_DBG, nothing in a -DNDEBUG build; DSPReadLong's HLE auto-ack is for the 68000's reads only). (r14 + n)
 * and (r15 + n) read and write (base & ~3) + 4n, the same long as (base + 4n) & ~3. */
static void dj_address(uint32_t index, uint32_t p1)
{
   switch (index)
   {
      case 43: case 49:   /* (r14 + n) */
         dj_op_eax_reg(0x8B, 14); dj_b(0x05); dj_d(dsp_convert_zero[p1] << 2); break;
      case 44: case 50:   /* (r15 + n) */
         dj_op_eax_reg(0x8B, 15); dj_b(0x05); dj_d(dsp_convert_zero[p1] << 2); break;
      case 58: case 60:   /* (r14 + Rm) */
         dj_op_eax_reg(0x8B, 14); dj_op_eax_reg(0x03, p1); break;
      case 59: case 61:   /* (r15 + Rm) */
         dj_op_eax_reg(0x8B, 15); dj_op_eax_reg(0x03, p1); break;
      default:            /* (Rm) */
         dj_op_eax_reg(0x8B, p1); break;
   }
}

/* The ALU instructions written inline here (exactly their handler's result and flags: the DSP's are the GPU's for
 * these); 0 for the others. A shift by 32 (shlq #0, shrq/sharq #32) calls its handler. */
static int dsp_jit_inline(uint32_t index, uint32_t p1)
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
static void dj_flags(int c)
{
   if (c)
      dj_setcc_m8(DJ_C, &dsp_flag_c);
   dj_setcc_m8(DJ_Z, &dsp_flag_z);
   dj_setcc_m8(DJ_S, &dsp_flag_n);
}

/* One inline instruction: its handler's work on rbx (live bank) and rbp (the other). imm32 is a movei's value. */
static void dj_inline(uint32_t index, uint32_t p1, uint32_t p2, uint32_t imm32)
{
   uint32_t q = dsp_convert_zero[p1];
   switch (index)
   {
      case 0:   /* add */
         dj_op_eax_reg(0x8B, p2); dj_op_eax_reg(0x03, p1); dj_store_eax_reg(p2); dj_flags(1); break;
      case 2:   /* addq */
         dj_op_eax_reg(0x8B, p2); dj_b(0x05); dj_d(q); dj_store_eax_reg(p2); dj_flags(1); break;
      case 3:   /* addqt */
         dj_bb(0x81, 0x43); dj_b((uint8_t)(p2 * 4)); dj_d(q); break;
      case 4:   /* sub */
         dj_op_eax_reg(0x8B, p2); dj_op_eax_reg(0x2B, p1); dj_store_eax_reg(p2); dj_flags(1); break;
      case 6:   /* subq */
         dj_op_eax_reg(0x8B, p2); dj_b(0x2D); dj_d(q); dj_store_eax_reg(p2); dj_flags(1); break;
      case 7:   /* subqt */
         dj_bb(0x81, 0x6B); dj_b((uint8_t)(p2 * 4)); dj_d(q); break;
      case 8:   /* neg: C = RN != 0, as x86's */
         dj_op_eax_reg(0x8B, p2); dj_bb(0xF7, 0xD8); dj_store_eax_reg(p2); dj_flags(1); break;
      case 9:   /* and */
         dj_op_eax_reg(0x8B, p2); dj_op_eax_reg(0x23, p1); dj_store_eax_reg(p2); dj_flags(0); break;
      case 10:  /* or */
         dj_op_eax_reg(0x8B, p2); dj_op_eax_reg(0x0B, p1); dj_store_eax_reg(p2); dj_flags(0); break;
      case 11:  /* xor */
         dj_op_eax_reg(0x8B, p2); dj_op_eax_reg(0x33, p1); dj_store_eax_reg(p2); dj_flags(0); break;
      case 12:  /* not */
         dj_op_eax_reg(0x8B, p2); dj_bb(0xF7, 0xD0); dj_store_eax_reg(p2); dj_bb(0x85, 0xC0); dj_flags(0); break;
      case 13:  /* btst: Z = the bit is clear */
         dj_b(0x0F); dj_bb(0xBA, 0x63); dj_b((uint8_t)(p2 * 4)); dj_b((uint8_t)p1); dj_setcc_m8(DJ_NC, &dsp_flag_z);
         break;
      case 16:  /* mult: (uint16)RM * (uint16)RN */
         dj_b(0x0F); dj_bb(0xB7, 0x43); dj_b((uint8_t)(p2 * 4));   /* movzx eax, word [rbx+RN] */
         dj_b(0x0F); dj_bb(0xB7, 0x4B); dj_b((uint8_t)(p1 * 4));   /* movzx ecx, word [rbx+RM] */
         dj_b(0x0F); dj_bb(0xAF, 0xC1);                            /* imul eax, ecx */
         dj_store_eax_reg(p2); dj_bb(0x85, 0xC0); dj_flags(0); break;
      case 17:  /* imult: (int16)RN * (int16)RM */
         dj_b(0x0F); dj_bb(0xBF, 0x43); dj_b((uint8_t)(p2 * 4));   /* movsx eax, word [rbx+RN] */
         dj_b(0x0F); dj_bb(0xBF, 0x4B); dj_b((uint8_t)(p1 * 4));   /* movsx ecx, word [rbx+RM] */
         dj_b(0x0F); dj_bb(0xAF, 0xC1);
         dj_store_eax_reg(p2); dj_bb(0x85, 0xC0); dj_flags(0); break;
      case 24:  /* shlq: by 32 - IMM_1, C = RN's top bit */
         dj_op_eax_reg(0x8B, p2);
         dj_bb(0x89, 0xC2); dj_bb(0xC1, 0xEA); dj_b(31);           /* mov edx, eax; shr edx, 31 */
         dj_bb(0x88, 0x15); dj_rel(&dsp_flag_c, 0);                /* mov [c], dl */
         dj_bb(0xC1, 0xE0); dj_b((uint8_t)(32 - p1));              /* shl eax, n */
         dj_store_eax_reg(p2); dj_bb(0x85, 0xC0); dj_flags(0); break;
      case 25:  /* shrq: C = RN's bottom bit */
      case 27:  /* sharq */
         dj_op_eax_reg(0x8B, p2);
         dj_bb(0x89, 0xC2); dj_bb(0x83, 0xE2); dj_b(1);            /* mov edx, eax; and edx, 1 */
         dj_bb(0x88, 0x15); dj_rel(&dsp_flag_c, 0);
         dj_bb(0xC1, index == 25 ? 0xE8 : 0xF8); dj_b((uint8_t)q); /* shr / sar eax, n */
         dj_store_eax_reg(p2); dj_bb(0x85, 0xC0); dj_flags(0); break;
      case 30:  /* cmp */
         dj_op_eax_reg(0x8B, p2); dj_op_eax_reg(0x3B, p1); dj_flags(1); break;
      case 31:  /* cmpq: IMM_1 sign-extended */
         dj_op_eax_reg(0x8B, p2); dj_b(0x3D); dj_d((uint32_t)(int32_t)((p1 & 0x10) ? (int32_t)p1 - 32 : (int32_t)p1));
         dj_flags(1); break;
      case 34:  /* move */
         dj_op_eax_reg(0x8B, p1); dj_store_eax_reg(p2); break;
      case 35:  /* moveq */
         dj_bb(0xC7, 0x43); dj_b((uint8_t)(p2 * 4)); dj_d(p1); break;
      case 36:  /* moveta: the other bank's RN = RM */
         dj_op_eax_reg(0x8B, p1); dj_bb(0x89, 0x45); dj_b((uint8_t)(p2 * 4)); break;
      case 37:  /* movefa: RN = the other bank's RM */
         dj_bb(0x8B, 0x45); dj_b((uint8_t)(p1 * 4)); dj_store_eax_reg(p2); break;
      case 38:  /* movei */
         dj_bb(0xC7, 0x43); dj_b((uint8_t)(p2 * 4)); dj_d(imm32); break;
      default:  /* 57: nop */
         break;
   }
}

/* A movei's value: the two words after it, low word first (dsp_opcode_movei). */
static uint32_t dsp_jit_movei_value(uint32_t off)
{
   return ((uint32_t)dsp_ram_8[off] << 8) | (uint32_t)dsp_ram_8[off + 1]
        | ((uint32_t)dsp_ram_8[off + 2] << 24) | ((uint32_t)dsp_ram_8[off + 3] << 16);
}

/* Could the instruction at off take a late load's place (DSPLoadOverwritePeek's own test, on the code here)? */
static int dsp_jit_ldx_possible(uint32_t off, uint32_t load_dest)
{
   uint16_t op;
   uint32_t n, p1, p2;
   if (off + 2 > DSP_JIT_RAM)
      return 1;
   op = ((uint16_t)dsp_ram_8[off] << 8) | (uint16_t)dsp_ram_8[off + 1];
   n = op >> 10; p1 = (op >> 5) & 0x1F; p2 = op & 0x1F;
   if (n != 19 && n != 34 && n != 35 && n != 37 && n != 38 && n != 51 && n != 55 && n != 56)
      return 0;
   if ((n == 34 || n == 55 || n == 56) && p1 == p2)
      return 0;
   return p2 == load_dest;
}

/* The bit mask of the eight flag states (Z | C<<1 | N<<2) in which a jr/jump with condition cc branches. */
static uint32_t dsp_jit_branch_mask(uint32_t cc)
{
   uint32_t mask = 0, f;
   for (f = 0; f < 8; f++)
      if (dsp_branch_condition_table[cc + (f << 5)])
         mask |= 1u << f;
   return mask;
}

/* Compiles the block at off, which ends at block_end with these bytes (hash), into the cache. */
static struct DspJitBlock *DspJitCompile(uint32_t off, uint32_t block_end, uint64_t hash)
{
   struct DspJitBlock *b;
   uint8_t *start, *epilogue, *body;   /* body: after the entry, where a loop to this block's start comes back */
   unsigned n = 0, i;
   uint32_t pc = off, end = off, pending = 0;
   int cont = -1;    /* the exit with the state after the last instruction that goes on, if the block ends there */
   int ended = 0;    /* ended by a jump or a tail: no exit to add */

   if (dsp_jit_used + DSP_JIT_BLOCK_ROOM + 512 > DSP_JIT_SIZE)   /* (+ its bytes' copy) */
   {
      /* Full: everything compiled again from nothing. Said in the log the 1st, 2nd, 4th, 8th... time a game. */
      DSPJitFlush();
      dsp_jit_full++;
      if ((dsp_jit_full & (dsp_jit_full - 1)) == 0)
         LOG_INF("[EmuSuNi] DSP recompiler: its %u MiB full %u times, %u blocks compiled\n",
                 (unsigned)(DSP_JIT_SIZE >> 20), dsp_jit_full, dsp_jit_compiled);
   }
#ifndef __ORBIS__
   {
      /* The PC's look at the code (EMUSUNI_DSP_DUMP=<file>, EMUSUNI_DSP_DUMP_PC=<hex>): local RAM as it is when the
       * block there is compiled, and the block's native code after it (<file>.x86). */
      const char *dump = dsp_jit_env(0), *at = dsp_jit_env(1);
      if (dump && at && strtoul(at, NULL, 16) == DSP_WORK_RAM_BASE + off)
      {
         FILE *f = fopen(dump, "wb");
         if (f)
         {
            fwrite(dsp_ram_8, 1, DSP_JIT_RAM, f);
            fclose(f);
         }
      }
   }
#endif
   start = dj_p = dsp_jit_buf + dsp_jit_used;
   dj_fail = 0;
   dj_n_exits = 0;
   dj_b(0x53); dj_b(0x55); dj_bb(0x41, 0x55);         /* push rbx, rbp, r13: the stack 16-byte aligned for calls */
   dj_load_banks();
   dj_b(0x44); dj_bb(0x8B, 0x2D); dj_rel(&dsp_jit_cycles, 0);   /* mov r13d, [cycles] */
   body = dj_p;
   for (;;)
   {
      uint16_t opcode;
      uint32_t index, p1, p2, len, pc_next;
      struct DjExit *e;
      if (pc + 2 > DSP_JIT_RAM)
         break;
      opcode = ((uint16_t)dsp_ram_8[pc] << 8) | (uint16_t)dsp_ram_8[pc + 1];
      index = opcode >> 10; p1 = (opcode >> 5) & 0x1F; p2 = opcode & 0x1F;
      len = index == 38 ? 6 : 2;   /* movei: its 32-bit value follows */
      if (pc + len > DSP_JIT_RAM)
         break;
      if (index == 21 && dsp_jit_div_held(pc))
         break;   /* the loop's (DspJitBlockEnd) */
      pc_next = DSP_WORK_RAM_BASE + pc + len;

      if (index == 52 || index == 53)
      {
         /* A jump. Inline when its delay slot is an inline or simple instruction. */
         uint32_t ds_off = pc + 2, ds_op, ds_index, ds_p1, ds_p2, ds_len, mask;
         int ds_ok = 0;
         if (ds_off + 2 <= DSP_JIT_RAM)
         {
            ds_op = ((uint32_t)dsp_ram_8[ds_off] << 8) | dsp_ram_8[ds_off + 1];
            ds_index = ds_op >> 10; ds_p1 = (ds_op >> 5) & 0x1F; ds_p2 = ds_op & 0x1F;
            ds_len = ds_index == 38 ? 6 : 2;
            ds_ok = ds_off + ds_len <= DSP_JIT_RAM && !dsp_jit_complex(ds_index) && ds_index != 51;
         }
         if (ds_ok)
         {
            uint8_t *not_taken = NULL;
            uint32_t target = (uint32_t)(DSP_WORK_RAM_BASE + pc + 2
                                         + 2 * (int32_t)((p1 & 0x10) ? (p1 | 0xFFFFFFF0u) : p1));
            pending++;   /* the jump; its delay slot, counted when it runs */
            mask = dsp_jit_branch_mask(p2);
            if (index == 52)
            {
               dj_op_eax_reg(0x8B, p1);            /* the target register, before the delay slot runs */
               dj_store_eax(&dsp_jit_jump_target);
            }
            if (mask != 0xFF && mask != 0)
            {
               dj_movzx_eax_m8(&dsp_flag_z);
               dj_movzx_ecx_m8(&dsp_flag_c);
               dj_b(0x8D); dj_bb(0x04, 0x48);       /* lea eax, [rax+rcx*2] */
               dj_movzx_ecx_m8(&dsp_flag_n);
               dj_b(0x8D); dj_bb(0x04, 0x88);       /* lea eax, [rax+rcx*4] */
               dj_b(0xB9); dj_d(mask);              /* mov ecx, mask */
               dj_b(0x0F); dj_bb(0xA3, 0xC1);       /* bt ecx, eax */
               not_taken = dj_jcc(DJ_NC);
            }
            if (mask != 0)
            {
               /* Taken: the delay slot, then the target. */
               if (index == 53)
                  dj_inc_m32(&dsp_jr_taken);
               if (dsp_jit_inline(ds_index, ds_p1))
                  dj_inline(ds_index, ds_p1, ds_p2, ds_index == 38 ? dsp_jit_movei_value(ds_off + 2) : 0);
               else
               {
                  dj_mov_m32(&dsp_opcode_first_parameter, ds_p1);
                  dj_mov_m32(&dsp_opcode_second_parameter, ds_p2);
                  dj_call((const void *)dsp_opcode[ds_index]);
               }
               dj_sub_r13(dsp_opcode_cycles[index]);
               if (index == 53 && target == DSP_WORK_RAM_BASE + off)
               {
                  /* A jr to this block's own start (a wait loop): around again here while cycles are left, after
                   * what the loop does after it - its count, and the idle probe when the loop would call it (taken
                   * backward within its reach, idleSkipActive). */
                  uint8_t *no_probe, *out;
                  dj_add_m32(&dsp_exec_opcode_count, pending + 1);
                  if (pc - off <= 30)
                  {
                     dj_cmp_m32_0(&dsp_jit_idle_skip);
                     no_probe = dj_jcc(DJ_Z);
                     dj_b(0x44); dj_bb(0x89, 0xEF);                  /* mov edi, r13d */
                     dj_b(0xBE); dj_d(target);                       /* mov esi, head */
                     dj_b(0xBA); dj_d(DSP_WORK_RAM_BASE + pc);       /* mov edx, the jr's address */
                     dj_call((const void *)DSPIdleLoopProbe);
                     dj_b(0x41); dj_bb(0x89, 0xC5);                  /* mov r13d, eax */
                     dj_patch(no_probe, dj_p);
                  }
                  dj_b(0x45); dj_bb(0x85, 0xED);                     /* test r13d, r13d */
                  out = dj_jcc(DJ_LE);
                  dj_patch(dj_jmp(), body);
                  e = dj_exit_add(out, DJ_EXIT_NORMAL);              /* (counted, probed: last_index none) */
                  e->pc_next = target; e->p1 = ds_p1; e->p2 = ds_p2; e->count = 0;
               }
               else if (index == 52)
               {
                  /* A jump: to this block's own start, around again here while cycles are left (no probe after a
                   * jump); elsewhere, out. */
                  uint8_t *other, *out;
                  dj_load_eax(&dsp_jit_jump_target);
                  dj_b(0x3D); dj_d(DSP_WORK_RAM_BASE + off);         /* cmp eax, this block's start */
                  other = dj_jcc(DJ_NZ);
                  dj_add_m32(&dsp_exec_opcode_count, pending + 1);
                  dj_b(0x45); dj_bb(0x85, 0xED);                     /* test r13d, r13d */
                  out = dj_jcc(DJ_LE);
                  dj_patch(dj_jmp(), body);
                  e = dj_exit_add(out, DJ_EXIT_NORMAL);
                  e->pc_next = DSP_WORK_RAM_BASE + off; e->p1 = ds_p1; e->p2 = ds_p2; e->count = 0;
                  e->last_index = index; e->last_pc = DSP_WORK_RAM_BASE + pc;
                  e = dj_exit_add(other, DJ_EXIT_NORMAL);
                  e->pc_next = target; e->pc_dynamic = 1;
                  e->p1 = ds_p1; e->p2 = ds_p2; e->count = pending + 1;
                  e->last_index = index; e->last_pc = DSP_WORK_RAM_BASE + pc;
               }
               else
               {
                  e = dj_exit_add(dj_jmp(), DJ_EXIT_NORMAL);
                  e->pc_next = target;
                  e->p1 = ds_p1; e->p2 = ds_p2; e->count = pending + 1;
                  e->last_index = index; e->last_pc = DSP_WORK_RAM_BASE + pc;
               }
            }
            if (mask != 0xFF)
            {
               if (not_taken)
                  dj_patch(not_taken, dj_p);
               dj_sub_r13(dsp_opcode_cycles[index]);
               e = dj_exit_add(dj_jmp(), DJ_EXIT_NORMAL);
               e->pc_next = DSP_WORK_RAM_BASE + pc + 2; e->p1 = p1; e->p2 = p2;
               e->count = pending; e->last_index = index; e->last_pc = DSP_WORK_RAM_BASE + pc;
            }
            n++;
            end = ds_off + ds_len;
            ended = 1;
            break;
         }
      }

      if (dsp_jit_inline(index, p1))
      {
         dj_inline(index, p1, p2, index == 38 ? dsp_jit_movei_value(pc + 2) : 0);
         pending++;
         dj_sub_r13(dsp_opcode_cycles[index]);
         cont = (int)dj_n_exits;
         e = dj_exit_add(dj_jcc(DJ_LE), DJ_EXIT_NORMAL);
         e->pc_next = pc_next; e->p1 = p1; e->p2 = p2; e->count = pending;
      }
      else if (!dsp_jit_complex(index))
      {
         /* A simple instruction through its handler: the fields it reads (move pc: the PC too). */
         dj_mov_m32(&dsp_opcode_first_parameter, p1);
         dj_mov_m32(&dsp_opcode_second_parameter, p2);
         if (index == 51)
            dj_mov_m32(&dsp_pc, DSP_WORK_RAM_BASE + pc + 2);
         dj_call((const void *)dsp_opcode[index]);
         pending++;
         dj_sub_r13(dsp_opcode_cycles[index]);
         cont = (int)dj_n_exits;
         e = dj_exit_add(dj_jcc(DJ_LE), DJ_EXIT_NORMAL);
         e->pc_next = pc_next; e->p1 = p1; e->p2 = p2; e->count = pending;
      }
      else
      {
         /* A load, a store, a jump with an unusual delay slot: the loop's start of it, its handler, the tail inline
          * when nothing unusual happened, else dsp_jit_tail and the block stops. */
         uint8_t *slow[8], *to_slow_path = NULL, *fast_join = NULL;
         unsigned n_slow = 0, k;
         dj_add_m32(&dsp_exec_opcode_count, pending + 1);
         pending = 0;
         if (index != 52 && index != 53)
         {
            /* Local RAM: the handler's work inline; any other address takes the handler below. */
            dj_address(index, p1);
            dj_bb(0x8D, 0x88); dj_d((uint32_t)-(int32_t)DSP_WORK_RAM_BASE);   /* lea ecx, [rax - base] */
            dj_bb(0x81, 0xF9); dj_d(0x1FFF);                                   /* cmp ecx, 0x1FFF */
            to_slow_path = dj_jcc(0x7);                                        /* ja: not local */
            dj_bb(0x81, 0xE1); dj_d(0x1FFC);                                   /* and ecx, 0x1FFC */
            if (dsp_jit_is_load(index))
            {
               dj_b(0x48); dj_bb(0x8D, 0x15); dj_rel(dsp_ram_8, 0);          /* lea rdx, [dsp_ram_8] */
               dj_b(0x8B); dj_bb(0x04, 0x0A);                                  /* mov eax, [rdx+rcx] */
               dj_bb(0x0F, 0xC8);                                              /* bswap eax */
               dj_store_eax_reg(p2);
               dj_sub_r13(dsp_opcode_cycles[index]);
               e = dj_exit_add(dj_jcc(DJ_LE), DJ_EXIT_NORMAL);
               e->pc_next = pc_next; e->p1 = p1; e->p2 = p2; e->count = 0;
               fast_join = dj_jmp();
            }
            else
            {
               uint8_t *hit;
               dj_bb(0x8B, 0x53); dj_b((uint8_t)(p2 * 4));                     /* mov edx, [rbx+RN] */
               if (index == 45 || index == 46)
               {
                  dj_bb(0x81, 0xE2); dj_d(index == 45 ? 0xFF : 0xFFFF);        /* and edx, ... */
               }
               dj_bb(0x0F, 0xCA);                                              /* bswap edx */
               dj_b(0x4C); dj_bb(0x8D, 0x05); dj_rel(dsp_ram_8, 0);          /* lea r8, [dsp_ram_8] */
               dj_b(0x41); dj_bb(0x89, 0x14); dj_b(0x08);                      /* mov [r8+rcx], edx */
               /* DSP_JIT_WROTE(off), (off + 2): its line's count, and whether compiled code is in its words. */
               dj_bb(0x89, 0xC8); dj_bb(0xC1, 0xE8); dj_b(4);                  /* mov eax, ecx; shr eax, 4 */
               dj_b(0x4C); dj_bb(0x8D, 0x05); dj_rel(dsp_jit_line_version, 0);
               dj_b(0x49); dj_bb(0xFF, 0x04); dj_b(0xC0);                      /* inc qword [r8+rax*8] */
               dj_b(0x4C); dj_bb(0x8D, 0x05); dj_rel(dsp_jit_code_word, 0);
               dj_bb(0x89, 0xCA); dj_bb(0xD1, 0xEA);                           /* mov edx, ecx; shr edx, 1 */
               dj_b(0x41); dj_b(0x0F); dj_bb(0xB7, 0x04); dj_b(0x10);          /* movzx eax, word [r8+rdx] */
               dj_bb(0x85, 0xC0);                                              /* test eax, eax */
               hit = dj_jcc(DJ_NZ);
               dj_sub_r13(dsp_opcode_cycles[index]);
               e = dj_exit_add(dj_jcc(DJ_LE), DJ_EXIT_NORMAL);
               e->pc_next = pc_next; e->p1 = p1; e->p2 = p2; e->count = 0;
               fast_join = dj_jmp();
               /* Written over compiled code: the block stops after it (what follows is compiled again), and
                * every block is checked again before it runs (DspJitCodeWritten, as DSP_JIT_WROTE). */
               dj_patch(hit, dj_p);
               dj_bb(0x89, 0xCF);                                              /* mov edi, ecx */
               dj_call((const void *)DspJitCodeWritten);
               dj_sub_r13(dsp_opcode_cycles[index]);
               e = dj_exit_add(dj_jmp(), DJ_EXIT_NORMAL);
               e->pc_next = pc_next; e->p1 = p1; e->p2 = p2; e->count = 0;
            }
            dj_patch(to_slow_path, dj_p);
         }
         dj_mov_m32(&dsp_pc, DSP_WORK_RAM_BASE + pc + 2);
         dj_mov_m32(&dsp_opcode_first_parameter, p1);
         dj_mov_m32(&dsp_opcode_second_parameter, p2);
         dj_call((const void *)dsp_opcode[index]);
         dj_load_banks();
         if (index != 52 && index != 53)
         {
            if (dsp_jit_is_load(index) && !dsp_jit_ldx_possible(pc + len, p2))
               dj_mov_m64_0(&dsp_ldx_slot);      /* DSPLoadOverwritePeek would find nothing */
            else
            {
               dj_b(0x48); dj_bb(0x83, 0x3D); dj_rel(&dsp_ldx_slot, 1); dj_b(0);   /* cmp qword [slot], 0 */
               slow[n_slow++] = dj_jcc(DJ_NZ);
            }
            dj_cmp_m32_0(&dspFlagsRetireDelay);  slow[n_slow++] = dj_jcc(DJ_NZ);
            dj_cmp_m8_0(&IMASKCleared);          slow[n_slow++] = dj_jcc(DJ_NZ);
            dj_cmp_m8_0(&dsp_jit_code_hit);      slow[n_slow++] = dj_jcc(DJ_NZ);
            dj_test_m8(&dsp_control, 0x01);      slow[n_slow++] = dj_jcc(DJ_Z);    /* stopped */
            dj_cmp_m32(&dsp_pc, pc_next);        slow[n_slow++] = dj_jcc(DJ_NZ);
            dj_sub_r13(dsp_opcode_cycles[index]);
            cont = (int)dj_n_exits;
            e = dj_exit_add(dj_jcc(DJ_LE), DJ_EXIT_NORMAL);
            e->pc_next = pc_next; e->p1 = p1; e->p2 = p2; e->count = 0;
            {
               uint8_t *over = dj_jmp();
               for (k = 0; k < n_slow; k++)
                  dj_patch(slow[k], dj_p);
               dj_store_r13();
               dj_b(0xBF); dj_d(index);         /* mov edi, index */
               dj_b(0xBE); dj_d(pc_next);       /* mov esi, pc_next */
               dj_call((const void *)dsp_jit_tail);
               e = dj_exit_add(dj_jmp(), DJ_EXIT_SLOW);
               e->last_index = index; e->last_pc = DSP_WORK_RAM_BASE + pc;
               dj_patch(over, dj_p);
               if (fast_join)
                  dj_patch(fast_join, dj_p);
            }
         }
         else
         {
            dj_store_r13();
            dj_b(0xBF); dj_d(index);
            dj_b(0xBE); dj_d(pc_next);
            dj_call((const void *)dsp_jit_tail);
            e = dj_exit_add(dj_jmp(), DJ_EXIT_SLOW);
            e->last_index = index; e->last_pc = DSP_WORK_RAM_BASE + pc;
            n++;
            end = pc + len;
            ended = 1;
            break;
         }
      }
      n++;
      pc += len;
      end = pc;
      if (n >= DSP_JIT_MAX_INSNS)
         break;
   }
   if (n == 0)
      return NULL;
   if (!ended && cont >= 0)
   {
      /* It ends after an instruction that would go on: that instruction's exit, taken always. */
      struct DjExit *e = dj_exit_add(dj_jmp(), DJ_EXIT_NORMAL);
      uint8_t *at = e->jump_at;
      *e = dj_exits[cont];
      e->jump_at = at;
   }

   /* The exits, then the way out. */
   {
      uint8_t *to_epilogue[DSP_JIT_MAX_INSNS * 4 + 8];
      for (i = 0; i < dj_n_exits; i++)
      {
         struct DjExit *e = &dj_exits[i];
         dj_patch(e->jump_at, dj_p);
         if (e->kind == DJ_EXIT_NORMAL)
         {
            dj_store_r13();
            if (e->pc_dynamic)
            {
               dj_load_eax(&dsp_jit_jump_target);
               dj_store_eax(&dsp_pc);
            }
            else
               dj_mov_m32(&dsp_pc, e->pc_next);
            dj_mov_m32(&dsp_opcode_first_parameter, e->p1);
            dj_mov_m32(&dsp_opcode_second_parameter, e->p2);
            if (e->count)
               dj_add_m32(&dsp_exec_opcode_count, e->count);
         }
         dj_mov_m32(&dsp_jit_last_index, e->last_index);
         dj_mov_m32(&dsp_jit_last_pc, e->last_pc);
         to_epilogue[i] = dj_jmp();
      }
      epilogue = dj_p;
      dj_bb(0x41, 0x5D); dj_b(0x5D); dj_b(0x5B);   /* pop r13, rbp, rbx */
      dj_b(0xC3);
      for (i = 0; i < dj_n_exits; i++)
         dj_patch(to_epilogue[i], epilogue);
   }
   if (dj_fail || (size_t)(dj_p - start) > DSP_JIT_BLOCK_ROOM)
      return NULL;
#ifndef __ORBIS__
   {
      const char *dump = dsp_jit_env(0), *at = dsp_jit_env(1);
      if (dump && at && strtoul(at, NULL, 16) == DSP_WORK_RAM_BASE + off)
      {
         char path[512];
         FILE *f;
         snprintf(path, sizeof(path), "%s.x86", dump);
         f = fopen(path, "wb");
         if (f)
         {
            fwrite(start, 1, (size_t)(dj_p - start), f);
            fclose(f);
         }
      }
   }
   {
      /* perf's map of the compiled code (EMUSUNI_DSP_PERFMAP set: /tmp/perf-<pid>.map, added to), a name for each
       * block. */
      static FILE *map;
      static int map_state = -1;
      if (map_state < 0)
      {
         char path[64];
         map_state = 0;
         if (dsp_jit_env(2))
         {
            snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
            map = fopen(path, "a");
            map_state = map != NULL;
         }
      }
      if (map_state > 0)
      {
         fprintf(map, "%llx %x dsp_jit_%06x_n%u\n", (unsigned long long)(uintptr_t)start, (unsigned)(dj_p - start),
                 DSP_WORK_RAM_BASE + off, n);
         fflush(map);
      }
   }
#endif
   if (end != block_end)   /* (DspJitBlockEnd's rules are this function's: never) */
      return NULL;
   /* A copy of its instructions' bytes after its code: a line written since, with these bytes the same, keeps the
    * block. */
   b = DspJitPlace(off, hash);
   b->hash = hash;
   memcpy(dj_p, dsp_ram_8 + off, end - off);
   b->src = dj_p;
   b->src_off = (uint16_t)off;
   b->src_len = (uint16_t)(end - off);
   dj_p += end - off;
   dsp_jit_used += (size_t)(dj_p - start);
   dsp_jit_used = (dsp_jit_used + 15) & ~(size_t)15;
   b->code = (DspJitCode)(void *)start;
   b->first_line = (uint16_t)(off >> 4);
   b->last_line = (uint16_t)((end - 1) >> 4);
   b->versions = DspJitVersions(b);
   b->gen = dsp_jit_code_gen;
   DspJitMark(b);
   return b;
}

#ifndef __ORBIS__
/* The PC's counts (EMUSUNI_DSP_JIT_STATS set): blocks run, compiled, instructions in them and not. */
static uint64_t dsp_jit_stat_runs, dsp_jit_stat_compiles, dsp_jit_stat_insns, dsp_jit_stat_declined;
static uint64_t dsp_jit_stat_new, dsp_jit_stat_changed, dsp_jit_stat_kept, dsp_jit_stat_fast;
static uint32_t dsp_jit_stat_count0;
static uint32_t dsp_jit_stat_at[DSP_JIT_RAM / 2];   /* compiles by entry point */
static void DspJitStatsPrint(void)
{
   unsigned i, k;
   fprintf(stderr, "DSP JIT: %llu blocks run, %llu compiled (%llu new, %llu changed bytes), %llu kept (same bytes), "
           "%llu flushes, %llu instructions in them (%.1f a block), %llu times the interpreter's, %u instructions in "
           "all\n",
           (unsigned long long)dsp_jit_stat_runs, (unsigned long long)dsp_jit_stat_compiles,
           (unsigned long long)dsp_jit_stat_new, (unsigned long long)dsp_jit_stat_changed,
           (unsigned long long)dsp_jit_stat_kept, (unsigned long long)dsp_jit_flushes,
           (unsigned long long)dsp_jit_stat_insns,
           dsp_jit_stat_runs ? (double)dsp_jit_stat_insns / dsp_jit_stat_runs : 0.0,
           (unsigned long long)dsp_jit_stat_declined, dsp_exec_opcode_count - dsp_jit_stat_count0);
   fprintf(stderr, "DSP JIT: %llu blocks found with no code written since their check\n",
           (unsigned long long)dsp_jit_stat_fast);
   fprintf(stderr, "DSP JIT: compiled most at:");
   for (k = 0; k < 12; k++)
   {
      unsigned best = 0;
      for (i = 1; i < DSP_JIT_RAM / 2; i++)
         if (dsp_jit_stat_at[i] > dsp_jit_stat_at[best])
            best = i;
      if (!dsp_jit_stat_at[best])
         break;
      fprintf(stderr, " %06x x%u", DSP_WORK_RAM_BASE + best * 2, dsp_jit_stat_at[best]);
      dsp_jit_stat_at[best] = 0;
   }
   fprintf(stderr, "\n");
}
static int dsp_jit_stats = -1;
#define DSP_JIT_STAT(x) do { if (dsp_jit_stats > 0) { x; } } while (0)
#else
#define DSP_JIT_STAT(x) do { } while (0)
#endif

/* The block for the code at off now (checked against what was written there since, compiled if it must be), or
 * NULL for the interpreter. */
static struct DspJitBlock *DspJitBlockAt(uint32_t off)
{
   struct DspJitBlock *b = dsp_jit_at[off >> 1];
   if (!b)
      DSP_JIT_STAT(dsp_jit_stat_new++);
   else if (b->gen == dsp_jit_code_gen)
      DSP_JIT_STAT(dsp_jit_stat_fast++);   /* (00.98) no word of it written since it was checked: nothing to check */
   else
   {
      uint64_t now = DspJitVersions(b);
      if (now != b->versions)
      {
         /* Its lines written since: kept when its own bytes are the same (its words marked again). */
         if (memcmp(dsp_ram_8 + b->src_off, b->src, b->src_len) == 0)
         {
            b->versions = now;
            DspJitMark(b);
            DSP_JIT_STAT(dsp_jit_stat_kept++);
         }
         else
         {
            b = NULL;
            DSP_JIT_STAT(dsp_jit_stat_changed++);
         }
      }
      if (b)
         b->gen = dsp_jit_code_gen;
   }
   if (!b)
   {
      /* Compiled before with these bytes (code loaded again), else now. */
      uint32_t end = DspJitBlockEnd(off);
      uint64_t hash;
      if (end == off)
         return NULL;
      hash = DspJitHash(off, end - off);
      b = DspJitFind(off, end - off, hash);
      if (b)
      {
         b->versions = DspJitVersions(b);
         b->gen = dsp_jit_code_gen;
         DspJitMark(b);
      }
      else
      {
         b = DspJitCompile(off, end, hash);
         dsp_jit_compiled++;
         DSP_JIT_STAT(dsp_jit_stat_compiles++; dsp_jit_stat_at[off >> 1]++);
         if (!b)
         {
            DSP_JIT_STAT(dsp_jit_stat_declined++);
            return NULL;
         }
      }
      dsp_jit_at[off >> 1] = b;
   }
   return b;
}

/* Blocks from dsp_pc, from DSPExecUntimed's loop when nothing it does is pending, one after another while the loop
 * would only go on to the next instruction - after each, what the loop does after its last instruction (a jr: the
 * idle probe, when idle_skip is its idleSkipActive). Nonzero when any ran (the loop's locals updated, a late load
 * armed), zero for the interpreter to run this instruction. */
static int DSPJitRun(int32_t *cycles, int idle_skip, uint32_t **ldx_armed, uint32_t *ldx_val, uint32_t *ldx_pc)
{
   int ran = 0;
#ifndef __ORBIS__
   if (dsp_jit_stats < 0)
   {
      dsp_jit_stats = dsp_jit_env(3) != NULL;
      dsp_jit_stat_count0 = dsp_exec_opcode_count;
      if (dsp_jit_stats)
         atexit(DspJitStatsPrint);
   }
#endif

   if (dsp_in_exec != 1)
   {
      DSP_JIT_STAT(dsp_jit_stat_declined++);
      return 0;
   }
   if (!dsp_jit_buf)
   {
      if (dsp_jit_tried)
         return 0;
      dsp_jit_tried = 1;
      if (!dsp_jit_cache)
         dsp_jit_cache = (struct DspJitBlock *)calloc(DSP_JIT_CACHE, sizeof(*dsp_jit_cache));
      dsp_jit_buf = dsp_jit_cache ? (uint8_t *)emusuni_jit_arena(DSP_JIT_SIZE, "jaguar dsp") : NULL;
      DSPJitFlush();
      if (!dsp_jit_buf)
         return 0;
   }
   dsp_jit_cycles = *cycles;
   dsp_jit_idle_skip = idle_skip != 0;
   for (;;)
   {
      uint32_t off = dsp_pc - DSP_WORK_RAM_BASE;
      struct DspJitBlock *b;
#ifndef __ORBIS__
      uint32_t count_before = dsp_exec_opcode_count;
#endif
      if (off >= DSP_JIT_RAM || (off & 1))
      {
         DSP_JIT_STAT(dsp_jit_stat_declined++);
         break;
      }
      b = DspJitBlockAt(off);
      if (!b)
         break;
      dsp_jit_ldx_armed = NULL;
      dsp_jit_code_hit = 0;   /* (writes before this block: its lines' counts, checked above) */
      b->code();
      ran = 1;
      DSP_JIT_STAT(dsp_jit_stat_runs++; dsp_jit_stat_insns += dsp_exec_opcode_count - count_before);
      if (idle_skip && dsp_jit_last_index == 53 && (uint32_t)(dsp_jit_last_pc - dsp_pc) <= 30)
         dsp_jit_cycles = DSPIdleLoopProbe(dsp_jit_cycles, dsp_pc, dsp_jit_last_pc);
      if (dsp_jit_ldx_armed)
      {
         *ldx_armed = dsp_jit_ldx_armed;
         *ldx_val = dsp_jit_ldx_val;
         *ldx_pc = dsp_jit_ldx_pc;
         break;
      }
      /* The loop's own test, its interrupts, a D_FLAGS store retiring, the hook's: the loop's turn. */
      if (dsp_jit_cycles <= 0 || !DSP_RUNNING || IMASKCleared || dspFlagsRetireDelay || blitMemoRecording)
         break;
   }
   *cycles = dsp_jit_cycles;
   return ran;
}

#ifndef __ORBIS__
/* The PC's comparison (EMUSUNI_DSP_TRACE=<file>): at the end of every DSPExecUntimed slice, the DSP's state - its
 * count, PC, cycles left, control, a hash of both register banks, flags and accumulator - one line each. Two runs,
 * the recompiler on and off, give the same file line for line when it does what the interpreter does. */
static FILE *dsp_jit_trace;
static int dsp_jit_trace_state = -1;
static void DSPJitTraceSlice(int32_t cycles)
{
   uint64_t h = 1469598103934665603ull;
   unsigned i;
   if (dsp_jit_trace_state < 0)
   {
      const char *path = getenv("EMUSUNI_DSP_TRACE");
      dsp_jit_trace = path ? fopen(path, "w") : NULL;
      dsp_jit_trace_state = dsp_jit_trace != NULL;
   }
   if (!dsp_jit_trace_state)
      return;
   for (i = 0; i < 32; i++)
   {
      h = (h ^ dsp_reg_bank_0[i]) * 1099511628211ull;
      h = (h ^ dsp_reg_bank_1[i]) * 1099511628211ull;
   }
   h = (h ^ ((uint32_t)dsp_flag_z | (uint32_t)dsp_flag_n << 8 | (uint32_t)dsp_flag_c << 16)) * 1099511628211ull;
   h = (h ^ dsp_acc) * 1099511628211ull;
   fprintf(dsp_jit_trace, "%u %06x %d %08x %016llx\n", dsp_exec_opcode_count, dsp_pc, cycles, dsp_control,
           (unsigned long long)h);
}
#endif

#endif /* EMUSUNI_DSP_JIT */
