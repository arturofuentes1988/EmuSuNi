/* dlmalloc 2.8.6 (Doug Lea, MIT-0) configured as the PS4 app heap: only the mspace API (no
 * global malloc symbols, so nothing clashes with the OpenOrbis libc), over a memory block we
 * map ourselves (no mmap/sbrk), with locks and footers. Corruption and bad frees are reported
 * through ps4_heap_error() in ps4_runtime.cpp with the operation and caller that hit them.
 *
 * Replaces Sony's sceLibcMspace, which stopped returning memory after ~800 small allocations
 * in Dolphin's static initialisation with 49 KB in use on a 1 GiB heap. */

#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define FOOTERS 1
#define INSECURE 0
#define USE_DEV_RANDOM 0
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
/* The PS4 page size; sysconf numbering differs between headers, so don't ask. */
#define malloc_getpagesize ((size_t)16384U)
#define DEFAULT_GRANULARITY ((size_t)64U * (size_t)1024U)

/* The heap's lock: a blocking mutex, not dlmalloc's spin lock. The PS4 runs the app's threads by
 * fixed priorities, and sched_yield() only lets threads of the same or a higher priority run: a
 * thread spinning on the spin lock kept a preempted lower-priority holder from ever running again,
 * and every thread that allocated then spun too - the whole app froze (PPSSPP compiling a large
 * pipeline cache at a game's start: its pool threads at 600 spinning, a shader compile thread at
 * 700 holding the lock). A waiter that sleeps lets the holder finish. */
#include <pthread.h>
#define USE_LOCKS 2
#define MLOCK_T pthread_mutex_t
#define INITIAL_LOCK(lk) pthread_mutex_init((lk), NULL)
#define DESTROY_LOCK(lk) pthread_mutex_destroy(lk)
#define ACQUIRE_LOCK(lk) pthread_mutex_lock(lk)
#define RELEASE_LOCK(lk) pthread_mutex_unlock(lk)
#define TRY_LOCK(lk) (pthread_mutex_trylock(lk) == 0)
static MLOCK_T malloc_global_mutex = PTHREAD_MUTEX_INITIALIZER;

void ps4_heap_error(void* mspace, void* chunk, int corruption);
#define CORRUPTION_ERROR_ACTION(m) ps4_heap_error((m), 0, 1)
#define USAGE_ERROR_ACTION(m, p) ps4_heap_error((m), (p), 0)

#include "third_party/dlmalloc/malloc.c"

/* Integrity check for diagnostics: is the mspace's state still intact? */
int ps4_heap_ok(void* msp) {
  mstate ms = (mstate)msp;
  return ok_magic(ms);
}
