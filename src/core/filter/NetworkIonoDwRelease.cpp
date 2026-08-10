/**
 * @file NetworkIonoDwRelease.cpp
 * @brief Implementation of @ref networkIonoDwRelease — the iono-deweight RELEASE
 *        hygiene seam (docs/SPEC_deweight_release_hygiene.md).
 */

#include "filter/NetworkIonoDwRelease.hpp"

#include "app/NetworkRunContext.hpp"
#include "app/NetworkSetupResult.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
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

namespace GnssProcessor {

namespace {

/// One station's sweep disposition for this epoch.  A station enters the sweep for
/// any of four independent reasons and may satisfy several at once; the flags stay
/// apart because the counters must distinguish them (invariant #4).
struct SweepFlags {
    bool released = false;   ///< INC-1: its zero-weight era ended in this epoch's advance().
    bool decoupled = false;  ///< INC-2: weight <= eps right now (the spec's own trigger).
    bool cold = false;       ///< INC-2: the gate holds NO samples for it (post-warm-resume).
    bool screen = false;     ///< INC-3 / INC-3b: it owns held rows, or admission is armed.
};

/// Erase every "NAME|SAT" entry of one station from a "|"-delimited keyed container.
/// Both are lexicographically ordered, so a station's entries form one contiguous
/// range; the delimiter-terminated prefix is what makes it exact ("BSHM" must not
/// match "BSHM00ISR").  '|' is 0x7C, so ++back() gives the open upper bound 0x7D.
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

/// INC-3b: consecutive barren (no harvested deviation) epochs a quarantined LOS may
/// serve before it is admitted anyway.  A judgeable LOS is harvested in the SAME epoch
/// its row would fold, so 2 is already generous; the bound exists only to stop a LOS
/// whose harvest NEVER runs from being withheld for the whole run.
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
    if (!iw.enabled) return;                   // the tie rows this card guards are not built

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
    // networkShHealth skips the WHOLE gate block on a datum-change epoch (its guard is
    // `adaptive() && !datum_changed`) and there is no advance(false) caller in the
    // tree, so the gate's per-call release list retains the PREVIOUS epoch's contents.
    // Consuming it unguarded would re-fire an old release on every held epoch.
    const long long seq = iono_deweight.advanceCount();
    const bool advanced = (seq != ctx.carry.dw_gate_adv_seen);
    ctx.carry.dw_gate_adv_seen = seq;
    if (advanced) ++ctr.gate_advances;

    // --- Which stations does this epoch sweep? -------------------------------
    // Cheap by construction: no reconstructStation.  The two quantities this seam
    // needs are the SOLVED SH field (from x_s by column lookup) and the SOLVED
    // per-LOS deviation, which networkShDevVtec has ALREADY harvested into
    // ctx.carry.sh_dev_carry a few lines earlier this same epoch.  So the cost is one
    // SH evaluation per LOS of the selected stations — no cov consumer, which is
    // exactly what the H1 de-duplication removed from this part of the epoch.
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
            // starts EVERY station at w = 1 with zero samples and re-folds every tie row
            // on its first assembly — there is no release edge to hook, which is
            // precisely the spec's own stated reason for INC-2 to exist.  A station the
            // gate has never scored is exactly that condition (and, in a cold run, the
            // pre-min_epochs warm-up, where the bound is equally correct).
            const bool cold = (iono_deweight.samples(nm) == 0);
            if (decoupled || cold) {
                auto& f = sweep[i];
                f.decoupled = decoupled;
                f.cold = cold;
            }
        }
        // INC-3b sweeps EVERY present station: admission is about LOS this seam has
        // never seen, so it cannot be driven off a list of stations it already knows.
        if (admit_on) { sweep[i].screen = true; continue; }
        if (validate_k > 0.0 && !held.empty()) {
            const std::string lo = nm + "|";
            std::string hi = lo; ++hi.back();
            if (held.lower_bound(lo) != held.lower_bound(hi)) sweep[i].screen = true;
        }
    }
    if (sweep.empty()) return;

    // --- The solved SH field (both shells) -----------------------------------
    // Mirrors the health sweep's reconstruction exactly: coefficients out of x_s by
    // column lookup, then vtec(coeff, IPP) * mapping * tecu_to_L1m per shell.
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
        // No trustworthy field ⇒ no trustworthy screen.  Doing nothing is the safe
        // disposition, but it is a SILENT skip of an armed response — say so, and leave
        // every hold in place (a held row must never be admitted by an error path).
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
        40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
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

        // Both per-station sets are rebuilt from scratch, so a LOS that sets (or is
        // pruned) can never leave a stale hold or a stale admission behind.  The
        // satellites held / admitted going IN are kept so entered-vs-released is
        // attributed per LOS rather than inferred from a count difference (which a
        // same-epoch swap would hide).
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
            if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // SH excludes GLONASS
            if (o.elevation < iw.elevation_cutoff_deg) continue;              // the admission gate
            if (!o.sat_pos.allFinite()) continue;
            const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            const IppGeometry g0 = computeIppGeometry(st.ecef, o.sat_pos, Re, H);
            if (!g0.valid) continue;

            // Field evaluation for this LOS, in L1 slant metres — the same quantity the
            // assembly's tie row asserts the state should equal, and the same form the
            // emit path uses (vtec * mapping * tecu_to_L1m).
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

            // --- The screen statistic: the SOLVED deviation, not the state ------
            // The producer is a per-epoch ABSOLUTE solve whose per-station StateManager
            // is a frozen LINEARIZATION POINT, and every innovation is formed against
            // it (data rows via em.innovations; the tie row via r.innov = -x(iono)).  A
            // shift of x_lin by d therefore moves y by exactly -A_r e_c d and leaves the
            // reduced shared RHS u_s - N_sr N_rr^-1 u_r ALGEBRAICALLY UNCHANGED — the
            // absolute estimate x_lin + x_r is invariant.  So |x_lin - field| is a
            // frame-arbitrary number and must NOT be screened on.  The physical quantity
            // is the deviation of the ABSOLUTE slant from the field, which
            // networkShDevVtec harvested into the stash this same epoch as
            // dev = (x_lin + x_r - SH) / M0 / tecu_to_L1m, in vertical TECU.  Its
            // natural standardization is against the representativeness sigma, in the
            // same frame — dimensionless, elevation-stationary, and free.
            const auto dit = stash.find(key);
            const bool has_dev = (dit != stash.end() && dit->second.t_s > 0.0);
            const double dev_v = has_dev ? dit->second.dev : 0.0;
            const double z = (has_dev && rep_sig > 0.0) ? std::fabs(dev_v) / rep_sig : 0.0;
            const double slant_abs = sh_slant + dev_v * m0_tecu;   // absolute, L1 metres

            // --- INC-2: while-decoupled (or gate-cold) park bound --------------
            // The state is an L1-referenced SLANT delay in metres, so the physical
            // comparison is |slant| / tecu_to_L1m with NO mapping function — slant TECU.
            // Actuation is the STASH erase (the tie row's predicted deviation is a real,
            // uncompensated input to y and R); the state write beside it is bookkeeping.
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

            // --- INC-1: release-time reseed ------------------------------------
            // The state write is BOOKKEEPING, not an actuator (see the invariance note
            // above): it makes the frame-broken readouts honest and gives the next
            // structural change a sane starting point.  The actuators of INC-1 are the
            // stash erase and the ambiguity refloat, both applied per station below.
            if (flags.released && reseed_on) {
                x(iono_idx) = sh_slant;
                x_dirty = true;
                reinflate_sats.push_back(o.satellite);   // de-duplicated below
                ++n_slant;
            }

            if (!flags.screen) continue;

            // --- INC-3b: ADMISSION screen on a LOS's FIRST tie row --------------
            // The measured valve at DOY-129 TOW 329160 was not a deweight release
            // (BSHM sat at w = 1.0 throughout; its only IONO_DW event is an ENGAGE at
            // the break) — it was a satellite crossing the elevation cutoff and
            // entering BSHM's tie stack for the first time, coupling an unvalidated
            // slant column to the SH block.  So a tie row must EARN admission: a LOS
            // this seam has not admitted is held for one epoch, during which
            // networkShDevVtec still harvests its deviation (that harvest is gated on
            // the deweight weight, not on whether the row folded), and it is admitted
            // at the end of that epoch iff the harvested deviation passes k.
            if (admit_on && !ctx.carry.dw_tie_seeded) {
                // One-shot start / resume SEED: admit every present LOS unscreened.
                // See NetworkCarryState::dw_tie_seeded — at start there is no
                // established field for a "first coupling" to be relative to, and
                // holding every row for an epoch would be a far larger shock than the
                // event guarded against.  Counted apart from a real admission.
                seen.insert(key);
                ++n_seeded;
                continue;
            }
            if (admit_on && !seen_before.count(o.satellite)) {
                if (!has_dev) {
                    // No statistic to judge by.  networkShDevVtec skips its harvest for
                    // a station at weight <= 0 and for a LOS with no local column, so
                    // for those the deviation NEVER arrives — and holding on missing
                    // data withholds the row for the whole run (measured: 58 consecutive
                    // epochs, CPVG00CPV|G30).  Reject on evidence, not on its absence:
                    // bound the barren wait, then admit and count apart.  A LOS the
                    // screen could actually judge is harvested in the SAME epoch its row
                    // would fold, so this never fails open for a judgeable one.
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

            // --- INC-3: release screen on an already-admitted LOS ---------------
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

        // --- INC-1: stash erase + refloat, once per released station ------------
        if (flags.released && reseed_on) {
            // Deviation stash: erase outright.  This IS an actuator — the entry sets the
            // tie row's predicted deviation and its variance (r.innov = -x + dev_pred,
            // r.var = floor^2 + var_pred), neither of which is compensated by the
            // linearization-point algebra.  It must also override the Iono-2 v3k
            // d5_keep policy, which an outright erase does by construction.
            const std::size_t n_stash = eraseStationRange(stash, st.name);
            ctr.reseed_stash_erased += static_cast<long long>(n_stash);

            // Ambiguity refloat, DEFERRED one epoch.  The funnel it must feed is a
            // per-epoch driver local whose load-bearing consumer already ran, and a bare
            // pending-reseed mark issued here is wiped by the leveling harvest a few
            // lines later — so queue it, exactly as the Card-4 D5 innovation gate queues
            // its own post-solve detections.
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
            // Break the stale (iono, bias) covariance partition the parked value baked
            // in.  Same doctrine as the slip path's sat-wide reset.
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
    // Latch AFTER the whole sweep so every present station is seeded in the same pass
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

} // namespace GnssProcessor
