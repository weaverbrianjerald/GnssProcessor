#pragma once
/**
 * @file NetworkClockEmitRebase.hpp
 * @brief Card CLK-R (docs/SPEC_clock_emit_rebase.md) — emission-frame re-base of
 *        the network's per-satellite clocks (the common-mode frame hold) plus the
 *        census-aligned per-satellite step test that gives the surviving
 *        re-realization class its wire marks.
 *
 * ## Why this exists (the internal 12 h forensics)
 *
 * On the internal (self-contained golden63) lineage the emitted satellite clock
 * re-realizes on **613 of 1440** census epochs (per-epoch common-mode steps
 * > 0.1 m, max 17.7 m; 273 epochs additionally carry per-sat outliers > 0.5 m)
 * riding on ONE wire mark — the clock channel has no magnitude jump test by
 * design (RTA-11: its counter carries datum-change components only), so the
 * whole class is invisible to every consumer.  FC-1's shadow measurement is the
 * design input: ~98 % of the emitted clock's epoch-to-epoch motion is producer
 * frame churn (the physical change is ~2 cm / 30 s, correlation ≈ 0), i.e.
 * nearly all of that wire motion carries no information.  The phase-bias channel
 * already has both an emission-frame re-base (@ref NetworkEmitRebase) and a
 * continuity filter (@ref NetworkPbEmitFilter) — which is why the P channel is
 * quasi-static on the same wire — the clock channel had neither.
 *
 * ## What it does (two independent stages, each its own default-OFF knob)
 *
 * **Stage 1 — common-mode frame hold** (`network.emit_clock_frame_hold`).
 * Per constellation, the per-epoch median innovation @f$\mu@f$ of the raw clocks
 * against the previous EMITTED values (fresh, unforced keys only, @c min_common
 * floor) is accumulated into a frame offset @f$A_c@f$, and every satellite —
 * new sights included — is emitted in the HELD frame @f$c_0 - A_c@f$.  The
 * removed direction is the per-epoch clock-ensemble gauge: a consumer's
 * per-epoch receiver clock absorbs it identically, and between-satellite
 * differences are preserved exactly (one shared subtrahend), so user SD/DD
 * quantities are untouched by construction.  Offline replay of the stored 12 h
 * product: census common-mode channel **613 → 0** epochs.
 *
 * **Stage 2 — per-sat step marks** (`network.clock_disc_jump_test`).  After the
 * frame hold, a satellite whose common-removed step exceeds
 * `network.clock_disc_jump_m` (default 1.5 m ≈ 16 σ of the measured per-epoch
 * walk step, ~7 marks/h on the reference lineage) is reported for a
 * @ref NetworkProductEmitter::markSatDiscontinuity retro-bump — the proven
 * R-record chain (per-sat phase/code/clock counters step; the consumer resets
 * that satellite's ambiguities at the event epoch).  The emitter itself is
 * untouched: RTA-11's "no magnitude test inside the clock counter" stands — the
 * test lives at the emission-prep tier and drives the FORCED path, which a
 * replay reproduces from the serialized R/E records with no re-run.
 *
 * ## What it deliberately does NOT do (falsified at design time — SPEC §2)
 *
 * No per-key value smoothing.  The per-sat channel is a random WALK (robust step
 * σ 0.09–0.16 m per 30 s, ~8× the physical change; 97 % of > 0.5 m steps are
 * sticky level shifts), so a per-key hold+gate either storms marks (~4 min
 * per-sat reset spacing at k=4) or walks metres off the solve's self-consistent
 * frame (30.8 m measured with a loose v2 gate).  Per-sat temporal stiffness is
 * estimator work (Q3's guarded temporal anchor), not emission work.
 *
 * ## Guards (standing invariants #1/#7)
 *
 * - **Leash**: @f$|A_c|@f$ > `network.clock_frame_leash_m` (default 50 m;
 *   measured 12 h wander 23 m) re-anchors to the solve frame — the caller MUST
 *   pair the re-anchor with @ref NetworkProductEmitter::markDatumChange so the
 *   re-coupling edge is marked, never silent.  The leaf reports the set of
 *   re-anchored constellations for exactly that purpose.
 * - **Forced keys** (pending per-sat retro bumps, datum-changed constellations)
 *   are excluded from the @f$\mu@f$ vote and the step test: a marked genuine
 *   event must neither drag the frame nor double-mark.
 * - **Gap/init keys** re-seed in the held frame with no step test (a
 *   re-appearance is a first sighting, matching the emitter's own semantics).
 *
 * ## Scope / conventions
 *
 * Pure numeric leaf (precedent: @ref NetworkEmitRebase, @ref NetworkPbEmitFilter);
 * no dependency on the estimator or the emitter.  The caller (networkEmitStore)
 * builds one Entry per emitted clock satellite, calls @ref rebaseEpoch BEFORE
 * @c emitToStore + the on-disk write (the transform is baked into the product —
 * replay never re-runs it), routes @c marked entries to
 * @c markSatDiscontinuity / the R-record set, and @c reanchored constellations
 * to @c markDatumChange.  Outputs-only: the solve is byte-identical even when
 * armed.  Driven directly from @c tests/test_clock_emit_rebase.cpp with
 * synthetic streams of known truth.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class NetworkClockEmitRebase
 * @brief Per-constellation common-mode frame hold + census-aligned per-sat step
 *        marks for the emitted satellite clocks (Card CLK-R).
 */
class NetworkClockEmitRebase {
public:
    /// Tunables (from `network.emit_clock_frame_hold` / `network.clock_disc_jump_*`;
    /// see GnssConfigLoadNetwork).
    struct Config {
        bool   frame_hold{false}; ///< Stage 1: hold the per-constellation common mode (emit in the held frame).
        double leash_m{50.0};     ///< Stage 1: |A_c| re-anchor bound (m).  Measured 12 h wander on the reference lineage: 23 m.
        bool   jump_test{false};  ///< Stage 2: mark per-sat common-removed steps (independent of stage 1 — alone it marks the raw wire).
        double jump_m{1.5};       ///< Stage 2: flat step threshold (m).  Empirical: ≈16 σ of the per-epoch walk step (SPEC §1); the census outlier floor (0.5 m) would mark 10× as often.
        double max_gap_s{600.0};  ///< Key gap (s) beyond which a returning satellite is a first sighting (re-init, no step test).
        double gap_jump_m{0.0};   ///< Stage 2b (E1, `network.clock_disc_gap_jump_m`): on a beyond-max_gap_s RETURN, test the common-removed step against the RETAINED pre-gap baseline with THIS threshold (m).  0 = OFF (a stale return stays an untested first sighting — the measured gap-blindness on ~55-min eclipse re-admissions, SPEC_synthetic_minimum_network §18.1; on non-Q3 postures nothing else marks those steps).  Designed for frame_hold lineages: with the hold OFF, the un-drained common wander over the gap contaminates the statistic and this threshold must absorb it.
        int    min_common{4};     ///< Minimum fresh unforced keys for a valid μ vote; below it the frame does not move this epoch.
    };

    /// One emitted satellite clock to re-base this epoch.
    struct Entry {
        std::string sat;       ///< RINEX satellite id ("G01").
        double      c0_m{0.0}; ///< IN: raw emitted clock (m).  OUT: the held-frame value the emitter/product must carry.
        bool        forced{false}; ///< A per-sat retro bump or constellation datum change is already pending for this satellite this epoch ⇒ excluded from the μ vote + step test (marked upstream; its step must not double-mark or drag the frame).
        bool        marked{false}; ///< OUT: stage-2 step test fired ⇒ the caller routes this satellite to markSatDiscontinuity (+ the R-record set).
        double      step_m{0.0};   ///< OUT: the common-removed step (m) when @ref marked — the event-row statistic; 0 otherwise.
        double      level_step_m{0.0}; ///< IN (card CLV): the satellite's declared level step since its previous emission (m), removed from its innovation before the frame vote and the step tests: a level re-base moved the clock AND the phase biases, so it is neither a frame move nor a discontinuity.  0 unless network.clock_level_carry (byte-identical).
    };

    /// Running tallies for the run-end telemetry line (invariant #4).
    struct Counters {
        long long epochs{0};      ///< rebaseEpoch invocations with ≥1 entry.
        long long rows{0};        ///< entries processed.
        long long marks{0};       ///< stage-2 step marks reported.
        long long reanchors{0};   ///< stage-1 leash re-anchors (each paired with a datum mark by the caller).
        long long inits{0};       ///< first-sighting seeds.
        long long gaps{0};        ///< beyond-horizon re-inits.
        long long gap_marks{0};   ///< stage-2b beyond-horizon-return step marks (gap_jump_m armed).
        double    sum_abs_mu{0.0};///< Σ|μ| drained into the frame accumulators (m) — the churn the wire no longer carries.
        double    max_abs_acc{0.0};///< max |A_c| reached (m) — headroom against the leash.
    };

    explicit NetworkClockEmitRebase(Config cfg) : cfg_(cfg) {}
    NetworkClockEmitRebase() = default;

    /// (Re)configure — the run context owns a default-constructed engine and
    /// configures it once from the loaded knobs before the epoch loop.
    void configure(const Config& cfg) { cfg_ = cfg; }

    /**
     * @brief The engine's cross-epoch FRAME state, exportable for the RTB-7
     *        snapshot (SPEC_synthetic_minimum_network §18.10 resume frame
     *        restore): the per-constellation accumulator A_c, the per-sat
     *        previous held value + last-seen stamp, and the epoch count.
     *
     * Without it every resume restarts the held frame at zero, so the emitted
     * common mode steps by A_c and the F1 seam rule (keyed on
     * `counters().epochs == 0`) marks a datum change that the consumer must
     * re-absorb — on a resume whose solve frame is otherwise continuous, that
     * mark is the only re-realization left on the wire.
     */
    struct FrameState {
        std::map<char, double>                          acc;     ///< constellation -> A_c (m).
        std::map<std::string, std::pair<double, double>> prev;   ///< sat -> {last held (m), last seen (s)}.
        long long                                       epochs{0};
    };
    /// Snapshot the frame state (cheap: a few hundred entries).
    FrameState exportFrameState() const {
        FrameState fs;
        fs.acc = acc_;
        for (const auto& [sat, ks] : prev_) fs.prev[sat] = {ks.last_held_m, ks.last_seen_s};
        fs.epochs = counters_.epochs;
        return fs;
    }
    /// Restore a previously exported frame state (a warm resume that continues
    /// the held frame).  Counters other than @c epochs restart at zero — they
    /// are run telemetry, not frame state.
    void restoreFrameState(const FrameState& fs) {
        acc_ = fs.acc;
        prev_.clear();
        for (const auto& [sat, hp] : fs.prev) prev_[sat] = KeyState{hp.first, hp.second};
        counters_.epochs = fs.epochs;
    }

    /**
     * @brief Re-base one epoch's emitted clocks IN PLACE.
     *
     * Per constellation: form innovations of fresh unforced keys against the
     * previous emitted values, take the median as this epoch's common-mode
     * innovation μ, advance the frame accumulator (stage 1 armed), and emit
     * every entry in the held frame.  Stage 2 (armed) marks entries whose
     * common-removed step exceeds the threshold.  A leash crossing re-anchors
     * the constellation to the solve frame (raw passthrough, accumulator
     * cleared) and reports it in @p reanchored — the caller pairs each with a
     * datum mark.  μ is computed (and the step statistic valid) regardless of
     * which stages are armed, so `clock_disc_jump_test` alone marks the RAW
     * wire with the census's own statistic.
     *
     * @param entries       This epoch's emitted clock satellites.
     * @param epoch_time_s  Continuous product time (week·604800+tow, s) — the
     *                      per-key gap clock (deterministic on replay).
     * @param reanchored    OUT: constellation letters that re-anchored this
     *                      epoch (leash).  The caller MUST mark each
     *                      (markDatumChange) — a silent re-anchor is exactly
     *                      the unmediated edge this card exists to close.
     */
    void rebaseEpoch(std::vector<Entry>& entries, double epoch_time_s,
                     std::set<char>& reanchored);

    const Counters& counters() const { return counters_; }

    /// Card GAL: the frame-hold tallies of @ref Counters that are per constellation
    /// by construction (the accumulator, the μ vote and the leash are all keyed on
    /// the constellation letter), split by that letter for the multi-constellation
    /// run-end line.  Filled on the same branches as @ref Counters, so the sums over
    /// letters equal the global values.
    struct ConstCounters {
        long long rows{0};         ///< entries processed.
        long long marks{0};        ///< stage-2 step marks.
        long long reanchors{0};    ///< stage-1 leash re-anchors.
        double    sum_abs_mu{0.0}; ///< Σ|μ| drained into this constellation's accumulator (m).
        double    max_abs_acc{0.0};///< max |A_c| reached (m).
    };
    /// Per-constellation tallies (letter -> @ref ConstCounters).
    const std::map<char, ConstCounters>& constCounters() const { return const_counters_; }

    /// Test seam: the current frame accumulator A_c (m) for a constellation
    /// letter (0.0 when the constellation has no frame state yet).
    double frameAccForTest(char constellation) const {
        const auto it = acc_.find(constellation);
        return it != acc_.end() ? it->second : 0.0;
    }

    /// Test seam: the previous emitted (held-frame) value for a satellite;
    /// false when the satellite has no state yet.
    bool prevEmittedForTest(const std::string& sat, double& value_m) const {
        const auto it = prev_.find(sat);
        if (it == prev_.end()) return false;
        value_m = it->second.last_held_m;
        return true;
    }

private:
    /// One satellite's previous emitted value + last-seen clock.
    struct KeyState { double last_held_m{0.0}; double last_seen_s{0.0}; };

    Config    cfg_;
    Counters  counters_;
    std::map<char, ConstCounters> const_counters_;   ///< card GAL: per-constellation tallies.
    std::map<char, double>          acc_;   ///< constellation -> frame accumulator A_c (m).
    std::map<std::string, KeyState> prev_;  ///< sat -> previous emitted (held-frame) value.
};

}  // namespace GnssProcessor
