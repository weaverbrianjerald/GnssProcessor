/**
 * @file NetworkMeasurementAdmission.cpp
 * @brief Product-referenced measurement admission: the screen for corrupt channels.
 *        The header describes the statistic, the increments and the ordering
 *        contract.
 */

#include "filter/NetworkMeasurementAdmission.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"   // GnssTime (event stamps)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

namespace {

/// One screened code observable of one line of sight, before the station centre is
/// known.  @c obs_idx indexes the station's `modeled[i]` vector, so a verdict can
/// reach back to the observation whose band it must withhold.
struct AdmSample {
    std::size_t obs_idx;
    char        band;
    double      d;
};

/// One (satellite, band) channel's verdict input, after centring.
struct AdmChannel {
    std::size_t obs_idx;
    char        band;
    double      d;      ///< the raw term of the worst observable of this band.
    double      z;      ///< its centred value: the screen statistic.
    double      absz;
};

/// Median of a non-empty sample (even n -> mean of the two central order
/// statistics, matching every other robust centre on the network path).
/// @p v is reordered in place.
double medianOf(std::vector<double>& v) {
    const std::size_t n = v.size();
    const std::size_t m = n / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
    const double hi = v[m];
    if (n % 2) return hi;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m - 1),
                     v.begin() + static_cast<std::ptrdiff_t>(m));
    return 0.5 * (v[m - 1] + hi);
}

/// "SAT|<band>": the per-channel admission-record key.
inline std::string chanKey(const std::string& sat, char band) {
    return sat + "|" + std::string(1, band);
}

} // namespace

char measurementAdmissionBand(const std::string& key) {
    // RINEX-3 observation codes on the uncombined producer path are the raw
    // 3-character forms ("C1C", "L2W", …).  Anything else, notably the iono-free
    // path's "C1C_C2W" combination keys, has no single band to withhold and is not
    // screened.
    if (key.size() != 3) return '\0';
    const char b = key[1];
    return (b >= '1' && b <= '9') ? b : '\0';
}

bool measurementAdmissionD(const PppObservation& obs, const std::string& code_key,
                           double& d_out) {
    const auto o = obs.observed.find(code_key);
    if (o == obs.observed.end()) return false;
    const auto m = obs.modeled.find(code_key);
    if (m == obs.modeled.end()) return false;
    const double d = o->second - m->second;
    if (!std::isfinite(d)) return false;
    d_out = d;
    return true;
}

void networkMeasurementAdmission(NetworkRunContext& ctx,
                                 const GnssConfig& config_,
                                 const std::shared_ptr<spdlog::logger>& log,
                                 NetworkRunTraces& traces,
                                 const GnssTime& t,
                                 long long epoch_idx,
                                 std::vector<NetStation>& stations,
                                 const std::vector<bool>& present,
                                 std::vector<std::vector<PppObservation>>& modeled,
                                 std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                 std::set<std::pair<int, std::string>>& carry_slipped_raw,
                                 bool carry_invariant, bool carry_warm) {
    (void)config_;   // every mode predicate is already resolved into ctx.knobs
    if (!ctx.knobs.adm_on) return;   // off: no map, no event, no observable touched

    const bool   screen   = ctx.knobs.adm_screen;
    const bool   census   = ctx.knobs.adm_trace;
    const double bound    = ctx.knobs.adm_bound_m;
    const int    min_sats = ctx.knobs.adm_min_sats;
    const int    heal_ep  = ctx.knobs.adm_heal_ep;
    const double max_frac = ctx.knobs.adm_max_frac;
    // INC-4, the re-entry vet.  Both the loader and the knob snapshot already force
    // `adm_revet` off unless `adm_screen` is on.
    const bool   revet        = ctx.knobs.adm_revet;
    const int    revet_gap    = ctx.knobs.adm_revet_gap_ep;
    const int    revet_ep     = ctx.knobs.adm_revet_ep;
    const bool   revet_hold   = ctx.knobs.adm_revet_hold;
    // The tighter bound a channel answers to while it is re-entering or vetting
    // (adm_bound_m times network.adm_revet_bound_scale; 200 m by default).  The
    // default comes from a measurement (card §10.10): across two legs, 272 epochs,
    // 63 stations and 49 arc_resume cells screened at a 50 m census bound, no healthy
    // station's re-entry cell exceeded 50 m.  The re-entry population above 50 m was
    // BSHM00ISR alone, arriving at 670.3 m (G16), 347.6 m (G22) and 81.5 m (G13) on
    // the return epoch, while the healthy tail (all steady-state) peaked at 61.9 m.
    // 200 m therefore sits 3.2x above the largest legitimate |z| in that sample and
    // 3.3x below the lowest corrupt arrival it is meant to catch.
    //
    // It does not catch G13 at 81.5 m.  That is 1.3x the healthy maximum and no
    // absolute bound separates it without firing on COCO00AUS G24; G13 ramps slowly,
    // and the census trigger names it at return + 4.  The two mechanisms cover
    // disjoint failure shapes (fast ramp, large on arrival; slow ramp, small on
    // arrival) and neither subsumes the other, so both are needed.
    const double revet_bound  = ctx.knobs.adm_revet_bound_m;

    // The PREFIT census's obs_code exceedance set, and whether it is fresh.
    //
    // The census walk runs later in the epoch than this seam, so its flags are one
    // loop epoch old.  That latency is available: the census named each of the FT63
    // day's four exposed re-entries 1-4 epochs before their |z| reached this card's
    // bound.  Both seams read `n_loop_epochs`, which is incremented once per loop
    // epoch before either runs, so the `- 1` is exact.
    //
    // Freshness is a hard gate: a flag set from five epochs ago names cells that may
    // since have recovered, and acting on it would act on evidence the vet does not
    // have.  An unusable consultation is counted (`revet_census_absent`), so "the
    // census flagged nothing" and "the census was not consulted" stay
    // distinguishable (invariant #4 applied to a cross-seam read).
    const long long loop_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const bool revet_census =
        revet && ctx.knobs.adm_revet_census
        && ctx.carry.prefit_code_flag_ep == loop_ep - 1;
    const bool revet_census_wanted = revet && ctx.knobs.adm_revet_census;

    auto& ctr  = ctx.counters.adm;
    auto& arcs = ctx.carry.adm_arc;
    ++ctr.epochs;

    // The warm-resume restore rule (INC-1, second clause).  The record map is not
    // serialized, so on a resumed leg every channel looks new the first time its
    // station is screened, while its estimator state was restored and may carry the
    // corrupt seed; a failure there is a restore, not a birth.
    //
    // The latch is per station, not one global one-shot.  A global one-shot would be
    // consumed by the seam's first pass, which screens only the stations present and
    // above the satellite floor; a station that was absent, thin, or had its
    // satellite erased by the two-station observability gate on that epoch would
    // then read every later first sighting as a birth, mislabelling a restored
    // corrupt arc.  The response does not depend on this latch (see the refloat rule
    // below), so it only sets the label, but that label names the event class this
    // card exists to report.
    const bool resume_run = ctx.carry.adm_resume_pending;
    if (resume_run && ctr.epochs == 1) ++ctr.resume_passes;

    auto emit = [&](const char* event, const std::string& sat,
                    const std::string& station, double statistic, const char* action,
                    const std::string& detail) {
        traces.emitEvent(t, "ADMISSION", event, sat, station, statistic, bound,
                         action, detail);
        ++ctr.events;
    };
    // Per-(event, station, satellite, band) rate limit for the two streams that can
    // repeat: the INC-3 census and a chronically bad station's guard event.  The
    // INC-1/INC-2 events are transitions (a birth, an onset, a heal) and do not
    // consult it.  This duplicates the prefit census's `holdoffAllows`, which is
    // file-local to its translation unit (the redundancy census set the same
    // precedent).
    auto holdoff_ok = [&](const char* event, const std::string& station,
                          const std::string& sat, char band) {
        const std::string k = fmt::format("{}|{}|{}|{}", event, station, sat, band);
        auto it = ctx.carry.adm_event_last.find(k);
        if (it != ctx.carry.adm_event_last.end()
            && epoch_idx - it->second < kAdmEventHoldoffEp) {
            ++ctr.events_held;
            return false;
        }
        ctx.carry.adm_event_last[k] = epoch_idx;
        return true;
    };

    // Every channel touched this epoch, so the ageing sweep can tell a channel that
    // was not screened this epoch from one whose record has gone stale.
    std::set<std::pair<int, std::string>> seen;

    // INC-3 per-epoch census accumulators.
    std::vector<double> station_worst;   // each screened station's worst |z|
    double      net_worst = 0.0;
    std::string net_worst_id;

    for (std::size_t i = 0; i < stations.size(); ++i) {
        // The gate is `present[i]`, not `modeled[i].empty()`: a PASS-A station that
        // threw after computeModels leaves a populated `modeled[i]` behind a false
        // presence flag, and screening it would produce verdicts for a station that
        // folds no rows (networkSignalIntegrity makes the same check for the same
        // reason).
        if (i >= present.size() || !present[i]) continue;
        if (i >= modeled.size()) continue;
        NetStation& st = stations[i];
        if (!st.sm) continue;
        const int si = static_cast<int>(i);

        // ---- (1) gather the raw terms ------------------------------------
        // One entry per code observable.  Not reduced per band yet: the reduction
        // keeps the worst |z|, and z is undefined until the station's centre is
        // known.  Built from the post-prefilter observation set, so a satellite the
        // two-station observability gate has just erased contributes nothing to the
        // median and raises no verdict.
        std::vector<AdmSample> samples;
        std::map<std::size_t, std::vector<double>> per_sat;   // obs_idx -> its code terms
        samples.reserve(modeled[i].size() * 2);
        for (std::size_t j = 0; j < modeled[i].size(); ++j) {
            const PppObservation& obs = modeled[i][j];
            if (obs.satellite.empty()) continue;
            ++ctr.los_epochs;
            for (const auto& [key, val] : obs.observed) {
                (void)val;
                if (key.empty() || key[0] != 'C') continue;   // code only, by design
                const char band = measurementAdmissionBand(key);
                if (band == '\0') continue;
                double d = 0.0;
                if (!measurementAdmissionD(obs, key, d)) { ++ctr.skips_nonfinite; continue; }
                samples.push_back(AdmSample{j, band, d});
                per_sat[j].push_back(d);
            }
        }

        // The centre is the median over satellites: one representative per line of
        // sight, itself the median of that satellite's code terms.  The two-level
        // reduction matters: a flat median over observables weights a satellite by
        // the number of bands it is tracked on, so one satellite tracked on four bands
        // out-votes three tracked on one and can set the centre.  The screen then
        // inverts (the corrupt satellite reads z ~ 0 and is admitted, its healthy
        // peers read z ~ |d| and are rejected), and the station guard cannot catch
        // it: the guard fires only when failing bands outnumber passing ones, while
        // the capture needs the corrupt bands to outnumber the healthy ones, and in
        // the inverted screen the failing bands are the healthy ones, so the two
        // conditions exclude each other.  The case is reachable with the repository's
        // shipped network configs (GPS+Galileo, frequency_mode "all", channel_mode
        // "all") at any station near the satellite floor.  Reducing per satellite
        // first also makes `adm_min_sats` bound the population it names: the median's
        // sample size is the satellite count, as spec section 2 defines it ("median
        // over the station's sats, n >= 4").
        std::vector<double> sat_med;
        sat_med.reserve(per_sat.size());
        for (auto& [idx, v] : per_sat) {
            (void)idx;
            sat_med.push_back(medianOf(v));
        }

        // A median of two or three values can be the outlier it is meant to remove.
        // Below the floor the station is skipped entirely and the skip is counted;
        // it is never treated as a centre of zero and never causes a rejection.
        if (static_cast<int>(sat_med.size()) < min_sats) {
            ++ctr.skips_thin;
            continue;
        }
        ++ctr.station_epochs;
        const std::size_t n_sats = sat_med.size();
        const double med = medianOf(sat_med);
        // True on this station's first screened epoch since a warm resume, the pass
        // on which its restored arcs appear as first sightings.
        const bool sta_resume_pass =
            resume_run && ctx.carry.adm_resume_seen.insert(si).second;

        // ---- (2) centre, and reduce to one verdict per (satellite, band) --
        // Several code observables can share a band (C1C and C1W); they are one
        // physical channel, so the band's verdict is that of its worst observable.
        std::map<std::pair<std::size_t, char>, AdmChannel> chans;
        for (const AdmSample& s : samples) {
            const double z = s.d - med;
            const double a = std::abs(z);
            const auto key = std::make_pair(s.obs_idx, s.band);
            auto it = chans.find(key);
            if (it == chans.end())
                chans.emplace(key, AdmChannel{s.obs_idx, s.band, s.d, z, a});
            else if (a > it->second.absz)
                it->second = AdmChannel{s.obs_idx, s.band, s.d, z, a};
        }
        ctr.channels += static_cast<long long>(chans.size());

        // ---- (3) the census statistics + the station guard's fraction -----
        // The fraction is computed before any verdict is acted on: if the centre is
        // untrusted, no per-channel verdict derived from it means anything, and the
        // guard refuses those verdicts outright instead of filtering them afterwards.
        long long n_fail = 0;
        std::set<std::size_t> fail_sats;   // distinct satellites with a failing band
        double      sta_worst = 0.0;
        std::string sta_worst_id;
        for (const auto& [k, c] : chans) {
            (void)k;
            if (c.absz > bound) { ++n_fail; fail_sats.insert(c.obs_idx); }
            if (c.absz > sta_worst) {
                sta_worst = c.absz;
                sta_worst_id = fmt::format("{}|{}|{}", st.name,
                                           modeled[i][c.obs_idx].satellite, c.band);
            }
        }
        ctr.over_bound += n_fail;
        station_worst.push_back(sta_worst);
        if (sta_worst > net_worst) { net_worst = sta_worst; net_worst_id = sta_worst_id; }
        if (sta_worst > ctr.max_z) {
            ctr.max_z     = sta_worst;
            ctr.max_z_tow = t.tow;
            ctr.max_z_id  = sta_worst_id;
        }

        // The guard asks whether the centre is trustworthy, so its denominator is the
        // population that formed the centre: satellites, one vote each.  Spec §3
        // writes "a station's screened bands"; with a band denominator the guard
        // fires on a single bad satellite tracked on many bands (4 of 7 channels at a
        // 4-satellite station can be one corrupt Galileo line of sight, not a corrupt
        // station), holds the whole station, and refuses per-channel verdicts that
        // were sound.  Counting satellites makes the test match §3's own
        // justification: the median is untrusted when most of what voted on it
        // disagrees with it.  The deviation is recorded in the card's §9.
        const bool station_event =
            screen && n_sats > 0
            && static_cast<double>(fail_sats.size())
                   > max_frac * static_cast<double>(n_sats);

        // ---- (4) the response --------------------------------------------
        StateManager& sm = *st.sm;
        std::set<std::string> withheld_pb;   // "sat|signal" phase-bias identities
        std::set<std::string> withheld_cb;   // sat-code-bias state keys
        std::set<std::string> emptied_sats;  // satellites left with no observable at all
        bool prune_states = false;

        // Erase every observable of one band, refloat the arcs behind the hole, and
        // count both.
        //
        // The refloat is unconditional, including at a birth, where spec §3 says
        // "no state exists yet … pure admission".  That holds, and it is why the
        // re-tap costs nothing there: inserting a fresh arc's key into the slip
        // funnel erases a DD chord and a GM deviation that do not exist, and marking
        // a not-yet-allocated ambiguity pending-reseed is a set insert that PASS B
        // consumes when it allocates.
        //
        // A conditional refloat would be a defect.  Birth versus restore is decided
        // from the seam's own record map, which is not serialized and has no entry
        // for a station that was absent, thin or satellite-pruned on an earlier
        // epoch.  A restored corrupt arc carrying a stiff Card-4 chord could then
        // appear as a first sighting, be labelled a birth, and have its band erased
        // while the chord carries across the hole, violating standing invariant #1
        // (the Card 4 slip-hole failure).
        // Correctness must not depend on a classification that can be wrong when the
        // alternative costs nothing.
        auto withhold_band = [&](std::size_t obs_idx, char band) {
            PppObservation& obs = modeled[i][obs_idx];
            std::vector<std::string> phase_keys, code_keys;
            for (auto it = obs.observed.begin(); it != obs.observed.end();) {
                if (measurementAdmissionBand(it->first) != band) { ++it; continue; }
                if (it->first[0] == 'L')      phase_keys.push_back(it->first);
                else if (it->first[0] == 'C') code_keys.push_back(it->first);
                // raw_observed is left untouched: it belongs to the slip detectors,
                // which have already run this epoch and must keep seeing the
                // receiver's true tracking state.
                it = obs.observed.erase(it);
            }
            const std::size_t n_removed = phase_keys.size() + code_keys.size();
            if (n_removed == 0) return;
            ++ctr.withheld_channels;
            ctr.withheld_obs += static_cast<long long>(n_removed);

            // Prune the now-orphaned shared states, as the two-station observability
            // gate in networkAssemblyPrefilter_ does when it removes observations
            // ("Prune the now-orphaned shared states").  Three later consumers
            // derive from the states rather than from the observables, and each
            // needs the prune:
            //   * the anchor election reads the phase-bias states, so an unpruned
            //     one could be seated as a datum pin whose supporting row this seam
            //     has removed; the pin then folds against nothing and the gauge
            //     loses a constraint without any signal;
            //   * the code-bias registration counts stations holding a state against
            //     its >= 2 coverage gate, so an unpruned one falsely supports a
            //     shared OSB column that only one station feeds (a single-station
            //     code column is confounded with that receiver's clock and goes
            //     near-null); and
            //   * `registerClock` unions the satellite clock states, so a satellite
            //     left with no observable at all here must stop claiming to observe
            //     it (recorded per satellite and applied only when the observation
            //     is emptied, since a satellite that keeps another band still
            //     supports the column).
            // Arc-ledger reason for the withhold (Card SID phase 4a, S1).  The
            // withhold removes the band's observables from `modeled`, the set the
            // admitted lifecycle is fed from, so this screen opens an admitted gap
            // and the identity is exact here: the station index, the satellite and
            // the erased signals.  Recorded per phase key, because a phase arc is
            // what the ledger tracks; the code keys of the same band go with it and
            // are not separate arcs.
            for (const std::string& b : phase_keys)
                arcReasonRecord(ctx.reasons, ArcLedger::Key{ si, obs.satellite, b },
                                kArcReasonAdmission);
            for (const std::string& b : phase_keys) {
                withheld_pb.insert(obs.satellite + "|" + b);
                prune_states = true;
            }
            for (const std::string& b : code_keys) {
                withheld_cb.insert(StateKeys::makeSatBiasKey(obs.satellite, b));
                prune_states = true;
            }
            if (obs.observed.empty()) {
                emptied_sats.insert(obs.satellite);
                prune_states = true;
            }
            // Reset, never repair.  Nothing bounds the error on a corrupt channel, so
            // there is no value to correct toward, only state to discard.  The
            // insertion uses the same funnel as the MW / LLI / IF-TD detections; a
            // parallel reset path would be a second definition of "this arc re-taps
            // fresh".  Both feeds are written and counted separately: a bare
            // `markAmbiguityPendingReseed` is a one-shot that PASS B consumes and the
            // leveling harvest clears, so on a lineage without warm leveling it is
            // not a refloat at all (NetworkIonoDwRelease.hpp describes that case).
            for (const std::string& b : phase_keys) {
                const std::string amb = StateKeys::makeAmbiguityKey(obs.satellite, b);
                if (carry_invariant) {
                    // Also the raw provenance set: a withheld band is a raw
                    // observable event (the channel leaves the model set), so the
                    // Iono-2 v3k erase-policy split must erase this line of sight's
                    // GM deviation rather than D5-keep a level carried across the
                    // hole this seam made.
                    carry_slipped_raw.insert({si, amb});
                    if (carry_slipped_ambs.insert({si, amb}).second) ++ctr.refloat_ambs;
                }
                if (carry_warm) {
                    sm.markAmbiguityPendingReseed(amb);
                    ++ctr.refloat_reseeds;
                }
            }
        };

        if (station_event) {
            // The IF-TD reference gate's incoherent-station rule (FireNoRebase in
            // IfPhaseSlipDetector.cpp): when a family disagrees with its own centre,
            // the centre is what is untrustworthy, so act at the family's scope and
            // make no per-member claim.  The station's screened bands are held, the
            // arcs behind the holes are refloated, one event is emitted, and every
            // admission record is left untouched, so the next epoch re-tests from
            // evidence rather than from a verdict this one could not support.  No heal
            // counter advances either: a verdict from a corrupted centre is not
            // evidence in either direction.
            ++ctr.station_events;
            ctr.station_held_channels += static_cast<long long>(chans.size());
            for (const auto& [k, c] : chans) {
                (void)k;
                withhold_band(c.obs_idx, c.band);
            }
            if (holdoff_ok("adm_station_event", st.name, std::string(), '0'))
                emit("adm_station_event", std::string(), st.name, sta_worst,
                     "hold_station",
                     fmt::format("fail_sats={}/{};frac={:.3f};fail_bands={}/{};"
                                 "med={:.3f};worst={}",
                                 fail_sats.size(), n_sats,
                                 static_cast<double>(fail_sats.size())
                                     / static_cast<double>(n_sats),
                                 n_fail, chans.size(), med, sta_worst_id));
        } else {
            for (const auto& [k, c] : chans) {
                (void)k;
                const std::string& sat = modeled[i][c.obs_idx].satellite;
                const std::string ck = chanKey(sat, c.band);
                seen.insert({si, ck});
                const bool fail = c.absz > bound;
                // The tighter re-entry bound, evaluated separately from `fail` and
                // never substituted for it.  `fail` is this card's absolute admission
                // verdict and also defines the INC-3 census's domain: §7 gate 5
                // grades on "enumerates every |z| over the bound on the day", which is
                // a statement about `adm_bound_m`.  Folding a vet-window threshold into
                // it would change what the census means for the ~0.3 % of
                // channel-epochs that are vetting, and make the measure-only arm's
                // numbers depend on a response knob.
                const bool fail_vet = revet && c.absz > revet_bound;
                // Built on demand: emitEvent does not short-circuit its arguments
                // (its only guard is inside the callee), so formatting a detail for
                // every channel of every station of every epoch would be paid in
                // full on a run with trace_events off.
                auto detail = [&] {
                    return fmt::format("band={};d={:.3f};med={:.3f};n_sats={}",
                                       c.band, c.d, med, n_sats);
                };
                bool evented = false;

                if (screen) {
                    auto it = arcs.find({si, ck});
                    const bool first = (it == arcs.end());
                    if (first) it = arcs.emplace(std::make_pair(si, ck),
                                                 MeasurementAdmissionArc{}).first;
                    MeasurementAdmissionArc& a = it->second;
                    // The previous screened epoch, captured before it is overwritten.
                    // `last_ep` advances only on epochs this seam screened the
                    // channel, so the gap counts absence from the model set as this
                    // seam sees it: a satellite the two-station observability gate
                    // erased, or a station under the satellite floor, reads as absent.
                    // That is the conservative direction: it re-screens.
                    const long long prev_ep = first ? -1 : a.last_ep;
                    const long long gap     = (prev_ep < 0) ? 0
                                                            : epoch_idx - prev_ep - 1;
                    a.last_ep = epoch_idx;

                    // INC-4.  A re-entry is a channel whose record survives but whose
                    // arc did not: it has been absent for at least `adm_revet_gap_ep`
                    // epochs, so its `admitted` verdict was made about a channel that
                    // is not necessarily the one now present.  It is re-screened as a
                    // birth rather than inheriting that verdict.
                    //
                    // This is the same defect class as §9.5's warm-resume latch, one
                    // level down: there the record map does not survive the boundary
                    // and a restored arc reads as a birth; here the record survives a
                    // boundary the arc did not, and a re-entry would read as an
                    // established arc.  Both give a stale answer to "is this the
                    // channel the stored verdict is about?", and both are handled the
                    // same way: the verdict is re-derived from this epoch's evidence.
                    const bool reentry = revet && !first && gap >= revet_gap;

                    if (first || reentry) {
                        if (reentry) {
                            // Open the vet window.  `births` is not incremented: it is
                            // the admission-opportunity denominator that the two birth
                            // rejection counts of §7 gate 2 are read against, and
                            // folding re-entries into it would move that gate's
                            // baseline.  Re-entries have their own counter.
                            ++ctr.reentries;
                            a.vet_left = revet_ep;
                        } else {
                            ++ctr.births;
                        }
                        // A re-entry answers to the tighter bound; a true birth does
                        // not.  The asymmetry follows the measurement: the §10.10
                        // population is "cells within 8 epochs of an arc_resume", and
                        // nothing supports extending a 200 m bound to a first-ever
                        // sighting, where the arc has no history to be suspicious of
                        // and the rising-satellite population is largest.
                        const bool refuse = fail || (reentry && fail_vet);
                        if (refuse) {
                            a.admitted  = false;
                            a.clean_run = 0;
                            withhold_band(c.obs_idx, c.band);
                            // `reentry` is tested first although it cannot co-occur
                            // with `sta_resume_pass` (a resumed run's map is empty, so
                            // every channel on a station's first screened epoch is
                            // `first` and no re-entry is reachable there).  Testing it
                            // first keeps the three labels mutually exclusive by
                            // construction rather than by a reachability argument that
                            // a later change could invalidate.
                            if (reentry) {
                                // Same response as the onset path, with a label that
                                // does not claim that a previously good channel went
                                // bad, which the evidence does not support.
                                // The bound that made the call is recorded, because the
                                // two mean different things: `bound` says the channel
                                // is in the 1e5 m corrupt class outright, `vet_bound`
                                // says it arrived outside every healthy re-entry in the
                                // §10.10 sample.  Only the latter is a claim this
                                // increment makes on its own evidence.
                                ++ctr.reject_reentry;
                                if (!fail) ++ctr.revet_bound_refusals;
                                emit("adm_reject_reentry", sat, st.name, c.z, "withhold",
                                     fmt::format("{};gap_ep={};trigger={}", detail(), gap,
                                                 fail ? "bound" : "vet_bound"));
                            } else if (sta_resume_pass) {
                                // A restored corrupt arc: its state crossed the snapshot
                                // boundary although this seam's record map did not.
                                ++ctr.reject_resume;
                                emit("adm_reject_resume", sat, st.name, c.z, "withhold",
                                     detail());
                            } else {
                                // Pure admission: nothing has been seeded, so nothing
                                // estimable is lost and nothing needs re-tapping.
                                ++ctr.reject_birth;
                                emit("adm_reject_birth", sat, st.name, c.z, "withhold",
                                     detail());
                            }
                            evented = true;
                        } else if (reentry && revet_hold) {
                            // Strict posture.  One in-bound epoch is thin evidence about
                            // a channel that has been away: on the FT63 day, G16@BSHM
                            // and G13@BSHM passed the bound on their return epoch,
                            // crossed it 2 and 5 epochs later, and were folded at full
                            // weight in between.  Holding the channel until the heal
                            // path readmits it removes that window.
                            //
                            // Off by default because it withholds every re-entry
                            // (~4650 on a 63-station day), a population dominated by
                            // legitimately rising satellites.  Admitting every rising
                            // satellite 5 epochs late costs observability, to catch a
                            // fault class that the re-entry refusal above already
                            // refuses at the return epoch whenever the fault is
                            // present on arrival.
                            a.admitted  = false;
                            a.clean_run = 0;
                            ++ctr.revet_hold_ep;
                            withhold_band(c.obs_idx, c.band);
                        } else {
                            a.admitted = true;
                            if (reentry) ++ctr.admits_reentry;
                            else         ++ctr.admits_birth;
                        }
                    } else if (a.admitted) {
                        // INC-4: inside a vet window the PREFIT census counts as
                        // evidence alongside this seam's own bound.  The two see
                        // different things: the census statistic is standardized
                        // (|y - m| sqrt(w)), so it resolves a ramping corruption while
                        // it is still under this card's absolute metre bound (at
                        // G16@BSHM it flagged the cell one epoch before |z| reached
                        // 1000 m).
                        //
                        // Scope: the flag is per line of sight, so a flagged satellite
                        // refuses whichever of its bands is currently vetting, not
                        // only the corrupt one.  There is no row -> band map to do
                        // better; the flicker card's demotion map made the same trade
                        // for the same reason (its deviation 6), and the vet window
                        // bounds the effect.  The refusals the census makes on its
                        // own are counted separately so the trade can be audited.
                        // The tighter bound stays live for the whole window, not only
                        // the return epoch.  §10.10's population is defined as "within
                        // 8 epochs of an arc_resume", which is this window, and it
                        // holds no healthy cell over 50 m, so the 200 m bound across
                        // the window stays inside the measurement.  It also catches a
                        // shape neither edge alone would: a channel that arrives clean
                        // and ramps through 200 m before it ramps through 1000 m.
                        bool vet_fail = revet && a.vet_left > 0 && fail_vet;
                        const bool vet_fail_bound = vet_fail;
                        if (a.vet_left > 0 && revet_census_wanted) {
                            // Counted before the freshness test so `absent` has a
                            // denominator.  This path is not reached on a return
                            // epoch (a re-entry takes the birth-equivalent branch
                            // above, a heal the rejected branch below), and the census
                            // defers a re-entering arc as a birth on its return epoch,
                            // so there is nothing to ask there.  Hence
                            // `revet_census_reads` falls one short of
                            // `revet_window_ep` per admitted return epoch, and exceeds
                            // it by one per refusal on this path.
                            ++ctr.revet_census_reads;
                            if (!revet_census) {
                                // Armed, but no fresh flag set to read.  Not a clean
                                // result (invariant #4 applied to a cross-seam read).
                                ++ctr.revet_census_absent;
                            } else if (ctx.carry.prefit_code_flag.count({si, sat})) {
                                vet_fail = true;
                            }
                        }
                        if (fail || vet_fail) {
                            ++ctr.onsets;
                            a.admitted  = false;
                            a.clean_run = 0;
                            // A refusal re-arms the window: the channel serves a full
                            // vet again, not the remainder of the old one.
                            if (revet) a.vet_left = revet_ep;
                            withhold_band(c.obs_idx, c.band);
                            if (vet_fail && !fail) {
                                // A vet-window refusal that the card's own bound would
                                // not have made this epoch.  The trigger is recorded
                                // and counted separately, because the two are evidence
                                // of different kinds and §10.10 shows they reach
                                // different cells: the vet bound says "no healthy
                                // re-entry in the measured sample has been this far
                                // out", the census says "a standardized prefit test
                                // flagged this line of sight last epoch".  Counted
                                // together, neither claim could be tested on its own.
                                if (vet_fail_bound) ++ctr.revet_bound_refusals;
                                else                ++ctr.revet_census_refusals;
                                emit("adm_revet_refuse", sat, st.name, c.z, "withhold",
                                     fmt::format("{};trigger={};vet_left={}", detail(),
                                                 vet_fail_bound ? "vet_bound"
                                                                : "prefit_census",
                                                 a.vet_left));
                            } else {
                                emit("adm_onset", sat, st.name, c.z, "withhold",
                                     detail());
                            }
                            evented = true;
                        }
                        // In bound and admitted: the steady state.  Nothing to do.
                    } else if (fail || (revet && a.vet_left > 0 && fail_vet)) {
                        // Rejected, and re-tested every epoch (spec §3 INC-2).
                        //
                        // The re-test uses the bound that refused the channel: while a
                        // vet window is open (it does not tick down while the channel
                        // is withheld), that includes the tighter vet bound.  Testing
                        // `fail` alone would let a channel held out by the 200 m vet
                        // bound accumulate clean epochs anywhere under 1000 m and be
                        // readmitted above the bound that refused it; the §10.11 field
                        // arm showed this on G22@BSHM as a refuse, readmit, refuse
                        // cycle with the bad readmission folded into a00.
                        a.clean_run = 0;
                        ++ctr.rejects_held;
                        withhold_band(c.obs_idx, c.band);
                    } else if (++a.clean_run >= heal_ep) {
                        // Readmit through the normal funnel with a fresh seed.  The
                        // band has been out of `observed` for at least heal_ep
                        // epochs, so `ensureRequiredStates` has already pruned its
                        // ambiguity and this seam pruned its bias states, but the
                        // invariant-DD chord is keyed independently of the
                        // StateManager, so the re-tap is what delivers "a heal never
                        // restores the old states".  The band is not withheld this
                        // epoch: that is what admission means.
                        ++ctr.heals;
                        a.admitted  = true;
                        a.clean_run = 0;
                        // A heal is a re-entry (the same arc returning with a fresh
                        // seed after an absence from the model set), so it opens a vet
                        // window for the same reason a gap does.  Otherwise the one
                        // path that returns a previously rejected channel to the solve
                        // would be the only one that returns it unvetted.
                        if (revet) a.vet_left = revet_ep;
                        for (const auto& [okey, oval] : modeled[i][c.obs_idx].observed) {
                            (void)oval;
                            if (okey.empty() || okey[0] != 'L') continue;
                            if (measurementAdmissionBand(okey) != c.band) continue;
                            const std::string amb = StateKeys::makeAmbiguityKey(sat, okey);
                            if (carry_invariant) {
                                carry_slipped_raw.insert({si, amb});
                                if (carry_slipped_ambs.insert({si, amb}).second)
                                    ++ctr.refloat_ambs;
                            }
                            if (carry_warm) {
                                sm.markAmbiguityPendingReseed(amb);
                                ++ctr.refloat_reseeds;
                            }
                        }
                        emit("adm_heal", sat, st.name, c.z, "readmit", detail());
                        evented = true;
                    } else {
                        ++ctr.heal_progress;
                        ++ctr.rejects_held;
                        withhold_band(c.obs_idx, c.band);
                    }

                    // --- INC-4 vet-window countdown ------------------------
                    // One decrement per screened epoch, at the end of the verdict, so
                    // the window is measured in epochs on which this seam looked at
                    // the channel (the same clock `last_ep` and the gap use).  An
                    // epoch on which the station was thin or absent does not use vet
                    // budget.
                    //
                    // The arithmetic (the flicker card's deviation 7 shows it can be
                    // read two ways): the window is set to `adm_revet_ep` on the
                    // return epoch and decremented at the end of it, so for a channel
                    // admitted throughout, coverage runs from the return epoch
                    // through return + (adm_revet_ep - 1).  The census cannot flag
                    // the return epoch itself (a re-entering arc's ambiguity is
                    // fresh, so the census defers it as a birth), and its flags are
                    // read one epoch late, so the first readable flag is at
                    // return + 2.  On the FT63 day the four exposed cells were
                    // flagged at return + 1, +3, +4 and +1, i.e. readable at
                    // return + 2, +4, +5 and +2, so the default of 8 (coverage
                    // through return + 7) covers every measured case with two
                    // epochs to spare.
                    // The window ticks down only on epochs the channel was admitted.
                    // A withheld epoch is not evidence of recovery, so spending vet
                    // budget on it would let a channel that is held out serve out
                    // its window while withheld and return unvetted, which INC-4
                    // exists to prevent.  It also keeps the vet bound live for as
                    // long as a channel is rejected, which the rejected-branch
                    // re-test above depends on.
                    if (a.vet_left > 0 && a.admitted) {
                        ++ctr.revet_window_ep;
                        if (--a.vet_left == 0) ++ctr.revet_expired;
                    }
                }

                // INC-3: name every over-bound cell that the response did not already
                // name this epoch.  That covers the measure-only arm and, on an armed
                // arm, the held rejections, which emit no transition event, so
                // without this their duration would be visible only as a counter.
                // Rate-limited per cell, like PREFIT.
                if (census && fail && !evented
                    && holdoff_ok("adm_census", st.name, sat, c.band))
                    emit("adm_census", sat, st.name, c.z,
                         screen ? "held" : "measure_only", detail());
            }
        }

        if (prune_states) {
            std::set<std::string> keep_pb;
            for (const auto& kv : sm.getSatPhaseBiasStates()) {
                const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                if (p && (withheld_pb.count(p->satellite + "|" + p->signal)
                          || emptied_sats.count(p->satellite))) {
                    ++ctr.withheld_pb_pruned;
                    continue;
                }
                keep_pb.insert(kv.first);
            }
            sm.pruneSatPhaseBiasStatesExcept(keep_pb);

            std::set<std::string> keep_cb;
            for (const auto& kv : sm.getSatBiasStates()) {
                if (withheld_cb.count(kv.first)) { ++ctr.withheld_cb_pruned; continue; }
                keep_cb.insert(kv.first);
            }
            sm.pruneSatBiasStatesExcept(keep_cb);

            if (!emptied_sats.empty()) {
                std::set<std::string> keep_clk;
                for (const auto& kv : sm.getSatClockStates()) {
                    if (emptied_sats.count(StateKeys::satelliteFromSatClockKey(kv.first))) {
                        ++ctr.withheld_clk_pruned;
                        continue;
                    }
                    keep_clk.insert(kv.first);
                }
                sm.pruneSatClockStatesExcept(keep_clk);
            }
        }
    }

    // --- INC-3 per-epoch scalars for network_epoch_status.csv ---------------
    // Two columns, per spec §3.  §3's "station-worst |z| and network-worst |z|" is
    // ambiguous for a file with one row per epoch: a single maximum is an extreme,
    // not a population, and the separation claim §7 gate 4 must test needs both.
    // The columns are therefore the median over screened stations of each station's
    // worst |z| (what a typical station's worst channel looks like) and the network
    // maximum (the extreme).  The identity of over-bound cells goes to the events,
    // as §3 says.  Stamped with the TOW so an epoch this seam did not screen leaves
    // the columns blank rather than repeating stale numbers (the redun_ep_tow
    // pattern, not the ep_dw_engaged pattern, whose sentinel is never reset and
    // therefore persists).
    if (!station_worst.empty()) {
        ctx.carry.adm_ep_tow         = t.tow;
        ctx.carry.ep_adm_sta_worst_z = medianOf(station_worst);
        ctx.carry.ep_adm_net_worst_z = net_worst;
    }

    // --- record ageing (memory bound only) ---------------------------------
    for (auto it = arcs.begin(); it != arcs.end();) {
        if (seen.count(it->first) || epoch_idx - it->second.last_ep <= kAdmArcForgetEp) {
            ++it;
            continue;
        }
        ++ctr.arcs_forgotten;
        it = arcs.erase(it);
    }

    if (ctr.epochs == 1)
        log->debug("Network (measurement admission): first screen pass — {} line(s) of "
                   "sight and {} CODE channel(s) screened at {} station(s); response "
                   "{}, census {}, bound {:g} m.{}",
                   ctr.los_epochs, ctr.channels, ctr.station_epochs,
                   screen ? "ARMED" : "off", census ? "on" : "off", bound,
                   resume_run ? "  This run is a warm RESUME: each station's first"
                              " screened epoch applies the RESTORE rule."
                               : "");
}

} // namespace GnssProcessor
