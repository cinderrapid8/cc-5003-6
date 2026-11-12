#pragma once

#include "probe_report.h"

#include "../src/ring_buffer.h"
#include "../src/pads.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace dbs {
namespace tst {

// Independent shadow of the bounded ring. It uses a plain deque keyed by
// absolute frame positions and a completely separate value generator, so it
// shares no storage, indexing or masking logic with RingBuffer. Expected
// values are re-derived for every check; they are never copied out of the
// ring under test.
class ShadowRing {
public:
    explicit ShadowRing(std::size_t capacity) : capacity_(capacity) {}

    std::int64_t write(const float* src, std::int64_t writeHead,
                       std::int64_t outputHead, std::int64_t maxFrames) {
        const std::int64_t room =
            static_cast<std::int64_t>(capacity_) - (writeHead - outputHead);
        const std::int64_t n = std::min(std::min(maxFrames, room),
                                        static_cast<std::int64_t>(capacity_));
        for (std::int64_t i = 0; i < n; ++i) {
            cells_.push_back({writeHead + i, src[i]});
        }
        return n;
    }

    // Explicit consumer advancement: cells the output head has passed leave
    // the bounded window. Kept separate from write() so the accepted-frame
    // decision depends only on the heads passed in, never on internal state.
    void advanceConsumer(std::int64_t outputHead) {
        while (!cells_.empty() && cells_.front().pos < outputHead) {
            cells_.pop_front();
        }
    }

    bool has(std::int64_t position) const {
        for (const Cell& c : cells_)
            if (c.pos == position) return true;
        return false;
    }

    float valueAt(std::int64_t position) const {
        for (const Cell& c : cells_)
            if (c.pos == position) return c.value;
        return 0.0f;
    }

    std::size_t capacity() const { return capacity_; }

private:
    struct Cell {
        std::int64_t pos;
        float value;
    };
    std::size_t capacity_;
    std::deque<Cell> cells_;
};

// Analytic, ring-independent value at an absolute frame. Distinct from the
// production generator so an accidental shared constant cannot mask a bug.
inline float probeSample(std::int64_t frame) {
    const double phase =
        0.6180339887498949 * static_cast<double>(frame + 1);
    const double frac = phase - static_cast<long long>(phase);
    return static_cast<float>(0.9f * (2.0 * frac - 1.0));
}

inline std::vector<float> probeBlock(std::int64_t base, std::int64_t n) {
    std::vector<float> out(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] = probeSample(base + i);
    return out;
}

// --- Positive probes -------------------------------------------------------

// Repeated wraps with awkward write sizes: every readable slot must match the
// independently derived value across many capacity cycles. The producer and
// consumer advance in strict alternation with equal totals, so the bounded
// window never overflows while both heads travel many times around the
// physical storage.
template <typename Ring>
void probeWrapCrossing(Ring& ring, ShadowRing& shadow, ProbeReport& report) {
    std::int64_t writeHead = 0;
    std::int64_t outputHead = 0;
    // The ring rounds its capacity up to a power of two; all window math must
    // use that rounded size, not the originally requested capacity.
    const std::int64_t cap = static_cast<std::int64_t>(ring.capacity());
    const std::int64_t base[] = {1, 3, 7, 13, 29, 17, 31, 23};

    for (int round = 0; round < 200; ++round) {
        const std::int64_t limit = std::max<std::int64_t>(2, cap / 2);
        const std::int64_t n = 1 + base[round % 8] % (limit - 1);
        auto block = probeBlock(writeHead, n);
        const std::int64_t got =
            ring.write(block.data(), writeHead, outputHead, n);
        const std::int64_t want =
            shadow.write(block.data(), writeHead, outputHead, n);
        report.expect(got == n && want == n,
                      "wrap: write fully accepted inside the bounded window");
        writeHead += got;

        // Cross-check every still-buffered slot against the analytic value.
        for (std::int64_t p = outputHead; p < writeHead; ++p) {
            report.expect(shadow.has(p), "shadow tracks every live slot");
            const float expected = probeSample(p);
            report.expect(std::fabs(ring.read(p) - expected) <= 1e-6f,
                          "wrap: ring read matches independently derived value");
            report.expect(std::fabs(shadow.valueAt(p) - expected) <= 1e-6f,
                          "wrap: shadow value matches the analytic generator");
        }

        // Consume exactly what was just fed so the window returns to empty
        // and the next write starts at a fresh physical offset.
        outputHead += n;
        shadow.advanceConsumer(outputHead);
        report.expect(ring.buffered(writeHead, outputHead) == 0,
                      "wrap: window drains exactly between feeds");
    }
    report.expect(writeHead >= cap,
                  "wrap: heads travelled at least one full physical cycle");
}

// The core invariant: a write must touch only its declared interval. Slots
// outside that interval are read before and after the operation and must be
// byte-identical, with expected values derived independently.
template <typename Ring>
void probeOutsideIntervalInvariant(Ring& ring, ShadowRing& shadow,
                                   ProbeReport& report) {
    std::int64_t writeHead = 0;
    std::int64_t outputHead = 0;
    const std::int64_t cap = static_cast<std::int64_t>(ring.capacity());

    auto prime = probeBlock(0, cap);
    writeHead += ring.write(prime.data(), 0, 0, cap);
    shadow.write(prime.data(), 0, 0, cap);

    // Free a middle band, then write a sub-range that wraps the physical edge.
    outputHead = cap / 2;
    const std::int64_t begin = writeHead;
    const std::int64_t len = cap / 2;
    auto band = probeBlock(begin, len);

    // Snapshot slots that must remain untouched (the whole current window).
    std::vector<std::pair<std::int64_t, float>> untouched;
    for (std::int64_t p = outputHead; p < writeHead; ++p)
        untouched.push_back({p, ring.read(p)});

    const std::int64_t got =
        ring.write(band.data(), writeHead, outputHead, len);
    shadow.write(band.data(), writeHead, outputHead, len);
    report.expect(got == len, "invariant: declared interval fully stored");
    writeHead += got;

    for (const auto& [p, before] : untouched) {
        // These positions are strictly outside the newly written interval.
        report.expect(p < begin, "invariant: probed slot predates the write");
        report.expect(ring.read(p) == before,
                      "invariant: samples outside the write interval do not change");
        report.expect(std::fabs(ring.read(p) - probeSample(p)) <= 1e-6f,
                      "invariant: untouched slot still equals analytic value");
    }
}

// Late operation: once the bounded window is saturated, further writes are
// rejected entirely and no live slot changes. Reads at positions outside the
// window are not trusted as data; the guard predicate must classify them.
template <typename Ring>
void probeLateWriteRejected(Ring& ring, ShadowRing& shadow,
                            ProbeReport& report) {
    std::int64_t writeHead = 0;
    std::int64_t outputHead = 0;
    const std::int64_t cap = static_cast<std::int64_t>(ring.capacity());

    auto prime = probeBlock(0, cap);
    writeHead += ring.write(prime.data(), 0, 0, cap);
    shadow.write(prime.data(), 0, 0, cap);

    std::vector<float> before;
    for (std::int64_t p = 0; p < cap; ++p) before.push_back(ring.read(p));

    auto late = probeBlock(writeHead, 64);
    const std::int64_t accepted =
        ring.write(late.data(), writeHead, outputHead, 64);
    const std::int64_t shadowAccepted =
        shadow.write(late.data(), writeHead, outputHead, 64);
    report.expect(accepted == 0 && shadowAccepted == 0,
                  "late write into a full window is refused");
    for (std::int64_t p = 0; p < cap; ++p) {
        report.expect(ring.read(p) == before[static_cast<std::size_t>(p)],
                      "late write leaves every buffered sample unchanged");
    }

    // Window membership guard: positions below outputHead or at/above
    // writeHead are not contained, even though a masked slot physically exists.
    report.expect(!ring.contains(outputHead - 1, writeHead, outputHead),
                  "already-aired position is outside the window");
    report.expect(!ring.contains(writeHead, writeHead, outputHead),
                  "not-yet-written position is outside the window");
    report.expect(ring.contains(outputHead, writeHead, outputHead),
                  "first live position is inside the window");
    report.expect(ring.contains(writeHead - 1, writeHead, outputHead),
                  "last live position is inside the window");
}

// Non-power-of-two requests and an unaligned declared capacity must round to
// the same power-of-two window in the ring and its independently sized shadow.
template <typename Ring>
void probeCapacityRounding(Ring& ring, ShadowRing& shadow,
                           ProbeReport& report, std::size_t requested) {
    std::size_t expectedPow2 = 1;
    while (expectedPow2 < requested) expectedPow2 <<= 1;
    report.expect(ring.capacity() == expectedPow2,
                  "ring rounds capacity up to a power of two");
    report.expect(shadow.capacity() == expectedPow2,
                  "shadow is constructed with the same rounded capacity");
}

// --- Negative ring mutants -------------------------------------------------

// Mutant A: ignores the bounded window and always stores, overwriting
// not-yet-aired material when the physical ring wraps.
class OverwriteRing {
public:
    explicit OverwriteRing(std::size_t capacity)
        : mask_(roundPow2(capacity) - 1), storage_(mask_ + 1, 0.0f) {}
    std::size_t capacity() const { return storage_.size(); }
    std::int64_t buffered(std::int64_t w, std::int64_t o) const { return w - o; }

    std::int64_t write(const float* src, std::int64_t writeHead,
                       std::int64_t outputHead, std::int64_t maxFrames) {
        (void)outputHead;
        for (std::int64_t i = 0; i < maxFrames; ++i)
            storage_[static_cast<std::uint64_t>(writeHead + i) & mask_] = src[i];
        return maxFrames;  // fault: claims everything was accepted
    }
    float read(std::int64_t p) const {
        return storage_[static_cast<std::uint64_t>(p) & mask_];
    }
    bool contains(std::int64_t p, std::int64_t w, std::int64_t o) const {
        return p >= o && p < w;
    }

private:
    static std::size_t roundPow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    std::size_t mask_;
    std::vector<float> storage_;
};

// Mutant B: wrong wrap mask (one bit short), so crossing the physical edge
// aliases onto a live slot.
class WrongMaskRing {
public:
    explicit WrongMaskRing(std::size_t capacity)
        : mask_(roundPow2(capacity) / 2 - 1), storage_(roundPow2(capacity), 0.0f) {}
    std::size_t capacity() const { return storage_.size(); }
    std::int64_t buffered(std::int64_t w, std::int64_t o) const { return w - o; }

    std::int64_t write(const float* src, std::int64_t writeHead,
                       std::int64_t outputHead, std::int64_t maxFrames) {
        const std::int64_t room =
            static_cast<std::int64_t>(storage_.size()) - (writeHead - outputHead);
        const std::int64_t n = std::min(maxFrames, room);
        for (std::int64_t i = 0; i < n; ++i)
            storage_[static_cast<std::uint64_t>(writeHead + i) & mask_] = src[i];
        return n;
    }
    float read(std::int64_t p) const {
        return storage_[static_cast<std::uint64_t>(p) & mask_];
    }
    bool contains(std::int64_t p, std::int64_t w, std::int64_t o) const {
        return p >= o && p < w;
    }

private:
    static std::size_t roundPow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    std::size_t mask_;
    std::vector<float> storage_;
};

// Mutant C: stale replay — a read past the write head returns an old physical
// slot instead of being treated as unavailable.
class StaleReadRing {
public:
    explicit StaleReadRing(std::size_t capacity)
        : mask_(roundPow2(capacity) - 1), storage_(mask_ + 1, 0.7f) {}
    std::size_t capacity() const { return storage_.size(); }
    std::int64_t buffered(std::int64_t w, std::int64_t o) const { return w - o; }

    std::int64_t write(const float* src, std::int64_t writeHead,
                       std::int64_t outputHead, std::int64_t maxFrames) {
        const std::int64_t room =
            static_cast<std::int64_t>(storage_.size()) - (writeHead - outputHead);
        const std::int64_t n = std::min(maxFrames, room);
        for (std::int64_t i = 0; i < n; ++i)
            storage_[static_cast<std::uint64_t>(writeHead + i) & mask_] = src[i];
        return n;
    }
    float read(std::int64_t p) const {
        return storage_[static_cast<std::uint64_t>(p) & mask_];
    }
    // Fault: claims every non-negative position is contained.
    bool contains(std::int64_t p, std::int64_t w, std::int64_t o) const {
        (void)w;
        (void)o;
        return p >= 0;
    }

private:
    static std::size_t roundPow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    std::size_t mask_;
    std::vector<float> storage_;
};

// Generic probe runner applied to both the good ring and every mutant. The
// good implementation must pass all probes; each mutant must fail at least
// one, proving the probes actually discriminate.
template <typename Ring>
ProbeReport runRingProbes(std::size_t requestedCapacity, bool loud = true) {
    ProbeReport report;
    report.loud = loud;
    Ring ring(requestedCapacity);
    ShadowRing shadow(ring.capacity());
    probeCapacityRounding(ring, shadow, report, requestedCapacity);
    probeWrapCrossing(ring, shadow, report);

    Ring ring2(requestedCapacity);
    ShadowRing shadow2(ring2.capacity());
    probeOutsideIntervalInvariant(ring2, shadow2, report);

    Ring ring3(requestedCapacity);
    ShadowRing shadow3(ring3.capacity());
    probeLateWriteRejected(ring3, shadow3, report);
    return report;
}

}  // namespace tst
}  // namespace dbs
