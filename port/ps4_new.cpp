// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
//
// C++ operator new and delete on the app's heap (dlmalloc, port/ps4_runtime.cpp).
//
// The sysroot's libc++ and libc++abi are built without them, so the program imported the system
// C library's versions (libSceLibcInternal). Those serve every C++ allocation from the system
// libc heap - the ~12.7 MiB pool that sceKernelInternalMemoryGetAvailableSize reports and that
// libkernel's own objects (threads, mutexes, memory mappings) draw from too; the malloc
// replacement table (port/ps4_crt1.S) doesn't reach them. Starting a game (a 4 MiB copy of the
// ROM, Snes9x's 8 MiB ROM buffer) ran that pool out and the system closed the app ("Internal
// Memory is running out", error CE-34878-0).

#include <cstddef>
#include <cstdlib>
#include <new>

extern "C" {
void* __wrap_malloc(size_t size);
void __wrap_free(void* ptr);
void* __wrap_memalign(size_t alignment, size_t size);
}

namespace
{
constexpr size_t MIN_ALIGNMENT = 16;  // what dlmalloc's malloc gives

[[noreturn]] void OutOfMemory()
{
#if defined(__cpp_exceptions)
  throw std::bad_alloc();
#else
  abort();
#endif
}

void* Allocate(size_t size)
{
  return __wrap_malloc(size ? size : 1);
}

void* AllocateAligned(size_t size, std::align_val_t alignment)
{
  const size_t align = static_cast<size_t>(alignment);
  if (align <= MIN_ALIGNMENT)
    return Allocate(size);
  return __wrap_memalign(align, size ? size : 1);
}
}  // namespace

void* operator new(std::size_t size)
{
  void* p = Allocate(size);
  if (!p)
    OutOfMemory();
  return p;
}

void* operator new[](std::size_t size)
{
  void* p = Allocate(size);
  if (!p)
    OutOfMemory();
  return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  return Allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  return Allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  void* p = AllocateAligned(size, alignment);
  if (!p)
    OutOfMemory();
  return p;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  void* p = AllocateAligned(size, alignment);
  if (!p)
    OutOfMemory();
  return p;
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return AllocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  return AllocateAligned(size, alignment);
}

void operator delete(void* ptr) noexcept
{
  __wrap_free(ptr);
}

void operator delete[](void* ptr) noexcept
{
  __wrap_free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
  __wrap_free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
  __wrap_free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
  __wrap_free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
  __wrap_free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
  __wrap_free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept
{
  __wrap_free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept
{
  __wrap_free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept
{
  __wrap_free(ptr);
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept
{
  __wrap_free(ptr);
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept
{
  __wrap_free(ptr);
}
