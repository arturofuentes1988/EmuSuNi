// Replacement for OpenOrbis' lib/crt1.o: the same entry point and process parameters (same
// section names, sizes and values, rebuilt from a disassembly of the original), except that the
// libc malloc-replacement table is filled in.
//
// Why: every malloc of the process - Sony's system modules included - is served by the system
// libc heap, which is only ~12.7 MiB for this app (measured with
// sceKernelInternalMemoryGetAvailableSize, and declaring a larger sceLibcHeapSize had no effect).
// Piglet's program links, shader compiles and command buffers exhausted it within minutes and the
// app crashed ("Internal Memory is running out"). With the table filled, the system libc forwards
// every malloc/free/... to ps4_replace_* (port/ps4_runtime.cpp), i.e. to our dlmalloc heap in
// system flexible memory.
//
// SceLibcMallocReplace (version 1, 0x70 bytes): size, version, init, finalize, malloc, free,
// calloc, realloc, memalign, reallocalign, posix_memalign, malloc_stats, malloc_stats_fast,
// malloc_usable_size. Found through _sceLibcParam + 0x30.

// (01.27) PS4_RELR (toolchain/ps4-love-style.cmake): the program's own data pointers relocated by the program
// (port/ps4_relr.c) before anything reads one - first thing in _start, and in the malloc-replacement entries Sony's
// libc calls before _start (each through a _relr entry that relocates first, once).
#ifdef PS4_RELR
#define REPLACE(f) f##_relr
#else
#define REPLACE(f) f
#endif

    .text
    .globl _start
    .type _start, @function
    .p2align 3
_start:
#ifdef PS4_RELR
    pushq %rbp                              // the loader's arguments kept, the stack aligned for the call
    movq %rsp, %rbp
    pushq %rdi
    pushq %rsi
    andq $-16, %rsp
    call ps4_relr_apply
    leaq -16(%rbp), %rsp
    popq %rsi
    popq %rdi
    popq %rbp
#endif
    jmp _start_ps4_c
    .p2align 3

#ifdef PS4_RELR
    // <f>_relr: relocate first (once: ps4_relr_done), then <f> with the same arguments.
    .macro RELR_FIRST f
    .section .text.\f\()_relr, "ax", @progbits
    .type \f\()_relr, @function
\f\()_relr:
    cmpb $0, ps4_relr_done(%rip)
    je 1f
    jmp \f
1:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    andq $-16, %rsp
    call ps4_relr_apply
    leaq -32(%rbp), %rsp
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    jmp \f
    .endm
    RELR_FIRST ps4_replace_malloc_init
    RELR_FIRST ps4_replace_malloc_finalize
    RELR_FIRST ps4_replace_malloc
    RELR_FIRST ps4_replace_free
    RELR_FIRST ps4_replace_calloc
    RELR_FIRST ps4_replace_realloc
    RELR_FIRST ps4_replace_memalign
    RELR_FIRST ps4_replace_reallocalign
    RELR_FIRST ps4_replace_posix_memalign
    RELR_FIRST ps4_replace_malloc_stats
    RELR_FIRST ps4_replace_malloc_stats_fast
    RELR_FIRST ps4_replace_malloc_usable_size
#endif

    .section .text._start_ps4_c, "ax", @progbits
    .globl _start_ps4_c
    .type _start_ps4_c, @function
_start_ps4_c:
    movl (%rdi), %esi
    leaq 8(%rdi), %rdx
    movq main@GOTPCREL(%rip), %rdi
    movq _init@GOTPCREL(%rip), %rcx
    movq _fini@GOTPCREL(%rip), %r8
    xorl %r9d, %r9d
    jmp __libc_start_main@PLT

    .weak _init
    .weak _fini

    .section .data.rel.ro._sceLibcParam, "aw", @progbits
    .p2align 3
_sceLibcParam:
    .quad 0x90                              // size
    .long 0x0c                              // entry count
    .long 1
    .quad sceLibcHeapSize
    .quad 0
    .quad sceLibcHeapExtendedAlloc
    .quad 0
    .quad _sceLibcMallocReplace             // + 0x30
    .quad _sceLibcNewReplace                // + 0x38
    .quad 0, 0, 0, 0
    .quad _sceLibcMallocReplaceForTls       // + 0x60
    .zero 0x90 - 0x68
    // (01.27) Each table's size in the symbol table: toolchain/relr-link.py leaves their pointers to the loader.
    .size _sceLibcParam, . - _sceLibcParam

    .section .data.rel.ro._sceKernelMemParam, "aw", @progbits
    .p2align 3
_sceKernelMemParam:
    .quad 0x30
    .zero 0x28
    .size _sceKernelMemParam, . - _sceKernelMemParam

    .section .data.rel.ro._sceKernelFsParam, "aw", @progbits
    .p2align 3
_sceKernelFsParam:
    .quad 0x10
    .quad 0
    .size _sceKernelFsParam, . - _sceKernelFsParam

    .section .data.rel.ro._sceLibcMallocReplace, "aw", @progbits
    .p2align 3
_sceLibcMallocReplace:
    .quad 0x70                              // size
    .quad 1                                 // version
    .quad REPLACE(ps4_replace_malloc_init)
    .quad REPLACE(ps4_replace_malloc_finalize)
    .quad REPLACE(ps4_replace_malloc)
    .quad REPLACE(ps4_replace_free)
    .quad REPLACE(ps4_replace_calloc)
    .quad REPLACE(ps4_replace_realloc)
    .quad REPLACE(ps4_replace_memalign)
    .quad REPLACE(ps4_replace_reallocalign)
    .quad REPLACE(ps4_replace_posix_memalign)
    .quad REPLACE(ps4_replace_malloc_stats)
    .quad REPLACE(ps4_replace_malloc_stats_fast)
    .quad REPLACE(ps4_replace_malloc_usable_size)
    .size _sceLibcMallocReplace, . - _sceLibcMallocReplace

    .section .data.rel.ro._sceLibcNewReplace, "aw", @progbits
    .p2align 3
_sceLibcNewReplace:
    .quad 0x70
    .quad 2
    .zero 0x60
    .size _sceLibcNewReplace, . - _sceLibcNewReplace

    .section .data.rel.ro._sceLibcMallocReplaceForTls, "aw", @progbits
    .p2align 3
_sceLibcMallocReplaceForTls:
    .quad 0x38
    .quad 1
    .zero 0x28
    .size _sceLibcMallocReplaceForTls, . - _sceLibcMallocReplaceForTls

    .section .data.sce_process_param, "aw", @progbits
    .p2align 3
_sceProcessParam:
    .quad 0x50                              // size
    .long 0x4942524f                        // "ORBI"
    .long 3                                 // entry count
    .quad 0x04508101                        // SDK version
    .zero 0x38 - 0x18
    .quad _sceLibcParam
    .quad _sceKernelMemParam
    .quad _sceKernelFsParam

    .data
sceLibcHeapExtendedAlloc:
    .long 1
    .long 0
sceLibcHeapSize:
    .quad 0xffffffffffffffff
sce_libc_heap_delayed_alloc:
    .long 0
sce_libc_heap_extended_alloc:
    .long 0
__dso_handle:
    .quad 0
_sceLibc:
    .quad 0

    .globl __ps4Argv
    .section .bss.__ps4Argv, "aw", @nobits
    .p2align 4
__ps4Argv:
    .zero 0x20

    .section .note.GNU-stack, "", @progbits
