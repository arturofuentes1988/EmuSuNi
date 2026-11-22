# EmuSuNi's JIT improvements for the 3DS core (the performance specialist's; rendimiento-jit-estudio.md).
# Run by scripts/build-3ds-core.sh inside its patch step, after all its own patches (one exec line there): it uses that
# step's patch() and addfile(), on the files as they are patched by then. Each improvement has its switch, a file in
# /data/EmuSuNi on the PS4 (the word off goes back to what was before) or a variable on the PC (0 or off).

# --- 01.41: 3ds-jit-native-calls (the study's idea 4). ARM's runtime library (RVCT's, which 3DS games are built with)
# copies memory and divides integers in code of its own: the ARM11 has no division instruction, and its memcpy runs
# 32 bytes at a time through LDM/STM. In Castlevania: Mirror of Fate's busy part memcpy is 5.9 % of the game's code and
# the division ~2 % (the PC's profile). They are recognised by their first words where a block starts (any game with
# that library, no list of games or addresses) and run natively:
# - __aeabi_idivmod, the SIGNED division: its entry (ORRS r3, r0, r1; BMI: a fast path when neither is negative) and its
#   signed path (+0x140, the BMI's target: signs taken off, divided, put back). 01.44: both are dynarmic's SDIV (01.41
#   took the entry for the unsigned division and lost the sign: the circle pad's left and down went right and up in
#   SM3DL and Luigi's Mansion 2). Quotient in r0, remainder in r1 (the dividend's sign); dividing by 0 gives 0 and the
#   dividend (the library calls __aeabi_idiv0, whose 0 it returns). The unsigned division isn't recognised.
# - memcpy (__aeabi_memcpy, r0 destination, r1 source, r2 bytes): EmuSuNiNativeMemcpy, called straight from the block
#   (dynarmic's CallHostFunction, as its memory fallbacks are called: no tick sync, no MXCSR switch; an SVC took 3 calls
#   of the frontend's and cost as much as it saved, ~800 copies a frame of ~160 bytes in Castlevania): the pages the
#   fast memory maps (plain memory) copied at once, any other through Azahar's CopyBlock (the GPU's cache flushed or
#   invalidated once a page, with the async GPU thread's queue, where the JIT's fallbacks did it once a word); r0 and r1
#   end past the copy as every path of the library's leaves them (the 8 bytes or fewer too); 40 ticks of the 3DS CPU a
#   copy (its loop's for ~160 bytes). Its first 10 words: memmove's backward copy starts with the same 8 (with r0 and
#   r1 at the ends), the 10th is the forward copy's post-indexed load.
# Every 600 frames a line with the native memcpy's calls and bytes; each routine logged once as it is found. On the PC:
# EMUSUNI_3DS_JIT_NATIVE_CALLS=0 for the old way.
patch('src/core/CMakeLists.txt', r'''
    target_link_libraries(citra_core PRIVATE dynarmic)
''', r'''
    target_link_libraries(citra_core PRIVATE dynarmic)
    if (EMUSUNI_PORT)
        # EmuSuNi (01.41, scripts/3ds-jit-patches.py): arm_dynarmic.cpp writes dynarmic IR (3ds-jit-native-calls).
        target_link_libraries(citra_core PRIVATE merry::mcl)
    endif()
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
#include <dynarmic/interface/A32/a32.h>
''', r'''
#include <dynarmic/interface/A32/a32.h>
#ifdef EMUSUNI_PORT
#include <algorithm>
#include <array>
#include <unordered_set>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/ir/terminal.h>
#endif
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
namespace Core {

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
''', r'''
namespace Core {

#ifdef EMUSUNI_PORT
// (01.41, 3ds-jit-native-calls, scripts/3ds-jit-patches.py) ARM's runtime library's memcpy and divisions run natively
// unless /data/EmuSuNi/3ds-jit-native-calls says off (read with each JIT); the system, for memcpy's slow path.
static bool emusuni_native_calls = false;
static Core::System* emusuni_native_system = nullptr;

// (01.41) The game's memcpy, called straight from its block (dynarmic's CallHostFunction): the destination in the low
// 32 bits of the first argument and the source in the high, the bytes, the block's JIT's page table (its pointers).
static void EmuSuNiNativeMemcpy(std::uint64_t dst_src, std::uint64_t size, std::uint64_t table) {
    VAddr dst = static_cast<VAddr>(dst_src);
    VAddr src = static_cast<VAddr>(dst_src >> 32);
    std::uint32_t left = static_cast<std::uint32_t>(size);
    EmuSuNi::Stats::counts[EmuSuNi::Stats::NativeMemcpyCalls]++;
    EmuSuNi::Stats::counts[EmuSuNi::Stats::NativeMemcpyBytes] += left;
    std::uint8_t* const* const raw = reinterpret_cast<std::uint8_t* const*>(table);
    while (left != 0) {
        const std::uint32_t chunk =
            std::min({left, static_cast<std::uint32_t>(Memory::CITRA_PAGE_SIZE - (src & Memory::CITRA_PAGE_MASK)),
                      static_cast<std::uint32_t>(Memory::CITRA_PAGE_SIZE - (dst & Memory::CITRA_PAGE_MASK))});
        std::uint8_t* const from = raw[src >> Memory::CITRA_PAGE_BITS];
        std::uint8_t* const to = raw[dst >> Memory::CITRA_PAGE_BITS];
        if (from != nullptr && to != nullptr) {
            // Plain memory, as the fast memory maps it; forwards, a byte at a time where they overlap.
            const std::uint8_t* s = from + (src & Memory::CITRA_PAGE_MASK);
            std::uint8_t* d = to + (dst & Memory::CITRA_PAGE_MASK);
            if (d > s && d < s + chunk) {
                for (std::uint32_t i = 0; i < chunk; i++) {
                    d[i] = s[i];
                }
            } else {
                std::memmove(d, s, chunk);
            }
        } else {
            // A page the GPU's cache holds (or none): Azahar's own copy, the source's range flushed and the
            // destination's invalidated once a chunk (RasterizerFlushVirtualRegion, with the async GPU thread's
            // queue), not once a word as the JIT's fallbacks would.
            emusuni_native_system->Memory().CopyBlock(*emusuni_native_system->Kernel().GetCurrentProcess(), dst, src,
                                                      chunk);
        }
        src += chunk;
        dst += chunk;
        left -= chunk;
    }
}
#endif

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }
''', r'''
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

#ifdef EMUSUNI_PORT
    // (01.41, 3ds-jit-native-calls, scripts/3ds-jit-patches.py) Where a block starts at RVCT's __aeabi_idivmod (its
    // entry or its signed path, 8 words) or memcpy (10 words), the block is the native routine and a return to lr.
    bool PreCodeReadHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        if (!emusuni_native_calls || is_thumb || !ir.block.empty() ||
            Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC() != pc) {
            return true;
        }
        // (01.44) __aeabi_idivmod's entry: ORRS r3, r0, r1; BMI to its signed path (a negative operand), then the
        // unsigned fast path. 01.41 named it __aeabi_uidivmod and divided without sign.
        static constexpr std::array<std::uint32_t, 8> sdiv_entry{0xE1903001, 0x4A00004D, 0xE07130A0, 0x3A000044,
                                                                 0xE3B02000, 0xE0713220, 0x3A000034, 0xE0713420};
        // __aeabi_idivmod's signed path (the entry +0x140): ANDS r2, r1, #0x80000000; RSBMI r1; EORS r12; RSBCS r0.
        static constexpr std::array<std::uint32_t, 8> sdiv_signed{0xE2112102, 0x42611000, 0xE032C040, 0x22600000,
                                                                  0xE0713220, 0x3A00001D, 0xE0713420, 0x3A00000F};
        // memcpy: its size dispatch, then the 3-byte case's post-indexed LDRH (memmove's backward copy has the same
        // first 8 words and a pre-indexed LDRH r2, [r1, #-2]! there).
        static constexpr std::array<std::uint32_t, 10> copy{0xE3520008, 0x8A00002F, 0x0A000029, 0xE3520004,
                                                            0x8A00000D, 0x0A000019, 0xE3520002, 0x3A000005,
                                                            0x0A000013, 0xE0D120B2};
        const auto first = memory.Read32OrNullopt(pc);
        if (!first) {
            return true;
        }
        const std::uint32_t* kind = *first == sdiv_entry[0]    ? sdiv_entry.data()
                                    : *first == sdiv_signed[0] ? sdiv_signed.data()
                                    : *first == copy[0]        ? copy.data()
                                                               : nullptr;
        if (kind == nullptr) {
            return true;
        }
        const bool is_copy = kind == copy.data();
        const std::size_t words = is_copy ? copy.size() : sdiv_entry.size();
        for (std::size_t i = 1; i < words; i++) {
            const auto word = memory.Read32OrNullopt(pc + static_cast<VAddr>(4 * i));
            if (!word || *word != kind[i]) {
                return true;
            }
        }
        const char* name = is_copy                     ? "memcpy"
                           : kind == sdiv_entry.data() ? "__aeabi_idivmod (entry)"
                                                       : "__aeabi_idivmod (signed path)";
        {
            static std::unordered_set<VAddr> logged;
            if (logged.insert(pc).second) {
                LOG_INFO(Core_ARM11,
                         "EmuSuNi: native calls: the game's {} at {:08X} runs natively (3ds-jit-native-calls)", name,
                         pc);
            }
        }
        using Dynarmic::A32::Reg;
        if (is_copy) {
            if (!parent.current_page_table) {
                return true;
            }
            emusuni_native_system = &parent.system;
            const Dynarmic::IR::U32 dst = ir.GetRegister(Reg::R0);
            const Dynarmic::IR::U32 src = ir.GetRegister(Reg::R1);
            const Dynarmic::IR::U32 size = ir.GetRegister(Reg::R2);
            const auto table = reinterpret_cast<std::uint64_t>(parent.current_page_table->GetPointerArray().data());
            ir.CallHostFunction(&EmuSuNiNativeMemcpy, ir.Pack2x32To1x64(dst, src), ir.ZeroExtendWordToLong(size),
                                ir.Imm64(table));
            // r0 and r1 past the copy, as the library's post-indexed stores leave them.
            ir.SetRegister(Reg::R0, ir.Add(dst, size));
            ir.SetRegister(Reg::R1, ir.Add(src, size));
            ir.BXWritePC(ir.GetRegister(Reg::LR));
            ir.SetTerm(Dynarmic::IR::Term::PopRSBHint{});
            ir.block.CycleCount() += 40; // (a copy of ~160 bytes: its 32-byte loop's turns)
        } else {
            const Dynarmic::IR::U32 dividend = ir.GetRegister(Reg::R0);
            const Dynarmic::IR::U32 divisor = ir.GetRegister(Reg::R1);
            // (01.44) Signed at both: the entry's fast path is for operands that aren't negative, where it's the same.
            const Dynarmic::IR::U32 quotient = ir.SignedDiv(dividend, divisor);
            ir.SetRegister(Reg::R0, quotient);
            ir.SetRegister(Reg::R1, ir.Sub(dividend, ir.Mul(quotient, divisor)));
            ir.BXWritePC(ir.GetRegister(Reg::LR));
            ir.SetTerm(Dynarmic::IR::Term::PopRSBHint{});
            ir.block.CycleCount() += 30; // (the library's loop: ~10-60 instructions)
        }
        // (the block's range: the words recognised)
        ir.current_location = ir.current_location.AdvancePC(static_cast<int>(4 * words));
        return false;
    }
#endif
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    if (Dynarmic::A32::emusuni_idle_loops) {
        LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the game's idle loops skipped (3ds-cpu-idle-loops)", GetID());
    }
''', r'''
    if (Dynarmic::A32::emusuni_idle_loops) {
        LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the game's idle loops skipped (3ds-cpu-idle-loops)", GetID());
    }
    // (01.41, scripts/3ds-jit-patches.py) The game's memcpy and divisions run natively unless
    // /data/EmuSuNi/3ds-jit-native-calls says off (DynarmicUserCallbacks::PreCodeReadHook).
    emusuni_native_calls = EmuSuNi::Async::TestFile("3ds-jit-native-calls", "EMUSUNI_3DS_JIT_NATIVE_CALLS", 0) >= 0;
    LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the game's memcpy and divisions {} (3ds-jit-native-calls)", GetID(),
             emusuni_native_calls ? "run natively" : "translated as they are (off)");
''')
patch('src/common/emusuni_stats.h', r'''
    CpuWritesQueued, CpuWritesPending, // (01.12) 3ds-async-cpu-writes: invalidations queued, writes to queued pages
''', r'''
    CpuWritesQueued, CpuWritesPending, // (01.12) 3ds-async-cpu-writes: invalidations queued, writes to queued pages
    NativeMemcpyCalls, NativeMemcpyBytes, // (01.41) 3ds-jit-native-calls: the game's memcpy run natively
''')
patch('src/common/emusuni_frame_stats.h', r'''
                         dc[IdleLoopCycles] * 100 / (268111856ull * 600 * 100 / 5983));
            }
''', r'''
                         dc[IdleLoopCycles] * 100 / (268111856ull * 600 * 100 / 5983));
            }
            // (01.41) The game's memcpy run natively (3ds-jit-native-calls, scripts/3ds-jit-patches.py).
            if (dc[NativeMemcpyCalls] != 0) {
                LOG_INFO(Frontend,
                         "EmuSuNi: native calls in those 600 frames (3ds-jit-native-calls): memcpy {} times, {} KiB",
                         dc[NativeMemcpyCalls], dc[NativeMemcpyBytes] / 1024);
            }
''')

# --- 01.45: 3ds-jit-cycles-register (the study's idea 3). dynarmic counts the 3DS CPU's cycles left in a slot of its
# stack: every block subtracts its cycles there (subq $n,(%rsp)) and every link compares it (cmpq $0,(%rsp)), a read
# and a write of memory each, chained from block to block. On the console 40 % of the game's code time is in blocks of 1
# or 2 ARM instructions (a conditional instruction ends a block), where that is a good part of the work: 10 % of the hot
# blocks' instructions, 17 % of their time on the PC (perfil-consola-0142-siguiente.md). Here the count lives in r12
# (callee-saved: C++ calls keep it) while the JIT's code runs: r12 leaves the register allocator, the fast dispatcher's
# table pointer moves from r12 to rcx (free there), and every place that read or wrote the stack's slot goes through
# BlockOfCode::EmuSuNiCycles(), r12 or that same slot (off: the code as before, byte for byte). The ticks given to
# Azahar (AddTicks, GetTicksRemaining) don't change. On the PC: EMUSUNI_3DS_JIT_CYCLES_REGISTER=0 for the old way.
def emusuni_replace_all(path, old, new, count):
    # Like patch(), for a text that is there count times (each one replaced).
    key = (root, path)
    if key not in patched:
        patched[key] = git_file(root, path)
    text = patched[key]
    assert text.count(old) == count, (path, text.count(old), count, old[:70])
    patched[key] = text.replace(old, new)

DYN = 'externals/dynarmic/src/dynarmic/'
patch(DYN + 'interface/A32/config.h', r'''
inline bool emusuni_idle_loops = false;
''', r'''
inline bool emusuni_idle_loops = false;
/// EmuSuNi (01.45, 3ds-jit-cycles-register, scripts/3ds-jit-patches.py): the JITs made from now on keep the cycles left
/// in a register (r12) instead of their stack (/data/EmuSuNi/3ds-jit-cycles-register: off).
inline bool emusuni_cycles_register = false;
''')
patch(DYN + 'backend/x64/block_of_code.h', r'''
    std::unique_ptr<Callback> GetTicksRemaining;
    bool enable_cycle_counting;
};
''', r'''
    std::unique_ptr<Callback> GetTicksRemaining;
    bool enable_cycle_counting;
    bool emusuni_cycles_reg = false;  // EmuSuNi (01.45, 3ds-jit-cycles-register): the cycles left in r12
};
''')
patch(DYN + 'backend/x64/block_of_code.h', r'''
    /// Code emitter: Performs a block lookup based on current state
    /// @note this clobbers ABI caller-save registers
    void LookupBlock();
''', r'''
    /// Code emitter: Performs a block lookup based on current state
    /// @note this clobbers ABI caller-save registers
    void LookupBlock();

    /// EmuSuNi (01.45, 3ds-jit-cycles-register, scripts/3ds-jit-patches.py): the cycles left to run, r12 for as long
    /// as the JIT's code runs (RunCodeCallbacks::emusuni_cycles_reg), or the stack's StackLayout::cycles_remaining as
    /// upstream. Every emitter reaches them through here.
    bool emusuni_cycles_reg = false;
    std::unique_ptr<Xbyak::Address> emusuni_cycles_mem;
    const Xbyak::Operand& EmuSuNiCycles() const {
        return emusuni_cycles_reg ? static_cast<const Xbyak::Operand&>(r12) : *emusuni_cycles_mem;
    }
''')
patch(DYN + 'backend/x64/block_of_code.cpp', r'''
        , host_features(GetHostFeatures()) {
    EnableWriting();
''', r'''
        , host_features(GetHostFeatures()) {
    // EmuSuNi (01.45, 3ds-jit-cycles-register): where the cycles left live, known before the prelude is written.
    emusuni_cycles_reg = this->cb.emusuni_cycles_reg && this->cb.enable_cycle_counting;
    emusuni_cycles_mem = std::make_unique<Xbyak::Address>(
        qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
    EnableWriting();
''')
CYC = 'qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]'
emusuni_replace_all(DYN + 'backend/x64/block_of_code.cpp', 'mov(' + CYC + ', ABI_RETURN);',
                    'mov(EmuSuNiCycles(), ABI_RETURN);', 2)
emusuni_replace_all(DYN + 'backend/x64/block_of_code.cpp', 'mov(' + CYC + ', 1);', 'mov(EmuSuNiCycles(), 1);', 1)
emusuni_replace_all(DYN + 'backend/x64/block_of_code.cpp', 'cmp(' + CYC + ', 0);', 'cmp(EmuSuNiCycles(), 0);', 2)
emusuni_replace_all(DYN + 'backend/x64/block_of_code.cpp', 'sub(param[0], ' + CYC + ');',
                    'sub(param[0], EmuSuNiCycles());', 2)
emusuni_replace_all(DYN + 'backend/x64/emit_x64.cpp', 'code.sub(' + CYC + ', static_cast<u32>(cycles));',
                    'code.sub(code.EmuSuNiCycles(), static_cast<u32>(cycles));', 1)
emusuni_replace_all(DYN + 'backend/x64/a32_emit_x64.cpp', 'code.sub(code.ABI_PARAM2, ' + CYC + ');',
                    'code.sub(code.ABI_PARAM2, code.EmuSuNiCycles());', 2)
emusuni_replace_all(DYN + 'backend/x64/a32_emit_x64.cpp', 'code.mov(' + CYC + ', code.ABI_RETURN);',
                    'code.mov(code.EmuSuNiCycles(), code.ABI_RETURN);', 2)
emusuni_replace_all(DYN + 'backend/x64/a32_emit_x64.cpp', 'code.cmp(' + CYC + ', 0);',
                    'code.cmp(code.EmuSuNiCycles(), 0);', 1)
emusuni_replace_all(DYN + 'backend/x64/a64_emit_x64.cpp', 'code.cmp(' + CYC + ', 0);',
                    'code.cmp(code.EmuSuNiCycles(), 0);', 1)
for f, n in (('block_of_code.cpp', 1), ('emit_x64.cpp', 0), ('a32_emit_x64.cpp', 0), ('a64_emit_x64.cpp', 0)):
    assert patched[(root, DYN + 'backend/x64/' + f)].count('offsetof(StackLayout, cycles_remaining)') == n, f
patch(DYN + 'backend/x64/a32_emit_x64.cpp', r'''
            if (conf.fastmem_pointer) {
                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));
            }
            return gprs;
        }();
    }
    const std::vector<HostLoc>& gpr_order = emusuni_gpr_order;
''', r'''
            if (conf.fastmem_pointer) {
                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));
            }
            if (code.emusuni_cycles_reg) {
                // (01.45, 3ds-jit-cycles-register) r12 holds the cycles left.
                gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R12));
            }
            return gprs;
        }();
    }
    const std::vector<HostLoc>& gpr_order = emusuni_gpr_order;
''')
patch(DYN + 'backend/x64/a32_emit_x64.cpp', r'''
        code.mov(r12, reinterpret_cast<u64>(fast_dispatch_table.data()));
        code.mov(rbp, rbx);
        if (code.HasHostFeature(HostFeature::SSE42)) {
            code.crc32(rbp, r12);
        }
        code.and_(ebp, fast_dispatch_table_mask);
        code.lea(rbp, ptr[r12 + rbp]);
''', r'''
        // EmuSuNi (01.45, 3ds-jit-cycles-register): the table in rcx (free here) where r12 holds the cycles left.
        const Xbyak::Reg64 emusuni_table = code.emusuni_cycles_reg ? rcx : r12;
        code.mov(emusuni_table, reinterpret_cast<u64>(fast_dispatch_table.data()));
        code.mov(rbp, rbx);
        if (code.HasHostFeature(HostFeature::SSE42)) {
            code.crc32(rbp, emusuni_table);
        }
        code.and_(ebp, fast_dispatch_table_mask);
        code.lea(rbp, ptr[emusuni_table + rbp]);
''')
patch(DYN + 'backend/x64/a32_interface.cpp', r'''
        std::make_unique<ArgCallback>(Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
''', r'''
        std::make_unique<ArgCallback>(Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
        A32::emusuni_cycles_register,  // EmuSuNi (01.45, 3ds-jit-cycles-register)
    };
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the game's memcpy and divisions {} (3ds-jit-native-calls)", GetID(),
             emusuni_native_calls ? "run natively" : "translated as they are (off)");
''', r'''
    LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the game's memcpy and divisions {} (3ds-jit-native-calls)", GetID(),
             emusuni_native_calls ? "run natively" : "translated as they are (off)");
    // (01.45, scripts/3ds-jit-patches.py) The cycles left in a register (r12) unless
    // /data/EmuSuNi/3ds-jit-cycles-register says off (dynarmic's BlockOfCode::EmuSuNiCycles).
    Dynarmic::A32::emusuni_cycles_register =
        EmuSuNi::Async::TestFile("3ds-jit-cycles-register", "EMUSUNI_3DS_JIT_CYCLES_REGISTER", 0) >= 0;
    LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: the cycles left {} (3ds-jit-cycles-register)", GetID(),
             Dynarmic::A32::emusuni_cycles_register ? "in a register" : "on the stack (off)");
''')

# --- 01.48: 3ds-jit-cond-branches (the 01.47 profile's first candidate, "a1"). dynarmic ends a block before any
# conditional instruction that isn't part of the block's own entry condition, and gives it a block of its own: a link
# to it, its condition tested at its entry, a link out. So every conditional branch (B<cond>) after other
# instructions costs a block: in Castlevania's busy part (the 01.47 console profile, its game blocks re-split by
# dynarmic's rules on the game's code) 10 % of the game's code time is in blocks that are only a B<cond>, and 38 % in
# blocks of 1 or 2 instructions. Here such a branch ends the block it follows with an If terminal, as dynarmic's A64
# does with B.cond: the condition tested there, then a link to the target or to the next instruction (both checking
# the cycles left, as that block's own links did; the same cycles counted). Left as before: a B<cond> in the run of
# the block's entry condition, one that starts a block, and the branch back of an idle loop when they are skipped
# (01.11: that loop is marked on the branch's own block). ARM code only (Thumb's as before). On the PC:
# EMUSUNI_3DS_JIT_COND_BRANCHES=0 for the old way.
patch(DYN + 'interface/A32/config.h', r'''
inline bool emusuni_cycles_register = false;
''', r'''
inline bool emusuni_cycles_register = false;
/// EmuSuNi (01.48, 3ds-jit-cond-branches, scripts/3ds-jit-patches.py): a conditional B after other instructions ends
/// their block with an If terminal instead of a block of its own (/data/EmuSuNi/3ds-jit-cond-branches: off).
inline bool emusuni_cond_branches = false;
''')
patch(DYN + 'frontend/A32/translate/translate_arm.cpp', r'''
IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
''', r'''
#ifdef EMUSUNI_PORT
// EmuSuNi (01.48, 3ds-jit-cond-branches, scripts/3ds-jit-patches.py): a conditional B (not BL) after other
// instructions ends their block with If{cond, link to the target, link to the next}: true when it did. False (the
// instruction translated as before): not such a branch, the first of its block, part of the run of the block's own
// entry condition, or an idle loop's branch back when they are skipped (EmuSuNiMarkIdleLoop marks it on the branch's
// own block).
static bool EmuSuNiCondBranch(TranslatorVisitor& v, TranslateCallbacks* tcb, u32 pc, u32 insn) {
    const u32 cond = insn >> 28;
    if ((insn & 0x0F000000) != 0x0A000000 || cond >= 0xE || v.ir.block.empty()) {
        return false;
    }
    switch (v.cond_state) {
    case ConditionalState::None:
    case ConditionalState::Trailing:
        break;
    case ConditionalState::Translating:
        if (v.ir.block.ConditionFailedLocation() == v.ir.current_location &&
            static_cast<IR::Cond>(cond) == v.ir.block.GetCondition()) {
            return false;
        }
        break;
    default:
        return false;
    }
    const s32 offset = (static_cast<s32>(insn << 8) >> 6) + 8;
    if (emusuni_idle_loops && offset <= 0 && offset >= -64) {
        u32 constants = 0;
        int loads = 0;
        bool idle = true;
        for (u32 at = pc + static_cast<u32>(offset); at < pc && idle; at += 4) {
            const auto body = tcb->MemoryReadCode(at);
            idle = body && EmuSuNiIdleBody(*body, constants, loads);
        }
        if (idle && loads != 0) {
            return false;
        }
    }
    if (v.cond_state == ConditionalState::Translating) {
        v.cond_state = ConditionalState::Trailing;  // (as IsConditionPassed leaves it before its break)
    }
    v.ir.SetTerm(IR::Term::If{static_cast<IR::Cond>(cond), IR::Term::LinkBlock{v.ir.current_location.AdvancePC(offset)},
                              IR::Term::LinkBlock{v.ir.current_location.AdvancePC(4)}});
    return true;
}
#endif

IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
''')
patch(DYN + 'frontend/A32/translate/translate_arm.cpp', r'''
            ticks_for_instruction = tcb->GetTicksForCode(false, arm_pc, *arm_instruction);

#ifdef EMUSUNI_PORT
            // EmuSuNi: the VFP and ASIMD tables only where their patterns can match (scripts/build-3ds-core.sh).
''', r'''
            ticks_for_instruction = tcb->GetTicksForCode(false, arm_pc, *arm_instruction);

#ifdef EMUSUNI_PORT
            // EmuSuNi (01.48, 3ds-jit-cond-branches): a conditional B ends the block with an If terminal.
            if (emusuni_cond_branches && !single_step &&
                EmuSuNiCondBranch(visitor, tcb, arm_pc, *arm_instruction)) {
                should_continue = false;
            } else
#endif
#ifdef EMUSUNI_PORT
            // EmuSuNi: the VFP and ASIMD tables only where their patterns can match (scripts/build-3ds-core.sh).
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
             Dynarmic::A32::emusuni_cycles_register ? "in a register" : "on the stack (off)");
''', r'''
             Dynarmic::A32::emusuni_cycles_register ? "in a register" : "on the stack (off)");
    // (01.48, scripts/3ds-jit-patches.py) A conditional B ends the block it follows (an If terminal) unless
    // /data/EmuSuNi/3ds-jit-cond-branches says off (dynarmic's TranslateArm: EmuSuNiCondBranch).
    Dynarmic::A32::emusuni_cond_branches =
        EmuSuNi::Async::TestFile("3ds-jit-cond-branches", "EMUSUNI_3DS_JIT_COND_BRANCHES", 0) >= 0;
    LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: conditional branches {} (3ds-jit-cond-branches)", GetID(),
             Dynarmic::A32::emusuni_cond_branches ? "end the block they follow"
                                                  : "in blocks of their own (off)");
''')

# --- 01.49: 3ds-jit-cond-alu (the 01.47 profile's "a2"). The other conditional instructions dynarmic gives a block of
# their own: in Castlevania's busy part 15 % of the game's code time is in blocks that start at a conditional data
# processing instruction, or right after a run of them (the condition-failed continuation). Here a conditional data
# processing instruction without S and not into PC (MOVEQ, ADDNE, ORRGT...; MUL/MLA without S; CLZ) is translated as
# if unconditional, and each register it writes becomes a select inside the block: ConditionalSelect(cond, the new
# value, the register as it was) - a cmov on the flags as they are there (dynarmic's flags pass keeps the stores
# before it). The same ticks counted. If its IR holds anything but values, register reads and writes (not PC) and the
# C flag's read, it is taken out again and the instruction translated as before. Not touched: flag setting (S, CMP...)
# conditional instructions, loads and stores, and the run of the block's own entry condition. ARM code only. On the
# PC: EMUSUNI_3DS_JIT_COND_ALU=0 for the old way.
patch(DYN + 'interface/A32/config.h', r'''
inline bool emusuni_cond_branches = false;
''', r'''
inline bool emusuni_cond_branches = false;
/// EmuSuNi (01.49, 3ds-jit-cond-alu, scripts/3ds-jit-patches.py): a conditional data processing instruction (no S,
/// not into PC) as selects inside its block instead of a block of its own (/data/EmuSuNi/3ds-jit-cond-alu: off).
inline bool emusuni_cond_alu = false;
''')
patch(DYN + 'frontend/A32/translate/translate_arm.cpp', r'''
#include "dynarmic/ir/basic_block.h"
''', r'''
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"  // EmuSuNi (01.49, 3ds-jit-cond-alu)
''')
patch(DYN + 'frontend/A32/translate/translate_arm.cpp', r'''
IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
''', r'''
#ifdef EMUSUNI_PORT
// EmuSuNi (01.49, 3ds-jit-cond-alu): the words taken: data processing (an immediate, a register shifted by an
// immediate or by a register) without S, not TST/TEQ/CMP/CMN, not into PC; MUL/MLA without S, not into PC; CLZ.
static bool EmuSuNiCondAluWord(u32 insn) {
    if ((insn >> 28) >= 0xE) {
        return false;
    }
    if ((insn & 0x0FC000F0) == 0x00000090) { // MUL, MLA (Rd: bits 19-16)
        return ((insn >> 20) & 1) == 0 && ((insn >> 16) & 15) != 15;
    }
    if ((insn & 0x0FFF0FF0) == 0x016F0F10) { // CLZ
        return ((insn >> 12) & 15) != 15;
    }
    const bool dp_imm = (insn & 0x0E000000) == 0x02000000;
    const bool dp_reg = (insn & 0x0E000010) == 0x00000000 || (insn & 0x0E000090) == 0x00000010;
    if (!dp_imm && !dp_reg) {
        return false;
    }
    const u32 op = (insn >> 21) & 15;
    return ((insn >> 20) & 1) == 0 && !(op >= 8 && op <= 11) && ((insn >> 12) & 15) != 15;
}

// EmuSuNi (01.49, 3ds-jit-cond-alu, scripts/3ds-jit-patches.py): such an instruction where dynarmic would end the
// block before it (or start a block with it) translated with its condition as AL, each register write's value
// replaced by ConditionalSelect(cond, value, the register before): true when it did. False (nothing left of it in the
// block): not such a word, part of the run of the block's own entry condition, or its IR wasn't plain values,
// register reads and writes and the C flag's read (taken out again: the instruction is translated as before).
static bool EmuSuNiCondAlu(TranslatorVisitor& v, u32 insn) {
    if (!EmuSuNiCondAluWord(insn)) {
        return false;
    }
    const auto cond = static_cast<IR::Cond>(insn >> 28);
    switch (v.cond_state) {
    case ConditionalState::None:
    case ConditionalState::Trailing:
        break;
    case ConditionalState::Translating:
        if (v.ir.block.ConditionFailedLocation() == v.ir.current_location && cond == v.ir.block.GetCondition()) {
            return false;
        }
        break;
    default:
        return false;
    }
    IR::Block& block = v.ir.block;
    IR::Inst* const last = block.empty() ? nullptr : &block.back();
    const ConditionalState state = v.cond_state;
    const u32 always = (insn & 0x0FFFFFFF) | 0xE0000000;
    const auto decoder = DecodeArm<TranslatorVisitor>(always);
    bool plain = decoder && decoder->get().call(v, always) && !block.HasTerminal() &&
                 v.cond_state != ConditionalState::Break;
    auto first = block.begin();
    if (last) {
        first = IR::Block::iterator{*last};
        ++first;
    }
    for (auto it = first; plain && it != block.end(); ++it) {
        switch (it->GetOpcode()) {
        case IR::Opcode::A32SetRegister:
            plain = it->GetArg(0).GetA32RegRef() != A32::Reg::PC;
            break;
        case IR::Opcode::A32GetRegister:
        case IR::Opcode::A32GetCFlag:
            break;
        default:
            plain = !it->MayHaveSideEffects() && !it->IsMemoryRead() && !it->ReadsFromCPSR() &&
                    !it->ReadsFromFPCR() && !it->IsCoprocessorInstruction();
            break;
        }
    }
    if (!plain) {
        // Taken out again, last first (each one's uses go before it does), back to the block as it was.
        while (!block.empty() && &block.back() != last) {
            auto it = block.end();
            --it;
            it->Invalidate();
            block.Instructions().erase(it);
        }
        if (block.HasTerminal()) {
            block.ReplaceTerminal(IR::Term::Invalid{});
        }
        v.cond_state = state;
        return false;
    }
    for (auto it = first; it != block.end(); ++it) {
        if (it->GetOpcode() != IR::Opcode::A32SetRegister) {
            continue;
        }
        const A32::Reg reg = it->GetArg(0).GetA32RegRef();
        v.ir.SetInsertionPointBefore(&*it);
        const IR::U32 before = v.ir.GetRegister(reg);
        const IR::U32 chosen = v.ir.ConditionalSelect(cond, IR::U32{it->GetArg(1)}, before);
        it->SetArg(1, chosen);
    }
    v.ir.SetInsertionPointBefore(block.end());
    return true;
}
#endif

IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
''')
patch(DYN + 'frontend/A32/translate/translate_arm.cpp', r'''
                should_continue = false;
            } else
#endif
#ifdef EMUSUNI_PORT
            // EmuSuNi: the VFP and ASIMD tables only where their patterns can match (scripts/build-3ds-core.sh).
''', r'''
                should_continue = false;
            } else if (emusuni_cond_alu && !single_step && EmuSuNiCondAlu(visitor, *arm_instruction)) {
                // EmuSuNi (01.49, 3ds-jit-cond-alu): translated, as selects inside the block.
            } else
#endif
#ifdef EMUSUNI_PORT
            // EmuSuNi: the VFP and ASIMD tables only where their patterns can match (scripts/build-3ds-core.sh).
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
                                                  : "in blocks of their own (off)");
''', r'''
                                                  : "in blocks of their own (off)");
    // (01.49, scripts/3ds-jit-patches.py) Conditional data processing as selects inside the block unless
    // /data/EmuSuNi/3ds-jit-cond-alu says off (dynarmic's TranslateArm: EmuSuNiCondAlu).
    Dynarmic::A32::emusuni_cond_alu = EmuSuNi::Async::TestFile("3ds-jit-cond-alu", "EMUSUNI_3DS_JIT_COND_ALU", 0) >= 0;
    LOG_INFO(Core_ARM11, "EmuSuNi: core {}'s JIT: conditional data processing {} (3ds-jit-cond-alu)", GetID(),
             Dynarmic::A32::emusuni_cond_alu ? "as selects inside the block" : "in blocks of their own (off)");
''')

# --- 01.50: the native library (3ds-jit-native-routines; 3ds-jit-native-castlevania). The owner's decision of
# 2026-11-16: "a native library of what 3DS games use most, and what Castlevania would use", then "seguir ampliando
# de todos modos" (the delivery: biblioteca-nativa-0150.md). Routines the owner's 33 games share run natively where a
# block starts at one. On by default only those that ran faster natively than dynarmic's own code for them, each
# measured on and off in play on the PC: RVCT's integer division (__aeabi_idivmod, its negative path,
# __aeabi_uidivmod: 32 of the 33 games) and strlen (9), a CRC-32 that builds its 1 KiB table on every call (9), Yaz0
# decompression (6). Off by default, named in the log's list (as fast or slower on the PC: dynarmic's blocks count
# their ticks for free and reach memory in one instruction; the console's costs may differ): the SDK's GPU command
# writer (18), float24 loop of uniforms (6) and three halves (10), RVCT's strcmp (8) and aligned memcpy (33); and
# Castlevania: Mirror of Fate's (USA, 0004000000096600) list search loop and heap best-fit search, behind a switch of
# their own. src/core/arm/dynarmic/emusuni_native_routines.h is each one instruction by instruction: the same memory
# reads and writes (stack pushes too), the same registers and flags left, the same 3DS ticks as dynarmic counts them
# (Azahar's TicksForInstruction for every instruction passed, one for a failed conditional run that starts a block;
# a1/a2 on); the CRC-32 runs whole only when its ticks fit in the JIT's slice (else its first block, as the JIT's) and
# Yaz0 stops at the end of the block where the JIT would stop: so the slices end where they did. Checked against
# dynarmic running the games' own code on odd inputs and random slices (0 different). Their ticks vary with the data:
# dynarmic gets IR instructions of ours, EmuSuNiSubCyclesFrom (a count a routine left at a host address, taken from
# the cycles left), EmuSuNiCheckBitFrom (a byte at a host address as the check bit: its way out) and
# EmuSuNiCyclesLeft (the slice's cycles as the block starts). A routine reads and writes the guest's registers, flags
# and VFP registers in dynarmic's state (its A32JitState, fixed with the place: a call costs a few nanoseconds) and
# its memory through the page table's pointers (Azahar's own accesses for any page without one: the GPU's cache and
# the rest, as the JIT's fallbacks). The switch files' words (read with each JIT, so a new game reads them again): off
# (all off), all (all on), +name (that one on), name or -name (that one off); names: division, strlen, crc32, yaz0,
# lz, yaz0s, crc16, writer, float24, half3, strcmp, memcpy4. Castlevania's file needs on (or +listwalk, +heapfit); listwalk or heapfit
# turns one off. Every 600 frames a line with each one's calls; each place found logged once with its title; each
# JIT's on and off lists. On the PC: EMUSUNI_3DS_JIT_NATIVE_ROUTINES (0, 1 or words) and
# EMUSUNI_3DS_JIT_NATIVE_CASTLEVANIA.
patch(DYN + 'ir/opcodes.inc', r'''
OPCODE(CallHostFunction,                                    Void,           U64,            Opaque,         Opaque,         Opaque          )
''', r'''
OPCODE(CallHostFunction,                                    Void,           U64,            Opaque,         Opaque,         Opaque          )
OPCODE(EmuSuNiSubCyclesFrom,                                Void,           U64                                                             )
OPCODE(EmuSuNiCheckBitFrom,                                 Void,           U64                                                             )
OPCODE(EmuSuNiCyclesLeft,                                   U64,                                                                            )
''')
patch(DYN + 'ir/ir_emitter.h', r'''
    void CallHostFunction(void (*fn)(u64, u64, u64), const U64& arg1, const U64& arg2, const U64& arg3);
''', r'''
    void CallHostFunction(void (*fn)(u64, u64, u64), const U64& arg1, const U64& arg2, const U64& arg3);
    /// EmuSuNi (01.50, 3ds-jit-native-routines): the 64-bit count at a host address taken from the cycles left.
    void EmuSuNiSubCyclesFrom(const U64& address);
    /// EmuSuNi (01.50): the check bit set from the byte at a host address.
    void EmuSuNiCheckBitFrom(const U64& address);
    /// EmuSuNi (01.50): the cycles left in the JIT's slice (as the block starts).
    U64 EmuSuNiCyclesLeft();
''')
patch(DYN + 'ir/ir_emitter.cpp', r'''
void IREmitter::SetTerm(const Terminal& terminal) {
''', r'''
void IREmitter::EmuSuNiSubCyclesFrom(const U64& address) {
    Inst(Opcode::EmuSuNiSubCyclesFrom, address);
}

void IREmitter::EmuSuNiCheckBitFrom(const U64& address) {
    Inst(Opcode::EmuSuNiCheckBitFrom, address);
}

U64 IREmitter::EmuSuNiCyclesLeft() {
    return Inst<U64>(Opcode::EmuSuNiCyclesLeft);
}

void IREmitter::SetTerm(const Terminal& terminal) {
''')
patch(DYN + 'ir/microinstruction.cpp', r'''
        || op == Opcode::CallHostFunction
''', r'''
        || op == Opcode::CallHostFunction
        || op == Opcode::EmuSuNiSubCyclesFrom  // EmuSuNi (01.50)
        || op == Opcode::EmuSuNiCheckBitFrom   // EmuSuNi (01.50)
        || op == Opcode::EmuSuNiCyclesLeft     // EmuSuNi (01.50)
''')
patch(DYN + 'backend/x64/emit_x64.cpp', r'''
void EmitX64::EmitCallHostFunction(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, args[1], args[2], args[3]);
    code.mov(rax, args[0].GetImmediateU64());
    code.call(rax);
}
''', r'''
void EmitX64::EmitCallHostFunction(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, args[1], args[2], args[3]);
    code.mov(rax, args[0].GetImmediateU64());
    code.call(rax);
}

void EmitX64::EmitEmuSuNiSubCyclesFrom(EmitContext& ctx, IR::Inst* inst) {
    // EmuSuNi (01.50, 3ds-jit-native-routines, scripts/3ds-jit-patches.py): the cycles left minus the count a native
    // routine left at a host address (the ticks it took, which vary with its data).
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 ticks = ctx.reg_alloc.ScratchGpr();
    code.mov(ticks, args[0].GetImmediateU64());
    code.mov(ticks, qword[ticks]);
    code.sub(code.EmuSuNiCycles(), ticks);
}

void EmitX64::EmitEmuSuNiCheckBitFrom(EmitContext& ctx, IR::Inst* inst) {
    // EmuSuNi (01.50): the check bit (a CheckBit terminal's) from the byte a native routine left at a host address.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 bit = ctx.reg_alloc.ScratchGpr();
    code.mov(bit, args[0].GetImmediateU64());
    code.movzx(bit.cvt32(), byte[bit]);
    code.mov(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], bit.cvt8());
}

void EmitX64::EmitEmuSuNiCyclesLeft(EmitContext& ctx, IR::Inst* inst) {
    // EmuSuNi (01.50): the cycles left in the slice (a routine that runs whole only when it fits, as the JIT would).
    const Xbyak::Reg64 left = ctx.reg_alloc.ScratchGpr();
    code.mov(left, code.EmuSuNiCycles());
    ctx.reg_alloc.DefineValue(inst, left);
}
''')
addfile('src/core/arm/dynarmic/emusuni_native_routines.h', r'''// EmuSuNi (01.50, 3ds-jit-native-routines, scripts/3ds-jit-patches.py): routines found word for word in many 3DS
// games, run natively where a block starts at one (arm_dynarmic.cpp's PreCodeReadHook). On by default only those
// that run faster natively than dynarmic's own code for them, each measured on and off in play on the PC: RVCT's
// integer division and strlen, a CRC-32 that builds its table on every call, Yaz0 decompression. Off by default
// (as fast or slower there: dynarmic's blocks count their ticks for free and reach memory in one instruction; the
// console may differ), each turned on by +its word in the switch file: the SDK's GPU command writer, float24 loop of
// uniforms and three halves, RVCT's strcmp and aligned memcpy; Castlevania: Mirror of Fate's two behind their own
// switch. Each is the ARM code as it is: the same memory reads and writes (its pushes onto the stack too), the same
// registers and flags left, the same 3DS CPU ticks as dynarmic counts them (3ds-jit-cond-branches and
// 3ds-jit-cond-alu on); the CRC-32 and Yaz0 also stop where the JIT's slice would stop them. Checked against dynarmic
// running the games' own code (the performance specialist's diff50.cpp and diff52.cpp: odd inputs, random slices).
//  - CommandWriter (56 words): a GPU command list's "write N consecutive registers" (the first value, the header, the
//    rest in pairs, 128 at a time), into the list whose current and end pointers sit at a literal's address.
//  - Float24Loop (19 words, the head of a loop inside a larger function): a uniform's floats turned into the PICA's
//    24-bit floats, the loop's remaining turns (at most `limit` a call: the caller comes back while r1 isn't 0).
//  - Half3 (73 words): three floats to halves and their GPU command (a header literal plus the index), appended to the
//    command list.
//  - Strcmp (46 words): RVCT's strcmp, a word at a time when both strings are aligned.
#pragma once

#include <cstdint>
#include <cstring>

namespace EmuSuNi::NativeRoutines {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum Kind : u32 {
    kNone = 0,
    kCommandWriter = 1,
    kFloat24Loop = 2,
    kHalf3 = 3,
    kStrcmp = 4,
    kStrlen = 5,
    kMemcpy4 = 6,
    kDivSigned = 7,      // __aeabi_idivmod's entry
    kDivSignedPath = 8,  // its path for a negative operand (+0x140), where a block can start
    kDivUnsigned = 9,    // __aeabi_uidivmod's entry, which runs on in the same body
    kListWalk = 10,      // Castlevania: Mirror of Fate's list search loop (0x14A5B8)
    kHeapBestFit = 11,   // Castlevania: Mirror of Fate's heap block search (0x146A44)
    kCrc32 = 12,         // a CRC-32 that builds its table on the stack on every call (nine games)
    kYaz0Rev = 13,       // Yaz0 decompression, its size by rev (Captain Toad, Tri Force Heroes, Miitopia)
    kYaz0Swap = 14,      // the same, its size by a swap of four words (Super Mario 3D Land, Mario Kart 7)
    kYaz0Head = 15,      // its loop's head (where the JIT goes back to it after a stop)
    kLz = 16,            // LZ10/LZ11 decompression (Kirby: Planet Robobot, four Pokemon)
    kLzHead = 17,        // its item's head (where the JIT goes back to it after a stop)
    kYazStream = 18,     // streaming Yaz0 (SM3DL, Mario Kart 7): its loop's head
    kYazStreamT = 19,    // streaming Yaz0 (Captain Toad, Tri Force Heroes, Miitopia): its loop's head
    kCrc16A = 20,        // CRC-16 CCITT bit by bit (SM3DL, Sonic): its loop's head, the count in r5
    kCrc16B = 21,        // the same, the count in r6
    kCrc16N = 22,        // the same, Nintendo's order (Kirby, MK7, Miitopia, NSMB2): poly r5, mask r4, count r12
    kCrc16S = 23,        // the same, Smash's registers (and Kirby, MK7, Miitopia, NSMB2): poly r4, mask r6
};

inline constexpr u32 kCommandWriterCode[56] = {
    0xE92D07F0, 0xE1A043A1, 0xE3C1C001, 0xE08C4084, 0xE59F60C8, 0xE8961008,
    0xE0834104, 0xE2844008, 0xE15C0004, 0x9586C000, 0x9A00002A, 0xE3510000,
    0xE3A05000, 0x82457601, 0x9A000025, 0xE082C105, 0xE0418005, 0xE49C4004,
    0xE3580080, 0x83A08080, 0xE4834004, 0xE1A040A8, 0xE0809005, 0xE0878A08,
    0xE1888009, 0xE3888102, 0xE388880F, 0xE3740106, 0xE4838004, 0x9A000006,
    0xE49C8004, 0xE2544001, 0xE4838004, 0xE49C8004, 0xE4838004, 0x1AFFFFF9,
    0xEA00000C, 0xE1A09084, 0xE3590000, 0xDA000009, 0xE24CC004, 0xE2438004,
    0xE1A090C9, 0xE59CA004, 0xE2599001, 0xE588A004, 0xE5BCA008, 0xE5A8A008,
    0x1AFFFFF9, 0xE0833184, 0xE2855080, 0xE1550001, 0x3AFFFFD9, 0xE5863000,
    0xE8BD07F0, 0xE12FFF1E,
};
inline constexpr u32 kFloat24LoopCode[19] = {
    0xECB30A01, 0xEE100A10, 0xE3D0C102, 0x0A000006, 0xE1A0C080, 0xE1A0CC2C,
    0xE24CC040, 0xE35C0000, 0xB1A00FA0, 0xB1A00B80, 0xBA000004, 0xE1A0E480,
    0xE1A00FA0, 0xE1A0E82E, 0xE18EC80C, 0xE18C0B80, 0xE2511001, 0xE4820004,
    0x1AFFFFEC,
};
inline constexpr u32 kHalf3Code[73] = {
    0xE92D00F0, 0xE3A0C000, 0xE5913000, 0xE59F4110, 0xE5912004, 0xE2135102,
    0xE1A06483, 0xE1A07083, 0xE1A064A6, 0x13A05902, 0xE3D33102, 0xE1A07C27,
    0x01A066A6, 0x0A000005, 0xE2573070, 0xE1A066A6, 0x4A000005, 0xE353001F,
    0xC3A05B1F, 0xCA000002, 0xE203701F, 0xE1863005, 0xE1835507, 0xE1A06082,
    0xE2123102, 0xE1A07C26, 0xE1A06482, 0x13A03902, 0xE1A064A6, 0xE3D22102,
    0xE6FF5075, 0x01A066A6, 0x0A000005, 0xE2572070, 0xE1A066A6, 0x4A000005,
    0xE352001F, 0xC3A03B1F, 0xCA000002, 0xE202701F, 0xE1862003, 0xE1823507,
    0xE1852803, 0xE0843200, 0xE5910008, 0xE2101102, 0xE1A04080, 0x13A01902,
    0xE1A05C24, 0xE1A04480, 0xE3D00102, 0xE1A044A4, 0x01A046A4, 0x0A000005,
    0xE2550070, 0xE1A046A4, 0x4A000005, 0xE350001F, 0xC3A01B1F, 0xCA000002,
    0xE200501F, 0xE1840001, 0xE1801505, 0xE59F4024, 0xE6FF0071, 0xE5941000,
    0xE2816008, 0xE2815010, 0xE8861001, 0xE1C120F0, 0xE5845000, 0xE8BD00F0,
    0xE12FFF1E,
};

inline constexpr u32 kStrcmpCode[46] = {
    0xE3100003, 0x03110003, 0x1A00001E, 0xE92D4000, 0xE59FE0A0, 0xE4902004,
    0xE4913004, 0xE042C00E, 0xE1CCC002, 0xE01CC38E, 0x01520003, 0x1A000006,
    0xE4902004, 0xE4913004, 0xE042C00E, 0xE1CCC002, 0xE01CC38E, 0x01520003,
    0x0AFFFFF1, 0xE8BD4000, 0xE1A00C03, 0xE0500C02, 0x031C0EFF, 0x1A000007,
    0xE1A00803, 0xE0500802, 0x031C0CFF, 0x1A000003, 0xE1A00403, 0xE0500402,
    0x031C08FF, 0x00530002, 0x11A00060, 0xE1A0F00E, 0xE4D02001, 0xE4D13001,
    0xE3520001, 0x21520003, 0x1A000004, 0xE4D02001, 0xE4D13001, 0xE3520001,
    0x21520003, 0x0AFFFFF5, 0xE0420003, 0xE1A0F00E,
};

inline constexpr u32 kStrlenCode[24] = {
    0xE2803001, 0xEA000003, 0xE4D01001, 0xE3510000, 0x00400003, 0x01A0F00E,
    0xE3100003, 0x059F203C, 0x1AFFFFF8, 0xE4901004, 0xE041C002, 0xE1CC1001,
    0xE0111382, 0x0AFFFFFA, 0xE1B02C01, 0xE0400003, 0x12400003, 0x11A0F00E,
    0xE1B02801, 0x12400002, 0x11A0F00E, 0xE1B01401, 0x12400001, 0xE1A0F00E,
};
inline constexpr u32 kMemcpy4Code[29] = {
    0xE92D47F0, 0xE2522020, 0x3A000009, 0xE8B10078, 0xF5D1F040, 0xE8B10780,
    0xE8A00078, 0xE2522020, 0xE8A00780, 0x28B10078, 0xF5D1F040, 0x28B10780,
    0x2AFFFFF8, 0xE1B0CE02, 0x28B15018, 0x28A05018, 0x48B10018, 0x48A00018,
    0xE8BD47F0, 0xE1B0CF02, 0x24913004, 0x24803004, 0x012FFF1E, 0xE1B02F82,
    0x20D130B2, 0x44D12001, 0x20C030B2, 0x44C02001, 0xE12FFF1E,
};

// RVCT's integer division: __aeabi_idivmod (its entry, word 0; its path for a negative operand, word 80) and the
// handler of a division by zero (133-138: push {r0, lr}, movs r0, #0, bl __aeabi_idiv0, pop {r1, pc}). Word 137 (the
// bl) is masked: its target differs from game to game.
inline constexpr u32 kDivideCode[139] = {
    0xE1903001, 0x4A00004D, 0xE07130A0, 0x3A000044, 0xE3B02000, 0xE0713220,
    0x3A000034, 0xE0713420, 0x3A000026, 0xE0713620, 0x3A000017, 0xE0713820,
    0x3A000009, 0xE1A01401, 0xE0713820, 0xE38224FF, 0x238228FF, 0x21A01401,
    0xE0713620, 0x3A00000E, 0xE2713000, 0x2A000070, 0x21A01421, 0xE07137A0,
    0x20400781, 0xE0B22002, 0xE0713720, 0x20400701, 0xE0B22002, 0xE07136A0,
    0x20400681, 0xE0B22002, 0xE0713620, 0x20400601, 0xE0B22002, 0xE07135A0,
    0x20400581, 0xE0B22002, 0xE0713520, 0x20400501, 0xE0B22002, 0xE07134A0,
    0x20400481, 0xE0B22002, 0xE0713420, 0x20400401, 0xE0B22002, 0x2AFFFFE5,
    0xE07133A0, 0x20400381, 0xE0B22002, 0xE0713320, 0x20400301, 0xE0B22002,
    0xE07132A0, 0x20400281, 0xE0B22002, 0xE0713220, 0x20400201, 0xE0B22002,
    0xE07131A0, 0x20400181, 0xE0B22002, 0xE0713120, 0x20400101, 0xE0B22002,
    0xE07130A0, 0x20400081, 0xE0B22002, 0xE0501001, 0x31A01000, 0xE0A20002,
    0xE12FFF1E, 0xE0501001, 0x3A000001, 0xE3B00001, 0xE12FFF1E, 0xE1A01000,
    0xE3B00000, 0xE12FFF1E, 0xE2112102, 0x42611000, 0xE032C040, 0x22600000,
    0xE0713220, 0x3A00001D, 0xE0713420, 0x3A00000F, 0xE1A01301, 0xE0713420,
    0xE382233F, 0x3A00000B, 0xE1A01301, 0xE0713420, 0xE382263F, 0x3A000007,
    0xE1A01301, 0xE0713420, 0xE382293F, 0x21A01301, 0x23822C3F, 0xE2713000,
    0x2A00001D, 0x21A01321, 0xE07133A0, 0x20400381, 0xE0B22002, 0xE0713320,
    0x20400301, 0xE0B22002, 0xE07132A0, 0x20400281, 0xE0B22002, 0xE0713220,
    0x20400201, 0xE0B22002, 0xE07131A0, 0x20400181, 0xE0B22002, 0xE0713120,
    0x20400101, 0xE0B22002, 0x2AFFFFEB, 0xE07130A0, 0x20400081, 0xE0B22002,
    0xE0501001, 0x31A01000, 0xE0A20002, 0xE1B0CFCC, 0x42600000, 0x22611000,
    0xE12FFF1E, 0xE1B0CFCC, 0x42600000, 0xE92D4001, 0xE3B00000, 0xEBF9ADCC,
    0xE8BD8002,
};
// __aeabi_uidivmod's entry (elsewhere in the library): its branches go into the body above (masked; its first one,
// word 2, must reach the body's word 66).
inline constexpr u32 kUdivEntryCode[12] = {
    0xE3B02000, 0xE0713120, 0x3AFF3B77, 0xE07132A0, 0x3AFF3B6C, 0xE0713420,
    0x3AFF3B61, 0xE0713620, 0x3AFF3B52, 0xE0713820, 0x3AFF3B44, 0xEAFF3B39,
};

// Castlevania: Mirror of Fate's own (its title only, their own switch): the loop of a list search (10 words, inside a
// larger function: the node's next, its previous, a flag in r11, the node's field +12) and a heap's best-fit search
// over its free list (37 words, a leaf).
inline constexpr u32 kListWalkCode[10] = {
    0xE5946004, 0xE5947000, 0xE35B0000, 0x1A000007, 0xE594000C, 0xE3500000,
    0x11A04006, 0x0A000003, 0xE3540000, 0x1AFFFFF5,
};
inline constexpr u32 kHeapBestFitCode[37] = {
    0xE2800018, 0xE1A00000, 0xE2802024, 0xE92D0070, 0xE3510000, 0xE5922000,
    0xB2611000, 0xE3A00000, 0xE3520000, 0xE3E0C000, 0x12415001, 0x0A000016,
    0xE5924004, 0xE0823001, 0xE2826010, 0xE283300F, 0xE1C33005, 0xE0866004,
    0xE1560003, 0x9A00000B, 0xE0846002, 0xE0434002, 0xE0463003, 0xE2833010,
    0xE1500003, 0xE2444010, 0x3A000002, 0x1A000003, 0xE15C0004, 0x9A000001,
    0xE1A00003, 0xE1A0C004, 0xE592200C, 0xE3520000, 0x1AFFFFE8, 0xE8BD0070,
    0xE12FFF1E,
};

// CRC-32 (51 words, its polynomial after them at +0xCC: 0xEDB88320): push {r4-r8}; a frame of 1040 bytes; the
// polynomial's reflected table built at sp + 4 on every call (256 entries of 8 steps: eorne/lsreq, selects with a2);
// [sp] = r2; the CRC of r1 bytes at r0 from r2 (an odd byte first, then two a turn); [sp] = r2; r0 = ~r2;
// pop {r4-r8}; bx lr. Castlevania: Mirror of Fate, Fire Emblem Awakening, two Zeldas, Pokemon X, Ultra Sun and Moon.
inline constexpr u32 kCrc32Code[51] = {
    0xE92D01F0, 0xE24DDE41, 0xE1A0800D, 0xE59F50B8, 0xE3A06000, 0xE28D7004, 0xE1A03006, 0xE3A04004,
    0xE3130001, 0x1025C0A3, 0x01A0C0A3, 0xE31C0001, 0x102530AC, 0x01A030AC, 0xE2544001, 0x1AFFFFF7,
    0xE7873106, 0xE2866001, 0xE3560C01, 0x3AFFFFF1, 0xE3510000, 0xE1A03007, 0xE58D2000, 0x0A000015,
    0xE3110001, 0xE2400001, 0x0A000004, 0xE5F0C001, 0xE02CC002, 0xE20CC0FF, 0xE793C10C, 0xE02C2422,
    0xE1B010A1, 0x0A00000B, 0xE5D0C001, 0xE2511001, 0xE02C4002, 0xE20440FF, 0xE5F0C002, 0xE7934104,
    0xE0242422, 0xE02CC002, 0xE20CC0FF, 0xE793C10C, 0xE02C2422, 0x1AFFFFF3, 0xE58D2000, 0xE28DDE41,
    0xE1E00002, 0xE8BD01F0, 0xE12FFF1E,
};

// Yaz0 decompression (Nintendo's): r0 the output, r1 the file ("Yaz0", its size big-endian at +4, data at +16); r0
// = the size. push {r4-r8, lr}; the size (rev r4, r5: Captain Toad, Tri Force Heroes, Miitopia; or a swap of four
// words: Super Mario 3D Land, Mario Kart 7); then its loop (from its head, the same in both: kYaz0HeadCode): a group's
// byte of 8 flags, a literal byte or a back reference (2 bytes: length - 2 and distance - 1, or a third byte: length
// - 18), copied a byte at a time; pop {r4-r8, lr}; mov r0, r2; bx lr.
inline constexpr u32 kYaz0RevCode[39] = {
    0xE92D41F0, 0xE5915004, 0xE6BF4F35, 0xE1A02004, 0xE2811010, 0xE3A05000, 0xE3A0EA01, 0xE24EE001,
    0xE1B050A5, 0x1A000001, 0xE4D16001, 0xE3A05080, 0xE1160005, 0x1A000011, 0xE4D13001, 0xE4D17001,
    0xE0873403, 0xE1B07623, 0xE003300E, 0xE2833001, 0xE2877002, 0x1A000001, 0xE4D17001, 0xE2877012,
    0xE0444007, 0xE750C003, 0xE2577001, 0xE4C0C001, 0x1AFFFFFB, 0xE3540000, 0x1AFFFFE8, 0xEA000003,
    0xE4D1C001, 0xE2544001, 0xE4C0C001, 0x1AFFFFE3, 0xE8BD41F0, 0xE1A00002, 0xE12FFF1E,
};
inline constexpr u32 kYaz0SwapCode[42] = {
    0xE92D41F0, 0xE5914004, 0xE0245864, 0xE3C558FF, 0xE1A04464, 0xE0244425, 0xE1A02004, 0xE2811010,
    0xE3A05000, 0xE3A0EA01, 0xE24EE001, 0xE1B050A5, 0x1A000001, 0xE4D16001, 0xE3A05080, 0xE1160005,
    0x1A000011, 0xE4D13001, 0xE4D17001, 0xE0873403, 0xE1B07623, 0xE003300E, 0xE2833001, 0xE2877002,
    0x1A000001, 0xE4D17001, 0xE2877012, 0xE0444007, 0xE750C003, 0xE2577001, 0xE4C0C001, 0x1AFFFFFB,
    0xE3540000, 0x1AFFFFE8, 0xEA000003, 0xE4D1C001, 0xE2544001, 0xE4C0C001, 0x1AFFFFE3, 0xE8BD41F0,
    0xE1A00002, 0xE12FFF1E,
};
inline constexpr u32 kYaz0HeadCode[31] = {
    0xE1B050A5, 0x1A000001, 0xE4D16001, 0xE3A05080, 0xE1160005, 0x1A000011, 0xE4D13001, 0xE4D17001,
    0xE0873403, 0xE1B07623, 0xE003300E, 0xE2833001, 0xE2877002, 0x1A000001, 0xE4D17001, 0xE2877012,
    0xE0444007, 0xE750C003, 0xE2577001, 0xE4C0C001, 0x1AFFFFFB, 0xE3540000, 0x1AFFFFE8, 0xEA000003,
    0xE4D1C001, 0xE2544001, 0xE4C0C001, 0x1AFFFFE3, 0xE8BD41F0, 0xE1A00002, 0xE12FFF1E,
};

// LZ10/LZ11 decompression (Nintendo's, the BIOS's format; 138 words): r0 the source (type 0x10 or 0x11 and a
// 24-bit size, or 0 and a 32-bit one), r1 its size, r2 the output; r0: 0, or -1 (not LZ), -2 (the source ran out),
// -3 (more source left than 32 bytes), -4 (a reference before the output). push {r4-r11}; groups of 8 flags (the top
// first); a literal byte, or a reference: LZ10 two bytes (length - 3, distance - 1); LZ11 two (length - 1 from 2),
// three (length - 0x11) or four (length - 0x111); copied a byte, then pairs. Kirby: Planet Robobot, Pokemon X, Omega
// Ruby, Ultra Sun and Moon. kLzHeadCode: its words from 38 (a group's head; the item's head, 43, is where the JIT comes
// back to it after a stop).
inline constexpr u32 kLzCode[138] = {
    0xE92D0FF0, 0xE1A03002, 0xE5904000, 0xE5D05003, 0xE5D0C000, 0xE3C444FF, 0xE1844C05, 0xE31C000F,
    0xE1A07424, 0x13A08001, 0x03A08000, 0xE20C40F0, 0xE3540010, 0x1A000004, 0xE31C000F, 0x0A000005,
    0xE20CC00F, 0xE35C0001, 0x0A000002, 0xE8BD0FF0, 0xE3E00000, 0xE12FFF1E, 0xE3510004, 0xE1A0C001,
    0x9A000024, 0xE3570000, 0xE2805004, 0xE24C6004, 0x1A000008, 0xE3560004, 0xBA00001E, 0xE5950000,
    0xE5D51003, 0xE2855004, 0xE3C004FF, 0xE2466004, 0xE1907C01, 0x0A00005E, 0xE4D59001, 0xE2466001,
    0xE3560000, 0xA3A0A000, 0xBA000012, 0xE3190080, 0x0A000013, 0xE5D51000, 0xE3580000, 0xE1A00221,
    0x02800003, 0x0A000004, 0xE3500001, 0x0A000013, 0xE3500000, 0x12800001, 0x0A00001A, 0xE4D51001,
    0xE2566002, 0xE4D5C001, 0xE1A01E01, 0xE18C1A21, 0xE2811001, 0x5A00001A, 0xE8BD0FF0, 0xE3E00001,
    0xE12FFF1E, 0xE4D50001, 0xE2466001, 0xE3560000, 0xA2477001, 0xE4C30001, 0xBAFFFFF6, 0xEA000035,
    0xE1A00E01, 0xE5D51001, 0xE5F5C002, 0xE1A00820, 0xE2466002, 0xE1800201, 0xE180022C, 0xE2800C01,
    0xE2800011, 0xEAFFFFE4, 0xE1A00E01, 0xE5F51001, 0xE1A00C20, 0xE2466001, 0xE1800221, 0xE2800011,
    0xEAFFFFDD, 0xE1500007, 0x9043C001, 0x9152000C, 0x9A000002, 0xE8BD0FF0, 0xE3E00003, 0xE12FFF1E,
    0xE5DCC000, 0xE0477000, 0xE2400001, 0xE3700106, 0xE4C3C001, 0x9A000005, 0xE043C001, 0xE2500001,
    0xE5DCC000, 0xE4C3C001, 0x1AFFFFFA, 0xEA000011, 0xE3500000, 0xDA00000F, 0xE0431001, 0xE3100001,
    0xE243C001, 0xE2411001, 0x0A000001, 0xE5F14001, 0xE5EC4001, 0xE1B040C0, 0x0A000005, 0xE5D1B001,
    0xE2544001, 0xE5CCB001, 0xE5F1B002, 0xE5ECB002, 0x1AFFFFF9, 0xE0833000, 0xE3570000, 0x0A000004,
    0xE28AA001, 0xE35A0008, 0xE1A09089, 0x3AFFFFA6, 0xEAFFFFA0, 0xE3560020, 0xE8BD0FF0, 0xC3E00002,
    0xD3A00000, 0xE12FFF1E,
};
inline constexpr u32 kLzHeadCode[100] = {
    0xE4D59001, 0xE2466001, 0xE3560000, 0xA3A0A000, 0xBA000012, 0xE3190080, 0x0A000013, 0xE5D51000,
    0xE3580000, 0xE1A00221, 0x02800003, 0x0A000004, 0xE3500001, 0x0A000013, 0xE3500000, 0x12800001,
    0x0A00001A, 0xE4D51001, 0xE2566002, 0xE4D5C001, 0xE1A01E01, 0xE18C1A21, 0xE2811001, 0x5A00001A,
    0xE8BD0FF0, 0xE3E00001, 0xE12FFF1E, 0xE4D50001, 0xE2466001, 0xE3560000, 0xA2477001, 0xE4C30001,
    0xBAFFFFF6, 0xEA000035, 0xE1A00E01, 0xE5D51001, 0xE5F5C002, 0xE1A00820, 0xE2466002, 0xE1800201,
    0xE180022C, 0xE2800C01, 0xE2800011, 0xEAFFFFE4, 0xE1A00E01, 0xE5F51001, 0xE1A00C20, 0xE2466001,
    0xE1800221, 0xE2800011, 0xEAFFFFDD, 0xE1500007, 0x9043C001, 0x9152000C, 0x9A000002, 0xE8BD0FF0,
    0xE3E00003, 0xE12FFF1E, 0xE5DCC000, 0xE0477000, 0xE2400001, 0xE3700106, 0xE4C3C001, 0x9A000005,
    0xE043C001, 0xE2500001, 0xE5DCC000, 0xE4C3C001, 0x1AFFFFFA, 0xEA000011, 0xE3500000, 0xDA00000F,
    0xE0431001, 0xE3100001, 0xE243C001, 0xE2411001, 0x0A000001, 0xE5F14001, 0xE5EC4001, 0xE1B040C0,
    0x0A000005, 0xE5D1B001, 0xE2544001, 0xE5CCB001, 0xE5F1B002, 0xE5ECB002, 0x1AFFFFF9, 0xE0833000,
    0xE3570000, 0x0A000004, 0xE28AA001, 0xE35A0008, 0xE1A09089, 0x3AFFFFA6, 0xEAFFFFA0, 0xE3560020,
    0xE8BD0FF0, 0xC3E00002, 0xD3A00000, 0xE12FFF1E,
};

// Streaming Yaz0 (Super Mario 3D Land, Mario Kart 7): its loop's 184 words from its head (word 49 of the function).
// A context at r0 (+0 the output, +4 the output left, +8 a limit, +0xC the flags' mask, +0xD the flags, +0xE a
// reference's first byte, +0xF the state: 0 an item, 1 a reference's second byte, 2 its third, +0x10 the distance,
// +0x12 the header's bytes left), a chunk at r1 of r2 bytes; a byte of the chunk a turn (r6 0, r7 2, r8 0x80, r9 1).
inline constexpr u32 kYazStreamCode[184] = {
    0xE5D0300F, 0xE3530002, 0x0A000021, 0xE3530001, 0x0A000050, 0xE5D0300C, 0xE3530000, 0x0A000088,
    0xEA00008C, 0xE5D56000, 0xE3560059, 0x1A00000B, 0xEAFFFFDD, 0xE5D56000, 0xE3560061, 0x1A000007,
    0xEAFFFFD9, 0xE5D56000, 0xE356007A, 0x1A000003, 0xEAFFFFD5, 0xE5D56000, 0xE3560030, 0x0AFFFFD2,
    0xE1A0C007, 0xEAFFFFDB, 0xE5D03012, 0xE3530000, 0x1AFFFFD8, 0xEAFFFFD2, 0xE5D01012, 0xE3510000,
    0x05900004, 0x0A000000, 0xE3E00000, 0xE8BD0FF0, 0xE12FFF1E, 0xE4D13001, 0xE590C004, 0xE2422001,
    0xE2833012, 0xE15C0003, 0x2A000003, 0xE5903008, 0xE3530000, 0x16FF307C, 0x0A000047, 0xE04CC003,
    0xE3730106, 0xE580C004, 0x9A00000A, 0xE1D0C1B0, 0xE5904000, 0xE2533001, 0xE044C00C, 0xE5DCC000,
    0xE5C4C000, 0xE590C000, 0xE28CC001, 0xE580C000, 0x1AFFFFF5, 0xEA000050, 0xE590A000, 0xE3530001,
    0xD3A03001, 0xE3530000, 0xDA00004B, 0xE1D051B0, 0xE3130001, 0xE24A4001, 0xE04A5005, 0xE245C001,
    0x0A000001, 0xE5FC5001, 0xE5E45001, 0xE1B050C3, 0x0A000005, 0xE5DCB001, 0xE2555001, 0xE5C4B001,
    0xE5FCB002, 0xE5E4B002, 0x1AFFFFF9, 0xE083300A, 0xE5803000, 0xEA000038, 0xE5D0C00E, 0xE4D13001,
    0xE2422001, 0xE183340C, 0xE3C3CA0F, 0xE28C4001, 0xE1B03623, 0xE1C041B0, 0x05C0700F, 0x0A000049,
    0xE590C004, 0xE2833002, 0xE153000C, 0x9A000003, 0xE5903008, 0xE3530000, 0x16FF307C, 0x0A00000E,
    0xE04CC003, 0xE3730106, 0xE580C004, 0x9A00000D, 0xE1D041B0, 0xE590C000, 0xE2533001, 0xE04C4004,
    0xE5D44000, 0xE5CC4000, 0xE590C000, 0xE28CC001, 0xE580C000, 0x1AFFFFF5, 0xEA000017, 0xE8BD0FF0,
    0xE3E00001, 0xE12FFF1E, 0xE590A000, 0xE3530001, 0xD3A03001, 0xE3530000, 0xDA00000F, 0xE04AC004,
    0xE3130001, 0xE24A4001, 0xE24CC001, 0x0A000001, 0xE5FC5001, 0xE5E45001, 0xE1B050C3, 0x0AFFFFCA,
    0xE5DCB001, 0xE2555001, 0xE5C4B001, 0xE5FCB002, 0xE5E4B002, 0x1AFFFFF9, 0xEAFFFFC3, 0xE5C0600F,
    0xEA000018, 0xE4D13001, 0xE2522001, 0xE5C0300D, 0xE5C0800C, 0x0A000017, 0xE5D0300D, 0xE5D0C00C,
    0xE113000C, 0x0A000008, 0xE5903000, 0xE4D1C001, 0xE2834001, 0xE5804000, 0xE5C3C000, 0xE5903004,
    0xE2433001, 0xE5803004, 0xEA000002, 0xE4D13001, 0xE5C0300E, 0xE5C0900F, 0xE5D0300C, 0xE2422001,
    0xE1A030A3, 0xE5C0300C, 0xE3520000, 0x15903004, 0x13530000, 0xCAFFFF51, 0xE5901004, 0xE3510000,
    0x05900008, 0x03500000, 0x1A000001, 0xE3520020, 0x8AFFFF6C, 0xE8BD0FF0, 0xE1A00001, 0xE12FFF1E,
};

// Streaming Yaz0, Captain Toad's (Tri Force Heroes, Miitopia): the same context as kYazStreamCode's, its fields in
// registers through the loop (r3 the output, r7 the output left, r4 the state, r12 the distance, r9 the mask, lr the
// flags, r10 a reference's first byte; stored back at word 189): the loop's 128 words from its head (word 76).
inline constexpr u32 kYazStreamTCode[128] = {
    0xE3540002, 0x0A00000A, 0xE3540001, 0x0A00002D, 0xE3590000, 0x0A000059, 0xEA00005C, 0xE5D01012,
    0xE3510000, 0x05900004, 0x0AFFFFD2, 0xE3E00000, 0xE8BD8FF0, 0xE4D14001, 0xE2422001, 0xE2848012,
    0xE1580007, 0x9A000003, 0xE5904008, 0xE3540000, 0x16FF8077, 0x0A000033, 0xE3780106, 0xE0477008,
    0x9A000005, 0xE043400C, 0xE2588001, 0xE5D44000, 0xE4C34001, 0x1AFFFFFA, 0xEA00003E, 0xE3580001,
    0xE043400C, 0xD3A08001, 0xE2445001, 0xE3180001, 0xE2434001, 0x0A000001, 0xE5F56001, 0xE5E46001,
    0xE1B060C8, 0x0A000005, 0xE5D5B001, 0xE2566001, 0xE5C4B001, 0xE5F5B002, 0xE5E4B002, 0x1AFFFFF9,
    0xE0833008, 0xEA00002B, 0xE4D1C001, 0xE2422001, 0xE18CC40A, 0xE3CC5A0F, 0xE1B0462C, 0xE285C001,
    0x03A04002, 0x0A000033, 0xE284B002, 0xE15B0007, 0x9A000003, 0xE5904008, 0xE3540000, 0x16FFB077,
    0x0A000008, 0xE37B0106, 0xE047700B, 0x9A000007, 0xE043400C, 0xE25BB001, 0xE5D44000, 0xE4C34001,
    0x1AFFFFFA, 0xEA000013, 0xE3E00001, 0xE8BD8FF0, 0xE35B0001, 0xE043400C, 0xD3A0B001, 0xE2445001,
    0xE31B0001, 0xE2434001, 0x0A000001, 0xE5F56001, 0xE5E46001, 0xE1B060CB, 0x0A000005, 0xE5D58001,
    0xE2566001, 0xE5C48001, 0xE5F58002, 0xE5E48002, 0x1AFFFFF9, 0xE083300B, 0xE3A04000, 0xEA00000D,
    0xE4D1E001, 0xE2522001, 0xE3A09080, 0x0A00000C, 0xE11E0009, 0x0A000003, 0xE4D15001, 0xE2477001,
    0xE4C35001, 0xEA000001, 0xE4D1A001, 0xE3A04001, 0xE1A090A9, 0xE2422001, 0xE3520000, 0x13570000,
    0xCAFFFF8E, 0xE5C0400F, 0xE5803000, 0xE1C0C1B0, 0xE5C0900C, 0xE5C0E00D, 0xE5C0A00E, 0xE3570000,
    0xE5807004, 0x05900008, 0x03500000, 0x1A000001, 0xE3520020, 0x8AFFFF8C, 0xE1A00007, 0xE8BD8FF0,
};

// CRC-16 CCITT a bit at a time (poly 0x1021 in a register, a mask 0xFFFF in another): r0 the CRC, r1 the bytes, r2
// their count; for each byte 4 turns of two steps (tst r0, #0x8000; the select of mask & r0 << 1 or poly ^ r0 << 1;
// uxthne, a run of its own), then ldrb r3, [r1], #1; the count up; cmp count, r2; eor r0, r0, r3; blo to the head.
// From each loop's head (mov r3, #4), 16 words. A and B (Super Mario 3D Land, Sonic Generations: andeq before eorne;
// the count r5 or r6), N and S (Kirby: Planet Robobot, Mario Kart 7, Miitopia, New Super Mario Bros. 2, Smash: eorne,
// uxthne, then andeq; poly and mask r5/r4 or r4/r6; the count r12).
inline constexpr u32 kCrc16ACode[16] = {
    0xE3A03004, 0xE3100902, 0x00040080, 0x102C0080, 0x16FF0070, 0xE3100902, 0x00040080, 0x102C0080,
    0x16FF0070, 0xE2533001, 0x1AFFFFF5, 0xE4D13001, 0xE2855001, 0xE1550002, 0xE0200003, 0x3AFFFFEF,
};
inline constexpr u32 kCrc16BCode[16] = {
    0xE3A03004, 0xE3100902, 0x00040080, 0x102C0080, 0x16FF0070, 0xE3100902, 0x00040080, 0x102C0080,
    0x16FF0070, 0xE2533001, 0x1AFFFFF5, 0xE4D13001, 0xE2866001, 0xE1560002, 0xE0200003, 0x3AFFFFEF,
};
inline constexpr u32 kCrc16NCode[16] = {
    0xE3A03004, 0xE3100902, 0x10250080, 0x16FF0070, 0x00040080, 0xE3100902, 0x10250080, 0x16FF0070,
    0x00040080, 0xE2533001, 0x1AFFFFF5, 0xE4D13001, 0xE28CC001, 0xE15C0002, 0xE0200003, 0x3AFFFFEF,
};
inline constexpr u32 kCrc16SCode[16] = {
    0xE3A03004, 0xE3100902, 0x10240080, 0x16FF0070, 0x00060080, 0xE3100902, 0x10240080, 0x16FF0070,
    0x00060080, 0xE2533001, 0x1AFFFFF5, 0xE4D13001, 0xE28CC001, 0xE15C0002, 0xE0200003, 0x3AFFFFEF,
};

// kListExits: a loop head with two exits (words*4: the list's end, r4 0; words*4 + 8: found), its caller told by the
// pc left in r15, and a third way back to its head when it stopped at its limit of turns.
// kWholeOrFirst: the routine whole up to its word `done` (left to the JIT: its return), when all its ticks to there
// fit in what's left of the JIT's slice (the JIT would run that far without stopping); else only its first block, as
// the JIT makes it, and on at its word `resume` (the JIT's from there: it stops where the JIT stops).
// kStopsLikeJit: the routine runs on while the JIT would (it stops at the end of the block where the JIT's slice
// ends, r15 that block's next word, back to the dispatcher), or to its word `done` (left to the JIT: its return).
enum Exit : u32 {
    kLoopHead = 0,
    kBxLr = 1,
    kMovPcLr = 2,
    kDivide = 3,
    kListExits = 4,
    kWholeOrFirst = 5,
    kStopsLikeJit = 6,
    kHostPc = 7,  // the routine leaves r15 (where it stopped like the JIT, or its return's block) for the dispatcher
};
struct Signature {
    Kind kind;
    const char* name;
    const char* word;  // its name in the switch files (the word that turns it off)
    const u32* code;
    u32 words;
    u32 literal[2];    // byte offsets of the literals it loads (from the start), 0: none
    Exit exit;         // bx lr / mov pc, lr at its end, a loop head (exit at the words' end), or the division's
    u32 entry;         // the word of `code` where its block starts
    bool castlevania;  // only in Castlevania: Mirror of Fate (its own switch)
    u32 resume = 0;    // kWholeOrFirst: the word after its first block
    u32 done = 0;      // kWholeOrFirst, kStopsLikeJit: the word left to the JIT (its return)
    bool on = true;    // on by default (else only with +its word in the switch file: slower or no faster on the PC)
};
// A B/BL word's offset differs where the code is elsewhere: only its condition and opcode are compared.
inline bool SameWord(u32 reference, u32 word) {
    if ((reference & 0x0E000000) == 0x0A000000)
        return (reference & 0xFF000000) == (word & 0xFF000000);
    return reference == word;
}
inline u32 BranchTarget(u32 at, u32 word) {
    const u32 off = word & 0xFFFFFF;
    return at + 8 + ((off & 0x800000) ? (off | 0xFF000000u) : off) * 4;
}
inline constexpr Signature kSignatures[] = {
    {kCommandWriter, "the GPU command writer", "writer", kCommandWriterCode, 56, {0xE0, 0}, kBxLr, 0, false, 0, 0,
     false},
    {kFloat24Loop, "the float24 loop of uniforms", "float24", kFloat24LoopCode, 19, {0, 0}, kLoopHead, 0, false, 0,
     0, false},
    {kHalf3, "three halves and their command", "half3", kHalf3Code, 73, {0x124, 0x128}, kBxLr, 0, false, 0, 0,
     false},
    {kStrcmp, "strcmp (RVCT)", "strcmp", kStrcmpCode, 46, {0xB8, 0}, kMovPcLr, 0, false, 0, 0, false},
    {kStrlen, "strlen (RVCT)", "strlen", kStrlenCode, 24, {0x60, 0}, kMovPcLr, 0, false},
    {kMemcpy4, "memcpy of aligned words (RVCT)", "memcpy4", kMemcpy4Code, 29, {0, 0}, kBxLr, 0, false, 0, 0, false},
    {kDivSigned, "__aeabi_idivmod (RVCT)", "division", kDivideCode, 139, {0, 0}, kDivide, 0, false},
    {kDivSignedPath, "__aeabi_idivmod's negative path (RVCT)", "division", kDivideCode, 139, {0, 0}, kDivide, 80,
     false},
    {kDivUnsigned, "__aeabi_uidivmod (RVCT)", "division", kUdivEntryCode, 12, {0, 0}, kDivide, 0, false},
    {kListWalk, "Castlevania's list search loop", "listwalk", kListWalkCode, 10, {0, 0}, kListExits, 0, true},
    {kHeapBestFit, "Castlevania's heap best-fit search", "heapfit", kHeapBestFitCode, 37, {0, 0}, kBxLr, 0, true},
    {kCrc32, "CRC-32 with its table built on the stack", "crc32", kCrc32Code, 51, {0xCC, 0}, kWholeOrFirst, 0, false, 8,
     50},
    {kYaz0Rev, "Yaz0 decompression", "yaz0", kYaz0RevCode, 39, {0, 0}, kStopsLikeJit, 0, false, 0, 36},
    {kYaz0Swap, "Yaz0 decompression (its size swapped by hand)", "yaz0", kYaz0SwapCode, 42, {0, 0}, kStopsLikeJit, 0,
     false, 0, 39},
    {kYaz0Head, "Yaz0 decompression's loop", "yaz0", kYaz0HeadCode, 31, {0, 0}, kStopsLikeJit, 0, false, 0, 28},
    {kLz, "LZ10/LZ11 decompression", "lz", kLzCode, 138, {0, 0}, kHostPc, 0, false},
    {kLzHead, "LZ10/LZ11 decompression's item", "lz", kLzHeadCode, 100, {0, 0}, kHostPc, 5, false},
    {kYazStream, "streaming Yaz0 decompression", "yaz0s", kYazStreamCode, 184, {0, 0}, kHostPc, 0, false},
    {kYazStreamT, "streaming Yaz0 decompression (its fields in registers)", "yaz0s", kYazStreamTCode, 128, {0, 0},
     kHostPc, 0, false},
    {kCrc16A, "CRC-16 CCITT bit by bit", "crc16", kCrc16ACode, 16, {0, 0}, kHostPc, 0, false},
    {kCrc16B, "CRC-16 CCITT bit by bit (count in r6)", "crc16", kCrc16BCode, 16, {0, 0}, kHostPc, 0, false},
    {kCrc16N, "CRC-16 CCITT bit by bit (Nintendo's order)", "crc16", kCrc16NCode, 16, {0, 0}, kHostPc, 0, false},
    {kCrc16S, "CRC-16 CCITT bit by bit (Nintendo's order, r4/r6)", "crc16", kCrc16SCode, 16, {0, 0}, kHostPc, 0,
     false},
};

// A place where one was found: its literals' values and each word's ticks (Core::TicksForInstruction). The
// division: literal[0] the body's address, literal[1] __aeabi_idiv0's; ticks the body's, entry_ticks the unsigned
// entry's.
struct Site {
    Kind kind = kNone;
    u32 pc = 0;
    u32 literal[2] = {0, 0};
    std::uint16_t ticks[192] = {};
    std::uint16_t entry_ticks[16] = {};
    // (the JIT's side, set where its block is made) dynarmic's state (A32JitState: its registers, flags, VFP
    // registers), the page table's pointers and the callbacks (Azahar's own accesses)
    void* state = nullptr;
    std::uint8_t* const* raw = nullptr;
    void* host = nullptr;
    const u32* table = nullptr;  // kCrc32: its polynomial's table (CrcTable)
};

// The reflected CRC-32 table of a polynomial as the routine builds it (8 steps an entry), kept for the process (four
// polynomials at most: nullptr past them).
inline const u32* CrcTable(u32 poly) {
    static u32 polys[4];
    static u32 tables[4][256];
    static int count = 0;
    for (int i = 0; i < count; i++)
        if (polys[i] == poly)
            return tables[i];
    if (count == 4)
        return nullptr;
    for (u32 k = 0; k < 256; k++) {
        u32 x = k;
        for (int step = 0; step < 8; step++)
            x = (x & 1) ? (poly ^ (x >> 1)) : (x >> 1);
        tables[count][k] = x;
    }
    polys[count] = poly;
    return tables[count++];
}

struct Flags {
    bool n, z, c, v;
};
inline Flags FlagsOf(u32 cpsr) {
    return {((cpsr >> 31) & 1) != 0, ((cpsr >> 30) & 1) != 0, ((cpsr >> 29) & 1) != 0, ((cpsr >> 28) & 1) != 0};
}
inline u32 BitsOf(Flags f) {
    return (u32(f.n) << 31) | (u32(f.z) << 30) | (u32(f.c) << 29) | (u32(f.v) << 28);
}
inline Flags Sub(u32 a, u32 b) {  // CMP / SUBS
    const u32 r = a - b;
    return {(r >> 31) != 0, r == 0, a >= b, (((a ^ b) & (a ^ r)) >> 31) != 0};
}
inline Flags Add(u32 a, u32 b) {  // CMN / ADDS
    const u32 r = a + b;
    return {(r >> 31) != 0, r == 0, r < a, ((~(a ^ b) & (a ^ r)) >> 31) != 0};
}
// ANDS/BICS with an immediate rotated into bit 31 (0x80000000): N, Z of the result, C the immediate's bit 31 (1),
// V as it was.
inline Flags Logic(u32 r, Flags old) {
    return {(r >> 31) != 0, r == 0, true, old.v};
}

struct Cpu {
    u32* r;     // r0-r15 (r15 untouched)
    u32 s0;     // VFP s0 (the float24 loop's)
    Flags f;    // NZCV
    u64 ticks;  // the 3DS CPU's ticks the routine took
};

// Memory: Mem has u32 Read32(u32) and void Write32(u32, u32) (8 and 16 too), and std::uint8_t* Span(u32 address,
// u32 bytes): the host's pointer to those bytes when the address is a word's and they sit in one page with a pointer
// (plain memory), else nullptr. A push, a pop or an LDM/STM of plain memory is one copy (its order can't be seen
// there); else word by word in the instruction's order, as before. Bytes(address, n): the same for bytes from any
// address (nullptr unless all n sit in one page with a pointer).
template <class Mem>
inline void ReadWords(Mem& m, u32 address, u32* out, u32 n) {
    if (const std::uint8_t* p = m.Span(address, 4 * n)) {
        std::memcpy(out, p, 4 * n);
        return;
    }
    for (u32 i = 0; i < n; i++)
        out[i] = m.Read32(address + 4 * i);
}
template <class Mem>
inline void WriteWords(Mem& m, u32 address, const u32* in, u32 n) {
    if (std::uint8_t* p = m.Span(address, 4 * n)) {
        std::memcpy(p, in, 4 * n);
        return;
    }
    for (u32 i = 0; i < n; i++)
        m.Write32(address + 4 * i, in[i]);
}
// Many words: a page at a time.
template <class Mem>
inline void WriteRange(Mem& m, u32 address, const u32* in, u32 n) {
    while (n != 0) {
        const u32 room = (0x1000 - (address & 0xFFF)) / 4;
        const u32 k = room == 0 ? 1 : (room < n ? room : n);
        WriteWords(m, address, in, k);
        address += 4 * k;
        in += k;
        n -= k;
    }
}

template <class Mem>
void CommandWriter(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    auto tk = [&](int a, int b) {
        for (int i = a; i <= b; i++)
            t += T[i];
    };
    const u32 r0 = r[0], r1 = r[1], r2 = r[2];
    u32 r3 = r[3], r4, r5, r6, r7 = r[7], r8, r9, r10, r12;
    const u32 sp = r[13] - 28;
    WriteWords(m, sp, r + 4, 7);  // 0 push {r4-r10}
    r4 = r1 >> 7;          // 1
    r12 = r1 & ~1u;        // 2
    r4 = r12 + (r4 << 1);  // 3
    r6 = s.literal[0];     // 4
    r3 = m.Read32(r6);     // 5 ldm r6, {r3, r12}
    r12 = m.Read32(r6 + 4);
    r4 = r3 + (r4 << 2);  // 6
    r4 = r4 + 8;          // 7
    Flags f = Sub(r12, r4);  // 8
    tk(0, 8);
    if (!f.c || f.z) {  // 9 strls, 10 bls to the pop (54): no room, the list left "full" (its current = its end)
        m.Write32(r6, r12);
        tk(9, 10);
        ReadWords(m, sp, r + 4, 7);  // 54 pop {r4-r10}
        tk(54, 55);
        r[3] = r3;
        r[12] = r12;
        c.f = f;
        c.ticks = t;
        return;
    } else {
        t += 2;  // (a condition-failed run of two: dynarmic counts one tick each)
        f = Sub(r1, 0);  // 11
        r5 = 0;          // 12
        if (f.c && !f.z) // 13 subhi
            r7 = r5 - 0x100000;
        tk(11, 14);
        if (f.c && !f.z) {  // 14 bls (not taken)
            for (;;) {
                r12 = r2 + (r5 << 2);   // 15
                r8 = r1 - r5;           // 16
                r4 = m.Read32(r12);     // 17
                r12 += 4;
                f = Sub(r8, 128);       // 18
                if (f.c && !f.z)        // 19 movhi
                    r8 = 128;
                m.Write32(r3, r4);      // 20
                r3 += 4;
                r4 = r8 >> 1;           // 21
                r9 = r0 + r5;           // 22
                r8 = r7 + (r8 << 20);   // 23
                r8 |= r9;               // 24
                r8 |= 0x80000000u;      // 25
                r8 |= 0xF0000u;         // 26
                f = Add(r4, 0x80000001u);  // 27 cmn
                m.Write32(r3, r8);      // 28
                r3 += 4;
                tk(15, 29);
                if (f.c && !f.z) {  // 29 bls not taken: the pairs, post-indexed (30-35), then b (36)
                    do {
                        r8 = m.Read32(r12);  // 30
                        r12 += 4;
                        f = Sub(r4, 1);      // 31
                        r4 -= 1;
                        m.Write32(r3, r8);   // 32
                        r3 += 4;
                        r8 = m.Read32(r12);  // 33
                        r12 += 4;
                        m.Write32(r3, r8);   // 34
                        r3 += 4;
                        tk(30, 35);
                    } while (!f.z);
                    t += T[36];
                } else {
                    r9 = r4 << 1;      // 37
                    f = Sub(r9, 0);    // 38
                    tk(37, 39);
                    if (!(f.z || f.n != f.v)) {  // 39 ble not taken
                        r12 -= 4;                                          // 40
                        r8 = r3 - 4;                                       // 41
                        r9 = static_cast<u32>(static_cast<std::int32_t>(r9) >> 1);  // 42
                        tk(40, 42);
                        do {
                            r10 = m.Read32(r12 + 4);  // 43
                            f = Sub(r9, 1);           // 44
                            r9 -= 1;
                            m.Write32(r8 + 4, r10);   // 45
                            r12 += 8;                 // 46
                            r10 = m.Read32(r12);
                            r8 += 8;                  // 47
                            m.Write32(r8, r10);
                            tk(43, 48);
                        } while (!f.z);
                        r3 = r3 + (r4 << 3);  // 49
                        t += T[49];
                    }
                }
                r5 += 128;        // 50
                f = Sub(r5, r1);  // 51
                tk(50, 52);
                if (f.c)          // 52 blo not taken
                    break;
            }
        }
    }
    m.Write32(r6, r3);  // 53
    ReadWords(m, sp, r + 4, 7);  // 54 pop {r4-r10}
    tk(53, 55);
    r[3] = r3;
    r[12] = r12;
    c.f = f;
    c.ticks = t;
}

// The loop's turns from its head, at most `limit` (true: the loop ended).
template <class Mem>
bool Float24Loop(Cpu& c, Mem& m, const Site& s, u32 limit) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    // each turn's ticks by its way: 0 (0-3, 11-18), a small exponent (0-10, 16-18), else (0-18)
    u64 head = 0, small = 0, tail = 0;
    for (int i = 0; i <= 3; i++)
        head += T[i];
    for (int i = 4; i <= 10; i++)
        small += T[i];
    for (int i = 11; i <= 15; i++)
        tail += T[i];
    const u64 end = T[16] + T[17] + T[18];
    const u64 zero_turn = head + tail + end, small_turn = head + small + end, full_turn = head + small + tail + end;
    u64 t = 0;
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r12 = r[12], lr = r[14], s0 = c.s0;
    Flags f = c.f;
    u32 turns = 0;
    do {
        s0 = m.Read32(r3);  // 0 vldmia r3!, {s0}
        r3 += 4;
        r0 = s0;                  // 1 vmov r0, s0
        r12 = r0 & 0x7FFFFFFFu;   // 2 bics
        f = Logic(r12, f);
        if (f.z) {         // 3 beq
            t += zero_turn;
        } else {
            r12 = (r0 << 1) >> 24;  // 4, 5
            r12 -= 64;              // 6
            f = Sub(r12, 0);        // 7
            if (f.n != f.v) {  // 8 lsrlt, 9 lsllt, 10 blt
                r0 = (r0 >> 31) << 23;
                t += small_turn;
                goto stored;
            }
            t += full_turn;
        }
        lr = r0 << 9;               // 11
        r0 = r0 >> 31;              // 12
        lr = lr >> 16;              // 13
        r12 = lr | (r12 << 16);     // 14
        r0 = r12 | (r0 << 23);      // 15
    stored:
        f = Sub(r1, 1);  // 16 subs
        r1 -= 1;
        m.Write32(r2, r0);  // 17
        r2 += 4;            // 18 bne
        turns++;
    } while (!f.z && turns < limit);
    r[0] = r0;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[12] = r12;
    r[14] = lr;
    c.s0 = s0;
    c.f = f;
    c.ticks = t;
    return f.z;
}

// One float's half, as Half3 does it (the three are the same but for their registers): x its bits; sign, mant, exp
// and the result left as the code leaves them; `at` the first word's index (ands), so the ticks come from its words.
struct HalfOut {
    u32 sign_reg;  // the register that ends as the half (0, 0x8000, 0x7C00 or the value)
    u32 mant;      // the mantissa register
    u32 exp_reg;   // the exponent register (exp, or exp & 31 on the full path)
    u32 work;      // the abs/sub register (abs, exp - 112, or mant | sign on the full path)
    Flags f;
};

template <class Mem>
void Half3(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    Flags f = c.f;
    const u32 sp = r[13] - 16;
    WriteWords(m, sp, r + 4, 4);  // 0 push {r4-r7}
    const u32 index = r[0], from = r[1];
    const u32 r12 = 0;        // 1
    u32 x0 = m.Read32(from);  // 2
    const u32 header = s.literal[0];  // 3
    u32 x1 = m.Read32(from + 4);      // 4
    t += T[0] + T[1] + T[2] + T[3] + T[4];
    // One half: x the float's bits. first: the index of its ands; the code's layout is the same for all three
    // (ands, ..., bics, ..., lsreq, beq +5 | subs, lsr, bmi +5 | cmp, movgt, bgt +2 | and, orr, orr), only some
    // unrelated instructions of the next half (or the uxth) sit in between, counted where they are.
    auto half = [&](u32 x, int beq, int bmi, int bgt, int end, u32& res, u32& mant, u32& exp_reg, u32& work) {
        res = x & 0x80000000u;  // ands
        f = Logic(res, f);
        mant = (x << 9) >> 9;
        exp_reg = x << 1;  // (lsl #1, then lsr #24)
        if (!f.z)  // movne
            res = 0x8000;
        work = x & 0x7FFFFFFFu;  // bics
        f = Logic(work, f);
        exp_reg >>= 24;
        if (f.z) {  // lsreq; beq to the and
            mant >>= 13;
            (void)beq;
            exp_reg = work & 31;          // and
            work = mant | res;            // orr
            res = work | (exp_reg << 10); // orr
            return 0;  // path: zero
        }
        work = exp_reg - 112;  // subs
        f = Sub(exp_reg, 112);
        mant >>= 13;
        if (f.n)  // bmi
            return 1;
        f = Sub(work, 31);  // cmp
        if (!f.z && f.n == f.v) {  // movgt, bgt
            res = 0x7C00;
            return 2;
        }
        exp_reg = work & 31;
        work = mant | res;
        res = work | (exp_reg << 10);
        (void)bmi;
        (void)bgt;
        (void)end;
        return 3;  // the full path
    };
    // Half 1: x0 in r3; r5 the result, r6 mant, r7 exp; words 5-22 (beq 13 -> 20, bmi 16 -> 23, bgt 19 -> 23).
    u32 r5, r6, r7, r3w;
    const int p1 = half(x0, 13, 16, 19, 22, r5, r6, r7, r3w);
    // its ticks: 5-13, then 20-22 (zero) | 14-16 (minus) | 14-19 (over) | 14-22 (full)
    for (int i = 5; i <= 13; i++)
        t += T[i];
    if (p1 == 0)
        t += T[20] + T[21] + T[22];
    else
        for (int i = 14; i <= (p1 == 1 ? 16 : p1 == 2 ? 19 : 22); i++)
            t += T[i];
    // Half 2: x1 in r2; r3 the result, r6 mant, r7 exp; words 23-41, with uxth r5, r5 at 30 (before its beq at 32).
    u32 r3, r2w, r6b, r7b;
    const int p2 = half(x1, 32, 35, 38, 41, r3, r6b, r7b, r2w);
    r5 &= 0xFFFFu;  // 30 uxth
    for (int i = 23; i <= 32; i++)
        t += T[i];
    if (p2 == 0)
        t += T[39] + T[40] + T[41];
    else
        for (int i = 33; i <= (p2 == 1 ? 35 : p2 == 2 ? 38 : 41); i++)
            t += T[i];
    u32 r2 = r5 | (r3 << 16);        // 42
    r3 = header + (index << 4);      // 43
    u32 x2 = m.Read32(from + 8);     // 44 ldr r0, [r1, #8]
    t += T[42] + T[43] + T[44];
    // Half 3: x2 in r0; r1 the result, r4 mant, r5 exp; words 45-62 (beq 53 -> 60, bmi 56 -> 63, bgt 59 -> 63).
    u32 r1, r4, r5c, r0w;
    const int p3 = half(x2, 53, 56, 59, 62, r1, r4, r5c, r0w);
    for (int i = 45; i <= 53; i++)
        t += T[i];
    if (p3 == 0)
        t += T[60] + T[61] + T[62];
    else
        for (int i = 54; i <= (p3 == 1 ? 56 : p3 == 2 ? 59 : 62); i++)
            t += T[i];
    const u32 list = s.literal[1];   // 63 ldr r4, [pc, #36]
    const u32 r0 = r1 & 0xFFFFu;     // 64 uxth r0, r1
    const u32 cur = m.Read32(list);  // 65 ldr r1, [r4]
    m.Write32(cur + 8, r0);          // 68 stm r6, {r0, r12} (r6 = cur + 8, 66)
    m.Write32(cur + 12, r12);
    m.Write32(cur, r2);              // 69 strd r2, r3, [r1]
    m.Write32(cur + 4, r3);
    m.Write32(list, cur + 16);       // 70 str r5, [r4] (r5 = cur + 16, 67)
    ReadWords(m, sp, r + 4, 4);      // 71 pop {r4-r7}
    for (int i = 63; i <= 72; i++)
        t += T[i];
    (void)r6;
    (void)r7;
    (void)r3w;
    (void)r6b;
    (void)r7b;
    (void)r2w;
    (void)r4;
    (void)r5c;
    (void)r0w;
    r[0] = r0;
    r[1] = cur;
    r[2] = r2;
    r[3] = r3;
    r[12] = r12;
    c.f = f;
    c.ticks = t;
}

// RVCT's strcmp (46 words): a word at a time while both are aligned (the 0x01010101 literal finds a zero byte), else
// a byte at a time. Its flag-setting conditional instructions (tsteq, cmpeq, cmphs, subseq) and the conditional
// branches right after them each start a block of dynarmic's: one tick when their condition fails.
template <class Mem>
void Strcmp(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    Flags f = c.f;
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r12 = r[12];
    auto cond_tick = [&](bool passed, int i) { t += passed ? T[i] : 1; };
    {
        const u32 x = r0 & 3;  // 0 tst r0, #3 (C, V as they were)
        f.n = false;
        f.z = x == 0;
        t += T[0];
        const bool eq = f.z;  // 1 tsteq r1, #3
        if (eq) {
            const u32 y = r1 & 3;
            f.n = false;
            f.z = y == 0;
        }
        cond_tick(eq, 1);
    }
    bool words = f.z;  // 2 bne to the bytes (34)
    cond_tick(!words, 2);
    if (words) {
        const u32 lr = s.literal[0];  // 3 push {lr}, 4 ldr lr, =0x01010101
        const u32 sp = r[13] - 4;
        m.Write32(sp, r[14]);
        t += T[3] + T[4];
        const bool carry = ((lr >> 25) & 1) != 0;  // (lsl #7's carry out)
        // 5-11, then 12-18, again from 5 while equal
        int half = 0;
        for (;;) {
            const int b = half == 0 ? 5 : 12;
            r2 = m.Read32(r0);
            r0 += 4;
            r3 = m.Read32(r1);
            r1 += 4;
            r12 = r2 - lr;
            r12 &= ~r2;
            r12 &= lr << 7;  // ands
            f.n = (r12 >> 31) != 0;
            f.z = r12 == 0;
            f.c = carry;
            t += T[b] + T[b + 1] + T[b + 2] + T[b + 3] + T[b + 4];
            const bool eq = f.z;  // cmpeq r2, r3
            if (eq)
                f = Sub(r2, r3);
            cond_tick(eq, b + 5);
            if (half == 0) {
                cond_tick(!f.z, 11);  // 11 bne to 19
                if (!f.z)
                    break;
                half = 1;
            } else {
                cond_tick(f.z, 18);  // 18 beq to 5
                if (!f.z)
                    break;
                half = 0;
            }
        }
        r[14] = m.Read32(sp);  // 19 pop {lr}
        t += T[19];
        // 20-33: the first byte that differs or ends, from the lowest
        r0 = r3 << 24;  // 20
        f = Sub(r0, r2 << 24);
        r0 = r0 - (r2 << 24);  // 21 subs
        t += T[20] + T[21];
        bool eq = f.z;  // 22 tsteq r12, #0xFF0
        if (eq) {
            const u32 x = r12 & 0xFF0u;
            f.n = (x >> 31) != 0;
            f.z = x == 0;
            f.c = false;
        }
        cond_tick(eq, 22);
        bool done = !f.z;  // 23 bne to 32
        cond_tick(done, 23);
        if (!done) {
            r0 = r3 << 16;  // 24
            f = Sub(r0, r2 << 16);
            r0 = r0 - (r2 << 16);  // 25
            t += T[24] + T[25];
            eq = f.z;  // 26 tsteq r12, #0xFF00
            if (eq) {
                const u32 x = r12 & 0xFF00u;
                f.n = (x >> 31) != 0;
                f.z = x == 0;
                f.c = false;
            }
            cond_tick(eq, 26);
            done = !f.z;  // 27 bne to 32
            cond_tick(done, 27);
            if (!done) {
                r0 = r3 << 8;  // 28
                f = Sub(r0, r2 << 8);
                r0 = r0 - (r2 << 8);  // 29
                t += T[28] + T[29];
                eq = f.z;  // 30 tsteq r12, #0xFF0000
                if (eq) {
                    const u32 x = r12 & 0xFF0000u;
                    f.n = (x >> 31) != 0;
                    f.z = x == 0;
                    f.c = false;
                }
                cond_tick(eq, 30);
                eq = f.z;  // 31 subseq r0, r3, r2
                if (eq) {
                    f = Sub(r3, r2);
                    r0 = r3 - r2;
                }
                cond_tick(eq, 31);
            }
        }
        if (!f.z)  // 32 rrxne r0, r0 (a select inside its block: counted either way)
            r0 = (u32(f.c) << 31) | (r0 >> 1);
        t += T[32] + T[33];  // 33 mov pc, lr
    } else {
        // 34-43, again from 34 while equal, then 44-45
        int half = 0;
        for (;;) {
            const int b = half == 0 ? 34 : 39;
            r2 = m.Read8(r0);
            r0 += 1;
            r3 = m.Read8(r1);
            r1 += 1;
            f = Sub(r2, 1);  // cmp r2, #1
            t += T[b] + T[b + 1] + T[b + 2];
            const bool hs = f.c;  // cmphs r2, r3
            if (hs)
                f = Sub(r2, r3);
            cond_tick(hs, b + 3);
            if (half == 0) {
                cond_tick(!f.z, 38);  // 38 bne to 44
                if (!f.z)
                    break;
                half = 1;
            } else {
                cond_tick(f.z, 43);  // 43 beq to 34
                if (!f.z)
                    break;
                half = 0;
            }
        }
        r0 = r2 - r3;  // 44
        t += T[44] + T[45];
    }
    r[0] = r0;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[12] = r12;
    c.f = f;
    c.ticks = t;
}

// RVCT's integer division (kDivideCode; the unsigned entry kUdivEntryCode): from `kind`'s entry the code's own
// steps (rsbs/subhs/adcs a bit of quotient each, its sentinel bits in r2, its loop for big quotients), every flag
// and register as it leaves them. True: the divisor was 0 and the code went to the handler's bl __aeabi_idiv0 (r0
// pushed with lr, r0 0, lr the pop after the bl): the caller goes on at __aeabi_idiv0 (literal[1]).
template <class Mem>
bool Divide(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    const std::uint16_t* const U = s.entry_ticks;
    u64 t = 0;
    Flags f = c.f;
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r12 = r[12];
    auto tk = [&](int a, int b) {
        for (int i = a; i <= b; i++)
            t += T[i];
    };
    auto rsbs_lsr = [&](int k) {  // rsbs r3, r1, r0, lsr #k
        f = Sub(r0 >> k, r1);
        r3 = (r0 >> k) - r1;
    };
    auto adcs = [&]() {  // adcs r2, r2, r2
        const u64 sum = u64(r2) + r2 + (f.c ? 1 : 0);
        const u32 res = static_cast<u32>(sum);
        f.n = (res >> 31) != 0;
        f.z = res == 0;
        f.v = ((r2 ^ res) >> 31) != 0;
        f.c = (sum >> 32) != 0;
        r2 = res;
    };
    auto step = [&](int k, int i) {  // i: its rsbs's word
        rsbs_lsr(k);
        if (f.c)  // subhs r0, r0, r1, lsl #k
            r0 -= r1 << k;
        adcs();
        tk(i, i + 2);
    };
    auto finish = [&]() {
        r[0] = r0;
        r[1] = r1;
        r[2] = r2;
        r[3] = r3;
        r[12] = r12;
        c.f = f;
        c.ticks = t;
    };
    switch (s.kind) {
    case kDivSignedPath:
        goto i80;
    case kDivUnsigned:
        goto u0;
    default:
        break;
    }
    // __aeabi_idivmod
    r3 = r0 | r1;  // 0 orrs
    f.n = (r3 >> 31) != 0;
    f.z = r3 == 0;
    tk(0, 1);
    if (f.n)  // 1 bmi
        goto i80;
    rsbs_lsr(1);  // 2
    tk(2, 3);
    if (!f.c)  // 3 blo
        goto i73;
    r2 = 0;  // 4 movs r2, #0
    f.n = false;
    f.z = true;
    t += T[4];
    rsbs_lsr(4);  // 5
    tk(5, 6);
    if (!f.c)
        goto i60;
    rsbs_lsr(8);  // 7
    tk(7, 8);
    if (!f.c)
        goto i48;
    rsbs_lsr(12);  // 9
    tk(9, 10);
    if (!f.c)
        goto i35;
    rsbs_lsr(16);  // 11
    tk(11, 12);
    if (!f.c)
        goto i23;
    goto i13;
u0:
    // __aeabi_uidivmod's entry
    r2 = 0;
    f.n = false;
    f.z = true;
    t += U[0];
    rsbs_lsr(2);
    t += U[1] + U[2];
    if (!f.c)
        goto i66;
    rsbs_lsr(5);
    t += U[3] + U[4];
    if (!f.c)
        goto i57;
    rsbs_lsr(8);
    t += U[5] + U[6];
    if (!f.c)
        goto i48;
    rsbs_lsr(12);
    t += U[7] + U[8];
    if (!f.c)
        goto i35;
    rsbs_lsr(16);
    t += U[9] + U[10];
    if (!f.c)
        goto i23;
    t += U[11];  // b to 13
i13:
    r1 <<= 8;      // 13
    rsbs_lsr(16);  // 14
    r2 |= 0xFF000000u;  // 15
    if (f.c) {          // 16 orrhs, 17 lslhs
        r2 |= 0x00FF0000u;
        r1 <<= 8;
    }
    rsbs_lsr(12);  // 18
    tk(13, 19);
    if (!f.c)  // 19 blo
        goto i35;
    f = Sub(0, r1);  // 20 rsbs r3, r1, #0
    r3 = 0 - r1;
    tk(20, 21);
    if (f.c)  // 21 bhs: the divisor was 0
        goto i135;
i22:
    if (f.c)  // 22 lsrhs r1, r1, #8
        r1 >>= 8;
    t += T[22];
i23:
    step(15, 23);
    step(14, 26);
    step(13, 29);
    step(12, 32);
i35:
    step(11, 35);
    step(10, 38);
    step(9, 41);
    step(8, 44);
    t += T[47];
    if (f.c)  // 47 bhs
        goto i22;
i48:
    step(7, 48);
    step(6, 51);
    step(5, 54);
i57:
    step(4, 57);
i60:
    step(3, 60);
    step(2, 63);
i66:
    step(1, 66);
    f = Sub(r0, r1);  // 69 subs r1, r0, r1
    r1 = r0 - r1;
    if (!f.c)  // 70 movlo r1, r0
        r1 = r0;
    r0 = r2 + r2 + (f.c ? 1 : 0);  // 71 adc
    tk(69, 72);                    // 72 bx lr
    finish();
    return false;
i73:
    f = Sub(r0, r1);  // 73 subs r1, r0, r1
    r1 = r0 - r1;
    tk(73, 74);
    if (!f.c)  // 74 blo
        goto i77;
    r0 = 1;  // 75 movs r0, #1
    f.n = false;
    f.z = false;
    tk(75, 76);  // 76 bx lr
    finish();
    return false;
i77:
    r1 = r0;  // 77
    r0 = 0;   // 78 movs r0, #0
    f.n = false;
    f.z = true;
    tk(77, 79);  // 79 bx lr
    finish();
    return false;
i80:
    r2 = r1 & 0x80000000u;  // 80 ands r2, r1, #0x80000000
    f.n = (r2 >> 31) != 0;
    f.z = r2 == 0;
    f.c = true;
    if (f.n)  // 81 rsbmi r1, r1, #0
        r1 = 0 - r1;
    {
        const u32 sign = static_cast<u32>(static_cast<std::int32_t>(r0) >> 31);  // 82 eors r12, r2, r0, asr #32
        r12 = r2 ^ sign;
        f.n = (r12 >> 31) != 0;
        f.z = r12 == 0;
        f.c = (r0 >> 31) != 0;
    }
    if (f.c)  // 83 rsbhs r0, r0, #0
        r0 = 0 - r0;
    tk(80, 83);
    rsbs_lsr(4);  // 84
    tk(84, 85);
    if (!f.c)
        goto i116;
    rsbs_lsr(8);  // 86
    tk(86, 87);
    if (!f.c)
        goto i104;
    r1 <<= 6;     // 88
    rsbs_lsr(8);  // 89
    r2 |= 0xFC000000u;  // 90
    tk(88, 91);
    if (!f.c)
        goto i104;
    r1 <<= 6;     // 92
    rsbs_lsr(8);  // 93
    r2 |= 0x03F00000u;  // 94
    tk(92, 95);
    if (!f.c)
        goto i104;
    r1 <<= 6;     // 96
    rsbs_lsr(8);  // 97
    r2 |= 0x000FC000u;  // 98
    if (f.c) {          // 99 lslhs, 100 orrhs
        r1 <<= 6;
        r2 |= 0x3F00u;
    }
    f = Sub(0, r1);  // 101 rsbs r3, r1, #0
    r3 = 0 - r1;
    tk(96, 102);
    if (f.c)  // 102 bhs: the divisor was 0
        goto i133;
i103:
    if (f.c)  // 103 lsrhs r1, r1, #6
        r1 >>= 6;
    t += T[103];
i104:
    step(7, 104);
    step(6, 107);
    step(5, 110);
    step(4, 113);
i116:
    step(3, 116);
    step(2, 119);
    t += T[122];
    if (f.c)  // 122 bhs
        goto i103;
    step(1, 123);
    f = Sub(r0, r1);  // 126 subs r1, r0, r1
    r1 = r0 - r1;
    if (!f.c)  // 127 movlo r1, r0
        r1 = r0;
    r0 = r2 + r2 + (f.c ? 1 : 0);  // 128 adc
    f.c = ((r12 >> 30) & 1) != 0;  // 129 asrs r12, r12, #31
    r12 = static_cast<u32>(static_cast<std::int32_t>(r12) >> 31);
    f.n = (r12 >> 31) != 0;
    f.z = r12 == 0;
    if (f.n)  // 130 rsbmi r0, r0, #0
        r0 = 0 - r0;
    if (f.c)  // 131 rsbhs r1, r1, #0
        r1 = 0 - r1;
    tk(126, 132);  // 132 bx lr
    finish();
    return false;
i133:
    f.c = ((r12 >> 30) & 1) != 0;  // 133 asrs r12, r12, #31
    r12 = static_cast<u32>(static_cast<std::int32_t>(r12) >> 31);
    f.n = (r12 >> 31) != 0;
    f.z = r12 == 0;
    if (f.n)  // 134 rsbmi r0, r0, #0
        r0 = 0 - r0;
    tk(133, 134);
i135: {
    const u32 sp = r[13] - 8;  // 135 push {r0, lr}
    m.Write32(sp, r0);
    m.Write32(sp + 4, r[14]);
    r[13] = sp;
    r0 = 0;  // 136 movs r0, #0
    f.n = false;
    f.z = true;
    r[14] = s.literal[0] + 4 * 138;  // 137 bl __aeabi_idiv0 (lr: the pop after it)
    tk(135, 137);
    finish();
    return true;
}
}

// Castlevania's list search loop from its head, at most `limit` turns: 0 it left at the list's end (r4 0, words 10),
// 1 it found its node (the flag in r11, or a node whose field +12 is 0: word 12), 2 it stopped at its head.
template <class Mem>
int ListWalk(Cpu& c, Mem& m, const Site& s, u32 limit) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    Flags f = c.f;
    u32 r0 = r[0], r4 = r[4], r6 = r[6], r7 = r[7];
    const u32 r11 = r[11];
    int way = 2;
    for (u32 n = 0; n < limit; n++) {
        r6 = m.Read32(r4 + 4);  // 0
        r7 = m.Read32(r4);      // 1
        f = Sub(r11, 0);        // 2
        t += T[0] + T[1] + T[2] + T[3];
        if (!f.z) {  // 3 bne
            way = 1;
            break;
        }
        r0 = m.Read32(r4 + 12);  // 4
        f = Sub(r0, 0);          // 5
        if (!f.z)                // 6 movne r4, r6
            r4 = r6;
        t += T[4] + T[5] + T[6] + T[7];
        if (f.z) {  // 7 beq
            way = 1;
            break;
        }
        f = Sub(r4, 0);  // 8
        t += T[8] + T[9];
        if (f.z) {  // 9 bne not taken
            way = 0;
            break;
        }
    }
    r[0] = r0;
    r[4] = r4;
    r[6] = r6;
    r[7] = r7;
    c.f = f;
    c.ticks = t;
    return way;
}

// Castlevania's heap best-fit search (a leaf): r0 the heap, r1 the alignment (its absolute value); over the free
// list from [r0 + 60], each block's room after aligning; the biggest room left in r0 (ties: the smallest remainder,
// r12), as the code keeps them.
template <class Mem>
void HeapBestFit(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    Flags f = c.f;
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], r5 = r[5], r6 = r[6], r12 = r[12];
    auto tk = [&](int a, int b) {
        for (int i = a; i <= b; i++)
            t += T[i];
    };
    r0 += 24;       // 0
    r2 = r0 + 36;   // 2 (1: mov r0, r0)
    const u32 sp = r[13] - 12;  // 3 push {r4, r5, r6}
    WriteWords(m, sp, r + 4, 3);
    f = Sub(r1, 0);  // 4
    r2 = m.Read32(r2);  // 5
    if (f.n != f.v)     // 6 rsblt r1, r1, #0
        r1 = 0 - r1;
    r0 = 0;          // 7
    f = Sub(r2, 0);  // 8
    r12 = 0xFFFFFFFFu;  // 9 mvn r12, #0
    if (!f.z)           // 10 subne r5, r1, #1
        r5 = r1 - 1;
    tk(0, 11);
    if (!f.z) {  // 11 beq to the pop (35)
        for (;;) {
            r4 = m.Read32(r2 + 4);  // 12
            r3 = r2 + r1;           // 13
            r6 = r2 + 16;           // 14
            r3 = r3 + 15;           // 15
            r3 = r3 & ~r5;          // 16
            r6 = r6 + r4;           // 17
            f = Sub(r6, r3);        // 18
            tk(12, 19);
            if (f.c && !f.z) {  // 19 bls not taken
                r6 = r4 + r2;        // 20
                r4 = r3 - r2;        // 21
                r3 = r6 - r3;        // 22
                r3 = r3 + 16;        // 23
                f = Sub(r0, r3);     // 24
                r4 = r4 - 16;        // 25
                tk(20, 26);
                bool take;
                if (!f.c) {  // 26 blo to 30
                    take = true;
                } else {
                    // 27 bne to 32: the else of 26's If, so a block that starts with it (one tick when it fails)
                    t += !f.z ? T[27] : 1;
                    if (!f.z) {
                        take = false;
                    } else {
                        f = Sub(r12, r4);  // 28
                        tk(28, 29);
                        take = f.c && !f.z;  // 29 bls not taken: on to 30
                    }
                }
                if (take) {
                    r0 = r3;   // 30
                    r12 = r4;  // 31
                    tk(30, 31);
                }
            }
            r2 = m.Read32(r2 + 12);  // 32
            f = Sub(r2, 0);          // 33
            tk(32, 34);
            if (f.z)  // 34 bne not taken
                break;
        }
    }
    ReadWords(m, sp, r + 4, 3);  // 35 pop {r4, r5, r6}
    tk(35, 36);
    (void)r4;
    (void)r5;
    (void)r6;
    r[0] = r0;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[12] = r12;
    c.f = f;
    c.ticks = t;
}

// RVCT's strlen (24 words): a byte at a time until aligned, then a word at a time (the 0x01010101 literal).
template <class Mem>
void Strlen(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    Flags f = c.f;
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r12 = r[12];
    const u32 r3 = r0 + 1;  // 0, then 1 b to 6
    t += T[0] + T[1];
    auto done = [&]() {
        r[0] = r0;
        r[1] = r1;
        r[2] = r2;
        r[3] = r3;
        r[12] = r12;
        c.f = f;
        c.ticks = t;
    };
    for (;;) {
        f.n = false;  // 6 tst r0, #3
        f.z = (r0 & 3) == 0;
        t += T[6];
        if (f.z) {  // 7 ldreq (passes), 8 bne (in its block's If: not taken)
            r2 = s.literal[0];
            t += T[7] + T[8];
            break;
        }
        t += 1 + T[8];  // (7 fails: a run of one; 8 bne its own block, taken)
        r1 = m.Read8(r0);  // 2 ldrb r1, [r0], #1
        r0 += 1;
        f = Sub(r1, 0);  // 3
        if (f.z)         // 4 subeq (a select)
            r0 = r0 - r3;
        t += T[2] + T[3] + T[4];
        if (f.z) {  // 5 moveq pc, lr
            t += T[5];
            done();
            return;
        }
        t += 1;
    }
    const bool carry = ((r2 >> 25) & 1) != 0;
    do {  // 9-13
        r1 = m.Read32(r0);
        r0 += 4;
        r12 = r1 - r2;
        r1 = r12 & ~r1;
        r1 &= r2 << 7;
        f.n = (r1 >> 31) != 0;
        f.z = r1 == 0;
        f.c = carry;
        t += T[9] + T[10] + T[11] + T[12] + T[13];
    } while (f.z);
    f.c = ((r1 >> 8) & 1) != 0;  // 14 lsls r2, r1, #24
    r2 = r1 << 24;
    f.n = (r2 >> 31) != 0;
    f.z = r2 == 0;
    r0 = r0 - r3;  // 15
    if (!f.z)      // 16 subne
        r0 -= 3;
    t += T[14] + T[15] + T[16];
    if (!f.z) {  // 17 movne pc, lr
        t += T[17];
        done();
        return;
    }
    t += 1;
    f.c = ((r1 >> 16) & 1) != 0;  // 18 lsls r2, r1, #16
    r2 = r1 << 16;
    f.n = (r2 >> 31) != 0;
    f.z = r2 == 0;
    if (!f.z)  // 19 subne
        r0 -= 2;
    t += T[18] + T[19];
    if (!f.z) {  // 20 movne pc, lr
        t += T[20];
        done();
        return;
    }
    t += 1;
    f.c = ((r1 >> 24) & 1) != 0;  // 21 lsls r1, r1, #8
    r1 = r1 << 8;
    f.n = (r1 >> 31) != 0;
    f.z = r1 == 0;
    if (!f.z)  // 22 subne
        r0 -= 1;
    t += T[21] + T[22] + T[23];  // 23 mov pc, lr
    done();
}

// RVCT's memcpy for word-aligned pointers (29 words): 32 bytes a turn by LDM/STM of eight registers (r4-r10 and lr
// pushed), then 16 and 8 bytes, then a word, a halfword and a byte by the size's low bits. Its conditional loads and
// stores are dynarmic blocks of their own: one tick each for a failed run.
template <class Mem>
void Memcpy4(Cpu& c, Mem& m, const Site& s) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    u64 t = 0;
    Flags f = c.f;
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], r12 = r[12], lr = r[14];
    u32 w[8];
    const u32 sp = r[13] - 32;
    {
        u32 saved[8];  // 0 push {r4-r10, lr}
        std::memcpy(saved, r + 4, 28);
        saved[7] = r[14];
        WriteWords(m, sp, saved, 8);
    }
    f = Sub(r2, 32);  // 1 subs r2, r2, #32
    r2 -= 32;
    t += T[0] + T[1] + T[2];
    if (f.c) {  // 2 blo not taken
        ReadWords(m, r1, w, 4);  // 3 ldm r1!, {r3-r6}
        r1 += 16;
        ReadWords(m, r1, w + 4, 4);  // 5 ldm r1!, {r7-r10} (4 pld: a hint)
        r1 += 16;
        t += T[3] + T[4] + T[5];
        for (;;) {
            WriteWords(m, r0, w, 4);  // 6 stm r0!, {r3-r6}
            r0 += 16;
            f = Sub(r2, 32);  // 7 subs
            r2 -= 32;
            WriteWords(m, r0, w + 4, 4);  // 8 stm r0!, {r7-r10}
            r0 += 16;
            t += T[6] + T[7] + T[8];
            if (!f.c) {
                t += 1 + T[10] + 2;  // 9 fails (1), 10 pld its own block, 11-12 fail (2)
                break;
            }
            ReadWords(m, r1, w, 4);  // 9 ldmhs
            r1 += 16;
            ReadWords(m, r1, w + 4, 4);  // 11 ldmhs
            r1 += 16;
            t += T[9] + T[10] + T[11] + T[12];  // 12 bhs to 6
        }
        r3 = w[0];
        r4 = w[1];
    }
    // 13 lsls r12, r2, #28
    f.c = ((r2 >> 4) & 1) != 0;
    r12 = r2 << 28;
    f.n = (r12 >> 31) != 0;
    f.z = r12 == 0;
    t += T[13];
    if (f.c) {  // 14 ldmhs r1!, {r3, r4, r12, lr}; 15 stmhs r0!, {...}
        r3 = m.Read32(r1);
        r4 = m.Read32(r1 + 4);
        r12 = m.Read32(r1 + 8);
        lr = m.Read32(r1 + 12);
        r1 += 16;
        m.Write32(r0, r3);
        m.Write32(r0 + 4, r4);
        m.Write32(r0 + 8, r12);
        m.Write32(r0 + 12, lr);
        r0 += 16;
        t += T[14] + T[15];
    } else {
        t += 2;
    }
    if (f.n) {  // 16 ldmmi r1!, {r3, r4}; 17 stmmi r0!, {r3, r4}
        r3 = m.Read32(r1);
        r4 = m.Read32(r1 + 4);
        r1 += 8;
        m.Write32(r0, r3);
        m.Write32(r0 + 4, r4);
        r0 += 8;
        t += T[16] + T[17];
    } else {
        t += 2;
    }
    {
        u32 saved[8];  // 18 pop {r4-r10, lr}
        ReadWords(m, sp, saved, 8);
        std::memcpy(r + 4, saved, 28);
        lr = saved[7];
    }
    f.c = ((r2 >> 2) & 1) != 0;  // 19 lsls r12, r2, #30
    r12 = r2 << 30;
    f.n = (r12 >> 31) != 0;
    f.z = r12 == 0;
    t += T[18] + T[19];
    if (f.c) {  // 20 ldrhs r3, [r1], #4; 21 strhs r3, [r0], #4
        r3 = m.Read32(r1);
        r1 += 4;
        m.Write32(r0, r3);
        r0 += 4;
        t += T[20] + T[21];
    } else {
        t += 2;
    }
    auto done = [&]() {
        r[0] = r0;
        r[1] = r1;
        r[2] = r2;
        r[3] = r3;
        r[12] = r12;
        r[14] = lr;
        c.f = f;
        c.ticks = t;
    };
    if (f.z) {  // 22 bxeq lr
        t += T[22];
        done();
        return;
    }
    t += 1;
    f.c = ((r2 >> 1) & 1) != 0;  // 23 lsls r2, r2, #31
    r2 = r2 << 31;
    f.n = (r2 >> 31) != 0;
    f.z = r2 == 0;
    t += T[23];
    const bool hs = f.c, mi = f.n;
    if (hs) {  // 24 ldrhhs r3, [r1], #2
        r3 = m.Read16(r1);
        r1 += 2;
        t += T[24];
    } else {
        t += 1;
    }
    if (mi) {  // 25 ldrbmi r2, [r1], #1
        r2 = m.Read8(r1);
        r1 += 1;
        t += T[25];
    } else {
        t += 1;
    }
    if (hs) {  // 26 strhhs r3, [r0], #2
        m.Write16(r0, r3);
        r0 += 2;
        t += T[26];
    } else {
        t += 1;
    }
    if (mi) {  // 27 strbmi r2, [r0], #1, then 28 bx lr in its block
        m.Write8(r0, r2);
        r0 += 1;
        t += T[27] + T[28];
    } else {
        t += 1 + T[28];
    }
    done();
}

// CRC-32 (kWholeOrFirst): all its ticks are known before it runs (the table's, then its bytes': an odd one, then
// pairs). Whole up to its pop (true; its bx lr left to the JIT) when they fit in `left`, the JIT's slice; else (false)
// only its first block as the JIT's (the push, the frame, the polynomial, entry 0's first turn, to the bne), on at
// the inner loop's head (word 8). The table: the host's copy written to the stack, its loads from that copy.
template <class Mem>
bool Crc32(Cpu& c, Mem& m, const Site& s, u64 left) {
    u32* const r = c.r;
    const std::uint16_t* const T = s.ticks;
    auto sum = [T](int a, int b) {
        u64 x = 0;
        for (int i = a; i <= b; i++)
            x += T[i];
        return x;
    };
    const u32 poly = s.literal[0];
    const u32 sp0 = r[13];
    const u32 sp = sp0 - 20 - 1040;
    const u32 n = r[1];
    u64 total = sum(0, 5) + 256 * (T[6] + T[7] + 4 * sum(8, 15) + sum(16, 19)) + sum(20, 23) + sum(46, 49);
    if (n != 0)
        total += sum(24, 26) + ((n & 1) ? sum(27, 31) : 0) + T[32] + T[33] + u64(n >> 1) * sum(34, 45);
    {
        const u32 saved[5] = {r[4], r[5], r[6], r[7], r[8]};
        WriteWords(m, sp0 - 20, saved, 5);  // 0 push {r4-r8}
    }
    if (total >= left) {
        r[13] = sp;     // 1 sub sp, sp, #1040
        r[8] = sp;      // 2
        r[5] = poly;    // 3 ldr r5, =poly
        r[6] = 0;       // 4
        r[7] = sp + 4;  // 5
        r[3] = 0;       // 6 mov r3, r6; 8-13: 0's two steps (tst, lsreq r12; tst, lsreq r3) leave 0
        r[12] = 0;
        r[4] = 3;       // 7 mov r4, #4; 14 subs r4 (NZCV 0010); 15 bne, taken
        c.f = {false, false, true, false};
        c.ticks = sum(0, 15);
        return false;
    }
    const u32* const table = s.table;
    WriteRange(m, sp + 4, table, 256);  // 6-19: str r3, [r7, r6, lsl #2] for 0-255
    u32 r12 = 255;  // (entry 255's seventh step: r12 as the table's loop leaves it)
    for (int step = 0; step < 7; step++)
        r12 = (r12 & 1) ? (poly ^ (r12 >> 1)) : (r12 >> 1);
    u32 r2 = r[2];
    u32 r0 = r[0];
    m.Write32(sp, r2);  // 20 cmp r1, #0; 21 mov r3, r7; 22 str r2, [sp]; 23 beq
    if (n != 0) {
        r0 -= 1;  // 24 tst r1, #1; 25 sub r0, r0, #1; 26 beq
        if (n & 1) {
            r0 += 1;  // 27 ldrb r12, [r0, #1]!; 28-30 the table's word; 31 eor r2, r12, r2, lsr #8
            r12 = table[(m.Read8(r0) ^ r2) & 255];
            r2 = r12 ^ (r2 >> 8);
        }
        for (u32 k = n >> 1; k != 0; k--) {  // 32 lsrs r1, r1, #1; 33 beq; 34-45 two bytes a turn
            const u32 x = table[(m.Read8(r0 + 1) ^ r2) & 255];  // 34 ldrb r12, [r0, #1]; 36, 37, 39
            const u32 b = m.Read8(r0 + 2);                      // 38 ldrb r12, [r0, #2]!
            r0 += 2;
            r2 = x ^ (r2 >> 8);                                  // 40
            r12 = table[(b ^ r2) & 255];                         // 41-43
            r2 = r12 ^ (r2 >> 8);                                // 44
        }
    }
    m.Write32(sp, r2);  // 46 str r2, [sp]; 47 add sp, sp, #1040; 48 mvn r0, r2
    ReadWords(m, sp0 - 20, r + 4, 5);  // 49 pop {r4-r8}
    r[13] = sp0;
    r[0] = ~r2;
    r[1] = 0;
    r[2] = r2;
    r[3] = sp + 4;
    r[12] = r12;
    c.f = {false, true, true, false};  // (cmp r1, #0 with none; else tst, lsrs and subs to 0: 0110 either way)
    c.ticks = total;
    return true;
}

// Yaz0 (kStopsLikeJit): `pro` its prologue's words (8 rev, 11 swap, 0 from its loop's head). Its blocks as the JIT
// makes them (a1: each conditional branch ends one): the prologue with the head's lsrs/bne; the head (0-1); 2-5 or
// 4-5 (the group's byte, the flag's tst; bne); a literal 24-27 (bne to the head); a reference 6-13 (bne), 14-20 or
// 16-20 (its first byte; bne), 17-20 a byte, 21-22 (cmp r4, #0; bne to the head), 23 (b to the pop). After each
// block the JIT goes on only while its ticks are under `left`: so does this, else it stops there (false, r15 the
// next block's word). True: at the pop (word 28 of the loop; its block left to the JIT).
template <class Mem>
bool Yaz0(Cpu& c, Mem& m, const Site& s, u64 left, u32 pro) {
    u32* const r = c.r;
    const std::uint16_t* const L = s.ticks + pro;  // (the loop's words' ticks, from its head)
    auto sum = [L](int a, int b) {
        u64 x = 0;
        for (int i = a; i <= b; i++)
            x += L[i];
        return x;
    };
    const u64 bHead = sum(0, 1), bByte = sum(2, 5), bFlag = sum(4, 5), bLit = sum(24, 27), bRef = sum(6, 13),
              bLong = sum(14, 15), bSub = L[16], bCopy = sum(17, 20), bEnd = sum(21, 22), bOut = L[23];
    const u32 head = s.pc + 4 * pro;
    u32 r0 = r[0], r1 = r[1], r3 = r[3], r4 = r[4], r5 = r[5], r6 = r[6], r7 = r[7], r12 = r[12], lr = r[14];
    Flags f = c.f;
    u64 t = 0;
    u32 stop = 0;
    if (pro != 0) {
        const u32 saved[6] = {r[4], r[5], r[6], r[7], r[8], r[14]};
        WriteWords(m, r[13] - 24, saved, 6);  // push {r4-r8, lr}
        r[13] -= 24;
        const u32 w = m.Read32(r1 + 4);  // ldr r5 (or r4), [r1, #4]
        r4 = (w >> 24) | ((w >> 8) & 0xFF00) | ((w << 8) & 0xFF0000) | (w << 24);
        r[2] = r4;     // mov r2, r4
        r1 += 16;
        r5 = 0;
        lr = 0xFFF;    // mov lr, #4096; sub lr, lr, #1
        for (u32 i = 0; i < pro; i++)
            t += s.ticks[i];
    }
    for (;;) {
        // 0 lsrs r5, r5, #1; 1 bne 4 (the head's block, or the end of the prologue's)
        f.c = (r5 & 1) != 0;
        r5 >>= 1;
        f.n = false;
        f.z = r5 == 0;
        t += bHead;
        if (t >= left) {
            stop = head + (f.z ? 8 : 16);
            break;
        }
        if (f.z) {
            r6 = m.Read8(r1);  // 2 ldrb r6, [r1], #1
            r1 += 1;
            r5 = 128;          // 3
            t += bByte;
        } else {
            t += bFlag;
        }
        // 4 tst r6, r5 (C and V as they were); 5 bne 24
        f.n = ((r6 & r5) >> 31) != 0;
        f.z = (r6 & r5) == 0;
        if (!f.z) {
            if (t >= left) {
                stop = head + 4 * 24;
                break;
            }
            r12 = m.Read8(r1);  // 24 ldrb r12, [r1], #1
            r1 += 1;
            f = Sub(r4, 1);     // 25 subs r4, r4, #1
            r4 -= 1;
            m.Write8(r0, r12);  // 26 strb r12, [r0], #1
            r0 += 1;
            t += bLit;          // 27 bne 0
            if (f.z)
                break;          // (done: the pop's block)
            if (t >= left) {
                stop = head;
                break;
            }
            continue;
        }
        if (t >= left) {
            stop = head + 4 * 6;
            break;
        }
        r3 = m.Read8(r1);        // 6 ldrb r3, [r1], #1
        r7 = m.Read8(r1 + 1);    // 7 ldrb r7, [r1], #1
        r1 += 2;
        r3 = r7 + (r3 << 8);     // 8
        f.c = ((r3 >> 11) & 1) != 0;  // 9 lsrs r7, r3, #12
        r7 = r3 >> 12;
        f.n = false;
        f.z = r7 == 0;
        r3 = (r3 & lr) + 1;      // 10, 11
        r7 += 2;                 // 12
        t += bRef;               // 13 bne 16
        if (t >= left) {
            stop = head + (f.z ? 4 * 14 : 4 * 16);
            break;
        }
        if (f.z) {
            r7 = m.Read8(r1);    // 14 ldrb r7, [r1], #1
            r1 += 1;
            r7 += 18;            // 15
            t += bLong;
        }
        r4 -= r7;                // 16 sub r4, r4, r7
        t += bSub;
        bool stopped = false;
        for (;;) {
            // the bytes before the slice's end (each one a block of 17-20), at most r7: at once in plain memory
            const u64 room = t < left ? (left - t + bCopy - 1) / bCopy : 1;
            const u32 k = room < r7 ? static_cast<u32>(room) : r7;
            const std::uint8_t* from = k > 1 ? m.Bytes(r0 - r3, k) : nullptr;
            std::uint8_t* to = from != nullptr ? m.Bytes(r0, k) : nullptr;
            if (to != nullptr) {
                for (u32 i = 0; i < k; i++)  // (forward a byte at a time: a near reference repeats its bytes)
                    to[i] = from[i];
                r12 = to[k - 1];
                f = Sub(r7 - k + 1, 1);
                r7 -= k;
                r0 += k;
                t += u64(k) * bCopy;
            } else {
                r12 = m.Read8(r0 - r3);  // 17 ldrb r12, [r0, -r3]
                f = Sub(r7, 1);          // 18 subs r7, r7, #1
                r7 -= 1;
                m.Write8(r0, r12);       // 19 strb r12, [r0], #1
                r0 += 1;
                t += bCopy;              // 20 bne 17
            }
            if (t >= left) {
                stop = head + (f.z ? 4 * 21 : 4 * 17);
                stopped = true;
                break;
            }
            if (f.z)
                break;
        }
        if (stopped)
            break;
        f = Sub(r4, 0);  // 21 cmp r4, #0; 22 bne 0
        t += bEnd;
        if (t >= left) {
            stop = f.z ? head + 4 * 23 : head;
            break;
        }
        if (!f.z)
            continue;
        t += bOut;       // 23 b 28 (the pop)
        break;
    }
    r[0] = r0;
    r[1] = r1;
    r[3] = r3;
    r[4] = r4;
    r[5] = r5;
    r[6] = r6;
    r[7] = r7;
    r[12] = r12;
    r[14] = lr;
    c.f = f;
    c.ticks = t;
    if (stop != 0) {
        r[15] = stop;
        return false;
    }
    return true;
}

// LZ10/LZ11 (kHostPc): its blocks as dynarmic makes them (a1/a2; checked one block at a time, blocks56.cpp); after
// each, the JIT goes on only while its ticks are under `left`, but 89 -> 91 -> 92 (cmpls, a condition's run, then
// bls) are linked without a check. `from`: 0 (the entry) or 43 (an item's head; its Site's words start at 38). Leaves
// r15: where it stopped, or the return's block (19, 62, 93: errors; 133: the end) for the JIT.
template <class Mem>
void Lz(Cpu& c, Mem& m, const Site& s, u64 left, u32 from) {
    u32* const r = c.r;
    const u32 base = from == 0 ? 0 : 38;  // (the Site's first word)
    const u32 at0 = s.pc - 4 * from;      // (word 0's address)
    auto T = [&](int i) -> u64 { return s.ticks[i - static_cast<int>(base)]; };
    auto S = [&](int a, int b) {
        u64 x = 0;
        for (int i = a; i <= b; i++)
            x += T(i);
        return x;
    };
    const u64 b38 = S(38, 42), b43 = S(43, 44), b45 = S(45, 49), b50 = S(50, 51), b52 = S(52, 54), b55 = S(55, 61),
              b65 = S(65, 70), b71 = T(71), b72 = S(72, 81), b82 = S(82, 88), b89 = S(89, 90), b91 = T(91),
              b92 = T(92), b96 = S(96, 101), b102 = S(102, 106), b107 = T(107), b108 = S(108, 109),
              b110 = S(110, 114), b115 = S(115, 118), b117 = S(117, 118), b119 = S(119, 124), b125 = S(125, 127),
              b126 = S(126, 127), b128 = S(128, 131), b132 = T(132);
    u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], r5 = r[5], r6 = r[6], r7 = r[7], r8 = r[8], r9 = r[9],
        r10 = r[10], r11 = r[11], r12 = r[12];
    Flags f = c.f;
    u64 t = 0;
    // whole items from an item's head: a literal (43, 65, 71, 126, 128) or a reference (43, 45, its form, 55, 89-92,
    // 96, 108, 110, 115 or 117, 119 for each pair, 125, 128), and a group's change after the 8th (132, 38)
    const u64 lit = b43 + b65 + b71 + b126 + b128, grp = b132 + b38;
    const u64 ref = b43 + b45 + b55 + b89 + b91 + b92 + b96 + b108 + b110 + b125 + b128;
    int at = static_cast<int>(from);
    int stop;
    for (;;) {
        if (at == 43) {
            // Items whose blocks all end under `left` (the JIT wouldn't stop inside them), straight through: the
            // literals, and the references inside plain memory that neither end the output nor fail; the rest by
            // blocks below.
            for (;;) {
                const bool group = r10 + 1 >= 8;  // (after it: 128 -> 132 -> 38, the next group's byte)
                if ((r9 & 128) == 0) {
                    if (t + lit + (group ? grp : 0) >= left ||
                        static_cast<std::int32_t>(r6) < (group ? 2 : 1) || r7 < 2)
                        break;
                    r0 = m.Read8(r5);  // 65 ldrb r0, [r5], #1; 66; 68 subge r7; 69 strb r0, [r3], #1
                    r5 += 1;
                    r6 -= 1;
                    r7 -= 1;
                    m.Write8(r3, r0);
                    r3 += 1;
                    t += lit;
                } else {
                    const u32 b0 = m.Read8(r5);
                    u32 len, bytes;
                    u64 form = 0;
                    if (r8 == 0) {  // LZ10
                        len = (b0 >> 4) + 3;
                        bytes = 2;
                    } else if ((b0 >> 4) == 1) {  // LZ11's four bytes (72-81)
                        const u32 b1 = m.Read8(r5 + 1), b2 = m.Read8(r5 + 2);
                        len = (((b0 & 15) << 12) | (b1 << 4) | (b2 >> 4)) + 0x111;
                        bytes = 4;
                        form = b50 + b72;
                    } else if ((b0 >> 4) == 0) {  // LZ11's three bytes (82-88)
                        const u32 b1 = m.Read8(r5 + 1);
                        len = (((b0 & 15) << 4) | (b1 >> 4)) + 0x11;
                        bytes = 3;
                        form = b50 + b52 + b82;
                    } else {  // LZ11's two bytes
                        len = (b0 >> 4) + 1;
                        bytes = 2;
                        form = b50 + b52;
                    }
                    if (static_cast<std::int32_t>(r6) < static_cast<std::int32_t>(bytes + (group ? 1 : 0)) ||
                        len >= r7)
                        break;
                    const u32 dist = (((m.Read8(r5 + bytes - 2) & 15) << 8) | m.Read8(r5 + bytes - 1)) + 1;
                    const u32 odd = (len - 1) & 1, pairs = (len - 1) >> 1;
                    const u64 tk = ref + form + (odd ? b115 : b117) + u64(pairs) * b119 + (group ? grp : 0);
                    if (t + tk >= left || r3 - dist < r2)
                        break;
                    const std::uint8_t* from_p = m.Bytes(r3 - dist, len);
                    std::uint8_t* to_p = from_p != nullptr ? m.Bytes(r3, len) : nullptr;
                    if (to_p == nullptr)
                        break;
                    for (u32 i = 0; i < len; i++)  // (forward a byte at a time: a near reference repeats its bytes)
                        to_p[i] = from_p[i];
                    if (pairs > 0)
                        r11 = to_p[len - 1];
                    r0 = len - 1;
                    r1 = r3 - dist + len - 1;
                    r12 = r3 + len - 1;
                    r4 = 0;
                    r3 += len;
                    r7 -= len;
                    r5 += bytes;
                    r6 -= bytes;
                    t += tk - (group ? grp : 0);
                }
                r10 += 1;  // 128 add r10; 129 cmp r10, #8; 130 lsl r9, r9, #1
                f = Sub(r10, 8);
                r9 <<= 1;
                if (group) {  // 132 b 38; 38 ldrb r9, [r5], #1; 39 sub r6; 40 cmp r6, #0; 41 movge r10, #0
                    r9 = m.Read8(r5);
                    r5 += 1;
                    r6 -= 1;
                    f = Sub(r6, 0);
                    r10 = 0;
                    t += grp;
                }
            }
        }
        int next;
        bool check = true;
        switch (at) {
        case 0: {
            const u32 saved[8] = {r4, r5, r6, r7, r8, r9, r10, r11};
            WriteWords(m, r[13] - 32, saved, 8);  // 0 push {r4-r11}
            r[13] -= 32;
            r3 = r2;                     // 1
            r4 = m.Read32(r0);           // 2 ldr r4, [r0]
            r5 = m.Read8(r0 + 3);        // 3
            r12 = m.Read8(r0);           // 4
            r4 = (r4 & 0x00FFFFFFu) | (r5 << 24);  // 5, 6
            f.n = false;                 // 7 tst r12, #15
            f.z = (r12 & 15) == 0;
            r7 = r4 >> 8;                // 8
            r8 = f.z ? 0 : 1;            // 9 movne, 10 moveq
            r4 = r12 & 0xF0;             // 11
            f = Sub(r4, 16);             // 12; 13 bne 19
            t += S(0, 13);
            next = f.z ? 14 : 19;
            break;
        }
        case 14:  // 14 tst r12, #15; 15 beq 22
            f.n = false;
            f.z = (r12 & 15) == 0;
            t += S(14, 15);
            next = f.z ? 22 : 16;
            break;
        case 16:  // 16 and r12, r12, #15; 17 cmp r12, #1; 18 beq 22
            r12 &= 15;
            f = Sub(r12, 1);
            t += S(16, 18);
            next = f.z ? 22 : 19;
            break;
        case 22:  // 22 cmp r1, #4; 23 mov r12, r1; 24 bls 62
            f = Sub(r1, 4);
            r12 = r1;
            t += S(22, 24);
            next = (!f.c || f.z) ? 62 : 25;
            break;
        case 25:  // 25 cmp r7, #0; 26 add r5, r0, #4; 27 sub r6, r12, #4; 28 bne 38
            f = Sub(r7, 0);
            r5 = r0 + 4;
            r6 = r12 - 4;
            t += S(25, 28);
            next = f.z ? 29 : 38;
            break;
        case 29:  // 29 cmp r6, #4; 30 blt 62
            f = Sub(r6, 4);
            t += S(29, 30);
            next = f.n != f.v ? 62 : 31;
            break;
        case 31:  // 31 ldr r0, [r5]; 32 ldrb r1, [r5, #3]; 33-35; 36 orrs r7, r0, r1, lsl #24; 37 beq 133
            r0 = m.Read32(r5);
            r1 = m.Read8(r5 + 3);
            r5 += 4;
            r0 &= 0x00FFFFFFu;
            r6 -= 4;
            r7 = r0 | (r1 << 24);
            f.n = (r7 >> 31) != 0;
            f.z = r7 == 0;
            f.c = ((r1 >> 8) & 1) != 0;
            t += S(31, 37);
            next = f.z ? 133 : 38;
            break;
        case 38:  // 38 ldrb r9, [r5], #1; 39 sub r6; 40 cmp r6, #0; 41 movge r10, #0; 42 blt 62
            r9 = m.Read8(r5);
            r5 += 1;
            r6 -= 1;
            f = Sub(r6, 0);
            if (f.n == f.v)
                r10 = 0;
            t += b38;
            next = f.n != f.v ? 62 : 43;
            break;
        case 43:  // 43 tst r9, #128; 44 beq 65
            f.n = false;
            f.z = (r9 & 128) == 0;
            t += b43;
            next = f.z ? 65 : 45;
            break;
        case 45:  // 45 ldrb r1, [r5]; 46 cmp r8, #0; 47 lsr r0, r1, #4; 48 addeq r0, r0, #3; 49 beq 55
            r1 = m.Read8(r5);
            f = Sub(r8, 0);
            r0 = r1 >> 4;
            if (f.z)
                r0 += 3;
            t += b45;
            next = f.z ? 55 : 50;
            break;
        case 50:  // 50 cmp r0, #1; 51 beq 72
            f = Sub(r0, 1);
            t += b50;
            next = f.z ? 72 : 52;
            break;
        case 52:  // 52 cmp r0, #0; 53 addne r0, r0, #1; 54 beq 82
            f = Sub(r0, 0);
            if (!f.z)
                r0 += 1;
            t += b52;
            next = f.z ? 82 : 55;
            break;
        case 55:  // 55 ldrb r1, [r5], #1; 56 subs r6, r6, #2; 57 ldrb r12, [r5], #1; 58-60; 61 bpl 89
            r1 = m.Read8(r5);
            r5 += 1;
            f = Sub(r6, 2);
            r6 -= 2;
            r12 = m.Read8(r5);
            r5 += 1;
            r1 = r12 | ((r1 << 28) >> 20);
            r1 += 1;
            t += b55;
            next = f.n ? 62 : 89;
            break;
        case 65:  // 65 ldrb r0, [r5], #1; 66 sub r6; 67 cmp r6, #0; 68 subge r7; 69 strb r0, [r3], #1; 70 blt 62
            r0 = m.Read8(r5);
            r5 += 1;
            r6 -= 1;
            f = Sub(r6, 0);
            if (f.n == f.v)
                r7 -= 1;
            m.Write8(r3, r0);
            r3 += 1;
            t += b65;
            next = f.n != f.v ? 62 : 71;
            break;
        case 71:  // 71 b 126
            t += b71;
            next = 126;
            break;
        case 72:  // 72 lsl r0, r1, #28; 73 ldrb r1, [r5, #1]; 74 ldrb r12, [r5, #2]!; 75-80; 81 b 55
            r0 = r1 << 28;
            r1 = m.Read8(r5 + 1);
            r12 = m.Read8(r5 + 2);
            r5 += 2;
            r0 >>= 16;
            r6 -= 2;
            r0 |= r1 << 4;
            r0 |= r12 >> 4;
            r0 += 256 + 17;
            t += b72;
            next = 55;
            break;
        case 82:  // 82 lsl r0, r1, #28; 83 ldrb r1, [r5, #1]!; 84-87; 88 b 55
            r0 = r1 << 28;
            r5 += 1;
            r1 = m.Read8(r5);
            r0 >>= 24;
            r6 -= 1;
            r0 |= r1 >> 4;
            r0 += 17;
            t += b82;
            next = 55;
            break;
        case 89:  // 89 cmp r0, r7; 90 subls r12, r3, r1 (then 91 linked without a check)
            f = Sub(r0, r7);
            if (!f.c || f.z)
                r12 = r3 - r1;
            t += b89;
            next = 91;
            check = false;
            break;
        case 91:  // 91 cmpls r2, r12: a block of its own (failed: one tick), linked on without a check
            if (!f.c || f.z) {
                f = Sub(r2, r12);
                t += b91;
            } else {
                t += 1;
            }
            next = 92;
            check = false;
            break;
        case 92:  // 92 bls 96
            t += b92;
            next = (!f.c || f.z) ? 96 : 93;
            break;
        case 96:  // 96 ldrb r12, [r12]; 97 sub r7, r7, r0; 98 sub r0, #1; 99 cmn r0, #0x80000001; 100 strb; 101 bls
            r12 = m.Read8(r12);
            r7 -= r0;
            r0 -= 1;
            f = Add(r0, 0x80000001u);
            m.Write8(r3, r12);
            r3 += 1;
            t += b96;
            next = (!f.c || f.z) ? 108 : 102;
            break;
        case 102:  // 102 sub r12, r3, r1; 103 subs r0; 104 ldrb r12, [r12]; 105 strb r12, [r3], #1; 106 bne 102
            r12 = r3 - r1;
            f = Sub(r0, 1);
            r0 -= 1;
            r12 = m.Read8(r12);
            m.Write8(r3, r12);
            r3 += 1;
            t += b102;
            next = f.z ? 107 : 102;
            break;
        case 107:  // 107 b 126
            t += b107;
            next = 126;
            break;
        case 108:  // 108 cmp r0, #0; 109 ble 126
            f = Sub(r0, 0);
            t += b108;
            next = (f.z || f.n != f.v) ? 126 : 110;
            break;
        case 110:  // 110 sub r1, r3, r1; 111 tst r0, #1; 112 sub r12, r3, #1; 113 sub r1, r1, #1; 114 beq 117
            r1 = r3 - r1;
            f.n = false;
            f.z = (r0 & 1) == 0;
            r12 = r3 - 1;
            r1 -= 1;
            t += b110;
            next = f.z ? 117 : 115;
            break;
        case 115:  // 115 ldrb r4, [r1, #1]!; 116 strb r4, [r12, #1]!; 117 asrs r4, r0, #1; 118 beq 125
        case 117:
            if (at == 115) {
                r1 += 1;
                r4 = m.Read8(r1);
                r12 += 1;
                m.Write8(r12, r4);
            }
            f.c = (r0 & 1) != 0;
            r4 = static_cast<u32>(static_cast<std::int32_t>(r0) >> 1);
            f.n = (r4 >> 31) != 0;
            f.z = r4 == 0;
            t += at == 115 ? b115 : b117;
            next = f.z ? 125 : 119;
            break;
        case 119: {  // 119 ldrb r11, [r1, #1]; 120 subs r4; 121 strb r11, [r12, #1]; 122 ldrb r11, [r1, #2]!;
                     // 123 strb r11, [r12, #2]!; 124 bne 119: each turn a block; many at once in plain memory
            const u64 room = t < left ? (left - t + b119 - 1) / b119 : 1;
            const u32 k = room < r4 ? static_cast<u32>(room) : r4;
            const std::uint8_t* from_p = k > 1 ? m.Bytes(r1 + 1, 2 * k) : nullptr;
            std::uint8_t* to_p = from_p != nullptr ? m.Bytes(r12 + 1, 2 * k) : nullptr;
            if (k > 1 && to_p != nullptr) {
                for (u32 i = 0; i < 2 * k; i++)  // (forward a byte at a time: a near reference repeats its bytes)
                    to_p[i] = from_p[i];
                r11 = to_p[2 * k - 1];
                f = Sub(r4 - k + 1, 1);
                r4 -= k;
                r1 += 2 * k;
                r12 += 2 * k;
                t += u64(k) * b119;
            } else {
                r11 = m.Read8(r1 + 1);
                f = Sub(r4, 1);
                r4 -= 1;
                m.Write8(r12 + 1, r11);
                r1 += 2;
                r11 = m.Read8(r1);
                r12 += 2;
                m.Write8(r12, r11);
                t += b119;
            }
            next = f.z ? 125 : 119;
            break;
        }
        case 125:  // 125 add r3, r3, r0; 126 cmp r7, #0; 127 beq 133
        case 126:
            if (at == 125)
                r3 += r0;
            f = Sub(r7, 0);
            t += at == 125 ? b125 : b126;
            next = f.z ? 133 : 128;
            break;
        case 128:  // 128 add r10, #1; 129 cmp r10, #8; 130 lsl r9, r9, #1; 131 blo 43
            r10 += 1;
            f = Sub(r10, 8);
            r9 <<= 1;
            t += b128;
            next = f.c ? 132 : 43;
            break;
        case 132:  // 132 b 38
            t += b132;
            next = 38;
            break;
        default:  // (not reached)
            next = 133;
            break;
        }
        if (next == 19 || next == 62 || next == 93 || next == 133 || (check && t >= left)) {
            stop = next;
            break;
        }
        at = next;
    }
    r[0] = r0;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[4] = r4;
    r[5] = r5;
    r[6] = r6;
    r[7] = r7;
    r[8] = r8;
    r[9] = r9;
    r[10] = r10;
    r[11] = r11;
    r[12] = r12;
    r[15] = at0 + 4 * static_cast<u32>(stop);
    c.f = f;
    c.ticks = t;
}

// Streaming Yaz0 (kHostPc; Site at its loop's head, word 49 of the function): whole turns while all their blocks'
// ticks fit under `left` (the JIT wouldn't stop inside them), straight through: a literal, a reference's first,
// second or third byte (and its copy), a new flags byte first when the mask ran out. Then, or for a turn it leaves to
// the JIT (one that won't fit, the output's end or its limit, memory that isn't plain), the head's first block
// (49-51: the state's test), as the JIT's: r15 52 or 86. r15 223 when the chunk or the output is done.
template <class Mem>
void YazStream(Cpu& c, Mem& m, const Site& s, u64 left) {
    u32* const r = c.r;
    auto T = [&](int i) -> u64 { return s.ticks[i - 49]; };
    auto S = [&](int a, int b) {
        u64 x = 0;
        for (int i = a; i <= b; i++)
            x += T(i);
        return x;
    };
    const u32 at0 = s.pc - 4 * 49;
    const u32 ctx = r[0];
    const u32 r6 = r[6], r7 = r[7], r8 = r[8], r9 = r[9];
    u32 r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], r5 = r[5], r10 = r[10], r11 = r[11], r12 = r[12];
    Flags f = c.f;
    u64 t = 0;
    const u64 head = S(49, 51), s52 = S(52, 53), s54 = S(54, 56), s194 = S(194, 198), s199 = S(199, 202),
              lit = S(203, 211), ref1 = S(212, 214), tail = S(215, 219), pass = S(220, 221), t222 = T(222),
              s86 = S(86, 91), s96 = S(96, 99), s111 = S(111, 115), s116 = S(116, 121), s122 = S(122, 125),
              s124 = S(124, 125), s126 = S(126, 131), s132 = S(132, 134), s192 = S(192, 193), s135 = S(135, 142),
              s145 = S(145, 148), s153 = S(153, 156), s171 = S(171, 175), s176 = S(176, 180), s181 = S(181, 184),
              s183 = S(183, 184), s185 = S(185, 190), t191 = T(191), t219 = T(219);
    // the context's fields as the code reads and writes them
    u32 dst = m.Read32(ctx), rem = m.Read32(ctx + 4);
    const u32 limit = m.Read32(ctx + 8);
    u32 mask = m.Read8(ctx + 12), flags = m.Read8(ctx + 13), hi = m.Read8(ctx + 14), state = m.Read8(ctx + 15);
    u32 dist = m.Read16(ctx + 16);
    auto clear = [&](u32 a, u32 n) { return a + n <= ctx || a >= ctx + 20; };  // (not over the context)
    // a reference's bytes from dst - d to dst, forward a byte at a time (a near one repeats its bytes)
    auto copy = [&](u32 d, u32 len) {
        const std::uint8_t* from = m.Bytes(dst - d, len);
        std::uint8_t* to = from != nullptr ? m.Bytes(dst, len) : nullptr;
        if (to != nullptr) {
            for (u32 i = 0; i < len; i++)
                to[i] = from[i];
        } else {
            for (u32 i = 0; i < len; i++)
                m.Write8(dst + i, m.Read8(dst - d + i));
        }
        return m.Read8(dst + len - 1);
    };
    u32 stop = 0;
    for (;;) {
        // (each turn ends in 219 cmp r2, #0; 220-221 ldrne r3, [r0, #4], cmpne r3, #0, a run of their own: when it
        // fails, a tick for each and a check of the slice before 222; 222 bgt to the head)
        u32 r2_after = r2;
        u64 tk = head;
        if (state == 2) {
            if (r2 < 1)
                break;
            const u32 len = m.Read8(r1) + 18;
            if (len > rem || !clear(dst, len))
                break;
            const u32 odd = len & 1, pairs = len >> 1;
            r2_after = r2 - 1;
            tk += s86 + s96 + s111 + s116 + (odd ? s122 : s124) + u64(pairs) * s126 + s132 + s192 + t219 +
                  (r2_after != 0 ? pass : 2) + t222;
            if (t + tk >= left)
                break;
            r1 += 1;
            rem -= len;
            const u32 last = copy(dist, len);
            if (pairs > 0)
                r11 = last;
            r4 = dst + len - 1;
            r5 = 0;
            r10 = dst;
            r12 = dst - dist + len - 1;
            dst += len;
            r3 = dst;
            state = r6;
        } else if (state == 1) {
            if (r2 < 1)
                break;
            const u32 b = m.Read8(r1);
            const u32 word = b | (hi << 8);
            const u32 count = word >> 12, d = (word & 0x0FFF) + 1;
            r2_after = r2 - 1;
            if (count == 0) {
                tk += s52 + s135 + T(143) + T(144) + t219 + (r2_after != 0 ? pass : 2) + t222;
                if (t + tk >= left)
                    break;
                r1 += 1;
                dist = d;
                state = r7;
                r12 = word & 0x0FFF;
                r4 = d;
                r3 = 0;
            } else {
                const u32 len = count + 2;
                if (len > rem || !clear(dst, len))
                    break;
                const u32 odd = len & 1, pairs = len >> 1;
                tk += s52 + s135 + 1 + T(144) + s145 + s153 + s171 + s176 + (odd ? s181 : s183) +
                      (pairs > 0 ? u64(pairs) * s185 + t191 : 0) + s132 + s192 + t219 +
                      (r2_after != 0 ? pass : 2) + t222;
                if (t + tk >= left)
                    break;
                r1 += 1;
                dist = d;
                rem -= len;
                const u32 last = copy(d, len);
                if (pairs > 0)
                    r11 = last;
                r4 = dst + len - 1;
                r5 = 0;
                r10 = dst;
                r12 = dst - d + len - 1;
                dst += len;
                r3 = dst;
                state = r6;
            }
        } else {
            // state 0: the flags' byte first when the mask ran out (194-198), then the item (199-202, 203-211 or
            // 212-214, 215-219)
            u32 pos = r1, fl = flags, mk = mask, r2_now = r2;
            tk += s52 + s54;
            if (mask == 0) {
                if (r2 < 1)
                    break;
                tk += s194;
                fl = m.Read8(pos);
                pos += 1;
                r2_now = r2 - 1;
                mk = r8 & 0xFF;
                if (r2_now == 0)
                    break;  // (the chunk ends there: left to the JIT)
            } else {
                tk += T(57);
            }
            const bool literal = (fl & mk) != 0;
            r2_after = r2_now - 1;
            tk += s199 + (literal ? lit : ref1) + tail + (r2_after != 0 ? pass : 2) + t222;
            if (literal && !clear(dst, 1))
                break;
            if (t + tk >= left)
                break;
            flags = fl;
            mask = mk;
            const u32 byte = m.Read8(pos);
            r1 = pos + 1;
            if (literal) {
                m.Write8(dst, byte);
                r4 = dst + 1;
                dst += 1;
                rem -= 1;
                r12 = byte;
            } else {
                hi = byte;
                state = r9;
                r12 = mk;
            }
            mask = (mask >> 1) & 0xFF;
            r3 = mask;
        }
        t += tk;
        r2 = r2_after;
        // the chain: r3 the output left while the chunk isn't done; on to the head while it's above 0
        if (r2 != 0) {
            r3 = rem;
            f = Sub(rem, 0);
        } else {
            f = Sub(r2, 0);
        }
        if (!(!f.z && f.n == f.v)) {
            stop = 223;
            break;
        }
    }
    if (stop == 0) {
        // the head's first block as the JIT's (49 ldrb r3, [r0, #0xf]; 50 cmp r3, #2; 51 beq 86)
        r3 = state;
        f = Sub(state, 2);
        t += head;
        stop = f.z ? 86 : 52;
    }
    m.Write32(ctx, dst);
    m.Write32(ctx + 4, rem);
    m.Write8(ctx + 12, mask);
    m.Write8(ctx + 13, flags);
    m.Write8(ctx + 14, hi);
    m.Write8(ctx + 15, state);
    m.Write16(ctx + 16, dist);
    (void)limit;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[4] = r4;
    r[5] = r5;
    r[10] = r10;
    r[11] = r11;
    r[12] = r12;
    r[15] = at0 + 4 * stop;
    c.f = f;
    c.ticks = t;
}

// Captain Toad's streaming Yaz0 (kHostPc; Site at its loop's head, word 76): whole turns while all their blocks' ticks
// fit under `left`, straight through, as kYazStream's; else the head's first block (76-77) as the JIT's: r15 78 or
// 89. r15 189 (the fields stored back, then the return) when the chunk or the output is done.
template <class Mem>
void YazStreamT(Cpu& c, Mem& m, const Site& s, u64 left) {
    u32* const r = c.r;
    auto T = [&](int i) -> u64 { return s.ticks[i - 76]; };
    auto S = [&](int a, int b) {
        u64 x = 0;
        for (int i = a; i <= b; i++)
            x += T(i);
        return x;
    };
    const u32 at0 = s.pc - 4 * 76;
    const u32 r0 = r[0];
    u32 r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], r5 = r[5], r6 = r[6], r7 = r[7], r8 = r[8], r9 = r[9], r10 = r[10],
        r11 = r[11], r12 = r[12], lr = r[14];
    Flags f = c.f;
    u64 t = 0;
    const u64 s76 = S(76, 77), s78 = S(78, 79), s80 = S(80, 81), t82 = T(82), s172 = S(172, 175), s176 = S(176, 177),
              lit = S(178, 181), ref1 = S(182, 183), s184 = S(184, 186), t186 = T(186), t187 = T(187),
              t188 = T(188), s89 = S(89, 93), s98 = S(98, 100), s107 = S(107, 113), s114 = S(114, 117),
              s116 = S(116, 117), s118 = S(118, 123), s124 = S(124, 125), s170 = S(170, 171), s126 = S(126, 133),
              s134 = S(134, 136), s141 = S(141, 143), s152 = S(152, 158), s159 = S(159, 162), s161 = S(161, 162),
              s163 = S(163, 168), s169 = S(169, 171);
    // a reference's bytes from r3 - d to r3, forward a byte at a time (a near one repeats its bytes)
    auto copy = [&](u32 d, u32 len) {
        const std::uint8_t* from = m.Bytes(r3 - d, len);
        std::uint8_t* to = from != nullptr ? m.Bytes(r3, len) : nullptr;
        if (to != nullptr) {
            for (u32 i = 0; i < len; i++)
                to[i] = from[i];
        } else {
            for (u32 i = 0; i < len; i++)
                m.Write8(r3 + i, m.Read8(r3 - d + i));
        }
        return m.Read8(r3 + len - 1);
    };
    u32 stop = 0;
    for (;;) {
        // (each turn ends in 186 cmp r2, #0; 187 cmpne r7, #0, a run of its own: one tick when it fails, and a check
        // of the slice before 188; 188 bgt to the head)
        u32 r2_after;
        u64 tk;
        if (r4 == 2) {
            if (r2 < 1)
                break;
            const u32 len = m.Read8(r1) + 18;
            if (len > r7)
                break;
            const u32 odd = len & 1, pairs = len >> 1;
            r2_after = r2 - 1;
            tk = s76 + s89 + s98 + s107 + (odd ? s114 : s116) + u64(pairs) * s118 + s124 + s170 + t186 +
                 (r2_after != 0 ? t187 : 1) + t188;
            if (t + tk >= left)
                break;
            r1 += 1;
            r7 -= len;
            const u32 last = copy(r12, len);
            if (pairs > 0)
                r11 = last;
            r5 = r3 - r12 + len - 1;
            r6 = 0;
            r8 = len;
            r3 += len;
            r4 = 0;
        } else if (r4 == 1) {
            if (r2 < 1)
                break;
            const u32 word = m.Read8(r1) | (r10 << 8);
            const u32 count = word >> 12, d = (word & 0x0FFF) + 1;
            r2_after = r2 - 1;
            if (count == 0) {
                tk = s76 + s78 + s126 + t186 + (r2_after != 0 ? t187 : 1) + t188;
                if (t + tk >= left)
                    break;
                r1 += 1;
                r5 = word & 0x0FFF;
                r12 = d;
                r4 = 2;
            } else {
                const u32 len = count + 2;
                if (len > r7)
                    break;
                const u32 odd = len & 1, pairs = len >> 1;
                tk = s76 + s78 + s126 + s134 + s141 + s152 + (odd ? s159 : s161) + u64(pairs) * s163 + s169 + t186 +
                     (r2_after != 0 ? t187 : 1) + t188;
                if (t + tk >= left)
                    break;
                r1 += 1;
                r12 = d;
                r7 -= len;
                const u32 last = copy(d, len);
                if (pairs > 0)
                    r8 = last;
                r5 = r3 - d + len - 1;
                r6 = 0;
                r11 = len;
                r3 += len;
                r4 = 0;
            }
        } else {
            // state 0: the flags' byte first when the mask ran out (172-175), then the item (176-177; 178-181 or
            // 182-183; 184-185)
            u32 pos = r1, fl = lr, mk = r9, r2_now = r2;
            tk = s76 + s78 + s80;
            if (r9 == 0) {
                if (r2 < 1)
                    break;
                tk += s172;
                fl = m.Read8(pos);
                pos += 1;
                r2_now = r2 - 1;
                mk = 128;
                if (r2_now == 0)
                    break;  // (the chunk ends there: left to the JIT)
            } else {
                tk += t82;
            }
            const bool literal = (fl & mk) != 0;
            r2_after = r2_now - 1;
            tk += s176 + (literal ? lit : ref1) + s184 + (r2_after != 0 ? t187 : 1) + t188;
            if (t + tk >= left)
                break;
            lr = fl;
            const u32 byte = m.Read8(pos);
            r1 = pos + 1;
            if (literal) {
                r5 = byte;
                r7 -= 1;
                m.Write8(r3, byte);
                r3 += 1;
            } else {
                r10 = byte;
                r4 = 1;
            }
            r9 = mk >> 1;
        }
        t += tk;
        r2 = r2_after;
        // the chain: on to the head while the chunk isn't done and the output left is above 0
        f = Sub(r2, 0);
        if (r2 != 0)
            f = Sub(r7, 0);
        if (!(!f.z && f.n == f.v)) {
            stop = 189;
            break;
        }
    }
    if (stop == 0) {
        // the head's first block as the JIT's (76 cmp r4, #2; 77 beq 89)
        f = Sub(r4, 2);
        t += s76;
        stop = f.z ? 89 : 78;
    }
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[4] = r4;
    r[5] = r5;
    r[6] = r6;
    r[7] = r7;
    r[8] = r8;
    r[9] = r9;
    r[10] = r10;
    r[11] = r11;
    r[12] = r12;
    r[14] = lr;
    r[15] = at0 + 4 * stop;
    (void)r0;
    c.f = f;
    c.ticks = t;
}

// CRC-16 CCITT bit by bit (kHostPc; Site at the loop's head, word 0: mov r3, #4). Its blocks (blocks56.cpp): each
// step's uxthne is a run of its own, linked on without a check when it passes, one tick and a check of the slice when
// it fails; bne and blo end blocks with a check. Whole bytes while all their ticks fit under `left`, straight through;
// then the head's blocks as the JIT's up to their first check (r15 there). r15 16 (the return's block) when the
// bytes are done. `ntype`: tst, eorne, uxthne, then andeq (else tst, andeq, eorne, uxthne); poly, mask, count: their
// registers.
template <class Mem>
void Crc16(Cpu& c, Mem& m, const Site& s, u64 left, bool ntype, int poly_r, int mask_r, int count_r) {
    u32* const r = c.r;
    auto T = [&](int i) -> u64 { return s.ticks[i]; };
    const int c1 = ntype ? 3 : 4, c2 = ntype ? 7 : 8;  // (the two runs' words)
    u64 bA = 0, bB = 0, bC = 0, end = 0;
    for (int i = 1; i < c1; i++)
        bA += T(i);
    for (int i = c1 + 1; i < c2; i++)
        bB += T(i);
    for (int i = c2 + 1; i <= 10; i++)
        bC += T(i);
    for (int i = 11; i <= 15; i++)
        end += T(i);
    const u32 poly = r[poly_r], mask = r[mask_r];
    u32 r0 = r[0], r1 = r[1], r3 = r[3], count = r[count_r];
    const u32 r2 = r[2];
    Flags f = c.f;
    u64 t = 0;
    int stop = -1;
    auto step = [&](u32 x) { return (x & 0x8000) ? ((poly ^ (x << 1)) & 0xFFFF) : (mask & (x << 1)); };
    for (;;) {
        u32 x = r0;
        u64 tk = T(0) + 4 * (bA + bB + bC) + end;
        for (int k = 0; k < 8; k++) {
            tk += (x & 0x8000) ? T(k & 1 ? c2 : c1) : 1;
            x = step(x);
        }
        if (t + tk >= left)
            break;
        r3 = m.Read8(r1);  // 11 ldrb r3, [r1], #1; 12 add count; 13 cmp count, r2; 14 eor r0, r0, r3; 15 blo
        r1 += 1;
        count += 1;
        f = Sub(count, r2);
        r0 = x ^ r3;
        t += tk;
        if (f.c) {
            stop = 16;
            break;
        }
    }
    if (stop < 0) {
        // the head's blocks as the JIT's up to the first check: 0 mov r3, #4, then the first turn's two steps
        r3 = 4;
        t += T(0);
        for (int half = 0; half < 2 && stop < 0; half++) {
            const bool set = (r0 & 0x8000) != 0;  // tst r0, #0x8000 (C: the rotated immediate's bit 31, 0)
            f.n = false;
            f.z = !set;
            f.c = false;
            const int cw = half ? c2 : c1;
            if (!ntype) {  // tst, andeq, eorne; the run uxthne
                r0 = set ? (poly ^ (r0 << 1)) : (mask & (r0 << 1));
                t += half ? bB : bA;
                if (set) {
                    r0 &= 0xFFFF;
                    t += T(cw);
                } else {
                    t += 1;
                    stop = cw + 1;
                }
            } else {  // (the last step's andeq), tst, eorne; the run uxthne
                if (set)
                    r0 = poly ^ (r0 << 1);
                t += half ? bB : bA;
                if (set) {
                    r0 &= 0xFFFF;
                    t += T(cw);
                } else {
                    t += 1;
                    stop = cw + 1;  // (its andeq is the next block's first word)
                }
            }
        }
        if (stop < 0) {
            r3 -= 1;  // subs r3, r3, #1; bne to the turn's tst (andeq first in Nintendo's order: Z clear, nothing)
            f = Sub(4, 1);
            t += bC;
            stop = 1;
        }
    }
    r[0] = r0;
    r[1] = r1;
    r[3] = r3;
    r[count_r] = count;
    r[15] = s.pc + 4 * static_cast<u32>(stop);
    c.f = f;
    c.ticks = t;
}

}  // namespace EmuSuNi::NativeRoutines
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
#include <dynarmic/ir/terminal.h>
#endif
''', r'''
#include <dynarmic/ir/terminal.h>
#include <cctype>
#include <cstddef>
#include <deque>
#include <string>
#include "core/arm/dynarmic/emusuni_native_routines.h"
#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/nzcv_util.h"
#endif
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
''', r'''
#ifdef EMUSUNI_PORT
// (01.50, 3ds-jit-native-routines, scripts/3ds-jit-patches.py) The native library: which routines run natively (by
// their switch files, read with each JIT), the ticks the last one took and its list loop's way back (read by its
// block: EmuSuNiSubCyclesFrom, EmuSuNiCheckBitFrom), the places found (their literals and ticks, kept for the
// process), and each routine's calls (the 600-frame line).
static bool emusuni_native_on[32] = {};
static std::uint64_t emusuni_native_ticks = 0;
static std::uint8_t emusuni_native_again = 0;
static std::deque<EmuSuNi::NativeRoutines::Site> emusuni_native_sites;

// A switch file's words (lower case): /data/EmuSuNi/<name> on the PS4, the environment variable on the PC. False: none.
static bool EmuSuNiSwitchWords(const char* name, const char* env, std::string& words) {
    words.clear();
#if defined(__ORBIS__)
    (void)env;
    char path[96];
    std::snprintf(path, sizeof(path), "/data/EmuSuNi/%s", name);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    char text[256] = {};
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    words.assign(text, length);
#else
    (void)name;
    const char* value = std::getenv(env);
    if (value == nullptr) {
        return false;
    }
    words = value;
    if (words == "0") {
        words = "off";
    } else if (words == "1") {
        words = "on";
    }
#endif
    for (char& c : words) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}
// A word in a switch file's words (separated by anything but letters, digits, + and -): 1 as +word, 0 as word or
// -word, -1 not there (the last one counts).
static int EmuSuNiWordState(const std::string& words, const char* word) {
    auto part = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-'; };
    int state = -1;
    std::size_t i = 0;
    while (i < words.size()) {
        while (i < words.size() && !part(words[i])) {
            i++;
        }
        std::size_t j = i;
        while (j < words.size() && part(words[j])) {
            j++;
        }
        if (j > i) {
            const bool plus = words[i] == '+';
            const std::size_t from = (words[i] == '+' || words[i] == '-') ? i + 1 : i;
            if (words.compare(from, j - from, word) == 0 && std::strlen(word) == j - from) {
                state = plus ? 1 : 0;
            }
        }
        i = j;
    }
    return state;
}
#endif

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
    bool PreCodeReadHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        if (!emusuni_native_calls || is_thumb || !ir.block.empty() ||
            Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC() != pc) {
            return true;
        }
''', r'''
    bool PreCodeReadHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        if (is_thumb || !ir.block.empty() || Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC() != pc) {
            return true;
        }
        // (01.50) The native library (3ds-jit-native-routines, 3ds-jit-native-castlevania).
        if (EmuSuNiNativeRoutineBlock(pc, ir)) {
            return false;
        }
        if (!emusuni_native_calls) {
            return true;
        }
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
        // (the block's range: the words recognised)
        ir.current_location = ir.current_location.AdvancePC(static_cast<int>(4 * words));
        return false;
    }
#endif
''', r'''
        // (the block's range: the words recognised)
        ir.current_location = ir.current_location.AdvancePC(static_cast<int>(4 * words));
        return false;
    }

    // (01.50, 3ds-jit-native-routines) Guest memory for the native routines: the page table's pointer where there is
    // one (as the JIT's fast path), else Azahar's own access (the GPU's cache, and the rest, as the JIT's fallbacks).
    struct EmuSuNiNativeMemory {
        DynarmicUserCallbacks& cb;
        std::uint8_t* const* raw;
        std::uint8_t* Page(u32 address, u32 size) {
            if ((address & (size - 1)) != 0) {
                return nullptr;
            }
            std::uint8_t* page = raw[address >> Memory::CITRA_PAGE_BITS];
            return page != nullptr ? page + (address & Memory::CITRA_PAGE_MASK) : nullptr;
        }
        // Bytes inside one page with a pointer (plain memory), from any address: their host pointer.
        std::uint8_t* Bytes(u32 address, u32 n) {
            if ((address & Memory::CITRA_PAGE_MASK) + n > Memory::CITRA_PAGE_SIZE) {
                return nullptr;
            }
            std::uint8_t* page = raw[address >> Memory::CITRA_PAGE_BITS];
            return page != nullptr ? page + (address & Memory::CITRA_PAGE_MASK) : nullptr;
        }
        // Bytes from a word's address inside one page with a pointer (plain memory): their host pointer.
        std::uint8_t* Span(u32 address, u32 bytes) {
            if ((address & 3) != 0 || (address & Memory::CITRA_PAGE_MASK) + bytes > Memory::CITRA_PAGE_SIZE) {
                return nullptr;
            }
            std::uint8_t* page = raw[address >> Memory::CITRA_PAGE_BITS];
            return page != nullptr ? page + (address & Memory::CITRA_PAGE_MASK) : nullptr;
        }
        u32 Read8(u32 address) {
            if (const std::uint8_t* p = Page(address, 1)) {
                return *p;
            }
            return cb.MemoryRead8(address);
        }
        u32 Read16(u32 address) {
            if (const std::uint8_t* p = Page(address, 2)) {
                std::uint16_t value;
                std::memcpy(&value, p, 2);
                return value;
            }
            return cb.MemoryRead16(address);
        }
        u32 Read32(u32 address) {
            if (const std::uint8_t* p = Page(address, 4)) {
                u32 value;
                std::memcpy(&value, p, 4);
                return value;
            }
            return cb.MemoryRead32(address);
        }
        void Write8(u32 address, u32 value) {
            if (std::uint8_t* p = Page(address, 1)) {
                *p = static_cast<std::uint8_t>(value);
                return;
            }
            cb.MemoryWrite8(address, static_cast<std::uint8_t>(value));
        }
        void Write16(u32 address, u32 value) {
            if (std::uint8_t* p = Page(address, 2)) {
                const std::uint16_t v = static_cast<std::uint16_t>(value);
                std::memcpy(p, &v, 2);
                return;
            }
            cb.MemoryWrite16(address, static_cast<std::uint16_t>(value));
        }
        void Write32(u32 address, u32 value) {
            if (std::uint8_t* p = Page(address, 4)) {
                std::memcpy(p, &value, 4);
                return;
            }
            cb.MemoryWrite32(address, value);
        }
    };

    // (01.50) A place's routine, called from its block (dynarmic's CallHostFunction, its Site the argument): the
    // guest's registers, flags (in dynarmic's x64 order) and VFP registers straight in the JIT's state; its ticks (and
    // the list loop's way back) left for its block.
    template <EmuSuNi::NativeRoutines::Kind K>
    static void EmuSuNiNativeRoutineRun(std::uint64_t where, std::uint64_t left, std::uint64_t) {
        namespace NR = EmuSuNi::NativeRoutines;
        namespace X64 = Dynarmic::Backend::X64;
        const NR::Site& site = *reinterpret_cast<const NR::Site*>(where);
        X64::A32JitState& state = *static_cast<X64::A32JitState*>(site.state);
        NR::Cpu c{state.Reg.data(), 0, NR::FlagsOf(X64::NZCV::FromX64(state.cpsr_nzcv)), 0};
        EmuSuNiNativeMemory m{*static_cast<DynarmicUserCallbacks*>(site.host), site.raw};
        (void)left;
        if constexpr (K == NR::kCommandWriter) {
            NR::CommandWriter(c, m, site);
        } else if constexpr (K == NR::kFloat24Loop) {
            c.s0 = state.ExtReg[0];
            NR::Float24Loop(c, m, site, 256);  // (at most 256 turns a call: the block comes back while r1 isn't 0)
            state.ExtReg[0] = c.s0;
        } else if constexpr (K == NR::kHalf3) {
            NR::Half3(c, m, site);
        } else if constexpr (K == NR::kStrcmp) {
            NR::Strcmp(c, m, site);
        } else if constexpr (K == NR::kStrlen) {
            NR::Strlen(c, m, site);
        } else if constexpr (K == NR::kMemcpy4) {
            NR::Memcpy4(c, m, site);
        } else if constexpr (K == NR::kDivSigned || K == NR::kDivSignedPath || K == NR::kDivUnsigned) {
            NR::Divide(c, m, site);  // (a divisor of 0: on to __aeabi_idiv0, its block's check bit)
        } else if constexpr (K == NR::kListWalk) {
            const int way = NR::ListWalk(c, m, site, 4096);
            emusuni_native_again = way == 2 ? 1 : 0;
            state.Reg[15] = site.pc + (way == 0 ? 40 : 48);
        } else if constexpr (K == NR::kHeapBestFit) {
            NR::HeapBestFit(c, m, site);
        } else if constexpr (K == NR::kCrc32) {
            // whole when its ticks fit in the slice's (left: the cycles as its block starts), else its first block
            emusuni_native_again = NR::Crc32(c, m, site, left) ? 0 : 1;
        } else if constexpr (K == NR::kYazStream) {
            NR::YazStream(c, m, site, left);  // (r15 left: on in the JIT, or the chunk's end)
        } else if constexpr (K == NR::kYazStreamT) {
            NR::YazStreamT(c, m, site, left);
        } else if constexpr (K == NR::kCrc16A || K == NR::kCrc16B) {
            NR::Crc16(c, m, site, left, false, 12, 4, K == NR::kCrc16A ? 5 : 6);
        } else if constexpr (K == NR::kCrc16N) {
            NR::Crc16(c, m, site, left, true, 5, 4, 12);
        } else if constexpr (K == NR::kCrc16S) {
            NR::Crc16(c, m, site, left, true, 4, 6, 12);
        } else if constexpr (K == NR::kLz || K == NR::kLzHead) {
            NR::Lz(c, m, site, left, K == NR::kLz ? 0 : 43);  // (r15 left: where it stopped, or its return's block)
        } else if constexpr (K == NR::kYaz0Rev || K == NR::kYaz0Swap || K == NR::kYaz0Head) {
            // on while the JIT would go on (its slice), stopped at a block's end (r15) or at its pop
            const u32 prologue = K == NR::kYaz0Rev ? 8 : K == NR::kYaz0Swap ? 11 : 0;
            emusuni_native_again = NR::Yaz0(c, m, site, left, prologue) ? 0 : 1;
        }
        state.cpsr_nzcv = X64::NZCV::ToX64(NR::BitsOf(c.f));
        emusuni_native_ticks = c.ticks;
        EmuSuNi::Stats::counts[EmuSuNi::Stats::NativeRoutine0 + K]++;
    }

    // (01.50) Where a block starts at one of the library's routines (all its words), the block is a call to it, its
    // ticks taken from the cycles left, then its way out: lr; the float24 loop's exit when r1 is 0, else its head; the
    // division's __aeabi_idiv0 when the divisor is 0; the list loop's exit (its pc) or its head; the CRC-32's return,
    // or its first block's next word; Yaz0's pop, or the dispatcher where it stopped. False: none here.
    bool EmuSuNiNativeRoutineBlock(VAddr pc, Dynarmic::A32::IREmitter& ir) {
        namespace NR = EmuSuNi::NativeRoutines;
        if (!parent.current_page_table) {
            return false;
        }
        const auto first = memory.Read32OrNullopt(pc);
        if (!first) {
            return false;
        }
        const NR::Signature* sig = nullptr;
        for (const auto& s : NR::kSignatures) {
            if (!emusuni_native_on[s.kind] || !NR::SameWord(s.code[s.entry], *first)) {
                continue;
            }
            const VAddr start = pc - 4 * s.entry;
            bool same = true;
            for (u32 i = 0; i < s.words && same; i++) {
                const auto word = memory.Read32OrNullopt(start + 4 * i);
                same = word && NR::SameWord(s.code[i], *word);
            }
            if (same) {
                sig = &s;
                break;
            }
        }
        if (sig == nullptr) {
            return false;
        }
        void (*run)(std::uint64_t, std::uint64_t, std::uint64_t) = nullptr;
        switch (sig->kind) {
        case NR::kCommandWriter:
            run = &EmuSuNiNativeRoutineRun<NR::kCommandWriter>;
            break;
        case NR::kFloat24Loop:
            run = &EmuSuNiNativeRoutineRun<NR::kFloat24Loop>;
            break;
        case NR::kHalf3:
            run = &EmuSuNiNativeRoutineRun<NR::kHalf3>;
            break;
        case NR::kStrcmp:
            run = &EmuSuNiNativeRoutineRun<NR::kStrcmp>;
            break;
        case NR::kStrlen:
            run = &EmuSuNiNativeRoutineRun<NR::kStrlen>;
            break;
        case NR::kMemcpy4:
            run = &EmuSuNiNativeRoutineRun<NR::kMemcpy4>;
            break;
        case NR::kDivSigned:
            run = &EmuSuNiNativeRoutineRun<NR::kDivSigned>;
            break;
        case NR::kDivSignedPath:
            run = &EmuSuNiNativeRoutineRun<NR::kDivSignedPath>;
            break;
        case NR::kDivUnsigned:
            run = &EmuSuNiNativeRoutineRun<NR::kDivUnsigned>;
            break;
        case NR::kListWalk:
            run = &EmuSuNiNativeRoutineRun<NR::kListWalk>;
            break;
        case NR::kHeapBestFit:
            run = &EmuSuNiNativeRoutineRun<NR::kHeapBestFit>;
            break;
        case NR::kCrc32:
            run = &EmuSuNiNativeRoutineRun<NR::kCrc32>;
            break;
        case NR::kYaz0Rev:
            run = &EmuSuNiNativeRoutineRun<NR::kYaz0Rev>;
            break;
        case NR::kYaz0Swap:
            run = &EmuSuNiNativeRoutineRun<NR::kYaz0Swap>;
            break;
        case NR::kYaz0Head:
            run = &EmuSuNiNativeRoutineRun<NR::kYaz0Head>;
            break;
        case NR::kLz:
            run = &EmuSuNiNativeRoutineRun<NR::kLz>;
            break;
        case NR::kYazStream:
            run = &EmuSuNiNativeRoutineRun<NR::kYazStream>;
            break;
        case NR::kYazStreamT:
            run = &EmuSuNiNativeRoutineRun<NR::kYazStreamT>;
            break;
        case NR::kCrc16A:
            run = &EmuSuNiNativeRoutineRun<NR::kCrc16A>;
            break;
        case NR::kCrc16B:
            run = &EmuSuNiNativeRoutineRun<NR::kCrc16B>;
            break;
        case NR::kCrc16N:
            run = &EmuSuNiNativeRoutineRun<NR::kCrc16N>;
            break;
        case NR::kCrc16S:
            run = &EmuSuNiNativeRoutineRun<NR::kCrc16S>;
            break;
        case NR::kLzHead:
            run = &EmuSuNiNativeRoutineRun<NR::kLzHead>;
            break;
        default:
            return false;
        }
        const auto* process = parent.system.Kernel().GetCurrentProcess().get();
        const u64 title = process != nullptr && process->codeset ? process->codeset->program_id : 0;
        if (sig->castlevania && title != 0x0004000000096600ull) {
            return false;
        }
        NR::Site site;
        site.kind = sig->kind;
        site.pc = pc;
        // (this JIT's state: its registers are its first member; the page table's pointers; the callbacks)
        static_assert(offsetof(Dynarmic::Backend::X64::A32JitState, Reg) == 0);
        site.state = static_cast<void*>(parent.jit->Regs().data());
        site.raw = parent.current_page_table->GetPointerArray().data();
        site.host = this;
        const VAddr start = pc - 4 * sig->entry;
        auto tick = [&](VAddr at) {
            const auto word = memory.Read32OrNullopt(at);
            return static_cast<std::uint16_t>(word ? Core::TicksForInstruction(false, *word) : 1);
        };
        for (int k = 0; k < 2; k++) {
            if (sig->literal[k] != 0) {
                const auto literal = memory.Read32OrNullopt(pc + sig->literal[k]);
                if (!literal) {
                    return false;
                }
                site.literal[k] = *literal;
            }
        }
        for (u32 i = 0; i < sig->words; i++) {
            site.ticks[i] = tick(start + 4 * i);
        }
        if (sig->kind == NR::kCrc32) {
            site.table = NR::CrcTable(site.literal[0]);  // (its polynomial's table: four at most)
            if (site.table == nullptr) {
                return false;
            }
        }
        if (sig->exit == NR::kDivide) {
            // The body (__aeabi_idivmod's 139 words): here, or where the unsigned entry's first branch goes (word 66).
            VAddr body = start;
            if (sig->kind == NR::kDivUnsigned) {
                const auto branch = memory.Read32OrNullopt(pc + 8);
                if (!branch) {
                    return false;
                }
                body = NR::BranchTarget(pc + 8, *branch) - 4 * 66;
                for (u32 i = 0; i < 12; i++) {
                    site.entry_ticks[i] = site.ticks[i];
                }
            }
            for (u32 i = 0; i < 139; i++) {
                const auto word = memory.Read32OrNullopt(body + 4 * i);
                if (!word || !NR::SameWord(NR::kDivideCode[i], *word)) {
                    return false;
                }
                site.ticks[i] = tick(body + 4 * i);
            }
            const auto bl = memory.Read32OrNullopt(body + 4 * 137);
            site.literal[0] = body;
            site.literal[1] = NR::BranchTarget(body + 4 * 137, *bl);
        }
        const NR::Site* kept = nullptr;
        for (const NR::Site& s : emusuni_native_sites) {
            if (s.kind == site.kind && s.pc == site.pc && s.literal[0] == site.literal[0] &&
                s.literal[1] == site.literal[1] && s.state == site.state && s.raw == site.raw) {
                kept = &s;
            }
        }
        if (kept == nullptr) {
            emusuni_native_sites.push_back(site);
            kept = &emusuni_native_sites.back();
            LOG_INFO(Core_ARM11, "EmuSuNi: native library: {} at {:08X} in {:016X} runs natively ({})", sig->name, pc,
                     title, sig->castlevania ? "3ds-jit-native-castlevania" : "3ds-jit-native-routines");
        }
        using Dynarmic::A32::Reg;
        namespace Term = Dynarmic::IR::Term;
        if (sig->exit == NR::kDivide) {
            ir.SetCheckBit(ir.IsZero(ir.GetRegister(Reg::R1)));  // (the divisor as it comes: 0 goes to the handler)
        }
        ir.CallHostFunction(run, ir.Imm64(reinterpret_cast<std::uint64_t>(kept)),
                            sig->exit == NR::kWholeOrFirst || sig->exit == NR::kStopsLikeJit ||
                                    sig->exit == NR::kHostPc
                                ? ir.EmuSuNiCyclesLeft()
                                : ir.Imm64(0),
                            ir.Imm64(0));
        ir.EmuSuNiSubCyclesFrom(ir.Imm64(reinterpret_cast<std::uint64_t>(&emusuni_native_ticks)));
        const auto head = ir.current_location;
        const auto after = ir.current_location.AdvancePC(static_cast<int>(4 * (sig->words - sig->entry)));
        switch (sig->exit) {
        case NR::kBxLr:
            ir.BXWritePC(ir.GetRegister(Reg::LR));
            ir.SetTerm(Term::PopRSBHint{});
            break;
        case NR::kMovPcLr:
            ir.ALUWritePC(ir.GetRegister(Reg::LR));  // (mov pc, lr: as dynarmic translates it)
            ir.SetTerm(Term::PopRSBHint{});
            break;
        case NR::kLoopHead:
            ir.SetCheckBit(ir.IsZero(ir.GetRegister(Reg::R1)));
            ir.SetTerm(Term::CheckBit{Term::LinkBlock{after}, Term::LinkBlock{head}});
            break;
        case NR::kDivide:
            // lr: the caller's, or (a divisor of 0) the handler's pop after its bl; then __aeabi_idiv0 or a return.
            ir.BXWritePC(ir.GetRegister(Reg::LR));
            ir.SetTerm(Term::CheckBit{
                Term::LinkBlock{Dynarmic::A32::LocationDescriptor{head}.SetPC(kept->literal[1])},
                Term::PopRSBHint{}});
            break;
        case NR::kListExits:
            ir.EmuSuNiCheckBitFrom(ir.Imm64(reinterpret_cast<std::uint64_t>(&emusuni_native_again)));
            ir.SetTerm(Term::CheckBit{Term::LinkBlock{head}, Term::FastDispatchHint{}});
            break;
        case NR::kWholeOrFirst:
            // its first block's next word (the JIT's from there), or its return's word (the JIT's bx lr)
            ir.EmuSuNiCheckBitFrom(ir.Imm64(reinterpret_cast<std::uint64_t>(&emusuni_native_again)));
            ir.SetTerm(Term::CheckBit{
                Term::LinkBlock{head.AdvancePC(static_cast<int>(4 * (sig->resume - sig->entry)))},
                Term::LinkBlock{head.AdvancePC(static_cast<int>(4 * (sig->done - sig->entry)))}});
            break;
        case NR::kHostPc:
            // on where the routine left r15 (where the JIT's slice would have stopped it, or its return's block)
            ir.SetTerm(Term::FastDispatchHint{});
            break;
        case NR::kStopsLikeJit:
            // stopped where the JIT's slice ends (r15 its next block: the dispatcher), or on to its pop's word
            ir.EmuSuNiCheckBitFrom(ir.Imm64(reinterpret_cast<std::uint64_t>(&emusuni_native_again)));
            ir.SetTerm(Term::CheckBit{
                Term::ReturnToDispatch{},
                Term::LinkBlock{head.AdvancePC(static_cast<int>(4 * (sig->done - sig->entry)))}});
            break;
        }
        ir.current_location = after;
        return true;
    }
#endif
''')
patch('src/core/arm/dynarmic/arm_dynarmic.cpp', r'''
             Dynarmic::A32::emusuni_cond_alu ? "as selects inside the block" : "in blocks of their own (off)");
''', r'''
             Dynarmic::A32::emusuni_cond_alu ? "as selects inside the block" : "in blocks of their own (off)");
    // (01.50, scripts/3ds-jit-patches.py) The native library: each routine as its default (on: those faster than
    // the JIT on the PC) unless /data/EmuSuNi/3ds-jit-native-routines says off (all), all, +name or name / -name;
    // Castlevania's only with the word on (or +name) in /data/EmuSuNi/3ds-jit-native-castlevania.
    {
        namespace NR = EmuSuNi::NativeRoutines;
        std::string common, castlevania;
        const bool common_file = EmuSuNiSwitchWords("3ds-jit-native-routines", "EMUSUNI_3DS_JIT_NATIVE_ROUTINES", common);
        const bool cv_file =
            EmuSuNiSwitchWords("3ds-jit-native-castlevania", "EMUSUNI_3DS_JIT_NATIVE_CASTLEVANIA", castlevania);
        const bool all_off = common_file && EmuSuNiWordState(common, "off") >= 0;
        const bool all_on = common_file && EmuSuNiWordState(common, "all") >= 0;
        const bool cv_on = cv_file &&
                           (EmuSuNiWordState(castlevania, "on") >= 0 || EmuSuNiWordState(castlevania, "all") >= 0) &&
                           EmuSuNiWordState(castlevania, "off") < 0;
        std::string on_list, off_list;
        for (const auto& s : NR::kSignatures) {
            const std::string& words = s.castlevania ? castlevania : common;
            bool on = s.castlevania ? cv_on : all_off ? false : all_on ? true : s.on;
            const int named = EmuSuNiWordState(words, s.word);
            if (named >= 0) {
                on = named == 1;
            }
            emusuni_native_on[s.kind] = on;
            std::string& list = on ? on_list : off_list;
            if (list.find(s.word) == std::string::npos) {
                list += list.empty() ? "" : " ";
                list += s.word;
            }
        }
        LOG_INFO(Core_ARM11,
                 "EmuSuNi: core {}'s JIT: the native library (3ds-jit-native-routines, 3ds-jit-native-castlevania): "
                 "on: {}; off: {}",
                 GetID(), on_list.empty() ? "none" : on_list, off_list.empty() ? "none" : off_list);
    }
''')
patch('src/common/emusuni_frame_stats.h', r'''
                         "EmuSuNi: native calls in those 600 frames (3ds-jit-native-calls): memcpy {} times, {} KiB",
                         dc[NativeMemcpyCalls], dc[NativeMemcpyBytes] / 1024);
            }
''', r'''
                         "EmuSuNi: native calls in those 600 frames (3ds-jit-native-calls): memcpy {} times, {} KiB",
                         dc[NativeMemcpyCalls], dc[NativeMemcpyBytes] / 1024);
            }
            // (01.50) The native library's calls (3ds-jit-native-routines, scripts/3ds-jit-patches.py).
            {
                static const char* const names[32] = {"", "writer", "float24", "half3", "strcmp", "strlen", "memcpy4",
                                                      "idivmod", "idivmod (negative)", "uidivmod", "listwalk",
                                                      "heapfit", "crc32", "yaz0", "yaz0 (swap)", "yaz0 (loop)",
                                                      "lz", "lz (item)", "yaz0s", "yaz0s (toad)", "crc16",
                                                      "crc16 (b)", "crc16 (n)", "crc16 (s)"};
                std::string line;
                for (int k = 1; k < 32; k++) {
                    if (dc[NativeRoutine0 + k] != 0) {
                        line += fmt::format("{}{} {}", line.empty() ? "" : ", ", names[k], dc[NativeRoutine0 + k]);
                    }
                }
                if (!line.empty()) {
                    LOG_INFO(Frontend, "EmuSuNi: the native library in those 600 frames (3ds-jit-native-routines): {}",
                             line);
                }
            }
''')
patch('src/common/emusuni_stats.h', r'''
    NativeMemcpyCalls, NativeMemcpyBytes, // (01.41) 3ds-jit-native-calls: the game's memcpy run natively
''', r'''
    NativeMemcpyCalls, NativeMemcpyBytes, // (01.41) 3ds-jit-native-calls: the game's memcpy run natively
    // (01.50) the native library's calls, by EmuSuNi::NativeRoutines::Kind (3ds-jit-native-routines)
    NativeRoutine0, NativeRoutine1, NativeRoutine2, NativeRoutine3, NativeRoutine4, NativeRoutine5, NativeRoutine6,
    NativeRoutine7, NativeRoutine8, NativeRoutine9, NativeRoutine10, NativeRoutine11, NativeRoutine12, NativeRoutine13,
    NativeRoutine14, NativeRoutine15, NativeRoutine16, NativeRoutine17, NativeRoutine18, NativeRoutine19,
    NativeRoutine20, NativeRoutine21, NativeRoutine22, NativeRoutine23, NativeRoutine24, NativeRoutine25,
    NativeRoutine26, NativeRoutine27, NativeRoutine28, NativeRoutine29, NativeRoutine30, NativeRoutine31,
''')

# --- 01.65: 3ds-fastmem-2m (e2 of the JIT plan). The 01.64 console's memory probe (port/ps4_mem_probe.cpp): direct
# memory mapped 2 MiB aligned (its direct memory and its address both) is in 2 MiB pages (4,096 of 4,096 of its 16 KiB
# pages), the same memory 16 KiB off isn't; a walk that crosses a page each step costs 16.6 ns in it against 46.5 over
# 16 MiB, 69.6 against 149.2 over 64 MiB (fewer TLB misses). So the 3DS's memory goes in 2 MiB pages where it can:
# - Each memory block of 2 MiB or more (FCRAM, VRAM, the New 3DS RAM: common/emusuni_mem.cpp) gets its direct memory
#   and its mapping 2 MiB aligned, its size rounded up to 2 MiB: whatever reads it through the page table (Azahar's
#   own accesses, the GPU's commands, the JIT's fallbacks) does so in 2 MiB pages. Refused: as before (64 KiB).
# - The fast memory (common/emusuni_fastmem.cpp's Sync) works in whole 2 MiB chunks of the guest's: a chunk whose 128
#   groups of 16 KiB all lie in order in direct memory from a 2 MiB-aligned offset (the linear heap always does: its
#   address and FCRAM's offset differ by 0x14000000 or 0x30000000) is mapped as one 2 MiB page; any other as before,
#   group by group. A chunk that stops being whole (a page the GPU's cache starts to watch, a mapping that changes) is
#   given back whole and mapped again by groups (never a part of a 2 MiB mapping unmapped); after 8 times it stays in
#   groups (no churn). A 2 MiB mapping refused: its groups as before.
# The file /data/EmuSuNi/3ds-fastmem-2m with the word off (the PC: EMUSUNI_3DS_FASTMEM_2M=0) goes back to what was
# before. At the game's 600th frame a line: the chunks mapped in 2 MiB pages and how many of their 16 KiB pages the
# kernel says are in 2 MiB pages (mincore's MINCORE_SUPER, as the probe), and the same for the memory blocks; at its
# end the counts. The fast memory's self-test and its marker files cover this too (a start that dies while trying it
# turns the fast memory off for the next).
patch('src/common/emusuni_fastmem.h', r'''// This game uses fast memory.
bool On();
''', r'''// This game uses fast memory.
bool On();

// (01.65, 3ds-fastmem-2m) This game's memory blocks and fast memory in 2 MiB pages where they can (read at its start).
bool LargePages();
''')
patch('src/common/emusuni_mem.cpp', r'''constexpr size_t PAGE = 16384;
''', r'''constexpr size_t PAGE = 16384;
constexpr size_t LARGE = 2 * 1024 * 1024; // (01.65, 3ds-fastmem-2m) the kernel's large page
''')
patch('src/common/emusuni_mem.cpp', r'''    if (direct && sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), size,
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
''', r'''    // (01.65, 3ds-fastmem-2m) A block of 2 MiB or more first in 2 MiB pages: its direct memory and its mapping 2 MiB
    // aligned and its size a multiple of 2 MiB (the kernel then maps it with 2 MiB pages, and the fast memory can map
    // its aligned chunks so too); refused, as before.
    bool large_pages = false;
    for (int attempt = Fastmem::LargePages() && size >= LARGE ? 0 : 1; direct && at == nullptr && attempt < 2;
         attempt++) {
        const bool large = attempt == 0;
        const size_t length = large ? (size + LARGE - 1) & ~(LARGE - 1) : size;
        if (sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), length,
                                          large ? LARGE : 64 * 1024, WB_ONION, &phys) != 0) {
            continue;
        }
        // A hint well above 0x200000000, where the kernel library puts its own objects.
        void* base = reinterpret_cast<void*>(0x2600000000ull);
        const int32_t ret = sceKernelMapDirectMemory(&base, length, PROT_CPU_RW, 0, phys, large ? LARGE : PAGE);
        if (ret == 0 && base != nullptr) {
            at = base;
            size = length;
            kind = Kind::Direct;
            large_pages = large;
            std::memset(at, 0, size); // direct memory comes as it was left
        } else {
            LOG_WARNING(Common, "EmuSuNi: mapping {} KiB of direct memory{} failed ({:#x})", length / 1024,
                        large ? " in 2 MiB pages" : "", static_cast<unsigned>(ret));
            sceKernelReleaseDirectMemory(phys, length);
        }
    }
''')
patch('src/common/emusuni_mem.cpp', r'''    LOG_INFO(Common, "EmuSuNi: {} KiB of {} at {}", size / 1024,
             kind == Kind::Direct ? "direct memory" : kind == Kind::Mapped ? "mapped memory" : "the heap", at);
''', r'''    LOG_INFO(Common, "EmuSuNi: {} KiB of {} at {}", size / 1024,
             kind != Kind::Direct  ? (kind == Kind::Mapped ? "mapped memory" : "the heap")
             : large_pages         ? "direct memory, 2 MiB aligned (3ds-fastmem-2m)"
                                   : "direct memory",
             at);
''')
patch('src/common/emusuni_fastmem.cpp', r'''struct Arena {
    std::uint8_t* base = nullptr;
    // Per 16 KiB group of the guest's: 0 nothing mapped there (reserved), else 1 + the direct memory offset mapped
    // there in 16 KiB units (consecutive groups of one run have consecutive values).
    std::vector<std::uint32_t> mapped;
    std::size_t groups_mapped = 0;
};
''', r'''struct Arena {
    std::uint8_t* base = nullptr;
    // Per 16 KiB group of the guest's: 0 nothing mapped there (reserved), else 1 + the direct memory offset mapped
    // there in 16 KiB units (consecutive groups of one run have consecutive values).
    std::vector<std::uint32_t> mapped;
    std::size_t groups_mapped = 0;
    // (01.65, 3ds-fastmem-2m) Per 2 MiB chunk of the guest's: mapped as one 2 MiB page; how often it was split again.
    std::vector<std::uint8_t> big, splits;
};

namespace {
// (01.65, 3ds-fastmem-2m) This game's switch (its start) and counts; the page tables' ranges, for the 600th frame's line.
bool g_large = false;
std::uint64_t g_large_maps = 0, g_large_refused = 0, g_large_splits = 0;
bool g_large_said = false;
Arena* g_arenas[4] = {};
constexpr int kLargeMaxSplits = 8;

bool LargeSwitch() {
#if defined(__ORBIS__)
    std::FILE* file = std::fopen("/data/EmuSuNi/3ds-fastmem-2m", "rb");
    if (file == nullptr) {
        return true;
    }
    char text[64] = "";
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return std::strstr(text, "off") == nullptr;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM_2M");
    return value == nullptr || (std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0);
#endif
}

#if defined(__ORBIS__)
extern "C" int emusuni_fastmem_mincore(const void* address, std::size_t length, char* vec) __asm__("mincore");
#endif
// Of [at, at + length) (16 KiB aligned): the 16 KiB pages the kernel says are in 2 MiB pages (mincore's
// MINCORE_SUPER, 0x20); -1 where it can't say (the PC).
long SuperPages(const void* at, std::size_t length) {
#if defined(__ORBIS__)
    std::vector<char> vec(length / Dmem::kPage + 16, 0);
    if (emusuni_fastmem_mincore(at, length, vec.data()) != 0) {
        return -1;
    }
    long super = 0;
    for (std::size_t i = 0; i < length / Dmem::kPage; i++) {
        super += (vec[i] & 0x20) ? 1 : 0;
    }
    return super;
#else
    (void)at;
    (void)length;
    return -1;
#endif
}
} // namespace
''')
patch('src/common/emusuni_fastmem.cpp', r'''#include <algorithm>
#include <atomic>
''', r'''#include <algorithm>
#include <atomic>
#include <cctype>
''')
patch('src/common/emusuni_fastmem.cpp', r'''    arena->mapped.assign(kGroups, 0);
''', r'''    arena->mapped.assign(kGroups, 0);
    arena->big.assign(kSpace / kLarge, 0);
    arena->splits.assign(kSpace / kLarge, 0);
    for (Arena*& slot : g_arenas) {
        if (slot == nullptr) {
            slot = arena;
            break;
        }
    }
''')
patch('src/common/emusuni_fastmem.cpp', r'''    sceKernelMunmap(arena->base, kSpace + kGuard);
    delete arena;
    g_live--;
''', r'''    sceKernelMunmap(arena->base, kSpace + kGuard);
    for (Arena*& slot : g_arenas) {
        if (slot == arena) {
            slot = nullptr;
        }
    }
    delete arena;
    g_live--;
''')
patch('src/common/emusuni_fastmem.cpp', r'''void Decide() {
    std::lock_guard lock{g_lock};
    g_on = false;
''', r'''void Decide() {
    std::lock_guard lock{g_lock};
    // (01.65) 3ds-fastmem-2m: read first (the memory blocks use it with the fast memory off too).
    g_large = LargeSwitch();
    g_large_maps = g_large_refused = g_large_splits = 0;
    g_large_said = false;
    LOG_INFO(Common, "EmuSuNi: fastmem: the 3DS's memory in 2 MiB pages where it can: {} (3ds-fastmem-2m)",
             g_large ? "on" : "off");
    g_on = false;
''')
patch('src/common/emusuni_fastmem.cpp', r'''bool On() {
    return g_on;
}
''', r'''bool On() {
    return g_on;
}

bool LargePages() {
    return g_large;
}
''')
# Sync: in whole 2 MiB chunks (the groups' runs as before inside each).
patch('src/common/emusuni_fastmem.cpp', r'''    std::lock_guard lock{g_sync};
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    g_syncs++;
    const std::size_t g0 = first_page / kGroupPages;
    const std::size_t g1 = std::min<std::size_t>((std::size_t(first_page) + count + kGroupPages - 1) / kGroupPages,
                                                 kGroups);
''', r'''    std::lock_guard lock{g_sync};
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    g_syncs++;
    std::size_t g0 = first_page / kGroupPages;
    std::size_t g1 = std::min<std::size_t>((std::size_t(first_page) + count + kGroupPages - 1) / kGroupPages,
                                           kGroups);
    // (01.65, 3ds-fastmem-2m) Whole 2 MiB chunks of the guest's (128 groups): a chunk whose groups all lie in order
    // in direct memory from a 2 MiB-aligned offset is one 2 MiB page; any other group by group, as before.
    constexpr std::size_t kChunk = kLarge / kGroup;
    if (g_large) {
        g0 -= g0 % kChunk;
        g1 = std::min<std::size_t>((g1 + kChunk - 1) / kChunk * kChunk, kGroups);
    }
''')
patch('src/common/emusuni_fastmem.cpp', r'''    for (std::size_t group = g0; group < g1; group++) {
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
''', r'''    // What a group should be: the direct memory behind its four pages when they lie in order in one block, from a
    // whole group's offset in it; else nothing.
    auto want_of = [&](std::size_t group) -> std::uint32_t {
        const std::size_t page = group * kGroupPages;
        const std::uint8_t* p = raw[page];
        if (p == nullptr) {
            return 0;
        }
        for (int i = 0; i < block_count; i++) {
            const auto* at = static_cast<const std::uint8_t*>(blocks[i].at);
            if (p < at || p >= at + blocks[i].size) {
                continue;
            }
            const std::size_t offset = std::size_t(p - at);
            if (offset % kGroup == 0 && offset + kGroup <= blocks[i].size && raw[page + 1] == p + kGuestPage &&
                raw[page + 2] == p + 2 * kGuestPage && raw[page + 3] == p + 3 * kGuestPage) {
                return std::uint32_t((std::uint64_t(blocks[i].phys) + offset) / kGroup) + 1;
            }
            return 0;
        }
        return 0;
    };
    // (01.65) A chunk's groups dropped (its 2 MiB page, or what was mapped there, given back whole).
    auto drop_chunk = [&](std::size_t c) {
        UnmapToReserved(arena->base + c * kGroup, kLarge);
        g_unmaps++;
        for (std::size_t i = 0; i < kChunk; i++) {
            std::uint32_t& slot = arena->mapped[c + i];
            arena->groups_mapped -= slot != 0 ? 1 : 0;
            slot = 0;
        }
        arena->big[c / kChunk] = 0;
    };
    // (01.65) A whole chunk of the guest's [c, c + kChunk): one 2 MiB page when its groups all lie in order in direct
    // memory from a 2 MiB-aligned offset (true: it is one, nothing more to do there); else its groups as before (a
    // 2 MiB page there given back whole first). Its groups' wants left in wants[].
    std::uint32_t wants[kChunk];
    std::size_t wants_at = kGroups; // (the chunk wants[] is of; none)
    auto large_chunk = [&](std::size_t c) -> bool {
        const std::size_t chunk = c / kChunk;
        for (std::size_t i = 0; i < kChunk; i++) {
            wants[i] = want_of(c + i);
        }
        wants_at = c;
        bool whole = wants[0] != 0 && (wants[0] - 1) % kChunk == 0 && arena->splits[chunk] < kLargeMaxSplits;
        for (std::size_t i = 1; whole && i < kChunk; i++) {
            whole = wants[i] == wants[0] + std::uint32_t(i);
        }
        if (!whole) {
            if (arena->big[chunk] != 0) {
                drop_chunk(c); // (a 2 MiB page whose groups change: given back whole, then group by group)
                arena->splits[chunk]++;
                g_large_splits++;
            }
            return false;
        }
        if (arena->big[chunk] != 0 && arena->mapped[c] == wants[0]) {
            return true; // (one 2 MiB page already)
        }
        bool any = false;
        for (std::size_t i = 0; i < kChunk && !any; i++) {
            any = arena->mapped[c + i] != 0;
        }
        if (any) {
            drop_chunk(c);
        }
        std::uint8_t* address = arena->base + c * kGroup;
        void* at = address;
        const int32_t r = sceKernelMapDirectMemory(&at, kLarge, Dmem::kProtRW, Dmem::kMapFixed,
                                                   off_t(wants[0] - 1) * off_t(kGroup), kLarge);
        g_maps++;
        if (r == 0 && at == address) {
            for (std::size_t i = 0; i < kChunk; i++) {
                arena->mapped[c + i] = wants[0] + std::uint32_t(i);
            }
            arena->groups_mapped += kChunk;
            arena->big[chunk] = 1;
            g_large_maps++;
            return true;
        }
        if (r == 0) {
            UnmapToReserved(at, kLarge);
        }
        arena->splits[chunk] = kLargeMaxSplits; // (refused: this chunk group by group from now on)
        if (g_large_refused++ < 4) {
            LOG_WARNING(Common, "EmuSuNi: fastmem: a 2 MiB page at guest {:#x} refused ({:#x}): its groups as "
                                "before (3ds-fastmem-2m)", c * kGroup, static_cast<unsigned>(r));
        }
        return false;
    };
    for (std::size_t group = g0; group < g1; group++) {
        if (g_large && group % kChunk == 0 && group + kChunk <= g1) {
            flush(); // (a run never crosses into a chunk)
            if (large_chunk(group)) {
                group += kChunk - 1;
                continue;
            }
        }
        const std::uint32_t want = group - wants_at < kChunk ? wants[group - wants_at] : want_of(group);
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

// (01.65, 3ds-fastmem-2m) Once, at the game's 600th frame: the chunks in 2 MiB pages and what the kernel says.
static void LargeReport() {
    std::lock_guard lock{g_lock};
    std::lock_guard sync{g_sync};
    std::size_t chunks = 0, groups = 0;
    long super = 0;
    bool known = true;
    for (Arena* arena : g_arenas) {
        if (arena == nullptr) {
            continue;
        }
        groups += arena->groups_mapped;
        for (std::size_t chunk = 0; chunk < arena->big.size(); chunk++) {
            if (arena->big[chunk] == 0) {
                continue;
            }
            chunks++;
            const long s = SuperPages(arena->base + chunk * kLarge, kLarge);
            known &= s >= 0;
            super += s > 0 ? s : 0;
        }
    }
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    std::size_t block_bytes = 0;
    long block_super = 0;
    for (int i = 0; i < block_count; i++) {
        block_bytes += blocks[i].size;
        const long s = SuperPages(blocks[i].at, blocks[i].size & ~(Dmem::kPage - 1));
        known &= s >= 0;
        block_super += s > 0 ? s : 0;
    }
    if (known) {
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: 2 MiB pages (3ds-fastmem-2m): {} chunks of the guest's mapped so ({} MiB of the "
                 "{} MiB mapped; the kernel says {} of their {} 16 KiB pages are in 2 MiB pages), {} refused, {} "
                 "split again; the 3DS's memory blocks: {} of {} MiB in 2 MiB pages",
                 chunks, chunks * 2, groups * kGroup / (1024 * 1024), super, chunks * (kLarge / Dmem::kPage),
                 g_large_refused, g_large_splits, block_super * long(Dmem::kPage) / (1024 * 1024),
                 block_bytes / (1024 * 1024));
    } else {
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: 2 MiB pages (3ds-fastmem-2m): {} chunks of the guest's mapped so ({} MiB of the "
                 "{} MiB mapped), {} refused, {} split again (the kernel can't say which pages are large here)",
                 chunks, chunks * 2, groups * kGroup / (1024 * 1024), g_large_refused, g_large_splits);
    }
}
''')
patch('src/common/emusuni_fastmem.cpp', r'''void FrameDone() {
    if (!g_on) {
        return;
    }
    g_frames++;
''', r'''void FrameDone() {
    if (!g_on) {
        return;
    }
    g_frames++;
    if (g_large && !g_large_said && g_frames >= 600) {
        g_large_said = true;
        LargeReport();
    }
''')

# --- 01.66: 3ds-fastmem-2m's diagnosis. The 01.65 console: e2's 600th-frame line said "the kernel can't say" (mincore
# refused somewhere), and the memory probe of the same session found the 3DS memory's 64 MiB at 0x2600000000 with no
# 2 MiB page ("0 of 4096", all resident), though e2 now allocates and maps it 2 MiB aligned; the probe's own direct
# memory, mapped the same way but where the kernel chooses (no address asked for), 64 MiB, had them all. Three things
# differ: the address asked for (0x2600000000), the size (256 MiB), and the fast memory's second mapping of the same
# memory (made after the block, before the probe). One console pass tells which:
# - Each memory block mapped for 2 MiB pages says, right after its mapping and again once all of it is written, how
#   many of its 16 KiB pages the kernel has in 2 MiB pages (mincore's MINCORE_SUPER), or mincore's errno. Both are
#   before the fast memory maps it a second time.
# - The 600th frame's line says the same for the fast memory's 2 MiB chunks and for each block apart, with the errno.
# - The words of /data/EmuSuNi/3ds-fastmem-2m (the PC: EMUSUNI_3DS_FASTMEM_2M), any together: "nohint" maps the blocks
#   where the kernel chooses (2 MiB aligned, as the probe's); "pieces" maps each block bigger than 64 MiB in 64 MiB
#   pieces inside one reservation of its whole length; "off" as before (no 2 MiB pages tried at all).
patch('src/common/emusuni_fastmem.h', r'''#include <cstdint>

namespace EmuSuNi::Fastmem {
''', r'''#include <cstddef>
#include <cstdint>
#include <string>

namespace EmuSuNi::Fastmem {
''')
patch('src/common/emusuni_fastmem.h', r'''bool LargePages();
''', r'''bool LargePages();
// (01.66, 3ds-fastmem-2m's diagnosis) Its words "nohint" (the blocks mapped where the kernel chooses) and "pieces"
// (in 64 MiB pieces inside one reservation).
bool LargeNoHint();
bool LargePieces();
// Of [at, at + length) (16 KiB aligned): the 16 KiB pages the kernel says are in 2 MiB pages (mincore's
// MINCORE_SUPER); -errno when mincore refuses; kCantSay where it can't say (the PC).
constexpr long kCantSay = -1000;
long SuperPages(const void* at, std::size_t length);
// For the log: "n of m", "the kernel can't say", "mincore error e".
std::string SaidPages(long said, std::size_t of);
''')
patch('src/common/emusuni_fastmem.cpp', r'''#include <cctype>
''', r'''#include <cctype>
#include <cerrno>
#include <string>
''')
patch('src/common/emusuni_fastmem.cpp', r'''bool g_large = false;
''', r'''bool g_large = false;
bool g_nohint = false, g_pieces = false; // (01.66) the switch's words
''')
patch('src/common/emusuni_fastmem.cpp', r'''bool LargeSwitch() {
#if defined(__ORBIS__)
    std::FILE* file = std::fopen("/data/EmuSuNi/3ds-fastmem-2m", "rb");
    if (file == nullptr) {
        return true;
    }
    char text[64] = "";
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[length] = '\0';
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return std::strstr(text, "off") == nullptr;
#else
    const char* value = std::getenv("EMUSUNI_3DS_FASTMEM_2M");
    return value == nullptr || (std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0);
#endif
}
''', r'''bool LargeSwitch() {
    // (01.66) The words: "off" (as before e2), "nohint", "pieces" (the diagnosis); the PC: "0" is off too.
    std::string words;
#if defined(__ORBIS__)
    if (std::FILE* file = std::fopen("/data/EmuSuNi/3ds-fastmem-2m", "rb")) {
        char text[128] = "";
        const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
        std::fclose(file);
        words.assign(text, length);
    }
#else
    if (const char* value = std::getenv("EMUSUNI_3DS_FASTMEM_2M")) {
        words = value;
    }
#endif
    for (char& c : words) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    g_nohint = words.find("nohint") != std::string::npos;
    g_pieces = words.find("pieces") != std::string::npos;
    return words.find("off") == std::string::npos && words != "0";
}
''')
# SuperPages out of the unnamed namespace (core/memory's blocks ask it too), with mincore's errno.
patch('src/common/emusuni_fastmem.cpp', r'''// Of [at, at + length) (16 KiB aligned): the 16 KiB pages the kernel says are in 2 MiB pages (mincore's
// MINCORE_SUPER, 0x20); -1 where it can't say (the PC).
long SuperPages(const void* at, std::size_t length) {
#if defined(__ORBIS__)
    std::vector<char> vec(length / Dmem::kPage + 16, 0);
    if (emusuni_fastmem_mincore(at, length, vec.data()) != 0) {
        return -1;
    }
    long super = 0;
    for (std::size_t i = 0; i < length / Dmem::kPage; i++) {
        super += (vec[i] & 0x20) ? 1 : 0;
    }
    return super;
#else
    (void)at;
    (void)length;
    return -1;
#endif
}
} // namespace
''', r'''} // namespace

// (01.66) Of [at, at + length) (16 KiB aligned): the 16 KiB pages the kernel says are in 2 MiB pages (mincore's
// MINCORE_SUPER, 0x20); -errno when mincore refuses (-1 without one); kCantSay where it can't say (the PC).
long SuperPages(const void* at, std::size_t length) {
#if defined(__ORBIS__)
    std::vector<char> vec(length / Dmem::kPage + 16, 0);
    errno = 0;
    if (emusuni_fastmem_mincore(at, length, vec.data()) != 0) {
        return errno > 0 ? -static_cast<long>(errno) : -1;
    }
    long super = 0;
    for (std::size_t i = 0; i < length / Dmem::kPage; i++) {
        super += (vec[i] & 0x20) ? 1 : 0;
    }
    return super;
#else
    (void)at;
    (void)length;
    return kCantSay;
#endif
}

// (01.66) What SuperPages said, for the log: "n of m", "the kernel can't say", "mincore error e".
std::string SaidPages(long said, std::size_t of) {
    if (said >= 0) {
        return std::to_string(said) + " of " + std::to_string(of);
    }
    if (said == kCantSay) {
        return "the kernel can't say";
    }
    return "mincore error " + std::to_string(-said);
}
''')
patch('src/common/emusuni_fastmem.cpp', r'''bool LargePages() {
    return g_large;
}
''', r'''bool LargePages() {
    return g_large;
}

bool LargeNoHint() {
    return g_large && g_nohint;
}

bool LargePieces() {
    return g_large && g_pieces;
}
''')
patch('src/common/emusuni_fastmem.cpp', r'''    std::size_t chunks = 0, groups = 0;
    long super = 0;
    bool known = true;
    for (Arena* arena : g_arenas) {
        if (arena == nullptr) {
            continue;
        }
        groups += arena->groups_mapped;
        for (std::size_t chunk = 0; chunk < arena->big.size(); chunk++) {
            if (arena->big[chunk] == 0) {
                continue;
            }
            chunks++;
            const long s = SuperPages(arena->base + chunk * kLarge, kLarge);
            known &= s >= 0;
            super += s > 0 ? s : 0;
        }
    }
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    std::size_t block_bytes = 0;
    long block_super = 0;
    for (int i = 0; i < block_count; i++) {
        block_bytes += blocks[i].size;
        const long s = SuperPages(blocks[i].at, blocks[i].size & ~(Dmem::kPage - 1));
        known &= s >= 0;
        block_super += s > 0 ? s : 0;
    }
    if (known) {
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: 2 MiB pages (3ds-fastmem-2m): {} chunks of the guest's mapped so ({} MiB of the "
                 "{} MiB mapped; the kernel says {} of their {} 16 KiB pages are in 2 MiB pages), {} refused, {} "
                 "split again; the 3DS's memory blocks: {} of {} MiB in 2 MiB pages",
                 chunks, chunks * 2, groups * kGroup / (1024 * 1024), super, chunks * (kLarge / Dmem::kPage),
                 g_large_refused, g_large_splits, block_super * long(Dmem::kPage) / (1024 * 1024),
                 block_bytes / (1024 * 1024));
    } else {
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: 2 MiB pages (3ds-fastmem-2m): {} chunks of the guest's mapped so ({} MiB of the "
                 "{} MiB mapped), {} refused, {} split again (the kernel can't say which pages are large here)",
                 chunks, chunks * 2, groups * kGroup / (1024 * 1024), g_large_refused, g_large_splits);
    }
}
''', r'''    // (01.66) The fast memory's chunks and each memory block apart, with mincore's errno where it refuses.
    std::size_t chunks = 0, groups = 0;
    long super = 0, chunk_error = 0;
    for (Arena* arena : g_arenas) {
        if (arena == nullptr) {
            continue;
        }
        groups += arena->groups_mapped;
        for (std::size_t chunk = 0; chunk < arena->big.size(); chunk++) {
            if (arena->big[chunk] == 0) {
                continue;
            }
            chunks++;
            const long s = SuperPages(arena->base + chunk * kLarge, kLarge);
            if (s >= 0) {
                super += s;
            } else if (chunk_error == 0) {
                chunk_error = s;
            }
        }
    }
    const std::string in_chunks = chunks == 0 ? std::string("none mapped so")
                                  : chunk_error != 0 ? SaidPages(chunk_error, 0)
                                                     : SaidPages(super, chunks * (kLarge / Dmem::kPage));
    DirectBlock blocks[16];
    const int block_count = PagesDirectBlocks(blocks, 16);
    std::string in_blocks;
    for (int i = 0; i < block_count; i++) {
        const std::size_t length = blocks[i].size & ~(Dmem::kPage - 1);
        in_blocks += (i ? "; " : "") + std::to_string(blocks[i].size / 1024) + " KiB: " +
                     SaidPages(SuperPages(blocks[i].at, length), length / Dmem::kPage);
    }
    LOG_INFO(Common,
             "EmuSuNi: fastmem: 2 MiB pages (3ds-fastmem-2m{}{}): {} chunks of the guest's mapped so ({} MiB of the {} "
             "MiB mapped), {} refused, {} split again; the kernel's 2 MiB pages (16 KiB pages): in those chunks {}; "
             "in the memory blocks {}",
             g_nohint ? ", nohint" : "", g_pieces ? ", pieces" : "", chunks, chunks * 2,
             groups * kGroup / (1024 * 1024), g_large_refused, g_large_splits, in_chunks,
             in_blocks.empty() ? std::string("none") : in_blocks);
}
''')
# core/memory's blocks (common/emusuni_mem.cpp): "nohint", "pieces", and what the kernel says right after.
patch('src/common/emusuni_mem.cpp', r'''#include <cstdint>
#include <cstring>
''', r'''#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
''')
patch('src/common/emusuni_mem.cpp', r'''constexpr size_t LARGE = 2 * 1024 * 1024; // (01.65, 3ds-fastmem-2m) the kernel's large page
''', r'''constexpr size_t LARGE = 2 * 1024 * 1024; // (01.65, 3ds-fastmem-2m) the kernel's large page
constexpr size_t PIECE = 64 * 1024 * 1024; // (01.66, its "pieces") the memory probe's size
''')
patch('src/common/emusuni_mem.cpp', r'''void* PagesAlloc(size_t size) {
''', r'''namespace {
// (01.66, 3ds-fastmem-2m's "pieces") A block mapped in 64 MiB pieces (the memory probe's size) inside one
// reservation of its whole length, 2 MiB aligned, at *base when that is free, else where the kernel chooses.
int32_t MapPieces(void** base, size_t length, off_t phys) {
    void* at = *base;
    int32_t ret = sceKernelReserveVirtualRange(&at, length, 0, LARGE);
    if (ret != 0 && *base != nullptr) {
        at = nullptr;
        ret = sceKernelReserveVirtualRange(&at, length, 0, LARGE);
    }
    if (ret != 0) {
        return ret;
    }
    for (size_t done = 0; done < length; done += PIECE) {
        void* piece = static_cast<char*>(at) + done;
        const size_t n = std::min(PIECE, length - done);
        ret = sceKernelMapDirectMemory(&piece, n, 0x03, Dmem::kMapFixed, phys + static_cast<off_t>(done), LARGE);
        if (ret != 0 || piece != static_cast<char*>(at) + done) {
            sceKernelMunmap(at, length);
            return ret != 0 ? ret : -1;
        }
    }
    *base = at;
    return 0;
}
} // namespace

void* PagesAlloc(size_t size) {
''')
patch('src/common/emusuni_mem.cpp', r'''    bool large_pages = false;
''', r'''    bool large_pages = false;
    long super_mapped = Fastmem::kCantSay, super_written = Fastmem::kCantSay; // (01.66) the kernel's 2 MiB pages
''')
patch('src/common/emusuni_mem.cpp', r'''        // A hint well above 0x200000000, where the kernel library puts its own objects.
        void* base = reinterpret_cast<void*>(0x2600000000ull);
        const int32_t ret = sceKernelMapDirectMemory(&base, length, PROT_CPU_RW, 0, phys, large ? LARGE : PAGE);
        if (ret == 0 && base != nullptr) {
            at = base;
            size = length;
            kind = Kind::Direct;
            large_pages = large;
            std::memset(at, 0, size); // direct memory comes as it was left
        } else {
''', r'''        // A hint well above 0x200000000, where the kernel library puts its own objects. (01.66, 3ds-fastmem-2m's
        // diagnosis) "nohint": none, where the kernel chooses (2 MiB aligned, as the memory probe's own); "pieces":
        // in 64 MiB pieces inside one reservation.
        void* base = large && Fastmem::LargeNoHint() ? nullptr : reinterpret_cast<void*>(0x2600000000ull);
        const int32_t ret = large && Fastmem::LargePieces() && length > PIECE
                                ? MapPieces(&base, length, phys)
                                : sceKernelMapDirectMemory(&base, length, PROT_CPU_RW, 0, phys, large ? LARGE : PAGE);
        if (ret == 0 && base != nullptr) {
            at = base;
            size = length;
            kind = Kind::Direct;
            large_pages = large;
            if (large) {
                super_mapped = Fastmem::SuperPages(at, size); // (01.66) just mapped
            }
            std::memset(at, 0, size); // direct memory comes as it was left
            if (large) {
                super_written = Fastmem::SuperPages(at, size); // (01.66) every page of it met once
            }
        } else {
''')
patch('src/common/emusuni_mem.cpp', r'''             : large_pages         ? "direct memory, 2 MiB aligned (3ds-fastmem-2m)"
                                   : "direct memory",
             at);
''', r'''             : large_pages         ? "direct memory, 2 MiB aligned (3ds-fastmem-2m)"
                                   : "direct memory",
             at);
    if (large_pages) {
        // (01.66) What the kernel says of it before the fast memory maps it a second time.
        LOG_INFO(Common,
                 "EmuSuNi: fastmem: that block's 2 MiB pages by the kernel (3ds-fastmem-2m{}{}; 16 KiB pages): "
                 "{} just mapped, {} once written",
                 Fastmem::LargeNoHint() ? ", nohint" : "", Fastmem::LargePieces() ? ", pieces" : "",
                 Fastmem::SaidPages(super_mapped, size / PAGE), Fastmem::SaidPages(super_written, size / PAGE));
    }
''')
