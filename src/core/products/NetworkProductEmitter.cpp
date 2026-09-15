/**
 * @file NetworkProductEmitter.cpp
 * @brief Implementation of the PPP-RTK network product emitter (Sink A).
 */

#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/GnssTypes.hpp"
#include "common/SignalBand.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <vector>

namespace GnssProcessor {
namespace {

/// Map a RINEX satellite-ID prefix to its constellation (for the SSR header).
System systemFromSat(const std::string& sat) {
    if (sat.empty()) return System::Unknown;
    switch (sat[0]) {
        case 'G': return System::GPS;
        case 'R': return System::GLONASS;
        case 'E': return System::Galileo;
        case 'C': return System::BeiDou;
        case 'J': return System::QZSS;
        case 'S': return System::SBAS;
        default:  return System::Unknown;
    }
}

/// Split a serialized-state line on ','.  Keys in the emitter state ("sat",
/// "sat|signal", "const|signal") and its numeric fields never contain commas.
std::vector<std::string> splitStateLine(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(',', start);
        if (pos == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

/// Strict full-consumption double parse (round-trips fmt's shortest form).
bool parseStateD(const std::string& s, double& v) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && errno != ERANGE;
}

/// Card JT: median of a scratch vector (sorted IN PLACE — the caller owns it and
/// never reads the order).  Even population takes the mean of the two central
/// values; the caller guarantees a non-empty vector.  Deterministic on any build
/// (std::sort on doubles with no ties broken by address), which the replay
/// emitter's byte-identity depends on.
double medianInPlace(std::vector<double>& v) {
    const std::size_t n = v.size();
    std::sort(v.begin(), v.end());
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Strict full-consumption int parse.
bool parseStateI(const std::string& s, int& v) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long r = std::strtol(s.c_str(), &end, 10);
    v = static_cast<int>(r);
    return end == s.c_str() + s.size() && errno != ERANGE;
}

} // namespace

NetworkProductEmitter::NetworkProductEmitter(double disc_threshold_m, bool emit_clock_disc,
                                             double staleness_horizon_s,
                                             bool code_bias_jump_test,
                                             bool clock_disc_on_set_change,
                                             double disc_threshold_frac_cycles,
                                             bool disc_jump_test_decommon,
                                             bool disc_mark_scope_channel)
    : disc_threshold_m_(disc_threshold_m),
      disc_threshold_frac_cycles_(disc_threshold_frac_cycles),
      disc_jump_test_decommon_(disc_jump_test_decommon),
      disc_mark_scope_channel_(disc_mark_scope_channel),
      emit_clock_disc_(emit_clock_disc),
      staleness_horizon_s_(staleness_horizon_s),
      code_bias_jump_test_(code_bias_jump_test),
      clock_disc_on_set_change_(clock_disc_on_set_change) {}

const char* NetworkProductEmitter::discChannelName(DiscChannel c) {
    switch (c) {
        case kDiscChanClock:     return "clk";
        case kDiscChanCodeBias:  return "cb";
        case kDiscChanPhaseBias: return "pb";
        default:                 return "?";
    }
}

double NetworkProductEmitter::discThresholdM(const std::string& sat,
                                             const std::string& signal) const {
    // Legacy flat metre gate unless the per-λ fractional-cycle mode is armed.
    if (disc_threshold_frac_cycles_ <= 0.0) return disc_threshold_m_;
    // frac·λ(sat,signal): a FIXED contract — λ is a public, static band identity
    // (deterministic on producer and replay consumer).  λ resolves via SignalBand's
    // stateless classifier; on an unrecognized signal (λ == 0) fall back to the flat
    // gate so a stray code never disables the discontinuity test.
    const auto cls = classifySignalForSat(sat, signal);
    const double lambda = bandNominalWavelengthMeters(cls.band);
    return (lambda > 0.0) ? disc_threshold_frac_cycles_ * lambda : disc_threshold_m_;
}

void NetworkProductEmitter::markDatumChange(const std::set<char>& constellations) {
    if (constellations.empty()) force_all_bump_ = true;   // global (pre-A2 semantics)
    else forced_bump_consts_.insert(constellations.begin(), constellations.end());
}

void NetworkProductEmitter::markSatDiscontinuity(const std::set<std::string>& satellites,
                                                 unsigned channels) {
    forced_bump_sats_.insert(satellites.begin(), satellites.end());
    // Card MS: remember WHICH channels this site says moved.  Only under the
    // per-channel scope — the legacy scope never reads the map, so it is not
    // built there and that path allocates and behaves exactly as before.  Two
    // sites marking the same satellite in one epoch OR their masks, which is how
    // a site that genuinely moves two channels (the T3c δtˢ↔dˢ re-split, an
    // eph-cutover with an unknown Δ) declares both.
    if (!disc_mark_scope_channel_) return;
    const unsigned mask = (channels & kDiscChanAll) ? (channels & kDiscChanAll) : kDiscChanAll;
    for (const auto& s : satellites) forced_bump_chan_[s] |= mask;
}

std::map<char, NetworkProductEmitter::PbSeamConstTally>
NetworkProductEmitter::pbSeamCensus(const NetworkSolutionEpoch& sol) const {
    // Deliberately a read-only TWIN of the phase jump test in emitToStore rather than
    // a refactor of it: the test's three decisions (population, threshold, key
    // spelling) are reproduced here and nothing else, so this cannot alter a single
    // emitted byte on any path — the OFF run never calls it and the ON run calls it
    // strictly before emitToStore.  Keep the two in step: a change to the jump test's
    // population or threshold belongs here too.
    std::map<char, PbSeamConstTally> out;
    const double epoch_s = sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;
    for (const auto& s : sol.sats) {
        if (s.sat.empty() || s.phase_bias.empty()) continue;
        const char cst = s.sat[0];
        for (const auto& [signal, npb] : s.phase_bias) {
            const std::string key = s.sat + "|" + signal;
            const auto last_it = last_phase_bias_.find(key);
            if (last_it == last_phase_bias_.end()) continue;          // first sighting: nothing to compare
            if (staleness_horizon_s_ > 0.0) {                          // RTA-11 staleness: same probe
                const auto seen = phase_last_seen_s_.find(key);
                if (seen != phase_last_seen_s_.end()
                    && (epoch_s - seen->second) > staleness_horizon_s_)
                    continue;                                          // stale ⇒ the test skips it too
            }
            PbSeamConstTally& t = out[cst];
            ++t.keys_tested;
            const double d = std::abs(npb.bias_m - last_it->second);
            if (d > t.max_abs_delta_m) t.max_abs_delta_m = d;
            if (d > discThresholdM(s.sat, signal)) ++t.keys_over;
        }
    }
    return out;
}

int NetworkProductEmitter::discontinuityCounter(const std::string& sat,
                                                const std::string& signal) const {
    const auto it = disc_counter_.find(sat + "|" + signal);
    return it != disc_counter_.end() ? it->second : 0;
}

int NetworkProductEmitter::codeBiasDiscontinuityCounter(const std::string& sat,
                                                        const std::string& signal) const {
    const auto it = code_disc_counter_.find(sat + "|" + signal);
    return it != code_disc_counter_.end() ? it->second : 0;
}

int NetworkProductEmitter::clockDiscontinuityCounter(const std::string& sat) const {
    const auto it = clock_disc_counter_.find(sat);
    return it != clock_disc_counter_.end() ? it->second : 0;
}

void NetworkProductEmitter::adoptDiscontinuityCounters(
    const std::map<std::string, int>& phase,
    const std::map<std::string, int>& code,
    const std::map<std::string, int>& clock) {
    // RTA-4 tail-reader adoption: overwrite ONLY the counters with the file's
    // authoritative post-emit values.  Baselines / first-sighting markers /
    // satellite-set trackers stay as the preceding emitToStore seeded them, so
    // subsequent epochs step increments on top of the adopted values.
    for (const auto& [key, v] : phase) disc_counter_[key]       = v;
    for (const auto& [key, v] : code)  code_disc_counter_[key]  = v;
    for (const auto& [sat, v] : clock) clock_disc_counter_[sat] = v;
}

void NetworkProductEmitter::emitToStore(const NetworkSolutionEpoch& sol,
                                        SsrCorrectionStore& store) {
    // RTA-3: publish the solution-generation id to the store's epoch surface so an
    // in-process consumer can detect a producer restart (a CHANGE of a present id
    // = global datum break).  Unset (0) when the producer is not stamping ⇒ the
    // store field stays absent and the consumer never reacts (byte-identical).
    if (sol.generation_id != 0) store.setGenerationId(sol.generation_id);

    // RTA-11: product-time stamp of this epoch, and the per-channel staleness
    // probe.  A key whose last emit is further back than the horizon is treated as
    // a FIRST SIGHTING on its channel (baseline reseed, no jump/forced bump) — the
    // return after a multi-hour vanish is a first-sighting-after-gap, not a fault.
    // Product time only (deterministic under replay); maps are populated only when
    // the horizon is armed, so the OFF path is byte-identical.
    const double epoch_s = sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;
    auto staleKey = [&](const std::map<std::string, double>& last_seen,
                        const std::string& key) {
        if (staleness_horizon_s_ <= 0.0) return false;
        const auto it = last_seen.find(key);
        return it != last_seen.end() && (epoch_s - it->second) > staleness_horizon_s_;
    };

    // --- Code-bias discontinuity pre-pass (H1) ----------------------------
    // Build this epoch's per-(constellation, signal) satellite set and each
    // satellite's distinct-band tuple FROM THE SOLUTION ITSELF, so the datum
    // re-reference detection is reproducible on replay with nothing extra
    // serialized.  The emitted code_bias_m keys are exactly the producer's
    // registered (sat, signal) shared columns — the same set the per-(const,
    // signal) sum-to-zero and the per-sat IF pin are built over — so a set or
    // band-tuple change here is precisely a datum re-reference.  The
    // constellation key is the satellite ID's leading char, matching
    // NetworkSbasis::constellationOf used by the producer's sum-to-zero rows.
    std::map<std::string, std::set<std::string>> cur_code_satset;  // "const|signal" -> sats
    std::map<std::string, std::vector<char>>     cur_band_tuple;   // "sat" -> sorted distinct band digits
    for (const auto& s : sol.sats) {
        if (s.code_bias_m.empty() || s.sat.empty()) continue;
        const char cst = s.sat[0];
        std::set<char> bands;
        for (const auto& [signal, bias] : s.code_bias_m) {
            (void)bias;
            cur_code_satset[std::string(1, cst) + "|" + signal].insert(s.sat);
            if (signal.size() >= 2) bands.insert(signal[1]);  // RINEX band digit
        }
        cur_band_tuple[s.sat].assign(bands.begin(), bands.end());  // std::set<char> is sorted-unique
    }
    // Channels whose satellite set changed vs the previous emit.  First-sighting
    // channels (absent from prev) are intentionally excluded — no step yet.
    std::set<std::string> changed_channels;
    for (const auto& [chan, sats] : cur_code_satset) {
        const auto it = prev_code_satset_.find(chan);
        if (it != prev_code_satset_.end() && it->second != sats)
            changed_channels.insert(chan);
    }

    // --- Clock set-change pre-pass (sum-to-zero datum signalling) ---------
    // Under Σδtˢ=0 the clock gauge is re-imposed on each epoch's satellite set,
    // so a membership change redistributes the departing/arriving common-mode
    // onto every survivor — a genuine datum re-reference the pivot channel never
    // signals (there is no pivot).  Derived from the emitted epoch itself like
    // the code set trigger, so replay reproduces the counters with no new
    // serialization.  First-sighting constellations (absent from prev) are
    // excluded — no consumer holds their old datum yet.  State is only tracked
    // when the toggle is armed (OFF path byte-identical, including the
    // serialized emitter section).
    std::set<char> clock_set_changed;
    std::map<char, std::set<std::string>> cur_clock_satset;
    if (clock_disc_on_set_change_) {
        for (const auto& s : sol.sats)
            if (s.has_clock && !s.sat.empty()) cur_clock_satset[s.sat[0]].insert(s.sat);
        for (const auto& [cst, sats] : cur_clock_satset) {
            const auto it = prev_clock_satset_.find(cst);
            if (it != prev_clock_satset_.end() && it->second != sats)
                clock_set_changed.insert(cst);
        }
    }

    // --- Card JT: the per-SIGNAL phase-bias common mode (de-commoned jump test) --
    // Standing invariant #5: a satellite-COMMON phase-bias move is absorbed by a
    // consumer's own receiver phase clock, so the quantity that lands in a consumer
    // ambiguity is the step MINUS the network's median step on that signal.  The raw
    // test is frame-blind — CST §4 measured a −1.451 m L1W frame move hiding a
    // +1.409 m per-satellite step under a 0.0476 m bar.
    //
    // Population = exactly the jump test's own population, one epoch earlier: keys
    // carrying a baseline that is not stale under the staleness horizon (a first
    // sighting or a beyond-horizon return contributes nothing, mirroring staleKey).
    // The median is taken WITHIN each signal — L1W and L2W phase biases do not share
    // a common mode (the band-differential part is a different receiver state), and
    // one median over a mixed population leaks a band-differential move into every
    // cell.  Computed BEFORE the emit loop, i.e. against the baselines the loop is
    // about to overwrite.  Nothing here runs (and nothing is allocated) when the knob
    // is off, so the OFF path is byte-identical.
    std::map<std::string, double> pb_common_by_signal;   // signal -> median ΔPB (m)
    std::map<std::string, int>    pb_common_pop;         // signal -> population size
    pb_jump_decommon_events_.clear();
    // Card MS: per-emit scratch for the mark-scope census (both stay empty/zero
    // under the legacy satellite scope, so that path allocates nothing).
    mark_scope_events_.clear();
    mark_channel_tally_ = MarkChannelTally{};
    if (disc_jump_test_decommon_) {
        std::map<std::string, std::vector<double>> pool;   // signal -> ΔPB samples
        for (const auto& s : sol.sats) {
            if (s.sat.empty() || s.phase_bias.empty()) continue;
            for (const auto& [signal, npb] : s.phase_bias) {
                const std::string key = s.sat + "|" + signal;
                const auto last_it = last_phase_bias_.find(key);
                if (last_it == last_phase_bias_.end()) continue;          // first sighting
                if (staleKey(phase_last_seen_s_, key)) continue;          // stale baseline
                pool[signal].push_back(npb.bias_m - last_it->second);
            }
        }
        for (auto& [signal, samples] : pool) {
            pb_common_pop[signal] = static_cast<int>(samples.size());
            pb_common_by_signal[signal] = medianInPlace(samples);
        }
    }

    // Whether a constellation's datum was force-bumped this emit (A2 per-
    // constellation scoping).  An empty-arg markDatumChange sets force_all_bump_
    // (every constellation — the pre-A2 global bump); a scoped call bumps only the
    // named constellations.
    auto constForced = [&](char cst) {
        return force_all_bump_ || forced_bump_consts_.count(cst) != 0;
    };
    // Whether a specific SATELLITE was retro-bumped this emit (B1 markSatDiscontinuity):
    // a post-emit QC tier flagged its corrections as contaminated last epoch, so its
    // phase/code/clock counters step now — per-satellite, not constellation-wide.
    // Card MS: under the per-channel scope the mark applies only to the channels
    // the marking site named; a channel it did not name is SUPPRESSED, counted,
    // and gets a MARK_SCOPE event row (invariant #7's decoupling identity).  The
    // channel's own jump test is untouched and remains the backstop — a phase
    // bias that genuinely re-realizes under a clock-only mark is still declared
    // by the `value_jump` branch below.  Under the legacy scope the mask is never
    // consulted and this reduces to the original set lookup exactly.
    auto satForced = [&](const std::string& sat, DiscChannel chan) {
        if (forced_bump_sats_.count(sat) == 0) return false;
        if (!disc_mark_scope_channel_) return true;
        const auto it = forced_bump_chan_.find(sat);
        const unsigned mask = (it == forced_bump_chan_.end()) ? kDiscChanAll : it->second;
        if (mask & chan) {
            switch (chan) {
                case kDiscChanClock:     ++mark_channel_tally_.clk; break;
                case kDiscChanCodeBias:  ++mark_channel_tally_.cb;  break;
                case kDiscChanPhaseBias: ++mark_channel_tally_.pb;  break;
                default: break;
            }
            return true;
        }
        // One row per (satellite, suppressed channel) per emit, not per signal:
        // the CODE and PHASE channels are visited once per signal.  The vector is
        // at most 3 × (marked satellites) long, so the scan is free.
        const bool already =
            std::any_of(mark_scope_events_.begin(), mark_scope_events_.end(),
                        [&](const MarkScopeEvent& e) {
                            return e.suppressed == chan && e.sat == sat;
                        });
        if (!already) mark_scope_events_.push_back(MarkScopeEvent{ sat, mask, chan });
        return false;
    };

    for (const auto& s : sol.sats) {
        SsrHeader header;
        header.epoch_time  = sol.epoch_time;
        header.iod_ssr     = sol.iod_ssr;
        header.provider_id = sol.provider_id;
        header.solution_id = sol.solution_id;
        header.system      = systemFromSat(s.sat);

        if (s.has_clock) {
            SsrClockCorrection rec;
            rec.sat           = s.sat;
            rec.c0            = s.clock_c0_m;
            rec.c1            = s.clock_c1_m_s;   // B3b: two-state clock-model drift (0 when clock_model off ⇒ ZOH)
            rec.c2            = 0.0;
            rec.received_time = sol.epoch_time;
            rec.epoch_time    = sol.epoch_time;
            rec.iod_ssr       = sol.iod_ssr;
            // RTF-2 (Card 4 plumbing): carry the per-sat broadcast-anchor IODE +
            // RTS-orbit echo VALUES from the solution into the wire struct so the
            // live closed loop AND file replay stamp the store identically.  All
            // three default to the "unstamped" sentinel (-1 / -1 / {0,0}) in sp3
            // mode (Card 5 populates them in brdc_rts), so getOrbitClock's clock
            // gate stays byte-identical (a standalone clock's clock_iode inherits
            // the paired orbit anchor — Card 3).
            rec.iode          = s.clock_iode;
            rec.orbit_iod_ssr = s.orbit_iod_ssr;
            rec.orbit_epoch   = s.orbit_epoch;
            // M4 (contract v2): carry the per-sat combined CODE-correction σ + code/phase
            // correlation into the store record so the live closed loop AND file replay
            // stamp it identically.  Both default 0 (unstamped) unless
            // network.emit_combined_sigma populated them ⇒ byte-identical store otherwise.
            rec.combined_code_sigma_m = s.sigcc_m;
            rec.code_phase_corr       = s.rho;

            // Clock discontinuity bookkeeping (A2): bump on a scoped datum change
            // for THIS satellite's constellation (a clock-pivot handover).  First
            // sighting records only (counter stays 0) so the consumer's
            // noteSsrDiscontinuity reacts only to CHANGES.  Gated on emit_clock_disc_
            // so a pre-A2 store is byte-identical (rec.disc_counter stays 0).
            if (emit_clock_disc_) {
                int&       cctr        = clock_disc_counter_[s.sat];  // value-inits to 0
                const bool seen_before = seen_clock_sats_.count(s.sat) != 0;
                // RTA-11 fix: the clock counter has ONLY datum-change components
                // (no magnitude jump test), so a genuine clock datum move — a
                // pivot handover / re-declaration, or under the sum-to-zero datum
                // a constellation clock-set change (clock_disc_on_set_change) —
                // must still step it even when the satellite returns beyond the
                // staleness horizon: a consumer that kept its state across the
                // gap needs the reset.  (staleKey only exists to suppress the
                // SPURIOUS jump test on the phase/code channels.)
                if (seen_before && !s.sat.empty()
                    && (constForced(s.sat[0]) || satForced(s.sat, kDiscChanClock)
                        || clock_set_changed.count(s.sat[0]) != 0)) ++cctr;
                seen_clock_sats_.insert(s.sat);
                if (staleness_horizon_s_ > 0.0) clock_last_seen_s_[s.sat] = epoch_s;
                rec.disc_counter = cctr;
            }

            store.pushClock(rec, header);

            // B1: per-satellite URA quality channel (EWMA(σ̂0)×formal clock σ, computed
            // by the driver).  Only pushed when the producer populated it (emit_ura on);
            // absent otherwise so the store is byte-identical.
            if (s.has_ura) {
                SsrUra ura;
                ura.sat           = s.sat;
                ura.ura_m         = s.clock_ura_m;
                ura.received_time = sol.epoch_time;
                ura.iod_ssr       = sol.iod_ssr;
                store.pushUra(ura, header);
            }
        }

        if (!s.code_bias_m.empty()) {
            SsrCodeBias rec;
            rec.sat           = s.sat;
            rec.received_time = sol.epoch_time;
            rec.iod_ssr       = sol.iod_ssr;

            // The per-satellite ionosphere-free pin re-references when this
            // satellite's distinct-band set changes (a band drops, or it falls
            // to a single distinct frequency → the all-ones gauge), even with
            // NO (const,signal) membership change → step all its code biases.
            const auto bt_it = prev_band_tuple_.find(s.sat);
            const bool sat_band_changed =
                (bt_it != prev_band_tuple_.end() && cur_band_tuple[s.sat] != bt_it->second);

            for (const auto& [signal, bias] : s.code_bias_m) {
                const std::string key  = s.sat + "|" + signal;
                const std::string chan = std::string(1, s.sat[0]) + "|" + signal;

                // Bump on a forced (clock-pivot) datum change, a per-(const,
                // signal) sum-to-zero set re-reference, or this satellite's IF-
                // pin band-set change.  First sighting records the bias and
                // leaves the counter at 0 — the consumer's noteSsrDiscontinuity
                // reacts only to CHANGES, so a steady counter never triggers.
                int&       ctr           = code_disc_counter_[key];  // value-inits to 0 on first sight
                const auto cb_last_it    = last_code_bias_.find(key);
                const bool have_baseline = (cb_last_it != last_code_bias_.end());
                const bool fresh         = have_baseline && !staleKey(code_last_seen_s_, key);
                const bool forced        = (!s.sat.empty() && constForced(s.sat[0]))
                                           || satForced(s.sat, kDiscChanCodeBias);  // B1 retro-bump (per-sat, card MS: code channel)
                // Opt-in magnitude test (network.code_bias_disc_jump_test): the
                // phase jump test applied to code, catching the weak-separation
                // VALUE wander the structural triggers cannot see (they detect
                // gauge re-references only, not the estimate walking).  Same
                // fresh-baseline suppression as phase.
                const bool value_jump = code_bias_jump_test_ && have_baseline
                    && std::abs(bias - cb_last_it->second) > discThresholdM(s.sat, signal);
                // RTA-11 fix: a genuine forced datum change steps the counter even
                // on a beyond-horizon return (the consumer may still hold
                // old-datum state), but the STRUCTURAL re-reference signals
                // (per-(const,signal) sum-to-zero channel change, IF-pin band-set
                // change) and the opt-in value-jump test are suppressed on a stale
                // baseline — exactly like the phase jump test — since they are
                // computed against stale state.
                if (have_baseline && (forced
                        || (fresh && (changed_channels.count(chan) || sat_band_changed
                                      || value_jump)))) {
                    ++ctr;
                }
                last_code_bias_[key]        = bias;
                if (staleness_horizon_s_ > 0.0) code_last_seen_s_[key] = epoch_s;
                rec.bias_per_signal[signal] = bias;
                rec.disc_per_signal[signal] = ctr;
            }
            store.pushCodeBias(rec, header);
        }

        if (!s.phase_bias.empty()) {
            SsrPhaseBias rec;
            rec.sat           = s.sat;
            rec.received_time = sol.epoch_time;
            rec.iod_ssr       = sol.iod_ssr;
            for (const auto& [signal, npb] : s.phase_bias) {
                const std::string key = s.sat + "|" + signal;

                // Discontinuity bookkeeping (stream state the estimator can't
                // know): bump on the first jump beyond threshold, or on a forced
                // datum change.  First sighting records the bias and leaves the
                // counter at 0 — the consumer's noteSsrDiscontinuity only reacts
                // to CHANGES, so a steady counter never triggers a reset.
                int& ctr = disc_counter_[key];   // value-inits to 0 on first sight
                const auto last_it       = last_phase_bias_.find(key);
                const bool have_baseline = (last_it != last_phase_bias_.end());
                const bool fresh         = have_baseline && !staleKey(phase_last_seen_s_, key);
                const bool forced        = (!s.sat.empty() && constForced(s.sat[0]))
                                           || satForced(s.sat, kDiscChanPhaseBias); // B1 retro-bump (per-sat, card MS: phase channel)
                // RTA-11 fix: a genuine forced datum change (clock-pivot handover /
                // global re-declaration) steps the counter even on a beyond-horizon
                // return — a consumer with an unbroken carrier arc across the gap
                // still needs the reset signal.  The JUMP TEST, by contrast, is
                // suppressed on a stale baseline: an hours-stale comparison would
                // mint a spurious bump on the satellite's mere re-appearance.
                //
                // Card JT: when armed, the test screens the DE-COMMONED step; the
                // threshold, the population and the forced/fresh structure above are
                // untouched.  A cell already FORCED is declared regardless and is not
                // classified — the census counts only declarations this knob decides.
                bool value_jump = false;
                if (fresh) {
                    const double raw_d = npb.bias_m - last_it->second;
                    const double bar   = discThresholdM(s.sat, signal);
                    const bool   raw_over = std::abs(raw_d) > bar;
                    if (!disc_jump_test_decommon_) {
                        value_jump = raw_over;
                    } else {
                        const auto pop_it = pb_common_pop.find(signal);
                        const int  pop    = (pop_it != pb_common_pop.end()) ? pop_it->second : 0;
                        if (pop < kDiscDecommonMinSats) {
                            // A thin signal's median is not a frame: fall back to the
                            // RAW test, exactly as a missing baseline does, and count it.
                            value_jump = raw_over;
                            if (!forced) {
                                pb_jump_decommon_events_.push_back(
                                    {s.sat, signal, raw_d, 0.0, raw_d, bar, pop, value_jump,
                                     PbJumpDecommonClass::FallbackRaw});
                            }
                        } else {
                            const double c  = pb_common_by_signal[signal];
                            const double dd = raw_d - c;
                            value_jump = std::abs(dd) > bar;
                            if (!forced && (value_jump || raw_over)) {
                                const PbJumpDecommonClass cls =
                                    value_jump ? (raw_over ? PbJumpDecommonClass::Both
                                                           : PbJumpDecommonClass::DecommonOnly)
                                               : PbJumpDecommonClass::RawOnlySuppressed;
                                pb_jump_decommon_events_.push_back(
                                    {s.sat, signal, raw_d, c, dd, bar, pop, value_jump, cls});
                            }
                        }
                    }
                }
                if (have_baseline && (forced || value_jump)) {
                    ++ctr;
                }
                last_phase_bias_[key] = npb.bias_m;
                if (staleness_horizon_s_ > 0.0) phase_last_seen_s_[key] = epoch_s;

                SsrPhaseBiasEntry entry;
                entry.bias_m                = npb.bias_m;
                entry.signal_integer        = npb.signal_integer;
                entry.wide_lane_integer     = npb.wide_lane_integer;
                entry.discontinuity_counter = ctr;
                entry.sigma_m               = npb.sigma_m;   // B1b: 0 unless emit_phase_bias_sigma populated it (gauge-exact); no wire record, in-process store only.
                entry.combined_sigma_m      = npb.sigc_m;     // M4: combined phase-correction σ (clock+phasebias); 0 unless emit_combined_sigma populated it.
                rec.bias_per_signal[signal] = entry;
            }
            store.pushPhaseBias(rec, header);
        }
    }

    // Replace (never merge) the previous-epoch code-bias trackers with this
    // epoch's — a channel/satellite that vanishes then returns is a clean
    // first-sighting on both live and a fresh replay process, so the counters
    // reproduce deterministically.
    prev_code_satset_ = std::move(cur_code_satset);
    prev_band_tuple_  = std::move(cur_band_tuple);
    if (clock_disc_on_set_change_) prev_clock_satset_ = std::move(cur_clock_satset);

    // Forced-bump consumption (RT0-5 seam, P0-1 precedent: the emitter's stream
    // state must describe the last EMITTED product).  A constellation's forced
    // datum bump is consumed ONLY if at least one of its satellites was in THIS
    // epoch's emission; a constellation with ZERO satellites here (its sats were
    // stripped by the runaway quarantine, or all set below the horizon) keeps the
    // flag PENDING so the bump lands at the first emit where it reappears.
    // Consuming it now would lose the discontinuity signal entirely: none of the
    // constellation's (sat,signal) keys were processed above, so no counter
    // stepped, and on its return the consumer would integrate new-datum values
    // against old-datum ambiguity state with no notification.
    //
    // Replay-deterministic by construction: the pending state is a pure function
    // of (the emitted epoch sequence, the markDatumChange calls), and the product
    // file reproduces both — the E-line datum_consts are recorded on the TRIGGER
    // epoch (even for a constellation pruned from that epoch) and the replayed
    // epochs are the same pruned epochs, so a fresh replay emitter defers and
    // lands the bump on exactly the same epochs.  In the normal case (every
    // flagged constellation present) everything clears here, exactly as before.
    std::set<char> present_consts;
    for (const auto& s : sol.sats)
        if (!s.sat.empty()) present_consts.insert(s.sat[0]);
    if (force_all_bump_) {
        // The global (empty-arg) bump forces EVERY constellation.  A TRACKED
        // constellation (one with previously-emitted keys — i.e. a consumer may
        // hold old-datum state for it) that is absent from this epoch would lose
        // the bump, so demote it to the scoped pending set; it lands at its next
        // actual emit.  "Tracked" derives from the emitter's own emitted-key
        // state, itself a pure function of the emitted sequence (replay-safe).
        auto pendIfAbsent = [&](char cst) {
            if (present_consts.count(cst) == 0) forced_bump_consts_.insert(cst);
        };
        for (const auto& kv : last_phase_bias_)
            if (!kv.first.empty()) pendIfAbsent(kv.first[0]);
        for (const auto& kv : last_code_bias_)
            if (!kv.first.empty()) pendIfAbsent(kv.first[0]);
        for (const auto& sat : seen_clock_sats_)
            if (!sat.empty()) pendIfAbsent(sat[0]);
        force_all_bump_ = false;
    }
    for (auto it = forced_bump_consts_.begin(); it != forced_bump_consts_.end();) {
        if (present_consts.count(*it)) it = forced_bump_consts_.erase(it);   // landed above
        else ++it;                                                           // pending
    }
    // B1 retro-bump: consumed by this one emit.  The driver only marks satellites
    // present in the (post-quarantine) epoch — an absent flagged sat stays pending
    // DRIVER-side (ura_retro_sats) and is neither marked nor serialized in this
    // epoch's R record, so mark == land and replay re-marks exactly the landed sats.
    forced_bump_sats_.clear();
    forced_bump_chan_.clear();   // card MS: the mask map is kept in step with the set
}

void NetworkProductEmitter::serializeState(std::ostream& os) const {
    // Header carries the construction parameters so a restore into a
    // differently-configured emitter fails (belt-and-suspenders on top of the
    // snapshot's config fingerprint).  fmt "{}" = shortest round-trip double.
    // v1 (three parameters) is written whenever the two optional discontinuity
    // channels are OFF, so a default-config snapshot is byte-identical to
    // pre-channel builds and old snapshots restore into off-emitters; v2 appends
    // the two channel flags only when one is armed.
    if (disc_threshold_frac_cycles_ > 0.0) {
        // v3 = the v2 fields + the H3 per-λ fractional-cycle threshold.  Written
        // ONLY when the frac mode is armed, so a legacy (frac==0) snapshot stays v1
        // or v2 and is byte-identical to a pre-H3 build.
        os << "EMITTER,v3," << fmt::format("{}", disc_threshold_m_) << ','
           << (emit_clock_disc_ ? 1 : 0) << ','
           << fmt::format("{}", staleness_horizon_s_) << ','
           << (code_bias_jump_test_ ? 1 : 0) << ','
           << (clock_disc_on_set_change_ ? 1 : 0) << ','
           << fmt::format("{}", disc_threshold_frac_cycles_) << '\n';
    } else if (!code_bias_jump_test_ && !clock_disc_on_set_change_) {
        os << "EMITTER,v1," << fmt::format("{}", disc_threshold_m_) << ','
           << (emit_clock_disc_ ? 1 : 0) << ','
           << fmt::format("{}", staleness_horizon_s_) << '\n';
    } else {
        os << "EMITTER,v2," << fmt::format("{}", disc_threshold_m_) << ','
           << (emit_clock_disc_ ? 1 : 0) << ','
           << fmt::format("{}", staleness_horizon_s_) << ','
           << (code_bias_jump_test_ ? 1 : 0) << ','
           << (clock_disc_on_set_change_ ? 1 : 0) << '\n';
    }
    for (const auto& [k, v] : disc_counter_)       os << "ep," << k << ',' << v << '\n';
    for (const auto& [k, v] : last_phase_bias_)
        os << "epb," << k << ',' << fmt::format("{}", v) << '\n';
    for (const auto& [k, v] : clock_disc_counter_) os << "ecc," << k << ',' << v << '\n';
    for (const auto& s : seen_clock_sats_)         os << "ecs," << s << '\n';
    for (const auto& [k, v] : code_disc_counter_)  os << "ecd," << k << ',' << v << '\n';
    for (const auto& [k, v] : last_code_bias_)
        os << "ecb," << k << ',' << fmt::format("{}", v) << '\n';
    for (const auto& [chan, sats] : prev_code_satset_) {
        os << "ess," << chan << ',';
        bool first = true;
        for (const auto& s : sats) {
            if (!first) os << ';';
            os << s;
            first = false;
        }
        os << '\n';
    }
    for (const auto& [sat, bands] : prev_band_tuple_) {
        os << "ebt," << sat << ',';
        for (char b : bands) os << b;
        os << '\n';
    }
    // Clock set-change tracker (only populated when clock_disc_on_set_change_ is
    // armed, so a default-config section carries no eks lines).
    for (const auto& [cst, sats] : prev_clock_satset_) {
        os << "eks," << cst << ',';
        bool first = true;
        for (const auto& s : sats) {
            if (!first) os << ';';
            os << s;
            first = false;
        }
        os << '\n';
    }
    for (const auto& [k, v] : phase_last_seen_s_)
        os << "elp," << k << ',' << fmt::format("{}", v) << '\n';
    for (const auto& [k, v] : code_last_seen_s_)
        os << "elc," << k << ',' << fmt::format("{}", v) << '\n';
    for (const auto& [k, v] : clock_last_seen_s_)
        os << "elk," << k << ',' << fmt::format("{}", v) << '\n';
    // RT0-5/B1 pending forced-bump state — the deferred datum bumps that MUST
    // land after a resume.  efa is written unconditionally so an all-defaults
    // emitter still round-trips a non-empty section.
    os << "efa," << (force_all_bump_ ? 1 : 0) << '\n';
    if (!forced_bump_consts_.empty()) {
        os << "efc,";
        for (char c : forced_bump_consts_) os << c;
        os << '\n';
    }
    // Card MS: under the per-channel scope the mark's CHANNEL MASK is part of the
    // pending state — without it a resume would land a narrow mark as a wide one.
    // The third field is written ONLY under that scope, so a legacy (satellite-
    // scope) snapshot is byte-identical to a pre-MS build and restores into any.
    for (const auto& s : forced_bump_sats_) {
        os << "efs," << s;
        if (disc_mark_scope_channel_) {
            const auto it = forced_bump_chan_.find(s);
            os << ',' << (it == forced_bump_chan_.end()
                              ? static_cast<unsigned>(kDiscChanAll) : it->second);
        }
        os << '\n';
    }
    os << "EEND\n";
}

bool NetworkProductEmitter::restoreState(std::istream& is) {
    std::string line;
    if (!std::getline(is, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        const std::vector<std::string> h = splitStateLine(line);
        // v1 = the three legacy parameters (both optional channels implicitly
        // OFF); v2 appends the code-bias jump-test + clock set-change flags.
        const bool v1 = (h.size() == 5 && h[0] == "EMITTER" && h[1] == "v1");
        const bool v2 = (h.size() == 7 && h[0] == "EMITTER" && h[1] == "v2");
        const bool v3 = (h.size() == 8 && h[0] == "EMITTER" && h[1] == "v3");
        if (!v1 && !v2 && !v3) return false;
        double thr = 0.0, stale = 0.0, frac = 0.0;
        int clk = 0, cbj = 0, cks = 0;
        if (!parseStateD(h[2], thr) || !parseStateI(h[3], clk) || !parseStateD(h[4], stale))
            return false;
        if ((v2 || v3) && (!parseStateI(h[5], cbj) || !parseStateI(h[6], cks)))
            return false;
        if (v3 && !parseStateD(h[7], frac))
            return false;
        // Parameter mismatch = the snapshotting producer ran a different emitter
        // config than this one — the stream state would be interpreted under the
        // wrong rules (jump threshold, clock channel, staleness horizon, optional
        // discontinuity channels, per-λ fractional-cycle threshold).  Cold.
        if (thr != disc_threshold_m_ || (clk != 0) != emit_clock_disc_
            || stale != staleness_horizon_s_
            || (cbj != 0) != code_bias_jump_test_
            || (cks != 0) != clock_disc_on_set_change_
            || frac != disc_threshold_frac_cycles_)
            return false;
    }

    // Stage everything; commit only on a complete, well-formed section.
    std::map<std::string, int>    st_disc, st_clock_disc, st_code_disc;
    std::map<std::string, double> st_last_pb, st_last_cb;
    std::set<std::string>         st_seen_clock, st_forced_sats;
    std::map<std::string, unsigned> st_forced_chan;   // card MS: mark -> channel mask
    std::map<std::string, std::set<std::string>> st_satset;
    std::map<std::string, std::vector<char>>     st_band;
    std::map<char, std::set<std::string>>        st_clock_satset;
    std::map<std::string, double> st_seen_p, st_seen_c, st_seen_k;
    bool           st_force_all = false;
    std::set<char> st_forced_consts;
    bool saw_end = false, saw_efa = false;

    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "EEND") { saw_end = true; break; }
        const std::vector<std::string> f = splitStateLine(line);
        if (f.empty()) return false;
        const std::string& tag = f[0];
        int    iv = 0;
        double dv = 0.0;
        if (tag == "ep" || tag == "ecc" || tag == "ecd") {
            if (f.size() != 3 || !parseStateI(f[2], iv)) return false;
            (tag == "ep" ? st_disc : tag == "ecc" ? st_clock_disc : st_code_disc)[f[1]] = iv;
        } else if (tag == "epb" || tag == "ecb") {
            if (f.size() != 3 || !parseStateD(f[2], dv)) return false;
            (tag == "epb" ? st_last_pb : st_last_cb)[f[1]] = dv;
        } else if (tag == "ecs") {
            if (f.size() != 2) return false;
            st_seen_clock.insert(f[1]);
        } else if (tag == "ess") {
            if (f.size() != 3) return false;
            std::set<std::string>& sats = st_satset[f[1]];
            std::size_t start = 0;
            const std::string& v = f[2];
            while (start <= v.size()) {
                const std::size_t pos = v.find(';', start);
                const std::string one =
                    v.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
                if (!one.empty()) sats.insert(one);
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
            if (sats.empty()) return false;   // an ess line always carries ≥1 sat
        } else if (tag == "ebt") {
            if (f.size() != 3 || f[2].empty()) return false;
            st_band[f[1]] = std::vector<char>(f[2].begin(), f[2].end());
        } else if (tag == "eks") {
            if (f.size() != 3 || f[1].size() != 1) return false;
            std::set<std::string>& sats = st_clock_satset[f[1][0]];
            std::size_t start = 0;
            const std::string& v = f[2];
            while (start <= v.size()) {
                const std::size_t pos = v.find(';', start);
                const std::string one =
                    v.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
                if (!one.empty()) sats.insert(one);
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
            if (sats.empty()) return false;   // an eks line always carries ≥1 sat
        } else if (tag == "elp" || tag == "elc" || tag == "elk") {
            if (f.size() != 3 || !parseStateD(f[2], dv)) return false;
            (tag == "elp" ? st_seen_p : tag == "elc" ? st_seen_c : st_seen_k)[f[1]] = dv;
        } else if (tag == "efa") {
            if (f.size() != 2 || !parseStateI(f[1], iv) || (iv != 0 && iv != 1)) return false;
            st_force_all = (iv == 1);
            saw_efa = true;
        } else if (tag == "efc") {
            if (f.size() != 2 || f[1].empty()) return false;
            for (char c : f[1]) st_forced_consts.insert(c);
        } else if (tag == "efs") {
            // Card MS: 2 fields = a legacy (satellite-scope) mark; 3 = the mark
            // plus its channel mask.  Both accepted so a snapshot crosses the knob.
            if ((f.size() != 2 && f.size() != 3) || f[1].empty()) return false;
            st_forced_sats.insert(f[1]);
            if (f.size() == 3) {
                if (!parseStateI(f[2], iv) || iv <= 0
                    || iv > static_cast<int>(kDiscChanAll))
                    return false;
                st_forced_chan[f[1]] = static_cast<unsigned>(iv);
            }
        } else {
            return false;   // unknown tag: fail loudly, never guess
        }
    }
    if (!saw_end || !saw_efa) return false;   // truncated / incomplete section

    disc_counter_       = std::move(st_disc);
    last_phase_bias_    = std::move(st_last_pb);
    clock_disc_counter_ = std::move(st_clock_disc);
    seen_clock_sats_    = std::move(st_seen_clock);
    code_disc_counter_  = std::move(st_code_disc);
    last_code_bias_     = std::move(st_last_cb);
    prev_code_satset_   = std::move(st_satset);
    prev_band_tuple_    = std::move(st_band);
    prev_clock_satset_  = std::move(st_clock_satset);
    phase_last_seen_s_  = std::move(st_seen_p);
    code_last_seen_s_   = std::move(st_seen_c);
    clock_last_seen_s_  = std::move(st_seen_k);
    force_all_bump_     = st_force_all;
    forced_bump_consts_ = std::move(st_forced_consts);
    forced_bump_sats_   = std::move(st_forced_sats);
    forced_bump_chan_   = std::move(st_forced_chan);   // card MS (empty on a legacy snapshot)
    return true;
}

} // namespace GnssProcessor
