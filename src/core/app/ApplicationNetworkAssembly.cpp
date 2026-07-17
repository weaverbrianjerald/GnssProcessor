/**
 * @file ApplicationNetworkAssembly.cpp
 * @brief Assembly-phase member functions of Application::runNetwork — the
 *        pre-loop column-split / iono-row helpers plus (as INC-7 lands) PASS-A
 *        geometry, the observability prefilter, the shared-column
 *        registration, and PASS-B produce/consume.
 *
 * Decomposition INC-7 (2026-07-16) — bodies relocated VERBATIM from
 * ApplicationNetwork.cpp (see docs/network_decomposition_addendum_2026-07-16.md).
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
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/NetworkCoupling.hpp"       // INC-7b: Card-1/1b/1d conditional coupling-row builder
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
                                           const NetworkIonoDeweightGate& iono_deweight,
                                           const GlobalVtecShConfig& shc, bool sh_dev_on,
                                           const std::map<std::string, ShDevCarry>& sh_dev_carry,
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
    const double H  = shc.layer_height_km * 1000.0;
    const double tecu_to_L1m =
        40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
    const double lon_sun = shc.sun_fixed ? meanSunLongitude(t) : 0.0;
    const int K = sh_model->numCoeffs();

    struct SRow {
        int    local_col;        ///< A_r column of this satellite's slant-iono state.
        double innov;            ///< −x(iono_idx).
        double var;              ///< per-LOS representativeness variance (m²).
        double map_tecu;         ///< M(el)·tecu_to_L1m (the STEC scale).
        std::vector<double> basis;  ///< Ȳ_nm(IPP) in coeff order.
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
        const IppGeometry g = computeIppGeometry(st.ecef, o.sat_pos, Re, H);
        if (!g.valid) continue;
        const double lon_sf = g.ipp_lon_rad - lon_sun;
        sh_model->basis(g.ipp_lat_rad, lon_sf, bvals);
        const double sigma =
            std::max(iw.floor_m, shc.representation_sigma_tecu * g.mapping * tecu_to_L1m);
        SRow r;
        r.local_col = hit->second;
        r.innov     = -x(iono_idx);
        r.var       = sigma * sigma;
        // T5: the pin constrains (ι − SH·c) to the PREDICTED deviation at the
        // prediction's variance + the floor, instead of to 0 at the full
        // representativeness σ.  READ-ONLY on the carry (serial pre-pass ages it;
        // a missing key means cold seed: dev 0, P = steady-state σ² — identical
        // information to the untreated row, so cold LOS behave as before).
        if (sh_dev_on) {
            const double sig_ss =
                shc.representation_sigma_tecu * g.mapping * tecu_to_L1m;
            double dev_pred = 0.0;
            double var_pred = sig_ss * sig_ss;
            const auto dit = sh_dev_carry.find(st.name + "|" + o.satellite);
            if (dit != sh_dev_carry.end() && dit->second.t_s > 0.0) {
                dev_pred = dit->second.dev_m;
                var_pred = dit->second.var_m2;
            }
            r.innov = -x(iono_idx) + dev_pred;
            r.var   = iw.floor_m * iw.floor_m + var_pred;
        }
        r.map_tecu  = g.mapping * tecu_to_L1m;
        r.basis     = bvals;
        by_const[o.satellite.front()].push_back(std::move(r));
    }

    int total = 0;
    for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
    if (total == 0) return;

    // SH column indices (shared border), looked up once per call.
    std::vector<Eigen::Index> sh_cols(static_cast<std::size_t>(K));
    for (int k = 0; k < K; ++k) {
        const auto& c = sh_model->coeffs()[static_cast<std::size_t>(k)];
        sh_cols[static_cast<std::size_t>(k)] = est.vtecCoeffColumn(c.n, c.m, c.trig);
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
        Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, nshared);   // −M·tecu·basis (SH cols)
        Eigen::VectorXd zc(mc);
        for (int k = 0; k < mc; ++k) {
            Hr(k, rws[k].local_col) = 1.0;
            for (int j = 0; j < K; ++j)
                Hs(k, sh_cols[static_cast<std::size_t>(j)]) =
                    -rws[k].map_tecu * rws[k].basis[static_cast<std::size_t>(j)];
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
        piv.mm->ensureNetworkSharedStates(modeled.front());  // allow all pivot phase signals
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
                                        std::set<std::pair<int, std::string>>& carry_slipped_ambs) {
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
                st.mm->ensureNetworkSharedStates(modeled[i]);
            else
                st.mm->ensureNetworkSharedStates(modeled[i], shared_phase_keys);
            a_ok[i] = 1;

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
                        const double frac_max = config_.networkCycleSlipTurboEditFracMax();
                        for (const auto& r : mwr) {
                            if (r.slip_wl_known && r.slip_wl_cycles != 0
                                && r.slip_frac <= frac_max
                                && !r.sig1.empty() && !r.sig2.empty())
                                sta_mw_refloat[i].emplace_back(r.satellite, r.sig1, r.sig2);
                        }
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

    // Serial slip-QC drains (INC-6): bodies relocated VERBATIM to
    // slip/NetworkSlipQc.cpp — phase-leveling T0 repair drain, MW-detect /
    // LLI refloat, IF-phase TD driver, in the original order.
    networkSlipDrainT0Repairs(pa, config_, stations, present, i_lo, n_sta_total,
                              local_carry, se, log);

    networkSlipDrainMwRefloat(pa, config_, stations, present, modeled, i_lo,
                              n_sta_total, carry_invariant, carry_warm,
                              carry_slipped_ambs, se, traces, log);

    networkSlipIfTdDriver(pa, config_, stations, present, i_lo, n_sta_total,
                          carry_invariant, carry_warm, carry_slipped_ambs,
                          trace_flag_rates, fr_if_slip, se, traces, log);
}

void Application::networkAssemblyPrefilter_(const SyncedEpoch& se,
                                            NetworkSetupResult& setup,
                                            NetworkRunTraces& traces,
                                            const std::unique_ptr<PppUtils>& slip_raw_ppp_utils,
                                            std::vector<std::vector<PppObservation>>& modeled,
                                            const std::vector<bool>& present,
                                            int& n_dropped_sats) {
    // Alias block (INC-7 migration mechanic).
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    auto& slip_raw_trace       = traces.slip_raw_trace;

    // RAW cycle-slip observables (network.trace_slip_raw), computed straight from
    // raw_observed — INDEPENDENT of whether any slip detector is armed.  Written BEFORE
    // the observability pre-filter so a satellite about to be dropped is still captured.
    // For each present station+satellite pick the primary dual-frequency pair (two lowest
    // DISTINCT bands whose phase AND matching code "C"+sig[1:] are present) and emit MW,
    // GF (=L1−L2), first-signal phase-minus-code (all m) plus the per-band RINEX LLI.  The
    // factor c cancels out of MW/GF, so wavelengths alone (FDMA-correct via the satellite)
    // suffice.  Absolute values — diff adjacent epochs offline for the slip signature.
    if (slip_raw_trace.is_open() && slip_raw_ppp_utils) {
        for (int i = 0; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            for (const auto& obs : modeled[i]) {
                std::vector<std::string> ph;  // phase signals with a matching code
                for (const auto& kv : obs.raw_observed) {
                    const std::string& k = kv.first;
                    if (k.size() < 2 || k[0] != 'L') continue;
                    if (obs.raw_observed.count("C" + k.substr(1))) ph.push_back(k);
                }
                if (ph.empty()) continue;
                std::sort(ph.begin(), ph.end(),
                          [](const std::string& a, const std::string& b) { return a[1] < b[1]; });
                const std::string s1 = ph.front();
                std::string s2;
                for (const auto& k : ph) if (k[1] != s1[1]) { s2 = k; break; }  // distinct band

                const double lam1 = slip_raw_ppp_utils->getWavelength(obs.system, s1, obs.satellite);
                if (!(std::isfinite(lam1) && lam1 > 0.0)) continue;
                const double L1_cyc = obs.raw_observed.at(s1);
                const double P1_m   = obs.raw_observed.at("C" + s1.substr(1));
                const double L1_m   = L1_cyc * lam1;
                const double l1c1   = L1_m - P1_m;
                const int    lli1   = obs.lli_flags.count(s1) ? obs.lli_flags.at(s1) : 0;

                bool dual = !s2.empty();
                double mw = 0.0, gf = 0.0;
                int    lli2 = 0;
                if (dual) {
                    const double lam2 = slip_raw_ppp_utils->getWavelength(obs.system, s2, obs.satellite);
                    if (std::isfinite(lam2) && lam2 > 0.0) {
                        const double inv1 = 1.0 / lam1, inv2 = 1.0 / lam2;
                        const double L2_cyc = obs.raw_observed.at(s2);
                        const double P2_m   = obs.raw_observed.at("C" + s2.substr(1));
                        gf = L1_m - L2_cyc * lam2;
                        mw = (L1_cyc - L2_cyc) / (inv1 - inv2)
                           - (P1_m * inv1 + P2_m * inv2) / (inv1 + inv2);
                        lli2 = obs.lli_flags.count(s2) ? obs.lli_flags.at(s2) : 0;
                    } else {
                        dual = false;
                    }
                }
                slip_raw_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << stations[i].name << ',' << obs.satellite << ','
                    << s1 << ',' << (dual ? s2 : std::string{}) << ','
                    << fmt::format("{:.2f}", obs.elevation) << ','
                    << (dual ? fmt::format("{:.4f}", mw) : std::string{}) << ','
                    << (dual ? fmt::format("{:.4f}", gf) : std::string{}) << ','
                    << fmt::format("{:.4f}", l1c1) << ',' << lli1 << ',' << lli2 << '\n';
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
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (parts) est.registerPhaseBias(parts->satellite, parts->signal);
    }
    // Register the satellite CODE-bias (OSB) columns (W6, full multi-band):
    // EVERY (sat,signal) — every frequency band AND every tracking channel
    // (attribute) — that at least two present stations carry an OSB state for.
    // Collected (deduplicated, first-seen order) for the code-bias datum
    // constraints, the D2 carry-forward, and the solution trace.  Empty unless
    // estimate_sat_code_bias.
    //
    // The ONLY admission gate is >=2-station coverage: a single-station code
    // bias is perfectly confounded with that one station's receiver code bias
    // and cannot be separated, so it stays a local nuisance (its sat-bias state
    // is pruned below, leaving its raw code row to fold into the receiver
    // clock / slant iono — the pre-W6 path).  Everything with >=2 stations is a
    // first-class shared OSB column and is datumed below: the GIM between-
    // satellite iono structure makes the inter-band DCBs observable, while
    // same-band channels (C1C vs C1W, C2W vs C2L, ...) need no iono at all
    // (equal frequency → the receiver and ionosphere terms cancel in the raw
    // between-channel difference, leaving the satellite inter-channel bias).
    // The earlier slice confined the OSB to the two lowest bands and so emitted
    // nothing on a 3rd band; the datum below now pins every registered column.
    if (config_.estimateNetworkSatCodeBias()) {
        std::map<std::string, int> cb_station_count;   // "sat|signal" -> #stations
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatBiasStates())
                ++cb_station_count[kv.first];
        }
        std::set<std::string> keep_keys;
        for (const auto& [key, cnt] : cb_station_count)
            if (cnt >= 2) keep_keys.insert(key);

        std::set<std::string> cb_seen;
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
    }

    // Register the global SH-VTEC coefficient columns (estimate_iono_sh).  The
    // layout is FIXED — all (N+1)² coefficients every epoch, independent of the
    // satellite set — so the field is continuous across epochs and the Kaula
    // prior can keep every coefficient soft-pinned (the rank gate then stays at
    // #constellations regardless of coverage; design §6.1).
    if (config_.estimateNetworkIonoSh() && sh_model) {
        for (const auto& c : sh_model->coeffs())
            est.registerVtecCoeff(c.n, c.m, c.trig);
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
            for (const auto& c : sh_model->coeffs())
                col_desc[static_cast<size_t>(est.vtecCoeffColumn(c.n, c.m, c.trig))] =
                    fmt::format("SH|{}|{}|{}", c.n, c.m, c.trig);
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
                                       double sh_dev_tau, bool sh_dev_on,
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
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // Card 1b: JOINTLY whiten the coupling rows (departed cols → M_w) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // Card 1d: border time update of the coupling rows at injection (κ shrink + M²κq inflation) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_shmarg = ctx.knobs.carry_cpl_shmarg; // Card 1d-SH: fully marginalize VtecSh coupling terms (GM border ≠ RW κ model — severs the amb↔SH carry feedback) — requires carry_cpl_cond (enforced at load)
    const bool   zwd_on        = ctx.knobs.zwd_on;                 // B9: carry per-station ZWD across epochs
    const double zwd_q         = ctx.knobs.zwd_q;                // B9: ZWD carry random-walk process noise (m²/s)
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;  // >0: inflate-q to this σ; ≤0: hard skip-stash refloat
    const bool carry_invariant         = ctx.knobs.carry_invariant;
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
    auto& rpb_carry = ctx.carry.rpb_carry;
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
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

    // T5: age the SH-deviation carries ONCE per epoch (SERIAL — the parallel
    // assembly below only reads).  GM prediction: dev *= λ, P → λ²P + σ_ss²(1−λ²);
    // entries unseen for > 5τ are dropped (fully decorrelated — the cold seed the
    // pin then uses is statistically equivalent).
    if (sh_dev_on) {
        const double now_s = se.time.week * 604800.0 + se.time.tow;
        for (auto it = sh_dev_carry.begin(); it != sh_dev_carry.end();) {
            ShDevCarry& dc = it->second;
            const double dt = now_s - dc.t_s;
            if (dc.t_s <= 0.0 || dt > 5.0 * sh_dev_tau) {
                it = sh_dev_carry.erase(it);
                continue;
            }
            if (dt > 0.0) {
                const double lam = std::exp(-dt / sh_dev_tau);
                dc.dev_m  *= lam;
                dc.var_m2  = lam * lam * dc.var_m2 + dc.var_ss * (1.0 - lam * lam);
            }
            ++it;
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
    if (carry_cpl_tu) {
        for (const CarryPrior& cp : prev_carry)
            if (cp.inflate_q)
                tu_reinit_hist[{cp.kind, cp.sat}] = pl_now_s;
        for (const std::string& sat : prev_clock_refloat_sats) {
            tu_reinit_hist[{SharedParamKind::Clock, sat}] = pl_now_s;
            if (!(clk_mon_reinit_sig > 0.0)) {
                tu_reinit_hist[{SharedParamKind::PhaseBias, sat}] = pl_now_s;
                tu_reinit_hist[{SharedParamKind::CodeBias, sat}] = pl_now_s;
            }
        }
    }

    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
    for (int i = 0; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        StationAssembly& R = asm_res[i];
        try {
            NetStation& st = stations[i];

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
                auto foldAmb = [&](const std::string& amb_key, double target) {
                    const int amb_idx = st.sm->getAmbiguityIndex(amb_key);
                    if (amb_idx < 0) return;   // no usable phase row for it this epoch
                    const auto hit = h2local.find(amb_idx);
                    if (hit == h2local.end()) return;  // column dropped (all-zero H)
                    pin_col.push_back(hit->second);
                    // Preserve the exact legacy expression (−piv_x) when pinning
                    // to 0 — "0.0 − x" flips the sign of a zero x, which would
                    // perturb the OFF golden hash; a nonzero eq34 re-pin target
                    // uses the general form.
                    pin_y.push_back(target == 0.0 ? -piv_x(amb_idx)
                                                  : target - piv_x(amb_idx));
                    pin_w.push_back(1e10);   // hard pin (exact gauge)
                };
                // eq34: r1's RPB pin — δ_{r1,σ}=0 on the RPB column in this
                // station's OWN block (the receiver half of the chain gauge).
                auto foldRpb = [&](const std::string& rpb_key) {
                    const int rpb_idx = st.sm->getReceiverPhaseBiasIndex(rpb_key);
                    if (rpb_idx < 0) return;
                    const auto hit = h2local.find(rpb_idx);
                    if (hit == h2local.end()) return;  // no phase row on σ this epoch
                    pin_col.push_back(hit->second);
                    pin_y.push_back(-piv_x(rpb_idx));
                    pin_w.push_back(1e10);   // hard pin (exact gauge)
                };
                if (phase_datum_prioritized) {
                    for (const auto& [amb_key, target] : st.pinned_amb_targets)
                        foldAmb(amb_key, target);
                    for (const auto& rpb_key : st.pinned_rpb_keys)
                        foldRpb(rpb_key);
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
                                         sh_model, iono_deweight, shc, sh_dev_on,
                                         sh_dev_carry, R.A_s, R.A_r, R.w, R.y);
            else if (config_.estimateNetworkSatCodeBias())
                networkAppendGimIonoRows_(st, modeled[i], h2local, se.time,
                                          R.A_s, R.A_r, R.w, R.y);

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
            if (carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on)) {
                const Eigen::VectorXd x_lin = st.sm->getStateVector();
                std::vector<std::tuple<Eigen::Index, double, double>> inj;  // (col, Λ, η) — legacy diagonal path
                // Card 1 (network.carry_conditional_coupling): each carried local column
                // becomes ONE pseudo-observation ROW coupling it to the FREE shared border
                // (Odijk S-conditional carry) instead of an independent diagonal prior — see
                // filter/NetworkCoupling.hpp.  The former loop-body scratch + the six [&]
                // lambdas are CouplingRowBuilder members; built fresh per station (no shared
                // write, safe in the parallel produce phase), appended after the carry loops.
                CouplingRowBuilder cb{est, shc, q_clock, q_cb, q_pb,
                                      carry_cpl_joint, carry_cpl_tu, carry_cpl_shmarg,
                                      tu_reinit_hist, R};
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
                if (do_amb_carry)
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

                    // A pending re-seed marks the prior arc value as no longer
                    // trustworthy (the network's analogue of a slip): re-float from
                    // data this epoch rather than clamp to the stale carry.  The
                    // stash below then captures the fresh value.  (Slip-driven
                    // REFLOAT/DROP proper is Increment 2+; this is the cheap guard
                    // for the only reset trigger the network can raise today.)
                    if (st.sm->isAmbiguityPendingReseed(key)) continue;
                    if (!std::isfinite(x_lin(amb_idx))) continue;  // non-finite state → don't inject

                    const auto cit = local_carry.find(
                        std::make_tuple(i, parts->satellite, parts->signal));
                    if (cit == local_carry.end()) continue;       // no carry yet (new arc)
                    const double gap = pl_now_s - cit->second.last_seen_s;
                    if (gap > pl_max_gap_s) continue;             // stale — housekeeping will drop it
                    // var_eff = var + q·gap, floored by the optional lock-in guard
                    // (var_floor>0 caps Λ so a slowly-biasing ambiguity can never
                    // become a rigid prior the data cannot override).  Only present
                    // (data-bearing) columns are injected, so a tiny Λ never leaves a
                    // column PD-held by the prior alone — the §3.4 floor is not
                    // load-bearing here; this guards the OPPOSITE (lock-in) direction.
                    double var_eff = cit->second.var + pl_q_amb * std::max(0.0, gap);
                    if (pl_var_floor > 0.0) var_eff = std::max(var_eff, pl_var_floor);
                    if (!(var_eff > 0.0)) continue;
                    if (carry_cpl_cond) {
                        cb.buildCplRow(col, x_lin(amb_idx), cit->second, var_eff,
                                    std::max(0.0, gap));
                        continue;
                    }
                    const double lambda = 1.0 / var_eff;
                    const double eta = lambda * (cit->second.value_abs - x_lin(amb_idx));
                    if (!std::isfinite(eta)) continue;            // belt-and-suspenders: never poison u_r
                    inj.emplace_back(col, lambda, eta);
                }
                // --- ZWD carry (B9): treat the tropo ZWD column like one more carried
                //     local state (random WALK, not random constant → positive q).  The
                //     prior tuple joins `inj` so it is ordered by the same ascending sort
                //     and folded by the single addStationPrior inject (byte-identical OFF).
                if (zwd_on) {
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
                                if (zit != zwd_carry.end()) {
                                    const double zgap = pl_now_s - zit->second.last_seen_s;
                                    if (zgap <= pl_max_gap_s) {
                                        const double zvar_eff =
                                            zit->second.var + zwd_q * std::max(0.0, zgap);
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
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (carry_cpl_cond) {
                    // Card 1/1b: batch-append this station's conditional coupling rows into
                    // R.A_s/R.A_r (joint-whitened when carry_cpl_joint, else scalar per-row);
                    // R.carry_cols stays EMPTY so the serial addStationPrior is skipped.
                    cb.appendCouplingRows();
                } else {
                    // ASCENDING column order → deterministic rank-1 update sequence.
                    std::sort(inj.begin(), inj.end(),
                              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                    const Eigen::Index ninj = static_cast<Eigen::Index>(inj.size());
                    R.carry_cols.resize(static_cast<size_t>(ninj));
                    R.carry_dNrr.resize(ninj);
                    R.carry_dur.resize(ninj);
                    for (Eigen::Index k = 0; k < ninj; ++k) {
                        R.carry_cols[static_cast<size_t>(k)] = std::get<0>(inj[static_cast<size_t>(k)]);
                        R.carry_dNrr(k) = std::get<1>(inj[static_cast<size_t>(k)]);
                        R.carry_dur(k)  = std::get<2>(inj[static_cast<size_t>(k)]);
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
    foldAndDumpCouplingRows(carry_cpl_cond, asm_res, est,
                            registered_clock_sats, shared_phase_keys, registered_code_keys,
                            se.time.tow, log,
                            n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                            n_cpl_row_dump);

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
            est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
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
                epoch_ok = false;
                break;
            }
            log->warn("Network: station '{}' addStation rejected ({}); dropping it this epoch",
                      st.name, ex.what());
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
        if (carry_warm && !R.carry_cols.empty()) {
            try {
                est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                    R.carry_cols, R.carry_dNrr, R.carry_dur);
            } catch (const std::exception& ex) {
                log->warn("Network[phase-leveling]: station '{}' carry inject rejected "
                          "({}); proceeding without its carry this epoch", st.name, ex.what());
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
