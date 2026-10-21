/**
 * @file IonoConstraintRows.cpp
 * @brief The ionosphere constraint rows: the SH-VTEC tie and the external GIM rows.
 */

#include "network/assembly/IonoConstraintRows.hpp"
#include "network/assembly/StationBuild.hpp"   // StationBuild, StationCarryFrame
#include "network/driver/Epoch.hpp"
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/StationParams.hpp"   // the station's parameter set: the slant ionosphere columns
#include "network/assembly/Contributions.hpp"
#include "network/carry/Coupling.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "rinex/MultiRinexSync.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "rinex/MultiRinexSync.hpp"
#include "network/solve/Estimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR: ils::ilsFix
#include "network/solve/Sbasis.hpp"
#include "network/assembly/ExtraBandScope.hpp"  // the extra-band / primary-pair predicates
#include "network/assembly/SecondCodeScope.hpp" // secondCodeCellEdges, the two cell edges
#include "network/assembly/SecondPhaseScope.hpp" // the cell edges + the witness
#include "network/solve/PivotHandover.hpp"   // pivot-handover datum-change detector
#include "network/carry/CarryVce.hpp"        // LS-VCE carry-row variance-component monitor
#include "network/carry/InvariantCarryDriver.hpp"  // DD cycle-basis invariant carry
#include "network/ar/SdEwlAr.hpp"               // the shared EWL band table (sdEwlBands) and the SD EWL integer decision
#include "network/carry/Coupling.hpp"       // conditional coupling-row builder
#include "network/assembly/ParamInventory.hpp" // the parameter inventory and the receiver IFB rule
#include "network/assembly/Contributions.hpp" // row tags and the contribution tallies
#include "network/qc/PrefitCensus.hpp"   // prefit innovation census
#include "network/diag/ShDevVtec.hpp"     // networkShDevCarryAge
#include "network/assembly/StationBuild.hpp"  // networkAssemblyTally
#include "network/solve/SolveStations.hpp" // the consume and the in-solve track
#include "network/solve/ChainDatum.hpp"    // networkEq34PinAudit
#include "network/ar/ArSuspect.hpp"      // arSuspectNote: the AR search's suspect-corner stamps
#include "network/driver/RunTypes.hpp"      // run-loop record types: NetStation / StationAssembly / carry PODs
#include "network/qc/RobustStats.hpp"   // median/MAD robust attribution helpers
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs
#include "network/diag/RunTraces.hpp" // trace-stream ownership + open logic + roster + FlagRate
#include "network/state/RunContext.hpp"    // ctx: knobs + carry state + service counters
#include "network/driver/SetupResult.hpp"   // setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair
#include "slip/TurboEditResolver.hpp"   // per-signal MW+GF integer repair
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "network/slip/SlipQc.hpp"       // slip-QC pure helpers: ifElevMap / parseIfPairResetKey
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"    // identity reads (systemOfSat, ObservableId, ...), carriers
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"
#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads: Eigen is pinned to 1 thread inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic
#include <fmt/format.h>
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
#include <numeric>             // std::iota (network AR ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun

namespace GnssProcessor {

// ---- PASS B station steps ----

StationStep networkIonoConstraintRows(StationBuild& build) {
    // ---- From the station build ----
    const NetworkRunContext& ctx = build.ctx;
    const GnssConfig& config_ = build.config;
    const NetworkEstimator& est = build.est;
    const NetworkSetupResult& setup = build.setup;
    const SyncedEpoch& se = build.se;
    const std::vector<std::vector<PppObservation>>& modeled = build.pass_a.modeled;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool   sh_dev_on      = ctx.knobs.sh_dev_on;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& sh_model = setup.sh_model;
    const net::LosTable& los_rows = ctx.entities.los;   // the SH deviation carries (read only: parallel produce)
    // ---- End of the bindings ----

    // Ionosphere constraint: the between-satellite structure that makes
    // the satellite DCB observable against the free slant iono.  The
    // co-estimated SH-VTEC field (the network's own iono) when enabled,
    // otherwise the external GIM rows when the satellite code bias is estimated,
    // folded into the station's own block (and the shared SH border).  The GIM
    // builder appends rows only with a GIM loaded and the ionosphere weighting on:
    // under network.w6_iono_constraint none (the goldens) the setup builds no GIM
    // and the loader forces the weighting off, so the call returns with nothing
    // appended.  The contributions table counts its rows (gim_iono) and its calls by
    // outcome.
    if (config_.estimateNetworkIonoSh())
        networkAppendShIonoRows(config_, st, est, modeled[i], h2local, se.time,
                                 sh_model, setup.sh_model2,
                                 shc, sh_dev_on, los_rows, static_cast<int>(i),
                                 R.A_s, R.A_r, R.w, R.y, R.row_tags);
    else if (config_.estimateNetworkSatCodeBias())
        R.gim_rows = networkAppendGimIonoRows(config_, build.gim, st, modeled[i], h2local, se.time,
                                               R.A_s, R.A_r, R.w, R.y, R.row_tags);
    return StationStep::Next;
}

// GIM ionosphere pseudo-observation (prerequisite of the satellite code bias).
// Ports Application::applyGimIonoUpdate's common-mode-free R into tight-prior
// rows folded into a station's own block: one row per non-GLONASS satellite
// above the weighting elevation cutoff with a usable slant-iono column and a
// valid GIM value, with a per-constellation R = diag(var) + σ_c²·11ᵀ.  The
// all-ones block leaves the per-constellation common mode (the receiver
// iono-datum level) effectively free (σ_c = iw.common_mode_sigma_m, default
// 50 m) and imposes the GIM's between-satellite structure, which separates the
// satellite DCB from the slant iono while leaving the constellation-wide DCB
// level to the explicit sum-to-zero datum.  The correlated R is whitened per
// constellation (L⁻¹ from LLᵀ = R) into unit-information rows, so it stays in
// the station's diagonal-weight block.  GLONASS is handled by its own loose
// anchors in PASS B (no satellite code bias).  The rows go into the station's
// own block with no shared write, so the function is safe in the parallel
// produce phase (the GIM query is const).
GimRowsOutcome networkAppendGimIonoRows(const GnssConfig& config_,
                                        const std::unique_ptr<GlobalIonosphereMap>& gim_,
                                        NetStation& st, const std::vector<PppObservation>& sta_obs,
                                        const std::map<int, int>& h2local, const GnssTime& t,
                                        Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                        Eigen::VectorXd& w, Eigen::VectorXd& y,
                                        std::vector<RowTag>& tags) {
    if (!gim_ || !gim_->isLoaded()) return GimRowsOutcome::NoGim;
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled) return GimRowsOutcome::WeightingOff;

    struct GRow { int col; double innov; double var; std::string sat; };
    std::map<char, std::vector<GRow>> by_const;   // constellation char -> rows
    for (const auto& o : sta_obs) {
        // Every system but GLONASS.
        if (o.satellite.empty() || systemOfSat(o.satellite) == System::GLONASS) continue;
        if (o.elevation < iw.elevation_cutoff_deg) continue;
        const int iono_idx = findSlantIono(st.params, o.satellite);
        if (iono_idx < 0) continue;
        const auto hit = h2local.find(iono_idx);
        if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
        const GimQuery q =
            gim_->slantIonoDelay(t, st.ecef, o.sat_pos,
                                 nominalCarrierFrequencyHz(FrequencyBand::GpsL1));
        if (!q.valid) continue;
        const double sigma = std::max(iw.floor_m, iw.std_scale * q.std_m);
        by_const[letterOfSat(o.satellite)].push_back(
            GRow{hit->second, q.delay_m - slotAt(st.params, iono_idx).x_lin, sigma * sigma,
                 o.satellite});
    }

    int total = 0;
    for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
    if (total == 0) return GimRowsOutcome::NoRows;

    const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
    const Eigen::Index m0     = A_s.rows();
    const Eigen::Index ncol_r = A_r.cols();
    networkGrowBlockRows(A_s, A_r, w, y, total);

    Eigen::Index out = m0;
    for (auto& kv : by_const) {
        const std::vector<GRow>& rws = kv.second;
        const int mc = static_cast<int>(rws.size());
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
        for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
        Eigen::MatrixXd Hc = Eigen::MatrixXd::Zero(mc, ncol_r);
        Eigen::VectorXd zc(mc);
        for (int k = 0; k < mc; ++k) { Hc(k, rws[k].col) = 1.0; zc(k) = rws[k].innov; }
        // Whiten by L⁻¹ (LLᵀ = R) into unit-information rows that contribute exactly
        // Hᵀ R⁻¹ H / Hᵀ R⁻¹ z to the station normals.
        networkWriteWhitenedRows(R, Hc, nullptr, zc, out, A_s, A_r, w, y);
        for (int k = 0; k < mc; ++k)   // one tag per row, in row order
            tags.push_back(makeRowTag(ContributionKind::GimIono, rws[k].sat));
        out += mc;
    }
    return GimRowsOutcome::Appended;
}

// Co-estimated SH-VTEC ionosphere pseudo-observation: the network's own
// spherical-harmonic field replaces the external-GIM target of networkAppendGimIonoRows.
// For each non-GLONASS satellite with a usable slant-iono column it folds one row
// asserting  I_local(rx,sat) = M(el)·VTEC_SH(IPP) :
//   H:  +1 on the local slant-iono column (A_r, as in the GIM row), and
//      −M(el)·tecu_to_L1m·Ȳ_nm(IPP) on each shared SH coefficient column (A_s);
//      that shared write is what turns the prior into a joint estimate.
// The innovation is −x(iono_idx), plus the predicted deviation when sh_dev_on (the
// field at coeff 0 is 0; absolute solve).  The common-mode-free per-constellation
// R = diag(var)+σ_c²·11ᵀ is reused unchanged: its all-ones block leaves the
// per-(rx,const) iono datum level free (the receiver DCB is absorbed there), so the
// field constrains only the between-satellite slant-iono structure.  a₀₀ (global
// mean) is observable only weakly, through the mapping-function spread, and is
// otherwise held by its loose Kaula prior (data-determined mean, not clamped to an
// external value).  GLONASS is excluded from the field (the FDMA per-slot IFB would
// leak into the global field).  Rows go into the station's own block and
// the shared border; the SH column lookups are const, so the function is safe in
// the parallel produce phase.
void networkAppendShIonoRows(const GnssConfig& config_, NetStation& st, const NetworkEstimator& est,
                             const std::vector<PppObservation>& sta_obs,
                             const std::map<int, int>& h2local, const GnssTime& t,
                             const std::unique_ptr<SphericalHarmonicIonosphere>& sh_model,
                             const std::unique_ptr<SphericalHarmonicIonosphere>& sh_model2,
                             const GlobalVtecShConfig& shc, bool sh_dev_on,
                             const net::LosTable& sh_dev_los, int sta_index,
                             Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                             Eigen::VectorXd& y, std::vector<RowTag>& tags) {
    if (!sh_model) return;
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled) return;
    const double Re = shc.earth_radius_km * 1000.0;
    const double tecu_to_L1m =
        ionoDelayMetresPerTecu(nominalCarrierFrequencyHz(FrequencyBand::GpsL1));
    const double lon_sun = shc.sun_fixed ? meanSunLongitude(t) : 0.0;
    // Dual layer: one row still asserts one slant constraint, but the field
    // side sums the shells, ι = Σ_l M_l(el)·tecu·SH_l(IPP_l)·c_l (+ d).  Each shell
    // has its own pierce geometry (a different H gives a different mapping and a
    // different IPP lat/lon); the single-layer path is the one-entry list
    // (byte-identical).  The representativeness σ and the deviation's
    // vertical-to-slant conversion use the layer-0 mapping (the dominant shell).
    const std::vector<ShLayer> layers = shLayers(shc, sh_model.get(),
                                                 sh_model2 ? sh_model2.get() : nullptr);
    const std::size_t n_lay = layers.size();

    struct SRow {
        int    local_col;        ///< A_r column of this satellite's slant-iono state.
        double innov;            ///< −x(iono_idx) (+ predicted deviation when sh_dev_on).
        double var;              ///< per-LOS representativeness variance (m²).
        std::vector<double> map_tecu;             ///< Per layer: M_l(el)·tecu_to_L1m.
        std::vector<std::vector<double>> basis;   ///< Per layer: Ȳ_nm(IPP_l) in coeff order.
        std::string sat;                          ///< The line of sight's satellite (the row's tag).
    };
    std::map<char, std::vector<SRow>> by_const;
    std::vector<double> bvals;
    for (const auto& o : sta_obs) {
        // Every system but GLONASS.
        if (o.satellite.empty() || systemOfSat(o.satellite) == System::GLONASS) continue;
        if (o.elevation < iw.elevation_cutoff_deg) continue;
        const int iono_idx = findSlantIono(st.params, o.satellite);
        if (iono_idx < 0) continue;
        const auto hit = h2local.find(iono_idx);
        if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
        const IppGeometry g0 =
            computeIppGeometry(st.ecef, o.sat_pos, Re, layers[0].shell_height_m);
        if (!g0.valid) continue;
        const double m0_tecu = g0.mapping * tecu_to_L1m;   // layer-0 STEC scale
        // Literal legacy product chain (rep·map)·tecu, not rep·m0_tecu: floating-point
        // multiplication is not associative, and the regrouped form moves the solve
        // checksum at 1e-14, which breaks off-path byte-identity.
        const double sigma = std::max(
            iw.floor_m, shc.representation_sigma_tecu * g0.mapping * tecu_to_L1m);
        SRow r;
        r.local_col = hit->second;
        r.sat       = o.satellite;
        r.innov     = -slotAt(st.params, iono_idx).x_lin;
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
        // With sh_dev_on, the pin constrains (ι − Σ_l SH_l·c_l) to the predicted deviation at
        // the prediction's variance plus the floor, instead of to 0 at the full
        // representativeness σ.  The carry is in vertical TECU (elevation-stationary);
        // the epoch's layer-0 mapping converts it to this row's slant frame.  The carry
        // is read-only here (the serial pre-pass ages and slip-erases it).  A missing
        // key (or t_s = 0) means a cold seed: dev 0 and P = steady-state σ², close to
        // the untreated row's information (floor² + σ² against max(floor, σ)²), so a
        // cold LOS behaves almost as it does without the deviation carry.
        if (sh_dev_on) {
            const double sig_ss_slant =
                shc.representation_sigma_tecu * g0.mapping * tecu_to_L1m;
            double dev_pred_slant = 0.0;
            double var_pred_slant = sig_ss_slant * sig_ss_slant;
            const ShDevCarry* dit =
                net::getLos(sh_dev_los, sta_index, o.satellite, &net::LosState::sh_dev);
            if (dit != nullptr && dit->t_s > 0.0) {
                dev_pred_slant = dit->dev * m0_tecu;
                var_pred_slant = dit->var * m0_tecu * m0_tecu;
            }
            r.innov = -slotAt(st.params, iono_idx).x_lin + dev_pred_slant;
            r.var   = iw.floor_m * iw.floor_m + var_pred_slant;
        }
        by_const[letterOfSat(o.satellite)].push_back(std::move(r));
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
    networkGrowBlockRows(A_s, A_r, w, y, total);

    Eigen::Index out = m0;
    for (auto& kv : by_const) {
        const std::vector<SRow>& rws = kv.second;
        const int mc = static_cast<int>(rws.size());
        Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
        for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
        // Temporal treatment of the quasi-static SH constraint.  Re-applying the
        // (sun-fixed, quasi-static) SH↔slant pseudo-obs as an independent white row
        // every epoch counts the same information again each epoch, so the formal σ
        // collapses without new information.  The row variance is inflated by
        // N = row_decimation_epochs (σ ×√N), so each epoch's white row carries 1/N of
        // the information: the rate of "one full look per N epochs", without
        // withholding.  Skipping rows instead removes the satellite code-bias
        // observability the SH rows provide, and the observability gate then withholds
        // every skipped epoch (an A/B at N=3 withheld 6 of 10 epochs), so σ-inflation
        // is the mechanism that preserves observability.  N ≤ 1: no scaling,
        // byte-identical.
        if (shc.row_decimation_epochs >= 2)
            R *= static_cast<double>(shc.row_decimation_epochs);
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
        // Whiten by L⁻¹ (LLᵀ = R) into unit-information rows; the local +1, the SH
        // partials and the innovation are whitened by the same factor, so the
        // common-mode-free structure carries through to both blocks identically.
        networkWriteWhitenedRows(R, Hr, &Hs, zc, out, A_s, A_r, w, y);
        for (int k = 0; k < mc; ++k)   // one tag per row, in row order
            tags.push_back(makeRowTag(ContributionKind::ShIonoTie, rws[k].sat));
        out += mc;
    }
}

// ---- End of the PASS B station steps ----

} // namespace GnssProcessor
