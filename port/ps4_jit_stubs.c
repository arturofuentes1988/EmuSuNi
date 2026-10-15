// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// port/ps4_runtime.cpp (from DolphinPS4) can map executable memory for Dolphin's JIT through
// libkernel's JIT shared memory. The 2022 PacBrew OpenOrbis stubs don't export those functions,
// and Snes9x has no JIT (ps4_exec_probe() is never called), so these only satisfy the linker.

#include <stddef.h>
#include <stdint.h>

int32_t sceKernelJitCreateSharedMemory(const char* name, size_t len, int32_t max_prot, int32_t* fd)
{
  (void)name;
  (void)len;
  (void)max_prot;
  if (fd)
    *fd = -1;
  return (int32_t)0x80020016;  // EINVAL
}

int32_t sceKernelJitCreateAliasOfSharedMemory(int32_t fd, int32_t max_prot, int32_t* alias_fd)
{
  (void)fd;
  (void)max_prot;
  if (alias_fd)
    *alias_fd = -1;
  return (int32_t)0x80020016;  // EINVAL
}
