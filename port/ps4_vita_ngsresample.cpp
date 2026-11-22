// EmuSuNi's PS Vita port (18-11, the performance specialist's, taken in with the set 0177): the NGS voices' own
// resampler, the switch "ngsresample" (the kernel's side: scripts/build-vita-core.sh, vita3k/ngs/src/rate_resampler.cpp,
// player.cpp, atrac9.cpp and ngs.cpp).
//
// Why: MotorStorm RC's "NGSynth Update Thread" is 79-88% of a PS4 core (01.71, 01.75: sceNgsSystemUpdate 11 ms a
// call). Vita3K takes every voice that isn't at 48 kHz through an FFmpeg resampler of its own (swr: 32 products a
// sample and channel, twice over - its coefficients interpolated between phases -, plain C on the PS4, whose FFmpeg has
// no assembly), and makes that resampler anew - its table of 1025 x 32 coefficients, each with a Bessel function and a
// sine - every time a voice's pitch changes: an engine's sound, every update. On the PC bench (MotorStorm's tutorial)
// that making is a quarter of the thread, the resampling another quarter.
//
// What: one short polyphase filter for every voice (16 taps, 1024 phases, a Kaiser-windowed sinc; its cutoff narrower
// for a voice played faster than 48 kHz, in 9 steps), its tables made once; a voice's position and its last 15 frames
// kept across updates, so a new pitch is only a new step - no new table, no reset, no click. 16 products a sample and
// channel.
//
// Switch: "ngsresample" of the port's table (ps4_vita_sys.h; 36, OFF): the empty file
// /data/EmuSuNi/vita-ngsresample-on (the PC: EMUSUNI_VITA_NGSRESAMPLE=1), read at each game's start (ngs::init); a
// line "vita: ngs: ..." in the log says which.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unistd.h>
#include <vector>

#if !defined(EMUSUNI_NGSRS_TEST)
#include "ps4_vita_sys.h"
#endif

namespace {

constexpr int kTaps = 16;                // input frames under one output frame: i-7 .. i+8
constexpr int kBefore = 7;
constexpr int kPhases = 1024;            // a frame's fractions (the nearest is taken: at most 1/2048 of a frame off)
constexpr int kHold = kTaps - 1;         // frames kept for the next call
constexpr double kCutoff = 0.90;         // of the lower of the two Nyquist frequencies
constexpr double kBeta = 5.0;            // the Kaiser window's
// (A voice played faster than the output: the filter's cutoff goes down with it, so that what the output can't carry
// isn't folded back. Steps: the first that isn't below the voice's.)
constexpr double kBuckets[] = {1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0};
constexpr int kBucketCount = static_cast<int>(sizeof(kBuckets) / sizeof(kBuckets[0]));

// The state of one voice, kept by Vita3K in the voice's runtime state (opaque bytes there: kStateBytes).
struct State {
    double pos;            // the next output frame's place, in frames from hist[0]
    uint32_t held;         // frames in hist
    uint32_t magic;
    float hist[2 * kHold];
};
constexpr uint32_t kMagic = 0x4E475352;  // "NGSR"

struct Table {
    std::once_flag made;
    std::vector<float> coef;  // (kPhases + 1) rows of 2 x kTaps: each coefficient twice, for L and R side by side
};
Table s_tables[kBucketCount];

double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; k++) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-12) {
            break;
        }
    }
    return sum;
}

const float* TableFor(int bucket) {
    Table& t = s_tables[bucket];
    std::call_once(t.made, [&t, bucket] {
        const double c = kCutoff / kBuckets[bucket];  // in cycles per input frame x2 (1.0 = the input's Nyquist)
        const double half = kTaps / 2.0;
        const double norm = BesselI0(kBeta);
        t.coef.resize(static_cast<size_t>(kPhases + 1) * kTaps * 2);
        for (int p = 0; p <= kPhases; p++) {
            const double frac = static_cast<double>(p) / kPhases;
            double row[kTaps], sum = 0.0;
            for (int k = 0; k < kTaps; k++) {
                const double d = (k - kBefore) - frac;  // the tap's distance from the output's place, in frames
                const double x = M_PI * c * d;
                const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
                const double w2 = 1.0 - (d / half) * (d / half);
                const double window = w2 <= 0.0 ? 0.0 : BesselI0(kBeta * std::sqrt(w2)) / norm;
                row[k] = c * sinc * window;
                sum += row[k];
            }
            for (int k = 0; k < kTaps; k++) {  // (a gain of 1 for every phase: no ripple from the phase)
                const float v = static_cast<float>(row[k] / sum);
                t.coef[(static_cast<size_t>(p) * kTaps + k) * 2] = v;
                t.coef[(static_cast<size_t>(p) * kTaps + k) * 2 + 1] = v;
            }
        }
    });
    return t.coef.data();
}

int BucketFor(double step) {
    for (int b = 0; b < kBucketCount; b++) {
        if (step <= kBuckets[b] * 1.0001) {
            return b;
        }
    }
    return kBucketCount - 1;
}

std::atomic<int> s_on{0};

}  // namespace

extern "C" {

// (Vita3K keeps this many bytes for each voice's state; checked here.)
enum { kStateBytes = 160 };
static_assert(sizeof(State) <= kStateBytes, "the NGS resampler's state doesn't fit Vita3K's room for it");

int emusuni_vita_ngs_rs_on(void) {
    return s_on.load(std::memory_order_relaxed);
}

// A game starts (ngs::init): the switch as it is now.
void emusuni_vita_ngs_rs_start(void) {
#if !defined(EMUSUNI_NGSRS_TEST)
    const bool on = emusuni_vita_switch(EMUSUNI_VITA_NGSRESAMPLE) != 0;
#else
    const char* const value = std::getenv("EMUSUNI_VITA_NGSRESAMPLE");
    const bool on = value != nullptr && std::strcmp(value, "1") == 0;
#endif
    s_on.store(on ? 1 : 0, std::memory_order_relaxed);
#if !defined(EMUSUNI_NGSRS_TEST)
    emusuni_vita_log("vita: ngs: the voices' own resampler (16 products a sample, kept across pitch changes) instead of "
                     "FFmpeg's made anew at each: %s", on ? "on (the file vita-ngsresample-on)" : "off (the file vita-ngsresample-on makes it on)");
#endif
}

// A voice starts again (keyed on or off): silence before it.
void emusuni_vita_ngs_rs_reset(void* state) {
    State* const s = static_cast<State*>(state);
    std::memset(s, 0, sizeof(State));
    s->held = kBefore;  // (7 frames of silence: the first output frame is the first input frame)
    s->pos = kBefore;
    s->magic = kMagic;
}

// At most how many frames a call with in_frames more gives.
unsigned emusuni_vita_ngs_rs_bound(const void* state, int source_rate, int dest_rate, unsigned in_frames) {
    const State* const s = static_cast<const State*>(state);
    const double step = static_cast<double>(source_rate) / dest_rate;
    const double held = s->magic == kMagic ? s->held : kBefore;
    return static_cast<unsigned>((held + in_frames) / step) + 2;
}

// in: in_frames stereo frames (floats, L R L R ...); out: room for emusuni_vita_ngs_rs_bound frames. Gives the frames
// made. The rates may change from one call to the next: the voice goes on from where it was.
unsigned emusuni_vita_ngs_rs_run(void* state, int source_rate, int dest_rate, const float* in, unsigned in_frames,
                                 float* out) {
    State* const s = static_cast<State*>(state);
    if (s->magic != kMagic) {
        emusuni_vita_ngs_rs_reset(s);
    }
    if (source_rate <= 0 || dest_rate <= 0 || in_frames == 0) {
        return 0;
    }
    const double step = static_cast<double>(source_rate) / dest_rate;
    const int bucket = BucketFor(step);
    const float* const coef = TableFor(bucket);
#if !defined(__ORBIS__) && !defined(EMUSUNI_NGSRS_TEST)
    // (The PC only, EMUSUNI_VITA_NGSRS_STATS=1: how fast the game plays its voices - frames by step - now and then.)
    static const bool stats = std::getenv("EMUSUNI_VITA_NGSRS_STATS") != nullptr;
    if (stats) {
        static std::atomic<uint64_t> by[kBucketCount + 1];
        by[step == 1.0 ? kBucketCount : bucket] += in_frames;
        static std::atomic<uint64_t> calls{0};
        if ((++calls & 0x3FFF) == 0) {
            emusuni_vita_log("vita: ngs: frames resampled by step: =1 %llu, <=1 %llu, <=1.25 %llu, <=1.5 %llu, <=2 %llu, <=2.5 %llu, "
                             "<=3 %llu, <=4 %llu, <=6 %llu, more %llu", (unsigned long long)by[kBucketCount].load(),
                             (unsigned long long)by[0].load(), (unsigned long long)by[1].load(), (unsigned long long)by[2].load(),
                             (unsigned long long)by[3].load(), (unsigned long long)by[4].load(), (unsigned long long)by[5].load(),
                             (unsigned long long)by[6].load(), (unsigned long long)by[7].load(), (unsigned long long)by[8].load());
        }
    }
#endif

    // The frames kept and the new ones in one row (a voice's update is a few hundred frames).
    thread_local std::vector<float> row;
    const uint32_t total = s->held + in_frames;
    if (row.size() < static_cast<size_t>(total) * 2) {
        row.resize(static_cast<size_t>(total) * 2 + 1024);
    }
    float* const x = row.data();
    std::memcpy(x, s->hist, static_cast<size_t>(s->held) * 2 * sizeof(float));
    std::memcpy(x + static_cast<size_t>(s->held) * 2, in, static_cast<size_t>(in_frames) * 2 * sizeof(float));

    double pos = s->pos;
    unsigned made = 0;
    const double last = static_cast<double>(total) - (kTaps - kBefore - 1);  // pos < last: its taps are all here
    while (pos < last) {
        const int i = static_cast<int>(pos);
        const int phase = static_cast<int>((pos - i) * kPhases + 0.5);
        const float* const c = coef + phase * kTaps * 2;
        const float* const at = x + static_cast<ptrdiff_t>(i - kBefore) * 2;
        // (four sums, two frames' L R at a time: one SSE register, each lane's order fixed)
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        for (int k = 0; k < kTaps * 2; k += 4) {
            a0 += c[k] * at[k];
            a1 += c[k + 1] * at[k + 1];
            a2 += c[k + 2] * at[k + 2];
            a3 += c[k + 3] * at[k + 3];
        }
        out[2 * made] = a0 + a2;
        out[2 * made + 1] = a1 + a3;
        made++;
        pos += step;
    }

    // Keep what the next output frames need: from 7 frames before the next one's place.
    int64_t first = static_cast<int64_t>(pos) - kBefore;
    if (first < 0) {
        first = 0;
    }
    if (first > total) {
        first = total;
    }
    uint32_t keep = total - static_cast<uint32_t>(first);
    if (keep > kHold) {  // (cannot be: pos >= last; but never more than the room)
        first += keep - kHold;
        keep = kHold;
    }
    std::memcpy(s->hist, x + first * 2, static_cast<size_t>(keep) * 2 * sizeof(float));
    s->held = keep;
    s->pos = pos - static_cast<double>(first);
    return made;
}

}  // extern "C"
