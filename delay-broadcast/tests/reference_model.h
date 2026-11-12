#pragma once

#include "probe_report.h"

#include "../src/audio_types.h"
#include "../src/delay_engine.h"
#include "../src/pads.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dbs {
namespace tst {

// Independent per-sample reference model for replacement behaviour.
//
// Everything in this class is derived from first principles:
//   * original material comes only from the analytic generator
//     expectedSampleAt() — the production RingBuffer is never read;
//   * replacement material comes only from the pad library the test itself
//     constructed;
//   * every head position is tracked by this model from the same event
//     sequence, never copied from an EngineCore snapshot.
//
// The edit bookkeeping, seam-fade rule, delay gate, underrun and end-of-input
// policies are re-derived here from the written requirements rather than
// imported from delay_engine.cpp. A bug shared by both would require two
// independent mistakes.
class ReferenceModel {
public:
    struct RefEdit {
        std::uint64_t id = 0;
        std::uint64_t seq = 0;
        std::int64_t begin = 0;
        std::int64_t end = 0;
        std::string pad;
    };

    ReferenceModel(EngineConfig config, PadLibrary pads, CodeTrackConfig track)
        : cfg_(config), pads_(std::move(pads)), track_(track) {}

    // Producer side: only the absolute write head is tracked; original values
    // are always re-derived from the analytic generator when needed.
    void feed(std::int64_t count) { writePos_ += count; }

    void endInput() { inputEnded_ = true; }

    // Re-derive the control-plane decision from the rules and check it
    // against the engine's actual SubmitResult. Accepted edits are then
    // replayed locally.
    SubmitStatus expectedEditStatus(std::int64_t begin, std::int64_t end,
                                    const std::string& pad) const {
        if (begin < 0 || end <= begin) return SubmitStatus::RejectedBadRange;
        if (begin < outputPos_) return SubmitStatus::RejectedAlreadyAired;
        if (end > writePos_) return SubmitStatus::RejectedNotBuffered;
        if (!pads_.find(pad)) return SubmitStatus::RejectedUnknownPad;
        return SubmitStatus::Accepted;
    }

    void applyEdit(const SubmitResult& r, std::int64_t begin, std::int64_t end,
                   const std::string& pad, ProbeReport& report) {
        if (r.status != SubmitStatus::Accepted) return;
        report.expect(r.seq == nextSeq_,
                      "engine sequence matches independently tracked sequence");
        RefEdit e{r.id, r.seq, begin, end, pad};
        // The render set mirrors the engine's overlap resolution exactly:
        // newest event wins, older edits are split (outside pieces kept) or
        // removed (fully covered). This set is never pruned by output
        // position, so predicting an already-aired frame still resolves to the
        // fragment that owned it at airtime; the separate active_ set is
        // pruned for control-plane comparison.
        std::vector<RefEdit> rebuiltRender{e};
        std::vector<RefEdit> rebuiltActive{e};
        for (const RefEdit& old : renderSet_) {
            if (old.end <= begin || old.begin >= end) {
                rebuiltRender.push_back(old);
                continue;
            }
            if (old.begin >= begin && old.end <= end) continue;  // fully covered
            if (old.begin < begin) {
                RefEdit left = old;
                left.end = begin;
                rebuiltRender.push_back(left);
            }
            if (old.end > end) {
                RefEdit right = old;
                right.begin = end;
                rebuiltRender.push_back(right);
            }
        }
        for (const RefEdit& old : active_) {
            if (old.end <= begin || old.begin >= end) {
                rebuiltActive.push_back(old);
                continue;
            }
            if (old.begin >= begin && old.end <= end) continue;
            if (old.begin < begin) {
                RefEdit left = old;
                left.end = begin;
                rebuiltActive.push_back(left);
            }
            if (old.end > end) {
                RefEdit right = old;
                right.begin = end;
                rebuiltActive.push_back(right);
            }
        }
        renderSet_.swap(rebuiltRender);
        active_.swap(rebuiltActive);
        ++nextSeq_;
    }

    void applyUndo(const SubmitResult& r, ProbeReport& report) {
        if (r.status != SubmitStatus::Accepted) {
            // Rejected undos leave the global sequence untouched in the engine.
            report.expect(r.seq == nextSeq_,
                          "rejected undo does not consume the global sequence");
            return;
        }
        report.expect(r.seq == nextSeq_,
                      "undo sequence matches independent sequence");
        // The cancelled edit never owned any aired frame, so drop its
        // fragments from both the render set and the active set.
        auto eraseId = [&](std::vector<RefEdit>& v) {
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const RefEdit& e) { return e.id == r.id; }),
                    v.end());
        };
        eraseId(renderSet_);
        eraseId(active_);
        ++nextSeq_;
    }

    // One pull request. The returned vector holds exactly the frames the
    // reference predicts for this call; its length must equal what the engine
    // reports producing so block tiling is checked too.
    std::vector<float> pull(std::int64_t count) {
        std::vector<float> out;
        for (std::int64_t requested = 0; requested < count; ++requested) {
            if (paused_) break;
            const std::int64_t pos = outputPos_;
            const bool buffered = pos < writePos_;
            const bool delayReady = writePos_ >= pos + cfg_.delayFrames + 1;
            const bool prebuffering = writePos_ < cfg_.delayFrames + 1;
            const bool available = buffered && (inputEnded_ || delayReady);

            if (!available) {
                if (prebuffering && !inputEnded_) break;       // startup fill waits
                if (inputEnded_ &&
                    cfg_.endOfInput == EndOfInputPolicy::StopOutput) {
                    break;
                }
                if (!inputEnded_ &&
                    cfg_.underrun == UnderrunPolicy::StopOutput) {
                    paused_ = true;
                    ++underrun_;
                    break;
                }
                // InsertSilence: the zero frame occupies its own absolute
                // slot and the write head advances with the output head, so
                // real input later lands at its correct position.
                silence_.push_back(writePos_);
                ++writePos_;
                out.push_back(0.0f);
                ++outputPos_;
                ++silenceFilled_;
                if (!inputEnded_) ++underrun_;
                prune();
                continue;
            }
            out.push_back(render(pos));
            ++outputPos_;
            prune();
        }
        return out;
    }

    bool resume() {
        if (!paused_) return false;
        if (writePos_ <= outputPos_) return false;
        paused_ = false;
        return true;
    }

    // Predicted value at any absolute frame, independent of what the engine
    // currently holds. Silence slots, original material and replaced material
    // are each resolved from this model's own state.
    float predict(std::int64_t frame) const {
        if (isSilenceSlot(frame)) return 0.0f;
        return render(frame);
    }

    bool isSilenceSlot(std::int64_t frame) const {
        return std::binary_search(silence_.begin(), silence_.end(), frame);
    }

    // True when the frame is inside an active declared replacement range.
    // Fades stay strictly interior, so a range boundary never bleeds.
    bool insideReplacement(std::int64_t frame) const { return owner(frame) != nullptr; }

    // True when the frame is owned by an edit but outside both seam fades,
    // meaning its rendered value is pure pad material with no original mix.
    bool deepInsideReplacement(std::int64_t frame) const {
        const RefEdit* here = owner(frame);
        if (!here || cfg_.fadeFrames <= 0) return here != nullptr;
        const std::int64_t f = fadeFor(here);
        const std::int64_t fromStart = frame - here->begin;
        const std::int64_t fromEnd = here->end - 1 - frame;
        return fromStart >= f && fromEnd >= f;
    }

    std::int64_t writePos() const { return writePos_; }
    std::int64_t outputPos() const { return outputPos_; }
    bool paused() const { return paused_; }
    std::int64_t underruns() const { return underrun_; }
    std::int64_t silenceFilled() const { return silenceFilled_; }
    const std::vector<RefEdit>& activeEdits() const { return active_; }
    const CodeTrackConfig& trackConfigForTest() const { return track_; }
    const EngineConfig& engineConfigForTest() const { return cfg_; }

private:
    // Render-time ownership uses the unpruned, overlap-resolved fragment set.
    // The engine picks the active edit with the highest sequence; because
    // splits already ceded covered territory to newer edits, the highest-seq
    // covering fragment here is exactly what aired at that absolute frame.
    const RefEdit* owner(std::int64_t frame) const {
        const RefEdit* winner = nullptr;
        for (const RefEdit& e : renderSet_) {
            if (frame >= e.begin && frame < e.end &&
                (!winner || e.seq > winner->seq)) {
                winner = &e;
            }
        }
        return winner;
    }

    const RefEdit* endingAt(std::int64_t boundary) const {
        const RefEdit* found = nullptr;
        for (const RefEdit& e : renderSet_) {
            if (e.end == boundary && (!found || e.seq > found->seq)) found = &e;
        }
        return found;
    }

    const RefEdit* startingAt(std::int64_t boundary) const {
        const RefEdit* found = nullptr;
        for (const RefEdit& e : renderSet_) {
            if (e.begin == boundary && (!found || e.seq > found->seq)) found = &e;
        }
        return found;
    }

    std::int64_t fadeFor(const RefEdit* region) const {
        std::int64_t len = 1LL << 40;
        if (region) len = region->end - region->begin;
        return std::max<std::int64_t>(
            1, std::min({cfg_.fadeFrames, (len + 1) / 2,
                         static_cast<std::int64_t>(65536)}));
    }

    float sourceOf(const RefEdit* e, std::int64_t frame) const {
        if (!e) return expectedSampleAt(frame, track_);
        if (frame < e->begin || frame >= e->end) {
            return expectedSampleAt(frame, track_);
        }
        const Pad* pad = pads_.find(e->pad);
        if (!pad) return expectedSampleAt(frame, track_);
        return pad->at(frame - e->begin);
    }

    float render(std::int64_t frame) const {
        const RefEdit* here = owner(frame);
        const float primary = sourceOf(here, frame);
        if (cfg_.fadeFrames <= 0 || !here) return primary;
        const std::int64_t f = fadeFor(here);
        const std::int64_t fromStart = frame - here->begin;
        if (fromStart >= 0 && fromStart < f) {
            const float g = static_cast<float>(fromStart + 1) /
                            static_cast<float>(f);
            return sourceOf(endingAt(here->begin), frame) * (1.0f - g) +
                   primary * g;
        }
        const std::int64_t fromEnd = here->end - 1 - frame;
        if (fromEnd >= 0 && fromEnd < f) {
            const float g = static_cast<float>(fromEnd + 1) /
                            static_cast<float>(f);
            return primary * g +
                   sourceOf(startingAt(here->end), frame) * (1.0f - g);
        }
        return primary;
    }

    void prune() {
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const RefEdit& e) {
                                         return e.end <= outputPos_;
                                     }),
                      active_.end());
    }

    EngineConfig cfg_;
    PadLibrary pads_;
    CodeTrackConfig track_;
    std::int64_t writePos_ = 0;
    std::int64_t outputPos_ = 0;
    std::uint64_t nextSeq_ = 1;
    bool inputEnded_ = false;
    bool paused_ = false;
    std::int64_t underrun_ = 0;
    std::int64_t silenceFilled_ = 0;
    std::vector<RefEdit> renderSet_;  // unpruned overlap-resolved fragments
    std::vector<RefEdit> active_;     // pruned, mirrors engine control state
    std::vector<std::int64_t> silence_;  // sorted absolute silence slots
};

}  // namespace tst
}  // namespace dbs
