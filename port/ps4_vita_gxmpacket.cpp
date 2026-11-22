// EmuSuNi's PS Vita port (19-11, the Vita specialist's; step 2 of the GPU library's draw path): the switch "gxmpacket"
// (50 of the port's table, ps4_vita_sys.h; OFF) - the bytes of the packets that carry a draw's commands to the renderer
// (the kernel's side: scripts/build-vita-core.sh, renderer.cpp's EmusuniPacket, scene.cpp's handler of the packet).
//
// Why: a draw of an immediate context was 8-15 commands (its uniform buffers, its programs after a precomputed draw,
// its textures, its vertex streams, the draw), each one taken from a ring, filled, linked, then looked up twice and
// freed one by one in the renderer's thread, which gave each back with an atomic add on a line the game's thread reads
// at every command (the console: 8.6 microseconds a draw in Uncharted's main thread, 01.78; 19 times the PC's). With
// the switch the same records, in the same order and with the same arguments, are written into one packet of a ring
// of the emulator's own, and the draw is one command that points at it; the renderer runs each record with the very
// handler of the command it stands for. The draw's highest index is found in the renderer (from the indices it copies
// anyway), not by a pass over all of them in the game's thread; and the frees are given back every 64 commands (64 KiB
// of packets) and at the end of each list, not one by one.
//
// A packet: 4 bytes "taken" (this file's: its bytes in the ring, those skipped at the ring's end before it included),
// 4 bytes its records' count, then the records (renderer/include/renderer/commands.h: EmusuniPacketRecord). The
// ring is freed in the order it was taken, as the game's own command ring: a count of bytes given back.
// Full (the renderer far behind) or a draw with more records than a packet holds: the commands as ever.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps4_vita_sys.h"

namespace {

constexpr uint64_t kRingBytes = uint64_t(2) << 20;  // (Uncharted: ~19,000 draws a second of ~200-400 bytes, the renderer up to ~50 ms behind)
constexpr uint64_t kGiveBytes = uint64_t(64) << 10;  // (freed bytes given back at once)
constexpr int kRings = 4;

struct Ring {
    std::atomic<const void*> ctx{nullptr};
    unsigned char* base = nullptr;
    uint64_t head = 0;       // (the context's thread) bytes taken so far
    uint64_t open_head = 0;  // (the context's thread) head when the open packet was opened
    uint64_t open_skip = 0;  // (the context's thread) bytes skipped at the ring's end for it
    alignas(64) std::atomic<uint64_t> freed{0};  // (the renderer's thread) bytes given back so far
};
alignas(64) Ring s_rings[kRings];

struct Kept {
    Ring* ring = nullptr;
    uint64_t bytes = 0;
};
thread_local Kept t_kept;  // freed and not yet given back (each thread its own: the renderer's, and a context's at its end)

std::atomic<uint64_t> s_count[3];  // as one packet, the old way (ring full), the old way (too many records)
std::atomic<uint64_t> s_bytes{0};
std::atomic<int> s_said{0};

uint64_t Round16(uint64_t n) {
    return (n + 15) & ~uint64_t(15);
}

Ring* RingOf(const void* ctx) {
    for (Ring& r : s_rings) {
        if (r.ctx.load(std::memory_order_acquire) == ctx) {
            return r.base != nullptr ? &r : nullptr;
        }
    }
    for (Ring& r : s_rings) {
        const void* none = nullptr;
        if (r.ctx.load(std::memory_order_relaxed) == nullptr && r.ctx.compare_exchange_strong(none, ctx)) {
            void* const memory = aligned_alloc(64, kRingBytes);
            if (memory == nullptr) {
                return nullptr;  // (the slot stays this context's, without a ring: the old way)
            }
            r.base = static_cast<unsigned char*>(memory);
            emusuni_vita_log("vita: gxmpacket: a ring of %llu KiB for the draws of the context at %p",
                             static_cast<unsigned long long>(kRingBytes / 1024), ctx);
            return &r;
        }
    }
    return nullptr;  // (more contexts than rings: the old way)
}

Ring* RingHolding(const void* at) {
    const unsigned char* const p = static_cast<const unsigned char*>(at);
    for (Ring& r : s_rings) {
        if (r.base != nullptr && p >= r.base && p < r.base + kRingBytes) {
            return &r;
        }
    }
    return nullptr;
}

void Give() {
    if (t_kept.ring != nullptr && t_kept.bytes != 0) {
        t_kept.ring->freed.fetch_add(t_kept.bytes, std::memory_order_release);
    }
    t_kept.bytes = 0;
}

}  // namespace

// Room for a packet of up to `bytes` for this immediate context (the context's thread), or nullptr: the old way.
extern "C" void* emusuni_vita_packet_open(const void* ctx, size_t bytes) {
    if (s_said.exchange(1, std::memory_order_relaxed) == 0) {
        emusuni_vita_log("vita: gxmpacket: a draw's commands to the renderer as one packet (the same records, in the same "
                         "order), its highest index found in the renderer, the frees given back every 64 commands and at "
                         "each list's end: on (the file vita-gxmpacket-on)");
    }
    Ring* const r = RingOf(ctx);
    if (r == nullptr) {
        return nullptr;
    }
    const uint64_t want = Round16(bytes);
    if (want > kRingBytes / 4) {
        return nullptr;
    }
    const uint64_t at = r->head % kRingBytes;
    const uint64_t skip = at + want > kRingBytes ? kRingBytes - at : 0;
    if (r->head + skip + want - r->freed.load(std::memory_order_acquire) > kRingBytes) {
        return nullptr;
    }
    r->open_head = r->head;
    r->open_skip = skip;
    return r->base + (at + skip) % kRingBytes;
}

// The open packet written: `used` bytes of it (0: nothing - its room goes back as it was).
extern "C" void emusuni_vita_packet_close(void* packet, size_t used) {
    Ring* const r = RingHolding(packet);
    if (r == nullptr || used == 0) {
        return;
    }
    const uint64_t taken = r->open_skip + Round16(used);
    *static_cast<uint32_t*>(packet) = static_cast<uint32_t>(taken);
    r->head = r->open_head + taken;
    s_bytes.fetch_add(Round16(used), std::memory_order_relaxed);
}

// A packet run (the renderer's thread) or thrown away (the context's thread, at its end): its bytes kept to be given back.
extern "C" void emusuni_vita_packet_free(const void* packet) {
    Ring* const r = RingHolding(packet);
    if (r == nullptr) {
        return;
    }
    if (t_kept.ring != r) {
        Give();
        t_kept.ring = r;
    }
    t_kept.bytes += *static_cast<const uint32_t*>(packet);
    if (t_kept.bytes >= kGiveBytes) {
        Give();
    }
}

// The bytes this thread kept back given now (the end of a list).
extern "C" void emusuni_vita_packet_publish(void) {
    Give();
}

// A draw: 0 as one packet, 1 the old way (the ring full), 2 the old way (more records than a packet holds).
extern "C" void emusuni_vita_packet_count(int how) {
    if (how < 0 || how > 2) {
        return;
    }
    s_count[how].fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint64_t> total{0};
    if ((total.fetch_add(1, std::memory_order_relaxed) & 0x3FFFF) == 0x3FFFF) {
        const uint64_t one = s_count[0].load(), full = s_count[1].load(), big = s_count[2].load();
        emusuni_vita_log("vita: gxmpacket: %llu draws so far: %llu as one packet (%llu bytes on average), %llu the old way "
                         "with the packet ring full, %llu with more records than a packet holds",
                         static_cast<unsigned long long>(one + full + big), static_cast<unsigned long long>(one),
                         static_cast<unsigned long long>(one ? s_bytes.load() / one : 0), static_cast<unsigned long long>(full),
                         static_cast<unsigned long long>(big));
    }
}
