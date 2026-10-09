/**
 * @file NetworkIonoDwRelease.cpp
 * @brief Implementation of @ref networkIonoDwRelease, the iono-deweight release
 *        hygiene seam (docs/SPEC_deweight_release_hygiene.md).
 */

#include "filter/NetworkIonoDwRelease.hpp"

#include "app/NetworkRunContext.hpp"
#include "app/NetworkSetupResult.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunGroups.hpp"
#include <cstddef>
#include <memory>

namespace GnssProcessor {

namespace {

/// One station's sweep disposition for this epoch.  A station enters the sweep for
/// any of four independent reasons and may satisfy several at once; the flags stay
/// apart because the counters must distinguish them (invariant #4).
struct SweepFlags {
    bool released = false;   ///< its zero-weight era ended in this epoch's advance() (INC-1).
    bool decoupled = false;  ///< weight <= eps now, the spec's trigger (INC-2).
    bool cold = false;       ///< the gate holds no samples for it, e.g. post-warm-resume (INC-2).
    bool screen = false;     ///< it owns held rows, or admission is armed (INC-3 / INC-3b).
};

/// Erase every "NAME|SAT" entry of one station from a "|"-delimited keyed container.
/// The containers are lexicographically ordered, so a station's entries form one
/// contiguous range; the delimiter-terminated prefix makes the match exact ("BSHM"
/// does not match "BSHM00ISR").  '|' is 0x7C, so ++back() gives the open upper bound
/// 0x7D.
template <typename ContainerT>
std::size_t eraseStationRange(ContainerT& c, const std::string& name) {
    const std::string lo = name + "|";
    std::string hi = lo;
    ++hi.back();
    auto it = c.lower_bound(lo);
    auto end = c.lower_bound(hi);
    const std::size_t n = static_cast<std::size_t>(std::distance(it, end));
    c.erase(it, end);
    return n;
}

/// Consecutive barren (no harvested deviation) epochs a quarantined LOS may serve
/// before it is admitted anyway (INC-3b).  A judgeable LOS is harvested in the same
/// epoch its row would fold, so 2 is already generous; the bound only keeps a LOS
/// whose harvest never runs from being withheld for the whole run.
constexpr int kAdmitNoDevMaxEp = 2;

} // namespace

void networkIonoDwRelease(NetworkRunContext& ctx,
                          const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces,
                          NetworkSetupResult& setup,
                          const SyncedEpoch& se,
                          const NetworkEstimator& est,
                          const Eigen::VectorXd& x_s,
                          const std::vector<int>& est_index_of,
                          const std::vector<std::vector<PppObservation>>& modeled,
                          const NetworkIonoDeweightGate& iono_deweight) {
    // --- OFF path: structurally inert (no sweep, no allocation, no state write) ---
    if (!ctx.knobs.dw_hygiene_on) return;

    auto& sh_model  = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;
    auto& stations  = setup.stations;
    if (!sh_model) return;                     // no field to screen against (loader warns)

    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled) return;                   // the tie rows this seam guards are not built

    auto& ctr     = ctx.counters.dwr;
    auto& held    = ctx.carry.dw_row_held;
    auto& seen    = ctx.carry.dw_tie_seen;
    auto& stash   = ctx.carry.sh_dev_carry;
    auto& pending = ctx.carry.dw_release_pending;
    const bool   reseed_on   = ctx.knobs.dw_release_reseed;
    const double eps         = ctx.knobs.dw_release_eps;
    const double bound_tecu  = ctx.knobs.dw_park_bound_tecu;      // 0 = INC-2 off
    const double validate_k  = ctx.knobs.dw_release_validate_k;   // 0 = INC-3 off
    const double admit_k     = ctx.knobs.dw_admit_validate_k;     // 0 = INC-3b off
    const bool   admit_on    = admit_k > 0.0;
    const double rep_sig     = shc.representation_sigma_tecu;
    ++ctr.epochs;

    // --- The gate-advance watermark ------------------------------------------
    // networkShHealth skips the whole gate block on a datum-change epoch (its guard is
    // `adaptive() && !datum_changed`) and there is no advance(false) caller in the
    // tree, so the gate's per-call release list retains the previous epoch's contents.
    // Consuming it unguarded would re-fire an old release on every such epoch.
    const long long seq = iono_deweight.advanceCount();
    const bool advanced = (seq != ctx.carry.dw_gate_adv_seen);
    ctx.carry.dw_gate_adv_seen = seq;
    if (advanced) ++ctr.gate_advances;

    // --- Which stations does this epoch sweep? -------------------------------
    // No reconstructStation call is needed.  The seam reads the solved SH field (from
    // x_s by column lookup) and the solved per-LOS deviation, which networkShDevVtec
    // has already harvested into ctx.carry.sh_dev_carry earlier in this epoch.  The
    // cost is one SH evaluation per LOS of the selected stations, with no covariance
    // consumer (H1 de-duplication).
    std::map<std::size_t, SweepFlags> sweep;
    if (advanced && reseed_on) {
        std::map<std::string, std::size_t> idx_of;
        for (std::size_t i = 0; i < stations.size(); ++i) idx_of.emplace(stations[i].name, i);
        for (const auto& nm : iono_deweight.lastReleased()) {
            const auto it = idx_of.find(nm);
            if (it == idx_of.end()) continue;
            ++ctr.releases_seen;
            sweep[it->second].released = true;
        }
    }
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const std::string& nm = stations[i].name;
        if (bound_tecu > 0.0) {
            const bool decoupled = (iono_deweight.weight(nm) <= eps);
            // Post-warm-resume coverage.  No gate state is serialized, so a resumed leg
            // starts every station at w = 1 with zero samples and re-folds every tie row
            // on its first assembly with no release edge to hook; the spec gives this as
            // the reason for INC-2.  A station the gate has never scored is that
            // condition (and, in a cold run, the pre-min_epochs warm-up, where the bound
            // applies equally).
            const bool cold = (iono_deweight.samples(nm) == 0);
            if (decoupled || cold) {
                auto& f = sweep[i];
                f.decoupled = decoupled;
                f.cold = cold;
            }
        }
        // Admission sweeps every present station (INC-3b): it concerns LOS this seam
        // has never seen, so it cannot be driven off a list of stations it already knows.
        if (admit_on) { sweep[i].screen = true; continue; }
        if (validate_k > 0.0 && !held.empty()) {
            const std::string lo = nm + "|";
            std::string hi = lo; ++hi.back();
            if (held.lower_bound(lo) != held.lower_bound(hi)) sweep[i].screen = true;
        }
    }
    if (sweep.empty()) return;

    // --- The solved SH field (both shells) -----------------------------------
    // Same reconstruction as the health sweep: coefficients out of x_s by column
    // lookup, then vtec(coeff, IPP) * mapping * tecu_to_L1m per shell.
    const auto& coeffs = sh_model->coeffs();
    Eigen::VectorXd coeff(static_cast<Eigen::Index>(coeffs.size()));
    bool coeff_ok = true;
    for (std::size_t k = 0; k < coeffs.size() && coeff_ok; ++k) {
        try {
            coeff(static_cast<Eigen::Index>(k)) =
                x_s(est.vtecCoeffColumn(coeffs[k].n, coeffs[k].m, coeffs[k].trig));
        } catch (...) { coeff_ok = false; }
    }
    Eigen::VectorXd coeff2;
    if (coeff_ok && sh_model2) {
        const auto& c2 = sh_model2->coeffs();
        coeff2.resize(static_cast<Eigen::Index>(c2.size()));
        for (std::size_t k = 0; k < c2.size() && coeff_ok; ++k) {
            try {
                coeff2(static_cast<Eigen::Index>(k)) =
                    x_s(est.vtecCoeffColumn(c2[k].n, c2[k].m, c2[k].trig, 1));
            } catch (...) { coeff_ok = false; }
        }
    }
    if (!coeff_ok) {
        // Without the field there is no screen.  Doing nothing is the safe disposition,
        // but it skips an armed response, so it is logged; every hold stays in place
        // (an error path does not admit a held row).
        log->warn("Network (dw release hygiene): SH coefficient columns unavailable at "
                  "TOW {:.1f} — the release/admission response is SKIPPED this epoch "
                  "(invariant #4: an armed guard that cannot read the field is not a "
                  "guard that passed).", se.time.tow);
        return;
    }

    const double Re = shc.earth_radius_km * 1000.0;
    const double H  = shc.layer_height_km * 1000.0;
    const double H2 = shc.layer2_height_km * 1000.0;
    const double tecu_to_L1m =
        ionoDelayMetresPerTecu(nominalCarrierFrequencyHz(FrequencyBand::GpsL1));
    const double lon_sun = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;

    auto emit = [&](const char* event, const std::string& sat, const std::string& station,
                    std::optional<double> statistic, std::optional<double> threshold,
                    const char* action, const std::string& detail) {
        traces.emitEvent(se.time, "IONO_DW", event, sat, station, statistic, threshold,
                         action, detail);
    };

    for (const auto& [i, flags] : sweep) {
        NetStation& st = stations[i];
        Eigen::VectorXd x = st.sm->getStateVector();
        bool x_dirty = false;
        long long n_slant = 0, n_park_dc = 0, n_park_cold = 0, n_rel = 0;
        long long n_held = 0, n_held_entered = 0, n_held_released = 0;
        long long n_admit_held = 0, n_admitted = 0, n_admit_rejected = 0, n_seeded = 0;
        long long n_admit_nodev = 0;
        std::vector<std::string> reinflate_sats;

        // For a screened station both per-station sets are rebuilt from scratch (the
        // admitted set only when admission is armed), so a LOS that sets or is pruned
        // cannot leave a stale hold or a stale admission behind.  The satellites held /
        // admitted on entry are kept so entered-vs-released is attributed per LOS
        // instead of inferred from a count difference, which a same-epoch swap would
        // hide.
        std::set<std::string> held_before, seen_before;
        if (flags.screen) {
            const std::string lo = st.name + "|";
            std::string hi = lo; ++hi.back();
            for (auto it = held.lower_bound(lo); it != held.lower_bound(hi); ++it)
                held_before.insert(it->substr(lo.size()));
            for (auto it = seen.lower_bound(lo); it != seen.lower_bound(hi); ++it)
                seen_before.insert(it->substr(lo.size()));
            eraseStationRange(held, st.name);
            if (admit_on) eraseStationRange(seen, st.name);
        }

        for (const auto& o : modeled[i]) {
            if (o.satellite.empty() || systemOfSat(o.satellite) == System::GLONASS) continue;  // SH excludes GLONASS
            if (o.elevation < iw.elevation_cutoff_deg) continue;              // the admission gate
            if (!o.sat_pos.allFinite()) continue;
            const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            const IppGeometry g0 = computeIppGeometry(st.ecef, o.sat_pos, Re, H);
            if (!g0.valid) continue;

            // Field evaluation for this LOS, in L1 slant metres: the quantity the
            // assembly's tie row asserts the state should equal, in the form the emit
            // path uses (vtec * mapping * tecu_to_L1m).
            double sh_slant =
                sh_model->vtec(coeff, g0.ipp_lat_rad, g0.ipp_lon_rad - lon_sun)
                * g0.mapping * tecu_to_L1m;
            if (sh_model2) {
                const IppGeometry g2 = computeIppGeometry(st.ecef, o.sat_pos, Re, H2);
                if (g2.valid)
                    sh_slant += sh_model2->vtec(coeff2, g2.ipp_lat_rad,
                                                g2.ipp_lon_rad - lon_sun)
                                * g2.mapping * tecu_to_L1m;
            }
            if (!std::isfinite(sh_slant)) continue;

            const std::string key = st.name + "|" + o.satellite;
            const double m0_tecu = g0.mapping * tecu_to_L1m;   // vertical TECU -> slant m

            // --- The screen statistic: the solved deviation, not the state -------
            // The producer is a per-epoch absolute solve whose per-station StateManager
            // is a frozen linearization point, and every innovation is formed against
            // it (data rows via em.innovations; the tie row via r.innov = -x(iono)).  A
            // shift of x_lin by d therefore moves y by exactly -A_r e_c d and leaves the
            // reduced shared RHS u_s - N_sr N_rr^-1 u_r algebraically unchanged, so the
            // absolute estimate x_lin + x_r is invariant and |x_lin - field| is a
            // frame-arbitrary number that is not screened on.  The physical quantity is
            // the deviation of the absolute slant from the field, which
            // networkShDevVtec harvested into the stash this same epoch as
            // dev = (x_lin + x_r - SH) / M0 / tecu_to_L1m, in vertical TECU.  It is
            // standardized by the representativeness sigma in the same frame, which
            // makes it dimensionless, elevation-stationary and free to compute.
            const auto dit = stash.find(key);
            const bool has_dev = (dit != stash.end() && dit->second.t_s > 0.0);
            const double dev_v = has_dev ? dit->second.dev : 0.0;
            const double z = (has_dev && rep_sig > 0.0) ? std::fabs(dev_v) / rep_sig : 0.0;
            const double slant_abs = sh_slant + dev_v * m0_tecu;   // absolute, L1 metres

            // --- Park bound while decoupled or gate-cold (INC-2) ----------------
            // slant_abs is an L1-referenced slant delay in metres, so the comparison is
            // |slant| / tecu_to_L1m with no mapping function, in slant TECU.  The
            // actuator is the stash erase (the tie row's predicted deviation is an
            // uncompensated input to y and R); the state write beside it is bookkeeping.
            // With admission armed, a reset LOS is not re-entered in the admitted set,
            // so its next tie row goes through the admission screen again.
            if (bound_tecu > 0.0 && (flags.decoupled || flags.cold)
                && std::isfinite(slant_abs)) {
                const double slant_tecu = std::fabs(slant_abs) / tecu_to_L1m;
                if (slant_tecu > bound_tecu) {
                    if (has_dev) stash.erase(dit);
                    x(iono_idx) = sh_slant;
                    x_dirty = true;
                    reinflate_sats.push_back(o.satellite);   // de-duplicated below
                    if (flags.decoupled) ++n_park_dc; else ++n_park_cold;
                    if (slant_tecu > ctr.park_max_tecu) ctr.park_max_tecu = slant_tecu;
                    emit("dw_park_bound_reset", o.satellite, st.name, slant_tecu,
                         bound_tecu, flags.decoupled ? "reset_decoupled" : "reset_cold",
                         fmt::format("slant {:.4g} m -> field {:.3f} m", slant_abs, sh_slant));
                    continue;   // reseeded to the field: no screen decision this epoch
                }
            }

            // --- Release-time reseed (INC-1) -----------------------------------
            // The state write is bookkeeping, not an actuator (see the invariance note
            // above): it keeps the frame-dependent readouts meaningful and gives the
            // next structural change a sane starting point.  The actuators of INC-1 are
            // the stash erase and the ambiguity refloat, both applied per station below.
            if (flags.released && reseed_on) {
                x(iono_idx) = sh_slant;
                x_dirty = true;
                reinflate_sats.push_back(o.satellite);   // de-duplicated below
                ++n_slant;
            }

            if (!flags.screen) continue;

            // --- Admission screen on a LOS's first tie row (INC-3b) -------------
            // A satellite crossing the elevation cutoff and entering a station's tie
            // stack for the first time couples its slant column to the SH block with no
            // deweight release involved (the measured event at DOY-129 TOW 329160; see
            // the header).  A LOS this seam has not admitted is therefore held for one
            // epoch, during which networkShDevVtec still harvests its deviation (that
            // harvest is gated on the deweight weight, not on whether the row folded),
            // and it is admitted at the end of that epoch if the harvested deviation
            // passes k.
            if (admit_on && !ctx.carry.dw_tie_seeded) {
                // One-shot start/resume seed: admit every present LOS unscreened.  See
                // NetworkCarryState::dw_tie_seeded: at start there is no established
                // field for a first coupling to be relative to, and holding every row for
                // an epoch would disturb the solve far more than the guarded event.
                // Counted apart from a screened admission.
                seen.insert(key);
                ++n_seeded;
                continue;
            }
            if (admit_on && !seen_before.count(o.satellite)) {
                if (!has_dev) {
                    // No statistic to judge by.  networkShDevVtec skips its harvest for
                    // a station at weight <= 0 and for a LOS with no local column, so
                    // for those the deviation never arrives, and holding on missing data
                    // would withhold the row for the whole run.  Rejection requires
                    // evidence: the barren wait is bounded (kAdmitNoDevMaxEp), then the
                    // LOS is admitted and counted apart.  A LOS the screen can judge is
                    // harvested in the same epoch its row would fold, so this path does
                    // not fail open for a judgeable one.
                    const int wait = ++ctx.carry.dw_admit_wait[key];
                    if (wait > kAdmitNoDevMaxEp) {
                        seen.insert(key);
                        ++n_admit_nodev;
                        ctx.carry.dw_admit_wait.erase(key);
                    } else {
                        held.insert(key);
                        ++n_admit_held;
                        emit("dw_admit_quarantine", o.satellite, st.name,
                             static_cast<double>(wait), admit_k, "hold_tie_row",
                             fmt::format("first tie row, no harvested deviation yet "
                                         "(field {:.3f} m)", sh_slant));
                    }
                } else if (z > admit_k) {
                    ctx.carry.dw_admit_wait.erase(key);   // judgeable now
                    held.insert(key);
                    ++n_admit_held; ++n_admit_rejected;
                    emit("dw_admit_reject", o.satellite, st.name, z, admit_k,
                         "hold_tie_row",
                         fmt::format("slant {:.4g} m vs field {:.3f} m "
                                     "(dev {:.2f} TECU, sigma_rep {:.2f} TECU)",
                                     slant_abs, sh_slant, dev_v, rep_sig));
                } else {
                    ctx.carry.dw_admit_wait.erase(key);   // judgeable now
                    seen.insert(key);
                    ++n_admitted;
                }
                continue;   // admission owns this LOS's disposition this epoch
            }

            // --- Release screen on an already-admitted LOS (INC-3) --------------
            if (validate_k > 0.0 && has_dev && z > validate_k) {
                held.insert(key);
                ++n_held;
                if (!held_before.count(o.satellite)) ++n_held_entered;
                emit("dw_release_row_held", o.satellite, st.name, z, validate_k,
                     "hold_tie_row",
                     fmt::format("slant {:.4g} m vs field {:.3f} m "
                                 "(dev {:.2f} TECU, sigma_rep {:.2f} TECU)",
                                 slant_abs, sh_slant, dev_v, rep_sig));
            } else {
                if (admit_on) seen.insert(key);
                if (held_before.count(o.satellite)) {
                    ++n_held_released;
                    emit("dw_release_row_held", o.satellite, st.name, z, validate_k,
                         "release_tie_row",
                         fmt::format("slant {:.4g} m vs field {:.3f} m (dev {:.2f} TECU)",
                                     slant_abs, sh_slant, dev_v));
                }
            }
        }

        // --- Stash erase + refloat, once per released station (INC-1) ----------
        if (flags.released && reseed_on) {
            // Deviation stash: erase outright.  This is an actuator: the entry sets the
            // tie row's predicted deviation and its variance (r.innov = -x + dev_pred,
            // r.var = floor^2 + var_pred), neither of which is compensated by the
            // linearization-point algebra.  It also has to override the Iono-2 v3k
            // d5_keep policy, which an outright erase does by construction.
            const std::size_t n_stash = eraseStationRange(stash, st.name);
            ctr.reseed_stash_erased += static_cast<long long>(n_stash);

            // Ambiguity refloat, deferred one epoch.  The funnel it feeds is a per-epoch
            // driver local whose load-bearing consumer has already run, and a bare
            // pending-reseed mark issued here is cleared by the leveling harvest a few
            // lines later.  It is queued instead, as the Card-4 D5 innovation gate
            // queues its own post-solve detections.
            for (const auto& [amb_key, sidx] : st.sm->getAmbiguities()) {
                (void)sidx;
                if (pending.insert({static_cast<int>(i), amb_key}).second) ++n_rel;
            }
            ctr.reseed_refloats += n_rel;
            ++ctr.reseed_stations;
            ctr.reseed_slants += n_slant;
            emit("dw_release_reseed", "", st.name,
                 static_cast<double>(n_slant), eps, "reseed_refloat",
                 fmt::format("n_slants={} n_refloats={} n_stash_erased={} w={:.4f}",
                             n_slant, n_rel, n_stash, iono_deweight.weight(st.name)));
            log->debug("Network (dw release hygiene): station {} released at TOW {:.1f} — "
                       "{} slant state(s) rewritten from the field (bookkeeping), {} "
                       "ambiguity refloat(s) queued, {} deviation stash entr(ies) erased.",
                       st.name, se.time.tow, n_slant, n_rel, n_stash);
        }

        if (x_dirty) {
            st.sm->setStateVector(x);
            // Re-inflate the slant-iono variance of every rewritten LOS to break the
            // stale (iono, bias) covariance built up around the parked value, as the
            // slip path's satellite-wide reset does.
            const double reinf = rep_sig * tecu_to_L1m;
            std::sort(reinflate_sats.begin(), reinflate_sats.end());
            reinflate_sats.erase(std::unique(reinflate_sats.begin(), reinflate_sats.end()),
                                 reinflate_sats.end());
            for (const auto& s : reinflate_sats)
                st.sm->reinflateIonosphereVariance(s, reinf * reinf);
        }
        ctr.park_reset_decoupled += n_park_dc;
        ctr.park_reset_cold      += n_park_cold;
        ctr.rows_held            += n_held;
        ctr.rows_held_entered    += n_held_entered;
        ctr.rows_released        += n_held_released;
        ctr.admit_held           += n_admit_held;
        ctr.admit_admitted       += n_admitted;
        ctr.admit_rejected       += n_admit_rejected;
        ctr.admit_seeded         += n_seeded;
        ctr.admit_nodev          += n_admit_nodev;
    }
    // Latch after the whole sweep so every present station is seeded in the same pass
    // (the flag is read per LOS above, so flipping it mid-sweep would seed some
    // stations and screen others in the same epoch).
    if (admit_on && !ctx.carry.dw_tie_seeded) {
        ctx.carry.dw_tie_seeded = true;
        log->info("Network (dw release hygiene INC-3b): start/resume SEED — {} line(s) "
                  "of sight admitted unscreened on the first pass; the admission screen "
                  "applies from the next epoch, i.e. to LOS that couple to an ALREADY "
                  "ESTABLISHED field.", ctr.admit_seeded);
    }
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the ionosphere-deweight release hygiene.
int networkDwReleaseHygieneRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // --- Iono-deweight release hygiene run-end telemetry -----------------------
    // (docs/SPEC_deweight_release_hygiene.md).  At the event the spec was written
    // against, the station carrying the parked column was never deweighted, so INC-1
    // and INC-2 have no opportunity there by construction.  A run reporting zero
    // releases is therefore not evidence that the release response works or fails
    // (invariant #4).  The denominators (`epochs`, `gate_advances`, `releases_seen`) are
    // reported first, so "the seam never ran", "it ran and the gate never advanced" and
    // "the gate advanced and nothing ever released" are three distinguishable answers.
    if (!ctx.knobs.dw_hygiene_on) return 0;
    const auto& d = ctx.counters.dwr;
    log->info("Network (dw release hygiene): {} seam pass(es), {} gate advance(s), "
              "{} station release(s) seen.",
              d.epochs, d.gate_advances, d.releases_seen);
    if (ctx.knobs.dw_release_reseed)
        log->info("Network (dw release hygiene INC-1): {} station(s) reseeded — {} "
                  "slant state(s) rewritten from the field, {} deviation stash "
                  "entr(ies) erased, {} ambiguity refloat(s) queued and {} "
                  "delivered into the funnel.  NOTE the slant rewrite is "
                  "BOOKKEEPING, not an actuator: the producer forms every "
                  "innovation against the linearization point, so shifting it "
                  "leaves the reduced shared RHS algebraically unchanged.  INC-1's "
                  "actuators are the stash erase and the refloat.",
                  d.reseed_stations, d.reseed_slants, d.reseed_stash_erased,
                  d.reseed_refloats, d.refloats_drained);
    if (ctx.knobs.dw_park_bound_tecu > 0.0)
        log->info("Network (dw release hygiene INC-2): {} park-bound reset(s) while "
                  "DECOUPLED + {} while the gate was COLD (bound {:g} slant TECU; "
                  "largest reset {:.4g} slant TECU).",
                  d.park_reset_decoupled, d.park_reset_cold,
                  ctx.knobs.dw_park_bound_tecu, d.park_max_tecu);
    if (ctx.knobs.dw_release_validate_k > 0.0)
        log->info("Network (dw release hygiene INC-3): {} tie-row LOS-epoch(s) held "
                  "on the release screen (k = {:g}); {} hold(s) entered, {} released "
                  "on a LOS that finally passed.",
                  d.rows_held, ctx.knobs.dw_release_validate_k,
                  d.rows_held_entered, d.rows_released);
    if (ctx.knobs.dw_admit_validate_k > 0.0) {
        log->info("Network (dw release hygiene INC-3b): {} tie-row LOS-epoch(s) held "
                  "pending admission (k = {:g}), {} LOS admitted, {} REJECTED on a "
                  "harvested deviation over k.  Every rising satellite serves ONE "
                  "quarantine epoch, so `held` scales with the constellation churn; "
                  "only `rejected` is the guarded event.  {} LOS were admitted "
                  "UNSCREENED by the one-shot start/resume seed — on a short leg "
                  "that number being large relative to `admitted` means most of the "
                  "run's tie rows were never screened at all.  {} more were "
                  "admitted with NO deviation ever harvested (decoupled station or "
                  "no local column) — the guard failing open on absent evidence "
                  "rather than starving the row for the whole run.",
                  d.admit_held, ctx.knobs.dw_admit_validate_k,
                  d.admit_admitted, d.admit_rejected, d.admit_seeded,
                  d.admit_nodev);
        if (d.admit_admitted == 0)
            log->warn("Network (dw release hygiene INC-3b): ARMED but ZERO LOS were "
                      "ever admitted — every tie row in the run was withheld.  That "
                      "is a rank-starved solve, not a guard; check k and the "
                      "representativeness sigma (invariant #4).");
        else if (d.admit_rejected == 0)
            log->info("Network (dw release hygiene INC-3b): no LOS was ever rejected "
                      "— the screen had opportunity ({} quarantines) and found "
                      "nothing over k.  Loosen nothing on that basis; grade it on a "
                      "run that contains a known coupling event.", d.admit_held);
    }
    if (d.epochs == 0)
        log->warn("Network (dw release hygiene): ARMED but the seam ran on ZERO "
                  "epochs — the wiring never executed (invariant #4).");
    else if (d.gate_advances == 0 && ctx.knobs.dw_release_reseed)
        log->warn("Network (dw release hygiene): ARMED and the seam ran, but the "
                  "deweight gate NEVER advanced — no release edge can exist, so "
                  "INC-1 is inert regardless of what the other counters say.  Check "
                  "network.iono_constraint_deweight_adaptive.");
    else if (d.releases_seen == 0 && ctx.knobs.dw_release_reseed)
        log->info("Network (dw release hygiene INC-1): the gate advanced on {} "
                  "epoch(s) and no station ever released — the response had ZERO "
                  "OPPORTUNITY this run, which is NOT evidence that it works.  Grade "
                  "it only on a run containing a release (the DOY-129 day had 22 "
                  "engage / 15 release events; the 19:26 break was NOT one of them).",
                  d.gate_advances);
    return 0;
}

// Run-end report: the station-adaptive SH-VTEC constraint deweight.
int networkIonoDeweightRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkIonoDeweightGate& iono_deweight = v.run.engines.iono_deweight;
    const bool idw_on = v.run.engines.iono_deweight.active();
    const IonoConstraintDeweightConfig& idw = v.config.getIonoConstraintDeweightConfig();

    if (!idw_on) return 0;
    std::string idw_list;
    for (const auto& nm : iono_deweight.everDeweighted()) {
        if (!idw_list.empty()) idw_list += ' ';
        idw_list += nm;
    }
    if (idw.adaptive)
        log->info("Network (iono deweight): {} station(s) ever deweighted (peak {} "
                  "simultaneous), {} adaptive engage / {} release event(s) over the run "
                  "(static w_s={:.3f}; adaptive gate {:.2f}σ↑ / {:.2f}σ↓ → floor {:.3f}; "
                  "network-relative robust z-score, chronic >3σ background removed by the "
                  "median-centering).{}{}",
                  iono_deweight.everDeweighted().size(), iono_deweight.peakDeweighted(),
                  iono_deweight.triggerEvents(), iono_deweight.releaseEvents(),
                  iono_deweight.staticWeight(), idw.sigma_hi, idw.sigma_lo, idw.floor,
                  idw_list.empty() ? "" : "  Stations: ", idw_list);
    else
        log->info("Network (iono deweight v1): {} station(s) statically deweighted at "
                  "w_s={:.3f} for the whole run — the SH-iono coupling was relaxed while "
                  "they kept serving the shared clocks/biases/datum/position.{}{}",
                  iono_deweight.staticCount(), iono_deweight.staticWeight(),
                  idw_list.empty() ? "" : "  Stations: ", idw_list);
    return 0;
}

// Run-end report: the producer's state-manager re-inflations.
int networkReinflateRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const std::vector<NetStation>& stations = v.setup.stations;

    // The producer's state-manager re-inflations, summed over the stations (card OWD,
    // O2).  The deweight-release hygiene's ionosphere re-inflation is the only producer
    // caller; a kept count names a variance the rule before OWD would have deflated.
    StateManager::ReinflateCounters sum{};
    std::string kept_at;
    for (const auto& st : stations) {
        if (!st.sm) continue;
        const auto& c = st.sm->reinflateCounters();
        long long kept = 0;
        for (std::size_t k = 0; k < sum.size(); ++k) {
            sum[k].raised += c[k].raised;
            sum[k].kept   += c[k].kept;
            kept          += c[k].kept;
        }
        if (kept > 0)
            kept_at += fmt::format("{}{} {}", kept_at.empty() ? "" : ", ", st.name, kept);
    }
    log->info("Network (card OWD re-inflations (producer, {} station(s))): {}; stations "
              "with a kept re-inflation: {}.", stations.size(),
              StateManager::reinflateSummary(sum), kept_at.empty() ? "none" : kept_at);
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
