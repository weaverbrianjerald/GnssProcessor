/**
 * @file ApplicationNetworkAssembly.cpp
 * @brief Assembly-phase member functions of Application::runNetwork — the
 *        pre-loop column-split / iono-row helpers plus (as INC-7 lands) PASS-A
 *        geometry, the observability prefilter, the shared-column
 *        registration, and PASS-B produce/consume.
 *
 * Decomposition INC-7 (2026-07-16) — bodies relocated VERBATIM from
 * ApplicationNetwork.cpp (see docs/network_decomposition_plan.md).
 * Former run-TU [&] lambdas become private Application member functions:
 * ambient captures become explicit parameters or member reads (capture→param
 * map recorded in the INC-7 cut spec); bodies are textually unchanged.  The
 * include block mirrors ApplicationNetwork.cpp (INC-1 precedent); unused-
 * include cleanup is deferred to INC-12.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // Card 4 (M1) phase D: emit-layer frame re-base
#include "products/NetworkPbEmitFilter.hpp" // M5 prereq: PB emission-continuity filter
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // M4: combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // RTB-7: atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // Card 5 (RTF-1): broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // Card 5 (RTF-1): RINEX-nav → LiveEphemerisStore loader
#include "filter/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkExtraBandScope.hpp"  // card L5P: the extra-band / primary-pair predicates
#include "filter/NetworkSecondCodeScope.hpp" // card SCC: secondCodeCellEdges (the two cell edges)
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/NetworkSdEwlAr.hpp"               // SD EWL: the shared EWL band table (sdEwlBands) + increment-2a decision
#include "filter/NetworkCoupling.hpp"       // INC-7b: Card-1/1b/1d conditional coupling-row builder
#include "filter/NetworkPrefitCensus.hpp"   // INC-2 structural fuse (SPEC_prefit_census_observability)
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // INC-2: run-loop record types (NetStation / StationAssembly / carry PODs)
#include "filter/NetworkRobustStats.hpp"   // INC-2: median/MAD robust attribution helpers
#include "logging/CsvRotator.hpp"     // P1: size-based rotation of the unbounded service CSVs
#include "logging/NetworkRunTraces.hpp" // INC-3: trace-stream ownership + open logic + roster + FlagRate
#include "app/NetworkRunContext.hpp"    // INC-4: knobs + Tier-1 carry state + service counters (ctx)
#include "app/NetworkSetupResult.hpp"   // INC-5: setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/MwLaneTiers.hpp"         // UWL-1 per-constellation lane-tier band table
#include "slip/MwSlipDetector.hpp"      // T0 phase-leveling slip repair (Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // INC-2: slip-QC pure helpers (ifElevMap / parseIfPairResetKey)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

bool Application::networkIsGaugePinned_(const std::vector<NetStation>& stations,
                                        bool phase_datum_prioritized, size_t i,
                                        const std::string& sat,
                                        const std::string& signal) const {
    if (!phase_datum_prioritized) return stations[i].is_pivot;
    return stations[i].pinned_amb_targets.count(sat + "_" + signal) != 0;
}

// Column-split helper: split a station's H into the shared border A_s
// (clock/phase-bias columns) and the local nuisance block A_r (everything
// except the 3 fixed-position columns and the shared columns).  Returns the
// H-column → A_r-column map (used for the pivot ambiguity pins).
void Application::networkSplitStation_(NetStation& st, const NetworkEstimator& est,
                                       const EpochMeasurement& em,
                                       Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                       Eigen::VectorXd& w, Eigen::VectorXd& y,
                                       std::map<int, int>& h2local) const {
    const Eigen::Index m       = em.H.rows();
    const Eigen::Index nstate  = em.H.cols();
    const Eigen::Index nshared = est.numShared();
    A_s = Eigen::MatrixXd::Zero(m, nshared);

    std::set<int> shared_cols;  // H-column indices that map to the shared border
    for (const auto& kv : st.sm->getSatClockStates()) {
        const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
        Eigen::Index col;
        try { col = est.clockColumn(sat); } catch (...) { continue; }
        A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
        shared_cols.insert(static_cast<int>(kv.second));
    }
    for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(kv.first);
        if (!parts) continue;
        Eigen::Index col;
        try { col = est.phaseBiasColumn(parts->satellite, parts->signal); }
        catch (...) { continue; }
        A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
        shared_cols.insert(static_cast<int>(kv.second));
    }
    // Satellite CODE bias (W6): scatter each per-(sat,signal) sat-bias column
    // into the shared border.  In network mode getSatBiasStates() holds ONLY
    // the network-owned satellite code biases (the single-receiver fallback is
    // gated off in ensureRequiredStates); GLONASS gets none (code-only scope).
    for (const auto& kv : st.sm->getSatBiasStates()) {
        const auto parts = StateKeys::parseSatBiasKey(kv.first);
        if (!parts) continue;
        Eigen::Index col;
        try { col = est.codeBiasColumn(parts->satellite, parts->signal); }
        catch (...) { continue; }
        A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
        shared_cols.insert(static_cast<int>(kv.second));
    }

    // Receiver inter-frequency-bias (IFB) columns on NON-primary bands: when
    // estimating the satellite code bias, drop the receiver IFB on every band
    // outside the iono-free reference pair (the "sys_IFB<band>" states).  An
    // EXPLICIT non-primary receiver IFB drains the code information that
    // otherwise separates that band's satellite clock from its phase bias —
    // regressing the L5/E5b/... phase-bias product to a near-gauge (verified:
    // keeping it drives the per-constellation L5Q phase-bias common mode below
    // the null-space cutoff and withholds the whole epoch).  Dropping it folds
    // the receiver's non-primary code delay into the per-(rx,constellation)
    // slant-iono common mode the GIM weighting deliberately leaves free (the
    // L1-referenced convention); the SATELLITE code bias on that band stays
    // observable through the GIM's BETWEEN-satellite iono structure (a per-
    // satellite quantity, so the per-receiver delay averages out across the
    // network), not through an explicit receiver counterpart.  The two primary-
    // band receiver DCB ("sys_DCB") is kept, and the GLONASS per-slot IFB (a
    // different state family) is untouched.
    std::set<int> drop_cols;
    if (config_.estimateNetworkSatCodeBias()) {
        for (const auto& kv : st.sm->getCodeBiasStates()) {
            if (StateKeys::isGlonassPerSlotIfbKey(kv.first)) continue;
            if (kv.first.find("_IFB") != std::string::npos)
                drop_cols.insert(static_cast<int>(kv.second));
        }
    }

    const int pos = static_cast<int>(st.sm->getPositionIndex());  // 0
    std::vector<int> local_cols;
    local_cols.reserve(static_cast<size_t>(nstate));
    for (int c = 0; c < static_cast<int>(nstate); ++c) {
        if (c == pos || c == pos + 1 || c == pos + 2) continue;  // coords fixed
        if (shared_cols.count(c)) continue;                      // shared border
        if (drop_cols.count(c)) continue;                        // non-primary rx IFB (dropped)
        // Drop a structurally-unobservable local state (all-zero H column this
        // epoch).  The usual case: the Galileo inter-system-bias state of a
        // station that tracks no Galileo this epoch (the StateManager allocates
        // an ISB per active non-GPS system regardless of per-station, per-epoch
        // visibility); also a stale ambiguity/iono column for a satellite the
        // cross-station observability gate just removed.  Keeping the dead
        // column makes the local normal block N_rr singular, and the kernel
        // then rejects the WHOLE station — throwing away its perfectly good GPS
        // observations (this is why GPS-only NVSK00RUS / MDVJ00RUS were dropped
        // wholesale).  Excluding ONLY the dead column lets the station
        // contribute every observable state it has; its receiver clock absorbs
        // its GPS rows, so the GPS clock common-mode gauge stays intact.
        if (em.H.col(c).cwiseAbs().maxCoeff() == 0.0) continue;
        h2local[c] = static_cast<int>(local_cols.size());
        local_cols.push_back(c);
    }
    // --- Linearity invariant (A6, insurance) --------------------------------
    // Every column retained in local_cols becomes a column of A_r, i.e. a
    // parameter the Schur kernel treats as LINEAR in the observations.  That is
    // valid ONLY because the receiver position is held fixed at its known ECEF:
    // the 3 position columns (pos..pos+2) are dropped above ("coords fixed"), so
    // A_r contains only genuinely-linear nuisances (rx clock, tropo ZWD, per-sat
    // slant iono, ambiguities, code/phase biases, ISBs).  A position column is
    // NONLINEAR in the geometry (its H entry is the line-of-sight unit vector,
    // re-linearized each epoch about a moving point) and must never enter A_r for
    // a fixed-coordinate station.  Should a future float-coordinate (non-fiducial)
    // station ever survive here, the fixed-linear-block assumption is violated and
    // the estimate would be silently wrong — hard-fail instead.  This scan never
    // fires on the current fixed-coord path (the loop above already `continue`s on
    // pos..pos+2), so it is byte-identical; the throw rides the produce-phase
    // try/catch (per-station, thread-safe) that dispositions a failed station.
    for (int lc : local_cols) {
        if (lc == pos || lc == pos + 1 || lc == pos + 2) {
            throw std::runtime_error(fmt::format(
                "Network splitStation: position column {} of station '{}' survived "
                "into the linear local block A_r. The network holds all station "
                "coordinates fixed; a position column here breaks the "
                "linear-in-observation invariant of A_r (is_pivot={}).",
                lc, st.name, st.is_pivot));
        }
    }
    A_r.resize(m, static_cast<Eigen::Index>(local_cols.size()));
    for (size_t j = 0; j < local_cols.size(); ++j)
        A_r.col(static_cast<Eigen::Index>(j)) = em.H.col(local_cols[j]);

    w = em.R_v.cwiseInverse();   // per-obs information = 1/σ²
    // --- flicker/gap signal integrity INC-2 (docs/SPEC_flicker_gap_integrity.md) ---
    // Demote the PHASE rows of every line of sight that is currently UNVETTED — one
    // that re-entered, flickered a band, is degraded, or is brand new, i.e. one
    // inside the structural window where NO time-difference detector can operate (MW
    // needs Welford history, IF-TD needs three consecutive epochs, LLI is
    // receiver-discretionary, GF is doctrine-barred at 30 s).  Across a loss of lock
    // the slip magnitude is unbounded and unknowable — a proportional counter reset
    // is invisible to BOTH MW and GF — so the only safe posture is to stop trusting
    // the phase until something can watch it again.
    //
    // CODE rows are deliberately untouched (`sig[0] != 'L'`): they are slip-immune,
    // and they are what re-anchors the float to a bounded error.  This is
    // prefer-fallback, not exclusion — the row still contributes, at σ × scale.
    //
    // This is the ONLY place that needs it, and the cheapest: `em.satellite_ids` /
    // `em.signal_types` are row-aligned with H/R_v by an asserted invariant, so
    // phase-vs-code and the satellite are O(1) lookups with no matrix scan — the
    // three existing post-assembly sites (WE-CI, frame-diag) each have to re-derive
    // "is this a phase row" from shared-column label archaeology.  Everything folded
    // later — the datum pins, the GLONASS anchors, the SH/GIM iono rows, the SD-EWL
    // rows, the Card-1 coupling rows — APPENDS, so rows [0, em.H.rows()) are
    // structurally the data rows and nothing else can be caught here.
    //
    // Thread-safety: `st.phase_sigma_scale` is republished SERIALLY by
    // @ref networkSignalIntegrity before the parallel produce and only find-read here
    // (the pinned_amb_sigma_scale / iono-deweight contract).  OFF byte-identity: the
    // map is never populated when the card is disarmed, so `w` keeps the exact
    // cwiseInverse result with zero additional floating-point operations.
    if (!st.phase_sigma_scale.empty()) {
        const Eigen::Index mr = std::min<Eigen::Index>(
            m, static_cast<Eigen::Index>(em.signal_types.size()));
        long long n_dem = 0;
        for (Eigen::Index r = 0; r < mr; ++r) {
            const std::string& sig = em.signal_types[static_cast<std::size_t>(r)];
            if (sig.empty() || sig[0] != 'L') continue;          // CODE UNTOUCHED
            if (r >= static_cast<Eigen::Index>(em.satellite_ids.size())) break;
            const auto sc = st.phase_sigma_scale.find(
                em.satellite_ids[static_cast<std::size_t>(r)]);
            if (sc == st.phase_sigma_scale.end() || !(sc->second > 1.0)) continue;
            w(r) /= sc->second * sc->second;                      // σ × scale
            ++n_dem;
        }
        st.si_rows_demoted += n_dem;
    }
    // --- card GL: the gross line-of-sight CODE deweight ------------------------
    // (docs/SPEC_event_harness.md §24.18.)  The exact CODE counterpart of the
    // block above, and the ONLY actuator in this producer that can bound a gross
    // CODE error on one line of sight: every post-fit tier screens carried-
    // ambiguity PHASE rows (NetworkRaim.cpp:328's |A_r(row, amb_col)| > 0.5), so a
    // code-only fault is not under-weighted there, it is unreachable.  A LOS that
    // @ref networkGrossLosScreen declared gross folds its code rows at σ × f, a
    // CAP and not a drop (prefer-fallback): the row keeps its sign and still
    // re-anchors the float if the LOS's phase side goes away.
    //
    // PHASE rows are deliberately untouched here — the declaration already erased
    // the LOS's carried levels and marked its ambiguities pending-reseed, which is
    // the phase side's response; demoting the phase rows too would remove the very
    // information the refloat needs to re-draw from.
    //
    // Same thread-safety contract as `phase_sigma_scale`: `gl_code_sigma_scale` is
    // republished SERIALLY (end of the previous epoch's QC block) and only
    // find-read here.  OFF byte-identity: the map is never populated when
    // network.gross_los_screen is off, so `w` keeps the exact cwiseInverse result
    // with zero additional floating-point operations.
    if (!st.gl_code_sigma_scale.empty()) {
        const Eigen::Index mr = std::min<Eigen::Index>(
            m, static_cast<Eigen::Index>(em.signal_types.size()));
        long long n_dw = 0;
        for (Eigen::Index r = 0; r < mr; ++r) {
            const std::string& sig = em.signal_types[static_cast<std::size_t>(r)];
            if (sig.empty() || sig[0] == 'L') continue;           // PHASE UNTOUCHED
            if (r >= static_cast<Eigen::Index>(em.satellite_ids.size())) break;
            const auto sc = st.gl_code_sigma_scale.find(
                em.satellite_ids[static_cast<std::size_t>(r)]);
            if (sc == st.gl_code_sigma_scale.end() || !(sc->second > 1.0)) continue;
            w(r) /= sc->second * sc->second;                      // σ × scale
            ++n_dw;
        }
        st.gl_rows_deweighted += n_dw;
    }
    // --- card L5P: the EXTRA-BAND code deweight -------------------------------
    // (docs/refound/L5P_l5_phase_posture_2026-09-21.md section 1.)  The code rows
    // of a signal whose band is neither band of the constellation's
    // ionosphere-free pin pair fold at σ × scale.  This is the ONE place the row
    // weight is formed, so the solve, the pre-fit census and every post-fit screen
    // read the same number — they all read R.w, which is this vector.
    //
    // WHY CODE ONLY, and why here rather than at the measurement model.  The card
    // separates two things the band-5-code-removed emulation removed together: the
    // band-5 PAIRS of the wide-lane detector (the mw_detect_pairs knob) and the
    // pull the band-5 CODE rows have on the shared states.  With B5D armed the
    // band-5 code-bias column is determined by those same rows, so deweighting
    // them keeps the wire's band-5 code bias — a drop would not — while shrinking
    // their contribution to the clock, the code biases and the slant ionosphere.
    // The band-5 PHASE rows are deliberately untouched: the posture exists to USE
    // the L5 phase.
    //
    // Thread-safety: networkPinBandsFor is a const read of the product cache's
    // clock-reference map, filled once at product load and never mutated in the
    // epoch loop — the same contract StateInitializer::resolveReceiverBias already
    // relies on from this parallel produce.  The per-system result is memoized
    // locally so each station resolves each constellation once.
    //
    // OFF byte-identity: at the default 1.0 the whole block is skipped, so `w`
    // keeps the exact cwiseInverse result with zero additional floating-point
    // operations.
    const double eb_scale = config_.networkExtraBandCodeSigmaScale();
    if (eb_scale > 1.0) {
        const Eigen::Index mr = std::min<Eigen::Index>(
            m, static_cast<Eigen::Index>(em.signal_types.size()));
        std::map<char, NetworkPinBands> pin_by_sys;
        long long n_eb = 0, n_nopin = 0;
        for (Eigen::Index r = 0; r < mr; ++r) {
            const std::string& sig = em.signal_types[static_cast<std::size_t>(r)];
            if (sig.empty() || sig[0] == 'L') continue;           // PHASE UNTOUCHED
            if (r >= static_cast<Eigen::Index>(em.satellite_ids.size())) break;
            const std::string& sat = em.satellite_ids[static_cast<std::size_t>(r)];
            if (sat.empty()) continue;
            auto pit = pin_by_sys.find(sat[0]);
            if (pit == pin_by_sys.end())
                pit = pin_by_sys.emplace(sat[0], networkPinBandsFor(sat[0])).first;
            // Invariant #4: a constellation with no resolvable pin pair is COUNTED,
            // never silently treated as all-extra-band or as all-primary.
            if (!pit->second.valid()) { ++n_nopin; continue; }
            if (!networkSignalIsExtraBand(sig, pit->second)) continue;
            w(r) /= eb_scale * eb_scale;                           // σ × scale
            ++n_eb;
        }
        st.eb_rows_deweighted += n_eb;
        st.eb_rows_no_pin     += n_nopin;
    }
    // --- card SCC: the SECOND CODE's rows (R5 + R7) ---------------------------
    // (docs/refound/SCC_second_code_channel_2026-09-21.md.)  Two things at one
    // walk, because both need the same test: which rows route through the receiver
    // INTER-SIGNAL state rather than through the per-band DCB.
    //
    // The test reads the STATE SET, not the configuration: a row routes through the
    // inter-signal state exactly when this station holds "<system>_ISC_<signal>",
    // which is the state @c StateInitializer::resolveReceiverBias allocated from the
    // same predicate.  Deriving it a second time from the allow-list and the latch
    // would be a second copy of that predicate, free to drift; reading the states
    // cannot disagree with the Jacobian.
    //
    // R5 scales those rows' code sigma.  R7 splits them into COMPANION rows (the
    // cell was republished serially into st.scc_cells after PASS A) and FALLBACK
    // rows (an allow-listed band-1 code that arrived with no canonical beside it —
    // R2 routes it through the same state, and a nonzero count is a finding about
    // the canonical latch, not about this card).  The counters run whether or not
    // the scale is applied: a scale of 1 is the card's default posture, not an
    // unarmed one.
    //
    // OFF ⇒ the whole block is skipped ⇒ byte-identical, and with the knob armed at
    // scale 1 the weights are untouched (no division is performed at all).
    if (config_.networkSecondCodeChannel() && st.sm) {
        std::set<std::string> isc_sigs;   // signals with an inter-signal state HERE
        for (const auto& [key, idx] : st.sm->getCodeBiasStates()) {
            (void)idx;
            if (const auto p = StateKeys::parseReceiverIscKey(key))
                isc_sigs.insert(p->signal);
        }
        if (!isc_sigs.empty()) {
            const double sc_scale = config_.networkSecondCodeSigmaScale();
            const Eigen::Index mr = std::min<Eigen::Index>(
                m, static_cast<Eigen::Index>(em.signal_types.size()));
            long long n_comp = 0, n_fall = 0;
            for (Eigen::Index r = 0; r < mr; ++r) {
                const std::string& sig = em.signal_types[static_cast<std::size_t>(r)];
                if (sig.empty() || sig[0] == 'L') continue;           // PHASE UNTOUCHED
                if (!isc_sigs.count(sig)) continue;
                if (r >= static_cast<Eigen::Index>(em.satellite_ids.size())) break;
                const std::string& sat = em.satellite_ids[static_cast<std::size_t>(r)];
                if (sat.empty()) continue;
                if (st.scc_cells.count(StateKeys::makeSatBiasKey(sat, sig))) ++n_comp;
                else                                                         ++n_fall;
                if (sc_scale > 1.0) w(r) /= sc_scale * sc_scale;       // σ × scale
            }
            st.scc_rows          += n_comp;
            st.scc_fallback_rows += n_fall;
        }
    }
    y = em.innovations;
}

// GIM ionosphere pseudo-observation (W6 satellite code-bias prerequisite).
// Ports Application::applyGimIonoUpdate's common-mode-free R into tight-prior
// rows folded into a station's OWN block: one row per GPS/Galileo satellite
// with a usable slant-iono column and a valid GIM value, with a per-
// constellation R = diag(var) + σ_c²·11ᵀ.  The all-ones block leaves the
// per-constellation common mode (the receiver iono-datum level) UNCONSTRAINED,
// imposing only the GIM's between-satellite structure — exactly what separates
// the satellite DCB from the slant iono while leaving the constellation-wide
// DCB level for the explicit sum-to-zero datum.  The correlated R is whitened
// per constellation (L⁻¹ from LLᵀ=R) into unit-information rows so it stays in
// the station's diagonal-weight block.  GLONASS is handled by its own loose
// anchors above (no satellite code bias).  Built into the station's own block —
// no shared write, safe in the parallel produce phase (GIM query is const).
// SD PRE-REDUCE EWL constraint (docs/SPEC_sd_ewl_prereduce.md).  Between-satellite
// single-differenced extra-wide-lane MW statistic as a pseudo-observation row in this
// station's OWN pre-reduce block.  See the header for the algebra and the thread-safety
// contract.  Default OFF => returns immediately => byte-identical.
void Application::networkAppendSdEwlRows_(NetStation& st, const NetworkEstimator& est,
                                          const std::vector<PppObservation>& sta_obs,
                                          const std::map<int, int>& h2local,
                                          const GnssTime& t,
                                          Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                          Eigen::VectorXd& w, Eigen::VectorXd& y,
                                          std::vector<std::string>& trace_rows,
                                          const std::map<std::pair<std::size_t, char>,
                                                         std::map<std::string, SdEwlFix>>* fix,
                                          std::size_t station_index) const {
    const auto& cfg = config_.getNetworkConfig().sd_ewl;
    if (!cfg.enable) return;
    auto& ctr = st.sd_ewl_ctr;
    // Increment 2b: the three trailing trace columns exist ONLY when the integer path
    // is armed, so an increment-1 artifact set stays byte-identical (the header in
    // NetworkRunTraces::openAll follows the same rule).
    const bool fix_on = cfg.fix_enable && fix != nullptr;
    // Per-satellite trace sink (§6).  Formatted here, drained by PASS B's SERIAL
    // consume — the produce phase must not touch a shared stream.
    const auto tr2 = [&](const char* status, const std::string& sat,
                         const std::string& pivot, const std::string& s1,
                         const std::string& s2, const std::string& c1,
                         const std::string& c2, std::size_t n_cand,
                         std::size_t n_arc, bool bia, double lam, double mean_m,
                         double pred_m, double resid_m, double sigma_m,
                         long long n_fix, bool row_fixed, bool grp_fixed) {
        if (!cfg.trace) return;
        std::string row = fmt::format(
            "{},{:.3f},{},{},{},{},{},{},{},{},{},{},{},{},{:.4f},{:.4f},{:.4f},"
            "{:.4f},{:.4f}",
            t.week, t.tow, st.name, sat.empty() ? '?' : sat.front(), sat, pivot,
            status, s1, s2, c1, c2, n_cand, n_arc, bia ? 1 : 0, lam, mean_m, pred_m,
            resid_m, sigma_m);
        if (cfg.fix_enable)
            row += fmt::format(",{},{},{}", n_fix, row_fixed ? 1 : 0, grp_fixed ? 1 : 0);
        row += '\n';
        trace_rows.push_back(std::move(row));
    };
    const auto tr = [&](const char* status, const std::string& sat,
                        const std::string& pivot, const std::string& s1,
                        const std::string& s2, const std::string& c1,
                        const std::string& c2, std::size_t n_cand,
                        std::size_t n_arc, bool bia, double lam, double mean_m,
                        double pred_m, double resid_m, double sigma_m) {
        tr2(status, sat, pivot, s1, s2, c1, c2, n_cand, n_arc, bia, lam, mean_m,
            pred_m, resid_m, sigma_m, 0, false, false);
    };
    // The MW arc accumulator is built only for NON-pivot stations, and only under
    // phase_leveling && (mw_detect || turbo_edit).  Count the station-epoch and bail —
    // an uncounted skip is how a feature ends up armed and dead (invariant #4).
    if (!st.mw_slip || !st.sm) {
        ++ctr.skip_no_detector;
        tr("NO_DETECTOR", {}, {}, {}, {}, {}, {}, 0, 0, false, 0.0, 0.0, 0.0, 0.0, 0.0);
        return;
    }

    // --- The extra-wide-lane band pair per constellation (the UWL-1 tier table). ------
    // Shared with the increment-2a integer decision via @ref sdEwlBands (one definition,
    // so the row and the integer folded into it can never disagree about which lane
    // they are on).  GPS (L2,L5) lambda_EWL 5.861 m; Galileo (E5b,E5a) 9.768 m; BeiDou
    // deferred and GLONASS excluded, both by simply producing no rows.
    auto bandsFor = [](char cons, char& b1, char& b2, double& f1, double& f2) -> bool {
        return sdEwlBands(cons, b1, b2, f1, f2);
    };

    // One resolved arc: the ZD-consistent EWL statistic plus the four state columns the
    // row needs (two local ambiguities, and the shared PB/CB pair per band).
    struct Arc {
        std::string sat;
        double      mean_m{ 0.0 };
        double      var_m2{ 0.0 };
        std::size_t count{ 0 };
        bool        bia{ false };
        double      lam_ewl{ 0.0 }, lam_b1{ 0.0 }, lam_b2{ 0.0 };
        double      f1{ 0.0 }, f2{ 0.0 };
        int         amb_col_b1{ -1 }, amb_col_b2{ -1 };
        Eigen::Index pb_b1{ -1 }, pb_b2{ -1 }, cb_b1{ -1 }, cb_b2{ -1 };
        /// This arc's LINEARIZED prediction of the MW observable, i.e. its half of
        /// @f$H\,x_{lin}@f$ — see the correction-frame note at the row build.  Split in
        /// two because increment 2b's fixed row DROPS its local ambiguity coefficients,
        /// and the linearization point must lose exactly the same terms in the same
        /// place or the row is folded in a mixed frame.
        double      pred_amb_m{ 0.0 };   ///< the two local ambiguity terms of H·x_lin
        double      pred_bias_m{ 0.0 };  ///< the four shared PB/CB terms of H·x_lin
        double      pred_m{ 0.0 };       ///< pred_amb_m + pred_bias_m (the float row's frame)
        // Trace-only provenance (§6): the elected phase pair, the code channels the
        // MW actually consumed, and how many candidate pairs were searched.
        std::string sig1, sig2, code1, code2;
        std::size_t n_cand{ 0 };
        // Increment 2b: the published integer for this arc, if any.  `n_cyc` is in the
        // r0-anchored frame, so only the between-satellite DIFFERENCE against the
        // group's pivot is meaningful.
        bool        has_fix{ false };
        long long   n_fix_cyc{ 0 };
        /// The integer came from a ROUNDED double difference rather than from a frame
        /// choice.  A fixed group must contain at least one of these (see the group
        /// split below) or it asserts a hard constraint with no measurement behind it.
        bool        fix_measured{ false };
    };

    // --- Correction frame -------------------------------------------------------------
    // The station's whole block is in the CORRECTION frame: networkSplitStation_ fills
    // y from `em.innovations` = z − h(x_lin), with x_lin = st.sm->getStateVector().  A
    // pseudo-observation must obey the SAME convention or it asserts an absolute value
    // against a design that solves for a correction — here catastrophically so, because
    // the raw MW mean carries λ_EWL·N_EWL with N_EWL in the thousands of cycles
    // (measured |z_SD| mean 203 m, max 4162 m, against a 0.13 m row σ).
    //
    // Which columns need the x_lin term: the LOCAL ambiguity states are seeded from
    // code-minus-phase and reseeded on slips, so theirs is large and load-bearing —
    // exactly the term NetworkMwWlLayer subtracts as `z_lin` (its z_corr = z_mw − z_lin).
    // The SHARED satellite phase/code-bias states are allocated at 0 and never written
    // back from the solve (MeasurementModel: "Allocated at value 0 ... a SHARED state the
    // network solves"), so their term is identically zero today — it is included anyway
    // rather than asserted, so a future seeding change cannot silently skew the frame.
    const Eigen::VectorXd x_lin = st.sm->getStateVector();
    const auto xAt = [&x_lin](int idx) -> double {
        return (idx >= 0 && idx < static_cast<int>(x_lin.size())) ? x_lin(idx) : 0.0;
    };
    // Shared PB/CB STATE indices at this station (distinct from the estimator's border
    // COLUMN indices): a column may be registered by another station, in which case this
    // station holds no state and its x_lin contribution is 0 — which is precisely what
    // its real rows do (em.H has no such column, so the innovations omit it).
    std::map<std::pair<std::string, std::string>, int> pb_state, cb_state;
    for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
        const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
        if (p) pb_state[{ p->satellite, p->signal }] = static_cast<int>(kv.second);
    }
    for (const auto& kv : st.sm->getSatBiasStates()) {
        const auto p = StateKeys::parseSatBiasKey(kv.first);
        if (p) cb_state[{ p->satellite, p->signal }] = static_cast<int>(kv.second);
    }
    const auto stateOf = [](const std::map<std::pair<std::string, std::string>, int>& m,
                            const std::string& sat, const std::string& sig) -> int {
        const auto it = m.find({ sat, sig });
        return it == m.end() ? -1 : it->second;
    };

    // --- This station's per-(satellite, band) ambiguity candidates ---------------------
    // Built ONCE from StateManager's own key map (the authority) rather than by
    // reconstructing a key string per candidate: the ambiguity key format is the state
    // layer's business, and a reconstructed key that does not round-trip fails SILENTLY
    // (every arc looks absent).  A candidate additionally needs a LOCAL COLUMN this
    // epoch — h2local presence is exactly "this arc has a phase row in A_r now".
    // Sorted ascending by signal name so the pair search below is a strict total order
    // (byte-identity: an insertion-order-dependent pick changes the row set run to run).
    // Entries are (signal, A_r local column, global state index) — the state index is
    // needed for the correction-frame x_lin term, the local column for the design.
    using Cand = std::tuple<std::string, int, int>;
    std::map<std::pair<std::string, char>, std::vector<Cand>> cand;
    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
        const auto parts = StateKeys::parseAmbiguityKey(key);
        if (!parts) continue;
        const std::string& sig = parts->signal;
        if (sig.size() < 2 || sig[0] != 'L') continue;
        const auto hit = h2local.find(static_cast<int>(sidx));
        if (hit == h2local.end()) continue;         // no phase row on this arc this epoch
        cand[{ parts->satellite, sig[1] }].emplace_back(sig, hit->second,
                                                        static_cast<int>(sidx));
    }
    for (auto& kv : cand)
        std::sort(kv.second.begin(), kv.second.end());   // ascending signal name first

    // --- Resolve every eligible arc, grouped by constellation (sorted => determinism). -
    // The MW arc key carries the two PHASE signal names, and which channel the detector
    // holds an arc on is ITS choice (collectUncombinedPairs' KF-chosen / attribute-
    // priority pick), not ours.  So SEARCH the candidate pairs in ascending order and
    // take the first that actually HAS a usable arc, instead of electing one channel and
    // declaring the arc missing when the detector picked the other.  A one-channel band
    // (the common case) has exactly one candidate, so this reduces to the direct lookup.
    std::map<char, std::vector<Arc>> by_const;
    for (const auto& o : sta_obs) {
        if (o.satellite.empty()) continue;
        const char cons = o.satellite.front();
        char b1 = 0, b2 = 0;
        double fb1 = 0.0, fb2 = 0.0;
        if (!bandsFor(cons, b1, b2, fb1, fb2)) continue;
        ++ctr.sat_seen;

        const auto c1it = cand.find({ o.satellite, b1 });
        const auto c2it = cand.find({ o.satellite, b2 });
        if (c1it == cand.end() || c2it == cand.end()) {
            ++ctr.skip_missing_amb_col;
            tr("NO_AMB_COL", o.satellite, {}, {}, {}, {}, {}, 0, 0, false,
               0.0, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        // First candidate pair with a MATURE arc wins; remember the best near-miss so
        // the skip is attributed to a CAUSE (never formed / young / unreconstructable)
        // rather than lumped into one uninformative tally.
        MwSlipDetector::MwArcZdMean zd;                 // the winner
        std::string sig1, sig2;
        int col1 = -1, col2 = -1, sidx1 = -1, sidx2 = -1;
        std::size_t n_cand = 0, best_n = 0;
        bool any_exists = false, any_recon_fail = false;
        std::string near_s1, near_s2;
        // Increment 2b: the published integer, if any, for this (station, satellite).
        // Looked up but NOT allowed to influence the channel search below.  An earlier
        // version tried the decision's pair FIRST, on the reasoning that the row must
        // fold on the pair the integer was decided on — true, but it made the integer
        // path able to change which pair the FLOAT path elects, and therefore to change
        // the solve even when no integer is ultimately folded.  Measured: the `inert`
        // A/B arm (fix armed, FFR gate set unreachable) differed from the float arm on 17
        // artifacts.  The dependency runs the other way now — the row elects its pair
        // exactly as increment 1 does, and the fix is used only if it was decided on THAT
        // pair (else `fix_pair_mismatch`, measured 0, because 2a searches the same
        // candidates in the same ascending order).
        const SdEwlFix* my_fix = nullptr;
        if (fix_on) {
            const auto git = fix->find({ station_index, cons });
            if (git != fix->end()) {
                const auto sit = git->second.find(o.satellite);
                if (sit != git->second.end() && sit->second.usable)
                    my_fix = &sit->second;
            }
        }
        for (const auto& [s1, k1, i1] : c1it->second) {
            for (const auto& [s2, k2, i2] : c2it->second) {
                ++n_cand;
                if (!sig1.empty()) continue;            // already decided; just count
                const auto q = st.mw_slip->arcZdMean(o.satellite, s1, s2);
                if (!q.exists) continue;                // this pair was never formed
                any_exists = true;
                // arcZdMean reports !valid for TWO different reasons — under two
                // lagged samples, and an SD arc whose group MW pivot arc is missing.
                // Only the latter is a frame-reconstruction failure; conflating them
                // reports a warm-up epoch as a datum defect.
                if (!q.valid && q.count >= 2) any_recon_fail = true;
                if (q.count > best_n) { best_n = q.count; near_s1 = s1; near_s2 = s2; }
                if (q.valid && q.count >= static_cast<std::size_t>(cfg.min_arc_epochs)) {
                    zd = q; sig1 = s1; sig2 = s2;
                    col1 = k1; col2 = k2; sidx1 = i1; sidx2 = i2;
                }
            }
        }
        if (sig1.empty()) {
            // Ordered most- to least-specific: a pair that exists but cannot be framed
            // is a DIFFERENT defect from one that is merely young, and both are
            // different from a pair the detector never built at all.
            const char* why = !any_exists      ? "NO_ARC"
                            : any_recon_fail   ? "FRAME_RECON"
                                               : "IMMATURE";
            if (!any_exists)          ++ctr.skip_no_arc;
            else if (any_recon_fail)  ++ctr.skip_frame_recon;
            else                      ++ctr.skip_immature;
            tr(why, o.satellite, {}, near_s1, near_s2, {}, {}, n_cand, best_n,
               false, 0.0, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        const double f1 = fb1, f2 = fb2;
        // λ cross-check: the detector's own λ_WL comes from the ACTUAL signal
        // frequencies, so a disagreement with the tier's nominal c/(f1−f2) means the
        // elected pair is not the tier this row's coefficients assume (a mis-tiered
        // row would fold a wrong-wavelength constraint silently).
        const double lam_nom = Constants::SPEED_OF_LIGHT / (f1 - f2);
        if (!(f1 > 0.0) || !(f2 > 0.0) || std::abs(f1 - f2) < 1.0
            || !(zd.lambda_wl > 0.0) || !std::isfinite(zd.lambda_wl)
            || std::abs(zd.lambda_wl - lam_nom) > 1e-6 * std::abs(lam_nom)) {
            ++ctr.skip_bad_lambda;
            tr("BAD_LAMBDA", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
               zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        // The satellite CODE-BIAS columns must be the ones the MW observable actually
        // carries.  The arc serves its own code channels (matchChannel's same-attribute
        // pick, with an in-band fallback), so no guessing: without them the OSB
        // attribution would be an assumption, and a wrong OSB column is undetectable
        // downstream.
        if (zd.code1.empty() || zd.code2.empty()) {
            ++ctr.skip_missing_code;
            tr("NO_CODE", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
               zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        Arc a;
        a.sat = o.satellite;
        a.mean_m = zd.mean_m;
        // Increment 2b: attach the published integer ONLY if the arc the search settled
        // on is the pair the integer was decided on.  A mismatch is not an error — the
        // decision runs before assembly and does not know which pairs will have local
        // columns this epoch — but folding across it would silently substitute one bias
        // frame's integer into another's row, so it is counted and the arc goes float.
        if (my_fix) {
            if (sig1 == my_fix->sig1 && sig2 == my_fix->sig2) {
                a.has_fix = true;
                a.n_fix_cyc = my_fix->n_cyc;
                a.fix_measured = my_fix->measured;
            } else {
                ++ctr.fix_pair_mismatch;
            }
        }
        // arcZdMean serves a VARIANCE OF THE MEAN (~1/n); floor it so a long quiet arc
        // cannot dominate the shared border with an unphysically tight row.  A FIXED row
        // takes the per-SAMPLE floor instead (amendment A1): the accumulator is
        // arc-cumulative, so consecutive-epoch rows share n−1 of n samples and refolding
        // a variance-of-mean as white every epoch over-claims information as sqrt(k) —
        // and the over-claim grows exactly as the fixed-row residuals fall to code-noise
        // scale.  Folding the current mean at per-sample weight accretes ~1 sample of
        // information per epoch instead, which is asymptotically honest.
        a.var_m2 = a.has_fix
                       ? std::max(zd.var_of_mean_m2,
                                  cfg.fix_sigma_floor_m * cfg.fix_sigma_floor_m)
                       : std::max(zd.var_of_mean_m2,
                                  cfg.sigma_floor_m * cfg.sigma_floor_m);
        a.count = zd.count;
        a.bia = zd.bia_corrected;
        a.lam_ewl = zd.lambda_wl;
        a.lam_b1 = Constants::SPEED_OF_LIGHT / f1;
        a.lam_b2 = Constants::SPEED_OF_LIGHT / f2;
        a.f1 = f1;
        a.f2 = f2;
        a.amb_col_b1 = col1;
        a.amb_col_b2 = col2;
        // NON-THROWING shared lookups: phaseBiasColumn / codeBiasColumn are `index_.at()`
        // and THROW when the key is unregistered.  Bare calls here would escape into the
        // parallel produce loop's catch and silently drop the WHOLE station's block
        // (losing its good phase rows) for a merely absent bias column.
        const auto pbCol = [&est, &o](const std::string& sig) -> Eigen::Index {
            try { return est.phaseBiasColumn(o.satellite, sig); }
            catch (...) { return -1; }
        };
        const auto cbCol = [&est, &o](const std::string& code) -> Eigen::Index {
            try { return est.codeBiasColumn(o.satellite, code); }
            catch (...) { return -1; }
        };
        a.pb_b1 = pbCol(sig1);
        a.pb_b2 = pbCol(sig2);
        a.cb_b1 = cbCol(zd.code1);
        a.cb_b2 = cbCol(zd.code2);
        if (a.pb_b1 < 0 || a.pb_b2 < 0 || a.cb_b1 < 0 || a.cb_b2 < 0) {
            ++ctr.skip_missing_shared_col;
            tr("NO_SHARED_COL", o.satellite, {}, sig1, sig2, zd.code1, zd.code2,
               n_cand, zd.count, a.bia, a.lam_ewl, a.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        // This arc's half of H·x_lin, with EXACTLY the coefficients the row folds below
        // (kept in one place so design and frame cannot drift apart).  Split into its
        // ambiguity and bias halves so the FIXED row, whose design drops the ambiguity
        // columns, can drop the matching linearization terms in the same breath.
        a.pred_amb_m = (a.lam_ewl / a.lam_b1) * xAt(sidx1)
                     - (a.lam_ewl / a.lam_b2) * xAt(sidx2);
        a.pred_bias_m = (f1 / (f1 - f2)) * xAt(stateOf(pb_state, o.satellite, sig1))
                      - (f2 / (f1 - f2)) * xAt(stateOf(pb_state, o.satellite, sig2))
                      - (f1 / (f1 + f2)) * xAt(stateOf(cb_state, o.satellite, zd.code1))
                      - (f2 / (f1 + f2)) * xAt(stateOf(cb_state, o.satellite, zd.code2));
        a.pred_m = a.pred_amb_m + a.pred_bias_m;
        a.sig1 = sig1;
        a.sig2 = sig2;
        a.code1 = zd.code1;
        a.code2 = zd.code2;
        a.n_cand = n_cand;
        by_const[cons].push_back(std::move(a));
    }

    // --- Build the rows. --------------------------------------------------------------
    // Increment 1: ONE whitened group per constellation.  Increment 2b: TWO — the arcs
    // whose integer is FIXED and the rest — because a group cannot be mixed.  R is
    // whitened by a dense L^-1, which would re-populate a fixed row's deliberately
    // zeroed local ambiguity columns out of its float siblings; and the pivot arc's
    // ambiguity columns appear in EVERY row of a group, so one mixed group would
    // simultaneously REMOVE and ESTIMATE the pivot ambiguity.  Splitting keeps both
    // halves exact and makes "a fixed row carries no local coefficients" true of the
    // whitened block, not just of H.  With the integer path off there is exactly one
    // group holding every arc, so the float behaviour is unchanged.
    const auto emitGroup = [&](const std::vector<const Arc*>& g, bool fixed_group) {
        if (g.size() < 2) {
            ++ctr.skip_no_pivot;
            for (const Arc* a : g)
                tr2("NO_PIVOT", a->sat, {}, a->sig1, a->sig2, a->code1, a->code2,
                    a->n_cand, a->count, a->bia, a->lam_ewl, a->mean_m, a->pred_m,
                    0.0, 0.0, a->n_fix_cyc, false, fixed_group);
            return;
        }
        const Arc& p = *g.front();
        tr2("PIVOT", p.sat, p.sat, p.sig1, p.sig2, p.code1, p.code2, p.n_cand, p.count,
            p.bia, p.lam_ewl, p.mean_m, p.pred_m, 0.0, std::sqrt(p.var_m2),
            p.n_fix_cyc, false, fixed_group);

        struct Row { const Arc* s; double z; double var; };
        std::vector<Row> rows;
        for (std::size_t k = 1; k < g.size(); ++k) {
            const Arc& s = *g[k];
            // Satellite-bias cancellation needs ONE frame across the difference — the
            // same rule NetworkMwWlLayer applies per cycle (skip_mixed_frame).
            if (s.bia != p.bia) {
                ++ctr.skip_mixed_frame;
                tr2("MIXED_FRAME", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2,
                    s.n_cand, s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, 0.0, 0.0,
                    s.n_fix_cyc, false, fixed_group);
                continue;
            }
            // CORRECTION FRAME: y = z_SD − H·x_lin, with H·x_lin = pred(s) − pred(p)
            // (the row's H is exactly the s-terms minus the p-terms).  The raw SD
            // difference is hundreds of metres; this residual is the only thing the
            // station's block can legally be handed.
            //
            // FIXED row (increment 2b): the integer leaves the RHS and the ambiguity
            // columns leave H, so H·x_lin must lose exactly its ambiguity half — a row
            // whose design changed and whose linearization point did not is folded in a
            // MIXED frame.  Both edits are here, in one expression, for that reason.
            const double z =
                fixed_group
                    ? (s.mean_m - p.mean_m)
                          - p.lam_ewl * static_cast<double>(s.n_fix_cyc - p.n_fix_cyc)
                          - (s.pred_bias_m - p.pred_bias_m)
                    : (s.mean_m - p.mean_m) - (s.pred_m - p.pred_m);
            // The limit gates the CORRECTION-frame residual, which is the quantity the
            // cold-clock x_lin pathology inflates (a 256 km x_lin makes pred absurd
            // while the raw statistic looks ordinary).
            if (cfg.max_abs_z_m > 0.0 && std::abs(z) > cfg.max_abs_z_m) {
                ++ctr.skip_big_z;
                tr2("BIG_Z", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                    s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, z, 0.0,
                    s.n_fix_cyc, false, fixed_group);
                continue;
            }
            if (!std::isfinite(z)) {   // never poison u_r with a non-finite RHS
                ++ctr.skip_bad_lambda;
                tr2("BAD_LAMBDA", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2,
                    s.n_cand, s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, z, 0.0,
                    s.n_fix_cyc, false, fixed_group);
                continue;
            }
            rows.push_back(Row{ &s, z, s.var_m2 });
        }
        if (rows.empty()) return;
        if (cfg.max_rows_per_station > 0
            && rows.size() > static_cast<std::size_t>(cfg.max_rows_per_station)) {
            std::stable_sort(rows.begin(), rows.end(),
                             [](const Row& a, const Row& b) { return a.var < b.var; });
            const std::size_t keep = static_cast<std::size_t>(cfg.max_rows_per_station);
            ctr.rows_capped += static_cast<long long>(rows.size() - keep);
            for (std::size_t k = keep; k < rows.size(); ++k) {
                const Arc& s = *rows[k].s;
                tr2("CAPPED", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                    s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, rows[k].z,
                    std::sqrt(rows[k].var), s.n_fix_cyc, false, fixed_group);
            }
            rows.resize(keep);
        }

        const int mc = static_cast<int>(rows.size());
        const Eigen::Index m0 = A_s.rows();
        const Eigen::Index ncol_s = A_s.cols();
        const Eigen::Index ncol_r = A_r.cols();
        A_s.conservativeResize(m0 + mc, Eigen::NoChange);
        A_s.bottomRows(mc).setZero();
        A_r.conservativeResize(m0 + mc, Eigen::NoChange);
        A_r.bottomRows(mc).setZero();
        w.conservativeResize(m0 + mc);
        y.conservativeResize(m0 + mc);

        // Every row shares the pivot arc's noise => R = diag(var_s) + var_p * 11^T,
        // structurally the GIM common-mode R.
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, p.var_m2);
        Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(mc, ncol_r);
        Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, ncol_s);
        Eigen::VectorXd zc(mc);
        for (int k = 0; k < mc; ++k) {
            const Arc& s = *rows[static_cast<std::size_t>(k)].s;
            R(k, k) += rows[static_cast<std::size_t>(k)].var;
            zc(k) = rows[static_cast<std::size_t>(k)].z;
            ctr.sum_var_m2 += R(k, k);
            // Local: lambda_EWL * (amb_b1/lambda_b1 - amb_b2/lambda_b2), s minus p.
            // A FIXED row has NONE of these: the local degree of freedom is REMOVED
            // (rank reduction), not constrained with a tight weight — a tight prior on
            // a gauge-adjacent local column is a stiff prior, and the whole point of
            // increment 2 is to stop the local nuisance absorbing the row's information
            // so the shared satellite bias receives it.
            if (!fixed_group) {
                Hr(k, s.amb_col_b1) += s.lam_ewl / s.lam_b1;
                Hr(k, s.amb_col_b2) -= s.lam_ewl / s.lam_b2;
                Hr(k, p.amb_col_b1) -= p.lam_ewl / p.lam_b1;
                Hr(k, p.amb_col_b2) += p.lam_ewl / p.lam_b2;
            }
            // Shared: MW = phi_WL - P_NL, and the satellite code bias enters the
            // pseudorange with +, hence the negative CB coefficients.
            const double wl_s1 = s.f1 / (s.f1 - s.f2), wl_s2 = -s.f2 / (s.f1 - s.f2);
            const double nl_s1 = -s.f1 / (s.f1 + s.f2), nl_s2 = -s.f2 / (s.f1 + s.f2);
            const double wl_p1 = p.f1 / (p.f1 - p.f2), wl_p2 = -p.f2 / (p.f1 - p.f2);
            const double nl_p1 = -p.f1 / (p.f1 + p.f2), nl_p2 = -p.f2 / (p.f1 + p.f2);
            Hs(k, s.pb_b1) += wl_s1;  Hs(k, s.pb_b2) += wl_s2;
            Hs(k, s.cb_b1) += nl_s1;  Hs(k, s.cb_b2) += nl_s2;
            Hs(k, p.pb_b1) -= wl_p1;  Hs(k, p.pb_b2) -= wl_p2;
            Hs(k, p.cb_b1) -= nl_p1;  Hs(k, p.cb_b2) -= nl_p2;
        }
        // Whiten by L^-1 (LL^T = R) into unit-information rows, so the block contributes
        // exactly H^T R^-1 H / H^T R^-1 z and stays in the diagonal-weight path.
        const Eigen::LLT<Eigen::MatrixXd> llt(R);
        if (llt.info() != Eigen::Success) {
            // UNWIND the append.  conservativeResize leaves the new w/y entries
            // UNINITIALIZED; leaving them behind — even against all-zero design rows —
            // risks 0·NaN = NaN poisoning N_rr/u_r, so the rows must go, not just be
            // zeroed.  (Cannot happen for a positive-variance R; belt-and-suspenders.)
            A_s.conservativeResize(m0, Eigen::NoChange);
            A_r.conservativeResize(m0, Eigen::NoChange);
            w.conservativeResize(m0);
            y.conservativeResize(m0);
            ctr.skip_bad_lambda += mc;
            return;
        }
        A_r.middleRows(m0, mc) = llt.matrixL().solve(Hr);
        A_s.middleRows(m0, mc) = llt.matrixL().solve(Hs);
        y.segment(m0, mc) = llt.matrixL().solve(zc);
        w.segment(m0, mc).setOnes();
        ctr.rows_built += mc;
        if (fixed_group) { ctr.rows_fixed += mc; ++ctr.groups_fixed; }
        else             { ctr.rows_float += mc; }
        for (int k = 0; k < mc; ++k) {
            const Arc& s = *rows[static_cast<std::size_t>(k)].s;
            tr2("BUILT", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m,
                rows[static_cast<std::size_t>(k)].z, std::sqrt(R(k, k)),
                s.n_fix_cyc - p.n_fix_cyc, fixed_group, fixed_group);
        }
    };

    for (auto& [cons, arcs] : by_const) {
        // Pivot election: most mature arc, tie-break ASCENDING satellite name.  Must be
        // deterministic or the row set (and hence byte-identity) drifts run to run.
        // Sorted ONCE over the whole constellation, so each partition inherits the same
        // ordering and elects its own most-mature member as its pivot.
        std::sort(arcs.begin(), arcs.end(), [](const Arc& a, const Arc& b) {
            if (a.count != b.count) return a.count > b.count;
            return a.sat < b.sat;
        });
        if (!fix_on) {
            std::vector<const Arc*> all;
            all.reserve(arcs.size());
            for (const Arc& a : arcs) all.push_back(&a);
            emitGroup(all, false);
            continue;
        }
        std::vector<const Arc*> fixed_g, float_g;
        for (const Arc& a : arcs) (a.has_fix ? fixed_g : float_g).push_back(&a);
        // A fixed group needs at least one MEASURED integer.  Datum-valued integers (the
        // component reference station's own satellites, each station's link satellite, a
        // satellite framed on first sighting) carry no rounding decision, so no gate can
        // reject them — a group built only from those would assert a hard constraint with
        // ZERO integer information while still removing a real local degree of freedom,
        // and it would make the whole feature impossible to disarm through its own gates.
        // Measured: without this test the S2 `inert` arm (fix armed, FFR gate set
        // unreachable) still differed from the float arm on 17 artifacts.  A MIXED group
        // is correct and wanted — the datum value is the reference the measurement is
        // relative to.
        if (!fixed_g.empty()
            && std::none_of(fixed_g.begin(), fixed_g.end(),
                            [](const Arc* a) { return a->fix_measured; })) {
            ctr.fix_datum_only += static_cast<long long>(fixed_g.size());
            float_g.insert(float_g.end(), fixed_g.begin(), fixed_g.end());
            std::stable_sort(float_g.begin(), float_g.end(),
                             [](const Arc* a, const Arc* b) {
                                 if (a->count != b->count) return a->count > b->count;
                                 return a->sat < b->sat;
                             });
            fixed_g.clear();
        }
        // A single fixed arc cannot form a group of its own (there is nothing to
        // difference against), and demoting it to the float group would be honest but
        // would silently change which arc is the float pivot between the ON and OFF legs
        // of an A/B.  Counting it as an unusable fix keeps the partition a pure function
        // of the published integers.
        if (fixed_g.size() == 1) {
            ++ctr.fix_lone_arc;
            float_g.insert(float_g.begin(), fixed_g.front());
            std::sort(float_g.begin(), float_g.end(),
                      [](const Arc* a, const Arc* b) {
                          if (a->count != b->count) return a->count > b->count;
                          return a->sat < b->sat;
                      });
            fixed_g.clear();
        }
        emitGroup(fixed_g, true);
        emitGroup(float_g, false);
    }
}

void Application::networkAppendGimIonoRows_(NetStation& st,
                                            const std::vector<PppObservation>& sta_obs,
                                            const std::map<int, int>& h2local,
                                            const GnssTime& t,
                                            Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                            Eigen::VectorXd& w, Eigen::VectorXd& y) const {
    if (!gim_ || !gim_->isLoaded()) return;
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled) return;
    const Eigen::VectorXd x = st.sm->getStateVector();

    struct GRow { int col; double innov; double var; };
    std::map<char, std::vector<GRow>> by_const;   // constellation char -> rows
    for (const auto& o : sta_obs) {
        if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // GPS/Galileo only
        if (o.elevation < iw.elevation_cutoff_deg) continue;
        const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
        if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
        const auto hit = h2local.find(iono_idx);
        if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
        const GimQuery q =
            gim_->slantIonoDelay(t, st.ecef, o.sat_pos, Constants::GPS_L1_FREQ);
        if (!q.valid) continue;
        const double sigma = std::max(iw.floor_m, iw.std_scale * q.std_m);
        by_const[o.satellite.front()].push_back(
            GRow{hit->second, q.delay_m - x(iono_idx), sigma * sigma});
    }

    int total = 0;
    for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
    if (total == 0) return;

    const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
    const Eigen::Index m0     = A_s.rows();
    const Eigen::Index ncol_r = A_r.cols();
    A_s.conservativeResize(m0 + total, Eigen::NoChange);
    A_s.bottomRows(total).setZero();
    A_r.conservativeResize(m0 + total, Eigen::NoChange);
    A_r.bottomRows(total).setZero();
    w.conservativeResize(m0 + total);
    y.conservativeResize(m0 + total);

    Eigen::Index out = m0;
    for (auto& kv : by_const) {
        const std::vector<GRow>& rws = kv.second;
        const int mc = static_cast<int>(rws.size());
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
        for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
        Eigen::MatrixXd Hc = Eigen::MatrixXd::Zero(mc, ncol_r);
        Eigen::VectorXd zc(mc);
        for (int k = 0; k < mc; ++k) { Hc(k, rws[k].col) = 1.0; zc(k) = rws[k].innov; }
        // Whiten by L⁻¹ (LLᵀ = R) → unit-information rows that contribute exactly
        // Hᵀ R⁻¹ H / Hᵀ R⁻¹ z to the station normals.
        const Eigen::LLT<Eigen::MatrixXd> llt(R);
        A_r.middleRows(out, mc) = llt.matrixL().solve(Hc);
        y.segment(out, mc)      = llt.matrixL().solve(zc);
        w.segment(out, mc).setOnes();
        out += mc;
    }
}

// Card LV-1 (docs/SPEC_level_information_budget.md): sample the SH-VTEC field at
// one line of sight's pierce point(s) and return the SLANT value in metres of L1
// delay — the same quantity a per-LOS iono state holds, so it can stand in for the
// station-common content of that state's birth seed.
//
//     field_slant = Σ_l  M_l(el) · tecu_to_L1m · Σ_k Ȳ_k(IPP_l) · c_{l,k}
//
// Two sources for c, in order:
//   1. @p coeff_lag1 — the previous epoch's SOLVED coefficients (the serial
//      post-solve snapshot in ctx.carry.sh_field_lag1).  The field is quasi-static
//      and sun-fixed, so one epoch of lag costs nothing; reading THIS epoch's field
//      is impossible by construction (the seed is needed to build the rows the
//      solve consumes).
//   2. Before the first solve, the a₀₀ CLIMATOLOGY PRIOR MEAN of each shell — a
//      uniform field, since the basis is orthonormal under (1/4π)∫dΩ so Ȳ₀₀ ≡ 1.
//      That is precisely the designed level authority (the EXT-CB ladder's GIM
//      anchor, or the self-contained posture's own climatology) doing its job at
//      cold start, rather than the receiver's hardware bias doing it.
//
// Returns false on a degenerate LOS (below the horizon on the dominant shell), and
// the caller then leaves that satellite out of the frame map — its seed stays raw
// and is counted.  Pure read of const inputs: safe in the parallel produce phase.
static bool networkShSlantAtLos_(const Eigen::Vector3d& rx_ecef,
                                 const Eigen::Vector3d& sat_pos,
                                 const std::vector<ShLayer>& layers,
                                 const std::vector<std::vector<double>>& coeff_lag1,
                                 double Re, double lon_sun, double tecu_to_L1m,
                                 std::vector<double>& bvals, double& slant_m) {
    if (layers.empty()) return false;
    const IppGeometry g0 = computeIppGeometry(rx_ecef, sat_pos, Re,
                                              layers[0].shell_height_m);
    if (!g0.valid) return false;
    const bool have_field = (coeff_lag1.size() == layers.size());
    double acc = 0.0;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const IppGeometry gl = (l == 0)
            ? g0
            : computeIppGeometry(rx_ecef, sat_pos, Re, layers[l].shell_height_m);
        if (!gl.valid) continue;             // this shell contributes nothing here
        const double m_tecu = gl.mapping * tecu_to_L1m;
        if (!have_field) {                   // cold start: uniform a₀₀ climatology
            acc += m_tecu * layers[l].mean_prior_tecu;
            continue;
        }
        const auto& c = coeff_lag1[l];
        if (static_cast<int>(c.size()) != layers[l].model->numCoeffs()) return false;
        layers[l].model->basis(gl.ipp_lat_rad, gl.ipp_lon_rad - lon_sun, bvals);
        if (bvals.size() != c.size()) return false;
        double vtec = 0.0;
        for (std::size_t k = 0; k < c.size(); ++k) vtec += bvals[k] * c[k];
        acc += m_tecu * vtec;
    }
    if (!std::isfinite(acc)) return false;
    slant_m = acc;
    return true;
}

// Co-estimated SH-VTEC ionosphere pseudo-observation (the design's hinge —
// replaces the external-GIM target of appendGimIonoRows with the network's OWN
// spherical-harmonic field).  Per GPS/Galileo satellite with a usable slant-iono
// column it folds one row asserting  I_local(rx,sat) = M(el)·VTEC_SH(IPP) :
//   H:  +1 on the local slant-iono column (A_r, exactly as the GIM row), and
//      −M(el)·tecu_to_L1m·Ȳ_nm(IPP) on each SHARED SH coefficient column (A_s) —
//      the ONE new write that turns the prior into a joint estimate.
// The innovation is −x(iono_idx) (the field at coeff 0 is 0; absolute solve).
// The common-mode-free per-constellation R = diag(var)+σ_c²·11ᵀ is REUSED
// unchanged: its all-ones block leaves the per-(rx,const) iono datum level FREE
// (the receiver DCB rides in it), so the field constrains only the between-
// satellite slant-iono STRUCTURE — a₀₀ (global mean) is observable only weakly,
// through the mapping-function spread, and is otherwise held by its loose Kaula
// prior (data-determined mean, not clamped to an external value).  GLONASS is
// excluded from the field (FDMA per-slot IFB would leak into the global field;
// see design §8).  Built into the station's own block + the shared border; the
// SH column lookups are const, safe in the parallel produce phase.
void Application::networkAppendShIonoRows_(NetStation& st, const NetworkEstimator& est,
                                           const std::vector<PppObservation>& sta_obs,
                                           const std::map<int, int>& h2local,
                                           const GnssTime& t,
                                           const std::unique_ptr<SphericalHarmonicIonosphere>& sh_model,
                                           const std::unique_ptr<SphericalHarmonicIonosphere>& sh_model2,
                                           const NetworkIonoDeweightGate& iono_deweight,
                                           const GlobalVtecShConfig& shc, bool sh_dev_on,
                                           const std::map<std::string, ShDevCarry>& sh_dev_carry,
                                           const std::set<std::string>& dw_row_held,
                                           const std::set<std::string>* dw_tie_admitted,
                                           Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                           Eigen::VectorXd& w, Eigen::VectorXd& y) const {
    if (!sh_model) return;
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled) return;
    // Iono-constraint deweight factor for THIS station (READ-only const lookup;
    // race-free per the threading contract above).  w_s==1 ⇒ untouched (byte-identical
    // OFF); w_s<=0 ⇒ append NO SH iono rows (the per-LOS iono floats freely on its own
    // GF code/phase — full decouple).
    const double w_s = iono_deweight.weight(st.name);
    if (w_s <= 0.0) return;
    const Eigen::VectorXd x = st.sm->getStateVector();
    const double Re = shc.earth_radius_km * 1000.0;
    const double tecu_to_L1m =
        40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
    const double lon_sun = shc.sun_fixed ? meanSunLongitude(t) : 0.0;
    // Iono-2 dual-layer: one row still asserts ONE slant constraint, but the
    // field side sums the shells — ι = Σ_l M_l(el)·tecu·SH_l(IPP_l)·c_l (+ d).
    // Each shell gets its OWN pierce geometry (different H ⇒ different mapping
    // AND different IPP lat/lon); the single-layer path is the one-entry list
    // (byte-identical).  The representativeness σ and the deviation's vertical↔
    // slant conversion use the LAYER-0 mapping (the dominant shell).
    const std::vector<ShLayer> layers = shLayers(shc, sh_model.get(),
                                                 sh_model2 ? sh_model2.get() : nullptr);
    const std::size_t n_lay = layers.size();

    struct SRow {
        int    local_col;        ///< A_r column of this satellite's slant-iono state.
        double innov;            ///< −x(iono_idx) (+ predicted deviation under T5v2).
        double var;              ///< per-LOS representativeness variance (m²).
        std::vector<double> map_tecu;             ///< Per layer: M_l(el)·tecu_to_L1m.
        std::vector<std::vector<double>> basis;   ///< Per layer: Ȳ_nm(IPP_l) in coeff order.
    };
    std::map<char, std::vector<SRow>> by_const;
    std::vector<double> bvals;
    for (const auto& o : sta_obs) {
        if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // GPS/Galileo only
        if (o.elevation < iw.elevation_cutoff_deg) continue;
        const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
        if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
        const auto hit = h2local.find(iono_idx);
        if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
        // Iono-deweight release hygiene (docs/SPEC_deweight_release_hygiene.md).  Two
        // dispositions keep a tie row OUT of this station's stack:
        //   INC-3  — the LOS failed the release screen: its solved deviation is still
        //            too far from the field for the row to be folding information
        //            rather than garbage, so it stays decoupled until the serial seam
        //            re-tests it next epoch.
        //   INC-3b — the LOS has not been ADMITTED yet.  This is a POSITIVE list, not a
        //            hold list, and it has to be: at a LOS's first sighting the seam has
        //            never run for that key, so there is nothing that could have put it
        //            in a hold list — and its first tie row is precisely the coupling
        //            event this card exists to screen (the measured DOY-129 valve).
        // Both are EMPTY / null unless armed, so the OFF path costs one empty() test and
        // one null test and never looks a key up (byte-identical).
        if (!dw_row_held.empty() || dw_tie_admitted) {
            const std::string dw_key = st.name + "|" + o.satellite;
            if (!dw_row_held.empty() && dw_row_held.count(dw_key)) continue;
            if (dw_tie_admitted && !dw_tie_admitted->count(dw_key)) continue;
        }
        const IppGeometry g0 =
            computeIppGeometry(st.ecef, o.sat_pos, Re, layers[0].shell_height_m);
        if (!g0.valid) continue;
        const double m0_tecu = g0.mapping * tecu_to_L1m;   // layer-0 STEC scale
        // Literal legacy product chain (rep·map)·tecu — NOT rep·m0_tecu: FP
        // multiplication is non-associative and the OFF-path byte-identity gate
        // caught the regrouped form drifting the solve checksum at 1e-14.
        const double sigma = std::max(
            iw.floor_m, shc.representation_sigma_tecu * g0.mapping * tecu_to_L1m);
        SRow r;
        r.local_col = hit->second;
        r.innov     = -x(iono_idx);
        r.var       = sigma * sigma;
        r.map_tecu.resize(n_lay, 0.0);
        r.basis.resize(n_lay);
        for (std::size_t l = 0; l < n_lay; ++l) {
            const IppGeometry gl = (l == 0)
                ? g0
                : computeIppGeometry(st.ecef, o.sat_pos, Re, layers[l].shell_height_m);
            if (!gl.valid) continue;   // shell contributes nothing on a degenerate LOS
            layers[l].model->basis(gl.ipp_lat_rad, gl.ipp_lon_rad - lon_sun, bvals);
            r.map_tecu[l] = gl.mapping * tecu_to_L1m;
            r.basis[l]    = bvals;
        }
        // T5v2: the pin constrains (ι − Σ_l SH_l·c_l) to the PREDICTED deviation
        // at the prediction's variance + the floor, instead of to 0 at the full
        // representativeness σ.  The carry is in VERTICAL TECU (elevation-
        // stationary); the epoch's layer-0 mapping converts to this row's slant
        // frame.  READ-ONLY on the carry (serial pre-pass ages + slip-erases it;
        // a missing key means cold seed: dev 0, P = steady-state σ² — identical
        // information to the untreated row, so cold LOS behave as before).
        if (sh_dev_on) {
            const double sig_ss_slant =
                shc.representation_sigma_tecu * g0.mapping * tecu_to_L1m;
            double dev_pred_slant = 0.0;
            double var_pred_slant = sig_ss_slant * sig_ss_slant;
            const auto dit = sh_dev_carry.find(st.name + "|" + o.satellite);
            if (dit != sh_dev_carry.end() && dit->second.t_s > 0.0) {
                dev_pred_slant = dit->second.dev * m0_tecu;
                var_pred_slant = dit->second.var * m0_tecu * m0_tecu;
            }
            r.innov = -x(iono_idx) + dev_pred_slant;
            r.var   = iw.floor_m * iw.floor_m + var_pred_slant;
        }
        by_const[o.satellite.front()].push_back(std::move(r));
    }

    int total = 0;
    for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
    if (total == 0) return;

    // Per-layer SH column indices (shared border), looked up once per call.
    std::vector<std::vector<Eigen::Index>> sh_cols(n_lay);
    for (std::size_t l = 0; l < n_lay; ++l) {
        const int Kl = layers[l].model->numCoeffs();
        sh_cols[l].resize(static_cast<std::size_t>(Kl));
        for (int k = 0; k < Kl; ++k) {
            const auto& c = layers[l].model->coeffs()[static_cast<std::size_t>(k)];
            sh_cols[l][static_cast<std::size_t>(k)] =
                est.vtecCoeffColumn(c.n, c.m, c.trig, layers[l].layer);
        }
    }

    const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
    const Eigen::Index m0      = A_s.rows();
    const Eigen::Index ncol_r  = A_r.cols();
    const Eigen::Index nshared = A_s.cols();
    A_s.conservativeResize(m0 + total, Eigen::NoChange);
    A_s.bottomRows(total).setZero();
    A_r.conservativeResize(m0 + total, Eigen::NoChange);
    A_r.bottomRows(total).setZero();
    w.conservativeResize(m0 + total);
    y.conservativeResize(m0 + total);

    Eigen::Index out = m0;
    for (auto& kv : by_const) {
        const std::vector<SRow>& rws = kv.second;
        const int mc = static_cast<int>(rws.size());
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
        for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
        // T1b: temporal treatment of the quasi-static SH constraint.  Re-applying the
        // (sun-fixed, quasi-static) SH↔slant pseudo-obs as an INDEPENDENT white row
        // every epoch is a σ-incest channel — the formal σ collapses without new
        // information.  Inflate the row noise by N (σ ×√N) so each epoch's white row
        // carries only 1/N of the information: this matches the information rate of
        // "one full look per N epochs" but WITHOUT withholding.  (Pure row-skip drops
        // the satellite-code-bias observability the SH rows provide → the observability
        // gate withholds every skipped epoch; the empirical A/B confirmed 6/10 epochs
        // withheld at N=3, so σ-inflation is the observability-preserving mechanism.)
        // dec ≤ 1 ⇒ no scaling ⇒ byte-identical.
        if (shc.row_decimation_epochs >= 2)
            R *= static_cast<double>(shc.row_decimation_epochs);
        // Station-adaptive iono-constraint deweight: scale this station's whole SH-iono
        // block information (σ² ×= 1/w_s ⇒ info ×= w_s), uniformly on the per-LOS
        // diagonal var AND the common-mode off-diagonal — preserving the receiver-DCB-
        // free structure.  w_s==1 ⇒ no scaling ⇒ byte-identical (mirrors the
        // row_decimation σ-inflation precedent above).  w_s<=0 already returned early.
        if (w_s < 1.0)
            R *= 1.0 / w_s;
        Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(mc, ncol_r);    // local +1 (slant iono)
        Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, nshared);   // −M_l·tecu·basis_l (SH cols, all shells)
        Eigen::VectorXd zc(mc);
        for (int k = 0; k < mc; ++k) {
            Hr(k, rws[k].local_col) = 1.0;
            for (std::size_t l = 0; l < n_lay; ++l) {
                const auto& bl = rws[k].basis[l];
                for (std::size_t j = 0; j < bl.size(); ++j)
                    Hs(k, sh_cols[l][j]) = -rws[k].map_tecu[l] * bl[j];
            }
            zc(k) = rws[k].innov;
        }
        // Whiten by L⁻¹ (LLᵀ = R) → unit-information rows; whiten the local +1,
        // the SH partials, AND the innovation by the SAME factor so the common-
        // mode-free structure carries through to both blocks identically.
        const Eigen::LLT<Eigen::MatrixXd> llt(R);
        A_r.middleRows(out, mc) = llt.matrixL().solve(Hr);
        A_s.middleRows(out, mc) = llt.matrixL().solve(Hs);
        y.segment(out, mc)      = llt.matrixL().solve(zc);
        w.segment(out, mc).setOnes();
        out += mc;
    }
}

EpochAction Application::networkPassAHead_(const SyncedEpoch& se,
                                           NetworkRunContext& ctx,
                                           NetworkSetupResult& setup,
                                           NetworkRunTraces& traces,
                                           std::vector<std::vector<PppObservation>>& modeled,
                                           std::vector<bool>& present,
                                           std::set<std::string>& shared_phase_keys) {
    // Alias block (INC-7 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names; the former
    // epoch-loop `continue`s are the EpochAction::SkipEpoch returns (their
    // site-specific bookkeeping is verbatim).
    auto& stations = setup.stations;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };
    // CB-ADM phase co-gate feed: the run-latched admitted code-key set (null
    // unless the flag is armed AND the census ran).  Passed into every
    // ensureNetworkSharedStates call so a signal whose code was quarantined also
    // allocates NO phase-bias state ⇒ shared_phase_keys (built from the states)
    // excludes it consistently.  Null ⇒ byte-identical.
    const std::set<std::string>* cbadm_admit =
        (config_.estimateNetworkSatCodeBias() && ctx.admission.active)
            ? &ctx.admission.admitted : nullptr;

    // Legacy single-pivot datum: model the pivot FIRST so its usable phase
    // signals restrict the shared phase-bias columns to ones with a pivot-
    // receiver ambiguity pin, and an absent pivot sinks the epoch (no datum).
    // Prioritized mode treats the pivot as an ordinary station — modeled in the
    // tail loop from i_lo=0, no special skip — and elects the per-(sat,signal)
    // anchors AFTER the cross-station prune, so this block is skipped and the
    // pivot blinking out no longer costs the whole epoch (the SPOF cure).
    if (!phase_datum_prioritized) {
        NetStation& piv = stations.front();
        auto it = se.by_station.find(piv.name);
        if (it == se.by_station.end() || it->second.empty()) {
            // No datum station this epoch: no clean phase-bias gauge — skip.
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ",0,0,0,0,0,0,0,0,0\n";
            emitEpochStatus("sync_skip", {});   // only week/tow available
            return EpochAction::SkipEpoch;
        }
        piv.pstate.epoch_time = se.time;
        if (!piv.model->computeModels(it->second, piv.pstate, modeled.front())
            || modeled.front().empty()) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ",0,0,0,0,0,0,0,0,0\n";
            emitEpochStatus("sync_skip", {});   // only week/tow available
            return EpochAction::SkipEpoch;
        }
        piv.mm->ensureNetworkSharedStates(modeled.front(), {}, cbadm_admit);  // allow all pivot phase signals (minus CB-ADM quarantined)
        present.front() = true;
    }

    if (!phase_datum_prioritized)
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);
    return EpochAction::Proceed;
}

void Application::networkPassASlipScan_(const SyncedEpoch& se,
                                        NetworkRunContext& ctx,
                                        NetworkSetupResult& setup,
                                        NetworkRunTraces& traces,
                                        double dt, double pl_now_s, bool carry_warm,
                                        std::vector<std::vector<PppObservation>>& modeled,
                                        std::vector<bool>& present,
                                        const std::set<std::string>& shared_phase_keys,
                                        std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                        SlipQcChannel& qc) {
    // Alias block (INC-7 migration mechanic).
    auto log = log_manager_.getLogger("main_log");
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    auto& local_carry = ctx.carry.local_carry;
    auto& fr_if_slip = ctx.counters.fr_if_slip;
    // CB-ADM phase co-gate feed (see networkPassAHead_): null unless flag armed
    // AND census ran.  Withholds a quarantined signal's phase-bias state.
    const std::set<std::string>* cbadm_admit =
        (config_.estimateNetworkSatCodeBias() && ctx.admission.active)
            ? &ctx.admission.admitted : nullptr;

    const int i_lo = phase_datum_prioritized ? 0 : 1;

    // PASS A tail (stations[1..]) — PARALLEL over stations when enabled.
    // computeModels + ensureNetworkSharedStates touch ONLY station i's own
    // sm/model/mm/pstate; shared_phase_keys is read-only here and modeled[i]
    // is a distinct slot per i — no shared write.  Each iteration catches its
    // own exceptions: an exception must never escape an OpenMP structured
    // block (UB).  present[] is std::vector<bool> (packed bits → concurrent
    // writes to adjacent indices race), so threads record into a per-index
    // char buffer that is folded into present[] serially afterwards.
    // PASS-A slip-scan seam (INC-6): the per-station buffers the parallel
    // loop below fills and the serial slip-QC drains (slip/NetworkSlipQc.cpp)
    // consume.
    PassABuffers pa(stations.size());
    auto& a_ok           = pa.a_ok;
    auto& a_err          = pa.a_err;
    auto& sta_repairs    = pa.sta_repairs;
    auto& sta_if_resets  = pa.sta_if_resets;
    auto& sta_mw_refloat = pa.sta_mw_refloat;

    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
    for (int i = i_lo; i < n_sta_total; ++i) {
        try {
            NetStation& st = stations[i];
            auto it = se.by_station.find(st.name);
            if (it == se.by_station.end() || it->second.empty()) continue;
            st.pstate.epoch_time = se.time;
            if (!st.model->computeModels(it->second, st.pstate, modeled[i])
                || modeled[i].empty())
                continue;
            // Prioritized: every station (incl. the former pivot, i=0)
            // allocates ALL its own trackable phase signals — the anchored
            // shared set is the post-election union, not a pivot restriction.
            if (phase_datum_prioritized)
                st.mm->ensureNetworkSharedStates(modeled[i], {}, cbadm_admit);
            else
                st.mm->ensureNetworkSharedStates(modeled[i], shared_phase_keys, cbadm_admit);
            a_ok[i] = 1;

            // Adaptive iono q (network.adaptive_iono_q): roll this station's GF-TD2
            // variability monitor forward.  Station-local state, no shared write
            // (same parallel contract as the T0 slip scan below); the per-satellite
            // multipliers are consumed SERIALLY by the T5v2 GM deviation aging later
            // this epoch.  Best-effort like the slip scan: a monitor failure costs
            // this station's boosts this epoch, never the station.
            if (st.iono_var) {
                try {
                    st.iono_var->update(modeled[i]);
                    ++st.iono_var_epochs_fed;
                } catch (...) { /* best-effort: no boosts from this station this epoch */ }
            }

            // T0 slip detection: roll this station's MW + GF arcs forward and
            // collect per-(sat,signal) integer repairs.  Station-local detector
            // state — no shared write, safe in the parallel region.  Run EVERY
            // epoch (the resolver MUST advance its GF arcs even with no slips)
            // and regardless of the carry warmup, so the adaptive noise floors
            // are warm when the carry activates.  A detection failure costs only
            // this station's repairs this epoch, never the station itself.
            if (st.mw_slip) {
                try {
                    const auto mwr = st.mw_slip->detectLargeMwResets(modeled[i]);
                    if (st.turbo_edit) {
                        // turbo_edit (1 s data): MW+GF → integer REPAIR of the carry.
                        auto er = st.turbo_edit->resolve(
                            mwr, st.mw_slip->getEpochCleanPairs(), modeled[i], dt,
                            st.mw_slip->getAcrossGapWlEvidence(pl_now_s));
                        sta_repairs[i] = std::move(er.repairs);
                    } else if (config_.networkCycleSlipMwDetect()) {
                        // mw_detect (any rate): detect-only → REFLOAT confident
                        // wide-lane integer jumps (no GF, no integer applied).  frac
                        // gates rounding cleanliness — a BIA-less float re-init rounds
                        // to a large spurious integer but with a large frac (LEFT); a
                        // KNOWN non-zero integer with small frac is a real WL slip.
                        // LLI loss-of-lock is handled directly in the serial drain.
                        // (Explicitly mw_detect-gated: under mw_wl-only the detector is
                        // built for its Welford means but must NOT refloat the carry —
                        // the WL layer is observation-side only.  Byte-identical for
                        // existing configs: the detector was previously built only when
                        // turbo_edit OR mw_detect, so this branch already implied mw_detect.)
                        //
                        // MW EVIDENCE REFLOAT (network.cycle_slip.mw_evidence_refloat):
                        // the frac gate above is the T0 REPAIR gate, and this path never
                        // applies an integer — so a strong, mature, bias-free-lane flag is
                        // ALSO admissible on the strength of the detection.  The census
                        // below runs knob-independently; with the knob off the rule is
                        // evaluated and counted but never admits (byte-identical).
                        const double frac_max = config_.networkCycleSlipTurboEditFracMax();
                        const bool   ev_on    = config_.networkCycleSlipMwEvidenceRefloat();
                        const double ev_ratio = config_.networkCycleSlipMwEvidenceRatioMin();
                        const int    ev_narc  = config_.networkCycleSlipMwEvidenceMinArcEpochs();
                        // Card L5P: the PAIR SCOPE.  Under 'primary' the network
                        // forms only the pair whose two signals both sit on the
                        // constellation's ionosphere-free pin bands — on a lineage
                        // that latches one signal per band, the station's latched
                        // band-1 / band-2 pair.  A pair carrying any other band is
                        // not formed at all: no candidate, no refloat, and no entry
                        // in the common-band scope's evidence below, so the
                        // detector's view of the epoch is the two-band one.  The
                        // band-5 PHASE keeps LLI, gap re-entry, IF-TD and the band
                        // scope, which is what gate G2 grades (invariant #1).
                        // 'all' (the default) evaluates the lambda to true for
                        // every pair, so an unarmed run is byte-identical.
                        const bool mw_pairs_primary =
                            config_.networkCycleSlipMwDetectPairs()
                                == NetworkMwDetectPairs::Primary;
                        std::map<char, NetworkPinBands> mw_pin_by_sys;
                        const auto mw_pair_ok =
                            [&](const std::string& sat, const std::string& s1,
                                const std::string& s2) {
                                if (!mw_pairs_primary) return true;
                                if (sat.empty()) return true;
                                auto pit = mw_pin_by_sys.find(sat[0]);
                                if (pit == mw_pin_by_sys.end())
                                    pit = mw_pin_by_sys.emplace(
                                              sat[0], networkPinBandsFor(sat[0])).first;
                                return networkMwPairInPrimaryScope(s1, s2, pit->second);
                            };
                        for (const auto& r : mwr) {
                            if (r.sig1.empty() || r.sig2.empty()) continue;
                            if (!mw_pair_ok(r.satellite, r.sig1, r.sig2)) {
                                ++pa.sta_mw_admit[i].scope_skipped;
                                continue;
                            }
                            const auto d = networkMwAdmitCandidate(
                                r.slip_wl_known, r.slip_wl_cycles, r.slip_frac, frac_max,
                                r.evidence_ratio, r.arc_epochs, r.bia_corrected,
                                ev_on, ev_ratio, ev_narc);
                            auto& ac = pa.sta_mw_admit[i];
                            if (d.admit_legacy)    ++ac.legacy_admitted;
                            if (d.via_evidence)    ++ac.evidence_admitted;
                            if (d.would_admit)     ++ac.evidence_would_admit;
                            if (d.drop_loose)      ++ac.drop_loose;
                            if (d.drop_unknown)    ++ac.drop_unknown;
                            if (d.admitted)
                                sta_mw_refloat[i].push_back(PassABuffers::MwRefloatCand{
                                    r.satellite, r.sig1, r.sig2,
                                    r.slip_wl_cycles, r.slip_frac, r.lambda_wl,
                                    r.slip_wl_known, r.evidence_ratio, r.arc_epochs,
                                    d.via_evidence});
                        }

                        // COMMON-BAND SCOPE evidence (SPEC_event_harness §14).
                        // Collected KNOB-INDEPENDENTLY (invariant #4) from what
                        // this station's detector already produced: every arc it
                        // PROCESSED a sample for with that sample's maturity and
                        // threshold, every flag (ADMITTED OR DROPPED — a dropped
                        // flag is still not silence), and the receiver's own LLI
                        // bits.  The processed map, NOT getEpochCleanPairs: that
                        // set answers TurboEdit's cleanliness question, whose
                        // σ-awareness bar a narrow lane on a noisy bed never
                        // meets (see MwSlipDetector::getEpochPairStatus).
                        // Per-station slot, no shared write.  The IF-TD half is
                        // a network-POOLED decision and is previewed serially in
                        // the verdict seam.
                        // Card L5P: the pair scope applies HERE TOO.  A pair the
                        // network did not form must not testify about which bands
                        // an epoch implicated — leaving it in the evidence would
                        // mean the detector still "formed" it, just without a
                        // candidate, which is not what the knob claims.  Under
                        // 'all' the predicate is true for every pair, so an
                        // unarmed run's evidence is byte-identical.
                        auto& cbev = pa.sta_cb_ev[i];
                        for (const auto& [pkey, pst] : st.mw_slip->getEpochPairStatus()) {
                            std::string csat, cs1, cs2;
                            if (!parseIfPairResetKey(pkey, csat, cs1, cs2)) continue;
                            if (!mw_pair_ok(csat, cs1, cs2)) continue;
                            CommonBandProcessed cp;
                            cp.arc_n     = pst.arc_n_at_sample;
                            cp.flagged   = pst.flagged;
                            cp.thresh_wl = (pst.lambda_wl > 0.0)
                                               ? pst.thresh_m / pst.lambda_wl
                                               : std::numeric_limits<double>::quiet_NaN();
                            cp.lli_shadow = pst.lli_shadow;   // §14.11 rule F2
                            cbev[csat].processed[{cs1, cs2}] = cp;
                        }
                        for (const auto& r : mwr) {
                            if (r.sig1.empty() || r.sig2.empty()) continue;
                            if (!mw_pair_ok(r.satellite, r.sig1, r.sig2)) continue;
                            cbev[r.satellite].flagged_pairs.insert({r.sig1, r.sig2});
                        }
                        for (const auto& obs : modeled[i])
                            for (const auto& [code, lli] : obs.lli_flags)
                                if (!code.empty() && code[0] == 'L' && (lli & 1))
                                    cbev[obs.satellite].lli_signals.insert(code);
                    }
                } catch (...) { /* best-effort: no MW action this epoch */ }
            }

            // IF-phase triple-difference slip detection: between-satellite,
            // between-epoch IF-phase OMC.  Station positions are fixed at
            // near-truth, so position_sigma_3d = 0 disables the pre-convergence
            // gate (the sole reason this detector was staged) and st.ecef makes
            // the inter-epoch geometry correction a no-op (Δpos ≡ 0).  Detector
            // state is station-local — no shared write, safe here.  Best-effort:
            // a failure costs only this station's flags this epoch.
            if (st.if_slip) {
                try {
                    sta_if_resets[i] = st.if_slip->detect(modeled[i], 0.0, st.ecef);
                } catch (...) { /* best-effort: no IF-phase flags this epoch */ }
            }
        } catch (const std::exception& ex) {
            a_err[i] = ex.what();
        } catch (...) {
            a_err[i] = "unknown exception";
        }
    }

    // Serial drain: commit present[] + log diagnostics in station order.
    // i_lo folds in station 0 too under the prioritized datum (it is an
    // ordinary station there; on the legacy path i_lo==1 and station 0's
    // present was already set by the pivot-first block above).
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (a_ok[i]) present[i] = true;
        else if (!a_err[i].empty())
            log->warn("Network: station '{}' PASS A geometry threw ({}); "
                      "skipping it this epoch", stations[i].name, a_err[i]);
    }

    // --- card SCC: the second code channel's per-epoch bookkeeping (R7) --------
    // (docs/refound/SCC_second_code_channel_2026-09-21.md.)  SERIAL, in station
    // order, so the counters and the event rows are deterministic and the edges are
    // diffed against a set only this thread touches.  Placed after present[] is
    // final — an absent station's cells CLOSE, which is the decoupling edge — and
    // before anything reads `modeled` again, which it only reads.
    //
    // Two jobs.  (1) Republish each station's COMPANION cell set for PASS B: the
    // row-weight seam has the signal and the satellite of every row but no way to
    // tell a companion from the FALLBACK case (a band-1 code that arrived without
    // its canonical beside it), and both route through the same receiver state.
    // (2) Count the cells, both cell edges, the receiver inter-signal states alive
    // and their RESEEDS, and the (satellite, signal) columns that reached the
    // two-station registration bar ONLY through companions — the card's thesis as a
    // number.
    //
    // OFF ⇒ the whole block is skipped, every set stays empty and nothing is
    // emitted, so the epoch is byte-identical.
    if (config_.networkSecondCodeChannel()) {
        auto& C = ctx.counters.second_code;
        // Per-(satellite, signal): how many present stations support the column at
        // all, and how many of those support it ONLY through a companion row.  The
        // registration bar is two stations (see the code-bias column registration
        // below), so a column with >= 2 supporters of which < 2 are canonical is one
        // this card put on the wire.
        std::map<std::string, std::pair<int, int>> col_support;   // "sat|signal" -> (total, canonical)
        long long isc_alive = 0;
        for (int i = 0; i < n_sta_total; ++i) {
            NetStation& st = stations[i];
            std::set<std::string> cells;
            if (present[i] && i < static_cast<int>(modeled.size())) {
                for (const auto& ob : modeled[static_cast<std::size_t>(i)]) {
                    for (const auto& [sig, val] : ob.observed) {
                        (void)val;
                        if (sig.empty() || sig[0] != 'C') continue;
                        const std::string key = StateKeys::makeSatBiasKey(ob.satellite, sig);
                        auto& sup = col_support[key];
                        ++sup.first;
                        if (ob.companion_codes.count(sig)) cells.insert(key);
                        else                               ++sup.second;
                    }
                }
            }
            // The two edges (standing invariant #7).  A cell OPENS when a station
            // starts contributing a companion row for a satellite and signal, and
            // CLOSES when it stops — including when the whole station drops out,
            // which is exactly the "a station with zero rows dropped out of the map
            // and its return read as a first sighting" failure the invariant names.
            std::vector<std::string> opened, closed;
            secondCodeCellEdges(st.scc_cells_prev, cells, &opened, &closed);
            for (const auto& cell : opened) {
                const auto parts = StateKeys::parseSatBiasKey(cell);
                traces.emitEvent(se.time, "SCC", "companion_open",
                                 parts ? parts->satellite : std::string{}, st.name,
                                 std::nullopt, std::nullopt, "open",
                                 parts ? parts->signal : cell);
            }
            for (const auto& cell : closed) {
                const auto parts = StateKeys::parseSatBiasKey(cell);
                traces.emitEvent(se.time, "SCC", "companion_close",
                                 parts ? parts->satellite : std::string{}, st.name,
                                 std::nullopt, std::nullopt, "close",
                                 parts ? parts->signal : cell);
            }
            C.cell_open  += static_cast<long long>(opened.size());
            C.cell_close += static_cast<long long>(closed.size());
            C.cells      += static_cast<long long>(cells.size());
            st.scc_cells_prev = cells;
            st.scc_cells      = std::move(cells);
            // The receiver inter-signal state census, and its RESEED test.  A state
            // this station has held before, did not hold last epoch and holds now,
            // cold-started instead of being restored by the receiver code-bias carry
            // — the one way this card can determine a receiver bias from a single
            // epoch of code (standing invariant #1's re-coupling edge).
            std::set<std::string> isc_now;
            if (st.sm) {
                for (const auto& [key, idx] : st.sm->getCodeBiasStates()) {
                    (void)idx;
                    if (!StateKeys::isReceiverIscKey(key)) continue;
                    isc_now.insert(key);
                    if (!st.scc_isc_prev.count(key) && st.scc_isc_seen.count(key))
                        ++C.isc_reseeds;
                }
            }
            isc_alive += static_cast<long long>(isc_now.size());
            st.scc_isc_seen.insert(isc_now.begin(), isc_now.end());
            st.scc_isc_prev = std::move(isc_now);
        }
        C.isc_states_max = std::max(C.isc_states_max, isc_alive);
        for (const auto& [key, sup] : col_support) {
            (void)key;
            if (sup.first >= 2 && sup.second < 2) ++C.cols_via_companion;
        }
    }

    // Shared RAW slip observables (docs/SPEC_slip_qc_hardening.md §1): computed
    // ONCE per epoch, here — after present[] is final and BEFORE both the drains
    // (Card L needs the evidence) and the observability pre-filter (a satellite
    // about to be pruned must still be measured, the contract the trace writer
    // has always kept).  Nothing between this point and the trace writer mutates
    // `modeled`, so the relocated rows are byte-identical.  Skipped entirely
    // when no consumer asked for them (qc.utils null ⇒ every card inert).
    qc.epoch.reset(stations.size());
    if (qc.armed()) {
        // Card GRK: the per-station canonical PHASE latch, bound over the station
        // vector rather than copied into a table, because the latch is locked for
        // the run and lives on each station's own PppModel.  A station whose model
        // is absent returns empty, which the tracker reads as "change nothing".
        const TrackerLatchLookup latch =
            [&stations](int sta, const std::string& system, char band) -> std::string {
                if (sta < 0 || sta >= static_cast<int>(stations.size())) return {};
                const auto& m = stations[static_cast<std::size_t>(sta)].model;
                if (!m) return {};
                return m->latchedCanonicalPhaseOnBandDigit(system, band);
            };
        // Card SID phase 2 (network.roster_tracker): the tracker's FIRST READ of the
        // phase-1 roster.  The same lookup shape, bound to the roster's modelled_phase
        // instead of to the station's latch, so the selection rule below is untouched
        // and only the SOURCE of the answer moves.  Armed, this lookup is bound and
        // the latch lookup above is not.
        //
        // ⚠ THE ONE FALL-BACK, and why it is not a bypass.  The roster is built at the
        // top of the epoch body, UPSTREAM of computeModels, and the canonical latch is
        // decided INSIDE computeModels — so on the FIRST epoch of a constellation the
        // roster's cells carry no modelled role at all (card SID section 6 states this
        // as a property of the build site).  Answering "nothing" there would put that
        // epoch's tracker back on the lexical rule while card GRK's lookup names the
        // latch, and the two rows would then differ at epoch 0 at every station whose
        // priority latch is not the lexically lowest channel.  That single epoch is not
        // free: it seeds `RawSlipHistory::prev`, so epoch 1's MW/GF step would be
        // formed across a pair change and could fire a detection that is purely an
        // artefact of the reader.  The lookup therefore falls back to that station's
        // latch exactly where the roster has no answer, which is the un-decided first
        // epoch and nothing else, and BOTH outcomes are counted so a run states how
        // much of the work the roster actually did (standing invariant 4).
        const bool roster_tracker = config_.networkRosterTracker() && ctx.roster.armed;
        const TrackerLatchLookup roster_lu =
            [&ctx, &stations](int sta, const std::string& system, char band) -> std::string {
                auto& c = ctx.counters.sid;
                const std::string m = signalRosterModelledPhase(ctx.roster, sta, system, band);
                if (!m.empty()) { ++c.trk_roster_hits; return m; }
                if (sta < 0 || sta >= static_cast<int>(stations.size())) {
                    ++c.trk_roster_none;
                    return {};
                }
                const auto& mdl = stations[static_cast<std::size_t>(sta)].model;
                if (!mdl) { ++c.trk_roster_none; return {}; }
                const std::string l = mdl->latchedCanonicalPhaseOnBandDigit(system, band);
                if (l.empty()) ++c.trk_roster_none;
                else           ++c.trk_roster_fallback;
                return l;
            };
        networkComputeRawObservables(modeled, present, n_sta_total, *qc.utils, qc.epoch,
                                     qc.params.gap_reentry_per_signal,
                                     // Card GRK: the tracker's band signal from the
                                     // latch, plus the mismatch cell counted in BOTH
                                     // knob states (the counters sink is passed
                                     // unconditionally for exactly that reason).
                                     // Card SID phase 2: the roster read arms the same
                                     // selection, from the roster's own answer.
                                     qc.params.tracker_modeled_first || roster_tracker,
                                     &qc.counters,
                                     roster_tracker ? &roster_lu : &latch);
        // The qualification pass (empirical step gates, LLI credibility, arc
        // resumptions) runs only for a DECISION card — with just the trace armed
        // the observables are pure output and nothing is adjudicated.
        if (qc.cardsArmed())
            networkQualifyRawObservables(modeled, present, n_sta_total, qc.epoch_idx,
                                         qc.params, qc.hist, qc.epoch, qc.counters);
        // CS-GF authority advance (QUEUE 8b successor, iono_carry_source =
        // smoothed_gf): one recursion step per dual-band LOS from THIS epoch's
        // qualified verdicts.  Serial, station-ordered → deterministic.  The
        // qualify pass's empirical gates (mw_step/gf_step) are the slip reset;
        // lost adjacency (gap / first sighting / pair change through the gate)
        // resets too.  Runs BEFORE the drains — the authority is detector-
        // independent evidence, and the harvest reads it post-solve.
        if (qc.params.iono_cs) {
            for (int i = 0; i < n_sta_total; ++i) {
                if (!present[i]
                    || i >= static_cast<int>(qc.epoch.rows.size())) continue;
                const auto& vmap = qc.epoch.verdict[static_cast<std::size_t>(i)];
                for (const auto& row : qc.epoch.rows[static_cast<std::size_t>(i)]) {
                    if (!row.dual || !(row.gf_denom > 0.0)) continue;
                    const auto vit = vmap.find(row.satellite);
                    const bool adjacent =
                        vit != vmap.end() && vit->second.measured;
                    const bool stepped =
                        adjacent && (vit->second.mw_step || vit->second.gf_step);
                    // I = gf_code/(1−γ) = −gf_code/(γ−1); ΔI = d_gf/(γ−1).
                    const double i_code = -row.gf_code_m / row.gf_denom;
                    const double di =
                        adjacent ? vit->second.d_gf / row.gf_denom : 0.0;
                    const double srad = std::sin(row.elev_deg * 0.017453292519943295);
                    const double sigma =
                        kIonoCsCodeSigmaZenith / std::max(srad, 0.15);
                    auto& cs = ctx.carry.iono_cs[{i, row.satellite}];
                    if (networkIonoCsUpdate(cs, i_code, di, adjacent, stepped, sigma))
                        ++ctx.counters.n_iono_cs_resets;
                    else
                        ++ctx.counters.n_iono_cs_blends;
                    cs.lam1_m = row.lam1_m;   // LV-6: exact per-pair TECU conversion
                }
            }
        }
    }

    // §9.7.10 datum-station slip DETECTION (network.cycle_slip.datum_station_detect):
    // under the legacy pivot datum the parallel scan above starts at i_lo == 1, so
    // the datum station's MW detector — built by setup under the same toggle — is
    // rolled HERE, serially (one station; determinism trivially preserved).  Only
    // detector-mode candidates are collected (never T0 repairs: the pivot's
    // ambiguities are gauge-pinned, so an integer repair of them is meaningless);
    // the MW drain below routes station-0 candidates, and station-0 LLI
    // assertions, to the network re-reference path instead of a local refloat.
    const bool datum_detect_on = !phase_datum_prioritized
                                 && config_.networkCycleSlipDatumStationDetect()
                                 && config_.networkCycleSlipMwDetect();
    if (datum_detect_on && present.front() && stations.front().mw_slip) {
        try {
            const auto mwr =
                stations.front().mw_slip->detectLargeMwResets(modeled.front());
            // Same admission rule as the parallel scan above (one helper, so the
            // datum station can never drift to a different gate).
            const double frac_max = config_.networkCycleSlipTurboEditFracMax();
            const bool   ev_on    = config_.networkCycleSlipMwEvidenceRefloat();
            const double ev_ratio = config_.networkCycleSlipMwEvidenceRatioMin();
            const int    ev_narc  = config_.networkCycleSlipMwEvidenceMinArcEpochs();
            // Card L5P: the same pair scope as the parallel scan above — the datum
            // station must never drift to a different pair set any more than to a
            // different admission gate.
            const bool dm_pairs_primary =
                config_.networkCycleSlipMwDetectPairs() == NetworkMwDetectPairs::Primary;
            std::map<char, NetworkPinBands> dm_pin_by_sys;
            for (const auto& r : mwr) {
                if (r.sig1.empty() || r.sig2.empty()) continue;
                if (dm_pairs_primary && !r.satellite.empty()) {
                    auto pit = dm_pin_by_sys.find(r.satellite[0]);
                    if (pit == dm_pin_by_sys.end())
                        pit = dm_pin_by_sys.emplace(
                                  r.satellite[0], networkPinBandsFor(r.satellite[0])).first;
                    if (!networkMwPairInPrimaryScope(r.sig1, r.sig2, pit->second)) {
                        ++pa.sta_mw_admit[0].scope_skipped;
                        continue;
                    }
                }
                const auto d = networkMwAdmitCandidate(
                    r.slip_wl_known, r.slip_wl_cycles, r.slip_frac, frac_max,
                    r.evidence_ratio, r.arc_epochs, r.bia_corrected,
                    ev_on, ev_ratio, ev_narc);
                auto& ac = pa.sta_mw_admit[0];
                if (d.admit_legacy) ++ac.legacy_admitted;
                if (d.via_evidence) ++ac.evidence_admitted;
                if (d.would_admit)  ++ac.evidence_would_admit;
                if (d.drop_loose)   ++ac.drop_loose;
                if (d.drop_unknown) ++ac.drop_unknown;
                if (d.admitted)
                    sta_mw_refloat[0].push_back(PassABuffers::MwRefloatCand{
                        r.satellite, r.sig1, r.sig2,
                        r.slip_wl_cycles, r.slip_frac, r.lambda_wl,
                        r.slip_wl_known, r.evidence_ratio, r.arc_epochs,
                        d.via_evidence});
            }
        } catch (...) { /* best-effort: no datum-station MW detection this epoch */ }
    }

    // MW admission census reduce: per-station slots were filled index-addressed
    // in the parallel region (and, for station 0, by the serial datum scan just
    // above), so the fold is serial and station-ordered — deterministic.  Run
    // unconditionally: these counters are the card's knob-independent telemetry.
    for (int i = 0; i < n_sta_total; ++i) {
        const auto& ac = pa.sta_mw_admit[static_cast<std::size_t>(i)];
        ctx.counters.n_mw_admit_legacy         += ac.legacy_admitted;
        ctx.counters.n_mw_evidence_admitted    += ac.evidence_admitted;
        ctx.counters.n_mw_evidence_would_admit += ac.evidence_would_admit;
        ctx.counters.n_mw_drop_loose           += ac.drop_loose;
        ctx.counters.n_mw_drop_unknown         += ac.drop_unknown;
        ctx.counters.n_mw_pair_scope_skipped   += ac.scope_skipped;   // card L5P
    }

    // Serial slip-QC drains (INC-6): bodies relocated VERBATIM to
    // slip/NetworkSlipQc.cpp — phase-leveling T0 repair drain, MW-detect /
    // LLI refloat, IF-phase TD driver, in the original order.
    networkSlipDrainT0Repairs(pa, config_, stations, present, i_lo, n_sta_total,
                              local_carry, se, log);

    // SPEC_slip_relevel_walk §6: the per-epoch re-level response state —
    // cleared HERE (the harvest and the PASS-B inject consumed last epoch's
    // sets after this point in the previous epoch), filled by the two drains
    // below, read by the inject site, the harvest guard, and the IF-TD bridge.
    // nullptr when the knob is off ⇒ the drains' inserts are never reached
    // (byte-identical — the refsat_mark_sats pattern).
    ctx.carry.relevel.clear();
    RelevelEpochState* relevel_ptr =
        config_.networkCycleSlipRelevelFreeEpoch() ? &ctx.carry.relevel : nullptr;

    // Resume slip warm-up (network.resume_slip_warmup_ep, SPEC_synthetic_minimum_
    // network §18.10): for the first K processed epochs after a warm resume the
    // MW/LLI (T0) drain runs DIAGNOSTIC-ONLY — its cold reference (two samples at
    // the second warm epoch, no Welford history) declared 146 false integer WL
    // slips on the §18.9 bed and re-levelled 5 % of the restored arcs.  The T1
    // IF-TD and wind-up drains below keep their live carry_warm, so a genuine
    // in-gap slip still has a response.  Decremented once per processed epoch
    // HERE (the single dispatch site); the retire is the DECOUPLING edge of the
    // warm-up window (invariant #7) and is logged.  0 on every continuous epoch
    // ⇒ mw_carry_warm == carry_warm ⇒ byte-identical.
    bool mw_carry_warm = carry_warm;
    if (ctx.carry.resume_slip_warmup_left > 0) {
        mw_carry_warm = false;
        ++ctx.counters.n_resume_slip_warmup_epochs;
        if (--ctx.carry.resume_slip_warmup_left == 0)
            log->info("Network (resume slip warm-up): RETIRED at TOW {:.1f} — the MW/LLI "
                      "drain acted diagnostic-only for {} post-resume epoch(s); it is live "
                      "from the next epoch.",
                      se.time.tow, ctx.counters.n_resume_slip_warmup_epochs);
    }
    // COMMON-BAND SCOPE (SPEC_event_harness §14): ONE verdict per (station,
    // satellite) for this epoch, formed HERE — before either drain — so the MW
    // refloat, the LLI free and the IF-TD pair refloat all obey the SAME
    // decision and their per-epoch union is exactly `implicated` (Study 3's
    // per-epoch invariant, §Z.2).  Runs armed or not: the census is what sizes
    // the card before arming it (invariant #4).  Only the ARMED run gets a
    // non-null pointer into the drains, so OFF is byte-identical.
    CommonBandEpoch common_band;
    networkCommonBandVerdictEpoch(pa, config_, stations, present, i_lo,
                                  n_sta_total, se, traces, log,
                                  &ctx.carry.anchor_incumbent, common_band);
    {
        auto& C = ctx.counters;
        C.n_cb_verdicts          += common_band.verdicts;
        C.n_cb_narrowed          += common_band.narrowed_n;
        C.n_cb_all_no_exoneration += common_band.all_no_exoneration;
        C.n_cb_all_inconsistent  += common_band.all_inconsistent;
        C.n_cb_bands_kept        += common_band.bands_kept;
        C.n_cb_iftd_veto         += common_band.iftd_veto;
        for (const auto& [pr, n] : common_band.iftd_veto_by_pair)   // §14.10
            C.n_cb_iftd_veto_by_pair[pr] += n;
        C.n_cb_iftd_ref_arc      += common_band.iftd_ref_arc;    // §22
        for (const auto& [pr, n] : common_band.iftd_ref_arc_by_pair)
            C.n_cb_iftd_ref_arc_by_pair[pr] += n;
        C.n_cb_immature_pairs    += common_band.immature_pairs;
        C.n_cb_lli_shadow        += common_band.lli_shadow;      // §14.11 F2
        C.n_cb_lli_verdicts      += common_band.lli_verdicts;    // §14.12 F3
        C.n_cb_lli_narrowed      += common_band.lli_narrowed;
        C.n_cb_lli_anchor_colocate += common_band.lli_anchor_colocate;   // §14.12.5
        C.n_cb_weak_silence      += common_band.weak_silence;
        C.n_cb_anchor_colocate   += common_band.anchor_colocate;
        C.n_cb_would_narrow      += common_band.would_narrow;
    }
    const CommonBandEpoch* common_band_ptr = common_band.armed ? &common_band : nullptr;

    networkSlipDrainMwRefloat(pa, config_, stations, present, modeled, i_lo,
                              n_sta_total, carry_invariant, mw_carry_warm,
                              carry_slipped_ambs, qc.epoch, qc.params, qc.hist,
                              qc.counters, se, traces, log,
                              datum_detect_on ? &ctx.carry.datum_reref_pb_sats : nullptr,
                              datum_detect_on ? &ctx.carry.datum_reref_amb_sats : nullptr,
                              datum_detect_on ? &ctx.carry.refsat_mark_sats : nullptr,
                              datum_detect_on ? &local_carry : nullptr,
                              relevel_ptr,
                              relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
                              relevel_ptr ? &local_carry : nullptr,
                              // SIM-ONLY ORACLE slip response: null unless
                              // network.cycle_slip.oracle_relevel armed the run,
                              // so the OFF path never reaches a hook call
                              // (structurally byte-identical, invariant #6).
                              ctx.carry.slip_oracle.get(),
                              common_band_ptr);

    // Edge-1 piece 2: pass the mark sink ONLY when armed, so the OFF path passes
    // nullptr and the driver's insert is not even reached (byte-identical).
    // §Z band-scoped detector refloat (docs/SPEC_event_harness.md): the two
    // whole-satellite drains below need the raw per-band evidence and a counter
    // sink they do not otherwise receive.  Off ⇒ both stay nullptr ⇒ byte-identical.
    const bool bscope_on = config_.getNetworkConfig().cycle_slip_refloat_band_scope
                               == NetworkRefloatBandScope::Implicated;
    networkSlipIfTdDriver(pa, config_, stations, present, i_lo, n_sta_total,
                          carry_invariant, carry_warm, carry_slipped_ambs,
                          trace_flag_rates, fr_if_slip, se, traces, log,
                          config_.networkMarkRefsatResetDisc()
                              ? &ctx.carry.refsat_mark_sats : nullptr,
                          relevel_ptr,
                          relevel_ptr ? &local_carry : nullptr,
                          (relevel_ptr || bscope_on) ? &qc.counters : nullptr,
                          relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
                          // Bridge v3 E4: the qualified raw verdicts (d_gf) +
                          // per-station empirical step scales, only when the
                          // GF row is armed (nullptr keeps the OFF path inert).
                          // §Z also needs the raw epoch (per-band MW/GF evidence)
                          // and a counter sink.  Widening these two conditions is
                          // inert with the scope off, and safe with it on: the
                          // bridge's own uses are all under `br_gf_on`, which still
                          // requires relevel_bridge_gf, and every bridge_cnt
                          // increment sits inside a `relevel` / `relevel_pend`
                          // guard that is null on that path.
                          (config_.networkCycleSlipRelevelBridgeGf() || bscope_on)
                              ? &qc.epoch : nullptr,
                          config_.networkCycleSlipRelevelBridgeGf() ? &qc.hist : nullptr,
                          ctx.carry.slip_oracle.get(),    // SIM-ONLY ORACLE
                          common_band_ptr,                // §14 COMMON-BAND SCOPE
                          // §14.10 PAIR WIDTH: ALWAYS passed — the per-pair-kind
                          // tally is knob-independent telemetry (invariant #4),
                          // so a `first` run's run-end clause already sizes what
                          // the one watched pair sees.  Counters only; the
                          // solve, the products and the event rows are untouched
                          // while the knob is `first`.
                          &ctx.counters.n_iftd_pair);

    // ---- §10.4: wind-up accumulator-restart response ------------------------
    // (SPEC_phase_windup_sign §10.4.)  Drains the per-station restart ledgers
    // the model evaluations above just filled and refloats the stepped arcs —
    // the wired invariant-#1 response that gates any accumulate_phase_windup
    // arm.  Sited BEFORE the lane observatory's PASS-A half below so a
    // restart-freed pair enters this epoch's freed set (and its lane response
    // is measured under cause=WU like every other).  No-op unless
    // network.cycle_slip.windup_restart_refloat (loader-disarmed without
    // accumulated mode).
    networkSlipDrainWindupRestart(config_, stations, present, i_lo, n_sta_total,
                                  carry_invariant, carry_warm,
                                  carry_slipped_ambs, qc.counters, se, traces,
                                  log, relevel_ptr, &ctx.carry.relevel_pend,
                                  ctx.carry.slip_oracle.get(),    // SIM-ONLY ORACLE
                                  bscope_on ? &qc.epoch : nullptr);  // §Z evidence

    // ---- Card G RESPONSE: the gap RE-ENTRY refloat --------------------------
    // (docs/SPEC_slip_qc_hardening.md §G3/W1.)  An arc that re-appears inside
    // network.phase_leveling_max_gap_s has NO detector covering its return epoch
    // — MW needs an adjacent predecessor and IF-TD three consecutive epochs — so
    // its pre-gap zero-q leveling level is re-injected across a break the raw
    // MW/GF evidence (or the receiver's own LLI) says it did not survive.  This
    // drain is the wired invariant-#1 response to that: it marks BOTH bands of a
    // SLIPPED re-entry exactly as a detected slip.
    //
    // Sited HERE for two reasons: it must run in the SAME epoch as the re-entry
    // (the PASS-B leveling inject a few hundred lines below is what would fold
    // the stale level, and it reads `relevel.free_ambs`), and it must precede
    // the lane observatory's PASS-A half so a gap-freed pair enters this epoch's
    // freed set and is measured under cause=GAP like every other response.
    //
    // ⚠ It is handed `local_carry` + (pl_now_s, pl_max_gap_s) and NOT just the
    // StateManager, because at THIS point in the epoch a re-entering arc has no
    // ambiguity column at all — PASS B's ensureRequiredStates re-creates it
    // below.  The carry entry is what survives the gap and what the inject would
    // bridge, so the carry is the drain's target and its oracle; the ambiguity
    // key is synthesised in the inject site's own format.  (The first cut walked
    // the live ambiguity map and was inert on every re-entry: 396 verdicts, 0
    // freed, byte-identical 12 h field A/B.)
    // No-op unless network.cycle_slip.gap_reentry_relevel.
    networkSlipDrainGapReentry(stations, present, i_lo, n_sta_total,
                               carry_invariant, carry_warm, carry_slipped_ambs,
                               qc.epoch, qc.params,
                               local_carry, pl_now_s, ctx.knobs.pl_max_gap_s,
                               qc.counters, se, traces, log,
                               relevel_ptr,
                               relevel_ptr ? &ctx.carry.relevel_pend : nullptr);

    // ---- Card GRS: the same response, keyed on the SIGNAL -------------------
    // (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md.)  The drain above
    // answers a raw ROW that went absent; a satellite that keeps one tracking
    // signal through a primary-pair loss of lock keeps its row, so that tier is
    // blind to it and the returning phases' carried ambiguities fold across the
    // break.  Sited immediately after it, for the same two reasons (same epoch
    // as the re-entry; before the lane observatory's PASS-A half), and handed
    // the identical arguments.
    // No-op unless network.cycle_slip.gap_reentry_per_signal (a rider of
    // gap_reentry_relevel).
    networkSlipDrainGapReentryPerSignal(stations, present, i_lo, n_sta_total,
                                        carry_invariant, carry_warm, carry_slipped_ambs,
                                        qc.epoch, qc.params,
                                        local_carry, pl_now_s, ctx.knobs.pl_max_gap_s,
                                        qc.counters, se, traces, log,
                                        relevel_ptr,
                                        relevel_ptr ? &ctx.carry.relevel_pend : nullptr);

    // ---- EVENT HARNESS Study 1: the injected free_epoch arc response -------
    // (docs/SPEC_event_harness.md §9.)  An `arc_erase` event carrying
    // "mode": "free_epoch" is answered HERE, as a DETECTOR refloat, instead of
    // by the T4 attribution block — the A/B that prices the erase path's four
    // extra actions.  Sited with the other PASS-A drains so the free-epoch latch
    // it fills is read by this epoch's PASS-B leveling inject (post-fit RAIM is
    // too late — relevel.clear() above would already have run next epoch).
    // Unreachable unless the injector staged a free_epoch directive.
    if (ctx.inject.armed && !ctx.inject.arc_erase_now.empty())
        networkSlipDrainInjectedFree(ctx.inject.arc_erase_now, stations, present,
                                     n_sta_total, carry_invariant, carry_warm,
                                     carry_slipped_ambs, ctx.inject, se, traces, log,
                                     relevel_ptr,
                                     relevel_ptr ? &ctx.carry.relevel_pend : nullptr);

    // ---- Rung 3 v3: the carry-domain lane observatory, PASS-A half ----------
    // (SPEC_lane_slip_response §7.3.)  Sited AFTER both slip drains so this
    // epoch's freed set is complete.  It carries ONLY the raw-MW observable
    // forward — no carry level is read here.  That is the design point: the raw
    // MW is untouched by the eq34 D4 / phase-datum S-transforms that shift
    // LocalCarry::value_abs between this seam and the harvest, so reading BOTH
    // lane levels inside the harvest keeps them in one gauge by construction
    // instead of interlocking around a frame step after the fact.
    //
    // Measure-only: nothing downstream mutates a value from this map, so an
    // armed run stays byte-identical on every solve artifact.
    if (relevel_ptr && config_.networkCycleSlipRelevelLaneSnap() && qc.epoch.computed) {
        for (std::size_t i = 0; i < stations.size() && i < qc.epoch.rows.size(); ++i) {
            if (i < present.size() && !present[i]) continue;
            // §7.6(a): per-satellite elevation for the per-event trace rows —
            // the raw pass carries none, but the modeled set (same station,
            // same epoch) does.  0 stays "unavailable" for a satellite the
            // model pass dropped.
            std::map<std::string, double> el_of;
            if (i < modeled.size())
                for (const auto& mo : modeled[i]) el_of[mo.satellite] = mo.elevation;
            for (const auto& row : qc.epoch.rows[i]) {
                // NON-dual rows are admitted too (sig2 empty).  Without them a
                // satellite the raw pass tracked single-band is indistinguishable
                // from one it never saw, and the "no MW pair" channel would be
                // dead by construction — an invariant-#4 shape in the very
                // instrument built to expose them.
                const auto vit = qc.epoch.verdict[i].find(row.satellite);
                RelevelEpochState::LaneMw m;
                m.sig1 = row.sig1;
                m.lam1_m = row.lam1_m;
                const auto eit = el_of.find(row.satellite);
                if (eit != el_of.end()) m.el_deg = eit->second;
                if (row.dual && row.lambda_wl_m > 0.0 && std::isfinite(row.lambda_wl_m)) {
                    m.sig2        = row.sig2;
                    m.lambda_wl_m = row.lambda_wl_m;
                    // lam2 from the WL definition 1/lam_wl = 1/lam1 - 1/lam2, so
                    // the pair's wavelengths stay mutually consistent with the MW
                    // being differenced against them.  ⚠ lambda_wl_m is stored as
                    // an ABSOLUTE value, so this inversion is only valid when the
                    // lower-band-digit signal has the SHORTER wavelength; the
                    // consumer rejects the pair when it does not (bad_lambda),
                    // because there the derived lam2 is wrong AND the MW's phase
                    // term is sign-flipped relative to the carry lane.
                    const double inv2 = (row.lam1_m > 0.0)
                                            ? (1.0 / row.lam1_m - 1.0 / row.lambda_wl_m)
                                            : 0.0;
                    m.lam2_m = (inv2 > 0.0) ? 1.0 / inv2 : 0.0;
                    if (vit != qc.epoch.verdict[i].end()) {
                        m.measured     = vit->second.measured;
                        m.pair_changed = vit->second.pair_changed;
                        m.d_mw_cyc     = m.measured ? vit->second.d_mw / row.lambda_wl_m : 0.0;
                    }
                }
                relevel_ptr->lane_mw[{static_cast<int>(i), row.satellite}] = m;
            }
        }
    }
}

void Application::networkAssemblyPrefilter_(const SyncedEpoch& se,
                                            NetworkSetupResult& setup,
                                            NetworkRunTraces& traces,
                                            const SlipQcChannel& qc,
                                            std::vector<std::vector<PppObservation>>& modeled,
                                            const std::vector<bool>& present,
                                            int& n_dropped_sats) {
    // Alias block (INC-7 migration mechanic).
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    auto& slip_raw_trace       = traces.slip_raw_trace;

    // RAW cycle-slip observables (network.trace_slip_raw) — INDEPENDENT of whether any
    // slip detector is armed.  Written BEFORE the observability pre-filter so a satellite
    // about to be dropped is still captured.  For each present station+satellite the
    // primary dual-frequency pair (two lowest DISTINCT bands whose phase AND matching code
    // "C"+sig[1:] are present) yields MW, GF (=L1−L2), first-signal phase-minus-code (all m)
    // plus the per-band RINEX LLI.  Absolute values — diff adjacent epochs offline for the
    // slip signature.
    //
    // The ARITHMETIC moved to slip/NetworkRawObservables.cpp at the 2026-07-21 slip-QC
    // hardening (spec §1): Cards L and G need the same numbers as evidence, and three
    // independent definitions of "the raw step" would have been a bug factory.  This site
    // is now a pure consumer walking qc.epoch.rows in the identical order, so its rows are
    // byte-unchanged when the trace is the only consumer.
    if (slip_raw_trace.is_open() && qc.epoch.computed) {
        for (int i = 0; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            for (const auto& r : qc.epoch.rows[static_cast<std::size_t>(i)]) {
                slip_raw_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << stations[i].name << ',' << r.satellite << ','
                    << r.sig1 << ',' << r.sig2 << ','
                    << fmt::format("{:.2f}", r.elev_deg) << ','
                    << (r.dual ? fmt::format("{:.4f}", r.mw_m) : std::string{}) << ','
                    << (r.dual ? fmt::format("{:.4f}", r.gf_m) : std::string{}) << ','
                    << fmt::format("{:.4f}", r.l1_c1_m) << ',' << r.lli1 << ',' << r.lli2 << '\n';
            }
        }
    }

    // --- Cross-station observability pre-filter -------------------------
    // A satellite clock is only well-determined for a network product when at
    // least kMinStationsPerSat stations observe it: a single-station satellite
    // clock is tied to just that one receiver clock and, at marginal geometry,
    // becomes the extra near-zero direction that trips the rank gate.  Drop
    // such satellites from the network ENTIRELY this epoch — both the shared
    // clock/phase-bias states AND the underlying observations.  Excluding the
    // observations is essential: a satellite kept in the design with NO shared
    // clock column would pin its station's receiver clock absolutely and so
    // erase the very clock gauge the datum is meant to pin (declared > observed
    // → the gate would then reject every epoch).  Dropping the whole satellite
    // lets one weak satellite no longer force the WHOLE epoch to be withheld;
    // the gate stays as the final safety net for a genuine residual deficiency.
    constexpr int kMinStationsPerSat = 2;
    n_dropped_sats = 0;
    {
        std::map<std::string, int> sat_station_count;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatClockStates())
                ++sat_station_count[StateKeys::satelliteFromSatClockKey(kv.first)];
        }
        std::set<std::string> dropped_sats;
        for (const auto& kv : sat_station_count)
            if (kv.second < kMinStationsPerSat) { dropped_sats.insert(kv.first); ++n_dropped_sats; }

        if (!dropped_sats.empty()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                // Remove the dropped satellites' observations so their rows are
                // not built (and so cannot pin the receiver clock).
                auto& obs = modeled[i];
                obs.erase(std::remove_if(obs.begin(), obs.end(),
                              [&](const PppObservation& o) { return dropped_sats.count(o.satellite) != 0; }),
                          obs.end());
                // Prune the now-orphaned shared states.
                StateManager& sm = *stations[i].sm;
                std::set<std::string> keep_clk, keep_pb;
                for (const auto& kv : sm.getSatClockStates())
                    if (!dropped_sats.count(StateKeys::satelliteFromSatClockKey(kv.first)))
                        keep_clk.insert(kv.first);
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (p && !dropped_sats.count(p->satellite)) keep_pb.insert(kv.first);
                }
                sm.pruneSatClockStatesExcept(keep_clk);
                sm.pruneSatPhaseBiasStatesExcept(keep_pb);
            }
        }
    }
}

EpochAction Application::networkAssemblyRegisterColumns_(const SyncedEpoch& se,
                                            NetworkRunContext& ctx,
                                            NetworkSetupResult& setup,
                                            NetworkRunTraces& traces,
                                            NetworkEstimator& est,
                                            const std::vector<bool>& present,
                                            const std::set<std::string>& shared_phase_keys,
                                            std::vector<std::string>& registered_clock_sats,
                                            std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                                            std::vector<std::string>& col_desc,
                                            int& n_clock, int& n_pb, int& n_cb,
                                            int& n_present) {
    // Alias block (INC-7 migration mechanic).
    const auto& nc = config_.getNetworkConfig();
    auto& stations = setup.stations;
    auto& sh_model = setup.sh_model;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& last_n_present = ctx.counters.last_n_present;
    auto& last_n_clock_sats = ctx.counters.last_n_clock_sats;
    auto& spectrum_diag_budget = ctx.counters.spectrum_diag_budget;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };

    // --- Register the shared column layout (reset the hoisted estimator) ---
    est.reset();
    std::set<std::string> clk_seen;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& kv : stations[i].sm->getSatClockStates()) {
            const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
            est.registerClock(sat);
            if (clk_seen.insert(sat).second) registered_clock_sats.push_back(sat);
        }
    }
    // CB-ADM per-epoch registration decision (ON only): an admitted code pair is
    // registered ONLY when >= 2 present stations hold its state THIS epoch (the
    // per-epoch guard).  A single-station code column is confounded with that
    // receiver's clock and goes near-null; window-latched admission alone
    // (register at >= 1 station) re-introduced exactly those columns and doubled
    // the storm (measured: 22 -> 43 near-null dirs on the 305 arm).  The anti-flap
    // win is preserved because the ADMITTED SET stays latched (census) — only
    // per-epoch REGISTRATION follows coverage, and the SM state is never pruned,
    // so a withheld column re-registers the moment >= 2 stations return.
    // cbadm_reg_code_keys = the (sat,signal) code columns registered this epoch.
    //
    // The phase co-gate is NOT applied here (Card H1 — this comment used to claim
    // it was "co-gated to them below", which is wrong and would send a reader
    // hunting for a gate that does not exist at this site).  It lives UPSTREAM, at
    // phase-bias STATE ALLOCATION in MeasurementModel::ensureNetworkSharedStates,
    // and it keys off the run-latched CENSUS-ADMITTED set — NOT off
    // cbadm_reg_code_keys, which is the narrower per-epoch REGISTERED subset.  Two
    // separate reasons, both load-bearing:
    //  - LEVEL: withholding a registerPhaseBias for a key still present in
    //    shared_phase_keys breaks the "every shared_phase_key is a registered
    //    column" invariant eq34 / carry / snapshot rely on (a registration-site
    //    co-gate was tried and CRASHED the ON path).  Allocating no phase-bias
    //    state at all keeps shared_phase_keys consistent everywhere by construction.
    //  - GRANULARITY (Card H2, ruled deliberately): the co-gate stays at CENSUS
    //    granularity.  Following per-epoch code coverage would churn phase-state
    //    allocation every epoch — the anti-flap design's whole point — and thrash
    //    eq34/carry/snapshot.  The accepted residue is a transient ORPHAN WINDOW,
    //    counted by ctx.counters.n_cbadm_phase_orphan_key_epochs below.
    //
    // Why a sparse signal must leave the shared system on BOTH bias families at
    // all: that phase column has no same-signal code co-anchor and, being
    // 2-station, no reachable ambiguity pin, so it collapses to a pure-PB near-null
    // gauge (measured: 89 such directions on the 305 arm — the exact phase
    // counterparts L2X/L7X/L1I of the quarantined code pairs C2X/C7X/C1I).
    // Withholding it folds the bias into each station's float ambiguity, symmetric
    // with the code sponge.  (A per-SAT co-gate was too coarse — a sat admitted on
    // its dense signals kept its sparse signals' phase columns; the granularity
    // must match the CODE ADMISSION unit, which is per-(sat,signal).)
    std::set<std::string> cbadm_reg_code_keys;   // raw "sat|sig" code keys registered this epoch (admitted AND >=2 present stations)
    const bool cbadm_on =
        config_.estimateNetworkSatCodeBias() && ctx.admission.active;
    if (cbadm_on) {
        std::map<std::string, int> code_epoch_count;   // admitted "sat|sig" -> #present stations this epoch
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatBiasStates())
                if (ctx.admission.isAdmitted(kv.first)) ++code_epoch_count[kv.first];
        }
        for (const auto& [key, cnt] : code_epoch_count)
            if (cnt >= 2) cbadm_reg_code_keys.insert(key);
    }
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // Register every shared_phase_key unconditionally — the CB-ADM phase
        // co-gate lives UPSTREAM at phase-bias STATE ALLOCATION
        // (MeasurementModel::ensureNetworkSharedStates, cbadm_admitted_code_keys),
        // so shared_phase_keys already excludes a quarantined signal's phase.
        // Gating here instead would break the "every shared_phase_key is a
        // registered column" invariant that eq34/carry/snapshot rely on (crash).
        est.registerPhaseBias(parts->satellite, parts->signal);
        // Card H2: MEASURE the granularity seam's orphan window.  This loop is the
        // only place that sees all three sets at once — the census-admitted set
        // (ctx.admission), this epoch's registered code keys (cbadm_reg_code_keys),
        // and the shared phase set (shared_phase_keys).  An ADMITTED code key that
        // fell below 2 present stations THIS epoch has its code column withheld
        // while its same-signal phase column stays shared: one (key, epoch) pair of
        // orphan window.  Expected 0 at 305-station density (every admitted sat has
        // >= 2 present stations there); a SPARSE-tier run showing it nonzero AND
        // coincident with withholds is the data that reopens the ruling.
        if (cbadm_on) {
            // KEY-FORMAT ASYMMETRY — the trap this counter exists to avoid falling
            // into.  Phase-bias keys carry the PHASE observable ("C26|L2X|PB");
            // the admission census and cbadm_reg_code_keys carry the CODE one
            // ("C26|C2X").  The companion mapping is L<band><attr> -> C<band><attr>,
            // the SAME transform the state-alloc co-gate applies
            // (MeasurementModel::ensureNetworkSharedStates) — reusing the phase
            // signal verbatim would match nothing and leave this counter
            // permanently 0, i.e. inert wiring that reads as "no orphans"
            // (standing invariant #4).
            std::string csig = parts->signal;
            if (!csig.empty()) csig[0] = 'C';
            const std::string code_key =
                StateKeys::makeSatBiasKey(parts->satellite, csig);
            if (ctx.admission.isAdmitted(code_key) && !cbadm_reg_code_keys.count(code_key))
                ++ctx.counters.n_cbadm_phase_orphan_key_epochs;
        }
    }
    // Register the satellite CODE-bias (OSB) columns (W6, full multi-band):
    // EVERY (sat,signal) — every frequency band AND every tracking channel
    // (attribute) — that at least two present stations carry an OSB state for.
    // Collected (deduplicated, first-seen order) for the code-bias datum
    // constraints, the D2 carry-forward, and the solution trace.  Empty unless
    // estimate_sat_code_bias.
    //
    // The default admission gate (CB-ADM OFF) is per-epoch >=2-station coverage:
    // a single-station code bias is perfectly confounded with that one station's
    // receiver code bias and cannot be separated, so it stays a local nuisance
    // (its sat-bias state is pruned below, leaving its raw code row to fold into
    // the receiver clock / slant iono — the pre-W6 path).  Everything with >=2
    // stations is a first-class shared OSB column and is datumed below: the GIM
    // between-satellite iono structure makes the inter-band DCBs observable,
    // while same-band channels (C1C vs C1W, C2W vs C2L, ...) need no iono at all
    // (equal frequency → the receiver and ionosphere terms cancel in the raw
    // between-channel difference, leaving the satellite inter-channel bias).
    // The earlier slice confined the OSB to the two lowest bands and so emitted
    // nothing on a 3rd band; the datum below now pins every registered column.
    //
    // Under CB-ADM (ctx.admission.active — network.cb_admission ON), the
    // per-epoch count is replaced by the run-latched census verdict: an ADMITTED
    // pair registers whenever any present station holds its state (riding through
    // 1-station dips — the anti-flap win); a QUARANTINED pair is NEVER pruned
    // (station_keep always holds it) so it survives as a station-local sponge
    // (Schur-eliminated station-side, never registered, never datumed).  See
    // filter/NetworkBiasAdmission.hpp.
    if (config_.estimateNetworkSatCodeBias()) {
        std::set<std::string> cb_seen;
        NetworkBiasAdmission& adm = ctx.admission;
        if (!adm.active) {
            // --- CB-ADM OFF: legacy per-epoch >=2-station gate (byte-identical). ---
            std::map<std::string, int> cb_station_count;   // "sat|signal" -> #stations
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatBiasStates())
                    ++cb_station_count[kv.first];
            }
            std::set<std::string> keep_keys;
            for (const auto& [key, cnt] : cb_station_count)
                if (cnt >= 2) keep_keys.insert(key);

            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    if (!keep_keys.count(kv.first)) continue;
                    station_keep.insert(kv.first);
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;
                    est.registerCodeBias(parts->satellite, parts->signal);
                    if (cb_seen.insert(kv.first).second)
                        registered_code_keys.emplace_back(parts->satellite, parts->signal);
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        } else {
            // --- CB-ADM ON: run-latched admission + PER-EPOCH >=2 registration +
            // station-local quarantine.  cbadm_reg_code_keys (computed above the
            // phase loop) is the admitted set restricted to keys with >= 2 present
            // stations THIS epoch — only those register a shared column.  Every SM
            // state is kept (station_keep holds it unconditionally: quarantined AND
            // per-epoch-withheld admitted states are never pruned), so a withheld
            // column rides the scatter's codeBiasColumn-throw local fallback this
            // epoch and re-registers when coverage returns.
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    station_keep.insert(kv.first);   // never pruned while the flag is on
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;            // malformed key: kept (harmless), never registered
                    if (cbadm_reg_code_keys.count(kv.first)) {
                        est.registerCodeBias(parts->satellite, parts->signal);
                        if (cb_seen.insert(kv.first).second)
                            registered_code_keys.emplace_back(parts->satellite, parts->signal);
                    } else if (!adm.isAdmitted(kv.first)
                               && !adm.seen.count(kv.first)
                               && adm.late_tracked.insert(kv.first).second) {
                        // A key the census never classified ⇒ first seen after the
                        // window ⇒ implicitly quarantined (invariant #4 telemetry).
                        // (An ADMITTED key merely below the per-epoch coverage this
                        // epoch is NOT late — it is kept and re-registers later.)
                        ++ctx.counters.n_cb_late_quarantined;
                    }
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        }
    }

    // Register the global SH-VTEC coefficient columns (estimate_iono_sh).  The
    // layout is FIXED — all (N+1)² coefficients of every shell every epoch,
    // independent of the satellite set — so the field is continuous across
    // epochs and the Kaula prior can keep every coefficient soft-pinned (the
    // rank gate then stays at #constellations regardless of coverage; design
    // §6.1).  Dual-layer (Iono-2): the second shell registers under its own
    // "SH2" keys after the legacy layer, preserving layer-0 column order.
    if (config_.estimateNetworkIonoSh() && sh_model) {
        for (const ShLayer& lay :
             shLayers(config_.getGlobalVtecShConfig(), sh_model.get(),
                      setup.sh_model2.get()))
            for (const auto& c : lay.model->coeffs())
                est.registerVtecCoeff(c.n, c.m, c.trig, lay.layer);
    }

    n_clock = static_cast<int>(registered_clock_sats.size());
    n_pb    = static_cast<int>(shared_phase_keys.size());
    n_cb    = static_cast<int>(registered_code_keys.size());
    n_present = static_cast<int>(std::count(present.begin(), present.end(), true));
    last_n_present    = n_present;   // RTB-6: most-recent station-presence count for the health heartbeat
    last_n_clock_sats = n_clock;     // RTB-6 (P4b): most-recent registered-clock-sat count for the health heartbeat

    // Column -> human descriptor map (W6 estimability diagnostic): lets the
    // one-shot spectrum dump name which shared parameters span the near-zero
    // eigen-directions (unpinned gauges) instead of guessing.  Populated only
    // when the (default-OFF) estimability trace is armed — it feeds nothing but
    // that dump, so a production run skips the string-building entirely.
    col_desc.resize(static_cast<size_t>(est.numShared()));
    if (spectrum_diag_budget > 0 && nc.trace_estimability) {
        for (const std::string& sat : registered_clock_sats)
            col_desc[static_cast<size_t>(est.clockColumn(sat))] = "CLK|" + sat;
        for (const auto& pk : shared_phase_keys) {
            const auto p = StateKeys::parseSatPhaseBiasKey(pk);
            if (p) col_desc[static_cast<size_t>(est.phaseBiasColumn(p->satellite, p->signal))]
                       = "PB|" + p->satellite + "|" + p->signal;
        }
        for (const auto& kv : registered_code_keys)
            col_desc[static_cast<size_t>(est.codeBiasColumn(kv.first, kv.second))]
                = "CB|" + kv.first + "|" + kv.second;
        // Name the SH-VTEC columns — a HARD requirement (design §6.4): otherwise
        // a missing SH datum prints as an anonymous colNNN in the spectrum dump,
        // indistinguishable from a clock-datum fault.
        if (config_.estimateNetworkIonoSh() && sh_model) {
            for (const ShLayer& lay :
                 shLayers(config_.getGlobalVtecShConfig(), sh_model.get(),
                          setup.sh_model2.get()))
                for (const auto& c : lay.model->coeffs())
                    col_desc[static_cast<size_t>(
                        est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer))] =
                        fmt::format("{}|{}|{}|{}", lay.layer > 0 ? "SH2" : "SH",
                                    c.n, c.m, c.trig);
        }
    }

    if (est.numShared() == 0) {
        ++n_skipped;
        if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                         << ',' << n_present << ",0,0,0,0,0,0,0,0\n";
        { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
          emitEpochStatus("sync_skip", es); }
        prev_week = se.time.week; prev_tow = se.time.tow;
        return EpochAction::SkipEpoch;
    }

    est.beginEpoch();
    return EpochAction::Proceed;
}

EpochAction Application::networkPassB_(const SyncedEpoch& se,
                                       NetworkRunContext& ctx,
                                       NetworkSetupResult& setup,
                                       NetworkRunTraces& traces,
                                       NetworkEstimator& est,
                                       const NetworkIonoDeweightGate& iono_deweight,
                                       double sh_dev_tau, bool sh_dev_on, bool sh_dev_demean,
                                       bool sh_dev_d5_keep,
                                       const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                                       const std::set<std::pair<int, std::string>>& carry_slipped_raw,
                                       bool carry_warm, double pl_now_s,
                                       std::vector<std::vector<PppObservation>>& modeled,
                                       std::vector<bool>& present,
                                       const std::set<std::string>& shared_phase_keys,
                                       const std::vector<std::string>& registered_clock_sats,
                                       const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                                       int n_clock, int n_pb, int n_present,
                                       std::vector<StationAssembly>& asm_res,
                                       std::vector<int>& est_index_of, int& total_pins,
                                       double& sec_asm_ms_epoch, double& sec_asm_wall_ms) {
    // Alias block (INC-7 migration mechanic) — verbatim copies of the
    // runNetwork() alias/derivation lines; same underlying objects.
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();
    const bool sec_timers_on = nc.trace_section_timers;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    auto& sh_model = setup.sh_model;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const double q_clock = ctx.knobs.q_clock;        // m²/s
    const double q_pb    = ctx.knobs.q_pb;   // m²/s
    const double q_cb    = ctx.knobs.q_cb;    // m²/s (W6 OSB carry-forward)
    const bool   pl_on         = ctx.knobs.pl_on;
    const double pl_q_amb      = ctx.knobs.pl_q_amb;
    const double pl_max_gap_s  = ctx.knobs.pl_max_gap_s;
    const double pl_var_floor  = ctx.knobs.pl_var_floor;  // lock-in guard; 0 = off
    const bool   pl_los_scale  = ctx.knobs.pl_los_scale;  // F1: per-LOS B_eff variance scale at inject (SPEC_amb_iono_split_overcount); OFF = byte-identical
    const bool   pl_local_block = ctx.knobs.pl_local_block; // F3: station-wide jointly-whitened local block at inject (SPEC_amb_iono_split_overcount); OFF = byte-identical
    // RF-1 §7.4 (network.trace_amb_state): record, per FOLDED ambiguity column, the
    // variance the fold ACTUALLY used and the ROUTE it took, into this station's own
    // StationAssembly — the pl_los_beff discipline.  MEASURE-ONLY, and OFF the map is
    // never touched, so the produce loop pays one bool test per folded column.
    const bool   amb_state_tr  = ctx.knobs.trace_amb_state;
    // RF-1 step 4: the vehicle owns the slip RESPONSE.  Under float_kf the free-epoch
    // latch and the gap cliff no longer SKIP the inject — the serial pre-assembly pass
    // (networkAmbFloatApplyDrops) has already dropped those arcs to their birth variance,
    // so they fold at birth strength.  OFF, both sites keep their `continue` verbatim.
    const bool   amb_float_kf  = ctx.knobs.amb_float_kf;
    const bool   amb_floor_on  = amb_float_kf && ctx.knobs.amb_birth_var_floor > 0.0;
    // SPEC_slip_relevel_walk: read-only parallel consult of the latched
    // free-epoch set (filled serially in PASS-A; const for the whole produce).
    const bool   relevel_free_on = config_.networkCycleSlipRelevelFreeEpoch();
    // SIM-ONLY ORACLE AMBIGUITY PIN (filter/NetworkOraclePin): THIS epoch's pin
    // table, built serially before PASS B and consulted READ-ONLY here.  A const
    // pointer, null on every run with no `oracle_pin` event armed, so the produce
    // loop's pin block is unreachable OFF (standing invariant #6).
    const std::map<std::tuple<int, std::string, std::string>,
                   NetworkOraclePin::Pin>* opin_table =
        ctx.carry.oracle_pin ? &ctx.carry.oracle_pin->table : nullptr;
    const bool   opin_on = (opin_table != nullptr) && !opin_table->empty();
    const auto&  vce_apply     = ctx.carry.vce_apply;     // M2 v2: carry variance factor α² (unity when off)
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // Card 1b: JOINTLY whiten the coupling rows (departed cols → M_w) — requires carry_cpl_cond (enforced at load)
    // F3 live predicate: the block's rows are Card-1 coupling rows (A_s/A_r), and the
    // Card-1b joint path owns that row block when armed — mutually exclusive here
    // (the loader warns INERT on either miss).  A single const bool, so OFF never
    // reaches a block branch.
    const bool   pl_blk_on = pl_local_block && carry_cpl_cond && !carry_cpl_joint;
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // Card 1d: border time update of the coupling rows at injection (κ shrink + M²κq inflation) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_shmarg = ctx.knobs.carry_cpl_shmarg; // Card 1d-SH: fully marginalize VtecSh coupling terms (GM border ≠ RW κ model — severs the amb↔SH carry feedback) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_reinit = ctx.knobs.carry_cpl_reinit; // Card 1g: widen the reinit feed to the realization-step sources and honour it with κ off — requires carry_cpl_cond (enforced at load)
    const bool   zwd_on        = ctx.knobs.zwd_on;                 // B9: carry per-station ZWD across epochs
    // B9: the ZWD carry's random-walk process noise (m²/s).  RS-10b retired the bare
    // `zwd_q` alias here — both fold sites read `zwd_q_eff` below instead, so a seam
    // window's own q cannot be missed at one of them.
    // Seam-scoped ZWD re-seed + unconditional Z-row capture (§17 finding 3).
    // zwd_seam_live: the post-resume inject window is open — run the exact B9 fold.
    // zwd_capture: snapshot-armed run — RECORD R.pl_zwd for the harvest (no inject).
    // Both are stable across the parallel produce phase (the countdown decrements
    // serially at the post-solve harvest).
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    // RS-10b (docs/SPEC_event_harness.md §21.21): the ZWD carry's EFFECTIVE RW q for
    // this epoch's folds.  network.zwd_carry_q is in the warm-resume fingerprint (a
    // continuous carry q must cold-start), while the seam response that borrowed its
    // weight is deliberately not — so the seam window's stiffness was untunable on a
    // resume leg.  network.zwd_seam_reseed_q (default -1 = unset) replaces it, and
    // ONLY while the window is live: a continuous carry_zwd row of a non-resumed epoch
    // never sees it, and an unset knob leaves this exactly `zwd_q` (byte-identical).
    // Resolved ONCE here, off the same `zwd_seam_live` the fold gates read, so the
    // diagonal B9 row, the B9-in-block column and the harvest's prior-residual census
    // cannot disagree about which q was folded.
    const double zwd_q_eff     = ctx.knobs.zwdQEff(zwd_seam_live);
    // B9-in-block live predicate (network.carry_zwd_in_block): the carried ZWD column
    // joins the F3 whitened block instead of folding as a diagonal coupling row outside
    // it.  Conjoined with pl_blk_on, so a station-epoch that takes the F1 / per-column
    // route (F3 off, joint whitening on, coupling rows off) is never touched — there the
    // ZWD keeps its existing B9 row.  The loader already refuses the arm without
    // carry_zwd + phase_leveling_local_block; this is the LIVE half of the same gate.
    const bool   zwd_blk_on    = ctx.knobs.zwd_in_block && pl_blk_on;
    // I-in-block live predicate (network.carry_iono_in_block): the station's carried
    // per-LOS slant-iono columns join the F3 whitened block.  Conjoined with pl_blk_on for
    // the same reason the ZWD's is — and here it is load-bearing rather than defensive:
    // this card has NO diagonal fallback row (a diagonal per-LOS iono prior IS the
    // falsified QUEUE-8(b) vehicle), so off the block path there is simply nothing to do.
    // The loader already refuses the arm without phase_leveling + phase_leveling_local_block
    // (and hard-errors on a network.carry_iono co-arm); this is the LIVE half of that gate.
    const bool   iono_blk_on   = ctx.knobs.iono_blk && pl_blk_on;
    // AGE-AWARE young-arc inflate (network.phase_leveling_young_inflate_var_m2,
    // SPEC_amb_iono_split_overcount 6.17-6.18).  NOT conjoined with pl_blk_on: the
    // card is wired on BOTH routes - inside the block as a congruence scaling of the
    // column's row/column of Sigma, outside it as var_eff += var_add on the per-column
    // row.  The loader has already disabled it off the leveling vehicle (phase_leveling
    // off, carry_invariant on), so on() is the whole live predicate.
    const auto&  yng           = ctx.knobs.yng;
    const bool   yng_on        = yng.on();
    // Age is counted in LOOP EPOCHS, and the denominator is the run's OWN observed
    // inter-epoch spacing (this PASS-B time minus the previous harvest stamp) - the
    // I-in-block card's rule, for the same reason: the knob then means the same thing at
    // 1 s and 30 s cadence and no nominal rate is invented.  A first epoch / implausible
    // spacing leaves it 0, which varAdd() reads as "age unknowable => inflate nothing".
    const double yng_dt_ep = [&] {
        if (!yng_on) return 0.0;
        const double dt = pl_now_s - ctx.carry.pl_prev_now_s;
        return (std::isfinite(dt) && dt > 0.0 && dt <= pl_max_gap_s) ? dt : 0.0;
    }();
    const double iono_blk_q    = ctx.knobs.iono_blk_q_m2;      // m² at L1, PER EPOCH
    const double iono_blk_gap_s = ctx.knobs.iono_blk_max_gap_s;
    // RX-CLK-in-block live predicate (network.carry_rx_clock_in_block): the station's
    // RECEIVER CLOCK column (and its ISBs) join the F3 whitened block.  Conjoined with
    // pl_blk_on and load-bearing for the same reason the iono card's is: this card has NO
    // diagonal fallback row (a diagonal prior on the most station-COMMON direction of all
    // is the n_LOS over-count that falsified the diagonal ZWD carry), so off the block
    // path there is simply nothing to do.  The loader already refuses the arm without
    // phase_leveling + phase_leveling_local_block; this is the LIVE half of that gate.
    const bool   rxclk_blk_on  = ctx.knobs.rxclk_blk && pl_blk_on;
    const double rxclk_q       = ctx.knobs.rxclk_q_m2;       // m², PER EPOCH
    const double rxclk_gap_s   = ctx.knobs.rxclk_max_gap_s;
    const double rxclk_rate_var0 = ctx.knobs.rxclk_rate_var0; // m²/s², the unknown-drift prior
    const bool   rxclk_isb     = ctx.knobs.rxclk_isb;
    const bool   iono_on       = ctx.knobs.iono_on;              // QUEUE 8b: slant-iono carry
    const bool   iono_scope_always = ctx.knobs.iono_scope_always; // QUEUE 8b: inject every epoch (vs re-leveling LOS only)
    const double iono_q        = ctx.knobs.iono_q;               // QUEUE 8b: iono carry RW process noise (m²/s)
    const double iono_var_floor = ctx.knobs.iono_var_floor;      // QUEUE 8b: inject-side variance floor (m²)
    const double iono_max_gap_s = ctx.knobs.iono_max_gap_s;      // QUEUE 8b: carry eviction gap (s)
    const bool   rx_cb_on      = ctx.knobs.rx_cb_on;             // rx-CB carry (SPEC_rx_cb_carry)
    const bool   rx_cb_datum  = ctx.knobs.rx_cb_datum;           // RXCB-DATUM: zero-mean-over-receivers DCB datum (shares the pl_cb walk with the carry)
    const double rx_cb_datum_sigma = ctx.knobs.rx_cb_datum_sigma; // RXCB-DATUM loose physical-bound sigma (m)
    const bool   sta_prior_pre_pd = ctx.knobs.sta_prior_pre_pd;   // RF-PD-fix: route the RXCB-DATUM rows to the PRE-PD fold
    const double rx_cb_q       = ctx.knobs.rx_cb_q;              // rx-CB carry RW process noise (m²/s)
    const bool   rx_cb_demean  = ctx.knobs.rx_cb_demean;         // Card LV-2: fold η = λ(innov − μ), gate on |innov − μ|
    const bool   rx_cb_step_gate = ctx.knobs.rx_cb_step_gate;    // Card LV-4: retire the frame-dependent inject screen (the harvest owns the event)
    const auto&  rx_cb_demean_mu = ctx.carry.rx_cb_demean_mu;    // LV-2: lag-1 μ per code-bias family — READ-ONLY here (serial harvest writes it)
    const bool   lv1_seed_frame = ctx.knobs.lv1_seed_frame;      // Card LV-1: seed per-LOS iono in the field's frame
    const int    lv1_min_sats   = ctx.knobs.lv1_min_sats;        // LV-1: per-station median sample floor
    const auto&  sh_field_lag1  = ctx.carry.sh_field_lag1;       // LV-1: lag-1 solved SH coefficients — READ-ONLY here
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;  // >0: inflate-q to this σ; ≤0: hard skip-stash refloat
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    // RAIM pool re-host (docs/SPEC_raim_pool_rehost.md): build the carry-neutral
    // checkable-column census `qc_amb` alongside (not instead of) the carry's pl_amb.
    // Already ANDed with raim_on + the RESOLVED carry_invariant when the knobs were
    // snapshotted, so this is a single bool here.  OFF ⇒ qc_amb is never allocated.
    const bool raim_ci_on              = ctx.knobs.raim_ci_on;
    // TWO-EDGE CONTINUOUS HANDOVER (docs/SPEC_pivot_visibility_gauge_defect.md): a
    // SECOND consumer of the carry-neutral `qc_amb` census.  The handover ledger
    // needs each candidate anchor's absolute ambiguity estimate, and the census is
    // already exactly the right population — columns PRESENT in A_r and UNPINNED,
    // i.e. every station that could receive a pin but does not hold one now.
    // Widening the existing census rather than adding a third parallel one keeps a
    // single answer to "which columns are enumerable" (the same discipline that
    // made `pl_amb` a subset of `qc_amb` instead of a sibling).
    const bool pd_handover_continuous  = ctx.knobs.pd_handover_continuous;
    // Station congruence screen, CG-INC-0 (docs/SPEC_redundancy_census.md).  The ONLY
    // thing the assembly owes that card: retain each data row's position partial,
    // which networkSplitStation_ is about to drop on the floor because the network
    // holds every coordinate fixed.  OFF ⇒ the block below never runs and
    // StationAssembly::row_pos_enu stays empty (byte-identical, zero allocation).
    const bool congruence_rows         = ctx.knobs.cg_on;
    const bool phase_datum_ensemble    = ctx.knobs.phase_datum_ensemble;
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    const double eq34_rpb_prior_sigma = ctx.knobs.eq34_rpb_prior_sigma;
    const double eq34_rpb_prior_w = ctx.knobs.eq34_rpb_prior_w;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const double phase_hold_sigma = ctx.knobs.phase_hold_sigma;
    const bool   phase_hold_on    = ctx.knobs.phase_hold_on;
    const double phase_hold_var   = ctx.knobs.phase_hold_var;
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_clock_refloat_sats = ctx.carry.prev_clock_refloat_sats;
    auto& tu_reinit_hist = ctx.carry.tu_reinit_hist;
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& iono_carry = ctx.carry.iono_carry;     // QUEUE 8b: produce phase READS only
    auto& iono_blk_carry = ctx.carry.iono_blk_carry;  // I-in-block: produce phase READS only
    auto& rx_clk_carry = ctx.carry.rx_clk_carry;      // RX-CLK-in-block: produce phase READS only
    auto& rx_clk_rate  = ctx.carry.rx_clk_rate;       // …and its DRIFT half (read-only here; std::map::find is concurrency-safe, operator[] would not be)
    auto& rpb_carry = ctx.carry.rpb_carry;
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
    auto& rx_cb_carry = ctx.carry.rx_cb_carry;   // rx-CB carry: produce phase READS only
    auto& n_cpl_joint_fallback = ctx.counters.n_cpl_joint_fallback;
    auto& n_cpl_terms_present = ctx.counters.n_cpl_terms_present;
    auto& n_cpl_terms_departed = ctx.counters.n_cpl_terms_departed;
    auto& n_cpl_terms_shrunk = ctx.counters.n_cpl_terms_shrunk;
    auto& n_cpl_row_dump = ctx.counters.n_cpl_row_dump;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };

    // --- PASS B: per-station assembly, then SERIAL scatter -------------
    // Split into a PARALLEL produce phase (pure per-station compute into
    // pre-sized slots — no shared write) and a SERIAL consume phase that
    // scatters each block into the shared arrowhead in deterministic station
    // order.  computeMeasurementUpdate + splitStation + the pivot pin fold
    // touch only station i's own state and CONST queries on `est` (its column
    // layout is frozen after registration; addStation is the only mutation
    // and runs serially below) — so the produced blocks and their add order
    // are identical for any thread count.
    //
    // D0: the assembly wall brackets the parallel produce (per-station row
    // build) + the serial scatter (est.addStation) — the "assembly" section.
    std::chrono::steady_clock::time_point sec_asm_t0;
    if (sec_timers_on) sec_asm_t0 = std::chrono::steady_clock::now();

    // T5v2: age the SH-deviation carries ONCE per epoch (SERIAL — the parallel
    // assembly below only reads).  GM prediction in vertical TECU: dev *= λ,
    // P → λ²P + σ_ss²(1−λ²); entries unseen for > 5τ are dropped (fully
    // decorrelated — the cold seed the pin then uses is statistically
    // equivalent).  Iono-2 slip integration (standing invariant #1, user spec
    // Jul-12): a cycle slip aliases n·λ into the GF slant iono, so any
    // (station,sat) with an entry in this epoch's C3c funnel is ERASED —
    // reset-not-repair, cold re-tap at σ_ss.  The funnel's per-signal keys
    // project to the satellite (union semantics; an LLI single-band event still
    // resets the per-sat state).  Recorded at the PASS-A DETECTION site, so the
    // erase lands the same epoch the slip was seen — the poisoned pre-slip
    // deviation never constrains a post-slip row.
    // QUEUE 8b slip edges (invariants #1/#7): serial adjudication of the slant-iono
    // carry BEFORE the parallel produce reads the map.  KEEP through any slip whose
    // bands were freed this epoch (the physical ionosphere is continuous through a
    // receiver cycle slip, and the free epoch disarms the stale-level aliasing
    // channel — the carried value predates the slip and is untouched by it); ERASE
    // for slip-funnel entries that were NOT freed (a still-leveled band can alias
    // n·λ into the GF slant estimate, the same reset-not-repair rationale as the
    // mech-B funnel below).  On the relevel posture the detector drains feed only
    // the free set (funnel inserts are carry_invariant-gated), so the erase branch
    // is expected ~0 there — a nonzero count on that posture is a posture change,
    // not noise.  The T4/RAIM erase edge lives in NetworkRaim::refloat_sat_at.
    if (iono_on) {
        std::set<std::pair<int, std::string>> freed_los;   // (station idx, sat)
        for (const auto& fa : ctx.carry.relevel.free_ambs) {
            const auto fparts = StateKeys::parseAmbiguityKey(fa.second);
            if (fparts) freed_los.insert({fa.first, fparts->satellite});
        }
        for (const auto& fl : freed_los)
            if (iono_carry.count(fl))
                ++ctx.counters.n_iono_carry_kept_through_slip;
        for (const auto& sk : carry_slipped_ambs) {
            const auto sparts = StateKeys::parseAmbiguityKey(sk.second);
            if (!sparts) continue;
            const std::pair<int, std::string> los{sk.first, sparts->satellite};
            if (freed_los.count(los)) continue;   // KEEP — counted above
            if (iono_carry.erase(los))
                ++ctx.counters.n_iono_carry_erased_slip;
        }
    }
    if (sh_dev_on) {
        std::set<std::string> slipped_los;   // "STATION|SAT" projection of the funnel
        std::set<std::string> raw_los;       // v3k: the RAW-detector-confirmed subset
        for (const auto& sk : carry_slipped_ambs) {
            if (sk.first < 0 || sk.first >= static_cast<int>(stations.size())) continue;
            const auto parts = StateKeys::parseAmbiguityKey(sk.second);
            if (!parts) continue;
            slipped_los.insert(stations[static_cast<std::size_t>(sk.first)].name
                               + "|" + parts->satellite);
        }
        if (sh_dev_d5_keep) {
            for (const auto& sk : carry_slipped_raw) {
                if (sk.first < 0 || sk.first >= static_cast<int>(stations.size())) continue;
                const auto parts = StateKeys::parseAmbiguityKey(sk.second);
                if (!parts) continue;
                raw_los.insert(stations[static_cast<std::size_t>(sk.first)].name
                               + "|" + parts->satellite);
            }
        }
        // Adaptive iono q (network.adaptive_iono_q): per-LOS GM boost factors from
        // the PASS-A station monitors (already rolled forward this epoch — PASS A
        // precedes this serial block).  The multiplier inflates ONLY the innovation
        // term σ_ss²(1−λ²) below — the GM shape (τ) and the carried level are
        // untouched, so a quiet LOS (m=1) ages byte-identically and the disarmed
        // path never builds the map.
        std::map<std::string, double> aiq_mult;
        const bool aiq_on = nc.adaptive_iono_q;
        if (aiq_on) {
            for (const auto& sta : stations) {
                if (!sta.iono_var) continue;
                for (const auto& [aiq_sat, aiq_m] : sta.iono_var->multipliers())
                    if (aiq_m > 1.0) aiq_mult.emplace(sta.name + "|" + aiq_sat, aiq_m);
            }
        }
        const double now_s = se.time.week * 604800.0 + se.time.tow;
        for (auto it = sh_dev_carry.begin(); it != sh_dev_carry.end();) {
            ShDevCarry& dc = it->second;
            const double dt = now_s - dc.t_s;
            if (!slipped_los.empty() && slipped_los.count(it->first)) {
                // v3k erase-policy split (SPEC_iono2_mechB_v3 addendum): a
                // RAW-confirmed slip (MW/LLI/IF-TD this epoch) aliases n·λ into
                // the GF iono — the pre-slip deviation is poison, ERASE
                // (invariant #1, unchanged).  A D5-ONLY re-tap (97 % have no
                // raw observable step) is iono tension the deviation exists to
                // ABSORB: keep the accumulated level, re-tap the confidence to
                // steady state (weak prior — a genuinely missed fine slip gets
                // re-estimated away within a few harvests instead of being
                // carried stiffly).  The v3 de-mean drains any common mode of
                // the kept levels, so the keep cannot re-open the ratchet.
                if (!sh_dev_d5_keep || raw_los.count(it->first)) {
                    ++ctx.carry.shdev_slip_resets_total;
                    it = sh_dev_carry.erase(it);
                    continue;
                }
                shDevD5Retap(dc);
                ++ctx.carry.shdev_d5_keeps_total;
                // falls through to staleness + GM aging (retap is idempotent
                // under the aging step: λ²σ_ss² + (1−λ²)σ_ss² = σ_ss²).
            }
            if (dc.t_s <= 0.0 || dt > 5.0 * sh_dev_tau) {
                it = sh_dev_carry.erase(it);
                continue;
            }
            if (dt > 0.0) {
                const double lam = std::exp(-dt / sh_dev_tau);
                dc.dev *= lam;
                // Adaptive iono q: storm-scaled innovation for this LOS (1.0 when
                // disarmed / quiet / unmonitored — the legacy expression exactly).
                double aiq_scale = 1.0;
                if (aiq_on && !aiq_mult.empty()) {
                    const auto mit = aiq_mult.find(it->first);
                    if (mit != aiq_mult.end()) {
                        aiq_scale = mit->second;
                        ++ctx.counters.aiq_boosts_total;
                        if (aiq_scale > ctx.counters.aiq_max_mult)
                            ctx.counters.aiq_max_mult = aiq_scale;
                    }
                }
                dc.var  = lam * lam * dc.var + aiq_scale * dc.var_ss * (1.0 - lam * lam);
            }
            ++it;
        }
        // Iono-2 v3 anti-ratchet (docs/SPEC_iono2_mechB_v3.md): remove the
        // information-weighted global + per-station means from the carried
        // priors.  A common vertical-TECU deviation shares the a00 column's
        // exact row signature (M(el)·tecu_to_m), so the pair is quasi-null and
        // the harvest→age→re-assert alternation ratchets it (measured a00
        // 25 → 104 TECU/3 h carry-alone, 207–255 with the dual layer).  The
        // common component belongs to a00 / the receiver DCB, which each
        // epoch's data re-derives; the carry must not re-assert it as an
        // independent prior.  Same serial context as the aging above.
        // Thresholds 30/4 per the spec (constants, not knobs).
        if (sh_dev_demean) {
            const ShDevDemeanStats ds = shDevDemean(sh_dev_carry, 30, 4);
            if (ds.applied) {
                ++ctx.carry.shdev_demean_epochs_total;
                ctx.carry.shdev_demean_sta_total += ds.n_sta;
                ctx.carry.shdev_demean_abs_g_sum += std::abs(ds.g_tecu);
            }
        }
    }

    // Card 1d (network.carry_coupling_time_update): record the (kind, sat) shared
    // keys whose carried state folds THIS epoch with the loose clock-monitor
    // REINIT variance (the D2 inflate_q path — the stash marks the flagged sat's
    // clock AND its phase-/code-bias entries, kind-independently; the phase-hold
    // path deliberately keeps its pb time-locked and is stashed inflate_q=false,
    // so mirroring the stash flags reproduces the fold's exact per-kind
    // treatment).  A coupling term on such a column must not shrink-carry — the
    // reinit decorrelates the border column from its stashed value, so the term
    // is fully marginalized (κ=0, σ² += M²·P_prev), mirroring the dense fold's
    // decorrelation rule.  Kept as a HISTORY map (key → content time of the last
    // reinit fold): a coupling row injects against its own harvest time, so a
    // reinit inside a MULTI-EPOCH carry gap still marginalizes (a q=∞ segment
    // anywhere inside the gap composes to κ=0).  The Clock entry also covers the
    // clock_model=ON oscillator reinit (the clock never rides prev_carry there)
    // and the HARD-refloat mode (clk_mon_reinit_sig ≤ 0 skip-stashes the flagged
    // sat entirely — no prev_carry entry to mirror; all three kinds decorrelate,
    // matching the skip-stash).  Updated SERIALLY; PASS B only reads it.
    //
    // Card 1g (network.carry_coupling_reinit_sources) widens BOTH halves of this: the
    // key gains a signal field (empty = the SAT-WIDE wildcard these clock-QC feeds
    // write, since they decorrelate a whole satellite kind-by-kind), and the
    // REALIZATION-STEP sources below join it.  Those are a different failure: not a
    // decorrelated variance but a FRAME STEP.  A phase-anchor handover, an eq34 chain
    // re-reference or a datum-slip PB re-realization each erase the re-referenced
    // family's OWN carry, yet every OTHER carried local keeps a coupling[] entry on
    // that column holding a retired-frame ŝ_prev — folded at full weight, that injects
    // the step as row bias at conditional-variance confidence.  The step is UNKNOWN
    // (that is what distinguishes it from the eq34 D4 S-transform, which knows its
    // own shift and re-bases the carry exactly), so the departed route is the only
    // correct treatment.  Both reref sets are resolved at the phase-datum election,
    // strictly BEFORE this serial write and hence before PASS B reads it.
    if (carry_cpl_tu || carry_cpl_reinit) {
        for (const CarryPrior& cp : prev_carry)
            if (cp.inflate_q)
                tu_reinit_hist[{cp.kind, cp.sat, std::string{}}] = pl_now_s;
        for (const std::string& sat : prev_clock_refloat_sats) {
            tu_reinit_hist[{SharedParamKind::Clock, sat, std::string{}}] = pl_now_s;
            if (!(clk_mon_reinit_sig > 0.0)) {
                tu_reinit_hist[{SharedParamKind::PhaseBias, sat, std::string{}}] = pl_now_s;
                tu_reinit_hist[{SharedParamKind::CodeBias, sat, std::string{}}] = pl_now_s;
            }
        }
    }
    if (carry_cpl_reinit) {
        // QUEUE item 1(d)'s anchored-election handovers and the eq34 chain datum's
        // re-references: both are sets of (sat, signal), which is exactly the
        // granularity at which a phase-bias datum is realized.
        for (const auto& [sat, sig] : ctx.carry.phase_anchor_reref)
            tu_reinit_hist[{SharedParamKind::PhaseBias, sat, sig}] = pl_now_s;
        for (const auto& [sat, sig] : ctx.carry.eq34_reref)
            tu_reinit_hist[{SharedParamKind::PhaseBias, sat, sig}] = pl_now_s;
        // The datum-slip re-realization marks whole satellites (the slip is on the
        // pin-holder's arc, not on one signal) — sat-wide wildcard.
        for (const std::string& sat : ctx.carry.datum_reref_pb_sats)
            tu_reinit_hist[{SharedParamKind::PhaseBias, sat, std::string{}}] = pl_now_s;
    }

    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
    for (int i = 0; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        StationAssembly& R = asm_res[i];
        try {
            NetStation& st = stations[i];

            // --- Card LV-1 (docs/SPEC_level_information_budget.md) -------------
            // Install this station's seed frame BEFORE the state pre-pass inside
            // computeMeasurementUpdate allocates (and seeds) any new per-LOS iono
            // state.  The map is the lag-1 field sampled at each of this station's
            // lines of sight; the pre-pass removes the station's MEDIAN raw seed
            // (its receiver GF hardware bias ⊕ median slant iono) and puts the
            // field's own value there instead, so a state born now — at cold start
            // OR at a slip refloat / gap re-birth mid-run — inherits the CURRENT
            // frame rather than the receiver's bias.  That "inside the seed path"
            // placement IS the invariant-#1 event response.
            //
            // Read-only on ctx (knobs + the lag-1 coefficient snapshot) and pure
            // per-station compute: safe in this parallel produce phase.  The frame
            // is consumed once by the pre-pass; a station that never reaches this
            // point simply seeds raw and is counted.
            if (lv1_seed_frame && sh_model) {
                const std::vector<ShLayer> lv1_lay =
                    shLayers(shc, sh_model.get(), setup.sh_model2.get());
                const double lv1_Re = shc.earth_radius_km * 1000.0;
                const double lv1_tecu_to_L1m =
                    40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
                const double lv1_lon_sun =
                    shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
                StateInitializer::IonoSeedFrame frame;
                frame.min_sats = lv1_min_sats;
                frame.station  = st.name;
                std::vector<double> lv1_bvals;
                for (const auto& o : modeled[i]) {
                    // GPS/Galileo only — the same scope as the SH↔slant tie rows
                    // (networkAppendShIonoRows_ excludes GLONASS so its FDMA
                    // per-slot IFB cannot leak into the global field).  A GLONASS
                    // LOS therefore has no tie to the field at all, and putting its
                    // seed in the field's frame would put it in a frame nothing else
                    // in its block shares; those births stay RAW and are counted as
                    // lv1_field_missing.
                    if (o.satellite.empty() || o.satellite.front() == 'R') continue;
                    double slant = 0.0;
                    if (networkShSlantAtLos_(st.ecef, o.sat_pos, lv1_lay, sh_field_lag1,
                                             lv1_Re, lv1_lon_sun, lv1_tecu_to_L1m,
                                             lv1_bvals, slant))
                        frame.field_slant_m[o.satellite] = slant;
                }
                st.mm->getStateInitializer().setIonoSeedFrame(std::move(frame));
            }

            EpochMeasurement em = st.mm->computeMeasurementUpdate(
                modeled[i], st.sm->getStateVector());
            if (em.innovations.size() == 0) { R.empty_innov = true; continue; }
            if (em.H.cols() != static_cast<Eigen::Index>(st.sm->getStateSize())) {
                R.state_mismatch = true;
                R.err = fmt::format("{} vs {}", em.H.cols(), st.sm->getStateSize());
                continue;
            }

            // Alias into the assembly slot so the H->A_r column map survives
            // for the post-solve slant-iono back-substitution (viz trace).
            std::map<int, int>& h2local = R.h2local;
            networkSplitStation_(st, est, em, R.A_s, R.A_r, R.w, R.y, h2local);

            // --- CG-INC-0, station congruence screen (docs/SPEC_redundancy_census.md)
            // Retain the per-row POSITION PARTIAL that networkSplitStation_ just
            // discarded, rotated into the station's local ENU frame.  This is the
            // one place in the run where the row geometry still exists: three
            // columns of `em.H` that the fixed-coordinate network drops by design
            // (the A6 linearity guard in networkSplitStation_ hard-fails if one ever
            // survives into A_r), so without this block a wrong held coordinate has
            // no observable footprint anywhere downstream.
            //
            // ∂y/∂δp_ENU = ∂y/∂δp_ECEF · R_{ENU→ECEF}, because δp_ECEF =
            // R_{ENU→ECEF} δp_ENU and enuToEcefMatrix's COLUMNS are the E/N/U unit
            // vectors in ECEF.  The partial itself is copied verbatim rather than
            // rebuilt from obs.los: `em.H` and `em.innovations` are ONE
            // linearization, so y ≈ H δx holds row by row for every observable the
            // measurement model can produce, and a sign convention re-derived per
            // row type would be a second source of truth waiting to disagree.
            //
            // Only the DATA rows exist at this point — every later fold (the datum
            // pins, the GLONASS anchors, the SH/GIM iono ties, the SD-EWL rows, the
            // Card-1 coupling rows) APPENDS — so sizing to em.H.rows() is exact and
            // the consumer's `row < row_pos_enu.rows()` test is the structural bound
            // on which rows can carry geometry at all.
            //
            // Thread-safety: pure per-station read of st.ecef + em, written into
            // this station's own slot.  Safe in the parallel produce phase.
            //
            // A station with no usable ECEF would rotate through a garbage
            // lat/lon, so it is left EMPTY instead: the sweep then counts its rows
            // in `no_geom_rows` and reports the station as having no geometry,
            // which is the honest reading.  A silently mis-rotated Jacobian would
            // name a real offset in the wrong direction.
            if (congruence_rows && em.H.rows() > 0 && st.ecef.norm() > 1.0) {
                const Eigen::Index pos_i =
                    static_cast<Eigen::Index>(st.sm->getPositionIndex());
                if (pos_i >= 0 && pos_i + 2 < em.H.cols()) {
                    const Coordinates::Llh llh = Coordinates::ecefToLlh(st.ecef);
                    const Eigen::Matrix3d R_enu2ecef =
                        Coordinates::enuToEcefMatrix(llh.lat_rad, llh.lon_rad);
                    R.row_pos_enu.noalias() =
                        em.H.middleCols(pos_i, 3) * R_enu2ecef;
                }
            }

            // Fix A (network.phase_datum_ensemble): record every present
            // ambiguity's (sat, signal, A_r column) so the driver can build the
            // per-(sat,signal) sum-to-zero gauge over all tracking stations after
            // the reduce.  h2local is keyed by state index (== the pl_amb carry's
            // key), so a present phase row ⟹ an entry here.  Filtered to the sats
            // with a shared phase-bias column at gauge-build time.  Pure
            // per-station read — safe in the parallel produce phase.
            if (phase_datum_ensemble) {
                for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    const auto hit = h2local.find(static_cast<int>(sidx));
                    if (hit == h2local.end()) continue;  // no phase row this epoch
                    R.ens_amb_cols.emplace_back(
                        parts->satellite, parts->signal,
                        static_cast<Eigen::Index>(hit->second));
                }
            }

            // The anchor station(s) fold tight-prior pins on the ambiguities
            // they gauge (the LOCAL half of the S-basis): drive each pinned
            // ambiguity's ABSOLUTE value to its integer TARGET so the estimable
            // phase bias absorbs THIS receiver's ambiguity lump (Odijk Eq. 4).
            // Built into the anchor's OWN block — no shared write.  Legacy: the
            // single pivot pins the whole shared_phase_keys set (target 0).
            // Prioritized: each station pins exactly its elected
            // pinned_amb_targets (target 0), the load-bearing invariant — the pin
            // must live in the block whose local ambiguity column it constrains.
            // eq34 (Card 3): the spanning-tree edge pins (target 0 or the
            // pin-to-round integer) PLUS the datum receiver's ONE RPB pin
            // (δ_{r1,σ}=0), both in this station's OWN block.
            const bool do_fold = phase_datum_prioritized
                ? (!st.pinned_amb_targets.empty() || !st.pinned_rpb_keys.empty())
                : st.is_pivot;
            if (do_fold) {
                const Eigen::VectorXd piv_x = st.sm->getStateVector();
                std::vector<int> pin_col;       // A_r column of each pin
                std::vector<double> pin_y;       // innovation = target - current absolute value
                std::vector<double> pin_w;       // row information weight: 1e10 hard pins, 1/σ² RPB priors
                // Chain-pin audit (SPEC_synthetic_minimum_network §14 T1): each fold lambda
                // reports its outcome so the eq34 call sites below can tally the
                // silent early-returns per (constellation,signal) — the number
                // standing invariant #4 requires.  0 = folded, 1 = state index
                // absent, 2 = no live phase row (h2local miss).  The legacy path
                // ignores the return value (unchanged behavior).
                enum : int { kPinFolded = 0, kPinMissState = 1, kPinMissRow = 2 };
                auto foldAmb = [&](const std::string& amb_key, double target) -> int {
                    const int amb_idx = st.sm->getAmbiguityIndex(amb_key);
                    if (amb_idx < 0) return kPinMissState;   // no usable phase row for it this epoch
                    const auto hit = h2local.find(amb_idx);
                    if (hit == h2local.end()) return kPinMissRow;  // column dropped (all-zero H)
                    // SEED-ROUND fold-deferred target (docs/SPEC_eq34_seed_round.md
                    // §3.3): the election could not read a state that did not exist
                    // yet; by now the pre-pass has seeded it, so round HERE and
                    // report the realized target back for next epoch's pinmove
                    // baseline (§3.4).  |y| ≤ λ/2 by construction of std::round.
                    // Map EMPTY unless the knob is armed — zero cost otherwise.
                    if (!st.pinned_amb_seed_lambda.empty()) {
                        const auto sl = st.pinned_amb_seed_lambda.find(amb_key);
                        if (sl != st.pinned_amb_seed_lambda.end()) {
                            const double lam = sl->second;
                            target = lam * std::round(piv_x(amb_idx) / lam);
                            R.pin_seed_targets.emplace_back(
                                amb_key, target, target - piv_x(amb_idx));
                        }
                    }
                    pin_col.push_back(hit->second);
                    // Preserve the exact legacy expression (−piv_x) when pinning
                    // to 0 — "0.0 − x" flips the sign of a zero x, which would
                    // perturb the OFF golden hash; a nonzero eq34 re-pin target
                    // uses the general form.
                    pin_y.push_back(target == 0.0 ? -piv_x(amb_idx)
                                                  : target - piv_x(amb_idx));
                    // eq34 reattach hygiene INC-3: a pin the election could not avoid
                    // placing on a FRESH host is folded with an inflated sigma (weight
                    // /= scale²), decaying to the hard pin as the arc matures — the
                    // gauge is still fixed, but the data can pull the code-seeded
                    // ambiguity instead of the whole component re-levelling onto it.
                    // The map is EMPTY unless the ramp is armed, so the lookup misses
                    // and the weight is the exact legacy 1e10 (byte-identical).
                    double w = 1e10;         // hard pin (exact gauge)
                    if (!st.pinned_amb_sigma_scale.empty()) {
                        const auto sc = st.pinned_amb_sigma_scale.find(amb_key);
                        if (sc != st.pinned_amb_sigma_scale.end() && sc->second > 1.0)
                            w /= (sc->second * sc->second);
                    }
                    pin_w.push_back(w);
                    return kPinFolded;
                };
                // eq34: r1's RPB pin — δ_{r1,σ}=0 on the RPB column in this
                // station's OWN block (the receiver half of the chain gauge).
                auto foldRpb = [&](const std::string& rpb_key) -> int {
                    const int rpb_idx = st.sm->getReceiverPhaseBiasIndex(rpb_key);
                    if (rpb_idx < 0) return kPinMissState;
                    const auto hit = h2local.find(rpb_idx);
                    if (hit == h2local.end()) return kPinMissRow;  // no phase row on σ this epoch
                    pin_col.push_back(hit->second);
                    pin_y.push_back(-piv_x(rpb_idx));
                    pin_w.push_back(1e10);   // hard pin (exact gauge)
                    return kPinFolded;
                };
                if (phase_datum_prioritized) {
                    // Audit group key = the election's dr_key, "<cons>|<signal>".
                    // The RPB key carries the system NAME ("GPS_RPB_L2W"), so map
                    // it back to the constellation char the election grouped by.
                    auto consOfSystem = [](const std::string& sys) -> char {
                        if (sys == "GPS")     return 'G';
                        if (sys == "Galileo") return 'E';
                        if (sys == "BeiDou")  return 'C';
                        if (sys == "QZSS")    return 'J';
                        if (sys == "GLONASS") return 'R';
                        return '?';
                    };
                    for (const auto& [amb_key, target] : st.pinned_amb_targets) {
                        const int out = foldAmb(amb_key, target);
                        if (phase_datum_eq34)
                            if (const auto p = StateKeys::parseAmbiguityKey(amb_key))
                                ++R.pin_audit[std::string(1, p->satellite.front())
                                              + "|" + p->signal]
                                             [static_cast<std::size_t>(out)];
                    }
                    for (const auto& rpb_key : st.pinned_rpb_keys) {
                        const int out = foldRpb(rpb_key);
                        if (phase_datum_eq34)
                            if (const auto p =
                                    StateKeys::parseReceiverPhaseBiasKey(rpb_key))
                                ++R.pin_audit[std::string(1, consOfSystem(p->system))
                                              + "|" + p->signal]
                                             [static_cast<std::size_t>(out)];
                    }
                } else {
                    for (const auto& pk : shared_phase_keys) {
                        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                        if (!parts) continue;
                        foldAmb(parts->satellite + "_" + parts->signal, 0.0);
                    }
                }
                const int n_hard = static_cast<int>(pin_col.size());
                // eq34: loose zero-mean prior (w = 1/σ², default σ = 1 m; 0
                // disables) on every UNPINNED RPB column with a live phase row.
                // The tree pins fix the GAUGE exactly, but the estimable
                // RPB↔ambiguity↔iono separation INSIDE a block can be nearly
                // rank-deficient (few sats / one signal): without this floor
                // the W5 gate withholds the epoch, or — under
                // station_pd_pin_aware — the near-singular block stays in and
                // degrades the shared solve (Jul-10 field A/B: datum-invariant
                // DD step 0.085 → 36–118 cyc/ep).  NOT a second datum: the
                // gauge is already fixed; the pull toward 0 is ≪ one phase
                // row.  GLONASS local-anchor precedent.  The datum receiver's
                // pinned RPB is skipped (already hard-pinned to 0).
                if (phase_datum_eq34
                    && (eq34_rpb_prior_w > 0.0 || eq34_rpb_q > 0.0)) {
                    for (const auto& [rpb_key, rpb_idx] :
                             st.sm->getReceiverPhaseBiasStates()) {
                        if (st.pinned_rpb_keys.count(rpb_key)) continue;
                        const auto hit = h2local.find(static_cast<int>(rpb_idx));
                        if (hit == h2local.end()) continue;  // no phase row on σ
                        const double x0 =
                            piv_x(static_cast<Eigen::Index>(rpb_idx));
                        if (eq34_rpb_prior_w > 0.0) {   // rank floor (zero-mean)
                            pin_col.push_back(hit->second);
                            pin_y.push_back(-x0);
                            pin_w.push_back(eq34_rpb_prior_w);
                        }
                        if (eq34_rpb_q > 0.0) {
                            // Local δ_{r,σ} time-linking (D6 v2, local form):
                            // prior toward the carried previous estimate with
                            // variance var + q·gap — the continuity the fresh
                            // per-epoch re-estimate lacks.  The carry map is
                            // read-only here (parallel build); stash happens
                            // post-solve, erase at the election dirty walk.
                            auto rc = rpb_carry.find(
                                {static_cast<int>(i), rpb_key});
                            if (rc != rpb_carry.end()) {
                                const double gap =
                                    pl_now_s - rc->second.last_seen_s;
                                if (gap >= 0.0 && gap <= pl_max_gap_s) {
                                    const double v = rc->second.var
                                                   + eq34_rpb_q * gap;
                                    if (v > 0.0 && std::isfinite(v)) {
                                        pin_col.push_back(hit->second);
                                        pin_y.push_back(
                                            rc->second.value_abs - x0);
                                        pin_w.push_back(1.0 / v);
                                    }
                                }
                            }
                            R.pl_rpb.emplace_back(
                                rpb_key,
                                static_cast<Eigen::Index>(hit->second), x0);
                        }
                    }
                }
                const int np = static_cast<int>(pin_col.size());
                if (np > 0) {
                    const Eigen::Index m0 = R.A_s.rows();
                    R.A_s.conservativeResize(m0 + np, Eigen::NoChange);
                    R.A_s.bottomRows(np).setZero();
                    R.A_r.conservativeResize(m0 + np, Eigen::NoChange);
                    R.A_r.bottomRows(np).setZero();
                    R.w.conservativeResize(m0 + np);
                    R.y.conservativeResize(m0 + np);
                    for (int k = 0; k < np; ++k) {
                        R.A_r(m0 + k, pin_col[k]) = 1.0;
                        R.w(m0 + k) = pin_w[k];   // 1e10 pins / 1/σ² RPB priors
                        R.y(m0 + k) = pin_y[k];
                    }
                    // HARD pins only — pd_pin_aware and the total_pins ledger
                    // must not count the soft RPB prior rows as gauge pins.
                    R.np = n_hard;
                }
            }

            // GLONASS local-nuisance anchors (code-only network scope).
            // The per-epoch ABSOLUTE network solve carries no Kalman prior on the
            // local nuisances, and GLONASS — unlike GPS/Galileo — has a per-FCN-
            // slot receiver IFB on EVERY band, so there is no IFB-free reference-
            // band code to anchor the slant iono.  Two unobservable families make
            // the local block N_rr non-positive-definite (then the kernel drops
            // the WHOLE station, losing its good GPS/Galileo rows too):
            //   (1) each GLONASS satellite's slant iono I_s — fully confounded
            //       with that satellite's IFB(band,k) + ambiguity (the code rows
            //       determine only the SUM IFB + alpha*I, leaving I_s free);
            //   (2) the {IFB(k)} <-> GLONASS-ISB common mode — unanchored unless a
            //       central-channel k=0 satellite is up (GLONASS has only ~2-3).
            // Append one LOOSE zero-mean prior row per GLONASS per-slot IFB column
            // AND per GLONASS per-satellite iono column to regularize exactly
            // those directions.  PRODUCT-SAFE: the emitted satellite clock depends
            // only on the data-determined IFB+alpha*I sum (and the cross-station
            // consensus), never on how this unobservable split is resolved, so
            // anchoring the split toward zero does not perturb the shared clock.
            // Weight 1.0 (sigma 1 m) sits well above the kernel's relative PD floor
            // (1e-12 * d_max, d_max ~ the 1e10 pivot ambiguity pins), so even the
            // pivot block stays PD.  Built into the station's OWN block (no shared
            // write); inert for GPS/Galileo-only stations and when the flag is off.
            if (config_.enableGlonassNetwork()) {
                std::vector<int> anchor_cols;
                for (const auto& [key, sidx] : st.sm->getCodeBiasStates()) {
                    if (!StateKeys::isGlonassPerSlotIfbKey(key)) continue;
                    const auto hit = h2local.find(static_cast<int>(sidx));
                    if (hit != h2local.end()) anchor_cols.push_back(hit->second);
                }
                for (const auto& [sat, sidx] : st.sm->getIonosphereStates()) {
                    if (sat.empty() || sat.front() != 'R') continue;  // GLONASS iono only
                    const auto hit = h2local.find(static_cast<int>(sidx));
                    if (hit != h2local.end()) anchor_cols.push_back(hit->second);
                }
                const int ni = static_cast<int>(anchor_cols.size());
                if (ni > 0) {
                    constexpr double kGloAnchorWeight = 1.0;  // 1/(1 m)^2
                    const Eigen::Index m0 = R.A_s.rows();
                    R.A_s.conservativeResize(m0 + ni, Eigen::NoChange);
                    R.A_s.bottomRows(ni).setZero();
                    R.A_r.conservativeResize(m0 + ni, Eigen::NoChange);
                    R.A_r.bottomRows(ni).setZero();
                    R.w.conservativeResize(m0 + ni);
                    R.y.conservativeResize(m0 + ni);
                    for (int k = 0; k < ni; ++k) {
                        R.A_r(m0 + k, anchor_cols[k]) = 1.0;
                        R.w(m0 + k) = kGloAnchorWeight;
                        R.y(m0 + k) = 0.0;  // toward 0 (absolute solve, lin pt 0)
                    }
                }
            }

            // Ionosphere constraint (W6): the between-satellite structure that
            // makes the satellite DCB observable against the free slant iono.
            // Co-estimated SH-VTEC field (network's own iono) when enabled, else
            // the external GIM.  Folded into the station's OWN block (+ the shared
            // SH border); inert when neither is on.
            if (config_.estimateNetworkIonoSh())
                networkAppendShIonoRows_(st, est, modeled[i], h2local, se.time,
                                         sh_model, setup.sh_model2, iono_deweight,
                                         shc, sh_dev_on,
                                         sh_dev_carry, ctx.carry.dw_row_held,
                                         // INC-3b: inert until the seam's one-shot
                                         // start/resume seed has run, or the very first
                                         // epoch would withhold EVERY tie row.
                                         (ctx.knobs.dw_admit_validate_k > 0.0
                                          && ctx.carry.dw_tie_seeded)
                                             ? &ctx.carry.dw_tie_seen : nullptr,
                                         R.A_s, R.A_r, R.w, R.y);
            else if (config_.estimateNetworkSatCodeBias())
                networkAppendGimIonoRows_(st, modeled[i], h2local, se.time,
                                          R.A_s, R.A_r, R.w, R.y);

            // --- SD PRE-REDUCE EWL constraint (docs/SPEC_sd_ewl_prereduce.md) --------
            // Appended AFTER the iono rows and BEFORE the block reaches
            // addStationObservations, so it folds into N_rr/N_sr/N_ss pre-reduce and
            // composes EXACTLY with the post-reduce Card-4 DD fold.  Default OFF =>
            // returns immediately => byte-identical.
            if (config_.getNetworkConfig().sd_ewl.enable)
                networkAppendSdEwlRows_(st, est, modeled[i], h2local, se.time,
                                        R.A_s, R.A_r, R.w, R.y, R.sd_ewl_trace,
                                        &ctx.carry.sd_ewl_fix, i);

            // --- Phase-leveling carry: record present ambiguities + build the
            //     diagonal carried prior (Increment 1) ----------------------
            // For every present NON-pivot ambiguity: record it for the
            // post-solve stash, and — if a converged carry exists and is not
            // stale — build its diagonal prior (Λ=1/var_eff, η=Λ·(value_abs−
            // x_lin)) to inject into this station's local block AFTER addStation,
            // BEFORE reduce.  Pivot ambiguities are SKIPPED: their 1e10 pins
            // define the phase-bias gauge exactly, so a carried diagonal there is
            // redundant.  Pure per-station compute + read-only local_carry lookup
            // — no shared write, safe in the parallel produce phase.
            // rx-CB carry (SPEC_rx_cb_carry): the legacy predicate is hoisted so the
            // receiver code-bias carry can enter this block WITHOUT the carry_warm
            // (phase-leveling) gate — the legacy interior below keeps its exact
            // original condition, so rx_cb_on=false is byte-identical.
            // zwd_seam_live joins the pivot-inclusion OR for the same reason zwd_on
            // is there: B9 keeps the ZWD carry out of any pivot/anchor exclusion,
            // and the seam window is the same fold on the same column.
            const bool legacy_local_inject =
                carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on
                               || zwd_seam_live || iono_on);
            // --- RAIM POOL RE-HOST (docs/SPEC_raim_pool_rehost.md) ----------------
            // The CHECKABLE-COLUMN census.  Deliberately its own block, ABOVE and
            // OUTSIDE `legacy_local_inject` (carry_warm ⇒ needs phase_leveling) and
            // outside `do_amb_carry` (needs !carry_invariant) — those two gates are
            // exactly what leaves every post-fit QC tier with an empty pool on a
            // Card-4 lineage.  The enumeration itself is carry-agnostic: it asks
            // which ambiguity columns are PRESENT (in A_r via h2local) and UNPINNED
            // (not a gauge anchor), which is an observability question with no
            // dependence on who owns the time-link.
            //
            // It is a separate block rather than a relaxation of the two existing
            // gates so that `pl_amb` keeps its exact current contents and every
            // reader of it stays byte-identical (standing invariant #6).  The one
            // cost is a second `getStateVector()` when both are live; that is a
            // per-station vector copy already paid once below, and it is not paid at
            // all on the OFF path.
            if (raim_ci_on || pd_handover_continuous) {
                const Eigen::VectorXd x_lin_qc = st.sm->getStateVector();
                for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    if (networkIsGaugePinned_(stations, phase_datum_prioritized,
                                              i, parts->satellite, parts->signal))
                        continue;  // anchored (gauge-pinned) — its 1e10 pin defines the
                                   // gauge, so its post-fit residual is not a fault test
                    const int amb_idx = static_cast<int>(sidx);
                    if (amb_idx < 0 || amb_idx >= x_lin_qc.size()) continue;
                    const auto hit = h2local.find(amb_idx);
                    if (hit == h2local.end()) continue;  // column not in A_r this epoch
                    R.qc_amb.push_back(PlAmb{static_cast<Eigen::Index>(hit->second),
                                             parts->satellite, parts->signal,
                                             x_lin_qc(amb_idx), key});
                }
            }
            // `rx_cb_datum` joins the gate for the same reason `rx_cb_on` did: the
            // RXCB-DATUM row build lives inside this block (the pl_cb walk below),
            // and its knob is independent of BOTH the legacy carry and the rx-CB
            // carry.  Without it the datum was silently inert wherever
            // legacy_local_inject is false — i.e. phase_leveling off entirely, AND
            // the phase_leveling_warmup_epochs cold window at the head of EVERY
            // leveled run — while the loader and the run-end line both reported it
            // ARMED.  That contradicted this block's own contract at the pl_cb walk
            // ("filled whenever EITHER is armed").
            // `opin_on` joins the gate for the same reason `rx_cb_datum` did: the
            // ORACLE PIN's row build lives inside this block and its arming is
            // independent of BOTH the legacy leveling carry and the rx-CB carry.
            // Without it the pin would be silently inert wherever
            // legacy_local_inject is false (phase_leveling off, carry_invariant, and
            // the phase_leveling_warmup_epochs cold window at the head of every
            // leveled run) while the loader and the run-end line both reported it
            // ARMED — the armed-and-inert failure invariant #4 names.  Null pointer
            // OFF, so the gate is byte-identical there.
            if (legacy_local_inject || rx_cb_on || rx_cb_datum || iono_on || opin_on) {
                const Eigen::VectorXd x_lin = st.sm->getStateVector();
                std::vector<std::tuple<Eigen::Index, double, double>> inj;  // (col, Λ, η) — legacy diagonal path
                // Card RF-PD-fix: the PRE-PD half of the same channel.  Rows land here
                // ONLY under network.station_prior_before_pd, and then never in `inj` —
                // the two are folded by different calls that do not sum.  Empty OFF, so
                // the finalize below is a no-op and R.prior_* stays empty.
                std::vector<std::tuple<Eigen::Index, double, double>> inj_prior;  // (col, Λ, η) — RF-PD-fix pre-PD datum rows
                // QUEUE 8b (scope=relevel): the per-sat re-level census, filled by the
                // pl_amb walk below and consumed by the iono-carry block after it.
                // freed = any band in the latched free set; seen/carried partition the
                // sats by whether ANY present band still has a live leveling carry —
                // carry-absence is the ONLY readable T4-erase/gap-re-birth signature at
                // this site (the T4 mark is consumed by the PASS-B pre-pass, the same
                // dead-check trap the free-epoch card documents below).
                std::set<std::string> iono_freed_sats, iono_sats_seen, iono_sats_carried;
                // Card 1 (network.carry_conditional_coupling): each carried local column
                // becomes ONE pseudo-observation ROW coupling it to the FREE shared border
                // (Odijk S-conditional carry) instead of an independent diagonal prior — see
                // filter/NetworkCoupling.hpp.  The former loop-body scratch + the six [&]
                // lambdas are CouplingRowBuilder members; built fresh per station (no shared
                // write, safe in the parallel produce phase), appended after the carry loops.
                CouplingRowBuilder cb{est, shc, q_clock, q_cb, q_pb,
                                      carry_cpl_joint, carry_cpl_tu, carry_cpl_shmarg,
                                      ctx.knobs.carry_cpl_gauge, ctx.knobs.carry_cpl_reinit,
                                      tu_reinit_hist, R};
                // READMIT re-coupling edge (network.readmit_coupling_marg_ep): the open
                // marginalization windows (written SERIALLY at the stash, read-only here).
                if (ctx.knobs.readmit_marg_ep > 0 && !ctx.carry.readmit_marg.empty()) {
                    cb.readmit_marg = &ctx.carry.readmit_marg;
                    cb.loop_ep      = static_cast<long long>(ctx.counters.n_loop_epochs);
                }
                if (legacy_local_inject) {
                // Ambiguity carry.  Legacy: the whole pivot station is skipped (all
                // its ambiguities are gauge pins).  Prioritized: every station
                // carries the arcs it does NOT anchor, so the skip is PER-(sat,
                // signal) — an anchored ambiguity's 1e10 pin already defines the
                // gauge, a carried diagonal there is redundant (isGaugePinned).
                // Inject and harvest partition on the SAME predicate so a pinned
                // amb never gets a spurious carry that fights its pin.  B9 (P1-4)
                // keeps the ZWD carry below OUT of any pivot/anchor exclusion.
                // Card 4 supersedes the phase-leveling ambiguity carry as a solve input
                // (the DD carry is the ambiguity time-link); skip the inject to avoid
                // double-counting.  Its harvest still feeds the VCE monitor + sticky tree.
                const bool do_amb_carry =
                    (phase_datum_prioritized || !st.is_pivot) && !carry_invariant;
                // F1 (network.phase_leveling_los_scale, SPEC_amb_iono_split_overcount).
                // Each carried ambiguity row's variance is the per-column conditional on
                // the shared border, MARGINAL over the LOS's own free slant iono — so with
                // phase ≈ exact (N_b = c_b + (μ_b/λ_b)·I) the B rows of ONE (station,
                // satellite) LOS assert the SAME iono-direction datum B times per epoch
                // and the carry recursion runs p_k = B·p_{k−1} + p_code instead of
                // p_{k−1} + p_code (formal σ collapses geometrically, the level freezes at
                // K_eff = (B+1)/(B−1) epochs).  Multiplying each row's variance by B_eff
                // (the carried columns injected on that LOS THIS epoch) makes the B rows
                // sum to ONE unit of iono-direction precision — exact along that
                // direction; along the B−1 lane directions the prior is B× looser, which
                // the phase rows re-fix every epoch at σ_φ.  Station-common directions
                // (rx clock / ZWD / rx-DCB, shared by every LOS of the station) stay
                // over-counted ~n_sat× — named, not fixed, by F1.  Loosens only.
                //
                // Structure: the variance TAIL (var floor, then the M2 v2 VCE factor) and
                // the ROW BUILD are one lambda each, shared by the inline (OFF) route and
                // the deferred (ON) route so the two cannot drift.  OFF: the loop body is
                // the original inline sequence with the tail/build behind the lambdas —
                // the same operations in the same order, byte-identical.  ON: B_eff is a
                // property of the whole LOS, so injectable columns are collected first
                // and then scaled + built in the same visiting order.
                //
                // M2 v2 (docs/SPEC_carry_vce_apply.md §2): the LASER 5-95 variance
                // FACTOR, applied last — "the hand model evaluated as today, then
                // × α²".  Read-only O(1) array lookup, safe in this parallel produce
                // phase; the table is refreshed once per epoch by the serial step at
                // the end of SB_STASH_STATUS, so every station in this pass and the
                // serial harvest that mirrors it read ONE value.  Exactly 1.0 when the
                // apply path is off or `pl` is unarmed ⇒ byte-identical.
                const auto pl_finish_var = [&](double var_eff, const std::string& sat) {
                    if (pl_var_floor > 0.0) var_eff = std::max(var_eff, pl_var_floor);
                    var_eff *= vce_apply.factor(NetworkCarryVceApply::kPl, sat);
                    return var_eff;
                };
                // RF-1 §7.4: record what this column's fold actually was, at the one
                // site that emits a per-column row.  Called AFTER the emit decided, so
                // a column screened out by the "never poison u_r" guard is recorded as
                // NOT folded — which is the honest answer and exactly the population D2
                // exists to count.
                const auto amb_state_note = [&](const std::string& sat,
                                                const std::string& signal,
                                                double var_eff, AmbFoldRoute route) {
                    if (!amb_state_tr) return;
                    R.amb_state_fold[{sat, signal}] = {var_eff, route};
                };
                const auto pl_emit_row = [&](Eigen::Index col, int amb_idx,
                                             const LocalCarry& lc, double var_eff,
                                             double gap_clamped,
                                             const std::string& sat = {},
                                             const std::string& signal = {}) {
                    if (!(var_eff > 0.0)) return;
                    if (carry_cpl_cond) {
                        cb.buildCplRow(col, x_lin(amb_idx), lc, var_eff, gap_clamped);
                        amb_state_note(sat, signal, var_eff, AmbFoldRoute::CouplingScalar);
                        return;
                    }
                    const double lambda = 1.0 / var_eff;
                    const double eta = lambda * (lc.value_abs - x_lin(amb_idx));
                    if (!std::isfinite(eta)) return;            // belt-and-suspenders: never poison u_r
                    inj.emplace_back(col, lambda, eta);
                    amb_state_note(sat, signal, var_eff, AmbFoldRoute::Diag);
                };
                struct PlPend {   // F1 ON route: an injectable column awaiting its LOS B_eff
                    Eigen::Index col; int amb_idx; const LocalCarry* lc;
                    double var_eff;   // var + q·gap — BEFORE the tail
                    double gap;       // clamped
                    std::string sat, signal;
                    /// Young-arc inflate: V0·exp(−age/τ) for this column, 0 when the
                    /// knob is off / the arc is mature / unstamped.  Carried through the
                    /// deferral so the F3 block (congruence) and the per-column fallback
                    /// (var_eff += var_add) read ONE computed value — the two routes
                    /// cannot drift, and the counters are keyed to the compute site.
                    double var_add{0.0};
                };
                std::vector<PlPend> pl_pend;   // stays empty OFF
                // RF-1 step 4: this station's LOS elevations by satellite, for the
                // BIRTH-PRIOR FLOOR's elevation mapping.  Built ONLY when the floor is
                // armed — the pl_los_beff / el_of precedent — so an unarmed run never
                // walks the modeled set for this.
                std::map<std::string, double> amb_elev_of;
                if (amb_floor_on)
                    for (const auto& mo : modeled[i]) amb_elev_of[mo.satellite] = mo.elevation;
                if (do_amb_carry) {
                for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    if (networkIsGaugePinned_(stations, phase_datum_prioritized,
                                              i, parts->satellite, parts->signal))
                        continue;  // anchored (gauge-pinned) — never carried
                    const int amb_idx = static_cast<int>(sidx);
                    if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                    const auto hit = h2local.find(amb_idx);
                    if (hit == h2local.end()) continue;  // ambiguity column not in A_r this epoch
                    const Eigen::Index col = static_cast<Eigen::Index>(hit->second);
                    R.pl_amb.push_back(PlAmb{col, parts->satellite, parts->signal,
                                             x_lin(amb_idx), key});
                    // RF-1 step 4: the LOS elevation the harvest maps the BIRTH-PRIOR
                    // FLOOR with.  Built only when the floor is armed — off it, the map
                    // is never walked and PlAmb keeps its -1 sentinel (⇒ zenith).
                    if (amb_floor_on) {
                        const auto ev = amb_elev_of.find(parts->satellite);
                        if (ev != amb_elev_of.end())
                            R.pl_amb.back().elev_deg = ev->second;
                    }
                    if (iono_on) iono_sats_seen.insert(parts->satellite);

                    // A pending re-seed marks the prior arc value as no longer
                    // trustworthy (the network's analogue of a slip): re-float from
                    // data this epoch rather than clamp to the stale carry.  The
                    // stash below then captures the fresh value.  (Slip-driven
                    // REFLOAT/DROP proper is Increment 2+; this is the cheap guard
                    // for the only reset trigger the network can raise today.)
                    // ⚠ SPEC_slip_relevel_walk: for DETECTOR marks this check is
                    // structurally DEAD — the PASS-B measurement pre-pass
                    // (ensureRequiredStates) consumes+clears the mark before this
                    // line runs, so the stale pre-slip level injected at converged
                    // weight for every detector-caught slip (the measured level-walk
                    // root).  The latched free set below is the same predicate on
                    // state that survives to this read point; bridged bands were
                    // REMOVED from it (their carry is already shifted to the
                    // post-slip level and SHOULD inject).
                    if (st.sm->isAmbiguityPendingReseed(key)) continue;
                    if (relevel_free_on
                        && ctx.carry.relevel.free_ambs.count(
                               {static_cast<int>(i), key})) {
                        ++R.relevel_free_skips;
                        if (iono_on) iono_freed_sats.insert(parts->satellite);
                        // RF-1 step 4 (§3.4): under the float_kf vehicle this arc's prior
                        // was already DROPPED to its birth variance by the serial
                        // pre-assembly pass, so it is NOT skipped — it folds, at birth
                        // strength.  That is the whole change: today's free epoch removes
                        // ALL information about the column and opens an unbounded
                        // direction the shared parameters re-balance into (R10's 88 % of
                        // unsignalled clock steps); a bounded drop removes it down to a
                        // STATED level.  The counter above still names the population, so
                        // the two vehicles' response loads stay comparable.
                        if (!amb_float_kf) continue;
                    }
                    if (!std::isfinite(x_lin(amb_idx))) continue;  // non-finite state → don't inject

                    const auto cit = local_carry.find(
                        std::make_tuple(i, parts->satellite, parts->signal));
                    if (cit == local_carry.end()) continue;       // no carry yet (new arc)
                    const double gap = pl_now_s - cit->second.last_seen_s;
                    if (gap > pl_max_gap_s) {
                        // SPEC_withhold_era_release §8: THE CLIFF.  The entry is
                        // not erased here — it is simply not injected, so this
                        // arc solves free this epoch and the harvest re-stashes
                        // it from one epoch of code.  A whole fleet crossing on
                        // one epoch is the §1.3 mass re-level; the post-harvest
                        // prune cannot see it, because the harvest refreshes
                        // last_seen_s first.  Per-station (parallel produce);
                        // folded serially below.
                        ++R.inject_stale_skips;
                        // RF-1 step 4 (§7.2): the same change as the free-epoch latch —
                        // the serial pre-assembly pass has already dropped this arc to
                        // its birth variance, so the cliff folds at birth strength
                        // instead of dropping the column's memory entirely for an epoch.
                        // The post-harvest PRUNE still ERASES the arcs that are genuinely
                        // gone, which is §3.4's one surviving erase case.
                        if (!amb_float_kf) continue;
                    }
                    if (iono_on) iono_sats_carried.insert(parts->satellite);
                    // var_eff = var + q·gap, floored by the optional lock-in guard
                    // (var_floor>0 caps Λ so a slowly-biasing ambiguity can never
                    // become a rigid prior the data cannot override).  Only present
                    // (data-bearing) columns are injected, so a tiny Λ never leaves a
                    // column PD-held by the prior alone — the §3.4 floor is not
                    // load-bearing here; this guards the OPPOSITE (lock-in) direction.
                    double var_eff = cit->second.var + pl_q_amb * std::max(0.0, gap);
                    // AGE-AWARE young-arc inflate: what this column's LEVEL is worth.  A
                    // freshly re-levelled arc holds a CODE-quality value at the
                    // PHASE-stiff variance the conditional hands back, because that
                    // conditional is taken over the station's MATURE arcs — the T4 tier
                    // then reads the resulting geometry inconsistency as a slip and
                    // erases it (21 erases/station-hour), and the erase–re-level
                    // treadmill sustains the ZWD/clock plateau.  Computed ONCE here and
                    // consumed by whichever route folds the column; counted here too, so
                    // the counter names the population the card acted on.
                    double yng_add = 0.0;
                    if (yng_on) {
                        const auto ya = NetworkYoungInflate::varAdd(
                            yng, cit->second.relevel_s, pl_now_s, yng_dt_ep);
                        if (ya.inflated) {
                            yng_add = ya.var_add;
                            ++R.yng_cols;
                            R.yng_var_add_sum += ya.var_add;
                            R.yng_age_sum     += ya.age_ep;
                        } else if (ya.over_age) {
                            ++R.yng_skip_age;
                        }
                    }
                    if (pl_los_scale || pl_blk_on) {
                        // F1 / F3: defer — membership uses the SAME positivity predicate the
                        // inline route applies to the (unscaled) tail; ×B_eff, a finite
                        // integer ≥ 1, cannot flip it, so the injected set is identical.
                        // (F3 reads the same finished tail as its Σ diagonal, and its
                        // scalar screen is the row build's own.)
                        if (!(pl_finish_var(var_eff, parts->satellite) > 0.0)) continue;
                        pl_pend.push_back(PlPend{col, amb_idx, &cit->second, var_eff,
                                                 std::max(0.0, gap), parts->satellite,
                                                 parts->signal, yng_add});
                        continue;
                    }
                    var_eff = pl_finish_var(var_eff, parts->satellite);
                    // DIAGONAL route: the inflation is a plain addition, applied AFTER
                    // the tail so it is symmetric with the block route (whose Sigma
                    // diagonal IS the finished tail).  yng_add is 0 OFF => byte-identical.
                    var_eff += yng_add;
                    pl_emit_row(col, amb_idx, cit->second, var_eff, std::max(0.0, gap),
                                parts->satellite, parts->signal);
                }
                // --- I-in-block (network.carry_iono_in_block): collect this station's
                //     carried per-LOS slant-iono columns for the SAME block the
                //     ambiguities are about to build.  Walked HERE, before the block
                //     build, because the harvest reads R.pl_iono to decide which columns
                //     joined `cols` — and R.pl_iono is recorded for EVERY present LOS,
                //     folded or not, so the carry keeps tracking the solution through the
                //     epochs it cannot act on (the pl_zwd_present pattern).
                //
                //     Why the per-LOS iono belongs in the block and not on a diagonal row:
                //     the station's B carried band priors already assert this LOS's iono
                //     direction implicitly (one per band), so an INDEPENDENT diagonal iono
                //     prior asserts it a further time — the B-fold over-count of
                //     SPEC_amb_iono_split_overcount §7 item 17, and the structural reason
                //     network.carry_iono was falsified ×4 in QUEUE 8(b).  Inside the block
                //     the harvested (N_rr⁻¹) cross terms subtract that overlap exactly.
                //     Consequently: NO inject-side var floor (the floor bounded how badly
                //     a double-counted prior could out-stiffen the code channel; exactness
                //     replaces it), NO F1 los_scale (F1 repairs the same over-count the
                //     block already handles — applying it here would UNDER-weight), and NO
                //     F3 young-column age gate (a first sighting already carries nothing).
                std::vector<LocalBlockCol> iono_bc;
                if (iono_blk_on) {
                    // q is PER EPOCH (m² at L1), so the gap needs an epoch length.  Take it
                    // from the run itself — the current PASS-B time minus the previous
                    // harvest stamp IS this epoch's spacing — so the knob means the same
                    // thing at 1 s and 30 s cadence and no nominal-rate config is invented.
                    // Floored at one epoch: a same-epoch re-fold must still pay one q.
                    double dt_ep = pl_now_s - ctx.carry.pl_prev_now_s;
                    if (!std::isfinite(dt_ep) || dt_ep <= 0.0 || dt_ep > pl_max_gap_s)
                        dt_ep = 0.0;   // first epoch / implausible spacing ⇒ gap_ep = 1
                    for (const auto& [isat, iidx_sz] : st.sm->getIonosphereStates()) {
                        const int iidx = static_cast<int>(iidx_sz);
                        if (iidx < 0 || iidx >= x_lin.size()) continue;
                        if (!std::isfinite(x_lin(iidx))) continue;
                        const auto ihit = h2local.find(iidx);
                        if (ihit == h2local.end()) continue;  // iono column not in A_r this epoch
                        const Eigen::Index icol = static_cast<Eigen::Index>(ihit->second);
                        R.pl_iono.emplace_back(isat, icol, x_lin(iidx));
                        const auto iit = iono_blk_carry.find({static_cast<int>(i), isat});
                        if (iit == iono_blk_carry.end()) continue;  // first sighting / post-erase — COLD by design
                        const double igap = pl_now_s - iit->second.last_seen_s;
                        if (igap > iono_blk_gap_s) {
                            // Invariant #1's absent-satellite edge: a stale LOS is refused,
                            // never aged into a fold.  The harvest prune erases it.
                            ++R.pl_blk_iono_gap_drop;
                            continue;
                        }
                        const double gap_ep =
                            (dt_ep > 0.0) ? std::max(1.0, igap / dt_ep) : 1.0;
                        const double ivar_eff = iit->second.var + iono_blk_q * gap_ep;
                        if (!(ivar_eff > 0.0) || !std::isfinite(ivar_eff)) continue;
                        iono_bc.push_back(LocalBlockCol{
                            icol, x_lin(iidx), &iit->second, ivar_eff,
                            std::max(0.0, igap), /*vce_fac=*/1.0,
                            /*is_zwd=*/false, /*is_iono=*/true});
                    }
                }
                // --- RX-CLK-in-block (network.carry_rx_clock_in_block): collect this
                //     station's carried RECEIVER-CLOCK-family columns — the primary clock
                //     and, under network.carry_rx_clock_isb, its per-system ISBs — for the
                //     SAME block.  Walked HERE, before the block build, because the
                //     harvest reads R.pl_rxclk to decide which columns joined `cols`, and
                //     R.pl_rxclk is recorded for EVERY present column, folded or not, so
                //     the carry keeps tracking the solution through epochs it cannot act
                //     on (the pl_zwd_present pattern).
                //
                //     WHY THE CLOCK BELONGS IN THE BLOCK.  The receiver clock is the most
                //     station-COMMON direction there is: every carried ambiguity row of
                //     the station already asserts it implicitly, so an independent
                //     diagonal prior would assert it a further n_LOS times — the over-count
                //     of SPEC_amb_iono_split_overcount §7 item 17 that falsified the
                //     DIAGONAL ZWD carry.  Inside the block the harvested (N_rr⁻¹) cross
                //     terms subtract the overlap exactly.  There is deliberately no
                //     diagonal fallback row.
                //
                //     ⚠ WHY THE MEAN IS A PREDICTION, NOT THE LAST VALUE.  The producer
                //     never writes its solution back into the station StateManager, so
                //     x_lin(clock) is a RUN-CONSTANT and x_r carries the whole clock —
                //     including a ~0.3 m/s drift, i.e. 9 m per 30 s epoch (measured
                //     0.29994 m/s on the sim truth bed, against 0.299 m of RW jitter).  A
                //     bias-only carry would inject a ~30 σ wrong mean every epoch.  The
                //     carried pair is therefore (bias, drift): the injected mean is
                //     value_abs + rate·gap and the injected variance carries the drift's
                //     own uncertainty over the gap, rate_var·gap².  Until the harvest has
                //     folded one increment, rate_var is the deliberately huge
                //     carry_rx_clock_rate_var0 (σ 1 m/s ⇒ σ_prior ≈ 30 m at 30 s), so a
                //     station whose drift is unknown injects a near-FREE prior rather than
                //     a stiff wrong one.  An ISB has no drift by construction (a
                //     near-constant bias; the sim injects a fixed 8.000 m) and takes a
                //     pure random walk.
                //
                //     Asymmetries against an ambiguity column, same as the ZWD's: NO F3
                //     young-column age gate (a clock is not an arc and has no code-level
                //     birth error to quarantine — its uninformative first prior is the
                //     quarantine), NO pl_finish_var tail (the var floor and the M2 VCE
                //     factor are keyed to the phase-leveling ambiguity family, α² being a
                //     per-SATELLITE lookup), vce_fac 1.0 so the cross terms are unscaled.
                std::vector<LocalBlockCol> rxclk_bc;
                // The drift-predicted COPIES the pushed columns point at.  A copy rather
                // than a mutation of the carry map because PASS B's contract is
                // READ-ONLY on the carry (it runs inside the parallel station produce);
                // the copy keeps blk / blk_idx / coupling, so pairing by harvest-block
                // pointer is unaffected.  ⚠ RESERVED to its maximum size before the first
                // push: a reallocation here would dangle every LocalBlockCol::lc already
                // in rxclk_bc.
                std::vector<LocalCarry> rxclk_pred;
                if (rxclk_blk_on) {
                    // q is PER EPOCH, so the gap needs an epoch length; take it from the
                    // run itself exactly as the iono card does.
                    double dt_ep = pl_now_s - ctx.carry.pl_prev_now_s;
                    if (!std::isfinite(dt_ep) || dt_ep <= 0.0 || dt_ep > pl_max_gap_s)
                        dt_ep = 0.0;   // first epoch / implausible spacing ⇒ gap_ep = 1
                    struct RxCand { std::string kind; int sidx; };
                    std::vector<RxCand> rx_cand;
                    const size_t clk_sz = st.sm->getClockIndex();
                    if (clk_sz != std::numeric_limits<size_t>::max())
                        rx_cand.push_back(RxCand{std::string{}, static_cast<int>(clk_sz)});
                    if (rxclk_isb)
                        for (const auto& [sys, sidx] : st.sm->getSystemTimeOffsetStates())
                            if (sidx != std::numeric_limits<size_t>::max())
                                rx_cand.push_back(RxCand{sys, static_cast<int>(sidx)});
                    rxclk_pred.reserve(rx_cand.size());
                    for (const RxCand& rc : rx_cand) {
                        if (rc.sidx < 0 || rc.sidx >= x_lin.size()) continue;
                        if (!std::isfinite(x_lin(rc.sidx))) continue;
                        const auto rhit = h2local.find(rc.sidx);
                        if (rhit == h2local.end()) continue;  // column not in A_r this epoch
                        const Eigen::Index rcol =
                            static_cast<Eigen::Index>(rhit->second);
                        R.pl_rxclk.push_back(PlRxClk{rcol, x_lin(rc.sidx), rc.kind});
                        const auto rkey = std::make_pair(static_cast<int>(i), rc.kind);
                        const auto rit = rx_clk_carry.find(rkey);
                        if (rit == rx_clk_carry.end()) continue;  // first sighting / post-jump — COLD by design
                        const double rgap = pl_now_s - rit->second.last_seen_s;
                        if (rgap > rxclk_gap_s) {
                            // Invariant #1's absent-station edge: a stale carry is refused,
                            // never aged into a fold.  The harvest prune erases it.
                            ++R.pl_blk_rxclk_gap_drop;
                            continue;
                        }
                        const double gap_ep =
                            (dt_ep > 0.0) ? std::max(1.0, rgap / dt_ep) : 1.0;
                        double rate = 0.0, rate_var = 0.0;
                        if (rc.kind.empty()) {   // the PRIMARY clock: the drift half
                            const auto rtit = rx_clk_rate.find(rkey);
                            rate_var = rxclk_rate_var0;
                            if (rtit != rx_clk_rate.end()) {
                                rate = rtit->second.rate;
                                if (rtit->second.rate_var >= 0.0)
                                    rate_var = rtit->second.rate_var;
                            }
                        }
                        // The SHARED (bias, drift) kernel — the harvest's jump gate
                        // forms its innovation against this very prediction, so the two
                        // edges can never drift apart (filter/NetworkRunTypes.hpp).
                        const RxClkPred rp = rxClockPredict(
                            rit->second.value_abs, rit->second.var, rate, rate_var,
                            std::max(0.0, rgap), gap_ep, rxclk_q);
                        const double mean_pred = rp.mean;
                        const double rvar_eff  = rp.var;
                        if (!(rvar_eff > 0.0) || !std::isfinite(rvar_eff)
                            || !std::isfinite(mean_pred))
                            continue;
                        rxclk_pred.push_back(rit->second);        // COPY of the carry …
                        rxclk_pred.back().value_abs = mean_pred;  // … with the PREDICTED mean
                        rxclk_bc.push_back(LocalBlockCol{
                            rcol, x_lin(rc.sidx), &rxclk_pred.back(), rvar_eff,
                            std::max(0.0, rgap), /*vce_fac=*/1.0, /*is_zwd=*/false,
                            /*is_iono=*/false, /*var_add=*/0.0, /*is_rx_clk=*/true,
                            /*is_rx_isb=*/!rc.kind.empty()});
                    }
                }
                // OFF ⇒ iono_bc and rxclk_bc are always empty, so this predicate is
                // exactly the pre-card `if (!pl_pend.empty())` (byte-identical).  ON it
                // also admits a station whose ambiguity set is entirely young/stale/absent
                // but whose iono or receiver-clock carry is live: those columns still
                // whiten jointly among themselves, which is the correct prior, and the
                // pl_pend-driven code below all no-ops on an empty pl_pend.
                if (!pl_pend.empty() || !iono_bc.empty() || !rxclk_bc.empty()) {
                    bool blk_done = false;
                    if (pl_blk_on) {
                        // F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_
                        // overcount): the station's ADMITTED columns as ONE jointly-
                        // whitened block.  The tail (var floor, VCE factor) is applied per
                        // column exactly as the per-row routes apply it — on the DIAGONAL
                        // only — and the builder reads the harvested conditional cross
                        // covariances for the off-diagonals (see buildLocalBlock).  Every
                        // carried entry of this station that is NOT in pl_pend (departed /
                        // stale / freed / reseed / non-finite x_lin) is thereby
                        // MARGINALIZED out of the block — invariant #7's decoupling edge,
                        // counted here, never conditioned on.  A non-SPD factor falls
                        // through to the per-column route below (F1-scaled if armed) and
                        // is counted; F1's pl_los_beff mirror stays empty on a whitened
                        // block, which is how the harvest mirror knows nothing was scaled.
                        // F3 block ADMISSION age gate (network.phase_leveling_local_block_
                        // min_age_ep, SPEC_amb_iono §6.12): a leg younger than the gate
                        // (loop epochs since its first stash) stays OUT of the block — it
                        // carries its code-level birth error, and the joint whitening would
                        // spread that into the station's mature columns — and is emitted
                        // below as a per-column row instead (F1-scaled over the young set
                        // when los_scale is armed).  0 = OFF: every pending column enters.
                        const int  pl_min_age = ctx.knobs.pl_blk_min_age_ep;
                        const long long loop_ep_now =
                            static_cast<long long>(ctx.counters.n_loop_epochs);
                        std::vector<PlPend> pl_young;
                        std::vector<LocalBlockCol> bc;
                        bc.reserve(pl_pend.size());
                        for (const PlPend& p : pl_pend) {
                            // localCarryYoung (filter/NetworkRunTypes.hpp) is the ONE
                            // definition of the maturity test, shared with the cold
                            // anchor-fallback candidacy bar — including the
                            // RESTORED-ARC AGE convention (SPEC_event_harness §15.4):
                            // a non-positive birth stamp is never young.
                            if (localCarryYoung(*p.lc, loop_ep_now, pl_min_age)) {
                                // "free" (default): the young leg is NOT injected at all —
                                // it re-solves from this epoch's data and carries no memory
                                // until it matures.  "diagonal": deferred to the per-column
                                // route below (measured harmful, kept for the record).
                                if (!ctx.knobs.pl_blk_young_free) pl_young.push_back(p);
                                else ++R.pl_blk_young_cols;
                                continue;
                            }
                            bc.push_back(LocalBlockCol{
                                p.col, x_lin(p.amb_idx), p.lc,
                                pl_finish_var(p.var_eff, p.sat), p.gap,
                                vce_apply.factor(NetworkCarryVceApply::kPl, p.sat),
                                /*is_zwd=*/false, /*is_iono=*/false,
                                /*var_add=*/p.var_add});
                        }
                        R.pl_blk_young_cols += static_cast<long long>(pl_young.size());
                        // "free" young legs must not fall through to the per-column fallback
                        // either: drop them from pl_pend so a factorization failure re-emits
                        // only the mature set (and the marginalized count below reads them as
                        // kept out of the block, which they are).
                        if (ctx.knobs.pl_blk_young_free && pl_min_age > 0
                            && bc.size() != pl_pend.size()) {
                            std::vector<PlPend> mature;
                            mature.reserve(bc.size());
                            for (const PlPend& p : pl_pend)
                                if (!localCarryYoung(*p.lc, loop_ep_now, pl_min_age))
                                    mature.push_back(p);
                            pl_pend.swap(mature);
                        }
                        long long n_sta_carry = 0;   // read-only walk of this station's entries
                        for (auto it = local_carry.lower_bound(std::make_tuple(
                                 static_cast<int>(i), std::string{}, std::string{}));
                             it != local_carry.end()
                                 && std::get<0>(it->first) == static_cast<int>(i);
                             ++it)
                            ++n_sta_carry;
                        R.pl_blk_marg_cols += std::max<long long>(
                            0, n_sta_carry - static_cast<long long>(pl_pend.size()));
                        // B9-in-block (network.carry_zwd_in_block): the station's carried
                        // ZWD as ONE MORE column of this same block.  Rationale: a diagonal
                        // B9 row asserts the ZWD direction OUTSIDE the block, while every
                        // ambiguity row inside it asserts that same station-COMMON direction
                        // implicitly — the ~n_LOS over-count of SPEC_amb_iono_split_overcount
                        // §7 item 17 ("block ALL carried local columns"), and the structural
                        // reason the diagonal carry was falsified on the quiet bed.  Inside
                        // the block the joint Σ carries the harvested (N_rr⁻¹) cross terms,
                        // so the ZWD's overlap with the ambiguities is subtracted exactly.
                        //
                        // The predicates are the B9 injector's own, re-read here (NOT
                        // duplicated behaviour: the diagonal route below is skipped for
                        // exactly the station-epochs this pushes).  Three deliberate
                        // asymmetries against an ambiguity column:
                        //   * NO young-column age gate — a ZWD is not an arc, it has no
                        //     code-level birth error to quarantine, and its state exists
                        //     from the station's first epoch.
                        //   * NO pl_finish_var tail — the var floor and the M2 VCE factor
                        //     are keyed to the phase-leveling ambiguity family (α² is a
                        //     per-SATELLITE lookup), so the block's ZWD diagonal is the
                        //     SAME zvar_eff the B9 row would have used; vce_fac 1.0 leaves
                        //     the cross terms unscaled.
                        //   * It is recorded (R.pl_zwd / R.pl_zwd_present) here as well as
                        //     in the B9 block below — the values are identical, and the
                        //     harvest needs the record whichever route folded.
                        bool zwd_pushed = false;
                        if (zwd_blk_on) {
                            const size_t ztidx_sz = st.sm->getTropoIndex();
                            if (ztidx_sz != std::numeric_limits<size_t>::max()) {
                                const int ztidx = static_cast<int>(ztidx_sz);
                                if (ztidx >= 0 && ztidx < x_lin.size()
                                    && std::isfinite(x_lin(ztidx))) {
                                    const auto zhit = h2local.find(ztidx);
                                    if (zhit != h2local.end()) {
                                        const Eigen::Index zcol =
                                            static_cast<Eigen::Index>(zhit->second);
                                        R.pl_zwd_present = true;   // record for the post-solve stash
                                        R.pl_zwd = PlZwd{zcol, x_lin(ztidx)};
                                        const auto zit = zwd_carry.find(static_cast<int>(i));
                                        if (zit != zwd_carry.end()) {
                                            const double zgap = pl_now_s - zit->second.last_seen_s;
                                            if (zgap <= pl_max_gap_s) {
                                                const double zvar_eff =
                                                    zit->second.var
                                                    + zwd_q_eff * std::max(0.0, zgap);
                                                if (zvar_eff > 0.0) {
                                                    bc.push_back(LocalBlockCol{
                                                        zcol, x_lin(ztidx), &zit->second,
                                                        zvar_eff, std::max(0.0, zgap),
                                                        /*vce_fac=*/1.0, /*is_zwd=*/true});
                                                    zwd_pushed = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        // I-in-block: the station's carried per-LOS iono columns as more
                        // columns of this SAME block, pushed AFTER the ZWD so the whole
                        // station — ambiguities, ZWD, iono — folds through ONE Cholesky
                        // and every local-local overlap is subtracted by the harvested
                        // cross terms rather than asserted twice.  Push order does not
                        // matter to the algebra (Σ is symmetric and the pairing is by
                        // harvest-block pointer, not by position); it is fixed only so
                        // pl_iono_blk_cols is deterministic for the RAIM census.
                        for (const LocalBlockCol& ic : iono_bc) {
                            bc.push_back(ic);
                            R.pl_iono_blk_cols.push_back(ic.col);
                        }
                        // RX-CLK-in-block: the station's receiver-clock family as the
                        // last columns of this SAME block, so the whole station —
                        // ambiguities, ZWD, iono, clock — folds through ONE Cholesky and
                        // every local-local overlap is subtracted by the harvested cross
                        // terms rather than asserted twice.  Push order does not matter to
                        // the algebra (Σ is symmetric and the pairing is by harvest-block
                        // pointer, not by position); it is fixed only so
                        // pl_rxclk_blk_cols is deterministic for the RAIM census.
                        for (const LocalBlockCol& rc : rxclk_bc) {
                            bc.push_back(rc);
                            R.pl_rxclk_blk_cols.push_back(rc.col);
                        }
                        blk_done = bc.empty() ? true : cb.buildLocalBlock(bc);
                        if (!blk_done) {
                            ++R.pl_blk_fallback;
                            // RF-1 step 3, design risk #3: record the failed block's
                            // DIMENSION so the serial scatter can WARN with identities.
                            // Kept here (produce-phase local) rather than logged here:
                            // this loop runs in parallel and a log from it would be
                            // thread-order noise.
                            R.pl_blk_fallback_dim =
                                static_cast<long long>(bc.size());
                        }
                        // RF-1 §7.4: the BLOCK route's fold record.  Only on success —
                        // a fallback re-emits every pending column through pl_emit_row
                        // below, which records the route it actually took.  That
                        // distinction IS design risk #3 (a non-SPD factor silently
                        // changing the vehicle on one station-epoch), made visible per
                        // arc instead of only as a run total.
                        if (amb_state_tr && blk_done)
                            for (const PlPend& p : pl_pend)
                                if (!localCarryYoung(*p.lc, loop_ep_now, pl_min_age))
                                    amb_state_note(p.sat, p.signal,
                                                   pl_finish_var(p.var_eff, p.sat) + p.var_add,
                                                   AmbFoldRoute::Block);
                        // I-in-block degrade edge (invariant #4/#7): a non-SPD block means
                        // the iono carries did NOT act this station-epoch.  Unlike the ZWD
                        // there is deliberately no diagonal row to fall back to, so this is
                        // a true "carried nothing", not a "carried it the old way" — the
                        // counter name says so, and pl_iono_blk_cols is cleared because no
                        // whitened iono row exists for the RAIM census to recognise.
                        if (!blk_done && !iono_bc.empty()) {
                            R.pl_blk_iono_not_folded +=
                                static_cast<long long>(iono_bc.size());
                            R.pl_iono_blk_cols.clear();
                        }
                        // RX-CLK-in-block degrade edge (invariant #4/#7): identical
                        // reading — a non-SPD block means the clock carries did NOT act
                        // this station-epoch.  There is deliberately no diagonal row to
                        // fall back to, so this is a true "carried nothing", and
                        // pl_rxclk_blk_cols is cleared because no whitened clock row
                        // exists for the RAIM census to recognise.
                        if (!blk_done && !rxclk_bc.empty()) {
                            R.pl_blk_rxclk_not_folded +=
                                static_cast<long long>(rxclk_bc.size());
                            R.pl_rxclk_blk_cols.clear();
                        }
                        // B9-in-block: the ZWD folded through the block ⇒ the diagonal B9
                        // row below must NOT also fire (that would be the double count this
                        // card removes), and network_zwd.csv's `carried` flag stays truthful.
                        // On a non-SPD block the flag stays false and the diagonal row runs,
                        // counted as pl_blk_zwd_fallback_diag at that site — degrading to
                        // exactly today's B9 behaviour, never to "no ZWD memory".
                        if (zwd_pushed && blk_done) {
                            R.pl_zwd_in_block = true;
                            R.pl_zwd_injected = true;   // L7: a carry was actually folded
                        }
                        // The young legs take the per-column route whether or not the block
                        // factorized (on a fallback the loop below re-emits EVERY pending
                        // column, young ones included, so emit them here only on success).
                        if (blk_done && !pl_young.empty()) {
                            std::map<std::string, int> beff_y;
                            if (pl_los_scale) {
                                for (const PlPend& p : pl_young) ++beff_y[p.sat];
                                for (const auto& [sat, b] : beff_y)
                                    ++R.pl_los_scale_hist[std::min(b, 4) - 1];
                            }
                            for (const PlPend& p : pl_young) {
                                double var_eff = p.var_eff;
                                if (pl_los_scale) {
                                    const int b = beff_y[p.sat];
                                    if (b > 1) {
                                        var_eff *= static_cast<double>(b);
                                        ++R.pl_los_scale_rows;
                                    }
                                    R.pl_los_beff[{p.sat, p.signal}] = b;
                                }
                                var_eff = pl_finish_var(var_eff, p.sat);
                                var_eff += p.var_add;   // young-arc inflate, diagonal route
                                pl_emit_row(p.col, p.amb_idx, *p.lc, var_eff, p.gap,
                                            p.sat, p.signal);
                            }
                        }
                    }
                    if (!blk_done) {
                        // F1 (or the F3 fallback): B_eff per (station, satellite) LOS =
                        // injectable columns on it, then the tail + build in the original
                        // visiting order.  Telemetry is per-station here (parallel produce)
                        // and folded serially below.  With F1 off (F3 fallback only) this is
                        // the plain per-column route: tail + build, unscaled.
                        std::map<std::string, int> beff;
                        if (pl_los_scale) {
                            for (const PlPend& p : pl_pend) ++beff[p.sat];
                            for (const auto& [sat, b] : beff)
                                ++R.pl_los_scale_hist[std::min(b, 4) - 1];
                        }
                        for (const PlPend& p : pl_pend) {
                            double var_eff = p.var_eff;
                            if (pl_los_scale) {
                                const int b = beff[p.sat];
                                if (b > 1) {
                                    var_eff *= static_cast<double>(b);
                                    ++R.pl_los_scale_rows;
                                }
                                R.pl_los_beff[{p.sat, p.signal}] = b;
                            }
                            var_eff = pl_finish_var(var_eff, p.sat);
                            var_eff += p.var_add;   // young-arc inflate, diagonal route
                            pl_emit_row(p.col, p.amb_idx, *p.lc, var_eff, p.gap,
                                        p.sat, p.signal);
                        }
                    }
                }
                }  // do_amb_carry
                // --- SIM-ONLY ORACLE AMBIGUITY PIN (filter/NetworkOraclePin) -------
                // One DIAGONAL local prior row per pinned ambiguity column, of the
                // SAME shape as pl_emit_row's diagonal branch — a real (Λ, η) pair,
                // never an `x_lin` shift (⚠ docs/SPEC_lane_slip_response.md §7.2: a
                // shift of x_lin alone is a proven EXACT solve no-op, which is why
                // the pin does not also write LocalCarry::value_abs).
                //
                // Its OWN loop, deliberately OUTSIDE `do_amb_carry`: the pin is
                // INDEPENDENT of the leveling carry.  It fires whether or not the arc
                // has a LocalCarry entry (a newborn leg is pinned too), whether or not
                // the slip response freed the arc this epoch (free_ambs), and
                // regardless of the F3 block route — that independence is the point of
                // the instrument.  The table is built SERIALLY once per epoch
                // (networkOraclePinEpoch) and is READ-ONLY here, so this is safe in
                // the parallel produce phase.  Counters are produce-phase local and
                // folded serially below, like relevel_free_skips.
                //
                // Closure the run-end clause states: offered = rows + gauge-pinned
                // skipped + no column.
                if (opin_on) {
                    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(key);
                        if (!parts) continue;
                        const auto pit = opin_table->find(
                            std::make_tuple(static_cast<int>(i), parts->satellite,
                                            parts->signal));
                        if (pit == opin_table->end()) continue;
                        ++R.opin_offered;
                        if (networkIsGaugePinned_(stations, phase_datum_prioritized,
                                                  i, parts->satellite, parts->signal)) {
                            // The anchor's 1e10 datum pin defines this family's gauge;
                            // a second row here would fight it.  The table build skips
                            // anchors too — this is the authoritative backstop, and it
                            // is counted so the closure never has to be inferred.
                            ++R.opin_gauge_skipped;
                            continue;
                        }
                        const int amb_idx = static_cast<int>(sidx);
                        if (amb_idx < 0 || amb_idx >= x_lin.size()) {
                            ++R.opin_no_column; continue;
                        }
                        const auto hit = h2local.find(amb_idx);
                        if (hit == h2local.end()) {       // column not in A_r this epoch
                            ++R.opin_no_column; continue;
                        }
                        if (!std::isfinite(x_lin(amb_idx))) { ++R.opin_no_column; continue; }
                        const double lam = 1.0 / (pit->second.sigma_m * pit->second.sigma_m);
                        const double eta = lam * (pit->second.value_m - x_lin(amb_idx));
                        if (!std::isfinite(lam) || !std::isfinite(eta)) {
                            ++R.opin_no_column; continue;  // never poison u_r
                        }
                        inj.emplace_back(static_cast<Eigen::Index>(hit->second), lam, eta);
                        ++R.opin_rows;
                    }
                }
                // --- ZWD carry (B9): treat the tropo ZWD column like one more carried
                //     local state (random WALK, not random constant → positive q).  The
                //     prior tuple joins `inj` so it is ordered by the same ascending sort
                //     and folded by the single addStationPrior inject (byte-identical OFF).
                //     zwd_seam_live opens the SAME fold for the bounded post-resume
                //     window (network.zwd_seam_reseed): first epoch folds the restored
                //     Z-row posterior, later epochs the rolling re-stash — (cz)'s
                //     validated mechanism with a retirement clock.
                if (zwd_on || zwd_seam_live) {
                    const size_t tropo_idx_sz = st.sm->getTropoIndex();
                    if (tropo_idx_sz != std::numeric_limits<size_t>::max()) {
                        const int tropo_idx = static_cast<int>(tropo_idx_sz);
                        if (tropo_idx >= 0 && tropo_idx < x_lin.size()) {
                            const auto zhit = h2local.find(tropo_idx);
                            if (zhit != h2local.end() && std::isfinite(x_lin(tropo_idx))) {
                                const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
                                R.pl_zwd_present = true;   // record for the post-solve stash
                                R.pl_zwd = PlZwd{zcol, x_lin(tropo_idx)};
                                const auto zit = zwd_carry.find(static_cast<int>(i));
                                // B9-in-block: this station-epoch's ZWD already folded as a
                                // column of the F3 whitened block above — the diagonal row
                                // here would be the SECOND assertion of the same direction
                                // (the double count the card removes), so it is skipped.
                                if (zit != zwd_carry.end() && !R.pl_zwd_in_block) {
                                    const double zgap = pl_now_s - zit->second.last_seen_s;
                                    if (zgap <= pl_max_gap_s) {
                                        const double zvar_eff =
                                            zit->second.var
                                            + zwd_q_eff * std::max(0.0, zgap);
                                        if (zvar_eff > 0.0) {
                                            if (carry_cpl_cond) {
                                                cb.buildCplRow(zcol, x_lin(tropo_idx),
                                                            zit->second, zvar_eff,
                                                            std::max(0.0, zgap));
                                                R.pl_zwd_injected = true;   // L7: a carry was actually folded
                                            } else {
                                                const double zlambda = 1.0 / zvar_eff;
                                                const double zeta =
                                                    zlambda * (zit->second.value_abs - x_lin(tropo_idx));
                                                if (std::isfinite(zeta)) {
                                                    inj.emplace_back(zcol, zlambda, zeta);
                                                    R.pl_zwd_injected = true;   // L7: a carry was actually folded
                                                }
                                            }
                                            // B9-in-block DEGRADE edge (invariant #4/#7): a
                                            // diagonal ZWD row emitted while the block route
                                            // is ARMED is a fallback — the station had no
                                            // block this epoch (no carried ambiguities, or
                                            // the carry is not warm for the ambiguity half),
                                            // or the block's factorization failed after the
                                            // ridge retry (n_pl_blk_fallback attributes
                                            // which).  Behaviour degrades to exactly today's
                                            // B9, never to "no ZWD memory" — but it is NOT
                                            // the mechanism under test, so it is counted.
                                            if (zwd_blk_on && R.pl_zwd_injected)
                                                ++R.pl_blk_zwd_fallback_diag;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                }   // legacy_local_inject
                // --- Slant-iono carry (QUEUE 8b, docs/SPEC_slip_relevel_walk.md §8
                //     residual): per-LOS iono columns as carried RW priors.  OUTSIDE
                //     the legacy bracket (rx-CB pattern) so a future no-leveling
                //     lineage can host it without re-plumbing; today the loader
                //     requires phase_leveling so the census sets above are always
                //     live when this runs warm.  The carry map is READ-ONLY here
                //     (parallel produce phase); the serial harvest stashes, the
                //     serial pre-pass adjudicates the slip edges, NetworkRaim owns
                //     the T4 erase.  Diagonal fold even under carry_cpl_cond — the
                //     rx-CB rationale: a coupling row would hand a strictly-local
                //     state a shared-border footprint (and pin the OLD gauge across
                //     datum re-realizations).
                if (iono_on) {
                    for (const auto& [isat, iidx_sz] : st.sm->getIonosphereStates()) {
                        const int iidx = static_cast<int>(iidx_sz);
                        if (iidx < 0 || iidx >= x_lin.size()) continue;
                        if (!std::isfinite(x_lin(iidx))) continue;
                        const auto ihit = h2local.find(iidx);
                        if (ihit == h2local.end()) continue;  // iono column not in A_r this epoch
                        const Eigen::Index icol = static_cast<Eigen::Index>(ihit->second);
                        // Record UNCONDITIONALLY: the harvest stashes every epoch so
                        // the carry tracks the solution even when the scope gate skips
                        // the inject (the pl_zwd_present pattern).
                        R.pl_iono.emplace_back(isat, icol, x_lin(iidx));
                        if (!iono_scope_always) {
                            // scope=relevel: inject only for a LOS RE-LEVELING this
                            // epoch — a detector-freed band, or a present-but-carryless
                            // LOS (post-T4-erase / gap re-birth / datum-station mass
                            // reseed).  One epoch of exposure per re-draw event.
                            const bool releveling =
                                iono_freed_sats.count(isat) > 0 ||
                                (iono_sats_seen.count(isat) > 0 &&
                                 iono_sats_carried.count(isat) == 0);
                            if (!releveling) continue;
                        }
                        const auto iit = iono_carry.find({static_cast<int>(i), isat});
                        if (iit == iono_carry.end()) continue;  // first sighting — no carry yet
                        const double igap = pl_now_s - iit->second.last_seen_s;
                        if (igap > iono_max_gap_s) continue;    // stale — harvest housekeeping drops it
                        // var_eff = max(var + q·gap, floor).  The floor is load-bearing
                        // (invariant #1): the harvested marginal is phase-locked-tiny
                        // mid-arc, and an unfloored prior out-weighs the fresh GF code
                        // ~80:1 — a wrong carried value would be invisible to every
                        // armed instrument (T4 pools phase rows only).  At the floor
                        // the per-event code adoption is ~10%, so a wrong carry heals
                        // over ~10 events while the churn kill (adoption² in variance)
                        // survives.
                        double ivar_eff = iit->second.var + iono_q * std::max(0.0, igap);
                        if (ivar_eff < iono_var_floor) {
                            ivar_eff = iono_var_floor;
                            ++R.iono_carry_floor_hits;
                        }
                        if (!(ivar_eff > 0.0)) continue;
                        const double ilambda = 1.0 / ivar_eff;
                        const double ieta =
                            ilambda * (iit->second.value_abs - x_lin(iidx));
                        if (!std::isfinite(ieta)) continue;
                        inj.emplace_back(icol, ilambda, ieta);
                        ++R.iono_carry_injected;
                        R.iono_injected_sats.insert(isat);
                    }
                }
                // --- rx-CB carry (docs/SPEC_rx_cb_carry.md): receiver code-bias
                //     columns as carried RW priors.  OUTSIDE the legacy gate above —
                //     live on the carry_invariant / no-leveling lineage.  The carry
                //     map is READ-ONLY here (parallel produce phase); decisions are
                //     counted in R and the serial harvest applies every consequence
                //     (stale-entry overwrite = the implicit drop).  Diagonal fold
                //     even under carry_cpl_cond — the RPB rationale: a coupling row
                //     would hand a strictly-local hardware state a shared-border
                //     footprint.
                // RXCB-DATUM shares this walk: the receiver code-bias DATUM needs the
                // same (key, A_r column) records the CARRY collects, so pl_cb is filled
                // whenever EITHER is armed and only the carry-injection half below stays
                // behind rx_cb_on.  (The post-solve harvest that consumes pl_cb is itself
                // gated on rx_cb_on, so a datum-only run fills the vector and no one
                // harvests it — intended.)
                if (rx_cb_on || rx_cb_datum) {
                    for (const auto& [cb_key, cb_sidx] : st.sm->getCodeBiasStates()) {
                        if (StateKeys::isGlonassPerSlotIfbKey(cb_key)) continue;  // anchored family
                        const int cb_idx = static_cast<int>(cb_sidx);
                        if (cb_idx < 0 || cb_idx >= x_lin.size()) continue;
                        if (!std::isfinite(x_lin(cb_idx))) continue;
                        const auto cbhit = h2local.find(cb_idx);
                        if (cbhit == h2local.end()) continue;
                        const Eigen::Index cbcol = static_cast<Eigen::Index>(cbhit->second);
                        R.pl_cb.emplace_back(cb_key, cbcol, x_lin(cb_idx));
                        // --- RXCB-DATUM: loose zero-mean prior on the receiver DCB ----
                        // The receiver-side counterpart of the W6 satellite code-bias
                        // sum-to-zero, realized PRE-REDUCE through the same diagonal
                        // prior channel Card KP uses for satellite OSBs — NOT as a
                        // post-reduce cross-station SumZeroGauge, because
                        // NetworkEstimator::solve rightly refuses to compose a
                        // post-reduce Woodbury gauge with the Card-4 DD priors (both
                        // fold against the same cached local factors; ambiguities and
                        // code biases are correlated through the observation rows, so
                        // "disjoint columns" does not make it exact).
                        //
                        // WHY A DIAGONAL PRIOR IS A DATUM HERE.  d_r ~ N(0, sigma^2) on
                        // every station adds 1/sigma^2 along EVERY direction of the DCB
                        // subspace.  The between-station DIFFERENCES are estimable and
                        // carry orders more data information, so the prior does not move
                        // them; the COMMON MODE has essentially none, so the prior alone
                        // determines it — and sets it to zero-mean-over-receivers, which
                        // is exactly the IGS DCB convention.  Note the realized
                        // common-mode eigenvalue is 1/sigma^2, INDEPENDENT of the station
                        // count (u = 1/sqrt(N) * 1 gives u^T (Lambda I) u = Lambda), the
                        // same N-independence the broadcast-anchored clock datum documents.
                        //
                        // Target is the ABSOLUTE zero, so in the local block's
                        // delta-around-x_lin coordinates the RHS is -Lambda * x_lin —
                        // the same convention the rx-CB carry's innovation uses.
                        if (rx_cb_datum && StateKeys::parseDcbKey(cb_key)) {
                            const double lam = 1.0 / (rx_cb_datum_sigma * rx_cb_datum_sigma);
                            const double eta = lam * (0.0 - x_lin(cb_idx));
                            if (std::isfinite(lam) && std::isfinite(eta)) {
                                // Card RF-PD-fix: with station_prior_before_pd the DATUM
                                // rows leave the carry vector entirely and go to the
                                // PRE-PD fold instead, so addStation's PD test sees the
                                // row that gauges this station's receiver DCB.  They must
                                // not appear in BOTH — addStationPrior does not sum with
                                // what addStation already folded.  OFF, this is the
                                // original emplace at the original point in the walk, so
                                // the coalesce order and every byte downstream are
                                // unchanged.
                                if (sta_prior_pre_pd) inj_prior.emplace_back(cbcol, lam, eta);
                                else                  inj.emplace_back(cbcol, lam, eta);
                                ++R.pl_cb_datum_rows;
                            }
                        }
                        if (!rx_cb_on) continue;   // datum-only: record the column, carry nothing
                        const auto cbit = rx_cb_carry.find({static_cast<int>(i), cb_key});
                        if (cbit == rx_cb_carry.end()) continue;   // cold — the harvest seeds it
                        double cb_var_eff = 0.0;
                        const double cb_innov_raw = cbit->second.value_abs - x_lin(cb_idx);
                        // --- Card LV-2 (docs/SPEC_level_information_budget.md) -------
                        // Subtract the information-weighted MEAN innovation of this
                        // code-bias family, computed at the previous epoch's SERIAL
                        // harvest (lag-1; the map is READ-ONLY in this parallel phase,
                        // exactly like the carry itself).  Missing key = the first
                        // epoch, or a frozen thin-mean era that never had one ⇒ μ = 0
                        // ⇒ legacy behaviour, which is the intended one-epoch warm-up.
                        //
                        // Σλ(innov−μ) = 0 by construction: the carry keeps every
                        // between-station DIFFERENCE at full stiffness (arm 14 proved
                        // that rigidity load-bearing) while exerting ZERO net force on
                        // the DCB common mode, which is a FRAME direction and belongs
                        // to the designed authority (rx_code_bias_datum / the a₀₀
                        // anchor / the satellite sum-to-zero), re-realized each epoch.
                        //
                        // The GATE judges the demeaned innovation too, deliberately: a
                        // coherent frame move is not a per-receiver hardware jump, and
                        // letting it mass-trigger the receiver-reset response withholds
                        // exactly the rows whose DIFFERENCES are still good.
                        double cb_mu = 0.0;
                        if (rx_cb_demean) {
                            const auto mit = rx_cb_demean_mu.find(cb_key);
                            if (mit != rx_cb_demean_mu.end() && std::isfinite(mit->second)
                                && mit->second != 0.0)
                                cb_mu = mit->second;
                        }
                        const double cb_innov = cb_innov_raw - cb_mu;
                        // --- Card LV-4 (docs/SPEC_level_information_budget.md §11) ---
                        // With the invariant step gate armed the fold-time INNOVATION
                        // test is switched off: |value_abs − x_lin(cb)| is a prefit
                        // quantity in a FROZEN frame, so it withholds by where the
                        // first-epoch seed landed rather than by anything the receiver
                        // did, and it cannot see a jump between two epochs at all.
                        // What survives here is frame-free (gap + variance sanity); the
                        // event response moved to the serial harvest, which compares two
                        // ABSOLUTE values.  The retired test is still EVALUATED, purely
                        // to count what it would have withheld — the A/B lives inside
                        // this one run instead of across two.
                        const RxCbInjectAction act = rxCbInjectGate(
                            pl_now_s - cbit->second.last_seen_s, /*max_gap_s=*/900.0,
                            cbit->second.var, rx_cb_q, cb_innov,
                            /*k_sigma=*/10.0, /*guard_var=*/0.25, cb_var_eff,
                            /*innov_test=*/!rx_cb_step_gate);
                        if (rx_cb_step_gate && act == RxCbInjectAction::Fold) {
                            double shadow_var = 0.0;
                            if (rxCbInjectGate(pl_now_s - cbit->second.last_seen_s, 900.0,
                                               cbit->second.var, rx_cb_q, cb_innov,
                                               10.0, 0.25, shadow_var, true)
                                == RxCbInjectAction::SkipInnov)
                                ++R.pl_cb_frame_gate_would_skip;
                        }
                        if (act == RxCbInjectAction::SkipInnov) {
                            ++R.pl_cb_innov_skips;   // receiver-reset response: withhold, harvest re-seeds
                            continue;
                        }
                        if (act != RxCbInjectAction::Fold) continue;
                        const double cb_lambda = 1.0 / cb_var_eff;
                        const double cb_eta = cb_lambda * cb_innov;
                        if (!std::isfinite(cb_eta)) continue;
                        inj.emplace_back(cbcol, cb_lambda, cb_eta);
                        ++R.pl_cb_injected;
                        // LV-2 accumulation for NEXT epoch's μ.  Only FOLDED rows vote
                        // (a withheld row exerts no force), and the RAW innovation is
                        // what the mean is taken over — μ must estimate the common
                        // content of the carry's actual pull, not of an already-demeaned
                        // residual, or the correction would decay geometrically.
                        if (rx_cb_demean) {
                            R.pl_cb_sum_lam[cb_key]       += cb_lambda;
                            R.pl_cb_sum_lam_innov[cb_key] += cb_lambda * cb_innov_raw;
                            if (cb_mu != 0.0) ++R.pl_cb_demeaned;
                        }
                    }
                }
                if (carry_cpl_cond) {
                    // Card 1/1b: batch-append this station's conditional coupling rows into
                    // R.A_s/R.A_r (joint-whitened when carry_cpl_joint, else scalar per-row).
                    cb.appendCouplingRows();
                }
                {
                    // Diagonal fold — under carry_cpl_cond the legacy paths routed through
                    // buildCplRow, so inj here holds ONLY rx-CB rows (empty when off —
                    // resize(0) leaves R.carry_* exactly as before: byte-identical).
                    // ASCENDING column order → deterministic rank-1 update sequence.
                    std::sort(inj.begin(), inj.end(),
                              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                    // COALESCE duplicate columns.  A column can now receive TWO diagonal
                    // rows — the RXCB-DATUM prior and the rx-CB carry prior — and
                    // information is additive, so they must be summed into one entry
                    // rather than handed to addStationPrior twice (which does not sum).
                    // A no-op for every pre-RXCB-DATUM config: without the datum each
                    // cb_key contributes at most one row, so the vector has no
                    // duplicates and this leaves it byte-identical.
                    if (inj.size() > 1) {
                        std::size_t w = 0;
                        for (std::size_t r = 1; r < inj.size(); ++r) {
                            if (std::get<0>(inj[r]) == std::get<0>(inj[w])) {
                                std::get<1>(inj[w]) += std::get<1>(inj[r]);
                                std::get<2>(inj[w]) += std::get<2>(inj[r]);
                            } else {
                                inj[++w] = inj[r];
                            }
                        }
                        inj.resize(w + 1);
                    }
                    const Eigen::Index ninj = static_cast<Eigen::Index>(inj.size());
                    R.carry_cols.resize(static_cast<size_t>(ninj));
                    R.carry_dNrr.resize(ninj);
                    R.carry_dur.resize(ninj);
                    for (Eigen::Index k = 0; k < ninj; ++k) {
                        R.carry_cols[static_cast<size_t>(k)] = std::get<0>(inj[static_cast<size_t>(k)]);
                        R.carry_dNrr(k) = std::get<1>(inj[static_cast<size_t>(k)]);
                        R.carry_dur(k)  = std::get<2>(inj[static_cast<size_t>(k)]);
                    }
                    // Card RF-PD-fix: the same finalize for the PRE-PD datum rows —
                    // ascending column order for a deterministic fold, and the same
                    // coalesce because a single receiver DCB column can in principle
                    // receive more than one datum row (information is additive, and
                    // addStation's diagonal fold, like addStationPrior's, does not sum
                    // duplicate entries).  Empty OFF ⇒ R.prior_* untouched.
                    std::sort(inj_prior.begin(), inj_prior.end(),
                              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                    if (inj_prior.size() > 1) {
                        std::size_t w = 0;
                        for (std::size_t r = 1; r < inj_prior.size(); ++r) {
                            if (std::get<0>(inj_prior[r]) == std::get<0>(inj_prior[w])) {
                                std::get<1>(inj_prior[w]) += std::get<1>(inj_prior[r]);
                                std::get<2>(inj_prior[w]) += std::get<2>(inj_prior[r]);
                            } else {
                                inj_prior[++w] = inj_prior[r];
                            }
                        }
                        inj_prior.resize(w + 1);
                    }
                    const Eigen::Index npri = static_cast<Eigen::Index>(inj_prior.size());
                    R.prior_cols.resize(static_cast<size_t>(npri));
                    R.prior_dNrr.resize(npri);
                    R.prior_dur.resize(npri);
                    for (Eigen::Index k = 0; k < npri; ++k) {
                        R.prior_cols[static_cast<size_t>(k)] = std::get<0>(inj_prior[static_cast<size_t>(k)]);
                        R.prior_dNrr(k) = std::get<1>(inj_prior[static_cast<size_t>(k)]);
                        R.prior_dur(k)  = std::get<2>(inj_prior[static_cast<size_t>(k)]);
                    }
                }
            }
            // --- Unconditional Z-row capture (§17 finding 3): RECORD the tropo
            // column + x_lin for the post-solve ZWD harvest on any snapshot-armed
            // run, so every RTB-7 snapshot serializes fresh Z rows without the B9
            // carry.  Record-only — no carry lookup, no fold: the solve is
            // untouched (read-only getStateVector copy, the qc_amb precedent).
            // Skipped when the B9/seam block above already recorded (identical
            // fields), and deliberately its own block OUTSIDE legacy_local_inject
            // — the RAIM-pool-re-host pattern — so the capture works through the
            // phase-leveling warmup window and on no-leveling lineages.
            if (zwd_capture && !R.pl_zwd_present) {
                const size_t tropo_idx_sz = st.sm->getTropoIndex();
                if (tropo_idx_sz != std::numeric_limits<size_t>::max()) {
                    const int tropo_idx = static_cast<int>(tropo_idx_sz);
                    const Eigen::VectorXd x_lin_z = st.sm->getStateVector();
                    if (tropo_idx >= 0 && tropo_idx < x_lin_z.size()
                        && std::isfinite(x_lin_z(tropo_idx))) {
                        const auto zhit = h2local.find(tropo_idx);
                        if (zhit != h2local.end()) {
                            R.pl_zwd_present = true;
                            R.pl_zwd = PlZwd{static_cast<Eigen::Index>(zhit->second),
                                             x_lin_z(tropo_idx)};
                        }
                    }
                }
            }
            R.ok = true;
        } catch (const std::exception& ex) {
            R.threw = true; R.err = ex.what();
        } catch (...) {
            R.threw = true; R.err = "unknown exception";
        }
    }

    // Card 1/1d observability (SERIAL, post-produce): fold the per-station
    // coupling-term counts into the run totals + the one-shot Card-1d debug dump
    // of the first few assembled coupling rows (see filter/NetworkCoupling.hpp).
    // SPEC_slip_relevel_walk: fold the per-station free-epoch skip counts
    // (SERIAL, post-produce — same discipline as the coupling fold below).
    if (relevel_free_on)
        for (const StationAssembly& Rc : asm_res)
            ctx.counters.n_relevel_free_skips += Rc.relevel_free_skips;
    // SIM-ONLY ORACLE AMBIGUITY PIN: fold this epoch's per-station pin accounting
    // into the run-scope counters (SERIAL, post-produce — the same discipline).
    // Null pointer OFF, so this is unreachable on an unarmed run.
    if (ctx.carry.oracle_pin) {
        auto& op = *ctx.carry.oracle_pin;
        for (const StationAssembly& Rc : asm_res) {
            op.rows_injected        += Rc.opin_rows;
            op.gauge_pinned_skipped += Rc.opin_gauge_skipped;
            op.no_column            += Rc.opin_no_column;
        }
    }
    // SPEC_withhold_era_release §8: fold the inject-side stale skips (the cliff
    // mechanism) and record the worst epoch with a handle a trace filter can use.
    {
        long long stale_ep = 0, stale_sta = 0;
        for (const StationAssembly& Rc : asm_res)
            if (Rc.inject_stale_skips > 0) { stale_ep += Rc.inject_stale_skips; ++stale_sta; }
        if (stale_ep > 0) {
            ctx.counters.n_cr_inject_stale += stale_ep;
            ++ctx.counters.n_cr_inject_stale_epochs;
            if (stale_ep > ctx.counters.n_cr_inject_stale_max) {
                ctx.counters.n_cr_inject_stale_max      = stale_ep;
                ctx.counters.n_cr_inject_stale_max_week = se.time.week;
                ctx.counters.cr_inject_stale_max_tow    = se.time.tow;
                ctx.counters.n_cr_inject_stale_max_sta  = stale_sta;
            }
        }
    }
    // QUEUE 8b: fold the per-station iono-carry inject counts (same discipline).
    if (iono_on)
        for (const StationAssembly& Rc : asm_res) {
            ctx.counters.n_iono_carry_injected += Rc.iono_carry_injected;
            ctx.counters.n_iono_carry_floor_hits += Rc.iono_carry_floor_hits;
        }
    // F1 (network.phase_leveling_los_scale): fold the per-station LOS-scale telemetry
    // (same discipline — counted in the parallel produce, reduced serially here).
    if (pl_los_scale)
        for (const StationAssembly& Rc : asm_res) {
            ctx.counters.n_pl_los_scale_rows += Rc.pl_los_scale_rows;
            for (int b = 0; b < 4; ++b)
                ctx.counters.n_pl_los_scale_hist[b] += Rc.pl_los_scale_hist[b];
        }
    // F3 (network.phase_leveling_local_block): fold the per-station block telemetry
    // (same discipline).  Keyed on the KNOB, not the live predicate, so an armed-but-
    // inert posture still prints its zeros at run end (invariant #4).
    // RF-1 step 3 (design risk #3): a non-SPD block is a VEHICLE CHANGE on one
    // station-epoch under float_kf — the arc silently reverts to the per-column route the
    // card exists to replace — so it is named with its identities here, in the SERIAL
    // scatter (the produce loop is parallel and would interleave).  Rate-limited to the
    // first 20 so a pathological posture cannot flood the log; the run-end bar carries
    // the totals either way.
    if (ctx.knobs.pl_blk_from_vehicle)
        for (std::size_t si = 0; si < asm_res.size(); ++si) {
            if (asm_res[si].pl_blk_fallback <= 0) continue;
            if (ctx.counters.n_amb_block_fallback < 20)
                log->warn("Network (RF-1 step 3): station '{}' TOW {:.1f} — the joint local "
                          "block (dim {}) was NON-SPD after the ridge retry, so this "
                          "station-epoch folded the carried prior as INDEPENDENT per-column "
                          "rows instead.  Under ambiguity_vehicle = float_kf that is a "
                          "VEHICLE CHANGE on one station-epoch, not a degraded option: those "
                          "columns carry today's over-count for this epoch.  The run FAILS if "
                          "the rate exceeds 1 % of attempted station-epochs.",
                          si < stations.size() ? stations[si].name : std::string("?"),
                          pl_now_s, asm_res[si].pl_blk_fallback_dim);
            ctx.counters.n_amb_block_fallback += asm_res[si].pl_blk_fallback;
        }
    if (pl_local_block)
        for (const StationAssembly& Rc : asm_res) {
            ctx.counters.n_pl_blk_blocks       += Rc.pl_blk_blocks;
            ctx.counters.n_pl_blk_rows         += Rc.pl_blk_rows;
            ctx.counters.n_pl_blk_marg_cols    += Rc.pl_blk_marg_cols;
            ctx.counters.n_pl_blk_pair_missing += Rc.pl_blk_pair_missing;
            ctx.counters.n_pl_blk_col_refused  += Rc.pl_blk_col_refused;
            ctx.counters.n_pl_blk_fallback     += Rc.pl_blk_fallback;
            ctx.counters.n_pl_blk_young_cols   += Rc.pl_blk_young_cols;
            ctx.counters.n_pl_blk_yng_scaled   += Rc.pl_blk_yng_scaled;
            // B9-in-block: same discipline (counted in the parallel produce, reduced here).
            ctx.counters.n_pl_blk_zwd_cols          += Rc.pl_blk_zwd_cols;
            ctx.counters.n_pl_blk_zwd_fallback_diag += Rc.pl_blk_zwd_fallback_diag;
            ctx.counters.n_pl_blk_zwd_missing_cross += Rc.pl_blk_zwd_missing_cross;
            // I-in-block: same discipline (counted in the parallel produce, reduced here).
            ctx.counters.n_pl_blk_iono_cols          += Rc.pl_blk_iono_cols;
            ctx.counters.n_pl_blk_iono_not_folded    += Rc.pl_blk_iono_not_folded;
            ctx.counters.n_pl_blk_iono_missing_cross += Rc.pl_blk_iono_missing_cross;
            ctx.counters.n_iono_blk_gap_drop         += Rc.pl_blk_iono_gap_drop;
            // RX-CLK-in-block: same discipline (counted in the parallel produce,
            // reduced here).  n_rxclk_carry_refused is NOT here: the jump response fires
            // at the serial HARVEST, where the one-step prediction innovation exists.
            ctx.counters.n_pl_blk_rxclk_cols          += Rc.pl_blk_rxclk_cols;
            ctx.counters.n_pl_blk_rxclk_isb_cols      += Rc.pl_blk_rxclk_isb_cols;
            ctx.counters.n_pl_blk_rxclk_not_folded    += Rc.pl_blk_rxclk_not_folded;
            ctx.counters.n_pl_blk_rxclk_missing_cross += Rc.pl_blk_rxclk_missing_cross;
            ctx.counters.n_rxclk_carry_gap_drop       += Rc.pl_blk_rxclk_gap_drop;
            for (int b = 0; b < 5; ++b)
                ctx.counters.n_pl_blk_hist[b] += Rc.pl_blk_hist[b];
            if (Rc.pl_blk_max_dim > ctx.counters.n_pl_blk_max_dim)
                ctx.counters.n_pl_blk_max_dim = Rc.pl_blk_max_dim;
        }
    // AGE-AWARE young-arc inflate: its own fold, keyed on the KNOB and NOT on
    // pl_local_block — the card is wired on the per-column route too, so folding it
    // inside the F3 block above would report a clean 0 on exactly the postures where
    // the diagonal route is doing all the work (invariant #4).
    if (ctx.knobs.yng.on())
        for (const StationAssembly& Rc : asm_res) {
            ctx.counters.n_yng_cols      += Rc.yng_cols;
            ctx.counters.n_yng_skip_age  += Rc.yng_skip_age;
            ctx.counters.yng_var_add_sum += Rc.yng_var_add_sum;
            ctx.counters.yng_age_sum     += Rc.yng_age_sum;
        }
    // §18.12: latch the FIRST warm epoch's present/departed split across this fold.  A
    // restore that reads 2871 rows and then MARGINALIZES most of their terms has SOFTENED
    // the rows (σ² += Sum M²·P_prev) — rung 3's mechanism by another route — and must be
    // read as a failure even though every "restored" counter looks healthy.  Both names
    // are run-total aliases into ctx.counters (:2115-2116), so the seam value is a
    // DIFFERENCE bracketing this one call, not anything the call returns.
    const long long cpl_p0 = n_cpl_terms_present, cpl_d0 = n_cpl_terms_departed;
    foldAndDumpCouplingRows(carry_cpl_cond, asm_res, est,
                            registered_clock_sats, shared_phase_keys, registered_code_keys,
                            se.time.tow, log,
                            n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                            ctx.counters.n_cpl_gauge_held, ctx.counters.n_cpl_gauge_orphan,
                            ctx.counters.n_cpl_reinit_diverted,
                            ctx.counters.n_cpl_readmit_diverted,
                            n_cpl_row_dump,
                            &ctx.counters.cpl_orphan_fam_total);
    if (ctx.carry.resume_cpl_census_pending && carry_cpl_cond) {
        ctx.counters.n_resume_cpl_seam_present  = n_cpl_terms_present  - cpl_p0;
        ctx.counters.n_resume_cpl_seam_departed = n_cpl_terms_departed - cpl_d0;
    }

    // SD pre-reduce EWL trace drain (SERIAL, post-produce — SPEC_sd_ewl_prereduce §6).
    // The rows were formatted per station inside the parallel produce; writing them
    // here in station index order makes the file thread-count independent.  Flushed
    // per epoch: this stream exists to diagnose runs that get killed mid-flight, and
    // an unflushed tail is exactly the evidence that would be missing.
    if (config_.getNetworkConfig().sd_ewl.trace && traces.sd_ewl_trace) {
        for (size_t i = 0; i < asm_res.size(); ++i)
            for (const std::string& row : asm_res[i].sd_ewl_trace)
                traces.sd_ewl_trace << row;
        traces.sd_ewl_trace.flush();
    }

    // --- INC-2 STRUCTURAL FUSE (docs/SPEC_prefit_census_observability.md §4) ---
    // The single last-touch site for every data row's weight: every appender
    // (splitStation, the GLONASS anchors, the SH/GIM iono ties, the SD-EWL rows)
    // has finished, and nothing between here and est.addStation writes R.w.
    // SERIAL on purpose — the row build is parallel, so a fuse inside it would
    // make the event order thread-count dependent.  s_fuse <= 0 (the default)
    // skips the whole block: no shared-column map, no classification, no
    // allocation — structurally byte-identical OFF.
    const double pc_fuse_sigma = ctx.knobs.pc_fuse_sigma;
    if (pc_fuse_sigma > 0.0) {
        const SharedColumnMap fuse_cm = buildSharedColumnMap(est, est.numShared());
        if (fuse_cm.n > 0) {
            ++ctx.counters.pc.fuse_epochs;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StationAssembly& R = asm_res[i];
                if (!R.ok || R.threw || R.state_mismatch || R.empty_innov) continue;
                networkPrefitFuseStation(ctx, traces, se.time, fuse_cm,
                                         stations[i].name, R, pc_fuse_sigma);
            }
        }
    }

    // --- Card RF-PD: NAME the null column behind each addStation PD rejection ---
    // The drop below is a per-station amputation that network_epoch_status.csv does
    // NOT report (n_present still counts the station) and whose only record is a warn
    // line naming a pivot MAGNITUDE — no parameter.  Counting is unconditional (free,
    // no output); the knob arms the kernel's eigen read and these event rows.
    const bool pd_null_on = nc.trace_station_pd_null;
    int  n_pd_dropped_ep  = 0;
    auto pdDropEvent = [&](const NetStation& sta, const StationAssembly& RA,
                           const char* action) {
        ++ctx.counters.pd_drop_total;
        ++n_pd_dropped_ep;
        if (!pd_null_on) return;
        const auto& f = est.normals().lastStationPdFailure();
        if (!f.valid) {
            // Armed but no record: the throw came from a DIFFERENT addStation guard
            // (dimension / symmetry / non-finite LDLᵀ), not the PD test.  Say so
            // rather than emit a row with no direction — invariant #4.
            traces.emitEvent(se.time, "TOPOLOGY", "sta_pd_dropped", "", sta.name,
                             std::nullopt, std::nullopt, action, "no_pd_record");
            return;
        }
        // Local column -> StateManager label, through the assembly's OWN h2local
        // map (the same inverse network.neq_export_epoch uses).  The kernel has no
        // label vocabulary, so the naming must happen here.
        std::vector<std::string> names(static_cast<std::size_t>(f.n_local), "?");
        {
            const auto sm_labels = sta.sm->getStateLabels();
            for (const auto& kv : RA.h2local) {
                const auto lc = static_cast<std::size_t>(kv.second);
                if (kv.second >= 0 && lc < names.size()
                    && static_cast<std::size_t>(kv.first) < sm_labels.size())
                    names[lc] = sm_labels[static_cast<std::size_t>(kv.first)];
            }
        }
        // Top-5 |weight| entries of the null eigenvector, descending.
        std::vector<Eigen::Index> ord;
        ord.reserve(static_cast<std::size_t>(f.null_vec.size()));
        for (Eigen::Index k = 0; k < f.null_vec.size(); ++k) ord.push_back(k);
        const std::size_t topn = std::min<std::size_t>(5, ord.size());
        std::partial_sort(ord.begin(), ord.begin() + static_cast<std::ptrdiff_t>(topn),
                          ord.end(), [&](Eigen::Index a, Eigen::Index b) {
                              const double fa = std::abs(f.null_vec(a));
                              const double fb = std::abs(f.null_vec(b));
                              if (fa != fb) return fa > fb;
                              return a < b;              // determinism on ties
                          });
        // DIR-row style: FAMILY|SAT|SIG=weight.  The StateManager key delimiters
        // (':' after the family tag, the FIRST '_' inside the key) become '|'; an
        // iono-free signal combo keeps its own underscore (AMB|G01|L1C_L5Q).
        auto dirName = [](std::string s) {
            const auto c = s.find(':');
            if (c != std::string::npos) {
                s[c] = '|';
                const auto u = s.find('_', c + 1);
                if (u != std::string::npos) s[u] = '|';
            }
            return s;
        };
        std::string cols;
        for (std::size_t k = 0; k < topn; ++k) {
            if (k) cols += ' ';
            cols += fmt::format("{}={:.4f}", dirName(names[static_cast<std::size_t>(ord[k])]),
                                f.null_vec(ord[k]));
        }
        // The null is spread almost EVENLY over a whole family here (every L2W
        // ambiguity at ~0.285), so a top-5 slice under-reports the families that
        // carry real mass in the tail — the receiver code bias above all.  Carry the
        // FULL eigenvector's per-family mass Σw² (it sums to 1) beside the top-5, so
        // the census is not a function of where the cut fell.
        std::map<std::string, double> fam_mass;
        for (Eigen::Index k = 0; k < f.null_vec.size(); ++k) {
            const std::string& lb = names[static_cast<std::size_t>(k)];
            const auto c = lb.find(':');
            fam_mass[c != std::string::npos ? lb.substr(0, c)
                                            : (lb == "?" ? std::string("other") : lb)]
                += f.null_vec(k) * f.null_vec(k);
        }
        std::string fam_s;
        for (const auto& [k, v] : fam_mass) {
            if (!fam_s.empty()) fam_s += ' ';
            fam_s += fmt::format("{}:{:.4f}", k, v);
            ctx.counters.pd_drop_family_mass[k] += v;
        }
        if (topn > 0) {
            std::string fam = names[static_cast<std::size_t>(ord[0])];
            const auto c = fam.find(':');
            fam = (c != std::string::npos) ? fam.substr(0, c)
                                           : (fam == "?" ? std::string("other") : fam);
            ++ctx.counters.pd_drop_family[fam];
        }
        ++ctx.counters.pd_drop_named;
        traces.emitEvent(se.time, "TOPOLOGY", "sta_pd_dropped", "", sta.name,
                         f.ev_min, f.d_ref, action,
                         // `statistic` carries ev_min too, but emitEvent formats every
                         // numeric column as {:.6f} — a 1e-10 eigenvalue prints as
                         // 0.000000 there and its SIGN is lost, so the headline number
                         // of this row would read as "exactly zero" on every rejection.
                         // Repeat it in the detail in scientific notation; that is the
                         // copy a grader must read.
                         fmt::format("evmin={:.6e};dmin={:.6e};dmax={:.6e};evmax={:.6e};"
                                     "neig={};nlocal={};fam={};cols={}",
                                     f.ev_min, f.d_min, f.d_max, f.ev_max, f.n_eig_below,
                                     static_cast<long long>(f.n_local), fam_s, cols));
    };

    // Serial consume: scatter each station's block into the shared arrowhead
    // in deterministic station order, preserving the exact pivot-aborts-epoch
    // / drop-non-pivot disposition of the original single-pass loop.
    total_pins = 0;
    bool epoch_ok   = true;
    for (size_t i = 0; i < stations.size() && epoch_ok; ++i) {
        if (!present[i]) continue;
        NetStation&      st = stations[i];
        StationAssembly& R  = asm_res[i];

        if (R.empty_innov) { present[i] = false; continue; }
        if (R.state_mismatch) {
            log->warn("Network: station '{}' H/state mismatch ({})", st.name, R.err);
            // Legacy: the single pivot carries the whole datum, so its loss sinks
            // the epoch.  Prioritized: the gauge is distributed across anchors, so
            // drop this station like any other — any keys it anchored are pruned
            // by the null-space gate (drop-and-heal; the epoch self-heals next).
            if (!phase_datum_prioritized && st.is_pivot) { epoch_ok = false; break; }
            present[i] = false;
            continue;
        }
        if (R.threw || !R.ok) {
            // computeMeasurementUpdate / splitStation threw.  The original
            // single-pass loop did not guard these (an exception escaped the
            // run); under threads they MUST be caught, so their disposition is
            // decided here, matching the addStation policy below: a failed
            // pivot sinks the epoch, any other station is dropped.
            const std::string why = R.err.empty() ? "no usable block" : R.err;
            if (!phase_datum_prioritized && st.is_pivot) {
                log->warn("Network: PIVOT '{}' assembly failed ({}); skipping epoch",
                          st.name, why);
                epoch_ok = false;
                break;
            }
            log->warn("Network: station '{}' assembly failed ({}); dropping it this epoch",
                      st.name, why);
            present[i] = false;
            continue;
        }

        total_pins += R.np;
        if (R.cpl_joint_fallback) ++n_cpl_joint_fallback;   // Card 1b: joint LLT degraded to scalar

        try {
            // Card RF-PD-fix: hand addStation this station's own DATUM rows so the
            // PD test judges the block the SOLVE uses.  R.prior_* is EMPTY unless
            // network.station_prior_before_pd is armed, and the kernel then takes
            // no copy of N_rr and folds nothing — the OFF path is byte-identical.
            // The failure path is unchanged BY CONSTRUCTION: addStation already
            // builds and validates the whole local block before it touches the
            // shared border, so a rejected station still leaves N_ss_/u_s_
            // untouched and there is nothing to roll back (which is why the prior
            // goes INTO addStation rather than into a two-phase add + validate).
            est_index_of[i] = static_cast<int>(
                est.addStation(R.A_s, R.A_r, R.w, R.y,
                               NetworkNormalEquations::StationPrior{
                                   R.prior_cols, R.prior_dNrr, R.prior_dur}));
        } catch (const std::exception& ex) {
            // One station's rank-deficient / ill-conditioned local block must
            // not sink the whole epoch in a large heterogeneous network: drop
            // it and keep the rest (the null-space gate still guards the
            // result).  On the legacy path the PIVOT is the exception — it
            // carries the S-basis, so without it the epoch has no datum and must
            // be skipped.  Prioritized: the gauge is distributed, so drop it too
            // (an anchor loss is caught by the null-space gate → withhold + heal).
            if (!phase_datum_prioritized && st.is_pivot) {
                log->warn("Network: PIVOT '{}' addStation rejected ({}); skipping epoch",
                          st.name, ex.what());
                ++ctx.counters.pd_drop_pivot_abort;   // RF-PD: the epoch-sinking flavour
                pdDropEvent(st, R, "epoch_sunk");
                epoch_ok = false;
                break;
            }
            log->warn("Network: station '{}' addStation rejected ({}); dropping it this epoch",
                      st.name, ex.what());
            pdDropEvent(st, R, "dropped");            // RF-PD: name the null column
            present[i] = false;
            continue;
        }

        // Phase-leveling carry: fold this station's diagonal carried prior into
        // its local block NOW (after addStation, before the gate/solve trigger
        // reduce) so the single arrowhead solve already reflects it.  The pivot's
        // ambiguity columns are gauge-pinned (never carried), but its ZWD column
        // may be (B9), so gate on a non-empty carry set — not on !is_pivot: for the
        // pivot R.carry_cols holds at most the ZWD column, and is empty (no call,
        // byte-identical) unless network.carry_zwd is on.  A reject here drops only
        // this station's carry, not the station — its data is already in the system.
        // rx-CB carry (SPEC_rx_cb_carry): rx_cb_on joins the gate — when carry_warm
        // is false the produce phase built ONLY rx-CB rows (the legacy inject paths
        // sit inside legacy_local_inject = carry_warm && …), so the extended gate
        // folds exactly them and rx_cb_on=false remains byte-identical.
        // RXCB-DATUM joins it for the SAME reason, and must: this is the INSTALL
        // site.  Widening only the build gate above would have produced rows that
        // are coalesced into R.carry_cols and then dropped here — the worst of both
        // worlds, because the gauge counter would then report folds that never
        // reached the normal equations (a telemetry channel that actively lies).
        // Both edges of the datum move together or neither does.
        // Card RF-PD-fix: under network.station_prior_before_pd the RXCB-DATUM rows are
        // NOT in R.carry_cols — they went to addStation above and were folded before the
        // PD test.  They must not be folded a second time here (addStationPrior does not
        // sum with what addStation already put in N_rr), and they are not: the routing is
        // exclusive at the emplace.  `rx_cb_datum` stays in the gate below because on a
        // datum-only, knob-ON run R.carry_cols is then empty and the second conjunct
        // already suppresses the call.
        // The ORACLE PIN joins the install gate for the same reason RXCB-DATUM did:
        // its rows are coalesced into R.carry_cols above, and a gate that built them
        // and then dropped them would leave the run-end clause reporting folds that
        // never reached the normal equations — a telemetry channel that actively
        // lies.  Both edges move together or neither does.
        if ((carry_warm || rx_cb_on || rx_cb_datum || R.opin_rows > 0)
            && !R.carry_cols.empty()) {
            try {
                est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                    R.carry_cols, R.carry_dNrr, R.carry_dur);
            } catch (const std::exception& ex) {
                log->warn("Network[phase-leveling]: station '{}' carry inject rejected "
                          "({}); proceeding without its carry this epoch", st.name, ex.what());
            }
        }
    }

    // Card RF-PD: roll this epoch's PD-drop count up.  The DENOMINATOR is every epoch
    // that reached the consume loop, so "0 dropped" is distinguishable from "the loop
    // never ran" (standing invariant #4).
    ++ctx.counters.pd_drop_epochs;
    if (n_pd_dropped_ep > ctx.counters.pd_drop_max_ep) {
        ctx.counters.pd_drop_max_ep     = n_pd_dropped_ep;
        ctx.counters.pd_drop_max_ep_tow = se.time.tow;
    }

    // --- Card RF-PD-fix: the TRUTHFUL in-solve count + invariant #7's RE-COUPLING edge.
    // network_epoch_status.csv reports `n_present`, which is the roster that reached the
    // consume loop, NOT the set that entered the arrowhead — a station dropped here is
    // still counted present.  Record what actually went into the solve, per epoch, and
    // mark every RETURN so a station's re-entry is never read as a first sighting (the
    // exact failure invariant #7 names, and the DECOUPLING edge is already instrumented
    // by RF-PD's `sta_pd_dropped`).  Counting is unconditional and free; the event rows
    // ride network.trace_topology, the topology ledger that owns invariant #7's edges.
    // Skipped on an ABORTED epoch (a pivot rejection breaks the loop early), where the
    // later stations were never judged and an absence would be an artifact of the break.
    if (epoch_ok) {
        auto& cnt = ctx.counters;
        int n_in_solve_ep = 0;
        for (size_t i = 0; i < stations.size(); ++i) {
            const bool in_solve = (est_index_of[i] >= 0);
            if (in_solve) ++n_in_solve_ep;
            const std::string& nm = stations[i].name;
            auto& tr = cnt.sta_solve_track[nm];
            if (in_solve) {
                if (tr.ever_in_solve && tr.absent_streak > 0) {
                    ++cnt.sta_readmit_events;
                    if (tr.absent_streak > cnt.sta_readmit_max_gap_ep)
                        cnt.sta_readmit_max_gap_ep = tr.absent_streak;
                    if (nc.trace_topology)
                        traces.emitEvent(se.time, "TOPOLOGY", "sta_readmitted", "", nm,
                                         static_cast<double>(tr.absent_streak), std::nullopt,
                                         "readmitted",
                                         fmt::format("epochs_absent={};prior_rows={};"
                                                     "pre_pd_prior={}",
                                                     tr.absent_streak,
                                                     asm_res[i].prior_cols.size(),
                                                     sta_prior_pre_pd ? 1 : 0));
                }
                tr.ever_in_solve  = true;
                tr.absent_streak  = 0;
            } else {
                ++tr.absent_streak;
            }
        }
        ++cnt.sta_in_solve_epochs;
        cnt.sta_in_solve_ep.push_back(n_in_solve_ep);
        if (cnt.sta_in_solve_min < 0 || n_in_solve_ep < cnt.sta_in_solve_min) {
            cnt.sta_in_solve_min     = n_in_solve_ep;
            cnt.sta_in_solve_min_tow = se.time.tow;
        }
    }

    // Chain-pin audit (SPEC_synthetic_minimum_network §14 T1): compare the pins that LANDED
    // — folded by foldAmb/foldRpb AND sitting on a station est.addStation
    // accepted — against the election's declared per-group gauge dimension
    // Σ (n_c + m_c), minus the hygiene bridge pins realized on the shared side.
    // SERIAL, after the consume, so acceptance is known (est_index_of ≥ 0);
    // an aborted epoch (pivot failure) never reaches a solve and is not audited.
    // Log-only run tallies — no solve input is touched.
    if (phase_datum_eq34 && epoch_ok) {
        auto& pa = ctx.counters.eq34_pin_audit;
        ++pa.epochs;
        std::map<std::string, long long> folded_ep, landed_ep;
        for (size_t i = 0; i < stations.size(); ++i) {
            for (const auto& [grp, t] : asm_res[i].pin_audit) {
                folded_ep[grp]     += t[0];
                pa.miss_state[grp] += t[1];
                pa.miss_row[grp]   += t[2];
                if (est_index_of[i] >= 0) landed_ep[grp] += t[0];
            }
            // SEED-ROUND §3.4: feed each ACCEPTED station's fold-realized targets
            // into next epoch's pinmove baseline — without this the election reads
            // the provisional 0 and computes a phantom km-scale step every epoch,
            // permanently re-referencing the (sat,σ).  A dropped station's pin did
            // not land; its arc re-elects from scratch next epoch.
            if (est_index_of[i] >= 0) {
                for (const auto& [amb, tgt, y] : asm_res[i].pin_seed_targets) {
                    ctx.carry.eq34_prev_target[{static_cast<int>(i), amb}] = tgt;
                    ++ctx.counters.seed_round_deferred;
                    const double ay = std::abs(y);
                    if (ay > ctx.counters.seed_round_max_abs_y_m)
                        ctx.counters.seed_round_max_abs_y_m = ay;
                }
            }
        }
        for (const auto& [grp, decl] : ctx.carry.eq34_epoch_declared) {
            const auto bit = ctx.carry.eq34_epoch_bridged.find(grp);
            const long long bridged =
                bit == ctx.carry.eq34_epoch_bridged.end() ? 0 : bit->second;
            pa.declared[grp] += decl;
            pa.bridged[grp]  += bridged;
            pa.folded[grp]   += folded_ep[grp];
            pa.landed[grp]   += landed_ep[grp];
            pa.dropped_station[grp] += folded_ep[grp] - landed_ep[grp];
            const long long short_n = (decl - bridged) - landed_ep[grp];
            if (short_n > 0) {
                ++pa.short_epochs[grp];
                if (static_cast<int>(short_n) > pa.worst_short[grp])
                    pa.worst_short[grp] = static_cast<int>(short_n);
            }
        }
    }

    // D0: close the assembly wall (produce + scatter).  Accumulated even when the
    // epoch skips below — the assembly work was still spent.
    if (sec_timers_on) {
        sec_asm_ms_epoch = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sec_asm_t0).count();
        sec_asm_wall_ms += sec_asm_ms_epoch;
    }

    if (!epoch_ok || est.numShared() == 0) {
        ++n_skipped;
        if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                         << ',' << n_present << ',' << n_clock << ',' << n_pb
                         << ',' << 0 << ",0,0,0,0," << total_pins << '\n';
        { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
          emitEpochStatus("sync_skip", es); }
        prev_week = se.time.week; prev_tow = se.time.tow;
        return EpochAction::SkipEpoch;
    }

    return EpochAction::Proceed;
}

} // namespace GnssProcessor
