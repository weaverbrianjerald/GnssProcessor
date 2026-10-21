/**
 * @file StationBuild.cpp
 * @brief PASS B's station build.
 */

#include "network/assembly/StationBuild.hpp"
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <exception>
#include "network/carry/Coupling.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/assembly/Responses.hpp"
#include <Eigen/Dense>   // LLT in networkWriteWhitenedRows
#include "network/slip/OraclePin.hpp"
#include "network/qc/Congruence.hpp"
#include "network/assembly/IonoConstraintRows.hpp"
#include "network/carry/LevelingCarry.hpp"
#include "network/carry/PhaseColourCarry.hpp"
#include "network/solve/PhaseDatum.hpp"
#include "network/qc/Raim.hpp"
#include "network/carry/RxClockCarry.hpp"
#include "network/carry/RxCodeBiasCarry.hpp"
#include "network/ar/SdEwlAr.hpp"
#include "network/carry/SlantIonoCarry.hpp"
#include "network/assembly/StationAnchors.hpp"
#include "network/assembly/StationCarry.hpp"
#include "network/assembly/StationSplit.hpp"
#include "network/carry/ZwdCarry.hpp"
#include "common/EpochMeasurement.hpp"

namespace GnssProcessor {

// ---- PASS B serial stages ----

void networkAssemblyTally(NetworkRun& run, const NetworkEpoch& ep,
                          const EpochAssembly& assembly) {
    // ---- From the run and the epoch (reads bind through `ep`; writes are marked `// written`) ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    double pl_now_s = ep.clock.pl_now_s;
    const std::vector<bool>& present = ep.pass_a.present;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys =
        ep.columns.registered_code_keys;
    const std::vector<StationAssembly>& asm_res = assembly.asm_res;
    // ---- End of the bindings ----
    auto& stations = setup.stations;

    // The produce wrote only each station's own state and slot; the run's counters, tables
    // and traces are folded here, serially and in station order, one family at a time. Each
    // family's fold is a tally function in the file of the family.
    networkAmbiguityFreeEpochTally(ctx, asm_res);
    // The stations' parameter inventories, folded serially (the same
    // discipline); networkParamInventoryEpochTail writes the table after the first
    // emitted epoch and the teardown writes the run's.  `present` tells a roster station
    // with no usable observations from one that was assembled but never reached its split.
    networkParamInventoryDrain(ctx.param_inventory, asm_res, present, se.time.week, se.time.tow);
    networkResponsesStationTally(ctx, asm_res);
    networkOraclePinStationTally(ctx, asm_res);
    networkAmbiguityStaleSkipTally(ctx, asm_res, se);
    // A non-SPD block under float_kf is a vehicle change on one station-epoch: the arc
    // reverts to the per-column route the vehicle exists to replace, so it is named with
    // its identities here, in the serial scatter (the produce loop is parallel and its
    // log lines would interleave).  Limited to the first 20 so a pathological posture
    // cannot flood the log; the run-end bar carries the totals either way.
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
    networkLocalBlockTally(ctx, asm_res);
    networkPhaseColourTally(ctx, asm_res);
    networkCouplingTermTally(ctx, est, asm_res, registered_clock_sats, shared_phase_keys,
                             registered_code_keys, se, log);
    networkCouplingExactTally(ctx, asm_res);
    networkSdEwlTraceDrain(config_, traces, asm_res);
}

// ---- End of the PASS B serial stages ----

// ---- PASS B station steps ----

namespace {
using CK = ContributionKind;
}  // namespace

const StationBuildSteps kStationBuildSteps = {{
    {"data_rows", "filter/NetworkStationSplit",
     kindBit(CK::DataCode) | kindBit(CK::DataCodeCompanion) | kindBit(CK::DataPhase) |
     kindBit(CK::DataPhaseSecond),
     &networkStationDataRows},
    {"rx_ifb_recouple", "filter/NetworkRxCodeBiasCarry",
     0,
     &networkRxIfbRecoupleEdge},
    {"congruence_geometry", "filter/NetworkCongruence",
     0,
     &networkCongruenceRowGeometry},
    {"phase_datum_pins", "filter/NetworkPhaseDatum",
     kindBit(CK::AmbiguityPin) | kindBit(CK::Eq34RpbPin) | kindBit(CK::Eq34RpbRankFloor) |
     kindBit(CK::Eq34RpbTimeLink),
     &networkPhaseDatumPinRows},
    {"glonass_anchors", "filter/NetworkStationAnchors",
     kindBit(CK::GloIfbAnchor) | kindBit(CK::GloIonoAnchor),
     &networkGlonassAnchorRows},
    {"sim_zero_iono", "filter/NetworkStationAnchors",
     kindBit(CK::SimZeroIono),
     &networkSimZeroIonoRows},
    {"iono_constraint", "filter/NetworkIonoConstraintRows",
     kindBit(CK::ShIonoTie) | kindBit(CK::GimIono),
     &networkIonoConstraintRows},
    {"sd_ewl", "filter/NetworkSdEwlAr",
     kindBit(CK::SdEwl),
     &networkSdEwlStationRows},
    {"raim_columns", "filter/NetworkRaim",
     0,
     &networkRaimColumnCensus},
    {"carried_priors", "filter/NetworkStationCarry",
     kindBit(CK::PriorAmbiguityCarry) | kindBit(CK::CouplingScalar) |
     kindBit(CK::BlockAmbiguityCarry) | kindBit(CK::BlockZwdCarry) | kindBit(CK::BlockIonoCarry) |
     kindBit(CK::BlockRxClockCarry) | kindBit(CK::BlockRxCbCarry) |
     kindBit(CK::BlockPhaseColourCarry) | kindBit(CK::PriorAmbiguityOraclePin) |
     kindBit(CK::PriorZwdCarry) | kindBit(CK::PriorRxCbDatum) | kindBit(CK::PriorRxCbDatumPrePd) |
     kindBit(CK::PriorRxCbCarry),
     &networkStationCarriedPriors},
    {"phase_colour_rows", "filter/NetworkPhaseColourCarry",
     kindBit(CK::PhaseColourBirth) | kindBit(CK::PhaseColourBackstop),
     &networkPhaseColourRows},
    {"zwd_capture", "filter/NetworkZwdCarry",
     0,
     &networkZwdRowCapture},
}};

const std::array<StationCarryStep, kStationCarryStepCount> kStationCarrySteps = {{
    {"ambiguity", "filter/NetworkLevelingCarry",
     kindBit(CK::PriorAmbiguityCarry) | kindBit(CK::CouplingScalar),
     &networkAmbiguityCarryColumns},
    {"slant_iono_block", "filter/NetworkSlantIonoCarry",
     0,
     &networkSlantIonoBlockColumns},
    {"rx_clock_block", "filter/NetworkRxClockCarry",
     0,
     &networkRxClockBlockColumns},
    {"rx_cb_block", "filter/NetworkRxCodeBiasCarry",
     0,
     &networkRxCbBlockColumns},
    {"phase_colour_block", "filter/NetworkPhaseColourCarry",
     0,
     &networkPhaseColourBlockColumns},
    {"local_block", "filter/NetworkCoupling",
     kindBit(CK::BlockAmbiguityCarry) | kindBit(CK::BlockZwdCarry) | kindBit(CK::BlockIonoCarry) |
     kindBit(CK::BlockRxClockCarry) | kindBit(CK::BlockRxCbCarry) |
     kindBit(CK::BlockPhaseColourCarry) | kindBit(CK::CouplingScalar),
     &networkStationLocalBlock},
    {"oracle_pin", "filter/NetworkOraclePin",
     kindBit(CK::PriorAmbiguityOraclePin),
     &networkOraclePinPriors},
    {"zwd", "filter/NetworkZwdCarry",
     kindBit(CK::PriorZwdCarry) | kindBit(CK::CouplingScalar),
     &networkZwdCarryPrior},
    {"rx_cb", "filter/NetworkRxCodeBiasCarry",
     kindBit(CK::PriorRxCbDatum) | kindBit(CK::PriorRxCbDatumPrePd) | kindBit(CK::PriorRxCbCarry),
     &networkRxCbPriors},
    {"coupling_region", "filter/NetworkCoupling",
     kindBit(CK::CouplingScalar) | kindBit(CK::BlockAmbiguityCarry) | kindBit(CK::BlockZwdCarry) |
     kindBit(CK::BlockIonoCarry) | kindBit(CK::BlockRxClockCarry) | kindBit(CK::BlockRxCbCarry) |
     kindBit(CK::BlockPhaseColourCarry),
     &networkCouplingRegionRows},
    {"prior_finalize", "filter/NetworkStationCarry",
     0,
     &networkStationPriorFinalize},
}};

void networkGrowBlockRows(Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                          Eigen::VectorXd& y, Eigen::Index n) {
    const Eigen::Index m0 = A_s.rows();
    A_s.conservativeResize(m0 + n, Eigen::NoChange);
    A_s.bottomRows(n).setZero();
    A_r.conservativeResize(m0 + n, Eigen::NoChange);
    A_r.bottomRows(n).setZero();
    w.conservativeResize(m0 + n);
    y.conservativeResize(m0 + n);
}

void networkWriteWhitenedRows(const Eigen::MatrixXd& cov, const Eigen::MatrixXd& Hr,
                              const Eigen::MatrixXd* Hs, const Eigen::VectorXd& zc,
                              Eigen::Index out, Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                              Eigen::VectorXd& w, Eigen::VectorXd& y) {
    const Eigen::Index mc = cov.rows();
    const Eigen::LLT<Eigen::MatrixXd> llt(cov);
    A_r.middleRows(out, mc) = llt.matrixL().solve(Hr);
    if (Hs) A_s.middleRows(out, mc) = llt.matrixL().solve(*Hs);
    y.segment(out, mc)      = llt.matrixL().solve(zc);
    w.segment(out, mc).setOnes();
}

const Eigen::VectorXd& StationBuild::postUpdateState() {
    if (!post_state_ready) {
        // The set was built after the measurement update, from the state vector, and its
        // slots hold that vector's values bit for bit.
        post_state.resize(static_cast<Eigen::Index>(st.params.slots.size()));
        for (std::size_t k = 0; k < st.params.slots.size(); ++k)
            post_state(static_cast<Eigen::Index>(k)) = st.params.slots[k].x_lin;
        post_state_ready = true;
    }
    return post_state;
}

bool networkStationBuild(StationBuild& build, const StationBuildSteps& steps) {
    for (const StationBuildStep& s : steps)
        if (s.build(build) == StationStep::Done) return false;
    return true;
}

void networkStationAssemble(StationBuild& build, const StationBuildSteps& steps) {
    StationAssembly& R = build.R;
    try {
        if (!networkStationBuild(build, steps)) return;   // empty innovations or an H/state mismatch
        R.ok = true;
        return;
    } catch (const std::exception& ex) {
        R.threw = true; R.err = ex.what();
    } catch (...) {
        R.threw = true; R.err = "unknown exception";
    }
    // The station's births write its set as they go, so a step that threw leaves the set as
    // the births left it, and the readers of the next epoch's PASS A find those columns.
}

void networkAssemblyProduce(NetworkRun& run, const NetworkEpoch& ep,
                            const std::unique_ptr<GlobalIonosphereMap>& gim,
                            EpochAssembly& assembly) {
    // ---- From the run and the epoch (reads bind through `ep`; writes are marked `// written`) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    NetworkSetupResult& setup = run.setup;
    NetworkEstimator& est = run.engines.est;
    const SyncedEpoch& se = ep.se;
    const EpochClock& epoch_clock = ep.clock;
    const EpochPassA& pass_a = ep.pass_a;
    const EpochEvents& events = ep.events;
    const std::vector<bool>& present = pass_a.present;
    std::vector<StationAssembly>& asm_res = assembly.asm_res;  // written
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    // ---- End of the bindings ----
    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
    for (int i = 0; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        StationBuild build{ctx, config_, est, setup, se, epoch_clock, pass_a, events, gim, i,
                           stations[i], asm_res[i]};
        networkStationAssemble(build);
    }
}

// ---- End of the PASS B station steps ----

} // namespace GnssProcessor
