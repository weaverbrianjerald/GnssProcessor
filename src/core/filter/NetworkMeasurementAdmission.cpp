/**
 * @file NetworkMeasurementAdmission.cpp
 * @brief Product-referenced measurement admission — the corrupt-channel screen.
 *        See the header for the statistic, the increments and the ordering
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

/// One screened CODE observable of one line of sight, before the station centre is
/// known.  @c obs_idx indexes the station's `modeled[i]` vector, so a verdict can
/// reach back to the exact observation whose band it must withhold.
struct AdmSample {
    std::size_t obs_idx;
    char        band;
    double      d;
};

/// One (satellite, band) channel's verdict input, after centring.
struct AdmChannel {
    std::size_t obs_idx;
    char        band;
    double      d;      ///< the raw term of the WORST observable of this band.
    double      z;      ///< its centred value — the screen statistic.
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

/// "SAT|<band>" — the per-channel admission-record key.
inline std::string chanKey(const std::string& sat, char band) {
    return sat + "|" + std::string(1, band);
}

} // namespace

char measurementAdmissionBand(const std::string& key) {
    // RINEX-3 observation codes on the uncombined producer path are always the raw
    // 3-character forms ("C1C", "L2W", …).  Anything else — most notably the
    // iono-free path's "C1C_C2W" combination keys — has no single band to withhold
    // and is deliberately not screened.
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
    if (!ctx.knobs.adm_on) return;   // OFF: no map, no event, no observable touched

    const bool   screen   = ctx.knobs.adm_screen;
    const bool   census   = ctx.knobs.adm_trace;
    const double bound    = ctx.knobs.adm_bound_m;
    const int    min_sats = ctx.knobs.adm_min_sats;
    const int    heal_ep  = ctx.knobs.adm_heal_ep;
    const double max_frac = ctx.knobs.adm_max_frac;
    // INC-4 the re-entry vet.  `adm_revet` already carries the `&& adm_screen`
    // INERT-guard from both the loader and the knob snapshot.
    const bool   revet        = ctx.knobs.adm_revet;
    const int    revet_gap    = ctx.knobs.adm_revet_gap_ep;
    const int    revet_ep     = ctx.knobs.adm_revet_ep;
    const bool   revet_hold   = ctx.knobs.adm_revet_hold;
    // The TIGHTER bound a channel answers to while it is re-entering or vetting.
    // Measured, not picked (card §10.10): across two legs, 272 epochs, 63 stations
    // and 49 arc_resume cells screened at a 50 m census bound, NOT ONE healthy
    // station's re-entry cell cleared 50 m — the whole >50 m re-entry population is
    // BSHM00ISR, arriving at 670.3 m (G16), 347.6 m (G22) and 81.5 m (G13) on their
    // RETURN epoch, while the healthy tail (all of it steady-state) tops out at
    // 61.9 m.  The default 200 m therefore sits 3.2x above the worst legitimate |z|
    // anywhere in that sample and 3.3x below the lowest corrupt arrival it is meant
    // to reach.
    //
    // It deliberately does NOT reach G13 at 81.5 m.  That is 1.3x the healthy
    // maximum and is not separable by ANY absolute bound without firing on
    // COCO00AUS G24 — but G13 ramps slowly, so the census trigger names it at
    // return + 4.  The two mechanisms cover DISJOINT failure shapes (fast-ramp /
    // large-on-arrival vs slow-ramp / small-on-arrival) and neither subsumes the
    // other, which is why a single one of them kept looking like a partial fix.
    const double revet_bound  = ctx.knobs.adm_revet_bound_m;

    // The PREFIT census's obs_code exceedance set, and whether it is FRESH.
    //
    // The census walk runs LATER in the epoch than this seam, so its flags are
    // necessarily one loop epoch old — which is precisely the latency the field
    // data says is available: the census named every one of the FT63 day's four
    // exposed re-entries 1-4 epochs before their |z| reached this card's bound.
    // Both seams read `n_loop_epochs`, incremented once per loop epoch before
    // either of them runs, so `- 1` is exact rather than assumed.
    //
    // Staleness is a hard gate, not a shrug: a flag set from five epochs ago names
    // cells that may since have healed, and believing it would make the vet act on
    // evidence it does not have.  An unusable consultation is COUNTED
    // (`revet_census_absent`) so "the census said nothing" and "nobody asked the
    // census" stay distinguishable — invariant #4 applied to a cross-seam read.
    const long long loop_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const bool revet_census =
        revet && ctx.knobs.adm_revet_census
        && ctx.carry.prefit_code_flag_ep == loop_ep - 1;
    const bool revet_census_wanted = revet && ctx.knobs.adm_revet_census;

    auto& ctr  = ctx.counters.adm;
    auto& arcs = ctx.carry.adm_arc;
    ++ctr.epochs;

    // The warm-resume RESTORE rule (INC-1, second clause).  The record map is not
    // serialized, so on a resumed leg every channel looks brand new the first time
    // its station is screened — but its estimator state was restored and may be
    // carrying the corrupt seed, so a failure there is a RESTORE, not a birth.
    //
    // The latch is PER STATION, not a single global one-shot.  A global one-shot is
    // wrong and was measured to be: the seam's first pass screens whichever stations
    // happen to be present AND above the satellite floor, and a station that is
    // absent, thin, or whose satellite the two-station observability gate erased
    // that epoch would consume the flag it never used — every later first sighting
    // at that station then reads as a birth, mislabelling a restored corrupt arc.
    // The RESPONSE no longer depends on this (see the refloat rule below), so the
    // latch is now purely a LABEL — but a wrong label on the one event class this
    // card exists to name is not a thing to leave in.
    const bool resume_run = ctx.carry.adm_resume_pending;
    if (resume_run && ctr.epochs == 1) ++ctr.resume_passes;

    auto emit = [&](const char* event, const std::string& sat,
                    const std::string& station, double statistic, const char* action,
                    const std::string& detail) {
        traces.emitEvent(t, "ADMISSION", event, sat, station, statistic, bound,
                         action, detail);
        ++ctr.events;
    };
    // Per-(event, station, satellite, band) rate limit for the two streams that CAN
    // repeat — the INC-3 census and a chronically-bad station's guard event.  The
    // INC-1/INC-2 events are transitions (a birth, an onset, a heal) and never
    // consult it.  Duplicated rather than shared with the prefit census's
    // `holdoffAllows`, which is file-local to that translation unit; the redundancy
    // census set the duplication precedent.
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
    // simply was not screened from one whose record has gone stale.
    std::set<std::pair<int, std::string>> seen;

    // INC-3 per-epoch census accumulators.
    std::vector<double> station_worst;   // each screened station's worst |z|
    double      net_worst = 0.0;
    std::string net_worst_id;

    for (std::size_t i = 0; i < stations.size(); ++i) {
        // `present[i]` is the gate, not `modeled[i].empty()`: a PASS-A station that
        // threw AFTER computeModels leaves a populated `modeled[i]` behind an absent
        // presence flag, and screening it would invent verdicts for a station that
        // folds no rows (NetworkSignalIntegrity.cpp:118 makes the same check for the
        // same reason).
        if (i >= present.size() || !present[i]) continue;
        if (i >= modeled.size()) continue;
        NetStation& st = stations[i];
        if (!st.sm) continue;
        const int si = static_cast<int>(i);

        // ---- (1) gather the raw terms ------------------------------------
        // One entry per CODE observable.  Deliberately NOT reduced per band yet:
        // the reduction picks the worst |z|, and z is undefined until the station's
        // centre is known.  Counted over the POST-prefilter observation set, so a
        // satellite the two-station observability gate has just erased contributes
        // nothing to the median and raises no verdict.
        std::vector<AdmSample> samples;
        std::map<std::size_t, std::vector<double>> per_sat;   // obs_idx -> its code terms
        samples.reserve(modeled[i].size() * 2);
        for (std::size_t j = 0; j < modeled[i].size(); ++j) {
            const PppObservation& obs = modeled[i][j];
            if (obs.satellite.empty()) continue;
            ++ctr.los_epochs;
            for (const auto& [key, val] : obs.observed) {
                (void)val;
                if (key.empty() || key[0] != 'C') continue;   // CODE only, by principle
                const char band = measurementAdmissionBand(key);
                if (band == '\0') continue;
                double d = 0.0;
                if (!measurementAdmissionD(obs, key, d)) { ++ctr.skips_nonfinite; continue; }
                samples.push_back(AdmSample{j, band, d});
                per_sat[j].push_back(d);
            }
        }

        // The centre is the median over SATELLITES — one representative per line of
        // sight, itself the median of that satellite's code terms.  The
        // two-level reduction is load-bearing, not tidiness: a flat median over
        // OBSERVABLES weights a satellite by how many bands it happens to be tracked
        // on, so ONE satellite tracked on four bands out-votes three tracked on one
        // and can OWN the centre.  In that regime the screen inverts — the corrupt
        // satellite reads z ~ 0 and is admitted while its healthy peers read z ~ |d|
        // and are rejected — and the station guard structurally CANNOT catch it: the
        // guard fires iff (healthy bands > corrupt bands) while the capture needs the
        // opposite, so the two conditions are exact negations.  Reachable on the
        // repo's own shipped network configs (GPS+Galileo, frequency_mode "all",
        // channel_mode "all") at any station near the satellite floor.  Reducing per
        // satellite first also makes `adm_min_sats` bound the population it names:
        // the median's sample size IS the satellite count, exactly as spec section 2
        // writes it ("median over the station's sats, n >= 4").
        std::vector<double> sat_med;
        sat_med.reserve(per_sat.size());
        for (auto& [idx, v] : per_sat) {
            (void)idx;
            sat_med.push_back(medianOf(v));
        }

        // A median of two or three values IS the outlier it is meant to remove.
        // Below the floor the station is SKIPPED entirely and the skip is counted —
        // never silently treated as a centre of zero, and never used to reject
        // anything.
        if (static_cast<int>(sat_med.size()) < min_sats) {
            ++ctr.skips_thin;
            continue;
        }
        ++ctr.station_epochs;
        const std::size_t n_sats = sat_med.size();
        const double med = medianOf(sat_med);
        // This station's FIRST screened epoch since a warm resume — the pass on
        // which its restored arcs present themselves as first sightings.
        const bool sta_resume_pass =
            resume_run && ctx.carry.adm_resume_seen.insert(si).second;

        // ---- (2) centre, and reduce to one verdict per (satellite, band) --
        // Several code observables can share a band (C1C and C1W); they are ONE
        // physical channel, so the band's verdict is that of its WORST observable.
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
        // The fraction is computed before ANY verdict is acted on: if the CENTRE is
        // untrusted then no per-channel verdict derived from it means anything, and
        // the guard's whole point is to refuse those claims rather than to filter
        // them after the fact.
        long long n_fail = 0;
        std::set<std::size_t> fail_sats;   // DISTINCT satellites with a failing band
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

        // The guard's question is "is the CENTRE trustworthy?", so its denominator
        // must be the population that FORMED the centre — SATELLITES, one vote each.
        // Spec §3 writes "a station's screened bands"; measured against a band
        // denominator the guard fires on a single very bad satellite that happens to
        // be tracked on many bands (4 of 7 channels at a 4-satellite station is one
        // corrupt Galileo line of sight, not a corrupt station), holding the whole
        // station and refusing the per-channel claims that were in fact perfectly
        // sound.  Counting satellites makes the test say what §3's own justification
        // says: the median is untrusted when most of the things that voted on it
        // disagree with it.  Deviation recorded in the card's §9.
        const bool station_event =
            screen && n_sats > 0
            && static_cast<double>(fail_sats.size())
                   > max_frac * static_cast<double>(n_sats);

        // ---- (4) the response --------------------------------------------
        StateManager& sm = *st.sm;
        std::set<std::string> withheld_pb;   // "sat|signal" phase-bias identities
        std::set<std::string> withheld_cb;   // sat-code-bias state keys
        std::set<std::string> emptied_sats;  // satellites left with NO observable at all
        bool prune_states = false;

        // Erase every observable of one band, refloat the arcs behind the hole, and
        // account for both.
        //
        // The refloat is UNCONDITIONAL — including at a birth, where spec §3 says
        // "no state exists yet … pure admission".  That is still true, and it is
        // exactly why the re-tap is free there: inserting a fresh arc's key into the
        // slip funnel erases a DD chord that does not exist and a GM deviation that
        // does not exist, and marking a not-yet-allocated ambiguity pending-reseed
        // is a plain set insert PASS B consumes when it allocates.  A no-op.
        //
        // Making it conditional was a REAL defect, not a saving.  "Is this a birth
        // or a restore?" is answered from the seam's own record map, which is not
        // serialized and which a station absent (or thin, or satellite-pruned) on an
        // earlier epoch has no entry in — so a restored corrupt arc carrying a stiff
        // Card-4 chord could present as a first sighting, be labelled a birth, and
        // have its band erased with the chord left riding straight across the hole.
        // That is standing invariant #1 violated, and it is the failure Card 4's
        // slip hole cost 10.9 cyc/ep in the field.  Correctness must not depend on a
        // classification that can be wrong when the alternative costs nothing.
        auto withhold_band = [&](std::size_t obs_idx, char band) {
            PppObservation& obs = modeled[i][obs_idx];
            std::vector<std::string> phase_keys, code_keys;
            for (auto it = obs.observed.begin(); it != obs.observed.end();) {
                if (measurementAdmissionBand(it->first) != band) { ++it; continue; }
                if (it->first[0] == 'L')      phase_keys.push_back(it->first);
                else if (it->first[0] == 'C') code_keys.push_back(it->first);
                // raw_observed is deliberately untouched: it belongs to the slip
                // detectors, which have already run this epoch and must keep seeing
                // the receiver's true tracking state.
                it = obs.observed.erase(it);
            }
            const std::size_t n_removed = phase_keys.size() + code_keys.size();
            if (n_removed == 0) return;
            ++ctr.withheld_channels;
            ctr.withheld_obs += static_cast<long long>(n_removed);

            // Prune the now-orphaned shared states, exactly as the two-station
            // observability gate does when IT removes observations
            // (ApplicationNetworkAssembly.cpp:1456 "Prune the now-orphaned shared
            // states").  This is load-bearing in three independent places, all of
            // which run AFTER this seam and all of which derive from the STATES
            // rather than from the observables:
            //   * the anchor election reads the phase-bias states, so an unpruned
            //     one could be seated as a datum pin whose supporting row this seam
            //     has just removed — the pin folds against nothing and the gauge
            //     silently loses a constraint;
            //   * the code-bias registration counts STATIONS HOLDING A STATE against
            //     its >= 2 coverage gate, so an unpruned one phantom-supports a
            //     shared OSB column that only one station actually feeds — the
            //     single-station code column that is perfectly confounded with that
            //     receiver's clock and goes near-null; and
            //   * `registerClock` unions the satellite clock states, so a satellite
            //     left with NO observable at all here must stop claiming to observe
            //     it (recorded per satellite and applied only when the observation
            //     is emptied, since a satellite that keeps another band is still a
            //     genuine supporter).
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
            // Reset, never repair.  Nothing bounds the error on a corrupt channel,
            // so there is no value to correct toward — only state to discard.  The
            // insertion rides the EXACT funnel the MW / LLI / IF-TD detections use;
            // a parallel reset path would be a second definition of "this arc
            // re-taps fresh".  BOTH feeds are written and counted apart: a bare
            // `markAmbiguityPendingReseed` is a one-shot PASS-B consumes and the
            // leveling harvest clears, so on a lineage without warm leveling it is
            // not a refloat at all (NetworkIonoDwRelease.hpp:185 records that trap).
            for (const std::string& b : phase_keys) {
                const std::string amb = StateKeys::makeAmbiguityKey(obs.satellite, b);
                if (carry_invariant) {
                    // The RAW provenance set too: a withheld band IS a raw
                    // observable event (the channel physically leaves the model
                    // set), so the Iono-2 v3k erase-policy split must ERASE this
                    // line of sight's GM deviation rather than D5-KEEP a level
                    // carried across the hole this seam just punched.
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
            // The IF-TD reference gate's incoherent-station precedent
            // (IfPhaseSlipDetector.cpp:454): when a family disagrees with its own
            // centre, the CENTRE is the untrustworthy thing, and the honest move is
            // to act at the family's scope while REFUSING every per-member claim.
            // So: hold the station's screened bands, refloat the arcs behind the
            // holes, emit ONE event — and leave every admission record untouched, so
            // the next epoch re-tests from evidence rather than from a verdict this
            // one was not entitled to make.  No heal counter advances either: a
            // verdict from a corrupted centre is not evidence in EITHER direction.
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
                // The tighter re-entry bound, evaluated apart from `fail` and NEVER
                // substituted for it.  `fail` is this card's absolute admission
                // verdict and is also the INC-3 census's domain — §7 gate 5 grades
                // on "enumerates every |z| over the bound on the day", which is a
                // statement about `adm_bound_m`.  Folding a vet-window threshold
                // into it would silently redefine what the census means for the
                // ~0.3 % of channel-epochs that happen to be vetting, and make the
                // measure-only arm's numbers depend on a response knob.
                const bool fail_vet = revet && c.absz > revet_bound;
                // Built ON DEMAND: emitEvent does not short-circuit its arguments
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
                    // The PREVIOUS screened epoch, captured before it is overwritten.
                    // `last_ep` advances only on epochs this seam actually screened
                    // the channel, so the gap counts absence from THE MODEL SET as
                    // this seam sees it — a satellite the two-station observability
                    // gate erased, or a station under the median floor, reads as
                    // absent.  That is the conservative direction: it re-screens.
                    const long long prev_ep = first ? -1 : a.last_ep;
                    const long long gap     = (prev_ep < 0) ? 0
                                                            : epoch_idx - prev_ep - 1;
                    a.last_ep = epoch_idx;

                    // INC-4.  A RE-ENTRY is a channel whose record survives but whose
                    // arc did not: it has been absent for at least `adm_revet_gap_ep`
                    // screened epochs, so the `admitted` verdict it is carrying was
                    // made about a channel that is not necessarily the one now
                    // present.  Re-screen it as a BIRTH rather than inherit that.
                    //
                    // This is the same defect class as §9.5's warm-resume latch, one
                    // level down: there the record map did not survive the boundary
                    // and a restored arc read as a birth; here the record survives a
                    // boundary the ARC did not, and a re-entry reads as an
                    // established arc.  Both are a stale answer to "is this the
                    // channel I am holding a verdict about?", and both get the same
                    // treatment — stop asking the question of a source that cannot
                    // answer it, and re-derive from this epoch's evidence.
                    const bool reentry = revet && !first && gap >= revet_gap;

                    if (first || reentry) {
                        if (reentry) {
                            // Open the vet window.  `births` is deliberately NOT
                            // incremented: it is the admission-opportunity denominator
                            // the two birth rejections of §7 gate 2 are read against,
                            // and folding re-entries into it would silently move that
                            // gate's baseline.  The re-entry population gets its own.
                            ++ctr.reentries;
                            a.vet_left = revet_ep;
                        } else {
                            ++ctr.births;
                        }
                        // A RE-ENTRY answers to the tighter bound; a true BIRTH does
                        // not.  The asymmetry is the measurement's, not a hedge: the
                        // §10.10 population is specifically "cells within 8 epochs of
                        // an arc_resume", and nothing licenses extending a 200 m bound
                        // to a first-ever sighting, where the arc has no history to be
                        // suspicious about and the rising-satellite population is
                        // largest.
                        const bool refuse = fail || (reentry && fail_vet);
                        if (refuse) {
                            a.admitted  = false;
                            a.clean_run = 0;
                            withhold_band(c.obs_idx, c.band);
                            // `reentry` is tested FIRST although it cannot co-occur
                            // with `sta_resume_pass` (a resumed run's map is empty, so
                            // every channel on a station's first screened epoch is
                            // `first` and no re-entry is reachable there).  Ordering
                            // it first anyway means the three labels stay mutually
                            // exclusive by CONSTRUCTION rather than by an argument
                            // about reachability that a later change could quietly
                            // invalidate.
                            if (reentry) {
                                // The twelve of the FT63 day.  Same response the onset
                                // path gave them — this is a relabel, and the point of
                                // the relabel is that the label was carrying a claim
                                // ("a previously-good channel went bad") that the
                                // evidence did not support.
                                // Which bound made the call is recorded, because the
                                // two mean different things: `bound` says the channel
                                // is in the 1e5 m corrupt class outright, `vet_bound`
                                // says it arrived somewhere no healthy re-entry in the
                                // §10.10 sample has ever been.  Only the latter is a
                                // claim this increment is making on its own evidence.
                                ++ctr.reject_reentry;
                                if (!fail) ++ctr.revet_bound_refusals;
                                emit("adm_reject_reentry", sat, st.name, c.z, "withhold",
                                     fmt::format("{};gap_ep={};trigger={}", detail(), gap,
                                                 fail ? "bound" : "vet_bound"));
                            } else if (sta_resume_pass) {
                                // A RESTORED corrupt arc: its state crossed the snapshot
                                // boundary even though this observer's memory did not.
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
                            // STRICT posture.  One in-bound epoch is thin evidence
                            // about a channel that has been away, and the FT63 day
                            // shows why: G16@BSHM and G13@BSHM both passed the bound
                            // on their return epoch and crossed it 2 and 5 epochs
                            // later, having been folded at full weight in between
                            // (a00 39.7 -> 59.4 and 30.8 -> 43.0 TECU).  Holding the
                            // channel until the heal path readmits it removes that
                            // window outright.
                            //
                            // Default OFF, and the reason is the prefer-fallback
                            // directive rather than timidity: this withholds EVERY
                            // re-entry, ~4650 of them on a 63-station day, and the
                            // population it delays is dominated by legitimately
                            // RISING satellites.  A 63-station producer that admits
                            // every rising satellite 5 epochs late is paying an
                            // observability cost to catch a fault class that the
                            // relabel above already refuses at the return epoch
                            // whenever the fault is present on arrival.
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
                        // INC-4: inside a vet window the PREFIT census is admitted as
                        // evidence alongside this seam's own bound.  The two see
                        // different things — the census statistic is STANDARDIZED
                        // (|y - m| sqrt(w)), so it resolves a ramping corruption while
                        // it is still under this card's absolute metre bound, which is
                        // exactly what it did at G16@BSHM (s = 1439.6 on a raw
                        // y = 975.5 m, one epoch before |z| reached 1000).
                        //
                        // Scope note, stated rather than hidden: the flag is per LINE
                        // OF SIGHT, so a flagged satellite refuses whichever of its
                        // bands is currently vetting, not only the corrupt one.  There
                        // is no row -> band map to do better, the flicker card's
                        // demotion map made the same trade for the same reason (its
                        // deviation 6), and the blast radius is bounded by the vet
                        // window.  The refusals it makes on its own are counted apart
                        // so the trade is auditable rather than argued.
                        // The tighter bound stays live for the whole window, not just
                        // the return epoch.  §10.10's population is defined as "within
                        // 8 epochs of an arc_resume" — exactly this window — and it is
                        // empty of healthy cells over 50 m, so extending the 200 m
                        // bound across it is covered by the measurement rather than
                        // extrapolated past it.  It also catches the shape neither
                        // edge alone would: a channel that arrives clean and ramps
                        // through 200 m before it ramps through 1000.
                        bool vet_fail = revet && a.vet_left > 0 && fail_vet;
                        const bool vet_fail_bound = vet_fail;
                        if (a.vet_left > 0 && revet_census_wanted) {
                            // Counted BEFORE the freshness test so `absent` has a
                            // denominator.  Note this path is not reached on the
                            // return epoch itself — that one takes the
                            // birth-equivalent branch above — which is why
                            // `revet_census_reads` sits exactly one below
                            // `revet_window_ep` per re-entry, and why it should: the
                            // census defers a re-entering arc as a BIRTH on its
                            // return epoch, so there is nothing there to ask about.
                            ++ctr.revet_census_reads;
                            if (!revet_census) {
                                // Armed, but there is no fresh flag set to read.  NOT
                                // a clean bill of health — invariant #4 applied to a
                                // cross-seam read.
                                ++ctr.revet_census_absent;
                            } else if (ctx.carry.prefit_code_flag.count({si, sat})) {
                                vet_fail = true;
                            }
                        }
                        if (fail || vet_fail) {
                            ++ctr.onsets;
                            a.admitted  = false;
                            a.clean_run = 0;
                            // Refusing RE-ARMS the window: the channel has to serve a
                            // full vet again, not the remainder of an old one.
                            if (revet) a.vet_left = revet_ep;
                            withhold_band(c.obs_idx, c.band);
                            if (vet_fail && !fail) {
                                // A vet-window refusal the card's own bound would NOT
                                // have made this epoch.  Which of the two triggers
                                // fired is recorded and counted apart, because they
                                // are evidence of different kinds and the whole point
                                // of §10.10 is that they reach different cells: the
                                // vet BOUND says "no healthy re-entry in the measured
                                // sample has ever been this far out", the CENSUS says
                                // "a standardized prefit test flagged this line of
                                // sight last epoch".  Fused, neither claim would be
                                // falsifiable on its own.
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
                        // Rejected, and re-testing every epoch (spec §3 INC-2).
                        //
                        // The re-test uses the SAME bound that refused the channel.
                        // Testing `fail` alone here was a real defect, found by the
                        // §10.11 field arm rather than by review: a channel held out
                        // by the 200 m vet bound accumulated "clean" epochs at
                        // anything under 1000, so G22@BSHM was refused at 347.6 m
                        // (273450), readmitted by the heal path at **782.2 m**
                        // (273600) and re-refused at 888.5 m one epoch later — a
                        // flicker, with the bad readmission visibly folded into a00
                        // (32.60 -> 37.54 -> 34.23).  An admission bound that lets a
                        // channel back in above itself is not a bound.
                        a.clean_run = 0;
                        ++ctr.rejects_held;
                        withhold_band(c.obs_idx, c.band);
                    } else if (++a.clean_run >= heal_ep) {
                        // Readmit through the normal funnel with a FRESH seed.  The
                        // band has been out of `observed` for at least heal_ep
                        // epochs, so `ensureRequiredStates` has already pruned its
                        // ambiguity and this seam pruned its bias states — but the
                        // invariant-DD chord is keyed INDEPENDENTLY of the
                        // StateManager, so the re-tap is what actually delivers
                        // "a heal never restores the old states".  The band is NOT
                        // withheld this epoch: that is what admission means.
                        ++ctr.heals;
                        a.admitted  = true;
                        a.clean_run = 0;
                        // A heal IS a re-entry — the same arc coming back with a fresh
                        // seed after an absence from the model set — so it opens a vet
                        // window for the same reason the gap does.  Without this, the
                        // one path that puts a previously-REJECTED channel back into
                        // the solve would be the only one that puts it back unvetted.
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
                    // ONE decrement per SCREENED epoch, at the end of the verdict, so
                    // the window is measured in epochs this seam actually looked at
                    // the channel — the same clock `last_ep` and the gap use.  An
                    // epoch the station was thin or absent does not burn vet budget.
                    //
                    // The arithmetic, stated because the flicker card's deviation 7
                    // shows it is the kind of thing that gets read two ways: the
                    // window is set to `adm_revet_ep` on the return epoch and
                    // decremented at the END of it, so coverage runs from the return
                    // epoch through return + (adm_revet_ep - 1).  The census cannot
                    // flag the return epoch itself (a re-entering arc's ambiguity is
                    // fresh, so the census defers it as a BIRTH) and its flags are
                    // read one epoch late, which puts the first readable flag at
                    // return + 2.  On the FT63 day the four exposed cells were
                    // flagged at return + 1, +3, +4 and +1, i.e. readable at return
                    // + 2, +4, +5 and +2 — so the default of 8 covers every measured
                    // case with three epochs to spare.
                    // The window ticks down only on epochs the channel was ADMITTED.
                    // A withheld epoch is not evidence of recovery, so spending vet
                    // budget on it would let a channel that is being held out serve
                    // out its window in absentia and come back UNVETTED — which is
                    // precisely the state INC-4 exists to abolish.  It is also what
                    // keeps the vet bound live for the whole time a channel is
                    // rejected, which is what the heal-path fix above depends on.
                    if (a.vet_left > 0 && a.admitted) {
                        ++ctr.revet_window_ep;
                        if (--a.vet_left == 0) ++ctr.revet_expired;
                    }
                }

                // INC-3: NAME every over-bound cell the response did not already
                // name this epoch.  That covers the measure-only arm outright and,
                // on an armed arm, the HELD rejections — which emit no transition
                // event by design, so without this their duration would be visible
                // only as a counter.  Rate-limited per cell, like PREFIT.
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
    // TWO columns, per spec §3.  The reading is recorded here because §3's
    // "station-worst |z| and network-worst |z|" is ambiguous on a file with one row
    // per EPOCH: a single maximum is an extreme, not a population, and the
    // separation claim §7 gate 4 has to falsify needs both halves.  So the columns
    // are the MEDIAN over screened stations of each station's worst |z| (the
    // healthy sea — what a typical station's worst channel looks like) and the
    // network MAXIMUM (the extreme).  The identity of the maximum goes to the
    // events, exactly as §3 says.  Stamped with the TOW so an epoch this seam did
    // not screen leaves the columns BLANK rather than repeating stale numbers (the
    // redun_ep_tow pattern; deliberately NOT the ep_dw_engaged pattern, whose
    // sentinel is never reset and therefore sticks).
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
