#pragma once
/**
 * @file NetworkClockEmitRebase.hpp
 * @brief Emission-frame re-base of the network's per-satellite clocks (the common-mode
 *        frame hold), plus the census-aligned per-satellite step test that gives the
 *        remaining re-realization class its wire marks (card CLK-R,
 *        docs/SPEC_clock_emit_rebase.md).
 *
 * ## Motivation (the internal 12 h forensics, SPEC §1)
 *
 * The clock channel has no magnitude jump test by design (RTA-11: its counter carries
 * datum-change components only), so a re-realization of the emitted satellite clocks
 * reaches the consumer without a wire mark.  On the internal (self-contained golden63)
 * lineage the emitted satellite clock re-realized on **613 of 1440** census epochs
 * (per-epoch common-mode steps > 0.1 m, max 17.7 m; 273 epochs also carry per-satellite
 * outliers > 0.5 m) under a single wire mark.  The FC-1 shadow measurement is the
 * design input: about 98 % of the emitted clock's epoch-to-epoch motion is producer frame
 * churn (the physical change is about 2 cm per 30 s, correlation ≈ 0), so nearly all of
 * that wire motion carries no information.  The phase-bias channel has both an
 * emission-frame re-base (@ref NetworkEmitRebase) and a continuity filter
 * (@ref NetworkPbEmitFilter), which keep the P channel quasi-static on the same wire;
 * this class supplies the equivalent for the clock channel.
 *
 * ## What it does (independent stages, each behind its own knob, off by default)
 *
 * **Stage 1: common-mode frame hold** (`network.emit_clock_frame_hold`).
 * Per constellation, the per-epoch median innovation @f$\mu@f$ of the raw clocks
 * against the previous emitted values (fresh, unforced keys only, at least
 * @c min_common of them) is accumulated into a frame offset @f$A_c@f$, and every
 * satellite, new sightings included, is emitted in the held frame @f$c_0 - A_c@f$.  The
 * removed direction is the per-epoch clock-ensemble gauge: a consumer's per-epoch
 * receiver clock absorbs it identically, and between-satellite differences are preserved
 * exactly (one shared subtrahend), so user SD/DD quantities are unchanged by
 * construction.  An offline replay of the stored 12 h product took the census
 * common-mode channel from **613 → 0** epochs.
 *
 * **Stage 2: per-satellite step marks** (`network.clock_disc_jump_test`).  After the
 * frame hold, a satellite whose common-removed step exceeds
 * `network.clock_disc_jump_m` (default 1.5 m, about 16 σ of the measured per-epoch
 * walk step, about 7 marks/h on the reference lineage) is reported for a
 * @ref NetworkProductEmitter::markSatDiscontinuity retro-bump, the existing R-record
 * chain (the per-satellite phase, code and clock counters step; the consumer resets
 * that satellite's ambiguities at the event epoch).  The emitter itself is unchanged,
 * and RTA-11's rule of no magnitude test inside the clock counter still holds: the test
 * lives at the emission-prep tier and drives the forced path, which a replay reproduces
 * from the serialized R/E records without a re-run.  Stage 2b
 * (`network.clock_disc_gap_jump_m`, requires stage 2) extends the test to a satellite
 * returning after a gap; see Config::gap_jump_m.
 *
 * ## Rejected design: per-key value smoothing (falsified at design time, SPEC §2)
 *
 * The per-satellite channel is a random walk (robust step σ 0.09–0.16 m per 30 s, about
 * 8× the physical change; 97 % of steps > 0.5 m are persistent level shifts), so a
 * per-key hold and gate either produces a flood of marks (per-satellite reset spacing
 * of about 4 min at k=4) or walks metres away from the solve's self-consistent frame
 * (30.8 m measured with a loose v2 gate).  Per-satellite temporal stiffness is estimator
 * work (the guarded temporal anchor of Q3), not emission work.
 *
 * ## Guards (standing invariants #1 and #7)
 *
 * - **Leash**: when @f$|A_c|@f$ exceeds `network.clock_frame_leash_m` (default 50 m;
 *   measured 12 h wander 23 m), the constellation re-anchors to the solve frame.  The
 *   caller must pair the re-anchor with @ref NetworkProductEmitter::markDatumChange so
 *   the re-coupling edge is marked.  The leaf reports the set of re-anchored
 *   constellations for that purpose.
 * - **Forced keys** (pending per-satellite retro-bumps, datum-changed constellations)
 *   are excluded from the @f$\mu@f$ vote and the step test, so a genuine event that is
 *   already marked neither drags the frame nor is marked twice.
 * - **Gap and init keys** re-seed in the held frame with no step test (a re-appearance
 *   is a first sighting, matching the emitter's own semantics).  The exception is stage
 *   2b, which tests a returning key when `network.clock_disc_gap_jump_m` > 0.
 *
 * ## Scope and conventions
 *
 * Pure numeric leaf (like @ref NetworkEmitRebase and @ref NetworkPbEmitFilter), with no
 * dependency on the estimator or the emitter.  The caller (networkEmitStore) builds one
 * Entry per emitted clock satellite, calls @ref rebaseEpoch before @c emitToStore and
 * the on-disk write (the transform is baked into the product; replay never re-runs it),
 * routes @c marked entries to @c markSatDiscontinuity and the R-record set, and routes
 * @c reanchored constellations to @c markDatumChange.  It changes outputs only: the
 * solve is byte-identical even when armed.  @c tests/test_clock_emit_rebase.cpp drives
 * it directly with synthetic streams of known truth.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class NetworkClockEmitRebase
 * @brief Per-constellation common-mode frame hold and census-aligned per-satellite step
 *        marks for the emitted satellite clocks (card CLK-R).
 */
class NetworkClockEmitRebase {
public:
    /// Tunables (from `network.emit_clock_frame_hold` / `network.clock_disc_jump_*`;
    /// see GnssConfigLoadNetwork).
    struct Config {
        bool   frame_hold{false}; ///< Stage 1: hold the per-constellation common mode (emit in the held frame).
        double leash_m{50.0};     ///< Stage 1: |A_c| re-anchor bound (m).  Measured 12 h wander on the reference lineage: 23 m.
        bool   jump_test{false};  ///< Stage 2: mark per-satellite common-removed steps.  Independent of stage 1; alone it marks the raw wire.
        double jump_m{1.5};       ///< Stage 2: flat step threshold (m).  Empirical: about 16 σ of the per-epoch walk step (SPEC §1); the census outlier floor (0.5 m) would mark 10× as often.
        double max_gap_s{600.0};  ///< Key gap (s) beyond which a returning satellite is a first sighting (re-init, no step test).
        double gap_jump_m{0.0};   ///< Stage 2b (E1, `network.clock_disc_gap_jump_m`): on a return after more than max_gap_s, test the common-removed step against the retained pre-gap baseline with this threshold (m).  Requires jump_test.  0 = off: a stale return stays an untested first sighting, which leaves the steps of eclipse re-admissions (gaps of about 55 min) unmarked, see SPEC_synthetic_minimum_network §18.1; on non-Q3 postures nothing else marks those steps.  Designed for frame_hold lineages: with the hold off, the common wander over the gap is not drained, contaminates the statistic, and must be absorbed by this threshold.
        int    min_common{4};     ///< Minimum fresh unforced keys for a valid μ vote; below it the frame does not move this epoch.
    };

    /// One emitted satellite clock to re-base this epoch.
    struct Entry {
        std::string sat;       ///< RINEX satellite id ("G01").
        double      c0_m{0.0}; ///< IN: raw emitted clock (m).  OUT: the held-frame value the emitter and product must carry.
        bool        forced{false}; ///< A per-satellite retro-bump or constellation datum change is already pending for this satellite this epoch, so it is excluded from the μ vote and the step test (marked upstream; its step must not be marked twice or drag the frame).
        bool        marked{false}; ///< OUT: the stage-2 (or stage-2b) step test fired; the caller routes this satellite to markSatDiscontinuity and the R-record set.
        double      step_m{0.0};   ///< OUT: the common-removed step (m) when @ref marked, the event-row statistic; 0 otherwise.
        double      level_step_m{0.0}; ///< IN (card CLV): the satellite's declared level step since its previous emission (m), removed from its innovation before the frame vote and the step tests.  A level re-base moves both the clock and the phase biases, so it is neither a frame move nor a discontinuity.  0 unless network.clock_level_carry (byte-identical).
    };

    /// Running tallies for the run-end telemetry line (invariant #4).
    struct Counters {
        long long epochs{0};      ///< rebaseEpoch invocations with ≥1 entry.
        long long rows{0};        ///< entries processed.
        long long marks{0};       ///< stage-2 step marks reported.
        long long reanchors{0};   ///< stage-1 leash re-anchors (each paired with a datum mark by the caller).
        long long inits{0};       ///< first-sighting seeds.
        long long gaps{0};        ///< beyond-horizon re-inits.
        long long gap_marks{0};   ///< stage-2b step marks on a beyond-horizon return (gap_jump_m armed).
        double    sum_abs_mu{0.0};///< Σ|μ| drained into the frame accumulators (m): the churn removed from the wire.
        double    max_abs_acc{0.0};///< max |A_c| reached (m); the headroom against the leash.
    };

    explicit NetworkClockEmitRebase(Config cfg) : cfg_(cfg) {}
    NetworkClockEmitRebase() = default;

    /// (Re)configure.  The run context owns a default-constructed engine and
    /// configures it once from the loaded knobs before the epoch loop.
    void configure(const Config& cfg) { cfg_ = cfg; }

    /**
     * @brief The engine's cross-epoch frame state, exportable for the RTB-7
     *        snapshot (SPEC_synthetic_minimum_network §18.10 resume frame
     *        restore): the per-constellation accumulator A_c, the per-satellite
     *        previous held value and last-seen stamp, and the epoch count.
     *
     * Without it every resume restarts the held frame at zero, so the emitted
     * common mode steps by A_c and the F1 seam rule (keyed on
     * `counters().epochs == 0`) marks a datum change that the consumer must
     * re-absorb.  On a resume whose solve frame is otherwise continuous, that
     * mark would be the only re-realization on the wire.
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
    /// Restore an exported frame state (a warm resume that continues the held
    /// frame).  Counters other than @c epochs restart at zero: they are run
    /// telemetry, not frame state.
    void restoreFrameState(const FrameState& fs) {
        acc_ = fs.acc;
        prev_.clear();
        for (const auto& [sat, hp] : fs.prev) prev_[sat] = KeyState{hp.first, hp.second};
        counters_.epochs = fs.epochs;
    }

    /**
     * @brief Re-base one epoch's emitted clocks in place.
     *
     * Per constellation: form innovations of fresh unforced keys against the
     * previous emitted values, take the median as this epoch's common-mode
     * innovation μ, advance the frame accumulator (stage 1 armed), and emit
     * every entry in the held frame.  Stage 2 (armed) marks entries whose
     * common-removed step exceeds the threshold.  A leash crossing re-anchors
     * the constellation to the solve frame (raw passthrough, accumulator
     * cleared, no step test for that constellation this epoch) and reports it
     * in @p reanchored; the caller pairs each with a datum mark.  μ is computed
     * (and the step statistic valid) whichever stages are armed, so
     * `clock_disc_jump_test` alone marks the raw wire with the census's own
     * statistic.
     *
     * @param entries       This epoch's emitted clock satellites.
     * @param epoch_time_s  Continuous product time (week·604800+tow, s): the
     *                      per-key gap clock (deterministic on replay).
     * @param reanchored    OUT: constellation letters that re-anchored this
     *                      epoch (leash).  The caller must mark each
     *                      (markDatumChange); an unmarked re-anchor is the
     *                      unmediated re-coupling edge that card CLK-R closes.
     */
    void rebaseEpoch(std::vector<Entry>& entries, double epoch_time_s,
                     std::set<char>& reanchored);

    const Counters& counters() const { return counters_; }

    /// Frame-hold tallies of @ref Counters that are per constellation by
    /// construction (the accumulator, the μ vote and the leash are all keyed on the
    /// constellation letter), split by that letter for the multi-constellation
    /// run-end line (card GAL).  Filled on the same branches as @ref Counters, so the
    /// sums over letters equal the global values.
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
    /// One satellite's previous emitted value and last-seen time.
    struct KeyState { double last_held_m{0.0}; double last_seen_s{0.0}; };

    Config    cfg_;
    Counters  counters_;
    std::map<char, ConstCounters> const_counters_;   ///< per-constellation tallies (card GAL).
    std::map<char, double>          acc_;   ///< constellation -> frame accumulator A_c (m).
    std::map<std::string, KeyState> prev_;  ///< sat -> previous emitted (held-frame) value.
};

}  // namespace GnssProcessor
